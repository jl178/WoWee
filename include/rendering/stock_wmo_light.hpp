#pragma once

/// How the stock 3.3.5a client (12340) lights a WMO, as pure functions.
///
/// Read out of Wow.exe; docs/re/world-lighting.md (WMO section) has the full
/// walk-through. In short:
///
/// - Load (CMapObjGroup, 0x7d7c30): a group with MOCV has its colours fixed by
///   0x7d7380 unless MOHD flag 0x8 is set - fixColorVertexAlpha below.
/// - First draw (0x7abf50 -> 0x7d78c0): the trans-batch vertices are pulled
///   toward 127 grey and given an alpha from their distance to the group's
///   portals into exterior groups - portalTerm / attenTransVertex below.
/// - A group without MOCV gets 0x7f7f7f in its vertex buffer, or black when
///   the root is MOHD flag 0x2 (0x7c8560) - noMocvColor below.
/// - Draw: 0x7ac6a0 (no MOCV) / 0x7ac9f0 (MOCV), or 0x7a9380 for every group
///   of a MOHD 0x2 root. The light per batch is one of four modes (0x7a8b10):
///   0 none, 1 the outdoor light, 2 the "window" light (windowLight), 3 the
///   MOHD ambient alone. The vertex programs (MapObjDiffuse_T1 / MapObjUDiffuse_T1,
///   Data/.../shaders/vertex/arbvp1) then compute mapObjLit / mapObjULit and
///   the pixel program doubles it (Mod2x).
/// - F_SIDN materials add their sidnColor scaled by the time of day
///   (0x7a8520, table 0xaf4c80) - sidnNightFactor. wmo.frag.glsl turns it
///   into the frame colour, sidnColor times floor(f * 255) / 256.
/// - A trans batch is drawn twice (0x7ac9f0 / 0x7a9380), the outdoor pass
///   with SrcAlphaOpaque and the room pass with InvSrcAlphaAdd, so the two
///   meet at the vertex alpha the portals gave it; wmo.frag.glsl mixes them.

#include "rendering/stock_day_night.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace wowee::rendering::stock_wmo {

/// A colour as the client keeps it: four bytes.
struct Rgba8 {
    uint8_t r = 0, g = 0, b = 0, a = 0;
    bool operator==(const Rgba8&) const = default;
};

/// 0x7d7380, CMapObjGroup::FixColorVertexAlpha (3.3.5a has no MOHD ambient
/// term here - later clients subtract it). Trans-batch vertices are halved
/// and keep their alpha; the rest become (c + c*a/64) / 2, clamped, alpha 255.
inline Rgba8 fixColorVertexAlpha(Rgba8 c, bool transVertex) {
    if (transVertex) {
        return {static_cast<uint8_t>(c.r >> 1), static_cast<uint8_t>(c.g >> 1),
                static_cast<uint8_t>(c.b >> 1), c.a};
    }
    const auto fix = [a = static_cast<uint32_t>(c.a)](uint8_t v) {
        const uint32_t x = ((static_cast<uint32_t>(v) * a >> 6) + v) >> 1;
        return static_cast<uint8_t>(std::min<uint32_t>(x, 255u));
    };
    return {fix(c.r), fix(c.g), fix(c.b), 255};
}

/// 0x7c8560: the colour a group without MOCV is drawn with. Neutral grey,
/// which the shader's doubling turns into 1; black for the unified (MOHD 0x2)
/// path, whose shader adds the vertex colour to the light instead.
inline Rgba8 noMocvColor(bool unifiedRoot) {
    return unifiedRoot ? Rgba8{0, 0, 0, 255} : Rgba8{127, 127, 127, 255};
}

/// 0x7d78c0, one portal's share of a trans vertex's outdoor weight.
/// `dist` is the vertex's distance to the portal: the signed plane distance
/// (positive into the group that owns the reference) when its projection
/// falls inside the portal, otherwise the distance to the polygon.
/// A portal into an exterior group (MOGI flags & 0x48) adds 1 - 0.15*max(d,0)
/// when that is above 0.001; a vertex within a yard of a portal into another
/// interior group gets no outdoor light at all (`blocked`).
inline float portalTerm(float dist, bool destExterior, bool& blocked) {
    blocked = false;
    if (!destExterior) {
        blocked = dist > -1.0f && dist < 1.0f;
        return 0.0f;
    }
    const float t = 1.0f - std::max(dist, 0.0f) * 0.15f;  // 0x9f23d4
    return t > 0.001f ? t : 0.0f;                          // 0x9e1134
}

/// The sum of the terms, as 0x7d78c0 finishes it: at most 1, and nothing at
/// all unless above 0.001.
inline float finishWeight(float sum) {
    if (!(sum > 0.001f)) return 0.0f;
    return std::min(sum, 1.0f);
}

