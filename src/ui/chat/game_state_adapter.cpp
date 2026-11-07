// GameStateAdapter - IGameState from GameHandler + Renderer (no interface).
#include "ui/chat/game_state_adapter.hpp"
#include "addons/lua_api_helpers.hpp"
#include "game/game_handler.hpp"
#include "game/entity.hpp"
#include "rendering/renderer.hpp"

namespace wowee { namespace ui {

GameStateAdapter::GameStateAdapter(game::GameHandler& gameHandler,
                                   rendering::Renderer* renderer)
    : gameHandler_(gameHandler), renderer_(renderer) {}

uint64_t GameStateAdapter::guidFor(const std::string& unit) const {
    // No unit: the current target (0x60ABF0 with its "default" flag set).
    if (unit.empty()) return gameHandler_.getTargetGuid();
    return addons::resolveUnitOrName(&gameHandler_, unit);
}

namespace {
std::shared_ptr<game::Unit> unitFor(game::GameHandler& gh, uint64_t guid) {
    if (guid == 0) return nullptr;
    return std::dynamic_pointer_cast<game::Unit>(gh.getEntityManager().getEntity(guid));
}
} // namespace

bool GameStateAdapter::unitExists(const std::string& unit) const {
    const uint64_t guid = guidFor(unit);
    return guid != 0 && gameHandler_.getEntityManager().getEntity(guid) != nullptr;
}

bool GameStateAdapter::unitCanAttack(const std::string& unit) const {
    auto u = unitFor(gameHandler_, guidFor(unit));
    return u && u->getGuid() != gameHandler_.getPlayerGuid() &&
           gameHandler_.isHostileFactionPublic(u->getFactionTemplate());
}

bool GameStateAdapter::unitCanAssist(const std::string& unit) const {
    auto u = unitFor(gameHandler_, guidFor(unit));
    return u && !gameHandler_.isHostileFactionPublic(u->getFactionTemplate());
}

bool GameStateAdapter::unitInParty(const std::string& unit) const {
    const uint64_t guid = guidFor(unit);
    if (guid == 0 || !gameHandler_.isInGroup()) return false;
    if (guid == gameHandler_.getPlayerGuid()) return true;
    for (const auto& m : gameHandler_.getPartyData().members)
        if (m.guid == guid) return true;
    return false;
}

bool GameStateAdapter::unitInRaid(const std::string& unit) const {
    return inRaid() && unitInParty(unit);
}

bool GameStateAdapter::unitIsDead(const std::string& unit) const {
    auto u = unitFor(gameHandler_, guidFor(unit));
    return u && u->getHealth() == 0;
}

bool GameStateAdapter::unitHasVehicleUI(const std::string& unit) const {
    return guidFor(unit) == gameHandler_.getPlayerGuid() && playerHasVehicleUI();
}

bool GameStateAdapter::inCombat() const { return gameHandler_.isInCombat(); }
bool GameStateAdapter::isOutdoors() const { return !(renderer_ && renderer_->isPlayerIndoors()); }
bool GameStateAdapter::isSwimming() const { return gameHandler_.isSwimming(); }
bool GameStateAdapter::isFlying() const { return gameHandler_.isPlayerFlying(); }
bool GameStateAdapter::isMounted() const { return gameHandler_.isMounted(); }

bool GameStateAdapter::isStealthed() const {
    auto u = unitFor(gameHandler_, gameHandler_.getPlayerGuid());
    return u && u->hasCreepVisibility();
}

bool GameStateAdapter::inParty() const { return gameHandler_.isInGroup(); }

bool GameStateAdapter::inRaid() const {
    return gameHandler_.isInGroup() && game::groupIsRaid(gameHandler_.getPartyData().groupType);
}

bool GameStateAdapter::playerHasVehicleUI() const { return gameHandler_.getVehicleId() != 0; }

int GameStateAdapter::activeTalentGroup() const {
    return static_cast<int>(gameHandler_.getActiveTalentSpec()) + 1;
}

bool GameStateAdapter::hasPet() const { return gameHandler_.hasPet(); }

std::string GameStateAdapter::petName() const {
    auto u = unitFor(gameHandler_, gameHandler_.getPetGuid());
    return u ? u->getName() : std::string();
}

std::string GameStateAdapter::channelingSpell() const {
    if (!gameHandler_.isChanneling()) return {};
    return gameHandler_.getSpellName(gameHandler_.getCurrentCastSpellId());
}

void GameStateAdapter::reportUnknownOption(const std::string& name) const {
    // ERR_UNKNOWN_MACRO_OPTION_S, a system chat line (0x5216F0 type 0).
    gameHandler_.addSystemChatMessage("Unknown macro option: " + name);
}

} // namespace ui
} // namespace wowee
