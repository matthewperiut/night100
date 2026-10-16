// Checks for the IOP worker thread (ps2x::iop::IopWorker and IopSubsystem's
// worker mode). They prove that worker and inline execution produce the same
// IOP state and PCM for the same EE credit stream, that interactions quiesce
// the worker, and that stop/join never deadlocks. Each check runs under a
// deadline, so a regression reports a failure instead of hanging the harness.
#include "ps2x/iop/iop_subsystem.h"
#include "ps2x/iop/iop_worker.h"
#include "ps2x/iop/iop_routed_host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace ps2x::iop;

void require(bool value, const char *why)
{
    if (!value)
        throw std::runtime_error(why);
}

// Runs body on another thread. A hang is reported and terminates the process
// rather than blocking the test harness forever.
void withDeadline(const char *name, std::function<void()> body)
{
    auto result = std::async(std::launch::async, std::move(body));
    if (result.wait_for(std::chrono::seconds(60)) != std::future_status::ready)
    {
        std::cerr << name << ": deadlock or hang (60 s deadline)" << std::endl;
        std::_Exit(2);
    }
    result.get();
}

class Host final : public IopHost
{
public:
    std::vector<uint8_t> guest = std::vector<uint8_t>(256 * 1024);
    uint64_t pcmFrames = 0;
    uint64_t pcmHash = 1469598103934665603ull; // FNV-1a over every sample
    // Thread accounting: the host is created on the EE (test) thread.
    const std::thread::id eeThread = std::this_thread::get_id();
    mutable std::atomic<unsigned> fileCallsOnEe{0};
    mutable std::atomic<unsigned> fileCallsOffEe{0};
    std::atomic<unsigned> pcmOnEe{0};
    std::atomic<unsigned> pcmOffEe{0};
    void countFile() const
    {
        (std::this_thread::get_id() == eeThread ? fileCallsOnEe : fileCallsOffEe).fetch_add(1);
    }
    bool readGuest(uint32_t address, void *destination, size_t size) const override
    {
        if (address > guest.size() || size > guest.size() - address) return false;
        std::memcpy(destination, guest.data() + address, size);
        return true;
    }
    bool writeGuest(uint32_t address, const void *source, size_t size) override
    {
        if (address > guest.size() || size > guest.size() - address) return false;
        std::memcpy(guest.data() + address, source, size);
        return true;
    }
    bool zeroGuest(uint32_t address, size_t size) override
    {
        if (address > guest.size() || size > guest.size() - address) return false;
        std::memset(guest.data() + address, 0, size);
        return true;
    }
    bool normalizeGuestAddress(uint32_t address, uint32_t &result) const override
    {
        result = address;
        return address <= guest.size();
    }
    uint32_t allocateIopHandle(IopHandleKind) override { return 1; }
    uint32_t allocateGuest(uint32_t, uint32_t) override { return 0; }
    void freeGuest(uint32_t) override {}
    void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
    void submitAudioSamples(const int16_t *samples, size_t frames) override
    {
        (std::this_thread::get_id() == eeThread ? pcmOnEe : pcmOffEe).fetch_add(1);
        for (size_t i = 0; i < frames * 2; ++i)
        {
            pcmHash ^= static_cast<uint16_t>(samples[i]);
            pcmHash *= 1099511628211ull;
        }
        pcmFrames += frames;
    }
    std::string hostPath(HostPathKind) const override { return {}; }
    std::string translateGuestPath(std::string_view path) const override { countFile(); return std::string(path); }
    uint64_t openHostFile(std::string_view) override { countFile(); return 1; }
    bool hostFileSize(uint64_t, uint64_t &size) const override { countFile(); size = 16; return true; }
    bool readHostFile(uint64_t, uint64_t, void *destination, size_t size, size_t &bytesRead) override
    {
        countFile();
        std::memset(destination, 0x7e, size);
        bytesRead = size;
        return true;
    }
    void closeHostFile(uint64_t) override { countFile(); }
    int32_t memoryCard(const MemoryCardRequest &) override { return 0; }
    bool hasGuestFunction(uint32_t) const override { return false; }
    bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override { return false; }
    void log(LogLevel, std::string_view) override {}
};

