#include "cpu_benchmark.h"
#include "native_cpu.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <switch.h>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

void iModelAnimMatrices_0x121ed0(uint8_t*, R5900Context*, PS2Runtime*);

namespace {
constexpr uint32_t region = 0x01000000, regionSize = 65536;
constexpr uint32_t model = region + 0x20, frame = region + 0x100;
constexpr uint32_t extension = region + 0x300, hierarchy = region + 0x400;
constexpr uint32_t quats = region + 0x1000, translations = region + 0x2000;
constexpr uint32_t nodes = region + 0x3000, output = region + 0x6000;
constexpr uint32_t stack = region + 0xf000, returnPc = 0x00280000;
constexpr uint64_t ticksPerSecond = 19200000;

uint64_t threadTicks() {
    uint64_t ticks = 0;
    const Result result = svcGetInfo(&ticks, InfoType_ThreadTickCount,
                                    CUR_THREAD_HANDLE, TickCountInfo_Total);
    if (R_FAILED(result)) throw std::runtime_error("CPU benchmark thread tick query failed");
    return ticks;
}
struct Restore {
    PS2Runtime& runtime;
    std::vector<uint8_t> bytes;
    R5900Context cpu;
    uint32_t plugin;
    explicit Restore(PS2Runtime& rt) : runtime(rt), bytes(regionSize), cpu(rt.cpu()) {
        std::memcpy(bytes.data(), runtime.memory().getRDRAM() + region, regionSize);
        std::memcpy(&plugin, runtime.memory().getRDRAM() + 0x2caa88, 4);
    }
    ~Restore() {
        std::memcpy(runtime.memory().getRDRAM() + region, bytes.data(), regionSize);
        std::memcpy(runtime.memory().getRDRAM() + 0x2caa88, &plugin, 4);
        runtime.cpu() = cpu;
    }
};
struct Fixture {
    R5900Context cpu{};
    std::vector<uint8_t> bytes;
};
Fixture fixture(PS2Runtime& runtime, unsigned bones) {
    auto* ram = runtime.memory().getRDRAM();
    std::memset(ram + region, 0x5a, regionSize);
    auto word = [ram](uint32_t at, uint32_t value) { std::memcpy(ram + at, &value, 4); };
    word(0x2caa88, 0); word(model + 4, frame); word(frame + 152, extension);
    word(extension + 4, hierarchy); word(hierarchy + 4, bones); word(hierarchy + 48, nodes);
    for (unsigned bone = 0; bone < bones; ++bone) {
        // Unit quaternions, noncommuting axes, and alternating branch parents.
        const float a = float(bone + 1) * 0.03125f;
        const float q[4] = {bone % 3 == 0 ? std::sin(a) : 0.0f,
                            bone % 3 == 1 ? std::sin(a) : 0.0f,
                            bone % 3 == 2 ? std::sin(a) : 0.0f, std::cos(a)};
        const float t[3] = {float(bone % 7) * 0.125f, -0.25f, 0.0625f};
        std::memcpy(ram + quats + bone * 16, q, 16);
        std::memcpy(ram + translations + bone * 12, t, 12);
        word(nodes + bone * 16 + 8, bone % 4 == 0 ? 2 : bone % 4 == 3 ? 1 : 0);
    }
    word(nodes + bones * 16 + 8, 0);
    Fixture result;
    result.cpu.pc = 0x121ed0;
    result.cpu.vu0_vf[0] = _mm_set_ps(1, 0, 0, 0);
    auto* cpu = &result.cpu;
    SET_GPR_U32(cpu, 4, model); SET_GPR_U32(cpu, 5, quats);
    SET_GPR_U32(cpu, 6, translations); SET_GPR_U32(cpu, 7, output);
    SET_GPR_U32(cpu, 29, stack); SET_GPR_U32(cpu, 31, returnPc);
    result.bytes.assign(ram + region, ram + region + regionSize);
    return result;
}
uint64_t complete(PS2Runtime& runtime, PS2Runtime::RecompiledFunction function,
                  R5900Context& cpu) {
    auto* ram = runtime.memory().getRDRAM();
    function(ram, &cpu, &runtime);
    uint64_t continuations = 0;
    while (cpu.pc != returnPc) {
        if (cpu.pc != 0x121f44 || ++continuations > 256)
            throw std::runtime_error("CPU benchmark unexpected animation continuation");
        iModelAnimMatrices_0x121ed0(ram, &cpu, &runtime);
    }
    return continuations;
}
struct Measurement { uint64_t calls = 0, ticks = 0, continuations = 0; };
Measurement measure(PS2Runtime& runtime, const Fixture& initial,
                    PS2Runtime::RecompiledFunction function, uint64_t deadline) {
    std::memcpy(runtime.memory().getRDRAM() + region, initial.bytes.data(), regionSize);
    // Equal warmup and reversed measurement order below reduce cold-code and
    // clock-ramp bias without introducing an FPS limiter or changing clocks.
    for (unsigned i = 0; i < 16; ++i) {
        auto cpu = initial.cpu;
        complete(runtime, function, cpu);
    }
    Measurement result;
    const uint64_t start = threadTicks();
    do {
        for (unsigned i = 0; i < 64; ++i) {
            auto cpu = initial.cpu;
            result.continuations += complete(runtime, function, cpu);
            ++result.calls;
        }
        result.ticks = threadTicks() - start;
    } while (result.ticks < ticksPerSecond / 50 && armGetSystemTick() < deadline);
    return result;
}
}

