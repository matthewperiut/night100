#include "native_cpu.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_recompiled_stubs.h"
#include "runtime/ee_scheduler.h"

#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>

void iModelAnimMatrices_0x121ed0(uint8_t*, R5900Context*, PS2Runtime*);
void xQuatToMat_0x113500(uint8_t*, R5900Context*, PS2Runtime*);
void xMat3x3Mul_0x113fe0(uint8_t*, R5900Context*, PS2Runtime*);

namespace {
constexpr uint32_t region = 0x10000, stack = region + 0x3800;
using Memory = std::array<uint8_t, 16384>;
std::mt19937 randomBits(0x113500);

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void compare(PS2Runtime& runtime, PS2Runtime::RecompiledFunction original,
             PS2Runtime::RecompiledFunction candidate, R5900Context initial,
             const Memory& memory, unsigned iteration) {
    auto* ram = runtime.memory().getRDRAM();
    R5900Context expected = initial;
    std::memcpy(ram + region, memory.data(), memory.size());
    original(ram, &expected, &runtime);
    Memory expectedMemory;
    std::memcpy(expectedMemory.data(), ram + region, memory.size());
    R5900Context actual = initial;
    std::memcpy(ram + region, memory.data(), memory.size());
    candidate(ram, &actual, &runtime);
    if (std::memcmp(&expected, &actual, sizeof(actual))) {
        const auto* a = reinterpret_cast<const uint8_t*>(&expected);
        const auto* b = reinterpret_cast<const uint8_t*>(&actual);
        for (unsigned i = 0; i < sizeof(actual); ++i) {
            if (a[i] != b[i]) {
                std::cerr << "context mismatch pc=" << std::hex << initial.pc << std::dec << " iteration=" << iteration << " byte=" << i
                          << " expected=" << unsigned(a[i]) << " actual=" << unsigned(b[i]) << '\n';
                for (unsigned q=0;initial.pc==0x121ed0 && q<4;++q) {
                    uint32_t bits; std::memcpy(&bits,memory.data()+GPR_U32((&expected),5)-16-region+q*4,4);
                    std::cerr << " q" << q << "=" << std::hex << bits << std::dec;
                }
                std::cerr << '\n';
                for(unsigned vf=1;vf<=5;++vf) {
                    uint32_t e[4],a[4]; std::memcpy(e,&expected.vu0_vf[vf],16); std::memcpy(a,&actual.vu0_vf[vf],16);
                    std::cerr << " vf"<<vf;
                    for(unsigned j=0;j<4;++j) std::cerr<< " " <<std::hex<<e[j]<<"/"<<a[j]<<std::dec;
                    std::cerr<<'\n';
                }
                break;
            }
        }
        throw std::runtime_error("Native CPU kernel changed register/control side effects");
    }
    require(!std::memcmp(expectedMemory.data(), ram + region, memory.size()),
            "Native CPU kernel changed RAM, padding, or guest stack side effects");
}
float value(unsigned iteration) {
    constexpr uint32_t edges[] = {0, 0x80000000, 1, 0x80000001, 0x007fffff,
        0x00800000, 0x7f7fffff, 0x7f800000, 0x7fffffff, 0xff800001, 0x3f800000};
    if (iteration % 4 == 0) return std::bit_cast<float>(edges[randomBits() % std::size(edges)]);
    return float(int32_t(randomBits())) / 2147483648.0f;
}
R5900Context context(uint32_t pc) {
    R5900Context cpu{};
    for (unsigned i = 1; i != 32; ++i)
        cpu.r[i] = _mm_set_epi32(randomBits(), randomBits(), randomBits(), randomBits());
    for (auto& f : cpu.f) f = value(1);
    SET_GPR_U32(&cpu, 29, stack);
    SET_GPR_U32(&cpu, 31, 0x280000);
    cpu.pc = pc;
    cpu.branch_pc = 0x123456;
    return cpu;
}
void put(Memory& memory, uint32_t address, float f) {
    std::memcpy(memory.data() + address - region, &f, 4);
}
double benchmark(PS2Runtime& runtime, PS2Runtime::RecompiledFunction function,
                 R5900Context cpu, unsigned repetitions) {
    auto* ram = runtime.memory().getRDRAM();
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i != repetitions; ++i) function(ram, &cpu, &runtime);
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / repetitions;
}
}

