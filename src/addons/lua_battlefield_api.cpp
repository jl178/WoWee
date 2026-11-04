// lua_battlefield_api.cpp - the battleground, arena and world state bindings
// as the stock 3.3.5a client answers them (Wow.exe's CGBattlefieldInfo and
// the WorldStateUI functions). The rules live in game/battlefield_rules and
// the state in SocialHandler; this file is the Lua face of both. Every
// function here names the Wow.exe address it follows; docs/re/pvp-instances.md.
//
// Registered after the other API sets, so these are the bindings in force.

#include "addons/lua_api_helpers.hpp"
#include "addons/lua_engine.hpp"
#include "core/logger.hpp"
#include "game/arena_rules.hpp"
#include "game/battlefield_rules.hpp"
#include "game/global_strings.hpp"
#include "game/update_field_table.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "game/social_handler.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"

#include <ctime>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::addons {

namespace {

using game::bf::Queues;

const game::SocialHandler* bfState(lua_State* L) {
    auto* gh = getGameHandler(L);
    return gh ? gh->battlefield() : nullptr;
}

int64_t nowMs() { return game::bf::clockMs(); }

// A queue index from Lua (1 or 2) as a slot, or -1.
int slotArg(lua_State* L, int idx) {
    const int i = static_cast<int>(luaL_optnumber(L, idx, 0));
    return (i >= 1 && i <= static_cast<int>(game::bf::kQueueSlots)) ? i - 1 : -1;
}

void pushOneOrNil(lua_State* L, bool v) {
    if (v) lua_pushnumber(L, 1); else lua_pushnil(L);
}

// Map.dbc rows the bindings need beyond the name: the two descriptions.
struct MapText { std::string name, descAlliance, descHorde; };
const MapText* mapText(game::GameHandler* gh, uint32_t mapId) {
    static std::unordered_map<uint32_t, MapText> cache;
    static bool loaded = false;
    if (!loaded && gh && gh->services().assetManager &&
        gh->services().assetManager->isInitialized()) {
        loaded = true;
        auto dbc = gh->services().assetManager->loadDBC("Map.dbc");
        // 3.3.5: 5 MapName, 23 MapDescription0 (Alliance), 40 MapDescription1 (Horde).
        if (dbc && dbc->isLoaded() && dbc->getFieldCount() >= 66) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                MapText t{dbc->getString(i, 5), dbc->getString(i, 23), dbc->getString(i, 40)};
                cache[dbc->getUInt32(i, 0)] = std::move(t);
            }
        }
    }
    auto it = cache.find(mapId);
    return it != cache.end() ? &it->second : nullptr;
}

// PvpDifficulty.dbc: whether the player's level has a bracket on a map
// (0x54BA00). The random battleground asks for level 80 instead.
bool levelHasBracket(game::GameHandler* gh, uint32_t mapId, uint32_t level) {
    struct Row { uint32_t map, minLevel, maxLevel; };
    static std::vector<Row> rows;
    static bool loaded = false;
    if (!loaded && gh && gh->services().assetManager &&
        gh->services().assetManager->isInitialized()) {
        loaded = true;
        auto dbc = gh->services().assetManager->loadDBC("PvpDifficulty.dbc");
        // 0 ID, 1 MapID, 2 BracketID, 3 MinLevel, 4 MaxLevel, 5 Difficulty.
        if (dbc && dbc->isLoaded() && dbc->getFieldCount() >= 5) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i)
                rows.push_back({dbc->getUInt32(i, 1), dbc->getUInt32(i, 3), dbc->getUInt32(i, 4)});
        }
    }
    for (const auto& r : rows)
        if (r.map == mapId && level >= r.minLevel && level <= r.maxLevel) return true;
    return false;
}

bool canEnter(game::GameHandler* gh, const game::GameHandler::BattlemasterEntry& bg) {
    const uint32_t level = gh->getPlayerLevel();
    if (bg.id == game::bf::kRandomBattleground) return level == 80;
    return !bg.mapIds.empty() && levelHasBracket(gh, bg.mapIds.front(), level);
}

bool holidayActive(game::GameHandler* gh, const game::GameHandler::BattlemasterEntry& bg) {
    if (bg.holidayWorldState == 0) return false;
    const auto v = gh->getWorldState(bg.holidayWorldState);
    return v && *v != 0;
}

// ---- The queues (0x54AE40) ----

// GetBattlefieldStatus(i) -> status, mapName, instanceID, minLevel, maxLevel,
// teamSize, registeredMatch (0x54BC30).
int lua_GetBattlefieldStatus(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* bf = bfState(L);
    const int slot = slotArg(L, 1);
    if (!gh || !bf || slot < 0) {
        lua_pushnil(L); lua_pushnil(L);
        lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
        lua_pushnil(L);
        return 7;
    }
    const auto& queues = bf->getBattlefieldQueues();
    const auto* q = queues.slot(static_cast<uint32_t>(slot));
    lua_pushstring(L, game::bf::statusToken(q->status));
    // The map's own name for an invitation (except the random battleground's)
    // and a battle in progress; the BattlemasterList name otherwise.
    std::string name;
    uint32_t mapId = 0;
    if (q->status == 2 && q->bgTypeId != game::bf::kRandomBattleground) mapId = q->mapId;
    else if (q->status == 3) mapId = q->mapId;
    if (mapId) name = gh->getMapName(mapId);
    if (name.empty()) {
        if (const auto* row = gh->getBattlemasterInfo(q->bgTypeId)) name = row->name;
    }
    if (name.empty()) lua_pushnil(L); else lua_pushstring(L, name.c_str());
    lua_pushnumber(L, q->instanceId);
    lua_pushnumber(L, q->minLevel);
    lua_pushnumber(L, q->maxLevel);
    lua_pushnumber(L, q->teamSize);
    pushOneOrNil(L, q->rated);
    return 7;
}

// GetBattlefieldPortExpiration(i): whole seconds left on the invitation (0x549B80).
int lua_GetBattlefieldPortExpiration(lua_State* L) {
    const auto* bf = bfState(L);
    const int slot = slotArg(L, 1);
    lua_pushnumber(L, bf && slot >= 0
        ? bf->getBattlefieldQueues().portExpirationSec(static_cast<uint32_t>(slot), nowMs()) : 0);
    return 1;
}

// GetBattlefieldTimeWaited(i): milliseconds in the queue (0x549DD0).
int lua_GetBattlefieldTimeWaited(lua_State* L) {
    const auto* bf = bfState(L);
    const int slot = slotArg(L, 1);
    lua_pushnumber(L, bf && slot >= 0
        ? static_cast<double>(bf->getBattlefieldQueues().timeWaitedMs(static_cast<uint32_t>(slot), nowMs()))
        : 0.0);
    return 1;
}

