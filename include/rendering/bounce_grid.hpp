#pragma once

/// "Bounce light (enhanced)": what the reference client's probe GI gives its shade,
/// from a world-space grid instead of probes (docs/re/enhanced-lighting.md
/// section 6b).
///
/// The reference client traces rays against a software BVH into irradiance probes and adds
/// what they hold, resampled into a froxel volume, to every lit pixel: shade
/// under a canopy is darker and green, shade beside sunlit ground or a sunlit
/// wall is warm, a forest floor the sky cannot see is dim. What decides those
/// colours is what is around the pixel - grass, leaves overhead, a wall - and
/// that changes over yards, not pixels. So the surroundings are baked once
/// into a grid laid over the ground around the camera, 2 yards a cell, and
/// the ambient pass (ambient_occlusion.frag, BOUNCE_GRID) reads it at each
/// pixel's world position:
///
/// - the ground's colour (the map's minimap picture: the only colour the client
///   ships for the land from above), brightness held near the fill's own
///   grass-and-dirt colour so only the hue and a little of the value follow;
/// - how much canopy stands over the cell (the foliage models' crowns, from
///   their bounds), the leaves' colour (their textures' mean) and the height
///   the crown spans, so a pixel under it takes it and one above does not;
/// - the outdoor walls of buildings (small exterior WMO groups), their foot
///   and top, so a face turned toward one picks up its sunlit side.
///
/// Built from data already loaded, on a worker, when the camera has moved 48
/// yards or the world under it has changed; never in a frame's path. World space means it does not move with the view: nothing to flicker
/// as the camera turns, no rim where a character stands in front of a wall.
///
/// Pure: the splat, the blur and the packing are here for test_bounce_light;
/// gathering the inputs and the GPU copy are BounceGrid's (bounce_grid_gpu).

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <utility>
#include <vector>

