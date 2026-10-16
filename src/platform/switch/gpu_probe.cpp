#include <switch.h>
#include "graphics/deko3d_backend.h"
#include "telemetry.h"
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>

// Use the real process limit in both application and hbmenu applet modes.
extern "C" {size_t __nx_heap_size=0;}

namespace {
struct Options {
    std::string root="/switch/night100";
    std::string logId=std::to_string(armGetSystemTick());
    double logInterval=15;
    double seconds=0;
};
Options options(int argc,char** argv) {
    Options result;
    for(int i=1;i<argc;++i) {
        const std::string argument=argv[i];
        auto value=[&]()->std::string {
            if(i+1>=argc)throw std::runtime_error("Missing value for "+argument);
            return argv[++i];
        };
        if(argument=="--gpu-self-test")continue; // The probe always runs these fixtures.
        if(argument=="--skip-videos")continue; // Same launch arguments; this NRO has no movie player.
        if(argument=="--root")result.root=value();
        else if(argument=="--log-id")result.logId=value();
        else if(argument=="--log-interval" || argument=="--seconds") {
            const auto text=value();size_t consumed=0;const double parsed=std::stod(text,&consumed);
            if(consumed!=text.size() || !std::isfinite(parsed))
                throw std::runtime_error("Invalid numeric argument: "+argument);
            if(argument=="--seconds")result.seconds=parsed;else result.logInterval=parsed;
        }else throw std::runtime_error("Unknown GPU probe argument: "+argument);
    }
    if(result.seconds<0 || result.seconds>86400)throw std::runtime_error("Run duration must be 0..86400 seconds");
    if(!result.root.starts_with("/switch/") || result.root.ends_with('/') || result.root.find("//")!=std::string::npos ||
       result.root.find("/../")!=std::string::npos || result.root.ends_with("/..") ||
       result.root.find("/./")!=std::string::npos || result.root.ends_with("/."))
        throw std::runtime_error("Use an application folder beneath /switch, such as /switch/night100");
    for(unsigned char c:result.root)if(c<32 || c==127 || c==':' || c=='\\')
        throw std::runtime_error("Invalid SD root path character");
    return result;
}
}

int main(int argc,char** argv) {
    std::unique_ptr<SwitchTelemetry::Session> telemetry;
    bool mountedRomfs=false;
    try {
        const auto config=options(argc,argv);
        telemetry=std::make_unique<SwitchTelemetry::Session>(config.logId,config.logInterval,config.root);
        if(!fsdevGetDeviceFileSystem("sdmc") && R_FAILED(fsdevMountSdmc()))
            throw std::runtime_error("Unable to mount SD diagnostics filesystem");
        if(chdir("sdmc:/"))throw std::runtime_error("Unable to select the SD filesystem");
        if(R_FAILED(romfsInit()))throw std::runtime_error("GPU probe NRO is missing its embedded shaders");
        mountedRomfs=true;
        telemetry->stage("gpu_probe_start");
        {
            Deko3DBackend backend;
            if(!backend.runSelfTest())throw std::runtime_error("Deko GPU fixtures failed");
        }
        telemetry->finish("NIGHT100_SWITCH_GPU_TEST_OK");
        romfsExit();return 0;
    }catch(const std::exception& error) {
        if(telemetry) {
            try {telemetry->finish(std::string("NIGHT100_SWITCH_FATAL ")+error.what());}
            catch(...) {}
        }else {
            consoleInit(nullptr);
            std::printf("NIGHT100_SWITCH_FATAL %s\n",error.what());
            const auto end=armGetSystemTick()+10*19200000ull;
            while(appletMainLoop() && armGetSystemTick()<end) {
                consoleUpdate(nullptr);svcSleepThread(16000000);
            }
            consoleExit(nullptr);
        }
        if(mountedRomfs)romfsExit();
        return 1;
    }
}
