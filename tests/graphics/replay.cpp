#include "graphics/gl33_backend.h"
#include "graphics/gs_trace.h"
#include "runtime/gs/gs_cpu_backend.h"
#include <raylib.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <zlib.h>
namespace {
class TraceInput {
    gzFile file=nullptr;
    bool good=true;
public:
    explicit TraceInput(const char* path):file(gzopen(path,"rb")) {if(!file) throw std::runtime_error("Cannot open GS trace");gzbuffer(file,128*1024);}
    ~TraceInput(){if(file)gzclose(file);}
    void read(char* dst,size_t n){if(gzread(file,dst,unsigned(n))!=int(n))good=false;}
    explicit operator bool()const{return good;}
    int peek(){int value=gzgetc(file);int code=0;gzerror(file,&code);if(code!=Z_OK && code!=Z_STREAM_END)throw std::runtime_error("Invalid/truncated gzip trace");return value;}
};
template<class T> T decode(const std::vector<uint8_t>& p) {
    if(p.size()!=sizeof(T)) throw std::runtime_error("GS trace payload size mismatch");
    T value;std::memcpy(&value,p.data(),sizeof(T));return value;
}
void png(const std::filesystem::path& p,std::vector<uint8_t>& pixels,uint32_t w,uint32_t h) {
    if(!w || !h || pixels.size()<size_t(w)*h*4) return;
    Image image{pixels.data(),int(w),int(h),1,PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    if(!ExportImage(image,p.c_str())) throw std::runtime_error("Cannot save replay image");
}
}
int main(int argc,char** argv) {
try {
    if(argc<2 || argc>3) throw std::runtime_error("Usage: graphics_replay TRACE [OUTPUT_DIRECTORY]");
    const std::filesystem::path output=argc==3 ? argv[2] : "logs/gl-replay";
    std::filesystem::create_directories(output);
    for(const auto* name : {"latest-cpu.png","latest-gl33.png","latest-diff.png","first-failure-cpu.png","first-failure-gl33.png","first-failure-diff.png","report.json","evidence.json"})
        std::filesystem::remove(output/name);
    TraceInput input(argv[1]);
    GSTrace::Header header{};input.read(reinterpret_cast<char*>(&header),sizeof(header));
    if(!input || std::memcmp(header.magic,"N100GS\r\n",8) || header.version!=1 || header.batchSize!=sizeof(GSPrimitiveBatch) || header.contextSize!=sizeof(GSContext) || header.presentationSize!=sizeof(GSPresentationRequest)) throw std::runtime_error("Incompatible GS trace header");
    GSCpuBackend cpu;GL33Backend gpu;
    std::cout<<"GPU: "<<gpu.device()<<'\n';
    std::vector<uint8_t> cpuRam,gpuRam,payload;
    uint64_t records=0,draws=0,frames=0,badFrames=0,nonemptyFrames=0,totalBadPixels=0,maxBadPixels=0;
    double worstRmse=0;int maximumDelta=0;bool closed=false;
    for(;;) {
        GSTrace::Record record{};input.read(reinterpret_cast<char*>(&record),sizeof(record));
        if(!input) throw std::runtime_error("Unclosed/truncated GS trace");
        if(record.size>4*1024*1024 || uint32_t(record.op)>uint32_t(GSTrace::Op::Write)) throw std::runtime_error("Invalid GS trace record");
        payload.resize(record.size);if(record.size) input.read(reinterpret_cast<char*>(payload.data()),payload.size());
        if(!input) throw std::runtime_error("Truncated GS trace payload");
        ++records;
        using GSTrace::Op;
        if(record.op==Op::End) {closed=true;break;}
        switch(record.op) {
        case Op::Initialize:
            if(payload.size()!=4*1024*1024) throw std::runtime_error("Trace requires a complete 4 MiB initial VRAM image");
            cpuRam=payload;gpuRam=payload;cpu.Initialize(cpuRam.data(),cpuRam.size());gpu.Initialize(gpuRam.data(),gpuRam.size());break;
        case Op::Reset:cpu.Reset();gpu.Reset();break;
        case Op::Submit:{auto b=decode<GSPrimitiveBatch>(payload);cpu.Submit(b);gpu.Submit(b);++draws;
            if(std::getenv("NIGHT100_REPLAY_STEP") && frames>=20) {
                float x=0,y=0;for(unsigned i=0;i<b.vertexCount;++i){x+=b.vertices[i].x;y+=b.vertices[i].y;}
                x=x/b.vertexCount-(b.state.context.xyoffset.ofx>>4);y=y/b.vertexCount-(b.state.context.xyoffset.ofy>>4);
                if(x>=0 && x<640 && y>=0 && y<448) {
                    const auto& c=b.state.context;
                    auto a=cpu.ReadVram(c.frame.psm,c.frame.fbp<<5,c.frame.fbw,uint32_t(x),uint32_t(y));
                    auto z=gpu.ReadVram(c.frame.psm,c.frame.fbp<<5,c.frame.fbw,uint32_t(x),uint32_t(y));
                    int d=0;for(int ch=0;ch<3;++ch)d=std::max(d,std::abs(int((a>>(ch*8))&255)-int((z>>(ch*8))&255)));
                    if(d>8) {
                        std::cerr<<"FIRST DRAW MISMATCH draw="<<draws<<" frame="<<frames<<" type="<<int(b.state.prim.type)<<" xy="<<x<<','<<y<<" cpu="<<std::hex<<a<<" gpu="<<z<<" test="<<c.test<<" alpha="<<c.alpha<<std::dec<<" fbp="<<c.frame.fbp<<" zbp="<<c.zbuf.zbp<<" fpsm="<<int(c.frame.psm)<<" zpsm="<<int(c.zbuf.psm)<<" scissor="<<c.scissor.x0<<','<<c.scissor.x1<<','<<c.scissor.y0<<','<<c.scissor.y1<<" offset="<<c.xyoffset.ofx<<','<<c.xyoffset.ofy<<" zmask="<<c.zbuf.zmask<<" textured="<<b.state.prim.tme<<" texbase="<<c.tex0.tbp0<<" texpsm="<<int(c.tex0.psm)<<" texw="<<b.state.textureWidth<<" texh="<<b.state.textureHeight<<'\n';
                        for(unsigned i=0;i<b.vertexCount;++i){auto& v=b.vertices[i];std::cerr<<"v "<<v.x<<','<<v.y<<','<<v.z<<" rgba="<<int(v.r)<<','<<int(v.g)<<','<<int(v.b)<<','<<int(v.a)<<" stq="<<v.s<<','<<v.t<<','<<v.q<<'\n';}
                        throw std::runtime_error("Stopped at first visible draw divergence");
                    }
                }
            }
            break;}
        case Op::Clut:{auto c=decode<GSTrace::Clut>(payload);cpu.LoadClut(c.tex,c.clut);gpu.LoadClut(c.tex,c.clut);break;}
        case Op::Transfer:{auto c=decode<GSTransferCommand>(payload);cpu.BeginTransfer(c);gpu.BeginTransfer(c);break;}
        case Op::Upload:cpu.UploadImage(payload.data(),payload.size());gpu.UploadImage(payload.data(),payload.size());break;
        case Op::Flush:cpu.Flush();gpu.Flush();break;
        case Op::TextureFlush:cpu.TextureFlush();gpu.TextureFlush();break;
        case Op::Sync:{auto r=decode<GSSyncReason>(payload);cpu.Sync(r);gpu.Sync(r);break;}
        case Op::Clear:{auto c=decode<GSTrace::Clear>(payload);if(cpu.ClearFramebuffer(c.context,c.rgba)!=gpu.ClearFramebuffer(c.context,c.rgba)) throw std::runtime_error("Clear support mismatch");break;}
        case Op::Consume:{auto n=decode<uint32_t>(payload);if(n>4*1024*1024) throw std::runtime_error("Invalid transfer read size");std::vector<uint8_t>a(n),b(n);auto ca=cpu.ConsumeLocalToHostBytes(a.data(),n),cb=gpu.ConsumeLocalToHostBytes(b.data(),n);if(ca!=cb || !std::equal(a.begin(),a.begin()+ca,b.begin())) throw std::runtime_error("Local-to-host transfer mismatch");break;}
        case Op::Write:{auto w=decode<GSTrace::Write>(payload);cpu.WriteVram(w.psm,w.base,w.bw,w.x,w.y,w.value);gpu.WriteVram(w.psm,w.base,w.bw,w.x,w.y,w.value);break;}
        case Op::Present:{
            auto r=decode<GSPresentationRequest>(payload);auto a=cpu.Present(r),b=gpu.Present(r);++frames;
            if(a.width!=b.width || a.height!=b.height || a.pixels.size()!=b.pixels.size()) throw std::runtime_error("Presentation dimensions differ");
            if(!a) break;
            ++nonemptyFrames;
            uint64_t bad=0;double square=0;int maxDelta=0;
            const auto count=size_t(a.width)*a.height;
            std::vector<uint8_t> diff(count*4,255);
            for(size_t i=0;i<count;++i) {
                int pixelDelta=0;
                for(int c=0;c<3;++c) {int d=std::abs(int(a.pixels[i*4+c])-int(b.pixels[i*4+c]));square+=double(d)*d;pixelDelta=std::max(pixelDelta,d);diff[i*4+c]=uint8_t(std::min(d*8,255));}
                if(pixelDelta>8) ++bad;
                maxDelta=std::max(maxDelta,pixelDelta);
            }
            const double rmse=std::sqrt(square/double(count*3));
            const bool pass=bad<=count/1000 && rmse<=1.5;
            if(!pass) ++badFrames;
            worstRmse=std::max(worstRmse,rmse);maximumDelta=std::max(maximumDelta,maxDelta);totalBadPixels+=bad;maxBadPixels=std::max(maxBadPixels,bad);
            if(frames%10==0 || !pass) std::cout<<"frame="<<frames<<" bad-pixels="<<bad<<'/'<<count<<" RMSE="<<rmse<<" max-delta="<<maxDelta<<(pass?" PASS":" FAIL")<<'\n';
            // Retain the latest frame plus the first failure, without an
            // unbounded gallery. Both use exactly the same recorded commands.
            const std::string prefix=(!pass && badFrames==1) ? "first-failure-" : "latest-";
            png(output/(prefix+"cpu.png"),a.pixels,a.width,a.height);png(output/(prefix+"gl33.png"),b.pixels,b.width,b.height);png(output/(prefix+"diff.png"),diff,a.width,a.height);
            break;
        }
        default:throw std::runtime_error("Unexpected GS trace operation");
        }
    }
    if(input.peek()!=std::char_traits<char>::eof()) throw std::runtime_error("Trailing data after GS trace footer");
    const auto stats=gpu.statistics();
    std::ofstream report(output/"report.json");
    report<<"{\n  \"version\": 1,\n  \"records\": "<<records<<",\n  \"recorded_draws\": "<<draws<<",\n  \"gpu_draws\": "<<gpu.gpuDraws()<<",\n  \"gpu_submissions\": "<<stats.submissions<<",\n  \"surface_copies\": "<<stats.surfaceCopies<<",\n  \"surface_readbacks\": "<<stats.surfaceReadbacks<<",\n  \"texture_uploads\": "<<stats.textureUploads<<",\n  \"cpu_raster_draws_in_gpu_backend\": 0,\n  \"frames\": "<<frames<<",\n  \"nonempty_frames\": "<<nonemptyFrames<<",\n  \"failed_frames\": "<<badFrames<<",\n  \"worst_rgb_rmse\": "<<worstRmse<<",\n  \"maximum_channel_delta\": "<<maximumDelta<<",\n  \"total_pixels_above_delta_8\": "<<totalBadPixels<<",\n  \"maximum_pixels_above_delta_8_per_frame\": "<<maxBadPixels<<",\n  \"allowed_bad_pixel_fraction\": 0.001,\n  \"allowed_rgb_rmse\": 1.5\n}\n";
    std::cout<<"Replayed "<<records<<" records, "<<draws<<" draws, "<<frames<<" frames; failed frames="<<badFrames<<'\n';
    return !closed || !draws || !gpu.gpuDraws() || !nonemptyFrames || badFrames ? 1:0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
