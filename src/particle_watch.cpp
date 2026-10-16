#include "particle_watch.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include <array>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <vector>

// Attribution: a dispatch trampoline tracks the guest function executing on the
// game thread (restored when a nested call returns), and the pool's host pages
// are protected again at every function entry/exit that follows an access. The
// first access per page per function activation faults and is attributed with
// its read/write direction (ESR WnR / x86 error code) and the node field offset.
// Later accesses in the same activation and page are not individually seen.
namespace ParticleWatch {
namespace {
constexpr uint32_t poolLo=0x379200,poolHi=0x3a0700,nodeSize=0x50,deadHead=0x358290;
uint8_t* ram=nullptr;
size_t hostPage=0;
struct Range {uintptr_t low=0,high=0;};
std::array<Range,2> ranges;
std::atomic<bool> armed{false},dirty{false};
pthread_t gameThread;
std::atomic<bool> gameThreadKnown{false};
uint64_t entriesOffThread=0;
uint32_t currentPc=0,currentCaller=0;
struct Slot {uint32_t pc=0,caller=0;uint64_t reads=0,writes=0,dead=0,other=0;std::array<uint64_t,2> readMask{},writeMask{};bool used=false;};
std::vector<Slot> slots(1u<<15);
uint64_t dropped=0,faults=0;
std::string directory;
struct sigaction previousBus{},previousSegv{};
std::vector<PS2Runtime::RecompiledFunction> saved;
uint32_t tableBase=0;

Slot* slot(uint32_t pc,uint32_t caller) {
    size_t i=(size_t(pc)*2654435761u ^ caller*40503u)&(slots.size()-1);
    for(size_t probe=0;probe<slots.size();++probe,i=(i+1)&(slots.size()-1)) {
        auto& s=slots[i];
        if(!s.used){s.used=true;s.pc=pc;s.caller=caller;return &s;}
        if(s.pc==pc && s.caller==caller)return &s;
    }
    return nullptr;
}
void protect(bool on) {
    for(const auto& r:ranges)
        if(mprotect(reinterpret_cast<void*>(r.low),r.high-r.low,on?PROT_NONE:PROT_READ|PROT_WRITE))
            throw std::runtime_error("particle watch mprotect failed");
}
void rearm(){if(dirty.exchange(false))protect(true);}
void chain(int signal,siginfo_t* info,void* context) {
    const struct sigaction& previous=signal==SIGBUS?previousBus:previousSegv;
    if(previous.sa_flags&SA_SIGINFO){previous.sa_sigaction(signal,info,context);return;}
    if(previous.sa_handler==SIG_IGN)return;
    if(previous.sa_handler==SIG_DFL){::signal(signal,SIG_DFL);raise(signal);return;}
    previous.sa_handler(signal);
}
void onFault(int signal,siginfo_t* info,void* context) {
    const uintptr_t address=reinterpret_cast<uintptr_t>(info->si_addr);
    const Range* hit=nullptr;
    for(const auto& r:ranges)if(address>=r.low && address<r.high)hit=&r;
    if(!armed.load(std::memory_order_acquire) || !hit){chain(signal,info,context);return;}
    bool write=false;
#if defined(__aarch64__)
    write=((static_cast<ucontext_t*>(context)->uc_mcontext->__es.__esr)>>6)&1u;
#elif defined(__x86_64__)
    write=(static_cast<ucontext_t*>(context)->uc_mcontext->__es.__err&2u)!=0;
#endif
    const uintptr_t page=address&~uintptr_t(hostPage-1);
    mprotect(reinterpret_cast<void*>(page),hostPage,PROT_READ|PROT_WRITE);dirty.store(true);++faults;
    const uint32_t guest=uint32_t(address-reinterpret_cast<uintptr_t>(ram));
    const bool pool=guest>=poolLo && guest<poolHi,dead=guest>=deadHead && guest<deadHead+4;
    if(!pool && !dead)return;
    Slot* s=slot(currentPc,currentCaller);
    if(!s){++dropped;return;}
    if(!gameThreadKnown.load() || !pthread_equal(pthread_self(),gameThread)){++s->other;return;}
    if(dead){++s->dead;(write?s->writes:s->reads)++;return;}
    const unsigned offset=(guest-poolLo)%nodeSize;
    auto& mask=write?s->writeMask:s->readMask;mask[offset/64]|=uint64_t(1)<<(offset%64);
    (write?s->writes:s->reads)++;
}
void trampoline(uint8_t* r,R5900Context* cpu,PS2Runtime* runtime) {
    const uint32_t pc=cpu->pc;
    const auto fn=saved[(pc-tableBase)>>2];
    // The EE executes guest code on one host thread, not the loader thread
    // that installed the hooks; latch it at the first dispatched entry.
    if(!gameThreadKnown.load(std::memory_order_acquire)){gameThread=pthread_self();gameThreadKnown.store(true,std::memory_order_release);}
    if(!pthread_equal(pthread_self(),gameThread)){++entriesOffThread;fn(r,cpu,runtime);return;}
    const uint32_t outerPc=currentPc,outerCaller=currentCaller;
    currentPc=pc;currentCaller=GPR_U32(cpu,31);rearm();
    fn(r,cpu,runtime);
    currentPc=outerPc;currentCaller=outerCaller;rearm();
}
void report() {
    armed.store(false);protect(false);
    std::filesystem::create_directories(directory);
    std::ofstream out(std::filesystem::path(directory)/"particle-access.csv");
    out<<"pc,caller,reads,writes,dead_head,other_thread,read_offsets,write_offsets\n";
    auto offsets=[](const std::array<uint64_t,2>& mask) {
        std::ostringstream s;bool first=true;
        for(unsigned o=0;o<nodeSize;++o)if(mask[o/64]>>(o%64)&1){s<<(first?"":" ")<<std::hex<<"0x"<<o;first=false;}
        return s.str();
    };
    unsigned rows=0;
    for(const auto& s:slots)if(s.used && (s.reads || s.writes || s.other)) {
        out<<std::hex<<"0x"<<s.pc<<",0x"<<s.caller<<std::dec<<','<<s.reads<<','<<s.writes<<','<<s.dead<<','<<s.other
           <<','<<offsets(s.readMask)<<','<<offsets(s.writeMask)<<'\n';
        ++rows;
    }
    std::cout<<"[particle-watch] faults="<<faults<<" functions="<<rows<<" dropped="<<dropped<<" off-thread-entries="<<entriesOffThread<<" -> "<<directory<<'\n';
}
}
void install(PS2Runtime& runtime) {
    const char* setting=std::getenv("NIGHT100_PARTICLE_WATCH");
    if(!setting)return;
    directory=setting;ram=runtime.memory().getRDRAM();hostPage=size_t(sysconf(_SC_PAGESIZE));
    auto range=[&](uint32_t low,uint32_t high) {
        const uintptr_t a=reinterpret_cast<uintptr_t>(ram)+low,b=reinterpret_cast<uintptr_t>(ram)+high;
        return Range{a&~uintptr_t(hostPage-1),(b+hostPage-1)&~uintptr_t(hostPage-1)};
    };
    ranges={range(poolLo,poolHi),range(deadHead,deadHead+4)};
    tableBase=g_ps2RecompiledFunctionTableBase;
    saved.assign(g_ps2RecompiledFunctionTableSlotCount,nullptr);
    unsigned wrapped=0;
    for(uint32_t index=0;index<g_ps2RecompiledFunctionTableSlotCount;++index) {
        const uint32_t pc=tableBase+index*4;
        if(pc>=g_ps2RecompiledFunctionTableEnd || !runtime.hasFunction(pc))continue;
        saved[index]=runtime.lookupFunction(pc);
        if(!runtime.replaceFunction(pc,trampoline))throw std::runtime_error("particle watch cannot wrap dispatch slot");
        ++wrapped;
    }
    struct sigaction action{};action.sa_sigaction=onFault;action.sa_flags=SA_SIGINFO|SA_NODEFER;sigemptyset(&action.sa_mask);
    sigaction(SIGBUS,&action,&previousBus);sigaction(SIGSEGV,&action,&previousSegv);
    std::atexit(report);
    armed.store(true,std::memory_order_release);protect(true);
    std::cout<<"[particle-watch] wrapping "<<wrapped<<" dispatch slots; page="<<hostPage<<'\n';
}
}
