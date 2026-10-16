#include "graphics/gl33_backend.h"
#include "graphics/native_asset_cache.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_frontend.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

namespace {
constexpr uint32_t memorySize=4*1024*1024;
std::vector<uint8_t> cpuRam(memorySize),gpuRam(memorySize),expected,actual;
unsigned cases=0, failures=0;
GSCpuBackend cpu;
GL33Backend* gpu=nullptr;
GSPrimitiveBatch sprite() {
    GSPrimitiveBatch b{};b.vertexCount=2;b.state.prim.type=GS_PRIM_SPRITE;
    b.state.context.frame={0,1,GS_PSM_CT32,0};b.state.context.scissor={0,31,0,31};
    b.state.context.zbuf={32,GS_PSM_Z32,false};b.state.context.test=uint64_t(1)<<17;
    b.state.textureWidth=8;b.state.textureHeight=8;
    b.vertices[0].x=3;b.vertices[0].y=5;
    b.vertices[1].x=25;b.vertices[1].y=23;b.vertices[1].z=100;
    b.vertices[1].r=180;b.vertices[1].g=71;b.vertices[1].b=39;b.vertices[1].a=128;
    b.vertices[1].fog=180;
    return b;
}
void initialize(const GSPrimitiveBatch& b) {
    std::fill(cpuRam.begin(),cpuRam.end(),0);std::fill(gpuRam.begin(),gpuRam.end(),0);
    cpu.Initialize(cpuRam.data(),memorySize);gpu->Initialize(gpuRam.data(),memorySize);
    for (uint32_t y=0;y<32;++y) for(uint32_t x=0;x<64;++x) {
        const auto& c=b.state.context;
        uint32_t color=0x95704723u ^ ((x&7)<<16) ^ ((y&7)<<8);
        if (c.frame.psm==GS_PSM_CT16 || c.frame.psm==GS_PSM_CT16S) color=0xb963;
        cpu.WriteVram(c.frame.psm,c.frame.fbp<<5,1,x,y,color);
        cpu.WriteVram(c.zbuf.psm,c.zbuf.zbp<<5,1,x,y,50);
    }
    gpuRam=cpuRam;
}
bool compare(const std::string& name,const GSPrimitiveBatch& b,int tolerance=0) {
    cpu.SnapshotVram(expected);gpu->SnapshotVram(actual);
    ++cases;
    if (expected==actual) return true;
    if (tolerance) {
        bool okay=true;
        const auto& c=b.state.context;
        for(uint32_t y=0;y<32;++y) for(uint32_t x=0;x<64;++x) {
            uint32_t a=cpu.ReadVram(c.frame.psm,c.frame.fbp<<5,1,x,y);
            uint32_t z=gpu->ReadVram(c.frame.psm,c.frame.fbp<<5,1,x,y);
            for(int ch=0;ch<4;++ch) if (std::abs(int((a>>(8*ch))&255)-int((z>>(8*ch))&255))>tolerance) okay=false;
            uint32_t za=cpu.ReadVram(c.zbuf.psm,c.zbuf.zbp<<5,1,x,y),zb=gpu->ReadVram(c.zbuf.psm,c.zbuf.zbp<<5,1,x,y);
            if (std::abs(double(za)-double(zb))>tolerance) okay=false;
        }
        if(okay) return true;
    }
    ++failures;
    auto mismatch=std::mismatch(expected.begin(),expected.end(),actual.begin());
    const auto offset=size_t(mismatch.first-expected.begin());
    std::cerr<<"FAIL "<<name<<" byte="<<offset<<" cpu="<<int(*mismatch.first)<<" gl="<<int(*mismatch.second)<<'\n';
    if(failures<=3) {
        std::filesystem::create_directories("logs/gl-failures");
        std::ofstream("logs/gl-failures/"+std::to_string(failures)+"-cpu.bin",std::ios::binary).write(reinterpret_cast<const char*>(expected.data()),expected.size());
        std::ofstream("logs/gl-failures/"+std::to_string(failures)+"-gl.bin",std::ios::binary).write(reinterpret_cast<const char*>(actual.data()),actual.size());
    }
    return false;
}
void draw(const std::string& name,GSPrimitiveBatch b,int tolerance=0) {
    initialize(b);cpu.Submit(b);gpu->Submit(b);compare(name,b,tolerance);
}
void textures(GSPrimitiveBatch& b,uint8_t psm,uint8_t cpsm=GS_PSM_CT32) {
    auto& t=b.state.context.tex0;t.tbp0=0x2000;t.tbw=2;t.psm=psm;t.cbp=0x3000;t.cpsm=cpsm;t.cld=1;t.tcc=1;
    for(uint32_t y=0;y<8;++y) for(uint32_t x=0;x<8;++x) {
        uint32_t value=0xa0000000u|(x*27)|(y*23<<8)|((x+y)*13<<16);
        if(psm==2 || psm==10) value=0x8000|(x*3)|((y*3)<<5)|(((x+y)&31)<<10);
        if(psm==19 || psm==27) value=(y*8+x);
        if(psm==20 || psm==36 || psm==44) value=(x+y)&15;
        cpu.WriteVram(psm,t.tbp0,t.tbw,x,y,value);gpu->WriteVram(psm,t.tbp0,t.tbw,x,y,value);
    }
    for(uint32_t i=0;i<256;++i) {
        uint32_t value=cpsm==0 ? 0x80000000|(i*3&255)|((i*7&255)<<8)|((i*13&255)<<16) : 0x8000|(i&0x7fff);
        cpu.WriteVram(cpsm,t.cbp,1,i%16,i/16,value);gpu->WriteVram(cpsm,t.cbp,1,i%16,i/16,value);
    }
    cpu.LoadClut(t,b.state.texclut);gpu->LoadClut(t,b.state.texclut);
}
}
int main() {
try {
    GL33Backend backend;gpu=&backend;
    std::cout<<"GPU: "<<gpu->device()<<'\n';
    for(uint8_t psm : {0,1,2,10}) for(uint8_t zpsm : {48,49,50,58}) {
        auto b=sprite();b.state.context.frame.psm=psm;b.state.context.zbuf.psm=zpsm;
        draw("frame/depth formats",b);
        initialize(b);cpu.ClearFramebuffer(b.state.context,0x4981a5ed);gpu->ClearFramebuffer(b.state.context,0x4981a5ed);compare("clear formats",b);
    }
    for(int op=0;op<8;++op) for(int fail=0;fail<4;++fail) for(int alpha : {0,63,64,127,128,255}) {
        auto b=sprite();b.vertices[1].a=alpha;b.state.context.test|=1|(uint64_t(op)<<1)|(128<<4)|(uint64_t(fail)<<12);
        draw("alpha comparison/AFAIL",b);
    }
    for(int equation=0;equation<256;++equation) for(int fix : {0,64,128,255}) {
        auto b=sprite();b.state.prim.abe=true;b.state.context.alpha=uint64_t(equation)|(uint64_t(fix)<<32);
        draw("GS blend equation",b);
    }
    for(uint8_t psm : {0,1,2,10}) for(int date=0;date<4;++date) for(int ztest=0;ztest<4;++ztest) {
        auto b=sprite();b.state.context.frame.psm=psm;b.state.context.test=(uint64_t(date)<<14)|(uint64_t(ztest)<<17);
        draw("DATE/DATM and depth tests",b);
    }
    for(uint32_t mask : {0u,0xffu,0xff000000u,0x13579bdfu,0xffffffffu}) for(int fba=0;fba<2;++fba) for(int pabe=0;pabe<2;++pabe) {
        auto b=sprite();b.state.context.frame.fbmsk=mask;b.state.context.fba=fba;b.state.pabe=pabe;b.state.prim.abe=true;b.vertices[1].a=63;
        b.state.context.alpha=0x44;draw("FBMSK/FBA/PABE",b);
    }
    for(int fog : {0,1,63,128,254,255}) {
        auto b=sprite();b.state.prim.fge=true;b.state.fogR=32;b.state.fogG=80;b.state.fogB=255;b.vertices[1].fog=fog;draw("fog",b);
    }
    for(uint8_t psm : {0,1,2,10,19,20,27,36,44}) for(int linear=0;linear<2;++linear) for(int tfx=0;tfx<4;++tfx) {
        auto b=sprite();b.state.prim.tme=true;b.state.prim.fst=true;b.state.linearFilter=linear;b.vertices[1].u=8*16;b.vertices[1].v=8*16;
        initialize(b);textures(b,psm);b.state.context.tex0.tfx=tfx;
        cpu.Submit(b);gpu->Submit(b);compare("texture format/filter/TFX",b,linear ? 1 : 0);
    }
    for(int winding=0;winding<2;++winding) for(int smooth=0;smooth<2;++smooth) for(int textured=0;textured<2;++textured) {
        auto b=sprite();b.state.prim.type=GS_PRIM_TRIANGLE;b.vertexCount=3;b.state.prim.iip=smooth;b.state.prim.tme=textured;
        b.vertices[0].x=2.25;b.vertices[0].y=3.5;b.vertices[0].z=100;
        b.vertices[1].x=29.25;b.vertices[1].y=5.5;b.vertices[1].z=100;
        b.vertices[2].x=7.25;b.vertices[2].y=29.5;b.vertices[2].z=100;
        b.vertices[0].r=25;b.vertices[0].a=128;b.vertices[1].g=200;b.vertices[2].b=240;b.vertices[2].a=128;
        b.vertices[0].s=0;b.vertices[0].t=0;b.vertices[0].q=1;
        b.vertices[1].s=.5;b.vertices[1].t=0;b.vertices[1].q=.5;
        b.vertices[2].s=0;b.vertices[2].t=.25;b.vertices[2].q=.25;
        if(winding) std::swap(b.vertices[0],b.vertices[1]);
        initialize(b);if(textured) textures(b,GS_PSM_CT32);
        cpu.Submit(b);gpu->Submit(b);compare("triangle winding/Gouraud/perspective",b,2);
    }
    for(int type : {0,1,2}) {
        auto b=sprite();b.state.prim.type=GSPrimType(type);b.state.prim.iip=true;draw("point/line coverage",b);
    }
    for(int mode=0;mode<4;++mode) for(int linear=0;linear<2;++linear) for(int fixed=0;fixed<2;++fixed) {
        auto b=sprite();b.state.prim.tme=true;b.state.prim.fst=fixed;b.state.linearFilter=linear;
        b.state.context.clamp=uint64_t(mode)|(uint64_t(mode)<<2)|(2ull<<4)|(5ull<<14)|(1ull<<24)|(6ull<<34);
        b.vertices[0].u=0;b.vertices[0].v=0;b.vertices[1].u=16*16;b.vertices[1].v=16*16;
        b.vertices[0].s=-.25;b.vertices[0].t=-.25;b.vertices[1].s=2;b.vertices[1].t=2;
        initialize(b);textures(b,GS_PSM_CT32);cpu.Submit(b);gpu->Submit(b);compare("wrapping/region modes",b,linear?1:0);
    }
    for(int ox : {-40,-2,0,20,40}) for(int oy : {-40,-2,0,20,40}) {
        auto b=sprite();for(int i=0;i<2;++i){b.vertices[i].x+=ox;b.vertices[i].y+=oy;}
        draw("sprite scissor/offscreen",b);
    }
    for(uint32_t z : {0u,49u,50u,51u,0x7fffffffu,0x80000000u,0xfffffffeu,0xffffffffu}) for(int test=0;test<4;++test) {
        auto b=sprite();b.vertices[1].z=z;b.state.context.test=uint64_t(test)<<17;draw("unsigned 32-bit depth",b);
    }
    for(uint8_t cpsm : {0,1,2,10}) for(uint8_t psm : {19,20,27,36,44}) {
        auto b=sprite();b.state.prim.tme=true;b.state.prim.fst=true;b.vertices[1].u=128;b.vertices[1].v=128;
        initialize(b);textures(b,psm,cpsm);cpu.Submit(b);gpu->Submit(b);compare("indexed CLUT formats",b);
    }
    // No-op TEX0/CLD writes must preserve both an in-flight indexed draw and
    // its decoded texture. The next draw uses the still-current palette.
    for(uint8_t psm:{0,1,2,10,19,20,27,36,44,48,49,50,58,63})for(uint8_t cld=0;cld<8;++cld) {
        const bool indexed=psm==19 || psm==20 || psm==27 || psm==36 || psm==44;
        if(indexed && cld>0 && cld<6)continue;
        auto b=sprite();b.state.prim.tme=true;b.state.prim.fst=true;b.vertices[1].u=128;b.vertices[1].v=128;
        initialize(b);textures(b,GS_PSM_T8);cpu.Submit(b);gpu->Submit(b);
        const auto before=gpu->statistics();
        auto noLoad=b.state.context.tex0;noLoad.psm=psm;noLoad.cld=cld;noLoad.cbp=0;
        cpu.LoadClut(noLoad,b.state.texclut);gpu->LoadClut(noLoad,b.state.texclut);
        b.vertices[0].x+=2;b.vertices[1].x+=2;cpu.Submit(b);gpu->Submit(b);
        const auto after=gpu->statistics();
        ++cases;if(after.surfaceReadbacks!=before.surfaceReadbacks || after.textureUploads!=before.textureUploads) {
            ++failures;std::cerr<<"FAIL no-op CLUT forced readback or texture upload psm="<<unsigned(psm)<<" cld="<<unsigned(cld)<<'\n';
        }
        compare("no-op CLUT preserves queued indexed draw and palette",b);
    }
    {
        auto b=sprite();b.state.prim.tme=true;b.state.prim.fst=true;b.vertices[1].u=128;b.vertices[1].v=128;
        initialize(b);textures(b,GS_PSM_T8);
        auto replacement=b.state.context.tex0;replacement.cbp+=32;
        for(uint32_t i=0;i<256;++i) {
            const uint32_t color=0x80000000u|((255-i)&255)|((i*11&255)<<8)|((i*5&255)<<16);
            cpu.WriteVram(GS_PSM_CT32,replacement.cbp,1,i%16,i/16,color);
            gpu->WriteVram(GS_PSM_CT32,replacement.cbp,1,i%16,i/16,color);
        }
        cpu.Submit(b);gpu->Submit(b);
        cpu.LoadClut(replacement,b.state.texclut);gpu->LoadClut(replacement,b.state.texclut);
        b.state.context.tex0=replacement;b.vertices[0].x+=6;b.vertices[1].x+=6;
        cpu.Submit(b);gpu->Submit(b);compare("real CLUT reload orders old and new indexed draws",b);
    }
    // Disjoint palette/texture reads must leave the target resident. Exercise
    // every indexed format, both CLUT layouts, and 16/32-bit palettes while
    // comparing all VRAM after differently colored old/new palette draws.
    for(uint8_t psm:{19,20,27,36,44})for(uint8_t cpsm:{0,1,2,10})for(uint8_t csm:{0,1}) {
        auto b=sprite();b.state.prim.tme=true;b.state.prim.fst=true;b.vertices[1].u=128;b.vertices[1].v=128;
        initialize(b);textures(b,psm,cpsm);cpu.Submit(b);gpu->Submit(b);
        auto replacement=b.state.context.tex0;replacement.csm=csm;
        auto clut=b.state.texclut;clut.cbw=1;clut.cou=0;clut.cov=0;
        const auto before=gpu->statistics();
        cpu.LoadClut(replacement,clut);gpu->LoadClut(replacement,clut);
        b.state.context.tex0=replacement;b.state.texclut=clut;b.vertices[0].x+=2;b.vertices[1].x+=2;
        cpu.Submit(b);gpu->Submit(b);
        ++cases;if(gpu->statistics().surfaceReadbacks!=before.surfaceReadbacks) {
            ++failures;std::cerr<<"FAIL disjoint CLUT/texture read synchronized the target\n";
        }
        compare("disjoint CLUT/texture preserves queued draws",b);
    }
    // A palette can alias either attachment, wrap at 4 MiB, or reach the
    // target through CSM2 coordinates despite having a distant base pointer.
    for(uint8_t cpsm:{0,1,2,10})for(unsigned alias=0;alias<4;++alias) {
        auto b=sprite();b.state.prim.tme=true;b.state.prim.fst=true;b.vertices[1].u=128;b.vertices[1].v=128;
        initialize(b);textures(b,GS_PSM_T8,cpsm);
        auto fill=b;fill.state.prim.tme=false;cpu.Submit(fill);gpu->Submit(fill);
        auto clut=b.state.texclut;clut.cbw=1;
        auto& t=b.state.context.tex0;
        t.cbp=alias==0?0:alias==1?1024:alias==2?16383:16352;
        t.csm=alias==3?1:0;
        if(alias==3)clut.cou=4; // next page wraps into the color target
        const auto before=gpu->statistics();
        cpu.LoadClut(t,clut);gpu->LoadClut(t,clut);
        ++cases;if(gpu->statistics().surfaceReadbacks!=before.surfaceReadbacks+1) {
            ++failures;std::cerr<<"FAIL aliased CLUT did not synchronize the target\n";
        }
        b.state.texclut=clut;cpu.Submit(b);gpu->Submit(b);compare("CLUT alias observes preceding target writes",b);
    }
    for(uint8_t psm:{0,1,2,10,19,20,27,36,44})for(unsigned alias=0;alias<3;++alias) {
        auto b=sprite();b.state.prim.tme=true;b.state.prim.fst=true;b.vertices[1].u=128;b.vertices[1].v=128;
        initialize(b);textures(b,psm);
        auto fill=b;fill.state.prim.tme=false;cpu.Submit(fill);gpu->Submit(fill);
        b.state.context.tex0.tbp0=alias==0?0:alias==1?1024:16383;
        const auto before=gpu->statistics();
        cpu.Submit(b);gpu->Submit(b);
        ++cases;if(gpu->statistics().surfaceReadbacks!=before.surfaceReadbacks+1) {
            ++failures;std::cerr<<"FAIL aliased texture did not synchronize the target\n";
        }
        compare("texture alias observes preceding target writes",b);
    }
    // Indexed layouts floor odd TBW/2 when advancing a row of pages. With
    // TBW=3 the second source row aliases page one, rather than page two.
    for(uint8_t psm:{GS_PSM_T8,GS_PSM_T4}) {
        auto b=sprite();b.state.context.frame.fbp=1;
        b.state.prim.tme=true;b.state.prim.fst=true;
        initialize(b);textures(b,psm);
        b.state.textureWidth=128;b.state.textureHeight=psm==GS_PSM_T8?128:256;
        // The sampled texel aliases target (0,0), outside this draw's own
        // rectangle. This checks preceding writes without intra-draw feedback.
        b.vertices[0].u=b.vertices[1].u=0;
        b.vertices[0].v=b.vertices[1].v=(psm==GS_PSM_T8?64:128)*16;
        auto fill=b;fill.state.prim.tme=false;
        fill.vertices[0].x=fill.vertices[0].y=0;fill.vertices[1].x=fill.vertices[1].y=32;
        cpu.Submit(fill);gpu->Submit(fill);
        b.state.context.tex0.tbp0=0;b.state.context.tex0.tbw=3;
        const auto before=gpu->statistics();cpu.Submit(b);gpu->Submit(b);
        ++cases;if(gpu->statistics().surfaceReadbacks!=before.surfaceReadbacks+1) {
            ++failures;std::cerr<<"FAIL odd indexed TBW missed second-row target alias\n";
        }
        compare("odd indexed TBW observes aliased second source row",b);
    }
    for(unsigned slot=0;slot<2;++slot) {
        auto b=sprite();b.state.prim.tme=true;b.state.prim.fst=true;b.vertices[1].u=128;b.vertices[1].v=128;
        initialize(b);textures(b,GS_PSM_T8);
        auto clut=b.state.context.tex0;clut.cld=2+slot;
        cpu.LoadClut(clut,b.state.texclut);gpu->LoadClut(clut,b.state.texclut);
        cpu.Submit(b);gpu->Submit(b);clut.cld=4+slot;
        const auto before=gpu->statistics();
        cpu.LoadClut(clut,b.state.texclut);gpu->LoadClut(clut,b.state.texclut);
        cpu.Submit(b);gpu->Submit(b);
        const auto after=gpu->statistics();
        ++cases;if(after.surfaceReadbacks!=before.surfaceReadbacks || after.textureUploads!=before.textureUploads || after.submissions!=before.submissions) {
            ++failures;std::cerr<<"FAIL repeated CLD4/5 flushed or invalidated queued draws\n";
        }
        compare("CLD4/5 matching CBP preserves palette and pending draws",b);
    }
    {
        auto b=sprite();b.state.prim.tme=true;b.state.prim.fst=true;b.vertices[1].u=128;b.vertices[1].v=128;
        initialize(b);textures(b,GS_PSM_CT32);b.state.context.tex0.tbp0=0;
        cpu.Submit(b);gpu->Submit(b);
        auto fill=b;fill.state.prim.tme=false;fill.vertices[1].r=19;fill.vertices[1].g=203;
        cpu.Submit(fill);gpu->Submit(fill);
        const auto before=gpu->statistics();cpu.Submit(b);gpu->Submit(b);
        ++cases;if(gpu->statistics().surfaceReadbacks!=before.surfaceReadbacks+1) {
            ++failures;std::cerr<<"FAIL same-key render-to-texture reused stale decoded pixels\n";
        }
        compare("same-key texture alias invalidates cached decode after rendering",b);
    }
    {auto b=sprite();b.state.context.scissor={0,2047,0,2047};draw("oversized scissor with small draw",b);}
    // Distinct decoded images must survive several store-ring wraps while
    // preceding batches remain queued on the GPU. Compare both storage paths
    // with the independent CPU renderer, without intermediate GPU readbacks.
    {
        struct StoreEnvironment {
            bool existed=std::getenv("NIGHT100_REFERENCE_TEXTURE_STORE")!=nullptr;
            std::string previous=existed?std::getenv("NIGHT100_REFERENCE_TEXTURE_STORE"):"";
            ~StoreEnvironment(){if(existed)setenv("NIGHT100_REFERENCE_TEXTURE_STORE",previous.c_str(),1);else unsetenv("NIGHT100_REFERENCE_TEXTURE_STORE");}
        } environment;
        auto stream=[&](bool reference) {
            setenv("NIGHT100_REFERENCE_TEXTURE_STORE",reference?"1":"0",1);
            GL33Backend backend;GSCpuBackend checker;std::vector<uint8_t> ram(memorySize),referenceRam(memorySize);
            checker.Initialize(referenceRam.data(),memorySize);
            for(unsigned i=0;i<48;++i)checker.WriteVram(GS_PSM_CT32,0x2000+i*32,1,0,0,0x80000000u|(i*5)|((255-i*3)<<8)|((i*7%256)<<16));
            ram=referenceRam;backend.Initialize(ram.data(),memorySize);
            for(unsigned i=0;i<48;++i) {
                auto b=sprite();b.state.prim.tme=b.state.prim.fst=true;
                b.state.context.scissor={0,63,0,31};b.state.textureWidth=b.state.textureHeight=1;
                b.state.context.tex0.tbp0=0x2000+i*32;b.state.context.tex0.tbw=1;
                b.state.context.tex0.tcc=1;b.state.context.tex0.tfx=1;
                b.vertices[0].x=(i%12)*5;b.vertices[0].y=(i/12)*6;
                b.vertices[1].x=b.vertices[0].x+4;b.vertices[1].y=b.vertices[0].y+5;
                b.vertices[1].u=b.vertices[1].v=16;
                checker.Submit(b);backend.Submit(b);
                if(i%11==0){checker.TextureFlush();backend.TextureFlush();}
            }
            const auto readbacks=backend.statistics().surfaceReadbacks;
            ++cases;if(readbacks){++failures;std::cerr<<"FAIL texture store stream introduced a GPU readback\n";}
            std::vector<uint8_t> result,cpuResult;backend.SnapshotVram(result);checker.SnapshotVram(cpuResult);
            ++cases;if(result!=cpuResult){++failures;std::cerr<<"FAIL texture store wrap changed queued GS draws\n";}
            return result;
        };
        const auto reference=stream(true),rotated=stream(false);
        ++cases;if(reference!=rotated){++failures;std::cerr<<"FAIL texture store rotation changed output\n";}
    }
    {
        auto b=sprite();initialize(b);cpu.Submit(b);gpu->Submit(b);
        GSTransferCommand transfer;transfer.direction=0;transfer.bitbltbuf.dbw=1;transfer.bitbltbuf.dpsm=GS_PSM_CT24;
        transfer.trxpos.dsax=4;transfer.trxpos.dsay=6;transfer.trxreg={2,1};
        const uint8_t payload[]={11,22,33,44,55,66};const auto before=gpu->statistics();
        cpu.BeginTransfer(transfer);gpu->BeginTransfer(transfer);
        cpu.UploadImage(payload,3);gpu->UploadImage(payload,3);cpu.UploadImage(payload+3,3);gpu->UploadImage(payload+3,3);
        ++cases;if(gpu->statistics().surfaceReadbacks!=before.surfaceReadbacks+1) {
            ++failures;std::cerr<<"FAIL split CT24 transfer did not synchronize its target exactly once\n";
        }
        b.vertices[0].x=32;b.vertices[1].x=48;cpu.Submit(b);gpu->Submit(b);
        compare("split CT24 transfer preserves alpha and reloads aliased resident target",b);
    }
    for(uint8_t psm:{GS_PSM_T8,GS_PSM_T4}) {
        auto b=sprite();initialize(b);cpu.Submit(b);gpu->Submit(b);
        GSTransferCommand transfer;transfer.direction=0;transfer.bitbltbuf.dbw=1;transfer.bitbltbuf.dpsm=psm;
        transfer.trxpos.dsay=psm==GS_PSM_T8?64:128;transfer.trxreg={2,1};
        const uint8_t payload[]={0x37,0x82};const auto before=gpu->statistics();
        cpu.BeginTransfer(transfer);gpu->BeginTransfer(transfer);
        cpu.UploadImage(payload,psm==GS_PSM_T8?2:1);gpu->UploadImage(payload,psm==GS_PSM_T8?2:1);
        ++cases;if(gpu->statistics().surfaceReadbacks!=before.surfaceReadbacks+1) {
            ++failures;std::cerr<<"FAIL indexed BW1 transfer missed wrapped page row\n";
        }
        compare("indexed BW1 transfer from a later page row aliases the first row",b);
    }
    {
        auto b=sprite();initialize(b);cpu.Submit(b);gpu->Submit(b);
        auto other=b;other.state.context.frame.fbp=4;other.state.context.zbuf.zmask=true;
        other.vertices[1].r=19;other.vertices[1].g=201;cpu.Submit(other);gpu->Submit(other);
        GSTransferCommand transfer;transfer.direction=2;
        transfer.bitbltbuf={0,1,GS_PSM_CT32,4<<5,1,GS_PSM_CT32};
        transfer.trxpos={3,5,4,6,3};transfer.trxreg={8,8};
        const auto before=gpu->statistics();cpu.BeginTransfer(transfer);gpu->BeginTransfer(transfer);
        ++cases;if(gpu->statistics().surfaceReadbacks!=before.surfaceReadbacks+2) {
            ++failures;std::cerr<<"FAIL local transfer did not synchronize resident source and destination\n";
        }
        b.vertices[0].x=32;b.vertices[1].x=48;cpu.Submit(b);gpu->Submit(b);
        compare("local transfer between dirty resident targets preserves physical VRAM",b);
    }
    // A normal 448-row target is reserved once. Slightly increasing draw
    // bounds formerly forced a full readback/repack/reallocation at every step.
    for(uint8_t psm:{0,2,10}) {
        auto b=sprite();b.state.context.frame.psm=psm;
        b.state.context.zbuf.psm=psm==0?GS_PSM_Z32:GS_PSM_Z16S;
        b.state.context.scissor={0,63,0,447};initialize(b);
        const auto before=gpu->statistics();
        unsigned first=0;
        for(unsigned last:{10u,100u,217u,318u,353u,354u,395u,448u}) {
            b.vertices[0].y=first;b.vertices[1].y=last;b.vertices[1].r=last&255;b.vertices[1].b=(last*3)&255;
            cpu.Submit(b);gpu->Submit(b);first=last;
        }
        ++cases;if(gpu->statistics().surfaceReadbacks!=before.surfaceReadbacks) {
            ++failures;std::cerr<<"FAIL increasing primitive bounds synchronized a bounded target\n";
        }
        compare("bounded target growth preserves ordered strips and untouched VRAM",b);
    }
    for(int slope=0;slope<8;++slope) {
        auto b=sprite();b.state.prim.type=GS_PRIM_LINE;b.state.prim.iip=true;
        const float endpoints[8][4]={{3,4,28,19},{3,4,19,28},{28,19,3,4},{19,28,3,4},{28,4,3,19},{28,4,19,28},{3,19,28,4},{19,28,28,4}};
        b.vertices[0].x=endpoints[slope][0];b.vertices[0].y=endpoints[slope][1];b.vertices[1].x=endpoints[slope][2];b.vertices[1].y=endpoints[slope][3];
        draw("line octants Gouraud",b,1);b.state.prim.iip=false;b.vertices[0].z=b.vertices[1].z;draw("line octants exact coverage",b);
    }
    // Sequential overlapping draws must see the preceding destination, even
    // when color/depth writes or the logical GS context changes.
    auto b=sprite();initialize(b);
    for(int i=0;i<12;++i) {b.vertices[1].a=32+i*16;b.state.prim.abe=true;b.state.context.alpha=0x44;cpu.Submit(b);gpu->Submit(b);}
    compare("ordered overlapping blend",b);
    // Destination reads must observe every preceding primitive inside a batch
    // and across its 32-primitive boundary, including AFAIL depth-only writes.
    b=sprite();initialize(b);
    const auto submissionsBefore=gpu->statistics().submissions;
    for(unsigned i=0;i<97;++i) {
        b.state.prim.abe=true;b.state.context.alpha=0x44;
        b.state.context.frame.fbmsk=i%3 ? 0 : 0x0000ff00u;
        b.state.context.test=(2ull<<17)|1|(uint64_t(i%8)<<1)|(96ull<<4)|(uint64_t(i%4)<<12);
        b.vertices[1].z=100+i;b.vertices[1].a=uint8_t((i*19)%256);
        b.vertices[1].r=uint8_t(i*31);b.vertices[1].g=uint8_t(i*43);b.vertices[1].b=uint8_t(i*59);
        cpu.Submit(b);gpu->Submit(b);
    }
    compare("ordered batch boundaries with blend/depth/alpha/masks",b);
    ++cases;
    if(gpu->statistics().submissions-submissionsBefore!=4) {
        ++failures;std::cerr<<"FAIL expected four GPU submissions for 97 ordered primitives\n";
    }
    for(unsigned i=0;i<9;++i) {
        b.vertices[1].z=300+i;b.vertices[1].a=128;b.state.context.test=2ull<<17;
        cpu.Submit(b);gpu->Submit(b);
    }
    compare("partial batch after intervening readback",b);
    initialize(b);
    for(int i=0;i<24;++i) {
        b=sprite();b.vertices[0].x=float((i*7)%23);b.vertices[0].y=float((i*11)%23);
        b.vertices[1].x=b.vertices[0].x+8;b.vertices[1].y=b.vertices[0].y+7;
        b.vertices[1].r=uint8_t(13*i);b.vertices[1].g=uint8_t(19*i);b.vertices[1].b=uint8_t(23*i);
        b.state.prim.abe=true;b.state.context.alpha=0x44;
        cpu.Submit(b);gpu->Submit(b);
    }
    compare("ping-pong disjoint and overlapping rectangles",b);
    b=sprite();initialize(b);cpu.Submit(b);gpu->Submit(b);
    auto textured=sprite();textured.state.prim.tme=true;textured.state.prim.fst=true;
    textured.state.textureWidth=32;textured.state.textureHeight=32;
    textured.state.context.frame.fbp=8;textured.state.context.tex0.tbp0=0;textured.state.context.tex0.tbw=1;
    textured.state.context.tex0.tcc=1;textured.state.context.tex0.tfx=1;
    textured.vertices[1].u=32*16;textured.vertices[1].v=32*16;
    cpu.TextureFlush();gpu->TextureFlush();cpu.Submit(textured);gpu->Submit(textured);
    compare("render-to-texture and framebuffer switch",textured);
    GSTransferCommand transfer{};transfer.direction=2;
    transfer.bitbltbuf={0,1,GS_PSM_CT32,512,1,GS_PSM_CT32};transfer.trxreg={32,32};
    cpu.BeginTransfer(transfer);gpu->BeginTransfer(transfer);compare("local-to-local DMA after GPU draws",b);
    transfer.direction=1;transfer.bitbltbuf.sbp=512;
    cpu.BeginTransfer(transfer);gpu->BeginTransfer(transfer);
    std::vector<uint8_t> ca(4096),ga(4096);
    auto cn=cpu.ConsumeLocalToHostBytes(ca.data(),ca.size()),gn=gpu->ConsumeLocalToHostBytes(ga.data(),ga.size());
    ++cases;if(cn!=gn || ca!=ga){++failures;std::cerr<<"FAIL local-to-host DMA after GPU draws\n";}
    // Independent small goldens keep a few rules anchored outside the shared
    // software oracle: unblended writes, alpha-128 identity, and AFAIL KEEP.
    for(int which=0;which<3;++which) {
        b=sprite();initialize(b);
        if(which==1){b.state.prim.abe=true;b.state.context.alpha=0x44;}
        if(which==2)b.state.context.test|=1;
        gpu->Submit(b);
        const uint32_t value=gpu->ReadVram(0,0,1,8,10);
        const uint32_t golden=which==2?0x95704523u:0x802747b4u;
        ++cases;if(value!=golden){++failures;std::cerr<<"FAIL independent fragment golden "<<which<<'\n';}
    }
    // More than one UBO ring's worth of distinct pixels must survive buffer
    // orphaning while prior GPU commands still reference the old store.
    b=sprite();b.vertexCount=1;b.state.prim.type=GS_PRIM_POINT;
    b.state.context.scissor={0,63,0,127};initialize(b);
    for(unsigned i=0;i<5000;++i) {
        auto& v=b.vertices[0];v.x=float(i%64);v.y=float(i/64);v.z=i;
        v.r=i&255;v.g=(i>>3)&255;v.b=(i>>7)&255;v.a=128;
        cpu.Submit(b);gpu->Submit(b);
    }
    compare("core uniform-buffer streaming across orphan/wrap",b);
    // Independent presentation goldens: both raster backends must retain all
    // source rows and hold a static image still across even/odd field ticks.
    std::fill(cpuRam.begin(),cpuRam.end(),0);std::fill(gpuRam.begin(),gpuRam.end(),0);
    cpu.Initialize(cpuRam.data(),memorySize);gpu->Initialize(gpuRam.data(),memorySize);
    const uint32_t rowColors[]={0x800000ffu,0x8000ff00u,0x80ff0000u,0x80ffff00u};
    for(unsigned row=0;row<4;++row) {
        b=sprite();b.state.context.frame.fbw=10;b.state.context.zbuf.zbp=300;
        b.state.context.scissor={0,639,0,447};
        b.vertices[0].x=0;b.vertices[1].x=640;b.vertices[0].y=float(row);b.vertices[1].y=float(row+1);
        b.vertices[1].r=uint8_t(rowColors[row]);b.vertices[1].g=uint8_t(rowColors[row]>>8);
        b.vertices[1].b=uint8_t(rowColors[row]>>16);b.vertices[1].a=128;
        cpu.Submit(b);gpu->Submit(b);
    }
    GSPresentationRequest presentation{};presentation.smode2=1;
    presentation.dispfb1=presentation.dispfb2=10ull<<9;
    presentation.display1=presentation.display2=(639ull<<32)|(447ull<<44);
    for(uint64_t pmode : {1ull,2ull,0xff23ull}) {
        presentation.pmode=pmode;std::vector<uint8_t> first;
        for(unsigned tick=0;tick<4;++tick) {
            presentation.vsyncTick=tick;
            auto a=cpu.Present(presentation),z=gpu->Present(presentation);
            bool okay=a.width==640 && a.height==448 && a.pixels==z.pixels;
            if(tick==0) first=a.pixels;
            else okay=okay && a.pixels==first;
            for(unsigned row=0;row<4 && okay;++row) {
                uint32_t value=0;std::memcpy(&value,a.pixels.data()+row*640*4,4);
                okay=value==(rowColors[row]|0xff000000u);
            }
            ++cases;if(!okay){++failures;std::cerr<<"FAIL stable progressive presentation circuit="<<pmode<<" tick="<<tick<<'\n';}
        }
    }
    // Alternating main colors share one physical depth plane. An unconditional
    // whole-plane clear supersedes old depth without downloading old colors.
    for(uint8_t zpsm:{GS_PSM_Z32,GS_PSM_Z16,GS_PSM_Z16S}) {
        auto clear=sprite();clear.state.context.scissor={0,63,0,63};clear.state.context.zbuf={64,zpsm,false};
        clear.vertices[0].x=clear.vertices[0].y=0;clear.vertices[1].x=clear.vertices[1].y=64;
        initialize(clear);const auto start=gpu->statistics();
        for(unsigned frame=0;frame<8;++frame) {
            clear.state.context.frame.fbp=(frame&1)?8:0;clear.vertices[1].z=100+frame;
            clear.vertices[1].r=30+frame*7;cpu.Submit(clear);gpu->Submit(clear);
            auto detail=clear;detail.vertices[0].x=detail.vertices[0].y=12;detail.vertices[1].x=detail.vertices[1].y=20;
            detail.vertices[1].z=900+frame;detail.vertices[1].b=frame*13;cpu.Submit(detail);gpu->Submit(detail);
        }
        ++cases;if(!std::getenv("NIGHT100_REFERENCE_DEPTH_CLEAR") && gpu->statistics().surfaceReadbacks!=start.surfaceReadbacks) {
            ++failures;std::cerr<<"FAIL whole shared-depth overwrite forced a main target readback\n";
        }
        // A depth-only clear must preserve both old color targets and replace
        // the latest nonuniform depth, including pixels outside the detail.
        clear.state.context.frame.fbp=0;clear.state.context.frame.fbmsk=0xffffffff;clear.vertices[1].z=0;
        cpu.Submit(clear);gpu->Submit(clear);compare("alternating full depth clears preserve both colors and final physical Z",clear);
        // Host writes make cached Z stale. The next full overwrite must commit
        // even values equal to that target's older CPU upload (zero here).
        GSTransferCommand transfer{};transfer.direction=0;transfer.bitbltbuf={0,1,zpsm,64<<5,1,zpsm};transfer.trxreg={1,1};transfer.trxpos.dsax=10;transfer.trxpos.dsay=3;
        const uint32_t hostDepth=0x7777;cpu.BeginTransfer(transfer);gpu->BeginTransfer(transfer);
        const auto bytes=zpsm==GS_PSM_Z32?4u:2u;cpu.UploadImage(reinterpret_cast<const uint8_t*>(&hostDepth),bytes);gpu->UploadImage(reinterpret_cast<const uint8_t*>(&hostDepth),bytes);
        clear.state.context.frame.fbp=8;cpu.Submit(clear);gpu->Submit(clear);
        compare("full depth overwrite supersedes stale CPU transfer and old target seed",clear);
    }
    // Rejected shortcuts must keep untouched pixels and depth dependencies.
    // Exercise each guard with a dirty older color/depth target still parked.
    for(unsigned variant=0;variant<10;++variant) {
        auto clear=sprite();clear.state.context.scissor={0,63,0,63};clear.state.context.zbuf={64,GS_PSM_Z32,false};
        clear.vertices[0].x=clear.vertices[0].y=0;clear.vertices[1].x=clear.vertices[1].y=64;clear.vertices[1].z=150;
        if(variant==7){clear.state.context.scissor.y1=127;clear.vertices[1].y=128;}
        if(variant==8)clear.state.context.zbuf.psm=GS_PSM_Z24;
        initialize(clear);cpu.Submit(clear);gpu->Submit(clear);
        auto next=clear;next.state.context.frame.fbp=8;next.vertices[1].z=50;
        switch(variant) {
        case 0:next.vertices[1].x=63;break;
        case 1:next.state.context.scissor.y0=1;break;
        case 2:next.state.context.test|=1;break; // alpha NEVER
        case 3:next.state.context.test|=1u<<14;break; // destination alpha
        case 4:next.state.context.test=2ull<<17;break; // depth comparison
        case 5:next.state.context.zbuf.zmask=true;break;
        case 6:next.state.context.frame.fbw=2;next.state.context.scissor.x1=127;next.vertices[1].x=128;break;
        case 7:next.state.context.scissor.y1=63;next.vertices[1].y=64;break;
        case 9:next.vertices[0].x=.9f;break;
        }
        const auto before=gpu->statistics();cpu.Submit(next);gpu->Submit(next);
        if(variant==9) {
            ++cases;if(gpu->statistics().surfaceReadbacks==before.surfaceReadbacks){++failures;std::cerr<<"FAIL fractional sprite conservatively retains depth dependency\n";}
        }
        compare("partial or conditional shared-depth clear retains old physical pixels "+std::to_string(variant),next);
    }
    // A target already allocated taller than the clear cannot relinquish its
    // lower rows, even though the new draw alone requests a smaller extent.
    {
        auto clear=sprite();clear.state.context.scissor={0,63,0,511};clear.state.context.zbuf={64,GS_PSM_Z16S,false};
        clear.vertices[0].x=clear.vertices[0].y=0;clear.vertices[1].x=64;clear.vertices[1].y=512;clear.vertices[1].z=432;
        initialize(clear);cpu.Submit(clear);gpu->Submit(clear);
        auto other=clear;other.state.context.frame.fbp=16;cpu.Submit(other);gpu->Submit(other);
        clear.vertices[1].y=448;clear.vertices[1].z=0;cpu.Submit(clear);gpu->Submit(clear);
        compare("448-row clear cannot retire an existing 512-row depth allocation",clear);
    }
    const auto draws=gpu->gpuDraws();
    // Independent native-pipeline goldens. These use ordinary indexed meshes
    // and bone palettes, so the GS replay alone cannot exercise them.
    const NativeMatrix identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    auto mesh=std::make_shared<NativeGeometry>();mesh->id=100;mesh->revision=1;mesh->flags=8;
    mesh->vertices.resize(3);mesh->indices={0,1,2};mesh->meshes={{0,3,0}};
    const std::array<std::array<float,3>,3> points={{{4,4,1},{28,4,1},{4,28,1}}};
    for(unsigned i=0;i<3;++i){mesh->vertices[i].position=points[i];mesh->vertices[i].color=0xff3264c8u;}
    NativeModelDraw model;model.geometry=mesh;model.materials.resize(1);model.world=identity;model.camera=identity;
    model.camera[0]=1.0f/64;model.camera[5]=1.0f/32;model.zScale=16384;
    auto nativeReset=[&] {
        std::fill(gpuRam.begin(),gpuRam.end(),0);gpu->Initialize(gpuRam.data(),memorySize);
        b=sprite();b.state.context.scissor={0,63,0,31};
    };
    auto pixel=[&](unsigned x,unsigned y,uint32_t color,uint32_t depth,const char* name) {
        const auto& c=b.state.context;
        const uint32_t got=gpu->ReadVram(c.frame.psm,c.frame.fbp<<5,c.frame.fbw,x,y);
        const uint32_t z=gpu->ReadVram(c.zbuf.psm,c.zbuf.zbp<<5,c.frame.fbw,x,y);
        ++cases;if(got!=color || z!=depth){++failures;std::cerr<<"FAIL native "<<name<<" color="<<std::hex<<got<<" depth="<<z<<std::dec<<'\n';}
    };
    nativeReset();gpu->DrawNativeModel(b.state.context,model);
    pixel(8,8,0x803264c8u,16384,"projection and indexed triangle");pixel(36,24,0,0,"uncovered pixel");
    // A larger generic scissor must preserve the native bridge's requested
    // 448-row viewport, both initially and after a short preceding GS draw.
    for(bool gsFirst:{false,true}) {
        nativeReset();b.state.context.scissor.y1=511;
        if(gsFirst)gpu->Submit(b);
        gpu->DrawNativeModel(b.state.context,model);
        pixel(8,60,0x803264c8u,16384,"512-row scissor retains native viewport top");
        pixel(8,420,0,0,"512-row scissor retains native viewport bottom");
    }
    // Main and shadow cameras share Z280 with different FBW layouts. A shadow
    // which disables depth must retain the dirty main target across switches.
    // Sampling each new shadow still observes that pass's actual color.
    {
        nativeReset();b.state.context.frame={0,10,GS_PSM_CT32,0};
        b.state.context.zbuf={280,GS_PSM_Z16S,false};b.state.context.scissor={0,639,0,447};
        const auto main=b.state.context;auto shadow=main;
        shadow.frame={496,4,GS_PSM_CT16S,0};shadow.scissor={0,255,0,255};
        gpu->DrawNativeModel(main,model);
        const auto start=gpu->statistics();
        const uint32_t colors[]={0xff0000f8,0xff00f800,0xfff80000};
        for(unsigned pass=0;pass<3;++pass) {
            auto silhouette=std::make_shared<NativeGeometry>(*mesh);silhouette->id=110+pass;
            for(auto& v:silhouette->vertices)v.color=colors[pass];
            auto draw=model;draw.geometry=silhouette;draw.camera=identity;
            draw.camera[0]=draw.camera[5]=1.0f/256;draw.depthTest=draw.depthWrite=false;
            gpu->DrawNativeModel(shadow,draw);gpu->DrawNativeModel(main,model);gpu->TextureFlush();
            ++cases;if(gpu->statistics().surfaceReadbacks!=start.surfaceReadbacks+pass) {
                ++failures;std::cerr<<"FAIL shared-Z shadow switch or TEXFLUSH forced main readback\n";
            }
            auto sample=sprite();sample.state.context=main;sample.state.context.zbuf.zmask=true;
            sample.state.prim.tme=sample.state.prim.fst=true;sample.state.textureWidth=sample.state.textureHeight=256;
            sample.state.texa.ta1=128;
            auto& t=sample.state.context.tex0;t.tbp0=496<<5;t.tbw=4;t.psm=GS_PSM_CT16S;t.tfx=1;t.tcc=1;
            sample.vertices[0].x=350+pass*40;sample.vertices[1].x=382+pass*40;
            sample.vertices[0].y=32;sample.vertices[1].y=64;
            sample.vertices[0].u=sample.vertices[1].u=8*16;sample.vertices[0].v=sample.vertices[1].v=8*16;
            gpu->Submit(sample);
        }
        ++cases;if(gpu->statistics().surfaceReadbacks!=start.surfaceReadbacks+3) {
            ++failures;std::cerr<<"FAIL shadow texture sampling synchronized unrelated targets\n";
        }
        pixel(80,112,0x803264c8,16384,"dirty main color/depth survives three shared-Z shadow passes");
        for(unsigned pass=0;pass<3;++pass)pixel(360+pass*40,40,(colors[pass]&0xffffff)|0x80000000,0,"resident shadow texture is fresh each pass");
    }
    // A color-only target may hold an old unused Z seed. Flushing its color
    // must not make that Z seed current; enabling Z later reloads shared depth.
    for(bool flushColor:{false,true}) {
        nativeReset();auto main=b.state.context;main.zbuf={64,GS_PSM_Z32,false};
        auto shadow=main;shadow.frame.fbp=16;
        auto colorOnly=model;colorOnly.depthTest=colorOnly.depthWrite=false;
        const auto start=gpu->statistics();
        gpu->DrawNativeModel(shadow,colorOnly);gpu->DrawNativeModel(main,model);gpu->DrawNativeModel(shadow,colorOnly);
        if(flushColor) {
            GSTex0Reg palette{};palette.psm=GS_PSM_T8;palette.cpsm=GS_PSM_CT32;palette.cbp=16<<5;palette.cld=1;
            gpu->LoadClut(palette,{});
        }
        auto red=std::make_shared<NativeGeometry>(*mesh);red->id=120+flushColor;
        for(auto& v:red->vertices)v.color=0xff0000f8;
        auto behind=colorOnly;behind.geometry=red;behind.depthTest=true;behind.zScale=8192;
        gpu->DrawNativeModel(shadow,behind);
        ++cases;if(gpu->statistics().surfaceReadbacks!=start.surfaceReadbacks+2) {
            ++failures;std::cerr<<"FAIL enabling formerly unused shared Z did not synchronize its dependencies\n";
        }
        b.state.context=shadow;
        pixel(8,8,0x803264c8,16384,"newly enabled shared depth rejects a mesh behind the main pass");
    }
    // Eviction and CPU mutation must commit every resident target and discard
    // old GPU views, including targets parked when the mutation occurs.
    {
        nativeReset();const auto start=gpu->statistics();auto target=b.state.context;
        for(unsigned i=0;i<6;++i) {
            target.frame.fbp=i*4;target.zbuf={64,GS_PSM_Z32,true};
            auto draw=model;draw.depthTest=draw.depthWrite=false;gpu->DrawNativeModel(target,draw);
        }
        gpu->SnapshotVram(actual);
        ++cases;if(gpu->statistics().surfaceReadbacks!=start.surfaceReadbacks+6) {
            ++failures;std::cerr<<"FAIL bounded target eviction did not commit each dirty surface once\n";
        }
        for(unsigned i=0;i<6;++i) {
            ++cases;if(gpu->ReadVram(GS_PSM_CT32,(i*4)<<5,1,8,8)!=0x803264c8) {
                ++failures;std::cerr<<"FAIL resident target lost color during eviction\n";
            }
        }
        gpu->WriteVram(GS_PSM_CT32,0,1,40,8,0x805a3412);
        target.frame.fbp=0;auto draw=model;draw.depthTest=draw.depthWrite=false;
        gpu->DrawNativeModel(target,draw);b.state.context=target;
        pixel(40,8,0x805a3412,0,"CPU write invalidates parked target seed");
    }
    // CLUT state changes between ordered native markers must not interrupt an
    // unrelated native target. An aliased palette must resolve both markers.
    {
        nativeReset();auto shifted=model;shifted.world[12]=32;
        const auto first=gpu->QueueNativeModel(model),second=gpu->QueueNativeModel(shifted);
        gpu->NativeCommand(first,b.state.context);
        GSTex0Reg palette{};palette.psm=GS_PSM_T8;palette.cpsm=GS_PSM_CT32;palette.cbp=0x3000;palette.cld=1;
        GSTexClutReg layout{};
        const auto before=gpu->statistics();gpu->LoadClut(palette,layout);
        gpu->NativeCommand(second,b.state.context);
        ++cases;if(gpu->statistics().surfaceReadbacks!=before.surfaceReadbacks) {
            ++failures;std::cerr<<"FAIL disjoint palette interrupted native pass\n";
        }
        palette.cbp=0;gpu->LoadClut(palette,layout);
        ++cases;if(gpu->statistics().surfaceReadbacks!=before.surfaceReadbacks+1) {
            ++failures;std::cerr<<"FAIL aliased palette did not resolve ordered native markers\n";
        }
        pixel(8,8,0x803264c8u,16384,"first native marker survives unrelated palette change");
        pixel(40,8,0x803264c8u,16384,"second native marker survives aliased palette resolve");
    }
    // Another native draw must retain depth from the first, and switching to
    // a GS draw must preserve both native color and raw depth.
    NativeModelDraw behind=model;behind.zScale=8192;behind.materials[0].color={0,1,0,1};
    mesh->flags|=64;mesh->revision++;gpu->DrawNativeModel(b.state.context,model);gpu->DrawNativeModel(b.state.context,behind);
    pixel(8,8,0x803264c8u,16384,"native depth across draws");
    b.vertices[1].z=10000;b.state.context.test=2ull<<17;gpu->Submit(b);
    pixel(8,8,0x803264c8u,16384,"GS draw behind native mesh");
    b.vertices[1].z=20000;gpu->Submit(b);pixel(8,8,0x802747b4u,20000,"GS draw in front of native mesh");
    // Noncommuting inverse/pose matrices and a weighted two-bone blend.
    nativeReset();mesh->boneCount=2;mesh->inverseBind[0]=mesh->inverseBind[1]=identity;
    mesh->inverseBind[0][12]=-2;mesh->inverseBind[1][12]=-4;
    model.pose[0]=model.pose[1]=identity;model.pose[0][0]=2;model.pose[0][12]=8;model.pose[1][12]=12;
    model.world[12]=2;mesh->revision++;
    for(auto& v:mesh->vertices){v.weights={0.25f,0.75f,0,0};v.bones={0,1,0,0};}
    gpu->DrawNativeModel(b.state.context,model);
    pixel(6,8,0,0,"skin translation excludes old position");pixel(18,8,0x803264c8u,16384,"inverse bind, pose, weights and world matrix");
    // Screen vertices retain fractional alpha (font fades), depth policy and
    // texture sampling; they must also compose with a preceding native model.
    NativeScreenDraw screen;screen.primitive=4;screen.vertices.resize(4);
    const std::array<std::array<float,2>,4> corners={{{0,0},{32,0},{0,32},{32,32}}};
    for(unsigned i=0;i<4;++i){screen.vertices[i].position={corners[i][0],corners[i][1],65535,0};screen.vertices[i].uv={0,0,1,0};screen.vertices[i].color={0,0,200,63.75f};}
    gpu->DrawNativeScreen(b.state.context,screen);
    pixel(6,8,0x20320000u,0,"screen alpha below one half");
    auto tex=std::make_shared<NativeTexture>();tex->id=100;tex->width=tex->height=1;tex->rgba={255,0,0,255};screen.texture=tex;
    for(auto& v:screen.vertices)v.color={255,255,255,255};
    screen.depthWrite=true;gpu->DrawNativeScreen(b.state.context,screen);
    pixel(18,8,0x800000ffu,65535,"cached texture and screen depth");
    // Indexed HUD quads must match expanded triangles, including material,
    // fractional alpha and depth. Reorder the vertex store independently of
    // winding so an accidental DrawArrays path cannot produce the same image.
    for(bool textured:{false,true})for(bool alpha:{false,true})for(bool depth:{false,true})for(bool reverse:{false,true}) {
        NativeScreenDraw indexed;indexed.primitive=3;indexed.vertexAlpha=alpha;
        indexed.depthTest=depth;indexed.depthWrite=depth;indexed.texture=textured?tex:nullptr;
        indexed.vertices.resize(4);
        const unsigned order[]={3,0,2,1};
        for(unsigned i=0;i<4;++i) {
            indexed.vertices[i].position={corners[order[i]][0],corners[order[i]][1],20000,0};
            indexed.vertices[i].uv={0,0,1,0};indexed.vertices[i].color={200,100,50,63.75f};
        }
        indexed.indices={1,3,2,3,0,2};
        if(reverse)for(unsigned i=0;i<6;i+=3)std::swap(indexed.indices[i],indexed.indices[i+1]);
        NativeScreenDraw expanded=indexed;expanded.indices.clear();expanded.vertices.clear();
        for(auto index:indexed.indices)expanded.vertices.push_back(indexed.vertices[index]);
        nativeReset();gpu->DrawNativeModel(b.state.context,model);gpu->DrawNativeScreen(b.state.context,expanded);
        std::vector<uint8_t> expandedVram;gpu->SnapshotVram(expandedVram);
        nativeReset();gpu->DrawNativeModel(b.state.context,model);gpu->DrawNativeScreen(b.state.context,indexed);
        std::vector<uint8_t> indexedVram;gpu->SnapshotVram(indexedVram);
        ++cases;if(expandedVram!=indexedVram){++failures;std::cerr<<"FAIL indexed HUD differs from expanded triangles\n";}
    }
    {
        nativeReset();NativeScreenDraw indexed;indexed.primitive=3;indexed.vertexAlpha=false;indexed.depthWrite=true;
        indexed.vertices.resize(4);indexed.indices={0,1,2};
        for(unsigned i=0;i<4;++i) {
            indexed.vertices[i].position={corners[i][0],corners[i][1],100,0};
            indexed.vertices[i].uv={0,0,1,0};indexed.vertices[i].color={255,0,0,255};
        }
        const auto first=gpu->QueueNativeScreen(indexed);
        indexed.indices={1,3,2};for(auto& v:indexed.vertices){v.color={0,0,255,255};v.position[2]=200;}
        const auto second=gpu->QueueNativeScreen(indexed);
        indexed.indices={0,0,0};for(auto& v:indexed.vertices)v.color={0,255,0,255};
        gpu->NativeCommand(first,b.state.context);gpu->NativeCommand(second,b.state.context);
        pixel(4,4,0x800000ff,100,"queued indexed HUD retains old indices and vertices");
        pixel(28,28,0x80ff0000,200,"later indexed HUD uses its own index snapshot");
    }
    // Initialization discards an unresolved native pass, as it discards a GS
    // batch; it must never write an old pass into newly supplied VRAM.
    gpu->DrawNativeScreen(b.state.context,screen);nativeReset();pixel(18,8,0,0,"reset discards unresolved native work");
    mesh->boneCount=0;mesh->flags=32|64;++mesh->revision;model.world=identity;
    for(auto& v:mesh->vertices)v.normal={0,0,1};
    model.materials[0].color={1,0.5f,1,1};model.materials[0].ambient=model.materials[0].diffuse=0.5f;
    model.lights.ambient={0.25f,0.5f,0,0};model.lights.count=1;
    model.lights.direction[0]={0,0,-1,0};model.lights.color[0]={0.5f,0.25f,1,0};
    gpu->DrawNativeModel(b.state.context,model);
    pixel(8,8,0x80803060u,16384,"ambient/directional light and material modulation");
    nativeReset();model.materials[0].color[3]=0.25f;gpu->DrawNativeModel(b.state.context,model);
    pixel(8,8,0,0,"model alpha rejection retains color and depth");
    model.materials[0]=NativeMaterial{};model.lights=NativeLights{};
    // Immediate particles use transient immutable snapshots and one GPU
    // resource slot. Faint alpha must remain visible without writing depth.
    {
        auto particleMesh=std::make_shared<NativeGeometry>();
        particleMesh->id=0xfffffffffffffff0ull;particleMesh->revision=1;
        particleMesh->flags=8;particleMesh->dynamic=true;
        particleMesh->vertices.resize(3);particleMesh->indices={0,1,2};particleMesh->meshes={{0,3,0}};
        for(unsigned i=0;i<3;++i){particleMesh->vertices[i].position=points[i];particleMesh->vertices[i].color=0x400000c8;}
        NativeModelDraw particle;particle.geometry=particleMesh;particle.materials.resize(1);
        particle.world=identity;particle.camera=identity;particle.camera[0]=1.0f/64;particle.camera[5]=1.0f/32;
        particle.zScale=16384;particle.depthWrite=false;particle.alphaReference=1.0e-8f;
        nativeReset();gpu->DrawNativeModel(b.state.context,particle);
        pixel(8,8,0x20000032,0,"indexed particle fractional alpha and depth-write disabled");
        auto blue=std::make_shared<NativeGeometry>(*particleMesh);blue->revision++;
        for(auto& vertex:blue->vertices)vertex.color=0x40c80000;
        particle.geometry=blue;particle.destinationBlend=2;gpu->DrawNativeModel(b.state.context,particle);
        pixel(8,8,0x20320032,0,"particle additive source-alpha blend");
        particle.geometry=particleMesh;particle.destinationBlend=6;particle.indexed=false;
        nativeReset();gpu->DrawNativeModel(b.state.context,particle);
        pixel(8,8,0x20000032,0,"nonindexed particle fractional alpha");
        auto faint=std::make_shared<NativeGeometry>(*particleMesh);faint->revision=3;
        for(auto& vertex:faint->vertices)vertex.color=0x010000ff;
        particle.geometry=faint;particle.indexed=true;nativeReset();gpu->DrawNativeModel(b.state.context,particle);
        pixel(8,8,0x01000001,0,"particle alpha one remains visible");
        auto transparent=std::make_shared<NativeGeometry>(*faint);transparent->revision=4;
        for(auto& vertex:transparent->vertices)vertex.color=0x000000ff;
        particle.geometry=transparent;particle.depthWrite=true;particle.zScale=30000;
        gpu->DrawNativeModel(b.state.context,particle);
        pixel(8,8,0x01000001,0,"transparent particle cannot overwrite depth");
        auto opaque=std::make_shared<NativeGeometry>(*particleMesh);opaque->revision=5;
        for(auto& vertex:opaque->vertices)vertex.color=0xff0000ff;
        particle.geometry=opaque;particle.depthWrite=false;particle.zScale=8192;particle.depthTest=false;
        particle.vertexAlpha=false;gpu->DrawNativeModel(b.state.context,particle);
        pixel(8,8,0x800000ff,0,"particle disables depth test and blending");
        particle.fogEnabled=true;particle.fogStart=0;particle.fogEnd=2;particle.fogColor={0,1,0,1};
        nativeReset();gpu->DrawNativeModel(b.state.context,particle);
        pixel(8,8,0x80008080,0,"particle linear camera fog");
        particle.fogEnabled=false;particle.depthTest=true;particle.vertexAlpha=true;particle.zScale=16384;
        nativeReset();
        auto first=std::make_shared<NativeGeometry>(*opaque);first->revision=6;
        particle.geometry=first;const auto firstToken=gpu->QueueNativeModel(particle);
        // Reusing the emitter's staging data must not mutate an earlier draw.
        for(auto& vertex:opaque->vertices){vertex.position[0]+=32;vertex.color=0xff00ff00;}
        auto second=std::make_shared<NativeGeometry>(*opaque);second->revision=7;
        particle.geometry=second;const auto secondToken=gpu->QueueNativeModel(particle);
        gpu->NativeCommand(firstToken,b.state.context);gpu->NativeCommand(secondToken,b.state.context);
        pixel(8,8,0x800000ff,0,"queued particle vertices retain their original snapshot");
        pixel(40,8,0x8000ff00,0,"later particle revision replaces its buffer store");
        const auto resources=gpu->statistics().nativeGeometryResources;
        for(unsigned revision=8;revision<72;++revision) {
            auto snapshot=std::make_shared<NativeGeometry>(*second);snapshot->revision=revision;
            particle.geometry=snapshot;const auto token=gpu->QueueNativeModel(particle);gpu->NativeCommand(token,b.state.context);
        }
        ++cases;if(gpu->statistics().nativeGeometryResources!=resources){++failures;std::cerr<<"FAIL particle revisions grew GPU resource cache\n";}
    }
    // The shadow camera is orthographic. Its PS2 view matrix maps XY to
    // [0,1] independently of Z; depth is linear rather than divided by Z.
    for(uint32_t format:{GS_PSM_CT32,GS_PSM_CT16,GS_PSM_CT16S}) {
        auto parallelMesh=std::make_shared<NativeGeometry>();parallelMesh->id=0xffffffffffffffefull;
        parallelMesh->flags=8;parallelMesh->dynamic=true;parallelMesh->vertices.resize(3);
        parallelMesh->indices={0,1,2};parallelMesh->meshes={{0,3,0}};
        NativeModelDraw parallel;parallel.geometry=parallelMesh;parallel.materials.resize(1);
        parallel.world=identity;parallel.camera=identity;parallel.camera[0]=1.0f/64;parallel.camera[5]=1.0f/32;
        parallel.zScale=-8192;parallel.zShift=49152;parallel.projectionType=2;
        for(float z:{0.5f,1.0f,2.0f,0.0f,6.0f}) {
            nativeReset();b.state.context.frame.psm=format;
            if(format!=GS_PSM_CT32)b.state.context.zbuf.psm=GS_PSM_Z16;
            parallelMesh->revision++;
            for(unsigned i=0;i<3;++i){parallelMesh->vertices[i].position=points[i];parallelMesh->vertices[i].position[2]=z;parallelMesh->vertices[i].color=0xff0000ff;}
            gpu->DrawNativeModel(b.state.context,parallel);
            const uint32_t depth=uint32_t(49152-z*8192);
            pixel(8,8,format==GS_PSM_CT32?0x800000ff:0x801f,depth,"parallel projection keeps footprint and linear depth");
            pixel(24,6,format==GS_PSM_CT32?0x800000ff:0x801f,depth,"parallel footprint edge does not shrink with distance");
            pixel(36,24,0,0,"parallel projection retains uncovered pixels");
        }
    }
    // ShadowCameraUpdate disables both depth test and writes: its Z raster
    // has no storage, so the placeholder ZBP=0 aliases the main color buffer.
    // A native silhouette must never leak through that alias into sky/fog.
    for(uint32_t format:{GS_PSM_CT16,GS_PSM_CT16S})for(float shadowDepth:{2048.0f,49152.0f}) {
        nativeReset();b.state.context.frame={0,4,GS_PSM_CT32,0};
        b.state.context.zbuf={300,GS_PSM_Z16,false};b.state.context.scissor={0,255,0,255};
        const GSContext mainTarget=b.state.context;
        NativeScreenDraw sky;sky.primitive=4;sky.vertexAlpha=false;sky.vertices.resize(4);
        for(unsigned i=0;i<4;++i) {
            sky.vertices[i].position={corners[i][0]*8,corners[i][1]*8,0,0};
            sky.vertices[i].uv={0,0,1,0};sky.vertices[i].color={8,16,32,255};
        }
        gpu->DrawNativeScreen(mainTarget,sky);
        std::vector<uint8_t> beforeShadow,afterShadow;gpu->SnapshotVram(beforeShadow);
        auto silhouetteMesh=std::make_shared<NativeGeometry>();silhouetteMesh->id=0xffffffffffffffedull;
        silhouetteMesh->flags=8;silhouetteMesh->vertices.resize(3);
        silhouetteMesh->indices={0,1,2};silhouetteMesh->meshes={{0,3,0}};
        for(unsigned i=0;i<3;++i){silhouetteMesh->vertices[i].position=points[i];silhouetteMesh->vertices[i].color=0xff0000ff;}
        NativeModelDraw silhouette;silhouette.geometry=silhouetteMesh;silhouette.materials.resize(1);
        silhouette.world=identity;silhouette.camera=identity;
        silhouette.camera[0]=silhouette.camera[5]=1.0f/256;
        silhouette.projectionType=2;silhouette.zScale=0;silhouette.zShift=shadowDepth;
        silhouette.depthTest=false;silhouette.depthWrite=false;
        b.state.context.frame={496,4,uint8_t(format),0};b.state.context.zbuf={0,GS_PSM_Z16,true};
        const auto shadowToken=gpu->QueueNativeModel(silhouette);gpu->NativeCommand(shadowToken,b.state.context);
        gpu->SnapshotVram(afterShadow);
        ++cases;if(!std::equal(beforeShadow.begin(),beforeShadow.begin()+256*256*4,afterShadow.begin())) {
            ++failures;std::cerr<<"FAIL shadow placeholder depth corrupted main sky color format="<<format<<'\n';
        }
        const auto shadowColor=gpu->ReadVram(format,496<<5,4,8,8);
        ++cases;if(shadowColor!=0x801f){++failures;std::cerr<<"FAIL shadow depth-disabled silhouette was rejected format="<<format<<'\n';}
        // Return to the main camera and draw fogged geometry across the same
        // screen area. Its independent real depth target must remain intact.
        b.state.context=mainTarget;auto fog= silhouette;
        fog.projectionType=1;fog.zScale=8192;fog.zShift=0;fog.depthTest=true;fog.depthWrite=true;
        fog.fogEnabled=true;fog.fogStart=0;fog.fogEnd=2;fog.fogColor={0,1,0,1};
        const auto fogToken=gpu->QueueNativeModel(fog);gpu->NativeCommand(fogToken,b.state.context);
        pixel(8,8,0x80008080,8192,"main sky/fog after depth-disabled shadow alias");
        pixel(40,40,0x80201008,0,"sky outside fog after shadow target transition");
    }
    {
        // The guest heap reuses raster/pixel and geometry/mesh-serial addresses
        // across scenes, even when the replacement has exactly the same format.
        // Host IDs must change while earlier queued snapshots remain immutable.
        // Independent cache IDs start at one; use a fresh backend so synthetic
        // IDs from the preceding fixtures are not part of this cache's domain.
        auto lifetimeBackend=std::make_unique<GL33Backend>();auto* previousBackend=gpu;gpu=lifetimeBackend.get();
        nativeReset();NativeAssetCache assets;
        constexpr uint64_t guestGeometry=0x123456780001ull;
        const NativeAssetCache::TextureKey guestRaster{0x13f9660,0x13f97d0,0x13fa820,1,1,32,4,2,1,1,0};
        auto makeAsset=[&](unsigned shift,std::array<uint8_t,4> rgba) {
            auto geometry=assets.createGeometry();geometry->flags=8;geometry->vertices.resize(3);
            geometry->indices={0,1,2};geometry->meshes={{0,3,0}};
            for(unsigned i=0;i<3;++i){geometry->vertices[i].position=points[i];geometry->vertices[i].position[0]+=shift;}
            auto texture=assets.createTexture();texture->width=texture->height=1;
            texture->rgba.assign(rgba.begin(),rgba.end());
            assets.geometries[guestGeometry]=geometry;assets.textures[guestRaster]=texture;
            NativeModelDraw draw;draw.geometry=geometry;draw.materials.resize(1);draw.materials[0].texture=texture;
            draw.world=identity;draw.camera=identity;draw.camera[0]=1.0f/64;draw.camera[5]=1.0f/32;draw.zScale=16384;
            return draw;
        };
        auto old=makeAsset(0,{255,0,0,255});
        const auto oldGeometry=old.geometry->id,oldTexture=old.materials[0].texture->id;
        std::weak_ptr<const NativeAssetLifetime> oldLifetime=old.geometry->lifetime;
        const auto oldToken=gpu->QueueNativeModel(old);old={};assets.retire();
        ++cases;if(!assets.geometries.empty() || !assets.textures.empty() || oldLifetime.expired()) {
            ++failures;std::cerr<<"FAIL scene retirement lost pending native snapshots or retained lookup entries\n";
        }
        auto replacement=makeAsset(32,{0,0,255,255});
        ++cases;if(replacement.geometry->id==oldGeometry || replacement.materials[0].texture->id==oldTexture || replacement.geometry->revision!=1) {
            ++failures;std::cerr<<"FAIL recycled guest asset reused host identity\n";
        }
        const auto replacementToken=gpu->QueueNativeModel(replacement);replacement={};
        gpu->NativeCommand(oldToken,b.state.context);
        pixel(8,8,0x800000ff,16384,"old queued geometry and texture survive scene retirement");
        pixel(40,8,0,0,"replacement geometry does not mutate earlier marker");
        ++cases;if(!oldLifetime.expired()){++failures;std::cerr<<"FAIL GPU cache kept retired scene alive\n";}
        gpu->NativeCommand(replacementToken,b.state.context);
        pixel(40,8,0x80ff0000,16384,"recycled guest geometry and raster use new assets");
        // Repeated reloads must retire GPU buffers/textures after their final
        // ordered command. Scans run once per 64 draws, keeping the tail bounded.
        assets.retire();
        bool bounded=true;
        for(unsigned scene=0;scene<192;++scene) {
            auto next=makeAsset(0,{0,255,0,255});const auto token=gpu->QueueNativeModel(next);
            next={};gpu->NativeCommand(token,b.state.context);assets.retire();
            const auto resources=gpu->statistics();
            bounded=bounded && resources.nativeGeometryResources<=64 && resources.nativeTextureResources<=64;
        }
        ++cases;if(!bounded){++failures;std::cerr<<"FAIL GPU resource cache grew across scene reloads\n";}
        gpu=previousBackend;
    }
    // Native 16-bit targets must exchange raw RGB5/A1 words with the GS path.
    // These goldens cover both GS layouts and all supported depth layouts;
    // they are independent of the software renderer's color conversion.
    mesh->boneCount=0;mesh->flags=8;++mesh->revision;model.world=identity;
    for(uint32_t format:{GS_PSM_CT16,GS_PSM_CT16S})for(uint32_t depthFormat:{GS_PSM_Z16,GS_PSM_Z16S,GS_PSM_Z32}) {
        nativeReset();b.state.context.frame.psm=format;b.state.context.zbuf.psm=depthFormat;
        const std::array<uint32_t,4> seedColors{0,0xffff,0x4210,0x9c67},seedDepths{0,65535,16384,1000};
        for(unsigned i=0;i<seedColors.size();++i) {
            gpu->WriteVram(format,0,1,36+i,24,seedColors[i]);
            gpu->WriteVram(depthFormat,32<<5,1,36+i,24,seedDepths[i]);
        }
        gpu->DrawNativeModel(b.state.context,model);
        pixel(8,8,0x9999,16384,"16-bit opaque model packing");
        for(unsigned i=0;i<seedColors.size();++i)pixel(36+i,24,seedColors[i],seedDepths[i],"16-bit native seed preserves raw color/depth");
        b.vertices[1].z=10000;b.state.context.test=2ull<<17;gpu->Submit(b);
        pixel(8,8,0x9999,16384,"16-bit GS draw behind native");
        b.vertices[1].z=20000;gpu->Submit(b);
        pixel(8,8,0x9116,20000,"16-bit GS draw in front of native");
        NativeScreenDraw alphaScreen;alphaScreen.primitive=4;alphaScreen.vertices.resize(4);alphaScreen.vertexAlpha=false;
        for(unsigned i=0;i<4;++i) {
            alphaScreen.vertices[i].position={corners[i][0],corners[i][1],0,0};
            alphaScreen.vertices[i].uv={0,0,1,0};alphaScreen.vertices[i].color={255,0,0,127.5f};
        }
        gpu->DrawNativeScreen(b.state.context,alphaScreen);
        pixel(8,8,0x001f,20000,"16-bit fractional alpha retains clear A1 bit");
        for(auto& v:alphaScreen.vertices)v.color[3]=255;
        gpu->DrawNativeScreen(b.state.context,alphaScreen);
        pixel(8,8,0x801f,20000,"16-bit opaque alpha sets A1 bit");
        // Switching color targets may not lose their shared depth surface.
        const GSContext sixteen=b.state.context;
        b.state.context.frame={4,1,GS_PSM_CT32,0};
        auto farther=model;farther.zScale=8192;gpu->DrawNativeModel(b.state.context,farther);
        pixel(8,8,0,20000,"32-bit target inherits 16-bit target depth");
        auto nearer=model;nearer.zScale=24576;gpu->DrawNativeModel(b.state.context,nearer);
        pixel(8,8,0x803264c8u,24576,"32-bit native target after 16-bit target");
        b.state.context=sixteen;farther.zScale=22000;gpu->DrawNativeModel(b.state.context,farther);
        pixel(8,8,0x801f,24576,"16-bit target inherits 32-bit target depth");
    }
    for(uint32_t format:{GS_PSM_CT16,GS_PSM_CT16S}) {
        // The gameplay shadow raster occupies the final 128 KiB of GS VRAM.
        nativeReset();b.state.context.frame={496,4,uint8_t(format),0};b.state.context.zbuf={0,GS_PSM_Z16,true};
        b.state.context.scissor={0,255,0,255};
        NativeScreenDraw shadow;shadow.primitive=4;shadow.vertices.resize(4);shadow.vertexAlpha=false;
        for(unsigned i=0;i<4;++i) {
            shadow.vertices[i].position={corners[i][0]*8,corners[i][1]*8,0,0};
            shadow.vertices[i].uv={0,0,1,0};shadow.vertices[i].color={255,255,255,255};
        }
        const auto token=gpu->QueueNativeScreen(shadow);gpu->NativeCommand(token,b.state.context);
        pixel(0,0,0xffff,0,"256-pixel shadow target origin");pixel(255,255,0xffff,0,"256-pixel shadow target edge");
        // A failed queued command must stay fatal even if a guest Store32
        // wrapper catches the original exception while processing a DMA.
        auto invalid=std::make_unique<GL33Backend>();std::vector<uint8_t> invalidRam(memorySize);
        invalid->Initialize(invalidRam.data(),memorySize);auto unsupported=b.state.context;unsupported.frame.psm=GS_PSM_T8;
        const auto invalidToken=invalid->QueueNativeScreen(shadow);bool threw=false;
        try{invalid->NativeCommand(invalidToken,unsupported);}catch(const std::exception&){threw=true;}
        ++cases;if(!threw || !invalid->hasFailed()){++failures;std::cerr<<"FAIL unsupported native target is not fatal\n";}
    }
    nativeReset();
    {
        // Native work is queued before the driver executes its pending clear.
        // Only the GIF marker may execute it, using the context at that point
        // in the stream. Eager drawing here caused blank/flickering game frames.
        std::vector<uint8_t> ram(memorySize);GS frontend;frontend.init(ram.data(),memorySize);
        auto ordered=std::make_unique<GL33Backend>();auto* target=ordered.get();frontend.setRasterBackend(std::move(ordered));
        mesh->boneCount=0;mesh->flags=8;++mesh->revision;model.world=identity;
        const auto token=target->QueueNativeModel(model);
        frontend.writeRegister(0x4c,1ull<<16);frontend.writeRegister(0x4e,32);
        frontend.writeRegister(0x40,(63ull<<16)|(31ull<<48));frontend.writeRegister(0x47,1ull<<17);
        target->ClearFramebuffer(b.state.context,0);
        ++cases;if(target->statistics().nativeMeshes){++failures;std::cerr<<"FAIL eager native command execution\n";}
        const uint64_t packet[]={0x1000000000008001ull,0xe,token,0x7f};
        frontend.processGIFPacket(reinterpret_cast<const uint8_t*>(packet),sizeof(packet));
        ++cases;if(target->ReadVram(0,0,1,8,8)!=0x803264c8u){++failures;std::cerr<<"FAIL ordered native GIF command after clear\n";}
        // A later GS clear must erase the native geometry, rather than a host
        // side queue replaying it after the clear at presentation time.
        target->ClearFramebuffer(b.state.context,0x80302010);
        ++cases;if(target->ReadVram(0,0,1,8,8)!=0x80302010u){++failures;std::cerr<<"FAIL GS clear following native GIF command\n";}
    }
    {
        // An accelerated share-parent models the existing window context. The
        // producer remains private, while completed textures are shared.
        struct ShareParent {
            CGLContextObj previous=CGLGetCurrentContext(),context=nullptr;
            ShareParent() {
                CGLPixelFormatAttribute attributes[]={kCGLPFAOpenGLProfile,static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_3_2_Core),kCGLPFAAccelerated,static_cast<CGLPixelFormatAttribute>(0)};
                CGLPixelFormatObj pixel=nullptr;GLint count=0;
                if(CGLChoosePixelFormat(attributes,&pixel,&count)!=kCGLNoError || !pixel)throw std::runtime_error("Cannot create presentation test share parent");
                const auto error=CGLCreateContext(pixel,nullptr,&context);CGLDestroyPixelFormat(pixel);
                if(error!=kCGLNoError || CGLSetCurrentContext(context)!=kCGLNoError)throw std::runtime_error("Cannot bind presentation test share parent");
            }
            ~ShareParent(){CGLSetCurrentContext(previous);CGLDestroyContext(context);}
        } share;
        GL33Backend displayGpu;GSCpuBackend reference;
        std::vector<uint8_t> referenceRam(memorySize),displayRam(memorySize);
        auto seed=[&](uint8_t psm1,uint8_t psm2,bool black=false) {
            std::fill(referenceRam.begin(),referenceRam.end(),0);reference.Initialize(referenceRam.data(),memorySize);
            for(unsigned circuit=0;circuit<2;++circuit)for(unsigned y=0;y<66;++y)for(unsigned x=0;x<128;++x) {
                const auto psm=circuit?psm2:psm1;
                uint32_t color=(x*11+y*3+circuit*127)&255;
                color|=((x*7+y*13+circuit*89)&255)<<8;
                color|=((x*17+y*5+circuit*41)&255)<<16;
                color|=((x*31+y*19+circuit*63)&255)<<24;
                if(psm==GS_PSM_CT16 || psm==GS_PSM_CT16S)color=((color>>3)&31)|(((color>>11)&31)<<5)|(((color>>19)&31)<<10)|((color>>16)&0x8000);
                if(black && !circuit)color=0;
                reference.WriteVram(psm,(circuit?32:8)<<5,2,x,y,color);
            }
            displayRam=referenceRam;displayGpu.Initialize(displayRam.data(),memorySize);
            for(unsigned circuit=0;circuit<2;++circuit) {
                auto noWrite=sprite();noWrite.state.context.frame={circuit?32u:8u,2,circuit?psm2:psm1,0xffffffff};
                noWrite.state.context.zbuf={96,GS_PSM_Z32,true};noWrite.state.context.scissor={0,127,0,65};
                displayGpu.Submit(noWrite);
            }
        };
        auto request=[&](uint8_t psm1,uint8_t psm2) {
            GSPresentationRequest r;r.pmode=3;r.bgcolor=0x002d7bca;
            r.dispfb1=8ull|(2ull<<9)|(uint64_t(psm1)<<15)|(1ull<<32)|(1ull<<43);
            r.dispfb2=32ull|(2ull<<9)|(uint64_t(psm2)<<15)|(2ull<<32)|(2ull<<43);
            r.display1=r.display2=(63ull<<32)|(63ull<<44);
            return r;
        };
        auto comparePresentation=[&](const GSPresentationRequest& r,bool expectedGpu,bool authoritative=false,const char* name="GPU completed composition") {
            const auto expected=reference.Present(r);const auto before=displayGpu.statistics();
            auto frame=displayGpu.PresentCompleted(r,authoritative);std::vector<uint8_t> pixels;
            const bool materialized=frame.gpu?frame.gpu->readPixels(pixels):(pixels=frame.pixels,!pixels.empty());
            ++cases;if(!materialized || bool(frame.gpu)!=expectedGpu || frame.width!=expected.width || frame.height!=expected.height ||
                frame.sourceFbp!=expected.sourceFbp || frame.displayFbp!=expected.displayFbp || frame.usedPreferred!=expected.usedPreferred || pixels!=expected.pixels) {
                ++failures;std::cerr<<"FAIL "<<name<<" gpu="<<bool(frame.gpu)<<" expected="<<expectedGpu<<" pmode="<<std::hex<<r.pmode<<std::dec<<'\n';
            }
            ++cases;if(expectedGpu && displayGpu.statistics().surfaceReadbacks!=before.surfaceReadbacks) {
                ++failures;std::cerr<<"FAIL completed GPU composition performed a surface readback\n";
            }
        };
        for(uint8_t psm1:{0,1,2,10})for(uint8_t psm2:{0,1,2,10}) {
            seed(psm1,psm2);auto r=request(psm1,psm2);
            for(unsigned modes=0;modes<8;++modes)for(unsigned alpha:{0u,127u,255u}) {
                r.pmode=3|(uint64_t(modes)<<5)|(uint64_t(alpha)<<8);
                comparePresentation(r,true);
            }
            for(unsigned crt:{1u,2u}){r.pmode=crt;comparePresentation(r,true);}
        }
        seed(GS_PSM_CT32,GS_PSM_CT32);auto r=request(GS_PSM_CT32,GS_PSM_CT32);r.pmode=1;
        {
            auto dirty=sprite();dirty.state.context.frame={8,2,GS_PSM_CT32,0};dirty.state.context.zbuf={96,GS_PSM_Z32,true};dirty.state.context.scissor={0,127,0,65};
            reference.Submit(dirty);displayGpu.Submit(dirty);
            GSTransferCommand transfer;transfer.direction=0;transfer.bitbltbuf={0,1,GS_PSM_CT32,0x2000,1,GS_PSM_CT32};transfer.trxreg={4,1};
            const uint32_t payload[]={0x87654321,0x11223344,0x55667788,0x99aabbcc};const auto before=displayGpu.statistics();
            reference.BeginTransfer(transfer);displayGpu.BeginTransfer(transfer);
            reference.UploadImage(reinterpret_cast<const uint8_t*>(payload),8);displayGpu.UploadImage(reinterpret_cast<const uint8_t*>(payload),8);
            reference.UploadImage(reinterpret_cast<const uint8_t*>(payload)+8,8);displayGpu.UploadImage(reinterpret_cast<const uint8_t*>(payload)+8,8);
            ++cases;if(displayGpu.statistics().surfaceReadbacks!=before.surfaceReadbacks) {
                ++failures;std::cerr<<"FAIL disjoint split upload synchronized a completed source\n";
            }
            comparePresentation(r,true,false,"disjoint texture upload retains dirty GPU completed source");
        }
        r.dispfb1=64|(2ull<<9)|(1ull<<32)|(1ull<<43);
        r.hasPreferredSource=true;r.preferredDestFbp=64;r.preferredSource={8<<5,2,GS_PSM_CT32,0};
        comparePresentation(r,true,false,"page-aligned preferred source uses block units and zero origin");
        ++r.preferredSource.fbp;comparePresentation(r,false,false,"non-page-aligned preferred source retains CPU layout conversion");
        // A CPU mutation of a cached source makes it ineligible until reloaded.
        r=request(GS_PSM_CT32,GS_PSM_CT32);r.pmode=1;
        displayGpu.WriteVram(GS_PSM_CT32,8<<5,2,3,3,0x80442211);reference.WriteVram(GS_PSM_CT32,8<<5,2,3,3,0x80442211);
        comparePresentation(r,false,false,"CPU mutation retains conservative completed display fallback");
        seed(GS_PSM_CT32,GS_PSM_CT32,true);r=request(GS_PSM_CT32,GS_PSM_CT32);
        r.pmode=1;r.dispfb1=2ull<<9;r.contextFrames[0]={32,2,GS_PSM_CT32,0};
        auto noWrite=sprite();noWrite.state.context.frame={0,2,GS_PSM_CT32,0xffffffff};
        noWrite.state.context.zbuf={96,GS_PSM_Z32,true};noWrite.state.context.scissor={0,127,0,65};displayGpu.Submit(noWrite);
        comparePresentation(r,false,false,"generic black FBP0 preserves nonblack context fallback");
        auto black=displayGpu.PresentCompleted(r,true);std::vector<uint8_t> blackPixels;
        std::vector<uint8_t> expectedBlack(640u*512u*4u);
        for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)expectedBlack[(y*640+x)*4+3]=255;
        ++cases;if(!black.gpu || !black.gpu->readPixels(blackPixels) || blackPixels!=expectedBlack || black.sourceFbp!=0 || black.displayFbp!=0) {
            ++failures;std::cerr<<"FAIL authoritative completed black frame was replaced by another context\n";
        }
    }
    // Private program uniforms survive GS, screen, seed and resolve program
    // switches. Compare the same alternating stream with caching disabled so
    // unchanged values survive switches and every changed field is restored.
    {
        struct UniformEnvironment {
            bool existed=std::getenv("NIGHT100_REFERENCE_UNIFORMS")!=nullptr;
            std::string previous=existed?std::getenv("NIGHT100_REFERENCE_UNIFORMS"):"";
            ~UniformEnvironment(){if(existed)setenv("NIGHT100_REFERENCE_UNIFORMS",previous.c_str(),1);else unsetenv("NIGHT100_REFERENCE_UNIFORMS");}
        } environment;
        auto runUniformStream=[&](bool reference) {
            if(reference)setenv("NIGHT100_REFERENCE_UNIFORMS","1",1);else unsetenv("NIGHT100_REFERENCE_UNIFORMS");
            GL33Backend backend;std::vector<uint8_t> ram(memorySize);backend.Initialize(ram.data(),memorySize);
            auto geometry=std::make_shared<NativeGeometry>();geometry->id=0xe001;geometry->revision=1;geometry->flags=8|32|64;
            geometry->vertices.resize(3);geometry->indices={0,1,2};geometry->meshes={{0,3,0}};
            for(unsigned i=0;i<3;++i){geometry->vertices[i].position=points[i];geometry->vertices[i].normal={0,0,1};geometry->vertices[i].color=0xff3264c8;}
            auto image=std::make_shared<NativeTexture>();image->id=0xe001;image->width=image->height=1;image->rgba={200,120,80,255};
            NativeModelDraw first;first.geometry=geometry;first.materials.resize(1);first.materials[0].texture=image;
            first.world=first.camera=identity;first.camera[0]=1.0f/64;first.camera[5]=1.0f/32;first.zScale=16384;
            first.alphaReference=0.75f;first.vertexAlpha=false;first.fogEnabled=true;first.fogStart=0;first.fogEnd=4;
            first.fogColor={0.1f,0.2f,0.7f,1};first.lights.ambient={0.125f,0.25f,0.0625f,1};first.lights.count=1;
            first.lights.color[0]={0.25f,0.125f,0.25f,1};first.lights.direction[0]={0,0,-1,0};
            auto changedGeometry=std::make_shared<NativeGeometry>(*geometry);changedGeometry->id=0xe002;changedGeometry->flags=8|64;changedGeometry->boneCount=1;
            changedGeometry->inverseBind[0]=identity;
            for(auto& v:changedGeometry->vertices){v.color=0x803264c8;v.weights={1,0,0,0};}
            auto second=first;second.geometry=changedGeometry;second.materials[0].texture.reset();second.materials[0].color={0.7f,0.4f,0.9f,0.5f};
            second.materials[0].ambient=0.25f;second.materials[0].diffuse=0.5f;second.alphaReference=0.2f;second.world[12]=32;
            second.camera[12]=0.03125f;second.zScale=8192;second.zShift=512;second.pose[0]=identity;second.fogEnabled=false;
            second.fogColor={0.8f,0.7f,0.1f,1};second.fogStart=0.5f;second.fogEnd=8;
            second.lights.count=2;second.lights.ambient={0.5f,0,0.25f,1};second.lights.color[1]={0.1f,0.2f,0.3f,1};second.lights.direction[1]={1,0,0,0};
            auto main=sprite().state.context;main.frame.fbp=128;main.zbuf.zbp=160;main.scissor={0,63,0,31};
            auto alternate=main;alternate.frame={192,1,GS_PSM_CT16S,0};alternate.zbuf={200,GS_PSM_Z16S,false};alternate.scissor.y1=63;
            NativeScreenDraw overlay;overlay.vertices.resize(3);overlay.vertexAlpha=false;overlay.texture=image;
            const std::array<std::array<float,4>,3> screenPoints={{{40,3,32768,1},{60,3,32768,1},{40,19,32768,1}}};
            for(unsigned i=0;i<3;++i){overlay.vertices[i].position=screenPoints[i];overlay.vertices[i].uv={0,0,1,0};overlay.vertices[i].color={255,128,64,255};}
            for(unsigned pass=0;pass<4;++pass) {
                backend.DrawNativeModel(main,first);backend.DrawNativeModel(main,first);
                backend.DrawNativeScreen(main,overlay);backend.DrawNativeModel(main,first);
                backend.DrawNativeModel(main,second);backend.DrawNativeModel(main,first);
                auto parallel=first;parallel.camera[5]=1.0f/64;parallel.projectionType=2;parallel.zScale=4096;parallel.zShift=500;
                backend.DrawNativeModel(alternate,parallel);backend.DrawNativeScreen(alternate,overlay);
                auto gs=sprite();gs.state.context=main;gs.vertices[0].x=50;gs.vertices[1].x=55;gs.vertices[0].y=20;gs.vertices[1].y=25;backend.Submit(gs);
                backend.DrawNativeModel(main,first);overlay.texture=pass&1?image:nullptr;backend.Flush();
            }
            std::vector<uint8_t> result;backend.SnapshotVram(result);return result;
        };
        const auto reference=runUniformStream(true),cached=runUniformStream(false);
        ++cases;if(reference!=cached){++failures;std::cerr<<"FAIL private uniform cache changed alternating model/screen/GS/seed/resolve output\n";}
        ++cases;if(std::all_of(cached.begin(),cached.end(),[](uint8_t value){return value==0;})){++failures;std::cerr<<"FAIL uniform cache stream produced an empty target\n";}
    }
    // Imported camera targets are snapshotted as conventional native RGBA8
    // textures. Compare with independently decoded native assets over the same
    // draw stream; no source readback is allowed on the resident path.
    {
        auto check=[&](bool condition,const char* name){++cases;if(!condition){++failures;std::cerr<<"FAIL native render texture "<<name<<'\n';}};
        struct Result {std::vector<uint8_t> memory;GL33Backend::Statistics stats;uint64_t readbacks=0;};
        auto stream=[&](bool imported,uint8_t psm,bool linear,uint32_t wrap,unsigned scenario,bool asModel=false) {
            Result result;std::vector<uint8_t> ram(memorySize);GSCpuBackend adapter;adapter.Initialize(ram.data(),memorySize);
            GSTex0Reg tex0{};tex0.tbp0=128<<5;tex0.tbw=1;tex0.psm=psm;tex0.tw=tex0.th=6;tex0.tcc=1;
            const GSTexaReg texa{43,true,128};
            for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x) {
                uint32_t value=(x*3)|((y*3)<<8)|(((x+y)*2)<<16)|(((x/8+y/8)&1?128u:0u)<<24);
                if(psm==GS_PSM_CT16 || psm==GS_PSM_CT16S)value=(x&31)|((y&31)<<5)|(((x+y)&31)<<10)|(((x/8+y/8)&1)?0x8000:0);
                adapter.WriteVram(psm,tex0.tbp0,1,x,y,value);
            }
            GL33Backend backend;backend.Initialize(ram.data(),memorySize);
            auto source=sprite();source.state.context.frame={128,1,psm,0xffffffff};source.state.context.zbuf={400,GS_PSM_Z32,true};
            source.state.context.scissor={0,63,0,63};source.state.context.test=0;
            // A masked primitive loads the seeded surface without changing it.
            if(scenario!=1)backend.Submit(source);
            if(scenario==2) {
                GSTransferCommand transfer{};transfer.direction=0;transfer.bitbltbuf={0,1,GS_PSM_CT32,tex0.tbp0,1,psm};transfer.trxreg={1,1};
                const uint32_t changed=(psm==GS_PSM_CT16 || psm==GS_PSM_CT16S)?0x83e0:0x80334455;
                adapter.WriteVram(psm,tex0.tbp0,1,0,0,changed);
                backend.BeginTransfer(transfer);backend.UploadImage(reinterpret_cast<const uint8_t*>(&changed),(psm==GS_PSM_CT16 || psm==GS_PSM_CT16S)?2:4);
            }
            if(scenario==4) {
                for(unsigned i=0;i<5;++i){auto evict=source;evict.state.context.frame.fbp=320+i*4;backend.Submit(evict);}
            }
            auto image=std::make_shared<NativeTexture>();image->id=0xf001;image->width=image->height=64;
            image->linear=linear;image->wrapU=wrap;image->wrapV=wrap;image->lifetime=std::make_shared<NativeAssetLifetime>();
            if(imported) {
                const auto id=backend.RegisterNativeRenderTexture(tex0,texa,image->lifetime);image->renderTarget=NativeRenderTexture{id};
            } else {
                GSCpuBackend decoder;decoder.Initialize(ram.data(),memorySize);GSDrawState state{};state.context.tex0=tex0;state.texa=texa;
                std::vector<uint32_t> decoded;decoder.DecodeTexture(state,64,64,decoded);image->rgba.resize(decoded.size()*4);
                for(size_t i=0;i<decoded.size();++i){const auto pixel=(decoded[i]&0xffffff)|(std::min(255u,(decoded[i]>>24)*2u)<<24);std::memcpy(image->rgba.data()+i*4,&pixel,4);}
            }
            auto destination=source.state.context;destination.frame={scenario==3?128u:256u,1,scenario==3?psm:uint8_t(GS_PSM_CT32),0};
            destination.zbuf={416,GS_PSM_Z32,true};
            NativeScreenDraw screen;screen.primitive=4;screen.texture=image;screen.vertices.resize(4);screen.vertexAlpha=true;
            const std::array<std::array<float,2>,4> positions={{{3,4},{51,4},{3,55},{51,55}}};
            for(unsigned i=0;i<4;++i){screen.vertices[i].position={positions[i][0],positions[i][1],32768,1};
                screen.vertices[i].uv={i&1?1.25f:-0.25f,i&2?1.125f:-0.125f,1,0};screen.vertices[i].color={210,180,150,200};}
            const auto before=backend.statistics();
            if(asModel) {
                auto geometry=std::make_shared<NativeGeometry>();geometry->id=0xf002;geometry->flags=4|8;geometry->vertices.resize(4);
                geometry->indices={0,1,2,2,1,3};geometry->meshes={{0,6,0}};
                for(unsigned i=0;i<4;++i){geometry->vertices[i].position={positions[i][0],positions[i][1],1};geometry->vertices[i].uv={screen.vertices[i].uv[0],screen.vertices[i].uv[1]};}
                NativeModelDraw model;model.geometry=geometry;model.materials.resize(1);model.materials[0].texture=image;
                model.world=model.camera=identity;model.camera[0]=model.camera[5]=1.0f/64;model.projectionType=2;model.depthTest=model.depthWrite=false;
                model.lights.ambient={1,1,1,1};model.alphaReference=0;backend.DrawNativeModel(destination,model);
            } else {
                backend.DrawNativeScreen(destination,screen);
                if(scenario==0)backend.DrawNativeScreen(destination,screen); // unchanged source reuses its snapshot
                if(scenario==5) {
                    auto rewrite=source.state.context;rewrite.frame.fbmsk=0;
                    backend.ClearFramebuffer(rewrite,0x80604020);
                    if(!imported) {
                        auto changed=std::make_shared<NativeTexture>(*image);changed->id=0xf003;
                        for(size_t i=0;i<changed->rgba.size();i+=4){changed->rgba[i]=32;changed->rgba[i+1]=64;changed->rgba[i+2]=96;changed->rgba[i+3]=255;}
                        screen.texture=changed;
                    }
                    backend.DrawNativeScreen(destination,screen);
                }
            }
            const auto after=backend.statistics();result.readbacks=after.surfaceReadbacks-before.surfaceReadbacks;result.stats=after;
            backend.SnapshotVram(result.memory);return result;
        };
        for(uint8_t psm:{GS_PSM_CT32,GS_PSM_CT24,GS_PSM_CT16,GS_PSM_CT16S})for(bool linear:{false,true})for(uint32_t wrap:{1u,2u,3u}) {
            const auto reference=stream(false,psm,linear,wrap,0),imported=stream(true,psm,linear,wrap,0);
            check(reference.memory==imported.memory,"RGBA/TEXA/alpha/blending/filter/wrap parity");
            check(imported.readbacks==0 && imported.stats.nativeRenderTextureCopies==1 && imported.stats.nativeRenderTextureFallbacks==0,"resident snapshot is cached without CPU readback");
        }
        for(unsigned scenario:{1u,2u,3u,4u}) {
            const auto reference=stream(false,GS_PSM_CT16S,true,3,scenario),imported=stream(true,GS_PSM_CT16S,true,3,scenario);
            check(reference.memory==imported.memory,"missing/stale/self-target/eviction source parity");
            check(scenario==3?imported.stats.nativeRenderTextureCopies==1:imported.stats.nativeRenderTextureFallbacks==1,"self-target snapshots or missing source falls back correctly");
        }
        const auto rewrittenReference=stream(false,GS_PSM_CT16S,true,3,5),rewrittenImported=stream(true,GS_PSM_CT16S,true,3,5);
        check(rewrittenReference.memory==rewrittenImported.memory && rewrittenImported.stats.nativeRenderTextureCopies==2 && rewrittenImported.readbacks==0,"ordered source rewrite refreshes cached RGBA without readback");
        const auto modelReference=stream(false,GS_PSM_CT16S,true,3,0,true),modelImported=stream(true,GS_PSM_CT16S,true,3,0,true);
        check(modelReference.memory==modelImported.memory && modelImported.stats.nativeRenderTextureCopies==1,"native model material uses imported texture");
        GL33Backend registration;GSTex0Reg invalid{};invalid.tbw=1;invalid.tcc=1;invalid.psm=GS_PSM_T8;
        check(registration.RegisterNativeRenderTexture(invalid,{},nullptr)==0,"unsupported imports reject before ordered draws");
        {
            std::vector<uint8_t> ram(memorySize);registration.Initialize(ram.data(),memorySize);
            GSTex0Reg source{};source.tbp0=128<<5;source.tbw=1;source.psm=GS_PSM_CT16S;source.tw=source.th=6;source.tcc=1;
            auto image=std::make_shared<NativeTexture>();image->width=image->height=64;image->lifetime=std::make_shared<NativeAssetLifetime>();
            image->renderTarget=NativeRenderTexture{registration.RegisterNativeRenderTexture(source,{0,false,128},image->lifetime)};
            NativeScreenDraw queued;queued.texture=image;queued.vertices.resize(3);queued.vertexAlpha=false;
            for(unsigned i=0;i<3;++i){queued.vertices[i].position={float(i==1?20:2),float(i==2?20:2),32768,1};queued.vertices[i].color={255,255,255,255};}
            const auto token=registration.QueueNativeScreen(queued);queued.texture.reset();image.reset();
            auto destination=sprite().state.context;destination.frame.fbp=32;destination.zbuf.zbp=64;
            for(unsigned i=0;i<65;++i)registration.DrawNativeScreen(destination,queued);
            check(registration.statistics().nativeRenderTextureResources==1,"queued draw keeps imported scene resource alive");
            registration.NativeCommand(token,destination);
            for(unsigned i=0;i<65;++i)registration.DrawNativeScreen(destination,queued);
            check(registration.statistics().nativeRenderTextureResources==0,"completed scene imports retire after queued consumption");
        }

    }
    // A fresh import shader copy must preserve an already active native
    // destination. Alternate model/screen/model across two disjoint sources.
    {
        auto stream=[&](bool imported) {
            GL33Backend backend;std::vector<uint8_t> ram(memorySize);backend.Initialize(ram.data(),memorySize);
            auto source=sprite().state.context;source.frame={128,1,GS_PSM_CT16S,0};source.zbuf={400,GS_PSM_Z16S,true};source.scissor={0,63,0,63};
            backend.ClearFramebuffer(source,0x800000f8);source.frame.fbp=192;backend.ClearFramebuffer(source,0x8000f800);
            auto image=[&](uint32_t base,uint64_t identity,std::array<uint8_t,4> color) {
                auto result=std::make_shared<NativeTexture>();result->id=identity;result->width=result->height=64;
                if(imported) {
                    GSTex0Reg descriptor{};descriptor.tbp0=base<<5;descriptor.tbw=1;descriptor.psm=GS_PSM_CT16S;descriptor.tw=descriptor.th=6;descriptor.tcc=1;
                    result->renderTarget=NativeRenderTexture{backend.RegisterNativeRenderTexture(descriptor,{0,false,128},nullptr)};
                } else {result->rgba.resize(64*64*4);for(size_t i=0;i<result->rgba.size();++i)result->rgba[i]=color[i&3];}
                return result;
            };
            auto first=image(128,0xf101,{248,0,0,255}),second=image(192,0xf102,{0,248,0,255});
            auto destination=source;destination.frame={256,1,GS_PSM_CT32,0};destination.zbuf={416,GS_PSM_Z32,true};
            auto geometry=std::make_shared<NativeGeometry>();geometry->id=0xf103;geometry->flags=8;geometry->vertices.resize(3);
            geometry->indices={0,1,2};geometry->meshes={{0,3,0}};
            const std::array<std::array<float,3>,3> positions={{{2,2,1},{20,2,1},{2,20,1}}};
            for(unsigned i=0;i<3;++i)geometry->vertices[i].position=positions[i];
            NativeModelDraw model;model.geometry=geometry;model.materials.resize(1);model.materials[0].texture=first;
            model.world=model.camera=identity;model.camera[0]=model.camera[5]=1.0f/64;model.projectionType=2;
            model.depthTest=model.depthWrite=model.vertexAlpha=false;model.alphaReference=0;
            const auto before=backend.statistics();backend.DrawNativeModel(destination,model);
            NativeScreenDraw screen;screen.primitive=4;screen.texture=second;screen.vertexAlpha=false;screen.vertices.resize(4);
            for(unsigned i=0;i<4;++i){screen.vertices[i].position={i&1?40.0f:25.0f,i&2?20.0f:2.0f,32768,1};screen.vertices[i].color={255,255,255,255};}
            backend.DrawNativeScreen(destination,screen);model.world[12]=40;backend.DrawNativeModel(destination,model);
            const auto stats=backend.statistics();std::vector<uint8_t> result;backend.SnapshotVram(result);
            ++cases;if(imported && (stats.surfaceReadbacks!=before.surfaceReadbacks || stats.nativeRenderTextureCopies!=2 || stats.nativeRenderTextureFallbacks)) {
                ++failures;std::cerr<<"FAIL disjoint native pass source snapshot introduced readback or incorrect copies\n";
            }
            GSCpuBackend pixels;pixels.Initialize(result.data(),memorySize);
            ++cases;if(pixels.ReadVram(GS_PSM_CT32,256<<5,1,4,4)!=0x800000f8 || pixels.ReadVram(GS_PSM_CT32,256<<5,1,30,4)!=0x8000f800 ||
                pixels.ReadVram(GS_PSM_CT32,256<<5,1,44,4)!=0x800000f8) {
                ++failures;std::cerr<<"FAIL alternating imported sources lost native destination pixels\n";
            }
            return result;
        };
        const auto reference=stream(false),imported=stream(true);
        ++cases;if(reference!=imported){++failures;std::cerr<<"FAIL model/sourceA -> screen/freshSourceB -> model/sourceA changed destination\n";}
    }
    // Overlay timing is opt-in, lazily probes the driver, and can be disabled
    // with an in-flight native query without changing renderer output.
    {
        struct TimerEnvironment {
            bool existed=std::getenv("NIGHT100_GPU_TIMING")!=nullptr;
            std::string previous=existed?std::getenv("NIGHT100_GPU_TIMING"):"";
            TimerEnvironment(){unsetenv("NIGHT100_GPU_TIMING");}
            ~TimerEnvironment(){if(existed)setenv("NIGHT100_GPU_TIMING",previous.c_str(),1);else unsetenv("NIGHT100_GPU_TIMING");}
        } environment;
        auto check=[&](bool okay,const char* message){++cases;if(!okay){++failures;std::cerr<<"FAIL dynamic GPU timing "<<message<<'\n';}};
        GL33Backend measured;std::vector<uint8_t> ram(memorySize);measured.Initialize(ram.data(),memorySize);
        check(!measured.statistics().gpuTimingEnabled && !measured.statistics().gpuTimingChecked,"starts without queries or capability probes");
        measured.setTimingEnabled(true);auto initial=measured.statistics();
        check(initial.gpuTimingEnabled && initial.gpuTimingChecked,"enables and reports driver availability");
        auto context=sprite().state.context;context.frame.fbp=32;context.zbuf.zbp=64;
        NativeScreenDraw screen;screen.primitive=3;screen.vertexAlpha=false;screen.vertices.resize(3);
        for(unsigned i=0;i<3;++i){screen.vertices[i].position={float(i==1?20:2),float(i==2?20:2),32768,1};screen.vertices[i].color={220,80,40,255};}
        for(unsigned i=0;i<3;++i){measured.DrawNativeScreen(context,screen);measured.Flush();}
        auto sampled=measured.statistics();
        check(sampled.gpuTimingSupported?sampled.gpuSamples>0:sampled.gpuSamples==0,"produces genuine samples or stays unavailable");
        measured.DrawNativeScreen(context,screen);measured.setTimingEnabled(false);measured.Flush();
        const auto disabled=measured.statistics();
        check(!disabled.gpuTimingEnabled && disabled.gpuTimingChecked,"disables pending native query safely");
        for(unsigned i=0;i<3;++i){measured.DrawNativeScreen(context,screen);measured.Flush();}
        check(measured.statistics().gpuSamples==disabled.gpuSamples,"hidden overlay does not collect queries");
        measured.setTimingEnabled(true);measured.DrawNativeScreen(context,screen);measured.Flush();measured.setTimingEnabled(false);
        check(!measured.hasFailed(),"reenables cleanly without GL state errors");
        check(measured.ReadVram(GS_PSM_CT32,32<<5,1,4,4)==0x802850dc,"toggling preserves native pixels");
    }
    std::cout<<"Cases: "<<cases<<" passed: "<<cases-failures<<" failed: "<<failures<<" GPU draws: "<<draws<<'\n';
    return failures || !draws ? 1:0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
