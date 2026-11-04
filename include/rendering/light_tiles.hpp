#pragma once

/// The local lights culled to a screen grid ("Torch and lamp light").
///
/// Every world pixel used to walk all the gathered lights - forty-eight in
/// Goldshire, sixty-four in a city - and nearly all of them are yards away from
/// it: a torch reaches three to a dozen yards. Each tile of a 32x18 grid gets a
/// 64-bit mask of the lights whose sphere can hold a pixel in it, and the
/// shaders walk that (assets/shaders/local_light_tiles.glsli).
///
/// The projection is conservative. A light that cannot be in front of the
/// camera marks nothing; one wholly in front marks the screen rectangle of its
/// sphere - the exact one, from the tangent planes in view space, when the
/// projection is a plain perspective (cullLightsToTiles(view, proj, ...)),
/// else the rectangle of its bounding box's eight projected corners - grown
/// by a margin (pixel centres, sample positions, a jittered projection);
/// anything straddling the camera plane marks every tile. So a tile leaves out
/// only lights no pixel in it can be within - lights the shaders' own radius
/// test skips - and the sum each pixel adds up is the same one, term for term.
///
/// The margin was a whole tile (about 39 x 55 pixels at 1251 x 986), which
/// the box's corners then grew further: a torch's few tiles became 3 x 3 or
/// 4 x 4, and every pixel there walked it. Two pixels cover what the margin
/// is for (enhancements-plan.md 3.12).

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace wowee {
namespace rendering {

inline constexpr uint32_t LIGHT_TILES_X = 32;
inline constexpr uint32_t LIGHT_TILES_Y = 18;
/// Two tiles per uvec4: (lo, hi) of the first, then of the second.
inline constexpr uint32_t LIGHT_TILE_WORDS = LIGHT_TILES_X * LIGHT_TILES_Y / 2;

/// Fills `tiles` (LIGHT_TILE_WORDS entries) for `count` lights (at most 64),
/// each xyz a world position and w its radius, seen through `viewProj` (a
/// Vulkan projection: positive viewport height, y down the screen).
namespace light_tiles_detail {

inline int tileOf(float ndc, int tiles) {
    const float t = std::floor((ndc * 0.5f + 0.5f) * static_cast<float>(tiles));
    return static_cast<int>(std::clamp(t, -2.0f, static_cast<float>(tiles + 1)));
}

inline void markRect(glm::uvec4* tiles, uint32_t i, int x0, int x1, int y0, int y1) {
    constexpr int TX = static_cast<int>(LIGHT_TILES_X);
    const uint32_t bit = 1u << (i & 31u);
    const int lane = i < 32 ? 0 : 1;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const int idx = y * TX + x;
            tiles[idx >> 1][(idx & 1) * 2 + lane] |= bit;
        }
    }
}

/// Marks the tiles the NDC rectangle [lo, hi], grown by `margin`, touches;
/// nothing when it is off screen.
inline void markNdcRect(glm::uvec4* tiles, uint32_t i, glm::vec2 lo, glm::vec2 hi, glm::vec2 margin) {
    constexpr int TX = static_cast<int>(LIGHT_TILES_X);
    constexpr int TY = static_cast<int>(LIGHT_TILES_Y);
    lo -= margin;
    hi += margin;
    if (lo.x > 1.0f || lo.y > 1.0f || hi.x < -1.0f || hi.y < -1.0f) return;  // off screen
    markRect(tiles, i, std::max(0, tileOf(lo.x, TX)), std::min(TX - 1, tileOf(hi.x, TX)),
             std::max(0, tileOf(lo.y, TY)), std::min(TY - 1, tileOf(hi.y, TY)));
}

/// The NDC extent along one screen axis of a sphere wholly in front of the
/// camera: c = (centre along the axis, centre z) in view space (z < 0 in
/// front), ndc = -scale * u / z - offset for a view-space direction (u, z).
/// The two tangent directions from the eye bound it exactly.
inline glm::vec2 sphereAxisNdc(glm::vec2 c, float r, float scale, float offset) {
    const float t = std::sqrt(std::max(glm::dot(c, c) - r * r, 0.0f));
    const glm::vec2 d1(t * c.x + r * c.y, t * c.y - r * c.x);
    const glm::vec2 d2(t * c.x - r * c.y, t * c.y + r * c.x);
    const float n1 = -scale * d1.x / d1.y - offset;
    const float n2 = -scale * d2.x / d2.y - offset;
    return {std::min(n1, n2), std::max(n1, n2)};
}

}  // namespace light_tiles_detail

/// One tile of margin in NDC, the grid's old growth.
inline glm::vec2 lightTileMarginOneTile() {
    return {2.0f / static_cast<float>(LIGHT_TILES_X), 2.0f / static_cast<float>(LIGHT_TILES_Y)};
}

