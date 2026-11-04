# Regression tour, 2026-11-04

A long scripted play-through of the client after the week's work
(`28327b3..95f1e52`, about 43 commits), looking for crashes, hangs, Vulkan
validation errors, Lua errors, memory growth and visible breakage before
Jered plays on Friday. No features were added; the fixes below are small and
each is tied to something the tour showed.

## The tour

One `WOWEE_DEBUG_LUA` script (scratchpad `tour/lua/tour.lua`, run by
`tour/run.sh`), Wptester (level 80 human mage, GM) on the lab realm, HOME
`scratchpad/testhome`, Iris Xe (RPL-P, Mesa 26.1.8), a 1251x986 window tiled
by Hyprland. Every stage writes `[luatest] STAGE <name>`; a sampler beside
the client writes RSS and VmPTE every 5 s.

1. Login, character select, world entry (`WOWEE_AUTO_LOGIN`,
   `WOWEE_AUTO_CHARACTER`). Every function a `Bindings.xml` body calls is
   checked for existence, and nine bindings are run with `RunBinding`.
2. Stormwind trade district. Every main panel by its binding, open and
   closed: character (paper doll, reputation, skills, currency), spellbook,
   talents, quest log, world map, bags, social, guild, LFD, LFR, PvP,
   achievements, statistics, game menu, key ring, calendar, macros, key
   bindings, help.
3. Options: every page of the Video, Sound and Interface frames (the WoWee
   pages, the three Enhancements pages included), opened through
   `OptionsFrame_OpenToCategory` / `InterfaceOptionsFrame_OpenToCategory` and
   closed with Cancel.
4. Presets: Classic, Ultra, Recommended set, Original, Recommended, Low,
   High, back to the start. Window size: `SetScreenResolution` 1, 3 and back.
5. Stormwind bank (Olivia Burnside) and auction house (Auctioneer Jaxon).
6. Goldshire; Fargodeep Mine with four kobold fights (Frostbolt, Fire Blast,
   Arcane Missiles); mounting (Brown Horse) and riding; GM fly across Elwynn
   at speed 6; swimming in Stormwind Harbor; the harbor's transports.
7. Ironforge, Rut'theran, Darnassus, Orgrimmar, Undercity, Thunder Bluff,
   Dalaran, Shattrath by `.tele`; the Deadmines instance with fights; a
   Random Battleground queue joined and left.
8. A chain of six teleports (Goldshire, Orgrimmar, Dalaran, Ironforge,
   Shattrath, Stormwind), the panels again, logout, and (with the new
   `WOWEE_AUTO_RELOG=1`) log back in and run the script again as pass 2.

Runs:

| Run | Binary | Validation | Length | Exit |
|-----|--------|------------|--------|------|
| v1 | HEAD 57797c1 | on | 34 min | clean |
| v2 | + first fixes | off | 14 min + relog | stuck at character select (fixed, see 7) |
| v3 | + fixes 1-7 | on | 27 min + relog | clean (pass 2 did not start: hook, fixed) |
| v4 | + fixes 1-7 | off | 13 min + relog + pass 2 | clean |
| ab-* | + fix 8, A/B | off / on | 4 min each, 16 teleports | one crash (see Crashes) |

Short targeted runs (`mini*.lua`, m1-m6, `ab.lua`) isolated single
questions. v4 shared the GPU with another agent's test client for its whole
length, so its frame times are worse than v2's for reasons that are not the
build's; compare its memory, not its fps.

## Found and fixed

### 1. A window resized during a loading screen kept its old swapchain

Seen as: half the interface missing. In Goldshire the action bar stopped at
the middle of the screen, the minimap ring sat in the middle of the top
edge and the minimap itself was drawn as a stretched ellipse at the right
(screenshot `v1_p1_goldshire.png`, `v1_p1_fly.png`). It lasted until a
later load happened to put the window back at its old size.

Cause: the three event pumps a map load runs instead of the main loop
(`world_loader.cpp`) took a `SDL_EVENT_WINDOW_RESIZED` off the queue, noted the
size and dropped it. The main loop's handler, which marks the swapchain
dirty and fires `DISPLAY_SIZE_CHANGED`, never saw it, and on Wayland nothing
else rebuilds the swapchain (presenting to a resized surface is not an
error there). Here it was another agent's client opening on the same
workspace during a teleport: Hyprland halved this window, the interface
laid itself out for 616 px and drew into a 1251 px image. A player sees the
same whenever a window is tiled in, a monitor changes or the window is
resized while a map loads.

Fix: the pumps hand window size, scale and close events back to the queue
when they finish (`core/window_events_for_main_loop.hpp`), so the main loop
handles them. Test: `test_loading_event_pump` drives the helper over SDL's
real event queue (a resize survives three pump passes; input is still
dropped).

