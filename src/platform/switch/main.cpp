#include <switch.h>
#include "ps2_runtime.h"
#include "controller_prompts.h"
#include "frame_events.h"
#include "graphics/deko3d_backend.h"
#include "native_scene.h"
#include "native_video.h"
#include "telemetry.h"
#include "widescreen.h"
#include "video.h"
#include "cpu_sampler.h"
#include "cpu_benchmark.h"
#include "clock_boost.h"
#include "switch_pgo.h"
#include "stall_watchdog.h"
#include "replay_ui.h"
#include "replay_harness.h"
#include "render_image.h"
#include "replay_catalog.h"
#include "machine_snapshot.h"
#include "night100_switch_host.h"
#include "runtime/resource_disc.h"
#include "runtime/ee_scheduler.h"
#include "runtime/cpu_work_profile.h"
#include "ps2x/iop/iop_subsystem.h"
#include "native_particle_policy.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

void pauseNativePortClock();
void resumeNativePortClock();
void excludeNativePortClock(uint64_t nanoseconds);

// Let libnx size the heap from this process's actual application/applet limit.
// A fixed desktop-sized heap would fail in hbmenu's smaller applet mode.
extern "C" {size_t __nx_heap_size=0;}

namespace {
using Clock=std::chrono::steady_clock;
struct IopPinState {
    std::atomic<bool> done{false};
    std::atomic<Result> set{0},read{0};
    std::atomic<uint64_t> mask{0};
    std::atomic<bool> logged{false};
} iopPin;

// IOP execution counters. Snapshots are taken on the polled main path (never from the
// summary provider, which can run after the runtime is destroyed), so the summary
// only reads stored values.
struct IopCounters {
    std::mutex mutex;
    ps2x::iop::IopExecutionStats last{}, total{}, delta{};
    void snapshot(const ps2x::iop::IopExecutionStats& now) {
        std::lock_guard lock(mutex);
        delta.workerCredits=now.workerCredits-last.workerCredits;
        delta.workerJobs=now.workerJobs-last.workerJobs;
        delta.workerCycles=now.workerCycles-last.workerCycles;
        delta.eeInlineCredits=now.eeInlineCredits-last.eeInlineCredits;
        delta.eeInlineJobs=now.eeInlineJobs-last.eeInlineJobs;
        delta.eeInlineCycles=now.eeInlineCycles-last.eeInlineCycles;
        last=now;total=now;
    }
    std::string deltaText() {
        std::lock_guard lock(mutex);
        return iopText(delta,"iop_");
    }
    std::string totalText() {
        std::lock_guard lock(mutex);
        return iopText(total,"iop_total_");
    }
    static std::string iopText(const ps2x::iop::IopExecutionStats& s,const char* prefix) {
        const std::string p=prefix;
        return " "+p+"worker_credits="+std::to_string(s.workerCredits)+" "+p+"worker_jobs="+std::to_string(s.workerJobs)+
            " "+p+"worker_cycles="+std::to_string(s.workerCycles)+" "+p+"ee_inline_credits="+std::to_string(s.eeInlineCredits)+
            " "+p+"ee_inline_jobs="+std::to_string(s.eeInlineJobs)+" "+p+"ee_inline_cycles="+std::to_string(s.eeInlineCycles);
    }
} iopCounters;
const char* cacheModeName(ps2x::ResourceCacheMode mode) {
    switch(mode) {
    case ps2x::ResourceCacheMode::WholeFile:return "file";
    case ps2x::ResourceCacheMode::WholeFilePrefetch:return "prefetch";
    default:return "off";
    }
}
struct Options {
    std::filesystem::path root="/switch/night100";
    std::string profile="sw",logId=std::to_string(armGetSystemTick());
    double seconds=0,logInterval=15;
    uint64_t replaySession=0;
    bool skipVideos=false,checkAssets=false,gpuSelfTest=false,cpuProfile=false,nativeCpuProfile=false,stallWatchdog=false,cpuKernelBenchmark=false;
    // Defaults: fast clocks and the core-2 IOP worker.
    SwitchClockBoost::Mode clocks=SwitchClockBoost::Mode::Fast;
    // Widescreen world output; --aspect 4:3 restores the original centered layout.
    bool wideScreen=true;
    // Internal resolution: -1 auto (handheld 1x, docked 2x), 0 half, 1 native, 2 double.
    int internalResolution=-1;
    bool iopWorker=true;
    bool referenceParticles=false;
    ps2x::ResourceCacheMode resourceCache=ps2x::ResourceCacheMode::Off;
    // The image mode needs a -DNIGHT100_DIRTY_LINES=ON build.
    RenderImage::Mode renderImage=RenderImage::Mode::Off;
};
const char* resolutionName(int mode){return mode==0?"1/2X (320X224)":mode==2?"2X (1280X720)":"1X (640X448)";}
const char* renderImageName(RenderImage::Mode mode) {
    return mode==RenderImage::Mode::Image?"image":mode==RenderImage::Mode::Rendermat?"rendermat":"off";
}
double number(const std::string& value) {
    size_t end=0;const double result=std::stod(value,&end);
    if(end!=value.size() || !std::isfinite(result))throw std::runtime_error("Invalid numeric argument");
    return result;
}
Options options(int argc,char** argv) {
    Options result;
    for(int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        auto value=[&]()->std::string {if(i+1==argc)throw std::runtime_error("Missing value for "+arg);return argv[++i];};
        if(arg=="--root")result.root=value();
        else if(arg=="--controller-prompts")result.profile=value();
        else if(arg=="--log-id")result.logId=value();
        else if(arg=="--log-interval")result.logInterval=number(value());
        else if(arg=="--seconds")result.seconds=number(value());
        else if(arg=="--replay-session") {
            const auto text=value();
            if(text.empty() || text.find_first_not_of("0123456789")!=std::string::npos)
                throw std::runtime_error("Replay session must be a positive recording number");
            result.replaySession=std::stoull(text);
            if(!result.replaySession)throw std::runtime_error("Replay session must be a positive recording number");
        }
        else if(arg=="--skip-videos")result.skipVideos=true;
        else if(arg=="--check-assets")result.checkAssets=true;
        else if(arg=="--gpu-self-test")result.gpuSelfTest=true;
        else if(arg=="--cpu-profile")result.cpuProfile=true;
        else if(arg=="--native-cpu-profile")result.nativeCpuProfile=true;
        else if(arg=="--stall-watchdog")result.stallWatchdog=true;
        else if(arg=="--cpu-kernel-benchmark")result.cpuKernelBenchmark=true;
        else if(arg=="--iop-worker"){}  // accepted for older command lines; the worker is already on
        else if(arg=="--no-iop-worker")result.iopWorker=false;
        else if(arg=="--reference-particles")result.referenceParticles=true;
        else if(arg=="--resource-cache") {
            const auto text=value();
            if(text=="off")result.resourceCache=ps2x::ResourceCacheMode::Off;
            else if(text=="file")result.resourceCache=ps2x::ResourceCacheMode::WholeFile;
            else if(text=="prefetch")result.resourceCache=ps2x::ResourceCacheMode::WholeFilePrefetch;
            else throw std::runtime_error("--resource-cache must be off, file or prefetch");
        }
        else if(arg=="--switch-clocks") {
            if(!SwitchClockBoost::parseMode(value(),result.clocks))throw std::runtime_error("--switch-clocks must be fast or default");
        }
        else if(arg=="--render-image") {
            const auto text=value();
            if(text=="off")result.renderImage=RenderImage::Mode::Off;
            else if(text=="rendermat")result.renderImage=RenderImage::Mode::Rendermat;
            else if(text=="image")result.renderImage=RenderImage::Mode::Image;
            else throw std::runtime_error("--render-image must be off, rendermat or image");
        }
        else if(arg=="--aspect") {
            const auto text=value();
            if(text=="16:9")result.wideScreen=true;
            else if(text=="4:3")result.wideScreen=false;
            else throw std::runtime_error("--aspect must be 4:3 or 16:9");
        }
        else if(arg=="--internal-res") {
            const std::string text=value();
            if(text=="auto")result.internalResolution=-1;
            else if(text=="half")result.internalResolution=0;
            else if(text=="1x" || text=="native")result.internalResolution=1;
            else if(text=="2x" || text=="720p")result.internalResolution=2;
            else throw std::runtime_error("--internal-res must be auto, half, 1x or 2x");
        }
        else throw std::runtime_error("Unknown Switch argument: "+arg);
    }
    if(result.seconds<0 || result.seconds>86400)throw std::runtime_error("Run duration must be 0..86400 seconds");
    const auto root=result.root.string();
    if(!root.starts_with("/switch/") || root.ends_with('/') || root.find("//")!=std::string::npos)
        throw std::runtime_error("Use an application folder beneath /switch, such as /switch/night100");
    for(const auto& part:result.root)
        if(part=="." || part=="..")throw std::runtime_error("Invalid SD root path component");
    for(unsigned char c:root)
        if(c<32 || c==127 || c==':' || c=='\\')throw std::runtime_error("Invalid SD root path character");
    return result;
}
void verifyElf(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    if(!file)throw std::runtime_error("Missing game ELF: install the game resources once with switch-deploy --assets");
    Sha256Context context;sha256ContextCreate(&context);
    std::array<char,65536> chunk;
    while(file) {
        file.read(chunk.data(),chunk.size());
        if(file.gcount())sha256ContextUpdate(&context,chunk.data(),size_t(file.gcount()));
    }
    if(!file.eof())throw std::runtime_error("Failed reading the game ELF");
    std::array<uint8_t,32> digest{};sha256ContextGetHash(&context,digest.data());
    constexpr std::array<uint8_t,32> expected{0xd5,0xfe,0xbf,0xa9,0x71,0x19,0xfb,0xa2,0xc2,0xf3,0xeb,0x58,0xab,0xaa,0xe2,0x68,
        0x03,0x7a,0xfa,0xe2,0x77,0x3d,0xdc,0x57,0xca,0xb7,0xe1,0xc8,0x13,0xb4,0x85,0xb1};
    if(digest!=expected)throw std::runtime_error("Unsupported ELF identity; use the verified USA disc resources");
}
struct HostPoll {
    SwitchTelemetry::Session& telemetry;
    Clock::time_point start=Clock::now();
    double seconds=0;
    bool deadline=false;
    bool nativeCpuProfile=false;
    bool replayClipActive=false;
    bool foreground=true;
    SwitchClockBoost::Controller* clocks=nullptr;
    bool chordStop=false;
    // Internal resolution state: 0 half, 1 native, 2 double; auto follows the dock.
    int resolution=1;bool resolutionAuto=true;bool dockedLast=false;Deko3DBackend* gpuForResolution=nullptr;
    std::atomic<bool> gameplay{false};
    Deko3DBackend* gpu=nullptr;
    // Overlay GPU time: mean over the samples added since the previous poll.
    Deko3DBackend::GpuTiming gpuPrevious{};
    double gpuMsOverlay=-1;
    SwitchStallWatchdog::Watchdog* watchdog=nullptr;
    SwitchReplayUi* replayUi=nullptr;
    Clock::time_point lastStallReport=Clock::now();
    Clock::time_point lastGpuReport=Clock::now();
};
struct AutoSleepGuard {
    bool previous=false,restore=false;
    Result result=0;
    AutoSleepGuard() {
        result=appletIsAutoSleepDisabled(&previous);
        if(R_SUCCEEDED(result)) {
            result=appletSetAutoSleepDisabled(true);
            restore=R_SUCCEEDED(result) && !previous;
        }
    }
    ~AutoSleepGuard(){if(restore)appletSetAutoSleepDisabled(previous);}
};
}

