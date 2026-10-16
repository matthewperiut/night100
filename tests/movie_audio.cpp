#include "movie_audio.h"
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
using Bytes=std::vector<uint8_t>;
unsigned checks;
void require(bool value,const char* name){++checks;if(!value)throw std::runtime_error(name);}
void le(Bytes& b,uint32_t v){for(unsigned n=0;n<4;++n)b.push_back(uint8_t(v>>(8*n)));}
Bytes source(unsigned channels=2,unsigned interleave=4,unsigned rate=48000,unsigned type=1){
    Bytes b={'S','S','h','d'};le(b,24);le(b,type);le(b,rate);le(b,channels);le(b,interleave);le(b,~0u);le(b,~0u);
    b.insert(b.end(),{'S','S','b','d'});le(b,channels*interleave*2);
    // Two channel-block groups with distinguishable signed samples.
    for(unsigned block=0;block<2;++block)for(unsigned ch=0;ch<channels;++ch)
        for(unsigned f=0;f<interleave/2;++f){const int16_t s=int16_t((ch? -1:1)*int(1+block*100+ch*10+f));b.push_back(uint8_t(s));b.push_back(uint8_t(uint16_t(s)>>8));}
    return b;
}
void packet(Bytes& out,const Bytes& data,std::optional<uint64_t> pts=16307){
    Bytes p={0x80,uint8_t(pts?0x80:0),uint8_t(pts?5:0)};
    if(pts){const auto n=*pts;p.insert(p.end(),{uint8_t(0x21|((n>>29)&14)),uint8_t(n>>22),uint8_t(1|((n>>14)&254)),uint8_t(n>>7),uint8_t(1|((n<<1)&254))});}
    p.insert(p.end(),{0xff,0xa0,0,0});p.insert(p.end(),data.begin(),data.end());
    out.insert(out.end(),{0,0,1,0xbd,uint8_t(p.size()>>8),uint8_t(p.size())});out.insert(out.end(),p.begin(),p.end());
}
Bytes stream(const Bytes& s,size_t split=19){
    Bytes out={0,0,1,0xba,0x44,0,0,0,0,0,0,0,0,0};
    out.insert(out.end(),{0,0,1,0xe0,0,3,0x80,0,0});
    packet(out,Bytes(s.begin(),s.begin()+std::min(split,s.size())));
    packet(out,Bytes(s.begin()+std::min(split,s.size()),s.end()),18216);
    out.insert(out.end(),{0,0,1,0xb9});return out;
}
struct Fixture {
    std::filesystem::path path=std::filesystem::temp_directory_path()/("night100-movie-audio-"+std::to_string(getpid())+".pss");
    ~Fixture(){std::error_code ec;std::filesystem::remove(path,ec);}
    MoviePlayback::MovieAudio read(const Bytes& b){std::ofstream f(path,std::ios::binary|std::ios::trunc);f.write(reinterpret_cast<const char*>(b.data()),std::streamsize(b.size()));f.close();return MoviePlayback::loadOriginalMovieAudio(path);}
    void rejects(const Bytes& b){bool failed=false;try{(void)read(b);}catch(const std::runtime_error&){failed=true;}require(failed,"malformed stream accepted");}
};
}
int main(int argc,char** argv){try{
    if(argc==2){auto a=MoviePlayback::loadOriginalMovieAudio(argv[1]);std::cout<<a.sampleRate<<" Hz "<<a.channels<<" channels "<<a.interleaved.size()/a.channels<<" frames, initialPTS "<<a.initialPtsSeconds.value_or(-1)<<'\n';return 0;}
    Fixture f;
    for(unsigned channels:{1u,2u})for(unsigned inter:{2u,4u,512u})for(unsigned split:{0u,1u,7u,31u,39u,40u,45u}){
        const auto a=f.read(stream(source(channels,inter),split));
        require(a.sampleRate==48000 && a.channels==channels,"PCM format differs");
        require(a.initialPtsSeconds && std::abs(*a.initialPtsSeconds-16307./90000)<1e-12,"PTS differs");
        require(a.interleaved.size()==channels*inter*2/2,"PCM count differs");
        for(unsigned block=0;block<2;++block)for(unsigned sample=0;sample<inter/2;++sample)for(unsigned ch=0;ch<channels;++ch)
            require(a.interleaved[(block*inter/2+sample)*channels+ch]==(ch?-1:1)*int(1+block*100+ch*10+sample),"PCM deinterleave differs");
    }
    require(f.read({0,0,1,0xb9}).interleaved.empty(),"audio-less stream differs");
    Bytes noPts;packet(noPts,source(),{});noPts.insert(noPts.end(),{0,0,1,0xb9});require(!f.read(noPts).initialPtsSeconds,"invented PTS");
    Bytes wrap;packet(wrap,source(),(1ull<<33)-1);wrap.insert(wrap.end(),{0,0,1,0xb9});require(std::abs(*f.read(wrap).initialPtsSeconds-double((1ull<<33)-1)/90000)<1e-10,"33bit PTS lost");
    auto valid=stream(source());for(size_t n=42;n<valid.size()-4;++n){Bytes cut(valid.begin(),valid.begin()+n);if(n==0)continue;f.rejects(cut);}
    for(unsigned type:{0u,2u,0xffffffffu})f.rejects(stream(source(2,4,48000,type)));
    for(unsigned channels:{0u,3u,49u})f.rejects(stream(source(channels,4)));
    f.rejects(stream(source(2,3)));f.rejects(stream(source(2,4,0)));f.rejects(stream(source(2,4,999999)));
    for(unsigned at:{0u,4u,32u,36u}){auto broken=source();broken[at]^=0x10;f.rejects(stream(broken));}
    auto extra=source();extra.push_back(0);f.rejects(stream(extra));auto missing=source();missing.pop_back();f.rejects(stream(missing));
    auto flags=valid;flags[14+9+6+1]=0x40;f.rejects(flags);
    std::cout<<"Movie audio checks passed: "<<checks<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
