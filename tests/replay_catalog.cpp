#include "replay_catalog.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unistd.h>

namespace {
unsigned checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F f,const char* message){bool caught=false;try{f();}catch(const std::exception&){caught=true;}check(caught,message);}
void write(const std::filesystem::path& path,const std::string& value){std::ofstream f(path,std::ios::binary);f<<value;if(!f)throw std::runtime_error("fixture write failed");}
std::string read(const std::filesystem::path& path){std::ifstream f(path,std::ios::binary);return {(std::istreambuf_iterator<char>(f)),{}};}
template<class T>void put(std::vector<uint8_t>& ram,uint32_t at,T value){std::memcpy(ram.data()+at,&value,sizeof(value));}
ReplayCatalog::Checkpoint checkpoint(){
    ReplayCatalog::Checkpoint c;c.context={11,2,2,0x49303031};c.playerValid=c.cameraValid=true;
    c.position={1.25f,-7.5f,100.1f};c.health=4;c.snacks=25;c.keys=1;c.control=1;
    for(unsigned i=0;i<c.camera.size();++i)c.camera[i]=float(i)*0.125f;return c;
}
}
int main(){
    const auto root=std::filesystem::temp_directory_path()/("night100-catalog-test-"+std::to_string(getpid()));
    try {
        check(!std::filesystem::exists(root),"test directory must be isolated");
        check(ReplayCatalog::list(root).empty(),"missing catalog must list empty");
        const auto first=ReplayCatalog::reserve(root),second=ReplayCatalog::reserve(root);
        check(first.id==1 && second.id==2 && first.directory!=second.directory,"reserving must never reuse incomplete directory");
        std::filesystem::create_directory(root/"session-000050");write(root/"session-000051","preserve me");
        const auto later=ReplayCatalog::reserve(root);
        check(later.id==52 && read(root/"session-000051")=="preserve me","allocation must preserve existing files and number gaps");
        ReplayCatalog::Metadata m;m.id=first.id;m.startFrame=100;m.endFrame=200;m.options="graphics=native renderer=gpu quoted=\"yes\" slash=\\";
        m.start=checkpoint();m.end=m.start;m.end.position[1]=2.375f;
        ReplayCatalog::save(first,m);auto parsed=ReplayCatalog::load(first);
        check(parsed.id==m.id && parsed.startFrame==100 && parsed.endFrame==200 && !parsed.complete && parsed.elf==m.elf && parsed.options==m.options,"metadata header round trip");
        check(parsed.start.context==m.start.context && parsed.end.position==m.end.position && parsed.end.camera==m.end.camera && parsed.end.keys==m.end.keys,"checkpoint float and gameplay field round trip");
        write(first.directory/"input.inputs","input fixture");write(first.directory/"start-state.txt","full snapshot fixture");
        write(first.directory/"clock.bin","clock fixture");
        write(first.directory/"state.bin","start snapshot fixture");write(first.directory/"end-state.bin","end snapshot fixture");
        check(ReplayCatalog::list(root).empty(),"incomplete metadata must stay hidden");
        m.complete=true;ReplayCatalog::save(first,m);
        auto listed=ReplayCatalog::list(root);check(listed.size()==1 && listed[0].id==1,"only complete session appears");
        std::filesystem::remove(first.directory/"state.bin");check(ReplayCatalog::list(root).empty(),"missing starting full snapshot must stay hidden");
        write(first.directory/"state.bin","start snapshot fixture");
        std::filesystem::remove(first.directory/"end-state.bin");check(ReplayCatalog::list(root).empty(),"missing ending full snapshot must stay hidden");
        write(first.directory/"end-state.bin","end snapshot fixture");
        std::filesystem::remove(first.directory/"start-state.txt");check(ReplayCatalog::list(root).empty(),"missing start checkpoint must stay hidden");
        write(first.directory/"start-state.txt","restoration mode");
#if defined(__SWITCH__)
        // An interrupted FAT update leaves the preceding valid generation in
        // metadata.previous; recovery must not turn an incomplete clip public.
        std::filesystem::rename(first.directory/"metadata.txt",first.directory/"metadata.current-test");
        check(!ReplayCatalog::load(first).complete && ReplayCatalog::list(root).empty(),"interrupted completion must recover the prior incomplete generation");
        std::filesystem::rename(first.directory/"metadata.current-test",first.directory/"metadata.txt");
        check(ReplayCatalog::load(first).complete,"successful current metadata must take precedence over an older generation");
        std::filesystem::remove(first.directory/"metadata.previous");
        std::filesystem::rename(first.directory/"metadata.txt",first.directory/"metadata.previous");
        check(ReplayCatalog::load(first).complete && ReplayCatalog::list(root).size()==1,"interrupted later update must recover a complete prior generation");
        ReplayCatalog::save(first,m);
        check(ReplayCatalog::load(first).complete,"recovered metadata must support subsequent FAT publication");
#endif
        const auto metadata=read(first.directory/"metadata.txt");
        auto corrupted=metadata;corrupted[corrupted.find("START")+6]='9';write(first.directory/"metadata.txt",corrupted);
        rejects([&]{ReplayCatalog::load(first);},"checksum corruption must be rejected");check(ReplayCatalog::list(root).empty(),"corrupt metadata must stay hidden");
        write(first.directory/"metadata.txt",metadata.substr(0,metadata.size()/2));rejects([&]{ReplayCatalog::load(first);},"truncated metadata must be rejected");
        write(first.directory/"metadata.txt",metadata);auto wrong=m;wrong.id=2;
        rejects([&]{ReplayCatalog::save(first,wrong);},"mismatched session ID must be rejected");
        wrong=m;wrong.elf="other";rejects([&]{ReplayCatalog::save(first,wrong);},"wrong ELF must be rejected");
        wrong=m;wrong.endFrame=99;rejects([&]{ReplayCatalog::save(first,wrong);},"reversed frame range must be rejected");
        wrong=m;wrong.end.position[0]=std::numeric_limits<float>::infinity();rejects([&]{ReplayCatalog::save(first,wrong);},"nonfinite stored position must be rejected");
        wrong=m;wrong.end.camera[0]=std::numeric_limits<float>::quiet_NaN();rejects([&]{ReplayCatalog::save(first,wrong);},"nonfinite stored camera must be rejected");
        wrong=m;wrong.options+="\nother";rejects([&]{ReplayCatalog::save(first,wrong);},"multiline startup options must be rejected");
        check(read(first.directory/"metadata.txt")==metadata,"invalid save must preserve valid metadata");
        auto c=checkpoint(),changed=c;
        check(ReplayCatalog::compare(c,changed).matches,"identical endpoint must match");
        changed.position[0]+=0.05f;check(ReplayCatalog::compare(c,changed).matches,"small numerical position drift must pass");
        changed=c;changed.position[0]+=0.08f;changed.position[1]+=0.08f;
        check(!ReplayCatalog::compare(c,changed).matches,"diagonal displacement must obey Euclidean tolerance");
        changed=c;changed.position[2]+=3;auto difference=ReplayCatalog::compare(c,changed);
        check(!difference.matches && difference.json.find("\"position_distance\"")!=std::string::npos && difference.json.find("\"position_tolerance\":0.1")!=std::string::npos && difference.json.find("\"matches\":false")!=std::string::npos,"far position report must include expected/actual/error/tolerance");
        changed=c;changed.context.scene^=1;check(!ReplayCatalog::compare(c,changed).matches,"wrong scene must fail");
        changed=c;changed.health--;check(!ReplayCatalog::compare(c,changed).matches,"wrong health must fail");
        changed=c;changed.snacks++;check(!ReplayCatalog::compare(c,changed).matches,"wrong inventory must fail");
        changed=c;changed.keys++;check(!ReplayCatalog::compare(c,changed).matches,"wrong key state must fail");
        changed=c;changed.control=0;check(!ReplayCatalog::compare(c,changed).matches,"wrong player-control state must fail");
        changed=c;changed.camera[11]+=0.01f;check(ReplayCatalog::compare(c,changed).matches,"small camera drift must pass");
        changed.camera[11]+=1;check(!ReplayCatalog::compare(c,changed).matches,"far camera matrix must fail");
        changed=c;changed.playerValid=false;check(!ReplayCatalog::compare(c,changed).matches,"missing expected player must fail");
        changed=c;changed.cameraValid=false;check(!ReplayCatalog::compare(c,changed).matches,"missing expected camera must fail");
        rejects([&]{ReplayCatalog::compare(c,c,-1);},"negative tolerance must be rejected");
        rejects([&]{ReplayCatalog::compare(c,c,0.1,std::numeric_limits<double>::quiet_NaN());},"nonfinite tolerance must be rejected");
        std::vector<uint8_t> ram(32*1024*1024);constexpr uint32_t scene=0x500000,frame=0x510000,camera=0x520000;
        put(ram,0x357e20,c.context.mode);put(ram,0x3574f4,c.context.state);put(ram,0x357e24,c.context.ostrich);
        put(ram,0x363b64,scene);put(ram,scene+0x6c,c.context.scene);put(ram,0x363238,frame);
        std::memcpy(ram.data()+frame+0x30,c.position.data(),12);
        put(ram,0x3632e8,c.health);put(ram,0x3635ac,c.keys);put(ram,0x3635b4,c.snacks);put(ram,0x3633b0,c.control);
        put(ram,0x2b5ad0,camera);
        for(unsigned column=0;column<4;++column){std::memcpy(ram.data()+camera+0x20+column*16,c.camera.data()+column*3,12);put(ram,camera+0x2c+column*16,0xffffffffu);}
        auto captured=ReplayCatalog::capture(ram.data(),ram.size());
        check(captured.playerValid && captured.cameraValid && ReplayCatalog::compare(c,captured,0,0).matches,"exact retail offsets and matrix padding lanes must decode correctly");
        // Heap addresses are deliberately omitted: identical state at relocated
        // objects must compare equal without RAM/pointer hashing.
        std::memcpy(ram.data()+frame+0x1000,ram.data()+frame,0xf0);put(ram,0x363238,frame+0x1000);
        std::memcpy(ram.data()+camera+0x1000,ram.data()+camera,0x60);put(ram,0x2b5ad0,camera+0x1000);
        check(ReplayCatalog::compare(captured,ReplayCatalog::capture(ram.data(),ram.size()),0,0).matches,"relocated heap objects must compare identically");
        put(ram,frame+0x1000+0x30,std::numeric_limits<float>::quiet_NaN());put(ram,camera+0x1000+0x20,std::numeric_limits<float>::infinity());
        captured=ReplayCatalog::capture(ram.data(),ram.size());check(!captured.playerValid && !captured.cameraValid,"invalid runtime matrices must be marked unavailable");
        put(ram,0x363b64,0xfffffff0u);put(ram,0x363238,0xfffffff0u);put(ram,0x2b5ad0,0xfffffff0u);
        captured=ReplayCatalog::capture(ram.data(),ram.size());check(!captured.context.scene && !captured.playerValid && !captured.cameraValid,"bad guest pointers must not escape RAM bounds");
        rejects([&]{ReplayCatalog::capture(nullptr,ram.size());},"null RAM must reject");
        rejects([&]{ReplayCatalog::capture(ram.data(),16);},"short RAM must reject");
        std::filesystem::remove_all(root);std::cout<<"Replay catalog checks: "<<checks<<" passed\n";return 0;
    }catch(const std::exception& e){std::filesystem::remove_all(root);std::cerr<<e.what()<<'\n';return 1;}
}
