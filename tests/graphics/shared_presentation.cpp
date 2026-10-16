#include "graphics/gl33_backend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include <raylib.h>
#include <rlgl.h>

#include <array>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
GSPrimitiveBatch rectangle(float x,float y,float width,float height,uint32_t color) {
    GSPrimitiveBatch batch{};batch.vertexCount=2;batch.state.prim.type=GS_PRIM_SPRITE;
    auto& c=batch.state.context;c.frame.fbw=10;c.zbuf.zbp=280;c.zbuf.psm=GS_PSM_Z32;c.zbuf.zmask=true;
    c.scissor={0,639,0,447};c.test=uint64_t(1)<<17;
    batch.vertices[0].x=x;batch.vertices[0].y=y;batch.vertices[1].x=x+width;batch.vertices[1].y=y+height;
    for(auto& v:batch.vertices){v.r=color;v.g=color>>8;v.b=color>>16;v.a=color>>24;}
    return batch;
}
void windowDraw(const std::shared_ptr<GSGpuPresentation>& image,const PresentationFrame& expected,bool capture) {
    BeginDrawing();ClearBackground(BLACK);image->waitForDraw();
    const Texture2D texture{image->textureId(),int(image->textureWidth()),int(image->textureHeight()),1,PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    DrawTexturePro(texture,{0,0,float(expected.width),float(expected.height)},
                   {0,0,float(GetScreenWidth()),float(GetScreenHeight())},{0,0},0,WHITE);
    rlDrawRenderBatchActive();
    if(capture) {
        Image screenshot=LoadImageFromScreen();
        require(screenshot.format==PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,"window readback format changed");
        const auto* pixels=static_cast<const uint8_t*>(screenshot.data);bool matches=true;
        for(unsigned y:{0u,1u,6u,7u,99u,100u,198u,199u,300u,446u})
        for(unsigned x:{0u,1u,31u,99u,100u,198u,199u,300u,639u}) {
            const unsigned sx=std::min(unsigned(screenshot.width-1),unsigned((double(x)+.5)*screenshot.width/expected.width));
            const unsigned sy=std::min(unsigned(screenshot.height-1),unsigned((double(y)+.5)*screenshot.height/expected.height));
            uint32_t got=0,want=0;std::memcpy(&got,pixels+(size_t(sy)*screenshot.width+sx)*4,4);
            std::memcpy(&want,expected.pixels.data()+(size_t(y)*640+x)*4,4);
            if(got!=want){std::cerr<<"screen pixel "<<x<<','<<y<<" got="<<std::hex<<got<<" want="<<want<<std::dec<<'\n';matches=false;break;}
        }
        UnloadImage(screenshot);require(matches,"shared GPU image has wrong window orientation, origin or color");
    }
    image->finishDraw();EndDrawing();
}
}
int main(){try{
    SetTraceLogLevel(LOG_WARNING);SetConfigFlags(FLAG_WINDOW_HIDDEN);InitWindow(640,447,"Night100 shared completed presentation regression");
    require(IsWindowReady(),"real Raylib window failed to initialize");
    std::vector<uint8_t> gpuRam(4*1024*1024),cpuRam(gpuRam.size());GSCpuBackend cpu;cpu.Initialize(cpuRam.data(),cpuRam.size());
    auto gpu=std::make_unique<GL33Backend>();gpu->Initialize(gpuRam.data(),gpuRam.size());
    for(const auto& draw:std::array<GSPrimitiveBatch,4>{{rectangle(0,0,640,448,0x80302010),rectangle(0,0,640,8,0x801414f0),
            rectangle(0,440,640,8,0x80f01414),rectangle(100,100,100,100,0x8014e614)}}){cpu.Submit(draw);gpu->Submit(draw);}
    GSPresentationRequest request{};request.pmode=0x8067;request.dispfb1=request.dispfb2=(10ull<<9)|(1ull<<43);
    request.display1=request.display2=(639ull<<32)|(446ull<<44);
    const auto expected=cpu.Present(request);require(expected && expected.width==640 && expected.height==447,"CPU window oracle did not cover actual retail display layout");
    const auto readsBefore=gpu->statistics().surfaceReadbacks;
    auto first=gpu->PresentCompleted(request,true),second=gpu->PresentCompleted(request,true),third=gpu->PresentCompleted(request,true);
    require(first.gpu && second.gpu && third.gpu && first.pixels.empty(),"real window/private CGL contexts did not share GPU completed frames");
    auto busy=gpu->PresentCompleted(request,true);require(busy.keepPrevious && !busy.gpu,"bounded presentation ring overwrote a live image or used CPU fallback");
    require(gpu->statistics().surfaceReadbacks==readsBefore,"ordinary shared presentation performed CPU surface readback");
    windowDraw(first.gpu,expected,true);
    std::vector<uint8_t> diagnostic;require(first.gpu->readPixels(diagnostic) && diagnostic==expected.pixels,"lazy GPU diagnostic differs from CPU presentation oracle");
    // Submit a consumer without CPU readback, release its lease, then reuse its
    // slot after a changed frame. Reuse must wait for the consumer on the GPU.
    windowDraw(second.gpu,expected,false);second.gpu.reset();third.gpu.reset();
    const auto replacement=rectangle(0,0,640,448,0x803c78b4);cpu.Submit(replacement);gpu->Submit(replacement);
    const auto changedExpected=cpu.Present(request);auto changed=gpu->PresentCompleted(request,true);
    require(bool(changed.gpu),"released image slot was not reusable");windowDraw(changed.gpu,changedExpected,true);
    require(first.gpu->readPixels(diagnostic) && diagnostic==expected.pixels,"producer changed an earlier immutable completed image");
    auto retained=first.gpu;first.gpu.reset();changed.gpu.reset();gpu.reset();
    require(retained->readPixels(diagnostic) && diagnostic==expected.pixels,"completed image lost its retained private context after backend destruction");
    windowDraw(retained,expected,true);CloseWindow();
    require(retained->readPixels(diagnostic) && diagnostic==expected.pixels,"post-run diagnostic lost its GPU image after window/context destruction");
    retained.reset();
    std::cout<<"PASS: real Raylib/CGL sharing, window orientation and origins, bounded GPU ring, consumer fences, lazy diagnostics and backend-independent image lifetime\n";
    return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';if(IsWindowReady())CloseWindow();return 1;}}
