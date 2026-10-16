#include "replay_clock.h"
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#include <unistd.h>

namespace {
unsigned checks=0;
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
template<class F>void rejects(F f,const char* reason){bool caught=false;try{f();}catch(const std::exception&){caught=true;}check(caught,reason);}
std::vector<uint8_t> read(const std::filesystem::path& path){std::ifstream in(path,std::ios::binary);return {(std::istreambuf_iterator<char>(in)),{}};}
void write(const std::filesystem::path& path,const std::vector<uint8_t>& bytes){std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());if(!out)throw std::runtime_error("fixture write failed");}
void put(std::vector<uint8_t>& bytes,size_t at,uint64_t value,unsigned count){for(unsigned i=0;i<count;++i)bytes.at(at+i)=uint8_t(value>>(i*8));}
void repair(std::vector<uint8_t>& bytes,size_t at,size_t size){uint32_t h=2166136261u;for(size_t i=0;i<size;++i){h^=bytes.at(at+i);h*=16777619u;}put(bytes,at+size,h,4);}
}
int main(){
    const auto directory=std::filesystem::temp_directory_path()/("night100-clock-test-"+std::to_string(getpid()));
    try {
        check(std::filesystem::create_directory(directory),"clock test directory must be isolated");
        using InputReplay::Mode;const auto path=directory/"clock.bin",other=directory/"bad.bin";
        constexpr uint64_t first=0x0102030405060708ull;
        {
            ReplayClock record(Mode::Record,path);
            check(record.sample(0,first)==first,"record must return actual clock tick");
            check(record.sample(0,first)==first,"same-frame identical timestamps must be valid");
            record.completed(1);record.sample(1,first+11);record.sample(3,first+101);record.completed(4);record.finish(4);record.finish(4);
            rejects([&]{record.finish(5);},"finish cannot silently change final frame");
            rejects([&]{record.sample(4,first+102);},"sample after finish must reject");
        }
        const auto original=read(path);
        check(original.size()==20+5*21,"binary format must contain header, four samples and footer");
        check(original[29]==8 && original[30]==7 && original[36]==1,"clock ticks must use explicit little-endian encoding");
        rejects([&]{ReplayClock duplicate(Mode::Record,path);},"recording must exclusively create its file");
        check(read(path)==original,"exclusive-create failure must preserve existing recording");
        {
            ReplayClock replay(Mode::Replay,path);
            check(replay.sample(0,999)==first && replay.sample(0,0)==first,"replay must ignore host clock jitter and preserve repeated calls");
            replay.completed(1);check(replay.sample(1,UINT64_MAX)==first+11,"next frame must receive exact recorded delta");
            check(replay.sample(3,1)==first+101,"frames without clock calls may be skipped");replay.completed(4);replay.finish(4);replay.finish(4);
        }
        {
            ReplayClock replay(Mode::Replay,path);rejects([&]{replay.sample(1,0);},"unexpected call frame must reject without consumption");
            check(replay.sample(0,0)==first,"failed frame check must preserve event cursor");
            rejects([&]{replay.finish(4);},"unconsumed clock calls must reject at end");
        }
        {
            ReplayClock replay(Mode::Replay,path);replay.sample(0,0);replay.sample(0,0);replay.sample(1,0);replay.sample(3,0);
            rejects([&]{replay.sample(3,0);},"extra clock call must reject");
            rejects([&]{replay.finish(5);},"wrong final frame must reject");replay.finish(4);
        }
        const auto decreasing=directory/"decreasing.bin";
        {
            ReplayClock record(Mode::Record,decreasing);record.sample(2,100);
            rejects([&]{record.sample(2,99);},"recording must reject decreasing timestamps");
            rejects([&]{record.sample(1,101);},"recording must reject decreasing frames");
            rejects([&]{record.finish(1);},"final frame cannot precede sample");
            record.completed(3);rejects([&]{record.sample(2,101);},"clock cannot run before completed frame boundary");
            rejects([&]{record.completed(2);},"completed frame cannot go backward");record.finish(3);
        }
        {
            ReplayClock replay(Mode::Replay,decreasing);check(replay.sample(2,7)==100,"rejected recorder calls must not enter file");replay.finish(3);
        }
        const auto empty=directory/"empty.bin";{ReplayClock record(Mode::Record,empty);record.finish(0);}
        {ReplayClock replay(Mode::Replay,empty);replay.finish(0);}
        check(read(empty).size()==41,"zero-call clip must still have a verified footer");
        const auto interrupted=directory/"interrupted.bin";{ReplayClock record(Mode::Record,interrupted);record.sample(0,1);}
        rejects([&]{ReplayClock replay(Mode::Replay,interrupted);},"unfinished clock trace cannot be replayed as complete");
        auto bad=original;bad[8]=2;repair(bad,0,16);write(other,bad);
        rejects([&]{ReplayClock replay(Mode::Replay,other);},"unsupported version must reject even with valid checksum");
        bad=original;bad[0]^=1;write(other,bad);rejects([&]{ReplayClock replay(Mode::Replay,other);},"header corruption must reject");
        bad=original;bad[29]^=1;write(other,bad);rejects([&]{ReplayClock replay(Mode::Replay,other);},"sample checksum corruption must reject");
        bad=original;bad.back()^=1;write(other,bad);rejects([&]{ReplayClock replay(Mode::Replay,other);},"footer checksum corruption must reject");
        bad=original;bad.pop_back();write(other,bad);rejects([&]{ReplayClock replay(Mode::Replay,other);},"partial footer must reject");
        bad=original;bad.resize(bad.size()-21);write(other,bad);rejects([&]{ReplayClock replay(Mode::Replay,other);},"missing footer must reject");
        bad=original;put(bad,20+21+9,first-1,8);repair(bad,20+21,17);write(other,bad);
        rejects([&]{ReplayClock replay(Mode::Replay,other);},"decreasing ticks must reject independently of checksums");
        bad=original;put(bad,20+3*21+1,0,8);repair(bad,20+3*21,17);write(other,bad);
        rejects([&]{ReplayClock replay(Mode::Replay,other);},"decreasing frame must reject independently of checksums");
        bad=original;put(bad,20+4*21+9,99,8);repair(bad,20+4*21,17);write(other,bad);
        rejects([&]{ReplayClock replay(Mode::Replay,other);},"footer sample count mismatch must reject");
        bad=original;bad[20]=17;repair(bad,20,17);write(other,bad);
        rejects([&]{ReplayClock replay(Mode::Replay,other);},"unknown record kind must reject");
        bad=original;bad.insert(bad.end(),original.begin()+20,original.begin()+41);write(other,bad);
        rejects([&]{ReplayClock replay(Mode::Replay,other);},"events after footer must reject");
        bad=original;put(bad,21,UINT64_MAX,8);repair(bad,20,17);write(other,bad);
        rejects([&]{ReplayClock replay(Mode::Replay,other);},"unbounded frame index must reject");
        rejects([&]{ReplayClock replay(Mode::Replay,directory/"missing.bin");},"missing trace must reject");
        rejects([&]{ReplayClock record(Mode::Record,{});},"empty path must reject");
        std::filesystem::remove_all(directory);std::cout<<"Replay clock checks: "<<checks<<" passed\n";return 0;
    }catch(const std::exception& error){std::filesystem::remove_all(directory);std::cerr<<error.what()<<'\n';return 1;}
}
