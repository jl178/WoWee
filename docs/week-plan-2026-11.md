# Week plan: Nov 2 to Nov 6, 2026 (Fri 3 PM CST)

Jered is away. An hourly loop moves this list forward. Baseline: 28327b3,
pushed to jl178/wowee branch audio-data-driven.

## Rules

- No PRs or repos outside jl178. Push to `fork` only.
- Builds: `nice -n 19 ionice -c3`, -j4, one build at a time.
- RE against stock Wow.exe 12340 with headless Ghidra. Each spec goes in docs/re/.
- Agents do the RE and the fixes. The loop plans, reviews, builds, tests and commits.
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
