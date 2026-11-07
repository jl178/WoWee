| `contactshadows` pass | 0 (indoors: not recorded) | 0 | 0 | 0.39-0.43 first version (1.46 GHz); 0.20 after the clip-space march (1.46, eight steps); final 0.41-0.42 (1.16 GHz, a compile on the CPU), 0.17-0.29 (half) | final 0.317-0.318 in all eight windows (full window, 1.5 GHz, slow regime); 0.15-0.23 (half) |
| `contactshadows` frame GPU | ~0 (±0.6) | +0.06 ±0.08 | -0.02 ±0.02 | +0.34 ±0.09 first version; final +0.38 ±0.10 (1.16 GHz, compile) | final +0.24 ±0.02 (1.5 GHz) |
| `contactshadows` CPU | ~0 | ~0 | ~0 | cpu_med +0.06 ±0.26; renderWorld +0.04 to +0.10 | cpu_med -0.01 ±0.19; renderWorld +0.12 (the per-frame depth view, framebuffer and descriptor write, and the barriers) |

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

Done (2026-11-04): E16 unstretched cliffs (`cliffprojection`), E15
sunlight under water (`caustics`) and E18 soft highlights
(`highlightrolloff`), each a row, stock off. Recommended: E15 and E18 on,
E16 off for its cost in hilly views. Costs in 3.13, status under each entry;
`test_phase2_surfaces` holds the rules. On the way: the scene's depth copy
for the shoreline foam is taken from the off-screen scene's own depth (it
read a stale image with MSAA and post-processing on), and an A/B of a row
recommended off flips it to its fullest "on".

Done (2026-11-04, evening): E12 room light indoors (`indoorlight`) and
E14 contact shadows (`contactshadows`), each a row, stock off, both
recommended on. Costs in 3.14, status under each entry; `test_phase2_light`
holds the probe bake and lookup and the contact ray's limits.

Done (2026-11-06, weather): an "Enhancements: Weather" page - E23 weather
intensity (`weatherintensity`, a 1-2.5 slider, recommended 1.5), E24 varied
raindrops (`variedrain`), E25 softer snow (`softsnow`), E26 distant rain and
snow (`weathersheets`, the stock mist Wowee lacked), E6 snow settles
(`snowcover`), E27 lightning and thunder (`stormlightning`), E28 sandstorms
(`sandstorms`), E29 breath in the cold (`coldbreath`), E22 fireflies
(`fireflies`, off), E30 rain heard indoors (`rainindoors`) and E31 a
client-only weather preview (`weatherpreview`). Each stock off; costs in
3.17, rules in `rendering/weather_fx.hpp` and `test_weather_fx`, further
ideas after E31.

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
| 8 | Generated normal maps | `normal_map.cpp`, wmo/m2/terrain .frag | `normalmapping`, `normalmapstrength` | off, 0.8 | off | off (Jered, Nov 6: the painted look) |
| 9 | Generated parallax | same | `parallax`, `parallaxquality` | off, Medium | off | off (3.11 for the budget; Jered, Nov 6, with 8) |
| 10 | Shoreline foam | `water.frag` (`shorelineFoam`), depth copy | `shorelinefoam` | on | off | on |
| 11 | Wet ground in rain | `wet_surface.glsli` | `wetground` | on | off | on |
| 12 | Sky reflection on water | `water.frag`, reflection pass | `waterreflections` | off | **not set** | off; stays an option (Jered, Nov 6) |
| 13 | Swim spray, wake froth, bubbles, reed midges | `swim_effects.cpp`, `water_renderer` wake | `watersplashes` | off | **not set** | split: wakes and spray on, midges off (note) |
| 14 | GPU grass blades | `grass_renderer.cpp` | `grassenabled` + 3 sliders | off | off | off (experimental) |
| 15 | Ray traced sun shadow, occlusion, bounce | `rt_lighting.cpp`, `rt_*.comp` | `raytracedlighting` | off | **not set** | off; stays off on integrated GPUs without ray tracing hardware (3.16) |
| 16 | Fog thickness and fog sky blend | `LightingManager` | `fogstrength`, `fogskyblend` | 1, 0 | 1, 0 | 1, 0 (stock) |
| 17 | Grass/clutter density above 100% | terrain manager | `groundclutter` | 70 | 100 | stock value (note) |
| 18 | View distance beyond the stock far clip (stock UI 177-1277, clamped to 791.7 on maps below 530) | terrain, M2, WMO | `viewdistance` | 1900 | 1900 | 1900, with the WDL horizon (E11); a 400-2400 slider under every preset (Jered, Nov 6) |
| 19 | Camera follow speed, pivot height, smooth follow, idle orbit | `camera_controller.cpp` | `camerastiffness`, `pivotheight`, `smoothfollow`, `idleorbit` | 30, 1.6, off, off | n/a | verify 30/1.6 reproduce stock (note) |
| 20 | Camera shake scale | camera controller | `camerashake` | 1.0 | n/a | 1.0 (stock) |
| 21 | Low health red vignette (Wowee's own HUD effect) | game screen | `lowhealthvignette` | on | n/a | off; stock has `screenEdgeFlash` |
| 22 | Wowee soundtrack in the music rotation | `ZoneMusicController` | `woweemusic` | off (schema, field and login screen agree) | n/a | off (Jered, Nov 6) |
| 23 | MSAA 2x, FXAA, FSR 1/3, frame generation | `post_process_pipeline.cpp` | `antialiasing`, `fxaa`, `upscaling` ... | 2x, off, off | 4x | 2x (MSAA is a stock option) |
| 24 | Anisotropic filtering | `VkContext::getOrCreateSampler` | `texturefiltering` | 16x | n/a | 16x (stock option) |
| 25 | Sharp interface text and art (N14, E32) | `WidgetRenderer::drawMarkupText`, `texturesMipped_` | `crispinterface` | on (recommended) | off | on |
| 26 | Ultrawide HUD (N15, E33) | `WidgetTree::setHudAspect` | `ultrawidehud` | on (recommended) | off | on; nothing at 16:9 and narrower |

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
  rule 4 this would default off, and it does: Jered chose the painted look
  (Nov 6, section 6), so both are off in the recommended set and the
  presets.
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

### 3.11 The set brought back toward budget (2026-11-04)

Measured as 3.5 (Iris Xe, a 1251x986 window - a window the compositor
narrowed is dropped, Ultra quality columns, noon, the camera and clock set
again before every row), each row flipped in place on a 4 s period, three
cycles a row and eight for the whole set (scratch `budget/rows_final.lua`),
at Stormwind's Trade District, Goldshire and Dalaran, each place run with
the build before this work and then the one after, back to back. The night
was not quiet: other agents built and ran clients throughout (load 4-20).
Windows within 10 s of a compiler or another client running are dropped
(`budget/pool.py`), and a run that shared the GPU for more than a minute
was run again. A compile on the CPU moved a GPU window from 15 to 60-90 ms,
which is why. GPU is the summed pass timestamps; CPU is the renderer's
main-thread stages and, beside it, the frame's own CPU time (`cpu_med`, the
frame less its waits); ms, paired median ± standard error.

Goldshire's camera does not land the same way every run, even set again
before each row: some runs look west down the road into the low part of
the sun through the trees, some at the inn. The two are different
measurements - stock draws the road view in 11-12 ms of GPU, the inn in
about 8 - and the screenshot each run takes says which; every Goldshire
pair below is the road view unless marked. Earlier sittings' Goldshire
figures (3.5-3.10) are a mix of the two.

Whole recommended set against stock:

| | Stormwind GPU / CPU (cpu_med) | Goldshire road GPU / CPU (cpu_med) | Dalaran GPU / CPU (cpu_med) |
|---|---|---|---|
| Before (7da25aa) | +4.13 ±0.10 / +0.62 (+0.54) | +7.16 ±0.38 / +0.62 (+0.50) | +2.46 ±0.35 / +1.30 (+1.06) |
| After the changes below, same set | +2.94 ±0.14 / +0.66 (+0.48 to +0.63) | +6.45 ±0.31 / +0.50 (+0.32) | +2.56 ±0.14 / +0.86 (+0.87) |
| After, the new recommended set (soft particles and parallax off) | +1.34 ±0.14 / +0.48 (+0.49) | +6.73 ±0.32 / +0.34 (+0.21) | +1.37 ±0.46 / +0.55 (+0.57) |
| Same sitting, the build before (old set), for comparison | +5.07 ±0.26 / +0.87 (+0.99) | +8.03 ±0.39 / +0.71 (+0.55) | +2.00 ±0.33 / +0.98 (+0.79) |

The last two rows are the whole set alone, fifteen 4 s cycles each, the two
builds run one after the other in each place (`budget/rows_v.lua`); the
Goldshire pair both landed on the road view. One earlier pair landed the
build before on the inn view (+3.64 / +0.61) - the view, not the build,
is most of Goldshire's figure.

Per row (GPU / CPU stages, ms; "-" not measured after a window was dropped).
Single rows read within about ±0.6 ms of GPU and ±0.15 ms of CPU of nothing
in Stormwind and Dalaran - rows that cannot cost anything (low health
warning in full health, wet ground in dry weather) read that much - and
within ±1.5 in Goldshire, whose road view swings by 3 ms on a ~16 s cycle of
its own; only the whole-set rows and the rows marked are outside that:

| Row | Stormwind before | after | Goldshire road before | after | Dalaran before | after |
|---|---|---|---|---|---|---|
| `shadows` | +1.77 / +0.07 | +0.90 / +0.07 | +1.71 / +0.21 | +1.14 / +0.18 | +1.76 / +0.47 | +1.10 / -0.03 |
| `shadowfilter` | +0.25 / -0.08 | +0.67 / +0.00 | +0.26 / -0.10 | -1.62 / -0.06 | -2.02 / -0.00 | +0.75 / +0.10 |
| `locallights` | -0.67 / -0.04 | -1.15 / +0.18 | +1.95 / +0.18 | +1.24 / -0.02 | -3.30 / +0.31 | -3.61 / +0.01 |
| `lightflicker` | +1.14 / +0.04 | +0.41 / -0.00 | -1.98 / -0.26 | +0.31 / +0.00 | +7.96 / +2.38 | +0.90 / +0.10 |
| `normalmapping` | +0.88 / -0.03 | +0.82 / +0.24 | -0.30 / +0.33 | -0.14 / +0.03 | +0.07 / -0.26 | +1.00 / -0.03 |
| `parallax` | +1.81 / -0.32 | +1.10 / +0.07 | +1.52 / -0.22 | -0.05 / -0.04 | +0.33 / +0.00 | +1.38 / -0.06 |
| `lightshafts` | +1.20 / +0.25 | +1.24 / +0.13 | +2.26 / +0.12 | -0.32 / +0.02 | +0.33 / +0.12 | +1.96 / +0.06 |
| `sunshafts` | -0.05 / +0.19 | +0.40 / +0.16 | +0.70 / -0.02 | -1.21 / -0.24 | -2.97 / +0.32 | +0.79 / +0.13 |
| `lensflare` | -0.50 / -0.07 | -0.40 / -0.04 | +0.31 / +0.21 | -1.46 / -0.19 | +0.24 / -0.01 | +0.32 / +0.11 |
| `sharpstars` | +0.34 / +0.13 | -0.63 / +0.08 | -0.49 / +0.01 | -0.82 / +0.30 | -0.82 / +0.58 | +0.44 / +0.19 |
| `streamfadein` | -0.08 / -0.10 | -0.42 / +0.15 | -2.03 / +0.02 | -1.59 / +0.26 | +1.34 / +0.29 | +0.44 / -0.24 |
| `horizondetail` | +0.15 / -0.14 | -0.52 / +0.13 | -0.33 / +0.06 | -1.17 / -0.08 | +3.16 / +0.21 | -0.01 / +0.23 |
| `shorelinefoam` | +0.71 / -0.12 | +0.57 / +0.19 | +1.07 / -0.01 | -1.07 / +0.06 | +1.68 / -0.43 | +0.45 / -0.47 |
| `wetground` | +0.31 / +0.05 | +0.39 / +0.07 | +1.32 / +0.21 | -1.31 / +0.00 | +3.08 / +0.17 | - |
| `watersplashes` | -0.61 / -0.04 | +0.27 / +0.23 | +0.91 / +0.12 | -1.33 / +0.18 | -4.42 / +0.29 | - |
| `rainripples` | -0.85 / +0.10 | +0.37 / +0.06 | +0.13 / -0.25 | -1.13 / +0.05 | -3.37 / +0.19 | - |
| `weatherwind` | -0.65 / +0.15 | +0.20 / -0.04 | -1.10 / -0.24 | -1.07 / +0.03 | +0.55 / +0.04 | - |
| `foliagewind` | -0.05 / -0.07 | +0.25 / +0.10 | -1.93 / -0.14 | -0.49 / -0.05 | -3.97 / -0.52 | - |
| `foliagepush` | -0.40 / +0.11 | +0.40 / +0.09 | -0.11 / -0.00 | -0.79 / +0.13 | -2.24 / +0.14 | - |
| `boatbob` | +0.79 / -0.14 | +0.26 / +0.19 | +0.27 / -0.23 | -1.15 / +0.33 | -2.93 / -0.02 | - |
| `mountdust` | +0.74 / +0.02 | +0.38 / -0.12 | +0.82 / -0.16 | - | +0.89 / +0.14 | - |
| `corpsesettle` | +0.25 / -0.07 | +0.28 / +0.06 | +0.85 / +0.22 | - | +1.89 / +0.38 | - |
| `secondarymotion` | -0.07 / -0.14 | +0.50 / +0.32 | +0.65 / -0.08 | - | -0.64 / +3.03 | - |
| `remotesmoothing` | -0.11 / +0.09 | +0.35 / +0.09 | -1.00 / +0.20 | - | -2.68 / -0.13 | - |
| `footplacement` | -0.43 / +0.18 | +0.42 / +0.06 | -1.78 / +0.08 | - | -1.72 / +0.07 | - |
| `smoothzoom` | +0.02 / -0.02 | +0.47 / +0.27 | -0.64 / -0.27 | - | +1.91 / -0.22 | - |
| `smoothleaves` | +0.05 / +0.12 | +0.33 / +0.31 | +0.98 / +0.27 | +0.41 / -0.04 | +5.87 / +0.40 | - |
| `softparticles` | +0.52 / +0.18 | +0.35 / +0.27 | +1.76 / +0.13 | +0.47 / +0.04 | +9.11 / +1.61 | - |
| `lowhealthvignette` | +0.75 / +0.05 | +0.47 / +0.21 | +1.12 / -0.12 | - | +9.41 / +0.64 | - |

