# Glue screens (login, character select and create, loading screen)

Stock 3.3.5a build 12340: GlueXML from the client's own archives
(`Interface\GlueXML\*.lua|xml`, extracted under
`wowee-dist/Data/expansions/wotlk/interface/gluexml`) and the C side in
`Wow.exe`, read with Ghidra. Wowee draws its own glue screens
(`src/ui/auth_screen.cpp`, `realm_screen.cpp`, `character_screen.cpp`,
`character_create_screen.cpp`, `glue_options.cpp`) over a
`rendering::CharacterPreview`; this page is what the stock client does and
where Wowee follows it. The rules that are plain numbers live in
`game/glue_rules.hpp` (tested by `test_glue_rules`).

## 1. How the stock glue screens are put together

`GlueParent` (GlueParent.xml) holds every screen. `GlueScreenInfo` maps the
C side's screen names to frames: `login` AccountLogin, `charselect`
CharacterSelect, `charcreate` CharacterCreate, `realmwizard`, `patchdownload`,
`trialconvert`, `movie`, `credits`, `options`. The C side raises
`SET_GLUE_SCREEN`; `SetGlueScreen(name)` hides the rest, shows the one, plays
the glue music (and, for `login`, the intro ambience). Going from login to
character select fades the login UI out over `LOGIN_FADE_OUT` = 0.5 s first.

**GlueParent_OnLoad**: a window wider than 16:9 is pillarboxed - GlueParent
is anchored `barWidth` in from each side, so every glue frame, the 3D scenes
included, is at most 16:9.

The three 3D screens are `ModelFFX` frames the size of GlueParent:

| Frame | Model | XML attributes |
|---|---|---|
| AccountLogin | `UI_MainMenu_Northrend.m2` (`UI_MainMenu.m2` when `IsStreamingTrial()`) | fogNear 0, fogFar 1200, glow 0.08 |
| CharacterSelect | `SetCharSelectBackground(path)` | - |
| CharacterCreate | `SetCharCustomizeBackground(path)` | - |
| RealmWizard | (realm wizard) | fogFar 1200, glow 0.08 |
| PatchDownload | `UI_NightElf.mdx` | fogFar 1200 |

Each one's `OnLoad` calls `SetCamera(0)` and `SetSequence(0)`: the M2's first
camera and its first animation.

### Lua API behind them (Wow.exe)

| Function | Address | What it does |
|---|---|---|
| PlayGlueMusic(name) | 0x4dc070 -> 0x9861c0 | Returns if that SoundEntries row is already playing; else stops the old one with a **3 s** fade (0x9ebbc4), plays the new as MUSIC (props type 1, no fade-in) and registers 0x986080, which restarts it when it ends. |
| PlayGlueAmbience(name, fade) | 0x4dc0c0 -> 0x985fb0 | Same early return; old one out over 3 s; new one as AMBIENCE (type 2), looping, fade-in = `fade` (GlueXML always passes 4.0). |
| StopGlueAmbience() | 0x4dc130 -> 0x985f70(1.0) | Fades the ambience out over 1 s. |
| SetCharSelectBackground(path) | 0x4e44e0 -> 0x4e3c10 | ModelFFX `SetModel`, then hooks the model's light callback 0x4e3a20 to the frame's light sets. |
| SetCharCustomizeBackground(path) | 0x4e0620 -> 0x4e0040 | Same for the create frame; re-attaches the character to attachment 0 (0x831630). |
| GetSelectBackgroundModel(i) | 0x4e3620 | `IsStreamingTrial()` -> "CharacterSelect". Class 6 (death knight) -> ChrClasses(6).Filename ("DEATHKNIGHT"); else ChrRaces.ClientFileString of the race, with gnome (7) -> race 3 (Dwarf) and troll (8) -> race 2 (Orc). |
| SetCharacterSelectFacing(deg) | 0x4e3030 -> 0x4e2e70 | Stores deg x pi/180 (0xb6b204); rotates the character model about Z on its attachment. |
| SetCharacterCreateFacing(deg) | 0x4e0c10 -> 0x4e0160 | Same for the create model (0xb6b188). |
| ModelFFX:ResetLights / AddLight / AddCharacterLight / AddPetLight | 0x4e6be0, 0x4e6c60, 0x4e6d60 | Up to 4 lights per set, 27 dwords each; sets are background/character/pet x live/ghost. |
| Model:SetCamera / SetSequence / SetFogFar / SetGlow | 0x960970, 0x9607e0, 0x9613a0, 0x9614b0 | |

