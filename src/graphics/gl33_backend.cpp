#include "gl33_backend.h"
#include "native_renderer.h"
#include "completed_presentation.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/cpu_work_profile.h"
#include "gs_shaders.h"
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#else
#error The offscreen context provider currently supports macOS; the renderer uses GL 3.3 core only.
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <variant>
#include <cstdlib>
#include <sstream>

namespace {
void checkGL(const char* stage) {
    const auto error=glGetError();
    if (error!=GL_NO_ERROR) throw std::runtime_error(std::string("OpenGL error at ")+stage+": "+std::to_string(error));
}
GLuint shader(GLenum type,const char* source) {
    GLuint id=glCreateShader(type);
    glShaderSource(id,1,&source,nullptr); glCompileShader(id);
    GLint status=0; glGetShaderiv(id,GL_COMPILE_STATUS,&status);
    if (!status) { char log[8192]{}; glGetShaderInfoLog(id,sizeof(log),nullptr,log); glDeleteShader(id); throw std::runtime_error(log); }
    return id;
}
// Every member occupies complete std140 vec4 slots. Driver reflection below
// verifies this host/shader ABI, including array strides, before drawing.
struct alignas(16) DrawParameters {
    std::array<float,12> position{},color{},stq{},uv{};
    std::array<int32_t,4> primitive{},flags{};
    std::array<uint32_t,4> tests{},format{};
    std::array<int32_t,4> textureInfo{},wrapping{},region{},sprite{},fogColor{};
    std::array<uint32_t,4> misc{};
    std::array<int32_t,4> bounds{};
};
static_assert(sizeof(DrawParameters)==368);
constexpr size_t batchLimit=32;
struct alignas(16) BatchParameters {
    std::array<int32_t,4> info{};
    std::array<DrawParameters,batchLimit> items{};
};
static_assert(sizeof(BatchParameters)==16+368*batchLimit);
static_assert(alignof(DrawParameters)==16);

void bindDrawParameters(GLuint program) {
    const GLuint block=glGetUniformBlockIndex(program,"DrawParameters");
    if(block==GL_INVALID_INDEX) throw std::runtime_error("Missing GL draw parameter block");
    GLint bytes=0;
    glGetActiveUniformBlockiv(program,block,GL_UNIFORM_BLOCK_DATA_SIZE,&bytes);
    if(bytes!=sizeof(BatchParameters)) throw std::runtime_error("GL std140 draw block size mismatch");
    const char* names[]={"items[0].position[0]","items[0].color[0]","items[0].stq[0]","items[0].uv[0]","items[0].primitive","items[0].flags","items[0].tests","items[0].format","items[0].textureInfo","items[0].wrapping","items[0].region","items[0].sprite","items[0].fogColor","items[0].misc","items[0].bounds","items[1].misc","batchInfo"};
    const GLint offsets[]={offsetof(DrawParameters,position),offsetof(DrawParameters,color),offsetof(DrawParameters,stq),offsetof(DrawParameters,uv),offsetof(DrawParameters,primitive),offsetof(DrawParameters,flags),offsetof(DrawParameters,tests),offsetof(DrawParameters,format),offsetof(DrawParameters,textureInfo),offsetof(DrawParameters,wrapping),offsetof(DrawParameters,region),offsetof(DrawParameters,sprite),offsetof(DrawParameters,fogColor),offsetof(DrawParameters,misc),offsetof(DrawParameters,bounds),sizeof(DrawParameters)+offsetof(DrawParameters,misc),-16};
    GLuint indices[17];GLint reflected[17];
    glGetUniformIndices(program,17,names,indices);
    for(auto index:indices) if(index==GL_INVALID_INDEX) throw std::runtime_error("Missing GL std140 draw member");
    glGetActiveUniformsiv(program,17,indices,GL_UNIFORM_OFFSET,reflected);
    for(unsigned i=0;i<17;++i) if(reflected[i]!=offsets[i]+16) throw std::runtime_error(std::string("GL std140 offset mismatch: ")+names[i]);
    glGetActiveUniformsiv(program,4,indices,GL_UNIFORM_ARRAY_STRIDE,reflected);
    for(unsigned i=0;i<4;++i) if(reflected[i]!=16) throw std::runtime_error("GL std140 array stride mismatch");
    glUniformBlockBinding(program,block,0);
}

using TargetKey=std::tuple<uint32_t,uint32_t,uint32_t,uint32_t,uint32_t>;
TargetKey targetKey(const GSContext& c) { return {c.frame.fbp,c.frame.fbw,c.frame.psm,c.zbuf.zbp,c.zbuf.psm}; }

// A page is the common 8 KiB unit of every GS layout. Block swizzles never
// leave it; a non-page-aligned texture base can carry into the following page.
// Conservatively include that carry instead of walking individual texels.
using VramPages=std::bitset<512>;
bool sourcePages(VramPages& pages,uint32_t psm,uint32_t base,uint32_t bw,
                 uint32_t x0,uint32_t y0,uint32_t x1,uint32_t y1) {
    uint32_t pageWidth=64,pageHeight=32,pagesPerRow=std::max(1u,bw);
    switch(psm) {
    case GS_PSM_CT32:case GS_PSM_CT24:case GS_PSM_Z32:case GS_PSM_Z24:
    case GS_PSM_T8H:case GS_PSM_T4HL:case GS_PSM_T4HH:break;
    case GS_PSM_CT16:case GS_PSM_CT16S:case GS_PSM_Z16:case GS_PSM_Z16S:pageHeight=64;break;
    case GS_PSM_T8:pageWidth=128;pageHeight=64;pagesPerRow=std::max(1u,bw>>1);break;
    case GS_PSM_T4:pageWidth=128;pageHeight=128;pagesPerRow=std::max(1u,bw>>1);break;
    default:return false;
    }
    for(uint32_t y=y0/pageHeight;y<=y1/pageHeight;++y)
        for(uint32_t x=x0/pageWidth;x<=x1/pageWidth;++x) {
            const auto page=(base>>5)+y*pagesPerRow+x;
            pages.set(page&511);
            if(base&31)pages.set((page+1)&511);
        }
    return true;
}
}

