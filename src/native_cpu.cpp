#include "native_cpu.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstring>
#include <stdexcept>

namespace {
PS2Runtime::RecompiledFunction originalQuat = nullptr;
PS2Runtime::RecompiledFunction originalMat = nullptr;
PS2Runtime::RecompiledFunction originalAnimMatrices = nullptr;

bool ramRange(uint32_t address, uint32_t size) {
    return address <= PS2_RAM_SIZE - size && !(address & 3u);
}
bool overlaps(uint32_t first, uint32_t second, uint32_t size) {
    return first < second + size && second < first + size;
}
float loadFloat(const uint8_t* address) {
    float value;
    std::memcpy(&value, address, sizeof(value));
    return value;
}
void storeFloat(uint8_t* address, float value) {
    std::memcpy(address, &value, sizeof(value));
}
void fallback(PS2Runtime::RecompiledFunction original, uint8_t* ram,
              R5900Context* cpu, PS2Runtime* runtime) {
    if (!original) throw std::runtime_error("Native CPU kernel lacks original fallback");
    original(ram, cpu, runtime);
}
}

void nativeQuatToMat(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    const uint32_t source = GPR_U32(cpu, 4), target = GPR_U32(cpu, 5);
    if (!ramRange(source, 16) || !ramRange(target, 44)) {
        fallback(originalQuat, ram, cpu, runtime);
        return;
    }
    // This leaf reads its complete quaternion before its first store, including
    // when input and output overlap. Keep the PS2 operation order and exceptional
    // float semantics, but keep intermediate values in host locals instead of
    // repeatedly touching the emulated register file and dispatching RAM helpers.
    float f[13];
    f[8] = loadFloat(ram + source);
    f[10] = loadFloat(ram + source + 4);
    f[4] = loadFloat(ram + source + 8);
    f[3] = loadFloat(ram + source + 12);
    f[0] = FPU_MUL_S(2.0f, f[8]);
    f[1] = FPU_MUL_S(2.0f, f[10]);
    f[2] = FPU_MUL_S(2.0f, f[4]);
    f[11] = FPU_MUL_S(f[2], f[4]);
    f[9] = FPU_MUL_S(f[1], f[10]);
    f[5] = FPU_MUL_S(f[0], f[3]);
    f[6] = FPU_MUL_S(f[1], f[3]);
    f[4] = FPU_MUL_S(f[0], f[8]);
    f[12] = 1.0f;
    f[7] = FPU_MUL_S(f[1], f[8]);
    f[0] = FPU_SUB_S(FPU_SUB_S(f[12], f[9]), f[11]);
    storeFloat(ram + target, f[0]);
    f[0] = FPU_SUB_S(f[12], f[11]);
    f[1] = FPU_SUB_S(f[0], f[4]);
    f[0] = FPU_SUB_S(f[12], f[4]);
    f[8] = FPU_MUL_S(f[2], f[8]);
    f[3] = FPU_MUL_S(f[2], f[3]);
    f[10] = FPU_MUL_S(f[2], f[10]);
    f[2] = FPU_SUB_S(f[7], f[3]);
    storeFloat(ram + target + 4, f[2]);
    f[4] = FPU_ADD_S(f[8], f[6]);
    f[3] = FPU_ADD_S(f[7], f[3]);
    storeFloat(ram + target + 8, f[4]);
    storeFloat(ram + target + 16, f[3]);
    storeFloat(ram + target + 20, f[1]);
    f[3] = FPU_SUB_S(f[10], f[5]);
    f[2] = FPU_SUB_S(f[8], f[6]);
    storeFloat(ram + target + 24, f[3]);
    f[1] = FPU_ADD_S(f[10], f[5]);
    storeFloat(ram + target + 32, f[2]);
    f[0] = FPU_SUB_S(f[0], f[9]);
    storeFloat(ram + target + 36, f[1]);
    storeFloat(ram + target + 40, f[0]);
    const uint32_t zero = GPR_U32(cpu, 0);
    std::memcpy(ram + target + 12, &zero, 4);
    std::memcpy(cpu->f, f, sizeof(f));
    SET_GPR_S32(cpu, 3, 0x3f800000);
    cpu->branch_pc = 0x1135a4;
    cpu->in_delay_slot = false;
    cpu->pc = GPR_U32(cpu, 31);
}

