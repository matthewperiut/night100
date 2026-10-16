#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// The host window owns this object. Callbacks publish requests to game-thread
// owners; this UI never reads guest memory or changes replay sessions directly.
class SessionUi {
public:
    enum class GpuState { Pending, Unsupported, Available };
    struct Metrics {
        std::optional<double> gameFps,cpuMilliseconds,gpuMilliseconds;
        GpuState gpuState=GpuState::Pending;
    };
    struct Recording {std::filesystem::path path;std::string label;uint64_t id=0;};
    struct Actions {
        std::function<void(bool)> setOverlayVisible,setPickerOpen;
        std::function<Metrics()> metrics;
        std::function<void()> startRecording,stopRecording;
        std::function<std::vector<Recording>()> recordings;
        std::function<void(const std::filesystem::path&)> replay;
        std::function<std::string()> status;
    };
    explicit SessionUi(Actions);
    // Invoke inside BeginDrawing/EndDrawing, after the game's completed image.
    void draw();
    bool visible() const{return overlayVisible;}
    bool pickerOpen() const{return pickerVisible;}
    void setVisible(bool value){overlayVisible=value;if(actions.setOverlayVisible)actions.setOverlayVisible(value);}
private:
    Actions actions;
    bool overlayVisible=false,pickerVisible=false;
    std::vector<Recording> recordingList;
    std::string selection,message,lastStatus;
    double messageUntil=0,statusUntil=0;
    size_t page=0;
    uint64_t largestId=0;
    void notify(std::string);
    void closePicker();
    void poll();
    void drawOverlay() const;
    void drawPicker();
};
