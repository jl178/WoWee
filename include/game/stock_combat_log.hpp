#pragma once

// The combat log as the stock 3.3.5a client (build 12340) keeps it
// (UnitCombatLog_C.cpp, 0x74D000-0x757000 in Wow.exe). See
// docs/re/chat-combatlog.md, "Combat log", for the addresses of every rule.
//
// An entry is built when a packet says something happened (0x750400 makes it,
// the per-event builders 0x750500-0x753100 fill it), its two units' flags are
// worked out at that moment (0x74DCB0), it waits for its names, and it is then
// handed to Lua as COMBAT_LOG_EVENT - when the filters set by
// CombatLogAddFilter let it through - and COMBAT_LOG_EVENT_UNFILTERED, always
// (0x74F910). The argument list is 0x74E290's. Entries stay in the history for
// combatLogRetentionTime seconds (default "300", 0x51EFE1), and the history is
// what CombatLogSetCurrentEntry and friends walk.
//
// Header only and free of the game: the handlers build Records, GameHandler
// supplies names, flags and spell data, LuaEngine keeps the History.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "game/handler_types.hpp"

namespace wowee {
namespace game {
namespace stock_combat_log {

// ---------------------------------------------------------------------------
// Events: the name table at 0xADB758, indexed by the entry's event number.
// ---------------------------------------------------------------------------
enum Event : int {
    ENVIRONMENTAL_DAMAGE = 0, SWING_DAMAGE, SWING_MISSED, RANGE_DAMAGE, RANGE_MISSED,
    SPELL_CAST_START, SPELL_CAST_SUCCESS, SPELL_CAST_FAILED, SPELL_MISSED, SPELL_DAMAGE,
    SPELL_HEAL, SPELL_ENERGIZE, SPELL_DRAIN, SPELL_LEECH, SPELL_INSTAKILL,
    SPELL_SUMMON, SPELL_CREATE, SPELL_INTERRUPT, SPELL_EXTRA_ATTACKS, SPELL_DURABILITY_DAMAGE,
    SPELL_DURABILITY_DAMAGE_ALL, SPELL_AURA_APPLIED, SPELL_AURA_APPLIED_DOSE,
    SPELL_AURA_REMOVED_DOSE, SPELL_AURA_REMOVED, SPELL_AURA_REFRESH, SPELL_DISPEL,
    SPELL_STOLEN, SPELL_AURA_BROKEN, SPELL_AURA_BROKEN_SPELL, DAMAGE_AURA_BROKEN,
    ENCHANT_APPLIED, ENCHANT_REMOVED, SPELL_PERIODIC_MISSED, SPELL_PERIODIC_DAMAGE,
    SPELL_PERIODIC_HEAL, SPELL_PERIODIC_ENERGIZE, SPELL_PERIODIC_DRAIN, SPELL_PERIODIC_LEECH,
    SPELL_DISPEL_FAILED, DAMAGE_SHIELD, DAMAGE_SHIELD_MISSED, DAMAGE_SPLIT, PARTY_KILL,
    UNIT_DIED, UNIT_DESTROYED, SPELL_RESURRECT, SPELL_BUILDING_DAMAGE, SPELL_BUILDING_HEAL,
    UNIT_DISSIPATES,
    kEventCount
};

inline const char* eventName(int e) {
    static constexpr const char* kNames[kEventCount] = {
        "ENVIRONMENTAL_DAMAGE", "SWING_DAMAGE", "SWING_MISSED", "RANGE_DAMAGE", "RANGE_MISSED",
        "SPELL_CAST_START", "SPELL_CAST_SUCCESS", "SPELL_CAST_FAILED", "SPELL_MISSED",
        "SPELL_DAMAGE", "SPELL_HEAL", "SPELL_ENERGIZE", "SPELL_DRAIN", "SPELL_LEECH",
        "SPELL_INSTAKILL", "SPELL_SUMMON", "SPELL_CREATE", "SPELL_INTERRUPT",
        "SPELL_EXTRA_ATTACKS", "SPELL_DURABILITY_DAMAGE", "SPELL_DURABILITY_DAMAGE_ALL",
        "SPELL_AURA_APPLIED", "SPELL_AURA_APPLIED_DOSE", "SPELL_AURA_REMOVED_DOSE",
        "SPELL_AURA_REMOVED", "SPELL_AURA_REFRESH", "SPELL_DISPEL", "SPELL_STOLEN",
        "SPELL_AURA_BROKEN", "SPELL_AURA_BROKEN_SPELL", "DAMAGE_AURA_BROKEN",
        "ENCHANT_APPLIED", "ENCHANT_REMOVED", "SPELL_PERIODIC_MISSED", "SPELL_PERIODIC_DAMAGE",
        "SPELL_PERIODIC_HEAL", "SPELL_PERIODIC_ENERGIZE", "SPELL_PERIODIC_DRAIN",
        "SPELL_PERIODIC_LEECH", "SPELL_DISPEL_FAILED", "DAMAGE_SHIELD", "DAMAGE_SHIELD_MISSED",
        "DAMAGE_SPLIT", "PARTY_KILL", "UNIT_DIED", "UNIT_DESTROYED", "SPELL_RESURRECT",
        "SPELL_BUILDING_DAMAGE", "SPELL_BUILDING_HEAL", "UNIT_DISSIPATES",
    };
    return (e >= 0 && e < kEventCount) ? kNames[e] : "";
}

/// The event number for a name, as CombatLogAddFilter's list is matched
/// (0x74E050: case-sensitive, one of the 50). -1 for none.
inline int eventIndex(std::string_view name) {
    for (int i = 0; i < kEventCount; ++i)
        if (name == eventName(i)) return i;
    return -1;
}

/// SpellMissInfo -> the missType string (table 0xADBFAC): 0 NONE, 1 MISS,
/// 2 RESIST, 3 DODGE, 4 PARRY, 5 BLOCK, 6 EVADE, 7 and 8 IMMUNE, 9 DEFLECT,
/// 10 ABSORB, 11 REFLECT.
inline const char* missTypeName(uint32_t missInfo) {
    static constexpr const char* kMiss[] = {"NONE", "MISS", "RESIST", "DODGE", "PARRY", "BLOCK",
                                            "EVADE", "IMMUNE", "IMMUNE", "DEFLECT", "ABSORB",
                                            "REFLECT"};
    return missInfo < 12 ? kMiss[missInfo] : nullptr;
}

/// EnviromentalDamage -> environmentalType (0xAD671C), upper case: the addon
/// looks up ACTION_ENVIRONMENTAL_DAMAGE_<type>.
inline const char* environmentalTypeName(uint32_t type) {
    static constexpr const char* kEnv[] = {"FATIGUE", "DROWNING", "FALLING", "LAVA", "SLIME", "FIRE"};
    return type < 6 ? kEnv[type] : nullptr;
}

/// The school an environmental hit is logged with: 1 << the index at
/// 0xA2D394 (physical, physical, physical, fire, nature, fire).
inline uint32_t environmentalSchool(uint32_t type) {
    static constexpr uint32_t kIndex[] = {0, 0, 0, 2, 3, 2, 5, 5};
    return 1u << (type < 8 ? kIndex[type] : 0);
}

// ---------------------------------------------------------------------------
// The entry (CombatLogEntry, 0x78 bytes). The value slots keep the stock
// offsets because the suffix bits read them crosswise: SPELL_MISSED's ABSORB
// amount is the damage suffix's absorbed slot, a dispel's extra spell and its
// aura type share 0x58/0x5C, and so on.
// ---------------------------------------------------------------------------
enum Suffix : uint32_t {
    kSufAmount        = 0x0001,  // int  +0x58        (SPELL_EXTRA_ATTACKS)
    kSufText          = 0x0002,  // text              (SPELL_CAST_FAILED failedType)
    kSufMissType      = 0x0004,  // missType[+0x58]
    kSufEnvironment   = 0x0008,  // environmentalType[+0x58]
    kSufDamage        = 0x0010,  // amount +0x5C, overkill +0x60, school +0x64,
                                 // resisted +0x6C, blocked +0x70, absorbed +0x68, bits +0x74
    kSufHeal          = 0x0020,  // amount +0x58, overhealing +0x5C, absorbed +0x60, crit +0x64
    kSufEnergize      = 0x0040,  // amount +0x58, powerType +0x5C
    kSufDrain         = 0x0080,  // amount +0x58, powerType +0x5C, extraAmount +0x60
    kSufExtraSpell    = 0x0100,  // extraSpellId +0x58, its name and school
    kSufItem          = 0x0200,  // itemId +0x58, itemName
    kSufAuraType      = 0x0400,  // +0x5C: "DEBUFF" if set, else "BUFF"
    kSufAuraAmount    = 0x0800,  // int +0x60 (the new stack count)
    kSufSpellNameText = 0x1000,  // text +0x60 (ENCHANT_* spellName)
    kSufAbsorbedAmt   = 0x2000,  // int +0x68 (a miss's amountMissed, ABSORB)
    kSufResistedAmt   = 0x4000,  // int +0x6C (RESIST)
    kSufBlockedAmt    = 0x8000,  // int +0x70 (BLOCK)
};

struct Record {
    int event = -1;
    uint64_t src = 0;   ///< 0: no source (0x80000000 flags, nil name)
    uint64_t dst = 0;
    uint32_t spellId = 0;  ///< the prefix; 0 for a swing, environment, enchant, kill
    uint32_t suffix = 0;
    int32_t v58 = 0, v5c = 0, v60 = 0, v64 = 0, v68 = 0, v6c = 0, v70 = 0;
    uint32_t hitBits = 0;  ///< +0x74: 1 critical, 2 glancing, 4 crushing
    std::string text;      ///< kSufText
    std::string spellNameText;  ///< kSufSpellNameText
};

/// What the formatter needs of a spell: Spell.dbc's name and SchoolMask.
struct SpellBrief {
    bool known = false;
    std::string name;
    uint32_t schoolMask = 0;
};

/// Everything resolved for one entry, at the moment it is handed to Lua.
struct Resolved {
    double timestamp = 0;
    std::string srcName, dstName;   ///< empty: nil
    uint32_t srcFlags = 0x80000000u, dstFlags = 0x80000000u;
    std::string itemName;           ///< kSufItem; empty: nil
};

/// An argument Lua should receive as a number even though it does not look
/// like a short integer (the timestamp: seconds with milliseconds).
inline constexpr char kNumberMark = '\x02';

inline std::string numberArg(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%c%.3f", kNumberMark, v);
    return buf;
}

inline std::string guidText(uint64_t guid) {
    // 0x74D0D0: "0x" and sixteen upper-case digits.
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(guid));
    return buf;
}

/// 0x74E290: the argument list, exactly as CombatLogGetCurrentEntry and the
/// two events give it.
inline std::vector<std::string> formatArgs(const Record& r, const Resolved& res,
                                           const std::function<SpellBrief(uint32_t)>& spell) {
    const std::string nil = kEventNil;
    auto num = [](int64_t v) { return std::to_string(v); };
    auto numOrNil = [&](int32_t v) { return v == 0 ? nil : num(v); };
    std::vector<std::string> a;
    a.reserve(24);
    a.push_back(numberArg(res.timestamp));
    a.push_back(eventName(r.event));
    a.push_back(guidText(r.src));
    a.push_back(res.srcName.empty() ? nil : res.srcName);
    a.push_back(num(res.srcFlags));
    a.push_back(guidText(r.dst));
    a.push_back(res.dstName.empty() ? nil : res.dstName);
    a.push_back(num(res.dstFlags));
    auto pushSpell = [&](uint32_t id) {
        a.push_back(num(id));
        const SpellBrief s = spell ? spell(id) : SpellBrief{};
        if (s.known) {
            a.push_back(s.name.empty() ? nil : s.name);
            a.push_back(num(s.schoolMask));
        } else {
            a.push_back(nil);
            a.push_back("0");
        }
    };
    if (r.spellId != 0) pushSpell(r.spellId);
    const uint32_t f = r.suffix;
    if (f & kSufAmount) a.push_back(num(r.v58));
    if (f & kSufText) a.push_back(r.text);
    if (f & kSufMissType) {
        const char* m = missTypeName(static_cast<uint32_t>(r.v58));
        a.push_back(m ? m : nil);
    }
    if (f & kSufEnvironment) {
        const char* e = environmentalTypeName(static_cast<uint32_t>(r.v58));
        a.push_back(e ? e : nil);
    }
    if (f & kSufDamage) {
        a.push_back(num(r.v5c));            // amount
        a.push_back(num(r.v60));            // overkill
        a.push_back(num(r.v64));            // school
        a.push_back(numOrNil(r.v6c));       // resisted
        a.push_back(numOrNil(r.v70));       // blocked
        a.push_back(numOrNil(r.v68));       // absorbed
        a.push_back(eventBool(r.hitBits & 1));
        a.push_back(eventBool(r.hitBits & 2));
        a.push_back(eventBool(r.hitBits & 4));
    }
    if (f & kSufAbsorbedAmt) a.push_back(num(r.v68));
    if (f & kSufResistedAmt) a.push_back(num(r.v6c));
    if (f & kSufBlockedAmt) a.push_back(num(r.v70));
    if (f & kSufHeal) {
        a.push_back(num(r.v58));            // amount
        a.push_back(num(r.v5c));            // overhealing
        a.push_back(num(r.v60));            // absorbed
        a.push_back(eventBool(r.v64 != 0)); // critical
    }
    if (f & kSufEnergize) {
        a.push_back(num(r.v58));
        a.push_back(num(r.v5c));
    }
    if (f & kSufDrain) {
        a.push_back(num(r.v58));
        a.push_back(num(r.v5c));
        a.push_back(numOrNil(r.v60));
    }
    if (f & kSufExtraSpell) pushSpell(static_cast<uint32_t>(r.v58));
    if (f & kSufSpellNameText) a.push_back(r.spellNameText);
    if (f & kSufItem) {
        a.push_back(num(r.v58));
        a.push_back(res.itemName.empty() ? nil : res.itemName);
    }
    if (f & kSufAuraType) a.push_back(r.v5c != 0 ? "DEBUFF" : "BUFF");
    if (f & kSufAuraAmount) a.push_back(num(r.v60));
    return a;
}

// ---------------------------------------------------------------------------
// The spell checks the builders make (0x751C40 and its siblings).
// ---------------------------------------------------------------------------
struct SpellRules {
    bool known = false;     ///< in Spell.dbc with a name
    uint32_t attr = 0, attrEx2 = 0, attrEx3 = 0, attrEx4 = 0, attrEx6 = 0, attrEx7 = 0;
    uint32_t stackAmount = 0;
    uint32_t effects[3] = {0, 0, 0};
    float speed = 0.0f;
    /// The caster's cast time for it is not zero (the unit's virtual
    /// GetSpellCastTime, 0x71AD20: SpellCastTimes from CastingTimeIndex, or
    /// "forever" for SPELL_ATTR0 0x2).
    bool castTime = false;
};

/// Logged at all: known, named, and neither SPELL_ATTR0_DO_NOT_DISPLAY (0x80)
/// nor SPELL_ATTR0_DO_NOT_LOG (0x100).
inline bool loggable(const SpellRules& s) { return s.known && (s.attr & 0x180u) == 0; }

/// 0x74D3F0: a cast that is not written as CAST_START / CAST_SUCCESS - not
/// loggable, a learn-spell effect (36), or SPELL_ATTR4_NO_CAST_LOG (0x1).
inline bool castNotLogged(const SpellRules& s) {
    if (!loggable(s)) return true;
    for (uint32_t e : s.effects) if (e == 36) return true;
    return (s.attrEx4 & 0x1u) != 0;
}

/// SPELL_CAST_START from SMSG_SPELL_START (0x806700 -> 0x751920): not for a
/// triggered cast (CAST_FLAG_PENDING 0x1), only for a spell with a cast time.
inline bool logsCastStart(uint32_t castFlags, const SpellRules& s) {
    return (castFlags & 0x1u) == 0 && !castNotLogged(s) && s.castTime;
}

inline bool hasEffect(const SpellRules& s, uint32_t effect) {
    for (uint32_t e : s.effects) if (e == effect) return true;
    return false;
}

/// SPELL_CAST_SUCCESS from SMSG_SPELL_GO (0x80E1B0 -> 0x7519E0): not for a
/// triggered cast unless SPELL_ATTR7 0x80000000, never for an open-lock
/// effect (33), and only for a spell with no cast time - one with a cast time
/// was written as SPELL_CAST_START.
inline bool logsCastSuccess(uint32_t castFlags, const SpellRules& s) {
    if ((castFlags & 0x1u) != 0 && (s.attrEx7 & 0x80000000u) == 0) return false;
    return !castNotLogged(s) && !hasEffect(s, 33) && !s.castTime;
}

/// The misses in SMSG_SPELL_GO's list are written at once for a spell with
/// no missile (Speed 0) and without SPELL_ATTR2 0x80.
inline bool logsGoMisses(const SpellRules& s) {
    return s.speed == 0.0f && (s.attrEx2 & 0x80u) == 0;
}

/// SPELL_ATTR3_NORMAL_RANGED_ATTACK: Auto Shot, Shoot, Throw log as RANGE_*.
inline bool rangedSpell(const SpellRules& s) { return (s.attrEx3 & 0x8000u) != 0; }
/// SPELL_ATTR3_TREAT_AS_PERIODIC: a heal or energize logs as the periodic one.
inline bool treatAsPeriodic(const SpellRules& s) { return (s.attrEx3 & 0x2000000u) != 0; }

// ---------------------------------------------------------------------------
// Builders. Each answers nothing when the stock client writes nothing.
// ---------------------------------------------------------------------------

/// SMSG_ATTACKERSTATEUPDATE (0x7512B0).
struct SwingInput {
    uint64_t attacker = 0, victim = 0;
    uint32_t hitInfo = 0;
    uint32_t victimState = 0;
    int32_t damage = 0;
    int32_t overkill = 0;
    uint32_t firstSchool = 0;   ///< the first sub-damage's school mask
    int32_t absorbed = 0;       ///< summed over the (at most two) sub-damages
    int32_t resisted = 0;
    int32_t blocked = 0;
};

inline std::optional<Record> buildSwing(const SwingInput& in) {
    Record r;
    r.src = in.attacker;
    r.dst = in.victim;
    if ((in.victimState == 1 || in.victimState == 5) && in.damage != 0) {
        r.event = SWING_DAMAGE;
        r.suffix = kSufDamage;
        r.v5c = in.damage;
        r.v60 = in.overkill;
        r.v64 = static_cast<int32_t>(in.firstSchool);
        r.v68 = in.absorbed;
        r.v6c = in.resisted;
        r.v70 = in.blocked;
        r.hitBits = ((in.hitInfo >> 9) & 1) | (((in.hitInfo >> 16) & 1) << 1) |
                    (((in.hitInfo >> 17) & 1) << 2);
        return r;
    }
    r.event = SWING_MISSED;
    r.suffix = kSufMissType;
    if (in.hitInfo & 0x10) { r.v58 = 1; return r; }                     // MISS
    if (in.hitInfo & 0x20) {                                             // full absorb
        r.v58 = 10; r.suffix |= kSufAbsorbedAmt; r.v68 = in.absorbed; return r;
    }
    if (in.hitInfo & 0x80) {                                             // full resist
        r.v58 = 2; r.suffix |= kSufResistedAmt; r.v6c = in.resisted; return r;
    }
    switch (in.victimState) {
        case 2: r.v58 = 3; return r;                                     // DODGE
        case 3: r.v58 = 4; return r;                                     // PARRY
        case 5: r.v58 = 5; r.suffix |= kSufBlockedAmt; r.v70 = in.blocked; return r;
        case 6: r.v58 = 6; return r;                                     // EVADE
        case 7: r.v58 = 7; return r;                                     // IMMUNE
        case 8: r.v58 = 9; return r;                                     // DEFLECT
        default: return std::nullopt;
    }
}

/// A spell's damage, from SMSG_SPELLNONMELEEDAMAGELOG (0x6D3840) and the
/// periodic log (0x7531F0), both through 0x751C40.
struct SpellDamageInput {
    uint64_t caster = 0, victim = 0;
    uint32_t spellId = 0;
    int32_t damage = 0, overkill = 0;
    uint32_t school = 0;
    int32_t absorbed = 0, resisted = 0, blocked = 0;
    bool crit = false;
    bool periodic = false;
    bool split = false;  ///< SPELL_HIT_TYPE 0x8 (0x752DE0): DAMAGE_SPLIT
};

inline std::optional<Record> buildSpellDamage(const SpellDamageInput& in, const SpellRules& s) {
    if (!loggable(s)) return std::nullopt;
    Record r;
    r.src = in.caster;
    r.dst = in.victim;
    r.spellId = in.spellId;
    if (in.split) {
        r.event = DAMAGE_SPLIT;
    } else if (in.damage != 0) {
        r.event = in.periodic ? SPELL_PERIODIC_DAMAGE : (rangedSpell(s) ? RANGE_DAMAGE : SPELL_DAMAGE);
    }
    if (in.split || in.damage != 0) {
        r.suffix = kSufDamage;
        r.v5c = in.damage;
        r.v60 = in.overkill;
        r.v64 = static_cast<int32_t>(in.school);
        r.v68 = in.absorbed;
        r.v6c = in.resisted;
        r.v70 = in.blocked;
        r.hitBits = in.crit ? 1u : 0u;
        return r;
    }
    // Nothing landed: absorbed, else blocked, else resisted - else nothing.
    r.event = in.periodic ? SPELL_PERIODIC_MISSED : (rangedSpell(s) ? RANGE_MISSED : SPELL_MISSED);
    r.suffix = kSufMissType;
    if (in.absorbed != 0) { r.v58 = 10; r.suffix |= kSufAbsorbedAmt; r.v68 = in.absorbed; return r; }
    if (in.blocked != 0)  { r.v58 = 5;  r.suffix |= kSufBlockedAmt;  r.v70 = in.blocked;  return r; }
    if (in.resisted != 0) { r.v58 = 2;  r.suffix |= kSufResistedAmt; r.v6c = in.resisted; return r; }
    return std::nullopt;
}

/// A miss with no amount (0x750500 via 0x751B80): SMSG_SPELLLOGMISS, the miss
/// list of SMSG_SPELL_GO, SMSG_SPELLORDAMAGE_IMMUNE. NONE writes nothing.
inline std::optional<Record> buildSpellMiss(uint64_t caster, uint64_t victim, uint32_t spellId,
                                            uint32_t missInfo, const SpellRules& s) {
    if (missInfo == 0 || missInfo > 11 || !loggable(s)) return std::nullopt;
    Record r;
    r.event = rangedSpell(s) ? RANGE_MISSED : SPELL_MISSED;
    r.src = caster;
    r.dst = victim;
    r.spellId = spellId;
    r.suffix = kSufMissType;
    r.v58 = static_cast<int32_t>(missInfo);
    // 0x74D980: the three kinds that carry an amount carry it even when it is 0.
    if (missInfo == 2) r.suffix |= kSufResistedAmt;
    else if (missInfo == 5) r.suffix |= kSufBlockedAmt;
    else if (missInfo == 10) r.suffix |= kSufAbsorbedAmt;
    return r;
}

/// SMSG_SPELLHEALLOG (0x6D3C30) and a periodic heal tick (0x7531F0), through
/// 0x751F70: nothing when amount, overhealing and absorbed are all zero.
inline std::optional<Record> buildHeal(uint64_t caster, uint64_t target, uint32_t spellId,
                                       int32_t amount, int32_t overheal, int32_t absorbed,
                                       bool crit, bool periodic, const SpellRules& s) {
    if (amount == 0 && overheal == 0 && absorbed == 0) return std::nullopt;
    if (!loggable(s)) return std::nullopt;
    Record r;
    r.event = (periodic || treatAsPeriodic(s)) ? SPELL_PERIODIC_HEAL : SPELL_HEAL;
    r.src = caster;
    r.dst = target;
    r.spellId = spellId;
    r.suffix = kSufHeal;
    r.v58 = amount;
    r.v5c = overheal;
    r.v60 = absorbed;
    r.v64 = crit ? 1 : 0;
    return r;
}

/// The divisor a power is shown with (0x7FDE00): rage and runic power in tenths.
inline int32_t powerDivisor(uint32_t powerType) { return (powerType == 1 || powerType == 6) ? 10 : 1; }

/// SMSG_SPELLENERGIZELOG (0x6D3DF0) and a periodic energize (0x752040).
inline std::optional<Record> buildEnergize(uint64_t caster, uint64_t target, uint32_t spellId,
                                           uint32_t powerType, int32_t amount, bool periodic,
                                           const SpellRules& s) {
    if (amount == 0 || !loggable(s)) return std::nullopt;
    Record r;
    r.event = (periodic || treatAsPeriodic(s)) ? SPELL_PERIODIC_ENERGIZE : SPELL_ENERGIZE;
    r.src = caster;
    r.dst = target;
    r.spellId = spellId;
    r.suffix = kSufEnergize;
    r.v58 = amount / powerDivisor(powerType);
    r.v5c = static_cast<int32_t>(powerType);
    return r;
}

/// A drain or leech (0x752100): from the periodic log's mana leech and from
/// SMSG_SPELLLOGEXECUTE's power drain and burn. Happiness (4) is not logged.
inline std::optional<Record> buildDrain(uint64_t caster, uint64_t target, uint32_t spellId,
                                        uint32_t powerType, uint32_t amount, float multiplier,
                                        bool periodic, const SpellRules& s) {
    if (amount == 0 || powerType == 4 || !loggable(s)) return std::nullopt;
    // 0x577370: the gain, rounded.
    const double gained = static_cast<double>(amount) * static_cast<double>(multiplier);
    const int32_t extra = static_cast<int32_t>(gained >= 0 ? gained + 0.5 : gained - 0.5);
    Record r;
    r.event = extra == 0 ? (periodic ? SPELL_PERIODIC_DRAIN : SPELL_DRAIN)
                         : (periodic ? SPELL_PERIODIC_LEECH : SPELL_LEECH);
    r.src = caster;
    r.dst = target;
    r.spellId = spellId;
    r.suffix = kSufDrain;
    const int32_t div = powerDivisor(powerType);
    r.v58 = static_cast<int32_t>(amount) / div;
    r.v5c = static_cast<int32_t>(powerType);
    r.v60 = extra / div;
    return r;
}

/// SMSG_ENVIRONMENTALDAMAGELOG (0x756800 -> 0x755270 -> 0x751150).
inline std::optional<Record> buildEnvironmental(uint64_t victim, uint32_t type, int32_t damage,
                                                int32_t absorbed, int32_t resisted) {
    if (damage == 0 && absorbed == 0 && resisted == 0) return std::nullopt;
    Record r;
    r.event = ENVIRONMENTAL_DAMAGE;
    r.dst = victim;
    r.suffix = kSufEnvironment | kSufDamage;
    r.v58 = static_cast<int32_t>(type);
    r.v5c = damage;
    r.v60 = 0;
    r.v64 = static_cast<int32_t>(environmentalSchool(type));
    r.v68 = absorbed;
    r.v6c = resisted;
    r.v70 = 0;
    return r;
}

/// SMSG_SPELLDAMAGESHIELD (0x6D3750 -> 0x752C90): the shield's owner hits back.
inline std::optional<Record> buildDamageShield(uint64_t owner, uint64_t attacker, uint32_t spellId,
                                               int32_t damage, int32_t overkill, uint32_t school,
                                               const SpellRules& s) {
    if (!loggable(s)) return std::nullopt;
    Record r;
    r.event = DAMAGE_SHIELD;
    r.src = owner;
    r.dst = attacker;
    r.spellId = spellId;
    r.suffix = kSufDamage;
    r.v5c = damage;
    r.v60 = overkill;
    r.v64 = static_cast<int32_t>(school);
    return r;
}

/// An event that is the spell prefix and nothing more (INSTAKILL 0x752210,
/// RESURRECT 0x752340, SUMMON/CREATE 0x7522A0, DURABILITY_DAMAGE_ALL 0x752680).
inline std::optional<Record> buildPlain(int event, uint64_t caster, uint64_t target,
                                        uint32_t spellId, const SpellRules& s) {
    if (!loggable(s)) return std::nullopt;
    Record r;
    r.event = event;
    r.src = caster;
    r.dst = target;
    r.spellId = spellId;
    return r;
}

/// SPELL_INTERRUPT (0x7523D0): the interrupted spell must be loggable too.
inline std::optional<Record> buildInterrupt(uint64_t caster, uint64_t target, uint32_t spellId,
                                            uint32_t interrupted, const SpellRules& s,
                                            const SpellRules& interruptedRules) {
    if (!loggable(s) || !loggable(interruptedRules)) return std::nullopt;
    Record r;
    r.event = SPELL_INTERRUPT;
    r.src = caster;
    r.dst = target;
    r.spellId = spellId;
    r.suffix = kSufExtraSpell;
    r.v58 = static_cast<int32_t>(interrupted);
    return r;
}

/// SPELL_EXTRA_ATTACKS (0x752520).
inline std::optional<Record> buildExtraAttacks(uint64_t caster, uint64_t target, uint32_t spellId,
                                               int32_t count, const SpellRules& s) {
    if (!loggable(s) || count == 0) return std::nullopt;
    Record r;
    r.event = SPELL_EXTRA_ATTACKS;
    r.src = caster;
    r.dst = target;
    r.spellId = spellId;
    r.suffix = kSufAmount;
    r.v58 = count;
    return r;
}

/// SPELL_DURABILITY_DAMAGE (0x7525E0): the item named.
inline std::optional<Record> buildDurabilityDamage(uint64_t caster, uint64_t target,
                                                   uint32_t spellId, uint32_t itemId,
                                                   const SpellRules& s) {
    if (!loggable(s)) return std::nullopt;
    Record r;
    r.event = SPELL_DURABILITY_DAMAGE;
    r.src = caster;
    r.dst = target;
    r.spellId = spellId;
    r.suffix = kSufItem;
    r.v58 = static_cast<int32_t>(itemId);
    return r;
}

/// SPELL_DISPEL / SPELL_STOLEN (0x752930), one per aura taken.
inline std::optional<Record> buildDispel(bool stolen, uint64_t caster, uint64_t victim,
                                         uint32_t spellId, uint32_t dispelled, bool debuff,
                                         const SpellRules& s, const SpellRules& dispelledRules) {
    if (!loggable(s) || !loggable(dispelledRules)) return std::nullopt;
    Record r;
    r.event = stolen ? SPELL_STOLEN : SPELL_DISPEL;
    r.src = caster;
    r.dst = victim;
    r.spellId = spellId;
    r.suffix = kSufExtraSpell | kSufAuraType;
    r.v58 = static_cast<int32_t>(dispelled);
    r.v5c = debuff ? 1 : 0;
    return r;
}

/// SPELL_AURA_BROKEN / SPELL_AURA_BROKEN_SPELL (0x752A40): the aura is the
/// prefix, the spell that broke it (if any) the extra spell. A breaking spell
/// with SPELL_ATTR4_DO_NOT_LOG_CASTER (0x2000) hides the source.
inline std::optional<Record> buildAuraBroken(uint64_t caster, uint64_t victim, uint32_t auraSpell,
                                             uint32_t breakingSpell, bool debuff,
                                             const SpellRules& auraRules,
                                             const SpellRules& breakingRules) {
    if (breakingSpell != 0 && !loggable(breakingRules)) return std::nullopt;
    if (!loggable(auraRules)) return std::nullopt;
    Record r;
    r.event = breakingSpell != 0 ? SPELL_AURA_BROKEN_SPELL : SPELL_AURA_BROKEN;
    r.src = (breakingRules.attrEx4 & 0x2000u) ? 0 : caster;
    r.dst = victim;
    r.spellId = auraSpell;
    if (breakingSpell != 0) {
        r.suffix |= kSufExtraSpell;
        r.v58 = static_cast<int32_t>(breakingSpell);
    }
    r.suffix |= kSufAuraType;
    r.v5c = debuff ? 1 : 0;
    return r;
}

/// SPELL_DISPEL_FAILED (0x750EA0), one per spell that held.
inline std::optional<Record> buildDispelFailed(uint64_t caster, uint64_t victim, uint32_t spellId,
                                               uint32_t failed, const SpellRules& s,
                                               const SpellRules& failedRules) {
    if (!loggable(s) || !loggable(failedRules)) return std::nullopt;
    Record r;
    r.event = SPELL_DISPEL_FAILED;
    r.src = caster;
    r.dst = victim;
    r.spellId = spellId;
    r.suffix = kSufExtraSpell;
    r.v58 = static_cast<int32_t>(failed);
    return r;
}

/// The aura events from an incremental SMSG_AURA_UPDATE (0x72F5D0 ->
/// 0x752710, 0x752860, 0x752BA0). Never from the full list.
///
/// Applied always; removed only while the unit is alive; passives
/// (SPELL_ATTR0_PASSIVE 0x40) and SPELL_ATTR6_NO_AURA_LOG (0x400) never.
inline std::optional<Record> buildAuraChange(bool applied, uint64_t caster, uint64_t unit,
                                             uint32_t spellId, bool debuff, bool unitAlive,
                                             const SpellRules& s) {
    if (!loggable(s) || (s.attr & 0x40u) || (s.attrEx6 & 0x400u)) return std::nullopt;
    if (!applied && !unitAlive) return std::nullopt;
    Record r;
    r.event = applied ? SPELL_AURA_APPLIED : SPELL_AURA_REMOVED;
    r.src = caster;
    r.dst = unit;
    r.spellId = spellId;
    r.suffix = kSufAuraType;
    r.v5c = debuff ? 1 : 0;
    return r;
}

/// A stack change (0x752860): only for spells that stack (StackAmount > 1).
inline std::optional<Record> buildAuraDose(uint64_t caster, uint64_t unit, uint32_t spellId,
                                           uint32_t oldCount, uint32_t newCount, bool debuff,
                                           const SpellRules& s) {
    if (!loggable(s) || s.stackAmount <= 1) return std::nullopt;
    Record r;
    r.event = newCount <= oldCount ? SPELL_AURA_REMOVED_DOSE : SPELL_AURA_APPLIED_DOSE;
    r.src = caster;
    r.dst = unit;
    r.spellId = spellId;
    r.suffix = kSufAuraType | kSufAuraAmount;
    r.v5c = debuff ? 1 : 0;
    r.v60 = static_cast<int32_t>(newCount);
    return r;
}

/// The same aura put on again (0x752BA0).
inline std::optional<Record> buildAuraRefresh(uint64_t caster, uint64_t unit, uint32_t spellId,
                                              bool debuff, const SpellRules& s) {
    if (!loggable(s) || (s.attr & 0x40u) || (s.attrEx6 & 0x400u)) return std::nullopt;
    Record r;
    r.event = SPELL_AURA_REFRESH;
    r.src = caster;
    r.dst = unit;
    r.spellId = spellId;
    r.suffix = kSufAuraType;
    r.v5c = debuff ? 1 : 0;
    return r;
}

// ---------------------------------------------------------------------------
// Unit flags (0x74DCB0).
// ---------------------------------------------------------------------------
inline bool isPlayerGuid(uint64_t g) {
    const auto hi = static_cast<uint32_t>(g >> 32);
    return (hi & 0xF0000000u) == 0 && g != 0;
}
inline uint32_t guidHighType(uint64_t g) { return static_cast<uint32_t>(g >> 32) & 0xF0F00000u; }

/// What 0x74DCB0 asks of the world about one unit.
struct UnitFacts {
    uint64_t guid = 0;
    uint64_t me = 0;
    bool objectKnown = false;      ///< the client has the object
    bool isUnit = false;           ///< ... and it is a unit (reaction is asked of it)
    uint64_t controller = 0;       ///< resolved as 0x74DCB0 does (self when nobody)
    bool charmedOrSummoned = false;///< the unit's CHARMEDBY or SUMMONEDBY is set
    int reaction = -1;             ///< 0 hated .. 7 exalted, toward the player; -1 unknown
    bool inGroup = false, inRaid = false;
    bool controllerInParty = false, controllerInRaid = false;
    bool isTarget = false, isFocus = false;
    uint8_t roleFlags = 0;         ///< the player's group member flags: 2 main tank, 4 main assist
    int raidIcon = -1;
};

inline uint32_t unitFlags(const UnitFacts& u) {
    if (u.guid == 0) return 0x80000000u;
    const uint64_t g = u.guid, c = u.controller;
    const uint32_t ht = guidHighType(g);
    uint32_t f = 0;
    if (isPlayerGuid(g) || ht == 0xF0300000u || ht == 0xF0500000u || ht == 0xF0400000u) {
        if (c == g) f = isPlayerGuid(g) ? 0x400u : (ht == 0xF0400000u ? 0x1000u : 0x800u);
        else f = u.charmedOrSummoned ? 0x1000u : 0x2000u;
    } else {
        f = 0x4000u;
    }
    f |= isPlayerGuid(c) ? 0x100u : 0x200u;
    if (c == u.me && c != 0) {
        f |= 0x11u;
    } else {
        bool partyCheck = true;
        if (u.isUnit && u.reaction >= 0) {
            if (u.reaction > 3) f |= 0x10u;
            else if (u.reaction <= 1) { f |= 0x40u; partyCheck = false; }
        }
        if (partyCheck && u.inGroup) {
            if (u.controllerInParty) f |= 0x12u;
            else if (u.inRaid && u.controllerInRaid) f |= 0x14u;
        }
        if ((f & 0xFu) == 0) f |= 0x8u;
        if ((f & 0xF0u) == 0) f |= 0x20u;
    }
    if (u.isTarget) f |= 0x10000u;
    if (u.isFocus) f |= 0x20000u;
    bool roles = (f & 0x400u) != 0;
    if (roles && (f & 0x6u) == 0) {
        if (c != u.me) roles = false;
        else if (!u.inGroup) return f;
    }
    if (roles) {
        if (u.roleFlags & 2) f |= 0x40000u;
        else if (u.roleFlags & 4) f |= 0x80000u;
    }
    if (u.inGroup && u.raidIcon >= 0 && u.raidIcon < 8) f |= 0x100000u << u.raidIcon;
    return f;
}

/// CombatLog_Object_IsA (0x74D600) and the filters' mask test (0x74D1A0):
/// a special bit in common is enough; otherwise all four of affiliation,
/// reaction, control and type must have a bit in common.
inline bool objectIsA(uint32_t flags, uint32_t mask) {
    const uint32_t m = flags & mask;
    if (m & 0xFFFF0000u) return true;
    return (m & 0xFu) && (m & 0xF0u) && (m & 0x300u) && (m & 0xFC00u);
}

/// CombatLogAddFilter's check of a numeric mask (0x74FF70): no special bit
/// and a category with nothing in it is "incomplete" and refused.
inline bool maskComplete(uint32_t mask) {
    if (mask & 0xFFFF0000u) return true;
    return (mask & 0xFu) && (mask & 0xF0u) && (mask & 0x300u) && (mask & 0xFC00u);
}

/// "0x..." or bare hex, up to sixteen digits (0x74D120).
inline uint64_t parseGuid(std::string_view s) {
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s.remove_prefix(2);
    uint64_t v = 0;
    for (size_t i = 0; i < s.size() && i < 16; ++i) {
        const char ch = s[i];
        uint32_t d;
        if (ch >= '0' && ch <= '9') d = static_cast<uint32_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = static_cast<uint32_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') d = static_cast<uint32_t>(ch - 'A' + 10);
        else break;
        v = (v << 4) | d;
    }
    return v;
}

// ---------------------------------------------------------------------------
// Filters and the history.
// ---------------------------------------------------------------------------
struct Filter {
    std::array<uint32_t, 2> events{0xFFFFFFFFu, 0xFFFFFFFFu};
    uint64_t srcGuid = 0;
    uint32_t srcMask = 0xFFFFFFFFu;
    uint64_t dstGuid = 0;
    uint32_t dstMask = 0xFFFFFFFFu;
    uint32_t spellId = 0;
    std::string spellName;  ///< matched when spellId is 0 and this is not empty
};

/// The event list (0x74E050): nil is every event; otherwise the names split
/// on spaces and commas, and an unknown name matches nothing.
inline void setFilterEvents(Filter& f, const char* list) {
    if (!list) { f.events = {0xFFFFFFFFu, 0xFFFFFFFFu}; return; }
    f.events = {0, 0};
    std::string_view s(list);
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == ',')) ++i;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != ',') ++j;
        if (j > i) {
            const int e = eventIndex(s.substr(i, j - i));
            if (e >= 0) f.events[static_cast<size_t>(e) >> 5] |= 1u << (e & 31);
        }
        i = j;
    }
}