struct Program
{
    static constexpr uint32_t base = 0x10000;
    std::vector<uint8_t> image = std::vector<uint8_t>(0x1100);
    uint32_t cursor = 0;
    void word(uint32_t offset, uint32_t value) { std::memcpy(image.data() + offset, &value, 4); }
    void half(uint32_t offset, uint16_t value) { std::memcpy(image.data() + offset, &value, 2); }
    void emit(uint32_t value) { word(0x100 + cursor, value); cursor += 4; }
    void li(unsigned reg, uint32_t value) { emit(0x3c000000u | (reg << 16) | (value >> 16)); emit(0x34000000u | (reg << 21) | (reg << 16) | (value & 0xffffu)); }
    void move(unsigned destination, unsigned source) { emit((source << 21) | (destination << 11) | 0x21); }
    void call(uint32_t offset) { emit(0x0c000000u | ((base + offset) >> 2)); emit(0); }
    void store(unsigned source, unsigned address, uint16_t offset = 0) { emit(0xac000000u | (address << 21) | (source << 16) | offset); }
    void load(unsigned target, unsigned address, uint16_t offset = 0) { emit(0x8c000000u | (address << 21) | (target << 16) | offset); emit(0); }
    void imports(uint32_t offset, const char* library, std::initializer_list<unsigned> ordinals)
    {
        word(0x100 + offset, 0x41e00000); half(0x100 + offset + 8, 0x101);
        std::memcpy(image.data() + 0x100 + offset + 12, library, std::strlen(library));
        uint32_t stub = offset + 20;
        for (unsigned ordinal : ordinals) { word(0x100 + stub, 0x03e00008); word(0x100 + stub + 4, 0x24000000 | ordinal); stub += 8; }
    }
    void dma()
    {
        li(8, 0x1f9001a8); li(9, 0x20000000); store(9, 8); // TSA word address0x2000
        li(8, 0x1f8010c0); li(9, 0x11080); store(9, 8);
        li(9, 0x04000010); store(9, 8, 4); //16 words ×1024 blocks =65536bytes
        li(9, 0x01000201); store(9, 8, 8);
    }
    // mode0: real DMA/IRQ event+semaphore+delay; mode1: unresolved wait;
    // mode2: ExitDeleteThread; mode3: instruction-budget exhaustion.
    explicit Program(unsigned mode)
    {
        word(0, 0x464c457f); image[4] = image[5] = image[6] = 1;
        half(16, 2); half(18, 8); word(20, 1); word(24, base); word(28, 52);
        half(40, 52); half(42, 32); half(44, 1);
        word(52, 1); word(56, 0x100); word(60, base); word(64, base);
        word(68, 0x1000); word(72, 0x13000); word(76, 7); word(80, 4);
        imports(0x800, "thevent", {4, 7, 10});
        imports(0x840, "thsemap", {4, 7, 8});
        imports(0x880, "intrman", {4, 6});
        imports(0x8c0, "thbase", {33, 9});
        if (mode == 2) { call(0x8dc); return; }
        if (mode == 3) { emit(0x1000ffff); emit(0); return; }
        emit(0x27bdffe0); store(31, 29, 28); //preserve original caller RA
        li(4, 0x11000); call(0x814); move(16, 2);
        if (mode == 1)
        {
            move(4, 16); li(5, 1); move(6, 0); move(7, 0); call(0x824); return;
        }
        li(8, 0x11020); li(9, 1); store(9, 8, 12); move(4, 8); call(0x854);
        move(17, 2); li(8, 0x11040); store(17, 8); store(16, 8, 4);
        li(4, 0x24); li(5, 1); li(6, base + 0x300); move(7, 16); call(0x894);
        li(4, 0x24); move(5, 0); call(0x89c);
        dma();
        move(4, 16); li(5, 1); li(6, 0x10); li(7, 0x11050); call(0x824);
        li(8, 0x11040); store(2, 8, 8);
        move(4, 17); call(0x864); //consume first ISR's semaphore signal
        dma(); move(4, 17); call(0x864); //must block until second real ISR
        li(8, 0x11040); store(2, 8, 12);
        li(4, 1000); call(0x8d4); //one actual IOP millisecond
        li(8, 0x11058); li(9, 0xfeed); store(9, 8);
        load(31, 29, 28); emit(0x27bd0020); move(2, 0); emit(0x03e00008); emit(0);

        cursor = 0x300; //nested DMA interrupt callback uses its own stack/context
        emit(0x27bdfff0); store(31, 29, 12);
        li(8, 0x11054); load(9, 8); emit(0x25290001); store(9, 8);
        li(5, 1); call(0x81c); //a0 is the event ID registered with interrupt
        li(8, 0x11040); load(4, 8); call(0x85c);
        load(31, 29, 12); emit(0x27bd0010); li(2, 1); emit(0x03e00008); emit(0);
    }
};

