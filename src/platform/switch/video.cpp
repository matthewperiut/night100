#include "video.h"
#include "video_policy.h"
#include "night100_switch_host.h"
#include "native_video.h"
#include "movie_audio.h"
#include "telemetry.h"
#include "resource_aliases.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "raylib.h"
#include <switch.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_nvtegra.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using Clock = std::chrono::steady_clock;
SwitchVideo::Present presenter = nullptr;
void* presenterData = nullptr;
std::atomic<bool> playing{false}, shutdownRequested{false};

void checked(int code, const char* operation) {
    if(code >= 0)return;
    std::array<char, AV_ERROR_MAX_STRING_SIZE> reason{};
    av_strerror(code, reason.data(), reason.size());
    throw std::runtime_error(std::string(operation) + ": " + reason.data());
}

std::filesystem::path moviePath(const std::string& guest) {
    std::string relative=guest;
    std::replace(relative.begin(),relative.end(),'\\','/');
    if(const auto at=relative.find(':');at!=std::string::npos)relative.erase(0,at+1);
    while(!relative.empty() && relative.front()=='/')relative.erase(relative.begin());
    if(const auto at=relative.rfind(';');at!=std::string::npos)relative.erase(at);
    std::filesystem::path path=PS2Runtime::getIoPaths().cdRoot;
    relative=ResourceAliases::resolve(path,relative);
    for(const auto& component:std::filesystem::path(relative)) {
        auto part=component.string();
        if(part.empty() || part==".")continue;
        if(part=="..")throw std::runtime_error("Movie path escapes disc root");
        if(std::filesystem::exists(path/component)){path/=component;continue;}
        std::transform(part.begin(),part.end(),part.begin(),[](unsigned char c){return char(std::toupper(c));});
        bool found=false;
        if(std::filesystem::is_directory(path))for(const auto& entry:std::filesystem::directory_iterator(path)) {
            auto name=entry.path().filename().string();
            std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::toupper(c));});
            if(name==part){path=entry.path();found=true;break;}
        }
        if(!found)throw std::runtime_error("Movie file not found: "+(path/component).string());
    }
    if(!std::filesystem::is_regular_file(path))throw std::runtime_error("Movie is not a file: "+path.string());
    return path;
}

uint32_t buttons(PS2Runtime& runtime) {
    std::array<uint8_t,32> pad{};
    if(!runtime.padBackend().readState(0,0,pad.data(),pad.size()))return 0;
    const uint16_t down=uint16_t(~(uint16_t(pad[2]) | uint16_t(pad[3])<<8));
    constexpr uint32_t converted[]={2,0x400,0x4000,1,0x10,0x20,0x40,0x80,
        0x200,0x2000,0x100,0x1000,0x40000,0x20000,0x10000,0x80000};
    uint32_t result=0;
    for(unsigned bit=0;bit<16;++bit)if(down&(1u<<bit))result|=converted[bit];
    return result;
}

struct Audio {
    MoviePlayback::MovieAudio source;
    AudioStream stream{};
    std::atomic<int64_t> cursor{0};
    std::atomic<uint64_t> consumed{0};
    bool loaded=false;
    explicit Audio(const std::filesystem::path& path):source(MoviePlayback::loadOriginalMovieAudio(path)){}
    void start(double videoPts);
    ~Audio();
};
std::atomic<Audio*> callbackAudio{nullptr};

void audioCallback(void* output,unsigned frames) {
    Audio* audio=callbackAudio.load(std::memory_order_acquire);
    if(!audio)return;
    const unsigned channels=audio->source.channels;
    auto* samples=static_cast<int16_t*>(output);
    std::memset(samples,0,size_t(frames)*channels*sizeof(int16_t));
    const int64_t cursor=audio->cursor.load(std::memory_order_relaxed);
    const int64_t total=int64_t(audio->source.interleaved.size()/channels);
    const int64_t begin=std::max<int64_t>(0,cursor),end=std::min<int64_t>(total,cursor+frames);
    if(end>begin) {
        std::memcpy(samples+(begin-cursor)*channels,audio->source.interleaved.data()+begin*channels,size_t(end-begin)*channels*sizeof(int16_t));
        audio->consumed.fetch_add(uint64_t(end-begin),std::memory_order_relaxed);
    }
    audio->cursor.store(cursor+frames,std::memory_order_relaxed);
}

