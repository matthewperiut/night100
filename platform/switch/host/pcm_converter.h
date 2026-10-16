#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace SwitchPcm {
// Converts source callback frames to the audout device's 48 kHz stereo frames.
// A rational phase preserves exact source consumption across buffer boundaries.
class Converter {
public:
  Converter(unsigned rate, unsigned channels, unsigned frames)
      : rate(rate), channels(channels) {
    pending.reserve((uint64_t(frames) * rate / 48000 + 3) * channels);
  }
  template <class Callback>
  void addTo(int32_t *stereo, unsigned frames, Callback callback) {
    const uint64_t end = phase + uint64_t(frames) * rate;
    const size_t required = std::max(
        (phase + uint64_t(frames - 1) * rate) / 48000 + 2, end / 48000);
    const size_t available = pending.size() / channels;
    if (required > available) {
      pending.resize(required * channels, 0);
      callback(pending.data() + available * channels,
               unsigned(required - available));
    }
    for (unsigned frame = 0; frame < frames; ++frame) {
      const uint64_t position = phase + uint64_t(frame) * rate;
      const size_t first = position / 48000;
      const double fraction = double(position % 48000) / 48000.0;
      for (unsigned channel = 0; channel < 2; ++channel) {
        const unsigned sourceChannel = channels == 1 ? 0 : channel;
        const int16_t a = pending[first * channels + sourceChannel];
        const int16_t b = pending[(first + 1) * channels + sourceChannel];
        stereo[frame * 2 + channel] += int32_t(a + (b - a) * fraction);
      }
    }
    const size_t consumed = end / 48000;
    pending.erase(pending.begin(), pending.begin() + consumed * channels);
    phase = end % 48000;
  }

private:
  unsigned rate, channels;
  uint64_t phase = 0;
  std::vector<int16_t> pending;
};
} // namespace SwitchPcm