int main(int argc,char** argv) {
    static std::array<char,65536> stdoutBuffer,stderrBuffer;
    std::setvbuf(stdout,stdoutBuffer.data(),_IOFBF,stdoutBuffer.size());
    std::setvbuf(stderr,stderrBuffer.data(),_IOFBF,stderrBuffer.size());
    std::unique_ptr<SwitchTelemetry::Session> telemetry;
    bool mountedRomfs=false;
    try {
        auto config=options(argc,argv);
        CpuWorkProfile::configure(config.cpuProfile);
        SwitchPgo::prepareDirectory();
        SwitchCpuSampler::configure(config.nativeCpuProfile);
        telemetry=std::make_unique<SwitchTelemetry::Session>(config.logId,config.logInterval,config.root.string(),config.cpuProfile);
        auto watchdog=std::make_unique<SwitchStallWatchdog::Watchdog>(config.stallWatchdog,config.root.string(),config.logId);
        if(config.stallWatchdog)telemetry->stage("stall_watchdog_ready="+std::to_string(watchdog->ready())+
            " result="+std::to_string(watchdog->setupResult()));
        AutoSleepGuard autoSleep;
        telemetry->stage("auto_sleep_disabled_result="+std::to_string(autoSleep.result));
        if(!fsdevGetDeviceFileSystem("sdmc") && R_FAILED(fsdevMountSdmc()))
            throw std::runtime_error("Unable to mount the SD device for game resources");
        if(chdir("sdmc:/"))throw std::runtime_error("Unable to select the SD filesystem");
        const Result romfs=romfsInit();
        if(R_FAILED(romfs))throw std::runtime_error("The NRO is missing its embedded shaders/controller resources");
        mountedRomfs=true;
        if(config.gpuSelfTest) {
            {Deko3DBackend gpu;if(!gpu.runSelfTest())throw std::runtime_error("Deko GPU self-test failed");}
            telemetry->finish("NIGHT100_SWITCH_GPU_TEST_OK");romfsExit();return 0;
        }
        const auto elf=config.root/"game/SLUS_203.49";
        verifyElf(elf);
        if(!std::filesystem::is_regular_file(config.root/"game/.disc-image"))
            throw std::runtime_error("Compact disc map is missing; install resources with switch-deploy --assets");
        // Validate every compact source once, before guest I/O can repeatedly
        // retry an incomplete install. The immutable reader is then cached.
        if(!ps2x::ResourceDisc::open(config.root/"game/.disc-image"))
            throw std::runtime_error("Cannot open compact disc; complete the --assets all installation");
        ControllerPrompts::configure(config.profile,"romfs:/controllers");
        telemetry->stage("assets_verified");
        if(config.checkAssets){telemetry->finish("NIGHT100_SWITCH_ASSETS_OK");romfsExit();return 0;}
        const auto replayDirectory=config.root/"logs/replays";
        std::filesystem::path sessionPlayback;
        if(config.replaySession) {
            for(const auto& entry:ReplayCatalog::list(replayDirectory))
                if(entry.id==config.replaySession)sessionPlayback=entry.directory;
            if(sessionPlayback.empty())throw std::runtime_error("No complete Switch replay "+std::to_string(config.replaySession));
            const auto metadata=ReplayCatalog::load({config.replaySession,sessionPlayback});
            const std::string prefix="platform=switch;skip_videos=";
            if(!metadata.options.starts_with(prefix) || metadata.options.size()<=prefix.size()+9)
                throw std::runtime_error("Replay was not recorded by this Switch runtime");
            const auto policy=metadata.options.substr(prefix.size());
            if(!policy.starts_with("0;prompts=") && !policy.starts_with("1;prompts="))
                throw std::runtime_error("Unsupported Switch replay options");
            config.skipVideos=policy[0]=='1';config.profile=policy.substr(10);
            ControllerPrompts::configure(config.profile,"romfs:/controllers");
        }
        if(config.skipVideos)setenv("NIGHT100_SKIP_VIDEOS","1",1);
        else unsetenv("NIGHT100_SKIP_VIDEOS");
        auto runtime=std::make_unique<PS2Runtime>();
        // Application-override mode has about 4 GiB; caps keep the cache well inside it.
        const uint64_t cacheCap=config.resourceCache==ps2x::ResourceCacheMode::WholeFilePrefetch?1024ull<<20:
            config.resourceCache==ps2x::ResourceCacheMode::WholeFile?512ull<<20:0;
        runtime->setResourceCacheMode(config.resourceCache,cacheCap?cacheCap:ps2x::ResourceDisc::kDefaultCacheBytes);
        // Before loadELF: the particle update commands read this policy when the overrides install.
        setReferenceParticles(config.referenceParticles);
        if(config.iopWorker) {
            // The hook runs on the worker thread: it only stores results. The main
            // thread logs them from the polled callback, so no stage line is written there.
            runtime->setIopWorkerThreadHook([]() {
                iopPin.set.store(svcSetThreadCoreMask(CUR_THREAD_HANDLE,2,1u<<2),std::memory_order_relaxed);
                uint64_t mask=0;s32 preferred=0;
                iopPin.read.store(svcGetThreadCoreMask(&preferred,&mask,CUR_THREAD_HANDLE),std::memory_order_relaxed);
                iopPin.mask.store(mask,std::memory_order_relaxed);
                iopPin.done.store(true,std::memory_order_release);
            });
            runtime->setIopWorkerEnabled(true);
            telemetry->stage("iop_worker_enabled=1");
        }
        runtime->setStateClockCallbacks(pauseNativePortClock,resumeNativePortClock,[session=telemetry.get()](uint64_t ns) {
            excludeNativePortClock(ns);session->excludeHostTime(ns);
            ReplayHarness::excludeHostTime(ns);
        });
        runtime->setMissingFunctionPolicy(PS2Runtime::MissingFunctionPolicy::Stop);
        HostPoll host{*telemetry,Clock::now(),config.seconds,false};
        host.nativeCpuProfile=config.nativeCpuProfile;
        host.watchdog=watchdog.get();
        runtime->setDebugUiCallbacks([](PS2Runtime&,void* data) {
            static_cast<HostPoll*>(data)->telemetry.stage("host_ready");
        },[](PS2Runtime& runtime,void* data) {
            auto& host=*static_cast<HostPoll*>(data);
            const auto now=Clock::now();
            const bool foreground=night100SwitchHostActive();
            host.watchdog->heartbeat(foreground);
            host.watchdog->audioThreadReady(night100SwitchHostAudioThreadHandle());
            if(host.replayUi)host.replayUi->poll();
            inputReplayWatchdog(&runtime);
            if(foreground!=host.foreground) {
                host.foreground=foreground;
                host.telemetry.stage(foreground?"foreground_resumed":"background_quiescing");
                host.lastStallReport=now;
            }
            if(!foreground)return;
            // Reads guest RAM only; no guest function is replaced for the gate.
            Widescreen::sample(runtime.memory().getRDRAM());
            if(host.gpu) {
                const auto metrics=host.telemetry.metrics();
                const auto timing=host.gpu->gpuTiming();
                if(timing.gpuSamples>host.gpuPrevious.gpuSamples) {
                    host.gpuMsOverlay=double(timing.gpuNanoseconds-host.gpuPrevious.gpuNanoseconds)*1e-6/
                        double(timing.gpuSamples-host.gpuPrevious.gpuSamples);
                    host.gpuPrevious=timing;
                }
                host.gpu->setMetricsOverlay({night100SwitchHostMetricsEnabled(),metrics.gameFps,metrics.producerCpuMs,host.gpuMsOverlay});
            }
            {
                // Track the replay clip itself; the sampler and PGO counters dump once it ends.
                const bool clipActive=ReplayHarness::uiState().playing;
                if(host.replayClipActive && !clipActive) {
                    if(host.nativeCpuProfile)SwitchCpuSampler::dump();
                    SwitchPgo::dumpCounters("replay_verification_end");
                }
                host.replayClipActive=clipActive;
            }
            if(host.nativeCpuProfile && host.gpu && host.replayClipActive) {
                const auto gpu=host.gpu->diagnostics();
                SwitchCpuSampler::poll((host.gameplay.load(std::memory_order_relaxed) || NativeVideo::active()) &&
                    runtime.eeScheduler().isExecutingGuest() && gpu.stage==Deko3DBackend::Stage::Idle);
            }
            if(now-host.lastGpuReport>=std::chrono::seconds(15) && host.clocks)host.clocks->refresh();
            if(now-host.lastGpuReport>=std::chrono::seconds(15))iopCounters.snapshot(runtime.iopExecutionStats());
            if(host.gpu && now-host.lastGpuReport>=std::chrono::seconds(15)) {
                host.lastGpuReport=now;
                const auto gpu=host.gpu->diagnostics();
                std::cout<<"[gpu] stage="<<Deko3DBackend::diagnosticStageName(gpu.stage)
                    <<" frames="<<gpu.gameFrames<<" submissions="<<gpu.submissions<<" readbacks="<<gpu.readbacks
                    <<" textures="<<gpu.textureCacheEntries<<" legacy_textures="<<gpu.legacyCacheEntries
                    <<" descriptor_high_water="<<gpu.descriptorHighWater<<" descriptor_retiring="<<gpu.descriptorPendingRetirement
                    <<" rtt_allocations="<<gpu.renderTextureImageAllocations<<" rtt_reuses="<<gpu.renderTextureImageReuses
                    <<" rtt_pool_bytes="<<gpu.renderTexturePoolBytes<<" rtt_pool_images="<<gpu.renderTexturePoolImages
                    <<" dynamic_allocations="<<gpu.dynamicBufferAllocations<<" dynamic_uploads="<<gpu.dynamicGeometryUploads
                    <<" dynamic_upload_bytes="<<gpu.dynamicUploadBytes<<" dynamic_buffer_bytes="<<gpu.dynamicBufferBytes
                    <<" dynamic_blocks="<<gpu.dynamicBufferBlocks<<'\n';
                {
                    const auto timing=host.gpu->gpuTiming();
                    const auto mean=[](uint64_t ns,uint64_t samples){return samples?double(ns)*1e-6/double(samples):-1.0;};
                    std::cout<<"[gpu] timing_frames="<<timing.gpuSamples<<" gpu_mean_ms="<<mean(timing.gpuNanoseconds,timing.gpuSamples)
                        <<" composition_mean_ms="<<mean(timing.compositionNanoseconds,timing.compositionSamples)<<'\n';
                }
                {
                    const auto probes=host.gpu->probes();
                    std::cout<<"[gpu-probe] wide_config="<<probes.wideConfig<<" world_gate="<<probes.worldGate
                        <<" wide_targets="<<probes.wideTargets<<" gate_tokens="<<probes.gateTokens
                        <<" full_width_models="<<probes.fullWidthModels<<" timing_reads="<<probes.timingReads
                        <<" timing_rejected="<<probes.timingRejected<<" timing_render_only="<<probes.timingRenderOnly
                        <<" last_start="<<probes.lastStart<<" last_composition="<<probes.lastComposition
                        <<" last_end="<<probes.lastEnd<<'\n';
                    const auto ws=Widescreen::status();
                    std::cout<<"[widescreen] enabled="<<ws.enabled<<" mode="<<ws.mode<<" ostrich="<<ws.ostrich
                        <<" gate="<<ws.gate<<" samples="<<ws.samples<<" gate_true_samples="<<ws.gateTrueSamples<<'\n';
                }
            }
            host.telemetry.poll();
            if(!NativeVideo::active() && host.telemetry.frameIdleSeconds()>=10 && now-host.lastStallReport>=std::chrono::seconds(15)) {
                host.lastStallReport=now;
                std::ostringstream info;
                info<<"frame_stall ee_executing="<<runtime.eeScheduler().isExecutingGuest()
                    <<" video_active="<<NativeVideo::active()<<" ee_pc=0x"<<std::hex<<runtime.m_debugPc.load(std::memory_order_relaxed)
                    <<" ee_ra=0x"<<runtime.m_debugRa.load(std::memory_order_relaxed)<<std::dec;
                const auto rpc=runtime.m_debugIopRpc.load(std::memory_order_relaxed);
                info<<" iop_rpc_depth="<<runtime.m_debugIopRpcDepth.load(std::memory_order_relaxed)
                    <<" iop_rpc_sid=0x"<<std::hex<<(rpc>>32)<<" iop_rpc_function=0x"<<uint32_t(rpc)<<std::dec;
                EeKernelSnapshot snapshot;
                if(runtime.eeScheduler().trySnapshot(snapshot)) {
                    info<<" ee_snapshot="<<snapshot.sequence<<" ee_cycle="<<snapshot.eeCycle<<" ee_running="<<snapshot.runningThreadId;
                    for(const auto& thread:snapshot.threads)
                        info<<" [thread="<<thread.id<<" pc=0x"<<std::hex<<thread.pc<<std::dec<<" status="<<unsigned(thread.status)
                            <<" wait="<<unsigned(thread.waitReason)<<" wait_id="<<thread.waitId<<']';
                    for(const auto& semaphore:snapshot.semaphores)
                        if(semaphore.waiters)info<<" [semaphore="<<semaphore.id<<" count="<<semaphore.count<<" waiters="<<semaphore.waiters<<']';
                }else info<<" ee_snapshot=busy";
                if(host.gpu) {
                    const auto gpu=host.gpu->diagnostics();
                    info<<" gpu_stage="<<Deko3DBackend::diagnosticStageName(gpu.stage)<<" gpu_op="<<gpu.operationSerial
                        <<" gpu_slot="<<gpu.slot<<" gpu_frames="<<gpu.gameFrames<<" gpu_submissions="<<gpu.submissions<<" gpu_readbacks="<<gpu.readbacks
                        <<" gpu_textures="<<gpu.textureCacheEntries<<" gpu_legacy_textures="<<gpu.legacyCacheEntries
                        <<" gpu_descriptor_high_water="<<gpu.descriptorHighWater<<" gpu_descriptor_retiring="<<gpu.descriptorPendingRetirement;
                }
                host.telemetry.stage(info.str());
            }
            if(!iopPin.logged.load(std::memory_order_relaxed) &&
               iopPin.done.load(std::memory_order_acquire)) {
                iopPin.logged.store(true,std::memory_order_relaxed);
                host.telemetry.stage("iop_worker_pin set_result="+std::to_string(iopPin.set.load())+
                    " read_result="+std::to_string(iopPin.read.load())+" core_mask="+std::to_string(iopPin.mask.load()));
            }
            if(host.gpuForResolution) {
                int wanted=host.resolution;
                if(night100SwitchHostTakeResolutionChord()){host.resolutionAuto=false;wanted=(host.resolution+1)%3;}
                else if(host.resolutionAuto){const bool docked=night100SwitchHostDocked();if(docked!=host.dockedLast){host.dockedLast=docked;wanted=docked?2:1;}}
                if(wanted!=host.resolution) {
                    host.resolution=wanted;
                    host.gpuForResolution->setInternalResolution(wanted==0?Deko3DBackend::InternalResolution::Half:wanted==2?Deko3DBackend::InternalResolution::Double:Deko3DBackend::InternalResolution::Native);
                    if(host.replayUi)host.replayUi->notify(std::string("RESOLUTION ")+resolutionName(wanted));
                    host.telemetry.stage(std::string("internal_res=")+resolutionName(wanted)+(host.resolutionAuto?" (auto)":" (chord)"));
                }
            }
            if(!host.deadline && !host.chordStop && night100SwitchHostTakeExitChord()) {
                host.chordStop=true;runtime.requestStop();host.telemetry.stage("chord_stop_requested");
            }
            if(host.seconds>0 && std::chrono::duration<double>(Clock::now()-host.start).count()>=host.seconds) {
                host.deadline=true;runtime.requestStop();host.telemetry.stage("deadline_stop_requested");
            }
        },nullptr,&host);
        if(!runtime->initialize("Night100"))throw std::runtime_error("Switch runtime initialization failed");
        // Created after host initialization so its focus hook is installed. Declared
        // before the guard, so the summary provider is cleared before it is destroyed.
        auto clocks=std::make_unique<SwitchClockBoost::Controller>(config.clocks);
        host.clocks=clocks.get();
        telemetry->setSummaryProvider([controller=clocks.get(),mode=config.resourceCache]{
            const auto cache=ps2x::ResourceDisc::cacheStats();
            return controller->summary()+" resource_cache_mode="+cacheModeName(mode)+
                " lifecycle_calls="+std::to_string(night100SwitchHostLifecycleCallCount())+iopCounters.deltaText()+
                " cache_resident_bytes="+std::to_string(cache.bytes)+" cache_files="+std::to_string(cache.files)+
                " cache_hits="+std::to_string(cache.hits)+" cache_misses="+std::to_string(cache.loads)+
                " cache_evictions="+std::to_string(cache.evictions)+" cache_uncached="+std::to_string(cache.uncached);
        });
        struct ProviderGuard {
            SwitchTelemetry::Session& session;
            HostPoll& host;
            ~ProviderGuard(){session.setSummaryProvider(nullptr);host.clocks=nullptr;}
        } providerGuard{*telemetry,host};
        clocks->setActive(night100SwitchHostActive());
        night100SwitchHostSetFailureReporter([](const char* message,void* session) {
            SwitchCpuSampler::dump();
            static_cast<SwitchTelemetry::Session*>(session)->stage(std::string("host_failure ")+message);
        },telemetry.get());
        auto gpu=std::make_unique<Deko3DBackend>();
        auto* gpuPointer=gpu.get();
        host.gpu=gpuPointer;
        gpuPointer->setPresentationActivePredicate(night100SwitchHostActive);
        night100SwitchHostSetProducerQuiesce([](void* pointer) {
            static_cast<Deko3DBackend*>(pointer)->quiesce();
        },gpuPointer);
        runtime->gs().setRasterBackend(std::move(gpu));
        configureNativeScene(gpuPointer);
        gpuPointer->setWideScreen(config.wideScreen);
        gpuPointer->setGuestWidensProjection(config.wideScreen);
        host.resolutionAuto=config.internalResolution<0;
        host.resolution=host.resolutionAuto?(night100SwitchHostDocked()?2:1):config.internalResolution;
        gpuPointer->setInternalResolution(host.resolution==0?Deko3DBackend::InternalResolution::Half:host.resolution==2?Deko3DBackend::InternalResolution::Double:Deko3DBackend::InternalResolution::Native);
        Widescreen::setEnabled(config.wideScreen);
        Widescreen::setBackend(gpuPointer);
        telemetry->stage(std::string("aspect=")+(config.wideScreen?"16:9":"4:3")+" internal_res="+resolutionName(host.resolution)+(host.resolutionAuto?" (auto)":"")+" docked="+(night100SwitchHostDocked()?"1":"0"));
        SwitchVideo::setPresenter([](const SwitchVideo::Frame& frame,void* opaque) {
            return static_cast<Deko3DBackend*>(opaque)->presentVideo(frame);
        },gpuPointer);
        const std::string sessionOptions="platform=switch;skip_videos="+std::to_string(config.skipVideos)+";prompts="+config.profile;
        ReplayHarness::configure({true,replayDirectory,sessionPlayback,sessionOptions},gpuPointer,{
            [gpuPointer](PS2Runtime&,const std::filesystem::path& path) {
                std::vector<uint8_t> rgba;uint32_t width=0,height=0;
                if(!gpuPointer->copyCompletedFrameRgba(rgba,width,height) || rgba.size()!=size_t(width)*height*4)
                    throw std::runtime_error("Completed Switch replay image is unavailable");
                std::vector<uint8_t> rgb(size_t(width)*height*3);
                for(size_t src=0,dst=0;src<rgba.size();src+=4,dst+=3)std::memcpy(rgb.data()+dst,rgba.data()+src,3);
                std::ofstream out(path,std::ios::binary);out<<"P6\n"<<width<<' '<<height<<"\n255\n";
                out.write(reinterpret_cast<const char*>(rgb.data()),rgb.size());
                out.close();if(!out)throw std::runtime_error("Cannot save Switch replay image");
            },[gpuPointer]{gpuPointer->presentOverlay();}
        });
        GameFrameEvents::configure([](uint8_t* ram,PS2Runtime* runtime,void* data) {
            // A focus transition may finish during a blocked display acquire.
            // Rebase before publishing that first returned frame's timings.
            runtime->serviceHostLifecycle();
            auto& host=*static_cast<HostPoll*>(data);
            host.watchdog->producerThreadReady();
            if(host.nativeCpuProfile) {
                // Producer-only reads use the verified ELF's gameplay predicate.
                auto word=[&](size_t offset){uint32_t value;std::memcpy(&value,ram+offset,sizeof(value));return value;};
                host.gameplay.store(word(0x357e20)==11 && word(0x357e24)==2 && !word(0x363860) && word(0x3633b0),std::memory_order_relaxed);
                SwitchCpuSampler::producerThreadReady();
            }
            host.telemetry.completedFrame();
            inputReplayFrameCompleted(ram,runtime);
        },&host);
        RenderImage::configure(config.renderImage); // read when the ELF's overrides install
        if(!runtime->loadELF(elf.string()))throw std::runtime_error("Cannot load the supported game ELF");
        if(config.cpuKernelBenchmark) {
            try {runNativeCpuKernelBenchmark(*runtime);}
            catch(const std::exception& error){telemetry->stage(std::string("cpu_kernel_benchmark_failed ")+error.what());}
        }
        auto paths=PS2Runtime::getIoPaths();
        paths.cdImage=config.root/"game/.disc-image";
        paths.mcRoot=config.root/"saves";
        std::filesystem::create_directories(paths.mcRoot);
        PS2Runtime::setIoPaths(paths);
        if(!sessionPlayback.empty()) {
            // The render worker may still hold work; drain it before the outer restore writes VRAM directly.
            gpuPointer->quiesce();
            loadMachineSnapshot(*runtime,sessionPlayback/"state.bin");ReplayHarness::restored(runtime.get());
        }
        SwitchReplayUi replayUi(*gpuPointer);host.replayUi=&replayUi;
        host.gpuForResolution=gpuPointer;host.dockedLast=night100SwitchHostDocked();
        // Thread layout for benchmark logs: producer (this thread), audio worker, IOP worker (pinned above).
        {
            auto maskText=[](Handle handle) {
                uint64_t mask=0;s32 preferred=0;
                const Result rc=svcGetThreadCoreMask(&preferred,&mask,handle);
                return R_SUCCEEDED(rc)?"core_mask="+std::to_string(mask)+" preferred_core="+std::to_string(preferred):
                    "result="+std::to_string(rc);
            };
            telemetry->stage("thread_layout producer "+maskText(CUR_THREAD_HANDLE)+
                " audio "+(night100SwitchHostAudioThreadHandle()?maskText(night100SwitchHostAudioThreadHandle()):std::string("unavailable"))+
                " render unexposed iop_worker_enabled="+std::to_string(config.iopWorker));
        }
        telemetry->stage("game_start "+clocks->summary()+" resource_cache_mode="+cacheModeName(config.resourceCache)+
            " iop_worker="+(config.iopWorker?"on":"off")+" reference_particles="+(config.referenceParticles?"on":"off")+
            " render_image="+renderImageName(config.renderImage));
        telemetry->stage(std::string("reference_particles_policy=")+(referenceParticles()?"on":"off")+" applied_before_overrides=1");
        host.start=Clock::now();
        std::exception_ptr gameFailure;
        try{runtime->run();}catch(...){gameFailure=std::current_exception();}
        iopCounters.snapshot(runtime->iopExecutionStats());
        telemetry->stage("iop_execution_totals"+iopCounters.totalText());
        replayUi.close();host.replayUi=nullptr;
        try{finishInputReplay(gameFailure?"crash":"stop");}catch(...){if(!gameFailure)gameFailure=std::current_exception();}
        if(gameFailure)std::rethrow_exception(gameFailure);
        SwitchCpuSampler::dump();
        telemetry->stage("runtime_returned");
        SwitchPgo::dumpCounters("runtime_returned");
        GameFrameEvents::configure(nullptr);SwitchVideo::setPresenter(nullptr,nullptr);
        const auto stats=gpuPointer->statistics();
        telemetry->stage("gpu_frames="+std::to_string(stats.gameFrames)+" gpu_readbacks="+std::to_string(stats.readbacks));
        night100SwitchHostSetProducerQuiesce(nullptr,nullptr);
        runtime.reset();
        if(const auto replay=ReplayHarness::requestedReplay();!replay.empty()) {
            const auto name=replay.filename().string();
            const auto id=std::stoull(name.substr(8));
            const std::string executable="sdmc:"+(config.root/"night100.nro").string();
            // hbloader restarts the same installed executable with a clean
            // process, then restores the complete starting machine state.
            auto quoted=[](const std::string& value) {
                std::string out="\"";for(char c:value){if(c=='\\'||c=='\"')out+='\\';out+=c;}return out+'\"';
            };
            const std::string arguments=quoted(executable)+" --root "+quoted(config.root.string())+
                " --replay-session "+std::to_string(id)+(config.cpuProfile?" --cpu-profile":"")+
                (config.nativeCpuProfile?" --native-cpu-profile":"")+(config.stallWatchdog?" --stall-watchdog":"")+
                " --switch-clocks "+SwitchClockBoost::modeName(config.clocks)+" --aspect "+(config.wideScreen?"16:9":"4:3")+
                (config.iopWorker?"":" --no-iop-worker")+(config.referenceParticles?" --reference-particles":"")+
                (config.resourceCache==ps2x::ResourceCacheMode::WholeFile?" --resource-cache file":
                 config.resourceCache==ps2x::ResourceCacheMode::WholeFilePrefetch?" --resource-cache prefetch":"")+
                (config.renderImage!=RenderImage::Mode::Off?std::string(" --render-image ")+renderImageName(config.renderImage):std::string());
            if(!envHasNextLoad() || R_FAILED(envSetNextLoad(executable.c_str(),arguments.c_str())))
                throw std::runtime_error("hbloader cannot restart this executable for replay");
            telemetry->finish("NIGHT100_SWITCH_STOP replay="+std::to_string(id));romfsExit();return 0;
        }
        telemetry->finish(host.deadline?"NIGHT100_SWITCH_STOP deadline":host.chordStop?"NIGHT100_SWITCH_STOP chord":"NIGHT100_SWITCH_STOP");
        romfsExit();return 0;
    }catch(const std::exception& error) {
        // Record the fatal to SD before anything else can fail. consoleInit is only a fallback when telemetry never opened.
        if(telemetry)try{telemetry->finish(std::string("NIGHT100_SWITCH_FATAL exception: ")+error.what());}catch(...){}
        else {consoleInit(nullptr);std::printf("Night100: %s\n",error.what());consoleUpdate(nullptr);consoleExit(nullptr);}
        try{GameFrameEvents::configure(nullptr);SwitchVideo::setPresenter(nullptr,nullptr);SwitchCpuSampler::dump();}catch(...){}
        if(mountedRomfs)romfsExit();
        return 1;
    }catch(...) {
        if(telemetry)try{telemetry->finish("NIGHT100_SWITCH_FATAL unknown exception");}catch(...){}
        if(mountedRomfs)romfsExit();
        return 1;
    }
}
