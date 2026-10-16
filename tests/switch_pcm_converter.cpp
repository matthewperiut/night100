#include "../platform/switch/host/pcm_converter.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>

static int16_t source(uint64_t frame, unsigned channel) {
    return int16_t((int(frame % 511) - 255) * 30 + channel * 103);
}
static void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}
int main() {try {
    unsigned scenarios = 0;
    for (unsigned rate : {8000u, 11025u, 22050u, 32000u, 44100u, 48000u, 96000u, 192000u})
        for (unsigned channels : {1u, 2u}) {
            SwitchPcm::Converter converter(rate, channels, 1024);
            uint64_t sourceCursor = 0, outputCursor = 0;
            for (unsigned block = 0; block < 120; ++block) {
                std::array<int32_t, 2048> output{};
                converter.addTo(output.data(), 1024, [&](int16_t* destination, unsigned frames) {
                    for (unsigned frame = 0; frame < frames; ++frame)
                        for (unsigned channel = 0; channel < channels; ++channel)
                            destination[frame * channels + channel] = source(sourceCursor + frame, channel);
                    sourceCursor += frames;
                });
                for (unsigned frame = 0; frame < 1024; ++frame)
                    for (unsigned channel = 0; channel < 2; ++channel) {
                        const uint64_t numerator = (outputCursor + frame) * rate;
                        const uint64_t first = numerator / 48000;
                        const unsigned inputChannel = channels == 1 ? 0 : channel;
                        const long double fraction = (numerator % 48000) / 48000.0L;
                        const long double expected = source(first, inputChannel) * (1 - fraction)
                                                   + source(first + 1, inputChannel) * fraction;
                        require(std::abs(output[frame * 2 + channel] - int32_t(expected)) <= 1, "PCM waveform conversion differs");
                    }
                outputCursor += 1024;
                const uint64_t nominal = outputCursor * rate / 48000;
                require(sourceCursor >= nominal && sourceCursor <= nominal + 2, "PCM source cursor drifted");
            }
            ++scenarios;
        }
    std::printf("Switch PCM conversion: %u rate/channel scenarios passed\n", scenarios);
} catch(const std::exception& error) {std::fprintf(stderr,"%s\n",error.what());return 1;}}