void nativeMat3x3Mul(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    const uint32_t target = GPR_U32(cpu, 4), left = GPR_U32(cpu, 5);
    const uint32_t right = GPR_U32(cpu, 6), stack = GPR_U32(cpu, 29);
    // Preserve the original's unusual alias path: it copies all 48 bytes from
    // a temporary guest matrix, including untouched padding. Partial overlaps,
    // unusual guest mappings and stack aliases retain the original routine.
    if (!ramRange(target, 48) || !ramRange(left, 48) || !ramRange(right, 48)
        || stack < 64 || !ramRange(stack - 64, 64)
        || overlaps(target, left, 48) || overlaps(target, right, 48)
        || overlaps(stack - 64, target, 64) || overlaps(stack - 64, left, 64)
        || overlaps(stack - 64, right, 64)
        || GPR_U64(cpu, 0) != 0 || GPR_U64(cpu, 4) != target
        || GPR_U64(cpu, 5) != left || GPR_U64(cpu, 6) != right) {
        fallback(originalMat, ram, cpu, runtime);
        return;
    }
    const __m128i savedReturn = GPR_VEC(cpu, 31);
    std::memcpy(ram + stack - 64, &savedReturn, 16);
    float a[12], b[12];
    std::memcpy(a, ram + left, sizeof(a));
    std::memcpy(b, ram + right, sizeof(b));
    float product0 = 0.0f, product1 = 0.0f, accumulator = 0.0f, result = 0.0f;
    for (unsigned row = 0; row != 3; ++row) {
        for (unsigned column = 0; column != 3; ++column) {
            product0 = FPU_MUL_S(a[row * 4], b[column]);
            product1 = FPU_MUL_S(a[row * 4 + 1], b[4 + column]);
            accumulator = FPU_ADD_S(product0, product1);
            result = FPU_ADD_S(accumulator, FPU_MUL_S(a[row * 4 + 2], b[8 + column]));
            storeFloat(ram + target + row * 16 + column * 4, result);
        }
    }
    const uint32_t zero = 0;
    std::memcpy(ram + target + 12, &zero, 4);
    // Preserve even caller-save register results. Translated callers can contain
    // internal continuations that inspect more than the platform C ABI requires.
    cpu->f[0] = result;
    cpu->f[1] = a[10];
    cpu->f[2] = product1;
    cpu->f[3] = a[9];
    cpu->f[4] = product0;
    cpu->f[5] = a[8];
    FPU_SET_ACC(cpu, accumulator);
    SET_GPR_VEC(cpu, 3, GPR_VEC(cpu, 4));
    SET_GPR_U64(cpu, 7, 0);
    SET_GPR_S32(cpu, 29, static_cast<int32_t>(stack));
    cpu->branch_pc = 0x1141b8;
    cpu->in_delay_slot = false;
    cpu->pc = GPR_U32(cpu, 31);
}


