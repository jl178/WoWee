#pragma once

// Locomotion animation for units this client does not control (other players
// and NPCs), driven by the same LocomotionFSM as the local player from what
// the server says about them: their movement flags and their motion.
//
// 3.3.5a movement flags read here: FORWARD 0x1, BACKWARD 0x2, STRAFE_LEFT
// 0x4, STRAFE_RIGHT 0x8, TURN_LEFT 0x10, TURN_RIGHT 0x20, WALKING 0x100,
// FALLING 0x1000 (jumping or falling), FALLINGFAR 0x2000, SWIMMING 0x200000,
// FLYING 0x2000000.

#include "rendering/animation/anim_capability_set.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/locomotion_fsm.hpp"
#include "rendering/animation/weapon_anim_select.hpp"

#include <cstdint>

namespace wowee::rendering::anim {

struct UnitLocomotion {
    /// The character instance the capabilities were probed on. Another
    /// instance (a display swap, a respawn) is another model: probe again.
    uint32_t instanceId = 0;
    AnimCapabilitySet caps;
    LocomotionFSM fsm;
    /// The FSM output last acted on. Stand to begin with: whoever spawned the
    /// model gave it its idle (or a state emote, or a sit) already.
    uint32_t lastRequest = STAND;
    bool lastLoop = true;
    /// Something else drove the model for a while (flight, a mount, death):
    /// play the FSM's answer once it is back, even if it is the same.
    bool needsReplay = false;
    /// MSG_MOVE_JUMP seen. The jump lasts until the unit has been airborne
    /// (FALLING) and landed again; one that never leaves the ground lapses.
    bool jumpPending = false;
    bool airborneSeen = false;
    float jumpAge = 0.0f;
    /// Radians the drawn body turned last frame (CCW positive): standing
    /// still, a step either way past 1e-5 is a turn in place (the shuffle).
    float lastBodyStep = 0.0f;
    /// The animation whose MoveSpeed was last looked up, and that speed: a
    /// moving unit's animation plays at its speed over it (0x7385C0).
    uint32_t rateAnimId = 0;
    float rateMoveSpeed = 0.0f;
};

/// What the sync loop knows about a unit this frame.
struct UnitLocomotionFrame {
    uint32_t moveFlags = 0;   // the movement flags above (direction, turn, falling)
    bool moving = false;      // translating: a spline, a dead-reckoned sample, or position change
    bool walking = false;     // WALKING: Walk rather than Run
    bool swimming = false;
    bool idleFree = true;     // the model is idling (Stand, Ready, StealthStand, a shuffle)
    float dt = 0.0f;
};

/// The LocomotionFSM input for a remote unit, and the bookkeeping of its
/// jump. Turning in place follows the stock gate (CGUnit_C 0x0073E2C0):
/// none of forward, back, strafe or falling, on the ground, out of water; then
/// TURN_LEFT or a counter-clockwise body step plays ShuffleLeft, TURN_RIGHT
/// or a clockwise step ShuffleRight.
inline LocomotionFSM::Input unitLocomotionInput(UnitLocomotion& u, const UnitLocomotionFrame& f) {
    constexpr uint32_t kForward = 0x1, kBackward = 0x2, kStrafeLeft = 0x4, kStrafeRight = 0x8;
    constexpr uint32_t kTurnLeft = 0x10, kTurnRight = 0x20, kFalling = 0x1000;
    constexpr float kStepEps = 1.0e-5f;

    const bool grounded = (f.moveFlags & kFalling) == 0 || f.swimming;
    if (u.jumpPending) {
        u.jumpAge += f.dt;
        if (!grounded) u.airborneSeen = true;
        if ((grounded && u.airborneSeen) || (!u.airborneSeen && u.jumpAge > 0.5f)) {
            u.jumpPending = false;
            u.airborneSeen = false;
            u.jumpAge = 0.0f;
        }
    }

    LocomotionFSM::Input in;
    in.moving = f.moving;
    in.movingBackward = f.moving && (f.moveFlags & kBackward) != 0;
    in.movingForward = f.moving && !in.movingBackward;
    in.strafeLeft = f.moving && (f.moveFlags & kStrafeLeft) != 0;
    in.strafeRight = f.moving && (f.moveFlags & kStrafeRight) != 0;
    in.sprinting = f.moving && !f.walking;
    in.grounded = grounded;
    in.jumping = u.jumpPending && !grounded;
    in.swimming = f.swimming;
    in.deltaTime = f.dt;

    const bool still = !f.moving && grounded && !f.swimming &&
                       (f.moveFlags & (kForward | kBackward | kStrafeLeft | kStrafeRight)) == 0;
    if (still && f.idleFree) {
        in.turnLeft = (f.moveFlags & kTurnLeft) != 0 || u.lastBodyStep > kStepEps;
        in.turnRight = !in.turnLeft &&
                       ((f.moveFlags & kTurnRight) != 0 || u.lastBodyStep < -kStepEps);
    }
    return in;
}

/// Whether `id` is an idle the locomotion may take over (and a turn may
/// shuffle out of): Stand, a Ready stance, StealthStand, or a shuffle.
inline bool isLocomotionIdle(uint32_t id) {
    return id == STAND || id == STEALTH_STAND || id == SHUFFLE_LEFT || id == SHUFFLE_RIGHT ||
           isReadyStance(id);
}

}  // namespace wowee::rendering::anim
