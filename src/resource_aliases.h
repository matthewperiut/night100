#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ResourceAliases {
// Canonical resource names on filesystems without symbolic links. The table
// contains only relative disc-root paths, with direct targets and ASCII folding.
class Table {
public:
    static Table parse(std::string_view text);
    std::string resolve(std::string_view relative) const;
private:
    std::unordered_map<std::string,std::string> entries;
};
// Loads cdRoot/.asset-aliases at most once per root; a missing table is valid.
// Malformed or escaping records fail explicitly before accessing a target.
std::string resolve(const std::filesystem::path& cdRoot,std::string_view relative);
}
