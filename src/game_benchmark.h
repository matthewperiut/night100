#pragma once
#include <cstdint>
class PS2Runtime;
class GL33Backend;
void installGameBenchmark(PS2Runtime&);
void configureGameBenchmark(GL33Backend*,bool nativeCompletions);
void finishGameBenchmark();
void benchmarkFrameCompleted(uint8_t*,PS2Runtime*);
