#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_aux_state.h"
#include "runtime/state_archive.h"
#include "../vendor/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/Pad.h"
#include "../vendor/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/LibC.h"
#include "../vendor/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/GS.h"
#include "../vendor/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/DMA.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks=0;
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
template<class F>void rejects(F function,const char* reason){bool rejected=false;try{function();}catch(const std::exception&){rejected=true;}check(rejected,reason);}
using GuestFunction=void(*)(uint8_t*,R5900Context*,PS2Runtime*);
uint32_t call(PS2Runtime& runtime,GuestFunction function,uint32_t a0=0,uint32_t a1=0,uint32_t a2=0) {
    auto& cpu=runtime.cpu();SET_GPR_U32(&cpu,4,a0);SET_GPR_U32(&cpu,5,a1);SET_GPR_U32(&cpu,6,a2);
    function(runtime.memory().getRDRAM(),&cpu,&runtime);return GPR_U32((&cpu),2);
}
}
int main(){try {
    using namespace ps2_stubs;
    PS2Runtime source;check(source.memory().initialize() && source.syncCoreSubsystems(),"runtime initialization must succeed");
    check(call(source,scePadInit,0)==1,"pad initialization must succeed");
    check(call(source,scePadPortOpen,0,0,0x170000)==1,"pad port zero must open");
    check(call(source,scePadPortOpen,1,0,0x171000)==1,"pad port one must open");
    check(call(source,scePadSetMainMode,0,0,1)==1,"analog mode must configure");
    check(call(source,scePadEnterPressMode,0,0)==1,"pressure mode must configure");
    check(call(source,scePadSetButtonInfo,0,0,0xa57b)==1,"button mask must configure");
    setPadOverrideState(0xb6ed,17,221,43,199);
    check(call(source,scePadRead,0,0,0x172000)==1,"configured pad must read");
    const auto before=getPadDebugSnapshot();
    std::array<uint8_t,32> expected{};std::memcpy(expected.data(),source.memory().getRDRAM()+0x172000,32);
    check(expected[1]==0x73 && expected[2]==0xed && expected[3]==0xb6 && expected[4]==43 && expected[5]==199 && expected[6]==17 && expected[7]==221 && std::any_of(expected.begin()+8,expected.begin()+20,[](uint8_t v){return v!=0;}),"fixture must contain nonneutral analog/pressure data");
    check(call(source,scePadSetReqState,0,0,1)==1,"pending request must configure");
    const auto state=savePadState();
    const auto expectedTransient=call(source,scePadGetState,0,0),expectedStable=call(source,scePadGetState,0,0);
    const auto expectedFrame=call(source,scePadGetFrameCount);
    check(expectedTransient==5 && expectedStable==6,"fixture must exercise one-shot transient state");
    check(call(source,scePadEnd)==1,"old pad ports must close");clearPadOverrideState();
    for(unsigned i=0;i<7;++i)(void)call(source,scePadGetFrameCount);
    PS2Runtime restored;check(restored.memory().initialize() && restored.syncCoreSubsystems(),"fresh runtime must initialize");
    check(call(restored,scePadGetState,0,0)==0,"fresh pad state must be disconnected");
    loadPadState(state);const auto loaded=getPadDebugSnapshot();
    check(loaded.overrideEnabled && loaded.overrideButtons==before.overrideButtons && loaded.overrideRx==43 && loaded.overrideRy==199 && loaded.overrideLx==17 && loaded.overrideLy==221,"pad override state must survive reset and fresh runtime");
    const auto& port=loaded.ports[0][0];
    check(port.open && port.analogMode && port.pressureEnabled && port.buttonMask==0xa57b && port.dmaAddr==0x170000 && port.reqState==1,"port configuration, DMA and request state must survive");
    check(port.readCount==before.ports[0][0].readCount && port.lastReadDataAddr==0x172000 && port.lastReadOk && port.lastUsedOverride && std::equal(expected.begin(),expected.end(),port.lastData),"last read result and raw input bytes must survive");
    check(loaded.ports[1][0].open && !loaded.ports[1][0].analogMode && loaded.ports[1][0].dmaAddr==0x171000,"second port must retain independent state");
    check(call(restored,scePadGetState,0,0)==expectedTransient && call(restored,scePadGetState,0,0)==expectedStable,"restored one-shot transition must match uninterrupted polling");
    check(call(restored,scePadGetFrameCount)==expectedFrame,"pad frame counter must rewind exactly");
    check(call(restored,scePadRead,0,0,0x173000)==1 && !std::memcmp(expected.data(),restored.memory().getRDRAM()+0x173000,32),"restored configured pad must produce identical nonneutral guest read");
    auto broken=state;broken.resize(state.size()-1);rejects([&]{loadPadState(broken);},"truncated pad snapshot must reject");
    check(getPadDebugSnapshot().ports[0][0].open,"invalid restore must leave live pad connected");
    broken=state;broken[0]=99;rejects([&]{loadPadState(broken);},"unknown pad schema must reject");
#if defined(__APPLE__)
    // Keep the port's existing libc sequence while isolating it from unrelated
    // host-library srand/rand calls. Include zero and sign-bit seed edges.
    for(unsigned seed:{0u,1u,2u,123456789u,0x7fffffffu,0x80000000u,0xffffffffu}) {
        std::srand(seed);std::array<uint32_t,10000> sequence{};
        for(auto& value:sequence)value=uint32_t(std::rand())&0x7fff;
        (void)call(source,ps2_stubs::srand,seed);
        bool matches=true;for(const auto expected:sequence)matches&=call(source,ps2_stubs::rand)==expected;
        check(matches,"private Darwin RNG must preserve paired host sequence");
    }
#endif
    (void)call(source,ps2_stubs::srand,29384891);
    for(unsigned i=0;i<33;++i)(void)call(source,ps2_stubs::rand);
    const auto randomState=saveLibcState();std::array<uint32_t,32> expectedRandom{};
    for(auto& value:expectedRandom)value=call(source,ps2_stubs::rand);
    (void)call(restored,ps2_stubs::srand,719);std::srand(84491); // Unrelated host RNG activity.
    loadLibcState(randomState);std::array<uint32_t,32> loadedRandom{};
    for(auto& value:loadedRandom)value=call(restored,ps2_stubs::rand);
    check(loadedRandom==expectedRandom,"restored libc RNG must continue exact guest sequence");
    auto wrongKind=randomState;wrongKind[4]^=1;
    rejects([&]{loadLibcState(wrongKind);},"foreign RNG backend must reject before restore");
    auto wrongAbi=randomState;wrongAbi[8]^=1;
    rejects([&]{loadLibcState(wrongAbi);},"foreign RNG integer ABI must reject before restore");

    // The HLE parameter block is distinct from the GS frontend's registers.
    ps2x::state::Writer gsParameters;gsParameters(uint32_t(1),uint8_t(0),uint8_t(3),uint8_t(1),uint8_t(9));
    loadGsStubState(gsParameters.data);const auto gsState=saveGsStubState();
    ps2x::state::Writer alternateGs;alternateGs(uint32_t(1),uint8_t(1),uint8_t(2),uint8_t(0),uint8_t(3));loadGsStubState(alternateGs.data);
    loadGsStubState(gsState);const auto address=call(restored,sceGsGetGParam);
    check(address==PS2_SCRATCHPAD_BASE+0x100 && restored.memory().getScratchpad()[0x100]==0 && restored.memory().getScratchpad()[0x101]==3 && restored.memory().getScratchpad()[0x102]==1 && restored.memory().getScratchpad()[0x103]==9,"restored GS HLE parameters must feed SDK getter");
    std::array<uint8_t,20> environment{};environment[0]=3;environment[1]=2;environment[2]=1;environment[3]=2;
    const std::array<uint32_t,4> words{0x1265,0x4411,0x200000,0x3000};std::memcpy(environment.data()+4,words.data(),16);
    std::memcpy(source.memory().getRDRAM()+0x174000,environment.data(),environment.size());
    check(call(source,sceDmaPutEnv,0x174000)==0,"DMA fixture environment must configure");const auto dmaState=saveDmaStubState();
    std::memset(source.memory().getRDRAM()+0x174000,0,20);(void)call(source,sceDmaPutEnv,0x174000);
    loadDmaStubState(dmaState);(void)call(restored,sceDmaGetEnv,0x175000);
    check(!std::memcmp(environment.data(),restored.memory().getRDRAM()+0x175000,20),"restored DMA HLE environment must feed SDK getter");
    std::cout<<"Savestate HLE checks: "<<checks<<" passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
