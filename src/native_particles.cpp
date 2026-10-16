#include "native_particles.h"
#include "native_particle_policy.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <bit>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace {
PS2Runtime::RecompiledFunction originalVelocity = nullptr;
PS2Runtime::RecompiledFunction originalSmoke = nullptr;
PS2Runtime::RecompiledFunction originalRotation = nullptr;
PS2Runtime::RecompiledFunction originalMoveRandom = nullptr;

bool overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn) {
    return uint64_t(a) < uint64_t(b) + bn && uint64_t(b) < uint64_t(a) + an;
}
bool ordinary(uint32_t address, uint32_t size) {
    return !(address & 3u) && size <= PS2_RAM_SIZE && address <= PS2_RAM_SIZE - size;
}
template<class T> T read(const uint8_t* ram, uint32_t address) {
    T value; std::memcpy(&value, ram + address, sizeof(value)); return value;
}
template<class T> void write(uint8_t* ram, uint32_t address, T value) {
    std::memcpy(ram + address, &value, sizeof(value));
}
void reference(PS2Runtime::RecompiledFunction fn, uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    if (!fn) throw std::runtime_error("Native particle kernel lacks original fallback");
    fn(ram, cpu, runtime);
}
bool regularZero(const R5900Context* cpu) {
    return GPR_U64(cpu, 0) == 0;
}

// The original fptoui's unpack helper is an integer bit decomposition. Preserve
// its caller-save registers and all guest stack writes, including the unwritten
// exponent/significand slots for exponent-zero input. No host float-to-unsigned
// cast is used. The two original call checkpoints remain at their exact PCs.
bool alphaToUnsigned(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    cpu->pc = 0x2523e8;
    if (runtime->eeCheckpointDue(EeScheduler::kGuestDispatchCycles)) return false;
    const uint32_t stack = GPR_U32(cpu, 29) - 48;
    SET_GPR_S32(cpu, 29, static_cast<int32_t>(stack));
    SET_GPR_S32(cpu, 4, static_cast<int32_t>(stack + 16));
    SET_GPR_U64(cpu, 5, GPR_U64(cpu, 29));
    write(ram, stack + 32, GPR_U64(cpu, 31));
    SET_GPR_U32(cpu, 31, 0x252400);
    cpu->branch_pc = 0x2523f8;
    cpu->in_delay_slot = false;
    write(ram, stack + 16, cpu->f[12]);
    cpu->pc = 0x252298;
    if (runtime->eeCheckpointDue(EeScheduler::kGuestDispatchCycles)) return false;

    const uint32_t bits = read<uint32_t>(ram, stack + 16);
    const uint32_t exponent = (bits >> 23) & 255;
    const uint32_t sign = bits >> 31;
    const uint32_t shifted = (bits & 0x7fffff) << 7;
    SET_GPR_U64(cpu, 6, GPR_U64(cpu, 5));
    SET_GPR_S32(cpu, 5, static_cast<int32_t>(exponent) - 127);
    SET_GPR_S32(cpu, 4, static_cast<int32_t>(shifted));
    write(ram, stack + 4, sign);
    if (exponent == 0) {
        write<uint32_t>(ram, stack, 2);
        SET_GPR_U64(cpu, 3, 0);
        SET_GPR_U64(cpu, 2, 0);
    } else {
        const uint32_t significand = shifted | 0x40000000;
        const int32_t power = static_cast<int32_t>(exponent) - 127;
        write(ram, stack + 8, power);
        write<uint32_t>(ram, stack, 3);
        write(ram, stack + 12, significand);
        SET_GPR_S32(cpu, 3, static_cast<int32_t>(sign));
        SET_GPR_U64(cpu, 2, 0);
        if (!sign) {
            SET_GPR_S32(cpu, 4, power);
            SET_GPR_U64(cpu, 3, power < 32 ? 1 : 0);
            if (power >= 32) SET_GPR_S32(cpu, 2, -1);
            else if (power >= 0) {
                SET_GPR_S32(cpu, 3, static_cast<int32_t>(significand));
                const uint32_t result = power < 31 ? significand >> ((30 - power) & 31)
                                                      : significand << ((power - 30) & 31);
                SET_GPR_S32(cpu, 2, static_cast<int32_t>(result));
            }
        }
    }
    SET_GPR_U64(cpu, 31, read<uint64_t>(ram, stack + 32));
    SET_GPR_S32(cpu, 29, static_cast<int32_t>(stack + 48));
    cpu->branch_pc = 0x252460;
    cpu->in_delay_slot = false;
    cpu->pc = GPR_U32(cpu, 31);
    return true;
}
// Typed bridge to this game's LCG. The state remains in guest RAM so snapshots,
// other random callers and intentionally aliased addresses see the same draws.
bool rotationRandom(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    cpu->pc = 0x110a50;
    if (runtime->eeCheckpointDue(EeScheduler::kGuestDispatchCycles)) return false;
    const uint32_t address = GPR_U32(cpu, 28) - 0x7768;
    const int32_t seed = read<int32_t>(ram, address);
    SET_GPR_S32(cpu, 4, seed);
    const int64_t product = int64_t(seed) * 1103515245;
    cpu->lo = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(product)));
    cpu->hi = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(product >> 32)));
    const uint32_t next = static_cast<uint32_t>(product) + 12345;
    write(ram, address, next);
    SET_GPR_S32(cpu, 2, static_cast<int32_t>(next));
    SET_GPR_S32(cpu, 3, static_cast<int32_t>(next >> 1));
    cpu->f[1] = std::bit_cast<float>(0x2f800000u);
    if (static_cast<int32_t>(next) < 0) {
        SET_GPR_U64(cpu, 2, next & 1);
        SET_GPR_U64(cpu, 3, (next >> 1) | (next & 1));
        cpu->f[0] = FPU_CVT_S_W(static_cast<int32_t>(GPR_U32(cpu, 3)));
        cpu->f[0] = FPU_ADD_S(cpu->f[0], cpu->f[0]);
    } else cpu->f[0] = FPU_CVT_S_W(static_cast<int32_t>(next));
    cpu->f[0] = FPU_MUL_S(cpu->f[1], cpu->f[0]);
    cpu->branch_pc = 0x110aa0;
    cpu->in_delay_slot = false;
    cpu->pc = GPR_U32(cpu, 31);
    return true;
}

}

