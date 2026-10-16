#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

class PS2Runtime;

namespace InputReplay {
inline constexpr const char* elfSha256="d5febfa97119fba2c2f3eb58abaae268037afae2773ddc57cab7e1c813b485b1";
struct Context {
    uint32_t mode=0,state=0,ostrich=0,scene=0;
    bool operator==(const Context&) const=default;
};
struct Pad {
    uint32_t port=0,slot=0,result=0;
    std::array<uint8_t,32> bytes{};
    bool operator==(const Pad&) const=default;
};
enum class Mode {Record,Replay};
// A final input poll must return to guest code before stopping. Interrupted
// recordings also need time to reach the fault after their final flushed input.
class EndPolicy {
public:
    bool begin(bool interrupted,bool /*atCompletedFrame*/) {
        remaining=interrupted?120:1;
        return remaining==0;
    }
    bool completed(){if(remaining)--remaining;return remaining==0;}
private:
    unsigned remaining=0;
};
// Completed game frames, not host redraws, define the recording clock. A
// recording without a footer is an interrupted session with a valid prefix.
class Session {
public:
    Session(Mode,const std::filesystem::path&,std::string options,
            std::string expectedElf=elfSha256);
    ~Session();
    Session(const Session&)=delete;
    Session& operator=(const Session&)=delete;
    void pad(Pad&,const Context&);
    void completed(const Context&);
    void finish(const std::string& reason="stop");
    // Publish an exclusive, complete copy through the latest completed frame.
    // Pending polls belong to the live recorder and are excluded from the copy.
    void savePrefix(const std::filesystem::path&) const;
    uint64_t frame() const;
    bool ended() const;
    bool interrupted() const;
    bool aligning() const;
    Mode mode() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}

// Call installation after benchmark input hooks so recordings contain the
// effective controller data. The caller's exact-ELF override gates installation.
void installInputReplay(PS2Runtime&);
void inputReplayFrameCompleted(uint8_t*,PS2Runtime*);
// Safe from the host watchdog thread; never touches game-thread session data.
void inputReplayWatchdog(PS2Runtime*);
void finishInputReplay(const std::string& reason="stop");
