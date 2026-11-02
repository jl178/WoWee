#pragma once

// Stock 3.3.5a (build 12340) rules for mounted units, reverse engineered in
// docs/re/mounts-taxi-vehicles.md. Pure functions, unit tested in
// tests/test_mount_rules.cpp.

#include "rendering/animation/animation_ids.hpp"

#include <cstdint>

namespace wowee::rendering::anim {

/// The ground pace of a moving unit (0x717050): above 11 yd/s (0xA34B20)
/// Sprint, above twice the unit's walk speed Run, otherwise Walk. A mount at
/// 60% (run 11.2) and 100% (14) therefore sprints; an unknown speed runs.
enum class MountPace : uint8_t { Walk, Run, Sprint };
inline MountPace mountGroundPace(float speed, float walkSpeed) {
    if (speed < 0.0f) return MountPace::Run;
    if (speed > 11.0f) return MountPace::Sprint;
    if (speed > walkSpeed + walkSpeed) return MountPace::Run;
    return MountPace::Walk;
}

/// Standing still on a Flying spline: Hover (0x71C6C0 counts a Flying spline
/// as hovering), which a model without it plays as Stand.
inline constexpr uint32_t mountIdleOnSpline() { return HOVER; }

/// The spline flags that make a unit move in Fly (135) (0x4F5290): Flying
/// (0x2000) without No_Spline (0x400). AzerothCore's taxi sets Flying.
inline constexpr uint32_t kSplineFlying = 0x2000;
inline constexpr uint32_t kSplineNoSpline = 0x400;
inline bool onFlyingSpline(uint32_t splineFlags) {
    return (splineFlags & kSplineFlying) != 0 && (splineFlags & kSplineNoSpline) == 0;
}

/// The rider's animation (CGUnit +0xB7C, played by 0x73D5D0): Mount (91),
/// unless an aura of type 78 (SPELL_AURA_MOUNTED) has a spell visual whose
/// StateKit names an animation (0x724820: AnimID >= 0). In 3.3.5a data that
/// is the flying carpets (120), the Mechano-hog and Mekgineer's Chopper
/// (484, ReclinedMount) and Summon Mouth Tentacle (127).
inline uint32_t mountRiderAnim(int32_t stateKitAnimId) {
    return stateKitAnimId >= 0 ? static_cast<uint32_t>(stateKitAnimId) : MOUNT;
}

/// SPELL_AURA_MOUNTED.
inline constexpr uint32_t kAuraMounted = 78;

}  // namespace wowee::rendering::anim