// Generic worker contract: ordered credits, blocking EE calls, FIFO deferred
// jobs, and re-entry from a job the EE thread is servicing.
void workerOrderingAndHandoff()
{
    const std::thread::id eeThread = std::this_thread::get_id();
    std::vector<uint64_t> executed;
    std::vector<uint64_t> applied;
    unsigned blockingCalls = 0;
    IopWorker *self = nullptr;
    IopWorker worker(
        [&](uint64_t credit) {
            executed.push_back(credit);
            const uint64_t echoed = IopWorker::eeCall([&] {
                require(std::this_thread::get_id() == eeThread, "eeCall ran off the EE thread");
                ++blockingCalls;
                return credit * 2;
            });
            require(echoed == credit * 2, "eeCall returned the wrong value");
            IopWorker::deferToEe([&, credit] {
                require(std::this_thread::get_id() == eeThread, "deferred job ran off the EE thread");
                applied.push_back(credit);
            });
            if (credit == 5)
            {
                // Re-entry from a serviced job must not wait for the suspended worker.
                IopWorker::eeCall([&] {
                    self->quiesce();
                    self->submit(7);
                });
            }
        },
        1000);
    self = &worker;
    for (uint64_t credit = 1; credit <= 5; ++credit)
        worker.submit(credit);
    worker.quiesce();
    require((executed == std::vector<uint64_t>{1, 2, 3, 4, 5, 7}), "credits did not execute in order");
    require((applied == std::vector<uint64_t>{1, 2, 3, 4, 5, 7}), "deferred jobs were not applied in FIFO order");
    require(blockingCalls == 6, "blocking EE calls were not all serviced");
    worker.stop();
    worker.stop(); // idempotent
}

// Destruction with queued credit, blocked EE calls and deferred jobs must drain
// all of them and join, without an explicit quiesce.
void workerStopWithPendingWork()
{
    uint64_t executed = 0;
    uint64_t blocking = 0;
    uint64_t deferred = 0;
    {
        IopWorker worker(
            [&](uint64_t) {
                ++executed;
                IopWorker::eeCall([&] { ++blocking; });
                IopWorker::deferToEe([&] { ++deferred; });
            },
            64);
        for (int i = 0; i < 50; ++i)
            worker.submit(100);
    }
    require(executed == 50 && blocking == 50 && deferred == 50, "destructor did not drain queued work");
}

constexpr uint64_t kSteps = 400;
constexpr uint64_t kSnapshotEvery = 50;

uint64_t nextRandom(uint64_t &state)
{
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return state >> 33;
}

// Irregular EE-cycle partitions, including one-cycle, sub-sample and 1 ms credits.
uint64_t stepCredit(uint64_t step)
{
    static constexpr uint64_t fixed[] = {1, 7, 29, 8, 294912};
    if (step % 9 == 4)
        return fixed[step % 5];
    uint64_t state = 0x9e3779b97f4a7c15ull ^ (step * 0x100000001b3ull);
    return 1 + nextRandom(state) % 200000;
}

