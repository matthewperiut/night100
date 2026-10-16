#include "native_renderer.h"
#include "gs_shaders.h"
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

namespace {
bool profileNativeStalls() {
    static const bool enabled=std::getenv("NIGHT100_PROFILE_NATIVE_STALLS")!=nullptr;
    return enabled;
}
struct NativeCostStamp {
    std::chrono::steady_clock::time_point wall;
    double cpu=0;
    static NativeCostStamp now() {
        timespec t{};clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);
        return {std::chrono::steady_clock::now(),t.tv_sec+t.tv_nsec*1e-9};
    }
    std::array<double,2> milliseconds() const {
        const auto end=now();
        return {std::chrono::duration<double,std::milli>(end.wall-wall).count(),(end.cpu-cpu)*1000};
    }
};
GLuint compile(GLenum type,const char* source) {
    GLuint id=glCreateShader(type);glShaderSource(id,1,&source,nullptr);glCompileShader(id);
    GLint okay=0;glGetShaderiv(id,GL_COMPILE_STATUS,&okay);
    if(!okay){char error[8192]{};glGetShaderInfoLog(id,sizeof(error),nullptr,error);glDeleteShader(id);throw std::runtime_error(error);}
    return id;
}
GLuint program(const char* vertex,const char* fragment) {
    const auto v=compile(GL_VERTEX_SHADER,vertex),f=compile(GL_FRAGMENT_SHADER,fragment);
    GLuint result=glCreateProgram();glAttachShader(result,v);glAttachShader(result,f);glLinkProgram(result);glDeleteShader(v);glDeleteShader(f);
    GLint okay=0;glGetProgramiv(result,GL_LINK_STATUS,&okay);
    if(!okay){char error[8192]{};glGetProgramInfoLog(result,sizeof(error),nullptr,error);glDeleteProgram(result);throw std::runtime_error(error);}
    return result;
}
GLint wrap(uint32_t value){return value==2?GL_MIRRORED_REPEAT:value>=3?GL_CLAMP_TO_EDGE:GL_REPEAT;}
void renderTextureSampling(const NativeTexture& texture) {
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,texture.linear?GL_LINEAR:GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,texture.linear?GL_LINEAR:GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,wrap(texture.wrapU));glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,wrap(texture.wrapV));
}
GLenum blend(uint32_t value) {
    constexpr GLenum factors[]={GL_ZERO,GL_ZERO,GL_ONE,GL_SRC_COLOR,GL_ONE_MINUS_SRC_COLOR,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_DST_ALPHA,GL_ONE_MINUS_DST_ALPHA,GL_DST_COLOR,GL_ONE_MINUS_DST_COLOR,GL_SRC_ALPHA_SATURATE};
    return factors[std::min(value,11u)];
}

