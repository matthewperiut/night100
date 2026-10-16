// Checks the whole-file cache for compact disc sources. Every read is compared
// byte-for-byte with an expected logical image, under each cache policy: off,
// whole-file, whole-file with prefetch, a cap that forces eviction, a cap smaller
// than every file, and concurrent readers. Also checks that a failed block decode
// cannot leave a stale block marked valid.
#include "runtime/resource_disc.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

#include <vector>
#include <zlib.h>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void number(std::vector<uint8_t>& out, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

void writeFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    require(bool(file), "fixture write failed");
}

struct Extent {
    uint64_t logical, length, offset, stored;
    uint32_t encoding;
    std::string path;
};

std::vector<uint8_t> manifest(uint64_t length, const std::vector<Extent>& extents) {
    std::vector<uint8_t> out{'N', '1', '0', '0', 'D', 'S', 'C', '1'};
    number(out, length, 8);
    number(out, extents.size(), 4);
    for (const auto& e : extents) {
        number(out, e.logical, 8);
        number(out, e.length, 8);
        number(out, e.offset, 8);
        number(out, e.stored, 8);
        number(out, e.encoding, 4);
        number(out, e.path.size(), 4);
        out.insert(out.end(), e.path.begin(), e.path.end());
    }
    return out;
}

std::vector<uint8_t> compress(const std::vector<uint8_t>& raw) {
    uLongf size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> out(size);
    require(compress2(out.data(), &size, raw.data(), static_cast<uLong>(raw.size()), Z_BEST_COMPRESSION) == Z_OK,
            "zlib compress failed");
    out.resize(size);
    return out;
}

// Fixture: raw sources, a blocks file of zlib streams, and a manifest whose
// extents exercise partial offsets, zero runs, compressed blocks and reuse of a
// source file across several extents.
struct Fixture {
    std::filesystem::path root;
    std::filesystem::path manifestPath;
    std::vector<uint8_t> expected; // full logical image
    uint64_t total = 0;
};

Fixture buildFixture(const std::filesystem::path& root, const std::string& manifestName) {
    std::filesystem::create_directories(root);
    std::mt19937_64 rng(0x6e31303043414348ull);
    auto random = [&](size_t n) {
        std::vector<uint8_t> v(n);
        for (auto& b : v) b = static_cast<uint8_t>(rng());
        return v;
    };
    const std::vector<uint8_t> rawA = random(90000), rawB = random(70000), rawC = random(80000);
    writeFile(root / "rawA.bin", rawA);
    writeFile(root / "rawB.bin", rawB);
    writeFile(root / "rawC.bin", rawC);

    // Three blocks: patterned, noisy-with-repeats, and small.
    std::vector<uint8_t> block0(40000), block1(65536), block2(1234);
    for (size_t i = 0; i < block0.size(); ++i) block0[i] = static_cast<uint8_t>((i * 7) % 251);
    block1 = random(65536);
    std::copy(block1.begin(), block1.begin() + 20000, block1.begin() + 30000);
    block2 = random(1234);
    const auto z0 = compress(block0), z1 = compress(block1), z2 = compress(block2);
    std::vector<uint8_t> blocks;
    blocks.insert(blocks.end(), z0.begin(), z0.end());
    const uint64_t off1 = blocks.size();
    blocks.insert(blocks.end(), z1.begin(), z1.end());
    const uint64_t off2 = blocks.size();
    blocks.insert(blocks.end(), z2.begin(), z2.end());
    writeFile(root / "blocks.bin", blocks);

    std::vector<Extent> extents{
        {0, 60000, 0, 60000, 0, "rawA.bin"},
        {0, 5000, 0, 0, 1, ""},
        {0, 40000, 0, z0.size(), 2, "blocks.bin"},
        {0, 60000, 1000, 60000, 0, "rawB.bin"},
        {0, 65536, off1, z1.size(), 2, "blocks.bin"},
        {0, 70000, 5000, 70000, 0, "rawC.bin"},
        {0, 1234, off2, z2.size(), 2, "blocks.bin"},
        {0, 50000, 30000, 50000, 0, "rawA.bin"},
        {0, 100, 0, 0, 1, ""},
        {0, 80000, 0, 80000, 0, "rawC.bin"},
    };
    Fixture fixture;
    fixture.root = root;
    fixture.manifestPath = root / manifestName;
    uint64_t logical = 0;
    std::vector<uint8_t>& image = fixture.expected;
    auto appendSlice = [&](const std::vector<uint8_t>& source, uint64_t offset, uint64_t length) {
        image.insert(image.end(), source.begin() + offset, source.begin() + offset + length);
    };
    for (auto& e : extents) {
        e.logical = logical;
        if (e.encoding == 1) {
            image.insert(image.end(), e.length, 0);
        } else if (e.path == "rawA.bin") {
            appendSlice(rawA, e.offset, e.length);
        } else if (e.path == "rawB.bin") {
            appendSlice(rawB, e.offset, e.length);
        } else if (e.path == "rawC.bin") {
            appendSlice(rawC, e.offset, e.length);
        } else {
            // Compressed extents were written in order: block0, block1, block2.
            const auto& block = e.offset == 0 ? block0 : e.offset == off1 ? block1 : block2;
            appendSlice(block, 0, e.length);
        }
        logical += e.length;
    }
    fixture.total = logical;
    writeFile(fixture.manifestPath, manifest(logical, extents));
    return fixture;
}

