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

### Ground clutter streamed by distance

Clutter placements stay with the tile; instances exist only for tiles within
the clutter distance (plus 20 yards, released 80 yards further out). Target:
125k instances down to the real doodads plus a few tiles of clutter.

## Next

See the backlog in the loop prompt: jump phases, M2 per-frame scans, FrameXML
layout, present mode, WMO ambience, footsteps, Windows package.
