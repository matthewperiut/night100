#include "resource_aliases.h"
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>

namespace ResourceAliases {
namespace {
constexpr size_t maxBytes=256*1024,maxEntries=4096,maxPath=1024;
[[noreturn]] void invalid(const char* why){throw std::runtime_error(std::string("Resource aliases: ")+why);}
std::string folded(std::string_view text) {
    std::string result(text);
    for(char& c:result)if(c>='a' && c<='z')c=char(c-'a'+'A');
    return result;
}
void validatePath(std::string_view path) {
    if(path.empty() || path.size()>maxPath || path.front()=='/' || path.back()=='/')invalid("expected a bounded relative path");
    size_t component=0;
    for(size_t i=0;i<=path.size();++i) {
        if(i==path.size() || path[i]=='/') {
            const auto part=path.substr(component,i-component);
            if(part.empty() || part=="." || part=="..")invalid("invalid or escaping path component");
            component=i+1;
        } else {
            const unsigned char c=path[i];
            if(c<32 || c>=127 || c==':' || c=='\\')invalid("invalid path byte or device prefix");
        }
    }
}
Table load(const std::filesystem::path& root) {
    const auto path=root/".asset-aliases";
    if(!std::filesystem::exists(path))return {};
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file)invalid("cannot open table");
    const auto size=file.tellg();
    if(size<0 || uint64_t(size)>maxBytes)invalid("table exceeds size limit");
    std::string text(size_t(size),'\0');file.seekg(0);
    if(!text.empty() && !file.read(text.data(),std::streamsize(text.size())))invalid("incomplete table read");
    return Table::parse(text);
}
}
Table Table::parse(std::string_view text) {
    if(text.size()>maxBytes)invalid("table exceeds size limit");
    Table result;
    while(!text.empty()) {
        const auto end=text.find('\n');
        auto line=text.substr(0,end);
        if(end==std::string_view::npos)text={};else text.remove_prefix(end+1);
        if(!line.empty() && line.back()=='\r')line.remove_suffix(1);
        if(line.empty())continue;
        const auto tab=line.find('\t');
        if(tab==std::string_view::npos || line.find('\t',tab+1)!=std::string_view::npos)invalid("expected exactly two TSV fields");
        const auto alias=line.substr(0,tab),target=line.substr(tab+1);
        validatePath(alias);validatePath(target);
        const auto key=folded(alias);
        if(key==folded(target))invalid("self alias");
        if(result.entries.size()>=maxEntries)invalid("too many records");
        if(!result.entries.emplace(key,std::string(target)).second)invalid("duplicate alias");
    }
    for(const auto& [key,target]:result.entries)
        if(result.entries.contains(folded(target)))invalid("alias target must be canonical, not another alias");
    return result;
}
std::string Table::resolve(std::string_view relative) const {
    validatePath(relative);
    const auto entry=entries.find(folded(relative));
    return entry==entries.end()?std::string(relative):entry->second;
}
std::string resolve(const std::filesystem::path& cdRoot,std::string_view relative) {
    static std::mutex mutex;
    static std::map<std::filesystem::path,Table> cache;
    std::lock_guard lock(mutex);
    const auto key=cdRoot.lexically_normal();
    auto found=cache.find(key);
    if(found==cache.end())found=cache.emplace(key,load(key)).first;
    return found->second.resolve(relative);
}
}
