#include "switch_pgo.h"
#include <cstdio>
#include <sys/stat.h>

#if defined(NIGHT100_PGO_GEN)
extern "C" void __gcov_dump(void);
#ifndef NIGHT100_PGO_RUNTIME_DIR
#error "NIGHT100_PGO_RUNTIME_DIR must name the SD directory used for gcov output"
#endif
#endif

namespace SwitchPgo {
void prepareDirectory() {
#if defined(NIGHT100_PGO_GEN)
    // libgcov writes into the compiled path but does not create it on libnx.
    // EEXIST is expected on repeated launches.
    mkdir(NIGHT100_PGO_RUNTIME_DIR,0777);
#endif
}
void dumpCounters(const char* reason) {
#if defined(NIGHT100_PGO_GEN)
    std::printf("[pgo] gcov_dump reason=%s dir=%s\n",reason,NIGHT100_PGO_RUNTIME_DIR);
    __gcov_dump();
#else
    (void)reason;
#endif
}
}
