#include "native_vu1_lastprogram.h"
#include "native_scene.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
// Every retail RwIm3DTransform caller outside the particle hook and the two
// built-in shadow sites. Each unit is RwIm3DTransform -> RwIm3DRenderPrimitive
// -> RwIm3DEnd with no state setter inside it; all but iShadowRender skip
// Render/End when Transform returns NULL. The bridge still requires a 36-byte
// XYZ|RGBA|UV batch with no matrix, the site's primitive and supported render
// state; anything else runs the original unit (and its VU1 program).
constexpr NativeVU1OwnerSite ownerSites[]={
    // iShadow.cpp: one batch per frame of blob shadows under entities.
    {"iShadowRender",0x1a16f0,0x1a16f8,0x1a1700,3,0x1a16f4},
    // zFeet.cpp: footprint/skid-mark triangle strips (up to 256 vertices).
    {"DrawSkidMarkStrip",0x1db1b4,0x1db1c4,0x1db1cc,4,0x1db1b8},
    // The remaining callers are not exercised by the desktop replays.
    {"SuperVillainGumDraw",0x1488b0,0x1488c0,0x1488c8,4,0x1488b4},  // zEntVillain.cpp
    {"xShadowReceiveShadow#1",0x1c3f88,0x1c3f98,0x1c3fa0,3,0x1c3f94}, // xShadow.cpp
    {"xShadowReceiveShadow#2",0x1c4434,0x1c4444,0x1c444c,3,0x1c4440},
    {"RenderLightning#1",0x24b098,0x24b0a8,0x24b0b0,4,0x24b09c},      // zLightning.cpp
    {"RenderLightning#2",0x24b2a8,0x24b2b8,0x24b2c0,4,0x24b2ac},
    {"RenderActionLine",0x279fd8,0x279fe8,0x279ff0,4,0x279fdc},        // zActionLine.cpp
};
constexpr uint32_t jal(uint32_t target){return 0x0c000000u|((target>>2)&0x03ffffffu);}

// Diagnostics, all opt-in. NIGHT100_VU1_CALLER attributes each MSCAL of the
// interpreted VU1 program to the RwIm3DTransform caller whose vertices it
// received (VU data at TOP matches a recorded input position).
// NIGHT100_VU1_SKIP=iShadowRender,DrawSkidMarkStrip,Particles disables an
// owner's draw to confirm attribution against launch counts and captures.
PS2Runtime* vu1Runtime=nullptr;
struct Im3DRecord {uint64_t serial;uint32_t ra;bool alphaKnown;uint64_t alpha;std::vector<float> xyz;};
// Per owner: GS draws whose ALPHA_1 matched / differed from the tracked value.
std::map<uint32_t,std::array<uint64_t,3>> alphaChecks;
std::vector<Im3DRecord> im3dRing;
uint64_t im3dSerial=0,probeLaunches=0;
std::map<uint32_t,uint64_t> launchOwners,transformCallers;
PS2Runtime::RecompiledFunction probedTransform=nullptr;
void probeTransform(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc==0x209318) {
        Im3DRecord record{++im3dSerial,GPR_U32(cpu,31),false,0,{}};
        if(++transformCallers[record.ra]==1 || (im3dSerial%2000)==0)
            for(const auto& [ra,n]:transformCallers)std::cout<<"[vu1-transform] ra=0x"<<std::hex<<ra<<std::dec<<" calls="<<n<<'\n';
        record.alphaKnown=nativeSceneTrackedAlpha(record.alpha);
        const uint32_t vertices=GPR_U32(cpu,4),count=std::min<uint32_t>(GPR_U32(cpu,5),4096);
        if(vertices>=0x1000 && vertices+size_t(count)*36<=PS2_RAM_SIZE) {
            record.xyz.resize(size_t(count)*3);
            for(uint32_t i=0;i<count;++i)std::memcpy(&record.xyz[i*3],ram+vertices+i*36,12);
        }
        if(im3dRing.size()>=256)im3dRing.erase(im3dRing.begin());
        im3dRing.push_back(std::move(record));
    }
    probedTransform(ram,cpu,runtime);
}
void probeMscal(uint32_t startPC,uint32_t top,uint32_t itop) {
    R5900Context* cpu=vu1Runtime->eeScheduler().currentContext();
    if(!cpu)cpu=&vu1Runtime->cpu();
    float v[3];std::memcpy(v,vu1Runtime->memory().getVU1Data()+(top&0x3ffu)*16,12);
    uint32_t owner=0;uint64_t age=0;const Im3DRecord* matched=nullptr;
    for(auto it=im3dRing.rbegin();it!=im3dRing.rend() && !owner;++it)
        for(size_t i=0;i+2<it->xyz.size();i+=3)
            if(it->xyz[i]==v[0] && it->xyz[i+1]==v[1] && it->xyz[i+2]==v[2]){owner=it->ra;age=im3dSerial-it->serial;matched=&*it;break;}
    uint64_t lastSeq=0;
    {const auto history=vu1Runtime->gs().getDebugHistory();if(!history.empty())lastSeq=history.back().seq;}
    ++launchOwners[owner];
    if(probeLaunches<64)std::cout<<"[vu1-caller] start=0x"<<std::hex<<startPC<<std::dec<<" top="<<top<<" itop="<<itop
        <<" im3d_ra=0x"<<std::hex<<owner<<std::dec<<" age="<<age<<'\n';
    if((++probeLaunches&255)==0)
        for(const auto& [ra,n]:launchOwners)std::cout<<"[vu1-owner] im3d_ra=0x"<<std::hex<<ra<<std::dec<<" launches="<<n<<'\n';
    // Identical to the runtime's default MSCAL callback.
    auto& vu=vu1Runtime->vu1();
    vu.state().dBitEnabled=(cpu->vu0_fbrst&(1u<<10))!=0u;
    vu.state().tBitEnabled=(cpu->vu0_fbrst&(1u<<11))!=0u;
    const uint64_t cyclesBefore=vu.state().cycles;
    const auto execStart=std::chrono::steady_clock::now();
    vu.execute(vu1Runtime->memory().getVU1Code(),PS2_VU1_CODE_SIZE,vu1Runtime->memory().getVU1Data(),PS2_VU1_DATA_SIZE,
               vu1Runtime->gs(),&vu1Runtime->memory(),startPC,top,itop,65536);
    vu1Runtime->memory().recordVU1Execution(vu.state().cycles-cyclesBefore,
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-execStart).count()));
    cpu->vu0_vpu_stat=(cpu->vu0_vpu_stat&~0x0600u)|(vu.state().stoppedByD?0x0200u:0u)|(vu.state().stoppedByT?0x0400u:0u);
    if(matched) {
        auto& counts=alphaChecks[owner];
        for(const auto& e:vu1Runtime->gs().getDebugHistory()) {
            if(e.seq<=lastSeq || e.kind!=GSDebugEventKind::Draw)continue;
            ++counts[!matched->alphaKnown?2:(e.alpha==matched->alpha?0:1)];
        }
        if((probeLaunches&255)==0)for(const auto& [ra,n]:alphaChecks)
            std::cout<<"[vu1-alpha] im3d_ra=0x"<<std::hex<<ra<<std::dec<<" match="<<n[0]<<" differ="<<n[1]<<" unknown="<<n[2]<<'\n';
    }
}
void skipShadow(uint8_t* ram,R5900Context* cpu,PS2Runtime*) {
    // iShadowRender ends by clearing its queued shadow vertex count.
    const uint32_t count=GPR_U32(cpu,28)-0x71f8;
    if(count<=PS2_RAM_SIZE-4)std::memset(ram+count,0,4);
    cpu->pc=GPR_U32(cpu,31);
}
void skipReturn(uint8_t*,R5900Context* cpu,PS2Runtime*){SET_GPR_U32(cpu,2,1);cpu->pc=GPR_U32(cpu,31);}
uint32_t guestWord(const void* ram,uint32_t address) {
    uint32_t value=0;
    if(address>=0x1000 && address<=PS2_RAM_SIZE-4)std::memcpy(&value,static_cast<const uint8_t*>(ram)+address,4);
    return value;
}
}

