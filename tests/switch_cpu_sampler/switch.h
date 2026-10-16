#pragma once
#include <cstdint>
using Handle=uint32_t;
using Result=uint32_t;
using u64=uint64_t;
#define R_FAILED(value) ((value)!=0)
enum ThreadActivity {ThreadActivity_Runnable=0,ThreadActivity_Paused=1};
struct CpuRegister {uint64_t x;};
struct ThreadContext {uint64_t lr,sp;CpuRegister pc;};
struct Thread {Handle handle;};
Thread* threadGetSelf();
bool envIsSyscallHinted(unsigned);
uint64_t armGetSystemTick();
Result svcSetThreadActivity(Handle,ThreadActivity);
Result svcGetThreadContext3(ThreadContext*,Handle);
[[noreturn]] void svcExitProcess();
