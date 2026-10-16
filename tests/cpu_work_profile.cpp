#define NIGHT100_CPU_WORK_PROFILE_TESTING
#include "runtime/cpu_work_profile.h"
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using CpuWorkProfile::Phase;
thread_local uint64_t now=0,reads=0;
thread_local bool fail=false;
unsigned checks=0;
bool clockForTest(uint64_t &value) noexcept {++reads;value=now;return !fail;}
void require(bool value,const char *message) {++checks;if(!value)throw std::runtime_error(message);}
uint64_t ns(const CpuWorkProfile::Counters &value,Phase phase) {return value.nanoseconds[std::size_t(phase)];}
uint64_t total(const CpuWorkProfile::Counters &value) {uint64_t sum=0;for(auto n:value.nanoseconds)sum+=n;return sum;}
void fresh(bool enabled=true) {
    now=reads=0;fail=false;CpuWorkProfile::configureForTesting(enabled,clockForTest);CpuWorkProfile::resetWork();
}
}

int main() {try {
    fresh();
    now=100;
    {
        CpuWorkProfile::Scope iop(Phase::Iop);
        now=200;
        {CpuWorkProfile::Scope spu(Phase::Spu);now=250;}
        now=300;const auto mid=CpuWorkProfile::snapshotWork();
        require(ns(mid,Phase::Other)==100 && ns(mid,Phase::Iop)==150 && ns(mid,Phase::Spu)==50,
                "Snapshot inside IOP lost or double-counted nested SPU work");
        require(total(mid)==300,"Exclusive phase sum does not reconcile at an active snapshot");
        now=350;
    }
    now=400;auto complete=CpuWorkProfile::snapshotWork();
    require(ns(complete,Phase::Other)==150 && ns(complete,Phase::Iop)==200 && ns(complete,Phase::Spu)==50,
            "Resuming after active snapshot charged the wrong phase");
    require(total(complete)==400 && complete.clockFailures==0,"Complete interval did not reconcile");
    require(complete.calls[std::size_t(Phase::Iop)]==1 && complete.calls[std::size_t(Phase::Spu)]==1,
            "Coarse scope call counts are incorrect");

    fresh();now=20;
    try {
        CpuWorkProfile::Scope graphics(Phase::Graphics);
        now=45;CpuWorkProfile::Scope iop(Phase::Iop);now=75;
        throw std::runtime_error("fixture unwind");
    }catch(const std::runtime_error&){}
    now=90;complete=CpuWorkProfile::snapshotWork();
    require(ns(complete,Phase::Other)==35 && ns(complete,Phase::Graphics)==25 && ns(complete,Phase::Iop)==30,
            "Exception unwind failed to restore parent phases");
    require(total(complete)==90,"Exception interval did not reconcile");

    fresh();now=10;
    {
        CpuWorkProfile::Scope outer(Phase::Graphics);
        const auto before=reads;
        {CpuWorkProfile::Scope inner(Phase::Graphics);now=30;}
        require(reads==before,"Nested same-phase helpers read the clock unnecessarily");
        now=40;
    }
    now=50;complete=CpuWorkProfile::snapshotWork();
    require(ns(complete,Phase::Graphics)==30 && ns(complete,Phase::Other)==20,
            "Nested same-phase work was double-counted");
    require(complete.calls[std::size_t(Phase::Graphics)]==2,"Same-phase scope calls were not counted");

    fresh();now=10;
    {
        CpuWorkProfile::Scope iop(Phase::Iop);now=20;
        {CpuWorkProfile::Scope spu(Phase::Spu);now=30;CpuWorkProfile::resetWork();now=40;}
        now=50;
    }
    now=60;complete=CpuWorkProfile::snapshotWork();
    require(ns(complete,Phase::Other)==10 && ns(complete,Phase::Iop)==10 && ns(complete,Phase::Spu)==10,
            "Reset inside active scopes changed their current/parent phase");
    require(total(complete)==30,"Reset retained old work or lost new work");

    fresh(false);now=100;
    {CpuWorkProfile::Scope off(Phase::Iop);now=200;}
    complete=CpuWorkProfile::snapshotWork();CpuWorkProfile::resetWork();
    require(reads==0 && total(complete)==0,"Disabled profiling reads clocks or charges CPU");
    fresh();const auto before=reads;
    {CpuWorkProfile::Scope empty(Phase::Spu,false);now=20;}
    require(reads==before,"Empty SPU advances read the clock");
    complete=CpuWorkProfile::snapshotWork();
    require(total(complete)==20 && ns(complete,Phase::Spu)==0,"Empty work changed the active phase");

    fresh();now=10;
    {
        CpuWorkProfile::Scope iop(Phase::Iop);now=20;fail=true;
        {CpuWorkProfile::Scope spu(Phase::Spu);now=30;fail=false;}
        now=40;
    }
    now=50;complete=CpuWorkProfile::snapshotWork();
    require(complete.clockFailures==1,"Clock failure was silently accepted");
    require(total(complete)==30,"Failed clock interval was misattributed or underflowed");
    fresh();now=100;CpuWorkProfile::resetWork();now=90;
    {CpuWorkProfile::Scope iop(Phase::Iop);now=95;}
    now=110;complete=CpuWorkProfile::snapshotWork();
    require(complete.clockFailures==1 && total(complete)==20,"Backwards clock produced unsigned underflow");

    fresh();now=10;CpuWorkProfile::Counters worker;
    std::thread separate([&] {
        fresh();now=7;{CpuWorkProfile::Scope graphics(Phase::Graphics);now=17;}
        now=30;worker=CpuWorkProfile::snapshotWork();
    });separate.join();
    now=40;complete=CpuWorkProfile::snapshotWork();
    require(total(complete)==40 && ns(complete,Phase::Graphics)==0,"Window/worker CPU leaked into game-thread counters");
    require(total(worker)==30 && ns(worker,Phase::Graphics)==10,"Worker thread did not maintain independent counters");
    now=50;const auto second=CpuWorkProfile::snapshotWork();
    require(total(second)-total(complete)==10,"Cumulative snapshots cannot form frame deltas");
    // Real Switch clock conversion must retain sub-second precision without
    // overflowing even when the raw SVC value itself fits in uint64.
    uint64_t converted=99;
    for(const auto [ticks,expected]:{std::pair<uint64_t,uint64_t>{0,0},{1,52},{12,625},{19200000,1000000000},
            {UINT64_C(192000000000000),UINT64_C(10000000000000000)}}) {
        require(CpuWorkProfile::detail::threadTicksToNanoseconds(ticks,converted) && converted==expected,
                "Horizon CPU ticks lost exact625/12 nanosecond conversion");
    }
    converted=99;
    require(!CpuWorkProfile::detail::threadTicksToNanoseconds(UINT64_MAX,converted) && converted==99,
            "Oversized Horizon CPU clock wrapped or modified its output");

    // Follow the production global flag rather than the per-test override.
    CpuWorkProfile::configureForTesting(-1,clockForTest);now=reads=0;
    CpuWorkProfile::configure(false);
    {CpuWorkProfile::Scope disabled(Phase::Graphics);now=100;}
    require(reads==0 && !CpuWorkProfile::enabled(),"Explicit global disable reads clocks");
    CpuWorkProfile::configure(true);CpuWorkProfile::resetWork();now=110;
    {
        CpuWorkProfile::Scope graphics(Phase::Graphics);now=120;
        auto observed=CpuWorkProfile::snapshotWork();
        require(ns(observed,Phase::Graphics)==10 && observed.clockReads==reads,
                "Explicit activation or clock-read overhead counter failed");
        CpuWorkProfile::setSampling(false);const auto disabledReads=reads;
        // Even with no clocks, an active child must restore its parent across
        // a frame boundary that enables profiling before its destructor.
        {
            CpuWorkProfile::Scope iop(Phase::Iop);now=1000;
            require(reads==disabledReads,"Unsampled work reads the CPU clock");
            CpuWorkProfile::setSampling(true);CpuWorkProfile::resetWork();now=1010;
        }
        now=1020;observed=CpuWorkProfile::snapshotWork();
        require(ns(observed,Phase::Iop)==10 && ns(observed,Phase::Graphics)==10 && total(observed)==20,
                "Sample activation inside nested work lost parent ownership or charged the disabled gap");
        CpuWorkProfile::setSampling(false);const auto offReads=reads;now=2000;
        // The outer destructor below must restore Other without charging.
        require(CpuWorkProfile::snapshotWork().clockReads==0 && reads==offReads,
                "Disabled sample snapshot reads clocks or exposes counters");
    }
    const auto offReads=reads;
    CpuWorkProfile::setSampling(true);CpuWorkProfile::resetWork();now=2010;
    complete=CpuWorkProfile::snapshotWork();
    require(ns(complete,Phase::Other)==10 && ns(complete,Phase::Graphics)==0,
            "Unsampled parent destructor failed to restore Other");
    require(reads==offReads+2,"Sample reset/snapshot read unexpected clocks");
    CpuWorkProfile::configure(false);const auto finalReads=reads;
    {CpuWorkProfile::Scope disabled(Phase::Spu);}
    require(reads==finalReads && !CpuWorkProfile::enabled(),"Explicit disable after activation reads clocks");
    std::cout<<"CPU work profile: "<<checks<<" checks passed\n";return 0;
}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}