// GetBattlefieldEstimatedWaitTime(i): milliseconds (0x549D30).
int lua_GetBattlefieldEstimatedWaitTime(lua_State* L) {
    const auto* bf = bfState(L);
    const int slot = slotArg(L, 1);
    lua_pushnumber(L, bf && slot >= 0
        ? bf->getBattlefieldQueues().estimatedWaitMs(static_cast<uint32_t>(slot)) : 0);
    return 1;
}

// GetBattlefieldInstanceExpiration(): milliseconds to the end-of-battle
// shutdown, 0 while none is pending (0x549C40).
int lua_GetBattlefieldInstanceExpiration(lua_State* L) {
    const auto* bf = bfState(L);
    lua_pushnumber(L, bf ? static_cast<double>(bf->getBattlefieldQueues().instanceExpirationMs(nowMs())) : 0.0);
    return 1;
}

// GetBattlefieldInstanceRunTime(): milliseconds since the battle began (0x549CD0).
int lua_GetBattlefieldInstanceRunTime(lua_State* L) {
    const auto* bf = bfState(L);
    lua_pushnumber(L, bf ? static_cast<double>(bf->getBattlefieldQueues().instanceRunTimeMs(nowMs())) : 0.0);
    return 1;
}

// IsActiveBattlefieldArena() -> isArena, isRegistered, each 1 or nil (0x549AD0).
int lua_IsActiveBattlefieldArena(lua_State* L) {
    const auto* bf = bfState(L);
    const auto* q = bf ? bf->getBattlefieldQueues().slot(
                             static_cast<uint32_t>(bf->getBattlefieldQueues().active().slot)) : nullptr;
    pushOneOrNil(L, bf && bf->getBattlefieldQueues().active().instanceType == 4);
    pushOneOrNil(L, q && q->rated);
    return 2;
}

// GetBattlefieldArenaFaction(): 1 or nil, the status's last byte (0x54A280).
int lua_GetBattlefieldArenaFaction(lua_State* L) {
    const auto* bf = bfState(L);
    pushOneOrNil(L, bf && bf->getBattlefieldQueues().active().arenaFaction);
    return 1;
}

// AcceptBattlefieldPort(i, accept) (0x54DA10): answers 1 when sent.
int lua_AcceptBattlefieldPort(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int slot = slotArg(L, 1);
    if (!gh || !gh->battlefield() || slot < 0) {
        // Stock answers nil for a queue that is not 1 or 2 (0x54DA10).
        LOG_WARNING("AcceptBattlefieldPort: no queue ", luaL_optnumber(L, 1, 0));
        return luaReturnNil(L);
    }
    const bool accept = lua_toboolean(L, 2) != 0;
    // Accepting is refused here first, unless the player already stands in a
    // battleground (Map.dbc InstanceType 3): with the player's pet-in-combat
    // unit flag (0x800), dead, falling, on a vehicle, or with the fatigue
    // timer running down - each its GameError and nil (0x54DA10).
    auto me = gh->getEntityManager().getEntity(gh->getPlayerGuid());
    if (accept && me && gh->getMapInstanceType(gh->getCurrentMapId()) != 3) {
        struct Refusal { bool when; const char* key; const char* text; };
        const auto& fatigue = gh->getMirrorTimer(0);
        const Refusal refusals[] = {
            {(me->getField(game::fieldIndex(game::UF::UNIT_FIELD_FLAGS)) & 0x800) != 0,
             "ERR_NOT_IN_COMBAT", "You can't do that while in combat"},
            {gh->isPlayerDead() || gh->isPlayerGhost(), "ERR_PLAYER_DEAD", "You can't do that when you're dead."},
            {(gh->getMovementInfo().flags & 0x1000) != 0, "ERR_NOT_WHILE_FALLING", "You can't do that while jumping or falling"},
            {gh->playerVehicleGuid() != 0, "ERR_CLIENT_LOCKED_OUT", "You can't do that right now."},
            {fatigue.active && fatigue.scale < 0, "ERR_NOT_WHILE_FATIGUED", "You can't do that while fatigued"},
        };
        for (const auto& r : refusals) {
            if (!r.when) continue;
            gh->displayGameMessage(r.key, r.text);
            return luaReturnNil(L);
        }
    }
    const bool sent = gh->battlefield()->acceptBattlefieldPort(static_cast<uint32_t>(slot), accept);
    if (sent) lua_pushnumber(L, 1); else lua_pushnil(L);
    return 1;
}

int lua_LeaveBattlefield(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->leaveBattlefield();
    return 0;
}

// ---- The battlemaster's list (0x54E390) ----

int lua_GetNumBattlefields(lua_State* L) {
    const auto* bf = bfState(L);
    lua_pushnumber(L, bf ? static_cast<double>(bf->getBattlefieldList().instanceIds.size()) : 0.0);
    return 1;
}

// GetBattlefieldInstanceInfo(i): the i-th instance id, nothing past the end (0x54D8F0).
int lua_GetBattlefieldInstanceInfo(lua_State* L) {
    const auto* bf = bfState(L);
    const int i = static_cast<int>(luaL_checknumber(L, 1));
    if (!bf) return 0;
    const auto& ids = bf->getBattlefieldList().instanceIds;
    if (i < 1 || i > static_cast<int>(ids.size())) return 0;
    lua_pushnumber(L, ids[static_cast<size_t>(i - 1)]);
    return 1;
}

// GetBattlefieldInfo() -> name, description, maxGroup, canEnter, isHoliday,
// isRandom (0x54D770): the battleground the last list named.
int lua_GetBattlefieldInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* bf = bfState(L);
    if (!gh || !bf) return 0;
    const auto* row = gh->getBattlemasterInfo(bf->getBattlefieldList().bgTypeId);
    if (!row || row->mapIds.empty()) return 0;
    const MapText* map = mapText(gh, row->mapIds.front());
    if (!map) return 0;
    lua_pushstring(L, row->name.c_str());
    const bool horde = game::bf::factionOfRace(gh->getPlayerRace()) == 0;
    lua_pushstring(L, (horde ? map->descHorde : map->descAlliance).c_str());
    lua_pushnumber(L, row->maxGroupSize);
    pushOneOrNil(L, canEnter(gh, *row));
    pushOneOrNil(L, holidayActive(gh, *row));
    pushOneOrNil(L, row->id == game::bf::kRandomBattleground);
    return 6;
}

// IsBattlefieldArena(): the list's battleground is an arena, 1 or nil (0x54BAE0).
int lua_IsBattlefieldArena(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* bf = bfState(L);
    const auto* row = (gh && bf) ? gh->getBattlemasterInfo(bf->getBattlefieldList().bgTypeId) : nullptr;
    pushOneOrNil(L, row && row->instanceType == 4);
    return 1;
}

