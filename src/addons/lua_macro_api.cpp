// lua_macro_api.cpp - SecureCmdOptionParse, IsEquippedItemType, and the C
// functions Bindings.xml calls for movement, the camera and debugging.
//
// Stock 3.3.5a (Wow.exe 12340) addresses throughout; the findings are in
// docs/re/macros-bindings.md.
#include "addons/lua_macro_state.hpp"
#include "addons/lua_api_helpers.hpp"
#include "addons/lua_api_registrations.hpp"
#include "pipeline/asset_manager.hpp"
#include "rendering/camera_controller.hpp"
#include "ui/chat/macro_evaluator.hpp"
#include "ui/keybinding_manager.hpp"

#include <array>
#include <cctype>
#include <map>
#include <unordered_map>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace wowee::addons {

// ---- LuaMacroState: each question asked of the Lua API ----

void LuaMacroState::call(const char* fn, std::initializer_list<const char*> args) const {
    lua_getglobal(L_, fn);
    if (!lua_isfunction(L_, -1)) {
        lua_pop(L_, 1);
        lua_pushnil(L_);
        return;
    }
    for (const char* a : args) lua_pushstring(L_, a);
    if (lua_pcall(L_, static_cast<int>(args.size()), 1, 0) != 0) {
        lua_pop(L_, 1);
        lua_pushnil(L_);
    }
}

bool LuaMacroState::truthy(const char* fn, std::initializer_list<const char*> args) const {
    call(fn, args);
    const bool v = lua_toboolean(L_, -1) != 0;
    lua_pop(L_, 1);
    return v;
}

double LuaMacroState::number(const char* fn, std::initializer_list<const char*> args) const {
    call(fn, args);
    const double v = lua_isnumber(L_, -1) ? lua_tonumber(L_, -1) : 0.0;
    lua_pop(L_, 1);
    return v;
}

std::string LuaMacroState::text(const char* fn, std::initializer_list<const char*> args) const {
    call(fn, args);
    std::string v = lua_isstring(L_, -1) ? lua_tostring(L_, -1) : "";
    lua_pop(L_, 1);
    return v;
}

namespace {
/// No unit after target= / @ means the current target (0x60ABF0).
const char* unitOrTarget(const std::string& unit) { return unit.empty() ? "target" : unit.c_str(); }
} // namespace

bool LuaMacroState::unitExists(const std::string& unit) const { return truthy("UnitExists", {unitOrTarget(unit)}); }
bool LuaMacroState::unitCanAssist(const std::string& unit) const { return truthy("UnitCanAssist", {"player", unitOrTarget(unit)}); }
bool LuaMacroState::unitCanAttack(const std::string& unit) const { return truthy("UnitCanAttack", {"player", unitOrTarget(unit)}); }
bool LuaMacroState::unitInParty(const std::string& unit) const { return truthy("UnitInParty", {unitOrTarget(unit)}); }
bool LuaMacroState::unitInRaid(const std::string& unit) const { return truthy("UnitInRaid", {unitOrTarget(unit)}); }
bool LuaMacroState::unitIsDead(const std::string& unit) const { return truthy("UnitIsDeadOrGhost", {unitOrTarget(unit)}); }
bool LuaMacroState::unitHasVehicleUI(const std::string& unit) const { return truthy("UnitHasVehicleUI", {unitOrTarget(unit)}); }

// The player, or the pet, in combat (0x5EF710 reads both units' flags).
bool LuaMacroState::inCombat() const {
    return truthy("UnitAffectingCombat", {"player"}) || truthy("UnitAffectingCombat", {"pet"});
}
bool LuaMacroState::isFlyableArea() const { return truthy("IsFlyableArea", {}); }
bool LuaMacroState::isOutdoors() const { return truthy("IsOutdoors", {}); }
bool LuaMacroState::isSwimming() const { return truthy("IsSwimming", {}); }
bool LuaMacroState::isFlying() const { return truthy("IsFlying", {}); }
bool LuaMacroState::isMounted() const { return truthy("IsMounted", {}); }
bool LuaMacroState::isStealthed() const { return truthy("IsStealthed", {}); }
bool LuaMacroState::inParty() const { return number("GetNumPartyMembers", {}) > 0; }
bool LuaMacroState::inRaid() const { return number("GetNumRaidMembers", {}) > 0; }
bool LuaMacroState::playerHasVehicleUI() const { return truthy("UnitHasVehicleUI", {"player"}); }

