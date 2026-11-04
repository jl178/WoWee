#pragma once

// The stock rules for what a spell's missile is made of when it is not a
// SpellVisualEffectName model: the caster's weapons, the ammo the server
// names in SMSG_SPELL_GO, and the ranged weapon's own SpellVisual, which a
// ranged attack borrows (Auto Shot, Shoot, Throw: a wand's school bolt, a
// gun's report, a bow's draw). Pure functions over DBC values, so they can
// be tested without assets. See docs/re/spell-cast-pipeline.md section 6.

#include <array>
#include <cstdint>
#include <string>

namespace wowee {
namespace rendering {
namespace missilerules {

/// A 3.3.5a SpellVisual record, column by column (32 four-byte columns:
/// 0 ID, 1 Precast, 2 Cast, 3 Impact, 4 State, 5 StateDone, 6 Channel,
/// 7 HasMissile, 8 MissileModel, 9 PathType, 10 MissileDestination,
/// 11 MissileSound, 12 AnimEventSound, 13 Flags, 14 CasterImpact,
/// 15 TargetImpact, 16 MissileAttachment, 17-20 FollowGround height, drop
/// speed, approach, flags, 21 Motion, 22 MissileTargetingKit, 23 InstantArea,
/// 24 ImpactArea, 25 PersistentArea, 26-31 cast and impact offsets).
using VisualColumns = std::array<int32_t, 32>;

/// Spell Attributes 0x2: the spell uses the ranged slot (Auto Shot, Shoot,
/// Throw, Arcane Shot, Multi-Shot...). Only these borrow the ranged weapon's
/// visual (Wow.exe 0x720F80 tests the spell's Attributes & 2).
constexpr bool usesRangedItemVisual(uint32_t attributes0) { return (attributes0 & 0x2u) != 0; }

/// CGUnit_C vtable +0xBC (Wow.exe 0x720F80): the visual a spell plays.
/// The spell's own row (null when it names none: Auto Shot, Shoot and Throw
/// have SpellVisual 0) with every one of these left 0 taken from the ranged
/// weapon's ItemDisplayInfo.SpellVisualID row: the precast, cast, impact,
/// caster impact and target impact kits, the missile (HasMissile and
/// MissileModel together, only when the weapon's row has one), the missile
/// attachments, the follow-ground columns, the motion, the missile sound and
/// the anim event sound. Nothing else is taken: a spell without a visual of
/// its own gets those columns and zeros elsewhere (flags, channel, area kits).
VisualColumns mergeRangedItemVisual(const VisualColumns* spell, const VisualColumns& item);

/// Whether a missile flies (Wow.exe 0x7FFFB0): the visual has one, or the
/// server named ammo for the shot (a bow's visual has HasMissile 0; the arrow
/// comes from SPELL_GO).
constexpr bool fliesMissile(bool hasMissile, uint32_t ammoDisplayId) {
    return hasMissile || ammoDisplayId != 0;
}

/// What the missile model is (Wow.exe 0x732FF0, by SpellVisual.MissileModel).
enum class MissileKind {
    None,       ///< nothing to fly (the stock draws Spells\ErrorCube.mdx)
    Effect,     ///< a SpellVisualEffectName model
    MainHand,   ///< -1: the caster's main-hand item (0x732400 slot 0)
    OffHand,    ///< -2: its off-hand item (slot 1)
    Ranged,     ///< -3: its ranged item (slot 2)
    Ammo,       ///< -4/-5: the ammo SPELL_GO names, by 0x732100
    AmmoModel,  ///< 0 with ammo: the ammo's flight model, by 0x4EAB50
};
MissileKind missileKind(int32_t missileModel, uint32_t ammoDisplayId);
/// The unit's item slot for MainHand/OffHand/Ranged (0, 1, 2); -1 otherwise.
int missileItemSlot(MissileKind kind);

/// ItemDisplayInfo's two model names and two textures (columns 1-4), as the
/// table has them ("ArrowFlight_01.mdx", "Arrow_A_01Brown").
struct ItemDisplayNames {
    std::string model[2];
    std::string texture[2];
};
/// A missile item model: the model file (still .mdx, as the table names it)
/// and its item texture (.blp), both with their component folder. The texture
/// fills the model's item-skin slot (replaceable texture 2).
struct ItemMissilePaths {
    std::string model;
    std::string texture;
};
/// The component folder 0x732100 puts an item under, by InventoryType:
/// 1 Head, 3 Shoulder, 14 Shield, 24 Ammo, 27 Quiver, anything else Weapon.
const char* itemComponentFolder(uint32_t inventoryType);
/// Wow.exe 0x732100 (MissileModel -1..-5): the first model name that is
/// set, and the texture of the same index, under the folder above. False
/// when the display names no model, or for a head item (its file carries the
/// wearer's race and sex, which a missile has no use for).
bool itemMissilePaths(const ItemDisplayNames& names, uint32_t inventoryType, ItemMissilePaths& out);
/// Wow.exe 0x4EAB50 (MissileModel 0 with ammo): only ammo (24, Ammo folder)
/// and thrown weapons (25, Weapon folder), and only the second model and
/// texture - the flight model of an arrow or bullet (5996 names
/// ArrowFlight_01.mdx there and nothing first). False otherwise, or when
/// either name is empty (then the stock flies its ErrorCube).
bool ammoMissilePaths(const ItemDisplayNames& names, uint32_t inventoryType, ItemMissilePaths& out);

}  // namespace missilerules
}  // namespace rendering
}  // namespace wowee
