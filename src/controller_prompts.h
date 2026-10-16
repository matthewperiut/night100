#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace ControllerPrompts {
// Configure before loading game assets. The runtime bridge is exact-ELF gated.
const char* defaultProfile();
void configure(std::string_view profile,const std::filesystem::path& directory);
const char* profile();
uint64_t fingerprint(std::span<const uint8_t> rgba);
// Only immutable decoded atlases are patched, before their first GPU upload.
bool apply(uint32_t width,uint32_t height,std::span<uint8_t> rgba);
}
