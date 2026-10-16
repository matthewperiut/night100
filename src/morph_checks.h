#pragma once
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

// Execute the disc's original routines, not a replacement morph implementation.
// Asymmetric signed inputs and tail lengths exercise all vector lanes, PHMADH's
// HI/LO side effects and the original VU0 conversion/store instructions.
inline bool checkOriginalMorphRoutines(PS2Runtime& runtime) {
    auto* ram=runtime.memory().getRDRAM();
    constexpr uint32_t destination=0x14000,pointers=0x14200,weights=0x14300,source=0x14400,sentinel=0x00fffffc;
    std::array<std::array<int16_t,40>,4> values{};
    for(unsigned a=0;a<4;++a)for(unsigned i=0;i<40;++i)
        values[a][i]=int16_t(int((i*7919+a*1217+37)%60001)-30000);
    const std::array<int16_t,4> blendWeights{-2048,4096,6144,8192};
    for(unsigned a=0;a<4;++a) {
        const uint32_t address=source+a*128;
        std::memcpy(ram+address,values[a].data(),sizeof(values[a]));
        std::memcpy(ram+pointers+a*4,&address,4);
    }
    std::memcpy(ram+weights,blendWeights.data(),sizeof(blendWeights));
    unsigned cases=0;
    for(unsigned terms:{1u,2u,4u})for(unsigned count:{1u,2u,3u,4u,5u,7u,8u,9u,11u,15u,16u,17u,31u}) {
        constexpr float scale=1.0f/16384.0f,guard=-123456.0f;
        std::array<float,40> output;output.fill(guard);std::memcpy(ram+destination,output.data(),sizeof(output));
        auto& state=runtime.cpu();state=R5900Context{};auto* cpu=&state;
        cpu->pc=terms==1?0x126830:terms==2?0x126730:0x1265f0;
        SET_GPR_U32(cpu,4,destination);SET_GPR_U32(cpu,5,terms==1?source:pointers);
        SET_GPR_U32(cpu,6,terms==1?count:weights);SET_GPR_U32(cpu,7,count);
        SET_GPR_U32(cpu,28,0x35f370);SET_GPR_U32(cpu,29,0x01fff000);SET_GPR_U32(cpu,31,sentinel);
        cpu->f[12]=scale;cpu->hi=cpu->lo=cpu->hi1=cpu->lo1=0xabcdef0123456789ull;
        for(unsigned steps=0;cpu->pc!=sentinel && steps<10000;++steps) {
            const auto fn=runtime.lookupFunction(cpu->pc);
            if(!fn)throw std::runtime_error("Morph regression reached missing function");
            fn(ram,cpu,&runtime);
        }
        if(cpu->pc!=sentinel)throw std::runtime_error("Morph regression did not return");
        std::memcpy(output.data(),ram+destination,sizeof(output));
        for(unsigned i=0;i<count;++i) {
            int64_t total=0;
            if(terms==1)total=values[0][i];
            else for(unsigned a=0;a<terms;++a)total+=int32_t(values[a][i])*int32_t(blendWeights[a]);
            const float expected=float(total)*scale;
            if(!std::isfinite(output[i]) || std::abs(output[i]-expected)>std::max(1e-5f,std::abs(expected)*1e-6f)) {
                std::cerr<<"FAIL original morph terms="<<terms<<" count="<<count<<" lane="<<i
                    <<" actual="<<output[i]<<" expected="<<expected<<'\n';return false;
            }
        }
        for(unsigned i=count;i<output.size();++i)if(output[i]!=guard) {
            std::cerr<<"FAIL original morph writes past count="<<count<<" terms="<<terms<<'\n';return false;
        }
        if(terms>1) {
            const unsigned block=((count-1)/8)*8,first=terms-2;
            std::array<uint64_t,4> accumulators{};
            for(unsigned lane=0;lane<4;++lane) {
                const unsigned i=block+4+lane;
                const int32_t odd=int32_t(values[first][i])*int32_t(blendWeights[first]);
                const int32_t even=int32_t(values[first+1][i])*int32_t(blendWeights[first+1]);
                const uint32_t sum=uint32_t(odd)+uint32_t(even);
                accumulators[lane]=uint64_t(sum)|(uint64_t(uint32_t(odd))<<32);
            }
            if(cpu->lo!=accumulators[0] || cpu->hi!=accumulators[1] || cpu->lo1!=accumulators[2] || cpu->hi1!=accumulators[3]) {
                std::cerr<<"FAIL PHMADH HI/LO lanes terms="<<terms<<" count="<<count<<'\n';return false;
            }
        }
        ++cases;
    }
    std::cout<<"PASS: "<<cases<<" original signed morph unpack/blend cases and PHMADH HI/LO lanes\n";
    return true;
}
