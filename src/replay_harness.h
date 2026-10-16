#pragma once
#include "input_replay.h"
#include "session_ui.h"
#include <filesystem>
#include <functional>
#include <cstdint>
#include <string>
class PS2Runtime;
#if defined(__SWITCH__)
class Deko3DBackend;
#else
class GL33Backend;
#endif
namespace ReplayHarness {
#if defined(__SWITCH__)
using Backend=Deko3DBackend;
#else
using Backend=GL33Backend;
#endif
struct Hooks {
    std::function<void(PS2Runtime&,const std::filesystem::path&)> captureImage;
    // Producer-only presentation while the picker has frozen game execution.
    std::function<void()> presentPaused;
};
struct UiState {
    bool recording=false,playing=false,busy=false;
    uint64_t sessionId=0,statusRevision=0;
};
struct Settings {
    bool enabled=false;
    std::filesystem::path directory;
    std::filesystem::path replay;
    std::string options;
};
void configure(Settings,Backend*,Hooks={});
UiState uiState();
void excludeHostTime(uint64_t nanoseconds);
void install();
bool enabled();
SessionUi::Actions actions();
void pad(InputReplay::Pad&,const InputReplay::Context&);
void completed(uint8_t*,PS2Runtime*);
void watchdog(PS2Runtime*);
void finish(const std::string&);
std::filesystem::path requestedReplay();
bool verificationFailed();
uint64_t clockTicks(uint64_t actual);
void restored(PS2Runtime*);
}
