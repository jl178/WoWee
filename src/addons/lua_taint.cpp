#include "addons/lua_taint.hpp"
#include "addons/lua_engine.hpp"
#include "addons/lua_api_helpers.hpp"
#include "core/logger.hpp"
#include "ui/widget_tree.hpp"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace wowee::addons::taint {

namespace {

std::vector<std::string>& names() {
    static std::vector<std::string> n{std::string()};  // 0 is secure
    return n;
}

bool gLockdown = false;
int gHardware = 0;
int gConstructing = 0;

LuaEngine* engineOf(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_lua_engine");
    auto* e = static_cast<LuaEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return e;
}

uint32_t widgetIdAt(lua_State* L, int index) {
    if (!lua_istable(L, index)) return 0;
    lua_getfield(L, index, "__wid");
    auto id = static_cast<uint32_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    if (id == 0) {
        // A secure handle's surrogate carries only [0], the frame's userdata.
        lua_rawgeti(L, index, 0);
        if (lua_type(L, -1) == LUA_TLIGHTUSERDATA)
            id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lua_touserdata(L, -1)));
        lua_pop(L, 1);
    }
    return id;
}

/// The interface is told, as the client tells it (0x00513530): the taint to
/// blame and what was refused, "CastSpellByName()" or "ElvUF_Parent:Hide()".
void report(lua_State* L, bool forbidden, const std::string& what) {
    const int t = lua_gettaint(L);
    const char* who = nameOf(t);
    const char* event = forbidden ? "ADDON_ACTION_FORBIDDEN" : "ADDON_ACTION_BLOCKED";
    LOG_INFO("[taint] ", event, " ", who, " ", what);
    static int depth = 0;
    if (depth > 0) return;  // a handler of the event doing the same again
    if (!engineOf(L)) return;
    ++depth;
    // Told by the client, so its handlers run as the client calls handlers:
    // each with its own taint, from a C function of the client's. The guard
    // that found the fault is transparent to taint, so the event goes out
    // from a function that is not.
    const int saved = lua_gettaint(L);
    lua_pushcfunction(L, [](lua_State* S) -> int {
        if (auto* e = engineOf(S)) {
            e->dispatchEvent(lua_tostring(S, 1), {lua_tostring(S, 2), lua_tostring(S, 3)});
        }
        return 0;
    });
    lua_pushstring(L, event);
    lua_pushstring(L, who);
    lua_pushstring(L, what.c_str());
    if (lua_pcall(L, 3, 0, 0) != 0) lua_pop(L, 1);
    lua_settaint(L, saved);
    --depth;
}

enum class Rule : int {
    Forbidden = 0,  // refused to tainted code always
    Combat = 1,     // refused to tainted code in combat lockdown
    Hardware = 2,   // refused to tainted code without a key or click behind it
};

