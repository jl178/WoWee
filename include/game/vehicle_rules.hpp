#pragma once

// Stock 3.3.5a (build 12340) vehicles and passengers, reverse engineered in
// docs/re/mounts-taxi-vehicles.md section 4. The records are Vehicle.dbc and
// VehicleSeat.dbc as the client reads them (Vehicle_C 0x757FA0, the seat
// table at Vehicle +0x18); the rules are pure and tested in
// tests/test_vehicle_rules.cpp.

#include <cmath>
#include <cstdint>
#include <functional>
#include <glm/glm.hpp>

namespace wowee::game::vehicle {

/// VehicleSeat.dbc (58 fields) - the columns the client reads.
struct SeatRecord {
    uint32_t id = 0;
    uint32_t flags = 0;               // 1
    int32_t attachmentId = -1;        // 2: a seat attachment id, see seatM2Attachment
    glm::vec3 attachmentOffset{0.0f}; // 3..5
    float enterPreDelay = 0.0f;       // 6  (+0x18)
    float enterSpeed = 0.0f;          // 7
    float enterGravity = 0.0f;        // 8
    float enterMinDuration = 0.0f;    // 9
    float enterMaxDuration = 0.0f;    // 10
    float enterMinArcHeight = 0.0f;   // 11
    float enterMaxArcHeight = 0.0f;   // 12
    int32_t enterAnimStart = -1;      // 13
    int32_t enterAnimLoop = -1;       // 14
    int32_t rideAnimStart = -1;       // 15
    int32_t rideAnimLoop = -1;        // 16
    int32_t rideUpperAnimStart = -1;  // 17
    int32_t rideUpperAnimLoop = -1;   // 18
    float exitPreDelay = 0.0f;        // 19 (+0x4c)
    float exitSpeed = 0.0f;           // 20
    float exitGravity = 0.0f;         // 21
    float exitMinDuration = 0.0f;     // 22
    float exitMaxDuration = 0.0f;     // 23
    float exitMinArcHeight = 0.0f;    // 24
    float exitMaxArcHeight = 0.0f;    // 25
    int32_t exitAnimStart = -1;       // 26
    int32_t exitAnimLoop = -1;        // 27
    float passengerYaw = 0.0f;        // 29
    float passengerPitch = 0.0f;      // 30
    float passengerRoll = 0.0f;       // 31
    int32_t passengerAttachmentId = -1; // 32: a raw M2 attachment on the passenger
    int32_t vehicleRideAnimLoop = -1; // 35
    uint32_t vehicleAbilityDisplay = 0; // 41 (+0xa4): 1 = the vehicle's spells on the bonus bar
    uint32_t enterUISoundId = 0;      // 42
    uint32_t exitUISoundId = 0;       // 43
    uint32_t uiSkin = 0;              // 44: 0 "Natural", 1 "Mechanical"
    uint32_t flagsB = 0;              // 45
    glm::vec3 cameraOffset{0.0f};     // 50..52 (+0xc8)
    float cameraPosChaseRate = 0.0f;  // 53 (+0xd4)
    float cameraFacingChaseRate = 0.0f; // 54
};

/// Vehicle.dbc (40 fields) - what the client reads of it.
struct VehicleRecord {
    uint32_t id = 0;
    uint32_t flags = 0;               // 1
    float turnSpeed = 0.0f;           // 2
    float pitchSpeed = 0.0f;          // 3
    float pitchMin = 0.0f;            // 4 (+0x10)
    float pitchMax = 0.0f;            // 5 (+0x14)
    uint32_t seatIds[8] = {};         // 6..13
    float mouseLookOffsetPitch = 0.0f; // 14 (+0x38)
    float cameraFadeDistScalarMin = 0.0f; // 15 (+0x3c)
    float cameraFadeDistScalarMax = 0.0f; // 16 (+0x40)
    float cameraPitchOffset = 0.0f;   // 17
    float facingLimitRight = 0.0f;    // 18 (+0x48)
    float facingLimitLeft = 0.0f;     // 19 (+0x4c)
    float cameraYawOffset = 0.0f;     // 33
    uint32_t uiIndicatorId = 0;       // 36 (+0x90)
};

// Vehicle.flags the client tests (TrinityCore's names).
inline constexpr uint32_t kVehicleNoStrafe = 0x1;            // MOVEMENTFLAG2_NO_STRAFE
inline constexpr uint32_t kVehicleNoJumping = 0x2;           // MOVEMENTFLAG2_NO_JUMPING
inline constexpr uint32_t kVehicleAllowPitching = 0x10;      // aim = the mover's pitch (0x6FCD60)
inline constexpr uint32_t kVehicleCustomPitch = 0x40;        // pitch limited to PitchMin/Max (0x5FB3A0)
inline constexpr uint32_t kVehicleAdjustAimAngle = 0x400;    // IsVehicleAimAngleAdjustable
inline constexpr uint32_t kVehicleAdjustAimPower = 0x800;    // IsVehicleAimPowerAdjustable
inline constexpr uint32_t kVehicleFixedPosition = 0x200000;  // facing held to the limits (0x6EAA50)
inline constexpr uint32_t kVehicleCameraFade = 0x80000;      // the model fades as the camera nears (0x6077xx)

/// How opaque a vehicle with kVehicleCameraFade draws with the camera
/// `distance` from it (Camera.cpp, 0x607000 area): clear within
/// CameraFadeDistScalarMin x its size, whole beyond Max x its size, eased in
/// between.
inline float cameraFadeAlpha(const VehicleRecord& v, float modelSize, float distance) {
    const float lo = v.cameraFadeDistScalarMin * modelSize;
    const float hi = v.cameraFadeDistScalarMax * modelSize;
    if (distance >= hi) return 1.0f;
    if (distance <= lo || hi - lo < 1.0e-4f) return 0.0f;
    const float t = (distance - lo) / (hi - lo);
    return 0.5f - std::cos(t * 3.1415927f) * 0.5f;
}

// VehicleSeat.flags the client tests.
inline constexpr uint32_t kSeatHasEnterAnim = 0x1;          // 0x747B20
inline constexpr uint32_t kSeatHasRideAnim = 0x2;           // 0x747B20
inline constexpr uint32_t kSeatHasUpperAnim = 0x4;          // 0x747BD0
inline constexpr uint32_t kSeatControlSeat = 0x800;         // UnitInVehicleControlSeat
inline constexpr uint32_t kSeatVehicleRideAnim = 0x20000;   // 0x7489C0
inline constexpr uint32_t kSeatUsesVehicleControls = 0x800000;  // IsUsingVehicleControls
inline constexpr uint32_t kSeatCanExit = 0x2000000;         // CanExitVehicle
inline constexpr uint32_t kSeatCanSwitch = 0x4000000;       // CanSwitchVehicleSeats
inline constexpr uint32_t kSeatHasVehicleUI = 0x20000000;   // UnitHasVehicleUI
// VehicleSeat.flagsB.
inline constexpr uint32_t kSeatBCameraOnPassenger = 0x100;  // 0x759200
inline constexpr uint32_t kSeatBCanEject = 0x20;            // UnitVehicleSeatInfo canEject

/// The M2 attachment a seat's AttachmentID names (the table at 0xA2D3F0,
/// read by 0x7493B0): seat ids are not M2 attachment ids. -1 for none
/// (an id of 22 or more, or negative): the seat is then relative to the
/// vehicle's base.
inline int seatM2Attachment(int32_t seatAttachmentId) {
    static constexpr int kMap[22] = {20, 34, 19, 21, 22, 17, 23, 24, 25, 15, 16,
                                     37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 0};
    if (seatAttachmentId < 0 || seatAttachmentId >= 22) return -1;
    return kMap[seatAttachmentId];
}

/// A passenger's state (VehiclePassenger_C +0x14).
enum class PassengerState : uint8_t { None = 0, EnterDelay = 1, Entering = 2, Seated = 3,
                                      ExitDelay = 4, Exiting = 5 };

/// No animation (AnimationData's count, 0x1FA).
inline constexpr uint32_t kNoAnim = 506;

/// What a passenger's whole body plays (0x747B20): seated on a seat with
/// flag 0x2, RideAnimStart once (if it names one) then RideAnimLoop;
/// entering with 0x1 the enter pair. Otherwise kNoAnim: the ordinary stand
/// chain runs - there is no fallback to Mount (91).
struct PassengerAnim {
    uint32_t start = kNoAnim;   // one-shot first, or kNoAnim
    uint32_t loop = kNoAnim;    // then this, or kNoAnim
};
inline PassengerAnim passengerAnim(const SeatRecord& seat, PassengerState state) {
    auto id = [](int32_t a) { return (a >= 0 && a < static_cast<int32_t>(kNoAnim))
                                         ? static_cast<uint32_t>(a) : kNoAnim; };
    PassengerAnim p;
    if (state == PassengerState::Seated && (seat.flags & kSeatHasRideAnim)) {
        p.start = id(seat.rideAnimStart);
        p.loop = id(seat.rideAnimLoop);
    } else if ((state == PassengerState::EnterDelay || state == PassengerState::Entering) &&
               (seat.flags & kSeatHasEnterAnim)) {
        p.start = id(seat.enterAnimStart);
        p.loop = id(seat.enterAnimLoop);
    } else if (state == PassengerState::ExitDelay || state == PassengerState::Exiting) {
        p.start = id(seat.exitAnimStart);
        p.loop = id(seat.exitAnimLoop);
    }
    return p;
}

/// The vehicle's own loop while this seat is taken (0x7489C0): seat flag
/// 0x20000 and a VehicleRideAnimLoop below 506; kNoAnim otherwise.
inline uint32_t vehicleRideAnim(const SeatRecord& seat) {
    if (!(seat.flags & kSeatVehicleRideAnim)) return kNoAnim;
    if (seat.vehicleRideAnimLoop < 0 || seat.vehicleRideAnimLoop >= static_cast<int32_t>(kNoAnim))
        return kNoAnim;
    return static_cast<uint32_t>(seat.vehicleRideAnimLoop);
}

/// The aim angle a vehicle allows (0x5FB3A0): with CUSTOM_PITCH (0x40)
/// Vehicle.PitchMin..PitchMax, otherwise -pi/2..pi/2 (0x9F1FF4 / 0x9E8D88).
inline glm::vec2 aimLimits(const VehicleRecord* v) {
    constexpr float kHalfPi = 1.5707963705062866f;
    if (v && (v->flags & kVehicleCustomPitch)) return {v->pitchMin, v->pitchMax};
    return {-kHalfPi, kHalfPi};
}
inline float clampAim(const VehicleRecord* v, float pitch) {
    const glm::vec2 l = aimLimits(v);
    return pitch < l.x ? l.x : (pitch > l.y ? l.y : pitch);
}
/// VehicleAimGetNormAngle (0x5F9E60) / VEHICLE_ANGLE_UPDATE's first value
/// (0x5FA910): the pitch across the limits, 0 when they are one value.
inline float aimNormAngle(const VehicleRecord* v, float pitch) {
    const glm::vec2 l = aimLimits(v);
    const float span = l.y - l.x;
    if (std::fabs(span) <= 1.0e-4f) return 0.0f;
    return (pitch - l.x) / span;
}
/// VehicleAimRequestNormAngle (0x5FB8C0): the pitch at a fraction of the limits.
inline float aimFromNorm(const VehicleRecord* v, float norm) {
    const glm::vec2 l = aimLimits(v);
    return l.x + (l.y - l.x) * norm;
}

/// Whether the seat hands the bonus action bar to the vehicle's spells
/// (0x5D4AD0: VehicleSeat.VehicleAbilityDisplay == 1; GetBonusBarOffset 5).
inline bool seatShowsVehicleBar(const SeatRecord& seat) { return seat.vehicleAbilityDisplay == 1; }

// ---- Boarding and leaving (VehiclePassenger_C 0x74A200 / 0x74A7F0 / 0x747D70) ----

/// One arc into or out of a seat: how long, how high, how eased.
struct SeatArc {
    float preDelay = 0.0f;   // seconds before the arc starts (Enter/ExitPreDelay, <= 10)
    float duration = 0.0f;   // seconds the arc takes
    float gravity = 0.0f;    // the parabola's gravity
    float liftScale = 1.0f;  // so the top of the arc sits in [MinArcHeight, MaxArcHeight]
    uint32_t easeIn = 0;     // the seat flag that squares the progress
    uint32_t easeOut = 0;    // ... and the one that eases it out
    uint32_t flags = 0;      // the seat's flags
};
/// The arc for a passenger `distance` from where it goes (0x74A200):
/// distance over Enter/ExitSpeed (less the vehicle's speed toward it when
/// boarding), clamped to Min..MaxDuration (MaxDuration capped at 10, 1.5
/// when unset); the arc's top g*T^2/8 held to Min..MaxArcHeight.
inline SeatArc seatArc(const SeatRecord& seat, bool entering, float distance,
                       float closingSpeed = 0.0f) {
    SeatArc a;
    const float speed = entering ? seat.enterSpeed : seat.exitSpeed;
    const float minD = entering ? seat.enterMinDuration : seat.exitMinDuration;
    const float maxDRaw = entering ? seat.enterMaxDuration : seat.exitMaxDuration;
    const float minH = entering ? seat.enterMinArcHeight : seat.exitMinArcHeight;
    const float maxH = entering ? seat.enterMaxArcHeight : seat.exitMaxArcHeight;
    const float pre = entering ? seat.enterPreDelay : seat.exitPreDelay;
    a.preDelay = pre < 10.0f ? (pre > 0.0f ? pre : 0.0f) : 10.0f;
    a.gravity = entering ? seat.enterGravity : seat.exitGravity;
    const float maxD = maxDRaw >= 1.0e-4f ? (maxDRaw < 10.0f ? maxDRaw : 10.0f) : 1.5f;
    float t = 0.0f;
    if (distance > 1.0e-4f) {
        const float rel = speed - closingSpeed;
        t = rel > 1.0e-4f ? distance / rel : maxD;
        if (t < minD) t = minD;
        if (t > maxD) t = maxD;
    }
    a.duration = t;
    if (std::fabs(a.gravity) < 1.0e-4f && (minH > 0.0f || maxH < 0.0f)) a.gravity = 0.1f;
    if (t > 1.0e-4f) {
        const float top = a.gravity * t * t * 0.125f;
        if (top < minH) a.liftScale = minH / top;
        else if (maxH < top) a.liftScale = maxH / top;
    }
    a.easeIn = entering ? 0x20u : 0x80u;
    a.easeOut = entering ? 0x40u : 0x100u;
    a.flags = seat.flags;
    return a;
}
/// Progress along the arc at `elapsed` seconds after it started (0x747D70).
inline float seatArcProgress(const SeatArc& a, float elapsed) {
    if (a.duration < 1.0e-4f) return 1.0f;
    float p = elapsed / a.duration;
    p = p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
    const bool in = (a.flags & a.easeIn) != 0, out = (a.flags & a.easeOut) != 0;
    if (in && out) return 0.5f - std::cos(p * 3.1415927f) * 0.5f;
    if (in) return p * p;
    if (out) return 1.0f - (1.0f - p) * (1.0f - p);
    return p;
}
/// Height above the straight line at progress `p` (0x74A7F0):
/// g/2 * t * (T - t) at t = p*T, times the arc's scale.
inline float seatArcLift(const SeatArc& a, float p) {
    if (std::fabs(a.gravity) <= 1.0e-4f) return 0.0f;
    const float t = p * a.duration;
    const float half = a.gravity * t * 0.5f;
    return (half * a.duration - half * t) * a.liftScale;
}

// ---- Missile trajectories (SpellMissile.dbc; Spell_C 0x6FCD60) ----

/// SpellMissile.dbc (15 fields), what the trajectory reads.
struct MissileRecord {
    uint32_t id = 0;
    uint32_t flags = 0;                       // 1: 0x1 = the cast carries a trajectory
    float defaultPitchMin = 0, defaultPitchMax = 0;   // 2, 3
    float defaultSpeedMin = 0, defaultSpeedMax = 0;   // 4, 5
    float randomizeFacingMin = 0, randomizeFacingMax = 0;  // 6, 7
    float randomizePitchMin = 0, randomizePitchMax = 0;    // 8, 9
    float randomizeSpeedMin = 0, randomizeSpeedMax = 0;    // 10, 11
    float gravity = 0;                        // 12
    float maxDuration = 0;                    // 13
    float collisionRadius = 0;                // 14
};
/// The elevation and speed a vehicle's trajectory spell goes out with
/// (0x6FCD60, before the randomization): with ALLOW_PITCHING the vehicle's
/// aim (held to its limits with CUSTOM_PITCH), else the middle of the
/// missile's default pitch; with ADJUST_AIM_POWER the missile's speed range
/// at the power (VehicleAimSetNormPower), else its middle.
inline glm::vec2 missileElevationSpeed(const MissileRecord& m, const VehicleRecord* v,
                                       float aimPitch, float power) {
    float elev = (m.defaultPitchMin + m.defaultPitchMax) * 0.5f;
    if (v && (v->flags & kVehicleAllowPitching)) {
        elev = aimPitch;
        if (v->flags & kVehicleCustomPitch) elev = clampAim(v, elev);
    }
    float speed = (m.defaultSpeedMin + m.defaultSpeedMax) * 0.5f;
    if (v && (v->flags & kVehicleAdjustAimPower))
        speed = m.defaultSpeedMin + (m.defaultSpeedMax - m.defaultSpeedMin) * power;
    return {elev, speed};
}
/// Where the missile lands (0x6FCD60's flight): 0.1 s steps from `src`
/// along `facing` at `elevation`, `speed`, falling by Gravity * 0.01 a
/// step (half of it on the first), at most MaxDuration steps (200 when
/// unset or more), until `hits(from, to, &fraction)` reports the segment
/// meets the world. Server axes (z up). Returns the impact point; `seconds`
/// the flight time.
inline glm::vec3 simulateMissile(const MissileRecord& m, const glm::vec3& src, float facing,
                                 float elevation, float speed,
                                 const std::function<bool(const glm::vec3&, const glm::vec3&, float&)>& hits,
                                 float* seconds = nullptr) {
    const float step = speed * 0.1f;
    glm::vec3 vel(std::cos(facing) * std::cos(elevation) * step,
                  std::sin(facing) * std::cos(elevation) * step,
                  std::sin(elevation) * step);
    const float g = m.gravity * 0.01f;
    vel.z -= g * 0.5f;
    int maxSteps = 200;
    if (m.maxDuration > 1.0e-4f) {
        const int d = static_cast<int>(m.maxDuration);
        if (d <= 200) maxSteps = d;
    }
    glm::vec3 from = src;
    for (int i = 0; i < maxSteps; ++i) {
        const glm::vec3 to = from + vel;
        float frac = 1.0f;
        if (hits && hits(from, to, frac)) {
            if (seconds) *seconds = (static_cast<float>(i) + frac) * 0.1f;
            return from + (to - from) * frac;
        }
        from = to;
        vel.z -= g;
    }
    if (seconds) *seconds = static_cast<float>(maxSteps) * 0.1f;
    return from;
}

/// UnitVehicleSkin's answer for a seat's uiSkin.
inline const char* uiSkinName(uint32_t uiSkin) {
    return uiSkin == 1 ? "Mechanical" : "Natural";
}

}  // namespace wowee::game::vehicle
