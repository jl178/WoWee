// lua_unit_api.cpp - Unit query, stats, party/raid, and player state Lua API bindings.
// Extracted from lua_engine.cpp as part of §5.1 (Tame LuaEngine).
#include "game/threat_standing.hpp"
#include "game/vehicle_rules.hpp"
#include "game/shapeshift_forms.hpp"
#include "addons/lua_api_helpers.hpp"

namespace wowee::addons {

static int lua_UnitName(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    // No game object branch here either, for the reason in UnitExists: a name
    // is the one answer about an object that would have been right, and giving
    // it while UnitExists says no is a unit that half exists.
    if (unit && !unit->getName().empty()) {
        lua_pushstring(L, unit->getName().c_str());
    } else {
        // Fallback: party member name for out-of-range members
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        if (pm && !pm->name.empty()) {
            lua_pushstring(L, pm->name.c_str());
        } else if (gh && guid != 0) {
            // Try player name cache
            const std::string& cached = gh->lookupName(guid);
            if (!cached.empty()) {
                lua_pushstring(L, cached.c_str());
            } else {
                // The player's own name, from the list they picked it off.
                //
                // "Unknown" is a real answer for a stranger whose name query
                // has not come back, and a wrong one for the player: the
                // client knows who they are before it knows anything else -
                // world_loader reads this very name to set up per-character
                // SavedVariables, two lines before it loads the addons.
                //
                // Addons ask at load and keep the answer. Bagnon takes
                // UnitName('player') into a local at file scope and compares
                // every later answer against it to decide whether a bag is the
                // live one or a remembered one; given "Unknown" once, it spent
                // the session certain it was looking at an offline character
                // and drew its bags out of an empty database. No error, no
                // warning, an empty window.
                std::string own;
                if (guid == gh->getPlayerGuid()) {
                    for (const auto& c : gh->getCharacters()) {
                        if (c.guid == guid) { own = c.name; break; }
                    }
                }
                lua_pushstring(L, own.empty() ? "Unknown" : own.c_str());
            }
        } else {
            // A token naming nobody - no target, an empty party slot - is
            // nil, nil (0x60E740), not UNKNOWNOBJECT: "Unknown" is the answer
            // for a unit that exists and whose name has not arrived.
            lua_pushnil(L);
            lua_pushnil(L);
            return 2;
        }
    }
    // The realm, nil for one's own (0x60E740 pushes the record's +0x34, empty
    // on a single realm).
    lua_pushnil(L);
    return 2;
}


static int lua_UnitHealth(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushnumber(L, unit->getHealth());
    } else {
        // Fallback: party member stats for out-of-range members
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        lua_pushnumber(L, pm ? pm->curHealth : 0);
    }
    return 1;
}

static int lua_UnitHealthMax(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushnumber(L, unit->getMaxHealth());
    } else {
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        lua_pushnumber(L, pm ? pm->maxHealth : 0);
    }
    return 1;
}

/// UnitPower(unit, powerType) - the second argument names *which* bar.
///
/// Absent, it means the one the unit is using, which is what the main power
/// bar wants. Given, it names one of the seven, and alternatepowerbar.lua is
/// the caller that needs it: a druid in a form has energy or rage as its
/// current power and mana as an alternate, and that frame asks for the
/// alternate by index. Ignoring the argument answered with the current power,
/// so the mana bar shown beside a cat's energy bar was that same energy.
///
/// The seven are all tracked - Entity keeps powers[7] and maxPowers[7] - so
/// this is a matter of reading the one asked for.
///
/// Rage (1) and runic power (6) travel in UNIT_FIELD_POWER/MAXPOWER in tenths:
/// a warrior's full bar is 1000 on the wire. The stock client hands Lua the
/// whole points (0-100), and every FrameXML text and bar is written for that,
/// so the tenths are dropped here, the one place Lua reads power.
static uint32_t luaPowerValue(uint8_t powerType, uint32_t raw) {
    return (powerType == 1 || powerType == 6) ? raw / 10 : raw;
}

static int lua_UnitPower(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    const bool byType = lua_isnumber(L, 2);
    const uint8_t want = byType ? static_cast<uint8_t>(lua_tonumber(L, 2)) : 0;
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        const uint8_t type = byType ? want : unit->getPowerType();
        lua_pushnumber(L, luaPowerValue(type,
            byType ? unit->getPowerByType(want) : unit->getPower()));
    } else {
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        // Party stats carry one power and say which it is, so a request for a
        // different one is answered with nothing rather than with that.
        const bool same = pm && (!byType || pm->powerType == want);
        lua_pushnumber(L, same ? luaPowerValue(pm->powerType, pm->curPower) : 0);
    }
    return 1;
}

/// UnitPowerMax(unit, powerType) - the same, and the one that decides whether
/// the alternate bar is drawn at all: alternatepowerbar tests this against zero
/// before showing itself.
static int lua_UnitPowerMax(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    const bool byType = lua_isnumber(L, 2);
    const uint8_t want = byType ? static_cast<uint8_t>(lua_tonumber(L, 2)) : 0;
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        const uint8_t type = byType ? want : unit->getPowerType();
        lua_pushnumber(L, luaPowerValue(type,
            byType ? unit->getMaxPowerByType(want) : unit->getMaxPower()));
    } else {
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        const bool same = pm && (!byType || pm->powerType == want);
        lua_pushnumber(L, same ? luaPowerValue(pm->powerType, pm->maxPower) : 0);
    }
    return 1;
}

static int lua_UnitLevel(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        // Stock answers -1 ("??", the skull) for a unit you can fight that is
        // ten or more levels above you; friendly units always give their level.
        int level = static_cast<int>(unit->getLevel());
        if (auto* gh = getGameHandler(L)) {
            const int playerLevel = static_cast<int>(gh->getPlayerLevel());
            if (unit->isHostile() && playerLevel > 0 && level >= playerLevel + 10) level = -1;
            // World bosses always read ??.
            if (unit->getType() == game::ObjectType::UNIT && gh->getCreatureRank(unit->getEntry()) == 3) level = -1;
        }
        lua_pushnumber(L, level);
    } else {
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        lua_pushnumber(L, pm ? pm->level : 0);
    }
    return 1;
}

static int lua_UnitExists(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    // A game object is not a unit, and saying otherwise here does not stop at
    // the name. This client targets objects because its right-click path reads
    // the selection back, and its own target frame drew one greyed out and
    // asked nothing further. FrameXML asks a great deal further - dead, level,
    // classification, reaction - and every one of those reads the zero behind
    // an object that has no such field, so a targeted mailbox came up dead, at
    // skull level, with an attackable portrait. Retail never targets one at
    // all; the selection stays for the interaction path, and the interface is
    // told what it asked, which is that there is no unit here.
    if (unit) {
        lua_pushboolean(L, 1);
    } else {
        // Party members in other zones don't have entities but still "exist"
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        lua_pushboolean(L, guid != 0 && findPartyMember(gh, guid) != nullptr);
    }
    return 1;
}

static int lua_UnitIsDead(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushboolean(L, unit->getHealth() == 0);
    } else {
        // Fallback: party member stats for out-of-range members
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        // Out of view the member's status says so (0x60F480: the party
        // block's MEMBER_STATUS_DEAD, 0x04), not a health of zero - which is
        // also what a member nothing has been heard from yet holds.
        lua_pushboolean(L, pm ? (pm->onlineStatus & 0x0004) != 0 : 0);
    }
    return 1;
}

/// UnitIsFeignDeath(unit) - true for a member of the player's group who is
/// feigning death: the client asks only of party and raid members, and reads
/// UNIT_DYNAMIC_FLAGS' dead bit (0x20), which feigning sets while the health
/// stays up (0x0060F3D0).
static int lua_UnitIsFeignDeath(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    auto* unit = resolveUnit(L, uid);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    const uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
    const bool grouped = guid != 0 && findPartyMember(gh, guid) != nullptr;
    if (unit && grouped && (unit->getDynamicFlags() & 0x20) != 0 && unit->getHealth() > 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
    return luaReturnNil(L);
}

static int lua_UnitClass(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    auto* unit = resolveUnit(L, uid);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    // The player's class is known from the character list whether or not
    // the player's own unit is in the entity list: during a map change it is
    // not, and the character sheet, which updates on events while it is open,
    // asked here, got nothing and stopped at strupper(nil)
    // (PaperDollFrame.lua:269, PaperDollFrame_SetStat). WoW answers for the
    // player at all times.
    const bool selfKnown = gh && uidStr == "player" && gh->getPlayerClass() != 0;
    if ((unit || selfKnown) && gh) {

        uint8_t classId = 0;
        if (uidStr == "player") {
            classId = gh->getPlayerClass();
        } else {
            // Read class from UNIT_FIELD_BYTES_0 (class is byte 1)
            uint64_t guid = resolveUnitGuid(gh, uidStr);
            if (guid != 0) {
                auto entity = gh->getEntityManager().getEntity(guid);
                if (entity) {
                    uint32_t bytes0 = entity->getField(
                        game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
                    classId = static_cast<uint8_t>((bytes0 >> 8) & 0xFF);
                }
            }
            // Fallback: check name query class/race cache
            if (classId == 0 && guid != 0) {
                classId = gh->lookupPlayerClass(guid);
            }
        }
        // Nothing at all when the class is not known, because that is what WoW
        // answers for a unit it cannot see and what every caller is written
        // against: `local _, class = UnitClass(self.unit); if ( class ) then`.
        //
        // "UNKNOWN" passes that guard and then indexes nothing -
        // CLASS_ICON_TCOORDS has no such key - so the line after it unpacked a
        // nil. ArenaEnemyFrame_OnLoad does exactly this, which took the whole
        // of Blizzard_ArenaUI down at load. A binding that answers a constant
        // where the real one answers nothing defeats every guard written for
        // it, and the guard is the caller saying it already knows how to cope.
        //
        // The token decides: it is the one thing that knows which class ids
        // WoW actually uses, and the two callers in the social API ask it the
        // same question for the same reason.
        const char* token = luaClassToken(classId);
        if (!token) return 0;
        // Second is the uppercase token, not the display name again. Every
        // class-indexed table in FrameXML is keyed by it, so answering "Mage"
        // where "MAGE" was meant looked up nothing: SetPortraitTexture found no
        // CLASS_ICON_TCOORDS entry and fell back to the placeholder portrait,
        // which is the blue question mark on the character sheet.
        lua_pushstring(L, kLuaClasses[classId]);
        lua_pushstring(L, token);
        lua_pushnumber(L, classId);
        return 3;
    }
    // The unit does not exist, which is the case WoW answers nothing for. See
    // above: a constant here reads as a class the caller then looks up.
    return 0;
}

// UnitIsGhost(unit) - true if unit is in ghost form
static int lua_UnitIsGhost(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    if (uidStr == "player") {
        lua_pushboolean(L, gh->isPlayerGhost());
    } else {
        // Check UNIT_FIELD_FLAGS for UNIT_FLAG_GHOST (0x00000100) - best approximation
        uint64_t guid = resolveUnitGuid(gh, uidStr);
        bool ghost = false;
        if (guid != 0) {
            auto entity = gh->getEntityManager().getEntity(guid);
            if (entity) {
                // Ghost is PLAYER_FLAGS bit 0x10, NOT UNIT_FIELD_FLAGS bit 0x100
                // (which is UNIT_FLAG_IMMUNE_TO_PC - would flag immune NPCs as ghosts).
                uint32_t pf = entity->getField(game::fieldIndex(game::UF::PLAYER_FLAGS));
                ghost = (pf & 0x00000010) != 0;
            } else if (const auto* pm = findPartyMember(gh, guid)) {
                ghost = (pm->onlineStatus & 0x0008) != 0;  // MEMBER_STATUS_GHOST
            }
        }
        lua_pushboolean(L, ghost);
    }
    return 1;
}

// UnitIsDeadOrGhost(unit)
static int lua_UnitIsDeadOrGhost(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    auto* gh = getGameHandler(L);
    bool dead = (unit && unit->getHealth() == 0);
    if (!dead && gh) {
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        if (uidStr == "player") {
            dead = gh->isPlayerGhost() || gh->isPlayerDead();
        } else if (!unit) {
            // A member out of view: MEMBER_STATUS_DEAD or _GHOST.
            const auto* pm = findPartyMember(gh, resolveUnitGuid(gh, uidStr));
            dead = pm && (pm->onlineStatus & 0x000C) != 0;
        }
    }
    lua_pushboolean(L, dead);
    return 1;
}

// UnitIsAFK(unit), UnitIsDND(unit)
/// A unit predicate answered from one bit of PLAYER_FLAGS.
///
/// Answers 1 or nil, the way WoW answers a Unit predicate. bnet.lua tests
/// `UnitIsAFK("player") == 1`, which a boolean fails silently, and the two
/// chat sites test it for truth - 1 and nil satisfy both, where true/false
/// and 1/0 each break one of them.
///
/// PLAYER_FLAGS, not UNIT_FIELD_FLAGS: 0x01 there is UNIT_FLAG_SERVER_CONTROLLED
/// and has nothing to do with being away.
static int pushPlayerFlagPredicate(lua_State* L, uint32_t bit) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid != 0) {
        auto entity = gh->getEntityManager().getEntity(guid);
        if (entity) {
            uint32_t playerFlags = entity->getField(game::fieldIndex(game::UF::PLAYER_FLAGS));
            if (playerFlags & bit) lua_pushnumber(L, 1); else lua_pushnil(L);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

// PLAYER_FLAGS: 0x1 is the group leader, 0x2 AFK, 0x4 DND.
static int lua_UnitIsAFK(lua_State* L) {
    return pushPlayerFlagPredicate(L, 0x02);
}

static int lua_UnitIsDND(lua_State* L) {
    return pushPlayerFlagPredicate(L, 0x04);
}

// UnitPlayerControlled(unit) - true for players and player-controlled pets
static int lua_UnitPlayerControlled(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnFalse(L); }
    auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity) { return luaReturnFalse(L); }
    // Players are always player-controlled; pets check UNIT_FLAG_PLAYER_CONTROLLED
    if (entity->getType() == game::ObjectType::PLAYER) {
        lua_pushboolean(L, 1);
    } else {
        uint32_t flags = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_FLAGS));
        lua_pushboolean(L, (flags & game::UNIT_FLAG_PLAYER_CONTROLLED) != 0);
    }
    return 1;
}

// UnitIsTapped(unit) - true if mob is tapped (tagged by any player)
static int lua_UnitIsTapped(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "target");
    auto* unit = resolveUnit(L, uid);
    if (!unit) { return luaReturnFalse(L); }
    lua_pushboolean(L, (unit->getDynamicFlags() & 0x0004) != 0); // UNIT_DYNFLAG_TAPPED_BY_PLAYER
    return 1;
}

// UnitIsTappedByPlayer(unit) - true if tapped by the local player (can loot)
static int lua_UnitIsTappedByPlayer(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "target");
    auto* unit = resolveUnit(L, uid);
    if (!unit) { return luaReturnFalse(L); }
    uint32_t df = unit->getDynamicFlags();
    // Tapped by player: has TAPPED flag but also LOOTABLE or TAPPED_BY_ALL
    bool tapped = (df & 0x0004) != 0;
    bool lootable = (df & 0x0001) != 0;
    bool sharedTag = (df & 0x0008) != 0;
    lua_pushboolean(L, tapped && (lootable || sharedTag));
    return 1;
}

// UnitIsTappedByAllThreatList(unit) - true if shared-tag mob
static int lua_UnitIsTappedByAllThreatList(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "target");
    auto* unit = resolveUnit(L, uid);
    if (!unit) { return luaReturnFalse(L); }
    lua_pushboolean(L, (unit->getDynamicFlags() & 0x0008) != 0);
    return 1;
}

namespace {

using game::ThreatStanding;

bool threatStandingFor(game::GameHandler* gh, uint64_t unitGuid, uint64_t mobGuid,
                       ThreatStanding& out) {
    if (!gh || unitGuid == 0 || mobGuid == 0) return false;
    return game::threatStandingOn(gh->getThreatList(mobGuid), unitGuid, out);
}

}  // namespace

