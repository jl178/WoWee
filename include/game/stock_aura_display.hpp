#pragma once

// Which of a unit's auras the stock 3.3.5a client lists: the list UnitAura,
// UnitBuff and UnitDebuff count into, and so what the buff frame, the target
// frame and the party frames draw.
//
// Wow.exe 0x72C510 rebuilds the unit's visible-aura list (CGUnit_C +0xDD4
// helpful, +0xE58 harmful) from its aura slots, and Script_UnitAura (0x614D40
// -> 0x6147C0 -> 0x72C9B0) counts its index into that list, not into the
// slots. An aura gets into the list only when
//   - its slot has a spell and an effect mask (flags & 7),
//   - Spell.dbc has a row for the spell (0x72C510 reads the table at
//     0xAD49D4 directly - an id outside it, or a hole in it, is skipped), and
//   - 0x53D580 passes the row:
//       Attributes 0x80 (DO_NOT_DISPLAY) or AttributesEx 0x10000000
//         (DONT_DISPLAY_IN_AURA_BAR): never;
//       Attributes 0x40 (passive): only with an area-aura effect
//         (0x76BED0: 27, 35, 65, 119, 128, 129, 143);
//       AttributesEx5 0x10000000 hides it when self-cast (aura flag 0x08),
//         0x08000000 when cast by someone else;
//       AttributesEx6 0x100000: only when the player or its pet (PetInfo's
//         list at 0xC23500, entry 0 the active pet) cast it;
//       RequiredAuraVision: only when the player has that vision bit
//         (0x4F72E0, the player's +0x10E7 byte).
// An aura that fails is not "Unknown" or blank: it is not there, and the
// auras after it move up an index. A server's own spells (a realm module's
// aura the client never shipped) are the common case of the missing row.

#include <cstdint>

namespace wowee::game::stock_aura {

/// The Spell.dbc row's words 0x53D580 reads. `hasRow` false: no row.
struct AuraDisplayRow {
    bool hasRow = false;
    uint32_t attributes = 0;         // column 4
    uint32_t attributesEx = 0;       // column 5
    uint32_t attributesEx5 = 0;      // column 9
    uint32_t attributesEx6 = 0;      // column 10
    uint32_t effects[3] = {0, 0, 0}; // columns 71-73 (Effect)
    uint32_t requiredAuraVision = 0; // column 221
};

inline constexpr uint32_t kAttrPassive = 0x40;
inline constexpr uint32_t kAttrDoNotDisplay = 0x80;
inline constexpr uint32_t kAttrExDontDisplayInAuraBar = 0x10000000;
inline constexpr uint32_t kAttrEx5HideIfSelfCast = 0x10000000;
inline constexpr uint32_t kAttrEx5HideIfNotSelfCast = 0x08000000;
inline constexpr uint32_t kAttrEx6OnlyVisibleToCaster = 0x00100000;
/// SMSG_AURA_UPDATE flag: no caster guid follows, the unit cast it on itself.
inline constexpr uint8_t kAuraFlagSelfCast = 0x08;
inline constexpr uint8_t kAuraFlagEffectMask = 0x07;

/// 0x76BED0: the effects that let a passive aura be listed - the area auras
/// (persistent area, party, raid, pet, friend, enemy, owner).
[[nodiscard]] constexpr bool isAreaAuraEffect(uint32_t effect) {
    switch (effect) {
        case 27: case 35: case 65: case 119: case 128: case 129: case 143:
            return true;
        default:
            return false;
    }
}

/// 0x53D580 with 0x72C510's own tests in front.
///   `auraFlags`      the slot's SMSG_AURA_UPDATE flags;
///   `wotlkFlags`     whether those flags are 3.3.5's (the effect mask and
///                    the self-cast bit mean something else before it);
///   `castByPlayer`   the caster is the player or its active pet;
///   `playerVisionMask` the player's aura-vision bits (bit n-1 for vision n).
[[nodiscard]] constexpr bool listed(const AuraDisplayRow& row, uint8_t auraFlags, bool wotlkFlags,
                                    bool castByPlayer, uint32_t playerVisionMask = 0) {
    if (!row.hasRow) return false;
    if (wotlkFlags && (auraFlags & kAuraFlagEffectMask) == 0) return false;
    if ((row.attributes & kAttrDoNotDisplay) != 0) return false;
    if ((row.attributesEx & kAttrExDontDisplayInAuraBar) != 0) return false;
    if ((row.attributes & kAttrPassive) != 0) {
        bool area = false;
        for (uint32_t e : row.effects) area = area || isAreaAuraEffect(e);
        if (!area) return false;
    }
    if (wotlkFlags) {
        const bool selfCast = (auraFlags & kAuraFlagSelfCast) != 0;
        const uint32_t hide = selfCast ? kAttrEx5HideIfSelfCast : kAttrEx5HideIfNotSelfCast;
        if ((row.attributesEx5 & hide) != 0) return false;
    }
    if ((row.attributesEx6 & kAttrEx6OnlyVisibleToCaster) != 0 && !castByPlayer) return false;
    if (row.requiredAuraVision != 0) {
        const uint32_t bit = row.requiredAuraVision - 1;
        if (bit >= 32 || (playerVisionMask & (1u << bit)) == 0) return false;
    }
    return true;
}

} // namespace wowee::game::stock_aura
