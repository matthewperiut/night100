#pragma once
#include <cstdint>
class PS2Runtime;
struct R5900Context;

// Install only inside the exact retail ELF gate, after original registration.
void installNativeParticles(PS2Runtime& runtime);
void nativeParticleVelocityApply(uint8_t*, R5900Context*, PS2Runtime*);
void nativeParticleSmokeAlpha(uint8_t*, R5900Context*, PS2Runtime*);
void nativeParticleRotation(uint8_t*, R5900Context*, PS2Runtime*);
void nativeParticleMoveRandom(uint8_t*, R5900Context*, PS2Runtime*);
