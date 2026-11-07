#pragma once

/// Which combat animation a unit plays, from the weapons it holds.
///
/// One set of rules for every unit - the local character, other players and
/// creatures - so a swing, a parry, a ready stance or a melee ability looks
/// the same whoever it belongs to. The local character used to be the only
/// one that knew its weapons; everyone else swung ATTACK_1H, parried with a
/// flinch and stood at ease between blows.
///
/// The stock rules these follow:
///  - A melee ability plays the AnimID of its SpellVisual's cast kit
///    (SpellVisualKit.AnimID). Heroic Strike, Mortal Strike, Cleave, Rend and
///    Hamstring name Special1H (57); Slam names Special2H (58); Shield Bash
///    and Shield Slam ShieldBash (59); Sinister Strike Attack1H (17);
///    Backstab Attack1HPierce (85). The weapon-generic ones are swapped for
///    the variant that fits what is in the hand: a two-hander's Special1H is
///    Special2H, an empty hand's is SpecialUnarmed.
///  - An auto-attack swing is picked from the swinging hand's weapon, the
///    hand coming from SMSG_ATTACKERSTATEUPDATE's HITINFO_OFFHAND bit.
///  - Parry is the parrying weapon's Parry1H/2H/2HL/Unarmed; a block is
///    ShieldBlock, since only a shield blocks.
///  - In combat with weapons drawn, the idle is the drawn weapon's Ready
///    stance instead of Stand.

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "game/inventory.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/melee_anim_chains.hpp"
#include "rendering/animation/weapon_type.hpp"

