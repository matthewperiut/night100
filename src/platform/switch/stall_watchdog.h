#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace SwitchStallWatchdog {
// Explicit diagnostic only. Declare before the runtime so workers stop before
// this object is destroyed. The independent worker takes at most one capture.
class Watchdog {
public:
    Watchdog(bool enabled,const std::string& root,const std::string& logId) noexcept;
    ~Watchdog();
    Watchdog(const Watchdog&)=delete;
    Watchdog& operator=(const Watchdog&)=delete;
    void heartbeat(bool foreground=true) noexcept;
    void producerThreadReady() noexcept;
    void audioThreadReady(uint32_t handle) noexcept;
    bool ready() const noexcept;
    uint32_t setupResult() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    uint32_t result=0;
};
}
extern "C" void night100SwitchStallWatchdogAnchor() noexcept;
