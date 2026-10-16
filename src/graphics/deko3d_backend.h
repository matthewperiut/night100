#pragma once
#include "runtime/gs/gs_backend.h"
#include "native_scene_backend.h"
#include <memory>
#include <string>
namespace SwitchVideo {struct Frame;}

// Switch GPU adapter. By default all commands execute on the guest's ordered GIF
// producer. NIGHT100_RENDER_THREAD=1 moves them onto one render thread, in issue
// order. Completed frames present directly
// through deko3d, without a CPU pixel roundtrip.
class Deko3DBackend final : public GSRasterBackend, public NativeSceneBackend {
public:
    explicit Deko3DBackend(std::string shaderDirectory="romfs:/shaders");
    ~Deko3DBackend() override;
    void Initialize(uint8_t*,uint32_t) override;
    void Reset() override;
    void prepareStateSave() override;
    void saveState(ps2x::state::Writer&) override;
    void loadState(ps2x::state::Reader&) override;
    void Submit(const GSPrimitiveBatch&) override;
    void LoadClut(const GSTex0Reg&,const GSTexClutReg&) override;
    void BeginTransfer(const GSTransferCommand&) override;
    void UploadImage(const uint8_t*,uint32_t) override;
    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason) override;
    PresentationFrame Present(const GSPresentationRequest&) override;
    PresentationFrame PresentCompleted(const GSPresentationRequest&,bool=false) override;
    bool ClearFramebuffer(const GSContext&,uint32_t) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t*,uint32_t) override;
    uint32_t ReadVram(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t) const override;
    void WriteVram(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t) override;
    void SnapshotVram(std::vector<uint8_t>&) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;
    uint64_t QueueNativeModel(NativeModelDraw) override;
    uint64_t QueueNativeScreen(NativeScreenDraw) override;
    uint64_t RegisterNativeRenderTexture(const GSTex0Reg&,const GSTexaReg&,
        const std::shared_ptr<const NativeAssetLifetime>&) override;
    void NativeCommand(uint64_t,const GSContext&) override;
    void finishTimingInterval() override;
    // Switch widescreen. The aspect is fixed at startup; the gate follows the game.
    void setWideScreen(bool enabled) noexcept;
    // Internal resolution of the main (fbw 10) frames: Half (320x224), Native (640x448)
    // or Double (the 1280x720 display size). Switching rebuilds the main targets from
    // canonical GS memory at the next use.
    enum class InternalResolution{Half,Native,Double};
    void setInternalResolution(InternalResolution mode) noexcept;
    void setWorldFrameGate(bool enabled) noexcept override;
    // The guest camera's view window is widened by the scene hook, so world draws
    // arrive already wide and HUD model draws must be narrowed back.
    void setGuestWidensProjection(bool enabled) noexcept;
    // Diagnostic counters for the widescreen gate and GPU timestamps.
    struct Probes {
        uint64_t wideTargets=0,gateTokens=0,fullWidthModels=0;
        uint64_t timingReads=0,timingRejected=0,timingRenderOnly=0;
        uint64_t lastStart=0,lastComposition=0,lastEnd=0;
        bool wideConfig=false,worldGate=false;
    };
    Probes probes() const noexcept;
    struct Statistics {
        uint64_t gameFrames=0,modelDraws=0,screenDraws=0,gsPrimitives=0;
        uint64_t textureUploads=0,geometryUploads=0,gpuSubmissions=0,readbacks=0;
        uint64_t renderTextureCopies=0;
        uint64_t renderTextureImageAllocations=0,renderTextureImageReuses=0;
        uint64_t dynamicBufferAllocations=0,dynamicGeometryUploads=0,dynamicUploadBytes=0;
        // Timestamp counters from completed frames; the sums are nanoseconds and
        // the sample counts are completed game frames.
        uint64_t gpuNanoseconds=0,gpuSamples=0,compositionNanoseconds=0,compositionSamples=0;
        bool gpuTimingSupported=true,gpuTimingChecked=true,gpuTimingEnabled=true;
    };
    struct GpuTiming {
        uint64_t gpuNanoseconds=0,gpuSamples=0,compositionNanoseconds=0,compositionSamples=0;
    };
    // Lock-free, readable from the host poll thread. Lags by a few slot reuses.
    GpuTiming gpuTiming() const noexcept;
    Statistics statistics() const;
    enum class Stage : uint32_t {
        Idle,Initializing,FenceWait,QueueSubmit,SwapchainAcquire,SwapchainPresent,
        QueueIdle,CanonicalDecode,Readback,Reset,VideoImport,ResourceRetirement
    };
    struct Diagnostics {
        Stage stage=Stage::Idle;
        uint32_t slot=0;
        uint64_t operationSerial=0,submissions=0,gameFrames=0,readbacks=0;
        uint32_t textureCacheEntries=0,legacyCacheEntries=0,descriptorHighWater=0,descriptorPendingRetirement=0;
        uint64_t renderTextureImageAllocations=0,renderTextureImageReuses=0,renderTexturePoolBytes=0;
        uint32_t renderTexturePoolImages=0;
        uint64_t dynamicBufferAllocations=0,dynamicGeometryUploads=0,dynamicUploadBytes=0,dynamicBufferBytes=0;
        uint32_t dynamicBufferBlocks=0;
    };
    // Approximate progress snapshot for the host watchdog. Never takes the
    // graphics mutex or invokes the driver, including when its producer stalls.
    Diagnostics diagnostics() const noexcept;
    static const char* diagnosticStageName(Stage) noexcept;
    // The applet thread publishes activity; only the producer drains GPU work.
    // A null predicate keeps presentation enabled (including standalone probes).
    void setPresentationActivePredicate(bool(*)()) noexcept;
    void quiesce();
    struct MetricsOverlay {
        bool enabled=false;
        double gameFps=0,producerCpuMs=0,gpuMs=-1;
    };
    // Host telemetry may publish values without taking the graphics mutex.
    // Negative or nonfinite timing values display N/A. Disabled by default.
    void setMetricsOverlay(const MetricsOverlay&) noexcept;
    void setReplayOverlay(std::string text);
    // Producer-only: repaint while the replay picker has parked guest work.
    // Neither operation increments game frames; capture alone reads pixels.
    bool presentOverlay();
    bool copyCompletedFrameRgba(std::vector<uint8_t>&,uint32_t&,uint32_t&);
    std::string device() const;
    // Only for native video import; the caller must use the producer thread.
    void* deviceHandle();
    void* queueHandle();
    bool presentVideo(const SwitchVideo::Frame&);
    // Device-only fixtures use explicit readback; regular game/video paths do not.
    bool runSelfTest();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
