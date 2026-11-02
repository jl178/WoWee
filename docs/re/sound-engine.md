# Sound engine (stock 3.3.5a build 12340)

How the stock client plays a sound once it has decided which one: the
SoundEntries row, the voice it gets, how loud it is at a distance, how it
mixes, and how zone music and ambience are scheduled. Which sound plays for
which event is in `docs/overnight-2026-10-31.md` (the "sound audit" and
"stock event sounds" entries). Reverse engineered from `Wow.exe` with Ghidra;
addresses are virtual addresses in that binary. The last sections compare it
with Wowee and list what to listen for.

The code is in three layers:

| Layer | Range | Source unit |
|---|---|---|
| SoundInterface2 ("SI2"): kits, categories, zone sounds, advanced kits | 0x4c4000-0x4d3000 | `.\SoundInterface2.cpp`, `...Internal.cpp`, `...ZoneSounds.cpp`, `...AdvancedKitProperties.cpp`, `...DSP.cpp` |
| SoundEngine ("SE"): channels, groups, fades, FMOD calls | 0x875000-0x882000 | `.\SoundEngine.cpp` |
| FMOD Ex, linked in statically | 0x8d0000- | `fmod_*.cpp` |

The FMOD C++ API wrappers sit in order at 0x8d0900-0x8d1c80, each
`validate(handle)` then the ChannelI/SystemI/SoundGroupI method. The ones
that matter here: Channel `setVolume` 0x8d0990, `setFrequency` 0x8d0a00,
`getFrequency` 0x8d0a30, `setPriority` 0x8d0ad0, `setPosition` 0x8d0b40,
`set3DAttributes` 0x8d0c70, `set3DMinMaxDistance` 0x8d0d00,
`get3DMinMaxDistance` 0x8d0d40, `set3DPanLevel` 0x8d0e00, `isPlaying`
0x8d0ea0, `isVirtual` 0x8d0ee0, `getMode` 0x8d0fa0; System `init` 0x8d13f0,
`setSoftwareChannels` 0x8d12f0, `set3DSettings` 0x8d1480,
`set3DListenerAttributes` 0x8d14c0, `set3DRolloffCallback` 0x8d1540,
`createSoundGroup` 0x8d16e0, `playSound` 0x8d1710; SoundGroup
`setMaxAudible` 0x8d1b60, `setMaxAudibleBehavior` 0x8d1b90,
`setMuteFadeSpeed` 0x8d1bc0.

## 1. CVars and defaults

Registered at 0x4d1050 (device) and 0x4d1600 (playback). Defaults are the
strings the registration passes.

| CVar | Default | Use |
|---|---|---|
| Sound_NumChannels | 32 | real voices; init clamps to 12..128 (0x87c710) |
| Sound_OutputQuality | 1 | 0: 22050 Hz, 1: 44100, 2: 48000 |
| Sound_EnableHardware | 0 | software mixing only |
| Sound_EnableReverb | **0** | EAX reverb (SoundProviderPreferences) off |
| Sound_EnableSoftwareHRTF | 0 | FMOD_INIT_SOFTWARE_HRTF |
| Sound_EnableDSPEffects | 1 on 2+ cores | voice filters (the Death Knight one) |
| Sound_MasterVolume | 1.0 | master channel group |
| Sound_SFXVolume | 1.0 | group "SFX" |
| Sound_MusicVolume | **0.4** | groups "MUSIC" and "SCRIPTMUSIC" |
| Sound_AmbienceVolume | **0.6** | group "AMBIENCE" |
| Sound_EnableAllSound / SFX / Music / Ambience | 1 | mute the groups (0x4d13e0..0x4d1590) |
| Sound_EnableErrorSpeech | 1 | VocalUISounds lines (0x986d00) |
| Sound_EnableEmoteSounds | 1 | emote voice (0x746d60) |
| Sound_ZoneMusicNoDelay | 0 | the Sound panel's **Loop Music**: no silence between tracks (0x4c87d0) |
| Sound_ListenerAtCharacter | 1 | listener at the character (0x4fa5f0) |
| Sound_EnableSoundWhenGameIsInBG | 0 | mute when the window loses focus (0x4c5dc0) |
| Sound_EnablePetSounds, ArmorFoleySoundForSelf/Others | 1 | |
| Sound_MaxCacheSizeInBytes | 16777216 | |
| Sound_MaxCacheableSizeInBytes | 1048576 | larger files stream (clamped to 2 MB) |
| Sound_ListenerBackDist, Sound_ListenerUpDist | (unregistered) | read only; defaults 2.0 and 4.0 |
| Sound_VirtualPlayWindow | (unregistered) | read only; default 400 ms |