// These programs are private to NativeRenderer and never relinked. Uniform
// values survive other programs being used, unlike context-wide GL bindings.
template<typename T,size_t N> struct UniformValue {
    GLint location=-1;
    std::array<T,N> previous{};
    bool initialized=false;
    bool changed(const T* value,bool cache) {
        if(!cache)return true;
        if(initialized && std::memcmp(previous.data(),value,sizeof(previous))==0)return false;
        std::memcpy(previous.data(),value,sizeof(previous));initialized=true;return true;
    }
    void locate(GLuint shader,const char* name){location=glGetUniformLocation(shader,name);}
};
template<typename T> struct ScalarUniform : UniformValue<T,1> {
    void set(T value,bool cache) {
        if(!this->changed(&value,cache))return;
        if constexpr(std::is_same_v<T,float>)glUniform1f(this->location,value);
        else if constexpr(std::is_same_v<T,GLint>)glUniform1i(this->location,value);
        else glUniform1ui(this->location,value);
    }
};
template<size_t N,bool Matrix=false> struct FloatUniform : UniformValue<float,N> {
    void set(const float* value,bool cache) {
        if(!this->changed(value,cache))return;
        if constexpr(Matrix)glUniformMatrix4fv(this->location,1,GL_FALSE,value);
        else if constexpr(N==2)glUniform2fv(this->location,1,value);
        else glUniform4fv(this->location,N/4,value);
    }
    void set(float first,float second,bool cache) requires(N==2) {
        const float value[]={first,second};set(value,cache);
    }
};
}
struct NativeRenderer::Impl {
    GLuint shader=0,screenShader=0,screenVao=0,screenVbo=0,screenIbo=0,white=0,seed=0,resolveShader=0,fullscreen=0,fbo=0,color=0,depth=0,pose=0;
    uint32_t width=0,height=0;
    // Main and shadow passes alternate dimensions. Keep their storage instead
    // of orphaning both textures at every switch. Contents are always seeded
    // from the authoritative integer surface before drawing.
    struct Framebuffer {GLuint fbo=0,color=0,depth=0;uint32_t width=0,height=0;uint64_t used=0;};
    std::array<Framebuffer,2> framebuffers{};
    uint64_t framebufferClock=0;
    static void destroy(Framebuffer& target) {
        glDeleteTextures(1,&target.color);glDeleteTextures(1,&target.depth);glDeleteFramebuffers(1,&target.fbo);
        target={};
    }
    struct AssetOwner {
        std::weak_ptr<const NativeAssetLifetime> lifetime;
        bool tracked=false;
        void follow(const std::shared_ptr<const NativeAssetLifetime>& owner){lifetime=owner;tracked=bool(owner);}
        bool retired() const{return tracked && lifetime.expired();}
    };
    struct Geometry : AssetOwner {GLuint vao=0,vbo=0,ibo=0,inverse=0;uint64_t revision=UINT64_MAX;};
    struct Texture : AssetOwner {GLuint id=0;};
    std::unordered_map<uint64_t,Geometry> geometries;
    std::unordered_map<uint64_t,Texture> textures;
    const bool cacheUniforms=std::getenv("NIGHT100_REFERENCE_UNIFORMS")==nullptr;
    struct ModelUniforms {
        FloatUniform<16,true> world,camera;
        ScalarUniform<GLint> boneCount,lightCount,textured,fogEnabled,projectionType;
        ScalarUniform<GLuint> geometryFlags;
        ScalarUniform<float> alphaReference;
        FloatUniform<2> surfaceProperties,depthMapping,fogRange;
        FloatUniform<4> materialColor,ambientLight,fogColor;
        FloatUniform<32> lightColor,lightDirection;
        void locate(GLuint shader) {
            world.locate(shader,"worldMatrix");camera.locate(shader,"cameraMatrix");
            boneCount.locate(shader,"boneCount");geometryFlags.locate(shader,"geometryFlags");
            materialColor.locate(shader,"materialColor");surfaceProperties.locate(shader,"surfaceProperties");
            depthMapping.locate(shader,"depthMapping");ambientLight.locate(shader,"ambientLight");
            lightCount.locate(shader,"lightCount");lightColor.locate(shader,"lightColor[0]");
            lightDirection.locate(shader,"lightDirection[0]");textured.locate(shader,"textured");
            alphaReference.locate(shader,"alphaReference");fogEnabled.locate(shader,"fogEnabled");
            fogColor.locate(shader,"fogColor");fogRange.locate(shader,"fogRange");projectionType.locate(shader,"projectionType");
        }
    } modelUniforms;
    struct ScreenUniforms {
        ScalarUniform<GLint> fogEnabled,textured;
        FloatUniform<2> viewport;
        void locate(GLuint shader) {
            fogEnabled.locate(shader,"fogEnabled");textured.locate(shader,"textured");viewport.locate(shader,"viewportSize");
        }
    } screenUniforms;
    ScalarUniform<GLuint> seedFormat,resolveColorFormat,resolveDepthFormat;
    unsigned drawsUntilRetirement=0;
    static void destroy(Geometry& g) {
        glDeleteVertexArrays(1,&g.vao);glDeleteBuffers(1,&g.vbo);glDeleteBuffers(1,&g.ibo);glDeleteBuffers(1,&g.inverse);
    }
    void retireUnusedAssets() {
        // Amortize lookup scans. This executes only with the ordered draw's GL
        // context current. GL retains already submitted buffer/texture stores;
        // queued host commands keep their epoch alive until they are consumed.
        if(drawsUntilRetirement){--drawsUntilRetirement;return;}
        drawsUntilRetirement=63;
        for(auto it=geometries.begin();it!=geometries.end();) {
            if(it->second.retired()){destroy(it->second);it=geometries.erase(it);}else ++it;
        }
        for(auto it=textures.begin();it!=textures.end();) {
            if(it->second.retired()){glDeleteTextures(1,&it->second.id);it=textures.erase(it);}else ++it;
        }
    }
    Impl() {
        shader=program(nativeVertexShader,nativeFragmentShader);
        screenShader=program(nativeScreenShader,nativeFragmentShader);
        glGenTextures(1,&white);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,white);
        const uint32_t opaqueWhite=0xffffffff;glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,&opaqueWhite);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glGenVertexArrays(1,&screenVao);glGenBuffers(1,&screenVbo);glBindVertexArray(screenVao);glBindBuffer(GL_ARRAY_BUFFER,screenVbo);
        glGenBuffers(1,&screenIbo);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,screenIbo);
        for(unsigned i=0;i<3;++i){glEnableVertexAttribArray(i);glVertexAttribPointer(i,4,GL_FLOAT,GL_FALSE,sizeof(NativeScreenVertex),reinterpret_cast<void*>(size_t(i)*16));}
        seed=program(gsVertexShader,nativeSeedShader);resolveShader=program(gsVertexShader,nativeResolveShader);
        glGenVertexArrays(1,&fullscreen);glGenBuffers(1,&pose);
        glBindBuffer(GL_UNIFORM_BUFFER,pose);glBufferData(GL_UNIFORM_BUFFER,sizeof(NativeMatrix)*64,nullptr,GL_STREAM_DRAW);
        glUseProgram(shader);glUniform1i(glGetUniformLocation(shader,"diffuseTexture"),0);
        glUniform1f(glGetUniformLocation(shader,"alphaReference"),0.5f);
        glUseProgram(screenShader);glUniform1i(glGetUniformLocation(screenShader,"diffuseTexture"),0);
        glUniform1f(glGetUniformLocation(screenShader,"alphaReference"),0.0f);
        modelUniforms.locate(shader);screenUniforms.locate(screenShader);
        seedFormat.locate(seed,"frameFormat");resolveColorFormat.locate(resolveShader,"frameFormat");resolveDepthFormat.locate(resolveShader,"depthFormat");
        for(auto [name,binding]:{std::pair{"InverseBind",1u},std::pair{"BonePose",2u}}) {
            const auto index=glGetUniformBlockIndex(shader,name);
            if(index==GL_INVALID_INDEX)throw std::runtime_error("Missing native bone uniform block");
            GLint bytes=0;glGetActiveUniformBlockiv(shader,index,GL_UNIFORM_BLOCK_DATA_SIZE,&bytes);
            if(bytes!=sizeof(NativeMatrix)*64)throw std::runtime_error("Native matrix palette layout mismatch");
            glUniformBlockBinding(shader,index,binding);
        }
        glUseProgram(seed);glUniform1i(glGetUniformLocation(seed,"integerSurface"),0);
        glUseProgram(resolveShader);glUniform1i(glGetUniformLocation(resolveShader,"nativeColor"),0);glUniform1i(glGetUniformLocation(resolveShader,"nativeDepth"),1);
    }
    ~Impl() {
        for(auto& [id,g]:geometries)destroy(g);
        for(auto& [id,t]:textures)glDeleteTextures(1,&t.id);
        for(auto& target:framebuffers)destroy(target);
        glDeleteBuffers(1,&pose);glDeleteVertexArrays(1,&fullscreen);
        glDeleteProgram(shader);glDeleteProgram(seed);glDeleteProgram(resolveShader);
        glDeleteProgram(screenShader);glDeleteVertexArrays(1,&screenVao);glDeleteBuffers(1,&screenVbo);glDeleteBuffers(1,&screenIbo);
        glDeleteTextures(1,&white);
    }
    Geometry& upload(const NativeGeometry& model) {
        auto& g=geometries[model.id];
        if(g.revision==model.revision)return g;
        g.follow(model.lifetime);
        const bool firstUpload=!g.vao;
        if(!g.vao){glGenVertexArrays(1,&g.vao);glGenBuffers(1,&g.vbo);glGenBuffers(1,&g.ibo);glGenBuffers(1,&g.inverse);}
        glBindVertexArray(g.vao);glBindBuffer(GL_ARRAY_BUFFER,g.vbo);
        glBufferData(GL_ARRAY_BUFFER,model.vertices.size()*sizeof(NativeVertex),model.vertices.data(),model.dynamic?GL_STREAM_DRAW:GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,g.ibo);glBufferData(GL_ELEMENT_ARRAY_BUFFER,model.indices.size()*sizeof(uint16_t),model.indices.data(),model.dynamic?GL_STREAM_DRAW:GL_STATIC_DRAW);
        auto attrib=[](unsigned index,int count,GLenum type,bool normalized,size_t offset){glEnableVertexAttribArray(index);glVertexAttribPointer(index,count,type,normalized,sizeof(NativeVertex),reinterpret_cast<void*>(offset));};
        attrib(0,3,GL_FLOAT,false,offsetof(NativeVertex,position));attrib(1,3,GL_FLOAT,false,offsetof(NativeVertex,normal));attrib(2,2,GL_FLOAT,false,offsetof(NativeVertex,uv));
        attrib(3,4,GL_UNSIGNED_BYTE,true,offsetof(NativeVertex,color));attrib(4,4,GL_FLOAT,false,offsetof(NativeVertex,weights));
        glEnableVertexAttribArray(5);glVertexAttribIPointer(5,4,GL_UNSIGNED_BYTE,sizeof(NativeVertex),reinterpret_cast<void*>(offsetof(NativeVertex,bones)));
        // Boneless particle revisions reuse the already allocated inverse
        // block; uploading an unused 4 KiB palette per batch wastes bandwidth.
        if(model.boneCount || firstUpload) {
            glBindBuffer(GL_UNIFORM_BUFFER,g.inverse);glBufferData(GL_UNIFORM_BUFFER,sizeof(model.inverseBind),model.inverseBind.data(),GL_STATIC_DRAW);
        }
        g.revision=model.revision;return g;
    }
    GLuint upload(const NativeTexture& data) {
        if(data.renderTarget)throw std::runtime_error("Render-target texture requires an ordered GPU binding");
        if(auto it=textures.find(data.id);it!=textures.end())return it->second.id;
        const bool profiling=profileNativeStalls();
        const auto stamp=profiling?NativeCostStamp::now():NativeCostStamp{};
        GLuint id=0;glGenTextures(1,&id);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,id);
        glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,data.width,data.height,0,GL_RGBA,GL_UNSIGNED_BYTE,data.rgba.data());
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,data.linear?GL_LINEAR:GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,data.linear?GL_LINEAR:GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,wrap(data.wrapU));glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,wrap(data.wrapV));
        Texture resource;resource.id=id;resource.follow(data.lifetime);textures.emplace(data.id,std::move(resource));
        if(profiling) {
            const auto ms=stamp.milliseconds();
            std::cerr<<"[native-stall] texture-upload id="<<data.id<<" size="<<data.width<<'x'<<data.height
                <<" wall_ms="<<ms[0]<<" cpu_ms="<<ms[1]<<'\n';
        }
        return id;
    }
};
NativeRenderer::NativeRenderer():impl(std::make_unique<Impl>()){}
NativeRenderer::~NativeRenderer()=default;
size_t NativeRenderer::geometryResourceCount() const { return impl->geometries.size(); }
size_t NativeRenderer::textureResourceCount() const { return impl->textures.size(); }
void NativeRenderer::begin(unsigned width,unsigned height,unsigned integerSurface,unsigned frameFormat) {
    auto& g=*impl;
    auto target=std::find_if(g.framebuffers.begin(),g.framebuffers.end(),[&](const auto& t){return t.fbo && t.width==width && t.height==height;});
    if(target==g.framebuffers.end()) {
        target=std::min_element(g.framebuffers.begin(),g.framebuffers.end(),[](const auto& a,const auto& b){return a.used<b.used;});
        Impl::destroy(*target);
        target->width=width;target->height=height;
        glGenFramebuffers(1,&target->fbo);glGenTextures(1,&target->color);glGenTextures(1,&target->depth);
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,target->color);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D,target->depth);glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT32F,width,height,0,GL_DEPTH_COMPONENT,GL_FLOAT,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_COMPARE_MODE,GL_NONE);
        glBindFramebuffer(GL_FRAMEBUFFER,target->fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target->color,0);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,target->depth,0);
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)throw std::runtime_error("Incomplete native framebuffer");
    }
    target->used=++g.framebufferClock;
    g.fbo=target->fbo;g.color=target->color;g.depth=target->depth;g.width=width;g.height=height;
    glBindFramebuffer(GL_FRAMEBUFFER,g.fbo);glViewport(0,0,width,height);glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);glDepthFunc(GL_ALWAYS);glDepthMask(GL_TRUE);
    glUseProgram(g.seed);g.seedFormat.set(frameFormat,g.cacheUniforms);glBindVertexArray(g.fullscreen);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,integerSurface);glDrawArrays(GL_TRIANGLES,0,3);
}
void NativeRenderer::draw(const NativeModelDraw& draw,std::span<const unsigned> renderTextures) {
    const bool profiling=profileNativeStalls();
    const auto stamp=profiling?NativeCostStamp::now():NativeCostStamp{};
    auto& g=*impl;g.retireUnusedAssets();const auto& model=*draw.geometry;auto& mesh=g.upload(model);
    glUseProgram(g.shader);glBindVertexArray(mesh.vao);
    glBindFramebuffer(GL_FRAMEBUFFER,g.fbo);glViewport(0,0,g.width,g.height);glDisable(GL_SCISSOR_TEST);glDisable(GL_CULL_FACE);
    if(draw.depthTest || draw.depthWrite)glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);
    glDepthFunc(draw.depthTest?GL_GEQUAL:GL_ALWAYS);glDepthMask(draw.depthWrite?GL_TRUE:GL_FALSE);
    if(draw.cullMode==2 || draw.cullMode==3){glEnable(GL_CULL_FACE);glFrontFace(GL_CCW);glCullFace(draw.cullMode==2?GL_BACK:GL_FRONT);}
    if(draw.vertexAlpha)glEnable(GL_BLEND);else glDisable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);glBlendFuncSeparate(blend(draw.sourceBlend),blend(draw.destinationBlend),GL_ONE,GL_ZERO);
    auto& u=g.modelUniforms;const bool cache=g.cacheUniforms;
    u.alphaReference.set(draw.alphaReference,cache);u.projectionType.set(draw.projectionType,cache);
    u.fogEnabled.set(draw.fogEnabled?1:0,cache);u.fogColor.set(draw.fogColor.data(),cache);
    u.fogRange.set(draw.fogStart,draw.fogEnd,cache);
    u.world.set(draw.world.data(),cache);u.camera.set(draw.camera.data(),cache);
    u.boneCount.set(model.boneCount,cache);u.geometryFlags.set(model.flags,cache);
    u.depthMapping.set(draw.zScale,draw.zShift,cache);
    u.ambientLight.set(draw.lights.ambient.data(),cache);u.lightCount.set(draw.lights.count,cache);
    u.lightColor.set(draw.lights.color[0].data(),cache);u.lightDirection.set(draw.lights.direction[0].data(),cache);
    glBindBufferBase(GL_UNIFORM_BUFFER,1,mesh.inverse);glBindBuffer(GL_UNIFORM_BUFFER,g.pose);
    if(model.boneCount)glBufferData(GL_UNIFORM_BUFFER,sizeof(draw.pose),draw.pose.data(),GL_STREAM_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER,2,g.pose);
    for(size_t i=0;i<model.meshes.size();++i) {
        const auto& sub=model.meshes[i];const auto& material=draw.materials[i];
        u.materialColor.set(material.color.data(),cache);u.surfaceProperties.set(material.ambient,material.diffuse,cache);
        u.textured.set(material.texture?1:0,cache);
        const GLuint renderTexture=i<renderTextures.size()?renderTextures[i]:0;
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,renderTexture?renderTexture:material.texture?g.upload(*material.texture):g.white);
        if(renderTexture)renderTextureSampling(*material.texture);
        const auto drawStamp=profiling?NativeCostStamp::now():NativeCostStamp{};
        if(draw.indexed)glDrawElements((model.flags&1)?GL_TRIANGLE_STRIP:GL_TRIANGLES,sub.indexCount,GL_UNSIGNED_SHORT,reinterpret_cast<void*>(size_t(sub.firstIndex)*sizeof(uint16_t)));
        else glDrawArrays((model.flags&1)?GL_TRIANGLE_STRIP:GL_TRIANGLES,sub.firstIndex,sub.indexCount);
        if(profiling) {
            const auto ms=drawStamp.milliseconds();
            if(ms[0]>2 || ms[1]>2)std::cerr<<"[native-stall] driver-draw geometry="<<model.id
                <<" vertices="<<model.vertices.size()<<" count="<<sub.indexCount
                <<" blend="<<draw.sourceBlend<<'/'<<draw.destinationBlend
                <<" depth="<<draw.depthTest<<'/'<<draw.depthWrite<<" wall_ms="<<ms[0]<<" cpu_ms="<<ms[1]<<'\n';
        }
    }
    if(profiling) {
        const auto ms=stamp.milliseconds();
        if(ms[0]>2 || ms[1]>2)std::cerr<<"[native-stall] native-draw geometry="<<model.id
            <<" vertices="<<model.vertices.size()<<" revision="<<model.revision
            <<" wall_ms="<<ms[0]<<" cpu_ms="<<ms[1]<<'\n';
    }
}
void NativeRenderer::draw(const NativeScreenDraw& draw,unsigned renderTexture) {
    auto& g=*impl;g.retireUnusedAssets();
    glBindFramebuffer(GL_FRAMEBUFFER,g.fbo);glViewport(0,0,g.width,g.height);glDisable(GL_SCISSOR_TEST);glDisable(GL_CULL_FACE);
    if(draw.depthTest || draw.depthWrite)glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);
    glDepthFunc(draw.depthTest?GL_GEQUAL:GL_ALWAYS);glDepthMask(draw.depthWrite?GL_TRUE:GL_FALSE);
    if(draw.vertexAlpha)glEnable(GL_BLEND);else glDisable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);glBlendFuncSeparate(blend(draw.sourceBlend),blend(draw.destinationBlend),GL_ONE,GL_ZERO);
    glUseProgram(g.screenShader);g.screenUniforms.fogEnabled.set(0,g.cacheUniforms);glBindVertexArray(g.screenVao);glBindBuffer(GL_ARRAY_BUFFER,g.screenVbo);
    glBufferData(GL_ARRAY_BUFFER,draw.vertices.size()*sizeof(NativeScreenVertex),draw.vertices.data(),GL_STREAM_DRAW);
    g.screenUniforms.viewport.set(g.width,g.height,g.cacheUniforms);
    g.screenUniforms.textured.set(draw.texture?1:0,g.cacheUniforms);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,renderTexture?renderTexture:draw.texture?g.upload(*draw.texture):g.white);
    if(renderTexture)renderTextureSampling(*draw.texture);
    constexpr GLenum primitives[]={GL_POINTS,GL_LINES,GL_LINE_STRIP,GL_TRIANGLES,GL_TRIANGLE_STRIP,GL_TRIANGLE_FAN};
    if(draw.indices.empty())glDrawArrays(primitives[std::min(draw.primitive,5u)],0,draw.vertices.size());
    else {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,g.screenIbo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,draw.indices.size()*sizeof(uint16_t),draw.indices.data(),GL_STREAM_DRAW);
        glDrawElements(primitives[std::min(draw.primitive,5u)],draw.indices.size(),GL_UNSIGNED_SHORT,nullptr);
    }
}
void NativeRenderer::resolve(unsigned framebuffer,unsigned frameFormat,unsigned depthFormat) {
    auto& g=*impl;
    glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);glViewport(0,0,g.width,g.height);glDisable(GL_BLEND);glDisable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);glDisable(GL_SCISSOR_TEST);
    glUseProgram(g.resolveShader);glBindVertexArray(g.fullscreen);
    g.resolveColorFormat.set(frameFormat,g.cacheUniforms);
    g.resolveDepthFormat.set(depthFormat,g.cacheUniforms);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,g.color);glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,g.depth);
    glDrawArrays(GL_TRIANGLES,0,3);
}
