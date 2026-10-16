#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_float_environment.h"
#include "runtime/ee_scheduler.h"
#include <array>
#include <bit>
#include <cfenv>
#include <cstring>
#include <iostream>
#include <stdexcept>

void xFontBoxCreate_0x199240(uint8_t*,R5900Context*,PS2Runtime*);
void xFontBoxSetInset_0x198590(uint8_t*,R5900Context*,PS2Runtime*);
void xFontBoxSetDrawSpacing_0x1985e0(uint8_t*,R5900Context*,PS2Runtime*);
void xFontBoxSetDrawSize_0x1993d0(uint8_t*,R5900Context*,PS2Runtime*);
void xFontBoxRender_0x198730(uint8_t*,R5900Context*,PS2Runtime*);
void JustifyLine_0x1988e0(uint8_t*,R5900Context*,PS2Runtime*);
void ProcessWord_0x198e60(uint8_t*,R5900Context*,PS2Runtime*);
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
unsigned glyphs=0;
void leaf(uint8_t*,R5900Context* cpu,PS2Runtime*){cpu->pc=GPR_U32(cpu,31);}
void glyph(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime){++glyphs;leaf(ram,cpu,runtime);}
void install(PS2Runtime& runtime,uint32_t start,uint32_t end,PS2Runtime::RecompiledFunction function) {
    for(uint32_t pc=start;pc<end;pc+=4)require(runtime.registerFunction(pc,function),"fixture registration failed");
}
void call(PS2Runtime& runtime,R5900Context& cpu,uint32_t pc) {
    cpu.pc=pc;SET_GPR_U32((&cpu),31,0x280000);
    for(unsigned step=0;cpu.pc!=0x280000 && step<10000;++step) {
        auto fn=runtime.lookupFunction(cpu.pc);require(fn!=nullptr,"missing original font continuation");
        fn(runtime.memory().getRDRAM(),&cpu,&runtime);
    }
    require(cpu.pc==0x280000,"original font fixture did not return");
}
unsigned fontRows(PS2Runtime& runtime,int mode) {
    require(!std::fesetround(mode),"cannot set font fixture rounding");
    auto* ram=runtime.memory().getRDRAM();
    constexpr uint32_t font=0x36fa00,pool=0x36fd50,text=0x180000;
    std::memset(ram+font,0,424);std::memset(ram+pool,0,4*76);
    const uint16_t metrics[]={0,7,6,8};std::memcpy(ram+font,metrics,sizeof(metrics));
    const char* strings[]={"Sound Mode","Vibration Mode","Music Volume","Sound Effects Volume"};
    glyphs=0;
    for(unsigned row=0;row<4;++row) {
        std::memset(ram+pool,0,4*76);
        R5900Context cpu{};SET_GPR_U32((&cpu),29,0x1fff000);
        SET_GPR_U32((&cpu),4,2);SET_GPR_U32((&cpu),5,0xff00ff00);
        cpu.f[12]=150;cpu.f[13]=row==0?110:160+50*(row-1);
        cpu.f[14]=440;cpu.f[15]=row==0?60:40;
        call(runtime,cpu,0x199240);const uint32_t box=GPR_U32((&cpu),2);
        require(box==pool,"original box allocator moved unexpectedly");
        SET_GPR_U32((&cpu),4,box);cpu.f[12]=18;cpu.f[13]=36;call(runtime,cpu,0x1985e0);
        SET_GPR_U32((&cpu),4,box);for(unsigned i=12;i<16;++i)cpu.f[i]=2;call(runtime,cpu,0x198590);
        SET_GPR_U32((&cpu),4,box);cpu.f[12]=18;cpu.f[13]=24;call(runtime,cpu,0x1993d0);
        std::strcpy(reinterpret_cast<char*>(ram+text),strings[row]);std::memcpy(ram+box+0x44,&text,4);
        SET_GPR_U32((&cpu),4,box);call(runtime,cpu,0x198730);
    }
    return glyphs;
}
void schedulerGuest(uint8_t*,R5900Context*,PS2Runtime* runtime) {
    require(std::fegetround()==FE_TOWARDZERO,"scheduler did not establish EE rounding");
    runtime->requestStop();
}
void schedulerThrows(uint8_t*,R5900Context*,PS2Runtime*) {
    require(std::fegetround()==FE_TOWARDZERO,"throwing dispatch lost EE rounding");
    throw std::runtime_error("fixture exception");
}
void schedulerScope(int mode,bool throwing) {
    PS2Runtime runtime;require(runtime.memory().initialize() && runtime.syncCoreSubsystems(),"scheduler RAM initialization failed");
    require(runtime.registerFunction(0x1000,throwing?schedulerThrows:schedulerGuest),"scheduler guest registration failed");
    R5900Context cpu{};cpu.pc=0x1000;SET_GPR_U32((&cpu),29,0x180000);
    runtime.eeScheduler().reset(runtime.memory().getRDRAM(),cpu);
    require(!std::fesetround(mode),"cannot set host fixture rounding");
    bool caught=false;try{runtime.eeScheduler().run();}catch(const std::runtime_error&){caught=true;}
    require(caught==throwing,"scheduler exception behavior changed");
    require(std::fegetround()==mode,"scheduler leaked EE rounding to host work");
}
void dividerCompatibility() {
    struct Case {uint32_t left,right,div,root,rsqrt;};
    std::array<Case,128> cases{};
    uint32_t seed=0x1993d0;
    auto random=[&]{seed=seed*1664525u+1013904223u;return seed;};
    require(!std::fesetround(FE_TONEAREST),"cannot set nearest oracle mode");
    for(auto& item:cases) {
        item.left=((random()%60+90)<<23)|(random()&0x807fffffu);
        item.right=((random()%60+90)<<23)|(random()&0x7fffffu);
        volatile float a=std::bit_cast<float>(item.left),b=std::bit_cast<float>(item.right);
        volatile float root=std::sqrt(b),div=a/b,rsqrt=a/root;
        item.div=std::bit_cast<uint32_t>(float(div));item.root=std::bit_cast<uint32_t>(float(root));item.rsqrt=std::bit_cast<uint32_t>(float(rsqrt));
    }
    for(int mode:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
        require(!std::fesetround(mode),"cannot set divider fixture mode");
        for(const auto& item:cases) {
            volatile float a=std::bit_cast<float>(item.left),b=std::bit_cast<float>(item.right);
            require(std::bit_cast<uint32_t>(FPU_DIV_S(a,b))==item.div,"divider nearest compatibility depends on host mode");
            require(std::bit_cast<uint32_t>(FPU_SQRT_S(b))==item.root,"root nearest compatibility depends on host mode");
            require(std::bit_cast<uint32_t>(FPU_RSQRT_S(a,b))==item.rsqrt,"reciprocal root lost its rounded root stage");
        }
        require(std::bit_cast<uint32_t>(ps2_from_double_nearest(1.0+0x1p-24))==0x3f800000,"even midpoint rounding changed");
        require(std::bit_cast<uint32_t>(ps2_from_double_nearest(1.0+3*0x1p-24))==0x3f800002,"odd midpoint rounding changed");
        require(std::bit_cast<uint32_t>(FPU_DIV_S(std::bit_cast<float>(0x7f800000u),2.0f))==0x7f000000u,"divider lost extended exponent input");
        require(std::bit_cast<uint32_t>(FPU_DIV_S(-1.0f,0.0f))==0xffffffffu,"negative zero-divisor sign changed");
        require(std::bit_cast<uint32_t>(FPU_SQRT_S(-0.0f))==0,"root retained a negative zero sign");
    }
}
void edges() {
    EeFloatEnvironment scope;
    volatile float one=1.0f,halfUlp=0x1p-24f,threeQuarterUlp=0x1.8p-24f;
    require(std::bit_cast<uint32_t>(FPU_ADD_S(one,threeQuarterUlp))==0x3f800000,"positive addition rounded away from zero");
    require(std::bit_cast<uint32_t>(FPU_ADD_S(-one,-threeQuarterUlp))==0xbf800000,"negative addition rounded away from zero");
    require(std::bit_cast<uint32_t>(FPU_SUB_S(one,-halfUlp))==0x3f800000,"halfway subtraction did not chop");
    require(std::bit_cast<uint32_t>(FPU_MUL_S(std::bit_cast<float>(0x3f800001u),std::bit_cast<float>(0x3f800001u)))==0x3f800002,"multiplication did not chop");
    require(std::bit_cast<uint32_t>(FPU_ADD_S(std::bit_cast<float>(0x7f7fffffu),std::bit_cast<float>(0x7f7fffffu)))==0x7fffffffu,"extended exponent addition changed");
    require(std::bit_cast<uint32_t>(FPU_MUL_S(std::bit_cast<float>(0x00800000u),.5f))==0,"underflow did not flush to zero");
    require(std::bit_cast<uint32_t>(FPU_DIV_S(one,0.0f))==0x7fffffffu,"zero-divisor saturation changed");
    const auto wide=PS2_VADD(_mm_set1_ps(std::bit_cast<float>(0x7f7fffffu)),_mm_set1_ps(std::bit_cast<float>(0x7f7fffffu)));
    alignas(16) uint32_t lanes[4];_mm_storeu_si128(reinterpret_cast<__m128i*>(lanes),_mm_castps_si128(wide));
    for(auto lane:lanes)require(lane==0x7fffffffu,"vector chop overflow lost the extended exponent");
}
}
// ProcessWord's generated JAL to iFontDrawChar is a guarded direct call: it runs
// this symbol only when the function table holds it. The fixture registers its
// own glyph routine at 0x199ab0, so the guard always dispatches there instead.
void iFontDrawChar_0x199ab0(uint8_t*,R5900Context*,PS2Runtime*){throw std::runtime_error("iFontDrawChar direct call bypassed the fixture registration");}
void iFontDrawBackdrop_0x19a750(uint8_t*,R5900Context*,PS2Runtime*){throw std::runtime_error("iFontDrawBackdrop direct call bypassed the fixture registration");}

