// include/game/unit_motion.hpp
//
// How the stock 3.3.5a client (build 12340) moves every unit it does not
// control: creatures and players alike, from SMSG_MONSTER_MOVE splines and from
// other players' MSG_MOVE_* packets. Everything here is read from Wow.exe; the
// addresses are in docs/re/unit-motion.md, which is the long form of these
// comments.
//
// All of it works in server coordinates (x north, y west, z up, facing
// counter-clockwise from +x) - or, for a unit on a transport, in that
// transport's local frame, which uses the same axes. The callers convert to
// Wowee's canonical space at the edge.
//
// Pure: no rendering, no networking, no clocks. Times are milliseconds on
// whatever monotonic clock the caller keeps, so the tests can drive it.
#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace wowee::game::motion {

// ── Constants (Wow.exe 12340) ─────────────────────────────────────────────

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTwoPi = 6.28318530717958647692f;

/// Gravity, yards/s^2 (0xA32F74; half of it is 0xAA33AC).
inline constexpr float kGravity = 19.29110527f;
/// Terminal fall speed (0xB2D9E8) and the safe-fall one (0xB2D9EC).
inline constexpr float kTerminalVelocity = 60.14800262f;
inline constexpr float kSafeFallTerminalVelocity = 7.0f;
/// The z speed a jump starts with, on land and from the water (0xAA33DC,
/// 0xAA33E0). Negative is up, as on the wire.
inline constexpr float kJumpVelocity = -7.955547333f;
inline constexpr float kSwimJumpVelocity = -9.096748352f;

/// SMSG_MONSTER_MOVE speed cap (0x73C8E0, the same as AzerothCore's
/// MoveSplineInit::Launch): a ground spline is never faster than
/// max(28, 4 * run speed), a smooth (Catmull-Rom or flying) one than 50.
inline constexpr float kSplineGroundSpeedFloor = 28.0f;   // 0xA34EE8
inline constexpr float kSplineRunSpeedFactor = 4.0f;      // 0x9E8D2C
inline constexpr float kSplineSmoothSpeedCap = 50.0f;     // 0x9F22EC
/// A path no longer than this is no path: the unit is just placed (0xA33B7C).
inline constexpr float kMinSplineLength = 1.0f / 6.0f;
/// Two points closer than 1/36 yard are the same point (0xA349B0).
inline constexpr float kSamePointDistSq = 0.0007716049f;
/// The trim of a ground path keeps the server's first node when the unit is
/// within a yard of it on the map (0x9E1130).
inline constexpr float kTrimKeepXYDistSq = 1.0f;
/// A spline only turns the unit when its direction has this much XY length
/// (0xAA33E4).
inline constexpr float kFacingMinXYSq = 0.001849f;
/// A finished spline puts the unit on its end point only from 3 yards off
/// (0x6E9470, 0x9E2FF8).
inline constexpr float kEndSnapDistSq = 9.0f;
/// Moving units turn at three quarters of their turn rate unless
/// MOVEMENTFLAG2_FULL_SPEED_TURNING (0x987770, 0x9E9EE4).
inline constexpr float kMovingTurnFactor = 0.75f;
/// Locomotion animation thresholds (0x717050): above this speed Sprint.
inline constexpr float kSprintSpeed = 11.0f;              // 0xA34B20
/// A remote unit's last packet keeps it going at full speed this long, then
/// it slows to a stop over the next kStallRampMs (0x6E9F50).
inline constexpr uint32_t kStallFullSpeedMs = 500;
inline constexpr uint32_t kStallRampMs = 1000;
/// The bounds on how far a remote movement packet is held or brought
/// forward (0x6EB730): [-500, +1000] ms.
inline constexpr int32_t kScheduleEarliestMs = -500;
inline constexpr int32_t kScheduleLatestMs = 1000;
/// The body turns toward the movement facing at turn rate * 8, and is never
/// more than a quarter turn behind it (0x73DAB0: 0x9E2EC8, 0x9E8D88).
inline constexpr float kBodyTurnRateFactor = 8.0f;
inline constexpr float kBodyMaxLag = kPi * 0.5f;

// ── Flags ─────────────────────────────────────────────────────────────────

