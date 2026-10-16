#include "runtime/state_archive.h"
#include "emulator/core/iop_memory.h"
#include "emulator/core/iop_kernel.h"
#include "../vendor/PS2Recomp/ps2xIOP/tests/iop_compat_test_support.h"
#include <fstream>
#include <unistd.h>

namespace {
unsigned checks=0;
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
template<class F>void rejects(F function,const char* reason){bool caught=false;try{function();}catch(const std::exception&){caught=true;}check(caught,reason);}
using ps2x::iop::detail::IopMemory;
using ps2x::iop::detail::IopKernel;
using ps2x::iop::detail::IopCpuState;
void spuAddress(IopMemory& m,uint32_t reg,uint32_t value){m.write16(IopMemory::Spu2Base+reg,uint16_t(value>>16));m.write16(IopMemory::Spu2Base+reg+2,uint16_t(value));}
}
int main(){try {
    using namespace ps2x::iop;
    iop_test::Host host;IopSubsystem original(host);
    constexpr uint32_t sid=0x91ab4012,reply=0x8d551a73;
    auto image=iop_test::rpcServer(sid,reply);image.install(host);
    const auto module=original.loadModuleBuffer(0x1000);
    check(module.moduleId>0 && original.canBindRpc(sid),"synthetic physical IRX/RPC must load");
    check(original.loadModule("rom0:LIBSD").moduleId>0,"HLE module must load");
    const auto allocation=original.allocateMemory(64,16);check(allocation!=0,"IOP allocation must succeed");
    const uint32_t marker=0x71aa92de;check(original.writeMemory(allocation,&marker,4),"IOP allocation must be writable");
    original.runEeCycles(294912);
    const auto bytes=original.saveState();
    const auto path=std::filesystem::temp_directory_path()/("night100-iop-state-"+std::to_string(getpid())+".bin");
    {std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
    std::ifstream f(path,std::ios::binary);std::vector<uint8_t> persisted((std::istreambuf_iterator<char>(f)),{});f.close();std::filesystem::remove(path);
    original.reset();check(!original.canBindRpc(sid),"reset must remove physical RPC server");
    // A fresh host/subsystem must reconstruct all guest addresses and routes.
    iop_test::Host restoredHost;IopSubsystem restored(restoredHost);restored.loadState(persisted);
    check(restored.canBindRpc(sid),"restored physical RPC server must bind without loading original IRX");
    const auto response=restored.handleRpc(iop_test::request(sid,1));
    check(response.handled && response.resultAddress==0x800,"restored RPC must execute original IOP machine code");
    uint32_t value=0;check(restoredHost.word(response.resultAddress)==reply,"restored RPC must copy its reply into fresh EE host RAM");
    check(restored.readMemory(allocation,&value,4) && value==marker,"IOP allocation bytes must survive");
    check(restored.freeMemory(allocation),"restored allocator metadata must release original address");
    check(restored.canBindRpc(0x80000701),"restored HLE module route must bind");
    auto wrongAbi=bytes;wrongAbi[sizeof(uint32_t)+sizeof(uint64_t)]^=1;
    const auto protectedAllocation=restored.allocateMemory(32,16);check(restored.writeMemory(protectedAllocation,&marker,4),"ABI preflight marker must be writable");
    rejects([&]{restored.loadState(wrongAbi);},"incompatible raw POD ABI must fail before restoration");
    check(restored.canBindRpc(sid) && restored.readMemory(protectedAllocation,&value,4) && value==marker,"ABI rejection must preserve existing RPC routes and guest memory");
    auto truncated=bytes;truncated.resize(bytes.size()/2);
    iop_test::Host badHost;IopSubsystem bad(badHost);rejects([&]{bad.loadState(truncated);},"truncated IOP state must fail");

    IopMemory memory;IopKernel kernel(memory);IopCpuState cpu{};
    constexpr uint32_t descriptor=0x10000;
    memory.write32(descriptor+8,0x18000);memory.write32(descriptor+12,512);memory.write32(descriptor+16,30);
    cpu.gpr[4]=descriptor;check(kernel.dispatchThreadImport(4,cpu,0) && cpu.gpr[2]>0,"kernel thread creation must succeed");
    const int threadId=cpu.gpr[2];cpu.gpr[4]=threadId;cpu.gpr[5]=0x7654;
    check(kernel.dispatchThreadImport(6,cpu,0),"kernel thread must start");
    auto* thread=kernel.beginNextReady(0);check(thread && thread->id==threadId,"started thread must schedule");
    thread->cpu.gpr[17]=0x12345678;thread->cpu.cop0[12]=0x11220011;thread->cpu.pendingLoad=true;thread->cpu.pendingLoadReg=9;thread->cpu.pendingLoadValue=0x54432211;
    kernel.delayCurrentUntil(500,thread->cpu);kernel.endTimeslice(*thread,0xdeadbeef);
    ps2x::state::Writer kernelBytes;kernelBytes(memory,kernel);
    IopMemory newMemory;IopKernel newKernel(newMemory);ps2x::state::Reader kernelState(kernelBytes.data);kernelState(newMemory,newKernel);kernelState.finish();
    check(newKernel.nextWakeCycle(1000)==500 && !newKernel.beginNextReady(499),"delayed thread must retain exact wake cycle");
    thread=newKernel.beginNextReady(500);
    check(thread && thread->id==threadId && thread->cpu.gpr[17]==0x12345678 && thread->cpu.cop0[12]==0x11220011 && thread->cpu.pendingLoad && thread->cpu.pendingLoadValue==0x54432211,"thread CPU/pending load context must resume exactly");
    rejects([&]{ps2x::state::Writer active;active(newKernel);},"running IOP thread must explicitly reject unsafe capture");
    newKernel.endTimeslice(*thread,0xdeadbeef);

    // Save a sounding voice inside its ADPCM block and attack envelope. Exact
    // subsequent PCM proves predictor/phase/fraction/clock/queue restoration.
    IopMemory sound;
    std::array<uint8_t,16> block{};block.fill(0x57);block[0]=0x21;block[1]=7;
    check(sound.writeRam(0x10000,block.data(),block.size()),"SPU DMA source must exist");
    spuAddress(sound,0x1a8,0x2000);sound.write32(0x1f8010c0,0x10000);sound.write32(0x1f8010c4,0x10004);sound.write32(0x1f8010c8,0x1000201);
    sound.write16(IopMemory::Spu2Base,0x3fff);sound.write16(IopMemory::Spu2Base+2,0x3fff);sound.write16(IopMemory::Spu2Base+4,0x943);
    sound.write16(IopMemory::Spu2Base+6,0xff);sound.write16(IopMemory::Spu2Base+8,0x1fc0);
    sound.write32(IopMemory::Spu2Base+0x188,1);sound.write32(IopMemory::Spu2Base+0x190,1);
    sound.write16(IopMemory::Spu2Base+0x760,0x3fff);sound.write16(IopMemory::Spu2Base+0x762,0x3fff);
    spuAddress(sound,0x1c0,0x2000);sound.write16(IopMemory::Spu2Base+0x1a0,1);sound.advanceSpuTo(768*13+100);
    ps2x::state::Writer soundBytes;soundBytes(sound);sound.advanceSpuTo(768*97+211);
    const std::vector<int16_t> expected(sound.audioSamples().begin(),sound.audioSamples().end());
    IopMemory loadedSound;ps2x::state::Reader soundState(soundBytes.data);soundState(loadedSound);soundState.finish();loadedSound.advanceSpuTo(768*97+211);
    check(std::any_of(expected.begin(),expected.end(),[](int16_t x){return x!=0;}),"SPU fixture must contain actual audio");
    check(std::vector<int16_t>(loadedSound.audioSamples().begin(),loadedSound.audioSamples().end())==expected,"resumed active SPU voice must produce bit-identical PCM");
    check(loadedSound.read32(IopMemory::Spu2Base+0x1c8)==sound.read32(IopMemory::Spu2Base+0x1c8),"resumed SPU next-address register must agree");
    std::cout<<"Savestate IOP checks: "<<checks<<" passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
