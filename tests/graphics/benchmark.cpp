// Measures one fixed recorded workload without the CPU reference or image export.
#include "graphics/gl33_backend.h"
#include "graphics/gs_trace.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <zlib.h>
namespace {
class Input {
    gzFile file;
public:
    explicit Input(const char* path):file(gzopen(path,"rb")) {if(!file)throw std::runtime_error("Cannot open GS trace");gzbuffer(file,128*1024);}
    ~Input(){gzclose(file);}
    void read(void* p,size_t n){if(gzread(file,p,unsigned(n))!=int(n))throw std::runtime_error("Truncated GS trace");}
    void end(){int value=gzgetc(file),code=0;gzerror(file,&code);if(value!=-1 || (code!=Z_OK && code!=Z_STREAM_END))throw std::runtime_error("Invalid GS trace footer");}
};
template<class T>T decode(const std::vector<uint8_t>& p){if(p.size()!=sizeof(T))throw std::runtime_error("Invalid GS payload size");T value;std::memcpy(&value,p.data(),sizeof(T));return value;}
}
int main(int argc,char** argv) {
try {
    if(argc!=2)throw std::runtime_error("Usage: graphics_benchmark TRACE");
    Input input(argv[1]);GSTrace::Header header{};input.read(&header,sizeof(header));
    if(std::memcmp(header.magic,"N100GS\r\n",8) || header.version!=1 || header.batchSize!=sizeof(GSPrimitiveBatch) || header.contextSize!=sizeof(GSContext) || header.presentationSize!=sizeof(GSPresentationRequest))throw std::runtime_error("Incompatible GS trace");
    GL33Backend gpu;std::vector<uint8_t> ram,payload;uint64_t records=0,commands=0,frames=0,nonempty=0;bool initialized=false;
    std::cerr<<"GPU: "<<gpu.device()<<'\n';
    const auto start=std::chrono::steady_clock::now();
    for(;;){
        GSTrace::Record record{};input.read(&record,sizeof(record));
        if(record.size>4*1024*1024 || uint32_t(record.op)>uint32_t(GSTrace::Op::Write))throw std::runtime_error("Invalid GS record");
        payload.resize(record.size);if(record.size)input.read(payload.data(),payload.size());++records;
        using GSTrace::Op;
        if(record.op==Op::End){if(record.size)throw std::runtime_error("Invalid GS footer");break;}
        if(record.op!=Op::Initialize && !initialized)throw std::runtime_error("GS command before initialization");
        switch(record.op){
        case Op::Initialize:if(payload.size()!=4*1024*1024)throw std::runtime_error("Expected complete VRAM");ram=payload;gpu.Initialize(ram.data(),ram.size());initialized=true;break;
        case Op::Reset:gpu.Reset();break;
        case Op::Submit:gpu.Submit(decode<GSPrimitiveBatch>(payload));++commands;break;
        case Op::Clut:{auto c=decode<GSTrace::Clut>(payload);gpu.LoadClut(c.tex,c.clut);break;}
        case Op::Transfer:gpu.BeginTransfer(decode<GSTransferCommand>(payload));break;
        case Op::Upload:gpu.UploadImage(payload.data(),payload.size());break;
        case Op::Flush:gpu.Flush();break;
        case Op::TextureFlush:gpu.TextureFlush();break;
        case Op::Sync:gpu.Sync(decode<GSSyncReason>(payload));break;
        case Op::Present:{auto frame=gpu.Present(decode<GSPresentationRequest>(payload));++frames;if(frame)++nonempty;break;}
        case Op::Clear:{auto c=decode<GSTrace::Clear>(payload);if(!gpu.ClearFramebuffer(c.context,c.rgba))throw std::runtime_error("Unsupported clear");break;}
        case Op::Consume:{auto n=decode<uint32_t>(payload);if(n>4*1024*1024)throw std::runtime_error("Invalid read size");std::vector<uint8_t> data(n);gpu.ConsumeLocalToHostBytes(data.data(),n);break;}
        case Op::Write:{auto w=decode<GSTrace::Write>(payload);gpu.WriteVram(w.psm,w.base,w.bw,w.x,w.y,w.value);break;}
        default:throw std::runtime_error("Invalid GS operation");
        }
    }
    input.end();gpu.Flush(); // Readback completes pending GPU work before timing ends.
    const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    const auto stats=gpu.statistics();
    if(!commands || !nonempty || gpu.hasFailed())throw std::runtime_error("Incomplete GS workload");
    std::cout<<"{\"seconds\":"<<seconds<<",\"records\":"<<records<<",\"commands\":"<<commands<<",\"presentations\":"<<frames<<",\"gpu_primitives\":"<<stats.primitives<<",\"submissions\":"<<stats.submissions<<",\"copies\":"<<stats.surfaceCopies<<",\"readbacks\":"<<stats.surfaceReadbacks<<",\"texture_uploads\":"<<stats.textureUploads<<"}\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
