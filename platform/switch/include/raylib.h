#pragma once
// Source compatibility for the runtime's host input/audio/image consumers only.
// Implemented with libnx and libpng; no desktop Raylib/GLFW/OpenGL is linked.
#include <cstdint>
struct Color {
  unsigned char r, g, b, a;
};
struct Image {
  void *data;
  int width, height, mipmaps, format;
};
struct Wave {
  unsigned frameCount, sampleRate, sampleSize, channels;
  void *data;
};
struct AudioStream {
  void *buffer;
  void *processor;
  unsigned sampleRate, sampleSize, channels;
};
struct Sound {
  AudioStream stream;
  unsigned frameCount;
};
using AudioCallback = void (*)(void *, unsigned);
enum { PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 = 7 };
enum {
  GAMEPAD_BUTTON_UNKNOWN = 0,
  GAMEPAD_BUTTON_LEFT_FACE_UP,
  GAMEPAD_BUTTON_LEFT_FACE_RIGHT,
  GAMEPAD_BUTTON_LEFT_FACE_DOWN,
  GAMEPAD_BUTTON_LEFT_FACE_LEFT,
  GAMEPAD_BUTTON_RIGHT_FACE_UP,
  GAMEPAD_BUTTON_RIGHT_FACE_RIGHT,
  GAMEPAD_BUTTON_RIGHT_FACE_DOWN,
  GAMEPAD_BUTTON_RIGHT_FACE_LEFT,
  GAMEPAD_BUTTON_LEFT_TRIGGER_1,
  GAMEPAD_BUTTON_LEFT_TRIGGER_2,
  GAMEPAD_BUTTON_RIGHT_TRIGGER_1,
  GAMEPAD_BUTTON_RIGHT_TRIGGER_2,
  GAMEPAD_BUTTON_MIDDLE_LEFT,
  GAMEPAD_BUTTON_MIDDLE,
  GAMEPAD_BUTTON_MIDDLE_RIGHT,
  GAMEPAD_BUTTON_LEFT_THUMB,
  GAMEPAD_BUTTON_RIGHT_THUMB
};
enum {
  GAMEPAD_AXIS_LEFT_X,
  GAMEPAD_AXIS_LEFT_Y,
  GAMEPAD_AXIS_RIGHT_X,
  GAMEPAD_AXIS_RIGHT_Y,
  GAMEPAD_AXIS_LEFT_TRIGGER,
  GAMEPAD_AXIS_RIGHT_TRIGGER
};
enum {
  KEY_NULL = 0,
  KEY_SPACE = 32,
  KEY_A = 65,
  KEY_C = 67,
  KEY_D = 68,
  KEY_E = 69,
  KEY_I = 73,
  KEY_J = 74,
  KEY_K = 75,
  KEY_L = 76,
  KEY_Q = 81,
  KEY_S = 83,
  KEY_V = 86,
  KEY_W = 87,
  KEY_X = 88,
  KEY_Z = 90,
  KEY_ONE = 49,
  KEY_THREE = 51,
  KEY_ESCAPE = 256,
  KEY_ENTER = 257,
  KEY_TAB = 258,
  KEY_RIGHT = 262,
  KEY_LEFT = 263,
  KEY_DOWN = 264,
  KEY_UP = 265,
  KEY_LEFT_CONTROL = 341,
  KEY_RIGHT_SHIFT = 344,
  KEY_RIGHT_CONTROL = 345,
  KEY_KP_0 = 320,
  KEY_KP_1 = 321
};
bool IsWindowReady();
bool IsGamepadAvailable(int);
bool IsGamepadButtonDown(int, int);
float GetGamepadAxisMovement(int, int);
bool IsKeyDown(int);
Image LoadImage(const char *);
void ImageFormat(Image *, int);
void UnloadImage(Image);
void InitAudioDevice();
void CloseAudioDevice();
bool IsAudioDeviceReady();
void SetAudioStreamBufferSizeDefault(int);
AudioStream LoadAudioStream(unsigned, unsigned, unsigned);
void SetAudioStreamCallback(AudioStream, AudioCallback);
void PlayAudioStream(AudioStream);
void StopAudioStream(AudioStream);
void UnloadAudioStream(AudioStream);
Wave LoadWaveFromMemory(const char *, const unsigned char *, int);
void UnloadWave(Wave);
Sound LoadSoundFromWave(Wave);
void UnloadSound(Sound);
bool IsSoundPlaying(Sound);
void SetSoundPitch(Sound, float);
void SetSoundVolume(Sound, float);
void PlaySound(Sound);
void StopSound(Sound);
