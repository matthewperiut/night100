#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>

// Main-thread applet messages publish transitions. Only the game producer
// consumes elapsed time; neither hooks nor audio touch game clock state.
namespace SwitchLifecycle {
class State {
    mutable std::mutex mutex;
    std::atomic<bool> focused{true},pending{false};
    uint64_t completed=0,since=0,consumed=0,generation=0;
    bool primed=false;
public:
    void reset(uint64_t now,bool active) {
        std::lock_guard lock(mutex);
        completed=consumed=0;generation=active?0:1;since=now;primed=false;
        focused.store(active,std::memory_order_relaxed);
        pending.store(!active,std::memory_order_release);
    }
    bool active() const {return focused.load(std::memory_order_acquire);}
    // Relaxed: a hint checked at every checkpoint. A stale false only defers
    // service to the next checkpoint; state changes are read under the mutex.
    bool needsService() const {return pending.load(std::memory_order_relaxed);}
    uint64_t inactiveGeneration() const {
        std::lock_guard lock(mutex);
        return focused.load(std::memory_order_relaxed)?0:generation;
    }
    void setFocus(uint64_t now,bool active) {
        std::lock_guard lock(mutex);
        if(active==focused.load(std::memory_order_relaxed))return;
        ++generation;
        if(active)completed+=now>=since?now-since:0;
        else since=now;
        focused.store(active,std::memory_order_release);
        pending.store(true,std::memory_order_release);
    }
    uint64_t inactiveNanoseconds(uint64_t now) const {
        std::lock_guard lock(mutex);
        return completed+(!focused.load(std::memory_order_relaxed) && now>=since?now-since:0);
    }
    // Establish the epoch before executing guest code. Later calls consume
    // complete gaps exactly once, including transitions missed while asleep.
    void prime(uint64_t now) {
        std::lock_guard lock(mutex);
        if(!primed){
            consumed=completed;primed=true;
            if(!focused.load(std::memory_order_relaxed))since=now;
        }
    }
    uint64_t consume() {
        std::lock_guard lock(mutex);
        if(!focused.load(std::memory_order_relaxed))return 0;
        const uint64_t result=completed-consumed;consumed=completed;
        pending.store(false,std::memory_order_release);return result;
    }
};
}
