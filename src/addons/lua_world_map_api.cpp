// lua_world_map_api.cpp - the stock world map's C functions, rebuilt from
// Wow.exe 3.3.5a (build 12340).
//
// In the real client the world map is FrameXML from top to bottom: the detail
// tiles, overlays, landmarks, quest markers, the player arrow and the zoom
// buttons are all WorldMapFrame.lua. What lives in C is the map's state and
// the arithmetic, in CWorldMap (WorldMap.cpp), behind about thirty Lua
// functions. Each function here names the address of the handler it was
// rebuilt from.
//
// The state is four numbers, at 0x00ACCF04..10 in the client:
//   continent  0-based index into the continent list, -1 the world view,
//              -2 "not on a continent" (the cosmic view, an instance, a
//              dungeon floor)
//   zone       0-based index into that continent's zone list, -1 for none
//   dungeon    DungeonMap row id of the floor showing, -1 for none
//   area       WorldMapArea id when continent is -2, otherwise -1
// and every change goes through one function (0x00546310), which also
// rebuilds the landmark and overlay lists and fires WORLD_MAP_UPDATE.
//
// The continent list (0x00BE8F10, 0x10028 bytes an entry) is built once from
// WorldMapArea: every row with AreaID 0 is a continent, in file order, and its
// zones are the rows whose DisplayMapID (or MapID when that is -1) matches,
// sorted by name. Each entry also carries the continent's rectangle on the
// world map, from WorldMapContinent, and a 128x128 grid from
// Interface\WorldMap\<name>.zmp that says which zone each cell belongs to -
// that grid, not the zone rectangles, is how a click finds a zone.

#include "addons/world_map_api.hpp"

#include "addons/lua_api_helpers.hpp"
#include "addons/lua_engine.hpp"
#include "addons/lua_services.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "game/game_handler.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"

#include <algorithm>
#include <array>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::addons::worldmap {

namespace {

// ---------------------------------------------------------------------------
// Constants, read out of Wow.exe's .rdata
// ---------------------------------------------------------------------------

/// 0x009EA27C. "Is this float zero" everywhere in WorldMap.cpp.
constexpr float kEpsilon = 2.384185791015625e-07f;
/// 0x00A0B5E0: 1 / (64 * 533.333) - world yards to a fraction of the map grid.
constexpr float kWorldToGrid = 2.9296876164153218e-05f;
/// 0x00A0B5EC / 0x00A0B5F4: 1/62.625 and 1/41.75 - ADT tiles to a fraction of
/// the world map, horizontally and vertically.
constexpr float kWorldMapPerTileX = 0.015968063846230507f;
constexpr float kWorldMapPerTileY = 0.023952096700668335f;
/// 0x00A0B5E8 / 0x00A0B5F0: the two above divided by 533.333 (yards).
constexpr float kWorldMapPerYardX = 2.994011992996093e-05f;
constexpr float kWorldMapPerYardY = 4.49101826234255e-05f;
/// 0x00A0B6DC / 0x00A0B6D8 / 0x009E8DD4: half the world map in tiles, and the
/// 32 tiles from the edge of the ADT grid to its middle.
constexpr float kWorldMapHalfTilesX = 31.3125f;
constexpr float kWorldMapHalfTilesY = 20.875f;
constexpr float kTilesToCentre = 32.0f;
/// 0x009E2ED8 / 0x009E2ED4: the zone map is 1002 x 668 pixels.
constexpr float kInvMapPixelsX = 0.0009980039903894067f;
constexpr float kInvMapPixelsY = 0.001497006043791771f;
/// 0x00A0B5D8.
constexpr float kMapPixelsX = 1002.0f;
/// WORLD_MAP_UPDATE's index in the event table at 0x00C24EB0.
constexpr const char* kWorldMapUpdate = "WORLD_MAP_UPDATE";

/// x87 FISTP in the default rounding mode, which is what the decompiler shows
/// as ROUND(): nearest, ties to even.
int roundNearest(float f) { return static_cast<int>(std::nearbyint(f)); }
/// _ftol: truncation, for the places the client switched the FPU to it.
int truncate(float f) { return static_cast<int>(f); }
using detail::luaRound;
using detail::luaTruncate;

// ---------------------------------------------------------------------------
// Client data
// ---------------------------------------------------------------------------

struct WorldMapAreaRow {        // WorldMapArea.dbc
    int id = 0, mapId = 0, areaId = 0;
    std::string name;           // internal name: the folder under Interface\WorldMap
    float left = 0, right = 0, top = 0, bottom = 0;  // left/right are world Y, top/bottom world X
    int displayMapId = -1;
    int defaultDungeonFloor = 0;  // a DungeonMap id; -1 = "uses the terrain map"
    int parentWorldMapId = 0;

    bool hasBounds() const {
        return std::fabs(left) >= kEpsilon || std::fabs(right) >= kEpsilon ||
               std::fabs(top) >= kEpsilon || std::fabs(bottom) >= kEpsilon;
    }
    int effectiveMapId() const { return displayMapId < 0 ? mapId : displayMapId; }
};

struct AreaRow {                // AreaTable.dbc
    int id = 0, mapId = 0, parent = 0, areaBit = 0, flags = 0, explorationLevel = 0;
    std::string name;
};

struct MapRow {                 // Map.dbc
    int id = 0;
    std::string name;
    int corpseMapId = -1;
    float corpseX = 0, corpseY = 0;
};

struct ContinentRow {           // WorldMapContinent.dbc
    int id = 0, mapId = 0;
    int left = 0, right = 0, top = 0, bottom = 0;   // ADT tiles
    float offsetX = 0, offsetY = 0, scale = 1;
    int worldMapId = 0;
};

struct TransformRow {           // WorldMapTransforms.dbc
    int id = 0, mapId = 0;
    float minX = 0, minY = 0, maxX = 0, maxY = 0;
    int newMapId = 0;
    float offsetX = 0, offsetY = 0;
    int newDungeonMapId = 0;
};

struct DungeonMapRow {          // DungeonMap.dbc
    int id = 0, mapId = 0, floor = 0;
    float y0 = 0, y1 = 0;       // fields 3 and 4: the horizontal (world Y) span
    float x0 = 0, x1 = 0;       // fields 5 and 6: the vertical (world X) span
    int parentWorldMapId = 0;
};

struct DungeonChunkRow {        // DungeonMapChunk.dbc
    int id = 0, mapId = 0, wmoGroupId = 0, dungeonMapId = 0;
    float minZ = 0;
};

struct OverlayRow {             // WorldMapOverlay.dbc
    int id = 0, mapAreaId = 0;
    std::array<int, 4> areaIds{};
    int mapPointX = 0, mapPointY = 0;
    std::string texture;
    int width = 0, height = 0, offsetX = 0, offsetY = 0;
    int hitTop = 0, hitLeft = 0, hitBottom = 0, hitRight = 0;
};

struct PoiRow {                 // AreaPOI.dbc
    int id = 0, importance = 0;
    std::array<int, 9> icons{};
    float x = 0, y = 0, z = 0;
    int mapId = 0, flags = 0, areaId = 0;
    std::string name, description;
    int worldStateId = 0, worldMapLink = 0;
};

/// One entry of the continent list at 0x00BE8F10.
struct Continent {
    int mapId = 0;              // +0x00
    int wmaId = 0;              // +0x04 the continent's own WorldMapArea row
    std::vector<int> zones;     // +0x0C/+0x10 WorldMapArea ids, sorted by name
    std::vector<int> grid;      // +0x14 128x128 WorldMapArea ids, from the .zmp
    float rectTop = 0, rectLeft = 0, rectBottom = 0, rectRight = 0;  // +0x10014..20
    int worldMapId = 0;         // +0x10024 zero: not on the world map (Outland)
};

struct Landmark {               // 0x14 bytes at 0x00BE8F1C
    int poiId = 0;              // +0x00 AreaPOI id (unset for the gossip point)
    float u = 0, v = 0;         // +0x04, +0x08
    bool gossip = false;        // +0x10: the gossip point at 0x00D394E8
};

/// The gossip point: SMSG_GOSSIP_POI (0x0058A870 -> 0x007F4870) writes slot 1
/// of the client's three map points (0x00D39490, 0x58 bytes each, laid out as
/// an AreaPOI row) and SetMap puts it on the map after the AreaPOI rows. Only
/// the newest one is kept. It goes after 480 seconds, or when the player comes
/// within 10 yards of it (the minimap's update, 0x007F5BA0); either way the
/// map is set again. The record's name points at a 64-byte buffer, its
/// description is null, its icon is the packet's icon, its map is the map the
/// player was on when it came.
struct GossipPoint {
    bool active = false;
    float x = 0, y = 0;         // client order (x north)
    int mapId = 0;
    int icon = 0;
    uint32_t flags = 0;         // the packet's, +0x40
    std::string name;
    std::chrono::steady_clock::time_point expires{};
    // The newest gossip entry of the handler's list last seen, to tell a new
    // packet from one already taken.
    size_t seenCount = 0;
    float seenX = 0, seenY = 0;
    uint32_t seenIcon = 0;
    std::string seenName;
};

struct Data {
    bool attempted = false;
    bool ok = false;

    std::vector<WorldMapAreaRow> wma;
    std::unordered_map<int, size_t> wmaById;
    std::unordered_map<int, AreaRow> areas;
    std::unordered_map<int, MapRow> maps;
    std::vector<ContinentRow> wmContinents;
    std::vector<TransformRow> transforms;
    std::vector<DungeonMapRow> dungeonMaps;
    std::unordered_map<int, size_t> dungeonMapById;
    std::vector<DungeonChunkRow> chunks;
    std::vector<OverlayRow> overlays;
    std::unordered_map<int, size_t> overlayById;
    std::vector<PoiRow> pois;
    std::unordered_map<int, size_t> poiById;
    std::vector<int> poiWorldStates;   // every WorldStateID an AreaPOI row names

    std::vector<Continent> continents;
    std::vector<int> orphans;   // 0x00BE8F3C

