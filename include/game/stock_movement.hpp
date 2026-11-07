#pragma once

// The stock 3.3.5a (build 12340) local-player movement rules, as numbers and
// pure functions. Each one names the Wow.exe function it was read from; the
// prose is docs/re/movement-camera.md.
//
// Conventions follow the stock client's CMovement status block (the struct at
// CGUnit_C +0xD8): a speed is yards per second, a facing is radians with
// 0 = +X and counter-clockwise positive (the wire orientation), and the stock
// vertical speed is positive DOWNWARD (+0xB8). Functions that hand a vertical
// speed to Wowee code say which way it points.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wowee::game::stock_movement {

// ---- Movement flags (CMovement +0x44), the subset these rules read ----
inline constexpr uint32_t kForward = 0x1;
inline constexpr uint32_t kBackward = 0x2;
inline constexpr uint32_t kStrafeLeft = 0x4;
inline constexpr uint32_t kStrafeRight = 0x8;
inline constexpr uint32_t kTurnLeft = 0x10;
inline constexpr uint32_t kTurnRight = 0x20;
inline constexpr uint32_t kWalking = 0x100;
inline constexpr uint32_t kFalling = 0x1000;
inline constexpr uint32_t kSwimming = 0x200000;
inline constexpr uint32_t kAscending = 0x400000;
inline constexpr uint32_t kDescending = 0x800000;
inline constexpr uint32_t kFlying = 0x2000000;
inline constexpr uint32_t kSafeFall = 0x20000000;  // feather fall / slow fall

/// What counts as "moving" for the heartbeat, the 0.75 turn factor and the
/// heartbeat timer: a direction, falling, ascending or descending. Turning and
/// pitching are not in it, and neither is SWIMMING on its own (0x6f09f0,
/// 0x987770, 0x6ef860 all test 0xC0100F).
inline constexpr uint32_t kMovingMask = 0x00C0100F;

// ---- Movement flags2 (CMovement +0x48) ----
inline constexpr uint32_t kFlag2NoStrafe = 0x1;
inline constexpr uint32_t kFlag2NoJumping = 0x2;
inline constexpr uint32_t kFlag2FullSpeedTurning = 0x8;
inline constexpr uint32_t kFlag2FullSpeedPitching = 0x10;

// ---- Constants ----
/// 0xA32F74. Yards / s^2.
inline constexpr float kGravity = 19.2911053f;
/// 0xB2D9E8, the fall speed cap.
inline constexpr float kTerminalVelocity = 60.1480026f;
/// 0xB2D9EC, the cap under SAFE_FALL (Feather Fall, Slow Fall, Levitate's fall).
inline constexpr float kSafeFallTerminalVelocity = 7.0f;
/// 0xAA33DC: the jump's initial vertical speed (stored -7.955547, down positive).
inline constexpr float kJumpVelocity = 7.95554733f;
/// 0xAA33E0: the same when the jump starts from the water (0x9883F0).
inline constexpr float kWaterJumpVelocity = 9.09674835f;
/// 0x9E9EE4: keyboard turning and pitching while moving (kMovingMask) run at
/// three quarters of the rate, unless flags2 says full speed (0x987770).
inline constexpr float kTurnWhileMovingFactor = 0.75f;
/// 0x9E8D48: a diagonal (forward or back plus a strafe) is the two unit
/// directions summed and scaled by 1/sqrt(2); the speed is unchanged.
inline constexpr float kHalfSqrt2 = 0.707106769f;
/// 0xA37F0C = cos 50 deg: the steepest ground a player-controlled unit stands
/// on. 0xA37F10 = cos 80 deg for everything else (0x75D1C0, 0x716710).
inline constexpr float kWalkableNormalZPlayer = 0.642787635f;
inline constexpr float kWalkableNormalZOther = 0.173648178f;
/// 0x6E9520 / 0x6E8FC0: how high a step the ground mover climbs - the object
/// scale (at least 1) for a player-controlled unit, 2.0 (0xA4040C) otherwise.
inline constexpr float kStepHeightPlayer = 1.0f;
inline constexpr float kStepHeightOther = 2.0f;
/// 0xA37F78 = tan 50 deg: the ground query reaches dist * this + step below
/// the feet, which is what keeps a walker on a 50-degree descent (0x75FF90).
inline constexpr float kTan50 = 1.19175363f;
/// 0x6E9B70 / 0x71F0C0: a heartbeat 500 ms after the last movement packet of
/// any kind, while moving (kMovingMask).
inline constexpr uint32_t kHeartbeatIntervalMs = 500;
/// 0xA349F0 (0x71AE80): MSG_MOVE_SET_FACING / SET_PITCH go out only when the
/// facing (or, swimming or flying, the pitch) differs from the one in the last
/// movement packet by at least this many radians.
inline constexpr float kFacingResendRadians = 0.1f;
/// 0x730D10: a unit starts swimming when the liquid is deeper over its feet
/// than 0.75 (0x9E9EE4) of its collision height, and stops when it is
/// shallower than that minus 1/36 yd (0xA3F854).
inline constexpr float kSwimDepthFactor = 0.75f;
inline constexpr float kSwimExitHysteresis = 1.0f / 36.0f;
/// 0x73D4A0: MSG_MOVE_FALL_LAND is sent for a landing whose fall height
/// (0x986FB0: start z - z, plus the jump's rise) is above 1/36 yd.
inline constexpr float kFallLandMinHeight = 1.0f / 36.0f;

