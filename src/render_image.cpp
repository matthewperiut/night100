#include "render_image.h"
#include "frame_phases.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace RenderImage {
namespace {
// Retail addresses (SLUS_203.49).
constexpr uint32_t kGlobals=0x362fd0,kGlobalsSize=0x3330;        // zGlobals globals (zEnv.cpp)
constexpr uint32_t kPlayerEnt=kGlobals+0x228;                     // globals.player.ent
constexpr uint32_t kPlayerRender=0x175040;                        // zEntPlayer_Render
constexpr uint32_t kRendermat=0x36e7c0,kRendermatSize=0x40;       // _xMat4x3 rendermat (zEntPlayer.cpp)
constexpr uint32_t kContinuation=0x176148;                        // zero padding after zEntPlayer_Render
constexpr uint32_t kCameraBegin=0x13c6f0,kCameraBeginReturn=0x131d10;
constexpr uint32_t kModelOffset=0x2c,kRenderOffset=0x3c;          // _xEnt::model, _xEnt::render
constexpr uint32_t kInstanceSize=0x30,kInstanceMat=0x20;          // xModelInstance, ::Mat
// zEntPlayer_Render's callees with effects beyond the player's model state
// and rendermat: drawing, lights, shadows and GS state. During the dry run
// they return at once; iModelCull reports "culled".
struct Stub {uint32_t pc;bool culled;PS2Runtime::RecompiledFunction saved=nullptr;};
std::array<Stub,11> stubs{{
    {0x122970,false},{0x129be0,false},{0x121da0,false},{0x121ca0,true},{0x1cc880,false},{0x1cc7c0,false},
    {0x1c3ad0,false},{0x1a1910,false},{0x1a1770,false},{0x13ccf0,false},{0x1da0e0,false}}};

Mode configured=Mode::Off;
bool installed=false,checking=false,imageChecking=false;
Statistics stats;
bool dryRunDone=false,dryRunActive=false,playerRendered=false;
std::array<__m128i,8> savedArgs{};   // a0-a3, t0-t3 of the xCameraBegin call
std::array<float,8> savedFloats{};   // f12-f19
std::array<uint8_t,kRendermatSize> previousRendermat{},computedRendermat{};
struct Saved {uint32_t address=0;std::vector<uint8_t> bytes,lines,granules;};
std::vector<Saved> savedRegions;
std::vector<uint8_t> checkSnapshot;
uint32_t checkStack=0;
PS2Runtime::RecompiledFunction playerRenderOriginal=nullptr;

bool guestRange(uint32_t address,uint32_t size){return address>=0x1000 && size<=PS2_RAM_SIZE && address<=PS2_RAM_SIZE-size;}
uint32_t word(const uint8_t* ram,uint32_t address) {
    if(!guestRange(address,4))return 0;
    uint32_t value;std::memcpy(&value,ram+address,4);return value;
}

// --- Stage 1: render image --------------------------------------------------
#if defined(NIGHT100_DIRTY_LINES)
constexpr uint32_t kLines=PS2_RAM_SIZE>>kPs2DirtyLineShift,kLineSize=1u<<kPs2DirtyLineShift;
static_assert(kLineSize==64,"render image masks assume 64-byte lines");
// Render-owned bytes that must stay in U: interrupt handlers and the frame
// pump read or write them outside the render phases. SDK/sky small data
// (0x357600-0x358600 minus the game's DWARF globals), immediate vertex staging
// and the DMA queue block, and the interrupt-written timer/vsync globals.
struct Range {uint32_t start,end;};
constexpr Range kShared[]={
    {0x35760c,0x357610},{0x357628,0x357634},{0x357644,0x357658},{0x35765c,0x357660},{0x3576a0,0x357b84},{0x357ba4,0x357c08},
    {0x357c0c,0x357c10},{0x357c14,0x357c18},{0x357c1e,0x357c20},{0x357c26,0x357c28},{0x357c38,0x357c50},{0x357c60,0x357c70},
    {0x357c8c,0x357c98},{0x357c9c,0x357ca0},{0x357cb0,0x357cb4},{0x357cc4,0x357cc8},{0x357d1e,0x357d20},{0x357d3c,0x357d40},
    {0x357d44,0x357d50},{0x357d59,0x357d5c},{0x357d5d,0x357d60},{0x357d61,0x357d64},{0x357d65,0x357d68},{0x357d69,0x357d6c},
    {0x357d8c,0x357d90},{0x357d94,0x357d98},{0x357d9c,0x357da0},{0x357da4,0x357da8},{0x357dac,0x357dc0},{0x357de8,0x357dec},
    {0x357e00,0x357e0c},{0x357e10,0x357e18},{0x357e1c,0x357e20},{0x357e28,0x357e30},{0x357e4c,0x357e54},{0x357ef0,0x357f20},
    {0x357fb0,0x357ff8},{0x358014,0x358024},{0x35803c,0x358048},{0x35804c,0x358068},{0x3580ca,0x3580cc},{0x3580ce,0x3580d0},
    {0x358170,0x358174},{0x358192,0x358194},{0x3581b2,0x3581b4},{0x3581c2,0x3581c4},{0x3581f4,0x358200},{0x358228,0x358258},
    {0x35828c,0x358290},{0x3582ac,0x3582b8},{0x3582ce,0x3582d0},{0x3582e4,0x3582f0},{0x358354,0x358368},{0x358384,0x358388},
    {0x35838c,0x358390},{0x3583a8,0x3583b0},{0x35840c,0x358414},{0x358454,0x358588},{0x3585ec,0x3585f4},
    {0x3a0700,0x3a5d80},                       // gRenderArr, gRenderBuffer, DMA queue block
    {0x357ca8,0x357cac},                       // gVsyncCount (vsyncCounterHandler)
    {0x357cbc,0x357ce0},                       // tim0_handler_id, tim0_high, profiling counters (TimerHandler)
    {0x1fe0000,0x1ff0000},                     // interrupt/callback stacks
};
// Heap blocks of the SDK DMA cursors/queue and the sky engine/texture cache
// state are found through pointers in the SDK small data and kept by page.
constexpr std::array<uint32_t,6> kSharedPointerWords{0x3577f8,0x3577fc,0x357940,0x357944,0x3576c8,0x35793c};
constexpr uint32_t kCursorWordsBegin=0x357718,kCursorWordsEnd=0x357778;
std::vector<uint8_t> sharedLine;      // 0 private, 1 shared, 2 check bytes against kShared
std::vector<uint8_t> sharedPage;      // 4 KiB pages kept shared (dynamic)
// 4 KiB pages update has stored to since the last resync. Render's writes there
// are shared: render also mutates RenderWare structures update owns (light
// lists, frame links), and byte-merging two divergent copies of a linked
// structure is not today's memory. Only pages update never writes stay private.
std::vector<uint8_t> updatePage;
struct PrivateLine {uint64_t mask=0;std::array<uint8_t,kLineSize> value{};};
std::unordered_map<uint32_t,PrivateLine> privateLines;
std::vector<uint8_t> shadow;          // S: U as last synchronized
bool imageReady=false,windowOpen=false;
uint32_t windowStack=0;
struct HostWriter {uint32_t pc;PS2Runtime::RecompiledFunction original;};
std::vector<HostWriter> hostWriters,cursorHooks;
std::map<std::pair<uint32_t,FramePhases::Phase>,uint64_t> holePages; // check mode: unmarked changes per 4 KiB page

inline uint64_t nowNs(){return FramePhases::wallNanoseconds();}
inline void markLines(uint32_t address,uint32_t size) {
    if(!size)return;
    address&=PS2_RAM_MASK;
    const uint32_t last=std::min<uint64_t>(uint64_t(address)+size-1,PS2_RAM_SIZE-1);
    for(uint32_t line=address>>kPs2DirtyLineShift;line<=(last>>kPs2DirtyLineShift);++line)g_ps2DirtyLines[line]=1;
    for(uint32_t granule=address>>kPs2DirtyGranuleShift;granule<=(last>>kPs2DirtyGranuleShift);++granule)g_ps2DirtyGranules[granule]=1;
}
bool shareAll=false,lineConflicts=false; // desktop experiment knobs
// Why a render-written byte stays in U (0: it is render-private).
enum Share : uint8_t {Private,Infrastructure,Stack,UpdatePage};
Share sharedByte(uint32_t address) {
    if(shareAll)return UpdatePage;
    const uint8_t kind=sharedLine[address>>kPs2DirtyLineShift];
    if(kind==1 || sharedPage[address>>12])return Infrastructure;
    if(kind==2)for(const auto& range:kShared)if(address>=range.start && address<range.end)return Infrastructure;
    if(address>=0x1fe0000)return Stack;   // interrupt stacks and the main stack's frames
    if(updatePage[address>>12])return UpdatePage;
    return Private;
}
std::map<uint32_t,uint64_t> updatePageShares; // render bytes shared only because update writes the page
void noteSharedPointers(const uint8_t* ram) {
    auto keep=[&](uint32_t value,uint32_t margin) {
        if(value<0x3b3500+margin || value>=0x1fe0000-margin)return;
        for(uint32_t page=(value-margin)>>12;page<=((value+margin)>>12);++page)sharedPage[page]=1;
    };
    for(uint32_t a=kCursorWordsBegin;a<=kCursorWordsEnd;a+=4)keep(word(ram,a),0x4000);
    for(uint32_t a:kSharedPointerWords)keep(word(ram,a),0x1000);
}
void cursorEntry(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    for(const auto& hook:cursorHooks)if(hook.pc==cpu->pc){noteSharedPointers(ram);hook.original(ram,cpu,runtime);return;}
    throw std::runtime_error("Unexpected render image cursor hook PC");
}
// HLE library routines write guest RAM from host code, outside the store
// macros' marking. Mark what they write.
void hostWriterEntry(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    const uint32_t pc=cpu->pc,a0=GPR_U32(cpu,4),a1=GPR_U32(cpu,5),a2=GPR_U32(cpu,6),a3=GPR_U32(cpu,7);
    PS2Runtime::RecompiledFunction original=nullptr;
    for(const auto& hook:hostWriters)if(hook.pc==pc)original=hook.original;
    if(!original)throw std::runtime_error("Unexpected render image host-writer PC");
    auto stringLength=[&](uint32_t address) {
        uint32_t n=0;while(guestRange(address+n,1) && n<0x100000 && ram[address+n])++n;return n+1;
    };
    switch(pc) {
    case 0x2572c8: case 0x257480: case 0x257378: markLines(a0,a2);break; // memcpy, memset, memmove
    case 0x101e78: markLines(a0,a1);break;                              // memclr
    case 0x25ab00: markLines(a0,a2);break;                              // strncpy
    case 0x259d68: markLines(a0,a1);break;                              // snprintf
    case 0x255170: markLines(a0,a1*a2);break;                           // fread
    case 0x105268: case 0x109418: markLines(a1,a2);break;               // read, sceRead
    case 0x257118: markLines(a1,4);break;                               // _mbtowc_r
    case 0x10e218: markLines(a2,32);break;                              // scePadRead
    case 0x113500: markLines(a1,48);break;                             // nativeQuatToMat (native_cpu)
    case 0x113fe0: markLines(a0,48);break;                             // nativeMat3x3Mul
    case 0x121ed0: markLines(a3,64*64);break;                          // nativeModelAnimMatrices: bone matrices
    case 0x1d3f60: case 0x1d48e0: case 0x1d3fb0: case 0x1d4b20:
        markLines(0x379200,0x27100);break;                             // native particle kernels: gParPool
    default: break;
    }
    original(ram,cpu,runtime);
    switch(pc) {
    case 0x25a548: case 0x25a140: case 0x25a798: case 0x25fbe0: markLines(a0,stringLength(a0));break; // strcpy, strcat, strncat, vsprintf
    case 0x253248: markLines(GPR_U32(cpu,2),a1*a2);break;               // _calloc_r
    case 0x2591c0: markLines(GPR_U32(cpu,2),a2);break;                  // _realloc_r
    default: break;
    }
    (void)a3;
}
void resync(uint8_t* ram) {
    shadow.assign(ram,ram+PS2_RAM_SIZE);updatePage.assign(PS2_RAM_SIZE>>12,0);
    std::memset(g_ps2DirtyLines,0,sizeof(g_ps2DirtyLines));std::memset(g_ps2DirtyGranules,0,sizeof(g_ps2DirtyGranules));
    privateLines.clear();windowOpen=false;imageReady=true;
}
template<class Visit>void scanDirty(Visit visit) {
    const uint64_t start=nowNs();
    const auto* words=reinterpret_cast<const uint64_t*>(g_ps2DirtyLines);
    for(uint32_t w=0;w<kLines/8;++w) {
        uint64_t bits;std::memcpy(&bits,words+w,8);
        if(!bits)continue;
        for(uint32_t i=0;i<8;++i)if(g_ps2DirtyLines[w*8+i]) {
            const uint32_t line=w*8+i;g_ps2DirtyLines[line]=0;visit(line);
            std::memset(g_ps2DirtyGranules+size_t(line)*(kLineSize>>kPs2DirtyGranuleShift),0,kLineSize>>kPs2DirtyGranuleShift);
        }
    }
    stats.scanNs+=nowNs()-start;
}
void swapInPrivate(uint8_t* ram) {
    for(auto it=privateLines.begin();it!=privateLines.end();) {
        auto& line=it->second;uint8_t* u=ram+size_t(it->first)*kLineSize;
        for(uint32_t i=0;i<kLineSize;++i)if(line.mask>>i&1)u[i]=line.value[i];
        ++it;
    }
}
// Window begin: U changes since the last window reach S (and win over P),
// then P is applied so the render phases see R.
void beginWindow(uint8_t* ram,const R5900Context* cpu) {
    if(!imageReady)resync(ram);
    const uint64_t start=nowNs();
    noteSharedPointers(ram);
    // A P byte loses to update when update stored to its 8-byte granule since
    // it was swapped out (same-value stores included) or its U value changed
    // (host writes). NIGHT100_RENDER_IMAGE_LINE_CONFLICTS=1 widens this to the line.
    for(auto it=privateLines.begin();it!=privateLines.end();) {
        auto& line=it->second;const uint8_t* u=ram+size_t(it->first)*kLineSize;const uint8_t* s=shadow.data()+size_t(it->first)*kLineSize;
        const uint8_t* granules=g_ps2DirtyGranules+size_t(it->first)*(kLineSize>>kPs2DirtyGranuleShift);
        if(lineConflicts && g_ps2DirtyLines[it->first])line.mask=0;
        for(uint32_t i=0;i<kLineSize;++i)
            if((line.mask>>i&1) && (u[i]!=s[i] || granules[i>>kPs2DirtyGranuleShift]))line.mask&=~(uint64_t(1)<<i);
        it=line.mask?std::next(it):privateLines.erase(it);
    }
    if(imageChecking) {
        // Lines that changed in U without a dirty mark are tracking holes.
        for(uint32_t l=0;l<kLines;++l) {
            if(g_ps2DirtyLines[l])continue;
            const size_t o=size_t(l)*kLineSize;
            if(std::memcmp(ram+o,shadow.data()+o,kLineSize)) {
                ++stats.holeLines;++holePages[{uint32_t(o>>12),FramePhases::state.current}];
                std::memcpy(shadow.data()+o,ram+o,kLineSize);
            }
        }
    }
    scanDirty([&](uint32_t line) {
        std::memcpy(shadow.data()+size_t(line)*kLineSize,ram+size_t(line)*kLineSize,kLineSize);++stats.syncedLines;
        updatePage[line>>(12-kPs2DirtyLineShift)]=1;
    });
    // P bytes in pages update now writes become shared: today's memory holds
    // render's value there until update overwrites it.
    for(auto it=privateLines.begin();it!=privateLines.end();) {
        const uint32_t lineIndex=it->first;
        if(!updatePage[lineIndex>>(12-kPs2DirtyLineShift)]){++it;continue;}
        uint8_t* u=ram+size_t(lineIndex)*kLineSize;uint8_t* sh=shadow.data()+size_t(lineIndex)*kLineSize;
        for(uint32_t i=0;i<kLineSize;++i)if(it->second.mask>>i&1){u[i]=it->second.value[i];sh[i]=u[i];++stats.promotedBytes;}
        it=privateLines.erase(it);
    }
    swapInPrivate(ram);
    windowStack=GPR_U32(cpu,29);windowOpen=true;++stats.windows;
    stats.beginNs+=nowNs()-start;
}
// Window end: render's writes become P (U restored from S), except shared
// bytes, which stay in U; untouched P bytes are swapped back out.
void endWindow(uint8_t* ram) {
    if(!windowOpen)return;
    const uint64_t start=nowNs();
    noteSharedPointers(ram);
    scanDirty([&](uint32_t lineIndex) {
        ++stats.renderLines;
        uint8_t* u=ram+size_t(lineIndex)*kLineSize;uint8_t* s=shadow.data()+size_t(lineIndex)*kLineSize;
        auto found=privateLines.find(lineIndex);
        PrivateLine* line=found==privateLines.end()?nullptr:&found->second;
        for(uint32_t i=0;i<kLineSize;++i) {
            const uint64_t bit=uint64_t(1)<<i;
            if(u[i]==s[i]){if(line)line->mask&=~bit;continue;}
            if(const Share share=sharedByte(lineIndex*kLineSize+i)) {
                s[i]=u[i];++stats.sharedBytes;if(line)line->mask&=~bit;
                if(share==Infrastructure)++stats.sharedInfrastructureBytes;
                else if(share==Stack)++stats.sharedStackBytes;
                else {++stats.sharedUpdatePageBytes;++updatePageShares[(lineIndex*kLineSize+i)&~0xfffu];}
                continue;
            }
            if(!line)line=&privateLines[lineIndex];
            line->value[i]=u[i];line->mask|=bit;u[i]=s[i];
        }
        if(line && !line->mask)privateLines.erase(lineIndex);
    });
    // P bytes in lines render did not touch this window: swap out.
    uint64_t bytes=0;
    for(auto& [lineIndex,line]:privateLines) {
        uint8_t* u=ram+size_t(lineIndex)*kLineSize;const uint8_t* s=shadow.data()+size_t(lineIndex)*kLineSize;
        for(uint32_t i=0;i<kLineSize;++i)if(line.mask>>i&1){u[i]=s[i];++bytes;}
    }
    stats.privateBytes=bytes;windowOpen=false;
    stats.endNs+=nowNs()-start;
}
void installImage(PS2Runtime& runtime) {
    sharedLine.assign(kLines,0);sharedPage.assign(PS2_RAM_SIZE>>12,0);
    for(const auto& range:kShared)
        for(uint32_t line=range.start>>kPs2DirtyLineShift;line<=((range.end-1)>>kPs2DirtyLineShift);++line) {
            const bool whole=range.start<=line*kLineSize && range.end>=(line+1)*kLineSize;
            sharedLine[line]=whole?1:std::max<uint8_t>(sharedLine[line],2);
        }
    auto wrap=[&](uint32_t pc,PS2Runtime::RecompiledFunction hook,std::vector<HostWriter>& list) {
        const auto original=runtime.lookupFunction(pc);
        if(!original || !runtime.replaceFunction(pc,hook))throw std::runtime_error("Cannot install render image hook");
        list.push_back({pc,original});
    };
    for(uint32_t pc:{0x2572c8u,0x257480u,0x257378u,0x101e78u,0x25ab00u,0x259d68u,0x255170u,0x105268u,0x109418u,
                     0x257118u,0x10e218u,0x25a548u,0x25a140u,0x25a798u,0x25fbe0u,0x253248u,0x2591c0u,
                     0x113500u,0x113fe0u,0x121ed0u,0x1d3f60u,0x1d48e0u,0x1d3fb0u,0x1d4b20u})
        wrap(pc,hostWriterEntry,hostWriters);
    for(uint32_t pc:{0x1f2498u,0x1f2070u,0x1f22a8u,0x1fe438u,0x1feab0u,0x1f2d00u,0x1f4118u})
        wrap(pc,cursorEntry,cursorHooks);
    shadow.reserve(PS2_RAM_SIZE);imageReady=false;
}
// Phase transitions that open and close the render windows.
void imageBoundary(FramePhases::Phase ended,FramePhases::Phase next,uint8_t* ram,R5900Context* cpu) {
    using namespace FramePhases;
    if(ended==next)return;
    if(next==RenderPrep || next==CameraEnd || next==Present)beginWindow(ram,cpu);
    else if((next==RenderEnd || next==Wait || next==PostPresent) && windowOpen) {
        endWindow(ram);
        if(next==PostPresent)++stats.frames;
    }
}
#endif

void save(const uint8_t* ram,uint32_t address,uint32_t size) {
    if(!guestRange(address,size))return;
    Saved region{address,std::vector<uint8_t>(ram+address,ram+address+size),{},{}};
#if defined(NIGHT100_DIRTY_LINES)
    // The dry run is invisible to the render image: its stores' dirty marks
    // are restored with the bytes (rendermat alone is marked afterwards).
    if(configured==Mode::Image) {
        const uint32_t firstLine=address>>kPs2DirtyLineShift,lastLine=(address+size-1)>>kPs2DirtyLineShift;
        region.lines.assign(g_ps2DirtyLines+firstLine,g_ps2DirtyLines+lastLine+1);
        region.granules.assign(g_ps2DirtyGranules+(size_t(firstLine)<<(kPs2DirtyLineShift-kPs2DirtyGranuleShift)),
                               g_ps2DirtyGranules+(size_t(lastLine+1)<<(kPs2DirtyLineShift-kPs2DirtyGranuleShift)));
    }
#endif
    savedRegions.push_back(std::move(region));
}
void stubFunction(uint8_t*,R5900Context* cpu,PS2Runtime*) {
    for(const auto& stub:stubs)if(stub.pc==cpu->pc) {
        if(stub.culled)SET_GPR_U32(cpu,2,1);
        cpu->pc=GPR_U32(cpu,31);return;
    }
    throw std::runtime_error("Unexpected rendermat dry-run stub");
}
void playerRenderEntry(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc==kPlayerRender && !dryRunActive)playerRendered=true;
    playerRenderOriginal(ram,cpu,runtime);
}
bool startDryRun(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(word(ram,kPlayerEnt+kRenderOffset)!=kPlayerRender){++stats.skipped;return false;}
    std::memcpy(previousRendermat.data(),ram+kRendermat,kRendermatSize);
    // Everything the dry run may change besides rendermat and its stack frame:
    // zGlobals (player state, render counters) and the player's model chain
    // (instance flags and bone fields, plus each instance's matrix).
    savedRegions.clear();
    save(ram,kGlobals,kGlobalsSize);
    uint32_t instance=word(ram,kPlayerEnt+kModelOffset);
    for(unsigned n=0;instance && n<64;++n,instance=word(ram,instance)) {
        save(ram,instance,kInstanceSize);
        save(ram,word(ram,instance+kInstanceMat),0x40);
    }
    if(checking) {
        checkSnapshot.assign(ram,ram+PS2_RAM_SIZE);checkStack=GPR_U32(cpu,29);
    }
    for(unsigned i=0;i<8;++i)savedArgs[i]=cpu->r[4+i];
    for(unsigned i=0;i<8;++i)savedFloats[i]=cpu->f[12+i];
    for(auto& stub:stubs) {
        stub.saved=runtime->lookupFunction(stub.pc);
        if(!stub.saved || !runtime->replaceFunction(stub.pc,stubFunction))throw std::runtime_error("Cannot install rendermat dry-run stubs");
    }
    dryRunActive=true;++stats.dryRuns;
    // A real guest call: scheduler yields and interrupts inside it keep their
    // ordinary continuations. zEntPlayer_Render returns to kContinuation.
    SET_GPR_U32(cpu,4,kPlayerEnt);SET_GPR_U32(cpu,31,kContinuation);cpu->pc=kPlayerRender;
    return true;
}
void finishDryRun(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(!dryRunActive || cpu->pc!=kContinuation)throw std::runtime_error("Unexpected rendermat dry-run continuation");
    for(auto& stub:stubs){runtime->replaceFunction(stub.pc,stub.saved);stub.saved=nullptr;}
    dryRunActive=false;
    std::memcpy(computedRendermat.data(),ram+kRendermat,kRendermatSize);
    for(const auto& region:savedRegions) {
        std::memcpy(ram+region.address,region.bytes.data(),region.bytes.size());
#if defined(NIGHT100_DIRTY_LINES)
        if(!region.lines.empty()) {
            const uint32_t firstLine=region.address>>kPs2DirtyLineShift;
            std::memcpy(g_ps2DirtyLines+firstLine,region.lines.data(),region.lines.size());
            std::memcpy(g_ps2DirtyGranules+(size_t(firstLine)<<(kPs2DirtyLineShift-kPs2DirtyGranuleShift)),region.granules.data(),region.granules.size());
        }
#endif
    }
#if defined(NIGHT100_DIRTY_LINES)
    if(configured==Mode::Image)markLines(kRendermat,kRendermatSize);
#endif
    if(checking) {
        // The only lasting change must be rendermat; the dry run's own frame
        // lies below the caller's stack pointer. The timer interrupt's
        // tim0_high may legitimately advance meanwhile.
        ++stats.checkFrames;uint64_t violations=0;
        for(uint32_t a=0;a<PS2_RAM_SIZE;++a) {
            if(ram[a]==checkSnapshot[a])continue;
            if(a>=kRendermat && a<kRendermat+kRendermatSize)continue;
            if(a<checkStack && a+0x4000>=checkStack)continue;
            if(a>=0x357cc0 && a<0x357cc4)continue;
            if(violations++<8)std::cerr<<"[render-image] rendermat dry run changed 0x"<<std::hex<<a<<std::dec<<'\n';
        }
        if(violations)++stats.checkViolations;
    }
    for(unsigned i=0;i<8;++i)cpu->r[4+i]=savedArgs[i];
    for(unsigned i=0;i<8;++i)cpu->f[12+i]=savedFloats[i];
    SET_GPR_U32(cpu,31,kCameraBeginReturn);cpu->pc=kCameraBegin;
}
void afterRender(uint8_t* ram) {
    if(!dryRunDone || stats.dryRuns==0)return;
    if(playerRendered) {
        ++stats.rendered;
        if(std::memcmp(ram+kRendermat,computedRendermat.data(),kRendermatSize))++stats.renderMismatches;
        else ++stats.renderMatches;
    } else {
        // The original leaves rendermat unchanged in a frame that does not
        // render the player. Single-threaded, this restores it exactly.
        ++stats.unrendered;std::memcpy(ram+kRendermat,previousRendermat.data(),kRendermatSize);
#if defined(NIGHT100_DIRTY_LINES)
        markLines(kRendermat,kRendermatSize);
#endif
    }
    playerRendered=false;
}
void report() {
    std::cout<<"[render-image] rendermat dry runs="<<stats.dryRuns<<" rendered="<<stats.rendered
             <<" unrendered="<<stats.unrendered<<" mismatches="<<stats.renderMismatches
             <<" skipped="<<stats.skipped<<" check_violations="<<stats.checkViolations;
#if defined(NIGHT100_DIRTY_LINES)
    if(configured==Mode::Image && stats.frames) {
        const double frames=double(stats.frames);
        std::cout<<" image_frames="<<stats.frames<<" synced_lines/frame="<<double(stats.syncedLines)/frames
                 <<" render_lines/frame="<<double(stats.renderLines)/frames<<" private_bytes="<<stats.privateBytes
                 <<" shared_bytes/frame="<<double(stats.sharedBytes)/frames
                 <<" begin_ms/frame="<<double(stats.beginNs)*1e-6/frames<<" end_ms/frame="<<double(stats.endNs)*1e-6/frames
                 <<" scan_ms/frame="<<double(stats.scanNs)*1e-6/frames<<" hole_lines="<<stats.holeLines
                 <<" promoted_bytes="<<stats.promotedBytes<<" update_pages="<<std::count(updatePage.begin(),updatePage.end(),1)
                 <<" shared_infrastructure/frame="<<double(stats.sharedInfrastructureBytes)/frames
                 <<" shared_stack/frame="<<double(stats.sharedStackBytes)/frames
                 <<" shared_update_page/frame="<<double(stats.sharedUpdatePageBytes)/frames;
        std::vector<std::pair<uint64_t,uint32_t>> top;
        for(const auto& [page,count]:updatePageShares)top.push_back({count,page});
        std::sort(top.rbegin(),top.rend());
        std::cout<<"\n[render-image] update-page shared bytes/frame by page:";
        for(size_t i=0;i<top.size() && i<16;++i)std::cout<<" 0x"<<std::hex<<top[i].second<<std::dec<<'='<<double(top[i].first)/frames;
    }
#endif
    std::cout<<'\n';
#if defined(NIGHT100_DIRTY_LINES)
    if(imageChecking && !holePages.empty()) {
        std::vector<std::pair<uint64_t,std::pair<uint32_t,FramePhases::Phase>>> top;
        for(const auto& [key,count]:holePages)top.push_back({count,key});
        std::sort(top.rbegin(),top.rend());
        std::cout<<"[render-image] unmarked U changes by 4 KiB page (lines, window opened after):";
        for(size_t i=0;i<top.size() && i<24;++i)
            std::cout<<" 0x"<<std::hex<<(top[i].second.first<<12)<<std::dec<<'/'<<FramePhases::names[top[i].second.second]<<'='<<top[i].first;
        std::cout<<'\n';
    }
#endif
}
bool boundary(FramePhases::Phase ended,FramePhases::Phase next,uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(next==FramePhases::RenderPrep && !dryRunDone) {
        dryRunDone=true;playerRendered=false;
        if(startDryRun(ram,cpu,runtime))return true;
    }
#if defined(NIGHT100_DIRTY_LINES)
    if(configured==Mode::Image)imageBoundary(ended,next,ram,cpu);
#else
    (void)ended;
#endif
    if(next==FramePhases::RenderEnd) {
        afterRender(ram);
        if(stats.dryRuns && stats.dryRuns%300==0)report();
    } else if(next==FramePhases::PostPresent)dryRunDone=false;
    return false;
}
}
void configure(Mode value){configured=value;}
Mode mode(){return installed?configured:Mode::Off;}
Statistics statistics(){return stats;}
void beforeSnapshotSave(PS2Runtime& runtime) {
#if defined(NIGHT100_DIRTY_LINES)
    // Outside a render window U lacks P; the saved state is the render image.
    if(installed && configured==Mode::Image && imageReady && !windowOpen)swapInPrivate(runtime.memory().getRDRAM());
#else
    (void)runtime;
#endif
}
void afterSnapshotSave(PS2Runtime& runtime) {
#if defined(NIGHT100_DIRTY_LINES)
    if(installed && configured==Mode::Image && imageReady && !windowOpen) {
        uint8_t* ram=runtime.memory().getRDRAM();
        for(auto& [lineIndex,line]:privateLines) {
            uint8_t* u=ram+size_t(lineIndex)*kLineSize;const uint8_t* s=shadow.data()+size_t(lineIndex)*kLineSize;
            for(uint32_t i=0;i<kLineSize;++i)if(line.mask>>i&1)u[i]=s[i];
        }
    }
#else
    (void)runtime;
#endif
}
void afterSnapshotLoad(PS2Runtime& runtime) {
#if defined(NIGHT100_DIRTY_LINES)
    if(installed && configured==Mode::Image)resync(runtime.memory().getRDRAM());
#else
    (void)runtime;
#endif
    dryRunDone=false;playerRendered=false;
}
void install(PS2Runtime& runtime) {
#if !defined(__SWITCH__)
    if(const char* value=std::getenv("NIGHT100_RENDER_IMAGE"))
        configured=std::strcmp(value,"rendermat")==0?Mode::Rendermat:std::strcmp(value,"image")==0?Mode::Image:Mode::Off;
    checking=std::getenv("NIGHT100_RENDERMAT_CHECK")!=nullptr;
    imageChecking=std::getenv("NIGHT100_RENDER_IMAGE_CHECK")!=nullptr;
#if defined(NIGHT100_DIRTY_LINES)
    shareAll=std::getenv("NIGHT100_RENDER_IMAGE_SHARE_ALL")!=nullptr;
    lineConflicts=std::getenv("NIGHT100_RENDER_IMAGE_LINE_CONFLICTS")!=nullptr;
#endif
#endif
    if(configured==Mode::Off || installed)return;
#if !defined(NIGHT100_DIRTY_LINES)
    if(configured==Mode::Image)throw std::runtime_error("The render image needs a -DNIGHT100_DIRTY_LINES=ON build");
#endif
    uint8_t* ram=runtime.memory().getRDRAM();
    for(uint32_t a=kContinuation-4;a<=kContinuation+4;a+=4)
        if(word(ram,a))throw std::runtime_error("Render image continuation padding is not free");
    if(word(ram,kPlayerRender)!=0x27bdfea0u)throw std::runtime_error("zEntPlayer_Render does not match the retail ELF");
    playerRenderOriginal=runtime.lookupFunction(kPlayerRender);
    if(!playerRenderOriginal || !runtime.replaceFunction(kPlayerRender,playerRenderEntry) ||
       !runtime.replaceFunction(kContinuation,finishDryRun))
        throw std::runtime_error("Cannot install the rendermat dry run");
#if defined(NIGHT100_DIRTY_LINES)
    if(configured==Mode::Image)installImage(runtime);
#endif
    FramePhases::setBoundaryHook(boundary);
    stats={};installed=true;
    std::cout<<"[night100] rendermat computed at end of update"<<(configured==Mode::Image?"; render image (stage 1, single-threaded)":"")
             <<(checking?", write-set check on":"")<<(imageChecking?", dirty-line hole check on":"")<<'\n';
}
}