// UnitThreatSituation(unit, mobUnit) → 0=not tanking, 1=not tanking but threat, 2=insecurely tanking, 3=securely tanking
static int lua_UnitThreatSituation(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnZero(L); }
    const char* uid = luaL_optstring(L, 1, "player");
    const char* mobUid = luaL_optstring(L, 2, nullptr);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t playerUnitGuid = resolveUnitGuid(gh, uidStr);
    if (playerUnitGuid == 0) { return luaReturnZero(L); }
    uint64_t mobGuid = 0;
    if (mobUid && *mobUid) {
        std::string mStr(mobUid);
        toLowerInPlace(mStr);
        mobGuid = resolveUnitGuid(gh, mStr);
    }
    // The mob's own threat list first, which is what the server actually sent.
    // This used to go straight to the guess below because the list was built
    // from a misread packet and was never worth consulting - SMSG_THREAT_UPDATE
    // was read as though it carried the second guid that only
    // SMSG_HIGHEST_THREAT_UPDATE has.
    // No mob: the unit's standing on every list it is on, the highest of them,
    // and nil when it is on none - what the stock function (0x613a60) answers
    // from its per-unit table. This used to say 1 whenever the player was "in
    // combat" by the client's own guess (auto-attack or a remembered
    // attacker), so PlayerFrameFlash, which asks exactly this, glowed the
    // yellow of status 1 with no mob holding the player on any list - and
    // never the red of actually tanking.
    if (mobGuid == 0) {
        // Whoever is on a list is in combat: with the server's combat flag
        // down, any list still naming this unit is one whose clear was lost.
        auto unitEnt = gh->getEntityManager().getEntity(playerUnitGuid);
        if (!unitEnt ||
            (unitEnt->getField(game::fieldIndex(game::UF::UNIT_FIELD_FLAGS)) & 0x00080000) == 0)
            return 0;   // nil
        int best = -1;
        if (const auto* lists = gh->getThreatLists()) {
            for (const auto& [listMob, list] : *lists) {
                (void)list;
                // A dead or vanished mob's list no longer counts, whether or
                // not its clear has arrived.
                auto mobEnt = gh->getEntityManager().getEntity(listMob);
                if (!mobEnt || !mobEnt->isUnit() ||
                    static_cast<const game::Unit*>(mobEnt.get())->getHealth() == 0) continue;
                ThreatStanding s;
                if (threatStandingFor(gh, playerUnitGuid, listMob, s) && s.onList)
                    best = std::max(best, s.status);
            }
        }
        if (best < 0) return 0;   // on no list: nil
        lua_pushnumber(L, best);
        return 1;
    }
    ThreatStanding standing;
    if (threatStandingFor(gh, playerUnitGuid, mobGuid, standing)) {
        lua_pushnumber(L, standing.status);
        return 1;
    }
    // Not on that unit's threat list: nil, as the stock function answers
    // (0x613a60). This used to guess 3, "securely tanking", whenever the unit
    // merely targeted this one - so targeting yourself, or a party member or
    // bot looking at you, lit TargetFrameFlash and the party frames' flash
    // red in a city with no fight anywhere.
    return 0;   // nil
}

// UnitDetailedThreatSituation(unit, mobUnit) → isTanking, status, threatPct, rawThreatPct, threatValue
static int lua_UnitDetailedThreatSituation(lua_State* L) {
    // Use UnitThreatSituation logic for the basics
    auto* gh = getGameHandler(L);
    if (!gh) {
        lua_pushboolean(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
        return 5;
    }
    const char* uid = luaL_optstring(L, 1, "player");
    const char* mobUid = luaL_optstring(L, 2, nullptr);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t unitGuid = resolveUnitGuid(gh, uidStr);
    uint64_t mobGuid = 0;
    if (mobUid && *mobUid) {
        std::string mStr(mobUid);
        toLowerInPlace(mStr);
        mobGuid = resolveUnitGuid(gh, mStr);
    }

    ThreatStanding standing;
    if (threatStandingFor(gh, unitGuid, mobGuid, standing)) {
        const bool isTanking = (standing.status >= 2);
        double rawPct = 0.0;
        if (standing.topThreat > 0)
            rawPct = 100.0 * standing.threat / standing.topThreat;
        // Against the point where aggro changes hands rather than against the
        // leader: WoW pulls at a tenth clear of the current tank, so a hundred
        // here means "about to take it" and is what the numeric indicator
        // colours on. Whoever holds it is already at a hundred by definition.
        const double pullPct = isTanking ? 100.0 : std::min(100.0, rawPct / 1.1);
        lua_pushboolean(L, isTanking);
        lua_pushnumber(L, standing.status);
        lua_pushnumber(L, pullPct);
        lua_pushnumber(L, rawPct);
        lua_pushnumber(L, standing.threat);
        return 5;
    }

    // No list for this mob yet. The same guess UnitThreatSituation falls back
    // to, and the same lack of a number behind it.
    bool isTanking = false;
    int status = 0;
    if (unitGuid != 0 && mobGuid != 0) {
        auto mobEnt = gh->getEntityManager().getEntity(mobGuid);
        if (mobEnt && game::unitTargetGuid(*mobEnt) == unitGuid) {
            isTanking = true;
            status = 3;
        }
    }
    lua_pushboolean(L, isTanking);
    lua_pushnumber(L, status);
    lua_pushnumber(L, isTanking ? 100.0 : 0.0); // threatPct
    lua_pushnumber(L, isTanking ? 100.0 : 0.0); // rawThreatPct
    lua_pushnumber(L, 0); // threatValue - unknown without the list
    return 5;
}

// UnitDistanceSquared(unit) → distSq, canCalculate
static int lua_UnitDistanceSquared(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); lua_pushboolean(L, 0); return 2; }
    const char* uid = luaL_checkstring(L, 1);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0 || guid == gh->getPlayerGuid()) { lua_pushnumber(L, 0); lua_pushboolean(L, 0); return 2; }
    auto targetEnt = gh->getEntityManager().getEntity(guid);
    auto playerEnt = gh->getEntityManager().getEntity(gh->getPlayerGuid());
    if (!targetEnt || !playerEnt) { lua_pushnumber(L, 0); lua_pushboolean(L, 0); return 2; }
    float dx = playerEnt->getX() - targetEnt->getX();
    float dy = playerEnt->getY() - targetEnt->getY();
    float dz = playerEnt->getZ() - targetEnt->getZ();
    lua_pushnumber(L, dx*dx + dy*dy + dz*dz);
    lua_pushboolean(L, 1);
    return 2;
}

// CheckInteractDistance(unit, distIndex) → boolean
//
// The stock table (0x51B240, squared yards at 0xAC8A64..70): 1 inspect 30,
// 2 trade 11.11, 3 duel 10, 4 follow 30, compared strictly, centre to centre
// in 3D. Any other index answers false.
static int lua_CheckInteractDistance(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    const char* uid = luaL_checkstring(L, 1);
    int distIdx = static_cast<int>(luaL_optnumber(L, 2, 4));
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnFalse(L); }
    auto targetEnt = gh->getEntityManager().getEntity(guid);
    auto playerEnt = gh->getEntityManager().getEntity(gh->getPlayerGuid());
    if (!targetEnt || !playerEnt) { return luaReturnFalse(L); }
    static constexpr float kMaxDistSq[4] = {900.0f, 123.45678f, 100.0f, 900.0f};
    if (distIdx < 1 || distIdx > 4) { return luaReturnFalse(L); }
    float dx = playerEnt->getX() - targetEnt->getX();
    float dy = playerEnt->getY() - targetEnt->getY();
    float dz = playerEnt->getZ() - targetEnt->getZ();
    lua_pushboolean(L, dx * dx + dy * dy + dz * dz < kMaxDistSq[distIdx - 1]);
    return 1;
}

// UnitInRange(unit) → inRange, checkedOk
//
// The raid frames dim a member who has gone too far to help. Forty yards is
// what WoW means by it - the range most party-useful spells share - and the
// second return says whether the question could be answered at all: a member
// on another part of the map has no entity here, and dimming them for being
// out of range would be a guess dressed as a measurement.
static int lua_UnitInRange(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); lua_pushboolean(L, 0); return 2; }
    std::string uidStr(luaL_optstring(L, 1, "player"));
    toLowerInPlace(uidStr);
    const uint64_t guid = resolveUnitGuid(gh, uidStr);
    auto other  = guid ? gh->getEntityManager().getEntity(guid) : nullptr;
    auto player = gh->getEntityManager().getEntity(gh->getPlayerGuid());
    if (!other || !player) {
        lua_pushboolean(L, 0);
        lua_pushboolean(L, 0);   // could not be checked
        return 2;
    }
    const float dx = player->getX() - other->getX();
    const float dy = player->getY() - other->getY();
    const float dz = player->getZ() - other->getZ();
    lua_pushboolean(L, (dx*dx + dy*dy + dz*dz) <= (40.0f * 40.0f));
    lua_pushboolean(L, 1);
    return 2;
}

// IsRaidLeader() / IsPartyLeader-style check against the group's leader guid.
static int lua_IsRaidLeader(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    const auto& group = gh->getPartyData();
    lua_pushboolean(L, group.leaderGuid != 0 &&
                       group.leaderGuid == gh->getPlayerGuid() ? 1 : 0);
    return 1;
}

// UnitIsVisible(unit) → boolean (entity exists in the client's entity manager)
static int lua_UnitIsVisible(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "target");
    auto* unit = resolveUnit(L, uid);
    lua_pushboolean(L, unit != nullptr);
    return 1;
}

/// UnitGroupRolesAssigned(unit) → isTank, isHealer, isDamage.
///
/// Three booleans, not a role name. Both callers in this interface are written
/// the same way:
///
///     local isTank, isHealer, isDamage = UnitGroupRolesAssigned(unit)
///     if ( isTank ) then ... elseif ( isHealer ) then ...
///
/// A single string filled the first of those and left the other two nil - and
/// every string is truthy in Lua, "NONE" included, so the first branch always
/// won. The player frame and every party member frame showed the tank icon
/// regardless of role, including for a player with no role and no party.
///
/// Nothing here reads the returned name, so there is no caller to keep happy:
/// playerframe.lua and partymemberframe.lua are the only two.
static int lua_UnitGroupRolesAssigned(lua_State* L) {
    auto pushRoles = [L](bool tank, bool healer, bool damage) {
        lua_pushboolean(L, tank   ? 1 : 0);
        lua_pushboolean(L, healer ? 1 : 0);
        lua_pushboolean(L, damage ? 1 : 0);
        return 3;
    };
    auto* gh = getGameHandler(L);
    if (!gh) return pushRoles(false, false, false);
    const char* uid = luaL_optstring(L, 1, "player");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) return pushRoles(false, false, false);
    const auto& pd = gh->getPartyData();
    for (const auto& m : pd.members) {
        if (m.guid != guid) continue;
        // WotLK LFG roles bitmask (from SMSG_GROUP_LIST / SMSG_LFG_ROLE_CHECK_UPDATE).
        // Bit 0x01 = Leader (not a combat role), 0x02 = Tank, 0x04 = Healer, 0x08 = DPS.
        constexpr uint8_t kRoleTank    = 0x02;
        constexpr uint8_t kRoleHealer  = 0x04;
        constexpr uint8_t kRoleDamager = 0x08;
        return pushRoles((m.roles & kRoleTank)   != 0,
                         (m.roles & kRoleHealer) != 0,
                         (m.roles & kRoleDamager) != 0);
    }
    // Not in the party, or in no party at all: no role, and the icon is hidden
    // rather than guessed at.
    return pushRoles(false, false, false);
}

/// Friendly is an answer of its own rather than "not hostile": neutral
/// wildlife is neither, and can be attacked but not helped. A faction with a
/// standing is friendly only from Friendly up, as the reaction says.
static bool isFriendlyUnit(game::GameHandler* gh, const game::Unit& unit) {
    return !unit.isHostile() && gh->unitReactionToPlayer(unit) >= 5;
}

/// Attackable unless its faction is friendly: neutral wildlife can be fought,
/// a faction with a standing only while at war with it.
static bool canAttackUnit(game::GameHandler* gh, const game::Unit& unit) {
    return unit.isHostile() || !gh->isFriendlyFaction(unit.getFactionTemplate());
}

static game::Unit* unitAskedAbout(lua_State* L);

// UnitCanAttack(unit, otherUnit) → boolean
static int lua_UnitCanAttack(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    const char* uid1 = luaL_checkstring(L, 1);
    const char* uid2 = luaL_checkstring(L, 2);
    std::string u1(uid1), u2(uid2);
    toLowerInPlace(u1);
    toLowerInPlace(u2);
    uint64_t g1 = resolveUnitGuid(gh, u1);
    uint64_t g2 = resolveUnitGuid(gh, u2);
    if (g1 == 0 || g2 == 0 || g1 == g2) { return luaReturnFalse(L); }
    auto* unit = unitAskedAbout(L);
    lua_pushboolean(L, unit && canAttackUnit(gh, *unit));
    return 1;
}

// UnitCanCooperate(unit, otherUnit) → boolean
static int lua_UnitCanCooperate(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    (void)luaL_checkstring(L, 1); // unit1 (unused - cooperation is based on unit2's hostility)
    const char* uid2 = luaL_checkstring(L, 2);
    auto* unit2 = resolveUnit(L, uid2);
    if (!unit2) { return luaReturnFalse(L); }
    lua_pushboolean(L, isFriendlyUnit(gh, *unit2));
    return 1;
}

// UnitCreatureFamily(unit) → familyName or nil
static int lua_UnitCreatureFamily(lua_State* L) {
    // (0x611820) The family's name from CreatureFamily.dbc, by the unit's
    // template; nil for a player or a creature without one. This used a
    // table by hand that was off by one from Raptor on and missed Felguard,
    // so a raptor read "Beast" and everything from Moth up was a family over.
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    const char* uid = luaL_optstring(L, 1, "target");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnNil(L); }
    auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity || entity->getType() == game::ObjectType::PLAYER) { return luaReturnNil(L); }
    auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
    if (!unit) { return luaReturnNil(L); }
    uint32_t family = gh->getCreatureFamily(unit->getEntry());
    if (family == 0 && guid == gh->getPetGuid()) family = gh->getPetFamily();
    const auto* info = family ? gh->petDbc().family(family) : nullptr;
    if (!info || info->name.empty()) { return luaReturnNil(L); }
    lua_pushstring(L, info->name.c_str());
    return 1;
}

// UnitOnTaxi(unit) → boolean (true if on a flight path)
static int lua_UnitOnTaxi(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    if (uidStr == "player") {
        lua_pushboolean(L, gh->isOnTaxiFlight());
    } else {
        lua_pushboolean(L, 0); // Can't determine for other units
    }
    return 1;
}

// UnitSex(unit) → 1=unknown, 2=male, 3=female
static int lua_UnitSex(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 1); return 1; }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid != 0) {
        auto entity = gh->getEntityManager().getEntity(guid);
        if (entity) {
            // Gender is byte 2 of UNIT_FIELD_BYTES_0 (0=male, 1=female)
            uint32_t bytes0 = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
            uint8_t gender = static_cast<uint8_t>((bytes0 >> 16) & 0xFF);
            lua_pushnumber(L, gender == 0 ? 2 : (gender == 1 ? 3 : 1)); // WoW: 2=male, 3=female
            return 1;
        }
    }
    lua_pushnumber(L, 1); // unknown
    return 1;
}

// --- Player/Game API ---

/// The character sheet's breakdowns - the buff parts of a stat, an armour or
/// resistance value, attack power's modifiers and multiplier, and the weapon
/// damage and speed the server has already worked out - are all unit fields
/// the server sends to the unit's owner (UpdateFields.h, PRIVATE|OWNER).
///
/// Each sits at a fixed distance from a field the layout table names, the
/// same in every expansion's unit block: POSSTAT0 and NEGSTAT0 follow the
/// five STATs, the two RESISTANCEBUFFMODS arrays follow the seven
/// RESISTANCES, ATTACK_POWER_MODS and _MULTIPLIER follow ATTACK_POWER (and
/// the ranged pair RANGED_ATTACK_POWER), the off-hand damage follows the main
/// hand's, RANGEDATTACKTIME follows the two BASEATTACKTIMEs and
/// MAXRANGEDDAMAGE follows MINRANGEDDAMAGE.
struct UnitStatFields {
    const game::Entity* entity = nullptr;
    bool has(game::UF base, int offset) const {
        const uint16_t idx = game::fieldIndex(base);
        return entity && idx != 0xFFFF && entity->hasField(static_cast<uint16_t>(idx + offset));
    }
    uint32_t raw(game::UF base, int offset) const {
        const uint16_t idx = game::fieldIndex(base);
        if (!entity || idx == 0xFFFF) return 0;
        return entity->getField(static_cast<uint16_t>(idx + offset));
    }
    /// The buff parts are floats on the server, sent as uint32(value) - so a
    /// negative one arrives as a two's-complement int (Unit.cpp,
    /// BuildValuesUpdate).
    int32_t asInt(game::UF base, int offset) const {
        return static_cast<int32_t>(raw(base, offset));
    }
    float asFloat(game::UF base, int offset) const {
        const uint32_t bits = raw(base, offset);
        float f = 0.0f;
        std::memcpy(&f, &bits, sizeof f);
        return f;
    }
};

/// The player's or the pet's unit, by the lowered unit id the sheet passes.
static UnitStatFields unitStatFields(game::GameHandler* gh, const std::string& who) {
    UnitStatFields f;
    if (!gh) return f;
    const uint64_t guid = (who == "player") ? gh->getPlayerGuid()
                        : (who == "pet")    ? gh->getPetGuid() : 0;
    if (guid == 0) return f;
    if (auto e = gh->getEntityManager().getEntity(guid)) f.entity = e.get();
    return f;
}

static int lua_UnitStat(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 4; }
    int statIdx = static_cast<int>(luaL_checknumber(L, 2)) - 1; // WoW API is 1-indexed
    // Whose stat. This ignored the unit and answered from the player, so the
    // paperdoll's pet tab listed the hunter's own Strength as the pet's.
    std::string who(luaL_optstring(L, 1, "player"));
    toLowerInPlace(who);
    int32_t val = 0;
    if (who == "pet") {
        if (statIdx >= 0 && statIdx < 5) val = gh->getPetStats()[static_cast<size_t>(statIdx)];
    } else if (who == "player") {
        val = gh->getPlayerStat(statIdx);
    }
    if (val < 0) val = 0;
    // stat, effectiveStat, posBuff, negBuff. The first two are both the
    // total: PaperDollFrame_SetStat prints effectiveStat and works the base
    // out itself as `stat - posBuff - negBuff`. The buffs are UNIT_FIELD_
    // POSSTAT0+i and NEGSTAT0+i (Unit::UpdateStatBuffMod); with them at zero
    // the sheet never coloured a buffed stat or explained it.
    int32_t posBuff = 0, negBuff = 0;
    if (statIdx >= 0 && statIdx < 5) {
        const auto f = unitStatFields(gh, who);
        posBuff = f.asInt(game::UF::UNIT_FIELD_STAT0, 5 + statIdx);
        negBuff = f.asInt(game::UF::UNIT_FIELD_STAT0, 10 + statIdx);
    }
    lua_pushnumber(L, val);
    lua_pushnumber(L, val);
    lua_pushnumber(L, posBuff);
    lua_pushnumber(L, negBuff);
    return 4;
}

