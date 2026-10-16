#include "runtime/ps2_pcm_ring.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

static void check(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}

int main()
{
    PS2PcmRing ring;
    check(ring.queuedFrames() == 0, "new ring is not empty");
    std::vector<int16_t> input((PS2PcmRing::Capacity + 8) * 2, 1729);
    std::vector<int16_t> output(input.size(), -1);
    check(ring.read(output.data(), 8) == 0, "empty ring supplied frames");
    for (size_t i = 0; i < 16; ++i) check(output[i] == 0, "underrun did not emit silence");
    check(ring.write(input.data(), PS2PcmRing::Capacity + 8) == PS2PcmRing::Capacity,
          "overflow did not enforce bounded capacity");
    check(ring.write(input.data(), 1) == 0, "full ring overwrote unread frames");
    check(ring.queuedFrames() == PS2PcmRing::Capacity, "full ring occupancy is incorrect");
    check(ring.read(output.data(), PS2PcmRing::Capacity + 8) == PS2PcmRing::Capacity,
          "full read lost queued frames");
    for (size_t i = 0; i < PS2PcmRing::Capacity * 2; ++i)
        check(output[i] == 1729, "overflow corrupted queued audio");
    for (size_t i = PS2PcmRing::Capacity * 2; i < output.size(); ++i)
        check(output[i] == 0, "partial underrun left stale samples");
    check(ring.queuedFrames() == 0, "drained ring retained queued frames");

    // A load stall emits silence, then the guest produces its overdue PCM.
    // Later effects must not wait behind that stale backlog. Retain the newest
    // stereo frames across ring wrap, then let the producer reuse freed space.
    for (size_t i = 0; i < 6000; ++i) {
        input[i * 2] = int16_t(i); input[i * 2 + 1] = -int16_t(i);
    }
    check(ring.read(output.data(), 2048) == 0, "stall did not emit silence");
    check(ring.write(input.data(), 6000) == 6000, "catch-up write failed");
    check(ring.trimLatency(2048, 1024) == 4976, "stale backlog was not trimmed");
    check(ring.queuedFrames() == 1024, "trim retained the wrong cushion");
    check(ring.trimLatency(2048, 1024) == 0, "healthy queue was trimmed");
    check(ring.write(input.data(), 1000) == 1000, "producer could not reuse trimmed space");
    check(ring.read(output.data(), 2024) == 2024, "trim lost recent frames");
    for (size_t i = 0; i < 2024; ++i) {
        const int16_t expected = int16_t(i < 1024 ? 4976 + i : i - 1024);
        check(output[i * 2] == expected && output[i * 2 + 1] == -expected,
              "latency trim corrupted recent stereo order");
    }

    constexpr size_t total = 1000000;
    std::thread producer([&] {
        std::array<int16_t, 73 * 2> block{};
        size_t sent = 0;
        while (sent < total)
        {
            const size_t frames = std::min(size_t(73), total - sent);
            for (size_t i = 0; i < frames; ++i)
            {
                const auto value = int16_t((sent + i) % 32767);
                block[i * 2] = value; block[i * 2 + 1] = -value;
            }
            sent += ring.write(block.data(), frames);
            std::this_thread::yield();
        }
    });
    std::array<int16_t, 109 * 2> block{};
    size_t received = 0;
    bool valid = true;
    while (received < total)
    {
        const size_t count = ring.read(block.data(), std::min(size_t(109), total - received));
        for (size_t i = 0; i < count; ++i)
        {
            const auto value = int16_t((received + i) % 32767);
            valid &= block[i * 2] == value && block[i * 2 + 1] == -value;
        }
        received += count;
        std::this_thread::yield();
    }
    producer.join();
    check(valid, "concurrent wraparound reordered or corrupted stereo frames");

    PS2PcmRing delayed;
    std::thread catchup([&] {
        std::array<int16_t, 73 * 2> samples{};
        size_t sent = 0;
        while (sent < total) {
            const size_t frames = std::min(size_t(73), total - sent);
            for (size_t i = 0; i < frames; ++i) {
                const auto value = int16_t((sent + i) % 32767);
                samples[i * 2] = value; samples[i * 2 + 1] = -value;
            }
            sent += delayed.write(samples.data(), frames);
            std::this_thread::yield();
        }
    });
    while (delayed.queuedFrames() < 6000) std::this_thread::yield();
    received = 0; size_t skipped = 0; valid = true;
    while (received + skipped < total) {
        skipped += delayed.trimLatency(2048, 1024);
        const size_t count = delayed.read(block.data(), std::min(size_t(109), total - received - skipped));
        for (size_t i = 0; i < count; ++i) {
            const auto value = int16_t((received + skipped + i) % 32767);
            valid &= block[i * 2] == value && block[i * 2 + 1] == -value;
        }
        received += count;
        std::this_thread::yield();
    }
    catchup.join();
    check(skipped > 0 && received + skipped == total && delayed.queuedFrames() == 0,
          "concurrent trim lost frame accounting");
    check(valid, "concurrent trim raced the producer or corrupted recent stereo frames");
    std::cout << "PASS: bounded stereo PCM queue, stale latency recovery and 2,000,000 concurrent frames\n";
}
