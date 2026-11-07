# Sound engine (stock 3.3.5a build 12340)

How the stock client plays a sound once it has decided which one: the
SoundEntries row, the voice it gets, how loud it is at a distance, how it
mixes, and how zone music and ambience are scheduled. Which sound plays for
which event is in `docs/overnight-2026-10-31.md` (the "sound audit" and
"stock event sounds" entries). Reverse engineered from `Wow.exe` with Ghidra;
addresses are virtual addresses in that binary. The last sections compare it
with Wowee and list what to listen for; section 11 is an end-to-end audit of
the events against a trace of every sound the client starts
(`WOWEE_SOUND_TRACE`).

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

### SoundEntriesAdvanced (SoundInterface2AdvancedKitProperties.cpp)
1641 rows; 1617 kits name one (kit +0xa8, from SoundEntries.SoundEntriesAdvancedID;
two rows' own SoundEntryID disagree with the kit that names them, and the
kit's column is the one followed). Columns: ID, SoundEntryID, InnerRadius2D,
TimeA..D, RandomOffsetRange, Usage, TimeIntervalMin/Max,
VolumeSliderCategory, DuckToSFX/Music/Ambience, InnerRadiusOfInfluence,
OuterRadiusOfInfluence, TimeToDuck, TimeToUnduck, InsideAngle, OutsideAngle,
OutsideVolume, OuterRadius2D, Name. Usage counts: 1487 rows 2, 132 rows 0,
22 rows 1; 1369 rows duck something (857 of them SFX 0.3 / music 0.75,
500 SFX 0.3 alone); 235 have 2D radii (69 of them 8..20 yd); one has a time
window (542, a waterfall emitter); every category is 0 but 17 ambience and
one music.

**Instance creation (0x4cb580)**, called from PlaySoundKit when the kit has a
row and the caller passed no instance:
`0x4cb580(row, kitId, position, coneDir = props[0x16], userHandle, 0, 0,
dupCheck = PlaySoundKit's 7th argument, forceLoop = props[7] == 1)`.
- dupCheck and a position: refused (0xf) while an instance of the same kit
  sits within sqrt(6) yd (0x4ca130, squared 6.0 at 0x9e8cf8). Every unit and
  model caller passes 1.
- The row is copied, normalised: TimeB, C, D each get +86400000 when before
  the time preceding it; a DuckTo* outside [0, 1] becomes 1;
  InnerRadiusOfInfluence and InsideAngle are clamped to their outer partners;
  forceLoop makes usage 0.
- Two draws (0x4c9d90, `min + (rand32 * (max - min)) >> 32`, or max when
  max <= min): the window offset +0x50 in [-RandomOffsetRange,
  RandomOffsetRange) and the interval countdown +0x5c in
  [TimeIntervalMin, TimeIntervalMax).
- The time volume +0x54 starts at 1. Usage 2 plays at once (0x4c9dc0) and,
  if it started, ducks at once (0x4ca1e0) whatever the distance.

**Play (0x4c9dc0)**: fresh props, category = VolumeSliderCategory.
- usage 0: loop (props[7] = 1), fades in and out **3 s** (0x9ebbc4; 0.7 s,
  0x9e2ec0, when the creator asked), keep-alive (props[0xe]), second sound
  group mute-with-fade (props[0xc] = 0); always at the instance position.
- usage 1 and 2: once (props[7] = 2), fades 0.01 s (0x9f1968); 2D when the
  position is (0, 0, 0).
- Inside/OutsideAngle and OutsideVolume go to props[0x13..0x15], the cone
  direction to props[0x16] (null when zero).

**Per frame (0x4ca7b0, under 0x4ca4f0's ducker update)**, `dt` = game-clock
ms since the last frame:
- not playing: usage 2, or usage 0 already stopped once (+0xc), or no
  handle: the instance is deleted (0x4ca080). Else, inside the window
  (0x4c9cf0): usage 0 plays; usage 1 counts +0x5c down by dt and plays when
  it goes negative, drawing a new interval. **The interval only runs while
  nothing plays.**
- playing, outside the window: stopped with its own fade (0x4c6390 with
  -1, 0x9e2ef4), +0xc set.
- playing, inside: usage 0 with more than one file and both interval
  bounds > 0 counts down and, at zero, stops (0x4c9d50) and plays again - a
  new file from the bag every 20-60 s for the emitter rows.
- playing: with OuterRadius2D > 0, the pan level from the listener
  distance: 0 inside InnerRadius2D, 1 beyond OuterRadius2D, linear between;
  `Channel::set3DPanLevel` (0x8787b0 -> 0x8d0e00). Without one, 1. **Only in
  this branch** is the radius of influence looked at: within
  InnerRadiusOfInfluence a ducker starts (0x4ca1e0).
- playing, usage not 2, with a window: the time volume. With `t` the clock
  (plus a day when D + offset passes midnight and the clock is still before
  A + offset): before B + offset `(t - A) / (B - A)`, before C + offset 1,
  after `1 - (t - C) / (D - C)`; clamped to [0, 1]. The ramps use the
  unshifted times, the window edges the shifted ones. In-window
  (0x4c9cf0) is `A + offset <= t <= D + offset`, always true with no times.

**Ducking (0x4ca1e0 make, 0x4ca4f0 move, 0x4c9fe0 read, 0x4ca080 release)**.
A ducker is made only when some DuckTo* is strictly inside (0, 1), and only
one per instance. It holds a level per group (SFX, music, ambience), starting
at 1, and TimeToUnduck. Each frame, per group:
- instance gone (released): level += dt / TimeToUnduck up to 1 (TimeToUnduck
  < 1: at once);
- instance at (0, 0, 0) - 2D - or closer than InnerRadiusOfInfluence, or
  closer than OuterRadiusOfInfluence while already ducking: level -= dt /
  TimeToDuck down to its DuckTo* (TimeToDuck < 1: at once); ducking flag set;
- else: ducking cleared and the instance lets go of its ducker (the groups
  after this one start back up in the same pass).
A ducker back at 1 on all three with no instance is deleted. The level a
sound is mixed at (volume callback 0x4cb220, every channel) is the instance's
time volume times the **lowest** level any ducker holds for its group,
skipping the ducker of its own instance. Group by category: 0 and 7..17 SFX,
1 music, 2 ambience, 3..6 none.

So a 2D boss line (SMSG_PLAY_SOUND, usage 2, SFX 0.3 / music 0.75) ducks the
rest of the mix over 1 s while it plays and lets it back over 3 s after; the
same line played at a unit with no radius of influence (most rows: 0, 0)
ducks nothing.

**SoundEmitters.** The emitter rows' kits are 426 usage 0 (loop, new file
every 20-60 s) and 68 usage 1 ("Emitter_IceCracking_Oneshots", the Dalaran
pet shop's frogs: one-shots every few seconds); 138 have no row. Where the
SoundEmitters table is played from was not found (the loader at 0x8b6820 is
reached only through the DB registration); the usage is taken from the row,
which those names bear out.

**Cone.** The angles are FMOD cone settings with the instance's direction;
1628 rows carry the defaults 180 / 270 / 0. Not applied in Wowee: nothing
that plays these kits has an orientation to give.

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
Every caller that plays a sound for a unit asks whether the unit is the
active player (its GUID against 0xca1238/0xca123c, or 0x4cee50 / 0x4d43c0)
and, if so: priority **110** (0x6e) always, and with Sound_ListenerAtCharacter
on, position nulled (**2D**) and the volume multiplier props[2] = **0.65**
(0x9f2818) - so `row.VolumeFloat x 0.65`. With the CVar off the sound is
placed at the unit at full volume, still at 110. The callers (every xref of
0x9f2818):

| Address | What |
|---|---|
| 0x4cf170 | footsteps: own steps 2D x0.65, priority **115**, category 17; others' category 13 |
| 0x4cfc10 | a table-driven sound at a point, z-offset (0x00a4040c) |
| 0x6da130 | kit 10063 (0x274f) on some UI events for the active player |
| 0x6f7b00 | an effect model's `$SND` event (Effect_C.cpp) on the player |
| 0x6f9840 | another model-event path (not traced further) |
| 0x71cba0 | scales a volume by 0.65 directly |
| 0x73a890 | a sound the unit repeats every 2000 ms in some movement state |
| 0x746720 | the unit's own kit at unit +0x8f0 (category 8) |
| 0x7469c0 | spell kit sounds on the unit |
| 0x746d60 | emote voice (EmotesTextSound / `$CSD`) |
| 0x7470d0 | kit 0xca12cc on the unit |
| 0x747310 | creature sounds (CreatureSoundData kinds) of a unit |
| 0x7474b0 | pet sounds (Sound_EnablePetSounds) |
| 0x7631a0 | the player unit's CreatureSoundData kinds (own injury: category 12) |

Weapon swings (0x4d0720) and impacts (0x4d0540, 0x4d0660) are **not** in
the list: the character's own swings are placed at it like anyone's.

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
| Panning | FMOD 3D pan (level 1) for every sound given a position | miniaudio's | kept: inverse model at rolloff 0 (`placeForPanning`). Its `attenuation_model_none` turns the spatializer off, panning and all, which put every unit's sounds in the listener's head |
| Unit sounds | at the unit (vtable +0x2c position): swing 0x4d0720, impact 0x4d0540/0x4d0660, injury and exertion 0x747310; only the active player's own are 2D | some 2D school-bank fallbacks for other units | other casters' casts and the player's hits on others play only their kits' sounds, at the unit; rows' MinDistance and cutoff for emote, $CSD, sheath and spell kit sounds |
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
| Volume defaults | music 0.4, ambience 0.6 | music 30%, ambient 100% (settings_panel.hpp) | 40 / 60 in the settings; GetCVarDefault answers 0.4 / 0.6 |
| Reverb | Sound_EnableReverb 0: off by default | always on (WorldFx) | gated on the CVar, default 0 (`AudioCoordinator::updateReverb`); a "Reverb" row on the Sound page; GetCVarDefault no longer answers it "1" |
| SoundEntriesAdvanced | 2D radius, time windows, ducking, usage | none | `stock_sound_advanced.hpp`: every kit the engine starts with a row gets the pan blend, time volume, category, duplicate radius and ducking (`AudioEngine::attachAdvanced`, `update`); music is ducked too. Emitters run usage 0/1 schedules (`WorldSoundEmitters::stepAdvanced`). Cone not applied |
| Own-character 2D x0.65 | own unit's voice, steps, emotes, model/spell/mount sounds 2D at 0.65, priority 110 (steps 115); swings placed | voices and steps 2D at their own levels | `SoundOptions::ownCharacter`, applied by the engine: activity, mount, movement, spell-fallback banks, emotes, `$CSD`, body sounds, spell kit and effect `$SND` sounds. Own steps now come from `$FSD` + FootstepTerrainLookup (2D x0.65, 115, category 17) |
| Stand sound | 40%, 10 s client-wide spacing | (not played) | not done |
| Category volumes | slider x kit, groups SFX/MUSIC/AMBIENCE | per-manager scales | unchanged |
| Sound_NumChannels | Sound panel slider, 32 or 64 (SoundPanelOptions), restarts sound | CVar applied, control hidden | "Sound channels" row on the Sound page (32..64 step 32), live |
| Sound_ListenerAtCharacter | on: listener at the character, own sounds 2D x0.65; off: camera | always at the character | the CVar, a "Sound at character" row; off puts the listener at the camera and own sounds back in 3D at full |
| Own exertion / injury | 35% / 30% rolls (100 on crits), categories 9 and 12, no timers | 30% grunt with 1.5 s timer, wounds 0.8 s timer | stock rolls and caps (`stock::unitSoundChance`, `unitSoundCategory`) |
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

- Boss and narrator lines sent as SMSG_PLAY_SOUND pull the rest of the
  sound effects down to 0.3 and the music to 0.75 over a second, and let
  them back over three after the line ends.
- The Dalaran pet shop's frogs and pigs, Icecrown's ice cracking: one-shots
  with seconds of silence between, not a loop of one clip. A waterfall
  emitter changes its file every 20-60 s with a 3 s crossfade.
- Your own footsteps, jump and land grunts, emotes and spell sounds sit in
  the middle of the head, a little quieter than before (0.65); other
  players' are placed where they stand. Turning "Sound at character" off
  moves the listener to the camera and places your own sounds too.
- Sound channels on the Sound page: 32 or 64.

## 10. Unresolved

- SoundEntriesAdvanced cone (Inside/OutsideAngle, OutsideVolume): not
  applied; the callers have no orientation to give, and FMOD's cone with a
  null direction was not traced.
- The pan level is approximated: miniaudio has no FMOD-style 3D pan level,
  so a sound below level 1 is moved towards straight ahead of the listener
  at the same distance (`AudioEngine::pannedPosition`), which centres it the
  same way.
- Where SoundEmitters.dbc rows are played from in Wow.exe was not found;
  emitters take the usage from their advanced row (the row names - "...
  Oneshots" with usage 1 - fit that).
- Usage 0 and 1 schedules run for the emitters only. Any other caller that
  plays such a kit (rare) gets the pan, time volume and ducking but plays
  the file once, as before.
- The own-character hand-made banks (landing, splash, swim, mount, school
  spell fallbacks) now take 0.65 on their own invented base levels; the
  stock sounds they stand in for are kits whose VolumeFloat was not matched.
- M2 `$SND` events played through EntitySpawner (entity_spawner.cpp, owned
  by other work at the time) were not given the own-character flag.
- The FMOD virtual-voice behaviour is approximated: a sound that would be
  virtual is dropped at once instead of waiting 400 ms for a voice, and a
  looping sound that loses its voice is stopped rather than resumed.
- Kinds 4 (stun), 5 (stand, 10 s spacing) and 8 (alert) have no Wowee
  caller: 0x747310 and 0x7631a0 are virtual (CGUnit vtable entry 0xa34ea8,
  CGPlayer 0xa327e0), so the call sites passing those kinds were not found.
- The meaning of category 8 (unit+0x8f0 kit) and of model flag 0x20
  (categories 15/16) was not traced; nor 0x4cfc10, 0x6f9840, 0x71cba0,
  0x73a890 beyond their own-unit handling.

## 11. Event audit, 2026-11-04

Sections 1-10 are the engine; this is the events that drive it, checked
end to end with a trace of every sound the client starts, after a week of
new systems (missiles, mounts, death, water, professions, UI flows).

### The trace

`WOWEE_SOUND_TRACE=1` writes one `[sndtrace]` line per start request to the
log; any other value is a file to write them to. `WOWEE_AUDIO_NULL=1` opens
miniaudio's null device, so a scripted run mixes (voice limits, caps and
ducking all apply) without sounding on the desk. A line holds the time,
the kind (2d, 3d, -loop, -handle), what became of it (`ok`,
`refused-voice` for the stock voice/category/kit limits, `out-of-range`
past the row's cutoff, `refused-advdup`, `load-fail`, `decode-fail`,
`muted`), the SoundEntries id and name, the file, the category, the FMOD
priority, the own-character flag, the volume and pitch, 2D or 3D with the
position, distance and min/max, the gain it was started at, and the call
stack as offsets into the binary (`addr2line -f -C -e wowee <offsets>`).
Stops and fade-outs are lines of their own (`kind=stop id=`), and every
`WoweeDebug_Log` line of a scripted run is copied in (`lua SCN ...`) so the
trace reads scenario by scenario. A buffer handed over as bytes is named by
`SoundTraceName` or by the file it was read from (the sample loaders note
it, `sound_trace_hook.hpp`). `AudioEngine::playMusic` lines name the track.

The scenarios were five scripted runs on Wptestb (Goldshire, Elwynn,
Crystal Lake, Stormwind, Hellfire), each a WOWEE_DEBUG_LUA list of GM
commands and API calls with a marker per step.

### Stock rules found for this audit

| Event | Wow.exe | Rule |
|---|---|---|
| Weather | SMSG_WEATHER 0x526530 -> Weather.dbc (0xad4eb0) -> 0x4c8610 | the row's AmbienceID ("Weather - RainLight" 8533 .. "SandstormHeavy" 8558) is ambience **slot 8**, day and night: it replaces the area's loop with the usual 5 s crossfade; 0x4c86f0 skips slot 8 while 0x4c8600's flag is up, which 0x78e9a0 sets from 0x7a1480 - the WMO group the camera is in **lacks the exterior flag (MOGP 0x8)** |
| Under water | 0x5fe7b0 (camera liquid) -> 0x4c8630 | slot 9 ambience = SoundEntries 4209 "Underwater (DONOTRENAME)" while the camera is in liquid; 0xb4aa9c ("changed") makes 0x4c9910 switch with **no fade**. No ducking row, no music change |
| Indoors, for sound | 0x7a1480 | the group lacks MOGP 0x8 (exterior). Not the same as having 0x2000 (indoor) |
| Close events | CloseMail 0x571a40, CloseGossip 0x58a550, CloseTrainer 0x5940e0, CloseAuctionHouse 0x59f150 | act only when that window is open (its guid set); CloseBankFrame 0x57b8d0 and CloseGuildBankFrame 0x5a4270 fire their event every call |
| Gossip quest pick | SelectGossipAvailableQuest 0x58b600 -> 0x58b070 | sends the query; the gossip stays until the answer replaces it |
| Accept | AcceptQuest 0x58d5b0 -> 0x58cd20 | sends, clears the quest guid, QUEST_FINISHED, and lets the NPC go (farewell) only when no gossip is open |
| Reward | GetQuestReward 0x58d620 -> 0x58cfa0 | sends the choice and sets a waiting flag (0xc0d6ac); nothing closes |
| Turn-in answer | SMSG_QUESTGIVER_QUEST_COMPLETE 0x6d0e10 -> 0x58ca70 | igQuestListComplete, the reward lines, then the frame closes (QUEST_FINISHED, NPC let go) unless the quest's NextQuestInChain (cache +0x2c) is set - then it waits for the next quest's details |
| Aggro | SMSG_AI_REACTION 0x716b10 | reaction 2 -> 0x7474b0 kind 0: CreatureSoundData SoundAggroID (+0x28) at the head attachment; a row with none is silent. Reaction 0 -> kind 8 (Alert, +0x34) |
| Spell failure | SMSG_SPELL_FAILURE 0x809c70 -> 0x4cff30 | the school's fizzle at the caster's position, default properties (placed, full level, the player's own too), for **every** caster. SMSG_CAST_FAILED's fizzle (0x808200) is the caster's second |
| Text emote voice | send 0x6dd9e0 -> 0x4ceb70; play 0x747000 -> 0x746d60 | the sender draws the file from the EmotesTextSound row's bag and sends its index in CMSG_TEXT_EMOTE; every receiver plays that file (props[3]) two yards above the unit (0xadb674) or at its head attachment, no-duplicate forced (props[9] = 2), on the unit's emote handle |
| Jump, land | 0x73ed10, event 0x346 -> 0x73d3d0, 0x7461e0 | kinds 11/12 (CreatureSoundData JumpStart/JumpEnd): no player model's row names one, so silent; the thud is the JumpLandRun animation's $FSD. A drop of more than 13 yd (0xa349f4; 70, 0xa349f8, in the other branch) forces kind 13 = the Injury column |
| Equip by right-click | UseContainerItem 0x5d8650 -> 0x6dfee0 -> 0x520770 / 0x6dfc40 -> ClearCursor 0x519280 | no pickup sound; the put-down sound (0x4d0020, ItemGroupSounds column 1; wrapped items "(DONOTRENAME)WrappedItemDrop") |
| Cursor put down | ClearCursor 0x519280 | item: its put-down; money: LOOTWINDOWCOINSOUND; spell, macro, pet action, stable pet, merchant: INTERFACESOUND_CURSORDROPOBJECT; ammo and the item kinds 5-7/9: put-down then the drop sound |
| Repair cursor | UseContainerItem 0x5d8650 (and the pickups) | cost check, ITEM_REPAIR, CMSG_REPAIR_ITEM. Repair All's sound is merchantframe.xml's |
| Object sounds | GO model events 0x70c050 -> 0x70bf70 | a game object's model events $GO0-$GO5 and $GC0-$GC3 play GameObjectDisplayInfo Sound[0-5] / Sound[6-9] at the event; $DSL loops, $SHK shakes. Doors, chests, the bobber's bite (custom 0 -> $GC0 -> Sound[6]) |
| Kit file pick | 0x4cbac0 | every caller of a SoundEntries row draws from the row's bag (section 2) |
| Hourly bells | (server) | AzerothCore's "Hourly Bells" game event plays them through the bell objects; the client has none |

### Scenario table

Expected is the stock rule (section 1-7 and the table above, or the
FrameXML the stock client runs, which Wowee runs too). Before is the trace
of the build this audit started from, after is the trace with the fixes.

| Scenario | Expected (stock) | Wowee before | Wowee after |
|---|---|---|---|
| Login, char select, enter world | gsLogin, GS_LichKing, GlueScreenIntro, char select race ambience, gsCharacterSelectionEnterWorld; zone music, area loop, $DSL loops | as expected | unchanged |
| Bags, spellbook, character, talents, map, quest log, game menu, friends | FrameXML PlaySound names (igBackPackOpen, igSpellBookOpen, igCharacterInfoOpen, TalentScreenOpen, igQuestLogOpen, igMainMenuOpen ...) | as expected, 2D at the rows' levels | unchanged |
| Money change | LOOTWINDOWCOINSOUND on every PLAYER_FIELD_COINAGE change (0x6da070) | as expected | unchanged |
| Item into bags (create, loot, buy, reward) | put-down of the item's ItemGroupSounds (SMSG_ITEM_PUSH_RESULT 0x523900) | as expected (cloth, small metal, chain, food, herb, wood) | unchanged |
| Pick up / drop an item | pickup / put-down by material | as expected | unchanged |
| Equip by right-click | put-down of the item (cursor path) | **silent** | PutDownSmallMetal for a sword, PutDownWoodSmall for a pole |
| Spell or macro dragged and dropped | INTERFACESOUND_CURSORDROPOBJECT | **silent** | uSpellIconDrop.wav |
| Destroy item | put-down as the cursor clears | as expected | unchanged |
| Eat | the Food aura's state kit "Food", the server's 5 s SMSG_PLAY_SPELL_VISUAL "Item - Eat" | as expected, but the state kit's one-shot **replayed on every teleport and model rebuild** | heard once; a looping state sound still restarts on the new model |
| Emotes /cheer /laugh | the sender's file index from CMSG_TEXT_EMOTE, 2 yd above the unit, refused while the same line plays; /laugh is the model's $CSD | **uniform random file per receiver**, index 0 sent, no duplicate check, at the feet; $CSD uniform pick | the sender's bag draw sent and played by all; second /cheer inside the line refused; $CSD through the row's bag |
| Level up | LEVELUP via LevelUp.m2 $SND, own 2D x0.65 | as expected | unchanged |
| Mail open/close | mailframe.lua: tab click, OpenBackpack, igSpellBookOpen; close sounds once | close sounds **twice** (MAIL_CLOSED fired by CloseMail and again by the frame's OnHide) | once |
| Gossip -> quest -> accept | greeting at open, igQuestListOpen, WriteQuest, farewell after accept, QUESTADDED | **farewell in the middle** (gossip closed on the pick, NPC let go for the round trip) | greeting, no mid-conversation farewell, farewell after accept |
| Quest turn-in | igQuestListComplete, the frame closes on the server's answer, one farewell | frame closed on the click: **farewell on the hand-in and again on the close** of the chained quest | one farewell, when the frame closes |
| Vendor, repair | igCharacterInfoOpen/Close (FrameXML), greeting/farewell, coin on money change; repair cursor: ITEM_REPAIR | repair cursor **silent** | ITEM_REPAIR (MiningHitA) |
| NPC clicks | NPCSounds hello x5 then the annoyed lines; never over itself | as expected | unchanged |
| NPC aggro | CreatureSoundData SoundAggroID or nothing | voice-type barks ("Attack my target!") **for rows with none** | the row's sound or silence |
| Melee | swing $CSS / blow $CAH: WeaponSwingSounds2, WeaponImpactSounds by armour, "(DONOTRENAME)Combat Miss 1H", exertion 70%/35%, injury 60%/30% | as expected (category caps refusing extras) | unchanged |
| Spell precast/cast/impact | the kits' SoundIDs (Precast Holy Low loop, Holy Cast, HolyImpactDDHigh, FistOfJustice, Devotion Aura, SealofMight, InnerFire) | as expected, files **picked uniformly** (repeats, no 0x400/0x800 spread) | bag draw and spreads through `AudioEngine::playKit` |
| Interrupted cast | SMSG_CAST_FAILED fizzle (2D) + SMSG_SPELL_FAILURE fizzle at the caster | second fizzle **2D**, none for other casters | second fizzle at the caster in 3D, for every caster |
| Missiles | MissileSound loop on the missile, stopped at impact | as expected (MissileHolyLoop, MissileIceLoop seen on others' casts; every loop stopped) | unchanged |
| Profession casts (fishing, crafting, gathering) | the kits' sounds | **silent**: profession spells' kits were skipped | "Fishing Cast", "Item - Crinkling Paper" (Linen Bandage), "TradeSkill - Smelt", "Gather_Herb" loop |
| Fishing bite | "Fishing Hooked" at the bobber ($GC0 -> GameObjectDisplayInfo Sound[6]) | 2D | at the bobber (no bite came in the after run's window; the path is the same playEntryAt the server's object sounds use) |
| Mount | precast kit (Tailoring loop for Brown Horse), HorseSummon on success; footsteps from the mount's $FSD | as expected | unchanged |
| Jump and land on foot | no voice; JumpLandRun $FSD; >13 yd fall: injury cry | **a hand-made footstep thud on every landing**, teleports included; fall voice from guessed LandHard files after a 1 s fall | no thud of its own; HumanMaleMainWound after a 28 yd drop, nothing after 6 yd, nothing for a teleport (the scripted jump did not leave the ground, so the JumpLandRun footfall was not seen) |
| Death, ghost, revive | death cry (own 2D), GhostMusic + GhostState in slot 10, back to the area on revive | as expected | unchanged |
| Rain, snow | Weather.dbc loop in slot 8, crossfading with the area's, not indoors | **no rain at all** (RainLight.wav does not exist; the loops are RainLightLoop.wav ...), snow as a one-shot every 20 s over the area's loop | RainMediumLoop (grade 0.4) and RainHeavyLoop (0.7) replace the forest loop; inside the Goldshire inn the room's Tavern loop, the rain again at the door |
| Under water | 4209 replaces the ambience at once; music untouched | separate loop on the interface bus over the area's, 0.4/0.6 s fades; music turned down to 30% while swimming | slot 9 swap, no fade, no music duck |
| Swimming | (not traced: see 11.4) | splash on every surface bob (enter/exit flapping each second), hand-made stroke one-shot every 0.8 s | one splash on entering; a bob that flips the state within 1.5 s is silent; strokes unchanged |
| Fountains, torches, braziers, lakes | the doodads' $DSL loops and SoundEmitters.dbc | $DSL loops **plus** guessed one-shot fire/fountain/water emitters (a Goldshire fountain replayed every 6 s over its own loop) | $DSL and SoundEmitters only |
| Smithy | the room's WMOAreaTable ambience and the forge doodads | hand-made forge loop in WMO 96048 replacing the area loop | the tables' (Goldshire's smithy room names BlackSmith.wav itself) |
| City bells | the server's hourly bells | **random bells every 2-3 minutes** on top | the server's only |
| Zone and WMO music/ambience | sections 6 | as expected; the room test used the indoor flag | the room test is the exterior flag, as 0x7a1480 |

### What changed

- `AudioEngine` trace (`WOWEE_SOUND_TRACE`, `WOWEE_AUDIO_NULL`,
  `SoundTraceName`, `soundTraceNote`, `sound_trace_hook.hpp`); the old
  `sfx:`/`sfx2d:` info lines it replaces are gone.
- `AudioEngine::playKit` / `KitPlay`: one way to play a SoundEntries row
  (bag, level spread, pitch, MinDistance, cutoff, no-duplicate, loops,
  handles, file index). Spell kits, emote voices, `$CSD`, sheath sounds and
  the placed fizzle use it.
- Ambience slots in `AudioCoordinator::updateTableAmbience`: 10 screen
  effect, 9 under water (instant), 8 weather (not indoors), then the area;
  `SoundData::weatherSound` reads Weather.dbc; `ZoneAudioContext` carries
  `weatherState` and `wmoSoundIndoors` (`WMORenderer::AreaHit::soundIndoors`).
- `AmbientSoundManager`: with the tables, no guessed positional emitters,
  bells, weather one-shots, underwater one-shots or forge loop.
- Quest and window flow: gossip kept across the quest pick, reward frame
  kept until SMSG_QUESTGIVER_QUEST_COMPLETE (closed unless the quest chains;
  `nextQuestInChain` from the query), and the guarded close events.
- `TextEmotePacket::build` sends the sound index; the receiver plays it.
- `NpcVoiceManager::playAggro`: no voice-type fallback with the tables.
- `SpellHandler::playFizzle(spell, unit)`; profession spells' kits play.
- Cursor: drop sound for non-item kinds, put-down on a right-click equip,
  ITEM_REPAIR on the repair cursor.
- `ActivitySoundManager::playLanding`: the stock landing with the tables;
  a splash only when the swim state it ends had lasted 1.5 s.
  `SfxStateDriver`: fall height from the highest point of the fall, reset by
  a teleport; no music swim duck with the tables.
- State kits re-attached to a new model do not replay a one-shot sound.

### 11.4 Not done

- ~~**Object sounds**~~: done in section 12.
- **Swimming strokes and the water-entry splash**: the stock trigger was not
  found (SoundEntries 1090/1096/1097 "CharacterSplashSound*" have no
  reference in the code; LiquidType.SoundBank names the river/ocean loops
  1111-1114). The hand-made strokes and splash remain.
- **Duels, battlegrounds**: need a second player / a running battleground;
  the countdown and BG sounds are FrameXML's and the server's and were not
  traced in game.
- AI reaction 0 (Alert, kind 8) is not played.
- The text emote's head attachment (0x11) is approximated by 2 yd above the
  feet, which is the stock fallback.
- A chained quest whose next quest the server does not offer (AzerothCore
  checks CanTakeQuest) leaves the reward frame up until it is closed, as the
  stock client's wait would.
- The UChatScrollButton click as Blizzard_CombatLog loads (its ADDON_LOADED
  handler presses a quick button) is heard at world entry; whether the stock
  loading screen swallows it was not checked.

## 12. Game object sounds, 2026-11-06

The rules are in interactions.md section 12 (the handler, its kinds and its
events); the sound half:

- **Where they come from.** A game object's model events, through the type
  handler's +0x40 (0x70d7b0 -> 0x70c050): `$GO0`-`$GO5` and `$GC0`-`$GC3` play
  GameObjectDisplayInfo Sound[0-5] and Sound[6-9] (0x70bf70) at the event's
  position on the model; `$DSL` loops its data's row, `$DSO` and `$SND` play
  it once. A map doodad's (0x7bd5a0): `$DSL`, `$DSE` (stop), `$DSO`.
  Transports' own models fire nothing.
- **One-shots** (a row without 0x200): default properties (0x4c5990), placed,
  category 0, the row's MinDistance and DistanceCutoff. The door's swing,
  the chest's lid, the bobber's bite.
- **Loops**: one handle per object (handler +0x1c; a doodad's +0x158). Not
  started while that handle sounds (0x4c5be0) or while the same row plays
  within 6 yards of the event (0x4cfe00: a scan of the playing sounds, 36 at
  0x9e8cf8 squared). Props: loop (props[7] = 1), keep-alive (props[0xe] = 1),
  props[0xc] = 0 and [0x12] = 1; a GO's `$DSL` fades in and out over 1 s
  (props[4], [5]), a doodad's in over 3 s (0x9ebbc4), a `$GOn` loop not at
  all. A looping sequence fires its event every lap, so a loop that was
  refused (out of range, no voice) is tried again on the next lap.
- **Stops**: the handle is stopped when the object changes state (0x70d690 ->
  0x4c6390) and when it goes; `$DSE` stops a doodad's.

In Wowee: `audio::GameObjectSounds` (src/audio/game_object_sounds.cpp), fed
by `M2Renderer::takeObjectEvents()` in `EntitySpawner::updateGameObjectPresentation`.
A transport doodad's loop is moved with its ship each frame
(`AudioEngine::setSoundPosition`); whether stock moves it was not traced.
The trace names them like any other kit (`3d` / `3d-loop`, the row's id and
name, `GameObjectSounds::playLoop`/`playOnce` on the stack).

| Scenario | Expected (stock) | Wowee before | Wowee after |
|---|---|---|---|
| Stormwind Officer's Door (`.gobject set state 26262 0 n`) | 8015 StormwindDoorOpen as Open starts, 8016 StormwindDoorClose as Close starts, at the door, its row's 10/30 yd | silent | as expected (3D at the door, 13 yd off: gain 0.29) |
| Door told to shut half way through opening | Close starts at the mirrored point, past its stamp at 0: no slam | - | no slam |
| Door ACTIVE_ALTERNATIVE -> READY (Rebuild) | no Rebuild/Destroy: Open run backwards; its `$GO1` stamp at 0 is reached last | - | 8015 three seconds after the change, as Open ends backwards |
| Door Custom0 (`set state 26262 4 0`) | the model has no 153: nothing | - | nothing |
| Stolen Chest (Chest01) opened / shut | 1277 ChestOpen, 1278 ChestClose, once each | silent; the lid never moved (chest models were kept from animating by name) | once each, at the chest; the lid animates |
| Temp chest (`.gobject add temp 32`), looted | AzerothCore does not change a chest's GO state on loot, so no lid and no sound (the same for the stock client on this server) | - | the same |
| Fishing bite | 3355 Fishing Hooked from the bobber's `$GC0` at 200 ms | 3355 played by hand | the event (not traced: no bite in these runs) |
| Zeppelins (Tirisfal tower) | ZepAnimation's 5457 ZeppelinEngineLoop and 5458 ZeppelinHeliumOneShots on the ship; the server's ZeppelinDocked | only the server's ZeppelinDocked (2D) | 5457 loops on each zeppelin, 5458 every 4 s at the moving doodad (out of range from where the test stood, 115 yd); ZeppelinDocked unchanged |
| Icebreaker | 11840 Icebreaker_paddlewheel loop | silent | loops on the ship |
| Undercity lift | no sound source: the lift's display has no sounds, its model no events | silent | silent |