// GetDodgeChance() → percent
static int lua_GetDodgeChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    float v = gh ? gh->getDodgePct() : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetParryChance() → percent
static int lua_GetParryChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    float v = gh ? gh->getParryPct() : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetBlockChance() → percent
static int lua_GetBlockChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    float v = gh ? gh->getBlockPct() : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetCritChance() → percent (melee crit)
static int lua_GetCritChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    float v = gh ? gh->getCritPct() : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetRangedCritChance() → percent
static int lua_GetRangedCritChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    float v = gh ? gh->getRangedCritPct() : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetSpellCritChance(school) → percent. The interface's schools count from
// one - 1 physical, 2 holy ... 7 arcane: PaperDollFrame_SetRating's
// CR_CRIT_SPELL branch starts at `holySchool = 2` and runs to
// MAX_SPELL_SCHOOLS (7) - while the PLAYER_SPELL_CRIT_PERCENTAGE1 fields are
// the wire's SpellSchools, physical at 0. Passed through unshifted, every
// school read its neighbour and arcane read past the end.
static int lua_GetSpellCritChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    int school = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    float v = gh ? gh->getSpellCritPct(school) : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetCombatRating(ratingIndex) → value.
// The FrameXML CR_* constants are 1-based (CR_WEAPON_SKILL=1 … CR_ARMOR_PENETRATION=25),
// but the player-field array and the combat game tables are 0-based (AzerothCore's
// CombatRating enum, weapon skill at 0). Convert at the boundary or every rating reads
// its neighbour's slot.
static int lua_GetCombatRating(lua_State* L) {
    auto* gh = getGameHandler(L);
    int cr = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    int32_t v = gh ? gh->getCombatRating(cr) : 0;
    lua_pushnumber(L, v >= 0 ? v : 0);
    return 1;
}

// GetSpellBonusDamage(school) → that school's bonus, schools numbered from
// one as GetSpellCritChance's are (2 holy ... 7 arcane). The character pane
// asks for each school and shows the smallest with a per-school tooltip
// (PaperDollFrame_SetSpellBonusDamage), so one figure for all of them hid
// every school-specific bonus. Without a school, the largest.
static int lua_GetSpellBonusDamage(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnZero(L); }
    int32_t sp = lua_isnumber(L, 1)
        ? gh->getSpellDamageBonus(static_cast<int>(lua_tonumber(L, 1)) - 1)
        : gh->getSpellPower();
    lua_pushnumber(L, sp >= 0 ? sp : 0);
    return 1;
}

// GetSpellBonusHealing() → value
static int lua_GetSpellBonusHealing(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnZero(L); }
    int32_t v = gh->getHealingPower();
    lua_pushnumber(L, v >= 0 ? v : 0);
    return 1;
}

// GetMeleeHaste / GetAttackPowerForStat stubs for addon compat
static int lua_GetAttackPower(lua_State* L) {
    auto* gh = getGameHandler(L);
    int32_t ap = gh ? gh->getMeleeAttackPower() : 0;
    if (ap < 0) ap = 0;
    lua_pushnumber(L, ap);  // base
    lua_pushnumber(L, 0);   // posBuff
    lua_pushnumber(L, 0);   // negBuff
    return 3;
}

static int lua_GetRangedAttackPower(lua_State* L) {
    auto* gh = getGameHandler(L);
    int32_t ap = gh ? gh->getRangedAttackPower() : 0;
    if (ap < 0) ap = 0;
    lua_pushnumber(L, ap);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    return 3;
}


static int lua_IsInGroup(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isInGroup());
    return 1;
}

static int lua_IsInRaid(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isInGroup() &&
                       game::groupIsRaid(gh->getPartyData().groupType));
    return 1;
}

// PlaySound(soundId) - play a WoW UI sound by ID or name

static int lua_UnitRace(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, "Unknown"); lua_pushstring(L, "Unknown"); lua_pushnumber(L, 0); return 3; }
    std::string uid(luaL_optstring(L, 1, "player"));
    toLowerInPlace(uid);

    const uint8_t raceId = unitRaceOf(gh, uid);
    const bool known = (raceId > 0 && raceId < 12);
    const char* name = known ? kLuaRaces[raceId] : "Unknown";
    const char* file = known ? kLuaRaceFileNames[raceId] : "Unknown";
    lua_pushstring(L, name);      // 1: localized race
    lua_pushstring(L, file);      // 2: race file name, spliced into asset paths
    lua_pushnumber(L, raceId);    // 3: raceId (WoW returns 3 values)
    return 3;
}

static int lua_UnitPowerType(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);

    if (unit) {
        uint8_t pt = unit->getPowerType();
        lua_pushnumber(L, pt);
        lua_pushstring(L, (pt < 7) ? kLuaPowerNames[pt] : "MANA");
        return 2;
    }
    // Fallback: party member stats for out-of-range members
    auto* gh = getGameHandler(L);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
    const auto* pm = findPartyMember(gh, guid);
    if (pm) {
        uint8_t pt = pm->powerType;
        lua_pushnumber(L, pt);
        lua_pushstring(L, (pt < 7) ? kLuaPowerNames[pt] : "MANA");
        return 2;
    }
    lua_pushnumber(L, 0);
    lua_pushstring(L, "MANA");
    return 2;
}

static int lua_GetNumGroupMembers(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->getPartyData().memberCount : 0);
    return 1;
}

static int lua_UnitGUID(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnNil(L); }
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%016llX", (unsigned long long)guid);
    lua_pushstring(L, buf);
    return 1;
}

/// UnitPVPName(unit) → the name with the player's title around it, or just
/// the name when there is no title.
///
/// The comment here used to say titles were not tracked. They are - the known
/// bits and the chosen one both come off the player - so the character sheet
/// showed a bare name for someone wearing a title.
///
/// Only for the player: a title is a bit index on the unit's own fields and
/// this client reads that for itself alone, so anyone else answers their name,
/// which is what WoW answers for a character wearing none.
static int lua_UnitPVPName(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* uid = luaL_optstring(L, 1, "player");
    if (gh && uid) {
        std::string id(uid);
        toLowerInPlace(id);
        if (id == "player" && gh->getChosenTitleBit() > 0) {
            const std::string titled =
                gh->getFormattedTitle(static_cast<uint32_t>(gh->getChosenTitleBit()));
            if (!titled.empty()) { lua_pushstring(L, titled.c_str()); return 1; }
        }
    }
    return lua_UnitName(L);
}

static int lua_UnitIsPlayer(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    auto entity = guid ? gh->getEntityManager().getEntity(guid) : nullptr;
    lua_pushboolean(L, entity && entity->getType() == game::ObjectType::PLAYER);
    return 1;
}

// Vehicles (stock 0xAD19F0 / 0xAD2658, docs/re/mounts-taxi-vehicles.md 4):
// a unit is in a vehicle when it sits in a vehicle's seat, and the seat's
// VehicleSeat flags answer the rest.
static uint64_t vehicleUnitArg(lua_State* L, game::GameHandler* gh) {
    std::string uid(luaL_optstring(L, 1, "player"));
    toLowerInPlace(uid);
    return gh ? resolveUnitGuid(gh, uid) : 0;
}
/// UnitHasVehicleUI(unit): the seat has flag 0x20000000 (0x613740) - the
/// action bar goes to VehicleMenuBar.
static int lua_UnitHasVehicleUI(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint64_t guid = vehicleUnitArg(L, gh);
    const auto* seat = guid ? gh->vehicleSeatOf(guid) : nullptr;
    lua_pushboolean(L, (seat && (seat->flags & game::vehicle::kSeatHasVehicleUI)) ? 1 : 0);
    return 1;
}
/// UnitInVehicleControlSeat(unit): seat flag 0x800 (0x613700).
static int lua_UnitInVehicleControlSeat(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint64_t guid = vehicleUnitArg(L, gh);
    const auto* seat = guid ? gh->vehicleSeatOf(guid) : nullptr;
    lua_pushboolean(L, (seat && (seat->flags & game::vehicle::kSeatControlSeat)) ? 1 : 0);
    return 1;
}
/// UnitVehicleSeatCount(unit): the seats of the vehicle at the bottom of
/// the unit's chain, accessories' seats in place of theirs (0x613830).
static int lua_UnitVehicleSeatCount(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint64_t guid = vehicleUnitArg(L, gh);
    const uint64_t root = guid ? gh->rootVehicleOf(guid) : 0;
    lua_pushnumber(L, root ? gh->vehicleVirtualSeatCount(root) : 0);
    return 1;
}
/// UnitVehicleSeatInfo(unit, i) -> controlType ("Root" for the root's
/// control seat, "Child" for an accessory's, else "None"), occupantName,
/// occupantRealm, canEject (flagsB 0x20), canSwitchSeats (0x4000000)
/// (0x6138C0 -> 0x7579E0).
static int lua_UnitVehicleSeatInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    std::string uid(luaL_optstring(L, 1, "player"));
    toLowerInPlace(uid);
    const uint64_t guid = gh ? resolveUnitGuid(gh, uid) : 0;
    const int index = static_cast<int>(luaL_optnumber(L, 2, 0)) - 1;
    const uint64_t root = guid ? gh->rootVehicleOf(guid) : 0;
    uint64_t veh = 0;
    int seat = 0;
    if (!root || !gh->vehicleVirtualSeat(root, index, veh, seat)) return 0;
    const auto* rec = gh->vehicleData().seatOf(gh->unitVehicleId(veh), seat);
    if (!rec) return 0;
    const bool control = (rec->flags & game::vehicle::kSeatControlSeat) != 0;
    lua_pushstring(L, control ? (veh == root ? "Root" : "Child") : "None");
    const uint64_t occ = gh->vehicleSeatOccupant(veh, seat);
    std::string name;
    if (occ) {
        if (auto u = std::dynamic_pointer_cast<game::Unit>(gh->getEntityManager().getEntity(occ)))
            name = u->getName();
        if (name.empty()) name = gh->lookupName(occ);
        // The player's own name, off the character list (as UnitName does).
        for (const auto& c : gh->getCharacters())
            if (name.empty() && c.guid == occ) name = c.name;
    }
    if (occ && !name.empty()) lua_pushstring(L, name.c_str()); else lua_pushnil(L);
    lua_pushnil(L);
    lua_pushboolean(L, (rec->flagsB & game::vehicle::kSeatBCanEject) ? 1 : 0);
    lua_pushboolean(L, (rec->flags & game::vehicle::kSeatCanSwitch) ? 1 : 0);
    return 5;
}
/// UnitSwitchToVehicleSeat("player", i) (0x6139B0 -> 0x74CA90).
static int lua_UnitSwitchToVehicleSeat(lua_State* L) {
    auto* gh = getGameHandler(L);
    std::string uid(luaL_optstring(L, 1, "player"));
    toLowerInPlace(uid);
    const uint64_t guid = gh ? resolveUnitGuid(gh, uid) : 0;
    if (!gh || guid != gh->getPlayerGuid() || !gh->vehiclePassenger(guid)) return 0;
    const uint64_t root = gh->rootVehicleOf(guid);
    uint64_t veh = 0;
    int seat = 0;
    if (root && gh->vehicleVirtualSeat(root, static_cast<int>(luaL_optnumber(L, 2, 0)) - 1, veh, seat))
        gh->switchToVehicleSeat(veh, seat);
    return 0;
}
/// CanSwitchVehicleSeat() - the player's seat lets it (0x608580).
static int lua_CanSwitchVehicleSeat(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, (gh && gh->vehiclePassenger(gh->getPlayerGuid()) &&
                        gh->playerSeatHasFlag(game::vehicle::kSeatCanSwitch)) ? 1 : 0);
    return 1;
}
/// UnitInVehicle(unit): seated (passenger state 3).
static int lua_UnitInVehicle(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint64_t guid = vehicleUnitArg(L, gh);
    lua_pushboolean(L, (guid && gh->vehiclePassenger(guid)) ? 1 : 0);
    return 1;
}
/// UnitControllingVehicle(unit): seated, and the unit's UNIT_FIELD_CHARM is
/// the vehicle it sits in (0x613570) - the driver of a charmed vehicle.
static int lua_UnitControllingVehicle(lua_State* L) {
    auto* gh = getGameHandler(L);
    std::string uid(luaL_optstring(L, 1, "player"));
    toLowerInPlace(uid);
    const uint64_t guid = gh ? resolveUnitGuid(gh, uid) : 0;
    const auto* p = guid ? gh->vehiclePassenger(guid) : nullptr;
    bool controls = false;
    const uint16_t idx = game::fieldIndex(game::UF::UNIT_FIELD_CHARM);
    if (p && idx != 0xFFFF) {
        if (auto e = gh->getEntityManager().getEntity(guid)) {
            const uint64_t charm = (static_cast<uint64_t>(e->getField(static_cast<uint16_t>(idx + 1))) << 32) |
                                   e->getField(idx);
            controls = charm != 0 && charm == p->vehicleGuid;
        }
    }
    lua_pushboolean(L, controls ? 1 : 0);
    return 1;
}
/// The guid held in a two-word unit field, or zero.
///
/// Absent on an expansion whose table does not name the field, which is every
/// one but WotLK for these two - an index that is not known is not guessed at.
static uint64_t guidField(game::GameHandler* gh, uint64_t unitGuid, game::UF field) {
    if (!gh) return 0;
    const uint16_t idx = game::fieldIndex(field);
    if (idx == 0xFFFF) return 0;
    auto entity = gh->getEntityManager().getEntity(unitGuid);
    if (!entity) return 0;
    const uint64_t lo = entity->getField(idx);
    const uint64_t hi = entity->getField(static_cast<uint16_t>(idx + 1));
    return (hi << 32) | lo;
}

/// Whether this unit is driving something else.
///
/// UNIT_FIELD_CHARM names what it controls, and it was not read because the
/// field was not in the table. Both these answered a flat false and said so.
static int lua_UnitIsPossessed(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* uid = luaL_optstring(L, 1, "player");
    const uint64_t guid = gh ? resolveUnitGuid(gh, uid) : 0;
    lua_pushboolean(L, guidField(gh, guid, game::UF::UNIT_FIELD_CHARM) != 0);
    return 1;
}

/// Whether something else is driving this unit - the other end of the same
/// relationship, in UNIT_FIELD_CHARMEDBY.
///
/// ToggleGameMenu reads it on every Escape that clears a target:
/// `ClearTarget() and (not UnitIsCharmed("player"))`.
static int lua_UnitIsCharmed(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* uid = luaL_optstring(L, 1, "player");
    const uint64_t guid = gh ? resolveUnitGuid(gh, uid) : 0;
    lua_pushboolean(L, guidField(gh, guid, game::UF::UNIT_FIELD_CHARMEDBY) != 0);
    return 1;
}
static int lua_UnitIsTalking(lua_State* L) { return luaReturnFalse(L); }
static int lua_UnitInBattleground(lua_State* L) { return luaReturnNil(L); }

/// True for a unit in the player's own group, and for its pet. The party
/// frames use this to decide whether a unit is one of ours at all.
static int lua_UnitPlayerOrPetInParty(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    // "partypet2" asks about party member 2's pet, and the answer is whatever
    // holds for the member.
    const size_t petPos = uidStr.find("pet");
    if (petPos != std::string::npos) uidStr.erase(petPos, 3);
    if (uidStr.empty() || uidStr == "player") {
        lua_pushboolean(L, gh->isInGroup());
        return 1;
    }
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnFalse(L); }
    const auto& pd = gh->getPartyData();
    bool found = false;
    for (const auto& m : pd.members) {
        if (m.guid == guid) { found = true; break; }
    }
    lua_pushboolean(L, found);
    return 1;
}

static int lua_UnitPlayerOrPetInRaid(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !game::groupIsRaid(gh->getPartyData().groupType)) { return luaReturnFalse(L); }
    return lua_UnitPlayerOrPetInParty(L);
}

/// Talent points, and the two the interface actually reads: unspent points and
/// the total spent. The micro button flashes on the first.
// UnitCharacterPoints(unit) → talent points, primary professions still learnable
//
// Two values. The trainer's confirmation reads the second bare -
// `if ( cp2 < MAX_LEARNABLE_PROFESSIONS )` - to decide whether it is offering a
// first profession or a second, so returning one value made confirming a
// profession an error rather than a question.
//
// Two is the limit, and a primary profession is category 11 in SkillLine.dbc,
// which is the same test isProfessionSpell uses rather than a second opinion
// about what counts.
static int lua_UnitCharacterPoints(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    if (!gh || uidStr != "player") { lua_pushinteger(L, 0); lua_pushinteger(L, 0); return 2; }

    constexpr int kMaxPrimaryProfessions = 2;
    int known = 0;
    for (const auto& [skillId, skill] : gh->getPlayerSkills()) {
        (void)skill;
        if (gh->getSkillCategory(skillId) == 11) ++known;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(gh->getUnspentTalentPoints()));
    lua_pushinteger(L, std::max(0, kMaxPrimaryProfessions - known));
    return 2;
}