// CanJoinBattlefieldAsGroup(): the list's battleground allows groups (0x54C6E0).
int lua_CanJoinBattlefieldAsGroup(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* bf = bfState(L);
    const auto* row = (gh && bf) ? gh->getBattlemasterInfo(bf->getBattlefieldList().bgTypeId) : nullptr;
    pushOneOrNil(L, row && row->groupsAllowed != 0);
    return 1;
}

// The selected instance, kept as its id (0xBEA538).
uint32_t& selectedInstanceId() { static uint32_t id = 0; return id; }

// GetSelectedBattlefield(): the selection's row, 0 when none (0x54BBD0).
int lua_GetSelectedBattlefield(lua_State* L) {
    const auto* bf = bfState(L);
    int row = 0;
    if (bf) {
        const auto& ids = bf->getBattlefieldList().instanceIds;
        for (size_t i = 0; i < ids.size(); ++i)
            if (ids[i] == selectedInstanceId()) { row = static_cast<int>(i) + 1; break; }
    }
    lua_pushnumber(L, row);
    return 1;
}

// SetSelectedBattlefield(i) (0x54BB40).
int lua_SetSelectedBattlefield(lua_State* L) {
    const auto* bf = bfState(L);
    const int i = static_cast<int>(luaL_checknumber(L, 1));
    selectedInstanceId() = 0;
    if (bf) {
        const auto& ids = bf->getBattlefieldList().instanceIds;
        if (i >= 1 && i <= static_cast<int>(ids.size())) selectedInstanceId() = ids[static_cast<size_t>(i - 1)];
    }
    return 0;
}

// JoinBattlefield(i, asGroup, isRated) (0x54D990 -> 0x54CC70).
int lua_JoinBattlefield(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int i = static_cast<int>(luaL_checknumber(L, 1));
    if (gh && gh->battlefield())
        gh->battlefield()->joinBattlefieldIndex(i, lua_toboolean(L, 2) != 0, lua_toboolean(L, 3) != 0);
    return 0;
}

// CloseBattlefield() (0x549B40).
int lua_CloseBattlefield(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh && gh->battlefield()) gh->battlefield()->closeBattlefield();
    return 0;
}

// The rewards the list carries (0x54A300, 0x54A370): hasWin, winHonor,
// winArena, lossHonor, and a zero. "Holiday" is the plain block.
int lua_GetHolidayBGHonorCurrencyBonuses(lua_State* L) {
    const auto* bf = bfState(L);
    game::bf::ListPacket empty;
    const auto& l = bf ? bf->getBattlefieldList() : empty;
    lua_pushboolean(L, l.hasWin);
    lua_pushnumber(L, l.winHonor);
    lua_pushnumber(L, l.winArena);
    lua_pushnumber(L, l.lossHonor);
    lua_pushnumber(L, 0);
    return 5;
}
int lua_GetRandomBGHonorCurrencyBonuses(lua_State* L) {
    const auto* bf = bfState(L);
    game::bf::ListPacket empty;
    const auto& l = bf ? bf->getBattlefieldList() : empty;
    lua_pushboolean(L, l.hasWinRandom);
    lua_pushnumber(L, l.winHonorRandom);
    lua_pushnumber(L, l.winArenaRandom);
    lua_pushnumber(L, l.lossHonorRandom);
    lua_pushnumber(L, 0);
    return 5;
}

// ---- The PvP frame's battleground list (BattlemasterList.dbc) ----

int lua_GetNumBattlegroundTypes(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? static_cast<double>(gh->getBattlegroundTypes().size()) : 0.0);
    return 1;
}

// GetBattlegroundInfo(i) -> name, canEnter, isHoliday, isRandom, id (0x54E010).
int lua_GetBattlegroundInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int i = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh) return 0;
    const auto& list = gh->getBattlegroundTypes();
    if (i < 1 || i > static_cast<int>(list.size())) return 0;
    const auto& bg = list[static_cast<size_t>(i - 1)];
    lua_pushstring(L, bg.name.c_str());
    pushOneOrNil(L, canEnter(gh, bg));
    pushOneOrNil(L, holidayActive(gh, bg));
    pushOneOrNil(L, bg.id == game::bf::kRandomBattleground);
    lua_pushnumber(L, bg.id);
    return 5;
}

// RequestBattlegroundInstanceInfo(i) (0x54E6D0 -> 0x54E5B0).
int lua_RequestBattlegroundInstanceInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int i = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh) return 0;
    const auto& list = gh->getBattlegroundTypes();
    if (i < 1 || i > static_cast<int>(list.size())) return 0;
    gh->requestBattlefieldList(list[static_cast<size_t>(i - 1)].id);
    return 0;
}

// ---- The scoreboard (0x54D280, 0x54AA30) ----

int lua_RequestBattlefieldScoreData(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->requestPvpLog();
    return 0;
}

int lua_GetNumBattlefieldScores(lua_State* L) {
    const auto* bf = bfState(L);
    lua_pushnumber(L, bf ? static_cast<double>(bf->getScoreboard().shown) : 0.0);
    return 1;
}

// GetBattlefieldScore(i) -> name, killingBlows, honorableKills, deaths,
// honorGained, faction, rank, race, class, classToken, damageDone,
// healingDone (0x54BE90). Indexed over the sorted rows, the filtered
// faction's first.
int lua_GetBattlefieldScore(lua_State* L) {
    const auto* bf = bfState(L);
    const int i = static_cast<int>(luaL_checknumber(L, 1));
    const game::bf::ScoreRow* r = nullptr;
    if (bf && i >= 1 && i <= static_cast<int>(bf->getScoreboard().log.rows.size()))
        r = &bf->getScoreboard().log.rows[static_cast<size_t>(i - 1)];
    if (!r || r->name.empty()) {
        lua_pushnil(L);
        for (int k = 0; k < 6; ++k) lua_pushnumber(L, 0);
        lua_pushnil(L); lua_pushnil(L); lua_pushnil(L);
        lua_pushnumber(L, 0); lua_pushnumber(L, 0);
        return 12;
    }
    lua_pushstring(L, r->name.c_str());
    lua_pushnumber(L, r->killingBlows);
    lua_pushnumber(L, r->honorableKills);
    lua_pushnumber(L, r->deaths);
    lua_pushnumber(L, r->honorGained);
    lua_pushnumber(L, r->faction);
    lua_pushnumber(L, 0);
    const char* race = r->raceId < sizeof(kLuaRaces) / sizeof(kLuaRaces[0]) ? kLuaRaces[r->raceId] : "";
    const char* cls = r->classId < sizeof(kLuaClasses) / sizeof(kLuaClasses[0]) ? kLuaClasses[r->classId] : "";
    if (race && *race) lua_pushstring(L, race); else lua_pushnil(L);
    if (cls && *cls) { lua_pushstring(L, cls); luaPushClassToken(L, r->classId); }
    else { lua_pushnil(L); lua_pushnil(L); }
    lua_pushnumber(L, r->damageDone);
    lua_pushnumber(L, r->healingDone);
    return 12;
}

