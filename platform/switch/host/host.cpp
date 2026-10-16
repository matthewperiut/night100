#include "night100_switch_host.h"
#include "pcm_converter.h"
#include "switch_lifecycle.h"
#include "input_chords.h"
#include "raylib.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <png.h>
#include <stdexcept>
#include <switch.h>
#include <thread>
#include <vector>
namespace {
PadState pad;
std::atomic<uint64_t> buttons{0};
std::atomic<bool> padConnected{false};
std::array<std::atomic<float>, 4> axes{};
bool initialized = false;
SwitchLifecycle::State lifecycle;
SwitchInput::ReplayControls replayControls;
SwitchInput::ReplayUpdate replayInput;
size_t replayCount=0;
std::atomic<bool> metricsEnabled{false};
AppletHookCookie lifecycleHook{};
std::atomic<uint64_t> producerParked{0},audioParked{0};
bool suspensionEnabled=false;
std::atomic<Result> lifecycleFailure{0};
std::atomic<bool> exitChordRequested{false};
std::atomic<bool> resolutionChordRequested{false};
bool resolutionChordHeld=false;
std::atomic<bool> dockedMode{false};
uint64_t exitChordSince=0;
bool exitChordFired=false;
void (*producerQuiesce)(void*)=nullptr;
void* producerQuiesceData=nullptr;
void (*failureReporter)(const char*,void*)=nullptr;
void* failureReporterData=nullptr;
uint64_t lifecycleNow() {return armTicksToNs(armGetSystemTick());}
void (*focusCallback)(bool,void*)=nullptr;
void* focusCallbackData=nullptr;
void focusChanged(AppletHookType type,void*) {
  if(type!=AppletHookType_OnFocusState && type!=AppletHookType_OnResume)return;
  const bool active=appletGetFocusState()==AppletFocusState_InFocus;
  if(active) {
    const Result result=appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);
    if(R_FAILED(result)){lifecycleFailure.store(result,std::memory_order_release);return;}
    suspensionEnabled=false;
  }
  lifecycle.setFocus(lifecycleNow(),active);
  night100SwitchLifecyclePending.store(lifecycle.needsService(),std::memory_order_release);
  // Runs on the applet hook thread; the callee serializes its own state.
  if(auto* callback=focusCallback)callback(active,focusCallbackData);
}
struct Stream {
  AudioCallback callback = nullptr;
  bool playing = false;
  unsigned rate = 48000, channels = 2;
  std::unique_ptr<SwitchPcm::Converter> converter;
};
struct Sample {
  std::vector<int16_t> pcm;
  unsigned rate = 48000;
  double frame = 0;
  float pitch = 1, volume = 1;
  bool playing = false;
};
std::mutex audioMutex;
std::vector<std::unique_ptr<Stream>> streams;
std::vector<std::unique_ptr<Sample>> samples;
std::atomic<bool> audioReady{false}, audioRunning{false};
std::atomic<Result> audioFailure{0};
std::thread audioThread;
std::atomic<uint32_t> audioThreadHandle{0};
constexpr unsigned outputFrames = 1024;
struct OutputBuffer {
  AudioOutBuffer descriptor{};
  void *memory = nullptr;
};
std::array<OutputBuffer, 2> outputs;
void check(Result rc, const char *action) {
  if (R_FAILED(rc))
    throw std::runtime_error(std::string(action) +
                             " failed: " + std::to_string(rc));
}
void mix(int16_t *destination) {
  std::array<int16_t, outputFrames * 2> temporary{};
  std::array<int32_t, outputFrames * 2> accumulator{};
  std::lock_guard lock(audioMutex);
  for (auto &stream : streams)
    if (stream->playing && stream->callback) {
      if (stream->rate == 48000 && stream->channels == 2) {
        temporary.fill(0);
        stream->callback(temporary.data(), outputFrames);
        for (size_t i = 0; i < temporary.size(); ++i)
          accumulator[i] += temporary[i];
      } else {
        stream->converter->addTo(accumulator.data(), outputFrames,
                                 stream->callback);
      }
    }
  for (auto &sample : samples)
    if (sample->playing) {
      const size_t count = sample->pcm.size() / 2;
      for (unsigned frame = 0; frame < outputFrames; ++frame) {
        const size_t cursor = size_t(sample->frame);
        if (cursor >= count) {
          sample->playing = false;
          break;
        }
        for (unsigned channel = 0; channel < 2; ++channel)
          accumulator[frame * 2 + channel] +=
              int32_t(sample->pcm[cursor * 2 + channel] * sample->volume);
        sample->frame += double(sample->rate) / 48000.0 * sample->pitch;
      }
    }
  for (size_t i = 0; i < accumulator.size(); ++i)
    destination[i] = int16_t(std::clamp(accumulator[i], -32768, 32767));
}
void outputWorker() {
  if(const auto* self=threadGetSelf())audioThreadHandle.store(self->handle,std::memory_order_release);
  for (auto &output : outputs) {
    mix(static_cast<int16_t *>(output.memory));
    armDCacheFlush(output.memory, output.descriptor.data_size);
    const Result rc = audoutAppendAudioOutBuffer(&output.descriptor);
    if (R_FAILED(rc)) {
      audioFailure = rc;
      audioRunning = false;
      return;
    }
  }
  while (audioRunning.load(std::memory_order_acquire)) {
    if(!lifecycle.active()) {
      const Result stopped=audoutStopAudioOut();
      if(R_FAILED(stopped)){audioFailure=stopped;audioRunning=false;break;}
      while(audioRunning.load(std::memory_order_acquire)) {
        const auto generation=lifecycle.inactiveGeneration();if(!generation)break;
        audioParked.store(generation,std::memory_order_release);
        while(audioRunning.load(std::memory_order_acquire) && lifecycle.inactiveGeneration()==generation)svcSleepThread(1000000);
        audioParked.store(0,std::memory_order_release);
      }
      if(!audioRunning.load(std::memory_order_acquire))break;
      const Result started=audoutStartAudioOut();
      if(R_FAILED(started)){audioFailure=started;audioRunning=false;break;}
    }
    AudioOutBuffer *released = nullptr;
    uint32_t count = 0;
    const Result rc = audoutWaitPlayFinish(&released, &count, 100000000);
    if (!audioRunning.load(std::memory_order_acquire))
      break;
    if (R_FAILED(rc)) {
      if (rc == KERNELRESULT(TimedOut))
        continue;
      audioFailure = rc;
      audioRunning = false;
      break;
    }
    while (released) {
      auto *next = released->next;
      mix(static_cast<int16_t *>(released->buffer));
      armDCacheFlush(released->buffer, released->data_size);
      released->next = nullptr;
      const Result append = audoutAppendAudioOutBuffer(released);
      if (R_FAILED(append)) {
        audioFailure = append;
        audioRunning = false;
        break;
      }
      released = next;
    }
  }
}
uint16_t readLe16(const unsigned char *p) {
  return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
uint32_t readLe32(const unsigned char *p) {
  return uint32_t(readLe16(p)) | uint32_t(readLe16(p + 2)) << 16;
}
} // namespace
void night100SwitchHostInitialize() {
  if (initialized)
    return;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);
  check(appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend),"Set cooperative focus handling");
  check(appletSetRestartMessageEnabled(true),"Enable resume notification");
  lifecycle.reset(lifecycleNow(),appletGetFocusState()==AppletFocusState_InFocus);
  night100SwitchLifecyclePending.store(lifecycle.needsService(),std::memory_order_release);
  producerParked=0;audioParked=0;suspensionEnabled=false;
  lifecycleFailure=0;
  replayControls={};replayInput={};replayCount=0;metricsEnabled=false;
  appletHook(&lifecycleHook,focusChanged,nullptr);
  initialized = true;
}
bool night100SwitchHostPoll() {
  check(audioFailure.load(std::memory_order_relaxed), "Switch audio output");
  if (!appletMainLoop()) {
    lifecycle.setFocus(lifecycleNow(),true);
    night100SwitchLifecyclePending.store(lifecycle.needsService(),std::memory_order_release);
    return false;
  }
  check(lifecycleFailure.load(std::memory_order_acquire),"Resume cooperative focus handling");
  if(!lifecycle.active()) {
    exitChordSince=0;exitChordFired=false;
    replayInput=replayControls.update({},lifecycleNow(),replayCount,false);
    buttons=0;padConnected=false;for(auto& axis:axes)axis=0;
    // Hand control back to Horizon only after both owners have stopped using
    // their services. Hooks themselves never wait for GPU/audio locks.
    const auto generation=lifecycle.inactiveGeneration();
    if(generation && !suspensionEnabled && producerParked.load(std::memory_order_acquire)==generation &&
       (!audioReady.load(std::memory_order_acquire) || audioParked.load(std::memory_order_acquire)==generation)) {
      suspensionEnabled=true;
      check(appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleepNotify),"Suspend quiesced application");
    }
    return true;
  }
  padUpdate(&pad);
  padConnected = padIsConnected(&pad);
  const auto rawButtons=padGetButtons(&pad);
  constexpr uint64_t bumpers=HidNpadButton_L|HidNpadButton_R;
  constexpr uint64_t directions=HidNpadButton_Up|HidNpadButton_Down|HidNpadButton_Left|HidNpadButton_Right;
  // L+R+Plus is the exit chord: while Plus is held, the bumpers are invisible to
  // the replay chords (picker, recording, metrics), so they cannot fire.
  const bool plusHeld=(rawButtons&HidNpadButton_Plus)!=0;
  auto held=[&](uint64_t mask){return (rawButtons&mask)!=0 && !(plusHeld && (mask&(HidNpadButton_L|HidNpadButton_R)));};
  const uint64_t now=lifecycleNow();
  // L+R+X cycles the internal resolution (edge-triggered on X while both bumpers are held).
  const bool xHeld=(rawButtons&HidNpadButton_X)!=0;
  if(xHeld && (rawButtons&bumpers)==bumpers) {
    if(!resolutionChordHeld){resolutionChordHeld=true;resolutionChordRequested=true;}
  } else resolutionChordHeld=false;
  dockedMode.store(appletGetOperationMode()==AppletOperationMode_Console,std::memory_order_relaxed);
  if(plusHeld && (rawButtons&bumpers)==bumpers) {
    if(!exitChordSince)exitChordSince=now;
    else if(!exitChordFired && now-exitChordSince>=1000000000ull) {exitChordFired=true;exitChordRequested=true;}
  } else {exitChordSince=0;exitChordFired=false;}
  replayInput=replayControls.update({held(HidNpadButton_L),held(HidNpadButton_R),
      held(HidNpadButton_Up),held(HidNpadButton_Down),held(HidNpadButton_Left),held(HidNpadButton_Right),
      held(HidNpadButton_B),held(HidNpadButton_A),
      bool(rawButtons&~(bumpers|directions|HidNpadButton_A|HidNpadButton_B))},
      lifecycleNow(),replayCount,padIsConnected(&pad));
  buttons=replayInput.consumeAll?0:replayInput.consumeChord?rawButtons&~(bumpers|directions):rawButtons;
  // Keep the exit chord from also opening the game's Start menu.
  if((rawButtons&bumpers)==bumpers)buttons&=~uint64_t(HidNpadButton_Plus);
  if(replayInput.toggleMetrics)
    metricsEnabled.store(!metricsEnabled.load(std::memory_order_relaxed),std::memory_order_release);
  for (unsigned side = 0; side < 2; ++side) {
    const auto stick = padGetStickPos(&pad, side);
    axes[side * 2] = replayInput.consumeAll?0:std::clamp(float(stick.x) / 32767.0f, -1.0f, 1.0f);
    axes[side * 2 + 1] = replayInput.consumeAll?0:std::clamp(-float(stick.y) / 32767.0f, -1.0f, 1.0f);
  }
  return true;
}
void night100SwitchHostShutdown() {
  CloseAudioDevice();
  if(initialized) {
    appletUnhook(&lifecycleHook);
    appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleep);
  }
  producerQuiesce=nullptr;producerQuiesceData=nullptr;
  failureReporter=nullptr;failureReporterData=nullptr;
  initialized = false;
}
bool night100SwitchHostActive(){return lifecycle.active();}
void night100SwitchHostSetFocusCallback(void (*callback)(bool,void*),void* data) {
  focusCallback=callback;focusCallbackData=data;
}
bool night100SwitchHostTakeExitChord(){return exitChordRequested.exchange(false);}
bool night100SwitchHostTakeResolutionChord(){return resolutionChordRequested.exchange(false);}
bool night100SwitchHostDocked(){return dockedMode.load(std::memory_order_relaxed);}
bool night100SwitchHostMetricsEnabled(){return metricsEnabled.load(std::memory_order_acquire);}
SwitchInput::ReplayUpdate night100SwitchHostReplayInput(){const auto result=replayInput;replayInput.action=SwitchInput::ReplayAction::None;return result;}
void night100SwitchHostSetReplayCatalogCount(size_t count,bool newest){replayCount=count;if(newest)replayControls.selectNewest(count);}
void night100SwitchHostCloseReplayPicker(){replayControls.closePicker();}
uint32_t night100SwitchHostAudioThreadHandle(){return audioThreadHandle.load(std::memory_order_acquire);}
void night100SwitchHostBeginExecution(){lifecycle.prime(lifecycleNow());}
void night100SwitchHostSetFailureReporter(void (*callback)(const char*,void*),void* data) {
  failureReporter=callback;failureReporterData=data;
}
void night100SwitchHostReportFailure(const char* message) noexcept {
  try{if(failureReporter)failureReporter(message,failureReporterData);}catch(...){}
}
uint64_t night100SwitchHostInactiveNanoseconds(){return lifecycle.inactiveNanoseconds(lifecycleNow());}
void night100SwitchHostSetProducerQuiesce(void (*callback)(void*),void* data) {
  producerQuiesce=callback;producerQuiesceData=data;
}
std::atomic<bool> night100SwitchLifecyclePending{false};
#if defined(NIGHT100_LIFECYCLE_CALL_COUNT)
std::atomic<uint64_t> night100SwitchLifecycleCalls{0};
#endif
uint64_t night100SwitchHostLifecycleCallCount() {
#if defined(NIGHT100_LIFECYCLE_CALL_COUNT)
    return night100SwitchLifecycleCalls.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}