/// Not while the pet's bar is the vehicle's or the possessed unit's bonus
/// bar (0x5D3720 tests the bonus bar's flag).
static int lua_PetHasActionBar(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->hasPet() && !gh->vehicleBarActive());
    return 1;
}
static int lua_PetCanBeDismissed(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->hasPet());
    return 1;
}
// Abandoning is for a hunter's pet, and the server says which pets those are:
// UNIT_FIELD_BYTES_2 byte 2 carries UNIT_CAN_BE_ABANDONED (0x02) on them and on
// no other. The pet menu offers Rename, Abandon and the pet paperdoll on it.
static int lua_PetCanBeAbandoned(lua_State* L) {
    // (0x5D3780) And the pet must be the player's own creation (CREATEDBY):
    // a charmed or summoned unit with the flag is not the player's to give up.
    auto* gh = getGameHandler(L);
    const uint64_t pet = gh ? gh->getPetGuid() : 0;
    auto entity = pet ? gh->getEntityManager().getEntity(pet) : nullptr;
    constexpr uint32_t kCanBeAbandoned = 0x02;
    const uint32_t bytes2 = entity ? entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_2)) : 0;
    bool mine = entity != nullptr;
    if (entity) {
        const uint16_t cb = game::fieldIndex(game::UF::UNIT_FIELD_CREATEDBY_LO);
        if (cb != 0xFFFF) {
            const uint64_t creator = (static_cast<uint64_t>(entity->getField(cb + 1)) << 32) | entity->getField(cb);
            mine = creator == gh->getPlayerGuid();
        }
    }
    // True or nil, as the client answers.
    if (mine && ((bytes2 >> 16) & kCanBeAbandoned) != 0) lua_pushboolean(L, 1); else lua_pushnil(L);
    return 1;
}

static int lua_InCombatLockdown(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isInCombat());
    return 1;
}

static int lua_IsMounted(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isMounted());
    return 1;
}

static int lua_IsFlying(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isPlayerFlying());
    return 1;
}

static int lua_IsSwimming(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isSwimming());
    return 1;
}

static int lua_IsResting(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isPlayerResting());
    return 1;
}

static int lua_IsFalling(lua_State* L) {
    auto* gh = getGameHandler(L);
    // Check FALLING movement flag
    if (!gh) { return luaReturnFalse(L); }
    const auto& mi = gh->getMovementInfo();
    lua_pushboolean(L, (mi.flags & 0x2000) != 0); // MOVEFLAG_FALLING = 0x2000
    return 1;
}

static int lua_IsStealthed(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    // Check for stealth auras (aura flags bit 0x40 = is harmful, stealth is a buff)
    // WoW detects stealth via unit flags: UNIT_FLAG_IMMUNE (0x02) or specific aura IDs
    // Simplified: check player auras for known stealth spell IDs
    bool stealthed = false;
    for (const auto& a : gh->getPlayerAuras()) {
        if (a.isEmpty() || a.spellId == 0) continue;
        // Common stealth IDs: 1784 (Stealth), 5215 (Prowl), 66 (Invisibility)
        if (a.spellId == 1784 || a.spellId == 5215 || a.spellId == 66 ||
            a.spellId == 1785 || a.spellId == 1786 || a.spellId == 1787 ||
            a.spellId == 11305 || a.spellId == 11306) {
            stealthed = true;
            break;
        }
    }
    lua_pushboolean(L, stealthed);
    return 1;
}

static int lua_GetUnitSpeed(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* uid = luaL_optstring(L, 1, "player");
    if (!gh || std::string(uid) != "player") {
        lua_pushnumber(L, 0);
        return 1;
    }
    lua_pushnumber(L, gh->getServerRunSpeed());
    return 1;
}

// --- Container/Bag API ---
// WoW bags: container 0 = backpack (16 slots), containers 1-4 = equipped bags


static int lua_UnitXP(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnZero(L); }
    std::string u(uid);
    toLowerInPlace(u);
    if (u == "player") lua_pushnumber(L, gh->getPlayerXp());
    else lua_pushnumber(L, 0);
    return 1;
}

static int lua_UnitXPMax(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 1); return 1; }
    std::string u(uid);
    toLowerInPlace(u);
    if (u == "player") {
        uint32_t nlxp = gh->getPlayerNextLevelXp();
        lua_pushnumber(L, nlxp > 0 ? nlxp : 1);
    } else {
        lua_pushnumber(L, 1);
    }
    return 1;
}

// GetXPExhaustion() → rested XP pool remaining (nil if none)
static int lua_GetXPExhaustion(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    uint32_t rested = gh->getPlayerRestedXp();
    if (rested > 0) lua_pushnumber(L, rested);
    else lua_pushnil(L);
    return 1;
}

/// GetWatchedFactionInfo() → name, standingID, barMin, barMax, barValue.
///
/// The reputation bar under the experience bar, which draws nothing at all
/// without this. Returns nothing when no faction is being watched, which is
/// what MainMenuBar_UpdateExperienceBars checks for before showing the bar.
///
/// The bounds are the rank's own, not the whole scale: WoW's bar fills from
/// the bottom of the current rank to the top of it, so a character halfway
/// through Honored shows a half-full bar rather than one two-thirds along.
static int lua_GetWatchedFactionInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const uint32_t factionId = gh->getWatchedFactionId();
    if (factionId == 0) return 0;
    const auto& standings = gh->getFactionStandings();
    auto it = standings.find(factionId);
    if (it == standings.end()) return 0;
    const int32_t standing = it->second;

    // Blizzard's own thresholds, and the standing id that goes with each. Hated
    // is 1 and Exalted is 8, which is what the interface indexes FACTION_BAR_
    // COLORS and the standing names by.
    static const struct { int32_t min, max; } kRanks[8] = {
        {-42000, -6000}, {-6000, -3000}, {-3000, 0}, {0, 3000},
        {3000, 9000}, {9000, 21000}, {21000, 42000}, {42000, 43000},
    };
    int rank = 7;
    for (int i = 0; i < 8; ++i) {
        if (standing < kRanks[i].max) { rank = i; break; }
    }

    lua_pushstring(L, gh->getFactionNamePublic(factionId).c_str());
    lua_pushnumber(L, rank + 1);
    lua_pushnumber(L, kRanks[rank].min);
    lua_pushnumber(L, kRanks[rank].max);
    lua_pushnumber(L, standing);
    lua_pushnumber(L, factionId);
    return 6;
}

/// GetNumBagSlots() → the four bags beside the backpack. A constant in every
/// expansion this client speaks, but the interface reads it rather than
/// assuming, and arithmetic on nil is what it gets otherwise.
static int lua_GetNumBagSlots(lua_State* L) {
    lua_pushnumber(L, 4);
    return 1;
}

/// GetMirrorTimerProgress(timer) → milliseconds left on breath, fatigue or
/// feign death. None of the three is tracked yet, and the honest answer is
/// zero rather than a stub that reads as a timer running.
/// GetMirrorTimerProgress(timer) → milliseconds left on that timer.
///
/// It answered a constant zero, and MirrorTimerFrame_OnUpdate divides it by a
/// thousand and calls SetValue with the result on every frame - so the breath
/// bar was reset to empty as fast as it could be drawn. An empty frame with a
/// label was the whole of what it could ever show.
///
/// Named, not numbered: WoW passes the timer's name through the event and
/// FrameXML hands the same string back here.
static int lua_GetMirrorTimerProgress(lua_State* L) {
    auto* gh = getGameHandler(L);
    std::string name(luaL_optstring(L, 1, ""));
    toLowerInPlace(name);
    int type = -1;
    if (name == "exhaustion") type = 0;
    else if (name == "breath") type = 1;
    else if (name == "death") type = 2;
    if (!gh || type < 0) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, gh->getMirrorTimer(type).value);
    return 1;
}

/// GetTimeToWellRested() → seconds until fully rested, or nil.
///
/// Confirmed called rather than guessed at: it turned up in the missing-API
/// report from a real session, read by the experience bar's tooltip. Rest
/// accrual is not modelled, and nil is what WoW itself answers when there is
/// no timer to report - which is the branch the tooltip already handles.
static int lua_GetTimeToWellRested(lua_State* L) {
    return luaReturnNil(L);
}


// ── Character sheet ────────────────────────────────────────────────────────
//
// PaperDollFrame reads about forty functions and does arithmetic with nearly
// all of them, so a missing one is not a blank field - it is "attempt to
// perform arithmetic on a nil value" and the rest of the panel never draws.
// The server sends most of what these want; where it does not, the honest
// answer is a zero of the right shape rather than nothing.

/// base, positive buff, negative buff for one of the two attack powers.
///
/// UNIT_FIELD_ATTACK_POWER is the base alone; the modifier and the
/// multiplier are the next two fields, and the total is
/// (base + mods) * (1 + multiplier) - Unit::GetTotalAttackPowerValue, from
/// what StatSystem.cpp's UpdateAttackPowerAndDamage stores. The sheet prints
/// base + posBuff + negBuff, so the difference is the buff: reading the base
/// alone left out every attack-power buff and talent multiplier.
static int pushAttackPowerParts(lua_State* L, game::GameHandler* gh,
                                game::UF baseField, int32_t fallbackBase) {
    const auto f = unitStatFields(gh, "player");
    int32_t base = fallbackBase > 0 ? fallbackBase : 0;
    double bonus = 0.0;
    if (f.has(baseField, 0)) {
        base = f.asInt(baseField, 0);
        const int32_t mods = f.asInt(baseField, 1);
        const float mult = f.asFloat(baseField, 2);
        const double total = (static_cast<double>(base) + mods) * (1.0 + mult);
        bonus = total - base;
    }
    lua_pushnumber(L, base);
    lua_pushnumber(L, bonus > 0 ? std::floor(bonus) : 0);
    lua_pushnumber(L, bonus < 0 ? std::ceil(bonus) : 0);
    return 3;
}

/// UnitAttackPower(unit) → base, positive buff, negative buff.
static int lua_UnitAttackPower(lua_State* L) {
    auto* gh = getGameHandler(L);
    // Whose. PaperDollFrame_SetAttackPower is shared and the pet tab calls it
    // with "Pet", so the pet's line showed the hunter's attack power.
    std::string who(luaL_optstring(L, 1, "player"));
    toLowerInPlace(who);
    if (who == "pet") {
        const int32_t petAp = gh ? gh->getPetAttackPower() : 0;
        lua_pushnumber(L, petAp > 0 ? petAp : 0);
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 3;
    }
    const int32_t ap = gh ? gh->getMeleeAttackPower() : -1;
    return pushAttackPowerParts(L, gh, game::UF::UNIT_FIELD_ATTACK_POWER, ap);
}
static int lua_UnitRangedAttackPower(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int32_t ap = gh ? gh->getRangedAttackPower() : -1;
    return pushAttackPowerParts(L, gh, game::UF::UNIT_FIELD_RANGED_ATTACK_POWER, ap);
}

/// UnitDefense(unit) → base, modifier. Base defense skill is five per level -
/// the cap a character sits at in practice, which is all the server lets us
/// infer since it does not send the trained skill. The modifier is the defense
/// skill granted by defense rating: AzerothCore takes int32(GetRatingBonusValue(
/// CR_DEFENSE_SKILL)) and each point is worth 0.04% avoidance. It was a flat
/// zero, so a geared tank's defense showed only the level cap with no sign of
/// the defence rating that pushes it to the crit-immunity threshold.
static int lua_UnitDefense(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int level = gh ? static_cast<int>(gh->getPlayerLevel()) : 1;
    constexpr int kCrDefenseSkill = 1;  // 0-based CombatRating enum
    const int modifier = gh ? static_cast<int>(gh->getCombatRatingBonus(kCrDefenseSkill)) : 0;
    lua_pushnumber(L, level * 5);
    lua_pushnumber(L, modifier);
    return 2;
}

/// UnitAttackSpeed(unit) → main hand, off hand, in seconds. Nothing here
/// tracks weapon speed yet; two seconds is a one-handed weapon and keeps the
/// damage-per-second division that follows from dividing by zero.
/// UnitAttackSpeed(unit) → mainHandSpeed, offHandSpeed.
///
/// The off-hand speed is nil when nothing is in that hand, and that is what
/// the character sheet branches on: PaperDollFrame_SetAttackSpeed does
/// `if ( offhandSpeed )` and then prints a two-handed line. Answering a speed
/// unconditionally claimed an off-hand weapon for everyone, and the line it
/// then built concatenated a value nothing had filled in.
static int lua_UnitAttackSpeed(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* uid = luaL_optstring(L, 1, "player");
    std::string u(uid);
    toLowerInPlace(u);
    // The comment above used to say nothing tracked weapon speed. The equipped
    // item carries delayMs and always has, so both hands answer from the
    // weapon rather than from a flat two seconds - which every damage-per-
    // second figure on the sheet is divided by.
    double mainSpeed = 2.0;      // unarmed, which is what WoW uses bare-handed
    double offSpeed = 0.0;
    bool hasOffHand = false;
    if (gh && u == "player") {
        const auto& inv = gh->getInventory();
        const auto& mh = inv.getEquipSlot(game::EquipSlot::MAIN_HAND);
        if (!mh.empty() && mh.item.delayMs > 0) mainSpeed = mh.item.delayMs / 1000.0;
        // A weapon in the off hand, not a shield or a held-in-off-hand item:
        // only a second weapon has a swing (paperdoll's `if ( offhandSpeed )`
        // prints a dual-wield line).
        const auto& oh = inv.getEquipSlot(game::EquipSlot::OFF_HAND);
        hasOffHand = !oh.empty() && oh.item.damageMax > 0.0f;
        if (hasOffHand) offSpeed = oh.item.delayMs > 0 ? oh.item.delayMs / 1000.0 : 2.0;
        // The swing times the server has set, haste included -
        // UNIT_FIELD_BASEATTACKTIME main and off hand, in milliseconds.
        const auto f = unitStatFields(gh, "player");
        const uint32_t mainMs = f.raw(game::UF::UNIT_FIELD_BASEATTACKTIME, 0);
        const uint32_t offMs = f.raw(game::UF::UNIT_FIELD_BASEATTACKTIME, 1);
        if (mainMs > 0) mainSpeed = mainMs / 1000.0;
        if (hasOffHand && offMs > 0) offSpeed = offMs / 1000.0;
    }
    lua_pushnumber(L, mainSpeed);
    if (hasOffHand) lua_pushnumber(L, offSpeed);
    else            lua_pushnil(L);
    return 2;
}

/// UnitDamage(unit) → min, max, off-hand min, off-hand max, positive bonus,
/// negative bonus, percent.
///
/// The comment here used to say weapon damage was not tracked, so it reported
/// the attack-power contribution alone against a two-second swing. The
/// equipped weapon carries its damage range and its speed, so the figure is
/// the weapon's own damage plus what attack power adds over one swing -
/// attack power divided by fourteen is damage per second, times the speed.
static int lua_UnitDamage(lua_State* L) {
    auto* gh = getGameHandler(L);
    // The pet's damage comes off its own fields rather than from a weapon it
    // does not carry, so it is answered before any of the equipment below.
    std::string who(luaL_optstring(L, 1, "player"));
    toLowerInPlace(who);
    if (who == "pet") {
        const double lo = gh ? gh->getPetMinDamage() : 0.0;
        const double hi = gh ? gh->getPetMaxDamage() : 0.0;
        lua_pushnumber(L, lo);   // minDamage
        lua_pushnumber(L, hi);   // maxDamage
        lua_pushnumber(L, 0.0);  // minOffHand
        lua_pushnumber(L, 0.0);  // maxOffHand
        lua_pushnumber(L, 0.0);  // physical bonus, positive
        lua_pushnumber(L, 0.0);  // ...and negative
        lua_pushnumber(L, 1.0);  // damage percent
        return 7;
    }
    const int32_t ap = gh ? gh->getMeleeAttackPower() : -1;
    double baseMin = 1.0, baseMax = 2.0;   // bare hands
    double speed = 2.0;
    double offMin = 0.0, offMax = 0.0;
    if (gh) {
        const auto& inv = gh->getInventory();
        const auto& mh = inv.getEquipSlot(game::EquipSlot::MAIN_HAND);
        if (!mh.empty() && mh.item.damageMax > 0.0f) {
            baseMin = mh.item.damageMin;
            baseMax = mh.item.damageMax;
            if (mh.item.delayMs > 0) speed = mh.item.delayMs / 1000.0;
        }
        const auto& oh = inv.getEquipSlot(game::EquipSlot::OFF_HAND);
        if (!oh.empty() && oh.item.damageMax > 0.0f) {
            offMin = oh.item.damageMin;
            offMax = oh.item.damageMax;
        }
    }
    const double apSwing = (ap > 0) ? (ap / 14.0) * speed : 0.0;
    double outMin = baseMin + apSwing, outMax = baseMax + apSwing;
    double outOffMin = offMin > 0.0 ? offMin + apSwing / 2.0 : 0.0;
    double outOffMax = offMax > 0.0 ? offMax + apSwing / 2.0 : 0.0;
    // The server's own figures when it has sent them: UNIT_FIELD_MINDAMAGE,
    // MAXDAMAGE, MINOFFHANDDAMAGE, MAXOFFHANDDAMAGE are the finished damage
    // range, attack power, talents and the dual-wield penalty already in
    // (Player::CalculateMinMaxDamage). The estimate above is the fallback.
    const auto f = unitStatFields(gh, "player");
    if (f.has(game::UF::UNIT_FIELD_MINDAMAGE, 1)) {
        const float lo = f.asFloat(game::UF::UNIT_FIELD_MINDAMAGE, 0);
        const float hi = f.asFloat(game::UF::UNIT_FIELD_MINDAMAGE, 1);
        if (hi > 0.0f) { outMin = lo; outMax = hi; }
        outOffMin = f.asFloat(game::UF::UNIT_FIELD_MINDAMAGE, 2);
        outOffMax = f.asFloat(game::UF::UNIT_FIELD_MINDAMAGE, 3);
    }
    lua_pushnumber(L, outMin);
    lua_pushnumber(L, outMax);
    lua_pushnumber(L, outOffMin);
    lua_pushnumber(L, outOffMax);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 1.0);
    return 7;
}

