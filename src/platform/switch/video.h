#pragma once

#include <cstdint>
#include <memory>

namespace SwitchVideo {
enum class Matrix : uint8_t { Bt601, Bt709, Bt2020 };
struct Frame {
    // At the pinned FFmpeg revision, decode.c invokes the NVTEGRA
    // post_process hook before returning a frame. nvtegra_decode.c's hook
    // waits the NVDEC syncpoint and validates its status, so decoder writes
    // have completed before this frame reaches Deko. Deko still invalidates
    // its image cache and fences its own subsequent texture reads.
    // NVDEC-owned storage imported into Deko with DkMemBlockMaker.storage.
    // The CPU address identifies the allocation; the CPU never reads pixels.
    void* storage = nullptr;
    uint32_t storageBytes = 0;
    uint32_t mapHandle = 0;
    uint32_t planeOffset[2]{};
    uint32_t planePitch[2]{};
    uint32_t width = 0, height = 0;
    bool pitchLinear = false;
    bool fullRange = false;
    Matrix matrix = Matrix::Bt601;
    double aspect = 4.0 / 3.0;
    // Retain this through the GPU consumer fence. Releasing it early allows
    // the decoder's bounded surface pool to overwrite a sampled frame.
    std::shared_ptr<const void> lifetime;
};
using Present = bool (*)(const Frame&, void* opaque);
// Register before runtime execution. Callback executes on the game/GPU thread.
void setPresenter(Present present, void* opaque);
}