/// MoveSplineFlag as the 3.3.5 client reads it.
namespace SplineFlags {
inline constexpr uint32_t AnimationMask = 0x000000FF;
inline constexpr uint32_t Done = 0x00000100;
inline constexpr uint32_t Falling = 0x00000200;
inline constexpr uint32_t NoSpline = 0x00000400;
inline constexpr uint32_t Parabolic = 0x00000800;
inline constexpr uint32_t WalkMode = 0x00001000;
inline constexpr uint32_t Flying = 0x00002000;
inline constexpr uint32_t OrientationFixed = 0x00004000;
inline constexpr uint32_t FinalPoint = 0x00008000;
inline constexpr uint32_t FinalTarget = 0x00010000;
inline constexpr uint32_t FinalAngle = 0x00020000;
inline constexpr uint32_t CatmullRom = 0x00040000;
inline constexpr uint32_t Cyclic = 0x00080000;
inline constexpr uint32_t EnterCycle = 0x00100000;
inline constexpr uint32_t Animation = 0x00200000;
inline constexpr uint32_t Frozen = 0x00400000;
inline constexpr uint32_t TransportEnter = 0x00800000;
inline constexpr uint32_t TransportExit = 0x01000000;
inline constexpr uint32_t OrientationInversed = 0x08000000;
/// Uncompressed nodes and a Catmull-Rom curve (0x73C8E0, 0x98C770).
inline constexpr uint32_t Smooth = CatmullRom | Flying;
}  // namespace SplineFlags

/// MovementFlags (3.3.5 wire values) the motion reads.
namespace MoveFlags {
inline constexpr uint32_t Forward = 0x00000001;
inline constexpr uint32_t Backward = 0x00000002;
inline constexpr uint32_t StrafeLeft = 0x00000004;
inline constexpr uint32_t StrafeRight = 0x00000008;
inline constexpr uint32_t TurnLeft = 0x00000010;
inline constexpr uint32_t TurnRight = 0x00000020;
inline constexpr uint32_t PitchUp = 0x00000040;
inline constexpr uint32_t PitchDown = 0x00000080;
inline constexpr uint32_t Walking = 0x00000100;
inline constexpr uint32_t OnTransport = 0x00000200;
inline constexpr uint32_t Root = 0x00000800;
inline constexpr uint32_t Falling = 0x00001000;
inline constexpr uint32_t FallingFar = 0x00002000;
inline constexpr uint32_t Swimming = 0x00200000;
inline constexpr uint32_t Ascending = 0x00400000;
inline constexpr uint32_t Descending = 0x00800000;
inline constexpr uint32_t CanFly = 0x01000000;
inline constexpr uint32_t Flying = 0x02000000;
inline constexpr uint32_t SplineElevation = 0x04000000;
inline constexpr uint32_t SplineEnabled = 0x08000000;
inline constexpr uint32_t FeatherFall = 0x20000000;
inline constexpr uint32_t Hover = 0x40000000;
inline constexpr uint32_t Directions = 0x0000000F;
/// "Under way" for the physics (0xC0100F): a direction, falling, ascending
/// or descending.
inline constexpr uint32_t Translating = 0x00C0100F;
/// Anything that changes the state over time (0xC010FF): the above and
/// turning and pitching.
inline constexpr uint32_t Moving = 0x00C010FF;
}  // namespace MoveFlags

namespace MoveFlags2 {
inline constexpr uint16_t FullSpeedTurning = 0x0008;
inline constexpr uint16_t FullSpeedPitching = 0x0010;
}  // namespace MoveFlags2

/// The nine movement speeds a unit carries (UPDATE_OBJECT's living block,
/// then the speed opcodes). Defaults are the server's base speeds.
struct MoveSpeeds {
    float walk = 2.5f;
    float run = 7.0f;
    float runBack = 4.5f;
    float swim = 4.722222f;
    float swimBack = 2.5f;
    float flight = 7.0f;
    float flightBack = 4.5f;
    float turnRate = kPi;
    float pitchRate = kPi;
};

/// The speed the physics moves at for these flags (0x987570).
[[nodiscard]] float selectSpeed(const MoveSpeeds& s, uint32_t moveFlags);

/// Distance fallen after `seconds` from a start z speed of `startVelocity`
/// (negative up), capped at terminal velocity (0x986F00).
[[nodiscard]] float fallDistance(float seconds, float startVelocity, bool safeFall);

/// The ground locomotion the stock client picks for a moving unit on its feet
/// (0x717050): Sprint (143) above 11 yd/s, Run (5) above twice the walk
/// speed, otherwise Walk (4). Backward is Walkbackwards (13) at any speed.
[[nodiscard]] uint32_t groundLocomotionAnim(float speed, float walkSpeed, bool backward);

