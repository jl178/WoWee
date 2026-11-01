// world_map_api.hpp - the C side of the stock world map, for FrameXML.
//
// The 3.3.5a client draws its world map entirely in FrameXML
// (WorldMapFrame.lua/.xml, QuestPOI, WatchFrame) and keeps only the state and
// the arithmetic in C: which continent, zone and dungeon floor the map shows,
// how a world position lands on it, what was clicked, what is highlighted and
// which overlays, landmarks and quest markers belong on it. This is that C
// side, rebuilt from Wow.exe 3.3.5a (12340); each function in the .cpp names
// the handler it was rebuilt from and what it does.
//
// Positions here are in the client's own world order, which is also the
// order on the wire: x is the north axis (server X), y the west axis (server
// Y). Wowee's "canonical" vectors have the two swapped, so callers convert
// with canonicalToServer first.
#pragma once

#include <cstdint>
#include <vector>

struct lua_State;

namespace wowee::addons::worldmap {

/// Registers every map function over whatever earlier code bound to the same
/// names. Called once, after the other API sets.
void registerWorldMapLuaAPI(lua_State* L);

/// Whether the client data loaded and the functions below answer.
bool ready(lua_State* L);

/// CWorldMap's world-to-map conversion (Wow.exe 0x00544140) against the map
/// now showing. x, y, z in the client's world order (see above). u, v are
/// fractions across and down the map; both zero when the point is off it.
/// `hidden` is set when the point is on a map the view cannot show (Outland
/// on the Azeroth world map). Returns true when the point was placed on a
/// zone/continent/dungeon map, false for the world view or a miss - the same
/// distinction the client's callers make.
bool worldToMap(lua_State* L, float x, float y, float z, uint32_t mapId,
                int dungeonMapOverride, float& u, float& v, bool* hidden = nullptr,
                bool noClamp = false);

/// WorldMapArea id the map shows, -1 for none (world/cosmic view).
int currentWorldMapAreaId(lua_State* L);
/// That area's MapID, or -1.
int currentWorldMapAreaMapId(lua_State* L);
/// The DungeonMap row id of the floor showing, -1 for none.
int currentDungeonMapId(lua_State* L);
/// The DungeonMap row's (MapID, FloorIndex) for an id, false if no such row.
bool dungeonMapInfo(lua_State* L, int dungeonMapId, int& mapId, int& floorIndex);
/// The client's map continent / zone indices (0-based, -1/-2 as stored).
void currentView(lua_State* L, int& continent, int& zone);

/// SetMapToCurrentZone (Wow.exe 0x00547170) - also used by
/// GetQuestWorldMapAreaID, which the client implements by moving the map.
void setMapToCurrentZone(lua_State* L);

/// A WorldMapArea row's MapID, -1 when there is no such row.
int worldMapAreaMapId(lua_State* L, int worldMapAreaId);
/// A WorldMapArea row's LocLeft - LocRight (its width in yards), 0 if none.
float worldMapAreaWidth(lua_State* L, int worldMapAreaId);
/// The first WorldMapArea row for an AreaTable id taken up to its zone, 0 if none.
int worldMapAreaForZone(lua_State* L, int areaId);

}  // namespace wowee::addons::worldmap
