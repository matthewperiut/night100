#include "emulator/core/iop_memory.h"
#include "emulator/services/iop_module_loader.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using ps2x::iop::detail::IopMemory;
using ps2x::iop::detail::IopModuleLoader;
static std::vector<uint8_t> moduleImage(uint32_t bytes, uint8_t marker) {
    std::vector<uint8_t> image(0x200, marker);
    std::fill(image.begin(),image.begin()+0x100,0);
    const auto word=[&](size_t offset,uint32_t value){std::memcpy(image.data()+offset,&value,4);};
    const auto half=[&](size_t offset,uint16_t value){std::memcpy(image.data()+offset,&value,2);};
    word(0,0x464c457f);image[4]=image[5]=image[6]=1;
    half(16,0xff80);half(18,8);word(20,1);word(28,52);
    half(40,52);half(42,32);half(44,1);
    word(52,1);word(56,0x100);word(68,0x100);word(72,bytes);word(76,7);word(80,0x100);
    return image;
}
int main() {
    IopMemory memory;
    const auto check=[](bool value,const char* why){if(!value)throw std::runtime_error(why);};
    const uint32_t capacity=IopMemory::HeapLimit-IopMemory::HeapBase;
    const auto a=memory.allocate(0x20000),b=memory.allocate(0x30000),c=memory.allocate(0x40000);
    const auto tail=capacity>0xd0000?memory.allocate(capacity-0xd0000):0;
    check(a && b && c,"initial allocations failed");
    check(memory.freeAllocation(b),"free failed");
    check(memory.maxFreeMemory()==0x40000,"largest free extent must include holes and tail");
    check(memory.totalFreeMemory()==0xd0000-0x60000,"total free space must include holes");
    const auto d=memory.allocate(0x30000);
    check(d==b,"allocation did not reuse the released hole");
    check(!memory.allocate(0x60000),"fragmented heap accepted an oversized extent");
    check(!memory.allocate(0xfffffff0),"allocation size overflow accepted");
    check(!memory.allocate(16,24),"non-power-of-two alignment accepted");
    check(!memory.allocate(32,16,0xfffffff0),"fixed allocation wrapped around RAM");
    check(memory.freeAllocation(a) && memory.freeAllocation(c) && memory.freeAllocation(d),"release failed");
    if(tail)check(memory.freeAllocation(tail),"tail release failed");
    // The game's repeatedly loaded 714 KiB VAG bank used to exhaust the
    // monotonic allocator after one load/free cycle.
    for(unsigned i=0;i<100;++i) {
        const auto p=memory.allocate(731136);
        check(p==IopMemory::HeapBase,"repeated sound-bank load exhausted the heap");
        check(memory.freeAllocation(p),"sound-bank release failed");
    }
    check(memory.maxFreeMemory()==capacity && memory.totalFreeMemory()==capacity,"heap did not fully recover");

    // Module images, including zero-filled BSS, and thread stacks occupy the
    // same tracked physical memory as guest bank reads. The previous static
    // split left less than 832KiB for a real 1,062,912-byte manor bank.
    const auto first=IopModuleLoader::load(moduleImage(0x20000,0x5a),memory,0x10000);
    check(bool(first),"first IRX image failed to load");
    const auto stack=memory.allocate(0x4040);
    check(stack!=0,"thread stack allocation failed");
    memory.write32(stack,0x12345678);
    const auto second=IopModuleLoader::load(moduleImage(0x28000,0xa5),memory,first.nextModuleCursor);
    check(bool(second),"IRX load failed beside live thread stack");
    check(memory.read32(stack)==0x12345678,"IRX load overwrote thread stack");
    check(memory.read8(first.base)==0x5a && memory.read8(second.base)==0xa5,"IRX payload changed");
    check(memory.read32(first.base+0x1fffc)==0 && memory.read32(second.base+0x27ffc)==0,"IRX BSS was not initialized");
    constexpr uint32_t bankBytes=1062912;
    const auto bank=memory.allocate(bankBytes);
    check(bank!=0,"physical IOP RAM could not hold real manor sound bank");
    std::vector<uint8_t> pcm(bankBytes,0x37);
    check(memory.writeRam(bank,pcm.data(),pcm.size()),"large bank write failed");
    const auto third=IopModuleLoader::load(moduleImage(0x6000,0xc3),memory,second.nextModuleCursor);
    check(bool(third),"later IRX failed to use remaining real free RAM");
    check(memory.read8(bank)==0x37 && memory.read8(bank+bankBytes-1)==0x37,"later IRX overwrote sound bank");
    check(memory.read32(stack)==0x12345678 && memory.read8(first.base)==0x5a && memory.read8(second.base)==0xa5,"bank upload overwrote live IOP resources");
    check(bank+bankBytes<=IopMemory::HeapLimit && third.base+third.size<=IopMemory::HeapLimit,"allocation entered callback stack area");
    const auto freeBeforeBadLoad=memory.totalFreeMemory();
    auto malformed=moduleImage(0x1000,0);
    const uint32_t badFileSize=0x1001;std::memcpy(malformed.data()+68,&badFileSize,4);
    check(!IopModuleLoader::load(malformed,memory,third.nextModuleCursor),"malformed IRX was accepted");
    check(memory.totalFreeMemory()==freeBeforeBadLoad,"failed IRX leaked RAM reservation");
    check(memory.freeAllocation(first.base) && memory.freeAllocation(second.base) && memory.freeAllocation(third.base) && memory.freeAllocation(stack) && memory.freeAllocation(bank),"shared arena release failed");
    check(memory.maxFreeMemory()==capacity && memory.totalFreeMemory()==capacity,"shared arena did not recover");
    std::cout<<"PASS: fragmented/reused IOP heap, real manor bank size, interleaved IRX/BSS/thread/bank reservations and malformed-image rollback\n";
}
