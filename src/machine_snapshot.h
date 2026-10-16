#pragma once
#include <cstdint>
#include <filesystem>
class PS2Runtime;
void saveMachineSnapshot(PS2Runtime&,const std::filesystem::path&);
void loadMachineSnapshot(PS2Runtime&,const std::filesystem::path&);
uint64_t nativePortClockSnapshot();
void nativePortClockRestore(uint64_t);
void pauseNativePortClock();
void resumeNativePortClock();
bool nativeSceneCanSnapshot();
void resetNativeSceneAfterSnapshot();
