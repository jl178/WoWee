#pragma once

/// The stock client's sun light, sun and moon, by the hour.
///
/// Read out of Wow.exe 3.3.5a (12340). DayNight's per-frame update (0x7f3920)
/// calls two functions that turn the time of day into directions:
///
/// - 0x7eea90 builds the world's directional light, the one terrain, M2s and
///   WMO exteriors are shaded by (written to 0xd38c9c, copied into the light
///   0x7ee750 hands the renderer). Global: no map, zone or latitude term.
/// - 0x7eecc0 places the sun sprite (Textures\sunCenter.blp) and the two moons
///   at twelve yards from the camera.
///
/// Both read an angle pair off small keyframe tables - phi from the zenith,
/// theta round from +X - interpolated by 0x7ed3b0, and turn the pair into
/// (cos theta sin phi, sin theta sin phi, cos phi) in WoW world coordinates
/// (+X north, +Y west, +Z up).
///
/// They are not the same direction. The light never follows the sun: it comes
/// from the north-west at 37.5 degrees above the horizon at noon and midnight
/// and 20 degrees at six, on a half-day cycle, and keeps shining at night -
/// Light.dbc's night colours are what make it moonlight. The sun sprite climbs
/// from the same north-west horizon to five degrees off the zenith at noon and
/// sets back where it rose.
///
/// Wowee had one invented formula for all of it, which put the noon sun 31
/// degrees up and drew the sprite, the shafts and the occlusion test there.

#include "core/coordinates.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <span>

