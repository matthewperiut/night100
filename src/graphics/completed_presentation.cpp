#include "completed_presentation.h"
#include <stdexcept>

namespace {
struct Current {
    CGLContextObj previous;
    explicit Current(CGLContextObj context):previous(CGLGetCurrentContext()) {
        if(previous!=context && CGLSetCurrentContext(context)!=kCGLNoError)
            throw std::runtime_error("Cannot bind completed presentation context");
    }
    ~Current(){if(CGLGetCurrentContext()!=previous)CGLSetCurrentContext(previous);}
};
}

CompletedPresentationImage::CompletedPresentationImage(CGLContextObj ctx,std::shared_ptr<std::mutex> mutex)
    :context(CGLRetainContext(ctx)),contextMutex(std::move(mutex)) {
    try {
    glGenTextures(1,&texture);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,texture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,640,512,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Incomplete completed presentation framebuffer");
    } catch(...) {
        glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&texture);CGLReleaseContext(context);throw;
    }
}
CompletedPresentationImage::~CompletedPresentationImage() {
    {
        std::lock_guard lock(*contextMutex);Current bind(context);
        if(producerFence)glDeleteSync(producerFence);
        if(consumerFence)glDeleteSync(consumerFence);
        glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&texture);
    }
    CGLReleaseContext(context);
}
void CompletedPresentationImage::beginWrite() {
    std::lock_guard lock(fenceMutex);
    if(consumerFence){glWaitSync(consumerFence,0,GL_TIMEOUT_IGNORED);glDeleteSync(consumerFence);consumerFence=nullptr;}
    if(producerFence){glDeleteSync(producerFence);producerFence=nullptr;}
}
void CompletedPresentationImage::endWrite() {
    producerFence=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
    if(!producerFence)throw std::runtime_error("Cannot fence completed presentation");
    glFlush();
}
void CompletedPresentationImage::waitForDraw() const {
    if(producerFence)glWaitSync(producerFence,0,GL_TIMEOUT_IGNORED);
}
void CompletedPresentationImage::finishDraw() const {
    std::lock_guard lock(fenceMutex);
    const auto fence=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
    if(!fence)throw std::runtime_error("Cannot fence presentation consumer");
    if(consumerFence)glDeleteSync(consumerFence);
    consumerFence=fence;glFlush();
}
bool CompletedPresentationImage::readPixels(std::vector<uint8_t>& out) const {
    std::lock_guard lock(*contextMutex);Current bind(context);
    if(producerFence)glWaitSync(producerFence,0,GL_TIMEOUT_IGNORED);
    out.resize(640u*512u*4u);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,fbo);glPixelStorei(GL_PACK_ALIGNMENT,4);
    glReadPixels(0,0,640,512,GL_RGBA,GL_UNSIGNED_BYTE,out.data());
    return glGetError()==GL_NO_ERROR;
}
