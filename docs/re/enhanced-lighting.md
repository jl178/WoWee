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

## 2c. Exposure: Enhanced's colours on 3.3.5's path

Jered's first morning on the keyframes (Goldshire road toward a high sun,
`jered_now.png`) blew the top third of the screen to white-yellow, washed
the foliage and took the sunlit cobbles near white. Enhanced's light data is
authored for its linear, tone-mapped pipeline; read raw it stacks with
everything after it. Measured against 3.3.5's bands for the same lights
(every linked light, clear slot, 8:00-17:00):

- **The sun and the ambient** are as bright as 3.3.5's (luminance ratio
  1.00 median for both), but the sun is whiter (`#eabd8a` against
  `#ff8800` at Elwynn's noon). The stock shaders add ambient and sun and
  clamp at 1 per channel, so a fully lit face took (1.33, 1.25, 1.15) ->
  white where 3.3.5's stays warm (1, 1, 0.6). Now
  (`enhanced_palette::linearLightDirect`) the sun is the colour that, added
  to the ambient, gives what adding them as *light* gives - both decoded
  with the 2.2 power, summed, encoded back: (0.98, 0.87, 0.79) at that noon,
  warm and under white. With the HDR scene the sum is left unclamped and the
  sun carries Enhanced's F052 (1.35, eased in by the day intensity) before
  the renderer's own 1.25 by day and the tone map.
- **The sky** is not: its bands are x1.26-1.30 3.3.5's (SkyMiddle 1.26,
  Band1 1.30, Band2 1.29, Smog 1.29, Fog 1.29, CloudSun 1.27, cloud layer
  1.29; 3762 samples each), their 75th percentile 1.30-1.31 and no higher.
  Most keys are 1.12's colour times 1.3 per channel, clipped at 255
  (Westfall's noon SkyMiddle `#3c919e` -> `#4ebdcd`): an exposure Enhanced's
  pipeline takes back. It is divided out as the table is read
  (`enhanced_lights::kSkyExposure`, channels 2-7, 10, 11), and out of the
  trend's sky luminance for the lights without keyframes.
- **The stock glow.** Enhanced's LightParams Glow is 0 on 106 of the 112
  linked profiles (its bloom is separate); 3.3.5's same slots carry 0.5-1,
  and its blur squared over a sky as bright as Enhanced's was most of the
  white sheet at Westfall's noon. Linked lights keep half of it
  (`enhanced_palette::kGlowScale`): with none the picture fell to 0.21-0.23
  mean luminance against Enhanced's captures' 0.32; half gives 0.25-0.26
  with the sky still under white.

And the passes after it, each only under its Enhanced row:

- The sun haze's light is held by its brightest channel (hue kept) at 0.88
  in the 8-bit scene and 1.5 under the HDR tone map (it was 1.3 and 3 per
  channel, which went white toward a high sun) - on a soft knee from 70% of
  the cap, since a hard limit printed a ring in the gradient round the sun.
- The screen-space shafts are eased onto a ceiling of 0.3 a channel under
  the haze (`SunShafts::FrameInputs::cap`); a streak through the canopy is
  well under it, the sheet by the sun is what it stops.
- The bloom's 8-bit halo is screened (times 1 - the picture) instead of
  added, so what is near white takes almost none of it (with the HDR scene
  it is not drawn at all; section 9b, "Glows held small").
- With the HDR scene the lit mid-tones are lifted (x1.3 between 0.2 and 0.8
  luminance, faded out as the brightest channel nears 0.9,
  `enhanced_palette::kHdrExposure`) and lit surfaces carry more chroma (x1.35
  from 0.25 luminance up, easing to x0.8 at the top, `kHdrSaturation`).
  Matched against Jered's live Enhanced captures at Goldshire 9:22, into the
  sun and away from it (lit and shade are the brightest and darkest thirds
  of the world view):

  | | Enhanced | old preset | now |
  |---|---|---|---|
  | lit luminance | 0.44 / 0.59 | 0.43 / 0.49 | 0.46 / 0.51 |
  | lit chroma | 0.62 / 0.65 | 0.46 / 0.53 | 0.63 / 0.69 |
  | shade luminance | 0.22 / 0.26 | 0.21 / 0.25 | 0.20 / 0.24 |
  | lit / shade | 2.07 / 2.27 | 2.08 / 1.95 | 2.27 / 2.14 |

  and the Barrens' noon road at 0.1% of pixels over 0.92 (old preset
  11.1%). Tried and left: a straight exposure (1.25-1.4) reached Enhanced's
  mean but took the Barrens' dirt to a glowing orange; a lift of the low end
  greyed the shade and flattened the picture; the sun past its F052 (1.7)
  put 8.5% of the Barrens' road over 0.92 (with the lift still on).
  (Superseded by section 2d: the lift and the boost are now 1 and the sun
  2x; and Enhanced's sun keeps 3.3.5's north-west bearing - the "east of the
  road" read here was the minimap's character arrow.)
- The final write is
  dithered (a triangular dither of one level) under the Enhanced rows: the
  shoulder and the grade spread a sky's gradient over a few 8-bit levels,
  which showed as rings round the sun.
- The colour grade fades out over 0.6-0.9 of a pixel's brightest channel:
  Enhanced's curve lifts the upper mid-tones of an already tone-mapped
  picture, and on 3.3.5's 0.7-0.9 sunlit ground it drove them to white.

Checked with the HDR row on (the Enhanced preset as Jered runs it): the
Goldshire road into and across the sun at 7:00, 9:00, 12:00, 15:00,
17:30, 19:30 and 22:00, in rain, and Westfall, the Barrens and Duskwood at
noon and 18:30. Near-white pixels (every channel over 0.96) in the largest
blob: Goldshire into the sun 0.6% at most (the disc at 9:00 is 0.25%;
11.3% at noon before), Duskwood noon 1.4% (14.1%), the Barrens 0.2% (39%),
across the sun and at night none. Westfall at noon looking up at the
zenith sun keeps 7% (58.5% before): there the stock picture itself clips
red and green over half the screen (Ultra: 47% of pixels with a channel at
255), and Enhanced's bluer sky makes that clip white rather than yellow.
The comparison sheets are in the session scratchpad (`ovx/shots/`).

## 2d. The sun's place, and what the sun lights

**Where Enhanced puts the sun.** Read from Wow.exe 1.60.1.70862 (file
offsets; no DB2 - LightParams' SunPolar/SunAzimuth are 0 on 873 of 882
profiles, which leaves the engine's own tables). 3.3.5's DayNight tables
(stock_day_night.hpp) are keyframes of {day fraction, radians}; the same
layout sits in Enhanced's `.data`:

- 0x48f7400: the **light**'s theta (225 degrees, all day) and phi (127 at
  midnight and noon, 110 at six) - byte for byte 3.3.5's 0xd390c4/0xd390e4.
- 0x49059b0: the **sun**'s phi: 95 degrees (five under the horizon) at
  05:30 and 20:30, 25 off the zenith from 11:55 to 12:05. 3.3.5's runs 100
  -> 5 -> 100 with the last key at 21:30.
- 0x4905a50: the sun's theta, 45 degrees at every key - as 3.3.5's.
- 0x49059e0 / 0x4905a80: the moon's phi (35 at midnight, 100 from 05:00 to
  21:00), its theta 135 -> 150 -> 165 degrees; 0x4905a10: fades (0 -> 1
  over 05:15-05:30, 1 -> 0 over 20:30-20:45; 06:00-06:30 / 19:30-20:00).

