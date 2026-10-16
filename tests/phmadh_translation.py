#!/usr/bin/env python3
"""Compile the actual PHMADH emitter text and compare with a scalar EE oracle."""
import ast
import pathlib
import platform
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
source = (ROOT / "vendor/PS2Recomp/ps2xRecomp/src/lib/mmi_translation_helpers.cpp").read_text()
body = source.split("CodeGenerator::translatePHMADH(", 1)[1].split("CodeGenerator::translatePMSUBH(", 1)[0]
emission = "".join(ast.literal_eval(s) for s in re.findall(r'"(?:\\.|[^"\\])*"', body))
# Expand fmt's literal braces before substituting the three register operands.
emission = emission.replace("{{", "{").replace("}}", "}")
for operand in ("sourceRegister", "targetRegister", "destinationRegister"):
    emission = emission.replace("{}", operand, 1)
if "{}" in emission:
    raise RuntimeError("Unexpected PHMADH emitter format")

arm = platform.machine() in ("arm64", "aarch64")
header = "sse2neon.h" if arm else "immintrin.h"
program = r'''
#include "HEADER"
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
struct Context { __m128i r[32]{}; uint64_t hi=0,lo=0,hi1=0,lo1=0; };
#define GPR_VEC(c, index) ((c)->r[index])
#define SET_GPR_VEC(c, index, value) do { if(index) (c)->r[index]=(value); } while(0)
static void execute(Context* ctx,unsigned sourceRegister,unsigned targetRegister,unsigned destinationRegister) {
EMISSION
}
int main() {
    uint32_t random=0x20349126;unsigned cases=0;
    auto next=[&](){random^=random<<13;random^=random>>17;random^=random<<5;return random;};
    for(unsigned n=0;n<10002;++n)for(unsigned destination:{0u,1u,2u,3u}) {
        std::array<int16_t,8> a,b;
        for(unsigned i=0;i<8;++i){a[i]=int16_t(next());b[i]=int16_t(next());}
        if(n==0){a.fill(-32768);b.fill(-32768);}
        if(n==1){a={1,-2,3,-4,5,-6,7,-8};b={-11,12,-13,14,-15,16,-17,18};}
        Context c{};std::memcpy(&c.r[1],a.data(),16);std::memcpy(&c.r[2],b.data(),16);
        c.hi=c.lo=c.hi1=c.lo1=0xabcdef0123456789ull;
        std::array<uint32_t,4> expected{};std::array<uint64_t,4> accumulators{};
        for(unsigned i=0;i<4;++i) {
            const int32_t odd=int32_t(a[i*2+1])*int32_t(b[i*2+1]);
            const int32_t even=int32_t(a[i*2])*int32_t(b[i*2]);
            expected[i]=uint32_t(odd)+uint32_t(even);
            accumulators[i]=uint64_t(expected[i])|(uint64_t(uint32_t(odd))<<32);
        }
        execute(&c,1,2,destination);
        std::array<uint32_t,4> result{};std::memcpy(result.data(),&c.r[destination],16);
        if(result!=(destination?expected:std::array<uint32_t,4>{}) || c.lo!=accumulators[0] || c.hi!=accumulators[1]
            || c.lo1!=accumulators[2] || c.hi1!=accumulators[3]) {
            std::cerr<<"FAIL PHMADH case="<<n<<" destination="<<destination<<'\n';return 1;
        }
        ++cases;
    }
    std::cout<<"PASS: "<<cases<<" PHMADH emitted-code lane/alias/zero/overflow and HI/LO oracle cases\n";
}
'''.replace("HEADER", header).replace("EMISSION", emission)
with tempfile.TemporaryDirectory(prefix="night100-phmadh-") as directory:
    path = pathlib.Path(directory)
    (path / "check.cpp").write_text(program)
    flags = ["-I", str(ROOT / "build/toolchain/_deps/sse2neon-src")] if arm else ["-msse4.1"]
    subprocess.run(["clang++", "-std=c++20", "-O2", *flags, str(path / "check.cpp"), "-o", str(path / "check")], check=True)
    subprocess.run([str(path / "check")], check=True)