    const WorldMapAreaRow* wmaRow(int id) const {
        auto it = wmaById.find(id);
        return it == wmaById.end() ? nullptr : &wma[it->second];
    }
    const AreaRow* area(int id) const {
        auto it = areas.find(id);
        return it == areas.end() ? nullptr : &it->second;
    }
    const MapRow* map(int id) const {
        auto it = maps.find(id);
        return it == maps.end() ? nullptr : &it->second;
    }
    const DungeonMapRow* dungeonMap(int id) const {
        auto it = dungeonMapById.find(id);
        return it == dungeonMapById.end() ? nullptr : &dungeonMaps[it->second];
    }
    const OverlayRow* overlay(int id) const {
        auto it = overlayById.find(id);
        return it == overlayById.end() ? nullptr : &overlays[it->second];
    }
    const PoiRow* poi(int id) const {
        auto it = poiById.find(id);
        return it == poiById.end() ? nullptr : &pois[it->second];
    }
};

/// The map's state, 0x00ACCF04..10, and the two lists SetMap rebuilds.
struct State {
    int continent = -1;
    int zone = -1;
    int dungeon = -1;
    int area = -1;
    bool initialised = false;
    std::vector<Landmark> landmarks;   // 0x00BE8EE0 count, 0x00BE8F1C
    std::vector<int> overlays;         // 0x00BE8EE4 count, 0x00BE8F2C
    uint64_t exploredHash = 0;
};

Data gData;
State gState;
GossipPoint gGossip;

int fieldInt(const pipeline::DBCFile& dbc, uint32_t row, uint32_t field, int fallback = 0) {
    return field < dbc.getFieldCount() ? dbc.getInt32(row, field) : fallback;
}
float fieldFloat(const pipeline::DBCFile& dbc, uint32_t row, uint32_t field) {
    return field < dbc.getFieldCount() ? dbc.getFloat(row, field) : 0.0f;
}
std::string fieldString(const pipeline::DBCFile& dbc, uint32_t row, uint32_t field) {
    return field < dbc.getFieldCount() ? dbc.getString(row, field) : std::string();
}

/// SStrCmpI, which is what the zone list's qsort comparator (0x005437B0)
/// calls on the two AreaTable names.
int compareNoCase(const std::string& a, const std::string& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int ca = std::toupper(static_cast<unsigned char>(a[i]));
        const int cb = std::toupper(static_cast<unsigned char>(b[i]));
        if (ca != cb) return ca - cb;
    }
    return static_cast<int>(a.size()) - static_cast<int>(b.size());
}

/// The continent list, 0x00547EB0.
void buildContinents(Data& d, pipeline::AssetManager& am) {
    d.continents.clear();
    for (const auto& row : d.wma) {
        if (row.areaId != 0) continue;
        Continent c;
        c.mapId = row.mapId;
        c.wmaId = row.id;
        for (const auto& z : d.wma) {
            if (z.areaId != 0 && z.effectiveMapId() == row.mapId) c.zones.push_back(z.id);
        }
        std::sort(c.zones.begin(), c.zones.end(), [&d](int a, int b) {
            const WorldMapAreaRow* ra = d.wmaRow(a);
            const WorldMapAreaRow* rb = d.wmaRow(b);
            const AreaRow* aa = ra ? d.area(ra->areaId) : nullptr;
            const AreaRow* ab = rb ? d.area(rb->areaId) : nullptr;
            if (!aa || !ab) return false;
            return compareNoCase(aa->name, ab->name) < 0;
        });
        // The rectangle the continent covers on the world map.
        for (const auto& wc : d.wmContinents) {
            if (wc.mapId != row.mapId) continue;
            const float k = wc.scale * kTilesToCentre;
            const float baseX = ((kWorldMapHalfTilesX - k) + wc.offsetX) * kWorldMapPerTileX;
            c.rectLeft = static_cast<float>(wc.left) * wc.scale * kWorldMapPerTileX + baseX;
            c.rectRight = static_cast<float>(wc.right + 1) * wc.scale * kWorldMapPerTileX + baseX;
            const float baseY = ((kWorldMapHalfTilesY - k) + wc.offsetY) * kWorldMapPerTileY;
            c.rectTop = static_cast<float>(wc.top) * wc.scale * kWorldMapPerTileY + baseY;
            c.rectBottom = static_cast<float>(wc.bottom + 1) * wc.scale * kWorldMapPerTileY + baseY;
            c.worldMapId = wc.worldMapId;
            break;
        }
        // Which zone each cell of the continent belongs to. The file holds
        // AreaTable ids; each is taken up one level to its zone and then to
        // that zone's WorldMapArea row on this continent.
        const std::string path = "Interface\\WorldMap\\" + row.name + ".zmp";
        auto bytes = am.readFileOptional(path);
        if (bytes.size() == 128 * 128 * 4) {
            c.grid.assign(128 * 128, 0);
            for (int i = 0; i < 128 * 128; ++i) {
                int areaId = 0;
                std::memcpy(&areaId, bytes.data() + i * 4, 4);
                if (areaId == 0) continue;
                const AreaRow* a = d.area(areaId);
                if (!a) continue;
                if (a->parent != 0) areaId = a->parent;
                for (const auto& z : d.wma) {
                    if (z.effectiveMapId() == row.mapId && z.areaId == areaId) {
                        c.grid[static_cast<size_t>(i)] = z.id;
                        break;
                    }
                }
            }
        } else {
            LOG_WARNING("WorldMap: ", path, " is missing or the wrong size (",
                        bytes.size(), " bytes) - clicks on that continent find no zone");
        }
        d.continents.push_back(std::move(c));
    }
    // Areas the continent list does not hold: on a map that is no continent,
    // or with no map of their own (Dalaran, which is all dungeon floors).
    d.orphans.clear();
    for (const auto& row : d.wma) {
        if (row.areaId == 0) continue;
        bool onContinent = false;
        if (row.hasBounds()) {
            for (const auto& c : d.continents) {
                if (c.mapId == row.mapId) { onContinent = true; break; }
            }
        }
        if (!onContinent) d.orphans.push_back(row.id);
    }
}

bool loadData(game::GameHandler* gh) {
    if (gData.attempted) return gData.ok;
    if (!gh) return false;
    pipeline::AssetManager* am = gh->services().assetManager;
    if (!am || !am->isInitialized()) return false;
    gData.attempted = true;

    auto wma = am->loadDBC("WorldMapArea.dbc");
    auto area = am->loadDBC("AreaTable.dbc");
    auto map = am->loadDBC("Map.dbc");
    if (!wma || !wma->isLoaded() || !area || !area->isLoaded() || !map || !map->isLoaded()) {
        LOG_WARNING("WorldMap: WorldMapArea, AreaTable or Map.dbc did not load - the map API "
                    "stays on its fallback");
        return false;
    }
    // The 3.3.5 shapes. An older expansion's files are laid out differently
    // and are left to the fallback rather than read through the wrong columns.
    if (wma->getFieldCount() < 11 || area->getFieldCount() < 12 || map->getFieldCount() < 62) {
        LOG_WARNING("WorldMap: client data is not 3.3.5-shaped (WorldMapArea has ",
                    wma->getFieldCount(), " columns) - the map API stays on its fallback");
        return false;
    }

    for (uint32_t i = 0; i < wma->getRecordCount(); ++i) {
        WorldMapAreaRow r;
        r.id = fieldInt(*wma, i, 0);
        r.mapId = fieldInt(*wma, i, 1);
        r.areaId = fieldInt(*wma, i, 2);
        r.name = fieldString(*wma, i, 3);
        r.left = fieldFloat(*wma, i, 4);
        r.right = fieldFloat(*wma, i, 5);
        r.top = fieldFloat(*wma, i, 6);
        r.bottom = fieldFloat(*wma, i, 7);
        r.displayMapId = fieldInt(*wma, i, 8, -1);
        r.defaultDungeonFloor = fieldInt(*wma, i, 9);
        r.parentWorldMapId = fieldInt(*wma, i, 10);
        gData.wmaById[r.id] = gData.wma.size();
        gData.wma.push_back(std::move(r));
    }
    for (uint32_t i = 0; i < area->getRecordCount(); ++i) {
        AreaRow r;
        r.id = fieldInt(*area, i, 0);
        r.mapId = fieldInt(*area, i, 1);
        r.parent = fieldInt(*area, i, 2);
        r.areaBit = fieldInt(*area, i, 3);
        r.flags = fieldInt(*area, i, 4);
        r.explorationLevel = fieldInt(*area, i, 10);
        r.name = fieldString(*area, i, 11);
        gData.areas[r.id] = std::move(r);
    }
    for (uint32_t i = 0; i < map->getRecordCount(); ++i) {
        MapRow r;
        r.id = fieldInt(*map, i, 0);
        r.name = fieldString(*map, i, 5);
        r.corpseMapId = fieldInt(*map, i, 59, -1);
        r.corpseX = fieldFloat(*map, i, 60);
        r.corpseY = fieldFloat(*map, i, 61);
        gData.maps[r.id] = std::move(r);
    }
    if (auto dbc = am->loadDBCOptional("WorldMapContinent.dbc"); dbc && dbc->isLoaded()) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            ContinentRow r;
            r.id = fieldInt(*dbc, i, 0);
            r.mapId = fieldInt(*dbc, i, 1);
            r.left = fieldInt(*dbc, i, 2);
            r.right = fieldInt(*dbc, i, 3);
            r.top = fieldInt(*dbc, i, 4);
            r.bottom = fieldInt(*dbc, i, 5);
            r.offsetX = fieldFloat(*dbc, i, 6);
            r.offsetY = fieldFloat(*dbc, i, 7);
            r.scale = fieldFloat(*dbc, i, 8);
            r.worldMapId = fieldInt(*dbc, i, 13);
            gData.wmContinents.push_back(r);
        }
    }
    if (auto dbc = am->loadDBCOptional("WorldMapTransforms.dbc"); dbc && dbc->isLoaded()) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            TransformRow r;
            r.id = fieldInt(*dbc, i, 0);
            r.mapId = fieldInt(*dbc, i, 1);
            r.minX = fieldFloat(*dbc, i, 2);
            r.minY = fieldFloat(*dbc, i, 3);
            r.maxX = fieldFloat(*dbc, i, 4);
            r.maxY = fieldFloat(*dbc, i, 5);
            r.newMapId = fieldInt(*dbc, i, 6);
            r.offsetX = fieldFloat(*dbc, i, 7);
            r.offsetY = fieldFloat(*dbc, i, 8);
            r.newDungeonMapId = fieldInt(*dbc, i, 9);
            gData.transforms.push_back(r);
        }
    }
    if (auto dbc = am->loadDBCOptional("DungeonMap.dbc"); dbc && dbc->isLoaded()) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            DungeonMapRow r;
            r.id = fieldInt(*dbc, i, 0);
            r.mapId = fieldInt(*dbc, i, 1);
            r.floor = fieldInt(*dbc, i, 2);
            r.y0 = fieldFloat(*dbc, i, 3);
            r.y1 = fieldFloat(*dbc, i, 4);
            r.x0 = fieldFloat(*dbc, i, 5);
            r.x1 = fieldFloat(*dbc, i, 6);
            r.parentWorldMapId = fieldInt(*dbc, i, 7);
            gData.dungeonMapById[r.id] = gData.dungeonMaps.size();
            gData.dungeonMaps.push_back(r);
        }
    }
    if (auto dbc = am->loadDBCOptional("DungeonMapChunk.dbc"); dbc && dbc->isLoaded()) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            DungeonChunkRow r;
            r.id = fieldInt(*dbc, i, 0);
            r.mapId = fieldInt(*dbc, i, 1);
            r.wmoGroupId = fieldInt(*dbc, i, 2);
            r.dungeonMapId = fieldInt(*dbc, i, 3);
            r.minZ = fieldFloat(*dbc, i, 4);
            gData.chunks.push_back(r);
        }
    }
    if (auto dbc = am->loadDBCOptional("WorldMapOverlay.dbc");
        dbc && dbc->isLoaded() && dbc->getFieldCount() >= 17) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            OverlayRow r;
            r.id = fieldInt(*dbc, i, 0);
            r.mapAreaId = fieldInt(*dbc, i, 1);
            for (int k = 0; k < 4; ++k) r.areaIds[k] = fieldInt(*dbc, i, 2 + k);
            r.mapPointX = fieldInt(*dbc, i, 6);
            r.mapPointY = fieldInt(*dbc, i, 7);
            r.texture = fieldString(*dbc, i, 8);
            r.width = fieldInt(*dbc, i, 9);
            r.height = fieldInt(*dbc, i, 10);
            r.offsetX = fieldInt(*dbc, i, 11);
            r.offsetY = fieldInt(*dbc, i, 12);
            r.hitTop = fieldInt(*dbc, i, 13);
            r.hitLeft = fieldInt(*dbc, i, 14);
            r.hitBottom = fieldInt(*dbc, i, 15);
            r.hitRight = fieldInt(*dbc, i, 16);
            gData.overlayById[r.id] = gData.overlays.size();
            gData.overlays.push_back(std::move(r));
        }
    }
    if (auto dbc = am->loadDBCOptional("AreaPOI.dbc");
        dbc && dbc->isLoaded() && dbc->getFieldCount() >= 54) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            PoiRow r;
            r.id = fieldInt(*dbc, i, 0);
            r.importance = fieldInt(*dbc, i, 1);
            for (int k = 0; k < 9; ++k) r.icons[k] = fieldInt(*dbc, i, 2 + k);
            r.x = fieldFloat(*dbc, i, 12);
            r.y = fieldFloat(*dbc, i, 13);
            r.z = fieldFloat(*dbc, i, 14);
            r.mapId = fieldInt(*dbc, i, 15);
            r.flags = fieldInt(*dbc, i, 16);
            r.areaId = fieldInt(*dbc, i, 17);
            r.name = fieldString(*dbc, i, 18);
            r.description = fieldString(*dbc, i, 35);
            r.worldStateId = fieldInt(*dbc, i, 52);
            r.worldMapLink = fieldInt(*dbc, i, 53);
            gData.poiById[r.id] = gData.pois.size();
            if (r.worldStateId != 0 &&
                std::find(gData.poiWorldStates.begin(), gData.poiWorldStates.end(),
                          r.worldStateId) == gData.poiWorldStates.end()) {
                gData.poiWorldStates.push_back(r.worldStateId);
            }
            gData.pois.push_back(std::move(r));
        }
    }

    buildContinents(gData, *am);
    gData.ok = !gData.continents.empty();
    LOG_INFO("WorldMap: ", gData.wma.size(), " areas, ", gData.continents.size(),
             " continents, ", gData.overlays.size(), " overlays, ", gData.pois.size(),
             " landmarks, ", gData.dungeonMaps.size(), " dungeon floors");
    return gData.ok;
}

