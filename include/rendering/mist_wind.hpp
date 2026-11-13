#pragma once

/// The wind the mist drifts on, zone by zone (volumetric_fog_inject.comp,
/// docs/re/enhanced-lighting.md 5c, fog drift).
///
/// 3.3.5 has no wind of its own to read: the foliage sways on one constant
/// direction (m2.vert's kWindDir) and the weather falls on a fixed drift, so
/// the table is ours, by the same zone classes the Enhanced lighting groups
/// its zones in (assets/enhanced_lighting_params.json). Dust blows across
/// Westfall and the Barrens off the sea, mist creeps through Duskwood and
/// Ashenvale, and a swamp's barely moves but keeps turning over.
///
/// The noise is moved by two offsets - the banks on the wind, a finer wisp
/// octave on more of it plus a slow churn of its own - so it evolves rather
/// than slides as one sheet. Both are accumulated (an eased wind, a zone
/// change, never jump the field) and wrapped on the noise's own period, so a
/// session of any length never loses float precision nor ever snaps.
///
/// Pure, for VolumetricFog and test_volumetric_fog_math.

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wowee {
namespace rendering {
namespace mist_wind {

/// One zone class's air: where it blows toward (world x/y, normalised on
/// use), how fast (yd/s), how fast the wisps churn on their own (yd/s, a
/// rise and turn), and the wisps' share of the density (0-1).
struct Wind {
    glm::vec2 toward{0.80f, 0.60f};
    float speed = 1.0f;
    float churn = 0.3f;
    float wisps = 0.2f;
};

enum class ZoneKind : uint8_t {
    Default,
    TemperateForest,
    Plains,
    DarkForest,
    TwilightForest,
    Desert,
    Snow,
    Swamp,
    Coastal,
};

/// AreaTable zone ids, as enhanced_lighting_params.json groups them.
inline ZoneKind kindOf(uint32_t zoneId) {
    switch (zoneId) {
        case 12: case 44: case 38: case 267: case 47: case 406: case 357: case 1519:
            return ZoneKind::TemperateForest;
        case 40: case 45: case 215: case 1638: case 17: case 14: case 1637:
            return ZoneKind::Plains;
        case 10: case 130: case 85: case 361: case 28: case 139: case 41: case 331:
            return ZoneKind::DarkForest;
        case 141: case 1657: case 493:
            return ZoneKind::TwilightForest;
        case 440: case 1377: case 405: case 400: case 3: case 51: case 46: case 4:
            return ZoneKind::Desert;
        case 1: case 618: case 36:
            return ZoneKind::Snow;
        case 8: case 15: case 11: case 490:
            return ZoneKind::Swamp;
        case 16: case 33: case 148:
            return ZoneKind::Coastal;
        default:
            return ZoneKind::Default;
    }
}

/// World x is north, y is west. "East" is -y.
inline Wind windOf(ZoneKind k) {
    switch (k) {
        // Elwynn, Redridge, Hillsbrad: a light breeze with the foliage.
        case ZoneKind::TemperateForest: return {{0.80f, 0.60f}, 0.8f, 0.25f, 0.18f};
        // Westfall, the Barrens, Mulgore: dust carried east off the sea and
        // across the grass, the fastest and the least churned.
        case ZoneKind::Plains:          return {{0.25f, -1.0f}, 2.6f, 0.20f, 0.22f};
        // Duskwood, Ashenvale, Silverpine: mist creeping, turning over.
        case ZoneKind::DarkForest:      return {{-0.70f, -0.70f}, 0.55f, 0.45f, 0.26f};
        // Teldrassil, Moonglade: still, with a slow stir under the boughs.
        case ZoneKind::TwilightForest:  return {{0.30f, 0.95f}, 0.45f, 0.35f, 0.20f};
        // Tanaris, the Badlands, Burning Steppes: hot wind, fast.
        case ZoneKind::Desert:          return {{-0.90f, -0.45f}, 3.0f, 0.30f, 0.22f};
        // Dun Morogh, Winterspring, Alterac: a cold, steady wind.
        case ZoneKind::Snow:            return {{0.95f, -0.30f}, 2.0f, 0.20f, 0.18f};
        // Wetlands, Dustwallow, the Swamp of Sorrows: barely moving air,
        // churning most.
        case ZoneKind::Swamp:           return {{0.60f, 0.80f}, 0.30f, 0.50f, 0.28f};
        // Azshara, Stranglethorn, Darkshore: onshore, brisk.
        case ZoneKind::Coastal:         return {{-0.60f, 0.80f}, 1.8f, 0.25f, 0.20f};
        case ZoneKind::Default:
        default:                        return {};
    }
}

/// The banks' size (yd), and the noise lattice's period in its own cells,
/// so an offset wraps without a seam. The wisps are a fifth the size.
inline constexpr float kBankYards = 40.0f;
inline constexpr float kPeriodCells = 256.0f;
inline constexpr float kWispScale = 5.0f;

/// Weather stirs the air: rain, snow and sand at full intensity blow up to
/// this much faster.
inline constexpr float kStormBoost = 1.5f;

/// The wind velocity the mist is moved by (yd/s, x, y and a slight lift).
inline glm::vec3 velocity(const Wind& w, float storm) {
    glm::vec2 d = w.toward;
    const float len = glm::length(d);
    d = len > 1e-4f ? d / len : glm::vec2(0.8f, 0.6f);
    const float s = w.speed * (1.0f + kStormBoost * glm::clamp(storm, 0.0f, 1.0f));
    return glm::vec3(d * s, 0.12f * std::min(s, 1.0f));
}

/// Eases one wind toward another, per component, over about `tau` seconds,
/// so walking into the next zone turns the air rather than snapping it.
inline Wind ease(const Wind& from, const Wind& to, float dt, float tau) {
    const float k = tau > 0.0f ? 1.0f - std::exp(-std::max(dt, 0.0f) / tau) : 1.0f;
    Wind w;
    w.toward = glm::mix(from.toward, to.toward, k);
    w.speed = glm::mix(from.speed, to.speed, k);
    w.churn = glm::mix(from.churn, to.churn, k);
    w.wisps = glm::mix(from.wisps, to.wisps, k);
    return w;
}

/// Wraps a noise-space offset onto [0, period).
inline glm::vec3 wrap(const glm::vec3& o, float period) {
    return o - period * glm::floor(o / period);
}

/// Advances the two noise-space offsets (in noise cells, not yards) by one
/// frame: the banks by the wind, the wisps by 1.6 of it - a finer octave
/// outruns a coarse one, as in air - and by the churn, a slow rise and turn.
struct Offsets {
    glm::vec3 banks{0.0f};
    glm::vec3 wisps{0.0f};
};
inline Offsets advance(Offsets o, const glm::vec3& vel, float churn, float time, float dt) {
    dt = glm::clamp(dt, 0.0f, 0.25f);
    o.banks = wrap(o.banks - vel * (dt / kBankYards), kPeriodCells);
    const float a = time * 0.05f;
    const glm::vec3 turn(std::cos(a), std::sin(a), 0.6f);
    const glm::vec3 wv = vel * 1.6f + turn * churn;
    o.wisps = wrap(o.wisps - wv * (dt * kWispScale / kBankYards), kPeriodCells);
    return o;
}

}  // namespace mist_wind
}  // namespace rendering
}  // namespace wowee
