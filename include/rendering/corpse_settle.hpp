#pragma once

/// "Corpses settle to the slope" (docs/enhancements-plan.md E8).
///
/// The stock client draws a dead unit as it draws a live one: upright on its
/// position, turned only by its facing. The death animation lays the body down
/// in the model's own flat plane, so on a hillside half of it hangs in the air
/// on the downhill side and the uphill half sinks into the ground. With the row
/// on, the body is tilted onto the ground under it as the fall ends: the slope
/// is measured once, from the floor at four points of the body's footprint
/// (terrain, building floors and doodad floors, as the selection circle reads
/// them), and the tilt eases in over the last moment of the fall. No ragdoll:
/// the animation plays as authored; only the whole body's plane turns.
///
/// Pure functions, for CharacterRenderer and test_phase2_enhancements.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace wowee {
namespace rendering {
namespace corpse_settle {

/// The steepest the body is tilted. A cliff edge or a stair's nose under one
/// end of the footprint would ask for far more; past this it reads as the
/// body sliding, not lying.
inline constexpr float kMaxTiltDegrees = 25.0f;
/// How long the tilt takes to ease in, ending as the death animation ends.
inline constexpr float kSettleSeconds = 0.3f;
/// How far a corpse may move (a server correction, a knockback as it dies)
/// before its slope is measured again.
inline constexpr float kResampleDistance = 0.25f;

/// Half the footprint's length and width for a body of this standing height:
/// a lying body is about as long as it stood tall. Held to 0.4-4 yd so a
/// rabbit still reads the ground and a dragon does not span a valley.
inline float footprintHalfExtent(float standingHeight) {
    return std::clamp(standingHeight * 0.45f, 0.4f, 4.0f);
}

/// The floor heights at the four ends of the footprint, along the unit's
/// facing (front, back) and across it (left, right), and at its centre.
/// A missing one had no floor near the body.
struct FootprintHeights {
    std::optional<float> front, back, left, right, centre;
};

/// The ground normal under the body, from the footprint's heights. `yaw` is
/// the unit's facing (radians, the model's +X turned by it), `half` the
/// footprint's half extent. An end with no floor is replaced by the centre
/// (half the baseline); an axis with neither end is level. The tilt is held
/// to kMaxTiltDegrees. Level ground gives exactly (0, 0, 1).
inline glm::vec3 groundNormal(const FootprintHeights& h, float yaw, float half) {
    if (half <= 0.0f) return {0.0f, 0.0f, 1.0f};
    auto slope = [&](const std::optional<float>& plus, const std::optional<float>& minus) {
        if (plus && minus) return (*plus - *minus) / (2.0f * half);
        if (plus && h.centre) return (*plus - *h.centre) / half;
        if (minus && h.centre) return (*h.centre - *minus) / half;
        return 0.0f;
    };
    const float gx = slope(h.front, h.back);   // rise per yard along the facing
    const float gy = slope(h.left, h.right);   // rise per yard to the left
    if (gx == 0.0f && gy == 0.0f) return {0.0f, 0.0f, 1.0f};
    const glm::vec2 ax(std::cos(yaw), std::sin(yaw));
    const glm::vec2 ay(-std::sin(yaw), std::cos(yaw));
    const glm::vec2 grad = gx * ax + gy * ay;
    glm::vec3 n = glm::normalize(glm::vec3(-grad.x, -grad.y, 1.0f));
    const float maxTilt = glm::radians(kMaxTiltDegrees);
    const float tilt = std::acos(std::clamp(n.z, -1.0f, 1.0f));
    if (tilt > maxTilt) {
        const glm::vec2 dir = glm::normalize(glm::vec2(n.x, n.y));
        n = glm::vec3(dir * std::sin(maxTilt), std::cos(maxTilt));
    }
    return n;
}

/// The rotation taking straight up to `n`, `amount` of the way (0-1, eased).
/// Identity for level ground or a zero amount.
inline glm::quat tilt(const glm::vec3& n, float amount) {
    if (amount <= 0.0f || n.z >= 0.99999f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    const glm::vec3 axis = glm::cross(up, n);
    const float len = glm::length(axis);
    if (len < 1e-6f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    const float angle = std::atan2(len, glm::dot(up, n));
    const float t = std::clamp(amount, 0.0f, 1.0f);
    const float eased = t * t * (3.0f - 2.0f * t);
    return glm::angleAxis(angle * eased, axis / len);
}

/// Whether the tilt should be easing in: the body is down, or will be within
/// kSettleSeconds (the death animation's time left, in ms; negative when the
/// unit is not dying - then it is lying already if `dead`).
inline bool settling(bool dead, float deathMsLeft) {
    if (!dead) return false;
    return deathMsLeft < 0.0f || deathMsLeft <= kSettleSeconds * 1000.0f;
}

/// One step of the settle amount: toward 1 while settling, back to 0 when
/// the unit rises, over kSettleSeconds either way.
inline float step(float amount, bool settle, float dt) {
    const float d = dt / kSettleSeconds;
    return settle ? std::min(1.0f, amount + d) : std::max(0.0f, amount - d);
}

}  // namespace corpse_settle
}  // namespace rendering
}  // namespace wowee
