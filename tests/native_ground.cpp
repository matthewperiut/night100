// Differential check for the native Ground particle callback (0x1af4c0)
// against the original translation on synthetic particle groups. Both run
// through the same dispatch loop, which charges the scheduler's per-dispatch
// cycles. Compared: every flush batch (vertex bits, colors, UVs, indices,
// batch boundaries), the guest-visible staging state and camera-axis copy,
// callee-saved registers, the SDK calls made, and the total EE cycle charge.
#include "native_scene.h"
#include "graphics/native_model.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

void iParMgrRenderParSys_Ground_0x1af4c0(uint8_t*, R5900Context*, PS2Runtime*);
void memcpy_0x2572c8(uint8_t*, R5900Context*, PS2Runtime*);

namespace {
constexpr uint32_t gp=0x35f370,stack=0x1fe8000,returnPc=0x280000;
constexpr uint32_t global=0x362fd0,cameraPointer=0x2b5ad0,staging=0x3a5980,pool=0x379200;
constexpr uint32_t camera=0x500000,frame=0x500200,group=0x500400,command=0x500500,data=0x500600,assetName=0x500700;
constexpr uint32_t asset=0x500800,vertexBuffer=0x510000,indexBuffer=0x520000,packets=0x530000;
constexpr uint32_t coefficients[16]={0x3d2aaaab,0xbab60b61,0x37d00d01,0xb493f27c,0x310f74f6,0xad47d74e,0xbe2aaaab,0x3c088889,
    0xb9500d01,0x3638ef1b,0xb2d72f34,0x2f2ec9d3,0x3f800000,0xbf000000,0,0};
std::mt19937 bits(0x1af4c0);
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
uint8_t* ram=nullptr;
template<class T>void put(uint32_t at,T value){std::memcpy(ram+at,&value,sizeof(value));}
template<class T>T get(uint32_t at){T value;std::memcpy(&value,ram+at,sizeof(value));return value;}

struct Batch {std::vector<std::array<uint32_t,6>> vertices;std::vector<uint16_t> indices;};
std::vector<Batch> batches;
std::vector<uint32_t> rasterSets;
bool assetFound=true;
std::vector<uint32_t> flushReturns;
std::array<uint32_t,32> states{};
// SDK and engine routines the original calls; same symbols the translation binds.
}
void xSTFindAsset_0x120ab0(uint8_t*,R5900Context* cpu,PS2Runtime*) {
    require(GPR_U32(cpu,4)==assetName,"Asset lookup received a different name");
    SET_GPR_U32(cpu,2,assetFound?asset:0);cpu->pc=GPR_U32(cpu,31);
}
void sub_001DDC48_0x1ddc48(uint8_t*,R5900Context* cpu,PS2Runtime*) {
    require(GPR_U32(cpu,4)==1,"Unexpected render state");rasterSets.push_back(GPR_U32(cpu,5));
    SET_GPR_U32(cpu,2,1);cpu->pc=GPR_U32(cpu,31);
}
void iRenderTrianglesImmediate_0x1b0740(uint8_t*,R5900Context* cpu,PS2Runtime*) {
    // The original's flush: a2 vertices (36 bytes), a3 count, t0 indices, t1 index count.
    require(GPR_U32(cpu,5)==36,"Flush stride changed");
    Batch batch;const uint32_t vertices=GPR_U32(cpu,6),count=GPR_U32(cpu,7),indices=GPR_U32(cpu,8),indexCount=GPR_U32(cpu,9);
    for(uint32_t i=0;i<count;++i){std::array<uint32_t,6> v;
        for(unsigned c=0;c<3;++c)v[c]=get<uint32_t>(vertices+i*36+c*4);
        v[3]=get<uint32_t>(vertices+i*36+12);v[4]=get<uint32_t>(vertices+i*36+28);v[5]=get<uint32_t>(vertices+i*36+32);batch.vertices.push_back(v);}
    for(uint32_t i=0;i<indexCount;++i)batch.indices.push_back(get<uint16_t>(indices+i*2));
    batches.push_back(std::move(batch));
    // As the real (native) renderParticles does: queue, then insert the ordered
    // marker through the packet allocator and resume the caller afterwards.
    flushReturns.push_back(GPR_U32(cpu,31));
    SET_GPR_U32(cpu,31,0x1afd7c);cpu->pc=0x1f2498;
}
namespace {
uint64_t submit(NativeModelDraw draw) {
    Batch batch;const auto& mesh=*draw.geometry;
    require(draw.indexed && mesh.meshes.size()==1 && mesh.meshes[0].indexCount==mesh.indices.size() && (mesh.flags&1)==0,"Ground batch layout changed");
    for(const auto& vertex:mesh.vertices){std::array<uint32_t,6> v;
        for(unsigned c=0;c<3;++c)v[c]=std::bit_cast<uint32_t>(vertex.position[c]);
        v[3]=vertex.color;v[4]=std::bit_cast<uint32_t>(vertex.uv[0]);v[5]=std::bit_cast<uint32_t>(vertex.uv[1]);batch.vertices.push_back(v);}
    batch.indices=mesh.indices;batches.push_back(std::move(batch));
    return 0x4e31303000000099ull;
}
void allocator(uint8_t*,R5900Context* cpu,PS2Runtime*){SET_GPR_U32(cpu,2,1);cpu->pc=GPR_U32(cpu,31);}
void flushMarkerReturn(uint8_t*,R5900Context* cpu,PS2Runtime*) {
    require(!flushReturns.empty(),"Unmatched original flush marker");
    const uint32_t target=flushReturns.back();flushReturns.pop_back();
    SET_GPR_U32(cpu,2,1);SET_GPR_U32(cpu,31,target);cpu->pc=target;
}
void getter(uint8_t*,R5900Context* cpu,PS2Runtime*) {
    const uint32_t state=GPR_U32(cpu,4);require(state<states.size(),"Unexpected SDK state query");
    put(GPR_U32(cpu,5),states[state]);SET_GPR_U32(cpu,2,1);cpu->pc=GPR_U32(cpu,31);
}
float random(float low,float high){return low+(high-low)*float(bits()%100000)/100000.0f;}
struct Case {unsigned count;bool texture,found,culled,unaligned;uint32_t initialVertices;uint32_t seed=0;};
void fixture(const Case& c) {
    std::memset(ram+pool,0,0x27500);
    put(global,1u);put(cameraPointer,camera);put(camera+4,frame);put(camera+0x14,1u);
    const NativeMatrix identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};std::memcpy(ram+camera+0x20,identity.data(),64);
    put(camera+0x84,90.f);put(camera+0x88,10.f);put(camera+0x8c,-6.f);put(camera+0x90,13.f);
    for(unsigned i=0;i<16;++i)put(frame+0x10+i*4,random(-1.f,1.f));
    // Four cull planes: |x|,|y| <= 50 + size pass. Lanes: d + size - n.p.
    const float planes[4][4]={{1,-1,0,0},{0,0,1,-1},{0,0,0,0},{50,50,50,50}};
    for(unsigned i=0;i<4;++i)for(unsigned l=0;l<4;++l)put(global+0x170+i*16+l*4,planes[i][l]);
    for(unsigned i=0;i<16;++i)put(0x28a180+i*4,coefficients[i]);
    const uint16_t corners[6]={0,1,2,3,0,1};for(unsigned i=0;i<6;++i)put(0x2b3e70+i*2,corners[i]);
    put(staging,indexBuffer);put(staging+4,vertexBuffer);put(staging+0x10,0x12345u);put(staging+0x14,36u);
    put(staging+0x18,c.initialVertices?6u:0u);put(staging+0x1c,c.initialVertices);
    put(data+0x10,data+0x40);put(data+0x50,assetName);put(assetName,0xabcdefu);put(asset,0x777000u);
    put(group+0x20,c.texture?command:0u);
    put(command+8,0.25f);put(command+0xc,0.5f);put(command+0x1c,0.125f);put(command+0x20,0.0625f);
    uint32_t previous=group;
    for(unsigned i=0;i<c.count;++i) {
        const uint32_t node=pool+i*0x50+(c.unaligned && i==c.count/2?8:0);
        put(previous,node);previous=node;
        put(node+0xc,uint32_t(bits()));
        const float range=c.culled?400.f:70.f;
        put(node+0x10,random(-range,range));put(node+0x14,random(-range,range));put(node+0x18,random(-20,20));
        put(node+0x1c,i%9==0?0.f:random(0.f,6.f));
        put<uint8_t>(node+0x32,uint8_t(bits()));put<uint8_t>(node+0x33,uint8_t(bits()));
        for(unsigned a=0;a<3;++a){const unsigned edge=bits()%5;put<uint8_t>(node+0x34+a,uint8_t(edge==0?0:edge==1?255:edge==2?128:bits()));}
    }
    put(previous,0u);
    put(gp-0x7b80,int64_t(0x44));put(gp-0x7c1c,packets);
}
struct Outcome {std::vector<Batch> batches;std::vector<uint32_t> rasters;std::array<uint8_t,0x60> axes;std::array<uint32_t,2> counts;R5900Context cpu;uint32_t cycles;};
Outcome run(PS2Runtime& runtime,const Case& c,bool native) {
    bits.seed(c.seed);fixture(c);batches.clear();rasterSets.clear();assetFound=c.found;
    R5900Context cpu{};for(unsigned i=1;i<32;++i)cpu.r[i]=_mm_set_epi32(bits(),bits(),bits(),bits());
    SET_GPR_U32(&cpu,4,data);SET_GPR_U32(&cpu,5,group);SET_GPR_U32(&cpu,28,gp);SET_GPR_U32(&cpu,29,stack);SET_GPR_U32(&cpu,31,returnPc);
    cpu.pc=0x1af4c0;
    const R5900Context entry=cpu;
    runtime.eeScheduler().reset(ram,cpu);
    // T0 counts bus cycles (EE/2) from the scheduler's charges; no interrupt.
    runtime.memory().writeIORegister(0x10000000,0);runtime.memory().writeIORegister(0x10000020,0xffff);
    runtime.memory().writeIORegister(0x10000010,0x80);
    const uint32_t before=runtime.memory().readIORegister(0x10000000);
    NativeSceneTesting::setupGround(iParMgrRenderParSys_Ground_0x1af4c0);
    for(unsigned step=0;cpu.pc!=returnPc;++step) {
        require(step<100000,"Ground fixture did not finish");
        (void)runtime.eeCheckpointDue(EeScheduler::kGuestDispatchCycles); // the scheduler's dispatch charge
        if(cpu.pc==0x1af4c0 && native)NativeSceneTesting::ground(ram,&cpu,&runtime);
        else runtime.lookupFunction(cpu.pc)(ram,&cpu,&runtime);
    }
    require(!NativeSceneTesting::groundPending(),"Native Ground left a pending continuation");
    Outcome out;out.batches=batches;out.rasters=rasterSets;std::memcpy(out.axes.data(),ram+staging+0x20,0x60);
    out.counts={get<uint32_t>(staging+0x18),get<uint32_t>(staging+0x1c)};out.cpu=cpu;
    out.cycles=runtime.memory().readIORegister(0x10000000)-before;
    {const R5900Context* final=&cpu;require(GPR_U32(final,29)==stack,"Stack pointer not restored");}
    for(unsigned r=16;r<24;++r)require(!std::memcmp(&cpu.r[r],&entry.r[r],16),"Callee-saved register changed");
    require(!std::memcmp(&cpu.r[30],&entry.r[30],16),"Frame pointer changed");
    return out;
}
}
int main(){try {
    PS2Runtime runtime;require(runtime.memory().initialize() && runtime.syncCoreSubsystems(),"Runtime initialization failed");
    ram=runtime.memory().getRDRAM();
    for(uint32_t pc:{0x1af4c0u,0x1af690u,0x1af6acu,0x1af6dcu,0x1af728u,0x1afcf8u,0x1afd44u})
        require(runtime.registerFunction(pc,iParMgrRenderParSys_Ground_0x1af4c0),"Ground registration failed");
    require(runtime.registerFunction(0x2572c8,memcpy_0x2572c8) && runtime.registerFunction(0x120ab0,xSTFindAsset_0x120ab0)
        && runtime.registerFunction(0x1ddc48,sub_001DDC48_0x1ddc48) && runtime.registerFunction(0x1b0740,iRenderTrianglesImmediate_0x1b0740)
        && runtime.registerFunction(0x1f2498,allocator) && runtime.registerFunction(0x1f5b00,getter)
        && runtime.registerFunction(0x121ecc,NativeSceneTesting::finishMarker)
        && runtime.registerFunction(0x1afd78,NativeSceneTesting::groundContinuation)
        && runtime.registerFunction(0x1afd7c,flushMarkerReturn),"Stub registration failed");
    NativeSceneTesting::setup(nullptr,nullptr,nullptr,submit);
    states.fill(0);states[3]=states[4]=3;states[6]=1;states[9]=1;states[10]=5;states[11]=6;states[12]=1;states[20]=1;
    std::vector<Case> cases;
    for(unsigned count:{0u,1u,2u,119u,120u,121u,239u,240u,241u,400u})
        for(unsigned variant=0;variant<4;++variant)cases.push_back({count,variant!=1,variant!=2,false,false,0});
    cases.push_back({300,true,true,true,false,0});   // nearly all culled
    cases.push_back({200,true,true,false,true,0});   // an unaligned node: native falls back mid-unit
    cases.push_back({50,true,true,false,false,4});   // nonzero initial staging: native declines at entry
    unsigned checked=0,quads=0;
    for(size_t i=0;i<cases.size();++i)cases[i].seed=uint32_t(0x9e3779b9u*(i+1));
    for(const auto& c:cases) {
        const auto expected=run(runtime,c,false),actual=run(runtime,c,true);
        require(expected.batches.size()==actual.batches.size(),"Flush batch count differs");
        for(size_t b=0;b<expected.batches.size();++b) {
            require(expected.batches[b].vertices==actual.batches[b].vertices,"Ground vertex bits differ");
            require(expected.batches[b].indices==actual.batches[b].indices,"Ground indices differ");
            quads+=unsigned(actual.batches[b].vertices.size()/4);
        }
        require(expected.rasters==actual.rasters,"Texture state calls differ");
        if(!(expected.axes==actual.axes && expected.counts==actual.counts)) {
            std::cerr<<"case count="<<c.count<<" texture="<<c.texture<<" found="<<c.found<<" counts "<<expected.counts[0]<<'/'<<expected.counts[1]
                <<" vs "<<actual.counts[0]<<'/'<<actual.counts[1]<<'\n';
            for(unsigned i=0;i<0x60;i+=4){uint32_t a,b;std::memcpy(&a,&expected.axes[i],4);std::memcpy(&b,&actual.axes[i],4);if(a!=b)std::cerr<<"  axes+0x"<<std::hex<<i+0x20<<" "<<a<<" vs "<<b<<std::dec<<'\n';}
        }
        require(expected.axes==actual.axes && expected.counts==actual.counts,"Staging camera axes or counters differ");
        require(expected.cycles==actual.cycles,"EE cycle charge differs");
        ++checked;
    }
    // Per-call cost of a 400-particle group (fixture rebuilt outside the timer).
    for(bool native:{false,true}) {
        const Case c{400,true,true,false,false,0,0x55aa};double total=0;
        for(unsigned i=0;i<200;++i) {
            bits.seed(c.seed);fixture(c);batches.clear();rasterSets.clear();
            R5900Context cpu{};SET_GPR_U32(&cpu,4,data);SET_GPR_U32(&cpu,5,group);SET_GPR_U32(&cpu,28,gp);SET_GPR_U32(&cpu,29,stack);
            SET_GPR_U32(&cpu,31,returnPc);cpu.pc=0x1af4c0;runtime.eeScheduler().reset(ram,cpu);
            const auto start=std::chrono::steady_clock::now();
            while(cpu.pc!=returnPc) {
                if(cpu.pc==0x1af4c0 && native)NativeSceneTesting::ground(ram,&cpu,&runtime);
                else runtime.lookupFunction(cpu.pc)(ram,&cpu,&runtime);
            }
            total+=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();
        }
        std::cout<<(native?"native":"original")<<" us per 400-particle Ground call (incl. batch capture)="<<total/200<<'\n';
    }
    std::cout<<"PASS: "<<checked<<" Ground cases, "<<quads<<" quads bit-identical to the original translation; batches, texture state, staging axes/counters, callee-saved registers and EE cycle charges match\n";
    return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
