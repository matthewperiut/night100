#pragma once
// Per-frame wall-clock split of the retail zGameLoop (exact ELF) at its call
// boundaries: pad, zSceneUpdate, the post-update services, zSceneRender, the
// render tail, the pacing wait and presentation. One monotonic clock read per
// boundary; the thread CPU clock is read only when entering or leaving the
// update, render and present phases (present's wall minus CPU is the
// vertical-blank and display wait). Producer (game) thread only: marks come from guest
// entry hooks and frame closes from the completed-frame callback.
#include <array>
#include <cstdint>
#if defined(__SWITCH__)
#include <switch.h>
#else
#include <chrono>
#include <ctime>
#endif

class PS2Runtime;
struct R5900Context;

namespace FramePhases {
// Order of a zGameLoop iteration after the previous completed frame.
enum Phase : uint8_t {
    Other,       // completion return path, menus and loops other than zGameLoop
    PostPresent, // zLaughUpdate, zMusicUpdate, zGameUpdateMode, loop top, dt
    Pad,         // xPadUpdate, zGameCheats, zGameExtras, profiling stubs
    Update,      // zSceneUpdate
    PostUpdate,  // zEntShaggy_UpdateDep, frame callback, sound, HUD, cutscene/camera update
    RenderPrep,  // xCameraBegin, zVolume_OccludePrecalc
    Render,      // zSceneRender
    RenderEnd,   // xDebugUpdate, xDrawEnd, iDebugScreenDump
    CameraEnd,   // xCameraEnd
    FxEnd,       // iEnvEndRenderFX
    Wait,        // one-vblank pacing wait
    Present,     // xCameraShowRaster through the drained completed frame
    Count
};
// frames.csv columns are "<name>_ms", in Phase order.
inline constexpr std::array<const char*,Count> names{
    "other","post_present","pad","update","post_update","render_prep","render",
    "render_end","camera_end","fx_end","wait","present"};
// frames.csv frame_schema value; files without that column are schema 1.
// 2: zGameLoop phase columns. 3: Switch rows also carry deko3d gpu_ms.
inline constexpr unsigned schema=3;

inline constexpr bool cpuPhase(Phase phase){return phase==Update || phase==Render || phase==Present;}
// frames.csv "<name>_cpu_ms" columns, in this order.
inline constexpr std::array<Phase,3> cpuPhases{Update,Render,Present};
struct Frame {
    std::array<double,Count> ms{};
    // Thread CPU of cpuPhase() phases; -1 when no interval closed or the clock failed.
    std::array<double,Count> cpuMs{};
    bool valid=false;
    Frame(){cpuMs.fill(-1);}
};
struct Totals {
    std::array<uint64_t,Count> ns{},cpuNs{},cpuIntervals{};
    uint64_t cpuFailures=0;
};
constexpr uint64_t unavailable=~uint64_t(0);

inline uint64_t wallNanoseconds() {
#if defined(__SWITCH__)
    return armTicksToNs(armGetSystemTick());
#else
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
inline uint64_t threadCpuNanoseconds() {
#if defined(__SWITCH__)
    uint64_t ticks=0;
    return R_SUCCEEDED(svcGetInfo(&ticks,InfoType_ThreadTickCount,CUR_THREAD_HANDLE,TickCountInfo_Total))?
        armTicksToNs(ticks):unavailable;
#else
    timespec value{};
    return clock_gettime(CLOCK_THREAD_CPUTIME_ID,&value)==0?
        uint64_t(value.tv_sec)*1000000000ull+uint64_t(value.tv_nsec):unavailable;
#endif
}

struct State {
    bool installed=false;
    Phase current=Other;
    uint64_t lastWall=0;
    uint64_t cpuStart=unavailable;
    Totals totals,closed;
};
inline State state;

// Ends the running phase at now and starts `next`. Marking the running phase
// again only folds the elapsed time into it.
inline void markAt(Phase next,uint64_t now,uint64_t cpu) {
    auto& s=state;
    if(s.lastWall && now>s.lastWall)s.totals.ns[s.current]+=now-s.lastWall;
    if(s.current!=next && (cpuPhase(s.current) || cpuPhase(next))) {
        if(cpuPhase(s.current)) {
            if(s.cpuStart!=unavailable && cpu!=unavailable && cpu>=s.cpuStart) {
                s.totals.cpuNs[s.current]+=cpu-s.cpuStart;++s.totals.cpuIntervals[s.current];
            } else ++s.totals.cpuFailures;
        }
        s.cpuStart=cpuPhase(next)?cpu:unavailable;
    }
    s.lastWall=now;s.current=next;
}
inline void mark(Phase next) {
    const bool needCpu=state.current!=next && (cpuPhase(state.current) || cpuPhase(next));
    markAt(next,wallNanoseconds(),needCpu?threadCpuNanoseconds():unavailable);
}
// Completed game frame boundary: closes the running phase and enters Other,
// so loops other than zGameLoop never extend the last named phase.
inline const Totals& boundary(){mark(Other);return state.totals;}
inline Frame difference(const Totals& current,const Totals& previous) {
    Frame frame;frame.valid=state.installed;
    for(unsigned n=0;n<Count;++n) {
        frame.ms[n]=double(current.ns[n]-previous.ns[n])*1e-6;
        if(current.cpuIntervals[n]!=previous.cpuIntervals[n])frame.cpuMs[n]=double(current.cpuNs[n]-previous.cpuNs[n])*1e-6;
    }
    return frame;
}
// The split of the game frame that just completed (since the previous close).
inline Frame closeFrameAt(uint64_t now,uint64_t cpu) {
    markAt(Other,now,cpu);
    const Frame frame=difference(state.totals,state.closed);
    state.closed=state.totals;return frame;
}
inline Frame closeFrame() {
    const bool needCpu=cpuPhase(state.current);
    return closeFrameAt(wallNanoseconds(),needCpu?threadCpuNanoseconds():unavailable);
}
// Host pauses (focus loss, the replay picker) are not phase time.
inline void excludeHostTime(uint64_t nanoseconds){if(state.lastWall)state.lastWall+=nanoseconds;}
inline void resetForTesting(bool installed){state=State{};state.installed=installed;}

// Installs the zGameLoop boundary hooks for the exact retail ELF. Call after
// every other frame-loop override (the wait/FX hooks wrap the pacing override).
void install(PS2Runtime&);
// Called at a recognized boundary before it is marked. Returning true means
// the hook redirected the guest (cpu->pc) and will re-enter this boundary
// later; the boundary is then neither marked nor executed now.
using BoundaryHook=bool(*)(Phase ended,Phase next,uint8_t* ram,::R5900Context* cpu,::PS2Runtime* runtime);
void setBoundaryHook(BoundaryHook);
}