namespace {
// Per-bone animation state carried between bones. Each bone is first computed
// with raw SSE arithmetic while recording any lane whose PS2 result would need
// the exact vfinish path (exponent 0 with mantissa, exponent 255, or the chop
// overflow endpoint). Such a bone is replayed through the exact PS2 operations
// from the saved state, so the common path has no per-operation branch.
struct AnimBone {
    __m128 acc = _mm_setzero_ps(), world[4] = {}, rotation[3] = {}, doubled = _mm_setzero_ps(),
           perm2 = _mm_setzero_ps(), perm3 = _mm_setzero_ps(), perm4 = _mm_setzero_ps(),
           position = _mm_setzero_ps();
    __m128i t0 = _mm_setzero_si128(), t1 = _mm_setzero_si128(), t2 = _mm_setzero_si128();
};
enum class AnimOp { Add, Sub, Mul };
inline __m128 specialLanes(__m128 r) {
    const __m128i x = _mm_and_si128(_mm_castps_si128(r), _mm_set1_epi32(0x7fffffff));
    const __m128i denormal = _mm_and_si128(_mm_cmpgt_epi32(x, _mm_setzero_si128()),
                                           _mm_cmplt_epi32(x, _mm_set1_epi32(0x00800000)));
    return _mm_castsi128_ps(_mm_or_si128(denormal, _mm_cmpgt_epi32(x, _mm_set1_epi32(0x7f7ffffe))));
}
template<AnimOp Op, bool Exact>
inline __m128 animOp(__m128 a, __m128 b, __m128& special) {
    if constexpr (Exact) {
        if constexpr (Op == AnimOp::Add) return PS2_VADD(a, b);
        else if constexpr (Op == AnimOp::Sub) return PS2_VSUB(a, b);
        else return PS2_VMUL(a, b);
    } else {
        const __m128 r = Op == AnimOp::Add ? _mm_add_ps(a, b) : Op == AnimOp::Sub ? _mm_sub_ps(a, b) : _mm_mul_ps(a, b);
        special = _mm_or_ps(special, specialLanes(r));
        return r;
    }
}
// Computes one bone from the previous state. Exact=false returns a nonzero
// special mask if the raw result must be replayed with Exact=true.
template<bool Exact>
__m128 animBone(AnimBone& s, const __m128* parent, __m128* nextParent, __m128 q,
                uint32_t tx, uint32_t ty, uint32_t tz, __m128 vf0, __m128 ones, __m128 one) {
    const __m128 zero = _mm_setzero_ps();
    __m128 special = zero;
    auto xyz = [](__m128 previous, __m128 value) { return _mm_blend_ps(previous, value, 7); };
    auto add = [&](__m128 a, __m128 b) { return animOp<AnimOp::Add, Exact>(a, b, special); };
    auto sub = [&](__m128 a, __m128 b) { return animOp<AnimOp::Sub, Exact>(a, b, special); };
    auto mul = [&](__m128 a, __m128 b) { return animOp<AnimOp::Mul, Exact>(a, b, special); };
    __m128& acc = s.acc;
    __m128* world = s.world;
    __m128* rotation = s.rotation;
    __m128& doubled = s.doubled;
    __m128& perm2 = s.perm2;
    __m128& perm3 = s.perm3;
    __m128& perm4 = s.perm4;
    __m128& position = s.position;
    __m128i& t0 = s.t0;
    __m128i& t1 = s.t1;
    __m128i& t2 = s.t2;
    doubled = xyz(q, add(q, q));
    t0 = _mm_castps_si128(q);
    t1 = PS2_PCPYLD(t0, _mm_unpackhi_epi64(t0, _mm_setzero_si128()));
    t2 = PS2_QFSRV(PS2_PEXEW(t0), PS2_PEXEW(t0), 32);
    // Broadcast before subtracting, just as the VU does: an unused
    // exponent-255 lane can select the shared vector slow path and change
    // signed-zero/denormal handling in lanes that will be written.
    perm3 = _mm_blend_ps(_mm_castsi128_ps(t1), sub(vf0, _mm_shuffle_ps(_mm_castsi128_ps(t1), _mm_castsi128_ps(t1), _MM_SHUFFLE(0,0,0,0))), 1);
    t1 = PS2_QFSRV(t2, t2, 32);
    perm2 = _mm_blend_ps(_mm_castsi128_ps(t2), sub(vf0, _mm_shuffle_ps(_mm_castsi128_ps(t2), _mm_castsi128_ps(t2), _MM_SHUFFLE(1,1,1,1))), 2);
    t1 = PS2_QFSRV(t1, t1, 32);
    perm4 = _mm_blend_ps(_mm_castsi128_ps(t1), sub(vf0, _mm_shuffle_ps(_mm_castsi128_ps(t1), _mm_castsi128_ps(t1), _MM_SHUFFLE(2,2,2,2))), 4);
    // Scalar LW changes only the low 64 bits of each packed EE register.
    auto scalarLoad = [](__m128i old, uint32_t bits) {
        return _mm_unpacklo_epi64(_mm_cvtsi64_si128(int64_t(int32_t(bits))), _mm_srli_si128(old, 8));
    };
    t0 = scalarLoad(t0, tx);
    t1 = scalarLoad(t1, ty);
    t2 = scalarLoad(t2, tz);
    const __m128 x = _mm_shuffle_ps(doubled, doubled, _MM_SHUFFLE(0,0,0,0));
    const __m128 y = _mm_shuffle_ps(doubled, doubled, _MM_SHUFFLE(1,1,1,1));
    const __m128 z = _mm_shuffle_ps(doubled, doubled, _MM_SHUFFLE(2,2,2,2));
    acc = xyz(acc, mul(perm3, z)); acc = _mm_blend_ps(acc, add(acc, mul(ones, one)), 1);
    rotation[0] = xyz(rotation[0], sub(acc, mul(perm2, y)));
    acc = xyz(acc, mul(perm2, x)); acc = _mm_blend_ps(acc, add(acc, mul(ones, one)), 2);
    rotation[1] = xyz(rotation[1], sub(acc, mul(perm4, z)));
    acc = xyz(acc, mul(perm4, y)); acc = _mm_blend_ps(acc, add(acc, mul(ones, one)), 4);
    rotation[2] = xyz(rotation[2], sub(acc, mul(perm3, x)));
    t0 = PS2_PCPYLD(t2, PS2_PEXTLW(t1, t0));
    position = _mm_castsi128_ps(t0);
    auto transform = [&](const __m128 v) {
        acc = xyz(acc, mul(parent[0], _mm_shuffle_ps(v, v, _MM_SHUFFLE(0,0,0,0))));
        acc = xyz(acc, add(acc, mul(parent[1], _mm_shuffle_ps(v, v, _MM_SHUFFLE(1,1,1,1)))));
        return add(acc, mul(parent[2], _mm_shuffle_ps(v, v, _MM_SHUFFLE(2,2,2,2))));
    };
    for (unsigned j = 0; j < 3; ++j) world[j] = xyz(world[j], transform(rotation[j]));
    world[0] = _mm_blend_ps(world[0], zero, 8);
    acc = xyz(acc, transform(position));
    world[3] = xyz(world[3], add(acc, mul(parent[3], one)));
    for (unsigned j = 0; j < 4; ++j) nextParent[j] = mul(world[j], one);
    return special;
}
}

