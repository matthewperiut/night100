#include "controller_prompts.h"
#include <raylib.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
unsigned checks = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
struct TemporaryDirectory {
    fs::path path = fs::temp_directory_path() / ("night100-prompts-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryDirectory() { fs::create_directories(path / "sw"); }
    ~TemporaryDirectory() { std::error_code ignored; fs::remove_all(path, ignored); }
};
Bytes pixels(unsigned width, unsigned height, unsigned seed) {
    Bytes result(width * height * 4);
    for (size_t i = 0; i < result.size(); ++i)
        result[i] = uint8_t(seed + i * 37 + (i / 4) * 11);
    return result;
}
void png(const fs::path& path, unsigned width, unsigned height, Bytes& bytes) {
    Image image{bytes.data(), int(width), int(height), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    require(ExportImage(image, path.string().c_str()), "write synthetic PNG: " + path.string());
}
std::string row(uint64_t hash, unsigned width, unsigned height, unsigned x, unsigned y,
                unsigned patchWidth, unsigned patchHeight, const std::string& image) {
    std::ostringstream out;
    out << std::hex << hash << std::dec << ' ' << width << ' ' << height << ' '
        << x << ' ' << y << ' ' << patchWidth << ' ' << patchHeight
        << " original.png " << image << '\n';
    return out.str();
}
void map(const fs::path& directory, const std::string& text) {
    std::ofstream file(directory / "prompts.map", std::ios::binary);
    file << text;
    require(bool(file), "write synthetic map");
}
void replace(Bytes& atlas, unsigned atlasWidth, unsigned x, unsigned y,
             unsigned width, unsigned height, const Bytes& patch) {
    for (unsigned py = 0; py < height; ++py)
        for (unsigned px = 0; px < width; ++px)
            for (unsigned c = 0; c < 4; ++c)
                atlas[((y + py) * atlasWidth + x + px) * 4 + c] =
                    patch[(py * width + px) * 4 + c];
}
void unchanged(unsigned width, unsigned height, const Bytes& source, const std::string& label) {
    auto result = source;
    require(!ControllerPrompts::apply(width, height, result), label + ": no match");
    require(result == source, label + ": all input bytes preserved");
}
}

int main() {
    try {
        SetTraceLogLevel(LOG_WARNING);
        TemporaryDirectory temporary;
        const auto directory = temporary.path;
        const Bytes atlas = pixels(8, 6, 9);
        const Bytes singleAtlas = pixels(6, 4, 81);
        const uint64_t hash = ControllerPrompts::fingerprint(atlas);
        const uint64_t singleHash = ControllerPrompts::fingerprint(singleAtlas);
        const std::string header = "NIGHT100_PROMPTS 1\n";

        const Bytes hello{'h', 'e', 'l', 'l', 'o'};
        require(ControllerPrompts::fingerprint(hello) == 0xa430d84680aabd0bull,
                "FNV-1a known hello oracle");
        require(ControllerPrompts::fingerprint({}) == 0xcbf29ce484222325ull,
                "FNV-1a empty oracle");
        require(std::string(ControllerPrompts::profile()) == ControllerPrompts::defaultProfile(),
                "initial profile follows compiled platform default");
        require(std::string(ControllerPrompts::defaultProfile()) == "ps" ||
                std::string(ControllerPrompts::defaultProfile()) == "sw", "valid compiled default");
        ControllerPrompts::configure("ps", directory / "does-not-exist");
        require(std::string(ControllerPrompts::profile()) == "ps", "PS needs no asset directory");
        unchanged(8, 6, atlas, "PS original atlas");

        Bytes replacements[4] = {pixels(2, 1, 41), pixels(2, 1, 72),
                                 pixels(2, 1, 103), pixels(2, 1, 134)};
        Bytes oneReplacement = pixels(2, 2, 165);
        std::string valid = header + "# Self-authored synthetic atlases and artwork only.\n\n";
        Bytes expected = atlas;
        const unsigned positions[4][2] = {{0, 0}, {6, 0}, {1, 3}, {6, 5}};
        for (unsigned i = 0; i < 4; ++i) {
            const auto name = "button-" + std::to_string(i) + ".png";
            png(directory / "sw" / name, 2, 1, replacements[i]);
            valid += row(hash, 8, 6, positions[i][0], positions[i][1], 2, 1, name);
            replace(expected, 8, positions[i][0], positions[i][1], 2, 1, replacements[i]);
        }
        png(directory / "sw" / "single.png", 2, 2, oneReplacement);
        valid += row(singleHash, 6, 4, 2, 1, 2, 2, "single.png");
        Bytes singleExpected = singleAtlas;
        replace(singleExpected, 6, 2, 1, 2, 2, oneReplacement);
        map(directory, valid);
        ControllerPrompts::configure("sw", directory);
        auto verifyGoodConfiguration = [&] {
            require(std::string(ControllerPrompts::profile()) == "sw", "SW profile retained");
            auto output = atlas;
            require(ControllerPrompts::apply(8, 6, output), "four-button atlas matched");
            require(output == expected, "four exact RGBA patches; every other byte preserved");
            output = singleAtlas;
            require(ControllerPrompts::apply(6, 4, output), "one-button atlas matched");
            require(output == singleExpected, "single exact RGBA patch; every other byte preserved");
        };
        verifyGoodConfiguration();
        auto alphaChanged = atlas;
        alphaChanged[3] ^= 1;
        unchanged(8, 6, alphaChanged, "alpha byte participates in full-atlas fingerprint");
        auto backgroundChanged = atlas;
        backgroundChanged[(2 * 8 + 4) * 4] ^= 1;
        unchanged(8, 6, backgroundChanged, "nonbutton byte participates in fingerprint");
        unchanged(6, 8, atlas, "same bytes and area with wrong dimensions");
        unchanged(8, 5, atlas, "inconsistent RGBA length");
        unchanged(8, 6, Bytes{}, "empty RGBA buffer");
        unchanged(8, 6, expected, "already patched image has no original fingerprint");

        auto reject = [&](const std::string& label, const std::string& contents) {
            map(directory, contents);
            bool rejected = false;
            try { ControllerPrompts::configure("sw", directory); }
            catch (const std::exception&) { rejected = true; }
            require(rejected, label + " rejected");
            verifyGoodConfiguration();
        };
        const auto goodFirst = row(hash, 8, 6, 0, 0, 2, 1, "button-0.png");
        reject("empty map", header);
        reject("wrong map magic", "OTHER 1\n" + goodFirst);
        reject("unsupported version", "NIGHT100_PROMPTS 2\n" + goodFirst);
        reject("trailing header fields", "NIGHT100_PROMPTS 1 extra\n" + goodFirst);
        reject("malformed row", header + goodFirst + "incomplete\n");
        reject("trailing row fields", header + goodFirst.substr(0, goodFirst.size() - 1) + " extra\n");
        reject("malformed hash", header + "nothex 8 6 0 0 2 1 original.png button-0.png\n");
        reject("zero dimensions", header + row(hash, 0, 6, 0, 0, 2, 1, "button-0.png"));
        reject("oversized dimensions", header + row(hash, 2049, 6, 0, 0, 2, 1, "button-0.png"));
        reject("zero patch", header + row(hash, 8, 6, 0, 0, 0, 1, "button-0.png"));
        reject("out-of-bounds patch", header + goodFirst + row(hash, 8, 6, 7, 5, 2, 1, "button-0.png"));
        reject("overflowing coordinates", header + row(hash, 8, 6, 0xffffffffu, 0, 2, 1, "button-0.png"));
        reject("overlapping patches", header + goodFirst + row(hash, 8, 6, 1, 0, 2, 1, "button-1.png"));
        reject("missing replacement", header + goodFirst + row(hash, 8, 6, 3, 0, 2, 1, "missing.png"));
        reject("wrong PNG dimensions", header + goodFirst + row(hash, 8, 6, 3, 0, 2, 1, "single.png"));
        reject("path traversal", header + row(hash, 8, 6, 0, 0, 2, 1, "../button-0.png"));
        { std::ofstream corrupt(directory / "sw" / "corrupt.png"); corrupt << "not a PNG"; }
        reject("corrupt PNG", header + goodFirst + row(hash, 8, 6, 3, 0, 2, 1, "corrupt.png"));

        bool rejected = false;
        try { ControllerPrompts::configure("xbox", directory); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "invalid profile rejected");
        verifyGoodConfiguration();
        rejected = false;
        try { ControllerPrompts::configure("sw", directory / "missing"); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "missing map rejected");
        verifyGoodConfiguration();

        // Asset files need not remain readable after the atomic configuration load.
        fs::remove_all(directory / "sw");
        verifyGoodConfiguration();
        ControllerPrompts::configure("ps", directory / "missing");
        require(std::string(ControllerPrompts::profile()) == "ps", "switch back to PS");
        unchanged(8, 6, atlas, "PS clears previous SW replacements");
        std::cout << "Controller prompt checks passed (" << checks << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Controller prompt check failed: " << error.what() << '\n';
        return 1;
    }
}