/// Animation playback rate for a moving unit (0x7385C0): its speed over the
/// sequence's MoveSpeed. 1 when the sequence has none.
[[nodiscard]] float locomotionPlaybackRate(float unitSpeed, float sequenceMoveSpeed);

/// One frame of the body turning toward the movement facing (0x73DAB0):
/// past a quarter turn behind, the excess is taken at once; the rest at
/// turnRate * 8 rad/s. Returns the new body facing in [-pi, pi).
[[nodiscard]] float stepBodyFacing(float body, float target, float turnRate, float dtSeconds);

// ── The curve (CMovementSpline, vtable 0x9E2F28) ──────────────────────────

/// The client's spline object: control points with one virtual point before
/// and after the real path (two after for a cyclic one), evaluated either
/// linearly or as a Catmull-Rom curve, at a parameter uniform in arc length
/// over the whole path (0x4C3BD0, 0x4C3FD0, 0x4C43B0).
class SplineCurve {
public:
    SplineCurve() = default;
    /// `points` includes the virtual points. Fewer than four is no curve.
    void init(std::vector<glm::vec3> points, bool catmullRom);

    struct Sample {
        glm::vec3 position{0.0f};
        glm::vec3 direction{0.0f};  // unit length, or zero
    };
    /// t in [0, 1] over the whole length.
    [[nodiscard]] Sample evaluate(float t) const;
    [[nodiscard]] float length() const { return length_; }
    [[nodiscard]] bool catmullRom() const { return catmull_; }
    [[nodiscard]] const std::vector<glm::vec3>& points() const { return points_; }
    [[nodiscard]] size_t segmentCount() const {
        return points_.size() >= 4 ? points_.size() - 3 : 0;
    }
    [[nodiscard]] bool valid() const { return segmentCount() > 0; }

private:
    [[nodiscard]] glm::vec3 position(size_t segment, float u) const;
    [[nodiscard]] float segmentLength(size_t segment) const;

    std::vector<glm::vec3> points_;
    std::vector<float> segmentLengths_;
    float length_ = 0.0f;
    bool catmull_ = false;
};

// ── SMSG_MONSTER_MOVE (0x73C8E0) ─────────────────────────────────────────

enum class FinalFacing : uint8_t { None, Spot, Target, Angle };

/// What a monster move says, in the frame it was sent in.
struct MonsterMoveSpec {
    glm::vec3 start{0.0f};   // the packet's position
    uint8_t moveType = 0;    // 0 normal, 1 stop, 2 spot, 3 target, 4 angle
    glm::vec3 facingSpot{0.0f};
    uint64_t facingTarget = 0;
    float facingAngle = 0.0f;
    uint32_t splineFlags = 0;
    uint32_t durationMs = 0;
    bool hasAnimation = false;
    uint8_t animationType = 0;
    uint32_t animationStartMs = 0;
    bool hasParabolic = false;
    float verticalAcceleration = 0.0f;
    uint32_t parabolicStartMs = 0;
    /// The nodes after the start, in path order; the last is the destination.
    std::vector<glm::vec3> points;
};

/// What the unit is doing now, as the client sees it.
struct MoverSnapshot {
    glm::vec3 position{0.0f};
    float facing = 0.0f;
    float runSpeed = 7.0f;
};

/// A running spline (the MoveSpline at CMovement+0xBC).
class UnitSpline {
public:
    SplineCurve curve;
    uint32_t flags = 0;
    uint32_t startMs = 0;        // +0x24
    uint32_t durationMs = 0;     // +0x2C, the client's own (0x73C8E0)
    float durationMod = 1.0f;    // +0x204
    float durationModNext = 1.0f;// +0x208
    float verticalAcceleration = 0.0f;  // +0x20C
    uint32_t effectStartMs = 0;  // +0x210
    glm::vec3 finalPoint{0.0f};  // +0x1F8
    FinalFacing finalFacing = FinalFacing::None;
    glm::vec3 facingSpot{0.0f};
    uint64_t facingTarget = 0;
    float facingAngle = 0.0f;
    float fallStartZ = 0.0f;     // CMovement+0x84
    bool effectStarted = false;  // MOVEMENTFLAG2 0x80 / 0x100 in the client
    bool finished = false;

