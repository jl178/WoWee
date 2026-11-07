// GameStateAdapter - IGameState straight from GameHandler + Renderer.
//
// The fallback for when no interface is loaded. With FrameXML up the chat
// commands ask addons::LuaMacroState instead, so both the interface's own
// SecureCmdOptionParse and this client's commands see the answers the Lua API
// gives (macro_eval_convenience.cpp).
#pragma once

#include "ui/chat/i_game_state.hpp"

namespace wowee {
namespace game { class GameHandler; }
namespace rendering { class Renderer; }

namespace ui {

class GameStateAdapter : public IGameState {
public:
    GameStateAdapter(game::GameHandler& gameHandler, rendering::Renderer* renderer);

    [[nodiscard]] bool unitExists(const std::string& unit) const override;
    [[nodiscard]] bool unitCanAssist(const std::string& unit) const override;
    [[nodiscard]] bool unitCanAttack(const std::string& unit) const override;
    [[nodiscard]] bool unitInParty(const std::string& unit) const override;
    [[nodiscard]] bool unitInRaid(const std::string& unit) const override;
    [[nodiscard]] bool unitIsDead(const std::string& unit) const override;
    [[nodiscard]] bool unitHasVehicleUI(const std::string& unit) const override;

    [[nodiscard]] bool inCombat() const override;
    [[nodiscard]] bool isFlyableArea() const override { return false; }
    [[nodiscard]] bool isOutdoors() const override;
    [[nodiscard]] bool isSwimming() const override;
    [[nodiscard]] bool isFlying() const override;
    [[nodiscard]] bool isMounted() const override;
    [[nodiscard]] bool isStealthed() const override;
    [[nodiscard]] bool inParty() const override;
    [[nodiscard]] bool inRaid() const override;
    [[nodiscard]] bool playerHasVehicleUI() const override;

    [[nodiscard]] int shapeshiftForm() const override { return 0; }
    [[nodiscard]] int actionBarPage() const override { return 1; }
    [[nodiscard]] int bonusBarOffset() const override { return 0; }
    [[nodiscard]] int activeTalentGroup() const override;

    [[nodiscard]] bool hasPet() const override;
    [[nodiscard]] std::string petName() const override;
    [[nodiscard]] std::string petFamily() const override { return {}; }
    [[nodiscard]] bool isEquippedItemType(const std::string&) const override { return false; }
    [[nodiscard]] std::string channelingSpell() const override;
    [[nodiscard]] std::string cursorType() const override { return {}; }
    [[nodiscard]] std::string clickedButton() const override { return {}; }

    void reportUnknownOption(const std::string& name) const override;

private:
    [[nodiscard]] uint64_t guidFor(const std::string& unit) const;

    game::GameHandler& gameHandler_;
    rendering::Renderer* renderer_;
};

} // namespace ui
} // namespace wowee