void nativeParticleVelocityApply(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    const bool resume = cpu->pc == 0x1d3f6c;
    if ((!resume && cpu->pc != 0x1d3f60) || !regularZero(cpu)
#if defined(PS2X_STRICT_RETURN_DIAGNOSTICS) && PS2X_STRICT_RETURN_DIAGNOSTICS
        || true
#endif
    ) { reference(originalVelocity, ram, cpu, runtime); return; }
    if (!resume) {
        const uint32_t group = GPR_U32(cpu, 5);
        if (!ordinary(group, 4)) { reference(originalVelocity, ram, cpu, runtime); return; }
        SET_GPR_S32(cpu, 3, static_cast<int32_t>(read<uint32_t>(ram, group)));
    }
    while (GPR_U64(cpu, 3) != 0) {
        const uint32_t particle = GPR_U32(cpu, 3);
        cpu->pc = 0x1d3f6c;
        // Each node is admitted before its first load/store. Ordered loads and
        // stores preserve even partial aliases, self-links and changing links.
        // A later unusual node resumes the original at this exact backedge.
        if (!ordinary(particle, 0x2c)) { reference(originalVelocity, ram, cpu, runtime); return; }
        float position = 0, result = 0;
        for (uint32_t component = 0; component != 12; component += 4) {
            position = read<float>(ram, particle + 0x10 + component);
            const float velocity = read<float>(ram, particle + 0x20 + component);
            result = FPU_ADD_S(position, velocity);
            write(ram, particle + 0x10 + component, result);
        }
        cpu->f[1] = position;
        cpu->f[0] = result;
        SET_GPR_S32(cpu, 3, static_cast<int32_t>(read<uint32_t>(ram, particle)));
        if (GPR_U64(cpu, 3) != 0) {
            cpu->pc = 0x1d3f6c;
            if (runtime->eeCheckpointDue()) return;
        }
    }
    cpu->pc = GPR_U32(cpu, 31);
}