Dalaran's "before" single rows are mostly scatter: its scene pass sat at
70-120 ms for stretches of that run in both states alike (as in 3.10); its
"after" rows past `sunshafts` fell in windows dropped for a compile. The rows
that stand out of the scatter where the view is steady: the sun shadows
(Stormwind +1.8 -> +0.9, Dalaran +1.8 -> +1.1), parallax (Stormwind +1.8 ->
+1.1), the light shafts (+1.2, of which the inject 0.7 -> 0.3), the local
lights in Goldshire's road view (+1.2 to +2.0) and soft particles' CPU
(+0.15 to +0.27).

What changed (each keeps the picture, except the last two):

- **The mist's inject pass** (`volumetric_fog_inject.comp.glsl`): a 4x4x4
  group now takes four slices of one parity out of eight (`fogInjectCell`
  in `volumetric_fog_math.hpp`, tested in `test_volumetric_fog_math`), so
  on a half-rate frame the carried slices are whole groups and return
  before the light gather, and the gather builds a 64-bit mask with shared
  atomics instead of one lane listing the lights while 63 wait. A cell walks
  the same lights in the same order as before, so it adds the same terms.
  The dispatch is unchanged. Inject 0.57-0.69 ms -> 0.27-0.34 in all three
  places.
- **The unit map draws only units whose shadow can be seen**
  (`shadowCasterSeen`, `shadow_cache.hpp`, `test_shadow_cache`). A unit's
  shadow lies in the capsule its bounds sweep down the sun's rays; one
  whose capsule is wholly behind a plane of the camera's frustum shades
  nothing on screen and is left out, counting as cast so the blob does not
  stand in for it. Receivers are taken to lie no more than 40 yards below
  the lowest of camera, player and unit; not with water reflections on;
  `WOWEE_UNIT_SHADOW_VIEW_CULL=0` draws them all. In the Trade District 19-31
  of the 64 nearest are left out: `shadow/characters` 0.27-0.29 ->
  0.19-0.21 ms, 0.33 -> 0.24 in Dalaran.
- **The light gather** (`M2Renderer::gatherLocalLights`) walks each model's
  light batches from a list made at load (`localLightBatches`) and tests
  the chandelier name once at load, and fetches each light instance and its
  model a few entries ahead: the walk over a few thousand of them was
  waiting on memory (perf: two loads, the instance's model pointer and the
  model's flags, took most of it). Same lights, same order.
  `record+ubo` 0.17 -> 0.12 in Stormwind, 0.31 -> 0.20 in Dalaran.
- **Unit blobs** are taken only for units whose blob is in view, before the
  128 nearest are chosen: the floor samples of blobs behind the camera were
  most of the blob pass (`renderSelectionCircle` was 11% of Dalaran's main
  thread). This is cheaper in stock as well, so it barely moves the set's
  difference.
- **The soft particles' late pass** sets its texture and constants only
  when a run changes them. Recording the pass on a worker was tried and
  given up: the job can start only once the M2 worker has written the
  particle records, and the main thread then waits on it at the late pass.
- **Parallax eases out between 25 and 45 yards** (wmo.frag, character.frag):
  its shift is under a pixel or two there. Its cost did not depend on the
  step count (Low and Medium the same, +1.97 against +1.98 in Stormwind,
  15 pairs each); the trace on every far wall was the cost. Stormwind
  +1.8-2.3 -> +1.1.
- **Recommended defaults: soft particles and parallax off.** With the above
  the set was inside 3 ms of GPU in Stormwind (+2.94) but not inside 0.5 ms
  of CPU (+0.66, cpu_med +0.48-0.63), and Dalaran's CPU was +0.86. The soft
  particles' late pass is 0.15-0.25 ms of main-thread time for edges where
  smoke meets a wall; parallax about a millisecond of scene pass in
  Stormwind and Goldshire for relief a few centimetres deep, and 2.1 #9 had
  it only following the normal maps. Both rows stay, off; the quality
  presets keep their own values.

Still over, and what it would take:

- **Goldshire's road view is over on GPU** (+6.7 ms, against 3; the inn
  view was +3.6 before this work and was not caught again after). Per row it
  shows the local lights (+1.2 ±0.3, the tile walk over a forest's overdraw
  of leaves at noon), the sun shadows (+1.1 ±0.4), soft particles (+0.5),
  smooth leaf edges (+0.4) and the mist's inject (+0.3), about 3.5 ms
  together; the rest of the whole-set difference shows in no single row.
  The likeliest reading is the iGPU running slower under the whole set's
  load than under any one row's (it shares its power with the CPU), which
  per-row A/B cannot see; logging `gt_act_freq_mhz` beside each window
  would say. (3.12 logged it: the clock is the same in both states; the
  memory's regime is what moves the figure.) Fitting 3 ms in this view would take the local lights or the
  sun shadows out of the set; that is Jered's call (section 6).
- **Dalaran is just over on CPU** (+0.55, frame CPU +0.57; +0.86 before
  the default change): the unit map's recording (0.32), the light gather
  (0.20) and the motion rows on the main thread (`renderer->update`,
  `update/characters`, footfalls: 0.25 together). Next: record the unit map's character draws on a worker with
  the scene's secondaries (the render graph would have to open the shadow
  pass after the join), and keep the gather's light positions between
  frames for fixtures whose bones do not move.
- Not done from the list in the brief: the world map's redraw cadence (a
  redraw every ~20 s standing in a city, three frames each - not where the
  time is), the unit map's size (its sampling is in marked cells only), and
  sharing the blob floor cache with the corpse and foot reads (in the set the
  blobs are for units past the unit map's reach and the feet within 30
  yards, so they never ask for the same floor).

### 3.12 Goldshire's GPU, a null shadow view, Dalaran's CPU (2026-11-04)

Iris Xe, 1251x986, Ultra columns, noon, the recommended set flipped against
stock on a 4 s period (`WOWEE_ENHANCE_AB`), camera set again before every
row and a screenshot taken there (scratch `gpu/`: `run.sh`, `chain.sh`,
`pp.py`, `freqab.py`). Every run also logged the iGPU's clock and throttle
reasons every 100 ms (`gt_act_freq_mhz`, `gt/gt0/throttle_reason_*`), and
per-pass figures come from runs with `WOWEE_SINGLE_THREAD_RECORD=1`, which
times terrain, WMO, characters and M2 apart. Goldshire is the road view
(west down the road into the sun, the inn on the right) in every run here;
the row screenshots say so.