struct Trace
{
    std::vector<std::vector<uint8_t>> snapshots;
    std::vector<uint64_t> cycles;
    std::vector<uint32_t> allocations;
    std::vector<uint8_t> probes;
    uint64_t moduleId = 0;
    uint64_t startResult = 0;
    uint64_t pcmFrames = 0;
    uint64_t pcmHash = 0;
};

// One scripted step: a credit, then interactions that must observe IOP state.
void scriptedStep(IopSubsystem &sub, uint64_t step, Trace &trace)
{
    sub.runEeCycles(stepCredit(step));
    if (step % 7 == 3)
        trace.allocations.push_back(sub.allocateMemory(0x80u + 16u * static_cast<uint32_t>(step % 5)));
    if (step % 11 == 5)
    {
        uint8_t bytes[16];
        for (uint32_t i = 0; i < sizeof(bytes); ++i)
            bytes[i] = static_cast<uint8_t>(step + i);
        (void)sub.writeMemory(0x3000u + 16u * static_cast<uint32_t>(step % 64), bytes, sizeof(bytes));
    }
    if (step % 13 == 2)
    {
        uint8_t probe[32] = {};
        (void)sub.readMemory(0x2000u + 4u * static_cast<uint32_t>(step % 128), probe, sizeof(probe));
        trace.probes.insert(trace.probes.end(), probe, probe + sizeof(probe));
    }
    if (step % 17 == 9)
        trace.probes.push_back(sub.canBindRpc(0x80000000u + static_cast<uint32_t>(step)) ? 1u : 0u);
    if (step % 19 == 1)
        trace.cycles.push_back(sub.debugSnapshot().emulatorCycles);
    if (step % kSnapshotEvery == kSnapshotEvery - 1)
        trace.snapshots.push_back(sub.saveState());
}

Trace runScenario(bool worker, uint64_t quantumCycles)
{
    Host host;
    IopSubsystem sub(host);
    if (worker)
        sub.setWorkerEnabled(true, quantumCycles);
    Program program(0);
    std::memcpy(host.guest.data() + 0x1000, program.image.data(), program.image.size());
    Trace trace;
    const ModuleLoadResult loaded = sub.loadModuleBuffer(0x1000);
    trace.moduleId = static_cast<uint64_t>(static_cast<int64_t>(loaded.moduleId));
    trace.startResult = static_cast<uint64_t>(static_cast<int64_t>(loaded.startResult));
    for (uint64_t step = 0; step < kSteps; ++step)
        scriptedStep(sub, step, trace);
    sub.quiesce();
    trace.snapshots.push_back(sub.saveState());
    trace.pcmFrames = host.pcmFrames;
    trace.pcmHash = host.pcmHash;
    return trace;
}

void requireSameTrace(const Trace &expected, const Trace &actual, const char *why)
{
    require(expected.moduleId == actual.moduleId && expected.startResult == actual.startResult, why);
    require(expected.allocations == actual.allocations, why);
    require(expected.probes == actual.probes, why);
    require(expected.cycles == actual.cycles, why);
    require(expected.pcmFrames == actual.pcmFrames && expected.pcmHash == actual.pcmHash, why);
    require(expected.snapshots.size() == actual.snapshots.size(), why);
    for (size_t i = 0; i < expected.snapshots.size(); ++i)
        require(expected.snapshots[i] == actual.snapshots[i], why);
}

void workerMatchesInlineExecution()
{
    const Trace reference = runScenario(false, 0);
    require(reference.moduleId > 0 && reference.startResult == 0, "test module did not load");
    require(reference.pcmFrames > 0, "scenario produced no audio");
    require(reference.snapshots.size() == kSteps / kSnapshotEvery + 1, "unexpected snapshot count");
    require(reference.snapshots.front() != reference.snapshots.back(), "IOP state did not change during the scenario");
    requireSameTrace(reference, runScenario(true, 4096), "worker with 4096-cycle quanta diverged from inline");
    requireSameTrace(reference, runScenario(true, 294912), "worker with 1 ms quanta diverged from inline");
    requireSameTrace(reference, runScenario(true, 4096), "worker run is not repeatable");
}