struct GL33Backend::Impl {
    std::shared_ptr<std::mutex> contextMutex=std::make_shared<std::mutex>();
    std::mutex& mutex=*contextMutex;
    bool failure=false;
    struct FailureGuard {
        Impl& owner;int exceptions=std::uncaught_exceptions();
        ~FailureGuard(){if(std::uncaught_exceptions()>exceptions)owner.failure=true;}
    };
    CGLContextObj context=nullptr;
    bool sharedPresentation=false;
    GLuint presentationProgram=0;
    std::array<GLint,8> presentationUniforms{};
    std::array<std::shared_ptr<CompletedPresentationImage>,3> presentationImages;
    size_t nextPresentationImage=0;
    struct Current {
        CGLContextObj old;
        explicit Current(CGLContextObj ctx):old(CGLGetCurrentContext()) {
            if (old!=ctx && CGLSetCurrentContext(ctx)!=kCGLNoError) throw std::runtime_error("Cannot bind GS core context");
        }
        ~Current() { if(CGLGetCurrentContext()!=old) CGLSetCurrentContext(old); }
    };
    GSCpuBackend memory; // GS memory layouts, CLUT lifetime and DMA, never Submit.
    bool initialized=false,active=false,dirty=false;
    GSContext surface{};
    uint32_t width=0,height=0;
    GLuint program=0,vao=0,textures[2]{},fbos[2]{},texture=0;
    // A short explicit-upload ring separates successive decoded images from
    // stores still sampled by queued draws. This is storage rotation only:
    // DecodeTexture must still run in order to preserve GS cache/CLUT history.
    std::array<GLuint,8> textureStores{};
    size_t nextTextureStore=0;
    const bool referenceTextureStore=[] {
        const char* value=std::getenv("NIGHT100_REFERENCE_TEXTURE_STORE");
        return !value || std::strcmp(value,"0")!=0;
    }();
    int current=0;
    std::array<int,4> previousDrawRect{};
    BatchParameters pending{};
    std::array<int,4> pendingRect{};
    GLuint parameterBuffer=0;
    size_t parameterStride=0,parameterCapacity=0,parameterOffset=0;
    std::vector<uint32_t> staging,before;
    std::vector<uint8_t> textureKey;
    std::vector<uint32_t> decodedTexture;
    uint32_t decodedTextureWidth=0,decodedTextureHeight=0;
    std::array<uint32_t,2> clutCbp{};
    VramPages transferDestination;
    bool knownTransferDestination=false;
    VramPages surfacePages,texturePages;
    bool knownSurfacePages=false,knownTexturePages=false;
    VramPages colorPages,depthPages,writtenPages,stalePages;
    uint32_t forceWritePlanes=0;
    uint64_t colorRevision=0,nextColorRevision=0;
    struct ResidentSurface {
        bool active=false,dirty=false,knownPages=false;
        GSContext surface{};
        uint32_t width=0,height=0;
        GLuint textures[2]{},fbos[2]{};
        int current=0;
        std::array<int,4> previousDrawRect{};
        std::vector<uint32_t> staging,before;
        VramPages pages,colorPages,depthPages,writtenPages,stalePages;
        uint32_t forceWritePlanes=0;
        uint64_t colorRevision=0;
    };
    // At most four integer targets are resident, including the active one.
    std::array<ResidentSurface,3> resident;
    size_t nextResident=0;
    struct RenderTextureSource {
        GSTex0Reg tex0{};GSTexaReg texa{};
        std::weak_ptr<const NativeAssetLifetime> lifetime;
        bool tracked=false;
        GLuint texture=0,fbo=0;
        uint32_t width=0,height=0;
        uint64_t revision=0;
    };
    std::unordered_map<uint64_t,RenderTextureSource> renderTextureSources;
    uint64_t nextRenderTextureSource=0;
    GLuint renderTextureProgram=0;
    std::array<GLint,3> renderTextureUniforms{};
    unsigned drawsUntilRenderTextureRetirement=0;
    void swapSurface(ResidentSurface& other) {
        std::swap(active,other.active);std::swap(dirty,other.dirty);std::swap(surface,other.surface);
        std::swap(width,other.width);std::swap(height,other.height);std::swap(current,other.current);
        std::swap(textures,other.textures);std::swap(fbos,other.fbos);
        std::swap(previousDrawRect,other.previousDrawRect);staging.swap(other.staging);before.swap(other.before);
        std::swap(surfacePages,other.pages);std::swap(knownSurfacePages,other.knownPages);
        std::swap(colorPages,other.colorPages);std::swap(depthPages,other.depthPages);std::swap(writtenPages,other.writtenPages);
        std::swap(stalePages,other.stalePages);
        std::swap(forceWritePlanes,other.forceWritePlanes);
        std::swap(colorRevision,other.colorRevision);
    }
    void discardSurfaces() {
        active=false;dirty=false;writtenPages.reset();forceWritePlanes=0;
        for(auto& cached:resident){cached.active=false;cached.dirty=false;cached.writtenPages.reset();cached.forceWritePlanes=0;}
        textureKey.clear();
    }
    struct SurfaceTransition {
        uint64_t frame=0;
        std::array<uint32_t,5> previous{},next{};
        uint32_t previousWidth=0,previousHeight=0,width=0,height=0,requested=0;
        bool hadPrevious=false,native=false;
    };
    std::unique_ptr<std::array<SurfaceTransition,4096>> surfaceProfile;
    std::string surfaceProfilePath;
    uint64_t surfaceProfileCount=0,presentCount=0;
    void tracePresentation(const GSPresentationRequest& r) const {
        static const bool enabled=std::getenv("NIGHT100_TRACE_PRESENTATION")!=nullptr;
        if(!enabled || (presentCount>4 && presentCount%120))return;
        std::cerr<<"[gpu-presentation] frame="<<presentCount<<std::hex<<" pmode="<<r.pmode
            <<" dispfb="<<r.dispfb1<<'/'<<r.dispfb2<<" display="<<r.display1<<'/'<<r.display2
            <<std::dec<<" preferred="<<r.hasPreferredSource<<':'<<r.preferredDestFbp<<'/'<<r.preferredSource.fbp
            <<'/'<<r.preferredSource.fbw<<'/'<<unsigned(r.preferredSource.psm);
        for(const auto& f:r.contextFrames)std::cerr<<" ctx="<<f.fbp<<'/'<<f.fbw<<'/'<<unsigned(f.psm);
        if(active)std::cerr<<" active="<<surface.frame.fbp<<'/'<<surface.frame.fbw<<'/'<<unsigned(surface.frame.psm)
            <<':'<<width<<'x'<<height<<":stale="<<(stalePages&colorPages).any();
        for(const auto& c:resident)if(c.active)std::cerr<<" resident="<<c.surface.frame.fbp<<'/'<<c.surface.frame.fbw<<'/'<<unsigned(c.surface.frame.psm)
            <<':'<<c.width<<'x'<<c.height<<":stale="<<(c.stalePages&c.colorPages).any();
        std::cerr<<'\n';
    }
    std::string deviceName;
    uint64_t draws=0;
    Statistics stats;
    std::array<GLuint,64> timerQueries{};
    size_t timerRead=0,timerWrite=0,timerPending=0;
    std::atomic<bool> timerActive{false};
    const bool forceGpuTiming=std::getenv("NIGHT100_GPU_TIMING")!=nullptr;
    void pollGpuTimers() {
        while(timerPending) {
            GLint ready=0;glGetQueryObjectiv(timerQueries[timerRead],GL_QUERY_RESULT_AVAILABLE,&ready);
            if(!ready)break;
            GLuint64 ns=0;glGetQueryObjectui64v(timerQueries[timerRead],GL_QUERY_RESULT,&ns);
            stats.gpuNanoseconds+=ns;++stats.gpuSamples;
            timerRead=(timerRead+1)%timerQueries.size();--timerPending;
        }
    }
    void beginGpuTimer() {
        if(!stats.gpuTimingEnabled || !stats.gpuTimingSupported || timerActive)return;
        pollGpuTimers();
        if(timerPending==timerQueries.size())return; // Never wait for telemetry.
        glBeginQuery(GL_TIME_ELAPSED,timerQueries[timerWrite]);timerActive=true;
    }
    void endGpuTimer() {
        if(!timerActive)return;
        glEndQuery(GL_TIME_ELAPSED);timerActive=false;
        timerWrite=(timerWrite+1)%timerQueries.size();++timerPending;
    }
    void finishGpuTimer() {
        if(timerActive) {
            endGpuTimer();
            // The end marker must reach the GPU before the CPU sleeps;
            // leaving it buffered would still include the host pacing gap.
            glFlush();
        }
        if(stats.gpuTimingEnabled)pollGpuTimers();
    }
    void setTimingEnabled(bool enabled) {
        enabled=enabled || forceGpuTiming;
        if(enabled==stats.gpuTimingEnabled)return;
        if(enabled && !stats.gpuTimingChecked) {
            GLint bits=0;glGetQueryiv(GL_TIME_ELAPSED,GL_QUERY_COUNTER_BITS,&bits);
            stats.gpuTimingChecked=true;stats.gpuTimingSupported=bits>0;
            if(!bits)std::cerr<<"[night100] GL elapsed timer unavailable; GPU timing is unknown\n";
        }
        stats.gpuTimingEnabled=enabled;
        if(enabled && stats.gpuTimingSupported)glGenQueries(timerQueries.size(),timerQueries.data());
        if(!enabled) {
            endGpuTimer();glDeleteQueries(timerQueries.size(),timerQueries.data());timerQueries.fill(0);
            timerRead=timerWrite=timerPending=0;
        }
    }
    std::unique_ptr<NativeRenderer> native;
    bool nativeActive=false;
    uint32_t nextNativeToken=0;
    struct ExecutedToken {uint64_t token=0;uint32_t issued=0,fbp=0,psm=0;};
    std::array<ExecutedToken,4096> executedTokens{};
    std::unordered_map<uint64_t,std::variant<NativeModelDraw,NativeScreenDraw>> nativeCommands;
    Impl() {
        if(const char* path=std::getenv("NIGHT100_PROFILE_SURFACES")) {
            surfaceProfilePath=path;surfaceProfile=std::make_unique<std::array<SurfaceTransition,4096>>();
        }
        CGLPixelFormatAttribute attributes[]={kCGLPFAOpenGLProfile,static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_3_2_Core),kCGLPFAAccelerated,static_cast<CGLPixelFormatAttribute>(0)};
        CGLPixelFormatObj pixelFormat=nullptr; GLint count=0;
        if (CGLChoosePixelFormat(attributes,&pixelFormat,&count)!=kCGLNoError || !pixelFormat) throw std::runtime_error("No accelerated core OpenGL pixel format");
        const auto windowContext=CGLGetCurrentContext();
        auto error=CGLCreateContext(pixelFormat,windowContext,&context);
        sharedPresentation=windowContext && error==kCGLNoError;
        if(error!=kCGLNoError && windowContext)error=CGLCreateContext(pixelFormat,nullptr,&context);
        CGLDestroyPixelFormat(pixelFormat);
        if (error!=kCGLNoError) throw std::runtime_error("Cannot create accelerated offscreen core context");
        try {
            Current bind(context);
            GLint major=0,minor=0,profile=0;
            glGetIntegerv(GL_MAJOR_VERSION,&major); glGetIntegerv(GL_MINOR_VERSION,&minor); glGetIntegerv(GL_CONTEXT_PROFILE_MASK,&profile);
            if (major<3 || (major==3 && minor<3) || !(profile&GL_CONTEXT_CORE_PROFILE_BIT)) throw std::runtime_error("GS requires OpenGL 3.3+ core");
            deviceName=std::string(reinterpret_cast<const char*>(glGetString(GL_RENDERER)))+" / "+reinterpret_cast<const char*>(glGetString(GL_VERSION));
            if(forceGpuTiming)setTimingEnabled(true);
            const auto vs=shader(GL_VERTEX_SHADER,gsVertexShader),fs=shader(GL_FRAGMENT_SHADER,gsFragmentShader);
            program=glCreateProgram(); glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);
            glDeleteShader(vs);glDeleteShader(fs);
            GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);
            if (!linked) { char log[8192]{};glGetProgramInfoLog(program,sizeof(log),nullptr,log);throw std::runtime_error(log); }
            glGenVertexArrays(1,&vao);glGenTextures(2,textures);glGenFramebuffers(2,fbos);
            glGenTextures(textureStores.size(),textureStores.data());texture=textureStores[0];
            glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,texture);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            const uint32_t zero=0;glTexImage2D(GL_TEXTURE_2D,0,GL_R32UI,1,1,0,GL_RED_INTEGER,GL_UNSIGNED_INT,&zero);
            glDisable(GL_BLEND);glDisable(GL_DEPTH_TEST);glDisable(GL_DITHER);glDisable(GL_MULTISAMPLE);glDisable(GL_FRAMEBUFFER_SRGB);
            glUseProgram(program);
            // Sampler units and UBO binding use core APIs, replacing shader
            // layout(binding=...) without requiring ARB_shading_language_420pack.
            glUniform1i(glGetUniformLocation(program,"previousSurface"),0);
            glUniform1i(glGetUniformLocation(program,"texturePixels"),1);
            bindDrawParameters(program);
            GLint alignment=0;glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT,&alignment);
            if(alignment<=0) throw std::runtime_error("Invalid GL uniform buffer alignment");
            parameterStride=((sizeof(BatchParameters)+alignment-1)/alignment)*alignment;
            parameterCapacity=parameterStride*128;
            glGenBuffers(1,&parameterBuffer);glBindBuffer(GL_UNIFORM_BUFFER,parameterBuffer);
            glBufferData(GL_UNIFORM_BUFFER,parameterCapacity,nullptr,GL_STREAM_DRAW);
            checkGL("initialize");
        } catch (...) { CGLDestroyContext(context);context=nullptr;throw; }
    }
    ~Impl() {
        std::lock_guard contextLock(mutex);
        if(surfaceProfile) {
            std::ofstream out(surfaceProfilePath);
            out<<"frame,had_previous,native,old_fbp,old_fbw,old_psm,old_zbp,old_zpsm,new_fbp,new_fbw,new_psm,new_zbp,new_zpsm,old_width,old_height,new_width,new_height,requested_height\n";
            for(uint64_t i=surfaceProfileCount>surfaceProfile->size()?surfaceProfileCount-surfaceProfile->size():0;i<surfaceProfileCount;++i) {
                const auto& event=(*surfaceProfile)[i%surfaceProfile->size()];
                out<<event.frame<<','<<event.hadPrevious<<','<<event.native;
                for(auto field:event.previous)out<<','<<field;
                for(auto field:event.next)out<<','<<field;
                out<<','<<event.previousWidth<<','<<event.previousHeight<<','<<event.width<<','<<event.height<<','<<event.requested<<'\n';
            }
        }
        if (context) {
            Current bind(context);
            native.reset();
            endGpuTimer();glDeleteQueries(timerQueries.size(),timerQueries.data());
            glDeleteBuffers(1,&parameterBuffer);glDeleteProgram(program);glDeleteVertexArrays(1,&vao);glDeleteTextures(2,textures);glDeleteTextures(textureStores.size(),textureStores.data());glDeleteFramebuffers(2,fbos);
            glDeleteProgram(presentationProgram);
            glDeleteProgram(renderTextureProgram);
            for(auto& [id,source]:renderTextureSources){glDeleteTextures(1,&source.texture);glDeleteFramebuffers(1,&source.fbo);}
            for(auto& cached:resident){glDeleteTextures(2,cached.textures);glDeleteFramebuffers(2,cached.fbos);}
        }
        if (context) CGLDestroyContext(context);
    }
    void uploadParameters(const BatchParameters& parameters) {
        glBindBuffer(GL_UNIFORM_BUFFER,parameterBuffer);
        if(parameterOffset+parameterStride>parameterCapacity) {
            // Orphan the old store on wrap; queued GPU draws retain it. This
            // is core 3.3 streaming, without persistent storage or fences.
            glBufferData(GL_UNIFORM_BUFFER,parameterCapacity,nullptr,GL_STREAM_DRAW);
            parameterOffset=0;
        }
        // Each range is used once until the whole store is orphaned on wrap.
        // Map only that unused range, so no in-flight draw needs a CPU wait.
        // Core GL mapping replaces the separate glBufferSubData upload.
        void* mapped=glMapBufferRange(GL_UNIFORM_BUFFER,parameterOffset,sizeof(parameters),
            GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_RANGE_BIT|GL_MAP_UNSYNCHRONIZED_BIT);
        if(!mapped) throw std::runtime_error("Cannot map GS parameter buffer");
        std::memcpy(mapped,&parameters,sizeof(parameters));
        if(!glUnmapBuffer(GL_UNIFORM_BUFFER)) throw std::runtime_error("GS parameter buffer contents became invalid");
        glBindBufferRange(GL_UNIFORM_BUFFER,0,parameterBuffer,parameterOffset,sizeof(parameters));
        parameterOffset+=parameterStride;
    }
    void submitPending() {
        if(!pending.info[0]) return;
        CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);
        Current bind(context);
        beginGpuTimer();
        glDisable(GL_BLEND);glDisable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);
        glUseProgram(program);glBindVertexArray(vao);
        uploadParameters(pending);
        const int next=1-current;
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER,fbos[current]);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,fbos[next]);
        const auto& r=previousDrawRect;
        if(r[2]>r[0] && r[3]>r[1]) {
            glBlitFramebuffer(r[0],r[1],r[2],r[3],r[0],r[1],r[2],r[3],GL_COLOR_BUFFER_BIT,GL_NEAREST);
            ++stats.surfaceCopies;
        }
        glBindFramebuffer(GL_FRAMEBUFFER,fbos[next]);
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,textures[current]);
        glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,texture);
        glViewport(0,0,width,height);glEnable(GL_SCISSOR_TEST);
        glScissor(pendingRect[0],pendingRect[1],pendingRect[2]-pendingRect[0],pendingRect[3]-pendingRect[1]);
        glDrawArrays(GL_TRIANGLES,0,3);
        current=next;previousDrawRect=pendingRect;dirty=true;++stats.submissions;
        pending.info[0]=0;
        checkGL("GS batch");
    }
    void flushCurrent() {
        resolveNative();
        submitPending();
        if (!active || !dirty) return;
        Current bind(context);
        endGpuTimer();
        staging.resize(size_t(width)*height*2);
        glBindFramebuffer(GL_READ_FRAMEBUFFER,fbos[current]);glPixelStorei(GL_PACK_ALIGNMENT,4);
        glReadPixels(0,0,width,height,GL_RG_INTEGER,GL_UNSIGNED_INT,staging.data());
        ++stats.surfaceReadbacks;
        if(stats.gpuTimingSupported)pollGpuTimers();
        checkGL("surface readback");
        const uint32_t planes=!knownSurfacePages?3u:
            (((writtenPages&colorPages).any()?1u:0u)|((writtenPages&depthPages).any()?2u:0u));
        memory.WriteSurface(surface,width-surfacePad,height,staging,before,planes,forceWritePlanes);
        // Other cached views of these physical pages now have an older CPU
        // seed. Keep unused stale planes marked until a draw needs them.
        for(auto& cached:resident)if(cached.active)cached.stalePages|=writtenPages&cached.pages;
        stalePages&=~writtenPages;
        before=staging;
        dirty=false;
        writtenPages.reset();
        forceWritePlanes=0;
        textureKey.clear(); // A rendered surface may alias a later texture.
    }
    void flushResident(ResidentSurface& cached) {
        resolveNative();submitPending();
        swapSurface(cached);flushCurrent();swapSurface(cached);
    }
    void flush() {
        flushCurrent();
        for(auto& cached:resident)if(cached.active && cached.dirty)flushResident(cached);
    }
    void resolveNative() {
        if(!nativeActive)return;
        Current bind(context);
        native->resolve(fbos[current],surface.frame.psm,surface.zbuf.psm);nativeActive=false;
        previousDrawRect={0,0,int(width),int(height)};dirty=true;
        checkGL("native resolve");
    }
    void invalidate() {flush();discardSurfaces();}
    bool readsPendingSurface(const VramPages& source,bool known) const {
        if(active && (dirty || nativeActive || pending.info[0]) &&
           (!known || !knownSurfacePages || (source&writtenPages).any()))return true;
        for(const auto& cached:resident)if(cached.active && cached.dirty &&
            (!known || !cached.knownPages || (source&cached.writtenPages).any()))return true;
        return false;
    }
    void syncPages(const VramPages& source,bool known,bool includeCurrent=true) {
        if(includeCurrent && active && (dirty || nativeActive || pending.info[0]) &&
           (!known || !knownSurfacePages || (source&writtenPages).any()))flushCurrent();
        for(auto& cached:resident)if(cached.active && cached.dirty &&
            (!known || !cached.knownPages || (source&cached.writtenPages).any()))flushResident(cached);
    }
    static bool transferPages(VramPages& pages,uint32_t psm,uint32_t base,uint32_t bw,
                              uint32_t x,uint32_t y,uint32_t w,uint32_t h) {
        pages.reset();
        if(!w || !h)return true;
        // All ordinary GS register extents fit here, including a rectangle
        // extending beyond the initial 11-bit transfer origin. Unusual callers
        // retain the conservative all-target synchronization boundary.
        if(w>4096 || h>4096 || x+w>8192 || y+h>8192)return false;
        bw=std::max(1u,bw);
        uint32_t lastY=y+h-1;
        // T8/T4 use floor(BW/2) page rows. BW=1 therefore has zero row
        // stride, including transfers starting below the first page row.
        if((psm==GS_PSM_T8 || psm==GS_PSM_T4) && bw==1)y=lastY=0;
        return sourcePages(pages,psm,base,bw,x,y,x+w-1,lastY);
    }
    void cpuWrittenPages(const VramPages& pages) {
        if(active){stalePages|=pages&surfacePages;if((pages&colorPages).any())colorRevision=++nextColorRevision;}
        for(auto& cached:resident)if(cached.active){cached.stalePages|=pages&cached.pages;if((pages&cached.colorPages).any())cached.colorRevision=++nextColorRevision;}
        textureKey.clear();
    }
    void loadSurface() {
        memory.ReadSurface(surface,width-surfacePad,height,staging);before=staging;stalePages.reset();
        colorRevision=++nextColorRevision;
        if(!textures[0])glGenTextures(2,textures);
        if(!fbos[0])glGenFramebuffers(2,fbos);
        current=0;previousDrawRect={};dirty=false;writtenPages.reset();forceWritePlanes=0;
        for(int i=0;i<2;++i) {
            glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,textures[i]);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RG32UI,width,height,0,GL_RG_INTEGER,GL_UNSIGNED_INT,staging.data());
            glBindFramebuffer(GL_FRAMEBUFFER,fbos[i]);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,textures[i],0);
            if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)throw std::runtime_error("Incomplete GS integer framebuffer");
        }
        active=true;
    }
    bool retireOverwrittenDepth(uint32_t w,uint32_t h) {
        // The upcoming primitive replaces every byte of this exact depth
        // plane. Keep older colors resident, but relinquish their depth writes
        // rather than materializing an obsolete plane in CPU VRAM.
        if(!knownSurfacePages || width!=w || height!=h || (colorPages&depthPages).any())return false;
        for(const auto& cached:resident)if(cached.active && cached.dirty && (cached.writtenPages&depthPages).any()) {
            if(!cached.knownPages || cached.depthPages!=depthPages || (cached.colorPages&depthPages).any() ||
               cached.surface.zbuf.zbp!=surface.zbuf.zbp || cached.surface.zbuf.psm!=surface.zbuf.psm ||
               cached.surface.frame.fbw!=surface.frame.fbw || cached.width!=w || cached.height!=h)return false;
        }
        for(auto& cached:resident)if(cached.active && (cached.depthPages&depthPages).any()) {
            cached.stalePages|=depthPages&cached.pages;
            cached.writtenPages&=~depthPages;cached.forceWritePlanes&=~2u;
            if(cached.writtenPages.none())cached.dirty=false;
        }
        writtenPages&=~depthPages;forceWritePlanes&=~2u;
        if(writtenPages.none())dirty=false;
        stalePages&=~depthPages;
        return true;
    }
    // Host columns beyond the guest's VRAM columns (widescreen).
    // Zero keeps host and VRAM widths identical; every VRAM read and write uses
    // the canonical columns only.
    static constexpr uint32_t surfacePad=0;
    void ensureSurface(const GSContext& ctx,uint32_t requiredHeight,bool nativeDraw=false,bool needsDepth=true,bool overwriteDepth=false) {
        auto w=std::max(1u,ctx.frame.fbw)*64+surfacePad;
        const uint32_t scissorHeight=uint32_t(ctx.scissor.y1)+1;
        // Ordinary screen/shadow targets are bounded by their small scissor.
        // Reserve that extent once: growing to each primitive's bottom edge
        // otherwise reads back, repacks and reallocates the entire target at
        // every increasing Y. Larger scissors retain the requested extent,
        // including the native bridge's existing 448-row viewport limit.
        auto h=std::max(1u,scissorHeight<=448?scissorHeight:std::min(requiredHeight,scissorHeight));
        if (w>2048 || h>2048) throw std::runtime_error("GS target extent exceeds supported surface: width="+std::to_string(w)+" height="+std::to_string(h)+" fbw="+std::to_string(ctx.frame.fbw));
        const bool same=active && targetKey(surface)==targetKey(ctx) && width==w && height>=h;
        if(same && !overwriteDepth) {
            const VramPages required=colorPages|(needsDepth?depthPages:VramPages{});
            const bool conflict=std::any_of(resident.begin(),resident.end(),[&](const auto& c){
                return c.active && c.dirty && (!knownSurfacePages || !c.knownPages || (required&c.writtenPages).any());
            });
            if(!conflict && !(required&stalePages).any())return;
        }
        if(surfaceProfile && !same) {
            auto fields=[](const GSContext& c){return std::array<uint32_t,5>{c.frame.fbp,c.frame.fbw,c.frame.psm,c.zbuf.zbp,c.zbuf.psm};};
            (*surfaceProfile)[surfaceProfileCount++%surfaceProfile->size()]={presentCount,fields(surface),fields(ctx),width,height,w,h,requiredHeight,active,nativeDraw};
        }
        Current bind(context);
        if(!same) {
            resolveNative();submitPending();
            // Unknown layouts and self-aliasing color/depth surfaces retain
            // the conservative lifetime; their planes cannot be independent.
            if(active && (!knownSurfacePages || (colorPages&depthPages).any())) {
                flushCurrent();active=false;
            }
            auto cached=std::find_if(resident.begin(),resident.end(),[&](const auto& c){
                return c.active && targetKey(c.surface)==targetKey(ctx) && c.width==w && c.height>=h;
            });
            if(cached==resident.end()) {
                cached=std::find_if(resident.begin(),resident.end(),[](const auto& c){return !c.active;});
                if(cached==resident.end())cached=resident.begin()+nextResident++%resident.size();
                if(cached->active && cached->dirty)flushResident(*cached);
                cached->active=false;
            }
            swapSurface(*cached);
            if(!active) {
                surface=ctx;width=w;height=h;
                colorPages.reset();depthPages.reset();
                knownSurfacePages=sourcePages(colorPages,ctx.frame.psm,ctx.frame.fbp<<5,ctx.frame.fbw,0,0,w-1,h-1);
                knownSurfacePages=sourcePages(depthPages,ctx.zbuf.psm,ctx.zbuf.zbp<<5,ctx.frame.fbw,0,0,w-1,h-1) && knownSurfacePages;
                surfacePages=colorPages|depthPages;
            }
        }
        if(overwriteDepth)submitPending();
        overwriteDepth=overwriteDepth && retireOverwrittenDepth(w,h);
        const VramPages required=colorPages|((needsDepth && !overwriteDepth)?depthPages:VramPages{});
        syncPages(required,knownSurfacePages,false);
        if(!active || (required&stalePages).any()){flushCurrent();loadSurface();}
        if(overwriteDepth){stalePages&=~depthPages;forceWritePlanes|=2u;}
    }
    void markWrites(bool color,bool depth) {
        if(color)colorRevision=++nextColorRevision;
        if(!knownSurfacePages && (color || depth)){writtenPages.set();return;}
        if(color)writtenPages|=colorPages;
        if(depth)writtenPages|=depthPages;
    }
    struct DisplayCircuit {
        GSFrameReg frame{};
        GLuint texture=0;
        uint32_t width=0,height=0,originX=0,originY=0,sourceFbp=0;
        bool preferred=false;
    };
    static DisplayCircuit displayCircuit(uint64_t dispfb,uint64_t display) {
        DisplayCircuit c;
        c.frame={uint32_t(dispfb&511),uint32_t((dispfb>>9)&63),uint8_t((dispfb>>15)&31),0};
        c.originX=(dispfb>>32)&2047;c.originY=(dispfb>>43)&2047;
        c.width=(((display>>32)&4095)+1)/(((display>>23)&15)+1);c.height=((display>>44)&2047)+1;
        if(c.width<64 || c.height<64){c.width=640;c.height=448;}
        c.width=std::min(c.width,640u);c.height=std::min(c.height,512u);c.sourceFbp=c.frame.fbp;
        return c;
    }
    static bool validCircuit(uint64_t display,const DisplayCircuit& c) {
        return c.frame.fbw || ((display>>32)&4095) || ((display>>44)&2047) || ((display>>23)&15);
    }
    static bool displayFormat(uint32_t psm){return psm==GS_PSM_CT32 || psm==GS_PSM_CT24 || psm==GS_PSM_CT16 || psm==GS_PSM_CT16S;}
    GLuint displayTexture(const GSFrameReg& source,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint64_t* revision=nullptr) const {
        if(!displayFormat(source.psm))return 0;
        auto find=[&](bool exists,const GSContext& ctx,uint32_t sw,uint32_t sh,GLuint image,
                      bool known,const VramPages& color,const VramPages& depth,const VramPages& stale,const VramPages* owner,uint64_t sourceRevision)->GLuint {
            if(!exists || !known || (color&depth).any() || (color&stale).any() ||
               ctx.frame.fbp!=source.fbp || ctx.frame.fbw!=(source.fbw?source.fbw:10) || ctx.frame.psm!=source.psm ||
               x+w>sw || y+h>sh)return 0;
            // A cached color plane cannot stand in for a newer dirty physical
            // alias held by another resident target.
            if(active && owner!=&writtenPages && dirty && (color&writtenPages).any())return 0;
            for(const auto& other:resident)if(other.active && owner!=&other.writtenPages && other.dirty && (color&other.writtenPages).any())return 0;
            if(revision)*revision=sourceRevision;return image;
        };
        if(auto image=find(active,surface,width,height,textures[current],knownSurfacePages,colorPages,depthPages,stalePages,&writtenPages,colorRevision))return image;
        for(const auto& cached:resident)if(auto image=find(cached.active,cached.surface,cached.width,cached.height,cached.textures[cached.current],
            cached.knownPages,cached.colorPages,cached.depthPages,cached.stalePages,&cached.writtenPages,cached.colorRevision))return image;
        return 0;
    }
    void retireRenderTextures() {
        if(drawsUntilRenderTextureRetirement){--drawsUntilRenderTextureRetirement;return;}
        drawsUntilRenderTextureRetirement=63;
        for(auto it=renderTextureSources.begin();it!=renderTextureSources.end();) {
            if(it->second.tracked && it->second.lifetime.expired()) {
                glDeleteTextures(1,&it->second.texture);glDeleteFramebuffers(1,&it->second.fbo);
                it=renderTextureSources.erase(it);
            } else ++it;
        }
    }
    void prepareRenderTextureProgram() {
        if(renderTextureProgram)return;
        const auto vertex=shader(GL_VERTEX_SHADER,gsVertexShader),fragment=shader(GL_FRAGMENT_SHADER,nativeRenderTextureShader);
        renderTextureProgram=glCreateProgram();glAttachShader(renderTextureProgram,vertex);glAttachShader(renderTextureProgram,fragment);
        glLinkProgram(renderTextureProgram);glDeleteShader(vertex);glDeleteShader(fragment);
        GLint linked=0;glGetProgramiv(renderTextureProgram,GL_LINK_STATUS,&linked);
        if(!linked)throw std::runtime_error("Cannot link native render texture import shader");
        const char* names[]={"sourceSurface","sourceFormat","textureAlpha"};
        for(unsigned i=0;i<renderTextureUniforms.size();++i)renderTextureUniforms[i]=glGetUniformLocation(renderTextureProgram,names[i]);
    }
    GLuint snapshotRenderTexture(const NativeTexture& data) {
        const auto found=renderTextureSources.find(data.renderTarget->sourceId);
        if(found==renderTextureSources.end())throw std::runtime_error("Unknown native render texture source");
        auto& source=found->second;
        if(!data.width || !data.height || data.width!=(1u<<source.tex0.tw) || data.height!=(1u<<source.tex0.th))
            throw std::runtime_error("Native render texture extent differs from registered source");
        // A normalized destination pass may continue while sampling a
        // disjoint parked camera target. Only a physical source dependency
        // requires resolving its in-flight native writes to raw GS storage.
        if(nativeActive) {
            VramPages pages;
            const bool known=sourcePages(pages,source.tex0.psm,source.tex0.tbp0,source.tex0.tbw,0,0,data.width-1,data.height-1);
            if(!known || !knownSurfacePages || (pages&writtenPages).any())resolveNative();
        }
        submitPending();
        const bool resized=source.width!=data.width || source.height!=data.height;
        if(!source.texture){glGenTextures(1,&source.texture);glGenFramebuffers(1,&source.fbo);}
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,source.texture);
        const auto wrap=[](uint32_t value){return value==2?GL_MIRRORED_REPEAT:value>=3?GL_CLAMP_TO_EDGE:GL_REPEAT;};
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,data.linear?GL_LINEAR:GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,data.linear?GL_LINEAR:GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,wrap(data.wrapU));glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,wrap(data.wrapV));
        if(resized) {
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,data.width,data.height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
            glBindFramebuffer(GL_FRAMEBUFFER,source.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,source.texture,0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)throw std::runtime_error("Incomplete native render texture snapshot");
            source.width=data.width;source.height=data.height;source.revision=0;
        }
        GSFrameReg frame{source.tex0.tbp0>>5,source.tex0.tbw,source.tex0.psm,0};
        uint64_t revision=0;
        if(const auto raw=displayTexture(frame,0,0,data.width,data.height,&revision)) {
            if(!resized && source.revision==revision)return source.texture;
            prepareRenderTextureProgram();
            glBindFramebuffer(GL_FRAMEBUFFER,source.fbo);glViewport(0,0,data.width,data.height);
            glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);glDisable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);glDisable(GL_CULL_FACE);
            glUseProgram(renderTextureProgram);glBindVertexArray(vao);
            glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,raw);
            glUniform1i(renderTextureUniforms[0],0);glUniform1i(renderTextureUniforms[1],source.tex0.psm);
            glUniform3ui(renderTextureUniforms[2],source.texa.ta0,source.texa.ta1,source.texa.aem);
            glDrawArrays(GL_TRIANGLES,0,3);source.revision=revision;++stats.nativeRenderTextureCopies;
        } else {
            // The queued native command is already committed. A target which
            // was evicted or changed through a physical alias must still yield
            // the correct image. This isolated adapter does not disturb the
            // legacy GS texture page cache's intentionally stale history.
            VramPages pages;
            const bool known=sourcePages(pages,source.tex0.psm,source.tex0.tbp0,source.tex0.tbw,0,0,data.width-1,data.height-1);
            syncPages(pages,known);
            std::vector<uint8_t> snapshot;memory.SnapshotVram(snapshot);
            GSCpuBackend decoder;decoder.Initialize(snapshot.data(),uint32_t(snapshot.size()));
            GSDrawState state{};state.context.tex0=source.tex0;state.texa=source.texa;
            state.textureWidth=data.width;state.textureHeight=data.height;
            std::vector<uint32_t> rgba;decoder.DecodeTexture(state,data.width,data.height,rgba);
            for(auto& pixel:rgba)pixel=(pixel&0xffffffu)|(std::min(255u,(pixel>>24)*2u)<<24);
            glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,source.texture);
            glTexSubImage2D(GL_TEXTURE_2D,0,0,0,data.width,data.height,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
            source.revision=0;++stats.nativeRenderTextureFallbacks;
        }
        return source.texture;
    }
    bool selectCircuit(DisplayCircuit& c,const GSPresentationRequest& r,bool allowPreferred,bool authoritativeDisplay) const {
        if(allowPreferred && r.hasPreferredSource && r.preferredDestFbp==c.frame.fbp &&
           (r.preferredSource.fbw || r.preferredSource.fbp!=c.frame.fbp) && displayFormat(r.preferredSource.psm)) {
            // Preferred sources are GS blocks, rather than framebuffer pages.
            // Arbitrary block offsets keep the existing CPU layout path.
            if(r.preferredSource.fbp&31)return false;
            auto preferred=r.preferredSource;preferred.fbp>>=5;
            c.texture=displayTexture(preferred,0,0,c.width,c.height);
            if(!c.texture)return false;
            c.frame=preferred;c.originX=c.originY=0;c.preferred=true;c.sourceFbp=r.preferredSource.fbp;
            return true;
        }
        // CPU bring-up can substitute a nonblack context for a black FBP0.
        // Only the exact game's completed native display explicitly declares
        // its framebuffer authoritative, including genuine black fade frames.
        if(!authoritativeDisplay && c.frame.fbp==0)for(const auto& candidate:r.contextFrames)
            if(displayFormat(candidate.psm) && (candidate.fbp!=c.frame.fbp || (candidate.fbw?candidate.fbw:10)!=(c.frame.fbw?c.frame.fbw:10) || candidate.psm!=c.frame.psm))return false;
        c.texture=displayTexture(c.frame,c.originX,c.originY,c.width,c.height);
        return c.texture!=0;
    }
    void preparePresentationProgram() {
        if(presentationProgram)return;
        const auto vertex=shader(GL_VERTEX_SHADER,gsVertexShader),fragment=shader(GL_FRAGMENT_SHADER,completedPresentShader);
        presentationProgram=glCreateProgram();glAttachShader(presentationProgram,vertex);glAttachShader(presentationProgram,fragment);
        glLinkProgram(presentationProgram);glDeleteShader(vertex);glDeleteShader(fragment);
        GLint linked=0;glGetProgramiv(presentationProgram,GL_LINK_STATUS,&linked);
        if(!linked)throw std::runtime_error("Cannot link completed presentation shader");
        const char* names[]={"circuit1","circuit2","source1","source2","formats","pmode","background","outputSize"};
        for(unsigned i=0;i<presentationUniforms.size();++i)presentationUniforms[i]=glGetUniformLocation(presentationProgram,names[i]);
    }
    std::optional<PresentationFrame> completedPresentation(const GSPresentationRequest& r,bool authoritativeDisplay) {
        if(!sharedPresentation || std::getenv("NIGHT100_DISABLE_GPU_PRESENTATION"))return std::nullopt;
        auto first=displayCircuit(r.dispfb1,r.display1),second=displayCircuit(r.dispfb2,r.display2);
        const bool valid1=(r.pmode&1) && validCircuit(r.display1,first),valid2=(r.pmode&2) && validCircuit(r.display2,second);
        if(!valid1 && !valid2)return PresentationFrame{};
        const bool dual=valid1 && valid2;
        const uint32_t displayFbp=valid1?first.frame.fbp:second.frame.fbp;
        if(dual) {
            if(!selectCircuit(first,r,false,authoritativeDisplay) || !selectCircuit(second,r,false,authoritativeDisplay))return std::nullopt;
        } else {
            if(!valid1)first=second;
            if(!selectCircuit(first,r,true,authoritativeDisplay))return std::nullopt;
            second=first;
        }
        size_t slot=presentationImages.size();
        for(size_t n=0;n<presentationImages.size();++n) {
            const size_t index=(nextPresentationImage+n)%presentationImages.size();
            if(!presentationImages[index] || presentationImages[index].use_count()==1){slot=index;break;}
        }
        if(slot==presentationImages.size()){++stats.completedRetainedFrames;PresentationFrame frame;frame.keepPrevious=true;return frame;}
        nextPresentationImage=(slot+1)%presentationImages.size();
        Current bind(context);preparePresentationProgram();
        if(!presentationImages[slot])presentationImages[slot]=std::make_shared<CompletedPresentationImage>(context,contextMutex);
        auto& image=presentationImages[slot];image->beginWrite();
        glBindFramebuffer(GL_FRAMEBUFFER,image->framebuffer());glViewport(0,0,640,512);
        glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);glDisable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);glDisable(GL_CULL_FACE);
        glUseProgram(presentationProgram);glBindVertexArray(vao);
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,first.texture);glUniform1i(presentationUniforms[0],0);
        glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,second.texture);glUniform1i(presentationUniforms[1],1);
        glUniform4i(presentationUniforms[2],first.width,first.height,first.originX,first.originY);
        glUniform4i(presentationUniforms[3],second.width,second.height,second.originX,second.originY);
        glUniform4i(presentationUniforms[4],first.frame.psm,second.frame.psm,dual,0);
        glUniform4i(presentationUniforms[5],(r.pmode>>5)&1,(r.pmode>>6)&1,(r.pmode>>7)&1,(r.pmode>>8)&255);
        glUniform3ui(presentationUniforms[6],r.bgcolor&255,(r.bgcolor>>8)&255,(r.bgcolor>>16)&255);
        const uint32_t outputWidth=dual?std::max(first.width,second.width):first.width,outputHeight=dual?std::max(first.height,second.height):first.height;
        glUniform2i(presentationUniforms[7],outputWidth,outputHeight);glDrawArrays(GL_TRIANGLES,0,3);
        image->endWrite();checkGL("completed presentation");
        PresentationFrame frame;frame.gpu=image;frame.width=outputWidth;frame.height=outputHeight;
        frame.displayFbp=displayFbp;frame.sourceFbp=first.sourceFbp;frame.usedPreferred=first.preferred;
        ++stats.completedGpuFrames;return frame;
    }
    void prepareTexture(const GSDrawState& state) {
        if (!state.prim.tme) return;
        // Decode formats/CLUT on CPU, sample/filter in the fragment shader.
        // Keys contain named fields, never padding in guest-independent structs.
        const auto& t=state.context.tex0;
        std::array<uint64_t,8> key={t.tbp0,uint64_t(t.tbw)|(uint64_t(t.psm)<<8)|(uint64_t(t.cbp)<<16),uint64_t(t.cpsm)|(uint64_t(t.csm)<<8)|(uint64_t(t.csa)<<16),uint64_t(state.texa.ta0)|(uint64_t(state.texa.ta1)<<8)|(uint64_t(state.texa.aem)<<16),state.context.clamp,uint64_t(state.textureWidth)|(uint64_t(state.textureHeight)<<16),uint64_t(state.texclut.cbw)|(uint64_t(state.texclut.cou)<<8)|(uint64_t(state.texclut.cov)<<16),0};
        const auto* bytes=reinterpret_cast<const uint8_t*>(key.data());
        if(textureKey.size()==sizeof(key) && !std::memcmp(textureKey.data(),bytes,sizeof(key)) &&
           !readsPendingSurface(texturePages,knownTexturePages))return;
        uint32_t tw=std::max(1u,uint32_t(state.textureWidth)),th=std::max(1u,uint32_t(state.textureHeight));
        const auto clamp=state.context.clamp;
        if ((clamp&3)>=2) tw=1024;
        if (((clamp>>2)&3)>=2) th=1024;
        VramPages source;
        const bool known=sourcePages(source,t.psm,t.tbp0,t.tbw,0,0,tw-1,th-1);
        const bool alias=readsPendingSurface(source,known);
        // Only a physical VRAM overlap needs GPU-to-CPU synchronization. Old
        // GS batches must still consume their decoded texture before replacing
        // its GL store, even when the next source is unrelated to the target.
        if(alias)syncPages(source,known);else submitPending();
        Current bind(context);
        memory.DecodeTexture(state,tw,th,decodedTexture);decodedTextureWidth=tw;decodedTextureHeight=th;
        // submitPending above consumed the previous object before rotation.
        // glTexImage2D replaces storage on wrap with normal core GL ordering;
        // never use unsynchronized overwrites or assume a unified memory GPU.
        texture=textureStores[referenceTextureStore?0:nextTextureStore];
        if(!referenceTextureStore)nextTextureStore=(nextTextureStore+1)%textureStores.size();
        glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,texture);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D,0,GL_R32UI,tw,th,0,GL_RED_INTEGER,GL_UNSIGNED_INT,decodedTexture.data());
        ++stats.textureUploads;
        textureKey.assign(bytes,bytes+sizeof(key));
        texturePages=source;knownTexturePages=known;
    }
    void draw(const GSPrimitiveBatch& batch, int clearWord=-1, bool clearing=false) {
        const auto& s=batch.state;const auto& c=s.context;
        if (!initialized || !batch.vertexCount) return;
        resolveNative();
        if (!clearing && s.prim.type>=GS_PRIM_TRIANGLE && s.prim.type<=GS_PRIM_TRIFAN) {
            const auto& a=batch.vertices[0];const auto& b=batch.vertices[1];const auto& d=batch.vertices[2];
            const float denominator=(b.y-d.y)*(a.x-d.x)+(d.x-b.x)*(a.y-d.y);
            if (std::abs(denominator)<.001f) return; // Same degenerate rejection as reference.
            if (std::max({a.x,b.x,d.x})<float(c.scissor.x0+(c.xyoffset.ofx>>4)) ||
                std::min({a.x,b.x,d.x})>float(c.scissor.x1+(c.xyoffset.ofx>>4)) ||
                std::max({a.y,b.y,d.y})<float(c.scissor.y0+(c.xyoffset.ofy>>4)) ||
                std::min({a.y,b.y,d.y})>float(c.scissor.y1+(c.xyoffset.ofy>>4))) return;
        }
        float maxY=batch.vertices[0].y-float(c.xyoffset.ofy>>4);
        for(unsigned i=1;i<batch.vertexCount;++i) maxY=std::max(maxY,batch.vertices[i].y-float(c.xyoffset.ofy>>4));
        const auto needed=uint32_t(std::max(1,int(std::ceil(maxY))+((s.prim.type==GS_PRIM_SPRITE || clearing)?0:1)));
        const bool writesDepth=!clearing && !c.zbuf.zmask && ((c.test>>17)&3)!=0;
        const bool needsDepth=writesDepth || (!clearing && ((c.test>>17)&3)>=2);
        static const bool referenceDepthClear=[] {
            const char* value=std::getenv("NIGHT100_REFERENCE_DEPTH_CLEAR");return value && std::strcmp(value,"1")==0;
        }();
        bool overwriteDepth=false;
        if(!referenceDepthClear && !clearing && s.prim.type==GS_PRIM_SPRITE && !s.prim.tme && !s.prim.abe && writesDepth &&
           ((c.test>>17)&3)==1 && !(c.test&(1u<<14)) && (!(c.test&1) || ((c.test>>1)&7)==1) &&
           (c.zbuf.psm==GS_PSM_Z32 || c.zbuf.psm==GS_PSM_Z16 || c.zbuf.psm==GS_PSM_Z16S)) {
            const uint32_t w=std::max(1u,c.frame.fbw)*64,scissorHeight=uint32_t(c.scissor.y1)+1;
            const uint32_t h=std::max(1u,scissorHeight<=448?scissorHeight:std::min(needed,scissorHeight));
            const float ax=batch.vertices[0].x-float(c.xyoffset.ofx>>4),ay=batch.vertices[0].y-float(c.xyoffset.ofy>>4);
            const float bx=batch.vertices[1].x-float(c.xyoffset.ofx>>4),by=batch.vertices[1].y-float(c.xyoffset.ofy>>4);
            const uint32_t pageHeight=c.zbuf.psm==GS_PSM_Z32?32:64;
            overwriteDepth=c.scissor.x0==0 && c.scissor.y0==0 && c.scissor.x1>=w-1 &&
                std::min(ax,bx)<=0 && std::min(ay,by)<=0 && std::max(ax,bx)>=int(w) && std::max(ay,by)>=int(h) &&
                h%pageHeight==0;
        }
        static const bool traceCoherence=std::getenv("NIGHT100_TRACE_SURFACE_COHERENCE")!=nullptr;
        if(traceCoherence) {
            static unsigned count=0;
            if(count<48) {
                VramPages colors,depths;
                const auto w=std::max(1u,c.frame.fbw)*64,scissorHeight=uint32_t(c.scissor.y1)+1;
                const auto h=std::max(1u,scissorHeight<=448?scissorHeight:std::min(needed,scissorHeight));
                const bool known=sourcePages(colors,c.frame.psm,c.frame.fbp<<5,c.frame.fbw,0,0,w-1,h-1) &&
                    sourcePages(depths,c.zbuf.psm,c.zbuf.zbp<<5,c.frame.fbw,0,0,w-1,h-1);
                const auto required=colors|(needsDepth?depths:VramPages{});
                const auto log=[&](const auto& cached,bool cachedKnown,bool cachedDirty) {
                    if(!cached.active || !cachedDirty || (known && cachedKnown && !(required&cached.writtenPages).any()))return;
                    if(targetKey(cached.surface)==targetKey(c))return;
                    ++count;
                    std::cerr<<"[surface-coherence] frame="<<presentCount<<" type="<<unsigned(s.prim.type)<<" clearing="<<clearing
                        <<" tex="<<s.prim.tme<<" blend="<<s.prim.abe<<" test="<<c.test<<" mask="<<c.frame.fbmsk
                        <<" target="<<c.frame.fbp<<'/'<<c.frame.fbw<<'/'<<unsigned(c.frame.psm)<<" z="<<c.zbuf.zbp<<'/'<<unsigned(c.zbuf.psm)<<'/'<<c.zbuf.zmask
                        <<" extent="<<w<<'x'<<h<<" scissor="<<c.scissor.x0<<','<<c.scissor.y0<<','<<c.scissor.x1<<','<<c.scissor.y1
                        <<" xyoffset="<<(c.xyoffset.ofx>>4)<<','<<(c.xyoffset.ofy>>4)<<" vertices=";
                    for(unsigned i=0;i<batch.vertexCount;++i){const auto& v=batch.vertices[i];std::cerr<<v.x<<','<<v.y<<','<<v.z<<','<<unsigned(v.a)<<';';}
                    std::cerr<<" cached="<<cached.surface.frame.fbp<<'/'<<cached.surface.frame.fbw<<'/'<<unsigned(cached.surface.frame.psm)
                        <<":"<<cached.width<<'x'<<cached.height<<" overlaps-color="<<(colors&cached.writtenPages).count()<<" overlaps-depth="<<(depths&cached.writtenPages).count()
                        <<" written-color="<<(cached.colorPages&cached.writtenPages).count()<<" written-depth="<<(cached.depthPages&cached.writtenPages).count()<<'\n';
                };
                log(*this,knownSurfacePages,dirty || nativeActive || pending.info[0]);
                for(const auto& cached:resident)log(cached,cached.knownPages,cached.dirty);
            }
        }
        ensureSurface(c,needed,false,needsDepth,overwriteDepth);prepareTexture(s);
        DrawParameters parameters{};
        auto* p=parameters.position.data();
        auto* colors=parameters.color.data();
        auto* stq=parameters.stq.data();
        auto* uv=parameters.uv.data();
        uint32_t base=UINT32_MAX;
        for (unsigned i=0;i<batch.vertexCount;++i) base=std::min(base,static_cast<uint32_t>(batch.vertices[i].z));
        if (s.prim.type==GS_PRIM_SPRITE) base=static_cast<uint32_t>(batch.vertices[1].z);
        for (int i=0;i<3;++i) {
            const auto& v=batch.vertices[i];
            p[i*4]=v.x-float(c.xyoffset.ofx>>4);p[i*4+1]=v.y-float(c.xyoffset.ofy>>4);
            if(s.prim.type<=GS_PRIM_LINESTRIP) {p[i*4]=float(int(v.x)-int(c.xyoffset.ofx>>4));p[i*4+1]=float(int(v.y)-int(c.xyoffset.ofy>>4));}
            p[i*4+2]=float(v.z-double(base));p[i*4+3]=v.fog;
            colors[i*4]=v.r;colors[i*4+1]=v.g;colors[i*4+2]=v.b;colors[i*4+3]=v.a;
            stq[i*4]=v.s;stq[i*4+1]=v.t;stq[i*4+2]=v.q;
            uv[i*4]=v.u;uv[i*4+1]=v.v;
        }
        int x0=0,y0=0,x1=0,y1=0;
        int type=clearing ? 7 : int(s.prim.type);
        if (type==6 || clearing) {
            const auto& a=batch.vertices[0];const auto& b=batch.vertices[1];
            int ax=int(a.x)-int(c.xyoffset.ofx>>4),ay=int(a.y)-int(c.xyoffset.ofy>>4);
            int bx=int(b.x)-int(c.xyoffset.ofx>>4),by=int(b.y)-int(c.xyoffset.ofy>>4);
            x0=std::min(ax,bx);y0=std::min(ay,by);
            int spanX=std::max(1,std::abs(bx-ax)),spanY=std::max(1,std::abs(by-ay));
            x1=x0+spanX-1;y1=y0+spanY-1;
            parameters.sprite={x0,y0,spanX,spanY};
        } else if (type==0 || type==1 || type==2) {
            x0=x1=int(batch.vertices[0].x)-int(c.xyoffset.ofx>>4);
            y0=y1=int(batch.vertices[0].y)-int(c.xyoffset.ofy>>4);
            if(type==1 || type==2) {
                const int bx=int(batch.vertices[1].x)-int(c.xyoffset.ofx>>4),by=int(batch.vertices[1].y)-int(c.xyoffset.ofy>>4);
                x0=std::min(x0,bx);x1=std::max(x1,bx);y0=std::min(y0,by);y1=std::max(y1,by);
            }
        } else {
            x0=int(std::floor(std::min({p[0],p[4],p[8]})));x1=int(std::ceil(std::max({p[0],p[4],p[8]})));
            y0=int(std::floor(std::min({p[1],p[5],p[9]})));y1=int(std::ceil(std::max({p[1],p[5],p[9]})));
        }
        x0=std::max(x0,int(c.scissor.x0));y0=std::max(y0,int(c.scissor.y0));
        x1=std::min(x1,int(c.scissor.x1));y1=std::min(y1,int(c.scissor.y1));
        if (x0>x1 || y0>y1) return;
        markWrites(c.frame.fbmsk!=0xffffffffu,writesDepth);
        parameters.primitive={type,s.prim.iip,s.prim.tme,s.prim.fst};
        parameters.flags={s.prim.fge,s.prim.abe,s.pabe,s.linearFilter};
        parameters.tests={uint32_t(c.test),uint32_t(c.alpha),uint32_t(c.alpha>>32)&255,uint32_t(c.fba)};
        parameters.format={c.frame.psm,c.zbuf.psm,c.zbuf.zmask,c.frame.fbmsk};
        parameters.textureInfo={s.textureWidth,s.textureHeight,c.tex0.tfx,c.tex0.tcc};
        parameters.wrapping={int32_t(c.clamp&3),int32_t((c.clamp>>2)&3),0,0};
        parameters.region={int32_t((c.clamp>>4)&1023),int32_t((c.clamp>>14)&1023),int32_t((c.clamp>>24)&1023),int32_t((c.clamp>>34)&1023)};
        parameters.fogColor={s.fogR,s.fogG,s.fogB,0};
        parameters.misc[0]=clearing ? uint32_t(clearWord) : base;
        parameters.bounds={x0,y0,x1+1,y1+1};
        if(!pending.info[0]) pendingRect=parameters.bounds;
        else {
            auto& r=pendingRect;
            r={std::min(r[0],x0),std::min(r[1],y0),std::max(r[2],x1+1),std::max(r[3],y1+1)};
        }
        pending.items[pending.info[0]++]=parameters;++draws;
        if(pending.info[0]==batchLimit) submitPending();
    }
};

