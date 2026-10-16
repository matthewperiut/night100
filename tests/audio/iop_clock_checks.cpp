#include "emulator/iop_emulator.h"
#include "ps2x/iop/iop_clock.h"

#include <algorithm>
#include <chrono>
#include <thread>
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
    std::vector<uint8_t> guest = std::vector<uint8_t>(65536);
    uint64_t pcmFrames = 0;
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

// Construct a tiny executable MIPS IRX in EE memory. Its 1,024 NOPs and
// original return instructions must execute synchronously during module load.
// This represents the same accounting path used by real RPC/IRQ callbacks.
void prepareModule(Host& host)
{
    constexpr uint32_t image = 0x1000, codeOffset = 0x100, load = 0x10000;
    constexpr unsigned nops = 1024;
    auto word = [&](uint32_t offset, uint32_t value) { std::memcpy(host.guest.data() + image + offset, &value, 4); };
    auto half = [&](uint32_t offset, uint16_t value) { std::memcpy(host.guest.data() + image + offset, &value, 2); };
    word(0, 0x464c457f); host.guest[image + 4] = 1; host.guest[image + 5] = 1; host.guest[image + 6] = 1;
    half(16, 2); half(18, 8); word(20, 1); word(24, load); word(28, 52);
    half(40, 52); half(42, 32); half(44, 1);
    word(52, 1); word(56, codeOffset); word(60, load); word(64, load);
    word(68, (nops + 3) * 4); word(72, (nops + 3) * 4); word(76, 5); word(80, 4);
    word(codeOffset + nops * 4, 0x24020000); // addiu v0,zero,0: resident
    word(codeOffset + (nops + 1) * 4, 0x03e00008); // jr ra
    word(codeOffset + (nops + 2) * 4, 0); // delay slot
}
}
int main()
{
    // Bracket every fast poll with the platform steady clock: switching to
    // mach_absolute_time would fail this check after a system sleep, because
    // its epoch excludes sleep while the game's continuous clock includes it.
    auto standardNow = [] {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    };
    // An independent 128-bit oracle covers overflow boundaries and arbitrary
    // raw counter values. Production uses only 64-bit operations.
    auto checkTicks=[](uint64_t ticks) {
        const auto wide=static_cast<unsigned __int128>(ticks)*625/12;
        const auto expected=wide>UINT64_MAX?UINT64_MAX:uint64_t(wide);
        require(IopHostClock::systemTicksToNanoseconds(ticks)==expected,
                "SDK system-counter conversion lost precision or overflowed");
    };
    for(uint64_t boundary:{uint64_t(0),uint64_t(19200000),uint64_t(1)<<31,uint64_t(1)<<32,
            UINT64_MAX/625,uint64_t(static_cast<unsigned __int128>(UINT64_MAX)*12/625),UINT64_MAX}) {
        checkTicks(boundary);
        if(boundary)checkTicks(boundary-1);
        if(boundary<UINT64_MAX)checkTicks(boundary+1);
    }
    uint64_t random=0x1920000062512;
    for(unsigned n=0;n<10000;++n){random^=random<<13;random^=random>>7;random^=random<<17;checkTicks(random);}
    uint64_t previous = 0;
    for (unsigned i = 0; i < 10000; ++i)
    {
        const uint64_t before = standardNow();
        const uint64_t fast = IopHostClock::monotonicNanoseconds();
        const uint64_t after = standardNow();
#if !defined(__SWITCH__)
        require(fast >= before && fast <= after, "fast clock changed the game's continuous clock domain");
#else
        (void)before;(void)after; // Switch IOP uses uptime rather than libc's wall-time epoch.
#endif
        require(fast >= previous, "fast clock moved backwards");
        previous = fast;
    }
    const uint64_t standardStart0=standardNow();
    const uint64_t idleStart = IopHostClock::monotonicNanoseconds();
    const uint64_t standardStart1=standardNow();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const uint64_t standardEnd0=standardNow();
    const uint64_t elapsed=IopHostClock::monotonicNanoseconds()-idleStart;
    const uint64_t standardEnd1=standardNow();
    require(elapsed>=2000000,"fast clock omitted host idle time");
    require(elapsed+104>=standardEnd0-standardStart1 && elapsed<=standardEnd1-standardStart0+104,
            "counter clock changed elapsed rate relative to platform clock");

    Host host;
    IopEmulator emulator(host);
    prepareModule(host);
    const auto loaded = emulator.loadModuleBuffer(0x1000, nullptr, 0);
    require(loaded.moduleId > 0 && loaded.startResult == 0, "synthetic IRX did not execute");
    const uint64_t synchronousCycles = emulator.cycles();
    require(synchronousCycles >= 1027, "module load skipped real IOP instructions");
    require(host.pcmFrames == synchronousCycles / 768, "synchronous work did not advance SPU hardware");

    emulator.runEeCycles(7);
    require(emulator.cycles() == synchronousCycles, "fractional EE cycles advanced an ahead-of-deadline IOP");
    emulator.runEeCycles(1);
    require(emulator.cycles() == synchronousCycles, "synchronous IOP work was charged twice");
    emulator.runEeCycles((synchronousCycles - 1) * 8);
    require(emulator.cycles() == synchronousCycles, "EE deadline catch-up repeated synchronous work");
    emulator.runEeCycles(8);
    require(emulator.cycles() == synchronousCycles + 1, "IOP did not resume after exhausting synchronous credit");

    constexpr uint64_t second = 36864000;
    emulator.runEeCycles((second - synchronousCycles - 1) * 8);
    require(emulator.cycles() == second, "idle IOP missed the absolute EE deadline");
    require(host.pcmFrames == 48000, "one guest second did not produce exactly 48000 PCM frames");

    // Advance in awkward fragments to exercise EE / 8 carry and idle fast paths.
    emulator.reset(); host.pcmFrames = 0;
    uint64_t supplied = 0;
    for (unsigned i = 0; i < 30000; ++i)
    {
        const uint64_t delta = (i % 13) + 1;
        supplied += delta;
        emulator.runEeCycles(delta);
        require(emulator.cycles() == supplied / 8, "fragmented EE advances lost or duplicated cycles");
        require(host.pcmFrames == (supplied / 8) / 768, "idle fast path lost or duplicated audio samples");
    }
    // A native GPU/asset operation can consume real time while accounting for
    // very few translated EE instructions. Its audio and original IOP drivers
    // must still advance, without later replaying this time as EE catches up.
    IopHostClock clock;
    require(clock.advance(73, 0) == 73, "reference clock changed deterministic EE cycles");
    constexpr uint64_t epoch = 100000000000u;
    clock.setRealtime(true); clock.reset(epoch);
    emulator.reset(); host.pcmFrames = 0;
    auto advanceHost = [&](uint64_t ee, uint64_t ns) {
        const uint64_t credit = clock.advance(ee, epoch + ns);
        require(credit <= IopHostClock::MaximumAdvance, "host pause requested unbounded IOP catch-up");
        emulator.runEeCycles(credit);
        return credit;
    };
    require(advanceHost(1, 250000000) == IopHostClock::MaximumAdvance,
            "expensive native work did not advance device time");
    unsigned slices = 1;
    while (advanceHost(1, 250000000)) require(++slices < 100, "host clock never caught up");
    require(emulator.cycles() == second / 4 && host.pcmFrames == 12000,
            "250ms native work did not produce 250ms of real SPU hardware output");
    advanceHost(294912000, 250000000);
    require(emulator.cycles() == second / 4 && host.pcmFrames == 12000,
            "translated EE catch-up duplicated already elapsed device time");
    slices = 0;
    while (advanceHost(1, 1000000000)) require(++slices < 200, "host clock failed to resume");
    require(emulator.cycles() == second && host.pcmFrames == 48000,
            "one host second did not produce exactly 48000 PCM frames");
    require(advanceHost(1, 999999999) == 0, "backwards host timestamp rewound device time");
    clock.reset(epoch); emulator.reset(); host.pcmFrames = 0;
    require(advanceHost(1, 500000) == 0, "sub-millisecond clock batching changed");
    require(advanceHost(1, 1000000) == IopHostClock::MinimumAdvance,
            "reset retained the previous host clock deadline");
    require(host.pcmFrames == 48, "millisecond service lost hardware samples");
    require(advanceHost(1, UINT64_MAX - epoch) <= IopHostClock::MaximumAdvance,
            "extreme host timestamp overflowed the bounded clock conversion");
    // Exclude completed background time, but retain active pre-suspend debt
    // and fractional batching exactly. A resume must not synthesize sleep PCM.
    {
        constexpr uint64_t background = 3600ull * 1000000000;
        IopHostClock reference, resumed;
        reference.setRealtime(true); resumed.setRealtime(true);
        reference.reset(epoch); resumed.reset(epoch);
        require(reference.advance(1, epoch + 20000000) == IopHostClock::MaximumAdvance &&
                resumed.advance(1, epoch + 20000000) == IopHostClock::MaximumAdvance,
                "background fixture did not retain pending active IOP time");
        resumed.excludeHostTime(background);
        for (unsigned n = 0; n < 10; ++n)
            require(resumed.advance(1, epoch + background + 20000000) == reference.advance(1, epoch + 20000000),
                    "background exclusion lost pre-suspend debt or retained sleeping time");
        resumed.reset(epoch); resumed.excludeHostTime(background);
        require(resumed.advance(1, epoch + background + 999999) == 0,
                "background exclusion changed fractional millisecond batching");
        emulator.reset(); host.pcmFrames = 0;
        const auto credit = resumed.advance(1, epoch + background + 1000000);
        require(credit == IopHostClock::MinimumAdvance, "resumed IOP charged sleep as catch-up work");
        emulator.runEeCycles(credit);
        require(host.pcmFrames == 48, "resumed IOP synthesized PCM for suspended time");
        require(resumed.advance(1, epoch + background + 1000000) == 0,
                "resumed IOP replayed the excluded background interval");
        IopHostClock deterministic; deterministic.excludeHostTime(background);
        require(deterministic.advance(73, 0) == 73, "host exclusion altered deterministic EE credit");
    }
    std::cout << "PASS: synchronous IOP credit, absolute EE/host deadlines, /8 carry, reset, native-work stalls and PCM pacing\n";
}
