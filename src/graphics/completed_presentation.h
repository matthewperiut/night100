#pragma once
#include "runtime/gs/gs_types.h"
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <memory>
#include <mutex>

// A ring slot remains immutable while a frontend/window/diagnostic lease owns
// it. Its retained context and lock also outlive the raster backend if needed.
class CompletedPresentationImage final : public GSGpuPresentation {
public:
    CompletedPresentationImage(CGLContextObj,std::shared_ptr<std::mutex>);
    ~CompletedPresentationImage() override;
    uint32_t textureId() const override{return texture;}
    uint32_t textureWidth() const override{return 640;}
    uint32_t textureHeight() const override{return 512;}
    void waitForDraw() const override;
    void finishDraw() const override;
    bool readPixels(std::vector<uint8_t>&) const override;
    GLuint framebuffer() const{return fbo;}
    // Called only in the private context, with its shared mutex held and no
    // external leases. Cross-context reuse waits happen on the GPU server.
    void beginWrite();
    void endWrite();
private:
    CGLContextObj context;
    std::shared_ptr<std::mutex> contextMutex;
    mutable std::mutex fenceMutex;
    GLuint texture=0,fbo=0;
    GLsync producerFence=nullptr;
    mutable GLsync consumerFence=nullptr;
};
