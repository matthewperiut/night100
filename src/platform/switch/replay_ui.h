#pragma once
#include "session_ui.h"
#include <string>
#include <vector>
#include <chrono>
class Deko3DBackend;

// HID and catalog work stay on the host; replay callbacks only queue requests
// for the producer's serializable frame boundary.
class SwitchReplayUi {
public:
    explicit SwitchReplayUi(Deko3DBackend&);
    void poll();
    void close();
    // Shows a short on-screen message (same timeout as replay notices).
    void notify(std::string text);
private:
    Deko3DBackend& gpu;
    SessionUi::Actions actions;
    std::vector<SessionUi::Recording> entries;
    std::string lastText,message;
    bool picker=false;
    size_t selected=0;
    uint64_t statusRevision=~uint64_t(0);
    std::chrono::steady_clock::time_point noticeUntil{};
    void refresh();
};