void nativeParticleSmokeAlpha(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    const bool resume = cpu->pc == 0x1d497c;
    if ((!resume && cpu->pc != 0x1d48e0) || !regularZero(cpu)
#if defined(PS2X_STRICT_RETURN_DIAGNOSTICS) && PS2X_STRICT_RETURN_DIAGNOSTICS
        || true
#endif
    ) { reference(originalSmoke, ram, cpu, runtime); return; }
    const uint32_t originalStack = GPR_U32(cpu, 29);
    if ((originalStack & 15) || originalStack < (resume ? 48u : 96u)
        || !ordinary(originalStack - (resume ? 48u : 96u), 96)) {
        reference(originalSmoke, ram, cpu, runtime); return;
    }
    if (!resume) {
        const uint32_t command = GPR_U32(cpu, 4), group = GPR_U32(cpu, 5);
        if (!ordinary(command, 8) || !ordinary(group, 4)
            || (originalStack & 15)
            || overlaps(command, 8, originalStack - 48, 48)
            || overlaps(group, 4, originalStack - 48, 48)
            || !ordinary(read<uint32_t>(ram, command + 4), 12)) {
            reference(originalSmoke, ram, cpu, runtime); return;
        }
        // Command/group aliases with the prologue retain the original. Other
        // aliases keep their original read/store order, including asset data.
        const uint32_t stack = originalStack - 48;
        SET_GPR_S32(cpu, 29, static_cast<int32_t>(stack));
        write(ram, stack + 32, GPR_VEC(cpu, 31));
        write(ram, stack + 16, GPR_VEC(cpu, 16));
        write(ram, stack + 12, cpu->f[23]);
        write(ram, stack + 8, cpu->f[22]);
        write(ram, stack + 4, cpu->f[21]);
        write(ram, stack, cpu->f[20]);
        const uint32_t asset = read<uint32_t>(ram, command + 4);
        const uint32_t mode = read<uint32_t>(ram, asset + 8);
        SET_GPR_S32(cpu, 6, static_cast<int32_t>(mode));
        if (mode == 0 || mode == 1) {
            cpu->branch_pc = mode == 1 ? 0x1d4908 : 0x1d4910;
            cpu->in_delay_slot = false;
            const uint32_t c21 = mode == 1 ? 0x3fa22222 : 0x4101135d;
            const uint32_t c22 = mode == 1 ? 0x4059999a : 0xc16ca37f;
            const uint32_t c23 = mode == 1 ? 0xc0955555 : 0x40d72045;
            cpu->f[20] = 0;
            cpu->f[21] = std::bit_cast<float>(c21);
            cpu->f[22] = std::bit_cast<float>(c22);
            cpu->f[23] = std::bit_cast<float>(c23);
            SET_GPR_S32(cpu, 3, static_cast<int32_t>(c23));
        } else {
            cpu->branch_pc = 0x1d4918;
            cpu->in_delay_slot = false;
            SET_GPR_S32(cpu, 3, 0x41010000);
        }
        SET_GPR_S32(cpu, 16, static_cast<int32_t>(read<uint32_t>(ram, group)));
    }
    while (GPR_U64(cpu, 16) != 0) {
        const uint32_t particle = GPR_U32(cpu, 16);
        cpu->pc = 0x1d497c;
        if (!ordinary(particle, 0x50)) { reference(originalSmoke, ram, cpu, runtime); return; }
        const uint32_t asset = read<uint32_t>(ram, particle + 0x3c);
        if (!ordinary(asset, 0x68)) { reference(originalSmoke, ram, cpu, runtime); return; }
        cpu->f[1] = read<float>(ram, particle + 8);
        cpu->f[2] = 0;
        cpu->f[0] = read<float>(ram, asset + 0x64);
        if (cpu->f[0] == 0) cpu->fcr31 |= 0x100000;
        cpu->f[1] = FPU_DIV_S(cpu->f[1], cpu->f[0]);
        const bool below = FPU_C_OLT_S(cpu->f[1], cpu->f[2]);
        cpu->fcr31 = below ? cpu->fcr31 | 0x800000 : cpu->fcr31 & ~0x800000;
        if (below) cpu->f[1] = FPU_MOV_S(cpu->f[2]);
        else {
            cpu->f[0] = 1;
            const bool within = FPU_C_OLE_S(cpu->f[1], cpu->f[0]);
            cpu->fcr31 = within ? cpu->fcr31 | 0x800000 : cpu->fcr31 & ~0x800000;
            if (!within) cpu->f[1] = FPU_MOV_S(cpu->f[0]);
        }
        cpu->f[0] = 1;
        cpu->f[4] = FPU_SUB_S(cpu->f[0], cpu->f[1]);
        cpu->f[0] = FPU_MUL_S(cpu->f[21], cpu->f[4]);
        cpu->f[1] = FPU_MUL_S(cpu->f[4], cpu->f[4]);
        cpu->f[2] = FPU_ADD_S(cpu->f[20], cpu->f[0]);
        cpu->f[0] = FPU_MUL_S(cpu->f[1], cpu->f[4]);
        cpu->f[1] = FPU_MUL_S(cpu->f[22], cpu->f[1]);
        FPU_SET_ACC(cpu, FPU_ADD_S(cpu->f[2], cpu->f[1]));
        cpu->f[3] = 255;
        cpu->f[0] = FPU_ADD_S(cpu->f_acc, FPU_MUL_S(cpu->f[23], cpu->f[0]));
        cpu->f[12] = FPU_MUL_S(cpu->f[3], cpu->f[0]);
        const bool within = FPU_C_OLE_S(cpu->f[12], cpu->f[3]);
        cpu->fcr31 = within ? cpu->fcr31 | 0x800000 : cpu->fcr31 & ~0x800000;
        if (!within) cpu->f[12] = FPU_MOV_S(cpu->f[3]);
        SET_GPR_S32(cpu, 2, 0x437f0000);
        cpu->branch_pc = below ? 0x1d49a8 : 0x1d49c4;
        cpu->in_delay_slot = false;
        SET_GPR_U32(cpu, 31, 0x1d4a28);
        if (!alphaToUnsigned(ram, cpu, runtime)) return;
        write<uint8_t>(ram, particle + 15, static_cast<uint8_t>(GPR_U32(cpu, 2)));
        write<uint32_t>(ram, particle + 0x4c, 0);
        SET_GPR_S32(cpu, 16, static_cast<int32_t>(read<uint32_t>(ram, particle)));
        if (GPR_U64(cpu, 16) != 0) {
            cpu->pc = 0x1d497c;
            if (runtime->eeCheckpointDue()) return;
        }
    }
    const uint32_t stack = GPR_U32(cpu, 29);
    SET_GPR_VEC(cpu, 31, read<__m128i>(ram, stack + 32));
    SET_GPR_VEC(cpu, 16, read<__m128i>(ram, stack + 16));
    cpu->f[23] = read<float>(ram, stack + 12);
    cpu->f[22] = read<float>(ram, stack + 8);
    cpu->f[21] = read<float>(ram, stack + 4);
    cpu->f[20] = read<float>(ram, stack);
    const uint32_t target = GPR_U32(cpu, 31);
    SET_GPR_S32(cpu, 29, static_cast<int32_t>(stack + 48));
    cpu->branch_pc = 0x1d4a58;
    cpu->in_delay_slot = false;
    cpu->pc = target;
}