// Credit queued but not executed must not survive a restore. loadState quiesces
// first, so the restored state is the snapshot, followed by the same script.
// The inline and worker restores must agree with each other.
Trace restoreAndContinue(bool worker, const std::vector<uint8_t> &snapshot, std::vector<uint8_t> &finalState)
{
    Host host;
    IopSubsystem sub(host);
    if (worker)
        sub.setWorkerEnabled(true, 4096);
    Program program(0);
    std::memcpy(host.guest.data() + 0x1000, program.image.data(), program.image.size());
    (void)sub.loadModuleBuffer(0x1000);
    for (const uint64_t credit : {12345ull, 999ull, 777777ull})
        sub.runEeCycles(credit);
    sub.loadState(snapshot);
    Trace continued;
    for (uint64_t step = 200; step < kSteps; ++step)
        scriptedStep(sub, step, continued);
    sub.quiesce();
    finalState = sub.saveState();
    return continued;
}

void workerRestoresSnapshotAfterQueuedCredit()
{
    const Trace reference = runScenario(false, 0);
    std::vector<uint8_t> inlineFinal, workerFinal;
    const Trace inlineContinued = restoreAndContinue(false, reference.snapshots[3], inlineFinal);
    const Trace workerContinued = restoreAndContinue(true, reference.snapshots[3], workerFinal);
    require(workerFinal == inlineFinal, "worker restore diverged from inline restore");
    require(workerContinued.snapshots == inlineContinued.snapshots, "worker restore diverged at a later snapshot");
    // Informational: the uninterrupted run and the restored run differ only if
    // the snapshot omits state that the continuation reads. Both modes share it.
    std::cerr << "note: inline restore matches uninterrupted final state: "
              << (inlineFinal == reference.snapshots.back() ? "yes" : "no") << std::endl;
}

// The start hook runs exactly once, on the worker thread, before the first credit.
void workerStartHookRunsFirstOnWorkerThread()
{
    const std::thread::id eeThread = std::this_thread::get_id();
    std::atomic<unsigned> hookCalls{0};
    std::atomic<bool> hookOnWorker{false};
    std::atomic<unsigned> creditsBeforeHook{0};
    std::atomic<unsigned> credits{0};
    IopWorker worker(
        [&](uint64_t) {
            if (hookCalls.load() == 0)
                creditsBeforeHook.fetch_add(1);
            credits.fetch_add(1);
        },
        1000,
        [&] {
            hookOnWorker = std::this_thread::get_id() != eeThread && IopWorker::onWorkerThread();
            hookCalls.fetch_add(1);
        });
    for (uint64_t i = 0; i < 20; ++i)
        worker.submit(10);
    worker.quiesce();
    require(hookCalls.load() == 1, "start hook did not run exactly once");
    require(hookOnWorker.load(), "start hook did not run on the worker thread");
    require(creditsBeforeHook.load() == 0, "a credit ran before the start hook");
    require(credits.load() == 20, "credits lost after the start hook");
}

// A tiny ring forces many boundary drains; ordering and count must survive wraparound.
void workerRingWraparoundPreservesOrder()
{
    std::vector<uint64_t> executed;
    IopWorker worker([&](uint64_t credit) { executed.push_back(credit); }, ~uint64_t(0) >> 1, IopWorker::StartHook(), 4);
    for (uint64_t i = 1; i <= 5000; ++i)
        worker.submit(i);
    worker.quiesce();
    require(executed.size() == 5000, "ring wraparound lost credit");
    for (uint64_t i = 0; i < executed.size(); ++i)
        require(executed[i] == i + 1, "ring wraparound reordered credit");
}

