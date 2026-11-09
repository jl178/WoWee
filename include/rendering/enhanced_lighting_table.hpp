#pragma once

/// the reference client's per-zone light keyframes, re-keyed to 3.3.5's Light.dbc: the
/// "Daylight (the reference client)" row on maps 0 and 1 (docs/re/enhanced-lighting.md
/// section 2b).
///
/// The table is Blizzard's data, converted, so it is not in the source tree.
/// tools/enhanced_light_gen.py writes it from a 3.3.5 install and Enhanced's
/// DB2 exports to the client data's override folder, where LightingManager
/// reads it as `wowee\enhanced_lights.bin`
/// (<data>/override/wowee/enhanced_lights.bin). Without it the row uses the
/// averaged trend (enhanced_palette.hpp) everywhere.
///
/// Each link names a 3.3.5 light and one of its LightParams slots (clear or
/// storm) and the profile that replaces that slot's bands: up to 16 keys
/// (Enhanced has 12 a day), each with the 18 LightIntBand colours and the
/// first four LightFloatBand values in 3.3.5's channel order and units. A
/// channel whose bit is clear in a profile's mask is one Enhanced leaves
/// empty; 3.3.5's band stays. A fog end of 0 on a key means that key has
/// none.
///
/// File layout, all little-endian 32-bit words:
///   "WFL1", keyCount, profileCount, linkCount
///   keyCount     x { time, color[18] (0x00RRGGBB), value[4] (float) }
///   profileCount x { enhancedParamsId, firstKey, keyCount, colorMask, floatMask }
///   linkCount    x { lightId, slot, profile }
/// No renderer includes.

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace wowee {
namespace rendering {
namespace enhanced_lights {

inline constexpr uint32_t kColorChannels = 18;
inline constexpr uint32_t kFloatChannels = 4;  ///< fog end, fog scaler, glow through, cloud density
inline constexpr uint32_t kMaxKeys = 16;       ///< a ColorBand's capacity
inline constexpr const char* kFileName = "wowee\\enhanced_lights.bin";

/// Enhanced's sky colours carry an exposure its linear pipeline takes back.
/// Over every linked light, 8:00 to 17:00, the sky bands' luminance against
/// 3.3.5's for the same light is x1.26-1.30 (SkyMiddle 1.26, Band1 1.30,
/// Band2 1.29, Smog 1.29, Fog 1.29, CloudSun 1.27, cloud layer 1.29; median
/// over 3762 samples each), its 75th percentile 1.30-1.31 and no higher: most
/// keys are 1.12's colour times 1.3 per channel, clipped at 255 (Westfall's
/// noon SkyMiddle `#3c919e` -> `#4ebdcd`). The sun, ambient, sun sprite and
/// shadow are not scaled (1.00-1.05). On 3.3.5's 8-bit path that 1.3 made the
/// sky round a high sun white; the sky channels are divided by it as they
/// are read (docs/re/enhanced-lighting.md section 2c).
inline constexpr float kSkyExposure = 1.3f;

/// The colour channels kSkyExposure applies to: SkyTop through SkyFog (2-7),
/// CloudSun (10) and the first cloud layer (11).
inline constexpr bool isSkyChannel(uint32_t c) { return (c >= 2 && c <= 7) || c == 10 || c == 11; }

/// A 0x00RRGGBB colour with Enhanced's sky exposure taken out.
inline constexpr uint32_t withoutSkyExposure(uint32_t rgb) {
    uint32_t out = 0;
    for (uint32_t shift = 0; shift <= 16; shift += 8) {
        const float v = static_cast<float>((rgb >> shift) & 0xFFu) / kSkyExposure;
        out |= (static_cast<uint32_t>(v + 0.5f) & 0xFFu) << shift;
    }
    return out;
}

struct Key {
    uint32_t time = 0;                    ///< half-minutes since midnight
    uint32_t color[kColorChannels] = {};  ///< 0x00RRGGBB
    float value[kFloatChannels] = {};
};

struct Profile {
    uint32_t enhancedParamsId = 0;  ///< Enhanced's LightParams id, for logs
    uint32_t firstKey = 0;         ///< into Table::keys
    uint32_t keyCount = 0;
    uint32_t colorMask = 0;        ///< bit c: Enhanced authors colour channel c
    uint32_t floatMask = 0;        ///< bit f: Enhanced authors float channel f
};

struct Link {
    uint32_t lightId = 0;  ///< 3.3.5 Light.dbc id (maps 0 and 1)
    uint32_t slot = 0;     ///< LightVolume::ParamsSlot: 0 clear, 2 storm
    uint32_t profile = 0;  ///< into Table::profiles
};

struct Table {
    std::vector<Key> keys;
    std::vector<Profile> profiles;
    std::vector<Link> links;
};

/// Reads the file. False, with `out` empty, on any inconsistency: a wrong
/// magic or size, a profile past the keys or over kMaxKeys, keys out of
/// order or past midnight, a link past the profiles.
inline bool parse(const std::vector<uint8_t>& data, Table& out) {
    out = Table{};
    size_t pos = 0;
    auto word = [&](uint32_t& w) {
        if (pos + 4 > data.size()) return false;
        std::memcpy(&w, data.data() + pos, 4);
        pos += 4;
        return true;
    };
    if (data.size() < 16 || std::memcmp(data.data(), "WFL1", 4) != 0) return false;
    pos = 4;
    uint32_t nKeys = 0, nProfiles = 0, nLinks = 0;
    if (!word(nKeys) || !word(nProfiles) || !word(nLinks)) return false;
    const uint64_t expected = 16 + uint64_t(nKeys) * (1 + kColorChannels + kFloatChannels) * 4 +
                              uint64_t(nProfiles) * 5 * 4 + uint64_t(nLinks) * 3 * 4;
    if (expected != data.size()) return false;
    Table t;
    t.keys.resize(nKeys);
    for (Key& k : t.keys) {
        word(k.time);
        for (uint32_t& c : k.color) word(c);
        for (float& v : k.value) {
            uint32_t w = 0;
            word(w);
            std::memcpy(&v, &w, 4);
        }
        if (k.time >= 2880) return false;
    }
    t.profiles.resize(nProfiles);
    for (Profile& p : t.profiles) {
        word(p.enhancedParamsId);
        word(p.firstKey);
        word(p.keyCount);
        word(p.colorMask);
        word(p.floatMask);
        if (p.keyCount == 0 || p.keyCount > kMaxKeys ||
            uint64_t(p.firstKey) + p.keyCount > nKeys) {
            return false;
        }
        for (uint32_t k = 1; k < p.keyCount; ++k) {
            if (t.keys[p.firstKey + k - 1].time >= t.keys[p.firstKey + k].time) return false;
        }
    }
    t.links.resize(nLinks);
    for (Link& l : t.links) {
        word(l.lightId);
        word(l.slot);
        word(l.profile);
        if (l.profile >= nProfiles || (l.slot != 0 && l.slot != 2)) return false;
    }
    out = std::move(t);
    return true;
}

}  // namespace enhanced_lights
}  // namespace rendering
}  // namespace wowee