int LuaMacroState::shapeshiftForm() const { return static_cast<int>(number("GetShapeshiftForm", {})); }
int LuaMacroState::actionBarPage() const { return static_cast<int>(number("GetActionBarPage", {})); }
int LuaMacroState::bonusBarOffset() const { return static_cast<int>(number("GetBonusBarOffset", {})); }
int LuaMacroState::activeTalentGroup() const { return static_cast<int>(number("GetActiveTalentGroup", {})); }

bool LuaMacroState::hasPet() const { return truthy("UnitExists", {"pet"}); }
std::string LuaMacroState::petName() const { return text("UnitName", {"pet"}); }
std::string LuaMacroState::petFamily() const { return text("UnitCreatureFamily", {"pet"}); }
bool LuaMacroState::isEquippedItemType(const std::string& type) const { return isEquippedItemTypeFor(L_, type); }
std::string LuaMacroState::channelingSpell() const { return text("UnitChannelInfo", {"player"}); }

std::string LuaMacroState::cursorType() const {
    // Only the kinds 0x5EF490 names; a pet action or a companion on the
    // cursor is no cursor to [cursor].
    std::string kind = text("GetCursorInfo", {});
    static constexpr const char* kKinds[] = {
        "item", "money", "spell", "merchant", "macro", "guildbankmoney", "equipmentset"};
    for (const char* k : kKinds) if (kind == k) return kind;
    return {};
}

std::string LuaMacroState::clickedButton() const { return text("GetMouseButtonClicked", {}); }

void LuaMacroState::reportUnknownOption(const std::string& name) const {
    // ERR_UNKNOWN_MACRO_OPTION_S as a system line (0x5216F0, message 0x23E).
    std::string format = "Unknown macro option: %s";
    lua_getglobal(L_, "ERR_UNKNOWN_MACRO_OPTION_S");
    if (lua_isstring(L_, -1)) format = lua_tostring(L_, -1);
    lua_pop(L_, 1);
    std::string line = format;
    if (const size_t at = line.find("%s"); at != std::string::npos) line.replace(at, 2, name);
    if (auto* gh = getGameHandler(L_)) gh->addSystemChatMessage(line);
}

bool LuaModifierState::isKeyDown(Key key) const {
    const char* fn = "IsLeftShiftKeyDown";
    switch (key) {
        case Key::LeftShift:  fn = "IsLeftShiftKeyDown"; break;
        case Key::RightShift: fn = "IsRightShiftKeyDown"; break;
        case Key::LeftCtrl:   fn = "IsLeftControlKeyDown"; break;
        case Key::RightCtrl:  fn = "IsRightControlKeyDown"; break;
        case Key::LeftAlt:    fn = "IsLeftAltKeyDown"; break;
        case Key::RightAlt:   fn = "IsRightAltKeyDown"; break;
    }
    lua_getglobal(L_, fn);
    if (!lua_isfunction(L_, -1)) { lua_pop(L_, 1); return false; }
    if (lua_pcall(L_, 0, 1, 0) != 0) { lua_pop(L_, 1); return false; }
    const bool down = lua_toboolean(L_, -1) != 0;
    lua_pop(L_, 1);
    return down;
}

bool LuaModifierState::isModifiedClick(const std::string& action) const {
    lua_getglobal(L_, "IsModifiedClick");
    if (!lua_isfunction(L_, -1)) { lua_pop(L_, 1); return false; }
    lua_pushstring(L_, action.c_str());
    if (lua_pcall(L_, 1, 1, 0) != 0) { lua_pop(L_, 1); return false; }
    const bool held = lua_toboolean(L_, -1) != 0;
    lua_pop(L_, 1);
    return held;
}

// ---- IsEquippedItemType ----

