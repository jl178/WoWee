#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game/handler_types.hpp"
#include "game/spell_defines.hpp"

namespace wowee {
namespace game {

/// COMBAT_TEXT_UPDATE's arguments for one hit that landed on the player.
///
/// Blizzard_CombatText draws incoming numbers from this event alone, and reads
/// one message per hit: a partial resist, block or absorb is not a second line
/// beside the damage but the damage itself, tagged - "RESIST", 600, 200 draws
/// "-600 (200 resisted)", and with resistances switched off the addon turns it
/// back into a plain "-600". So the hit is reduced to one message here, in the
/// order the addon's own trailers suggest, a crit outranking all of them.
///
/// A spell hit is the SPELL_ family, drawn in the addon's spell colour; a
/// physical one - a melee swing, or a physical ability or bleed - is the plain
/// family. A hit reduced to nothing by a full resist, block or absorb carries
/// no trailer, which is how the addon knows to print the bare word.
///
/// Empty when there is nothing to say: a zero-damage hit with nothing taken
/// off it is a message the addon itself would drop.
inline std::vector<std::string> combatTextUpdateForHit(bool spell, bool crit, uint32_t damage,
                                                       uint32_t resisted, uint32_t blocked,
                                                       uint32_t absorbed) {
    const std::string pre = spell ? "SPELL_" : "";
    const std::string dmg = std::to_string(damage);
    const auto reduced = [&](const char* kind, uint32_t amount) -> std::vector<std::string> {
        if (damage == 0) return {pre + kind};
        return {pre + kind, dmg, std::to_string(amount)};
    };
    if (crit && damage > 0) return {pre + "DAMAGE_CRIT", dmg};
    if (resisted > 0) return reduced("RESIST", resisted);
    if (blocked > 0)  return reduced("BLOCK", blocked);
    if (absorbed > 0) return reduced("ABSORB", absorbed);
    if (damage == 0) return {};
    return {pre + "DAMAGE", dmg};
}

/// The power token Blizzard_CombatText names an energize with ("MANA", "RAGE"
/// ...), which it also looks the colour up by. Nullptr for a power it has no
/// word for, which the addon would print bare.
inline const char* combatTextPowerToken(uint8_t powerType) {
    switch (powerType) {
        case 0: return "MANA";
        case 1: return "RAGE";
        case 2: return "FOCUS";
        case 3: return "ENERGY";
        case 6: return "RUNIC_POWER";
        default: return nullptr;
    }
}

/// The COMBAT_TEXT_UPDATE type for a spell that missed the player outright,
/// from the miss already turned into a line type. A whole miss carries no
/// amount, so the addon prints the bare word ("Resist", "Reflect"). Nullptr for
/// anything that is not a miss.
inline const char* combatTextSpellMissType(CombatTextEntry::Type t) {
    using T = CombatTextEntry;
    switch (t) {
        case T::MISS:    return "SPELL_MISS";
        case T::DODGE:   return "SPELL_DODGE";
        case T::PARRY:   return "SPELL_PARRY";
        case T::BLOCK:   return "SPELL_BLOCK";
        case T::EVADE:   return "SPELL_EVADE";
        case T::IMMUNE:  return "SPELL_IMMUNE";
        case T::DEFLECT: return "SPELL_DEFLECT";
        case T::REFLECT: return "SPELL_REFLECT";
        case T::RESIST:  return "SPELL_RESIST";
        case T::ABSORB:  return "SPELL_ABSORB";
        default:         return nullptr;
    }
}

/// Which Combat Text panel checkbox covers one floating line.
///
/// Six controls in that panel name a CVar that nothing read, so clearing any
/// of them changed the panel and not the screen. They divide one stream by what
/// produced it, so the mapping is decided once, here, rather than at each of
/// the call sites that raise a line.
///
/// The rule is returned rather than applied: this header knows which question
/// to ask and what the answer is when nobody has chosen, and the caller does
/// the CVar lookup. That keeps the decision testable without a settings store,
/// and keeps the store out of a header that only describes combat text.
struct CombatTextFilterRule {
    /// The CVar that governs this line, or nullptr when none does.
    const char* cvar = nullptr;
    /// What that CVar means when it has never been set.
    const char* fallback = "1";
};

/// The rule covering one line, given who caused it and where it landed.
///
/// Only lines the player caused are filtered. Damage arriving at the player is
/// a different question with its own rows in the panel, and a checkbox reading
/// "Target Damage" does not mean "damage taken".
inline CombatTextFilterRule combatTextFilterFor(CombatTextEntry::Type type,
                                                bool isPlayerSource,
                                                uint64_t srcGuid,
                                                uint64_t dstGuid,
                                                uint64_t petGuid,
                                                uint64_t targetGuid) {
    using T = CombatTextEntry;
    switch (type) {
        // Effects rather than numbers, read against the unit they landed on
        // instead of who caused them: the panel offers your target and
        // everything else as two separate rows.
        case T::IMMUNE: case T::RESIST: case T::DEFLECT: case T::REFLECT:
        case T::INTERRUPT: case T::DISPEL: case T::STEAL: case T::ABSORB: {
            const bool onTarget = (dstGuid != 0 && dstGuid == targetGuid);
            return onTarget ? CombatTextFilterRule{.cvar = "fctSpellMechanics", .fallback = "1"}
                            : CombatTextFilterRule{.cvar = "fctSpellMechanicsOther", .fallback = "0"};
        }
        // A pet's swing is its own row, and it is answered before the damage
        // row below so that clearing pet damage does not also require the
        // player to clear their own.
        case T::MELEE_DAMAGE:
            if (petGuid != 0 && srcGuid == petGuid) {
                return {.cvar = "PetMeleeDamage", .fallback = "1"};
            }
            [[fallthrough]];
        case T::SPELL_DAMAGE: case T::CRIT_DAMAGE:
        case T::GLANCING: case T::CRUSHING:
            if (!isPlayerSource) return {};
            return {.cvar = "CombatDamage", .fallback = "1"};
        case T::PERIODIC_DAMAGE: case T::PERIODIC_HEAL:
            if (!isPlayerSource) return {};
            return {.cvar = "CombatLogPeriodicSpells", .fallback = "1"};
        case T::HEAL: case T::CRIT_HEAL:
            if (!isPlayerSource) return {};
            return {.cvar = "CombatHealing", .fallback = "1"};
        default:
            return {};
    }
}

} // namespace game
} // namespace wowee
