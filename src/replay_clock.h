#pragma once
#include "input_replay.h"
#include <cstdint>
#include <filesystem>
#include <memory>

// A clip's native game-clock calls, indexed by its logical input frame. Device
// clocks remain asynchronous; semantic end checkpoints detect resulting drift.
class ReplayClock {
public:
    ReplayClock(InputReplay::Mode,const std::filesystem::path&);
    ~ReplayClock();
    ReplayClock(const ReplayClock&)=delete;
    ReplayClock& operator=(const ReplayClock&)=delete;
    uint64_t sample(uint64_t frame,uint64_t actualTicks);
    // Optional durability boundary; sample() itself never flushes its stream.
    void completed(uint64_t frame);
    void finish(uint64_t finalFrame);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
