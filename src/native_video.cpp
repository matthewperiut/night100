#include "native_video.h"
#include "movie_audio.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "raylib.h"
#include "rlgl.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

namespace NativeVideo {
namespace {
using Clock = std::chrono::steady_clock;
struct PausedGameClocks {
    PS2Runtime& runtime;
    explicit PausedGameClocks(PS2Runtime& value):runtime(value) {runtime.pauseStateClocks();}
    ~PausedGameClocks() {runtime.resumeStateClocks();}
};
struct Frame {
    std::vector<uint8_t> rgba;
    int width = 0, height = 0;
    double pts = 0, duration = 1.0 / 30, aspect = 4.0 / 3;
};
struct Movie {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Frame> frames;
    MoviePlayback::MovieAudio audio;
    std::string filename, error;
    uint32_t buttons = 0;
    double minimum = 0, firstVideoPts = 0, end = 0;
    bool haveFirstPts = false, started = false, eof = false, done = false, aborted = false, skipped = false;
    bool diagnosticCaptured = false;
    Clock::time_point start;
    uint64_t decoded = 0, displayed = 0, dropped = 0, nonzeroAudio = 0;
    // Only the audio callback changes this cursor after stream startup. Negative
    // positions generate initial silence when the audio PES starts after video.
    std::atomic<int64_t> audioCursor{0};
    std::atomic<uint64_t> callbackSourceFrames{0}, callbackCalls{0};
};
std::mutex currentMutex;
std::shared_ptr<Movie> current;
std::atomic<Movie*> callbackMovie{nullptr};
struct WindowPlayback {
    std::shared_ptr<Movie> movie;
    Texture2D texture{};
    AudioStream audio{};
    bool audioLoaded = false;
    uint32_t previousButtons = 0;
    double aspect = 4.0 / 3;
} window;
struct AttractDiagnostic {
    bool enabled = false;
    unsigned selected = 0, played = 0, menuFrames = 0;
    std::vector<unsigned> sequence;
    std::filesystem::path evidence;
    std::atomic<unsigned> menuCapture{0};
    PS2Runtime::RecompiledFunction originalName = nullptr;
} attract;
void diagnosticMovieName(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    if (GPR_U32(cpu,31)==0x1326b8 && attract.played<attract.sequence.size()) {
        attract.selected=attract.sequence[attract.played++];
        SET_GPR_U32(cpu,4,attract.selected+1);
        attract.menuFrames=0;
        std::cout<<"[attract-test] selecting DEMO"<<attract.selected<<" through original menu call\n"<<std::flush;
    }
    attract.originalName(ram,cpu,runtime);
}
void diagnosticMovie(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    const uint32_t address=GPR_U32(cpu,4);
    if(address<PS2_RAM_SIZE) {
        const std::string name(reinterpret_cast<char*>(ram+address),strnlen(reinterpret_cast<char*>(ram+address),std::min<size_t>(1024,PS2_RAM_SIZE-address)));
        std::string lower=name;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return char(std::tolower(c));});
        if(lower.find("demo") == std::string::npos) {
            std::cout<<"[attract-test] omitting unrelated startup movie "<<name<<'\n'<<std::flush;
            SET_GPR_VEC(cpu,2,_mm_setzero_si128());cpu->pc=GPR_U32(cpu,31);return;
        }
    }
    // A completion audit must reach genuine EOF even if the user interacts
    // with another window during this long, explicitly requested test.
    SET_GPR_U32(cpu,5,0);
    play(ram,cpu,runtime);
}
std::string ffmpegError(int result) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    av_strerror(result, text.data(), text.size()); return text.data();
}
void checked(int result, const char* operation) {
    if (result < 0) throw std::runtime_error(std::string(operation) + ": " + ffmpegError(result));
}
std::filesystem::path moviePath(const std::string& guest) {
    std::string relative = guest;
    std::replace(relative.begin(), relative.end(), '\\', '/');
    if (const auto colon = relative.find(':'); colon != std::string::npos) relative.erase(0, colon + 1);
    while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
    if (const auto version = relative.rfind(';'); version != std::string::npos) relative.erase(version);
    std::filesystem::path path = PS2Runtime::getIoPaths().cdRoot;
    for (const auto& component : std::filesystem::path(relative)) {
        const auto part = component.string();
        if (part.empty() || part == ".") continue;
        if (part == "..") throw std::runtime_error("Movie path escapes disc root");
        auto exact = path / component;
        if (std::filesystem::exists(exact)) { path = std::move(exact); continue; }
        std::string upper = part;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        bool found = false;
        if (std::filesystem::is_directory(path)) for (const auto& entry : std::filesystem::directory_iterator(path)) {
            std::string name = entry.path().filename().string();
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::toupper(c)); });
            if (name == upper) { path = entry.path(); found = true; break; }
        }
        if (!found) throw std::runtime_error("Movie file not found: " + (path / part).string());
    }
    if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("Movie is not a file: " + path.string());
    return path;
}
uint32_t readButtons(PS2Runtime& runtime) {
    std::array<uint8_t, 32> pad{};
    if (!runtime.padBackend().readState(0, 0, pad.data(), pad.size())) return 0;
    const uint16_t down = uint16_t(~(uint16_t(pad[2]) | uint16_t(pad[3]) << 8));
    // Original movie skip masks use xPad's converted bits, rather than the
    // libpad packet layout. Keep keyboard and controller behavior identical.
    constexpr uint32_t converted[] = {2, 0x400, 0x4000, 1, 0x10, 0x20, 0x40, 0x80,
        0x200, 0x2000, 0x100, 0x1000, 0x40000, 0x20000, 0x10000, 0x80000};
    uint32_t result = 0;
    for (unsigned bit = 0; bit != 16; ++bit) if (down & (1u << bit)) result |= converted[bit];
    return result;
}
void audioCallback(void* output, unsigned frames) {
    Movie* movie = callbackMovie.load(std::memory_order_acquire);
    if (!movie) return;
    const auto channels = movie->audio.channels;
    auto* samples = static_cast<int16_t*>(output);
    std::memset(samples, 0, size_t(frames) * channels * sizeof(int16_t));
    const int64_t cursor = movie->audioCursor.load(std::memory_order_relaxed);
    const int64_t total = static_cast<int64_t>(movie->audio.interleaved.size() / channels);
    const int64_t begin = std::max<int64_t>(0, cursor);
    const int64_t end = std::min<int64_t>(total, cursor + frames);
    if (end > begin) {
        std::memcpy(samples + (begin - cursor) * channels,
            movie->audio.interleaved.data() + begin * channels, size_t(end - begin) * channels * sizeof(int16_t));
        movie->callbackSourceFrames.fetch_add(static_cast<uint64_t>(end - begin), std::memory_order_relaxed);
    }
    movie->callbackCalls.fetch_add(1, std::memory_order_relaxed);
    movie->audioCursor.store(cursor + frames, std::memory_order_relaxed);
}
void releaseWindowResources() {
    if (window.audioLoaded) {
        if (IsAudioDeviceReady()) {
            StopAudioStream(window.audio);
            SetAudioStreamCallback(window.audio, nullptr);
            UnloadAudioStream(window.audio);
        }
        callbackMovie.store(nullptr, std::memory_order_release);
        window.audio = {}; window.audioLoaded = false;
    }
    if (window.texture.id) {
        rlDrawRenderBatchActive(); UnloadTexture(window.texture); window.texture = {};
    }
    window.movie.reset();
}
void finish(Movie& movie, bool skipped, bool aborted) {
    movie.skipped = skipped; movie.aborted = aborted; movie.done = true;
    movie.changed.notify_all();
}
int interrupted(void* data) {
    return static_cast<PS2Runtime*>(data)->isStopRequested() ? 1 : 0;
}
void decode(const std::filesystem::path& path, const std::shared_ptr<Movie>& movie, PS2Runtime& runtime) {
    AVFormatContext* rawFormat = avformat_alloc_context();
    if (!rawFormat) throw std::runtime_error("Cannot allocate movie demuxer");
    rawFormat->interrupt_callback = {interrupted, &runtime};
    int result = avformat_open_input(&rawFormat, path.c_str(), nullptr, nullptr);
    if (result < 0) { if (rawFormat) avformat_free_context(rawFormat); checked(result, "Open movie"); }
    std::unique_ptr<AVFormatContext, void(*)(AVFormatContext*)> format(rawFormat, [](AVFormatContext* p) { avformat_close_input(&p); });
    checked(avformat_find_stream_info(format.get(), nullptr), "Read movie streams");
    const int streamIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    checked(streamIndex, "Find movie video");
    AVStream* stream = format->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) throw std::runtime_error("Movie video decoder is unavailable");
    std::unique_ptr<AVCodecContext, void(*)(AVCodecContext*)> decoder(avcodec_alloc_context3(codec), [](AVCodecContext* p) { avcodec_free_context(&p); });
    if (!decoder) throw std::runtime_error("Cannot allocate movie video decoder");
    checked(avcodec_parameters_to_context(decoder.get(), stream->codecpar), "Configure movie decoder");
    decoder->thread_count = 2;
    checked(avcodec_open2(decoder.get(), codec, nullptr), "Start movie decoder");
    std::unique_ptr<AVFrame, void(*)(AVFrame*)> frame(av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); });
    std::unique_ptr<AVPacket, void(*)(AVPacket*)> packet(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); });
    if (!frame || !packet) throw std::runtime_error("Cannot allocate movie packet/frame");
    std::unique_ptr<SwsContext, void(*)(SwsContext*)> scaler(nullptr, sws_freeContext);
    const AVRational guessed = av_guess_frame_rate(format.get(), stream, nullptr);
    const double duration = guessed.num > 0 && guessed.den > 0 ? av_q2d(av_inv_q(guessed)) : 1.0 / 30;
    double nextPts = 0;
    auto receive = [&]() -> bool {
        for (;;) {
            const int received = avcodec_receive_frame(decoder.get(), frame.get());
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return true;
            checked(received, "Decode movie frame");
            if (frame->width <= 0 || frame->height <= 0 || frame->width > 4096 || frame->height > 2048)
                throw std::runtime_error("Movie dimensions exceed bounded frame storage");
            Frame output; output.width = frame->width; output.height = frame->height; output.duration = duration;
            const int64_t pts = frame->best_effort_timestamp;
            const double sourcePts = pts == AV_NOPTS_VALUE ? nextPts : double(pts) * av_q2d(stream->time_base);
            nextPts = sourcePts + duration;
            {
                std::lock_guard lock(movie->mutex);
                if (!movie->haveFirstPts) { movie->firstVideoPts = sourcePts; movie->haveFirstPts = true; }
                output.pts = std::max(0.0, sourcePts - movie->firstVideoPts);
            }
            const AVRational sar = av_guess_sample_aspect_ratio(format.get(), stream, frame.get());
            output.aspect = double(output.width) / output.height * (sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0);
            output.rgba.resize(size_t(output.width) * output.height * 4);
            SwsContext* updated = sws_getCachedContext(scaler.release(), output.width, output.height,
                static_cast<AVPixelFormat>(frame->format), output.width, output.height, AV_PIX_FMT_RGBA,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            scaler.reset(updated);
            if (!scaler) throw std::runtime_error("Cannot create movie color conversion");
            uint8_t* planes[] = {output.rgba.data(), nullptr, nullptr, nullptr};
            const int strides[] = {output.width * 4, 0, 0, 0};
            if (sws_scale(scaler.get(), frame->data, frame->linesize, 0, output.height, planes, strides) != output.height)
                throw std::runtime_error("Movie color conversion returned an incomplete frame");
            std::unique_lock lock(movie->mutex);
            while (movie->frames.size() >= 3 && !movie->done && !runtime.isStopRequested())
                movie->changed.wait_for(lock, std::chrono::milliseconds(10));
            if (movie->done || runtime.isStopRequested()) return false;
            movie->end = std::max(movie->end, output.pts + output.duration);
            ++movie->decoded; movie->frames.push_back(std::move(output)); movie->changed.notify_all();
            av_frame_unref(frame.get());
        }
    };
    while (!runtime.isStopRequested()) {
        { std::lock_guard lock(movie->mutex); if (movie->done) return; }
        result = av_read_frame(format.get(), packet.get());
        if (result == AVERROR_EOF) break;
        checked(result, "Read movie packet");
        if (packet->stream_index == streamIndex) {
            const int sent = avcodec_send_packet(decoder.get(), packet.get());
            if (sent == AVERROR(EAGAIN)) { if (!receive()) return; checked(avcodec_send_packet(decoder.get(), packet.get()), "Send movie packet"); }
            else checked(sent, "Send movie packet");
            if (!receive()) return;
        }
        av_packet_unref(packet.get());
    }
    if (!runtime.isStopRequested()) {
        checked(avcodec_send_packet(decoder.get(), nullptr), "Finish movie decoder");
        if (!receive()) return;
    }
    std::lock_guard lock(movie->mutex);
    movie->eof = true;
    if (!movie->decoded) { movie->error = "Movie contains no decoded video frames"; finish(*movie, false, false); }
    movie->changed.notify_all();
}
}

