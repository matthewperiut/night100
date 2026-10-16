#include "emulator/core/iop_spu2.h"
#include "runtime/state_archive.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <string_view>

using ps2x::iop::detail::IopSpu2;
static unsigned checks;
static void require(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
static std::vector<uint8_t> save(IopSpu2& s) { ps2x::state::Writer w; s.state(w); return std::move(w.data); }
static void load(IopSpu2& s,const std::vector<uint8_t>& b) { ps2x::state::Reader r(b); s.state(r); r.finish(); }
static void address(IopSpu2& s,uint32_t reg,uint32_t value) { s.write(reg,uint16_t(value>>16)); s.write(reg+2,uint16_t(value)); }
static uint32_t randomState=0x173249ab;
static uint32_t next() { randomState^=randomState<<13; randomState^=randomState>>17; randomState^=randomState<<5; return randomState; }
static void configure(IopSpu2& s,unsigned voices,unsigned predictor,unsigned mode)
{
    std::vector<uint8_t> ram(IopSpu2::RamBytes);
    for(size_t i=0;i<ram.size();++i) ram[i]=uint8_t(next());
    for(unsigned v=0;v<48;++v)
        for(unsigned block=0;block<16;++block)
        {
            const size_t byte=(0x2000+v*0x100)*2+block*16;
            ram[byte]=uint8_t(((predictor+block)%6)<<4 | ((mode+block)%16));
            // Loop marker and terminating/looping ENDX cases.
            ram[byte+1]=block==2 ? 4 : block==15 ? (mode%2 ? 3 : 1) : 0;
        }
    address(s,0x1a8,0); s.transfer(0,ram,0,ram.size(),true);
    for(unsigned c=0;c<2;++c)
    {
        const uint32_t base=c*0x400;
        s.write(base+0x188,0xffa5); s.write(base+0x18a,0x00ff);
        s.write(base+0x190,0xff5a); s.write(base+0x192,0x00ff);
        s.write(0x760+c*0x28,mode%2 ? 0x4567 : 0x3fff);
        s.write(0x762+c*0x28,mode%3 ? 0x3fff : 0x6fff);
    }
    constexpr unsigned pitches[]={0,0x800,0x1000,0x3fff,0xffff};
    constexpr unsigned adsr1[]={0x00ff,0x8f0f,0x7f70,0xffff};
    constexpr unsigned adsr2[]={0x1fc0,0xdfec,0x0000,0xffff};
    for(unsigned v=0;v<voices;++v)
    {
        const uint32_t base=(v/24)*0x400+(v%24)*16;
        s.write(base,uint16_t(next())); s.write(base+2,uint16_t(next()));
        s.write(base+4,uint16_t(pitches[(v+mode)%5]));
        s.write(base+6,uint16_t(adsr1[(v+mode)%4])); s.write(base+8,uint16_t(adsr2[(v+mode)%4]));
        address(s,(v/24)*0x400+0x1c0+(v%24)*12,0x2000+v*0x100);
        address(s,(v/24)*0x400+0x1c4+(v%24)*12,0x2010+v*0x100);
    }
    for(unsigned c=0;c<2;++c)
    {
        const unsigned count=std::min(24u,voices>c*24 ? voices-c*24 : 0u);
        const uint32_t mask=count ? (1u<<count)-1 : 0;
        s.write(c*0x400+0x1a0,uint16_t(mask)); s.write(c*0x400+0x1a2,uint16_t(mask>>16));
    }
}
static void compare(IopSpu2& reference,IopSpu2& candidate)
{
    const auto a=reference.audioSamples(),b=candidate.audioSamples();
    require(a.size()==b.size(),"PCM size differs");
    require(a.empty() || !std::memcmp(a.data(),b.data(),a.size_bytes()),"PCM bits differ");
    require(save(reference)==save(candidate),"SPU archive differs");
}
static void differential()
{
    for(unsigned voices: {0u,1u,3u,24u,48u}) for(unsigned mode=0;mode<6;++mode)
    {
        setenv("NIGHT100_REFERENCE_SPU_MIX","1",1); IopSpu2 reference;
        setenv("NIGHT100_REFERENCE_SPU_MIX","0",1); IopSpu2 candidate;
        configure(reference,voices,mode,mode); load(candidate,save(reference));
        uint64_t samples=0;
        constexpr unsigned steps[]={0,1,27,28,29,31,32,33,127,800,2345,1,77};
        for(unsigned i=0;i<std::size(steps);++i)
        {
            samples+=steps[i];
            const auto cycle=samples*IopSpu2::CyclesPerSample+(i%3)*23;
            reference.advanceTo(cycle); candidate.advanceTo(cycle); compare(reference,candidate);
            if(i==2) // Manual loop changes during an active voice.
                for(auto* s:{&reference,&candidate}) address(*s,0x1c4,0x2020);
            if(i==4) // Routing/pitch/envelope changes only between synchronous calls.
                for(auto* s:{&reference,&candidate}) { s->write(4,0x3fff); s->write(8,0x4021); s->write(0x190,0xffff); }
            if(i==6) // Restore mid-ADPCM block into a fresh native mixer.
            {
                IopSpu2 cold; load(cold,save(candidate)); cold.advanceTo(cycle+11*768);
                candidate.advanceTo(cycle+11*768); reference.advanceTo(cycle+11*768);
                compare(reference,cold); compare(reference,candidate); samples+=11;
            }
            if(i==8)
                for(auto* s:{&reference,&candidate}) { s->write(0x1a4,0x5555); s->write(0x5a4,0xaaaa); s->write(0x340,0xffff); }
        }
        // Wrapping upload overwrites decoding RAM between advances, both cores.
        std::vector<uint8_t> io(1024); for(auto& b:io)b=uint8_t(next());
        for(auto* s:{&reference,&candidate}) { address(*s,0x5a8,0xffffd); s->transfer(1,io,1011,95,true); }
        samples+=800; reference.advanceTo(samples*768);candidate.advanceTo(samples*768); compare(reference,candidate);
        std::vector<uint8_t> ra(1024),rb(1024);
        address(reference,0x5a8,0xffffd);address(candidate,0x5a8,0xffffd);
        reference.transfer(1,ra,1011,95,false);candidate.transfer(1,rb,1011,95,false);require(ra==rb,"DMA readback differs");
        // Producer saturation must still advance all voices and readback state.
        samples+=97001;reference.advanceTo(samples*768);candidate.advanceTo(samples*768);compare(reference,candidate);
        reference.clearAudioSamples();candidate.clearAudioSamples();samples+=37;
        reference.advanceTo(samples*768);candidate.advanceTo(samples*768);compare(reference,candidate);
    }
    std::cout<<"SPU differential checks passed: "<<checks<<" (PCM and serialized state)\n";
}
static void benchmark()
{
    for(unsigned voices:{0u,1u,3u,24u,48u}) for(unsigned batch:{1u,8u,32u,800u}) for(bool ref:{true,false})
    {
        setenv("NIGHT100_REFERENCE_SPU_MIX",ref?"1":"0",1);
        IopSpu2 s;configure(s,voices,2,1);
        const auto begin=std::chrono::steady_clock::now();
        for(uint64_t n=batch;n<=96000;n+=batch){s.advanceTo(n*768);s.clearAudioSamples();}
        const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        std::cout<<"voices="<<voices<<" batch="<<batch<<" reference="<<ref<<" ms="<<ms<<'\n';
    }
}
int main(int argc,char** argv)
{
    unsetenv("NIGHT100_SPU_WORK_COUNTERS");unsetenv("NIGHT100_CAPTURE_SPU");
    try { if(argc>1 && std::string_view(argv[1])=="--benchmark")benchmark();else differential(); }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
