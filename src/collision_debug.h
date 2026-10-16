#pragma once
#include <cstdint>
class PS2Runtime;

namespace CollisionDebug {
// Caller gates this diagnostic to the exact retail ELF and calls on the guest
// executor after a completed frame. NIGHT100_COLLISION_DEBUG selects a directory.
// This function only reads guest RAM; it never changes collision behavior.
void completed(const uint8_t* ram,uint64_t frame);
// Optional entry/return observers. Installs only when the diagnostic environment
// variable is set; the caller supplies the same exact-ELF identity gate.
void install(PS2Runtime& runtime);
}