// Reads a range and compares with the expected image, clipped at EOF.
void checkRange(ps2x::ResourceDisc& disc, const std::vector<uint8_t>& expected, uint64_t offset, size_t length) {
    std::vector<uint8_t> actual(length, 0x5a);
    size_t read = 0;
    require(disc.read(offset, actual.data(), length, read), "read failed");
    const size_t wanted = offset >= expected.size() ? 0 : static_cast<size_t>(std::min<uint64_t>(length, expected.size() - offset));
    require(read == wanted, "short-read count differs from the expected image");
    if (wanted == 0) return;
    require(std::equal(actual.begin(), actual.begin() + static_cast<std::ptrdiff_t>(read), expected.begin() + static_cast<std::ptrdiff_t>(offset)),
            "bytes differ from the expected image");
}

void checkAllRanges(ps2x::ResourceDisc& disc, const Fixture& fixture, uint64_t seed, unsigned randomCount) {
    std::mt19937_64 rng(seed);
    // Every extent boundary, plus one byte either side, with several lengths.
    const std::vector<size_t> lengths{1, 2, 27, 28, 29, 1024, 4096, 65536, 70001};
    for (uint64_t b : {uint64_t(0), uint64_t(60000), uint64_t(65000), uint64_t(70000), uint64_t(70100), uint64_t(70100 + 40000)}) {
        for (size_t length : lengths) {
            for (int delta = -1; delta <= 1; ++delta) {
                const int64_t offset = static_cast<int64_t>(b) + delta;
                if (offset >= 0) checkRange(disc, fixture.expected, static_cast<uint64_t>(offset), length);
            }
        }
    }
    for (unsigned i = 0; i < randomCount; ++i) {
        const uint64_t offset = rng() % (fixture.total + 100);
        const size_t length = 1 + static_cast<size_t>(rng() % 90000);
        checkRange(disc, fixture.expected, offset, length);
    }
    // Entire disc in one read.
    checkRange(disc, fixture.expected, 0, static_cast<size_t>(fixture.total));
}

void runPolicy(const char* name, ps2x::ResourceCacheMode mode, uint64_t cap,
               const Fixture& fixture, bool expectEviction, bool expectUncached) {
    ps2x::ResourceDisc::setCacheMode(mode, cap);
    auto disc = ps2x::ResourceDisc::open(fixture.manifestPath);
    require(bool(disc), "manifest not recognized");
    require(disc->size() == fixture.total, "logical size differs");
    checkAllRanges(*disc, fixture, 0x1234 + cap, 1500);
    const auto stats = ps2x::ResourceDisc::cacheStats();
    if (mode == ps2x::ResourceCacheMode::Off) {
        require(stats.loads == 0 && stats.hits == 0 && stats.files == 0, "off mode touched the cache");
    } else {
        require(stats.bytes <= cap, "cache exceeded its cap");
        require(stats.loads > 0 && stats.hits > 0, "cache did not load or serve from memory");
    }
    if (expectEviction) require(stats.evictions > 0, "eviction path was not exercised");
    if (expectUncached) require(stats.uncached > 0, "over-cap files were not declined");
    std::cout << "PASS: " << name << " (loads=" << stats.loads << " hits=" << stats.hits
              << " evictions=" << stats.evictions << " uncached=" << stats.uncached
              << " resident=" << stats.bytes << " bytes)\n";
}

void checkDecodeInvalidation(const std::filesystem::path& root) {
    // Block 0 is valid; block 1 is garbage. A failed decode must not leave block 0
    // marked as the scratch block.
    std::mt19937_64 rng(77);
    std::vector<uint8_t> junk(100);
    for (auto& b : junk) b = static_cast<uint8_t>(rng());
    std::vector<uint8_t> blocks;
    const auto valid = compress(std::vector<uint8_t>(20000, 0x33));
    blocks.insert(blocks.end(), valid.begin(), valid.end());
    blocks.insert(blocks.end(), junk.begin(), junk.end());
    writeFile(root / "invalidation.bin", blocks);
    std::vector<Extent> extents{
        {0, 20000, 0, valid.size(), 2, "invalidation.bin"},
        {0, 5000, 0, 100, 2, "invalidation.bin"},
    };
    std::vector<uint8_t> image(20000, 0x33);
    image.resize(25000, 0x00);
    // Logical offsets of the second extent.
    extents[1].logical = 20000;
    extents[1].offset = valid.size();
    writeFile(root / "invalidation.disc", manifest(25000, extents));
    ps2x::ResourceDisc::setCacheMode(ps2x::ResourceCacheMode::Off);
    auto disc = ps2x::ResourceDisc::open(root / "invalidation.disc");
    require(bool(disc), "invalidation manifest not recognized");
    checkRange(*disc, image, 0, 20000);
    size_t read = 0;
    std::vector<uint8_t> out(5000);
    require(!disc->read(20000, out.data(), out.size(), read), "garbage block decoded as valid");
    checkRange(*disc, image, 100, 19900);
    std::cout << "PASS: failed block decode invalidates the scratch block\n";
}