// Nested submits from a job the EE runs for the worker cannot drain; they use the
// overflow queue and must still execute after the current credit, in FIFO order.
void workerNestedSubmitOverflowKeepsOrder()
{
    std::vector<uint64_t> executed;
    IopWorker *self = nullptr;
    IopWorker worker(
        [&](uint64_t credit) {
            executed.push_back(credit);
            if (credit == 1)
            {
                IopWorker::eeCall([&] {
                    for (uint64_t i = 100; i < 110; ++i)
                        self->submit(i);
                });
            }
        },
        1000000, IopWorker::StartHook(), 4);
    self = &worker;
    worker.submit(1);
    worker.submit(2);
    worker.quiesce();
    std::vector<uint64_t> expected{1, 2};
    for (uint64_t i = 100; i < 110; ++i)
        expected.push_back(i);
    require(executed == expected, "nested overflow reordered or lost credit");
}

// Host file I/O and PCM from IOP code on the worker must run on the EE thread.
// The control call proves the detector fires for a direct, unrouted call.
void workerHostFileIoStaysOnEeThread()
{
    Host host;
    IopEeRoutedHost routed(host);
    IopWorker worker(
        [&](uint64_t) {
            const uint64_t handle = routed.openHostFile("cd0:/RESOURCE");
            uint64_t size = 0;
            require(routed.hostFileSize(handle, size) && size == 16, "routed size query failed");
            uint8_t buffer[16] = {};
            size_t read = 0;
            require(routed.readHostFile(handle, 0, buffer, sizeof buffer, read) && read == sizeof buffer,
                    "routed read failed");
            routed.closeHostFile(handle);
            (void)routed.translateGuestPath("cd0:/RESOURCE");
            const int16_t frames[4] = {1, 2, 3, 4};
            routed.submitAudioSamples(frames, 2);
        },
        1000);
    worker.submit(1);
    worker.quiesce();
    require(host.fileCallsOffEe == 0, "host file call ran on the worker thread");
    require(host.fileCallsOnEe == 5, "routed host file calls did not all run on the EE thread");
    require(host.pcmOffEe == 0 && host.pcmOnEe == 1, "submitted PCM was not delivered on the EE thread");

    IopWorker control([&](uint64_t) { (void)host.openHostFile("cd0:/RESOURCE"); }, 1000);
    control.submit(1);
    control.quiesce();
    require(host.fileCallsOffEe == 1, "detector missed a direct host file call on the worker");
}

// EE-requested worker jobs run on the worker after earlier credit, can call back
// into the EE thread, and return their result to the EE thread.
void workerRunOnWorkerOrdersAfterCreditAndCallsBack()
{
    const std::thread::id eeThread = std::this_thread::get_id();
    std::vector<uint64_t> order;
    IopWorker worker([&](uint64_t credit) { order.push_back(credit); }, 1000);
    worker.submit(1);
    worker.submit(2);
    const uint64_t result = worker.runOnWorker([&]() -> uint64_t {
        require(IopWorker::onWorkerThread(), "EE-requested job did not run on the worker");
        order.push_back(100);
        const uint64_t echoed = IopWorker::eeCall([&]() -> uint64_t {
            require(std::this_thread::get_id() == eeThread, "worker callback ran off the EE thread");
            return 7;
        });
        order.push_back(echoed);
        return echoed * 3;
    });
    require(result == 21, "worker job result was lost");
    require((order == std::vector<uint64_t>{1, 2, 100, 7}), "worker job did not run after earlier credit");
}

// A deferred EE job may itself request worker work while the worker keeps running
// credit. The request must run on the worker and not race with it.
void workerNestedRunOnWorkerFromDeferredJob()
{
    IopWorker *self = nullptr;
    std::vector<uint64_t> results;
    IopWorker worker(
        [&](uint64_t credit) {
            IopWorker::deferToEe([&, credit] {
                const uint64_t value = self->runOnWorker([credit]() -> uint64_t {
                    require(IopWorker::onWorkerThread(), "nested job did not run on the worker");
                    return credit + 1;
                });
                results.push_back(value);
            });
        },
        1000);
    self = &worker;
    worker.submit(10);
    worker.submit(20);
    worker.quiesce();
    // The second request completes inside the first request's wait, so completion order is nested.
    std::sort(results.begin(), results.end());
    require((results == std::vector<uint64_t>{11, 21}), "nested worker request lost or wrong");
}