// SetBattlefieldScoreFaction(nil|0|1) (0x54C120).
int lua_SetBattlefieldScoreFaction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->battlefield()) return 0;
    if (lua_isnoneornil(L, 1)) { gh->battlefield()->setScoreFaction(-1); return 0; }
    const int f = static_cast<int>(lua_tonumber(L, 1));
    if (f == 0 || f == 1) gh->battlefield()->setScoreFaction(f);
    return 0;
}

// SortBattlefieldScoreData("column") (0x54DE00).
int lua_SortBattlefieldScoreData(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* c = luaL_checkstring(L, 1);
    if (gh && gh->battlefield()) gh->battlefield()->sortScores(c);
    return 0;
}

// GetBattlefieldWinner(): nil until the battle ended (0x549EC0).
int lua_GetBattlefieldWinner(lua_State* L) {
    const auto* bf = bfState(L);
    if (!bf || !bf->getScoreboard().log.ended) return luaReturnNil(L);
    lua_pushnumber(L, bf->getScoreboard().log.winner);
    return 1;
}

// GetBattlefieldTeamInfo(0|1) -> name, the two ratings as sent, MMR (0x54A180).
int lua_GetBattlefieldTeamInfo(lua_State* L) {
    const auto* bf = bfState(L);
    const int i = static_cast<int>(luaL_checknumber(L, 1));
    if (!bf || i < 0 || i > 1) {
        lua_pushnil(L); lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
        return 4;
    }
    const auto& t = bf->getScoreboard().log.teams[static_cast<size_t>(i)];
    lua_pushstring(L, t.name.c_str());
    lua_pushnumber(L, t.rating1);
    lua_pushnumber(L, t.rating2);
    lua_pushnumber(L, t.mmr);
    return 4;
}

// GetNumBattlefieldStats() / GetBattlefieldStatInfo(i) -> text, icon, tooltip:
// the battleground's WorldStateUI rows of type 2 (0x549F20, 0x54C170).
int lua_GetNumBattlefieldStats(lua_State* L) {
    const auto* bf = bfState(L);
    lua_pushnumber(L, bf ? static_cast<double>(bf->getScoreColumns().size()) : 0.0);
    return 1;
}
int lua_GetBattlefieldStatInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* bf = bfState(L);
    const int i = static_cast<int>(luaL_checknumber(L, 1));
    if (gh && bf && i >= 1 && i <= static_cast<int>(bf->getScoreColumns().size())) {
        const auto& rows = gh->worldStateUiRows();
        const size_t r = bf->getScoreColumns()[static_cast<size_t>(i - 1)];
        if (r < rows.size()) {
            lua_pushstring(L, rows[r].text.c_str());
            lua_pushstring(L, rows[r].icon.c_str());
            lua_pushstring(L, rows[r].tooltip.c_str());
            return 3;
        }
    }
    lua_pushnil(L); lua_pushnil(L); lua_pushnil(L);
    return 3;
}

// GetBattlefieldStatData(row, column) (0x549F60).
int lua_GetBattlefieldStatData(lua_State* L) {
    const auto* bf = bfState(L);
    const int row = static_cast<int>(luaL_checknumber(L, 1));
    const int col = static_cast<int>(luaL_checknumber(L, 2));
    double v = 0.0;
    if (bf && row >= 1 && row <= static_cast<int>(bf->getScoreboard().log.rows.size()) &&
        col >= 1 && col <= 8)
        v = bf->getScoreboard().log.rows[static_cast<size_t>(row - 1)].stats[static_cast<size_t>(col - 1)];
    lua_pushnumber(L, v);
    return 1;
}

// ReportPlayerIsPVPAFK(name): the scoreboard row of that name.
int lua_ReportPlayerIsPVPAFK(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* name = lua_isstring(L, 1) ? lua_tostring(L, 1) : nullptr;
    if (!gh || !gh->battlefield() || !name || !*name) return 0;
    for (const auto& r : gh->battlefield()->getScoreboard().log.rows)
        if (r.name == name) { gh->reportPvpAfk(r.guid); break; }
    return 0;
}

// ---- WorldStateUI (0x548720, 0x548D40) ----

std::vector<size_t> worldStateLines(game::GameHandler* gh) {
    if (!gh) return {};
    return game::bf::visibleRows(gh->worldStateUiRows(), gh->getWorldStateMapId(),
                                 gh->getWorldStateZoneId(), gh->getWorldStateAreaId(), 1u);
}

int lua_GetNumWorldStateUI(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(worldStateLines(getGameHandler(L)).size()));
    return 1;
}

// GetWorldStateUIInfo(i) -> uiType, state, text, icon, dynamicIcon, tooltip,
// dynamicTooltip, extendedUI, extendedUIState1..3. An index past the end
// answers a single 0.
int lua_GetWorldStateUIInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int i = static_cast<int>(luaL_checknumber(L, 1));
    const auto lines = worldStateLines(gh);
    if (!gh || i < 1 || i > static_cast<int>(lines.size())) { lua_pushnumber(L, 0); return 1; }
    const auto& r = gh->worldStateUiRows()[lines[static_cast<size_t>(i - 1)]];
    auto ws = [gh](uint32_t id) -> uint32_t {
        const auto v = gh->getWorldState(id);
        return v ? *v : 0;
    };
    lua_pushnumber(L, r.type);
    // No world state of its own is "shown"; one the server never set is 0.
    lua_pushnumber(L, r.stateVariable == 0 ? 1.0 : static_cast<double>(ws(r.stateVariable)));
    lua_pushstring(L, game::bf::formatText(r.text, ws, gh->getWorldStateServerTime()).c_str());
    lua_pushstring(L, r.icon.c_str());
    lua_pushstring(L, r.dynamicIcon.c_str());
    lua_pushstring(L, r.tooltip.c_str());
    lua_pushstring(L, r.dynamicTooltip.c_str());
    lua_pushstring(L, r.extendedUI.c_str());
    for (uint32_t v : r.extendedUIStateVariable) lua_pushnumber(L, ws(v));
    return 11;
}


// ---- World PvP and arena seasons ----

uint32_t worldStateValue(game::GameHandler* gh, uint32_t id) {
    const auto v = gh ? gh->getWorldState(id) : std::nullopt;
    return v ? *v : 0;
}

// GetWorldPVPQueueStatus(1) -> status, zone name, battle id, seconds left on
// an invitation (0x54C8A0). Any other index answers nothing.
int lua_GetWorldPVPQueueStatus(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int i = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || i != 1) return 0;
    const auto& q = gh->getWorldPvpQueue();
    lua_pushstring(L, game::bf::statusToken(q.status));
    const std::string zone = q.zoneId ? gh->getAreaName(q.zoneId) : std::string();
    if (zone.empty()) lua_pushnil(L); else lua_pushstring(L, zone.c_str());
    lua_pushnumber(L, q.battleId);
    int64_t left = 0;
    if (q.status == 2) left = static_cast<int64_t>(q.acceptBy) - gh->getWorldStateServerTime();
    lua_pushnumber(L, static_cast<double>(left > 0 ? left : 0));
    return 4;
}

