# The minimap and the world map

How the 3.3.5a client (build 12340) draws its minimap and answers the world
map's Lua API, read from `Wow.exe` 12340 with Ghidra, and what Wowee does
with it. Each rule names the function or data it came from. FrameXML
(Minimap.xml/.lua, WorldMapFrame.xml/.lua) is stock in both clients; only the
C side differs.

Code:

- `include/ui/minimap_stock.hpp` - the minimap's rules as pure functions
  (placement, ranges, blip classification, POI arrows, town tracking), tested
  by `tests/test_minimap_stock.cpp`.
- `src/ui/game_screen_minimap.cpp` - the marker pass that draws blips, POIs,
  rim arrows and the player arrow over the map.
- `src/rendering/minimap.cpp`, `assets/shaders/minimap_display.frag.glsl` -
  the map image itself (terrain tiles or a WMO's own tiles).
- `src/addons/lua_engine.cpp` (Minimap methods), `src/addons/lua_system_api.cpp`
  (tracking, zoom CVars), `src/game/game_handler.cpp` (pings).
- `src/addons/lua_world_map_api.cpp` - the world map's C functions (section 3).

Units: sizes below are interface units (FrameXML's 768-high space). The engine
keeps frame coordinates in "DDC", where the screen diagonal is 1.0
(world-ui.md section 0); the minimap's sizes are DDC fractions of the screen
height times 5/3, which comes out the same number of interface units at any
resolution.

## 1. Minimap

### 1.1 The frame and its methods

`MinimapFrame.cpp`. Lua methods (table at 0x00ACEBB8, 15 entries):

| Method | Handler | What it does |
|---|---|---|
| SetMaskTexture | 0x00583860 | replaces `Textures\MinimapMask` (0x00BEBA30) |
| SetIconTexture | 0x00583CE0 | replaces `Interface\Minimap\POIIcons` |
| SetBlipTexture | 0x00583E00 | replaces `Interface\Minimap\ObjectIcons` |
| SetClassBlipTexture | 0x00583F60 | replaces `Interface\Minimap\PartyRaidBlips` |
| SetPOIArrowTexture | 0x00583980 | replaces the guide arrow |
| SetStaticPOIArrowTexture | 0x00583AA0 | replaces Rotating-MinimapArrow |
| SetCorpsePOIArrowTexture | 0x00583BC0 | replaces the corpse arrow |
| SetPlayerTexture | 0x0057E100 | the player arrow texture (default `MinimapArrow`) |
| SetPlayerTextureHeight / Width | 0x0057E1C0 / 0x0057E280 | size of that texture region (+0x2A0) |
| GetZoomLevels | 0x0057BF50 -> 0x007F3B60 | always 6 |
| GetZoom | 0x0057BF90 -> 0x007F3B40 | the indoor or outdoor level, whichever is in use |
| SetZoom | 0x0057BFD0 -> 0x007F3AE0 | see 1.2 |
| PingLocation | 0x0057ED70 | see 1.8 |
| GetPingPosition | 0x0057EFE0 | see 1.8 |

The textures are loaded once by 0x005832F0: MinimapMask, POIIcons,
ObjectIcons, PartyRaidBlips, the seven Vehicle-* icons and the four rotating
arrows (Rotating-MinimapArrow, -GuideArrow, -CorpseArrow, -GroupArrow).

Wowee: SetZoom, GetZoom, GetZoomLevels, PingLocation, GetPingPosition and
SetPlayerTextureWidth/Height are real; the texture setters are not needed by
stock FrameXML and are not implemented.

### 1.2 Zoom

Two levels kept apart (0x00AF4E4C outdoor, 0x00AF4E50 indoor), in the CVars
`minimapZoom` and `minimapInsideZoom` (read back at world load by
0x007F6730). `0x007F3B90` turns the level into the map's radius in yards:

- outdoors: `{14, 12, 10, 8, 6, 4}[level] * 0.5 * 33.333` (0x00A41E04 ints,
  0x009E2EC4, 0x00A3E554) = 233.3, 200, 166.7, 133.3, 100, 66.7 yards;
- indoors: `{150, 120, 90, 60, 40, 25}[level]` (0x00A41E1C floats).

The frame is `2 * radius` yards across. SetZoom rounds its argument and
0x007F3AE0 compares it **unsigned**: anything above 4, negative numbers
included, becomes 5. A change sets the redraw flag; switching between indoor
and outdoor fires `MINIMAP_UPDATE_ZOOM` (event 0xA9, from 0x007F5BA0), as does
world load (0x007F6730).

Wowee: the same tables (`Minimap::kZoomRadius`, `kInsideZoomRadius`), the
unsigned clamp, and the stock CVar names (it used to write `wowee_minimapZoom`;
those are still read).

### 1.3 The map image

`Minimap.cpp`, 0x007F5BA0 every frame:

- Outdoors it keeps four terrain tiles - the 2x2 block around the player
  (offset table 0x00A41EF0) - named through md5translate
  (`%s\md5translate.trs`, 0x00A41F18; `%s\map%d_%02d.blp` 0x00AF4E5C) under
  `Textures\Minimap`, and draws them with the mask as a second texture
  (0x0057E540).
- Indoors - the player in a WMO group without the exterior flag 0x8
  (0x007A1480), or WMOAreaTable flags 0x1 / 0x18 - it draws the building's
  own tiles (`%s_%03d_%02d_%02d.blp`, 0x00AF4E60) into a 256x256
  `MinimapRenderTarget` (0x005831C0) and shows that through the mask. Which
  groups: those reached from the player's group through portals
  (0x007A17E0 -> 0x007B00A0), at most `minimapPortalMax` portals deep, within
  a box the size of the indoor radius around the player. They are drawn in
  order of `(group centre z) - player z`, the player's own group last
  (key FLT_MAX, 0x007F5070, sorted by 0x0057BD10).
- `Textures\MinimapMask` is a plain disc, alpha 255 to its last texel; no
  shading at the rim.

Wowee: a 3x3 terrain composite (the same image, a larger cache), the WMO
tiles of the indoor groups whose height range holds the player
(`wmo_minimap::groupsToDraw`), lowest first and the current group last; the
disc is cut in the shader. The rim used to be darkened by 70%; it is not now.
Wowee entered the indoor map on MOGP flag 0x2000 alone, and Stormwind's
Trade District - hundreds of indoor groups flagged exterior-lit (0x40) - drew
a black map there. It now also requires the exterior flag 0x8 (0x007A1480's
test) and 0x40 to be clear; the stock client shows those streets as terrain.
Exactly which bit decides it there was not pinned down (0x007A1480 reads a
group record's +0x30 & 8; WMOAreaTable flags 0x20 also force outdoors in
0x007F5BA0).
**Gap:** the portal walk - Wowee shows every indoor group at the player's
height, stock only those connected to the player's group.

### 1.4 Rotation

`rotateMinimap` (CVar at 0x00BD093C+0x30). The angle is the facing of the unit
the camera follows (0x0057C6A0 -> 0x0057BCC0, stored at frame+0x2A4 and
0x00BEBA70) - the **player's facing, not the camera's**: orbiting the camera
does not turn the map. With rotation on:

- the map image is turned so the facing is up;
- every blip, POI and arrow position is turned by minus the facing about the
  centre (0x004C2300 with the matrix 0x004C3460 builds);
- the player arrow gets rotation 0 (0x0057BCC0), otherwise the facing;
- `MinimapCompassTexture` (found by name at 0x0057BEA0, frame+0x2A8) is turned
  by minus the facing (0x0057D5B0), else 0.

Minimap_UpdateRotationSetting shows the compass ring or the N tag.

Wowee: it turned the map by the camera's yaw, half a turn out (the old
comments said so); now by the player's facing (`Minimap::setBearing`, shader
rotation = facing), the compass texture turns (Texture:SetRotation is now real
- it also makes `MinimapPingSpinner` spin), and the N tag is no longer hidden
by a Lua snippet.

