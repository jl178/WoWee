#pragma once

/// The stock client's terrain arithmetic, read out of Wow.exe 3.3.5a (12340)
/// and the stock terrain programs (shaders\vertex\arbvp1\terrain.bls,
/// shaders\pixel\arbfp1\terrain0-3.bls).
///
/// Pure functions only, so the unit tests can hold them to the binary without
/// a GPU. docs/re/terrain.md has the walk-through; the addresses below are
/// where each rule lives.
///
/// - Layer blending: the pixel program is picked by 0x79e5c0 from the map's
///   big-alpha flag (MPHD 0x4, 0xcf08d0 bit 2). Without it the layers are a
///   chain of lerps, each over the result so far (terrain1.bls variants
///   0-7); with it they are independent weights and the base layer takes
///   what is left, 1 - sat(a1 + a2 + a3) (variants 8-15).
/// - A layer without an alpha map (MCLY flag 0x100 clear) reads the zero row
///   0xd1d018 (0x7b74a0): it covers nothing.
/// - MCLY 0x80: an unlit layer. 0x7d2d70 sets pixel c1 to 0 for it, and the
///   program (variants 16-31) then takes its texture as it is instead of
///   primary * texture * 2.
/// - MCLY 0x40: a scrolling layer. Eight directions (0x7 bits) advance by the
///   frame time in 0x7831a0 and wrap to 0 on reaching 64; the speed (0x38
///   bits) divides by table 0xaf14f8. 0x7d2d70 turns them into the layer's
///   texture offset (vertex c13 + layer).
/// - Specular (the `specular` CVar, "Terrain highlights"): the `_s` tileset
///   (0x7d6980) carries the mask in its alpha; the vertex program adds
///   pow(N.H, 20) * sun colour (c27, 0x7cfbe0), and the pixel program adds it
///   times the blended alpha times the shadow.
/// - The horizon (WDL): 0x795f80 draws the map's low-detail heights in the
///   flat fog colour (fog start 0, end 1), between the far clip less 50 yards
///   and horizonFarclipScale (4) far clips (0x791170), at depth 0.998-0.999,
///   behind everything but the sky.

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace wowee::rendering::stock_terrain {

// ---------------------------------------------------------------- layers ----

/// MCLY flags the renderer reads.
inline constexpr uint32_t kLayerAnimDirMask = 0x7;
inline constexpr uint32_t kLayerAnimSpeedShift = 3;
inline constexpr uint32_t kLayerAnimSpeedMask = 0x7;
inline constexpr uint32_t kLayerAnimate = 0x40;
inline constexpr uint32_t kLayerUnlit = 0x80;
inline constexpr uint32_t kLayerUseAlpha = 0x100;

/// MPHD 0x4: the map's alpha maps are eight-bit, and blended as weights.
inline constexpr uint32_t kMphdBigAlpha = 0x4;

/// The four weights the pixel program puts on the base layer and layers 1-3,
/// given each layer's alpha (0 for a layer that is not there). In the lerp
/// chain each layer takes its alpha of what is under it; as weights the
/// layers keep theirs and the base has the remainder.
inline std::array<float, 4> layerWeights(float a1, float a2, float a3, bool bigAlpha) {
    if (bigAlpha) {
        const float base = 1.0f - std::clamp(a1 + a2 + a3, 0.0f, 1.0f);
        return {base, a1, a2, a3};
    }
    // lerp(lerp(lerp(t0, t1, a1), t2, a2), t3, a3), expanded.
    const float k3 = a3;
    const float k2 = a2 * (1.0f - a3);
    const float k1 = a1 * (1.0f - a2) * (1.0f - a3);
    const float k0 = (1.0f - a1) * (1.0f - a2) * (1.0f - a3);
    return {k0, k1, k2, k3};
}

/// 0x7831a0 steps eight accumulators by these, in the order of MCLY's low
/// three bits (table 0xadee78).
inline constexpr std::array<glm::vec2, 8> kAnimDirections = {{
    {-1.0f, 0.0f}, {-1.0f, 1.0f}, {0.0f, 1.0f}, {1.0f, 1.0f},
    {1.0f, 0.0f},  {1.0f, -1.0f}, {0.0f, -1.0f}, {-1.0f, -1.0f},
}};

/// Table 0xaf14f8, by MCLY bits 3-5: what the accumulator is divided by.
inline constexpr std::array<float, 8> kAnimSpeedDivisors = {64.0f, 48.0f, 32.0f, 16.0f,
                                                            8.0f,  4.0f,  2.0f,  1.0f};

/// One quad (4.1667 yards, 0xa3fdd4) is one repeat of a layer texture: the
/// vertex program scales by 1/4.1667 (0x7c3d90, 0xd25488).
inline constexpr float kLayerRepeatsPerYard = 0.24f;

/// 0x7831a0's accumulator after `seconds`: each axis grows by its direction
/// per second and goes back to 0 once it reaches 64. Only the upper end
/// wraps, so an axis running backwards just keeps going.
inline float animAccumulator(float direction, float seconds) {
    if (direction > 0.0f) return std::fmod(direction * seconds, 64.0f);
    return direction * seconds;
}

/// The layer's texture offset, in repeats, as (u, v) of the stock program's
/// texture coordinate: 0x7d2d70 writes c13 = -(acc * (1/0.24) / speed * 0.125)
/// and the program adds c13.yx, so u takes the second axis.
inline glm::vec2 animOffset(uint32_t flags, float seconds) {
    if ((flags & kLayerAnimate) == 0) return glm::vec2(0.0f);
    const glm::vec2 dir = kAnimDirections[flags & kLayerAnimDirMask];
    const float speed = kAnimSpeedDivisors[(flags >> kLayerAnimSpeedShift) & kLayerAnimSpeedMask];
    const float k = (1.0f / kLayerRepeatsPerYard) / speed * 0.125f;
    const glm::vec2 acc(animAccumulator(dir.x, seconds), animAccumulator(dir.y, seconds));
    return glm::vec2(-acc.y * k, -acc.x * k);
}

// -------------------------------------------------------------- specular ----

/// The specular power the terrain programs get in c27.w (0x7cfbe0, 0xa3fff0).
inline constexpr float kSpecularPower = 20.0f;

/// The vertex program's highlight: pow(max(N.H, 0), 20) * colour, H halfway
/// between the eye and the light. `toLight` and `toEye` are unit vectors.
inline glm::vec3 specular(const glm::vec3& normal, const glm::vec3& toLight,
                          const glm::vec3& toEye, const glm::vec3& colour) {
    const glm::vec3 h = glm::normalize(toEye + toLight);
    return std::pow(std::max(glm::dot(normal, h), 0.0f), kSpecularPower) * colour;
}

/// The tileset the client loads for a texture when specular is on: the same
/// name with `_s` before the extension (0x7d6980). Empty if there is no
/// extension to put it before.
inline std::string specularTexturePath(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("\\/");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return path.substr(0, dot) + "_s" + path.substr(dot);
}

// --------------------------------------------------------------- horizon ----

/// horizonFarclipScale's default (CVar 0x78e62e, "4.0").
inline constexpr float kHorizonFarclipScale = 4.0f;
/// 0x791170: the horizon's near plane is the far clip less this.
inline constexpr float kHorizonNearInset = 50.0f;

/// The view depths the horizon draws between, for a far clip.
inline glm::vec2 horizonDepthRange(float farClip) {
    return {std::max(farClip - kHorizonNearInset, 0.0f), farClip * kHorizonFarclipScale};
}

/// One WDL tile: MARE's 17x17 outer heights then 16x16 inner ones, in yards,
/// and MAHO's sixteen rows of hole bits (bit c of row r: cell r, c).
struct WdlTile {
    int tileX = 0;
    int tileY = 0;
    std::array<int16_t, 17 * 17 + 16 * 16> heights{};
    std::array<uint16_t, 16> holes{};
};

namespace detail {
inline uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
inline constexpr uint32_t fourCC(const char (&s)[5]) {
    // Stored reversed in the file: "MAOF" reads as 'F','O','A','M'.
    return (static_cast<uint32_t>(s[0]) << 24) | (static_cast<uint32_t>(s[1]) << 16) |
           (static_cast<uint32_t>(s[2]) << 8) | static_cast<uint32_t>(s[3]);
}
}  // namespace detail

/// The tiles of a .wdl (0x7cc310): MAOF holds 64x64 offsets, row-major by the
/// ADT name's second number, each to a MARE chunk optionally followed by
/// MAHO. Tiles whose offset is 0 or out of the file are skipped.
inline std::vector<WdlTile> parseWdl(const std::vector<uint8_t>& data) {
    std::vector<WdlTile> tiles;
    const size_t n = data.size();
    size_t pos = 0;
    const uint8_t* maof = nullptr;
    while (pos + 8 <= n) {
        const uint32_t tag = detail::rd32(&data[pos]);
        const uint32_t size = detail::rd32(&data[pos + 4]);
        if (tag == detail::fourCC("MAOF")) {
            if (pos + 8 + 64 * 64 * 4 <= n) maof = &data[pos + 8];
            break;
        }
        if (size > n - pos - 8) break;
        pos += 8 + size;
    }
    if (!maof) return tiles;
    constexpr size_t kMareBytes = (17 * 17 + 16 * 16) * 2;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const uint32_t off = detail::rd32(maof + (static_cast<size_t>(y) * 64 + x) * 4);
            if (off == 0 || off + 8 + kMareBytes > n) continue;
            if (detail::rd32(&data[off]) != detail::fourCC("MARE")) continue;
            WdlTile t;
            t.tileX = x;
            t.tileY = y;
            std::memcpy(t.heights.data(), &data[off + 8], kMareBytes);
            const size_t maho = off + 8 + kMareBytes;
            if (maho + 8 + 32 <= n && detail::rd32(&data[maho]) == detail::fourCC("MAHO")) {
                std::memcpy(t.holes.data(), &data[maho + 8], 32);
            }
            tiles.push_back(t);
        }
    }
    return tiles;
}

