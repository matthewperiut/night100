#include "replay_clock.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
constexpr std::array<uint8_t,8> magic{'N','1','0','0','C','L','K',0};
constexpr uint64_t maxFrames=60ull*60*60*24*31,maxBytes=256ull*1024*1024;
constexpr size_t headerBytes=20,recordBytes=21;
[[noreturn]] void invalid(const std::string& reason){throw std::runtime_error("Replay clock: "+reason);}
uint32_t checksum(const uint8_t* data,size_t size){uint32_t value=2166136261u;for(size_t i=0;i<size;++i){value^=data[i];value*=16777619u;}return value;}
void put(uint8_t* data,uint64_t value,unsigned bytes){for(unsigned i=0;i<bytes;++i)data[i]=uint8_t(value>>(i*8));}
uint64_t get(const uint8_t* data,unsigned bytes){uint64_t value=0;for(unsigned i=0;i<bytes;++i)value|=uint64_t(data[i])<<(i*8);return value;}
void boundedFrame(uint64_t frame){if(frame>maxFrames)invalid("frame exceeds the 31-day recording limit");}
}
struct ReplayClock::Impl {
    struct Sample {uint64_t frame,ticks;};
    InputReplay::Mode mode;
    FILE* output=nullptr;
    std::vector<Sample> samples;
    size_t next=0;
    uint64_t count=0,lastFrame=0,lastTicks=0,finalFrame=0,frameFloor=0;
    bool finished=false;
    explicit Impl(InputReplay::Mode value):mode(value){}
    ~Impl(){if(output)std::fclose(output);}
    void write(const uint8_t* data,size_t size) {
        if(std::fwrite(data,1,size,output)!=size)invalid("cannot write recording: "+std::string(std::strerror(errno)));
    }
    void record(uint8_t kind,uint64_t frame,uint64_t value) {
        constexpr uint64_t capacity=(maxBytes-headerBytes-recordBytes)/recordBytes;
        if(count>capacity || (kind==1 && count==capacity))invalid("recording exceeds the 256 MiB limit");
        std::array<uint8_t,recordBytes> bytes{};bytes[0]=kind;put(bytes.data()+1,frame,8);put(bytes.data()+9,value,8);
        put(bytes.data()+17,checksum(bytes.data(),17),4);write(bytes.data(),bytes.size());
    }
};
ReplayClock::ReplayClock(InputReplay::Mode mode,const std::filesystem::path& path):impl(std::make_unique<Impl>(mode)) {
    if(path.empty())invalid("empty recording path");
    if(mode==InputReplay::Mode::Record) {
        // fdopen preserves O_EXCL without an intervening close/reopen race.
        // stdio buffers these small records; only completed()/finish() flush.
        const int fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
        if(fd<0)invalid("cannot create "+path.string()+": "+std::strerror(errno));
        impl->output=::fdopen(fd,"wb");
        if(!impl->output){::close(fd);invalid("cannot create buffered recording stream");}
#if defined(__SWITCH__)
        if(std::setvbuf(impl->output,nullptr,_IOFBF,65536))invalid("cannot buffer clock recording");
#endif
        std::array<uint8_t,headerBytes> bytes{};
        std::copy(magic.begin(),magic.end(),bytes.begin());put(bytes.data()+8,1,4);put(bytes.data()+12,0,4);
        put(bytes.data()+16,checksum(bytes.data(),16),4);impl->write(bytes.data(),bytes.size());return;
    }
    std::error_code error;const auto size=std::filesystem::file_size(path,error);
    if(error || size>maxBytes || size<headerBytes+recordBytes || (size-headerBytes)%recordBytes)
        invalid("missing, oversized or truncated recording");
    std::ifstream input(path,std::ios::binary);std::array<uint8_t,headerBytes> header{};
    if(!input.read(reinterpret_cast<char*>(header.data()),header.size()))invalid("cannot read header");
    if(!std::equal(magic.begin(),magic.end(),header.begin()) || get(header.data()+8,4)!=1 || get(header.data()+12,4)!=0)
        invalid("unsupported recording header");
    if(get(header.data()+16,4)!=checksum(header.data(),16))invalid("header checksum mismatch");
    bool footer=false;uint64_t previousFrame=0,previousTicks=0;
    const uint64_t records=(size-headerBytes)/recordBytes;
    impl->samples.reserve(size_t(records-1));
    for(uint64_t i=0;i<records;++i) {
        std::array<uint8_t,recordBytes> bytes{};
        if(!input.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))invalid("truncated record");
        if(get(bytes.data()+17,4)!=checksum(bytes.data(),17))invalid("record checksum mismatch");
        const auto frame=get(bytes.data()+1,8),value=get(bytes.data()+9,8);boundedFrame(frame);
        if(footer)invalid("data after session footer");
        if(bytes[0]==1) {
            if(!impl->samples.empty() && (frame<previousFrame || value<previousTicks))invalid("unordered frame or decreasing clock ticks");
            impl->samples.push_back({frame,value});previousFrame=frame;previousTicks=value;
        } else if(bytes[0]==2) {
            if(i+1!=records || value!=impl->samples.size() || frame<previousFrame)invalid("invalid final frame or sample count");
            impl->finalFrame=frame;footer=true;
        } else invalid("unknown record kind");
    }
    if(!footer)invalid("missing session footer");
}
ReplayClock::~ReplayClock()=default;
uint64_t ReplayClock::sample(uint64_t frame,uint64_t actualTicks) {
    boundedFrame(frame);if(impl->finished)invalid("sample after session finish");
    if(frame<impl->frameFloor)invalid("clock call precedes a completed input frame");
    if(impl->mode==InputReplay::Mode::Replay) {
        if(impl->next==impl->samples.size())invalid("unexpected extra clock call at input frame "+std::to_string(frame));
        const auto& sample=impl->samples[impl->next];
        if(frame!=sample.frame)invalid("clock call order differs: expected input frame "+std::to_string(sample.frame)+", got "+std::to_string(frame));
        ++impl->next;impl->lastFrame=frame;return sample.ticks;
    }
    if(impl->count && (frame<impl->lastFrame || actualTicks<impl->lastTicks))invalid("unordered frame or decreasing native clock ticks");
    impl->record(1,frame,actualTicks);++impl->count;impl->lastFrame=frame;impl->lastTicks=actualTicks;return actualTicks;
}
void ReplayClock::completed(uint64_t frame) {
    boundedFrame(frame);if(impl->finished)invalid("completed frame after session finish");
    if(frame<impl->lastFrame || frame<impl->frameFloor)invalid("completed frame precedes its clock calls");
    if(impl->mode==InputReplay::Mode::Replay) {
        if(frame>impl->finalFrame)invalid("completed frame exceeds recorded session");
    } else {
#if defined(__SWITCH__)
        if(frame%60==0 && std::fflush(impl->output))invalid("cannot flush clock recording batch");
#else
        if(std::fflush(impl->output))invalid("cannot flush completed-frame clock records");
#endif
    }
    impl->frameFloor=frame;
}
void ReplayClock::finish(uint64_t finalFrame) {
    boundedFrame(finalFrame);
    if(impl->finished){if(finalFrame!=impl->finalFrame)invalid("session already finished at another frame");return;}
    if(finalFrame<impl->lastFrame || finalFrame<impl->frameFloor)invalid("final frame precedes its clock calls");
    if(impl->mode==InputReplay::Mode::Replay) {
        if(impl->next!=impl->samples.size())invalid("unconsumed clock calls at session end: "+std::to_string(impl->samples.size()-impl->next));
        if(finalFrame!=impl->finalFrame)invalid("final input frame differs from recording");
    } else {
        impl->record(2,finalFrame,impl->count);
        if(std::fflush(impl->output))invalid("cannot flush completed recording");
        FILE* output=impl->output;impl->output=nullptr;
        if(std::fclose(output))invalid("cannot close completed recording");
    }
    impl->finalFrame=finalFrame;impl->finished=true;
}
