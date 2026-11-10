// More of the 3.3.5 global API, the in-game part of what the client has and
// this did not: each answered from what the client keeps, as Wow.exe does.

#include "addons/lua_api_helpers.hpp"
#include "addons/lua_api_registrations.hpp"
#include "addons/lua_engine.hpp"
#include "game/game_handler.hpp"
#include "game/opcodes.hpp"
#include "network/world_socket.hpp"
#include "network/packet.hpp"
#include "ui/widget_tree.hpp"

#include <SDL3/SDL.h>
#include <cctype>
#include <string>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace wowee::addons {

namespace {

uint32_t itemIdOf(lua_State* L, int index) {
    if (lua_type(L, index) == LUA_TNUMBER) return static_cast<uint32_t>(lua_tonumber(L, index));
    if (lua_type(L, index) != LUA_TSTRING) return 0;
    const std::string s = lua_tostring(L, index);
    const auto pos = s.find("item:");
    if (pos == std::string::npos) return 0;
    try {
        return static_cast<uint32_t>(std::stoul(s.substr(pos + 5)));
    } catch (...) {
        return 0;
    }
}

/// ItemModType, as GetItemStats names them: "ITEM_MOD_" .. this .. "_SHORT".
const char* itemModName(uint32_t type) {
    switch (type) {
        case 0: return "MANA";
        case 1: return "HEALTH";
        case 3: return "AGILITY";
        case 4: return "STRENGTH";
        case 5: return "INTELLECT";
        case 6: return "SPIRIT";
        case 7: return "STAMINA";
        case 12: return "DEFENSE_SKILL_RATING";
        case 13: return "DODGE_RATING";
        case 14: return "PARRY_RATING";
        case 15: return "BLOCK_RATING";
        case 16: return "HIT_MELEE_RATING";
        case 17: return "HIT_RANGED_RATING";
        case 18: return "HIT_SPELL_RATING";
        case 19: return "CRIT_MELEE_RATING";
        case 20: return "CRIT_RANGED_RATING";
        case 21: return "CRIT_SPELL_RATING";
        case 22: return "HIT_TAKEN_MELEE_RATING";
        case 23: return "HIT_TAKEN_RANGED_RATING";
        case 24: return "HIT_TAKEN_SPELL_RATING";
        case 25: return "CRIT_TAKEN_MELEE_RATING";
        case 26: return "CRIT_TAKEN_RANGED_RATING";
        case 27: return "CRIT_TAKEN_SPELL_RATING";
        case 28: return "HASTE_MELEE_RATING";
        case 29: return "HASTE_RANGED_RATING";
        case 30: return "HASTE_SPELL_RATING";
        case 31: return "HIT_RATING";
        case 32: return "CRIT_RATING";
        case 33: return "HIT_TAKEN_RATING";
        case 34: return "CRIT_TAKEN_RATING";
        case 35: return "RESILIENCE_RATING";
        case 36: return "HASTE_RATING";
        case 37: return "EXPERTISE_RATING";
        case 38: return "ATTACK_POWER";
        case 39: return "RANGED_ATTACK_POWER";
        case 40: return "FERAL_ATTACK_POWER";
        case 41: return "SPELL_HEALING_DONE";
        case 42: return "SPELL_DAMAGE_DONE";
        case 43: return "MANA_REGENERATION";
        case 44: return "ARMOR_PENETRATION_RATING";
        case 45: return "SPELL_POWER";
        case 46: return "HEALTH_REGEN";
        case 47: return "SPELL_PENETRATION";
        case 48: return "BLOCK_VALUE";
        default: return nullptr;
    }
}

void addStat(lua_State* L, int table, const std::string& key, double value, double sign) {
    if (value == 0.0) return;
    lua_pushstring(L, key.c_str());
    lua_rawget(L, table);
    const double before = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
    lua_pop(L, 1);
    const double after = before + sign * value;
    lua_pushstring(L, key.c_str());
    if (after == 0.0) lua_pushnil(L); else lua_pushnumber(L, after);
    lua_rawset(L, table);
}

/// The stats of one item, added (sign 1) or taken away (sign -1) from the
/// table at `table`: its ITEM_MOD stats, armour and resistances
/// (RESISTANCE0_NAME to RESISTANCE6_NAME), its empty sockets by colour and a
/// weapon's damage per second - the keys GetItemStats fills (0x0050E3A0's
/// table, named from GlobalStrings).
bool addItemStats(lua_State* L, game::GameHandler* gh, uint32_t itemId, int table, double sign) {
    const auto* info = gh ? gh->getItemInfo(itemId) : nullptr;
    if (!info || !info->valid) return false;
    auto mod = [&](uint32_t type, int32_t value) {
        if (const char* n = itemModName(type)) {
            addStat(L, table, std::string("ITEM_MOD_") + n + "_SHORT", value, sign);
        }
    };
    mod(3, info->agility);
    mod(4, info->strength);
    mod(5, info->intellect);
    mod(6, info->spirit);
    mod(7, info->stamina);
    for (const auto& s : info->extraStats) mod(s.statType, s.statValue);
    addStat(L, table, "RESISTANCE0_NAME", info->armor, sign);
    addStat(L, table, "RESISTANCE1_NAME", info->holyRes, sign);
    addStat(L, table, "RESISTANCE2_NAME", info->fireRes, sign);
    addStat(L, table, "RESISTANCE3_NAME", info->natureRes, sign);
    addStat(L, table, "RESISTANCE4_NAME", info->frostRes, sign);
    addStat(L, table, "RESISTANCE5_NAME", info->shadowRes, sign);
    addStat(L, table, "RESISTANCE6_NAME", info->arcaneRes, sign);
    for (uint32_t color : info->socketColor) {
        if (color & 1) addStat(L, table, "EMPTY_SOCKET_META", 1, sign);
        if (color & 2) addStat(L, table, "EMPTY_SOCKET_RED", 1, sign);
        if (color & 4) addStat(L, table, "EMPTY_SOCKET_YELLOW", 1, sign);
        if (color & 8) addStat(L, table, "EMPTY_SOCKET_BLUE", 1, sign);
    }
    if (info->delayMs > 0 && info->damageMax > 0.0f) {
        const double dps = (info->damageMin + info->damageMax) * 0.5 / (info->delayMs / 1000.0);
        addStat(L, table, "ITEM_MOD_DAMAGE_PER_SECOND_SHORT", dps, sign);
    }
    return true;
}

/// GetItemStats(link [, table]) - the item's stats keyed as above; nothing
/// for an item the client has not seen yet.
int lua_GetItemStats(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t id = itemIdOf(L, 1);
    if (lua_istable(L, 2)) lua_pushvalue(L, 2); else lua_newtable(L);
    const int t = lua_gettop(L);
    if (!addItemStats(L, gh, id, t, 1.0)) return 0;
    return 1;
}

/// GetItemStatDelta(link1, link2 [, table]) - the first item's stats less
/// the second's, as a comparison tooltip reads them.
int lua_GetItemStatDelta(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t a = itemIdOf(L, 1);
    const uint32_t b = itemIdOf(L, 2);
    if (lua_istable(L, 3)) lua_pushvalue(L, 3); else lua_newtable(L);
    const int t = lua_gettop(L);
    const bool haveA = addItemStats(L, gh, a, t, 1.0);
    const bool haveB = addItemStats(L, gh, b, t, -1.0);
    if (!haveA && !haveB) return 0;
    return 1;
}

/// QueryQuestsCompleted() - ask the server for every quest this character
/// has finished; QUEST_QUERY_COMPLETE says the answer is in, and
/// GetQuestsCompleted reads it.
int lua_QueryQuestsCompleted(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->getSocket()) return 0;
    const uint16_t wire = game::wireOpcode(game::Opcode::CMSG_QUERY_QUESTS_COMPLETED);
    if (wire == 0xFFFF) return 0;
    network::Packet pkt(wire);
    gh->getSocket()->send(pkt);
    return 0;
}