### 1.5 Placement and ranges

0x0057C660: a point dNorth, dWest yards from the player lands at

    x = w/2 - (w/2) * dWest / radius,   y = w/2 + (w/2) * dNorth / radius

(w the frame width, y up), then turned as in 1.4. `ui::minimap_stock::mapOffset`.

- Objects further than **0.8 of the radius** (0x00A11C4C; 2D distance) are not
  gathered at all. The group and the POIs use the same 0.8 (0x00A34C18) to
  decide blip or rim arrow.
- Rim arrows sit at `0.8 * 70.4` = 56.32 units from the centre
  (0x00A11F84 = -0.8, 0x00A11C4C, 0x00BEBA44 = 0.055 of the basis), whatever
  the frame's size, on the direction 0x004F5130 gives (atan2(dWest, dNorth),
  with its axis cases).
- Sizes (0x0057DCA0, fractions 0x00A11F88..94 times 5/3): blips 16 units,
  vehicle icons 48, rim arrow quads 57.6. The arrow textures are mapped onto
  their quad turned 45 degrees with the corners on the quad's edges, so the art
  shows 57.6/sqrt(2) = 40.7 units across, its tip pointing outward
  (art turn 3/4 pi, 0x00ACEC64).

Wowee: blips were cut off at the rim minus three pixels and edge arrows sat on
the rim at 32 units; now 0.8 and the stock ring and size.