So there is no latitude, tilt or per-map term and no east-west arc: the
bearing is fixed in the north-west as in 3.3.5, the light's direction is
3.3.5's, and what Enhanced changed is how high the sun climbs (65 degrees at
noon, not 85) and when it rises and sets (05:58 and 19:54, not 06:10 and
20:30). The tables are not referenced by a direct RIP-relative load in
`.text` (searched for every disp32 into them), so which one drives the
shadow map is not proven from code; the captures agree with the stock
bearing: at 09:23 (092315) the camera looks east-south-east and the fence
fronts and the character's sides turned toward it are sunlit, the light
from behind; at 15:23 (enhanced_02) the inn view (the same framing as
Wowee's yaw 225 one, where the sun stands) looks into the sun. Earlier
notes here said Enhanced's 9:22 sun stood east of the road; that was the
character's facing on the minimap, not the camera's.

**Ported** (Enhanced's daylight only): `stock_day_night::kEnhancedSunPhi`,
`enhancedSunDirection`; `LightingManager` takes the sun's direction from it
while `enhancedActive_` (the sprite, the leaf light, the haze's phase, the
shafts, the HDR sun's day weight). The light direction is unchanged.

**What the sun lights.** Split by region, the canopy's gap was the tree
crowns (foliage pixels in the top third): luminance +29%, 90th percentile
+41%, chroma +38%, blue/luminance -23% against Enhanced, with the grass
matching. Halving the sun on every M2 moved the crowns by under 0.02:
they are lit by the sky (ambient) alone, at 0.25-0.30 - inside the band
the composite's mid-tone lift (kHdrExposure x1.3, section 2c) and chroma
boost (kHdrSaturation x1.35) act on. Enhanced brightens what its sun
reaches (F052 is a sun intensity); the lift brightened everything at that
luminance. So, with the HDR scene under the Enhanced daylight: the lift and
the boost are 1 (off) and the sun's light by day is 2x Light.dbc's
(enhanced_tonemap::kSunLight, was 1.25). Tried: sun 1.6 and 1.7 left the
sunlit ground 22-25% short; 2.0 with the boost kept took the ground's
chroma +20%.

**And a softer shoulder.** The 2x sun alone took the Barrens' noon road
into the tone curve's shoulder: paler, its cobbles near white (its ground's
99th percentile luminance 0.86 against 0.76 before, 2.1% of it with a
channel over 0.92 against 0.07%) - and the Barrens' sun was already
"sooo bright" to Jered. Enhanced's own tone curve was not read (its
`ffxcolorcorrection` is a plain power, and the shader that tone maps its
linear scene is not among those extracted). Under Enhanced's daylight,
outdoors and by day the knee moves from 0.8 to 0.5
(enhanced_tonemap::kEnhancedKnee, eased by the indoor factor and the sun's
height): the Reinhard shoulder starts at half of white, so what passes it
is compressed where the old curve left it linear up to 0.8. The crowns
(0.25-0.30), the shade and every mid-tone below 0.5 keep their values.
Knee 0.6 left the road's 95th percentile 0.755, above the old build's.

The Barrens' road at noon (lower middle of the view), the old build (the
commit before this one), the 2x sun alone, and now:

| | before | 2x sun, knee 0.8 | now |
|---|---|---|---|
| mean luminance | 0.602 | 0.609 | 0.573 |
| 95th / 99th pct | 0.720 / 0.757 | 0.827 / 0.862 | 0.717 / 0.739 |
| a channel over 0.92 | 0.07% | 2.06% | 0.00% |
| a channel over 0.85 | 73% | 49% | 0.05% |
| chroma | 0.60 | 0.57 | 0.56 |

(The old road's 73% over 0.85 was its red channel flat near the top - a
saturated orange sheet; now the cobbles keep their texture.)

Measured (the three matched Goldshire views; gap is Wowee against Enhanced,
mean of the views; before = the commit before this one, now = Enhanced's
sun table, the 2x sun, no lift or boost, the 0.5 knee):

| region | Enhanced | before | now |
|---|---|---|---|
| crown luminance | 0.25 0.26 0.25 | +26% | +17% |
| crown 90th pct | 0.31 0.32 0.32 | +40% | +20% |
| crown chroma | 0.34 0.33 0.38 | +35% | +19% |
| crown blue / luminance | 0.69 0.71 0.65 | -21% | -13% |
| canopy / ground luminance | 0.78 0.58 0.86 | +28% | +23% |
| canopy sd | 0.08 0.09 0.16 | +27% | +10% |
| ground luminance | 0.36 0.51 0.37 | -17% | -15% |
| ground chroma | 0.70 0.63 0.58 | -1% | +8% |
| lit / shade | 1.91 2.26 2.39 | +9% | -6% |
| all luminance | 0.31 0.38 0.32 | -4% | -4% |
| world 5th pct | 0.18 0.20 0.17 | -18% | -12% |
| grass luminance | 0.28 0.31 0.26 | -11% | -20% |
| grass chroma | 0.29 0.51 0.37 | +2% | -21% |
| sum over 20 regions | | 421 | 319 |

The grass rows are mostly the measure: they count pixels of grass's hue
(55-165 degrees), and with the stronger orange sun the lower band's grass
turns 8-12 degrees toward yellow (82.6 -> 70.8 at the inn, 64.2 -> 53.1 at
the crossroads, 70.5 -> 62.1 at 15:23), so the yellower half leaves the
count. The same pixels in both builds: luminance -3%, +1%, +7%; chroma
-4%, +10%, +8%. What is real is that hue: Enhanced's lower band keeps more
yellow-green (32%, 23%, 17% of its pixels between 45 and 65 degrees
against Wowee's 6-8%), and its grass is green where Wowee's is now olive.

Elsewhere: into the sun at 9:22 4.2% of pixels have a channel over 0.92
(9.4% before; none with every channel over 0.96, 3.4% before); Goldshire
17:51, Elwynn's forest and the sun-behind view: mean -0.012..+0.012, 5th
percentile +0.011..+0.015, 95th -0.046..-0.055, lit/shade 1.89-2.02
(2.22-2.37) - a little flatter at the top. Sheets in the session
scratchpad: `fgap/sheet_k.png` (the Barrens, the inn and into the sun:
before | 2x sun, knee 0.8 | now), `fgap/sheet_g.png` (Enhanced | before |
now), `fgap/sheet_g_regress.png`.

## 2e. Lit and shadowed from Enhanced's sun

Section 2d found that Enhanced's sun tables keep 3.3.5's bearing, so the
light was left on the stock table. The captures say otherwise about what
it lights: at 9:23 Enhanced's crossroads road lies in the sun with tree
shade in patches, golden flagstones (hue 34-37 degrees) beside warm shade;
under 3.3.5's light (north-west, 30 degrees up at 9:22) the trees shade the
whole road, held at 70% by the stock floor - a flat red-brown (28-32).
Lighting the world from the sun's disc instead (by day; Enhanced's sun is
37 degrees up at 9:22, 65 at noon, 17 at 17:51) puts the road in the sun
and dapples it as Enhanced's is. So, under Enhanced's daylight
(`LightingManager`, `enhanced_palette::sunLightDirection`):

- The world's light and its shadows come from the sun, eased in over the
  sun's first 9 degrees so the night keeps 3.3.5's moonlight direction.
- The sun's light is scaled by the ratio of the stock light's vertical
  component to the sun's (`sunIrradianceScale`, 0.5-1.5: x0.67 at noon,
  x0.82 at 9:22, x1.2 at 17:51), so flat ground keeps the light every
  earlier exposure here was set against; `kSunLight` stays 2. Without it
  either the Barrens' noon road (65 degrees against 37) passed its old
  brightness or, at a flat 1.25, the evening and forest views fell 12-16%.
