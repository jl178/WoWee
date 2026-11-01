# Sound

How the client chooses music, ambience and the character's voice, and where
each table is read.

## Tables

All of this is in the player's extracted game data, in `dbfilesclient/`.
`audio::SoundData` (`include/audio/sound_data.hpp`) reads the tables once
when the asset manager is ready and answers lookups by id. Column indices
are for build 3.3.5.12340 and come from WoWDBDefs; a table with fewer
columns than that layout is skipped, so other expansions fall back to the
hand-written tables rather than read the wrong column.

| Table | Used for |
|---|---|
| `SoundEntries` | the files behind every sound id, the directory they sit in, `VolumeFloat` |
| `AreaTable` | per area: `ParentAreaID`, `AmbienceID`, `ZoneMusic`, `IntroSound`, name |
| `ZoneMusic` | a day sound and a night sound, and the silence interval between tracks |
| `SoundAmbience` | a day loop and a night loop |
| `ZoneIntroMusicTable` | the piece played on arriving in a city, and how often |
| `ChrRaces`, `CreatureDisplayInfo`, `CreatureModelData`, `CreatureSoundData` | a player model's exertion, wound and death sounds |

Area lookups follow `ParentAreaID` while the area's own column is 0.
Northshire Valley has no music of its own and plays Elwynn's; Stormwind's
districts play the city's.

## Music

`audio::ZoneMusicController` is the schedule, and it is pure: the audio
coordinator feeds it the area under the player, the time of day and whether
the music channel is busy, and applies the action it returns.

- Entering an area whose music set differs from the current one starts a
  track at once. A set that is the same (Northshire to Elwynn) changes
  nothing.
- A city with an intro piece plays that first, once per its
  `MinDelayMinutes`.
- When a track ends the channel rests for a random time inside the set's
  silence interval (three to five minutes for most of the old world), then
  the next track starts, from the day or the night pool for the time it is
  then.
- Entering an area with no set stops the music.
- Tracks play at the `VolumeFloat` of their `SoundEntries` row under the
  music volume setting, through `MusicManager::setTrackGain`.

The area id comes from the terrain chunk under the player (`MCNK.areaid`),
not from the server's zone, because the tables are keyed by area.

The tavern and blacksmith overrides in `AudioCoordinator` still apply while
the player is inside one of those buildings.

The client's own soundtrack (`assets/Original Music/`) joins the pool only
when **WoWee soundtrack** is on. It is off by default.

## Ambience

`SoundAmbience` names a day loop and a night loop per area, each about a
minute of audio meant to repeat. `AmbientSoundManager::setAreaAmbience`
loops the one for the current time of day, fades it out and the next one in
when the area or the time changes, and stops it indoors. Volume is the
row's `VolumeFloat` under the ambience volume setting.

Once the tables drive ambience, the hand-written zone and city libraries,
the bird and cricket one-shots and the wind bed stand down. The loops carry
those sounds already, and the stock client plays nothing else for an area.
Weather, water, fire emitters, blacksmith and bell sounds are unchanged.

## Voice

`ActivitySoundManager::setCharacterVoiceProfile` has an overload that takes
the race id and the `SoundData`. It fills the lists from guessed paths
first, as before, then replaces each list the tables have a row for:

- attack grunt: `CreatureSoundData.SoundExertionID`
- wound, critical wound, death: `SoundInjuryID`, `SoundInjuryCriticalID`,
  `SoundDeathID`
- jump and land: `SoundEntries` rows named `<Race><Sex>MainJump` and
  `<Race><Sex>MainLand`. Player models have no jump ids in
  `CreatureSoundData`; the rows exist for all ten races and both sexes,
  three of them with "Female" spelt "Feamle", which the lookup allows for.

Voice clips play at the row's `VolumeFloat` with no pitch change. The
stock client does not retune a grunt, and the random pitch the clips used
to get is a good part of why they sounded wrong. Landing from a jump now
plays the land grunt over the footstep.

## Testing

`tests/test_sound_data.cpp` and `tests/test_zone_music_controller.cpp`
build synthetic tables in the 3.3.5 layout and check the lookups, the
parent chain, the silence interval, day and night pools, the intro delay
and the stop on an area without music. They link no asset code.

`WOWEE_AUTO_LOGIN` and `WOWEE_AUTO_CHARACTER` (see `docs/perf_baseline.md`)
log a build in and put it in the world without a hand on the mouse, which
is how the music and ambience lines below were collected:

```
Zone music: Sound\Music\ZoneMusic\Forest\DayForest02.mp3 (set 1)
Zone ambience: Sound\Ambience\ZoneAmbience\ForestNormalDay.wav
```
