# Phase 2 plan: enhancements over the stock 3.3.5a picture

Status: written 2026-11-03 against branch `audio-data-driven`, after stock
passes 1-4 (see `docs/week-plan-2026-11.md` and `docs/re/*.md`).

Done (2026-11-03, framework step): section 3.1-3.3 and the U list.
`rendering::Enhancements` (`include/rendering/enhancements.hpp`) and
`kEnhancementRows` (`include/ui/enhancement_rows.hpp`) are the one table; the
schema rows, saving, loading, the presets (Classic = `Enhancements{}`), the
login options and the three pages "Enhancements", "Enhancements: Sky" and
"Enhancements: Motion" (plus the Grass, Ray Tracing, HUD and Sound rows) are
read off it, with an "Enhancement set" chooser (Custom / Original / Recommended)
as the reset-to-stock control. `test_enhancements_stock` holds Classic to
`Enhancements{}`. `WOWEE_FREEZE_CLOCK`, `WOWEE_ENHANCE_AB=<key>:<s>` and
`WOWEE_ENHANCE=stock|recommended` exist. U1-U4, U6-U10, U12, U13 have rows
(U5 is part of `locallights`); U11's shoulder curve is in
`postprocess.frag.glsl`, which nothing loads, so there was nothing to switch;
U14-U17 are unchanged. S1 (the stock blob) and S2-S4 are done; the water
refraction switch is gone. The stock behaviour for off came from new RE:
characters.md §1 "Point lights: full rule" and §6 "Unit blob shadow: full
rule", world-lighting.md "Shadow map filter (PCF taps)" and "Drunk".

Done (2026-11-03, step 3 part): E4 soft particles, E2 streaming fade-in and
E5 rain ripples, each a row (`softparticles`, `streamfadein`,
`rainripples`), recommended on, stock off. Status, what each looks like and
its measured cost are in 3.6 and under each entry in section 4.
`test_phase2_enhancements` holds the logic; `test_enhancements_stock` still
holds Classic to `Enhancements{}`.

Done (2026-11-03, step 4 and E4/E2 follow-ups): E4's draw order fixed (the
water, weather, spray and minimap come after the late particle pass in every
configuration; a particle under water is under it again), E8 corpses settle
to the slope (`corpsesettle`), E21 footfall dust for everyone's feet
replacing the mount dust (`mountdust`, relabelled "Dust kicked up"), and E2
now fades in ground clutter and server game objects. Each new row is
recommended on and stock off; costs in 3.7, status under each entry.

Done (2026-11-03, step 6): E7 capes, robe flaps, tails and hair
(`secondarymotion`, Off / Capes and robes / everything, recommended
everything) and E10 smoother other units (`remotesmoothing`, recommended
on), both stock off; one spring chain system for all of E7, its bones chosen
from each model by a documented rule. Costs in 3.8, status under each entry;
`test_secondary_motion` holds the solver and the rules.

Done (2026-11-03, after Friday's list): E9 feet on the ground
(`footplacement`), E19 rain and snow in the wind (`weatherwind`), E20 smooth
zoom (`smoothzoom`) and E11 the lit far horizon (`horizondetail`), with the
WDL's low-detail WMOs now drawn in the horizon pass in both modes (a stock
gap closed). Each is a row, stock off; recommended values and costs in 3.10,
status under each entry. `test_phase2_motion` holds the IK's rule and
limits, the zoom's ease, the wind's push and the horizon's haze.

## 1. Goal and rules

The client now draws close to stock 3.3.5a (build 12340). Phase 2 adds
enhancements on top, in the spirit of "the reference client": the classic feel,
with the rough edges smoothed. The game must still look like 3.3.5a.

Rules for every enhancement:

1. **One toggle each**, in the graphics settings. Off is the exact stock
   behaviour: the same pixels, the same sounds, the same timing.
2. **Off costs nothing.** No pass runs, no resource is allocated, no
   per-frame CPU work happens. The shader path is the stock path, not a
   blend with weight 0.
3. **Classic preset is all off.** Picking Classic gives the stock picture,
   whatever was set before. A test enforces this (section 3.3).
4. **Taste first.** An enhancement may add motion, smooth an artefact or
   soften an edge. It may not change a zone's palette, the art's shapes or
   the light Light.dbc asks for. If it makes the game read as a newer
   expansion or a different game, it is rejected.
5. **Data before invention.** Prefer driving an effect from data the client
   already has (Light.dbc, LiquidType, FootstepTerrainLookup, WDL, MOCV,
   MOLT, bone tables) over hand-tuned constants.

## 2. Inventory: what is already not stock

Sources: `src/ui/settings_schema.cpp`, `include/ui/settings_panel.hpp`,
`include/ui/graphics_presets.hpp`, the "Wowee extras" lists in
`docs/re/world-lighting.md` §9, `water.md` §7, `terrain.md` §10,
`m2-runtime.md` §5, `particles.md` §6, `characters.md` §8,
`sound-engine.md` §8, and a read of the renderer and shaders.

"Default now" is the schema default a fresh install gets. "Classic" is the
Classic preset's value. "Recommend" is the proposed fresh-install default.
The reasons follow the table.

### 2.1 Features with a toggle

