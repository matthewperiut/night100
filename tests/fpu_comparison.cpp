#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>

void properSphereIsectTri_0x12ed60(uint8_t*,R5900Context*,PS2Runtime*);
void PointWithinTriangle_0x12f040(uint8_t*,R5900Context*,PS2Runtime*);
void FindNearestPointOnLine_0x12ef70(uint8_t*,R5900Context*,PS2Runtime*);
void p2sqrt_0x110ae0(uint8_t*,R5900Context*,PS2Runtime*);
void fabsf_0x250238(uint8_t*,R5900Context*,PS2Runtime*);

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
long double oracle(uint32_t bits) {
    const unsigned exponent=(bits>>23)&255u;
    if(!exponent)return 0; // Both signs and every denormal are EE zero.
    const long double significand=1.0L+static_cast<long double>(bits&0x7fffffu)/8388608.0L;
    const long double magnitude=std::ldexp(significand,int(exponent)-127);
    return bits>>31?-magnitude:magnitude;
}
void comparisons(uint32_t left,uint32_t right) {
    const float a=std::bit_cast<float>(left),b=std::bit_cast<float>(right);
    const auto x=oracle(left),y=oracle(right);const bool eq=x==y,lt=x<y,le=x<=y;
    const std::array<bool,16> expected={false,false,eq,eq,lt,lt,le,le,false,false,eq,eq,lt,lt,le,le};
    const std::array<bool,16> actual={bool(FPU_C_F_S(a,b)),bool(FPU_C_UN_S(a,b)),FPU_C_EQ_S(a,b),FPU_C_UEQ_S(a,b),
        FPU_C_OLT_S(a,b),FPU_C_ULT_S(a,b),FPU_C_OLE_S(a,b),FPU_C_ULE_S(a,b),bool(FPU_C_SF_S(a,b)),bool(FPU_C_NGLE_S(a,b)),
        FPU_C_SEQ_S(a,b),FPU_C_NGL_S(a,b),FPU_C_LT_S(a,b),FPU_C_NGE_S(a,b),FPU_C_LE_S(a,b),FPU_C_NGT_S(a,b)};
    for(unsigned condition=0;condition<16;++condition) {
        if(actual[condition]!=expected[condition]) {
            std::cerr<<"Comparison bits="<<std::hex<<left<<','<<right<<std::dec<<" condition="<<condition<<'\n';
            throw std::runtime_error("EE comparison differs from independent finite-format oracle");
        }
    }
}
using Vec=std::array<float,3>;
Vec add(Vec a,Vec b){for(unsigned i=0;i<3;++i)a[i]+=b[i];return a;}
Vec scale(Vec a,float s){for(auto& value:a)value*=s;return a;}
Vec cross(Vec a,Vec b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
Vec unit(Vec a){return scale(a,1/std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]));}
void geometry() {
    PS2Runtime runtime;require(runtime.memory().initialize(),"Geometry fixture RAM initialization failed");
    require(runtime.registerFunction(0x12f040,PointWithinTriangle_0x12f040) &&
        runtime.registerFunction(0x12ef70,FindNearestPointOnLine_0x12ef70) &&
        runtime.registerFunction(0x110ae0,p2sqrt_0x110ae0) && runtime.registerFunction(0x250238,fabsf_0x250238),
        "Geometry fixture original helper registration failed");
    auto* ram=runtime.memory().getRDRAM();
    const Vec origin={7,-11,13};
    struct Case{float u,v,height,radius,distance;bool hit;};
    const Case cases[]={{1.1f,1.1f,.5f,.91f,std::sqrt(.27f),true}, // closest point is on the opposite edge
        {2.2f,-.1f,.5f,.91f,std::sqrt(.30f),true},               // closest point is a vertex
        {.5f,.5f,.5f,.91f,.5f,true},                            // projected point lies inside
        {1.1f,1.1f,.5f,.4f,0,false}};
    for(Vec normal:{Vec{0,1,0},Vec{-.2f,.97f,.1f},Vec{.6f,.3f,-.7f}}) {
        normal=unit(normal);const Vec right=unit(cross(normal,Vec{0,0,1})),forward=cross(right,normal);
        const std::array<Vec,3> vertices={origin,add(origin,scale(right,2)),add(origin,scale(forward,2))};
        for(const auto& test:cases) {
            const Vec center=add(origin,add(add(scale(right,test.u),scale(forward,test.v)),scale(normal,test.height)));
            const std::array<float,4> sphere={center[0],center[1],center[2],test.radius};
            std::memcpy(ram+0x1000,sphere.data(),16);std::memcpy(ram+0x2000,vertices.data(),36);
            std::memcpy(ram+0x3000,normal.data(),12);std::memcpy(ram+0x300c,origin.data(),12);
            const uint32_t pointers[]={0x2000,0x200c,0x2018};std::memcpy(ram+0x301c,pointers,12);
            const float initialDistance=1e38f;std::memcpy(ram+0x4010,&initialDistance,4);
            R5900Context context{};auto* cpu=&context;cpu->pc=0x12ed60;
            SET_GPR_U32(cpu,4,0x1000);SET_GPR_U32(cpu,5,0x4000);SET_GPR_U32(cpu,6,0x4010);SET_GPR_U32(cpu,7,0x3000);
            SET_GPR_U32(cpu,29,0x1ff8000);SET_GPR_U32(cpu,31,0x280000);cpu->f[12]=test.radius;
            properSphereIsectTri_0x12ed60(ram,cpu,&runtime);
            float distance;std::memcpy(&distance,ram+0x4010,4);
            require(cpu->pc==0x280000 && GPR_U32(cpu,29)==0x1ff8000,"Original collision routine did not return cleanly");
            require((distance>0 && distance<=test.radius)==test.hit,"Original collision lost a sphere/triangle edge or vertex contact");
            if(test.hit) {
                require(std::abs(distance-test.distance)<3e-5f,"Original collision returned an incorrect closest-point distance");
                Vec tohit;std::memcpy(tohit.data(),ram+0x4000,12);
                const float length=std::sqrt(tohit[0]*tohit[0]+tohit[1]*tohit[1]+tohit[2]*tohit[2]);
                require(std::abs(length-distance)<3e-5f,"Original collision separation vector disagrees with its distance");
            }
        }
    }
}
}
int main(){try {
    constexpr uint32_t edges[]={0,0x80000000u,1,0x80000001u,0x007fffffu,0x807fffffu,0x00800000u,0x80800000u,
        0x3f000000u,0xbf000000u,0x3f800000u,0xbf800000u,0x7f000000u,0xff000000u,0x7f7fffffu,0xff7fffffu,
        0x7f800000u,0xff800000u,0x7f800001u,0xff800001u,0x7fc00000u,0xffc00000u,0x7fffffffu,0xffffffffu};
    for(uint32_t a:edges)for(uint32_t b:edges)comparisons(a,b);
    std::mt19937 random(0x12eef0);
    for(unsigned i=0;i<20000;++i)comparisons(random(),random());
    const float sentinel=FPU_MUL_S(1e38f,1e38f);
    require(std::bit_cast<uint32_t>(sentinel)==0x7fffffffu && FPU_C_LT_S(.49f,sentinel),
        "Squared collision distance sentinel must remain ordered above finite candidates");
    geometry();
    std::cout<<"PASS: 20576 finite EE float comparison pairs/all condition aliases and 12 original collision edge/vertex/face regressions\n";
    return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
