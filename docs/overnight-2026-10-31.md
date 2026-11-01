# Overnight work log, 2026-10-31 to 2026-11-01

Fork branch `audio-data-driven`. Goal: close the gap to the stock 3.3.5
client in sound, movement and frame rate. Each entry says what changed, what
was measured and what is next. Measurements are from the lab server at
192.168.1.121 (2000 playerbots) with a level 1 human in Northshire, RTX 4060
Laptop GPU, 1920x1080, the Ultra preset Jered's settings file carries.

## Baseline (19:08)

Frame profile in Northshire, steady state:

| Stage | ms/frame |
|---|---|
| whole frame | 56 (17.8 fps) |
| gameHandler->update | 20.9 |
| renderWorld | 9.5 |
| beginFrame | 8.8 |
| addonWidgets (FrameXML) | 7.7 |
| frameXml->draw | 1.3 |

perf, 15 s: 70% of CPU samples on the worker threads in `M2Renderer::render`
(visibility lambda, 23%), `M2Renderer::update` (10.5%), particles (6%),
`WMORenderer::render` (6%). `M2 diag: total=125363 ... visible=2457`. Most
of the 125k instances are ground clutter: 880 per tile over every loaded
tile, while clutter draws within 300 yards.

Audio before: zone music from a hand-written table mixed with the client's
own soundtrack (on by default), ambience from a zone-type switch, voices from
guessed file names with random pitch.

## Done

### Sound from the client's tables

`audio::SoundData` reads SoundEntries, ZoneMusic, SoundAmbience, AreaTable,
ZoneIntroMusicTable, ChrRaces, CreatureDisplayInfo, CreatureModelData and
CreatureSoundData. `ZoneMusicController` schedules music as the stock client
does (track on arrival, silence interval, day and night pools, city intro).
Ambience loops per area. Player voice rows from the tables; no pitch shift;
land grunt on landing. WoWee soundtrack off by default. Tests:
`test_sound_data`, `test_zone_music_controller`. Verified in-world:

```
Zone music: Sound\Music\ZoneMusic\Forest\DayForest03.mp3 (set 1)
Zone ambience: Sound\Ambience\ZoneAmbience\ForestNormalNight.wav
Activity SFX voice profile (tables): race 1 male jump=1 land=1 attack=3 wound=3 death=1
```

### Unattended login

`WOWEE_AUTO_LOGIN` and `WOWEE_AUTO_CHARACTER` so a profile run needs no one
at the keyboard. The game handler update now reports its sections to the
frame profiler, and `renderer->update` is in the table.

### Ground clutter streamed by distance (19:30)

Clutter placements stay with the tile; instances exist only for tiles within
the clutter distance (plus 20 yards, released 80 yards further out). 125k
instances became 70k; 17.8 fps became 21.7.

### Transport manager rebuilt the WMO spatial grid every frame (19:38)

The game handler sections showed `gameHandler/world/transport` at 15.9 ms a
frame. `WMORenderer::setInstanceTransform` ended with `rebuildSpatialIndex()`,
every WMO instance on the map, and the zone's elevators and boats call it
every frame. It now refiles the one instance. 21.7 fps became 24.7.

### Mailbox presentation, wider M2 visibility walk, FrameXML layout (19:45)

Vsync on now asks for mailbox with FIFO fallback and three swapchain images.
The M2 visibility walk uses every frame worker, not four. The FrameXML layout
skips subtrees already marked hidden (anchor walk 1.8 ms to 0.5 ms) and looks
up a font face only for strings it will measure (sizing 3.15 ms to 2.5 ms).
Result 26.4 to 27 fps, 37 ms a frame:

| Stage | ms/frame |
|---|---|
| beginFrame (waiting for the GPU) | 12.6 |
| renderWorld | 7.7 |
| addonWidgets | 7.5 |
| renderer->update | 5.5 |
| gameHandler->update | 0.3 |

The GPU sits at 46% utilisation and 960 MHz of a 3105 MHz ceiling while the
frame waits on it. Its own breakdown is 9.6 ms a frame, 7.7 of it in the
post-process pass (which absorbs the scene pass on this driver).

### Movement (19:35)

Movement keys latch on leaving the ground and release on landing. Mount jump
uses the character's velocity. Locomotion gains Fall and JumpLandRun. Tests in
`test_locomotion_fsm` cover both. Not yet seen in-world: no input device in
the unattended runs.

### Footsteps from the ground's TerrainType (19:40)

`TerrainManager::getTerrainSoundTypeAt` follows the dominant layer's
`GroundEffectTexture.Sound` to `TerrainType.SoundID`; the footstep driver maps
that the way FootstepTerrainLookup does for a character, with texture-name
guessing kept as the fallback.

### Room music and ambience from WMOAreaTable (19:55)

The WMO loader keeps the MOHD wmoID, placements keep their MODF nameSet, and
the renderer reports which group the camera is in. `SoundData::wmoArea` picks
the group row, then the building's -1 row. A room's ZoneMusic overrides the
area's set; its AmbienceID replaces the area loop; ambience no longer stops
indoors. The hand-written tavern list applies only when the tables have no
row for the building.

### Widget name index, lazy M2 clocks (20:10)

`WidgetTree::findByName` was a reverse scan of 28k widgets, asked every
frame for panels that do not exist yet (pet, inspect, paperdoll): a name map
took `addonWidgets/models` and `/paperdoll` from 2.6 ms to nothing. The M2
renderer no longer steps 70k animation clocks a frame; clocks advance when
read (`advanceClock`): `renderer->update` 5.5 ms to 3.5 ms.

Northshire, same spot as the baseline: **33.5 fps, 29.8 ms a frame** (was
17.8 fps, 56 ms).

