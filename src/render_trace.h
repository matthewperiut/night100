#pragma once
// Desktop diagnostic for the decoupled-render experiments.
// NIGHT100_RENDER_TRACE=<directory> records, on sampled zGameLoop iterations,
// which guest RAM bytes each frame phase changed (snapshot diff), which
// 16 KiB host pages each phase touched (mprotect faults), which guest
// functions each phase entered and on which guest thread, and the dynamic
// data the native draw hooks consume. Off by default; distorts timing.
#include "frame_phases.h"
#include <cstdint>

class PS2Runtime;
struct R5900Context;

namespace RenderTrace {
// Returns whether tracing is enabled. Installs the dispatch trampoline last.
bool install(PS2Runtime&);
void transition(FramePhases::Phase ended,FramePhases::Phase next,uint8_t* ram,R5900Context*,PS2Runtime*);
}
