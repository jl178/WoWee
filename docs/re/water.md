# Water, liquids and underwater (stock 3.3.5a, build 12340)

Every liquid surface, how it is coloured and animated, and what changes when
the camera goes under one. Addresses are in Wow.exe 3.3.5a; the stock shaders
are the ARB text in `Data/expansions/wotlk/shaders/{vertex/arbvp1,pixel/arbfp1}`
(read as in m2-runtime.md §3). The shader maths, the material choice and the
depth-ramp builders were first read for world-lighting.md §7, and that section
is still the place for them; this document adds the vertex data, the texture
matrices, the draw state, the underwater rules and the particulates, and is
where the gap table now lives.

| Wowee file | What it holds | Test |
|---|---|---|
| `include/rendering/stock_liquid.hpp` | frame picker, scroll, ramp row, uv scales, rotation slip | `tests/test_stock_water.cpp`, `tests/test_stock_world_fx.cpp` |
| `include/rendering/stock_particulates.hpp` | the underwater motes | `tests/test_stock_water.cpp` |
| `src/rendering/water_renderer.cpp`, `assets/shaders/water.{vert,frag}.glsl` | the surfaces | |
| `src/rendering/swim_effects.cpp`, `assets/shaders/water_mote.{vert,frag}.glsl` | the motes (and the optional splashes) | |
| `src/pipeline/adt_loader.cpp`, `src/pipeline/wmo_loader.cpp` | MH2O / MCLQ / MLIQ vertex data | |

## 1. LiquidType.dbc

45 fields, read at 0x8b2710; 0x8a27c0 copies the textures, colours, Float[18]
and Int[4] into the per-type material settings. Byte offsets:
`+0x08 Flags, +0x0c Type (0 water, 1 ocean, 2 magma, 3 slime), +0x10 SoundBank,
+0x14 SpellID, +0x18 MaxDarkenDepth, +0x1c/+0x20/+0x24 fog/ambient/diffuse
darken, +0x28 LightID, +0x2c ParticleScale (float), +0x30 ParticleMovement,
+0x34 ParticleTexSlots, +0x38 MaterialID, +0x3c Texture[6], Color[2],
Float[18] (field 23..), Int[4] (field 41..)`.

The shipped rows (Float/Int give only the non-zero leading entries):

| ID | Name | Flags | Mat | Texture[0] (frames) | Ramp (Texture[1]) | Float | Int[0..1] | Light | Particles |
|---|---|---|---|---|---|---|---|---|---|
| 1, 5, 41, 61, 81 | Water, Slow Water, Coilfang/Hyjal/Wintergrasp water | 0xf | 1 | `river\lake_a.%d` (30) | river | F0 1, F2 1 | 0, 1250 | - | scale 1, move 0, slots 0 |
| 9 | Fast Water | 0xf | 1 | `river\fast_a.%d` (16) | river | 1, 0, 1 | 0, 1250 | - | same |
| 2, 6, 10 | Ocean, Slow/Fast Ocean | 0xf | 1 | `ocean\ocean_h.%d` (30) | ocean | 1, 0, 1 | 1, 1250 | - | same; darken 30 / 0.5 / 0.5 / 0.25 |
| 13, 17 | WMO Water, WMO Water - Interior | 0xf | 1 | `lake_a` | WMO | 1, 0, 1 | 1, 1250 | - | same |
| 14 | WMO Ocean | 0x20f | 1 | `ocean_h` | ocean | **0.25, 180, 1** | 1, 1250 | - | same |
| 181 | Orange Slime (a water type) | 0xf | 1 | `LavaOrange.%d` (30) | river | 2, ?, 8.5 | 0, 1250 | - | same |
| 3, 7, 11, 19, 121, 141 | Magma family | 0x78 | 2 | `lava\lava.%d` (30) | - | 0.025 (ADT) / 0, 0.1 (WMO, CoA, Chamber) | - | 7 | scale 4, move 1, slots 1 |
| 15 | Green Lava | 0x78 | 2 | `LavaGreen\lavagreen.%d` (30) | - | 0.025 | - | 6 | scale 4, move 1, slots 1 |
| 4, 8, 12, 20, 21 | Slime family (21 Naxxramas) | 0x142 | 2 | `slime\slime.%d` (30) | - | 0.025 / 0, 0.1 (WMO) | - | 6 | none |
| 100 | Basic Procedural Water | 0xf | 3 | cube + height maps | ocean | ... | | - | unused by any map |