namespace {

/// The item class and subclass names a type can be, from the client's own
/// tables (0x517740): ItemClass.dbc's name, ItemSubClass.dbc's display and
/// verbose names, ItemSubClassMask.dbc's group names. Loaded once.
struct ItemTypeNames {
    bool loaded = false;
    std::unordered_map<uint32_t, std::string> className;
    std::map<std::pair<uint32_t, uint32_t>, std::array<std::string, 2>> subclassName;
    struct Mask { uint32_t itemClass; uint32_t mask; std::string name; };
    std::vector<Mask> masks;
};

ItemTypeNames& itemTypeNames(game::GameHandler* gh) {
    static ItemTypeNames names;
    if (names.loaded || !gh) return names;
    auto* am = gh->services().assetManager;
    if (!am || !am->isInitialized()) return names;
    names.loaded = true;
    if (auto dbc = am->loadDBC("ItemClass.dbc"); dbc && dbc->isLoaded() && dbc->getFieldCount() > 3) {
        for (uint32_t r = 0; r < dbc->getRecordCount(); ++r)
            names.className[dbc->getUInt32(r, 0)] = dbc->getString(r, 3);
    }
    if (auto dbc = am->loadDBC("ItemSubClass.dbc"); dbc && dbc->isLoaded() && dbc->getFieldCount() >= 28) {
        for (uint32_t r = 0; r < dbc->getRecordCount(); ++r)
            names.subclassName[{dbc->getUInt32(r, 0), dbc->getUInt32(r, 1)}] = {
                dbc->getString(r, 10), dbc->getString(r, 27)};
    }
    if (auto dbc = am->loadDBC("ItemSubClassMask.dbc"); dbc && dbc->isLoaded() && dbc->getFieldCount() >= 3) {
        for (uint32_t r = 0; r < dbc->getRecordCount(); ++r)
            names.masks.push_back({dbc->getUInt32(r, 0), dbc->getUInt32(r, 1), dbc->getString(r, 2)});
    }
    return names;
}

bool sameNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size() || a.empty()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

/// The INVTYPE_ tokens in InventoryType order (0xAC7FD8).
constexpr std::array<const char*, 29> kInvTypeTokens{
    "", "INVTYPE_HEAD", "INVTYPE_NECK", "INVTYPE_SHOULDER", "INVTYPE_BODY", "INVTYPE_CHEST",
    "INVTYPE_WAIST", "INVTYPE_LEGS", "INVTYPE_FEET", "INVTYPE_WRIST", "INVTYPE_HAND",
    "INVTYPE_FINGER", "INVTYPE_TRINKET", "INVTYPE_WEAPON", "INVTYPE_SHIELD", "INVTYPE_RANGED",
    "INVTYPE_CLOAK", "INVTYPE_2HWEAPON", "INVTYPE_BAG", "INVTYPE_TABARD", "INVTYPE_ROBE",
    "INVTYPE_WEAPONMAINHAND", "INVTYPE_WEAPONOFFHAND", "INVTYPE_HOLDABLE", "INVTYPE_AMMO",
    "INVTYPE_THROWN", "INVTYPE_RANGEDRIGHT", "INVTYPE_QUIVER", "INVTYPE_RELIC"};

} // namespace

bool isEquippedItemTypeFor(lua_State* L, const std::string& type) {
    auto* gh = getGameHandler(L);
    if (!gh || type.empty()) return false;
    // An inventory type by token or by its global string first, as 0x51C740
    // walks the table before it tries the class names.
    int invType = -1;
    for (size_t i = 1; i < kInvTypeTokens.size() && invType < 0; ++i) {
        if (sameNoCase(type, kInvTypeTokens[i])) { invType = static_cast<int>(i); break; }
        lua_getglobal(L, kInvTypeTokens[i]);
        if (lua_isstring(L, -1) && sameNoCase(type, lua_tostring(L, -1))) invType = static_cast<int>(i);
        lua_pop(L, 1);
    }
    const auto& names = itemTypeNames(gh);
    const auto& inv = gh->getInventory();
    for (int s = 0; s <= static_cast<int>(game::EquipSlot::TABARD); ++s) {
        const auto& slot = inv.getEquipSlot(static_cast<game::EquipSlot>(s));
        if (slot.empty()) continue;
        const auto* info = gh->getItemInfo(slot.item.itemId);
        if (invType > 0) {
            const uint32_t t = info ? info->inventoryType : slot.item.inventoryType;
            if (static_cast<int>(t) == invType) return true;
            continue;
        }
        if (!info) continue;
        if (auto it = names.className.find(info->itemClass);
            it != names.className.end() && sameNoCase(type, it->second)) return true;
        if (auto it = names.subclassName.find({info->itemClass, info->subClass});
            it != names.subclassName.end() &&
            (sameNoCase(type, it->second[0]) || sameNoCase(type, it->second[1]))) return true;
        for (const auto& m : names.masks)
            if (m.itemClass == info->itemClass && info->subClass < 32 &&
                (m.mask & (1u << info->subClass)) && sameNoCase(type, m.name)) return true;
    }
    return false;
}

