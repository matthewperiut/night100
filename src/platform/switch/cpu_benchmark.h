#pragma once
class PS2Runtime;
// Diagnostic only. Runs before the game executor starts, restores the temporary
// RAM and CPU context, and emits one aggregate record per skeleton size.
void runNativeCpuKernelBenchmark(PS2Runtime& runtime);
