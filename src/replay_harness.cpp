#include "replay_harness.h"
#include "frame_telemetry.h"
#include "replay_catalog.h"
#include "replay_clock.h"
#if !defined(__SWITCH__)
#include "collision_debug.h"
#endif
#include "ps2_runtime.h"
#include "machine_snapshot.h"
#include "runtime/ee_scheduler.h"
#include "runtime/state_archive.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <memory>
#include <thread>
#include <unistd.h>

namespace ReplayHarness {
namespace {
// frames.csv schema 2 appends the zGameLoop phase split (frame_phases.h).
// Readers select columns by name; schema-1 files simply lack these.
void phaseHeader(std::ostream& out) {
    out<<",frame_schema";
    for(const char* name:FramePhases::names)out<<','<<name<<"_ms";
    for(const auto phase:FramePhases::cpuPhases)out<<','<<FramePhases::names[phase]<<"_cpu_ms";
}
void phaseColumns(std::ostream& out,const FramePhases::Frame& phases) {
    out<<','<<FramePhases::schema;
    for(const double ms:phases.ms){out<<',';if(phases.valid)out<<ms;}
    for(const auto phase:FramePhases::cpuPhases){out<<',';if(phases.valid && phases.cpuMs[phase]>=0)out<<phases.cpuMs[phase];}
}
struct Manager {
    Settings settings;
    Backend* gpu=nullptr;
    Hooks hooks;
    FrameTelemetry telemetry;
    std::unique_ptr<InputReplay::Session> journal,playback;
    std::unique_ptr<ReplayClock> clock;
    std::optional<uint64_t> lastClock;
    std::filesystem::path replayRequest;
    std::mutex mutex;
    std::string status;
    std::atomic<unsigned> commands{0},neutralFrames{0};
    std::atomic<bool> overlay{false},picker{false},recording{false},playing{false},busy{false};
    std::atomic<uint64_t> visibleSession{0},statusRevision{0};
    ReplayCatalog::Entry entry;
    ReplayCatalog::Metadata metadata;
    std::ofstream timings;
    // Rows are batched in memory and written in 64-frame batches (see drainTimings),
    // so the SD card sees a few large writes instead of an IPC per frame.
    std::ostringstream pendingTimings;
    void drainTimings() {
        if(timings.is_open()) {timings<<pendingTimings.str();timings.flush();}
        pendingTimings.str(std::string());pendingTimings.clear();
    }
    bool timingActive=false;
    bool startPending=false,failedVerification=false;
    unsigned startRetries=0;
    uint64_t completedFrames=0;
    std::filesystem::path replayResults;
    void say(std::string value){std::lock_guard lock(mutex);status=std::move(value);statusRevision.fetch_add(1,std::memory_order_release);std::cout<<"[sessions] "<<status<<std::endl;}
    void image(PS2Runtime* runtime,const std::filesystem::path& path) {
        if(hooks.captureImage){hooks.captureImage(*runtime,path);return;}
        std::vector<uint8_t> pixels;uint32_t w=0,h=0;
        if(!runtime->gs().copyLatchedHostPresentationFrame(pixels,w,h) || pixels.size()!=size_t(w)*h*4)
            throw std::runtime_error("Completed game image is unavailable");
        std::ofstream out(path,std::ios::binary);out<<"P6\n"<<w<<' '<<h<<"\n255\n";
        for(size_t i=0;i<pixels.size();i+=4)out.write(reinterpret_cast<const char*>(pixels.data()+i),3);
        if(!out)throw std::runtime_error("Cannot save replay checkpoint image");
    }
    void start(uint8_t* ram,PS2Runtime* runtime) {
        if(recording.load()){busy.store(false);say("Already recording replay "+std::to_string(entry.id));return;}
        if(playback){busy.store(false);say("Wait for the replay to finish before recording.");return;}
        if(!startPending){entry=ReplayCatalog::reserve(settings.directory);visibleSession.store(entry.id);startPending=true;startRetries=0;}
        saveMachineSnapshot(*runtime,entry.directory/"state.bin");
        metadata={};metadata.id=entry.id;metadata.options=settings.options;
        metadata.startFrame=metadata.endFrame=0;
        metadata.start=metadata.end=ReplayCatalog::capture(ram,PS2_RAM_SIZE);
        ReplayCatalog::save(entry,metadata);
        std::ofstream state(entry.directory/"start-state.txt");
        state<<"Full machine snapshot: state.bin\nInput frame zero resumes directly from that snapshot.\n";
        state.close();if(!state)throw std::runtime_error("Cannot write starting checkpoint");
        image(runtime,entry.directory/"start.ppm");
        journal=std::make_unique<InputReplay::Session>(InputReplay::Mode::Record,entry.directory/"input.inputs",settings.options);
        clock=std::make_unique<ReplayClock>(InputReplay::Mode::Record,entry.directory/"clock.bin");
        timings.open(entry.directory/"frames.csv");
        if(!timings)throw std::runtime_error("Cannot write recording frame timings");
        timings<<"frame,clip_frame,wall_ms,cpu_ms,process_cpu_ms,gpu_ms,gpu_queries,readbacks,texture_uploads,timing_valid";
        phaseHeader(timings);timings<<'\n';
        telemetry.reset();startPending=false;startRetries=0;recording.store(true);busy.store(false);say("Recording replay "+std::to_string(entry.id));
    }
    void stop(uint8_t* ram,PS2Runtime* runtime) {
        if(!recording.load()){busy.store(false);say("No active recording.");return;}
        if(journal->frame()<=metadata.startFrame){commands.fetch_or(2);return;}
        metadata.endFrame=journal->frame();metadata.end=ReplayCatalog::capture(ram,PS2_RAM_SIZE);
        saveMachineSnapshot(*runtime,entry.directory/"end-state.bin");
        image(runtime,entry.directory/"end.ppm");
        clock->finish(journal->frame());clock.reset();journal->finish();journal.reset();
        drainTimings();timings.flush();timings.close();if(!timings)throw std::runtime_error("Cannot finish recording frame timings");
        metadata.complete=true;ReplayCatalog::save(entry,metadata);recording.store(false);busy.store(false);
        telemetry.reset();
        say("Saved replay "+std::to_string(entry.id)+" ("+std::to_string(metadata.endFrame-metadata.startFrame)+" game frames)");
    }
    void verify(uint8_t* ram,PS2Runtime* runtime) {
        clock->finish(playback->frame());clock.reset();
        if(lastClock)nativePortClockRestore(*lastClock);
        const auto actual=ReplayCatalog::capture(ram,PS2_RAM_SIZE);
        const auto comparison=ReplayCatalog::compare(metadata.end,actual);
        saveMachineSnapshot(*runtime,replayResults/"actual-state.bin");
        image(runtime,replayResults/"actual-end.ppm");
        std::ofstream report(replayResults/"verification.json");report<<comparison.json<<'\n';report.close();
        if(!report)throw std::runtime_error("Cannot save endpoint verification");
        // Cumulative VU1 microcode counters for this clip (see patches/zzzz-vu1-stats.patch).
        std::cout<<"[vu1-stats] replay="<<entry.id<<" "<<formatVU1MicrocodeStats(runtime->memory().vu1MicrocodeStats())<<"\n";
        // Per-program VU1 histogram (see patches/zzzzz-vu1-program-histogram.patch).
        for(const std::string& line:formatVU1ProgramHistogram(runtime->memory().vu1ProgramHistogram(),static_cast<int>(entry.id)))std::cout<<line<<"\n";
        failedVerification=!comparison.matches;playback.reset();playing.store(false);busy.store(false);drainTimings();timings.close();
        telemetry.reset();
        say("Replay "+std::to_string(entry.id)+(comparison.matches?" verified: endpoint matches":" DRIFT: endpoint differs")+" — "+replayResults.string());
    }
};
std::unique_ptr<Manager> manager;
}
void configure(Settings settings,Backend* gpu,Hooks hooks) {
    manager=std::make_unique<Manager>();manager->settings=std::move(settings);manager->gpu=gpu;
    manager->hooks=std::move(hooks);manager->telemetry.configure(gpu);
}
UiState uiState() {
    if(!manager)return {};
    return {manager->recording.load(),manager->playing.load(),manager->busy.load(),
            manager->visibleSession.load(),manager->statusRevision.load(std::memory_order_acquire)};
}
void excludeHostTime(uint64_t nanoseconds) {
    FramePhases::excludeHostTime(nanoseconds);
    if(manager)manager->telemetry.excludeHostTime(nanoseconds);
}
bool enabled(){return manager && manager->settings.enabled;}
void install() {
    if(!enabled())return;
    auto& m=*manager;
    if(!m.settings.replay.empty()) {
        const auto name=m.settings.replay.filename().string();
        if(!name.starts_with("session-"))throw std::runtime_error("Invalid numbered replay directory");
        m.entry={std::stoull(name.substr(8)),m.settings.replay};m.visibleSession.store(m.entry.id);m.metadata=ReplayCatalog::load(m.entry);
        if(!m.metadata.complete || m.metadata.options!=m.settings.options)throw std::runtime_error("Replay startup options do not match");
        m.playback=std::make_unique<InputReplay::Session>(InputReplay::Mode::Replay,m.entry.directory/"input.inputs",m.settings.options);
        m.clock=std::make_unique<ReplayClock>(InputReplay::Mode::Replay,m.entry.directory/"clock.bin");
        m.playing.store(true);
        const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
        m.replayResults=m.entry.directory/"replays"/(std::to_string(stamp)+"-"+std::to_string(getpid()));
        if(!std::filesystem::create_directories(m.replayResults))throw std::runtime_error("Cannot reserve replay result directory");
        m.timings.open(m.replayResults/"frames.csv");
        m.timings<<"frame,wall_ms,cpu_ms,process_cpu_ms,gpu_ms";phaseHeader(m.timings);m.timings<<'\n';
        m.say("Playing replay "+std::to_string(m.entry.id)+" from its saved machine state");
    }
}
SessionUi::Actions actions() {
    SessionUi::Actions result;
    result.setOverlayVisible=[](bool value){if(manager)manager->overlay.store(value);};
    result.setPickerOpen=[](bool value){if(manager){manager->picker.store(value);if(!value)manager->neutralFrames.store(6);}};
    result.metrics=[] {return manager?manager->telemetry.snapshot():SessionUi::Metrics{};};
    result.status=[] {if(!manager)return std::string{};std::lock_guard lock(manager->mutex);return manager->status;};
    result.startRecording=[] {if(enabled()){manager->busy.store(true);manager->commands.fetch_or(1);}else if(manager)manager->say("Recording requires native engine play.");};
    result.stopRecording=[] {if(enabled()){manager->busy.store(true);manager->commands.fetch_or(2);}};
    result.recordings=[] {
        std::vector<SessionUi::Recording> result;if(!manager)return result;
        if(manager->recording.load() || manager->busy.load())throw std::runtime_error("Finish saving the current recording first.");
        for(const auto& entry:ReplayCatalog::list(manager->settings.directory)) {
            const auto metadata=ReplayCatalog::load(entry);
            result.push_back({entry.directory,std::to_string(metadata.endFrame-metadata.startFrame)+" game frames",entry.id});
        }
        return result;
    };
    result.replay=[](const auto& path) {
        if(!enabled())throw std::runtime_error("Replay requires native engine play.");
        if(manager->recording.load() || manager->busy.load())throw std::runtime_error("Finish saving the current recording first.");
        std::lock_guard lock(manager->mutex);manager->replayRequest=path;
    };
    return result;
}
void pad(InputReplay::Pad& pad,const InputReplay::Context& context) {
    if(!enabled())return;
    auto& m=*manager;
    if(m.playback)m.playback->pad(pad,context);
    if(!m.playback && (m.picker.load() || m.neutralFrames.load())) {
        pad.bytes[2]=pad.bytes[3]=255;
        for(unsigned i=4;i<8;++i)pad.bytes[i]=128;
        for(unsigned i=8;i<32;++i)pad.bytes[i]=0;
    }
    if(m.journal)m.journal->pad(pad,context);
}
void completed(uint8_t* ram,PS2Runtime* runtime) {
    if(!manager)return;
    auto& m=*manager;++m.completedFrames;
    const bool timing=m.overlay.load() || m.recording.load() || bool(m.playback);
#if !defined(__SWITCH__)
    const auto sample=m.telemetry.completed();
    if(m.gpu && timing!=m.timingActive){m.gpu->setTimingEnabled(timing);m.timingActive=timing;}
#else
    // Switch already collects ordinary HUD telemetry. Keep this recorder's
    // extra CPU tick query off frames that have no active replay/recording.
    if(timing!=m.timingActive){m.telemetry.reset();m.timingActive=timing;}
    const auto sample=timing?m.telemetry.completed():FrameTelemetry::Sample{};
#endif
    if(!enabled())return;
    const auto context=ReplayCatalog::capture(ram,PS2_RAM_SIZE).context;
    if(m.journal){m.journal->completed(context);if(m.clock)m.clock->completed(m.journal->frame());}
    if(m.playback) {
        m.playback->completed(context);
#if !defined(__SWITCH__)
        CollisionDebug::completed(ram,m.playback->frame());
#endif
        if(const char* dumps=std::getenv("NIGHT100_REPLAY_FRAME_DUMPS")) {
            const std::filesystem::path directory(dumps);
            std::filesystem::create_directories(directory);
            const auto path=directory/(std::to_string(m.playback->frame())+".ppm");
            if(std::filesystem::exists(path))throw std::runtime_error("Replay frame dump already exists: "+path.string());
            m.image(runtime,path);
        }
        auto& rows=m.pendingTimings;
        rows<<m.playback->frame()<<','<<sample.wallMs<<',';
        if(sample.cpuMs>=0)rows<<sample.cpuMs;rows<<',';
        if(sample.processCpuMs>=0)rows<<sample.processCpuMs;rows<<',';
        if(sample.gpuMs)rows<<*sample.gpuMs;
        phaseColumns(rows,sample.phases);rows<<'\n';
        if(m.playback->frame()%64==0)m.drainTimings();
        if(m.playback->ended())runtime->eeScheduler().atSafePoint([ram,runtime]{manager->verify(ram,runtime);});
    }
    if(m.recording.load()) {
        auto& rows=m.pendingTimings;
        rows<<m.journal->frame()<<','<<m.journal->frame()-m.metadata.startFrame<<','<<std::fixed<<std::setprecision(6)
            <<sample.wallMs<<',';
        if(sample.cpuMs>=0)rows<<sample.cpuMs;rows<<',';
        if(sample.processCpuMs>=0)rows<<sample.processCpuMs;rows<<',';
        if(sample.gpuMs)rows<<*sample.gpuMs;
        rows<<','<<sample.stats.gpuSamples<<','<<sample.stats.surfaceReadbacks<<','<<sample.stats.textureUploads<<','<<(sample.wallMs>0);
        phaseColumns(rows,sample.phases);rows<<'\n';
        if(m.journal->frame()%64==0)m.drainTimings();
    }
    if(const char* test=std::getenv("NIGHT100_SESSION_CAPTURE_FRAMES")) {
        unsigned long long first=0,last=0;
        if(std::sscanf(test,"%llu:%llu",&first,&last)==2) {
            if(m.completedFrames==first)m.commands.fetch_or(1);
            if(m.completedFrames==last)m.commands.fetch_or(2);
        }
    }
    const auto commands=m.commands.exchange(0);
    if(commands)runtime->eeScheduler().atSafePoint([ram,runtime,commands] {
        auto& m=*manager;
        try {if(commands&1)m.start(ram,runtime);if(commands&2)m.stop(ram,runtime);}
        catch(const ps2x::state::NotReady& error) {
            if(++m.startRetries<300){m.commands.fetch_or(commands);if(m.startRetries==1 || m.startRetries%60==0)m.say(std::string("Waiting for a serializable frame: ")+error.what());}
            else if((commands&2) && m.recording.load()) {
                m.startRetries=0;m.busy.store(false);
                m.say(std::string("Still recording; retry save when the frame is serializable: ")+error.what());
            } else {m.startPending=false;m.recording.store(false);m.busy.store(false);m.journal.reset();m.clock.reset();m.drainTimings();m.timings.close();m.say(std::string("Snapshot unavailable after 300 frames: ")+error.what());}
        }
        catch(const std::exception& error){m.startPending=false;m.recording.store(false);m.busy.store(false);m.journal.reset();m.clock.reset();m.drainTimings();m.timings.close();m.say(std::string("Capture failed: ")+error.what());}
    });
    if(m.neutralFrames.load())m.neutralFrames.fetch_sub(1);
    if(m.picker.load()) {
        runtime->pauseStateClocks();
        while(m.picker.load() && !runtime->isStopRequested()) {
            runtime->serviceHostLifecycle();
            if(runtime->isStopRequested())break;
            if(m.hooks.presentPaused)m.hooks.presentPaused();
            std::this_thread::sleep_for(std::chrono::milliseconds(m.hooks.presentPaused?33:10));
        }
        runtime->resumeStateClocks();m.telemetry.reset();
    }
}
void watchdog(PS2Runtime* runtime) {
    if(!manager)return;
    std::lock_guard lock(manager->mutex);if(!manager->replayRequest.empty())runtime->requestStop();
}
void finish(const std::string& reason) {
    if(!manager)return;
    if(manager->journal)manager->journal->finish(reason);manager->drainTimings();
    if(manager->playback){manager->failedVerification=true;std::ofstream report(manager->replayResults/"verification.json");report<<"{\"matches\":false,\"error\":\"Replay stopped before its endpoint\"}\n";}
    if(manager->recording.load())manager->say("Recording interrupted before saving; incomplete capture retained at "+manager->entry.directory.string());
}
std::filesystem::path requestedReplay(){if(!manager)return {};std::lock_guard lock(manager->mutex);return manager->replayRequest;}
bool verificationFailed(){return manager && manager->failedVerification;}
void restored(PS2Runtime* runtime) {
    if(!manager || !manager->playback)return;
    const auto comparison=ReplayCatalog::compare(manager->metadata.start,ReplayCatalog::capture(runtime->memory().getRDRAM(),PS2_RAM_SIZE));
    std::ofstream report(manager->replayResults/"start-verification.json");report<<comparison.json<<'\n';report.close();
    if(!report || !comparison.matches)throw std::runtime_error("Restored starting checkpoint does not match its recording");
}
uint64_t clockTicks(uint64_t actual) {
    if(!manager || !manager->clock)return actual;
    auto& m=*manager;const uint64_t frame=m.journal?m.journal->frame():m.playback->frame();
    const auto ticks=m.clock->sample(frame,actual);m.lastClock=ticks;return ticks;
}
}