void Audio::start(double videoPts) {
    if(source.interleaved.empty())return;
    if(!IsAudioDeviceReady())throw std::runtime_error("Original movie PCM requires the Switch audio device");
    SetAudioStreamBufferSizeDefault(1024);
    stream=LoadAudioStream(source.sampleRate,16,source.channels);
    loaded=stream.buffer!=nullptr;
    if(!loaded)throw std::runtime_error("Cannot create original movie PCM stream");
    const double audioPts=source.initialPtsSeconds.value_or(videoPts);
    cursor.store(int64_t(std::llround((videoPts-audioPts)*source.sampleRate)),std::memory_order_relaxed);
    callbackAudio.store(this,std::memory_order_release);
    SetAudioStreamCallback(stream,audioCallback);
    PlayAudioStream(stream);
}
Audio::~Audio() {
    // Detach the callback pointer before stopping: the audio thread may still
    // be inside audioCallback when teardown begins, and must not see a dead Audio.
    Audio* expected=this;
    callbackAudio.compare_exchange_strong(expected,nullptr,std::memory_order_acq_rel);
    if(loaded) {
        SetAudioStreamCallback(stream,nullptr);
        StopAudioStream(stream);
        UnloadAudioStream(stream);
    }
}

AVPixelFormat hardwareFormat(AVCodecContext*,const AVPixelFormat* formats) {
    for(const AVPixelFormat* p=formats;*p!=AV_PIX_FMT_NONE;++p)if(*p==AV_PIX_FMT_NVTEGRA)return *p;
    // Returning NONE is deliberate: the port must never silently fall back to
    // software decoding when the requested hardware format is unavailable.
    return AV_PIX_FMT_NONE;
}
int interrupted(void* data) {
    return static_cast<PS2Runtime*>(data)->isStopRequested() || shutdownRequested.load(std::memory_order_relaxed);
}

SwitchVideo::Frame hardwareFrame(AVFrame* frame,double aspect) {
    if(frame->format!=AV_PIX_FMT_NVTEGRA || !frame->hw_frames_ctx)
        throw std::runtime_error("Movie decoder returned a software frame");
    const auto* frames=reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
    if(frames->sw_format!=AV_PIX_FMT_NV12)throw std::runtime_error("NVDEC returned an unsupported hardware surface layout");
    AVNVTegraMap* map=av_nvtegra_frame_get_fbuf_map(frame);
    if(!map || !av_nvtegra_map_get_addr(map))throw std::runtime_error("NVDEC surface storage is unavailable");
    AVFrame* retained=av_frame_clone(frame);
    if(!retained)throw std::runtime_error("Cannot retain NVDEC surface");
    SwitchVideo::Frame out;
    out.lifetime=std::shared_ptr<const void>(retained,[](const void* p){auto* f=const_cast<AVFrame*>(static_cast<const AVFrame*>(p));av_frame_free(&f);});
    out.storage=av_nvtegra_map_get_addr(map);
    out.storageBytes=av_nvtegra_map_get_size(map);
    out.mapHandle=av_nvtegra_map_get_handle(map);
    out.width=uint32_t(frame->width);out.height=uint32_t(frame->height);
    out.pitchLinear=map->is_linear;out.aspect=aspect;
    out.fullRange=frame->color_range==AVCOL_RANGE_JPEG;
    if(frame->colorspace==AVCOL_SPC_BT709)out.matrix=SwitchVideo::Matrix::Bt709;
    if(frame->colorspace==AVCOL_SPC_BT2020_NCL || frame->colorspace==AVCOL_SPC_BT2020_CL)out.matrix=SwitchVideo::Matrix::Bt2020;
    for(unsigned plane=0;plane<2;++plane) {
        const auto offset=reinterpret_cast<uintptr_t>(frame->data[plane])-reinterpret_cast<uintptr_t>(out.storage);
        if(uint64_t(offset)>=out.storageBytes || frame->linesize[plane]<=0)
            throw std::runtime_error("NVDEC plane exceeds its allocation");
        out.planeOffset[plane]=uint32_t(offset);out.planePitch[plane]=uint32_t(frame->linesize[plane]);
    }
    return out;
}