Flags bits the client reads: **0x8** the underwater particulates are shown
(0x790920); **0x10** they are fogged (0x79ca70); **0x200** a WMO water stays
exterior (0x793d20, WMO Ocean); **0x4** the WMO liquid height test gets its
0.01 allowance (0x7c8360); 0x20 / 0x40 / 0x100 pick the liquid fog set against
the zone's (0x7f16f0). The rest is server side (SpellID, the fishing and
fatigue rules) or sound (SoundBank, sound-engine.md).

What the terrain actually uses (scan of the 687 Azeroth ADTs): ocean is
LiquidType 2 in vertex format 2 (depth only; 70 528 of 86 222 ocean layers
carry no vertex data at all), lakes and rivers are 5 "Slow Water" in format 0,
magma is 7 "Slow Magma" in format 1. No Azeroth tile has an MCLQ.

## 2. The vertex

### ADT liquid (MH2O and MCLQ)

Built by 0x7ce390 (from CChunkGeomFactory::Build 0x7d4ab0):

- **Position:** the vertex height; x,y on the 25/6-yard cell grid. **Normal:**
  always (0, 0, 1). **Colour (attrib 4):** always white, water and magma.
- **Ramp coordinate (attrib 7):** `(0, table[depthByte])` when the
  LiquidMaterial format (LVF) is 0 or 2 (0x79b870). The table is chosen by
  **LiquidType Int[0]**: 0 is the river table 0xcdf7d0, built by 0x79e3c0 as
  `x = d/9; x <= 14/3 ? x*3/14 : 1` = **min(d/42, 1)**; 1 is the ocean table
  0xcdfbd0, **d/255**. Magma and slime (LVF 1) have no table and read 0.
- **Depth byte:** MH2O accessor 0x8a3160 - LVF 0: after the n heights; LVF 2:
  the n bytes of vertex data, or **255 when the layer has no vertex data**
  (the deep end - most open ocean); LVF 1/3: 0. MCLQ accessor 0x8a3220: byte
  0 of each 8-byte vertex.
- **Texture uv (attrib 6):** LVF other than 1: `world.xy * 0.06` (0xa40410),
  one repeat every 16.67 yards. LVF 1: the vertex's u16 pair x 3/256
  (0xa40414) - Searing Gorge's lava runs about 0.107 a yard, slightly turned.
- MCLQ type: MCNK flags 0x4 / 0x8 / 0x10 / 0x20 become LiquidType **1 / 2 /
  3 / 4** (0x7c5690); on map 530 the magma bit becomes 15, Green Lava.
- Only the cells whose mask bit is set get indices (0x7ce270).

### WMO liquid (MLIQ)

Built by 0x7a7b00 (CMeshGeomFactory 0x7d43f0):

- Position in group space; normal up.
- **Colour:** white in an exterior group; in a group that is not exterior,
  the liquid's MOMT colour (`WMO+0x160 + materialId*0x40 + 0x1c`, 0x7a6d70).
- **Ramp coordinate:** `(u, d/255)` from MLIQ vertex byte 0, with u = 0 for
  exterior groups and 1 for interior ones. proceduralWmoWaterTex holds the
  river far colour in columns 0-3 and white in 4-7, so exterior WMO water is
  the river far colour and interior is white x the MOMT colour.
- **Texture uv:** water-format MLIQ `(vertex - corner).xy * 0.24` (0xa3fab4),
  one repeat per 25/6-yard cell - four times denser than terrain water;
  magma-format MLIQ the s16 s,t pair / 256.
- **Interior** (0x793d20): the group has no 0x48 flag (or its liquid flag 0x2)
  and the type lacks 0x200. Interior water types become LiquidType 17.
- **Light** (CClientEnvironment 0x7d5120 / 0x7d4f40): outside the DayNight
  light; inside a fixed light, direction (0, 0, -1), every colour 1.0.

## 3. Texture matrices (0x8a5590 water, 0x8a6090 magma)

- **Stage 0 (the ramp):** `(u, v * Float[2])` - 1.0 everywhere but Orange
  Slime's 8.5.
