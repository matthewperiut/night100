#pragma once
#include <cstdlib>
#include <cstring>
// Shared opt-out for the native particle paths: the Ground render callback
// and the particle update commands. Desktop: NIGHT100_REFERENCE_PARTICLES=1.
// The Switch has no environment; its launcher calls setReferenceParticles
// (for example from --reference-particles) before the game overrides install.
namespace NativeParticlePolicy {
inline bool& referenceFlag() {
    static bool value=[] {
        const char* setting=std::getenv("NIGHT100_REFERENCE_PARTICLES");
        return setting && !std::strcmp(setting,"1");
    }();
    return value;
}
}
inline void setReferenceParticles(bool reference){NativeParticlePolicy::referenceFlag()=reference;}
inline bool referenceParticles(){return NativeParticlePolicy::referenceFlag();}
