#include "stall_watchdog.h"
#include <switch.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

extern "C" [[gnu::noinline]] void night100SwitchStallWatchdogAnchor() noexcept {
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

namespace SwitchStallWatchdog {
namespace {
constexpr uint64_t hz=19200000,staleTicks=5*hz;
constexpr Result invalidSetup=0xFFFFFFFFu;
struct SetupFailure {Result result;};
void check(Result result) {if(R_FAILED(result))throw SetupFailure{result};}
// No locale, heap, stream, or stdio dependencies even after the capture: the
// original stall may itself involve a libc/application lock held by a peer.
struct Text {
    std::array<char,2048> bytes{};
    size_t used=0;
    void add(const char* s) noexcept {while(*s && used<bytes.size())bytes[used++]=*s++;}
    void number(uint64_t n,unsigned radix=10) noexcept {
        char digits[32];size_t count=0;
        do {digits[count++]="0123456789abcdef"[n%radix];n/=radix;}while(n);
        while(count && used<bytes.size())bytes[used++]=digits[--count];
    }
    void hex(uint64_t n) noexcept {add("0x");number(n,16);}
};
struct Observation {
    Handle handle=0;
    bool attempted=false,available=false;
    Result pause=0,context=0,resume=0;
    uint32_t resumeRetries=0;
    uint64_t pc=0,lr=0,sp=0;
};
class ResumePeer {
    Handle handle;
    Observation& result;
    bool paused=true;
public:
    ResumePeer(Handle h,Observation& r) noexcept:handle(h),result(r){}
    ~ResumePeer() noexcept {resume();}
    void resume() noexcept {
        if(!paused)return;
        result.resume=svcSetThreadActivity(handle,ThreadActivity_Runnable);
        if(R_FAILED(result.resume)) {
            ++result.resumeRetries;
            result.resume=svcSetThreadActivity(handle,ThreadActivity_Runnable);
            if(R_FAILED(result.resume))svcExitProcess();
        }
        paused=false;
    }
};
void capture(Observation& out,Handle self) noexcept {
    if(!out.handle || out.handle==self)return;
    ThreadContext context;
    out.attempted=true;
    out.pause=svcSetThreadActivity(out.handle,ThreadActivity_Paused);
    if(R_FAILED(out.pause))return;
    {
        // From successful pause until checked resume: stack/POD and SVC only.
        ResumePeer resume{out.handle,out};
        out.context=svcGetThreadContext3(&context,out.handle);
        resume.resume();
    }
    if(R_SUCCEEDED(out.context)) {
        out.available=true;out.pc=context.pc.x;out.lr=context.lr;out.sp=context.sp;
    }
}
void row(Text& text,const char* role,const Observation& item) noexcept {
    text.add(role);text.add(" handle=");text.hex(item.handle);
    text.add(" attempted=");text.number(item.attempted);
    text.add(" available=");text.number(item.available);
    text.add(" pc=");text.hex(item.pc);text.add(" lr=");text.hex(item.lr);text.add(" sp=");text.hex(item.sp);
    text.add(" pause_result=");text.hex(item.pause);text.add(" context_result=");text.hex(item.context);
    text.add(" resume_result=");text.hex(item.resume);text.add(" resume_retries=");text.number(item.resumeRetries);text.add("\n");
}
}
struct Watchdog::Impl {
    std::atomic<bool> stop{false},foreground{true};
    std::atomic<uint64_t> heartbeat{0};
    std::atomic<Handle> producer{0},audio{0};
    Handle main=0;
    Service proxy{};
    FsFileSystem sd{};
    FsFile file{};
    s64 offset=0;
    bool armed=false;
    std::thread worker;
    ~Impl() {
        stop.store(true,std::memory_order_release);
        if(worker.joinable())worker.join();
        if(serviceIsActive(&file.s))fsFileClose(&file);
        if(serviceIsActive(&sd.s))fsFsClose(&sd);
        if(serviceIsActive(&proxy))serviceClose(&proxy);
    }
    Result write(const Text& text) noexcept {
        const Result result=fsFileWrite(&file,offset,text.bytes.data(),text.used,FsWriteOption_Flush);
        if(R_SUCCEEDED(result))offset+=text.used;
        return result;
    }
    void initialize(const std::string& root,const std::string& id) {
        if(root.empty() || root[0]!='/' || root.size()+id.size()+32>=FS_MAX_PATH ||
           root.find("..")!=std::string::npos || root.find(':')!=std::string::npos ||
           root.find('\\')!=std::string::npos || id.empty() || id.size()>64)throw SetupFailure{invalidSetup};
        for(unsigned char c:id)
            if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'))throw SetupFailure{invalidSetup};
        const auto* self=threadGetSelf();main=self?self->handle:0;
        // Separate non-domain fsp-srv connection. libnx fsFs*/fsFile* bypass
        // its global FS SessionMgr for objects whose session differs from the
        // global fsGetServiceSession(). No devoptab/Session logger is involved.
        check(smGetService(&proxy,"fsp-srv"));
        const u64 pidPlaceholder=0;
        check(serviceDispatchIn(&proxy,1,pidPlaceholder,.in_send_pid=true));
        check(serviceDispatch(&proxy,18,.out_num_objects=1,.out_objects=&sd.s));
        std::array<char,FS_MAX_PATH> path{};
        const std::string name=root+"/logs/stall-"+id+".log";
        for(size_t n=0;n<name.size();++n)path[n]=name[n];
        fsFsCreateFile(&sd,path.data(),0,0);
        check(fsFsOpenFile(&sd,path.data(),FsOpenMode_Write|FsOpenMode_Append,&file));
        check(fsFileGetSize(&file,&offset));
        Text text;text.add("[stall-watchdog] ");
        if(!main || !envIsSyscallHinted(0x32) || !envIsSyscallHinted(0x33)) {
            text.add("disabled=context_svcs_or_main_handle_unavailable\n");check(write(text));return;
        }
        text.add("armed stale_seconds=5 anchor=");
        text.hex(reinterpret_cast<uintptr_t>(&night100SwitchStallWatchdogAnchor));
        text.add(" main_handle=");text.hex(main);text.add("\n");check(write(text));
        heartbeat.store(armGetSystemTick(),std::memory_order_relaxed);
        worker=std::thread([this]{run();});armed=true;
    }
    void run() noexcept {
        const auto* self=threadGetSelf();const Handle current=self?self->handle:0;
        while(!stop.load(std::memory_order_acquire)) {
            svcSleepThread(1000000000);
            if(stop.load(std::memory_order_acquire))return;
            if(!foreground.load(std::memory_order_acquire))continue;
            const uint64_t last=heartbeat.load(std::memory_order_acquire),now=armGetSystemTick();
            if(now<last || now-last<staleTicks)continue;
            std::array<Observation,3> observations{};
            observations[0].handle=main;
            observations[1].handle=producer.load(std::memory_order_acquire);
            observations[2].handle=audio.load(std::memory_order_acquire);
            const uint64_t start=armGetSystemTick();
            for(size_t n=0;n<observations.size();++n) {
                bool duplicate=false;
                for(size_t earlier=0;earlier<n;++earlier)duplicate|=observations[n].handle==observations[earlier].handle;
                if(!duplicate)capture(observations[n],current);
            }
            // Every peer is resumed before formatting or invoking any FS IPC.
            Text text;text.add("[stall-watchdog] captured anchor=");
            text.hex(reinterpret_cast<uintptr_t>(&night100SwitchStallWatchdogAnchor));
            text.add(" heartbeat_age_ms=");text.number((now-last)/19200);
            text.add(" capture_us=");text.number((armGetSystemTick()-start)*5/96);
            text.add(" watchdog_handle=");text.hex(current);text.add("\n");
            row(text,"main",observations[0]);row(text,"producer",observations[1]);
            if(observations[2].handle)row(text,"audio",observations[2]);
            write(text);return;
        }
    }
};
Watchdog::Watchdog(bool enabled,const std::string& root,const std::string& id) noexcept {
    if(!enabled)return;
    try {impl=std::make_unique<Impl>();impl->initialize(root,id);}
    catch(const SetupFailure& failure){result=failure.result;impl.reset();}
    catch(...){result=invalidSetup;impl.reset();}
}
Watchdog::~Watchdog()=default;
void Watchdog::heartbeat(bool active) noexcept {
    if(!impl || !impl->armed)return;
    impl->heartbeat.store(armGetSystemTick(),std::memory_order_release);
    impl->foreground.store(active,std::memory_order_release);
}
void Watchdog::producerThreadReady() noexcept {
    if(!impl || !impl->armed || impl->producer.load(std::memory_order_relaxed))return;
    const auto* self=threadGetSelf();if(self)impl->producer.store(self->handle,std::memory_order_release);
}
void Watchdog::audioThreadReady(uint32_t handle) noexcept {
    if(impl && impl->armed && handle)impl->audio.store(handle,std::memory_order_release);
}
bool Watchdog::ready() const noexcept {return impl && impl->armed;}
uint32_t Watchdog::setupResult() const noexcept {return result;}
}