void nativeParticleRotation(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    const bool resume = cpu->pc == 0x1d4050;
    const uint32_t originalStack = GPR_U32(cpu, 29);
    const uint32_t randomAddress = GPR_U32(cpu, 28) - 0x7768;
    if ((!resume && cpu->pc != 0x1d3fb0) || !regularZero(cpu)
        || (originalStack & 15) || originalStack < (resume ? 48u : 112u)
        || !ordinary(originalStack - (resume ? 48u : 112u), 112)
        || !ordinary(randomAddress, 4)
#if defined(PS2X_STRICT_RETURN_DIAGNOSTICS) && PS2X_STRICT_RETURN_DIAGNOSTICS
        || true
#endif
    ) { reference(originalRotation, ram, cpu, runtime); return; }
    if (!resume) {
        const uint32_t command = GPR_U32(cpu, 4), group = GPR_U32(cpu, 5);
        if (!ordinary(command, 8) || !ordinary(group, 4)
            || overlaps(command, 8, originalStack - 64, 64)
            || overlaps(group, 4, originalStack - 64, 64)
            || !ordinary(read<uint32_t>(ram, command + 4), 32)) {
            reference(originalRotation, ram, cpu, runtime); return;
        }
        const uint32_t stack = originalStack - 64;
        SET_GPR_S32(cpu, 29, static_cast<int32_t>(stack));
        write(ram, stack + 48, GPR_VEC(cpu, 31));
        write(ram, stack + 32, GPR_VEC(cpu, 16));
        for (int i = 6; i >= 0; --i) write(ram, stack + i * 4, cpu->f[20 + i]);
        const uint32_t asset = read<uint32_t>(ram, command + 4);
        SET_GPR_S32(cpu, 4, static_cast<int32_t>(asset));
        cpu->f[6] = 360;
        SET_GPR_S32(cpu, 16, static_cast<int32_t>(read<uint32_t>(ram, group)));
        SET_GPR_S32(cpu, 3, 0x437f0000);
        cpu->f[7] = 255;
        cpu->f[0] = read<float>(ram, asset + 28);
        cpu->f[5] = read<float>(ram, asset + 16);
        cpu->f[1] = read<float>(ram, asset + 24);
        cpu->f[4] = read<float>(ram, asset + 12);
        cpu->f[3] = read<float>(ram, asset + 8);
        cpu->f[2] = read<float>(ram, asset + 20);
        cpu->f[26] = FPU_MOV_S(cpu->f[12]);
        cpu->f[0] = FPU_DIV_S(FPU_SUB_S(cpu->f[0], cpu->f[5]), cpu->f[6]);
        cpu->f[22] = FPU_MUL_S(cpu->f[7], cpu->f[0]);
        cpu->f[1] = FPU_DIV_S(FPU_SUB_S(cpu->f[1], cpu->f[4]), cpu->f[6]);
        cpu->f[0] = FPU_DIV_S(cpu->f[3], cpu->f[6]);
        cpu->f[21] = FPU_MUL_S(cpu->f[7], cpu->f[1]);
        cpu->f[23] = FPU_MUL_S(cpu->f[7], cpu->f[0]);
        cpu->f[2] = FPU_DIV_S(FPU_SUB_S(cpu->f[2], cpu->f[3]), cpu->f[6]);
        cpu->f[1] = FPU_DIV_S(cpu->f[4], cpu->f[6]);
        cpu->f[0] = FPU_DIV_S(cpu->f[5], cpu->f[6]);
        cpu->f[20] = FPU_MUL_S(cpu->f[7], cpu->f[2]);
        cpu->f[24] = FPU_MUL_S(cpu->f[7], cpu->f[1]);
        cpu->f[25] = FPU_MUL_S(cpu->f[7], cpu->f[0]);
        cpu->branch_pc = 0x1d4048;
        cpu->in_delay_slot = false;
    }
    while (GPR_U64(cpu, 16) != 0) {
        cpu->pc = 0x1d4050;
        const uint32_t particle = GPR_U32(cpu, 16);
        if (!ordinary(particle, 0x38)) { reference(originalRotation, ram, cpu, runtime); return; }
        constexpr uint32_t randomCall[] = {0x1d4050, 0x1d409c, 0x1d40e8};
        constexpr uint32_t randomReturn[] = {0x1d4058, 0x1d40a4, 0x1d40f0};
        constexpr uint32_t convertCall[] = {0x1d4094, 0x1d40e0, 0x1d412c};
        constexpr uint32_t convertReturn[] = {0x1d409c, 0x1d40e8, 0x1d4134};
        for (unsigned axis = 0; axis != 3; ++axis) {
            SET_GPR_U32(cpu, 31, randomReturn[axis]);
            if (axis) {
                cpu->branch_pc = randomCall[axis];
                write<uint8_t>(ram, particle + 0x33 + axis, static_cast<uint8_t>(GPR_U32(cpu, 2)));
            }
            if (!rotationRandom(ram, cpu, runtime)) return;
            cpu->f[0] = FPU_MUL_S(cpu->f[20 + axis], cpu->f[0]);
            const uint8_t rotation = read<uint8_t>(ram, particle + 0x34 + axis);
            SET_GPR_ZE32(cpu, 2, rotation);
            cpu->f[0] = FPU_ADD_S(cpu->f[23 + axis], cpu->f[0]);
            cpu->f[1] = FPU_MUL_S(cpu->f[26], cpu->f[0]);
            cpu->f[0] = FPU_CVT_S_W(rotation);
            cpu->branch_pc = convertCall[axis];
            cpu->in_delay_slot = false;
            SET_GPR_U32(cpu, 31, convertReturn[axis]);
            cpu->f[12] = FPU_ADD_S(cpu->f[0], cpu->f[1]);
            if (!alphaToUnsigned(ram, cpu, runtime)) return;
        }
        write<uint8_t>(ram, particle + 0x36, static_cast<uint8_t>(GPR_U32(cpu, 2)));
        SET_GPR_S32(cpu, 16, static_cast<int32_t>(read<uint32_t>(ram, particle)));
        if (GPR_U64(cpu, 16) != 0) {
            cpu->pc = 0x1d4050;
            if (runtime->eeCheckpointDue()) return;
        }
    }
    const uint32_t stack = GPR_U32(cpu, 29);
    SET_GPR_VEC(cpu, 31, read<__m128i>(ram, stack + 48));
    SET_GPR_VEC(cpu, 16, read<__m128i>(ram, stack + 32));
    for (int i = 6; i >= 0; --i) cpu->f[20 + i] = read<float>(ram, stack + i * 4);
    const uint32_t target = GPR_U32(cpu, 31);
    SET_GPR_S32(cpu, 29, static_cast<int32_t>(stack + 64));
    cpu->branch_pc = 0x1d416c;
    cpu->in_delay_slot = false;
    cpu->pc = target;
}

