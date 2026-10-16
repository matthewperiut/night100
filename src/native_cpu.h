#pragma once

#include <cstdint>

class PS2Runtime;
struct R5900Context;

// Call only from the exact retail ELF override after original registration.
void installNativeCpu(PS2Runtime& runtime, bool animationMatrices = false);
void nativeQuatToMat(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime);
void nativeMat3x3Mul(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime);
void nativeModelAnimMatrices(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime);