struct PlaybackInterrupted {};

// Movie boundaries are flushed to SD at once; the buffered stream is lost on a hard fatal.
void marker(const std::string& text) {
    std::cout<<text<<'\n';
    SwitchTelemetry::stageCurrent("[movie-marker] "+text);
}

// One NVDEC hardware device for the whole process. Creating and finalizing a
// device per movie interleaved MMU requests and channel teardown across movies:
// the third movie hit an invalid-handle result and the fourth hung in the mm
// service. Each decoder takes its own reference, and frames keep the device
// alive through their presenter fence.
AVBufferRef* processNvdecDevice=nullptr;
AVBufferRef* acquireNvdecDevice() {
    if(!processNvdecDevice) {
        AVBufferRef* device=nullptr;
        checked(av_hwdevice_ctx_create(&device,AV_HWDEVICE_TYPE_NVTEGRA,nullptr,nullptr,0),"Open Tegra X1 NVDEC hardware");
        processNvdecDevice=device;
        marker("[nvdec-lifetime] device-init process=1 ref="+std::to_string(reinterpret_cast<uintptr_t>(device)));
    }
    AVBufferRef* reference=av_buffer_ref(processNvdecDevice);
    if(!reference)throw std::runtime_error("Cannot reference NVDEC device");
    return reference;
}