void iFontRestoreRenderState_0x19ab40(uint8_t*,R5900Context*,PS2Runtime*){throw std::runtime_error("iFontRestoreRenderState_0x19ab40 direct call bypassed the fixture registration");}
void iFontSaveRenderState_0x19ad10(uint8_t*,R5900Context*,PS2Runtime*){throw std::runtime_error("iFontSaveRenderState_0x19ad10 direct call bypassed the fixture registration");}
void iFontSetBackdropRenderState_0x19acc0(uint8_t*,R5900Context*,PS2Runtime*){throw std::runtime_error("iFontSetBackdropRenderState_0x19acc0 direct call bypassed the fixture registration");}
void iFontSetCharRenderState_0x19ac20(uint8_t*,R5900Context*,PS2Runtime*){throw std::runtime_error("iFontSetCharRenderState_0x19ac20 direct call bypassed the fixture registration");}

int main() {
    const int saved=std::fegetround();
    try {
        PS2Runtime runtime;require(runtime.memory().initialize() && runtime.syncCoreSubsystems(),"font RAM initialization failed");
        install(runtime,0x199240,0x1993cc,xFontBoxCreate_0x199240);
        install(runtime,0x198590,0x1985e0,xFontBoxSetInset_0x198590);
        install(runtime,0x1985e0,0x198624,xFontBoxSetDrawSpacing_0x1985e0);
        install(runtime,0x1993d0,0x199414,xFontBoxSetDrawSize_0x1993d0);
        install(runtime,0x198730,0x1988dc,xFontBoxRender_0x198730);
        install(runtime,0x1988e0,0x198e5c,JustifyLine_0x1988e0);
        install(runtime,0x198e60,0x199238,ProcessWord_0x198e60);
        for(uint32_t pc:{0x19ad10u,0x19acc0u,0x19a750u,0x19ab40u,0x19ac20u})install(runtime,pc,pc+4,leaf);
        install(runtime,0x199ab0,0x199ab4,glyph);
        const auto nearest=fontRows(runtime,FE_TONEAREST),chop=fontRows(runtime,FE_TOWARDZERO);
        std::cout<<"font glyphs nearest="<<nearest<<" chop="<<chop<<std::endl;
        require(nearest==9,"nearest-rounding fixture no longer reproduces lost Options rows");
        require(chop==51,"EE rounding did not preserve all four original Options labels");
        edges();
        dividerCompatibility();
        for(int mode:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO})for(bool throwing:{false,true})schedulerScope(mode,throwing);
        std::fesetround(saved);
        std::cout<<"PASS: original Options font layout 9 -> 51 glyphs, scalar rounding/range edges, 1536 divider/root comparisons and 8 scheduler scope/exception cases\n";
        return 0;
    }catch(const std::exception& error){std::fesetround(saved);std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