/// UnitRangedDamage(unit) → speed, min, max, positive, negative, percent.
///
/// UNIT_FIELD_RANGEDATTACKTIME and MINRANGEDDAMAGE/MAXRANGEDDAMAGE, which the
/// server keeps for the equipped ranged weapon. This answered a 2.8-second
/// swing of attack power alone, whatever bow or wand was in the slot.
static int lua_UnitRangedDamage(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int32_t ap = gh ? gh->getRangedAttackPower() : -1;
    double speed = 2.8;
    double swing = (ap > 0) ? (ap / 14.0) * 2.8 : 0.0;
    double lo = swing, hi = swing;
    const auto f = unitStatFields(gh, "player");
    const uint32_t ms = f.raw(game::UF::UNIT_FIELD_BASEATTACKTIME, 2);
    if (f.has(game::UF::UNIT_FIELD_MINRANGEDDAMAGE, 1)) {
        lo = f.asFloat(game::UF::UNIT_FIELD_MINRANGEDDAMAGE, 0);
        hi = f.asFloat(game::UF::UNIT_FIELD_MINRANGEDDAMAGE, 1);
        // Nothing in the ranged slot: the server zeroes both, and the sheet
        // shows a zero speed as no ranged attack.
        speed = (hi > 0.0 && ms > 0) ? ms / 1000.0 : 0.0;
    }
    lua_pushnumber(L, speed);
    lua_pushnumber(L, lo);
    lua_pushnumber(L, hi);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 1.0);
    return 6;
}

/// UnitRangedAttack(unit) → skill, modifier. As with defense, five per level.
static int lua_UnitRangedAttack(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int level = gh ? static_cast<int>(gh->getPlayerLevel()) : 1;
    lua_pushnumber(L, level * 5);
    lua_pushnumber(L, 0);
    return 2;
}

/// UnitAttackBothHands(unit) → main skill, main modifier, off skill, off
/// modifier.
static int lua_UnitAttackBothHands(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int level = gh ? static_cast<int>(gh->getPlayerLevel()) : 1;
    lua_pushnumber(L, level * 5);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, level * 5);
    lua_pushnumber(L, 0);
    return 4;
}

/// GetManaRegen() → while not casting, while casting. Both per second; the
/// paperdoll multiplies by five to show mana-per-5-seconds. The server sends
/// both figures already computed (UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER and its
/// interrupted twin), so this reads them rather than modelling the spirit and
/// intellect formula - it was a flat zero, so every caster's mana regen read 0.
static int lua_GetManaRegen(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->getManaRegen() : 0.0);
    lua_pushnumber(L, gh ? gh->getManaRegenCasting() : 0.0);
    return 2;
}

/// The several figures the sheet works out from a rating or a stat. None of
/// the conversion formulae are modelled - they are level-dependent tables the
/// server does not send - and the percentages that matter are reported
/// directly by GetCritChance, GetDodgeChance and their kind, which do come
/// from the server. Zero here loses a tooltip breakdown, not a number anyone
/// plays by.
static int lua_ZeroPercent(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}

// GetCritChanceFromAgility([unit]) → the melee crit percent the player's Agility
// gives, from the combat game tables. Was a flat zero, so the crit stat's
// flyout read "0.00% from Agility" for every character.
static int lua_GetCritChanceFromAgility(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->getMeleeCritFromAgility() : 0.0);
    return 1;
}

// GetSpellCritChanceFromIntellect([unit]) → the spell crit percent Intellect
// gives, from the spell-crit game tables. Was a flat zero.
static int lua_GetSpellCritChanceFromIntellect(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->getSpellCritFromIntellect() : 0.0);
    return 1;
}

// GetCombatRatingBonus(ratingIndex) → the percent that combat rating grants.
// The paperdoll reads it for dodge, parry, haste, defense, weapon skill and the
// other rating-based stats; it was a flat zero for all of them.
static int lua_GetCombatRatingBonus(lua_State* L) {
    auto* gh = getGameHandler(L);
    // Same 1-based CR_* → 0-based table convention as GetCombatRating above.
    const int cr = static_cast<int>(luaL_optnumber(L, 1, 0)) - 1;
    lua_pushnumber(L, gh ? gh->getCombatRatingBonus(cr) : 0.0);
    return 1;
}

// GetArmorPenetration() → the percent of the target's armour the player ignores.
// The server stores armour penetration as a rating in the combat-rating field
// (Player::UpdateArmorPenetration writes PLAYER_FIELD_COMBAT_RATING_1 +
// CR_ARMOR_PENETRATION), and the paperdoll shows the rating's converted percent
// beside it - so this is the CR_ARMOR_PENETRATION rating bonus, index 24 in the
// 0-based internal scheme. Takes no unit; the flyout only asks about the player.
static int lua_GetArmorPenetration(lua_State* L) {
    auto* gh = getGameHandler(L);
    constexpr int kCrArmorPenetration = 24;  // 0-based CombatRating enum
    lua_pushnumber(L, gh ? gh->getCombatRatingBonus(kCrArmorPenetration) : 0.0);
    return 1;
}

// Health / mana regenerated per second from Spirit - the pair the Spirit stat
// flyout shows. Were flat zeros. Only the player's Spirit and class are tracked,
// so any other unit answers zero rather than the player's value under its name.
static int lua_GetUnitHealthRegenRateFromSpirit(lua_State* L) {
    auto* gh = getGameHandler(L);
    std::string unit(luaL_optstring(L, 1, "player"));
    toLowerInPlace(unit);
    lua_pushnumber(L, (gh && unit == "player") ? gh->getHealthRegenFromSpirit() : 0.0);
    return 1;
}
static int lua_GetUnitManaRegenRateFromSpirit(lua_State* L) {
    auto* gh = getGameHandler(L);
    std::string unit(luaL_optstring(L, 1, "player"));
    toLowerInPlace(unit);
    lua_pushnumber(L, (gh && unit == "player") ? gh->getManaRegenFromSpirit() : 0.0);
    return 1;
}

/// GetUnitMaxHealthModifier(unit) → the multiplier on maximum health. One,
/// because nothing here modifies it - and one rather than zero because the
/// sheet multiplies by it.
static int lua_GetUnitMaxHealthModifier(lua_State* L) {
    lua_pushnumber(L, 1.0);
    return 1;
}

/// GetInventoryItemCooldown(unit, slot) → start, duration, enabled. All zero
/// is "nothing on cooldown", which is what the sheet checks for before doing
/// arithmetic with the first two.
/// GetInventoryItemCooldown(unit, slot) → start, duration, enable.
///
/// The same as the container one and for the same reason: an equipped trinket
/// on cooldown drew nothing on the paperdoll, which is handed over.
static int lua_GetInventoryItemCooldown(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int slot = static_cast<int>(luaL_optnumber(L, 2, 0));
    double start = 0.0, duration = 0.0;
    bool enabled = true;
    if (gh && slot >= 1 && slot <= 19) {
        const auto& sl = gh->getInventory().getEquipSlot(
            static_cast<game::EquipSlot>(slot - 1));
        if (!sl.empty()) itemUseCooldown(gh, sl.item.itemId, start, duration, &enabled);
    }
    lua_pushnumber(L, start);
    lua_pushnumber(L, duration);
    lua_pushnumber(L, enabled ? 1 : 0);
    return 3;
}

// SetPortraitTexture(texture, unit) - put a unit's face in a texture.
//
// The player's and the target's, each rendered to an offscreen image of its
// own. Any other unit leaves the texture as it was and clears every claim on
// it, so a portrait frame reused for a party member does not keep showing
// whichever of the two it last held.
//
// The texture is remembered rather than filled here: the handle is rebuilt
// whenever the portrait's render target is, so it has to be assigned every
// frame, which the render loop does for everything on this list.
static int lua_SetPortraitTexture(lua_State* L) {
    auto* tree = getWidgetTree(L);
    if (!tree || !lua_istable(L, 1)) return 0;
    // The widget id the frame table carries. widgetIdOf lives in lua_engine.cpp
    // and is not declared anywhere this file can see it.
    lua_getfield(L, 1, "__wid");
    const uint32_t id = static_cast<uint32_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    if (id == 0) return 0;

    std::string unit(luaL_optstring(L, 2, ""));
    toLowerInPlace(unit);
    // Only the units this client can build a face for. Anything else releases
    // the texture rather than claiming it, so a portrait frame reused for a
    // unit with no picture does not keep the last one it had.
    const bool answerable =
        (unit == "player" || unit == "target" || unit == "pet" ||
         unit == "focus" || unit == "npc" || unit == "questnpc" ||
         (unit.rfind("party", 0) == 0 && unit.size() == 6 &&
          unit[5] >= '1' && unit[5] <= '4'));
    tree->setPortraitUnit(id, answerable ? unit : std::string());
    return 0;
}

// --- What the barber shop calls the two things it can change ---
//
// These name a category rather than describing it, and the barber builds a
// global's name out of the answer:
//
//     BarberShopFrameSelector1Category:SetText(_G["HAIR_"..GetHairCustomization().."_STYLE"])
//
// Concatenating nil raises, and this runs from BarberShop_OnLoad - which the
// client reaches the moment a player sits down, because BARBER_SHOP_OPEN is
// fired and the interface answers it by loading the barber addon. So sitting
// in the chair took the addon down as it loaded.
//
// The hair category is NORMAL for everyone but tauren, who have horns where
// other races have hair - the barber offers them "Horn Style" and "Horn Color",
// and HAIR_HORNS_STYLE and HAIR_HORNS_COLOR are both there in globalstrings
// beside the NORMAL pair. It is the only race that differs, which is why the
// facial-hair function below has a switch and this has an if.
static int lua_GetHairCustomization(lua_State* L) {
    auto* gh = getGameHandler(L);
    const bool tauren = gh &&
        static_cast<game::Race>(gh->getPlayerRace()) == game::Race::TAUREN;
    lua_pushstring(L, tauren ? "HORNS" : "NORMAL");
    return 1;
}

/// The facial category, which genuinely differs by race - a troll's is tusks
/// and a night elf's is markings, and calling either "hair" reads as a mistake
/// rather than as a shortcut. These are the game's own categories; anything
/// unlisted falls back to NORMAL, which is what a beard is.
static int lua_GetFacialHairCustomization(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* category = "NORMAL";
    if (gh) {
        switch (static_cast<game::Race>(gh->getPlayerRace())) {
            case game::Race::NIGHT_ELF: category = "MARKINGS"; break;
            case game::Race::UNDEAD:    category = "FEATURES"; break;
            case game::Race::TAUREN:    category = "HORNS";    break;
            case game::Race::TROLL:     category = "TUSKS";    break;
            case game::Race::BLOOD_ELF: category = "EARRINGS"; break;
            default: break;
        }
    }
    lua_pushstring(L, category);
    return 1;
}

/// CanAlterSkin() - whether the barber offers a fourth selector for skin.
///
/// False, and not merely for want of data: answering yes would put a selector
/// on screen that this client cannot fill, and the barber tests every selector
/// it has drawn before it will let the player buy anything.
static int lua_CanAlterSkin(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

// --- The barber's selectors, its price and its two exits ---
//
// The style lists and the originals used to be built inside this client's own
// barber window, so with the panel handed over nothing had built them. They
// come from WindowManager through LuaServices now, from a version that runs
// whether or not this client is drawing the chair.

/// GetBarberShopStyleInfo(selector) → name, category, cost, isCurrent
///
/// blizzard_barbershopui.lua reads the first and the fourth, and reads the
/// fourth through select(4, ...) - so all four have to be there for the index
/// to land on the right one.
static int lua_GetBarberShopStyleInfo(lua_State* L) {
    const int selector = static_cast<int>(luaL_optinteger(L, 1, 0));
    auto* svc = getLuaServices(L);
    std::string name;
    bool isCurrent = false;
    if (!svc || !svc->getBarberStyleInfo ||
        !svc->getBarberStyleInfo(selector, name, isCurrent)) {
        return 0;
    }
    lua_pushstring(L, name.c_str());
    lua_pushstring(L, "");     // category, which this client does not name
    lua_pushinteger(L, 0);     // per-selector cost; the total is the one asked for
    lua_pushboolean(L, isCurrent ? 1 : 0);
    return 4;
}

/// SetNextBarberShopStyle(selector, direction) - the arrows beside a selector.
///
/// The forward arrow passes 1 and the back arrow passes nothing at all
/// (blizzard_barbershopui.xml:31 and :56), so an absent argument means back
/// rather than a missing parameter.
static int lua_SetNextBarberShopStyle(lua_State* L) {
    const int selector = static_cast<int>(luaL_optinteger(L, 1, 0));
    const int direction = lua_isnoneornil(L, 2) ? -1 : 1;
    if (auto* svc = getLuaServices(L); svc && svc->setNextBarberStyle) {
        svc->setNextBarberStyle(selector, direction);
    }
    return 0;
}

/// GetBarberShopTotalCost() → copper for everything currently changed.
static int lua_GetBarberShopTotalCost(lua_State* L) {
    auto* svc = getLuaServices(L);
    lua_pushinteger(L, svc && svc->getBarberTotalCost
                           ? static_cast<lua_Integer>(svc->getBarberTotalCost()) : 0);
    return 1;
}

/// BarberShopReset() - put every selector back to what the character wears.
static int lua_BarberShopReset(lua_State* L) {
    if (auto* svc = getLuaServices(L); svc && svc->barberReset) svc->barberReset();
    return 0;
}

/// CancelBarberShop() - leave the chair without buying anything.
static int lua_CancelBarberShop(lua_State* L) {
    // The preview has been changing the character as the selectors moved, so
    // leaving without buying has to put them back. Before the close, because
    // closing the shop is what drops the state the reset is read from.
    if (auto* svc = getLuaServices(L); svc && svc->barberReset) svc->barberReset();
    if (auto* gh = getGameHandler(L)) gh->closeBarberShop();
    return 0;
}

/// GetMaxCombatRatingBonus(rating) → the cap on what a rating can give. Zero
/// would read as "capped at nothing", so this answers with a hundred percent,
/// which is no cap in any practical sense.
static int lua_GetMaxCombatRatingBonus(lua_State* L) {
    lua_pushnumber(L, 100);
    return 1;
}

/// GetRestState() → stateID, stateName, xpMultiplier.
///
/// One is rested and two is normal, which is the way round WoW numbers them -
/// this answered the opposite. MainMenuBar multiplies by the third return the
/// line after reading it, so returning only the id was arithmetic on nil every
/// time the cursor crossed the experience bar.
///
/// Rested means there is rested experience left to spend, not that the player
/// is standing in an inn. Those are different questions and this asked the
/// second: it read IsResting, which ends the moment you walk outside, while
/// the pool you earned in there is spent over the next several levels. So the
/// bar went back to the normal purple with the exhaustion tick still several
/// bubbles ahead of the fill - the two disagreeing on screen about the same
/// thing.
///
/// IsResting reads PLAYER_FLAGS 0x20, which is the right source for the
/// "Resting" indicator it feeds.
static int lua_GetRestState(lua_State* L) {
    auto* gh = getGameHandler(L);
    const bool rested = gh && gh->getPlayerRestedXp() > 0;
    lua_pushnumber(L, rested ? 1 : 2);
    lua_pushstring(L, rested ? "Rested" : "Normal");
    lua_pushnumber(L, rested ? 1.5 : 1.0);
    return 3;
}

// --- Quest Log API ---

static int lua_UnitAffectingCombat(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    // UNIT_FLAG_IN_COMBAT (0x00080000) in UNIT_FIELD_FLAGS, for the player as
    // for anyone: the stock function (0x60f860) reads only that bit. The
    // player used to answer from the client's own guess (auto-attack or a
    // remembered attacker) instead, which outlives the server's flag.
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    bool inCombat = false;
    if (guid != 0) {
        auto entity = gh->getEntityManager().getEntity(guid);
        if (entity) {
            uint32_t flags = entity->getField(
                game::fieldIndex(game::UF::UNIT_FIELD_FLAGS));
            inCombat = (flags & 0x00080000) != 0;
        }
    }
    lua_pushboolean(L, inCombat);
    return 1;
}



// HasFullControl() - whether the player is in command of their own character
//
// Read as `not HasFullControl()` to grey out the right-click menu, so an absent
// answer disabled duelling, trading and inviting permanently. Nothing here
// charms, fears or confuses anyone, so control is never lost and saying so is
// the truthful answer rather than a convenient one.
// GetPetSpellBonusDamage() → the spell power a pet inherits
//
// The pet tab formats this with %d before testing it, and %d against nil raises
// on the spot - on a tab of the character frame, which is drawn by default.
// Nothing here works out what a pet inherits, and zero is the honest figure
// rather than a guess at a scaling rule.
static int lua_GetPetSpellBonusDamage(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}

static int lua_HasFullControl(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}

// --- Dying, and getting back up ---
//
// The death popup already appears; its buttons had nothing behind them, so a
// player could be told they had died and be unable to do anything about it.

// RepopMe() - release the spirit and run back

// RetrieveCorpse() - resurrect where the body is
static int lua_RetrieveCorpse(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->reclaimCorpse();
    return 0;
}

// Dismount() - what /dismount does
static int lua_Dismount(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->dismount();
    return 0;
}

// StartAttack([unit]) / StopAttack() - swing at what is targeted
//
// The interface passes a unit only to attack something other than the current
// target, which this client cannot do without changing target first, so the
// argument is read and the current target used.
static int lua_StartAttack(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const uint64_t target = gh->getTargetGuid();
    if (target != 0) gh->startAutoAttack(target);
    return 0;
}

// AttackTarget() - the ATTACKTARGET binding's body and a common macro line.
// It toggles: auto attack on the current target, or off when it is already
// swinging at that target (stock 3.3.5a). Missing, the binding raised a Lua
// error and the macro did nothing.
static int lua_AttackTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const uint64_t target = gh->getTargetGuid();
    if (target == 0) {
        // Nothing to swing at, which stock answers with nothing either; said
        // in the log so a binding that did nothing is not mistaken for one
        // that was never wired.
        static core::LogBudget noTarget(5, "AttackTarget with no target");
        if (noTarget.take()) LOG_WARNING("AttackTarget: no target, nothing to attack");
        return 0;
    }
    if (gh->isAutoAttacking() && gh->getAutoAttackTargetGuid() == target) {
        gh->stopAutoAttack();
    } else {
        gh->startAutoAttack(target);
    }
    return 0;
}