/// The server's default speeds (what SMSG_UPDATE_OBJECT carries for a player
/// with no modifiers); the client keeps whatever the server last sent.
struct Speeds {
    float walk = 2.5f;          // +0x90
    float run = 7.0f;           // +0x94
    float runBack = 4.5f;       // +0x98
    float swim = 4.72222f;      // +0x9C
    float swimBack = 2.5f;      // +0xA0
    float flight = 7.0f;        // +0xA4
    float flightBack = 4.5f;    // +0xA8
    float turnRate = 3.14159274f;   // +0xAC, rad/s
    float pitchRate = 3.14159274f;  // +0xB0, rad/s
};

/// CMovement::GetSpeed (0x987570): the speed a set of flags moves at.
///
/// No direction flag is zero. Flying and swimming take their own pair, the
/// backward one when BACKWARD is set and it is the slower. On the ground,
/// WALKING - or forceWalk, which is how a jump's direction started in mid-air
/// is priced - gives min(walk, run) whichever way the unit goes, so a walker
/// backs up at walk speed; otherwise BACKWARD gives min(runBack, run) and
/// anything else run. A strafe, alone or with forward, is the run speed; with
/// backward it is the backward one.
inline float selectSpeed(uint32_t flags, const Speeds& s, bool forceWalk = false) {
    if ((flags & (0x00C00000u | 0xFu)) == 0) return 0.0f;
    const bool back = (flags & kBackward) != 0;
    if (flags & kFlying) {
        return (back && s.flightBack <= s.flight) ? s.flightBack : s.flight;
    }
    if (flags & kSwimming) {
        return (back && s.swimBack <= s.swim) ? s.swimBack : s.swim;
    }
    if ((flags & kWalking) || forceWalk) {
        return std::min(s.walk, s.run);
    }
    if (back && s.runBack <= s.run) return s.runBack;
    return s.run;
}

struct Dir2 {
    float x = 0.0f;
    float y = 0.0f;
};

/// The unit direction of travel for a set of flags and a facing (0x987EF0 /
/// 0x988490): forward along the facing, backward opposite, a strafe at +90
/// (left) or -90 (right) degrees, a strafe with forward or backward the two
/// summed and scaled by kHalfSqrt2. With no direction flag it is the facing
/// itself, which is what a standing jump reports as its direction.
inline Dir2 moveDirection(uint32_t flags, float facing) {
    const float c = std::cos(facing);
    const float s = std::sin(facing);
    Dir2 fwd{c, s};
    if (flags & kBackward) fwd = {-c, -s};
    const bool fb = (flags & (kForward | kBackward)) != 0;
    const bool strafe = (flags & (kStrafeLeft | kStrafeRight)) != 0;
    if (!strafe) return fwd;
    // Left is the facing turned +90 degrees, right -90.
    const Dir2 side = (flags & kStrafeLeft) ? Dir2{-s, c} : Dir2{s, -c};
    if (!fb) return side;
    return {(fwd.x + side.x) * kHalfSqrt2, (fwd.y + side.y) * kHalfSqrt2};
}

