#include "emulator/core/iop_kernel.h"
#include "emulator/core/iop_memory.h"
#include <cstdint>
#include <iostream>
#include <stdexcept>

using namespace ps2x::iop::detail;
namespace
{
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
constexpr uint32_t returnSentinel = 0x1ffffcu;
int createThread(IopKernel& kernel, IopMemory& memory)
{
    memory.write32(0x1000 + 8, 0x20000); // entry
    memory.write32(0x1000 + 12, 0x400); // stack
    memory.write32(0x1000 + 16, 40); // priority
    IopCpuState control{}; control.gpr[4] = 0x1000;
    require(kernel.dispatchThreadImport(4, control, 0), "CreateThread import missing");
    require(int32_t(control.gpr[2]) > 0, "CreateThread failed");
    return int(control.gpr[2]);
}
void command(IopKernel& kernel, unsigned ordinal, int id, uint64_t cycles = 0)
{
    IopCpuState control{}; control.gpr[4] = uint32_t(id);
    require(kernel.dispatchThreadImport(ordinal, control, cycles), "thread control import missing");
    require(control.gpr[2] == 0, "thread control failed");
}
void idle(IopKernel& kernel, uint64_t cycle)
{
    require(kernel.beginNextReady(cycle) == nullptr, "blocked kernel unexpectedly has a runnable thread");
    require(kernel.beginNextReady(cycle) == nullptr, "cached idle kernel unexpectedly has a runnable thread");
}
}
int main()
{
    IopMemory memory;
    IopKernel kernel(memory);
    idle(kernel, 0);
    const int id = createThread(kernel, memory);
    idle(kernel, 0); // dormant; cache no-runnable before external StartThread
    command(kernel, 6, id);
    IopThread* thread = kernel.beginNextReady(1);
    require(thread && thread->id == id, "StartThread did not invalidate cached idle state");

    require(kernel.dispatchThreadImport(24, thread->cpu, 1), "SleepThread import missing");
    kernel.endTimeslice(*thread, returnSentinel);
    idle(kernel, 2);
    command(kernel, 25, id);
    thread = kernel.beginNextReady(3);
    require(thread && thread->id == id, "WakeupThread was hidden by cached idle state");

    kernel.delayCurrentUntil(1000, thread->cpu);
    kernel.endTimeslice(*thread, returnSentinel);
    idle(kernel, 4);
    require(kernel.nextWakeCycle(5000) == 1000, "cached delayed wake deadline changed");
    require(kernel.nextWakeCycle(999) == 999, "cached wake ignored an earlier external event");
    idle(kernel, 999);
    thread = kernel.beginNextReady(1000);
    require(thread && thread->id == id, "delay did not wake at its exact deadline");

    const int event = kernel.createInternalEventFlag(0, 0, 0);
    thread->cpu.gpr[4] = uint32_t(event); thread->cpu.gpr[5] = 4;
    thread->cpu.gpr[6] = 1; thread->cpu.gpr[7] = 0x1100;
    require(kernel.dispatchEventImport(10, thread->cpu), "WaitEventFlag import missing");
    kernel.endTimeslice(*thread, returnSentinel);
    idle(kernel, 1001);
    require(kernel.setInternalEventFlag(event, 2), "external event setter failed");
    idle(kernel, 1002);
    require(kernel.setInternalEventFlag(event, 4), "external event setter failed");
    thread = kernel.beginNextReady(1003);
    require(thread && thread->id == id, "external event signal was hidden by cached idle state");
    require(memory.read32(0x1100) == 6, "event wake result changed");

    memory.write32(0x1200 + 8, 0); memory.write32(0x1200 + 12, 1);
    IopCpuState control{}; control.gpr[4] = 0x1200;
    require(kernel.dispatchSemaphoreImport(4, control), "CreateSema import missing");
    const int sema = int(control.gpr[2]); require(sema > 0, "CreateSema failed");
    thread->cpu.gpr[4] = uint32_t(sema);
    require(kernel.dispatchSemaphoreImport(8, thread->cpu), "WaitSema import missing");
    kernel.endTimeslice(*thread, returnSentinel);
    idle(kernel, 1004);
    control.gpr[4] = uint32_t(sema);
    require(kernel.dispatchSemaphoreImport(6, control), "SignalSema import missing");
    thread = kernel.beginNextReady(1005);
    require(thread && thread->id == id, "semaphore signal was hidden by cached idle state");
    kernel.endTimeslice(*thread, returnSentinel);

    command(kernel, 29, id);
    idle(kernel, 1006);
    command(kernel, 31, id);
    thread = kernel.beginNextReady(1007);
    require(thread && thread->id == id, "ResumeThread was hidden by cached idle state");
    kernel.endTimeslice(*thread, returnSentinel);
    kernel.terminateThreadsInRange(0x20000, 0x100);
    kernel.cleanupDeadThreads();
    require(kernel.threadCount() == 0, "terminated thread was not cleaned up");
    idle(kernel, 1008);
    kernel.reset(); memory.reset();
    idle(kernel, 0);
    const int fresh = createThread(kernel, memory);
    command(kernel, 6, fresh);
    require(kernel.beginNextReady(1) != nullptr, "reset retained stale idle cache");
    std::cout << "PASS: IOP idle cache preserves start, sleep/wake, exact delay, external events, semaphore, resume, cleanup and reset\n";
}
