#include "machine_snapshot.h"
#include "render_image.h"
#include "input_replay.h"
#include "ps2_runtime.h"
#include "native_video.h"
#include "runtime/state_archive.h"
#include <bit>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <zlib.h>
#include <fcntl.h>
#include <unistd.h>

namespace {
constexpr uint32_t schema=1;
constexpr uint64_t maxBytes=256ull*1024*1024;
std::vector<uint8_t> read(const std::filesystem::path& path) {
    const auto size=std::filesystem::file_size(path);
    if(size>maxBytes)throw std::runtime_error("Snapshot file is oversized");
    std::vector<uint8_t> bytes(size);std::ifstream in(path,std::ios::binary);
    in.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
    if(!in)throw std::runtime_error("Cannot read complete snapshot");return bytes;
}
void publish(const std::filesystem::path& path,const std::vector<uint8_t>& bytes) {
    const auto temporary=path.string()+".tmp-"+std::to_string(getpid());
    int fd=::open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
    if(fd<0)throw std::runtime_error("Cannot create snapshot: "+std::string(std::strerror(errno)));
    try {
        size_t offset=0;
        while(offset<bytes.size()) {
            const auto n=::write(fd,bytes.data()+offset,bytes.size()-offset);
            if(n<0 && errno==EINTR)continue;
            if(n<=0)throw std::runtime_error("Cannot write snapshot");offset+=size_t(n);
        }
        if(::fsync(fd))throw std::runtime_error("Cannot flush snapshot");
        const int closed=::close(fd);fd=-1;
        if(closed)throw std::runtime_error("Cannot close snapshot");
#if defined(__SWITCH__)
        // libnx fsdev rename maps to FsFileSystem::RenameFile, which fails if
        // the destination exists. FAT has no hard links. Staging stays on the
        // destination filesystem so readers see only the fully flushed file.
        if(::rename(temporary.c_str(),path.c_str()))throw std::runtime_error("Cannot publish snapshot without overwriting an existing file");
#else
        if(::link(temporary.c_str(),path.c_str()))throw std::runtime_error("Cannot publish snapshot without overwriting an existing file");
        ::unlink(temporary.c_str());
#endif
    } catch(...) {if(fd>=0)::close(fd);::unlink(temporary.c_str());throw;}
}
}
void saveMachineSnapshot(PS2Runtime& runtime,const std::filesystem::path& path) {
    if(NativeVideo::active())throw ps2x::state::NotReady("A native movie is playing");
    if(!nativeSceneCanSnapshot())throw ps2x::state::NotReady("Native graphics continuations are still active");
    // atSafePoint already owns a pause. Nesting here also protects callers
    // saving an initialized, unwound runtime outside its dispatch loop.
    runtime.pauseStateClocks();
    try {
    const auto clock=nativePortClockSnapshot();
    ps2x::state::Writer machine;
    // The render image's private bytes belong in the saved RAM (as before it).
    RenderImage::beforeSnapshotSave(runtime);
    try {runtime.saveMachineState(machine);}
    catch(...){RenderImage::afterSnapshotSave(runtime);throw;}
    RenderImage::afterSnapshotSave(runtime);
    const uint64_t size=machine.data.size();
    if(size>maxBytes)throw std::runtime_error("Snapshot payload is oversized");
    uLongf compressedSize=compressBound(size);std::vector<uint8_t> compressed(compressedSize);
    if(compress2(compressed.data(),&compressedSize,machine.data.data(),size,Z_BEST_SPEED)!=Z_OK)
        throw std::runtime_error("Cannot compress snapshot");
    compressed.resize(compressedSize);
    ps2x::state::Writer file;
    file(std::string("NIGHT100_MACHINE_STATE"),schema,std::string(InputReplay::elfSha256),uint32_t(sizeof(R5900Context)),
         uint32_t(std::endian::native==std::endian::little?0x12345678:0x78563412),clock,size,
         uint32_t(crc32(0,machine.data.data(),machine.data.size())),compressed);
    publish(path,file.data);
    // Saving blocks the game thread. Keep this development operation out of
    // subsequent gameplay delta time rather than creating a physics jump.
    nativePortClockRestore(clock);
    } catch(...) {runtime.resumeStateClocks();throw;}
    runtime.resumeStateClocks();
}
void loadMachineSnapshot(PS2Runtime& runtime,const std::filesystem::path& path) {
    const auto file=read(path);ps2x::state::Reader header(file);
    std::string magic,elf;uint32_t version=0,contextSize=0,endian=0,checksum=0;uint64_t clock=0,size=0;
    std::vector<uint8_t> compressed;
    header(magic,version,elf,contextSize,endian,clock,size,checksum,compressed);header.finish();
    if(magic!="NIGHT100_MACHINE_STATE" || version!=schema || elf!=InputReplay::elfSha256 ||
       contextSize!=sizeof(R5900Context) || endian!=(std::endian::native==std::endian::little?0x12345678u:0x78563412u) || size>maxBytes)
        throw std::runtime_error("Snapshot identity, schema or host ABI mismatch");
    std::vector<uint8_t> machine(size);uLongf decodedSize=size;
    if(uncompress(machine.data(),&decodedSize,compressed.data(),compressed.size())!=Z_OK || decodedSize!=size ||
       crc32(0,machine.data(),machine.size())!=checksum)throw std::runtime_error("Snapshot payload checksum or compression is invalid");
    ps2x::state::Reader archive(machine);runtime.loadMachineState(archive);archive.finish();
    resetNativeSceneAfterSnapshot();RenderImage::afterSnapshotLoad(runtime);nativePortClockRestore(clock);
}