// CanQueueForWintergrasp(): world state 4375 set (0x543600).
int lua_CanQueueForWintergrasp(lua_State* L) {
    pushOneOrNil(L, worldStateValue(getGameHandler(L), 4375) != 0);
    return 1;
}

// GetWintergraspWaitTime(): while world state 3801 is set (no battle), the
// seconds to world state 4354, the next battle's server time; nothing during
// the battle (0x5435A0).
int lua_GetWintergraspWaitTime(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || worldStateValue(gh, 3801) == 0) return 0;
    const int64_t left = static_cast<int64_t>(worldStateValue(gh, 4354)) - gh->getWorldStateServerTime();
    lua_pushnumber(L, static_cast<double>(left > 0 ? left : 0));
    return 1;
}

// GetCurrentArenaSeason() / GetPreviousArenaSeason(): world states 3191 and
// 3901 (0x5A2A40, 0x5A2A70). Zero reads as the off-season and disables the
// rated arena buttons.
int lua_GetCurrentArenaSeason(lua_State* L) {
    lua_pushnumber(L, worldStateValue(getGameHandler(L), 3191));
    return 1;
}
int lua_GetPreviousArenaSeason(lua_State* L) {
    lua_pushnumber(L, worldStateValue(getGameHandler(L), 3901));
    return 1;
}

// ---- PvP fields (3.3.5 indices; PLAYER_FIELD_HONOR_CURRENCY at 0x4FD
// identifies the layout) ----

uint32_t playerField(game::GameHandler* gh, uint16_t index) {
    if (!gh || game::fieldIndex(game::UF::PLAYER_FIELD_HONOR_CURRENCY) != 0x4FD) return 0;
    auto e = gh->getEntityManager().getEntity(gh->getPlayerGuid());
    return e ? e->getField(index) : 0;
}

// GetPVPSessionStats() -> today's kills, honor (0x611A20).
int lua_GetPVPSessionStats(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, playerField(gh, 0x4C9) & 0xFFFF);
    lua_pushnumber(L, playerField(gh, 0x4CA));
    return 2;
}
// GetPVPYesterdayStats() -> yesterday's kills, honor (0x611AD0).
int lua_GetPVPYesterdayStats(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, playerField(gh, 0x4C9) >> 16);
    lua_pushnumber(L, playerField(gh, 0x4CB));
    return 2;
}
// GetPVPLifetimeStats() -> lifetime kills, highest rank (0 below 5) (0x611B80).
int lua_GetPVPLifetimeStats(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, playerField(gh, 0x4CC));
    const uint32_t rank = playerField(gh, 0x4AD) >> 24;
    lua_pushnumber(L, rank < 5 ? 0 : rank);
    return 2;
}

// UnitIsPVP / UnitIsPVPFreeForAll / UnitIsPVPSanctuary: UNIT_FIELD_BYTES_2
// byte 1 bits 0x01, 0x04, 0x08 (0x60CE20, 0x60CFB0, 0x60CF20).
int unitPvpFlag(lua_State* L, uint32_t bit) {
    auto* gh = getGameHandler(L);
    const char* uid = luaL_optstring(L, 1, "player");
    uint64_t guid = gh ? resolveUnitGuid(gh, std::string(uid)) : 0;
    auto e = guid ? gh->getEntityManager().getEntity(guid) : nullptr;
    if (!e) return luaReturnNil(L);
    const uint32_t bytes2 = e->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_2));
    pushOneOrNil(L, ((bytes2 >> 8) & bit) != 0);
    return 1;
}
int lua_UnitIsPVP(lua_State* L) { return unitPvpFlag(L, 0x01); }
int lua_UnitIsPVPFreeForAll(lua_State* L) { return unitPvpFlag(L, 0x04); }
int lua_UnitIsPVPSanctuary(lua_State* L) { return unitPvpFlag(L, 0x08); }

// ---- Instances (Map.dbc, MapDifficulty.dbc) ----

const char* instanceTypeToken(uint32_t type) {
    static const char* const kTypes[] = {"none", "party", "raid", "pvp", "arena"};  // 0xAC8074
    return type < 5 ? kTypes[type] : "none";
}

std::string difficultyName(game::GameHandler* gh, const std::string& key) {
    if (key.empty()) return {};
    std::string s = game::globalString(gh->services().assetManager, key);
    return s.empty() ? key : s;
}

// IsInInstance() -> 1 or nil, and the type token (0x5156A0).
int lua_IsInInstance(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t type = gh ? gh->getMapInstanceType(gh->getCurrentMapId()) : 0;
    pushOneOrNil(L, type != 0);
    lua_pushstring(L, instanceTypeToken(type));
    return 2;
}

// GetInstanceInfo() -> name, type, difficultyIndex, difficultyName,
// maxPlayers, dynamicDifficulty, isDynamic (0x51A8C0).
int lua_GetInstanceInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* soc = gh ? gh->battlefield() : nullptr;
    const uint32_t map = gh ? gh->getCurrentMapId() : 0;
    const std::string name = gh ? gh->getMapName(map) : std::string();
    if (!gh || !soc || name.empty()) {
        lua_pushnil(L); lua_pushstring(L, "none"); lua_pushnumber(L, 0);
        lua_pushstring(L, ""); lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnil(L);
        return 7;
    }
    const uint32_t diff = soc->getInstanceDifficulty();
    const auto md = gh->getMapDifficulty(map, diff);
    lua_pushstring(L, name.c_str());
    lua_pushstring(L, instanceTypeToken(gh->getMapInstanceType(map)));
    lua_pushnumber(L, diff + 1);
    lua_pushstring(L, difficultyName(gh, md.difficultyKey).c_str());
    lua_pushnumber(L, md.found ? md.maxPlayers : 0);
    lua_pushnumber(L, soc->getInstanceDynamicHeroic());
    lua_pushboolean(L, (gh->getMapFlags(map) & 0x100) != 0);
    return 7;
}

