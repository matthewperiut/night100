#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_stubs.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "graphics/gl33_backend.h"
#include "graphics/gs_trace.h"
#include "frame_metrics.h"
#include "game_benchmark.h"
#include "input_replay.h"
#include "replay_harness.h"
#include "replay_catalog.h"
#include "machine_snapshot.h"
#include "session_ui.h"
#include "native_scene.h"
#include "morph_checks.h"
#include <raylib.h>
#include <rlgl.h>
#include <chrono>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <exception>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
#include "native_video.h"
#include "controller_prompts.h"
#include "frame_events.h"
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static std::filesystem::path defaultElf(const char* executable) {
    std::filesystem::path executablePath;
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) executablePath = buffer.data();
#endif
    if (executablePath.empty()) executablePath = executable;
    auto directory = std::filesystem::weakly_canonical(executablePath).parent_path();
    while (!directory.empty()) {
        const auto candidate = directory / "game/SLUS_203.49";
        if (std::filesystem::is_regular_file(candidate)) return candidate;
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    return std::filesystem::absolute("game/SLUS_203.49");
}

static void dumpState(PS2Runtime& runtime, const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    const auto binary = [&](const char* name, const void* data, size_t size) {
        std::ofstream file(directory / name, std::ios::binary);
        file.write(static_cast<const char*>(data), size);
        if (!file) throw std::runtime_error("Failed to write diagnostic state");
    };
    binary("ee-ram.bin", runtime.memory().getRDRAM(), PS2_RAM_SIZE);
    binary("vu1-code.bin", runtime.memory().getVU1Code(), PS2_VU1_CODE_SIZE);
    binary("vu1-data.bin", runtime.memory().getVU1Data(), PS2_VU1_DATA_SIZE);
    binary("gs-vram.bin", runtime.memory().getGSVRAM(), PS2_GS_VRAM_SIZE);
    std::vector<uint8_t> iop(2*1024*1024);
    if(runtime.readIopMemory(0,iop.data(),iop.size()))binary("iop-ram.bin",iop.data(),iop.size());
    const auto iopSnapshot=runtime.iopDebugSnapshot();
    std::ofstream iopDebug(directory/"iop.txt");
    for(const auto& service:iopSnapshot.services) {
        iopDebug<<service.name<<'\n';
        for(const auto& metric:service.metrics)iopDebug<<metric.name<<'='<<metric.value<<'\n';
    }
    for(const auto& diagnostic:iopSnapshot.diagnostics)iopDebug<<diagnostic<<'\n';
    const auto eeSnapshot=runtime.eeScheduler().snapshot();
    std::ofstream timing(directory/"clocks.json");
    timing<<"{\"ee_cycles\":"<<eeSnapshot.eeCycle
          <<",\"vsync_tick\":"<<runtime.eeScheduler().currentVSyncTick()<<"}\n";
    const auto& ee=runtime.cpu();
    std::ofstream eeRegisters(directory/"ee-registers.json");
    eeRegisters<<"{\"pc\":"<<ee.pc<<",\"gpr_words\":[";
    for(unsigned i=0;i<32;++i) {
        uint32_t words[4];std::memcpy(words,&ee.r[i],sizeof(words));
        eeRegisters<<(i?",[":"[");
        for(unsigned lane=0;lane<4;++lane)eeRegisters<<(lane?",":"")<<words[lane];
        eeRegisters<<']';
    }
    eeRegisters<<"]}\n";
    std::vector<uint8_t> frame;
    uint32_t width = 0, height = 0;
    if (runtime.gs().copyLatchedHostPresentationFrame(frame, width, height) && !frame.empty()) {
        const Image image{frame.data(), static_cast<int>(width), static_cast<int>(height),
                          1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        ExportImage(image, (directory / "presentation.png").c_str());
    }
    const auto& vu = runtime.vu1().state();
    std::ofstream registers(directory / "vu1-registers.json");
    registers << "{\"pc\":" << vu.pc << ",\"top\":" << vu.top << ",\"itop\":" << vu.itop << ",\"vi\":[";
    for (int i = 0; i < 16; ++i) registers << (i ? "," : "") << vu.vi[i];
    registers << "],\"vf_bits\":[";
    for (int i = 0; i < 32; ++i) {
        registers << (i ? ",[" : "[");
        for (int lane = 0; lane < 4; ++lane) {
            uint32_t bits;
            std::memcpy(&bits, &vu.vf[i][lane], sizeof(bits));
            registers << (lane ? "," : "") << bits;
        }
        registers << ']';
    }
    registers << "]}\n";
    std::ofstream events(directory / "gs-events.csv");
    events << "sequence,frame,kind,nloop,format,vertices,fbp,fbw,psm,x_min,x_max,y_min,y_max,alpha_min,alpha_max\n";
    for (const auto& event : runtime.gs().getDebugHistory()) {
        events << event.seq << ',' << event.frameIndex << ',' << int(event.kind) << ',' << event.gifNloop
               << ',' << int(event.gifFlg) << ',' << event.vertexCount << ',' << event.frame.fbp
               << ',' << event.frame.fbw << ',' << int(event.frame.psm) << ',' << event.xMin
               << ',' << event.xMax << ',' << event.yMin << ',' << event.yMax
               << ',' << int(event.aMin) << ',' << int(event.aMax) << '\n';
    }
    std::cout << "[night100] saved diagnostic state to " << directory << '\n';
}

static int selfTest(PS2Runtime& runtime, const std::filesystem::path& elf) {
    // RenderWare's actual MFC1 / PEXEW / MFC1 / PPACW material packing
    // sequence must retain alpha and green above the overwritten low 64 bits.
    R5900Context packing{};
    auto* packingContext = &packing;
    SET_GPR_VEC(packingContext, 2, _mm_set_epi32(0, 0x6c0103fb, 0, 0));
    SET_GPR_VEC(packingContext, 3, _mm_set_epi32(0, 0x6c0103fc, 0, 0));
    SET_GPR_U32(packingContext, 2, 0x3f800000); // alpha = 1
    SET_GPR_U32(packingContext, 3, 0x3f000000); // green = .5
    SET_GPR_VEC(packingContext, 2, PS2_PEXEW(GPR_VEC(packingContext, 2)));
    SET_GPR_VEC(packingContext, 3, PS2_PEXEW(GPR_VEC(packingContext, 3)));
    SET_GPR_U32(packingContext, 2, 0x3e800000); // blue = .25
    SET_GPR_U32(packingContext, 3, 0x3e000000); // red = .125
    const auto material = PS2_PPACW(GPR_VEC(packingContext, 2), GPR_VEC(packingContext, 3));
    uint32_t materialLanes[4];
    _mm_storeu_si128(reinterpret_cast<__m128i*>(materialLanes), material);
    const uint32_t expectedMaterial[] = {0x3e000000, 0x3f000000, 0x3e800000, 0x3f800000};
    if (std::memcmp(materialLanes, expectedMaterial, sizeof(materialLanes)) != 0) {
        std::cerr << "FAIL: EE material packing corrupts RGBA lanes\n";
        return 1;
    }
    std::cout << "PASS: RenderWare EE material packing preserves all RGBA lanes\n";
    if (!runtime.memory().initialize() || !runtime.loadELF(elf.string())) return 1;
    const auto callbackTop = runtime.reserveAsyncCallbackStack(0x4000, 16);
    if (callbackTop == 0 || callbackTop >= 0x01ff0000) {
        std::cerr << "FAIL: callback stack overlaps the main EE stack\n";
        return 1;
    }
    if (!runtime.syncCoreSubsystems()) return 1;
    if (!checkOriginalMorphRoutines(runtime)) return 1;
    {
        struct Conversion {uint32_t bits;int32_t expected;};
        const Conversion cases[]={
            {0x00000000,0},{0x80000000,0},{0x00000001,0},{0x80000001,0},
            {0x3f666666,0},{0xbf666666,0},{0x3ff33333,1},{0xbff33333,-1},
            {0x3fc00000,1},{0xbfc00000,-1},{0x40000000,2},{0xc0000000,-2},
            {0x41efd70a,29},{0xc1efd70a,-29},
            {0x4effffff,2147483520},{0xceffffff,-2147483520},
            {0x4f000000,INT32_MAX},{0xcf000000,INT32_MIN},
            {0x4f800000,INT32_MAX},{0xcf800000,INT32_MIN},
            {0x7f7fffff,INT32_MAX},{0xff7fffff,INT32_MIN},
            {0x7f800000,INT32_MAX},{0xff800000,INT32_MIN},
            {0x7fc00001,INT32_MAX},{0xffc00001,INT32_MIN},
            {0x7f800001,INT32_MAX},{0xff800001,INT32_MIN},
        };
        const int previous=std::fegetround();
        for(int mode:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
            if(std::fesetround(mode))throw std::runtime_error("Cannot set host rounding mode for CVT.W.S test");
            for(const auto& test:cases) {
                float input;std::memcpy(&input,&test.bits,4);
                if(FPU_CVT_W_S(input)!=test.expected) {
                    std::fesetround(previous);std::cerr<<"FAIL: EE CVT.W.S input=0x"<<std::hex<<test.bits<<std::dec<<'\n';return 1;
                }
            }
        }
        std::fesetround(previous);
        std::cout<<"PASS: 112 EE CVT.W.S truncation/saturation cases across all host rounding modes\n";
    }
    {
        // This original HUD callback owns the portal's 30-frame countdown.
        // Stop at its ordinary HUD animation call or requested state switch;
        // no scene resources are needed to execute the real countdown code.
        static unsigned switches=0;
        static uint32_t requestedState=0;
        constexpr uint32_t stop=0x00fffffc,gp=0x35f370;
        const auto animate=runtime.lookupFunction(0x1c55c0);
        const auto switchState=runtime.lookupFunction(0x136d70);
        runtime.replaceFunction(0x1c55c0,[](uint8_t*,R5900Context* cpu,PS2Runtime*){cpu->pc=0x00fffffc;});
        runtime.replaceFunction(0x136d70,[](uint8_t*,R5900Context* cpu,PS2Runtime*) {
            ++switches;requestedState=GPR_U32(cpu,4);cpu->pc=0x00fffffc;
        });
        auto* bytes=runtime.memory().getRDRAM();
        const auto store=[&](uint32_t address,uint32_t value){std::memcpy(bytes+address,&value,4);};
        const auto load=[&](uint32_t address){uint32_t value;std::memcpy(&value,bytes+address,4);return value;};
        for(float dt:{1.0f/60.0f,1.0f/30.0f,0.02207357f}) {
            switches=0;requestedState=0;
            store(0x3574f4,4);store(gp-0x7020,1);store(gp-0x705c,30);
            store(0x3aa710+0x48,0); // first ordinary HUD animation case
            for(unsigned frame=0;frame<30;++frame) {
                auto& cpu=runtime.cpu();cpu=R5900Context{};cpu.pc=0x1c63c0;cpu.f[12]=dt;
                SET_GPR_U32(&cpu,28,gp);SET_GPR_U32(&cpu,29,0x01fff000);SET_GPR_U32(&cpu,31,stop);
                for(unsigned dispatch=0;cpu.pc!=stop && dispatch<64;++dispatch) {
                    const auto fn=runtime.lookupFunction(cpu.pc);if(!fn)break;
                    fn(bytes,&cpu,&runtime);
                }
                if(cpu.pc!=stop || load(gp-0x705c)!=29-frame || switches!=(frame==29?1u:0u)) {
                    std::cerr<<"FAIL: original manor HUD transition countdown dt="<<dt<<" frame="<<frame
                        <<" count="<<load(gp-0x705c)<<" switches="<<switches<<'\n';return 1;
                }
            }
            if(requestedState!=5){std::cerr<<"FAIL: original HUD did not request scene-load state\n";return 1;}
        }
        runtime.replaceFunction(0x1c55c0,animate);runtime.replaceFunction(0x136d70,switchState);
        store(0x3574f4,0);store(gp-0x7020,0);store(gp-0x705c,0);
        std::cout<<"PASS: original HUD portal countdown reaches scene-load state after 30 frames at three frame intervals\n";
    }
    {
        // Enter both the real first JAL into EndRenderFX and the wait hook's
        // resumed PC. The first call must unwind before the inline busy loop.
        constexpr uint32_t gp=0x35f370,stack=0x01fff000,lastAddress=0x14800,environment=0x14820;
        auto* bytes=runtime.memory().getRDRAM();
        uint32_t savedEnvironment=0;std::memcpy(&savedEnvironment,bytes+gp-0x7504,4);
        std::memcpy(bytes+gp-0x7504,&environment,4);std::memset(bytes+environment,0,16);
        const auto render=runtime.lookupFunction(0x13a130);
        runtime.replaceFunction(0x13a130,[](uint8_t*,R5900Context* cpu,PS2Runtime*){cpu->pc=0x00fffffc;});
        for(bool firstCall:{false,true})
        for(const auto& counters:std::array<std::array<uint32_t,2>,4>{{{0,1},{0xffffffff,0},{10,13},{0x7fffffff,0x80000000}}}) {
            std::memcpy(bytes+gp-0x76c8,&counters[1],4);
            std::memcpy(bytes+lastAddress,&counters[0],4);
            std::memcpy(bytes+stack+0x100,&lastAddress,4);
            auto& cpu=runtime.cpu();cpu=R5900Context{};cpu.pc=firstCall?0x131d90:0x131da8;
            SET_GPR_U32(&cpu,28,gp);SET_GPR_U32(&cpu,29,stack);SET_GPR_S32(&cpu,4,int32_t(counters[0]));
            if(firstCall) {
                runtime.lookupFunction(cpu.pc)(bytes,&cpu,&runtime);
                uint32_t stored=0;std::memcpy(&stored,bytes+lastAddress,4);
                if(cpu.pc!=0x131d98 || GPR_U32((&cpu),29)!=stack || GPR_U32((&cpu),31)!=0x131d98
                    || stored!=counters[0]) {
                    std::cerr<<"FAIL: first frame wait did not unwind at the original call continuation\n";return 1;
                }
            }
            for(unsigned n=0;cpu.pc!=0x00fffffc && n<8;++n) {
                const auto fn=runtime.lookupFunction(cpu.pc);if(!fn)break;
                fn(bytes,&cpu,&runtime);
            }
            uint32_t stored=0;std::memcpy(&stored,bytes+lastAddress,4);
            if(cpu.pc!=0x00fffffc || stored!=counters[1] || GPR_U32((&cpu),3)!=counters[1]) {
                std::cerr<<"FAIL: 60 Hz frame continuation or vblank counter wrap\n";return 1;
            }
        }
        // The same EndRenderFX wrapper serves other engine callers unchanged.
        auto& cpu=runtime.cpu();cpu=R5900Context{};cpu.pc=0x13c7c0;
        SET_GPR_U32(&cpu,28,gp);SET_GPR_U32(&cpu,29,stack);SET_GPR_U32(&cpu,31,0x00fffffc);
        runtime.lookupFunction(cpu.pc)(bytes,&cpu,&runtime);
        if(cpu.pc!=0x00fffffc || GPR_U32((&cpu),29)!=stack || GPR_U32((&cpu),31)!=0x00fffffc) {
            std::cerr<<"FAIL: frame wait handoff changed an unrelated EndRenderFX caller\n";return 1;
        }
        std::memcpy(bytes+gp-0x7504,&savedEnvironment,4);
        runtime.replaceFunction(0x13a130,render);
        std::cout<<"PASS: first-call and resumed one-vblank pacing preserve render continuation, stack, other callers and counter wrap\n";
    }
    const uint32_t vectorPacket[] = {0x01000101, 0x64010000, 0x11223344, 0x55667788};
    runtime.memory().processVIF1Data(reinterpret_cast<const uint8_t*>(vectorPacket), sizeof(vectorPacket));
    uint32_t vectorLanes[4];
    std::memcpy(vectorLanes, runtime.memory().getVU1Data(), sizeof(vectorLanes));
    if (vectorLanes[0] != 0x11223344 || vectorLanes[1] != 0x55667788 ||
        vectorLanes[2] != vectorLanes[0] || vectorLanes[3] != vectorLanes[1]) {
        std::cerr << "FAIL VIF V2 hardware broadcast lanes\n";
        return 1;
    }
    for (uint64_t format : {2u, 3u}) {
        auto& gs = runtime.gs();
        gs.writeRegister(0x50, uint64_t{1} << 48); // DBW=1, PSMCT32
        gs.writeRegister(0x51, 0);
        gs.writeRegister(0x52, (uint64_t{1} << 32) | 1); // 1x1
        gs.writeRegister(0x53, 0); // host to local
        const uint32_t pixel = format == 2 ? 0x11223344u : 0x55667788u;
        const uint64_t packet[] = {1 | 0x8000 | (format << 58), 0, pixel, 0};
        runtime.gifArbiter().submit(GifPathId::Path3, reinterpret_cast<const uint8_t*>(packet), sizeof(packet));
        runtime.gifArbiter().drain();
        if (gs.ReadVram(0, 0, 1, 0, 0) != pixel) {
            std::cerr << "FAIL: GIF image transfer format " << format << '\n';
            return 1;
        }
    }
    auto* ram = runtime.memory().getRDRAM();
    constexpr uint32_t text = 0x10000, sentinel = 0x00fffffc;
    const auto call = [&](uint32_t address, uint32_t a0, uint32_t a1, uint32_t a2) {
        auto& cpu = runtime.cpu();
        cpu = R5900Context{};
        cpu.pc = address;
        SET_GPR_U32(&cpu, 4, a0);
        SET_GPR_U32(&cpu, 5, a1);
        SET_GPR_U32(&cpu, 6, a2);
        SET_GPR_U32(&cpu, 28, 0x35f370);
        SET_GPR_U32(&cpu, 29, 0x01fff000);
        SET_GPR_U32(&cpu, 31, sentinel);
        for (unsigned i = 0; i < 10000 && cpu.pc != sentinel; ++i) {
            const auto fn = runtime.lookupFunction(cpu.pc);
            if (!fn) throw std::runtime_error("Self-test reached missing function");
            fn(ram, &cpu, &runtime);
        }
        if (cpu.pc != sentinel) throw std::runtime_error("Self-test did not return");
        return GPR_U32((&cpu), 2);
    };
    {
        // The original cutscene destructor requires an outstanding Stop
        // command to report INPROG. Immediate completion caused a second
        // destruction of already released models on the following frame.
        const uint32_t reading=1;
        std::memcpy(ram+0x35f370-0x7764,&reading,4);
        ps2_stubs::sceCdInit(ram,&runtime.cpu(),&runtime);
        ps2_stubs::sceCdStop(ram,&runtime.cpu(),&runtime);
        if(call(0x110c10,0,0,0)!=1) {
            std::cerr<<"FAIL: original file status misses pending asynchronous CD Stop\n";return 1;
        }
        ps2_stubs::sceCdInit(ram,&runtime.cpu(),&runtime);
        if(call(0x110c10,0,0,0)!=2) {
            std::cerr<<"FAIL: original file status does not return completed after CD reset\n";return 1;
        }
        std::cout<<"PASS: original file-status routine observes asynchronous CD Stop and reset\n";
    }
    // Golden values for Heavy Iron's asset hash, including case folding and wraparound.
    struct Case { const char* input; uint32_t expected; };
    const Case cases[] = {{"", 0}, {"A", 65}, {"a", 65}, {"BOOT", 149740158},
                          {"Scooby", 782627383}, {"sCoObY", 782627383}};
    for (const auto& test : cases) {
        std::strcpy(reinterpret_cast<char*>(ram + text), test.input);
        const uint32_t actual = call(0x13d330, text, 0, 0);
        if (actual != test.expected) {
            std::cerr << "FAIL xStrHash(" << test.input << "): " << actual
                      << " expected " << test.expected << '\n';
            return 1;
        }
    }
    std::cout << "PASS: 6 original recompiled xStrHash cases executed natively\n";
    std::cout << "PASS: callback stack isolation and GIF IMAGE/IMAGE2 pixel transfers\n";
    const auto clockStart=std::chrono::steady_clock::now();
    call(0x1264d0,0,0,0);const uint64_t firstTicks=GPR_U64((&runtime.cpu()),2);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    call(0x1264d0,0,0,0);const uint64_t secondTicks=GPR_U64((&runtime.cpu()),2);
    const double hostElapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-clockStart).count();
    const double gameElapsed=double(secondTicks-firstTicks)/9216000.0;
    if(secondTicks<=firstTicks || std::abs(gameElapsed-hostElapsed)>.005 || gameElapsed<.015) {
        std::cerr<<"FAIL: native game platform clock loses idle time or changes tick units\n";return 1;
    }
    std::cout<<"PASS: native 9.216 MHz platform clock advances through host idle\n";
    const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,0};
    const float matrix[16] = {2,0,0,0, 0,3,0,0, 0,0,4,0, 7,11,13,0};
    std::memcpy(ram + 0x12000, identity, sizeof(identity));
    std::memcpy(ram + 0x12100, matrix, sizeof(matrix));
    call(0x113950, 0x12200, 0x12000, 0x12100); // xMat4x3Mul(identity, matrix)
    for (unsigned row = 0; row < 4; ++row) for (unsigned lane = 0; lane < 3; ++lane) {
        float actual;
        std::memcpy(&actual, ram + 0x12200 + (row * 4 + lane) * 4, 4);
        if (actual != matrix[row * 4 + lane]) {
            std::cerr << "FAIL original xMat4x3Mul row=" << row << " lane=" << lane
                      << " actual=" << actual << " expected=" << matrix[row * 4 + lane] << '\n';
            return 1;
        }
    }
    const float vector[3] = {5,7,11};
    std::memcpy(ram + 0x12300, vector, sizeof(vector));
    call(0x113f00, 0x12400, 0x12100, 0x12300); // xMat3x3RMulVec
    const float expectedVector[3] = {10,21,44};
    for (unsigned lane = 0; lane < 3; ++lane) {
        float actual;
        std::memcpy(&actual, ram + 0x12400 + lane * 4, 4);
        if (actual != expectedVector[lane]) {
            std::cerr << "FAIL original matrix/vector transform lane=" << lane << " value=" << actual << '\n';
            return 1;
        }
    }
    std::cout << "PASS: original recompiled matrix multiply and vertex transform\n";
    const float zeroAngles[3] = {0,0,0};
    std::memcpy(ram + 0x12500, zeroAngles, sizeof(zeroAngles));
    call(0x1147a0, 0x12600, 0x12500, 0);
    for (unsigned row = 0; row < 3; ++row) for (unsigned lane = 0; lane < 3; ++lane) {
        float value;
        std::memcpy(&value, ram + 0x12600 + (row*4+lane)*4, 4);
        if (value != (row == lane ? 1.0f : 0.0f)) {
            std::cerr << "FAIL original Euler rotation at zero: row=" << row << " lane=" << lane
                      << " value=" << value << '\n';
            return 1;
        }
    }
    std::cout << "PASS: original Euler rotation and trigonometric helpers\n";
    std::memcpy(ram + 0x12700, matrix, sizeof(matrix));
    call(0x1ddfa8, 0x12800, 0x12700, 0); // RenderWare matrix orthonormalization
    for (unsigned row = 0; row < 3; ++row) for (unsigned lane = 0; lane < 3; ++lane) {
        float value;
        std::memcpy(&value, ram + 0x12800 + (row*4+lane)*4, 4);
        if (value != (row == lane ? 1.0f : 0.0f)) {
            std::cerr << "FAIL original RenderWare matrix normalization: row=" << row
                      << " lane=" << lane << " value=" << value << '\n';
            return 1;
        }
    }
    std::cout << "PASS: original RenderWare normalization using EE SQRT.S\n";
    uint16_t savedGsParameters[4];
    std::memcpy(savedGsParameters, ram + 0x288a40, sizeof(savedGsParameters));
    for (uint16_t frameMode : {0, 1}) {
        const uint16_t parameters[] = {1, 2, frameMode, 3};
        std::memcpy(ram + 0x288a40, parameters, sizeof(parameters));
        for (uint32_t psm : {0u, 2u}) {
            const uint32_t expected = (psm == 0 ? 140 : 70) * (frameMode ? 2 : 1);
            if (call(0x102e80, psm, 640, 448) != expected) {
                std::cerr << "FAIL original GS buffer layout: mode=" << frameMode << " psm=" << psm << '\n';
                return 1;
            }
        }
    }
    std::memcpy(ram + 0x288a40, savedGsParameters, sizeof(savedGsParameters));
    std::cout << "PASS: original GS buffer layout for 16/32-bit field/frame modes\n";
    return 0;
}

