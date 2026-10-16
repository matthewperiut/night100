#include "cpu_sampler.h"
#include <switch.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <iostream>

extern "C" [[gnu::noinline]] void night100SwitchCpuSamplerAnchor() noexcept {
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

namespace SwitchCpuSampler {
namespace {
constexpr size_t capacity=1024;
constexpr uint64_t ticksPerSecond=19200000;
constexpr uint64_t periodTicks=ticksPerSecond/20;
constexpr uint64_t durationTicks=ticksPerSecond*60;
struct Observation {uint64_t pc=0,lr=0,sp=0;};
struct State {
    std::atomic<Handle> producer{0};
    Handle host=0;
    bool requested=false,active=false,finished=false,dumped=false;
    const char* reason="disabled";
    uint64_t begin=0,next=0,last=0,longestPauseTicks=0;
    uint32_t pauseAttempts=0,pauseFailures=0,contextFailures=0,resumeRetries=0;
    Result lastPause=0,lastContext=0,lastResume=0;
    size_t count=0;
    std::array<Observation,capacity> observations{};
};
constinit State state;
static_assert(std::atomic<Handle>::is_always_lock_free);

// From successful pause through checked resume, use only stack/POD storage and
// SVCs. The peer may hold allocator, iostream, FS or application locks. In
// particular neither errors nor the destructor below may format a message.
class ResumePeer {
public:
    explicit ResumePeer(Handle handle) noexcept:handle(handle){}
    ~ResumePeer() noexcept {resume();}
    void resume() noexcept {
        if(!paused)return;
        state.lastResume=svcSetThreadActivity(handle,ThreadActivity_Runnable);
        if(R_FAILED(state.lastResume)) {
            ++state.resumeRetries;
            state.lastResume=svcSetThreadActivity(handle,ThreadActivity_Runnable);
            if(R_FAILED(state.lastResume)) {
                // Continuing, throwing, logging or joining could deadlock on
                // locks owned by the still-paused peer. Terminate this opt-in
                // diagnostic process without allocating or running destructors.
                svcExitProcess();
            }
        }
        paused=false;
    }
private:
    Handle handle;
    bool paused=true;
};
void stop(const char* reason) noexcept {
    state.active=false;state.finished=true;state.reason=reason;
}
}

void configure(bool enabled) noexcept {
    // Called before runtime worker creation, or after all workers have joined.
    state.producer.store(0,std::memory_order_relaxed);
    state.requested=enabled;state.active=false;state.finished=false;state.dumped=false;
    state.reason="disabled";state.begin=state.next=state.last=state.longestPauseTicks=0;
    state.pauseAttempts=state.pauseFailures=state.contextFailures=state.resumeRetries=0;
    state.lastPause=state.lastContext=state.lastResume=0;state.count=0;
    if(!enabled)return;
    const auto* self=threadGetSelf();state.host=self?self->handle:0;
    if(!state.host){stop("host_handle_unavailable");return;}
    if(!envIsSyscallHinted(0x32) || !envIsSyscallHinted(0x33)) {
        stop("context_svcs_not_hinted");return;
    }
    state.reason="waiting_for_eligible_gameplay";state.active=true;
}
void producerThreadReady() noexcept {
    if(state.producer.load(std::memory_order_relaxed))return;
    const auto* self=threadGetSelf();
    if(self && self->handle)state.producer.store(self->handle,std::memory_order_release);
}
void poll(bool eligible) noexcept {
    if(!state.requested || state.dumped)return;
    if(state.finished){dump();return;}
    if(!state.active)return;
    const Handle peer=state.producer.load(std::memory_order_acquire);
    if(!peer)return;
    if(peer==state.host){stop("refused_current_thread");dump();return;}
    if(!state.begin && !eligible)return;
    const uint64_t now=armGetSystemTick();
    if(state.begin && now-state.begin>=durationTicks){stop("time_limit");dump();return;}
    if(!eligible || now<state.next)return;
    if(!state.begin)state.begin=now;
    state.next=now+periodTicks;
    // The SVC fills PC/LR/SP on success; read none of them on failure. Leaving
    // the remaining registers uninitialized avoids a large context memset.
    ThreadContext context;
    ++state.pauseAttempts;
    state.lastPause=svcSetThreadActivity(peer,ThreadActivity_Paused);
    if(R_FAILED(state.lastPause)) {
        ++state.pauseFailures;stop("pause_failed");dump();return;
    }
    {
        ResumePeer resume{peer};
        state.lastContext=svcGetThreadContext3(&context,peer);
        resume.resume();
    }
    // Everything below runs only after a successful checked resume.
    state.last=armGetSystemTick();
    const uint64_t pauseTicks=state.last-now;
    if(pauseTicks>state.longestPauseTicks)state.longestPauseTicks=pauseTicks;
    if(R_FAILED(state.lastContext)) {
        ++state.contextFailures;stop("context_failed");dump();return;
    }
    state.observations[state.count++]={context.pc.x,context.lr,context.sp};
    if(state.count==capacity){stop("sample_limit");dump();}
}
void dump() noexcept {
    if(!state.requested || state.dumped)return;
    if(!state.finished)stop("explicit_dump");
    state.dumped=true;
    // Keep 1024 rows below 53KB even for full 16-digit addresses, leaving room in
    // the existing 64KB buffered logger. No std::hex stream flag is shared with
    // producer logs; formatting happens locally after the peer was resumed.
    try {
        char line[512];
        const int size=std::snprintf(line,sizeof(line),
            "\n[cpu-native] reason=%s samples=%zu attempts=%u pause_failures=%u context_failures=%u resume_retries=%u pause_result=0x%x context_result=0x%x resume_result=0x%x anchor=0x%llx longest_pause_us=%llu span_ms=%llu\n[cpu-native] csv_hex=pc,lr,sp\n",
            state.reason,state.count,state.pauseAttempts,state.pauseFailures,state.contextFailures,state.resumeRetries,
            state.lastPause,state.lastContext,state.lastResume,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&night100SwitchCpuSamplerAnchor)),
            static_cast<unsigned long long>(state.longestPauseTicks*1000000/ticksPerSecond),
            static_cast<unsigned long long>((state.last && state.begin)?(state.last-state.begin)*1000/ticksPerSecond:0));
        if(size>0)std::cout.write(line,size_t(size)<sizeof(line)?size:sizeof(line)-1);
        for(size_t i=0;i<state.count;++i) {
            const auto& sample=state.observations[i];
            const int length=std::snprintf(line,sizeof(line),"%llx,%llx,%llx\n",
                static_cast<unsigned long long>(sample.pc),static_cast<unsigned long long>(sample.lr),static_cast<unsigned long long>(sample.sp));
            if(length>0)std::cout.write(line,length);
        }
        std::cout<<"[cpu-native] end\n";
    } catch(...) {
        // This is diagnostic output after resume; losing its report must not
        // change game behavior or re-pause the producer.
    }
}
}
