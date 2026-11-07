# World lighting (stock 3.3.5a build 12340)

How the stock client lights and colours the world: the light it picks, the
time-of-day bands, fog, the sky, terrain, WMOs, water, weather and the
full-screen glow. Reverse engineered from `Wow.exe` with Ghidra; addresses are
virtual addresses in that binary. The stock shaders were read as text: the
`Data/expansions/wotlk/shaders/vertex/arbvp1/*.bls` and
`pixel/arbfp1/*.bls` files are ARB assembly, so `terrain.bls`,
`mapobj*.bls`, `vsliquidwater.bls`, `ffxglow.bls` and the rest give the exact
per-vertex and per-pixel formulas. The last part of each section compares
Wowee before and after this work. Section 9 lists the gaps that remain.

The pure maths is in header-only files with unit tests:

| Header | What | Test |
|---|---|---|
| `include/rendering/stock_light_math.hpp` | bands, light-volume paint order and weights, fog curve, terrain light | `tests/test_stock_light_math.cpp` |
| `include/rendering/stock_day_night.hpp` | light, sun and moon directions (earlier work) | `tests/test_sun_direction.cpp` |
| `include/rendering/stock_sky.hpp` | dome rings and colours, highlight, sun and moon sizes, glare, stars, clouds | `tests/test_stock_world_fx.cpp` |
| `include/rendering/stock_wmo_light.hpp` | MOCV fix, portal attenuation, light modes, window glow | `tests/test_stock_world_fx.cpp` |
| `include/rendering/stock_liquid.hpp` | liquid frames, scroll, light, depth ramp | `tests/test_stock_world_fx.cpp` |
| `include/rendering/stock_screen_fx.hpp` | weather amounts, glow, ghost world | `tests/test_stock_world_fx.cpp` |
| `assets/shaders/stock_fog.glsli` | the stock fog curve, for every lit shader | (shader) |

## 1. DayNight: from Light.dbc to one light

### Frame flow
- The world frame (0x7816f0) runs DayNight each frame. First it sets the fog
  mode (0x7ecc90, see section 2). Then it calls:
  - 0x7f3920: the per-frame update. It takes the camera yaw, sets the storm
    weight `0xd38b88 = min(1, weather * 4)`, then calls 0x7f3230 (pick and
    blend the light), 0x7eea90 (the light's direction) and 0x7eecc0 (sun and
    moons).
  - 0x7f1010 (clouds), 0x7eea80 (glares), 0x7f16f0 (fog).
- Then it copies the result into the global Gx light at `0xce04a8`:
  - `+0x88` ambient;
  - `+0x94` diffuse;
  - `+0xa0` channel 9 (the sun colour);
  - the direction via 0x834ae0.
- DayNight's state is one block at `0xd38b00`. Its getter is 0x7ecef0
  (`mov eax, 0xd38b00`). Every consumer reads `[base + offset]`, so the
  fields are found by offset, not by address.

### The DNLight record (0x9c bytes; current one at `0xd38bd4` = base+0xd4)
0x7ebff0 fills it from one LightParams row at one time key:

| index | contents |
|---|---|
| [0] | IntBand channel 1: **ambient** |
| [1] | IntBand channel 0: **diffuse** (the directional light) |
| [2] | channel 8 (loaded, but no renderer reads it) |
| [3..8] | channels 2..7: sky top, middle, band 1, band 2, smog, **fog colour** |
| [9..0x11] | channels 9..17: sun, cloud sun, cloud emissive, cloud layer 1, cloud layer 2, ocean close, ocean far, river close, river far |
| [0x12] | FloatBand 0, the fog end. Sampled ×1/36 by 0x7eaef0 (constant 0xa3f854), so in yards. |
| [0x13] | FloatBand 1, the fog start as a fraction of the end, clamped to [-1, 1] |
| [0x14] | fog exponent: 1.0, or 0x7ecd00's curve in fog mode 1 |
| [0x15] | LightParams.HighlightSky (+4), int to float |
| [0x16] | LightParams.Glow (+0x10) |
| [0x17..0x1a] | FloatBands 2..5. Band 2, "glow through", is never read. Band 3 is the cloud cover. |
| [0x1b..0x1e] | LightParams water shallow/deep alpha and ocean shallow/deep alpha (+0x14..+0x20) |
| [0x1f], [0x20] | LightSkybox id (+8) and weight 1.0. Up to three (id, weight) pairs in [0x1f..0x24]. |
| [0x25], [0x26] | CloudTypeID (+0xc) and 1.0 |

The colour channel order is confirmed at the consumers:
- 0x780cd0, the outdoor M2 lighting object, adds base+0x1ac as the
  **ambient** (0x834d90) and base+0x1a8 with the direction at base+0x19c as
  the **directional** light (0x834dc0).
- 0x7ee750 sets +0x1ac from [0] (channel 1) and +0x1a8 from [1] (channel 0).
- So channel 0 is the diffuse and channel 1 the ambient, as Wowee already had
  them (`LightParamsProfile::DIFFUSE_COLOR = 0`).
- 0x7ee750 also derives two colours:
  - the "window" light: +0x1b0 is the mean of diffuse and ambient, and
    +0x1b4 is that mean + 16/255;
  - +0x1c8, a third of the ambient plus 1.

### Bands
- **Ids.** IntBand id = LightParams × 18 − 17 + channel (0x7ebf30).
  FloatBand id = LightParams × 6 − 5 + channel (0x7ebf90).
- **Time key.** `round(dayFraction × 2880 − 0.5)`, in half-minutes
  (0x7f3230). ROUND is the FPU default: to nearest, ties to even.
- **Finding the keys (0x7eae70).** It walks the keys in file order for the
  first pair (i, i+1 mod n) that holds the time.
  - A pair that runs backwards spans midnight: it holds [a, 2880) and [0, b],
    and 2880 is added to b (and to the time if needed).
  - One key is that value all day.
- **Colours (0x7eb070).** Each byte is interpolated linearly and rounded.
  A band with no keys is black (`0xff000000`).
- **Floats (0x7eaef0).** Linear. A band with no keys is 0.

### Which LightParams: the slots
- Light.dbc +0x1c + slot × 4, read by 0x7eb180. The slots are:
  - 0 clear;
  - 1 underwater (the camera is in liquid);
  - 2 storm;
  - 3 storm underwater;
  - 4 death.
- The death slot is never chosen by the blend. 0x7f3230 has a slot override,
  `0xd38b58`, set by 0x7ecec0 from the ScreenEffect.dbc row (0x4f7020). The
  "Ghost Screen Effect" row gives slot 4. While it is set, the result is the
  map default light's slot alone: no local lights, no storm. Its sky model is
  then the death sky (`0xd38b5c`, weight 1).

### Which lights, and how they are blended (0x7f1360)
- **The light list (0x7ecb30).** Element 0 is the map's default light: the
  Light.dbc row for the map whose position is (0,0,0). 0x7eaf60 leaves that
  row unconverted. A map without one gets Light 1.
- **The default light** is sampled first, at full weight (0x7f3230), with its
  storm set mixed in (0x7ec220) by the storm weight.
- **Which other lights count.** Every other light of the map counts if both:
  - its outer radius is at least **3 yards** (`0xaf4a68`, constant
    0x9ebbc4);
  - the light-pick position (DayNight +0x24, the player) is inside that
    radius, by 3D distance.
- **Order.** Those lights go into a binary heap (0x7f0dc0 push, 0x7f1280
  pop, comparator 0x7ed0a0) keyed by squared distance. They pop **farthest
  first**. Two lights whose centres are within **1/3 yard** (`0xaf4da8`)
  are ordered by inner radius, biggest first.
- **Painting.** Each popped light is painted over the running result by
  0x7ee5d0, then 0x7ed4c0:
  - `w = 1` inside the inner radius;
  - otherwise `w = clamp(1 − (d − inner)/(outer − inner), 0, 1)`, a
    **linear** falloff;
  - `result = lerp(result, light, w)`, colours byte by byte;
  - each light is itself clear or storm-blended first (0x7ee510).
- **No normalising and no limit:** the nearest light ends on top.
- **Sky models along the way.** 0x7ed4c0 adds each painted light's sky model
  id at its weight: summed and capped at 1 if already listed, up to three
  entries.
- **The final sky list (end of 0x7f3230).** A sky model above 0.99 weight
  whose LightSkybox flags lack bit 2 restarts the list, so it replaces the
  ones before it. The result goes to `0xd38b64` (ids), `0xd38b70` (weights)
  and `0xd38b7c` (flags & 2).
- **Timed lights.** Up to 5 extra lights at an explicit percentage, added by
  0x7ed150 (`(id, weight)` at +0x1ec/+0x200), are painted last (0x7ee6b0,
  `w = percent/100`).
  - 0x77eed0 adds them for 11 hard-coded map rectangles (table at
    `0xadef58`).
  - A second list at `0xd3901c` holds lights that fade in (type 1) or out
    (type 2) over a set time. This is the server's light override.

### Storm
- The weather object eases the grade (0x784850). Its storm term is
  `clamp(grade, 0, 0.25)`, eased at a quarter per 10 s (`(|Δ| × 4 + 0.001)
  × 10 s`), times Weather.dbc's fourth field. That field is 1 for rain and
  snow and 0 for the coloured sandstorms.
- The result goes to DayNight+0x4c (0x7849f4).
- 0x7f3920 takes ×4, capped at 1. **A quarter-grade rain is already the whole
  storm light**, ten seconds after a clear sky.