**The clock.** The iGPU does not clock down under the set. Quiet, both
states run at 1500 MHz (e2: on 1500, off 1489; Stormwind and Dalaran 1500
and 1447-1492 in both). Under a compile or a busy server the package hits
its power limit (`throttle_reason_pl1` set 90-100% of the time) and the
iGPU runs at 1050-1250 MHz - in both states alike (e1: on 1230, off 1188;
L_2's later rows 1076-1098 both ways). Every millisecond then reads 25-40%
longer, the set's difference with it; that, and other clients on the GPU,
is part of why earlier sittings' Goldshire figures (3.5-3.11) moved so much.

**The memory.** Even at a full 1500 MHz the same road view has two
regimes. With another agent's client running beside it (L_1, L_2, Gno, Gdd)
stock drew in 6.2-7.1 ms and the set added +3.3 to +3.5; with no other
client (L_3, Gw1, Gyes, G16) stock took 11.6-12.1 ms and the set +5.6 to
+6.3. The set's share is the same in both (+0.48 to +0.55 of stock): it is
not the set that saturates anything, the whole frame runs at a different
speed. A busy CPU core at nice 19 (`yes`, cache-resident) did not bring the
fast regime back (Gyes: stock 11.7, set +5.63); the run with a memory
streamer beside it also had another client up, so it settles nothing. The
likeliest reading is the memory or ring clock (Intel's SAGV and the ring
follow load and display demand; neither is readable without root here, and
`perf_event_paranoid=2` keeps the IMC counters closed). A player runs one
client, so the slow regime is the one to budget for.

**Where it goes.** Split by pass, the set's difference is spread over the
world in proportion to it, not in one pass: terrain +1.2 to +2.2, M2 +1.3
to +2.3, WMO +0.1 to +0.7, characters +0.2 to +0.4, the mist's inject +0.3
and the water copy below. Leave-one-out rows at full clock (paired, ms):

| Row | fast regime | slow regime |
|---|---|---|
| `shadows` | +1.48 ±0.52 (L_1) | +1.70 ±0.40 (G16, with 16-bit maps) |
| `locallights` | +0.83 ±0.35 (L_1) | +3.29 ±0.31 (GLnew; +4.14 ±0.33 with the old tile cull) |
| `smoothleaves` | +0.67 ±0.47 (L_1) | +1.40 ±0.45 (Q, a compile on the CPU) |
| `shadowfilter` | +0.96 ±1.03 (L_2) | +0.85 ±0.41 (G16) |
| `foliagewind` | +0.82 ±0.49 (L_2) | - |
| `horizondetail`, `normalmapping`, `streamfadein` | - | +0.8 to +1.2 ±0.4-0.5 (L_3; rows that should cost little - the noise floor there) |

What changed (no row's picture changes):

- **The water's scene copy no longer copies the colour.** The shoreline foam
  reads only the depth copy; nothing samples the colour history since the
  water stopped refracting (water.frag), yet every frame copied the whole
  colour image into it (`WaterRenderer::captureSceneHistory`). "water
  refraction copy" 0.26-0.27 ms -> 0.07-0.17.
- **Local lights are culled to tiles by the sphere's tangent planes and two
  pixels of margin** (`light_tiles.hpp`, `cullLightsToTiles(view, proj, ...)`;
  `test_light_tiles` checks every point of every sphere, offset by a pixel,
  with and without a jittered projection). It was the box's eight corners
  and a whole tile all round, so a torch a tile across marked 3 x 3 or
  more and every pixel there walked it. Same lights per pixel that can
  reach it, same sum. Same rows, same structure, two runs apart:
  `locallights` +4.14 ±0.33 -> +3.29 ±0.31, the whole set +3.80 -> +3.26.
  `WOWEE_LIGHT_TILE_MARGIN_TILE=1` puts the old cull back.
- **16-bit shadow maps were tried** (`WOWEE_SHADOW_D16=1`, left as an
  experiment switch): the whole set +5.98 ±0.33 against +6.33 ±0.38 with
  32-bit, `shadows` +1.70 ±0.40 - no gain to speak of, so the bytes behind
  the filter taps are not the cost. Not taken.

The whole set against stock after this (ms; GPU at 1500 MHz unless marked;
CPU is the renderer's stages and, beside it, `cpu_med`):

| | GPU | CPU (cpu_med) | run |
|---|---|---|---|
| Stormwind, Trade District | +1.02 ±0.12 | +0.23 ±0.03 (+0.22 ±0.15) | Sw1 |
| Dalaran | -0.16 ±0.33 (1447-1492 MHz) | +0.37 ±0.05 (+0.33 ±0.10) | Dw1 |
| Goldshire road, slow regime | +6.33 ±0.38 | +0.22 ±0.04 (+0.19 ±0.08) | Gw1 |
| Goldshire road, fast regime | +3.36 ±0.16 | - | Gno |
| 3.11 (before), for comparison | Stormwind +1.34, Dalaran +1.37, Goldshire road +6.73 | +0.48, +0.55, +0.34 | |

Goldshire's road view is still over 3 ms for a player alone on the
machine. What would fit it, from the rows above: the
local lights are the row that grows with the slow regime (+3.3 there, +0.8
in the fast one) - the per-pixel walk over Goldshire's 48 lights, on every
layer of leaves the canopy draws. Taking `locallights` out of the
recommended set (the stock rule, the four nearest M2 lights, takes over -
stock, not a loss against it) brings the road view to about +3.0 in the slow
regime and +2.5 in the fast; the sun shadows alone (-1.5 to -1.7) would not
be enough. That is Jered's call (section 6); the default is unchanged.

**The null shadow view (validation).** Reproduced on the first try: a
teleport tour under the Khronos layer (`WOWEE_VULKAN_VALIDATION=1`,
`VK_LAYER_PATH` at the layer's `explicit_layer.d`; scratch `gpu/tele.sh`)
reported, after the first map change, "vkCmdDrawIndexed(): the combined
image sampler descriptor [Set 0, Binding 0] is using imageView 0x0" ten
times in a frame. The suspected path was not it: with the M2 shadow pass
guarded it came back, and every report followed a `CharacterRenderer::clear`
(each map change). `clear()` destroyed and recreated the renderer's fallback
textures, but the unit shadow's fallback set (`shadowParams_.set`, the set
at 0 for every opaque batch of every unit) had been written at initialize
with the old white texture's view and was never written again - so after
the first map change every unit's shadow draw bound a set naming a
destroyed view. The fallback textures are now kept across `clear()` (they
are 1x1 constants). Under the layer, nine map changes after the fix: no
such report. Two more found on the way: the WMO shadow pass pushed its
constants for the vertex stage only into a range declared for both
(VUID-vkCmdPushConstants-offset-01796, every building's shadow) - fixed;
and the M2 texture loader ignored a failed upload and cached a texture
without a view, which the M2 shadow pass would then have written into a
live descriptor - the loader now treats it as a failed load, and the M2
shadow pass skips a foliage batch whose texture cannot be sampled, as the
character pass already did. Still reported, not this work's: a scene depth
image submitted in UNDEFINED at the first world frame, after an MSAA change
and in one load hitch, and the pipelines' unused vertex attributes.

**Dalaran's CPU: the unit map's units on a worker.** Right after
`planShadowFrame` a frame worker records the unit map's character draws into
a secondary command buffer that continues the map's pass, while the main
thread does the per-frame block (the light gather), the M2 and grass culls
and the composites ahead of the shadow pass; the pass then records the moved
doodads and buildings into a second secondary, joins the job and executes
both (`Renderer::startUnitShadowRecording`; `WOWEE_UNIT_SHADOW_WORKER=0` for
the old path). Nothing on the main thread touches the character renderer in
between. Dalaran, same binary, same sitting: worker off +0.50 ±0.07
(cpu_med +0.52 ±0.10), on +0.37 ±0.05 (+0.33 ±0.10); the shadow pass's main
thread share 0.29 -> 0.13 ms, the units' 0.29 ms now on the worker, the
main thread waiting 0.02-0.06 ms for it. Under the layer the secondaries
raise nothing.

### 3.13 Measured cost of E15, E16 and E18 (2026-11-04)

Measured as 3.5 (Iris Xe, a 1251x986 window - a run the compositor narrowed
is marked, Ultra quality columns, noon, the camera set again before every
row), each row flipped in place on a 4 s period over the recommended set,
six to eight cycles a row, paired adjacent windows (scratch `enh6/`:
`chain.sh`, `mrun.sh`, `ab_body.lua`, `pp.py`), with
`WOWEE_SINGLE_THREAD_RECORD=1` so the terrain and water passes are timed
apart. Places: Goldshire's road view, the Trade District, Thousand Needles
from the canyon floor below the needles (-4969 -1726), the Storm Peaks at
Brunnhildar (7056 -1698, snow and steep slopes on every side) and the
Echo Isles strand (-1042 -5347, shallow sea at noon). Every row of a place
was measured in one sitting; the regime is given, as performance.md warns:
another agent's client or compile was on the machine for parts of the
afternoon, and the iGPU then ran at 1.05-1.35 GHz instead of 1.5. GPU ms,
paired median, the pass that moves beside it:

| Row | Goldshire | Trade District | Thousand Needles | Storm Peaks | Echo Isles |
|---|---|---|---|---|---|
| `cliffprojection` (final: steep chunks only, 45 degrees) | +0.35 ±0.04 terrain +0.37 (1.46 GHz); +0.69 terrain (1.09 GHz, compile) | +0.12 ±0.05 (1.19 GHz); +0.14 terrain +0.21 (1.36 GHz) | +0.09 to +0.10 (1.5 GHz) | +0.62 terrain +0.54 (1.34 GHz); +0.75 terrain +0.48 (1.15 GHz, compile) | +0.12 ±0.04 (1.45 GHz) |
| `cliffprojection`, first version (every chunk) | +0.86 ±0.03 | +0.27 ±0.03 | +0.69 ±0.14 (1.08 GHz) | +0.72 ±0.07 | +0.18 ±0.06 |
| `caustics` | ~0 (no water in view; pairs -1.6 to +0.2 - scatter) | +0.03 to +0.41, water pass +0.36 where the canals show | ~0 | ~0 | water pass +0.32 to +0.43; row +0.04 to +0.28 |
| `highlightrolloff` | ~0 (pairs -0.9 to +1.0, ±0.6) | ~0 (±0.35) | ~0 (±0.3) | ~0 | ~0 (±0.4) |
| Whole recommended set vs stock (E15, E16 and E18 all in it for these runs) | +2.65 ±0.55 (1.38 GHz) | +0.87 ±0.45 (1.48 GHz) | +1.21 ±0.24 (1.5 GHz) | +3.46 ±0.32 (1.5 GHz) | +2.48 ±0.40 (1.48 GHz) |

What each costs and why:

- **Soft highlights** is two push constants and, on pixels above 0.8, an
  exponential per channel in the pass the glow already runs: nothing
  measurable anywhere (every pair inside its scatter).
- **Sunlight under water** is one more draw of each water surface within
  80 yards, after the water, which reads the depth copy and discards
  whatever is too deep, too far or in shade before the pattern: the water
  pass's timestamp moves by 0.3-0.43 ms where shallow water fills much of
  the view (the Echo Isles strand at a downward camera), nothing where
  there is none. It takes the depth copy the shoreline foam already takes
  (0.08-0.1 ms, "water refraction copy", in the set either way). Within the
  0.5 ms; recommended on.
- **Unstretched cliffs** at first drew every chunk with the cliff build:
  +0.27 ms in the flat Trade District and +0.86 in Goldshire, the shader's
  extra work on every pixel. Now only chunks with a slope over 45 degrees
  in them take it (flat land costs nothing - those chunks are the stock
  pipeline), a steep pixel reads each layer once or twice (weights under
  0.12 folded away) and the derivatives come from the world position's two:
  +0.1 on flat land and coasts, +0.35 in Goldshire, +0.5-0.75 in the Storm
  Peaks - over 0.5 where it is wanted most. Recommended off; on for anyone
  who wants it at that price. Drawing the steep chunks in a pass of their
  own (two pipeline binds a part) was tried and dropped: the change of
  order moved the terrain pass by -1.6 to +1.3 ms with the view.
- The whole set's figures are the set as 3.12 left it plus these rows;
  they move with the view and the clock as 3.12 describes and are not
  these rows' (their sum is at most 0.5 here).

Shots (scratch `enh6/`): `z_needles1.png` and `z_cliff2.png` (a Thousand
Needles cliff face, cliffs on and off), `z6c.png` (the Echo Isles shallows,
sunlight under water on and off), `z_e18.png` (sunlit sandstone in Thousand
Needles, soft highlights on and off).

### 3.14 Measured cost of E12 and E14 (2026-11-04, evening)

Measured as 3.5 (Iris Xe, Ultra quality columns, the recommended set, noon,
the camera set again before every row), each row flipped in place on a 4 s
period, six to eight cycles a row, paired adjacent windows, with
`WOWEE_SINGLE_THREAD_RECORD=1` (scratch `enh7/`: `chain.sh`, `mrun.sh`,
`ab_body.lua`, `pp.py`, which now also pairs `cpu_med` and the
`update/footfall+sfx` stage the probe reads run in). Places: Goldshire's
Lion's Pride Inn by the hearth, the Deadmines' first tunnel, Scarlet
Monastery's entrance hall, the Trade District and Goldshire's road view.
Each place's rows were one sitting; the clock regime is given, as
performance.md asks. The evening was not quiet: another agent's compiles
ran through the Elwynn runs (the iGPU at 300 MHz in one, which was thrown
away), and from 18:40 to 19:30 another agent's client held half the
screen, so one Elwynn and one Trade District run were at a 616-621 x 986
window (marked "half"); the final runs of both were at the full window. The contact shadow pass carries its own timestamp ("contact
shadows"), which is the figure to trust where the frame's scatter is wide.

| Row | Inn | Deadmines | Scarlet | Trade District | Goldshire road |
|---|---|---|---|---|---|
| `indoorlight` GPU | +0.05 ±0.02 (1.5 GHz) | +0.00 ±0.01 (1.5) | +0.01 ±0.01 (1.5) | +0.01 ±0.05 (1.47); -0.12 ±0.5 (half) | ~0 (±0.5, under a compile) |
| `indoorlight` CPU, `update/footfall+sfx` | +0.06 | +0.003 | +0.002 | +0.01 to +0.03 | +0.01 to +0.02 |
| `contactshadows` pass | 0 (indoors: not recorded) | 0 | 0 | 0.39-0.43 first version (1.46 GHz); 0.20 after the clip-space march (1.46); 0.17-0.29 (half, 1.35) | 0.32 (full window, 1.4 GHz, one window); 0.15-0.23 (half) |
| `contactshadows` frame GPU | ~0 (±0.6) | +0.06 ±0.08 | -0.02 ±0.02 | +0.34 ±0.09 first version; ~0 (±0.2-0.5) after | +0.5 ±0.7 (scatter) |
| `contactshadows` CPU | ~0 | ~0 | ~0 | renderWorld +0.06 to +0.10 (the barriers and the pass's recording) | +0.2 under a compile |

What each costs and why:

- **Room light indoors** is a branch on a material value in
  `character.frag` (+0.05 ms where units fill the view indoors, nothing
  outdoors) and one WMO containment query per unit every half second or
  third of a yard on the main thread: 0.06 ms in the inn with its dozen
  NPCs, a few hundredths elsewhere. The bake is at group load, a few
  passes over at most 16^3 cells; the doodads' colours are set at placement.
  Well inside 0.5 ms. Recommended on.
- **Contact shadows** is one full-screen pass: a depth read for every
  pixel, and for the pixels within reach facing the sun, a six-step march.
  The first version (eight steps, 45 yards, a matrix product per step) was
  0.39-0.43 ms in the Trade District at full clock; marching between the
  ray's two clip-space ends, six steps and a 35-yard reach brought it to
  0.32 ms in Goldshire's road view at 1.5 GHz (the frame +0.24 ±0.02) and
  0.42 in the Trade District with the iGPU held at 1.16 GHz by a compile
  (about 0.33 at full clock). Inside 0.5 ms where it is wanted, closest to
  it in a city under load; indoors it is not recorded. Recommended on, as
  sun shadows are. If it is ever over: half resolution, or the depth view
  and framebuffer kept across frames (the CPU side, +0.1 ms of renderWorld).
- Neither moves the whole set's figures (3.12, 3.13) beyond their scatter;
  they were not re-measured as a set tonight.

Shots (scratch `enh7/run/`): `s4_inn1_il.png`, `s4_dm1_il.png`,
`s_inn1_il.png` and `s2_inn2_il.png`
(Goldshire's inn, room light on and off), `s2_dm1_il.png` (Deadmines),
`s_sm1_il.png` (Scarlet Monastery), `r3_e.png` (the contact rays' debug tint
in Goldshire), `d4_*.png` (contact shadows on, off, and where on is darker,
with the clock frozen).

### 3.15 The shadow rows at a player's window (2026-11-06)

Jered played with the recommended set and found the sun shadows, their soft
edges and the contact shadows made the picture worse and cost a lot of
frames. Everything above was measured in a 1251x986 window at the Ultra
columns (4096 shadow map). His client runs on the same Iris Xe, but at
2560x1080 (2.2x the pixels), FSR2 at native scale (`upscaling_mode=2`,
quality 1.0: the projection jitters every frame), no MSAA, normal maps on,
the 500 yd shadow distance and the *default* shadow quality level, a 2048
map. Measured with his `settings.cfg` copied into a test home (scratch
`sh2/`: `mrun.sh`, `pair.sh`, `ab_body.lua`, `look_body.lua`, `pp.py`),
`WOWEE_ENHANCE_AB` with `WOWEE_FRAME_PROFILE=1` and
`WOWEE_SINGLE_THREAD_RECORD=1`, 4 s windows, paired. The machine was not
quiet: another agent's ASan soak client ran beside every run and compiles
held the iGPU at 0.9-1.07 GHz (power limit), so every figure below reads
about 40% longer than at 1.5 GHz; before and after were run back to back in
each place (the "before" build from a copy of its shaders, so both ran in
the same sitting).

**What looked wrong.**

- *The world map's texel was half a yard.* At a 2048 map over 1.05 x 500 +
  8 yd each way, one texel is 0.52 yd: tree and building shadows near the
  player were smears of blocks a few texels wide, and the Soft filter's
  1.5-texel disk turned by screen-space noise put a coarse speckle on every
  penumbra, which crawled under FSR2's jitter.
- *The bias was wrong in units.* `sunShadow` offset the receiver along its
  normal by `shadowParams.z` - one texel in uv, 1/2048 - as if it were
  yards: a thousandth of a yard, no normal offset at all. The depth bias was
  a fixed 0.0005-0.00005 of the depth range, 0.16-1.6 yd at the 500 yd
  distance whatever the map's size: too little for a sun-facing slope at a
  half-yard texel (acne in the filter's taps on hills and roofs), too much
  at grazing light (shadows lifted off the walls that cast them). The WMO
  and unit shaders also passed the *bumped* normal, so the bias followed the
  generated normal map's relief.
- *Contact shadows* are where 3.14 left them in looks: feet, the bases of
  grass tufts, crates (the debug tint, `WOWEE_CONTACT_SHADOW_DEBUG=1`), plus
  a thin false line along concave creases seen from close (a tent's seam).
  Their cost is the problem: a full-screen pass at 2.2x the pixels.
- Not these rows, but seen in every "shadows off" shot: the light shafts'
  mist reads the world map, so turning shadows on takes the sunlit haze out
  of a forest's shade - at dusk under Elwynn's canopy nearly all of it. With
  shadows off the mist is lit everywhere. The two pictures differ by far more
  than the shadows themselves (mean brightness 56 against 70 in the dusk
  shot), which is a good part of why "shadows on" read as darker and duller.
  That is the `lightshafts` row's look, left as it is here.
- Not seen: stale world-map shadows after walking (the redraw lands on
  snapped texels) or units' shadows cut off at the unit map's 90 yd edge in
  these views (a unit there is a few pixels).

**What changed** (`shadow_filter.glsli`, `contact_shadow.frag`,
`contact_shadows.hpp`, `Renderer::initialize` and `buildLocalLightTiles`):

- **The world map is twice the quality level's side, up to 4096**
  (`shadowLevelSide_`; the unit map stays at half the level's side). It is
  drawn only on a redraw now, not every frame, which is what made the
  quarter-size default necessary; the level's side is still what the one-map
  path draws (`WOWEE_SHADOW_SPLIT=0`), and
  `WOWEE_SHADOW_WORLD_SIDE_LEVEL=1` keeps it for the split too. At the
  default level the texel is 0.26 yd; 64 MB of depth more (two kept images).
- **Bias in yards.** The receiver moves along its geometric normal by 2.5
  texels x sin(angle to the sun) - which lifts it off its own plane by 2.5
  texels x tan, what the filter's reach needs - plus a quarter texel of depth
  bias, both from the map's own matrix (texel and depth range in yards). The
  unit map takes the same rule at its own texel. WMO and unit pixels pass
  their vertex normal, not the bumped one.
- **Soft is a tent of four bilinear compares** 1.5 texels apart (about 3.5
  texels wide), no noise and no trigonometry: one tap fewer than the disk.
  Original is the stock five taps, unchanged.
- **Far pixels take one bilinear compare.** Where a screen pixel spans more
  than 1.5 world-map texels (its distance x the pixel angle, now in
  `localLightTileScale.z` every frame, over the slant), the filter is below
  what the pixel can show; the far half of a 2560-wide view is most of its
  pixels.
- **Contact shadows share their march across each 2x2 quad**: pixel q takes
  steps q and q + 4 of eight, and the quad pools what it found through fine
  derivatives, each pixel taking a neighbour's finding only if the
  neighbour's depth is within 0.1 yd + 2% of its own - a quarter of the depth
  reads, a quarter-resolution march with a depth-aware reconstruction, no new
  target. The reach is 24 yd (was 35), fading over the last 8.

Off is unchanged: shadows off records nothing and draws the stock blob,
`shadowfilter` 0 is the stock five taps, contact shadows off builds nothing.

**Cost** (GPU ms, paired median; the row flipped over Jered's settings;
`contact shadows` is the pass's own timestamp):

| Row | Goldshire road, before | after (round 1) | Trade District, before | after (round 1) |
|---|---|---|---|---|
| `shadows` (includes the contact pass) | +5.14 ±1.03 (terrain +2.06, M2 +0.88, WMO +0.59) | +3.44 ±0.90 (terrain +1.40, M2 +0.48, WMO +0.26) | +4.45 ±1.43 (WMO +1.20, terrain +1.10) | +2.99 ±0.26 (terrain +1.04, WMO +0.73) |
| `shadowfilter` | -0.51 ±0.81 | +1.09 ±0.52 | +1.17 ±0.50 | -0.18 ±0.39 |
| `contactshadows` pass | 1.21 | 0.81-0.86 | 1.20-1.29 | 0.79-0.99 (at 0.88 GHz) |
| Whole set vs stock | +7.04 ±2.25 (frame 34 vs 29 ms) | +7.58 ±1.51 | +2.02 ±1.22 | +1.31 ±1.47 |

2560x1080 fullscreen, 1.05-1.07 GHz in Goldshire and 0.88-1.03 in the Trade
District. Round 1 is the bias, filter, map size and quad march; round 2 adds
the far-pixel tap and the 24 yd reach:

Round 2 could not be measured cleanly: by then Jered was at the desk on
the workspace the test client opens on, so it ran in a small tiled window
(455x486 before, 924x486 after), and with three other clients up the iGPU
was at 0.45-0.9 GHz. The one figure that survives is the contact pass per
pixel, about 1.0 ms per million pixels before against 0.5-1.0 after at a
lower clock; the far-pixel tap and the 24 yd reach still want a quiet
fullscreen A/B at 2560x1080.

**Recommended values.** Sun shadows and Soft edges stay on: the picture is
fixed (no acne or lifted shadows, twice the texels, no grain) and the row is
a third cheaper at Jered's window (+3.0 to +3.4 ms at about 1.05 GHz, so
about 2.2-2.4 at 1.5, the contact pass included). That is still most of the
set's 3 ms budget on its own at 2560x1080, and Goldshire's whole set is
over (3.12). **Contact shadows: recommended off** (`enhancement_rows.hpp`).
Even with the quad-shared march its pass is 0.8 ms at 2560x1080 and 1.05
GHz, over its 0.5 ms, for a hand-width line at feet and crates; it stays as
an option and the quality presets are unchanged. If the shadows row has to
come out of the set for forest views, that is still Jered's call (section
6).

Shots (scratch `sh2/run/`): `look1_*` (before, Jered's settings, each row
off in turn: `_on`, `_cs0`, `_sf0`, `_sh0`), `look2_*` (the contact debug
tint), `L3base_*` and `L3new_*` (before and after, the clock frozen),
`BA_*.png` (before | after | shadows off, side by side), `k_dusk1.png` (the
mist with shadows on and off at dusk).

### 3.16 Ray traced sun shadows on the Iris Xe (2026-11-06)

Jered turned on Ray traced lighting (Sun shadows) and found the shadows
terrible, the client very slow, and things in the distance black. His
client: the Iris Xe, 2560x1080, FSR2 at native scale (jitter every frame),
the 500 yd shadow distance, 2400 yd view distance. Reproduced with his
`settings.cfg` in a test home (scratch `rt/`), RT off and on, in Goldshire,
the Trade District, an Elwynn forest path and Kharanos looking at Dun
Morogh's mountains (`rt/run/base1_*`, before; `new3_*`, after;
`rt/run/sbs/` side by side).

**Black.** Every WMO wall the rays found shadowed was pure black, and so
were whole buildings and trees in the distance (`base1_gs1_rt1.png`,
`base1_gs2_rt1.png`). Two causes:

- `rtShadow` took the traced 0..1 as the shadow, where each surface shader
  had mixed the shadow map's 0..1 toward 1 by the shadow strength first
  (Light.dbc's shadow band x the Shadow darkness setting; the stock
  0.7..1 for WMOs). The WMO shader scales its *whole* colour by the shadow
  (the stock shadow-map programs do), so a traced 0 was black; on terrain
  and doodads it was a shadow far darker than the map's. `rtShadow` now
  takes the shader's strength (`rt_lighting.glsli`; `wmo`, `m2`,
  `character`, `terrain.frag`), so a traced shadow is exactly as dark as a
  mapped one.
- Far surfaces were traced at all. With a 0.5 yd near plane and 24 bits of
  depth, a surface 1000 yd out is placed to about 0.1 yd, 2000 yd out to
  half a yard; the normal rebuilt from neighbouring depths turns to noise,
  the ray starts under its own surface (the distance was also kept in a
  half float, a yard per step past 2048), and the surface reads shadowed.
  The trace now covers 100 yd from the camera (`kTraceRange`), the
  surfaces fade from the traced shadow to the map's over the last quarter
  (`rtParams.y`), and pixels past it cost nothing. The WDL horizon was
  never traced (its shaders do not read the result), and neither is
  anything else out there now.

**Looks terrible.** Beyond the black: the trace gated the sun on the
normal rebuilt from depth (`dot(n, L) > 0`), which on leaf cards, grass
and edges is noise, so a random part of the foliage went into shadow each
frame. Now only what plainly faces away (n.L below -0.25) skips the ray;
the rest is decided by the ray, which crosses a back-facing surface on its
own, and a sun-facing start is lifted toward the sun so grazing rays clear
their triangles. The temporal filter and the 3-pass a-trous filter were
already there and work under FSR2's jitter (the history test is by
distance and normal, not by pixel). After the fixes (`new3_*`), the traced
shadow differs from the shadow map's mostly in contact detail - under a
window ledge, the barrels on Goldshire's inn steps, the eaves - and is
otherwise the same picture (mean brightness within 1-4 of RT off in every
view); nothing is black and the distance is untouched.

**Slow.** See performance.md, "Ray traced lighting on the Iris Xe". The
scene held the whole loaded world (60k meshes, 18 million triangles),
built into BVHs at 150k triangles a frame - 50-100 ms frames for tens of
seconds after switching on - and every placement was in a top level
rebuilt on the CPU whenever any of them changed; the M2 sync hashed all
70k placements a frame (7.4 ms); shadow rays were 1500 yd. Now
`RtScene::setFocus` keeps 350 yd around the camera (`RtFocus`, unit
tested in `test_rt_bvh.cpp`), builds only the meshes placed there,
ignores changes outside it, and the M2 sync drops far placements before
any lookup; rays are 250 yd, the shadow map covering far casters.

**Cost** (Goldshire's road view, 1251x986 test window - the compositor
would not give the test client 2560x1080 - Jered's settings, the iGPU at
1.0-1.1 GHz with another client and compiles on the machine, so read the
GPU figures to +-30%):

| | Before | After |
|---|---|---|
| `rt_lighting` GPU pass | 5.7-6.8 ms | 8.5 ms |
| CPU, M2/WMO sync | 7.4 ms | 2.6 ms |
| CPU, scene upkeep, first seconds after switching on | 52-62 ms a frame, for tens of seconds | under 0.1 ms after six frames |
| CPU, scene upkeep, steady | 3.3-4.8 ms (top level rebuilt every frame) | 0.04 ms (no rebuild with the camera still) |
| Frame, RT off -> on | 24 -> 36-38 ms | 24-27 -> 30-34 ms |

The GPU pass is dearer than before because the pixels the old gate
skipped (and wrongly shadowed) now trace. At 2560x1080, 2.2 times the
pixels, the pass alone is about 19 ms - six times the whole set's budget.

**Decision.** Even with the CPU side fixed, the trace on this iGPU costs
several times the whole set's 3 ms budget at Jered's window and gives a
picture about like the improved shadow map's (3.15). So without ray
tracing hardware on an integrated GPU the row **stays off**:
`Renderer::setRtLightingMode` refuses it there (one log line;
`WOWEE_RT_ALLOW_SOFTWARE=1` overrides) and the tooltip says so. It stays
off in Original and in the recommended set (both 0); on a discrete GPU,
or one with ray queries, it is still offered as highly experimental.

### 3.17 The weather page (2026-11-06)

E23-E31 and E6 (snow settling), on the new "Enhancements: Weather" page
(`kEnhPageWeather`; `wetground`, `rainripples` and `weatherwind` moved
there, keys unchanged). Measured with `WOWEE_ENHANCE_AB=<row>:4` and
`WOWEE_FRAME_PROFILE=1`, the weather forced with the new preview row
(`weatherpreview`) since the lab server sent none, everything else at the
recommended set, seven to twenty-three adjacent on/off pairs per row, the
median of the pair differences. The window was the tiling compositor's
1251x986 (a client cannot size itself there, and full screen would have
taken Jered's 2560x1080 display while he played); the GPU numbers scale
roughly with pixels, x2.2 at 2560x1080 before FSR2's lower internal
resolution takes most of that back. The machine was loaded (load average
15-20: Jered's client and other agents' builds), the frame vsync-bound at
16.7 ms, so GPU differences under about 0.3 ms are noise; "wx cpu" is the
main-thread stage `update/pre/sky+weather+water+fx`, which is steady.

| Row | Place, weather | GPU ms (median, range) | wx cpu ms |
|---|---|---|---|
| `weatherintensity` 1.5 vs 1 | Goldshire, heavy rain | +0.02 (-0.04..+0.11) | +0.17 |
| `weatherintensity` | Stranglethorn, thunderstorm | ~0 (-1.5..0) | +0.17 |
| `weatherintensity` | Kharanos, heavy snow | +0.12 (-0.37..+0.87) | +0.22 |
| `variedrain` | Goldshire, heavy rain | +0.01 | +0.00 |
| `softsnow` | Kharanos / Grizzly Hills, heavy snow | +0.18 / +0.07 (noisy) | +0.18 / +0.21 |
| `weathersheets` | Goldshire / Stranglethorn / Kharanos / Silithus | +0.06 / ~0 / ~0 / ~0 | +0.01 |
| `snowcover` (one-octave noise) | Grizzly Hills, heavy snow, brown ground | ~0 (-0.20, n23, -4.4..+2.2) | +0.01 |
| `stormlightning` | Stranglethorn, thunderstorm | ~0 | ~0 |
| `sandstorms` | Silithus, sandstorm | +0.13 (-0.45..+0.52) | +0.11 |
| `coldbreath` | Kharanos, 8-24 units near | +0.10 (noisy) | ~0 |
| `fireflies` (recommended off) | Goldshire at 23:00 | +0.03 | +0.01 |
| `rainindoors` | Goldshire (outdoors: nothing to do) | ~0 | ~0 |

Every recommended-on row is under 0.3 ms on both sides; the slider's CPU
(about 0.17-0.22 ms at 1.5, the half again as many drops built on the CPU)
and soft snow's turning flakes (about 0.2 ms) are the largest. The first
snow cover (two noise octaves) read +0.2 to +1.0 ms on a full screen of
ground; one octave reads nothing measurable, and is what ships. No row costs
anything while dry: the style only acts on particles that exist, the snow
cover and haze are 0, and the lightning's timer, the breath's scan and the
fireflies are skipped outside their weather, climate and hour.

Shots (scratch `wx/shots/`): `a_*`, `b_*`, `c_*` are `<place>_stock`
(every weather row off, the preview's weather) and `<place>_enh` (the rows
on), with `_seq1-3` 0.12 s apart: Goldshire heavy rain (`a_goldshire_*`),
Stranglethorn thunderstorm (`b_stv_*`), Grizzly Hills rain (`b_grizzly_*`)
and heavy snow with the cover after 90 s (`c_grizzlysnow_*`, `b_` has the
first, too strong, cover), Kharanos (`a_dunmorogh_*`), Winterspring
(`a_winterspring_*`), the Storm Peaks (`c_stormpeaks_*`), Tanaris and
Silithus sandstorms (`c_tanaris_*`, `a_silithus_*`), Goldshire at night with
fireflies (`c_elwynnnight_*`). Thunder was seen in the sound trace
(`wx/logs/shots1/sound.txt`: the crack 0.7 s and a rumble 2.6 s after
their flashes). Seen on the way: in Gadgetzan and at K3 the weather is
switched off (`Renderer`'s `insideWmoArea` puts the camera "inside" those
towns' WMO areas out of doors), so neither the stock rain nor these rows
draw there; not changed here.

### 3.18 Measured cost of the interface rows, N14 and N15 (2026-11-07)

`WOWEE_ENHANCE_AB=<row>:5` from a `WOWEE_DEBUG_LUA` script with
`WOWEE_FRAME_PROFILE=1`, four adjacent on/off pairs per row, Nagrand, the
stock FrameXML interface, a 1680x720 window (7:3, held with the new
`WOWEE_WINDOW_FIXED`, which a tiling compositor floats; the 21:9 rule is
the same at 2560x1080). The machine was loaded (load average 16-17: other
agents' builds). CPU stages are the main thread's `addonWidgets/layout`
(the tree's layout and the text/art sizing) and `frameXml->draw` (building
the draw lists, text included); GPU is the `interface` pass.

| Row | Panels | layout ms (on, off) | draw ms (on, off) | pair difference | GPU `interface` |
|---|---|---|---|---|---|
| `crispinterface` | none open | 0.60-0.66, 0.60-0.65 | 0.27-0.28, 0.27-0.28 | layout -0.03..+0.03, draw -0.01..+0.01 | 0.09-0.11 both |
| `ultrawidehud` | character frame and backpack open | 0.88-0.94, 0.92-0.94 | 0.53-0.56, 0.54-0.57 | layout -0.04..+0.02, draw -0.02..+0.02 | 0.18-0.20 both |

Both are inside the scatter, well under the 0.2 ms budget. The HUD rule is
one pass over a stock frame's anchors when its parent or anchor is
UIParent (most frames skip it on the first test); the sharp text bakes
from a second font at the same sizes, so the per-glyph work is the same.
The one cost that shows is the moment `crispinterface` is first turned on:
every visible interface texture is uploaded again with its mip chain (the
CPU box filter adds about a third to each upload) and the glyphs are baked
from the second face - two frames of 16-20 ms of `frameXml->draw` in this
run, once. With the row on from the start this is part of the ordinary
first-use upload of each texture. Memory: the mipped copies are a third
larger than the single-level ones, and with the row toggled both sets stay
resident until the device's UI textures are rebuilt.

**Off against 3ab580b.** The base commit built in a second tree (with only
the two test switches, `WOWEE_WINDOW_FIXED` and `WOWEE_SCREENSHOT_FRAME`,
added), both rows off, the same frames captured from both binaries, and
two captures from each binary for its own noise. Neither glue nor world is
deterministic frame to frame (the login and select scenes animate; units,
streaming and the portrait move), so the numbers are per UI region against
that noise:

| Region | base vs base | base vs new |
|---|---|---|
| In world, main bar left (opaque art), 17,100 px | 0 differ | 0 differ (twice) |
| In world, main bar right | 158 differ, max 4 | 158 / 0 differ, max 4 |
| In world, player frame (translucent over the world) | 795 differ, max 1 | 273 / 549 differ, max 1 |
| In world, minimap cluster | 1,802 differ, max 24 | 1,158 / 1,088 differ, max 24 / 1 |
| Login 16:9, Login button | 927 differ, max 7 | 715 differ, max 6 |
| Login 16:9, Options / Quit / left buttons | max 22 / 28 / 39 | max 32 / 18 / 38 |
| Login 21:9 (new, GlueParent rect) vs base 16:9 | - | Login max 6, left buttons max 39, Options max 32, Quit max 18 |
| Login 21:9 bars | - | 0 of 288,000 px not black |
| Character select 16:9, list panel / bottom buttons | - | 5 / 3 px over 16 (the scene behind) |

Every difference is the size of the binary's own run-to-run difference and
sits where the UI is translucent over something that moves; the one region
of opaque art is identical to the byte. The version string is the one real
difference ("unknown" in the exported base tree, which has no `.git`). The
whole frames differ by much more (login 89% of pixels in both base-vs-base
and base-vs-new; world 0.4-0.6% over 16 levels, the trees and terrain
streamed differently and the 3D portrait), which is the scenes, not the
interface. What this does not show is byte-identity under translucent UI:
that needs a deterministic scene, which the glue does not have.

Shots (scratch `ui-re/shots/`, 1680x720): glue `glue_login_1680x720.png`,
`glue_charselect_1680x720.png` (stock pillarbox, 200 px bars);
in game `g_hud_stock.png` / `g_hud_wide.png` and `g_panels_stock.png` /
`g_panels_wide.png` (N15 off/on), `g_text_stock.png` / `g_text_crisp.png`
with the zone text above 32 px and `text_pair_zone.png` (crop, 2x),
`g_scale072_stock.png` / `g_scale072_crisp.png` and `scale072_pair.png`
(UI scale 0.72, crop 4x).

### 3.19 Measured cost of E34, HD water (2026-11-07)

`WOWEE_ENHANCE_AB=hdwater:4` set per place from a `WOWEE_DEBUG_LUA` script
(the `enhanceab` setting), `WOWEE_FRAME_PROFILE=1` and
`WOWEE_SINGLE_THREAD_RECORD=1`, the Iris Xe with Jered's anti-aliasing and
upscaler (MSAA 4x, FSR2 at `fsr_quality` 3, which is scale 1: the scene is
drawn at the window's size), soft particles on (so the water draws after the
scene pass and the colour copy exists), the rest of his set as saved. Noon.
A 1280x540 window, not 2560x1080: Jered was playing on the same machine, so
the windows were kept small. The water pass is per pixel, so its cost at
2560x1080 is about four times these. Eleven on/off pairs a place; paired
median, ms, the water pass and its copy (`water`, `water refraction copy`),
the final shader:

| Place | Stock water pass | HD water pass | HD minus stock (water + copy) | At 2560x1080 (x4) |
|---|---|---|---|---|
| Stormwind canals, Trade District (-8706 594) | 0.50 | 0.91 | +0.42 +0.04 | about 1.8 |
| Elwynn, Crystal Lake shore | 0.63 | 0.86 | +0.22 +0.04 | about 1.0 |
| Westfall Lighthouse, the coast | 2.32 (slow regime) | 4.62 | +1.24 +0.12 (slow regime: the stock pass ran at twice its 1.26 of an earlier run; about +0.6 in the fast one) | about 2.5 |
| Thondroril River, below the bridge | 0.81 | 1.32 | +0.50 +0.04 | about 2.2 |

So 1-2.5 ms at Jered's 2560x1080 where water fills most of the view, over
the 1 ms this row was given. **Recommended off**; on for anyone who wants
it at that price. How it got here (river and canal, 1280x540):

- First version, with a screen-space reflection of the shore (twelve
  steps of a matrix each through the depth copy, a bisection on a hit) and
  the sky dome as a loop over local arrays: canal +1.60, river +1.18.
- The reflection's ray stepped as a line in clip space, eight steps, only
  where the Fresnel term is above 0.06: canal +0.73, river +0.89. Variants
  of that shader at the river: without the reflection +0.58, with flat
  normals +0.77, without the refraction's bend +0.83 - the reflection
  +0.3, the normals (six taps of the liquid texture) +0.13, the bend (a
  depth tap and a colour tap) +0.07.
- Five steps within 60 yards, the sky dome one branch per ring, the depth
  behind the pixel read once for the HD surface and the foam: river +0.84
  with the reflection, +0.53 without. The reflection was dropped; the
  surface mirrors Light.dbc's sky (and the planar sky pass where "Water
  reflections" is on). Final: the table.
- What is left: the body colour by depth, the Fresnel, the glint (one
  shadow tap where it shows), the composition over the copied bed, the fog,
  the normals and the bend. The colour copy itself (`water refraction
  copy`, which copies the colour as well as the depth while the row is on)
  is +0.04 at this size.

The whole frame's GPU time moves with the iGPU's clock regime far more than
by this (performance.md); the pairs are adjacent 4 s windows.

Shots (scratch `hdw/shots/`, 1280x540): `ab_<place>_<noon|dusk>_<stock|hd>.png`
for `canal1`, `canal2`, `canal3`, `crystal`, `westfall`, `river`, `river2`;
`ab_under_noon_<stock|hd>.png` (from under the canal's surface: the same,
the HD surface is drawn only from above). Contact sheets `hdw/final_*.png`.
### 3.20 Measured cost of E35, smoother characters (2026-11-07)

`WOWEE_ENHANCE_AB=characterfidelity:5` from a `WOWEE_DEBUG_LUA` script with
`WOWEE_FRAME_PROFILE=1`, the Iris Xe, a 1280x540 window (kept small: Jered
was playing on the machine, so the GPU was shared and every number is
noisy), Stormwind Trade District at noon. The script spawned 29 friendly
units with `.npc add temp` (guards, sentinels, two trainers) on a 2.5 yd
grid around the player, the plaza's own units beyond them, and the camera at
9 yd: about 30 characters in view, most within 10 yd. GPU is the main
`pass`, as the difference of each "on" window from the mean of the two
"off" windows either side (the first pair dropped: it holds the pipeline
builds and the weld buffers).

| Variant | main pass, on - off (paired) | median |
|---|---|---|
| Every unit within 22 yd tessellated, level up to 5 | +3.1..+5.0 ms (frame GPU 18.1 vs 14.4 median) | about +4 ms |
| The six nearest on screen, level up to 4 (committed) | -1.1..+2.2 ms, 11 pairs | +0.92 ms |

The patch stages cost about the same per unit whatever the level, so the
budget is on the number of units, not the level: about 0.13 ms a unit. The
committed rule tessellates the six nearest units on screen and eases each
one's rounding out over the last 3 yd of that radius, so a unit leaving the
six changes nothing on screen. Main-thread CPU: `beginFrame/record+ubo`
±0.01 ms, `renderWorld` +0.15 ms median (the extra pipeline binds and the
per-frame sort of up to a few dozen distances). Memory: one 16-byte weld
vertex per vertex of each model drawn tessellated (made on first use), and
four pipelines. With the row off none of it exists, and
`character.vert.spv` and `character.frag.spv` build byte-identical to the
base commit's (the tessellation vertex code is under `CHAR_TESS` and built
separately as `character_tess.vert.spv`).

The 2x composite bake (the second part of the brief) was built, shot and
dropped: replaying the stock bake's layers through a Catmull-Rom resample at
1024x1024 took 34-77 ms of main-thread CPU per composite (7-19 layers;
every change of equipment re-bakes), quadrupled each composite's memory
(1.4 MB to 5.6 MB with mips), and at both a 1280x540 window and a 2.2 yd
close-up the result could not be told from the stock 512 bake (scratch
`fid/shots/zoom_selfclose_front.png`). The stock 512 atlas is already about
one texel per pixel on a near character.

Shots (scratch `fid/shots/`, 1280x540, `off_*` stock and `on_*` the row):
morphed displays `human|orc|tauren|nelf` x `plate|robe` x `front|side|close`,
the player in a robe `selfclose_front|side|back`, crowds `crowd0|60|300`;
side-by-side crops `pair_*.png` and `zoom_*.png`.

## 4. Proposed enhancements

Ranked by feel gained over cost and risk. "Cost" is implementation effort
(S under a day, M one to three days, L more). "Risk" is the chance it breaks
the classic look or the stock behaviour.

| Rank | Id | Name | Feel | Cost | Risk | Default |
|---|---|---|---|---|---|---|
| 1 | E1 | Stock blob, then shadow-map units (S1) | high | M | low | map on |
| 2 | E4 | Soft particles | high | S | low | off since 3.11 (main-thread CPU) |
| 3 | E2 | Streaming fade-in | high | S | low | on |
| 4 | E3 | Shadow quality: cascades and soft edges | high | M | low | on |
| 5 | E5 | Rain ripples on water and puddles | medium | S | low | on |
| 6 | E7 | Capes, tails and hair: secondary motion | high | L | medium | on |
| 7 | E8 | Corpse settles to the ground slope | medium | S | low | on |
| 8 | E12 | Indoor light for units and doodads | high | M | medium | on (3.14) |
| 9 | E10 | Animation smoothing for other units | medium | S | medium | on |
| 10 | E9 | Foot placement on slopes | medium | M | medium | on |
| 11 | E11 | Horizon detail (lit WDL, far WMOs) | high | M | medium | on |
| 12 | E6 | Snow cover while it snows | medium | S | low | on |
| 13 | E21 | Dust and snow kicked up | medium | S | low | on |
| 14 | E14 | Contact shadows | medium | S | low | on (3.14) |
| 15 | E19 | Weather follows the wind | low | S | low | on |
| 16 | E20 | Smooth zoom and collision ease | low | S | low | on (zoom only) |
| 17 | E13 | Ambient occlusion (ambient term only) | medium | M | high | off |
| 18 | E15 | Caustics in shallow water | low | M | high | on (3.13) |
| 19 | E16 | Cliff texture stretch fix | medium | M | high | off by cost (3.13) |
| 20 | E18 | Highlight roll-off (on the glow composite) | low | S | low | on (3.13) |
| 21 | E17 | Upscaled texture pack | varies | M | high | off |
| 22 | E22 | Ambient life from stock assets (fireflies) | low | M | high | off |
| 23 | E23 | Weather intensity | medium | S | low | 1.5 |
| 24 | E24 | Varied raindrops | medium | S | low | on |
| 25 | E25 | Softer snow | medium | S | low | on |
| 26 | E26 | Distant rain and snow (the stock mist) | medium | S | low | on |
| 27 | E27 | Lightning and thunder | medium | S | medium | on |
| 28 | E28 | Sandstorms | medium | S | low | on |
| 29 | E29 | Breath in the cold | low | S | medium | on |
| 30 | E30 | Rain heard indoors | low | S | low | on |
| 31 | E31 | Weather preview (client only) | tool | S | none | off |
| 32 | E34 | HD water | medium | M | medium | off by cost (3.19) |

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
- **2026-11-06 (3.15):** the bias is in yards now (it was a uv texel used as
  a world length, and a fixed fraction of the depth range), the kept world
  map is twice the quality level's side, Soft is a four-tap tent with no
  noise, and pixels whose footprint is wider than the filter take one
  compare.

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
  recommended on; off in the recommended set since 3.11 (its late pass is
  0.15-0.25 ms of main-thread time, over the set's CPU budget).** Built differently from the plan: no depth copy. With
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
- **Status (2026-11-06): done, row `snowcover` "Snow settles", on the
  weather page (E23-E31), recommended on.** Not the wet path's factor but
  its own: `weather_fx::snowCoverTarget` (0.45-1 by the stock particle level
  while it snows) eased by `easeSnowCover` (time constant 90 s settling,
  200 s melting; a minute of heavy snow is about half), carried in
  `localLightTileScale.w` (free; 0 in the character preview's block),
  read by `snowCoverAmount`/`snowCoverAlbedo` in `wet_surface.glsli`:
  faces with `normal.z` past 0.55-0.9, albedo luminance under 0.5-0.75
  (so a snowfield's own texture is left alone), spreading from patches a
  two-octave noise picks as the cover grows, at most 70% toward a cold
  off-white that the scene then lights. Terrain and outdoor-lit WMO groups
  only, as the wet ground. Off: the factor is 0 and both shaders skip on
  the uniform. Cost: 3.17.

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
- **Cloth pass (2026-11-07).** Stronger, more visible cloth, a strength
  slider, robes, and a body collider. All in `secondary_motion.hpp`
  (`test_secondary_motion`), `CharacterRenderer` and `character.vert`.
  - **Strength row** `secondarymotionstrength` "How far capes swing", 0-2,
    stock 0, recommended 1, saved as `secondary_motion_strength`. It sits
    under "Capes, tails and hair" on the Motion page, live only while that
    row is on. It scales every chain's steady swing (gain x s) and its
    limits (x `limitScale` = 0.6 + 0.4 s). 0 is off, the same as level 0: no
    spring, collider, robe or cloak bend runs, and every bone and vertex is
    stock. It reaches the springs with the level, through
    `Renderer::setEnhancements` and the per-frame
    `CharacterRenderer::setMotionEnhancements` (clamped by `clampStrength`).
  - **The camera's model fade** (first person, `stock_camera`) changes only
    the player's material opacity and blend pipeline. The cloak bend and
    the robe offsets ride the vertex shader and the bone buffer, so they
    fade with the model, and a fully faded model is neither drawn nor
    uploaded.
  - **Cloak tuning (s = 1).** Cloth springs 1.6 Hz at the root to 1.1 Hz at
    the tip, damping 0.35, gain 0.032 rad per yd/s^2, limits 43 deg out,
    6 deg toward the legs, 23 deg aside. The steady answer eases into the
    limits (`tanh`), so a hard start bends the cloak round instead of
    pinning it. The probe lever behind a cloak is 0.45 yd. The probe takes
    35% of its motion from the root's parent bone as last posed
    (`kGaitShare`), so the stride's bob and a jump's crouch push the cloth
    too, not only the model's travel. The foliage wind counts at 40%
    (`kWindShare`): a breeze stirs a cloak a few degrees. A frame is
    simulated up to 0.1 s (was 0.05), so a hitch doesn't run the spring in
    slow motion. Measured in game at 60 fps (WOWEE_SPRING_LOG, human male):
    the cloak kicks about 33 deg setting off, trails 10-14 deg at a run,
    20 deg out and 15-20 deg aside in a running turn, swings in about 5 deg
    on a stop and settles in 0.7 s, floats about 30 deg out on a jump's
    fall, and drops on landing. Offline (`sim.cpp`), the same chain at 60 fps
    goes to 37 deg at the start, 14 deg steady, 33/22 deg in a running turn
    and 26 deg on a fall.
  - **Cloak bend.** The chain's own bones (HumanMale 5-12) carry only the
    lower part of a cloak; most of it is skinned to the chest. So the whole
    cloak also bends as a plate about a line across the shoulders (15% below
    the cloak geosets' top): per yard below the line it moves out and aside
    by the chain's summed swing (x `kCapeBendShare` = 1), and up by half the
    square so it keeps its length. `CharacterRenderer::updateCapeBend` puts
    this in bone slot 238 (`kCapeSlot`), and `character.vert` applies it on
    cloak batches (geosets 1502-1599, `stages.y` bit 1).
  - **Robes and skirts.** Long robes (geosets 1302-1399) have no bones of
    their own; the hem is skinned to the thighs, knees and ankles. Each leg
    gets one point spring (`stepHem`): 2.2 Hz, damping 0.45, air drag 0.8/s.
    The spring hangs on the middle of the shin, with the spring and damper
    relative to the shin, so moving at a steady speed leaves no lag; the
    stride and the body's starts, stops and turns move it off. The lag is
    softly limited (`tanh`) to 0.10 of the leg's length x `limitScale`, and
    its vertical part is cut to 30%. `character.vert` (`stages.y` bit 0,
    bone slot 239 `kHemSlot`) spreads it over the robe by depth below the
    robe's top, squared, and by distance from that side's leg line, eased
    from 0.09 to 0.22 leg lengths. The cloth that hugs the shin stays on it,
    so the leg does not poke through. Each side takes its own leg's lag.
  - **Body collider.** Five capsules from `foot_ik::findLegs`: each thigh
    (0.11 leg lengths), each shin (0.08) and one across the hips (0.13).
    Each radius is fitted to 85% of the gap the rest pose leaves to every
    tested cloth point, so nothing at rest is moved. After the pose (and
    after E9's feet), each cloth bone's tip and middle, on the midline and
    at 75% of the cloth's half-width each side, are tested. A bone that
    meets a capsule is turned about its pivot along the chain's posed
    outward direction until it clears, at most 0.7 rad, and the bones below
    it go with it (`clearanceTurn`). This also makes the tabard's front flap
    ride over a thigh that kicks into it.
  - **Cost.** 3.8's method (`perf/parse2.py` pairing, 5 s A/B windows,
    WOWEE_FRAME_PROFILE=1, `enhanceab=secondarymotion:5`), noon,
    recommended set, Release, a 1251x986 window, on a busy machine (load
    16-27). `update/characters`, paired median on minus off: Stormwind
    Trade District +0.043 ms (0.64 on, 0.60 off, n=15); Dalaran +0.037 ms
    (1.23 on, n=13). No GPU work beyond a branch on robe and cloak batches.
  - **Not done.** Shoulder pads and dangling armour bits (step 4) were
    dropped. Shoulders are separate rigid item models attached to the
    shoulder bones, with no free bones to swing. Any motion would mean
    animating the attachment matrix, which reads as the pad sliding off the
    shoulder rather than dangling. Creature loincloths are not chains: the
    rule's cloth kind needs character geosets, and a creature's waist
    flaps can't be told from its legs by weight alone.

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
  position (selection, range, nameplates, the blob) is untouched.
  **Changed 2026-11-06:** with the stock unit motion (WotLK) a step is no
  longer guessed from the drawn velocity: the motion reports its
  corrections (a remote packet applied where dead reckoning did not have
  the unit, a monster move that placed it, a ground path's sideways step;
  `Entity::takeMotionCorrection`, `remote_smoothing::stepReported`) and
  only those glide. The guess read a charge's start (0 to 28 yd/s in a
  frame) as a step behind and its stop as a step ahead: the charger lagged
  up to 2 yd, then overshot the end and slid back - a rubber band of the
  row's own making (`unit-motion.md` section 6). The guess stays for the
  older expansions' interpolation. In
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
- **Status (2026-11-04): done, row `indoorlight` "Room light indoors",
  recommended on.** Built from MOCV and MODD, not MOLT: MOLT lights nothing
  in 3.3.5a (`wmo-rendering.md` §7), so its lights would be invented; the
  MOCV is the light the artists baked, and the MODD colours the light they
  gave each doodad (§6). What it does (`rendering/indoor_probes.hpp`):
  - **Bake.** When a group lit as an interior (0x2000, not 0x48) with MOCV
    loads, `WMORenderer::createGroupResources` takes each triangle's
    centroid with the colour the stock unit sample would read there (MOCV
    x 2 clamped, plus the MOHD ambient under MOHD flag 2, characters.md §1
    "Indoors"), weighted by area (held to 25 sq yd), and each MODD doodad of
    the group at its own colour (the MODD averages sit beside the MOCV x 2:
    Goldshire's inn 0.20-0.36 against 0.15-0.48, so no doubling) at the
    weight of 4 sq yd. A grid of about 3-yard cells (1-16 a side) over the
    group's box gets each sample spread over its eight cells, blurred twice
    by [1 2 1] along each axis, divided by the blurred weights; cells nothing
    reaches grow in from their neighbours. RGBA8 per cell, at most 16 KB a
    group; Goldshire, Stormwind and two dungeons baked 970 groups in a run.
  - **Units.** `CharacterRenderer::updateIndoorProbes` reads each visible
    unit within 120 yd (a weapon or rider at its matrix) a yard over its
    feet - again when it moves a third of a yard or half a second passes -
    through `WMORenderer::sampleIndoorProbe`: the tightest group around it,
    and if that group is lit as an interior, the grid's colour there,
    muted a quarter toward its grey, moved 70% of the way from the stock
    colour (the building's MOHD ambient x 2, at least 0.35, which every unit
    takes now). Colour, indoor weight and lean ease over 0.25 s. The
    character material carries them (`probeW` 0 = stock); `character.frag`
    then puts the unit's own colour through the stock split (diffuse raised
    to 168/255, ambient capped at 96/255), leans the fixed direction toward
    the brighter side of the room (the luminance's slope across a cell, at
    most half the direction's horizontal length), adds a fill of the indoor
    ambient x 0.35 on the side turned from it, and uses the unit's own indoor
    weight instead of the player's. So an NPC in the inn is lit as indoors
    while the player stands outside, which stock does too (per unit) and
    Wowee did not.
  - **Doodads.** Each placed MODD doodad of an interior group with a grid
    takes the room's colour at its position through the stock MODD split,
    mixed 50% into its ambient and 35% into its diffuse; set once at
    placement and re-applied to every placed doodad when the row changes
    (`WMORenderer::setIndoorProbes`). No per-frame work, no shader change.
  - **Off.** No grid is baked, no probe read, `probeW` 0 so `character.frag`
    runs the stock expression untouched, the doodads keep the stock MODD
    light. Turning the row on reaches buildings loaded from then on (the
    row's restart note); turning it off takes effect at once.
- **Looks like** (scratch `enh7/run/`): `s4_inn1_il.png` (final, clock
  frozen), `s_inn1_il.png` (before the room colour was muted),
  `s2_inn2_il.png`
  (Goldshire's inn by the hearth: on, the paladin and the innkeeper take the
  room's amber and sit in it; off, both are the building's pale grey and look
  pasted on), `s2_dm1_il.png` (the Deadmines' first tunnel: a cold
  green-blue cave light on, warm skin off), `s_sm1_il.png` (Scarlet
  Monastery's candle-lit hall: warm on, grey off).
- **Cost** (3.14): GPU +0.05 ±0.02 ms in the inn (the extra branch), 0
  elsewhere; CPU +0.06 ms of `update/footfall+sfx` in the inn with its
  NPCs, under 0.03 elsewhere.
- **Gaps.** No per-unit ray to the MOCV under the feet (step 1, the stock
  rule, is still open; this replaces the building's one colour, not that
  ray). MOCV is light, not the walls' colour, so a white-washed room and a
  dark-panelled one with the same lamps read alike. Transition faces (MOCV
  alpha toward the outdoor light) are not read; the indoor weight eases
  instead. A building loaded before the row was turned on keeps no grid
  until it reloads. Groups without MOCV (Stormwind's exterior-lit streets)
  are not interiors and take nothing.

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
- **Status (2026-11-04): done, row `contactshadows` "Contact shadows"
  (enabled with sun shadows), recommended on.** As a pass, not in the four
  surface shaders: they cannot read the depth they are writing. After the
  scene pass closes and before the late particles, `ContactShadowPass`
  (`contact_shadow_pass.cpp`, `contact_shadow.frag`) draws one full-screen
  triangle over the resolved scene colour with the one-sample depth as a
  texture - the same frame the soft particles' late pass uses, through a
  `SoftParticlePass` of its own - and the per-frame block and sun maps at
  set 0. Per pixel (`rendering/contact_shadows.hpp` has the same constants
  and is what the tests hold):
  - sky, pixels past 35 yd (faded over the last 12), silhouettes (the depth
    jumps by more than 0.15 yd + 3% of the distance to the next pixel) and
    surfaces turned from the sun (N.L <= 0.05, the facing from the depth's
    slope) are discarded after one depth read;
  - a ray of 0.6 yd toward the sun from a point lifted 0.02 yd + 0.2% of the
    distance off the surface, six steps jittered per pixel, each a blend of
    the ray's two ends in clip space; a step behind the depth by more than
    0.02 yd and less than 0.35 yd + 1% of the distance is blocked, weighted
    1 - (i/6)^2 by how far along;
  - only then the sun maps (`sunShadow`): the new lit is min(map, 1 -
    blocked), and the pixel's colour is multiplied by (A + D N.L s') / (A +
    D N.L s), A and D the luminances of Light.dbc's ambient and diffuse, s
    and s' the stock shadow term before and after with Light.dbc's shadow
    darkness x the player's setting. So it takes away the sun's share and
    nothing else, never shades a pixel past a full shadow and never twice
    where the map already shades it; at night or under cloud, where the band
    is light, it is faint.
  - Indoors (the player's indoor factor at 0.99) the pass is not recorded,
    and its darkness is scaled by how far outdoors the player is.
  `WOWEE_CONTACT_SHADOW_DEBUG=1` tints red what the ray finds blocked,
  shaded or not.
- **Looks like** (scratch `enh7/run/`): `r3_e.png` (the debug tint in
  Goldshire: feet, a barrel's foot, a crate, the base of the inn's wall, the
  grass tufts); `d2_elwynn1.png` and `d2_sw1.png` (on, off, and where on is
  darker in red); `d4_*.png` the same with the clock frozen. A boot on the road and a barrel's rim on the grass get a
  hand-width line of shade where the unit map's edge is soft; the sunlit
  ground beside them does not change.
- **2026-11-06 (3.15): recommended off.** The march is shared across each
  2x2 quad (two steps of eight per pixel, pooled through fine derivatives
  with a depth check) and reaches 24 yd, not 35; at Jered's 2560x1080 the
  pass was still 0.8 ms at 1.05 GHz.
- **Cost** (3.14): the pass 0.32 ms in Goldshire's road view at 1.5 GHz,
  about 0.33 in the Trade District (0.42 at 1.16 GHz under a compile);
  nothing indoors.
- **Gaps.** Screen space: what is off screen or hidden behind something casts
  nothing, and a thin thing seen edge-on (a sword) can shade the ground
  behind it for a pixel or two at a silhouette the edge test lets through.
  It darkens the resolved colour, so water drawn in the scene pass (MSAA)
  over a shaded bed is darkened with it. Not with MSAA on a device without
  depth resolve (no one-sample depth), as for the soft particles.

### E15. Caustics in shallow water

Done (2026-11-04): the row `caustics`, "Sunlight under water"; costs in 3.13.

- **Looks like.** A faint net of bright lines drifting over the sand, rock
  and anything else in the shallows, as the sun focused by the surface
  would lay it: strongest a hand-span to a yard down, gone by four yards,
  fading out past 45-80 yards from the camera, in the sun's shadow (with
  the shadow map on) and at dusk; nothing at night, on magma or slime, or
  on water inside a building. It is light, not paint: what shows through
  the surface is multiplied by at most 1.3 where a line crosses it, tinted
  by Light.dbc's sun colour, so dark rock takes a dim net and pale sand a
  brighter one.
- **Engine.** `WaterRenderer` draws each water and ocean surface in view
  and within 80 yards a second time, after the water, with
  `water_caustics.frag` and the blend dst * (1 + src). The pixel behind
  the surface comes from the scene's depth copy (the shoreline foam's); its
  height below the surface is the depth; the pattern (two layers of moving
  cell edges, no texture) is read where the sun's ray through that point
  enters the water. The strength follows the sun's height on the stock
  clock (`water_caustics::dayStrength`). The rules are in
  `rendering/water_caustics.hpp`, tested in `test_phase2_surfaces`.
- **Why after the water, not under it.** Drawn before the water - lighting
  the bed alone, which was the plan - it was invisible: the stock water's
  alpha leaves a tenth or two of the bed showing even a hand-span down
  (Echo Isles at noon read the same on and off). After the water it
  brightens what the player sees through the surface, by the same rule.
- **A stock-path fix on the way.** With MSAA and an off-screen scene (any
  frame the glow, FXAA or an upscaler post-processes - the default) the
  water draws inside the scene pass, and the depth copy was taken at the
  end of the frame from the swapchain path's depth, which such a scene
  never writes: the shoreline foam (and so the caustics) read a stale
  image. The copy is now taken from the scene's own depth as its pass
  closes (`setSceneClosedHook`). Nothing changes with foam and caustics
  off: no copy is taken then.
- **Off = stock.** No pipeline (made on first use, released when the row
  goes off), no draw, and no depth copy for it.
- **Budget.** 0.5 ms.
- **Verify.** `WOWEE_CAUSTICS_DEBUG=1` draws the pass's coverage in place of
  the net (red where it draws, green where the depth copy has something in
  front of the surface, blue the depth behind it). Echo Isles at noon.
- **Default.** Recommended on (3.13).

### E16. Cliff texture stretch fix

Done (2026-11-04): the row `cliffprojection`, "Unstretched cliffs"; costs in
3.13.

- **Looks like.** Steep slopes show their rock and earth at the painted
  size instead of pulled into streaks down the fall line; the same layers,
  the same alpha maps, MCCV and baked shadows. Ground flatter than 45
  degrees is untouched. Between 45 and 62 degrees the side reads fade in;
  past about 58 the stock top-down read is gone.
- **Engine.** `terrain_cliff.frag.spv` and `terrain_cliff_fade.frag.spv`,
  builds of `terrain.frag` with `CLIFF_PROJECTION`: on a steep pixel each
  layer is also read projected along world x and along world y (same 8
  repeats a chunk, `textureGrad` with derivatives taken in uniform flow)
  and the reads blended by the slope, the two side reads split by the
  eighth power of the normal's x and y, and any read weighted under 0.12
  folded into the others, so a steep pixel reads a layer once or twice
  (`rendering/cliff_projection.hpp`, tested in `test_phase2_surfaces`).
  Scrolling layers scroll on the sides too. `TerrainRenderer` binds these
  pipelines only for chunks with a vertex steeper than 45 degrees
  (`TerrainChunkGPU::steep`; a pixel's normal is never steeper than its
  vertices'), the stock ones for every other chunk; they are made when the
  row first goes on and released when it goes off.
- **Off = stock.** The stock `terrain.frag` pipelines; the cliff build is
  never bound. With it on, a flat chunk is drawn by the stock pipeline and a
  pixel under 45 degrees in a steep one takes the stock read: the
  frozen-clock shots at Thousand Needles, row on and off, differ only on
  the steep faces (0 of 120,000 pixels on the canyon floor).
- **Budget.** 0.5 ms in mountain zones.
- **Default.** Recommended off, for cost (3.13): 0.1-0.2 ms on flat land
  and coasts, 0.35-0.75 in views full of slopes, over the 0.5 each of these
  rows was allowed. By taste it would be on: it reads as the same art, only
  not smeared - the streaks it removes are the texture's own grain
  stretched, not something the zones were painted with. The quality
  presets take the recommended value.

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

Done (2026-11-04): the row `highlightrolloff`, "Soft highlights"; costs in
3.13.

- **Looks like.** Where the stock full-screen glow pushes a bright surface
  past white - sunlit sandstone in Thousand Needles, snow, pale stone, the
  core of a bright spell - the colour eases into white instead of clipping
  flat, so the shape and some of the hue stay. Every channel at or below
  0.8 is the stock value bit for bit; in a zone or hour whose Light.dbc
  glow is 0 nothing changes at all.
- **Where stock clips.** The scene is drawn into an 8-bit target, so every
  surface reaches the last pass at or below 1; the clip comes from the
  stock glow (ffxGlow: scene + blur^2 x LightParams.Glow), which can carry
  a channel to 1 + glow. That is where the curve sits: at the end of the
  glow composite (`fxaa.frag`, `stockScreenEffect`).
- **Engine.** Per channel above the knee 0.8, [0.8, 1 + glow] is mapped
  onto [0.8, 1] by an exponential shoulder with slope 1 at the knee
  (`rendering/highlight_rolloff.hpp`, which also solves its rate; tested in
  `test_phase2_surfaces`): no channel
  clips, the curve bends only as far as this zone's glow needs. Two push
  constants that were padding. No colour grading, no LUT, no change below
  0.8, no float scene target.
- **Not the plan's design.** The plan proposed a 16-bit float scene target
  and a curve after it. That changes the format of the scene pass, which
  every world pipeline is built against (all recreated, like an MSAA
  change), and costs 0.3-0.5 ms of bandwidth on the Iris Xe; and since the
  surfaces stay within 1 in the stock lighting, the glow is what clips.
  Gap: a surface whose own shader passes 1 (MCCV brighter than neutral, a
  stack of additive particles) still clips at the 8-bit write; covering
  that needs the float target.
- **Off = stock.** White 0 in the push constant and the curve is skipped
  (one uniform branch): the frozen-clock shot with the row off matches the
  build before it in every world pixel (Thousand Needles; only the
  interface's clock text differs).
- **Default.** Recommended on (3.13).

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
- **Status (2026-11-06): fireflies only, row `fireflies` "Fireflies at
  night", recommended off (E22's own risk).** Not a stock model: up to 40
  additive glows (SnowFlake01 tinted yellow-green) within 16 yd, each
  wandering about a spot 0.4-2.4 yd over the ground and pulsing on its own
  clock; only in the zones `weather_fx::climateForZone` marks as woods,
  meadows or marsh, between 19:30-21:00 dusk and 5:00-6:30 dawn, out of
  doors, out of the water and with nothing falling. Drawn in the weather's
  pass (Weather::updateFireflies). Moths at lamps and anything placed per
  doodad were not done.

### E23. Weather intensity

- **Looks like.** A slider, 1 (stock) to 2.5: more drops, flakes or grains
  in the air for the same server weather, the mist sheets and splashes a
  little more (on the square root). It never makes weather where the server
  sends none, and nothing falls below the stock quarter grade.
- **Engine.** `Weather::Style::intensity`; `weather_fx::particleTarget` is
  the stock count expression times `countScale`. The vertex buffers grow on
  first need (`Weather::ensureVertexCapacity`, the old ones retired after
  the frames in flight).
- **Off = stock.** At 1 the target is the stock expression, bit for bit
  (`test_weather_fx`).
- **Status (2026-11-06): done, row `weatherintensity`, recommended 1.5.**
  Cost: 3.17.

### E24. Varied raindrops

- **Looks like.** Most drops fine and faint, a few heavy ones falling faster
  on longer, wider streaks; splashes on the ground and roofs whether or not
  the camera moves, out to 30 yd.
- **Engine.** `Weather::Style::variedRain`: at spawn a drop's size (0.55 +
  r^3), its speed (0.85 + 0.2 size), its streak length and opacity; splash
  size follows the drop. Stock: one size band, splashes within 22 yd and
  only while the camera's averaged speed squared is under 2 (0xa4040c).
- **Status (2026-11-06): done, row `variedrain`, recommended on.** The
  splashes land where the ground query lands them: the terrain and WMO
  floors and roofs, not M2 doodads (rocks, M2 houses). Cost: 3.17.

### E25. Softer snow

- **Looks like.** Flakes from a speck to a clump (0.45-1.9 of the stock
  size, most small), each turning slowly, riding a shared field of slow
  eddies with a lift and drop, the large ones drifting slower.
- **Engine.** `Weather::Style::softSnow`, at spawn and in the update; the
  quad is turned about the view axis by its own spin.
- **Status (2026-11-06): done, row `softsnow`, recommended on.** Cost: 3.17.

### E26. Distant rain and snow (the stock mist)

- **Looks like.** In a heavy fall, soft sheets of rain or snow drift across
  the middle distance (16-70 yd) in the fog colour; in a sandstorm, low
  dust clouds closer in.
- **Engine.** `Weather::Style::mistSheets`: up to 40 camera-facing (about
  the vertical) `SnowMist01.blp` sheets 12-18 yd wide and 20-34 tall,
  standing on the ground under them, living 6-11 s with a sine fade, drifting
  with the weather's drift and the foliage wind; alpha-blended in Light.dbc's
  fog colour, faded by the stock mist rule (none inside 6 yd, all past 18).
  Count by `weather_fx::mistSheetTarget`: rain and snow past half their
  level, as the stock spawn rate (level - 0.5) x 2 (world-lighting.md 8),
  sand at any level.
- **Off = stock.** The stock mist is drawn either way (2026-11-07,
  docs/re/weather-sky.md 2: 12 yd SnowMist01 / WeatherMistGrainy01 squares
  blowing back past the player, 2.7 s, past half the level for rain and
  snow). The row adds only these taller distant sheets beyond it.
- **Status (2026-11-06): done, row `weathersheets`, recommended on.**
  Cost: 3.17.

### E27. Lightning and thunder

- **Looks like.** In a thunderstorm (Weather.dbc 86, past the quarter
  grade), a strike every 4-36 s (by the grade): the land, the haze and the
  units flash cold white in a stroke and a return stroke, then the thunder
  rolls in at the speed of sound - a crack within 700 yd, one of three low
  rumbles beyond, quieter the farther. Indoors the flash is 15%, the
  thunder 60%.
- **Engine.** `weather_fx` strike interval, distance (250-3000 yd, mostly
  far), flash envelope, delay (375 yd/s) and level;
  `Renderer::updateWeatherFx` keeps the timer and the queued thunder;
  `applyWeatherFxLight` lifts the frame's sun, ambient and fog colours. The
  samples are the stock `BlastedLandsLightningBolt01Stand-Bolt*.wav`
  (the Blasted Lands strike doodads' own), played 2D in the Ambience
  category at pitch 0.85-1.05.
- **Off = stock.** No timer runs, nothing is added. The old `Lightning`
  bolt renderer (U17) stays forced off: bolts across the sky read as a
  newer game.
- **Gaps.** The sky dome does not flash (its colours come from the sky
  system, not the frame's light); a flash on the clouds would want the
  sky's own params.
- **Status (2026-11-06): done, row `stormlightning`, recommended on.**
  Cost: 3.17.

### E28. Sandstorms

- **Looks like.** When the server sends a sandstorm (states 22, 41, 42),
  fine grains blow level past the eye along the wind in a low box around
  it, dust clouds roll by (with E26) and the fog closes in by up to 40%
  (55% at the top of E23).
- **Engine.** `Weather::Type::SAND` (only asked for with the row on),
  grains as short dashes of the drop texture in the fog colour, as the
  stock sand.bls is drawn; `weather_fx::sandFogScale` on the frame's fog
  start and end, eased over 3 s; the foliage and cloth wind takes 1.2-1.8
  and the volumetric mist (if on) thickens.
- **Off = stock.** Since 2026-11-07 the stock sand draws with the row off
  (docs/re/weather-sky.md 2: fog-coloured grains from 34-40 yd ahead at
  18.7 yd/s, and the grainy mist). The row no longer draws grains of its
  own; it adds the haze, the stronger foliage and cloth wind and the thicker
  volumetric mist on top of the stock sand.
- **Status (2026-11-06): done, row `sandstorms`, recommended on.** Cost: 3.17.

### E29. Breath in the cold

- **Looks like.** Every three and a half to five seconds a small puff of
  mist leaves the mouth of each player and creature within 32 yd and drifts
  forward and up: always in the snowfields (Dun Morogh, Alterac,
  Winterspring, Northrend but Sholazar), at night in Grizzly Hills, and
  anywhere it is snowing.
- **Engine.** A second `FootfallDust` (built on first use) fed by
  `Renderer::updateBreath` from the units' attachment 17 (Breath; the head,
  11, as a fallback) four times a second.
- **Now (2026-11-07).** The stock breath is traced and drawn with the row
  off (docs/re/weather-sky.md 3): the model's `$BTH` event plays
  `Particles\ColdBreath.m2` at attachment 17 where the unit's area (or its
  zone) has AreaTable flag 0x1. The row's own puffs and zone list are gone;
  it now only extends the same stock breath to wherever snow is falling.
- **Status (2026-11-06): done, row `coldbreath`, recommended on.** Cost: 3.17.

### E30. Rain heard indoors

- **Looks like.** Inside a building whose room drops the weather's loop
  (the stock rule: the group lacks the exterior flag), the same loop goes on
  at a quarter of its level beside the room's own.
- **Engine.** `AudioCoordinator::updateTableAmbience` ->
  `AmbientSoundManager::setRoofRain`, a second looped 2D sound faded in and
  out over the stock 5 s. No low-pass: the engine has none per sound, so
  "muffled" is only quieter.
- **Status (2026-11-06): done, row `rainindoors`, recommended on.** Cost: 3.17.

### E31. Weather preview (this client only)

- **What.** A choice on the weather page - Off, light/medium/heavy rain,
  thunderstorm, light/medium/heavy snow, light/heavy sandstorm - that this
  client draws and sounds in place of the server's weather
  (`Renderer::effectiveWeather`): the particles, the storm light, the
  weather loop, everything that reads the weather. Nothing is sent; the
  server and other players keep the real weather. Stock and recommended
  Off; it is saved like any row, so the tooltip asks to set it back.
- **Status (2026-11-06): done, row `weatherpreview`.**

### E32. Sharp interface text and art (N14)

- **Stock.** A font string's pixel size is `round(height x screenH x
  effective scale / 768)`; FreeType rasterises it unhinted, one sample per
  pixel, at `min(px, 32)`, and above 32 pixels the 32 pixel bitmaps are
  stretched (`stock_font_metrics.hpp`, `docs/re/screen-space.md` section 5).
  So stock text is already at its real size up to 32 pixels, UI scale below
  1 included; what is soft is the 32 pixel cap at large sizes (zone text,
  headings, combat text at 1080p and up). Wowee's off path draws through the
  same pen and sizes but bakes with ImGui's two horizontal samples per pixel,
  a filtered, half-pixel-shifted bitmap; that is left as it was.
- **What.** Row `crispinterface` (HUD page, "Display"), recommended on.
  Glyphs come from a twin of each interface face baked one sample per pixel
  with whole-pixel origins (`registerInterfaceCrispFace`, `OversampleH 1`,
  `PixelSnapH`), at the drawn pixel size with no 32 pixel cap; each glyph is
  placed on the nearest whole pixel of the stock pen's origin. Measurement,
  wrapping and layout are the stock client's either way, so nothing moves.
  FrameXML art is uploaded with a mip chain (CPU 2x2 box, colour weighted by
  alpha so transparent texels do not darken edges) and sampled trilinearly,
  in a texture cache of its own (`WidgetRenderer::texturesMipped_`).
- **Off is stock.** With the row off the draw takes the stock bake and the
  single-level image through exactly the code it did before; the twin faces
  are in the font list but are never asked for a glyph, and no mipped
  texture is made.
- **Cost.** See 3.18.

### E33. Ultrawide HUD (N15)

- **Stock.** In game UIParent is the whole window at every aspect and the
  default frames are pinned to its edges and middle; on 21:9 the corners go
  out to the window's corners (screen-space.md section 4). Only the glue is
  boxed to 16:9 (GlueParent_OnLoad), and that is stock work, done (1.10).
- **What.** Row `ultrawidehud` (HUD page), recommended on (it does nothing
  at 16:9 and narrower). `WidgetTree::setHudAspect(16/9)`: the full layout
  computes a centred area of UIParent's height x 16/9, and an anchor on
  UIParent is measured from that area instead of from UIParent when
  - the frame was made by the stock interface (`Widget::stockFrame`: made
    while FrameXML or a `Blizzard_` addon was loading);
  - the player has not dragged it (`userMoved`; a picked-up frame keeps its
    place and returns to the area when the interface next positions it);
  - its anchors on UIParent do not pin both UIParent's left and right edges
    (full-screen overlays, cinematic bars and anything `SetAllPoints` keep
    the whole width).
- **Why this rule.** The alternatives were (a) insetting UIParent itself,
  which also boxes the full-screen overlays and every addon, and makes
  `GetScreenWidth`/`UIParent:GetWidth` lie; and (b) re-parenting the
  default frames under a new safe frame, which changes `GetParent`, frame
  levels, scale inheritance and `IsShown` chains that FrameXML reads. The
  anchor rule changes nothing a script can see except where those frames
  land: UIParent, `GetScreenWidth` and every frame's parent are stock, and
  rects read by Lua are the drawn ones, so hit testing and Lua agree.
  Centred anchors (the action bar, the cast bar) land where they did since
  the area is centred; the minimap, chat, bags, right bars, tooltip default
  and UI panels move in to the area's edges. Frames hung off those follow
  them. Addons' own frames keep the whole screen, which is what an addon
  laid out for an ultrawide expects.
- **Gaps.** Frames the stock Lua creates at run time (`CreateFrame` from an
  event handler after the load) are not marked stock and keep the whole
  width. An addon that moves a stock frame with `SetPoint(..., UIParent,
  ...)` gets the area's edges, not the screen's. Lua that measures against
  `GetScreenWidth` (the bag columns' wrap, the tooltip's right-edge clamp)
  still uses the whole width. The engine-drawn world text (nameplates,
  bubbles) is not part of it.
- **Off is stock.** Aspect 0: no area is computed and the anchor loop takes
  UIParent's rect as before.
- **Cost.** See 3.18.

### E34. HD water

Done (2026-11-07): the row `hdwater`, "HD water" (the Sky page, after
"Sunlight under water"); costs in 3.19. Recommended off by cost.

- **Looks like.** The stock water, sharper: the same LiquidType texture
  sequence (lake_a, ocean_h and the rest) through its stock texture matrix,
  added as stock adds it, over Light.dbc's close and far colours. The
  surface ripples with the frame's own pattern; it mirrors the zone's sky
  at grazing angles (at most 40%), takes a glint of the sun and a faint
  sheen round it, is clear in the shallows and goes to the far colour and
  LightParams' deep alpha with depth, fades out over the last 0.3 yards to
  the shore instead of cutting a line, and bends what lies beneath a little.
  The canals keep the river far colour stock gives WMO water and gain only
  the clarity. Magma, slime, water inside a building and water seen from
  below are the stock surface.
- **Engine.** `water_hd.frag` with `water.vert`, the water's layout, vertex
  format and blend (`WaterRenderer::buildHdPipeline`, made on first use for
  the scene pass or the 1x pass, let go when the row goes off). Per pixel:
  - *Normals*: two layers of the current stock frame, one at the stock
    0.06 per yard drifting one way, one at 0.026 turned 37 degrees and
    drifting another; the gradient of the frame's brightness (rgb and its
    shimmer alpha) by forward differences a texel or a pixel's footprint
    apart, at 0.35 strength, fading to a third by 350 yards. No texture of
    our own.
  - *Depth*: the scene's depth copy (the foam's) behind the pixel gives the
    bed's position: its depth straight down sets the colour (1 - e^-d/3.5
    for rivers, lakes and canals, /7 for the ocean, close -> far), the path
    through the water the opacity (shallow alpha x 0.4 -> deep alpha, /3 and
    /5). Where the copy has nothing behind the pixel, the stock ramp.
  - *Reflection*: Fresnel (F0 0.02) through half the normals' tilt (the
    full tilt sent rays into the canal walls: dark blots), capped at 0.4,
    of Light.dbc's six sky colours by the skybox's ring rule (skybox.frag),
    or the planar sky pass where "Water reflections" is on.
  - *Glint*: Blinn-Phong from the stock light direction and colour,
    exponents 400 and 48, by day, one shadow-map tap with the shadows on.
  - *Refraction*: where the water draws after the scene pass the colour
    copy is taken with the depth (`captureSceneHistory`, 1:1 into the
    corner of the history; `hdParams.zw` scales the uv), and the shader
    composes the bed itself: bed x (1 - op)(1 - F) + body x op (1 - F) +
    sky x F + glint, alpha 1. The bed tap is bent by the normals up to 2.2%
    of the screen, nothing at the waterline (so the shore meets it), and
    falls back to the straight tap if the bent one lands on something in
    front of the water. With MSAA and the water in the scene pass there is
    no colour copy of the scene without the water (the end-of-frame copy has
    the water in it, so that call now passes no colour); there the same
    result is blended as the stock water blends, unbent.
  - *Shore and foam*: the soft edge is the HD surface's; the foam is still
    the "Shoreline foam" row's, drawn as water.frag draws it, held a little
    off the sand so it sits on the fading water. Rain ripples and the wake
    froth as water.frag.
- **Tried and dropped.** A screen-space reflection of the shore through
  the depth copy: it looked right (the Thondroril bridge in the river) but
  cost 0.3 ms at 1280x540 even cut to five steps (3.19).
- **Off = stock.** `water.frag` and its `.spv` are unchanged, and with the
  row off it is the pipeline bound; the HD pipelines do not exist; no colour
  is copied; the frame UBO's new fields lie past the end of the block
  water.frag declares. With the row on and no other reader the depth copy
  is taken for it, as for the foam.
- **Budget.** 1 ms at 2560x1080 with FSR2; measured 1-2.5 ms where water
  fills the view (3.19).
- **Verify.** Stock-vs-HD shots at the canals, Crystal Lake, the Westfall
  coast and the Thondroril, noon and dusk, and from under the canal's
  surface (3.19).
### E35. Smoother characters

- **Stock.** A unit is its M2 mesh, skinned per vertex, and on a near
  character the outline of a shoulder, a skull or a calf shows its facets.
  Wowee already lights units per pixel from the interpolated, renormalised
  normal with the stock light model (character.frag), so the facets show on
  the outline only.
- **What.** Row `characterfidelity` (first Enhancements page, "Surfaces"),
  recommended off. Phong tessellation (Boubekeur and Alexa 2008) of the
  skinned mesh: `character_tess.vert` (the stock vertex shader built with
  `CHAR_TESS`) skins exactly as stock, robe hem and cloak bend included, and
  hands on the clip position, the weld normal and its clip-space image;
  `character.tesc` sets each edge's level from its two corners alone (view
  depth, how edge-on they are, the unit's share of the budget), so the two
  patches on an edge agree and no crack opens; `character.tese` pulls each
  new point 0.4 of the way to the corners' tangent planes, in world and
  clip space at once (the projection is linear, so no matrix is needed),
  and the stock `character.frag` shades it.
  - **Seams.** The planes are weld normals (`weldNormals`): one normal per
    position, averaged over the split copies a UV seam or a hard edge makes,
    so both sides of a seam move alike. Where the copies disagree by
    15-35 degrees or more (a boot's cuff, a belt edge, the two faces of a
    hair card) the corner's weight eases to 0 and it stays put.
  - **Where.** Only the outline: an edge's level rises only where a corner
    is near edge-on to the camera, up to 4, full inside 6 yd and none at
    22 yd. Only the six nearest units on screen; past that a unit is drawn
    by the stock pipelines. Not the attached items (shoulders, helmets,
    weapons): Phong rounding blew a plate pauldron up into a cushion, a
    different shape rather than a smoother one. Alpha 0.6 made boots and
    gloves puffy; 0.4 keeps the art's shapes.
  - **Lighting.** Nothing to add: the per-pixel light from interpolated,
    renormalised normals is already what the off path does, and the
    tessellated points shade through the same fragment shader. A rim term
    was not added; on these textures it reads as a newer game.
  - **Texture.** A 2x bake of the composite was tried and dropped (3.20).
    Item and composite textures already have full mip chains and 16x
    anisotropic samplers on the off path.
- **Winding.** With Vulkan's upper-left tessellation domain the evaluation
  stage declares `cw`; `ccw` turned every face's `gl_FrontFacing` around and
  character.frag lit it from behind.
- **Off is stock.** No pipeline, weld buffer or per-frame work is made; the
  stock pipelines and `character.vert.spv` are unchanged; `stages.z` in the
  push constant, which only the tessellation vertex shader reads, is 0. A
  device without `tessellationShader` keeps the stock mesh with a warning.
- **Cost.** About 0.9 ms of GPU in a 30-unit crowd, 0.15 ms main thread;
  see 3.20.

### Further weather ideas (not done)

- **The stock mist and sand on the stock path** (E26, E28): trace the mist
  sheets' life and motion (0x78c420, 0x786e10) and the sand emitter's
  texture (0x784580) and draw them with the rows off.
- **The stock breath** (E29): trace what makes a place cold for `$BTH`.
- **Weather.dbc's own textures**: rows 90 and 21/83/91/96-100/108 name
  `Particles\LeafBrown.blp`, a red rain and others; Wowee draws every rain
  as RainDrop01. Falling leaves would then come from the data (row 90),
  not invention.
- **Sky flash**: hand the lightning to the sky system so the clouds light.
- **Drips from eaves** under WMO roof edges after rain (needs the roof
  edges; the WMO's MOPY/normals could give them).
- **Low-pass on indoor rain and thunder** once the audio engine has a per-sound filter.
- **Puddles that stay** a while after rain on level ground (the wet factor
  and the ripple mask already know where).
- **Snow on branches and M2 roofs** (m2.frag; the cover only reaches
  terrain and WMOs).
- **Dust motes in sunbeams** indoors, by the light shafts' volume.
- Set aside: camera-local rain on the screen edge, distant birds, falling
  leaves without data - each reads as a different game.

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

All decided by Jered on Nov 6:

- **Generated normal maps and parallax** (2.1 #8, #9): **off** by default,
  for the painted look. `normalmapping` is off in the recommended set and,
  since the quality presets start from it, in every preset (Low to Ultra);
  `parallax` was already off (3.11). Both stay switches on the page.
- **The budget and Goldshire's road view** (+6.3 ms GPU, 3.12): the torch
  and lamp light (`locallights`) **stays on** in the recommended set; the
  forest view stays over the budget.
- **Volumetric mist, sun shafts, lens flare, sharp stars**: left as they
  are.
- **Water sky reflection** (stock has none): **stays an option, off by
  default.**
- **Midges over water** split from splashes and off by default: left as
  they are.
- **Low health vignette**: left as it is.
- **Wowee soundtrack**: **off by default.** The row, `Enhancements::
  woweeMusic`, `ZoneManager` and the login screen's flag all start off.
- **Classic preset view distance**: **1900**, as today, so the fog keeps
  its reference distance (`LightingManager::kFogReferenceViewDistance`).
  The View distance slider (Graphics page of Video options) runs 400-2400
  under every preset, Classic included - a preset sets it and moving it
  reads Custom - and its tooltip gives the stock far clip (791.7 yards
  below map 530, 1583.3 from 530, docs/re/terrain.md). Lowering it looks as
  stock's does: the fog's end is the smaller of Light.dbc's and the far
  clip (0x7f16f0, `stock_light::frameFog`), its start that fraction of the
  end, and from map 530 on the end is the far clip and its exponent is
  read against it (`stock_light::lightFog`). So the fog closes in with the
  distance and the world never stops short of it.
- Outside this page: action bars cast on key-down by default
  (`ActionButtonUseKeyDown` 1, docs/re/input-timing.md), Wowee's disenchant
  prompt is gone (docs/re/items.md), and the Captured Valgarde Child stays
  white, as stock draws it.
