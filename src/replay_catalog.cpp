#include "replay_catalog.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace ReplayCatalog {
namespace {
[[noreturn]] void invalid(const std::string& reason){throw std::runtime_error("Replay catalog: "+reason);}
uint32_t checksum(const std::string& value){uint32_t h=2166136261u;for(unsigned char c:value){h^=c;h*=16777619u;}return h;}
uint64_t number(const std::filesystem::path& path) {
    const auto name=path.filename().string();
    if(!name.starts_with("session-"))return 0;
    uint64_t id=0;const auto* first=name.data()+8;const auto* last=name.data()+name.size();
    auto result=std::from_chars(first,last,id);
    return result.ec==std::errc{} && result.ptr==last?id:0;
}
template<size_t N>bool finite(const std::array<float,N>& values){return std::all_of(values.begin(),values.end(),[](float f){return std::isfinite(f);});}
void validate(const Checkpoint& c) {
    if(!finite(c.position) || !finite(c.camera))invalid("nonfinite checkpoint");
}
void writeCheckpoint(std::ostream& out,const char* name,const Checkpoint& c) {
    out<<name<<' '<<c.context.mode<<' '<<c.context.state<<' '<<c.context.ostrich<<' '<<c.context.scene
       <<' '<<c.playerValid<<' '<<c.health<<' '<<c.snacks<<' '<<c.keys<<' '<<c.control;
    for(float f:c.position)out<<' '<<f;
    out<<' '<<c.cameraValid;for(float f:c.camera)out<<' '<<f;out<<'\n';
}
void readCheckpoint(std::istream& in,const char* name,Checkpoint& c) {
    std::string label;unsigned player=0,camera=0;
    if(!(in>>label) || label!=name || !(in>>c.context.mode>>c.context.state>>c.context.ostrich>>c.context.scene
        >>player>>c.health>>c.snacks>>c.keys>>c.control) || player>1)invalid("invalid checkpoint header");
    for(auto& f:c.position)if(!(in>>f))invalid("invalid player position");
    if(!(in>>camera) || camera>1)invalid("invalid camera availability");
    for(auto& f:c.camera)if(!(in>>f))invalid("invalid camera matrix");
    c.playerValid=player;c.cameraValid=camera;validate(c);
}
void validate(const Entry& e,const Metadata& m) {
    if(!e.id || e.id!=m.id || number(e.directory)!=e.id)invalid("session identity mismatch");
    if(m.elf!=InputReplay::elfSha256)invalid("ELF identity mismatch");
    if(m.endFrame<m.startFrame || m.options.size()>16384 || m.options.find_first_of("\r\n")!=std::string::npos)
        invalid("invalid frame range or startup options");
    validate(m.start);validate(m.end);
}
}
Entry reserve(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    uint64_t largest=0;
    for(const auto& item:std::filesystem::directory_iterator(root))largest=std::max(largest,number(item.path()));
    for(;;) {
        if(largest==std::numeric_limits<uint64_t>::max())invalid("session numbers exhausted");
        ++largest;std::ostringstream name;name<<"session-"<<std::setw(6)<<std::setfill('0')<<largest;
        const auto directory=root/name.str();std::error_code error;
        if(std::filesystem::create_directory(directory,error))return {largest,directory};
        if(error && error!=std::errc::file_exists)invalid("cannot reserve session: "+error.message());
    }
}
void save(const Entry& entry,const Metadata& metadata) {
    validate(entry,metadata);
    std::ostringstream out;out<<std::setprecision(std::numeric_limits<float>::max_digits10);
    out<<"NIGHT100_SESSION 1 "<<metadata.id<<' '<<std::quoted(metadata.elf)<<' '<<std::quoted(metadata.options)
       <<' '<<metadata.startFrame<<' '<<metadata.endFrame<<' '<<metadata.complete<<'\n';
    writeCheckpoint(out,"START",metadata.start);writeCheckpoint(out,"END",metadata.end);
    const auto payload=out.str();out<<"CHECKSUM "<<std::hex<<checksum(payload)<<'\n';
    const auto temporary=entry.directory/"metadata.tmp";
    // Only the owning game thread updates a reserved entry. A temporary file
    // keeps interrupted completion writes from replacing valid metadata.
    {
        std::ofstream file(temporary,std::ios::binary|std::ios::trunc);file<<out.str();file.close();
        if(!file)invalid("cannot write session metadata");
    }
#if defined(__SWITCH__)
    const auto destination=entry.directory/"metadata.txt",previous=entry.directory/"metadata.previous";
    // FAT/libnx rename never replaces an existing file. Preserve the preceding
    // valid generation across the two renames, including an interrupted one.
    const bool existed=std::filesystem::exists(destination);
    if(existed) {
        // A previous generation is expendable only while the current one is
        // still in place; load() falls back to it if publication is interrupted.
        if(std::filesystem::exists(previous))std::filesystem::remove(previous);
        std::filesystem::rename(destination,previous);
    }
    try {std::filesystem::rename(temporary,destination);}
    catch(...) {
        if(existed && !std::filesystem::exists(destination)) {
            std::error_code rollback;std::filesystem::rename(previous,destination,rollback);
        }
        throw;
    }
#else
    std::filesystem::rename(temporary,entry.directory/"metadata.txt");
#endif
}
Metadata load(const Entry& entry) {
    auto path=entry.directory/"metadata.txt";
#if defined(__SWITCH__)
    if(!std::filesystem::exists(path) && std::filesystem::exists(entry.directory/"metadata.previous"))
        path=entry.directory/"metadata.previous";
#endif
    std::error_code error;
    const auto size=std::filesystem::file_size(path,error);
    if(error || size>65536)invalid("missing or oversized metadata");
    std::ifstream file(path,std::ios::binary);std::string data((std::istreambuf_iterator<char>(file)),{});
    const auto offset=data.rfind("CHECKSUM ");
    if(offset==std::string::npos)invalid("missing metadata checksum");
    const auto payload=data.substr(0,offset);std::istringstream digest(data.substr(offset+9));uint32_t expected=0;
    if(!(digest>>std::hex>>expected) || checksum(payload)!=expected)invalid("metadata checksum mismatch");
    digest>>std::ws;if(!digest.eof())invalid("trailing checksum data");
    std::istringstream in(payload);std::string magic;unsigned version=0,complete=0;Metadata m;
    if(!(in>>magic>>version>>m.id>>std::quoted(m.elf)>>std::quoted(m.options)>>m.startFrame>>m.endFrame>>complete)
        || magic!="NIGHT100_SESSION" || version!=1 || complete>1)invalid("unsupported session metadata");
    m.complete=complete;readCheckpoint(in,"START",m.start);readCheckpoint(in,"END",m.end);
    in>>std::ws;if(!in.eof())invalid("trailing metadata data");validate(entry,m);return m;
}
std::vector<Entry> list(const std::filesystem::path& root) {
    std::vector<Entry> result;std::error_code error;if(!std::filesystem::exists(root,error))return result;
    for(const auto& item:std::filesystem::directory_iterator(root)) {
        Entry entry{number(item.path()),item.path()};if(!entry.id || !item.is_directory())continue;
        try {
            if(load(entry).complete && std::filesystem::is_regular_file(entry.directory/"input.inputs") && std::filesystem::is_regular_file(entry.directory/"clock.bin") &&
                std::filesystem::is_regular_file(entry.directory/"start-state.txt") &&
                std::filesystem::is_regular_file(entry.directory/"state.bin") &&
                std::filesystem::is_regular_file(entry.directory/"end-state.bin"))result.push_back(std::move(entry));
        }catch(const std::exception&){} // Incomplete/corrupt captures remain on disk for diagnosis.
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.id<b.id;});return result;
}
Checkpoint capture(const uint8_t* ram,size_t size) {
    if(!ram || size<0x363b68)invalid("guest RAM is unavailable");
    const auto valid=[&](uint32_t p,size_t n){return p>=0x1000 && n<=size && p<=size-n;};
    const auto word=[&](uint32_t p){uint32_t value;std::memcpy(&value,ram+p,4);return value;};
    Checkpoint c;const auto scene=word(0x363b64);
    c.context={word(0x357e20),word(0x3574f4),word(0x357e24),valid(scene,0x70)?word(scene+0x6c):0};
    const uint32_t frame=word(0x363238);
    if(valid(frame,0xf0)) {
        std::memcpy(c.position.data(),ram+frame+0x30,12);c.playerValid=finite(c.position);
        if(!c.playerValid)c.position={};
    }
    // Retail DWARF: zGlobals.player at +0x228; zPlayerGlobals Health +0xf0,
    // Inv_Key +0x3b4, Inv_ScoobySnacks +0x3bc, ControlOn +0x1b8.
    c.health=word(0x3632e8);c.keys=word(0x3635ac);c.snacks=word(0x3635b4);c.control=word(0x3633b0);
    const uint32_t camera=word(0x2b5ad0);
    if(valid(camera,0x60)) {
        for(unsigned column=0;column<4;++column)std::memcpy(c.camera.data()+column*3,ram+camera+0x20+column*16,12);
        c.cameraValid=finite(c.camera);if(!c.cameraValid)c.camera={};
    }
    return c;
}
Comparison compare(const Checkpoint& expected,const Checkpoint& actual,double positionTolerance,double cameraTolerance) {
    validate(expected);validate(actual);
    if(!std::isfinite(positionTolerance) || positionTolerance<0 || !std::isfinite(cameraTolerance) || cameraTolerance<0)
        invalid("invalid comparison tolerance");
    Comparison result{true,{}};std::ostringstream out;out<<std::setprecision(10);
    out<<"{\"version\":1,\"position_tolerance\":"<<positionTolerance<<",\"camera_tolerance\":"<<cameraTolerance<<",\"fields\":{";
    bool first=true;
    const auto field=[&](const std::string& name,double a,double b,double tolerance) {
        const double error=std::abs(a-b);const bool matches=error<=tolerance;result.matches&=matches;
        if(!first)out<<',';first=false;out<<'"'<<name<<"\":{\"expected\":"<<a<<",\"actual\":"<<b
            <<",\"error\":"<<error<<",\"tolerance\":"<<tolerance<<",\"matches\":"<<(matches?"true":"false")<<'}';
    };
    field("mode",expected.context.mode,actual.context.mode,0);field("state",expected.context.state,actual.context.state,0);
    field("ostrich",expected.context.ostrich,actual.context.ostrich,0);field("scene",expected.context.scene,actual.context.scene,0);
    field("player_valid",expected.playerValid,actual.playerValid,0);
    if(expected.playerValid && actual.playerValid) {
        double squared=0;for(unsigned i=0;i<3;++i){const double d=double(actual.position[i])-expected.position[i];squared+=d*d;
            field("position_"+std::to_string(i),expected.position[i],actual.position[i],positionTolerance);}
        field("position_distance",0,std::sqrt(squared),positionTolerance);
        field("health",expected.health,actual.health,0);field("snacks",expected.snacks,actual.snacks,0);
        field("keys",expected.keys,actual.keys,0);field("control",expected.control,actual.control,0);
    }
    field("camera_valid",expected.cameraValid,actual.cameraValid,0);
    if(expected.cameraValid && actual.cameraValid)for(unsigned i=0;i<12;++i)
        field("camera_"+std::to_string(i),expected.camera[i],actual.camera[i],cameraTolerance);
    out<<"},\"matches\":"<<(result.matches?"true":"false")<<'}';result.json=out.str();return result;
}
}