- 0x7ec220 then lerps the clear set toward the storm set by that weight.
  Colours go byte by byte, by `round(t × 255)`. Of the floats it lerps fog
  end, fog scaler, fog exponent, [0x16], [0x18] and [0x1b..0x1e].

### Liquids (0x7f3230)
- The camera's liquid comes from 0x780620. The LiquidType record:
  - **+0x28 LightID.** If set, that light replaces Light.dbc entirely while
    the camera is in the liquid. Magma uses Light 7 and slime Light 6.
  - **Darkening by depth.** With +0x18 MaxDarkenDepth > 0, the ambient and
    diffuse lose HSV value (0x7ed790) by `t × intensity`:
    - `t = clamp(depth / MaxDarkenDepth, 0, 1)`;
    - the intensities are +0x20 (ambient) and +0x24 (diffuse);
    - only Ocean sets them: 30 yd, 0.5 ambient, 0.25 diffuse.
  - **Fog.** The fog colour gets the same treatment from +0x1c. But 0x7f16f0
    rebuilds the fog from channel 7 straight after, so the darkened fog never
    shows.

### Wowee before and after

| Feature | Stock | Wowee before | Wowee after |
|---|---|---|---|
| Light set | map default (0,0,0 row; Light 1 if none) under every local light | top 2 lights by weight, normalised to sum 1 | stock (`findLightVolumes`, `update`) |
| Falloff | linear, inner to outer | smoothstep | linear (`stock_light::volumeWeight`) |
| Blend | painter's lerp, farthest first, nearest on top | weighted average | stock |
| Lights considered | outer ≥ 3 yd, any number | every one, top 2 used | stock |
| Band interpolation | 0x7eae70 pairing, byte-rounded colours, key = round(t·2880 − 0.5) | own search, float colours, floor | stock (`stock_light::sampleColorBand`) |
| Empty band | black / 0 | grey 0.5 / 1.0, fallbacks to other channels | black / 0. The liquid colours keep their fallbacks (water renderer). |
| Ambient + diffuse > 1 | clamped per channel in the shader | both scaled down together (`applyLightHeadroom`) | not scaled. Terrain, M2, WMO and water clamp per channel. |
| Temporal smoothing | none | everything eased at 5/s | none. Only the storm grade eases, as stock. |
| Storm | 4 × min(grade, 0.25), 10 s ramp | grade, linear | stock |
| Death | default light's death set only | death set of the 2 blended lights | stock |
| Liquid light | LightID override (magma, slime), ocean darkening | underwater slot only | stock (`setCameraLiquid`) |
| Sky model list | 0x7ed4c0 accumulate + 0x7f3230 full-weight restart | weights by light share, eased | stock; `SkyboxLayer::replacesSky` drives the procedural-sky switch |
| Stars | stars.mdl by its own table | dnc.db night × (1 − zone sky) | stock table (`stock_sky::starsAlpha`), hidden only by a full sky model |
| Shadow opacity | fixed 0.3 (shaders: 0.7 + 0.3·lit); channel 8 unused | from channel 8, 0.4..0.9 | 0.3 |
| HighlightSky | dawn/dusk dome highlight | not read (0.3 default) | read into `horizonGlow` |
| Glow | LightParams.Glow → ffxGlow | not read | read into `LightingParams::glow` |

## 2. Fog

### The values (0x7f16f0, 0x7ecd80, 0x7ecd00, 0x7ed1b0)
- **Per light, before blending (0x7ecd80):**
  - The fog end is never under **10 yards** (0x9e30cc).
  - **Fog mode 1** (`0xd38acc == 1`): if the fog end is over 1000/36 yards
    (0xa41b20), then:
    - the exponent becomes 0x7ecd00(start, end):
      `span = min(700, farclip) − 200`;
      `exp = (end − start) ≤ span ? (1 − (end − start)/span) × 5.5 + 1.5 : 1.5`;
    - the fog end becomes the **far clip** (`0xd38b40`).
  - Also in mode 1, a negative start fraction is set to 0.
- **The frame's fog (0x7f16f0):**
  - `end = min(farclip, fogEnd)`;
  - `start = end × scaler`;
  - colour = channel 7;
  - exponent = [0x14].
  - Under liquid in mode 1 the exponent doubles (0xa4040c).
- **When mode 1 is on (0x781700).** The current map id `0xadfbc4` is
  ≥ **530**, and the Gx caps (`0xc5df88`+0x214, +0xb4) report vertex shaders.
  - So Azeroth and Kalimdor have **linear fog** ending at the band's fog end.
  - Outland, Northrend and every instance with an id from 530 on run the fog
    out to the far clip on a curve with a power of 1.5 to 7.
- **Handing it to the renderer.** The fog goes to the Gx and M2 lighting
  through 0x834990(colour, start, end, exponent), which stores
  `+0xa8 start, +0xac end, +0xb0 1/(end − start), +0xb4 exponent`.
- **WMO materials** with F_UNFOGGED draw with fog off (0x7a8440). A second fog
  set at base+0xa0 is used for some WMO groups (0x7c1730, group flag 0x8000).

### The curve (every arbvp1 shader)
```
fogcoord = clamp(pow(max(viewZ * c.x + c.y, 0), c.z), 0, 1)   // per vertex
         = clamp(((end − viewZ)/(end − start))^exp, 0, 1)    // the fog factor
shown    = mix(fogColour, colour, fogcoord)
```
- `viewZ` is the **view-space depth**, not the distance.
- **The constant upload (traced, 0x873210).** In shader mode
  (`0xd43020 != 0`) the M2 fog goes to vertex constant **c30 =
  (−s·k, k·end, exponent, 0)** with `k = 1/(end − start)` and `s =
  0xd4300c`, which is only ever written once, to 1.0 (0x872d09). So
  `c.x = −1/(end − start)`, `c.y = end/(end − start)`, and fogcoord is the
  **fog factor itself** (D3D's oFog: 1 the surface, 0 the fog colour), with
  the exponent on it. vsLiquidWater's c4 has the same form (0x8a38b0);
  terrain.bls reads its c12 with the same instructions (that upload is not
  traced). In fixed function (`0xd43020 == 0`) the same call sets Gx render
  states 8/9 (start, end) and 10 (colour) instead.
- **Correction (Nov 2026).** This section first read the curve as
  `shown = 1 − clamp(((z − start)/(end − start))^exp)`, from an assumed
  `c = (1/(end − start), −start/(end − start))` and `ARB_fog_linear` over
  0..1. The upload says otherwise. Both agree for the linear fog of Azeroth
  and Kalimdor (exponent 1). On the far-clip maps (≥ 530) the exponent
  raises what **shows**, so a high exponent (a short band fog) pulls the
  fog back in toward the camera although its end moved out to the far clip,
  instead of holding it back. Example: band fog 75–300 yd, far clip 1277,
  exponent 4.525: halfway between start (319) and end, 4.4% of a surface
  shows (it was 95.6%). `stock_fog.glsli`, `stock_light::fogVisibility` and
  its test now follow the upload.

### Height and underwater fog
- **Height fog:** none. The fog depends on depth alone.
- **Underwater:** it is the underwater LightParams slot's fog (about 170
  yards, teal). There is a separate liquid fog set at `0xd38bb0..0xd38bbc`
  (0x7ed1b0), and in mode 1 the exponent doubles. There is no screen overlay.

### Particles and ribbons
- **Shaders.** `shaders\effects\particle.wfx`: effect `Particle` is
  vertex shader CDiffuse_T1 (lit), `Particle_Unlit` Color_T1, both with
  pixel shader Combiners_Mod. Both write the fogcoord above from c30 (in
  color_t1.bls: `DP4 R1.x, v0, c33` is the view depth, then `MAD c30.x,
  c30.y; MAX 0; POW c30.z; MIN 1`). So particles take the **same curve and
  view depth as every M2 batch**, per vertex.
- **Fog per material** (0x81FB10, from the particle material 0x8214E0 and
  the ribbon element 0x820F40 alike): no fog if the material is unfogged
  or `1/(end − start) ≤ 0`; otherwise the fog colour by M2 blend from table
  **0xA45390** = `{1,1,1,2,2,3,4}`:

  | M2 blend | mode | fog colour |
  |---|---|---|
  | 0 opaque, 1 alpha key, 2 alpha | 1 | the scene's (Gx +0xb8..+0xc0 × 255) |
  | 3 one+one, 4 add | 2 | black |
  | 5 mod | 3 | white 0xffffff |
  | 6 mod2x | 4 | grey 0x808080 |

  then 0x873210(start, end, exponent, colour) and fog on (0x873390).
- **Ribbons.** 0x820F40 points the element's material at the ribbon's
  first material (model material table) and runs the batch setup
  (0x81FB10, so the colour above). The draw (0x980B70) then turns fog on per
  material layer from the layer's bit 1, which 0x833900 sets from the M2
  material flags as `~flags & 0x2`: unfogged materials draw without fog.
- Wowee before: particles and ribbons took a **radial** linear fog
  (`length(camera − p)`), the ribbons faded to black whatever their blend.
  Now: `stockFogVisibility` per vertex (m2_particle_gpu.vert,
  m2_ribbon.vert, and m2_particle.vert for the glow sprites); the particle
  colours were already the table's (Mod2x grey now exactly 128/255); the
  ribbon fragment takes the table's colour from its material's blend and
  unfogged flag (push constant). Wowee's volumetric fog still thins them on
  top, as before.

