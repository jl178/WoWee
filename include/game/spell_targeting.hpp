#pragma once

// Spells cast at a point on the ground - Blizzard, Flamestrike, Rain of Fire,
// Volley, Hurricane, Death and Decay, Flare, Mass Dispel, the engineering
// bombs - as Wow.exe 3.3.5a (12340) asks for the point. See
// docs/re/spell-cast-pipeline.md section 15 for the functions.
//
// The rules only: no game state, so the tests can hold them to the stock
// numbers.

#include <algorithm>
#include <cstdint>

namespace wowee::game::spell_targeting {

// SpellCastTargets flags (Spell.dbc Targets and the CMSG_CAST_SPELL block).
constexpr uint32_t kTargetFlagUnit = 0x00000002;
constexpr uint32_t kTargetFlagSourceLocation = 0x00000020;
constexpr uint32_t kTargetFlagDestLocation = 0x00000040;
/// The flags that are a point on the ground (0x7FD750 tests these two).
constexpr uint32_t kTargetFlagsLocation = kTargetFlagSourceLocation | kTargetFlagDestLocation;

/// What a cast still needs once it has been pressed (0x80C790): Spell.dbc
/// Targets, adjusted by the first effect's implicit target A. Only the first
/// effect is asked. The returned mask is what the spell waits for before it
/// goes out; a spell left with nothing waits for nothing.
inline uint32_t pendingTargetMask(uint32_t spellTargets, uint32_t implicitTargetA0) {
    uint32_t m = spellTargets;
    switch (implicitTargetA0) {
        case 1:  m &= ~0x400u; break;    // UNIT_CASTER
        case 5:  m &= ~0x8000u; break;   // UNIT_PET
        case 6: case 53: m |= 0x80u; break;    // UNIT_TARGET_ENEMY, DEST_TARGET_ENEMY
        case 21: case 45: m |= 0x100u; break;  // UNIT_TARGET_ALLY, UNIT_CHAINHEAL_ALLY
        case 23: m |= 0x800u; break;     // GAMEOBJECT_TARGET
        case 25: case 63: case 64: case 65: case 66: case 67: case 68: case 69:
        case 70: case 71: case 74: case 75:
            m |= kTargetFlagUnit; break;  // UNIT_TARGET_ANY, DEST_TARGET_*
        case 26: m |= 0x4000u; break;    // GAMEOBJECT_ITEM_TARGET
        case 35: m |= 0x8u; break;       // UNIT_TARGET_PARTY
        case 57: case 61: m |= 0x4u; break;    // UNIT_TARGET_RAID, ..._RAID_CLASS
        case 90: m |= 0x10000u; break;   // UNIT_TARGET_MINIPET
        default: break;
    }
    return m;
}

/// Whether pressing the spell puts the cursor in targeting mode for a point:
/// it waits on a location (0x80C790 leaves 0x20 or 0x40 standing).
inline bool needsGroundTarget(uint32_t spellTargets, uint32_t implicitTargetA0) {
    return (pendingTargetMask(spellTargets, implicitTargetA0) & kTargetFlagsLocation) != 0;
}

/// The ground circle (WorldFrame 0x4F8A40): Spell-Shadow-Acceptable.blp in
/// range, Spell-Shadow-Unacceptable.blp out of it (Interface/SpellShadow),
/// projected over the box centre +/- R in x and y and +/- 2 in z.
constexpr float kDecalHalfHeight = 2.0f;           // 0xA4040C
constexpr float kDecalDefaultHalfWidth = 1.3888888f;  // 0x9F98DC, when R is 0
constexpr float kDecalMaxHalfWidth = 20.0f;        // 0x9F281C

/// The spell's area radius as the targeting circle sizes it (0x8019C0):
/// the larger of effects 0 and 1, each SpellRadius Radius + RadiusPerLevel x
/// the caster's level (RadiusMax is not read), then capped at 20 (0x4F66C0).
/// Spell mods (SPELLMOD_RADIUS) are applied by the caller.
inline float decalHalfWidth(float radius0, float perLevel0, float radius1, float perLevel1,
                            uint32_t casterLevel) {
    const float lvl = static_cast<float>(casterLevel);
    const float r = std::max(radius0 + lvl * perLevel0, radius1 + lvl * perLevel1);
    return std::min(r, kDecalMaxHalfWidth);
}

/// Where the cursor's point stands against the spell's range (0x803EE0):
/// within [min, max] the circle is the spell's and green, the cursor Cast;
/// beyond four times the range there is no circle at all; between, the small
/// red one and UnableCast. The distance is 3D, from the caster.
enum class RangeState : uint8_t { InRange = 0, OutOfRange = 1, Hidden = 2 };
inline RangeState destRangeState(float distSq, float minRange, float maxRange) {
    if (minRange * minRange <= distSq && distSq <= maxRange * maxRange) return RangeState::InRange;
    if (maxRange * maxRange * 16.0f < distSq) return RangeState::Hidden;
    return RangeState::OutOfRange;
}

}  // namespace wowee::game::spell_targeting