// xParCmdMoveRandomPar_Update (0x1d4b20): per particle, two draws of the shared
// iurand LCG jitter x and z by scale*(r-0.5), scale = asset*(dt*0.5). Same
// contract as the other commands: exact prologue/epilogue frame, register and
// control side effects, each draw's call checkpoint and the loop back-edge
// checkpoint; any yield leaves the exact original state at its resume label.
void nativeParticleMoveRandom(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    const uint32_t originalStack = GPR_U32(cpu, 29);
    const uint32_t randomAddress = GPR_U32(cpu, 28) - 0x7768;
    if (cpu->pc != 0x1d4b20 || !regularZero(cpu)
        || (originalStack & 15) || originalStack < 48u || !ordinary(originalStack - 48u, 48)
        || !ordinary(randomAddress, 4)
#if defined(PS2X_STRICT_RETURN_DIAGNOSTICS) && PS2X_STRICT_RETURN_DIAGNOSTICS
        || true
#endif
    ) { reference(originalMoveRandom, ram, cpu, runtime); return; }
    const uint32_t command = GPR_U32(cpu, 4), group = GPR_U32(cpu, 5);
    if (!ordinary(command, 8) || !ordinary(group, 4)
        || overlaps(command, 8, originalStack - 48, 48)
        || overlaps(group, 4, originalStack - 48, 48)
        || !ordinary(read<uint32_t>(ram, command + 4), 0x14)) {
        reference(originalMoveRandom, ram, cpu, runtime); return;
    }
    const uint32_t stack = originalStack - 48;
    SET_GPR_S32(cpu, 29, static_cast<int32_t>(stack));
    write(ram, stack + 32, GPR_VEC(cpu, 31));
    write(ram, stack + 16, GPR_VEC(cpu, 16));
    write(ram, stack + 4, cpu->f[21]);
    write(ram, stack, cpu->f[20]);
    SET_GPR_S32(cpu, 3, 0x3f000000);
    cpu->f[0] = std::bit_cast<float>(0x3f000000u);
    const uint32_t asset = read<uint32_t>(ram, command + 4);
    SET_GPR_S32(cpu, 4, static_cast<int32_t>(asset));
    cpu->f[12] = FPU_MUL_S(cpu->f[12], cpu->f[0]);
    SET_GPR_S32(cpu, 16, static_cast<int32_t>(read<uint32_t>(ram, group)));
    cpu->f[1] = read<float>(ram, asset + 8);
    cpu->f[0] = read<float>(ram, asset + 16);
    cpu->f[20] = FPU_MUL_S(cpu->f[1], cpu->f[12]);
    cpu->branch_pc = 0x1d4b54;
    cpu->f[21] = FPU_MUL_S(cpu->f[0], cpu->f[12]);
    cpu->in_delay_slot = false;
    while (GPR_U64(cpu, 16) != 0) {
        cpu->pc = 0x1d4b5c;
        const uint32_t particle = GPR_U32(cpu, 16);
        if (!ordinary(particle, 0x1c)) { reference(originalMoveRandom, ram, cpu, runtime); return; }
        SET_GPR_U32(cpu, 31, 0x1d4b64);
        if (!rotationRandom(ram, cpu, runtime)) return;
        SET_GPR_S32(cpu, 2, 0x3f000000);
        cpu->f[2] = std::bit_cast<float>(0x3f000000u);
        cpu->f[1] = read<float>(ram, particle + 16);
        cpu->f[0] = FPU_SUB_S(cpu->f[0], cpu->f[2]);
        cpu->f[0] = FPU_MUL_S(cpu->f[20], cpu->f[0]);
        cpu->f[0] = FPU_ADD_S(cpu->f[1], cpu->f[0]);
        SET_GPR_U32(cpu, 31, 0x1d4b84);
        cpu->branch_pc = 0x1d4b7c;
        write(ram, particle + 16, cpu->f[0]);
        cpu->in_delay_slot = false;
        if (!rotationRandom(ram, cpu, runtime)) return;
        SET_GPR_S32(cpu, 3, 0x3f000000);
        cpu->f[2] = std::bit_cast<float>(0x3f000000u);
        cpu->f[1] = read<float>(ram, particle + 24);
        cpu->f[0] = FPU_SUB_S(cpu->f[0], cpu->f[2]);
        cpu->f[0] = FPU_MUL_S(cpu->f[21], cpu->f[0]);
        cpu->f[0] = FPU_ADD_S(cpu->f[1], cpu->f[0]);
        write(ram, particle + 24, cpu->f[0]);
        SET_GPR_S32(cpu, 16, static_cast<int32_t>(read<uint32_t>(ram, particle)));
        if (GPR_U64(cpu, 16) != 0) {
            cpu->pc = 0x1d4b5c;
            if (runtime->eeCheckpointDue()) return;
        }
    }
    const uint32_t frame = GPR_U32(cpu, 29);
    SET_GPR_VEC(cpu, 31, read<__m128i>(ram, frame + 32));
    SET_GPR_VEC(cpu, 16, read<__m128i>(ram, frame + 16));
    cpu->f[21] = read<float>(ram, frame + 4);
    cpu->f[20] = read<float>(ram, frame);
    const uint32_t target = GPR_U32(cpu, 31);
    SET_GPR_S32(cpu, 29, static_cast<int32_t>(frame + 48));
    cpu->branch_pc = 0x1d4bc0;
    cpu->in_delay_slot = false;
    cpu->pc = target;
}

void installNativeParticles(PS2Runtime& runtime) {
    originalVelocity = runtime.lookupFunction(0x1d3f60);
    originalSmoke = runtime.lookupFunction(0x1d48e0);
    originalRotation = runtime.lookupFunction(0x1d3fb0);
    originalMoveRandom = runtime.lookupFunction(0x1d4b20);
    if (!originalVelocity || !originalSmoke || !originalRotation || !originalMoveRandom)
        throw std::runtime_error("Cannot find original retail particle commands");
    // Default since the differential checks (tests/native_particles.cpp) match
    // the originals bit-for-bit, including forced continuations and cycles.
    // NIGHT100_REFERENCE_PARTICLES=1 / setReferenceParticles(true) keeps them.
    if (referenceParticles()) return;
    if (!runtime.replaceFunction(0x1d3f60, nativeParticleVelocityApply)
        || !runtime.replaceFunction(0x1d48e0, nativeParticleSmokeAlpha)
        || !runtime.replaceFunction(0x1d3fb0, nativeParticleRotation)
        || !runtime.replaceFunction(0x1d4b20, nativeParticleMoveRandom))
        throw std::runtime_error("Cannot install native retail particle commands");
}