| Stage | ms/frame |
|---|---|
| beginFrame | 8.4 |
| renderWorld | 7.3 |
| addonWidgets | 6.3 (layout 3.9, engine 1.9) |
| renderer->update | 3.5 |
| frameXml->draw | 1.2 |
| gameHandler->update | 0.4 |

Verified in the Goldshire inn (character moved beside Innkeeper Farley):
`Zone music: Sound\Music\ZoneMusic\TavernHuman\RA_HumanTavern2A.mp3 (set 156)`
and `Zone ambience: Sound\Ambience\WMOAmbience\Tavern.wav`.

### Where beginFrame waits (20:20)

beginFrame's 8 ms was assumed to be the GPU or the compositor. Timing the
fence wait and the swapchain acquire separately showed both under 0.05 ms:
the GPU sits at 930 MHz in P4 with "Idle" as its clock reason. The time is
CPU work inside Renderer::beginFrame: the M2 cull input upload (a loop over
every instance), grass population, and the render graph that records the
shadow, HiZ and fog passes on the main thread. Sections added to name them.

### M2 cull fields on one cache line (20:17)

The visibility walk and the cull upload read a dozen scattered fields per
instance; grouped behind the position, the M2 record worker fell from
10-12 ms to 4-5 ms (`renderWorld: ... workers ... m2 4.68`).

### Named the rest of beginFrame (20:35)

Per-pass timing in the render graph and per-renderer timing in the shadow
pass. At 34.5 fps (28.9 ms): shadow_pass 3.9 ms of the 4.1 ms graph,
record+ubo 1.9 ms, M2 cull upload 1.0 ms, acquire 0.1 ms, volumetric fog
0.03 ms. The WMO pass now rejects whole buildings by bounds before testing
their groups, and the M2 shadow walk rejects casters by distance before
touching the model. NPC greeting, farewell and annoyed lines come from
CreatureDisplayInfo.NPCSoundID through NPCSounds; the race voice library is
the fallback. Numeric PlaySound ids resolve through SoundEntries too.

### Instanced M2 shadow pass (20:55)

The shadow pass recorded a push constant and a draw per batch per caster.
Casters now go into a per-frame matrix buffer sorted by model and each
model's batches draw once, instanced (`m2_shadow.vert.glsl`). Before: 2.5 ms
for `shadow/m2` after the caster filters; after: 1.7 ms, and the frame as a
whole fell to **22 ms (45 fps)** from 29 ms. Login, realm and character
screens now play the Wrath glue theme unless the WoWee soundtrack is on.

### Visibility dirty list, lit-instance list, sized-widget list (21:15)

`LuaEngine::updateVisibility` takes the widgets the layout flipped or Lua
toggled instead of walking 28k (`addonWidgets/engine` 2.0 ms to 0.9 ms).
`gatherLocalLights` and the FrameXML sizing pass walk lists of their own kinds.
Steady state before these two lists: **41-44 fps (22.5-24 ms)**.

Full test suite: 234 of 236 pass. `sweep_guard` flagged the duplicated
synthetic-DBC helper in the two new tests, now shared through
`tests/synthetic_dbc.hpp`. `settings_panel_layout` fails on the Interface
panel ("previewtalents lands at -441, past the bottom at -418"); nothing
tonight touched the settings schema or layout, so it is treated as
pre-existing, to be confirmed against the base commit.

## Next

See the backlog in the loop prompt: jump phases, M2 per-frame scans, FrameXML
layout, present mode, WMO ambience, footsteps, Windows package.

## Playtest fixes, 2026-11-01 morning

Jered played the native client against the lab server and reported gaps
against stock 3.3.5a. Each fix goes after the general cause, not the one
case he saw.

- **Combat and casting:** hit reactions on both sides; remote casts stop on
  SPELL_GO, failure, or a cast-time timeout, not only on channel end.
- **Movement:** jump fixes; patrols walk because the WotLK spline Walkmode
  bit is now read on every monster move; creatures turn to face a new target.
- **Reaction colours:** nameplates, names, the selection circle and Lua
  UnitReaction/UnitIsEnemy colour by how the unit regards the player, so
  non-aggressive beasts are yellow.
- **Character art:**
  - Robe and dress skirts come from GeosetGroup[2] on the chest or legs
    item, and robe leg textures are drawn last.
  - NPC clothing groups 5/7/9/10/11 keep exactly one variant.
  - Equipment is re-applied after every player model rebuild.
  - Shoulders attach on the correct sides for everyone; NPCs also get helms.
  - Facial hair textures come from CharSections section 2.
  - Characters get no specular and no generated normal maps (they embossed
    composited skins). The world keeps normal mapping and parallax.
- **Lighting:** interior WMO groups use MOCV x2 with no sun shadow; unit
  lighting eases indoors over about a second.
- **Particles:** emitters use authored gravity, spread and drag (candles no
  longer drip).
- **UI:**
  - Message frames honour justifyH.
  - Subzone text is the real subzone.
  - Chat bubbles use the stock textures, font and colours.
  - A hidden subtree is re-marked after a partial resolve, which fixes the
    half-closed achievement window.
- **Map:** one input owner when hosted in WorldMapFrame; highlights use
  additive alpha (no black squares in Northrend); cosmic highlights sit at
  the stock rects.
- **Barber:** previews revert unless bought; chairs seat at the server's
  position and facing; remote units show their stand state.

Not client bugs:
- Undead-looking Stormwind commoners wear their Hallow's End costume (SmartAI
  casts the appearance spell; the event runs through Nov 1).
- Faces like Dordam's are Death Knight-only CharSections rows that the
  playerbot randomizer hands to other classes.