### 2. Terrain alpha maps were never freed: 4 GB of dead textures, 0.4 s hitches

Seen as: VMA allocations climbing from 88 thousand to 1.06 million over a
14-minute tour, device memory 2.4 to 7.3 GB, RSS to 13.4 GB, and none of it
coming back at logout. Late in the tour two arrivals stalled for 422 and
433 ms in one frame:

```
[15:42:22.489] Terrain texture cache reached its 4096 MB budget and is now evicting - freed 65537 textures, 256 MB
[15:42:22.495] Terrain chunks [37,44] 4 in 421.848ms: buffers 0.038229, textures 421.765, ...
[15:42:45.508] Terrain chunks [32,47] 4 in 432.709ms: buffers 0.062703, textures 432.57, ...
```

Cause: `createAlphaTexture` makes a 64x64 image (one VMA allocation, a view
and a sampler) for every alpha layer and baked shadow of every terrain
chunk, under a key nothing looks up again, and puts it in the texture cache.
`destroyChunkGPU` left them there for the eviction pass. So every chunk ever
streamed kept its alpha maps until the 4 GB budget filled - about a million
of them - and from then on every pass that made room walked and sorted the
whole million (the 420 ms frames), once per 256 MB of new terrain. A long
flight or a taxi ride gets there; the tour got there in eleven minutes.

Fix: the cache records each alpha map's key by texture, and
`destroyChunkGPU` releases the chunk's own alpha and shadow maps with the
chunk, deferred past the frames in flight like its buffers
(`TerrainRenderer::releaseChunkAlphaTextures`). The shared opaque fallback is
never in that map. Result (v3, same tour, same stage): VMA 32 thousand
allocations and 3.2 GB instead of 430 thousand and 3.9 GB, RSS 9.5 GB
instead of 11.9 GB at the teleport chain, and no eviction pass. The
validation run (v3) reports nothing about destroyed images in live
descriptor sets.

| | VMA allocations at the teleport chain | device memory | RSS |
|---|---|---|---|
| v1 (HEAD, validation) | 429k (1.06M by the end of v2) | 3.9 GB (7.3 GB) | 11.9 GB |
| v3 (fixed, validation) | 32k | 3.2 GB | 9.5 GB |
| ab-keep1 (16 teleports, old path) | 659k | 4.4 GB | 6.9 GB |
| ab-rel3 (same, fixed) | 96k | 2.9 GB | 5.7 GB |

### 8. A terrain tile unload copied the whole chunk list 256 times

Seen while checking fix 2 (A/B, `ab.lua`, sixteen teleports across Elwynn
and Westfall): `update/pre/terrain/unloads` frames of 26-37 ms with the old
code and 66-73 ms with fix 2 alone.

Cause: `TerrainRenderer::removeTile` erased each of a tile's 256 chunks from
the `chunks` vector where it stood, moving every chunk after it each time;
with 45 thousand chunks resident (about 200 bytes each) that is hundreds of
megabytes of copying per unloaded tile, inside the unload budget that is
meant to keep it to 8 ms.

Fix: destroy the tile's chunks in order, then one `remove_if`. Measured in
the client (temporary timing, ab-rel3): a whole tile now unloads in 1.0-1.6
ms (destroy 0.4-1.3 ms of which alpha release 0.4-0.7 ms, compaction 0.2-0.3
ms). Unload stage over the same sixteen teleports: worst 15.4 ms, mean 0.02
ms per frame (old: 37.0 / 0.10; fix 2 alone: 73.0 / 0.94); frames over 25 ms
of CPU 69 (old 132).

### 3. `ZONE_CHANGED_NEW_AREA` came before the client knew the zone

Seen as: after any teleport within a continent (and a hearthstone is one),
`GetPlayerMapPosition("player")` answered 0,0 and `GetCurrentMapAreaID()`
was the zone just left, until the next zone change. The quest tracker's
map POIs were the old zone's for as long.

```
TOUR TELE Goldshire
ZEV ZONE_CHANGED_NEW_AREA areaid 302 Wptester zone=Stormwind City ... map=0.000,0.000
ZEV ZONE_CHANGED areaid 302 Wptester zone=Elwynn Forest sub=Goldshire map=0.000,0.000
```

Cause: the server's `SMSG_INIT_WORLD_STATES` names the new zone before the
client's own area lookup has terrain under the player, and
`announceZoneChange` fired the event from the packet. WatchFrame answered
it with `SetMapToCurrentZone`, which reads the client's (still old) zone.
When the lookup caught up, `announceZoneChange` refused a second event for
the same zone.

Fix (`Renderer::fireZoneEvents`): when the client's lookup arrives at a zone
that was already announced while it still said otherwise, the event is
fired again then. Checked: Goldshire, Ironforge and Orgrimmar arrivals now
have the right map and position on the first read (v2, v3).