/// CGInputControl's forward/back vote (0x5FAE70): forward key, autorun and
/// both mouse buttons each count +1, the backward key -1. Zero stops, so
/// forward and backward together stand still, and so do autorun with back.
/// Returns +1 forward, -1 backward, 0 neither.
inline int resolveForwardBack(bool forwardKey, bool backwardKey, bool autorun,
                              bool bothMouseButtons) {
    int n = (forwardKey ? 1 : 0) + (autorun ? 1 : 0) + (bothMouseButtons ? 1 : 0) -
            (backwardKey ? 1 : 0);
    return n > 0 ? 1 : (n < 0 ? -1 : 0);
}

/// The strafe vote (0x5FAFB0): the strafe keys, and with the right button held
/// the turn keys too, count +1 left / -1 right. Returns +1 left, -1 right, 0.
inline int resolveStrafe(bool strafeLeftKey, bool strafeRightKey, bool turnLeftKey,
                         bool turnRightKey, bool rightButtonSteering) {
    int n = (strafeLeftKey ? 1 : 0) - (strafeRightKey ? 1 : 0);
    if (rightButtonSteering) n += (turnLeftKey ? 1 : 0) - (turnRightKey ? 1 : 0);
    return n > 0 ? 1 : (n < 0 ? -1 : 0);
}

/// The turn vote (0x5FB0B0): the turn keys while the right button is not
/// steering. Returns +1 left, -1 right, 0.
inline int resolveTurn(bool turnLeftKey, bool turnRightKey, bool rightButtonSteering) {
    if (rightButtonSteering) return 0;
    int n = (turnLeftKey ? 1 : 0) - (turnRightKey ? 1 : 0);
    return n > 0 ? 1 : (n < 0 ? -1 : 0);
}

/// The keyboard turn rate for the flags (0x987770): the server's rate, times
/// kTurnWhileMovingFactor while moving or falling unless flags2 asks for full
/// speed turning.
inline float keyboardTurnRate(float turnRate, uint32_t flags, uint32_t flags2 = 0) {
    if ((flags & kMovingMask) != 0 && (flags2 & kFlag2FullSpeedTurning) == 0) {
        return turnRate * kTurnWhileMovingFactor;
    }
    return turnRate;
}

/// The fall speed cap for the flags (0x986E10).
inline float terminalVelocity(bool safeFall) {
    return safeFall ? kSafeFallTerminalVelocity : kTerminalVelocity;
}

/// How far a fall has dropped after t seconds, starting at vertical speed v0
/// (stock convention: positive down, so a jump starts at -kJumpVelocity)
/// (0x986F00). Gravity to the cap, then the cap.
inline float fallDistance(float t, float v0, bool safeFall) {
    const float vt = terminalVelocity(safeFall);
    const float v = std::min(v0, vt);
    if (kGravity * t + v >= vt) {
        const float tCap = (vt - v) / kGravity;
        return (0.5f * kGravity * tCap + v) * tCap + (t - tCap) * vt;
    }
    return (0.5f * kGravity * t + v) * t;
}

/// The vertical speed after t seconds (positive down), capped.
inline float fallSpeed(float t, float v0, bool safeFall) {
    return std::min(std::min(v0, terminalVelocity(safeFall)) + kGravity * t,
                    terminalVelocity(safeFall));
}

/// The time a fall from rest takes to drop d yards (0x988220).
inline float fallTimeForDistance(float d, bool safeFall) {
    const float vt = terminalVelocity(safeFall);
    const float dCap = vt * vt / (2.0f * kGravity);
    if (d >= dCap) return vt / kGravity + (d - dCap) / vt;
    if (d <= 0.0f) return 0.0f;
    return std::sqrt(2.0f * d / kGravity);
}

/// The height a jump rises: v^2 / 2g (0x986FB0 adds it to the fall height).
inline float jumpApexHeight(float jumpVelocity = kJumpVelocity) {
    return jumpVelocity * jumpVelocity / (2.0f * kGravity);
}

