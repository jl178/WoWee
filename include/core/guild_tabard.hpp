#pragma once

// A guild tabard on a player: the chest's blank template swapped for the
// player's guild design (pipeline::applyGuildTabard). An unknown guild is
// asked for; when it answers, GameHandler::guildDataVersion moves and the
// callers composite again.

#include "game/game_handler.hpp"
#include "game/world_packets.hpp"
#include "pipeline/item_textures.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wowee::core {

inline void applyGuildTabardFor(game::GameHandler& gh, uint64_t playerGuid,
                                std::vector<std::pair<int, std::string>>& regionLayers) {
    const uint32_t guildId = gh.getEntityGuildId(playerGuid);
    if (guildId == 0) return;
    const game::GuildQueryResponseData* g = gh.findGuildData(guildId);
    if (!g) {
        (void)gh.lookupGuildName(guildId);  // queries the server; the answer redraws
        return;
    }
    pipeline::GuildTabardDesign d;
    d.emblemStyle = g->emblemStyle;
    d.emblemColor = g->emblemColor;
    d.borderStyle = g->borderStyle;
    d.borderColor = g->borderColor;
    d.backgroundColor = g->backgroundColor;
    pipeline::applyGuildTabard(regionLayers, d);
}

}  // namespace wowee::core