`IsStreamingTrial()` is 0x422140 -> 0x428010 (two flags at 0xb38180).

## 2. Login screen

- **Scene**: `UI_MainMenu_Northrend.m2`, camera 0, sequence 0. It has one
  light: ambient (0.72, 0.83, 0.93) x 1.3 and no directional light; nothing
  calls SetLighting, so that is all of it.
- **Camera**: the ModelFFX projection (0x95fc30 -> 0x4bf0c0 -> 0x4becf0 ->
  0x6bfe00) takes the M2 camera's field of view as **diagonal** and converts
  it with the frame's own aspect: `vertical = fov / sqrt(1 + (w/h)^2)`, near and
  far from the camera (0.222 and 2777.8 for this one). The frame is at most
  16:9 (above), so 16:9 is the widest aspect it ever sees.
- **Music**: `CurrentGlueMusic = "GS_LichKing"` -> SoundEntries 12765,
  `Sound\Music\GlueScreenMusic\WotLK_main_title.mp3`, volume 1.0. It plays
  across every glue screen (each `SetGlueScreen` calls PlayGlueMusic, which
  returns early while it is playing). There is no separate character-create
  music in 3.3.5.
- **Ambience**: `PlayGlueAmbience(GlueAmbienceTracks["DARKPORTAL"], 4.0)` =
  GlueScreenIntro (9902, `Sound\Ambience\GlueScreen\GlueScreenLogin.wav`,
  0.45) on AccountLogin_OnShow and START_GLUE_MUSIC.
- **Logo**: `Glues-WoW-WotLKLogo`, 256x128 at TOPLEFT(3,-7) of GlueParent.
- **Sounds**: Login `gsLogin`; Options `gsTitleOptions`; Quit none (its
  PlaySound is commented out); any GlueDialog button `gsTitleOptionOK`.

## 3. Realm list and queue

RealmList.xml: OK plays `gsLoginChangeRealmOK`, Cancel
`gsLoginChangeRealmCancel`. The queue position and disconnection are
GlueDialogs; `DISCONNECTED_FROM_SERVER` sets the login screen and shows
`DISCONNECTED` ("You have been disconnected from the server.", Okay) - or
`PARENTAL_CONTROL` for reason 4.

## 4. Character select

### The scene (0x4e3cd0, run on each selection)

- `CharacterSelect_SelectCharacter` asks `GetSelectBackgroundModel(id)` and
  calls `SetBackgroundModel(CharacterSelect, name)`:
  `Interface\Glues\Models\UI_<name>\UI_<name>.m2`, then
  `PlayGlueAmbience(GlueAmbienceTracks[NAME], 4.0)` and `SetLighting`.
- **Facing**: 0x4e3cd0 sets the facing (0xb6b204) to **0** each time, and
  attaches the character model to the scene's **attachment 0** (0x831630).
  Facing is measured from that attachment's own orientation: most marks face
  +X, the human one -16.5 degrees, the blood elf and death knight ones 143 -
  each near the camera's direction but not on it.
- **Pet**: if the character has a pet (SMSG_CHAR_ENUM pet display, level,
  family) its CreatureDisplayInfo -> CreatureModelData model is attached to
  **attachment 1**. Size: ModelScale x CreatureDisplayInfo scale, replaced,
  when the family has a CreatureFamily row, by
  `MinScale + (MaxScale - MinScale) x clamp(level - MinScaleLevel) / (MaxScaleLevel - MinScaleLevel)`.
- **Gear**: slots 0..22 from the enum; a hunter shows the ranged slot and no
  hands, anyone else the hands and no ranged; flag 0x400 hides the helm, 0x800
  the cloak.
- **Ghost** (character flag 0x2000): SpellVisualKit **989** applied by
  0x4e2ce0 - effect 717 `Spells\Ghost_state.mdx` on the Base attachment,
  colour 0x8CBAFD on the model (CM2Model +0x180) and alpha 0.5 (+0x178) - and
  the scene's light callback (0x4e3a20) switches to the ghost light set. The
  frame's full-screen effect switches from 0xb6a188 to the ghost one 0xb6a184,
  with the glow value 0.4 -> 0.15 (0xac441c / 0xac4420).
