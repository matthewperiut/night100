#pragma once
#include <cstdint>

class NativeSceneBackend;

// Switch 16:9 output. The world fills the 1280x720 display only while the game
// shows its gameplay camera; 2D, menus, loading and movies stay in the 4:3 area.
// The gate is read from guest RAM by the host poll and never replaces guest code.
namespace Widescreen {
// Set from the Switch launcher before the game starts (--aspect 16:9 is the default).
void setEnabled(bool enabled);
// Whether 16:9 output is configured (Switch launcher).
bool isEnabled();
// The render backend that receives the per-frame world gate.
void setBackend(NativeSceneBackend* backend);
// True during gameplay with the camera showing the world (eGameMode Game or Pause,
// eGameOstrich InScene). Map, menus, title, loading and movies return false.
bool worldCameraActive(const uint8_t* ram);
// Publishes the gate sampled from guest RAM (host poll thread).
void sample(const uint8_t* ram);
// Diagnostic snapshot of the last sample, for the periodic summary.
struct Status {
    bool enabled=false,gate=false;
    uint32_t mode=0,ostrich=0;
    unsigned long long samples=0,gateTrueSamples=0;
};
Status status();
}
