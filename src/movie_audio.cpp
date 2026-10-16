#include "movie_audio.h"
#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace MoviePlayback {
namespace {
constexpr uint64_t maxFileBytes = 1024ull * 1024 * 1024;
constexpr uint64_t maxPcmBytes = 256ull * 1024 * 1024;
[[noreturn]] void invalid(const char* reason) { throw std::runtime_error(std::string("PSS audio: ") + reason); }
struct Input {
    std::ifstream stream;
    uint64_t size = 0, position = 0;
    explicit Input(const std::filesystem::path& path) : stream(path, std::ios::binary) {
        if (!stream) invalid("cannot open movie");
        stream.seekg(0,std::ios::end);
        const auto end=stream.tellg();
        if (end<0 || uint64_t(end)>maxFileBytes) invalid("movie size exceeds limit");
        size=uint64_t(end);stream.seekg(0);
    }
    void read(uint8_t* out,size_t bytes) {
        if (bytes>size-position) invalid("truncated packet");
        if (bytes && !stream.read(reinterpret_cast<char*>(out),std::streamsize(bytes))) invalid("movie read failed");
        position+=bytes;
    }
    uint8_t byte() { uint8_t b=0;read(&b,1);return b; }
    void skip(uint64_t bytes) {
        if (bytes>size-position) invalid("packet exceeds movie");
        if(bytes && !stream.seekg(std::streamoff(bytes),std::ios::cur)) invalid("movie seek failed");
        position+=bytes;
    }
};
uint32_t le32(const uint8_t* b) { return uint32_t(b[0]) | uint32_t(b[1])<<8 | uint32_t(b[2])<<16 | uint32_t(b[3])<<24; }
uint64_t timestamp(const uint8_t* b) {
    if (!(b[0]&1) || !(b[2]&1) || !(b[4]&1)) invalid("invalid PES timestamp markers");
    return (uint64_t((b[0]>>1)&7)<<30) | (uint64_t(b[1])<<22) |
        (uint64_t((b[2]>>1)&127)<<15) | (uint64_t(b[3])<<7) | (b[4]>>1);
}
struct Pes { size_t payload=0;std::optional<double> pts; };
Pes pesHeader(const std::vector<uint8_t>& p) {
    if (p.size()<3 || (p[0]&0xc0)!=0x80) invalid("unsupported or truncated MPEG2 audio PES header");
    const size_t end=3u+p[2];
    if(end>p.size())invalid("truncated optional PES header");
    const unsigned flags=(p[1]>>6)&3;
    if(flags==1)invalid("reserved PES timestamp flags");
    Pes result{end,{}};
    if(flags==2 || flags==3) {
        if(p[2]<(flags==3?10u:5u))invalid("truncated PES timestamp");
        if((p[3]>>4)!=(flags==3?3u:2u))invalid("invalid PES PTS marker");
        result.pts=double(timestamp(p.data()+3))/90000.0;
        if(flags==3) {
            if((p[8]>>4)!=1)invalid("invalid PES DTS marker");
            (void)timestamp(p.data()+8);
        }
    }
    return result;
}
}
MovieAudio loadOriginalMovieAudio(const std::filesystem::path& path) {
    Input in(path);
    std::vector<uint8_t> encoded;
    std::optional<double> firstPts;
    bool sawAudio=false,headerChecked=false;
    uint32_t channels=0,rate=0,interleave=0,bodyBytes=0;
    while(in.position<in.size) {
        if(in.size-in.position<4)invalid("truncated program-stream start code");
        std::array<uint8_t,4> code{};in.read(code.data(),code.size());
        if(code[0] || code[1] || code[2]!=1)invalid("invalid program-stream packet boundary");
        const uint8_t id=code[3];
        if(id==0xb9) {
            // A program end is terminal. Permit only zero stuffing after it.
            while(in.position<in.size)if(in.byte()!=0)invalid("data after program end");
            break;
        }
        if(id==0xba) {
            const uint8_t first=in.byte();
            if((first&0xc0)==0x40) {
                std::array<uint8_t,9> rest{};in.read(rest.data(),rest.size());in.skip(rest.back()&7);
            } else if((first&0xf0)==0x20) in.skip(7);
            else invalid("invalid MPEG pack header");
            continue;
        }
        std::array<uint8_t,2> lengthBytes{};in.read(lengthBytes.data(),2);
        const unsigned length=unsigned(lengthBytes[0])*256+lengthBytes[1];
        // The original game's PSS uses bounded PES packets. Reject unbounded
        // packets instead of guessing where an audio payload ends.
        if(!length)invalid("unbounded program-stream packet unsupported");
        if(id!=0xbd) { in.skip(length);continue; }
        std::vector<uint8_t> packet(length);in.read(packet.data(),packet.size());
        const auto pes=pesHeader(packet);
        const size_t bytes=packet.size()-pes.payload;
        if(bytes<4)invalid("truncated private audio prefix");
        const uint8_t* payload=packet.data()+pes.payload;
        if(payload[0]!=0xff || payload[1]!=0xa0) {
            if(sawAudio || payload[0]==0xff)invalid("unsupported private audio stream");
            continue;
        }
        if(payload[2] || payload[3])invalid("unsupported private audio prefix");
        if(!sawAudio) {firstPts=pes.pts;sawAudio=true;}
        const size_t append=bytes-4;
        if(append>maxPcmBytes+40-encoded.size())invalid("decoded audio exceeds limit");
        encoded.insert(encoded.end(),payload+4,payload+bytes);
        if(!headerChecked && encoded.size()>=40) {
            if(!std::equal(encoded.begin(),encoded.begin()+4,"SShd") || le32(encoded.data()+4)!=24 ||
               !std::equal(encoded.begin()+32,encoded.begin()+36,"SSbd")) invalid("invalid SShd/SSbd header");
            if(le32(encoded.data()+8)!=1)invalid("unsupported audio encoding (expected PCM16LE)");
            rate=le32(encoded.data()+12);channels=le32(encoded.data()+16);interleave=le32(encoded.data()+20);
            bodyBytes=le32(encoded.data()+36);
            if(rate<8000 || rate>192000 || channels<1 || channels>2 || !interleave || interleave>1024*1024 || (interleave&1))invalid("invalid PCM format");
            if(bodyBytes>maxPcmBytes || bodyBytes%(uint64_t(channels)*interleave))invalid("invalid PCM body size");
            headerChecked=true;
        }
        if(headerChecked && encoded.size()>uint64_t(bodyBytes)+40)invalid("PCM data exceeds declared body");
    }
    if(!sawAudio)return {};
    if(!headerChecked)invalid("truncated SShd/SSbd header");
    if(encoded.size()!=uint64_t(bodyBytes)+40)invalid("truncated PCM body");
    MovieAudio result;result.sampleRate=rate;result.channels=channels;result.initialPtsSeconds=firstPts;
    result.interleaved.resize(bodyBytes/2);
    const uint64_t stride=uint64_t(channels)*interleave;
    const uint8_t* body=encoded.data()+40;
    for(uint64_t block=0;block<bodyBytes;block+=stride)
        for(unsigned frame=0;frame<interleave/2;++frame)
            for(unsigned channel=0;channel<channels;++channel) {
                const uint64_t source=block+uint64_t(channel)*interleave+frame*2;
                const uint64_t dest=block/2+uint64_t(frame)*channels+channel;
                result.interleaved[size_t(dest)]=std::bit_cast<int16_t>(uint16_t(uint16_t(body[source]) | uint16_t(body[source+1])<<8));
            }
    return result;
}
}