void nativeModelAnimMatrices(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    // The original also has a scheduler continuation registered at 0x121f44.
    // Retain it for suspended calls and all noncanonical/aliased guest layouts.
    const uint32_t model = GPR_U32(cpu, 4), quats = GPR_U32(cpu, 5);
    const uint32_t translations = GPR_U32(cpu, 6), output = GPR_U32(cpu, 7);
    const uint32_t sp = GPR_U32(cpu, 29);
    auto read = [ram](uint32_t at) { uint32_t v; std::memcpy(&v, ram + at, 4); return v; };
    auto reject = [&] { fallback(originalAnimMatrices, ram, cpu, runtime); };
    if (cpu->pc == 0x121f44 || !ramRange(model, 8) || sp < 2048
        || !ramRange(sp - 2048, 2048)
        || _mm_movemask_epi8(_mm_cmpeq_epi32(GPR_VEC(cpu, 0), _mm_setzero_si128())) != 0xffff
        || _mm_movemask_epi8(_mm_cmpeq_epi32(_mm_castps_si128(cpu->vu0_vf[0]),
              _mm_set_epi32(0x3f800000, 0, 0, 0))) != 0xffff) { reject(); return; }
    const uint32_t frame = read(model + 4);
    if (!ramRange(frame, 156)) { reject(); return; }
    const uint32_t extension = read(frame + 152), plugin = read(0x2caa88);
    const uint64_t pluginAddress = uint64_t(extension) + plugin;
    if (!extension || pluginAddress > PS2_RAM_SIZE - 8 || !ramRange(uint32_t(pluginAddress), 8)) {
        reject(); return;
    }
    const uint32_t hierarchy = read(uint32_t(pluginAddress) + 4);
    if (!hierarchy || !ramRange(hierarchy, 52)) { reject(); return; }
    const uint32_t count = read(hierarchy + 4), nodes = read(hierarchy + 48);
    if (!count || count > 256 || !ramRange(nodes, (count + 1) * 16)
        || !ramRange(quats, count * 16) || !ramRange(translations, count * 12)
        || !ramRange(output, count * 64) || ((quats | output | (sp - 2048)) & 15)) {
        reject(); return;
    }
    struct Range { uint32_t address, size; };
    const Range inputs[] = {{model,8},{frame,156},{uint32_t(pluginAddress),8},
        {hierarchy,52},{nodes,(count+1)*16},{quats,count*16},{translations,count*12},{0x2caa88,4}};
    const Range writes[] = {{output,count*64},{sp-2048,2048}};
    auto intersects = [](Range a, Range b) {
        return a.address < b.address + b.size && b.address < a.address + a.size;
    };
    if (intersects(writes[0], writes[1])) { reject(); return; }
    for (auto a : writes) for (auto b : inputs) if (intersects(a,b)) { reject(); return; }
    int depth = 1;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t flags = read(nodes + i * 16 + 8);
        if (flags & 2) ++depth;
        if (depth > 32 || ((flags & 1) && --depth < 0)) { reject(); return; }
    }
    // Keep matrix and quaternion intermediates in host locals. PS2 operations
    // retain chop/denormal/exponent-255 handling and never contract into FMA.
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    auto xyz = [](__m128 previous, __m128 value) { return _mm_blend_ps(previous, value, 7); };
    auto load = [ram](uint32_t at) { __m128 v; std::memcpy(&v, ram+at, 16); return v; };
    auto store = [ram](uint32_t at, __m128 v) { std::memcpy(ram+at, &v, 16); };
    __m128 parent[4] = {_mm_set_ps(0,0,0,1),_mm_set_ps(0,0,1,0),_mm_set_ps(0,1,0,0),zero};
    AnimBone bone;
    for (unsigned j = 0; j < 4; ++j) bone.world[j] = cpu->vu0_vf[9 + j];
    for (unsigned j = 0; j < 3; ++j) bone.rotation[j] = cpu->vu0_vf[16 + j];
    bone.acc = cpu->vu0_acc;
    const __m128 ones = xyz(cpu->vu0_vf[5], one), vf0 = cpu->vu0_vf[0];
    uint32_t top = sp - 2048, currentFlags = read(nodes + 8), done = 0;
    for (unsigned j = 0; j < 4; ++j) store(top + j*16, parent[j]);
    top += 64;
    auto exportState = [&] {
        SET_GPR_S32(cpu, 1, 0x2d0000);
        SET_GPR_U64(cpu, 3, currentFlags & 2);
        SET_GPR_S32(cpu, 4, nodes + done*16);
        SET_GPR_S32(cpu, 5, quats + done*16);
        SET_GPR_S32(cpu, 6, translations + done*12);
        SET_GPR_S32(cpu, 7, output + done*64);
        SET_GPR_VEC(cpu, 8, bone.t0); SET_GPR_VEC(cpu, 9, bone.t1); SET_GPR_VEC(cpu, 10, bone.t2);
        SET_GPR_S32(cpu, 11, currentFlags);
        SET_GPR_VEC(cpu, 12, _mm_setzero_si128()); SET_GPR_S32(cpu, 12, done);
        SET_GPR_S32(cpu, 13, count); SET_GPR_S32(cpu, 14, top);
        cpu->sa = 32;
        cpu->vu0_vf[1]=bone.doubled; cpu->vu0_vf[2]=bone.perm2; cpu->vu0_vf[3]=bone.perm3;
        cpu->vu0_vf[4]=bone.perm4; cpu->vu0_vf[5]=ones;
        for(unsigned j=0;j<4;++j) { cpu->vu0_vf[9+j]=bone.world[j]; cpu->vu0_vf[20+j]=parent[j]; }
        for(unsigned j=0;j<3;++j) cpu->vu0_vf[16+j]=bone.rotation[j];
        cpu->vu0_vf[19]=bone.position; cpu->vu0_acc=bone.acc;
        cpu->in_delay_slot=false;
    };
    // The compiled loop enters through its bottom condition, so the original
    // checks scheduling before the first bone as well as between bones.
    if (runtime->eeCheckpointDue()) {
        SET_GPR_S32(cpu, 1, 0x2d0000); SET_GPR_U64(cpu, 3, currentFlags & 2);
        SET_GPR_S32(cpu, 4, nodes); SET_GPR_S32(cpu, 11, currentFlags);
        SET_GPR_VEC(cpu, 12, _mm_setzero_si128());
        SET_GPR_S32(cpu, 13, count); SET_GPR_S32(cpu, 14, top);
        SET_GPR_S32(cpu, 29, sp-2048);
        for (unsigned j=0;j<4;++j) cpu->vu0_vf[20+j]=parent[j];
        cpu->branch_pc=0x12207c; cpu->pc=0x121f44; cpu->in_delay_slot=false;
        return;
    }
    while (done < count) {
        if (currentFlags & 2) {
            for (unsigned j=0;j<4;++j) store(top+j*16,parent[j]);
            top+=64;
        }
        const __m128 q = load(quats+done*16);
        const uint32_t tx = read(translations+done*12), ty = read(translations+done*12+4);
        const uint32_t tz = read(translations+done*12+8);
        __m128 nextParent[4];
        AnimBone next = bone;
        if (_mm_movemask_epi8(_mm_castps_si128(animBone<false>(next, parent, nextParent, q, tx, ty, tz, vf0, ones, one))) != 0) {
            next = bone;
            animBone<true>(next, parent, nextParent, q, tx, ty, tz, vf0, ones, one);
        }
        bone = next;
        for(unsigned j=0;j<4;++j) store(output+done*64+j*16,bone.world[j]);
        if(currentFlags&1) {
            top-=64; for(unsigned j=0;j<4;++j) parent[j]=load(top+j*16);
        } else {
            for(unsigned j=0;j<4;++j) parent[j]=nextParent[j];
        }
        ++done;
        currentFlags=read(nodes+done*16+8);
        if(done<count && runtime->eeCheckpointDue()) {
            exportState(); SET_GPR_S32(cpu,29,sp-2048);
            cpu->branch_pc=0x12207c; cpu->pc=0x121f44;
            return;
        }
    }
    exportState(); SET_GPR_S32(cpu,29,sp);
    cpu->branch_pc=0x122084; cpu->pc=GPR_U32(cpu,31);
}

void installNativeCpu(PS2Runtime& runtime, bool animationMatrices) {
    originalQuat = runtime.lookupFunction(0x113500);
    originalMat = runtime.lookupFunction(0x113fe0);
    originalAnimMatrices = runtime.lookupFunction(0x121ed0);
    if (!originalQuat || !originalMat || !originalAnimMatrices
        || !runtime.replaceFunction(0x113500, nativeQuatToMat)
        || !runtime.replaceFunction(0x113fe0, nativeMat3x3Mul)
        || (animationMatrices && !runtime.replaceFunction(0x121ed0, nativeModelAnimMatrices)))
        throw std::runtime_error("Cannot install native retail CPU kernels");
}
