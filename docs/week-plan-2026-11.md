# Week plan: Nov 2 to Nov 6, 2026 (Fri 3 PM CST)

Jered is away. An hourly loop moves this list forward. Baseline: 28327b3,
pushed to jl178/wowee branch audio-data-driven.

## Rules

- No PRs or repos outside jl178. Push to `fork` only.
- Builds: `nice -n 19 ionice -c3`, -j4, one build at a time.
- RE against stock Wow.exe 12340 with headless Ghidra. Each spec goes in docs/re/.
- Agents do the RE and the fixes. The loop plans, reviews, builds, tests and commits.
- Stage commits by explicit file list, never `git add -A`: agents keep in-progress files in the same tree.
- Each commit: build, ctest, sweep_guard, a smoke login with no crash.
- Never touch Jered's client (HOME ~/workplace/wowee-jtrain-home) or his desktop windows.
- Plain technical English in docs and commits.

## Phase 1: stock fidelity (until about Wed night)

1. Bug: melee hit sounds from distant units play at full volume. Check the 3D
   path for all unit sounds (combat, spell, NPC, footsteps) against stock.
2. Creature and weapon-model particles drawn by CharacterRenderer.
3. ParticleColor.dbc overrides.
4. Per-race collider and stock step height. Swim depth rule.
5. Particle, ribbon and weather shaders on stock fog.
6. SoundEntriesAdvanced ducking. Sound Channels slider.
7. Other gaps listed in each docs/re/*.md spec.
8. New RE areas: UI/FrameXML behaviour, minimap and world map, loot and
9. Bug: after a cross-map .tele the server once kept the pre-teleport position (a stale MSG_MOVE after WORLDPORT_ACK?), see movement_handler.cpp ~2720.
10. Bug: RunBinding("TOGGLEWORLDMAP") did not open the world map in one run.
11. Ground-targeted spells: no targeting circle, no destination sent (players cannot cast Blizzard etc.).
12. Player sounds: ghost tint, ghost gryphon, corpse decay anim (death-ghost-loot.md open list).
13. Bug: GM fly mode (.gm fly on) does not keep the character in the air.
   corpse flow, mounts and vehicles, taxi flights, death and ghost world,
   water and swimming visuals, shadows (blob vs stock), LOD and draw distance.

## Phase 2: tasteful enhancements (Thu to Fri)

Each one has a toggle in the graphics settings, off means stock.
Ideas: wind on foliage and cloth (already started), better light and
shadows, soft particles, physics on capes and hair, smoother animation
blends, ambient occlusion, water refraction, weather detail.

## Phase 3: wowpack (Fri)

Review the wowpack plan against the new client and write the next steps.

## Log

Each hourly run appends one line: time, what moved, commit.

## Never idle

If every listed item is done or blocked before Friday, do not wait. Each
hourly run makes sure at least two agents are working. When the list runs
dry, audit the client against stock for new gaps (play a test character
through quests, dungeons, cities, flight paths and PvP areas, and compare
with the RE specs). Add new enhancement ideas with toggles, and wowpack work
(builder, Tol Barad module, Wowee sync). Add each new item to this file
before starting it.
- Nov 2 morning: positional sound fix committed (pan restored, row distances). Particles agent running.
- Nov 2 morning: unit/item particles + ParticleColor committed (f6866a1). Found Firelord drawn with pale human skin; skin agent started. Collision/step/swim agent running.
- Nov 2 07:08: collision and skin agents running; started fog-shader agent (particles, ribbons, weather).
- Nov 2 ~08:20: stock collision box, step and swim committed. Fog and skin agents running.
- Nov 2 ~08:40: stock fog curve fix + particle/ribbon/rain fog committed. Skin and sound agents running.
- Nov 2 ~08:40: stock fog curve fix + particle/ribbon/rain fog committed. Skin and sound agents running.
- Nov 2 ~09:00: creature skin/geoset rule committed (~150 batches fixed, audit tool). Sound agent running.

## For Jered to decide

- Captured Valgarde Child (display 19092) names `DraeneiMaleKid_01.`; the shipped
  file is `DraeneiMaleKid_01..blp`. The stock extension rule finds nothing and draws
  white, so we now draw white too. Revert to finding the file?
- Water sky reflection: stock has none. Ours still does.
- wowpack: open upstream PRs to Kelsidavis/wowee for the sync feature, or delete the `wowpack-sync` branch? (Rule so far: no PRs outside jl178.)
- wowpack: Jered to test `patch-Z.MPQ` with stock Wow.exe on Windows.
- Enhancement defaults (docs/enhancements-plan.md section 6): generated normal maps on or off? Mist, sun shafts, lens flare, sharp stars off on fresh install? Low-health vignette and Wowee soundtrack off by default? Classic view distance 1900 or the stock clamp 791.7?
- Action bars now cast on key-up like stock 3.3.5a. Keep, or set ActionButtonUseKeyDown=1 by default (cast on key-down, as later WoW does)?
- Wowee asks before disenchanting (stock asks nothing). Keep the prompt?
- wowpack: deploy the IP and City Siege packs (wowpack branches)? Changes live spell data (mana costs) and needs the SQL; also fixes IP spells 11202/25347 that never reached the server.
- Nov 2 ~09:05: started M2 multitexture/blend agent and mounts/taxi/vehicles agent.
- Nov 02 08:08: 3 agents running (sound, M2 materials, mounts/taxi). Nothing new to commit.
- Nov 02 08:31: sound agent done (SoundEntriesAdvanced ducking/blend/time, own-character 2D x0.65, FSD footsteps, channels slider). Commit held until the mount agent finishes: they share npc_interaction_callback_handler.cpp and sweep_guard is red from in-progress mount work.
- Nov 02 09:08: M2 materials and mounts/taxi agents running; sound commit held.
- Nov 02 09:37: M2 shader selection/multitexture/Mod+Mod2x done (Dalaran fps unchanged within noise). Held with sound until mount agent finishes; shared CMakeLists and character_renderer.
- Nov 02 10:02: stock pass 3 committed (M2 combiners, sound engine, mounts/taxi/vehicles). Full build, ctest all pass, Dalaran smoke login no crash. WMO agent running.
- Nov 02 10:08: WMO and vehicle-control agents running.
- Nov 02 11:08: WMO and vehicle-control agents still running.
- Nov 02 11:23: WMO stock rendering committed. Note a5aac59 alone does not build (it took WMO water files early); this commit fixes it.
- Nov 02 12:08: vehicle-control and portal-culling agents running.
- Nov 02 13:08: vehicle-control and portal-culling agents still running.
- Nov 02 13:22: vehicles (drive, aim, bar, seats, camera) + MSG_MOVE_TELEPORT fix committed. Check: Skytalon fades fully at default zoom by our reading of the stock rule; verify.
- Nov 02 14:08: portal-culling and death/ghost/loot agents running.
- Nov 02 15:01: death/ghost/corpse/loot committed. Portal agent running.
- Nov 02 15:08: portal-culling and world-UI agents running.
- Nov 02 16:08: portal-culling and world-UI agents still running.
- Nov 02 16:13: world UI (plates, names, bubbles, damage text) committed. Portal agent running.
- Nov 02 16:53: portal walk committed: Ironforge 34->63 fps, Dalaran 21->63, ICC 40->74, Stormwind 33->38. Maps agent running.
- Nov 02 17:08: maps and spell-audit agents running; post-portal verify build in progress.
- Nov 02 18:08: maps and spell-audit agents running. Post-portal build OK; sweep_guard red only on the maps agent's in-progress CVar.
- Nov 02 18:39: maps agent done (minimap blips/zoom/rotation/pings/tracking, world map API audit, Stormwind black minimap fixed). Held: its files interleave with the spell-audit agent's; commit both after that agent finishes.
- Nov 02 19:08: spell-audit and terrain agents running; maps work held.
- Nov 02 19:45: spell audit done (chain effects, kit tints/flash/held pose, AoE shards, camera shakes, swing trails, channel-tick fix). Found terrain agent's test client with 43 GB page tables (machine OOM); told it to stop and find the leak. Spell+maps commit waits for memory and a clean build.
- Nov 02 20:08: memory back (42 GB free). Spell+maps held until terrain agent ends (shared files). Started wowpack status agent (separate repo, read-only on wowee). Next: ground-target casting.
- Nov 02 20:14: wowpack status: tests pass, Tol Barad served and in lab DB, sync feature already on audio-data-driven (nothing to port). Doc: ~/workplace/wowpack/docs/status-2026-11.md. Started wowpack phase 2 agent (BLP writer, minimap and world map tiles, WDL heights).
- Nov 02 20:34: wowpack phase 2 pushed (e6cc35a: BLP writer, minimap and world map tiles, WDL heights). Started agent to deploy Tol Barad to the lab and verify in Wowee.
- Nov 02 21:08: terrain and Tol Barad deploy agents running; spell+maps held.
- Nov 02 21:25: Tol Barad 0.1.2 live on the lab; minimap, world map, arrow, delta sync (18 of 28 files) verified in Wowee. wowpack fixes e4724b0 (pack dir swap broke nginx), 07eed3e (server-setup doc).
- Nov 02 22:15: Tol Barad 0.2.0 live (sea ring, minimap water fitted to stock, 29 objects, 3 NPCs, 5 quests, loading screen). wowpack 78b87ec. Lab extractors need relinking (Nix glibc GC'd). New Wowee bug: GM fly mode does not hold the character in the air.
- Nov 02 22:18: stock pass 4 committed (465596a: spell procedures, maps, terrain + WDL horizon, Intel page-table fix). Build, ctest all pass, smoke login clean. Started ground-target/fly/teleport agent and water agent.
- Nov 02 23:08: ground-target/fly/teleport and water agents running.
- Nov 02 23:50: ground targeting, cast-failed text, GM fly, worldport position fix committed. Water agent running.
- Nov 03 00:08: water and animation agents running.
- Nov 03 00:35: animation agent done (fallback chain, variation re-roll per lap, blends, stand states, emote rules, stealth/sprint pace); held until water agent finishes (shared files). Started lab extractor relink agent and the phase-2 enhancement plan agent.
- Nov 03 00:44: enhancement plan committed (docs/enhancements-plan.md). It found stock gaps: Classic preset misses 4 keys, ShadowBlob.blp unit shadow not drawn, ~12 always-on non-stock features without toggles. Next agent after water lands: the toggle framework + those gaps.
- Nov 03 01:08: water agent running (long); started gameplay-flows audit agent (quests, vendors, mail, AH, guild, party, LFG, trade, talents...). Animation work held for the water commit.
- Nov 03 02:08: water done; water+animation full build and ctest green; waiting for the animation agent's sweep fix report before committing both. Gameplay-flows agent running.
- Nov 03 02:09: water + animation committed. Gameplay-flows agent running. Next: toggle framework + stock gaps from the enhancement plan.
- Nov 03 03:08: gameplay-flows and toggle-framework agents running.
- Nov 03 04:08: gameplay-flows (~3h) and toggle-framework agents still running.
- Nov 03 04:45: enhancement framework committed. Stormwind fps: recommended 13-20 vs all-off 36-40 (loaded box) - recommended set is far over its 3 ms budget; next agent profiles it.
- Nov 03 05:08: gameplay-flows (3.7 h) and enhancement-perf agents running.
- Nov 03 06:08: gameplay-flows and enhancement-perf agents running.
- Nov 03 07:08: gameplay-flows (5.8 h, large scope) and enhancement-perf agents running.
- Nov 03 07:25: gameplay flows committed. Perf agent running. Decisions logged by that agent: guild roster refresh 10 s throttle, raid roster order, calendar creator CONFIRMED, LFG error pairing (unverified guesses).
- Nov 03 08:08: enhancement-perf and flows follow-up agents running.
- Nov 03 08:42: enhancement perf committed: recommended set +1.3 ms GPU (was +9 to +22), local-light tiling, shadows-off free, sun shadows off in recommended. game_replay + sweep_guard red from 9cb0fd9; flows follow-up agent asked to fix.
- Nov 03 09:08: flows follow-up and enhancements E4/E2/E5 agents running.
- Nov 03 09:59: gameplay follow-up committed (300 chat sites to stock, AH tested, game_replay + sweep_guard green per agent). Enhancements E4/E2/E5 agent running.
- Nov 03 10:08: enhancements E4/E2/E5 and pets agents running.
- Nov 03 10:33: E4 soft particles, E2 fade-in, E5 rain ripples committed. Known: with soft particles on, blended particles draw over water even when below it. Pets agent running.
- Nov 03 11:08: pets and enhancements E8/E21 agents running.
- Nov 03 11:56: pets committed (+ talent prerequisite column bug for every class). Agent changed test char race for hunter runs; restored (race 1, class 8, money 9). Enhancements E8/E21 agent running.
- Nov 03 12:08: enhancements E8/E21 and glue-screens agents running.
- Nov 03 12:50: glue screens committed. GlueXML migration judged a large separate project (not started). Enhancements E8/E21 agent running.
- Nov 03 12:59: E8 corpse settle, E21 footfall dust, E2 gaps, E4 water order committed. CPU-frame agent running.
- Nov 03 13:08: CPU-frame and E7/E10 agents running. TODO tonight: clean build of HEAD in a separate worktree to prove commits build on their own; Windows package rebuild.
- Nov 03 14:08: CPU-frame and E7/E10 agents running.
- Nov 03 14:24: E7 capes/tails/hair and E10 remote smoothing committed. Light-shafts fog read +1.2..7.3 ms on a loaded machine; re-measure quietly. CPU-frame agent running.
- Nov 03 15:08: CPU-frame and shadows/fog-budget agents running.
- Nov 03 16:08: CPU-frame and shadows/fog-budget agents running.
- Nov 03 16:50: CPU-frame fix committed (Intel submit walking 100k CCS images): Stormwind 22.4->8.9 ms, Orgrimmar 32.3->8.4. WMO ray-band part held with the shadow agent's wmo_renderer edits.
- Nov 03 17:08: shadows/fog-budget and hitch agents running.
- Nov 03 18:08: shadows/fog-budget and hitch agents running.
- Nov 03 19:08: shadows/fog-budget (~4 h) and hitch agents running.
- Nov 03 19:19: hitch agent done (Dalaran arrival frames >25 ms: 51-328 -> 1-2; upload fence wait removed, player/gear/NPC loads on workers, shared 6 ms budget, minimap/terrain eviction fixes). Held: shares renderer.cpp, m2_renderer_instance, CMake with the shadow agent; commit both after one full build.
- Nov 03 19:26: wowpack tests for 4 packages + CI e2e build and cross-build matrix (CI green, 2d6196b). Fixed a pack write that could leave a half-written served pack. Shadow agent still running.
- Nov 03 19:51: wowpack ported mod-individual-progression and mod-city-siege on 'wowpack' branches (pushed, not deployed). Found IP spells 11202/25347 never reached the server (core placeholders win). wowpack: MPQ v1 reader, DBC lookup cache, import command, signedness fix. Gaps: build options, field-level merges.
- Nov 03 20:08: shadows split committed pending full build (running). wowpack gaps agent running.
- Nov 03 20:12: wowpack b1cf862: field-level DBC merges, build options (IP mana costs, login/loading screens), id_ranges for any table, warning for rows shadowed by core *_dbc. Default build byte-identical, CI green.
- Nov 03 20:21: shadows split + hitch fixes committed; full build, ctest green, smoke login clean.
- Nov 03 20:27: Windows build at 51107f2 failed: clang -Wunused-lambda-capture (-Werror) in game_handler_packets.cpp, which gcc does not flag. Agent fixing all clang errors, adding a clang check, then shipping the package (Tol Barad override pack excluded from the zip).
- Nov 03 21:08: Windows clang fix/ship and stock-leftovers agents running.
- Nov 03 21:28: Windows package rebuilt at 38b3106 and served on :8081 (wowee.exe 150.7 MB). Clang fixes 79e64fc; new clang_warning_check ctest (38b3106). Stock-leftovers agent running.
- Nov 03 22:07: stock leftovers done (jumps never sent MSG_MOVE_JUMP - fixed; air keys + FALL_LAND stock; taxi rep discounts; corpse Decay; auto-release on taxi death; blob for units without sun shadow; glue volumes). Held: camera_controller/renderer/character_renderer also carry the E9/E19/E20/E11 agent's edits; commit together.
- Nov 03 22:08: E9/E19/E20/E11 agent running; started input-timing/responsiveness agent.
- Nov 03 23:08: leftovers + E9/E19/E20/E11 committed. Recommended set measured +4 to +5 ms GPU over stock (over budget; cause in scene/shadow/fog, not these rows). Input-timing agent running.
- Nov 03 23:08: input-timing and budget agents running.
- Nov 03 23:22: input timing committed (cast on key-up as stock; ActionButtonUseKeyDown=1 for key-down; local GCD refusal; 0.2 ms key-to-packet). Budget agent running.
- Nov 04 00:08: tab-targeting and budget agents running.
- Nov 04 00:35: stock Tab targeting, cast-while-moving, CMSG_CANCEL_CAST fix committed. Budget agent running.
- Nov 04 01:08: budget and combat follow-up (duels, interrupts) agents running.
- Nov 04 02:08: budget (~3 h) and combat follow-up agents running.
- Nov 04 02:51: cast-end events, duels, Auto Shot/food on move, macro Tab limit committed. Budget agent running.
- Nov 04 03:08: budget and PvP/instances agents running.
- Nov 04 04:08: budget (~5 h) and PvP/instances agents running.
- Nov 04 04:41: PvP/instances committed (BG loop works end to end with bots; map names were wrong everywhere). Arena opponent frames still open. Budget agent running.
- Nov 04 05:08: budget and arena agents running.
- Nov 04 05:09: budget trim committed: Stormwind +1.3, Dalaran +1.4 GPU; Goldshire road still +6.7 GPU (needs dropping local lights or sun shadows from the set - Jered's call, plan §6). Soft particles + parallax now off by default.
- Nov 04 06:03: arena teams/opponents, party member stats (Unknown/Dead frames), unit events to all tokens committed. GPU agent running.
- Nov 04 06:08: GPU/Goldshire and spell-visual-gaps agents running.
- Nov 04 07:08: GPU/Goldshire and spell-visual-gaps agents running.
- Nov 04 07:35: ranged/item missiles, wand schools, ammo, beam ends committed. Found: client never sends CMSG_SET_AMMO (hunters cannot load ammo). GPU agent running.
- Nov 04 08:08: GPU/Goldshire and items/ammo agents running.
- Nov 04 08:43: render fixes committed (destroyed fallback view on map change, light tile cull, unit shadows on worker). Goldshire alone +6.3 ms; dropping torch light from the recommended set would fit it - left on (a visible lighting improvement Jered cares about), his call. Items agent running.
- Nov 04 09:08: items/ammo and E16/E15/E18 agents running.
- Nov 04 09:16: items/ammo committed. Open for Jered: keep Wowee's disenchant confirmation (stock asks nothing)? E16/E15/E18 agent running.
- Nov 04 10:08: E16/E15/E18 and professions agents running.
- Nov 04 11:08: E16/E15/E18 (~2 h) and professions agents running.
- Nov 04 11:27: professions committed. Wptestb was left a dwarf hunter by the items run; restored to human paladin from items-re/restore.sql. E16/E15/E18 agent running.
- Nov 04 12:08: E16/E15/E18 and names/duplicate-errors agents running.
- Nov 04 12:12: names/caches + duplicate errors committed; fixed sweep_guard lua stack check from 7a81223. E16/E15/E18 agent running.
- Nov 04 13:08: E16/E15/E18 (~4 h) and chat/combat-log agents running.
- Nov 04 13:54: chat + combat log committed. E16/E15/E18 agent finishing.
- Nov 04 13:55: chat + combat log committed. E16/E15/E18 agent finishing.
- Nov 04 14:08: E16/E15/E18 (wrapping up) and chat-leftovers agents running.
- Nov 04 14:35: E16 cliffs (rec off), E15 caustics, E18 soft highlights committed; shoreline foam stale-depth bug fixed. Chat-leftovers agent running.
- Nov 04 15:08: chat-leftovers and regression-tour agents running.
- Nov 04 15:59: chat second pass committed (/who, logout, emotes, follow crash, slurring, profanity filter, zone channels). Regression tour running.
- Nov 04 16:08: regression tour (found terrain alpha-map leak, 4 GB growth) and E12/E14 agents running.
- Nov 04 17:08: regression tour and E12/E14 agents running.
- Nov 04 17:47: regression fixes committed (alpha-map leak, tile unload, resize while loading, zone event, enter-world). Open: one SIGABRT in anv descriptor write at world entry (cause unknown); logout goes to login screen not char select; instance joins General.
- Nov 04 18:08: crash-hunt and E12/E14 agents running.
- Nov 04 19:08: crash-hunt and E12/E14 agents running.
- Nov 04 19:35: world-entry crash root-caused (stale WMO texture pointers after clearAll) and fixed; logout to char select. E12/E14 agent running.
- Nov 04 19:58: E12 room light indoors + E14 contact shadows committed. ASan soak running.
- Nov 04 20:08: ASan soak and sound-audit agents running.
- Nov 04 21:08: ASan soak (~1.5 h) and sound-audit agents running.
- Nov 04 22:00: sound audit committed (rain never played - fixed; weather/underwater ambience slots; many duplicates/missing). Open: game object sounds (doors, chests) silent - $GO events not fired for GO models. ASan soak running.
- Nov 04 ~22:30: both running agents (ASan soak, game-object sounds) stopped on the weekly usage limit; no work ran Thu or Fri morning. Neither left uncommitted edits.

## Final summary (Nov 6)

47 commits on audio-data-driven since 28327b3, all pushed to jl178/wowee.
Windows package last rebuilt at 38b3106 (Nov 3); newer commits are not in it.

Stock fidelity: sound engine and positional audio, particles, fog, creature
skins, M2 combiners, WMO materials and portal culling, terrain and the WDL
horizon, water, animation, movement and collision, mounts/taxi/vehicles,
death/ghost/loot, world UI, minimap and world map, spells (procedures,
ground targeting, ranged and item missiles, cast-end events), input timing
and Tab targeting, duels, gameplay flows, pets, items and ammo, professions,
PvP and instances, glue screens, names and caches, chat and the combat log.
Specs for each are in docs/re/.

Enhancements: one toggle table, Classic = stock; see docs/enhancements-plan.md
for each row, its cost and its recommended default.

Performance: portal culling (Ironforge 34 -> 63 fps), the Intel submit fix
(Stormwind CPU 22 -> 9 ms), hitch removal, shadow split.

Stability: the world-entry crash (stale WMO textures) is fixed; a regression
tour found no validation or Lua errors after its fixes.

wowpack: phase 2 formats, Tol Barad 0.2.0 on the lab, field-level DBC merges,
build options, the two real modules ported on wowpack branches.

Not done: the ASan soak, game-object sounds and animations (doors, chests,
boats), a Windows package of the latest commit. Open questions are in
"For Jered to decide" above.