GL33Backend::GL33Backend():impl(std::make_unique<Impl>()) {}
GL33Backend::~GL33Backend()=default;
void GL33Backend::Initialize(uint8_t* v,uint32_t size) {std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};impl->pending.info[0]=0;impl->nativeActive=false;impl->discardSurfaces();impl->memory.Initialize(v,size);impl->initialized=v!=nullptr;impl->clutCbp.fill(0);}
void GL33Backend::Reset() {std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};impl->invalidate();impl->memory.Reset();impl->clutCbp.fill(0);}

void GL33Backend::prepareStateSave() {
    std::lock_guard lock(impl->mutex);
    if(!impl->nativeCommands.empty())throw ps2x::state::NotReady("Snapshot requires consumed ordered native commands");
    Impl::FailureGuard guard{*impl};impl->flush();
}
void GL33Backend::saveState(ps2x::state::Writer& archive) {
    std::lock_guard lock(impl->mutex);
    if(!impl->nativeCommands.empty())throw ps2x::state::NotReady("Snapshot requires consumed ordered native commands");
    Impl::FailureGuard guard{*impl};impl->flush();impl->memory.saveState(archive);archive(impl->clutCbp);
    // These page bits describe an unfinished host upload. They contain only
    // physical page numbers, never GL handles or process addresses.
    archive.pod(impl->transferDestination);archive(impl->knownTransferDestination);
    archive(impl->textureKey);
    if(!impl->textureKey.empty()) {
        archive.pod(impl->texturePages);archive(impl->knownTexturePages,impl->decodedTextureWidth,impl->decodedTextureHeight);
        archive.bytes(impl->decodedTexture.data(),impl->decodedTexture.size()*sizeof(uint32_t));
    }
}
void GL33Backend::loadState(ps2x::state::Reader& archive) {
    std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};Impl::Current bind(impl->context);
    // Canonical memory has already been restored. Flushing the old timeline's
    // dirty surfaces here would overwrite that memory, so discard them first.
    impl->pending.info[0]=0;impl->nativeActive=false;impl->discardSurfaces();impl->nativeCommands.clear();
    impl->endGpuTimer();glDeleteQueries(impl->timerQueries.size(),impl->timerQueries.data());impl->timerQueries.fill(0);
    impl->timerRead=impl->timerWrite=impl->timerPending=0;
    if(impl->stats.gpuTimingEnabled && impl->stats.gpuTimingSupported)glGenQueries(impl->timerQueries.size(),impl->timerQueries.data());
    impl->native.reset();
    for(auto& [id,source]:impl->renderTextureSources){glDeleteTextures(1,&source.texture);glDeleteFramebuffers(1,&source.fbo);}
    impl->renderTextureSources.clear();
    impl->memory.loadState(archive);archive(impl->clutCbp);archive.pod(impl->transferDestination);archive(impl->knownTransferDestination);
    archive(impl->textureKey);
    if(!impl->textureKey.empty()) {
        archive.pod(impl->texturePages);archive(impl->knownTexturePages,impl->decodedTextureWidth,impl->decodedTextureHeight);
        if(impl->textureKey.size()!=8*sizeof(uint64_t) || !impl->decodedTextureWidth || !impl->decodedTextureHeight ||
           impl->decodedTextureWidth>1024 || impl->decodedTextureHeight>1024)throw std::runtime_error("Invalid snapshot decoded texture cache");
        impl->decodedTexture.resize(size_t(impl->decodedTextureWidth)*impl->decodedTextureHeight);
        archive.bytes(impl->decodedTexture.data(),impl->decodedTexture.size()*sizeof(uint32_t));
        glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,impl->texture);
        glTexImage2D(GL_TEXTURE_2D,0,GL_R32UI,impl->decodedTextureWidth,impl->decodedTextureHeight,0,GL_RED_INTEGER,GL_UNSIGNED_INT,impl->decodedTexture.data());
    }
    impl->failure=false;checkGL("snapshot restore");
}
void GL33Backend::Submit(const GSPrimitiveBatch& b) {std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};impl->draw(b);}

