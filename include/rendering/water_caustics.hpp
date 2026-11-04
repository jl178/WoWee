#pragma once

/// Sunlight under water (E15, the "Sunlight under water" enhancement row).
///
/// What it draws. The sun focused by the moving surface lays a net of soft
/// bright lines on whatever lies under shallow water - sand, rock, a wreck, a
/// pier post - drifting slowly. It is light, not paint: what shows through the
/// surface is multiplied up by at most 1 + kMaxGain where a line crosses the
/// bed below, so dark rock gets a dim net and pale sand a brighter one. Water
/// and ocean only (not magma or slime), not water inside a building, not at
/// night, not in the sun's shadow while the shadow map is on.
///
/// How. WaterRenderer draws each water surface in view a second time, after
/// the water itself, with water_caustics.frag: the pixel behind the surface is
/// found from the scene's depth copy (the one the shoreline foam reads), its
/// height below the surface gives the depth, and the pattern is evaluated
/// where the sun's ray through that bed point enters the water. The blend is
/// dst * (1 + src). Drawn before the water instead - lighting the bed alone -
/// it was all but invisible: the stock water's alpha lets a tenth or two of
/// the bed through even a hand-span down. No texture is made; the pattern is
/// two layers of moving cell edges in the shader.
///
/// The shader carries the same constants and curves (water_caustics.frag);
/// test_phase2_water holds these.

#include <algorithm>

#include <glm/glm.hpp>

namespace wowee {
namespace rendering {
namespace water_caustics {

/// The most the ground under a bright line is brightened: 1 + this.
inline constexpr float kMaxGain = 0.3f;
/// Depth (yards below the surface) over which the net forms - at the very
/// waterline the surface focuses nothing - and by which it is gone.
inline constexpr float kDepthIn = 0.15f;
inline constexpr float kDepthPeak = 0.6f;
inline constexpr float kDepthOut = 4.0f;
/// Distance from the camera over which it fades out: past this the lines are
/// finer than a pixel and would only shimmer.
inline constexpr float kFarStart = 45.0f;
inline constexpr float kFarEnd = 80.0f;

inline float smoothstep01(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// How much sun there is to focus, from the height of the sun on the stock
/// clock (LightingParams::sunDir.z, the sun sprite's direction). Nothing once
/// it is under the horizon; full from about 15 degrees up. The light's own
/// colour, which the shader also multiplies by, dims it further at dusk and
/// under storm light.
inline float dayStrength(float sunDirZ) { return smoothstep01(0.02f, 0.25f, sunDirZ); }

/// The net's strength at a depth below the surface: rising over the first
/// few hand-spans, full by kDepthPeak, gone by kDepthOut.
inline float depthFade(float depth) {
    if (!(depth > 0.0f)) return 0.0f;
    const float in = smoothstep01(0.0f, kDepthIn, depth) * 0.5f +
                     0.5f * smoothstep01(kDepthIn, kDepthPeak, depth);
    const float out = 1.0f - smoothstep01(kDepthPeak, kDepthOut, depth);
    return in * out;
}

/// The net's strength at a distance from the camera.
inline float distanceFade(float dist) { return 1.0f - smoothstep01(kFarStart, kFarEnd, dist); }

/// Whether a liquid surface lies wholly past the net's reach (kFarEnd) from
/// the eye: its box from an origin and two edge vectors in the horizontal
/// plane, and its height range. Such a surface is not drawn a second time.
inline bool beyondReach(const glm::vec3& origin, const glm::vec3& edgeX, const glm::vec3& edgeY,
                        float minZ, float maxZ, const glm::vec3& eye) {
    const glm::vec3 c[4] = {origin, origin + edgeX, origin + edgeY, origin + edgeX + edgeY};
    glm::vec3 lo(c[0].x, c[0].y, minZ), hi(c[0].x, c[0].y, maxZ);
    for (const auto& p : c) {
        lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y);
        hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y);
    }
    const glm::vec3 nearest = glm::clamp(eye, lo, hi);
    const glm::vec3 d = nearest - eye;
    return glm::dot(d, d) > kFarEnd * kFarEnd;
}

/// Whether a surface takes the net: water (0) and ocean (1), not magma (2)
/// or slime (3), and not water inside a building.
inline bool liquidTakesCaustics(int basicType, bool interior) {
    return (basicType == 0 || basicType == 1) && !interior;
}

}  // namespace water_caustics
}  // namespace rendering
}  // namespace wowee