- The froxel mist's forward lobe, now on the sun's disc rather than beside
  it, is tighter (g 0.9, half even) and gives way to even scattering
  within about 20 degrees of the disc (a smoothstep from 25 to 5 degrees,
  85%): with it on the disc the bright air round the sun was a wide
  near-white field (4.6% of the view over 0.92). Radial luminance by 20 px
  from the disc: Enhanced 1.00 0.88 0.65 0.56 0.47, before 0.94 0.88 0.79
  0.73 0.68, now 0.92 0.81 0.72 0.65 0.61 (the mist off: 0.88 0.83 0.67
  0.63 0.58 - the rest is the sky and the glare).
- Two-sided leaf cards seen from their back are lit as their back (the
  sunward cards seen from the shade glowed as if lit), and the leaf light
  is 0.25 (1.3 elsewhere); grass blades take the ground's light.
- The terrain's shadow floor follows the sky's opacity
  (`enhanced_palette::shadowOpacity`), held at 3.3.5's 0.3. Enhanced's shade
  on the crossroads' flagstones keeps 54% of its sunlit colour (opacity
  0.46): that gave the road Enhanced's lit/shade (2.2) and warm shade but
  took the evening and forest views 11-25% darker (0.36: forest -11.5%).

The inn-side crowns that read yellow are Enhanced's own: its obj0 ADT
places the same models at the same spots and scales (ElwynnTreeMid01 at
(-9482.6, 46.0) x0.86 and (-9485.2, 44.9) x1.23), the model's geometry is
3.3.5's, and its textures are the same art at 1024 px (canopy texels
0.39 0.39 0.09 mean in both). Only CanopylessTree01's textures and one
ElwynnTreeCanopy04 were re-authored nearby. So they stay in the measure.

Measured by region (the lit and shaded road, the verge, the crowns, the
far band; `fgap/regions.py` in the session scratchpad) and the merge bars,
against the commit before this one:

| | before | now |
|---|---|---|
| Barrens noon road mean / p95 / p99 | 0.573 / 0.717 / 0.739 | 0.571 / 0.715 / 0.738 |
| into the sun, pixels over 0.92 | 4.22% | 1.31% |
| Goldshire 17:51 (evening) mean | | +3.0% |
| Elwynn forest 10:00 mean | | -6.2% |
| crown chroma gap | +18.7% | +20.8% |
| lit road luminance gap | -19% | -6% (blue/luminance +22% -> +8%) |
| gap sum (section 2d's 20 rows) | 319 | 292 |

Left as it is: the forest view is 6% darker - a slope facing the camera
under Enhanced's higher 10:00 sun (44 degrees against the stock light's
31), which the flat-ground scaling does not hold; no Enhanced capture of
that view says whether darker is right. And the crowns' chroma is about
+20% (+15% with the leaf light off). Sheets in the session scratchpad:
`fgap/sheet_y5.png` (Enhanced | before | now), `fgap/sheet_y5_regress.png`.

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

**Halos (round 5).** Jered's Goldshire road showed dark grey jagged lines
along every branch, leaf-card cluster and the character's arms. Two causes,
both in the shader now: on a silhouette the facing rebuilt from depth is a
guess and the disk counted whatever stood behind or beside as cover; and up
close the 1-yard disk spans a hundred pixels or more, where six fixed taps
print copies of the edges they cross as bands. So a pixel whose neighbours
do not continue its surface (ASSAO's depth edges: the second difference of
the view positions over 0.08 yd + 1.2%/yd) takes no occlusion; a tap more
than 0.35-0.8 of the radius in front of or behind the pixel along the view
(a leaf card, a branch, an arm across the body) counts less, then not at
all; and the disk is held to 3% of the screen height, the radius in yards
shrinking with it, so up close it is contact shading. On the close-ups
(AO on against off) the pixels darkened by over 20% on the character view
fell from 13.9% to 7.4% of the frame, and the bands along the character's
side and the house eaves are gone; a real crease (a branch growing out of
the trunk) keeps its shade.

Round 6: Jered still saw grey patches come and go on the characters and on
tree limbs, and on his live client called it "too strong - it just creates
shadows around things"; the preset leaves it off (0829bd7). On a curved,
thin, moving or alpha-tested surface the facing rebuilt from depth changes
from frame to frame, and the pass sees only depth, so it cannot tell a
character or a leaf card apart. The occlusion now goes only onto
ground-like receivers (a face turned up, from n.z 0.45 to 0.75): what it is
for is seating grass, roots, crates, walls and feet into the ground; a body,
an arm or a branch takes none. And it is subtler: at most a fifth of the
ambient (it was 0.4) over a 0.6-yard disk (1). AO on against off, Goldshire
10:00: pixels darkened over 20% fell from 5.1-12.7% to 2.2-11.0% of the
frame (character 5.1 -> 2.2, crates 5.9 -> 2.7; the tree view is mostly
leaves moving in the wind between the two shots), and side by side on and
off are hard to tell apart. Ten-frame sequences of the character walking
and of trees in the wind show no grey patches at any frame.


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

## 5b. The air: Enhanced's global fog layer, decoded

What lifts Enhanced's dark tones from about twenty yards out (section 9,
gap 6) is not its distance fog. Read from the shaders and the data:

**The distance fog** (`PSFog`, 14 float4 per fog, read by
`material3_mesh_ps`/`material3_wmo_ps` for every lit pixel, permutation 26
of 5446721): two paths, picked by row 1.x. The legacy one is
`T = saturate(((row1.y - d) * row1.z) ^ row1.w)` - 3.3.5's curve, end,
1/(end - start) and power. The new one is exponential,
`T = exp(-row4.x * max(d - row0.x - row0.w, 0))` with a height fog
(a plane, row 2, faded by row 3.w, cubic coefficients in rows 9-11), the
colour lerped from row 6 to row 13 (EndFogColor) by distance and a sun
fog colour toward row 8's direction. Which path and every row are filled on
the CPU from LightData's fog columns; Enhanced's outdoor keys leave
FogDensity, the height fog, MainFogStart/End, EndFogColor and SunFogColor
at 0 (Elwynn's LightParams 7748 on every key) and set only FogEnd (18000
at noon, a single key without one at 8:00) and FogScaler (0.08-0.25). So
the distance fog starts thousands of yards out: none of it near the camera.

**The global fog layers** (`volumefogglobalfog.bls`, 6674334, all eight
permutations the same core; `GlobalFogParams`, nine float4 a layer from
cbuffer row 23): per froxel at depth z and per layer,

    coverage   = saturate((z - start) / sliceThickness)        row+0.y start
    t          = saturate(max(z - start, 0) / row18.x)
    depth      = coverage * sliceThickness * density            row+0.z density
                 * (1 + k1 * t^k0)                              row+8.xy
               * upper fade saturate(exp2( k*log2e*(h - upperH))) (flag 64, row+2.xy)
               * lower fade saturate(exp2(-k*log2e*(h - lowerH))) (flag 128, row+2.zw)
    inscatter += (emissive + scatter * phase(cos) * intensity) * depth
                                     row+1.xyz emissive, row+5.xyz scatter,
                                     row+1.w intensity, row+0.w the phase row
    phase LUT u = 1 - (0.5 - 0.5 cos)^(1/3)

