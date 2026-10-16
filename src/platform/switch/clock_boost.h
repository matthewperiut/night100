#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

// Homebrew-only clock override through libnx clkrst. Unverified on hardware
// until a benchmark records the read-back rates. Mode Default opens the
// sessions only to read and log the system's rates; Fast also requests
// CPU 1785 MHz, GPU 921 MHz (768 MHz fallback) and EMC 1600 MHz while the
// application is in focus. Rates are restored on focus loss and exit.
namespace SwitchClockBoost {
enum class Mode : uint8_t {Default,Fast};
// Returns false for unrecognized text.
bool parseMode(const std::string& text,Mode& mode);
const char* modeName(Mode mode);

class Controller {
public:
    explicit Controller(Mode mode);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;
    // Applies or restores rates for the focus state. Never throws.
    void setActive(bool active);
    // Reads back all three rates (IPC; call periodically, not per frame).
    void refresh();
    // Last read-back line for periodic summaries; does not touch the services.
    std::string summary() const;
    bool available() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    mutable std::mutex mutex;
};
}
