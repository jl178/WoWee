#include "game/glue_rules.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace wowee {
namespace game {
namespace glue {

namespace {

std::string upper(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// RaceLights, row for row from GlueParent.lua (3.3.5a). Each row is
// {enabled, omni, dirX, dirY, dirZ, ambIntensity, ambR, ambG, ambB,
//  dirIntensity, dirR, dirG, dirB}; the enabled column is 1 throughout.
SceneLight L(float omni, float dx, float dy, float dz, float ai, float ar, float ag, float ab,
             float di, float dr, float dg, float db) {
    SceneLight l;
    l.omni = omni != 0.0f;
    l.dir = glm::vec3(dx, dy, dz);
    l.ambIntensity = ai;
    l.ambColor = glm::vec3(ar, ag, ab);
    l.dirIntensity = di;
    l.dirColor = glm::vec3(dr, dg, db);
    return l;
}

struct RaceLightRow {
    const char* key;
    std::vector<SceneLight> lights;
};

const std::vector<RaceLightRow>& raceLightTable() {
    static const std::vector<RaceLightRow> table = {
        {"HUMAN", {
            L(0, 0.0f, 0.0f, -1.0f, 1.0f, 0.27f, 0.27f, 0.27f, 1.0f, 0, 0, 0),
            L(0, -0.45756075f, -0.58900136f, -0.66611975f, 1.0f, 0, 0, 0, 1.0f, 0.19882353f, 0.34921569f, 0.43588236f),
            L(0, -0.64623469f, 0.57582057f, -0.50081086f, 1.0f, 0, 0, 0, 2.0f, 0.52196085f, 0.44f, 0.29764709f),
        }},
        {"ORC", {
            L(0, 0.0f, 0.0f, -1.0f, 1.0f, 0.15f, 0.15f, 0.15f, 1.0f, 0, 0, 0),
            L(0, -0.74919f, 0.35208f, -0.56103f, 1.0f, 0, 0, 0, 1.0f, 0.44706f, 0.54510f, 0.73725f),
            L(0, 0.53162f, -0.84340f, 0.07780f, 1.0f, 0, 0, 0, 2.0f, 0.55f, 0.338625f, 0.148825f),
        }},
        {"DWARF", {
            L(0, -0.0f, -0.0f, -1.0f, 1.0f, 0.3f, 0.3f, 0.3f, 0.0f, 0, 0, 0),
            L(0, -0.88314f, 0.42916f, -0.18945f, 1.0f, 0, 0, 0, 2.0f, 0.44706f, 0.67451f, 0.760785f),
        }},
        {"TAUREN", {
            L(0, -0.48073f, 0.71827f, -0.50297f, 1.0f, 0, 0, 0, 2.0f, 0.65f, 0.397645f, 0.2727f),
            L(0, -0.49767f, -0.78677f, 0.36513f, 1.0f, 0, 0, 0, 1.0f, 0.6f, 0.47059f, 0.32471f),
        }},
        {"SCOURGE", {
            L(0, 0.0f, 0.0f, -1.0f, 1.0f, 0.2f, 0.2f, 0.2f, 1.0f, 0, 0, 0),
        }},
        {"NIGHTELF", {
            L(0, -0.0f, -0.0f, -1.0f, 1.0f, 0.0902f, 0.0902f, 0.1702f, 1.0f, 0, 0, 0),
        }},
        {"DRAENEI", {
            L(0, 0.61185f, 0.62942f, -0.47903f, 1.0f, 0, 0, 0, 1.0f, 0.56941f, 0.52f, 0.6f),
            L(0, -0.64345f, -0.31052f, -0.69968f, 1.0f, 0, 0, 0, 1.0f, 0.60941f, 0.60392f, 0.7f),
            L(0, -0.46481f, -0.14320f, 0.87376f, 1.0f, 0, 0, 0, 2.0f, 0.5835f, 0.48941f, 0.6f),
        }},
        {"BLOODELF", {
            L(0, -0.82249f, -0.54912f, -0.14822f, 1.0f, 0, 0, 0, 2.0f, 0.581175f, 0.50588f, 0.42588f),
            L(0, 0.0f, -0.0f, -1.0f, 1.0f, 0.60392f, 0.61490f, 0.7f, 1.0f, 0, 0, 0),
            L(0, 0.02575f, 0.86518f, -0.50081f, 1.0f, 0, 0, 0, 1.0f, 0.59137f, 0.51745f, 0.63471f),
        }},
        {"DEATHKNIGHT", {
            L(0, 0.0f, 0.0f, -1.0f, 1.0f, 0.38824f, 0.66353f, 0.76941f, 1.0f, 0, 0, 0),
        }},
        {"CHARACTERSELECT", {
            L(0, 0.0f, 0.0f, -1.0f, 1.0f, 0.15f, 0.15f, 0.15f, 1.0f, 0, 0, 0),
            L(0, -0.74919f, 0.35208f, -0.56103f, 1.0f, 0, 0, 0, 1.0f, 0.44706f, 0.54510f, 0.73725f),
            L(0, 0.53162f, -0.84340f, 0.07780f, 1.0f, 0, 0, 0, 2.0f, 0.55f, 0.338625f, 0.148825f),
        }},
    };
    return table;
}

} // namespace

std::string sceneKey(std::string_view sceneName) {
    if (sceneName.size() > 3 && (sceneName[0] == 'U' || sceneName[0] == 'u') &&
        (sceneName[1] == 'I' || sceneName[1] == 'i') && sceneName[2] == '_') {
        sceneName.remove_prefix(3);
    }
    return upper(sceneName);
}

const std::vector<SceneLight>& raceLights(std::string_view key) {
    static const std::vector<SceneLight> none;
    const std::string k = upper(key);
    for (const auto& row : raceLightTable()) {
        if (k == row.key) return row.lights;
    }
    return none;
}

std::string_view ambienceForScene(std::string_view key) {
    // GlueAmbienceTracks, GlueParent.lua.
    static constexpr std::pair<const char*, const char*> kTracks[] = {
        {"HUMAN", "GlueScreenHuman"},
        {"ORC", "GlueScreenOrcTroll"},
        {"DWARF", "GlueScreenDwarfGnome"},
        {"TAUREN", "GlueScreenTauren"},
        {"SCOURGE", "GlueScreenUndead"},
        {"NIGHTELF", "GlueScreenNightElf"},
        {"DRAENEI", "GlueScreenDraenei"},
        {"BLOODELF", "GlueScreenBloodElf"},
        {"DARKPORTAL", "GlueScreenIntro"},
        {"DEATHKNIGHT", "GlueScreenIntro"},
        {"CHARACTERSELECT", "GlueScreenIntro"},
    };
    const std::string k = upper(key);
    for (const auto& [name, track] : kTracks) {
        if (k == name) return track;
    }
    return {};
}

float verticalFovFromDiagonal(float diagonalRadians, float aspect) {
    if (!(aspect > 0.0f) || !std::isfinite(aspect)) aspect = 4.0f / 3.0f;
    aspect = std::min(aspect, kMaxGlueAspect);
    return diagonalRadians / std::sqrt(1.0f + aspect * aspect);
}

bool useWideLoadingArt(float aspect) {
    return aspect > 4.0f / 3.0f + 0.001f;
}

Rect loadingImageRect(float screenW, float screenH, bool wideArt) {
    Rect r{0.0f, 0.0f, screenW, screenH};
    if (screenW <= 0.0f || screenH <= 0.0f) return r;
    // FUN_0047bfe0 is the window's aspect over 4:3; the wide art's own is
    // 1.6 (0xab63b8) over 4:3 (0xab63b4).
    const float imageAspect = wideArt ? 1.6f : 4.0f / 3.0f;
    const float ratio = (screenW / screenH) / imageAspect;
    if (ratio > 1.0f) {
        const float w = screenW / ratio;
        r.x0 = (screenW - w) * 0.5f;
        r.x1 = r.x0 + w;
    } else if (ratio < 1.0f) {
        const float h = screenH * ratio;
        r.y0 = (screenH - h) * 0.5f;
        r.y1 = r.y0 + h;
    }
    return r;
}

LoadingBar loadingBarRects(const Rect& image, float progress) {
    progress = std::clamp(progress, 0.0f, 1.0f);
    const float w = image.width();
    const float h = image.height();
    // The table's y runs up from the bottom of the image.
    const float cx = image.x0 + 0.5f * w;
    const float cy = image.y1 - 0.075f * h;
    LoadingBar bar;
    bar.border = Rect{cx - 0.3f * w, cy - 0.025f * h, cx + 0.3f * w, cy + 0.025f * h};
    const float fx0 = cx - 0.2625f * w;
    bar.fill = Rect{fx0, cy - 0.0125f * h, fx0 + 0.525f * w * progress, cy + 0.0125f * h};
    return bar;
}

void NameChain::build(const std::vector<std::string>& names) {
    pieces_.clear();
    for (const auto& raw : names) {
        std::string s = "_" + raw;
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (s.size() > 63) s.resize(63);
        // i < strlen - 1: every three-letter window, then the final pair.
        for (size_t i = 0; i + 1 < s.size(); ++i) {
            Piece p;
            const size_t n = std::min<size_t>(3, s.size() - i);
            std::memcpy(p.key, s.data() + i, n);
            auto it = std::find_if(pieces_.begin(), pieces_.end(), [&](const Piece& q) {
                return std::strcmp(q.key, p.key) == 0;
            });
            if (it != pieces_.end()) {
                ++it->count;
            } else {
                p.count = 1;
                pieces_.push_back(p);
            }
        }
    }
    std::sort(pieces_.begin(), pieces_.end(),
              [](const Piece& a, const Piece& b) { return std::strcmp(a.key, b.key) < 0; });
}

std::string NameChain::draw(std::mt19937& rng) const {
    if (pieces_.empty()) return {};
    constexpr size_t kMaxLen = 14;  // the caller's buffer, 0xe
    std::string buf;
    while (true) {
        // The run of pieces that may come next: those starting "_" for the
        // first, else those whose first two letters are the name's last two.
        size_t lo = pieces_.size(), hi = 0;
        bool found = false;
        for (size_t i = 0; i < pieces_.size(); ++i) {
            const bool match = buf.empty()
                ? pieces_[i].key[0] == '_'
                : std::strncmp(pieces_[i].key, buf.c_str() + buf.size() - 2, 2) == 0;
            if (match) {
                lo = std::min(lo, i);
                hi = std::max(hi, i);
                found = true;
            } else if (found) {
                break;
            }
        }
        if (!found) break;
        uint64_t total = 0;
        for (size_t i = lo; i <= hi; ++i) total += pieces_[i].count;
        if (total == 0) break;
        uint64_t r = std::uniform_int_distribution<uint64_t>(0, total - 1)(rng);
        size_t pick = lo;
        for (size_t i = lo; i <= hi; ++i) {
            if (r < pieces_[i].count) { pick = i; break; }
            r -= pieces_[i].count;
        }
        const Piece& p = pieces_[pick];
        if (buf.empty()) {
            buf.assign(p.key);
            continue;
        }
        if (p.key[2] == '\0') break;  // a final pair: the name ends here
        buf.push_back(p.key[2]);
        if (buf.size() >= kMaxLen - 1) break;
    }
    if (!buf.empty() && buf[0] == '_') buf.erase(buf.begin());
    if (!buf.empty()) buf[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(buf[0])));
    return buf;
}

bool plausibleCharacterName(std::string_view name) {
    if (name.empty()) return false;
    int run = 0;
    char last = 0;
    for (char c : name) {
        if (!std::isalpha(static_cast<unsigned char>(c))) return false;
        const char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        run = (lc == last) ? run + 1 : 1;
        if (run >= 3) return false;
        last = lc;
    }
    return true;
}

} // namespace glue
} // namespace game
} // namespace wowee
