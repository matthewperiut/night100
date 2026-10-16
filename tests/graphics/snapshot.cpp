#include "graphics/gl33_backend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "runtime/state_archive.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

namespace {
constexpr uint32_t size=4*1024*1024;
unsigned checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
std::vector<uint32_t> decode(GSCpuBackend& backend,const GSTex0Reg& tex0,uint32_t width=1) {
    GSDrawState state{};state.context.tex0=tex0;state.texa={0,false,128};state.textureWidth=width;
    std::vector<uint32_t> output;backend.DecodeTexture(state,width,1,output);return output;
}
GSContext context(){GSContext c{};c.frame={0,1,GS_PSM_CT32,0};c.zbuf={128,GS_PSM_Z32,true};c.scissor={0,63,0,63};return c;}
NativeScreenDraw triangle() {
    NativeScreenDraw d;d.vertexAlpha=false;d.vertices.resize(3);
    const float p[3][2]={{2,2},{30,2},{2,30}};
    for(unsigned i=0;i<3;++i){d.vertices[i].position={p[i][0],p[i][1],32768,1};d.vertices[i].color={240,80,40,255};}
    return d;
}
void initializeFrontend(GS& gs,std::vector<uint8_t>& ram,GSRegisters* regs=nullptr) {
    gs.init(ram.data(),size,regs);gs.writeRegister(GS_REG_FRAME_1,1ull<<16);
    gs.writeRegister(GS_REG_ZBUF_1,128|(1ull<<32));gs.writeRegister(GS_REG_TEST_1,1ull<<17);gs.writeRegister(GS_REG_SCISSOR_1,(63ull<<16)|(63ull<<48));
}
uint64_t xyz(unsigned x,unsigned y){return (uint64_t(x)<<4)|(uint64_t(y)<<20)|(100ull<<32);}
}
int main() try {
    // A physical write does not refresh the intentionally stale texture page.
    // Restoring into a new CPU backend must preserve that stale page exactly.
    {
        std::vector<uint8_t> ram(size);GSCpuBackend original;original.Initialize(ram.data(),size);
        GSTex0Reg tex{};tex.tbp0=0x2000;tex.tbw=1;tex.psm=GS_PSM_CT32;tex.tcc=1;
        original.WriteVram(tex.psm,tex.tbp0,1,0,0,0x80112233);check(decode(original,tex)[0]==0x80112233,"warm original texture page");
        original.WriteVram(tex.psm,tex.tbp0,1,0,0,0x80445566);
        ps2x::state::Writer saved;original.saveState(saved);
        auto restoredRam=ram;GSCpuBackend restored;restored.Initialize(restoredRam.data(),size);
        ps2x::state::Reader reader(saved.data);restored.loadState(reader);reader.finish();
        check(decode(restored,tex)[0]==0x80112233,"restored stale texture page survives physical overwrite");
        restored.TextureFlush();check(decode(restored,tex)[0]==0x80445566,"TEXFLUSH exposes restored canonical VRAM");
    }
    // CLD conditional loads use both the palette contents and remembered CBP.
    {
        std::vector<uint8_t> ram(size);GSCpuBackend original;original.Initialize(ram.data(),size);
        GSTex0Reg tex{};tex.tbp0=0x2000;tex.tbw=2;tex.psm=GS_PSM_T8;tex.cbp=0x3000;tex.cpsm=GS_PSM_CT32;tex.cld=2;tex.tcc=1;
        original.WriteVram(GS_PSM_CT32,tex.cbp,1,0,0,0x80112233);original.LoadClut(tex,{});
        original.WriteVram(GS_PSM_CT32,tex.cbp,1,0,0,0x80445566);
        ps2x::state::Writer saved;original.saveState(saved);auto restoredRam=ram;
        GSCpuBackend restored;restored.Initialize(restoredRam.data(),size);ps2x::state::Reader reader(saved.data);restored.loadState(reader);reader.finish();
        tex.cld=4;restored.LoadClut(tex,{});check(decode(restored,tex)[0]==0x80112233,"restored conditional CLUT load retains old palette");
        tex.cld=1;restored.LoadClut(tex,{});check(decode(restored,tex)[0]==0x80445566,"explicit CLUT reload exposes newer palette");
    }
    // Continue an incomplete CT24 host upload without disturbing the shared
    // physical alpha lane, then restore a partially consumed local readback.
    {
        std::vector<uint8_t> ram(size);GSCpuBackend original;original.Initialize(ram.data(),size);
        for(unsigned x=0;x<4;++x)original.WriteVram(GS_PSM_CT32,0x1000,1,x,0,0xaa000000);
        GSTransferCommand transfer{};transfer.direction=0;transfer.bitbltbuf={0,1,GS_PSM_CT32,0x1000,1,GS_PSM_CT24};transfer.trxreg={4,1};
        const uint8_t bytes[]={1,2,3,4,5,6,7,8,9,10,11,12};original.BeginTransfer(transfer);original.UploadImage(bytes,6);
        ps2x::state::Writer saved;original.saveState(saved);auto restoredRam=ram;GSCpuBackend restored;restored.Initialize(restoredRam.data(),size);
        ps2x::state::Reader reader(saved.data);restored.loadState(reader);reader.finish();original.UploadImage(bytes+6,6);restored.UploadImage(bytes+6,6);
        check(ram==restoredRam,"partial image transfer resumes at the saved pixel");
        check(restored.ReadVram(GS_PSM_CT32,0x1000,1,3,0)==0xaa0c0b0a,"restored CT24 upload preserves neighboring alpha");
        transfer.direction=1;transfer.bitbltbuf={0x1000,1,GS_PSM_CT32,0,1,GS_PSM_CT32};original.BeginTransfer(transfer);
        uint8_t prefix[3];check(original.ConsumeLocalToHostBytes(prefix,3)==3,"consume local transfer prefix");
        ps2x::state::Writer local;original.saveState(local);ps2x::state::Reader localReader(local.data);restored.loadState(localReader);localReader.finish();
        uint8_t first[16]{},second[16]{};const auto n=original.ConsumeLocalToHostBytes(first,16),m=restored.ConsumeLocalToHostBytes(second,16);
        check(n==13 && m==n && !std::memcmp(first,second,n),"local readback buffer and byte cursor survive restore");
    }
    // Two vertices and all current attributes are live frontend state; writing
    // the third vertex after restore must emit the same original triangle.
    {
        std::vector<uint8_t> ram(size);GS original;initializeFrontend(original,ram);
        original.writeRegister(GS_REG_PRIM,GS_PRIM_TRIANGLE);original.writeRegister(GS_REG_RGBAQ,0x3f80000080654321ull);
        original.writeRegister(GS_REG_XYZ2,xyz(2,2));original.writeRegister(GS_REG_XYZ2,xyz(30,2));
        original.prepareStateSave();ps2x::state::Writer saved;original.state(saved);auto restoredRam=ram;
        GS restored;initializeFrontend(restored,restoredRam);ps2x::state::Reader reader(saved.data);restored.state(reader);reader.finish();
        original.writeRegister(GS_REG_XYZ2,xyz(2,30));restored.writeRegister(GS_REG_XYZ2,xyz(2,30));
        check(ram==restoredRam,"frontend partial primitive resumes identically");
        check(restored.ReadVram(GS_PSM_CT32,0,1,4,4)==0x80654321,"restored current attributes shade the triangle");
        auto incompatible=saved.data;incompatible[16]^=1; // frontend codec schema, after vector count and tag
        restored.writeRegister(GS_REG_FRAME_1,5|(1ull<<16));bool rejected=false;
        try{ps2x::state::Reader wrong(incompatible);restored.state(wrong);}catch(const std::exception&){rejected=true;}
        check(rejected && restored.getContextFrame(0).fbp==5,"incompatible GS ABI rejects before frontend mutation");
    }
    // Persist the completed frame as pixels, not a process-local GPU lease.
    {
        std::vector<uint8_t> ram(size);GSRegisters regs{};regs.pmode=1;regs.dispfb1=1ull<<9;regs.display1=(63ull<<32)|(63ull<<44);
        GS original;initializeFrontend(original,ram,&regs);original.clearFramebufferContext(0,0x80557799);original.latchHostPresentationFrame();
        std::vector<uint8_t> expected,actual;uint32_t ew=0,eh=0,aw=0,ah=0;
        original.copyLatchedHostPresentationFrame(expected,ew,eh);original.prepareStateSave();ps2x::state::Writer saved;original.state(saved);
        auto restoredRam=ram;GSRegisters newRegs{};GS restored;initializeFrontend(restored,restoredRam,&newRegs);
        ps2x::state::Reader reader(saved.data);restored.state(reader);reader.finish();
        check(restored.copyLatchedHostPresentationFrame(actual,aw,ah) && ew==aw && eh==ah && expected==actual,"completed image is available before resumed rendering");
    }
    // A dirty GPU timeline must never flush over memory restored by the outer
    // machine loader. Save drains once; load discards and lazily reseeds.
    {
        std::vector<uint8_t> ram(size);GL33Backend backend;backend.Initialize(ram.data(),size);const auto c=context();
        backend.ClearFramebuffer(c,0x80112233);backend.DrawNativeScreen(c,triangle());backend.prepareStateSave();
        const auto before=backend.statistics().surfaceReadbacks;ps2x::state::Writer saved;backend.saveState(saved);auto restoredRam=ram;
        check(before==1 && backend.statistics().surfaceReadbacks==before,"GPU snapshot drains target only once");
        backend.ClearFramebuffer(c,0x807799bb);backend.DrawNativeScreen(c,triangle());
        std::copy(restoredRam.begin(),restoredRam.end(),ram.begin());ps2x::state::Reader reader(saved.data);backend.loadState(reader);reader.finish();
        check(ram==restoredRam && backend.statistics().surfaceReadbacks==before,"GPU restore discards dirty old timeline without readback");
        check(backend.ReadVram(GS_PSM_CT32,0,1,4,4)==0x802850f0,"GPU restore reads canonical saved pixels");
        backend.DrawNativeScreen(c,triangle());backend.Flush();check(!backend.hasFailed(),"native GPU renderer rebuilds after restore");
    }
    {
        std::vector<uint8_t> ram(size);GL33Backend backend;backend.Initialize(ram.data(),size);backend.QueueNativeScreen(triangle());
        bool rejected=false;try{backend.prepareStateSave();}catch(const std::exception&){rejected=true;}
        check(rejected && !backend.hasFailed(),"unconsumed native command defers snapshot without poisoning renderer");
    }
    // A valid multi-page decoded GL texture can retain an older prefix than
    // the CPU decoder's final page. Restore that upload verbatim, not by decode.
    {
        std::vector<uint8_t> ram(size);GL33Backend original;original.Initialize(ram.data(),size);
        original.WriteVram(GS_PSM_CT32,0x2000,2,0,0,0x80112233);
        GSPrimitiveBatch offscreen{};offscreen.vertexCount=2;offscreen.state.context=context();offscreen.state.context.test=1ull<<17;
        offscreen.state.prim.type=GS_PRIM_SPRITE;offscreen.state.prim.tme=offscreen.state.prim.fst=true;
        offscreen.state.context.tex0.tbp0=0x2000;offscreen.state.context.tex0.tbw=2;offscreen.state.context.tex0.psm=GS_PSM_CT32;
        offscreen.state.context.tex0.tcc=1;offscreen.state.context.tex0.tfx=1;offscreen.state.textureWidth=offscreen.state.textureHeight=1;
        offscreen.vertices[0].x=100;offscreen.vertices[0].y=100;offscreen.vertices[1].x=116;offscreen.vertices[1].y=116;
        original.Submit(offscreen); // warms the first physical texture page
        original.WriteVram(GS_PSM_CT32,0x2000,2,0,0,0x80445566);
        offscreen.state.textureWidth=128;offscreen.state.textureHeight=64;original.Submit(offscreen);
        original.prepareStateSave();ps2x::state::Writer saved;original.saveState(saved);auto restoredRam=ram;
        GL33Backend restored;restored.Initialize(restoredRam.data(),size);ps2x::state::Reader reader(saved.data);restored.loadState(reader);reader.finish();
        auto visible=offscreen;visible.vertices[0].x=visible.vertices[0].y=2;visible.vertices[1].x=visible.vertices[1].y=18;
        original.Submit(visible);restored.Submit(visible);
        check(original.ReadVram(GS_PSM_CT32,0,1,4,4)==0x80112233,"original decoded GL cache contains intentionally stale prefix");
        check(restored.ReadVram(GS_PSM_CT32,0,1,4,4)==0x80112233,"restored decoded GL upload preserves stale prefix exactly");
    }
    {
        struct ShareParent {
            CGLContextObj previous=CGLGetCurrentContext(),value=nullptr;
            ShareParent() {
                CGLPixelFormatAttribute attributes[]={kCGLPFAOpenGLProfile,static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_3_2_Core),kCGLPFAAccelerated,static_cast<CGLPixelFormatAttribute>(0)};
                CGLPixelFormatObj pixel=nullptr;GLint count=0;
                if(CGLChoosePixelFormat(attributes,&pixel,&count)!=kCGLNoError || !pixel)throw std::runtime_error("No snapshot test share context");
                const auto error=CGLCreateContext(pixel,nullptr,&value);CGLDestroyPixelFormat(pixel);
                if(error!=kCGLNoError || CGLSetCurrentContext(value)!=kCGLNoError)throw std::runtime_error("Cannot bind snapshot share context");
            }
            ~ShareParent(){CGLSetCurrentContext(previous);CGLDestroyContext(value);}
        } share;
        std::vector<uint8_t> ram(size);GSRegisters regs{};regs.pmode=1;regs.dispfb1=1ull<<9;regs.display1=(63ull<<32)|(63ull<<44);
        GS original;initializeFrontend(original,ram,&regs);original.setRasterBackend(std::make_unique<GL33Backend>());
        original.clearFramebufferContext(0,0x80557799);original.latchHostPresentationFrame(true);
        uint32_t w=0,h=0;auto lease=original.latchedHostPresentationTexture(w,h);check(lease && w==64 && h==64,"default completed source is a shared GPU lease");
        original.prepareStateSave();ps2x::state::Writer saved;original.state(saved);auto restoredRam=ram;
        GS restored;initializeFrontend(restored,restoredRam,&regs);restored.setRasterBackend(std::make_unique<GL33Backend>());
        ps2x::state::Reader reader(saved.data);restored.state(reader);reader.finish();
        std::vector<uint8_t> first,second;uint32_t ew=0,eh=0,aw=0,ah=0;
        original.copyLatchedHostPresentationFrame(first,ew,eh);
        check(restored.copyLatchedHostPresentationFrame(second,aw,ah) && first==second && ew==aw && eh==ah,"shared GPU completed image survives persistent CPU materialization");
        check(!restored.latchedHostPresentationTexture(w,h),"restored presentation contains no process-local GPU lease");
        {
            // F11 enables timing after an otherwise untimed game. Completion
            // must end queries in its producer context, never the caller's
            // current window context (or the game thread's null context).
            GL33Backend timed;std::vector<uint8_t> timedRam(size);timed.Initialize(timedRam.data(),size);timed.setTimingEnabled(true);
            GSPresentationRequest request{};request.pmode=1;request.dispfb1=1ull<<9;request.display1=(63ull<<32)|(63ull<<44);
            for(unsigned i=0;i<4;++i) {
                timed.DrawNativeScreen(context(),triangle());auto frame=timed.PresentCompleted(request,true);
                check(frame.gpu && glGetError()==GL_NO_ERROR,"completed GPU timing preserves caller context and closes producer query");
                if(i==1){timed.prepareStateSave();ps2x::state::Writer checkpoint;timed.saveState(checkpoint);}
            }
            timed.Flush();timed.setTimingEnabled(false);check(!timed.hasFailed(),"recording snapshot continues with GPU timing enabled");
        }
        {
            // Explicit pacing ends only the currently submitted interval. A
            // repeated SDK polling continuation must not allocate more queries
            // while asleep; final resolve/composition gets its own interval.
            GL33Backend timed;std::vector<uint8_t> timedRam(size);timed.Initialize(timedRam.data(),size);timed.setTimingEnabled(true);
            GSPresentationRequest request{};request.pmode=1;request.dispfb1=1ull<<9;request.display1=(63ull<<32)|(63ull<<44);
            timed.DrawNativeScreen(context(),triangle());timed.finishTimingInterval();
            for(unsigned i=0;i<128;++i)timed.finishTimingInterval();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            auto frame=timed.PresentCompleted(request,true);std::vector<uint8_t> pixels;
            check(frame.gpu && frame.gpu->readPixels(pixels),"timing boundary preserves completed GPU image");
            // The synchronous diagnostic read above completes both submitted
            // intervals; the next begin polls them without waiting itself.
            auto next=timed.PresentCompleted(request,true);const auto after=timed.statistics();
            check(!after.gpuTimingSupported || (after.gpuSamples>=2 && after.gpuSamples<=3),"host pacing closes two bounded intervals without query flooding");
            check(timed.ReadVram(GS_PSM_CT32,0,1,4,4)!=0 && !timed.hasFailed() && glGetError()==GL_NO_ERROR,"timing boundary preserves rendering and caller context");
            std::cout<<"GPU timing pacing probe: "<<after.gpuSamples<<" delayed intervals, "<<double(after.gpuNanoseconds)*1e-6<<" ms total (30 ms host sleep excluded by submitted boundaries)\n";
        }

    }
    std::cout<<"PASS "<<checks<<" graphics snapshot continuation checks\n";return 0;
} catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
