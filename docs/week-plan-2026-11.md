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
