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

#include <cmath>
#include <cstdint>
#include <vector>

struct lua_State;

namespace wowee::addons::worldmap {

/// Registers every map function, each with its fallback below. Called once,
/// after the other API sets.
void registerWorldMapLuaAPI(lua_State* L);

/// What each map function answers while the client data is not loaded or is
/// not 3.3.5-shaped (lua_system_api.cpp): this client's own map, reached
/// through LuaServices. Not bound by name - each is handed to the map function
/// of the same name, so every name has one binding.
namespace fallback {
int GetMapContinents(lua_State* L);
int GetMapZones(lua_State* L);
int SetMapZoom(lua_State* L);
int SetMapToCurrentZone(lua_State* L);
int GetCurrentMapContinent(lua_State* L);
int GetCurrentMapZone(lua_State* L);
int GetCurrentMapAreaID(lua_State* L);
int SetMapByID(lua_State* L);
int GetMapInfo(lua_State* L);
int GetNumMapOverlays(lua_State* L);
int GetMapOverlayInfo(lua_State* L);
int GetNumMapLandmarks(lua_State* L);
int GetMapLandmarkInfo(lua_State* L);
int ProcessMapClick(lua_State* L);
int UpdateMapHighlight(lua_State* L);
int ZoomOut(lua_State* L);
int IsZoomOutAvailable(lua_State* L);
int GetPlayerMapPosition(lua_State* L);
int GetCorpseMapPosition(lua_State* L);
int GetDeathReleasePosition(lua_State* L);
int GetNumDungeonMapLevels(lua_State* L);
int GetCurrentMapDungeonLevel(lua_State* L);
int SetDungeonMapLevel(lua_State* L);
int DungeonUsesTerrainMap(lua_State* L);
int ClickLandmark(lua_State* L);
}  // namespace fallback

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

/// The arithmetic the map functions share, as Wow.exe has it. Pure, so the
/// unit tests can hold it to the client without the game.
namespace detail {

/// A Lua number made an int the way the handlers do: FISTP in the default
/// rounding mode (nearest, ties to even) for indices (GetMapLandmarkInfo,
/// GetMapOverlayInfo), _ftol (truncation) for SetMapZoom and
/// SetDungeonMapLevel. Both go through 64 bits and keep the low 32; a value
/// that does not fit (or NaN) comes out as the integer indefinite, whose low
/// half is zero.
inline int luaRound(double d) {
    if (!(std::fabs(d) < 9.2e18)) return 0;
    return static_cast<int>(static_cast<uint32_t>(static_cast<uint64_t>(
        static_cast<int64_t>(std::nearbyint(d)))));
}
inline int luaTruncate(double d) {
    if (!(std::fabs(d) < 9.2e18)) return 0;
    return static_cast<int>(static_cast<uint32_t>(static_cast<uint64_t>(
        static_cast<int64_t>(d))));
}

/// Every rectangle test in WorldMap.cpp is closed on all four sides: the
/// continent rectangles on the world map (0x00544420, 0x005468F0), the
/// overlay hit rectangles (CRect::Contains, 0x00488D40) and the
/// WorldMapTransforms regions (0x00543920). NaN is never inside.
inline bool closedRectContains(float left, float top, float right, float bottom, float x,
                               float y) {
    return left <= x && x <= right && top <= y && y <= bottom;
}

/// The end of 0x00544140: a point off the map comes back as (0, 0). The
/// tests are written so NaN is off the map too.
inline void clampToMap(float& u, float& v) {
    if (!(u >= 0.0f) || u > 1.0f || v < 0.0f || !(v <= 1.0f)) {
        u = 0.0f;
        v = 0.0f;
    }
}

/// A cell of a continent's 128x128 .zmp grid from the fractions down and
/// across it (0x00543E50, 0x00547170): row ROUND(r * -128) negated, column
/// ROUND(c * 128), read at column + row * 128 with no clamping - a column of
/// 128 is the first cell of the next row. Kept inside the grid here, where the
/// client would read past it.
inline int zmpCell(float rowFraction, float colFraction) {
    const int row = -static_cast<int>(std::nearbyint(rowFraction * -128.0f));
    const int col = static_cast<int>(std::nearbyint(colFraction * 128.0f));
    const int i = col + row * 128;
    return i < 0 ? 0 : (i > 128 * 128 - 1 ? 128 * 128 - 1 : i);
}

/// GetMapLandmarkInfo's icon slot (0x00545A30): the world state's value minus
/// one, slot 0 when there is no world state or the value is not 1..9.
inline unsigned landmarkIconSlot(bool hasWorldState, int worldStateValue) {
    if (!hasWorldState) return 0;
    const unsigned k = static_cast<unsigned>(worldStateValue - 1);
    return k > 8 ? 0 : k;
}

/// The low byte rounded up to a power of two, as GetMapInfo (0x00544CA0) does
/// to its third value.
inline unsigned lowBytePow2(unsigned v) {
    unsigned low = v & 0xFFu;
    const unsigned high = v - low;
    if ((low & (low - 1)) != 0) {
        int i = 8;
        do {
            --i;
            if (low & (1u << i)) {
                low = 1u << (i + 1);
                break;
            }
        } while (i != 0);
    }
    return low + high;
}

/// UpdateMapHighlight's texture size (0x00545110).
inline unsigned highlightTextureSize(unsigned size) {
    const unsigned low = size & 0xFFu;
    unsigned result = size;
    if (low != 0) {
        result = low;
        if ((low & (low - 1)) != 0) {
            int i = 8;
            do {
                --i;
                if (low & (1u << i)) {
                    result = 1u << (i + 1);
                    break;
                }
            } while (i != 0);
        }
        unsigned probe = result * 8;
        while (probe < low) {
            probe = result * 16;
            result *= 2;
        }
        if (size > 0x100) result = result + 0x100 + (size & 0xFFFFFF00u);
    }
    return result;
}

/// How long a gossip point (SMSG_GOSSIP_POI) stays on the maps, and how near
/// the player must come for it to go (0x007F4870 / 0x007F5BA0): 480 seconds,
/// and inside 10 yards (squared distance below 100, flat).
constexpr int kGossipPoiSeconds = 0x1E0;
constexpr float kGossipPoiReachedSq = 100.0f;

}  // namespace detail

/// A WorldMapArea row's MapID, -1 when there is no such row.
int worldMapAreaMapId(lua_State* L, int worldMapAreaId);
/// A WorldMapArea row's LocLeft - LocRight (its width in yards), 0 if none.
float worldMapAreaWidth(lua_State* L, int worldMapAreaId);
/// The first WorldMapArea row for an AreaTable id taken up to its zone, 0 if none.
int worldMapAreaForZone(lua_State* L, int areaId);

}  // namespace wowee::addons::worldmap
