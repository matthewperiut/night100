#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
namespace {
using Read = uint32_t (*)(uint8_t*,uint32_t,uint32_t,uint32_t,uint32_t);
using Write = void (*)(uint8_t*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
struct Format { uint8_t psm; Read read; Write write; };
const std::array<Format,14> formats = {{
 {0,GSMem::ReadCT32,GSMem::WriteCT32},{1,GSMem::ReadCT24,GSMem::WriteCT24},
 {2,GSMem::ReadCT16,GSMem::WriteCT16},{10,GSMem::ReadCT16S,GSMem::WriteCT16S},
 {48,GSMem::ReadZ32,GSMem::WriteZ32},{49,GSMem::ReadZ24,GSMem::WriteZ24},
 {50,GSMem::ReadZ16,GSMem::WriteZ16},{58,GSMem::ReadZ16S,GSMem::WriteZ16S},
 {19,GSMem::ReadP8,GSMem::WriteP8},{20,GSMem::ReadP4,GSMem::WriteP4},
 {27,GSMem::ReadP8H,GSMem::WriteP8H},{36,GSMem::ReadP4HL,GSMem::WriteP4HL},
 {44,GSMem::ReadP4HH,GSMem::WriteP4HH},{63,GSMem::ReadNull,GSMem::WriteNull}
}};
void require(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
void referenceRead(const GSContext& c,uint32_t w,uint32_t h,const Format& f,const Format& z,std::vector<uint8_t>&ram,std::vector<uint32_t>&out) {
 out.resize(size_t(w)*h*2);const auto bw=std::max(1u,c.frame.fbw);
 for(uint32_t y=0;y<h;y++)for(uint32_t x=0;x<w;x++){auto i=(size_t(y)*w+x)*2;out[i]=f.read(ram.data(),c.frame.fbp<<5,bw,x,y);out[i+1]=z.read(ram.data(),c.zbuf.zbp<<5,bw,x,y);}
}
void referenceWrite(const GSContext& c,uint32_t w,uint32_t h,const Format& f,const Format& z,std::vector<uint8_t>&ram,const std::vector<uint32_t>&p,const std::vector<uint32_t>&b) {
 const auto bw=std::max(1u,c.frame.fbw);
 for(uint32_t y=0;y<h;y++)for(uint32_t x=0;x<w;x++){auto i=(size_t(y)*w+x)*2;if(p[i]!=b[i])f.write(ram.data(),c.frame.fbp<<5,bw,x,y,p[i]);if(p[i+1]!=b[i+1])z.write(ram.data(),c.zbuf.zbp<<5,bw,x,y,p[i+1]);}
}
uint32_t presentationColor(const Format& f,std::vector<uint8_t>&ram,uint32_t base,uint32_t bw,uint32_t x,uint32_t y,bool preserve) {
 const uint32_t packed=f.read(ram.data(),base,bw,x,y);
 if(f.psm==0)return preserve?packed:packed|0xff000000u;
 if(f.psm==1)return packed|0xff000000u;
 const uint32_t r=packed&31,g=(packed>>5)&31,b=(packed>>10)&31;
 const uint32_t a=preserve?((packed&0x8000)?0x80:0):255;
 return ((r<<3)|(r>>2))|(((g<<3)|(g>>2))<<8)|(((b<<3)|(b>>2))<<16)|(a<<24);
}
uint64_t displayFrame(const Format& f,uint32_t page,uint32_t bw,uint32_t x,uint32_t y) {
 return page|(uint64_t(bw)<<9)|(uint64_t(f.psm)<<15)|(uint64_t(x)<<32)|(uint64_t(y)<<43);
}
void presentationChecks(GSCpuBackend& backend,std::vector<uint8_t>&ram) {
 unsigned cases=0;
 for(const auto& f:std::array<Format,4>{formats[0],formats[1],formats[2],formats[3]})
  for(unsigned scenario=0;scenario<6;scenario++) {
   const uint32_t w=scenario==0?640:scenario==1?193:65,h=scenario==0?448:scenario==1?129:67;
   const uint32_t page=scenario==2?511:3,bw=scenario==3?0:scenario==4?3:10;
   const uint32_t ox=scenario==0?0:scenario==5?2047:61,oy=scenario==0?0:scenario==5?2047:31;
   GSPresentationRequest request{};request.pmode=1;
   request.dispfb1=displayFrame(f,page,bw,ox,oy);
   request.display1=(uint64_t(w-1)<<32)|(uint64_t(h-1)<<44);
   const auto result=backend.Present(request);
   require(result.width==w && result.height==h,"presentation dimensions changed");
   std::vector<uint8_t> expected(640*512*4,0);
   for(uint32_t y=0;y<h;y++)for(uint32_t x=0;x<w;x++) {
    const auto color=presentationColor(f,ram,page<<5,bw?bw:10,ox+x,oy+y,false);
    std::memcpy(expected.data()+(size_t(y)*640+x)*4,&color,4);
   }
   require(result.pixels==expected,"cached presentation swizzle/alpha/origin/wrap differs from scalar reference");
   ++cases;
   // Preferred sources use 256-byte blocks, including a base not aligned to
   // a GS page. This also checks that old cached layouts never retain VRAM.
   request.hasPreferredSource=true;request.preferredDestFbp=page;
   request.preferredSource={16383,3,f.psm,0};
   const auto preferred=backend.Present(request);
   require(preferred.usedPreferred && preferred.sourceFbp==16383,"preferred presentation source was not selected");
   std::fill(expected.begin(),expected.end(),0);
   for(uint32_t y=0;y<h;y++)for(uint32_t x=0;x<w;x++) {
    const auto color=presentationColor(f,ram,16383,3,x,y,false);
    std::memcpy(expected.data()+(size_t(y)*640+x)*4,&color,4);
   }
   require(preferred.pixels==expected,"cached preferred source lost block carry/wrap");
   ++cases;
  }
 // Exercise preserveAlpha=true through real PMODE mixing. Test both source
 // format families and nonzero source origins against scalar reads/math.
 for(const auto& f:std::array<Format,4>{formats[0],formats[1],formats[2],formats[3]})
  for(uint64_t mode:{3ull,0x8023ull,0xffa3ull,0x4063ull}) {
   GSPresentationRequest request{};request.pmode=mode;request.bgcolor=0x456789;
   request.dispfb1=displayFrame(f,511,3,63,61);
   request.dispfb2=displayFrame(formats[0],7,10,5,9);
   request.display1=request.display2=(64ull<<32)|(66ull<<44);
   const auto result=backend.Present(request);std::vector<uint8_t> expected(640*512*4,0);
   for(unsigned y=0;y<67;y++)for(unsigned x=0;x<65;x++) {
    const uint32_t src=presentationColor(f,ram,511<<5,3,63+x,61+y,true);
    const uint32_t dst=mode&128?uint32_t(request.bgcolor):presentationColor(formats[0],ram,7<<5,10,5+x,9+y,true);
    const uint32_t factor=mode&32?uint32_t((mode>>8)&255):std::min(255u,(src>>24)*2);
    uint32_t color=0xff000000u;
    for(unsigned channel=0;channel<3;channel++) {
     const int a=(src>>(channel*8))&255,b=(dst>>(channel*8))&255;
     const int mixed=std::clamp(b+((a-b)*int(factor))/255,0,255);
     color|=uint32_t(mixed)<<(channel*8);
    }
    std::memcpy(expected.data()+(size_t(y)*640+x)*4,&color,4);
   }
   require(result.pixels==expected,"presentation alpha/PMODE composition differs from scalar reference");
   ++cases;
  }
 std::cout<<"PASS: "<<cases<<" presentation format/origin/preferred block/wrap/alpha/PMODE scalar-reference cases\n";
}
uint32_t textureAddress(const GSTex0Reg& t,uint32_t x,uint32_t y) {
 // The generic texture accessor treats TBW=0 as zero page-row stride.
 // Older surface address helpers normalize that invalid stride to one.
 if(!t.tbw)y%=t.psm==0 || t.psm==1?32:64;
 if(t.psm==0 || t.psm==1)return GSPSMCT32::addrPSMCT32(t.tbp0,t.tbw,x,y)&(GSMem::MEMORY_SIZE-4);
 if(t.psm==2)return GSPSMCT16::addrPSMCT16(t.tbp0,t.tbw,x,y)&(GSMem::MEMORY_SIZE-2);
 return GSPSMCT16::addrPSMCT16S(t.tbp0,t.tbw,x,y)&(GSMem::MEMORY_SIZE-2);
}
uint32_t referenceTexture(GSMem::TexturePageCache& cache,const GSDrawState& state,
                          std::vector<uint8_t>&ram,uint32_t x,uint32_t y) {
 const auto& t=state.context.tex0;
 uint32_t raw=GSMem::ReadTexture(cache,ram.data(),t.psm,t.tbp0,t.tbw,x,y);
 if(t.psm==0)return raw;
 uint32_t rgb=raw&0xffffff,alpha=state.texa.ta0;
 if(t.psm==2 || t.psm==10) {
  rgb=((raw&31)<<3)|(((raw>>5)&31)<<11)|(((raw>>10)&31)<<19);
  alpha=raw&0x8000?state.texa.ta1:(state.texa.aem && rgb==0?0:state.texa.ta0);
 } else if(state.texa.aem && rgb==0)alpha=0;
 return rgb|(alpha<<24);
}
void textureChecks(GSCpuBackend& backend,std::vector<uint8_t>&ram,const std::vector<uint8_t>&original) {
 unsigned cases=0;uint64_t texels=0;
 const std::array<std::array<unsigned,2>,6> sizes={{{1,1},{63,65},{65,67},{129,131},{256,256},{1024,2}}};
 for(const auto& f:std::array<Format,4>{formats[0],formats[1],formats[2],formats[3]})
  for(uint32_t base:{0u,31u,0x2200u,16383u})for(uint32_t stride:{0u,1u,3u,4u,63u})
   for(bool aem:{false,true})for(const auto& size:sizes) {
    const auto w=size[0],h=size[1];ram=original;backend.Initialize(ram.data(),ram.size());
    GSDrawState state{};state.context.tex0.tbp0=base;state.context.tex0.tbw=stride;state.context.tex0.psm=f.psm;
    state.texa.ta0=37;state.texa.ta1=201;state.texa.aem=aem;
    state.linearFilter=true;state.prim.fst=false;state.context.clamp=~uint64_t(0);
    const uint32_t edges16[]={0,0x8000,0x7fff,0xffff};
    const uint32_t edges32[]={0,0xff000000,0x12345678,0x80010203};
    for(unsigned x=0;x<4;x++)f.write(ram.data(),base,stride,x,0,f.psm==2 || f.psm==10?edges16[x]:edges32[x]);
    GSMem::TexturePageCache cache;std::vector<uint32_t> decoded;
    backend.DecodeTexture(state,1,1,decoded);referenceTexture(cache,state,ram,0,0);
    // Simulate rendered VRAM changes after the old page was cached. A decode
    // must keep the stale prefix until its first page miss, rather than silently
    // invalidating/replacing the one-page cache to make bulk reads convenient.
    const auto firstPage=textureAddress(state.context.tex0,0,0)&~8191u;
    for(unsigned i=0;i<8192;i++)ram[firstPage+i]^=0x5a;
    backend.DecodeTexture(state,w,h,decoded);
    for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++) {
     require(decoded[size_t(y)*w+x]==referenceTexture(cache,state,ram,x,y),"bulk texture decode changed stale prefix, swizzle, TEXA or row-major cache order");
    }
    texels+=uint64_t(w)*h*2;
    // A palette load immediately afterward must see the exact final cached
    // page, including its pre-mutation contents. Then index zero samples it.
    const auto lastPage=textureAddress(state.context.tex0,w-1,h-1)&~8191u;
    for(unsigned i=0;i<8192;i++)ram[lastPage+i]^=0xa5;
    const auto palette=GSMem::ReadTexture(cache,ram.data(),GS_PSM_CT32,lastPage>>8,1,0,0);
    GSTex0Reg t{};t.psm=GS_PSM_T8;t.cpsm=GS_PSM_CT32;t.cbp=lastPage>>8;t.cld=1;
    backend.LoadClut(t,{});
    const uint32_t indices=(lastPage>>8)^0x1000;
    GSMem::WriteP8(ram.data(),indices,2,0,0,0);
    GSDrawState indexed{};indexed.context.tex0=t;indexed.context.tex0.tbp0=indices;indexed.context.tex0.tbw=2;
    backend.DecodeTexture(indexed,1,1,decoded);
    require(decoded[0]==palette,"bulk texture decode changed cache history observed by the following CLUT load");
    backend.TextureFlush();cache.Invalidate();backend.DecodeTexture(state,w,h,decoded);
    for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)
     require(decoded[size_t(y)*w+x]==referenceTexture(cache,state,ram,x,y),"TEXFLUSH did not restore fresh bulk texture data");
    ++cases;
   }
 // A wrapped traversal can return to its initial page after other cache
 // misses. The final page must then be fresh, even though its ID matches the
 // stale initial page. Exercise this in both 32- and 16-bit address families.
 for(bool sixteen:{false,true}) {
  ram=original;backend.Initialize(ram.data(),ram.size());
  GSDrawState state{};auto& t=state.context.tex0;t.psm=sixteen?GS_PSM_CT16S:GS_PSM_CT32;
  t.tbw=sixteen?56:32;state.texa.ta0=37;state.texa.ta1=201;
  const unsigned w=sixteen?513:63,h=sixteen?577:513;
  GSMem::TexturePageCache cache;std::vector<uint32_t> decoded;
  backend.DecodeTexture(state,1,1,decoded);referenceTexture(cache,state,ram,0,0);
  for(unsigned i=0;i<8192;i++)ram[i]^=0x5a;
  backend.DecodeTexture(state,w,h,decoded);
  for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)
   require(decoded[size_t(y)*w+x]==referenceTexture(cache,state,ram,x,y),"wrapped texture traversal reused its stale initial page");
  require((textureAddress(t,w-1,h-1)&~8191u)==0,"cache return fixture no longer reaches initial page");
  for(unsigned i=0;i<8192;i++)ram[i]^=0xa5;
  const auto palette=GSMem::ReadTexture(cache,ram.data(),GS_PSM_CT32,0,1,0,0);
  GSTex0Reg clut{};clut.psm=GS_PSM_T8;clut.cpsm=GS_PSM_CT32;clut.cld=1;backend.LoadClut(clut,{});
  GSMem::WriteP8(ram.data(),0x2000,2,0,0,0);
  GSDrawState indexed{};indexed.context.tex0=clut;indexed.context.tex0.tbp0=0x2000;indexed.context.tex0.tbw=2;
  backend.DecodeTexture(indexed,1,1,decoded);
  require(decoded[0]==palette,"returned final cache page retained stale initial bytes");
  texels+=uint64_t(w)*h;++cases;
 }
 std::cout<<"PASS: "<<cases<<" bulk texture cases / "<<texels<<" scalar-reference texels, stale first page, final CLUT history, TEXA, odd/zero strides and wrapping\n";
}
}
int main(int argc,char**) {
 std::mt19937 rng(0x4e313030);std::vector<uint8_t> original(GSMem::MEMORY_SIZE);
 for(auto&v:original)v=uint8_t(rng());
 std::vector<uint8_t> actual=original,expected=original;GSCpuBackend backend;backend.Initialize(actual.data(),actual.size());
 unsigned cases=0;
 for(const auto&f:formats)for(const auto&z:formats)for(unsigned scenario=0;scenario<4;scenario++) {
  GSContext c{};c.frame.psm=f.psm;c.zbuf.psm=z.psm;c.frame.fbw=scenario==0?0:scenario==1?1:10;
  c.frame.fbp=scenario==2?511:scenario==3?0xfffffff1u:3;
  c.zbuf.zbp=scenario==0?c.frame.fbp:scenario==1?c.frame.fbp+1:scenario==2?511:0xfffffff2u;
  const uint32_t w=scenario==1?137:scenario==3?73:65,h=scenario==1?131:67;
  actual=original;expected=original;
  std::vector<uint32_t> before,reference,pixels;backend.ReadSurface(c,w,h,before);referenceRead(c,w,h,f,z,expected,reference);
  require(before==reference,"cached surface read differs from scalar GSMem reference");
  // Mix unchanged, color-only, depth-only and both changes. The deliberately
  // overlapping/wrapped targets make changed-only writes/order observable.
  pixels=before;
  for(size_t i=0;i<pixels.size();++i)if((rng()&3)!=0)pixels[i]=rng();
  backend.WriteSurface(c,w,h,pixels,before);referenceWrite(c,w,h,f,z,expected,pixels,before);
  require(actual==expected,"cached surface write changed aliased VRAM/order/lane semantics");
  // A DMA/other context may have modified untouched aliases after upload.
  actual[12345]^=0x5a;expected[12345]^=0x5a;
  backend.WriteSurface(c,w,h,before,before);
  require(actual==expected,"unchanged surface erased a concurrent alias");
  backend.ReadSurface(c,w,h,pixels);referenceRead(c,w,h,f,z,expected,reference);
  require(pixels==reference,"cached read retained stale VRAM contents");
  auto sparse=pixels;
  for(size_t pixel:{size_t(0),size_t(7),size_t(8),size_t(31),size_t(w)*h-1}) {
   sparse[pixel*2]^=0x13579bdf;sparse[pixel*2+1]^=0x2468ace0;
  }
  backend.WriteSurface(c,w,h,sparse,pixels);referenceWrite(c,w,h,f,z,expected,sparse,pixels);
  require(actual==expected,"sparse cache-line skip changed wrapped aliases, row boundaries or final partial block");
  ++cases;
 }
 // Exercise height growth, LRU target-layout eviction, reinitialization, and
 // high PSM bits normalized exactly as the existing dispatch table.
 for(unsigned n=1;n<=12;n++) {
  const auto&f=formats[n%8];const auto&z=formats[(n+3)%8];GSContext c{};
  c.frame.psm=f.psm|0x80;c.zbuf.psm=z.psm|0x80;c.frame.fbp=n;c.zbuf.zbp=500+n;c.frame.fbw=3;
  std::vector<uint32_t>a,b;backend.ReadSurface(c,193,n*17,a);referenceRead(c,193,n*17,f,z,actual,b);require(a==b,"cache growth/eviction/high-PSM normalization failed");
 }
 backend.Initialize(nullptr,0);std::vector<uint32_t> zero;backend.ReadSurface({},7,9,zero);require(std::all_of(zero.begin(),zero.end(),[](uint32_t p){return p==0;}),"null VRAM read changed");
 backend.Initialize(actual.data(),actual.size());
 presentationChecks(backend,actual);
 textureChecks(backend,actual,original);
 bool rejected=false;try{backend.WriteSurface({},2,2,{},{});}catch(const std::invalid_argument&){rejected=true;}require(rejected,"invalid surface sizes accepted");
 std::cout<<"PASS: "<<cases<<" surface format pairs/wrap/overlap cases, changed-only writes, cache growth/eviction, null VRAM and invalid sizes\n";
 if(argc>1) {
  GSContext c{};c.frame.fbp=0;c.frame.fbw=10;c.frame.psm=0;c.zbuf.zbp=140;c.zbuf.psm=58;
  std::vector<uint32_t>a,b,p;referenceRead(c,640,448,formats[0],formats[7],actual,b);p=b;for(auto&v:p)v^=0x13579bd;
  backend.ReadSurface(c,640,448,a);const auto t=std::chrono::steady_clock::now();
  for(unsigned i=0;i<120;i++){backend.ReadSurface(c,640,448,a);backend.WriteSurface(c,640,448,p,b);}
  const auto candidate=std::chrono::duration<double>(std::chrono::steady_clock::now()-t).count();const auto t2=std::chrono::steady_clock::now();
  for(unsigned i=0;i<120;i++){referenceRead(c,640,448,formats[0],formats[7],actual,a);referenceWrite(c,640,448,formats[0],formats[7],actual,p,b);}
  const auto scalar=std::chrono::duration<double>(std::chrono::steady_clock::now()-t2).count();
  std::cout<<"120x640x448 CT32/Z16S read+changed-write cached="<<candidate<<" scalar="<<scalar<<" speedup="<<scalar/candidate<<"\n";
  GSPresentationRequest request{};request.pmode=1;request.dispfb1=(10ull<<9)|3;request.display1=(639ull<<32)|(447ull<<44);
  backend.Present(request);const auto tp=std::chrono::steady_clock::now();
  for(unsigned i=0;i<120;i++)backend.Present(request);
  std::cout<<"120x640x448 CT32 single-CRT presentation="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-tp).count()<<"s\n";
  for(unsigned changed:{0u,1u,10u,25u,50u,100u}) {
   backend.ReadSurface(c,640,448,b);p=b;
   for(size_t pixel=0;pixel<640*448;pixel++)if(pixel%100<changed) {
    p[pixel*2]^=0x11335577;p[pixel*2+1]^=0x1234;
   }
   const auto ts=std::chrono::steady_clock::now();
   for(unsigned i=0;i<240;i++)backend.WriteSurface(c,640,448,p,b);
   std::cout<<"240x640x448 changed="<<changed<<"% WriteSurface="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-ts).count()<<"s\n";
  }
  for(uint8_t psm:{0,1,2,10}) {
   GSDrawState state{};state.context.tex0.psm=psm;state.context.tex0.tbp0=0x2200;state.context.tex0.tbw=4;
   state.texa.ta0=37;state.texa.ta1=201;state.texa.aem=true;
   backend.DecodeTexture(state,256,256,p);const auto td=std::chrono::steady_clock::now();
   for(unsigned i=0;i<240;i++){backend.TextureFlush();backend.DecodeTexture(state,256,256,p);}
   std::cout<<"240x256x256 psm="<<unsigned(psm)<<" texture decode="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-td).count()<<"s\n";
  }
  GSDrawState mainTexture{};mainTexture.context.tex0.psm=GS_PSM_CT32;
  mainTexture.context.tex0.tbp0=0;mainTexture.context.tex0.tbw=10;
  const auto tm=std::chrono::steady_clock::now();
  for(unsigned i=0;i<120;i++){backend.TextureFlush();backend.DecodeTexture(mainTexture,640,448,p);}
  std::cout<<"120x640x448 CT32 main texture decode="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-tm).count()<<"s\n";
 }
}