// ---------------------------------------------------------------------------
// The player and the world, as the map sees them
// ---------------------------------------------------------------------------

/// The player's position in the client's order: x north (server X), y west.
bool playerPosition(game::GameHandler* gh, float& x, float& y, float& z) {
    if (!gh || gh->getPlayerGuid() == 0) return false;
    const auto& mi = gh->getMovementInfo();
    const glm::vec3 s = core::coords::canonicalToServer(glm::vec3(mi.x, mi.y, mi.z));
    x = s.x;
    y = s.y;
    z = s.z;
    return true;
}

/// The zone (top-level AreaTable id) the player is in - 0x00BD080C.
int playerZoneId(lua_State* L, game::GameHandler* gh) {
    if (auto* svc = getLuaServices(L); svc && svc->getLiveZoneId) {
        if (const uint32_t z = svc->getLiveZoneId()) return static_cast<int>(z);
    }
    return gh ? static_cast<int>(gh->getWorldStateZoneId()) : 0;
}

/// The global WMO group id the player stands in - 0x00BD0814.
///
/// Asked of the WMO renderer at most ten times a second: the map asks for
/// every party and raid member's position every frame, and the client itself
/// only learns the group when the player crosses into another one.
int playerWmoGroupId(lua_State* L) {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point askedAt{};
    static int cached = 0;
    const auto now = Clock::now();
    if (now - askedAt < std::chrono::milliseconds(100)) return cached;
    askedAt = now;
    cached = 0;
    if (auto* svc = getLuaServices(L); svc && svc->getLiveWmoGroupId) {
        cached = static_cast<int>(svc->getLiveWmoGroupId());
    }
    return cached;
}

/// CGPlayer::IsExplored (0x00546100): bit `areaBit` of PLAYER_EXPLORED_ZONES.
bool explored(game::GameHandler* gh, int areaBit) {
    if (!gh) return false;
    const int byte = (areaBit + ((areaBit >> 31) & 7)) >> 3;
    if (static_cast<unsigned>(byte) >= 0x200u) return true;
    int bit = areaBit & static_cast<int>(0x80000007u);
    if (bit < 0) bit = ((bit - 1) | static_cast<int>(0xFFFFFFF8u)) + 1;
    const auto& masks = gh->getPlayerExploredZoneMasks();
    const size_t word = static_cast<size_t>(byte) / 4;
    if (word >= masks.size()) return false;
    const uint32_t b = (masks[word] >> ((byte % 4) * 8)) & 0xFFu;
    return (b & (1u << (bit & 31))) != 0;
}

/// FUN_00548D10: a world state's value, zero when the client has none.
int worldStateValue(game::GameHandler* gh, int id) {
    if (!gh) return 0;
    auto v = gh->getWorldState(static_cast<uint32_t>(id));
    return v ? static_cast<int>(*v) : 0;
}

/// What the landmark and overlay lists depend on besides the view: the
/// explored-area bits, and the world states AreaPOI rows are keyed on. The
/// client rebuilds the lists when either changes - 0x007F5BA0 for new ground,
/// 0x005285F0 for a world state with a landmark on it (which also fires
/// UPDATE_WORLD_STATES) - by calling SetMap again with what is showing.
uint64_t exploredHash(game::GameHandler* gh) {
    if (!gh) return 0;
    uint64_t h = 1469598103934665603ull;
    for (uint32_t m : gh->getPlayerExploredZoneMasks()) {
        h ^= m;
        h *= 1099511628211ull;
    }
    for (int id : gData.poiWorldStates) {
        h ^= static_cast<uint64_t>(worldStateValue(gh, id)) + (static_cast<uint64_t>(id) << 32);
        h *= 1099511628211ull;
    }
    return h;
}

/// Takes a new SMSG_GOSSIP_POI into the gossip point and lets it go when it
/// has run out or been reached. True when the map has to be set again.
bool updateGossipPoint(game::GameHandler* gh) {
    if (!gh) return false;
    bool changed = false;
    const game::GossipPoi* newest = nullptr;
    size_t count = 0;
    for (const auto& p : gh->getGossipPois()) {
        if (p.questObjectiveIndex != -2) continue;   // a quest POI, not a gossip point
        newest = &p;
        ++count;
    }
    const auto now = std::chrono::steady_clock::now();
    // A packet came when the list grew, or kept its length (it is capped)
    // with another entry at its end. A shorter list is entries taken away
    // (a quest's markers on its removal), which the client never sees.
    const bool differs = newest && (newest->x != gGossip.seenX || newest->y != gGossip.seenY ||
                                    newest->icon != gGossip.seenIcon ||
                                    newest->name != gGossip.seenName);
    const bool arrived = newest && (count > gGossip.seenCount ||
                                    (count == gGossip.seenCount && differs));
    gGossip.seenCount = count;
    if (newest) {
        gGossip.seenX = newest->x;
        gGossip.seenY = newest->y;
        gGossip.seenIcon = newest->icon;
        gGossip.seenName = newest->name;
    }
    if (arrived) {
        // The list holds Wowee's canonical order (x west); the map wants the
        // wire's (x north).
        gGossip.active = true;
        gGossip.x = newest->y;
        gGossip.y = newest->x;
        gGossip.mapId = static_cast<int>(gh->getCurrentMapId());
        gGossip.icon = static_cast<int>(newest->icon);
        gGossip.flags = newest->flags;
        gGossip.name = newest->name.substr(0, 63);
        gGossip.expires = now + std::chrono::seconds(detail::kGossipPoiSeconds);
        changed = true;
    }
    if (gGossip.active) {
        bool gone = now >= gGossip.expires;
        float px = 0, py = 0, pz = 0;
        if (!gone && playerPosition(gh, px, py, pz)) {
            const float dx = px - gGossip.x;
            const float dy = py - gGossip.y;
            gone = dx * dx + dy * dy < detail::kGossipPoiReachedSq;
        }
        if (gone) {
            gGossip.active = false;
            changed = true;
        }
    }
    return changed;
}

void fireWorldMapUpdate(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_lua_engine");
    auto* engine = static_cast<LuaEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (engine) engine->fireEvent(kWorldMapUpdate, {});
}

// ---------------------------------------------------------------------------
// CWorldMap
// ---------------------------------------------------------------------------

/// The WorldMapArea the map shows - 0x00543770.
int currentWma() {
    const State& s = gState;
    if (s.continent < 0) return s.area;
    const Continent& c = gData.continents[static_cast<size_t>(s.continent)];
    if (s.zone >= 0) return c.zones[static_cast<size_t>(s.zone)];
    return c.wmaId;
}

/// The dungeon floor showing: 0x00542F60, and the "accf10 == -1" pattern.
int currentDungeon() { return gState.area != -1 ? gState.dungeon : -1; }

/// WorldMapTransforms (0x00543920): a region of one map drawn on another. The
/// region is closed on all four sides.
bool transform(int mapId, float x, float y, int& outMap, float& outX, float& outY,
               int& outDungeon) {
    outMap = mapId;
    outX = x;
    outY = y;
    outDungeon = 0;
    for (const auto& t : gData.transforms) {
        if (t.mapId != mapId) continue;
        if (detail::closedRectContains(t.minY, t.minX, t.maxY, t.maxX, y, x)) {
            outMap = t.newMapId;
            outX = x + t.offsetX;
            outY = y + t.offsetY;
            outDungeon = t.newDungeonMapId;
            return true;
        }
    }
    return false;
}