static int lua_StopAttack(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->stopAutoAttack();
    return 0;
}

// GetReleaseTimeRemaining() → milliseconds before the corpse releases itself,
// or -1 when nothing will force it
//
// UIParent asks this the moment the player dies, as `> 0 or == -1`, so an
// absent answer is an error on every death rather than a popup that does not
// appear. Nothing here counts down to a forced release, and -1 is the game's
// own way of saying so.

/// GetNumRaidMembers() → everyone in the raid, this player included, or nought
/// when the group is not a raid.
///
/// The player is counted because the interface counts them: GetDisplayedAllyFrames
/// compares this against MEMBERS_PER_RAID_GROUP to decide between the party
/// frames and the raid frames, and a raid of five that answered four would be
/// drawn as a party for ever.
static int lua_GetNumRaidMembers(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->isInGroup()) { return luaReturnZero(L); }
    // The same list raidN and GetRaidRosterInfo index, so the three agree.
    lua_pushnumber(L, static_cast<int>(raidRoster(gh).size()));
    return 1;
}

/// GetNumPartyMembers() → the other members of this player's own party.
///
/// The roster the server sends is already written without the player in it -
/// SMSG_GROUP_LIST carries members-minus-one and then that many entries - and
/// this subtracted one from it again. A pair answered nought, which is a party
/// the interface draws no frames for at all, and a five-man answered three and
/// lost its last member.
///
/// In a raid it is the player's own subgroup that is asked about, which is what
/// the party frames along the left of the screen show for a raid of one group.
static int lua_GetNumPartyMembers(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->isInGroup()) { return luaReturnZero(L); }
    const auto& pd = gh->getPartyData();
    if (!game::groupIsRaid(pd.groupType)) {
        lua_pushnumber(L, static_cast<int>(pd.members.size()));
        return 1;
    }
    int inSubgroup = 0;
    for (const auto& member : pd.members) {
        if (member.subGroup == pd.subGroup) ++inSubgroup;
    }
    lua_pushnumber(L, inSubgroup);
    return 1;
}

// GetPartyMember(index) → 1 when that party slot is filled. The party frames
// show and hide on it.
static int lua_GetPartyMember(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(luaL_checkinteger(L, 1));
    if (!gh || index < 1 || index > 4) { return luaReturnNil(L); }
    if (resolveUnitGuid(gh, "party" + std::to_string(index)) == 0) { return luaReturnNil(L); }
    lua_pushnumber(L, 1);
    return 1;
}

static int lua_UnitInParty(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    if (uidStr == "player") {
        lua_pushboolean(L, gh->isInGroup());
    } else {
        uint64_t guid = resolveUnitGuid(gh, uidStr);
        if (guid == 0) { return luaReturnFalse(L); }
        const auto& pd = gh->getPartyData();
        bool found = false;
        for (const auto& m : pd.members) {
            if (m.guid == guid) { found = true; break; }
        }
        lua_pushboolean(L, found);
    }
    return 1;
}

static int lua_UnitInRaid(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    // The unit's raid index counted from zero, or nil - not a boolean.
    // TargetFrameDropDown_Initialize does `id = UnitInRaid("target")` and then
    // GetRaidRosterInfo(id + 1); a true there is arithmetic on a boolean.
    const int index = raidIndexOf(gh, resolveUnitGuid(gh, uidStr));
    if (index == 0) { return luaReturnNil(L); }
    lua_pushnumber(L, index - 1);
    return 1;
}

// GetRaidRosterInfo(index) → name, rank, subgroup, level, class, fileName, zone, online, isDead, role, isML
static int lua_GetRaidRosterInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) { return luaReturnNil(L); }
    // The raid with the player in it, the list raidN and GetNumRaidMembers
    // use. Indexing the server's list - which leaves the player out - lost
    // the last member and never showed the player.
    const auto raid = raidRoster(gh);
    if (index > static_cast<int>(raid.size())) { return luaReturnNil(L); }
    const RaidSlot& s = raid[static_cast<size_t>(index) - 1];
    const game::GroupMember* m = s.member;
    const auto& pd = gh->getPartyData();
    const bool self = (m == nullptr);

    std::string name = self ? gh->lookupName(s.guid) : m->name;
    if (self && name.empty()) {
        if (const auto* ch = gh->getActiveCharacter()) name = ch->name;
    }
    lua_pushstring(L, name.c_str());                         // 1: name
    lua_pushnumber(L, s.guid == pd.leaderGuid ? 2 : ((s.flags & 0x01) ? 1 : 0)); // 2: rank
    lua_pushnumber(L, s.subGroup + 1);                       // 3: subgroup (1-indexed)

    // Level and zone from the party stats once they have come; the player's
    // own from the player.
    auto entity = gh->getEntityManager().getEntity(s.guid);
    uint32_t level = self ? gh->getPlayerLevel() : (m->level ? m->level : 0u);
    if (level == 0 && entity) level = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL));
    lua_pushnumber(L, level);                                // 4: level

    // Class from the unit when it is in range, else the name query's cache,
    // else nil. blizzard_raidui files the row under RAID_SUBGROUP_LISTS
    // [fileName] when there is one, and "Unknown" is a key that table does
    // not have - tinsert into nil raised and took the raid tab down.
    uint8_t classId = self ? gh->getPlayerClass() : 0;
    if (classId == 0 && entity) {
        const uint32_t bytes0 = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
        classId = static_cast<uint8_t>((bytes0 >> 8) & 0xFF);
    }
    if (classId == 0) classId = gh->lookupPlayerClass(s.guid);
    if (classId > 0 && classId < 12) {
        lua_pushstring(L, kLuaClasses[classId]);             // 5: class (localized)
        luaPushClassToken(L, classId);                       // 6: fileName
    } else {
        lua_pushnil(L);
        lua_pushnil(L);
    }

    // From C++ for the player too: calling GetRealZoneText here would pop a
    // result off the stack between the return values already pushed, which
    // slides the return window (tools/lua_return_window_check.py).
    std::string zone;
    if (self) {
        zone = gh->getWhoAreaName(gh->getWorldStateZoneId());
    } else if (m->hasPartyStats && m->zoneId != 0) {
        zone = gh->getWhoAreaName(m->zoneId);
    }
    if (zone.empty()) lua_pushnil(L); else lua_pushstring(L, zone.c_str()); // 7: zone
    lua_pushboolean(L, s.online ? 1 : 0);                    // 8: online
    bool dead = false;
    if (self) {
        if (entity) dead = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_HEALTH)) == 0;
    } else {
        dead = m->hasPartyStats && m->curHealth == 0 && s.online;
    }
    lua_pushboolean(L, dead ? 1 : 0);                        // 9: isDead
    // 10: role - "MAINTANK" or "MAINASSIST" from the member flags
    // (MEMBER_FLAG_MAINTANK 0x02, MAINASSIST 0x04), else nil. The raid UI
    // does tinsert(RAID_SUBGROUP_LISTS[role], i) whenever role is set, and
    // has lists for those two only - "NONE" indexed nothing and raised.
    if (s.flags & 0x02)      lua_pushstring(L, "MAINTANK");
    else if (s.flags & 0x04) lua_pushstring(L, "MAINASSIST");
    else                     lua_pushnil(L);
    // 11: isML - the master looter, when master loot is the method.
    lua_pushboolean(L, (pd.lootMethod == 2 && pd.looterGuid == s.guid) ? 1 : 0);
    return 11;
}

// GetThreatStatusColor(statusIndex) → r, g, b
static int lua_GetThreatStatusColor(lua_State* L) {
    int status = static_cast<int>(luaL_optnumber(L, 1, 0));
    switch (status) {
        case 0: lua_pushnumber(L, 0.69f); lua_pushnumber(L, 0.69f); lua_pushnumber(L, 0.69f); break; // gray (no threat)
        case 1: lua_pushnumber(L, 1.0f);  lua_pushnumber(L, 1.0f);  lua_pushnumber(L, 0.47f); break; // yellow (threat)
        case 2: lua_pushnumber(L, 1.0f);  lua_pushnumber(L, 0.6f);  lua_pushnumber(L, 0.0f);  break; // orange (high threat)
        case 3: lua_pushnumber(L, 1.0f);  lua_pushnumber(L, 0.0f);  lua_pushnumber(L, 0.0f);  break; // red (tanking)
        default: lua_pushnumber(L, 1.0f); lua_pushnumber(L, 1.0f);  lua_pushnumber(L, 1.0f);  break;
    }
    return 3;
}

// GetReadyCheckStatus(unit) → status string
//
// "waiting", "ready" or "notready" while a check is under way, nil otherwise.
// PlayerFrame_UpdateReadyCheck and PartyMemberFrame_UpdateReadyCheck draw the
// check icons from nothing else, so a stub nil meant no icon on any frame.
static int lua_GetReadyCheckStatus(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return luaReturnNil(L);
    std::string unit = luaL_optstring(L, 1, "player");
    toLowerInPlace(unit);
    const char* status = gh->readyCheckStatusFor(resolveUnitGuid(gh, unit));
    if (!status) return luaReturnNil(L);
    lua_pushstring(L, status);
    return 1;
}

// RegisterUnitWatch / UnregisterUnitWatch - secure unit frame stubs
static int lua_RegisterUnitWatch(lua_State* L) { (void)L; return 0; }
static int lua_UnregisterUnitWatch(lua_State* L) { (void)L; return 0; }

static int lua_UnitIsUnit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    const char* uid1 = luaL_checkstring(L, 1);
    const char* uid2 = luaL_checkstring(L, 2);
    std::string u1(uid1), u2(uid2);
    toLowerInPlace(u1);
    toLowerInPlace(u2);
    uint64_t g1 = resolveUnitGuid(gh, u1);
    uint64_t g2 = resolveUnitGuid(gh, u2);
    lua_pushboolean(L, g1 != 0 && g1 == g2);
    return 1;
}

/// UnitIsFriend(unit1, unit2) - is unit2 friendly to unit1.
///
/// Two units, and the answer is about the *second*. This read only the first,
/// which every caller in the interface passes as "player" - and the player is
/// never hostile, so it answered true for whatever was being asked about.
/// targetframe.lua colours the name from it, picks the debuff layout from it
/// and filters with it, so every target read as a friend.
///
/// Hostility is modelled relative to the player, so the unit that carries the
/// answer is whichever of the pair is *not* the player - and the interface
/// passes both orders: UnitIsFriend("player", self.unit) beside
/// UnitIsEnemy(self.unit, "player"). Keying on position would be right for one
/// of them and wrong for the other.
static game::Unit* unitAskedAbout(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* a = luaL_optstring(L, 1, "player");
    const char* b = luaL_optstring(L, 2, nullptr);
    if (!b) return resolveUnit(L, a);
    if (!gh) return resolveUnit(L, b);
    std::string ua(a), ub(b);
    toLowerInPlace(ua);
    toLowerInPlace(ub);
    const uint64_t player = gh->getPlayerGuid();
    if (resolveUnitGuid(gh, ua) == player) return resolveUnit(L, b);
    if (resolveUnitGuid(gh, ub) == player) return resolveUnit(L, a);
    // Neither is the player: answered about the second, which is the one the
    // question is grammatically about.
    return resolveUnit(L, b);
}

static int lua_UnitIsFriend(lua_State* L) {
    auto* gh = getGameHandler(L);
    auto* unit = unitAskedAbout(L);
    lua_pushboolean(L, gh && unit && isFriendlyUnit(gh, *unit));
    return 1;
}

/// UnitIsEnemy(unit1, unit2) - is unit2 hostile to unit1. The same shape as
/// UnitIsFriend above, and it was wrong the same way: asked about the player it
/// answered false for every enemy.
static int lua_UnitIsEnemy(lua_State* L) {
    auto* unit = unitAskedAbout(L);
    auto* gh = getGameHandler(L);
    // Reaction, not attackability: a neutral beast is attackable but not an enemy.
    lua_pushboolean(L, unit && gh && gh->unitReactionToPlayer(*unit) <= 2);
    return 1;
}

static int lua_UnitCreatureType(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, "Unknown"); return 1; }
    const char* uid = luaL_optstring(L, 1, "target");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { lua_pushstring(L, "Unknown"); return 1; }
    auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity) { lua_pushstring(L, "Unknown"); return 1; }
    // Player units are always "Humanoid"
    if (entity->getType() == game::ObjectType::PLAYER) {
        lua_pushstring(L, "Humanoid");
        return 1;
    }
    auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
    if (!unit) { lua_pushstring(L, "Unknown"); return 1; }
    uint32_t ct = gh->getCreatureType(unit->getEntry());
    static constexpr const char* kTypes[] = {
        "Unknown", "Beast", "Dragonkin", "Demon", "Elemental",
        "Giant", "Undead", "Humanoid", "Critter", "Mechanical",
        "Not specified", "Totem", "Non-combat Pet", "Gas Cloud"
    };
    lua_pushstring(L, (ct < 14) ? kTypes[ct] : "Unknown");
    return 1;
}

// GetPlayerInfoByGUID(guid) → localizedClass, englishClass, localizedRace, englishRace, sex, name, realm
static int lua_GetPlayerInfoByGUID(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* guidStr = luaL_checkstring(L, 1);
    if (!gh || !guidStr) {
        for (int i = 0; i < 7; i++) lua_pushnil(L);
        return 7;
    }
    // Parse hex GUID string "0x0000000000000001"
    uint64_t guid = 0;
    if (guidStr[0] == '0' && (guidStr[1] == 'x' || guidStr[1] == 'X'))
        guid = strtoull(guidStr + 2, nullptr, 16);
    else
        guid = strtoull(guidStr, nullptr, 16);

    if (guid == 0) { for (int i = 0; i < 7; i++) lua_pushnil(L); return 7; }

    // Look up entity name
    std::string name = gh->lookupName(guid);
    if (name.empty() && guid == gh->getPlayerGuid()) {
        const auto& chars = gh->getCharacters();
        for (const auto& c : chars)
            if (c.guid == guid) { name = c.name; break; }
    }

    // For player GUID, return class/race if it's the local player
    const char* className = "Unknown";
    const char* raceName = "Unknown";
    if (guid == gh->getPlayerGuid()) {
        uint8_t cid = gh->getPlayerClass();
        uint8_t rid = gh->getPlayerRace();
        if (cid < 12) className = kLuaClasses[cid];
        if (rid < 12) raceName = kLuaRaces[rid];
    }

    lua_pushstring(L, className);  // 1: localizedClass
    lua_pushstring(L, className);  // 2: englishClass
    lua_pushstring(L, raceName);   // 3: localizedRace
    lua_pushstring(L, raceName);   // 4: englishRace
    lua_pushnumber(L, 0);          // 5: sex (0=unknown)
    lua_pushstring(L, name.c_str()); // 6: name
    lua_pushstring(L, "");         // 7: realm
    return 7;
}


static int lua_UnitClassification(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, "normal"); return 1; }
    const char* uid = luaL_optstring(L, 1, "target");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { lua_pushstring(L, "normal"); return 1; }
    auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity || entity->getType() == game::ObjectType::PLAYER) {
        lua_pushstring(L, "normal");
        return 1;
    }
    auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
    if (!unit) { lua_pushstring(L, "normal"); return 1; }
    int rank = gh->getCreatureRank(unit->getEntry());
    switch (rank) {
        case 1:  lua_pushstring(L, "elite"); break;
        case 2:  lua_pushstring(L, "rareelite"); break;
        case 3:  lua_pushstring(L, "worldboss"); break;
        case 4:  lua_pushstring(L, "rare"); break;
        default: lua_pushstring(L, "normal"); break;
    }
    return 1;
}