// GetInstanceDifficulty() (0x515750), GetDungeonDifficulty() (0x515790) and
// GetRaidDifficulty() (0x515810): the setting in force and the personal one.
int lua_GetInstanceDifficulty(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh && gh->battlefield() ? gh->battlefield()->getInstanceDifficulty() + 1 : 1);
    return 1;
}
int lua_GetDungeonDifficulty(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* soc = gh ? gh->battlefield() : nullptr;
    lua_pushnumber(L, soc ? soc->effectiveDungeonDifficulty() + 1 : 1);
    lua_pushnumber(L, soc ? soc->personalDungeonDifficulty() + 1 : 1);
    return 2;
}
int lua_GetRaidDifficulty(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* soc = gh ? gh->battlefield() : nullptr;
    lua_pushnumber(L, soc ? soc->effectiveRaidDifficulty() + 1 : 1);
    lua_pushnumber(L, soc ? soc->personalRaidDifficulty() + 1 : 1);
    return 2;
}
int lua_SetDungeonDifficulty(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int n = static_cast<int>(luaL_checknumber(L, 1));
    if (gh && gh->battlefield() && n > 0) gh->battlefield()->setDifficultyFromUi(static_cast<uint32_t>(n), false);
    return 0;
}
int lua_SetRaidDifficulty(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int n = static_cast<int>(luaL_checknumber(L, 1));
    if (gh && gh->battlefield() && n > 0) gh->battlefield()->setDifficultyFromUi(static_cast<uint32_t>(n), true);
    return 0;
}

// GetSavedInstanceInfo(i) -> name, id, reset, difficulty, locked, extended,
// instanceIDMostSig, isRaid, maxPlayers, difficultyName (0x4FF2C0).
int lua_GetSavedInstanceInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int i = static_cast<int>(luaL_checknumber(L, 1));
    const game::InstanceLockout* lo = nullptr;
    if (gh) {
        const auto& all = gh->getInstanceLockouts();
        if (i >= 1 && i <= static_cast<int>(all.size())) lo = &all[static_cast<size_t>(i - 1)];
    }
    const std::string name = lo ? gh->getMapName(lo->mapId) : std::string();
    if (!lo || name.empty()) {
        lua_pushnil(L); lua_pushnumber(L, 0); lua_pushnil(L); lua_pushnumber(L, 0);
        lua_pushboolean(L, 0); lua_pushboolean(L, 0); lua_pushnumber(L, 0);
        lua_pushboolean(L, 0); lua_pushnumber(L, 0); lua_pushstring(L, "");
        return 10;
    }
    const auto md = gh->getMapDifficulty(lo->mapId, lo->difficulty);
    lua_pushstring(L, name.c_str());
    lua_pushnumber(L, static_cast<double>(lo->instanceId & 0xFFFFFFFFu));
    lua_pushnumber(L, lo->resetSeconds);
    lua_pushnumber(L, lo->difficulty + 1);
    lua_pushboolean(L, lo->locked);
    lua_pushboolean(L, lo->extended);
    lua_pushnumber(L, static_cast<double>(lo->instanceId >> 32));
    lua_pushboolean(L, gh->getMapInstanceType(lo->mapId) == 2);
    lua_pushnumber(L, md.found ? md.maxPlayers : 0);
    lua_pushstring(L, difficultyName(gh, md.difficultyKey).c_str());
    return 10;
}

// ---- Arena opponents ----

// GetNumArenaOpponents() (0x54A2C0).
int lua_GetNumArenaOpponents(lua_State* L) {
    const auto* soc = bfState(L);
    lua_pushnumber(L, soc ? soc->arenaOpponentCount() : 0);
    return 1;
}

// ---- Arena teams (ArenaTeamInfo.cpp) ----
//
// The team at an index is the slot of PLAYER_FIELD_ARENA_TEAM_INFO (1 2v2,
// 2 3v3, 3 5v5), not the order the server's packets happened to come in.

game::SocialHandler* bfMutable(lua_State* L) {
    auto* gh = getGameHandler(L);
    return gh ? gh->battlefield() : nullptr;
}

int teamSlotArg(lua_State* L, int idx) {
    const int i = static_cast<int>(luaL_optnumber(L, idx, 0));
    return (i >= 1 && i <= static_cast<int>(game::arena::kTeamSlots)) ? i - 1 : -1;
}

void pushColor(lua_State* L, uint32_t color) {
    for (float c : game::arena::colorComponents(color)) lua_pushnumber(L, c);
}

// GetArenaTeam(i) (0x5A3860): name, size, rating, played, wins, season
// played, season wins, the player's played and season played, rank, the
// player's rating, then the tabard: background r g b, emblem, its r g b,
// border, its r g b. Nil and twenty-one zeros until the team's record is in
// the cache - a miss asks the server for it (CMSG_ARENA_TEAM_QUERY), whose
// answer fires ARENA_TEAM_UPDATE. UIParent asks for all three on
// PLAYER_ENTERING_WORLD for exactly that reason.
int lua_GetArenaTeam(lua_State* L) {
    auto* soc = bfMutable(L);
    const int slot = teamSlotArg(L, 1);
    const game::arena::TeamInfo info = (soc && slot >= 0) ? soc->arenaTeamInfo(static_cast<size_t>(slot))
                                                          : game::arena::TeamInfo{};
    const game::arena::TeamQuery* q = (soc && info.teamId) ? soc->arenaTeamQuery(info.teamId) : nullptr;
    if (!q) {
        lua_pushnil(L);
        for (int i = 0; i < 21; ++i) lua_pushnumber(L, 0);
        return 22;
    }
    const game::ArenaTeamStats* st = soc->arenaTeamStatsFor(info.teamId);
    lua_pushstring(L, q->name.c_str());
    lua_pushnumber(L, q->type);
    lua_pushnumber(L, st ? st->rating : 0);
    lua_pushnumber(L, st ? st->weekGames : 0);
    lua_pushnumber(L, st ? st->weekWins : 0);
    lua_pushnumber(L, st ? st->seasonGames : 0);
    lua_pushnumber(L, st ? st->seasonWins : 0);
    lua_pushnumber(L, info.gamesWeek);
    lua_pushnumber(L, info.gamesSeason);
    lua_pushnumber(L, st ? st->rank : 0);
    lua_pushnumber(L, info.personalRating);
    pushColor(L, q->backgroundColor);
    lua_pushnumber(L, q->emblemStyle);
    pushColor(L, q->emblemColor);
    lua_pushnumber(L, q->borderStyle);
    pushColor(L, q->borderColor);
    return 22;
}

// IsArenaTeamCaptain(i) (0x60DC70): the slot's member field is 0. An empty
// slot reads 0 too, and stock answers 1 for it as well.
int lua_IsArenaTeamCaptain(lua_State* L) {
    const auto* soc = bfState(L);
    const int slot = teamSlotArg(L, 1);
    pushOneOrNil(L, soc && slot >= 0 && soc->arenaTeamInfo(static_cast<size_t>(slot)).member == 0);
    return 1;
}

// IsInArenaTeam() (0x60DBF0): any slot with a team; a boolean.
int lua_IsInArenaTeam(lua_State* L) {
    const auto* soc = bfState(L);
    bool any = false;
    for (size_t i = 0; soc && i < game::arena::kTeamSlots; ++i)
        any = any || soc->arenaTeamInfo(i).teamId != 0;
    lua_pushboolean(L, any ? 1 : 0);
    return 1;
}

