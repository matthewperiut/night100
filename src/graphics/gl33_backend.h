#pragma once
#include "runtime/gs/gs_backend.h"
#include "native_model.h"
#include "native_scene_backend.h"
#include <memory>
#include <string>

// GS draws run in a private core context. CPU memory-format/CLUT decoding is
// retained; coverage, filtering, fragment tests, blending and depth run on GPU.
class GL33Backend final : public GSRasterBackend, public NativeSceneBackend {
public:
    GL33Backend();
    ~GL33Backend() override;
    void Initialize(uint8_t*, uint32_t) override;
    void Reset() override;
    void prepareStateSave() override;
    void saveState(ps2x::state::Writer&) override;
    void loadState(ps2x::state::Reader&) override;

    void Submit(const GSPrimitiveBatch&) override;
    void LoadClut(const GSTex0Reg&, const GSTexClutReg&) override;
    void BeginTransfer(const GSTransferCommand&) override;
    void UploadImage(const uint8_t*, uint32_t) override;
    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason) override;
    PresentationFrame Present(const GSPresentationRequest&) override;
    PresentationFrame PresentCompleted(const GSPresentationRequest&,bool authoritativeDisplay=false) override;
    bool ClearFramebuffer(const GSContext&, uint32_t) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t*, uint32_t) override;
    uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const override;
    void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override;
    void SnapshotVram(std::vector<uint8_t>&) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;
    std::string device() const;
    uint64_t gpuDraws() const;
    bool DrawNativeModel(const GSContext&,const NativeModelDraw&);
    bool DrawNativeScreen(const GSContext&,const NativeScreenDraw&);
    uint64_t QueueNativeModel(NativeModelDraw) override;
    uint64_t QueueNativeScreen(NativeScreenDraw) override;
    // Guest GS layout is confined to this adapter. Native materials retain
    // only the opaque host source ID returned here (zero means unsupported).
    uint64_t RegisterNativeRenderTexture(const GSTex0Reg&,const GSTexaReg&,const std::shared_ptr<const NativeAssetLifetime>&) override;
    void NativeCommand(uint64_t,const GSContext&) override;
    struct Statistics {
        uint64_t primitives=0,submissions=0,surfaceCopies=0,surfaceReadbacks=0,textureUploads=0,nativeMeshes=0;
        uint64_t gpuNanoseconds=0,gpuSamples=0,nativeGeometryResources=0,nativeTextureResources=0;
        uint64_t completedGpuFrames=0,completedCpuFallbacks=0,completedRetainedFrames=0;
        uint64_t nativeRenderTextureCopies=0,nativeRenderTextureFallbacks=0,nativeRenderTextureResources=0;
        bool gpuTimingSupported=false,gpuTimingChecked=false,gpuTimingEnabled=false;
    };
    // Dynamic overlay telemetry; queries never wait for GPU completion.
    void setTimingEnabled(bool);
    // Submit the current elapsed interval before an intentional host wait.
    // Does not flush pending renderer batches or wait for query results.
    void finishTimingInterval() override;
    Statistics statistics() const;
    bool hasFailed() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