- **Stage 1 (the frame):** `u' = (x c - y s) F0, v' = (x s + y c) F0`, no
  translation (0x4c3290, 0x4c1bf0), with the angle **Float[1] x 57.29578
  (0x9ed910) passed to cos/sin as radians** - the degrees/radians factor
  upside down. Only WMO Ocean has a Float[1] (180): 10313.24 rad, cos
  -0.82054, sin 0.57159, about 145.14 degrees; with F0 = 0.25 it repeats
  every 16.67 yards like the terrain ocean.
- **Magma and slime:** `uv + (frac(t / round(1000/F0)), frac(t / round(1000/F1)))`,
  scale 1 (0xb23f64 set by 0x8a1770). ADT lava and slime (0.025, 0) scroll u
  once every 40 s; WMO magma/slime, CoA and Chamber magma (0, 0.1) scroll v
  once every 10 s.

## 4. The texture sequences

0x8a2450 tries `%d` = 1..30 and keeps what loads; 0x8a1d60 shows frame
`floor(N * (t % period) / period)` with period LiquidType Int[1] (1250 ms on
every water material row; magma passes 1250 itself). So lake_a, ocean_h, lava,
slime, lavagreen and LavaOrange (30 frames each) run at 24 frames a second and
fast_a (16) at 12.8.

## 5. Shading

As world-lighting.md §7, with the vertex terms now known:

```
vsLiquidWater:  primary   = (c34 + c35 * sat(N . -L)) * attrib4      (colour register clamps)
                secondary = pow(max(N . -H, 0), 6) * c36             (c36 = local lights only)
psLiquidWater:  rgb = primary.rgb * ramp.rgb + tex.rgb + (secondary + 0.25) * tex.a
                a   = primary.a * ramp.a
psLiquidMagma:  rgb = attrib4 * tex.rgb, a = 1                       (unlit, fogged)
```

- **No reflection, of anything.** Water and NoSpec bind only the ramp and the
  frame; magma only the frame. The procedural material (3) has two cube maps,
  but they are the static `XTextures\procWater\basicReflectionMap.blp` and
  only LiquidType 100 uses that material, which no map does. Nothing in the
  world render draws a mirrored camera or a render target for water (the
  "Reflections" strings in the exe are EAX reverb settings).
- **No waves.** No shipped vertex shader moves a vertex, and CWaveManager
  (0x7d6240) is an all-zero block read only by the unused procedural draw.
- **No sun glint.** The specular colour is the local lights' (0x834f60);
  under the sun only the constant 0.25 x tex.a shimmer is left. The
  `specular` option (0xce04a0 -> 0xb23f68) picks Water over WaterNoSpec,
  which drops even that.
- **No fresnel, no shoreline foam, no horizon fade.** The only shallow-water
  effect is the ramp row from the depth byte; the far water goes into the fog.