// ArenaTeamRoster(i) (0x5A3CF0 -> 0x5A3600).
int lua_ArenaTeamRoster(lua_State* L) {
    auto* soc = bfMutable(L);
    const int slot = teamSlotArg(L, 1);
    if (soc && slot >= 0) soc->requestArenaTeamRosterSlot(static_cast<size_t>(slot));
    return 0;
}

const game::ArenaTeamRoster* slotRoster(lua_State* L, int idx) {
    auto* gh = getGameHandler(L);
    const auto* soc = gh ? gh->battlefield() : nullptr;
    const int slot = teamSlotArg(L, idx);
    if (!soc || slot < 0) return nullptr;
    const uint32_t id = soc->arenaTeamInfo(static_cast<size_t>(slot)).teamId;
    return id ? gh->getArenaTeamRoster(id) : nullptr;
}

// GetNumArenaTeamMembers(i [, showOffline]) (0x5A2930): every member while
// the show-offline switch is on (its default), else the online ones.
int lua_GetNumArenaTeamMembers(lua_State* L) {
    const auto* r = slotRoster(L, 1);
    lua_pushnumber(L, r ? static_cast<lua_Number>(r->members.size()) : 0);
    return 1;
}

// GetArenaTeamRosterInfo(i, n) (0x5A2FC0): name, rank (0 captain, 1
// member), level, class (the localized name), online (1 or nil), played,
// wins, season played, season wins, rating.
int lua_GetArenaTeamRosterInfo(lua_State* L) {
    const auto* r = slotRoster(L, 1);
    const int n = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (!r || n < 1 || n > static_cast<int>(r->members.size())) {
        lua_pushnil(L); lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnil(L); lua_pushnil(L);
        for (int i = 0; i < 5; ++i) lua_pushnumber(L, 0);
        return 10;
    }
    const auto& m = r->members[static_cast<size_t>(n) - 1];
    lua_pushstring(L, m.name.c_str());
    lua_pushnumber(L, m.isCaptain ? 0 : 1);
    lua_pushnumber(L, m.level);
    if (m.classId > 0 && m.classId < 12 && kLuaClasses[m.classId][0] != '\0')
        lua_pushstring(L, kLuaClasses[m.classId]);
    else
        lua_pushnil(L);
    pushOneOrNil(L, m.online);
    lua_pushnumber(L, m.weekGames);
    lua_pushnumber(L, m.weekWins);
    lua_pushnumber(L, m.seasonGames);
    lua_pushnumber(L, m.seasonWins);
    lua_pushnumber(L, m.personalRating);
    return 10;
}

// GetInspectArenaTeamData(i) (0x5E8030): name, size, rating, season played,
// season wins, the player's season played, the player's rating, and the
// tabard - eighteen; nothing at all until the reply and the team's record
// are both in.
int lua_GetInspectArenaTeamData(lua_State* L) {
    auto* gh = getGameHandler(L);
    auto* soc = gh ? gh->battlefield() : nullptr;
    const int slot = teamSlotArg(L, 1);
    const auto* ir = gh ? gh->getInspectResult() : nullptr;
    if (!soc || !ir || slot < 0) return 0;
    const auto& t = ir->arenaTeams[static_cast<size_t>(slot)];
    if (t.teamId == 0) return 0;
    const game::arena::TeamQuery* q = soc->arenaTeamQuery(t.teamId);
    if (!q) return 0;
    lua_pushstring(L, q->name.c_str());
    lua_pushnumber(L, q->type);
    lua_pushnumber(L, t.rating);
    lua_pushnumber(L, t.seasonGames);
    lua_pushnumber(L, t.seasonWins);
    lua_pushnumber(L, t.playerSeasonGames);
    lua_pushnumber(L, t.personalRating);
    pushColor(L, q->backgroundColor);
    lua_pushnumber(L, q->emblemStyle);
    pushColor(L, q->emblemColor);
    lua_pushnumber(L, q->borderStyle);
    pushColor(L, q->borderColor);
    return 18;
}

// GetZonePVPInfo() (0x51BA50): the area under the player decides sanctuary,
// arena and combat; past those, nothing on a realm without player killing
// unless the zone is a capital; then friendly, hostile or contested by the
// zone's FactionGroupMask, and the faction's name.
int lua_GetZonePVPInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    uint32_t zoneId = 0, areaId = 0;
    if (auto* svc = getLuaServices(L)) {
        if (svc->getLiveZoneId) zoneId = svc->getLiveZoneId();
        if (svc->getLiveAreaId) areaId = svc->getLiveAreaId();
    }
    if (zoneId == 0) zoneId = gh->getWorldStateZoneId();
    if (areaId == 0) areaId = gh->getWorldStateAreaId();
    auto row = [gh](uint32_t id) -> std::optional<game::bf::AreaPvp> {
        const auto* a = gh->areaPvpInfo(id);
        if (!a) return std::nullopt;
        return game::bf::AreaPvp{a->flags, a->team};
    };
    const auto area = row(areaId);
    const auto zone = row(zoneId);
    // The player's faction template's friend and enemy groups: the player's
    // own side (Alliance 2, Horde 4), and the other side with the monsters.
    const bool alliance = gh->isPlayerAlliance();
    const uint32_t friendGroups = alliance ? 0x2u : 0x4u;
    const uint32_t enemyGroups = alliance ? 0xCu : 0xAu;
    static std::vector<std::pair<uint32_t, std::string>> groups;  // FactionGroup.dbc: mask id, name
    if (groups.empty() && gh->services().assetManager && gh->services().assetManager->isInitialized()) {
        auto dbc = gh->services().assetManager->loadDBC("FactionGroup.dbc");
        if (dbc && dbc->isLoaded() && dbc->getFieldCount() > 3)
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i)
                groups.emplace_back(dbc->getUInt32(i, 1), dbc->getString(i, 3));
    }
    auto factionName = [](uint32_t mask) -> std::string {
        for (const auto& [bit, name] : groups)
            if (bit < 32 && (mask & (1u << bit)) && !name.empty()) return name;
        return {};
    };
    const bool hasPlayer = gh->getEntityManager().hasEntity(gh->getPlayerGuid());
    const game::bf::ZonePvp z = game::bf::zonePvpInfo(area ? &*area : nullptr, zone ? &*zone : nullptr,
                                                      gh->realmAllowsPlayerKilling(), hasPlayer,
                                                      friendGroups, enemyGroups, factionName);
    if (z.count == 0) return 0;
    lua_pushstring(L, z.type.c_str());
    pushOneOrNil(L, z.fromArea);
    if (z.count == 2) return 2;
    lua_pushstring(L, z.faction.c_str());
    return 3;
}