/// `pixels` of margin in NDC for a viewport `width` x `height`.
inline glm::vec2 lightTileMarginPixels(float pixels, float width, float height) {
    return {2.0f * pixels / std::max(width, 1.0f), 2.0f * pixels / std::max(height, 1.0f)};
}

inline void cullLightsToTiles(const glm::mat4& viewProj, const glm::vec4* posRadius,
                              uint32_t count, glm::uvec4* tiles,
                              glm::vec2 margin = lightTileMarginOneTile()) {
    using namespace light_tiles_detail;
    std::memset(tiles, 0, sizeof(glm::uvec4) * LIGHT_TILE_WORDS);
    constexpr int TX = static_cast<int>(LIGHT_TILES_X);
    constexpr int TY = static_cast<int>(LIGHT_TILES_Y);
    // clip.w as a function of world position: w = dot(row.xyz, p) + row.w.
    // Its largest change over a sphere of radius r is r * |row.xyz|.
    const glm::vec4 wRow(viewProj[0][3], viewProj[1][3], viewProj[2][3], viewProj[3][3]);
    const float wGrad = glm::length(glm::vec3(wRow));
    constexpr float kMinW = 1e-3f;
    const uint32_t n = std::min<uint32_t>(count, 64);
    for (uint32_t i = 0; i < n; ++i) {
        const glm::vec4 pr = posRadius[i];
        const float r = pr.w;
        if (!(r > 0.0f)) continue;  // the shaders skip it whatever the mask says
        const glm::vec3 c(pr);
        const float wc = glm::dot(glm::vec3(wRow), c) + wRow.w;
        if (wc + r * wGrad <= kMinW) continue;  // wholly behind the camera
        // The box round the sphere reaches r * sqrt(3) from its centre.
        if (wc - r * 1.7320509f * wGrad > kMinW) {
            glm::vec2 lo(1e30f), hi(-1e30f);
            for (int k = 0; k < 8; ++k) {
                const glm::vec3 corner = c + r * glm::vec3((k & 1) ? 1.0f : -1.0f,
                                                           (k & 2) ? 1.0f : -1.0f,
                                                           (k & 4) ? 1.0f : -1.0f);
                const glm::vec4 clip = viewProj * glm::vec4(corner, 1.0f);
                const glm::vec2 ndc = glm::vec2(clip) / clip.w;
                lo = glm::min(lo, ndc);
                hi = glm::max(hi, ndc);
            }
            markNdcRect(tiles, i, lo, hi, margin);
        } else {
            markRect(tiles, i, 0, TX - 1, 0, TY - 1);
        }
    }
}

/// The same, from the view and a projection kept apart, which bounds each
/// sphere by its tangent planes instead of its box. A projection that is not
/// a plain perspective (w = -z, no shear) falls back to the box.
inline void cullLightsToTiles(const glm::mat4& view, const glm::mat4& proj, const glm::vec4* posRadius,
                              uint32_t count, glm::uvec4* tiles, glm::vec2 margin) {
    using namespace light_tiles_detail;
    const bool plain = proj[2][3] == -1.0f && proj[0][3] == 0.0f && proj[1][3] == 0.0f &&
                       proj[3][3] == 0.0f && proj[1][0] == 0.0f && proj[0][1] == 0.0f &&
                       proj[3][0] == 0.0f && proj[3][1] == 0.0f;
    if (!plain) {
        cullLightsToTiles(proj * view, posRadius, count, tiles, margin);
        return;
    }
    std::memset(tiles, 0, sizeof(glm::uvec4) * LIGHT_TILE_WORDS);
    constexpr int TX = static_cast<int>(LIGHT_TILES_X);
    constexpr int TY = static_cast<int>(LIGHT_TILES_Y);
    constexpr float kMinW = 1e-3f;
    // A camera's view is rigid; a scale in it would scale the spheres too.
    const float viewScale = std::max({glm::length(glm::vec3(view[0])), glm::length(glm::vec3(view[1])),
                                      glm::length(glm::vec3(view[2]))});
    const uint32_t n = std::min<uint32_t>(count, 64);
    for (uint32_t i = 0; i < n; ++i) {
        const glm::vec4 pr = posRadius[i];
        if (!(pr.w > 0.0f)) continue;
        const float r = pr.w * viewScale;
        // View space: w = -z, in front where z < 0.
        const glm::vec3 v = glm::vec3(view * glm::vec4(glm::vec3(pr), 1.0f));
        if (-v.z + r <= kMinW) continue;  // wholly behind the camera
        if (-v.z - r <= kMinW) {          // straddles the camera plane
            markRect(tiles, i, 0, TX - 1, 0, TY - 1);
            continue;
        }
        const glm::vec2 x = sphereAxisNdc({v.x, v.z}, r, proj[0][0], proj[2][0]);
        const glm::vec2 y = sphereAxisNdc({v.y, v.z}, r, proj[1][1], proj[2][1]);
        markNdcRect(tiles, i, {x.x, y.x}, {x.y, y.y}, margin);
    }
}

}  // namespace rendering
}  // namespace wowee
