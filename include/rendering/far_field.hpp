#pragma once

/// "Far field (the reference client)": the land past the view distance drawn in its
/// own colours, lit and fogged as the near ground is, out to Enhanced's
/// horizonClip.
///
/// the reference client draws full detail to horizonStart (1900 yards) and a
/// low-detail horizon to horizonClip (7000), with a fog that ends at 18000
/// (docs/re/enhanced-lighting.md). The stock 3.3.5 client draws the WDL
/// heights past its far clip in the flat fog colour (terrain.md section 8),
/// which is right for a fog that ends at the far clip and wrong for one that
/// ends far past it: the silhouette reads as a grey cut-out against land the
/// fog barely touches. With this row on, the same WDL mesh (TerrainHorizon)
/// takes its colour from the map's minimap tiles - one top-down picture per
/// ADT tile, the only colour the client ships for land it has not loaded -
/// packed into one BC1 atlas over the map's WDL tiles, 64 texels a tile
/// (8.3 yards a texel, about the pixel size at 1900-4000 yards), and is lit
/// by terrain.frag's terms (ambient + sun x N.L clamped at one, the shadow
/// floor's average) and fogged by the same stock fog and air, so where the
/// real ground stops the far field carries on in the same light.
///
/// Pure: the atlas layout, the box filter and the BC1 encoder are used by
/// TerrainManager (on an IO worker) and test_far_field.
///
/// The near ground's chunks are kept by their centre's distance in the
/// plane (TerrainRenderer::cull), so every point nearer than the far clip
/// less half a chunk's diagonal is drawn by them. The far field draws from
/// there on, by the same distance in the plane - not the stock horizon's
/// view depth, which at the sides of a wide view starts hundreds of yards
/// past where the ground has stopped, a gap the stock fog hides and the long
/// view does not.

#include "rendering/stock_terrain.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wowee::rendering::far_field {

/// How far the far field reaches, in yards: Enhanced's horizonClip.
inline constexpr float kReach = 7000.0f;
/// The near limit is the far clip less this: half a chunk's diagonal
/// (33.33 x sqrt 2 / 2 = 23.57) and a little.
inline constexpr float kNearInset = 24.0f;
/// The far field is pushed this fraction back along each view ray, so where
/// it overlaps the near ground (the last half chunk) the real ground wins
/// the depth test. Screen positions do not move.
inline constexpr float kDepthPushBack = 0.004f;
/// Over the last part of the reach the far field fades to the fog colour,
/// so it ends against the sky's fog band rather than at an edge.
inline constexpr float kEdgeFadeFrom = 0.82f;
/// Texels per ADT tile in the atlas, and the mip levels (64 down to 4).
inline constexpr int kTexelsPerTile = 64;
inline constexpr int kMipLevels = 5;

/// Where the map's tiles sit in the atlas: tile (x, y) at texel
/// ((x - minX) * 64, (y - minY) * 64).
struct AtlasLayout {
    int minX = 0;
    int minY = 0;
    int tilesW = 0;
    int tilesH = 0;
    [[nodiscard]] bool empty() const { return tilesW <= 0 || tilesH <= 0; }
    [[nodiscard]] int width() const { return tilesW * kTexelsPerTile; }
    [[nodiscard]] int height() const { return tilesH * kTexelsPerTile; }
};

inline AtlasLayout layoutFor(const std::vector<stock_terrain::WdlTile>& tiles) {
    AtlasLayout l;
    if (tiles.empty()) return l;
    int x0 = 64, y0 = 64, x1 = -1, y1 = -1;
    for (const auto& t : tiles) {
        x0 = std::min(x0, t.tileX);
        y0 = std::min(y0, t.tileY);
        x1 = std::max(x1, t.tileX);
        y1 = std::max(y1, t.tileY);
    }
    l.minX = x0;
    l.minY = y0;
    l.tilesW = x1 - x0 + 1;
    l.tilesH = y1 - y0 + 1;
    return l;
}

/// An RGBA8 image (w x h) box-filtered down to out x out, into `dst` at
/// (dx, dy) of an RGBA8 image `dstW` texels wide. w and h must be multiples
/// of out.
inline void boxDownsample(const uint8_t* src, int w, int h, int out, uint8_t* dst, int dstW, int dx,
                          int dy) {
    const int fx = w / out, fy = h / out;
    if (fx <= 0 || fy <= 0) return;
    const uint32_t n = static_cast<uint32_t>(fx * fy);
    for (int y = 0; y < out; ++y) {
        for (int x = 0; x < out; ++x) {
            uint32_t s[4] = {0, 0, 0, 0};
            for (int j = 0; j < fy; ++j) {
                const uint8_t* row = src + (static_cast<size_t>(y * fy + j) * w + x * fx) * 4;
                for (int i = 0; i < fx; ++i)
                    for (int c = 0; c < 4; ++c) s[c] += row[i * 4 + c];
            }
            uint8_t* o = dst + (static_cast<size_t>(dy + y) * dstW + dx + x) * 4;
            for (int c = 0; c < 4; ++c) o[c] = static_cast<uint8_t>((s[c] + n / 2) / n);
        }
    }
}