### Rain and splashes
- Both rain draws (0x78A640 streaks with rain.bls, 0x78A030 splashes with
  patter.bls) set **c0 = (0xd4300c × −0.2, 15, 1, 0)** (0xa3ebdc, 0x9e8d7c)
  and fog on with colour **0x80808080**. Both shaders feed c0 the vertex's
  **height above the camera** (`(v − c1).z`, c1 the camera position
  0xcd8f5c), not its depth: `fogcoord = clamp(15 − 0.2·Δz)`. A drop goes to
  the grey only from 70 to 75 yd above the eye, above the rain box, so in
  practice stock rain is unfogged.
- Snow (snowpoint.bls) writes no fogcoord.
- Wowee before: rain and splashes faded with the scene's radial linear fog.
  Now: weather.vert computes the stock height fade (snow still ignores it).
  The fade toward the box's edge (`vAlpha`) is Wowee's and stays.

### Wowee before and after

| Feature | Stock | Wowee before | Wowee after |
|---|---|---|---|
| Curve | linear on Azeroth; power 1.5–7 to the far clip on maps ≥ 530 | linear everywhere | stock (`stock_fog.glsli`, exponent in `localLightMeta.y`) |
| Distance measure | view-space depth | radial distance | view depth (terrain, M2, WMO, water, characters, grass, particles, ribbons) |
| Fog end vs far clip | min(band end, far clip); mode 1: far clip | band end ÷ strength 0.4, then stretched with the view distance | stock; the view distance is the far clip |
| Fog colour | channel 7 | 70% pulled to the sky's middle colour | channel 7 (sky blend default 0) |
| Fog strength | as authored | 0.4 (2.5× further) | 1.0 on every preset (`graphics_presets.hpp`, the schema, the login state) |
| Minimum end | 10 yd | none | 10 yd |

## 3. Terrain (shaders/vertex/arbvp1/terrain.bls, pixel/arbfp1/terrain0–3.bls)

### Vertex shader (per vertex)
```
light   = min(ambient + diffuse * clamp(N.L, 0, 1), 1)   // per channel
primary = light * (MCCV ? MCCV : 0.5)
spec    = pow(max(N.H, 0), c27.w) * c27.rgb      // only in the specular variants
fog     as section 2
```

### Pixel shader (terrain1, one layer)
```
lit = min(bakedShadow.a, shadowMap)          // terrain2/3 add the shadow map (5-tap PCF)
out = primary * tex * (0.7 + 0.3 * lit) * 2 + spec * tex.a * lit
```

### The pieces
- **Per-vertex lighting** with the vertex normal. The light is clamped before
  the texture, so a channel that passes 1 stops there and the hue leans
  toward the others.
- **MCCV, the ×2 rule.** The colour register holds the MCCV byte as 0..1
  (0x7F ≈ 0.5), and the pixel shader doubles it, so 0x7F is neutral.
  - Without MCCV the vertex shader writes 0.5, which the doubling makes 1.
  - So `ground = light × 2·MCCV × texture`.
- **Shadows.** MCSH is unpacked by 0x7b87f0 (and 0x7b7c60 / 0x7b8070) into
  the alpha map's alpha: bit 0 (lit) becomes 0xFF and bit 1 (shadow) 0x00
  (tables 0xa4000c, 0xa4004c).
  - A shadowed texel shows **70%** of the whole lit colour, ambient included.
  - The shadow map, where it is on, is MINed with it and gets the same 70%
    floor.
- **Specular.** Present when the `specular` CVar is on (it needs pixel
  shaders). It uses the light colour c27 and power c27.w, and is masked by
  the texture alpha (`_s` tilesets) and by the shadow.
- **Layers.**
  - Two layers: `lerp(tex0, tex1, alpha)`.
  - The alpha unpacker writes the base layer's weight as
    `255 − sum(others)` in one path (0x7b7c60, `param_6`).

### Shadow map filter (PCF taps)
- **Offsets** (0x8742e0): eight vec4s at `0xd431d0`, `(x, y, 0, 0) / size` in texels:
  `k0 (0.8,-1.0)  k1 (-0.2,-0.8)  k2 (0.2,-0.6)  k3 (1.0,-0.4)`
  `k4 (-0.6,-0.2)  k5 (0.6,0.2)  k6 (-1.0,-0.4)  k7 (-0.4,-0.6)`.
  The same `1/size` is used for every map.
- **Upload** (pixel-constant setter, vtable +0x118, type 4): terrain (0x874660) puts k0..k7 at
  local[3..10]; M2/WMO/detail doodads (0x874760, 0x8744e0) put them at local[5..12].
- **5-tap** (terrain2_pcf, terrain3_pcf, mapobj*, combiners_*, detaildoodad): the centre plus
  k0, k2, k4, k6 (terrain c3/c5/c7/c9; M2/WMO c5/c7/c9/c11), equal weights ×0.2. WMO is 5-tap
  too, not 4. The **9-tap** terrain3_pcf variant uses the centre plus all eight (c3..c10) ×1/9 on the
  near map (texture[5]); the other cascades stay 5-tap.
- **Edge fade:** `m = max(|s.x|,|s.y|)` (shadow NDC), `f = sat((0.99 - m) / 0.29)`,
  `lit = 1 + f·(avg - 1)`: full shadow inside 0.70, none at 0.99. The cascaded variants
  select the finest map whose `m < 1`; the outermost one fades over 0.90..0.99 (`-11.11m + 11`).
- **Hardware PCF:** CVar `hwPCF` (default 1, needs caps+0xac; 0x78e070 sets `0xd43014`).
  On: a D24 map (format 0xc) with linear filter (flags 0x81), `SHADOW2D` → each tap is a 2×2
  bilinear compare; the `*_pcf` / `OPTION ARB_fragment_program_shadow` programs (0x79e4f0).
  Off: an R32F map (0xb), point filter, `terrain2.bls` does `SGE depth >= ref` per tap.
- **Map size** (0x875d30, `0xd43150`), `extShadowQuality` 0..5 (`0xd43154`, validated 0x8740d0):
  0 none; 1 → 1024; 2 → 2048; 3 → 1024; 4, 5 → 2048. Levels ≥3 add three cascade maps of
  half-extent 40 / 160 / 640 yd (`0xb1d520`), re-centred after moving 2 / 4 / 32 yd; below 5
  they are double-buffered and redrawn over 9 / 9 / 25 frames in tiles (0x874890).

### Wowee before and after

| Feature | Stock | Wowee before | Wowee after |
|---|---|---|---|
| Light sum | min(amb + diff·N·L, 1) per channel, then × texture | amb·tex + diff·N·L·tex, no clamp | stock clamp (`terrain.frag.glsl`) |
| MCCV | × 2·MCCV | × 2·MCCV | unchanged |
| Baked shadow | whole colour × 0.7 | diffuse only × 0.5 | stock 0.7 floor, on the whole colour |
| Shadow map | MIN with baked, same 0.7 floor | diffuse only, darkness from channel 8 | stock (the strength setting scales the map's term) |
| Luminance bump | none | removed earlier | none |
| Specular | `specular` CVar, masked by texture alpha | none | none (gap, section 9) |
| Per-pixel vs per-vertex | per vertex | per pixel | per pixel. Same clamp; smoother. |

## 4. M2 (shaders/vertex/arbvp1/diffuse_*.bls)
- **Formula:** `colour = clamp(clamp(amb + diff × clamp(N·L)) × c28 + c29)`,
  where c28 is the material and vertex colour and c29 the emissive. The fog
  uses c30 with the section 2 curve.
- **Specular:** none.
- **Doubling:** the pixel combiners apply Mod/Mod2x per texture stage.
- **Wowee:** `m2.frag.glsl` now clamps `ambient + shadow × diffuse × N·L` at 1
  per channel before the texture, and uses `stock_fog.glsli`.
- **The doorway ambient for units and doodads** (0x7a1bc0 / 0x7a1e90 /
  0x7c1730) is described in the WMO section. It belongs to the character and
  M2 side and is not changed here. The AreaTable AmbientMultiplier (+0x48)
  feeds it: 0x7831a0 sets `0xadeebc = 2 × mult + 1`, eased.

## 5. Sky: dome, sun, moons, glare, stars, clouds, sky models

Binary: Wow.exe 3.3.5a (12340). DayNight state base 0xd38b00 (getter 0x7ecef0). Pure maths in
`include/rendering/stock_sky.hpp` (namespace `wowee::rendering::stock_sky`).

### Per-frame inputs (0x4f8770, WorldFrame)
- base+0x18 (0xd38b18) camera position; +0x24 player position (light lookup); +0x30 (0xd38b30)
  camera forward, normalised; +0x3c (0xd38b3c) its yaw = atan2(fwd.y, fwd.x) (0x7f3920);
  +0x40 far clip; +0x04 day fraction; +0x08 day count; +0x4c weather intensity
  (storm 0xd38b88 = weather x 4, clamped 1).

### Draw (0x7f09b0), depth range 0.999..1.0 (0xadeef0)
1. If no sky model is up at > 0.99 without LightSkybox flag 2 (and no override/death sky at > 0.99):
   stars.mdl (0x9abd50), sun / White Lady / Blue Child sprites (0x9ac660 on 0xd38e28/48/68),
   the dome (0x9acb00, **GxBlend_Add**), the clouds (0x9acd40, alpha blend).
2. The up-to-three LightSkybox M2s (0xd38b64 ids, 0xd38b70 weights, 0xd38b7c flags&2) at their
   weights as M2 alpha (0x7f08c0, +0x178), then the override sky (0xd38b5c / 0xd38b60).
3. Glares (sunGlare.blp 0xd38ea8, moonGlare.blp 0xd38f58) later, from 0x7f0870 via 0x9ac400,
   additive (blend 3).
Because the dome is additive and drawn last of the procedural set, the sun, moons and stars show
as sprite + sky.

### Dome geometry (0x7f2470) and colour (0x7f0530)
- Unit sphere, centre lowered cos(pi/4) (0x9eaf48, double) so the eye sits on the 45-degree ring;
  24 segments; ring polar angles (x pi, 0xa41a90): 0, 0.17, 0.20, 0.23, 0.24, 0.25, 1.0 -> seen
  from the eye: zenith, 16.8, 9.8, 3.7, 1.8, 0 degrees, nadir. Drawn scaled x6.667 (0xaa9864).
- Colours (DNLight ch2..ch7): apex = top (ch2, through 0x7ed790 HSV scale 1.0), ring 1 middle (3),
  ring 2 band1 (4), ring 3 band2 (5), ring 4 smog (6), ring 5 and the nadir = fog (7). Below the
  horizon everything is the fog colour.
- Highlight h = table 0xaf4b7c (dawn peak 06:30, dusk peak 21:30, 0 from 07:00 to 20:30) x
  LightParams.HighlightSky (DNLight[0x15], 369 of 850 rows). Rings 1-4: lit = lerp(band, middle, h);
  g = table 0xaf4bac at the vertex's world azimuth fraction (1 at 0.125 = north-west, -0.7 at
  0.625); g >= 0: lerp(band, lit, (1-g)h) (FSUBRP 0xdee1 at 0x7f06b6 = 1-g); g < 0:
  lerp(lit, lerp(lit, top, 0.7h), -g h).