| # | Feature | Where | Toggle key | Default now | Classic | Recommend |
|---|---|---|---|---|---|---|
| 1 | Shadow map from the sun on terrain, M2, WMO and units | `renderShadowPass`, `sampleShadowPCF` in terrain/m2/wmo/character .frag | `shadows`, `shadowdistance` | on, 300 yd | on, 350 | off by cost (3.5); the quality presets keep it |
| 2 | Shadow darkness multiplier | `shadowParams.y` | `shadowstrength` | 1.0 | 1.0 | 1.0 (stock) |
| 3 | Foliage and cloth wind (trees, banners, awnings, WMO banners), weather-driven, indoor cut | `m2.vert`, `m2_shadow.vert`, `wmo.vert`, `Renderer` wind block | `foliagewind` | 1.0 | 0 | 1.0 |
| 4 | Volumetric mist and light shafts | `volumetric_fog.cpp`, `volumetric_fog_*.comp` | `lightshafts`, `mistdensity` | Medium | Off | Off; Low or Medium only on High/Ultra |
| 5 | Screen-space sun shafts | `sun_shafts.cpp` | `sunshafts` | on | off | off |
| 6 | Lens flare | `lens_flare.cpp` | `lensflare` | 1.0 | 0 | 0 |
| 7 | Sharp point stars | `starfield.cpp` | `sharpstars` | on | off | off (taste; see note) |
| 8 | Generated normal maps | `normal_map.cpp`, wmo/m2/terrain .frag | `normalmapping`, `normalmapstrength` | on, 0.8 | off | on, per Jered's stated preference (note) |
| 9 | Generated parallax | same | `parallax`, `parallaxquality` | on, Medium | off | follows 8 |
| 10 | Shoreline foam | `water.frag` (`shorelineFoam`), depth copy | `shorelinefoam` | on | off | on |
| 11 | Wet ground in rain | `wet_surface.glsli` | `wetground` | on | off | on |
| 12 | Sky reflection on water | `water.frag`, reflection pass | `waterreflections` | off | **not set** | off (open decision for Jered) |
| 13 | Swim spray, wake froth, bubbles, reed midges | `swim_effects.cpp`, `water_renderer` wake | `watersplashes` | off | **not set** | split: wakes and spray on, midges off (note) |
| 14 | GPU grass blades | `grass_renderer.cpp` | `grassenabled` + 3 sliders | off | off | off (experimental) |
| 15 | Ray traced sun shadow, occlusion, bounce | `rt_lighting.cpp`, `rt_*.comp` | `raytracedlighting` | off | **not set** | off |
| 16 | Fog thickness and fog sky blend | `LightingManager` | `fogstrength`, `fogskyblend` | 1, 0 | 1, 0 | 1, 0 (stock) |
| 17 | Grass/clutter density above 100% | terrain manager | `groundclutter` | 70 | 100 | stock value (note) |
| 18 | View distance beyond the stock far clip (stock UI 177-1277, clamped to 791.7 on maps below 530) | terrain, M2, WMO | `viewdistance` | 1900 | 1900 | 1900, with the WDL horizon (E11) |
| 19 | Camera follow speed, pivot height, smooth follow, idle orbit | `camera_controller.cpp` | `camerastiffness`, `pivotheight`, `smoothfollow`, `idleorbit` | 30, 1.6, off, off | n/a | verify 30/1.6 reproduce stock (note) |
| 20 | Camera shake scale | camera controller | `camerashake` | 1.0 | n/a | 1.0 (stock) |
| 21 | Low health red vignette (Wowee's own HUD effect) | game screen | `lowhealthvignette` | on | n/a | off; stock has `screenEdgeFlash` |
| 22 | Wowee soundtrack in the music rotation | `ZoneMusicController` | `woweemusic` | schema 1, field `pendingUseOriginalSoundtrack = false` | n/a | off; fix the disagreement |
| 23 | MSAA 2x, FXAA, FSR 1/3, frame generation | `post_process_pipeline.cpp` | `antialiasing`, `fxaa`, `upscaling` ... | 2x, off, off | 4x | 2x (MSAA is a stock option) |
| 24 | Anisotropic filtering | `VkContext::getOrCreateSampler` | `texturefiltering` | 16x | n/a | 16x (stock option) |

Notes:

- **Classic does not cover everything.** `kGraphicsPresetKeys` lists 24 keys
  and leaves out `waterreflections`, `watersplashes`, `raytracedlighting`
  and `mistdensity`. A player who turned reflections on and then picks
  Classic keeps the reflections. Fix in the framework step (3.2).
- **4 to 7, the sky and air.** These add layers the stock sky never had.
  Volumetric mist in particular adds haze on top of Light.dbc's fog, which
  changes a zone's palette. `world-lighting.md` §9 already recommends sun
  shafts and sharp stars off. Proposal: off on a fresh install, on from High
  (shafts Low) and Ultra.
- **8 and 9, generated relief.** The 3.3.5 art is hand-painted with the light
  in the texture; generated normal maps emboss that paint a second time. By
  rule 4 this would default off. `graphics_presets.hpp` records that Jered
  prefers it on, so it stays on. Listed under "For Jered to decide".
- **13, splashes.** Wakes and spray at the swimmer are motion on existing
  surfaces and fit the rules. Midges over reeds add creatures that are not in
  the zone data. Split the row into "Splashes and wakes" (on) and "Insects
  over water" (off).
- **17, clutter.** Stock `groundEffectDensity` is 16 of 16-256. Check what
  100% here maps to, and make the default equal stock.
- **19, camera.** Stock smooths with `cameraTargetSmoothSpeed 90`,
  `cameraHeightSmoothSpeed 1.2`, `cameraGroundSmoothSpeed 7.5` and
  `cameraPivot 1` (`movement-camera.md` §10). Wowee's stiffness 30 and pivot
  1.6 are its own model. Confirm the defaults reproduce stock, or derive them
  from the stock CVars. Any extra smoothing goes under E20.
- **21, 22.** Interface and sound, not graphics, but they are non-stock and
  on by default. Both should be off on a fresh install.

### 2.2 Non-stock features with no toggle

These are on for every player and cannot be turned off from the settings.
Each needs a toggle (or removal) before phase 2 adds anything new, or
"off = stock" is not true.

| # | Feature | Where | What it does | Proposed toggle | Recommend |
|---|---|---|---|---|---|
| U1 | Foliage parts around the player and springs back | `m2.vert` (`playerWake`, `isFoliage`) | grass, ferns and bushes bend away from the player | `foliagepush` "Plants part as you pass" | on |
| U2 | Boats bob and rock on water | `m2.vert` (instance flag 1), `boatInstanceIndices_` | moored boats rise and fall | `boatbob` "Boats move on the water" | on |
| U3 | Point lights from M2 and WMO lights on terrain, WMO and M2 (64 lights) | `localLightContribution` in terrain/m2/wmo .frag, `gatherLocalLights` | pools of light at torches and lamps | `locallights` "Torch and lamp light" | on |
| U4 | Lamp flicker on those lights and on glow sprites | `lampFlicker` (`m2_renderer_internal.h`) | lights gutter | `lightflicker` "Flickering flames" | on (needs U3) |
| U5 | Characters lit by every gathered light, not the 4 nearest M2 lights | `character.frag`, `characters.md` L3 | units lit by non-stock lights | part of `locallights`; off = stock 4 nearest | follows U3 |
| U6 | Glow cards replaced by billboard sprites | `m2_glow_card.hpp`, `recordGlowSprites` | lamp halos and flame cards drawn as sprites | `glowsprites` "Soft lamp halos" | off until compared with stock cards |
| U7 | Synthesised smoke particles for models named "smoke" | `m2_renderer_particles.cpp` (`smokeParticles`) | extra smoke puffs | `extrasmoke` or remove | off; check whether stock emitters already draw this smoke (double smoke) |
| U8 | Mount dust | `mount_dust.cpp` (retired) | dust at a running mount's feet | folded into E21: `mountdust` is now "Dust kicked up" (`footfall_dust.cpp`) | on |
| U9 | Alpha-to-coverage on cutout foliage, cutoff 0.4/0.5 | `m2_renderer.cpp`, `character_renderer.cpp` | soft leaf edges under MSAA | `smoothleaves` "Smooth leaf edges"; off = stock ref 224/255 (`m2-runtime.md` #23) | on when MSAA is on |
| U10 | 9-tap rotated Vogel-disk shadow filter | `sampleShadowPCF` (four shaders) | softer shadow edges | part of E3; off = stock 5-tap PCF | on |
| U11 | Shoulder tone map | `postprocess.frag` (FSR 1 path only) | rolls off highlights above 0.9, on one path only | move under E18 or remove | off (stock) |
| U12 | Wowee drunk wobble and 9-tap blur | `fxaa.frag`, `camera_controller.cpp` | wavy, blurred view when drunk | fidelity fix: stock is the glow blend with z = drunk | stock |
| U13 | Terrain highlights always on | `terrain.vert` (`WOWEE_TERRAIN_SPECULAR` env only) | sun glint on `_s` tilesets | `terrainspecular` bound to cvar `specular` (stock option, stock default 0) | on; off in Classic |
| U14 | Coarse horizon tiles past 2 far clips | `terrain_horizon.cpp` | performance | none | keep (not visible) |
| U15 | LOD-3 bone skip, stepped emitters, weather particle cap | M2 renderer, `weather.cpp` | performance | none | keep, documented in the RE specs |
| U16 | WMO lamp glass, clock fire, lava scroll | `wmo_renderer.cpp` (`WOWEE_WMO_EXTRAS=1`) | already off | none | keep off |
| U17 | Storm lightning bolts | `lightning.cpp` (forced off in `Renderer`) | already off | none | keep off; do the stock dome flash tint first (fidelity) |

Believed stock and so not on this list, but worth one check against
Wow.exe: footprints (`footprint_renderer.cpp`, from FootprintTextures.dbc),
the M2 distance dither fade (`m2.frag`), the pressed-object highlight,
quest markers, loot sparkles (`Particles\LootFX.m2`), the level-up effect
(`Spells\LevelUp.m2`) and the underwater motes.

### 2.3 Gaps where "off" cannot be stock yet

| # | Toggle | Off today | Stock | Work |
|---|---|---|---|---|
| S1 | `shadows` | no shadow sampled at all | default cvars (`extShadowQuality 0`, `shadowLOD 1`) draw `ShadowBlob.blp` under each unit and no shadow map; MCSH baked shadows stay (`characters.md` §6) | implement the blob projector (terrain chunks and up-facing WMO triangles, ramps ShadowAdd/ShadowMod, model alpha). Then `shadows` off = blob + MCSH, which is the stock default |
| S2 | `foliagewind` = 0 | still trees and cloth, but U1 and U2 still move | nothing moves | U1 and U2 toggles |
| S3 | `locallights` (U3) | no toggle | no point lights on world geometry | U3 |
| S4 | Classic preset | 4 keys not set | everything stock | section 3.2 |

## 3. Framework (do first)

Adding one boolean today touches about nine places: the schema row, the
pending field, `kGraphicsPresetKeys`, `kGraphicsApplyKeys`, the preset
assign and match code in `settings_panel.cpp`, the binding table, the side
effect switch, three lists in `glue_options.cpp`, the save and load in
`game_screen_minimap.cpp`, the preset table and a renderer setter. That is
how Classic lost four keys. Phase 2 adds 20 more; fix the cause first.

### 3.1 One enhancement table

- A single `rendering::Enhancements` struct holds every enhancement value.
  Its default-constructed value is the stock picture: every bool false, every
  strength 0. The renderer reads it once a frame and pushes it to the passes.
- One table, `kEnhancementRows`, lists for each field: schema key, label,
  tooltip, kind, range, the stock value, the recommended value, and a member
  pointer. The schema rows, pending fields, apply list, save/load, the glue
  options page and the preset columns are generated from it.
- `GraphicsPresetValues` keeps its quality columns; the enhancement columns
  become one `Enhancements` value per preset. Classic is
  `Enhancements{}`, by construction.
- Saved keys stay as they are (`shoreline_foam` and the like), so no saved
  file changes meaning. New keys are added at the end; no stored index moves.

### 3.2 A page of its own

`test_settings_panel_layout` says Graphics, Detail and the "Grass" page are
full; the water and weather switches already live under "Grass" for lack of
room. Add an **Enhancements** category with these sections, and move the
existing rows into it (the keys do not change, only the category):

- Light and shadow: shadows quality (E3), local lights (U3), flicker (U4),
  contact shadows (E14), ambient occlusion (E13), indoor light (E12),
  terrain highlights (U13), normal maps and parallax.
- Sky and air: light shafts, mist, sun shafts, lens flare, sharp stars,
  highlight roll-off (E18).
- Water and weather: foam, wet ground, rain ripples (E5), snow cover (E6),
  splashes, insects, reflections, weather follows the wind (E19), caustics
  (E15).
- Motion: foliage wind, plants part (U1), boats (U2), capes and tails (E7),
  foot placement (E9), corpse settle (E8), animation smoothing (E10),
  smooth zoom (E20), dust (E21).
- Distance: horizon detail (E11), streaming fade-in (E2).
- Edges: soft particles (E4), smooth leaf edges (U9).

A "Reset to stock" button on the page sets `Enhancements{}`. The Classic
preset does the same plus the stock quality columns.

### 3.3 Proof that off is stock

- **Unit test** `test_enhancements_stock`: `Enhancements{}` equals the
  Classic row; every row's stock value equals the member's default; a
  `sizeof(Enhancements)` guard fails when a field is added without a row.
- **Shader rule.** Each enhancement in a shader is a specialization constant
  or a uniform-branch around a block that only adds to the stock result.
  With the constant off the code is the stock expression, unchanged in
  operation order. Reviewers check the diff for this.
- **Pixel test.** A new `WOWEE_FREEZE_CLOCK=<seconds>` holds the scene,
  wind, water and sky clocks. With it, a screenshot with every enhancement
  off must match the same build's Classic screenshot, and the pre-phase-2
  build's screenshot, at 0 differing pixels (MSAA and FXAA fixed). Places:
  Elwynn at noon, Stormwind Trade District, Ironforge, Nagrand, Dalaran,
  a rainy Grizzly Hills, Stranglethorn coast. Run after every enhancement
  commit; add it to the commit checklist next to `sweep_guard`.
- **A/B switch.** `WOWEE_ENHANCE_AB=<key>:<seconds>` toggles one row in
  place on a timer, as `WOWEE_TERRAIN_AB_PERIOD` and
  `WOWEE_WMO_PORTALS_ALTERNATE` do. Used for the screenshots and for the
  `WOWEE_FRAME_PROFILE=1` cost measurement on the same view.

### 3.4 Performance budget

Reference machine: the test machine's Intel Iris Xe (Mesa ANV), the
~1250x990 window used in `terrain.md` §13. Budgets below are GPU time on
that machine unless marked CPU. The recommended default set (everything
marked "on" in sections 2 and 4) must cost at most **3 ms GPU and 0.5 ms
main-thread CPU** together in Stormwind Trade District and in Nagrand,
measured with the A/B switch. Each enhancement states its own budget; one
that runs over is not committed as a default.

### 3.5 Measured cost (2026-11-03)

Measured as 3.4 says: the Iris Xe, a 1251x986 window, the Ultra quality
columns (2400 view distance, 500 yd shadows, 4x MSAA) with the enhancement
set on top, noon, the camera fixed. Each row flipped in place on a 5 s
period (`WOWEE_ENHANCE_AB`, `docs/performance.md` "Measuring an
enhancement's cost"), adjacent on/off windows compared. Places: Elwynn
(Goldshire, -9465 47), Stormwind (Trade District, -8829 640), Dalaran
(5804 624). GPU is the summed pass timestamps, CPU the main-thread stages
the renderer owns. Single-row differences under about 1 ms GPU and
0.25 ms CPU are inside the scatter between windows (rows that cannot cost
anything, such as wet ground in dry weather, read up to +1.8 ms) and are
given as "~0".

The recommended set against stock, the same run for each pair:

| | Elwynn GPU / CPU | Stormwind GPU / CPU | Dalaran GPU / CPU |
|---|---|---|---|
| Before (old set with shadows, old code paths) | +9.4 / +0.3 | +22.1 / +0.3 (frame 44.8 vs 32.3 ms) | +19 / ~0 (CPU-bound at 50 ms) |
| After, the same set (shadows on) | +2.9 / +0.7 | +6.5 / +1.3 | +6.5 / +1.5 |
| After, the recommended set (shadows off) | +1.3 / ~0 | +1.4 / ~0 | ~0 / ~0 |

"Before" re-enables the two old paths in the new build
(`WOWEE_LIGHT_TILES=0`, `WOWEE_SHADOW_CASTERS_WHEN_OFF=1`). Stock itself is
cheaper than it was: with shadows off the casters are no longer drawn, which
was 0.8 / 1.0 / 2.2 ms of CPU and 2.7 / 3.3 / 5.2 ms of GPU in the three
places.

Per row, after the changes below (GPU / CPU ms):

| Row | Elwynn | Stormwind | Dalaran | Before |
|---|---|---|---|---|
| Sun shadows | 2.8 / 0.7 | 4.3 / 1.0 | 4.0 / 1.5 | casters drawn when off too |
| Torch and lamp light | 1.9 / ~0 | ~0 / ~0 | ~0 / ~0 | 3.3, 21.1, 22.4 GPU |
| Light shafts and mist (Medium) | 0.9 (inject) / ~0 | 1.4 / ~0 | 1.7 / 0.4 | same |
| Surface depth (parallax) | 1.4 / 0.2 | ~1-3 / ~0 | 2.6 / 0.2 | same |
| Surface bumps | 1.1 / ~0 | ~0 / ~0 | ~0 / ~0 | same |
| Shadow edges (Soft) | ~0 | 1.2 / ~0 | ~0 | only with shadows |
| Every other row | ~0 | ~0 | ~0 | lens flare drew at zero intensity; mount dust, splashes and midges allocated at start |

What changed:

- **Torch and lamp light** was the whole of the gap: every world pixel
  walked all 48-64 gathered lights. The lights are now culled to a 32x18
  screen grid on the CPU each frame (`rendering/light_tiles.hpp`,
  `local_light_tiles.glsli`); a pixel walks its tile's mask, in the same
  order, and the grid only leaves out lights no pixel in the tile can be
  inside - the shaders' own radius test would have skipped them - so the
  sum is the same term for term (`test_light_tiles`). It is now cheaper
  than the stock rule, which still sorts every gathered light per pixel.
- **Shadows off** draws nothing: each frame slot's map is cleared and left
  readable once, then the pass records nothing.
- Lens flare at 0 draws nothing; mount dust, splashes and midges make
  their pipelines and buffers on first use and release them a few frames
  after they go off; the light gather keeps its candidate list between
  frames; the fog volume lookups multiply the position by the view then the
  projection rather than forming the product per pixel.
- **Sun shadows: recommended off.** At the Ultra shadow distance the map
  costs 2.8-4.3 ms of GPU and 0.7-1.5 ms of main-thread recording (the M2
  and unit casters), each over the whole set's budget on its own. The
  quality presets keep it on (`presetEnhancements`); Recommended is the
  stock blob. Bringing it back into the set needs the casters recorded off
  the main thread (or instanced units) and a static-caster cache. (The
  cache it was, 3.9: recommended on again.)

### 3.6 Measured cost of E4, E2, E5 (2026-11-03)

Measured as 3.5 (Iris Xe, 1251x986, Ultra quality columns, Recommended set,
noon, camera fixed, `WOWEE_ENHANCE_AB` on a 5 s period, adjacent windows
paired, `perf/parse2.py`). Elwynn (Goldshire) and Stormwind (Trade District)
dry; Elwynn again under server rain (`.wchange 1 1`, weather state 5;
Stormwind City has no weather, so its "rain" run was a second dry sample).
The machine was busy (load average 5-10 from builds on the same CPU, which
also feed the iGPU's power budget) and every frame here was CPU-bound
(21-38 ms), so the GPU column over-states as 3.5 warns. GPU / CPU ms:

| Row | Elwynn | Stormwind | Stormwind, 2nd | Elwynn in rain |
|---|---|---|---|---|
| Soft particles | +0.44 ±0.15 / ~0 (late pass itself 0.07) | +0.10 ±0.24 / ~0 (0.06) | +1.5 ±0.5 / ~0 (noisy window) | +0.47 ±0.6 / ~0 |
| Fade in new scenery | ~0 (+0.3 ±0.6) / ~0 | ~0 (+0.5 ±0.4) / ~0 | - | - |
| Rain ripples | ~0 (dry) | ~0 (dry) | ~0 (dry) | +0.42 ±0.54 / ~0 |
| Whole recommended set vs stock | +2.7 / ~0 | +1.8 / ~0 | +0.3 / +0.2 | +2.0 / ~0 |

The late particle pass's own timestamp ("soft particles" in the profile) is
0.06-0.07 ms in both places; the rest of the soft particle rows' GPU figure
is the scene pass moving by the same few tenths in both directions between
windows. Fade-in costs nothing while standing still (no chunk is stamped)
and one push or flag byte per fading chunk or instance while streaming.
Ripples cost nothing dry (`rtParams.z` and `push.rainRipple` are 0 and the
blocks are skipped). The whole recommended set stays inside the 3 ms GPU /
0.5 ms CPU budget of 3.4 in all four windows; its largest item is still the
light shafts' fog inject (0.85-1.0 ms), not these three.

### 3.7 Measured cost of the E4 order fix, E8, E21 and the E2 gaps (2026-11-03)

As 3.6 (Iris Xe, 1251x986, Ultra quality columns, Recommended set, noon,
camera fixed, 5 s period, adjacent windows paired, `perf/parse2.py`),
Goldshire and the Trade District, dry. The first run shared the CPU with
builds (load average 12-14 falling to 4.5) and its Elwynn windows for the
corpse and dust rows held a long hitch; those rows were run again at load
4.3. GPU / CPU (renderer stages) ms, paired median:

| Row | Elwynn | Stormwind |
|---|---|---|
| Soft particles (with the water now after the late pass) | +0.07 ±0.3 / +0.29; again -0.80 ±0.8 / +0.01 | +0.07 ±0.2 / +0.19 |
| Bodies lie along the ground | -0.23 ±0.1 / +0.11 | -0.56 ±0.5 / -0.16 |
| Dust kicked up | -0.05 ±0.2 / +0.03 | +0.05 ±0.2 / -0.03 |
| Fade in new scenery (now with clutter and objects) | -0.50 ±1.1 / -0.07 | -0.22 ±0.3 / +0.07 |
| Whole recommended set vs stock | +2.07 / +0.26 | +1.48 / +0.25 |

The water pass itself reads 0.13-0.22 ms in the soft particle windows (it
was inside the scene pass's figure before), and the late pass 0.08; the
two together are a wash against the scene pass, as the GPU column says.
The CPU side of the move is the extra pass begin and the frame's view and
framebuffer, within the scatter. Corpses and dust cost nothing standing
still: no corpse is measured and no footfall is near the camera on dirt.
Moving, the dust is a queue entry per footfall in view, three height
queries per walker every 2 yd, and at most 512 sprites rebuilt on the CPU
(the probe's run peaked at 12); a corpse is five floor queries once. The
whole recommended set stays within the 3 ms GPU / 0.5 ms CPU of 3.4 in both
places; its largest item is still the light shafts' fog inject (0.95 ms).

### 3.8 Measured cost of E7 and E10 (2026-11-03)

As 3.7 (Iris Xe, Ultra quality columns, Recommended set, noon, camera fixed,
5 s period, adjacent windows paired, `perf/parse2.py`), Goldshire and the
Trade District (Stormwind's guards and citizens carry cloaks and tabards).
Two runs; the machine was busy both times (load average 13-22 in the first,
from another agent's LTO build, 6-9 in the second) and the compositor put
the second run's window at 621x986. The rows do no GPU work (the bones are
uploaded every frame either way), so their GPU column is the scatter; the
CPU they cost runs in `update/characters` (the springs on the animation
workers, the smoothing in the instance loop), measured by itself as well.
GPU / CPU (renderer stages) ms, paired median, and `update/characters`:

| Row | Elwynn | Stormwind | `update/characters` |
|---|---|---|---|
| Capes, tails and hair (Off vs all) | +0.36 ±0.7 / +0.02; +0.62 ±0.5 / +0.07 | -0.09 ±3.6 / +0.07; (second window disturbed, 78 ms frames) | +0.006 to +0.024 |
| Smoother other players | +1.12 ±0.8 / +0.02; -0.23 ±0.7 / +0.11 | -0.05 ±0.5 / +0.04; +0.89 ±0.8 / -0.13 | -0.012 to +0.010 |
| Whole recommended set vs stock | (first window held a hitch); +6.1 / +0.57 | +3.14 / +0.62; +7.6 / +0.37 | +0.04 to +0.08 |

The two new rows cost at most a few hundredths of a millisecond of CPU and
nothing on the GPU. The whole set read over the 3 ms / 0.5 ms budget in
these runs, but its excess is the light shafts' fog (inject and integrate,
+1.2 to +7.3 ms here against 0.95 in 3.7) and the scene pass, under the
load and at the odd window size, not these rows; it wants measuring again
on a quiet machine before anything else is added to the set.

### 3.9 Measured cost of the shadow split and the cheaper mist (2026-11-03)

E3 as built (the kept world map and the per-frame unit map,
`performance.md` "The sun's shadow in two maps") and the light shafts made
smaller: Medium is 128x72x48 cells rather than 160x90x64, and each frame
lights only every other slice, the rest carried from the last frame
reprojected (`WOWEE_FOG_HALF_RATE=0` lights them all). Measured as 3.5 (Iris
Xe, Ultra quality columns, noon, camera fixed, 5 s windows, adjacent
windows paired, `perf/parse2.py` less the idle windows): the sun shadows
row is flipped in place over the Recommended set; "Whole set" is
Recommended (now with shadows) against stock. The new build was measured
both ways in one sitting, its one-map path (`WOWEE_SHADOW_SPLIT=0`, the old
casters and the old map, with the new mist) right after the split, on a
quiet machine (load 4-5, no other client, 1251x986 throughout for the
one-map run; the split run had a second client for its first minutes and
the compositor halved the window for some windows, marked). The old build
(before this work) is given from the same session where it ran at the same
size. GPU / CPU (renderer stages) ms, paired median:

| Row | Elwynn | Stormwind | Dalaran |
|---|---|---|---|
| Sun shadows, one map, every frame | +1.71 / +0.45 | +3.07 / +0.97 | +2.78 / +1.50 |
| Sun shadows, split | +1.03 / +0.22 (half width) | +1.98 / +0.24; +1.20 / +0.24 with the last change* | +0.61 / ~0 |
| Light shafts (Medium), old build | +0.7 to +2.1 (inject 0.9-4.9) | +9.7 to +14.2 (inject 5.9-13.8, integrate to 2.5) | +4.1 to +5.1 (inject 3.8-5.4) |
| Light shafts (Medium), now | ~0 to +0.98 (inject 0.62-0.64) | +1.36 to +1.40 (inject 0.71-0.81) | +1.50 to +2.44 (inject 0.67-0.91) |
| Whole set, old build with shadows (all+shadows) | +7.9 / +1.84 | +9.6 / +1.57 (half width) | +28.5 / +2.62 (half width; inject 23.8) |
| Whole set, one-map shadows, new mist | +7.3 / +1.40 | +7.2 / +1.84 | +5.5 / +2.86 |
| Whole set, split | +2.1 to +2.8 / +0.37 to +0.45 (half width) | +4.96 / +0.79; +2.96 ±0.10 / +0.50 with the last change* | +1.3 to +1.6 / +0.20 to +0.28 (half width) |

\* The last change skips the unit map where the world map has already put
a pixel wholly in shade (most of a city street at noon); measured again in
Stormwind only, 15 pairs, load 5-8, full width throughout.

What the split moved: the shadow pass's GPU time fell from 1.5-3.7 ms to
0.05-0.1 (only units are drawn each frame), its main-thread recording from
0.6-2.7 ms to 0.2-0.4 (the 64 nearest units; the M2 and terrain casters are
recorded only on a redraw, a third of them a frame). The second filter
round units costs 0.2-1.2 ms of the scene pass in its place (the scene
pass's share of the row, against the one map's), most in Stormwind, where
units stand thick on lit ground near the camera. Walking (Goldshire, up and
down the road) redraws the world map every few seconds and costs +1.05 /
+0.18 (half width) against +3.42 / +1.41 for the one map every frame.
The mist no longer varies with the machine's load as it did (inject 0.6-0.8
ms in every window, against 0.9-23.8 before): the old Medium volume's
inject was memory-bound enough that a busy CPU sharing the bus put
anything from one to twenty milliseconds on it.

The whole set with shadows is inside the 3 ms GPU / 0.5 ms CPU budget in
all three places: Elwynn and Dalaran at half width, Stormwind's Trade
District at full width at the limit (+2.96 / +0.50), where it was +5.0 /
+0.8 before the skip above. Sun shadows are recommended on, Soft edges at
five taps. Gaps: Elwynn and Dalaran were not measured again at full width
after the last change; the unit map's second filter costs the scene pass
+0.3 in Stormwind after the skip (the one map's scene share +0.82 against
+1.14); units past the 64
nearest or the unit map's reach have no shadow at all; a city with many
lights still lights the mist per light per cell.

### 3.10 Measured cost of E9, E11, E19 and E20 (2026-11-03)

As 3.5 (Iris Xe, 1251x986, Ultra quality columns, Recommended set, noon,
camera fixed, 5 s windows, adjacent windows paired, `perf/parse2.py`, only
full-width windows), two runs at load 4-8 with no other client: Goldshire,
the Trade District, Dalaran, a Dragonblight ridge looking over the far
horizon (4800, -733, facing Icecrown and the WDL cathedral) and Goldshire
under server rain for the weather row. Dalaran's windows went from about
25 ms to 70-120 ms partway through both runs in both states alike (the GPU
scene pass, not anything flipped); its pairs are given but are scatter.
GPU / CPU (renderer stages) ms, paired median (± the GPU pairs' standard
error):

| Row | Elwynn | Stormwind | Northrend (ridge; Dalaran) | Rain |
|---|---|---|---|---|
| Feet on the ground | -0.11 ±0.07 / +0.08 | -0.56 ±0.37 / +0.05; -1.12 ±0.38 / -0.20 | -0.36 ±4.6 / -0.66; Dalaran +0.39 ±0.67 / +0.04 | - |
| Lit far horizon | -0.07 ±0.12 / +0.02 | +0.49 ±0.48 / -0.02; +0.04 ±0.28 / +0.08 | -0.11 ±1.38 / -0.12; Dalaran +1.75 ±8.1 (scatter) | - |
| Rain and snow in the wind | ~0 (dry: nothing added) | - | - | +1.15 ±1.11 / +0.06 |
| Smooth zoom | +2.09 ±1.40 / -0.17 (three pairs; the camera did not move: scatter) | - | - | - |
| Whole recommended set vs stock | +5.15 / +0.63 | +4.04 / +0.88; +3.92 / +0.85 | +2.79 / +0.20; Dalaran +1.16 / +1.43 | +5.12 / +0.60 |

The four rows cost nothing measurable: every pair is inside its scatter,
and the CPU they add is at most a few hundredths. Feet on the ground reads
the floor only for units within 30 yd whose feet have moved 0.12 yd since the
last read, and solves two legs per such unit on the animation workers; the
lit horizon is the same draws with one more 4-byte stream and a dozen ALU
operations a pixel, on a pass that covers the strip past the far clip; the
wind is a sine and two multiply-adds per drop while it rains; the zoom a
few operations a frame. All four stay recommended on.

The whole set is over the 3 ms / 0.5 ms of 3.4 in these runs (Stormwind
+3.9 to +4.0 / +0.85, against +2.96 / +0.50 in 3.9), but the excess is not
these rows (their sum is about zero): it is the scene pass and the shadow
stages (`shadow_pass`, `shadow/characters`, `beginFrame/graph`) and the
mist's inject, i.e. the sun shadows and light shafts measured again in a
different sitting. The working tree also carries another change in
progress that draws the stock blob for the units outside the sun shadow
map with shadows on (`Renderer::updateUnitBlobs`), which moves the set's
CPU; the set wants measuring again once that lands, before anything else
is added.

## 4. Proposed enhancements

Ranked by feel gained over cost and risk. "Cost" is implementation effort
(S under a day, M one to three days, L more). "Risk" is the chance it breaks
the classic look or the stock behaviour.

| Rank | Id | Name | Feel | Cost | Risk | Default |
|---|---|---|---|---|---|---|
| 1 | E1 | Stock blob, then shadow-map units (S1) | high | M | low | map on |
| 2 | E4 | Soft particles | high | S | low | on |
| 3 | E2 | Streaming fade-in | high | S | low | on |
| 4 | E3 | Shadow quality: cascades and soft edges | high | M | low | on |
| 5 | E5 | Rain ripples on water and puddles | medium | S | low | on |
| 6 | E7 | Capes, tails and hair: secondary motion | high | L | medium | on |
| 7 | E8 | Corpse settles to the ground slope | medium | S | low | on |
| 8 | E12 | Indoor light for units and doodads | high | M | medium | on |
| 9 | E10 | Animation smoothing for other units | medium | S | medium | on |
| 10 | E9 | Foot placement on slopes | medium | M | medium | on |
| 11 | E11 | Horizon detail (lit WDL, far WMOs) | high | M | medium | on |
| 12 | E6 | Snow cover while it snows | medium | S | low | on |
| 13 | E21 | Dust and snow kicked up | medium | S | low | on |
| 14 | E14 | Contact shadows | medium | S | low | on |
| 15 | E19 | Weather follows the wind | low | S | low | on |
| 16 | E20 | Smooth zoom and collision ease | low | S | low | on (zoom only) |
| 17 | E13 | Ambient occlusion (ambient term only) | medium | M | high | off |
| 18 | E15 | Caustics in shallow water | low | M | high | off |
| 19 | E16 | Cliff texture stretch fix | medium | M | high | off |
| 20 | E18 | Highlight roll-off (HDR scene, faithful tone map) | low | M | high | off |
| 21 | E17 | Upscaled texture pack | varies | M | high | off |
| 22 | E22 | Ambient life from stock assets | low | M | high | off |

Each entry below gives what it looks like, the engine area, how off stays
stock, the budget, and how to verify.

### E1. Stock unit blob, and shadow-map units as the enhancement

- **Looks like.** Off: the stock round blob under every unit, as a default
  3.3.5a client draws. On: units cast real shadow-map shadows (today's
  behaviour).
- **Engine.** New `UnitBlobShadow` in `overlay_system.cpp`, next to the
  selection circle, which already uses the stock blob projector. Projects
  `Textures\ShadowBlob.blp` onto terrain chunks and up-facing WMO triangles
  in the model's box, ramps ShadowAdd/ShadowMod (64x8), alpha = model alpha
  (`characters.md` §6). Character casters leave the shadow pass when off.
- **Off = stock.** `unitshadows` off draws the blob and removes units from
  the shadow map. `shadows` off removes the world shadow map too, which
  leaves MCSH and the blob: the stock default.
- **Budget.** Blob: one small projected quad set per unit, under 0.2 ms for
  200 units. On is today's cost.
- **Verify.** Stealthed rogue: blob at 0.3 alpha. Unit on a WMO bridge:
  blob on the deck, not on the water below. Pixel test with both off.

### E2. Streaming fade-in

- **Looks like.** Terrain tiles, WMOs and doodads that finish loading while
  in view fade in over about 0.4 s with the existing 4x4 dither, instead of
  appearing in one frame. Most visible on a fast flying mount and after a
  teleport.
- **Engine.** `TerrainManager::processReadyTiles` stamps a load time per
  tile and per WMO/M2 instance. `m2.frag` already has the dither
  (`bayerDither4x4`, `vFadeAlpha`); add the same to `terrain.frag` and
  `wmo.frag`, multiplied by `min(1, (now - loaded) / 0.4)`.
- **Off = stock.** Load-time factor forced to 1; the dither branch is the
  existing distance fade, unchanged.
- **Budget.** One multiply per instance on the CPU, one compare per pixel on
  new geometry only. Under 0.1 ms.
- **Verify.** Fly Dalaran to Crystalsong at 280%: record with A/B. Count
  frames where a tile appears whole (should be 0 with it on). Pixel test.
- **Status (2026-11-03): done, row `streamfadein` "Fade in new scenery",
  recommended on.** As planned, with these differences. The stamp is per
  terrain *chunk* (`TerrainRenderer::loadTerrainIncremental` uploads 16 a
  call, so a tile already arrived piecewise), per WMO instance and per
  doodad and WMO doodad the streamer places (`markStreamedIn`; a placement
  that matched an instance already standing gets its id back and is not
  stamped). The clock is drawn time with each step held to 0.1 s
  (`Renderer::streamFadeClock_`): a far teleport's reload draws nothing for
  seconds, and a wall clock had every tile "faded" before it was first
  seen; now they dissolve in over the frames after the hitch. Shaders:
  `stream_fade.glsli` (the C++ twin `rendering/stream_fade.hpp`); M2 carries
  the hidden fraction in instance flags bits 24-31 (`vStreamHide`), WMO in
  `push.frame.y`, both a branch on a flat value that is 0 with the row off,
  placed after every texture read. Terrain has no discard in its stock
  shader (which keeps early depth), so a fading chunk binds a second
  pipeline built from `terrain.frag` with `-DSTREAM_FADE`
  (`terrain_fade.frag.spv`), made when the row first goes on and released
  when it goes off; its push range grew by a vec4 for the fraction, which
  the stock shaders do not read. `WOWEE_STREAM_FADE_SECONDS=<s>` stretches
  the dissolve for a look. Cost: 3.6, ~0. Since (2026-11-03, second pass):
  ground clutter is stamped as each chunk's tufts are made
  (`TerrainManager::streamGroundClutter`; the near sweep after an arrival
  fills in round the player in view, the far one past the clutter distance
  where nothing sees it) and so are server game objects as they come into
  range - M2 objects and WMO objects, both the cached and the incremental
  upload path (`entity_spawner_player.cpp`, `entity_spawner_processing.cpp`):
  a chest, a mailbox or a ship now dissolves in like the scenery. Units
  already had their own 0.5 s opacity fade-in. Cost: the same flag byte per
  fading instance; 3.7. Gap: a whole-world reload dissolves the terrain
  against the sky and horizon, which reads as a 0.4 s sparkle - it could be
  held back until the terrain under the camera is in.

### E3. Shadow quality: cascades and soft edges

- **Looks like.** Sharp shadows near the player and shadows that reach
  further, with edges that soften with distance from the caster (a tree's
  trunk shadow is crisp at the base and soft at the crown).
- **Engine.** `renderShadowPass` and `shadow_params.cpp`: two or three
  cascades instead of one map; `sampleShadowPCF` in the four .frag files
  (keep them in one `shadow_filter.glsli`, which also fixes the "repeated in
  four shaders" note). Contact hardening: a 16-tap blocker search on the
  nearest cascade only.
- **Off = stock.** The stock shadow programs use a 5-tap PCF on one map
  (`world-lighting.md` §3), with the 0.7 floor. Off selects one map and a
  5-tap kernel. U10 (today's 9-tap Vogel disk) becomes the middle setting.
  Setting: Stock / Soft / Soft + cascades.
- **Budget.** Second cascade: the shadow pass is 4.1 ms CPU today
  (`performance.md`), so cascades must come with per-model instanced casters
  (the fix that file names). Target +0.8 ms GPU, +0 CPU after instancing.
- **Verify.** Elwynn tree line at noon and at dusk, Stormwind gate: A/B
  shots; shadow acne and peter-panning checks at the cascade seam. Profile
  `shadow/*` lines.
- **Status (2026-11-03): done as two maps, not distance cascades; sun
  shadows recommended on again.** What cost the time was not the map's
  resolution but drawing every caster into it every frame, so the split is
  by what moves rather than by distance (`rendering/shadow_cache.hpp`,
  `performance.md` "The sun's shadow in two maps"):
  - the *world map* (terrain, buildings, doodads that stand still) is kept
    and drawn again only when the player has walked 5% of the shadow
    distance, the sun has turned a quarter of a degree or the world has
    changed (at most every 2 s), a part a frame into a second image that
    then swaps in; its texels are snapped, so static shadows never crawl;
  - the *unit map* (the 64 nearest units, and doodads and transports that
    moved lately) is drawn every frame at half the world map's side over a
    fifth of the shadow distance (32-90 yd), so a unit's shadow is two to
    three times sharper than before; a 64x64 mask of the cells a caster can
    shade keeps the second lookup to the ground round units.
  The four world fragment shaders share `shadow_filter.glsli` (the "repeated
  in four shaders" note is gone). Soft is now five taps on the turning disk,
  as many as the stock filter; the unit map always takes the stock five.
  Off is unchanged: shadows off records nothing and draws the stock blob,
  Original is the stock 5-tap kernel. Not done: contact-hardening (blocker
  search) and distance cascades for the world map - with it kept, a finer
  near cascade would be the next step for sharper static shadows near the
  player, at the cost of drawing what is near every frame again. Units
  beyond the unit map's reach, or past the 64 nearest in a crowd, cast no
  map shadow (and get no blob). Swaying trees cast still shadows. Cost:
  3.9.

### E4. Soft particles

- **Looks like.** Fire, smoke, spell clouds, dust and waterfall spray no
  longer cut a hard line where their quads meet the ground or a wall.
  Campfires sit in the ground instead of on a visible plane.
- **Engine.** `m2_particle_gpu.frag` and the weather splash shader
  (ribbons are thin strips and do not need it). Fade alpha (or colour, for additive and Mod blends) by
  `sat((sceneDepth - particleDepth) / k)`, k about 0.5 yd, scaled by
  particle size. The scene depth copy exists in `water_renderer.cpp`
  (taken only while foam is on); move it to a shared resource taken once
  per frame when either needs it. MSAA depth: use sample 0 or a resolved
  min depth.
- **Off = stock.** Specialization constant off: no depth read, the stock
  blend (`particles.md` §6 #34-#36) untouched. No depth copy when neither
  foam nor soft particles want it.
- **Budget.** One depth fetch per particle pixel; 0.3 ms in a busy raid
  fight. The copy is ~0.1 ms at this size.
- **Verify.** Campfire in Goldshire, Blizzard on flat ground, Hellfire
  Peninsula smoke columns: A/B shots. Mod2x particles must still fold to
  grey, not to black, at the fade.
- **Status (2026-11-03): done, row `softparticles` "Soft particles",
  recommended on.** Built differently from the plan: no depth copy. With
  the row on, the scene pass draws only the particles that write depth
  (blend 0 and 1) and uploads the glow halos without drawing them; the
  blended runs and the halos are drawn in a *late particle pass*
  (`rendering/soft_particle_pass.*`): single sample, colour only (the
  overlay pass's attachment), recorded on the resolved scene colour as soon
  as the scene pass closes (`Renderer::recordSoftParticlePass`, from the
  water continuation, the post-process `sceneClosedHook`, or `endFrame`),
  with the scene's one-sample depth (its own without MSAA, its resolve with
  it) moved to `DEPTH_STENCIL_READ_ONLY_OPTIMAL` and bound as a texture,
  then put back. The shaders (the `-DSOFT_PARTICLES` builds of
  `m2_particle_gpu.*` and `m2_particle.*`, sharing `soft_particle.glsli`;
  the stock builds are byte-identical to before) do the stock
  LESS_OR_EQUAL test themselves and fade over the last half-size of the
  quad in front of the scene (0.15-1.5 yd); flat world quads (XY quads:
  ground glows) keep the hard test. Each blend fades the way fog does:
  alpha for 2 and 4, colour to black for 3, to white for 5, to 0x80 grey
  for 6. A device with MSAA and no depth resolve has no one-sample depth
  and keeps the stock path. Cost: 3.6, about 0.1 ms (the pass itself
  0.07). Ordering while on (fixed 2026-11-03, second pass): the late pass
  draws after the scene pass, and the water, the weather, the spray and the
  minimap always come after it (`water_pass_order.hpp`), so a particle under
  a water surface is seen through the water and the rain falls over the
  fire, as stock draws particles, water, then weather. In the default
  configuration (MSAA into the glow/FXAA target) that used to leave the
  water in the scene pass, under the particles; there the water now leaves
  the scene pass for the single-sample water pass, drawn over the resolved
  scene colour and depth (`sceneWater1xFramebuffer`, the depth-keeping
  sibling of the 1x water pass), made on demand the first time it is wanted
  - a session that started with MSAA never had one. The water's refraction
  copy is then taken before the water, as in the other configurations,
  instead of from the finished frame. Off, the water stays where it was in
  every configuration (`test_phase2_enhancements` holds the old rule). The
  minimap follows the water; a minimap, spray or weather made after the
  pass decision is rebuilt for its pass before it is drawn there. The late
  pass and the water pass own their per-frame views and framebuffers per
  frame slot: a preview renderer shut down while the UI draws flushes
  VkContext's deferred queue mid-frame, which destroyed them while the frame
  still recorded. Remaining differences
  while on: the water's shoreline edge is drawn single-sampled (as in the
  MSAA-to-swapchain configuration), the brightness overlay scales neither
  the particles nor the water (it is drawn in the scene pass; at the
  default brightness of 1 it draws nothing), and the mount dust and
  footprints stay in the scene pass, under the particles. Gaps: the stock
  glow cards (M2 batches, U6 off) and ribbons are not softened;
  `extrasmoke` stays hard.

### E5. Rain ripples on water and puddles

- **Looks like.** While it rains, small rings form and fade on lakes,
  rivers, and on wet ground (2.1 #11) where it is flat and faces up.
  Intensity follows the server's rain grade.
- **Engine.** `water.frag`: a tiling animated ripple normal (procedural, no
  new texture) added to the stock surface normal, weighted by the rain level
  already eased for wet ground. `wet_surface.glsli`: the same on surfaces
  where `normal.z > 0.9` and the wet factor is high. Indoors and under
  roofs: the same shadow/indoor mask the wet ground uses.
- **Off = stock.** Weight 0 skips the block; stock water has no ripples
  from rain (stock `waterRipples` decals are unit wakes and default off).
- **Budget.** Two texture-free noise evaluations per water pixel in rain
  only; under 0.2 ms.
- **Verify.** Grizzly Hills or Elwynn in server rain, a lake and a road.
  Check the stock water ramp colour is unchanged at the ripple's mean.
- **Status (2026-11-03): done, row `rainripples` "Rain ripples",
  recommended on.** Rings, not a wave normal: `rain_ripples.glsli` cuts the
  surface into cells (two layers, 0.6 and 0.9 yd), each takes at most one
  drop a cycle at its own spot and moment, and the ring's height
  (sin(pi x) e^-x^2, fading as it spreads) gives a slope that catches the
  light toward the eye on one side and away on the other by the same
  amount, so the stock ramp colour is the ripple's mean. Drawn out to about
  30 yd on water (`water.frag`, `push.rainRipple`, never on interior WMO
  water) and 28 yd on ground (`wet_surface.glsli` `wetGroundRipples`: only
  where the ground is level, soaked through and in the hollows a slow noise
  picks out; exterior WMO groups only, as the wet ground). The strength
  follows the server's rain and storm grade, eased over about a second
  (`rendering/rain_ripples.hpp`, `rtParams.z`), and is also each cell's
  chance of a drop, so a drizzle rings sparsely. Ring width widens with
  the pixel footprint so far rings blur instead of shimmering. Verified in
  server rain in Goldshire (puddles on the grass) and Booty Bay harbour
  (Stormwind City has no weather to switch on). Cost: 3.6, ~0 dry, about
  0.4 ms on a water-heavy view in rain. Gap: no splash particles where
  drops land - the rings only.

### E6. Snow cover while it snows

- **Looks like.** During snowfall outdoors, the tops of terrain, rocks,
  roofs and branches slowly whiten (minutes), and lose it slowly after.
  Never indoors, never on already snowy tilesets.
- **Engine.** The wet ground path (`wet_surface.glsli`, `Renderer` wet
  easing): a second factor `snowCover` with a slower time constant, blended
  toward a white from the zone's own fog/ambient colour (so it takes the
  zone's light), masked by `normal.z` and the sun-facing test the wet ground
  uses.
- **Off = stock.** Factor held at 0; the block is skipped.
- **Budget.** Same as wet ground: under 0.1 ms.
- **Verify.** Dun Morogh edges and Grizzly Hills in server snow. Make sure
  Winterspring and Storm Peaks do not change (already white).

### E7. Capes, tails and hair: secondary motion

- **Looks like.** A cape trails and swings when the character runs, turns,
  jumps and lands, and settles when standing; tauren and draenei tails and
  long hair where the rig has free bones follow with a lag. Mounted capes
  stream. The authored run animation still drives the base pose.
- **Engine.** `CharacterRenderer::calculateBoneMatrices`: after the stock
  pose, a spring-damper chain on listed bones, driven by the unit's world
  acceleration and turn rate, with per-bone angle limits and a capsule
  around the spine and legs to stop clipping. A generated table of chains
  per model file (cape bones, tail bones, hair bones), produced by a
  one-time audit tool over the character and creature models, like the
  creature skin audit. One system for all three, not three systems.
- **Off = stock.** The spring step is skipped; bones are the sampled
  animation exactly. Toggle values: Off / Capes / Capes, tails and hair.
- **Budget.** CPU, on the animation workers: 12 bones per unit, units
  within 40 yd only, 0.1 ms for 50 units. Skipped for culled units (the
  existing anim-cull).
- **Risk.** Clipping through the body and through mounts; broken rigs on
  odd models. Ship capes first, tails and hair after the audit.
- **Verify.** Run, strafe, turn on the spot, jump, mount and dismount, sit;
  every race and gender with a long cape; a tauren tail. Screenshot series.
  With it off, bone matrices compare equal to a build without the code
  (test: same sampled pose in, same matrices out).
- **Status (2026-11-03): done, row `secondarymotion` "Capes, tails and
  hair" (Off / Capes and robes / Capes, robes, tails and hair), recommended
  "Capes, robes, tails and hair", stock Off.** One system:
  `rendering/secondary_motion.hpp` (pure, `test_secondary_motion`) and
  `CharacterRenderer::analyzeSpringChains` / `stepSprings`, applied in
  `calculateBoneMatrices` (on the animation workers) after the stock pose,
  the blend and the look twist: each chosen bone gets an extra rotation about
  its own pivot, its model-space axis taken into the parent's frame. No
  table, no audit tool: the bones are chosen from the model itself when the
  row first goes on (the rule below), and the survey of all 972 character
  and creature models in 3.3.5a that settled the rule found 68 cloth, 27
  hair and 135 tail chains (96 parent-to-child, 39 flat fans).
  - **The rule.** A bone is *free* when it is not a key bone, has no key
    bone below it and has vertices. A *chain* is a run of free bones, each
    the one free child of the last, from where the free tree starts or
    branches to a leaf, 2-12 bones; a *fan* is 3-12 free leaf siblings on
    the midline behind their parent (the wolf's and the great dragons' tail
    bones all hang off the pelvis). Cloth: >= 60% of the chain's vertex
    weight in the cloak (15xx), tabard (12xx) or robe (13xx) geosets and
    <= 10% in the body (geoset 0) - on HumanMale the back flap 5-12 the
    cloak and tabard hang from and the front flap 6-13-19. Hair: below the
    Head key bone, >= 60% in hair or facial hair geosets (1-399) -
    ponytails, the dwarf's and tauren's braids. Tail: nearest key ancestor
    the Waist (key bone 5), not below the Head, >= 60% body weight, pointing
    back (|dx| >= |dz|/2, |dy| <= |dx|/3), starting within a tenth of the
    model's height of the midline, at least 8% of it long and, unless a fan,
    above 12% of it (which leaves the legs out). Wings hang off the chest
    and spread sideways and never pass; the mechanical models (catapults,
    gyrocopters) have no Waist. Found that way: draenei tails, cats, wolves,
    raptors, wyverns, gryphons, kodos, drakes and the great dragons; the
    tauren tail rides the cloak chain (it is skinned to the back flap).
    At most 32 bones a model.
  - **The spring.** Per bone a two-way swing (out from the body - for a
    tail, up - and sideways), a damped spring back to the authored pose
    whose steady answer to a push is shared along the chain, the tip softer
    than the root (cloth 2.4 -> 1.5 Hz, hair 3 -> 2, tail 2 -> 1.2, slower
    with the square root of the length past 1.5 yd). The push is the
    pseudo-force on a probe point out behind the root as the model moves
    (minus its acceleration, from the drawn model matrix, so mounts, seats
    and E10's glide all count) plus air drag against its velocity relative
    to the wind - the foliage wind's direction and eased outdoor strength,
    5 yd/s at 1, with the foliage gust and a flutter; with foliage wind off
    there is none. Hanging kinds also take half the vertical push: a fall
    floats a cloak out, a landing drops it. Hard limits per chain: cloth 29
    degrees out, 4 toward the legs, 14 aside; hair 15 / 3 / 10; tail 12 up,
    10 down, 20 aside. Semi-implicit Euler in 1/120 s substeps, at most
    50 ms simulated a frame; a frame over 250 ms, a probe jump over 4 yd or
    a non-finite value starts the chain over from rest.
  - **Looks like.** A cloak lifts a few degrees more behind a runner, swings
    out when setting off, aside when turning, floats on a fall, drops on
    landing and settles in about a second when standing; standing in the
    wind it stirs by a degree or few. Tails and braids follow with a lag.
    The authored swing is all still there; the spring rides on top. In the
    Goldshire probe (human male, cloak 3511, `WOWEE_SPRING_LOG=1` logs the
    player's chains) the cloak chain reached at most 25 degrees out and 12
    aside over runs, turns, stops and jumps, a degree or two standing in the
    breeze.
  - **Cost.** Only units drawn this frame (the anim cull) within 45 yd,
    full to 30 yd and fading out to 45; per bone a mat3 inverse and a
    quaternion; on the animation workers. 3.8: about 0.02 ms of CPU, no GPU.
  - **Off = stock.** `springLevel_` 0: `calculateBoneMatrices` compares one
    int and never looks at a spring slot; no model is searched; turning the
    row off clears every state.
  - **Gaps.** No collision capsule: the toward-the-legs limit is the only
    guard against a cloak entering the body, so a strong turn can still put
    a cloak edge into a leg for a frame, as the authored animation already
    does. Chains are found per model, not per item: a cloak geoset that is
    hidden still has its (invisible) chain stepped. Long hair that is
    skinned to the Head bone alone (most styles) has no chain and does not
    move. Robes (13xx) follow the legs and get only the front and back
    flap motion. The verification so far is the player (human male, cloak
    3511) and a raptor mount in Goldshire; other races and the great
    dragons were checked by the chain survey, not on screen.

### E8. Corpse settles to the ground slope

- **Looks like.** A dead unit on a hillside lies along the slope instead of
  being drawn level with half its body in the air or in the hill. No
  ragdoll: the death animation plays as authored, then the model tilts to
  the ground normal over 0.3 s.
- **Engine.** At the end of the death animation (`death-ghost-loot.md` §4,
  the corpse state), sample terrain/WMO floor height at four points of the
  model's footprint (`TerrainManager::getHeightAt`,
  `WMORenderer::getFloorHeight`), fit a plane, clamp the tilt to 25
  degrees, apply as a root rotation in `CharacterRenderer`.
- **Off = stock.** No tilt; root transform unchanged.
- **Budget.** Four height queries once per death; nothing per frame.
- **Verify.** Kill mobs on Thousand Needles slopes, stairs in Stormwind,
  a bridge. Loot and the corpse sparkle still sit on the body.
- **Status (2026-11-03): done, row `corpsesettle` "Bodies lie along the
  ground", recommended on.** `rendering/corpse_settle.hpp` (pure, tested)
  and `CharacterRenderer::updateCorpseSettle`, run by the renderer on the
  main thread after the M2 update has joined. Once a dead unit's Death
  animation has 0.3 s left (or at once for a unit with only the looping Dead
  pose), the floor is read five times - the centre and the two ends of the
  body's footprint along and across its facing, half extent 0.45 x its
  standing height held to 0.4-4 yd - as the selection circle reads it (the
  highest terrain, WMO or M2 floor up to 1.5 yd above the body and 4 below),
  a plane is fitted (a missing end uses the centre; no floor anywhere is
  level), held to 25 degrees, and the tilt eases in over those 0.3 s so the
  body lands on the slope. A corpse that comes into view lying there (or is
  there when the row goes on) is drawn settled at once; one that moves more
  than 0.25 yd is measured again; a rising unit eases back up. The tilt is
  a rotation about the body's position on the ground, applied in
  `getModelMatrix` only when non-zero, so everything that hangs off the
  model (weapons, emitters, the shadow caster) follows. Level ground gives
  exactly the stock matrix. Off: no query, no tilt, and the per-frame loop
  is skipped once nothing is tilted. Verified with the player's own corpse
  on the 20 degree road bank at (-9098, -185) in Elwynn: off, the legs stick
  up out of the bank on one side and the head is buried on the other; on,
  the body lies along it. Cost: 3.7, ~0. Gaps: the
  footprint is a cross, not the body's actual outline, so a body lying
  diagonally over a ridge takes the mean slope; stairs give the stair's
  mean slope (no per-limb fit; that is E9's foot IK).

### E9. Foot placement on slopes

- **Looks like.** A standing or slowly walking unit on a slope or stair
  plants its feet on the ground: the uphill knee bends, the pelvis drops a
  little. No more one foot floating over a step.
- **Engine.** After the stock pose: two-bone IK on each leg (hip, knee,
  ankle key bones), targets from ground height under each foot, pelvis
  offset = the lower foot's correction, all eased. Only for stand, idle,
  walk and emotes, not runs, jumps, swims or mounts. Height queries as E8.
- **Off = stock.** IK step skipped; bones untouched.
- **Budget.** CPU: two height queries and a few vector ops per unit per
  frame, units within 30 yd and on screen: under 0.15 ms for 40 units.
- **Risk.** Odd rigs (no clean leg chain), large creatures, knees bending
  backward. Apply to player race models first, with a per-model allow list.
- **Verify.** Stand on the Stormwind cathedral steps, Thunder Bluff ramps,
  Nagrand hills; each race. Check the feet still meet flat ground exactly
  as before on flat ground (no change there).
- **Status (2026-11-03): done, row `footplacement` "Feet on the ground",
  recommended on, stock off.** `rendering/foot_ik.hpp` (pure, in
  `test_phase2_motion`), `CharacterRenderer::updateFootPlacement` (main
  thread, after the M2 update has joined, beside the corpse settle and with
  its floor reads: the highest terrain, WMO or doodad floor up to 1.5 yd
  above) and `foot_ik::apply` at the end of `calculateBoneMatrices` on the
  animation workers. The legs are found per model by a rule, not a list: the
  children of the Waist key bone to either side that reach the body's lowest
  point, walked down by the child reaching lowest to within a fifth of the
  Waist's height of the floor; exactly two, mirrored, of like length, each
  with a knee (all twenty player models pass: plantigrade, and the Draenei
  and Tauren hooves, whose hock turns with the shin as one piece;
  quadrupeds, odd rigs and anything without a Waist have no rig and are left
  alone). Each frame, for a unit within 30 yd, drawn, standing on its feet
  (`standsOnFeet`: not dead, jumping, falling, swimming, sitting, kneeling,
  mounted, flying - a rider is never a candidate) and not on a seat, the
  floor is read under the unit, 0.5 leg ahead of it and under each ankle as
  the last pose put it - again only once that point has moved 0.12 yd, so a
  still unit reads nothing. Each foot's target is its floor less the
  unit's, held to +0.30 / -0.22 of the leg (a floor more than 0.75 leg away
  is a ledge or a wall and ignored); the body drops by the lower foot's
  need, at most 0.22 leg; the feet tilt with the slope (from the front read
  and the two feet), at most 20 degrees and not at all on steps (over 40
  degrees between reads). The two-bone solve keeps each bone's length and
  the knee's bend side (forward if the leg is straight), never reaches past
  0.999 of the leg, and turns the foot back to the animation's turn plus the
  slope. Everything eases (0.07 s the heights, 0.15 s the weight) and the
  weight is 1 to 3 yd/s, 0 from 5.5 (walk 2.5, run 7). Zero targets skip the
  solve: flat ground is the stock pose bit for bit (tested). Off: no read,
  no solve, `footIkOn_` one compare per pose. Seen: on the Dalaran curb
  step the front foot stands on the step with its knee bent where off it
  sank into the stone; on the Elwynn road bank (-9098, -185) the uphill foot
  no longer sinks. Cost: 3.10. Gaps: the floor is read at the ankle, not
  under the toe, so a foot half over a step's nose stands on whichever side
  its ankle is; units seen through the remote smoothing's offset read the
  floor where they are drawn; no per-race tuning (one set of limits in leg
  lengths).

### E10. Animation smoothing for other units

- **Looks like.** Other players' and NPCs' changes between moving
  animations, and their turns from packet to packet, are a little softer.
  Fewer hard cuts where a sequence has blend time 0, and no snap of facing
  between heartbeats.
- **Engine.** `CharacterRenderer` blend (`beginSequenceBlend`): a minimum
  blend of 100-150 ms for locomotion-to-locomotion changes only; never for
  attacks, casts, deaths or emotes, whose timing is gameplay. Remote facing:
  ease toward the packet facing at the stock turn rate rather than snapping
  (`unit-motion.md` §2).
- **Off = stock.** Stock blend times (0 cuts) and stock facing application.
- **Budget.** Negligible.
- **Risk.** Foot sliding during longer blends; masking timing a player reads.
  Keep the floor short and exclude combat.
- **Verify.** Watch a playerbot group run and stop, a guard patrol turn;
  recording at 60 fps, A/B.
- **Status (2026-11-03): done, row `remotesmoothing` "Smoother other
  players", recommended on, stock off.** `rendering/remote_smoothing.hpp`
  (pure, in `test_secondary_motion`) and `CharacterRenderer`: for every
  instance but the player's character, native form and mount (and attached
  items, effects, backdrops, riders and passengers, which follow their
  seat), each frame a step in the position that the unit's own recent
  velocity does not explain - more than 0.25 yd beyond the prediction and
  under 5 yd moved (a teleport or a long correction still snaps) - becomes
  an offset, held to 2 yd, that decays with a 0.1 s time constant; a turn
  of more than 0.6 rad in one frame likewise becomes a yaw offset decaying
  over 0.08 s (the stock body ease, `stepBodyFacing`, already turns
  everything smaller). `getModelMatrix` draws the offsets; the unit's real
  position (selection, range, nameplates, the blob) is untouched. In
  `beginSequenceBlend`, another unit's switch between two locomotion
  sequences (Stand, SwimIdle and the moving animations other than the
  jumps) fades over at least 150 ms where the sequence's own blendTime is
  shorter (about 4% of the 3.3.5 character and creature locomotion
  sequences have 0: some Stand, Walk and Run on bogbeasts and fel orcs, the
  guard and citizen NPC models' WalkBackwards); attacks, casts, deaths,
  emotes and jumps keep the stock timing. Off: no offset is kept and the
  matrix and every blend are stock. Cost: 3.8. Gaps: the position snaps
  that Wowee itself makes for remote units in `Application` (its own
  large-correction and creature render-position cache) are upstream of
  this and unchanged; an offset is drawn but not passed to the unit's
  name plate or selection circle, which can lead the model by its decaying
  part of a step.

### E11. Horizon detail: lit WDL and far WMOs

- **Looks like.** Past the view distance, the far mountains keep a hint of
  their shape and colour under the haze instead of a flat fog-coloured
  silhouette. Big landmarks from the WDL (New Hearthglen's cathedral,
  Valgarde, the oil platform) appear on the horizon. The fog still covers
  them; on a hazy zone they still fade out.
- **Engine.** `terrain_horizon.cpp`, `terrain_horizon*.frag`: light the WDL
  heights with the sun and ambient, colour from the minimap tiles (already
  loaded), then apply the stock fog curve instead of the stock "every pixel
  is the fog colour" (fog start 0, end 1). Draw the WDL's MWMO/MODF
  low-detail WMOs (a `terrain.md` §11 gap) in the same pass.
- **Off = stock.** The current stock horizon: fog-coloured silhouette,
  near = far clip - 50, far = 4 far clips, depth range 0.998-0.999.
  The low-detail WMOs are a stock gap: draw them in both modes (fog-coloured
  when off), since stock draws them.
- **Budget.** The horizon is already measured at about 0 ms extra; lighting
  it adds one texture fetch per pixel. Under 0.3 ms.
- **Risk.** A lit horizon can read as a modern open-world client. Keep the
  minimap colour desaturated toward the fog and the fog curve stock.
- **Verify.** Nagrand, Storm Peaks, Barrens at dusk and noon: A/B shots
  from the same spot (`.go` then fixed yaw).
- **Status (2026-11-03): done, row `horizondetail` "Lit far horizon",
  recommended on, stock off.** `rendering/horizon_detail.hpp` (pure, in
  `test_phase2_motion`), `TerrainHorizon` and the `HORIZON_LIT` builds of
  `terrain_horizon.vert/.frag/_far.frag` (`terrain_horizon.glsli`). Lit is
  not coloured: each pixel stays the fog colour, scaled per channel by the
  light its face takes (ambient + sun x N.L, terrain.frag's own terms) over
  what level ground takes, so level ground is the fog colour exactly and a
  sunlit slope a little lighter, a shaded one a little darker; at most 35%
  either way, none at the near limit (the far clip less 50, where the real
  ground is already wholly fog: no seam), coming in over the first 6% of
  the span, thinning as (1 - t)^1.5 to four far clips, and none in a zone
  whose fog ends short of 35% of the far clip (full above 85%). Normals per
  WDL vertex are built from the heights (4 bytes each) when the row is
  first on and released when it goes off, with the two lit pipelines; off
  draws the stock pipelines and binds nothing more (the push constant's z,
  the far clip, is unread there). The minimap colours the plan named are
  not used: the palette stays the fog's. **The WDL's low-detail WMOs**
  (MWMO/MWID/MODF; in 3.3.5 only Northrend has any: New Hearthglen's
  cathedral twice, Valgarde's Icecrown low model, the oil platform) are
  loaded with the map (`TerrainManager::loadHorizonWmos`, the ADT MODF
  placement transform, the render batches' triangles) and drawn in the
  horizon pass by the tiles' reach and near-limit rule, flat fog colour off
  - as stock draws them (terrain.md section 8) - and shaded on. Seen: from
  the Dragonblight ridge (4800, -733), off, Icecrown, the far cathedral and
  the mountains are one cut-out; on, the ridges and faces show under the
  same haze. Cost: 3.10. Gaps: no occluder test (as before); the WMOs'
  doodads and their MOCV are not drawn (stock draws the low model only,
  colour unknown under the fog anyway); the shading takes the sun's
  direction but no shadow (a face turned away is darker, a valley in a
  ridge's shadow is not).

### E12. Indoor light for units and doodads

- **Looks like.** A character in an inn takes the warm colour of the room
  around it, darker in a corner and brighter by the hearth, matching the
  walls. Doodads inside take the same.
- **Engine.** Stock lights an indoor unit from MOCV under its feet with a
  fixed direction (`characters.md` L4, still partly open). Step 1 is the
  stock rule: a per-unit ray to the WMO group's MOCV (fidelity, not an
  enhancement). Step 2, the enhancement: at group load, bake a coarse grid
  of light probes per interior group from MOCV and MOLT (colour and
  direction per cell), and light units and M2 doodads from the probe at
  their position, blended between cells.
- **Off = stock.** Step 1 only.
- **Budget.** Bake at load on a worker (a few ms per group, once); per
  frame one trilinear lookup per unit; under 0.1 ms.
- **Verify.** Goldshire inn, Ironforge Great Forge, Karazhan halls, a cave.
  Compare unit brightness to the wall behind it in the same shot.

### E13. Ambient occlusion, ambient term only

- **Looks like.** Faint darkening where walls meet floors, under doodads,
  inside crevices. Applied only to the ambient part of the light, never to
  the sun, never to the stock baked colours, capped at about 25%.
- **Engine.** A half-resolution GTAO compute pass on the scene depth, then a
  bilateral upsample, read by terrain/wmo/m2/character .frag as an ambient
  multiplier. `rt_lighting` already has an occlusion term behind the
  experimental row; share the read side.
- **Off = stock.** No pass; multiplier constant 1 by specialization.
- **Budget.** 0.8-1.2 ms on Iris Xe at half resolution. That is the reason
  it defaults off.
- **Risk.** High. WMO interiors already carry baked occlusion in MOCV;
  doubling it reads as dirty and modern. Default off; never on WMO interior
  geometry (only on units and doodads there).
- **Verify.** Stormwind streets, Ironforge, Elwynn forest floor: A/B shots,
  check for halos around characters against the sky.

### E14. Contact shadows

- **Looks like.** Feet, hands on weapons and small doodads get a short,
  sharp shadow where the shadow map is too coarse to give one. Characters
  stop looking pasted on the ground at a distance.
- **Engine.** A short screen-space ray march toward the sun (8-12 steps,
  under 0.5 yd) in the character and M2 fragment shaders, or as a pass
  multiplied into the shadow term.
- **Off = stock.** No march.
- **Budget.** 0.3-0.5 ms at full resolution; half resolution if over.
- **Verify.** Character on grass at noon and at sunset, at 15 and 50 yd
  zoom. Check no shadow appears on the lit side at grazing sun angles.

### E15. Caustics in shallow water

- **Looks like.** Faint moving light patterns on the ground under shallow,
  sunlit water.
- **Engine.** Terrain .frag where the pixel is below a known water surface
  (the per-vertex water height already loaded), add an animated pattern
  scaled by sun light, depth (fades out by 3 yd) and LiquidType (water and
  ocean only).
- **Off = stock.** No pattern.
- **Budget.** Under 0.2 ms.
- **Risk.** High for taste: caustics read as a newer client. Default off.
- **Verify.** Stranglethorn coast, Nagrand ponds at noon.

### E16. Cliff texture stretch fix

- **Looks like.** Steep cliffs no longer show the ground texture stretched
  into vertical streaks.
- **Engine.** `terrain.frag`: on pixels where `normal.z < 0.5`, sample each
  layer with two planar projections (x-z and y-z) and blend by the normal,
  instead of the stock top-down coordinates. Same layers, same alpha maps.
- **Off = stock.** Stock coordinates (8 repeats per chunk).
- **Budget.** Two extra fetches per layer on steep pixels only; 0.3-0.6 ms
  in mountain zones.
- **Risk.** High: the stretched streaks are part of how some zones read
  (Thousand Needles, Grizzly Hills cliffs). Default off; only on above a
  steepness threshold.
- **Verify.** Thousand Needles, Storm Peaks, Alterac. A/B shots.

### E17. Upscaled texture pack

- **Looks like.** Sharper textures at high resolution, with the same
  painting.
- **Engine.** No runtime upscaling. The asset manager already reads DDS/PNG
  overrides; a content pack (`docs/upgraded-assets.md`) supplies the files.
  The toggle selects whether the override directory is searched.
- **Off = stock.** Override directory not searched; BLPs only.
- **Budget.** Memory: 4x per upscaled texture. Load time.
- **Risk.** Most AI upscales change the painting. Only packs that pass a
  side-by-side review. Default off.
- **Verify.** Side-by-side at 1x zoom: no change in colour or shape, only
  detail.

### E18. Highlight roll-off

- **Looks like.** Bright skies and sunlit snow keep a little detail instead
  of clipping flat white. Nothing below about 0.8 changes, so the zone
  palette is the same.
- **Engine.** Render the scene into a 16-bit float target, then one tone
  curve: identity up to 0.8, soft shoulder above, applied once in the final
  post pass on every path. Replaces U11, which runs only on the FSR 1 path
  today. No per-zone grading, no LUTs, no bloom beyond the stock ffxGlow.
- **Off = stock.** 8-bit scene target, no curve.
- **Budget.** Bandwidth of the wider target: 0.3-0.5 ms on Iris Xe.
- **Risk.** High if the curve drifts into contrast or saturation changes.
  Default off. A test checks the curve is identity below 0.8.
- **Verify.** Winterspring noon, Nagrand sky, Shattrath sunlit stone: A/B,
  plus a histogram of the frame below 0.8 (must match).

### E19. Weather follows the wind

- **Looks like.** Rain and snow slant and drift with the same wind that
  moves the trees, stronger in a storm. Snow eddies.
- **Engine.** `weather.cpp`: add the shared wind vector (the one `Renderer`
  eases for foliage) to the particle velocity; stretch rain quads along the
  result.
- **Off = stock.** Stock fall direction and speed.
- **Budget.** CPU, the weather particle update: under 0.05 ms.
- **Verify.** Stormy Grizzly Hills, Dun Morogh snow, with foliage wind 1
  and 2.
- **Status (2026-11-03): done, row `weatherwind` "Rain and snow in the
  wind", recommended on, stock off.** `rendering/weather_wind.hpp` (pure, in
  `test_phase2_motion`) and `Weather::setWind`: each drop and flake moves
  with its stock velocity plus the foliage wind at its place - m2.vert's
  direction and gust band (0.55 + 0.45 sin(p.dir * 0.11 - 0.7 t), on the
  scene clock, so a gust passes through the rain and the canopy together),
  9 yd/s at the band's peak times the cloth wind's eased strength (the
  setting times the weather's stir: rain 1.3, storm up to 1.8) - and rain
  streaks are drawn along that velocity. Snow takes 35% of it and an eddy
  of 0.6 yd/s per unit of strength about it. The splash landing estimate
  carries the wind too. Foliage wind at 0 is no wind: the row then changes
  nothing. Off: nothing is added (`windStrength_` 0 skips every block).
  Seen: four frames over 0.2 s of Dun Morogh snow, off, fall in vertical
  dotted trails; on, they slant downwind. Cost: 3.10. Gaps: the rain's
  slant did not show in stills here (the Mod2x streaks are faint against
  Elwynn's canopy); no wind indoors is moot (the weather is off there).

### E20. Smooth zoom and collision ease

- **Looks like.** Mouse-wheel zoom eases in and out instead of moving at a
  constant 8.33 yd/s; the camera's pull-in against a wall eases rather than
  snapping.
- **Engine.** `camera_controller.cpp`: an exponential ease on the zoom
  target; collision pull-in eased over 0.1 s (stock snaps in, eases out over
  2 s, `movement-camera.md` §10).
- **Off = stock.** Linear zoom at 8.33 yd/s, snap-in.
- **Budget.** Negligible.
- **Risk.** Low, but the stock snap stops the camera seeing through walls.
  Keep the ease short and never let the eased camera go past the hit.
  Default off: it changes how the game handles, not how it looks.
- **Verify.** Zoom in a corridor in Karazhan and under trees.
- **Status (2026-11-03): done for the zoom, row `smoothzoom` "Smooth camera
  zoom", recommended on (the plan said off), stock off.**
  `rendering/camera_zoom.hpp` (pure, in `test_phase2_motion`) and
  `CameraController::stepZoomTowardTarget`: the distance follows the
  wheel's request as a critically damped spring (time constant 0.06 s, the
  exact solution over each frame, so the path is the same at 30 and 240
  fps), capped at 60 yd/s, never passing the request, settling onto it; a
  notch turned while it moves carries the speed on. A one-yard notch is 90%
  there in about 0.23 s against the stock 0.12 s. The collision pull-in is
  untouched - a wall still snaps the camera in at once and it eases back out
  as stock - which is why it can be on by default: the plan's concern was
  the eased pull-in letting the camera see through walls, and that half is
  not done. Off: the stock 8.33 yd/s step. Cost: none to measure (a few
  operations a frame). Gaps: the collision ease is left as stock (by
  choice); the barber chair and the login intro set the distance directly
  and leave any speed to the next notch.

### E21. Dust and snow kicked up

- **Looks like.** Running on dry ground or snow throws a small puff at each
  footfall; landing from a jump throws a larger one; mounts do the same
  (replaces today's mount dust, U8). Colour from the terrain under the
  foot.
- **Engine.** The footstep driver already has footfall events and
  FootstepTerrainLookup's terrain type for the sounds
  (`animation/footstep_driver.cpp`). Spawn a small particle burst per event
  with the colour of the top terrain layer under the foot. One system for
  players, NPCs within 30 yd and mounts; retire `mount_dust.cpp`'s own
  spawner.
- **Off = stock.** No bursts. Footprints (stock) unaffected.
- **Budget.** CPU under 0.05 ms; a few hundred particles at most.
- **Verify.** Barrens road, Tanaris sand, Dun Morogh snow, a raid of
  mounted players. No dust on grass, water, wood or stone.
- **Status (2026-11-03): dust done, row `mountdust` relabelled "Dust kicked
  up", recommended on; snow not done.** One system for every footfall:
  `FootfallDust` (`footfall_dust.cpp`, `footfall_dust.{vert,frag}.glsl`)
  replaces `MountDust` (removed with its shaders) and keeps its row's keys,
  so a saved "off" stays off. Feeds: every unit's model $FSD footfall
  events (the same events that play its footstep sound) and, for the local
  character and its mount, the footstep driver's own footfalls (it times
  them with a fallback where the model has no $FSD). They are queued in the
  character update and turned into dust after the M2 update has joined. The
  ground is read the way the footstep sound reads it
  (`footfall_dust_rules.hpp`, tested): water and building floors first,
  then TerrainType.SoundID (1 dirt and 8 sand raise dust; grass, leaves,
  snow, soggy, stone, wood, metal and water never), and for a layer with no
  ground effect the texture's name (dirt, road, path, sand, desert...; any
  name with grass, moss, mud, snow, swamp in it never). Nothing while it
  rains or snows, nothing past 35 yd from the camera, and the ground is
  asked again only after a walker has gone 2 yd. How much follows the
  walker's speed between footfalls and its size: nothing under 1.8 yd/s, a
  single wisp on about every other step at a walk, two small puffs at a run,
  three under a galloping mount; peak opacity 0.22-0.46. Each puff is a
  soft, slightly clumpy camera-facing sprite that starts at about 0.2 yd,
  spreads to 3.5x while it rises slowly and is kicked back from the stride,
  lit by the scene's ambient and sun and fogged like the world, and gone in
  about a second. At most 512 sprites. Verified in Westfall (dirt) on foot
  and mounted and that Elwynn's roads (grass by the
  tables) and the Barrens around the Crossroads (stone and grass by the
  tables) raise none - the dust follows what the footstep sound says, not
  how the ground looks. `WOWEE_FOOTFALL_DUST_LOG=1` logs each footfall's
  speed, ground and the sprite count; `WOWEE_FOOTFALL_DUST_GAIN=<k>` scales
  the opacity for finding puffs in a shot. Cost: 3.7. Gaps: no landing puff
  after a jump, no snow (the row is dust only; snow puffs on SoundID 4 would
  be the same system with a white colour), and the puffs are not soft
  where they meet the ground (they are small and faint enough that the cut
  does not read).

### E22. Ambient life from stock assets

- **Looks like.** At most: fireflies near water at night, moths at lamps,
  using particle models the game already ships, only where a zone already
  places similar doodads.
- **Engine.** A rule table keyed on area and time of day.
- **Off = stock.** Nothing spawned.
- **Risk.** High: this adds content to zones, which is the line between
  "enhanced" and "different". Default off. Do last, or not at all.

### Considered and set aside

- **Torch flicker** and **foliage interaction**: already exist (U3, U4, U1);
  the work is their toggles, not new features.
- **Sun shafts, god rays, volumetric fog, water reflections, wind,
  anisotropic filtering, anti-aliasing**: already exist (section 2). Any
  further work is tuning, not a new row.
- **Ragdoll deaths**: changes the death animation the game authored. E8
  covers the visible problem without it.
- **Look-at heads**: stock has none (`animation.md` §5); turning heads
  toward the player changes how NPCs behave. Not proposed.
- **Per-zone colour grading**: Light.dbc is the zone's grade. Any LUT is a
  second grade on top. Not proposed.
- **Bloom beyond ffxGlow**: the stock glow chain is already done and
  data-driven per LightParams. Not proposed.

## 5. Order of work for Thursday and Friday

1. Framework (3.1-3.3): the enhancement table, the page, the stock test, the
   freeze clock and the pixel test. Classic covers every key.
2. Toggles for the ungated items U1-U9, U11, U13, and the default changes
   in section 2 (shafts, sun shafts, flare, stars, vignette, soundtrack,
   midges). Each is small and makes "off = stock" true.
3. E1 (blob), E4 (soft particles), E2 (fade-in), E5 (rain ripples).
   Done 2026-11-03 (3.6).
4. E8 (corpse settle) and E21 (dust), both small and using existing events.
   Done 2026-11-03 (3.7), with the E4 draw-order fix and E2's clutter and
   game objects.
5. E3 (shadow quality), with the instanced shadow casters. Done
   2026-11-03 (3.9) as a kept world map and a per-frame unit map; sun
   shadows are back in the recommended set.
6. If time is left: E7 capes only, then E10. Done 2026-11-03 (3.8): E7
   with tails and hair as well, and E10.

The rest (E6, E9, E11-E22) waits for after Friday. Each new item goes into
`week-plan-2026-11.md` before it starts, per the plan's rule.

## 6. For Jered to decide

- Generated normal maps and parallax on by default (2.1 #8, #9): keep, or
  default off for the painted look?
- Volumetric mist, sun shafts, lens flare and sharp stars off on a fresh
  install, on from High: agree?
- Water sky reflection (stock has none): stays off by default.
- Midges over water split from splashes and off by default: agree?
- Low health vignette off by default, leaving the stock edge flash: agree?
- Wowee soundtrack off by default (the schema and the field disagree today).
- Classic preset view distance: 1900 (today) or the stock clamp (791.7
  below map 530)? The fog's reference distance is tied to 1900
  (`LightingManager::kFogReferenceViewDistance`), so changing it needs the
  fog checked again.
