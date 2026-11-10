# the reference client's water

How the official "the reference client" client (build 1.60.1.70862, the retail 12.x
engine on 1.12 data; enhanced-lighting.md section 1) draws lakes, rivers, the
sea and the canals, read from its shaders and its LiquidType data, and the
cheap version Wowee draws in the Enhanced preset. Stock 3.3.5 water, and
Wowee's HD water (E34) that this builds on, are in water.md and
enhancements-plan.md E34.

Sources (same build config `5ada954223f7c409ea172361f5c9e565`, from
wago.tools; nothing was executed): the DB2 tables `LiquidType`,
`LiquidMaterial`, `LiquidObject`, `LiquidTypeXFFTTile`, `LiquidTypeXTexture`;
the shaders below, inflated from their `GXSH` containers and disassembled
with `dxc -dumpbin`; and five Enhanced ADT roots (MH2O) for which liquid type
each place uses. Jered's settings: `pbrLiquidDetail 1`, `waterDetail 2`,
`refraction 2`, `reflectionMode 2` (SSR; the exe's own text: "0 = None, 1 =
SSR, No Sky, 2 = SSR, 3 = Planar").

## 1. The shaders

The PBR water is LiquidMaterial 130 (`Liquid::LiquidTypePBRWater` in the
exe, "PBRWater Prepass"). Its files have no names in the community listfile;
they sort between `particulatevolumeprepass` and `perturbdisplmap`, i.e.
`pbrwater*`:

| FDID | Stage | What it is |
|---|---|---|
| 7552022 | vertex (4 permutations) | `VSConstantsPBRWater`: patches (`StructuredBuffer<Patch>`, `Key`), FFT tile arrays (`Texture2DArray` x3), the liquid struct |
| 7552032 | pixel (12) | `PSConstantsPBRWaterPrepass`: writes a 4-target G-buffer of the water |
| 7552027 | pixel (8) | `PSConstantsPBRWaterDeferred`: the full-screen shading |
| 7552038 | compute 8x8 | relights a cube map face by face (gbuffer cube -> sun, shadow, sky): the reflection probe the deferred pass samples |

Per liquid the GPU gets `struct LiquidTypePBRWater { float4 x11; int4;
int4; int[6] }` (232 bytes); the six ints are the texture-array slices of
LiquidType's six textures (foam high/mid/low, rim, depth, river), the two
int4s the FFT tiles' slices and sizes (`LiquidTypeXFFTTile`).

### The prepass (7552032) - what the G-buffer holds

- RT0: the water's view depth.
- RT1: `x` = liquid struct index (low 6 bits) and flags (64-384), `y` =
  the displaced height, `z` = an FFT crest term (foam and roughness),
  `w` = a second FFT term (the sun's sub-surface weight).
- RT2: foam colour (rgb, sRGB textures to linear with `pow 2.2`) and its
  cover (a), the sum of four foams, saturated:
  - whitecaps: the foam high/mid/low textures weighted by the crest term to
    the powers 0.5, 1.5 and 4.5, each tiled per ADT tile (scales per liquid),
    scrolled by time;
  - **rim foam** by the distance to the shore (the `AreaLiquidShoreDistanceField`
    texture): `strength * smoothstep(sat(1 - d / width))^4`;
  - **depth foam** by the water behind the pixel (scene depth minus water
    depth): `strength * smoothstep(sat(1 - t / width))^4` - foam at the
    shore *and round every rock and post that comes near the surface*;
  - **river foam** where the flow is fast, `strength * smoothstep(sat((sqrt(speed)
    - 0.22) * 3.03))`, the texture read at two flow phases blended by the
    flow weight (the classic two-phase flow map) - rivers "foaming".
  The flow (speed, phase uvs) comes from the vertex stage; the FFT amplitude
  is scaled by `1 + k * speed` (k negative: running water is flatter).
- RT3: the surface slope (xy) and the slope variance terms.

### The deferred shading (7552027)

With `t` the water's depth behind the pixel (view depth of the bed, read
through the refracted uv, minus the surface's), `N` from the slopes, `V` to
the eye, `L` to the sun:

```
uvRefr  = uv + slope.xy * 0.005 * min(50, t)        (kept only if the bed there is behind the water)
bed     = scene(uvRefr), with the froxel fog in front of it divided out
T       = exp(-t * density * absorb.rgb)            per channel
albedo  = lerp(Color_2, Color_1, sat(height / k + 0.5))   (crests lighter)
body    = albedo * (1 - T) * [ sunIn * shadow * (crest SSS, NdotV^2 term, HG(g, V.L))
                               + B * (sunIn / 4pi * exp(-0.0072 z) + 0.933 * skyIrradiance) ]
sunIn   = sun * pi * 0.98 * (1 - (1 - sinElev)^5)    (Fresnel transmission in)
foamLit = lerp(sky, sun, shadow * sunUp * lerp(sat(N.L + 0.25), (N.L)^8, crest)) * foam.rgb
envSpec = EnvBRDFApprox(roughness, N.V) [F0 0.02] * reflScale * probeCube(R, mip = roughness)
sunSpec = shadow * sun * LUT(roughness', angle(R, L)) * intensity
Fsun    = (0.02 + 0.98 (1 - V.H)^5) * sunUp
out     = bed + sat(2 t) * (body + envSpec + sunSpec + (1 - Fsun) * lerp(bed * T, foamLit, cover) - bed)
out     = out * (1 - fog.a) + fog.rgb
```

- `sunUp` = smoothstep(sat((sin elevation + 0.1) * 5)); roughness =
  sat(lerp(base, 0.8, foam) + sqrt(sqrt(slopeVariance / 2))): the base is
  0.013, glassy; what roughens the sun path is the waves' own slope variance.
- Constants recognised: Karis' EnvBRDFApprox (`-1, -0.0275, -0.572, 0.022 /
  1, 0.0425, 1.04, -0.04`, `exp2(-9.28 N.V)`), HG phase `(1 - g^2) / (1 + g^2
  - 2 g cos)^1.5 / 4pi`, Schlick at 0.02.
- The refraction is not multiplied by `1 - F` of the environment, only by the
  sun's Fresnel; the shoreline is the `sat(2 t)` fade from the bed itself
  (the last half yard).
- **Reflection**: a cube map of the surroundings (relit each frame by 7552038,
  so it has the terrain, trees and sky) plus the sun/moon through a
  pre-integrated highlight table; `reflectionMode 2` adds SSR on top. No
  planar mirror.
- **Under water**, `ffxunderwaterhigh` (FDID 2990812) is a full-screen pass
  that rebuilds each pixel's world position and lays caustics on what is
  under the surface: a caustic texture read three ways at world x 0.075
  (triplanar, weighted by the face's normal), faded over the first 15 yards
  below the surface and over 70 yards of distance. Wowee's "Sunlight under
  water" row (E15) is the same idea.
- **From below** (a flag in the constants): the shader keeps only `bed * T`
  over the camera-to-surface distance and the foam - the world above seen
  through the water column.

## 2. The data

`LiquidType` rows with MaterialID 130 carry three colours and 31 floats
(Float_0..30; 31-37 are zero on every row) and four `Coefficient`s. The GPU
struct is eleven float4s; the deferred pass reads struct float4 0 as
`(absorb.rgb, density)` and float4 10 as `(sun LUT base, roughness, reflection
scale, -)`, which with Float_28..30 = `0.5, 0.013, 1` on every lake puts the
floats in order with the three colours as three float4s among them. So,
firmly: **Float_0..2 is the absorption, Float_3 the density**; Color_1 and
Color_2 the scattering colours lerped by the wave's height.

Which column is which channel is not in the shader (the client copies the
row into the struct). Enhanced's own Crystal Lake settles it (Jered's capture,
`enhanced_water_092847.png`, 9:27): from the shore out, the bed goes ochre,
then olive, then is lost under a grey-teal - blue is absorbed first, so
**Float_0 is red**, as stored. (Wowee's first version read the columns blue
first, from the rows' scattering colours; the capture overturned that.) The
foam, scattering-coefficient and FFT columns in between are not pinned to a
meaning (the client fills the struct; that code was not read).

The capture also shows how little the scattering colours show: Crystal Lake's
deep water measures rgb (72, 85, 82) - hue 160, chroma 13 of 255, luma 80 -
against rows whose Color_1/Color_2 are a clear sky blue. What the eye sees is
mostly the mirrored surroundings (a grey-blue sky, the trees, the haze) over a
dim body; the far lake is (91, 99, 88), the shallow edge (101, 81, 52).

Which type each place uses (MH2O of the Enhanced ADT roots):

| Place | LiquidType | Absorb (Float_0..2) x density (Float_3) | Color_1 / Color_2 |
|---|---|---|---|
| Crystal Lake (azeroth_32_49) | 1324 Generic - Lake - Forest | 0.283 0.477 2.657 x 2 | #9bbfec / #71b2dc |
| The stream into it | 1288 Generic - River | same | same |
| Lake Everstill, Redridge (36_49) | 1325 Generic - Lake - Large | same | same |
| Lushwater Oasis (kalimdor_35_33) | 1240 Generic - Lake | same | same |
| Stormwind harbour, Westfall coast (29_47, 28_50) | 1250 Generic - Ocean | 0.25 0.75 6 x 8 | #48aab0 / #336d7c |
| Stormwind canals (WMO water) | 1344 Stormwind - Canals | 0.3 1 1 x 0.5 | #1bb2e6 / #235b9a |

Others in the table: swamps (1297/1313: 2.8 0.5 1.5 x 8, olive), Desolace
(x 20, murky brown), dirty lake (1340), fel (1341/1345), hot springs, Feralas,
Tanaris puddles, Teldrassil, Orgrimmar, WMO interior (1290/1354). The four
lake/river rows differ only in Float_7/9/11/13/25 (wave and foam columns).

So Enhanced's fresh water loses blue within a yard: the bed shows ochre for a
few yards at the shore and goes olive, then the dim body and the mirror take
over. Its sea is far denser (x 8): the bed is gone within a yard or two. The
canals are thin (x 0.5) and pass red best: the floor stays visible, browning
with depth.

## 3. What Wowee draws ("Water (the reference client)")

Row `enhancedwater` (Enhanced page, on in the Enhanced preset only; needs and
turns on the HD water pipelines; off = the HD water or stock rows as they
were). `water_hd.frag` in Enhanced's mode (`hdParams.x == 2`), per pixel from
above:

- **Absorption and scattering**: Enhanced's formula with its per-liquid
  absorption, density and colours (table above, Float_0 red): ocean (terrain
  ocean, LiquidType 2 family) -> 1250, exterior WMO water -> 1344, all other
  water -> the lake row. `t` is the view-depth difference from the scene's
  depth copy, as Enhanced's, taken at 0.7 for the absorption (Enhanced's bed
  shows through more water near the shore than its constants over our depth
  copy give; section 4). The scene is display-encoded, so `T` is taken to
  that encoding (`T^(1/2.2)`). The body's brightness and hue were set
  numerically against the capture (section 4): the colours keep 15% of their
  saturation, the sky light that reaches into the water is Light.dbc's
  ambient brightness x 0.58 in a faint green-teal (Light.dbc's ambient is a
  blue Enhanced's water does not take on), the sun x (0.05 + 0.05 HG(g 0.6))
  through Enhanced's Fresnel transmission and sun gate, one shadow-map tap.
  The sun gate and highlight use the sun sprite's own direction
  (`LightingParams::sunDir`, in the sky colours' `.w` of the water's frame
  block), since the stock light stays up all night. The body veils in over
  the depth as one weight, `1 - T . (0.3, 0.55, 0.15)`, not per channel:
  per channel the body's blue came in within a yard over a red-brown bed and
  the shallows went mauve (86 63 65 against Enhanced's ochre 102 80 50). Thin
  veil starts olive (x 0.8 1.6 1.05, fading to the deep tint as the veil
  thickens), as Enhanced's shallows turn from ochre to olive. The
  wave height moves the colour between Color_2 and Color_1 by 0.3 of its
  swing (at full swing it drew the long waves as broad light and dark bands,
  the largest share of the ripple contrast, twice the capture's).
- **Waves**: Enhanced's surface is an FFT sea, per liquid up to four `FFTTile`
  patches (`LiquidTypeXFFTTile`; FFTTile column 0 the patch size in yards,
  column 2 the wind speed): the lakes and rivers 16, 32 and 64-yard patches at
  winds of 3-4, the sea 128-512 yards at 2-6, the canals one 46-yard patch at
  6 - a Phillips spectrum that peaks at waves of a few yards. Enhanced has no
  ripple texture: `LiquidTypeXTexture` gives its PBR liquids only the six
  foam maps, so every ripple is the FFT's (no scrolled detail normals).
  Wowee sums twelve directional waves analytically (eight in the canals;
  slopes, and the height from the five longest, no
  texture, so no tile repeats): wavelengths spaced geometrically over
  0.35-8 yd (lakes), 1.5-64 yd (sea), 0.7-23 yd (canals), each weighted by
  the root of Phillips' cut-off `exp(-1 / (k L)^2)` (L = wind^2 / g: 0.5 yd
  lakes, 2.6 sea, 3.7 canals) and by `(lambda / lMax)^0.5` (the short waves
  faint, as an FFT sea's damped small waves are), at 0.8 strength across the
  wind, deep-water dispersion (w = 0.8
  sqrt(g k)), the directions turned by the golden angle from wave to wave,
  the field warped by value noise and each wave's phase bent by the same
  noise (2 cycles across it) so the crests break up after a few
  wavelengths. A wave is faded out where it is under two pixels measured
  along its own direction (so waves seen side-on at a grazing angle stay),
  and its slope variance goes to the roughness as Enhanced's prepass does;
  once the waves are shorter than twice the narrowest footprint any
  direction can have, the rest are skipped and their variance added in
  closed form, so far water runs a few waves only
  (`roughness = 0.013 + sqrt(sqrt(variance / 2))`, at least 0.06; it widens
  the sun path and lowers the environment Fresnel far off). In rivers the
  field is carried down the flow (below). The stock lake_a/ocean_h frames
  are not read in this mode.
- **Refraction**: Enhanced's bend (slope x depth), in view space, eased off
  with distance; refused where the bent tap lands in front of the water.
- **Reflection**: EnvBRDFApprox at F0 0.02 (Enhanced's), of Light.dbc's sky
  by the ring rule or the planar sky pass ("Water reflections"), taken to a
  grey with a touch of green at 0.86 of its brightness (Enhanced's mirrored
  sky is grey-blue; set against the capture's far lake). Enhanced mirrors a
  relit probe of the surroundings; as a stand-in, the lower a reflected ray
  (below 0.6 of the zenith), the more of it (up to 0.75) is the "surroundings":
  the fog colour and the ambient, green-tinted, at 0.6 brightness - the shore,
  trees and haze; round lakes and canals (not the sea) its edge is a ragged
  skyline by the reflected ray's bearing (value noise, 0.06-0.5 of the
  zenith), so the ripples break the mirror into streaks of shore and sky,
  and darker (x 0.8) where the skyline stands tall - the trees of
  Enhanced's probe.
  The sky part is graded to grey (tint 0.81 0.83 0.86). Over that, Enhanced's
  SSR (`reflectionMode 2`): the reflected ray marched through the scene's
  depth copy (6 steps doubling to ~60 yd; the hit put between the last two
  by their depth differences, no refining taps), the scene's colour where it
  crosses something, faded at the copy's edges - islands, posts, bridges,
  the player stand in the water. Marched only where the Fresnel is over
  0.035 (looking down near the camera the mirror does not show). No probe
  cube.
- **Its own shader build**: `water_hd.frag` built twice, plain
  (`water_hd.frag.spv`, HD water) and with `ENHANCED_WATER`
  (`water_fw.frag.spv`, this row), the renderer binding one or the other.
  With both paths in one shader the Iris Xe allocated registers for the
  larger, so growing Enhanced's path made HD water slower too, and cutting
  Enhanced's work (fewer waves, a shorter march) changed nothing measurable;
  unrolling the march made both slower.
- **Sun and moon**: GGX (alpha max(0.05, roughness^2)) with Schlick at the half vector, the
  shadow tap; in the HDR scene up to 6 (it blooms), else 1.2. By night the
  path is the moon's, taken opposite the sun, at 0.35 of the night light
  times dnc.db's night intensity. The planar sky pass's own sun and moon
  sprites are held to 1.6x the ring sky there (they mirrored as hard discs
  cut off by the pass's target edge - also visible with HD water alone).
- **Foam**: Enhanced's depth-foam band `smoothstep(1 - t / w)^4` (w 0.7 yd
  lakes, 1.2 sea, 0.5 canals) on a procedural pattern - the shore and every
  rock or post; and **river foam**: 3.3.5 has no flow data, but a river's
  sheet slopes, so the surface's own gradient (derivatives of the position)
  gives the downhill direction and a speed; the foam and the wave field are
  carried down it with a two-phase flow (bounded offsets, no seams), and the
  waves get choppier. Lit as Enhanced lights foam.
- **Composition and fog**: Enhanced's, `bed + sat(2t)(body + reflection +
  sun + (1 - Fsun) lerp(bed T, foam, cover) - bed)`; the bed was fogged with
  the scene, so its fog is taken off before the water's is laid once.
- From below, inside buildings, magma and slime: unchanged (stock / HD).

Constants are in `water_hd.frag` (`fwLiquidFor`); `tests/test_enhanced_look.cpp`
checks them against the table and the row against the preset.

## 4. Measured, and what is left

**Against Enhanced's capture.** Crystal Lake from its south-east shore at 9:27
in the lab (Enhanced preset, `.go xyz -9474 -110`, facing north-west), the
same regions measured as in the capture (mean rgb of each, 0-255):

| Region | Enhanced | HD water | Water (the reference client) |
|---|---|---|---|
| Deep, near | 72 85 82 (luma 80, chroma 13, hue 160) | 83 126 120 (113, 44, 172) | 67 79 75 (75, 12, 161) |
| Deep, far | 91 99 88 (96, 11, 104) | 109 154 140 (139, 45, 161) | 90 106 103 (101, 17, 166) |
| Shallow edge | 101 81 52 (83, 48, 35) | - | 101 74 61 (81, 40, 19) |

Within about 6% in brightness and saturation in the deep water and at the
shore; the far lake is a little greener than Enhanced's (its probe mirrors the
real trees and island, here a fog-coloured stand-in), and the shallow bed a
little redder (Enhanced's ground textures are its own). The first version
(blue-first absorption, the colours at full saturation, the sky as it is) read
38 65 79 near and 79 109 116 far: a saturated blue, as Jered saw.

**The ripples against the capture.** Jered, of the version above: "ripples
on water dont look like enhanced", then "better but not great still". Measured
from the capture's own viewpoint (`.go xyz -9474 -110`, facing 4.49, camera
pitch -28 at 16 yards, 9:27, the regions below line up with the capture's
island and shore to a few pixels), rendered at the capture's 2560x1080 (at
1280x540 the finest ripples are lost before the comparison starts) and both
halved to 1280x540, six frames 0.7 s apart averaged. Per region: luma detail
under ~6 px (fine: luma minus a sigma-2 blur, std), detail 6-30 px (mid:
sigma 2 minus sigma 10), and the power-weighted wavelength of the detail
across (x) and down (y) the screen, in pixels:

| Region (1280x540 px) | Enhanced: mid / fine / wl x / wl y | 89942de | Now |
|---|---|---|---|
| deep, near (50,200)-(370,285) | 1.24 / 0.71 / 46 / 19 | 2.51 / 1.81 / 69 / 16 | 1.14 / 0.98 / 57 / 13 |
| middle (0,130)-(450,195) | 1.58 / 1.78 / 46 / 8.8 | 4.69 / 4.78 / 62 / 9.6 | 1.40 / 2.22 / 36 / 7.3 |
| middle, wide (150,115)-(750,150) | 1.71 / 2.39 / 32 / 7.5 | 4.39 / 5.40 / 69 / 8.8 | 1.66 / 2.21 / 34 / 9.9 |
| far (575,50)-(750,100) | 1.32 / 3.12 / 38 / 5.2 | 2.55 / 9.48 / 57 / 4.8 | 1.17 / 3.71 / 44 / 5.0 |
| shallows by the player (380,200)-(560,280) | 0.67 / 1.00 / 16 / 12 | 1.54 / 1.00 / 68 / 17 | 0.60 / 0.61 / 27 / 14 |

Mean |log ratio| to Enhanced over the regions and the four terms: 0.62
before, 0.20 now (mid 0.85 -> 0.09, fine 0.77 -> 0.26, wl x 0.67 -> 0.25,
wl y 0.17 -> 0.19); colour 0.067 -> 0.037. What it took, by how much each
moved the numbers: the crest-to-trough colour swing was most of the ripple
contrast (at full swing it painted the long waves' height as broad bands;
at 0.3 the mid contrast halved and the wavelengths came within 20% of the
capture's), then the spectrum: equal slope per wave over 0.6-7 yd drew
"scales" of short ripples near the camera at twice the capture's fine
detail, where Enhanced's sheet is smooth near and finely streaked far off;
weighting the short waves down ((lambda / 8)^0.5) and the band 0.35-8 yd
put both in range. The noise bend shortened the crests across the screen
(wl x in the middle 77 -> 36-42). The colour of the body and of the shallows:
deep water within 3 of the capture's channels in the near and middle
regions, the far lake still 5-6 brighter (it mirrors a stand-in, not the
trees), the shallows ochre (93 65 51 against 102 80 50) where they were
mauve (86 63 65).

**The shore, the far lake and the cost (second pass).** The shore by a
transect: from the waterline right of the player out into the lake
(capture from (650,272), ours from (635,282), both reaching water 24 px
in), 14 px patches every 8 px for 112 px, mean |rgb difference| to the
capture and the bed's detail (luma std after a sigma-3 high-pass) as a
share of the capture's. Enhanced's bed shows ochre, then olive (77 77 59
at 48 px), then grey-teal at ~80 px; ours went grey within ~16 px. With the
absorption taken over 0.7 of the depth difference and the thin veil
starting olive: |rgb diff| 6.7 -> 3.9, bed detail 0.53 -> 1.01 of the
capture's, the deep regions' colour unchanged (colour score 0.037). At
0.5 of the depth the bed showed through the deep water too (our bed is
redder than Enhanced's) and the colour score went to 0.063.

The far lake (region (575,50)-(750,100)) is what the probe stand-in shows
- the SSR finds nothing there - so the excess was the stand-in's: darker
under the tall parts of its skyline (x 0.8) it reads 86 102 97 against
Enhanced's 82 99 99 (was 88 105 101); at x 0.55 the whole lake went 5 too
dark and lost ripple contrast. Ripple score with everything: 0.176 (mid
0.19, fine 0.08, wl x 0.26, wl y 0.18), first pass 0.197.

Cost, 1280x540, Iris Xe, the `water` GPU pass in Enhanced mode and with
the row off (HD water), medians of three 8 s windows, no other client on
the GPU except where noted:

| Build | Crystal Lake: HD / Enhanced | Canal: HD / Enhanced |
|---|---|---|
| 89942de (one shader) | 1.29 / 1.31 | 1.80 / 1.88-2.00 |
| first pass, 1906857 (one shader) | 1.52 / 2.13 | 1.46 / 2.29 |
| now (two builds) | 1.00 / 1.36 | 1.05 / 1.83 (a second client for part) |

Enhanced water is back to what 89942de cost at the lake and below it in the
canal, ~0.5-0.8 ms under the first pass; HD water got 0.3-0.75 ms cheaper
by not carrying Enhanced's path. Over HD water the row is +0.36 (lake) and
+0.78 (canal), the canal being the march (~0.3: grazing water, the mirror
everywhere) and eight waves (~0.3). Measured on the way (canal, Enhanced's
build): flat water and no march +0.10 over HD, the waves alone +0.41.
The GPU's own pace wandered by up to a third between runs (whole-frame
GPU time 10.4-14.4 ms for the same view), so only figures from one run
compare closely; the deltas above are each from a single run.

**Cost** (lab server, Iris Xe, 1280x540, Enhanced preset at noon, the row off
(HD water) and on in turn, four 8 s pairs a place, `WOWEE_FRAME_PROFILE=1`,
the GPU's `water` pass, paired medians):

| Place | Difference over HD water | At 2560x1080 (x4) |
|---|---|---|
| Crystal Lake shore | +0.06 | about +0.25 |
| Stormwind canal (water fills the view) | +0.08 | about +0.3 |

The twelve analytic waves replaced nine reads of the stock frame texture, so
the row now costs less over HD water than its first version did (+0.14 and
+0.24); the frame GPU time is within noise. These windows were taken with
another client sharing the GPU (absolute times up to twice the first run's),
so the deltas are the paired medians only. The foam pattern is read only where
there is foam, and the fog over the composed water is one lookup.

Seen in the lab (Enhanced preset, HD water against this row, 12:00, 19:36 and
23:00; Crystal Lake, the stream into it, the Stormwind canal and harbour, the
Westfall coast, Lushwater Oasis, and from under the surface at the lake and
the canal): the lakes are a calm slate grey-teal with an ochre-to-olive band of
clear shallows and small soft ripples; the canal shows its floor browning
with depth under a grey mirror instead of a royal blue; the sea is grey-teal
with a gold sun path at sunset; nights are dark with the moon's glint. Under
the surface nothing changed. Side by side with Enhanced: the session
scratchpad's `fwater/side_crystal.png` (Enhanced | HD water | this row) and
`fwater/sheet_r1.png` (the tour).

- Not done: the FFT surface and its whitecaps (Wowee's sheet is flat; the
  ripples are sixteen analytic waves shading it); the probe cube of the
  surroundings (the SSR covers what is on screen, a stand-in the rest);
  per-zone liquid types beyond the three classes (swamps, Desolace
  and the rest need the area -> LiquidType link Enhanced's ADTs carry); the
  shore distance field (rim foam), which 3.3.5 does not have; the view of
  the world above from under the surface (`bed * T`).