namespace wowee::rendering::anim {

/// Item class 2 (weapon) subclasses, as Item.dbc and the item cache number them.
namespace WeaponSubclass {
    constexpr uint32_t AXE_1H = 0, AXE_2H = 1, BOW = 2, GUN = 3, MACE_1H = 4,
                       MACE_2H = 5, POLEARM = 6, SWORD_1H = 7, SWORD_2H = 8,
                       STAFF = 10, FIST = 13, MISC = 14, DAGGER = 15, THROWN = 16,
                       SPEAR = 17, CROSSBOW = 18, WAND = 19, FISHING_POLE = 20;
}
constexpr uint32_t kItemClassWeapon = 2;
constexpr uint32_t kItemClassArmor = 4;
constexpr uint32_t kArmorSubclassShield = 6;

/// One equipped item as the animation rules need it.
struct WeaponItem {
    uint32_t itemClass = 0;
    uint32_t subclass = 0;
    uint32_t inventoryType = 0;
    [[nodiscard]] bool empty() const { return inventoryType == 0 && itemClass == 0; }
    [[nodiscard]] bool isWeapon() const { return itemClass == kItemClassWeapon; }
    [[nodiscard]] bool isShield() const {
        return inventoryType == game::InvType::SHIELD ||
               (itemClass == kItemClassArmor && subclass == kArmorSubclassShield);
    }
};

/// The loadout from the three weapon slots' items.
inline WeaponLoadout loadoutFromItems(const WeaponItem& mainHand, const WeaponItem& offHand,
                                      const WeaponItem& ranged) {
    WeaponLoadout w;
    if (mainHand.isWeapon() || (mainHand.inventoryType != 0 && !mainHand.isShield())) {
        w.inventoryType = mainHand.inventoryType;
        if (mainHand.isWeapon()) {
            w.is2HLoose = mainHand.subclass == WeaponSubclass::POLEARM ||
                          mainHand.subclass == WeaponSubclass::STAFF ||
                          mainHand.subclass == WeaponSubclass::SPEAR;
            w.isFist = mainHand.subclass == WeaponSubclass::FIST;
            w.isDagger = mainHand.subclass == WeaponSubclass::DAGGER;
            // A two-hander told only by its subclass (a creature's virtual
            // item can carry inventory type 0).
            if (w.inventoryType == 0) {
                switch (mainHand.subclass) {
                    case WeaponSubclass::AXE_2H: case WeaponSubclass::MACE_2H:
                    case WeaponSubclass::SWORD_2H: case WeaponSubclass::POLEARM:
                    case WeaponSubclass::STAFF: case WeaponSubclass::SPEAR:
                    case WeaponSubclass::FISHING_POLE:
                        w.inventoryType = game::InvType::TWO_HAND; break;
                    default:
                        w.inventoryType = game::InvType::ONE_HAND; break;
                }
            }
        }
    }
    if (offHand.isShield()) {
        w.hasShield = true;
    } else if (offHand.isWeapon() ||
               game::isOffHandWeaponInventoryType(static_cast<uint8_t>(offHand.inventoryType))) {
        w.hasOffHand = true;
        w.offHandIsFist = offHand.isWeapon() && offHand.subclass == WeaponSubclass::FIST;
        w.offHandIsDagger = offHand.isWeapon() && offHand.subclass == WeaponSubclass::DAGGER;
    }
    if (ranged.isWeapon()) {
        switch (ranged.subclass) {
            case WeaponSubclass::BOW:      w.rangedType = RangedWeaponType::BOW; break;
            case WeaponSubclass::CROSSBOW: w.rangedType = RangedWeaponType::CROSSBOW; break;
            case WeaponSubclass::GUN:      w.rangedType = RangedWeaponType::GUN; break;
            case WeaponSubclass::THROWN:   w.rangedType = RangedWeaponType::THROWN; break;
            case WeaponSubclass::WAND:     w.rangedType = RangedWeaponType::WAND; break;
            default: break;
        }
    } else if (ranged.inventoryType == game::InvType::RANGED_BOW) {
        w.rangedType = RangedWeaponType::BOW;
    } else if (ranged.inventoryType == game::InvType::RANGED_GUN) {
        w.rangedType = RangedWeaponType::GUN;
    } else if (ranged.inventoryType == game::InvType::THROWN) {
        w.rangedType = RangedWeaponType::THROWN;
    }
    return w;
}

inline bool isUnarmedMainHand(const WeaponLoadout& w) {
    return w.inventoryType == game::InvType::NON_EQUIP || w.isFist;
}
inline bool isTwoHandMainHand(const WeaponLoadout& w) {
    return w.inventoryType == game::InvType::TWO_HAND || w.is2HLoose;
}

/// A two-hander held with something in the other hand (Titan's Grip). The
/// stock client gives it the one-handed swing, stance and parry: each of its
/// picks takes 2H minus one when the off-hand slot holds an item.
inline bool isGrippedTwoHander(const WeaponLoadout& w) {
    return w.inventoryType == game::InvType::TWO_HAND && !w.is2HLoose &&
           (w.hasOffHand || w.hasShield);
}

/// The chain an auto-attack swing walks, for one hand. The head is what the
/// stock client picks on SMSG_ATTACKERSTATEUPDATE (Wow.exe 0x755130), from
/// the swinging hand's item:
///  - off hand (HITINFO_OFFHAND): a dagger AttackOffPierce, any other weapon
///    (a fist weapon too) AttackOff, no weapon there AttackUnarmedOff;
///  - main hand: a dagger Attack1HPierce; a polearm, staff, spear or fishing
///    pole Attack2HL; a two-handed axe, mace or sword Attack2H (Attack1H when
///    gripped with something else); other one-handers Attack1H; a fist
///    weapon, or nothing, AttackUnarmed.
inline MeleeChain swingChain(const WeaponLoadout& w, bool offHand) {
    if (offHand) {
        if (!w.hasOffHand) return MeleeChain::OffHandUnarmed;
        return w.offHandIsDagger ? MeleeChain::OffHandPierce : MeleeChain::OffHand;
    }
    if (w.isFist)   return MeleeChain::Fist;
    if (w.isDagger) return MeleeChain::Dagger;
    if (w.is2HLoose) return MeleeChain::TwoHandLoose;
    if (w.inventoryType == game::InvType::TWO_HAND)
        return isGrippedTwoHander(w) ? MeleeChain::OneHand : MeleeChain::TwoHand;
    if (w.inventoryType == game::InvType::NON_EQUIP) return MeleeChain::Unarmed;
    return MeleeChain::OneHand;
}

/// The spell-cast families the cast-animation path already handles.
inline bool isSpellCastFamily(uint32_t id) {
    switch (id) {
        case SPELL: case SPELL_PRECAST: case SPELL_CAST: case SPELL_CAST_AREA:
        case READY_SPELL_DIRECTED: case READY_SPELL_OMNI:
        case SPELL_CAST_DIRECTED: case SPELL_CAST_OMNI:
        case CHANNEL_CAST_DIRECTED: case CHANNEL_CAST_OMNI:
            return true;
        default:
            return false;
    }
}

/// A weapon (or fist, or shield) swing of some kind - what gets a whoosh.
inline bool isMeleeAttackAnim(uint32_t id) {
    switch (id) {
        case ATTACK_UNARMED: case ATTACK_1H: case ATTACK_2H: case ATTACK_2H_LOOSE:
        case SPECIAL_1H: case SPECIAL_2H: case SHIELD_BASH: case SPECIAL_UNARMED:
        case ATTACK_1H_PIERCE: case ATTACK_2H_LOOSE_PIERCE: case ATTACK_OFF:
        case ATTACK_OFF_PIERCE: case ATTACK_UNARMED_OFF: case KICK: case WHIRLWIND:
        case MUTILATE:
            return true;
        default:
            return false;
    }
}

/// The candidates for an ability's kit animation, best first, with the
/// weapon-generic attacks swapped for the held weapon's variant. Empty for
/// "no animation of its own": the caller swings normally.
inline std::vector<uint32_t> abilityAnimCandidates(uint32_t kitAnim, const WeaponLoadout& w) {
    std::vector<uint32_t> out;
    auto push = [&](std::initializer_list<uint32_t> ids) {
        for (uint32_t id : ids) out.push_back(id);
    };
    auto pushChain = [&](MeleeChain c) {
        for (uint32_t id : meleeAnimChain(c)) out.push_back(id);
    };
    switch (kitAnim) {
        case 0:
            break;
        case SPECIAL_1H: case SPECIAL_2H: case SPECIAL_UNARMED:
            // The weapon-generic special: the held weapon's.
            if (isUnarmedMainHand(w))      push({SPECIAL_UNARMED, SPECIAL_1H});
            else if (isTwoHandMainHand(w)) push({SPECIAL_2H, SPECIAL_1H});
            else                           push({SPECIAL_1H});
            pushChain(swingChain(w, false));
            break;
        case ATTACK_UNARMED: case ATTACK_1H: case ATTACK_2H: case ATTACK_2H_LOOSE:
            pushChain(swingChain(w, false));
            break;
        case ATTACK_OFF: case ATTACK_OFF_PIERCE: case ATTACK_UNARMED_OFF:
            pushChain(swingChain(w, true));
            break;
        default:
            // An animation of its own (ShieldBash, Attack1HPierce, Whirlwind,
            // Kick, BattleRoar, Mutilate...): that, then AnimationData's
            // fallbacks for it (ShieldBash -> AttackOff -> AttackOffPierce ->
            // AttackUnarmed) for a model that has not got it.
            out.push_back(kitAnim);
            for (uint32_t fb = dbcFallback(kitAnim), step = 0;
                 step < 8 && fb != UINT32_MAX && fb != 0; ++step, fb = dbcFallback(fb))
                out.push_back(fb);
            break;
    }
    return out;
}

/// The parry for the weapon that parries (Wow.exe 0x73B050: the main hand's
/// subclass; a fist weapon or an empty hand ParryUnarmed, a gripped
/// two-hander Parry1H).
inline uint32_t parryAnim(const WeaponLoadout& w) {
    if (isUnarmedMainHand(w)) return PARRY_UNARMED;
    if (w.is2HLoose) return PARRY_2H_LOOSE;
    if (w.inventoryType == game::InvType::TWO_HAND) return isGrippedTwoHander(w) ? PARRY_1H : PARRY_2H;
    return PARRY_1H;
}

/// UNIT_FIELD_BYTES_2 byte 0.
enum class SheathState : uint8_t { UNARMED = 0, MELEE = 1, RANGED = 2 };

/// The ready stance a unit holds in combat: the ranged weapon's when that is
/// what is drawn, else the main hand's - the same rule the local character's
/// CombatFSM follows, which reads the loadout and not the sheath byte.
/// A sheath state of 0 used to mean ReadyUnarmed (fists up) whatever the
/// unit held, and AzerothCore never sets that byte for anyone who does not
/// send CMSG_SET_SHEATHED: playerbots stay at 0 for good, so every bot
/// priest, warlock and mage boxed between casts with a staff on its back.
inline uint32_t readyAnim(const WeaponLoadout& w, SheathState sheath) {
    if (sheath == SheathState::RANGED) {
        switch (w.rangedType) {
            case RangedWeaponType::BOW:
            case RangedWeaponType::CROSSBOW: return READY_BOW;
            case RangedWeaponType::GUN:      return READY_RIFLE;
            case RangedWeaponType::THROWN:   return READY_THROWN;
            case RangedWeaponType::WAND:     return READY_SPELL_DIRECTED;
            default: break;
        }
    }
    // The melee stance from the main hand's weapon, else the off hand's
    // (Wow.exe 0x714DD0): a fist weapon takes Ready1H like any one-hander -
    // it is only the swing that is a punch - and nothing in either hand
    // ReadyUnarmed.
    if (w.inventoryType == game::InvType::NON_EQUIP)
        return w.hasOffHand ? READY_1H : READY_UNARMED;
    if (w.is2HLoose) return READY_2H_LOOSE;
    if (w.inventoryType == game::InvType::TWO_HAND) return isGrippedTwoHander(w) ? READY_1H : READY_2H;
    return READY_1H;
}

/// Whether an animation may play on the upper body alone, over legs that
/// keep walking or running (Wow.exe 0x71D800, by AnimationData BehaviorID).
/// While the unit moves, the stock client plays these on the upper-body bone
/// set and leaves the locomotion on the rest (0x7385C0, 0x723E30): attacks,
/// parries, wounds, Ready stances, spell casts, ranged shots, emotes, the
/// sheath reach. Anything else (Kick, Whirlwind, Death) takes the whole body.
inline bool isUpperBodyBehavior(uint32_t b) {
    if (b == 2 || (b >= 8 && b <= 10) || (b >= 14 && b <= 36) || (b >= 46 && b <= 49) ||
        (b >= 51 && b <= 74) || (b >= 76 && b <= 78) || (b >= 80 && b <= 90) ||
        (b >= 105 && b <= 113) || b == 117 || b == 118 || (b >= 122 && b <= 125) ||
        (b >= 128 && b <= 130) || b == 133 || b == 134 || (b >= 136 && b <= 138) ||
        (b >= 153 && b <= 156) || b == 185 || b == 186 || b == 195 ||
        (b >= 213 && b <= 222) || b == 225)
        return true;
    return false;
}
inline bool playsOnUpperBody(uint32_t anim) { return isUpperBodyBehavior(dbcBehavior(anim)); }

/// The locomotion animations (Wow.exe 0x714E80): walk, run, sprint, the
/// shuffles and strafes' base, backpedal, the jump phases, swimming,
/// stealth walk and run, JumpLandRun. A unit playing one of these is moving
/// its legs, so a swing or a flinch goes to its upper body.
inline bool isLocomotionAnim(uint32_t id) {
    switch (id) {
        case 4: case 5: case 11: case 12: case 13: case 37: case 38: case 39:
        case 42: case 43: case 44: case 45: case 119: case 135: case 143: case 187: case 223:
            return true;
        default:
            return false;
    }
}

/// The flinch a blow plays on its victim (Wow.exe 0x736640, from 0x755E40
/// when the swing has HITINFO_AFFECTS_VICTIM): CombatCritical for a crit;
/// otherwise CombatWound while the victim has an attack target of its own,
/// StandWound when it is not fighting back.
inline uint32_t woundAnim(bool crit, bool attacking) {
    if (crit) return COMBAT_CRITICAL;
    return attacking ? COMBAT_WOUND : STAND_WOUND;
}

/// Which track that flinch takes (0x736640): the whole body only for a
/// StandWound on a unit at ease - not moving, not casting; a crit or a unit
/// with an attack target of its own flinches on the upper-body track, over
/// whatever its body plays (its stance, its swing, its cast).
inline bool woundOnUpperBody(bool crit, bool attacking, bool moving, bool casting) {
    return crit || attacking || moving || casting;
}

/// The sheath state a unit's weapons are shown in, which in the stock client
/// is not the server's byte alone (Wow.exe 0x738180, run on every animation
/// change). In order:
///  - an animation that always puts them away (WeaponFlags 0x04: spell casts,
///    swimming, mounted, loot) puts them away;
///  - a ranged weapon out stays out, unless an animation that is not a shot
///    puts it away (0x10) or draws the melee weapons (0x20);
///  - an animation that puts away (0x10: emotes, ReadyUnarmed) or draws
///    (0x20: Attack1H/2H, Ready1H/2H, Special*) does that - except that a
///    unit with an attack target keeps them drawn through its combat moves
///    (parries, wounds, swings);
///  - a unit with an attack target (SMSG_ATTACKSTART until SMSG_ATTACKSTOP)
///    has them drawn;
///  - otherwise the server's byte.
/// So a playerbot (whose byte AzerothCore leaves at 0, as it sends no
/// CMSG_SET_SHEATHED) and a creature whose addon sheathes it draw when they
/// swing, and an NPC that talks puts its sword away for the emote.
/// `shown` is what the unit shows now. Left out: the play flag that keeps
/// the state (bit 5 of the play call's flags) and the cast of a spell
/// without SPELL_ATTR0_DO_NOT_SHEATH, whose cast animations put the weapons
/// away already.
inline bool isCombatMoveBehavior(uint32_t b) {
    switch (b) {
        case 10: case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23:
        case 24: case 30: case 36: case 57: case 58: case 59: case 85: case 86: case 87:
        case 88: case 95: case 117: case 118: case 212:
            return true;
        default:
            return b >= 170 && b <= 179;
    }
}
inline bool isRangedShotBehavior(uint32_t b) {
    return b == ATTACK_BOW || b == ATTACK_RIFLE || (b >= 105 && b <= 112);
}
inline SheathState presentedSheath(SheathState server, SheathState shown, uint32_t anim,
                                   bool hasAttackTarget) {
    const uint32_t wf = dbcWeaponFlags(anim);
    const uint32_t behavior = dbcBehavior(anim);
    if (wf & ANIM_WF_PUT_AWAY_ALWAYS) return SheathState::UNARMED;
    // This client never draws the ranged weapon on its own, so a ranged
    // state that the server's byte no longer backs was changed by the
    // server - the field update the stock client takes as it comes.
    if (shown == SheathState::RANGED && server != SheathState::RANGED) shown = server;
    if (shown == SheathState::RANGED) {
        if (!isRangedShotBehavior(behavior)) {
            if (wf & ANIM_WF_PUT_AWAY) return SheathState::UNARMED;
            if (wf & ANIM_WF_DRAW) return SheathState::MELEE;
        }
        return SheathState::RANGED;
    }
    if (!hasAttackTarget || !isCombatMoveBehavior(behavior)) {
        if (wf & ANIM_WF_PUT_AWAY) return SheathState::UNARMED;
        if (wf & ANIM_WF_DRAW) return SheathState::MELEE;
    }
    if (hasAttackTarget) return SheathState::MELEE;
    return server;
}

/// The reach for an item put away (or drawn from) a spot: Sheath (89) for
/// the back, HipSheath (90) for the hip, 0 for an empty hand. 3.3.5 has no
/// separate unsheathe; the same reach plays both ways, and the model's
/// $SHL/$SHR event marks where the weapon changes hands.
inline uint32_t sheathReachAnim(SheathSpot spot) {
    return spot == SheathSpot::HIP ? HIP_SHEATHE : spot == SheathSpot::BACK ? SHEATHE : 0u;
}

inline bool isReadyStance(uint32_t id) {
    return (id >= READY_UNARMED && id <= READY_BOW) || id == READY_RIFLE ||
           id == READY_THROWN || id == READY_SPELL_DIRECTED;
}

/// The first id in `ids` the model can play, walking each one's
/// AnimationData fallback too; 0 when none. `has` answers for the model.
template <typename Has>
uint32_t firstPlayable(const Has& has, std::initializer_list<uint32_t> ids) {
    for (uint32_t id : ids) if (has(id)) return id;
    for (uint32_t id : ids) {
        uint32_t fb = dbcFallback(id);
        for (int step = 0; step < 8 && fb != UINT32_MAX && fb != 0; ++step) {
            if (has(fb)) return fb;
            fb = dbcFallback(fb);
        }
    }
    return 0;
}
template <typename Has>
uint32_t firstPlayable(const Has& has, const std::vector<uint32_t>& ids) {
    for (uint32_t id : ids) if (has(id)) return id;
    return 0;
}

}  // namespace wowee::rendering::anim
