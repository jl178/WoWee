# the reference client's lighting and atmosphere

What the official "the reference client" client (product `wowf`, build 1.60.1.70862,
launched Nov 4 2026) does to make 1.12's world look the way it does, read from
its binary, its shaders and its data rather than from screenshots, and what
Wowee took from it.

Sources, all from Jered's install (`_enhanced_/`, the shared CASC store) and
the same build's files on wago.tools (same build config
`5ada954223f7c409ea172361f5c9e565`):

- `_enhanced_/Wow.exe` (77 MB, x64, D3D12) - strings, CVar registrations.
- `_enhanced_/WTF/Config.wtf` - the settings Jered runs (graphicsQuality 6).
- `_enhanced_/Logs/gx.log` - the device and what it enabled.
- Shaders: `shaders/{compute,pixel,vertex}/dx_6_0/*.bls`. A `.bls` is a
  `GXSH` container of zlib streams holding DXIL; inflated and disassembled
  with `dxc -dumpbin` (DirectXShaderCompiler 1.10). cbuffer *struct* names
  survive in the DXIL type table (`SceneData`, `PSFog`, `GlobalFogParams`,
  `ProbeMetaData`, `CompactBVHNode`, `GIRayRecord`...); member names do not.
- DB2 tables (`Light`, `LightData`, `LightParams`, `SSAOSettings`,
  `GroundEffectTexture`, `GroundEffectDoodad`) of 1.60.1.70862, compared
  with Classic Era 1.15.9.70576, the 1.12 data on the same engine.
- Colour grading LUTs, FileDataIDs 8286666, 8286665, 8286667, 8248426 and
  the neutral 1140733.

The scripts that did this are in the session scratchpad
(`scripts/bls_extract.py`, `dis_all.py`, `lut.py`, `lut2.py`, `cmp.py`,
`palette.py`, `ground*.py`); nothing from the client was executed.

## 1. What Enhanced is

Enhanced is not a new renderer bolted onto 1.12. `gx.log` names it "World of
Warcraft Retail x86_64 1.60.1.70862", it uses the retail shader family
(`SM6`), and its exe carries the whole retail settings set. It is the modern
(12.x, Midnight-era) engine running 1.12's world with re-authored data. Its
look comes from three things:

1. Retail's modern lighting pipeline, all of it on: cascaded sun shadows with
   soft filtering, ASSAO, clustered local lights, froxel volumetric fog with
   layered global fog and sun shafts, a probe-based GI ("Secondary Lighting",
   `giQuality`), PBR water, and colour grading.
2. Re-authored light data: every outdoor `Light` on the two continents has
   new `LightParams` and twelve keyframes a day instead of seven, with new
   columns only Enhanced has.
3. Distance: `farclip 7000`, `horizonStart 1900`, `horizonClip 7000`, and
   fog that ends far past what is drawn.

Jered's `Config.wtf` (graphicsQuality 6): `giQuality 3`, `graphicsLightMode
2`, `SSAO 3`, `sunShafts 2`, `graphicsComputeEffects 3` (volumetric fog
level), `shadowMode 3`, `shadowSoft 1`, `shadowTextureSize 2048`,
`shadowNumCascades 3`, `shadowBlendCascades 1`, `groundEffectDensity 48`,
`groundEffectDist 200`, `reflectionMode 2` (SSR), `pbrLiquidDetail 1`,
`waterDetail 2`, `refraction 2`, `farclip 7000`, `horizonStart 1900`.
`gx.log`: `rt:None` - the GPU's ray tracing is not used (it is a VM), so
nothing below needs hardware ray tracing.

## 2. Light data: re-authored, more keyframes, new intensities

`Light` row 1 (the map default of Eastern Kingdoms, used across Northshire
and most of Elwynn) points at `LightParams` 12 in Classic Era and 7748 in
Enhanced. Era has keys at 0, 3, 6, 12, 18, 21, 22 h; Enhanced at 0, 4.5, 5.5,
7, 8, 12, 15, 17, 18, 19.5, 20.67, 21.5 h - a slow dawn and a long golden
evening. At noon:

| | Era (1.12) | Enhanced |
|---|---|---|
| DirectColor | `#ff8800` | `#eabd8a` |
| AmbientColor | `#68829a` | `#68829a` |
| SkyFogColor | `#4d788f` | `#659ebb` |
| SkyMiddleColor | `#3aa2cf` | `#45c4ff` |
| FogEnd / FogScaler | 18000 / 0.25 | 18000 / 0.25 |
| ColorGradingFileDataID | 0 | 8286666 |
| new F052 | - | 1.35 (night 1.0, dawn/dusk 1.5) |

Over every light matched by ID and position on maps 0 and 1, sampled 8-17 h
(`palette.py`): Enhanced's direct light keeps its brightness but loses ~19% of
its saturation (chroma ratio 0.81: a whiter sun), ambient is ~15% brighter,
the sky's fog colour ~30% brighter and more saturated, the sky's middle band
~19% brighter. The lights were re-authored one by one - a single matrix does
not fit them (error 0.1 per channel) - so these are trends, not a formula.

