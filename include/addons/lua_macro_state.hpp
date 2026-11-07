// lua_macro_state.hpp - the macro option conditions, answered by the Lua API.
//
// SecureCmdOptionParse runs inside the interface, and what a condition says
// has to agree with what the interface itself sees: [stance:2] with
// GetShapeshiftForm, [bar:2] with GetActionBarPage, [cursor] with
// GetCursorInfo, [@party1,help] with UnitCanAssist. So the answers come from
// those functions, called on the same state, rather than from a second copy
// of their logic. The chat commands use this too whenever an interface is
// loaded (ui::evaluateMacroOptions).
#pragma once

#include "ui/chat/i_game_state.hpp"
#include "ui/chat/i_modifier_state.hpp"

#include <initializer_list>
#include <string>

struct lua_State;

namespace wowee::addons {

class LuaMacroState : public ui::IGameState {
public:
    explicit LuaMacroState(lua_State* L) : L_(L) {}

    [[nodiscard]] bool unitExists(const std::string& unit) const override;
    [[nodiscard]] bool unitCanAssist(const std::string& unit) const override;
    [[nodiscard]] bool unitCanAttack(const std::string& unit) const override;
    [[nodiscard]] bool unitInParty(const std::string& unit) const override;
    [[nodiscard]] bool unitInRaid(const std::string& unit) const override;
    [[nodiscard]] bool unitIsDead(const std::string& unit) const override;
    [[nodiscard]] bool unitHasVehicleUI(const std::string& unit) const override;

    [[nodiscard]] bool inCombat() const override;
    [[nodiscard]] bool isFlyableArea() const override;
    [[nodiscard]] bool isOutdoors() const override;
    [[nodiscard]] bool isSwimming() const override;
    [[nodiscard]] bool isFlying() const override;
    [[nodiscard]] bool isMounted() const override;
    [[nodiscard]] bool isStealthed() const override;
    [[nodiscard]] bool inParty() const override;
    [[nodiscard]] bool inRaid() const override;
    [[nodiscard]] bool playerHasVehicleUI() const override;

    [[nodiscard]] int shapeshiftForm() const override;
    [[nodiscard]] int actionBarPage() const override;
    [[nodiscard]] int bonusBarOffset() const override;
    [[nodiscard]] int activeTalentGroup() const override;

    [[nodiscard]] bool hasPet() const override;
    [[nodiscard]] std::string petName() const override;
    [[nodiscard]] std::string petFamily() const override;
    [[nodiscard]] bool isEquippedItemType(const std::string& type) const override;
    [[nodiscard]] std::string channelingSpell() const override;
    [[nodiscard]] std::string cursorType() const override;
    [[nodiscard]] std::string clickedButton() const override;

    void reportUnknownOption(const std::string& name) const override;

private:
    /// Calls the global `fn` with string arguments and leaves its first result
    /// on the stack (nil when it is missing or raised).
    void call(const char* fn, std::initializer_list<const char*> args) const;
    [[nodiscard]] bool truthy(const char* fn, std::initializer_list<const char*> args) const;
    [[nodiscard]] double number(const char* fn, std::initializer_list<const char*> args) const;
    [[nodiscard]] std::string text(const char* fn, std::initializer_list<const char*> args) const;

    lua_State* L_;
};

class LuaModifierState : public ui::IModifierState {
public:
    explicit LuaModifierState(lua_State* L) : L_(L) {}
    [[nodiscard]] bool isKeyDown(Key key) const override;
    [[nodiscard]] bool isModifiedClick(const std::string& action) const override;

private:
    lua_State* L_;
};

/// IsEquippedItemType(type) (0x51C740) for this client: an equipped item whose
/// inventory type is the INVTYPE_ token or its global string, or whose item
/// class, subclass (ItemSubClass.dbc display or verbose name) or subclass
/// group (ItemSubClassMask.dbc) is called `type`. Case-insensitive.
bool isEquippedItemTypeFor(lua_State* L, const std::string& type);

} // namespace wowee::addons