- **Rotation**: drag `CHARACTER_ROTATION_CONSTANT` = 0.6 degrees per pixel of
  cursor x; the rotate buttons `CHARACTER_FACING_INCREMENT` = 2 degrees a frame
  while pushed, and play `igInventoryRotateCharacter` on mouse down and up.
- **Sounds**: Enter World `gsCharacterSelectionEnterWorld` (and
  StopGlueAmbience), Create `gsCharacterSelectionCreateNew`, Back / Escape
  `gsCharacterSelectionExit`, Delete and Change Realm
  `gsCharacterSelectionDelCharacter`, AddOns via the options sound.

### SetLighting (GlueParent.lua)

```
model:SetSequence(0); model:SetCamera(0)
CharModelFogInfo[race] -> SetFogColor, SetFogNear(0), SetFogFar(far)  (else ClearFog)
CharModelGlowInfo[race] -> SetGlow (else 0.3)
model:ResetLights()
for each RaceLights[race] row: AddCharacterLight, AddLight, AddPetLight (LIGHT_LIVE, ...)
```

`ResetLights` puts all six light sets back to the model's. **Adding a light to
a set replaces all of that set's default lights** ("NONE of the default lights
are used for that set"); only directional lights are supported and the point
lights still come from the model. So the stock scene is lit by the RaceLights
table plus the model's point lights. The table repeats most models' own
lights - but the orc scene (shared by trolls) carries only an ambient 0.15 and
a point light, and RaceLights gives it a cool (0.447, 0.545, 0.737) and a warm
(0.55, 0.339, 0.149) x 2 directional light.

Fog (CharModelFogInfo): HUMAN 0.8/0.65/0.73 far 222, ORC grey 270, DWARF
0.85/0.88/1.0 500, NIGHTELF 0.25/0.22/0.55 611, TAUREN 1.0/0.61/0.42 153,
SCOURGE 0/0.22/0.22 26. Glow: HUMAN and DWARF 0.15, everything else 0.3.

Ambience (GlueAmbienceTracks, SoundEntries): HUMAN GlueScreenHuman
(ForestNormalDay, 0.3), ORC GlueScreenOrcTroll (CanyonDesertDay), DWARF
GlueScreenDwarfGnome (ForestSnowDay), TAUREN GlueScreenTauren
(GrasslandsDay), SCOURGE GlueScreenUndead (ForestScaryNight), NIGHTELF
GlueScreenNightElf (EnchantedForestNight), DRAENEI GlueScreenDraenei
(ExodarCity1, 0.4), BLOODELF GlueScreenBloodElf (SilverMoonGeneralDay1),
DEATHKNIGHT and CHARACTERSELECT GlueScreenIntro.

Scene cameras (camera 0, all type -1): fov (diagonal, rad) / near / far -
Human 1.396 / 0.222 / 611; Orc and CharacterSelect 1.134 / 0.222 / 27.8;
Dwarf 1.134 / 611; NightElf 1.047 / 617; Scourge 1.134 / 125; Tauren 1.134 /
87.9; BloodElf and DeathKnight 0.846 / 514; Draenei 0.950 / 2778;
MainMenu_Northrend 2.094 / 2778.

## 5. Character create

- **Entering** (`CharacterCreate_OnShow`): `ResetCharCustomize()` (0x4e1fd0)
  "randomly selects a combination": a random sex, a random race among those
  the account's expansion allows (0x4dff10), a random look (0x4e13a0 with its
  randomise flag) and a random class the race can be (0x4e0f50); then
  `SetCharacterCreateFacing(-15)`.
- **Race click**: `gsCharacterCreationClass`; 0x4e20b0 keeps the look the old
  race and sex had (0xb6b0d0[race*2+sex]) and restores the new one's if it was
  visited, otherwise rolls a random one; the class is kept if the new race can
  be it, else a random one; `SetCharacterCreateFacing(-15)` again.
- **Class / gender click**: `gsCharacterCreationClass` (on any click).
  Changing class keeps the look (0x4e1740, rechecked for the death knight
  options). A death knight stands in `UI_DeathKnight`.