void GL33Backend::LoadClut(const GSTex0Reg& t,const GSTexClutReg& c) {
    std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};
    // TEX0 writes also arrive for direct textures and non-loading CLD modes.
    // These leave the GS palette unchanged, so retain queued draws and their
    // decoded texture instead of reading back an unrelated render target.
    const bool indexed=t.psm==GS_PSM_T8 || t.psm==GS_PSM_T4 || t.psm==GS_PSM_T8H || t.psm==GS_PSM_T4HL || t.psm==GS_PSM_T4HH;
    if(!indexed || t.cld==0 || t.cld>=6)return;
    if(!impl->initialized)return;
    if(t.cld==4 && impl->clutCbp[0]==t.cbp)return;
    if(t.cld==5 && impl->clutCbp[1]==t.cbp)return;
    if(t.cld==2 || t.cld==4)impl->clutCbp[0]=t.cbp;
    if(t.cld==3 || t.cld==5)impl->clutCbp[1]=t.cbp;
    VramPages source;
    const bool fourBit=t.psm==GS_PSM_T4 || t.psm==GS_PSM_T4HL || t.psm==GS_PSM_T4HH;
    const uint32_t entries=fourBit?16:256;
    const bool supported=t.cpsm==GS_PSM_CT32 || t.cpsm==GS_PSM_CT24 || t.cpsm==GS_PSM_CT16 || t.cpsm==GS_PSM_CT16S;
    const bool known=supported && (t.csm==0?
        sourcePages(source,t.cpsm,t.cbp,1,0,0,15,(entries-1)/16):
        sourcePages(source,t.cpsm,t.cbp,c.cbw,uint32_t(c.cou)<<4,c.cov,(uint32_t(c.cou)<<4)+entries-1,c.cov));
    if(impl->readsPendingSurface(source,known))impl->syncPages(source,known);else impl->submitPending();
    impl->memory.LoadClut(t,c);impl->textureKey.clear();
}
void GL33Backend::BeginTransfer(const GSTransferCommand& c) {
    std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};
    auto& g=*impl;const auto& b=c.bitbltbuf;const auto& p=c.trxpos;
    const uint32_t w=c.trxreg.rrw,h=c.trxreg.rrh;
    g.transferDestination.reset();g.knownTransferDestination=true;
    if(c.direction==0 || c.direction==2)
        g.knownTransferDestination=Impl::transferPages(g.transferDestination,b.dpsm,b.dbp,b.dbw,p.dsax,p.dsay,w,h);
    VramPages source;bool knownSource=true;
    if(c.direction==1 || c.direction==2)
        knownSource=Impl::transferPages(source,b.spsm,b.sbp,b.sbw,p.ssax,p.ssay,w,h);
    if(c.direction>3 || !knownSource || !g.knownTransferDestination) {
        g.invalidate();g.memory.BeginTransfer(c);return;
    }
    // Host uploads do not mutate VRAM until their IMAGE payload arrives.
    // Local copies execute immediately and need both source and masked
    // destination bytes current before the original CPU transfer runs.
    if(c.direction==1 || c.direction==2)
        g.syncPages(source|(c.direction==2?g.transferDestination:VramPages{}),true);
    g.memory.BeginTransfer(c);
    if(c.direction==2)g.cpuWrittenPages(g.transferDestination);
}
void GL33Backend::UploadImage(const uint8_t* p,uint32_t n) {
    std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};auto& g=*impl;
    const auto transfer=g.memory.GetTransferSnapshot();
    if(!p || !n || transfer.direction!=0 || !transfer.totalPixels){g.memory.UploadImage(p,n);return;}
    if(!g.knownTransferDestination){g.invalidate();g.memory.UploadImage(p,n);return;}
    // Commit dirty aliases before partial CT24/high-index/nibble writes so
    // the CPU transfer preserves the other bytes, then retire only stale views.
    g.syncPages(g.transferDestination,true);
    g.memory.UploadImage(p,n);g.cpuWrittenPages(g.transferDestination);
}
void GL33Backend::Flush() {std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};impl->flush();}
void GL33Backend::TextureFlush() {std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};impl->resolveNative();impl->submitPending();impl->memory.TextureFlush();impl->textureKey.clear();}
void GL33Backend::Sync(GSSyncReason r) {std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};impl->flush();impl->memory.Sync(r);}
PresentationFrame GL33Backend::Present(const GSPresentationRequest& r) {std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};++impl->presentCount;impl->tracePresentation(r);impl->flush();return impl->memory.Present(r);}
PresentationFrame GL33Backend::PresentCompleted(const GSPresentationRequest& r,bool authoritativeDisplay) {
    CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);
    std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};
    // Timer queries belong to the producer context, even after the completed
    // image helper returns. The game thread need not have any current context.
    Impl::Current bind(impl->context);
    // A native SDK fence may have closed the rendering interval before its
    // host wait. Include the remaining resolve/composition as another bounded
    // interval without spanning that wait or forcing GPU completion.
    impl->beginGpuTimer();
    ++impl->presentCount;impl->tracePresentation(r);impl->resolveNative();impl->submitPending();
    if(auto frame=impl->completedPresentation(r,authoritativeDisplay)) {
        // Seal this frame's queries before game pacing can sleep. Polling is
        // availability-only, so telemetry never forces GPU completion.
        impl->finishGpuTimer();
        return std::move(*frame);
    }
    ++impl->stats.completedCpuFallbacks;impl->flush();impl->finishGpuTimer();return impl->memory.Present(r);
}
bool GL33Backend::ClearFramebuffer(const GSContext& c,uint32_t rgba) {
    if (!c.frame.fbw) return false;
    uint32_t value=rgba;
    if ((c.fba&1) && c.frame.psm!=GS_PSM_CT24) value|=0x80000000;
    if (c.frame.psm==GS_PSM_CT16 || c.frame.psm==GS_PSM_CT16S)
        value=((value>>3)&31)|(((value>>11)&31)<<5)|(((value>>19)&31)<<10)|(((value>>24)>=64)?0x8000:0);
    else if (c.frame.psm==GS_PSM_CT24) value&=0xffffff;
    else if (c.frame.psm!=GS_PSM_CT32) return false;
    GSPrimitiveBatch b{};b.vertexCount=2;b.state.context=c;
    b.vertices[0].x=c.scissor.x0+float(c.xyoffset.ofx>>4);b.vertices[0].y=c.scissor.y0+float(c.xyoffset.ofy>>4);
    b.vertices[1].x=c.scissor.x1+1.f+float(c.xyoffset.ofx>>4);b.vertices[1].y=c.scissor.y1+1.f+float(c.xyoffset.ofy>>4);
    std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};impl->draw(b,int(value),true);return true;
}
uint32_t GL33Backend::ConsumeLocalToHostBytes(uint8_t* p,uint32_t n) {std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};impl->flush();return impl->memory.ConsumeLocalToHostBytes(p,n);}
uint32_t GL33Backend::ReadVram(uint32_t p,uint32_t b,uint32_t w,uint32_t x,uint32_t y) const {std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};impl->flush();return impl->memory.ReadVram(p,b,w,x,y);}
void GL33Backend::WriteVram(uint32_t p,uint32_t b,uint32_t w,uint32_t x,uint32_t y,uint32_t v) {std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};impl->invalidate();impl->memory.WriteVram(p,b,w,x,y,v);}
void GL33Backend::SnapshotVram(std::vector<uint8_t>& out) const {std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};impl->flush();impl->memory.SnapshotVram(out);}
GSTransferSnapshot GL33Backend::GetTransferSnapshot() const {std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};return impl->memory.GetTransferSnapshot();}
std::string GL33Backend::device() const {return impl->deviceName;}
uint64_t GL33Backend::gpuDraws() const {std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};return impl->draws;}
void GL33Backend::setTimingEnabled(bool enabled) {
    std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};Impl::Current bind(impl->context);impl->setTimingEnabled(enabled);
}
void GL33Backend::finishTimingInterval() {
    // Hidden telemetry has no context switch or renderer lock on this path.
    if(!impl->timerActive.load(std::memory_order_relaxed))return;
    std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};Impl::Current bind(impl->context);
    impl->finishGpuTimer();
}
GL33Backend::Statistics GL33Backend::statistics() const {std::lock_guard lock(impl->mutex);auto result=impl->stats;result.primitives=impl->draws;result.nativeGeometryResources=impl->native?impl->native->geometryResourceCount():0;result.nativeTextureResources=impl->native?impl->native->textureResourceCount():0;result.nativeRenderTextureResources=impl->renderTextureSources.size();return result;}
uint64_t GL33Backend::RegisterNativeRenderTexture(const GSTex0Reg& tex0,const GSTexaReg& texa,const std::shared_ptr<const NativeAssetLifetime>& lifetime) {
    if(!Impl::displayFormat(tex0.psm) || !tex0.tbw || (tex0.tbp0&31) || tex0.tw>10 || tex0.th>10 || tex0.tcc!=1 || tex0.tfx!=0)return 0;
    std::lock_guard lock(impl->mutex);
    const uint64_t id=++impl->nextRenderTextureSource;
    auto& source=impl->renderTextureSources[id];source.tex0=tex0;source.texa=texa;source.lifetime=lifetime;source.tracked=bool(lifetime);
    return id;
}
bool GL33Backend::DrawNativeModel(const GSContext& context,const NativeModelDraw& draw) {
    if((context.frame.psm!=GS_PSM_CT32 && context.frame.psm!=GS_PSM_CT16 && context.frame.psm!=GS_PSM_CT16S) || !context.frame.fbw)return false;
    std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};Impl::Current bind(impl->context);
    impl->retireRenderTextures();
    std::vector<unsigned> renderTextures;
    if(std::any_of(draw.materials.begin(),draw.materials.end(),[](const auto& material){return material.texture && material.texture->renderTarget.has_value();})) {
        // Snapshot before selecting the destination; source dependencies
        // resolve in snapshotRenderTexture, while disjoint passes continue.
        renderTextures.resize(draw.materials.size());
        for(size_t i=0;i<draw.materials.size();++i)if(const auto& texture=draw.materials[i].texture;texture && texture->renderTarget)
            renderTextures[i]=impl->snapshotRenderTexture(*texture);
    }
    impl->ensureSurface(context,std::min(448u,uint32_t(context.scissor.y1)+1),true,draw.depthTest || draw.depthWrite);
    impl->submitPending();
    impl->beginGpuTimer();
    if(!impl->native)impl->native=std::make_unique<NativeRenderer>();
    if(!impl->nativeActive){impl->native->begin(impl->width,impl->height,impl->textures[impl->current],context.frame.psm);impl->nativeActive=true;}
    impl->native->draw(draw,renderTextures);impl->markWrites(true,draw.depthWrite);impl->stats.nativeMeshes+=draw.geometry->meshes.size();
    checkGL("native model");return true;
}

