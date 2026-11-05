#pragma once

/// Indoor light for units and doodads (docs/enhancements-plan.md E12, the
/// "Room light indoors" row). Not stock.
///
/// Stock lights a unit inside a building from the MOCV under its feet: a ray
/// in the WMO group from just above the unit down to just below it, the
/// barycentric vertex colour x 2 at the hit (plus the MOHD ambient when MOHD
/// flags & 2), split into a diffuse raised to 168/255 and an ambient capped at
/// 96/255, from a fixed direction above (docs/re/characters.md section 1
/// "Indoors (per unit)"). Wowee has no per-unit ray yet and gives every unit
/// the player's building's MOHD ambient.
///
/// With the row on, each interior group (MOGP 0x2000 and not 0x48, with MOCV)
/// gets a small grid of probes when it loads: the light its walls, floor and
/// ceiling carry in their MOCV (x 2, as the stock sample), averaged around
/// each cell by area, plus the light of the MODD doodads standing in it. A
/// unit is lit by the room around it rather than by the building's one
/// colour: warm under an inn's lamps, cold in a crypt, darker in a corner;
/// and the light leans a little toward the brighter side of the room. The
/// stock split and fixed direction still shape the light; this only chooses
/// its colour and adds a faint fill from the room on the side away from it.
///
/// Everything here is CPU arithmetic on small arrays; the tests hold the bake
/// and the lookup (test_phase2_light).

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wowee {
namespace rendering {
namespace indoor_probe {

/// The target cell edge, yards: about a room's corner to its middle.
inline constexpr float kCellYards = 3.0f;
/// No axis has more cells than this; a bigger group gets bigger cells.
inline constexpr int kMaxCellsPerAxis = 16;
/// How far the room's colour replaces the building's one colour (the rest is
/// the stock colour): gentle, so a unit never looks lit by something else.
inline constexpr float kRoomMix = 0.7f;
/// How much of the room's own saturation is kept (unitColour).
inline constexpr float kRoomSaturation = 0.75f;
/// How far the light leans toward the brighter side, at most (a fraction of
/// the fixed direction's horizontal length, which is 0.44).
inline constexpr float kMaxLean = 0.5f;
/// The fill from the room on the side away from the light, as a fraction of
/// the indoor ambient (character.frag, E12).
inline constexpr float kFill = 0.35f;
/// How strongly a doodad's own MODD light is replaced by the room's.
inline constexpr float kDoodadAmbientMix = 0.5f;
inline constexpr float kDoodadDiffuseMix = 0.35f;
/// A MODD doodad counts as this many square yards of wall.
inline constexpr float kDoodadWeight = 4.0f;

/// One light sample: a point on a surface (or a doodad) and the colour the
/// stock rule would give a unit standing there (MOCV x 2, clamped).
struct Sample {
    glm::vec3 pos{0.0f};
    glm::vec3 colour{0.0f};
    float weight = 1.0f;
};

/// A group's probes, in the model's space. Colours are 0..1 per channel,
/// packed RGBA8 (A unused) to keep a dungeon's few hundred groups small.
struct Grid {
    glm::vec3 origin{0.0f};  ///< the first cell's centre
    glm::vec3 cell{1.0f};    ///< cell edge per axis
    glm::ivec3 dims{0};
    std::vector<uint32_t> rgba;
    glm::vec3 mean{0.0f};

    [[nodiscard]] bool empty() const { return rgba.empty(); }
    [[nodiscard]] size_t index(int x, int y, int z) const {
        return (static_cast<size_t>(z) * dims.y + y) * dims.x + x;
    }
    [[nodiscard]] glm::vec3 at(int x, int y, int z) const {
        const uint32_t c = rgba[index(x, y, z)];
        return glm::vec3(static_cast<float>(c & 0xffu), static_cast<float>((c >> 8) & 0xffu),
                         static_cast<float>((c >> 16) & 0xffu)) / 255.0f;
    }
};

inline uint32_t packColour(const glm::vec3& c) {
    const auto b = [](float v) {
        return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    return b(c.r) | (b(c.g) << 8) | (b(c.b) << 16) | 0xff000000u;
}

/// Cells per axis for an extent: about kCellYards each, 1..kMaxCellsPerAxis.
inline int cellsFor(float extent) {
    if (!(extent > 0.0f)) return 1;
    const int n = static_cast<int>(std::ceil(extent / kCellYards));
    return std::clamp(n, 1, kMaxCellsPerAxis);
}

/// Bake a group's grid over [bmin, bmax] from its samples. Each sample is
/// spread over the eight cells around it (trilinear weights), the sums are
/// blurred twice by [1 2 1] along each axis - about two cells, six yards -
/// and divided by the blurred weights, so a cell holds the area-weighted
/// light of the surfaces near it. A cell no surface reaches takes its
/// neighbours' (grown in from the walls), and with no samples at all the
/// grid is empty. Cost: the samples once, then a few passes over at most
/// 16^3 cells.
inline Grid bake(const std::vector<Sample>& samples, const glm::vec3& bmin, const glm::vec3& bmax) {
    Grid g;
    if (samples.empty()) return g;
    const glm::vec3 ext = glm::max(bmax - bmin, glm::vec3(0.0f));
    g.dims = glm::ivec3(cellsFor(ext.x), cellsFor(ext.y), cellsFor(ext.z));
    g.cell = glm::max(ext / glm::vec3(g.dims), glm::vec3(0.01f));
    g.origin = bmin + g.cell * 0.5f;
    const size_t n = static_cast<size_t>(g.dims.x) * g.dims.y * g.dims.z;

    std::vector<glm::vec4> acc(n, glm::vec4(0.0f));  // rgb * w, w
    glm::vec3 meanSum(0.0f);
    float meanW = 0.0f;
    for (const Sample& s : samples) {
        if (!(s.weight > 0.0f)) continue;
        meanSum += s.colour * s.weight;
        meanW += s.weight;
        const glm::vec3 f = glm::clamp((s.pos - g.origin) / g.cell, glm::vec3(0.0f), glm::vec3(g.dims - 1));
        const glm::ivec3 i0 = glm::min(glm::ivec3(glm::floor(f)), g.dims - 1);
        const glm::ivec3 i1 = glm::min(i0 + 1, g.dims - 1);
        const glm::vec3 t = f - glm::vec3(i0);
        for (int k = 0; k < 8; ++k) {
            const int x = (k & 1) ? i1.x : i0.x;
            const int y = (k & 2) ? i1.y : i0.y;
            const int z = (k & 4) ? i1.z : i0.z;
            const float w = ((k & 1) ? t.x : 1.0f - t.x) * ((k & 2) ? t.y : 1.0f - t.y) *
                            ((k & 4) ? t.z : 1.0f - t.z) * s.weight;
            if (w <= 0.0f) continue;
            acc[g.index(x, y, z)] += glm::vec4(s.colour * w, w);
        }
    }
    if (!(meanW > 0.0f)) return Grid{};
    g.mean = glm::clamp(meanSum / meanW, glm::vec3(0.0f), glm::vec3(1.0f));

    // Two [1 2 1] passes along each axis, clamped at the ends.
    std::vector<glm::vec4> tmp(n);
    for (int pass = 0; pass < 2; ++pass) {
        for (int axis = 0; axis < 3; ++axis) {
            const int len = g.dims[axis];
            if (len < 2) continue;
            for (int z = 0; z < g.dims.z; ++z)
                for (int y = 0; y < g.dims.y; ++y)
                    for (int x = 0; x < g.dims.x; ++x) {
                        glm::ivec3 p(x, y, z), a = p, b = p;
                        a[axis] = std::max(p[axis] - 1, 0);
                        b[axis] = std::min(p[axis] + 1, len - 1);
                        tmp[g.index(x, y, z)] = 0.25f * acc[g.index(a.x, a.y, a.z)] +
                                                0.5f * acc[g.index(x, y, z)] +
                                                0.25f * acc[g.index(b.x, b.y, b.z)];
                    }
            acc.swap(tmp);
        }
    }

    // Normalise; grow into cells nothing reached.
    float maxW = 0.0f;
    for (const auto& a : acc) maxW = std::max(maxW, a.w);
    const float floorW = maxW * 1e-4f;
    std::vector<glm::vec3> col(n, glm::vec3(0.0f));
    std::vector<uint8_t> have(n, 0);
    size_t missing = 0;
    for (size_t i = 0; i < n; ++i) {
        if (acc[i].w > floorW) {
            col[i] = glm::vec3(acc[i]) / acc[i].w;
            have[i] = 1;
        } else {
            ++missing;
        }
    }
    for (int iter = 0; missing > 0 && iter < 3 * kMaxCellsPerAxis; ++iter) {
        std::vector<uint8_t> next = have;
        for (int z = 0; z < g.dims.z; ++z)
            for (int y = 0; y < g.dims.y; ++y)
                for (int x = 0; x < g.dims.x; ++x) {
                    const size_t i = g.index(x, y, z);
                    if (have[i]) continue;
                    glm::vec3 sum(0.0f);
                    int cnt = 0;
                    const glm::ivec3 nb[6] = {{x - 1, y, z}, {x + 1, y, z}, {x, y - 1, z},
                                              {x, y + 1, z}, {x, y, z - 1}, {x, y, z + 1}};
                    for (const auto& q : nb) {
                        if (q.x < 0 || q.y < 0 || q.z < 0 || q.x >= g.dims.x || q.y >= g.dims.y ||
                            q.z >= g.dims.z) continue;
                        const size_t j = g.index(q.x, q.y, q.z);
                        if (!have[j]) continue;
                        sum += col[j];
                        ++cnt;
                    }
                    if (cnt > 0) {
                        col[i] = sum / static_cast<float>(cnt);
                        next[i] = 1;
                        --missing;
                    }
                }
        have.swap(next);
    }
    g.rgba.resize(n);
    for (size_t i = 0; i < n; ++i) g.rgba[i] = packColour(have[i] ? col[i] : g.mean);
    return g;
}

/// The grid's colour at `p` (model space), trilinear between cell centres and
/// held at the outer cells' value beyond them. Black for an empty grid.
inline glm::vec3 sample(const Grid& g, const glm::vec3& p) {
    if (g.empty()) return glm::vec3(0.0f);
    const glm::vec3 f = glm::clamp((p - g.origin) / g.cell, glm::vec3(0.0f), glm::vec3(g.dims - 1));
    const glm::ivec3 i0 = glm::min(glm::ivec3(glm::floor(f)), g.dims - 1);
    const glm::ivec3 i1 = glm::min(i0 + 1, g.dims - 1);
    const glm::vec3 t = f - glm::vec3(i0);
    const glm::vec3 c00 = glm::mix(g.at(i0.x, i0.y, i0.z), g.at(i1.x, i0.y, i0.z), t.x);
    const glm::vec3 c10 = glm::mix(g.at(i0.x, i1.y, i0.z), g.at(i1.x, i1.y, i0.z), t.x);
    const glm::vec3 c01 = glm::mix(g.at(i0.x, i0.y, i1.z), g.at(i1.x, i0.y, i1.z), t.x);
    const glm::vec3 c11 = glm::mix(g.at(i0.x, i1.y, i1.z), g.at(i1.x, i1.y, i1.z), t.x);
    return glm::mix(glm::mix(c00, c10, t.y), glm::mix(c01, c11, t.y), t.z);
}

inline float luminance(const glm::vec3& c) { return glm::dot(c, glm::vec3(0.299f, 0.587f, 0.114f)); }

/// Which way the room is brighter at `p`, in the model's horizontal plane:
/// the luminance's change over one cell each way, as a fraction of the
/// luminance there, held to kMaxLean. Zero in an even room, in a grid one
/// cell wide, or in the dark.
inline glm::vec2 lean(const Grid& g, const glm::vec3& p) {
    if (g.empty()) return glm::vec2(0.0f);
    const float l = luminance(sample(g, p));
    if (l < 0.02f) return glm::vec2(0.0f);
    glm::vec2 d(0.0f);
    for (int axis = 0; axis < 2; ++axis) {
        if (g.dims[axis] < 2) continue;
        glm::vec3 a = p, b = p;
        a[axis] -= g.cell[axis];
        b[axis] += g.cell[axis];
        d[axis] = (luminance(sample(g, b)) - luminance(sample(g, a))) / (2.0f * l);
    }
    const float len = glm::length(d);
    if (len > kMaxLean) d *= kMaxLean / len;
    return d;
}

/// The colour a unit indoors is lit by with the row on: the stock colour
/// (the building's, as Wowee hands it to character.frag) moved kRoomMix of
/// the way to the room's at the unit.
inline glm::vec3 unitColour(const glm::vec3& stockColour, const glm::vec3& room) {
    // The room's hue a quarter muted first: the stock split raises a dim
    // colour's brightest channel to 168/255 and keeps its hue whole, which on
    // skin read as a tint rather than as light.
    const glm::vec3 muted = glm::mix(glm::vec3(luminance(room)), room, kRoomSaturation);
    return glm::mix(stockColour, muted, kRoomMix);
}

/// Ease a unit's indoor light toward its new target: the stock client steps
/// the ambient over 0.5 s (characters.md section 1 step 6); this eases
/// colour, weight and lean together with a 0.25 s time constant.
inline float easeFactor(float dt) {
    if (!(dt > 0.0f)) return 0.0f;
    return 1.0f - std::exp(-dt / 0.25f);
}

/// Whether a unit's probe should be read again: it moved a third of a yard,
/// or half a second passed. Standing NPCs cost one lookup every half second.
inline bool needsResample(const glm::vec3& at, const glm::vec3& last, float sinceSeconds) {
    const glm::vec3 d = at - last;
    return glm::dot(d, d) > (1.0f / 9.0f) || sinceSeconds >= 0.5f;
}

}  // namespace indoor_probe
}  // namespace rendering
}  // namespace wowee