// The Switch order: the cache is Off, then set to whole-file, then the disc is
// opened and read for the first time. The read runs under a deadline, because the
// device hung on this exact sequence. A hang reports failure instead of blocking.
void firstReadAfterOffToFileTransition(const std::filesystem::path& root) {
    const Fixture fixture = buildFixture(root / "transition", "transition.bin");
    ps2x::ResourceDisc::setCacheMode(ps2x::ResourceCacheMode::Off);
    ps2x::ResourceDisc::setCacheMode(ps2x::ResourceCacheMode::WholeFile, ps2x::ResourceDisc::kDefaultCacheBytes);
    auto task = std::async(std::launch::async, [&fixture] {
        auto disc = ps2x::ResourceDisc::open(fixture.manifestPath);
        require(bool(disc), "transition manifest not recognized");
        checkRange(*disc, fixture.expected, 0, 4096);
        checkRange(*disc, fixture.expected, 0, static_cast<size_t>(fixture.total));
        return ps2x::ResourceDisc::cacheStats();
    });
    if (task.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
        std::cerr << "first cached read after Off->file did not complete within 30 s\n";
        std::_Exit(2);
    }
    const auto stats = task.get();
    require(stats.loads >= 1 && stats.files >= 1, "first read did not make a file resident");
    std::cout << "PASS: first read after Off->file transition (loads=" << stats.loads << ")\n";
}

void checkConcurrentReaders(const Fixture& fixture) {
    ps2x::ResourceDisc::setCacheMode(ps2x::ResourceCacheMode::WholeFile, 100000);
    auto disc = ps2x::ResourceDisc::open(fixture.manifestPath);
    require(bool(disc), "concurrent manifest not recognized");
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            try {
                checkAllRanges(*disc, fixture, 0x9000 + t, 300);
            } catch (...) {
                failed = true;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    require(!failed.load(), "concurrent reader observed wrong bytes");
    std::cout << "PASS: four concurrent readers under eviction churn\n";
}
}

int main() {
    try {
        const auto root = std::filesystem::temp_directory_path() / ("night100-resource-cache-" + std::to_string(::getpid()));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        const Fixture fixture = buildFixture(root / "main", "disc.bin");

        firstReadAfterOffToFileTransition(root);

        // Off is the reference path. Its output is what the cache must reproduce.
        runPolicy("off: streaming reference", ps2x::ResourceCacheMode::Off, 0, fixture, false, false);
        runPolicy("whole-file with default cap", ps2x::ResourceCacheMode::WholeFile,
                  ps2x::ResourceDisc::kDefaultCacheBytes, fixture, false, false);
        runPolicy("whole-file, cap forces LRU eviction", ps2x::ResourceCacheMode::WholeFile, 100000,
                  fixture, true, false);
        runPolicy("whole-file, cap smaller than the large sources", ps2x::ResourceCacheMode::WholeFile, 50000,
                  fixture, false, true);

        // Prefetch: a fresh manifest name, so open() performs the prefetch.
        const Fixture prefetchFixture = buildFixture(root / "prefetch", "prefetch.bin");
        ps2x::ResourceDisc::setCacheMode(ps2x::ResourceCacheMode::WholeFilePrefetch, 1000000);
        auto prefetched = ps2x::ResourceDisc::open(prefetchFixture.manifestPath);
        require(bool(prefetched), "prefetch manifest not recognized");
        const auto afterOpen = ps2x::ResourceDisc::cacheStats();
        // Four distinct sources: rawA, rawB, rawC and blocks.bin.
        require(afterOpen.files == 4 && afterOpen.loads == 4, "prefetch did not load each listed source once");
        checkAllRanges(*prefetched, prefetchFixture, 0x2222, 500);
        require(ps2x::ResourceDisc::cacheStats().loads == 4, "prefetched reads reloaded files");
        std::cout << "PASS: prefetch loads each listed source once and serves reads from memory\n";

        checkDecodeInvalidation(root);
        ps2x::ResourceDisc::setCacheMode(ps2x::ResourceCacheMode::WholeFile, 100000);
        checkConcurrentReaders(fixture);

        // Back to the default state.
        ps2x::ResourceDisc::setCacheMode(ps2x::ResourceCacheMode::Off);
        require(ps2x::ResourceDisc::cacheMode() == ps2x::ResourceCacheMode::Off, "mode not restored");
        std::filesystem::remove_all(root);
    } catch (const std::exception& error) {
        std::cerr << "resource_cache_checks: " << error.what() << '\n';
        return 1;
    }
    std::cout << "resource_cache_checks passed\n";
    return 0;
}