void runNativeCpuKernelBenchmark(PS2Runtime& runtime) {
    Restore restore(runtime);
    const uint64_t started = armGetSystemTick(), deadline = started + ticksPerSecond;
    for (const unsigned bones : {1u, 8u, 32u, 64u}) {
        const Fixture initial = fixture(runtime, bones);
        auto expected = initial.cpu;
        const auto originalContinues = complete(runtime, iModelAnimMatrices_0x121ed0, expected);
        std::vector<uint8_t> expectedBytes(regionSize);
        std::memcpy(expectedBytes.data(), runtime.memory().getRDRAM() + region, regionSize);
        std::memcpy(runtime.memory().getRDRAM() + region, initial.bytes.data(), regionSize);
        auto actual = initial.cpu;
        const auto nativeContinues = complete(runtime, nativeModelAnimMatrices, actual);
        if (std::memcmp(&expected, &actual, sizeof(actual)) ||
            std::memcmp(expectedBytes.data(), runtime.memory().getRDRAM() + region, regionSize))
            throw std::runtime_error("Switch native animation differential failed");
        auto original = measure(runtime, initial, iModelAnimMatrices_0x121ed0, deadline);
        auto candidate = measure(runtime, initial, nativeModelAnimMatrices, deadline);
        const auto candidateAgain = measure(runtime, initial, nativeModelAnimMatrices, deadline);
        const auto originalAgain = measure(runtime, initial, iModelAnimMatrices_0x121ed0, deadline);
        original.calls += originalAgain.calls; original.ticks += originalAgain.ticks;
        original.continuations += originalAgain.continuations;
        candidate.calls += candidateAgain.calls; candidate.ticks += candidateAgain.ticks;
        candidate.continuations += candidateAgain.continuations;
        const double originalNs = double(original.ticks) * 1e9 / ticksPerSecond / original.calls;
        const double nativeNs = double(candidate.ticks) * 1e9 / ticksPerSecond / candidate.calls;
        std::cout << "[cpu-kernel-benchmark] routine=iModelAnimMatrices bones=" << bones
                  << " original_ns=" << originalNs << " native_ns=" << nativeNs
                  << " speedup=" << originalNs / nativeNs << " original_calls=" << original.calls
                  << " native_calls=" << candidate.calls << " original_continuations="
                  << original.continuations + originalContinues << " native_continuations="
                  << candidate.continuations + nativeContinues << " differential=pass\n";
    }
    std::cout << "[cpu-kernel-benchmark] elapsed_ms="
              << double(armGetSystemTick() - started) * 1000 / ticksPerSecond
              << " temporary_ram_and_cpu=restored_on_return\n";
}
