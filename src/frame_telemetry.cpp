#include "frame_telemetry.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace {
constexpr double windowSeconds=0.5;
double wallSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
#if defined(__SWITCH__)
double threadCpuSeconds() {
    uint64_t ticks=0;
    return R_SUCCEEDED(svcGetInfo(&ticks,InfoType_ThreadTickCount,CUR_THREAD_HANDLE,TickCountInfo_Total))?
        double(ticks)/19200000.0:-1;
}
#else
double cpuSeconds(clockid_t clock) {
    timespec value{};
    return clock_gettime(clock,&value)==0?double(value.tv_sec)+double(value.tv_nsec)*1e-9:0;
}
#endif
double deltaMilliseconds(double current,double previous) {
    const double delta=(current-previous)*1000;
    return std::isfinite(delta)?std::max(0.0,delta):0;
}
uint64_t counterDelta(uint64_t current,uint64_t previous){return current>=previous?current-previous:0;}
}
void FrameTelemetry::configure(Backend* value) {
    std::lock_guard lock(mutex);backend=value;resetLocked();
}
void FrameTelemetry::resetLocked() {
    intervals.clear();last={};lastWall=lastCpu=lastProcess=0;initialized=false;haveGpu=backend!=nullptr;
}
void FrameTelemetry::reset(){std::lock_guard lock(mutex);resetLocked();}
void FrameTelemetry::excludeHostTime(uint64_t nanoseconds) {
    const double seconds=double(nanoseconds)/1e9;
    std::lock_guard lock(mutex);
    if(initialized)lastWall+=seconds;
    for(auto& interval:intervals){interval.start+=seconds;interval.end+=seconds;}
}
FrameTelemetry::Sample FrameTelemetry::completed() {
    // These clocks must run on the game thread. In particular, reading the
    // thread clock from snapshot() would measure the window instead.
#if defined(__SWITCH__)
    const double wall=wallSeconds(),cpu=threadCpuSeconds(),process=-1;
    const auto phases=FramePhases::closeFrame();
    Statistics stats;
    if(backend) {
        const auto gpu=backend->statistics();stats.surfaceReadbacks=gpu.readbacks;stats.textureUploads=gpu.textureUploads;
        // deko3d timestamp sums (completed frames); gpu_ms is their per-interval delta.
        stats.gpuNanoseconds=gpu.gpuNanoseconds;stats.gpuSamples=gpu.gpuSamples;
        stats.gpuTimingSupported=gpu.gpuTimingSupported;stats.gpuTimingChecked=gpu.gpuTimingChecked;stats.gpuTimingEnabled=gpu.gpuTimingEnabled;
    }
    std::lock_guard lock(mutex);
    return completedLocked(wall,cpu,process,stats,backend!=nullptr,phases);
#else
    const double wall=wallSeconds(),cpu=cpuSeconds(CLOCK_THREAD_CPUTIME_ID),process=cpuSeconds(CLOCK_PROCESS_CPUTIME_ID);
    // Closing here keeps the phase split on the same completed-frame boundary.
    const auto phases=FramePhases::closeFrame();
    std::lock_guard lock(mutex);
    return completedLocked(wall,cpu,process,backend?backend->statistics():Statistics{},backend!=nullptr,phases);
#endif
}
FrameTelemetry::Sample FrameTelemetry::completedLocked(double wall,double cpu,double process,
        const Statistics& stats,bool hasGpu,const FramePhases::Frame& phases) {
    Sample sample;sample.frame=last.frame+1;sample.stats=stats;haveGpu=hasGpu;
    // A baseline sample's split spans everything since the previous close.
    if(initialized)sample.phases=phases;
    if(cpu<0)sample.cpuMs=-1;
    if(process<0)sample.processCpuMs=-1;
    if(initialized) {
        sample.wallMs=deltaMilliseconds(wall,lastWall);sample.cpuMs=cpu<0 || lastCpu<0?-1:deltaMilliseconds(cpu,lastCpu);
        sample.processCpuMs=process<0 || lastProcess<0?-1:deltaMilliseconds(process,lastProcess);
        const bool gpu=hasGpu && stats.gpuTimingChecked && stats.gpuTimingSupported && stats.gpuTimingEnabled;
        // A toggle starts a fresh GPU averaging window. CPU/FPS history remains
        // useful while the overlay requests or disables asynchronous queries.
        if(stats.gpuTimingEnabled!=last.stats.gpuTimingEnabled ||
           stats.gpuNanoseconds<last.stats.gpuNanoseconds || stats.gpuSamples<last.stats.gpuSamples)
            for(auto& interval:intervals){interval.gpuNanoseconds=0;interval.gpuSamples=0;interval.gpuTimed=false;}
        const uint64_t ns=gpu?counterDelta(stats.gpuNanoseconds,last.stats.gpuNanoseconds):0;
        const uint64_t samples=gpu?counterDelta(stats.gpuSamples,last.stats.gpuSamples):0;
        if(gpu && samples)sample.gpuMs=double(ns)*1e-6;
        intervals.push_back({lastWall,wall,std::max(0.0,sample.cpuMs),ns,samples,gpu});
        while(!intervals.empty() && intervals.front().end<=wall-windowSeconds)intervals.pop_front();
    }
    initialized=true;lastWall=wall;lastCpu=cpu;lastProcess=process;last=sample;return sample;
}
SessionUi::Metrics FrameTelemetry::snapshot() const {
    std::lock_guard lock(mutex);return snapshotLocked(wallSeconds());
}
SessionUi::Metrics FrameTelemetry::snapshotLocked(double wall) const {
    SessionUi::Metrics result;
    if(!haveGpu || (last.stats.gpuTimingChecked && !last.stats.gpuTimingSupported))
        result.gpuState=SessionUi::GpuState::Unsupported;
    if(!initialized)return result;
    // A stalled producer must not leave "60 FPS" frozen on the window.
    if(wall-lastWall>=windowSeconds){result.gameFps=0;return result;}
    uint64_t count=0,gpuNs=0,gpuSamples=0,gpuFrames=0;double cpu=0,start=wall;
    for(const auto& interval:intervals)if(interval.end>wall-windowSeconds) {
        ++count;cpu+=interval.cpuMs;gpuNs+=interval.gpuNanoseconds;gpuSamples+=interval.gpuSamples;
        if(interval.gpuTimed)++gpuFrames;
        start=std::min(start,interval.start);
    }
    if(count) {
        const double duration=wall-std::max(start,wall-windowSeconds);
        if(duration>0)result.gameFps=double(count)/duration;
        if(last.cpuMs>=0)result.cpuMilliseconds=cpu/double(count);
        if(haveGpu && last.stats.gpuTimingChecked && last.stats.gpuTimingSupported && last.stats.gpuTimingEnabled && gpuSamples && gpuFrames) {
            result.gpuState=SessionUi::GpuState::Available;
            // Several pass queries can complete per frame, or a result can
            // arrive several frames late. Divide total GPU work by completed
            // game frames, never by the number of query objects returned.
            result.gpuMilliseconds=double(gpuNs)*1e-6/double(gpuFrames);
        }
    }
    return result;
}
#ifdef NIGHT100_FRAME_TELEMETRY_TESTING
FrameTelemetry::Sample FrameTelemetry::completedForTesting(double wall,double cpu,double process,
        const Statistics& stats,bool hasGpu,const FramePhases::Frame& phases) {
    std::lock_guard lock(mutex);return completedLocked(wall,cpu,process,stats,hasGpu,phases);
}
SessionUi::Metrics FrameTelemetry::snapshotForTesting(double wall) const {
    std::lock_guard lock(mutex);return snapshotLocked(wall);
}
#endif