- Timed tint 0xd38b50 (alpha byte) / 0xd38b51 (colour) from 0x7f9e10 (fade in/hold/out, callers
  0x7fca30, 0x704963) lerps every dome colour; 0xd38184 bit 0 (lightning flash, 0x7f16f0) lerps
  ch2..7 toward 0xd38d44.

### Sun and moons (0x7eecc0, 0x7edbe0, 0x7edee0)
- Quads `size` yards on a side, 12 yards out (0xa1047c), level-billboarded (0x9abb60).
- Sun size table 0xd39128: 2.0 at 06:00 -> 1.0 by 06:45 ... 1.0 to 20:15 -> 2.0 by 21:00 (x1.0).
  Moon size 0xd39108: 1.0 at 01:00 -> 1.5 at 04:00 ... 22:00 -> 1.0 at 23:59, x1.75 White Lady,
  x1.0 Blue Child; Blue Child phase cycles every 1.7 days (0xd38e84) on the server day count.
- Colour of sun, both moons and both glares = ch9 (sun colour, 0x7f3230 copies 0xd38bf8), alpha
  1 - storm.
- Horizon: clipped at the eye's height and alpha = clamp(z x 2.5) over the last 0.4 yd (0x9f98d8).
  No day/night gating - only geometry.
- Glare (0x7ef6e0): f = clamp((dot(fwd, dir) - 0.7)/0.3); sun size lerp(3, 20, f), alpha
  lerp(0.5, 1, f); moon size 2 x moon size, alpha lerp(0.1, 1, f); x eased visibility (sun rise 4/s,
  fall 1.515/s; moon 3.03/s, 1.515/s) toward target = occlusion (ray cast 0x9abc60, query 0x9abe00)
  x (1 - max sky model weight) x day table (sun 0xd38f18: 06:30 0 -> 07:30 1 ... 19:30 1 -> 21:00 0;
  moon 0xd38fc8: 1 at midnight, 0 by 03:15, back from 22:45).

### Stars (0x7ee0d0)
- Environments\Stars\stars.mdl (0x9abb00) at alpha table 0xaf4c20 (x254+1): 1 to 03:00, 0 by
  04:30, 0 from 22:30, 1 at midnight. Global; not dnc.db, not per zone.

### Clouds (0x7f1b10, 0x7efd00, 0x7efae0, 0x7f20e0)
- CloudTypeID is 0 in every LightParams row; one procedural layer. FloatBand 2 (DNLight[0x17],
  "celestial glow through") is copied but never read; FloatBand 3 (0xd38c34) is the cover.
- Texture 128^2 (SkyCloudLOD 0..3 -> 128..1024, 0xa41aac), double-buffered DNClouds0/1, 8 rows a
  frame: 5 octaves of 3D value noise (time is the third axis; clouds churn, they do not drift),
  frequencies 16..256/256 cell per texel (0xaf4dc4), weights 1..1/16.
- Opacity: v = round(n x 64 + 128) - round((1 - cover) x 255); alpha = 1 - 0.96^v (ramp 0xd38588
  built by 0x7edb50 from 0.96).
- Colour: ch12 + ch11 x shade + ch10 x N.L x (1 - 0.75 storm); shade = ((255 - a)/2 + 64)/255;
  N from the first three octaves' slope; L toward the sun (moon outside 04:50..22:10) placed on the
  texture by its angle from the zenith (capped 45 deg), at height 64 + 192 x storm texels.
  ch13 is unused.
- Dome: same lowered sphere, 12 rings (0xa41ad4: 0 .. 0.25 pi) x 16, polar UVs radius ring/22,
  vertex alpha 1 to ring 8 (8.8 deg), 0.5 at ring 9, 0 on the last two (0xa41b04).

### Gap table
| Feature | Stock | Wowee before | Wowee after |
|---|---|---|---|
| Dome band elevations | 90/16.8/9.8/3.7/1.8/0 deg, linear in polar angle on the lowered sphere | 18/10/3/0/-30 deg linear in elevation | stock rings (skybox.frag) |
| Below horizon | fog colour (ch7) | smog -> fog over 30 deg | fog colour |
| Dawn/dusk highlight | HighlightSky x table x azimuth table | none | implemented; LightParams.HighlightSky read into `horizonGlow` |
| Sun size | 1-2 yd at 12 yd (2.4-4.8 deg half) by time | fixed 3.4 deg half | stock table |
| Sun/moon visibility | always, clipped/faded at the horizon | dnc day/night x FB2 glow-through | stock (horizon clip in shader) |
| Moon colour / blend | ch9, additive (dome added over) | white, alpha blended | ch9, additive |
| Moon size | 1.75 x (1.0..1.5) | fixed | stock |
| Glares | size/alpha by view alignment, eased, day tables, occlusion | fixed size, day x tint | stock (occlusion = renderer ray test) |
| Storm | sun/moon/glare alpha 1 - storm | none | 1 - clamp(weather x 4) |
| Clouds | value noise, 1-0.96^v ramp, ch12+ch11 shade+ch10 N.L, ring fade | domain-warped cumulus + cirrus, own light model | stock model in shader (random table differs) |
| Stars | stars.mdl by fixed table | Stars.mdx by dnc night x (1 - zone sky) | stock table (`stock_sky::starsAlpha` in LightingManager) |
| Blue Child | drawn, 1.7-day cycle | not drawn | not drawn (needs server day count) |
| Sky model rule | > 0.99 & !flag2 removes procedural sky; models drawn after clouds | coverage >= 0.5 hides sun/moon/clouds; models before them | hide rule stock (`SkyboxLayer::replacesSky`); draw order still models first |
| Lens flare | none | off already | off |
| Sun shafts / sharp stars | none | on in High+ presets | unchanged (extras; recommend default off) |


## 6. WMO lighting

Sources: Ghidra on Wow.exe (addresses below), the stock programs in
`Data/expansions/wotlk/shaders/vertex/arbvp1/mapobj*.bls`, `pixel/arbfp1/mapobj*.bls`
(ARB text), `shaders/effects/mapobj.wfx` / `mapobju.wfx`, and a scan of every WMO
in the wotlk data. The pure maths is in `include/rendering/stock_wmo_light.hpp`.

### Effects and programs
- `0x7afee0` loads the effects: `MapObjDiffuse/Specular/Metal/Env/Opaque/EnvMetal`
  (table `0xd1c3f0`, indexed by MOMT shader) and the `MapObjU*` set (`0xd1c3d4`,
  plus `MapObjUComposite`). The U set is used for every group of a root with
  **MOHD flag 0x2** (the "unified" path `0x7a9380`).
- Every effect is `ColorOp0(Mod2x)`: the pixel program outputs `2 * primary * tex`.
- Vertex programs (arbvp1, variant 1 = lit, variant 0 = lighting off):
  - `MapObjDiffuse_T1`: `primary = clamp(MOCV.rgb * clamp(c10 + c11*clamp(N.-c12)) + c29)`, `primary.a = MOCV.a`.
  - `MapObjUDiffuse_T1`: `primary = clamp(clamp(c10 + c11*clamp(N.-c12)) * c28 + MOCV.rgb + c29)`.
  - variant 0: `primary = MOCV` (no light, no emissive).
  - c10 ambient, c11 diffuse, c12 light dir (set by `0x7a8b10`), c28 material
    diffuse = `0xff7f7f7f` (0.498), c29 emissive (`0x7a8940`), c13 specular.
  - Lighting is per vertex and clamped to 1 per channel before the doubling.
- Pixel programs: `MapObjDiffuse` alpha = `primary.a * tex.a`; `Opaque/Specular/Metal/Env/EnvMetal`
  alpha = `primary.a` only. **No pixel program reads the secondary colour: WMOs have no
  specular in 3.3.5a**, even with MapObjSpecular. `Env`: `+ tex.a * envTex`;
  `EnvMetal`: `+ tex.rgb * tex.a * envTex` (sphere map from `_Refl` vertex programs).