- **Customisation arrows, Randomize, the random name**:
  `gsCharacterCreationLook`. Accept `gsCharacterCreationCreateChar`, Back
  `gsCharacterCreationCancel`. Rotate as on the select screen.
- **No class pose**: 3.3.5 plays the stand animation only; the starter outfit
  is put on from CharStartOutfit (0x4e0fd0).
- **Random name** (`GetRandomName`, 0x4dfca0): not a NameGen row. 0x4e6790
  builds a chain from the race and sex's NameGen.dbc names: each name,
  lower-cased with "_" in front ("_%s"), is cut into overlapping 3-letter
  pieces (the last one 2 letters) counted by occurrence and sorted (0x4e6560).
  0x4e63a0 starts on a "_" piece drawn by count, then repeatedly draws a piece
  whose first two letters are the name's last two, appending its third letter,
  until it draws a 2-letter (ending) piece or reaches 13 letters; drops the "_"
  and capitalises. GetRandomName repeats until the name is 4..10 letters and
  passes the client's name check (CHAR_NAME_SUCCESS).

## 6. Loading screen (LoadingScreen.cpp, 0x409000-0x40b200)

- **Art** (0x409ed0): Map.LoadingScreenID -> LoadingScreens.FileName. If
  `HasWideScreen` and the window's aspect > 4/3 + 0.001, "Wide" is inserted
  before the extension. No row or no file: `Interface\Glues\loading`.
- **Fit** (0x40a270): the art keeps its aspect - 4:3, or 1.6 for wide art -
  and the viewport is narrowed (pillarbox) or shortened (letterbox) to it,
  centred, black around. Everything after is drawn in that viewport.
- **Bar** (layout table 0x9e2dfc, normalised to the art, y up): only
  `Loading-BarBorder` and `Loading-BarFill`. Border centred (0.5, 0.075), size
  0.6 x 0.05; fill centred the same, 0.525 x 0.025, its right edge at
  `left + 0.525 x progress`, its texture coordinates fixed at 0..1 so the whole
  fill art is squeezed over the filled part. (`Loading-BarGlow` and
  `Loading-BarGlass` are not drawn here.) A second layout at y 0.975 is used by
  the taxi variant (0x409b10).
- **Fill rules** (0x4079d0): progress = A if B is 0, else 0.5 A + 0.5 B, where
  A is reported by the world/map loader callbacks (0x40af40) and B by the
  object loader (0x40af90); a third input is multiplied by 0. Each setter
  ignores a report that is not larger than the last. In streaming-trial mode
  it is remapped to 0.3 x p + 0.7. The screen repaints at most every 250 ms
  (0x40a920) and always at 1.0.
- **Taxi variant**: with a LoadingScreenTaxiSplines path for the trip, the
  screen draws `Interface\Glues\LoadingScreens\DynamicElements` over the
  twelve `Interface\WorldMap\World\World%d` tiles and moves a marker along the
  spline (0x409880, 0x40a160).

## 7. Wowee, against the above