const NativeVU1OwnerSite* nativeVU1OwnerSites(unsigned& count) {
    count=sizeof(ownerSites)/sizeof(ownerSites[0]);return ownerSites;
}
bool nativeVU1SiteMatches(const NativeVU1OwnerSite& site,uint32_t (*word)(const void*,uint32_t),const void* context) {
    return word(context,site.transformReturn-8)==jal(0x209318) && word(context,site.renderReturn-8)==jal(0x208910)
        && word(context,site.endReturn-8)==jal(0x2093c0) && site.endReturn==site.renderReturn+8
        && word(context,site.primitiveInstruction)==(0x24040000u|site.primitive);
}

void installNativeVU1(PS2Runtime& runtime) {
    vu1Runtime=&runtime;
    const uint8_t* ram=runtime.memory().getRDRAM();
    // NIGHT100_REFERENCE_VU1_OWNERS=1 (or containing "sites") keeps the owner
    // units on the original Im3D/VU1 pipeline for comparisons.
    const char* reference=std::getenv("NIGHT100_REFERENCE_VU1_OWNERS");
    const bool admit=!reference || (std::strcmp(reference,"1") && !std::strstr(reference,"sites"));
    for(const auto& site:ownerSites) {
        if(!admit)break;
        if(!nativeVU1SiteMatches(site,guestWord,ram))
            throw std::runtime_error(std::string("Native VU1 owner contract changed: ")+site.name);
        admitNativeIm3DSite({site.transformReturn,site.renderReturn,site.endReturn,site.primitive});
    }
    if(const char* skip=std::getenv("NIGHT100_VU1_SKIP")) {
        const std::string list=skip;
        if(list.find("iShadowRender")!=std::string::npos)runtime.replaceFunction(0x1a1660,skipShadow);
        if(list.find("DrawSkidMarkStrip")!=std::string::npos)runtime.replaceFunction(0x1daea0,skipReturn);
        if(list.find("Particles")!=std::string::npos)runtime.replaceFunction(0x1b0740,skipReturn);
        std::cout<<"[vu1-skip] "<<list<<'\n';
    }
    if(std::getenv("NIGHT100_VU1_CALLER")) {
        runtime.memory().setVu1MscalCallback([](uint32_t startPC,uint32_t top,uint32_t itop){probeMscal(startPC,top,itop);});
        runtime.gs().setDebugHistoryPaused(false);
        probedTransform=runtime.lookupFunction(0x209318);
        if(probedTransform)runtime.replaceFunction(0x209318,probeTransform);
    }
}