- Shadow-map variants of the pixel programs multiply the whole colour by
  `0.7 + 0.3 * lit` (5-tap PCF, faded at the map edge; see "Shadow map filter
  (PCF taps)" - an earlier reading here said 4).

### Vertex colours
- Load `0x7d7c30`: MOCV is read when MOGP flag 0x4; unless **MOHD flag 0x8**, `0x7d7380`
  (FixColorVertexAlpha) runs: trans-batch vertices (0 .. last vertex of the last A batch)
  `rgb >>= 1`, alpha kept; all others `rgb = min(255, ((c*a)>>6 + c) >> 1)`, `a = 255`.
  **No MOHD ambient term** (Wowee had the later clients' rebasing on the ambient).
- First draw `0x7abf50` -> `0x7d78c0` (AttenTransVerts, every root, MOHD 0x1 not checked):
  for each trans vertex, over the group's MOPR refs: distance to the portal (signed plane
  distance x side when the projection is inside the polygon `0x9830d0`, else distance to the
  polygon `0x984db0`); a portal into an exterior group (MOGI flags & 0x48) adds
  `1 - 0.15*max(d,0)` if > 0.001 (`0x9f23d4`, `0x9e1134`); a portal into an interior group
  with |d| < 1 zeroes the vertex. `w = sum > 0.001 ? min(sum,1) : 0`;
  `rgb = round(c + (127 - c) w)` (`0xa40594`), `a = round(255 w)`.
- No MOCV (`0x7c8560`): the vertex buffer gets `0x7f7f7f` (classic) or `0x000000`
  (MOHD 0x2), alpha 255. MOCV is BGRA, swizzled for GL.

### Which light each batch gets
Render dispatch `0x7ad050`: `0xcfbec4 = 0x7ac6a0` (group without MOCV, `group+0x108 == 0`),
`0xcfbecc = 0x7ac9f0` (with MOCV); both jump to `0x7a9380` for a MOHD 0x2 root.
Light modes of `0x7a8b10`: 0 off; 1 outdoor (diffuse DayNight+0x1a8, ambient +0x1ac);
2 window (diffuse +0x1b0 = mean(diff, amb), ambient +0x1b4 = mean + 16/255, from `0x7ee750`);
3 MOHD ambient alone (root+0x1a0), no diffuse.

| path | batch | light |
|---|---|---|
| classic, no MOCV (`0x7ac6a0`) | all | F_UNLIT ? 0 : 1 |
| classic, MOCV (`0x7ac9f0`) | A (trans) | pass 1: F_WINDOW(0x20) ? 2 : 1, blend SrcAlphaOpaque(9); pass 2: mode 0, blend InvSrcAlphaAdd(7) |
| | B (interior) | 0 (MOCV only, no SIDN) |
| | C (exterior) | F_WINDOW ? 2 : 1 |
| unified (`0x7a9380`) | A | pass 1: F_UNLIT ? 0 : (F_WINDOW ? 2 : 1); pass 2: mode 3 |
| | other, group flags & 0x48 == 0 | F_WINDOW ? 2 : 3 |
| | other, exterior group | F_UNLIT ? 0 : 1 |

Data (scan of all wotlk WMOs): exterior groups with MOCV are almost all in MOHD 0xb roots
(unified, so their MOCV is *added*: Stormwind/Northrend "dark" MOCV adds ~nothing);
interior groups nearly all have MOCV with A/B batches; groups without MOCV are C-only.

### Emissive / window glow
- c29 = `(0xd1befc + frameSidn) >> 1` per byte (`0x7ac6a0`, `0x7a8940`); `0xd1befc` is the
  DayNight ambient only for the WMO under the cursor (`0x79678a`, GUID == `0xcd7770`), else 0.
- `frameSidn` (MOMT+0x14) = sidnColor (MOMT+0x10) x round(f*255 - 0.5)/256 per frame
  (`0x7a8520`), f = DayNight+0x1dc = table `0xaf4c80` by time of day: 1 until 06:00, 0 from
  07:00 to 20:30, 1 from 21:30 (linear between).
- No other glow: lamp glass is just F_SIDN; no texture animation on WMO geometry.

### Fog
- `0x7a8440`: F_UNFOGGED (MOMT 0x2) draws with fog off; otherwise the DayNight fog
  (+0x8c set, or the blended +0xa0 set).

### Doorway light for units/doodads (not WMO geometry)
`0x7a1bc0` samples the light under an object into `+0xc0` (from the Gx light at
`0xce04a8+0x88`), `0x7a1e90` steps `+0x84` toward it by a few bytes per frame;
`0x7c1730` hands it to the M2 as ambient (x AreaTable multiplier when flag 0x8000).
Character/M2 territory - not changed here.

### Gap table
| feature | stock | Wowee before | Wowee after |
|---|---|---|---|
| MOCV fix on load | 0x7d7380, all MOCV groups, no ambient | interior groups only, minus MOHD ambient | stock (`stockVertexColors`) |
| trans-vertex alpha | portal distance (0x7d78c0) | authored MOCV alpha | stock, plane from MOPV polygon (loader drops MOPT plane) |
| no-MOCV colour | 0x7f7f7f / 0 (unified) | white (1.0) | stock |
| interior B batches | 2*MOCV, unlit | MOHD amb + 2*rebased MOCV, floor 0.12 | stock |
| exterior / C batches | 2*clamp(MOCV*L + e/2) | amb+diff*NdotL, MOCV ignored | stock |
| unified (MOHD 0x2) | MOCV added to 0.498*L, MOHD ambient indoors | not distinguished | stock |
| F_WINDOW light | mode 2 | none | stock |
| doorway blend | two passes by vertex alpha | mix(interior, exterior) by MOCV alpha | stock single-pass mix |
| light clamp | per channel to 1, then x2 | unclamped sum | stock |
| specular | none | 0 (already) | none |
| F_SIDN | sidn x night table, all lit modes | sidn x luminance heuristic | stock table (needs renderer to pass the hour; heuristic fallback) |
| F_UNFOGGED | no fog | fogged | stock |
| alpha | Diffuse: vA*tA, others vA | tA | stock |
| lamp glass / clock fire / lava scroll | none | always | off unless WOWEE_WMO_EXTRAS=1 |
| Env/EnvMetal env map | yes | no | stock (wmo-rendering.md §2) |
| shadow-map darkening | whole colour x(0.7+0.3 lit) | diffuse term only | outdoor pass x shadow (shadowParams.y = 0.3 -> 0.7..1) |
| N.L | per vertex | per pixel (normal maps optional) | unchanged (setting) |
| fog | view-depth, exponent | radial linear | stock_fog.glsli |

Extras kept (setting/env gated, document): generated normal maps + POM (settings,
default on - they do change flat walls), wet surfaces (rain setting), Wowee point lights,
RT ambient/shadow, volumetric fog, cloth sway of banner geometry (motion, not light).


## 7. Water and other liquids

The vertex data, texture matrices, draw state, underwater rules and
particulates are in [water.md](water.md), which also holds the current gap
table; this section keeps the shader maths it builds on.

Stock 3.3.5a (12340). Addresses are in Wow.exe. The stock shaders are the ARB text in
`Data/expansions/wotlk/shaders/{vertex/arbvp1,pixel/arbfp1}/*liquid*.bls`.

### LiquidType.dbc and LiquidMaterial.dbc

- Loaded at 0x8b2710 (LiquidType) and 0x8b2a90 (LiquidMaterial).
- LiquidType fields the client reads (byte offsets in the record):
  - +0x08 Flags.
  - +0x0c Type: 0 water, 1 ocean, 2 magma, 3 slime.
  - +0x18..+0x24 MaxDarkenDepth and the Fog, Ambient and Dir darken intensities. These drive the underwater darkening in DayNight (see Light).
  - +0x28 LightID: an override light while the camera is in this liquid (magma uses 7, slime uses 6).
  - +0x38 MaterialID (field 14).
  - +0x3c..+0x50 Texture[6].
  - +0x54/+0x58 Color[2].
  - +0x5c.. Float[18].
  - +0xa4.. Int[4].
  - 0x8a27c0 copies the six texture names, Color, Int[0..3] and Float[0..17] into the per-type settings (`CMaterialSettings`, bank 0x8a28f0).
- LiquidMaterial has 3 rows (ID, LVF, Flags): 1 = water (LVF 0), 2 = magma (LVF 1, i.e. uv), 3 = procedural (LVF 0). Flags bit 1 goes to settings +0x360.
- In the shipped table:
  - All water and ocean rows (1, 2, 5, 6, 9, 10, 13, 14, 17, 41, 61, 81, 181) are material 1.
  - All magma and slime rows are material 2.
  - Only row 100, "Basic Procedural Water", is material 3, and no map uses it.
- Texture[0] is the animated sequence, for example `XTextures\river\lake_a.%d.blp`, `ocean_h.%d`, `fast_a.%d`, `lava.%d`, `slime.%d`. Texture[1] is the depth ramp: `proceduralRiverDepthTex`, `proceduralOceanDepthTex` or `proceduralWmoWaterTex`.

### Which material and shader is drawn: 0x8a1fa0

```
shaderPath = 0xd43af0 && GxCaps.vsLevel(+0xb4) >= 1 && GxCaps.psLevel(+0xc4) >= 3
material 1: shaderPath ? (0xb23f68 ? CMaterialWater  (vsLiquidWater/psLiquidWater, 0x8a3f70)
                                   : CMaterialWaterNoSpec (0x8a4070))
                       : CMaterialWaterFFP
material 2: shaderPath ? CMaterialMagma (vsLiquidMagma/psLiquidMagma, 0x8a4190) : CMaterialMagmaFFP
material 3: shaderPath ? CMaterialProcWater (vs/psLiquidProcWater%s, 0x8a3e00) : CMaterialProcWaterFFP
```

- 0xd43af0 and 0xb23f68 are set at 0x7814d7 and 0x7814e4 from the world render flags (0xcd7750 / 0xcd774c & 0x10000000) and from 0xce04a0, which is the specular option.
- With the default settings on shader hardware, the result is **psLiquidWater** for every water and ocean, and **psLiquidMagma** for lava and slime.
- The procedural shader, with its two cube maps and wave vertex shader, is not used by any shipped liquid. Those waves are the Taylor-series constants in vsliquidprocwater.

### Water: 0x8a5590 draws CMaterialWater

- **Stage 0 (Gx 0x15)** is the depth ramp (Texture[1]). Its texture matrix is identity, with v scaled by Float[2] (1.0).
- **Stage 1 (Gx 0x16)** is the frame of Texture[0], picked by 0x8a1d60.
  - Its texture matrix is rotated by Float[1] (×57.2958) and scaled by Float[0]. Float[0] is 1.0, or 0.25 for "WMO Ocean". Float[1] is 0, or 180 for WMO Ocean.
- **Vertex constants** come from 0x8a38b0:
  - c33 is the light direction in view space; c34 ambient; c35 diffuse.
  - c36 is the specular colour, with w = 6.0 (0x9e8cf8, the specular power).
  - c4 is fog, written as (−1/(end−start)·k, end/(end−start), exponent).
  - Those colours come from the frame's lighting object: +0x54 ambient, +0x60 diffuse, +0x6c specular, +0x78 direction.
  - Outdoors that object is filled by 0x780cd0 from DayNight. The specular (+0x6c) is only ever added to by local point lights (0x834f60), so **under the sun the specular colour is 0**.
- **vsLiquidWater:**
  ```
  primary   = (amb + diff * sat(N·−L)) * attrib4      // colour register clamps to 0..1
  secondary = pow(max(N·−H, 0), 6) * specColour        // H = normalize(normalize(P) + L)
  ```
- **psLiquidWater:**
  ```
  rgb = primary * ramp.rgb + tex.rgb + (secondary + 0.25) * tex.a
  a   = primary.a * ramp.a
  ```
  So the colour is the lit ramp plus the animated texture, plus a constant shimmer of 0.25 × tex.a. **There is no reflection term** in the default path: the "second texture" is the depth ramp. psLiquidWaterNoSpec drops the shimmer.

### The depth ramps (built on the GPU on demand, 8 × 64, 0x8a2e20)

- **proceduralRiverDepthTex / proceduralOceanDepthTex** (0x8a2bf0):
  - Row r = close + (far − close)·r/64. Alpha = shallow + (deep − shallow)·r/64, in 8.8 fixed point.
  - Colours come from DayNight base+0x10c/+0x110 + type·8. That is IntBand 14/15 (ocean close/far) and 16/17 (river close/far).
  - Alphas come from base+0x140..+0x14c = LightParams WaterShallow, WaterDeep, OceanShallow and OceanDeep alpha (DNLight [0x1b..0x1e]). The river uses Water*, the ocean uses Ocean*.
  - The ocean's last row has its HSV value × 0.9 (0xa02a34).
- **proceduralWmoWaterTex** (0x8a2ac0):
  - RGB is the river far colour (base+0x118 = IntBand 17) in 4 of the 8 columns, and white in the other 4.
  - Alpha runs WaterShallow → WaterDeep down the 64 rows.
- These textures are rebuilt from the light (0x8a2a10 re-uploads them), so the water colour follows Light.dbc by the hour and by the light volume.

### Animation (0x8a1d60)

- `frame = floor(N * (timeMs % period) / period)`. The rounding is `round(x − 0.5)` with 0xb23f74 = 0.5.
- N is the number of frames found: 0x8a2450 tries `%d` = 1..30.
- For water the period is LiquidType Int[1] (field 42), which is **1250 ms** for every water and ocean row. So lake_a and ocean_h (30 frames) run at **24 fps**, and fast_a (16 frames) at 12.8 fps.
- The magma material passes a hard 1250 ms (0x4e2) and also scrolls its texture (0x8a34b0). The u and v offsets are `(t % round(1000/Float[0..1])) / that`, i.e. Float[0] and Float[1] are repeats per second. Lava and slime have Float[0] = 0.025.

### Magma and slime

- vsLiquidMagma passes through the vertex colour. psLiquidMagma = `vertexColour * tex`, alpha 1. It is **unlit and opaque**, with fog.

### WMO liquids (0x793d20)

- The group's liquid type is resolved, with "not found → water". Water types (`(t−1)&3 == 0`, t < 21) in a group that is not exterior (group flags & 0x48 == 0), on a type without flag 0x200, become **type 17, "WMO Water − Interior"**, lit as interior. Both 13 and 17 draw proceduralWmoWaterTex.

### Underwater

- This is in DayNight 0x7f3230/0x7f16f0 (see Light):
  - The underwater LightParams slot is used.
  - LiquidType.LightID overrides the light (magma and slime).
  - The darkening by depth comes from MaxDarkenDepth and the three intensities. Only Ocean has them: 30, 0.5, 0.5, 0.25.
  - In fog mode 1, the fog exponent ×2.
- There is no screen overlay in the liquid code, and the sky is not drawn
  under liquid; the frame clears to the fog colour (water.md §6).

### Ripples and waves

- The surface is flat: no stock vertex shader moves a vertex (vsLiquidWater, vsLiquidMagma).
- waterRipples (CVar, registered at 0x7813a9) and waterParticulates are the splash-ripple sprites (WaterRipples.bls: `tex * vertex alpha`) and the underwater motes. They are not surface waves.

### Gap table

| Feature | Stock | Wowee before | Wowee after |
|---|---|---|---|
| Surface geometry | Flat sheet | Gerstner code with amplitude 0 (flat), normal from the waves | Flat; the Gerstner code is removed; normal is up |
| Frame rate | Sequence once per LiquidType Int[1] = 1250 ms (24 fps for 30 frames) | Fixed 30 fps (1.0 s cycle) | Per-row period from the DBC, 0x8a1d60 formula |
| Water colour | clamp(amb + diff·N·L) × ramp + tex.rgb | Ramp + tex.rgb, **unlit** | Lit as stock |
| Ramp shape | 64 rows, close → far, 1/64 per row, ocean last row ×0.9 | Linear close → far | 64-row sampling shape and ocean ×0.9 |
| WMO water colour | River far colour; alpha shallow → deep | River close → far | River far + alpha ramp (depthRamp = 2) |
| Ramp choice | By the LiquidType texture name | By basic type | By field 16 (river/ocean/WMO) |
| Sun glint | None: specular is local lights only; constant 0.25 × tex.a shimmer | GGX glint × shimmerStrength, also raised alpha | Stock 0.25 × tex.a; GGX removed; alpha is the ramp's |
| Texture scale and rotation | Scale Float[0] (0.25 WMO Ocean), rotation Float[1] | 1 repeat / 16.7 yd | Scale and rotation (with the stock slip) applied; see water.md |
| Magma/slime | Unlit tex, period 1250 ms, scroll Float[0..1]/s | Unlit tex, 30 fps, no scroll | 1250 ms, scroll |
| Reflection | **None** in the default path (cube maps only in unused material 3) | Sky-only reflection, fresnel ≤ 0.35 | The "Water reflections" setting, off by default (water.md) |
| Shoreline foam, wake froth | None | Option / always | Foam a setting; wake froth under "Splashes, wakes and bubbles", off by default (water.md) |
| Horizon alpha fade (600–2400 yd) | None (fog only) | Present | Removed (water.md) |
| Depth source | Per-vertex depth (attrib 7) from the liquid data into the 64-row ramp | Scene depth buffer, ramp over 6 yd (river) / 15 yd (ocean) | Stock: depth byte / 42 or / 255 per vertex (water.md §2) |


## 8. Weather, full-screen glow and the ghost world

### Server grade to weather
- SMSG_WEATHER handler: 0x526530 case 0x2f4. Reads state (uint32), grade (float), abrupt (uint8). The state is a Weather.dbc ID, looked up by 0x65c290. The handler passes EffectType (+8), the grade, the record, `!abrupt` and field +0xc to the weather object, 0x7846a0. It also logs "Weather changed to %d, intensity %f".
- Weather.dbc (3.3.5) fields: ID, AmbienceID, EffectType (0 none, 1 rain, 2 snow, 3 sand), a float scale for the storm light (1.0 on every rain and snow row, 0 on the coloured sandstorms 103 and 105-107), Color[3] (+0x10..0x18, all 1.0), EffectTexture (+0x1c).
- 0x7846a0 stores the target grade clamped to 0..1 ([0]) and the previous grade ([1]). It also stores the storm-light target clamp(grade, 0, 0.25) ([3]) and its previous value ([4]). An abrupt change (abrupt byte set) applies at once.
- Per frame, 0x784850 does the following:
  - **Particle grade.** Eased over `(|Δ| + 0.001) * 10 s`.
  - **Particle level.** `max(0, (grade - 0.25) * 4/3)`, using 0x9e8ce4 and 0x9f987c. It goes to the rain (0x7840b0), snow (0x7844f0) and sand (0x784580) emitters.
  - **Storm-light value [0x14].** Eased from [4] to [3] over `(|Δ| * 4 + 0.001) * 10 s`. So clear to a full storm takes 10 s.
  - **Weather.dbc scale [0x28].** Eased over `(|Δ| * 4 + 0.001) * 5 s`.
  - **Storm weight.** `DayNight+0x4c = [0x14] * [0x28]`. DayNight multiplies it by 4 and caps it at 1 into 0xd38b88 (0x7f3920). The light reaches the full storm set at a grade of 0.25.
- When the weather type changes (0x78d170), the old emitter fades out before the new one is created. The new one's first target is capped at 0.25.

### Emitters and density
- **CVar `weatherDensity`.** Created in 0x787780, default "2". 0x784040 maps it to a factor in 0xadf1f8: 0 → 0.10, 1 → 0.33, 2 → 0.66, 3 → 1.0.
- **CVar `useWeatherShaders`.** Default 1. Shaders are used when the CVar is on and the device can run them (0x78ca90 checks this).
- **Spawn rates** are `base * density * level` particles per second. The base is the shader path's value, with the fixed-function value in brackets:
  - Rain: 35000 (6500).
  - Snow: 14000 (1300).
  - Sand: 32000 (6000).
- **Volumes** are box half-extents round the camera:
  - Rain: 65 x 37.5 yd (0xa3ec90 130, 0xa0ff58 75).
  - Snow: 45 x 30 yd.
  - Sand: 20 x 12.5 yd.
- **Mist.** Rain mist is `textures\Weather\SnowMist01.blp`, in sheets 12 yd wide, created by 0x78c420. At most 128 are alive. Spawn rates:
  - Rain: `max(0, (level - 0.5) * 2) * density * 38` per second.
  - Snow: the same with 48.
  - Sand: `64 * density * level`.
- **Mist look** (0x786e10). The colour is the fog colour (DayNight+0x8c) and the blend is Alpha. The fade is `alpha *= 1 - clamp(1.5 - d/12, 0, 1)`: nothing inside 6 yd, full beyond 18 yd. On top of that sits a fade in and out over the sheet's life.
- **Splashes** ("patter", 0x785640, 0x78a030). These were already matched in Wowee: a triangle 1/12 x 1/6 yd, a 0.25 s life, a 4x4 sheet, and none while the camera's speed² is over 2.

### How they are drawn

| Particle | Blend | Colour | Fog | Lit |
|---|---|---|---|---|
| Rain streak (rain.bls, 0x78a640/0x7891b0) | **Mod2x** (GxBlend 5; alpha-test reference table 0xad8b7c confirms the enum order) | Weather.dbc colour (white) × texture | fog colour forced to 0x80808080 (neutral for Mod2x); fogcoord by height above the camera, `clamp(15 − 0.2Δz)` (section 2) | no |
| Splash (patter.bls, 0x78a030) | Mod2x | as above | 0x80808080, same height fade | no |
| Snow (snowpoint.bls, 0x78aee0) | Alpha | Weather.dbc colour (white) | none (the shader writes no fogcoord) | no |
| Sand (sand.bls, 0x78bee0) | Alpha | the fog colour (DayNight+0x8c) | n/a | no |
| Mist (0x786e10) | Alpha | the fog colour | n/a | no |

RainDrop01.blp and RainDropSplash01.blp have RGB 123..164, which averages 128. Under Mod2x a drop multiplies the scene by about 0.97 to 1.29. Stock rain is therefore a faint brightening streak, not a grey alpha line.

### Full-screen glow (EffectGlow.cpp / PassGlow.cpp) and the ghost world (FFXEffects.cpp)

- **Setup.** WorldFrame's constructor (0x4fabd0) creates:
  - glow 0x8bfe80, stored in 0xb74364;
  - death 0x7ea260, stored in 0xb74368;
  - nether 0x7ea470;
  - special 0x7ea5f0.
- **CVars.** `ffxGlow` ("full screen glow effect") and `ffxDeath` both default to 1. `ffx` (0x8c12f0) is the master switch.
- **Choosing the effect** (0x4f7020). The ScreenEffect.dbc row's Effect field picks it: 0 glow, 1 death, 2 nether, 3 special. The row's LightParams slot (+0x1c) goes to 0x7ecec0, which sets DayNight 0xd38b58, the Light.dbc slot override. Row 1, "Ghost Screen Effect", is Effect 1 with slot 4 (death LightParams), ambience 462 and music 36. Without a row, glow is used.
- **Glow chain.** The scene is copied at full size into 0xd457a0. Then:
  1. **FFXBox4** (0x8c1f70/0x8c2040): taps at (-1.5, +0.5)² source texels → a 4x4 box to quarter size (0xd45810; sizes from 0x8c15f0).
  2. **FFXGauss4** (0x8c1b40/0x8c1c20): across, then down, with taps at -2.5, -0.5, +0.5, +2.5 quarter texels (0xa94268, 0x9e8d30) weighted 1/8, 3/8, 3/8, 1/8.
  3. **FFXGlow** (0x8c21e0): `out = lerp(scene, blur, color.z) + blur² * color.w`.
  4. Under liquid the chain uses list 1 instead, which is FFXGlowWave: the same combine through a wave-distorted UV (0xd45c70).
- **Parameters** (0x4f8770, every frame, set through vtable+0x10 = 0x8bfde0, which packs ARGB = (w, z, z, z)):
  - **w** is `floor(LightParams.Glow * 255)`. Glow is DNLight [0x16], at DayNight base+0x12c. It is blended between lights and toward the storm set like the other floats. Values in LightParams.dbc: mostly 0.5. Elwynn (LP 12) is 0.65 and underwater (LP 13) is 1.0.
  - **z** is `max(underwater ? 0x54 : 0, drunk byte)`. The drunk level is the player's +0x1d or +0x2b8, capped at 100, divided by 100 (0x4f7290). The mode is 1 when the camera is in liquid.
- **Death chain** (0x7ea260): Box4 → Gauss4 → FFXDeath (0x7e86a0). The FFXDeath pass (0x7e87b0) uses `color = (R 0x53, G 0x93, B 0xa8, A = floor(Glow * 255))`. The shader computes the following, where 0.144 is the shader's own constant, not Rec.601's 0.114:

  ```
  lit  = scene + w * blur²
  grey = sat(dot(lit, (0.299, 0.587, 0.144)))
  out  = grey + tint * sat(4 * grey * (1 - grey))
  ```
  The death LightParams (4) has Glow 0.8.

### Drunk
**The value** (0x4F7290, `ecx = player+0x1008`, the player descriptor block):
`drunk = min(max(PLAYER_BYTES_3 byte 1 (+0x1D), PLAYER_FAKE_INEBRIATION (+0x2B8)), 100) · 0.01`
(0x9F1968). The byte is the server's 0..100 inebriation; the int is the fake
one (UNIT_END + 0xAE). The client has **no tipsy/drunk/smashed thresholds and
no easing of the value**; the one threshold is 0.5 for the bubbles below.
Every caller of 0x4F7290 (all of them traced) does one of these:

| Effect | Where | Rule |
|---|---|---|
| Full-screen glow | 0x4F8770 (every frame, active player) | `z = max(underwater ? 0x54 : 0, floor(drunk · 255))` (·255 0x9E30C0, +512 bit trick), as above. No easing. |
| Field of view | 0x6DA6B0 (on a change of PLAYER_BYTES_3 byte 1, registered at 0x6E46F6), 0x6066E0 (camera target set) → 0x604940 | target FOV offset `camera+0x138 = drunk · 1.5533 rad` (89°, 0xA1EA20), eased by 0x6030E0/0x603D30 at **cameraFoVSmoothSpeed 0.5 °/s** (linear in time: duration = Δ / (0.5°)); set at once when the camera gets a new target. FOV = `clamp(base (+0x40) + offset, 0, 3.1241)` (getter 0x5FF500, CGCamera vtable 0xA1EA54 slot 0; inferred to be the projection FOV). Skipped when camera flags & 0x80008000. |
| Steering drift | 0x6DEB30 (event-6 handler 0x4F8730, inferred per frame) | see movement-camera.md §5: facing += sway each frame while moving; pitch += 4·sway while swimming/flying |
| Click-to-move | 0x7317A0 (from 0x6DEB30 when CTM is active) | the CTM's wanted facing gets the same sway |
| Camera dive | 0x6023D0, 0x606F90 | when the camera's target is a drunk player, the cameraDive surface/submerge pitch limits (cameraSurfacePitch / cameraSubmergePitch) are not applied |
| Breath effect | 0x732650, M2 event `$BTH` | a player with drunk ≥ 0.5 (0x9E2EC4) gets effect 7 "HARDCODED Inebriated Bubbles" (SpellVisualEffectName 1223, `Spells\Bubble_Drunk.mdx`) at attachment 17 (Breath) instead of the cold (3) / underwater (2) breath (0x6F9260, names at 0xADA764, attachments at 0xA32AF4) |
| Slurred chat | 0x50D170 → 0x577F60 (own outgoing chat) | each word-initial `s` (not followed by space/s) becomes `sh` with probability drunk; an `s` at a word end gets an `h` with probability drunk; with probability `drunk · 0.25` the message gets the SLURRED_SPEECH suffix |
| Hostile levels | 0x60F9E0 (UnitLevel), 0x621070 (tooltip), 0x98E8A0 | while the *viewing* player is drunk, a unit for which 0x729740 holds (inferred: hostile/attackable) shows `level − round(drunk · 5)` (0x9EBF34), at least 1 |

**Not in stock:** no camera wobble, roll, shake or bob; no blur other than
the glow's own; no Light.dbc "drunk" slot (the slot override 0xD38B58 is only
set from ScreenEffect.dbc, and no ScreenEffect row is drunk); no sound of
its own (the bubbles model is the only asset); no random strafing - the only
movement effect is the deterministic facing/pitch drift.

