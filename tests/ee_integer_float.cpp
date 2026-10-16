#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_float_environment.h"
#include <bit>
#include <iostream>
#include <stdexcept>
#include <vector>
void dptofp_0x253070(uint8_t*,R5900Context*,PS2Runtime*);
void ps2___pack_f_0x2521c8(uint8_t*,R5900Context*,PS2Runtime*);
void dpadd_0x252890(uint8_t*,R5900Context*,PS2Runtime*);
void ps2___make_fp_0x252378(uint8_t*,R5900Context*,PS2Runtime*);
void ps2___pack_d_0x252468(uint8_t*,R5900Context*,PS2Runtime*);
void _fpadd_parts_0x252630(uint8_t*,R5900Context*,PS2Runtime*);
void litodp_0x252ee8(uint8_t*,R5900Context*,PS2Runtime*);
void ps2___floatdisf_0x251f50(uint8_t*,R5900Context*,PS2Runtime*);
void dpmul_0x252950(uint8_t*,R5900Context*,PS2Runtime*);
void ps2___muldi3_0x250620(uint8_t*,R5900Context*,PS2Runtime*);
void ps2___unpack_d_0x252580(uint8_t*,R5900Context*,PS2Runtime*);
namespace {
void require(bool condition,const char* reason){if(!condition)throw std::runtime_error(reason);}
// Independent integer-only nearest/even oracle: no host conversion or FP mode.
uint32_t expectedBits(int64_t value) {
 if(!value)return 0;
 const uint32_t sign=value<0?0x80000000u:0;
 const uint64_t magnitude=value<0?uint64_t(0)-uint64_t(value):uint64_t(value);
 unsigned exponent=63-std::countl_zero(magnitude);
 uint64_t fraction;
 if(exponent<=23)fraction=magnitude<<(23-exponent);
 else {
  const unsigned shift=exponent-23;
  fraction=magnitude>>shift;
  const uint64_t remainder=magnitude&((uint64_t(1)<<shift)-1),half=uint64_t(1)<<(shift-1);
  if(remainder>half || (remainder==half && (fraction&1)))++fraction;
  if(fraction==(uint64_t(1)<<24)){fraction>>=1;++exponent;}
 }
 return sign|((exponent+127)<<23)|(uint32_t(fraction)&0x7fffff);
}
}
int main(){try {
 PS2Runtime runtime;require(runtime.memory().initialize() && runtime.syncCoreSubsystems(),"RAM initialization failed");
 for(uint32_t pc=0x253070;pc<0x2530c8;pc+=4)require(runtime.registerFunction(pc,dptofp_0x253070),"original helper registration failed");
 for(uint32_t pc=0x2521c8;pc<0x252298;pc+=4)require(runtime.registerFunction(pc,ps2___pack_f_0x2521c8),"original helper registration failed");
 for(uint32_t pc=0x252890;pc<0x2528e8;pc+=4)require(runtime.registerFunction(pc,dpadd_0x252890),"original helper registration failed");
 for(uint32_t pc=0x252378;pc<0x2523a8;pc+=4)require(runtime.registerFunction(pc,ps2___make_fp_0x252378),"original helper registration failed");
 for(uint32_t pc=0x252468;pc<0x252580;pc+=4)require(runtime.registerFunction(pc,ps2___pack_d_0x252468),"original helper registration failed");
 for(uint32_t pc=0x252630;pc<0x252890;pc+=4)require(runtime.registerFunction(pc,_fpadd_parts_0x252630),"original helper registration failed");
 for(uint32_t pc=0x252ee8;pc<0x252fa0;pc+=4)require(runtime.registerFunction(pc,litodp_0x252ee8),"original helper registration failed");
 for(uint32_t pc=0x251f50;pc<0x252038;pc+=4)require(runtime.registerFunction(pc,ps2___floatdisf_0x251f50),"original helper registration failed");
 for(uint32_t pc=0x252950;pc<0x252c00;pc+=4)require(runtime.registerFunction(pc,dpmul_0x252950),"original helper registration failed");
 for(uint32_t pc=0x250620;pc<0x250698;pc+=4)require(runtime.registerFunction(pc,ps2___muldi3_0x250620),"original helper registration failed");
 for(uint32_t pc=0x252580;pc<0x252630;pc+=4)require(runtime.registerFunction(pc,ps2___unpack_d_0x252580),"original helper registration failed");
 EeFloatEnvironment scope;
 std::vector<int64_t> inputs={0,1,-1,INT32_MAX,INT32_MIN,int64_t(INT32_MAX)+1,int64_t(UINT32_MAX),int64_t(UINT32_MAX)+1,INT64_MAX,INT64_MIN};
 for(int64_t boundary:{int64_t(1)<<31,int64_t(1)<<32,int64_t(1)<<53})for(int64_t delta=-1024;delta<=1024;delta+=127)inputs.push_back(boundary+delta);
 for(int64_t boundary:{int64_t(1)<<31,int64_t(1)<<32})for(int64_t delta=0;delta<10000000;delta+=104729)inputs.push_back(boundary+delta);
 uint64_t random=0x251f502527b4ull;
 for(unsigned n=0;n<2000;++n){random^=random<<13;random^=random>>7;random^=random<<17;inputs.push_back(std::bit_cast<int64_t>(random));}
 unsigned failed=0;
 for(int64_t input:inputs){
  auto& cpu=runtime.cpu();cpu=R5900Context{};cpu.pc=0x251f50;SET_GPR_U32((&cpu),29,0x1ffe000);SET_GPR_U32((&cpu),31,0x280000);SET_GPR_S64((&cpu),4,input);
  for(unsigned step=0;cpu.pc!=0x280000 && step<10000;++step){auto fn=runtime.lookupFunction(cpu.pc);require(fn!=nullptr,"missing original conversion continuation");fn(runtime.memory().getRDRAM(),&cpu,&runtime);}
  require(cpu.pc==0x280000,"original conversion did not return");
  // GCC's original software __floatdisf rounds integer conversions to nearest,
  // independently of the EE scalar arithmetic rounding mode.
  const auto actualBits=std::bit_cast<uint32_t>(cpu.f[0]),oracleBits=expectedBits(input);
  const float expected=std::bit_cast<float>(oracleBits);
  if(actualBits!=oracleBits){++failed;std::cerr<<"input="<<input<<" actual="<<cpu.f[0]<<" expected="<<expected<<" bits="<<std::hex<<actualBits<<"/"<<oracleBits<<std::dec<<'\n';}
 }
 require(failed==0,"original signed64-to-float conversion mismatched boundary oracle");
 std::cout<<"PASS: original software integer conversion "<<inputs.size()<<" signed64 boundary cases\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
