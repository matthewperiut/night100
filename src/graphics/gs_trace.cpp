#include "gs_trace.h"
#include <cstring>
#include <zlib.h>
#include <mutex>
#include <stdexcept>
namespace GSTrace {
namespace {
class Recorder final : public GSRasterBackend {
    std::unique_ptr<GSRasterBackend> inner;
    mutable std::mutex mutex;
    mutable gzFile stream=nullptr;
    mutable uint64_t bytes=0;
    void put(Op op,const void* data,uint32_t size) const {
        if (bytes+sizeof(Record)+size>1024ull*1024*1024) throw std::runtime_error("GS trace exceeded 1 GiB; shorten the capture");
        Record r{op,size};
        if(gzwrite(stream,&r,sizeof(r))!=sizeof(r) || (size && gzwrite(stream,data,size)!=int(size))) throw std::runtime_error("Cannot write GS trace");
        bytes+=sizeof(r)+size;
    }
    template<class T> void put(Op op,const T& value) const {put(op,&value,sizeof(value));}
public:
    Recorder(std::unique_ptr<GSRasterBackend> backend,const std::string& path):inner(std::move(backend)),stream(gzopen(path.c_str(),"wb1")) {
        if(!stream) throw std::runtime_error("Cannot open GS trace: "+path);
        gzbuffer(stream,128*1024);
        Header h{{'N','1','0','0','G','S','\r','\n'},1,sizeof(GSPrimitiveBatch),sizeof(GSContext),sizeof(GSPresentationRequest)};
        if(gzwrite(stream,&h,sizeof(h))!=sizeof(h)) {gzclose(stream);stream=nullptr;throw std::runtime_error("Cannot write GS trace header");}
    }
    ~Recorder() override {if(stream){try{put(Op::End,nullptr,0);}catch(...) {}gzclose(stream);}}
    void Initialize(uint8_t* p,uint32_t size) override {std::lock_guard lock(mutex);put(Op::Initialize,p,p?size:0);inner->Initialize(p,size);}
    void Reset() override {std::lock_guard lock(mutex);put(Op::Reset,nullptr,0);inner->Reset();}
    void Submit(const GSPrimitiveBatch& b) override {std::lock_guard lock(mutex);put(Op::Submit,b);inner->Submit(b);}
    void LoadClut(const GSTex0Reg& t,const GSTexClutReg& c) override {std::lock_guard lock(mutex);put(Op::Clut,Clut{t,c});inner->LoadClut(t,c);}
    void BeginTransfer(const GSTransferCommand& c) override {std::lock_guard lock(mutex);put(Op::Transfer,c);inner->BeginTransfer(c);}
    void UploadImage(const uint8_t* p,uint32_t n) override {std::lock_guard lock(mutex);put(Op::Upload,p,n);inner->UploadImage(p,n);}
    void Flush() override {std::lock_guard lock(mutex);put(Op::Flush,nullptr,0);inner->Flush();}
    void TextureFlush() override {std::lock_guard lock(mutex);put(Op::TextureFlush,nullptr,0);inner->TextureFlush();}
    void Sync(GSSyncReason r) override {std::lock_guard lock(mutex);put(Op::Sync,r);inner->Sync(r);}
    PresentationFrame Present(const GSPresentationRequest& r) override {std::lock_guard lock(mutex);put(Op::Present,r);auto frame=inner->Present(r);if(gzflush(stream,Z_SYNC_FLUSH)!=Z_OK) throw std::runtime_error("Cannot flush GS trace");return frame;}
    bool ClearFramebuffer(const GSContext& c,uint32_t rgba) override {std::lock_guard lock(mutex);put(Op::Clear,Clear{c,rgba});return inner->ClearFramebuffer(c,rgba);}
    uint32_t ConsumeLocalToHostBytes(uint8_t* p,uint32_t n) override {std::lock_guard lock(mutex);put(Op::Consume,n);return inner->ConsumeLocalToHostBytes(p,n);}
    uint32_t ReadVram(uint32_t p,uint32_t b,uint32_t w,uint32_t x,uint32_t y) const override {std::lock_guard lock(mutex);return inner->ReadVram(p,b,w,x,y);}
    void WriteVram(uint32_t p,uint32_t b,uint32_t w,uint32_t x,uint32_t y,uint32_t v) override {std::lock_guard lock(mutex);put(Op::Write,Write{p,b,w,x,y,v});inner->WriteVram(p,b,w,x,y,v);}
    void SnapshotVram(std::vector<uint8_t>& out) const override {std::lock_guard lock(mutex);inner->SnapshotVram(out);}
    GSTransferSnapshot GetTransferSnapshot() const override {std::lock_guard lock(mutex);return inner->GetTransferSnapshot();}
};
}
std::unique_ptr<GSRasterBackend> record(std::unique_ptr<GSRasterBackend> inner,const std::string& path) {return std::make_unique<Recorder>(std::move(inner),path);}
}