    struct Frame {
        glm::vec3 position{0.0f};
        std::optional<float> facing;  // the direction of travel, when it turns the unit
        float pitch = 0.0f;
        bool hasPitch = false;
        bool finished = false;        // reached the end this frame or before
        bool jumpStarted = false;     // a parabolic effect began: JumpStart
        bool animationStarted = false;// an Animation effect began: the tier
        uint8_t animationTier = 0;
        bool landed = false;          // a falling spline met its end height
    };

    /// 0x98CA00, plus the effect edges 0x6E9C30 reports. `moveFlags` are the
    /// unit's (swimming and flying ones pitch instead of banking).
    [[nodiscard]] Frame update(uint32_t nowMs, uint32_t moveFlags);

    /// The speed over the ground the spline implies (0x987570).
    [[nodiscard]] float speed() const;
    [[nodiscard]] int32_t elapsedMs(uint32_t nowMs) const {
        return static_cast<int32_t>(nowMs - startMs);
    }
};

/// What became of a monster move.
struct MonsterMovePlan {
    enum class Kind : uint8_t {
        Stop,      // moveType 1 near the unit: stays where it is
        Place,     // no path worth walking: placed at `position`
        Spline,    // a spline to run
    } kind = Kind::Place;
    glm::vec3 position{0.0f};
    /// Facing to take now (a path too short to walk still turns the unit).
    FinalFacing immediateFacing = FinalFacing::None;
    float immediateAngle = 0.0f;
    glm::vec3 immediateSpot{0.0f};
    uint64_t immediateTarget = 0;
    std::optional<UnitSpline> spline;
};

/// The client's reading of SMSG_MONSTER_MOVE (0x73C8E0): where the path
/// starts (the unit, not the packet), how its ends are padded, how long it
/// takes. `nowMs` is when it arrived.
[[nodiscard]] MonsterMovePlan planMonsterMove(const MonsterMoveSpec& spec,
                                              const MoverSnapshot& mover,
                                              uint32_t nowMs);

/// The trim of a ground path to the unit's position (0x7180C0): drops the
/// nodes the unit has passed and starts the path where it is. `points` is
/// the path without virtual points; returns the new one (possibly empty).
[[nodiscard]] std::vector<glm::vec3> trimPathToMover(const std::vector<glm::vec3>& points,
                                                      const glm::vec3& mover);

/// A spline that UPDATE_OBJECT carries for a unit already on its way
/// (create block, MOVEMENTFLAG_SPLINE_ENABLED): the server's own padded
/// points, its time passed and duration.
struct CreateSplineSpec {
    uint32_t splineFlags = 0;
    uint32_t timePassedMs = 0;
    uint32_t durationMs = 0;
    float durationMod = 1.0f;
    float durationModNext = 1.0f;
    float verticalAcceleration = 0.0f;
    uint32_t effectStartMs = 0;
    std::vector<glm::vec3> nodes;   // padded, as written
    uint8_t mode = 0;               // 0 linear, 1 Catmull-Rom
    glm::vec3 endPoint{0.0f};
    FinalFacing finalFacing = FinalFacing::None;
    glm::vec3 facingSpot{0.0f};
    uint64_t facingTarget = 0;
    float facingAngle = 0.0f;
};

[[nodiscard]] std::optional<UnitSpline> splineFromCreate(const CreateSplineSpec& spec,
                                                         uint32_t nowMs);

// ── Other players' MSG_MOVE_* (CMovement, 0x6EB730 .. 0x987B50) ─────────

/// One movement packet's state.
struct MovementSample {
    uint32_t time = 0;          // the sender's clock
    uint32_t flags = 0;
    uint16_t flags2 = 0;
    glm::vec3 position{0.0f};   // world, or transport-local when on one
    float facing = 0.0f;
    float pitch = 0.0f;
    uint32_t fallTimeMs = 0;
    float jumpVelocity = 0.0f;  // negative up
    float jumpSin = 0.0f;
    float jumpCos = 0.0f;
    float jumpXYSpeed = 0.0f;
    float splineElevation = 0.0f;
};