/// DungeonMapChunk (0x005439E0): which floor a WMO group at a height is on.
int dungeonMapForPosition(int mapId, int wmoGroupId, float z) {
    if (mapId <= 0 || wmoGroupId <= 0) return -1;
    int result = -1;
    bool inMap = false, inGroup = false;
    for (const auto& c : gData.chunks) {
        if (c.mapId == mapId) {
            inMap = true;
            if (c.wmoGroupId == wmoGroupId) {
                inGroup = true;
                if (c.minZ <= z) result = c.dungeonMapId;
            } else if (inGroup) {
                return result;
            }
        } else if (inMap) {
            return result;
        }
    }
    return result;
}

/// 0x00544090: the WorldMapContinent row of a map, searched from the end.
const ContinentRow* worldMapContinentReverse(int mapId) {
    for (auto it = gData.wmContinents.rbegin(); it != gData.wmContinents.rend(); ++it) {
        if (it->mapId == mapId) return &*it;
    }
    return nullptr;
}
const ContinentRow* worldMapContinent(int mapId) {
    for (const auto& c : gData.wmContinents) {
        if (c.mapId == mapId) return &c;
    }
    return nullptr;
}

/// 0x00544140, see the header.
bool worldToMapImpl(int wmoGroupId, float px, float py, float pz, int mapId, float& u,
                    float& v, bool* hidden, bool noClamp, int dungeonOverride) {
    u = 0.0f;
    v = 0.0f;
    if (hidden) *hidden = false;
    const State& s = gState;
    int newMap = mapId, newDungeon = 0;
    float tx = px, ty = py;
    transform(mapId, px, py, newMap, tx, ty, newDungeon);

    if (const ContinentRow* wc = worldMapContinentReverse(newMap); wc && s.continent == -1) {
        if (hidden && newMap == 530) *hidden = true;
        u = (wc->offsetX * kWorldMapPerTileX + 0.5f) - ty * kWorldMapPerYardX * wc->scale;
        v = (wc->offsetY * kWorldMapPerTileY + 0.5f) - tx * kWorldMapPerYardY * wc->scale;
        return false;
    }
    int wmaId = s.area;
    if (s.continent >= 0) {
        if (s.continent >= static_cast<int>(gData.continents.size())) return false;
        const Continent& c = gData.continents[static_cast<size_t>(s.continent)];
        if (newMap != c.mapId) return false;
        wmaId = s.zone < 0 ? c.wmaId : c.zones[static_cast<size_t>(s.zone)];
    }
    const WorldMapAreaRow* row = wmaId == -1 ? nullptr : gData.wmaRow(wmaId);
    if (!row) return false;
    if (row->mapId != mapId && row->mapId != newMap) return false;

    // A continent is drawn in transformed space; a zone in its own.
    const float lx = s.zone < 0 ? tx : px;
    const float ly = s.zone < 0 ? ty : py;

    const int floor = dungeonOverride >= 1 ? dungeonOverride
                                           : dungeonMapForPosition(mapId, wmoGroupId, pz);
    const int showing = currentDungeon();
    const DungeonMapRow* dm = nullptr;
    if (floor == showing) {
        dm = gData.dungeonMap(floor);
    } else if (newDungeon == showing) {
        dm = gData.dungeonMap(newDungeon);
    } else if (showing > 0) {
        return false;   // on another floor than the one showing
    }
    const float hiY = dm ? dm->y1 : row->left;
    const float loY = dm ? dm->y0 : row->right;
    u = ((ly - loY) == 0.0f || (hiY - loY) == 0.0f) ? 0.0f : 1.0f - (ly - loY) / (hiY - loY);
    const float hiX = dm ? dm->x1 : row->top;
    const float loX = dm ? dm->x0 : row->bottom;
    v = ((lx - loX) == 0.0f || (hiX - loX) == 0.0f) ? 0.0f : 1.0f - (lx - loX) / (hiX - loX);
    if (!noClamp) detail::clampToMap(u, v);
    return true;
}

/// Where an AreaPOI lands on the map showing, or nowhere - 0x005445F0.
void poiPosition(const PoiRow& p, int wmoGroupId, float& u, float& v) {
    u = 0.0f;
    v = 0.0f;
    const State& s = gState;
    const bool a = ((s.continent < 0 || s.zone < 0) && s.continent != -2) || (p.flags & 4) != 0;
    const bool b = s.continent != -1 || (p.flags & 0x10) != 0;
    const bool c = s.zone >= 0 || s.area >= 0 || (p.flags & 8) != 0;
    if (!(a && b && c)) return;
    if ((p.flags & 0x400) != 0) {
        // 0x00543860: the zone of a continent showing - none on a continent
        // map, the world view or an instance/dungeon view.
        int zoneWma = -1;
        if (s.continent >= 0 && s.continent < static_cast<int>(gData.continents.size())) {
            const Continent& c = gData.continents[static_cast<size_t>(s.continent)];
            if (s.zone >= 0 && s.zone < static_cast<int>(c.zones.size()))
                zoneWma = c.zones[static_cast<size_t>(s.zone)];
        }
        const WorldMapAreaRow* row = gData.wmaRow(zoneWma);
        if (!row || row->areaId != p.areaId || row->mapId != p.mapId) return;
    }
    bool hidden = false;
    worldToMapImpl(wmoGroupId, p.x, p.y, 0.0f, p.mapId, u, v, &hidden, false, 0);
    if (hidden) {
        u = 0.0f;
        v = 0.0f;
    }
}

/// CWorldMap::SetMap, 0x00546310. Every change of view comes through here.
void setMap(lua_State* L, int continent, int zone, int dungeon) {
    game::GameHandler* gh = getGameHandler(L);
    State& s = gState;
    const int numContinents = static_cast<int>(gData.continents.size());
    bool redirected = false;
    if (continent >= 0 && continent < numContinents) {
        const Continent& c = gData.continents[static_cast<size_t>(continent)];
        const bool zoneValid = zone >= 0 && zone < static_cast<int>(c.zones.size());
        const WorldMapAreaRow* row = zoneValid ? gData.wmaRow(c.zones[static_cast<size_t>(zone)])
                                               : nullptr;
        if (row && row->defaultDungeonFloor >= 1) {
            // A zone that is really a set of dungeon floors (Dalaran): shown
            // as its floor, off the continent.
            zone = row->id;
            continent = -2;
            dungeon = row->defaultDungeonFloor;
            redirected = true;
        } else {
            s.zone = zoneValid ? zone : -1;
            s.area = -1;
            s.dungeon = -1;
        }
    } else {
        redirected = true;
    }
    if (redirected) {
        s.zone = -1;
        s.dungeon = dungeon;
        s.area = continent == -2 ? zone : -1;
    }
    s.continent = continent;
    s.initialised = true;

    const int wmo = playerWmoGroupId(L);
    const bool havePlayer = gh && gh->getPlayerGuid() != 0;

    // Landmarks: every AreaPOI that lands on this map, is in an area the
    // player has found, and whose world state (if it has one) is set.
    s.landmarks.clear();
    for (const auto& p : gData.pois) {
        float u = 0.0f, v = 0.0f;
        poiPosition(p, wmo, u, v);
        if (std::fabs(u) < kEpsilon && std::fabs(v) < kEpsilon) continue;
        if (p.areaId >= 1) {
            const AreaRow* a = gData.area(p.areaId);
            if (!a || !havePlayer) continue;
            if (a->explorationLevel >= 0 && !explored(gh, a->areaBit)) continue;
        }
        if (p.worldStateId != 0 && worldStateValue(gh, p.worldStateId) == 0) continue;
        s.landmarks.push_back({p.id, u, v, false});
    }
    // The gossip point, last, whatever the map's flags: placed straight
    // through 0x00544140 and dropped only when off the map or hidden.
    if (gGossip.active && (std::fabs(gGossip.x) >= kEpsilon || std::fabs(gGossip.y) >= kEpsilon)) {
        float u = 0.0f, v = 0.0f;
        bool hidden = false;
        worldToMapImpl(wmo, gGossip.x, gGossip.y, 0.0f, gGossip.mapId, u, v, &hidden, false, 0);
        if (!hidden && (std::fabs(u) >= kEpsilon || std::fabs(v) >= kEpsilon)) {
            s.landmarks.push_back({0, u, v, true});
        }
    }

    // Overlays: the explored-area art of the zone showing.
    s.overlays.clear();
    if (havePlayer && continent != -2 && (s.zone >= 0 || s.area > 0)) {
        const int wmaId = currentWma();
        for (const auto& o : gData.overlays) {
            if (o.mapAreaId != wmaId) continue;
            for (int areaId : o.areaIds) {
                if (areaId == 0) continue;
                const AreaRow* a = gData.area(areaId);
                if (!a) continue;
                if (a->explorationLevel >= 0 && !explored(gh, a->areaBit)) continue;
                s.overlays.push_back(o.id);
                break;
            }
        }
    }
    s.exploredHash = exploredHash(gh);
    fireWorldMapUpdate(L);
}

/// 0x0050F490: SetMap again with what is showing, after the data behind it moved.
void refresh(lua_State* L) {
    const State& s = gState;
    if (s.area == -1) setMap(L, s.continent, s.zone, -1);
    else setMap(L, s.continent, s.area, s.dungeon);
}

/// The zone under a point of the map showing, from the continent's grid -
/// 0x00543E50.
int zoneAtPoint(float x, float y) {
    const State& s = gState;
    const int wmaId = currentWma();
    if (wmaId == -1 || s.continent == -2) return 0;
    const WorldMapAreaRow* row = gData.wmaRow(wmaId);
    if (!row || s.continent < 0) return 0;
    int m = 0, dn = 0;
    float topX = 0, leftY = 0, bottomX = 0, rightY = 0;
    transform(row->mapId, row->top, row->left, m, topX, leftY, dn);
    transform(row->mapId, row->bottom, row->right, m, bottomX, rightY, dn);
    // Clamped the client's way round: x tested against 1 first and kept
    // when NaN, y tested "below 1" first and so made 1 when NaN.
    if (x >= 1.0f) x = 1.0f; else if (x < 0.0f) x = 0.0f;
    if (!(y < 1.0f)) y = 1.0f; else if (y < 0.0f) y = 0.0f;
    if ((leftY - rightY) == 0.0f || (topX - bottomX) == 0.0f) return 0;
    const float col = 0.5f - (rightY + (1.0f - x) * (leftY - rightY)) * kWorldToGrid;
    const float rowF = 0.5f - ((1.0f - y) * (topX - bottomX) + bottomX) * kWorldToGrid;
    if (col < 0.0f || col > 1.0f || rowF < 0.0f || rowF > 1.0f) return 0;
    const Continent& c = gData.continents[static_cast<size_t>(s.continent)];
    if (c.grid.empty()) return 0;
    return c.grid[static_cast<size_t>(detail::zmpCell(rowF, col))];
}