namespace {

// ---- The Lua functions ----

// SecureCmdOptionParse(options) -> action, target (0x564AE0). Nothing at all
// when no clause matches; the target is nil when the bracket named none.
int lua_SecureCmdOptionParse(lua_State* L) {
    const char* options = lua_tostring(L, 1);
    if (!options) return luaL_error(L, "Usage: SecureCmdOptionParse(\"options\")");
    LuaMacroState state(L);
    LuaModifierState mods(L);
    const ui::MacroOptions r = ui::MacroEvaluator(state, mods).parse(options);
    if (!r.matched) return 0;
    lua_pushlstring(L, r.action.data(), r.action.size());
    if (r.hasTarget) lua_pushlstring(L, r.target.data(), r.target.size());
    else lua_pushnil(L);
    return 2;
}

// IsEquippedItemType(type) -> 1 or nil (0x51C740).
int lua_IsEquippedItemType(lua_State* L) {
    const char* type = lua_tostring(L, 1);
    if (!type || !*type) return luaL_error(L, "Usage: IsEquippedItemType(\"type\")");
    if (isEquippedItemTypeFor(L, type)) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}

// IsFlyableArea() -> 1 or nil (0x612260).
int lua_IsFlyableArea(lua_State* L) {
    auto* svc = getLuaServices(L);
    if (svc && svc->isFlyableArea && svc->isFlyableArea()) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}

rendering::CameraController* camera(lua_State* L) {
    auto* svc = getLuaServices(L);
    return svc && svc->getCameraController ? svc->getCameraController() : nullptr;
}

/// A binding command pressed once, for the client to act on the next frame
/// as it acts on the key (the autorun, walk and sheathe toggles).
void pressCommand(const char* command) {
    auto& kb = ui::KeybindingManager::getInstance();
    kb.setVirtualCommand(command, "lua", true);
    kb.setVirtualCommand(command, "lua", false);
}

// ToggleAutoRun (0x5FC190), ToggleRun (0x5FAAE0), ToggleSheath (0x51A6B0).
int lua_ToggleAutoRun(lua_State*) { pressCommand("TOGGLEAUTORUN"); return 0; }
int lua_ToggleRun(lua_State*) { pressCommand("TOGGLERUN"); return 0; }
int lua_ToggleSheath(lua_State*) { pressCommand("TOGGLESHEATH"); return 0; }

// PitchUp/Down Start/Stop (0x5FC8E0 / 0x5FC570, 0x5FC920 / 0x5FC5C0): the
// control bits 0x400 / 0x800 held, which pitch a swimmer or a flyer.
int lua_PitchUpStart(lua_State*) { ui::KeybindingManager::getInstance().setVirtualCommand("PITCHUP", "lua", true); return 0; }
int lua_PitchUpStop(lua_State*) { ui::KeybindingManager::getInstance().setVirtualCommand("PITCHUP", "lua", false); return 0; }
int lua_PitchDownStart(lua_State*) { ui::KeybindingManager::getInstance().setVirtualCommand("PITCHDOWN", "lua", true); return 0; }
int lua_PitchDownStop(lua_State*) { ui::KeybindingManager::getInstance().setVirtualCommand("PITCHDOWN", "lua", false); return 0; }

// TurnOrActionStart/Stop (0x5FC610 / 0x5FC680): the right button's control
// bit 0x1; CameraOrSelectOrMoveStart/Stop (0x5FC6C0 / 0x5FC730): the left
// button's 0x2; MoveAndSteerStart/Stop (0x5FC780 / 0x5FC830): both.
int lua_TurnOrActionStart(lua_State* L) { if (auto* c = camera(L)) c->setScriptedMouseButton(true, true); return 0; }
int lua_TurnOrActionStop(lua_State* L) { if (auto* c = camera(L)) c->setScriptedMouseButton(true, false); return 0; }
int lua_CameraOrSelectOrMoveStart(lua_State* L) { if (auto* c = camera(L)) c->setScriptedMouseButton(false, true); return 0; }
int lua_CameraOrSelectOrMoveStop(lua_State* L) { if (auto* c = camera(L)) c->setScriptedMouseButton(false, false); return 0; }
int lua_MoveAndSteerStart(lua_State* L) {
    if (auto* c = camera(L)) { c->setScriptedMouseButton(false, true); c->setScriptedMouseButton(true, true); }
    return 0;
}
int lua_MoveAndSteerStop(lua_State* L) {
    if (auto* c = camera(L)) { c->setScriptedMouseButton(false, false); c->setScriptedMouseButton(true, false); }
    return 0;
}

// CameraZoomIn/Out(yards = 1) (0x6017E0 / 0x601840), and the vehicle pair
// (0x6018A0 / 0x6018B0), which are the same calls.
int lua_CameraZoomIn(lua_State* L) {
    const float yards = static_cast<float>(luaL_optnumber(L, 1, 1.0));
    if (auto* c = camera(L)) c->processMouseWheel(yards);
    return 0;
}
int lua_CameraZoomOut(lua_State* L) {
    const float yards = static_cast<float>(luaL_optnumber(L, 1, 1.0));
    if (auto* c = camera(L)) c->processMouseWheel(-yards);
    return 0;
}

// MoveView*Start(speed = 1) / Stop (0x5FF080 .. 0x5FF230 -> 0x5FF000).
template <rendering::CameraController::ViewMove M>
int lua_MoveViewStart(lua_State* L) {
    const float speed = static_cast<float>(luaL_optnumber(L, 1, 1.0));
    if (auto* c = camera(L)) c->setViewMove(M, true, speed);
    return 0;
}
template <rendering::CameraController::ViewMove M>
int lua_MoveViewStop(lua_State* L) {
    if (auto* c = camera(L)) c->setViewMove(M, false);
    return 0;
}

/// The view index argument, as the stock functions read it: a usage error
/// without one, nothing done outside 1..5.
int viewArg(lua_State* L, const char* usage) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "%s", usage);
    const int index = static_cast<int>(lua_tonumber(L, 1));
    return index >= 1 && index <= 5 ? index : 0;
}