bool GL33Backend::hasFailed() const {std::lock_guard lock(impl->mutex);return impl->failure;}
bool GL33Backend::DrawNativeScreen(const GSContext& context,const NativeScreenDraw& draw) {
    if((context.frame.psm!=GS_PSM_CT32 && context.frame.psm!=GS_PSM_CT16 && context.frame.psm!=GS_PSM_CT16S) || !context.frame.fbw)return false;
    std::lock_guard lock(impl->mutex);Impl::FailureGuard guard{*impl};Impl::Current bind(impl->context);
    impl->retireRenderTextures();
    unsigned renderTexture=0;
    if(draw.texture && draw.texture->renderTarget)renderTexture=impl->snapshotRenderTexture(*draw.texture);
    impl->ensureSurface(context,std::min(448u,uint32_t(context.scissor.y1)+1),true,draw.depthTest || draw.depthWrite);
    impl->submitPending();
    impl->beginGpuTimer();
    if(!impl->native)impl->native=std::make_unique<NativeRenderer>();
    if(!impl->nativeActive){impl->native->begin(impl->width,impl->height,impl->textures[impl->current],context.frame.psm);impl->nativeActive=true;}
    impl->native->draw(draw,renderTexture);impl->markWrites(true,draw.depthWrite);++impl->stats.nativeMeshes;
    checkGL("native screen");return true;
}
uint64_t GL33Backend::QueueNativeModel(NativeModelDraw draw) {
    std::lock_guard lock(impl->mutex);
    const uint64_t token=0x4e31303000000000ull|++impl->nextNativeToken;
    impl->nativeCommands.emplace(token,std::move(draw));return token;
}
uint64_t GL33Backend::QueueNativeScreen(NativeScreenDraw draw) {
    std::lock_guard lock(impl->mutex);
    const uint64_t token=0x4e31303000000000ull|++impl->nextNativeToken;
    impl->nativeCommands.emplace(token,std::move(draw));return token;
}
void GL33Backend::NativeCommand(uint64_t token,const GSContext& context) {
    CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);
    try {
    std::variant<NativeModelDraw,NativeScreenDraw> command;
    {
        std::lock_guard lock(impl->mutex);
        auto it=impl->nativeCommands.find(token);
        if(it==impl->nativeCommands.end()) {
            impl->failure=true;
            const auto& previous=impl->executedTokens[uint32_t(token)%impl->executedTokens.size()];
            std::ostringstream message;
            message<<"Missing ordered native graphics command token=0x"<<std::hex<<token
                <<" latest=0x"<<impl->nextNativeToken<<" replay="<<(previous.token==token)
                <<" previous-latest=0x"<<previous.issued<<" previous-target="<<previous.fbp<<'/'<<previous.psm
                <<" current-target="<<context.frame.fbp<<'/'<<unsigned(context.frame.psm);
            throw std::runtime_error(message.str());
        }
        impl->executedTokens[uint32_t(token)%impl->executedTokens.size()]={token,impl->nextNativeToken,context.frame.fbp,context.frame.psm};
        command=std::move(it->second);impl->nativeCommands.erase(it);
    }
    const bool rendered=std::visit([&](const auto& draw) {
        if constexpr(std::is_same_v<std::decay_t<decltype(draw)>,NativeModelDraw>)return DrawNativeModel(context,draw);
        else return DrawNativeScreen(context,draw);
    },command);
    if(!rendered)throw std::runtime_error("Unsupported target for ordered native graphics command");
    } catch(...) {
        std::lock_guard lock(impl->mutex);impl->failure=true;throw;
    }
}