/// A tile's vertices in Wowee's render space, the ADT mesh's placement
/// (terrain_mesh.cpp): outer (r, c) then inner (r + 0.5, c + 0.5), one cell
/// being a chunk (33.33 yards).
inline void wdlTileVertices(const WdlTile& t, float tileSize, std::vector<glm::vec3>& out) {
    const auto place = [&](float row, float col, int16_t h) {
        out.emplace_back((32.0f - static_cast<float>(t.tileY) - row / 16.0f) * tileSize,
                         (32.0f - static_cast<float>(t.tileX) - col / 16.0f) * tileSize,
                         static_cast<float>(h));
    };
    for (int r = 0; r < 17; ++r)
        for (int c = 0; c < 17; ++c) place(static_cast<float>(r), static_cast<float>(c), t.heights[r * 17 + c]);
    for (int r = 0; r < 16; ++r)
        for (int c = 0; c < 16; ++c)
            place(static_cast<float>(r) + 0.5f, static_cast<float>(c) + 0.5f, t.heights[289 + r * 16 + c]);
}

/// Four triangles round each cell's centre, cells under a MAHO bit left out,
/// as indices into wdlTileVertices' order.
inline void wdlTileIndices(const WdlTile& t, std::vector<uint16_t>& out) {
    for (int r = 0; r < 16; ++r) {
        for (int c = 0; c < 16; ++c) {
            if (t.holes[r] & (1u << c)) continue;
            const auto tl = static_cast<uint16_t>(r * 17 + c);
            const auto tr = static_cast<uint16_t>(tl + 1);
            const auto bl = static_cast<uint16_t>(tl + 17);
            const auto br = static_cast<uint16_t>(bl + 1);
            const auto ce = static_cast<uint16_t>(289 + r * 16 + c);
            const uint16_t tris[12] = {ce, tl, tr, ce, tr, br, ce, br, bl, ce, bl, tl};
            out.insert(out.end(), tris, tris + 12);
        }
    }
}

/// Wowee's distant level of the same tile: the outer grid at every other
/// point, two triangles a cell of two chunks (128 for a tile instead of
/// 1024), a cell left out if any of its four is a hole. For tiles far enough
/// that a 66-yard cell is a few pixels; the stock client draws the full mesh.
inline void wdlTileIndicesCoarse(const WdlTile& t, std::vector<uint16_t>& out) {
    for (int r = 0; r < 16; r += 2) {
        for (int c = 0; c < 16; c += 2) {
            const uint32_t rows = static_cast<uint32_t>(t.holes[r]) | t.holes[r + 1];
            if (rows & (3u << c)) continue;
            const auto tl = static_cast<uint16_t>(r * 17 + c);
            const auto tr = static_cast<uint16_t>(tl + 2);
            const auto bl = static_cast<uint16_t>(tl + 34);
            const auto br = static_cast<uint16_t>(bl + 2);
            const uint16_t tris[6] = {tl, tr, br, tl, br, bl};
            out.insert(out.end(), tris, tris + 6);
        }
    }
}

}  // namespace wowee::rendering::stock_terrain
