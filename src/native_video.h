#pragma once
#include <cstdint>
class PS2Runtime;
struct R5900Context;

namespace NativeVideo {
// Exact-ELF iFMVPlay hook. Runs on the game thread and blocks until playback
// finishes/skips/stops; decoded frames cross a bounded queue to the window.
void play(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime);
// Main window thread, inside BeginDrawing/EndDrawing, before other overlays.
void tickAndDraw(PS2Runtime& runtime);
// Main window shutdown callback, before destroying its GL/audio resources.
void shutdown();
bool active();
// Explicit exact-ELF attract regression route; normal launches leave it disabled.
void installAttractDiagnostic(PS2Runtime& runtime);
void diagnosticMenuTick(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime);
}