/// The protected functions, by the check each reaches in the client.
///
/// 0x005191C0 takes a category and decides by it: movement, spell casting
/// and targeting, macros and party assignments are refused to tainted code
/// outright; trade, auction, dungeon finder and calendar actions only
/// without a hardware event; raid groups, macro editing, bindings, the
/// action bars and picking things up only in combat. Targeting, attacking,
/// focus and a handful more test the execution's taint themselves (the
/// functions around 0x0051FF20 and 0x00524FC0) and are refused outright.
struct Protected { const char* name; Rule rule; };
const Protected kProtected[] = {
    // movement and the camera (category 0)
    {"MoveForwardStart", Rule::Forbidden}, {"MoveForwardStop", Rule::Forbidden},
    {"MoveBackwardStart", Rule::Forbidden}, {"MoveBackwardStop", Rule::Forbidden},
    {"TurnLeftStart", Rule::Forbidden}, {"TurnLeftStop", Rule::Forbidden},
    {"TurnRightStart", Rule::Forbidden}, {"TurnRightStop", Rule::Forbidden},
    {"StrafeLeftStart", Rule::Forbidden}, {"StrafeLeftStop", Rule::Forbidden},
    {"StrafeRightStart", Rule::Forbidden}, {"StrafeRightStop", Rule::Forbidden},
    {"PitchUpStart", Rule::Forbidden}, {"PitchUpStop", Rule::Forbidden},
    {"PitchDownStart", Rule::Forbidden}, {"PitchDownStop", Rule::Forbidden},
    {"JumpOrAscendStart", Rule::Forbidden}, {"AscendStop", Rule::Forbidden},
    {"DescendStop", Rule::Forbidden}, {"SitStandOrDescendStart", Rule::Forbidden},
    {"ToggleRun", Rule::Forbidden}, {"ToggleAutoRun", Rule::Forbidden},
    {"TurnOrActionStart", Rule::Forbidden}, {"TurnOrActionStop", Rule::Forbidden},
    {"CameraOrSelectOrMoveStart", Rule::Forbidden},
    {"CameraOrSelectOrMoveStop", Rule::Forbidden},
    {"MoveAndSteerStart", Rule::Forbidden}, {"MoveAndSteerStop", Rule::Forbidden},
    // spells, items and the pet (category 2)
    {"CastSpell", Rule::Forbidden}, {"CastSpellByName", Rule::Forbidden},
    {"CastSpellByID", Rule::Forbidden}, {"CastShapeshiftForm", Rule::Forbidden},
    {"CancelShapeshiftForm", Rule::Forbidden}, {"CastPetAction", Rule::Forbidden},
    {"TogglePetAutocast", Rule::Forbidden}, {"SpellTargetItem", Rule::Forbidden},
    {"SpellTargetUnit", Rule::Forbidden}, {"SpellStopTargeting", Rule::Forbidden},
    {"SpellStopCasting", Rule::Forbidden}, {"UseAction", Rule::Forbidden},
    {"UseContainerItem", Rule::Forbidden}, {"UseInventoryItem", Rule::Forbidden},
    {"UseItemByName", Rule::Forbidden}, {"UseQuestLogSpecialItem", Rule::Forbidden},
    {"CancelUnitBuff", Rule::Forbidden}, {"DestroyTotem", Rule::Forbidden},
    {"PetAttack", Rule::Forbidden}, {"PetFollow", Rule::Forbidden},
    {"PetWait", Rule::Forbidden}, {"PetPassiveMode", Rule::Forbidden},
    {"PetDefensiveMode", Rule::Forbidden}, {"PetAggressiveMode", Rule::Forbidden},
    {"PetStopAttack", Rule::Forbidden},
    // attacking, targeting and focus (their own taint test)
    {"AttackTarget", Rule::Forbidden}, {"StartAttack", Rule::Forbidden},
    {"StopAttack", Rule::Forbidden}, {"TargetUnit", Rule::Forbidden},
    {"AssistUnit", Rule::Forbidden}, {"FocusUnit", Rule::Forbidden},
    {"ClearFocus", Rule::Forbidden}, {"ClearTarget", Rule::Forbidden},
    {"TargetNearest", Rule::Forbidden}, {"TargetNearestEnemy", Rule::Forbidden},
    {"TargetNearestEnemyPlayer", Rule::Forbidden}, {"TargetNearestFriend", Rule::Forbidden},
    {"TargetNearestFriendPlayer", Rule::Forbidden},
    {"TargetNearestPartyMember", Rule::Forbidden},
    {"TargetNearestRaidMember", Rule::Forbidden}, {"TargetLastTarget", Rule::Forbidden},
    {"TargetLastEnemy", Rule::Forbidden}, {"TargetLastFriend", Rule::Forbidden},
    {"TargetDirectionEnemy", Rule::Forbidden}, {"TargetDirectionFriend", Rule::Forbidden},
    {"TargetTotem", Rule::Forbidden}, {"InteractUnit", Rule::Forbidden},
    // macros (category 3) and party assignments (category 4)
    {"RunMacro", Rule::Forbidden}, {"RunMacroText", Rule::Forbidden},
    {"StopMacro", Rule::Forbidden}, {"SetPartyAssignment", Rule::Forbidden},
    {"ClearPartyAssignment", Rule::Forbidden},
    // the rest that test the execution's taint themselves
    {"RunBinding", Rule::Forbidden}, {"Logout", Rule::Forbidden},
    {"Quit", Rule::Forbidden}, {"ForceQuit", Rule::Forbidden},
    {"RegisterForSave", Rule::Forbidden}, {"RegisterForSavePerCharacter", Rule::Forbidden},
    // GM tickets (category 17)
    {"NewGMTicket", Rule::Forbidden}, {"UpdateGMTicket", Rule::Forbidden},
    {"GMResponseNeedMoreHelp", Rule::Forbidden},
    // trade (7), auction (8), the dungeon finder (9), the calendar (19)
    {"AcceptTrade", Rule::Hardware}, {"CancelTradeAccept", Rule::Hardware},
    {"PlaceAuctionBid", Rule::Hardware}, {"CancelAuction", Rule::Hardware},
    {"AcceptProposal", Rule::Hardware}, {"SearchLFGJoin", Rule::Hardware},
    {"SearchLFGLeave", Rule::Hardware}, {"CalendarAddEvent", Rule::Hardware},
    {"CalendarUpdateEvent", Rule::Hardware},
    // raid groups (11), macro editing (12), bindings (13), the action bars and
    // the cursor (14), equipping (22): only in combat
    {"SetRaidSubgroup", Rule::Combat}, {"SwapRaidSubgroup", Rule::Combat},
    {"CreateMacro", Rule::Combat}, {"EditMacro", Rule::Combat},
    {"DeleteMacro", Rule::Combat}, {"SetBinding", Rule::Combat},
    {"SetBindingSpell", Rule::Combat}, {"SetBindingClick", Rule::Combat},
    {"SetBindingItem", Rule::Combat}, {"SetBindingMacro", Rule::Combat},
    {"SetOverrideBinding", Rule::Combat}, {"SetOverrideBindingSpell", Rule::Combat},
    {"SetOverrideBindingClick", Rule::Combat}, {"SetOverrideBindingItem", Rule::Combat},
    {"SetOverrideBindingMacro", Rule::Combat}, {"ClearOverrideBindings", Rule::Combat},
    {"SetMouselookOverrideBinding", Rule::Combat}, {"PickupSpell", Rule::Combat},
    {"PickupCompanion", Rule::Combat}, {"PickupAction", Rule::Combat},
    {"PlaceAction", Rule::Combat}, {"ChangeActionBarPage", Rule::Combat},
    {"SetMultiCastSpell", Rule::Combat}, {"PickupPetAction", Rule::Combat},
    {"PickupEquipmentSetByName", Rule::Combat}, {"PickupMacro", Rule::Combat},
    {"EquipCursorItem", Rule::Combat}, {"EquipItemByName", Rule::Combat},
    {"PutItemInBag", Rule::Combat}, {"AutoEquipCursorItem", Rule::Combat},
    {"PickupInventoryItem", Rule::Combat}, {"PickupBagFromSlot", Rule::Combat},
};

