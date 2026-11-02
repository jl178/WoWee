// wmo_area.hpp - which WMOAreaTable row a WMO group belongs to, and the area
// a player inside it is in.
//
// The terrain chunk under Ironforge is Dun Morogh's and the one under the
// Undercity is Tirisfal's. The stock client names the city anyway because,
// while the player stands in a WMO group, the area comes from WMOAreaTable:
// the row keyed by the root's MOHD wmoID, the placement's MODF name set and
// the group's MOGP group id, or the root's -1 row when the group has none of
// its own. The row's AreaTableID is the area, and its name - "The Great
// Forge", "Trade Quarter" - is the subzone text. Only when no row names an
// area does the terrain chunk's area apply.
//
// The group key is MOGP's own group id, global across all WMOs (Ironforge's
// run 3549 to 3699 and on), not the group's index in the root file.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wowee {
namespace game {

/// The WMOAreaTable columns the area lookup needs.
struct WmoAreaRow {
    uint32_t wmoId = 0;
    uint16_t nameSet = 0;
    int32_t groupId = -1;   ///< MOGP group id; -1 is the whole building
    uint32_t areaId = 0;    ///< AreaTable.dbc, 0 when the row names none
    std::string name;       ///< AreaName_lang; often empty
};

/// The row for a group among one root's rows: the group's own row first, then
/// the root's -1 row, each with the placement's name set preferred over set 0.
/// Row needs nameSet and groupId members. Null when neither exists.
template <class Row>
[[nodiscard]] const Row* findWmoAreaRow(const std::vector<Row>& rows,
                                        uint16_t nameSet, int32_t groupId) {
    const Row* best = nullptr;
    int bestRank = 0;
    for (const Row& row : rows) {
        if (row.groupId != groupId && row.groupId != -1) continue;
        if (row.nameSet != nameSet && row.nameSet != 0) continue;
        const int rank = (row.groupId == groupId ? 2 : 0) + (row.nameSet == nameSet ? 1 : 0) + 1;
        if (rank > bestRank) {
            bestRank = rank;
            best = &row;
        }
    }
    return best;
}

/// What a WMO group says about the area it is in.
struct WmoAreaInfo {
    uint32_t areaId = 0;  ///< 0: no row names one, use the terrain's
    std::string name;     ///< the matched row's name; empty for none
};

/// The area for a group: the matched row's AreaTableID, or the root's -1 row's
/// when the group's own row leaves it 0. The name is the matched row's only -
/// an unnamed room in a named building is not given the building's name.
[[nodiscard]] inline WmoAreaInfo pickWmoArea(const std::vector<WmoAreaRow>& rows,
                                             uint16_t nameSet, int32_t groupId) {
    WmoAreaInfo out;
    const WmoAreaRow* own = findWmoAreaRow(rows, nameSet, groupId);
    if (!own) return out;
    out.name = own->name;
    out.areaId = own->areaId;
    if (out.areaId == 0 && own->groupId != -1) {
        if (const WmoAreaRow* building = findWmoAreaRow(rows, nameSet, -1)) {
            out.areaId = building->areaId;
        }
    }
    return out;
}

/// Whether the WMO under the player decides the area rather than the terrain,
/// the test the server applies to its own copy of the data (Map::GetAreaId):
/// the player stands on the WMO's floor, and that floor is either above the
/// ground or the player is below the ground - inside the mountain Ironforge is
/// cut into, or under Tirisfal. Standing on the mountain over Ironforge, with
/// the city's groups below and around, is Dun Morogh.
///
/// No terrain height (a hole, or the tile not resident) leaves it to the WMO.
[[nodiscard]] inline bool wmoDecidesArea(float playerZ, std::optional<float> wmoFloorZ,
                                         std::optional<float> terrainZ) {
    constexpr float kTolerance = 0.5f;
    if (!wmoFloorZ) return false;
    if (playerZ < *wmoFloorZ - kTolerance) return false;
    if (!terrainZ) return true;
    return playerZ < *terrainZ - kTolerance || *wmoFloorZ > *terrainZ;
}

} // namespace game
} // namespace wowee
