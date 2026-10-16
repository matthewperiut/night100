#include "widescreen.h"
#include "native_scene.h"
#include "native_particle_policy.h"
#include "controller_prompts.h"
#include "frame_events.h"
#include "graphics/native_scene_backend.h"
#include "graphics/native_asset_cache.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace {
bool profileNativeStalls() {
    static const bool enabled=std::getenv("NIGHT100_PROFILE_NATIVE_STALLS")!=nullptr;
    return enabled;
}
struct NativeCostStamp {
    std::chrono::steady_clock::time_point wall;
    double cpu=0;
    static NativeCostStamp now() {
#ifdef __SWITCH__
        uint64_t ticks=0;
        if(R_FAILED(svcGetInfo(&ticks,InfoType_ThreadTickCount,CUR_THREAD_HANDLE,TickCountInfo_Total)))
            throw std::runtime_error("Thread CPU timing is unavailable");
        return {std::chrono::steady_clock::now(),double(ticks)/19200000.0};
#else
        timespec t{};clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);
        return {std::chrono::steady_clock::now(),t.tv_sec+t.tv_nsec*1e-9};
#endif
    }
    std::array<double,2> milliseconds() const {
        const auto end=now();
        return {std::chrono::duration<double,std::milli>(end.wall-wall).count(),(end.cpu-cpu)*1000};
    }
};
NativeSceneBackend* backend=nullptr;
PS2Runtime::RecompiledFunction resourceDestroyOriginal=nullptr,resourceWaitOriginal=nullptr,resourcePluginDestroyOriginal=nullptr;
// Diagnostic: NIGHT100_VU1_CALLER removes the per-reason print caps below.
bool traceAllNativeScene() {
    static const bool enabled=std::getenv("NIGHT100_VU1_CALLER")!=nullptr;
    return enabled;
}
bool debugWord(uint8_t* ram,uint32_t address,uint32_t& value) {
    if(address>PS2_RAM_SIZE-4)return false;
    std::memcpy(&value,ram+address,4);return true;
}
void traceResourcePluginDestroy(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc==0x249390) {
        static unsigned count=0;
        if(count++<128) {
            const uint32_t object=GPR_U32(cpu,4),offset=GPR_U32(cpu,5);uint32_t resource=0,head=0;
            debugWord(ram,object+offset,resource);debugWord(ram,resource,head);
            std::cerr<<"[native-resource:plugin-destroy] object=0x"<<std::hex<<object<<" offset=0x"<<offset
                <<" field=0x"<<(object+offset)<<" resource=0x"<<resource<<" header=0x"<<head
                <<" ra=0x"<<GPR_U32(cpu,31)<<std::dec<<'\n';
        }
    }
    resourcePluginDestroyOriginal(ram,cpu,runtime);
}
void traceResourceDestroy(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    static unsigned entryLogs=0,loopLogs=0;
    if(cpu->pc==0x239508 && entryLogs++<128) {
        const uint32_t header=GPR_U32(cpu,4);uint32_t count=0;const bool valid=debugWord(ram,header,count);
        std::cerr<<"[native-resource:destroy] header=0x"<<std::hex<<header<<" count=0x"<<count<<" valid="<<valid
            <<" ra=0x"<<GPR_U32(cpu,31)<<" sp=0x"<<GPR_U32(cpu,29)<<" entries=";
        for(unsigned i=0;i<std::min(count,8u);++i){uint32_t value=0;debugWord(ram,header+4+i*4,value);std::cerr<<" 0x"<<value;}
        std::cerr<<std::dec<<'\n';
    }
    if(cpu->pc==0x239548 && loopLogs++<32) {
        uint32_t resource=0;const auto element=GPR_U32(cpu,18);const bool valid=debugWord(ram,element,resource);
        std::cerr<<"[native-resource:destroy-loop] header=0x"<<std::hex<<GPR_U32(cpu,19)<<" index=0x"<<GPR_U32(cpu,17)
            <<" element=0x"<<element<<" resource=0x"<<resource<<" valid="<<valid<<std::dec<<'\n';
    }
    resourceDestroyOriginal(ram,cpu,runtime);
}
void traceResourceWait(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc==0x241b08) {
        static unsigned count=0;
        if(count++<128) {
            const uint32_t resource=GPR_U32(cpu,4);uint32_t pending=0;const bool valid=debugWord(ram,resource+0x18,pending);
            std::cerr<<"[native-resource:wait] resource=0x"<<std::hex<<resource<<" pending=0x"<<pending
                <<" valid="<<valid<<" ra=0x"<<GPR_U32(cpu,31)<<std::dec<<'\n';
        }
    }
    resourceWaitOriginal(ram,cpu,runtime);
}
PS2Runtime::RecompiledFunction original=nullptr;
uint64_t nativeCalls=0,fallbackCalls=0;
// xModelRender2D (0x122540) draws 3D models at screen positions (HUD icons, menu
// models). Atomic draws issued while it is active are tagged so a wide backend
// keeps them in the 4:3 area. A preempted call resumes through the pass-through
// path below, so the depth can only be wrong for the remainder of that one frame.
PS2Runtime::RecompiledFunction hud2DOriginal=nullptr;
int hud2DDepth=0;
// Widescreen view window. The game's main-camera FOV setter (iCameraSetFOV, 0x138b40)
// reaches RwCameraSetViewWindow (0x1e6240, SDK: camera in $a0, RwV2d* window in $a1).
// While 16:9 is on and the gameplay gate is active, the FOV setter's window is widened
// by 854/640 before the SDK copies it, so the view matrix the native draws capture and
// the frustum planes the game culls with both cover the wide view. Other cameras
// (shadow, previews) are never touched because only calls made inside the FOV setter
// are widened. xModelRender2D reads the window to place icons on screen, so it is
// given the original window for the duration of its call.
PS2Runtime::RecompiledFunction viewWindowOriginal=nullptr,cameraFovOriginal=nullptr;
constexpr float wideWindowScale=854.f/640.f;
int cameraFovDepth=0;
uint32_t widenedCamera=0;   // camera whose window was last widened by the hook (0 = none)
float baseWindowX=0.f,widenedWindowX=0.f;
void setViewWindow(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc!=0x1e6240u){viewWindowOriginal(ram,cpu,runtime);return;}
    const uint32_t camera=GPR_U32(cpu,4),window=GPR_U32(cpu,5);
    const bool widen=cameraFovDepth>0 && Widescreen::isEnabled() && Widescreen::worldCameraActive(ram) && window+8<=PS2_RAM_SIZE;
    float x=0.f;
    if(widen){
        std::memcpy(&x,ram+window,4);const float wide=x*wideWindowScale;std::memcpy(ram+window,&wide,4);
        baseWindowX=x;widenedWindowX=wide;widenedCamera=camera;
    } else if(cameraFovDepth>0 && camera==widenedCamera)widenedCamera=0;
    viewWindowOriginal(ram,cpu,runtime);
    if(widen)std::memcpy(ram+window,&x,4);
}
void setCameraFov(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc!=0x138b40u){cameraFovOriginal(ram,cpu,runtime);return;}
    struct Depth{Depth(){++cameraFovDepth;}~Depth(){--cameraFovDepth;}} depth;
    cameraFovOriginal(ram,cpu,runtime);
}
// True when the camera's clip transform was built from the widened window.
bool cameraWidened(uint32_t camera){return camera && camera==widenedCamera;}
void renderHud2D(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc!=0x122540u){hud2DOriginal(ram,cpu,runtime);return;}
    struct Depth{Depth(){++hud2DDepth;}~Depth(){--hud2DDepth;}} depth;
    // xModelRender2D places the model from camera+0x68 (view window x). Give it the
    // original 4:3 window while the camera is widened; the clip transform stays wide
    // and the backend narrows the HUD draw back to the 4:3 projection.
    uint32_t camera=0;std::memcpy(&camera,ram+0x2b5ad0,4);
    float current=0.f;bool swapped=false;
    if(cameraWidened(camera) && camera+0x70<=PS2_RAM_SIZE) {
        std::memcpy(&current,ram+camera+0x68,4);
        if(current==widenedWindowX){std::memcpy(ram+camera+0x68,&baseWindowX,4);swapped=true;}
    }
    hud2DOriginal(ram,cpu,runtime);
    if(swapped){
        // The original call may have changed the window only through SetViewWindow
        // (not expected); restore only if our base value is still in place.
        float now=0.f;std::memcpy(&now,ram+camera+0x68,4);
        if(now==baseWindowX)std::memcpy(ram+camera+0x68,&widenedWindowX,4);
    }
}
PS2Runtime::RecompiledFunction im2dOriginal=nullptr;
PS2Runtime::RecompiledFunction im2dIndexedOriginal=nullptr;
PS2Runtime::RecompiledFunction showOriginal=nullptr;
PS2Runtime::RecompiledFunction movieOriginal=nullptr;
constexpr uint32_t nativeFrameReturnPC=0x121ec8;
struct PendingFrame {uint32_t returnPC,result;};
std::vector<PendingFrame> pendingFrames;
void presentMovie(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc==0x270460)runtime->setGameControlsPresentation(false);
    movieOriginal(ram,cpu,runtime);
}
void finishCompletedFrame(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(pendingFrames.empty())throw std::runtime_error("Unmatched native frame continuation");
    runtime->memory().processPendingTransfers();
    const uint32_t gp=GPR_U32(cpu,28);
    auto sdkWord=[&](uint32_t offset) {
        const uint32_t address=gp-offset;
        if(address>PS2_RAM_SIZE-4)throw std::runtime_error("Invalid native frame queue address");
        uint32_t value=0;std::memcpy(&value,ram+address,4);return value;
    };
    // The SDK advances its read pointer after starting a DMA, so an empty
    // queue alone is insufficient: its DMA and vertical-blank fences must
    // also finish. Original interrupt handlers own all of these fields.
    const bool waiting=sdkWord(0x7c74)!=0;
    if(waiting) {
        if(backend)backend->finishTimingInterval();
        runtime->eeScheduler().waitVSync(runtime->eeScheduler().currentVSyncTick());
    }
    if(sdkWord(0x7c10)!=sdkWord(0x7c0c) || sdkWord(0x6ea4) || sdkWord(0x7c70)) {
        SET_GPR_U32(cpu,31,nativeFrameReturnPC);
        runtime->dispatchGuestBranch(ram,cpu,0x1f4118,nativeFrameReturnPC,nativeFrameReturnPC,
            PS2Runtime::GuestBranchKind::DirectCall,"native frame queue pump");
        return;
    }
    const auto pending=pendingFrames.back();pendingFrames.pop_back();
    SET_GPR_U32(cpu,2,pending.result);SET_GPR_U32(cpu,31,pending.returnPC);cpu->pc=pending.returnPC;
    runtime->setGameControlsPresentation(true);
    // This exact-ELF continuation has drained the real camera's render queue.
    // Its display target is authoritative, including intentionally black fades;
    // the bring-up fallback to a nonblack alternate context is inappropriate.
    runtime->gs().latchHostPresentationFrame(true);
    GameFrameEvents::completed(ram,runtime);
    if(const char* directory=std::getenv("NIGHT100_CAPTURE_COMPLETED")) {
        static unsigned count=0;
        if(++count>=300 && count<324) {
            std::vector<uint8_t> rgba;uint32_t width=0,height=0;
            if(runtime->gs().copyLatchedHostPresentationFrame(rgba,width,height)) {
                std::filesystem::create_directories(directory);
                std::ofstream out(std::filesystem::path(directory)/(std::to_string(count)+".ppm"),std::ios::binary);
                out<<"P6\n"<<width<<' '<<height<<"\n255\n";
                for(size_t i=0;i<rgba.size();i+=4)out.write(reinterpret_cast<const char*>(rgba.data()+i),3);
            }
        }
    }
}
void presentCompletedFrame(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    showOriginal(ram,cpu,runtime);
    if(cpu->pc==0x1e6180) {
        // RwRasterShowRaster queues a flip; it does not finish the SDK's work.
        // Allowing its four-frame queue to fill rendered in bursts, then
        // latched the same old image for the next three camera calls. Drain
        // with the real SDK queue pump before exposing this frame to the host.
        // This is a guest call so DMA, interrupts and scheduler yields retain
        // their ordinary continuations, including the original vsync fence.
        pendingFrames.push_back({cpu->pc,GPR_U32(cpu,2)});
        SET_GPR_U32(cpu,31,nativeFrameReturnPC);cpu->pc=0x1f4118;
    }
}
PS2Runtime::RecompiledFunction queueWaitOriginal=nullptr;
void waitGraphicsQueue(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    // The SDK marks a queued vertical-blank fence at gp-0x7c74. Its original
    // polling loop resumes here after a scheduler checkpoint. Sleep on that
    // same event; the original interrupt handler still completes the queue.
    if(cpu->pc==0x1f4dc8) {
        const uint32_t address=GPR_U32(cpu,28)-0x7c74;
        uint32_t waiting=0;if(address<=PS2_RAM_SIZE-4)std::memcpy(&waiting,ram+address,4);
        if(waiting) {
            if(backend)backend->finishTimingInterval();
            runtime->eeScheduler().waitVSync(runtime->eeScheduler().currentVSyncTick());
        }
    }
    queueWaitOriginal(ram,cpu,runtime);
}
void traceIm2D(uint8_t*,R5900Context*,PS2Runtime*);
// Unused alignment NOP after iModelRender, inside the generated dispatch table.
// No guest instruction bytes are modified; only our private return uses it.
constexpr uint32_t nativeReturnPC=0x121ecc;
struct PendingMarker {uint32_t returnPC,result;uint64_t token;};
std::vector<PendingMarker> markers;
void finishNativeMarker(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(markers.empty())throw std::runtime_error("Unmatched native packet continuation");
    if(!GPR_U32(cpu,2))throw std::runtime_error("Native graphics packet allocation failed");
    const auto marker=markers.back();markers.pop_back();
    const uint32_t global=GPR_U32(cpu,28)-0x7c1c;
    if(global>PS2_RAM_SIZE-4)throw std::runtime_error("Invalid native packet cursor address");
    const uint32_t packet=runtime->Load32(ram,cpu,global);
    runtime->Store64(ram,cpu,packet,0x1000000000008001ull);
    runtime->Store64(ram,cpu,packet+8,0xe);
    runtime->Store64(ram,cpu,packet+16,marker.token);
    runtime->Store64(ram,cpu,packet+24,0x7f);
    const uint32_t next=packet+32;std::memcpy(ram+global,&next,4);
    SET_GPR_U32(cpu,2,marker.result);SET_GPR_U32(cpu,31,marker.returnPC);cpu->pc=marker.returnPC;
}
void insertNativeMarker(R5900Context* cpu,uint64_t token,uint32_t result) {
    markers.push_back({GPR_U32(cpu,31),result,token});
    // Reserve a two-qword VIF DIRECT packet using the game's allocator. This
    // is a real guest call/continuation: allocator yields and IRQs remain valid.
    SET_GPR_U32(cpu,4,1);SET_GPR_U32(cpu,5,uint32_t(-2));SET_GPR_U32(cpu,31,nativeReturnPC);cpu->pc=0x1f2498;
}
struct Guest {
    uint8_t* ram;
    void valid(uint32_t address,size_t bytes)const {if(address<0x1000 || bytes>PS2_RAM_SIZE || address>PS2_RAM_SIZE-bytes)throw std::runtime_error("Native scene guest range out of bounds");}
    template<class T>T read(uint32_t address)const{valid(address,sizeof(T));T result;std::memcpy(&result,ram+address,sizeof(T));return result;}
    uint32_t u(uint32_t address)const{return read<uint32_t>(address);}
    float f(uint32_t address)const{return read<float>(address);}
    NativeMatrix matrix(uint32_t address)const {
        valid(address,64);NativeMatrix result;std::memcpy(result.data(),ram+address,64);
        result[3]=result[7]=result[11]=0;result[15]=1;return result;
    }
};
NativeAssetCache assetCache;
auto& geometryCache=assetCache.geometries;
auto& textureCache=assetCache.textures;
std::map<std::array<uint64_t,7>,std::shared_ptr<NativeTexture>> renderTextureCache;
bool nativeRenderTexturesEnabled() {
    static const bool enabled=[] {
        const char* value=std::getenv("NIGHT100_REFERENCE_NATIVE_RTT");
        return !value || std::strcmp(value,"1")!=0;
    }();
    return enabled;
}
PS2Runtime::RecompiledFunction sceneExitOriginal=nullptr,sceneInitOriginal=nullptr;
void resetNativeIm3D();
void sceneExit(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc==0x135970){assetCache.retire();renderTextureCache.clear();resetNativeIm3D();}
    sceneExitOriginal(ram,cpu,runtime);
}
void sceneInit(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc==0x135b90){assetCache.retire();renderTextureCache.clear();resetNativeIm3D();}
    sceneInitOriginal(ram,cpu,runtime);
}
// RenderWare's indexed textures store each four-row group in PS2 transfer
// order. Bit permutation follows librw's ps2raster.cpp (MIT, see licenses/).
uint32_t indexedOffset(uint32_t x,uint32_t y,uint32_t width) {
    unsigned logWidth=0;while((1u<<logWidth)<width)++logWidth;
    x^=(((y>>1)^(y>>2))&1u)<<2;
    const uint32_t nx=(x&7u)|((x>>1)&~7u),ny=(y&1u)|((y>>1)&~1u);
    const uint32_t offset=((y>>1)&1u)|(((x>>3)&1u)<<1)|(nx<<2)|(ny<<(logWidth+1));
    return (y&~3u)*width+(offset&((width<<2)-1));
}
std::shared_ptr<NativeTexture> rasterTexture(const Guest& g,uint32_t raster,uint32_t filter,uint32_t wrapU,uint32_t wrapV,bool allowRenderTarget=false) {
    if(!raster)return {};
    const uint32_t pixels=g.u(raster+4),palette=g.u(raster+8);
    const uint32_t width=g.u(raster+0xc),height=g.u(raster+0x10),depth=g.u(raster+0x14),stride=g.u(raster+0x18);
    if(allowRenderTarget && (g.read<uint8_t>(raster+0x20)&7)==5 && nativeRenderTexturesEnabled()) {
        // Camera rasters contain an allocation/staging pointer at cpPixels;
        // their current pixels live in the ordered GPU render target instead.
        if(g.u(raster)!=raster || g.u(raster+0x1c)!=0 || !width || !height || width>2048 || height>2048)
            throw std::runtime_error("Unsupported native camera raster");
        const uint64_t packed=g.read<uint64_t>(raster+0x3c);
        GSTex0Reg source{};
        source.tbp0=packed&0x3fffu;source.tbw=(packed>>14)&63;source.psm=(packed>>20)&63;
        source.tw=(packed>>26)&15;source.th=(packed>>30)&15;
        source.tcc=(packed>>34)&1;source.tfx=(packed>>35)&3;
        if(!source.tbw || (source.tbp0&31) || source.tcc!=1 || source.tfx!=0 || source.tw>11 || source.th>11 ||
            width!=(1u<<source.tw) || height!=(1u<<source.th) || width>uint32_t(source.tbw)*64 ||
            !((depth==32 && (source.psm==GS_PSM_CT32 || source.psm==GS_PSM_CT24)) ||
              (depth==16 && (source.psm==GS_PSM_CT16 || source.psm==GS_PSM_CT16S))))
            throw std::runtime_error("Unsupported native camera texture source");
        const std::array<uint64_t,7> key{raster,packed,width,height,filter,wrapU,wrapV};
        if(auto it=renderTextureCache.find(key);it!=renderTextureCache.end())return it->second;
        auto result=assetCache.createTexture();result->width=width;result->height=height;
        result->linear=filter==2 || filter==4 || filter==6;result->wrapU=wrapU;result->wrapV=wrapV;
        // Native camera textures use the same canonical alpha range as native
        // assets: a CT16 alpha bit expands to zero or fully opaque.
        const uint64_t sourceId=backend->RegisterNativeRenderTexture(source,{0,false,128},result->lifetime);
        if(!sourceId)throw std::runtime_error("Unsupported native camera texture registration");
        result->renderTarget=NativeRenderTexture{sourceId};
        renderTextureCache.emplace(key,result);return result;
    }
    if(!width || !height || width>2048 || height>2048 || (depth!=32 && depth!=8 && depth!=4))throw std::runtime_error("Unsupported native texture layout");
    const uint8_t nativeFlags=g.read<uint8_t>(raster+0x6a);
    const NativeAssetCache::TextureKey key{raster,pixels,palette,width,height,depth,stride,filter,wrapU,wrapV,nativeFlags};
    if(auto it=textureCache.find(key);it!=textureCache.end())return it->second;
    const bool profiling=profileNativeStalls();
    const auto stamp=profiling?NativeCostStamp::now():NativeCostStamp{};
    auto result=assetCache.createTexture();result->width=width;result->height=height;
    result->linear=filter==2 || filter==4 || filter==6;
    result->wrapU=wrapU;result->wrapV=wrapV;
    g.valid(pixels,size_t(stride)*height);
    if(depth!=32)g.valid(palette,depth==8?1024:64);
    result->rgba.resize(size_t(width)*height*4);
    const bool swizzled=(depth==8 && (nativeFlags&2)) || (depth==4 && (nativeFlags&4));
    if(swizzled && (width&(width-1)))throw std::runtime_error("Unsupported indexed texture stride");
    const uint32_t storageWidth=std::max(width,depth==4?32u:16u);
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
        uint32_t color=0;
        if(depth==32)color=g.u(pixels+y*stride+x*4);
        else {
            const uint32_t offset=swizzled?indexedOffset(x,y,storageWidth):y*stride*(depth==4?2:1)+x;
            unsigned index=g.read<uint8_t>(pixels+(depth==8?offset:offset/2));
            if(depth==4)index=(index>>((offset&1)*4))&15;
            else index=(index&~0x18u)|((index&8)<<1)|((index&16)>>1);
            color=g.u(palette+index*4);
        }
        const size_t i=(size_t(y)*width+x)*4;
        result->rgba[i]=color;result->rgba[i+1]=color>>8;result->rgba[i+2]=color>>16;
        result->rgba[i+3]=std::min(255u,(color>>24)*2);
    }
    ControllerPrompts::apply(width,height,result->rgba);
    textureCache.emplace(key,result);
    if(profiling) {
        const auto ms=stamp.milliseconds();
        std::cerr<<"[native-stall] texture-decode raster="<<raster<<" id="<<result->id
            <<" size="<<width<<'x'<<height<<" depth="<<depth<<" wall_ms="<<ms[0]<<" cpu_ms="<<ms[1]<<'\n';
    }
    return result;
}
std::shared_ptr<NativeTexture> texture(const Guest& g,uint32_t pointer) {
    if(!pointer)return {};
    return rasterTexture(g,g.u(pointer),g.u(pointer+0x54),g.u(pointer+0x58),g.u(pointer+0x5c));
}
uint32_t renderState(const Guest& g,R5900Context* cpu,PS2Runtime* runtime,unsigned state) {
    // This SDK getter is a leaf: evaluate it on a copy of the registers, with
    // four temporary bytes below the caller's stack, then restore those bytes.
    const uint32_t scratch=GPR_U32(cpu,29)-16;
    const uint32_t saved=g.u(scratch);
    R5900Context copy=*cpu;auto* c=&copy;c->pc=0x1f5b00;
    SET_GPR_U32(c,4,state);SET_GPR_U32(c,5,scratch);SET_GPR_U32(c,31,0);
    runtime->lookupFunction(0x1f5b00)(g.ram,c,runtime);
    const uint32_t result=g.u(scratch);std::memcpy(g.ram+scratch,&saved,4);
    // The getter returns zero without writing its output for a state its PS2
    // cache cannot express, such as an unsupported blend pair. Never use the
    // untouched scratch word as a render state.
    if(!GPR_U32(c,2))throw std::runtime_error("SDK render state query failed");
    return result;
}
// RenderWare's PS2 blend setter (0x1f5208, states 10/11) keeps one SDK index
// per factor and writes ALPHA_1 from its 6x6 table at 0x2ba170 only when the
// pair is supported. An unsupported pair (for example SRCALPHA/INVDESTALPHA
// from some particle systems) caches ALPHA_1=-1, leaves the GS on its previous
// ALPHA_1 and makes the getter fail. The original VU path therefore blends with
// whatever ALPHA_1 the GS last received. Track that register in guest program
// order at every retail ALPHA_1 writer (all are SDK routines):
//   setter 0x1f5208 (valid pairs), initializer 0x1fc0c8 (0x44),
//   cache re-emitters 0x1feff0/0x21e420/0x21e4b0 (gp-0x7b80, possibly -1),
//   raw state 2 of 0x1f5e70, and constant writers 0x1fec60 (0x44) and the
//   raster renders 0x21ef40/0x21f380/0x27ccd8/0x27d728 (FIX=0x80, 0x02).
// The fixed writers store ALPHA_1 on their only path, so their value is
// recorded at entry. A snapshot restore seeds the register from the restored
// GS when the SDK queue is drained; otherwise it is unknown until the next write.
constexpr uint32_t rwBlendFactors[6]={1,2,5,6,7,8};
int rwBlendIndex(uint32_t factor) {
    for(int i=0;i<6;++i)if(rwBlendFactors[i]==factor)return i;
    return -1;
}
int64_t sdkBlendAlpha(const Guest& g,int source,int destination) {
    const int32_t value=g.read<int32_t>(0x2ba170+destination*24+source*4);
    if(value<0)return -1;
    // The SDK adds FIX=0x80 for ONE/ONE (table selector C=FIX).
    return int64_t(value)|(source==1 && destination==1?int64_t(0x80)<<32:0);
}
// One factor of GS (A-B)*C+D as a RenderWare factor: constant+scaled*C.
bool gsFactor(int constant,int scaled,unsigned c,uint32_t fix,uint32_t& factor) {
    if(scaled && c==2) {
        if(fix!=0x80)return false;
        constant+=scaled;scaled=0;
    }
    if(!scaled && (constant==0 || constant==1)){factor=constant?2:1;return true;}
    if(c==3)return false;
    if(scaled==1 && constant==0){factor=c?7:5;return true;}   // DESTALPHA : SRCALPHA
    if(scaled==-1 && constant==1){factor=c?8:6;return true;}  // INVDESTALPHA : INVSRCALPHA
    return false;
}
}
bool nativeBlendFactors(uint64_t alpha,uint32_t& source,uint32_t& destination) {
    const unsigned a=alpha&3,b=(alpha>>2)&3,c=(alpha>>4)&3,d=(alpha>>6)&3;
    const uint32_t fix=(alpha>>32)&0xff;
    if(a==3 || b==3 || d==3)return false;
    // Coefficients of Cs (selector 0) and Cd (selector 1).
    const int sourceScaled=(a==0)-(b==0),destinationScaled=(a==1)-(b==1);
    return gsFactor(d==0,sourceScaled,c,fix,source) && gsFactor(d==1,destinationScaled,c,fix,destination);
}
namespace {
struct TrackedAlpha {bool known=false;uint64_t value=0;};
TrackedAlpha trackedAlpha;
void writeTrackedAlpha(uint64_t value){trackedAlpha={true,value};}
PS2Runtime::RecompiledFunction blendSetterOriginal=nullptr,blendInitOriginal=nullptr,blendRawOriginal=nullptr;
std::array<PS2Runtime::RecompiledFunction,3> blendReemitOriginal{};
constexpr std::array<uint32_t,3> blendReemitEntries{0x1feff0,0x21e420,0x21e4b0};
struct ConstantAlphaWriter {uint32_t entry;uint64_t alpha;};
constexpr std::array<ConstantAlphaWriter,5> blendConstantWriters{{
    {0x1fec60,0x44},{0x21ef40,0x8000000002ull},{0x21f380,0x8000000002ull},{0x27ccd8,0x8000000002ull},{0x27d728,0x8000000002ull}}};
std::array<PS2Runtime::RecompiledFunction,5> blendConstantOriginal{};
PS2Runtime* sceneRuntime=nullptr;
void trackBlendSetter(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc==0x1f5208) {
        const uint32_t state=GPR_U32(cpu,4);const int index=rwBlendIndex(GPR_U32(cpu,5));
        // Same decision as the SDK, from its inputs and its stored indices.
        if((state==10 || state==11) && index>=0) {
            const Guest g{ram};const uint32_t gp=GPR_U32(cpu,28);
            const int source=state==10?index:int(g.u(gp-0x7b68)),destination=state==11?index:int(g.u(gp-0x7b64));
            if(source>=0 && source<6 && destination>=0 && destination<6)
                if(const int64_t alpha=sdkBlendAlpha(g,source,destination);alpha>=0)writeTrackedAlpha(alpha);
        }
    }
    blendSetterOriginal(ram,cpu,runtime);
}
void trackBlendInit(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc==0x1fc0c8)writeTrackedAlpha(0x44); // SRCALPHA/INVSRCALPHA
    blendInitOriginal(ram,cpu,runtime);
}
void trackBlendRaw(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    // State 2 writes ALPHA_1 from a packed word: ABCD in bits 0-7, FIX in 24-31.
    if(cpu->pc==0x1f5e70 && GPR_U32(cpu,4)==2) {
        const uint64_t packed=GPR_U32(cpu,5);
        writeTrackedAlpha((packed&0xff)|((packed&0xff000000u)<<8));
    }
    blendRawOriginal(ram,cpu,runtime);
}
void trackBlendConstant(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    for(size_t i=0;i<blendConstantWriters.size();++i) {
        if(cpu->pc!=blendConstantWriters[i].entry)continue;
        writeTrackedAlpha(blendConstantWriters[i].alpha);blendConstantOriginal[i](ram,cpu,runtime);return;
    }
    throw std::runtime_error("Unexpected native blend writer entry");
}
void trackBlendReemit(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    for(size_t i=0;i<blendReemitEntries.size();++i) {
        if(cpu->pc!=blendReemitEntries[i])continue;
        // These write the cached ALPHA_1 to the GS, including -1 (unusable).
        const Guest g{ram};writeTrackedAlpha(g.read<uint64_t>(GPR_U32(cpu,28)-0x7b80));
        blendReemitOriginal[i](ram,cpu,runtime);return;
    }
    throw std::runtime_error("Unexpected native blend re-emit entry");
}
// After a restore the GS register is current once the SDK queue (the same
// fields the completed-frame pump drains) holds no unsent packets.
void seedTrackedAlpha(const Guest& g,uint32_t gp,uint64_t gsAlpha) {
    trackedAlpha={};
    if(gp<0x8000 || gp>PS2_RAM_SIZE)return;
    if(g.u(gp-0x7c10)==g.u(gp-0x7c0c) && !g.u(gp-0x6ea4) && !g.u(gp-0x7c70))writeTrackedAlpha(gsAlpha);
}
// The blend the original GS applies at this point, as RenderWare factors.
std::pair<uint32_t,uint32_t> effectiveBlend(const Guest& g,R5900Context* cpu,PS2Runtime* runtime) {
    // NIGHT100_REFERENCE_VU1_OWNERS=1 (or containing "blend") restores the
    // SDK getter alone, keeping rejected pairs on the original VU1 pipeline.
    static const bool reference=[] {
        const char* value=std::getenv("NIGHT100_REFERENCE_VU1_OWNERS");
        return value && (!std::strcmp(value,"1") || std::strstr(value,"blend"));
    }();
    const bool cacheValid=g.read<int64_t>(GPR_U32(cpu,28)-0x7b80)>=0;
    if(!reference && trackedAlpha.known) {
        uint32_t source=0,destination=0;
        if(!nativeBlendFactors(trackedAlpha.value,source,destination))throw std::runtime_error("Unsupported effective PS2 blend");
        return {source,destination};
    }
    if(cacheValid)return {renderState(g,cpu,runtime,10),renderState(g,cpu,runtime,11)};
    throw std::runtime_error("Unknown effective PS2 blend");
}
void traceIm2D(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    const bool indexed=cpu->pc>=0x2055b8 && cpu->pc<0x205620;
    const auto fallback=indexed?im2dIndexedOriginal:im2dOriginal;
    if(cpu->pc!=(indexed?0x2055b8u:0x205548u)){fallback(ram,cpu,runtime);return;}
    static unsigned calls=0;
    try {
        const Guest g{ram};const uint32_t type=GPR_U32(cpu,4),count=GPR_U32(cpu,6),data=GPR_U32(cpu,5);
        if(type<1 || type>5 || count>65536)throw std::runtime_error("Unsupported screen primitive");
        NativeScreenDraw draw;draw.primitive=type;g.valid(data,size_t(count)*64);
        draw.vertices.resize(count);std::memcpy(draw.vertices.data(),ram+data,size_t(count)*64);
        if(indexed) {
            // Retail RwIm2DRenderIndexedPrimitive takes its fifth argument in
            // t0. Snapshot both buffers before their shared HUD scratch is reused.
            const uint32_t indexData=GPR_U32(cpu,7),indexCount=GPR_U32(cpu,8);
            if(indexCount>65536)throw std::runtime_error("Unsupported screen index count");
            if(!indexCount){SET_GPR_U32(cpu,2,1);cpu->pc=GPR_U32(cpu,31);return;}
            g.valid(indexData,size_t(indexCount)*sizeof(uint16_t));draw.indices.resize(indexCount);
            std::memcpy(draw.indices.data(),ram+indexData,size_t(indexCount)*sizeof(uint16_t));
            for(uint16_t index:draw.indices)if(index>=count)throw std::runtime_error("Screen index outside vertex buffer");
        }
        const uint32_t raster=renderState(g,cpu,runtime,1),filter=renderState(g,cpu,runtime,9);
        draw.texture=rasterTexture(g,raster,filter,renderState(g,cpu,runtime,3),renderState(g,cpu,runtime,4));
        draw.depthTest=renderState(g,cpu,runtime,6)!=0;draw.depthWrite=renderState(g,cpu,runtime,8)!=0;
        std::tie(draw.sourceBlend,draw.destinationBlend)=effectiveBlend(g,cpu,runtime);draw.vertexAlpha=renderState(g,cpu,runtime,12)!=0;
        const auto token=backend->QueueNativeScreen(std::move(draw));
        if(calls++<16)std::cout<<"[native-im2d] queued type="<<type<<" count="<<count<<" indexed="<<indexed<<'\n';
        insertNativeMarker(cpu,token,1);return;
    }catch(const std::exception& e){if(calls++<16)std::cerr<<"[native-im2d] fallback: "<<e.what()<<'\n';}
    fallback(ram,cpu,runtime);
}
// Particle staging buffers are reused immediately by the original emitter.
// Each queued command owns its own immutable CPU snapshot. The GPU cache uses
// one reserved ID and replaces its buffer store in marker execution order.
#ifdef NIGHT100_NATIVE_SCENE_TESTING
NativeSceneTesting::SubmitFunction testIm3DSubmit=nullptr;
#endif
PS2Runtime::RecompiledFunction particlesOriginal=nullptr;
constexpr uint64_t particleGeometryId=0xfffffffffffffff0ull;
uint64_t particleRevision=0;
// iRenderTrianglesImmediate's flush state: the active camera and every SDK
// render state it would read when the batch is submitted. Throws if any state
// is unsupported, so callers can keep the original pipeline.
void particleDrawState(const Guest& g,R5900Context* cpu,PS2Runtime* runtime,NativeModelDraw& draw) {
    const uint32_t camera=g.u(0x2b5ad0);
    draw.world={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};draw.camera=g.matrix(camera+0x20);draw.cameraWidened=cameraWidened(camera);draw.projectionType=g.u(camera+0x14);
    draw.zScale=g.f(camera+0x8c);draw.zShift=g.f(camera+0x90);
    draw.depthTest=renderState(g,cpu,runtime,6)!=0;draw.depthWrite=renderState(g,cpu,runtime,8)!=0;
    std::tie(draw.sourceBlend,draw.destinationBlend)=effectiveBlend(g,cpu,runtime);
    draw.vertexAlpha=renderState(g,cpu,runtime,12)!=0;draw.cullMode=renderState(g,cpu,runtime,20);
    if(draw.sourceBlend<1 || draw.sourceBlend>11 || draw.destinationBlend<1 || draw.destinationBlend>11 || draw.cullMode<1 || draw.cullMode>3)
        throw std::runtime_error("Unsupported particle render state");
    // Keep faint particles: the opaque model cutoff would reject alpha
    // below one half. Only fully transparent samples are discarded here.
    draw.alphaReference=1.0e-8f;
    draw.fogEnabled=renderState(g,cpu,runtime,14)!=0;
    if(draw.fogEnabled) {
        if(renderState(g,cpu,runtime,16)!=1)throw std::runtime_error("Unsupported particle fog type");
        const uint32_t rgba=renderState(g,cpu,runtime,15);
        for(unsigned channel=0;channel<4;++channel)draw.fogColor[channel]=float((rgba>>(channel*8))&255)/255.0f;
        draw.fogStart=g.f(camera+0x88);draw.fogEnd=g.f(camera+0x84);
        if(!std::isfinite(draw.fogStart) || !std::isfinite(draw.fogEnd) || draw.fogEnd<=draw.fogStart)
            throw std::runtime_error("Invalid particle fog range");
    }
    NativeMaterial material;
    const uint32_t raster=renderState(g,cpu,runtime,1),filter=renderState(g,cpu,runtime,9);
    material.texture=rasterTexture(g,raster,filter,renderState(g,cpu,runtime,3),renderState(g,cpu,runtime,4));
    draw.materials.push_back(std::move(material));
}
// NIGHT100_GROUND_ORACLE=1 (diagnostic): Ground runs the original, and each
// of its flushes is compared bit-for-bit against the native quads built at entry.
struct GroundOracle {std::vector<NativeVertex> vertices;std::vector<uint16_t> indices;size_t vertex=0,index=0;bool active=false;};
GroundOracle groundOracle;
uint64_t oracleBatches=0,oracleVertices=0,oracleMismatches=0;
bool groundOracleEnabled(){static const bool enabled=std::getenv("NIGHT100_GROUND_ORACLE")!=nullptr;return enabled;}
void checkGroundOracle(const Guest& g,R5900Context* cpu) {
    const uint32_t ra=GPR_U32(cpu,31);
    if(!groundOracle.active || (ra!=0x1af728 && ra!=0x1afd44))return;
    const uint32_t vertices=GPR_U32(cpu,6),count=GPR_U32(cpu,7),indices=GPR_U32(cpu,8),indexCount=GPR_U32(cpu,9);
    ++oracleBatches;
    for(uint32_t i=0;i<count;++i) {
        const size_t n=groundOracle.vertex+i;++oracleVertices;
        if(n>=groundOracle.vertices.size()){++oracleMismatches;continue;}
        const auto& v=groundOracle.vertices[n];const uint32_t a=vertices+i*36;
        bool same=g.u(a+12)==v.color;
        for(unsigned c=0;c<3;++c){const uint32_t bits=g.u(a+c*4);uint32_t mine;std::memcpy(&mine,&v.position[c],4);same&=bits==mine;}
        for(unsigned c=0;c<2;++c){const uint32_t bits=g.u(a+28+c*4);uint32_t mine;std::memcpy(&mine,&v.uv[c],4);same&=bits==mine;}
        if(!same && oracleMismatches<8)std::cerr<<"[ground-oracle] vertex "<<n<<" differs\n";
        oracleMismatches+=!same;
    }
    for(uint32_t i=0;i<indexCount;++i) {
        const size_t n=groundOracle.index+i;
        const bool same=n<groundOracle.indices.size() && uint32_t(g.read<uint16_t>(indices+i*2))+groundOracle.vertex==groundOracle.indices[n];
        if(!same && oracleMismatches<8)std::cerr<<"[ground-oracle] index "<<n<<" differs\n";
        oracleMismatches+=!same;
    }
    groundOracle.vertex+=count;groundOracle.index+=indexCount;
    if(ra==0x1afd44) {
        if(groundOracle.vertex!=groundOracle.vertices.size())++oracleMismatches;
        groundOracle.active=false;
    }
    if((oracleBatches&63)==0)std::cout<<"[ground-oracle] batches="<<oracleBatches<<" vertices="<<oracleVertices<<" mismatches="<<oracleMismatches<<'\n';
}
void renderParticles(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc!=0x1b0740){particlesOriginal(ram,cpu,runtime);return;}
    if(groundOracleEnabled())checkGroundOracle(Guest{ram},cpu);
    static unsigned calls=0,failures=0;
    try {
        const Guest g{ram};
        // This exact retail function has two legacy format arguments before
        // vertices/count/indices/indexCount, despite the shortened DWARF name.
        const uint32_t vertices=GPR_U32(cpu,6),count=GPR_U32(cpu,7);
        const uint32_t indices=GPR_U32(cpu,8),indexCount=GPR_U32(cpu,9);
        if(!count || count>65535 || indexCount>196608 || (indexCount && indexCount%3))
            throw std::runtime_error("Unsupported particle batch size");
        g.valid(vertices,size_t(count)*36);
        if(indexCount)g.valid(indices,size_t(indexCount)*2);
        auto mesh=std::make_shared<NativeGeometry>();
        mesh->id=particleGeometryId;mesh->revision=++particleRevision;mesh->flags=8;mesh->dynamic=true;
        mesh->vertices.resize(count);
        unsigned alphaMin=255,alphaMax=0;
        for(unsigned i=0;i<count;++i) {
            const uint32_t address=vertices+i*36;auto& vertex=mesh->vertices[i];
            for(unsigned component=0;component<3;++component) {
                vertex.position[component]=g.f(address+component*4);
                if(!std::isfinite(vertex.position[component]))throw std::runtime_error("Invalid particle position");
            }
            vertex.color=g.u(address+12);
            alphaMin=std::min(alphaMin,vertex.color>>24);alphaMax=std::max(alphaMax,vertex.color>>24);
            vertex.uv={g.f(address+28),g.f(address+32)};
            if(!std::isfinite(vertex.uv[0]) || !std::isfinite(vertex.uv[1]))throw std::runtime_error("Invalid particle UV");
        }
        if(indexCount) {
            mesh->indices.resize(indexCount);
            for(unsigned i=0;i<indexCount;++i) {
                const auto index=g.read<uint16_t>(indices+i*2);
                if(index>=count)throw std::runtime_error("Particle index exceeds vertex count");
                mesh->indices[i]=index;
            }
        }
        mesh->meshes={{0,indexCount?indexCount:count,0}};
        NativeModelDraw draw;draw.geometry=mesh;draw.indexed=indexCount!=0;
        particleDrawState(g,cpu,runtime,draw);
        // All validation and value snapshots precede the first CPU mutation.
        const bool textured=draw.materials[0].texture!=nullptr;
        const auto token=backend->QueueNativeModel(std::move(draw));
        if(calls++<16)std::cout<<"[native-particles] queued vertices="<<count<<" indices="<<indexCount
            <<" alpha="<<alphaMin<<".."<<alphaMax<<" texture="<<textured<<'\n';
        insertNativeMarker(cpu,token,1);return;
    } catch(const std::exception& e) {
        if(failures++<16)std::cerr<<"[native-particles] fallback: "<<e.what()<<'\n';
    }
    particlesOriginal(ram,cpu,runtime);
}
// iParMgrRenderParSys_Ground (iParMgr.cpp, 0x1af4c0), the zParSysRender
// callback for ground-aligned particles. Native unit: the original's camera-
// axis copy, its xSTFindAsset/RwRenderStateSet guest calls (real calls that may
// yield, resumed at a private continuation), then one native pass over the
// particle list producing the same quads the original writes to its staging
// buffers. Its 120-particle flushes all carry identical state, so they become
// one ordered draw; primitive order is unchanged. Arithmetic uses the runtime's
// own FPU_*/PS2_V* helpers in the original operation order. Anything outside
// the admitted layout runs the original from an exact resume label.
PS2Runtime::RecompiledFunction groundOriginal=nullptr;
constexpr uint32_t groundEntry=0x1af4c0,groundContinuationPC=0x1afd78; // alignment NOP after Ground
constexpr uint32_t groundStaging=0x3a5980,groundGlobal=0x362fd0,groundIndexTable=0x2b3e70,groundCoefficients=0x28a180;
struct GroundBatch {std::shared_ptr<NativeGeometry> mesh;uint32_t cycles=0;};
struct GroundCall {
    uint32_t data=0,group=0,head=0,returnPC=0,stage=0;
    // Stage 3: original flush batches still to submit, in order.
    std::vector<GroundBatch> batches;size_t next=0;uint32_t returnCycles=0;
    std::shared_ptr<const NativeModelDraw> state;
};
std::optional<GroundCall> groundCall;
float floatBits(uint32_t word){float value;std::memcpy(&value,&word,4);return value;}
using V4=std::array<float,4>;
V4 vstore(__m128 value){V4 out;_mm_storeu_ps(out.data(),value);return out;}
__m128 vload(const V4& value){return _mm_loadu_ps(value.data());}
V4 vmul(const V4& a,const V4& b){return vstore(PS2_VMUL(vload(a),vload(b)));}
V4 vadd(const V4& a,const V4& b){return vstore(PS2_VADD(vload(a),vload(b)));}
V4 vsub(const V4& a,const V4& b){return vstore(PS2_VSUB(vload(a),vload(b)));}
V4 vsplat(float f){return {f,f,f,f};}
V4 vfmadd(const V4& acc,const V4& a,const V4& b){return vadd(acc,vmul(a,b));}
void vmask(V4& destination,const V4& source,unsigned lanes){for(unsigned i=0;i<4;++i)if(lanes&(1u<<i))destination[i]=source[i];}
constexpr V4 vf0{0,0,0,1};
// sin/cos of the three Euler bytes. Each lane of the original VU polynomial
// depends only on its own byte, so a per-byte table equals the lane result.
struct GroundTrig {std::array<uint32_t,16> words{};bool valid=false;std::array<float,256> sine{},cosine{};};
GroundTrig groundTrig;
float groundAngle(unsigned byte) {
    if(!byte)return 0.0f;
    float angle=FPU_DIV_S(FPU_CVT_S_W(int32_t(byte)),floatBits(0x437f0000));
    angle=FPU_MUL_S(floatBits(0x40c90fdb),angle);
    if(!FPU_C_OLE_S(angle,floatBits(0x40490fdb)))angle=FPU_SUB_S(angle,floatBits(0x40c90fdb));
    return angle;
}
void buildGroundTrig(const std::array<uint32_t,16>& words) {
    std::array<V4,16> k;
    for(unsigned i=0;i<16;++i){k[i]=vadd(vf0,vsplat(floatBits(words[i])));k[i][3]=0;}
    for(unsigned byte=0;byte<256;++byte) {
        const float t=groundAngle(byte);const V4 v1{t,t,t,0};
        const V4 a=vmul(v1,v1);
        V4 p5=vmul(a,k[5]);V4 acc=vmul(a,k[4]);
        const V4 b=vmul(a,a);
        p5=vfmadd(acc,a,p5);
        acc=vmul(a,k[2]);
        V4 p6=vfmadd(acc,b,k[3]);
        acc=vadd(vf0,k[13]);
        const V4 c=vmul(b,b);
        acc=vfmadd(acc,a,k[0]);acc=vfmadd(acc,b,k[1]);acc=vfmadd(acc,b,p6);
        V4 h=vfmadd(acc,c,p5);
        acc=vmul(b,k[11]);p5=vfmadd(acc,a,k[10]);
        acc=vmul(b,k[9]);p6=vfmadd(acc,a,k[8]);
        acc=vadd(vf0,k[12]);
        const V4 cosine=vfmadd(acc,h,a);
        acc=vfmadd(acc,b,k[7]);acc=vfmadd(acc,a,k[6]);acc=vfmadd(acc,b,p6);
        const V4 sine=vmul(vfmadd(acc,c,p5),v1);
        groundTrig.sine[byte]=sine[0];groundTrig.cosine[byte]=cosine[0];
    }
    groundTrig.words=words;groundTrig.valid=true;
}
// Restores the original's prologue frame and live registers, then continues
// the original at one of its own resume labels (0x1af690 or 0x1af6ac).
void groundResumeOriginal(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime,const GroundCall& call,uint32_t resumePC) {
    const uint32_t sp=GPR_U32(cpu,29)-0xc0;
    if(sp<0x1000 || sp>PS2_RAM_SIZE-0xc0)throw std::runtime_error("Invalid Ground fallback stack");
    // The caller's return address: ra itself now holds this continuation.
    const int64_t savedReturn=int32_t(call.returnPC);const uint64_t zero=0;
    std::memcpy(ram+sp+0x80,&savedReturn,8);std::memcpy(ram+sp+0x88,&zero,8);
    for(unsigned i=0;i<8;++i)std::memcpy(ram+sp+i*16,&cpu->r[16+i],16);
    SET_GPR_U32(cpu,29,sp);SET_GPR_U32(cpu,31,call.returnPC);
    SET_GPR_U32(cpu,16,call.head);SET_GPR_U32(cpu,21,groundStaging);SET_GPR_U32(cpu,22,groundGlobal);SET_GPR_U32(cpu,23,call.group);
    cpu->pc=resumePC;groundOriginal(ram,cpu,runtime);
}
void continueGround(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime);
// The original's JAL to an SDK routine: the runtime's own inline direct call
// (same 8-cycle charge). A callee that yields returns to the continuation.
void groundGuestCall(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime,GroundCall call,uint32_t target,uint32_t site,uint32_t a0,uint32_t a1) {
    SET_GPR_U32(cpu,4,a0);SET_GPR_U32(cpu,5,a1);SET_GPR_U32(cpu,31,groundContinuationPC);
    groundCall=std::move(call);cpu->pc=target;
    if(runtime->dispatchDirectCall(ram,cpu,runtime->lookupFunction(target),target,site,groundContinuationPC,"native Ground"))
        continueGround(ram,cpu,runtime);
}
uint64_t groundBatches=0,groundParticles=0,groundFallbacks=0;
// Builds every visible quad; false means "run the original loop instead".
// Guest-time parity: the original charges 32 EE cycles per taken loop
// back-edge, 8 per direct call (memcpy per visible quad, each flush, the two
// SDK calls) and 8 for its return. The native unit charges the same amounts at
// the same flush/call boundaries so EE timers and the IOP see the same time.
constexpr uint32_t groundEdgeCycles=32,groundCallCycles=8;
std::shared_ptr<NativeGeometry> groundMesh() {
    auto mesh=std::make_shared<NativeGeometry>();mesh->id=particleGeometryId;mesh->flags=8;mesh->dynamic=true;return mesh;
}
bool groundBuild(const Guest& g,GroundCall& call,std::vector<GroundBatch>& batches) {
    batches.clear();
    auto mesh=groundMesh();uint32_t pending=0;bool firstNode=true;
    std::array<uint32_t,16> words;
    for(unsigned i=0;i<16;++i)words[i]=g.u(groundCoefficients+i*4);
    if(!groundTrig.valid || words!=groundTrig.words)buildGroundTrig(words);
    std::array<int16_t,6> corners;
    for(unsigned i=0;i<6;++i){corners[i]=g.read<int16_t>(groundIndexTable+i*2);if(corners[i]<0 || corners[i]>3)return false;}
    std::array<V4,4> planes;
    for(unsigned i=0;i<4;++i)for(unsigned c=0;c<4;++c)planes[i][c]=g.f(groundGlobal+0x170+i*16+c*4);
    const uint32_t command=g.u(call.group+0x20);
    float originU=0,originV=0,width=0,height=0;
    if(command){originU=g.f(command+8);originV=g.f(command+0xc);width=g.f(command+0x1c);height=g.f(command+0x20);}
    unsigned visited=0;
    for(uint32_t node=call.head;node;node=g.u(node)) {
        // lqc2 ignores the low four address bits; admit only aligned nodes.
        if((node&15) || ++visited>65536)return false;
        if(!firstNode)pending+=groundEdgeCycles;
        firstNode=false;
        // Capacity check at the top of every iteration: 120 quads fill the
        // 480-vertex/960-index staging buffers and flush before this node.
        if(mesh->vertices.size()+4>480) {
            batches.push_back({mesh,pending+groundCallCycles});pending=0;mesh=groundMesh();
        }
        auto& vertices=mesh->vertices;auto& indices=mesh->indices;
        g.valid(node,0x38);
        const V4 p{g.f(node+0x10),g.f(node+0x14),g.f(node+0x18),g.f(node+0x1c)};
        V4 acc=vadd(planes[3],vsplat(p[3]));
        acc=vsub(acc,vmul(planes[0],vsplat(p[0])));
        acc=vsub(acc,vmul(planes[1],vsplat(p[1])));
        const V4 cull=vsub(acc,vmul(planes[2],vsplat(p[2])));
        bool culled=false;
        for(float lane:cull){uint32_t bits;std::memcpy(&bits,&lane,4);culled|=(bits>>31)!=0;}
        if(culled)continue;

        const float half=FPU_MUL_S(floatBits(0x3f000000),p[3]);
        const unsigned b0=g.read<uint8_t>(node+0x34),b1=g.read<uint8_t>(node+0x35),b2=g.read<uint8_t>(node+0x36);
        const V4 cosv{groundTrig.cosine[b0],groundTrig.cosine[b1],groundTrig.cosine[b2],0};
        const V4 sinv{groundTrig.sine[b0],groundTrig.sine[b1],groundTrig.sine[b2],0};
        // Euler combination 0x1afa20..0x1afa5c (vf10 = right, vf12 = at).
        V4 vf1{0,0,0,0},vf12{0,0,0,0},vf3{0,0,0,0},vf10{0,0,0,0};
        vmask(vf1,vadd(vf0,vsplat(cosv[0])),1);
        vmask(vf12,vadd(vf0,vsplat(cosv[0])),4);
        V4 accum=vmul({cosv[1],cosv[2],cosv[0],0},{sinv[2],sinv[0],sinv[1],0});
        vmask(vf3,vfmadd(accum,vf0,vf0),7);
        vmask(vf3,vmul(sinv,vsplat(sinv[1])),1);
        vmask(vf3,vadd(vf0,cosv),2);
        vmask(vf1,vsub(vf0,vsplat(sinv[0])),4);
        vmask(vf12,vadd(vf0,vsplat(sinv[0])),1);
        vmask(vf12,vsub(vf0,vsplat(sinv[1])),2);
        accum=vmul(vf3,vsplat(sinv[2]));
        vmask(accum,vfmadd(accum,vf1,vsplat(cosv[2])),5);
        vmask(vf10,vfmadd(accum,vf0,vf0),7);
        vmask(vf12,vmul(vf12,vsplat(cosv[1])),5);
        const float ax=FPU_MUL_S(vf12[0],half),ay=FPU_MUL_S(vf12[1],half),az=FPU_MUL_S(vf12[2],half);
        const float rx=FPU_MUL_S(vf10[0],half),ry=FPU_MUL_S(vf10[1],half),rz=FPU_MUL_S(vf10[2],half);
        const std::array<float,3> center{p[0],p[1],p[2]},right{rx,ry,rz},at{ax,ay,az};
        std::array<NativeVertex,4> quad{};
        for(unsigned c=0;c<3;++c) {
            const float minus=FPU_SUB_S(center[c],right[c]),plus=FPU_ADD_S(center[c],right[c]);
            quad[0].position[c]=FPU_SUB_S(minus,at[c]);quad[3].position[c]=FPU_ADD_S(at[c],minus);
            quad[1].position[c]=FPU_ADD_S(at[c],plus);quad[2].position[c]=FPU_SUB_S(plus,at[c]);
        }
        float u1=0,u2=1,v1=0,v2=1;
        if(command) {
            const unsigned cellX=g.read<uint8_t>(node+0x32),cellY=g.read<uint8_t>(node+0x33);
            u1=FPU_ADD_S(originU,FPU_MUL_S(FPU_CVT_S_W(int32_t(cellX)),width));
            u2=FPU_ADD_S(originU,FPU_MUL_S(FPU_CVT_S_W(int32_t(cellX+1)),width));
            v1=FPU_ADD_S(originV,FPU_MUL_S(FPU_CVT_S_W(int32_t(cellY)),height));
            v2=FPU_ADD_S(originV,FPU_MUL_S(FPU_CVT_S_W(int32_t(cellY+1)),height));
        }
        quad[0].uv={u1,v1};quad[1].uv={u2,v2};quad[2].uv={u2,v1};quad[3].uv={u1,v2};
        const uint32_t color=g.u(node+0xc);
        for(auto& vertex:quad) {
            vertex.color=color;
            // The original flush rejects these in renderParticles.
            for(float f:vertex.position)if(!std::isfinite(f))return false;
            if(!std::isfinite(vertex.uv[0]) || !std::isfinite(vertex.uv[1]))return false;
        }
        const uint16_t base=uint16_t(vertices.size());
        vertices.insert(vertices.end(),quad.begin(),quad.end());
        for(int16_t corner:corners)indices.push_back(uint16_t(base+corner));
        pending+=groundCallCycles; // memcpy of the quad
    }
    if(!mesh->vertices.empty()){batches.push_back({mesh,pending+groundCallCycles});pending=0;}
    // After a flush the original resumes from the scheduler (one dispatch);
    // without one it returns inline to zParSysRender.
    call.returnCycles=pending+(batches.empty()?0:groundCallCycles);
    return true;
}
// Submits the next original flush batch as one ordered native draw. The
// marker returns here for the following batch; the last returns to the caller.
void submitGroundBatch(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime,GroundCall call) {
    auto batch=std::move(call.batches[call.next++]);
    runtime->eeCheckpointDue(batch.cycles);
    batch.mesh->revision=++particleRevision;batch.mesh->meshes={{0,uint32_t(batch.mesh->indices.size()),0}};
    NativeModelDraw draw=*call.state;draw.geometry=batch.mesh;
    ++groundBatches;groundParticles+=batch.mesh->vertices.size()/4;
    if(groundBatches<=4)std::cout<<"[native-ground] queued particles="<<batch.mesh->vertices.size()/4<<'\n';
    const auto token=
#ifdef NIGHT100_NATIVE_SCENE_TESTING
        testIm3DSubmit?testIm3DSubmit(std::move(draw)):
#endif
        backend->QueueNativeModel(std::move(draw));
    if(call.next<call.batches.size()) {
        SET_GPR_U32(cpu,31,groundContinuationPC);call.stage=3;groundCall=std::move(call);
    } else {
        runtime->eeCheckpointDue(call.returnCycles);SET_GPR_U32(cpu,31,call.returnPC);
    }
    insertNativeMarker(cpu,token,1);
}
void finishGround(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime,GroundCall call) {
    const Guest g{ram};
    bool built=false;
    try {
        built=groundBuild(g,call,call.batches);
        if(built && !call.batches.empty()) {
            auto state=std::make_shared<NativeModelDraw>();state->indexed=true;particleDrawState(g,cpu,runtime,*state);
            call.state=std::move(state);
        }
    } catch(const std::exception& e) {
        if(groundFallbacks<16)std::cerr<<"[native-ground] fallback: "<<e.what()<<'\n';
        built=false;
    }
    if(!built){++groundFallbacks;groundResumeOriginal(ram,cpu,runtime,call,0x1af6ac);return;}
    // The original leaves both staging counters at zero after its last flush.
    std::memset(ram+groundStaging+0x18,0,8);
    if(call.batches.empty()) {
        if(call.returnCycles)runtime->eeCheckpointDue(call.returnCycles);
        SET_GPR_U32(cpu,31,call.returnPC);cpu->pc=call.returnPC;return;
    }
    call.next=0;submitGroundBatch(ram,cpu,runtime,std::move(call));
}
void continueGround(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(!groundCall)throw std::runtime_error("Unmatched native Ground continuation");
    auto call=std::move(*groundCall);groundCall.reset();
    if(call.stage==3){submitGroundBatch(ram,cpu,runtime,std::move(call));return;}
    if(call.stage==1) {
        // xSTFindAsset returned: RwRenderStateSet(TEXTURERASTER, *asset) if any.
        const uint32_t asset=GPR_U32(cpu,2);
        if(asset) {
            if(asset<0x1000 || asset>PS2_RAM_SIZE-4){groundResumeOriginal(ram,cpu,runtime,call,0x1af690);return;}
            uint32_t raster=0;std::memcpy(&raster,ram+asset,4);
            if(raster){call.stage=2;groundGuestCall(ram,cpu,runtime,std::move(call),0x1ddc48,0x1af6a4,1,raster);return;}
        }
    }
    finishGround(ram,cpu,runtime,std::move(call));
}
void renderGround(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc==groundEntry && groundOracleEnabled()) {
        try {
            GroundCall call;call.data=GPR_U32(cpu,4);call.group=GPR_U32(cpu,5);call.head=Guest{ram}.u(call.group);
            std::vector<GroundBatch> batches;
            if(groundBuild(Guest{ram},call,batches)) {
                groundOracle={{},{},0,0,true};
                for(const auto& batch:batches) {
                    const auto base=uint16_t(groundOracle.vertices.size());
                    groundOracle.vertices.insert(groundOracle.vertices.end(),batch.mesh->vertices.begin(),batch.mesh->vertices.end());
                    for(auto index:batch.mesh->indices)groundOracle.indices.push_back(uint16_t(base+index));
                }
            } else groundOracle.active=false;
        }catch(const std::exception&){groundOracle.active=false;}
        groundOriginal(ram,cpu,runtime);return;
    }
    if(cpu->pc!=groundEntry || referenceParticles() || groundCall){groundOriginal(ram,cpu,runtime);return;}
    try {
        const Guest g{ram};
        const uint32_t data=GPR_U32(cpu,4),group=GPR_U32(cpu,5);
        g.valid(group,0x24);
        // The original's alternate prologue copies camera axes from address 0.
        if(!g.u(groundGlobal))throw std::runtime_error("Ground camera global is unset");
        const uint32_t camera=g.u(0x2b5ad0),frame=g.u(camera+4);g.valid(frame+0x10,0x40);
        if(g.u(groundStaging+0x18) || g.u(groundStaging+0x1c) || g.u(groundStaging+0x14)!=36)
            throw std::runtime_error("Ground staging buffers are not empty 36-byte batches");
        const uint32_t asset=g.u(g.u(data+0x10)+0x10);
        // Same writes as the original prologue: camera frame axes, then the
        // first two axes negated. Other particle callbacks read these.
        std::array<uint32_t,16> axes;
        for(unsigned i=0;i<16;++i)axes[i]=g.u(frame+0x10+i*4);
        std::memcpy(ram+groundStaging+0x20,axes.data(),64);
        for(unsigned i=0;i<3;++i) {
            const float x=FPU_MUL_S(floatBits(axes[i]),-1.0f),y=FPU_MUL_S(floatBits(axes[4+i]),-1.0f);
            std::memcpy(ram+groundStaging+0x60+i*4,&x,4);std::memcpy(ram+groundStaging+0x70+i*4,&y,4);
        }
        GroundCall call;call.data=data;call.group=group;call.head=g.u(group);call.returnPC=GPR_U32(cpu,31);call.stage=1;
        groundGuestCall(ram,cpu,runtime,std::move(call),0x120ab0,0x1af688,asset,0);return;
    } catch(const std::exception& e) {
        if(groundFallbacks++<16)std::cerr<<"[native-ground] fallback: "<<e.what()<<'\n';
    }
    groundOriginal(ram,cpu,runtime);
}
// The generic SDK ABI is bridged as a complete Transform/Render/End unit. Only
// admitted callsites take it: each immediately renders one nonindexed primitive
// of a fixed type, without intervening state setters. The two shadow receiver
// callsites are built in; installNativeVU1 admits further proved game features.
// Other callers retain their original transform pipeline in full.
PS2Runtime::RecompiledFunction im3dTransformOriginal=nullptr,im3dRenderOriginal=nullptr,im3dEndOriginal=nullptr;
constexpr uint64_t shadowGeometryId=0xffffffffffffffefull;
uint64_t shadowRevision=0;
const std::vector<NativeIm3DSite> builtinIm3DSites{
    {0x1c33f4,0x1c3404,0x1c340c,3}, // ShadowRender
    {0x1c3770,0x1c3780,0x1c3788,3}, // ShadowRenderTriangleCB
};
std::vector<NativeIm3DSite> im3dSites=builtinIm3DSites;
const NativeIm3DSite* im3dSite(uint32_t transformReturn) {
    for(const auto& site:im3dSites)if(site.transformReturn==transformReturn)return &site;
    return nullptr;
}
struct NativeIm3DBatch {
    uint32_t plugin=0,renderReturn=0,endReturn=0,primitive=3;
    NativeModelDraw draw;
    bool rendered=false;
};
std::optional<NativeIm3DBatch> im3dBatch;
void resetNativeIm3D(){im3dBatch.reset();}
void transformNativeIm3D(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc!=0x209318){im3dTransformOriginal(ram,cpu,runtime);return;}
    const uint32_t caller=GPR_U32(cpu,31);
    const NativeIm3DSite* site=im3dSite(caller);
    if(!nativeRenderTexturesEnabled() || !site) {
        resetNativeIm3D();im3dTransformOriginal(ram,cpu,runtime);return;
    }
    static unsigned calls=0,failures=0;
    try {
        const Guest g{ram};
        const uint32_t vertices=GPR_U32(cpu,4),count=GPR_U32(cpu,5),matrix=GPR_U32(cpu,6),flags=GPR_U32(cpu,7);
        const bool strip=site->primitive==4;
        if(!vertices || count<3 || count>65535 || (!strip && count%3) || matrix || flags!=0x19)
            throw std::runtime_error("Unsupported shadow Im3D transform");
        const uint32_t offset=g.u(GPR_U32(cpu,28)-0x7aa0);
        if(offset>PS2_RAM_SIZE-0x2b5ad0-0x9c)throw std::runtime_error("Invalid Im3D plugin offset");
        const uint32_t plugin=0x2b5ad0+offset+0x60;g.valid(plugin,60);g.valid(vertices,size_t(count)*36);
        auto mesh=std::make_shared<NativeGeometry>();
        // Prelit (8); rpGEOMETRYTRISTRIP (1) selects strip assembly in both backends.
        mesh->id=shadowGeometryId;mesh->revision=++shadowRevision;mesh->flags=strip?9:8;mesh->dynamic=true;
        mesh->vertices.resize(count);mesh->meshes={{0,count,0}};
        for(unsigned i=0;i<count;++i) {
            const uint32_t address=vertices+i*36;auto& vertex=mesh->vertices[i];
            for(unsigned c=0;c<3;++c) {
                vertex.position[c]=g.f(address+c*4);
                if(!std::isfinite(vertex.position[c]))throw std::runtime_error("Invalid shadow Im3D position");
            }
            vertex.color=g.u(address+12);vertex.uv={g.f(address+28),g.f(address+32)};
            if(!std::isfinite(vertex.uv[0]) || !std::isfinite(vertex.uv[1]))
                throw std::runtime_error("Invalid shadow Im3D UV");
        }
        NativeIm3DBatch batch;batch.plugin=plugin;batch.renderReturn=site->renderReturn;batch.endReturn=site->endReturn;batch.primitive=site->primitive;
        auto& draw=batch.draw;draw.geometry=std::move(mesh);draw.indexed=false;
        const uint32_t camera=g.u(0x2b5ad0);
        draw.world={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};draw.camera=g.matrix(camera+0x20);draw.cameraWidened=cameraWidened(camera);
        draw.projectionType=g.u(camera+0x14);draw.zScale=g.f(camera+0x8c);draw.zShift=g.f(camera+0x90);
        draw.depthTest=renderState(g,cpu,runtime,6)!=0;draw.depthWrite=renderState(g,cpu,runtime,8)!=0;
        std::tie(draw.sourceBlend,draw.destinationBlend)=effectiveBlend(g,cpu,runtime);
        draw.vertexAlpha=renderState(g,cpu,runtime,12)!=0;draw.cullMode=renderState(g,cpu,runtime,20);
        if(draw.sourceBlend<1 || draw.sourceBlend>11 || draw.destinationBlend<1 || draw.destinationBlend>11 ||
            draw.cullMode<1 || draw.cullMode>3 || (draw.projectionType!=1 && draw.projectionType!=2))
            throw std::runtime_error("Unsupported shadow Im3D render state");
        draw.alphaReference=1.0e-8f;draw.fogEnabled=renderState(g,cpu,runtime,14)!=0;
        if(draw.fogEnabled) {
            if(renderState(g,cpu,runtime,16)!=1)throw std::runtime_error("Unsupported shadow Im3D fog type");
            const uint32_t rgba=renderState(g,cpu,runtime,15);
            for(unsigned c=0;c<4;++c)draw.fogColor[c]=float((rgba>>(c*8))&255)/255.0f;
            draw.fogStart=g.f(camera+0x88);draw.fogEnd=g.f(camera+0x84);
            if(!std::isfinite(draw.fogStart) || !std::isfinite(draw.fogEnd) || draw.fogEnd<=draw.fogStart)
                throw std::runtime_error("Invalid shadow Im3D fog range");
        }
        NativeMaterial material;
        material.texture=rasterTexture(g,renderState(g,cpu,runtime,1),renderState(g,cpu,runtime,9),
            renderState(g,cpu,runtime,3),renderState(g,cpu,runtime,4),true);
        draw.materials.push_back(std::move(material));
        // Preserve the SDK's public input descriptor. Its pipeline workspace is
        // unnecessary on this admitted unit; End still clears the same 60 bytes.
        const uint16_t count16=count;const uint32_t stride=36,nativeFlags=flags|0x18;
        std::memcpy(ram+plugin,&count16,2);std::memcpy(ram+plugin+4,&vertices,4);
        std::memcpy(ram+plugin+8,&stride,4);std::memcpy(ram+plugin+12,&nativeFlags,4);
        std::memcpy(ram+plugin+16,&matrix,4);im3dBatch=std::move(batch);
        SET_GPR_U32(cpu,2,vertices);cpu->pc=caller;
        if(calls++<8)std::cout<<"[native-shadow] transformed vertices="<<count<<'\n';
        return;
    }catch(const std::exception& error) {
        if(failures++<8)std::cerr<<"[native-shadow] fallback: "<<error.what()<<'\n';
    }
    // No guest pipeline state has been changed before all checks succeed.
    resetNativeIm3D();im3dTransformOriginal(ram,cpu,runtime);
}
void renderNativeIm3D(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc!=0x208910 || !im3dBatch){im3dRenderOriginal(ram,cpu,runtime);return;}
    if(GPR_U32(cpu,4)!=im3dBatch->primitive || GPR_U32(cpu,31)!=im3dBatch->renderReturn || im3dBatch->rendered)
        throw std::runtime_error("Native shadow Im3D caller contract changed");
    const auto token=
