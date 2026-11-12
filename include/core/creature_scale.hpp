#pragma once

/**
 * creature_scale.hpp - how big the stock client draws a creature (3.3.5a build
 * 12340, docs/re/characters.md §5).
 *
 * Two numbers, kept on the unit:
 *
 *  - The unit scale (`unit+0xB3C`), from 0x71C110 by way of 0x722AE0, worked
 *    out again whenever the display or the creature's cache record changes
 *    (0x72CBB0; 0x72CE3B when the record arrives with a family):
 *      display = extraRaceScale x CreatureDisplayInfo.CreatureModelScale
 *                x CreatureModelData.ModelScale, and <= 0 means 1.
 *    extraRaceScale (0x71C050) is, for a display with a
 *    CreatureDisplayInfoExtra, the CreatureModelScale of the display ChrRaces
 *    gives that race and sex (a tauren NPC is 1.35 like a tauren player);
 *    1 otherwise. Then, when the creature's cache record names a
 *    CreatureFamily row (`unit+0x964` -> `+0x14`), the family scale
 *      MinScale + (MaxScale - MinScale) x t,
 *      t = clamp(level - MinScaleLevel, 0, span) / span,
 *      span = MaxScaleLevel - MinScaleLevel (t = 0 when span is 0)
 *    replaces it when it is larger, and always for a pet - a unit with a
 *    UNIT_FIELD_PETNUMBER (`fields+0x114`).
 *  - The render scale (0x71C0E0) = unit scale x `unit+0x9C` (1 unless an
 *    effect shrinks the model) x OBJECT_FIELD_SCALE_X, and x the mount
 *    display's scale while mounted.
 *
 * So a tamed beast is sized by its family and its level, never by the display
 * it was tamed with: a Frostworg is a 3.0 display, and as a level 80 hunter's
 * wolf it is drawn at the wolf family's 1.0 - a third of the wild one. The
 * server's object scale still multiplies on top (AzerothCore sends its own
 * family scale there for hunter pets).
 */

#include <algorithm>
#include <cstdint>

namespace wowee {
namespace core {

/// The scale columns of one CreatureFamily.dbc row (ID, MinScale,
/// MinScaleLevel, MaxScale, MaxScaleLevel in every build).
struct CreatureFamilyScale {
    float minScale = 0.0f;
    int32_t minScaleLevel = 0;
    float maxScale = 0.0f;
    int32_t maxScaleLevel = 0;
};

/// The family's scale at a level (the tail of 0x71C110, and the stable
/// preview's 0x4E3CD0).
inline float creatureFamilyScaleAt(const CreatureFamilyScale& f, uint32_t level) {
    const int32_t span = f.maxScaleLevel - f.minScaleLevel;
    float t = 0.0f;
    if (span > 0) {
        const int32_t into = std::clamp(static_cast<int32_t>(level) - f.minScaleLevel, 0, span);
        t = static_cast<float>(into) / static_cast<float>(span);
    }
    return (f.maxScale - f.minScale) * t + f.minScale;
}

/// A creature's unit scale (0x71C110): `displayScale` is extraRaceScale x
/// CreatureModelScale x ModelScale; `family` the creature's CreatureFamily
/// row or null; `isPet` whether the unit has a pet number. The family scale
/// wins when larger, and always for a pet.
///
/// One departure: a family whose scale works out to zero or less (the
/// "Remote Control" row is all zeroes) leaves the display's size alone
/// rather than drawing the unit at nothing.
inline float creatureUnitScale(float displayScale, const CreatureFamilyScale* family,
                               uint32_t level, bool isPet) {
    if (!(displayScale > 0.0f)) displayScale = 1.0f;
    if (!family) return displayScale;
    const float fam = creatureFamilyScaleAt(*family, level);
    if (!(fam > 0.0f)) return displayScale;
    return (fam > displayScale || isPet) ? fam : displayScale;
}

}  // namespace core
}  // namespace wowee