/// What a filter is matched against, read once from the entry.
struct EntryKey {
    int event = -1;
    uint64_t src = 0, dst = 0;
    uint32_t srcFlags = 0, dstFlags = 0;
    uint32_t spellId = 0;
    std::string spellName;
};

/// 0x74E1A0.
inline bool filterMatches(const Filter& f, const EntryKey& e) {
    if (e.event < 0 || e.event >= kEventCount) return false;
    if ((f.events[static_cast<size_t>(e.event) >> 5] & (1u << (e.event & 31))) == 0) return false;
    if (f.srcGuid != 0) { if (e.src != f.srcGuid) return false; }
    else if (!objectIsA(e.srcFlags, f.srcMask)) return false;
    if (f.dstGuid != 0) { if (e.dst != f.dstGuid) return false; }
    else if (!objectIsA(e.dstFlags, f.dstMask)) return false;
    if (f.spellId != 0) return e.spellId == f.spellId;
    if (!f.spellName.empty()) return !e.spellName.empty() && e.spellName == f.spellName;
    return true;
}

/// Whether an event's arguments start with the spell prefix after the units.
inline bool hasSpellPrefix(int e) {
    switch (e) {
        case ENVIRONMENTAL_DAMAGE: case SWING_DAMAGE: case SWING_MISSED:
        case ENCHANT_APPLIED: case ENCHANT_REMOVED: case PARTY_KILL:
        case UNIT_DIED: case UNIT_DESTROYED: case UNIT_DISSIPATES:
            return false;
        default:
            return e >= 0 && e < kEventCount;
    }
}

