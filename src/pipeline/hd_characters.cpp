#include "pipeline/hd_characters.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"

namespace wowee {
namespace pipeline {

namespace {

std::atomic<bool> gHdCharacters{false};

std::string lowerKey(std::string s) {
    std::replace(s.begin(), s.end(), '/', '\\');
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

void setHdCharactersEnabled(bool on) { gHdCharacters.store(on, std::memory_order_relaxed); }
bool hdCharactersEnabled() { return gHdCharacters.load(std::memory_order_relaxed); }

bool isHdCharacterModelPath(const std::string& m2Path) {
    return m2Path.size() > 3 && (m2Path[0] == 'H' || m2Path[0] == 'h') &&
           (m2Path[1] == 'D' || m2Path[1] == 'd') && (m2Path[2] == '\\' || m2Path[2] == '/');
}

std::string hdCharacterModelPath(const AssetManager& assets, const std::string& m2Path) {
    if (!hdCharactersEnabled() || m2Path.empty() || isHdCharacterModelPath(m2Path)) return m2Path;
    std::string hd = std::string(kHdCharacterPrefix) + m2Path;
    return assets.fileExists(hd) ? hd : m2Path;
}

std::shared_ptr<const HdCharacterLook> loadHdCharacterLook(AssetManager& assets,
                                                           const std::string& hdM2Path) {
    if (!isHdCharacterModelPath(hdM2Path)) return nullptr;
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const HdCharacterLook>> cache;
    const std::string key = lowerKey(hdM2Path);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
    }
    std::string path = hdM2Path;
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) path.resize(dot);
    path += ".hdlook";
    std::shared_ptr<const HdCharacterLook> look;
    auto data = assets.readFile(path);
    if (data.empty()) {
        LOG_WARNING("HD characters: no appearance table ", path, " - this model draws untextured");
    } else {
        look = std::make_shared<HdCharacterLook>(
            parseHdCharacterLook(std::string(data.begin(), data.end())));
    }
    std::lock_guard<std::mutex> lock(mutex);
    cache[key] = look;
    return look;
}

}  // namespace pipeline
}  // namespace wowee
