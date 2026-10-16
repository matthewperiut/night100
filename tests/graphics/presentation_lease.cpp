#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition,const char* reason) {if(!condition)throw std::runtime_error(reason);}
struct Image final : GSGpuPresentation {
    uint32_t id;mutable unsigned reads=0,draws=0,finishes=0;bool fail=false;
    std::vector<uint8_t> pixels=std::vector<uint8_t>(640*512*4);
    explicit Image(uint32_t value):id(value) {
        for(unsigned y=0;y<2;++y)for(unsigned x=0;x<3;++x) {
            const uint32_t rgba=0xff000000u|value|(x<<8)|(y<<16);
            std::memcpy(pixels.data()+(y*640+x)*4,&rgba,4);
        }
    }
    uint32_t textureId() const override{return id;}
    uint32_t textureWidth() const override{return 640;}
    uint32_t textureHeight() const override{return 512;}
    void waitForDraw() const override{++draws;}
    void finishDraw() const override{++finishes;}
    bool readPixels(std::vector<uint8_t>& out) const override{++reads;if(fail)return false;out=pixels;return true;}
};
struct Backend final : GSRasterBackend {
    PresentationFrame pending;std::string calls;bool reference=false,lastAuthoritative=false;
    void Initialize(uint8_t*,uint32_t) override{}
    void Reset() override{}
    void Submit(const GSPrimitiveBatch&) override{}
    void LoadClut(const GSTex0Reg&,const GSTexClutReg&) override{}
    void BeginTransfer(const GSTransferCommand&) override{}
    void UploadImage(const uint8_t*,uint32_t) override{}
    void Flush() override{calls+='F';}
    void TextureFlush() override{}
    void Sync(GSSyncReason reason) override{require(reason==GSSyncReason::Presentation || reason==GSSyncReason::Reset,"unexpected sync reason");calls+='S';}
    PresentationFrame Present(const GSPresentationRequest&) override{calls+='P';return pending;}
    PresentationFrame PresentCompleted(const GSPresentationRequest& request,bool authoritativeDisplay=false) override {
        lastAuthoritative=authoritativeDisplay;
        return reference?GSRasterBackend::PresentCompleted(request,authoritativeDisplay):pending;
    }
    bool ClearFramebuffer(const GSContext&,uint32_t) override{return false;}
    uint32_t ConsumeLocalToHostBytes(uint8_t*,uint32_t) override{return 0;}
    uint32_t ReadVram(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t) const override{return 0;}
    void WriteVram(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t) override{}
    void SnapshotVram(std::vector<uint8_t>& out) const override{out.clear();}
    GSTransferSnapshot GetTransferSnapshot() const override{return {};}
};
PresentationFrame frame(std::shared_ptr<Image> image) {
    PresentationFrame result;result.gpu=std::move(image);result.width=3;result.height=2;
    result.displayFbp=7;result.sourceFbp=9;result.usedPreferred=true;return result;
}
}
int main(){try{
    GS gs;GSRegisters registers{};std::vector<uint8_t> vram(4*1024*1024);
    gs.init(vram.data(),vram.size(),&registers);
    auto backend=std::make_unique<Backend>();auto* producer=backend.get();gs.setRasterBackend(std::move(backend));
    auto first=std::make_shared<Image>(17);producer->pending=frame(first);producer->calls.clear();
    gs.latchHostPresentationFrame(true);require(producer->lastAuthoritative,"native completed display policy did not reach producer");require(producer->calls.empty(),"GPU latch forced the reference readback boundary");
    uint32_t width=0,height=0;auto lease=gs.latchedHostPresentationTexture(width,height);
    require(lease==first && width==3 && height==2 && first->reads==0,"borrowing completed GPU image performed CPU readback or changed metadata");
    lease->waitForDraw();lease->finishDraw();require(first->draws==1 && first->finishes==1,"consumer lease lost synchronization hooks");
    std::vector<uint8_t> packed;uint32_t display=0,source=0;bool preferred=false;
    require(gs.copyLatchedHostPresentationFrame(packed,width,height,&display,&source,&preferred),"lazy diagnostic did not materialize GPU image");
    require(first->reads==1 && width==3 && height==2 && display==7 && source==9 && preferred && packed.size()==24,"lazy diagnostic changed metadata/layout");
    for(unsigned y=0;y<2;++y)for(unsigned x=0;x<3;++x){uint32_t rgba=0;std::memcpy(&rgba,packed.data()+(y*3+x)*4,4);require(rgba==(0xff000011u|(x<<8)|(y<<16)),"lazy diagnostic did not pack fixed-stride rows correctly");}
    require(gs.copyLatchedHostPresentationFrame(packed,width,height) && first->reads==1,"repeated diagnostic read the same immutable GPU image twice");
    producer->pending={};producer->pending.keepPrevious=true;gs.latchHostPresentationFrame();
    require(gs.latchedHostPresentationTexture(width,height)==first && width==3 && height==2,"busy bounded producer erased last completed frame");
    auto second=std::make_shared<Image>(23);producer->pending=frame(second);gs.latchHostPresentationFrame();
    require(gs.latchedHostPresentationTexture(width,height)==second && lease==first,"publishing next frame invalidated an outstanding consumer lease");
    require(gs.copyLatchedHostPresentationFrame(packed,width,height) && second->reads==1 && packed[0]==23,"next GPU frame reused old lazy diagnostic pixels");
    producer->reference=true;producer->pending={};producer->pending.width=3;producer->pending.height=2;
    producer->pending.pixels=std::vector<uint8_t>(640*512*4,42);producer->calls.clear();gs.latchHostPresentationFrame();
    require(!producer->lastAuthoritative,"native display policy leaked to a generic presentation");
    require(producer->calls=="FSP","reference completed latch changed Flush/Sync/Present ordering");
    require(!gs.latchedHostPresentationTexture(width,height) && gs.copyLatchedHostPresentationFrame(packed,width,height) && packed.size()==24 && packed[0]==42,"CPU fallback retained old GPU image or changed CPU layout");
    producer->reference=false;auto broken=std::make_shared<Image>(41);broken->fail=true;producer->pending=frame(broken);gs.latchHostPresentationFrame();
    require(!gs.copyLatchedHostPresentationFrame(packed,width,height) && packed.empty() && width==0 && height==0,"failed diagnostic returned pixels from a different completed frame");
    gs.reset();require(!gs.latchedHostPresentationTexture(width,height) && width==0 && height==0 && lease==first,"reset invalidated a live external image lease or kept old presentation");
    producer->pending={};producer->pending.keepPrevious=true;gs.latchHostPresentationFrame();
    require(!gs.latchedHostPresentationTexture(width,height),"keepPrevious invented a frame after reset");
    std::cout<<"PASS: GPU completed-frame leases, no eager readback, lazy packed diagnostics, retained busy frame, CPU fallback, reference barriers and reset\n";
    return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
