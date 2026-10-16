#include "telemetry.h"
#include "runtime/cpu_work_profile.h"
#include "frame_phases.h"
#include <switch.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <exception>
#include <streambuf>
#include <string_view>

namespace SwitchTelemetry {
namespace {
constexpr double ticksPerSecond=19200000.0;
class BufferedStreams final : public std::streambuf {
    std::array<char,65536> bytes{};
    size_t used=0;
    uint64_t omitted=0;
    std::mutex mutex;
    std::streamsize xsputn(const char* text,std::streamsize count) override {
        if(count<=0)return count;
        std::lock_guard lock(mutex);
        const size_t length=static_cast<size_t>(count);
        const size_t keep=std::min(length,bytes.size()-used);
        std::memcpy(bytes.data()+used,text,keep);used+=keep;omitted+=length-keep;
        return count;
    }
    int_type overflow(int_type value) override {
        if(!traits_type::eq_int_type(value,traits_type::eof())) {
            const char byte=traits_type::to_char_type(value);xsputn(&byte,1);
        }
        return traits_type::not_eof(value);
    }
    int sync() override {return 0;} // std::endl never forces an SD flush.
public:
    std::string take() {
        std::lock_guard lock(mutex);
        std::string result(bytes.data(),used);used=0;
        if(omitted){result+="\n[log] omitted "+std::to_string(omitted)+" buffered bytes\n";omitted=0;}
        return result;
    }
};
struct Sample {
    uint64_t frames=0,wallTicks=0,cpuTicks=0,cpuFrames=0;
    CpuWorkProfile::Counters phases{};
    uint64_t phaseFrames=0;
    uint64_t sampledCpuTicks=0,sampledCpuFrames=0,plainCpuTicks=0,plainCpuFrames=0;
    // Cumulative zGameLoop phase split (frame_phases.h) at completed frames.
    FramePhases::Totals framePhases{};
    uint64_t framePhaseFrames=0;
};
Sample interval(const Sample& current,const Sample& previous) {
    Sample result;
    result.frames=current.frames-previous.frames;result.wallTicks=current.wallTicks-previous.wallTicks;
    result.cpuTicks=current.cpuTicks-previous.cpuTicks;result.cpuFrames=current.cpuFrames-previous.cpuFrames;
    result.phaseFrames=current.phaseFrames-previous.phaseFrames;
    result.sampledCpuTicks=current.sampledCpuTicks-previous.sampledCpuTicks;
    result.sampledCpuFrames=current.sampledCpuFrames-previous.sampledCpuFrames;
    result.plainCpuTicks=current.plainCpuTicks-previous.plainCpuTicks;
    result.plainCpuFrames=current.plainCpuFrames-previous.plainCpuFrames;
    for(size_t n=0;n<CpuWorkProfile::phaseCount;++n) {
        result.phases.nanoseconds[n]=current.phases.nanoseconds[n]-previous.phases.nanoseconds[n];
        result.phases.calls[n]=current.phases.calls[n]-previous.phases.calls[n];
    }
    result.phases.clockReads=current.phases.clockReads-previous.phases.clockReads;
    result.phases.clockFailures=current.phases.clockFailures-previous.phases.clockFailures;
    for(size_t n=0;n<FramePhases::Count;++n) {
        result.framePhases.ns[n]=current.framePhases.ns[n]-previous.framePhases.ns[n];
        result.framePhases.cpuNs[n]=current.framePhases.cpuNs[n]-previous.framePhases.cpuNs[n];
    }
    result.framePhaseFrames=current.framePhaseFrames-previous.framePhaseFrames;
    return result;
}
struct ServicePath {
    // libnx fsFs* sends FS_MAX_PATH bytes over IPC, including for short paths.
    // Never pass a string's small allocation or an SSO stack temporary.
    std::array<char,FS_MAX_PATH> bytes{};
    explicit ServicePath(std::string_view path) {
        if(path.size()>=bytes.size())throw std::runtime_error("Switch filesystem path is too long");
        std::memcpy(bytes.data(),path.data(),path.size());
    }
    const char* data()const{return bytes.data();}
};
void directory(FsFileSystem& sd,const char* path) {
    const ServicePath ipcPath(path);
    const Result rc=fsFsCreateDirectory(&sd,ipcPath.data());
    FsDir opened{};
    if(R_FAILED(rc) && R_FAILED(fsFsOpenDirectory(&sd,ipcPath.data(),FsDirOpenMode_ReadDirs,&opened)))
        throw std::runtime_error(std::string("Cannot create Switch log directory ")+path+" create_result="+std::to_string(rc));
    if(R_FAILED(rc))fsDirClose(&opened);
}
void write(FsFileSystem& sd,const std::string& path,std::string_view data,bool append) {
    const ServicePath ipcPath(path);
    fsFsCreateFile(&sd,ipcPath.data(),0,0);
    FsFile file{};
    Result rc=fsFsOpenFile(&sd,ipcPath.data(),FsOpenMode_Write|FsOpenMode_Append,&file);
    if(R_FAILED(rc))throw std::runtime_error("Cannot open Switch log file");
    s64 offset=0;
    if(append)rc=fsFileGetSize(&file,&offset);
    else rc=fsFileSetSize(&file,0);
    if(R_SUCCEEDED(rc))rc=fsFileWrite(&file,offset,data.data(),data.size(),FsWriteOption_Flush);
    fsFileClose(&file);
    if(R_FAILED(rc))throw std::runtime_error("Cannot flush Switch log file");
}
}
struct Session::Impl {
    BufferedStreams buffer;
    std::streambuf* out=std::cout.rdbuf();std::streambuf* error=std::cerr.rdbuf();
    std::ostream* oldTie=std::cerr.tie();std::ios_base::fmtflags errorFlags=std::cerr.flags();
    FsFileSystem sd{};bool opened=false,finished=false,reportedReady=false;
    std::string filename;
    uint64_t begin=armGetSystemTick(),lastFlush=begin,flushTicks;
    Sample local{},published{},previousPublication{};
    uint64_t firstWall=0,previousCpu=0;bool previousAvailable=false;
    uint64_t hudPreviousWall=0;
    double hudWallAverage=0,hudCpuAverage=0;
    std::atomic<double> hudFps{0},hudCpuMs{-1};
    bool cpuProfile=false,sampling=false;
    std::atomic<uint64_t> completed{0};
    std::atomic<uint64_t> lastFrameWall{0};
    std::mutex sampleMutex;
    std::function<std::string()> summaryProvider;
    // Serializes every flush and its shared FsFileSystem session. Any thread may stage.
    std::mutex flushMutex;
    Impl(const std::string& id,double interval,const std::string& root,bool profile):flushTicks(0),cpuProfile(profile) {
        if(id.empty() || id.size()>64 || !std::all_of(id.begin(),id.end(),[](unsigned char c){return std::isalnum(c)||c=='_'||c=='-';}))
            throw std::runtime_error("Invalid Switch log ID");
        if(!std::isfinite(interval)||interval<5||interval>300)throw std::runtime_error("Log interval must be 5..300 seconds");
        flushTicks=uint64_t(interval*ticksPerSecond);
        if(R_FAILED(fsOpenSdCardFileSystem(&sd)))throw std::runtime_error("Cannot open Switch SD filesystem for diagnostics");
        opened=true;
        try {
            for(size_t end=root.find('/',1);end!=std::string::npos;end=root.find('/',end+1))
                directory(sd,root.substr(0,end).c_str());
            directory(sd,root.c_str());directory(sd,(root+"/logs").c_str());
            filename=root+"/logs/run-"+id+".log";
            const ServicePath logPath(filename),volumePath("/");
            FsFile existing{};
            if(R_SUCCEEDED(fsFsOpenFile(&sd,logPath.data(),FsOpenMode_Read,&existing))) {
                fsFileClose(&existing);throw std::runtime_error("Switch log already exists; choose another --log-id");
            }
            s64 free=0;uint64_t memory=0,cpuMask=0,threadMask=0;s32 preferredCore=0;
            const bool haveFree=R_SUCCEEDED(fsFsGetFreeSpace(&sd,volumePath.data(),&free));
            const bool haveMemory=R_SUCCEEDED(svcGetInfo(&memory,InfoType_TotalMemorySize,CUR_PROCESS_HANDLE,0));
            const bool haveCpuMask=R_SUCCEEDED(svcGetInfo(&cpuMask,InfoType_CoreMask,CUR_PROCESS_HANDLE,0));
            const bool haveThreadMask=R_SUCCEEDED(svcGetThreadCoreMask(&preferredCore,&threadMask,CUR_THREAD_HANDLE));
            std::string boot="NIGHT100_SWITCH_BOOT applet_type="+std::to_string(int(appletGetAppletType()));
            if(haveFree)boot+=" sd_free_bytes="+std::to_string(free);
            if(haveMemory)boot+=" process_memory_limit="+std::to_string(memory);
            if(haveCpuMask)boot+=" allowed_cpu_mask="+std::to_string(cpuMask);
            if(haveThreadMask)boot+=" main_thread_cpu_mask="+std::to_string(threadMask);
            write(sd,filename,boot+"\n",false);
            write(sd,root+"/logs/latest.txt",filename+"\n",false);
        }catch(...){fsFsClose(&sd);opened=false;throw;}
        std::cerr.tie(nullptr);std::cerr.unsetf(std::ios_base::unitbuf);
        std::cout.rdbuf(&buffer);std::cerr.rdbuf(&buffer);
    }
    ~Impl() {
        std::cout.rdbuf(out);std::cerr.rdbuf(error);std::cerr.tie(oldTie);std::cerr.flags(errorFlags);
        if(opened)fsFsClose(&sd);
    }
    void flush(const std::string& marker) {
        Sample sample;{std::lock_guard lock(sampleMutex);sample=published;}
        std::ostringstream text;text.precision(6);
        text<<buffer.take()<<"\n[stage] "<<marker<<" elapsed_s="<<double(armGetSystemTick()-begin)/ticksPerSecond
            <<" frames="<<completed.load(std::memory_order_relaxed);
        const auto lastFrame=lastFrameWall.load(std::memory_order_acquire);
        const double idle=double(armGetSystemTick()-(lastFrame?lastFrame:begin))/ticksPerSecond;
        text<<" frame_idle_s="<<idle;
        if(idle>=5)text<<" game_fps=0";
        else if(sample.frames && sample.wallTicks)text<<" game_fps="<<double(sample.frames)*ticksPerSecond/double(sample.wallTicks);
        if(sample.cpuFrames)text<<" game_cpu_ms="<<double(sample.cpuTicks)*1000/(ticksPerSecond*double(sample.cpuFrames));
        else text<<" game_cpu_ms=unavailable";
        if(cpuProfile) {
            text<<" cpu_profile_frames="<<sample.phaseFrames;
            constexpr std::array<const char*,CpuWorkProfile::phaseCount> names{"other","iop","spu","graphics"};
            if(sample.phaseFrames)for(size_t n=0;n<names.size();++n)
                text<<" cpu_"<<names[n]<<"_ms="<<double(sample.phases.nanoseconds[n])/(1000000.0*sample.phaseFrames);
            text<<" profile_clock_reads="<<sample.phases.clockReads<<" profile_clock_failures="<<sample.phases.clockFailures;
            if(sample.sampledCpuFrames)text<<" sampled_frame_cpu_ms="<<double(sample.sampledCpuTicks)*1000/(ticksPerSecond*sample.sampledCpuFrames);
            if(sample.plainCpuFrames)text<<" unprofiled_frame_cpu_ms="<<double(sample.plainCpuTicks)*1000/(ticksPerSecond*sample.plainCpuFrames);
        }
        if(sample.framePhaseFrames) {
            // Per-completed-frame means of the zGameLoop split; present includes the display wait.
            const double frames=double(sample.framePhaseFrames)*1e6;
            for(size_t n=0;n<FramePhases::Count;++n)text<<" phase_"<<FramePhases::names[n]<<"_ms="<<double(sample.framePhases.ns[n])/frames;
            for(const auto phase:FramePhases::cpuPhases)text<<" phase_"<<FramePhases::names[phase]<<"_cpu_ms="<<double(sample.framePhases.cpuNs[phase])/frames;
        }
        if(summaryProvider)text<<' '<<summaryProvider();
        text<<'\n';write(sd,filename,text.str(),true);lastFlush=armGetSystemTick();
    }
};
namespace {
std::atomic<Session*> activeSession{nullptr};
std::terminate_handler previousTerminate=nullptr;
[[noreturn]] void terminateWithRecord() {
    SwitchTelemetry::finishCurrent("NIGHT100_SWITCH_FATAL std::terminate");
    if(previousTerminate)previousTerminate();
    std::abort();
}
}
Session::Session(const std::string& id,double interval,const std::string& root,bool profile):impl(std::make_unique<Impl>(id,interval,root,profile)){
    activeSession.store(this,std::memory_order_release);
    previousTerminate=std::set_terminate(terminateWithRecord);
}
Session::~Session(){
    Session* expected=this;
    activeSession.compare_exchange_strong(expected,nullptr,std::memory_order_acq_rel);
    if(!impl->finished)try{finish("shutdown");}catch(...) {}
}
void stageCurrent(const std::string& text) noexcept {
    Session* session=activeSession.load(std::memory_order_acquire);
    if(session)try{session->stage(text);}catch(...) {}
}
void finishCurrent(const std::string& text) noexcept {
    Session* session=activeSession.load(std::memory_order_acquire);
    if(session)try{session->finish(text);}catch(...) {}
}
void Session::stage(const std::string& value){std::lock_guard lock(impl->flushMutex);impl->flush(value);}
void Session::poll() {
    std::lock_guard lock(impl->flushMutex);
    if(!impl->reportedReady && impl->completed.load(std::memory_order_acquire)) {
        impl->reportedReady=true;impl->flush("NIGHT100_SWITCH_FRAME_READY");
    } else if(armGetSystemTick()-impl->lastFlush>=impl->flushTicks)impl->flush("metrics");
}
void Session::setSummaryProvider(std::function<std::string()> provider){impl->summaryProvider=std::move(provider);}
void Session::finish(const std::string& value){std::lock_guard lock(impl->flushMutex);if(!impl->finished){
    {std::lock_guard lock(impl->sampleMutex);
        if(impl->local.frames>impl->previousPublication.frames)
            impl->published=interval(impl->local,impl->previousPublication);}
    impl->flush(value);impl->finished=true;
}}
void Session::completedFrame() {
    const uint64_t wall=armGetSystemTick();uint64_t cpu=0;
    const bool wasSampling=impl->sampling;
    if(impl->sampling) {
        const auto phases=CpuWorkProfile::snapshotWork();
        CpuWorkProfile::setSampling(false);impl->sampling=false;
        for(size_t n=0;n<CpuWorkProfile::phaseCount;++n) {
            impl->local.phases.nanoseconds[n]+=phases.nanoseconds[n];
            impl->local.phases.calls[n]+=phases.calls[n];
        }
        impl->local.phases.clockReads+=phases.clockReads;
        impl->local.phases.clockFailures+=phases.clockFailures;
        ++impl->local.phaseFrames;
    }
    impl->lastFrameWall.store(wall,std::memory_order_release);
    const Result cpuResult=svcGetInfo(&cpu,InfoType_ThreadTickCount,CUR_THREAD_HANDLE,TickCountInfo_Total);
    const bool available=R_SUCCEEDED(cpuResult);
    if(impl->hudPreviousWall && wall>impl->hudPreviousWall) {
        const uint64_t delta=wall-impl->hudPreviousWall;
        if(delta>uint64_t(ticksPerSecond))impl->hudWallAverage=0;
        impl->hudWallAverage=impl->hudWallAverage?impl->hudWallAverage*.85+double(delta)*.15:double(delta);
        impl->hudFps.store(ticksPerSecond/impl->hudWallAverage,std::memory_order_relaxed);
        if(available && impl->previousAvailable) {
            const double current=double(cpu-impl->previousCpu)*1000/ticksPerSecond;
            impl->hudCpuAverage=impl->hudCpuAverage?impl->hudCpuAverage*.85+current*.15:current;
            impl->hudCpuMs.store(impl->hudCpuAverage,std::memory_order_relaxed);
        }
    }
    impl->hudPreviousWall=wall;
    if(!impl->firstWall) {
        s32 preferredCore=0;uint64_t mask=0;
        std::string info="[host] game_thread_cpu_ticks_result="+std::to_string(cpuResult);
        if(R_SUCCEEDED(svcGetThreadCoreMask(&preferredCore,&mask,CUR_THREAD_HANDLE)))info+=" game_thread_cpu_mask="+std::to_string(mask);
        std::cout<<info<<'\n';
    }
    if(!impl->firstWall) {
        impl->firstWall=wall;impl->previousCpu=cpu;
        // Boot time before the first completed frame is not a frame split.
        if(FramePhases::state.installed)impl->local.framePhases=FramePhases::boundary();
    }
    else {
        ++impl->local.frames;impl->local.wallTicks=wall-impl->firstWall;
        if(FramePhases::state.installed){impl->local.framePhases=FramePhases::boundary();++impl->local.framePhaseFrames;}
        if(available && impl->previousAvailable) {
            const auto delta=cpu-impl->previousCpu;
            impl->local.cpuTicks+=delta;++impl->local.cpuFrames;
            if(wasSampling){impl->local.sampledCpuTicks+=delta;++impl->local.sampledCpuFrames;}
            else {impl->local.plainCpuTicks+=delta;++impl->local.plainCpuFrames;}
        }
        impl->previousCpu=cpu;
    }
    impl->previousAvailable=available;
    const uint64_t count=impl->completed.fetch_add(1,std::memory_order_release)+1;
    if(count==1 || count%300==0) {
        std::lock_guard lock(impl->sampleMutex);
        impl->published=interval(impl->local,impl->previousPublication);impl->previousPublication=impl->local;
    }
    if(impl->cpuProfile && (count==1 || count%60==0)) {
        CpuWorkProfile::setSampling(true);CpuWorkProfile::resetWork();impl->sampling=true;
    }
}
const std::string& Session::path()const{return impl->filename;}
void Session::excludeHostTime(uint64_t nanoseconds) {
    // Producer-owned timing origins move with the game clocks, so the HUD
    // measures active frames rather than reporting sleep as a slow frame.
    const uint64_t ticks=(nanoseconds/1000000000ull)*19200000ull+
        (nanoseconds%1000000000ull)*19200000ull/1000000000ull;
    if(impl->firstWall)impl->firstWall+=ticks;
    if(impl->hudPreviousWall)impl->hudPreviousWall+=ticks;
}
Session::Metrics Session::metrics() const noexcept {
    return {frameIdleSeconds()>=2?0:impl->hudFps.load(std::memory_order_relaxed),
        impl->hudCpuMs.load(std::memory_order_relaxed)};
}
uint64_t Session::frameCount()const noexcept{return impl->completed.load(std::memory_order_acquire);}
double Session::frameIdleSeconds()const noexcept {
    const auto last=impl->lastFrameWall.load(std::memory_order_acquire);
    return double(armGetSystemTick()-(last?last:impl->begin))/ticksPerSecond;
}
}
