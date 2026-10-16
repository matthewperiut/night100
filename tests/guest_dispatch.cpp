#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <iostream>
#include <stdexcept>
#include <atomic>
#include <thread>

namespace {
constexpr uint32_t entry=0x1000,middleEntry=0x1100,callerReturn=0x2008,stack=0x180000;
enum class Scenario { UnchangedHle, NormalReturn, ExplicitReturn, Recursive, IndirectRecursive, Backedge,
    PendingAtCallerReturn, Nested, DirectTail, IndirectTail, TailAtCallerReturn };
Scenario scenario;
unsigned entries=0;
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
void pendingWake(PS2Runtime& runtime){runtime.postEeEvent({EeEventType::ExternalWake,0,0});}
void middle(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    ++entries;SET_GPR_U32(cpu,29,GPR_U32(cpu,29)-32);SET_GPR_U32(cpu,31,0x1118);
    pendingWake(*runtime);
    require(!runtime->dispatchGuestBranch(ram,cpu,entry,0x1110,0x1118,
        PS2Runtime::GuestBranchKind::IndirectCall,"nested recursive call"),"nested pending call unexpectedly completed");
}
void callee(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    ++entries;
    if(scenario==Scenario::UnchangedHle)return;
    if(scenario==Scenario::NormalReturn){cpu->pc=callerReturn;return;}
    if(scenario==Scenario::ExplicitReturn) {
        require(!runtime->dispatchGuestBranch(ram,cpu,callerReturn,0x1010,0,
            PS2Runtime::GuestBranchKind::Return,"explicit ordinary return"),"return dispatch unexpectedly continued");
        return;
    }
    SET_GPR_U32(cpu,29,GPR_U32(cpu,29)-64);SET_GPR_U32(cpu,31,0x1018);
    if(scenario==Scenario::DirectTail || scenario==Scenario::IndirectTail || scenario==Scenario::TailAtCallerReturn) {
        const auto kind=scenario==Scenario::DirectTail?PS2Runtime::GuestBranchKind::DirectJump:PS2Runtime::GuestBranchKind::IndirectJump;
        const auto target=scenario==Scenario::TailAtCallerReturn?callerReturn:entry;
        require(!runtime->dispatchGuestBranch(ram,cpu,target,0x1010,0,kind,"pending tail transfer"),"tail transfer unexpectedly completed");
        return;
    }
    if(scenario==Scenario::Nested) {
        require(!runtime->dispatchGuestBranch(ram,cpu,middleEntry,0x1010,0x1018,
            PS2Runtime::GuestBranchKind::DirectCall,"nested call"),"nested yield did not unwind its parent");
        return;
    }
    pendingWake(*runtime);
    if(scenario==Scenario::Backedge) {
        cpu->pc=entry;
        require(runtime->eeCheckpointDue(),"generated backedge did not observe pending event");
        return;
    }
    const uint32_t target=scenario==Scenario::PendingAtCallerReturn?callerReturn:entry;
    const auto kind=scenario==Scenario::IndirectRecursive?PS2Runtime::GuestBranchKind::IndirectCall:PS2Runtime::GuestBranchKind::DirectCall;
    require(!runtime->dispatchGuestBranch(ram,cpu,target,0x1010,0x1018,kind,"pending recursive call"),
        "recursive checkpoint did not yield before reentering callee");
}
}
int main() {
    try {
        PS2Runtime runtime;
        require(runtime.memory().initialize() && runtime.syncCoreSubsystems(),"runtime memory initialization failed");
        require(runtime.registerFunction(entry,callee) && runtime.registerFunction(middleEntry,middle)
            && runtime.registerFunction(callerReturn,callee),"synthetic function registration failed");
        auto* ram=runtime.memory().getRDRAM();
        for(auto value:{Scenario::UnchangedHle,Scenario::NormalReturn,Scenario::ExplicitReturn,Scenario::Recursive,Scenario::IndirectRecursive,
                Scenario::Backedge,Scenario::PendingAtCallerReturn,Scenario::Nested,Scenario::DirectTail,Scenario::IndirectTail,Scenario::TailAtCallerReturn}) {
            scenario=value;entries=0;
            auto& cpu=runtime.cpu();cpu=R5900Context{};
            SET_GPR_U32(&cpu,29,stack);SET_GPR_U32(&cpu,31,callerReturn);
            runtime.eeScheduler().reset(ram,cpu);
            const bool continued=runtime.dispatchGuestBranch(ram,&cpu,entry,0x2000,callerReturn,
                PS2Runtime::GuestBranchKind::DirectCall,"outer call");
            const bool completes=value==Scenario::UnchangedHle || value==Scenario::NormalReturn || value==Scenario::ExplicitReturn;
            require(continued==completes,"caller continued after nested cooperative yield");
            const uint32_t expectedPc=completes || value==Scenario::PendingAtCallerReturn || value==Scenario::TailAtCallerReturn?callerReturn:entry;
            require(cpu.pc==expectedPc,"pending guest PC was replaced by caller continuation");
            require(GPR_U32((&cpu),29)==stack-(completes?0:value==Scenario::Nested?96:64),"guest stack was changed while unwinding native calls");
            require(entries==(value==Scenario::Nested?2u:1u),"pending recursive callee ran before scheduler checkpoint");
            require(!runtime.isStopRequested(),"checkpoint incorrectly stopped runtime");
        }
        // The host must inspect a stable published view while the executor
        // publishes new views, without borrowing or racing live thread records.
        std::atomic<bool> published{false},readerReady{false},done{false};
        unsigned observations=0,contended=0;
        bool valid=true;
        auto context=runtime.cpu();context.pc=entry;
        std::jthread publisher([&] {
            runtime.eeScheduler().reset(ram,context);
            published.store(true,std::memory_order_release);
            while(!readerReady.load(std::memory_order_acquire))std::this_thread::yield();
            for(unsigned n=0;n<5000;++n)runtime.eeScheduler().publishSnapshot();
            done.store(true,std::memory_order_release);
        });
        while(!published.load(std::memory_order_acquire))std::this_thread::yield();
        uint64_t previous=0;
        while(!done.load(std::memory_order_acquire)) {
            EeKernelSnapshot view;view.sequence=UINT64_MAX;
            if(runtime.eeScheduler().trySnapshot(view)) {
                ++observations;
                valid &= view.sequence>=previous && !view.threads.empty() && view.threads.front().pc==entry;
                previous=view.sequence;
                readerReady.store(true,std::memory_order_release);
            } else {++contended;valid &= view.sequence==UINT64_MAX;}
        }
        publisher.join();
        require(valid && observations>0,"nonblocking concurrent EE view was invalid");
        EeKernelSnapshot finalView;
        require(runtime.eeScheduler().trySnapshot(finalView),"final published EE view unavailable");
        std::cout<<"PASS: 11 direct/indirect recursive and nested yields, generated backedges, pending tail transfers, caller-PC targets, unchanged-PC HLE and ordinary returns\n";
        std::cout<<"PASS: concurrent nonblocking published EE views="<<observations<<" contended="<<contended<<'\n';
        return 0;
    } catch(const std::exception& error){std::cerr<<"FAIL: scenario="<<static_cast<unsigned>(scenario)<<" "<<error.what()<<'\n';return 1;}
}
