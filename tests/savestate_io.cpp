#include "runtime/ps2_vfs.h"
#include "runtime/ps2_rom_device.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_pcm_ring.h"
#include "runtime/state_archive.h"
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace {
unsigned checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F function,const char* message){bool caught=false;try{function();}catch(const std::exception&){caught=true;}check(caught,message);}
}
int main(){
    const auto directory=std::filesystem::temp_directory_path()/("night100-state-io-"+std::to_string(getpid()));
    try {
        check(std::filesystem::create_directory(directory),"IO fixture directory must be isolated");
        {std::ofstream f(directory/"source.bin",std::ios::binary);f<<"abcdefghijklmnopqrstuvwxyz";}
        PS2VfsMounts mounts{directory,directory,directory};PS2RomDevice rom;PS2Vfs source;
        const auto fd=source.open("host0:source.bin",PS2_FIO_O_RDONLY,mounts,rom);
        check(fd>=3,"read-only source descriptor must open");
        std::array<char,5> first{};check(source.read(fd,first.data(),first.size())==5 && std::string(first.data(),5)=="abcde","source must advance file offset");
        const auto romFd=source.open("rom0:ROMVER",PS2_FIO_O_RDONLY,mounts,rom);check(romFd>=3,"ROM file must open");
        check(source.seek(romFd,3,SEEK_SET)==3,"ROM offset must advance");
        const auto state=source.saveState();check(source.close(fd)==0 && source.close(romFd)==0,"original handles must close");
        PS2Vfs loaded;loaded.loadState(state);
        std::array<char,4> next{};check(loaded.read(fd,next.data(),4)==4 && std::string(next.data(),4)=="fghi","fresh VFS must reopen file under original descriptor and offset");
        check(loaded.seek(romFd,0,SEEK_CUR)==3,"fresh ROM descriptor must preserve independent offset");
        const auto extra=loaded.open("host0:source.bin",PS2_FIO_O_RDONLY,mounts,rom);check(extra>romFd,"restored next descriptor ID must not collide");
        const auto writable=source.open("host0:source.bin",PS2_FIO_O_RDWR,mounts,rom);check(writable>=3,"writable fixture must open");
        rejects([&]{(void)source.saveState();},"active writable transaction must reject capture explicitly");check(source.close(writable)==0,"writable transaction must close");
        {std::ofstream f(directory/"source.bin",std::ios::app);f<<"changed";}
        rejects([&]{loaded.loadState(state);},"changed external source size must reject restore");
        check(loaded.seek(romFd,0,SEEK_CUR)==3,"failed VFS restore must preserve current live descriptors");
        auto truncated=state;truncated.resize(3);rejects([&]{loaded.loadState(truncated);},"truncated VFS state must fail");

        PS2PcmRing ring;std::array<int16_t,24> samples{};for(unsigned i=0;i<samples.size();++i)samples[i]=int16_t(i*79-500);
        check(ring.write(samples.data(),12)==12,"PCM source queue must fill");std::array<int16_t,8> consumed{};check(ring.read(consumed.data(),4)==4,"PCM queue must consume prefix");
        ps2x::state::Writer bytes;bytes(ring);std::array<int16_t,16> expected{};check(ring.read(expected.data(),8)==8,"PCM queue must retain tail");
        PS2PcmRing restored;ps2x::state::Reader reader(bytes.data);reader(restored);reader.finish();std::array<int16_t,16> actual{};
        check(restored.queuedFrames()==8 && restored.read(actual.data(),8)==8 && actual==expected,"restored PCM queue must resume exact unconsumed samples and cursors");
        check(restored.queuedFrames()==0,"restored PCM consumption must drain normally");
        std::filesystem::remove_all(directory);std::cout<<"Savestate IO checks: "<<checks<<" passed\n";return 0;
    }catch(const std::exception& error){std::filesystem::remove_all(directory);std::cerr<<error.what()<<'\n';return 1;}
}