// SetView(n) (0x6039B0), SaveView(n) (0x5FF260), ResetView(n) (0x604C80).
int lua_SetView(lua_State* L) {
    const int index = viewArg(L, "Usage: SetView(viewModeIndex)");
    if (index > 0) if (auto* c = camera(L)) c->setView(index);
    return 0;
}
int lua_SaveView(lua_State* L) {
    const int index = viewArg(L, "Usage: SaveView(viewModeIndex)");
    if (index > 0) if (auto* c = camera(L)) c->saveView(index);
    return 0;
}
int lua_ResetView(lua_State* L) {
    const int index = viewArg(L, "Usage: ResetView(viewModeIndex)");
    if (index > 0) if (auto* c = camera(L)) c->resetView(index);
    return 0;
}
// NextView (0x604CE0) up to 5, PrevView (0x604D10) down to 1.
int lua_NextView(lua_State* L) {
    if (auto* c = camera(L); c && c->currentView() < 5) c->setView(c->currentView() + 1);
    return 0;
}
int lua_PrevView(lua_State* L) {
    if (auto* c = camera(L); c && c->currentView() > 1) c->setView(c->currentView() - 1);
    return 0;
}
// FlipCameraYaw(degrees) (0x5FF2C0).
int lua_FlipCameraYaw(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: FlipCameraYaw(degrees)");
    if (auto* c = camera(L)) c->flipYaw(static_cast<float>(lua_tonumber(L, 1)));
    return 0;
}

// The debug bindings' functions are one shared `return 0` in the shipped
// client (0x8E5250): bound, and doing nothing.
int lua_DebugNoOp(lua_State*) { return 0; }
int lua_EmptyString(lua_State* L) { lua_pushstring(L, ""); return 1; }

} // namespace