/// The frame methods that test CanChangeProtectedState (0x00488540) before
/// doing anything: the region's geometry, and the frame's visibility,
/// layering, attributes, mouse and keyboard. A button's Enable and Disable
/// and a scroll frame's offsets as well.
const char* const kProtectedMethods[] = {
    "SetParent", "SetWidth", "SetHeight", "SetSize", "SetPoint", "SetAllPoints",
    "ClearAllPoints", "SetFrameStrata", "SetFrameLevel", "SetAttribute", "SetScale",
    "SetID", "SetToplevel", "Show", "Hide", "Raise", "Lower", "SetHitRectInsets",
    "EnableKeyboard", "EnableMouse", "EnableMouseWheel", "EnableJoystick",
    "SetHorizontalScroll", "SetVerticalScroll", "Enable", "Disable",
};

/// The guard in front of a protected function: upvalue 1 the function, 2
/// its name, 3 its rule.
int guardedFunction(lua_State* L) {
    const int t = lua_gettaint(L);
    if (t > 0) {
        const auto rule = static_cast<Rule>(lua_tointeger(L, lua_upvalueindex(3)));
        const bool refuse = rule == Rule::Forbidden ||
                            (rule == Rule::Combat && gLockdown) ||
                            (rule == Rule::Hardware && gHardware == 0);
        if (refuse) {
            std::string what = lua_tostring(L, lua_upvalueindex(2));
            what += "()";
            report(L, rule == Rule::Forbidden, what);
            return 0;
        }
    }
    const int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, n, LUA_MULTRET);
    return lua_gettop(L);
}

