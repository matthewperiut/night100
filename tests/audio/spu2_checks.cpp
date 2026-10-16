#include "emulator/core/iop_memory.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <iostream>

using ps2x::iop::detail::IopMemory;
static void require(bool condition)
{
    if (!condition) { std::cerr << "SPU2 requirement failed\n"; std::abort(); }
}
constexpr uint32_t spu = IopMemory::Spu2Base;
static void address(IopMemory& memory, uint32_t reg, uint32_t word)
{
    memory.write16(spu + reg, uint16_t(word >> 16));
    memory.write16(spu + reg + 2, uint16_t(word));
}
static uint32_t address(IopMemory& memory, uint32_t reg)
{
    return (uint32_t(memory.read16(spu + reg)) << 16) | memory.read16(spu + reg + 2);
}
static void upload(IopMemory& memory, unsigned core, uint32_t destination, const std::array<uint8_t,16>& block)
{
    require(memory.writeRam(0x10000, block.data(), block.size()));
    address(memory, core * 0x400 + 0x1a8, destination);
    const uint32_t dma = core ? 0x1f801500u : 0x1f8010c0u;
    memory.write32(dma, 0x10000);
    memory.write32(dma + 4, 0x10004); // one block, four words
    memory.write32(dma + 8, 0x1000201);
    require(memory.takeDmaStart().has_value());
    require(address(memory, core * 0x400 + 0x1a8) == destination + 8);
    require((memory.read32(dma + 8) & (1u << 24)) == 0);
}
static void voice(IopMemory& memory, unsigned core, uint32_t start)
{
    const uint32_t base = core * 0x400;
    memory.write16(spu + base, 0x3fff);
    memory.write16(spu + base + 2, 0x3fff);
    memory.write16(spu + base + 4, 0x1000);
    memory.write16(spu + base + 6, 0xff);
    memory.write16(spu + base + 8, 0x1fc0);
    memory.write32(spu + base + 0x188, 1);
    memory.write32(spu + base + 0x190, 1);
    memory.write16(spu + 0x760 + core * 0x28, 0x3fff);
    memory.write16(spu + 0x762 + core * 0x28, 0x3fff);
    address(memory, base + 0x1c0, start);
    memory.write16(spu + base + 0x1a0, 1);
}
int main()
{
    IopMemory memory;
    std::array<uint8_t,16> block{};
    block.fill(0x11); block[0] = 0; block[1] = 1; // PCM4096, end without loop
    upload(memory, 0, 0x2000, block);
    // Explicit readback verifies sound RAM is copied rather than a fake DMA IRQ.
    address(memory, 0x1a8, 0x2000);
    memory.write32(0x1f8010c0, 0x11000);
    memory.write32(0x1f8010c4, 0x10004);
    memory.write32(0x1f8010c8, 0x1000200);
    std::array<uint8_t,16> back{};
    require(memory.readRam(0x11000, back.data(), back.size()));
    require(back == block);
    voice(memory, 0, 0x2000);
    require((memory.read32(spu + 0x340) & 1) == 0);
    memory.advanceSpuTo(768 * 10);
    require(memory.read16(spu + 0xa) > 0);
    require(address(memory, 0x1c8) > 0x2000);
    const auto first = memory.audioSamples();
    require(first.size() == 20);
    require(first[10] > 4000 && first[10] < 4100);
    require(first[10] == first[11]);
    memory.advanceSpuTo(768 * 30);
    require(memory.read16(spu + 0xa) == 0);
    require(memory.read32(spu + 0x340) & 1);
    memory.clearAudioSamples();

    // Loop-start/end continues decoding and sets ENDX without muting the voice.
    memory.reset(); block[1] = 7;
    upload(memory, 1, 0x12340, block);
    voice(memory, 1, 0x12340);
    memory.advanceSpuTo(768 * 100);
    require(memory.read16(spu + 0x40a) > 0);
    require(memory.read32(spu + 0x740) & 1);
    require(address(memory, 0x5c4) == 0x12340);
    require(address(memory, 0x5c8) >= 0x12340 && address(memory, 0x5c8) < 0x12348);
    memory.write16(spu + 0x744, 0); // status write cannot stop voice
    memory.write16(spu + 0x740, 1); // ENDX write-one-clear
    require((memory.read32(spu + 0x740) & 1) == 0);
    memory.write16(spu + 0x5a4, 1);
    memory.advanceSpuTo(768 * 110);
    require(memory.read16(spu + 0x40a) == 0);

    // Half pitch advances half as many source samples; delayed key-on still
    // produces the real attack envelope and not a synthetic readiness bit.
    memory.reset(); block[1] = 7;
    upload(memory, 0, 0x4000, block); voice(memory, 0, 0x4000);
    memory.write16(spu + 4, 0x800);
    memory.advanceSpuTo(768 * 24);
    require(address(memory, 0x1c8) == 0x4004);
    std::cout << "SPU2 checks passed: DMA, ADPCM PCM, core1 addressing, ADSR, looping, ENDX, NAX, pitch, release\n";
}
