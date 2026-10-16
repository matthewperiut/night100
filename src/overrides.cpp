// Build-specific fixes belong here, gated by this retail ELF's identity.
// SDK bindings are generated from scanner matches in analysis/scooby.toml.
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_stubs.h"
#include "frame_metrics.h"
#include "native_scene.h"
#include "native_cpu.h"
#include "native_particles.h"
#include "native_vu1_lastprogram.h"
#include "native_video.h"
#include "input_replay.h"
#include "machine_snapshot.h"
#include "replay_harness.h"
#include "frame_phases.h"
#ifndef __SWITCH__
#include "game_benchmark.h"
#include "collision_debug.h"
#include "particle_watch.h"
#else
#include <switch.h>
#endif
#include "runtime/ee_scheduler.h"
#include <chrono>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {
PS2Runtime::RecompiledFunction gameLoopOriginal=nullptr;
void waitGameFrame(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    // zGameLoop's original 0x131da8..0x131dc0 loop spins until the guest
    // vertical-blank counter advances by TWO ticks: an explicit 30 Hz cap.
    // The port presents at 60 Hz. Wait for one genuine tick, retaining the
    // original interrupt counter, actual-time delta and renderer continuations.
    const uint32_t counterAddress=GPR_U32(cpu,28)-0x76c8;
    if(counterAddress>PS2_RAM_SIZE-4)throw std::runtime_error("Invalid game frame counter address");
    uint32_t counter=0;std::memcpy(&counter,ram+counterAddress,4);
    const uint32_t elapsed=counter-GPR_U32(cpu,4);
    if(elapsed<1u)runtime->eeScheduler().waitVSync(runtime->eeScheduler().currentVSyncTick());
    // Match the original successful loop's registers, then resume before its
    // existing counter store and zGameRender call. No game state is bypassed.
    SET_GPR_S32(cpu,3,static_cast<int32_t>(counter));SET_GPR_U64(cpu,2,0);
    cpu->pc=0x131dc4;
    gameLoopOriginal(ram,cpu,runtime);
}
PS2Runtime::RecompiledFunction endRenderFxOriginal=nullptr;
void endRenderFxBeforeFrameWait(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    const uint32_t returnPc=GPR_U32(cpu,31);
    endRenderFxOriginal(ram,cpu,runtime);
    // The first visit to the polling loop is an inline generated branch, so
    // replacing its resume PC alone still spins until a scheduler checkpoint.
    // EndRenderFX is the adjacent real call boundary. Finish its original work
    // and unwind to the two original loads preceding the sleeping wait hook.
    // Internal yields retain their pending PC; other callers return normally.
    if(returnPc==0x131d98 && cpu->pc==returnPc && GPR_U32(cpu,31)==returnPc && !runtime->isStopRequested())
        (void)runtime->dispatchGuestBranch(ram,cpu,returnPc,0x13c824,0,
            PS2Runtime::GuestBranchKind::DirectJump,"native frame wait handoff");
}
void beginGameFrameWait(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    // Match the original 0x131d98/0x131d9c LW instructions, including signed
    // register extension, before the unchanged counter comparison/wait policy.
    cpu->pc=0x131d98;
    SET_GPR_S32(cpu,2,int32_t(runtime->Load32(ram,cpu,GPR_U32(cpu,29)+0x100)));
    cpu->pc=0x131d9c;
    SET_GPR_S32(cpu,4,int32_t(runtime->Load32(ram,cpu,GPR_U32(cpu,2))));
    cpu->pc=0x131da8;
    waitGameFrame(ram,cpu,runtime);
}
PS2Runtime::RecompiledFunction rasterShowOriginal=nullptr;
uint64_t cameraFrames=0,lastCameraFrames=0;
std::chrono::steady_clock::time_point firstCameraFrame,lastFrameReport;
double firstCameraCpu=0,lastReportCpu=0;
uint64_t lastReportGameClock=0;
std::chrono::steady_clock::time_point portClockEpoch;
bool portClockPaused=false;
uint64_t portClockFrozenTicks=0;
uint64_t portClockTicks() {
    if(portClockPaused)return portClockFrozenTicks;
    const uint64_t ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-portClockEpoch).count();
    // Preserve the original platform ABI: 9.216 MHz ticks, unsigned64 in v0.
    return (ns/1000000000ull)*9216000ull+(ns%1000000000ull)*9216000ull/1000000000ull;
}
void nativeTimeGet(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    NativeVideo::diagnosticMenuTick(ram,cpu,runtime);
    SET_GPR_U64(cpu,2,ReplayHarness::clockTicks(portClockTicks()));
    cpu->pc = GPR_U32(cpu,31);
}
double threadCpuSeconds() {
#ifdef __SWITCH__
    uint64_t ticks=0;
    if(R_FAILED(svcGetInfo(&ticks,InfoType_ThreadTickCount,CUR_THREAD_HANDLE,TickCountInfo_Total)))
        throw std::runtime_error("Thread CPU timing is unavailable");
    return double(ticks)/19200000.0;
#else
    timespec value{};clock_gettime(CLOCK_THREAD_CPUTIME_ID,&value);
    return double(value.tv_sec)+double(value.tv_nsec)*1e-9;
#endif
}
void profileRasterShow(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    // iCameraShowRaster -> RwCameraShowRaster -> RwRasterShowRaster.
    // Count this caller once on entry; resumed interior PCs are not frames.
    if(cpu->pc==0x1eb4a8 && GPR_U32(cpu,31)==0x1e6180) {
        const auto now=std::chrono::steady_clock::now();
        const uint64_t gameClock=portClockTicks();
        if(cameraFrames++==0) {firstCameraFrame=lastFrameReport=now;lastCameraFrames=1;firstCameraCpu=lastReportCpu=threadCpuSeconds();lastReportGameClock=gameClock;}
        const double seconds=std::chrono::duration<double>(now-lastFrameReport).count();
        if(seconds>=2) {
            std::cout<<"[game-frames] total="<<cameraFrames<<" interval-fps="
                     <<double(cameraFrames-lastCameraFrames)/seconds<<" seconds="<<seconds
                     <<" game-thread-cpu-ms/frame="<<(threadCpuSeconds()-lastReportCpu)*1000/double(cameraFrames-lastCameraFrames)
                     <<" game-clock-rate="<<double(gameClock-lastReportGameClock)/9216000.0/seconds<<'\n';
            lastReportCpu=threadCpuSeconds();
            lastReportGameClock=gameClock;
            lastFrameReport=now;lastCameraFrames=cameraFrames;
        }
    }
    rasterShowOriginal(ram,cpu,runtime);
}
PS2Runtime::RecompiledFunction rwStartOriginal = nullptr;
PS2Runtime::RecompiledFunction rwInstanceOriginal = nullptr;
PS2Runtime::RecompiledFunction rwStateOriginal = nullptr;
PS2Runtime::RecompiledFunction rwTextureOriginal = nullptr;
PS2Runtime::RecompiledFunction rwCacheOriginal = nullptr;
unsigned rwTraceCount = 0;
void traceRwCache(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    static unsigned count = 0;
    if (cpu->pc == 0x206eb0 && count++ < 20) {
        const uint32_t raster = GPR_U32(cpu, 4);
        const uint32_t plugin = raster + runtime->Load32(ram, cpu, GPR_U32(cpu, 28) - 0x7b10);
        std::cout << "[night100:rw-cache] raster=0x" << std::hex << raster
                  << " mode=" << GPR_U32(cpu, 5) << " flags=" << runtime->Load32(ram, cpu, plugin+0x34)
                  << " entry=" << runtime->Load32(ram, cpu, plugin+0x58)
                  << " size=" << runtime->Load32(ram, cpu, plugin+0x30) << std::dec << '\n';
    }
    rwCacheOriginal(ram, cpu, runtime);
}
void traceRwTexture(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    static unsigned count = 0;
    rwTextureOriginal(ram, cpu, runtime);
    if (cpu->pc == 0x1f5264 && count++ < 20)
        std::cout << "[night100:rw-texture] result=" << GPR_U32(cpu, 2) << '\n';
}
void traceRwInstance(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    std::cout << "[night100:rw-instance] pc=0x" << std::hex << cpu->pc
              << " a0=0x" << GPR_U32(cpu, 4) << " global=0x" << runtime->Load32(ram, cpu, 0x2b5ad0)
              << std::dec << '\n';
    rwInstanceOriginal(ram, cpu, runtime);
}
void traceRwState(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    static unsigned count = 0;
    if (cpu->pc == 0x1ddc48 && count++ < 64) {
        std::cout << "[night100:rw-state] state=" << GPR_U32(cpu, 4) << " value=0x" << std::hex
                  << GPR_U32(cpu, 5) << " instance=0x" << runtime->Load32(ram, cpu, 0x2b5ad0)
                  << std::dec << '\n';
    }
    rwStateOriginal(ram, cpu, runtime);
}
void traceRwStart(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    if (rwTraceCount++ < 20) {
        std::cout << "[night100:rw-start] enter pc=0x" << std::hex << cpu->pc
                  << " ra=0x" << GPR_U32(cpu, 31) << " sp=0x" << GPR_U32(cpu, 29)
                  << " saved=0x" << runtime->Load32(ram, cpu, GPR_U32(cpu, 29)+64)
                  << std::dec << '\n';
    }
    rwStartOriginal(ram, cpu, runtime);
    if (rwTraceCount < 20) {
        std::cout << "[night100:rw-start] leave pc=0x" << std::hex << cpu->pc
                  << " ra=0x" << GPR_U32(cpu, 31) << " sp=0x" << GPR_U32(cpu, 29)
                  << std::dec << '\n';
    }
}
void discReady(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    cpu->pc = GPR_U32(cpu, 31);
    ps2_stubs::sceCdDiskReady(ram, cpu, runtime);
}