std::string frameLabel(lua_State* L, int index) {
    std::string label = "<unnamed>";
    if (auto* tree = getWidgetTree(L)) {
        if (const auto* w = tree->get(widgetIdAt(L, index))) {
            if (!w->name.empty()) label = w->name;
        }
    }
    return label;
}

/// The guard in front of a protected frame method: upvalue 1 the method, 2
/// its name.
int guardedMethod(lua_State* L) {
    if (gLockdown && gConstructing == 0 && lua_gettaint(L) > 0 &&
        !canChangeProtectedState(L, 1)) {
        std::string what = frameLabel(L, 1) + ":" + lua_tostring(L, lua_upvalueindex(2)) + "()";
        report(L, false, what);
        return 0;
    }
    const int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, n, LUA_MULTRET);
    return lua_gettop(L);
}

/// Push a C closure that is transparent to taint, as the standard library is.
void pushTransparent(lua_State* L, lua_CFunction fn, int nup) {
    lua_setmarkstdlib(L, 1);
    lua_pushcclosure(L, fn, nup);
    lua_setmarkstdlib(L, 0);
}

int lua_issecure(lua_State* L) {
    lua_pushboolean(L, lua_gettaint(L) == 0);
    return 1;
}

/// issecurevariable([table,] name): isSecure, taint. The taint of the slot,
/// not of the running code.
int lua_issecurevariable(lua_State* L) {
    int tableIndex = LUA_GLOBALSINDEX;
    int keyIndex = 1;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 1)) {
        luaL_checktype(L, 1, LUA_TTABLE);
        tableIndex = 1;
        keyIndex = 2;
    }
    if (lua_isnoneornil(L, keyIndex)) luaL_error(L, "Usage: issecurevariable([table,] \"variable\")");
    lua_pushvalue(L, keyIndex);
    const int t = lua_rawgettaint(L, tableIndex);
    if (t > 0) {
        lua_pushnil(L);
        lua_pushstring(L, nameOf(t));
        return 2;
    }
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

int callErrorHandler(lua_State* L) {
    // The message is on top. geterrorhandler()(message), as the client does
    // for an error raised inside securecall.
    lua_getglobal(L, "geterrorhandler");
    if (lua_isfunction(L, -1) && lua_pcall(L, 0, 1, 0) == 0 && lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);
        if (lua_pcall(L, 1, 0, 0) != 0) lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
    }
    return 0;
}

/// securecall(fn or name, ...): the call made with the caller's taint, and
/// the caller's taint as it was afterwards, whatever the call did. An error
/// goes to the error handler rather than out through the caller.
int lua_securecall(lua_State* L) {
    const int saved = lua_gettaint(L);
    if (lua_type(L, 1) == LUA_TSTRING) {
        lua_pushvalue(L, 1);
        lua_gettable(L, LUA_GLOBALSINDEX);
        lua_replace(L, 1);
    }
    if (!lua_isfunction(L, 1)) {
        lua_settaint(L, saved);
        return luaL_error(L, "Usage: securecall(function, ...)");
    }
    const int n = lua_gettop(L) - 1;
    const int rc = lua_pcall(L, n, LUA_MULTRET, 0);
    lua_settaint(L, saved);
    if (rc != 0) {
        callErrorHandler(L);
        lua_settaint(L, saved);
        return 0;
    }
    return lua_gettop(L);
}

