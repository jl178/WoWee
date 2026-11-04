#pragma once

/// "Lit far horizon" (docs/enhancements-plan.md E11), and the WDL's
/// low-detail buildings the stock horizon draws.
///
/// The stock horizon (TerrainHorizon, terrain.md section 8) draws the WDL's
/// heights past the far clip with fog start 0 and end 1: every pixel is the
/// fog colour, one flat silhouette. With the row on, the same mountains are
/// shaded by the sun under that colour: a face toward the sun a little lighter
/// than the fog, one away from it a little darker, level ground exactly the
/// fog colour. How much shows is the haze's call - it comes in over the first
/// stretch past the far clip (so the horizon meets the real ground, which the
/// fog has already made flat, without a seam), thins toward the far limit,
/// and goes in a zone whose fog closes in short of the far clip. The palette
/// is still the fog's: nothing is coloured that the zone's light did not ask
/// for.
///
/// The WDL also places a few low-detail WMOs (MWMO/MODF; in 3.3.5 only
/// Northrend's: New Hearthglen's cathedral, Valgarde, the oil platform) that
/// the stock pass draws with the mountains. They are drawn in both modes, flat
/// fog-coloured when off as stock draws them, shaded with the row on.
///
/// Pure, for TerrainHorizon, TerrainManager, the shader (the same formula in
/// terrain_horizon.glsli) and test_phase2_motion.