### Gap table

| Feature | Stock | Wowee before | Wowee after |
|---|---|---|---|
| Particles vs grade | `(g - 0.25) * 4/3`, nothing below 0.25 | linear in grade | stock (`stock_screen_fx::particleLevel`) |
| Rain blend | Mod2x, unlit, fog → 0x80 grey by height above the eye (70–75 yd) | alpha 0.7, tinted by ambient + 0.6·diffuse, fog colour | stock Mod2x (second pipeline), stock height fog to 128/255 |
| Splash blend | Mod2x | alpha 0.75, tinted | stock Mod2x |
| Snow colour | white, unlit, unfogged, Alpha | lit-tinted, fogged, alpha 0.95 | stock |
| Storm light | 4·min(g, 0.25)·Weather.dbc scale, 10 s ease | grade linear | stock: 4·min(g, 0.25), a quarter per 10 s (LightingManager); Weather.dbc scale not read |
| Mist sheets (SnowMist01, fog colour) | yes, past level 0.5 | none | stock (weather-sky.md 2) |
| Volumes / counts | 65x37.5 yd rain, rates 35000·0.66·p /s | 32 yd box, 3000 particles max | unchanged (Wowee extra: count cap and box sized for its CPU path) |
| weatherDensity default | 2 (0.66) | Weather Detail 3 (1.0) | unchanged (setting is the user's) |
| ffxGlow | on: Box4 → Gauss4 → lerp + blur²·Glow | none (shoulder tone map only in postprocess.frag, not in the FXAA path) | stock: blit 2x2 twice to a quarter-size mip, 16-tap product Gauss4 in fxaa.frag, `setGlow(LightParams.Glow, underwater 0x54/255)` |
| Glow under water | z = 0.33 + wave UV | none | z = 0.33; **wave distortion not done** |
| Drunk | glow blend z = drunk; FOV widening, facing drift, bubbles, slurred chat (see "Drunk") | Wowee's own wobble + 9-tap blur | stock glow blend from PLAYER_BYTES_3 byte 1 (`Application::updateDrunkLevel`) and the stock facing drift (`CameraController`, 0x6DEB30); Wowee's wobble only under the "Drunken wobble" enhancement (off). FOV widening (inferred), bubbles and slurred chat not done |
| Ghost | ffxDeath: blur, grey, tint (0x53,0x93,0xa8)·4g(1-g) | per-pixel desaturate ×(0.88,0.95,1.10) | stock ffxDeath formula with the glow blur |
| Nether/special (ScreenEffect types 2/3) | colour wash per ScreenEffect.dbc | none | not done |


## 9. Wowee extras, open gaps and what to check

### Extras kept (not in the stock client)
Each of these is either off in the Classic preset or leaves the stock picture
alone when it is idle.

| Extra | Changes the classic look? | Where |
|---|---|---|
| Real-time shadow map on terrain, M2 and WMO | Stock has one too at its higher shadow levels. Its darkness is the stock 70% floor, scaled by the Shadow darkness setting. "Sun shadows" enhancement; off is the stock default (blob, characters.md L10). The "Shadow edges" row picks the stock 5 taps (0x8742e0 offsets) or Wowee's 9-tap disk; the stock edge fade (0.70..0.99) is not reproduced | `shadowParams.y` = 0.3 × setting |
| Shoreline foam | yes; off on Low and Classic | water.frag (`shorelineFoam`) |
| Wet ground in rain | yes; off on Low and Classic | `wet_surface.glsli` (`wetGround`) |
| Sky reflection on water | yes. The stock default water has **no** reflection (psLiquidWater adds no environment term); kept because a sky-only look was asked for | water.frag |
| Wake froth, horizon alpha fade (600–2400 yd) | slightly; the fade lies beyond the fog in practice | water.frag |
| Volumetric fog / light shafts, sun shafts, lens flare, sharp stars, foliage and cloth wind | yes; all off on Classic, on from Low or Medium up | presets |
| Normal maps and parallax (generated) | yes; off on Classic | presets |
| Point lights from M2 and WMO (`localLight*`) | adds light near torches and the like; stock has the 4 (terrain 3) nearest M2 lights only | terrain, M2, WMO shaders; "Torch and lamp light" enhancement, off = the stock rule (characters.md L3) |
| RT ambient and shadow | off unless enabled | `rt_lighting.glsli` |
| WMO lamp glass, clock fire and lava scroll | off; `WOWEE_WMO_EXTRAS=1` brings them back | wmo_renderer.cpp |
| Fog strength and fog sky blend sliders | no at the defaults (1 and 0) | settings |

The sky part recommends defaulting sun shafts and sharp stars off on High and
Ultra as well. The presets were left as they are; that is a taste call.

### Gaps that remain
- **Terrain specular.** Stock has specular with the `specular` CVar, masked by
  `_s` tileset alpha. Wowee has none.
- **Per-vertex vs per-pixel lighting.** Wowee lights per pixel, as it did
  before, with the same clamp.
- **The fog constant mapping** (c12/c30/c4 from Gx +0xa8..+0xb4) is inferred,
  not traced through the constant upload.
- **The light-pick position.** DayNight +0x24 is the player, according to the
  sky trace at 0x4f8770. Wowee uses the character position, which matches.
- **Missing pieces of the stock light pick:**
  - the hard-coded light rectangles (0x77eed0);
  - the server light override list (`0xd3901c`, SMSG_OVERRIDE_LIGHT);
  - the drunk or ScreenEffect light slots other than death.
- **Water:**
  - the per-vertex depth used to index the ramp (vertex attribute 7) is not
    recovered;
  - the per-vertex colour (attribute 4) is not resolved and is assumed white;
  - texture rotation (Float[1]) is not applied;
  - (done, see wmo-rendering.md §8) WMO liquids now take the stock
    LiquidType (0x7d82e0, 0x793d20) and their per-vertex heights.
- **WMO:**
  - (done, see wmo-rendering.md §2) the Env/EnvMetal env map;
  - the portal planes are rebuilt from MOPV, because the loader drops MOPT's
    plane;
  - (done, see wmo-rendering.md §4) which draws the shadow map darkens (0x8745d0);
  - (done, see wmo-rendering.md §3) the alpha test is the blend's reference against the program's alpha.
- **Sky:**
  - the Blue Child is not drawn;
  - the sky models are drawn before the sun, moons and clouds instead of
    after;
  - the cloud noise uses a hash instead of the client's random table, and its
    churn speed is a stand-in;
  - the timed screen tint (`0xd38b50`) is missing. The lightning-flash
    dome tint (0xd38184 bit 0) is unreachable in stock: nothing sets the
    bit (weather-sky.md 4).
- **Weather and glow:**
  - the underwater glow wave and the nether/special ScreenEffect washes are
    missing (the mist sheets and sand: weather-sky.md 2);
  - Weather.dbc's colour and storm scale are not read;
  - the drunk effect still uses Wowee's own blur.
- **Old radial fog:** none left. Character, grass, particles, ribbons and
  the glow sprites use `stock_fog.glsli`; rain uses its own stock height
  fade. Clouds and sky have no distance fog (only Wowee's volumetric air).
- **M2 batches with Mod/Mod2x** (m2.frag) still fog to black: Wowee draws
  them additive (m2-runtime gap 22); with real Mod/Mod2x pipelines they
  would fog to white / 0x80 like the particles.
- **Ribbon blends.** Wowee has only alpha and additive ribbon pipelines, so
  blends 0, 1, 5 and 6 draw as alpha; their fog colour is the stock one for
  their blend regardless.

### What to look at in game
- **Fog.**
  - Azeroth: haze starts where Light.dbc says, about 2.5× nearer than before,
    and ends at the band's distance in the zone's own fog colour.
  - Northrend and Outland: the fog runs to the view distance, but zones
    with a short band fog (high exponent) close in well before it.
  - Saved settings keep their old fog strength (0.4) and sky blend (0.7). To
    see the stock fog, pick a preset again, or set Fog thickness 1 and the sky
    colour blend 0.
- **Light borders** (Elwynn–Westfall, Stormwind gate, Hellfire–Zangarmarsh):
  a linear cross-fade, and the nearest light wins inside its inner radius.
  Nothing should pop or flicker.
- **Bright zones** (Mulgore, Durotar at noon): ground lit near the texture
  colour with clamped highlights. It may lean warmer than before, where the
  headroom scaling darkened everything.
- **Shadows** (trees, buildings, MCSH under forests): lighter, about 70%,
  over the whole colour.
- **Storm:** a light rain already turns the sky and light to the storm set,
  over 10 seconds.
- **Lava and slime:** with the camera inside, the world takes Light 7 or 6.
  Diving in the ocean darkens the ambient toward 30 yards.
- **Ghost:** the map default death light only; the local lights go away.

### Performance
- **CPU.** Light selection still scans the map's lights once a frame (as
  before) and sorts the few in range. The per-frame easing of about 30 fields
  is gone.
- **GPU, fog.** `stockFogVisibility` adds a mat4 × vec4 and a `pow` per
  fragment: negligible.
- **GPU, glow.** The glow pass is the largest cost. It adds two small blits
  and one full-screen pass whenever glow is on, an estimated 0.2–0.5 ms at
  1080p (not measured).
- **The rest.** Water lost its Gerstner loop and GGX glint, and the sky,
  clouds and WMO changes cost about the same as before.