/// The function hooksecurefunc puts in place: upvalue 1 the original, 2 the
/// hook. The original runs as it would have; the hook runs after it with the
/// same arguments, and whatever taint the hook carries ends with it.
int securehookWrapper(lua_State* L) {
    const int n = lua_gettop(L);
    // original(...)
    lua_pushvalue(L, lua_upvalueindex(1));
    for (int i = 1; i <= n; ++i) lua_pushvalue(L, i);
    lua_call(L, n, LUA_MULTRET);
    const int nres = lua_gettop(L) - n;
    // hook(...)
    const int saved = lua_gettaint(L);
    lua_pushvalue(L, lua_upvalueindex(2));
    for (int i = 1; i <= n; ++i) lua_pushvalue(L, i);
    if (lua_pcall(L, n, 0, 0) != 0) {
        lua_settaint(L, saved);
        callErrorHandler(L);
        lua_pop(L, 1);
    }
    lua_settaint(L, saved);
    return nres;
}

/// hooksecurefunc([table,] "name", hook). The slot keeps the taint it had:
/// putting a hook on a secure function leaves it secure.
int lua_hooksecurefunc(lua_State* L) {
    int tableIndex = LUA_GLOBALSINDEX;
    int nameIndex = 1;
    if (lua_type(L, 1) == LUA_TTABLE) {
        tableIndex = 1;
        nameIndex = 2;
    }
    const int hookIndex = nameIndex + 1;
    if (lua_type(L, nameIndex) != LUA_TSTRING || !lua_isfunction(L, hookIndex))
        return luaL_error(L, "Usage: hooksecurefunc([table,] \"functionName\", hookfunc)");
    // Through the metatable: a widget's methods live there, and the hook is
    // put on the widget itself (hooksecurefunc(GameTooltip, "SetBagItem", f)).
    lua_pushvalue(L, nameIndex);
    lua_gettable(L, tableIndex);
    if (!lua_isfunction(L, -1)) {
        return luaL_error(L, "hooksecurefunc(): %s is not a function",
                          lua_tostring(L, nameIndex));
    }
    lua_pushvalue(L, nameIndex);
    const int slotTaint = lua_rawgettaint(L, tableIndex);
    lua_pushvalue(L, hookIndex);
    pushTransparent(L, securehookWrapper, 2);
    lua_pushvalue(L, nameIndex);
    lua_insert(L, -2);
    lua_rawset(L, tableIndex);
    lua_pushvalue(L, nameIndex);
    lua_rawsettaint(L, tableIndex, slotTaint);
    return 0;
}

/// forceinsecure(): the rest of this execution is tainted.
int lua_forceinsecure(lua_State* L) {
    if (lua_gettaint(L) == 0) lua_settaint(L, idFor("*** TaintForced ***"));
    return 0;
}

int lua_InCombatLockdown(lua_State* L) {
    if (gLockdown) lua_pushboolean(L, 1); else lua_pushnil(L);
    return 1;
}

int lua_Frame_IsProtected(lua_State* L) {
    auto* tree = getWidgetTree(L);
    bool isExplicit = false;
    const bool prot = tree && tree->isProtected(widgetIdAt(L, 1), &isExplicit);
    if (prot) lua_pushboolean(L, 1); else lua_pushnil(L);
    if (isExplicit) lua_pushboolean(L, 1); else lua_pushnil(L);
    return 2;
}

int lua_Frame_CanChangeProtectedState(lua_State* L) {
    lua_pushboolean(L, canChangeProtectedState(L, 1));
    return 1;
}

int lua_WoweeSetProtected(lua_State* L) {
    if (auto* tree = getWidgetTree(L)) tree->setProtectedExplicit(widgetIdAt(L, 1), true);
    return 0;
}

}  // namespace

int idFor(const std::string& name) {
    if (name.empty()) return 0;
    static std::unordered_map<std::string, int> ids;
    auto it = ids.find(name);
    if (it != ids.end()) return it->second;
    names().push_back(name);
    const int id = static_cast<int>(names().size()) - 1;
    ids.emplace(name, id);
    return id;
}

int idForAddon(const std::string& addonName) {
    // The client's own load-on-demand parts are as secure as FrameXML.
    if (addonName.rfind("Blizzard_", 0) == 0) return 0;
    return idFor(addonName);
}