#include "rendering/stock_terrain.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace wowee {
namespace rendering {
namespace horizon_detail {

// ---- The haze ---------------------------------------------------------------
/// The most the light moves a horizon pixel off the fog colour, as a factor:
/// a sunlit face at most this much brighter or darker.
inline constexpr float kMaxRelief = 0.35f;
/// The shading comes in over this fraction of the span past the near limit.
inline constexpr float kRampIn = 0.06f;
/// The fog's end as a fraction of the far clip below which the zone counts as
/// hazy and the shading is gone, and above which it shows in full.
inline constexpr float kHazyFrom = 0.35f;
inline constexpr float kClearFrom = 0.85f;

/// How much of the shading shows at view depth `z`, between the horizon's
/// near and far limits, for a fog ending at `fogEnd` with this far clip.
inline float reliefWeight(float z, float nearLimit, float farLimit, float fogEnd, float farClip) {
    if (farLimit <= nearLimit || farClip <= 0.0f) return 0.0f;
    const float t = std::clamp((z - nearLimit) / (farLimit - nearLimit), 0.0f, 1.0f);
    const float in = std::clamp(t / kRampIn, 0.0f, 1.0f);
    const float out = std::pow(1.0f - t, 1.5f);
    const float h = std::clamp((fogEnd / farClip - kHazyFrom) / (kClearFrom - kHazyFrom), 0.0f, 1.0f);
    const float haze = h * h * (3.0f - 2.0f * h);
    return kMaxRelief * in * in * (3.0f - 2.0f * in) * out * haze;
}

/// The shading as a factor on the fog colour, per channel: the light this
/// face takes (ambient + sun x N.L, as terrain.frag lights) over what level
/// ground takes, moved `weight` of the way from 1. Level ground is exactly 1.
/// `toSun` points at the sun.
inline glm::vec3 relief(const glm::vec3& normal, const glm::vec3& toSun, const glm::vec3& ambient,
                        const glm::vec3& sun, float weight) {
    const float ndl = std::max(glm::dot(normal, toSun), 0.0f);
    const float flat = std::max(toSun.z, 0.0f);
    const glm::vec3 lit = glm::min(ambient + sun * ndl, glm::vec3(1.0f));
    const glm::vec3 level = glm::max(glm::min(ambient + sun * flat, glm::vec3(1.0f)), glm::vec3(0.02f));
    const glm::vec3 r = glm::clamp(lit / level, glm::vec3(0.5f), glm::vec3(1.5f));
    return glm::mix(glm::vec3(1.0f), r, weight);
}

// ---- WDL normals ------------------------------------------------------------
/// A normal as four signed bytes (VK_FORMAT_R8G8B8A8_SNORM).
inline uint32_t packNormal(const glm::vec3& n) {
    auto b = [](float v) {
        return static_cast<uint32_t>(static_cast<uint8_t>(static_cast<int8_t>(
            std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f))));
    };
    return b(n.x) | (b(n.y) << 8) | (b(n.z) << 16);
}

inline glm::vec3 unpackNormal(uint32_t p) {
    auto f = [&](int s) { return static_cast<float>(static_cast<int8_t>((p >> s) & 0xffu)) / 127.0f; };
    return {f(0), f(8), f(16)};
}

/// One normal per vertex of stock_terrain::wdlTileVertices, packed: the outer
/// grid by differences across its neighbours (one-sided at the tile's edge),
/// each inner vertex from the four outer points round it.
inline void wdlTileNormals(const stock_terrain::WdlTile& t, float tileSize, std::vector<uint32_t>& out) {
    const float cell = tileSize / 16.0f;
    auto outer = [&](int r, int c) {
        return static_cast<float>(t.heights[std::clamp(r, 0, 16) * 17 + std::clamp(c, 0, 16)]);
    };
    // Rows run toward -x, columns toward -y (wdlTileVertices).
    auto normalAt = [&](float dhdRow, float dhdCol) {
        const float dhdx = -dhdRow / cell, dhdy = -dhdCol / cell;
        return glm::normalize(glm::vec3(-dhdx, -dhdy, 1.0f));
    };
    for (int r = 0; r < 17; ++r) {
        for (int c = 0; c < 17; ++c) {
            const int r0 = std::max(r - 1, 0), r1 = std::min(r + 1, 16);
            const int c0 = std::max(c - 1, 0), c1 = std::min(c + 1, 16);
            const float dr = (outer(r1, c) - outer(r0, c)) / static_cast<float>(r1 - r0);
            const float dc = (outer(r, c1) - outer(r, c0)) / static_cast<float>(c1 - c0);
            out.push_back(packNormal(normalAt(dr, dc)));
        }
    }
    for (int r = 0; r < 16; ++r) {
        for (int c = 0; c < 16; ++c) {
            const float dr = 0.5f * ((outer(r + 1, c) + outer(r + 1, c + 1)) - (outer(r, c) + outer(r, c + 1)));
            const float dc = 0.5f * ((outer(r, c + 1) + outer(r + 1, c + 1)) - (outer(r, c) + outer(r + 1, c)));
            out.push_back(packNormal(normalAt(dr, dc)));
        }
    }
}

// ---- WDL low-detail WMOs ------------------------------------------------------
/// One MODF placement of the WDL, with its MWMO name. The fields are as in an
/// ADT's MODF (position and rotation in placement space, degrees).
struct WdlWmo {
    std::string path;
    float position[3] = {0, 0, 0};
    float rotation[3] = {0, 0, 0};
    uint16_t flags = 0;
    uint16_t doodadSet = 0;
    float scale = 1.0f;
};

/// The WDL's MWMO names and MODF placements (0x7cc310 reads the same chunks
/// ahead of MAOF). Placements naming no string are skipped.
inline std::vector<WdlWmo> parseWdlWmos(const std::vector<uint8_t>& data) {
    std::vector<WdlWmo> out;
    const size_t n = data.size();
    size_t pos = 0;
    const uint8_t* names = nullptr;
    size_t namesSize = 0;
    const uint8_t* modf = nullptr;
    size_t modfSize = 0;
    std::vector<uint32_t> ids;  // MWID: offsets into MWMO
    auto rd32 = [&](size_t at) {
        uint32_t v;
        std::memcpy(&v, &data[at], 4);
        return v;
    };
    auto tag = [](const char (&s)[5]) {
        return (static_cast<uint32_t>(s[0]) << 24) | (static_cast<uint32_t>(s[1]) << 16) |
               (static_cast<uint32_t>(s[2]) << 8) | static_cast<uint32_t>(s[3]);
    };
    while (pos + 8 <= n) {
        const uint32_t t = rd32(pos);
        const uint32_t size = rd32(pos + 4);
        if (size > n - pos - 8) break;
        if (t == tag("MWMO")) { names = &data[pos + 8]; namesSize = size; }
        if (t == tag("MWID")) {
            for (size_t i = 0; i + 4 <= size; i += 4) ids.push_back(rd32(pos + 8 + i));
        }
        if (t == tag("MODF")) { modf = &data[pos + 8]; modfSize = size; }
        if (t == tag("MAOF")) break;
        pos += 8 + size;
    }
    if (!names || !modf) return out;
    for (size_t off = 0; off + 64 <= modfSize; off += 64) {
        const uint8_t* e = modf + off;
        uint32_t nameId;
        std::memcpy(&nameId, e, 4);
        // MODF's name index goes through MWID to a string offset; without an
        // MWID, the index is taken as the n-th string.
        size_t strOff = 0;
        if (!ids.empty()) {
            if (nameId >= ids.size()) continue;
            strOff = ids[nameId];
        } else {
            size_t k = 0, at = 0;
            while (k < nameId && at < namesSize) {
                if (names[at] == 0) ++k;
                ++at;
            }
            strOff = at;
        }
        if (strOff >= namesSize) continue;
        const char* s = reinterpret_cast<const char*>(names + strOff);
        const size_t len = strnlen(s, namesSize - strOff);
        if (len == 0) continue;
        WdlWmo w;
        w.path.assign(s, len);
        std::memcpy(w.position, e + 8, 12);
        std::memcpy(w.rotation, e + 20, 12);
        uint16_t u16[4];
        std::memcpy(u16, e + 56, 8);
        w.flags = u16[0];
        w.doodadSet = u16[1];
        // Scale is 1024 = 1 in later builds; 3.3.5 leaves it 0.
        w.scale = u16[3] ? static_cast<float>(u16[3]) / 1024.0f : 1.0f;
        out.push_back(w);
    }
    return out;
}

/// A mesh for the horizon: world positions, packed normals and triangles,
/// with a bounding sphere per object for the cull.
struct FarMesh {
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> normals;
    std::vector<uint32_t> indices;
    struct Object {
        glm::vec3 center{0.0f};
        float radius = 0.0f;
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
    };
    std::vector<Object> objects;
};

}  // namespace horizon_detail
}  // namespace rendering
}  // namespace wowee
