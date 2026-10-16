#include "game_benchmark.h"
#include "input_replay.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "graphics/gl33_backend.h"
#include "runtime/ee_scheduler.h"
#include <chrono>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <ctime>

namespace GameBenchmark {
using Clock=std::chrono::steady_clock;
Clock::time_point start,lastFrame;
double lastCpu=0,lastProcessCpu=0;
uint64_t frames=0,titleFrames=0,playFrames=0,introFrames=0;
unsigned inputStep=0;
uint64_t pressFrame=0;
bool scripted=false,nativeCompletions=false;
unsigned manorStep=0;
uint64_t manorReadyFrame=0;
uint64_t interiorPlayFrames=0;
bool snackWarped=false;
unsigned batsStep=0;
uint64_t batsStepFrame=0,batsTriggeredFrame=0,batsWallFrame=0;
uint32_t batsKey=0,batsTrigger=0,batsWall=0,batsWallHanging=0;
uint32_t batsLastKeyState=~0u;
bool batsWallTranslucent=false,batsWallFaded=false,batsTurnedRight=false;
bool batsDialogueActive=false;
uint64_t batsDialogueFrame=0,batsDialogueCycle=~0ull;
Clock::time_point batsDialogueStart;
std::filesystem::path directory;
std::ofstream samples,events;
PS2Runtime::RecompiledFunction benchmarkShowOriginal,padOriginal,sceneOriginal,loopOriginal,cutsceneOriginal,portalOriginal;
struct LoadingStage {
    uint32_t address;
    const char* name;
    PS2Runtime::RecompiledFunction original{};
    double last=-10;
    bool frequent=false;
};
LoadingStage loadingStages[]={
    {0x135970,"scene-exit"}, {0x19da80,"sound-wait-dead"},
    {0x19f4f0,"sound-suspend-cd"}, {0x120e70,"assets-unload"},
    {0x125ad0,"memory-pop-base"}, {0x1210e0,"assets-preload"},
    {0x121080,"assets-queue"}, {0x19eb80,"sound-init"},
    {0x120d70,"assets-switch"}, {0x13c740,"camera-create"},
    {0x13c730,"camera-destroy"}, {0x134800,"scene-setup"},
    {0x19ed00,"sound-load"}, {0x11ffa0,"assets-load-step",{},-10,true},
    {0x19e4f0,"sound-update",{},-10,true},
};
GL33Backend* gpu=nullptr;
double cpuSeconds(clockid_t id) {timespec t{};clock_gettime(id,&t);return t.tv_sec+t.tv_nsec*1e-9;}
double elapsed() {return std::chrono::duration<double>(Clock::now()-start).count();}
uint32_t word(const uint8_t* ram,uint32_t address) {uint32_t v;std::memcpy(&v,ram+address,4);return v;}
void event(const char* name,uint32_t value=0) {
    events<<std::fixed<<std::setprecision(6)<<elapsed()<<','<<frames<<','<<name<<','<<value<<'\n';events.flush();
    std::cout<<"[benchmark] "<<name<<" value="<<value<<" at="<<elapsed()<<" frame="<<frames<<std::endl;
}
void capture(PS2Runtime* runtime) {
    static const bool disabled=std::getenv("NIGHT100_BENCHMARK_NO_CAPTURES")!=nullptr;
    if(disabled)return;
    std::vector<uint8_t> rgba;uint32_t w=0,h=0;
    if(!runtime->gs().copyLatchedHostPresentationFrame(rgba,w,h))return;
    std::ofstream out(directory/("frame-"+std::to_string(frames)+".ppm"),std::ios::binary);
    out<<"P6\n"<<w<<' '<<h<<"\n255\n";
    std::vector<uint8_t> rgb(size_t(w)*h*3);
    for(size_t p=0,q=0;p<rgba.size();p+=4,q+=3)std::memcpy(rgb.data()+q,rgba.data()+p,3);
    out.write(reinterpret_cast<const char*>(rgb.data()),rgb.size());
}
uint32_t sceneObject(const uint8_t* ram,uint32_t scene,uint32_t id) {
    if(scene<0x1000 || scene>PS2_RAM_SIZE-0x1dc)return 0;
    const uint32_t count=word(ram,scene+0x74),array=word(ram,scene+0x78);
    if(count>100000 || array<0x1000 || array>PS2_RAM_SIZE-uint64_t(count)*4)return 0;
    for(uint32_t i=0;i<count;++i) {
        const uint32_t object=word(ram,array+i*4);
        if(object>=0x1000 && object<=PS2_RAM_SIZE-0x160 && word(ram,object)==id)return object;
    }
    return 0;
}
void warpPlayer(uint8_t* ram,const std::array<float,3>& position) {
    const uint32_t frame=word(ram,0x363238),model=word(ram,0x363224);
    const uint32_t matrix=model>=0x1000 && model<=PS2_RAM_SIZE-0x24?word(ram,model+0x20):0;
    if(frame<0x1000 || frame>PS2_RAM_SIZE-0xf0 || matrix<0x1000 || matrix>PS2_RAM_SIZE-0x40)
        throw std::runtime_error("Diagnostic warp could not locate the original player matrices");
    // xEntBeginUpdate copies model->Mat back into the frame each update.
    // The original player reset synchronizes both matrices as well.
    for(uint32_t address:{matrix+0x30,frame+0x30,frame+0x80})std::memcpy(ram+address,position.data(),12);
    const float zero[3]={};
    for(uint32_t offset:{0x70u,0x8cu,0xc8u,0xd4u,0xe0u})std::memcpy(ram+frame+offset,zero,sizeof(zero));
}
void prepareSnack(uint8_t* ram,PS2Runtime* runtime) {
    if(!std::getenv("NIGHT100_BENCHMARK_SNACK") || snackWarped || playFrames<120)return;
    const uint32_t scene=word(ram,0x363b64);
    if(scene<0x1000 || scene>PS2_RAM_SIZE-0x1dc || word(ram,scene+0x6c)!=0x48303031)return;
    const uint32_t crumb=sceneObject(ram,scene,0x432bd8f7);
    if(!crumb)throw std::runtime_error("Snack diagnostic fixture could not locate its original H001 pickup");
    warpPlayer(ram,{-6.32f,0.0f,4.20293f});
    snackWarped=true;event("debug-snack-warp",crumb);capture(runtime);
}
void prepareManor(uint8_t* ram,PS2Runtime* runtime) {
    if(!std::getenv("NIGHT100_BENCHMARK_MANOR") || manorStep || playFrames<30)return;
    const uint32_t scene=word(ram,0x363b64);
    if(scene<0x1000 || scene>PS2_RAM_SIZE-0x1dc || word(ram,scene+0x6c)!=0x48303031)return;
    const uint32_t gate=sceneObject(ram,scene,0x4191199d),trigger=sceneObject(ram,scene,0xd7b49d23),portal=sceneObject(ram,scene,0xfbf8d05f);
    if(!gate || !trigger || !portal)
        throw std::runtime_error("Manor diagnostic fixture could not locate original H001 objects");
    const uint32_t asset=word(ram,portal+0x10);
    if(asset<0x1000 || asset>PS2_RAM_SIZE-0x18 || std::memcmp(ram+asset+0x14,"I001",4))
        throw std::runtime_error("Manor diagnostic fixture found an unexpected portal destination");
    // User-authorized diagnostic warp and gate unlock. The original unlock
    // routine emits its normal links/FX; controller Circle drives the portal.
    warpPlayer(ram,{4.35f,0.5f,12.5f});
    manorStep=1;event("debug-manor-warp",gate);capture(runtime);
    GuestInvocation invocation;invocation.kind=GuestInvocationKind::HleCall;invocation.tag=0x4d414e4f52ull;
    invocation.context.pc=0x18ef30;
    SET_GPR_U32(&invocation.context,4,gate);SET_GPR_U32(&invocation.context,28,0x35f370);
    invocation.onComplete=[ram,gate,trigger](const R5900Context&,R5900Context&) {
        uint16_t flags=0;std::memcpy(&flags,ram+trigger+6,2);
        if(!(flags&1))throw std::runtime_error("Original snack-gate unlock did not enable manor entrance");
        manorStep=2;manorReadyFrame=playFrames;event("debug-manor-unlocked",word(ram,gate+0xec));
    };
    event("debug-manor-unlock-original",gate);
    runtime->eeScheduler().queueInvocation(std::move(invocation));
}
void prepareBats(uint8_t* ram,PS2Runtime* runtime) {
    if(!std::getenv("NIGHT100_BENCHMARK_BATS"))return;
    const uint32_t scene=word(ram,0x363b64);
    if(scene<0x1000 || scene>PS2_RAM_SIZE-0x1dc || word(ram,scene+0x6c)!=0x49303031)return;
    if(word(ram,0x357e20)!=11 || word(ram,0x357e24)!=2 || word(ram,0x363860) || !word(ram,0x3633b0))return;
    if(!batsStep) {
        if(interiorPlayFrames<60)return;
        batsKey=sceneObject(ram,scene,0x0013c74b);batsTrigger=sceneObject(ram,scene,0x586e19b9);
        batsWall=sceneObject(ram,scene,0xfff2b7a4);batsWallHanging=sceneObject(ram,scene,0x676e3710);
        if(!batsKey || !batsTrigger || !batsWall || !batsWallHanging)
            throw std::runtime_error("Bat diagnostic fixture could not locate original I001 key, trigger or hallway walls");
        const uint32_t model=word(ram,batsKey+0x2c);
        const uint32_t matrix=model>=0x1000 && model<=PS2_RAM_SIZE-0x24?word(ram,model+0x20):0;
        if(matrix<0x1000 || matrix>PS2_RAM_SIZE-0x40)
            throw std::runtime_error("Bat diagnostic fixture could not locate original key position");
        std::array<float,3> position;std::memcpy(position.data(),ram+matrix+0x30,12);
        position[1]-=0.65f; // Put the player's collision volume against the live key.
        warpPlayer(ram,position);batsStep=1;batsStepFrame=interiorPlayFrames;
        event("debug-bats-key-warp",batsKey);capture(runtime);
    }
    const uint32_t keyState=word(ram,batsKey+0xec);
    if(keyState!=batsLastKeyState){batsLastKeyState=keyState;event("debug-bats-key-state",keyState);}
    uint16_t triggerFlags=0;std::memcpy(&triggerFlags,ram+batsTrigger+6,2);
    if(batsStep==1) {
        // Original pickup event 0x23 enables BATSON. Do not grant inventory or
        // enable the trigger here: this is evidence of actual key collision.
        if(triggerFlags&1) {
            batsStep=2;batsStepFrame=interiorPlayFrames;
            event("debug-bats-key-collected",keyState);event("debug-bats-trigger-enabled",batsTrigger);capture(runtime);
        } else if(interiorPlayFrames>batsStepFrame+180)
            throw std::runtime_error("Bat diagnostic fixture did not collect the original key after collision warp");
    }
    if(batsStep==2 && interiorPlayFrames>=batsStepFrame+45) {
        warpPlayer(ram,{1.14f,0.0f,-3.0f});batsStep=3;batsStepFrame=interiorPlayFrames;
        event("debug-bats-hall-start");capture(runtime);
    }
    if(batsStep==3 && interiorPlayFrames>=batsStepFrame+60) {
        batsStep=4;batsStepFrame=interiorPlayFrames;event("debug-bats-walk");capture(runtime);
    }
    if(batsStep>=4 && !batsTriggeredFrame && !(triggerFlags&1)) {
        batsTriggeredFrame=frames;event("debug-bats-triggered",batsTrigger);capture(runtime);
    }
    const bool translucent=(ram[batsWall+0x17]&1)!=0;
    if(translucent && !batsWallTranslucent) {
        batsWallTranslucent=true;batsWallFrame=frames;
        event("debug-bats-wall-translucent",batsWall);event("debug-bats-wall-hanging-flags",ram[batsWallHanging+0x17]);capture(runtime);
    }
    float alpha=255;std::memcpy(&alpha,ram+batsWall+0x20,4);
    if(translucent && !batsWallFaded && alpha<=128) {
        batsWallFaded=true;event("debug-bats-wall-alpha",uint32_t(alpha));capture(runtime);
    }
    if(batsWallFrame && !batsTurnedRight && frames>=batsWallFrame+30) {
        batsTurnedRight=true;event("debug-bats-steer-right");capture(runtime);
    }
    if((batsTriggeredFrame && frames-batsTriggeredFrame<=60 && (frames-batsTriggeredFrame)%3==0) ||
       (batsWallFrame && frames-batsWallFrame<=60 && (frames-batsWallFrame)%3==0))capture(runtime);
    if(batsStep==4) {
        const uint32_t frame=word(ram,0x363238);float z=0;
        if(frame>=0x1000 && frame<=PS2_RAM_SIZE-0x40)std::memcpy(&z,ram+frame+0x38,4);
        if(z<-18.0f || interiorPlayFrames>=batsStepFrame+480) {
            batsStep=5;event("debug-bats-stop-walking");capture(runtime);
            if(!batsTriggeredFrame || !batsWallTranslucent)
                throw std::runtime_error("Bat diagnostic walk did not reach both original bat and hallway-camera triggers");
        }
        if((interiorPlayFrames-batsStepFrame)%30==0)capture(runtime);
    }
}
void completed(uint8_t* ram,PS2Runtime* runtime) {
    const auto now=Clock::now();const double tcpu=cpuSeconds(CLOCK_THREAD_CPUTIME_ID),pcpu=cpuSeconds(CLOCK_PROCESS_CPUTIME_ID);
    const uint32_t mode=word(ram,0x357e20),state=word(ram,0x3574f4),ostrich=word(ram,0x357e24);
    const bool cinematic=word(ram,0x363860)!=0;
    const uint32_t control=word(ram,0x3633b0),playerFrame=word(ram,0x363238);
    const uint32_t scenePointer=word(ram,0x363b64);
    const uint32_t sceneId=scenePointer>=0x1000 && scenePointer<=PS2_RAM_SIZE-0x70?word(ram,scenePointer+0x6c):0;
    float position[3]{};
    if(playerFrame>=0x1000 && playerFrame<PS2_RAM_SIZE-64)std::memcpy(position,ram+playerFrame+0x30,sizeof(position));
    ++frames;if(mode==2)++titleFrames;
    if(mode==11 && cinematic)++introFrames;
    if(mode==11 && ostrich==2 && !cinematic && control) {
        ++playFrames;if(sceneId==0x49303031)++interiorPlayFrames;
    }
    const auto stats=gpu?gpu->statistics():GL33Backend::Statistics{};
    if(frames>1)samples<<frames<<','<<std::fixed<<std::setprecision(6)<<elapsed()<<','
        <<std::chrono::duration<double,std::milli>(now-lastFrame).count()<<','<<(tcpu-lastCpu)*1000<<','<<(pcpu-lastProcessCpu)*1000
        <<','<<mode<<','<<state<<','<<ostrich<<','<<stats.nativeMeshes<<','<<stats.primitives<<','<<stats.surfaceReadbacks
        <<','<<stats.gpuNanoseconds<<','<<stats.gpuSamples<<','<<stats.gpuTimingSupported
        <<','<<cinematic<<','<<control<<','<<position[0]<<','<<position[1]<<','<<position[2]<<','<<sceneId
        <<','<<stats.textureUploads<<','<<word(ram,0x3635b4)
        <<','<<stats.completedGpuFrames<<','<<stats.completedCpuFallbacks<<','<<stats.completedRetainedFrames
        <<','<<stats.nativeRenderTextureCopies<<','<<stats.nativeRenderTextureFallbacks<<'\n';
    lastFrame=now;lastCpu=tcpu;lastProcessCpu=pcpu;
    static uint32_t previousMode=~0u,previousState=~0u,previousOstrich=~0u;
    if(mode!=previousMode || state!=previousState || ostrich!=previousOstrich) {
        event("mode",mode);event("state",state);event("ostrich",ostrich);
        previousMode=mode;previousState=state;previousOstrich=ostrich;capture(runtime);
    }
    if(frames%120==0){samples.flush();capture(runtime);}
    if(playFrames>=30 && playFrames<54)capture(runtime);
    static uint32_t previousSceneId=0;
    if(sceneId && sceneId!=previousSceneId){event("completed-scene",sceneId);previousSceneId=sceneId;capture(runtime);}
    // Opt-in captures bracket the scripted jump without changing its inputs.
    // Camera completion guarantees each image contains a finished game frame.
    if(std::getenv("NIGHT100_CAPTURE_SHADOW") &&
       (playFrames==390 || (playFrames>=400 && playFrames<=490 && playFrames%3==1)))capture(runtime);
    if(std::getenv("NIGHT100_BENCHMARK_SNACK")) {
        static uint32_t previousSnacks=0;
        const uint32_t snacks=word(ram,0x3635b4);
        if(snacks!=previousSnacks){event("snack-count",snacks);previousSnacks=snacks;capture(runtime);}
        if(playFrames>=180 && playFrames<=240 && playFrames%3==0)capture(runtime);
    }
    prepareSnack(ram,runtime);
    prepareManor(ram,runtime);
    prepareBats(ram,runtime);
}
void show(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    benchmarkShowOriginal(ram,cpu,runtime);
    if(cpu->pc==0x1e6180)completed(ram,runtime);
}
void pad(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    const uint32_t buffer=GPR_U32(cpu,6),port=GPR_U32(cpu,4);
    padOriginal(ram,cpu,runtime);
    if(!scripted || port!=0 || !GPR_U32(cpu,2) || buffer>PS2_RAM_SIZE-32)return;
    // Inject ordinary controller bytes after the real pad service. Never alter
    // menu/game state. Holds use camera frames so slow machines see the input.
    uint16_t buttons=0;
    if(inputStep==0 && titleFrames>=240){inputStep=1;pressFrame=frames;event("press-start");}
    if(inputStep==1) {
        if(frames-pressFrame<12)buttons=0x0008;
        else {inputStep=2;pressFrame=frames;event("release-start");}
    }
    if(inputStep==2 && frames-pressFrame>=120){inputStep=3;pressFrame=frames;event("press-cross");}
    if(inputStep==3) {
        if(frames-pressFrame<12)buttons=0x4000;
        else {inputStep=4;event("release-cross");}
    }
    if(inputStep==4 && std::getenv("NIGHT100_AUTOPLAY_SKIP_INTRO") && introFrames>=360
            && word(ram,0x363860) && word(ram,0x35841c)) {
        inputStep=5;pressFrame=frames;event("press-cross-skip-intro");
    }
    if(inputStep==5) {
        if(frames-pressFrame<12)buttons=0x4000;
        else {inputStep=6;event("release-cross-skip-intro");}
    }
    if(manorStep==2 && playFrames>=manorReadyFrame+120) {
        manorStep=3;pressFrame=frames;event("debug-manor-press-circle");
    }
    if(manorStep==3) {
        if(frames-pressFrame<12)buttons|=0x2000;
        else {manorStep=4;event("debug-manor-release-circle");}
    }
    if(std::getenv("NIGHT100_BENCHMARK_BATS") && batsTriggeredFrame && batsTurnedRight) {
        const uint32_t scene=word(ram,0x363b64);
        const bool dialogue=scene>=0x1000 && scene<=PS2_RAM_SIZE-0x70 && word(ram,scene+0x6c)==0x49303031
            && word(ram,0x357e20)==11 && word(ram,0x357e24)==2 && !word(ram,0x363860) && !word(ram,0x3633b0);
        if(dialogue) {
            if(!batsDialogueActive) {
                batsDialogueActive=true;batsDialogueFrame=frames;batsDialogueStart=Clock::now();batsDialogueCycle=~0ull;
                event("debug-bats-dialogue-start");capture(runtime);
            }
            if(frames-batsDialogueFrame>=900 || Clock::now()-batsDialogueStart>std::chrono::seconds(20))
                throw std::runtime_error("Bat diagnostic could not dismiss the original Holly dialogue within 900 game frames or 20 seconds");
            const uint64_t cycle=(frames-batsDialogueFrame)/30;
            if((frames-batsDialogueFrame)%30<6) {
                buttons|=0x4000;
                if(cycle!=batsDialogueCycle){batsDialogueCycle=cycle;event("debug-bats-dialogue-cross",uint32_t(cycle+1));}
            }
        } else if(batsDialogueActive) {
            batsDialogueActive=false;event("debug-bats-dialogue-finished");capture(runtime);
        }
    }
    const uint16_t activeLow=~buttons;
    ram[buffer+2]=activeLow&255;ram[buffer+3]=activeLow>>8;
    ram[buffer+4]=ram[buffer+5]=ram[buffer+6]=ram[buffer+7]=128;
    if(!std::getenv("NIGHT100_BENCHMARK_MANOR")) {
        if(inputStep>=4 && playFrames>=120 && playFrames<240)ram[buffer+7]=0;
        if(inputStep>=4 && playFrames>=300 && playFrames<360)ram[buffer+4]=255;
        if(inputStep>=4 && playFrames>=420 && playFrames<432)ram[buffer+3]&=~0x40;
    } else if(std::getenv("NIGHT100_BENCHMARK_BATS")) {
        if(batsStep==4) {
            if(!batsWallFrame)ram[buffer+7]=0;
            else if(batsTurnedRight)ram[buffer+6]=255;
        }
    } else {
        if(interiorPlayFrames>=120 && interiorPlayFrames<240)ram[buffer+7]=0;
        if(interiorPlayFrames>=300 && interiorPlayFrames<360)ram[buffer+4]=255;
        if(interiorPlayFrames>=420 && interiorPlayFrames<432)ram[buffer+3]&=~0x40;
    }
    static uint64_t previousPlay=0;
    if(playFrames!=previousPlay) {
        if(playFrames==1)event("playable-frame");
        if(!std::getenv("NIGHT100_BENCHMARK_MANOR")) {
            if(playFrames==120)event("walk-forward");
            if(playFrames==240)event("stop-walking");
            if(playFrames==300)event("turn-camera");
            if(playFrames==360)event("stop-camera");
            if(playFrames==420)event("jump");
            if(playFrames==432)event("release-jump");
        }
        previousPlay=playFrames;
    }
}
void scene(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    event("scene-init",GPR_U32(cpu,4));sceneOriginal(ram,cpu,runtime);
}
void portal(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(cpu->pc==0x135850)event("debug-manor-original-portal-request",GPR_U32(cpu,4));
    portalOriginal(ram,cpu,runtime);
}
void loadingStage(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    for(auto& stage:loadingStages)if(cpu->pc==stage.address) {
        const uint32_t caller=GPR_U32(cpu,31);
        const bool logged=!stage.frequent || elapsed()-stage.last>=2;
        if(logged) {
            stage.last=elapsed();event(stage.name,GPR_U32(cpu,4));
            std::cout<<"[benchmark:loading] stage="<<stage.name<<" ra=0x"<<std::hex<<caller
                <<" sound-pending=0x"<<word(ram,0x3575b0)
                <<" sound-suspended=0x"<<word(ram,0x3748ac)
                <<" scene=0x"<<word(ram,0x363b64)<<std::dec<<std::endl;
        }
        stage.original(ram,cpu,runtime);
        if(logged && cpu->pc==caller)event("loading-stage-return",stage.address);
        return;
    }
    throw std::runtime_error("Unexpected loading diagnostic entry");
}
void loop(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    event("game-loop-entry");loopOriginal(ram,cpu,runtime);
}
void cutscene(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    static double previous=-10;
    if(elapsed()-previous>=2) {
        previous=elapsed();const uint32_t p=GPR_U32(cpu,4);
        if(p<PS2_RAM_SIZE-0x138) {
            std::cout<<"[benchmark:cutscene] ptr="<<std::hex<<p<<" waiting="<<word(ram,p+0x1c)
                <<" ready="<<word(ram,p+0x34)<<" dataLoading="<<word(ram,p+0x38)
                <<" gotData="<<word(ram,p+0x3c)<<" sector="<<word(ram,p+0x10c)
                <<" file="<<word(ram,p+0xfc)<<std::dec<<std::endl;
            if(!std::getenv("NIGHT100_BENCHMARK_STATE")){cutsceneOriginal(ram,cpu,runtime);return;}
            std::ofstream dump(directory/"cutscene.bin",std::ios::binary);dump.write(reinterpret_cast<char*>(ram+p),0x138);
            std::vector<uint8_t> iop(2*1024*1024);
            if(runtime->readIopMemory(0,iop.data(),iop.size())) {
                std::ofstream memory(directory/"iop.bin",std::ios::binary);memory.write(reinterpret_cast<char*>(iop.data()),iop.size());
            }
            const auto snapshot=runtime->iopDebugSnapshot();
            std::ofstream debug(directory/"iop.txt");
            for(const auto& service:snapshot.services) {
                debug<<service.name<<'\n';for(const auto& m:service.metrics)debug<<m.name<<'='<<m.value<<'\n';
            }
            for(const auto& d:snapshot.diagnostics)debug<<d<<'\n';
        }
    }
    cutsceneOriginal(ram,cpu,runtime);
}
}
void configureGameBenchmark(GL33Backend* value,bool nativeCompletions){GameBenchmark::gpu=value;GameBenchmark::nativeCompletions=nativeCompletions;}
void benchmarkFrameCompleted(uint8_t* ram,PS2Runtime* runtime){
    inputReplayFrameCompleted(ram,runtime);
    if(GameBenchmark::samples.is_open())GameBenchmark::completed(ram,runtime);
}
void finishGameBenchmark(){if(GameBenchmark::events.is_open()){GameBenchmark::event("stop");GameBenchmark::samples.flush();}}
void installGameBenchmark(PS2Runtime& runtime) {
    using namespace GameBenchmark;
    const char* path=std::getenv("NIGHT100_BENCHMARK");if(!path)return;
    directory=path;std::filesystem::create_directories(directory);
    samples.open(directory/"frames.csv");events.open(directory/"events.csv");
    if(!samples || !events)throw std::runtime_error("Cannot create benchmark files");
    samples<<"frame,seconds,wall_ms,game_thread_cpu_ms,process_cpu_ms,mode,state,ostrich,native_meshes,gs_primitives,readbacks,gpu_ns,gpu_samples,gpu_timing_supported,cutscene,player_control,player_x,player_y,player_z,scene_id,texture_uploads,snacks,gpu_completed_frames,cpu_completed_fallbacks,retained_completed_frames,native_render_texture_copies,native_render_texture_fallbacks\n";
    events<<"seconds,frame,event,value\n";
    start=Clock::now();scripted=std::getenv("NIGHT100_AUTOPLAY")!=nullptr;
    if(!nativeCompletions) {
        benchmarkShowOriginal=runtime.lookupFunction(0x1eb4a8);
        for(uint32_t pc=0x1eb4a8;pc<0x1eb510;pc+=4)
            if(runtime.hasFunction(pc) && runtime.lookupFunction(pc)==benchmarkShowOriginal)runtime.replaceFunction(pc,show);
    }
    padOriginal=runtime.lookupFunction(0x10e218);runtime.replaceFunction(0x10e218,pad);
    sceneOriginal=runtime.lookupFunction(0x135b90);runtime.replaceFunction(0x135b90,scene);
    loopOriginal=runtime.lookupFunction(0x131920);runtime.replaceFunction(0x131920,loop);
    cutsceneOriginal=runtime.lookupFunction(0x1d15b0);runtime.replaceFunction(0x1d15b0,cutscene);
    if(std::getenv("NIGHT100_BENCHMARK_MANOR")) {
        portalOriginal=runtime.lookupFunction(0x135850);
        for(uint32_t pc=0x135850;pc<0x13596c;pc+=4)
            if(runtime.hasFunction(pc) && runtime.lookupFunction(pc)==portalOriginal)runtime.replaceFunction(pc,portal);
        for(auto& stage:loadingStages) {
            stage.original=runtime.lookupFunction(stage.address);
            if(!stage.original)throw std::runtime_error("Missing original loading diagnostic function");
            runtime.replaceFunction(stage.address,loadingStage);
        }
    }
    event("boot");
}
