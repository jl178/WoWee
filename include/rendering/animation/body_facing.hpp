#pragma once

// Stock 3.3.5a (build 12340) body yaw and look twist for units.
//
// Reverse-engineered from CGUnit_C's per-frame update (vtable function
// 0x0073DAB0, block 0x0073DD47-0x0073E26D) and its critically damped spring
// 0x00719660. The client keeps two yaws per unit: the logical facing
// (unit+0xAA0, the movement facing the server and melee arcs use) and the
// drawn body yaw (unit+0xA94). While strafing, the body springs to the facing
// plus 45 degrees (diagonal) or 90 degrees (pure strafe) toward travel, and
// the SpineLow (key bone 4) and Head (key bone 6) bones twist back toward the
// logical facing, at most 45 degrees each, so the chest and face stay on the
// facing (a target in front of the camera) while the hips and legs run
// sideways. All angles are radians, counter-clockwise positive; only
// differences matter, so any CCW-positive yaw works.

#include <algorithm>
#include <cmath>

namespace wowee::rendering::anim {

struct BodyFacingState {
    float body = 0.0f;   // unit+0xA94: yaw the whole model is drawn at
    float vel = 0.0f;    // unit+0xA98: spring velocity
    float blend = 0.0f;  // unit+0xA9C: weight of the spring against the target
    bool initialized = false;
};

struct BodyFacingInput {
    float facing = 0.0f;  // unit+0xAA0: logical facing
    // Movement flags: FORWARD 0x1, BACKWARD 0x2, STRAFE_LEFT 0x4,
    // STRAFE_RIGHT 0x8, FALLING 0x1000.
    bool forward = false;
    bool backward = false;
    bool strafeLeft = false;
    bool strafeRight = false;
    bool falling = false;
    // False where the stock gate drops out: swimming or flying (flags
    // 0x2200000), a vehicle seat, or a model with neither twist bone. The
    // body then is the facing and nothing twists.
    bool enabled = true;
    // unit+0xA38 bit 0: turning in place (turn flags 0x10/0x20 or mouse
    // turning). Only the active player sets it.
    bool turning = false;
    // The active player puts the twist on the spine first; everyone else
    // splits it evenly between spine and head (0x0073E128-0x0073E150).
    bool activePlayer = false;
    float dt = 0.0f;  // frame seconds (the 0x00B7436C+0xB14 step)
    // Seconds since unit+0xABC, the last facing update while not turning.
    // Only the active player's client stamps it; for every other unit it is
    // effectively indefinitely, so the body catches up at once.
    float catchUpElapsed = 1.0e9f;
    float turnRate = 3.14159265f;  // CMovement+0xAC (unit+0x834), rad/s
};

struct BodyFacingResult {
    float body = 0.0f;      // drawn yaw of the whole model
    float spineYaw = 0.0f;  // twist on key bone 4 (SpineLow), CCW positive
    float headYaw = 0.0f;   // twist on key bone 6 (Head), on top of the spine
    float bodyStep = 0.0f;  // what the clamp and catch-up turned the body this frame
};

namespace body_facing_detail {
constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;
constexpr float kQuarterPi = 0.785398163f;
constexpr float kHalfPi = 1.570796327f;

inline float wrapPi(float a) {  // 0x007156C0
    return std::remainder(a, kTwoPi);
}

// 0x00719660: critically damped spring (omega 20 rad/s, 0x009F281C) toward
// `target`, with the body first held within 89.98 degrees (0x00A34BBC) of the
// facing, and the result mixed with the target by the blend weight.
inline void spring(BodyFacingState& s, float facing, float target, float dt) {
    float f = facing;
    if (s.body > f + kPi) f += kTwoPi;
    else if (s.body < f - kPi) f -= kTwoPi;
    constexpr float kMaxLead = 1.5704822f;
    if (f > s.body + kMaxLead) s.body = f - kMaxLead;
    else if (f < s.body - kMaxLead) s.body = f + kMaxLead;
    s.body = wrapPi(s.body);

    float t = target;
    if (s.body > t + kPi) t += kTwoPi;
    else if (s.body < t - kPi) t -= kTwoPi;

    constexpr float kOmega = 20.0f;
    const float x = kOmega * dt;
    const float e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    const float change = s.body - t;
    const float temp = (s.vel + kOmega * change) * dt;
    const float sprung = t + (change + temp) * e;
    s.vel = (s.vel - kOmega * temp) * e;
    s.body = sprung * s.blend + (1.0f - s.blend) * t;
}
}  // namespace body_facing_detail

inline BodyFacingResult updateBodyFacing(BodyFacingState& s, const BodyFacingInput& in) {
    using namespace body_facing_detail;
    BodyFacingResult out;
    // The stock facing is kept in [0, 2pi); Wowee's camera yaw accumulates
    // without bound, and the spring only unwraps by one turn, so a facing
    // several turns away swung the body (and the legs) across the circle.
    const float facing = wrapPi(in.facing);
    if (!s.initialized || !std::isfinite(s.body) || !std::isfinite(s.vel)) {
        s.body = facing;
        s.vel = 0.0f;
        s.blend = 0.0f;
        s.initialized = true;
    }
    const float dt = std::max(in.dt, 0.0f);
    const bool strafing = in.strafeLeft || in.strafeRight;  // flags & 0xC

    if (!in.enabled) {
        s.body = facing;
        s.vel = 0.0f;
        s.blend = 0.0f;
        out.body = s.body;
        return out;  // both twist bones released
    }

    if (strafing) {
        // 0x0073DDA2: 45 degrees with forward or back held, else 90; toward
        // the left unless (flags & 6) is 0 (right, no back) or 6 (left with
        // back), so a back diagonal turns the other way and the back leads.
        float offset = (in.forward || in.backward) ? kQuarterPi : kHalfPi;
        if (in.backward == in.strafeLeft) offset = -offset;
        s.blend = 1.0f;
        const float target = wrapPi(wrapPi(facing - s.body + offset) + s.body);
        spring(s, facing, target, dt);
    } else if (in.forward || in.backward || in.falling) {  // flags & 0x1003
        if (s.blend <= 0.0f) {
            s.body = facing;
            s.vel = 0.0f;
        } else {
            s.blend -= dt * 2.5f;  // 0x009EDCE8
            if (s.blend > 1.0f) s.blend = 1.0f;
            spring(s, facing, facing, dt);
        }
    } else if (s.blend < 1.0f) {
        s.blend += dt * 2.5f;  // standing: no spring, the blend recovers
    }

    // 0x0073DEBF: whatever lies beyond 90 degrees turns the body at once;
    // when neither strafing nor turning, the body also catches up at a rate
    // that grows with the time since the last facing update.
    const float d = wrapPi(facing - s.body);
    const float ad = std::abs(d);
    if (ad < 0.001f) {  // 0x009E1134
        out.body = s.body;
        return out;
    }
    float step = ad > kHalfPi ? std::copysign(ad - kHalfPi, d) : 0.0f;
    if (!strafing && !in.turning) {
        const float catchUp = std::min(ad, std::max(in.catchUpElapsed, 0.0f) * in.turnRate * 8.0f);
        step += std::copysign(catchUp, d);
    }
    s.body = wrapPi(s.body + step);
    out.body = s.body;
    out.bodyStep = step;

    const float r = wrapPi(facing - s.body);
    const float ar = std::abs(r);
    if (ar < 1.0e-5f) return out;  // 0x009EA558
    float spine = in.activePlayer ? ar : ar * 0.5f;
    spine = std::min(spine, kQuarterPi);
    const float head = std::min(kQuarterPi, ar - spine);
    out.spineYaw = std::copysign(spine, r);
    out.headYaw = std::copysign(head, r);
    return out;
}

}  // namespace wowee::rendering::anim