void playMovie(const std::filesystem::path& path,uint32_t skipMask,double minimum,PS2Runtime& runtime) {
    auto checkPlayback=[&](int code,const char* operation) {
        // An explicit libavformat interrupt belongs to ordinary shutdown. Keep
        // unrelated decode/I/O failures fatal even if shutdown races them.
        if(interrupted(&runtime) && (code>=0 || code==AVERROR_EXIT))throw PlaybackInterrupted{};
        checked(code,operation);
    };
    if(!presenter)throw std::runtime_error("Switch NVDEC movie presenter is not registered");
    AVFormatContext* raw=avformat_alloc_context();
    if(!raw)throw std::runtime_error("Cannot allocate movie demuxer");
    raw->interrupt_callback={interrupted,&runtime};
    const std::string filename="file:"+path.string();
    int result=avformat_open_input(&raw,filename.c_str(),nullptr,nullptr);
    if(result<0){if(raw)avformat_free_context(raw);checkPlayback(result,"Open original PSS movie");}
    std::unique_ptr<AVFormatContext,void(*)(AVFormatContext*)> format(raw,[](AVFormatContext* p){avformat_close_input(&p);});
    checkPlayback(avformat_find_stream_info(format.get(),nullptr),"Read original PSS streams");
    const int index=av_find_best_stream(format.get(),AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
    checkPlayback(index,"Find original movie video");
    AVStream* stream=format->streams[index];
    const auto* parameters=stream->codecpar;
    const auto relative=path.lexically_relative(PS2Runtime::getIoPaths().cdRoot);
    const bool optional=SwitchVideo::optionalMovie(relative.parent_path().generic_string(),
                                                  relative.filename().string());
    const auto* descriptor=avcodec_descriptor_get(parameters->codec_id);
    auto support=!descriptor || descriptor->type!=AVMEDIA_TYPE_VIDEO
        ? SwitchVideo::VideoSupport::Unidentified
        : parameters->codec_id==AV_CODEC_ID_MPEG2VIDEO ? SwitchVideo::VideoSupport::HardwareMpeg2
        : SwitchVideo::VideoSupport::Unsupported;
    if (support==SwitchVideo::VideoSupport::HardwareMpeg2
        && parameters->format!=AV_PIX_FMT_NONE && parameters->format!=AV_PIX_FMT_YUV420P)
        support=SwitchVideo::VideoSupport::Unsupported;
    const auto decision=SwitchVideo::movieDecision(support,optional);
    const std::string description=relative.generic_string()+" codec="
        +avcodec_get_name(parameters->codec_id)+" id="+std::to_string(parameters->codec_id)
        +" format="+std::to_string(parameters->format);
    if (decision==SwitchVideo::VideoDecision::SkipOptional) {
        marker("[movie-switch] optional unsupported movie skipped: "+description);
        return;
    }
    if (decision==SwitchVideo::VideoDecision::Fail)
        throw std::runtime_error("Switch hardware movie unsupported or unidentified: "+description);
    marker("[movie-switch] start "+path.filename().string()+" "+description);
    Audio audio(path);
    marker("[movie-switch] pcm-loaded "+path.filename().string()+" bytes="+std::to_string(audio.source.interleaved.size()*2)+
        " channels="+std::to_string(audio.source.channels)+" rate="+std::to_string(audio.source.sampleRate));
    const AVCodec* codec=avcodec_find_decoder(stream->codecpar->codec_id);
    if(!codec)throw std::runtime_error("MPEG-2 NVDEC decoder is missing");
    // Declared before the decoder, so this logs after avcodec_free_context ran.
    struct DecoderLifetime {
        std::string name;
        ~DecoderLifetime() {marker("[nvdec-lifetime] decoder-close "+name);}
    } decoderLifetime{path.filename().string()};
    std::unique_ptr<AVCodecContext,void(*)(AVCodecContext*)> decoder(avcodec_alloc_context3(codec),[](AVCodecContext* p){avcodec_free_context(&p);});
    if(!decoder)throw std::runtime_error("Cannot allocate MPEG-2 decoder");
    checkPlayback(avcodec_parameters_to_context(decoder.get(),stream->codecpar),"Configure MPEG-2 decoder");
    decoder->get_format=hardwareFormat;decoder->thread_count=1;
    decoder->hw_device_ctx=acquireNvdecDevice();
    checkPlayback(avcodec_open2(decoder.get(),codec,nullptr),"Start hardware MPEG-2 decoder");
    marker("[movie-switch] decoder-open "+path.filename().string()+" device-ref="+std::to_string(reinterpret_cast<uintptr_t>(decoder->hw_device_ctx)));
    std::unique_ptr<AVPacket,void(*)(AVPacket*)> packet(av_packet_alloc(),[](AVPacket* p){av_packet_free(&p);});
    std::unique_ptr<AVFrame,void(*)(AVFrame*)> frame(av_frame_alloc(),[](AVFrame* p){av_frame_free(&p);});
    if(!packet || !frame)throw std::runtime_error("Cannot allocate movie frame metadata");
    const AVRational rate=av_guess_frame_rate(format.get(),stream,nullptr);
    const double duration=rate.num>0 && rate.den>0 ? av_q2d(av_inv_q(rate)) : 1.0/30;
    bool started=false,skipped=false,stopped=false;
    double firstPts=0,nextPts=0,end=0;
    Clock::time_point start{};
    uint64_t inactiveAtStart=0;
    uint64_t decoded=0,presented=0;
    uint32_t previous=buttons(runtime);
    auto elapsed=[&](){
        if(!started)return 0.0;
        const auto wall=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
        return double(SwitchVideo::activeMovieNanoseconds(uint64_t(std::max<int64_t>(0,wall)),
            inactiveAtStart,night100SwitchHostInactiveNanoseconds()))/1e9;
    };
    auto continuePlaying=[&](){
        if(interrupted(&runtime)){stopped=true;return false;}
        runtime.serviceHostLifecycle();
        if(interrupted(&runtime)){stopped=true;return false;}
        const uint32_t down=buttons(runtime),pressed=down&~previous;previous=down;
        if(started && elapsed()>=minimum && (pressed&skipMask)){skipped=true;return false;}
        return true;
    };
    auto waitUntil=[&](double pts){
        while(continuePlaying() && elapsed()+0.001<pts)svcSleepThread(1000000);
        return !stopped && !skipped;
    };
    auto receive=[&](){
        for(;;) {
            const int received=avcodec_receive_frame(decoder.get(),frame.get());
            if(received==AVERROR(EAGAIN) || received==AVERROR_EOF)return true;
            checkPlayback(received,"Decode MPEG-2 on NVDEC");
            if(frame->width<=0 || frame->height<=0 || frame->width>4096 || frame->height>2048)
                throw std::runtime_error("Movie dimensions exceed bounded hardware storage");
            const int64_t pts=frame->best_effort_timestamp;
            const double sourcePts=pts==AV_NOPTS_VALUE?nextPts:double(pts)*av_q2d(stream->time_base);
            nextPts=sourcePts+duration;
            if(!started) {
                firstPts=sourcePts;audio.start(firstPts);start=Clock::now();
                inactiveAtStart=night100SwitchHostInactiveNanoseconds();started=true;
                marker("[movie-switch] NVDEC MPEG2 "+path.filename().string()+" audio="+std::to_string(audio.source.sampleRate)+"Hz/"+std::to_string(audio.source.channels)+"ch");
            }
            ++decoded;
            const double relative=std::max(0.0,sourcePts-firstPts);
            end=std::max(end,relative+duration);
            if(!waitUntil(relative))return false;
            const AVRational sar=av_guess_sample_aspect_ratio(format.get(),stream,frame.get());
            const double aspect=double(frame->width)/frame->height*(sar.num>0 && sar.den>0?av_q2d(sar):1.0);
            auto surface=hardwareFrame(frame.get(),aspect);
            if(!presenter(surface,presenterData))throw std::runtime_error("Deko failed to present the NVDEC surface");
            ++presented;
            av_frame_unref(frame.get());
        }
    };
    // FFmpeg's nvtegra start-frame path waits on the previous frame's fence and
    // requests the NVDEC clock. The Horizon result -58369 (0xE401) from that path
    // has been observed on HILogo.pss, while the decoder's own status is clean.
    // Retry the same packet a few times, with a logged marker, before failing.
    constexpr int nvdecTransientResult=AVERROR(0xE401);
    constexpr unsigned nvdecRetries=4;
    auto sendPacket=[&]()->bool {
        for(unsigned attempt=0;;++attempt) {
            int sent=avcodec_send_packet(decoder.get(),packet.get());
            if(sent==AVERROR(EAGAIN)) {
                if(!receive())return false;
                sent=avcodec_send_packet(decoder.get(),packet.get());
            }
            if(sent!=nvdecTransientResult || attempt>=nvdecRetries) {
                checkPlayback(sent,"Send MPEG-2 packet");
                return true;
            }
            marker("[movie-switch] NVDEC transient result retry "+std::to_string(attempt+1)+" "+path.filename().string());
            svcSleepThread(5000000);
            if(!receive())return false;
        }
    };
    while(continuePlaying()) {
        result=av_read_frame(format.get(),packet.get());
        if(result==AVERROR_EOF)break;
        checkPlayback(result,"Read original PSS packet");
        if(packet->stream_index==index) {
            if(!sendPacket())break;
            if(!receive())break;
        }
        av_packet_unref(packet.get());
    }
    if(!stopped && !skipped) {
        checkPlayback(avcodec_send_packet(decoder.get(),nullptr),"Finish NVDEC decoder");
        receive();
        if(!decoded)throw std::runtime_error("Movie contained no decoded hardware frames");
        if(!audio.source.interleaved.empty())end=std::max(end,audio.source.initialPtsSeconds.value_or(firstPts)-firstPts+double(audio.source.interleaved.size()/audio.source.channels)/audio.source.sampleRate);
        waitUntil(end);
    }
    marker(std::string("[movie-switch] ")+(skipped?"skipped ":stopped?"stopped ":"finished ")+path.filename().string()+
        " hardware-decoded="+std::to_string(decoded)+" presented="+std::to_string(presented)+" elapsed="+std::to_string(elapsed())+
        " audio-source-frames="+std::to_string(audio.consumed.load(std::memory_order_relaxed)));
}
}