/// What the cursor is over, for the highlight - 0x00544420.
int hoveredZone(float x, float y) {
    const State& s = gState;
    if (s.continent == -2) return 0;
    if (s.continent == -1) {
        for (const auto& c : gData.continents) {
            if (c.worldMapId != 0 &&
                detail::closedRectContains(c.rectLeft, c.rectTop, c.rectRight, c.rectBottom, x, y)) {
                return c.wmaId;
            }
        }
        return 0;
    }
    const int z = zoneAtPoint(x, y);
    if (s.zone >= 0 &&
        gData.continents[static_cast<size_t>(s.continent)].zones[static_cast<size_t>(s.zone)] == z) {
        return 0;
    }
    return z;
}

int zoneIndexOn(const Continent& c, int wmaId) {
    for (size_t i = 0; i < c.zones.size(); ++i) {
        if (c.zones[i] == wmaId) return static_cast<int>(i);
    }
    return -1;
}

int continentIndexOfMap(int mapId) {
    for (size_t i = 0; i < gData.continents.size(); ++i) {
        if (gData.continents[i].mapId == mapId) return static_cast<int>(i);
    }
    return -1;
}

/// SetMapToCurrentZone, 0x00547170 with its diagnostic off.
void setMapToCurrentZoneImpl(lua_State* L) {
    game::GameHandler* gh = getGameHandler(L);
    float px = 0, py = 0, pz = 0;
    if (!playerPosition(gh, px, py, pz)) {
        setMap(L, -1, -1, -1);
        return;
    }
    const int mapId = static_cast<int>(gh->getCurrentMapId());
    const int wmo = playerWmoGroupId(L);
    int newMap = mapId, overrideDungeon = 0;
    float tx = px, ty = py;
    transform(mapId, px, py, newMap, tx, ty, overrideDungeon);
    int cont = continentIndexOfMap(newMap);

    if (cont < 0) {
        // An instance, a battleground, an arena: its own WorldMapArea.
        for (int orphan : gData.orphans) {
            const WorldMapAreaRow* row = gData.wmaRow(orphan);
            if (!row || row->mapId != mapId) continue;
            // The floor under the player; failing that the transform's
            // floor; failing that whatever the floor lookup said (-1).
            int floor = dungeonMapForPosition(mapId, wmo, pz);
            if (floor < 1 && overrideDungeon != 0) floor = overrideDungeon;
            setMap(L, -2, row->id, floor);
            return;
        }
        // An instance with no map of its own: the zone its entrance is in.
        const MapRow* mrow = gData.map(newMap);
        bool placed = false;
        if (mrow && (std::fabs(mrow->corpseX) >= kEpsilon || std::fabs(mrow->corpseY) >= kEpsilon)) {
            int cmap = mrow->corpseMapId, cd = 0;
            float cx = mrow->corpseX, cy = mrow->corpseY;
            transform(mrow->corpseMapId, mrow->corpseX, mrow->corpseY, cmap, cx, cy, cd);
            const int ci = continentIndexOfMap(cmap);
            if (ci >= 0) {
                cont = ci;
                const float fr = 0.5f - cx * kWorldToGrid;
                const float fc = 0.5f - cy * kWorldToGrid;
                if (fr >= 0.0f && fr <= 1.0f && fc >= 0.0f && fc <= 1.0f) {
                    const Continent& c = gData.continents[static_cast<size_t>(ci)];
                    if (!c.grid.empty()) {
                        const int w = c.grid[static_cast<size_t>(detail::zmpCell(fr, fc))];
                        const int zi = zoneIndexOn(c, w);
                        if (zi >= 0) {
                            setMap(L, ci, zi, -1);
                            return;
                        }
                    }
                }
                placed = true;
            }
        }
        if (!placed) {
            // "True Orphan": 0x005477BD sets (-2, -1, -1) - no continent, no
            // area - not the world view.
            setMap(L, -2, -1, -1);
            return;
        }
    }

    // On a continent: the zone the player is in, or its parent.
    const Continent& c = gData.continents[static_cast<size_t>(cont)];
    const int zoneId = playerZoneId(L, gh);
    int selected = -1;
    for (size_t j = 0; j < c.zones.size(); ++j) {
        const WorldMapAreaRow* row = gData.wmaRow(c.zones[j]);
        if (!row || row->areaId != zoneId) continue;
        if (row->hasBounds()) {
            selected = static_cast<int>(j);
            break;
        }
        const int floor = dungeonMapForPosition(mapId, wmo, pz);
        if (floor >= 0) {
            setMap(L, -2, row->id, floor);
            return;
        }
        if (row->parentWorldMapId != 0) {
            const int pi = zoneIndexOn(c, row->parentWorldMapId);
            if (pi >= 0) selected = pi;
        }
    }
    setMap(L, cont, selected, -1);
}