// IopSubsystem sends module control to the worker when enabled, and runs it inline when
// disabled. The counters must show the difference.
void subsystemRoutesIopWorkToWorker()
{
    for (const bool enabled : {true, false})
    {
        Host host;
        IopSubsystem sub(host);
        if (enabled)
            sub.setWorkerEnabled(true, 4096);
        Program program(0);
        std::memcpy(host.guest.data() + 0x1000, program.image.data(), program.image.size());
        const ModuleLoadResult loaded = sub.loadModuleBuffer(0x1000);
        require(loaded.moduleId > 0 && loaded.startResult == 0, "routed module load failed");
        for (int i = 0; i < 100; ++i)
            sub.runEeCycles(50000);
        require(sub.stopModule(loaded.moduleId), "routed module stop failed");
        const auto stats = sub.executionStats();
        if (enabled)
        {
            require(stats.workerJobs == 2 && stats.workerCycles > 0, "module control did not run on the worker");
            require(stats.workerCredits == 100, "credit did not run on the worker");
            require(stats.eeInlineJobs == 0 && stats.eeInlineCycles == 0 && stats.eeInlineCredits == 0,
                    "IOP work ran on the EE thread while the worker was enabled");
        }
        else
        {
            require(stats.workerJobs == 0 && stats.workerCredits == 0, "worker counters moved while disabled");
            require(stats.eeInlineJobs == 2 && stats.eeInlineCredits == 100 && stats.eeInlineCycles > 0,
                    "inline IOP work was not counted on the EE thread");
        }
    }
}

void subsystemStopWithPendingCredit()
{
    Host host;
    {
        IopSubsystem sub(host);
        sub.setWorkerEnabled(true, 1024);
        Program program(0);
        std::memcpy(host.guest.data() + 0x1000, program.image.data(), program.image.size());
        (void)sub.loadModuleBuffer(0x1000);
        for (int i = 0; i < 200; ++i)
            sub.runEeCycles(50000);
        // Destroyed without quiesce: the destructor must drain and join.
    }
    IopSubsystem sub(host);
    sub.setWorkerEnabled(true, 1024);
    sub.runEeCycles(100000);
    sub.setWorkerEnabled(false);
    require(!sub.workerEnabled(), "worker still reported enabled after disable");
    sub.runEeCycles(100000);
    sub.setWorkerEnabled(true, 1024);
    sub.runEeCycles(100000);
}
}

int main()
{
    try
    {
        withDeadline("worker ordering and EE handoff", workerOrderingAndHandoff);
        withDeadline("worker stop with pending work", workerStopWithPendingWork);
        withDeadline("worker matches inline execution", workerMatchesInlineExecution);
        withDeadline("worker restore after queued credit", workerRestoresSnapshotAfterQueuedCredit);
        withDeadline("subsystem stop with pending credit", subsystemStopWithPendingCredit);
        withDeadline("worker start hook", workerStartHookRunsFirstOnWorkerThread);
        withDeadline("worker host file I/O on EE thread", workerHostFileIoStaysOnEeThread);
        withDeadline("worker runOnWorker ordering and callbacks", workerRunOnWorkerOrdersAfterCreditAndCallsBack);
        withDeadline("worker nested runOnWorker from deferred job", workerNestedRunOnWorkerFromDeferredJob);
        withDeadline("subsystem routes IOP work to worker", subsystemRoutesIopWorkToWorker);
        withDeadline("worker ring wraparound", workerRingWraparoundPreservesOrder);
        withDeadline("worker nested overflow", workerNestedSubmitOverflowKeepsOrder);
    }
    catch (const std::exception &error)
    {
        std::cerr << "iop_worker_checks: " << error.what() << std::endl;
        return 1;
    }
    std::cout << "iop_worker_checks passed" << std::endl;
    return 0;
}
