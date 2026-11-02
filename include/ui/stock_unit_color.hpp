#pragma once

#include <glm/glm.hpp>

namespace wowee {
namespace game { class GameHandler; class Unit; }
namespace ui {

/// A unit's name and selection colour, the stock rule (Wow.exe 0x521bf0 with
/// its "extended" argument, reached through CGUnit_C vtable +0x78): hostile
/// red, unfriendly orange, neutral yellow, friendly green; a dead creature
/// grey; a player (or a player's pet) red when it can fight you, light blue
/// when unflagged, by reaction when PvP flagged, party blue or green for your
/// party, sky blue for a friend. `attackRed` adds the red the target turns
/// while you are attacking it (CGUnit_C +0xA30 bit 0x10).
glm::vec3 stockUnitColor(game::GameHandler& gameHandler, const game::Unit& unit, bool attackRed);

}  // namespace ui
}  // namespace wowee