- **No detail levels.** `waterLOD` rejects anything but 0 ("waterLOD fixed to
  0", 0x78d8b0); the Lua `SetWaterDetail` (0x5101d0) checks its argument and
  does nothing, `GetWaterDetail` (0x608560) returns 0.
- **Draw state** (decoded through the GL device at 0x69cdc6): culling off (seen
  from below as well), alpha blend (SrcAlpha, 1-SrcAlpha), alpha test >= 1/255,
  fog on, depth test on; depth write is `MaxUserClipPlanes == 0`, i.e. off on
  any real card. Magma sets only culling off and fog on, and draws opaque.

## 6. Underwater

- **The test** (0x790920, every frame, from the camera eye 0xcd8f5c, not the
  player): outside WMOs 0x7a0b00 checks WMO liquids whose box holds the eye,
  then the terrain chunk's liquid (mask 0x7ce1f0, height 0x7ce0b0). In liquid
  when **eye z < surface + 0.01** (0x9f1968) and the ground under the eye is
  below it. Inside a WMO group, 0x7c8360 on the MLIQ grid (cells of 25/6
  yards, a tile with flags & 0xf == 0xf is dry), allowance 0.01 only with
  LiquidType Flags 0x4. No hysteresis. The depth (0xcd8790) is surface - z.
- **What it changes:**
  - the light: the underwater LightParams slot; LiquidType.LightID replaces
    the light outright (magma 7, slime and Green Lava 6); ocean's darkening
    by depth (MaxDarkenDepth 30, 0.5 / 0.5 / 0.25); in fog mode 1 the fog
    exponent doubles (world-lighting.md §1-2);
  - **the sky is not drawn** - 0x79a870 calls the sky (0x7f09b0) only while
    0xcd8794 is 0 - and the frame is **cleared to the fog colour**
    (DayNight+0x8c, the underwater fog);
  - the sun/moon fade over the first 10 yards (0x7eda30: 1 - clamp(depth/10));
  - the particulates (below), the sound (0x5fe7bc, sound-engine.md), the M2
    transparent sort for models crossing the plane (0x821dc4).
- **There is no screen tint, overlay or waterline.** Every reader of the
  camera's liquid (0x77f9dc, 0x780632, 0x7833dc, 0x7834e5, 0x79ab74, 0x79acaf)
  and every caller of 0x780620 was checked; none draws a full-screen quad.
  A half-submerged camera is simply in or out by the eye's height: the whole
  view switches. The surface seen from below is the same two-sided sheet.
- `transparent_underwater` (0xa3bc48) sits in a dead debug-name table at
  0xadef28 that no code reads.

### The particulates (`waterParticulates`)

- One object (0x79e100, made in World init 0x780f50: unit 1/36, a 30-yard box,
  `Textures\WaterPoop02.blp`) of **4000 motes**, each a camera-relative point
  in [-15, 15]^3 and a size of **rand[0.5, 1.5] x ParticleScale / 36** yards
  (water 0.014-0.042, magma 0.056-0.167).
- **On by default** (world flag 0x2000000 in 0x780f5b's 0x7104b73; the
  `waterParticulates` console command toggles it). Shown while the camera is
  in a liquid with Flags 0x8 - water, ocean and magma, not slime. A new set is
  made (0x79b8e0) on entering a liquid and whenever the camera jumps more than
  30 yards in a frame.
- **Each frame** (0x79bf40): every mote takes the camera's move off (it stays
  put in the world), the drift (0x79be50) on, and wraps into the box by 30.
  ParticleMovement 0 (water) is a "current": direction from two angles over
  [-pi, pi] as (cos b sin a, sin b sin a, |cos a| / 4), normalised, never
  down; rate rand[1,2) x 0.0125, amplitude rand[1,2) x 0.005; the push is
  `dir * amp * sin(2 pi phase rate)` **per frame** (not scaled by time), and a
  new current is drawn once phase x rate passes 0.5 - a 20-40 s swell of up
  to 0.005-0.01 yards a frame. Movement 1 (every magma) sinks 0.02 yd/s, 2
  rises 0.02 yd/s.
- **Drawn** (0x79ca70): camera-facing quads built in view space (corners
  0xadf808), only those with view depth d > 0 and |x|, |y| < d (a 90 degree
  cone), the **first 666** in array order (vertex buffer 0xa68), white, alpha
  blended with alpha ref 1, no depth write, lighting off, fixed function; fog
  from Flags 0x10 (water unfogged, magma fogged). The texture is a 5 x 5 atlas
  of 51/256 cells (table 0xadf838: rows of four for cells 0-11, the fifth
  column 12-16, rows 3 and 4 for 17-24); the cell is table 0xa3f970 [slots][(i-1)
  & 7] - water 0..7, magma 8..11 - and mote 0 always cell 8.

### Ripples, splashes and wakes (`waterRipples`) - off by default

A pool of 128 projected decals (Water0Ripple, 0x79e080; init 0x79e1a0) with
`XTextures\splash\splash.blp` and `wake.blp`, spawned per unit from movement
(0x71cba0) where the unit's depth is under max(1, 2 x its height): wake when
moving, splash otherwise; size about 1/3-5/3 growing 1-1.5 yd/s, life 0.6-0.7
s, alpha up over 0.4 of it and down over 0.6, additive, unfogged, depth
offset farclip/1024; a new one every 400 ms + rand (about 250 ms x speed
factor when moving). **But spawning (0x79d460) needs 0xadf7f0, which only the
console command `waterRipples 1` (0x77f690) sets - there is no CVar and the
default is 0.** A default 3.3.5a client draws none. `RainDropSplash01.blp` is
the weather's.

There are no breath bubbles in the client code either: "Breath Underwater"
and "Inebriated Bubbles" are SoundEntries names.

### Magma and slime

Unlit texture (vertex colour white outside, the MOMT colour in an interior
WMO group), opaque, fogged, scrolled. No glow pass: what reads as glow is the
full-bright texture plus LightID 7's light while the camera is in it, and
outside the doodads (lava bubbles, smoke) the zones place themselves. Magma's
motes are the particulates above at scale 4, sinking and fogged.

