#pragma once
#include "native_model.h"
#include "runtime/gs/gs_types.h"

// Native asset commands remain ordered by the guest GIF stream. Platform
// backends own their GPU resources, uploads and completed-frame presentation.
class NativeSceneBackend {
public:
    virtual ~NativeSceneBackend() = default;
    virtual uint64_t QueueNativeModel(NativeModelDraw) = 0;
    virtual uint64_t QueueNativeScreen(NativeScreenDraw) = 0;
    virtual uint64_t RegisterNativeRenderTexture(const GSTex0Reg&,const GSTexaReg&,
        const std::shared_ptr<const NativeAssetLifetime>&) = 0;
    virtual void finishTimingInterval() = 0;
    // Sampled once per scene render on the game thread. Wide-capable backends
    // widen world projections and full-width 2D quads while this is true.
    virtual void setWorldFrameGate(bool) {}
};
