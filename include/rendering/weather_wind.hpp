#pragma once

/// "Rain and snow in the wind" (docs/enhancements-plan.md E19).
///
/// The stock weather falls on its own fixed drift (Weather::spawnParticle:
/// rain 1.5, 0.8 yd/s across, a storm 7, 3.5) whatever the trees are doing.
/// With the row on, each drop and flake is carried as well by the wind that
/// moves the foliage: its direction (m2.vert's kWindDir), its eased strength
/// (Renderer's cloth wind: the setting times the weather's stir) and the same
/// gust band that rolls across the trees, so a gust visibly passes through the
/// rain and the canopy together. Snow takes less of it and eddies.
///
/// Pure, for Weather and test_phase2_motion.

#include <glm/glm.hpp>

#include <cmath>

namespace wowee {
namespace rendering {
namespace weather_wind {

/// The prevailing wind's direction, m2.vert's kWindDir.
inline glm::vec2 direction() { return glm::normalize(glm::vec2(0.80f, 0.60f)); }

/// Air speed, yd/s, at strength 1 and the gust band's peak; the band's mean is
/// 0.55 of it, about the 5 yd/s the capes take (Renderer, E7).
inline constexpr float kPeakSpeed = 9.0f;
/// How much of the wind a snowflake takes, and the eddy's size (yd/s per
/// unit of strength) about its drift.
inline constexpr float kSnowShare = 0.35f;
inline constexpr float kSnowEddy = 0.6f;

/// m2.vert's gust band at a point: 0.1-1, a swell rolling downwind.
inline float gust(const glm::vec2& p, float time) {
    return 0.55f + 0.45f * std::sin(glm::dot(p, direction()) * 0.11f - time * 0.7f);
}

/// The wind's push on a drop or flake at `p` (yd/s, horizontal). `strength`
/// is the foliage's eased wind (0: none, and nothing is added); `phase` the
/// flake's own (its stock sway phase), for the eddy.
inline glm::vec2 push(float strength, const glm::vec2& p, float time, bool snow, float phase) {
    if (!(strength > 0.0f)) return glm::vec2(0.0f);
    const glm::vec2 dir = direction();
    glm::vec2 v = dir * (kPeakSpeed * strength * gust(p, time));
    if (snow) {
        v *= kSnowShare;
        const float a = time * 0.9f + phase * 2.3f;
        v += glm::vec2(std::cos(a), std::sin(a)) * (kSnowEddy * strength);
    }
    return v;
}

}  // namespace weather_wind
}  // namespace rendering
}  // namespace wowee