| Item | Before | Now |
|---|---|---|
| Scene field of view | diagonal / sqrt(1 + (4/3)^2) at every aspect: a 16:9 window framed 22% wider than stock | diagonal / sqrt(1 + a^2), a = min(window aspect, 16:9): stock framing up to 16:9; wider windows keep 16:9's vertical field and show more at the sides where stock has black bars |
| Scene clip planes | 0.5 / 30000 | the M2 camera's near/far |
| Character facing | turned to face the camera | the stand mark's orientation + facing; 0 on each selection, -15 on entering create and on each race click |
| Scene lights | the model's own lights | RaceLights rows replace the model's directional lights and ambient; model point lights kept (orc and troll gain their two directional lights) |
| Pet at select | none | the pet on attachment 1 at the family's size |
| Ghost at select | "(Ghost)" text only | kit 989 tint 0x8CBAFD and alpha 0.5 |
| Glue ambience | none | GlueScreenIntro on login, the scene's track on select and create, 4 s in, 1 s out on Enter World |
| Glue sounds | none | the PlaySound names above, through SoundEntries |
| Glue music | 80% volume, 1.8 s fade-in | full volume, no fade-in |
| Create start | Human male warrior, look 0 | random sex/race/class/look; looks kept per race and sex |
| Random name | a whole NameGen row | the 3-letter chain |
| Loading art | wide above 1.5, wide art stretched to the window | wide above 4/3 + 0.001, both fitted at their own aspect, `Interface\Glues\loading` fallback |
| Saved volumes | applied when the game screen first drew: the glue music, ambience and sounds played at the audio defaults (music at 30%); stock has its Sound_* CVars from Config.wtf before the first frame | applied at startup, right after the audio managers are made (`Application::initialize`, `SettingsPanel::applyAudioVolumes`); the game screen still re-applies them with the saved mute |
| Loading bar | 512x64 border at fixed screen units, glow and glass, any fill order | border/fill from the layout table in the art's rectangle, border and fill only, never moves back |
| Above 16:9 | every glue screen, the scenes included, across the whole window | GlueParent pillarboxed (2026-11-07, screen-space.md section 3): `glue::GlueParentScope` around the glue states in `UIManager::render` gives the screens GlueParent's size and pointer, moves their draw lists into the centred 16:9 rect, clips them to it and fills the bars black; the scenes are built at that size, so the ModelFFX aspect is never above 16:9 |

### Not done

- **Glow** (`SetGlow`, ModelFFX glow 0.08/0.15/0.3, ghost 0.15): the preview
  has no full-screen glow pass.
- **Ghost extras**: the `Ghost_state` effect model on the Base attachment, the
  ghost light set and the ghost full-screen effect.
- **Loading screen** taxi variant and the exact A/B progress split (Wowee
  reports one figure from its own load steps; it is now monotonic).

### Checking it

`WOWEE_GLUE_OPEN=create` with `WOWEE_AUTO_LOGIN` goes straight from the
character list to character creation, so the create screen can be screenshot
unattended. The glue ambience and scene lighting choices log at info level
(`glue_audio: ambience ...`, `CharacterPreview: scene lights N (RaceLights KEY)`).

## 8. Moving to the stock GlueXML

Wowee's FrameXML engine (`src/addons`, `src/ui/widget_*`) runs the in-game
FrameXML. Running GlueXML on it would need, at least:

1. A glue Lua state started before login, loading `GlueXML.toc` (the TOC
   loader already recognises the directory, `addon_manager.cpp`), with the
   glue globals (GlueStrings) and `GlueParent` as the root.
2. The glue C API: about 150 functions - account login/token/EULA
   (`DefaultServerLogin`, `GetSavedAccountName`, ...), realm list
   (`RequestRealmList`, `GetRealmInfo`, `ChangeRealm`, ...), character list
   (`GetNumCharacters`, `GetCharacterInfo`, `SelectCharacter`,
   `EnterWorld`, `DeleteCharacter`, rename/declension), character
   customisation (`GetAvailableRaces/Classes`, `SetSelectedRace/Sex/Class`,
   `CycleCharCustomization`, `RandomizeCharCustomization`, `GetRandomName`,
   `CreateCharacter`, paid services), `PlayGlueMusic`/`PlayGlueAmbience`,
   options and addon list - each wired to `AuthHandler`/`GameHandler`.
3. The glue events (`SET_GLUE_SCREEN`, `CHARACTER_LIST_UPDATE`,
   `UPDATE_SELECTED_CHARACTER`, `OPEN_STATUS_DIALOG`, `DISCONNECTED_FROM_SERVER`,
   ...) raised from the C++ state machine instead of the screens' own logic.
4. A `ModelFFX` widget: a full-frame `CharacterPreview` with `SetModel`,
   `SetCamera`, `SetSequence`, fog, glow, `ResetLights`/`Add*Light`, and the
   character/pet attachment that `SetCharSelectBackground` /
   `SetCharCustomizeBackground` / `Set*Facing` drive.
5. The glue frame templates and fonts (GlueFonts, GlueButtonTemplate,
   GlueDropDownMenu) through the existing XML/template loader.

The 3D side (item 4) is what this pass made stock-correct inside
`CharacterPreview`, so it would carry over. Items 1-3 are the bulk: a second
UI runtime and a large API surface, for screens that are now close to stock
already. Not started; it should be its own project.