The layer's values come from **LightParams' new columns**: field 031 is
0.001 on 370 of 882 profiles and 0.002 on one, and on all but three of
them field 035 is 1 (0 on 508 profiles that have no 031) - a switch and a
density per yard. Field 037 (0.7 or 0.8 on 856) has the spread of an
anisotropy and 036 (1, 8, 16...) of the distance curve's exponent; they are
not used here. Of the 112 Enhanced profiles the 3.3.5 lights link to, 108
carry the layer at 0.001/yd (Elwynn's 7748 among them).

Its colour was the open question. LightData field 055 (a Enhanced-only
colour, set on 527 keys of 44 profiles, every one with field 035 on) is
pale green-grey at Elwynn's 8:00 (`#b6cbb0`), white-blue at noon, rose at
dusk: it reads like an air colour. Tried as the layer's light it measured
worse: the added light came out yellow (red and green +0.046, blue +0.018
on the inn view) once the HDR mid-tone lift and chroma (section 2c) had
taken the brighter canopy, and the canopy's blue fell. The light's fog
colour (SkyFogColor, channel 7, Enhanced's own `#73a29d` at 8:00, cool
blue-green) closed the floor and raised the canopy's blue. So 055 is left
unread; the layer scatters the fog colour.

**Ported** (the "Sun haze (the reference client)" row, Enhanced preset only):
`tools/enhanced_light_gen.py` writes `WFL2` tables - `WFL1` plus each
profile's density (field 031 where field 035 is on); `enhanced_lights::parse`
reads both, a `WFL1` table meaning no air. `LightingManager` carries it as
`LightingParams::airDensity`, blended between lights like the rest, 0 on
3.3.5's own profiles, under water and outside Enhanced's daylight. The haze
pass (`enhanced_haze.frag`) takes it in its spare push vec4 and lays the fog
colour's light over every opaque pixel, `T = exp(-od)` with the density
thinning with height as the haze's own (220-yard scale above the ground
under the player), from the camera out, thinned by weather as the haze is,
outdoors only and not on the sky (the dome has its fog band). The haze,
farther (it starts 120 yards out), composites behind it. One more `exp` in a
pass that already runs. Regenerate the local table with the command in
section 2b's tools line (`enhanced_light_gen.py --dbc ... --enhanced ...
--out <data>/override/wowee/enhanced_lights.bin`); a client older than this
refuses a `WFL2` table and falls back to the averaged trend, so the two go
together.

Measured (the three matched Goldshire views; one build, the `WFL1` table
then the `WFL2`; gap is Wowee against Enhanced, mean over the views):

| region | Enhanced | no air | air | gap no air | gap air |
|---|---|---|---|---|---|
| world 5th pct luminance | 0.18 0.20 0.17 | 0.14 0.17 0.12 | 0.15 0.19 0.12 | -24% | -18% |
| foliage 10th pct luminance | 0.21 0.20 0.19 | 0.17 0.17 0.12 | 0.22 0.21 0.13 | -24% | -6% |
| far band 5th pct | 0.18 0.20 0.17 | 0.13 0.21 0.11 | 0.15 0.23 0.12 | -18% | -12% |
| canopy blue / luminance | 0.72 0.67 0.70 | 0.61 0.43 0.71 | 0.63 0.48 0.71 | -17% | -13% |
| lit / shade | 1.91 2.26 2.39 | 2.50 2.28 2.63 | 2.39 2.17 2.51 | +14% | +9% |
| all luminance | 0.31 0.38 0.32 | 0.31 0.35 0.27 | 0.33 0.37 0.28 | -7% | -4% |
| foliage 90th pct luminance | 0.33 0.35 0.33 | 0.42 0.46 0.34 | 0.47 0.50 0.37 | +21% | +33% |
| canopy / ground luminance | 0.78 0.58 0.86 | 0.84 0.95 0.73 | 0.91 1.02 0.78 | +19% | +28% |
| foliage chroma | 0.34 0.36 0.40 | 0.43 0.49 0.39 | 0.45 0.50 0.42 | +19% | +24% |

The floor closes by a quarter to three quarters; what it costs is the lit
canopy, which was already too bright for Enhanced's and takes the veil's
light on top (then the HDR mid-tone lift and chroma). Over all fifteen
measured regions the summed gap moves 313 -> 310: the air is Enhanced's, the
canopy's brightness is a separate gap. Elsewhere (Goldshire 17:51, the
Barrens at noon, Elwynn's forest at 10:00, Goldshire with the sun behind)
the frame's mean rises 0.005-0.015, its 5th percentile 0.010-0.018, its
contrast (sd) falls 0.001-0.005. Sheets in the session scratchpad:
`fgap/sheet_c.png` (Enhanced | no air | air).

**The far canopy's colour.** Enhanced's far trees read green-grey where
Wowee's read warm yellow. By screen band (foliage pixels; top = the near
crowns, far = the tree line), red and green to luminance are the same in
both - 1.02 : 1.03 in Enhanced, 1.01-1.03 : 1.03-1.04 in Wowee - so it is
not the leaf textures or a tint, which would move red against green. What
differs is blue alone: 0.71-0.78 of luminance in Enhanced's near crowns
against 0.53-0.55 in Wowee's, 0.65-0.74 against 0.62-0.66 at the tree
line. The air takes the tree line's blue up by 0.02-0.04 and the near
crowns' by 0.01-0.04; most of the rest is at twenty yards, too near for
any air - the leaves' own light: Enhanced's are lit more by the sky's blue
ambient than by its orange morning sun, which at Goldshire 9:22 stands
elsewhere in Enhanced (section 2c) and leaves the camera-facing leaves in
the shade.

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

## 6b. Bounce light ("Bounce light (the reference client)")

The fill light (section 6) is the same colour everywhere. What Enhanced's
probes add varies with place: under its canopies the shade is darker and a
muted green, beside a sunlit wall or open ground it is warm, a forest floor
the sky cannot see is dim. Those colours change over yards, not pixels, and
depend on what is round a point, not on what is on screen beside it - so
the stand-in is a world-space grid rather than a screen-space gather, which
would rim every silhouette and change as things leave the view.

**The grid** (`include/rendering/bounce_grid.hpp`, `BounceGrid` in
`bounce_grid_gpu.hpp`): 256x256 cells of 2 yards round the camera, three
RGBA16F layers, baked from what is already loaded:

- the ground's colour: the map's minimap tile under the cell (the only
  colour the client ships for land from above, as the far field reads it),
  its hue and 0.8 of its chroma kept, its brightness held within 0.65-1.35
  of the fill's own grass-and-dirt colour so a dark or bright picture cannot
  swing the shade;
- the canopy: each foliage model taller than 5 yards a disc of 0.65 of its
  widest half-extent, densest at the middle (0.85, or 0.3 for a tree with no
  leaf cards), crowns over one cell combining as each lets light through;
  the leaves' colour (their cutout textures' mean, `VkTexture::averageColor`)
  and the crown's foot (a third of the way up) and top. Seen with the grid
  drawn over the picture (`WOWEE_BOUNCE_DEBUG=1`), the full extent covered
  Goldshire's open square between the oaks;
- walls: exterior WMO groups (MOGP 0x8) under 48 yards a side and over 3
  tall - houses, towers, walls; a city block's or a street's group is mostly
  air and is left out - with their foot and top.

Heights are carried 8 yards past each crown and wall, so a filtered read
across an edge fades the cover, not the heights. The camera's position
decides the grid's corner, snapped to cells, so a rebuild puts every cell
on the same ground: it is rebuilt when the camera is 48 yards from the
middle or the loaded world has changed (checked every 2 s), gathered on the
main thread (0.5-1.3 ms over 50-95 thousand M2 instances) and baked on a
worker (9-19 ms), then copied outside any render pass before the ambient
pass reads it.

**The shading** (`ambient_occlusion.frag` built with `BOUNCE_GRID` as
`ambient_bounce.frag.spv`; the stock build is byte-identical): at each
pixel's world position the fill's terms take the grid's values -

- the canopy over the point counts fully below the crowns' foot and not at
  their top; under it the sky's fill falls by 0.7 of the cover, the ground's
  bounce by 0.7 (the ground below is in the leaves' shade), and the ambient
  itself is 0.9 as bright (0.8 at first; section 6c) and tinted by the light under the leaves: the
  sky's hue and the leaves' (35%) at unit brightness, so that green comes
  from the sky's light through the leaves rather than from their yellow
  texture;
- the ground bounce is the sun times the cell's own ground colour;
- a wall within 3 yards on one of four sides, spanning the pixel's height,
  whose face toward the pixel turns to the sun, sends 0.25 of the sun in a
  stone-and-timber colour to a face turned toward it.

A face the sun lights keeps what the plain fill gave it (blended over 5-40%
of its light being the sun's), so the row never brightens or darkens a lit
surface; a pixel past that is not read at all. The outer tenth of the grid
fades to the plain fill. Indoors and at night it is the plain fill (both
already scale by the indoor factor and the sun's height). It runs the
ambient pass with or without the fill row; off is stock (no grid, no
pass for it).

Against Jered's live captures (the darkest third of the world view as
shade):

| view | | Enhanced | row off | row on |
|---|---|---|---|---|
| Goldshire 9:22 into the sun | shade luminance | 0.216 | 0.223 | 0.211 |
| | shade r : g : b / luminance | 1.06 1.02 0.73 | 1.04 1.08 0.47 | 1.04 1.09 0.45 |
| Goldshire 9:22 sun behind | shade luminance | 0.243 | 0.236 | 0.214 |
| | shade r : g : b / luminance | 1.05 1.03 0.71 | 1.09 1.06 0.44 | 1.05 1.09 0.41 |
| Elwynn forest, off the road | shade luminance | - | 0.224 | 0.206 |
| | shade r : g : b / luminance | - | 1.00 1.09 0.51 | 0.95 1.12 0.50 |
| Duskwood, Darkshire's road | shade luminance | - | 0.178 | 0.176 |
| the Barrens, noon | shade luminance | - | 0.368 | 0.360 |
| Stormwind, Trade District | shade luminance | - | 0.180 | 0.172 |

Indoors (the Lion's Pride) 0.5% of pixels change by more than 8/255, at
night 2-4% (the wind's own noise between two shots). Stormwind's street
darkens where it lies under the Trade District's tree (the grid drawn over
it shows the crown's disc there); its buildings are groups of the city's
one WMO, mostly over 48 yards, so the walls there are not in the grid.

The shade under the trees moves toward Enhanced's: darker and less golden
(red down against green). What it does not close is the blue in Enhanced's
shade (its shade is 0.71-0.73 blue to luminance, Wowee's 0.42-0.47 with or
without the row): 3.3.5's leaf and grass textures have almost no blue for
any light to bring out, and Wowee's later passes (the HDR saturation, the
grade) add chroma; that is a grading question, not a light one (section 6c).

Motion (Goldshire 10:00, ten frames 0.25 s apart, the row off then on):
walking under the oaks and a still camera on trees in the wind with the
character in front show no rim, patch or flicker; the mean frame-to-frame
change is the same with the row off and on (walk 3.1-29.7 against
3.7-29.5 /255; wind 2.4-3.3 against 2.3-3.2).

Cost (Iris Xe, 1280x540, Goldshire 9:22 looking across the canopy, the row
off and on in turn, ten 6 s windows each): the ambient
pass 0.38 ms median off (the fill alone) and 0.69 ms on, +0.31 ms; the
frame's GPU 10.82 -> 10.96 ms median. The first version read the grid at
every pixel and through the four wall looks unconditionally (+0.38 ms);
sunlit pixels no longer read it and the wall looks run only where a wall's
carried heights say one stands near. The sheets are in the session
scratchpad (`gi/sheet_c1.png`, `gi/sheet_c2.png`, `gi/seq_walk.png`,
`gi/seq_wind.png`).

## 6c. The shade's hue

Enhanced's shade in Jered's live Goldshire 9:22 captures is a near-neutral
warm grey: the darkest third of the world view 1.05 : 1.03 : 0.72 red :
green : blue to luminance, chroma (max - min) / max 0.46-0.57. Wowee's
under the Enhanced preset was a saturated yellow-green, 1.05 : 1.09 : 0.41
and chroma 0.67-0.72, with the sunlit third matching (chroma 0.64-0.70
against 0.62-0.65).

What it is not. Enhanced's light data at that hour already gives an ambient
the colour of a blue sky (0.40, 0.51, 0.60; the sun 0.73, 0.40, 0.13), and
Wowee shades with it: the yellow is 3.3.5's leaf, grass and dirt textures,
which carry almost no blue for any light to bring out. Tinting the shade's
light toward the sky's own bands did not close it - tried in the bounce pass,
the shade's light moved toward SkyMiddle's hue with its luminance kept: the
bands are cyan beside the ambient (0.21, 0.59, 0.75), and the shade went
green (red/luminance 1.05 -> 0.78, blue only 0.45 -> 0.50). The grade's
curve is per channel and near the identity below 0.3 (section 3), and the
HDR chroma boost already starts at 0.25 luminance, so neither was what
warmed it.

What it is: Enhanced's shade is Wowee's with a little over half its chroma,
round a slightly cooler grey. So, in the final composite under the HDR
scene and the Enhanced daylight (`fxaa.frag`, beside the lit chroma boost),
by day (`enhanced_palette::dayShare`, the sun's sprite 0-0.15 up) and
outdoors:

- pixels below 0.18 luminance keep `kHdrShadeChroma` = 0.6 of their chroma,
  easing to all of it at 0.38 - below where the lit boost reaches full;
- round a grey that leans to the ambient's blue against the mean of its red
  and green (`shadeTint`, `kHdrShadeSky` = 0.6 of the way from white): the
  ambient's own hue leans green and turned the shade a green grey (red 0.98
  to luminance);
- only where the pixel is warm (blue short of red and green by up to 0.1):
  Duskwood's teal shade keeps its colour (unchanged, 0.32 : 1.26 : 1.46).

And the canopy's ambient is 0.9 as bright under the crowns (it was 0.8):
with the sun behind, the shade had gone darker than Enhanced's (0.214 against
0.243).

| view (darkest third) | | Enhanced | before | now |
|---|---|---|---|---|
| Goldshire 9:22, the inn (Enhanced's 09:22 framing) | luminance | 0.216 | 0.192 | 0.205 |
| | r : g : b / luminance | 1.06 1.02 0.73 | 1.06 1.06 0.52 | 1.04 1.02 0.79 |
| | chroma | 0.46 | 0.68 | 0.38 |
| Goldshire 9:22, the crossroads (09:23:15 framing) | luminance | 0.243 | 0.235 | 0.253 |
| | r : g : b / luminance | 1.05 1.03 0.71 | 1.16 1.04 0.40 | 1.12 1.01 0.62 |
| | chroma | 0.57 | 0.72 | 0.46 |
| Goldshire 9:22, into the sun | luminance | 0.216 | 0.211 | 0.222 |
| | r : g : b / luminance | 1.06 1.02 0.73 | 1.04 1.09 0.45 | 1.01 1.05 0.72 |
| Goldshire 9:22, sun behind | luminance | 0.243 | 0.213 | 0.230 |
| | r : g : b / luminance | 1.05 1.03 0.71 | 1.05 1.09 0.41 | 1.03 1.05 0.68 |

The sunlit third is where it was: chroma 0.64-0.69 (0.64-0.68 before),
luminance 0.45-0.55 (the same within 0.01); lit/shade 2.10-2.27 on the two
matched views (2.24-2.38; Enhanced 2.07-2.27). Elwynn's forest floor goes
from 0.25 to 0.49 blue (forest_a) and 0.50 to 0.75 (forest_b); the Barrens'
noon shade, mostly above the band, 0.58 -> 0.62; Duskwood, the Lion's Pride
inside and Goldshire at 22:30 unchanged within a shot's noise. Cost below
the profiler's noise (the composite 1.95-2.36 ms windows either way, Iris
Xe 1280x540). Sheets in the session scratchpad: `shade/sheet_goldshire.png`
(Enhanced | before | now), `shade/sheet_regress.png`.

## 6d. The ambient turned toward the sun

Measured against Jered's three Goldshire captures (09:22 at the inn and the
crossroads, 15:23 at the inn), region by region at matched views, the
largest gaps left under the Enhanced preset were all the canopy's: it was as
bright as the road under it (canopy/ground luminance 0.97-0.99 against
Enhanced's 0.58-0.86), more saturated (foliage chroma 0.48-0.51 against
0.34-0.40), its sunlit leaves brighter (90th percentile 0.44-0.46 against
0.33-0.35) and yellower (canopy blue/luminance 0.42-0.62 against 0.67-0.72).
Turning the leaf light off moved none of it (within 0.02); the HDR mid-tone
lift does most of the brightening, but taking it out darkens the road with
the leaves.

**What Enhanced's shader does.** `material3_mesh_ps` (5446721, permutation 26
read in full) lights a model with `PSGlobalLight`, eleven float4: rows 0-2
the hemisphere ambient (sky, horizon, ground), row 3 the light's direction,
row 6 its colour, row 10 the up axis. The ambient is the horizon colour
lerped toward the sky's by `N.up` above the horizon and toward the ground's
below it, and then **scaled by a half-Lambert ramp toward the sun**:

    k        = mix(away, toward, 0.5 + 0.5 * dot(N, -lightDir))   (N.L signed)
    lit      = ambient * k + sunColour * sat(N.L)
    colour   = albedo * lit

with `(away, toward)` = (0.4562, 1.2333) on its linear path and (0.7, 1.1)
on its display-encoded one - the same ramp (0.4562^(1/2.2) = 0.700,
1.2333^(1/2.2) = 1.100). `material3_wmo_ps` (5446718) carries the identical
block (twelve permutations with both pairs); the terrain's
`terrainlightmap` does not. So in Enhanced a model's side turned from the
sun takes 0.7 of the ambient and its sunward side 1.1: a tree's crown reads
round, its far side and underside deep, and the leaves' own sunlit chroma
sits on a darker body.

**Ported** (Enhanced preset only): `enh_bits::kEnhancedAmbient` (128), set by
the renderer while Enhanced's daylight lights the frame
(`LightingManager::isEnhancedActive`: the Daylight row, maps 0 and 1, above
water); `m2.frag` (outdoor models), `wmo.frag` (the outdoor light mode) and
`character.frag` (units outdoors) scale their ambient by
`mix(0.7, 1.1, 0.5 + 0.5 * N.L)` (`enhanced_palette::ambientRamp`), the
display-encoded pair, since Wowee's lit shaders work in display-encoded
light. The hemisphere itself is not: Elwynn's default profile (LightParams
7748) leaves Horizon/GroundAmbient at 0 on every key. `WOWEE_ENHANCED_AMBIENT=0`
turns it off for side-by-side work. One mix and multiply per lit pixel, no
texture or shadow reads (not profiled separately).

Measured (the three matched views, the ramp off then on, one build; gap is
Wowee against Enhanced, mean over the views):

| region | Enhanced | off | on | gap off | gap on |
|---|---|---|---|---|---|
| canopy / ground luminance | 0.78 0.58 0.86 | 0.99 0.97 0.86 | 0.85 0.95 0.74 | +32% | +20% |
| foliage chroma | 0.34 0.36 0.40 | 0.48 0.51 0.46 | 0.41 0.49 0.38 | +32% | +17% |
| foliage 90th pct luminance | 0.33 0.35 0.33 | 0.44 0.46 0.38 | 0.41 0.46 0.33 | +29% | +20% |
| canopy blue / luminance | 0.72 0.67 0.70 | 0.58 0.42 0.62 | 0.63 0.43 0.72 | -23% | -15% |
| world 5th pct luminance | 0.18 0.20 0.17 | 0.14 0.18 0.14 | 0.13 0.17 0.11 | -16% | -26% |
| foliage 10th pct luminance | 0.21 0.20 0.19 | 0.19 0.19 0.16 | 0.16 0.17 0.12 | -10% | -25% |
| lit / shade | 1.91 2.26 2.39 | 2.37 2.19 2.38 | 2.54 2.29 2.65 | +7% | +15% |
| ground luminance (terrain, unchanged) | 0.36 0.51 0.37 | 0.31 0.37 0.32 | 0.32 0.37 0.30 | -18% | -19% |

The canopy rows close by a third to a half. What opens is the floor: the
darkest tones of models go darker, where Enhanced's are lifted - and
Enhanced's floor is lifted only at distance (the near road's 5th percentile
is Wowee's within 0.01; the canopy and far band are 0.04-0.06 brighter), by
a cool grey veil (section 9, gap 1 below). Elsewhere (Goldshire 17:51, the
Barrens at noon, Elwynn's forest at 10:00) the frame's mean moves under
0.01 and its 5th percentile 0.006-0.016 down. Sheets in the session
scratchpad: `fgap/sheet_f.png` (off | on), `fgap/final_sheet.png`.

## 6e. The sky over each point

The gaps left after 6d were the shade's: Enhanced's shaded road is the
ambient's colour (a cool, brownish grey), Wowee's kept 70% of the sun's
orange (3.3.5's shadow floor), and under Enhanced's trees the shade is darker
than in the open. Enhanced's probe GI (section 6, giQuality 3) is what makes
that difference: it adds light where the sky and lit surfaces are seen, and
little under a canopy. The bounce grid's crown discs (6b) could not stand in
for it - they mark most of Elwynn as covered, so a fill keyed on them lifted
nothing.

All of it is the "Sky shade (the reference client)" row (`skyShade`, Enhanced page,
on in the Enhanced preset), which needs the bounce light on. It flips live.
Off is the bounce light exactly as it was: the pass's `BOUNCE_GRID` build
(`ambient_bounce`/`ambient_fill.frag.spv`, byte for byte the commit before;
the sky code is a third define, `SKY_FIELD`, in `ambient_sky`/
`ambient_sky_fill.frag.spv`), no height field made or drawn, and the
terrain's floor - the terrain's Enhanced light is its own bit,
`enh_bits::kSkyShade`, set only while the pass is the sky build. Jered keeps
the warm classic shade that way; on, the shaded road and the evening road go
grey-blue.

**Enhanced's terrain light.** Its terrain pixel shader (FDID 2990783,
permutation 0) lights the ground as `material3_mesh_ps` lights models: the
hemisphere ambient by the half-Lambert ramp toward the sun (6d's
0.4562/1.2333 or 0.7/1.1, no normalisation), plus the sun by `max(N.L, 0)`.
Its shadow comes in two ways, picked by a constant (the w of the first
row of one of its constant buffers): with it set, the sun is unshadowed and
the whole colour is scaled by `0.7 + 0.3 * shadow` (3.3.5's floor); with it
clear, the shadow scales the sun alone and there is no floor. Enhanced's
captures match the second (lit that way, Wowee's shaded road's red and blue
to luminance come within 2% of Enhanced's), so under Enhanced's
daylight `terrain.frag` does that: ambient by the ramp, shadow on the sun
only. One change from Enhanced: the ramp is divided by flat ground's value
under the same sun, so level ground keeps the ambient it had and only slopes
turn darker or lighter. Unnormalised, the Barrens' noon road (sun 65 degrees
up, ramp 1.08) went over its brightness bar (0.576/0.727/0.749).

**The height field** (`sky_height_field.hpp`, `sky_field.hpp`): three depth
maps, 512 texels over 256 yards round the camera (half a yard a texel), drawn
the way the sun's shadow map is - the same casters, pipelines and depth
format, through `beginShadowTarget` - with an orthographic matrix from
straight above:

- the tops of buildings and doodads (WMO and M2 static casters, alpha-tested
  leaves) seen from above;
- their undersides, seen from below (the same square, depth from the
  bottom): with only the tops, a crown 8 yards up was a solid column to the
  ground, and every point between two oaks saw no sky;
- the terrain alone, for where the ground under a point is.

Two sets, front and back: the back is redrawn when the camera has walked 32
yards from the front's middle, or every 2 s, in two parts on two frames (the
ground and the tops, then the undersides) and then the two swap. Each part
asks the doodads once: `M2Renderer::renderShadow` keeps one static caster
list and writes its matrices to the frame slot's caster buffer, so a second
matrix in the same frame would overwrite what the first draw reads. For the
same reason a part is drawn only on a frame the sun's world map is not
(`Renderer::renderSkyField`, between the world map and the unit map in the
shadow pass), and with the world map's centre and radius, which keeps the
doodads' candidate list. The world map's next part rebuilds the static list
for its own matrix, as it would for a moved sun.

**Visibility** (`ambient_occlusion.frag`, the bounce build): from each
pixel's world position, eight directions, three reaches each (1.5, 5 and 12
yards). At each reach what stands there is a slab from its underside to its
top; it hides the elevations between the two, and `sin^2` of the top's
elevation less `sin^2` of the underside's is that band's share of the slice
of a cosine-weighted hemisphere. A crown overhead hides the steep sky and
leaves the sky under its edge; a wall or trunk, which stands on the ground,
hides everything up to its top. The eight directions are shared over the
2x2 quad (each pixel looks along two; the quad pools them across depths
within 0.3 yards plus 5% of the distance, before any pixel of it
discards), so a pixel pays twelve gathers. The term fades out between
about 95 and 115 yards from the field's middle, and leaves (below) skip it.
`WOWEE_SKY_DEBUG=1` darkens each pixel by it; `WOWEE_SKY_FIELD=0` leaves
the field undrawn and the term out.

**Using it**, under Enhanced's daylight and outdoors (the light each surface
shader gave the pixel is taken apart again, as the occlusion pass does):

- **Leaves** - a point well off the ground inside something whose underside
  is well above the ground there (a crown, not a trunk or a wall, which stand
  on the ground; not a unit, which is in none of the maps) - keep the bounce
  light (6b, 6c) as it was. The crowns' colour was matched with it, and the
  warm bounce stays out of them: given the new fill, trunks and crowns went
  brown and the crown rows lost 5-8 points.
- **Models and units** keep the bounce light too, their ambient scaled by
  `mix(0.9, 1, visibility)`.
- **The ground**: the ambient by `mix(0.7, 1, visibility)`, and in shade
  (the sun under a fifth of the light) a fill for the probe GI: the fog
  colour x 0.2 (the sky at the horizon), and the sun off the ground round
  it - `sun x ground colour x 0.6`, the bounce grid's ground colour at 60%
  of its chroma, x 0.8 to 1 by how much of the ground on two rings 3 and 8
  yards out the world shadow map says is lit (a point on each, turned by
  the pixel's place in its quad), x (sin of the sun's height / sin 37
  degrees)^2, 35% of it in the sky's hue (Enhanced's shade keeps the
  ambient's colour). The square: the light's colour is already scaled to
  hold level ground's light as the sun sinks
  (`enhanced_palette::sunIrradianceScale`), and with the plain ratio
  Goldshire's evening road came out 12% too bright. All of the fill only
  where the point sees open sky (`smoothstep(0.3, 0.9, visibility)`), none
  under a canopy; with it the shade may lift to 2.5x, where the plain fill
  stops at 1.15x.

The forest (Elwynn 10:00, a meadow wholly in tree shadow) and Goldshire's
evening road are the two views the fill is held by: with the shadow on the
sun alone and no fill they were 16% and 10% darker than before (branch
`wip-enhanced-ground`), and Enhanced's own captures do not show them.

Measured against the commit before this one, rebuilt and run through the
same harness (`before`), and Enhanced (gap, mean of the three matched
Goldshire views; the bars as set for this change):

| | before | now (mean of 3 runs) | bar |
|---|---|---|---|
| Barrens noon road mean / p95 / p99 | 0.573 / 0.715 / 0.738 | 0.533 / 0.689 / 0.713 | 0.571 / 0.715 / 0.738 |
| into the sun, pixels over 0.92 | 1.25% | 1.27% (1.25-1.30) | 1.31% |
| Goldshire 17:51 (evening), mean | +0.0% | -4.2% (-4.0..-4.4) | within 5% |
| Elwynn forest 10:00, mean | -0.2% | -2.8% (-2.8..-2.9) | within 5% |
| crown chroma gap | +18.8% | +20.1% (17.4-23.2) | +21% |
| gap sum (section 2d's 20 rows) | 277.5 | 286.2 (280.2-294.7) | 292 |
| region sum (2e's 23 rows) | 282.0 | 270.2 (264.9-279.0) | 280 |

(The evening and forest changes are against the earlier `y5` shots, as the
bars were set; `before` reproduces them within 0.2%. Its gap sums come out
lower than `y5`'s 291.6 and 291.3: single runs move by 5-15 points on the
sums and 3-6 on the crown chroma - NPCs walk through the views, the wind
turns the leaves - so the bars are read on the mean of three runs. One of
the three went over the crown chroma and gap sum bars on its own.)

Region by region (gap against Enhanced, mean of the three views):

| region | before | now |
|---|---|---|
| road shade luminance | +18.5% | +12.8% |
| road shade chroma | +19.2% | -0.6% |
| road shade blue / luminance | -14.0% | +1.2% |
| road shade red / luminance | +12.0% | -0.4% |
| verge blue / luminance | -29.2% | +7.4% |
| verge chroma | +15.0% | -15.4% |
| grass luminance | -23.4% | -12.1% |
| road lit blue / luminance | +7.8% | +27.5% |
| road lit chroma | -4.9% | -16.8% |
| ground chroma | +9.8% | -11.3% |
| far chroma | -0.0% | -10.5% |
| world 5th percentile | -12.7% | -17.7% |

The shade is Enhanced's colour now - the ambient's, not 3.3.5's orange floor -
and darker under the canopy than in the open. What got worse is the "lit
road": the brightest third of the road pixels now takes in the lifted open
shade beside the sunlit cobbles (the inn view's left half), which is bluer
than sunlight; the picture's chroma overall, 3.3.5's warm shade having been
much of it; and the darkest tones, under the crowns. The shade's luminance
is still 13% over Enhanced's, and Enhanced's lit/shade on the road (1.9-2.3)
is not reached (1.5-1.7, as before): the evening and forest views hold the
fill up.

Cost (Iris Xe, 1280x540, the perf views at 12:00, the commit before this one
and this one in turn; `perf/` in the session scratchpad):

| view (12:00) | ambient pass before | now | frame GPU change |
|---|---|---|---|
| Goldshire crossroads | 0.49 ms | 0.90 ms | +0.41 ms in the pass (+0.71 frame, water +0.25 of it) |
| Crystal Lake | 0.48 ms | 0.89 ms | +0.41 ms (+0.26 frame) |

(Two rounds each, three 7 s windows a round, no other client on the GPU;
`perf/logs/perf-pk6-*.log`.) The height field's own draws average 0.005 ms
a frame (two parts every 2 s or 32 yards). The first version - eight
directions at four reaches at every pixel, six ring points - cost the pass
+1.1 to +1.4 ms; sharing the directions over the quad, skipping leaves,
three reaches and two ring points took it to this. On the CPU, each part
rebuilds the doodads' static caster list once (and the world map's next
part rebuilds it back), within the noise of the frame times measured.

Sheets in the session scratchpad: `sv/sheet_matched.png` (Enhanced | before |
now, the three Goldshire views), `sv/sheet_regress.png` (before | now: the
evening, the forest, the Barrens, into the sun, away from it).

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
Opacity"), HD water and sky reflections with the "Water (the reference client)" row
over them (its PBR water and SSR; enhanced-water.md); 1900-yard (Enhanced's horizonStart, with the far field past it)
view distance, 500-yard shadows, ground clutter 150%, and FXAA instead of
MSAA (Enhanced's anti-aliasing is CMAA2, a post filter, and the occlusion
pass needs a one-sample depth), and the bounce light (section 6b) over the
fill. Round 2 added the fill light, the sun haze
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

1. Indirect light. The bounce light (section 6b) now darkens and cools
   the shade under canopies and warms it by sunlit ground and walls, from a
   grid baked round the camera, and the shade's hue is Enhanced's (section
   6c); the ground's shade takes the sky it sees from a height field round
   the camera and a fill in the open (6e), but stays 13% brighter than
   Enhanced's, held up by the views it was not captured in; its crisper
   dappling through the canopy (its sun shadows) is not matched.
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
6. The mid-distance veil: ported as the air (section 5b), Enhanced's global
   fog layer at its own density, scattering the fog colour. The floor's
   gap is -18% (was -24%); what is left of the veil's blue in the near
   crowns is the leaves' light, not air.

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

   Glows held small (round 2): Jered saw a huge halo round the player on
   Holy Light, lamps and torches with big bright halos, and the Barrens'
   sunlit road glowing. Additive effects and lamp cards pile up far past 1 in
   the 16F scene where the 8-bit one stopped each draw at 1, and the bloom
   spread all of it four quarter-size texels wide, on top of the 8-bit halo
   over everything above 0.8. Now, with the HDR scene, only light above 1.6
   blooms (`kHdrBloomFloor`: the sun's disc and glare, lamps, fire, spells),
   its taps are a texel and a half apart, it adds at most 0.3 a channel
   (`kHdrBloomMax`), and the 8-bit halo is left to the 8-bit scene. A
   candle or a torch keeps a small local glow, as in Enhanced's capture.
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

## 9c. Sun through leaves ("Sun through leaves (the reference client)")

In Enhanced's Goldshire at 9:22, looking toward the low sun, the canopy in
front of it glows warm yellow-green: light coming through the leaves. 3.3.5
lights a leaf's back with the ambient alone, so the same canopy read as a
dark mass against a white sky.

**What Enhanced's shader does.** `material3_mesh_ps` (5446721; 20 of its 40
permutations disassemble) lights M2s with the sun (`N.L` saturated, a
half-Lambert lerp of the hemisphere ambient) and with one more directional
light from a 32-byte cbuffer: `(dir.xyz, w)`, `(colour.rgb, a)`. With `w`
of 0 or more it is a second diffuse light, `w * albedo * colour * sat(N.L)`;
with `w` negative it is a light from behind:

    t        = (1 - (alpha * a)^2) * sat(-N.L)
    colour  += albedo * colour * -w * t       (times the model's fade)
    alpha   *= sat(1 - sat(5 t) * sat(-w (0.8 - 0.75 a)))

added to the lit colour before its gamma/HDR output. `material3_wmo_ps` has
the same block, so it is a scene light, not a foliage material; which light
the engine puts there (the sun, or a fill) is not visible from the shader,
and there is no view-dependent lobe in either shader - no `V.L` term at all.

**What Wowee does** (`leafLight`, m2.frag, on in the Enhanced preset only):
for the foliage cutout batches (`alphaTest == 2`: a foliage-classified
model's alpha-keyed batch, m2_renderer_render.cpp `foliageCutout`; trunks
are opaque batches and ground clutter is `alphaTest == 3`), Enhanced's form
with the sun as the light, `albedo * sunColour * sat(-N.L)`, where `N` is
the normal turned toward the eye (a card's authored normal says little
about which side the eye is on) and a quarter of it whatever the angle (a
thin leaf passes some light either way), and three weights Enhanced does not
have in this shader:

- a forward lobe toward the **sun sprite**, `x^4` and `x^8` mixed (x the
  cosine between the view ray and the sun): half at about 26 degrees off
  the sun, gone at 90. The world's light direction is not the sun - the
  stock light stays 20-37 degrees up all day and night - so the sprite's
  direction and a day weight (0 at the horizon, 1 from 9 degrees up, as the
  HDR sun's) go to the shaders in `localLightMeta.w` (leaf_light.hpp).
- the sun's shadow at the leaf, read from the side the sun is on (a card
  turned away from the light is offset toward it, not into its own shadow);
  it is the shadow the pixel already reads, so no extra taps. A leaf in
  another's shade stays dark, so the crown glows at its rim and its gaps.
- the leaf's colour twice, `albedo * (0.4 + 0.6 albedo)`: light through a
  leaf is the leaf's colour, more saturated than its face.

Strength 1.3, and the sum stops at 1.0 a channel (or where the leaf
already was), under the tone map's 0.8 knee plus a little, so the bloom
gets a glow and nothing blows out.

The grass blades' light-through (round 4) also takes the sprite's
direction and day weight now: it took the way the light *travels*, which
points away from an eye looking into the sun, so the blades glowed with the
sun behind the camera, and with the stock light up all night they glowed
in the moonlight too.

Side by side (Goldshire 09:22 toward and away from the sun, 12:00, 17:30,
22:30; Duskwood and Ashenvale toward the sun; `leaf/sheet_b.png` in the
session scratchpad, with the live captures beside the matched views):
toward the sun 24-26% of the frame brightens by more than 8/255 in
Goldshire and 20-22% in Ashenvale (its purple canopy in its own colour);
looking away, at night and in Duskwood's dark canopy the change is within
the frame-to-frame noise of the wind and the mist. Cost (Iris Xe,
1280x540, Goldshire 9:22 looking into the sun through the canopy, the row
off and on in turn, 6 s windows; other lab clients shared the GPU, so only
the 12 windows with the frame's GPU time under 20 ms are kept): scene pass
median 7.51 ms off, 7.29 ms on - no cost that can be told from the noise
(+-0.3 ms). It is a dozen ALU operations on foliage pixels only, with no
texture or shadow reads of its own.

## 9d. No forward scattering under a sky without a sun

Outland's skies have a planet and no sun: a LightSkybox model at full weight
whose flags lack bit 2 takes the whole procedural sky, sun included
(0x7f09b0; `LightingManager`'s `replacesSky`). The light still has a
direction there, and the sun haze and the froxel mist scattered it forward:
at Hellfire's noon, looking at the planet, 6.2% of the view was white (the
mist off: the planet shows, nothing white). `LightingParams::sunDisc` is
now how much of the procedural sun the sky shows - 0 under such a model,
eased in from half its weight so a border does not pop - and both passes
leave their forward lobe out by it, keeping the even share (the haze's 0.4,
the mist's 0.35), so the air away from the planet is no brighter. Hellfire
noon looking up: mean 0.517 -> 0.199, white 6.2% -> 0%; its ground view
0.442 -> 0.429; Nagrand 0.526 -> 0.275, white 2.5% -> 0%. Azeroth and
Northrend (no such model) are unchanged: Goldshire's sky view 0.301 / 0.300,
Grizzly Hills 0.460 / 0.459, the Goldshire and Barrens views within run
noise. The HDR row's mid-tone lift outside Enhanced's daylight is untouched.

## 10. Not done, and why

- Enhanced's ZoneLight polygons (section 2b), and its keyframes for the 11
  3.3.5 lights with no Enhanced match. The keyframe table itself is generated
  locally (section 2b), never committed.
- Fully linear colour: the HDR row (9b) keeps 3.3.5's display-encoded
  textures and Light.dbc colours and lifts only the clamp; textures are not
  decoded to linear, and the tone curve is the identity below its knee.
- Probe GI itself (section 6): the bounce light (6b) is a baked stand-in.
  The layered global fog constants (section 5).
