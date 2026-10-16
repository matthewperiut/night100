#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
class PS2Runtime;

namespace SwitchTelemetry {
// Captures verbose runtime streams in a bounded RAM buffer. SD writes happen at
// boot milestones, infrequent host polls, final shutdown and errors.
class Session {
public:
    explicit Session(const std::string& runId,double flushSeconds=15,const std::string& root="/switch/night100",bool cpuProfile=false);
    ~Session();
    void stage(const std::string&);
    void poll();
    void finish(const std::string&);
    // Appends the provider's text to each stage/summary line (called under no lock).
    void setSummaryProvider(std::function<std::string()> provider);
    void completedFrame();
    void excludeHostTime(uint64_t nanoseconds);
    struct Metrics {double gameFps=0,producerCpuMs=-1;};
    Metrics metrics() const noexcept;
    uint64_t frameCount() const noexcept;
    double frameIdleSeconds() const noexcept;
        const std::string& path() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
// Process-wide access for code that has no Session pointer (e.g. the movie
// player). Both are no-ops without a live session. stageCurrent flushes to SD
// immediately, so a later crash keeps the marker. finishCurrent is the
// best-effort fatal record, also installed as the std::terminate handler.
void stageCurrent(const std::string& text) noexcept;
void finishCurrent(const std::string& text) noexcept;
}
