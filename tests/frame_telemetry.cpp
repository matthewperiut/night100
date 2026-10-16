#include "frame_telemetry.h"
#include <atomic>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
unsigned checks=0;
void require(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
bool near(const std::optional<double>& value,double expected){return value && std::abs(*value-expected)<1e-7;}
GL33Backend::Statistics supported() {
    GL33Backend::Statistics result;result.gpuTimingChecked=true;result.gpuTimingSupported=true;result.gpuTimingEnabled=true;return result;
}
}
int main(){try {
    FrameTelemetry telemetry;auto stats=supported();
    auto sample=telemetry.completedForTesting(100,10,20,stats);
    require(sample.frame==1 && sample.wallMs==0 && sample.cpuMs==0 && !sample.gpuMs,"First completion did not establish a clean timing baseline");
    require(!telemetry.snapshotForTesting(100).gameFps,"One completion fabricated a frame interval");
    telemetry.completedForTesting(100.1,10.003,20.005,stats);
    require(telemetry.snapshotForTesting(100.1).gpuState==SessionUi::GpuState::Pending,"Unsupported pending GPU queries fabricated a timing");
    stats.gpuNanoseconds=6000000;stats.gpuSamples=4;
    sample=telemetry.completedForTesting(100.2,10.007,20.011,stats);
    require(sample.frame==3 && std::abs(sample.wallMs-100)<1e-7 && std::abs(sample.cpuMs-4)<1e-7 && std::abs(sample.processCpuMs-6)<1e-7,"Completed-frame CPU/wall/process deltas changed units or ownership");
    require(near(sample.gpuMs,6),"Delayed GPU result batch was divided by query count");
    stats.gpuNanoseconds=9000000;stats.gpuSamples=6;
    telemetry.completedForTesting(100.3,10.012,20.020,stats);
    auto metrics=telemetry.snapshotForTesting(100.3);
    require(near(metrics.gameFps,10) && near(metrics.cpuMilliseconds,4),"Rolling completed-frame FPS/CPU averages are wrong");
    require(metrics.gpuState==SessionUi::GpuState::Available && near(metrics.gpuMilliseconds,3),"GPU rolling work must divide nine milliseconds by three game frames, including a delayed result frame");
    telemetry.completedForTesting(100.4,10.016,20.025,stats);
    telemetry.completedForTesting(100.5,10.020,20.030,stats);
    metrics=telemetry.snapshotForTesting(100.61);
    require(near(metrics.gameFps,8),"Stalled completed-frame producer did not decay rolling FPS");
    metrics=telemetry.snapshotForTesting(101.001);
    require(near(metrics.gameFps,0) && !metrics.cpuMilliseconds && !metrics.gpuMilliseconds,"Long-stalled producer retained stale CPU/GPU/FPS values");
    telemetry.completedForTesting(110,11,22,stats);
    metrics=telemetry.snapshotForTesting(110);
    require(near(metrics.gameFps,2),"A long loading gap escaped the half-second FPS window");
    telemetry.reset();stats={};telemetry.completedForTesting(0,0,0,stats);
    telemetry.completedForTesting(.1,.002,.004,stats);
    require(telemetry.snapshotForTesting(.1).gpuState==SessionUi::GpuState::Pending,"Unchecked GPU capability was labeled unsupported");
    stats.gpuTimingChecked=true;telemetry.completedForTesting(.2,.004,.008,stats);
    require(telemetry.snapshotForTesting(.2).gpuState==SessionUi::GpuState::Unsupported,"Checked unsupported GPU capability was labeled pending");
    telemetry.reset();stats=supported();telemetry.completedForTesting(0,0,0,stats,false);
    telemetry.completedForTesting(.1,.002,.004,stats,false);
    require(telemetry.snapshotForTesting(.1).gpuState==SessionUi::GpuState::Unsupported,"CPU renderer exposed a GPU timing capability");
    telemetry.reset();stats=supported();stats.gpuTimingEnabled=false;telemetry.completedForTesting(0,0,0,stats);
    telemetry.completedForTesting(.1,.002,.004,stats);stats.gpuTimingEnabled=true;
    stats.gpuNanoseconds=2000000;stats.gpuSamples=1;telemetry.completedForTesting(.2,.004,.008,stats);
    require(near(telemetry.snapshotForTesting(.2).gpuMilliseconds,2),"Disabled-query frames diluted the newly enabled GPU average");
    stats.gpuTimingEnabled=false;telemetry.completedForTesting(.3,.006,.012,stats);
    metrics=telemetry.snapshotForTesting(.3);
    require(metrics.gpuState==SessionUi::GpuState::Pending && !metrics.gpuMilliseconds,"Disabling GPU timing retained an available GPU number");
    telemetry.reset();stats=supported();telemetry.completedForTesting(0,0,0,stats);
    stats.gpuNanoseconds=3000000;stats.gpuSamples=2;telemetry.completedForTesting(.1,.001,.002,stats);
    stats.gpuNanoseconds=1;stats.gpuSamples=0;sample=telemetry.completedForTesting(.2,.0001,.0001,stats);
    metrics=telemetry.snapshotForTesting(.2);
    require(sample.cpuMs==0 && sample.processCpuMs==0 && !sample.gpuMs && !metrics.gpuMilliseconds,"Reset clock/counters produced negative CPU or wrapped GPU durations");
    telemetry.reset();stats=supported();telemetry.completedForTesting(10,1,2,stats);
    telemetry.completedForTesting(10.1,1.002,2.003,stats);
    telemetry.excludeHostTime(20000000000ULL);
    sample=telemetry.completedForTesting(30.2,1.006,2.008,stats);
    metrics=telemetry.snapshotForTesting(30.2);
    require(std::abs(sample.wallMs-100)<1e-7 && near(metrics.gameFps,10),"Background exclusion changed foreground frame timing or discarded the rolling history");
    require(std::abs(sample.cpuMs-4)<1e-7 && std::abs(sample.processCpuMs-5)<1e-7,"Background wall-time exclusion changed real CPU work");
    telemetry.reset();telemetry.completedForTesting(0,-1,-1,stats);
    sample=telemetry.completedForTesting(.1,-1,-1,stats);
    require(sample.cpuMs==-1 && sample.processCpuMs==-1 && !telemetry.snapshotForTesting(.1).cpuMilliseconds,"Unavailable CPU clocks fabricated zero work");
    sample=telemetry.completedForTesting(.2,1,2,stats);
    require(sample.cpuMs==-1 && sample.processCpuMs==-1,"Recovered CPU clocks fabricated work across an unavailable baseline");
    sample=telemetry.completedForTesting(.3,1.002,2.003,stats);
    require(std::abs(sample.cpuMs-2)<1e-7 && std::abs(sample.processCpuMs-3)<1e-7,"Recovered CPU clock did not establish a new valid baseline");
    telemetry.reset();stats=supported();std::atomic<bool> done=false;
    std::thread producer([&] {
        for(unsigned i=0;i<3000;++i)telemetry.completedForTesting(i*.001,i*.0001,i*.0002,stats);
        done.store(true,std::memory_order_release);
    });
    while(!done.load(std::memory_order_acquire)) {
        const auto snapshot=telemetry.snapshotForTesting(3);
        require(!snapshot.gameFps || (std::isfinite(*snapshot.gameFps) && *snapshot.gameFps>=0),"Concurrent UI snapshot observed invalid producer data");
    }
    producer.join();telemetry.configure(nullptr);
    require(!telemetry.snapshot().gameFps && telemetry.snapshot().gpuState==SessionUi::GpuState::Unsupported,"Configure failed to clear the previous runtime's metrics");
    {
        // zGameLoop phase split: wall per phase, thread CPU for update/render/present.
        using namespace FramePhases;
        resetForTesting(true);
        markAt(PostPresent,1000,unavailable);markAt(Pad,1500,unavailable);
        markAt(Update,2000,500);markAt(PostUpdate,5000,2500);
        markAt(Render,6000,3000);markAt(Render,6500,unavailable);markAt(RenderEnd,9000,5500);
        markAt(Present,10000,6000);
        auto frame=closeFrameAt(14000,6300);
        require(frame.valid && std::abs(frame.ms[Update]-0.003)<1e-12 && std::abs(frame.ms[Render]-0.003)<1e-12,"Phase wall time was not split at its boundaries");
        require(std::abs(frame.ms[Present]-0.004)<1e-12 && std::abs(frame.ms[PostPresent]-0.0005)<1e-12 && frame.ms[Other]==0,"Present or loop-top phases lost time");
        require(std::abs(frame.cpuMs[Update]-0.002)<1e-12 && std::abs(frame.cpuMs[Render]-0.0025)<1e-12 && std::abs(frame.cpuMs[Present]-0.0003)<1e-12,"Phase CPU was not taken at update/render/present boundaries");
        require(frame.cpuMs[Pad]==-1 && frame.cpuMs[PostUpdate]==-1,"Untimed phases reported CPU");
        double total=0;for(double ms:frame.ms)total+=ms;
        require(std::abs(total-0.013)<1e-12,"Phase split does not cover the whole completed-frame interval");
        excludeHostTime(100000);markAt(PostPresent,114500,unavailable);
        frame=closeFrameAt(115000,unavailable);
        require(std::abs(frame.ms[Other]-0.0005)<1e-12 && std::abs(frame.ms[PostPresent]-0.0005)<1e-12 && frame.cpuMs[Update]==-1,"Host pause leaked into phase time or a stale CPU interval repeated");
        markAt(Update,116000,unavailable);markAt(Render,117000,9000);
        frame=closeFrameAt(118000,9500);
        require(frame.cpuMs[Update]==-1 && std::abs(frame.cpuMs[Render]-0.0005)<1e-12 && state.totals.cpuFailures==1,"An unavailable CPU clock fabricated update CPU");
        FrameTelemetry phases;Frame split;split.valid=true;split.ms[Render]=2;
        sample=phases.completedForTesting(0,0,0,supported(),true,split);
        require(!sample.phases.valid,"Baseline completion reported a phase split");
        sample=phases.completedForTesting(.016,.004,.005,supported(),true,split);
        require(sample.phases.valid && sample.phases.ms[Render]==2,"Completed frame dropped its phase split");
        resetForTesting(false);
    }
    std::cout<<"Frame telemetry checks: "<<checks<<" passed\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