/// One step of the stock fall for Wowee's frame loop, exact for any dt.
///
/// upVelocity is Wowee's vertical speed, positive UP. Returns the change in
/// height over dt and leaves the new vertical speed in upVelocity. The stock
/// client evaluates the fall in closed form from its start (0x986F00), so the
/// arc does not depend on the frame rate; this matches it frame by frame.
inline float fallStep(float& upVelocity, float dt, bool safeFall) {
    const float v0 = -upVelocity;  // stock convention, down positive
    const float dz = -(fallDistance(dt, v0, safeFall));
    upVelocity = -fallSpeed(dt, v0, safeFall);
    return dz;
}

/// Whether the ground under a normal can be stood on (0x75D1C0).
inline bool isWalkableNormalZ(float normalZ, bool playerControlled = true) {
    return normalZ > (playerControlled ? kWalkableNormalZPlayer : kWalkableNormalZOther);
}

/// The height a unit steps up (0x6E9520): for a player, its object scale (not
/// below 1).
inline float stepHeight(bool playerControlled, float objectScale = 1.0f) {
    return playerControlled ? std::max(1.0f, objectScale) : kStepHeightOther;
}

/// A swimmer's step (0x760B40): none. The swim trace meets a ledge side as a
/// wall; a swimmer leaves the water onto a ledge only by the hop at the
/// surface (0x72EB80) or by wading out where the water is shallower than the
/// exit depth (0x730D10).
inline constexpr float kSwimStepHeight = 0.0f;

/// Whether standing on ground with this normal is a fall (0x75D1C0 refuses
/// the triangle, the walker starts a fall, 0x988370(0), and the air step
/// 0x7618B0 slides the box down it, FALLING, to walkable ground).
inline bool groundIsFall(float normalZ, bool playerControlled = true) {
    return !isWalkableNormalZ(normalZ, playerControlled);
}

/// Whether a heartbeat is due (0x6F09F0): moving, and 500 ms since the last
/// movement packet.
/// The ground walker's snap after a move runs off the triangle it was on
/// (0x7620F0): the box is traced straight down by `dist x 1.8494` (0xA32830)
/// for a slice of `dist` yards; walkable ground within that is landed on
/// (0x75B690 -> 0x6EC7B0), none starts a fall with v0 = 0 (0x988370(0)). On a
/// continuous walkable surface the walker moves along the triangle's plane
/// and never needs it; it is what carries the box over a convex edge (a
/// 50 degree descent needs tan 50 = 1.19 x dist). A sheer drop deeper than
/// it is a fall, however short - stock has no step-down.
inline constexpr float kWalkOffSnapPerYard = 1.84939897f;
inline float walkOffSnapReach(float dist) {
    return std::max(dist, 0.0f) * kWalkOffSnapPerYard;
}

inline bool heartbeatDue(uint32_t flags, uint32_t msSinceLastMovementPacket) {
    return (flags & kMovingMask) != 0 && msSinceLastMovementPacket >= kHeartbeatIntervalMs;
}

/// Whether a facing change is worth a MSG_MOVE_SET_FACING (0x71AE80): the
/// facing moved at least kFacingResendRadians from the one the last movement
/// packet carried. Angles in radians; the difference is taken the short way.
inline bool facingChangeWorthSending(float lastSentFacing, float facing) {
    constexpr float kTwoPi = 6.28318531f;
    float d = std::fmod(facing - lastSentFacing, kTwoPi);
    if (d > 3.14159265f) d -= kTwoPi;
    if (d < -3.14159265f) d += kTwoPi;
    return std::abs(d) >= kFacingResendRadians;
}

/// MSG_MOVE_SET_PITCH the same way (0x71AE80 case 0xDB): only swimming or
/// flying, at kFacingResendRadians from the pitch the last packet carried
/// (CGUnit +0xA54).
inline bool pitchChangeWorthSending(float lastSentPitch, float pitch, uint32_t flags) {
    if ((flags & (kSwimming | kFlying)) == 0) return false;
    return std::abs(pitch - lastSentPitch) >= kFacingResendRadians;
}

// ---- The pitch keys (PitchUp/PitchDown, CGInputControl bits 0x400/0x800) ----

/// The player's pitch limits (0x9F1FF4 / 0x9E8D88), radians.
inline constexpr float kMaxPitch = 1.57079637f;

/// Keyboard pitching (0x9877D0): the pitch rate, times kTurnWhileMovingFactor
/// while moving unless flags2 has FULL_SPEED_PITCHING.
inline float keyboardPitchRate(float pitchRate, uint32_t flags, uint32_t flags2 = 0) {
    if ((flags & kMovingMask) != 0 && (flags2 & kFlag2FullSpeedPitching) == 0) {
        return pitchRate * kTurnWhileMovingFactor;
    }
    return pitchRate;
}