// Reached only when the inline header check saw a pending transition.
uint64_t night100SwitchHostServiceLifecycleSlow(bool (*stop)(void*),void* data) {
  if(!lifecycle.needsService())return 0;
  if(!lifecycle.active()) {
    if(producerQuiesce)producerQuiesce(producerQuiesceData);
    for(;;) {
      const auto generation=lifecycle.inactiveGeneration();if(!generation || (stop && stop(data)))break;
      producerParked.store(generation,std::memory_order_release);
      while(lifecycle.inactiveGeneration()==generation && !(stop && stop(data)))svcSleepThread(1000000);
      producerParked.store(0,std::memory_order_release);
    }
  }
  const uint64_t result=lifecycle.consume();
  night100SwitchLifecyclePending.store(lifecycle.needsService(),std::memory_order_release);
  return result;
}
bool IsWindowReady() { return initialized; }
bool IsGamepadAvailable(int index) {
  return initialized && padConnected.load(std::memory_order_relaxed) &&
         index == 0;
}
bool IsGamepadButtonDown(int index, int button) {
  if (!IsGamepadAvailable(index))
    return false;
  static constexpr uint64_t mapping[] = {0,
                                         HidNpadButton_Up,
                                         HidNpadButton_Right,
                                         HidNpadButton_Down,
                                         HidNpadButton_Left,
                                         HidNpadButton_X,
                                         HidNpadButton_A,
                                         HidNpadButton_B,
                                         HidNpadButton_Y,
                                         HidNpadButton_L,
                                         HidNpadButton_ZL,
                                         HidNpadButton_R,
                                         HidNpadButton_ZR,
                                         HidNpadButton_Minus,
                                         0,
                                         HidNpadButton_Plus,
                                         HidNpadButton_StickL,
                                         HidNpadButton_StickR};
  return button >= 0 && unsigned(button) < std::size(mapping) &&
         (buttons.load(std::memory_order_relaxed) & mapping[button]);
}
float GetGamepadAxisMovement(int index, int axis) {
  if (!IsGamepadAvailable(index))
    return 0;
  return axis >= 0 && axis < 4 ? axes[axis].load(std::memory_order_relaxed) : 0;
}
// This front end exposes the native controller; no keyboard device is opened.
bool IsKeyDown(int) { return false; }
Image LoadImage(const char *path) {
  png_image decoded{};
  decoded.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_file(&decoded, path))
    return {};
  decoded.format = PNG_FORMAT_RGBA;
  void *pixels = malloc(PNG_IMAGE_SIZE(decoded));
  if (!pixels ||
      !png_image_finish_read(&decoded, nullptr, pixels, 0, nullptr)) {
    free(pixels);
    png_image_free(&decoded);
    return {};
  }
  Image result{pixels, int(decoded.width), int(decoded.height), 1,
               PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
  png_image_free(&decoded);
  return result;
}
void ImageFormat(Image *image, int format) {
  if (image && image->data && format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
    throw std::runtime_error("Unsupported Switch image conversion");
}
void UnloadImage(Image image) { free(image.data); }
void InitAudioDevice() {
  if (audioReady)
    return;
  audioFailure = 0;
  check(audoutInitialize(), "audoutInitialize");
  if (audoutGetSampleRate() != 48000 || audoutGetChannelCount() != 2 ||
      audoutGetPcmFormat() != PcmFormat_Int16) {
    audoutExit();
    throw std::runtime_error(
        "Switch audio output requires 48 kHz stereo PCM16");
  }
  for (auto &output : outputs) {
    output.memory = memalign(0x1000, outputFrames * 4);
    if (!output.memory)
      throw std::bad_alloc();
    output.descriptor.buffer = output.memory;
    output.descriptor.buffer_size = outputFrames * 4;
    output.descriptor.data_size = outputFrames * 4;
  }
  check(audoutStartAudioOut(), "audoutStartAudioOut");
  audioRunning = true;
  audioReady = true;
  audioThread = std::thread(outputWorker);
}
void CloseAudioDevice() {
  if (!audioReady.exchange(false))
    return;
  audioRunning = false;
  audoutStopAudioOut();
  if (audioThread.joinable())
    audioThread.join();
  audioThreadHandle=0;
  audoutExit();
  for (auto &output : outputs) {
    free(output.memory);
    output = {};
  }
  std::lock_guard lock(audioMutex);
  streams.clear();
  samples.clear();
}
bool IsAudioDeviceReady() { return audioReady; }
void SetAudioStreamBufferSizeDefault(int frames) {
  if (frames != 0 && frames != int(outputFrames))
    throw std::runtime_error("Switch audio uses 1024-frame hardware buffers");
}
AudioStream LoadAudioStream(unsigned rate, unsigned bits, unsigned channels) {
  if (rate < 8000 || rate > 192000 || bits != 16 ||
      (channels != 1 && channels != 2))
    throw std::runtime_error("Unsupported Switch PCM stream layout");
  std::lock_guard lock(audioMutex);
  auto stream = std::make_unique<Stream>();
  stream->rate = rate;
  stream->channels = channels;
  stream->converter =
      std::make_unique<SwitchPcm::Converter>(rate, channels, outputFrames);
  auto *pointer = stream.get();
  streams.push_back(std::move(stream));
  return {pointer, nullptr, rate, bits, channels};
}
void SetAudioStreamCallback(AudioStream stream, AudioCallback callback) {
  std::lock_guard lock(audioMutex);
  if (stream.buffer)
    static_cast<Stream *>(stream.buffer)->callback = callback;
}
void PlayAudioStream(AudioStream stream) {
  std::lock_guard lock(audioMutex);
  if (stream.buffer)
    static_cast<Stream *>(stream.buffer)->playing = true;
}
void StopAudioStream(AudioStream stream) {
  std::lock_guard lock(audioMutex);
  if (stream.buffer)
    static_cast<Stream *>(stream.buffer)->playing = false;
}
void UnloadAudioStream(AudioStream stream) {
  std::lock_guard lock(audioMutex);
  std::erase_if(streams,
                [&](const auto &item) { return item.get() == stream.buffer; });
}
Wave LoadWaveFromMemory(const char *, const unsigned char *bytes, int size) {
  if (size < 12 || std::memcmp(bytes, "RIFF", 4) ||
      std::memcmp(bytes + 8, "WAVE", 4))
    return {};
  unsigned channels = 0, rate = 0, bits = 0;
  const unsigned char *pcm = nullptr;
  size_t length = 0;
  for (size_t offset = 12; offset + 8 <= size;) {
    const uint32_t chunk = readLe32(bytes + offset + 4);
    if (chunk > size - offset - 8)
      return {};
    const auto *data = bytes + offset + 8;
    if (!std::memcmp(bytes + offset, "fmt ", 4) && chunk >= 16) {
      if (readLe16(data) != 1)
        return {};
      channels = readLe16(data + 2);
      rate = readLe32(data + 4);
      bits = readLe16(data + 14);
    }
    if (!std::memcmp(bytes + offset, "data", 4)) {
      pcm = data;
      length = chunk;
    }
    offset += 8 + chunk + (chunk & 1);
  }
  if (!pcm || !rate || bits != 16 || (channels != 1 && channels != 2))
    return {};
  void *copy = malloc(length);
  if (!copy)
    return {};
  std::memcpy(copy, pcm, length);
  return {unsigned(length / (channels * 2)), rate, bits, channels, copy};
}
void UnloadWave(Wave wave) { free(wave.data); }
Sound LoadSoundFromWave(Wave wave) {
  if (!wave.data || wave.sampleSize != 16 ||
      (wave.channels != 1 && wave.channels != 2))
    return {};
  auto sample = std::make_unique<Sample>();
  sample->rate = wave.sampleRate;
  sample->pcm.resize(size_t(wave.frameCount) * 2);
  const auto *source = static_cast<const int16_t *>(wave.data);
  for (unsigned frame = 0; frame < wave.frameCount; ++frame)
    for (unsigned channel = 0; channel < 2; ++channel)
      sample->pcm[frame * 2 + channel] =
          source[frame * wave.channels + (wave.channels == 1 ? 0 : channel)];
  auto *pointer = sample.get();
  std::lock_guard lock(audioMutex);
  samples.push_back(std::move(sample));
  return {{pointer, nullptr, wave.sampleRate, 16, 2}, wave.frameCount};
}
void UnloadSound(Sound sound) {
  std::lock_guard lock(audioMutex);
  std::erase_if(samples, [&](const auto &item) {
    return item.get() == sound.stream.buffer;
  });
}
bool IsSoundPlaying(Sound sound) {
  std::lock_guard lock(audioMutex);
  return sound.stream.buffer &&
         static_cast<Sample *>(sound.stream.buffer)->playing;
}
void SetSoundPitch(Sound sound, float pitch) {
  std::lock_guard lock(audioMutex);
  if (sound.stream.buffer)
    static_cast<Sample *>(sound.stream.buffer)->pitch =
        std::clamp(pitch, .01f, 16.0f);
}
void SetSoundVolume(Sound sound, float volume) {
  std::lock_guard lock(audioMutex);
  if (sound.stream.buffer)
    static_cast<Sample *>(sound.stream.buffer)->volume =
        std::clamp(volume, 0.0f, 1.0f);
}
void PlaySound(Sound sound) {
  std::lock_guard lock(audioMutex);
  if (sound.stream.buffer) {
    auto *sample = static_cast<Sample *>(sound.stream.buffer);
    sample->frame = 0;
    sample->playing = true;
  }
}
void StopSound(Sound sound) {
  std::lock_guard lock(audioMutex);
  if (sound.stream.buffer)
    static_cast<Sample *>(sound.stream.buffer)->playing = false;
}