/// The movement state a remote unit carries between packets: where it was
/// at its last start, how long ago, and what it is doing.
struct MoveState {
    uint32_t flags = 0;
    uint16_t flags2 = 0;
    glm::vec3 position{0.0f};       // +0x10
    float facing = 0.0f;            // +0x20
    float pitch = 0.0f;             // +0x24
    glm::vec3 startPosition{0.0f};  // +0x4C
    float startFacing = 0.0f;       // +0x58
    float startPitch = 0.0f;        // +0x5C
    uint32_t elapsedMs = 0;         // +0x60
    uint32_t fallTimeMs = 0;        // +0x80
    float fallStartZ = 0.0f;        // +0x84
    float jumpVelocity = 0.0f;      // +0xB8
    float jumpCos = 1.0f, jumpSin = 0.0f;  // +0x70/+0x74 while falling
    float jumpXYSpeed = 0.0f;
    float splineElevation = 0.0f;   // +0x88

    /// Take a packet's state as the new start (0x988920, 0x988990).
    void applySample(const MovementSample& s);
    /// Re-base: the current state becomes the start (0x9881D0).
    void rebase();
    [[nodiscard]] bool translating() const { return (flags & MoveFlags::Translating) != 0; }
    [[nodiscard]] bool moving() const { return (flags & MoveFlags::Moving) != 0; }
    [[nodiscard]] bool falling() const { return (flags & MoveFlags::Falling) != 0; }
};

/// Where the state's motion has taken it `ms` after its start (0x987B50 and
/// the fall it hands to collision): position offset from startPosition, and
/// the facing and pitch then.
struct Displacement {
    glm::vec3 offset{0.0f};
    float facing = 0.0f;
    float pitch = 0.0f;
};
[[nodiscard]] Displacement displacementAt(const MoveState& st, const MoveSpeeds& speeds,
                                          uint32_t ms);

/// A remote player's mover: packets are applied at the time their sender
/// spaced them (a jitter buffer), and between them the unit runs on its
/// flags and speeds, slowing to a stop when packets stop coming.
class RemoteMover {
public:
    MoveSpeeds speeds;

    /// A packet arrived at `localNowMs`.
    void receive(const MovementSample& s, uint32_t localNowMs);
    /// Advance to `localNowMs`: apply what is due, move.
    void update(uint32_t localNowMs);
    /// Forget everything (teleport, transport change): the next packet is
    /// applied as it arrives.
    void reset() {
        init_ = false;
        queue_.clear();
        jitterHold_ = 0;
        jitter_.fill(0);
        jitterIndex_ = 0;
        sincePacketMs_ = 0;
        appliedCount_ = 0;
    }
    /// Put the unit somewhere without a packet (a spline ended, a teleport).
    void place(const glm::vec3& position, float facing);

    [[nodiscard]] const MoveState& state() const { return state_; }
    [[nodiscard]] bool initialised() const { return init_; }
    [[nodiscard]] size_t pendingCount() const { return queue_.size(); }
    /// The sample last applied (for flags and animation hints).
    [[nodiscard]] const MovementSample& lastApplied() const { return lastApplied_; }
    [[nodiscard]] uint32_t appliedCount() const { return appliedCount_; }
    [[nodiscard]] int32_t jitterHoldMs() const { return jitterHold_; }

private:
    struct Pending {
        MovementSample sample;
        uint32_t applyAtMs = 0;
    };
    void apply(const MovementSample& s, uint32_t applyAtMs, uint32_t localNowMs);
    void advanceTo(uint32_t localNowMs);
    [[nodiscard]] int32_t pushJitter(int32_t dtPacket, int32_t dtLocal);

    MoveState state_;
    MovementSample lastApplied_;
    std::deque<Pending> queue_;
    bool init_ = false;
    uint32_t lastPacketTime_ = 0;   // +0xC4
    uint32_t lastApplyMs_ = 0;      // +0xC0
    uint32_t stateClockMs_ = 0;     // local time the state was last advanced to
    uint32_t sincePacketMs_ = 0;    // +0x134
    int32_t jitterHold_ = 0;        // +0x12C
    std::array<int16_t, 32> jitter_{};  // +0xE8
    uint32_t jitterIndex_ = 0;      // +0x128
    uint32_t appliedCount_ = 0;  // since the last reset
};

/// The movement time the stall rule leaves after `sincePacketMs` without a
/// packet (0x6E9F50).
[[nodiscard]] uint32_t stalledElapsed(uint32_t elapsedMs, uint32_t sincePacketMs);

/// Angle in [0, 2pi).
[[nodiscard]] float wrapTwoPi(float a);

}  // namespace wowee::game::motion
