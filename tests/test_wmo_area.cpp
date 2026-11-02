// The WMOAreaTable lookup that names Ironforge and the Undercity over the
// terrain chunks under them (Dun Morogh's, Tirisfal's). Rows below are copied
// from the 3.3.5 table: Ironforge is root wmoID 208, name set 0, AreaTable
// 1537; the Undercity is root 1150, name set 1, AreaTable 1497.
#include <catch_amalgamated.hpp>
#include "game/wmo_area.hpp"

#include <optional>
#include <vector>

using wowee::game::WmoAreaRow;
using wowee::game::findWmoAreaRow;
using wowee::game::pickWmoArea;
using wowee::game::wmoDecidesArea;

namespace {

std::vector<WmoAreaRow> ironforge() {
    return {
        {208, 0, -1, 1537, "City of Ironforge"},
        {208, 0, 3582, 1537, "The Great Forge"},
        {208, 0, 3560, 1537, "Ironforge"},
        {208, 0, 3557, 1537, ""},
        // Not real rows: a room naming no area of its own, and one naming a
        // different area, to pin the fallback and the precedence.
        {208, 0, 9001, 0, "Vault"},
        {208, 0, 9002, 4679, "The Forlorn Cavern"},
    };
}

std::vector<WmoAreaRow> undercity() {
    return {
        {1150, 1, -1, 1497, "Undercity"},
        {1150, 1, 3115, 1497, "Trade Quarter"},
        {1150, 1, 3061, 1497, "War Quarter"},
    };
}

} // namespace

TEST_CASE("A group's own WMOAreaTable row wins over the building's", "[wmo_area]") {
    const auto rows = ironforge();
    const auto forge = pickWmoArea(rows, 0, 3582);
    REQUIRE(forge.areaId == 1537);
    REQUIRE(forge.name == "The Great Forge");

    const auto cavern = pickWmoArea(rows, 0, 9002);
    REQUIRE(cavern.areaId == 4679);
    REQUIRE(cavern.name == "The Forlorn Cavern");
}

TEST_CASE("A group with no row of its own takes the building's -1 row", "[wmo_area]") {
    const auto rows = ironforge();
    const auto hall = pickWmoArea(rows, 0, 4242);
    REQUIRE(hall.areaId == 1537);
    REQUIRE(hall.name == "City of Ironforge");
}

TEST_CASE("A group row naming no area falls back to the building's area, not its name", "[wmo_area]") {
    const auto rows = ironforge();
    const auto vault = pickWmoArea(rows, 0, 9001);
    REQUIRE(vault.areaId == 1537);
    REQUIRE(vault.name == "Vault");

    // An unnamed room in a named building stays unnamed: the subzone text
    // then comes from the area (here the zone itself, so none).
    const auto unnamed = pickWmoArea(rows, 0, 3557);
    REQUIRE(unnamed.areaId == 1537);
    REQUIRE(unnamed.name.empty());
}

TEST_CASE("The placement's name set is preferred, then set 0", "[wmo_area]") {
    const auto rows = undercity();
    REQUIRE(pickWmoArea(rows, 1, 3115).name == "Trade Quarter");
    REQUIRE(pickWmoArea(rows, 1, 9999).name == "Undercity");
    // Set 1 rows do not answer for another placement of the same root.
    REQUIRE(pickWmoArea(rows, 0, 3115).areaId == 0);

    std::vector<WmoAreaRow> both = {
        {53, 0, -1, 0, "Lion's Pride Inn"},
        {53, 1, -1, 2104, "Deepwater Tavern"},
        {53, 0, 893, 0, "Common Room"},
    };
    // The group's own row in set 0 outranks the building's row in the
    // placement's own set: the group is the more specific key.
    REQUIRE(findWmoAreaRow(both, 1, 893)->name == "Common Room");
    REQUIRE(findWmoAreaRow(both, 1, 5)->name == "Deepwater Tavern");
    REQUIRE(findWmoAreaRow(both, 0, 5)->name == "Lion's Pride Inn");
    // The set 0 group row names no area; the set 1 building row does.
    REQUIRE(pickWmoArea(both, 1, 893).areaId == 2104);
}

TEST_CASE("The group key is the MOGP group id, not the file index", "[wmo_area]") {
    const auto rows = ironforge();
    // Group file index 76 of Ironforge is not a key; it falls to the -1 row.
    REQUIRE(pickWmoArea(rows, 0, 76).name == "City of Ironforge");
    REQUIRE(pickWmoArea(rows, 0, 3560).name == "Ironforge");
}

TEST_CASE("No rows, no area", "[wmo_area]") {
    const std::vector<WmoAreaRow> none;
    const auto info = pickWmoArea(none, 0, 3582);
    REQUIRE(info.areaId == 0);
    REQUIRE(info.name.empty());
    REQUIRE(findWmoAreaRow(none, 0, -1) == nullptr);
}

TEST_CASE("The WMO decides the area only from a floor not under open ground", "[wmo_area]") {
    // Inside Ironforge: standing on the city floor, under the mountain.
    REQUIRE(wmoDecidesArea(500.0f, 500.0f, 620.0f));
    // On the mountain over it: on the terrain, the city floor far below.
    REQUIRE_FALSE(wmoDecidesArea(620.0f, 500.0f, 620.0f));
    // A street or bridge laid over the ground (Stormwind): floor above terrain.
    REQUIRE(wmoDecidesArea(101.0f, 101.0f, 100.0f));
    // No WMO floor under the player at all.
    REQUIRE_FALSE(wmoDecidesArea(500.0f, std::nullopt, 620.0f));
    // Below the floor found (it is above the player): not standing on it.
    REQUIRE_FALSE(wmoDecidesArea(490.0f, 500.0f, 620.0f));
    // A terrain hole or a tile not resident leaves it to the WMO.
    REQUIRE(wmoDecidesArea(500.0f, 500.0f, std::nullopt));
}