## 7. Wowee before and after

| Feature | Stock | Wowee before | Wowee after |
|---|---|---|---|
| Ramp row | per-vertex depth byte / 42 (river) or / 255 (ocean, WMO), clamped per vertex; 255 where no data | scene depth buffer under the pixel, ramp over 6 yd (river) / 15 yd (ocean) | stock: MH2O / MCLQ / MLIQ depth bytes kept by the loaders, ramp v per vertex |
| Terrain uv | world x 0.06 | world / 16.67 | stock (same scale, now per vertex) |
| WMO water uv | (v - corner) x 0.24: a repeat per cell | world / 16.67 | stock |
| WMO Ocean | F0 0.25, rotated about 145.14 degrees | / 16.67 x 0.25 (one repeat per 66 yd), not rotated | stock, slip included |
| Magma / slime uv | the data's own (u16 x 3/256; MLIQ s16 / 256) + scroll | world / 16.67 + scroll | stock |
| Orange Slime ramp | v x 8.5 | not applied | stock |
| Interior WMO water | white ramp x MOMT colour, fixed light 1 | river far x sun light | stock |
| Interior WMO magma / slime | MOMT colour x texture | texture | stock |
| MCLQ liquid type | 1-4 | 0-3 (ocean read as LiquidType 1, river) | stock 1-4 (Green Lava on 530 not done) |
| Reflection | none | sky mirrored, fresnel <= 0.35, always | "Water reflections" setting, **off by default**; the reflection pass is skipped when off |
| Shoreline foam | none | "Shoreline foam" setting (on from Medium) | unchanged setting; the scene depth copy is now taken only while it is on |
| Wake froth on the surface | none | always | "Splashes, wakes and bubbles" setting, off by default |
| Swim spray, ripple droplets, wading spray, bubbles, reed midges | none (ripple decals off by default) | always | same setting, off by default |
| Underwater motes | 4000 WaterPoop02 motes, per LiquidType | none | stock (`stock_particulates.hpp`, drawn by SwimEffects) |
| Horizon alpha fade | none | 600-2400 yd fade | removed |
| Alpha test | >= 1/255 | none | stock |
| Underwater tint | none | full-screen tint 0.38-0.75 and a per-pixel waterline seam | removed |
| Sky under liquid | not drawn, frame cleared to the fog colour | drawn | stock |
| Camera test | eye z < surface + 0.01 | eye z < surface | stock |
| Water geometry, frames, light, ramp builders, magma unlit | as §5 | done earlier (world-lighting.md §7) | unchanged |

Settings added: `waterreflections` ("Water reflections", default off) and
`watersplashes` ("Splashes, wakes and bubbles", default off), both on the
Advanced page beside "Shoreline foam"; saved as `water_reflections` and
`water_splashes`. The old "Water refraction" switch did nothing and still does
nothing.

## 8. Gaps

- The terrain liquid test does not check that the ground under the eye is
  below it, and WMO liquids use the same test as terrain (the Flags 0x4
  allowance is not applied).
- The sun/moon fade over the first 10 yards under water is not done; the sky
  (sun and moon with it) is simply not drawn under liquid.
- The ripple decals are not implemented (stock has them off).
- Green Lava for MCLQ magma on map 530 needs the map id in the ADT loader.
- The 0x8a2bf0 ramp is sampled analytically, not from an 8 x 64 texture, so
  the 8.8 fixed point of its alpha is not reproduced.
- A mote set made on a change of liquid takes the new liquid's scale at once;
  stock takes the old one's for that first set (0x790920 regenerates before
  it stores the scale).
- The particulates are drawn with the swim effects, after the water sheet
  (stock draws them with the world, 0x77f9d0); this only matters where a mote
  meets the surface.

## 9. Measured (Nov 2026)

Interleaved runs of the same tour (another client shared the GPU, so single
runs were noisy), mean fps over 30 s windows after 40 s settling, 1251 x 986:

| Coast | Before (2 runs) | After (2 runs) |
|---|---|---|
| Howling Fjord, Valgarde harbour | 37.6 | 41.7 |
| Westfall, Longshore | 29.2 | 34.7 |
| Booty Bay harbour | 28.5 | 30.3 |

The reflection pass (a second sky into a 512 x 512 target) no longer runs by
default, and with "Shoreline foam" off the scene depth copy is skipped too.
