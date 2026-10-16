#include "frame_phases.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "render_image.h"
#if !defined(__SWITCH__)
#include "render_trace.h"
#endif
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace FramePhases {
namespace {
// Retail zGameLoop (0x131920) call boundaries. A hook marks only its loop
// call site (return address), so the same routines called from zSceneRender
// or menus do not move the split. The two pacing PCs are the frame-wait
// overrides' entries and mark on any arrival.
struct Hook {
    uint32_t pc,returnAddress;Phase phase;
    PS2Runtime::RecompiledFunction original=nullptr;
};
std::array<Hook,12> hooks{{
    {0x112a80,0x131a78,Pad},        // xPadUpdate
    {0x1342d0,0x131c5c,Update},     // zSceneUpdate
    {0x1b5990,0x131c64,PostUpdate}, // zEntShaggy_UpdateDep
    {0x13c6f0,0x131d10,RenderPrep}, // xCameraBegin
    {0x133920,0x131d20,Render},     // zSceneRender
    {0x120050,0x131d28,RenderEnd},  // xDebugUpdate
    {0x13a140,0x131d90,CameraEnd},  // xCameraEnd
    {0x13c7c0,0x131d98,FxEnd},      // iEnvEndRenderFX
    {0x131d98,0,Wait},              // frame wait (first visit)
    {0x131da8,0,Wait},              // frame wait (resumed poll)
    {0x13a130,0x131dd4,Present},    // xCameraShowRaster
    {0x2773d0,0x131ddc,PostPresent},// zLaughUpdate
}};
#if !defined(__SWITCH__)
bool tracing=false;
#endif
BoundaryHook boundaryObserver=nullptr;
void boundaryHook(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    const uint32_t pc=cpu->pc;
    for(const auto& hook:hooks) {
        if(hook.pc!=pc)continue;
        if(!hook.returnAddress || GPR_U32(cpu,31)==hook.returnAddress) {
            if(boundaryObserver && boundaryObserver(state.current,hook.phase,ram,cpu,runtime))return;
#if !defined(__SWITCH__)
            if(tracing)RenderTrace::transition(state.current,hook.phase,ram,cpu,runtime);
#endif
            mark(hook.phase);
        }
        hook.original(ram,cpu,runtime);return;
    }
    throw std::runtime_error("Unexpected frame phase hook PC");
}
}
void setBoundaryHook(BoundaryHook hook){boundaryObserver=hook;}
namespace {
uint32_t word(PS2Runtime& runtime,uint32_t address) {
    uint32_t value=0;std::memcpy(&value,runtime.memory().getRDRAM()+address,4);return value;
}
}
void install(PS2Runtime& runtime) {
    if(state.installed)return;
    // The loop's JAL words and the two wait PCs' original loads must match
    // this ELF, so a changed call site cannot silently mislabel a phase.
    for(const auto& hook:hooks) {
        const uint32_t expected=hook.returnAddress?0x0c000000u|(hook.pc>>2):
            (hook.pc==0x131d98?0x8fa20100u:0x8f838938u);
        const uint32_t site=hook.returnAddress?hook.returnAddress-8:hook.pc;
        if(word(runtime,site)!=expected)throw std::runtime_error("Frame phase call site does not match the retail game loop");
    }
    for(auto& hook:hooks) {
        hook.original=runtime.lookupFunction(hook.pc);
        if(!hook.original || !runtime.replaceFunction(hook.pc,boundaryHook))
            throw std::runtime_error("Cannot install frame phase timing");
    }
    state=State{};state.installed=true;
    RenderImage::install(runtime);
#if !defined(__SWITCH__)
    tracing=RenderTrace::install(runtime);
#endif
    std::cout<<"[night100] frame phase timing at "<<hooks.size()<<" zGameLoop boundaries\n";
}
}