### 1.6 Which blip

The gathering (0x00581E80 -> 0x004D4B30 walking the object manager with
0x0057F7F0) runs **every frame while the player moves, and once a second
(999 ms) otherwise** - or when the zoom, the rotation CVar or the map changes.
Between gatherings the blips stay where they were put, so a creature crosses a
standing player's map in one-second steps. Positions are recomputed from the
saved ones every frame, so turning with rotation on is smooth.

0x0057F7F0 puts each object into one of 23 lists (0x00BF8098, 16 bytes each);
lists 2..15 are drawn with ObjectIcons cell = list number (texture
coordinates built by 0x005832F0: 8 columns x 2 rows):

| Cell | Art | Who |
|---|---|---|
| 2 | red, silver rim | another player who can be attacked (0x00729740) |
| 3 | gold, silver rim | another player, other team, not attackable |
| 4 | green, silver rim | another player, same team |
| 5 / 6 / 7 | red / gold / green, gold rim | an NPC by reaction (0x007251C0: 0-1, 2-3, 4+) |
| 8 | gold | tracked resource; town-tracked NPC; tracked mailbox |
| 9 | yellow ! | quest giver status 8, or 2 with Low Level Quests tracking |
| 10 | yellow ? | status 10 |
| 11 | blue ! | status 7, or 4 with Low Level Quests tracking |
| 13 | green ! | a flight master whose node the player does not know |
| 15 | (empty cell) | the player's own pet or charm |

Status 5, 6 and 9 draw nothing. In order, for a unit (type 9) or a player
(type 0x19, party and raid members excluded - they are PartyRaidBlips):

1. charmed-by or summoned-by the player (descriptor +0x18 / +0x20 of the unit
   block): cell 15;
2. health 0, unless the creature type flags have 0x80 (0x00715E30, visible to
   ghosts - spirit healers): nothing;
3. town tracking type 1 whose mask is in the unit's NPC flags (0x0071ACF0) and
   the unit is one the player can deal with (0x00729530, reaction > 2 both
   ways): cell 8;
4. quest giver status (unit+0x90): 10 -> 10, 8 -> 9, 2 -> 9 with trivial
   tracking, 7 -> 11, 4 -> 11 with trivial tracking;