// GetComboPoints("player"|"vehicle", "target") → number
//
// The points are on one unit: SMSG_UPDATE_COMBO_POINTS names the guid they are
// built on, and in 3.3.5 they stay there when the player changes target. So
// the count is the target's only while the target is that unit - a rogue who
// tabs to a fresh mob sees an empty combo frame, and the five come back on
// tabbing back. Answering the bare count lit the frame on any target.
static int lua_GetComboPoints(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); return 1; }
    std::string unit = luaL_optstring(L, 1, "player");
    std::string target = luaL_optstring(L, 2, "target");
    toLowerInPlace(unit);
    toLowerInPlace(target);
    // Only the player's own points are tracked; a vehicle's arrive on
    // SMSG_PET_UPDATE_COMBO_POINTS, which this client does not read.
    if (unit != "player") { lua_pushnumber(L, 0); return 1; }
    const uint64_t targetGuid = resolveUnitGuid(gh, target);
    const bool onIt = targetGuid != 0 && targetGuid == gh->getComboTarget();
    lua_pushnumber(L, onIt ? gh->getComboPoints() : 0);
    return 1;
}

// UnitReaction(unit, otherUnit) → 1-8 (hostile to exalted)
static int lua_UnitReaction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    std::string u1(luaL_checkstring(L, 1));
    toLowerInPlace(u1);
    std::string u2(luaL_checkstring(L, 2));
    toLowerInPlace(u2);
    if (!resolveUnit(L, u2.c_str())) { return luaReturnNil(L); }
    if (resolveUnitGuid(gh, u1) == resolveUnitGuid(gh, u2)) {
        lua_pushnumber(L, 5); // same unit = friendly
        return 1;
    }
    // How the unit regards the player, which is the question the tooltip and
    // the name colours ask - UnitReaction(unit, "player"), in either order and
    // for players as well as creatures. For a faction with a standing that is
    // the standing: answered hostile-or-friendly, the Kurenai read as friendly
    // to an Alliance player at Unfriendly, who could not then find out why
    // Telaar would not speak to them.
    auto* unit = unitAskedAbout(L);
    if (!unit) { return luaReturnNil(L); }
    lua_pushnumber(L, gh->unitReactionToPlayer(*unit));
    return 1;
}

// UnitIsConnected(unit) → boolean
static int lua_UnitIsConnected(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    const char* uid = luaL_optstring(L, 1, "player");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnFalse(L); }
    // Player is always connected
    if (guid == gh->getPlayerGuid()) { lua_pushboolean(L, 1); return 1; }
    // Check party/raid member online status
    const auto& pd = gh->getPartyData();
    for (const auto& m : pd.members) {
        if (m.guid == guid) {
            lua_pushboolean(L, m.isOnline ? 1 : 0);
            return 1;
        }
    }
    // Non-party entities that exist are considered connected
    auto entity = gh->getEntityManager().getEntity(guid);
    lua_pushboolean(L, entity ? 1 : 0);
    return 1;
}

// HasAction(slot) → boolean (1-indexed slot)