### 4. `AttackTarget()` did not exist

The `ATTACKTARGET` binding's body and the usual `/script AttackTarget()`
macro called a nil global. Added as stock: it toggles auto attack on the
current target; no target is logged (budgeted warning, which
`silent_refusal_check` asks for). Committed with 95f1e52.

### 5. `UnitClass("player")` answered nothing while the player's unit was absent

During a map change the player's own entity is not in the entity list,
and `UnitClass("player")` returned nothing - which stops
`PaperDollFrame_SetStat` at `strupper(nil)` (paperdollframe.lua:269) if the
character sheet updates then. The player's class is known from the
character list, and WoW always answers for the player. Committed with
95f1e52. Not reproduced in my own runs (the frame closes on the loading
screen); the error was in another client's `lua_errors.txt` in the shared
test HOME, together with `pvpbattlegroundframe.lua:123` (`UnitFactionGroup`
nil, same family; `unitRaceOf` already answers for the player from the
character list, so that one needs no active character - see Remaining).

### 6. Options pages' own text was stale

The WoWee headings in Video and Sound said "the game's own Resolution and
Effects panels are above" and "The game's Sound panel above carries the
master, music, ambience and effects volumes" - but the Effects and Sound
pages were retired (`kRemovedCategories`) and are not in either list.
Corrected.

### 7. The character screen could enter the world on the previous session's list

After a logout the old character list stays while the next login runs. A
press of Enter World in that window (or `WOWEE_AUTO_CHARACTER`) reached
`selectCharacter` before `SMSG_CHAR_ENUM`, which refused it ("Cannot select
character in state: 4") after the screen had already set "Entering world":
the screen then waited for ever. Enter World now waits for this session's
list.

### Test tooling (not player-facing)

- `WOWEE_AUTO_RELOG=N`: after a logout, the auto login and auto character
  run again (up to N times) and the `WOWEE_DEBUG_LUA` script runs again in
  the new interface with `WoweeDebug_Pass` = 2, 3, ...
- `WoweeDebug_PadCommand` re-asserts the bindings' focus on each press, as
  `WoweeDebug_PushKey` does. A background window loses focus whenever
  another window opens beside it, and held commands then count for nothing.
- The script reruns on a new interface load (`debugLuaHookNoteInterfaceLoaded`
  from the world loader), not on a new Lua state: the state survives a
  logout, so v3's pass 2 never started.

## Validation

v1 (HEAD, validation on, 34 minutes, 24 map loads) and v3 (all fixes but 8,
27 minutes, logout and log back in): no validation error. The only layer
output is performance warnings from pipeline creation (vertex attributes 0,
2, 3 not consumed, and one vertex output with no fragment input - the water
caustics pipeline), at most ten each. At v1's shutdown VMA held 0
allocations. ab-val (every fix, validation on, 16 teleports with their tile
unloads): the same pipeline warnings and nothing else, and at shutdown
"VMA still holds 0 allocations" - the released alpha maps are all accounted
for and none is destroyed while a frame still uses it.

## Lua errors

None from my client in any run. The shared test HOME's `lua_errors.txt` was
twice written by another agent's client (at 15:30:56, before this client had
built its interface) with the two errors under 5.

`missing_api.txt` at the end of v1: the functions the interface reads while
nil are `CaptureBar_Hide` and `OptionsFrame_ToggleSubCategories`, both nil in
the stock client too.

## Crashes and hangs

**One crash, not explained.** ab-keep2 (the A/B build with the old alpha path
selected, validation off) aborted at world entry in Stormwind, about a second
after the terrain started loading:

```
[17:18:11.067] [WARN ] Terrain mega slots: 0 tiles within 1600 yd, 0 chunks in sl*** stack smashing detected ***: terminated
=== CRASH: signal SIGABRT (6) ===
#5  __stack_chk_fail (libc.so.6)
#6  anv_descriptor_set_write_image_view (libvulkan_intel.so + 0x6ce0d)
#7  0x000079ad00001000 n/a          <- the stack above here is overwritten
```

Main thread, inside `vkUpdateDescriptorSets` on Mesa 26.1.8 (Iris Xe); the
other threads were terrain workers in `generateNormalHeightMap` and a BLP
decode. The stack above the driver frame is garbage, so the caller is not
known, and the binary was rebuilt before the core could be symbolised (core:
`coredumpctl info 4162529`, 2.5 GB). One crash in about twenty client runs
today, none under validation. A plausible reading, not a proven one:
anv writes a per-plane descriptor array on the stack sized for three planes,
and a destroyed or garbage `VkImageView` handed to it reads a plane count
past three - i.e. a descriptor written with a stale view at world entry.
Next step: a loop of logins under `WOWEE_VULKAN_GPU_VALIDATION=1`, and
keeping the binary beside any core.