inline EntryKey keyFromArgs(const std::vector<std::string>& a) {
    EntryKey k;
    if (a.size() < 8) return k;
    k.event = eventIndex(a[1]);
    k.src = parseGuid(a[2]);
    k.srcFlags = static_cast<uint32_t>(std::strtoul(a[4].c_str(), nullptr, 10));
    k.dst = parseGuid(a[5]);
    k.dstFlags = static_cast<uint32_t>(std::strtoul(a[7].c_str(), nullptr, 10));
    if (hasSpellPrefix(k.event) && a.size() >= 10) {
        k.spellId = static_cast<uint32_t>(std::strtoul(a[8].c_str(), nullptr, 10));
        if (a[9] != kEventNil) k.spellName = a[9];
    }
    return k;
}

/// The history (the entry list at 0xADB97C) with the filter list (0xADB970)
/// and the cursor CombatLogGetCurrentEntry reads (0xCA1390).
class History {
public:
    struct Entry {
        std::vector<std::string> args;
        EntryKey key;
        int64_t tickMs = 0;
    };

    /// 0x750400: at most one expired entry is let go per new one.
    void add(std::vector<std::string> args, int64_t nowMs) {
        if (!entries_.empty() && nowMs - retentionSec_ * 1000LL - entries_.front().tickMs >= 0) {
            entries_.pop_front();
            if (cursor_ > 0) --cursor_;
            else if (cursor_ == 0 && entries_.empty()) cursor_ = -1;
        }
        Entry e;
        e.key = keyFromArgs(args);
        e.args = std::move(args);
        e.tickMs = nowMs;
        entries_.push_back(std::move(e));
        // Not in the stock client, which keeps whatever arrives within the
        // retention time: a ceiling so a runaway cannot take the memory.
        if (entries_.size() > kHardCap) {
            entries_.pop_front();
            if (cursor_ > 0) --cursor_;
        }
    }

