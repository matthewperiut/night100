#pragma once
#if defined(__SWITCH__)
#include "graphics/deko3d_backend.h"
#else
#include "graphics/gl33_backend.h"
#endif
#include "frame_phases.h"
#include "session_ui.h"
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>

// One producer at completed game frames; the window only consumes snapshot().
// GPU durations are asynchronous query results, not synchronous frame timings.
class FrameTelemetry {
public:
#if defined(__SWITCH__)
    using Backend=Deko3DBackend;
    struct Statistics {
        uint64_t surfaceReadbacks=0,textureUploads=0,gpuNanoseconds=0,gpuSamples=0;
        bool gpuTimingSupported=false,gpuTimingChecked=true,gpuTimingEnabled=false;
    };
#else
    using Backend=GL33Backend;
    using Statistics=GL33Backend::Statistics;
#endif
    struct Sample {
        uint64_t frame=0;
        double wallMs=0,cpuMs=0,processCpuMs=0;
        std::optional<double> gpuMs;
        Statistics stats;
        // zGameLoop split of this interval; invalid on a baseline sample.
        FramePhases::Frame phases;
    };
    void configure(Backend*);
    Sample completed();
    SessionUi::Metrics snapshot() const;
    void reset();
    void excludeHostTime(uint64_t nanoseconds);
#ifdef NIGHT100_FRAME_TELEMETRY_TESTING
    Sample completedForTesting(double wallSeconds,double cpuSeconds,double processSeconds,
                               const Statistics&,bool hasGpu=true,const FramePhases::Frame& phases={});
    SessionUi::Metrics snapshotForTesting(double wallSeconds) const;
#endif
private:
    using Clock=std::chrono::steady_clock;
    struct Interval {
        double start=0,end=0,cpuMs=0;
        uint64_t gpuNanoseconds=0,gpuSamples=0;
        bool gpuTimed=false;
    };
    Sample completedLocked(double wall,double cpu,double process,const Statistics&,bool,const FramePhases::Frame&);
    SessionUi::Metrics snapshotLocked(double wall) const;
    void resetLocked();
    mutable std::mutex mutex;
    Backend* backend=nullptr;
    std::deque<Interval> intervals;
    Sample last;
    double lastWall=0,lastCpu=0,lastProcess=0;
    bool initialized=false,haveGpu=false;
};