void registerMacroBindingLuaAPI(lua_State* L) {
    using VM = rendering::CameraController::ViewMove;
    static const luaL_Reg api[] = {
        {"SecureCmdOptionParse",      lua_SecureCmdOptionParse},
        {"IsEquippedItemType",        lua_IsEquippedItemType},
        {"IsFlyableArea",             lua_IsFlyableArea},
        {"ToggleAutoRun",             lua_ToggleAutoRun},
        {"ToggleRun",                 lua_ToggleRun},
        {"ToggleSheath",              lua_ToggleSheath},
        {"PitchUpStart",              lua_PitchUpStart},
        {"PitchUpStop",               lua_PitchUpStop},
        {"PitchDownStart",            lua_PitchDownStart},
        {"PitchDownStop",             lua_PitchDownStop},
        {"TurnOrActionStart",         lua_TurnOrActionStart},
        {"TurnOrActionStop",          lua_TurnOrActionStop},
        {"CameraOrSelectOrMoveStart", lua_CameraOrSelectOrMoveStart},
        {"CameraOrSelectOrMoveStop",  lua_CameraOrSelectOrMoveStop},
        {"MoveAndSteerStart",         lua_MoveAndSteerStart},
        {"MoveAndSteerStop",          lua_MoveAndSteerStop},
        {"CameraZoomIn",              lua_CameraZoomIn},
        {"CameraZoomOut",             lua_CameraZoomOut},
        {"VehicleCameraZoomIn",       lua_CameraZoomIn},
        {"VehicleCameraZoomOut",      lua_CameraZoomOut},
        {"MoveViewInStart",           lua_MoveViewStart<VM::In>},
        {"MoveViewInStop",            lua_MoveViewStop<VM::In>},
        {"MoveViewOutStart",          lua_MoveViewStart<VM::Out>},
        {"MoveViewOutStop",           lua_MoveViewStop<VM::Out>},
        {"MoveViewLeftStart",         lua_MoveViewStart<VM::Left>},
        {"MoveViewLeftStop",          lua_MoveViewStop<VM::Left>},
        {"MoveViewRightStart",        lua_MoveViewStart<VM::Right>},
        {"MoveViewRightStop",         lua_MoveViewStop<VM::Right>},
        {"MoveViewUpStart",           lua_MoveViewStart<VM::Up>},
        {"MoveViewUpStop",            lua_MoveViewStop<VM::Up>},
        {"MoveViewDownStart",         lua_MoveViewStart<VM::Down>},
        {"MoveViewDownStop",          lua_MoveViewStop<VM::Down>},
        {"SetView",                   lua_SetView},
        {"SaveView",                  lua_SaveView},
        {"ResetView",                 lua_ResetView},
        {"NextView",                  lua_NextView},
        {"PrevView",                  lua_PrevView},
        {"FlipCameraYaw",             lua_FlipCameraYaw},
        {"TogglePerformanceDisplay",  lua_DebugNoOp},
        {"TogglePerformancePause",    lua_DebugNoOp},
        {"TogglePerformanceValues",   lua_DebugNoOp},
        {"ResetPerformanceValues",    lua_DebugNoOp},
        {"ToggleCollision",           lua_DebugNoOp},
        {"ToggleCollisionDisplay",    lua_DebugNoOp},
        {"TogglePlayerBounds",        lua_DebugNoOp},
        {"TogglePortals",             lua_DebugNoOp},
        {"ToggleTris",                lua_DebugNoOp},
        {"TeleportToDebugObject",     lua_DebugNoOp},
        // Stock debugging hooks FrameXML calls from basiccontrols.xml (the
        // ScriptErrors frame) and Blizzard_DebugTools: debuginfo is the same
        // shared no-op in the shipped client; debuglocals answers a string.
        {"debuginfo",                 lua_DebugNoOp},
        {"FrameXML_Debug",            lua_DebugNoOp},
        {"debuglocals",               lua_EmptyString},
        // Interfaces FrameXML still draws for features this client does not
        // have: voice chat's loopback test (audiooptionspanels.xml), the
        // tic-tac-toe minigame (minigameframe.lua), the GM survey, Real ID
        // invites, the pre-2.0 skill-point buttons (skillframe.xml), the
        // in-game movie and cinematic players. Inert, so the buttons that
        // reach them do nothing rather than raise.
        {"VoiceChat_PlayLoopbackSound",   lua_DebugNoOp},
        {"VoiceChat_RecordLoopbackSound", lua_DebugNoOp},
        {"SetActiveVoiceChannel",     lua_DebugNoOp},
        {"GetVoiceSessionMemberInfoBySessionID", lua_DebugNoOp},
        {"GetMinigameState",          lua_DebugNoOp},
        {"GetMinigameType",           lua_DebugNoOp},
        {"MakeMinigameMove",          lua_DebugNoOp},
        {"GMSurveyAnswerSubmit",      lua_DebugNoOp},
        {"GMSurveyCommentSubmit",     lua_DebugNoOp},
        {"GMSurveySubmit",            lua_DebugNoOp},
        {"BNAcceptFriendInvite",      lua_DebugNoOp},
        {"AcceptSkillUps",            lua_DebugNoOp},
        {"AddSkillUp",                lua_DebugNoOp},
        {"RemoveSkillUp",             lua_DebugNoOp},
        {"BuySkillTier",              lua_DebugNoOp},
        {"StopCinematic",             lua_DebugNoOp},
        {"GameMovieFinished",         lua_DebugNoOp},
    };
    for (const auto& f : api) {
        lua_pushcfunction(L, f.func);
        lua_setglobal(L, f.name);
    }
}

} // namespace wowee::addons
