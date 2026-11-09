#pragma once

// The geosets a character model shows, the stock way (Wow.exe FUN_004ed900,
// with HelmetGeosetVisData at 0x4ef0d0), for every path that dresses one: the
// local player, other players, CreatureDisplayInfoExtra NPCs and the portrait.
// They were four hand-written versions that disagreed with stock and with each
// other (gloves and sleeves both shown, robes over boots, no pant flaps, no
// belts on NPCs). The rule itself is the pure core::stockEquipmentGeosets in
// geoset_rules.hpp; this file only reads the tables it needs.

#include "core/geoset_rules.hpp"

#include <array>
#include <vector>
#include <cstdint>
#include <unordered_set>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace core {

struct CharacterGeosetQuery {
    /// ItemDisplayInfo id worn per StockGeosetInput::Kind, 0 for nothing.
    std::array<uint32_t, StockGeosetInput::KindCount> displays{};
    /// CharHairGeosets scalp for the hair style (0: none found, stock uses 1).
    uint16_t hairScalp = 0;
    /// CharacterFacialHairStyles row: geoset variants for groups 1, 2, 3, 17.
    bool haveFacialRow = false;
    uint16_t facial100 = 0, facial200 = 0, facial300 = 0, facial1700 = 0;
    uint8_t race = 0;
    uint8_t gender = 0;
    bool deathKnight = false;
    /// The helm is worn and shown (its HelmetGeosetVisData applies).
    bool helmShown = true;
    /// The cloak is worn and shown.
    bool cloakShown = true;
    bool guildTabard = false;
    /// Geosets in groups the stock rules do not know (above 20), kept as
    /// given: an HD model's face shape, eyes and torso.
    std::vector<uint16_t> extra;
};

/// Map an inventory type (Item.dbc InventoryType) to the component kind its
/// geosets are read for, or -1 when it touches none.
int geosetKindForInventoryType(uint8_t inventoryType);

/// The geoset ids to enable, stock-exact, plus both spellings of the HD
/// models' bare-feet group 20 (stock models carry none and are unaffected).
std::unordered_set<uint16_t> buildStockCharacterGeosets(pipeline::AssetManager& assets,
                                                        const CharacterGeosetQuery& q);

}  // namespace core
}  // namespace wowee