/// Half the size, each texel the mean of four.
inline std::vector<uint8_t> halve(const std::vector<uint8_t>& src, int w, int h) {
    std::vector<uint8_t> out(static_cast<size_t>(w / 2) * (h / 2) * 4);
    for (int y = 0; y < h / 2; ++y) {
        for (int x = 0; x < w / 2; ++x) {
            for (int c = 0; c < 4; ++c) {
                const auto at = [&](int xx, int yy) {
                    return static_cast<uint32_t>(src[(static_cast<size_t>(yy) * w + xx) * 4 + c]);
                };
                const uint32_t sum = at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) +
                                     at(2 * x + 1, 2 * y + 1);
                out[(static_cast<size_t>(y) * (w / 2) + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
            }
        }
    }
    return out;
}

inline uint16_t to565(int r, int g, int b) {
    r = std::clamp(r, 0, 255);
    g = std::clamp(g, 0, 255);
    b = std::clamp(b, 0, 255);
    return static_cast<uint16_t>(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 |
                                 ((b * 31 + 127) / 255));
}

inline std::array<int, 3> from565(uint16_t c) {
    const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 3)};
}

/// One opaque BC1 block (8 bytes) from 16 RGBA8 texels, row by row: the
/// endpoints are the box's ends along its widest channel's spread (range
/// fit), each texel the nearest of the four colours. Plenty for a picture
/// seen from two miles off.
inline void encodeBC1Block(const uint8_t texels[16][4], uint8_t out[8]) {
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i)
        for (int c = 0; c < 3; ++c) {
            lo[c] = std::min(lo[c], static_cast<int>(texels[i][c]));
            hi[c] = std::max(hi[c], static_cast<int>(texels[i][c]));
        }
    // Inset by a sixteenth of the range, as the common fast encoders do: the
    // ends are then nearer the texels they stand for.
    for (int c = 0; c < 3; ++c) {
        const int inset = (hi[c] - lo[c]) / 16;
        lo[c] += inset;
        hi[c] -= inset;
    }
    uint16_t c0 = to565(hi[0], hi[1], hi[2]);
    uint16_t c1 = to565(lo[0], lo[1], lo[2]);
    if (c0 < c1) std::swap(c0, c1);
    uint32_t bits = 0;
    if (c0 != c1) {
        const auto e0 = from565(c0), e1 = from565(c1);
        int pal[4][3];
        for (int c = 0; c < 3; ++c) {
            pal[0][c] = e0[c];
            pal[1][c] = e1[c];
            pal[2][c] = (2 * e0[c] + e1[c]) / 3;
            pal[3][c] = (e0[c] + 2 * e1[c]) / 3;
        }
        for (int i = 0; i < 16; ++i) {
            int best = 0, bestD = 1 << 30;
            for (int k = 0; k < 4; ++k) {
                int d = 0;
                for (int c = 0; c < 3; ++c) {
                    const int e = static_cast<int>(texels[i][c]) - pal[k][c];
                    d += e * e;
                }
                if (d < bestD) {
                    bestD = d;
                    best = k;
                }
            }
            bits |= static_cast<uint32_t>(best) << (2 * i);
        }
    }
    out[0] = static_cast<uint8_t>(c0 & 0xff);
    out[1] = static_cast<uint8_t>(c0 >> 8);
    out[2] = static_cast<uint8_t>(c1 & 0xff);
    out[3] = static_cast<uint8_t>(c1 >> 8);
    std::memcpy(out + 4, &bits, 4);
}

/// An RGBA8 image (both sides multiples of 4) as BC1 blocks, row by row.
inline std::vector<uint8_t> encodeBC1(const std::vector<uint8_t>& rgba, int w, int h) {
    std::vector<uint8_t> out(static_cast<size_t>(w / 4) * (h / 4) * 8);
    uint8_t block[16][4];
    size_t o = 0;
    for (int by = 0; by < h / 4; ++by) {
        for (int bx = 0; bx < w / 4; ++bx) {
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i)
                    std::memcpy(block[j * 4 + i],
                                &rgba[(static_cast<size_t>(by * 4 + j) * w + bx * 4 + i) * 4], 4);
            encodeBC1Block(block, &out[o]);
            o += 8;
        }
    }
    return out;
}

/// The atlas as the GPU takes it: BC1 levels from 64 texels a tile down
/// to 4.
struct Atlas {
    AtlasLayout layout;
    std::vector<std::vector<uint8_t>> levels;
    int tilesFound = 0;
    [[nodiscard]] bool empty() const { return layout.empty() || levels.empty(); }
};

/// Every level of an RGBA8 atlas (layout's size), encoded.
inline Atlas encodeAtlas(const AtlasLayout& layout, std::vector<uint8_t> rgba) {
    Atlas a;
    a.layout = layout;
    int w = layout.width(), h = layout.height();
    for (int level = 0; level < kMipLevels && w >= 4 && h >= 4; ++level) {
        a.levels.push_back(encodeBC1(rgba, w, h));
        if (level + 1 < kMipLevels) {
            rgba = halve(rgba, w, h);
            w /= 2;
            h /= 2;
        }
    }
    return a;
}

}  // namespace wowee::rendering::far_field
