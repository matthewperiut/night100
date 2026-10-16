#pragma once
#include "input_replay.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ReplayCatalog {
struct Checkpoint {
    InputReplay::Context context;
    bool playerValid=false,cameraValid=false;
    std::array<float,3> position{};
    uint32_t health=0,snacks=0,keys=0,control=0;
    // RenderWare view matrix, excluding its four flags/padding words.
    std::array<float,12> camera{};
};
struct Metadata {
    uint64_t id=0,startFrame=0,endFrame=0;
    bool complete=false;
    std::string elf=InputReplay::elfSha256,options;
    Checkpoint start,end;
};
struct Entry {uint64_t id=0;std::filesystem::path directory;};
struct Comparison {bool matches=false;std::string json;};
Entry reserve(const std::filesystem::path& root);
void save(const Entry&,const Metadata&);
Metadata load(const Entry&);
// Interrupted captures consume their number but are not offered for replay.
std::vector<Entry> list(const std::filesystem::path& root);
// Call only inside the exact-ELF game-thread completion hook. This is a semantic
// checkpoint for comparison, not a restorable runtime memory snapshot.
Checkpoint capture(const uint8_t* ram,size_t size);
Comparison compare(const Checkpoint& expected,const Checkpoint& actual,
                   double positionTolerance=0.10,double cameraTolerance=0.02);
}
