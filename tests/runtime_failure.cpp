#include "ps2_runtime.h"
#include "ps2_host_backend.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
constexpr uint32_t entry = 0x1000;
enum class Scenario { Normal, StandardException, UnknownException, PresentationException };
struct GuestFailure : std::runtime_error
{
    GuestFailure() : std::runtime_error("synthetic guest execution failure") {}
    int payload = 901;
};
struct State
{
    Scenario scenario;
    std::atomic<bool> entered{false};
    unsigned initialized = 0, shutdown = 0;
    bool callbackOnWrongThread = false;
    std::thread::id hostThread = std::this_thread::get_id();
};
State *active = nullptr;
void require(bool value, const char *why)
{
    if (!value) throw std::runtime_error(why);
}
void guest(uint8_t *, R5900Context *, PS2Runtime *runtime)
{
    active->entered = true;
    switch (active->scenario)
    {
    case Scenario::StandardException: throw GuestFailure{};
    case Scenario::UnknownException: throw uint32_t(0xabc123);
    case Scenario::Normal: runtime->requestStop(); break;
    case Scenario::PresentationException: break; // Host draw failure must stop/join this live producer.
    }
}
void initialized(PS2Runtime &, void *user)
{
    auto &state = *static_cast<State *>(user);
    ++state.initialized;
    state.callbackOnWrongThread |= std::this_thread::get_id() != state.hostThread;
}
void draw(PS2Runtime &, void *user)
{
    auto &state = *static_cast<State *>(user);
    if (state.scenario == Scenario::PresentationException && state.entered.load(std::memory_order_acquire))
        throw GuestFailure{};
}
void shutdown(PS2Runtime &, void *user)
{
    auto &state = *static_cast<State *>(user);
    ++state.shutdown;
    state.callbackOnWrongThread |= std::this_thread::get_id() != state.hostThread;
}
void runScenario(Scenario scenario)
{
    State state{scenario};
    active = &state;
    bool standardCaught = false, unknownCaught = false, wrongException = false;
    {
        PS2Runtime runtime;
        runtime.setDebugUiCallbacks(initialized, draw, shutdown, &state);
        require(runtime.initialize("Night100 runtime failure regression"), "runtime initialization failed");
        SetWindowSize(160, 120);
        SetWindowState(FLAG_WINDOW_HIDDEN);
        require(runtime.registerFunction(entry, guest), "synthetic EE function registration failed");
        runtime.cpu().pc = entry;

        // A broken dispatcher must fail the test instead of hanging forever.
        std::mutex mutex;
        std::condition_variable done;
        bool finished = false;
        std::atomic<bool> timedOut{false};
        std::thread watchdog([&] {
            std::unique_lock lock(mutex);
            if (!done.wait_for(lock, std::chrono::seconds(3), [&] { return finished; }))
            {
                timedOut.store(true);
                runtime.requestStop();
            }
        });
        try { runtime.run(); }
        catch (const GuestFailure &failure)
        {
            standardCaught = failure.payload == 901 &&
                std::string(failure.what()) == "synthetic guest execution failure";
        }
        catch (uint32_t failure) { unknownCaught = failure == 0xabc123; }
        catch (...) { wrongException = true; }
        {
            std::lock_guard lock(mutex);
            finished = true;
        }
        done.notify_one();
        watchdog.join();

        require(!timedOut.load(), "runtime failed to finish its synthetic EE execution");
        require(state.entered, "synthetic function did not execute on the real game thread");
        require(!wrongException, "runtime changed the guest exception type");
        require(standardCaught == (scenario == Scenario::StandardException || scenario == Scenario::PresentationException),
                "runtime swallowed or changed a standard game-thread exception");
        require(unknownCaught == (scenario == Scenario::UnknownException),
                "runtime swallowed or changed a non-standard game-thread exception");
        require(!IsWindowReady(), "runtime rethrew before closing its window");
        require(state.initialized == 1 && state.shutdown == 1,
                "debug UI cleanup did not execute exactly once before returning to the caller");
        require(!state.callbackOnWrongThread, "window cleanup ran on the game thread");
    }
    require(state.shutdown == 1, "runtime destructor repeated debug UI cleanup");
    require(!IsAudioDeviceReady(), "runtime destruction did not close its audio device");
    active = nullptr;
}
}

int main()
{
    try
    {
        runScenario(Scenario::StandardException);
        runScenario(Scenario::UnknownException);
        runScenario(Scenario::Normal);
        runScenario(Scenario::PresentationException);
        std::cout << "PASS: real runtime propagates game-thread and presentation exceptions after producer join and window/audio cleanup; normal stop succeeds\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