/// HideCursor() / ShowCursor() - the hardware pointer, as the client hides it
/// while the mouse turns the camera.
int lua_HideCursor(lua_State*) {
    SDL_HideCursor();
    return 0;
}
int lua_ShowCursor(lua_State*) {
    SDL_ShowCursor();
    return 0;
}

/// GetNumFrames() - how many frames exist (textures and font strings are
/// regions, not frames).
int lua_GetNumFrames(lua_State* L) {
    auto* tree = getWidgetTree(L);
    int n = 0;
    if (tree) {
        for (size_t id = 1; id < tree->size(); ++id) {
            const auto* w = tree->get(static_cast<uint32_t>(id));
            if (w && w->kind == ui::WidgetKind::Frame) ++n;
        }
    }
    lua_pushinteger(L, n);
    return 1;
}

/// GetCurrentKeyBoardFocus() - the edit box typing goes to, or nil.
int lua_GetCurrentKeyBoardFocus(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_lua_engine");
    auto* engine = static_cast<LuaEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    const uint32_t id = engine ? engine->focusedWidget() : 0;
    if (id == 0) { lua_pushnil(L); return 1; }
    lua_getglobal(L, "__WoweeFramesByWid");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_pushnil(L); return 1; }
    lua_rawgeti(L, -1, static_cast<int>(id));
    lua_remove(L, -2);
    return 1;
}

/// FindSpellBookSlotByID(spellID) - the spell book slot holding that spell.
int lua_FindSpellBookSlotByID(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto want = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
    if (!gh || want == 0) return 0;
    int slot = 0;
    for (const auto& tab : gh->getSpellBookTabs()) {
        for (uint32_t id : tab.spellIds) {
            ++slot;
            if (id == want) {
                lua_pushinteger(L, slot);
                return 1;
            }
        }
    }
    return 0;
}