void resetGraph(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    const uint32_t mode = GPR_U32(cpu, 4);
    const uint16_t parameters[] = {static_cast<uint16_t>(GPR_U32(cpu, 5) & 1u),
                                   static_cast<uint16_t>(GPR_U32(cpu, 6) & 0xffu),
                                   static_cast<uint16_t>(GPR_U32(cpu, 7) != 0u)};
    cpu->pc = GPR_U32(cpu, 31);
    ps2_stubs::sceGsResetGraph(ram, cpu, runtime);
    // This SDK's original sceGsGetGParam returns four 16-bit fields at 0x288a40.
    // ResetGraph HLE keeps a separate host copy; keep the guest's copy coherent.
    if (mode == 0 || mode == 5) {
        std::memcpy(ram + 0x288a40, parameters, sizeof(parameters));
        std::cout << "[night100] GS mode interlace=" << parameters[0]
                  << " standard=" << parameters[1] << " frame=" << parameters[2] << '\n';
    }
}

void skipVideo(uint8_t*, R5900Context* cpu, PS2Runtime*) {
    std::cout << "[night100] skipped FMV (--skip-videos)\n";
    setReturnS32(cpu, 1);
    cpu->pc = GPR_U32(cpu, 31);
}

void applyScooby(PS2Runtime& runtime) {
    // _start declares [0x01ff0000, 0x02000000) as the main EE stack.
    // The runtime's async callback arena also grows down from 0x02000000.
    // Exclude that occupied 64 KiB before it allocates interrupt stacks.
    runtime.reserveAsyncCallbackStack(0x10000, 16);
    // A native port uses the host's monotonic platform clock. The legacy
    // 16-bit EE overflow interrupt can coalesce while waiting for a host
    // vblank, losing elapsed game time and desynchronizing animation/audio.
    portClockEpoch=std::chrono::steady_clock::now();
    // NIGHT100_IOP_CLOCK=deterministic credits the IOP from guest EE cycles
    // (1 IOP cycle per 8 EE cycles, carried exactly) instead of host time.
    // Experimental: guest EE cycles advance only at checkpoints and vblank
    // waits, so IOP/audio rate follows guest progress rather than wall time
    // while the guest is CPU-bound. The Switch has no environment; the device
    // experiment must flip kDeterministicIopClock instead.
    constexpr bool kDeterministicIopClock=false;
    bool deterministicIop=kDeterministicIopClock;
    if(const char* mode=std::getenv("NIGHT100_IOP_CLOCK")) {
        if(!std::strcmp(mode,"deterministic"))deterministicIop=true;
        else if(!std::strcmp(mode,"realtime"))deterministicIop=false;
    }
    runtime.setRealtimeIopClock(!deterministicIop);
    runtime.replaceFunction(0x1264d0,nativeTimeGet);
    gameLoopOriginal=runtime.lookupFunction(0x131da8);
    endRenderFxOriginal=runtime.lookupFunction(0x13c7c0);
    if(!gameLoopOriginal || !endRenderFxOriginal || !runtime.replaceFunction(0x131da8,waitGameFrame)
        || !runtime.replaceFunction(0x131d98,beginGameFrameWait)
        || !runtime.replaceFunction(0x13c7c0,endRenderFxBeforeFrameWait))
        throw std::runtime_error("Cannot install 60 Hz game frame pacing");
    std::cout<<"[night100] game loop uses one-vblank 60 Hz pacing without busy waiting\n";
    installNativeScene(runtime,nullptr);
    if(!std::getenv("NIGHT100_REFERENCE_CPU"))installNativeCpu(runtime);
    installNativeParticles(runtime);
    installNativeVU1(runtime);
#ifndef __SWITCH__
    CollisionDebug::install(runtime);
#endif
    // Verified by the 0x8000059a CDVD RPC ID and the SDK's diagnostic strings.
    // The scanner misses this SDK revision. Route it to the runtime's disc HLE.
    runtime.replaceFunction(0x101220, discReady);
    runtime.replaceFunction(0x102948, resetGraph);
    runtime.replaceFunction(0x270460,std::getenv("NIGHT100_SKIP_VIDEOS")?skipVideo:NativeVideo::play);
    NativeVideo::installAttractDiagnostic(runtime);
    if (std::getenv("NIGHT100_PROFILE_FRAMES")) {
        cameraFrames=lastCameraFrames=0;
        rasterShowOriginal=runtime.lookupFunction(0x1eb4a8);
        for(uint32_t pc=0x1eb4a8;pc<0x1eb510;pc+=4)
            if(runtime.hasFunction(pc) && runtime.lookupFunction(pc)==rasterShowOriginal)
                runtime.replaceFunction(pc,profileRasterShow);
    }
    if (std::getenv("NIGHT100_TRACE_RW")) {
        const auto bindTrace = [&](uint32_t start, uint32_t end,
                                   PS2Runtime::RecompiledFunction& original,
                                   PS2Runtime::RecompiledFunction trace) {
            original = runtime.lookupFunction(start);
            for (uint32_t pc = start; pc < end; pc += 4)
                if (runtime.hasFunction(pc) && runtime.lookupFunction(pc) == original)
                    runtime.replaceFunction(pc, trace);
        };
        bindTrace(0x1e6810, 0x1e6870, rwInstanceOriginal, traceRwInstance);
        bindTrace(0x1ddc48, 0x1ddcf8, rwStateOriginal, traceRwState);
        bindTrace(0x207600, 0x2079d0, rwTextureOriginal, traceRwTexture);
        bindTrace(0x206eb0, 0x2073d0, rwCacheOriginal, traceRwCache);
        rwStartOriginal = runtime.lookupFunction(0x1e6e58);
        for (uint32_t pc = 0x1e6e58; pc < 0x1e6f58; pc += 4) {
            if (runtime.hasFunction(pc) && runtime.lookupFunction(pc) == rwStartOriginal)
                runtime.replaceFunction(pc, traceRwStart);
        }
    }
#ifndef __SWITCH__
    installGameBenchmark(runtime);
#endif
    installInputReplay(runtime);
    // Last: wraps the frame-wait/EndRenderFX overrides above.
    FramePhases::install(runtime);
#ifndef __SWITCH__
    ParticleWatch::install(runtime); // diagnostic; wraps every replacement above
#endif
    std::cout << "[night100] installed retail sceCdDiskReady binding\n";
}
}

