#include "runtime/resource_disc.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <zlib.h>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void number(std::vector<uint8_t>& out,uint64_t value,size_t width){for(size_t i=0;i<width;++i)out.push_back(static_cast<uint8_t>(value>>(i*8)));}
void write(const std::filesystem::path& path,const std::vector<uint8_t>& data){
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));require(bool(file),"fixture file write failed");
}
struct Extent {uint64_t logical,length,offset,stored;uint32_t encoding;std::string path;};
std::vector<uint8_t> manifest(uint64_t length,const std::vector<Extent>& extents){
    std::vector<uint8_t> out{'N','1','0','0','D','S','C','1'};number(out,length,8);number(out,extents.size(),4);
    for(const auto& e:extents){number(out,e.logical,8);number(out,e.length,8);number(out,e.offset,8);number(out,e.stored,8);number(out,e.encoding,4);number(out,e.path.size(),4);out.insert(out.end(),e.path.begin(),e.path.end());}
    return out;
}
int comparePackage(const std::filesystem::path& package,const std::filesystem::path& iso){
    auto disc=ps2x::ResourceDisc::open(package);require(bool(disc),"compact disc not recognized");
    require(disc->size()==std::filesystem::file_size(iso),"original ISO logical size differs");
    std::ifstream original(iso,std::ios::binary);require(bool(original),"cannot open original ISO");
    auto descriptor=std::filesystem::is_directory(package)?package/".disc-image":package;
    std::ifstream layout(descriptor,std::ios::binary);layout.seekg(8);
    auto get=[&](size_t width){uint64_t value=0;for(size_t i=0;i<width;++i){const int byte=layout.get();require(byte!=EOF,"truncated comparison manifest");value|=uint64_t(byte)<<(8*i);}return value;};
    require(get(8)==disc->size(),"descriptor size differs");const uint64_t count=get(4);
    std::vector<std::pair<uint64_t,size_t>> ranges;
    for(uint64_t i=0;i<count;++i){
        const uint64_t begin=get(8),length=get(8);(void)get(8);(void)get(8);(void)get(4);const uint64_t pathLength=get(4);
        layout.seekg(static_cast<std::streamoff>(pathLength),std::ios::cur);require(bool(layout),"invalid comparison manifest path");
        for(uint64_t boundary:{begin,begin+length})ranges.emplace_back(boundary>2048?boundary-2048:0,4096);
    }
    std::mt19937_64 rng(0x4e31303044534331ull);
    for(unsigned i=0;i<3000;++i)ranges.emplace_back(rng()%disc->size(),static_cast<size_t>(1+rng()%65536));
    uint64_t checked=0;
    for(const auto& [offset,length]:ranges){
        std::vector<uint8_t> actual(length,0x71),expected(length,0x71);size_t read=0;
        require(disc->read(offset,actual.data(),length,read),"compact comparison read failed");
        original.clear();original.seekg(static_cast<std::streamoff>(offset));original.read(reinterpret_cast<char*>(expected.data()),static_cast<std::streamsize>(length));
        require(read==static_cast<size_t>(original.gcount())&&actual==expected,"compact range differs from original ISO");checked+=read;
    }
    std::cout<<"PASS: C++ compact reader matches original ISO across "<<ranges.size()<<" ranges (every extent boundary + 3000 random ranges), "<<checked<<" bytes compared\n";
    return 0;
}
}
int main(int argc,char** argv){
    std::filesystem::path root;
    try{
        if(argc==4 && std::string(argv[1])=="--compare")return comparePackage(argv[2],argv[3]);
        require(argc==1,"usage: resource_disc_checks [--compare PACKAGE ORIGINAL_ISO]");
        root=std::filesystem::temp_directory_path()/("night100-resource-disc-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        std::vector<uint8_t> first(317),decoded(65536),tail(123);
        for(size_t i=0;i<first.size();++i)first[i]=static_cast<uint8_t>(i*13);
        for(size_t i=0;i<decoded.size();++i)decoded[i]=static_cast<uint8_t>((i/17)%11);
        for(size_t i=0;i<tail.size();++i)tail[i]=static_cast<uint8_t>(255-i);
        std::vector<uint8_t> packed(compressBound(decoded.size()));uLongf packedSize=packed.size();
        require(compress2(packed.data(),&packedSize,decoded.data(),decoded.size(),1)==Z_OK,"compression failed");packed.resize(packedSize);
        std::vector<uint8_t> prefixed(9,0xa7);prefixed.insert(prefixed.end(),first.begin(),first.end());
        write(root/"asset.hip",prefixed);write(root/"gaps.bin",packed);write(root/"tail.bin",tail);
        std::vector<uint8_t> reference=first;reference.resize(first.size()+9000);reference.insert(reference.end(),decoded.begin(),decoded.end());reference.insert(reference.end(),tail.begin(),tail.end());
        std::vector<Extent> extents{{0,first.size(),9,first.size(),0,"asset.hip"},{first.size(),9000,0,0,1,""},
            {first.size()+9000,decoded.size(),0,packed.size(),2,"gaps.bin"},
            {first.size()+9000+decoded.size(),tail.size(),0,tail.size(),0,"tail.bin"}};
        write(root/".disc-image",manifest(reference.size(),extents));
        auto disc=ps2x::ResourceDisc::open(root);require(bool(disc),"directory manifest not recognized");
        require(disc->size()==reference.size(),"logical size mismatch");
        require(ps2x::ResourceDisc::open(root/".disc-image")==disc,"manifest cache not shared");
        require(!ps2x::ResourceDisc::open(root/"asset.hip"),"ordinary file mistaken for disc");
        require(!ps2x::ResourceDisc::open(root/"missing"),"missing file mistaken for disc");
        size_t count=55;require(disc->read(0,nullptr,0,count)&&count==0,"zero read failed");
        require(!disc->read(0,nullptr,1,count)&&count==0,"null output accepted");
        uint8_t sentinel=99;require(disc->read(UINT64_MAX,&sentinel,1,count)&&count==0&&sentinel==99,"large EOF read failed");
        std::vector<uint8_t> all(reference.size()+17,77);
        require(disc->read(0,all.data(),all.size(),count)&&count==reference.size(),"cross-extent short read failed");
        require(std::equal(reference.begin(),reference.end(),all.begin())&&all.back()==77,"logical disc reconstruction mismatch");
        auto checkReads=[&](unsigned seed){std::mt19937 rng(seed);for(int i=0;i<2000;++i){
            const size_t offset=rng()%(reference.size()+25),length=rng()%12000;
            std::vector<uint8_t> out(length+1,88);size_t read=0;
            if(!disc->read(offset,out.data(),length,read))return false;
            const size_t expected=offset>=reference.size()?0:std::min(length,reference.size()-offset);
            if(read!=expected || out.back()!=88 || (expected&&!std::equal(out.begin(),out.begin()+expected,reference.begin()+offset)))return false;
        }return true;};
        require(checkReads(42),"random logical ranges mismatch");
        std::atomic<bool> parallel{true};std::vector<std::thread> readers;
        for(unsigned i=0;i<4;++i)readers.emplace_back([&,i]{if(!checkReads(91+i))parallel=false;});
        for(auto& thread:readers)thread.join();require(parallel,"concurrent reads mismatch");
        unsigned cases=0;
        auto reject=[&](std::vector<Extent> bad,uint64_t size,std::vector<uint8_t> suffix={}){
            const auto dir=root/("bad-"+std::to_string(cases++));std::filesystem::create_directory(dir);
            for(const char* name:{"asset.hip","gaps.bin","tail.bin"})std::filesystem::copy_file(root/name,dir/name);
            auto bytes=manifest(size,bad);bytes.insert(bytes.end(),suffix.begin(),suffix.end());write(dir/".disc-image",bytes);
            bool rejected=false;try{(void)ps2x::ResourceDisc::open(dir);}catch(const std::exception&){rejected=true;}require(rejected,"malformed manifest accepted");
        };
        auto bad=extents;bad[1].logical++;reject(bad,reference.size());
        bad=extents;bad[1].length=UINT64_MAX;reject(bad,reference.size());
        bad=extents;bad[0].offset=UINT64_MAX;reject(bad,reference.size());
        bad=extents;bad[0].stored--;reject(bad,reference.size());
        bad=extents;bad[0].path="../asset.hip";reject(bad,reference.size());
        bad=extents;bad[0].path="/asset.hip";reject(bad,reference.size());
        bad=extents;bad[0].path="missing";reject(bad,reference.size());
        bad=extents;bad[1].path="asset.hip";reject(bad,reference.size());
        bad=extents;bad[2].encoding=3;reject(bad,reference.size());
        bad=extents;bad[2].length=1024*1024+1;reject(bad,reference.size()+1024*1024);
        reject(extents,reference.size()+1);reject(extents,reference.size(),{0});reject({},reference.size());
        // Corrupt compressed bytes are recognized but fail reads, never become
        // silent zero-fill or unbounded allocations.
        const auto corrupt=root/"corrupt";std::filesystem::create_directory(corrupt);
        auto corruptPacked=packed;corruptPacked[0]^=0x55;write(corrupt/"gaps.bin",corruptPacked);
        write(corrupt/".disc-image",manifest(decoded.size(),{{0,decoded.size(),0,packed.size(),2,"gaps.bin"}}));
        auto broken=ps2x::ResourceDisc::open(corrupt);std::vector<uint8_t> output(100);
        require(!broken->read(0,output.data(),output.size(),count),"corrupt zlib block accepted");
        // Internal aliases are portable package paths; external symlinks must
        // never turn a manifest into a reader of files outside its own root.
        const auto aliases=root/"aliases";std::filesystem::create_directory(aliases);
        write(aliases/"real.bin",tail);std::filesystem::create_symlink("real.bin",aliases/"alias.bin");
        write(aliases/".disc-image",manifest(tail.size(),{{0,tail.size(),0,tail.size(),0,"alias.bin"}}));
        auto aliasDisc=ps2x::ResourceDisc::open(aliases);std::vector<uint8_t> aliasOut(tail.size());
        require(aliasDisc->read(0,aliasOut.data(),aliasOut.size(),count)&&aliasOut==tail,"internal alias failed");
        const auto escape=root/"escape";std::filesystem::create_directory(escape);
        std::filesystem::create_symlink("../tail.bin",escape/"outside.bin");
        write(escape/".disc-image",manifest(tail.size(),{{0,tail.size(),0,tail.size(),0,"outside.bin"}}));
        bool escaped=false;try{(void)ps2x::ResourceDisc::open(escape);}catch(const std::exception&){escaped=true;}
        require(escaped,"external source alias accepted");
        // More than sixteen payload files exercises handle eviction/reopening.
        const auto many=root/"many";std::filesystem::create_directory(many);std::vector<Extent> pieces;
        for(unsigned i=0;i<40;++i){const auto name=std::to_string(i)+".bin";write(many/name,{static_cast<uint8_t>(i)});pieces.push_back({i,1,0,1,0,name});}
        write(many/".disc-image",manifest(40,pieces));auto manyDisc=ps2x::ResourceDisc::open(many);
        for(int pass=0;pass<2;++pass){std::vector<uint8_t> result(40);require(manyDisc->read(0,result.data(),40,count)&&count==40,"eviction read failed");for(unsigned i=0;i<40;++i)require(result[i]==i,"eviction bytes mismatch");}
        std::filesystem::remove_all(root);
        std::cout<<"PASS: lossless raw/zero/zlib extents, 10000 random/concurrent ranges, EOF, "<<cases<<" malformed manifests, corrupted compression and bounded handle eviction\n";
        return 0;
    }catch(const std::exception& error){if(!root.empty())std::filesystem::remove_all(root);std::cerr<<error.what()<<'\n';return 1;}
}
