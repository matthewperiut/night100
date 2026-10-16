#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace MoviePlayback {
struct MovieAudio {
    // Native signed PCM16, ordinary frame/channel interleaving.
    std::vector<int16_t> interleaved;
    unsigned sampleRate = 0;
    unsigned channels = 0;
    // Original MPEG PES timestamp, before the player's common A/V rebase.
    std::optional<double> initialPtsSeconds;
};
// Reads original PSS audio without modifying the movie. Returns an empty value
// when no supported private PCM stream exists; recognized malformed audio throws.
// File size <=1 GiB and decoded PCM <=256 MiB. No host audio API is called.
MovieAudio loadOriginalMovieAudio(const std::filesystem::path& path);
}