void printGameFrameMetrics() {
    if(!cameraFrames) return;
    const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-firstCameraFrame).count();
    std::cout<<"[game-frames] camera-present-calls="<<cameraFrames
             <<" elapsed-since-first="<<seconds<<" average-fps="<<double(cameraFrames-1)/seconds<<'\n';
}

uint64_t nativePortClockSnapshot(){return portClockTicks();}
void nativePortClockRestore(uint64_t ticks) {
    if(portClockPaused){portClockFrozenTicks=ticks;return;}
    constexpr uint64_t hz=9216000;
    if(ticks/hz>uint64_t(INT64_MAX)/1000000000ull)throw std::runtime_error("Invalid snapshot game clock");
    const uint64_t ns=(ticks/hz)*1000000000ull+(ticks%hz)*1000000000ull/hz;
    portClockEpoch=std::chrono::steady_clock::now()-std::chrono::nanoseconds(ns);
}
void pauseNativePortClock(){if(!portClockPaused){portClockFrozenTicks=portClockTicks();portClockPaused=true;}}
void resumeNativePortClock(){if(portClockPaused){const auto ticks=portClockFrozenTicks;portClockPaused=false;nativePortClockRestore(ticks);}}
void excludeNativePortClock(uint64_t nanoseconds) {
    if (!portClockPaused)portClockEpoch += std::chrono::nanoseconds(nanoseconds);
}

PS2_REGISTER_GAME_OVERRIDE("Scooby USA retail", "SLUS_203.49", 0x100008, 0x860e1336, applyScooby)
