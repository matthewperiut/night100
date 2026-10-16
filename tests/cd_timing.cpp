#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_stubs.h"
#include "ps2_host_backend.h"
#include "runtime/ee_scheduler.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
constexpr uint32_t entry=0x1000, resumed=0x1004;
uint64_t started=0;
std::atomic<unsigned> hostFrames{0};
bool completed=false;
void require(bool value,const char* reason) {if(!value)throw std::runtime_error(reason);}
void hostInit(PS2Runtime&,void*) {}
void hostDraw(PS2Runtime&,void*) {++hostFrames;}
void guest(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc==resumed) {
        require(GPR_U32(cpu,2)==0,"blocking Sync did not resume with completed result");
        require(runtime->eeScheduler().currentVSyncTick()-started>=10,"Stop completed before spin-down deadline");
        SET_GPR_U32(cpu,4,1);ps2_stubs::sceCdSync(ram,cpu,runtime);
        require(GPR_U32(cpu,2)==0,"Stop remained busy after blocking Sync");
        ps2_stubs::sceCdGetError(ram,cpu,runtime);
        require(GPR_U32(cpu,2)==0,"Stop manufactured a read error");
        require(hostFrames.load()>=2,"blocking Sync prevented host window updates");
        completed=true;runtime->requestStop();return;
    }
    ps2_stubs::sceCdInit(ram,cpu,runtime);
    started=runtime->eeScheduler().currentVSyncTick();
    ps2_stubs::sceCdStop(ram,cpu,runtime);
    require(GPR_U32(cpu,2)==1,"Stop command was not accepted");
    for(unsigned mode:{1u,17u}) {
        SET_GPR_U32(cpu,4,mode);ps2_stubs::sceCdSync(ram,cpu,runtime);
        require(GPR_U32(cpu,2)==1,"nonblocking Sync missed pending Stop");
    }
    ps2_stubs::sceCdStop(ram,cpu,runtime);
    require(GPR_U32(cpu,2)==0,"overlapping Stop command was accepted");
    // HLE wrappers place PC at the caller continuation before entering a stub.
    // Blocking Sync must return through that continuation after scheduler wakeup.
    cpu->pc=resumed;SET_GPR_U32(cpu,4,0);
    ps2_stubs::sceCdSync(ram,cpu,runtime);
    throw std::runtime_error("blocking Sync returned before its scheduler wait");
}
}
int main() {
    try {
        PS2Runtime runtime;
        runtime.setDebugUiCallbacks(hostInit,hostDraw,nullptr,nullptr);
        require(runtime.initialize("Night100 CD command timing regression"),"runtime initialization failed");
        SetWindowSize(160,120);SetWindowState(FLAG_WINDOW_HIDDEN);
        require(runtime.registerFunction(entry,guest) && runtime.registerFunction(resumed,guest),"synthetic EE registration failed");
        runtime.cpu().pc=entry;
        std::mutex mutex;std::condition_variable done;bool finished=false;
        std::atomic<bool> timedOut{false};
        std::thread watchdog([&] {
            std::unique_lock lock(mutex);
            if(!done.wait_for(lock,std::chrono::seconds(3),[&]{return finished;})) {
                timedOut=true;runtime.requestStop();
            }
        });
        std::exception_ptr failure;
        try {runtime.run();} catch(...) {failure=std::current_exception();}
        {std::lock_guard lock(mutex);finished=true;}done.notify_one();watchdog.join();
        if(failure)std::rethrow_exception(failure);
        require(!timedOut && completed,"CD Sync continuation did not complete");
        std::cout<<"PASS: asynchronous CD Stop, both poll modes, scheduler blocking completion, host progress and unchanged error state\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