There is no dialog volume in 3.3.5 (Sound_DialogVolume is a 4.x CVar), and
no SoundMaxHardwareChannels/SoundMaxSoftwareChannels: the channel count is
Sound_NumChannels alone.

FMOD is initialised (0x87c710) with 512 virtual channels
(`System::init(0x200, FMOD_INIT_3D_RIGHTHANDED [| SOFTWARE_HRTF])`),
`setHardwareChannels(0,0,0,0)`, `setSoftwareChannels(Sound_NumChannels)`,
`set3DSettings(doppler 1.0, distance factor 1.0, rolloff 4.0)` and
`set3DRolloffCallback(0x878320)`. FMOD keeps the 32 most important of the
512 playing; the rest are virtual (silent, still running).

## 2. SoundEntries

### What the kit builder reads (0x4cd4a0)
Kits are built once from SoundEntries.dbc into an array by id
(`0xb4ad44[id]`, 0xb0 bytes each). Of the row's 30 columns only these are
read:

| Kit field | Column | Note |
|---|---|---|
| +0x00 name | Name | also the FMOD sound group name |
| +0x04.. file paths | File[10] + DirectoryBase | `dir + '\' + file`; the `\` only when dir lacks one. **Stops at the first empty File** |
| +0x40 file count | | |
| +0x48.. weights | Freq[i] for each file kept | |
| +0x2c min | MinDistance | FMOD min distance |
| +0x30 max | DistanceCutoff | FMOD max distance |
| +0x34 volume | VolumeFloat | |
| +0x38 id | ID | |
| +0x3c flags | Flags | |
| +0xa8/+0xac | SoundEntriesAdvancedID, its row | |

**SoundType and EAXDef are never read.** The category a sound plays in comes
from the caller (section 3), not from the row.

Flags the engine reads (0x4c6a40); the 3.3.5 table sets no others:

| Bit | Rows | Meaning |
|---|---|---|
| 0x20 | 76 | no duplicates: refused (code 0xf) while the same kit plays; the active list is 0xb4a398 |
| 0x200 | 1218 | loop, unless the caller forces loop (props[7] = 1) or one-shot (2) |
| 0x400 | 126 | pitch: frequency x uniform [0.85, 1.15) (0x9f23d0/0x9f23cc) via 0x878760 |
| 0x800 | 7 | volume + uniform [-0.15, 0.15) (0x9f23d8/0x9f23d4), then clamped |

Two FMOD sound groups are made per kit, named after it, each with
`setMaxAudible(3)` (0x4cbdb0 -> 0x878ad0).

### File selection (0x4cbac0)
`pick(mode = props[6] (default 2), fileIndex = props[3] (default -1))`.
- `fileIndex >= 0` plays that file (clamped). The NPC annoyed lines use it.
- One file: always it.
- Otherwise a bag at kit+0x70 (10 counters) and the last pick at kit+0x44:
  1. sum the bag over every file except the last pick;
  2. if 0, refill the bag (mode 0: all 1; modes 1 and 2: Freq) and try
     again; on the third try forget the last pick; after that, give up
     (no file, the play fails);
  3. `r = (rand * sum) >> 32`; walk the files (skipping the last pick)
     subtracting each bag count until `r < 0`; that file is chosen, its bag
     count drops by one;
  4. mode 1 then empties the bag (weighted draw with replacement); mode 2,
     the default, keeps it (draw without replacement); mode 0 always takes
     the first available (sequential).

With Freq 1 for every file - 16634 of the 16669 weights in rows with more than
one file - mode 2
plays each file once in random order before any repeats and never plays the
same file twice running. The bag lives on the kit, so it carries across all
callers of the row.

### Volume and pitch (0x4c6a40)
- `props[1]` in (0, 1] replaces the volume outright; otherwise
  `volume = row.VolumeFloat * props[2]` (default 1.0), plus the 0x800
  spread; `0x879710` clamps to [0, 1].
- `props[4]`, `props[5]`: fade-in and fade-out seconds (linear, SE update
  ~0x87a3d0 adds `dt/fadeIn` to the channel's fade gain each frame).

### SoundEntriesAdvanced (0x4cb580, 0x4ca7b0 per frame)
1641 rows. A kit with an advanced row (and no caller-supplied instance) is
not played directly: an instance is queued and the per-frame update plays
it. Columns: ID, SoundEntryID, InnerRadius2D, TimeA..D, RandomOffsetRange,
Usage, TimeIntervalMin/Max, VolumeSliderCategory, DuckToSFX/Music/Ambience,
InnerRadiusOfInfluence, OuterRadiusOfInfluence, TimeToDuck, TimeToUnduck,
InsideAngle, OutsideAngle, OutsideVolume, OuterRadius2D, Name.
- **Usage** 0: play once while inside the time window; 1: replay every
  random TimeIntervalMin..Max ms while in the window; 2: play at once.
- **TimeA..D** are game-clock ms of the day (wrapping at 86400000), offset
  by a random RandomOffsetRange: the instance volume ramps 0->1 from A to
  B, holds to C, ramps 1->0 to D (0x4ca7b0, kept at instance+0x54 and
  applied through the volume callback 0x4cb220).
- **InnerRadius2D / OuterRadius2D**: `set3DPanLevel` (0x8787b0): 0 (heard
  in the head, unpanned) inside the inner radius, 1 (fully 3D) outside the
  outer, linear between.
- **Ducking** (0x4ca1e0, 0x4ca4f0): while the listener is within
  InnerRadiusOfInfluence (or OuterRadiusOfInfluence once ducking), the SFX,
  music and ambience categories are pulled down to DuckToSFX/Music/Ambience
  over TimeToDuck ms, and back to 1 over TimeToUnduck once out of range.
  The lowest duck of all active duckers wins; the ducker itself is exempt.
  The boss and narrator lines (0.3 SFX, 0.75 music, 1000/3000 ms) use it.
- **VolumeSliderCategory** becomes the instance's category (props[0]).
- **Inside/OutsideAngle, OutsideVolume**: FMOD cone settings.

## 3. Playing a kit: PlaySoundKit (0x4c6a40)

`PlaySoundKit(kitId, position, userHandle, props, advInstance, outHandle, ...)`.
`position` null means 2D. `props` (0x4c5990 fills the defaults):

| props | Default | Meaning |
|---|---|---|
| [0] | 0 | category (below) |
| [1] | -1 | volume override |
| [2] | 1.0 | volume multiplier |
| [3] | -1 | file index |
| [4], [5] | 0 | fade in, fade out (s) |
| [6] | 2 | file selection mode |
| [7] | 0 | loop: 0 from flag 0x200, 1 loop, 2 once |
| [8] | -1 | FMOD priority (-1 and 128 leave FMOD's 128) |
| [9] | 0 | 2 forces the no-duplicate flag on the kit, 1 clears it |
| [0xb] | -1 | max distance override |
| [0xc], [0xd] | 1, 0.5 | the kit's second sound group behaviour and mute fade |
| [0xe] | 0 | keep alive: exempt from the virtual window and the first group |
| [0x12..0x16] | | cone |

Checks, in order, each returning an error code without playing:
1. the kit exists and has files;
2. sound on, not muted (Sound_EnableAllSound and the category's switch);
3. **category cap**: `count[type] >= cap[type]` (counts at 0xb4a340, caps
   at 0x9f2268) -> 0xe. The new sound is refused; nothing is stolen;
4. no-duplicate kit already playing -> 0xf;
5. a file to play (section 2).

Then it creates the sound (2D: 0x87f7a0, 3D: 0x87f810 -> 0x87ee60) with the
mode `FMOD_2D|SOFTWARE` or `FMOD_3D`, loop or not, plus
`NONBLOCKING|IGNORETAGS|VIRTUAL_PLAYFROMSTART`; `.mp3` streams, a `.wav`
up to 1 MB is cached. For 3D: `set3DAttributes(pos, velocity = null)` and
`set3DMinMaxDistance(MinDistance, DistanceCutoff or props[0xb])`. When the
load completes (0x87b5f0): `setPriority(props[8])` if it is not 128,
`setFrequency(freq * pitch)` for 0x400 rows, cone settings, then the volume
(0x878fd0) and unpause. A loop with the "random start" flag - every loop
that is not music (kit+0x88 = not MUSIC/SCRIPTMUSIC) - starts at a random
byte offset (0x879760).

### Categories (props[0])
Groups by name at 0x9f2010, caps at 0x9f2268. 7..17 all mix in "SFX".

| # | Group | Cap | Used by |
|---|---|---|---|
| 0 | SFX | - | almost everything |
| 1 | MUSIC | - | zone music |
| 2 | AMBIENCE | - | zone ambience, SoundEmitters-style loops (0x4d2390) |
| 3 | CINEMATIC | - | cinematics (0x528af0); no master volume parent |
| 4 | SCRIPTSOUND | - | PlaySoundFile |
| 5 | SCRIPTMUSIC | - | PlayMusic |
| 6 | RACIALCINEMATIC | 1 | (0x526370) |
| 7 | SFX | 1 | error speech, VocalUISounds (0x986d00) |
| 8 | SFX | 2 | a unit's own kit at unit+0x8f0 (0x746720) |
| 9 | SFX | 1 | creature exertion, CreatureSoundData kinds 0/1 (0x747310) |
| 10 | SFX | 2 | weapon swing, WeaponSwingSounds2 (0x4d0720) |
| 11 | SFX | 2 | creature injury, kinds 2/3/9 |
| 12 | SFX | 1 | the active player's own injury cries (0x7631a0) |
| 13 | SFX | 6 | other units' footsteps (0x4cf170) |
| 14 | SFX | 4 | weapon impacts (0x4d0540, 0x4d0660) and $AH0..$AH3 (0x756240) |
| 15 | SFX | 1 | exertion of models with flag 0x20 |
| 16 | SFX | 2 | injury of models with flag 0x20 |
| 17 | SFX | 4 | the player's own footsteps |

### Priorities
FMOD priority 0 is most important. Defaults: 128. Zone music and ambience:
100 (0x4c9910). Sounds of the player's own unit (voice, swings, emotes,
greetings it triggers, its target's in some paths): 110 (0x6e at 0x746720,
0x747310, 0x7631a0, 0x7469c0, 0x4cfc10, 0x73a890 ...). The player's own
footsteps: 115 (0x4cf170). Each frame (0x87a580) a one-shot at 128 that
is playing and not virtual is raised to 127, so newer default sounds cannot
take its voice.

### Voices and stealing
- **Channel limit**: Sound_NumChannels (32) real voices, FMOD's virtual
  voice order (priority, then audibility) decides which 32. Music and
  ambience at 100 always keep theirs.
- **Virtual window** (0x87a580): a one-shot that is still virtual 400 ms
  after it started (Sound_VirtualPlayWindow, unregistered) is stopped; one
  with props[0xe] is exempt.
- **Per kit**: the kit's first sound group (STEALLOWEST, max 3) holds the
  sound for its first 400 ms, then it moves to the second group (behaviour
  props[0xc]: 0 mute with fade, 1 STEALLOWEST (default), 2 fail; max 3).
  So **at most three copies of one SoundEntries row sound at once; a fourth
  replaces the least important of them.**
- **Per category**: the caps in the table above, refusing newcomers.

### Own-character sounds
With Sound_ListenerAtCharacter on, sounds of the player's own unit are
played **2D** (position nulled) at **0.65** x volume (0x9f2818) and
priority 110: unit sounds 0x747310/0x7631a0, footsteps 0x4cf170 (max
distance 20, or 50 by a unit-size test at 0x754f40), greetings 0x73a890, emotes 0x746d60,
spell sounds 0x7469c0.

## 4. 3D audio

### Distance: the rolloff callback (0x878320 -> 0x8782b0)
FMOD calls it for every 3D channel with the distance in yards; it reads
the channel's min/max (MinDistance, DistanceCutoff) and returns:

```
d >= max          0
d <= min          1
otherwise         g = min / (min + 4 * (d - min))         ; FMOD inverse, rolloff 4
                  if d > 0.9 * max:
                      g *= 1 - (d - 0.9 max) / (max - 0.9 max)
