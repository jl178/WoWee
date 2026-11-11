# Terrain (stock 3.3.5a build 12340)

How the stock client draws the ground: which shader it picks, how the texture
layers blend, what the alpha, shadow and colour maps do, the scrolling and
unlit layers, the specular highlight, holes, the distant mountains (WDL), the
far clip and the draw-distance options, and how the tiles stream. Reverse
engineered from `Wow.exe` with Ghidra; addresses are virtual addresses in that
binary. The stock terrain programs were read as ARB text out of
`Data/expansions/wotlk/shaders/vertex/arbvp1/terrain.bls` (128 variants) and
`pixel/arbfp1/terrain0.bls` (3), `terrain0_env.bls` (1), `terrain1.bls` (32),
`terrain2.bls` (32), `terrain3.bls` (96) and `terrainsm.bls`. The lighting and
fog side (the light sum, the 70% shadow floor, the fog curve) is in
[world-lighting.md](world-lighting.md) sections 2 and 3 and is not repeated.

The pure maths is in `include/rendering/stock_terrain.hpp` with tests in
`tests/test_stock_terrain.cpp`.

## 1. The programs and how one is picked

### Loading (0x79e7c0, 0x79e4f0)
- Vertex: `Shaders\Vertex\Terrain`, 128 variants, into the table at
  `0xce0008`.
- Pixel:
  - `Terrain0` (3) at `0xce0488`, `Terrain0_env` (1) at `0xce0004`: the
    multi-pass programs, one layer a pass.
  - `Terrain1` (32) at `0xce0408`: one pass, every layer, no shadow map.
  - `Terrain2` / `Terrain2_pcf` (32) at `0xce0388` and `Terrain3` /
    `Terrain3_pcf` (96) at `0xce0208`: the same with the shadow map (the
    `_pcf` set when `hwPCF` is on, `0xd43014`).
  - `Terrain1w*` for older pixel-shader levels; `TerrainSM` is the shadow
    map's own depth program.
- The ARB files hold the same variants in the same order as every other
  profile, so the arbfp1/arbvp1 text gives each variant's formula.

### The vertex variant (0x79e470)
Index = `shadowMap + 2·env + 4·mccv + 8·specular + 16·(layers − 1) +
64·pointLights` (called from 0x7d0050), confirmed by the variant bodies:

| bit | what it adds | from |
|---|---|---|
| 0 | the shadow map's coordinates (texcoord 3-6, c37-c48) | 0x873ff0, the shadow map is on |
| 1 | layer 0 is a cube map: a reflection vector instead of a 2D coordinate | chunk flag 4 (`+0xa`) |
| 2 | MCCV: `vertex.attrib[4]` instead of the constant 0.5 | `0xd1d06c == 2`, set from **MPHD 0x2** at 0x7bfcb2 |
| 3 | specular: `color.secondary = pow(max(N·H, 0), c27.w) · c27.rgb` | `0xce049d`, the `specular` CVar with pixel shaders |
| 4-5 | layer count − 1: one 2D coordinate per layer plus the alpha map's | the chunk |
| 6 | three point lights (c28-c36, attenuated per vertex) | 0x8349e0 found a local light near the chunk |

### The pixel variant (0x79e5c0)
Index = `cube + 2·(layers − 1 + 4·(bigAlpha + 2·unlit))` into Terrain1's 32:

| variants | blend | layers |
|---|---|---|
| 0-7 | **chain of lerps**: `lerp(lerp(lerp(t0, t1, a.x), t2, a.y), t3, a.z)` | lit |
| 8-15 | **weights**: `t0·(1 − sat(a.x + a.y + a.z)) + t1·a.x + t2·a.y + t3·a.z` | lit |
| 16-23 | chain, with per-layer c1 | a layer whose c1 is 0 is unlit |
| 24-31 | weights, with per-layer c1 | the same |