/// 0x7d78c0's write: rgb toward 127 by w (rounded), alpha w * 255.
inline Rgba8 attenTransVertex(Rgba8 c, float w) {
    const auto toward = [w](uint8_t v) {
        return static_cast<uint8_t>(std::lround(v + (127.0f - v) * w));
    };
    return {toward(c.r), toward(c.g), toward(c.b),
            static_cast<uint8_t>(std::lround(w * 255.0f))};
}

/// Distance from `p` to a convex portal polygon on the plane n.x + d = 0,
/// the way 0x7d78c0 measures it: when p's projection is inside the polygon,
/// the plane distance (times `side`, MOPR's -1/1); otherwise the distance to
/// the nearest edge, which is never negative.
inline float portalDistance(const glm::vec3& p, std::span<const glm::vec3> poly,
                            const glm::vec3& n, float d, int side) {
    const float plane = glm::dot(n, p) + d;
    if (poly.size() < 3) return std::abs(plane);
    const glm::vec3 q = p - n * plane;
    bool inside = true;
    float sign = 0.0f;
    for (size_t i = 0; i < poly.size() && inside; ++i) {
        const glm::vec3& a = poly[i];
        const glm::vec3& b = poly[(i + 1) % poly.size()];
        const float s = glm::dot(glm::cross(b - a, q - a), n);
        if (std::abs(s) < 1e-6f) continue;
        if (sign == 0.0f) sign = s > 0.0f ? 1.0f : -1.0f;
        else if ((s > 0.0f) != (sign > 0.0f)) inside = false;
    }
    if (inside) return side == 1 ? plane : -plane;
    float best = 3.4e38f;
    for (size_t i = 0; i < poly.size(); ++i) {
        const glm::vec3& a = poly[i];
        const glm::vec3& b = poly[(i + 1) % poly.size()];
        const glm::vec3 ab = b - a;
        const float len2 = glm::dot(ab, ab);
        const float t = len2 > 0.0f ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        best = std::min(best, glm::length(p - (a + ab * t)));
    }
    return best;
}

/// 0xaf4c80: how much of an F_SIDN material's glow is on, by the time of
/// day (0 midnight, 0.5 noon). Full from 21:30 to 06:00, off from 07:00 to
/// 20:30, linear between.
inline constexpr std::array<stock_day_night::Key, 4> kSidnNight{{
    {0.25f, 1.0f}, {0.29166666f, 0.0f}, {0.85416663f, 0.0f}, {0.89583331f, 1.0f},
}};

inline float sidnNightFactor(float dayFraction) {
    return stock_day_night::sample(kSidnNight, dayFraction);
}

/// 0x7a8b10 mode 2 (F_WINDOW, MOMT flag 0x20), out of 0x7ee750's 0xd38cb0
/// and 0xd38cb4: the diffuse is the mean of the sky's diffuse and ambient,
/// and the ambient the same mean plus 16/255.
struct LightPair {
    glm::vec3 diffuse{0.0f};
    glm::vec3 ambient{0.0f};
};
inline LightPair windowLight(const glm::vec3& diffuse, const glm::vec3& ambient) {
    const glm::vec3 mean = (diffuse + ambient) * 0.5f;
    return {mean, glm::min(mean + glm::vec3(16.0f / 255.0f), glm::vec3(1.0f))};
}

/// The light the vertex programs use: clamp(ambient + diffuse * clamp(N.L)).
inline glm::vec3 vertexLight(const glm::vec3& ambient, const glm::vec3& diffuse, float nDotL) {
    return glm::clamp(ambient + diffuse * std::clamp(nDotL, 0.0f, 1.0f), 0.0f, 1.0f);
}

/// MapObjDiffuse_T1 (lit) and the pixel program's doubling: what the texture
/// is multiplied by. `emissive` is the material's frame sidn colour; the
/// client halves it into c29 (0x7a8940) before the doubling gives it back.
inline glm::vec3 mapObjLit(const glm::vec3& mocv, const glm::vec3& light, const glm::vec3& emissive) {
    return 2.0f * glm::clamp(mocv * light + emissive * 0.5f, 0.0f, 1.0f);
}

/// MapObjUDiffuse_T1 (the unified path, MOHD 0x2): the vertex colour is added
/// to the light, which is first scaled by the material diffuse c28 = 0x7f/255.
inline glm::vec3 mapObjULit(const glm::vec3& mocv, const glm::vec3& light, const glm::vec3& emissive) {
    constexpr float kC28 = 127.0f / 255.0f;
    return 2.0f * glm::clamp(light * kC28 + mocv + emissive * 0.5f, 0.0f, 1.0f);
}

/// Variant 0 of both programs, lighting off: the vertex colour, doubled.
inline glm::vec3 mapObjUnlit(const glm::vec3& mocv) { return 2.0f * mocv; }

}  // namespace wowee::rendering::stock_wmo
