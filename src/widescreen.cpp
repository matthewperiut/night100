#include "widescreen.h"
#include "graphics/native_scene_backend.h"
#include <atomic>
#include <cstring>

namespace Widescreen {
namespace {
// Retail globals used by the replay checkpoints (replay_catalog.cpp).
// eGameMode_Game = 11, eGameMode_Pause = 7 (types.json enum), and
// eGameOstrich_InScene = 2 with PlayingMovie = 1 and Loading = 0.
constexpr uint32_t gameModeAddress=0x357e20;
constexpr uint32_t gameOstrichAddress=0x357e24;
constexpr uint32_t modePause=7,modeGame=11,ostrichInScene=2;

NativeSceneBackend* backend=nullptr;
bool enabled=false;
std::atomic<uint32_t> lastMode{0},lastOstrich{0};
std::atomic<bool> lastGate{false};
std::atomic<unsigned long long> sampleCount{0},gateTrueCount{0};

uint32_t loadWord(const uint8_t* ram,uint32_t address) {
    uint32_t value=0;std::memcpy(&value,ram+address,4);return value;
}
}

void setEnabled(bool value){enabled=value;}
bool isEnabled(){return enabled;}
void setBackend(NativeSceneBackend* value){backend=value;}

bool worldCameraActive(const uint8_t* ram) {
    if(!ram)return false;
    const uint32_t mode=loadWord(ram,gameModeAddress),ostrich=loadWord(ram,gameOstrichAddress);
    return (mode==modeGame || mode==modePause) && ostrich==ostrichInScene;
}

void sample(const uint8_t* ram) {
    const bool gate=worldCameraActive(ram);
    if(!ram)return;
    lastMode.store(loadWord(ram,gameModeAddress),std::memory_order_relaxed);
    lastOstrich.store(loadWord(ram,gameOstrichAddress),std::memory_order_relaxed);
    lastGate.store(gate,std::memory_order_relaxed);
    sampleCount.fetch_add(1,std::memory_order_relaxed);
    if(gate)gateTrueCount.fetch_add(1,std::memory_order_relaxed);
    if(backend && enabled)backend->setWorldFrameGate(gate);
}
Status status() {
    Status result;
    result.enabled=enabled;result.gate=lastGate.load(std::memory_order_relaxed);
    result.mode=lastMode.load(std::memory_order_relaxed);result.ostrich=lastOstrich.load(std::memory_order_relaxed);
    result.samples=sampleCount.load(std::memory_order_relaxed);result.gateTrueSamples=gateTrueCount.load(std::memory_order_relaxed);
    return result;
}
}