```

With the commonest row (8, 45): full to 8 yd, 0.2 at 16, 0.08 at 30,
0.06 at 40.5, then a straight fade to 0 at 45. The 4 is the same constant
passed to `set3DSettings` (0x9e8d2c); 0.9 is 0xa02a34.

### Listener (WorldFrame 0x4fa5f0 -> 0x4c5b20 -> 0x879320)
- Sound_ListenerAtCharacter on (default) and the player has a unit: the
  listener is at the unit's position `- cameraForward * BackDist +
  (0, 0, UpDist)`, i.e. **2 yd back along the view direction and 4 yd up**
  from the character's feet, oriented by the camera (forward = camera
  forward, up = world up). A unit on a transport uses its world transform.
- Off, or no unit: the camera's position and orientation.
- Velocity is always 0.

### Panning, Doppler, occlusion
- Panning: FMOD's 3D panning (stereo or the speaker mode), full 3D unless
  an advanced row's InnerRadius2D pulls the pan level towards 2D.
- **No Doppler**: doppler scale 1, but `set3DAttributes` is always given
  a null velocity, so no channel ever moves.
- **No occlusion**: no occlusion or geometry calls are made; walls do not
  muffle. Indoors changes only the reverb (when Sound_EnableReverb is on)
  and the zone sounds (section 6).

## 5. Mixing

Channel volume (0x878fd0) =
`clamp(kit volume) x fade x product over the channel group chain (volume x mute)
 x volume callback`.
- The group chain is the category's group ("SFX", "MUSIC", ...) and its
  parent "<master>". Sound_SFXVolume sets "SFX", Sound_MusicVolume "MUSIC"
  and "SCRIPTMUSIC", Sound_AmbienceVolume "AMBIENCE", Sound_MasterVolume
  "<master>" (0x879460). Mute switches set a group's mute factor 0/1.
  CINEMATIC has no parent and ignores the master slider.
- The volume callback (0x4cb220): the advanced instance's time-window
  volume times the lowest duck level for the category (SFX for 0 and 7+,
  MUSIC 1, AMBIENCE 2; none for 3..6).
- **Background** (0x4c5dc0, focus event): with
  Sound_EnableSoundWhenGameIsInBG 0, losing focus mutes the master group
  and CINEMATIC; regaining it unmutes.

## 6. Zone music and ambience (SoundInterface2ZoneSounds.cpp)

### Sources and slots
Eleven priority slots (`0xb4aab0 + slot*0x24`), each holding a music day
and night sound, silence min and max day/night, an ambience day/night, and
a SoundProviderPreferences id. Slot 10 is the highest.

| Slot | Filled by |
|---|---|
| 0 | the area (0x78e9a0/0x78eeb0): music, ambience, reverb |
| 1 | WorldStateZoneSounds (0x4cbe70) |
| 2 | WorldChunkSounds (0x4c68e0; empty table in 3.3.5) |
| 5 | the area's zone intro (ZoneIntroMusicTable) |
| 6 | WorldStateZoneSounds' intro |
| 7 | WorldChunkSounds' intro |
| 10 | 0x4c8fa0 (glue / script override of music and ambience) |

**Area resolution (0x78e9a0)**, from the area under the player, its parent
zone, and the WMOAreaTable rows of the WMO group the player is in (the
group's own row, then the WMO's -1 row):
- intro, music, ambience: the first non-zero of WMO group, WMO root, then
  - outdoors (or in an exterior group): the area, then its parent zone;
  - **indoors (the group lacks the exterior flag): nothing more** - a room
    whose WMOAreaTable rows name no music or ambience has none.
- reverb: the same order with the SoundProviderPref columns.
- The update runs every frame (EVENT_ID_IDLE). An area change applies at
  once; there is no switching delay.

### Day and night (0x4c9850)
Day is game time 05:30 to 21:00, night otherwise. The ZoneMusic and
SoundAmbience day and night entries swap at those times.

### Music schedule (0x4c9910, 0x4c88b0)
Each frame the music choice walks slots 10 to 0:
- slots 5..7 (intros): if an intro is playing, keep it (an intro is never
  interrupted); else the slot's intro if its cooldown (0xb4acd8[sound])
  has passed;
- slots 0..2: the slot's day or night sound, unless the silence is still
  running (`now < 0xb4aaa0` and Sound_ZoneMusicNoDelay off) -> "wait".

If the choice is a sound other than the one playing (and not "wait"), it is
started (0x4c7140): props type MUSIC, mode 2, priority 100, fade in **4 s**
(0.01 s when nothing was playing), fade out **4 s**; the old track fades
out over its 4 s meanwhile - a crossfade between two voices. A choice of
nothing stops the music with its 4 s fade.

When a track ends (callbacks):
- zone track (0x4c8ab0): if the same sound is still the choice, the next
  may start after a random `SilenceIntervalMin..Max` (day or night by the
  clock then; ZoneMusic values are ms, 180000-300000 for most of the old
  world); if the choice moved on, at once;
- intro (0x4c9080): its cooldown becomes `end + MinDelayMinutes * 60000`.
  ZoneIntroMusicTable.Priority is not read.

When the area's music changes (0x4c8d80) to a choice that differs from the
playing sound and is not "wait", the silence is cleared (0xb4aaa0 = 0); an
area with no music clears it too. A silence already running is not cut
short by walking into a new area with music.

### Ambience (0x4c9910, 0x4c86f0)
The ambience choice is the top slot's day or night SoundAmbience sound. A
change starts it with props type AMBIENCE, loop, priority 100, fade in
**5 s**, fade out **5 s**, crossfading over two voices. As a loop that is
not music it starts at a random point of the file (0x879760). A choice of
none fades the loop out.

## 7. Creature, footstep and voice sounds

- **CreatureSoundData kinds** (0x747310 creatures, 0x7631a0 players,
  0x7461e0 maps kind -> column): 0 exertion, 1 exertion critical, 2 injury,
  3 injury critical, 4 stun, 5 stand, 7 wing flap, 8 alert, 9 injury
  crushing blow, 10 wing glide, 11 jump start, 12 jump end. A roll of
  0..99 against the kind's chance (0xadb5d8 creatures: 70, 100, 60, 100,
  100, 40, ...; 0xadba74 players: 35, 100, 30, ...) unless forced.
  **Stand (5) is spaced at least 10 s apart, client-wide** (0xca12d4).
  Categories 9/11 (or 15/16), 12 for the player's own cries.
- **Footsteps** (0x4cf170, 0x747240): FootstepTerrainLookup by the model's
  footstep id and the ground's terrain sound. No timer beyond the
  animation's events; other units' steps are category 13 (six at once),
  the player's own 17 (four), in 2D at 0.65 and priority 115.
- **NPC greetings** (0x72a680, 0x746900): one counter for the client
  (0xca1190), reset when another NPC is spoken to. Clicks 0..4 play the
  hello line; from 5 the annoyed lines play in order, one file each click
  (props[3] = click - 5); after the last annoyed line, hello and the count
  restarts. **A creature never talks over itself** (its handle at
  unit+0x8f4 is checked); there is no time-based cooldown. The count only
  advances when the line actually starts.
- **Error speech** (0x986d00): only if Sound_EnableErrorSpeech and no line
  is still speaking (handle 0xdcec08); category 7 (one at a time). No timer.
- **Emotes** (0x746d60): props[9] = 2 forces no-duplicates for the emote
  kit; the unit's handle cuts its previous emote line.

## 8. Wowee: gap table

| Area | Stock | Wowee before | Now |
|---|---|---|---|
| Distance curve | min/(min+4(d-min)), fade over last 10% of cutoff, 0 past it | linear from a clamped min to the cutoff | `stock::rolloffGain`, applied per frame in `AudioEngine::update` |
| Min distance | SoundEntries.MinDistance | 8 or a quarter of the cutoff; rows' values only for emitters | rows' MinDistance through `SoundOptions` (combat, NPC, emitters, UI-at-point) |
| Default cutoff | per row | 100 yd | 45 yd (the table's commonest) |
| Listener | character - 2 x camera forward + 4 up | character + 1.5 up | `stock::listenerPosition` (renderer.cpp) |
| Doppler | none | miniaudio default (no velocities set) | doppler factor 0 |
| Voice limit | 32 real, priority then audibility, 400 ms virtual window | none | `stock::decideVoice` in `AudioEngine::admitVoice`, 32 voices, music reserved |
| Per-kit limit | 3 audible, steal lowest | none | 3, steal least important |
| Category caps | 18 categories, e.g. 6 footsteps, 4 impacts, 1 exertion | none | `SoundOptions::category` on footsteps, impacts, swings, injuries, exertion, error speech |
| Priorities | 128, 127 once heard, 110 own, 115 own steps, 100 zone | none | in the voice limiter; 127 bump in update |
| File choice | Freq bag, no repeats until exhausted, never same twice | uniform random (some: avoid last) | `SoundData::nextFile` (`stock::pickKitFile`), bag on the cached row |
| Kit gaps | stop at first empty file | skipped gaps | stop at first |
| Flags 0x400/0x800 | random pitch / volume | ignored; Wowee jitter on impacts and steps | `stock::kitPitch/kitVolume` where kits play; impact jitter dropped |
| Day window | 05:30-21:00 | 06:00-20:00 | 05:30-21:00 (`day_night.hpp`) |
| Music silence | not cut short by area change; cleared by no-music area | reset on every set change | stock (`ZoneMusicController`) |
| Intro | preempts when its cooldown (from its end) passes; never interrupted | only on area entry, cooldown from start | stock |
| Day/night music change | crossfade at once | next track only | at once |
| Crossfade | 4 s, 0.01 s from silence | 3 s | 4 s / 0.01 s |
| Loop Music | Sound_ZoneMusicNoDelay: no silence between tracks | repeat the same track | no-delay |
| Indoors | WMO rows only; none -> no music/ambience | area's music and ambience | WMO rows only (music and table ambience) |
| Night-silent sets | silent at night (2 sets) | played the day side | silent |
| Ambience fades | 5 s in and out | 1.5 s | 5 s |
| Ambience loop start | random point | file start | random point (area loops, emitters) |
| NPC greeting | no talk-over; 5 hellos then annoyed lines in order; no timer | 2 s per-NPC timer | stock (`NpcVoiceManager::playFromTables`) |
| Creature exertion | 70% roll, one at a time world-wide | 1.25 s per-creature timer | stock |
| Error speech | one line at a time, no timer | 1 s timer | no timer with the table; category cap 1 |
| Volume defaults | music 0.4, ambience 0.6 | music 30%, ambient 100% (settings_panel.hpp) | **not changed** (UI) |
| Reverb | Sound_EnableReverb 0: off by default | always on (WorldFx) | **not changed** |
| SoundEntriesAdvanced | 2D radius, time windows, ducking, usage | none | **not done** |
| Own-character 2D x0.65 | voice/steps/swings in 2D at 0.65, priority 110 | voices and steps 2D at their own levels | steps get priority 115 and cap 4; the 0.65 is **not applied** |
| Stand sound | 40%, 10 s client-wide spacing | (not played) | not done |
| Category volumes | slider x kit, groups SFX/MUSIC/AMBIENCE | per-manager scales | unchanged |
| Background mute | mute on focus loss by default | `setSuspended` on focus loss | already stock |

## 9. In game

- Distance: a creature fight 20 yd off is clearly quieter than one at 10,
  and things fall silent right at their cutoff without a hard cut (the last
  4.5 yd of a 45 yd sound fade). Footsteps and swings of mobs 30 yd off are
  barely there.
- A crowd (Stormwind bank, a raid): no more than six other people's steps
  at a time, no more than one creature grunt at a time, impacts capped at
  four; nothing louder than before, just thinner and cleaner.
- The same weapon impact or step never plays the same file twice running,
  and a kit with four files cycles all four before repeating.
- Music: arriving somewhere with music during a silence does not start it
  early; walking into a building whose WMOAreaTable names nothing fades the
  music and ambience out over 4-5 s; a day/night change (05:30, 21:00)
  crossfades at once; Loop Music plays the next track straight after the
  last. Entering Stormwind plays the intro; staying an hour plays it again
  over the city music.
- Ambience comes and goes over 5 s and does not open on the same bird each
  visit.
- Clicking an NPC: no greeting while its last line still plays; the sixth
  click starts the annoyed lines in order.
- The listener sits above and behind the character: the character's own
  3D-placed sounds are a little further off, sounds behind the camera pan
  slightly differently than before.

## 10. Unresolved

- SoundEntriesAdvanced (2D radius, time windows, ducking, intervals) is not
  implemented; boss lines do not duck music.
- Own-character sounds are not uniformly 2D at 0.65 with priority 110: the
  hand-made banks in activity, footstep and mount managers keep their own
  levels.
- The volume slider defaults and the reverb default live in the UI and
  WorldFx and were left alone: stock is music 0.4, ambience 0.6, reverb off.
- The FMOD virtual-voice behaviour is approximated: a sound that would be
  virtual is dropped at once instead of waiting 400 ms for a voice, and a
  looping sound that loses its voice is stopped rather than resumed.
- Kinds 4 (stun), 5 (stand, 10 s spacing), 8 (alert) and the player's own
  injury category 12 have no Wowee caller to attach to.
- The meaning of category 8 (unit+0x8f0 kit) and of model flag 0x20
  (categories 15/16) was not traced.
