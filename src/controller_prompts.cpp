#include "controller_prompts.h"
#include <raylib.h>
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ControllerPrompts {
namespace {
struct Patch {
    uint32_t x=0,y=0,width=0,height=0;
    std::vector<uint8_t> rgba;
};
struct Atlas {
    uint64_t hash=0;
    uint32_t width=0,height=0;
    std::vector<Patch> patches;
};
std::vector<Atlas> atlases;
std::string selected=defaultProfile();
[[noreturn]] void invalid(const std::string& reason) {
    throw std::runtime_error("Controller prompts: "+reason);
}
bool filename(const std::string& name) {
    if(name.empty() || name.size()>128 || !name.ends_with(".png"))return false;
    return std::all_of(name.begin(),name.end(),[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='0' && c<='9') || c=='_' || c=='-' || c=='.';
    }) && name.find("..") == std::string::npos;
}
std::vector<uint8_t> load(const std::filesystem::path& path,uint32_t width,uint32_t height) {
    if(!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path)>4*1024*1024)
        invalid("missing or oversized image: "+path.string());
    Image image=LoadImage(path.string().c_str());
    struct Release {Image& image;~Release(){if(image.data)UnloadImage(image);}} release{image};
    if(!image.data || image.width!=int(width) || image.height!=int(height))
        invalid("incorrect image dimensions: "+path.string());
    ImageFormat(&image,PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    if(!image.data || image.format!=PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
        invalid("cannot decode image: "+path.string());
    const auto* bytes=static_cast<const uint8_t*>(image.data);
    return {bytes,bytes+size_t(width)*height*4};
}
}

const char* defaultProfile() {
#ifdef NIGHT100_DEFAULT_CONTROLLER_PROFILE
    return NIGHT100_DEFAULT_CONTROLLER_PROFILE;
#elif defined(__SWITCH__)
    return "sw";
#else
    return "ps";
#endif
}
const char* profile() {return selected.c_str();}
uint64_t fingerprint(std::span<const uint8_t> rgba) {
    uint64_t hash=14695981039346656037ull;
    for(uint8_t value:rgba){hash^=value;hash*=1099511628211ull;}
    return hash;
}
void configure(std::string_view requested,const std::filesystem::path& directory) {
    if(requested!="ps" && requested!="sw")invalid("choose ps or sw");
    if(requested=="ps"){atlases.clear();selected="ps";return;}
    const auto path=directory/"prompts.map";
    std::ifstream file(path);
    if(!file || std::filesystem::file_size(path)>128*1024)invalid("missing or oversized map: "+path.string());
    std::string magic;unsigned version=0;
    if(!(file>>magic>>version) || magic!="NIGHT100_PROMPTS" || version!=1)invalid("unsupported map header");
    std::string line;std::getline(file,line);
    if(line.find_first_not_of(" \t\r")!=std::string::npos)invalid("trailing map header fields");
    std::vector<Atlas> next;
    unsigned count=0;
    while(std::getline(file,line)) {
        if(line.empty() || line[0]=='#')continue;
        if(++count>256)invalid("too many replacements");
        std::istringstream row(line);
        std::string hashText,psImage,swImage,trailing;
        uint32_t width=0,height=0;Patch patch;
        if(!(row>>hashText>>width>>height>>patch.x>>patch.y>>patch.width>>patch.height>>psImage>>swImage) || row>>trailing)
            invalid("malformed replacement row");
        uint64_t hash=0;const auto parsed=std::from_chars(hashText.data(),hashText.data()+hashText.size(),hash,16);
        if(parsed.ec!=std::errc{} || parsed.ptr!=hashText.data()+hashText.size() || !width || !height || width>2048 || height>2048 ||
           !patch.width || !patch.height || patch.width>width || patch.height>height || patch.x>width-patch.width || patch.y>height-patch.height ||
           !filename(psImage) || !filename(swImage))invalid("invalid atlas bounds, fingerprint or image name");
        auto atlas=std::find_if(next.begin(),next.end(),[&](const Atlas& value){return value.hash==hash && value.width==width && value.height==height;});
        if(atlas==next.end()){next.push_back({hash,width,height,{}});atlas=std::prev(next.end());}
        for(const auto& previous:atlas->patches)
            if(patch.x<previous.x+previous.width && previous.x<patch.x+patch.width &&
               patch.y<previous.y+previous.height && previous.y<patch.y+patch.height)invalid("overlapping replacements");
        patch.rgba=load(directory/"sw"/swImage,patch.width,patch.height);
        atlas->patches.push_back(std::move(patch));
    }
    if(!file.eof() || !count)invalid("empty or unreadable replacement map");
    atlases=std::move(next);selected="sw";
}
bool apply(uint32_t width,uint32_t height,std::span<uint8_t> rgba) {
    if(selected!="sw" || rgba.size()!=size_t(width)*height*4)return false;
    bool possible=false;
    for(const auto& atlas:atlases)if(atlas.width==width && atlas.height==height){possible=true;break;}
    if(!possible)return false;
    const uint64_t hash=fingerprint(rgba);
    if(std::getenv("NIGHT100_TRACE_CONTROLLER"))
        std::cerr<<"[controller-prompts] atlas="<<width<<'x'<<height<<" fingerprint="<<std::hex<<hash<<std::dec<<'\n';
    for(const auto& atlas:atlases)if(atlas.width==width && atlas.height==height && atlas.hash==hash) {
        for(const auto& patch:atlas.patches)for(uint32_t y=0;y<patch.height;++y)
            std::copy_n(patch.rgba.data()+size_t(y)*patch.width*4,size_t(patch.width)*4,
                        rgba.data()+(size_t(y+patch.y)*width+patch.x)*4);
        if(std::getenv("NIGHT100_TRACE_CONTROLLER"))
            std::cerr<<"[controller-prompts] applied "<<atlas.patches.size()<<" Switch buttons\n";
        return true;
    }
    return false;
}
}
