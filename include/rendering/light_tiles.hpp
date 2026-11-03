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
/// camera marks nothing; one whose bounding box is wholly in front marks the
/// screen rectangle of the box's eight projected corners, grown by a tile all
/// round (pixel centres, sample positions, a jittered projection); anything
/// straddling the camera plane marks every tile. So a tile leaves out only
/// lights no pixel in it can be within - lights the shaders' own radius test
/// skips - and the sum each pixel adds up is the same one, term for term.

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
inline void cullLightsToTiles(const glm::mat4& viewProj, const glm::vec4* posRadius,
                              uint32_t count, glm::uvec4* tiles) {
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
        int x0 = 0, x1 = TX - 1, y0 = 0, y1 = TY - 1;
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
            if (lo.x > 1.0f || lo.y > 1.0f || hi.x < -1.0f || hi.y < -1.0f) continue;  // off screen
            const auto tile = [](float ndc, int tiles) {
                const float t = std::floor((ndc * 0.5f + 0.5f) * static_cast<float>(tiles));
                return static_cast<int>(std::clamp(t, -2.0f, static_cast<float>(tiles + 1)));
            };
            x0 = std::max(0, tile(lo.x, TX) - 1);
            x1 = std::min(TX - 1, tile(hi.x, TX) + 1);
            y0 = std::max(0, tile(lo.y, TY) - 1);
            y1 = std::min(TY - 1, tile(hi.y, TY) + 1);
        }
        const uint32_t bit = 1u << (i & 31u);
        const int lane = i < 32 ? 0 : 1;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const int idx = y * TX + x;
                tiles[idx >> 1][(idx & 1) * 2 + lane] |= bit;
            }
        }
    }
}

}  // namespace rendering
}  // namespace wowee
