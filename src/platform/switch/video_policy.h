#pragma once

#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace SwitchVideo {
// These are optional logos, trailers and attract demos in the original catalog.
// Story/introduction/credits movies and unidentified streams remain strict.
inline bool optionalMovie(std::string_view directory, std::string_view filename) {
    auto equal = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i=0; i<a.size(); ++i) {
            const auto upper=[](char c) { return c>='a' && c<='z' ? char(c-'a'+'A') : c; };
            if (upper(a[i]) != upper(b[i])) return false;
        }
        return true;
    };
    if (equal(directory,"FMV")) {
        for (auto name : {"THQLOGO.PSS", "HILOGO.PSS", "WBLOGO.PSS", "RW4.PSS",
                          "HWTRAILR.PSS", "SDTRAILR.PSS"})
            if (equal(filename,name)) return true;
    }
    if (!equal(directory,"FMV/DEMO") || filename.size()<9 || filename.size()>10
        || !equal(filename.substr(0,4),"DEMO")
        || !equal(filename.substr(filename.size()-4),".PSS")) return false;
    const auto number=filename.substr(4,filename.size()-8);
    if (number.size()>1 && number.front()=='0') return false;
    unsigned value=0;
    for (char digit : number) {
        if (digit<'0' || digit>'9') return false;
        value=value*10+unsigned(digit-'0');
    }
    return value>=1 && value<=15;
}

enum class VideoSupport { HardwareMpeg2, Unsupported, Unidentified };
enum class VideoDecision { Play, SkipOptional, Fail };
inline VideoDecision movieDecision(VideoSupport support, bool optional) {
    if (support==VideoSupport::HardwareMpeg2) return VideoDecision::Play;
    if (support==VideoSupport::Unsupported && optional) return VideoDecision::SkipOptional;
    return VideoDecision::Fail;
}

// Exclude background/suspend time without losing the original timestamp origin.
// Saturate defensively for out-of-order observations; normal counters are monotonic.
inline uint64_t activeMovieNanoseconds(uint64_t wall, uint64_t inactiveAtStart,
                                      uint64_t inactiveNow) {
    const uint64_t inactive=inactiveNow>=inactiveAtStart ? inactiveNow-inactiveAtStart : 0;
    return wall>inactive ? wall-inactive : 0;
}
}