int main() {
    try {
        PS2Runtime runtime;
        require(runtime.memory().initialize() && runtime.syncCoreSubsystems(), "Runtime initialization failed");
        require(runtime.registerFunction(0x113500, xQuatToMat_0x113500)
            && runtime.registerFunction(0x113fe0, xMat3x3Mul_0x113fe0)
            && runtime.registerFunction(0x121ed0, iModelAnimMatrices_0x121ed0)
            && runtime.registerFunction(0x2572c8, memcpy_0x2572c8), "Original registration failed");
        installNativeCpu(runtime);
        unsigned cases = 0;
        for (unsigned i = 0; i != 6000; ++i) {
            Memory memory;
            for (auto& b : memory) b = randomBits();
            auto cpu = context(0x113500);
            const uint32_t source = region + 0x100;
            const uint32_t targets[] = {region + 0x200, source, source + 4, source - 4};
            SET_GPR_U32(&cpu, 4, source);
            SET_GPR_U32(&cpu, 5, targets[i % std::size(targets)]);
            for (unsigned j = 0; j != 4; ++j) put(memory, source + j * 4, value(i));
            compare(runtime, xQuatToMat_0x113500, nativeQuatToMat, cpu, memory, i);
            ++cases;
        }
        for (unsigned i = 0; i != 6000; ++i) {
            Memory memory;
            for (auto& b : memory) b = randomBits();
            auto cpu = context(0x113fe0);
            // Nonalias fast path, equal inputs, exact aliases and partial overlap.
            const uint32_t left = region, right = i % 7 == 0 ? left : region + 0x100;
            const uint32_t targets[] = {region + 0x200, left, right, left + 4, region + 0x200};
            SET_GPR_U64(&cpu, 4, targets[i % std::size(targets)]);
            SET_GPR_U64(&cpu, 5, left);
            SET_GPR_U64(&cpu, 6, right);
            for (unsigned j = 0; j != 12; ++j) {
                put(memory, left + j * 4, value(i));
                put(memory, right + j * 4, value(i));
            }
            compare(runtime, xMat3x3Mul_0x113fe0, nativeMat3x3Mul, cpu, memory, i);
            ++cases;
        }

        // Real RpAtomic -> frame plugin -> RpHAnimHierarchy -> nodeInfo layout.
        // Test branching hierarchy pushes/pops, sentinel flags, packed matrix
        // padding and VU/GPR state, rather than a replacement success stub.
        auto putWord = [](Memory& m, uint32_t at, uint32_t v) {
            std::memcpy(m.data()+at-region,&v,4);
        };
        uint32_t pluginOffset=0;
        std::memcpy(runtime.memory().getRDRAM()+0x2caa88,&pluginOffset,4);
        R5900Context animationCpu{};
        Memory animationMemory{};
        for(unsigned i=0;i<1200;++i) {
            Memory memory;
            for(auto& b:memory) b=randomBits();
            auto cpu=context(0x121ed0);
            cpu.vu0_vf[0]=_mm_set_ps(1,0,0,0);
            for(unsigned j=1;j<32;++j) {
                float v[4]; for(auto& f:v) f=value(i);
                std::memcpy(&cpu.vu0_vf[j],v,16);
            }
            const uint32_t model=region+0x20, frame=region+0x40, extension=region+0x100;
            const uint32_t hierarchy=region+0x140, quat=region+0x400, tran=region+0x800;
            const uint32_t nodes=region+0xc00, matrices=region+0x1400;
            const unsigned bones=i==1199 ? 48 : (i%19==0 ? 1 : 1+randomBits()%48);
            putWord(memory,model+4,frame); putWord(memory,frame+152,extension);
            putWord(memory,extension+4,hierarchy); putWord(memory,hierarchy+4,bones);
            putWord(memory,hierarchy+48,nodes);
            int depth=1;
            for(unsigned j=0;j<=bones;++j) {
                uint32_t flags=0;
                if(j<bones) {
                    if(depth<8 && randomBits()%3==0) { flags|=2; ++depth; }
                    if(depth>1 && randomBits()%3==0) { flags|=1; --depth; }
                } else flags=randomBits()&3;
                putWord(memory,nodes+j*16+8,flags);
                if(j<bones) {
                    for(unsigned k=0;k<4;++k) put(memory,quat+j*16+k*4,value(i));
                    for(unsigned k=0;k<3;++k) put(memory,tran+j*12+k*4,value(i));
                }
            }
            SET_GPR_U32(&cpu,4,model); SET_GPR_U32(&cpu,5,quat);
            SET_GPR_U32(&cpu,6,tran); SET_GPR_U32(&cpu,7,matrices);
            if (i%19==0) SET_GPR_U32(&cpu,7,quat); // supported original alias fallback
            if (i%23==0) putWord(memory,frame+152,0); // no hierarchy fallback
            if (i == 1199) { animationCpu = cpu; animationMemory = memory; }
            compare(runtime,iModelAnimMatrices_0x121ed0,nativeModelAnimMatrices,cpu,memory,i);
            ++cases;
        }
        std::cout << "PASS: " << cases << " original/native CPU differential cases; full register/control state, RAM, padding, stack, aliases and PS2 float edges\n";
        auto animationBenchmark = [&](PS2Runtime::RecompiledFunction fn) {
            const auto start=std::chrono::steady_clock::now();
            for(unsigned i=0;i<10000;++i) {
                auto state=animationCpu;
                fn(runtime.memory().getRDRAM(),&state,&runtime);
            }
            return std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/10000;
        };
        std::cout << "iModelAnimMatrices ns/call original=" << animationBenchmark(iModelAnimMatrices_0x121ed0)
                  << " native=" << animationBenchmark(nativeModelAnimMatrices) << '\n';
        auto cpu = context(0x113500);
        SET_GPR_U64(&cpu, 4, region);
        SET_GPR_U64(&cpu, 5, region + 0x200);
        auto* ram = runtime.memory().getRDRAM();
        for (unsigned i = 0; i != 12; ++i) {
            const float f = value(1);
            std::memcpy(ram + region + i * 4, &f, 4);
            std::memcpy(ram + region + 0x100 + i * 4, &f, 4);
        }
        std::cout << "xQuatToMat ns/call original=" << benchmark(runtime, xQuatToMat_0x113500, cpu, 200000)
                  << " native=" << benchmark(runtime, nativeQuatToMat, cpu, 200000) << '\n';
        SET_GPR_U64(&cpu, 4, region + 0x200);
        SET_GPR_U64(&cpu, 5, region);
        SET_GPR_U64(&cpu, 6, region + 0x100);
        std::cout << "xMat3x3Mul ns/call original=" << benchmark(runtime, xMat3x3Mul_0x113fe0, cpu, 200000)
                  << " native=" << benchmark(runtime, nativeMat3x3Mul, cpu, 200000) << '\n';
        // A pending stop forces both routines to export exactly the same
        // first-bone scheduler continuation, including its unfinished stack.
        runtime.eeScheduler().requestStop();
        compare(runtime,iModelAnimMatrices_0x121ed0,nativeModelAnimMatrices,
                animationCpu,animationMemory,1200);
        std::memcpy(runtime.memory().getRDRAM()+region,animationMemory.data(),animationMemory.size());
        auto continuation=animationCpu;
        iModelAnimMatrices_0x121ed0(runtime.memory().getRDRAM(),&continuation,&runtime);
        require(continuation.pc==0x121f44,"Animation did not expose its original continuation");
        std::memcpy(animationMemory.data(),runtime.memory().getRDRAM()+region,animationMemory.size());
        compare(runtime,iModelAnimMatrices_0x121ed0,nativeModelAnimMatrices,
                continuation,animationMemory,1201);
        std::cout << "PASS: animation scheduler continuation and original fallback\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