5. unit+0x94 non-zero -> cell 13. That field is SMSG_TAXINODE_STATUS's
   answer, stored by 0x006D5FC0 as 7 when the status byte is 0 (node unknown)
   and 0 when it is 1. The client asks for every flight master it meets that
   is not hostile (0x00729F40 -> 0x006D5130, CMSG_TAXINODE_STATUS_QUERY 0x1AA);
6. otherwise only if tracked (0x006DCA00): not UNIT_BYTES_1 vis flag 0x4
   (untrackable), and either UNIT_DYNFLAG_TRACK_UNIT (0x2) or the creature
   type's bit in PLAYER_TRACK_CREATURES (players count as humanoids). Then
   players by attackable / team, NPCs by reaction.

A game object (type 0x21): mailbox tracking (type 2, GO type 19) -> 8; then
the quest status as above; then a skill lock (Lock.dbc key type 2) on the
object's lock field (by GO type, property table 0x00A36F34: doors and buttons
field 1, fishing holes field 4, chests and the rest field 0) whose LockType bit
is in PLAYER_TRACK_RESOURCES -> 8 (0x006DCA90).

Buildings (0x0077F160 for the object, 0x00581E80 for the player): when the
player is in a WMO, only objects in the same WMO are gathered; when the
player is outside, objects inside a WMO are drawn tinted 0xB0B0B0
(0x00580380), after the others.

Draw order (0x00581E80): POI icons, PartyRaidBlips (0x0057FF70), vehicles
(0x00580720), ObjectIcons (0x00580380, then the greyed pass), POI rim arrows
(0x0057D860), party arrows (0x00580AE0), raid arrows (0x00580EC0).

Wowee before: tracked creatures in silver-rimmed cells by its own colour rule,
every player with Track Humanoids, the flight master's tracking boot over
every flight master always, quest givers from a different status table, no
pet rule, no building rule, everything every frame. Now: the table above
(`ui::minimap_stock::classifyUnit` / `classifyGameObject`), the one-second
gathering, the building rule via `WMORenderer::insideWmoArea` (interior groups
only - see gaps), CMSG_TAXINODE_STATUS_QUERY sent for flight masters on
creation. Its optional overlays (every creature, rares, chests, loot corpses,
kill targets, battleground carriers) stay behind their settings and are off by
default.

### 1.7 The group

0x007F4320 (party, 4) and 0x007F4390 (raid, 40, skipping the player's own
subgroup) every frame, per member (0x007F3F40): the unit if in sight,
otherwise SMSG_PARTY_MEMBER_STATS' area (must be on this map) and int16
position, if online and without status flag 0x20 (0x005444F0). Within 0.8 of
the radius it is a PartyRaidBlips blip - class - 1 for the party, class + 15
for the rest of the raid (8 x 4 cells), always untinted; further out a
Rotating-MinimapGroupArrow on the rim.

Wowee: the same, except that a dead or ghost member is no longer greyed (the
client draws them white).

### 1.8 Pings

`Minimap:PingLocation(x, y)` (0x0057ED70): x, y are interface units from the
centre (Minimap_OnClick). As a fraction of the frame they are turned back by
the rotation and scaled by 2 * radius into yards (`minimap_stock::worldOffset`),
added to the player's position, sent as MSG_MINIMAP_PING (0x1D5: float x,
float y in world order) **only in a party or raid**, and shown locally through
0x0057EB80.

0x0057EB80 (local pings and incoming MSG_MINIMAP_PING, from 0x006E2E90):
keeps the one latest ping (0x00BEBA8C/90) and fires `MINIMAP_PING` (0x15F)
with `"%s%f%f"`: the unit - "player" unless the sender is "partyN" or "raidN"
- and the ping's offset from the player as a **fraction of the map's width**,
east then north, turned by the rotation. Minimap_SetPing multiplies by the
frame's size and places the MinimapPing frame. `GetPingPosition` returns the
same pair for the stored ping against the player's current position.