/// IsIgnored(name) - whether that player is on the ignore list.
int lua_IsIgnored(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* name = luaL_optstring(L, 1, "");
    if (!gh || !*name) { lua_pushnil(L); return 1; }
    std::string want(name);
    for (char& c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& [n, guid] : gh->getIgnoreCache()) {
        std::string have = n;
        for (char& c : have) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (have == want) { lua_pushboolean(L, 1); return 1; }
    }
    lua_pushnil(L);
    return 1;
}

/// GetRuneCount(id) - 1 while the rune is ready, 0 while it recharges.
int lua_GetRuneCount(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int id = static_cast<int>(luaL_optinteger(L, 1, 0));
    if (!gh || id < 1 || id > 6) return 0;
    lua_pushinteger(L, gh->getPlayerRunes()[static_cast<size_t>(id) - 1].ready ? 1 : 0);
    return 1;
}

}  // namespace

void registerStockMoreLuaAPI(lua_State* L) {
    static const luaL_Reg fns[] = {
        {"GetItemStats", lua_GetItemStats},
        {"GetItemStatDelta", lua_GetItemStatDelta},
        {"QueryQuestsCompleted", lua_QueryQuestsCompleted},
        {"HideCursor", lua_HideCursor},
        {"ShowCursor", lua_ShowCursor},
        {"GetNumFrames", lua_GetNumFrames},
        {"GetCurrentKeyBoardFocus", lua_GetCurrentKeyBoardFocus},
        {"FindSpellBookSlotByID", lua_FindSpellBookSlotByID},
        {"IsIgnored", lua_IsIgnored},
        {"GetRuneCount", lua_GetRuneCount},
        {nullptr, nullptr},
    };
    for (const luaL_Reg* r = fns; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setglobal(L, r->name);
    }

    // The rest are the client answering from what it already answers
    // elsewhere, so they are written in terms of those.
    luaL_dostring(L,
        // Mute, as the client keeps it, counts as ignored for this question.
        "function IsIgnoredOrMuted(name) return IsIgnored(name) end\n"
        // The party or raid leader in the group the player is really in -
        // which, with no battleground or dungeon-finder group standing in
        // for it, is the group the player is in.
        "function IsRealPartyLeader() return IsPartyLeader() end\n"
        "function IsRealRaidLeader() return IsRaidLeader() end\n"
        "function GetMouseButtonName(i)\n"
        "    local names = {'LeftButton', 'RightButton', 'MiddleButton', 'Button4', 'Button5'}\n"
        "    return names[i]\n"
        "end\n"
        "function GetClientExpansionLevel() return 2 end\n"
        "function IsDesaturateSupported() return 1 end\n"
        "function IsDebugBuild() return nil end\n"
        "function IsConsoleActive() return nil end\n"
        "function CursorHasMoney() if GetCursorInfo() == 'money' then return 1 end end\n"
        "function CursorHasMacro() if GetCursorInfo() == 'macro' then return 1 end end\n"
        // value, default, stored account-wide, stored per character.
        "function GetCVarInfo(name)\n"
        "    local v = GetCVar(name)\n"
        "    if v == nil then return nil end\n"
        "    return v, GetCVarDefault and GetCVarDefault(name) or v, nil, nil\n"
        "end\n"
        "function RegisterCVar(name, value)\n"
        "    if GetCVar(name) == nil then SetCVar(name, value or '') end\n"
        "end\n"
        // A unit the player gets nothing for: grey to them, by the client's
        // grey-level rule (the same one GetQuestDifficultyColor draws with).
        "function UnitIsTrivial(unit)\n"
        "    if not UnitExists(unit) or UnitIsPlayer(unit) then return nil end\n"
        "    local pl, ul = UnitLevel('player'), UnitLevel(unit)\n"
        "    if not pl or not ul or ul < 0 then return nil end\n"
        "    local grey\n"
        "    if pl <= 5 then grey = 0\n"
        "    elseif pl <= 39 then grey = pl - 5 - math.floor(pl / 10)\n"
        "    elseif pl <= 59 then grey = pl - 1 - math.floor(pl / 5)\n"
        "    else grey = pl - 9 end\n"
        "    if ul <= grey then return 1 end\n"
        "end\n"
        // Every faction header at once - what the reputation panel's
        // expand/collapse-all does.
        "function ExpandAllFactionHeaders()\n"
        "    for i = GetNumFactions(), 1, -1 do\n"
        "        local _, _, _, _, _, _, _, _, isHeader, isCollapsed = GetFactionInfo(i)\n"
        "        if isHeader and isCollapsed then ExpandFactionHeader(i) end\n"
        "    end\n"
        "end\n"
        "function CollapseAllFactionHeaders()\n"
        "    for i = GetNumFactions(), 1, -1 do\n"
        "        local _, _, _, _, _, _, _, _, isHeader, isCollapsed = GetFactionInfo(i)\n"
        "        if isHeader and not isCollapsed then CollapseFactionHeader(i) end\n"
        "    end\n"
        "end\n");
}

}  // namespace wowee::addons