namespace wowee::rendering::bounce_grid {

/// Cells across, yards a cell: 512 yards square, about the shadow map's
/// reach and well past where the ambient pass's shade can be told apart.
inline constexpr int kCells = 256;
inline constexpr float kCellYards = 2.0f;
inline constexpr float kSpan = kCells * kCellYards;
/// The grid is rebuilt when the camera is this far from its centre; the
/// outer quarter fades to the plain fill (the shader's kGridEdge).
inline constexpr float kRecentreYards = 48.0f;
/// Three RGBA16F layers (see Layer).
inline constexpr int kLayers = 3;

/// The fill light's ground colour (ambient_occlusion.frag kGroundAlbedo):
/// a cell's ground keeps this brightness within kGroundLumRange.
inline constexpr glm::vec3 kFillGroundAlbedo{0.45f, 0.42f, 0.28f};
/// The minimap's typical brightness for that ground (Elwynn's grass and
/// road, measured over its tiles): a cell this bright gets the fill's own.
inline constexpr float kMinimapLumRef = 0.26f;
inline constexpr float kGroundLumMin = 0.65f;
inline constexpr float kGroundLumMax = 1.35f;
/// How much of the minimap's own saturation a cell keeps. The minimap is
/// a painted, lit picture; its full chroma tinted the shade like paint.
inline constexpr float kGroundChroma = 0.8f;

/// A tree's crown. Discs, not boxes: a crown's footprint is round and an
/// axis-aligned box over a rotated tree is half again too big.
struct Canopy {
    glm::vec2 centre{0.0f};
    float radius = 0.0f;
    float bottom = 0.0f;   ///< z where the leaves start
    float top = 0.0f;      ///< z of the crown's top
    glm::vec3 leaf{0.3f, 0.4f, 0.15f};  ///< the leaf textures' mean colour
    float density = 0.8f;  ///< how much of the sky the crown takes at its middle
};

/// A building's outdoor wall block: an exterior WMO group's world box.
struct Building {
    glm::vec2 lo{0.0f};
    glm::vec2 hi{0.0f};
    float bottom = 0.0f;
    float top = 0.0f;
};

struct Inputs {
    /// World x, y of cell (0, 0)'s corner.
    glm::vec2 origin{0.0f};
    std::vector<Canopy> canopies;
    std::vector<Building> buildings;
    /// The ground colour at a world x, y (0..1 RGB), or false where there
    /// is none (no minimap there): the cell keeps the fill's own colour.
    std::function<bool(float x, float y, glm::vec3& rgb)> groundAt;
};

/// The baked grid, one float per channel; packed to halves for the GPU.
///   layer 0: ground rgb (an albedo near the fill's), canopy cover over the cell
///   layer 1: leaf rgb, wall presence (0..1)
///   layer 2: canopy bottom z, canopy top z, wall foot z, wall top z
struct Grid {
    glm::vec2 origin{0.0f};
    std::vector<float> data;  ///< kLayers x kCells x kCells x 4
    [[nodiscard]] bool empty() const { return data.empty(); }
    [[nodiscard]] float* at(int layer, int x, int y) {
        return &data[((static_cast<size_t>(layer) * kCells + y) * kCells + x) * 4];
    }
    [[nodiscard]] const float* at(int layer, int x, int y) const {
        return &data[((static_cast<size_t>(layer) * kCells + y) * kCells + x) * 4];
    }
};

inline float luminance(const glm::vec3& c) { return glm::dot(c, glm::vec3(0.299f, 0.587f, 0.114f)); }

/// The fill's ground colour moved toward a minimap colour: its hue and
/// part of its chroma, its brightness within a band round the fill's.
inline glm::vec3 groundAlbedo(const glm::vec3& minimap) {
    const float l = luminance(minimap);
    if (l < 1e-4f) return kFillGroundAlbedo;
    const float fillL = luminance(kFillGroundAlbedo);
    const float scale = std::clamp(l / kMinimapLumRef, kGroundLumMin, kGroundLumMax);
    const glm::vec3 grey(l);
    const glm::vec3 c = grey + (minimap - grey) * kGroundChroma;
    return glm::clamp(c * (fillL * scale / l), glm::vec3(0.0f), glm::vec3(1.0f));
}

/// The grid's corner for a camera at (x, y): snapped to whole cells, so
/// a rebuild puts every cell on the same ground as before.
inline glm::vec2 originFor(float x, float y) {
    const float half = kSpan * 0.5f;
    return {std::floor((x - half) / kCellYards) * kCellYards, std::floor((y - half) / kCellYards) * kCellYards};
}

/// Whether a camera at (x, y) has left the middle of a grid at `origin`.
inline bool needsRecentre(const glm::vec2& origin, float x, float y) {
    const glm::vec2 c = origin + glm::vec2(kSpan * 0.5f);
    return std::abs(x - c.x) > kRecentreYards || std::abs(y - c.y) > kRecentreYards;
}

namespace detail {
/// A separable box blur of one channel of one layer, `r` cells each way.
inline void blurChannel(Grid& g, int layer, int ch, int r) {
    std::vector<float> line(kCells), out(kCells);
    for (int pass = 0; pass < 2; ++pass) {
        for (int j = 0; j < kCells; ++j) {
            for (int i = 0; i < kCells; ++i) line[i] = pass == 0 ? g.at(layer, i, j)[ch] : g.at(layer, j, i)[ch];
            float s = 0.0f;
            int n = 0;
            for (int i = -r; i <= r; ++i) {
                if (i >= 0 && i < kCells) {
                    s += line[i];
                    ++n;
                }
            }
            for (int i = 0; i < kCells; ++i) {
                out[i] = s / static_cast<float>(std::max(n, 1));
                const int drop = i - r, add = i + r + 1;
                if (drop >= 0) {
                    s -= line[drop];
                    --n;
                }
                if (add < kCells) {
                    s += line[add];
                    ++n;
                }
            }
            for (int i = 0; i < kCells; ++i) (pass == 0 ? g.at(layer, i, j)[ch] : g.at(layer, j, i)[ch]) = out[i];
        }
    }
}
/// Cells whose mask is zero take the mean of their masked neighbours'
/// channels, `rings` times outward; the mask grows with them. What a
/// filtered read across the edge of a crown or a wall needs: the heights
/// carried on past it, so only the cover or presence fades.
inline void dilate(Grid& g, std::vector<float>& mask, std::initializer_list<std::pair<int, int>> channels,
                   int rings) {
    for (int iter = 0; iter < rings; ++iter) {
        std::vector<float> next = mask;
        for (int y = 0; y < kCells; ++y) {
            for (int x = 0; x < kCells; ++x) {
                const size_t i = static_cast<size_t>(y) * kCells + x;
                if (mask[i] > 0.0f) continue;
                float s[8] = {};
                float n = 0.0f;
                const int nx[4] = {x - 1, x + 1, x, x};
                const int ny[4] = {y, y, y - 1, y + 1};
                for (int k = 0; k < 4; ++k) {
                    if (nx[k] < 0 || ny[k] < 0 || nx[k] >= kCells || ny[k] >= kCells) continue;
                    if (mask[static_cast<size_t>(ny[k]) * kCells + nx[k]] <= 0.0f) continue;
                    int c = 0;
                    for (const auto& [layer, ch] : channels) s[c++] += g.at(layer, nx[k], ny[k])[ch];
                    n += 1.0f;
                }
                if (n <= 0.0f) continue;
                int c = 0;
                for (const auto& [layer, ch] : channels) g.at(layer, x, y)[ch] = s[c++] / n;
                next[i] = 1e-6f;
            }
        }
        mask.swap(next);
    }
}
}  // namespace detail

/// Bake the grid from its inputs.
inline Grid bake(const Inputs& in) {
    Grid g;
    g.origin = in.origin;
    g.data.assign(static_cast<size_t>(kLayers) * kCells * kCells * 4, 0.0f);
    const auto cellCentre = [&](int x, int y) {
        return in.origin + (glm::vec2(static_cast<float>(x), static_cast<float>(y)) + 0.5f) * kCellYards;
    };

    // Ground: the minimap's colour at the cell, or the fill's own.
    for (int y = 0; y < kCells; ++y) {
        for (int x = 0; x < kCells; ++x) {
            glm::vec3 c = kFillGroundAlbedo;
            glm::vec3 m;
            const glm::vec2 p = cellCentre(x, y);
            if (in.groundAt && in.groundAt(p.x, p.y, m)) c = groundAlbedo(m);
            float* l0 = g.at(0, x, y);
            l0[0] = c.r;
            l0[1] = c.g;
            l0[2] = c.b;
        }
    }

    // Canopy: each crown a disc, densest at its middle (1 - (d/r)^2); two
    // crowns over one cell let through what each lets through. The leaf
    // colour and the heights are the cover-weighted mean of the crowns.
    std::vector<float> wsum(static_cast<size_t>(kCells) * kCells, 0.0f);
    for (const Canopy& c : in.canopies) {
        if (c.radius <= 0.0f || c.density <= 0.0f) continue;
        const int x0 = std::max(0, static_cast<int>(std::floor((c.centre.x - c.radius - in.origin.x) / kCellYards)));
        const int x1 = std::min(kCells - 1, static_cast<int>(std::floor((c.centre.x + c.radius - in.origin.x) / kCellYards)));
        const int y0 = std::max(0, static_cast<int>(std::floor((c.centre.y - c.radius - in.origin.y) / kCellYards)));
        const int y1 = std::min(kCells - 1, static_cast<int>(std::floor((c.centre.y + c.radius - in.origin.y) / kCellYards)));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const glm::vec2 d = cellCentre(x, y) - c.centre;
                const float q = glm::dot(d, d) / (c.radius * c.radius);
                if (q >= 1.0f) continue;
                const float a = std::clamp(c.density * (1.0f - q), 0.0f, 1.0f);
                float* l0 = g.at(0, x, y);
                float* l1 = g.at(1, x, y);
                float* l2 = g.at(2, x, y);
                l0[3] = 1.0f - (1.0f - l0[3]) * (1.0f - a);
                float& w = wsum[static_cast<size_t>(y) * kCells + x];
                l1[0] += c.leaf.r * a;
                l1[1] += c.leaf.g * a;
                l1[2] += c.leaf.b * a;
                l2[0] += c.bottom * a;
                l2[1] += c.top * a;
                w += a;
            }
        }
    }
    for (int y = 0; y < kCells; ++y) {
        for (int x = 0; x < kCells; ++x) {
            const float w = wsum[static_cast<size_t>(y) * kCells + x];
            float* l1 = g.at(1, x, y);
            float* l2 = g.at(2, x, y);
            if (w > 0.0f) {
                l1[0] /= w;
                l1[1] /= w;
                l1[2] /= w;
                l2[0] /= w;
                l2[1] /= w;
            }
        }
    }
    // A cell no crown reaches takes its neighbours' leaf colour and heights
    // (a few rings outward), so the filtered read at a crown's edge blends
    // its cover to zero without blending its heights toward nothing.
    detail::dilate(g, wsum, {{1, 0}, {1, 1}, {1, 2}, {2, 0}, {2, 1}}, 4);
    // Where no crown is near at all: a crown far below anything (its
    // heights then never cover a pixel) and the fill's default leaf.
    for (int y = 0; y < kCells; ++y) {
        for (int x = 0; x < kCells; ++x) {
            if (wsum[static_cast<size_t>(y) * kCells + x] > 0.0f) continue;
            float* l1 = g.at(1, x, y);
            float* l2 = g.at(2, x, y);
            l1[0] = 0.3f;
            l1[1] = 0.4f;
            l1[2] = 0.15f;
            l2[0] = -20000.0f;
            l2[1] = -20000.0f;
        }
    }

    // Walls: each building box marks its cells with its foot and top (the
    // tallest where two overlap).
    for (const Building& b : in.buildings) {
        const int x0 = std::max(0, static_cast<int>(std::floor((b.lo.x - in.origin.x) / kCellYards)));
        const int x1 = std::min(kCells - 1, static_cast<int>(std::floor((b.hi.x - in.origin.x) / kCellYards)));
        const int y0 = std::max(0, static_cast<int>(std::floor((b.lo.y - in.origin.y) / kCellYards)));
        const int y1 = std::min(kCells - 1, static_cast<int>(std::floor((b.hi.y - in.origin.y) / kCellYards)));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                float* l1 = g.at(1, x, y);
                float* l2 = g.at(2, x, y);
                if (l1[3] > 0.0f && l2[3] >= b.top) continue;
                l1[3] = 1.0f;
                l2[2] = b.bottom;
                l2[3] = b.top;
            }
        }
    }
    std::vector<float> wallMask(static_cast<size_t>(kCells) * kCells, 0.0f);
    for (int y = 0; y < kCells; ++y)
        for (int x = 0; x < kCells; ++x) wallMask[static_cast<size_t>(y) * kCells + x] = g.at(1, x, y)[3];
    detail::dilate(g, wallMask, {{2, 2}, {2, 3}}, 4);
    for (int y = 0; y < kCells; ++y) {
        for (int x = 0; x < kCells; ++x) {
            if (wallMask[static_cast<size_t>(y) * kCells + x] > 0.0f) continue;
            float* l2 = g.at(2, x, y);
            l2[2] = -20000.0f;
            l2[3] = -20000.0f;
        }
    }

    // Soften: the ground's colour over ~5 yards (light bounces from an area,
    // not a point), the cover over ~3 (crowns' edges are ragged).
    for (int ch = 0; ch < 3; ++ch) detail::blurChannel(g, 0, ch, 2);
    detail::blurChannel(g, 0, 3, 1);
    return g;
}

/// IEEE half from a float (round to nearest; no NaN in the grid).
inline uint16_t toHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t e = static_cast<int32_t>((x >> 23) & 0xff) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (e <= 0) {
        if (e < -10) return static_cast<uint16_t>(sign);
        m |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - e);
        uint32_t h = m >> shift;
        if ((m >> (shift - 1)) & 1u) ++h;
        return static_cast<uint16_t>(sign | h);
    }
    if (e >= 31) return static_cast<uint16_t>(sign | 0x7bffu);  // the largest finite
    uint32_t h = (static_cast<uint32_t>(e) << 10) | (m >> 13);
    if (m & 0x1000u) ++h;  // round half up
    if (h >= 0x7c00u) h = 0x7bffu;
    return static_cast<uint16_t>(sign | h);
}

/// The grid as the GPU image's bytes: layer after layer, RGBA16F.
inline void pack(const Grid& g, std::vector<uint16_t>& out) {
    out.resize(g.data.size());
    for (size_t i = 0; i < g.data.size(); ++i) out[i] = toHalf(g.data[i]);
}

}  // namespace wowee::rendering::bounce_grid