/// The pitch vote (0x5FB1A0): PitchUp +1, PitchDown -1, nothing while the
/// mouse steers (input bits 0x2000001) or unless the player swims or flies
/// (0x5FBBC0: flags 0x2200000 or flags2 0x20). +1 starts PITCH_UP
/// (0x72E900(t, 1), event 0xE), -1 PITCH_DOWN (event 0xF), 0 stops (0x72E9B0,
/// event 0x10).
inline int pitchVote(bool pitchUpKey, bool pitchDownKey, bool mouseSteering,
                     bool swimmingOrFlying) {
    if (mouseSteering || !swimmingOrFlying) return 0;
    return (pitchUpKey ? 1 : 0) - (pitchDownKey ? 1 : 0);
}

/// One frame of the pitch keys: the pitch moved by vote x rate x dt and held
/// to +-pi/2.
inline float stepPitch(float pitch, int vote, float rate, float dt) {
    return std::clamp(pitch + static_cast<float>(vote) * rate * dt, -kMaxPitch, kMaxPitch);
}

/// The liquid depth over the feet at which swimming starts, and the one below
/// which it stops (0x730D10), for a unit of the given collision height.
inline float swimEnterDepth(float collisionHeight) {
    return collisionHeight * kSwimDepthFactor;
}
inline float swimExitDepth(float collisionHeight) {
    return collisionHeight * kSwimDepthFactor - kSwimExitHysteresis;
}

// ---- The collision box (docs/re/movement-camera.md section 7) ----

/// 0xA12098 / 0xA32848: the box a unit has before its model is known.
inline constexpr float kDefaultCollisionHalfWidth = 1.0f / 3.0f;
inline constexpr float kDefaultCollisionHeight = 2.02777767f;
/// 0xA12098 again, in 0x730D10: a swimmer holding ascend within this far of
/// its float line (depth - 0.75 h) jumps out of the water.
inline constexpr float kSurfaceHopWindow = 1.0f / 3.0f;

/// The axis-aligned box the stock client moves a unit as (0x75CD00): x and y
/// within halfWidth of the position, z from the feet to feet + height. The
/// step height is how far the ground mover climbs when a wall blocks it
/// (0x761B00 traces up by it, 0x6E9520 reads it).
struct Collider {
    float halfWidth = 0.30555556f;  // HumanMale: CollisionWidth 0.611 / 2
    float height = 2.03126550f;     // HumanMale: CollisionHeight
    float stepHeight = kStepHeightPlayer;
};

/// The box for a model (0x6D78C0 -> 0x6E9570), from CreatureModelData's
/// CollisionWidth / CollisionHeight (fields 14 / 15).
///
/// modelScale is the display's CreatureModelScale times the model's
/// ModelScale (0x71C110). The object scale (OBJECT_FIELD_SCALE_X) counts only
/// when it enlarges - 0x6CF350 clamps it to at least 1 - so a shrink effect
/// does not shrink the box. The width is halved; the height is not. For a
/// player-controlled unit the step height is that same clamped object scale
/// (+0xD0 = full scale / model scale): 1 yd normally, more for a unit grown
/// by an effect.
inline Collider colliderFromModel(float collisionWidth, float collisionHeight,
                                  float modelScale = 1.0f, float objectScale = 1.0f) {
    const float grow = std::max(1.0f, objectScale);
    const float s = modelScale * grow;
    Collider c;
    c.halfWidth = collisionWidth * s * 0.5f;
    c.height = collisionHeight * s;
    c.stepHeight = grow;
    return c;
}

/// The height of a mounted unit's box (0x6D7720), in the rider's model units
/// (the caller scales it as colliderFromModel does): half the rider's
/// CollisionHeight plus the mount's MountHeight (CreatureModelData field 16)
/// times the mount display's CreatureModelScale, never less than the rider on
/// foot. The width stays the rider's.
inline float mountedCollisionHeight(float riderCollisionHeight, float mountDisplayScale,
                                    float mountModelMountHeight) {
    const float h = riderCollisionHeight * 0.5f + mountDisplayScale * mountModelMountHeight;
    return std::max(h, riderCollisionHeight);
}

