#include "render_trace.h"
#include "replay_catalog.h"
#include "replay_harness.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace RenderTrace {
namespace {
using FramePhases::Phase;
constexpr uint32_t page4k=4096,runGap=8;
struct Config {
    std::filesystem::path directory;
    uint64_t skip=60,every=16,samples=24;
    bool faults=false,calls=true,attribute=false;
};
Config config;
bool enabled=false;
std::vector<uint8_t> snapshot,scratchSnapshot;
uint64_t iteration=0,samplesTaken=0;
bool sampleActive=false;
std::ofstream runs,pages,access,calls,threads,native,scratch,summary,writers,checkpoints,cursorLog;
bool checkpointImages=false,exitAfterReplay=false,sawReplay=false;
uint64_t dumpIteration=0;
// Attribution: pages a phase changed in earlier samples are compared at every
// guest function entry; changes go to the function entered most recently.
std::array<std::vector<uint32_t>,FramePhases::Count> learnedPages;
std::vector<uint8_t> shadow;uint32_t lastPc=0;Phase attributedPhase=FramePhases::Count;

// --- Snapshot diff -------------------------------------------------------
struct DiffTotals {uint64_t bytes=0,runs=0,pages=0;};
DiffTotals diffAndRecord(uint8_t* ram,Phase phase) {
    DiffTotals totals;
    std::vector<uint32_t> pageBytes(PS2_RAM_SIZE/page4k,0);
    uint32_t runStart=0,runEnd=0,runChanged=0;bool open=false;
    auto flush=[&] {
        if(!open)return;
        runs<<samplesTaken<<','<<iteration<<','<<FramePhases::names[phase]<<','<<runStart<<','<<runEnd<<','<<runChanged<<'\n';
        ++totals.runs;open=false;
    };
    uint8_t* old=snapshot.data();
    for(uint32_t offset=0;offset<PS2_RAM_SIZE;offset+=8) {
        uint64_t a,b;std::memcpy(&a,ram+offset,8);std::memcpy(&b,old+offset,8);
        if(a==b)continue;
        for(uint32_t i=0;i<8;++i) {
            if(ram[offset+i]==old[offset+i])continue;
            const uint32_t at=offset+i;
            if(open && at<=runEnd+runGap){runEnd=at+1;++runChanged;}
            else {flush();runStart=at;runEnd=at+1;runChanged=1;open=true;}
            ++pageBytes[at/page4k];++totals.bytes;
        }
        std::memcpy(old+offset,ram+offset,8);
    }
    flush();
    auto& learned=learnedPages[phase];
    for(size_t p=0;p<pageBytes.size();++p)if(pageBytes[p]) {
        if(!std::binary_search(learned.begin(),learned.end(),uint32_t(p)))learned.insert(std::upper_bound(learned.begin(),learned.end(),uint32_t(p)),uint32_t(p));
        pages<<samplesTaken<<','<<iteration<<','<<FramePhases::names[phase]<<','<<p*page4k<<','<<pageBytes[p]<<'\n';
        ++totals.pages;
    }
    return totals;
}

// --- Page access tracking (mprotect faults) --------------------------------
uintptr_t guardLow=0,guardHigh=0;size_t hostPage=0,guardPages=0;
std::unique_ptr<std::atomic<uint8_t>[]> pageState,pageFlags;
std::atomic<bool> guardActive{false};
pthread_t gameThread;
struct sigaction previousBus{},previousSegv{};
void chain(int signal,siginfo_t* info,void* context) {
    const struct sigaction& previous=signal==SIGBUS?previousBus:previousSegv;
    if(previous.sa_flags&SA_SIGINFO){previous.sa_sigaction(signal,info,context);return;}
    if(previous.sa_handler==SIG_IGN)return;
    if(previous.sa_handler==SIG_DFL){::signal(signal,SIG_DFL);raise(signal);return;}
    previous.sa_handler(signal);
}
void onFault(int signal,siginfo_t* info,void* context) {
    const uintptr_t address=reinterpret_cast<uintptr_t>(info->si_addr);
    if(!guardActive.load(std::memory_order_acquire) || address<guardLow || address>=guardHigh){chain(signal,info,context);return;}
    const size_t index=(address-guardLow)/hostPage;
    void* pageAddress=reinterpret_cast<void*>(guardLow+index*hostPage);
    const bool main=pthread_equal(pthread_self(),gameThread);
    // Two levels: the first fault grants read, a second (write) fault grants write.
    uint8_t expected=0;
    if(pageState[index].compare_exchange_strong(expected,1)) {
        mprotect(pageAddress,hostPage,PROT_READ);pageFlags[index].fetch_or(main?1:4);return;
    }
    if(expected==1 && pageState[index].compare_exchange_strong(expected,2)) {
        mprotect(pageAddress,hostPage,PROT_READ|PROT_WRITE);pageFlags[index].fetch_or(main?2:8);return;
    }
    // Another thread already widened this page; retry the access.
    if(expected==1)mprotect(pageAddress,hostPage,PROT_READ|PROT_WRITE);
}
void protectAll() {
    for(size_t i=0;i<guardPages;++i){pageState[i].store(0);pageFlags[i].store(0);}
    guardActive.store(true,std::memory_order_release);
    if(mprotect(reinterpret_cast<void*>(guardLow),guardHigh-guardLow,PROT_NONE))throw std::runtime_error("render trace mprotect failed");
}
void unprotectAll() {
    if(!guardActive.load())return;
    if(mprotect(reinterpret_cast<void*>(guardLow),guardHigh-guardLow,PROT_READ|PROT_WRITE))throw std::runtime_error("render trace unprotect failed");
    guardActive.store(false,std::memory_order_release);
}
void recordAccess(uint8_t* ram,Phase phase) {
    for(size_t i=0;i<guardPages;++i) {
        const uint8_t flags=pageFlags[i].load();if(!flags)continue;
        const uintptr_t start=guardLow+i*hostPage-reinterpret_cast<uintptr_t>(ram);
        access<<samplesTaken<<','<<iteration<<','<<FramePhases::names[phase]<<','<<start<<','<<hostPage<<','
              <<(flags&1?1:0)<<','<<(flags&2?1:0)<<','<<(flags&4?1:0)<<','<<(flags&8?1:0)<<'\n';
    }
}

// --- Dispatch trampoline ---------------------------------------------------
std::vector<PS2Runtime::RecompiledFunction> saved;
uint32_t tableBase=0;
std::array<std::unordered_map<uint32_t,uint64_t>,FramePhases::Count> entryCounts;
std::map<std::pair<unsigned,int>,uint64_t> threadEntries;
std::map<std::pair<unsigned,uint32_t>,uint64_t> irqEntries;
struct NativeBytes {uint64_t calls=0,dynamicBytes=0,items=0;};
std::array<std::map<std::string,NativeBytes>,FramePhases::Count> nativeBytes;
uint32_t guestWord(uint8_t* ram,uint32_t address) {
    if(address<0x1000 || address>PS2_RAM_SIZE-4)return 0;
    uint32_t value;std::memcpy(&value,ram+address,4);return value;
}
uint16_t guestHalf(uint8_t* ram,uint32_t address) {
    if(address<0x1000 || address>PS2_RAM_SIZE-2)return 0;
    uint16_t value;std::memcpy(&value,ram+address,2);return value;
}
// Dynamic per-draw data the native hooks copy (geometry and textures are
// cached by identity and excluded unless the guest marks geometry dirty).
void estimateNative(uint32_t pc,uint8_t* ram,R5900Context* cpu,Phase phase) {
    auto add=[&](const char* kind,uint64_t bytes,uint64_t items) {
        auto& entry=nativeBytes[phase][kind];++entry.calls;entry.dynamicBytes+=bytes;entry.items+=items;
    };
    constexpr uint64_t drawState=64+16+64; // camera matrix, z/projection/blend/cull words, render-state queries
    switch(pc) {
    case 0x2250c0: { // RpAtomic default render callback
        const uint32_t atomic=GPR_U32(cpu,4),geo=guestWord(ram,atomic+0x18);
        const uint32_t header=guestWord(ram,geo+0x60),meshes=guestHalf(ram,header+4);
        const uint32_t skinOffset=guestWord(ram,0x2fe440),skin=skinOffset && skinOffset<0x1000?guestWord(ram,geo+skinOffset):0;
        const uint32_t bones=skin?guestWord(ram,skin):0,vertices=guestWord(ram,geo+0x14);
        const bool dirty=guestHalf(ram,geo+0xc)!=0;
        add("atomic",drawState+64+uint64_t(bones)*64+uint64_t(meshes)*24,1);
        if(bones)add("atomic_skinned",uint64_t(bones)*64,bones);
        if(dirty)add("atomic_dirty_geometry",uint64_t(vertices)*36,vertices);
        break;
    }
    case 0x22f848: { // world sector
        const uint32_t sector=GPR_U32(cpu,4),header=guestWord(ram,sector+0x84);
        add("world_sector",drawState+uint64_t(guestHalf(ram,header+4))*24,1);break;
    }
    case 0x1b0740: { // iRenderTrianglesImmediate (particles)
        const uint32_t count=GPR_U32(cpu,7),indices=GPR_U32(cpu,9);
        add("particles",drawState+uint64_t(count)*36+uint64_t(indices)*2,count);break;
    }
    case 0x205548: add("im2d",64+uint64_t(GPR_U32(cpu,6))*64,GPR_U32(cpu,6));break;
    case 0x2055b8: add("im2d_indexed",64+uint64_t(GPR_U32(cpu,6))*64+uint64_t(GPR_U32(cpu,8))*2,GPR_U32(cpu,6));break;
    case 0x209318: add("im3d_transform",drawState+uint64_t(GPR_U32(cpu,5))*36,GPR_U32(cpu,5));break;
    case 0x1f2498: if(GPR_U32(cpu,31)==0x121ecc)add("native_marker_packet",32,1);break;
    default: break;
    }
}
bool attributing(Phase phase) {
    return config.attribute && samplesTaken>0 && phase!=FramePhases::Other && phase!=FramePhases::Wait && !learnedPages[phase].empty();
}
void startAttribution(uint8_t* ram,Phase phase,uint32_t pc) {
    attributedPhase=phase;lastPc=pc;
    for(uint32_t page:learnedPages[phase])std::memcpy(shadow.data()+size_t(page)*page4k,ram+size_t(page)*page4k,page4k);
}
void attribute(uint8_t* ram,uint32_t writer) {
    for(uint32_t page:learnedPages[attributedPhase]) {
        uint8_t* now=ram+size_t(page)*page4k;uint8_t* old=shadow.data()+size_t(page)*page4k;
        if(!std::memcmp(now,old,page4k))continue;
        uint32_t start=0,last=0,changed=0;bool open=false;
        auto flush=[&] {
            if(open)writers<<samplesTaken<<','<<iteration<<','<<FramePhases::names[attributedPhase]<<','<<writer<<','
                           <<page*page4k+start<<','<<page*page4k+last+1<<','<<changed<<'\n';
            open=false;
        };
        for(uint32_t i=0;i<page4k;++i) {
            if(now[i]==old[i])continue;
            if(open && i>last+runGap)flush();
            if(!open){open=true;start=i;changed=0;}
            last=i;++changed;
        }
        flush();
        std::memcpy(old,now,page4k);
    }
}
void trampoline(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    const uint32_t pc=cpu->pc;
    const auto fn=saved[(pc-tableBase)>>2];
    if(sampleActive) {
        const Phase phase=FramePhases::state.current;
        ++entryCounts[phase][pc];
        // Entries on the interrupt/callback stack run as interrupt invocations.
        const uint32_t sp=GPR_U32(cpu,29);
        if(sp>=0x1fe0000 && sp<0x1ff0000)++irqEntries[{unsigned(phase),pc}];
        ++threadEntries[{unsigned(phase),runtime->eeScheduler().currentThreadId()}];
        estimateNative(pc,ram,cpu,phase);
        if(cursorLog.is_open() && (pc==0x1f2498 || pc==0x1f2070 || pc==0x1f22a8 || pc==0x1fe438 || pc==0x1feab0 || pc==0x1f2d00 || pc==0x1f4118)) {
            cursorLog<<samplesTaken<<','<<FramePhases::names[phase]<<','<<pc<<','<<GPR_U32(cpu,31);
            for(uint32_t a=0x357718;a<=0x357778;a+=4){uint32_t v;std::memcpy(&v,ram+a,4);cursorLog<<','<<v;}
            cursorLog<<'\n';
        }
        if(attributedPhase==phase){attribute(ram,lastPc);lastPc=pc;}
    }
    fn(ram,cpu,runtime);
}
void installTrampoline(PS2Runtime& runtime) {
    tableBase=g_ps2RecompiledFunctionTableBase;
    saved.assign(g_ps2RecompiledFunctionTableSlotCount,nullptr);
    unsigned wrapped=0;
    for(uint32_t slot=0;slot<g_ps2RecompiledFunctionTableSlotCount;++slot) {
        const uint32_t pc=tableBase+slot*4;
        if(pc>=g_ps2RecompiledFunctionTableEnd || !runtime.hasFunction(pc))continue;
        saved[slot]=runtime.lookupFunction(pc);
        if(!runtime.replaceFunction(pc,trampoline))throw std::runtime_error("render trace cannot wrap dispatch slot");
        ++wrapped;
    }
    std::cout<<"[render-trace] dispatch trampoline on "<<wrapped<<" slots\n";
}
// Inclusive wall time of selected guest functions (NIGHT100_RENDER_TRACE_TIMERS,
// comma-separated PCs), across all frames, by the phase active at entry. Time
// after a scheduler unwind inside the call is not included.
struct Timer {uint32_t pc;PS2Runtime::RecompiledFunction original;std::array<uint64_t,FramePhases::Count> ns{},calls{};};
std::vector<Timer> timers;
uint64_t timedIterations=0;
void timed(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    const uint32_t pc=cpu->pc;
    for(auto& timer:timers) {
        if(timer.pc!=pc)continue;
        if(iteration<=config.skip){timer.original(ram,cpu,runtime);return;}
        const Phase phase=FramePhases::state.current;const uint64_t start=FramePhases::wallNanoseconds();
        timer.original(ram,cpu,runtime);
        timer.ns[phase]+=FramePhases::wallNanoseconds()-start;++timer.calls[phase];return;
    }
    throw std::runtime_error("render trace timer PC mismatch");
}
void installTimers(PS2Runtime& runtime,const char* list) {
    for(const char* cursor=list;*cursor;) {
        char* end=nullptr;const uint32_t pc=uint32_t(std::strtoul(cursor,&end,16));
        if(end==cursor)break;
        if(runtime.hasFunction(pc)){timers.push_back({pc,runtime.lookupFunction(pc)});runtime.replaceFunction(pc,timed);}
        cursor=*end==','?end+1:end;
    }
    std::cout<<"[render-trace] "<<timers.size()<<" inclusive timers\n";
}
void writeTimers() {
    if(timers.empty())return;
    std::ofstream out(config.directory/"timers.csv");
    out<<"pc,phase,calls,total_ns,iterations\n";
    for(const auto& timer:timers)for(unsigned p=0;p<FramePhases::Count;++p)if(timer.calls[p])
        out<<timer.pc<<','<<FramePhases::names[p]<<','<<timer.calls[p]<<','<<timer.ns[p]<<','<<timedIterations<<'\n';
}
void flushCalls() {
    for(unsigned p=0;p<FramePhases::Count;++p) {
        for(const auto& [pc,count]:entryCounts[p])
            calls<<samplesTaken<<','<<iteration<<','<<FramePhases::names[p]<<','<<pc<<','<<count<<'\n';
        entryCounts[p].clear();
        for(const auto& [kind,bytes]:nativeBytes[p])
            native<<samplesTaken<<','<<iteration<<','<<FramePhases::names[p]<<','<<kind<<','<<bytes.calls<<','<<bytes.items<<','<<bytes.dynamicBytes<<'\n';
        nativeBytes[p].clear();
    }
    for(const auto& [key,count]:threadEntries)
        threads<<samplesTaken<<','<<iteration<<','<<FramePhases::names[key.first]<<','<<key.second<<','<<count<<'\n';
    threadEntries.clear();
    for(const auto& [key,count]:irqEntries)
        threads<<samplesTaken<<','<<iteration<<','<<FramePhases::names[key.first]<<",irq:"<<key.second<<','<<count<<'\n';
    irqEntries.clear();
}

// --- Render isolation experiment -------------------------------------------
// NIGHT100_RENDER_REVERT=1 discards, every frame after the skip, the guest RAM
// bytes that render-side phases changed (render_prep+render and
// camera_end+fx_end), except the stack and NIGHT100_RENDER_REVERT_KEEP ranges
// ("start-end" hex, comma separated). This is the "render image" semantics:
// the next update never observes render writes. An unchanged replay endpoint
// shows the update does not consume them on that clip.
struct KeepRange {uint32_t start,end;};
std::vector<KeepRange> keepRanges,onlyRanges; // NIGHT100_RENDER_REVERT_ONLY limits the revert to these
bool revertEnabled=false;
std::vector<uint8_t> revertSnapshot;
std::ofstream revertLog,revertBytes;
uint64_t revertLogFrom=~uint64_t(0);
uint64_t revertedTotal=0;
void parseRanges(const char* list,std::vector<KeepRange>& keepRanges) {
    for(const char* cursor=list;cursor && *cursor;) {
        char* end=nullptr;const uint32_t start=uint32_t(std::strtoul(cursor,&end,16));
        if(end==cursor || *end!='-')break;
        const char* next=end+1;const uint32_t stop=uint32_t(std::strtoul(next,&end,16));
        if(end==next)break;
        keepRanges.push_back({start,stop});cursor=*end==','?end+1:end;
    }
}
// NIGHT100_RENDER_REVERT_INFRA=1 also keeps every 4 KiB heap page within 16 KiB
// of a heap pointer in the SDK block 0x357718-0x357778 (packet cursors and
// DMA queue) sampled at SDK packet routine entries. The sky engine block the
// words at 0x3577f8 point to is not kept: keeping it while discarding the
// rest of render's writes leaves RenderWare inconsistent (hangs replay 4).
std::vector<uint8_t> infraPages;
struct InfraHook {uint32_t pc;PS2Runtime::RecompiledFunction original;};
std::vector<InfraHook> infraHooks;
void noteCursors(const uint8_t* ram) {
    for(uint32_t a=0x357718;a<=0x357778;a+=4) {
        uint32_t v;std::memcpy(&v,ram+a,4);
        if(v<0x3b3500+0x4000 || v>=0x1ff0000-0x4000)continue;
        for(uint32_t page=(v-0x4000)>>12;page<=((v+0x4000)>>12) && page<infraPages.size();++page)infraPages[page]=1;
    }
}
void infraEntry(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    for(const auto& hook:infraHooks)if(hook.pc==cpu->pc){noteCursors(ram);hook.original(ram,cpu,runtime);return;}
    throw std::runtime_error("render trace infra hook PC mismatch");
}
bool kept(uint32_t address) {
    if(address>=0x1ff0000)return true;
    if(!infraPages.empty() && infraPages[address>>12])return true;
    for(const auto& range:keepRanges)if(address>=range.start && address<range.end)return true;
    if(onlyRanges.empty())return false;
    for(const auto& range:onlyRanges)if(address>=range.start && address<range.end)return false;
    return true;
}
void revertSince(uint8_t* ram,Phase window) {
    uint64_t reverted=0,keptBytes=0;std::array<uint64_t,5> regions{};
    const uint8_t* old=revertSnapshot.data();
    for(uint32_t offset=0;offset<PS2_RAM_SIZE;offset+=8) {
        uint64_t a,b;std::memcpy(&a,ram+offset,8);std::memcpy(&b,old+offset,8);
        if(a==b)continue;
        for(uint32_t i=0;i<8;++i) {
            const uint32_t at=offset+i;if(ram[at]==old[at])continue;
            if(kept(at)){++keptBytes;continue;}
            if(iteration>=revertLogFrom)revertBytes<<iteration<<','<<FramePhases::names[window]<<','<<at<<'\n';
            ram[at]=old[at];++reverted;
            ++regions[at<0x357c00?0:at<0x3b3500?1:at<0x1ff0000?2:3];
        }
    }
    revertedTotal+=reverted;
    revertLog<<iteration<<','<<FramePhases::names[window]<<','<<reverted<<','<<keptBytes<<','<<regions[0]<<','<<regions[1]<<','<<regions[2]<<'\n';
}
uint64_t revertUntil=~uint64_t(0);
void revertTransition(Phase ended,Phase next,uint8_t* ram) {
    if(iteration<=config.skip || iteration>revertUntil)return;
    const bool windowOpen=next==FramePhases::RenderPrep || next==FramePhases::CameraEnd;
    const bool windowClose=(ended==FramePhases::Render || ended==FramePhases::RenderPrep) ?
        next==FramePhases::RenderEnd : (ended==FramePhases::FxEnd || ended==FramePhases::CameraEnd) && next==FramePhases::Wait;
    if(windowClose)revertSince(ram,ended);
    if(windowOpen)std::memcpy(revertSnapshot.data(),ram,PS2_RAM_SIZE);
}

// --- Dirty-line probe (only in the force-included probe build) -------------
// Counts 64-byte lines stored to by the update side [zLaughUpdate, xCameraBegin)
// and the render side [xCameraBegin, next zLaughUpdate) with NIGHT100_DIRTY_COUNT=1.
#ifdef NIGHT100_DIRTY_PROBE
bool dirtyCounting=false;
std::ofstream dirtyLog;
void dirtyTransition(Phase next) {
    if(next!=FramePhases::PostPresent && next!=FramePhases::RenderPrep)return;
    uint64_t lines=0;for(uint8_t v:night100DirtyLines)lines+=v;
    std::memset(night100DirtyLines,0,sizeof(night100DirtyLines));
    if(iteration>config.skip)dirtyLog<<iteration<<','<<(next==FramePhases::RenderPrep?"update_side":"render_side")<<','<<lines<<'\n';
    if(iteration%60==0)dirtyLog.flush();
}
#endif

// --- Phase boundaries ------------------------------------------------------
void endPhase(uint8_t* ram,PS2Runtime* runtime,Phase phase) {
    if(config.faults){unprotectAll();recordAccess(ram,phase);}
    if(attributedPhase==phase){attribute(ram,lastPc);attributedPhase=FramePhases::Count;}
    const auto totals=diffAndRecord(ram,phase);
    uint8_t* spr=runtime->memory().getScratchpad();uint64_t sprBytes=0;
    for(size_t i=0;i<scratchSnapshot.size();++i)if(spr[i]!=scratchSnapshot[i]){++sprBytes;scratchSnapshot[i]=spr[i];}
    scratch<<samplesTaken<<','<<iteration<<','<<FramePhases::names[phase]<<','<<sprBytes<<'\n';
    summary<<samplesTaken<<','<<iteration<<','<<FramePhases::names[phase]<<','<<totals.bytes<<','<<totals.runs<<','<<totals.pages<<'\n';
}
void beginSample(uint8_t* ram,PS2Runtime* runtime) {
    gameThread=pthread_self(); // the guest executor thread, not the ELF loader
    // NIGHT100_RENDER_TRACE_WORDS=start-end (hex): 32-bit words at each sample start.
    if(const char* words=std::getenv("NIGHT100_RENDER_TRACE_WORDS")) {
        char* end=nullptr;const uint32_t start=uint32_t(std::strtoul(words,&end,16));
        const uint32_t stop=*end=='-'?uint32_t(std::strtoul(end+1,nullptr,16)):start+4;
        std::ofstream out(config.directory/"words.csv",std::ios::app);
        for(uint32_t a=start&~3u;a<stop && a<=PS2_RAM_SIZE-4;a+=4){uint32_t v;std::memcpy(&v,ram+a,4);out<<samplesTaken<<','<<a<<','<<v<<'\n';}
    }
    std::memcpy(snapshot.data(),ram,PS2_RAM_SIZE);
    std::memcpy(scratchSnapshot.data(),runtime->memory().getScratchpad(),scratchSnapshot.size());
    sampleActive=true;
    if(config.faults)protectAll();
}
void finishSample() {
    sampleActive=false;flushCalls();writeTimers();++samplesTaken;
    for(auto* stream:{&runs,&pages,&access,&calls,&threads,&native,&scratch,&summary,&writers})stream->flush();
    std::cout<<"[render-trace] sample "<<samplesTaken<<" iteration "<<iteration<<'\n';
}
std::ofstream open(const char* name,const char* header) {
    std::ofstream out(config.directory/name);
    if(!out)throw std::runtime_error(std::string("render trace cannot create ")+name);
    out<<header<<'\n';return out;
}
uint64_t number(const char* name,uint64_t fallback) {
    const char* value=std::getenv(name);return value && *value?std::strtoull(value,nullptr,10):fallback;
}
}
bool install(PS2Runtime& runtime) {
    const char* directory=std::getenv("NIGHT100_RENDER_TRACE");
    if(!directory || !*directory)return false;
    config.directory=directory;std::filesystem::create_directories(config.directory);
    config.skip=number("NIGHT100_RENDER_TRACE_SKIP",60);config.every=std::max<uint64_t>(1,number("NIGHT100_RENDER_TRACE_EVERY",16));
    config.samples=number("NIGHT100_RENDER_TRACE_SAMPLES",24);
    config.faults=number("NIGHT100_RENDER_TRACE_FAULTS",0)!=0;config.attribute=number("NIGHT100_RENDER_TRACE_ATTRIBUTE",0)!=0;config.calls=number("NIGHT100_RENDER_TRACE_CALLS",1)!=0;
    snapshot.resize(PS2_RAM_SIZE);scratchSnapshot.resize(16*1024);
    runs=open("runs.csv","sample,iteration,phase,start,end,changed_bytes");
    pages=open("pages.csv","sample,iteration,phase,page,changed_bytes");
    access=open("access.csv","sample,iteration,phase,start,bytes,read_game,write_game,read_other,write_other");
    calls=open("calls.csv","sample,iteration,phase,pc,entries");
    threads=open("threads.csv","sample,iteration,phase,thread,entries");
    native=open("native.csv","sample,iteration,phase,kind,calls,items,dynamic_bytes");
    scratch=open("scratchpad.csv","sample,iteration,phase,changed_bytes");
    summary=open("summary.csv","sample,iteration,phase,changed_bytes,runs,pages_4k");
    writers=open("writers.csv","sample,iteration,phase,writer_pc,start,end,changed_bytes");
    shadow.resize(PS2_RAM_SIZE);
    if(config.faults) {
        uint8_t* ram=runtime.memory().getRDRAM();
        hostPage=size_t(sysconf(_SC_PAGESIZE));
        guardLow=(reinterpret_cast<uintptr_t>(ram)+hostPage-1)&~(hostPage-1);
        guardHigh=(reinterpret_cast<uintptr_t>(ram)+PS2_RAM_SIZE)&~(hostPage-1);
        guardPages=(guardHigh-guardLow)/hostPage;
        pageState=std::make_unique<std::atomic<uint8_t>[]>(guardPages);pageFlags=std::make_unique<std::atomic<uint8_t>[]>(guardPages);
        struct sigaction action{};action.sa_sigaction=onFault;action.sa_flags=SA_SIGINFO|SA_NODEFER;sigemptyset(&action.sa_mask);
        sigaction(SIGBUS,&action,&previousBus);sigaction(SIGSEGV,&action,&previousSegv);
        std::cout<<"[render-trace] fault tracking "<<guardPages<<" host pages of "<<hostPage<<" bytes, ram offset "
                 <<(guardLow-reinterpret_cast<uintptr_t>(ram))<<'\n';
    }
    if(const char* list=std::getenv("NIGHT100_RENDER_TRACE_TIMERS"))installTimers(runtime,list);
    if(number("NIGHT100_RENDER_TRACE_CURSORS",0))cursorLog=open("cursors.csv","sample,phase,pc,ra,w357718..w357778");
    checkpointImages=number("NIGHT100_RENDER_TRACE_CHECKPOINTS",0)>1;
    exitAfterReplay=number("NIGHT100_RENDER_TRACE_EXIT_AFTER_REPLAY",0)!=0;
    dumpIteration=number("NIGHT100_RENDER_TRACE_DUMP",0);
    if(number("NIGHT100_RENDER_TRACE_CHECKPOINTS",0))
        checkpoints=open("checkpoints.csv",checkpointImages?"iteration,player_valid,x,y,z,health,snacks,keys,control,camera_valid,c0,c1,c2,c3,c4,c5,c6,c7,c8,c9,c10,c11,image":
            "iteration,player_valid,x,y,z,health,snacks,keys,control,camera_valid,c0,c1,c2,c3,c4,c5,c6,c7,c8,c9,c10,c11");
#ifdef NIGHT100_DIRTY_PROBE
    dirtyCounting=number("NIGHT100_DIRTY_COUNT",0)!=0;
    if(dirtyCounting)dirtyLog=open("dirty.csv","iteration,window,lines_64b");
#endif
    revertEnabled=number("NIGHT100_RENDER_REVERT",0)!=0;revertUntil=number("NIGHT100_RENDER_REVERT_UNTIL",~uint64_t(0));
    revertLogFrom=number("NIGHT100_RENDER_REVERT_LOG_FROM",~uint64_t(0));
    if(revertEnabled && revertLogFrom!=~uint64_t(0))revertBytes=open("revert-bytes.csv","iteration,window_end_phase,address");
    if(revertEnabled) {
        parseRanges(std::getenv("NIGHT100_RENDER_REVERT_KEEP"),keepRanges);parseRanges(std::getenv("NIGHT100_RENDER_REVERT_ONLY"),onlyRanges);revertSnapshot.resize(PS2_RAM_SIZE);
        revertLog=open("revert.csv","iteration,window_end_phase,reverted_bytes,kept_bytes,data,bss,heap");
        if(number("NIGHT100_RENDER_REVERT_INFRA",0)) {
            infraPages.assign(PS2_RAM_SIZE>>12,0);
            for(uint32_t pc:{0x1f2498u,0x1f2070u,0x1f22a8u,0x1fe438u,0x1feab0u,0x1f2d00u,0x1f4118u}) {
                const auto original=runtime.lookupFunction(pc);
                if(!original || !runtime.replaceFunction(pc,infraEntry))throw std::runtime_error("render trace cannot hook SDK packet routine");
                infraHooks.push_back({pc,original});
            }
        }
        std::cout<<"[render-trace] reverting render-side writes with "<<keepRanges.size()<<" keep ranges\n";
    }
    if(config.calls)installTrampoline(runtime);
    std::cout<<"[render-trace] "<<config.directory.string()<<" skip="<<config.skip<<" every="<<config.every<<" samples="<<config.samples<<'\n';
    enabled=true;return true;
}
void transition(Phase ended,Phase next,uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(!enabled || ended==next)return;
    // NIGHT100_RENDER_TRACE_DUMP=<iteration>: raw guest RAM as the render
    // phases enter (xCameraBegin, zSceneRender) in that zGameLoop iteration.
    if(dumpIteration && iteration==dumpIteration && (next==FramePhases::RenderPrep || next==FramePhases::Render)) {
        std::ofstream out(config.directory/("ram-"+std::to_string(iteration)+"-"+FramePhases::names[next]+".bin"),std::ios::binary);
        out.write(reinterpret_cast<const char*>(ram),PS2_RAM_SIZE);
    }
    if(revertEnabled)revertTransition(ended,next,ram);
#ifdef NIGHT100_DIRTY_PROBE
    if(dirtyCounting)dirtyTransition(next);
#endif
    if(sampleActive)endPhase(ram,runtime,ended);
    if(next!=FramePhases::PostPresent) {
        if(sampleActive && attributing(next))startAttribution(ram,next,cpu->pc);
        if(sampleActive && config.faults)protectAll();
        return;
    }
    // A zGameLoop iteration starts at zLaughUpdate, just after a completed frame.
    if(sampleActive)finishSample();
    // NIGHT100_RENDER_TRACE_EXIT_AFTER_REPLAY=1: stop once a replay has verified,
    // so diagnostic windows close with the clip instead of at the deadline.
    if(exitAfterReplay) {
        const bool playing=ReplayHarness::uiState().playing;
        if(sawReplay && !playing)runtime->requestStop();
        sawReplay|=playing;
    }
    if(checkpoints.is_open()) {
        // Gameplay state after the previous iteration; the comparison
        // baseline for render-isolation runs (positions at full precision).
        const auto point=ReplayCatalog::capture(ram,PS2_RAM_SIZE);
        checkpoints<<iteration<<','<<point.playerValid<<std::setprecision(9);
        for(float v:point.position)checkpoints<<','<<v;
        checkpoints<<','<<point.health<<','<<point.snacks<<','<<point.keys<<','<<point.control<<','<<point.cameraValid;
        for(float v:point.camera)checkpoints<<','<<v;
        if(checkpointImages) {
            // FNV-1a over the latched completed frame (the image this iteration presented).
            std::vector<uint8_t> pixels;uint32_t width=0,height=0;uint64_t hash=1469598103934665603ull;
            if(runtime->gs().copyLatchedHostPresentationFrame(pixels,width,height))
                for(uint8_t byte:pixels){hash^=byte;hash*=1099511628211ull;}
            else hash=0;
            checkpoints<<','<<width<<'x'<<height<<':'<<std::hex<<hash<<std::dec;
        }
        checkpoints<<'\n';
        if(iteration%60==0)checkpoints.flush();
    }
    ++iteration;if(iteration>config.skip)++timedIterations;
    if(!timers.empty() && iteration%120==0)writeTimers();
    if(revertEnabled && iteration%60==0)revertLog.flush();
    if(samplesTaken<config.samples && iteration>=config.skip && (iteration-config.skip)%config.every==0)beginSample(ram,runtime);
}
}