    /// 0x74F6C0: no filters lets everything through.
    bool passes(const EntryKey& k) const {
        if (filters_.empty()) return true;
        for (const auto& f : filters_) if (filterMatches(f, k)) return true;
        return false;
    }
    bool passes(const Entry& e) const { return passes(e.key); }

    void resetFilters() { filters_.clear(); }
    void addFilter(Filter f) { filters_.push_back(std::move(f)); }
    size_t filterCount() const { return filters_.size(); }

    int numEntries(bool ignoreFilter) const {
        int n = 0;
        for (const auto& e : entries_) if (ignoreFilter || passes(e)) ++n;
        return n;
    }

    /// 0x74FAE0. index >= 1 counts matching entries from the oldest; 0 is the
    /// newest matching, -1 the one before it. False clears the cursor.
    bool setCurrent(int index, bool ignoreFilter) {
        cursor_ = -1;
        const int n = static_cast<int>(entries_.size());
        if (index < 1) {
            for (int i = n - 1; i >= 0; --i) {
                if (!ignoreFilter && !passes(entries_[static_cast<size_t>(i)])) continue;
                if (index == 0) { cursor_ = i; return true; }
                ++index;
            }
        } else {
            for (int i = 0; i < n; ++i) {
                if (!ignoreFilter && !passes(entries_[static_cast<size_t>(i)])) continue;
                if (--index == 0) { cursor_ = i; return true; }
            }
        }
        return false;
    }

