#include "native_scene.h"
#include "graphics/native_model.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

// Frame benchmarking is unrelated to the immediate-geometry adapter; this
// CPU-only fixture never presents a frame or starts the window runtime.
void benchmarkFrameCompleted(uint8_t*,PS2Runtime*){}

namespace {
constexpr uint32_t gp=0x350000,stack=0x1fe0000,vertices=0x130000,camera=0x140000;
constexpr uint32_t plugin=0x2b5ad0+0x200+0x60,packet=0x170000,raster=0x180000;
std::array<uint32_t,32> states{};
unsigned originalTransforms=0,originalRenders=0,originalEnds=0;
std::vector<NativeModelDraw> submitted;
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
template<class T>void put(uint8_t* ram,uint32_t at,T value){std::memcpy(ram+at,&value,sizeof(value));}
template<class T>T get(uint8_t* ram,uint32_t at){T value;std::memcpy(&value,ram+at,sizeof(value));return value;}
void getter(uint8_t* ram,R5900Context* cpu,PS2Runtime*) {
    const uint32_t state=GPR_U32(cpu,4);require(state<states.size(),"Unexpected SDK state query");
    put(ram,GPR_U32(cpu,5),states[state]);SET_GPR_U32(cpu,2,1);cpu->pc=GPR_U32(cpu,31);
}
void originalTransform(uint8_t*,R5900Context* cpu,PS2Runtime*){++originalTransforms;SET_GPR_U32(cpu,2,0);cpu->pc=GPR_U32(cpu,31);}
void originalRender(uint8_t*,R5900Context* cpu,PS2Runtime*){++originalRenders;SET_GPR_U32(cpu,2,0);cpu->pc=GPR_U32(cpu,31);}
void originalEnd(uint8_t*,R5900Context* cpu,PS2Runtime*){++originalEnds;SET_GPR_U32(cpu,2,0);cpu->pc=GPR_U32(cpu,31);}
uint64_t submit(NativeModelDraw draw){submitted.push_back(std::move(draw));return 0x4e31303000000042ull;}
R5900Context entry(uint32_t caller=0x1c33f4) {
    R5900Context cpu{};cpu.pc=0x209318;
    SET_GPR_U32((&cpu),4,vertices);SET_GPR_U32((&cpu),5,3);SET_GPR_U32((&cpu),6,0);SET_GPR_U32((&cpu),7,0x19);
    SET_GPR_U32((&cpu),28,gp);SET_GPR_U32((&cpu),29,stack);SET_GPR_U32((&cpu),31,caller);return cpu;
}
void initialize(uint8_t* ram) {
    NativeSceneTesting::setup(originalTransform,originalRender,originalEnd,submit);
    states.fill(0);states[3]=states[4]=3;states[6]=1;states[8]=0;states[9]=1;
    states[10]=5;states[11]=6;states[12]=1;states[14]=1;states[15]=0xa1742b19;states[16]=1;states[20]=2;
    put(ram,gp-0x7aa0,0x200u);put(ram,0x2b5ad0,camera);
    std::memset(ram+plugin-16,0xa5,92);put(ram,stack-16,0x63524110u);
    const NativeMatrix identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    std::memcpy(ram+camera+0x20,identity.data(),64);put(ram,camera+0x14,1u);
    put(ram,camera+0x84,90.f);put(ram,camera+0x88,10.f);put(ram,camera+0x8c,-6.f);put(ram,camera+0x90,13.f);
    for(unsigned i=0;i<3;++i) {
        const uint32_t address=vertices+i*36;
        for(unsigned c=0;c<3;++c)put(ram,address+c*4,float(i*10+c)+0.25f);
        put(ram,address+12,0x80402010u+i*0x11030507u);
        for(unsigned c=0;c<3;++c)put(ram,address+16+c*4,-777.f);
        put(ram,address+28,float(i)*0.125f);put(ram,address+32,1.f-float(i)*0.25f);
    }
}
void checkPluginGuards(uint8_t* ram) {
    for(unsigned i=0;i<16;++i)require(ram[plugin-16+i]==0xa5 && ram[plugin+60+i]==0xa5,"Im3D modified plugin guard bytes");
    require(get<uint32_t>(ram,stack-16)==0x63524110u,"SDK state getter scratch was not restored");
}
}
int main(){try {
    PS2Runtime runtime;require(runtime.memory().initialize() && runtime.syncCoreSubsystems(),"Runtime initialization failed");
    require(runtime.registerFunction(0x1f5b00,getter),"SDK getter registration failed");
    auto* ram=runtime.memory().getRDRAM();
    for(uint32_t caller:{0x1c33f4u,0x1c3770u}) {
        initialize(ram);auto cpu=entry(caller);const auto original=cpu;
        std::array<uint8_t,60> expected{};expected.fill(0xa5);
        const uint16_t count=3;const uint32_t stride=36,flags=0x19,matrix=0;
        std::memcpy(expected.data(),&count,2);std::memcpy(expected.data()+4,&vertices,4);
        std::memcpy(expected.data()+8,&stride,4);std::memcpy(expected.data()+12,&flags,4);std::memcpy(expected.data()+16,&matrix,4);
        NativeSceneTesting::transform(ram,&cpu,&runtime);
        require(cpu.pc==caller && GPR_U32((&cpu),2)==vertices,"Transform changed return contract");
        require(!std::memcmp(ram+plugin,expected.data(),60),"Transform descriptor differs from SDK layout");
        require(!std::memcmp(cpu.r+3,original.r+3,sizeof(cpu.r)-sizeof(cpu.r[0])*3),"Transform damaged caller registers");
        checkPluginGuards(ram);const auto* draw=NativeSceneTesting::pendingDraw();require(draw,"Admitted transform did not retain a batch");
        require(draw->geometry->vertices.size()==3 && draw->geometry->indices.empty() && !draw->indexed && draw->geometry->dynamic,"Shadow topology or streaming policy changed");
        require(draw->geometry->meshes.size()==1 && draw->geometry->meshes[0].indexCount==3 && draw->geometry->meshes[0].firstIndex==0,"Shadow primitive range changed");
        const auto retained=draw->geometry;
        for(unsigned i=0;i<3;++i) {
            const auto& vertex=retained->vertices[i];
            for(unsigned c=0;c<3;++c)require(vertex.position[c]==float(i*10+c)+0.25f,"36-byte vertex position decoded incorrectly");
            require(vertex.color==0x80402010u+i*0x11030507u && vertex.uv[0]==float(i)*0.125f && vertex.uv[1]==1.f-float(i)*0.25f,"36-byte vertex color/UV decoded incorrectly");
        }
        require(draw->depthTest && !draw->depthWrite && draw->vertexAlpha && draw->sourceBlend==5 && draw->destinationBlend==6 && draw->cullMode==2,"Shadow render states changed");
        require(draw->fogEnabled && draw->fogStart==10 && draw->fogEnd==90 && draw->zScale==-6 && draw->zShift==13,"Shadow camera/fog snapshot changed");
        std::memset(ram+vertices,0,108);put(ram,camera+0x20,1234.f);states[10]=1;
        const size_t before=submitted.size();cpu.pc=0x208910;SET_GPR_U32((&cpu),4,3);SET_GPR_U32((&cpu),31,caller+0x10);
        NativeSceneTesting::render(ram,&cpu,&runtime);
        require(submitted.size()==before+1 && submitted.back().geometry==retained && submitted.back().sourceBlend==5 && submitted.back().camera[0]==1,"Queued draw did not own its transform-time snapshot");
        require(cpu.pc==0x1f2498 && GPR_U32((&cpu),4)==1 && GPR_U32((&cpu),5)==uint32_t(-2),"Native draw bypassed the original ordered packet allocator");
        put(ram,gp-0x7c1c,packet);SET_GPR_U32((&cpu),2,1);NativeSceneTesting::finishMarker(ram,&cpu,&runtime);
        require(cpu.pc==caller+0x10 && GPR_U32((&cpu),31)==caller+0x10 && GPR_U32((&cpu),2)==1,"Ordered marker changed render return contract");
        require(get<uint64_t>(ram,packet)==0x1000000000008001ull && get<uint64_t>(ram,packet+8)==0xe && get<uint64_t>(ram,packet+16)==0x4e31303000000042ull && get<uint64_t>(ram,packet+24)==0x7f && get<uint32_t>(ram,gp-0x7c1c)==packet+32,"Native marker was not encoded into the ordered GIF stream");
        cpu.pc=0x2093c0;SET_GPR_U32((&cpu),31,caller+0x18);NativeSceneTesting::end(ram,&cpu,&runtime);
        require(cpu.pc==caller+0x18 && GPR_U32((&cpu),2)==1 && !NativeSceneTesting::pendingDraw(),"End changed success/cleanup contract");
        for(unsigned i=0;i<60;++i)require(!ram[plugin+i],"End did not clear exactly the SDK workspace");
        checkPluginGuards(ram);require(retained->vertices[2].position[0]==20.25f,"End invalidated queued geometry ownership");
    }
    unsigned rejected=0;
    for(unsigned variant=0;variant<14;++variant) {
        initialize(ram);auto cpu=entry();
        switch(variant) {
            case 0:SET_GPR_U32((&cpu),31,0x1a16f0);break;
            case 1:SET_GPR_U32((&cpu),5,0);break;
            case 2:SET_GPR_U32((&cpu),5,2);break;
            case 3:SET_GPR_U32((&cpu),5,65536);break;
            case 4:SET_GPR_U32((&cpu),6,vertices);break;
            case 5:SET_GPR_U32((&cpu),7,1);break;
            case 6:SET_GPR_U32((&cpu),4,0);break;
            case 7:SET_GPR_U32((&cpu),4,PS2_RAM_SIZE-36);break;
            case 8:put(ram,vertices,std::numeric_limits<float>::quiet_NaN());break;
            case 9:states[16]=2;break;
            case 10:states[10]=12;break;
            case 11:states[1]=raster;std::memset(ram+raster,0,128);put(ram,raster+0x20,5u);break;
            case 12:put(ram,gp-0x7aa0,0xfffffff0u);break;
            case 13:
                states[1]=raster;std::memset(ram+raster,0,128);put(ram,raster,raster);put(ram,raster+0x20,5u);
                put(ram,raster+0xc,256u);put(ram,raster+0x10,256u);put(ram,raster+0x14,8u);
                put(ram,raster+0x3c,0x0007c00620a13e00ull);break;
        }
        std::array<uint8_t,60> before;std::memcpy(before.data(),ram+plugin,60);
        const auto transforms=originalTransforms,renders=originalRenders,ends=originalEnds;
        NativeSceneTesting::transform(ram,&cpu,&runtime);require(originalTransforms==transforms+1 && !NativeSceneTesting::pendingDraw(),"Unsupported transform skipped the original pipeline");
        require(!std::memcmp(before.data(),ram+plugin,60),"Rejected transform changed SDK descriptor before fallback");
        cpu.pc=0x208910;NativeSceneTesting::render(ram,&cpu,&runtime);cpu.pc=0x2093c0;NativeSceneTesting::end(ram,&cpu,&runtime);
        require(originalRenders==renders+1 && originalEnds==ends+1,"Unsupported batch mixed native and legacy stages");
        checkPluginGuards(ram);++rejected;
    }
    std::cout<<"PASS: two original shadow caller contracts, immutable 36-byte vertex snapshots, exact plugin/End boundaries, ordered GIF marker and "<<rejected<<" whole-unit fallbacks\n";
    return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