int main(int argc, char** argv) {
    try {
        bool test = false;
        double seconds = 0;
        std::string renderer = "gl33";
        std::string graphics = "native";
        std::string controllerProfile=ControllerPrompts::defaultProfile();
        std::string tracePath;
        std::string capturePath;
        double captureAfter = 5;
        std::filesystem::path iso;
        std::filesystem::path stateDirectory;
        std::filesystem::path inputRecord,inputPlayback,sessionPlayback;
        bool replayContinue=false,metricsVisible=false;
        std::filesystem::path elf = defaultElf(argv[0]);
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                std::cout << "night100 [ELF] [--iso PATH] [--self-test] [--seconds N] [--skip-videos]\n"
                             "         [--capture PNG] [--capture-after N] [--dump-state DIRECTORY]\n"
                             "         [--renderer cpu|gl33] [--graphics native|gs] [--record-gs FILE]\n"
                             "         [--controller-prompts ps|sw] (button artwork; controls stay the same)\n"
                             "         [--record-inputs FILE | --replay-inputs FILE] [--replay-continue]\n"
                             "         [--replay-session DIRECTORY] (full-state numbered capture)\n"
                             "         [--cpu-scale 1..100] (development executor slowdown proxy)\n";
                return 0;
            }
            else if (arg == "--self-test") test = true;
            else if (arg == "--renderer" && i + 1 < argc) renderer = argv[++i];
            else if (arg == "--graphics" && i + 1 < argc) graphics = argv[++i];
            else if (arg == "--controller-prompts" && i + 1 < argc) controllerProfile = argv[++i];
            else if (arg == "--record-gs" && i + 1 < argc) tracePath = argv[++i];
            else if (arg == "--record-inputs" && i + 1 < argc) inputRecord = argv[++i];
            else if (arg == "--replay-inputs" && i + 1 < argc) inputPlayback = argv[++i];
            else if (arg == "--replay-session" && i + 1 < argc) sessionPlayback = std::filesystem::absolute(argv[++i]);
            else if (arg == "--replay-continue") replayContinue = true;
            else if (arg == "--metrics") metricsVisible = true;
            else if (arg == "--skip-videos") setenv("NIGHT100_SKIP_VIDEOS", "1", 1);
            else if (arg == "--cpu-scale" && i + 1 < argc) {
                const std::string value=argv[++i];size_t end=0;const double scale=std::stod(value,&end);
                if(end!=value.size() || !std::isfinite(scale) || scale<1 || scale>100)
                    throw std::runtime_error("CPU scale must be between 1 and 100");
                setenv("NIGHT100_CPU_SCALE",value.c_str(),1);
            }
            else if (arg == "--seconds" && i + 1 < argc) seconds = std::stod(argv[++i]);
            else if (arg == "--capture" && i + 1 < argc) capturePath = argv[++i];
            else if (arg == "--capture-after" && i + 1 < argc) captureAfter = std::stod(argv[++i]);
            else if (arg == "--iso" && i + 1 < argc) iso = argv[++i];
            else if (arg == "--dump-state" && i + 1 < argc) stateDirectory = argv[++i];
            else if (arg.starts_with("--")) throw std::runtime_error("Unknown or incomplete option: " + arg);
            else elf = arg;
        }
        elf = std::filesystem::absolute(elf);
        if(!sessionPlayback.empty()) {
            if(!inputRecord.empty() || !inputPlayback.empty())throw std::runtime_error("Choose one replay or recording mode");
            const auto name=sessionPlayback.filename().string();
            if(!name.starts_with("session-"))throw std::runtime_error("Invalid numbered replay directory");
            const auto metadata=ReplayCatalog::load({std::stoull(name.substr(8)),sessionPlayback});
            auto option=[&](const std::string& key) {
                const auto begin=metadata.options.find(key+"=");
                if(begin==std::string::npos)throw std::runtime_error("Missing replay launch option: "+key);
                const auto at=begin+key.size()+1;return metadata.options.substr(at,metadata.options.find(';',at)-at);
            };
            renderer=option("renderer");graphics=option("graphics");
            controllerProfile=metadata.options.find("controller_prompts=")!=std::string::npos?option("controller_prompts"):"ps";
            if(option("skip_videos")=="1")setenv("NIGHT100_SKIP_VIDEOS","1",1);else unsetenv("NIGHT100_SKIP_VIDEOS");
            setenv("NIGHT100_CPU_SCALE",option("cpu_scale").c_str(),1);
        }
        const char* sessionCpuScale=std::getenv("NIGHT100_CPU_SCALE");
        const std::string sessionOptions="renderer="+renderer+";graphics="+graphics+";skip_videos="+
            (std::getenv("NIGHT100_SKIP_VIDEOS")?"1":"0")+";cpu_scale="+std::to_string(sessionCpuScale?std::stod(sessionCpuScale):1.0)+
            (controllerProfile=="sw"?";controller_prompts=sw":"");
        if(!inputRecord.empty() && !inputPlayback.empty())throw std::runtime_error("Choose --record-inputs or --replay-inputs");
        if(replayContinue && inputPlayback.empty())throw std::runtime_error("--replay-continue requires --replay-inputs");
        if(!inputRecord.empty() || !inputPlayback.empty()) {
            if(renderer!="gl33" || graphics!="native" || !tracePath.empty())
                throw std::runtime_error("Input sessions currently require the native OpenGL graphics path");
            const auto input=std::filesystem::absolute(inputRecord.empty()?inputPlayback:inputRecord);
            if(!inputRecord.empty()) {
                if(std::filesystem::exists(input))throw std::runtime_error("Input recording already exists: "+input.string());
                std::filesystem::create_directories(input.parent_path());
            } else if(!std::filesystem::is_regular_file(input))throw std::runtime_error("Input recording not found: "+input.string());
            setenv(inputRecord.empty()?"NIGHT100_INPUT_REPLAY":"NIGHT100_INPUT_RECORD",input.c_str(),1);
            if(replayContinue)setenv("NIGHT100_INPUT_RETURN_CONTROL","1",1);
            const char* cpuScale=std::getenv("NIGHT100_CPU_SCALE");
            const std::string options="renderer="+renderer+";graphics="+graphics+";skip_videos="+
                (std::getenv("NIGHT100_SKIP_VIDEOS")?"1":"0")+";cpu_scale="+std::to_string(cpuScale?std::stod(cpuScale):1.0);
            setenv("NIGHT100_INPUT_OPTIONS",options.c_str(),1);
            if(stateDirectory.empty()) {
                const auto stamp=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                stateDirectory=elf.parent_path().parent_path()/"logs"/"input-runs"/
                    (input.stem().string()+(inputRecord.empty()?"-replay-":"-record-")+std::to_string(stamp));
            }
            std::cout<<"[input] diagnostics: "<<std::filesystem::absolute(stateDirectory)<<'\n';
        }
        if (!std::filesystem::is_regular_file(elf)) {
            throw std::runtime_error("ELF not found: " + elf.string() +
                                     ". Run ./night100 prepare or pass the ELF path explicitly.");
        }
        std::cout << "[night100] loading ELF: " << elf << '\n';
        ControllerPrompts::configure(controllerProfile,elf.parent_path().parent_path()/"controllers");
        auto runtimeOwner=std::make_unique<PS2Runtime>();
        PS2Runtime& runtime=*runtimeOwner;
        runtime.setStateClockCallbacks(pauseNativePortClock,resumeNativePortClock);
        if (renderer != "cpu" && renderer != "gl33") throw std::runtime_error("Renderer must be cpu or gl33");
        if (graphics != "native" && graphics != "gs") throw std::runtime_error("Graphics must be native or gs");
        if(controllerProfile=="sw" && (graphics!="native" || renderer!="gl33"))
            throw std::runtime_error("Switch controller prompts require the native GPU graphics path");
        runtime.setMissingFunctionPolicy(PS2Runtime::MissingFunctionPolicy::Stop);
        if (test) return selfTest(runtime, elf);
        struct Capture {
            std::string path;
            double after;
            std::chrono::steady_clock::time_point start;
            bool done = false;
        } capture{capturePath, captureAfter, {}};
        SessionUi sessionUi(ReplayHarness::actions());
        struct WindowUi {SessionUi* ui;Capture* capture;} windowUi{&sessionUi,&capture};
        {
            runtime.setDebugUiCallbacks(
                [](PS2Runtime&, void* data) { static_cast<WindowUi*>(data)->capture->start = std::chrono::steady_clock::now(); },
                [](PS2Runtime& runtime, void* data) {
                    NativeVideo::tickAndDraw(runtime);
                    auto& window=*static_cast<WindowUi*>(data);window.ui->draw();
                    auto& capture=*window.capture;
                    if (!capture.path.empty() && !capture.done && std::chrono::duration<double>(std::chrono::steady_clock::now()-capture.start).count() >= capture.after) {
                        rlDrawRenderBatchActive();
                        const Image screen = LoadImageFromScreen();
                        ExportImage(screen, capture.path.c_str());
                        UnloadImage(screen);
                        capture.done = true;
                    }
                }, [](PS2Runtime&,void*) { NativeVideo::shutdown(); }, &windowUi);
        }
        if (!runtime.initialize("Scooby-Doo! Night of 100 Frights")) return 1;
        SetExitKey(KEY_NULL);
        GL33Backend* gpuBackend = nullptr;
        if (renderer == "gl33" || !tracePath.empty()) {
            std::unique_ptr<GSRasterBackend> backend;
            if (renderer == "gl33") {
                auto gpu = std::make_unique<GL33Backend>();
                gpuBackend = gpu.get();
                std::cout << "[night100] GS OpenGL 3.3 core backend: " << gpu->device() << '\n';
                backend = std::move(gpu);
            } else backend = std::make_unique<GSCpuBackend>();
            if (!tracePath.empty()) backend = GSTrace::record(std::move(backend), tracePath);
            runtime.gs().setRasterBackend(std::move(backend));
        }
        if(graphics=="native" && tracePath.empty())configureNativeScene(gpuBackend);
        else if(!tracePath.empty())std::cout<<"[night100] GS recording uses the reference graphics pipeline\n";
        configureGameBenchmark(gpuBackend,gpuBackend && graphics=="native" && tracePath.empty());
        GameFrameEvents::configure([](uint8_t* ram,PS2Runtime* runtime,void*) {benchmarkFrameCompleted(ram,runtime);});
        const bool interactiveSessions=renderer=="gl33" && graphics=="native" && tracePath.empty() && inputRecord.empty() && inputPlayback.empty()
            && (!std::getenv("NIGHT100_BENCHMARK") || std::getenv("NIGHT100_SESSION_CAPTURE_FRAMES"));
        if(!sessionPlayback.empty() && !interactiveSessions)throw std::runtime_error("Numbered replays require the ordinary native graphics path");
        const char* replayRoot=std::getenv("NIGHT100_REPLAY_DIRECTORY");
        const auto replayDirectory=replayRoot?std::filesystem::absolute(replayRoot):elf.parent_path().parent_path()/"logs"/"replays";
        ReplayHarness::configure({interactiveSessions,replayDirectory,sessionPlayback,sessionOptions},gpuBackend);
        if(metricsVisible || !sessionPlayback.empty())sessionUi.setVisible(true);
        if (!runtime.loadELF(elf.string())) return 1;
        if (!capturePath.empty() || !stateDirectory.empty()) runtime.gs().setDebugHistoryPaused(false);
        if (iso.empty() && std::filesystem::is_regular_file(elf.parent_path()/".disc-image"))
            iso = elf.parent_path()/".disc-image";
        if (iso.empty()) {
            for (const auto& entry : std::filesystem::directory_iterator(elf.parent_path().parent_path())) {
                if (entry.path().extension() == ".iso") { iso = entry.path(); break; }
            }
        }
        if (!iso.empty()) {
            auto paths = PS2Runtime::getIoPaths();
            paths.cdImage = std::filesystem::absolute(iso);
            PS2Runtime::setIoPaths(paths);
        }
        if(!sessionPlayback.empty()){loadMachineSnapshot(runtime,sessionPlayback/"state.bin");ReplayHarness::restored(&runtime);}
        std::jthread watchdog([&](std::stop_token stop) {
            const auto start = std::chrono::steady_clock::now();
            while (!stop.stop_requested()) {
                inputReplayWatchdog(&runtime);
                if (seconds > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count() >= seconds) {
                    runtime.requestStop();
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
        std::exception_ptr gameFailure;
        try {runtime.run();}catch(...){gameFailure=std::current_exception();}
        finishGameBenchmark();
        printGameFrameMetrics();
        watchdog.request_stop();
        watchdog.join();
        if (gpuBackend) {
            const auto stats=gpuBackend->statistics();
            std::cout << "[night100] GPU draws=" << stats.primitives << " CPU raster draws=0"
                      << " submissions=" << stats.submissions << " surface-copies=" << stats.surfaceCopies
                      << " readbacks=" << stats.surfaceReadbacks << " texture-uploads=" << stats.textureUploads << " native-meshes=" << stats.nativeMeshes << '\n';
        }
        std::cout << "[night100] stopped pc=0x" << std::hex << runtime.cpu().pc << std::dec
                  << " dma=" << runtime.memory().dmaStartCount()
                  << " gif=" << runtime.memory().gifCopyCount()
                  << " gs=" << runtime.memory().gsWriteCount()
                  << " vif=" << runtime.memory().vifWriteCount() << '\n';
        bool missing = false;
        const auto kernel = runtime.eeScheduler().snapshot();
        for (const auto& thread : kernel.threads) {
            std::cout << "[night100] thread=" << thread.id << " pc=0x" << std::hex << thread.pc
                      << " ra=0x" << thread.ra << std::dec << " state=" << int(thread.status)
                      << " wait=" << int(thread.waitReason) << " waitId=" << thread.waitId << '\n';
            if (thread.pc != 0 && !runtime.hasFunction(thread.pc)) missing = true;
        }
        const auto gs = runtime.gs().getDebugSnapshot();
        const auto history = runtime.gs().getDebugHistory();
        unsigned draws = 0, transfers = 0;
        for (const auto& event : history) {
            draws += event.kind == GSDebugEventKind::Draw;
            transfers += event.kind == GSDebugEventKind::Transfer;
        }
        std::cout << "[night100] gs-history=" << history.size() << " draws=" << draws
                  << " transfers=" << transfers << " presentation=" << gs.hasHostPresentationFrame
                  << " width=" << gs.hostPresentationWidth << " height=" << gs.hostPresentationHeight
                  << " display-fbp=" << gs.hostPresentationDisplayFbp
                  << " source-fbp=" << gs.hostPresentationSourceFbp << '\n';
        unsigned printedDraws = 0;
        for (auto event = history.rbegin(); event != history.rend() && printedDraws < 3; ++event) {
            if (event->kind != GSDebugEventKind::Draw) continue;
            ++printedDraws;
            std::cout << "[night100] draw fbp=" << event->frame.fbp << " bw=" << event->frame.fbw
                      << " psm=" << int(event->frame.psm) << " mask=0x" << std::hex << event->frame.fbmsk
                      << " test=0x" << event->test << std::dec
                      << " x=" << event->xMin << ',' << event->xMax << " y=" << event->yMin << ',' << event->yMax
                      << " alpha=" << int(event->aMin) << ',' << int(event->aMax) << '\n';
        }
        for (size_t i = history.size() > 8 ? history.size() - 8 : 0; i < history.size(); ++i) {
            const auto& event = history[i];
            std::cout << "[night100] gs-event kind=" << int(event.kind) << " frame=" << event.frameIndex
                      << " nloop=" << event.gifNloop << " format=" << int(event.gifFlg)
                      << " vertices=" << event.vertexCount << " reg=" << int(event.reg)
                      << " value=0x" << std::hex << event.regValue << std::dec << '\n';
        }
        std::vector<uint8_t> pixels;
        uint32_t width = 0, height = 0;
        runtime.gs().copyLatchedHostPresentationFrame(pixels, width, height);
        size_t colored = 0;
        for (size_t i = 0; i + 3 < pixels.size(); i += 4) colored += pixels[i] || pixels[i+1] || pixels[i+2];
        std::cout << "[night100] visible-colored-pixels=" << colored << '/' << width * height << '\n';
        try {finishInputReplay(gameFailure || missing || (gpuBackend && gpuBackend->hasFailed())?"crash":"stop");}
        catch(...){if(!gameFailure)gameFailure=std::current_exception();}
        if (!stateDirectory.empty()) {
            dumpState(runtime, stateDirectory);
            std::ofstream summary(stateDirectory/"run.txt");
            summary<<"ELF: "<<elf<<"\nrenderer: "<<renderer<<"\ngraphics: "<<graphics
                <<"\nrecorded inputs: "<<inputRecord<<"\nreplayed inputs: "<<inputPlayback
                <<"\npc: 0x"<<std::hex<<runtime.cpu().pc<<"\nra: 0x"<<GPR_U32((&runtime.cpu()),31)<<std::dec
                <<"\nmissing target: "<<missing<<"\ngraphics failure: "<<(gpuBackend && gpuBackend->hasFailed())<<'\n';
            if(gameFailure) {
                try{std::rethrow_exception(gameFailure);}
                catch(const std::exception& error){summary<<"runtime failure: "<<error.what()<<'\n';}
                catch(...){summary<<"runtime failure: non-standard exception\n";}
            }
        }
        if(gameFailure)std::rethrow_exception(gameFailure);
        if(gpuBackend && gpuBackend->hasFailed())throw std::runtime_error("OpenGL GS execution failed; see the game-thread error above");
        if (missing) return 2;
        if(const auto replay=ReplayHarness::requestedReplay();!replay.empty()) {
            std::vector<std::string> command{std::filesystem::absolute(argv[0]).string(),elf.string(),"--renderer",renderer,"--graphics",graphics,"--replay-session",replay.string()};
            if(!iso.empty()){command.push_back("--iso");command.push_back(std::filesystem::absolute(iso).string());}
            std::vector<char*> arguments;for(auto& argument:command)arguments.push_back(argument.data());arguments.push_back(nullptr);
            runtimeOwner.reset();
            ::execv(arguments.front(),arguments.data());throw std::runtime_error("Cannot restart the runtime for snapshot playback");
        }
        if(ReplayHarness::verificationFailed())return 2;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "night100: " << error.what() << '\n';
        return 1;
    }
}
