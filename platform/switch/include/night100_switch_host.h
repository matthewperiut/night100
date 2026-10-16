#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>
#include "input_chords.h"
// The GPU backend owns its device, queues and ordered completed-frame
// presentation. This frontend supplies libnx lifecycle, HID and audout; it
// never downloads frames.
void night100SwitchHostInitialize();
bool night100SwitchHostPoll();
void night100SwitchHostShutdown();
bool night100SwitchHostActive();
// Called on the applet hook thread after each focus transition (true = foreground).
void night100SwitchHostSetFocusCallback(void (*callback)(bool,void*),void* data);
bool night100SwitchHostMetricsEnabled();
// True once after L+R+Plus has been held for one second.
bool night100SwitchHostTakeExitChord();
// L+R+X was pressed since the last call (internal resolution cycle).
bool night100SwitchHostTakeResolutionChord();
// Console (docked) operation mode as of the last host poll.
bool night100SwitchHostDocked();
// Main-thread replay UI consumes one edge-triggered HID action per host poll.
SwitchInput::ReplayUpdate night100SwitchHostReplayInput();
void night100SwitchHostSetReplayCatalogCount(size_t count,bool selectNewest=false);
void night100SwitchHostCloseReplayPicker();
uint32_t night100SwitchHostAudioThreadHandle();
void night100SwitchHostBeginExecution();
uint64_t night100SwitchHostInactiveNanoseconds();
// Mirrors SwitchLifecycle::needsService(). Host code republishes it on every
// transition and consumption; a stale false only defers service to the next checkpoint.
extern std::atomic<bool> night100SwitchLifecyclePending;
uint64_t night100SwitchHostServiceLifecycleSlow(bool (*stop)(void*),void* data);
// Inline fast path: one relaxed load, no call, no lock, no syscall while nothing is pending.
// Diagnostic only: build with -DNIGHT100_LIFECYCLE_CALL_COUNT to count checkpoint calls.
#if defined(NIGHT100_LIFECYCLE_CALL_COUNT)
extern std::atomic<uint64_t> night100SwitchLifecycleCalls;
#define NIGHT100_LIFECYCLE_COUNT_CALL() night100SwitchLifecycleCalls.fetch_add(1,std::memory_order_relaxed)
#else
#define NIGHT100_LIFECYCLE_COUNT_CALL() ((void)0)
#endif
uint64_t night100SwitchHostLifecycleCallCount();
inline uint64_t night100SwitchHostServiceLifecycle(bool (*stop)(void*),void* data) {
    NIGHT100_LIFECYCLE_COUNT_CALL();
    if(__builtin_expect(!night100SwitchLifecyclePending.load(std::memory_order_relaxed),1))return 0;
    return night100SwitchHostServiceLifecycleSlow(stop,data);
}
void night100SwitchHostSetProducerQuiesce(void (*callback)(void*),void* data);
void night100SwitchHostSetFailureReporter(void (*callback)(const char*,void*),void* data);
void night100SwitchHostReportFailure(const char* message) noexcept;