- `bigAlpha` is **MPHD flag 0x4** (`0xcf08d0` bit 2; the same word's 0x2 bit
  picks the MCCV vertex format at 0x7bfcb2, so it is the WDT's MPHD).
  Of the continents only Northrend has it (MPHD 0xe); Azeroth, Kalimdor and
  Outland have 0.
- `unlit` is the render chunk's flag 1 (`+0xa`): some layer has MCLY 0x80
  (section 3).
- The terrain draw (0x7989c0) groups the visible chunks by these four
  combinations and by layer count and draws each group with its program
  (0x7d2d70). Without pixel shaders it falls back to the multi-pass path
  (0x7d2520, 0x7d20a0: Terrain0, the first pass opaque, the rest alpha
  blended for the chain or added with black fog for the weights); the
  arithmetic is the same.

### The pixel program, whole (terrain1.bls variant 6)
```
c   = blend(t0..t3, alphaMap.rgb)          // the table above; .a blends too
out = primary · c · (0.7 + 0.3·shadow) · 2 + secondary · c.a · shadow
```
- `alphaMap.rgb` holds layers 1-3, `alphaMap.a` the MCSH shadow (section 4).
- `primary` is the vertex light × 0.5 (or × MCCV): world-lighting.md §3.
- `secondary` is the highlight (section 5), masked by the blended alpha and
  the shadow, **not** doubled and not floored.
- Terrain2/3 take the shadow as `min(shadow, shadowMap)` (5-tap PCF).

## 2. Layers, texture scale and the alpha maps

### Texture coordinates (vertex program, 0x7d0050)
- Every layer: `uv = (pos − c23).yx · c18+i + c13+i`.
  - `c23` is the chunk's corner (the render chunk `+0x18`).
  - `c18+i = (−k, −k)`, `k = 1/4.1667` = **0.24 per yard** (0x7c3d90:
    `0xd25488 = −1 / 0xd254a8`, the quad size −4.1667 in the vertex table
    0x7c3c60). **One repeat per quad, eight per chunk**, for every layer. There
    is no per-layer scale in 3.3.5a: MTXF has no scale bits (they arrive in
    Cataclysm) and the client reads only its bit 0 (section 6).
  - `c13+i` is the layer's scroll offset (section 3), 0 otherwise.
- The alpha map: `uv = (pos − c23).yx · (−k/8) + c(13+n)`, i.e. **0 to 1
  across the chunk**, the whole 64 texels; when two chunks share one alpha
  texture (render chunk `+8` bits 1/2), one axis is halved (−0.0625·k) and
  the offset picks the half.

### The alpha texture ("TerrainBlend", 0x7b7a70, 0x7b99b0)
- One texture per chunk (or chunk pair): RGB = layers 1-3's alpha, A = the
  MCSH shadow.
- Format: `genformat` 3 = ARGB4444 without big alpha, 2 = ARGB8888 with it
  (`terrainAlphaBitDepth`, default 8, decides only the GPU format when big
  alpha is on: `(depth != 8) + 2`).
- Size: `64 >> shadowLevel` (0x7b99b0). `shadowLevel` is the "Terrain
  Blending" option (`SetTerrainMip(v)` stores `1 − v`, 0x54eb40), default 1:
  **32x32** on the Low to High presets, 64x64 on Ultra (`TerrainDetail` 1 in
  `GraphicsQualityLevels`).
- Unpacking (0x7b87f0):
  - Without big alpha: four bits a texel (0x7b7530 reads `size/2` bytes a row
    per layer), and unless the chunk has **MCNK 0x8000** the last row and
    column copy the ones before (0x7b85a0 repeats the last row; 0x7b8620 is
    the "do not fix" path). A big-alpha chunk without 0x8000 is an error
    ("Bad genformat", 0x7b87f0); every Northrend and Outland chunk has it.
  - With big alpha: eight bits (0x7b74a0), **compressed** when MCLY 0x200 is
    set (0x7b7420: a run byte, bit 7 fill or copy, low seven bits the count).
  - **A layer without MCLY 0x100** gets the zero row `0xd1d018`: it covers
    nothing. (No shipped layer above 0 lacks the flag.)
  - Second chunk of a shared texture: its layers are matched to the first's
    by texture id, and its base layer's slot gets `255 − sum(others)`
    (0x7b7c60 `param_6`). That is packing, not a blending rule.

## 3. Scrolling and unlit layers (MCLY)

| MCLY bits | meaning | where |
|---|---|---|
| 0x007 | scroll direction, table `0xadee78` | 0x7831a0 |
| 0x038 | scroll speed, divisor table `0xaf14f8` = 64, 48, 32, 16, 8, 4, 2, 1 | 0x7d2d70 |
| 0x040 | scrolls | 0x7d2d70 |
| 0x080 | **unlit** ("overbright"): pixel c1 = 0 for the layer | 0x7d2d70 |
| 0x100 | has an alpha map | 0x7b99b0 |
| 0x200 | alpha map compressed | 0x7b74a0 |
| 0x400 | cube-map layer (with MTXF bit 0) | 0x7d6980 |

- **Scroll.** 0x7831a0 keeps eight (x, y) accumulators, one per direction
  `(−1,0) (−1,1) (0,1) (1,1) (1,0) (1,−1) (0,−1) (−1,−1)`, each axis `+= dir ·
  frameSeconds`, reset to 0 on reaching **64** (only the upper end: a falling
  axis never wraps). The layer's offset is `c13+i = −acc · (1/0.24) / speed ·
  0.125` (in repeats); the program adds it as `.yx`. Speed 7 is 0.52
  repeats (2.2 yards) a second, speed 0 1/64 of that.
- **Unlit.** In variants 16-31 each layer is `c1[i] ≠ 0 ? primary·t·2 : t`
  before blending; the result still takes the shadow floor and the fog. The
  multi-pass path does the same with the ambient (c25) forced to 1. This is
  what makes lava glow: 1999 layers in Outland, 204 in Azeroth.
- Shipped counts (every ADT): Azeroth 164 scrolling / 204 unlit, Kalimdor 3 /
  52, Outland 426 / 1999, Northrend 0 / 1, plus 968 cube layers in 39 tiles
  (Crystalsong).

## 4. Shadow, colour, normals, holes

- **MCSH** (shadow): the alpha texture's A, bit set → 0x00 (shadowed), clear
  → 0xFF (tables `0xa4004c`, `0xa4000c`, `0xa4002c`), only when the chunk has
  MCNK flag 1 and `showShadow` is on (`0xcd774c` & 0x40). Shown as 70% of
  the colour (world-lighting.md §3). Same coordinates as the alpha maps.
- **MCCV**: used only on maps with **MPHD 0x2** (the vertex format at
  0x7bfcb2; the colour attribute exists only there). A scan of every ADT:
  only Northrend's chunks carry MCCV at all (all 16128 sampled; 1115 of them
  not neutral), so the rule changes nothing on the shipped maps.
- **Normals** (MCNR): per vertex, `normal = byte/127`, transformed by the
  model matrix (c0-c2) and lit per vertex in the program.
- **Holes**: MCNK's 16-bit mask, bit `x/2 + (y/2)·4` (table `0xa3faf0`,
  0x7d8730) for quad (x, y) of the 8x8: a 2x2 block of quads each.
- **Mesh**: 9x9 outer + 8x8 inner vertices, four triangles round each inner
  one (the vertex table 0x7c3c60, quad 4.1667 yards).

## 5. Specular ("Terrain Highlights")

- The `specular` CVar (default "0", on in quality levels 2-5 of
  `GraphicsQualityLevels`, the Video panel's "Specular Lighting") needs pixel
  shaders (`0xce049d`, 0x7bd8a0).
- **Textures** (0x7d6980): with it on, each MTEX texture whose MTXF bit 0 is
  clear is loaded as `<name>_s.blp`. Those carry the same colours with the
  highlight mask in alpha. 1079 of the 2364 tileset files are `_s`.
- **Colour and power** (0x7cfbe0): the terrain's lighting object is filled
  from the global Gx light record (`0xce04a8` + 0x58, 0x834f60), whose +0x48
  is the sun colour (DayNight channel 9, world-lighting.md §1); 0x8355d0
  hands ambient (c25), diffuse (c26) and that **specular colour (c27)**,
  and `c27.w = 20` (`0xa3fff0`).
- **The term** (vertex variants with bit 3): `H = normalize(normalize(−P) +
  L)`, `secondary = pow(max(N·H, 0), 20) · c27`, **per vertex**; the pixel
  program adds `secondary · blendedAlpha · shadow`.

## 6. MTXF, height and cube textures

- 3.3.5a reads one bit of MTXF (`+0xb4` per texture, 0x7d6980): bit 0 = the
  texture is a cube map; it is not given the `_s` name, and without pixel
  shaders it becomes black.
- There are **no height textures** (`_h`) in 3.3.5a: no `_h` tileset ships,
  no `_h` name is built, and the programs have no parallax.
- Cube layers: MCLY 0x400 with an MTXF bit-0 texture (`tileset\terrain cube
  maps`); vertex variant bit 1 (reflection vector), pixel `CUBE` sampler on
  layer 0 (odd variants). Only Crystalsong uses them.

## 7. The far clip and the draw-distance options

| CVar | default | range / effect | where |
|---|---|---|---|
| `farclip` | 350 | Video "View Distance" 177-1277. Clamped by 0x780770 to 183.3-791.7 on maps below 530 (unless `farClipOverride`), up to 1583.3 from 530 on with more than 1 GB of video memory | 0x78e50d, `0xcd8580` → `0xcd7748` |
| `horizonFarclipScale` | 4.0 | 3-6: the WDL's far plane in far clips | 0x78d7c0 → `0xadeecc` |
| `horizonNearclipScale` | 0.7 | 0.01-1 | 0x78d810 |
| `shadowLevel` | 1 | alpha/shadow texture `64 >> level`; "Terrain Blending" = `1 − level` | `0xcd8578` → `0xcd7550` |
| `terrainAlphaBitDepth` | 8 | the big-alpha texture format | `0xcd85c4` → `0xcd765c` |
| `groundEffectDensity` | 16 | 16-256 | 0x78dab0 |
| `groundEffectDist` | 70 | 0-140 | 0x78db10 |
| `specular` | 0 | section 5 | `0xcd858c` |
| `showLowDetail` | on (`0xcd774c` & 0x4000000) | the horizon (section 8) | 0x77f820 |

`terrainMipLevel` does not exist in 3.3.5a; `SetTerrainMip` writes
`shadowLevel`.

## 8. The horizon: WDL far terrain

- **Load** (0x7cc310, `MapLowDetail.cpp`): `%s\%s.wdl`. MAOF holds 64x64
  offsets, **index = second ADT number × 64 + first** (checked against the
  ADT heights: `azeroth_32_48`'s first MCVT row is MARE's first outer row);
  each MARE is 17x17 outer + 16x16 inner int16 heights (yards), one cell per
  chunk, optionally followed by MAHO (16 rows of 16 hole bits). MWMO/MWID/MODF
  carry low-detail WMOs (drawn by the same pass, section 9).
- **Pick** (0x7cc0b0): tiles within `horizonFarclipScale · farclip ·
  0.03` chunks of the camera, frustum-tested (0x78fb20), not hidden by the
  occluders (0x7cce00), and with a far clip over 300 not wholly inside it
  (0x78fdc0).
- **Draw** (0x795f80, after the terrain, 0x79ac3a):
  - its own projection (0x791170): near = far clip − **50**, far =
    `horizonFarclipScale` × far clip;
  - depth range **0.998-0.999** (`0xadeee8`/`0xadeeec`), just in front of the
    sky's 0.999-1.0 (world-lighting.md §5);
  - fog start 0, end 1, colour = the DayNight fog colour: **every pixel is the
    fog colour**. The distant mountains are a flat silhouette in the haze's
    colour against the sky, which is how they read in the stock client;
  - 0x7d5e70 draws each tile with `lowDetailIndexPool` (0x79ec18; the hole
    variant splits the indices in two).

## 9. Streaming (0x780860, 0x7b5950)

- The loaded area is a square of **chunks** round the camera: half-size
  `1 + r·0.03` (rounded to even, plus 2-3 chunks of margin, at least 8), with
  `r` the distance to the view frustum's far corner (≈ 1.25 far clips).
- Tiles (`>> 4`) touching it are loaded, **nearest first** (qsort on the
  distance to each tile's box, 0x7b4830); tiles leaving it are unloaded at
  once.
- Only the WDL covers what lies past it.

## 9b. Ground effect placement (0x7d34c0)

Per map chunk, with `groundEffectDensity` (0xcd773c; default **16**, range
16-256, set through 0x78dab0 -> 0x780710):

1. **Samples.** `groundEffectDensity` times, a random quad (`rand & 7` twice)
   goes into a table at 0xd30148; a byte mask marks the quads already set up
   (their bounds and heights are prepared once), so a quad can be drawn more
   than once and repeats are not re-rolled.
2. **Per sample** (second loop, 0x7d389f): skipped if the quad's
   `noEffectDoodad` bit (MCNK +0x50) or its hole bit is set. The layer is the
   quad's two bits of `doodadMapping` (MCNK +0x40), the effect that layer's
   MCLY `effectId` (+0xc), the row `GroundEffectTexture`. A row with density
   0 counts as 8 (`[row + 0x24]`).
3. A 16-slot table of the row's four doodads by their weights (each doodad
   `weight` times at stride 13, the rest filled round-robin), then **density
   pieces** in the quad, each at a random point, slot `(piece + sample) & 15`;
   an empty slot (a row with no doodads) places nothing. No cap per quad or
   chunk was seen; each piece goes to 0x7b31e0 with a random scale.

So a quad expects `density x groundEffectDensity / 64` pieces, in clumps of
`density` (at the default 16, about 14 of the 64 quads get one clump each).

Wowee (`TerrainManager::generateGroundClutterPlacements`) gives each unmasked
quad `density x g / 512` pieces, spread evenly, with its slider's 0-150%
mapped to `g` = 0-64 up to 70% and 64-256 above, and at most 8 a quad. At the
Classic preset's 100% (`g` 136) that is 2.1 per quad for density 8, against
stock's 2.0 at its default 16: the same amount, without stock's clumps. Its
70% default is half of stock's, the Enhanced preset's 150% twice it.

Durotar's sparse clutter is the data, in both clients. Round the Valley of
Trials (`kalimdor_39_33`) the ground is `DurotarDirt`, effect 1949: density 8
and **no doodads**, so it grows nothing. Only `DurotarDirtSmooth` (1972,
density 3: DurBus01/03, DurRoc02/04) does, on a few quads, and 42% of the
quads are masked (82% round Razor Hill). the reference client's split ADT of the same
tile (`_tex0` and root, 1.60.1.70862) has the same MCLY effect ids, the same
doodad mapping and masks, and its `_obj0` the same 102 M2 placements within
80 yd, by model; its `GroundEffectTexture` rows 1949 and 1972 match 3.3.5's.
Pieces within 32 yd that the data asks for at Wowee's 70% / 150%
(`shadows/clutter_audit.py` in the session scratchpad):

| place | quads masked | 70% | 150% | effects that grow |
|---|---|---|---|---|
| Valley of Trials | 42% | 11 | 45 | 1972 (d3) |
| Razor Hill | 82% | 10 | 40 | 1972 (d3) |
| Durotar, south | 43% | 21 | 82 | 1972 (d3) |
| Barrens crossroads | 47% | 169 | 676 | 2872, 2873, 2877 (d8) |
| Ashenvale, Astranaar | 37% | 235 | 940 | 2520, 2522 (d8) |
| Goldshire | 44% | 266 | 1062 | 505, 507, 1106 |
| Elwynn forest | 15% | 314 | 1255 | 505, 507, 512, 798, 801, 802, 1106 |

## 10. Wowee before and after

| Feature | Stock | Wowee before | Wowee after |
|---|---|---|---|
| Blend without big alpha | chain of lerps | chain of lerps | unchanged |
| Blend with big alpha (Northrend) | weights, base = 1 − sat(sum) | chain of lerps | **stock** (`chunkMode.x`, `stock_terrain::layerWeights`) |
| Layer without MCLY 0x100 | covers nothing | covers everything | **stock** (none ship) |
| Texture scale | 8 repeats a chunk, every layer | 8 | unchanged |
| Alpha / shadow map coordinates | 0..1 over the chunk (64 texels) | texel centres 0.5..63.5 | **stock** (`terrain_mesh.cpp`) |
| Alpha seam blur | none | 4-tap blur near chunk edges within 260 yd | **removed** |
| 4-bit fix, big alpha, compressed | 0x7b85a0 / 0x7b74a0 / 0x7b7420 | the same rules (by size, not MPHD) | unchanged |
| Scrolling layers (0x40) | 8 directions, 8 speeds, wrap at 64 | static | **stock** (`layerScroll`) |
| Unlit layers (0x80) | texture as it is, shadow floor and fog only | lit like the rest | **stock** |
| Specular | `_s` tilesets, pow(N·H, 20) × sun colour × alpha × shadow, per vertex | none | **stock**, per vertex (`terrain.vert`), `WOWEE_TERRAIN_SPECULAR=0` turns it off |
| MCCV | MPHD 0x2 maps only | when the chunk has it | unchanged (only Northrend has it) |
| Holes | 2x2-quad bits | the same | unchanged |
| WDL horizon | fog-coloured, far clip − 50 to 4 far clips | none: sky past the far clip | **stock** (`TerrainHorizon`), with the WDL's low-detail WMOs; tiles past 2 far clips drawn at a coarse level (128 triangles, Wowee's), `WOWEE_TERRAIN_HORIZON=0` turns it off; the "Lit far horizon" enhancement (E11) shades it under the fog colour; "Far field (the reference client)" (E39, `far_field.hpp`) colours it from the minimap, lit and fogged as the ground, from the far clip less 24 yd (distance in the plane) to 7000 yd |
| Alpha resolution option | 32 or 64 (`shadowLevel`) | 64 | unchanged (= the Ultra preset) |
| Cube-map layers | Crystalsong | drawn as a flat 2D texture | gap |
| Streaming | chunk square ≈ 1.25 far clips, nearest first | tile circle ⌈far/533⌉+1, nearest first, unload at +3 | unchanged |
| Far clip | 177-1277 (UI), clamps 791.7 / 1583.3 | 400-2400 | unchanged |

## 11. Gaps

- **Cube-map layers** (MCLY 0x400 with MTXF bit 0, Crystalsong's 968
  layers): drawn as an ordinary 2D layer, not as the reflection cube.
- **Low-detail WMOs from the WDL** (MWMO/MODF: New Hearthglen's cathedral,
  Valgarde, the oil platform in Northrend): drawn since 2026-11-03 in the
  horizon pass, fog-coloured, by the tiles' reach and near limit
  (`TerrainManager::loadHorizonWmos`, `TerrainHorizon::setFarMesh`); their
  doodads are not, and whether stock culls them by anything else is not
  known.
- **The horizon's occluder test** (0x7cce00) is not done; tiles are only
  frustum and range culled.
- **Per-vertex vs per-pixel**: Wowee lights per pixel (as before); the
  highlight is per vertex, as stock.
- **Alpha resolution option**: always 64x64 (stock Ultra); the 32x32 of the
  lower presets is not offered.
- **Streaming shape**: Wowee keeps its tile circle with an unload margin
  rather than the stock chunk square; both load nearest first.
- **Specular option**: `WOWEE_TERRAIN_SPECULAR=0` only; there is no settings
  checkbox yet.

## 12. What to look at in game

- **Northrend** (Sholazar, Storm Peaks, Grizzly Hills): painted transitions
  are softer where layers overlap, and the base shows through where the
  layers' alphas add to less than one; no lerp-chain "last layer wins".
- **Lava** (Searing Gorge, Burning Steppes, Shadowmoon Valley, Hellfire):
  lava ground glows at full brightness at night and in shade; scrolling lava
  and water layers move.
- **Highlights**: wet rock and snow catch the sun when facing it.
- **The horizon**: past the view distance, the far mountains continue as a
  flat fog-coloured silhouette instead of the sky.
- **Chunk edges**: the alpha maps now meet at chunk borders as in stock,
  without the 4-tap blur Wowee used there.

## 13. Performance (measured Nov 2026)

Measured with `WOWEE_FRAME_PROFILE=1` on the test machine's Intel Iris Xe
(RPL-P, Mesa ANV), a ~1250x990 window, view distance 2400, on a shared and
loaded machine. Runs of different binaries do not land on the same view (the
character's facing after `.go` differs from run to run), so the costs below
come from one run with `WOWEE_TERRAIN_AB_PERIOD=30`, which turns the
highlight and the horizon off and on every 30 s from the same spot; frame and
GPU times are 10-second averages, the first window after each switch dropped.

| Toggle (same run, same view) | on | off |
|---|---|---|
| Horizon, first version (discard on every tile, full mesh), Nagrand | scene 16.9 ms, GPU 23.8 ms | 14.3 ms, 20.9 ms |
| Horizon, final (no discard past the near limit, front to back, coarse past 2 far clips), Nagrand | scene 13.94 ms, GPU 21.1 ms, 43.2 fps | 13.93 ms, 21.0 ms, 45.2 fps |
| Horizon, discard version, Sholazar | scene 7.40 ms | 7.25 ms |
| Highlight (per-vertex term), Nagrand | scene 12.1 ms | 14.3 ms |
| Highlight, Sholazar | scene 10.2 ms | 10.9 ms |

The highlight's cost is under the noise; the horizon's is now too. The
`_s` tilesets are DXT5 where most plain ones are DXT1, twice the bytes per
texel fetched; that cost is in every "on" number above and could not be
separated from the view. Removing the seam blur saves up to four alpha
taps per layer within 260 yards. Whole-binary comparisons (before/after at
Elwynn, Nagrand, Sholazar, Storm Peaks) are in the change report; they are
dominated by which way the camera ended up facing.

`WOWEE_TERRAIN_SPECULAR=0`, `WOWEE_TERRAIN_HORIZON=0` and
`WOWEE_TERRAIN_AB_PERIOD=<s>` are the switches.