namespace SwitchVideo {
void setPresenter(Present present,void* opaque){presenter=present;presenterData=opaque;}
}
namespace NativeVideo {
bool active(){return playing.load(std::memory_order_acquire);}
void play(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    // Cooperate before pausing movie clocks, so background entry cannot leave
    // the producer drawing or unbalance a later resume of the state clocks.
    runtime->serviceHostLifecycle();
    if(runtime->isStopRequested()) {
        SET_GPR_VEC(cpu,2,_mm_setzero_si128());cpu->pc=GPR_U32(cpu,31);return;
    }
    struct PlaybackScope {
        PS2Runtime& runtime;
        explicit PlaybackScope(PS2Runtime& r):runtime(r){playing.store(true,std::memory_order_release);shutdownRequested.store(false,std::memory_order_relaxed);runtime.pauseStateClocks();}
        ~PlaybackScope() noexcept(false) {
            playing.store(false,std::memory_order_release);
            try {runtime.resumeStateClocks();}
            catch(...) {
                runtime.requestStop();
                // A failed lifecycle drain must propagate on normal exit.
                // During another failure, preserve that original exception.
                if(!std::uncaught_exceptions())throw;
            }
        }
    } scope(*runtime);
    try {
        const uint32_t address=GPR_U32(cpu,4);
        if(!address || address>=PS2_RAM_SIZE)throw std::runtime_error("Movie filename is outside guest RAM");
        const size_t limit=std::min<size_t>(1024,PS2_RAM_SIZE-address);
        const auto* filename=reinterpret_cast<const char*>(ram+address);
        const size_t length=strnlen(filename,limit);
        if(length==limit)throw std::runtime_error("Movie filename is not terminated");
        const float requested=cpu->f[12];
        const double frames=std::isfinite(requested)?std::max(10.0,30.0*requested):10.0;
        const double minimum=(std::floor(frames)+1)/30;
        playMovie(moviePath(std::string(filename,length)),GPR_U32(cpu,5),minimum,*runtime);
    } catch(const PlaybackInterrupted&) {
        std::cout<<"[movie-switch] stopped during movie setup or demux\n";
    } catch(const std::exception& error) {
        std::cerr<<"[movie-switch] hardware playback failed: "<<error.what()<<'\n';
        SwitchTelemetry::finishCurrent(std::string("NIGHT100_SWITCH_FATAL movie: ")+error.what());
        runtime->requestStop();
        // run() catches producer exceptions, joins that thread, then rethrows on
        // the main thread. Preserve the failure for its fatal telemetry path;
        // returning here would turn a failed decoder into a normal STOP exit.
        throw;
    } catch(...) {
        runtime->requestStop();
        throw std::runtime_error("Hardware movie playback failed with an unknown exception");
    }
    SET_GPR_VEC(cpu,2,_mm_setzero_si128());
    cpu->pc=GPR_U32(cpu,31);
}
// The Deko queue belongs to the game thread. Presentation happens inside play;
// the host/main thread continues libnx applet, HID and audout polling separately.
void tickAndDraw(PS2Runtime&){}
void shutdown(){shutdownRequested.store(true,std::memory_order_relaxed);}
// Desktop-only attract automation is deliberately separate from ordinary Switch
// menu timing. Real original menu idle transitions still call the same player.
void installAttractDiagnostic(PS2Runtime&){}
void diagnosticMenuTick(uint8_t*,R5900Context*,PS2Runtime*){}
}