    /// 0x74FC20: count matching entries forward (newer) or back (older) from
    /// the current one, which itself counts when it matches. Falling off
    /// either end clears the cursor.
    bool advance(int count, bool ignoreFilter) {
        if (count == 0) return cursor_ >= 0;
        if (cursor_ < 0) return false;
        const int n = static_cast<int>(entries_.size());
        const int step = count > 0 ? 1 : -1;
        int remaining = count > 0 ? count : -count;
        for (int i = cursor_; i >= 0 && i < n; i += step) {
            if (!ignoreFilter && !passes(entries_[static_cast<size_t>(i)])) continue;
            if (remaining == 0) { cursor_ = i; return true; }
            --remaining;
        }
        cursor_ = -1;
        return false;
    }

    const Entry* current() const {
        return (cursor_ >= 0 && cursor_ < static_cast<int>(entries_.size()))
                   ? &entries_[static_cast<size_t>(cursor_)] : nullptr;
    }

    void clear() { entries_.clear(); cursor_ = -1; }

    void setRetentionSeconds(int s) { retentionSec_ = s; }
    int retentionSeconds() const { return retentionSec_; }
    size_t size() const { return entries_.size(); }

private:
    static constexpr size_t kHardCap = 100000;
    std::deque<Entry> entries_;
    std::vector<Filter> filters_;
    int cursor_ = -1;
    int retentionSec_ = 300;
};

}  // namespace stock_combat_log
}  // namespace game
}  // namespace wowee