**Ported** as the "Daylight (the reference client)" row (`enhancedDaylight`,
`include/rendering/enhanced_palette.hpp`, applied in `LightingManager::update`
after the band blend): each colour's luminance and chroma scaled by the
measured trend (direct x1.03 / x0.81, ambient x1.15 / x1.08, sky top x1.09 /
x1.12, sky middle x1.19 / x1.28, fog and sky fog x1.30 / x1.35, sun sprite
x1.11 / x0.72), weighted by dnc.db's day intensity, on maps 0 and 1, not
under water. 3.3.5's old-world lights are 1.12's, so the trend applies to
what Wowee reads.

Enhanced-only columns (names unknown; no public definitions exist):

- LightData `Field_1_60_1_69876_045..059`. 048 (1 or 1.5), 049 (0.3-4, mostly
  1 or 1.5), 051 (0.1-8; Light 1: 1 by day, 2 at dawn, 3 at night), 052 (0.1-5;
  Light 1: 1 at night, 1.35-1.5 by day), 058 (0.1-5), 059 (0.2-3, mostly
  0.45). 052's shape is that of a sun intensity in a linear-light pipeline
  (the exe's `linear` CVar: "0 - disabled (live), 1 - linear", and
  "BGRA8Unorm is not supported with linear lighting enabled. Using RGBA16F").
  055 is a colour (`#1b2745` at midnight, `#e8f4f7` at noon) on 527 rows,
  probably a sky-light or GI colour. 045-047 are colours on 428 rows. Their
  meaning is not established; nothing here depends on it.
- LightParams `Field_1_60_1_69876_032..038` and retail 12.0's `_016..031`.
  `_031` is 0.001 on 370 rows; `_037` 0.7/0.8; `_036` 1/8/16/32.
  `SsaoSettingsID` is 116 on 260 of 882 rows (section 4).

## 2b. Per-zone keyframes: Enhanced's own light, re-keyed to 3.3.5

The trend above is now only the fallback. With the "Daylight (the reference client)"
row on, every 3.3.5 light on maps 0 and 1 that has a Enhanced counterpart is
sampled from Enhanced's own keyframes; the rest keep the trend.