// IsPVPTimerRunning() (0x51BD60): PLAYER_FLAGS_PVP_TIMER, 1 or nil.
// GetPVPTimer() (0x51BD00): milliseconds, plus a second - 301000 with no
// timer, 999 once it has run out - and 0 without a player.
int lua_IsPVPTimerRunning(lua_State* L) {
    auto* gh = getGameHandler(L);
    auto me = gh ? gh->getEntityManager().getEntity(gh->getPlayerGuid()) : nullptr;
    pushOneOrNil(L, me && (me->getField(game::fieldIndex(game::UF::PLAYER_FLAGS)) & 0x40000) != 0);
    return 1;
}
int lua_GetPVPTimer(lua_State* L) {
    auto* gh = getGameHandler(L);
    const bool me = gh && gh->getEntityManager().hasEntity(gh->getPlayerGuid());
    lua_pushnumber(L, me ? static_cast<lua_Number>(gh->pvpTimerRemainingMs() + 1000) : 0);
    return 1;
}

}  // namespace

void registerBattlefieldLuaAPI(lua_State* L) {
    static const std::pair<const char*, lua_CFunction> api[] = {
        {"GetBattlefieldStatus",             lua_GetBattlefieldStatus},
        {"GetBattlefieldPortExpiration",     lua_GetBattlefieldPortExpiration},
        {"GetBattlefieldTimeWaited",         lua_GetBattlefieldTimeWaited},
        {"GetBattlefieldEstimatedWaitTime",  lua_GetBattlefieldEstimatedWaitTime},
        {"GetBattlefieldInstanceExpiration", lua_GetBattlefieldInstanceExpiration},
        {"GetBattlefieldInstanceRunTime",    lua_GetBattlefieldInstanceRunTime},
        {"IsActiveBattlefieldArena",         lua_IsActiveBattlefieldArena},
        {"GetBattlefieldArenaFaction",       lua_GetBattlefieldArenaFaction},
        {"AcceptBattlefieldPort",            lua_AcceptBattlefieldPort},
        {"LeaveBattlefield",                 lua_LeaveBattlefield},
        {"GetNumBattlefields",               lua_GetNumBattlefields},
        {"GetBattlefieldInstanceInfo",       lua_GetBattlefieldInstanceInfo},
        {"GetBattlefieldInfo",               lua_GetBattlefieldInfo},
        {"IsBattlefieldArena",               lua_IsBattlefieldArena},
        {"CanJoinBattlefieldAsGroup",        lua_CanJoinBattlefieldAsGroup},
        {"GetSelectedBattlefield",           lua_GetSelectedBattlefield},
        {"SetSelectedBattlefield",           lua_SetSelectedBattlefield},
        {"JoinBattlefield",                  lua_JoinBattlefield},
        {"CloseBattlefield",                 lua_CloseBattlefield},
        {"GetHolidayBGHonorCurrencyBonuses", lua_GetHolidayBGHonorCurrencyBonuses},
        {"GetRandomBGHonorCurrencyBonuses",  lua_GetRandomBGHonorCurrencyBonuses},
        {"GetNumBattlegroundTypes",          lua_GetNumBattlegroundTypes},
        {"GetBattlegroundInfo",              lua_GetBattlegroundInfo},
        {"RequestBattlegroundInstanceInfo",  lua_RequestBattlegroundInstanceInfo},
        {"RequestBattlefieldScoreData",      lua_RequestBattlefieldScoreData},
        {"GetNumBattlefieldScores",          lua_GetNumBattlefieldScores},
        {"GetBattlefieldScore",              lua_GetBattlefieldScore},
        {"SetBattlefieldScoreFaction",       lua_SetBattlefieldScoreFaction},
        {"SortBattlefieldScoreData",         lua_SortBattlefieldScoreData},
        {"GetBattlefieldWinner",             lua_GetBattlefieldWinner},
        {"GetBattlefieldTeamInfo",           lua_GetBattlefieldTeamInfo},
        {"GetNumBattlefieldStats",           lua_GetNumBattlefieldStats},
        {"GetBattlefieldStatInfo",           lua_GetBattlefieldStatInfo},
        {"GetBattlefieldStatData",           lua_GetBattlefieldStatData},
        {"ReportPlayerIsPVPAFK",             lua_ReportPlayerIsPVPAFK},
        {"GetNumWorldStateUI",               lua_GetNumWorldStateUI},
        {"GetWorldStateUIInfo",              lua_GetWorldStateUIInfo},
        {"GetWorldPVPQueueStatus",           lua_GetWorldPVPQueueStatus},
        {"CanQueueForWintergrasp",           lua_CanQueueForWintergrasp},
        {"GetWintergraspWaitTime",           lua_GetWintergraspWaitTime},
        {"GetCurrentArenaSeason",            lua_GetCurrentArenaSeason},
        {"GetPreviousArenaSeason",           lua_GetPreviousArenaSeason},
        {"GetPVPSessionStats",               lua_GetPVPSessionStats},
        {"GetPVPYesterdayStats",             lua_GetPVPYesterdayStats},
        {"GetPVPLifetimeStats",              lua_GetPVPLifetimeStats},
        {"UnitIsPVP",                        lua_UnitIsPVP},
        {"UnitIsPVPFreeForAll",              lua_UnitIsPVPFreeForAll},
        {"UnitIsPVPSanctuary",               lua_UnitIsPVPSanctuary},
        {"IsInInstance",                     lua_IsInInstance},
        {"GetInstanceInfo",                  lua_GetInstanceInfo},
        {"GetInstanceDifficulty",            lua_GetInstanceDifficulty},
        {"GetDungeonDifficulty",             lua_GetDungeonDifficulty},
        {"GetRaidDifficulty",                lua_GetRaidDifficulty},
        {"SetDungeonDifficulty",             lua_SetDungeonDifficulty},
        {"SetRaidDifficulty",                lua_SetRaidDifficulty},
        {"GetSavedInstanceInfo",             lua_GetSavedInstanceInfo},
        {"GetNumArenaOpponents",             lua_GetNumArenaOpponents},
        {"GetArenaTeam",                     lua_GetArenaTeam},
        {"IsArenaTeamCaptain",               lua_IsArenaTeamCaptain},
        {"IsInArenaTeam",                    lua_IsInArenaTeam},
        {"ArenaTeamRoster",                  lua_ArenaTeamRoster},
        {"GetNumArenaTeamMembers",           lua_GetNumArenaTeamMembers},
        {"GetArenaTeamRosterInfo",           lua_GetArenaTeamRosterInfo},
        {"GetInspectArenaTeamData",          lua_GetInspectArenaTeamData},
        {"GetZonePVPInfo",                   lua_GetZonePVPInfo},
        {"IsPVPTimerRunning",                lua_IsPVPTimerRunning},
        {"GetPVPTimer",                      lua_GetPVPTimer},
    };
    for (const auto& [name, func] : api) {
        lua_pushcfunction(L, func);
        lua_setglobal(L, name);
    }
}

}  // namespace wowee::addons
