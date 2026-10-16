#include "resource_aliases.h"
#include <iostream>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {
unsigned checks=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
void rejects(std::string_view text){bool bad=false;try{ResourceAliases::Table::parse(text);}catch(const std::runtime_error&){bad=true;}require(bad,"malformed table accepted");}
}
int main(){try{
    auto table=ResourceAliases::Table::parse("FMV/DEMO/DEMO1.PSS\tFMV/DEMO/DEMO2.PSS\nFMV/DEMO/DEMO3.PSS\tFMV/DEMO/DEMO2.PSS\r\n");
    require(table.resolve("fmv/demo/demo1.pss")=="FMV/DEMO/DEMO2.PSS","case-folded alias failed");
    require(table.resolve("FMV/DEMO/DEMO3.PSS")=="FMV/DEMO/DEMO2.PSS","duplicate-content name failed");
    require(table.resolve("FMV/INTRO.PSS")=="FMV/INTRO.PSS","canonical filename changed");
    require(ResourceAliases::Table{}.resolve("FMV/INTRO.PSS")=="FMV/INTRO.PSS","absent table changed path");
    const std::string invalidPaths[]={"../escape","FMV/../escape","/absolute","sdmc:/file","FMV\\file","FMV//file","FMV/./file","FMV/",std::string("FMV/\0file",9)};
    for(const auto& path:invalidPaths){
        rejects(path+"\tFMV/DEMO.PSS\n");rejects("FMV/DEMO.PSS\t"+path+"\n");
    }
    rejects("a\ta\n");rejects("a\tA\n");rejects("a\tb\nA\tc\n");rejects("a\tb\nb\tc\n");
    rejects("a\tb\nb\ta\n");rejects("no separator\n");rejects("a\tb\tc\n");rejects("a\t\n");
    rejects(std::string(256*1024+1,'a'));rejects(std::string(1025,'a')+"\tb\n");
    std::string many;for(unsigned n=0;n<4097;++n)many+="alias"+std::to_string(n)+"\tcanonical\n";rejects(many);
    struct Fixture {
        std::filesystem::path root=std::filesystem::temp_directory_path()/("night100-aliases-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Fixture(){if(!std::filesystem::create_directory(root))throw std::runtime_error("cannot create fixture");}
        ~Fixture(){std::error_code ignored;std::filesystem::remove_all(root,ignored);}
    } fixture;
    require(ResourceAliases::resolve(fixture.root,"FMV/INTRO.PSS")=="FMV/INTRO.PSS","missing table failed");
    const auto second=fixture.root/"present";std::filesystem::create_directory(second);
    {std::ofstream f(second/".asset-aliases");f<<"FMV/DEMO/DEMO1.PSS\tFMV/DEMO/DEMO2.PSS\n";}
    require(ResourceAliases::resolve(second,"fmv/demo/demo1.pss")=="FMV/DEMO/DEMO2.PSS","on-disk alias failed");
    {std::ofstream f(second/".asset-aliases");f<<"malformed after first load";}
    require(ResourceAliases::resolve(second,"FMV/DEMO/DEMO1.PSS")=="FMV/DEMO/DEMO2.PSS","table was reread after caching");
    std::cout<<"Resource alias checks passed: "<<checks<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
