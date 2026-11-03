#pragma once

/// "Smoother other units" (docs/enhancements-plan.md E10).
///
/// The stock client puts another player where each movement packet says, at
/// the packet's time (unit-motion.md section 2: "set, not blended"), so a
/// dead-reckoning correction is a step; and it fades between animations over
/// the incoming sequence's blendTime, which a few locomotion sequences leave
/// at 0 (a hard cut). With the row on, for units other than the player's own
/// character and mount:
///
///   - A step in the drawn position that ordinary motion does not explain -
///     more than kStepThreshold beyond where the unit's own recent velocity
///     would have carried it, and less than kSnapDistance (a teleport, a
///     respawn, a long correction, which still snap) - is drawn as an offset
///     that decays to nothing with time constant kPositionTau, so the model
///     glides the last part of the way instead of jumping. The unit's real
///     position (selection, nameplates, range) is untouched; only its model
///     matrix carries the offset.
///   - A turn of more than kYawStepThreshold in one frame - the body-facing
///     ease (stepBodyFacing) already turns everything else smoothly - is
///     likewise drawn as a decaying yaw offset.
///   - A switch between two locomotion animations (stand, walk, run, sprint,
///     the shuffles, backpedal, the swims) whose incoming sequence would fade
///     in under kLocomotionBlendFloorMs fades over that long instead. Attacks,
///     casts, deaths, emotes and jumps keep their stock timing.
///
/// Off, none of this runs: the matrix is built from the position and facing as
/// set, and every blend is the sequence's own.
///
/// Pure functions, for CharacterRenderer and test_secondary_motion.

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wowee {
namespace rendering {
namespace remote_smoothing {

inline constexpr float kStepThreshold = 0.25f;      ///< yd beyond the predicted step
inline constexpr float kSnapDistance = 5.0f;        ///< yd: farther than this snaps, as stock
inline constexpr float kMaxOffset = 2.0f;           ///< yd of offset carried at most
inline constexpr float kPositionTau = 0.10f;        ///< s
inline constexpr float kYawStepThreshold = 0.6f;    ///< rad in one frame
inline constexpr float kMaxYawOffset = 3.1415927f;
inline constexpr float kYawTau = 0.08f;             ///< s
inline constexpr float kVelocitySmooth = 0.15f;     ///< s
inline constexpr float kMaxDt = 0.25f;              ///< s: a longer frame starts over
inline constexpr uint32_t kLocomotionBlendFloorMs = 150;

struct State {
    glm::vec3 lastPosition{0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 offset{0.0f};
    float lastYaw = 0.0f;
    float yawOffset = 0.0f;
    bool primed = false;
};

inline float wrapAngle(float a) {
    return std::remainder(a, 6.28318531f);
}

/// Advance by dt, given where the unit is now and its facing. Afterwards draw
/// it at position + s.offset, turned by yaw + s.yawOffset.
inline void step(State& s, const glm::vec3& position, float yaw, float dt) {
    if (!s.primed || !(dt >= 0.0f) || dt > kMaxDt) {
        // First sight, or back from a long gap (out of range, a hitch): no
        // history to judge a step by - draw it where it is.
        s = State{};
        s.primed = true;
        s.lastPosition = position;
        s.lastYaw = yaw;
        return;
    }
    if (dt == 0.0f) return;
    // Position: what the last velocity explains is motion; the rest is a step.
    const glm::vec3 moved = position - s.lastPosition;
    const glm::vec3 predicted = s.velocity * dt;
    const glm::vec3 err = moved - predicted;
    const float errLen = glm::length(err);
    if (glm::length(moved) > kSnapDistance) {
        // A teleport: snap, as stock, and forget the old motion.
        s.offset = glm::vec3(0.0f);
        s.velocity = glm::vec3(0.0f);
    } else {
        if (errLen > kStepThreshold) {
            s.offset -= err;
            const float o = glm::length(s.offset);
            if (o > kMaxOffset) s.offset *= kMaxOffset / o;
        }
        // The velocity follows what the unit really did, step or not, so a
        // unit that keeps moving is soon predicted again (a one-frame step
        // only nudges it).
        glm::vec3 v = moved / dt;
        const float vl = glm::length(v);
        if (vl > 60.0f) v *= 60.0f / vl;
        s.velocity += (v - s.velocity) * (1.0f - std::exp(-dt / kVelocitySmooth));
    }
    s.offset *= std::exp(-dt / kPositionTau);
    if (glm::dot(s.offset, s.offset) < 1e-8f) s.offset = glm::vec3(0.0f);
    s.lastPosition = position;

    // Facing.
    const float turned = wrapAngle(yaw - s.lastYaw);
    if (std::abs(turned) > kYawStepThreshold) {
        s.yawOffset = std::clamp(wrapAngle(s.yawOffset - turned), -kMaxYawOffset, kMaxYawOffset);
    }
    s.yawOffset *= std::exp(-dt / kYawTau);
    if (std::abs(s.yawOffset) < 1e-5f) s.yawOffset = 0.0f;
    s.lastYaw = yaw;
}

/// The animations whose switches get the blend floor: standing and moving
/// on the ground or in the water. `isMoving` is anim::isMovingAnim's answer
/// for the id; the jumps (37-40, 187) are left at stock - their timing is the
/// jump's.
inline bool smoothableLocomotion(uint32_t animId, bool isMoving) {
    if (animId == 0 || animId == 41) return true;  // Stand, SwimIdle
    if (animId >= 37 && animId <= 40) return false;
    if (animId == 187) return false;
    return isMoving;
}

/// The blend a switch gets: the sequence's own, or the floor when both ends
/// are locomotion and the row is on.
inline uint32_t blendMs(uint32_t stockMs, bool enabled, bool fromLocomotion, bool toLocomotion) {
    if (!enabled || !fromLocomotion || !toLocomotion) return stockMs;
    return std::max(stockMs, kLocomotionBlendFloorMs);
}

}  // namespace remote_smoothing
}  // namespace rendering
}  // namespace wowee
