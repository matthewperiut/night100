#include "emulator/iop_emulator.h"
#include "ps2x/iop/iop_clock.h"
#include "emulator/core/iop_kernel.h"
#include "emulator/core/iop_memory.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
using namespace ps2x::iop;
using ps2x::iop::detail::IopEmulator;
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
class Host final : public IopHost
{
public:
    std::vector<uint8_t> guest = std::vector<uint8_t>(256*1024);
    uint64_t pcmFrames = 0;
    unsigned stopAfterPoll = 0;
    mutable unsigned stopPolls = 0;
    bool stopRequested() const noexcept override
    {
        return stopAfterPoll && ++stopPolls >= stopAfterPoll;
    }
    bool readGuest(uint32_t address, void* destination, size_t size) const override
    {
        if (address > guest.size() || size > guest.size() - address) return false;
        std::memcpy(destination, guest.data() + address, size); return true;
    }
    bool writeGuest(uint32_t address, const void* source, size_t size) override
    {
        if (address > guest.size() || size > guest.size() - address) return false;
        std::memcpy(guest.data() + address, source, size); return true;
    }
    bool zeroGuest(uint32_t address, size_t size) override
    {
        if (address > guest.size() || size > guest.size() - address) return false;
        std::memset(guest.data() + address, 0, size); return true;
    }
    bool normalizeGuestAddress(uint32_t address, uint32_t& result) const override { result = address; return address <= guest.size(); }
    uint32_t allocateIopHandle(IopHandleKind) override { return 1; }
    uint32_t allocateGuest(uint32_t, uint32_t) override { return 0; }
    void freeGuest(uint32_t) override {}
    void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
    void submitAudioSamples(const int16_t* samples, size_t frames) override
    {
        require(std::all_of(samples, samples + frames * 2, [](int16_t value) { return value == 0; }), "idle SPU generated unexpected sound");
        pcmFrames += frames;
    }
    std::string hostPath(HostPathKind) const override { return {}; }
    std::string translateGuestPath(std::string_view path) const override { return std::string(path); }
    uint64_t openHostFile(std::string_view) override { return 0; }
    bool hostFileSize(uint64_t, uint64_t&) const override { return false; }
    bool readHostFile(uint64_t, uint64_t, void*, size_t, size_t&) override { return false; }
    void closeHostFile(uint64_t) override {}
    int32_t memoryCard(const MemoryCardRequest&) override { return 0; }
    bool hasGuestFunction(uint32_t) const override { return false; }
    bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t*) override { return false; }
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

void verifyKernelContexts()
{
    using namespace ps2x::iop::detail;
    IopMemory memory; IopKernel kernel(memory);
    IopThread &interrupted = kernel.beginGuestCall();
    interrupted.cpu.gpr[4] = 0x11223344;
    IopThread &nested = kernel.beginGuestCall();
    require(kernel.currentThread() == &nested, "nested guest call borrowed interrupted context");
    nested.cpu.gpr[4] = 1000;
    require(kernel.dispatchThreadImport(33, nested.cpu, 0), "nested DelayThread import missing");
    require(nested.state == IopThreadState::Delay && interrupted.state == IopThreadState::Running,
            "nested wait changed interrupted thread state");
    require(interrupted.cpu.gpr[4] == 0x11223344, "nested call changed interrupted registers");
    nested.state = IopThreadState::Dead; kernel.cleanupDeadThreads();
    require(kernel.threadCount() == 2, "cleanup erased a live guest-call context");
    kernel.endGuestCall(nested.id, &interrupted);
    require(kernel.currentThread() == &interrupted, "nested context did not restore interrupted thread");
    kernel.endGuestCall(interrupted.id, nullptr);
    require(kernel.threadCount() == 0 && !kernel.currentThread(), "guest-call contexts leaked");
}
}

int main()
{
    verifyKernelContexts();
    // Execute real MIPS code through the synchronous loader/RPC executor.
    // Cancellation must unwind its invocation rather than exhaust the large
    // instruction budget or report a spurious driver wait timeout.
    for (unsigned mode : {3u, 1u})
    {
        Host host; IopEmulator emulator(host);
        Program program(mode);
        std::memcpy(host.guest.data() + 0x1000, program.image.data(), program.image.size());
        host.stopAfterPoll = mode == 3 ? 4 : 8;
        const auto cancelled = emulator.loadModuleBuffer(0x1000, nullptr, 0);
        require(host.stopPolls >= host.stopAfterPoll, "synchronous IOP call did not consult host cancellation");
        require(cancelled.moduleId < 0 && emulator.loadedModuleCount() == 0 && emulator.instructions() <= 3 * 256,
                "IOP cancellation ran past its scheduling quanta");
        require(emulator.threadCount() == 0, "cancelled invocation leaked a kernel thread");
        const auto stoppedCycles = emulator.cycles();
        emulator.runEeCycles(2949120);
        require(emulator.cycles() == stoppedCycles, "IOP advanced hardware after host cancellation");
        host.stopAfterPoll = 0;
        Program retry(0);
        std::memcpy(host.guest.data() + 0x1000, retry.image.data(), retry.image.size());
        const auto resumed = emulator.loadModuleBuffer(0x1000, nullptr, 0);
        require(resumed.moduleId > 0 && resumed.startResult == 0 && emulator.threadCount() == 0,
                "cancelled invocation failed to restore contexts for a subsequent call");
    }
    for (unsigned mode : {0u, 2u, 1u, 3u, 0u})
    {
        Host host; IopEmulator emulator(host);
        Program program(mode);
        std::memcpy(host.guest.data() + 0x1000, program.image.data(), program.image.size());
        bool failed = false;
        try
        {
            const auto result = emulator.loadModuleBuffer(0x1000, nullptr, 0);
            require(result.moduleId > 0 && result.startResult == 0, "real MIPS invocation did not return successfully");
        }
        catch (const std::runtime_error&) { failed = true; }
        require(failed == (mode == 1 || mode == 3), "wait/budget failure was hidden or valid invocation failed");
        require(emulator.threadCount() == 0, "returned/failed invocation left a dangling kernel thread");
        if (failed)
        {
            Program retry(0);
            std::memcpy(host.guest.data() + 0x1000, retry.image.data(), retry.image.size());
            const auto result = emulator.loadModuleBuffer(0x1000, nullptr, 0);
            require(result.moduleId > 0 && result.startResult == 0 && emulator.threadCount() == 0,
                    "exception unwind leaked image/thread/CPU context or prevented a subsequent guest call");
        }
        if (mode == 0)
        {
            std::array<uint32_t, 7> state{};
            require(emulator.readMemory(0x11040, state.data(), sizeof(state)), "cannot inspect real guest continuation");
            require(state[2] == 0 && state[3] == 0, "event/semaphore wait did not complete normally");
            require(state[4] == 1 && state[5] == 2 && state[6] == 0xfeed,
                    "guest continued before DMA interrupt, lost event bits, or failed to resume");
            require(emulator.cycles() >= 2 * 32768 + 36864, "event/semaphore/delay waits skipped actual IOP time");
            require(host.pcmFrames == emulator.cycles() / 768, "waiting scheduler failed to advance SPU PCM coherently");
        }
    }
    std::cout << "PASS: real MIPS DMA→IRQ→event/semaphore waits, delay continuation, nested contexts, ExitDeleteThread, bounded failures and cooperative synchronous cancellation\n";
}
