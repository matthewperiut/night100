#pragma once
#include <cstdint>
class PS2Runtime;
// The last VU1 microprogram the native path interpreted (image MD5
// 3314bf24608be3b002437a3065687baf) is RenderWare's PS2 Im3D pipeline. Its
// retail owners are the RwIm3DTransform/RenderPrimitive/End units listed in
// nativeVU1OwnerSites (every Im3D caller outside the particle hook and the two
// built-in shadow sites); installNativeVU1 admits them to native_scene's
// ordered Im3D bridge after checking the retail call instructions. Particle
// batches reach the same program only when their PS2 blend pair is
// unsupported; native_scene's ALPHA_1 tracking handles that case. Exact-ELF only.
void installNativeVU1(PS2Runtime& runtime);

struct NativeVU1OwnerSite {
    const char* name;
    // Return addresses of the three SDK calls, the rwPRIMTYPE the unit renders
    // and the instruction that loads it into a0 for RwIm3DRenderPrimitive.
    uint32_t transformReturn,renderReturn,endReturn,primitive,primitiveInstruction;
};
// Retail call-site contracts (return addresses of the three SDK calls).
const NativeVU1OwnerSite* nativeVU1OwnerSites(unsigned& count);
// True if the guest code at the site still holds the three retail JALs and the
// fixed primitive argument. word(address) reads one guest instruction.
bool nativeVU1SiteMatches(const NativeVU1OwnerSite&,uint32_t (*word)(const void*,uint32_t),const void* context);
