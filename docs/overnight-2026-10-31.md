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

## Playtest fixes, 2026-11-01 late morning

### Fixed

**Rendering**
- Interior groups flagged exterior-lit (Stormwind's districts, Silvermoon) now take outdoor light.
- Glue scenes are lit by their own M2 lights.
- Login scene composited.
- Northrend alpha RLE off-by-one: blocky cliff patches.
- MCCV tint.
- Stock texture tiling: 8 per chunk.
- Terrain bump eased on slopes.

**Characters**
- Eyebrows: the face and facial-hair layers are placed by region in the equipment composite.
- DK faces only for DKs.
- Starter outfits in character create.
- Robes drawn over shirts.
- Kneepads and the back panel.
- Chest wins over shirt.

**Movement and animation**
- Melee abilities use their kit animation per weapon.
- Every unit swings and flinches.
- Ready stances, parry, dodge and block.
- Sheath reach with a timed hand-off and sounds.
- Weapons on the model's sheath points.
- Cross-fades.
- Death holds; corpses spawn down; release acks.
- Slope limit; fence floors.
- Jump lands on the arc.

**Sound**
- Listener at the character with linear falloff (quiet emotes and greetings).
- M2 $CSD emote voices.
- Coin clink on sale.
- Greetings on interact.

**UI**
- Spell book filtered by the stock rules.
- Stock nameplates.
- Unit frame backgrounds.
- Gossip fonts left-aligned.
- Rage and runic power /10.
- Cast errors from GlobalStrings.
- Live-zone PvP text.
- Chat: own emotes, zone channels, own bubbles.
- Character window no longer drags; rotate buttons hold; item locks clear.

**World**
- Area triggers fire reliably (heartbeat before CMSG_AREATRIGGER).
- Instance portal particles.
- Patrols walk (pace-based).

### Performance

| Location | Frame time |
|---|---|
| Elwynn, before | 26.9 fps, 37 ms |
| Northrend, before | 24-30 fps |

Changes: off-screen particle throttle, ribbons walk the emitter list, no spatial-index rebuild per model load, incremental M2 removals, clutter out of the collision grid, stable shadow candidates, tile-level terrain shadow cull, parallel cull upload, safe widget-layout speed-ups. The visible-only draw list was reverted because it dropped action bar textures. Not yet measured in game.

### Regression

The glue-scene particle draw caused a GPU device loss on the Night Elf create scene. It is gated behind WOWEE_GLUE_PARTICLES=1 until fixed.
Measured after (Northshire, test account, same machine): **37.0 fps, 27 ms** (was 26.9 fps, 37 ms). Remaining top costs: renderWorld 7.9 ms, widgets 4.9 ms (layout 3.6), update 4.8 ms (m2 join 3.2, particles 2.8 ms for 112k live particles), shadows 2.4 ms. Next: particle counts (112k live is far above what is on screen).

## Afternoon, 2026-11-01: reverse-engineered behaviour from Wow.exe 12340

Several fixes now come straight from the stock binary (Ghidra projects are in the session scratchpad):

- **Strafe:** CGUnit_C 0x73DAB0. The legs turn 45 or 90 degrees through a critically damped spring (omega 20); SpineLow and Head twist back toward the facing, 45 degrees each. The camera yaw is wrapped first, because it accumulates without bound.
- **Shuffles:** 0x73E2C0. ShuffleLeft/ShuffleRight while standing and turning.
- **Camera:** CGCamera, 0x606F90 and friends.
  - Vertical FOV is 54 degrees (pi/2 x 0.6).
  - Zoom defaults to 15 yd, times a factor of 1 to 2, capped at 50.
  - Zoom moves at 8.33 yd/s, 1 yd per wheel notch.
  - Follow style defaults to "Smarter" (4).
  - Collision snaps the camera in, then eases back over 2 s.
- **Selection circle:** Textures\UnitSelectTexture.blp, projected onto the ground. Radius is the square root of half the GeoBox diagonal times scale, capped at 10. Colours follow the UnitSelectionColor rules.
- **Glue scenes:** per-race RaceLights and fog from GlueParent.lua.

Other fixes:

- **Rendering:**
  - Characters, M2 and WMO were all lit from behind. The models wind counter-clockwise, so the front face is now set accordingly.
  - Stock texture tiling, the Northrend alpha RLE fix, MCCV, and MCSH baked shadows.
  - Creatures without the hover or fly flags settle onto the floor.
  - Particles go dormant off screen, with closed-form warm-up when they return.
- **Stability:** the startup crash came from the glue scene being destroyed mid-frame. It is now retired a frame later; verified with validation layers, zero errors.
- **UI:**
  - Talent window: server talent group count, ordered learn requests, no spec-change event storm.
  - Minimap: stock blips.
  - Combat text and combo points.
  - Spell book filtering.
  - Login Options glue frame.
  - Level ?? only for attackable units 10 or more levels above you.
- **Animation:**
  - Melee abilities play their kit animations.
  - Ready stances, parry, dodge and block.
  - Sheathe reach timed by the model's $SHL/$SHR events.
  - Cross-fades; idle variations weighted by frequency; death lock; morphs; stealth and ghost translucency.
- **Audio:**
  - Combat sounds from WeaponSwingSounds2 and WeaponImpactSounds.
  - Creature wound and death sounds; footsteps from FootstepTerrainLookup.
  - SoundEmitters and doodad $DSL loops.
  - The listener sits at the character.
- **Measured:** Northshire went from 26.9 fps to 37 fps after the morning's performance passes. Particle dormancy has not been measured yet.

### Next

- The world map rebuilt the stock way, from reverse-engineered C APIs (in progress on an isolated branch).
- A plan of tasteful enhancements: wind sway, cloth flutter, ambience.
- The Windows package rebuild.

## Sunday afternoon: first enhancement batch, Windows package

- `3350897` enhancements batch 1:
  - Trees sway in the grass's wind, or play their own sway animation if they have one.
  - Awnings, tents and tarps flutter, with their poles and edges held still.
  - Wind follows the weather and fades for foliage indoors.
  - Shadow darkness follows Light.dbc, and shadow edges use a rotated 9-tap PCF.
  - Zone music crossfades over 3 s.
  - Footsteps and hits vary.
  - New settings: `foliagewind` on the Grass page and `shadowstrength` on Graphics. The Sky rows moved to Display and the character-sheet rows to Gameplay, so every settings page fits again.
- Tests updated to stock rules that were already in place: the no-cape panel on bare bodies, no sheath when combat starts, no weather chat lines.
- `e58eed9`: canvas is any texture that isn't structural. The Stormwind awning uses "overhang copy.blp" and the Durotar tents use "mm_orc_roof"; both now flutter.
- `dd83374`: a fog field was named `far`, which is a macro in windef.h and broke the Windows build. The Windows package was rebuilt from that commit, staged to `~/workplace/wowee-dist`, the zip refreshed, and `wowee-share` restarted. It serves on port 8081.
- Smoke test, Wptester: no errors; 55-60 fps once loaded.
- Open: sweep_guard is over its ceilings (dead declarations, a too-long function, one duplicate). An agent is cleaning that up. The world map rebuild is still running.

## Sunday afternoon, later: enhancements batch 2, world map merged

- `c4fc12c`, enhancements batch 2:
  - Reverb by place from SoundProviderPreferences, on a world sound bus. UI and music stay dry.
  - Under water, world sound is low-passed and the stock UndwaterLoop plays.
  - Sun shafts follow the sun's visibility.
  - Fog follows view distance.
  - Terrain bump eases off on slopes and follows the surface-bumps setting.
  - New "Classic" preset.
  - Same commit: the sweep cleanup (dead code, the duplicate sampler-set code, a too-long function split, glue dialogs shared).
- Merged `map-stock-framexml`. The world map's C side was rebuilt from the Wow.exe 3.3.5a handlers: continents, zones, .zmp click grid, overlays, landmarks, quest POIs, zoom-out rules, dungeon floors, and the player arrow. The stock WorldMapFrame now draws the map; the native ImGui map is only a fallback.
  - `WOWEE_DEBUG_LUA=tools/lua_tests/world_map_test.lua` passes on the merged build: 0 failures, including real M and Escape keys and mouse clicks.
- Found: the three "disconnects" in Goldshire were the map agent's test client logging into the same WPTEST account. The server closes the older session when the same account logs in again. Lab worldserver logging now includes `Logger.network=3`; the backup of the old config is in the scratchpad.
- Goldshire, test window 1053x986: 42-48 fps.
  - Top stages: renderWorld 5.8 ms, beginFrame 3.8 ms, FrameXML widget layout 2.6 ms, renderer update 3.3 ms, shadow pass 2.2 ms.
  - The frame is spread across many stages; no single stage dominates.
- Next: remove the superseded map bindings (an agent is on it), relaunch for Jered, then performance in Goldshire (layout cost, shadow pass).

## Particles by screen size

- `8554177`: an emitter is stepped and drawn out to about 120 times the larger of its model radius and its particle reach (speed and gravity over its lifespan, plus emission area and size, computed once per model). The minimum range is 150 yd, and it never goes past the instance's own range.
- Goldshire, same clocks (2.6 GHz ceiling):

  | | before | after |
  |---|---|---|
  | live particles | 36-47k | 20k |
  | particles drawn | 26-32k (budget full) | 9.9k |
  | particle update | 1.8-2.1 ms | 0.75 ms |
  | particle draw | 1.6-4.5 ms | 0.42 ms |
  | frame rate | 40-47 fps | 60 fps (vsync cap) |

- `03fa503`: one registration per map binding. The old system-set map functions live on only as the fallback for when the map data isn't loaded.
- Host note: Jered's System76 power profile had been "Battery" (50% cap, no turbo). It is now Performance, but scaling_max_freq is still 2.6 GHz on all policies and needs root to reset.
- `862e88f`: only instances spawned by the spell system (kits, missiles) keep full-range particles. Placed doodads the classifier calls effects (Felwood mushrooms, wisps, portals) are now ranged by size and sleep off screen like everything else.
  - Goldshire: live particles 20k -> 10k; update 0.75 -> 0.38 ms; draw 0.42 -> 0.26 ms.
  - Particle total per frame is now about 0.6 ms, down from 4-6 ms at the start of the day.
- Windows package rebuilt at `10df8c2` (map, both enhancement batches, particle range). The zip was refreshed and the share restarted; it serves on port 8081.
- Next: incremental FrameXML layout. An agent is working on it; it has a `WOWEE_LAYOUT_VERIFY` mode that compares against the full layout.

## FrameXML layout, idle heartbeats

- `13efbca`: incremental FrameXML layout.
  - A label or texture is re-sized only after something took it writable; a change mark is kept per widget id.
  - Text measurements are cached.
  - The draw order comes from a byte mirror of the visible flag.
  - `WOWEE_LAYOUT_VERIFY=1` compares against the full layout every 30 frames, and stayed clean through the scripted map test. `WOWEE_LAYOUT_FULL=1` restores the old path.
  - Goldshire: addonWidgets/layout 2.3 -> 0.51 ms; addonWidgets 3.0 -> 1.35 ms. 16 of 22229 labels and textures are sized per frame.
- `1564d38`: MSG_MOVE_HEARTBEAT is sent only while a locomotion flag is set, as the stock client does. Taxi flights and the classic combat sync keep their own cadence.
- Totals at Goldshire since this morning, same 2.6 GHz clocks:
  - particles 4-6 ms -> 0.6 ms
  - UI layout 2.3 -> 0.5 ms
  - the test window now holds the 60 fps vsync cap (it was 40-47).
- Next candidates: shadow pass (1.5 ms), renderer->update (2.2 ms), addon OnUpdate (1.0 ms), and the full-tree scans left in hitTestFor and the tooltip loop.
- `7abe121`: `expireMessages` read all 28k widgets every frame (0.87 ms) to find message frames. The tree now keeps a list of the ids AddMessage has written to.
  - Goldshire: addon OnUpdate 1.2 -> 0.34 ms.
  - The profile now also names the costliest OnUpdate handlers. They total about 0.13 ms; UIParent and WorldFrame lead.
- Uncapped Goldshire: about 71 fps, 14.1 ms per frame (test window, 2.6 GHz).
- In progress: a shadow caster cache. It never hits yet ("kept in 0 frames"), so the agent is adding miss-reason counters.

## Shadow cache, voices, font check

- `271e53f`: the static shadow caster list is kept between frames. Goldshire: kept in 747 of 803 frames; shadow/m2 1.16 -> 0.45 ms; 72.5 -> 80.7 fps uncapped. `WOWEE_SHADOW_CACHE=0` restores the old per-frame cull.
- `8f20f51`: the hit test and tooltip sizing read only visible widgets.
- `7e10b7e`: creatures with no NPCSounds row say nothing. Spirit healers had played the gnome "Hey!" through the GENERIC fallback.
- `d65dda7`: error speech comes from VocalUISounds.dbc and SoundEntries kits (the listed takes, at their volume).
- Font size, from Wow.exe:
  - FontHeight is the em size: px = round(screenH * h/768), clamped to 32 px for rasterising and scaled up above that.
  - Advances are floor(adv) + 1 px, +1 more for THICK outlines.
  - So Wowee's em mapping is the right base. Stock text is 6-12% wider than ours, not narrower.
  - Long zone names wrap in the 512-wide ZoneTextString in stock too.
  - Not changed yet. Notes and decompiled output are in scratchpad/font-re.
- Lab server set to noon and clear weather for Jered:
  - TZ=LAB+9 in docker-compose.override.yml.
  - game_weather chances zeroed; backup in game_weather_lab_backup.
- Lab account JTRAIN20 created for Jered's own test client (HOME ~/workplace/wowee-jtrain-home).

## Jered's playtest, Sunday afternoon (JTRAIN20)

- `9e34ac6`: the intro cinematic is answered with COMPLETE_CINEMATIC. NEXT_CINEMATIC_CAMERA had told AzerothCore that the cinematic started, so a new character's viewpoint rode the intro camera for about 2 min and nearby NPCs never arrived. The frame profile now also reports spawn queue depths.
- `90598cd`: stock day/night directions from Wow.exe.
  - The light comes from DayNight 0x7eea90: phi 127/110 deg, azimuth 225 deg, half-day period, using the client's own cubic sin/cos.
  - Sun and moon sprites come from 0x7eecc0; the noon sun sits at 85 deg.
- `0d98a19`: damage flash is off by default. Stock has no per-hit flash; its only red edge is screenEdgeFlash while on a hate list.
- In progress, one agent each:
  - remote casters show an unarmed ready stance and their cast animations are overridden; Seal of Righteousness replays its kit; the selection circle colour needs checking
  - stock loot sounds
  - stock equipment texture layer order (the belt is hidden under the pants; the layering is duplicated four times)
  - stale questgiver status after objectives complete; the learn-spell sound
- `9290471`: loot sounds as Wow.exe plays them.
  - Coin sound (LOOTWINDOWCOINSOUND) on every change to coinage, and when honor or arena points rise.
  - Item pickup sound when a loot slot is clicked.
  - Item put-down sound on ITEM_PUSH_RESULT.
  - The loot window itself plays nothing.
- Open: "jumping during a cast doesn't interrupt".
  - MovementHandler::sendMovement cancels the cast on MSG_MOVE_JUMP, and nothing gates jump input while casting.
  - Can't reproduce from a Lua test yet: JUMP polls the keyboard, which SDL_PushEvent doesn't update, and JumpOrAscendStart isn't implemented.
  - Next: log the cancel path and implement JumpOrAscendStart/AscendStop (stock FrameXML bindings use them).
- WPTEST has gmlevel 3 on the lab (for test setup commands).
- `fbe620b`: quest givers are re-asked when the quest log changes.
  - AzerothCore completes most quests through update fields alone, with no packet.
  - The client now sends CMSG_QUESTGIVER_STATUS_MULTIPLE_QUERY on any slot-state change.
  - Learning a spell no longer plays the quest-accept sound; stock's is SpellVisualKit 362 from the server.
- `aa42cc2`: equipment textures are layered in the stock CCharacterComponent order (DAT_009f6a00), with one shared helper for all four composite sites. Belts now paint over trousers.
- `47cc6f1`: combat animations.
  - Remote casters take a weapon stance and hold their cast loop. The sheath byte stays 0 for bots, so it can't be used.
  - One-shot spell kits hold their last frame instead of looping back; this removes the double Judgement hammer and the repeating seal.
  - Target auras load from the cache on target change.
- All 239 tests pass, including sweep_guard. Smoke login is clean.
- Still open:
  - Jump-during-cast interrupt; needs JumpOrAscendStart and logging.
  - LEARNED_SPELL_IN_TAB passes the spell id instead of the tab index.
  - Bots' weapons stay sheathed (server sheath 0). Need a stock comparison of whether the stock client draws them anyway.
  - Restart Jered's client.
- `1ad8cad`: JumpOrAscendStart/AscendStop.
  - Scripted presses skip the focus gate and are queued to the next input frame.
  - "Movement: ... interrupts the cast" is now logged.
  - Jump-interrupt still unconfirmed. In a test client that isn't focused, `grounded` stays false, so a scripted jump never fires. Jered's next jump mid-cast will show in his log.
- `343fc96`: LEARNED_SPELL_IN_TAB carries the spellbook tab index, so the tab flashes.
- Windows package rebuild from 343fc96 started.
- Windows package rebuilt from 343fc96, zip refreshed, share restarted (port 8081, 22:49 UTC).

## Sunday evening (Jered asleep; hourly checks until ~3:30)

- `50c3485`: FontString metrics as Wow.exe measures them.
  - em px, raster clamp at 32 px, floor(adv)+1 (+1 for THICK), negative kerning only, outline pad, last-glyph cell.
  - "Stranglethorn Vale" at 62 THICK = 601 units, matching stock. Layout verify stayed clean in game, and all tests pass.
- `6df47d7`: interface sound audit.
  - Error.wav removed. Stock never plays it, and errors had played it twice.
  - igQuestFailed and Aggro_Pulled_Aggro per the Wow.exe error table and threat code.
  - Doubled sounds removed: whisper, ping, achievement, food/drink, Charge, duel.
  - Names resolve through SoundEntries (random file, row volume).
- Test account WPTEST2/WPTEST2 (character Wptestb, HOME scratchpad/testhome2), for when Jered uses WPTEST in the stock client.
- Next: stock sounds Wowee never plays (party join and decline, taxi node discovered, friend online, LFG ready/denied, PvP queue, aggro warning). SMSG_PLAY_SOUND to use the row's volume and a random file. Level-up and achievement sounds on other players.
- `31f9bd5`: M2Instance cull fields packed into one cache line; the WMO draw ranges are joined. Northshire: 86 -> 93.5 fps uncapped.
- `c56b190`: gatherLocalLights skips light-giving M2s beyond 300 yd before their bone transforms. record+ubo 0.6 -> 0.15 ms; ~97 fps.
  - Same 2.6 GHz ceiling and test window throughout. This morning's Goldshire figure was 40-47 fps.
- `8c4e518`: the stock event sounds Wowee never played, from the Wow.exe UI message table and LFG/threat code. Server sounds now play their row's random file at the row volume. M2 $SND events play, so level-up and achievement sound at any player. Verified with `.levelup`: LevelUp.m2 showed for us and for a nearby bot; all tests pass.
