#pragma once
namespace SwitchPgo {
// Writes gcov counters to the instrumented build's SD directory. Safe to call
// repeatedly; does nothing unless the NRO was built with NIGHT100_PGO_GEN.
void dumpCounters(const char* reason);
// Creates the instrumented build's SD directory before the first counter write.
void prepareDirectory();
}