bool active() {
    std::shared_ptr<Movie> movie;
    { std::lock_guard lock(currentMutex); movie = current; }
    if (!movie) return false;
    std::lock_guard lock(movie->mutex); return !movie->done;
}
void installAttractDiagnostic(PS2Runtime& runtime) {
    const char* evidence=std::getenv("NIGHT100_ATTRACT_TEST");
    if(!evidence || !*evidence)return;
    if(std::getenv("NIGHT100_SKIP_VIDEOS") || std::getenv("NIGHT100_MOVIE_AUTO_SKIP_SECONDS"))
        throw std::runtime_error("Attract completion test requires movies enabled and no automatic skip");
    attract.enabled=true;attract.evidence=std::filesystem::absolute(evidence);
    attract.sequence.clear();attract.selected=attract.played=attract.menuFrames=0;
    if(const char* order=std::getenv("NIGHT100_ATTRACT_SEQUENCE")) {
        std::istringstream input(order);std::string token;
        while(std::getline(input,token,',')) {
            size_t end=0;const unsigned number=unsigned(std::stoul(token,&end));
            if(end!=token.size() || number<1 || number>15 || std::find(attract.sequence.begin(),attract.sequence.end(),number)!=attract.sequence.end())
                throw std::runtime_error("Attract sequence requires distinct DEMO numbers 1..15");
            attract.sequence.push_back(number);
        }
        if(attract.sequence.empty())throw std::runtime_error("Attract sequence is empty");
    } else for(unsigned number=1;number<=15;++number)attract.sequence.push_back(number);
    std::filesystem::create_directories(attract.evidence);
    attract.originalName=runtime.lookupFunction(0x26c3d0);
    if(!attract.originalName || !runtime.replaceFunction(0x26c3d0,diagnosticMovieName)
       || !runtime.replaceFunction(0x270460,diagnosticMovie))throw std::runtime_error("Cannot install attract diagnostic");
    std::cout<<"[attract-test] original menu demo sequence, full duration, controller skip disabled; evidence "<<attract.evidence<<'\n';
}
void diagnosticMenuTick(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    if(!attract.enabled || GPR_U32(cpu,31)!=0x132540)return;
    ++attract.menuFrames;
    if(attract.menuFrames==60) {
        std::cout<<"[attract-test] menu responsive after "<<attract.played<<" movie returns; last-demo="<<attract.selected<<" guest frames=60\n"<<std::flush;
        attract.menuCapture.store(attract.played+1,std::memory_order_release);
    }
    if(attract.menuFrames<120)return;
    if(attract.played==attract.sequence.size()) {
        std::cout<<"[attract-test] completed attract sequence of "<<attract.played<<" movies and returned to menu\n"<<std::flush;
        runtime->requestStop();return;
    }
    // Lower only the original idle timer. The menu performs its own state
    // change, zFMVPlay loader wait, sound suspend/resume, and return sequence.
    const float expired=-1;const uint32_t notFirstBoot=0;
    std::memcpy(ram+0x357ddc,&expired,4);std::memcpy(ram+0x3574d0,&notFirstBoot,4);
}
void play(uint8_t* ram, R5900Context* cpu, PS2Runtime* runtime) {
    uint32_t result = 1;
    std::shared_ptr<Movie> movie;
    try {
        PausedGameClocks clocks(*runtime);
        const uint32_t filenameAddress = GPR_U32(cpu, 4);
        if (filenameAddress == 0) { SET_GPR_S32(cpu, 2, 1); cpu->pc = GPR_U32(cpu, 31); return; }
        if (filenameAddress >= PS2_RAM_SIZE) throw std::runtime_error("Movie filename is outside ordinary guest RAM");
        const size_t limit = std::min<size_t>(1024, PS2_RAM_SIZE - filenameAddress);
        const char* filename = reinterpret_cast<const char*>(ram + filenameAddress);
        const size_t length = strnlen(filename, limit);
        if (length == limit) throw std::runtime_error("Movie filename is not terminated");
        const auto path = moviePath(std::string(filename, length));
        movie = std::make_shared<Movie>(); movie->filename = path.filename().string();
        movie->buttons = GPR_U32(cpu, 5);
        const float requested = cpu->f[12];
        const float frames = std::isfinite(requested) ? std::max(10.0f, 30.0f * requested) : 10.0f;
        movie->minimum = (std::floor(double(frames)) + 1.0) / 30.0;
        movie->audio = MoviePlayback::loadOriginalMovieAudio(path);
        movie->nonzeroAudio = std::count_if(movie->audio.interleaved.begin(), movie->audio.interleaved.end(), [](int16_t value) { return value != 0; });
        std::cout << "[movie] starting " << movie->filename << " skip-mask=0x" << std::hex << movie->buttons << std::dec
                  << " minimum=" << movie->minimum << "s audio=" << movie->audio.sampleRate << "Hz/" << movie->audio.channels
                  << "ch samples=" << movie->audio.interleaved.size() << " nonzero=" << movie->nonzeroAudio << std::endl;
        { std::lock_guard lock(currentMutex); current = movie; }
        decode(path, movie, *runtime);
        std::unique_lock lock(movie->mutex);
        while (!movie->done && !runtime->isStopRequested()) movie->changed.wait_for(lock, std::chrono::milliseconds(10));
        if (runtime->isStopRequested() && !movie->done) finish(*movie, false, true);
        if (!movie->error.empty()) throw std::runtime_error(movie->error);
        result = 0;
        std::cout << "[movie] " << (movie->skipped ? "skipped " : movie->aborted ? "stopped " : "finished ") << movie->filename
                  << " decoded=" << movie->decoded << " displayed=" << movie->displayed << " dropped=" << movie->dropped
                  << " audio-callbacks=" << movie->callbackCalls.load(std::memory_order_relaxed)
                  << " callback-source-frames=" << movie->callbackSourceFrames.load(std::memory_order_relaxed) << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "[movie] playback failed: " << error.what() << std::endl;
        if (movie) { std::lock_guard lock(movie->mutex); movie->error = error.what(); finish(*movie, false, true); }
    }
    // The window retains its own shared ownership until the audio callback is
    // detached and texture released; completed movies need no global retention.
    { std::lock_guard lock(currentMutex); if (current == movie) current.reset(); }
    if (result == 0) SET_GPR_VEC(cpu, 2, _mm_setzero_si128());
    else SET_GPR_S32(cpu, 2, static_cast<int32_t>(result));
    cpu->pc = GPR_U32(cpu, 31);
}
void tickAndDraw(PS2Runtime& runtime) {
    if(const unsigned capture=attract.menuCapture.exchange(0,std::memory_order_acq_rel)) {
        rlDrawRenderBatchActive();const Image screen=LoadImageFromScreen();
        ExportImage(screen,(attract.evidence/("menu-after-"+std::to_string(capture-1)+".png")).c_str());UnloadImage(screen);
    }
    std::shared_ptr<Movie> movie;
    { std::lock_guard lock(currentMutex); movie = current; }
    if (window.movie != movie) {
        releaseWindowResources(); window.movie = movie;
        window.previousButtons = readButtons(runtime);
    }
    if (!movie) return;
    Frame upload; bool haveUpload = false, show = false, done = false;
    {
        std::lock_guard lock(movie->mutex);
        if (movie->done || runtime.isStopRequested()) {
            if (!movie->done) finish(*movie, false, true);
            done = true;
        } else {
            const uint32_t down = readButtons(runtime), pressed = down & ~window.previousButtons;
            window.previousButtons = down;
            if (!movie->started && !movie->frames.empty()) {
                movie->start = Clock::now(); movie->started = true;
                if (!movie->audio.interleaved.empty() && movie->audio.sampleRate && movie->audio.channels && IsAudioDeviceReady()) {
                    SetAudioStreamBufferSizeDefault(1024);
                    window.audio = LoadAudioStream(movie->audio.sampleRate, 16, movie->audio.channels);
                    SetAudioStreamBufferSizeDefault(0);
                    window.audioLoaded = window.audio.buffer != nullptr;
                    if (window.audioLoaded) {
                        const double audioPts = movie->audio.initialPtsSeconds.value_or(movie->firstVideoPts);
                        movie->audioCursor.store(static_cast<int64_t>(std::llround((movie->firstVideoPts - audioPts) * movie->audio.sampleRate)), std::memory_order_relaxed);
                        callbackMovie.store(movie.get(), std::memory_order_release);
                        SetAudioStreamCallback(window.audio, audioCallback); PlayAudioStream(window.audio);
                    } else std::cerr << "[movie] host audio stream creation failed" << std::endl;
                }
            }
            const double elapsed = movie->started ? std::chrono::duration<double>(Clock::now() - movie->start).count() : 0;
            bool autoSkip = false;
            if (const char* value = std::getenv("NIGHT100_MOVIE_AUTO_SKIP_SECONDS")) {
                const double after = std::strtod(value, nullptr);
                autoSkip = after > 0 && elapsed >= after && elapsed >= movie->minimum;
            }
            if (movie->started && elapsed >= movie->minimum && ((pressed & movie->buttons) || autoSkip)) {
                finish(*movie, true, false); done = true;
            } else {
                while (!movie->frames.empty() && movie->frames.front().pts <= elapsed + 0.002) {
                    if (haveUpload) ++movie->dropped;
                    upload = std::move(movie->frames.front()); movie->frames.pop_front(); haveUpload = true;
                }
                movie->changed.notify_all();
                double end = movie->end;
                if (!movie->audio.interleaved.empty() && movie->audio.sampleRate && movie->audio.channels) {
                    const double audioPts = movie->audio.initialPtsSeconds.value_or(movie->firstVideoPts);
                    end = std::max(end, audioPts - movie->firstVideoPts
                        + double(movie->audio.interleaved.size() / movie->audio.channels) / movie->audio.sampleRate);
                }
                if (movie->eof && movie->frames.empty() && movie->started && elapsed >= end) {
                    finish(*movie, false, false); done = true;
                } else show = true;
            }
        }
    }
    if (done) { releaseWindowResources(); return; }
    if (haveUpload) {
        if (window.texture.id && (window.texture.width != upload.width || window.texture.height != upload.height)) {
            rlDrawRenderBatchActive(); UnloadTexture(window.texture); window.texture = {};
        }
        if (!window.texture.id) {
            Image image{upload.rgba.data(), upload.width, upload.height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            window.texture = LoadTextureFromImage(image); SetTextureFilter(window.texture, TEXTURE_FILTER_BILINEAR);
        } else UpdateTexture(window.texture, upload.rgba.data());
        window.aspect = upload.aspect;
        std::lock_guard lock(movie->mutex); ++movie->displayed;
    }
    if (show) {
        const float width = float(GetScreenWidth()), height = float(GetScreenHeight());
        DrawRectangle(0, 0, int(width), int(height), BLACK);
        if (window.texture.id) {
            const float videoWidth = std::min(width, float(height * window.aspect));
            const float videoHeight = float(videoWidth / window.aspect);
            DrawTexturePro(window.texture, {0, 0, float(window.texture.width), float(window.texture.height)},
                {(width - videoWidth) * 0.5f, (height - videoHeight) * 0.5f, videoWidth, videoHeight}, {0, 0}, 0, WHITE);
            if(attract.enabled && !movie->diagnosticCaptured && movie->displayed>=30) {
                rlDrawRenderBatchActive();const Image screen=LoadImageFromScreen();
                ExportImage(screen,(attract.evidence/("demo-"+std::to_string(attract.selected)+".png")).c_str());UnloadImage(screen);
                movie->diagnosticCaptured=true;
            }
        }
    }
}
void shutdown() {
    std::shared_ptr<Movie> movie;
    { std::lock_guard lock(currentMutex); movie = std::move(current); }
    if (movie) { std::lock_guard lock(movie->mutex); if (!movie->done) finish(*movie, false, true); }
    releaseWindowResources();
}
}