// ---- Water (0x730D10, 0x75F9D0, 0x760B40) ----

/// How far under the surface a swimmer's feet float. While swimming, the
/// liquid surface is a ceiling to the swim trace, and that trace runs the box
/// at 0.75 of its height against the liquid set (0x75F9D0: SWIMMING and the
/// liquid list scale +0xCC by 0x9E9EE4) - so the top of that shortened box
/// rests on the surface, exactly where swimming starts (swimEnterDepth).
inline float swimFloatDepth(float collisionHeight) {
    return collisionHeight * kSwimDepthFactor;
}

/// Whether a unit that is not swimming starts to (0x730D10): the liquid is
/// deeper over its feet than 0.75 h, it is swimmable, and the unit is not on
/// the rising half of a jump (0x986EA0).
inline bool startsSwimming(float depthOverFeet, float collisionHeight, bool swimmable,
                           bool risingInJump) {
    return swimmable && !risingInJump && depthOverFeet > swimEnterDepth(collisionHeight);
}

/// Whether a swimmer keeps swimming (0x730D10): it stops when there is no
/// liquid, the liquid is not swimmable, or the depth over its feet drops
/// below 0.75 h - 1/36. A flier (FLYING) is never stopped here.
inline bool keepsSwimming(float depthOverFeet, float collisionHeight, bool hasLiquid,
                          bool swimmable, bool flying = false) {
    if (flying) return true;
    return hasLiquid && swimmable && depthOverFeet >= swimExitDepth(collisionHeight);
}

/// Whether a swimmer holding ascend (ASCENDING) jumps out of the water this
/// frame (0x730D10 -> 0x72EB80, a JUMP event): its feet are within
/// kSurfaceHopWindow of the float line, i.e. at the surface. The jump is the
/// water one, kWaterJumpVelocity (0x9883F0), which is how a swimmer gets onto
/// a dock: it rises 2.14 yd from 0.75 h under the surface.
inline bool surfaceHopDue(bool ascending, float depthOverFeet, float collisionHeight) {
    return ascending && depthOverFeet - swimFloatDepth(collisionHeight) < kSurfaceHopWindow;
}

// ---- keys pressed in the air (0x988A20, 0x988B00, 0x98C8D0, 0x98BF80) -----
//
// While FALLING with a direction, a direction key changes nothing about the
// fall: it leaves a PENDING flag, which the landing applies (0x6EB3B0). The
// packet (START_FORWARD and the rest) is sent at once all the same, carrying
// the pending flag. A fall with no direction at all takes the first one at
// once (the standing-jump drift). The flags are MovementFlags as on the wire.
inline constexpr uint32_t kPendingStop = 0x4000;
inline constexpr uint32_t kPendingStrafeStop = 0x8000;
inline constexpr uint32_t kPendingForward = 0x10000;
inline constexpr uint32_t kPendingBackward = 0x20000;
inline constexpr uint32_t kPendingStrafeLeft = 0x40000;
inline constexpr uint32_t kPendingStrafeRight = 0x80000;
inline constexpr uint32_t kPendingMask = 0x1FC000;  // 0x6EB3B0 clears these

/// START_FORWARD / START_BACKWARD (0x988A20 with force 0).
inline uint32_t startForwardBackward(uint32_t flags, bool forward) {
    flags &= ~(kPendingStop | kPendingForward | kPendingBackward);
    if ((flags & kFalling) && (flags & 0xF)) {
        if (forward && !(flags & kForward)) flags |= kPendingForward;
        if (!forward && !(flags & kBackward)) flags |= kPendingBackward;
        return flags;
    }
    return forward ? ((flags & ~kBackward) | kForward) : ((flags & ~kForward) | kBackward);
}

/// START_STRAFE_LEFT / RIGHT (0x988B00); nothing under flags2 NO_STRAFE.
inline uint32_t startStrafe(uint32_t flags, bool left, uint32_t flags2 = 0) {
    flags &= ~(kPendingStrafeStop | kPendingStrafeLeft | kPendingStrafeRight);
    if (flags2 & kFlag2NoStrafe) return flags;
    if ((flags & kFalling) && (flags & 0xF)) {
        if (left && !(flags & kStrafeLeft)) flags |= kPendingStrafeLeft;
        if (!left && !(flags & kStrafeRight)) flags |= kPendingStrafeRight;
        return flags;
    }
    return left ? ((flags & ~kStrafeRight) | kStrafeLeft) : ((flags & ~kStrafeLeft) | kStrafeRight);
}

