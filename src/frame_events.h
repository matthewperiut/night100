#pragma once
#include <cstdint>
class PS2Runtime;

namespace GameFrameEvents {
using Callback = void(*)(uint8_t*,PS2Runtime*,void*);
// Install before starting the game executor. Events describe drained, completed
// game camera frames, independently of host redraws or the graphics API.
inline Callback callback = nullptr;
inline void* context = nullptr;
inline void configure(Callback value,void* data=nullptr) { callback=value;context=data; }
inline void completed(uint8_t* ram,PS2Runtime* runtime) {
    if(callback)callback(ram,runtime,context);
}
}
