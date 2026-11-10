#pragma once

#include "pipeline/asset_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::pipeline {

/// One map's minimap tile pictures, from Textures\Minimap\md5translate.trs:
/// "<map>\map<x>_<y>.blp<TAB><hash>.blp" lines. Keyed by "map<x>_<y>" in
/// lower case, the value the hashed file name, both without ".blp". The far
/// field's colour atlas and the bounce light's ground colour both read it.
inline std::unordered_map<std::string, std::string> readMinimapTrs(AssetManager* assets,
                                                                   const std::string& map) {
    const auto lower = [](std::string v) {
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v;
    };
    const auto stripBlp = [&](std::string& v) {
        if (v.size() > 4 && lower(v.substr(v.size() - 4)) == ".blp") v.resize(v.size() - 4);
    };
    std::unordered_map<std::string, std::string> trs;
    if (!assets) return trs;
    const std::vector<uint8_t> data = assets->readFile("Textures\\Minimap\\md5translate.trs");
    const std::string prefix = lower(map) + "\\";
    std::string line;
    size_t i = 0;
    while (i < data.size()) {
        size_t j = i;
        while (j < data.size() && data[j] != '\n') ++j;
        line.assign(reinterpret_cast<const char*>(data.data()) + i, j - i);
        i = j + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string key = lower(line.substr(0, tab));
        if (key.compare(0, prefix.size(), prefix) != 0) continue;
        stripBlp(key);
        std::string hash = line.substr(tab + 1);
        stripBlp(hash);
        trs[key.substr(prefix.size())] = std::move(hash);
    }
    return trs;
}

}  // namespace wowee::pipeline