/// MSG_MOVE_STOP (0x98C8D0): not moving forward or back, only a pending
/// start is dropped; falling, the stop waits for the landing.
inline uint32_t stopForwardBackward(uint32_t flags) {
    if (!(flags & (kForward | kBackward))) return flags & ~(kPendingForward | kPendingBackward);
    if (flags & kFalling) return (flags & ~(kPendingForward | kPendingBackward)) | kPendingStop;
    return flags & ~(kForward | kBackward | kPendingStop);
}

/// MSG_MOVE_STOP_STRAFE (0x98BF80), the same for the strafe pair.
inline uint32_t stopStrafe(uint32_t flags) {
    if (!(flags & (kStrafeLeft | kStrafeRight)))
        return flags & ~(kPendingStrafeLeft | kPendingStrafeRight);
    if (flags & kFalling)
        return (flags & ~(kPendingStrafeLeft | kPendingStrafeRight)) | kPendingStrafeStop;
    return flags & ~(kStrafeLeft | kStrafeRight | kPendingStrafeStop);
}

/// The landing (0x988490 clears FALLING, then 0x6EB3B0): the pending stops,
/// then the pending starts, then every pending flag cleared.
inline uint32_t landWithPendingKeys(uint32_t flags, uint32_t flags2 = 0) {
    const uint32_t pending = flags;
    flags &= ~kFalling;
    if (pending & kPendingStop) flags &= ~(kForward | kBackward);
    if (pending & kPendingStrafeStop) flags &= ~(kStrafeLeft | kStrafeRight);
    if (pending & (kPendingForward | kPendingBackward))
        flags = startForwardBackward(flags, (pending & kPendingForward) != 0);
    if (pending & (kPendingStrafeLeft | kPendingStrafeRight))
        flags = startStrafe(flags, (pending & kPendingStrafeLeft) != 0, flags2);
    return flags & ~kPendingMask;
}

/// SMSG_MOVE_KNOCK_BACK on the mover (0x6EF860 event 0x22 -> 0x6E9FF0), the
/// flags it leaves: swimming or flying stopped, a ROOT made a PENDING_ROOT,
/// CAN_FLY dropped (AzerothCore gives it back on the ack to a free flier),
/// a fall from where the unit is (0x988370: not on a transport; SWIMMING,
/// ASCENDING, DESCENDING, FLYING, SPLINE_ELEVATION and, without
/// ALWAYS_ALLOW_PITCHING, the pitch keys cleared), then moving along the
/// packet's direction: FORWARD and PENDING_STOP, so the landing stops it,
/// BACKWARD cleared. The fall's z speed is the packet's (negative up), its
/// direction the packet's (x, y) and its speed the packet's.
inline uint32_t knockBackFlags(uint32_t flags, uint16_t flags2) {
    constexpr uint32_t kRoot = 0x800;
    constexpr uint32_t kPendingRoot = 0x100000;
    constexpr uint32_t kCanFly = 0x1000000;
    constexpr uint32_t kOnTransport = 0x200;
    constexpr uint32_t kFlag2AlwaysAllowPitching = 0x20;
    if (flags & kRoot) flags = (flags & ~kRoot) | kPendingRoot;
    flags &= ~kCanFly;
    if (!(flags & kOnTransport)) {
        flags = (flags & 0xF91FFFFFu) | kFalling;
        if (!(flags2 & kFlag2AlwaysAllowPitching)) flags &= ~0xC0u;
    }
    return (flags & ~kBackward) | kForward | kPendingStop;
}

/// Whether a landing sends MSG_MOVE_FALL_LAND (0x73D4A0): the fall height
/// (0x986FB0: the fall's start z less the landing z, plus v^2 / 2g of an
/// upward start speed - a jump's rise) above 1/36 yd. `apexZ` is that start
/// z plus the rise.
inline bool fallLandDue(float apexZ, float landZ) {
    return apexZ - landZ > kFallLandMinHeight;
}

}  // namespace wowee::game::stock_movement