void registerUnitLuaAPI(lua_State* L) {
    static const struct { const char* name; lua_CFunction func; } api[] = {
                {"UnitName",      lua_UnitName},
                {"UnitFullName",  lua_UnitName},
                {"GetUnitName",   lua_UnitName},
                {"UnitHealth",    lua_UnitHealth},
                {"UnitHealthMax", lua_UnitHealthMax},
                {"UnitPower",     lua_UnitPower},
                {"UnitPowerMax",  lua_UnitPowerMax},
                {"UnitMana",      lua_UnitPower},
                {"UnitManaMax",   lua_UnitPowerMax},
                {"UnitRage",      lua_UnitPower},
                {"UnitEnergy",    lua_UnitPower},
                {"UnitFocus",     lua_UnitPower},
                {"UnitRunicPower", lua_UnitPower},
                {"UnitLevel",     lua_UnitLevel},
                {"UnitExists",    lua_UnitExists},
                {"UnitIsDead",    lua_UnitIsDead},
                {"UnitIsFeignDeath", lua_UnitIsFeignDeath},
                // UnitUsingVehicle(unit) - in a vehicle's seat or on the way
                // into or out of one (passenger state != 0, 0x6134A0).
                {"UnitUsingVehicle", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint64_t guid = vehicleUnitArg(L, gh);
            lua_pushboolean(L, (guid && gh->vehiclePassenger(guid)) ? 1 : 0);
            return 1;
        }},
                {"UnitVehicleSeatInfo",      lua_UnitVehicleSeatInfo},
                {"UnitVehicleSeatCount",     lua_UnitVehicleSeatCount},
                {"UnitSwitchToVehicleSeat",  lua_UnitSwitchToVehicleSeat},
                {"CanSwitchVehicleSeat",     lua_CanSwitchVehicleSeat},
                {"UnitInVehicleControlSeat", lua_UnitInVehicleControlSeat},
                {"UnitIsGhost",   lua_UnitIsGhost},
                {"UnitIsDeadOrGhost", lua_UnitIsDeadOrGhost},
                {"UnitIsAFK",     lua_UnitIsAFK},
                {"UnitIsDND",     lua_UnitIsDND},
                {"UnitPlayerControlled", lua_UnitPlayerControlled},
                {"UnitIsTapped",        lua_UnitIsTapped},
                {"UnitIsTappedByPlayer", lua_UnitIsTappedByPlayer},
                {"UnitIsTappedByAllThreatList", lua_UnitIsTappedByAllThreatList},
                {"UnitIsVisible",       lua_UnitIsVisible},
                {"UnitGroupRolesAssigned", lua_UnitGroupRolesAssigned},
                {"UnitCanAttack",       lua_UnitCanAttack},
                {"UnitCanCooperate",    lua_UnitCanCooperate},
                {"UnitCreatureFamily",  lua_UnitCreatureFamily},
                {"UnitOnTaxi",          lua_UnitOnTaxi},
                {"UnitThreatSituation", lua_UnitThreatSituation},
                {"UnitDetailedThreatSituation", lua_UnitDetailedThreatSituation},
                {"UnitSex",       lua_UnitSex},
                {"UnitClass",     lua_UnitClass},
                {"UnitArmor",     [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            // Whose armour. PaperDollFrame_SetArmor is shared between the two
            // sheets and the pet tab calls it with "Pet" - capitalised, which
            // is why this compares lowered.
            std::string who(luaL_optstring(L, 1, "player"));
            toLowerInPlace(who);
            int32_t armor = 0;
            if (gh) {
                armor = (who == "pet") ? gh->getPetResistances()[0]
                      : (who == "player") ? gh->getArmorRating() : 0;
            }
            if (armor < 0) armor = 0;
            // base, effectiveArmor, armor, posBuff, negBuff. The sheet prints
            // base + posBuff + negBuff (PaperDollFormatStat), so the base is
            // what is left of the total once the buffs - UNIT_FIELD_
            // RESISTANCEBUFFMODSPOSITIVE/NEGATIVE, school 0 - are taken off.
            const auto f = unitStatFields(gh, who);
            const int32_t pos = f.asInt(game::UF::UNIT_FIELD_RESISTANCES, 7);
            const int32_t neg = f.asInt(game::UF::UNIT_FIELD_RESISTANCES, 14);
            lua_pushnumber(L, armor - pos - neg); // base
            lua_pushnumber(L, armor);             // effective
            lua_pushnumber(L, armor);             // armor
            lua_pushnumber(L, pos);
            lua_pushnumber(L, neg);
            return 5;
        }},
                {"UnitResistance", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int school = static_cast<int>(luaL_optnumber(L, 2, 0));
            int32_t val = 0;
            // Whose resistance, for the same reason as UnitStat above.
            std::string who(luaL_optstring(L, 1, "player"));
            toLowerInPlace(who);
            if (gh && school >= 0 && school <= 6) {
                if (who == "pet") {
                    val = gh->getPetResistances()[static_cast<size_t>(school)];
                } else if (who == "player") {
                    // Physical is armor, and it is index zero of the same block.
                    val = (school == 0) ? gh->getArmorRating() : gh->getResistance(school);
                }
            }
            if (val < 0) val = 0;
            // base, resistance, positive, negative: the buff parts from the
            // two RESISTANCEBUFFMODS arrays (Unit::UpdateResistanceBuffModsMod)
            // and the base what remains. The resistance frame colours the
            // value and builds its "( base +pos -neg )" tooltip from them.
            int32_t pos = 0, neg = 0;
            if (school >= 0 && school <= 6) {
                const auto f = unitStatFields(gh, who);
                pos = f.asInt(game::UF::UNIT_FIELD_RESISTANCES, 7 + school);
                neg = f.asInt(game::UF::UNIT_FIELD_RESISTANCES, 14 + school);
            }
            lua_pushnumber(L, val - pos - neg); // base
            lua_pushnumber(L, val);             // effective
            lua_pushnumber(L, pos);
            lua_pushnumber(L, neg);
            return 4;
        }},
                {"UnitStat",      lua_UnitStat},
                {"GetDodgeChance",    lua_GetDodgeChance},
                {"GetParryChance",    lua_GetParryChance},
                {"GetBlockChance",    lua_GetBlockChance},
                {"GetCritChance",     lua_GetCritChance},
                {"GetRangedCritChance", lua_GetRangedCritChance},
                {"GetSpellCritChance",  lua_GetSpellCritChance},
                {"GetCombatRating",     lua_GetCombatRating},
                {"GetSpellBonusDamage", lua_GetSpellBonusDamage},
                {"GetSpellBonusHealing", lua_GetSpellBonusHealing},
                {"GetAttackPowerForStat", lua_GetAttackPower},
                {"GetRangedAttackPower",  lua_GetRangedAttackPower},
                {"IsInGroup",     lua_IsInGroup},
                {"IsInRaid",      lua_IsInRaid},
                {"GetShapeshiftFormInfo", [](lua_State* L) -> int {
            // GetShapeshiftFormInfo(index) → icon, name, isActive, isCastable
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            if (!gh || index < 1) { return luaReturnNil(L); }
            uint8_t classId = gh->getPlayerClass();
            uint8_t currentForm = gh->getShapeshiftFormId();

            // The one table CastShapeshiftForm and GetNumShapeshiftForms
            // also read, filtered the same way. The bar walks
            // 1..GetNumShapeshiftForms() and asks about each index, so all
            // three have to be looking at the same list in the same order.
            const auto forms = game::knownShapeshiftForms(classId, gh->getKnownSpells());
            if (static_cast<size_t>(index) > forms.size()) { return luaReturnNil(L); }
            const auto& fi = forms[static_cast<size_t>(index) - 1];
            lua_pushstring(L, fi.icon);                          // icon
            lua_pushstring(L, fi.name);                          // name
            lua_pushboolean(L, currentForm == fi.formId ? 1 : 0); // isActive
            lua_pushboolean(L, 1);                               // isCastable
            return 4;
        }},
                // The honour panel prints "(Rank N)" from the second return
                // and concatenates it unguarded, so a missing one takes the
                // panel's whole update with it. Rank zero is what an
                // unranked character has.
                // Whether the off hand holds a weapon rather than a shield or
                // a held item. DurabilityFrame_SetAlerts branches on it to
                // decide which of its two off-hand icons to show.
                {"OffhandHasWeapon", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) { lua_pushboolean(L, 0); return 1; }
            const auto& slot = gh->getInventory().getEquipSlot(game::EquipSlot::OFF_HAND);
            if (slot.empty()) { lua_pushboolean(L, 0); return 1; }
            const auto* info = gh->getItemInfo(slot.item.itemId);
            const uint8_t t = info ? info->inventoryType : 0;
            // A weapon, not a shield or a held book: OFF_HAND in this table
            // is the held-in-off-hand class, which is not one.
            lua_pushboolean(L, (t == game::InvType::ONE_HAND ||
                                t == game::InvType::TWO_HAND) ? 1 : 0);
            return 1;
        }},
                // The colour a unit's name is drawn in: red for hostile, orange
                // for unfriendly, yellow for neutral, green for friendly. Unit
                // frames read all four components straight into SetTextColor.
                {"UnitSelectionColor", [](lua_State* L) -> int {
            const char* uid = luaL_optstring(L, 1, "player");
            auto* unit = resolveUnit(L, uid);
            float r = 0.0f, g = 1.0f, b = 0.0f;
            auto* gh = getGameHandler(L);
            if (unit && gh && unit->getType() == game::ObjectType::UNIT) {
                // A creature by how it regards the player, standing and all.
                const int reaction = gh->unitReactionToPlayer(*unit);
                if (reaction <= 2)      { r = 1.0f; g = 0.0f; }
                else if (reaction == 3) { r = 1.0f; g = 0.5f; }
                else if (reaction == 4) { r = 1.0f; g = 1.0f; }
            } else if (unit && unit->isHostile()) { r = 1.0f; g = 0.0f; b = 0.0f; }
            lua_pushnumber(L, r);
            lua_pushnumber(L, g);
            lua_pushnumber(L, b);
            lua_pushnumber(L, 1.0);
            return 4;
        }},
                {"UnitIsPartyLeader", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* uid = luaL_optstring(L, 1, "player");
            std::string uidStr(uid);
            toLowerInPlace(uidStr);
            const uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
            lua_pushboolean(L, (gh && guid != 0 &&
                                gh->getPartyData().leaderGuid == guid) ? 1 : 0);
            return 1;
        }},
                {"UnitIsCorpse", [](lua_State* L) -> int {
            const char* uid = luaL_optstring(L, 1, "player");
            auto* unit = resolveUnit(L, uid);
            lua_pushboolean(L, (unit && unit->getHealth() == 0) ? 1 : 0);
            return 1;
        }},
                // Whether the first unit may help the second - only a friendly
                // one, never neutral wildlife.
                {"UnitCanAssist", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            auto* other = resolveUnit(L, luaL_optstring(L, 2, "target"));
            lua_pushboolean(L, (gh && other && isFriendlyUnit(gh, *other)) ? 1 : 0);
            return 1;
        }},
                {"GetPVPRankInfo", [](lua_State* L) -> int {
            const int rank = static_cast<int>(luaL_optnumber(L, 1, 0));
            lua_pushstring(L, "");          // rank name
            lua_pushnumber(L, rank > 0 ? rank : 0);
            return 2;
        }},
                {"UnitRace",          lua_UnitRace},
                {"UnitPowerType",     lua_UnitPowerType},
                {"GetNumGroupMembers", lua_GetNumGroupMembers},
                {"UnitGUID",          lua_UnitGUID},
                {"UnitIsPlayer",      lua_UnitIsPlayer},
                {"UnitPVPName",       lua_UnitPVPName},
                {"InCombatLockdown",  lua_InCombatLockdown},
                {"UnitDistanceSquared", lua_UnitDistanceSquared},
                {"CheckInteractDistance", lua_CheckInteractDistance},
                {"UnitInRange",           lua_UnitInRange},
                {"IsRaidLeader",          lua_IsRaidLeader},
                // The class token without the localised name beside it, which
                // is all UnitClassBase is: the same second return UnitClass
                // already computes.
                {"UnitClassBase",         [](lua_State* L) -> int {
            lua_getglobal(L, "UnitClass");
            lua_pushvalue(L, 1);
            if (lua_pcall(L, 1, 3, 0) != 0) { lua_pop(L, 1); lua_pushnil(L); return 1; }
            // UnitClass answers name, token, id; the token is what is wanted.
            lua_remove(L, -3);
            lua_pop(L, 1);
            return 1;
        }},
                {"IsMounted",         lua_IsMounted},
                {"IsFlying",          lua_IsFlying},
                {"IsSwimming",        lua_IsSwimming},
                {"IsResting",         lua_IsResting},
                {"IsFalling",         lua_IsFalling},
                {"IsStealthed",       lua_IsStealthed},
                {"GetUnitSpeed",      lua_GetUnitSpeed},
                {"UnitAffectingCombat", lua_UnitAffectingCombat},
                {"GetNumRaidMembers",   lua_GetNumRaidMembers},
                {"GetNumPartyMembers",  lua_GetNumPartyMembers},
                {"GetPartyMember",      lua_GetPartyMember},
                // The counts before the dungeon finder inflates them. There is
                // no dungeon finder here, so they are the same number - and
                // UIParent does arithmetic on them, where absent is an error
                // rather than a zero.
                {"GetRealNumRaidMembers",  lua_GetNumRaidMembers},
                {"GetRealNumPartyMembers", lua_GetNumPartyMembers},
                {"HasFullControl",      lua_HasFullControl},
                {"GetPetSpellBonusDamage", lua_GetPetSpellBonusDamage},
                {"RetrieveCorpse",      lua_RetrieveCorpse},
                {"Dismount",            lua_Dismount},
                {"StartAttack",         lua_StartAttack},
                {"AttackTarget",        lua_AttackTarget},
                {"StopAttack",          lua_StopAttack},
                {"UnitInParty",         lua_UnitInParty},
                {"UnitInRaid",          lua_UnitInRaid},
                {"UnitHasVehicleUI",    lua_UnitHasVehicleUI},
                // The seat's uiSkin: "Natural" or "Mechanical" (nil on foot).
                {"UnitVehicleSkin",     [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint64_t guid = vehicleUnitArg(L, gh);
            const auto* seat = guid ? gh->vehicleSeatOf(guid) : nullptr;
            if (!seat) return luaReturnNil(L);
            lua_pushstring(L, game::vehicle::uiSkinName(seat->uiSkin));
            return 1;
        }},
                {"UnitInVehicle",       lua_UnitInVehicle},
                {"UnitControllingVehicle", lua_UnitControllingVehicle},
                {"UnitIsPossessed",     lua_UnitIsPossessed},
                {"UnitIsCharmed",       lua_UnitIsCharmed},
                {"UnitIsTalking",       lua_UnitIsTalking},
                {"UnitInBattleground",  lua_UnitInBattleground},
                {"UnitPlayerOrPetInParty", lua_UnitPlayerOrPetInParty},
                {"UnitPlayerOrPetInRaid",  lua_UnitPlayerOrPetInRaid},
                {"UnitCharacterPoints", lua_UnitCharacterPoints},
                {"PetHasActionBar",     lua_PetHasActionBar},
                {"PetCanBeDismissed",   lua_PetCanBeDismissed},
                {"PetCanBeAbandoned",   lua_PetCanBeAbandoned},
                {"GetRaidRosterInfo",   lua_GetRaidRosterInfo},
                {"GetThreatStatusColor", lua_GetThreatStatusColor},
                {"GetReadyCheckStatus", lua_GetReadyCheckStatus},
                {"RegisterUnitWatch",   lua_RegisterUnitWatch},
                {"UnregisterUnitWatch", lua_UnregisterUnitWatch},
                {"UnitIsUnit",          lua_UnitIsUnit},
                {"UnitIsFriend",        lua_UnitIsFriend},
                {"UnitIsEnemy",         lua_UnitIsEnemy},
                {"UnitCreatureType",    lua_UnitCreatureType},
                {"UnitClassification",  lua_UnitClassification},
                {"UnitReaction",        lua_UnitReaction},
                {"UnitIsConnected",     lua_UnitIsConnected},
                {"GetComboPoints",      lua_GetComboPoints},
                {"GetPlayerInfoByGUID",  lua_GetPlayerInfoByGUID},
                {"UnitXP",                  lua_UnitXP},
                {"UnitXPMax",               lua_UnitXPMax},
                {"GetXPExhaustion",         lua_GetXPExhaustion},
                {"GetTimeToWellRested",     lua_GetTimeToWellRested},
                {"UnitAttackPower",         lua_UnitAttackPower},
                {"UnitRangedAttackPower",   lua_UnitRangedAttackPower},
                {"UnitDefense",             lua_UnitDefense},
                {"UnitAttackSpeed",         lua_UnitAttackSpeed},
                {"UnitDamage",              lua_UnitDamage},
                {"UnitRangedDamage",        lua_UnitRangedDamage},
                {"UnitRangedAttack",        lua_UnitRangedAttack},
                {"UnitAttackBothHands",     lua_UnitAttackBothHands},
                {"GetManaRegen",            lua_GetManaRegen},
                {"GetMaxCombatRatingBonus", lua_GetMaxCombatRatingBonus},
                {"GetUnitMaxHealthModifier", lua_GetUnitMaxHealthModifier},
                {"GetInventoryItemCooldown", lua_GetInventoryItemCooldown},
                {"SetPortraitTexture",         lua_SetPortraitTexture},
                {"GetHairCustomization",       lua_GetHairCustomization},
                {"GetFacialHairCustomization", lua_GetFacialHairCustomization},
                {"CanAlterSkin",               lua_CanAlterSkin},
                {"GetBarberShopStyleInfo",     lua_GetBarberShopStyleInfo},
                {"SetNextBarberShopStyle",     lua_SetNextBarberShopStyle},
                {"GetBarberShopTotalCost",     lua_GetBarberShopTotalCost},
                {"BarberShopReset",            lua_BarberShopReset},
                {"CancelBarberShop",           lua_CancelBarberShop},
                // The Okay button, named as an OnClick attribute rather than
                // called from a script body - which is exactly why the
                // readiness report called this element finished while its one
                // committing action raised.
                {"ApplyBarberShopStyle", [](lua_State* L) -> int {
            if (auto* svc = getLuaServices(L); svc && svc->barberApply) svc->barberApply();
            return 0;
        }},
                // HasWandEquipped() - a wand in the ranged slot, which the
                // character sheet needs because a wand's damage is read
                // differently from a bow's: PaperDollFrame_SetRangedDamage
                // takes the plain average and skips the attack-power bonus a
                // physical ranged weapon gets. Answering no meant a wand user's
                // ranged damage was computed as though the wand were a bow.
                //
                // Weapon class 2, subclass 19, off the same table the client's
                // own subclass names come from.
                {"HasWandEquipped",         [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) { lua_pushboolean(L, 0); return 1; }
            const auto& sl = gh->getInventory().getEquipSlot(game::EquipSlot::RANGED);
            bool wand = false;
            if (!sl.empty()) {
                if (const auto* info = gh->getItemInfo(sl.item.itemId); info && info->valid) {
                    wand = (info->itemClass == 2 && info->subClass == 19);
                }
            }
            lua_pushboolean(L, wand);
            return 1;
        }},
                // UnitHasRelicSlot(unit) - the four classes whose ranged slot
                // holds a relic instead of a weapon: paladin librams, death
                // knight sigils, shaman totems and druid idols. Answering no
                // for all of them made the paperdoll label that slot "Ranged"
                // and read its stats as a ranged weapon's.
                {"UnitHasRelicSlot",        [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* uid = luaL_optstring(L, 1, "player");
            // Only the player's class is known well enough to answer this.
            if (!gh || std::string(uid) != "player") { lua_pushboolean(L, 0); return 1; }
            const uint8_t c = gh->getPlayerClass();
            lua_pushboolean(L, c == 2 || c == 6 || c == 7 || c == 11);
            return 1;
        }},
                // Whether the repair cursor is up. A flat false meant the
                // merchant's repair button re-armed on every click instead of
                // toggling, and neither the bag nor the paperdoll tooltip ever
                // showed an item's repair cost.
                {"InRepairMode", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const bool active = repairCursorUp() && gh && gh->isVendorWindowOpen();
            lua_pushboolean(L, active ? 1 : 0);
            return 1;
        }},
                // IsInventoryItemLocked(slot) - that slot's item is on the
                // cursor, so the paperdoll greys it while it is in the air.
                // Answering no meant a picked-up item stayed drawn in the slot
                // it had already left.
                //
                // It does show, despite SetDesaturated being a no-op here:
                // SetItemButtonDesaturated reads the return value as
                // "shaderSupported" and greys with SetVertexColor(0.5) when it
                // is falsy, which is the branch a no-op takes.
                {"IsInventoryItemLocked",   [](lua_State* L) -> int {
            const int slot = static_cast<int>(luaL_optnumber(L, 1, 0));
            lua_pushboolean(L, slot > 0 && cursorEquipSlot() == slot);
            return 1;
        }},
                // GetInventoryItemBroken(unit, slot) - worn out, so the
                // paperdoll draws that slot's icon red. The durability is
                // already tracked and already answered by
                // GetInventoryItemDurability; only this was left saying no.
                {"GetInventoryItemBroken",  [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int slotId = static_cast<int>(luaL_optnumber(L, 2, 0));
            if (!gh || slotId < 1 || slotId > 19) { lua_pushboolean(L, 0); return 1; }
            const auto& sl = gh->getInventory().getEquipSlot(
                static_cast<game::EquipSlot>(slotId - 1));
            lua_pushboolean(L, !sl.empty() && sl.item.maxDurability > 0 &&
                               sl.item.curDurability == 0);
            return 1;
        }},
                // CursorCanGoInSlot(slot) - whether what the cursor is holding
                // could be worn in that paperdoll slot, which is what makes the
                // slot light up while an item is being dragged. Answering no
                // for everything meant nothing ever lit up.
                //
                // Which slot of a pair, this does not decide: both rings and
                // both trinkets light up, as they do in WoW. That is a
                // different question from the one InventoryScreen answers when
                // it picks a slot to equip into, so this does not go looking
                // for that logic.
                {"CursorCanGoInSlot",       [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int slot = static_cast<int>(luaL_optnumber(L, 1, 0));
            // The ammo slot takes ammunition: lifted off it (cursor kind 9),
            // or a projectile carried out of a bag (Wow.exe 0x5EA880).
            if (slot == 0) {
                const uint32_t held = cursorItemId();
                lua_pushboolean(L, cursorAmmoId() != 0 ||
                                   (gh && held != 0 && gh->isProjectileItem(held)));
                return 1;
            }
            const uint32_t held = cursorItemId();
            if (!gh || slot < 1 || held == 0) { lua_pushboolean(L, 0); return 1; }
            const auto* info = gh->getItemInfo(held);
            if (!info || !info->valid) { lua_pushboolean(L, 0); return 1; }
            // Paperdoll slot ids are the EquipSlot enum plus one.
            using namespace game::InvType;
            bool fits = false;
            switch (info->inventoryType) {
                case HEAD:       fits = (slot == 1);  break;
                case NECK:       fits = (slot == 2);  break;
                case SHOULDERS:  fits = (slot == 3);  break;
                case SHIRT:      fits = (slot == 4);  break;
                case CHEST: case ROBE: fits = (slot == 5); break;
                case WAIST:      fits = (slot == 6);  break;
                case LEGS:       fits = (slot == 7);  break;
                case FEET:       fits = (slot == 8);  break;
                case WRISTS:     fits = (slot == 9);  break;
                case HANDS:      fits = (slot == 10); break;
                case FINGER:     fits = (slot == 11 || slot == 12); break;
                case TRINKET:    fits = (slot == 13 || slot == 14); break;
                case BACK:       fits = (slot == 15); break;
                // A one-hander goes in either hand; a main-hand-only weapon and
                // a two-hander only in the first.
                case ONE_HAND:   fits = (slot == 16 || slot == 17); break;
                case MAIN_HAND: case TWO_HAND: fits = (slot == 16); break;
                case SHIELD: case OFF_HAND: case HOLDABLE: fits = (slot == 17); break;
                case RANGED_BOW: case RANGED_GUN: case THROWN: fits = (slot == 18); break;
                case TABARD:     fits = (slot == 19); break;
                // A relic (INVTYPE_RELIC) is the ranged slot's for the classes
                // that have one; bags and quivers go on the bag bar.
                case 28:         fits = (slot == 18); break;
                case BAG: case 27: fits = (slot >= 20 && slot <= 23); break;
                default:         fits = false; break;
            }
            lua_pushboolean(L, fits);
            return 1;
        }},
                // Zero rather than false: paperdollframe.lua asks
                // `IsTitleKnown(i) ~= 0`, and `false ~= 0` compares two
                // different types and is therefore true - so every title would
                // have counted as known. This matters live, not just in
                // theory: GetNumTitles answers 192, so the 1..GetNumTitles
                // loop does run and reads this on every title.
                // IsTitleKnown(bit) - the client tracks these already, as a
                // set of bits off the player's known-titles mask. Answering a
                // constant zero meant the paperdoll's title dropdown listed
                // nothing however many the character had earned.
                //
                // A number rather than a boolean, because paperdollframe.lua
                // asks `IsTitleKnown(i) ~= 0` and false compares unequal to
                // zero - which is the trap that made this a zero rather than a
                // false in the first place.
                // The pet tab of the character sheet, which is handed over -
                // PetPaperDollFrame_SetStats and PetExpBar_Update run whenever
                // it is shown, so with a pet out these raised on opening it.
                //
                // The two modifiers are multipliers, so one is the neutral
                // answer; zero would have read as a pet with no health at all.
                {"GetUnitHealthModifier",   [](lua_State* L) -> int {
            lua_pushnumber(L, 1.0); return 1; }},
                {"GetUnitPowerModifier",    [](lua_State* L) -> int {
            lua_pushnumber(L, 1.0); return 1; }},
                {"GetPetExperience", [](lua_State* L) -> int {
            // currXP, nextXP off the pet unit's own fields - a hunter's pet
            // only (0x5D3A20); anyone else's answers 0, 0.
            auto* gh = getGameHandler(L);
            const bool hunter = gh && gh->isHunterPet();
            lua_pushnumber(L, hunter ? gh->getPetExperience() : 0);
            lua_pushnumber(L, hunter ? gh->getPetNextLevelExp() : 0);
            return 2;
        }},
                // GetCompanionCooldown(mode, index) → start, duration, enabled.
                //
                // A companion's cooldown is its summon spell's, the same
                // relationship GetTradeSkillCooldown reads for a recipe. The
                // comment that used to sit here said this client does not
                // enumerate companions, which stopped being true when
                // rebuildCompanions was written - GetCompanionInfo has listed
                // them, and CallCompanion below has summoned them, ever since.
                //
                // (start, duration), not (now, remaining): the cooldown frame
                // draws a sweep of `duration` beginning at `start`, so the
                // start is wound back by however much has already run.
                {"GetCompanionCooldown", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* kind = luaL_optstring(L, 1, "");
            const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
            if (!gh || index < 1) {
                lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 1);
                return 3;
            }
            const auto& list = gh->getCompanions(std::string(kind) == "MOUNT");
            float left = 0.0f, total = 0.0f;
            if (index <= static_cast<int>(list.size())) {
                const uint32_t spellId = list[static_cast<size_t>(index) - 1].spellId;
                left = gh->getSpellCooldown(spellId);
                total = gh->getSpellCooldownTotal(spellId);
                if (total < left) total = left;
            }
            if (left <= 0.01f) {
                lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 1);
                return 3;
            }
            lua_pushnumber(L, luaGetTimeNow() - (total - left));
            lua_pushnumber(L, total);
            lua_pushnumber(L, 1);
            return 3;
        }},
                // Summoning a mount or a critter is casting its spell - there
                // is no separate companion message on the wire in 3.3.5.
                {"CallCompanion", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* kind = luaL_optstring(L, 1, "");
            const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
            if (!gh || index < 1) return 0;
            const auto& list = gh->getCompanions(std::string(kind) == "MOUNT");
            if (index <= static_cast<int>(list.size())) {
                gh->castSpell(list[static_cast<size_t>(index) - 1].spellId, 0);
            }
            return 0;
        }},
                // Putting one away is cancelling its aura, which is the same
                // thing right-clicking the buff does.
                {"DismissCompanion", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* kind = luaL_optstring(L, 1, "");
            if (!gh) return 0;
            const auto& list = gh->getCompanions(std::string(kind) == "MOUNT");
            for (const auto& c : list) {
                for (const auto& a : gh->getPlayerAuras()) {
                    if (a.spellId != c.spellId) continue;
                    gh->cancelAura(c.spellId);
                    return 0;
                }
            }
            return 0;
        }},
                {"IsTitleKnown",            [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int bit = static_cast<int>(luaL_optnumber(L, 1, -1));
            const bool known = gh && bit >= 0 &&
                               gh->getKnownTitleBits().count(static_cast<uint32_t>(bit)) > 0;
            lua_pushnumber(L, known ? 1 : 0);
            return 1; }},
                {"GetCombatRatingBonus",    lua_GetCombatRatingBonus},
                {"GetCritChanceFromAgility", lua_GetCritChanceFromAgility},
                {"GetSpellCritChanceFromIntellect", lua_GetSpellCritChanceFromIntellect},
                // Three values - main hand, off hand, ranged - because the
                // character sheet reads the second and concatenates it. One
                // value left it nil, and the line that prints "expertise /
                // off-hand expertise" raised rather than printing.
                // GetExpertise() → expertise points, main hand and off hand.
                //
                // Two values, not three: the character sheet destructures
                // `local expertise, offhandExpertise = GetExpertise()` and
                // prints them either side of a slash when an off hand is being
                // swung. The third was never read, and both of the first two
                // were zero for everyone - the server sends them, in points,
                // and nothing here was reading the fields.
                {"GetExpertise", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? gh->getExpertise() : 0);
            lua_pushnumber(L, gh ? gh->getOffhandExpertise() : 0);
            return 2;
        }},
                // Two, for the same reason and under the same guard, four
                // lines below the call above: the character sheet formats the
                // off-hand percentage whenever an off-hand is equipped, and
                // format("%.2f", nil) raises. Fixing GetExpertise alone left
                // every dual-wielder's stat panel broken on the next line.
                // A point of expertise is a quarter of a percent off the
                // chance to be dodged or parried, which is the arithmetic the
                // sheet's tooltip line does not do for itself - it formats
                // whatever it is given with two decimal places and a % sign.
                {"GetExpertisePercent", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, (gh ? gh->getExpertise() : 0) * 0.25);
            lua_pushnumber(L, (gh ? gh->getOffhandExpertise() : 0) * 0.25);
            return 2;
        }},
                {"GetArmorPenetration",     lua_GetArmorPenetration},
                // Spell penetration and shield block value are summed from equipped-item
                // stat mods (ITEM_MOD_SPELL_PENETRATION, the shield's block value), not
                // any player field the server sends - the inventory does not yet total
                // item stat mods, so these stay a genuine zero rather than a stale stub.
                {"GetSpellPenetration",     lua_ZeroPercent},
                {"GetShieldBlock",          lua_ZeroPercent},
                {"GetUnitHealthRegenRateFromSpirit", lua_GetUnitHealthRegenRateFromSpirit},
                {"GetUnitManaRegenRateFromSpirit",   lua_GetUnitManaRegenRateFromSpirit},
                {"GetWatchedFactionInfo",   lua_GetWatchedFactionInfo},
                {"GetNumBagSlots",          lua_GetNumBagSlots},
                {"GetMirrorTimerProgress",  lua_GetMirrorTimerProgress},
                {"GetRestState",            lua_GetRestState},
                {"HasFocus", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, gh && gh->hasFocus() ? 1 : 0);
            return 1;
        }},
    };
    for (const auto& [name, func] : api) {
        lua_pushcfunction(L, func);
        lua_setglobal(L, name);
    }
}

} // namespace wowee::addons
