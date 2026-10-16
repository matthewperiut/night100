// Contracts that retire the last interpreted VU1 program (RenderWare's PS2
// Im3D pipeline) on the native path: the two retail owner callsites, strip and
// list topology, and the PS2 blend the original GS applies when the SDK rejects
// a blend pair. CPU-only; no window, GPU or game executor.
#include "native_scene.h"
#include "native_vu1_lastprogram.h"
#include "graphics/native_model.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
constexpr uint32_t gp=0x350000,stack=0x1fe0000,vertices=0x130000,camera=0x140000;
constexpr uint32_t plugin=0x2b5ad0+0x200+0x60,packet=0x170000;
// Retail ELF table at 0x2ba170: ALPHA_1 for [destination][source] SDK index,
// -1 where the PS2 cannot express the pair.
constexpr int32_t retailBlendTable[6][6]={
    {0x8a,0x0a,0x88,0x02,0x98,0x12},{0x4a,0x68,0x48,-1,0x58,-1},{0x89,0x09,-1,0x01,-1,-1},
    {0x46,-1,0x44,-1,-1,-1},{0x99,0x19,-1,-1,-1,0x11},{0x56,-1,-1,-1,0x54,-1}};
std::array<uint32_t,32> states{};
bool getterFails=false;
unsigned originalTransforms=0,originalRenders=0,originalEnds=0,originalSetters=0,originalOverrides=0;
std::vector<NativeModelDraw> submitted;
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
template<class T>void put(uint8_t* ram,uint32_t at,T value){std::memcpy(ram+at,&value,sizeof(value));}
template<class T>T get(uint8_t* ram,uint32_t at){T value;std::memcpy(&value,ram+at,sizeof(value));return value;}
void getter(uint8_t* ram,R5900Context* cpu,PS2Runtime*) {
    const uint32_t state=GPR_U32(cpu,4);require(state<states.size(),"Unexpected SDK state query");
    // Like the retail getter: an inconsistent blend cache fails without output.
    if(getterFails && (state==10 || state==11)){SET_GPR_U32(cpu,2,0);cpu->pc=GPR_U32(cpu,31);return;}
    put(ram,GPR_U32(cpu,5),states[state]);SET_GPR_U32(cpu,2,1);cpu->pc=GPR_U32(cpu,31);
}
void originalTransform(uint8_t*,R5900Context* cpu,PS2Runtime*){++originalTransforms;SET_GPR_U32(cpu,2,0);cpu->pc=GPR_U32(cpu,31);}
void originalRender(uint8_t*,R5900Context* cpu,PS2Runtime*){++originalRenders;SET_GPR_U32(cpu,2,0);cpu->pc=GPR_U32(cpu,31);}
void originalEnd(uint8_t*,R5900Context* cpu,PS2Runtime*){++originalEnds;SET_GPR_U32(cpu,2,0);cpu->pc=GPR_U32(cpu,31);}
void originalBlend(uint8_t* ram,R5900Context* cpu,PS2Runtime*) {
    // Minimal model of the retail setter for states 10/11: store the index,
    // then cache the table value (or -1). Overrides only count their calls.
    if(cpu->pc!=0x1f5208){++originalOverrides;cpu->pc=GPR_U32(cpu,31);return;}
    ++originalSetters;
    const uint32_t state=GPR_U32(cpu,4),value=GPR_U32(cpu,5);
    constexpr int map[9]={-1,0,1,-1,-1,2,3,4,5};
    const int index=value<9?map[value]:-1;
    if(index>=0) {
        put<uint32_t>(ram,gp-(state==10?0x7b68:0x7b64),index);
        const int s=get<uint32_t>(ram,gp-0x7b68),d=get<uint32_t>(ram,gp-0x7b64);
        put<int64_t>(ram,gp-0x7b80,retailBlendTable[d][s]);
    }
    cpu->pc=GPR_U32(cpu,31);
}
uint64_t submit(NativeModelDraw draw){submitted.push_back(std::move(draw));return 0x4e31303000000077ull;}
void initialize(uint8_t* ram,unsigned count) {
    NativeSceneTesting::setup(originalTransform,originalRender,originalEnd,submit);
    NativeSceneTesting::setupBlendTracking(originalBlend,originalBlend,originalBlend);
    getterFails=false;submitted.clear();
    originalTransforms=originalRenders=originalEnds=originalSetters=originalOverrides=0;
    states.fill(0);states[3]=states[4]=3;states[6]=1;states[8]=0;states[9]=1;
    states[10]=5;states[11]=6;states[12]=1;states[14]=0;states[20]=1;
    put(ram,gp-0x7aa0,0x200u);put(ram,0x2b5ad0,camera);
    for(unsigned d=0;d<6;++d)for(unsigned s=0;s<6;++s)put(ram,0x2ba170+d*24+s*4,retailBlendTable[d][s]);
    put<uint32_t>(ram,gp-0x7b68,2);put<uint32_t>(ram,gp-0x7b64,3);put<int64_t>(ram,gp-0x7b80,0x44);
    const NativeMatrix identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    std::memcpy(ram+camera+0x20,identity.data(),64);put(ram,camera+0x14,1u);
    put(ram,camera+0x84,90.f);put(ram,camera+0x88,10.f);put(ram,camera+0x8c,-6.f);put(ram,camera+0x90,13.f);
    for(unsigned i=0;i<count;++i) {
        const uint32_t address=vertices+i*36;
        for(unsigned c=0;c<3;++c)put(ram,address+c*4,float(i*10+c)+0.5f);
        put(ram,address+12,0x50000000u|i);
        put(ram,address+28,float(i&1));put(ram,address+32,float(i>>1));
    }
}
R5900Context entry(uint32_t caller,unsigned count) {
    R5900Context cpu{};cpu.pc=0x209318;
    SET_GPR_U32((&cpu),4,vertices);SET_GPR_U32((&cpu),5,count);SET_GPR_U32((&cpu),6,0);SET_GPR_U32((&cpu),7,0x19);
    SET_GPR_U32((&cpu),28,gp);SET_GPR_U32((&cpu),29,stack);SET_GPR_U32((&cpu),31,caller);return cpu;
}
const NativeVU1OwnerSite& site(const char* name) {
    unsigned count=0;const auto* sites=nativeVU1OwnerSites(count);
    for(unsigned i=0;i<count;++i)if(!std::strcmp(sites[i].name,name))return sites[i];
    throw std::runtime_error("Missing owner site");
}
// Runs one admitted unit and returns its queued draw.
NativeModelDraw runUnit(uint8_t* ram,PS2Runtime& runtime,const NativeVU1OwnerSite& owner,unsigned count) {
    auto cpu=entry(owner.transformReturn,count);
    NativeSceneTesting::transform(ram,&cpu,&runtime);
    require(cpu.pc==owner.transformReturn && GPR_U32((&cpu),2)==vertices,"Owner transform changed return contract");
    require(NativeSceneTesting::pendingDraw(),"Owner transform was not admitted");
    const size_t before=submitted.size();
    cpu.pc=0x208910;SET_GPR_U32((&cpu),4,owner.primitive);SET_GPR_U32((&cpu),31,owner.renderReturn);
    NativeSceneTesting::render(ram,&cpu,&runtime);
    require(submitted.size()==before+1 && cpu.pc==0x1f2498,"Owner render bypassed the ordered packet allocator");
    put(ram,gp-0x7c1c,packet);SET_GPR_U32((&cpu),2,1);NativeSceneTesting::finishMarker(ram,&cpu,&runtime);
    require(cpu.pc==owner.renderReturn && get<uint64_t>(ram,packet+16)==0x4e31303000000077ull,"Owner marker changed render return");
    cpu.pc=0x2093c0;SET_GPR_U32((&cpu),31,owner.endReturn);NativeSceneTesting::end(ram,&cpu,&runtime);
    require(cpu.pc==owner.endReturn && GPR_U32((&cpu),2)==1 && !NativeSceneTesting::pendingDraw(),"Owner End changed contract");
    for(unsigned i=0;i<60;++i)require(!ram[plugin+i],"Owner End did not clear the SDK workspace");
    require(originalTransforms==0 && originalRenders==0 && originalEnds==0,"Admitted unit ran the VU1 pipeline");
    return submitted.back();
}
void expectFallback(uint8_t* ram,PS2Runtime& runtime,uint32_t caller,unsigned count,const char* reason) {
    const auto transforms=originalTransforms;auto cpu=entry(caller,count);
    NativeSceneTesting::transform(ram,&cpu,&runtime);
    require(originalTransforms==transforms+1 && !NativeSceneTesting::pendingDraw(),reason);
}
uint32_t wordAt(const void* words,uint32_t address) {
    for(auto* w=static_cast<const std::pair<uint32_t,uint32_t>*>(words);w->first;++w)if(w->first==address)return w->second;
    return 0;
}
}
int main(){try {
    // Retail instruction words (from SLUS_203.49) at every owner callsite:
    // the three SDK JALs and the instruction that loads the primitive.
    constexpr uint32_t transformJal=0x0c0824c6,renderJal=0x0c082244,endJal=0x0c0824f0;
    const std::pair<uint32_t,uint32_t> retail[]={
        {0x1a16e8,transformJal},{0x1a16f0,renderJal},{0x1a16f4,0x24040003},{0x1a16f8,endJal},
        {0x1db1ac,transformJal},{0x1db1b8,0x24040004},{0x1db1bc,renderJal},{0x1db1c4,endJal},
        {0x1488a8,transformJal},{0x1488b4,0x24040004},{0x1488b8,renderJal},{0x1488c0,endJal},
        {0x1c3f80,transformJal},{0x1c3f90,renderJal},{0x1c3f94,0x24040003},{0x1c3f98,endJal},
        {0x1c442c,transformJal},{0x1c443c,renderJal},{0x1c4440,0x24040003},{0x1c4444,endJal},
        {0x24b090,transformJal},{0x24b09c,0x24040004},{0x24b0a0,renderJal},{0x24b0a8,endJal},
        {0x24b2a0,transformJal},{0x24b2ac,0x24040004},{0x24b2b0,renderJal},{0x24b2b8,endJal},
        {0x279fd0,transformJal},{0x279fdc,0x24040004},{0x279fe0,renderJal},{0x279fe8,endJal},{0,0}};
    unsigned siteCount=0;const auto* sites=nativeVU1OwnerSites(siteCount);
    require(siteCount==8,"Unexpected owner site count");
    for(unsigned i=0;i<siteCount;++i)require(nativeVU1SiteMatches(sites[i],wordAt,retail),"Retail owner site rejected");
    auto mutated=std::to_array(retail);mutated[5].second=0x24040003;
    require(!nativeVU1SiteMatches(site("DrawSkidMarkStrip"),wordAt,mutated.data()),"Changed strip primitive accepted");
    mutated=std::to_array(retail);mutated[13].second=0x0c082245;
    require(!nativeVU1SiteMatches(site("xShadowReceiveShadow#1"),wordAt,mutated.data()),"Changed render call accepted");

    // GS ALPHA -> RenderWare factors. Every supported retail table entry must
    // round-trip to its own pair; other forms the native blend cannot express fail.
    {
        constexpr uint32_t factors[6]={1,2,5,6,7,8};unsigned checked=0;
        for(unsigned d=0;d<6;++d)for(unsigned s=0;s<6;++s) {
            if(retailBlendTable[d][s]<0)continue;
            const uint64_t alpha=uint64_t(retailBlendTable[d][s])|(s==1 && d==1?uint64_t(0x80)<<32:0);
            uint32_t source=0,destination=0;
            require(nativeBlendFactors(alpha,source,destination) && source==factors[s] && destination==factors[d],"Retail ALPHA_1 table entry did not round-trip");
            ++checked;
        }
        require(checked==20,"Unexpected retail blend table coverage");
        uint32_t source=0,destination=0;
        require(nativeBlendFactors(0x8000000002ull,source,destination) && source==6 && destination==1,"Raster ALPHA_1 0x02 is INVSRCALPHA/ZERO");
        require(nativeBlendFactors(0,source,destination) && source==2 && destination==1,"ALPHA_1 0 is ONE/ZERO");
        require(!nativeBlendFactors(0x4000000068ull,source,destination),"FIX other than 0x80 accepted");
        require(!nativeBlendFactors(0x41,source,destination),"Reverse-subtract blend accepted");
        require(!nativeBlendFactors(~0ull,source,destination),"Reserved ALPHA_1 selectors accepted");
    }

    PS2Runtime runtime;require(runtime.memory().initialize() && runtime.syncCoreSubsystems(),"Runtime initialization failed");
    require(runtime.registerFunction(0x1f5b00,getter),"SDK getter registration failed");
    auto* ram=runtime.memory().getRDRAM();
    const auto& shadow=site("iShadowRender");const auto& skid=site("DrawSkidMarkStrip");

    // Owners are not admitted until installNativeVU1 registers them.
    initialize(ram,6);expectFallback(ram,runtime,shadow.transformReturn,6,"Unregistered owner was admitted");
    initialize(ram,6);
    for(unsigned i=0;i<siteCount;++i)admitNativeIm3DSite({sites[i].transformReturn,sites[i].renderReturn,sites[i].endReturn,sites[i].primitive});
    {
        const auto draw=runUnit(ram,runtime,shadow,6);
        require(!(draw.geometry->flags&1) && draw.geometry->meshes[0].indexCount==6 && !draw.indexed,"Blob shadow is not a triangle list");
        require(draw.sourceBlend==5 && draw.destinationBlend==6 && draw.depthTest && !draw.depthWrite,"Blob shadow states changed");
        require(draw.geometry->vertices[5].color==0x50000005u && draw.geometry->vertices[5].position[0]==50.5f,"Blob shadow vertices changed");
    }
    // Strips: any count of at least three; RenderPrimitive must pass TRISTRIP.
    for(const unsigned count:{3u,4u,7u,256u}) {
        initialize(ram,count);
        for(unsigned i=0;i<siteCount;++i)admitNativeIm3DSite({sites[i].transformReturn,sites[i].renderReturn,sites[i].endReturn,sites[i].primitive});
        const auto draw=runUnit(ram,runtime,skid,count);
        require((draw.geometry->flags&1) && (draw.geometry->flags&8) && draw.geometry->meshes[0].indexCount==count,"Skid mark is not a prelit strip");
    }
    initialize(ram,6);
    for(unsigned i=0;i<siteCount;++i)admitNativeIm3DSite({sites[i].transformReturn,sites[i].renderReturn,sites[i].endReturn,sites[i].primitive});
    expectFallback(ram,runtime,skid.transformReturn,2,"Degenerate strip was admitted");
    expectFallback(ram,runtime,shadow.transformReturn,4,"Partial triangle list was admitted");
    {
        auto cpu=entry(skid.transformReturn,4);NativeSceneTesting::transform(ram,&cpu,&runtime);
        cpu.pc=0x208910;SET_GPR_U32((&cpu),4,3);SET_GPR_U32((&cpu),31,skid.renderReturn);bool threw=false;
        try{NativeSceneTesting::render(ram,&cpu,&runtime);}catch(const std::runtime_error&){threw=true;}
        require(threw,"Strip unit accepted a triangle-list render");
        initialize(ram,6);
        for(unsigned i=0;i<siteCount;++i)admitNativeIm3DSite({sites[i].transformReturn,sites[i].renderReturn,sites[i].endReturn,sites[i].primitive});
    }
    bool conflict=false;
    try{admitNativeIm3DSite({skid.transformReturn,skid.renderReturn,skid.endReturn,3});}catch(const std::runtime_error&){conflict=true;}
    require(conflict,"Conflicting site contract accepted");

    // PS2 blend: SRCALPHA then INVDESTALPHA. The pair is unsupported, so the GS
    // keeps SRCALPHA/ONE written by the previous valid pair (ALPHA_1 0x48).
    initialize(ram,6);
    for(unsigned i=0;i<siteCount;++i)admitNativeIm3DSite({sites[i].transformReturn,sites[i].renderReturn,sites[i].endReturn,sites[i].primitive});
    auto setState=[&](uint32_t state,uint32_t value) {
        R5900Context cpu{};cpu.pc=0x1f5208;SET_GPR_U32((&cpu),4,state);SET_GPR_U32((&cpu),5,value);
        SET_GPR_U32((&cpu),28,gp);SET_GPR_U32((&cpu),31,0x100000);NativeSceneTesting::blendSetter(ram,&cpu,&runtime);
    };
    setState(11,2);require(get<int64_t>(ram,gp-0x7b80)==0x48,"Test setter model differs from retail table");
    setState(10,5);setState(11,8);
    require(get<int64_t>(ram,gp-0x7b80)==-1 && originalSetters==3,"Unsupported pair did not reach the SDK");
    getterFails=true;
    {
        const auto draw=runUnit(ram,runtime,shadow,6);
        require(draw.sourceBlend==5 && draw.destinationBlend==2,"Unsupported pair did not keep the GS's previous blend");
    }
    // A raster render writes ALPHA_1 0x80_00000002 directly; the GS then blends
    // INVSRCALPHA/ZERO although the SDK cache still names the old pair.
    {
        R5900Context cpu{};cpu.pc=0x21ef40;SET_GPR_U32((&cpu),28,gp);SET_GPR_U32((&cpu),31,0x100000);
        NativeSceneTesting::blendConstant(ram,&cpu,&runtime);require(originalOverrides==1,"Constant writer skipped original");
        originalTransforms=0;const auto draw=runUnit(ram,runtime,shadow,6);
        require(draw.sourceBlend==6 && draw.destinationBlend==1,"Constant ALPHA_1 writer not followed");
        // The raw-ALPHA state (2) of 0x1f5e70 packs FIX in bits 24-31; state 1 is DIMX.
        cpu.pc=0x1f5e70;SET_GPR_U32((&cpu),4,1);SET_GPR_U32((&cpu),5,0x80000068u);NativeSceneTesting::blendRaw(ram,&cpu,&runtime);
        originalTransforms=0;require(runUnit(ram,runtime,shadow,6).sourceBlend==6,"Non-ALPHA raw state changed the blend");
        cpu.pc=0x1f5e70;SET_GPR_U32((&cpu),4,2);NativeSceneTesting::blendRaw(ram,&cpu,&runtime);
        originalTransforms=0;const auto raw=runUnit(ram,runtime,shadow,6);
        require(raw.sourceBlend==2 && raw.destinationBlend==2,"Raw ONE/ONE ALPHA_1 not followed");
    }
    // The SDK re-emitting its cache (-1) leaves the GS register unusable; a
    // later supported setter call makes it known again.
    {
        R5900Context cpu{};cpu.pc=0x21e4b0;SET_GPR_U32((&cpu),28,gp);SET_GPR_U32((&cpu),31,0x100000);
        NativeSceneTesting::blendReemit(ram,&cpu,&runtime);
    }
    expectFallback(ram,runtime,shadow.transformReturn,6,"Re-emitted invalid ALPHA_1 was drawn natively");
    setState(11,6);getterFails=false;
    {
        originalTransforms=0;const auto draw=runUnit(ram,runtime,shadow,6);
        require(draw.sourceBlend==5 && draw.destinationBlend==6,"Supported pair not used");
    }
    // Re-emitting a valid cache and the SDK initializer are known pairs.
    put<int64_t>(ram,gp-0x7b80,0x68|(int64_t(0x80)<<32));getterFails=true;
    {
        R5900Context cpu{};cpu.pc=0x1feff0;SET_GPR_U32((&cpu),28,gp);SET_GPR_U32((&cpu),31,0x100000);
        NativeSceneTesting::blendReemit(ram,&cpu,&runtime);put<int64_t>(ram,gp-0x7b80,-1);
        originalTransforms=0;const auto draw=runUnit(ram,runtime,shadow,6);
        require(draw.sourceBlend==2 && draw.destinationBlend==2,"Re-emitted ONE/ONE (FIX) not recognized");
        cpu.pc=0x1fc0c8;NativeSceneTesting::blendInit(ram,&cpu,&runtime);
        originalTransforms=0;const auto initial=runUnit(ram,runtime,shadow,6);
        require(initial.sourceBlend==5 && initial.destinationBlend==6,"SDK initializer pair not recognized");
    }
    // Snapshot restore: with the SDK queue drained the restored GS ALPHA_1 is
    // current, so a rejected pair right after the restore still draws natively.
    put<int64_t>(ram,gp-0x7b80,-1);getterFails=true;
    put<uint32_t>(ram,gp-0x7c10,0x200000);put<uint32_t>(ram,gp-0x7c0c,0x200000);put<uint32_t>(ram,gp-0x6ea4,0);put<uint32_t>(ram,gp-0x7c70,0);
    NativeSceneTesting::seedBlendAfterRestore(ram,gp,0x48);
    {
        originalTransforms=0;const auto draw=runUnit(ram,runtime,shadow,6);
        require(draw.sourceBlend==5 && draw.destinationBlend==2,"Restored GS ALPHA_1 not used");
    }
    NativeSceneTesting::seedBlendAfterRestore(ram,gp,0);
    {
        originalTransforms=0;const auto draw=runUnit(ram,runtime,shadow,6);
        require(draw.sourceBlend==2 && draw.destinationBlend==1,"Restored ALPHA_1 0 (clear) not ONE/ZERO");
    }
    // Unsent SDK packets may still change ALPHA_1: unknown, keep VU1 for the
    // rejected pair; a valid cache alone still uses the getter.
    put<uint32_t>(ram,gp-0x7c10,0x200010);
    NativeSceneTesting::seedBlendAfterRestore(ram,gp,0x48);
    expectFallback(ram,runtime,shadow.transformReturn,6,"Restore with queued packets trusted the GS register");
    put<int64_t>(ram,gp-0x7b80,0x44);getterFails=false;
    {
        originalTransforms=0;const auto draw=runUnit(ram,runtime,shadow,6);
        require(draw.sourceBlend==5 && draw.destinationBlend==6,"Valid SDK cache not used after an undrained restore");
    }
    std::cout<<"PASS: 8 retail owner sites (list and strip), call-site guards, topology/count fallbacks, GS ALPHA conversion (20 table entries) and ALPHA_1 tracking across writers and restore\n";
    return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
