#include "pipeline/m2_asset_loader.hpp"
#include "core/hitch_legacy.hpp"

#include <algorithm>
#include <cstdio>
#include <list>
#include <mutex>
#include <unordered_map>

#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"

namespace wowee {
namespace pipeline {

namespace {

bool parseM2WithSkin(AssetManager& assets, const std::string& m2Path, M2Model& outModel) {
    auto m2Data = assets.readFile(m2Path);
    if (m2Data.empty()) return false;

    outModel = M2Loader::load(m2Data);
    // Everything downstream identifies a model by its name, and a model that
    // does not carry one is identified by nothing.
    if (outModel.name.empty()) outModel.name = m2Path;

    if (outModel.version >= 264) {
        auto skinData = assets.readFile(skinPathForM2(m2Path));
        if (!skinData.empty()) M2Loader::loadSkin(skinData, outModel);
    }
    return outModel.isValid();
}

// Item models are small (a few hundred KB parsed at most) and a city holds a
// few hundred distinct ones; 512 covers a battleground's worth of gear.
constexpr size_t kM2CacheCap = 512;

struct M2Cache {
    std::mutex mutex;
    struct Entry {
        std::shared_ptr<const M2Model> model;
        std::list<std::string>::iterator lru;
    };
    std::unordered_map<std::string, Entry> byKey;
    std::list<std::string> lru;  // front = most recent
};

M2Cache& m2Cache() {
    static M2Cache cache;
    return cache;
}

std::string cacheKeyFor(const AssetManager& assets, const std::string& m2Path) {
    std::string key = m2Path;
    std::replace(key.begin(), key.end(), '/', '\\');
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    char tag[32];
    std::snprintf(tag, sizeof(tag), "%p|", static_cast<const void*>(&assets));
    return tag + key;
}

}  // namespace

std::shared_ptr<const M2Model> loadM2WithSkinShared(AssetManager& assets, const std::string& m2Path) {
    if (m2Path.empty()) return nullptr;
    if (core::hitchLegacy()) {
        auto model = std::make_shared<M2Model>();
        if (!parseM2WithSkin(assets, m2Path, *model)) return nullptr;
        return model;
    }
    const std::string key = cacheKeyFor(assets, m2Path);
    auto& cache = m2Cache();
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        auto it = cache.byKey.find(key);
        if (it != cache.byKey.end()) {
            cache.lru.splice(cache.lru.begin(), cache.lru, it->second.lru);
            return it->second.model;
        }
    }
    // Parsed outside the lock: two threads asking for the same new model both
    // parse it, which is wasted work but not wrong, and nobody waits on a parse
    // they did not ask for.
    auto model = std::make_shared<M2Model>();
    if (!parseM2WithSkin(assets, m2Path, *model)) return nullptr;
    std::shared_ptr<const M2Model> shared = std::move(model);
    std::lock_guard<std::mutex> lock(cache.mutex);
    auto it = cache.byKey.find(key);
    if (it != cache.byKey.end()) {
        cache.lru.splice(cache.lru.begin(), cache.lru, it->second.lru);
        return it->second.model;
    }
    cache.lru.push_front(key);
    cache.byKey.emplace(key, M2Cache::Entry{shared, cache.lru.begin()});
    while (cache.byKey.size() > kM2CacheCap) {
        cache.byKey.erase(cache.lru.back());
        cache.lru.pop_back();
    }
    return shared;
}

void prefetchM2WithSkin(AssetManager& assets, const std::string& m2Path) {
    (void)loadM2WithSkinShared(assets, m2Path);
}

void clearM2ModelCache() {
    auto& cache = m2Cache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.byKey.clear();
    cache.lru.clear();
}

size_t m2ModelCacheSize() {
    auto& cache = m2Cache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    return cache.byKey.size();
}

bool loadM2WithSkin(AssetManager& assets, const std::string& m2Path, M2Model& outModel) {
    auto shared = loadM2WithSkinShared(assets, m2Path);
    if (!shared) {
        // As before: the caller's model is whatever a failed load left. The
        // callers test the return and reset it themselves.
        outModel = M2Model{};
        return false;
    }
    outModel = *shared;
    return true;
}

std::string animPathForM2(const std::string& m2Path, uint32_t animId, uint32_t variationIndex) {
    const std::string base = skinPathForM2(m2Path);
    // skinPathForM2 answers "<stem>00.skin"; the stem is what is wanted here.
    const std::string stem = base.substr(0, base.size() - 7);
    char tail[32];
    std::snprintf(tail, sizeof(tail), "%04u-%02u.anim", animId, variationIndex);
    return stem + tail;
}

void loadExternalAnimations(AssetManager& assets, const std::string& m2Path,
                            const std::vector<uint8_t>& m2Data, M2Model& model,
                            std::initializer_list<uint32_t> wantedAnimIds) {
    for (uint32_t si = 0; si < model.sequences.size(); si++) {
        // 0x20 means the keyframes are inside the M2 already.
        if (model.sequences[si].flags & 0x20) continue;
        const uint32_t animId = model.sequences[si].id;
        if (wantedAnimIds.size() > 0 &&
            std::find(wantedAnimIds.begin(), wantedAnimIds.end(), animId) == wantedAnimIds.end()) {
            continue;
        }
        auto animData = assets.readFileOptional(
            animPathForM2(m2Path, animId, model.sequences[si].variationIndex));
        if (!animData.empty()) M2Loader::loadAnimFile(m2Data, animData, si, model);
    }
}

}  // namespace pipeline
}  // namespace wowee
