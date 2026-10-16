#include "native_particles.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

void xParCmdVelocityApply_Update_0x1d3f60(uint8_t*, R5900Context*, PS2Runtime*);
void xParCmdSmokeAlpha_Update_0x1d48e0(uint8_t*, R5900Context*, PS2Runtime*);
void xParCmdRotPar_Update_0x1d3fb0(uint8_t*, R5900Context*, PS2Runtime*);
void xParCmdMoveRandomPar_Update_0x1d4b20(uint8_t*, R5900Context*, PS2Runtime*);
void iurand_0x110a50(uint8_t*, R5900Context*, PS2Runtime*);
void fptoui_0x2523e8(uint8_t*, R5900Context*, PS2Runtime*);
void ps2___unpack_f_0x252298(uint8_t*, R5900Context*, PS2Runtime*);

namespace {
constexpr uint32_t base = 0x10000, stack = base + 0xf000;
constexpr uint32_t group = base + 0x40, command = base + 0x80;
constexpr uint32_t asset = base + 0x100, nodes = base + 0x1000, returnPc = 0x280000;
using Memory = std::array<uint8_t, 0x10000>;
std::mt19937 randomBits(0x1d48e0);
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> void put(Memory& memory, uint32_t address, T value) {
    require(address >= base && address + sizeof(value) <= base + memory.size(), "Fixture write out of bounds");
    std::memcpy(memory.data() + address - base, &value, sizeof(value));
}
float floating(unsigned i) {
    constexpr uint32_t edges[] = {0, 0x80000000, 1, 0x80000001, 0x007fffff,
        0x00800000, 0x7f7fffff, 0x7f800000, 0x7fffffff, 0xff800001, 0x3f800000};
    if (i % 4 == 0) return std::bit_cast<float>(edges[randomBits() % std::size(edges)]);
    if (i % 4 == 1) return std::bit_cast<float>(randomBits());
    return float(int32_t(randomBits())) / 1073741824.0f;
}
R5900Context context(uint32_t pc) {
    R5900Context cpu{};
    for (unsigned i = 1; i != 32; ++i) cpu.r[i] = _mm_set_epi32(randomBits(), randomBits(), randomBits(), randomBits());
    for (auto& f : cpu.f) f = floating(1);
    cpu.fcr31 = randomBits(); cpu.f_acc = floating(1);
    cpu.pc = pc; cpu.branch_pc = randomBits(); cpu.in_delay_slot = randomBits() & 1;
    SET_GPR_U32(&cpu, 4, command); SET_GPR_U32(&cpu, 5, group);
    SET_GPR_U32(&cpu, 28, base + 0x200 + 0x7768);
    SET_GPR_U32(&cpu, 29, stack); SET_GPR_U32(&cpu, 31, returnPc);
    return cpu;
}
Memory memory(unsigned count, unsigned mode, unsigned pattern) {
    Memory result;
    for (auto& value : result) value = randomBits();
    put(result, group, count ? nodes : 0u); put(result, command + 4, asset); put(result, asset + 8, mode);
    put(result, asset + 0x64, pattern < 2 ? 1.0f : floating(pattern));
    if (pattern < 2) for (unsigned i = 0; i < 6; ++i) put(result, asset + 8 + 4 * i, float(i * 60));
    put(result, asset + 8, mode);
    std::vector<unsigned> order;
    for (unsigned i = 1; i < count; ++i) order.push_back(i);
    std::shuffle(order.begin(), order.end(), randomBits); order.insert(order.begin(), 0);
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t node = nodes + 0x50 * order[i];
        put(result, node, i + 1 == count ? 0u : nodes + 0x50 * order[i + 1]);
        put(result, node + 0x3c, asset);
        put(result, node + 8, pattern < 2 ? float(i % 17) / 16 : floating(pattern));
        for (unsigned j = 0; j < 3; ++j) {
            put(result, node + 0x10 + j * 4, floating(pattern));
            put(result, node + 0x20 + j * 4, floating(pattern));
        }
    }
    return result;
}
void invoke(PS2Runtime& runtime, R5900Context& cpu, bool candidate, unsigned kind) {
    auto* ram = runtime.memory().getRDRAM();
    if (cpu.pc == 0x110a50) iurand_0x110a50(ram, &cpu, &runtime);
    else if (cpu.pc >= 0x252298 && cpu.pc < 0x2522f8) ps2___unpack_f_0x252298(ram, &cpu, &runtime);
    else if (cpu.pc >= 0x2523e8 && cpu.pc < 0x252468) fptoui_0x2523e8(ram, &cpu, &runtime);
    else if (kind == 3) (candidate ? nativeParticleMoveRandom : xParCmdMoveRandomPar_Update_0x1d4b20)(ram, &cpu, &runtime);
    else if (kind == 2) (candidate ? nativeParticleRotation : xParCmdRotPar_Update_0x1d3fb0)(ram, &cpu, &runtime);
    else if (kind == 1) (candidate ? nativeParticleSmokeAlpha : xParCmdSmokeAlpha_Update_0x1d48e0)(ram, &cpu, &runtime);
    else (candidate ? nativeParticleVelocityApply : xParCmdVelocityApply_Update_0x1d3f60)(ram, &cpu, &runtime);
}
struct Step { R5900Context cpu; Memory memory; uint32_t timer; };
std::vector<Step> execute(PS2Runtime& runtime, R5900Context cpu, const Memory& initial,
                          bool candidate, unsigned kind, unsigned interruptCycle, bool stopAfterYield) {
    auto* ram = runtime.memory().getRDRAM();
    std::memcpy(ram + base, initial.data(), initial.size());
    std::vector<Step> steps;
    for (unsigned step = 0; step != 1024; ++step) {
        runtime.eeScheduler().reset(ram, cpu);
        if (interruptCycle) {
            runtime.memory().writeIORegister(0x10000020, interruptCycle / 2);
            runtime.memory().writeIORegister(0x10000010, 0x180);
        }
        invoke(runtime, cpu, candidate, kind);
        Step output; output.cpu = cpu;
        std::memcpy(output.memory.data(), ram + base, output.memory.size());
        output.timer = runtime.memory().readIORegister(0x10000000);
        steps.push_back(output);
        if (cpu.pc == returnPc || stopAfterYield) return steps;
    }
    throw std::runtime_error("Particle fixture failed to finish after bounded continuations");
}
void compare(PS2Runtime& runtime, R5900Context cpu, const Memory& initial, unsigned kind,
             unsigned interruptCycle, unsigned iteration, bool stopAfterYield = false) {
    const auto expected = execute(runtime, cpu, initial, false, kind, interruptCycle, stopAfterYield);
    const auto actual = execute(runtime, cpu, initial, true, kind, interruptCycle, stopAfterYield);
    if (expected.size() != actual.size()) {
        std::cerr << "continuation count iteration=" << iteration << " expected=" << expected.size() << " actual=" << actual.size() << '\n';
        throw std::runtime_error("Native particle checkpoint/continuation count changed");
    }
    for (size_t step = 0; step < expected.size(); ++step) {
        if (std::memcmp(&expected[step].cpu, &actual[step].cpu, sizeof(cpu))) {
            auto* a = reinterpret_cast<const uint8_t*>(&expected[step].cpu);
            auto* b = reinterpret_cast<const uint8_t*>(&actual[step].cpu);
            for (size_t i = 0; i < sizeof(cpu); ++i) if (a[i] != b[i]) {
                std::cerr << "context iteration=" << iteration << " step=" << step << " byte=" << i
                          << " expected=" << unsigned(a[i]) << " actual=" << unsigned(b[i])
                          << " pc=" << std::hex << actual[step].cpu.pc << std::dec << '\n'; break;
            }
            throw std::runtime_error("Native particle register/control side effects changed");
        }
        if (expected[step].memory != actual[step].memory) {
            for (size_t i = 0; i < initial.size(); ++i) if (expected[step].memory[i] != actual[step].memory[i]) {
                std::cerr << "memory iteration=" << iteration << " step=" << step << " address=" << std::hex << base + i << std::dec << '\n'; break;
            }
            throw std::runtime_error("Native particle RAM or guest-stack side effects changed");
        }
        require(expected[step].timer == actual[step].timer, "Native particle checkpoint cycle charge changed");
    }
}
uint32_t entryPc(unsigned kind) { return kind == 3 ? 0x1d4b20 : kind == 2 ? 0x1d3fb0 : kind == 1 ? 0x1d48e0 : 0x1d3f60; }
const char* kindName(unsigned kind) { return kind == 3 ? "MoveRandomPar" : kind == 2 ? "RotPar" : kind == 1 ? "SmokeAlpha" : "VelocityApply"; }
double benchmark(PS2Runtime& runtime, R5900Context cpu, const Memory& initial, bool candidate, unsigned kind) {
    auto* ram = runtime.memory().getRDRAM();
    std::memcpy(ram + base, initial.data(), initial.size()); runtime.eeScheduler().reset(ram, cpu);
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i != 300; ++i) {
        cpu.pc = entryPc(kind);
        SET_GPR_U32(&cpu, 4, command); SET_GPR_U32(&cpu, 5, group);
        SET_GPR_U32(&cpu, 31, returnPc);
        invoke(runtime, cpu, candidate, kind);
        if (cpu.pc != returnPc) { std::cerr << "benchmark yielded i=" << i << " pc=" << std::hex << cpu.pc << std::dec << '\n'; throw std::runtime_error("Benchmark unexpectedly yielded"); }
    }
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / 300;
}
}
int main() {
    try {
        PS2Runtime runtime;
        require(runtime.memory().initialize() && runtime.syncCoreSubsystems(), "Runtime initialization failed");
        require(runtime.registerFunction(0x1d3f60, xParCmdVelocityApply_Update_0x1d3f60)
            && runtime.registerFunction(0x1d48e0, xParCmdSmokeAlpha_Update_0x1d48e0)
            && runtime.registerFunction(0x1d3fb0, xParCmdRotPar_Update_0x1d3fb0)
            && runtime.registerFunction(0x1d4b20, xParCmdMoveRandomPar_Update_0x1d4b20)
            && runtime.registerFunction(0x110a50, iurand_0x110a50)
            && runtime.registerFunction(0x2523e8, fptoui_0x2523e8)
            && runtime.registerFunction(0x252298, ps2___unpack_f_0x252298), "Original particle registration failed");
        installNativeParticles(runtime);
        unsigned cases = 0;
        for (unsigned kind : {0u, 1u, 2u, 3u}) for (unsigned i = 0; i != 400; ++i) {
            auto cpu = context(entryPc(kind));
            auto initial = memory(i % 65, i % 4, i % 8);
            compare(runtime, cpu, initial, kind, 0, cases++);
            if (i < 40) for (unsigned cycle : {8u, 16u, 24u, 32u, 40u, 48u, 56u, 64u, 72u, 80u, 88u, 104u}) compare(runtime, cpu, initial, kind, cycle, cases++);
        }
        for (unsigned kind : {0u, 1u, 2u, 3u}) {
            auto cpu = context(entryPc(kind));
            auto initial = memory(1, 0, 0);
            put(initial, nodes, nodes);
            compare(runtime, cpu, initial, kind, 48, cases++, true); // bounded cyclic list
            initial = memory(3, 1, 0);
            SET_GPR_U32(&cpu, 0, 0); // architectural zero with arbitrary unused high lanes remains valid
            if (kind) {
                // Command/group aliases with the prologue must choose the original.
                SET_GPR_U32(&cpu, 4, stack - 32);
                put(initial, stack - 28, asset);
                compare(runtime, cpu, initial, kind, 8, cases++, true);
            } else {
                // The root aliases a position that this very command changes.
                SET_GPR_U32(&cpu, 5, nodes + 0x10);
                put(initial, nodes + 0x10, nodes);
                compare(runtime, cpu, initial, kind, 0, cases++);
            }
        }
        for (unsigned kind : {0u, 1u, 2u, 3u}) {
            const uint32_t entry = entryPc(kind);
            auto cpu = context(entry);
            auto initial = memory(2, 1, 0);
            // A valid first node followed by the KSEG alias of the second node:
            // the bridge must export the exact resume state before fallback.
            put(initial, nodes, (nodes + 0x50) | 0x80000000u);
            compare(runtime, cpu, initial, kind, 0, cases++);
            // Partially overlapping nodes retain ordered reads and writes.
            initial = memory(1, 1, 0);
            put(initial, nodes, nodes + 16); put(initial, nodes + 16, 0u);
            put(initial, nodes + 16 + 0x3c, asset);
            put(initial, nodes + 0x3c, asset);
            put(initial, nodes + 0x20, 0.0f);
            compare(runtime, cpu, initial, kind, 0, cases++);
            // A particle overlaps both command/helper stack scratch. No SoA
            // gathering or delayed scattering may reorder these mutations.
            initial = memory(1, 1, 0);
            const uint32_t alias = stack - (kind == 2 ? 128u : 112u);
            put(initial, group, alias); put(initial, alias, 0u);
            put(initial, alias + 0x3c, asset);
            compare(runtime, cpu, initial, kind, 0, cases++);
            // Unusual entries preserve the generated function's own policy.
            initial = memory(1, 0, 0); cpu.pc = entry + 4;
            compare(runtime, cpu, initial, kind, 0, cases++);
        }
        std::cout << "PASS: " << cases << " particle differential cases; full context, RAM/stack, exceptional float bits, aliases, cycles and forced helper/backedge continuations\n";
        for (unsigned kind : {0u, 1u, 2u, 3u}) {
            auto cpu = context(entryPc(kind));
            const auto initial = memory(64, 0, 0);
            std::cout << kindName(kind) << " ns/64-particle command original="
                      << benchmark(runtime, cpu, initial, false, kind) << " native="
                      << benchmark(runtime, cpu, initial, true, kind) << '\n';
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