**Columns.** Classic Era 1.15's LightData against 3.3.5's LightIntBand and
LightFloatBand for the same LightParams ids (1.12's rows; `colmap.py`): 99%
of keys equal, which fixes the order. Colour channels 0-17 are Direct,
Ambient, SkyTop, SkyMiddle, SkyBand1, SkyBand2, SkySmog, SkyFog,
ShadowOpacity, Sun, CloudSun, **CloudLayer1Ambient** (channel 11),
**CloudEmissive** (12), CloudLayer2Ambient (13), OceanClose, OceanFar,
RiverClose, RiverFar - the DB2 names 11 and 12 the other way round from the
order Wowee's enum gives them. Float bands 0-3 are FogEnd, FogScaler,
SunFogAngle (always 1: celestial glow through) and CloudDensity; 4 and 5
are not in the DB2 (0.95 and 1 throughout). Units are 3.3.5's: Light 1's
noon FogEnd is 18000 in Era, 3.3.5 and Enhanced alike, so Enhanced's fog is
read exactly as 3.3.5's is (and on the same 1/36 scale; the "18000 yards"
of section 5 is 500 yards in 3.3.5's reading). Times are half-minutes.

**Which light.** Light ids are shared: 186 of 3.3.5's 209 lights on the two
continents are at the same place and size in Enhanced. But Enhanced moved or
shrank some (Mulgore's 201 from 1131 to 105 yards, Ashenvale's 192 to 22)
because it also lights by **ZoneLight polygons** (`ZoneLight`,
`ZoneLightPoint`, 17 on maps 0 and 1: Elwynn, Mulgore, Ashenvale, Durotar
north, Orgrimmar, Wetlands, Burning Steppes, Blasted Lands, Stormwind
Harbor, Hyjal...): inside one, its light is the canvas instead of the map
default. So the match is by ground: each 3.3.5 light is sampled over its
area (32x32, weighted by its own falloff), Enhanced's lights are painted at
each point as the stock client paints them (canvas, then farthest first),
and the Enhanced light with the largest share wins. Map defaults take map
defaults; a 1.12 light that Enhanced gave to the default or a zone polygon
takes that; a light 3.3.5 added (TBC/WotLK: 54, 59, 60, 70, 1763...) or one
no Enhanced light holds half of keeps the trend. Result: 198 of 209 lights,
both slots clear and storm, 112 Enhanced profiles, 1287 keys. Moved: 20, 50
and 78 to Burning Steppes' 7 (its polygon), 202 to Mulgore's 201, 201 and
239 to the Kalimdor default, 34 to 31. Unmatched: 54, 59, 60, 70, 71, 192,
204, 248, 255, 269, 1763.

**What is copied.** The colour and fog bands of the clear and storm slots,
up to 16 keys (Enhanced has 12). A column a Enhanced profile leaves at zero on
every key is one Enhanced does not use (water colours on 115 of 126 outdoor
profiles, the shadow grey, the second cloud layer); 3.3.5's band stays. A
zero FogEnd on one key means no fog end there (Enhanced sets it on 277 of
1455 keys) and is left out of that band. LightParams fields (skybox, glow,
water alphas) stay 3.3.5's: Enhanced's Glow is 0 on 120 of 126 (its bloom is
separate) and its skyboxes 712/715 do not exist in 3.3.5. Underwater and
death slots stay stock.

**Not used.** Enhanced's extra columns (above) and retail's
`HorizonAmbientColor`/`GroundAmbientColor` (837 keys, near the ambient: a
sky/ground hemisphere ambient). From the data alone: 057 is close to the
DirectColor (mean RGB distance 47), 055 to SkyBand1 (72); the intensities
051/052/058 need a linear pipeline to mean anything (section 9b). The
ZoneLight polygons themselves are not shipped: where 3.3.5 has only its map
default (most of Elwynn, Wetlands), Enhanced's zone light is not applied -
Elwynn's 16849 (LightParams 7747) differs from the default's 7748 only in
the dawn sun.

**The ambient is bounded (an approximation).** A few Enhanced profiles carry
ambients far outside the rest - Mulgore's LightParams 7567 is `#bae6ff` by
day, `#cb84ff` at 19:30 and `#1d9eff` at night - which on 3.3.5's clamped
8-bit lighting read as a strong violet or cyan cast. Enhanced must scale
them in its linear pipeline, but no column explains how: its 058 is
0.5-0.8 on those keys and 1-2 on Light 1's darker ambient, yet over all
outdoor keys no column makes ambient brightness more consistent. So the
generator bounds each key's ambient by what Enhanced's zones use at that
hour: every half hour, the 90th percentile of the ambient's luminance and
of its chroma (distance from its grey) over all the shipped profiles; a key
above either is scaled down, then pulled toward its grey, keeping its hue.
210 of 1287 keys move (median 22/255 on the largest channel); 7567's become
`#697d89` by day, `#9167af` at 19:30 and `#3a7bac` at midnight. When
Enhanced's scaling is understood this should be replaced by it.

**Where the data lives.** The converted table is Blizzard's data, so it is
not in the source tree (the repository is public). `tools/enhanced_light_gen.py`
(3.3.5 DBCs + Enhanced CSVs) writes it to the client data's override folder,
`<data>/override/wowee/enhanced_lights.bin` (125 KB; layout in
`include/rendering/enhanced_lights.hpp`), and `LightingManager` reads it
through the AssetManager as `wowee\enhanced_lights.bin`, as an override pack
file. Without it the log says so once and the row uses the averaged trend
everywhere; a file that fails its checks is treated the same way. The test
reads a made-up two-key table, not Enhanced's.

**Code.** `LightingManager::loadEnhancedProfiles` builds, at load, one profile
per linked light and slot over the slot's stock profile; `profileForSlot` returns it while the row is on, on maps 0 and 1,
not under water (a storm slot that falls back to the clear set falls back to
Enhanced's). The trend now applies per light before the blend, only to lights
sampled from 3.3.5; it is linear, so on a map without Enhanced links the
result equals the old after-blend trend. Off is stock exactly: the profiles
are not looked up.

**Seen in the lab** (Enhanced preset, the row off and on, 6:30 to 20:30, in
Goldshire, Westfall, Darkshire, the Crossroads and Bloodhoof; the
`sky:` log line counts the lights on Enhanced data as `enhanced=`):
Goldshire's noon sun is whiter and its sky brighter, as section 2's table
says; Darkshire keeps its blue night-in-daytime; the Barrens' haze toward
a morning sun is paler. Mulgore changed most. Before the ambient bound it
was washed pale by day, violet at dusk and cyan at night. With the bound,
Bloodhoof and Thunder Bluff at noon are close to 3.3.5 with a cooler
shade, and at 23:00 they are a clean blue night. At 19:30 both are still
warmer and pinker than 3.3.5: Enhanced's orange sun (`#ff6d00`) is over a
violet ambient that sits at the 90th percentile of what the other zones
use at that hour. A 75th-percentile bound would have moved 533 of the
1287 keys, so the bound stays at the 90th. Only two Enhanced captures exist
(Northshire, Goldshire); none of Mulgore was available to check against.

## 3. Colour grading

`ffxcolorgrading.bls` (FDID 2990792) samples the finished picture, clamps it
to 0..1, and reads a 32x32x32 LUT laid out as a 1024x32 strip: blue picks the
slice (`b*31`, two slices lerped by `frac`), red and green the texel
(`r*0.0302734 + 0.000488 + slice/32`, `g*0.96875 + 0.015625`). So the grade
is on the display-encoded LDR image, after the glow and tone curve.

Which LUT is used is LightData's `ColorGradingFileDataID` per keyframe. On
the two continents 181 of Enhanced's 870 LightParams set it, all on the 12:00
key only; the 8:00 and 15:00 keys either side carry 0. Eastern Kingdoms'
outdoor lights use 8286666 (70 of 83 lights), Kalimdor's 8286665 (103 of
132); 8286667 and 8248426 appear in a few instances. 1140733 is the identity
LUT (mean error 0.002).

Every Enhanced LUT is *separable*: `lut(r,g,b) = (c(r), c(g), c(b))` with zero
error, so each is one 32-point curve. 8286666:

```
0.000 0.031 0.063 0.094 0.129 0.165 0.200 0.235 0.267 0.306 0.341 0.376
0.412 0.451 0.490 0.529 0.565 0.604 0.647 0.686 0.725 0.765 0.804 0.851
0.890 0.929 0.957 0.980 0.988 0.996 0.996 1.000
```

Shadows untouched, mid-tones x1.10 (0.52 -> 0.56, 0.65 -> 0.73, 0.77 ->
0.89), the top fifth on a shoulder. Applied per channel it also raises
saturation (median chroma x1.13) with no hue shift. This is a large part of
the "polished" daytime look: brighter, punchier mid-tones without a tint.

**Ported** as the "Colour grade (the reference client)" row (`colorGrade`,
`include/rendering/enhanced_grade.hpp`, the end of `fxaa.frag`): both curves
exactly, map 0 -> 8286666, map 1 -> 8286665, other maps none; weight by the
hour as the keyframes interpolate (0 at 8:00, 1 at 12:00, 0 at 15:00), times
(1 - indoor factor), off under water. Two push constants in a pass that
already runs; cost below the profiler's noise.

## 4. Ambient occlusion: ASSAO, tuned hard

The exe registers Intel's ASSAO (`Engine/Source/ShaderEffect/ASSAO.cpp`,
`assao*` CVars with Intel's own descriptions) and ships its shaders
(`assao_generateq0..q3`, `assao_smartblur`, `assao_apply`). Enhanced's
`LightParams.SsaoSettingsID` is 116 on 260 rows, including Light 1 of both
continents. `SSAOSettings` 116:

| Column | Value | ASSAO meaning (Intel defaults) |
|---|---|---|
| Field_8_2_0_30080_001 | 0.1 | HorizonAngleThreshold (0.06) |
| Field_..._002 | 1 | - |
| Field_..._003 | 2 | blur passes (2) |
| Radius | 1 | 1 yard (1.2) |
| ShadowMult | 3 | strength x4.3 (1) |
| ShadowPower | 3 | (1.5) |
| ShadowClamp | 1 | (0.98) |

Row 106 (5 rows) is Radius 4, Mult 1.15, Power 1.6. So Enhanced's AO is
tight (a yard) and very contrasty: three times ASSAO's default strength and
twice its power. It is what seats grass, roots, crates and walls into the
ground.

**Ported** as the "Ambient occlusion" row (`ambientOcclusion`,
`assets/shaders/ambient_occlusion.frag.glsl`, run by `ContactShadowPass`
with its own shader): ASSAO's obscurance term `max(0, N.D - 0.1) *
max(0, 1 - |D|^2/r^2)`, r = 1 yd, clamp 1, power 3, on a fixed 24-point
spiral shared by each 2x2 quad (6 depth taps a pixel), positions rebuilt in
view space, faded out 60-120 yd. It takes only the ambient share of each
pixel's light (the sun's share times N.L times the shadow map keeps its
value), blended dst * src, before the contact shadows. Needs a one-sample
depth (no MSAA, as the contact shadows).

Two departures, both from side-by-side shots in Goldshire: the multiplier
is ASSAO's default 1 (x4.3) instead of Enhanced's 3, and the ambient keeps at
least 0.6 of itself. At Enhanced's 3 the road, every shaded bush and the
characters' legs went near black: Enhanced adds its probe GI (section 6) over
the occluded ambient and Wowee has nothing to add back. Three lessons from
getting it to work, kept in the shader: the projection's [1][1] is negative
(Vulkan's flip), so the disk radius needs its absolute value; world
positions near 10^4 yd lose the precision the facing needs, so it works in
view space; and a per-pixel noise rotation with no blur pass after it (ASSAO
runs two) shows as diagonal bands.

## 5. Volumetric fog and the two fog layers

Two systems:

1. **Classic distance fog**, now retail's `PSFog` (14 float4 per fog, 12 of
   them in `SceneData`) with main fog + height fog. Enhanced's outdoor data
   uses almost none of the extras: `FogDensity` set on 218 of 6008 keys,
   `FogHeight` on 84, `MainFogStartDist` never. Its outdoor fog ends at
   `FogEnd` 18000 with `FogScaler` 0.25 by day: past the 7000-yard far clip.
   So within what is drawn, the distance fog is a thin haze, and the far
   terrain (horizon to 7000 yd) is never swallowed by a fog wall.
2. **Froxel volumetric fog** (`volumefog*.bls`, `gxMTVolFog`, "Compute
   Effects"). `volumefogglobalfog.bls` (FDID 6674334) injects a list of
   *global fog layers* (`GlobalFogParams[36]`, count in the cbuffer) into the
   froxel volume. Read from its DXIL:
   - Slices are cubic in depth: `z = far * ((slice + 0.5 + jitter) / N)^3`
     (`log2 * 3`, `exp2`), jittered per frame by a blue-noise texture plus
     the golden ratio 0.618034.
   - Per layer and froxel: coverage `saturate((z - start) / thickness)`;
     optical depth `coverage * thickness * density * (1 + k1 * t^k0)` with
     `t = saturate((z - start) / far)` (a distance curve); optional upper
     and lower height fades `saturate(exp(+-k * (worldZ - h)))` (flags 64
     and 128; the editor calls them Upper/Lower Height and Height Density).
   - In-scattering `(emissive + scatterColour * phase(cos) * intensity) *
     opticalDepth`, where `phase` is a 2D LUT indexed by
     `1 - (0.5 - 0.5 cos)^(1/3)` and a per-layer row (the anisotropy).
   The WMO editor strings (`WWFFogVolumeLayerData`) name the same fields for
   local fog volumes: Start Distance, Emissive Color, Thickness, Upper/Lower
   Height and Height Density, Camera Relative, two scrolling 3D noise layers
   ("Use Noise", "Noise Scale 01/02", "Noise Direction 01/02").

   The "two stacked layers" of the public talk are most likely this layer
   list; how many layers the outdoor zones use, and their values, come from
   data this work did not decode.

Wowee already has a froxel volumetric fog (`lightShafts`, `mistDensity`,
`volumetric_fog_inject.comp`) with shadowed in-scattering, a height falloff
and HG phase; the Enhanced preset keeps its stock mist (section 8c). Wowee's base
extinction works out at 0.0013/yd in Elwynn by day, the same order as the
0.001 that 370 of Enhanced's LightParams carry in `Field_12_0_1_65617_031`
(a likely volumetric density; unconfirmed).

**Ported** as the "Sun haze (the reference client)" row (`sunHaze`,
`assets/shaders/enhanced_haze.frag.glsl`, a full-screen pass after the
opaque scene, blended `src.rgb + dst * src.a`): one global layer of the
model above over the whole view, integrated in closed form along each
pixel's ray instead of in cells - density `0.00022/yd * exp(-(z - ground) /
220 yd)` (first 0.0006; section 8c), clear for 120 yd and ramping in over 400, sky pixels taken to 5000 yd, in-scattering
of the fog colour (x0.35) and the sun (x1.1 times HG g 0.7 mixed 60/40 with
even scattering; Enhanced's phase table itself was not read), outdoors
only. Wowee's froxel mist still does the shadowed shafts in the first 400
yards; with the haze on, the screen-space sun shafts (no stronger, section 8b)
keep three quarters of the sun's hue instead of half. 0.06 ms at 1280x540.
The constants were set side by side against Enhanced's Goldshire afternoon
(section 9). Not ported: the cubic slice spacing, several layers, the
height fades' per-zone values, and the phase table - data not decoded.

## 6. Global illumination ("Secondary Lighting")

Enhanced-era shaders with no names in the community listfile
(`unknown_8xxxxxx_generatemip-guide`, `unknown_8316767/8316772_gen-gla`, i.e.
names sorting between "generatemips" and "guidedfilter" - `gi...`) carry the
types `ProbeMetaData { float4; int; float; float; int }`, `GIRayRecord
{ float3; float; float3; float }`, `CompactBVHNode { int[8] }`, `BLASData`
and `GILightProbeDebug`, and the exe has
`WorldClient2/WorldGlobalIllumination.cpp` and a "Show GI" debug toggle.
From their dispatch shapes:

- 8390637, 8300023 (16x1x1): rays traced in compute against a software BVH
  (`StructuredBuffer<CompactBVHNode>` + `BLASData`), writing hit records -
  no hardware ray tracing (`gx.log` says `rt:None`).
- 8283581, 7970368 (5x5x1): per-probe accumulation of `GIRayRecord`s into a
  probe atlas (5x5 texels per probe) and probe metadata updates.
- 8159093 (4x4x4, 12809 lines): resamples the probes into a camera-aligned
  `RWTexture3D`.
- The material shaders (`material3_mesh_ps`, `material3_wmo_ps`) sample a 3D
  texture with *logarithmic* depth slices
  (`ln(max(z, near)/near) / ln(far/near)`) at the pixel's screen position,
  adjust its saturation by a constant, and **add** it to the lit result.

So "Secondary Lighting" is world-space irradiance probes, updated by
software ray tracing, resampled into a froxel volume each frame and added
as indirect diffuse. Wowee's "Ray traced lighting" row (bounce mode) is the
nearest thing it has; it is too expensive for Jered's laptop and is not in
the Enhanced preset.

**Stand-in** as the "Fill light in shade" row (`fillLight`), in the same
pass as the ambient occlusion (now blended `2 * dst * src`, so it can
brighten): each pixel's ambient gains sky light from above in the zone's
horizon colour (x0.35) and sun bounced off the ground from below
(sun colour x albedo (0.45, 0.42, 0.28) x 0.2), weighted by how far the
face turns up or down, occluded as the ambient is, outdoors only. It costs
nothing over the occlusion pass it rides in.

## 7. Ground clutter

`GroundEffectTexture` is the 1.12 table: 1060 of the 1065 changed rows went
from density 0 to 8 with no doodads (a default, no effect). Doodads gained
`Size_variation 0.67..1.33` and `Rotation_variation 0..360` on every row
(Wowee scatters at 0.80..1.15 and a random yaw) and a `TerrainColorGradingRampID`
that is 0 everywhere. So Enhanced's denser look is its settings
(`groundEffectDensity 48` on retail's scale, `groundEffectDist 200`) and its
models, not new placement data. The Enhanced preset uses the clutter maximum.

Wowee's experimental grass generator ("Grass (experimental)") did not stall
the client (measured before the round-3 change below): switching it on in Goldshire spends 17-32 ms of the main
thread a frame (`beginFrame/grass+misc`) for about a second while it grows
the blades around the player (136834 generated, 34173 after culling),
then costs 0.02 ms a frame. The scripted test driver that switched it on
stopped advancing at that point, which is what read as a stall; the client
kept running. It stays out of the preset until its generation is spread
over frames or moved off the main thread.

Round 3: the generator's slice is now sized by time (`Renderer::
updateGrassPopulation`, `kGrassSliceMs` 2.5): after each slice the cell
count is set to what 2.5 ms bought at the rate just measured, rising by at
most a quarter a frame. Ordinary slices now stay near that budget, but two
costs are not in the cells and remain: the first sight of each ground
effect derives its profile from the effect's doodad models (one 40-80 ms
frame each, seven in Goldshire), and the finished field's upload packs and
stages 175264 blades (10-13 ms, once per rebuild). Side by side the blades
also read as dark stubs against an afternoon sun, nothing like Enhanced's
lit clutter. So the preset carries Enhanced's distance (200 yd, its
groundEffectDist) and the terrain's own density and height for the grass
rows but leaves the generator off; prewarming the profiles at zone load,
packing the upload across frames, and lighting the blades are what it
needs first.

Round 4: the grass is on in the preset. What the round-3 timing missed
was not in the generator at all: the profile table and the finished field
went up through `immediateSubmit` outside an upload batch, which submits and
waits on a fence for the queue - 20-76 ms on the main thread with a frame in
flight, each time a new ground effect was met and each time a field landed.
Both now go in an upload batch of their own (submitted, not waited on), the
field's packing runs on a worker straight into the mapped staging buffer
(`GrassRenderer::setPopulationAsync`, landed by `pollPopulation`), and the
generator's slice also stops on the clock (every 8 cells, at 4 ms). A 2.5 ms
slice was too short: walking outran the build, which restarts when the
player moves 24 yards, and only one field landed in three places. The
blades are lit through when the camera looks toward the sun
(`grass.frag`, strongest straight into it, gone by 60 degrees), so a meadow
glows against a low sun instead of reading as dark stubs.

Walking 20 s each through Goldshire, the fields south of it and the
Barrens at 16:30 (1280x540): nine fields built and landed, the generator's
worst slice 11 ms (76 ms before), no hitch frame with more than 4.2 ms of
grass in it; the hitches left are terrain tiles streaming in after the
teleports. Frame median 16.7-16.9 ms (vsync), GPU 10.1-10.3 ms.

## 8. What the Enhanced preset sets

The "Enhanced" quality preset (`kGraphicsPresets[kEnhancedPresetIndex]`,
`enhancedEnhancements()`): Ultra's rows plus ambient occlusion, the colour
grade, Enhanced's daylight, the Medium light volume at the stock mist
(High's volume cost 2.5 ms more GPU at 1280x540 for little visible
difference side by side; twice the mist was too foggy in play, section 8c),
soft particles (Enhanced's "Depth Based
Opacity"), HD water and sky reflections (its PBR water and SSR); 1900-yard (Enhanced's horizonStart, with the far field past it)
view distance, 500-yard shadows, ground clutter 150%, and FXAA instead of
MSAA (Enhanced's anti-aliasing is CMAA2, a post filter, and the occlusion
pass needs a one-sample depth). Round 2 added the fill light, the sun haze
(with stronger, golder screen-space shafts) and bloom at 0.6: what is
brighter than 0.8 in nine taps of the glow's quarter-size scene, added on
(`fxaa.frag` `bloomAt`). Enhanced ships its Bloom Intensity at the maximum;
its bloom shader was not read, so the knee and spread were set by eye
against its capture. These four rows live on their own settings page,
"Enhancements: Enhanced".

## 8b. Round 3: distance, shaft depth, and what blew out

- **Over-bright backlit faces.** A sunlit trunk beside an off-screen
  afternoon sun went toward white with the stronger screen-space shafts,
  the fill and the bloom stacked on it. The shafts no longer get stronger
  with the haze (only golder), the bloom's addition is capped at 0.25 and
  the fill at 1.5x the light a pixel had.
- **Shaft depth (round 4, done).** With the haze on the froxel mist spans
  800 yards (`VolumetricFog::kFarLong`, same cells) and torch light is
  gathered only in its first 150 yards (`kLocalLightReach` in the inject
  shader); the inject pass measured 0.32-0.33 ms walking Goldshire, Elwynn
  and the Barrens, against 0.3 ms at 400 yards. The round-3 attempt, below,
  had kept the gather everywhere.
- **Shaft depth (round 3).** Tried: the froxel mist over 800 yards instead of 400
  (same cells). Its inject pass went from 0.3 ms to 6.6 ms in Goldshire -
  more of the volume in the dense part of the frustum gathering local
  lights and sampling the shadow map - so it was taken out again. Longer
  shafts need the inject cost per cell cut first (a coarser far half, or
  the light gather skipped past 200 yd).
- **Long view (the reference client)** row (`longView`, on the Enhanced page):
  Enhanced's outdoor fog ends at 18000 yd over a 7000-yard far clip; here the
  frame fog may end at 18/7 of the view distance instead of at it (maps
  below 530, not under water), so the land at the edge of the 2400-yard
  view is hazy rather than gone, and the WDL horizon and the sun haze carry
  the distance past it. The view distance itself stays at 2400, Wowee's
  maximum: Enhanced's 1900-yard horizonStart is the range it draws in full
  detail, which 2400 already covers, and past that both clients draw the
  low-detail horizon. Raising the detailed range past 2400 means a terrain
  load radius of seven or more tiles (225 tiles resident against 169) and
  was not measured here. The far field (below) replaced that plan.
- **Far field (the reference client)** row (`farfield`, on the Enhanced page, E39 in
  `docs/enhancements-plan.md`): Enhanced's distance as it is built - full
  detail to 1900 yd (the preset's view distance now), then the WDL heights
  in the minimap's colours, lit and fogged as the ground, to 7000 yd, with
  the loaded buildings' big outdoor groups to 3000; with the long view the
  fog ends at 18/7 of 7000, Enhanced's 18000. Within the noise on the GPU
  and the CPU in A/B; see E39.

## 8c. Too foggy in play (round 2 as Jered first played it)

Jered's first evening in Goldshire on the round-2 preset (17:52, under
cloud) was a grey wall: low contrast, washed foreground, shade lifted flat.
Four things stacked, all fixed:

1. The froxel mist at twice its density (chosen in a clear afternoon), and
   rain thickening it a further 2.2x. Back to the stock 1.
2. The haze from 40 yards at 0.0006/yd, with a third of its light from the
   fog colour - grey under cloud. Now 0.00022/yd, clear for 120 yards and
   ramping in over the next 400, the fog colour's share 0.35, and thinned
   by 80% in rain or snow; what is left is the sun's forward light, gold
   toward a low sun and little elsewhere.
3. The fill light lifting shade by up to 1.5x, sun or not. Now at most
   1.15x, only by day, and 70% less in rain, so shade stays clearly darker
   than what the sun reaches.
4. The daylight trend brightening the fog colour by 30%: the fog colour
   now keeps Light.dbc's value (the sky bands keep the trend). And the
   colour grade lays at most 0.6 of Enhanced's curve: Enhanced grades a
   tone-mapped linear picture, and on 3.3.5's already bright mid-tones the
   full lift washed them out.

Checked at 8:00, 12:00, 15:00, 17:48, 19:00, 22:30 and in previewed rain
(`enhanced/lab/shots/tod_sheet.png`): clear and contrasty by day, gold
toward the evening sun, dark at night, and rain no greyer than Ultra's.

## 9. Measured cost and the remaining gap

Lab server, Goldshire at 13:00-15:30, 1280x540 window on Jered's laptop,
`WOWEE_FRAME_PROFILE=1` windows of 8-20 s (`PROFILE_AB` lines):

| | GPU ms/frame | notes |
|---|---|---|
| Ultra (MSAA, its highest step) | 13.5-15.8 | scene pass 11.9-14.8 |
| Enhanced preset | 10.0-10.9 | scene pass 7.3 (no MSAA) |
| ambient occlusion + fill pass | 0.17-0.48 | scales with pixels: ~0.7-1.9 at 2560x1080 |
| sun haze pass | 0.06 | |
| FXAA + glow + grade + bloom pass | 1.04-1.06 (Ultra 0.55, no FXAA) | bloom ~0.2, the grade below noise |
| volumetric fog inject, Medium | 0.3-0.5 | High was 3.0 (why the preset stays at Medium) |
| HD water + reflections | 1.6 + 0.3 | where water is on screen |

Screenshots (session scratchpad, `enhanced/lab/shots/`): `side_by_side.png`
(Enhanced's own Goldshire capture beside Wowee's Enhanced preset and Ultra at
15:23, toward the sun and across it), `attr_1ultra.png`,
`attr_2enhanced.png`, `attr_3noao.png`, `attr_5nograde.png` (one view, a piece
off at a time), `aodbg_v2.png` (the occlusion alone), `final_sheet.png`;
Enhanced itself: `enhanced/shots/enhanced_01.png` (Northshire 14:15),
`enhanced_02.png` (Goldshire 15:23).

What still separates the two pictures, largest first:

1. Indirect light. The fill light lifts and tints the shade, but it is the
   same everywhere: Enhanced's probes put green under trees and warm stone
   by a lit wall, and darken a forest floor that the sky cannot see.
2. Light toward the sun. The haze and stronger shafts now give the golden
   glow looking into an afternoon sun, but Enhanced's shafts through the
   canopy are crisper and longer (its fog is shadowed by the cascades
   across the whole depth, Wowee's only in the first 400 yd). Its linear
   HDR sun is now approximated by the HDR lighting row (9b).
3. Ground cover. Enhanced's clutter (same placement data, its own models,
   groundEffectDist 200) is denser and taller than Wowee's at 150%.
4. Per-zone light keyframes: ported for 198 of 209 lights (section 2b);
   ZoneLight polygons where 3.3.5 has no light of its own are not.
5. Draw distance: Enhanced draws terrain to 7000 yd with fog ending at 18000.
   With the far field the Enhanced preset does too (low detail past 1900);
   what is left is the light data's own fog, which in 3.3.5 ends short of
   1900 in most zones (Enhanced's own keyframes, section 2b, are thinner).

## 9b. The HDR/linear path ("HDR lighting (the reference client)")

Built as one row (`hdrScene`, Enhanced page, on in the Enhanced preset only),
in four steps, each with the row off drawing exactly what it drew before:

1. **The scene in RGBA16F** (`VkContext::getSceneColorFormat`). Every pass
   the scene pipelines draw in is made in the scene's format: the scene pass
   with and without MSAA, its continuation, the MSAA colour image, the 1x
   water pass, the water's refraction copy and reflection target, the
   post-process scene image and the glow's boxed-down copy, and a
   scene-format overlay pass for the full-screen passes over the finished
   scene (ambient occlusion, haze, contact shadows, late particles, the late
   minimap). The switch rides the anti-aliasing rebuild
   (`Renderer::decideHdrScene` -> `applyMsaaChange`), which already remakes
   the swapchain, every render pass and every scene pipeline. With it on
   there are no swapchain scene framebuffers: the scene always goes through
   the post-process pass. It stands down under FSR 1/2 (their targets stay
   8-bit) and for the session if RGBA16F cannot be drawn and blitted or the
   off-screen target cannot be made.
2. **A tone map in the final composite** (`fxaa.frag`, `enhanced_tonemap.hpp`),
   ahead of the grade: the identity up to a knee at 0.8 - everything the
   8-bit scene showed below it is shown as before - and above it Reinhard on
   the excess, `k + d h / (h + d)` (d = x - k, h = 1 - k), slope 1 at the
   knee and 1 never reached, per channel, so a hot highlight eases toward
   white instead of clipping flat. It runs at every tap, so FXAA, the stock
   glow and the grade work on the displayed picture as before. What the glow
   and bloom add goes on in the scene's light and the sum is mapped once; the
   bloom also spreads the light above 1 (x0.75 of the Bloom row). The sun's
   disc and glare draw at 2.5x and the haze's light may reach 3 (1.3 in the
   8-bit scene).
3. **Cost** (Iris Xe, 1280x540, Goldshire 15:24, row off and on in turn,
   five 10 s windows each): the final composite +0.49 ms (1.93 -> 2.42), the
   scene pass +0.43 ms (the 16F writes), the frame's GPU time 15.2 -> 16.3
   ms. The first version of the curve cost the composite 1.45 ms, two thirds
   of it a select at every tap; written as one division, with FXAA's edge
   taps mapping the luma alone and the glow blurs mapping their sum once, it
   is a third of that. At 2560x1080 (4x the pixels) about +2 ms composite and
   +1.7 ms scene pass. RGBA16F is twice the scene colour memory; R11G11B10
   would halve the bandwidth but band in the sky (6-bit mantissas).
4. **Linear light in the lit shaders.** With the HDR scene the renderer sets
   `enh_bits::kHdrLight` in `localLightMeta.z`, and terrain, WMO (outdoor
   mode only), M2 and character shaders lift their clamp of ambient +
   diffuse at 1: where sun and sky together pass 1 in a channel the light no
   longer stops there, leaning the hue toward the other channels (the
   yellow-green cast of 3.3.5's noon ground) - it keeps its hue and goes to
   the tone map. The sun's light is 1.25x Light.dbc's by day
   (`kSunLight`, Enhanced's F052 is 1.35-1.5), eased in by the sun sprite's
   height so the night keeps its moonlight.

Side by side (Goldshire 08:00-22:30, rain, Crystal Lake above and under the
water, Stormwind harbour, MSAA 4x; `hdr/lab/final_sheet.png` in the session
scratchpad): by day the picture is 8-10% brighter and warmer, sunlit dirt
ochre rather than yellow-green, the sky round the sun a gradient instead of
a clipped white disc, pixels at 250+ down from 0.2-3.6% of the frame to
0.03-3.2%; night within 2%, rain 6% brighter. `WOWEE_HDR_TUNE="knee,bloom,
sunGain,sunLight"` overrides the four for tuning.

## 10. Not done, and why

- Enhanced's ZoneLight polygons (section 2b), and its keyframes for the 11
  3.3.5 lights with no Enhanced match. The keyframe table itself is generated
  locally (section 2b), never committed.
- Fully linear colour: the HDR row (9b) keeps 3.3.5's display-encoded
  textures and Light.dbc colours and lifts only the clamp; textures are not
  decoded to linear, and the tone curve is the identity below its knee.
- Probe GI (section 6), the layered global fog constants (section 5).