#ifdef NIGHT100_NATIVE_SCENE_TESTING
        testIm3DSubmit?testIm3DSubmit(std::move(im3dBatch->draw)):
#endif
        backend->QueueNativeModel(std::move(im3dBatch->draw));
    im3dBatch->rendered=true;insertNativeMarker(cpu,token,1);
}
void endNativeIm3D(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc!=0x2093c0 || !im3dBatch){im3dEndOriginal(ram,cpu,runtime);return;}
    if(GPR_U32(cpu,31)!=im3dBatch->endReturn)
        throw std::runtime_error("Native shadow Im3D end contract changed");
    const Guest g{ram};const bool active=g.u(im3dBatch->plugin+4)!=0;
    if(active)std::memset(ram+im3dBatch->plugin,0,60);
    resetNativeIm3D();SET_GPR_U32(cpu,2,active?1:0);cpu->pc=GPR_U32(cpu,31);
}
std::shared_ptr<NativeGeometry> geometry(const Guest& g,uint32_t pointer) {
    const uint32_t flags=g.u(pointer+8),count=g.u(pointer+0x14),header=g.u(pointer+0x60),morph=g.u(pointer+0x68);
    const uint32_t positions=g.u(morph+0x14),normals=g.u(morph+0x18),colors=g.u(pointer+0x30),uv=g.u(pointer+0x34);
    if(!count || count>65535 || (flags&0x1000000u))throw std::runtime_error("Unsupported native geometry layout");
    const uint16_t meshes=g.read<uint16_t>(header+4),serial=g.read<uint16_t>(header+6);
    if(!meshes || meshes>4096)throw std::runtime_error("Unsupported native mesh list");
    const uint64_t key=(uint64_t(pointer)<<32)|serial;
    const bool dirty=g.read<uint16_t>(pointer+0xc)!=0;
    auto it=geometryCache.find(key);
    if(it!=geometryCache.end() && !dirty)return it->second;
    // Queued commands retain their immutable geometry until their GIF marker
    // executes. A later morph edit must not mutate an earlier queued draw.
    auto result=assetCache.createGeometry(it==geometryCache.end()?nullptr:it->second);
    result->flags=flags;result->vertices.resize(count);
    const uint32_t skinOffset=g.u(0x2fe440),skin=skinOffset && skinOffset<0x1000?g.u(pointer+skinOffset):0;
    uint32_t weights=0,indices=0;
    if(skin) {
        result->boneCount=g.u(skin);
        if(!result->boneCount || result->boneCount>64)throw std::runtime_error("Native bone palette exceeds 64 entries");
        const uint32_t inverse=g.u(skin+4);indices=g.u(skin+8);weights=g.u(skin+0xc);
        g.valid(weights,size_t(count)*16);g.valid(indices,size_t(count)*4);
        for(unsigned i=0;i<result->boneCount;++i)result->inverseBind[i]=g.matrix(inverse+i*64);
    }
    g.valid(positions,size_t(count)*12);
    if(normals)g.valid(normals,size_t(count)*12);
    if(colors)g.valid(colors,size_t(count)*4);
    if(uv)g.valid(uv,size_t(count)*8);
    for(uint32_t i=0;i<count;++i) {
        auto& v=result->vertices[i];std::memcpy(v.position.data(),g.ram+positions+i*12,12);
        if(normals)std::memcpy(v.normal.data(),g.ram+normals+i*12,12);
        if(colors)v.color=g.u(colors+i*4);
        if(uv)std::memcpy(v.uv.data(),g.ram+uv+i*8,8);
        if(skin)for(unsigned j=0;j<4;++j) {
            v.bones[j]=g.read<uint8_t>(indices+i*4+j);
            // The PS2 instance step packs a matrix index in the low ten
            // mantissa bits. Recover the original float weight for the GPU.
            const uint32_t bits=g.u(weights+i*16+j*4)&~0x3ffu;
            std::memcpy(&v.weights[j],&bits,4);
            if(v.weights[j]>0 && v.bones[j]>=result->boneCount)throw std::runtime_error("Native bone index out of range");
        }
    }
    result->indices.clear();result->meshes.clear();
    const uint32_t first=header+16+g.u(header+0xc);
    for(unsigned i=0;i<meshes;++i) {
        const uint32_t mesh=first+i*12,data=g.u(mesh),n=g.u(mesh+4),material=g.u(mesh+8);
        if(n>1000000)throw std::runtime_error("Native mesh index range too large");
        g.valid(data,size_t(n)*2);
        NativeMesh out{uint32_t(result->indices.size()),n,material};
        for(uint32_t j=0;j<n;++j){const auto index=g.read<uint16_t>(data+j*2);if(index>=count)throw std::runtime_error("Native vertex index out of range");result->indices.push_back(index);}
        result->meshes.push_back(out);
    }
    geometryCache[key]=result;
    return result;
}
NativeLights lights(const Guest& g) {
    NativeLights result{};
    const uint32_t scene=g.u(0x362fd0+0xb94),env=g.u(scene+0x40),world=g.u(g.u(env));
    for(const uint32_t head:{world+0x34,world+0x3c}) {
        unsigned visited=0;
        for(uint32_t node=g.u(head);node!=head && node && visited++<64;node=g.u(node)) {
            const uint32_t light=node-0x34,type=g.read<uint8_t>(light+1),frame=g.u(light+4);
            if(type==2)for(unsigned c=0;c<3;++c)result.ambient[c]+=g.f(light+0x18+c*4);
            else if(type==1 && frame && result.count<8) {
                const auto i=result.count++;
                for(unsigned c=0;c<3;++c){result.color[i][c]=g.f(light+0x18+c*4);result.direction[i][c]=g.f(frame+0x50+0x20+c*4);}
            }
        }
    }
    return result;
}
void captureNativeModel(const Guest& g,uint32_t atomic,uint32_t geo,uint32_t frame,const NativeModelDraw& draw) {
    const char* path=std::getenv("NIGHT100_CAPTURE_NATIVE");if(!path)return;
    const auto mode=g.u(0x357e20);const uint64_t key=(uint64_t(mode)<<32)|geo;
    static std::unordered_map<uint64_t,std::pair<uint64_t,unsigned>> captured;
    static unsigned total=0;
    auto& previous=captured[key];
    if(total>=256 || (previous.second && (mode!=11 || previous.second>=4 || previous.first==draw.geometry->revision)))return;
    previous.first=draw.geometry->revision;++previous.second;++total;
    const auto directory=std::filesystem::path(path)/(std::to_string(mode)+"-"+std::to_string(geo)+"-"+std::to_string(draw.geometry->revision));
    std::filesystem::create_directories(directory);
    auto bytes=[&](const char* name,const void* p,size_t n) {
        std::ofstream out(directory/name,std::ios::binary);out.write(static_cast<const char*>(p),n);
    };
    const auto& model=*draw.geometry;
    const uint32_t morph=g.u(geo+0x68),positions=g.u(morph+0x14),normals=g.u(morph+0x18);
    bytes("guest-positions.bin",g.ram+positions,model.vertices.size()*12);
    bytes("vertices.bin",model.vertices.data(),model.vertices.size()*sizeof(NativeVertex));
    bytes("inverse.bin",model.inverseBind.data(),sizeof(model.inverseBind));
    bytes("pose.bin",draw.pose.data(),sizeof(draw.pose));
    bytes("world.bin",draw.world.data(),sizeof(draw.world));bytes("camera.bin",draw.camera.data(),sizeof(draw.camera));
    uint32_t skin=0,hierarchy=0,poses=0;
    const auto offset=g.u(0x2fe440);if(offset && offset<0x1000)skin=g.u(geo+offset);
    if(model.boneCount) {
        hierarchy=g.u(g.u(frame+0x98)+g.u(0x2caa88)+4);poses=g.u(hierarchy+0x28);
        bytes("guest-inverse.bin",g.ram+g.u(skin+4),size_t(model.boneCount)*64);
        bytes("guest-weights.bin",g.ram+g.u(skin+0xc),model.vertices.size()*16);
        bytes("guest-indices.bin",g.ram+g.u(skin+8),model.vertices.size()*4);
    }
    std::ofstream meta(directory/"meta.json");
    meta<<"{\"atomic\":"<<atomic<<",\"geometry\":"<<geo<<",\"frame\":"<<frame
        <<",\"camera\":"<<g.u(0x2b5ad0)
        <<",\"skin\":"<<skin<<",\"hierarchy\":"<<hierarchy<<",\"poses\":"<<poses
        <<",\"mode\":"<<mode<<",\"flags\":"<<model.flags<<",\"bone_count\":"<<model.boneCount
        <<",\"vertices\":"<<model.vertices.size()<<",\"hierarchy_flags\":"<<(hierarchy?g.u(hierarchy):0)
        <<",\"positions\":"<<positions<<",\"normals\":"<<normals<<",\"revision\":"<<model.revision
        <<",\"dirty\":"<<g.read<uint16_t>(geo+0xc)<<",\"z_scale\":"<<draw.zScale<<",\"z_shift\":"<<draw.zShift<<"}\n";
}
PS2Runtime::RecompiledFunction worldOriginal=nullptr;
std::shared_ptr<NativeGeometry> worldGeometry(const Guest& g,uint32_t sector,uint32_t world) {
    const uint32_t header=g.u(sector+0x84),count=g.read<uint16_t>(sector+0x8e);
    const uint64_t key=(uint64_t(sector)<<32)|g.read<uint16_t>(header+6);
    if(auto it=geometryCache.find(key);it!=geometryCache.end())return it->second;
    if(!count || (g.u(world+8)&0x1000000))throw std::runtime_error("Unsupported world geometry");
    auto result=assetCache.createGeometry();result->flags=g.u(world+8);result->vertices.resize(count);
    const uint32_t positions=g.u(sector+8),normals=g.u(sector+0xc),uv=g.u(sector+0x10),colors=g.u(sector+0x30);
    g.valid(positions,size_t(count)*12);if(normals)g.valid(normals,size_t(count)*4);
    if(uv)g.valid(uv,size_t(count)*8);if(colors)g.valid(colors,size_t(count)*4);
    for(unsigned i=0;i<count;++i) {
        auto& v=result->vertices[i];std::memcpy(v.position.data(),g.ram+positions+i*12,12);
        if(normals)for(unsigned j=0;j<3;++j)v.normal[j]=float(g.read<int8_t>(normals+i*4+j))/128.0f;
        if(uv)std::memcpy(v.uv.data(),g.ram+uv+i*8,8);if(colors)v.color=g.u(colors+i*4);
    }
    const unsigned meshes=g.read<uint16_t>(header+4);if(meshes>4096)throw std::runtime_error("Invalid world mesh count");
    const uint32_t first=header+16+g.u(header+0xc);
    for(unsigned i=0;i<meshes;++i) {
        const uint32_t p=first+i*12,data=g.u(p),n=g.u(p+4);if(n>1000000)throw std::runtime_error("Invalid world index range");g.valid(data,size_t(n)*2);
        result->meshes.push_back({uint32_t(result->indices.size()),n,g.u(p+8)});
        for(unsigned j=0;j<n;++j){const auto index=g.read<uint16_t>(data+j*2);if(index>=count)throw std::runtime_error("Invalid world vertex index");result->indices.push_back(index);}
    }
    geometryCache[key]=result;return result;
}
void renderWorld(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc!=0x22f848){worldOriginal(ram,cpu,runtime);return;}
    const uint32_t sector=GPR_U32(cpu,4);static unsigned calls=0;
    try {
        const Guest g{ram};const uint32_t world=g.u(0x2b5ad4),camera=g.u(0x2b5ad0);
        if(!g.read<uint16_t>(sector+0x90)){SET_GPR_U32(cpu,2,sector);cpu->pc=GPR_U32(cpu,31);return;}
        NativeModelDraw draw;draw.geometry=worldGeometry(g,sector,world);
        draw.world={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};draw.camera=g.matrix(camera+0x20);draw.cameraWidened=cameraWidened(camera);draw.projectionType=g.u(camera+0x14);
        draw.zScale=g.f(camera+0x8c);draw.zShift=g.f(camera+0x90);draw.lights=lights(g);
        draw.depthTest=renderState(g,cpu,runtime,6)!=0;draw.depthWrite=renderState(g,cpu,runtime,8)!=0;
        for(const auto& mesh:draw.geometry->meshes) {
            NativeMaterial material;const uint32_t rgba=g.u(mesh.material+4);
            for(unsigned c=0;c<4;++c)material.color[c]=float((rgba>>(c*8))&255)/255.0f;
            material.ambient=g.f(mesh.material+0xc);material.diffuse=g.f(mesh.material+0x14);material.texture=texture(g,g.u(mesh.material));draw.materials.push_back(material);
        }
        const auto token=backend->QueueNativeModel(std::move(draw));
        if(calls++<8)std::cout<<"[native-world] queued sector=0x"<<std::hex<<sector<<std::dec<<'\n';
        insertNativeMarker(cpu,token,sector);return;
    }catch(const std::exception& e){if(calls++<16)std::cerr<<"[native-world] fallback: "<<e.what()<<'\n';}
    worldOriginal(ram,cpu,runtime);
}
void render(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc!=0x2250c0){original(ram,cpu,runtime);return;}
    const uint32_t atomic=GPR_U32(cpu,4);
    try {
        // RwCameraBeginUpdate (0x1e6828) stores the active camera here and
        // EndUpdate (0x1e67ec) clears it. Shadows and model previews temporarily
        // replace the game camera, so the native draw must follow that camera.
        const Guest g{ram};const uint32_t geo=g.u(atomic+0x18),frame=g.u(atomic+4),camera=g.u(0x2b5ad0);
        NativeModelDraw draw;draw.geometry=geometry(g,geo);draw.world=g.matrix(frame+0x50);draw.camera=g.matrix(camera+0x20);draw.cameraWidened=cameraWidened(camera);draw.projectionType=g.u(camera+0x14);
        draw.hud=hud2DDepth>0;
        draw.zScale=g.f(camera+0x8c);draw.zShift=g.f(camera+0x90);draw.lights=lights(g);
        // ShadowCameraUpdate explicitly disables Z for its color-only raster.
        // Its unused Z address aliases visible GS memory, so honoring this
        // policy is necessary even when the shadow color target is separate.
        draw.depthTest=renderState(g,cpu,runtime,6)!=0;draw.depthWrite=renderState(g,cpu,runtime,8)!=0;
        if(draw.geometry->boneCount) {
            const uint32_t child=g.u(frame+0x98),offset=g.u(0x2caa88),hierarchy=g.u(child+offset+4),poses=g.u(hierarchy+0x28);
            if(!(g.u(hierarchy)&0x4000u))throw std::runtime_error("Native skin requires model-space bone matrices");
            for(unsigned i=0;i<draw.geometry->boneCount;++i)draw.pose[i]=g.matrix(poses+i*64);
        }
        for(const auto& mesh:draw.geometry->meshes) {
            NativeMaterial material;const uint32_t rgba=g.u(mesh.material+4);
            for(unsigned c=0;c<4;++c)material.color[c]=float((rgba>>(c*8))&255)/255.0f;
            material.ambient=g.f(mesh.material+0xc);material.diffuse=g.f(mesh.material+0x14);material.texture=texture(g,g.u(mesh.material));draw.materials.push_back(material);
        }
        captureNativeModel(g,atomic,geo,frame,draw);
        const auto token=backend->QueueNativeModel(std::move(draw));
        const uint16_t clean=0;std::memcpy(ram+geo+0xc,&clean,2);
        if(nativeCalls++<8 || traceAllNativeScene())std::cout<<"[native-scene] queued atomic=0x"<<std::hex<<atomic<<std::dec<<'\n';
        insertNativeMarker(cpu,token,atomic);return;
    }catch(const std::exception& e) {
        if(fallbackCalls++<16 || traceAllNativeScene())std::cerr<<"[native-scene] fallback atomic=0x"<<std::hex<<atomic<<std::dec<<" ra=0x"<<std::hex<<GPR_U32(cpu,31)<<std::dec<<": "<<e.what()<<'\n';
    }
    original(ram,cpu,runtime);
}
}
#ifdef NIGHT100_NATIVE_SCENE_TESTING
namespace NativeSceneTesting {
void setup(GuestFunction transform,GuestFunction render,GuestFunction end,SubmitFunction submit) {
    im3dTransformOriginal=transform;im3dRenderOriginal=render;im3dEndOriginal=end;testIm3DSubmit=submit;
    resetNativeIm3D();markers.clear();assetCache.retire();renderTextureCache.clear();
    im3dSites=builtinIm3DSites;trackedAlpha={};
}
void setupBlendTracking(GuestFunction setter,GuestFunction init,GuestFunction reemit) {
    blendSetterOriginal=setter;blendInitOriginal=init;blendRawOriginal=reemit;
    blendReemitOriginal.fill(reemit);blendConstantOriginal.fill(reemit);
}
void blendConstant(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime){trackBlendConstant(ram,cpu,runtime);}
void blendRaw(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime){trackBlendRaw(ram,cpu,runtime);}
void seedBlendAfterRestore(uint8_t* ram,uint32_t gp,uint64_t gsAlpha){seedTrackedAlpha(Guest{ram},gp,gsAlpha);}
void blendSetter(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime){trackBlendSetter(ram,cpu,runtime);}
void blendInit(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime){trackBlendInit(ram,cpu,runtime);}
void blendReemit(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime){trackBlendReemit(ram,cpu,runtime);}
void setupGround(GuestFunction original){groundOriginal=original;groundCall.reset();}
void ground(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime){renderGround(ram,cpu,runtime);}
void groundContinuation(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime){continueGround(ram,cpu,runtime);}
bool groundPending(){return groundCall.has_value();}
void transform(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime){transformNativeIm3D(ram,cpu,runtime);}
void render(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime){renderNativeIm3D(ram,cpu,runtime);}
void end(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime){endNativeIm3D(ram,cpu,runtime);}
void finishMarker(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime){finishNativeMarker(ram,cpu,runtime);}
const NativeModelDraw* pendingDraw(){return im3dBatch?&im3dBatch->draw:nullptr;}
}
#endif
void configureNativeScene(NativeSceneBackend* value){backend=value;resetNativeIm3D();}
void admitNativeIm3DSite(const NativeIm3DSite& site) {
    if(site.primitive!=3 && site.primitive!=4)throw std::runtime_error("Unsupported native Im3D site primitive");
    if(const auto* existing=im3dSite(site.transformReturn)) {
        if(existing->renderReturn!=site.renderReturn || existing->endReturn!=site.endReturn || existing->primitive!=site.primitive)
            throw std::runtime_error("Conflicting native Im3D site contract");
        return;
    }
    im3dSites.push_back(site);
}
bool nativeSceneTrackedAlpha(uint64_t& alpha){alpha=trackedAlpha.value;return trackedAlpha.known;}
bool nativeSceneCanSnapshot(){return pendingFrames.empty() && markers.empty() && !im3dBatch && !groundCall;}
void resetNativeSceneAfterSnapshot(){
    if(!nativeSceneCanSnapshot())throw std::runtime_error("Cannot restore across active native continuations");
    assetCache.retire();renderTextureCache.clear();resetNativeIm3D();trackedAlpha={};
    // Machine state, including the GS registers, is already restored. The SDK
    // keeps its gp (for the gp-relative queue and blend cache) at 0x2b9bc8.
    if(sceneRuntime && backend) {
        uint8_t* ram=sceneRuntime->memory().getRDRAM();uint32_t gp=0;std::memcpy(&gp,ram+0x2b9bc8,4);
        seedTrackedAlpha(Guest{ram},gp,sceneRuntime->gs().getDebugSnapshot().ctx[0].alpha);
        std::cout<<"[native-scene] restored ALPHA_1 tracking: "<<(trackedAlpha.known?"seeded from GS":"unknown until the next SDK write")<<'\n';
    }
}
void installNativeScene(PS2Runtime& runtime,NativeSceneBackend*) {
    if(!backend)return;
    assetCache.retire();renderTextureCache.clear();resetNativeIm3D();trackedAlpha={};im3dSites=builtinIm3DSites;sceneRuntime=&runtime;
    // Exact entries only: these SDK routines share generated functions with
    // neighbors such as the state getter at 0x1f5b00.
    auto entry=[&](uint32_t pc,PS2Runtime::RecompiledFunction& original,PS2Runtime::RecompiledFunction wrapper) {
        original=runtime.lookupFunction(pc);
        if(!original || !runtime.replaceFunction(pc,wrapper))throw std::runtime_error("Cannot install native blend tracking");
    };
    entry(0x1f5208,blendSetterOriginal,trackBlendSetter);entry(0x1fc0c8,blendInitOriginal,trackBlendInit);
    for(size_t i=0;i<blendReemitEntries.size();++i)entry(blendReemitEntries[i],blendReemitOriginal[i],trackBlendReemit);
    entry(0x1f5e70,blendRawOriginal,trackBlendRaw);
    for(size_t i=0;i<blendConstantWriters.size();++i)entry(blendConstantWriters[i].entry,blendConstantOriginal[i],trackBlendConstant);
    std::cout<<"[native-scene] tracking PS2 ALPHA_1 at the SDK setter, initializer, raw state, "<<blendReemitEntries.size()
        <<" re-emitters and "<<blendConstantWriters.size()<<" constant writers\n";
    // A scene unload/reload can reuse exactly the same raster, pixel, geometry
    // and mesh serial addresses for unrelated assets. Keep these lifecycle
    // boundaries on the CPU; old queued GIF markers retain their own snapshots.
    sceneExitOriginal=runtime.lookupFunction(0x135970);
    for(uint32_t pc=0x135970;pc<0x135b8c;pc+=4)if(runtime.hasFunction(pc) && runtime.lookupFunction(pc)==sceneExitOriginal)runtime.replaceFunction(pc,sceneExit);
    sceneInitOriginal=runtime.lookupFunction(0x135b90);
    for(uint32_t pc=0x135b90;pc<0x1360c0;pc+=4)if(runtime.hasFunction(pc) && runtime.lookupFunction(pc)==sceneInitOriginal)runtime.replaceFunction(pc,sceneInit);
    if(std::getenv("NIGHT100_TRACE_RESOURCE")) {
        auto hook=[&](uint32_t start,uint32_t end,PS2Runtime::RecompiledFunction& original,PS2Runtime::RecompiledFunction wrapper) {
            original=runtime.lookupFunction(start);
            for(uint32_t pc=start;pc<end;pc+=4)if(runtime.hasFunction(pc) && runtime.lookupFunction(pc)==original)runtime.replaceFunction(pc,wrapper);
        };
        hook(0x239508,0x2395c8,resourceDestroyOriginal,traceResourceDestroy);
        hook(0x241b08,0x241b48,resourceWaitOriginal,traceResourceWait);
        hook(0x249390,0x2493d0,resourcePluginDestroyOriginal,traceResourcePluginDestroy);
    }
    if(runtime.hasFunction(nativeFrameReturnPC))throw std::runtime_error("Native frame continuation address is occupied");
    if(!runtime.registerFunction(nativeFrameReturnPC,finishCompletedFrame))throw std::runtime_error("Cannot register native frame continuation");
    if(runtime.hasFunction(nativeReturnPC))throw std::runtime_error("Native graphics continuation address is occupied");
    if(!runtime.registerFunction(nativeReturnPC,finishNativeMarker))throw std::runtime_error("Cannot register native graphics continuation");
    original=runtime.lookupFunction(0x2250c0);
    for(uint32_t pc=0x2250c0;pc<0x225110;pc+=4)if(runtime.hasFunction(pc) && runtime.lookupFunction(pc)==original)runtime.replaceFunction(pc,render);
    cameraFovOriginal=runtime.lookupFunction(0x138b40);
    if(cameraFovOriginal)for(uint32_t pc=0x138b40;pc<0x138bd8;pc+=4)if(runtime.hasFunction(pc) && runtime.lookupFunction(pc)==cameraFovOriginal)runtime.replaceFunction(pc,setCameraFov);
    viewWindowOriginal=runtime.lookupFunction(0x1e6240);
    if(viewWindowOriginal)for(uint32_t pc=0x1e6240;pc<0x1e62a8;pc+=4)if(runtime.hasFunction(pc) && runtime.lookupFunction(pc)==viewWindowOriginal)runtime.replaceFunction(pc,setViewWindow);
    hud2DOriginal=runtime.lookupFunction(0x122540);
    if(hud2DOriginal)for(uint32_t pc=0x122540;pc<0x12296c;pc+=4)if(runtime.hasFunction(pc) && runtime.lookupFunction(pc)==hud2DOriginal)runtime.replaceFunction(pc,renderHud2D);
    std::cout<<"[native-scene] using cached GPU meshes and shader skinning\n";
    showOriginal=runtime.lookupFunction(0x1eb4a8);
    for(uint32_t pc=0x1eb4a8;pc<0x1eb510;pc+=4)if(runtime.hasFunction(pc) && runtime.lookupFunction(pc)==showOriginal)runtime.replaceFunction(pc,presentCompletedFrame);
    movieOriginal=runtime.lookupFunction(0x270460);
    for(uint32_t pc=0x270460;pc<0x270518;pc+=4)if(runtime.hasFunction(pc) && runtime.lookupFunction(pc)==movieOriginal)runtime.replaceFunction(pc,presentMovie);
    {
        queueWaitOriginal=runtime.lookupFunction(0x1f4da8);
        for(uint32_t pc=0x1f4da8;pc<0x1f4e98;pc+=4)if(runtime.hasFunction(pc) && runtime.lookupFunction(pc)==queueWaitOriginal)runtime.replaceFunction(pc,waitGraphicsQueue);
    }
    {
        worldOriginal=runtime.lookupFunction(0x22f848);
        for(uint32_t pc=0x22f848;pc<0x22f958;pc+=4)if(runtime.hasFunction(pc) && runtime.lookupFunction(pc)==worldOriginal)runtime.replaceFunction(pc,renderWorld);
    }
    groundCall.reset();
    groundOriginal=runtime.lookupFunction(groundEntry);
    if(!groundOriginal || runtime.hasFunction(groundContinuationPC) || !runtime.registerFunction(groundContinuationPC,continueGround)
        || !runtime.replaceFunction(groundEntry,renderGround))
        throw std::runtime_error("Cannot install native Ground particles");
    {
        particlesOriginal=runtime.lookupFunction(0x1b0740);
        for(uint32_t pc=0x1b0740;pc<0x1b07b8;pc+=4)if(runtime.hasFunction(pc) && runtime.lookupFunction(pc)==particlesOriginal)runtime.replaceFunction(pc,renderParticles);
    }
    if(nativeRenderTexturesEnabled()) {
        auto hook=[&](uint32_t begin,uint32_t end,PS2Runtime::RecompiledFunction& original,PS2Runtime::RecompiledFunction wrapper) {
            original=runtime.lookupFunction(begin);
            for(uint32_t pc=begin;pc<end;pc+=4)
                if(runtime.hasFunction(pc) && runtime.lookupFunction(pc)==original)runtime.replaceFunction(pc,wrapper);
        };
        hook(0x209318,0x2093c0,im3dTransformOriginal,transformNativeIm3D);
        hook(0x208910,0x208a98,im3dRenderOriginal,renderNativeIm3D);
        // The generated function contains several SDK routines after End's
        // actual return. Leave those later aliases on their original function.
        hook(0x2093c0,0x20940c,im3dEndOriginal,endNativeIm3D);
    }
    {
        im2dOriginal=runtime.lookupFunction(0x205548);
        for(uint32_t pc=0x205548;pc<0x2055b8;pc+=4)if(runtime.hasFunction(pc) && runtime.lookupFunction(pc)==im2dOriginal)runtime.replaceFunction(pc,traceIm2D);
        im2dIndexedOriginal=runtime.lookupFunction(0x2055b8);
        for(uint32_t pc=0x2055b8;pc<0x205620;pc+=4)if(runtime.hasFunction(pc) && runtime.lookupFunction(pc)==im2dIndexedOriginal)runtime.replaceFunction(pc,traceIm2D);
    }
}
