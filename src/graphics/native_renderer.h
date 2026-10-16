#pragma once
#include "native_model.h"
#include <memory>
#include <span>

// Called with the GS core context current. All mesh/texture resources are
// explicit GPU buffers; animation updates upload only the matrix palette.
class NativeRenderer {
public:
    NativeRenderer();
    ~NativeRenderer();
    void begin(unsigned width,unsigned height,unsigned integerSurface,unsigned frameFormat);
    void draw(const NativeModelDraw&,std::span<const unsigned> renderTextures={});
    void draw(const NativeScreenDraw&,unsigned renderTexture=0);
    void resolve(unsigned integerFramebuffer,unsigned frameFormat,unsigned depthFormat);
    // Diagnostic for regression checks: streaming particle revisions must
    // replace one resource entry rather than allocate one per draw.
    size_t geometryResourceCount() const;
    size_t textureResourceCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