const char* nameOf(int id) {
    if (id <= 0 || id >= static_cast<int>(names().size())) return "";
    return names()[id].c_str();
}

void setCombatLockdown(bool on) { gLockdown = on; }

HardwareEventScope::HardwareEventScope() { ++gHardware; }
HardwareEventScope::~HardwareEventScope() { --gHardware; }

bool constructing() { return gConstructing > 0; }
ConstructionScope::ConstructionScope() { ++gConstructing; }
ConstructionScope::~ConstructionScope() { --gConstructing; }

SourceLoadScope::SourceLoadScope(lua_State* state, int taint) : L(state) {
    savedTaint = lua_gettaint(L);
    savedMode = lua_getloadsystem(L);
    lua_setloadsystem(L, LUA_LOAD_SOURCE);
    lua_settaint(L, taint);
}

SourceLoadScope::~SourceLoadScope() {
    lua_setloadsystem(L, savedMode);
    lua_settaint(L, savedTaint);
}

bool canChangeProtectedState(lua_State* L, int frameIndex) {
    if (!gLockdown || lua_gettaint(L) == 0) return true;
    auto* tree = getWidgetTree(L);
    if (!tree) return true;
    return !tree->isProtected(widgetIdAt(L, frameIndex));
}

void registerAPI(lua_State* L) {
    lua_pushcfunction(L, lua_issecure);
    lua_setglobal(L, "issecure");
    lua_pushcfunction(L, lua_issecurevariable);
    lua_setglobal(L, "issecurevariable");
    pushTransparent(L, lua_securecall, 0);
    lua_setglobal(L, "securecall");
    lua_pushcfunction(L, lua_hooksecurefunc);
    lua_setglobal(L, "hooksecurefunc");
    lua_pushcfunction(L, lua_forceinsecure);
    lua_setglobal(L, "forceinsecure");
    lua_pushcfunction(L, lua_InCombatLockdown);
    lua_setglobal(L, "InCombatLockdown");
    lua_pushcfunction(L, lua_WoweeSetProtected);
    lua_setglobal(L, "__WoweeSetProtected");

    lua_getglobal(L, "__WoweeFrameMT");
    if (lua_istable(L, -1)) {
        lua_pushcfunction(L, lua_Frame_IsProtected);
        lua_setfield(L, -2, "IsProtected");
        lua_pushcfunction(L, lua_Frame_CanChangeProtectedState);
        lua_setfield(L, -2, "CanChangeProtectedState");
        lua_pushcfunction(L, lua_Frame_CanChangeProtectedState);
        lua_setfield(L, -2, "CanChangeAttribute");
    }
    lua_pop(L, 1);
}

void protectFunctions(lua_State* L) {
    // Once per Lua state; a second pass would put a guard in front of a guard.
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_protected_done");
    const bool already = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    if (already) return;
    lua_pushboolean(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, "wowee_protected_done");

    int guarded = 0;
    for (const Protected& p : kProtected) {
        lua_getglobal(L, p.name);
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); continue; }
        lua_pushstring(L, p.name);
        lua_pushinteger(L, static_cast<int>(p.rule));
        pushTransparent(L, guardedFunction, 3);
        lua_setglobal(L, p.name);
        ++guarded;
    }
    int methods = 0;
    lua_getglobal(L, "__WoweeFrameMT");
    if (lua_istable(L, -1)) {
        for (const char* m : kProtectedMethods) {
            lua_pushstring(L, m);
            lua_rawget(L, -2);
            if (!lua_isfunction(L, -1)) { lua_pop(L, 1); continue; }
            lua_pushstring(L, m);
            pushTransparent(L, guardedMethod, 2);
            lua_pushstring(L, m);
            lua_insert(L, -2);
            lua_rawset(L, -3);
            ++methods;
        }
    }
    lua_pop(L, 1);
    LOG_INFO("[taint] protected ", guarded, " functions and ", methods, " frame methods");
}

}  // namespace wowee::addons::taint