No other crash: no other core dump, no FATAL, no ERROR line in any run.
Main-loop stalls under validation (1.5-1.7 s) are the layer's cost during
loads. No hang.

## Frame hitches (v2, validation off)

70 ten-second windows: median 70 fps, lowest window 15 fps (a load).
Frames over 25 ms of CPU, by stage, excluding loading screens: most are the
screenshot frames the script takes (160-450 ms, untimed in the profile).
The real ones:

| Where | Frame | Cause |
|-------|-------|-------|
| teleport chain, Ironforge and Stormwind arrivals | 431, 433 ms | terrain texture eviction (fixed, 2) |
| any tile unload | 26-37 ms | chunk list compaction (fixed, 8) |
| Thunder Bluff arrival | 152 ms | terrain chunk textures, 136 ms for 4 chunks |
| first character sheet | 158 ms | `addonWidgets/paperdoll` 128 ms building the model |
| quality preset change | 101 ms | swapchain rebuild (expected) |
| arrivals in general | 55-70 ms | `update/pre/terrain/ready tiles` |

## Looked like bugs, were not

- **Bank and auction house did not open after the options stage** (v1-v3:
  BANKFRAME_OPENED, then `ShowUIPanel(BankFrame)` refused and the bank closed
  again; in a run without the options stage both open). m6 traced it panel by
  panel: after the script's `ShowUIPanel(VideoOptionsFrame)` and its Cancel
  button, the panel manager's "center" slot still held the hidden
  VideoOptionsFrame and `CanOpenPanels()` was nil, so every left or doublewide
  panel was refused. Cancel hides the frame with `:Hide()`; only
  `OptionsFrame_OnHide` returning to `lastFrame` (the game menu, which sets it)
  frees the slot - stock FrameXML, same in the real client, and nothing in this
  client opens the options any other way. The tour now opens them the game
  menu's way.
- **Falling after `.go` into the air with GM fly on**: CAN_FLY without FLYING
  falls until jump is held, as stock. Held jump takes off and flies (v3:
  `Flight: took off`; v4: 141 yd per 5 s by `.gps`, the 28 yd/s that
  `.modify speed all 4` asks for, from Goldshire into Stormwind).
- **The first AH miss in v1** was the script's position (upper floor).
- **Dismounted while riding** (v3, v4): the script rode from the Goldshire
  crossroads straight into the smithy, and the server takes the mount in an
  indoor area ("Authoritative mount field cleared" as the WMO ambience
  switches to BlackSmith.wav). Stock.
- **v1's options pages "HIDDEN"**: stock `OptionsFrame_OpenToCategory`
  returns at once when given a panel rather than a name.

## Remaining

- **The world-entry crash above** (`anv_descriptor_set_write_image_view`,
  stack smashed). Unexplained; the most important open item.
- **Memory is held by design.** Even with fix 2, RSS reaches 9.5-9.9 GB by
  the end of the tour. The caches are sized from free RAM at start (this box: file
  cache 12 GB, terrain tiles 4 GB, terrain/M2/character textures 4 GB each,
  WMO textures 8 GB) and none is emptied at logout; at character select after
  logout 13.2 GB were anonymous heap (v2, before fix 2). Nothing leaks - the
  validation run's shutdown finds 0 VMA allocations left - but a long session
  climbs toward the sum of the budgets. A decision for Jered: smaller budgets,
  or emptying the world caches at logout.
- **Duplicate option names.** The retired stock pages (Camera, Combat, Names,
  Combat Text, Sound) are still in the category lists under their stock names,
  so `InterfaceOptionsFrame_OpenToCategory("Combat")` - by name, as addons
  call it - finds the hidden stock page and shows nothing. Clicking in the
  list is not affected.
- **`RunBinding("TOGGLEAUTORUN")`** and the camera-view bindings (NEXTVIEW,
  PREVVIEW, SETVIEWn, SAVEVIEWn, RESETVIEWn, PITCHUP, FLIPCAMERAYAW) run
  without error but do nothing: their Bindings.xml bodies call functions this
  client does not define (`ToggleAutoRun`, `NextView`, `SetView`, ...). The
  NumLock key itself works through the client's own autorun path. 45 such
  names in all, most of them debug bindings (performance display, collision
  display, music player).
- **Instance chat channels.** Entering the Deadmines printed "Changed
  Channel: [1. General - The Deadmines]"; stock does not join zone channels
  in an instance. Not investigated (chat code belongs to another agent).
- **`WoweeDebug_PushKey` does not move the character** in a background
  window (the key reaches the input trace; `isCommandDown` does not see it).
  `WoweeDebug_PadCommand` does. Test-hook only.
- **Logout went to the login screen**, not character select (stock goes to
  character select). Long-standing; noted, not changed.