/// SetMap happens once by itself in the client: when the player's zone is
/// first known (0x005204C0). Here, the first time the map is asked anything
/// once the player is in the world.
bool ensureReady(lua_State* L) {
    game::GameHandler* gh = getGameHandler(L);
    if (!loadData(gh)) return false;
    if (!gState.initialised && gh && gh->getPlayerGuid() != 0) {
        updateGossipPoint(gh);
        setMapToCurrentZoneImpl(L);
        return true;
    }
    // New ground or a landmark's world state: the overlays and landmarks
    // change (0x005470D0 / 0x005285F0 -> 0x0050F490), and so does the gossip
    // point when it comes, runs out or is reached (0x007F4870, 0x007F5BA0).
    // Looked at four times a
    // second rather than on every call - the map makes dozens a frame.
    using Clock = std::chrono::steady_clock;
    static Clock::time_point checkedAt{};
    const auto now = Clock::now();
    if (gState.initialised && gh && now - checkedAt > std::chrono::milliseconds(250)) {
        checkedAt = now;
        const bool gossipChanged = updateGossipPoint(gh);
        if (gossipChanged || exploredHash(gh) != gState.exploredHash) refresh(L);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Lua
// ---------------------------------------------------------------------------

/// Bound with its fallback (worldmap::fallback, lua_system_api.cpp) as upvalue
/// 1, which answers instead while the client data is not loaded.
#define WM_FALLBACK(L)                                                   \
    do {                                                                 \
        if (!ensureReady(L)) {                                           \
            if (lua_isnil(L, lua_upvalueindex(1))) return 0;             \
            const int n = lua_gettop(L);                                 \
            lua_pushvalue(L, lua_upvalueindex(1));                       \
            lua_insert(L, 1);                                            \
            lua_call(L, n, LUA_MULTRET);                                 \
            return lua_gettop(L);                                        \
        }                                                                \
    } while (0)

int pushStringOrNil(lua_State* L, const std::string* s) {
    if (s) lua_pushstring(L, s->c_str());
    else lua_pushnil(L);
    return 1;
}

/// GetMapContinents() - 0x00544B20.
int lua_GetMapContinents(lua_State* L) {
    WM_FALLBACK(L);
    lua_checkstack(L, static_cast<int>(gData.continents.size()) + 4);
    for (const auto& c : gData.continents) {
        const MapRow* m = gData.map(c.mapId);
        pushStringOrNil(L, m ? &m->name : nullptr);
    }
    return static_cast<int>(gData.continents.size());
}

/// GetMapZones(continentIndex) - 0x00544B90.
int lua_GetMapZones(lua_State* L) {
    WM_FALLBACK(L);
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GetMapZones(continentIndex)");
    const int idx = static_cast<int>(static_cast<unsigned>(luaRound(lua_tonumber(L, 1))) - 1u);
    if (idx < 0 || idx >= static_cast<int>(gData.continents.size())) return 0;
    const Continent& c = gData.continents[static_cast<size_t>(idx)];
    lua_checkstack(L, static_cast<int>(c.zones.size()) + 4);
    for (int z : c.zones) {
        const WorldMapAreaRow* row = gData.wmaRow(z);
        const AreaRow* a = row ? gData.area(row->areaId) : nullptr;
        pushStringOrNil(L, a ? &a->name : nullptr);
    }
    return static_cast<int>(c.zones.size());
}

/// SetMapZoom(continentIndex [, zoneIndex]) - 0x005469E0.
int lua_SetMapZoom(lua_State* L) {
    WM_FALLBACK(L);
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: SetMapZoom(continentIndex [,zoneIndex])");
    const int c = static_cast<int>(static_cast<unsigned>(luaTruncate(lua_tonumber(L, 1))) - 1u);
    if (c < -2 || c >= static_cast<int>(gData.continents.size())) {
        return luaL_error(L, "Usage: SetMapZoom(continentIndex [,zoneIndex]) .. requested continent out of bounds");
    }
    int z = -1;
    if (lua_isnumber(L, 2)) z = static_cast<int>(static_cast<unsigned>(luaTruncate(lua_tonumber(L, 2))) - 1u);
    setMap(L, c, z, -1);
    return 0;
}

/// SetMapToCurrentZone() - 0x00547C10.
int lua_SetMapToCurrentZone(lua_State* L) {
    WM_FALLBACK(L);
    setMapToCurrentZoneImpl(L);
    return 0;
}

/// GetCurrentMapContinent() - 0x00544D40. A dungeon floor answers the
/// continent its map is on (Dalaran: Northrend).
int lua_GetCurrentMapContinent(lua_State* L) {
    WM_FALLBACK(L);
    if (const DungeonMapRow* dm = gData.dungeonMap(currentDungeon())) {
        const int ci = continentIndexOfMap(dm->mapId);
        if (ci >= 0) {
            lua_pushnumber(L, ci + 1);
            return 1;
        }
    }
    lua_pushnumber(L, gState.continent + 1);
    return 1;
}

/// GetCurrentMapZone() - 0x00544E80.
int lua_GetCurrentMapZone(lua_State* L) {
    WM_FALLBACK(L);
    const State& s = gState;
    if (s.zone < 0) {
        const int wmaId = s.continent >= 0
            ? gData.continents[static_cast<size_t>(s.continent)].wmaId : s.area;
        if (const WorldMapAreaRow* row = gData.wmaRow(wmaId)) {
            for (const auto& c : gData.continents) {
                if (c.mapId != row->mapId) continue;
                const int zi = zoneIndexOn(c, row->id);
                if (zi >= 0) {
                    lua_pushnumber(L, zi + 1);
                    return 1;
                }
            }
        }
    }
    lua_pushnumber(L, s.zone + 1);
    return 1;
}

/// GetCurrentMapAreaID() - 0x00544E10: the WorldMapArea id plus one.
int lua_GetCurrentMapAreaID(lua_State* L) {
    WM_FALLBACK(L);
    lua_pushnumber(L, currentWma() + 1);
    return 1;
}

/// SetMapByID(worldMapAreaId) - 0x00546C50.
int lua_SetMapByID(lua_State* L) {
    WM_FALLBACK(L);
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: SetMapByID(mapID)");
    const int id = luaTruncate(lua_tonumber(L, 1));
    const WorldMapAreaRow* row = gData.wmaRow(id);
    if (!row) return 0;
    for (size_t i = 0; i < gData.continents.size(); ++i) {
        const Continent& c = gData.continents[i];
        if (c.mapId != row->effectiveMapId()) continue;
        if (row->defaultDungeonFloor > 0) {
            setMap(L, -2, row->id, row->defaultDungeonFloor);
            lua_pushboolean(L, 1);
            return 1;
        }
        if (row->areaId == 0) {
            setMap(L, static_cast<int>(i), -1, -1);
            lua_pushboolean(L, 1);
            return 1;
        }
        const int zi = zoneIndexOn(c, row->id);
        if (zi >= 0) {
            setMap(L, static_cast<int>(i), zi, -1);
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    if (row->defaultDungeonFloor == -1) {
        setMap(L, -2, id, -1);
        lua_pushboolean(L, 1);
        return 1;
    }
    for (const auto& dm : gData.dungeonMaps) {
        if (dm.mapId == row->mapId && dm.floor == 1) {
            setMap(L, -2, id, dm.id);
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    setMap(L, -2, id, -1);
    lua_pushboolean(L, 1);
    return 1;
}

/// The "texture height" GetMapInfo returns - 0x00543CD0.
int mapTextureHeight() {
    const State& s = gState;
    const WorldMapAreaRow* row = gData.wmaRow(currentWma());
    if (!row) return 0;
    float w = 0, h = 0;
    if (const DungeonMapRow* dm = gData.dungeonMap(currentDungeon())) {
        w = dm->y1 - dm->y0;
        h = dm->x1 - dm->x0;
    } else if (s.zone == -1) {
        const ContinentRow* wc = worldMapContinent(row->mapId);
        if (!wc) return 0;
        w = static_cast<float>(wc->right - wc->left + 1);
        h = static_cast<float>(wc->bottom - wc->top + 1);
    } else {
        w = row->left - row->right;
        h = row->top - row->bottom;
    }
    if (w == 0.0f || h == 0.0f) return 0;
    return roundNearest((h * kMapPixelsX) / w);
}

using detail::lowBytePow2;

/// GetMapInfo() - 0x00544CA0: mapFileName, textureHeight, and that height
/// with its low byte rounded up to a power of two.
int lua_GetMapInfo(lua_State* L) {
    WM_FALLBACK(L);
    const WorldMapAreaRow* row = gData.wmaRow(currentWma());
    pushStringOrNil(L, row ? &row->name : nullptr);
    const int h = mapTextureHeight();
    lua_pushnumber(L, static_cast<unsigned>(h));
    lua_pushnumber(L, lowBytePow2(static_cast<unsigned>(h)));
    return 3;
}

/// GetNumMapOverlays() - 0x00543060.
int lua_GetNumMapOverlays(lua_State* L) {
    WM_FALLBACK(L);
    lua_pushnumber(L, static_cast<lua_Number>(gState.overlays.size()));
    return 1;
}

/// GetMapOverlayInfo(index) - 0x00545C80.
int lua_GetMapOverlayInfo(lua_State* L) {
    WM_FALLBACK(L);
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GetMapOverlayInfo(index)");
    const int idx = static_cast<int>(static_cast<unsigned>(luaRound(lua_tonumber(L, 1))) - 1u);
    const OverlayRow* o = (idx >= 0 && idx < static_cast<int>(gState.overlays.size()))
        ? gData.overlay(gState.overlays[static_cast<size_t>(idx)]) : nullptr;
    if (!o) {
        lua_pushnil(L);
        for (int i = 0; i < 6; ++i) lua_pushnumber(L, 0);
        return 7;
    }
    std::string path;
    if (!o->texture.empty()) {
        const WorldMapAreaRow* row = gData.wmaRow(o->mapAreaId);
        path = row ? "Interface\\WorldMap\\" + row->name + "\\" + o->texture
                   : "Interface\\WorldMap\\World\\" + o->texture;
    }
    lua_pushstring(L, path.c_str());
    lua_pushnumber(L, o->width);
    lua_pushnumber(L, o->height);
    lua_pushnumber(L, o->offsetX);
    lua_pushnumber(L, o->offsetY);
    lua_pushnumber(L, o->mapPointX);
    lua_pushnumber(L, o->mapPointY);
    return 7;
}

/// GetNumMapLandmarks() - 0x00543020.
int lua_GetNumMapLandmarks(lua_State* L) {
    WM_FALLBACK(L);
    lua_pushnumber(L, static_cast<lua_Number>(gState.landmarks.size()));
    return 1;
}

/// GetMapLandmarkInfo(index) - 0x00545A30: name, description, textureIndex,
/// x, y, mapLinkID, showInBattleMap.
int lua_GetMapLandmarkInfo(lua_State* L) {
    WM_FALLBACK(L);
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GetMapLandmarkInfo(index)");
    const int idx = static_cast<int>(static_cast<unsigned>(luaRound(lua_tonumber(L, 1))) - 1u);
    const Landmark* lm = (idx >= 0 && idx < static_cast<int>(gState.landmarks.size()))
        ? &gState.landmarks[static_cast<size_t>(idx)] : nullptr;
    if (lm && lm->gossip) {
        // The gossip point's record: its name, a null description, its icon
        // (slot 0 - it has no world state), no map link, and the packet's
        // flags for showInBattleMap (0x200).
        lua_pushstring(L, gGossip.name.c_str());
        lua_pushnil(L);
        lua_pushnumber(L, gGossip.icon);
        lua_pushnumber(L, lm->u);
        lua_pushnumber(L, lm->v);
        lua_pushnil(L);
        lua_pushboolean(L, (gGossip.flags & 0x200) != 0);
        return 7;
    }
    const PoiRow* p = lm ? gData.poi(lm->poiId) : nullptr;
    if (!p) {
        lua_pushnil(L);
        lua_pushnil(L);
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        lua_pushnil(L);
        lua_pushnil(L);
        return 7;
    }
    game::GameHandler* gh = getGameHandler(L);
    lua_pushstring(L, p->name.c_str());
    lua_pushstring(L, p->description.c_str());
    int texture = 0;
    const State& s = gState;
    if ((p->flags & 0x80) != 0 || (s.zone == -1 && s.continent != -2)) {
        const unsigned k = detail::landmarkIconSlot(
            p->worldStateId != 0, p->worldStateId != 0 ? worldStateValue(gh, p->worldStateId) : 0);
        texture = p->icons[k];
    }
    lua_pushnumber(L, texture);
    lua_pushnumber(L, lm->u);
    lua_pushnumber(L, lm->v);
    if (p->worldMapLink != 0) lua_pushnumber(L, p->worldMapLink);
    else lua_pushnil(L);
    lua_pushboolean(L, (p->flags & 0x200) != 0);
    return 7;
}

/// ProcessMapClick(x, y) - 0x00546E80 / 0x005468F0. A click on the world map
/// opens the continent under it; anywhere else the zone under it.
int lua_ProcessMapClick(lua_State* L) {
    WM_FALLBACK(L);
    if (!lua_isnumber(L, 1) || !lua_isnumber(L, 2)) return luaL_error(L, "Usage: ProcessMapClick(x, y)");
    const float x = static_cast<float>(lua_tonumber(L, 1));
    const float y = static_cast<float>(lua_tonumber(L, 2));
    const State& s = gState;
    if (s.continent == -2) return 0;
    if (s.continent == -1) {
        for (size_t i = 0; i < gData.continents.size(); ++i) {
            const Continent& c = gData.continents[i];
            if (c.worldMapId != 0 &&
                detail::closedRectContains(c.rectLeft, c.rectTop, c.rectRight, c.rectBottom, x, y)) {
                setMap(L, static_cast<int>(i), -1, -1);
                return 0;
            }
        }
        return 0;
    }
    const int w = zoneAtPoint(x, y);
    if (w == 0) return 0;
    const int cont = s.continent;
    const int zi = zoneIndexOn(gData.continents[static_cast<size_t>(cont)], w);
    if (zi >= 0) setMap(L, cont, zi, -1);
    return 0;
}

using detail::highlightTextureSize;

/// UpdateMapHighlight(x, y) - 0x00545110: name, fileName, texPercentageX,
/// texPercentageY, textureX, textureY, scrollChildX, scrollChildY.
int lua_UpdateMapHighlight(lua_State* L) {
    WM_FALLBACK(L);
    if (!lua_isnumber(L, 1) || !lua_isnumber(L, 2)) return luaL_error(L, "Usage: UpdateMapHighlight(x, y)");
    const float x = static_cast<float>(lua_tonumber(L, 1));
    const float y = static_cast<float>(lua_tonumber(L, 2));
    const State& s = gState;
    auto nameOnly = [L](const std::string* name) {
        pushStringOrNil(L, name);
        lua_pushnil(L);
        for (int i = 0; i < 6; ++i) lua_pushnumber(L, 0);
        return 8;
    };

    // On a zone map, an explored overlay under the cursor names its area.
    if (s.zone >= 0) {
        for (int oid : s.overlays) {
            const OverlayRow* o = gData.overlay(oid);
            if (!o) continue;
            const float left = static_cast<float>(o->hitLeft) * kInvMapPixelsX;
            const float right = static_cast<float>(o->hitRight) * kInvMapPixelsX;
            const float top = static_cast<float>(o->hitTop) * kInvMapPixelsY;
            const float bottom = static_cast<float>(o->hitBottom) * kInvMapPixelsY;
            if ((right - left) == 0.0f || (bottom - top) == 0.0f) continue;
            if (detail::closedRectContains(left, top, right, bottom, x, y)) {   // 0x00488D40
                const AreaRow* a = gData.area(o->areaIds[0]);
                if (a) return nameOnly(&a->name);
            }
        }
    }

    const int hovered = hoveredZone(x, y);
    const WorldMapAreaRow* row = hovered != 0 ? gData.wmaRow(hovered) : nullptr;
    if (!row) return nameOnly(nullptr);

    if (s.zone >= 0) {
        // Another zone on a zone map: its name, no art.
        const AreaRow* a = gData.area(row->areaId);
        return nameOnly(a ? &a->name : nullptr);
    }
    if (!row->hasBounds()) return nameOnly(nullptr);

    if (row->areaId == 0) {
        const MapRow* m = gData.map(row->mapId);
        pushStringOrNil(L, m ? &m->name : nullptr);
    } else {
        const AreaRow* a = gData.area(row->areaId);
        pushStringOrNil(L, a ? &a->name : nullptr);
    }
    lua_pushstring(L, row->name.c_str());

    float w = 64.0f, h = 64.0f;
    int size = 0x80;
    const ContinentRow* wc = nullptr;
    if (row->areaId == 0) {
        wc = worldMapContinent(row->mapId);
        if (wc) {
            w = static_cast<float>(wc->right - wc->left + 1);
            h = static_cast<float>(wc->bottom - wc->top + 1);
            size = truncate((512.0f * h) / w);
        }
    } else {
        w = row->left - row->right;
        h = row->top - row->bottom;
        size = truncate((128.0f * h) / w);
    }
    const unsigned texSize = highlightTextureSize(static_cast<unsigned>(size));

    if (row->areaId == 0) {
        // A continent on the world map.
        if (w <= h) {
            lua_pushnumber(L, w / h);
            lua_pushnumber(L, 1.0);
        } else {
            lua_pushnumber(L, 1.0);
            lua_pushnumber(L, h / w);
        }
        if (wc) {
            const float k = wc->scale * kTilesToCentre;
            const float sx = (kWorldMapHalfTilesX - k) + static_cast<float>(wc->left) * wc->scale + wc->offsetX;
            const float sy = static_cast<float>(wc->top) * wc->scale + (kWorldMapHalfTilesY - k) + wc->offsetY;
            lua_pushnumber(L, wc->scale * w * kWorldMapPerTileX);
            lua_pushnumber(L, wc->scale * h * kWorldMapPerTileY);
            lua_pushnumber(L, sx * kWorldMapPerTileX);
            lua_pushnumber(L, sy * kWorldMapPerTileY);
            return 8;
        }
        for (int i = 0; i < 4; ++i) lua_pushnumber(L, 0);
        return 8;
    }

    // A zone on its continent.
    lua_pushnumber(L, 1.0);
    lua_pushnumber(L, static_cast<float>(size) / static_cast<float>(texSize));
    for (const auto& cont : gData.wma) {
        if (cont.areaId != 0 || cont.effectiveMapId() != row->effectiveMapId()) continue;
        const float cw = cont.left - cont.right;
        const float ch = cont.top - cont.bottom;
        float zoneLeft = row->left, zoneTop = row->top;
        if (row->displayMapId >= 0) {
            int m = 0, dn = 0;
            transform(row->mapId, row->top, row->left, m, zoneTop, zoneLeft, dn);
        }
        lua_pushnumber(L, (1.0f / cw) * w);
        lua_pushnumber(L, (1.0f / ch) * h);
        lua_pushnumber(L, (1.0f / cw) * (cont.left - zoneLeft));
        lua_pushnumber(L, static_cast<double>(1.0f / ch) * static_cast<double>(cont.top - zoneTop));
        return 8;
    }
    for (int i = 0; i < 4; ++i) lua_pushnumber(L, 0);
    return 8;
}

/// ZoomOut() - 0x00546A90. Only from a dungeon floor or an instance map, to
/// the zone it belongs to; the zoom-out button handles everything else in Lua.
int lua_ZoomOut(lua_State* L) {
    WM_FALLBACK(L);
    const State& s = gState;
    int target = 0;
    if (s.area == -1 || s.dungeon < 1) {
        if (s.continent != -2) return 0;
        const WorldMapAreaRow* row = gData.wmaRow(currentWma());
        if (!row) return 0;
        target = row->parentWorldMapId;
        if (target == 0) {
            for (const auto& dm : gData.dungeonMaps) {
                if (dm.mapId == row->mapId && dm.floor == 1) target = dm.parentWorldMapId;
            }
            if (target == 0) return 0;
        }
    } else {
        const DungeonMapRow* dm = gData.dungeonMap(s.dungeon);
        if (!dm || dm->parentWorldMapId == 0) return 0;
        target = dm->parentWorldMapId;
    }
    const WorldMapAreaRow* trow = gData.wmaRow(target);
    if (!trow) return 0;
    for (size_t i = 0; i < gData.continents.size(); ++i) {
        const Continent& c = gData.continents[i];
        if (c.mapId != trow->mapId) continue;
        const int zi = zoneIndexOn(c, trow->id);
        if (zi >= 0) {
            setMap(L, static_cast<int>(i), zi, -1);
            return 0;
        }
    }
    return 0;
}

/// IsZoomOutAvailable() - 0x00545050.
int lua_IsZoomOutAvailable(lua_State* L) {
    WM_FALLBACK(L);
    bool available = true;
    if (!gData.dungeonMap(currentDungeon()) && gState.continent == -2) {
        const WorldMapAreaRow* row = gData.wmaRow(currentWma());
        if (!row) {
            available = false;
        } else if (row->defaultDungeonFloor != -1) {
            available = gData.wmaRow(row->parentWorldMapId) != nullptr;
        }
    }
    lua_pushboolean(L, available);
    return 1;
}

/// GetPlayerMapPosition(unit) - 0x00545880. The player, and party or raid
/// members (from their entity in range, or the group list's coarse position).
int lua_GetPlayerMapPosition(lua_State* L) {
    WM_FALLBACK(L);
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: GetPlayerMapPosition(\"player\")");
    game::GameHandler* gh = getGameHandler(L);
    float u = 0.0f, v = 0.0f;
    std::string unit = lua_tostring(L, 1);
    toLowerInPlace(unit);
    const uint64_t guid = gh ? resolveUnitGuid(gh, unit) : 0;
    bool inGroup = false;
    if (gh && guid != 0) {
        for (const auto& m : gh->getPartyData().members) {
            if (m.guid == guid) { inGroup = true; break; }
        }
    }
    if (gh && guid != 0 && (guid == gh->getPlayerGuid() || inGroup)) {
        float px = 0, py = 0, pz = 0;
        int mapId = static_cast<int>(gh->getCurrentMapId());
        bool have = false;
        if (guid == gh->getPlayerGuid()) {
            have = playerPosition(gh, px, py, pz);
        } else if (auto e = gh->getEntityManager().getEntity(guid)) {
            const glm::vec3 sp = core::coords::canonicalToServer(
                glm::vec3(e->getX(), e->getY(), e->getZ()));
            px = sp.x; py = sp.y; pz = sp.z;
            have = true;
        } else if (const auto* m = findPartyMember(gh, guid)) {
            // Out of range (0x005444F0): SMSG_PARTY_MEMBER_STATS' area and
            // int16 position, unless the member's status has 0x20 set. The
            // client does not look at "online" (0x01) here.
            if ((m->onlineStatus & 0x20) == 0) {
                const AreaRow* a = gData.area(m->zoneId);
                if (a) {
                    mapId = a->mapId;
                    px = static_cast<float>(m->posX);
                    py = static_cast<float>(m->posY);
                    pz = 0.0f;
                    have = true;
                }
            }
        }
        if (have) {
            bool hidden = false;
            worldToMapImpl(playerWmoGroupId(L), px, py, pz, mapId, u, v, &hidden, false, 0);
            if (hidden) { u = 0.0f; v = 0.0f; }
        }
    }
    lua_pushnumber(L, u);
    lua_pushnumber(L, v);
    return 2;
}

/// GetCorpseMapPosition() - 0x00545950.
int lua_GetCorpseMapPosition(lua_State* L) {
    WM_FALLBACK(L);
    game::GameHandler* gh = getGameHandler(L);
    float u = 0.0f, v = 0.0f;
    // 0x00523E80: the corpse's own map and position (MSG_CORPSE_QUERY), not
    // the map the player is on - a corpse on another continent still shows
    // on that continent's map and on the world map.
    if (gh && gh->corpsePositionValidRef()) {
        const glm::vec3 sp = core::coords::canonicalToServer(
            glm::vec3(gh->corpseYRef(), gh->corpseXRef(), gh->corpseZRef()));
        worldToMapImpl(playerWmoGroupId(L), sp.x, sp.y, sp.z,
                       static_cast<int>(gh->corpseMapIdRef()), u, v, nullptr, false, 0);
    }
    lua_pushnumber(L, u);
    lua_pushnumber(L, v);
    return 2;
}

/// GetDeathReleasePosition() - 0x005459C0.
int lua_GetDeathReleasePosition(lua_State* L) {
    WM_FALLBACK(L);
    game::GameHandler* gh = getGameHandler(L);
    float u = 0.0f, v = 0.0f;
    uint32_t mapId = 0;
    glm::vec3 canonical{0.0f};
    if (gh && gh->getDeathReleaseLocation(mapId, canonical)) {
        const glm::vec3 sp = core::coords::canonicalToServer(canonical);
        worldToMapImpl(playerWmoGroupId(L), sp.x, sp.y, sp.z, static_cast<int>(mapId), u, v,
                       nullptr, false, 0);
    }
    lua_pushnumber(L, u);
    lua_pushnumber(L, v);
    return 2;
}

/// GetNumDungeonMapLevels() - 0x00546290 / 0x00546190.
int lua_GetNumDungeonMapLevels(lua_State* L) {
    WM_FALLBACK(L);
    const State& s = gState;
    int count = 0;
    if (s.area >= 0) {
        bool same = true;
        if (s.continent >= 0) same = currentWma() == s.area;
        if (same) {
            if (const WorldMapAreaRow* row = gData.wmaRow(s.area)) {
                uint32_t seen = 0;
                for (const auto& dm : gData.dungeonMaps) {
                    if (dm.mapId != row->mapId) continue;
                    const uint32_t bit = 1u << (dm.floor & 31);
                    if ((seen & bit) == 0) {
                        seen |= bit;
                        ++count;
                    }
                }
                if (row->defaultDungeonFloor == -1) ++count;
            }
        }
    }
    lua_pushnumber(L, count);
    return 1;
}

/// GetCurrentMapDungeonLevel() - 0x00544FC0.
int lua_GetCurrentMapDungeonLevel(lua_State* L) {
    WM_FALLBACK(L);
    int level = 0;
    if (const DungeonMapRow* dm = gData.dungeonMap(currentDungeon())) level = dm->floor;
    if (const WorldMapAreaRow* row = gData.wmaRow(gState.area); row && row->defaultDungeonFloor == -1) {
        ++level;
    }
    lua_pushnumber(L, level);
    return 1;
}

/// SetDungeonMapLevel(level) - 0x00547B80 / 0x00547A70.
int lua_SetDungeonMapLevel(lua_State* L) {
    WM_FALLBACK(L);
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: SetDungeonMapLevel(level)");
    int level = luaTruncate(lua_tonumber(L, 1));
    State& s = gState;
    const WorldMapAreaRow* area = gData.wmaRow(s.area);
    if (area && area->defaultDungeonFloor == -1) {
        --level;
        if (level < 0) return 0;
    } else if (level < 1) {
        return 0;
    }
    if (s.area < 0) return 0;
    if (s.continent >= 0 && currentWma() != s.area) return 0;
    const WorldMapAreaRow* row = gData.wmaRow(s.area);
    if (!row) return 0;
    if (level == 0) {
        s.dungeon = -1;
        setMap(L, -2, row->id, -1);
        return 0;
    }
    bool inMap = false;
    for (const auto& dm : gData.dungeonMaps) {
        if (dm.mapId == row->mapId) {
            if (dm.floor == level) {
                if (dm.id < 1) return 0;
                s.dungeon = dm.id;
                refresh(L);
                return 0;
            }
            inMap = true;
        } else if (inMap) {
            return 0;
        }
    }
    return 0;
}

/// DungeonUsesTerrainMap() - 0x00544C40.
int lua_DungeonUsesTerrainMap(lua_State* L) {
    WM_FALLBACK(L);
    const WorldMapAreaRow* row = gData.wmaRow(gState.area);
    if (row && row->defaultDungeonFloor == -1) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}

/// ClickLandmark(mapLinkID) - 0x00546EF0: a landmark that opens a dungeon map.
int lua_ClickLandmark(lua_State* L) {
    WM_FALLBACK(L);
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: ClickLandmark(mapLinkID)");
    const int link = luaRound(lua_tonumber(L, 1));
    const DungeonMapRow* dm = gData.dungeonMap(link);
    if (!dm) return 0;
    for (int orphan : gData.orphans) {
        const WorldMapAreaRow* row = gData.wmaRow(orphan);
        if (row && row->mapId == dm->mapId) {
            setMap(L, -2, row->id, link);
            return 0;
        }
    }
    return 0;
}

struct Binding {
    const char* name;
    lua_CFunction fn;
    lua_CFunction fallback;   // upvalue 1, see WM_FALLBACK
};

const Binding kBindings[] = {
    {"GetMapContinents", lua_GetMapContinents, fallback::GetMapContinents},
    {"GetMapZones", lua_GetMapZones, fallback::GetMapZones},
    {"SetMapZoom", lua_SetMapZoom, fallback::SetMapZoom},
    {"SetMapToCurrentZone", lua_SetMapToCurrentZone, fallback::SetMapToCurrentZone},
    {"GetCurrentMapContinent", lua_GetCurrentMapContinent, fallback::GetCurrentMapContinent},
    {"GetCurrentMapZone", lua_GetCurrentMapZone, fallback::GetCurrentMapZone},
    {"GetCurrentMapAreaID", lua_GetCurrentMapAreaID, fallback::GetCurrentMapAreaID},
    {"SetMapByID", lua_SetMapByID, fallback::SetMapByID},
    {"GetMapInfo", lua_GetMapInfo, fallback::GetMapInfo},
    {"GetNumMapOverlays", lua_GetNumMapOverlays, fallback::GetNumMapOverlays},
    {"GetMapOverlayInfo", lua_GetMapOverlayInfo, fallback::GetMapOverlayInfo},
    {"GetNumMapLandmarks", lua_GetNumMapLandmarks, fallback::GetNumMapLandmarks},
    {"GetMapLandmarkInfo", lua_GetMapLandmarkInfo, fallback::GetMapLandmarkInfo},
    {"ProcessMapClick", lua_ProcessMapClick, fallback::ProcessMapClick},
    {"UpdateMapHighlight", lua_UpdateMapHighlight, fallback::UpdateMapHighlight},
    {"ZoomOut", lua_ZoomOut, fallback::ZoomOut},
    {"IsZoomOutAvailable", lua_IsZoomOutAvailable, fallback::IsZoomOutAvailable},
    {"GetPlayerMapPosition", lua_GetPlayerMapPosition, fallback::GetPlayerMapPosition},
    {"GetCorpseMapPosition", lua_GetCorpseMapPosition, fallback::GetCorpseMapPosition},
    {"GetDeathReleasePosition", lua_GetDeathReleasePosition, fallback::GetDeathReleasePosition},
    {"GetNumDungeonMapLevels", lua_GetNumDungeonMapLevels, fallback::GetNumDungeonMapLevels},
    {"GetCurrentMapDungeonLevel", lua_GetCurrentMapDungeonLevel,
     fallback::GetCurrentMapDungeonLevel},
    {"SetDungeonMapLevel", lua_SetDungeonMapLevel, fallback::SetDungeonMapLevel},
    {"DungeonUsesTerrainMap", lua_DungeonUsesTerrainMap, fallback::DungeonUsesTerrainMap},
    {"ClickLandmark", lua_ClickLandmark, fallback::ClickLandmark},
};

/// The player arrow. CreateWorldMapArrowFrame (0x00545E60 -> 0x00544750)
/// makes two CSimpleModel frames under the map, PlayerArrowFrame and
/// PlayerArrowEffectFrame, both showing Interface\Minimap\MinimapArrow.mdx;
/// PositionWorldMapArrowFrame (0x005430A0) anchors both, ShowWorldMapArrowFrame
/// (0x005434E0) shows or hides both, and UpdateWorldMapArrowFrames
/// (0x00545FE0 -> 0x005449F0) turns both to the player's facing.
///
/// The model is a flat arrow seen from above, so here it is the same arrow as
/// a texture, turned by its texture coordinates. Defined in Lua because all it
/// does is make and move frames; WorldMapFrame_OnLoad calls the first, and
/// WorldMapButton_OnUpdate the other three every frame.
constexpr const char* kArrowLua = R"LUA(
-- Kept here rather than looked up by name: with the missing-API fallback on,
-- an unset global answers with a stand-in that is not nil.
local arrows = {}
local function makeArrow(name, parent)
    local f = CreateFrame("Frame", name, parent)
    f:SetWidth(32)
    f:SetHeight(32)
    f:SetFrameLevel(199)
    local t = f:CreateTexture(name .. "Texture", "ARTWORK")
    t:SetAllPoints(f)
    t:SetTexture("Interface\\Minimap\\MinimapArrow")
    f.texture = t
    f:Hide()
    arrows[#arrows + 1] = f
    return f
end
function CreateWorldMapArrowFrame(parent)
    if type(parent) ~= "table" then
        error("Usage: CreateWorldMapArrowFrame(parent)", 2)
    end
    if #arrows > 0 then return end
    makeArrow("PlayerArrowFrame", parent)
    makeArrow("PlayerArrowEffectFrame", parent)
end
function PositionWorldMapArrowFrame(point, relativeTo, relativePoint, x, y)
    for _, f in ipairs(arrows) do
        f:ClearAllPoints()
        f:SetPoint(point, relativeTo, relativePoint or point, x or 0, y or 0)
    end
end
function ShowWorldMapArrowFrame(show)
    for _, f in ipairs(arrows) do
        if show then f:Show() else f:Hide() end
    end
end
function UpdateWorldMapArrowFrames()
    if #arrows == 0 then return end
    local a = GetPlayerFacing() or 0
    local c, s = math.cos(a), math.sin(a)
    -- The texture turned counter-clockwise by the facing: each corner
    -- samples the point the inverse rotation brings it from.
    local function uv(dx, dy) return 0.5 + c * dx - s * dy, 0.5 + s * dx + c * dy end
    local ulx, uly = uv(-0.5, -0.5)
    local llx, lly = uv(-0.5, 0.5)
    local urx, ury = uv(0.5, -0.5)
    local lrx, lry = uv(0.5, 0.5)
    for _, f in ipairs(arrows) do
        f.texture:SetTexCoord(ulx, uly, llx, lly, urx, ury, lrx, lry)
    end
end
)LUA";

}  // namespace

void registerWorldMapLuaAPI(lua_State* L) {
    for (const Binding& b : kBindings) {
        lua_pushcfunction(L, b.fallback);
        lua_pushcclosure(L, b.fn, 1);
        lua_setglobal(L, b.name);
    }
    if (luaL_dostring(L, kArrowLua) != 0) {
        LOG_WARNING("WorldMap: the player arrow functions failed to load: ",
                    lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
        lua_pop(L, 1);
    }
}

bool ready(lua_State* L) { return ensureReady(L); }

bool worldToMap(lua_State* L, float x, float y, float z, uint32_t mapId, int dungeonMapOverride,
                float& u, float& v, bool* hidden, bool noClamp) {
    u = 0.0f;
    v = 0.0f;
    if (!ensureReady(L)) return false;
    return worldToMapImpl(playerWmoGroupId(L), x, y, z, static_cast<int>(mapId), u, v, hidden,
                          noClamp, dungeonMapOverride);
}

int currentWorldMapAreaId(lua_State* L) {
    if (!ensureReady(L)) return -1;
    return currentWma();
}

int currentWorldMapAreaMapId(lua_State* L) {
    if (!ensureReady(L)) return -1;
    const WorldMapAreaRow* row = gData.wmaRow(currentWma());
    return row ? row->mapId : -1;
}

int currentDungeonMapId(lua_State* L) {
    if (!ensureReady(L)) return -1;
    return currentDungeon();
}

bool dungeonMapInfo(lua_State* L, int dungeonMapId, int& mapId, int& floorIndex) {
    if (!ensureReady(L)) return false;
    const DungeonMapRow* dm = gData.dungeonMap(dungeonMapId);
    if (!dm) return false;
    mapId = dm->mapId;
    floorIndex = dm->floor;
    return true;
}

void currentView(lua_State* L, int& continent, int& zone) {
    continent = -1;
    zone = -1;
    if (!ensureReady(L)) return;
    continent = gState.continent;
    zone = gState.zone;
}

void setMapToCurrentZone(lua_State* L) {
    if (!ensureReady(L)) return;
    setMapToCurrentZoneImpl(L);
}

int worldMapAreaMapId(lua_State* L, int worldMapAreaId) {
    if (!ensureReady(L)) return -1;
    const WorldMapAreaRow* row = gData.wmaRow(worldMapAreaId);
    return row ? row->mapId : -1;
}

float worldMapAreaWidth(lua_State* L, int worldMapAreaId) {
    if (!ensureReady(L)) return 0.0f;
    const WorldMapAreaRow* row = gData.wmaRow(worldMapAreaId);
    return row ? row->left - row->right : 0.0f;
}

int worldMapAreaForZone(lua_State* L, int areaId) {
    if (!ensureReady(L)) return 0;
    for (int guard = 0; guard < 8; ++guard) {
        const AreaRow* a = gData.area(areaId);
        if (!a || a->parent == 0) break;
        areaId = a->parent;
    }
    for (const auto& row : gData.wma) {
        if (row.areaId == areaId) return row.id;
    }
    return 0;
}

}  // namespace wowee::addons::worldmap
