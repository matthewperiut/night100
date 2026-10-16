#pragma once
#include <cstdint>
class PS2Runtime;
class NativeSceneBackend;
void installNativeScene(PS2Runtime&,NativeSceneBackend*);
void configureNativeScene(NativeSceneBackend*);
// An RwIm3DTransform callsite whose Transform/RenderPrimitive/End unit may run
// natively: guest return addresses of the three calls and the rwPRIMTYPE the
// unit always renders (3 triangle list, 4 triangle strip). Exact-ELF callers only.
struct NativeIm3DSite {uint32_t transformReturn,renderReturn,endReturn,primitive;};
void admitNativeIm3DSite(const NativeIm3DSite&);
// GS ALPHA register ((A-B)*C+D) as RenderWare SRCBLEND/DESTBLEND factors for
// the native renderer's additive blend; false for subtractive, reserved or
// FIX!=0x80 forms that it cannot express.
bool nativeBlendFactors(uint64_t alpha,uint32_t& source,uint32_t& destination);
// Diagnostics: the GS ALPHA_1 the native path believes is current, if known.
bool nativeSceneTrackedAlpha(uint64_t& alpha);
#ifdef NIGHT100_NATIVE_SCENE_TESTING
struct R5900Context;
struct NativeModelDraw;
namespace NativeSceneTesting {
using GuestFunction=void(*)(uint8_t*,R5900Context*,PS2Runtime*);
using SubmitFunction=uint64_t(*)(NativeModelDraw);
void setup(GuestFunction transform,GuestFunction render,GuestFunction end,SubmitFunction submit);
void transform(uint8_t*,R5900Context*,PS2Runtime*);
void render(uint8_t*,R5900Context*,PS2Runtime*);
void end(uint8_t*,R5900Context*,PS2Runtime*);
void finishMarker(uint8_t*,R5900Context*,PS2Runtime*);
const NativeModelDraw* pendingDraw();
void setupBlendTracking(GuestFunction setter,GuestFunction init,GuestFunction reemit);
void blendSetter(uint8_t*,R5900Context*,PS2Runtime*);
void blendInit(uint8_t*,R5900Context*,PS2Runtime*);
void blendReemit(uint8_t*,R5900Context*,PS2Runtime*);
void blendConstant(uint8_t*,R5900Context*,PS2Runtime*);
void blendRaw(uint8_t*,R5900Context*,PS2Runtime*);
// Ground particles (0x1af4c0); the submit function from setup() receives batches.
void setupGround(GuestFunction original);
void ground(uint8_t*,R5900Context*,PS2Runtime*);
void groundContinuation(uint8_t*,R5900Context*,PS2Runtime*);
bool groundPending();
void seedBlendAfterRestore(uint8_t* ram,uint32_t gp,uint64_t gsAlpha);
}
#endif