Wowee before: MINIMAP_PING carried the world coordinates (FrameXML put the
ping off the map, so only this client's own circles showed it), the player's
own pings fired nothing, every ping was sent even solo, GetPingPosition did
not exist. Now as stock (`GameHandler::notifyMinimapPing`,
`minimapPingPosition`); this client's circles are drawn only when it owns the
ring.

### 1.9 POIs and rim arrows

The POI list (0x007F6730, at map load): every AreaPOI row of the map with flag
0x1, plus four slots of the client's own (0x00D39490, 0x58 bytes each):

| Slot | Set by | Icon | Flags | Importance | Arrow |
|---|---|---|---|---|---|
| 0 | SelectQuestLogEntry (0x005DFFA0): the quest's point | 6 | 0 | quest's | guide |
| 1 | SMSG_GOSSIP_POI (0x0058A8E0 -> 0x007F4870), kept 480 s | packet | packet | packet's data | guide |
| 2 | the corpse (0x007F4990, "CORPSE_RED") | 8 | 0x2 | -1 | corpse |
| 3 | the spirit healer (0x007F4A00) | 8 | 0x2 | -1 | guide |

0x007F44A0, whenever the player moves: a POI with a position, whose
WorldStateID (if any) has a non-zero value, within 0.8 of the radius and with
flag 0x2, is drawn as its POIIcons icon (icon array index = world state value
- 1, else 0; POIIcons is 14 icons to a row on an 18-texel pitch, sampled from
one texel in, 0x005832F0). Of the rest, those within 694.44 yards
(0x00A41E00) - and the corpse at any distance - compete for **three** rim
arrows: the lowest Importance wins, ties to the nearer. Arrow art
(0x0057D860): corpse slot -> Rotating-MinimapCorpseArrow, AreaPOI ->
Rotating-MinimapArrow, the other slots -> Rotating-MinimapGuideArrow. Indoors
only the client's slots 0-2 and AreaPOIs with flag 0x100 get one.

So walking out of Goldshire, a silver arrow on the rim points back at the town
until it is 694 yards away; EPL towers and battleground nodes show their
world-state icons on the map.

Wowee before: gossip points as icons (all 200 kept, no expiry), the quest
objective areas shaded and the turn-in ? drawn on the minimap (the stock
minimap draws neither), the corpse as an icon or an arrow on the rim, no
AreaPOI at all. Now the list above (`renderMinimapPois`), except slot 0
(see gaps).

### 1.10 Tracking

GetNumTrackingTypes (0x0057F170): the tracking spells (0x00BE8DE8, in the
order they were learned) plus the town entries the class is offered
(0x0057E980 over the table at 0x00A11C50):

| Entry | Type | Mask | Texture | Classes |
|---|---|---|---|---|
| MINIMAP_TRACKING_REPAIR | 1 | 0x1000 | Repair | all |
| ..._VENDOR_FOOD | 1 | 0x200 | Food | all |
| ..._VENDOR_POISON | 1 | 0x400 | Poisons | rogue |
| ..._VENDOR_AMMO | 1 | 0x100 | Ammunition | warrior, hunter, rogue |
| ..._VENDOR_REAGENT | 1 | 0x800 | Reagents | all |
| ..._INNKEEPER | 1 | 0x10000 | Innkeeper | all |
| ..._FLIGHTMASTER | 1 | 0x2000 | FlightMaster | all |
| ..._STABLEMASTER | 1 | 0x400000 | StableMaster | hunter |
| ..._BATTLEMASTER | 1 | 0x100000 | BattleMaster | all |
| ..._TRAINER_CLASS | 1 | 0x20 | Class | all |
| ..._TRAINER_PROFESSION | 1 | 0x40 | Profession | all |
| ..._AUCTIONEER | 1 | 0x200000 | Auctioneer | all |
| ..._BANKER | 1 | 0x20000 | Banker | all |
| ..._MAILBOX | 2 | GO type 19 | Mailbox | all |
| ..._TRIVIAL_QUESTS | 3 | - | TrivialQuests | all |

GetTrackingInfo(i) (0x0057F1B0): a spell -> name, icon, 1 or nil, "spell"; a
town entry -> its GlobalStrings text, `Interface\Minimap\Tracking\<tex>`, 1 or
nil, "other". SetTracking (0x0057F380): a spell is cast; a town entry cancels
the tracking aura and becomes the tracking (0x0057E070, which also stores its
key in `minimapTrackedInfo` and fires MINIMAP_UPDATE_TRACKING, 0xAA); nil
cancels both. A tracking spell starting clears the town entry (0x0057EA30).
GetTrackingTexture (0x0057F4F0): the town entry's texture, else the spell's
icon, else `Interface\Minimap\Tracking\None`. Switching to or from Low Level
Quests makes every quest giver with status 2-4 ask again (0x0057E020).

Wowee before: spells only. Now the town entries, the one-tracking rule,
`minimapTrackedInfo`, and their blips (1.6). Spells are listed by name, not in
learned order (gap).

## 2. Gaps (minimap)

- The indoor map's group choice is by height, not by the portal walk
  (1.3).
- The building rule uses interior WMO groups only; stock counts any WMO group
  the object stands in unless the root's flag 0x400 and the group's exterior
  flag both say otherwise (0x007A18D0).
- Vehicle icons (0x00580720: Vehicle-Air/Ground-Occupied/Unoccupied,
  -Horde/-Alliance, 48 units, turned to the vehicle's facing) are not drawn.
- POI slot 0 (the selected quest log entry's point and its guide arrow) is not
  kept.
- Tracking spells are ordered by name rather than in the order learned.
- The off-map party position does not check that the member's zone is on the
  player's map.

## 3. World map

In the client the world map is FrameXML from top to bottom (detail tiles,
overlays, landmarks, arrows); C keeps the state - continent, zone, dungeon
floor, area at 0x00ACCF04..10 - and the arithmetic, in `WorldMap.cpp`. Every
view change goes through SetMap (0x00546310), which rebuilds the landmark and
overlay lists and fires WORLD_MAP_UPDATE (event 0x163). Wowee's
`src/addons/lua_world_map_api.cpp` was rebuilt from those handlers earlier and
cites each address; this pass compared them line by line again. Pure helpers
are in `include/addons/world_map_api.hpp` (`detail::`), tested by
`tests/test_world_map_lua_arith.cpp`.

### 3.1 State and SetMap (0x00546310)

- Zone selection; a zone with WorldMapArea.DefaultDungeonFloor >= 1 shows that
  floor as (-2, wmaId, floor).
- Landmarks: AreaPOI rows placed by 0x005445F0 whose position is non-zero
  (eps 0x009EA27C), whose AreaID is < 1, or has ExplorationLevel < 0, or is
  explored (0x00546100: PLAYER_EXPLORED_ZONES bit; a bit past 0x200 counts as
  explored), and whose WorldStateID is 0 or has a non-zero value (0x00548D10).
  Flag 0x400 rows show only when a zone of a continent is showing (WMA from
  0x00543860). After them, one more landmark: the gossip point (SMSG_GOSSIP_POI,
  the minimap's POI slot 1, record 0x00D394E8 via 0x007F3BE0), placed through
  0x00544140 without the flag checks, kept 480 s or until the player is within
  10 yards (dist^2 < 100, 0x009E8CC8, checked in 0x007F5BA0), either way
  SetMap re-runs.
- Overlays (the fog): only with a player, continent != -2 and a zone or area
  showing. A WorldMapOverlay row is added when any of its four non-zero
  AreaTableIDs is explored or has ExplorationLevel < 0. No faction filter.
- Re-runs: PLAYER_EXPLORED_ZONES changes (0x005470D0) and AreaPOI world
  states (0x005285F0, also event 0x1D0). Wowee polls a hash of both (and now
  the gossip point) every 250 ms.

### 3.2 Coordinates (0x00544140)

World to map: WorldMapTransforms first (0x00543920; the region test is closed
on all four sides), then the continent / zone rectangle; the world view from
WorldMapContinent searched from the end (0x00544090; Outland, map 530, is
hidden on the Azeroth world map); dungeon floors from DungeonMapChunk
(0x005439E0) with the player's WMO group (0x00BD0814). Off the map - or NaN -
gives 0, 0. Constants: 1/(64*533.333) 0x00A0B5E0, the world map's 62.625 x
41.75 tiles 0x00A0B5EC/F4, the zone map 1002 x 668 0x009E2ED8/D4.

GetPlayerMapPosition (0x00545880 -> 0x005444F0): the player, party
(0x0052C680) and raid (0x005726F0) members. In sight, the unit on the current
map; out of sight the group record (0x00513C30): 0, 0 if status flag 0x20,
otherwise its zone's map and int16 position (online is not checked).
GetCorpseMapPosition (0x00545950 / 0x00523E80): the corpse's own map
(0x00BD0818) and position. GetDeathReleasePosition (0x005459C0): map 0x00BD0844,
position 0x00BD0A64.

### 3.3 The Lua functions

| Function | Handler | Notes |
|---|---|---|
| SetMapZoom | 0x005469E0 | continent -2..count-1, zone default -1; arguments truncated from the double (_ftol) |
| SetMapToCurrentZone | 0x00547C10 -> 0x00547170 | instance with no map and no corpse entrance: the cosmic view (-2,-1,-1) |
| GetCurrentMapAreaID | 0x00544E10 | WMA id + 1 |
| GetMapInfo | 0x00544CA0 / 0x00543CD0 | name, height ROUND(h*1002/w), third value's low byte rounded to a power of two |
| GetNumMapOverlays / GetMapOverlayInfo | 0x00543060 / 0x00545C80 | path, width, height, offsetX, offsetY, mapPointX, mapPointY |
| GetNumMapLandmarks / GetMapLandmarkInfo | 0x00543020 / 0x00545A30 | name, description, textureIndex (world-state slot), x, y, mapLinkID, flags & 0x200 |
| UpdateMapHighlight | 0x00545110, 0x00544420 | closed rectangles (CRect::Contains 0x00488D40) |
| ProcessMapClick | 0x00546E80 / 0x005468F0 | .zmp 128x128 grid, index col + row*128 |
| GetNumDungeonMapLevels, SetDungeonMapLevel, GetCurrentMapDungeonLevel, DungeonUsesTerrainMap | 0x00546290, 0x00547B80, 0x00544FC0, 0x00544C40 | |
| ZoomOut / IsZoomOutAvailable | 0x00546A90 / 0x00545050 | |

### 3.4 What changed in Wowee (this pass)

- The gossip point as the last landmark (it never showed).
- The cosmic view for an instance without a map (was the world view).
- Corpse position on its own map (was the player's map only).
- Closed rectangles in WorldMapTransforms, ProcessMapClick's continent test
  and UpdateMapHighlight; NaN clamps; .zmp index wrap; double truncation in
  SetMapZoom / SetDungeonMapLevel; flag 0x400 POIs only on zone views;
  out-of-range party members by flag 0x20 rather than "online".

### 3.5 Gaps (world map)

- Gossip arrival is noticed by polling the handler's list every 250 ms; an
  identical point resent after the 200-entry cap is not seen as new.
- x87 80-bit intermediates are not reproduced (Wowee computes in float).
- Stock reads past the .zmp grid at row fraction 1.0; Wowee clamps.