namespace wowee::rendering {

namespace stock_day_night {

/// One keyframe: time as a fraction of the day (0 midnight, 0.5 noon), and an
/// angle in radians.
struct Key {
    float time;
    float value;
};

/// 0x7ed3b0: the table's value at `dayFraction`, linear between keys and
/// wrapping from the last key round to the first through midnight.
inline float sample(std::span<const Key> keys, float dayFraction) {
    const float t = glm::clamp(dayFraction, 0.0f, 1.0f);
    const size_t n = keys.size();
    size_t hi = 0;
    while (hi < n && t > keys[hi].time) ++hi;
    // Before the first key or past the last: between the last and the first.
    size_t lo = 0;
    if (hi == n || hi == 0) {
        hi = 0;
        lo = n - 1;
    } else {
        lo = hi - 1;
    }
    float span = keys[hi].time - keys[lo].time;
    if (std::abs(span) < 0.001f) return keys[lo].value;
    if (span < 0.0f) span += 1.0f;
    float into = t - keys[lo].time;
    if (into < 0.0f) into += 1.0f;
    return keys[lo].value + (keys[hi].value - keys[lo].value) * (into / span);
}

/// The client's cosine of `x` half turns (x = angle / pi), as every caller of
/// 0x5fe800 computes it: whole half turns flip the sign, and the fraction f
/// goes through the cubic 1 - 6f^2 + 4f^3 rather than a real cosine. Within
/// two percent of cos(pi x), and the reason its vectors come out 0.96 long.
inline float cosHalfTurns(float x) {
    const float whole = std::floor(x);
    const float f = x - whole;
    const float c = 1.0f - (6.0f - 4.0f * f) * f * f;
    return (static_cast<long long>(whole) & 1) ? -c : c;
}

/// The direction both functions build from phi and theta, normalised and in
/// render coordinates (renderX = wowY, renderY = wowX).
inline glm::vec3 direction(float phi, float theta) {
    constexpr float kInvPi = 0.31830987f;  // 0xa1e8d4
    const float sinPhi = cosHalfTurns(phi * kInvPi - 0.5f);
    const float cosPhi = cosHalfTurns(phi * kInvPi);
    const float sinTheta = cosHalfTurns(theta * kInvPi - 0.5f);
    const float cosTheta = cosHalfTurns(theta * kInvPi);
    const glm::vec3 wow(cosTheta * sinPhi, sinTheta * sinPhi, cosPhi);
    return core::coords::canonicalToRender(glm::normalize(wow));
}

// The tables, as 0x7eea90 and 0x7eecc0 fill them on first use, with the
// .rdata constant each value is loaded from.

/// Light phi (0xd390e4): 127 degrees (0xa41c4c) at midnight and noon,
/// 110 degrees (0xa41c48) at six.
inline constexpr std::array<Key, 4> kLightPhi{{
    {0.00f, 2.2165682f}, {0.25f, 1.9198623f}, {0.50f, 2.2165682f}, {0.75f, 1.9198623f},
}};
/// Light theta (0xd390c4): 225 degrees (0xa41c44) all day - travelling
/// south-east, so coming from the north-west.
inline constexpr std::array<Key, 4> kLightTheta{{
    {0.00f, 3.9269910f}, {0.25f, 3.9269910f}, {0.50f, 3.9269910f}, {0.75f, 3.9269910f},
}};

/// Sun sprite phi (0xd391e0): 100 degrees, ten under the horizon, from 21:30
/// to 05:30; five degrees off the zenith from 11:55 to 12:05. It crosses the
/// horizon at about 06:10 and 20:30.
inline constexpr std::array<Key, 5> kSunPhi{{
    {0.22916667f, 1.7453293f},   // 0xa41a74, 0xa41c8c
    {0.49652779f, 0.087266468f}, // 0xa41c88, 0xa41c84
    {0.50000000f, 0.087266468f},
    {0.50347221f, 0.087266468f}, // 0xa41c80
    {0.89583331f, 1.7453293f},   // 0xa41a7c
}};
/// Sun sprite theta (0xd391c8): 45 degrees, the north-west, all day.
inline constexpr std::array<Key, 3> kSunTheta{{
    {0.22916667f, 0.78539819f}, {0.5f, 0.78539819f}, {0.89583331f, 0.78539819f},
}};

/// White Lady phi (0xd391a0): 35 degrees off the zenith at midnight, 100
/// from 04:00 to 22:00. It sets at about 03:25 and rises at about 22:20.
inline constexpr std::array<Key, 5> kMoonPhi{{
    {0.0f, 0.61086524f},         // 0xa41c7c
    {0.0034722222f, 0.61086524f},// 0xa41c78
    {0.16666667f, 1.7453293f},   // 0xa41a8c
    {0.91666669f, 1.7453293f},   // 0xa41a80
    {0.99652779f, 0.61086524f},  // 0xa41c74
}};
/// White Lady theta (0xd39188): 45 degrees all night, beside the sun's path.
inline constexpr std::array<Key, 3> kMoonTheta{{
    {0.0f, 0.78539819f}, {0.16666667f, 0.78539819f}, {0.91666669f, 0.78539819f},
}};

inline constexpr float kHalfMinutesPerDay = 2880.0f;

/// the reference client's sun sprite phi (Wow.exe 1.60.1.70862, file offset
/// 0x49059b0, the same {time, radians} layout as 0xd391e0): 95 degrees (five
/// under the horizon) at 05:30 and 20:30, 25 off the zenith from 11:55 to
/// 12:05 - lower at noon (65 degrees up against 85), up at 05:58 and down at
/// 19:54. Its theta (0x4905a50) is 45 degrees all day, as stock, and its
/// light tables (0x48f7400: theta 225, phi 127 / 110 / 127 / 110) are
/// byte for byte 0xd390c4 and 0xd390e4: Enhanced moved neither the light nor
/// the sun's bearing, only how high the sun climbs (docs/re/enhanced-lighting.md
/// section 2d).
inline constexpr std::array<Key, 5> kEnhancedSunPhi{{
    {0.22916667f, 1.6580628f},   // 95 degrees
    {0.49652779f, 0.43633232f},  // 25 degrees
    {0.50000000f, 0.43633232f},
    {0.50347221f, 0.43633232f},
    {0.85416669f, 1.6580628f},
}};

}  // namespace stock_day_night

/// The way the world's directional light travels at `timeHalfMinutes`
/// (0..2880, fractional allowed), in render coordinates: always downward, so
/// the shadow it casts never lifts off the ground and never turns over at
/// night.
inline glm::vec3 stockLightDirection(float timeHalfMinutes) {
    using namespace stock_day_night;
    const float t = timeHalfMinutes / kHalfMinutesPerDay;
    return direction(sample(kLightPhi, t), sample(kLightTheta, t));
}

/// The unit direction from the eye to the sun sprite, in render coordinates.
/// Above the horizon from about 06:10 to 20:30.
inline glm::vec3 stockSunDirection(float timeHalfMinutes) {
    using namespace stock_day_night;
    const float t = timeHalfMinutes / kHalfMinutesPerDay;
    return direction(sample(kSunPhi, t), sample(kSunTheta, t));
}

/// The unit direction from the eye to the reference client's sun (kEnhancedSunPhi, the
/// stock bearing), in render coordinates.
inline glm::vec3 enhancedSunDirection(float timeHalfMinutes) {
    using namespace stock_day_night;
    const float t = timeHalfMinutes / kHalfMinutesPerDay;
    return direction(sample(kEnhancedSunPhi, t), sample(kSunTheta, t));
}

/// The unit direction from the eye to the White Lady, in render coordinates.
inline glm::vec3 stockMoonDirection(float timeHalfMinutes) {
    using namespace stock_day_night;
    const float t = timeHalfMinutes / kHalfMinutesPerDay;
    return direction(sample(kMoonPhi, t), sample(kMoonTheta, t));
}

}  // namespace wowee::rendering
