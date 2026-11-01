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

## Next

See the backlog in the loop prompt: jump phases, M2 per-frame scans, FrameXML
layout, present mode, WMO ambience, footsteps, Windows package.
