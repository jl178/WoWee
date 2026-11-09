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
row into the struct). Read Float_0 as red, the fresh water absorbs blue
fastest and its shallows go amber, the sea's bed turns orange, and in the lab
the lakes' shallows and the canal floor came out pink-purple under the blue
body. Read Float_2 as red (the columns blue first, as the DB2's colour ints
are stored), every row takes the hue of its own colours: the sea and the
lakes absorb red (turquoise and green shallows), the hot springs and canals
pass blue (their colours are blue), Desolace absorbs blue (murky brown). Wowee
uses the second reading; the table below lists the columns as stored. The foam,
scattering-coefficient and FFT columns in between are not pinned to a
meaning (the client fills the struct; that code was not read).

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

So (blue-first reading) Enhanced's fresh water loses red within a yard and the
bed goes green while the light-blue scattering colour builds over it - green
shallows going to a blue body. Its sea is far denser (x 8): the bed is gone
within a yard or two and the water is its teal scattering colour. The canals
are thin (x 0.5) and absorb red and green alike: the floor turns blue with
depth.

## 3. What Wowee draws ("Water (the reference client)")

Row `enhancedwater` (Enhanced page, on in the Enhanced preset only; needs and
turns on the HD water pipelines; off = the HD water or stock rows as they
were). `water_hd.frag` in Enhanced's mode (`hdParams.x == 2`), per pixel from
above:

- **Absorption and scattering**: Enhanced's formula with its per-liquid
  absorption, density and colours (table above): ocean (terrain ocean,
  LiquidType 2 family) -> 1250, exterior WMO water -> 1344, all other water
  -> the lake row. `t` is the view-depth difference from the scene's depth
  copy, as Enhanced's. The scene is display-encoded, so `T` is taken to that
  encoding (`T^(1/2.2)`). Light: Light.dbc's ambient x 0.55 for the sky
  irradiance and the sun x (0.12 + 0.12 HG(g 0.6)) through Enhanced's Fresnel
  transmission and sun gate, one shadow-map tap - scaled so the body comes
  out near 0.6 of its colour by day, as Enhanced's linear albedo times its
  light does once encoded. The sun gate and highlight use the sun sprite's
  own direction (`LightingParams::sunDir`, in the sky colours' `.w` of the
  water's frame block), since the stock light stays up all night. The wave height that
  picks between the two colours is the stock frame's brightness.
- **Refraction**: Enhanced's bend (slope x depth), in view space, eased off
  with distance; refused where the bent tap lands in front of the water.
- **Reflection**: EnvBRDFApprox at F0 0.02 (Enhanced's), of Light.dbc's sky
  by the ring rule or the planar sky pass ("Water reflections"). No probe
  cube or SSR: the surroundings are not mirrored (see 4).
- **Sun and moon**: GGX (alpha 0.05) with Schlick at the half vector, the
  shadow tap; in the HDR scene up to 6 (it blooms), else 1.2. By night the
  path is the moon's, taken opposite the sun, at 0.35 of the night light
  times dnc.db's night intensity. The planar sky pass's own sun and moon
  sprites are held to 1.6x the ring sky there (they mirrored as hard discs
  cut off by the pass's target edge - also visible with HD water alone).
- **Foam**: Enhanced's depth-foam band `smoothstep(1 - t / w)^4` (w 0.7 yd
  lakes, 1.2 sea, 0.5 canals) on a procedural pattern - the shore and every
  rock or post; and **river foam**: 3.3.5 has no flow data, but a river's
  sheet slopes, so the surface's own gradient (derivatives of the position)
  gives the downhill direction and a speed; the foam and the fine ripple
  layer are carried down it with a two-phase flow (bounded offsets, no
  seams), and the ripples get choppier. Lit as Enhanced lights foam.
- **Composition and fog**: Enhanced's, `bed + sat(2t)(body + reflection +
  sun + (1 - Fsun) lerp(bed T, foam, cover) - bed)`; the bed was fogged with
  the scene, so its fog is taken off before the water's is laid once.
- From below, inside buildings, magma and slime: unchanged (stock / HD).

Constants are in `water_hd.frag` (`fwLiquidFor`); `tests/test_enhanced_look.cpp`
checks them against the table and the row against the preset.

## 4. Measured, and what is left

Cost (lab server, Iris Xe, 1280x540, Enhanced preset at noon, the row off
(HD water) and on in turn, four 8 s pairs a place, `WOWEE_FRAME_PROFILE=1`,
the GPU's `water` pass):

| Place | HD water | Enhanced water | Difference | At 2560x1080 (x4) |
|---|---|---|---|---|
| Crystal Lake shore | 1.10 | 1.24 | +0.14 | about +0.6 |
| Stormwind canal (water fills the view) | 1.05 | 1.30 | +0.24 | about +1.0 |
| Stormwind harbour (water in the distance) | 0.56 | 0.56 | 0 | 0 |

The frame's GPU time is within its noise of HD water's (10.8 -> 10.9 ms at
the lake). A first version cost the lake +0.26 and the canal +0.36: the foam
pattern is now read only where there is foam (the band or a flowing river),
and the fog over the composed water is one fog lookup instead of two.

Seen in the lab (Enhanced preset, HD water against this row, 12:00, 19:36 and
23:00; Crystal Lake, the stream into it, the Stormwind canal and harbour, the
Westfall coast, Lake Everstill at Lakeshire, Lushwater Oasis, and from under
the surface at the lake and the canal): the lakes now go green at the edge to
light blue and then a deeper blue instead of HD water's even pale cyan; the
canal is a saturated blue with its floor fading out with depth; at sunset the
sea carries a long gold sun path; at night the water is dark blue with the
moon's glint; the shore and rocks carry a thin foam band. The first reading of
the absorption (Float_0 red) gave pink-purple shallows and canal floor, which
is why the columns are read blue first (section 2). Under the surface nothing
changed (as section 3 says).

- Not done: the FFT surface and its whitecaps (Wowee's sheet is flat; the
  ripples are the stock frames'); the reflection of the surroundings (probe
  cube / SSR - a five-step SSR cost 0.3 ms at 1280x540 in E34 and was
  dropped); per-zone liquid types beyond the three classes (swamps, Desolace
  and the rest need the area -> LiquidType link Enhanced's ADTs carry); the
  shore distance field (rim foam), which 3.3.5 does not have; the view of
  the world above from under the surface (`bed * T`).
