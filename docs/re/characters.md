# Units: lighting, dress and attachments (stock 3.3.5a build 12340)

How the stock client draws characters and creatures: the light on a unit,
the skin composite, geosets, item models and their glows, creature displays,
the unit shadow, the highlight, translucency and mounts. It is reverse
engineered from `Wow.exe` with Ghidra; addresses are virtual addresses in
that binary. The last two sections compare this with Wowee and list what to
look at in game. Shading equations come from the ARB text shaders the client
ships (`shaders\vertex\arbvp1\diffuse_t1.bls` and the `combiners_*` pixel
shaders).

Terms:
- **Entity**: `CMapEntity`, the world-side wrapper every `CGObject_C` model sits
  in (vtable 0xA3FD90, created by 0x781A10).
- **Component**: `CCharacterComponent`, the player-style body builder
  (`..\..\Common\ComponentCore\CharacterComponent.cpp`, about 0x4E0000-0x4F3000).
  It is shared by char select, the dress-up frame, players and
  CreatureDisplayInfoExtra NPCs. Offsets `cc+0x..` are into it.
- **CDI / CMD**: CreatureDisplayInfo / CreatureModelData.

## 1. Unit light

### Per model, per frame
- The lighting update **0x831AF0** runs for each model and each attached
  child. A child with a parent reuses the parent's lighting.
  1. `CM2Lighting::Init(scene, sphere)` **0x834900** clears the 0xD4-byte
     record. The sphere is the model's bounds in world space.
  2. `CM2Scene::AddLights` **0x81E400** adds the scene's directional lights
     and walks the scene's **64 x 64 point-light grid** (20-yard cells,
     `pos * 0.05 & 0x3F`) over the cells the sphere covers.
  3. The model's lighting callback `model+0x2AC` runs. For units and game
     objects it is **0x780CD0**: it sets the fog, then calls the entity's
     vtable +4 (**0x7C1730**, the colours) and +8 (0x7C10C0, the liquid clip
     plane).
  4. `CM2Lighting::Resolve` **0x835280**, then CameraSpace 0x8350A0. At draw
     time the batch calls **0x873CA0**: with vertex shaders it uploads c10-c12
     (sun) and up to **4 point lights** (0x872900, c17-c27); the fixed-function
     fallback **0x8353D0** has light 0 directional and only lights 1-3 for
     points (see "Point lights: full rule").

### Accumulation
- AddAmbient 0x834D90 adds the colour.
- AddDiffuse 0x834DC0 accumulates `c.r*dir`, `c.g*dir`, `c.b*dir`,
  `lum(c)*dir` (lum = 0.2127 R + 0.7152 G + 0.0722 B) and `sum += c`.
- AddLight 0x834F60:
  - a directional light folds its ambient, diffuse and specular in;
  - a **point light** goes into a list sorted by distance; the **four
    nearest** are kept, with **no range cut-off**.
- Resolve 0x835280: the luminance-weighted direction D (fallback (0,0,-1)),
  `proj = dot(D, dir_c)` per channel, `diffuse = 1.25 proj - 0.25 sum`,
  `ambient += 0.25 (sum - proj)`. With one directional light that is the
  light itself.
- **Point-light attenuation** is set by the CM2Light constructor 0x834A40 to
  (0, 0.7, 0.03) and never changed: falloff `1 / (0.7 d + 0.03 d²)`. The M2
  attenuation start/end tracks (M2Light +0x60/+0x74) are never read.
- Only CM2Light type 1 records are point lights: the M2 lights in torches,
  braziers and lanterns (linked into the grid by 0x834C70 / 0x835690). WMO
  MOLT lights are baked into MOCV and never reach units.

### Point lights: full rule
The only point lights in 3.3.5a are **M2Light records of type 1** in M2
models that are in the scene this frame. They light **M2 models, WMO group
geometry and terrain** (all three call AddLights). Nothing else (MOLT, Light.dbc)
is a point light.

**The light itself** (per model per frame, 0x828A00, after the animation
pass 0x82F0F0; the per-model record is 0xD4 bytes at `model+0x1D0`, the
CM2Light at record+0x68):
- Position: `M2Light.position (+0x04) × boneMatrix[M2Light.bone (+0x02)]`,
  taken back to world space through `scene+0xC4` (0x828A00 type-1 branch,
  0x835690 stores it at CM2Light+0x0C and relinks the grid cell). **The light
  moves with its bone.** (Type 0 lights get a direction, `-bone.zAxis`, and
  go on the scene-wide directional list `scene+0x20`, so they would light
  every model in the scene; 0x834C70 else-branch.)
- Colour (animation pass 0x82F0F0; stores at 0x830516 / 0x8305EA):
  `diffuse = M2Light.diffuseColor (+0x38) × diffuseIntensity (+0x4C) × model alpha (model+0x198)`
  goes to CM2Light+0x3C; ambient colour (+0x10) × ambient intensity (+0x24) ×
  alpha goes to CM2Light+0x30, **but no point-light path reads +0x30**:
  AddLight only sorts point lights, and both uploaders (0x872900, 0x8353D0)
  read +0x3C (colour) and +0x54..+0x5C (attenuation) only. So a point light's
  colour is **diffuse colour × diffuse intensity × model alpha**, nothing
  else. Intensity defaults to 1 (the model's light init in 0x832EA0 sets record+0x1C and +0x3C to 1.0).
  The diffuse product is recomputed only when the diffuse-colour track has
  more than one key, or one key before the model's first full-alpha frame
  (`model+0x90`); a constant colour with an animated intensity keeps the
  first value (stock quirk: the track-count test sits right before the store at 0x8305EA).
- Attenuation start/end tracks (+0x60/+0x74) are not evaluated; the
  attenuation is the constructor's (0, 0.7, 0.03) (0x834A40; 0x9E2EC0 = 0.7,
  0x9F23C8 = 0.03).
- On/off: enabled when `record+0x64` (default 1) and the **visibility track**
  (+0x88, evaluated into record+0x60 by 0x82B270) are non-zero (0x828A00 →
  SetEnabled 0x8356F0). The light is stamped with the scene frame
  (`CM2Light+4 = scene+0x14`).

**The grid** (`scene+0x24`, 64 × 64 list heads, allocated in 0x834C70):
- A light sits in **one cell, from its position only** (no radius):
  `cell = (floor(y·0.05) & 63)·64 + (floor(x·0.05) & 63)` (0x834C70,
  0xAF59D4 = 0.05). Z is ignored. The grid wraps every 1280 yd.
- AddLights 0x81E400 takes a sphere (centre C, radius r) and visits, per
  axis, the cells `floor((C−r)·0.05 − 0.5) .. floor((C+r)·0.05 + 0.5)`
  (wrapped & 63; 0xAF58A0 = 0.05, 0x9E2EC4 = 0.5). That covers the sphere's
  square plus at least 10 yd (up to 30 yd) beyond it in X and Y.
- A light found with a stale frame stamp (its model was not updated this
  frame) is unlinked there (0x81E54C → 0x8356F0(0)); it relinks on its
  model's next update. Only lights of models in this frame's scene list
  count (inferred: the models being drawn).

**Which sphere** (inputs to AddLights):
- M2 models (0x831AF0): centre = the model's origin (`model+0x124`, the
  translation row of its model-view matrix, back to world through
  `scene+0xC4`), **radius 0** (`fldz` at 0x831B25). So a model sees the
  **2 × 2 cells around the grid corner nearest its origin**: lights 10 to
  30 yd away per axis, depending on where it stands in its cell. Every
  CM2Model in the scene's frame list goes through this: units, players, game
  objects, map doodads and WMO doodads alike (0x821A20 calls 0x828A00 then
  0x831AF0 for every listed model; the lighting callbacks, 0x780CD0 for
  units and doodads, add sun/ambient/fog only and never touch the point
  list). Attached children reuse the parent's lighting.
- WMO groups (0x7964A0): the group's sphere (`group+0x3C`), then 0x7A9160
  → 0x872900 with up to 4 lights. **WMO geometry does get M2 point lights**:
  the mapobj vertex programs have the same c17-c27 point-light block, and
  the variant index includes the count (0x7A84D0: `lit + 2·(count +
  15·min(0xD43010, 2))`, count from 0xCFBEB4).
- Terrain chunks (0x7D0050): the chunk sphere (`chunk+0x24`), then the
  **3 nearest** (0x8349E0 for i = 0..2) into c28-c36 (terrain.md, vertex
  variant bit 6). The fixed-function terrain path (0x7D04A0) uses 0x8353D0.

**Selection** (AddLight 0x834F60): squared 3D distance from the sphere
**centre** to the light position; insertion sort ascending; the 4 nearest
are kept; a fifth only replaces the farthest when strictly nearer. No range
cut-off.

**Upload:**
- Shader path (the normal one; 0x873CA0 from the batch draw 0x81FB4B, lit
  batches only): 0x872900 writes all kept lights, **nearest first, up to
  4**: colour → c17-c20 (`w = 1`), view-space position → c21-c24 (from
  CameraSpace 0x8350A0: `CM2Light+0x18 = pos × view`), and the attenuation
  transposed: constant c25, linear c26, quadratic c27 (one light per
  component). Unused slots are zero. The vertex-program variant is
  `lit + 2·(count + 5·(… + 3·bones))` (0x873160), count 0-4; diffuse_t1 has
  all five counts.
- Fixed-function fallback 0x8353D0 (no vertex shaders): Gx lights 1-3 only,
  filled from the **end** of the sorted list (0x8354CB: `entry[n−1]`,
  `entry[n−2]`, …, stop at index 4). With 4 lights the **nearest is
  dropped**. Not the path a shader client takes.

**Shader** (diffuse_t1.bls arbvp1 variant 9 = 4 lights, view space, per
vertex, `P = modelView · v`, `N = normalize(modelView3 · n)`):

```
for i in 0..count-1:
    L = c[21+i].xyz - P;  d = |L|
    ndl = max(dot(N, L / d), 0)                     // no upper clamp
    att = 1 / (c25[i] + c26[i]·d + c27[i]·d²)       // 1 / (0.7 d + 0.03 d²)
    sum += c[17+i].rgb · ndl · att
lit = clamp(clamp(dot(N, -c12), 0, 1) · c10 + c11 + sum, 0, 1)
```
d is in yards (the view matrix is rigid). The WMO programs use the same
block with c10/c11 swapped (ambient/diffuse) and the result multiplied by
MOCV; terrain clamps `N·L` to at most 1 and keeps its own constants
(c28 + 3i: position, colour, attenuation).

### The shading equation
Per vertex, view space (`diffuse_t1.bls`):

```
lit  = clamp(max(N·-L0, 0) * diffuse0 + ambient
             + Σ_{i<4} colour_i * max(N·L_i, 0) / (0.7 d_i + 0.03 d_i²), 0, 1)
col  = clamp(lit * matDiffuse + matEmissive, 0, 1)      // rgba
out  = texture * col   (combiners_*.bls), then linear fog
fog  = clamp(pow(max(z * c.x + c.y, 0), c.z), 0, 1)     // the world's curve
```

- **No specular term and no normal maps** in any M2 shader. The only "shine"
  is sphere-environment texture units (`diffuse_t1_env`):
  `r = reflect(view, N)`, `uv = r.xy / |r + (0,0,1)| * 0.5 + 0.5`, added by
  `combiners_opaque_add` (`rgb = t0 * col + t1`).
- `combiners_opaque` takes alpha from the vertex colour, not the texture.
- **Material colour** (0x8201A9..0x8202B6): diffuse = model diffuse (+0x1A0)
  × M2Color.rgb; emissive = model emissive (+0x1AC), which is where the
  highlight goes. An unlit material (render flag 0x01) moves the diffuse into
  the emissive: **unlit = texture × M2Color**, not brighter.

### Where a unit's colours come from (0x7C1730)
- Only while the entity is nearer than the fog end (`entity+0x30 < fogEnd`).
  Past it the unit gets fog only.
- **Outdoors** (entity flag 4):
  - ambient = `entity+0x84`, eased every frame to the world unit ambient
    `0xD38CAC`;
  - diffuse = the sun light's diffuse `0xD38CA8` × `entity+0x8C`;
  - direction = the time-of-day sun direction `0xD38C9C` (0x7EEA90).
- **World unit colours** (0x7EE750, every frame from 0x7816F0): LightIntBand
  0 (global ambient, `0xD38BD4`) and band 1 (global diffuse, `0xD38BD8`).
  **They are the world's own values**; 3.3.5a has no separate character band.
  0x7816F0 scales both by `1 - 0.35 * weather` (0xD38F4C, 0.35 at 0xA3E860).
- **Unit-only ambient boost** (entity flag 0x8000, set for every unit and
  player: 0x7437BE passes 0x10 for TYPEMASK_UNIT, 0x781A10 maps it):
  - `ambient *= g_unitAmbientMul` (0xADEEBC), then each channel clamped to 1;
  - `g_unitAmbientMul = 1 + 2 * AreaTable.AmbientMultiplier` (record +0x48,
    DBC column 34) for the **player's** area; a subarea without flag 0x2000
    uses its parent (0x783429..0x7834DF);
  - eased `cur += (target - cur) * dt`, snapping within 0.01;
  - Dun Morogh 0.3 → ×1.6, Durotar 0.4 → ×1.8, 387 areas at 0.6 → ×2.2;
    1,832 areas are 0. **The world does not get it.**
- **Terrain-shadow darkening** (extShadowQuality < 2, the default 0): under a
  baked MCSH shadow bit (0x7A1BC0 → 0x7A06A0) the diffuse scale target is
  0.5, else 1; `entity+0x8C` steps toward it at 3.33/s.

### Indoors (per unit)
1. **Placement** (0x7A1BC0 → 0x7C2A70 / 0x7C2E70 → 0x7C15F0) finds the WMO
   group under the entity. Exterior (MOGP 0x8), exterior-lit (0x40) or no
   group means outdoors.
2. **Sample** 0x7A0D60: a ray in the group from `pos + 1` down to `pos - 12`
   (doodads +1.5 / -1760). The colour at the hit (0x7C7FE0) is the
   **barycentric MOCV × 2**, clamped to 255, plus the MOHD ambient when MOHD
   flags & 2. The alpha is the MOCV alpha; MOPY flag 1 marks a transition
   face.
3. **Split** 0x7C1AD0(colour, 168, 96): `m = max(r, g, b)`.
   - Diffuse = colour, its HSV value raised so the brightest channel is
     **168/255** when m < 168.
   - Ambient = colour scaled down so the brightest channel is at most
     **96/255**.
4. **Transition faces**: diffuse and ambient lerp toward the outdoor unit
   values by MOCV alpha / 256 (0x6ACC50), and the direction toward the sun.
5. **Light** (0x7C1730 interior branch): diffuse `entity+0x88 × +0x8C`,
   direction the **fixed (-0.30822, -0.30822, -0.9)** (0xAEEDF0). The sun is
   not switched off; it is replaced by a light from above.
6. **Over time** (0x7A1E90): ambient bytes step `max(1, int(dt * 510))` per
   frame (0.5 s for a full swing, linear); the diffuse colour snaps when
   resampled; the diffuse scale eases at 3.33/s. Placement re-runs as the
   entity moves.

### Fog
The same light state as the world: colour light+0x8C, start +0x90, end
+0x94, exponent +0x98; under water light+0xA0..0xAC (entity flag 0x8000 at
0x7C1730 / 0x780CD0, set through `CM2Lighting::SetFog` 0x834990).

## 2. The skin composite

### Size (0x401FF0 → 0x4F1A20)
- `componentTextureLevel` default "8" (0xB2F9EC), `componentThread` "1",
  `componentCompress` "1". Fewer than two CPUs forces thread 0, which clamps
  the level to 8 and compress to 0.
- 0x4F1A20 clamps the level to [6, 9], at most 8 without compress. Edge =
  `1 << level`: **256 at "Player Detail: Low", 512 at High** (level 9, the
  options panel's 8..9, needs a restart).
- Region rectangles are stored at 512 (static init 0x9C8F30) and shifted
  right by `9 - level`.

| # | region | rect at 512 (x, y, w, h) |
|---|---|---|
| 0 | ArmUpper | 0, 0, 256, 128 |
| 1 | ArmLower | 0, 128, 256, 128 |
| 2 | Hand | 0, 256, 256, 64 |
| 3 | TorsoUpper | 256, 0, 256, 128 |
| 4 | TorsoLower | 256, 128, 256, 64 |
| 5 | LegUpper | 256, 192, 256, 128 |
| 6 | LegLower | 256, 320, 256, 128 |
| 7 | Foot | 256, 448, 256, 64 |
| 8 | FaceUpper | 0, 320, 256, 64 |
| 9 | FaceLower | 0, 384, 256, 128 |

### Region painters (table at 0xB6B834, run by 0x4EE0D0 per dirty region)
Each blits the base skin into its own rectangle (0x4F07D0), then its layers
(0x4F08A0):

| region | painter | layers |
|---|---|---|
| ArmUpper | 0x4F09D0 | 0-1 |
| ArmLower | 0x4F0A30 | 0-6 |
| Hand | 0x4F0A90 | 0 |
| TorsoUpper | 0x4F0C10 | **torso underwear only if layers 0-2 are empty**, then 0-4 |
| TorsoLower | 0x4F0CA0 | 0-6 |
| LegUpper | 0x4F0D00 | **pelvis underwear only if layers 0-1 are empty or the chest hides the legs**, then 0-2 (only 1 with the chest flag) |
| LegLower | 0x4F0DB0 | 0-5 (1-3 with the chest flag) |
| Foot | 0x4F0E40 | 0 |
| FaceUpper | 0x4F0AD0 | base skin only for a flag-0x8 skin; face upper, facial hair upper, **scalp upper** |
| FaceLower | 0x4F0B70 | the same with the lower textures |

- The equipment layer order DAT_009F6A00 is verified; it equals
  `include/pipeline/item_textures.hpp`.

### Blits
- An item texture smaller than its region (authored for 256 on a 512
  composite) is grown **2x with neighbour averaging** (0x4E89F0, 0x4EC690,
  0x4ECC20, 0x4ED200): even/even copies, odd x `(a + b) * 128 >> 8`, odd y the
  same down, both `(a + b + c + d) * 64 >> 8`, alpha included.
- A source the region's size or larger copies the mip that fits.
- Blend: depth 0 opaque, 1 keyed, 4/8 `(src*a + dst*(255-a)) >> 8`.
  **Destination alpha is always 255.**

### CharSections (loader 0x4EA1F0; slot `cc+0x194 + 4*(section*3+i)`)

| section | row key | tex 0 | tex 1 | tex 2 |
|---|---|---|---|---|
| 0 skin | (0, skin) | base skin | M2 texture type 8 (0x4EA0B0) | - |
| 1 face | (face, skin) | face lower | face upper | - |
| 2 facial hair | (style, **hair colour**) | lower | upper | - |
| 3 hair | (style, hair colour) | hair, M2 type 6 (0x4EA150) | **scalp lower** | **scalp upper** |
| 4 underwear | (0, skin) | pelvis | torso | - |

- **Skins with flag 0x8** (74 rows): no face and no underwear (0x4EA490,
  0x4EA6B0); the base skin fills the face regions.
- **Bald male** (0x4EA2F0): sex 0, style 0 and ChrRaces flag 0x8 load hair
  style 1's texture in the current colour first. 0x4EA590 does the same when
  a facial-hair style has geosets and a scalp texture is missing. Beards on
  bald heads take the hair colour.
- **Baked NPCs** (0x4F1FC0): `Textures\BakedNpcTextures\<BakeName>` straight
  into M2 type 1; no face, facial hair, scalp or underwear. Any non-player
  unit with a CreatureDisplayInfoExtra uses its bake (0x71D010 / 0x71C500).
  Only players on character models (CMD flag 4) compose.

### Item textures and models
- **Region texture path** (0x4E6FB0): `Item\TextureComponents\<Region>\<name>_U.blp`;
  if missing the U becomes M or F (table 0xAC46A0). **Unisex first; the bare
  name is never tried.** 93 textures in the data have both.
- **Guild tabard** (0x4EC1C0, slot 18 with item flag 1): clears TorsoUpper and
  TorsoLower layers 2-4 and loads `Textures\GuildEmblems\Background_%02d_T?_U`,
  `Border_%02d_%02d_T?_U` and `Emblem_%02d_%02d_T?_U` into layers 2, 3 and 4;
  component flag 0x40.
- **Cape** (0x4F21E0): never composited. `Item\ObjectComponents\Cape\` +
  ModelTexture[0] + `.blp` as M2 texture type 2; no right texture, no suffix.
- **Helm** (0x4EEFA0 / 0x4E7800): `Item\ObjectComponents\Head\` +
  ModelName[0] without its extension + `_` + ChrRaces.ClientPrefix + M/F +
  `.mdx`, **no fallback to the bare name**; texture `Head\` + ModelTexture[0]
  with no race suffix; attachment **11**.
- **Shoulders** (0x4EF4B0 / 0x4EF840): ModelName/Texture [0] on attachment
  **6**, [1] on **5**; a side with no name is skipped; no scale.
- **Quiver** (0x4EF250): `Item\ObjectComponents\Quiver\`, attachment **26**,
  fed only from bag slots 19-22 at char select (0x4E3CD0). In game other
  players' visible-item fields carry no bag, so no quiver.

## 3. Geosets

### Defaults (0x4DFDA0)
`1, 101, 201, 301, 401, 501, 601, 702, 801, 901, 1001, 1101, 1201, 1301,
1401, 1501, 1601, 1701, 1801` for groups 0-18.

- Group 0 from CharHairGeosets (0x4EA050), 1 if none.
- CharacterFacialHairStyles (0x4EA590): group 1 = 100+g0, **group 3 =
  300+g1, group 2 = 200+g2**, group 16 = 1600+g3, group 17 = 1700+g4.

### Helm: HelmetGeosetVisData (0x4EF0D0)
ItemDisplayInfo HelmetGeosetVis[sex] names a row of **seven race bitmasks**,
each tested with `1 << race`:

| column | group set to | meaning |
|---|---|---|
| 1 | 0 = 1 | hair hidden |
| 2 | 1 = 101 | facial 1 |
| 3 | 2 = 201 | facial 2 |
| 4 | 3 = 301 | facial 3 |
| 5 | 7 = 701 | ears hidden |
| 6 | 16 = 1601 | group 16 |
| 7 | 17 = 1701 | eye glow off |

Row 246 hides hair on every race but tauren; row 249 hides the ears only on
tauren and draenei.

### Equipment: FUN_004ED900
One routine for players and Extra NPCs. `GG(k, i)` is GeosetGroup[i] of the
item worn in kind k (head, shoulder, shirt, chest, belt, legs, boots, wrist,
gloves, tabard, cape); `on`/`off` act on exact ids (0x82C7C0); **nothing is
substituted**: an id the model lacks draws nothing.

```
dk = class == 6 || face row flag 4
everything off; 0 on; each group's value on (group 17 = 1703 for dk)
if GG(gloves,0): off 401-499; on 401+GG(gloves,0)
elif GG(chest,0): on 801+GG(chest,0)
if no ArmLower layer 1-6 is painted and GG(shirt,0): on 801+GG(shirt,0)
if GG(chest,2):                     robe: off 501-599, 902-999, 1100-1199, 1300-1399; on 1301+GG(chest,2)
elif GG(legs,2) and !chestHidesLegs: the same with the legs' value
else:
    if GG(boots,0): off 501-599; on 901; on 501+GG(boots,0)
    elif GG(legs,1) and !chestHidesLegs: on 901+GG(legs,1)       kneepads
    else: on 901
    if GG(tabard,0): on 1201+GG(tabard,0)
guild tabard: on 1201; outside robes on 1202
if not a chest robe:
    if no tabard and GG(shirt,1): on 1001+GG(shirt,1)
    if !chestHidesLegs and GG(legs,0):
        >= 3: off 1300-1399; on 1101+GG(legs,0)
        else if no tabard: on 1101+GG(legs,0)
if GG(cape,0): off 1500-1599; on 1501+GG(cape,0)
if GG(belt,0): off 1800-1899; on 1801+GG(belt,0)
```

- `chestHidesLegs` is the chest's ItemDisplayInfo Flags & 4 (component flag
  0x20, set at 0x4EE...: `((flags >> 2) << 5) & 0x20`).
- Wrists never touch geosets; the chest's GG[1] is never read.
- A cape with GG0 = 0 keeps 1501.

### Creatures: CreatureGeosetData (0x4E7790)
CDI column 14: for groups 1-8, nibble g-1 = v, when non-zero, turns
`100g..100g+99` off and `100g+v` on. 39 displays use it.

## 4. Item attachments and glows

### Attaching (CM2Model::AttachToParent 0x831630)
- The id is resolved only through the parent's attachment lookup
  (`H+0xF8/0xFC`). A missing id returns without attaching: no scan by id,
  no key bone, no bone 0. No scale or offset; the child inherits the parent's
  transform at the point and is linked into `parent+0x58`.
- DetachAt 0x827560; HasAttachment 0x8273D0.
- **Hand grip** 0x4E7700: attaching to hand 1 plays anim 15 (HandsClosed) on
  key bones 8-12 (13-17 for hand 2); 0x4E7750 / 0x4E79A0 release them.

### Weapons (0x4EACD0, 0x4EAA70; in game 0x72DBC0)
- `Item\ObjectComponents\Weapon\` (shield: `...\Shield\`) + modelName[0];
  texture modelTexture[0] bound to replaceable slot 2.
- In hand: main hand **1**, off hand **2**, shield **0**, ranged **2**, but
  guns, crossbows, wands and thrown (invtype 0x1A, 0x19) **1**.
- Sheathed (0x4E7940):

  | sheatheType | main | off |
  |---|---|---|
  | 1 | 26 | 27 |
  | 2 | 30 | 31 |
  | 3 | 32 | 33 |
  | 4 (shield) | 28 | 28 |
  | other | not drawn | not drawn |

- No scale: the weapon takes the unit's scale through the attachment.
- Sheathe and draw (0x7310A0) re-parent the existing model, glows included.

### Item visuals (0x4EA8F0)
1. Detach every child of the item model.
2. ItemVisuals[id], range-checked (rows carry garbage slot values such as
   row 28's 90148992).
3. For slot i = 0..4: ItemVisualEffects[row.Slot[i]] (range-checked, path
   non-empty), loaded and **attached at the item model's own attachment i**.
   An item without that point drops the effect.

No scale, offset or fade; they live as long as the item model.

- **Which visual** (0x759050, per `PLAYER_VISIBLE_ITEM_n` record at field
  283 + 2n):
  1. ItemDisplayInfo.ItemVisual (column 23, record +0x5C) when it names a
     row: **enchants are ignored**;
  2. else the **permanent** enchant's (low 16 bits of
     `PLAYER_VISIBLE_ITEM_n_ENCHANTMENT`, SpellItemEnchantment.ItemVisual
     record +0x3C);
  3. else the temporary one's (high 16 bits);
  4. else none.
- 0x6E1290 → 0x6D6BA0 apply it; an enchant-half change on the same item
  only refreshes the glows (0x6E1E10 → 0x6D6C10) on both the hand and the
  sheath point. NPCs only get the display's own visual.
- The dress-up / inspect variant 0x708040 takes the first non-zero visual over
  the item's twelve enchant slots in order.
- **Particle colour** (0x4EA9E0): ItemDisplayInfo ParticleColorID (record
  +0x60) recolours the item model's emitters whose colour index
  (`emitter+0x2A`) is 11, 12 or 13 with ParticleColor Start/Mid/End[i]
  (0x825410 → 0x97A990), RGB only. No row writes 0xFF00FF00.
- **Data**: 1,633 displays have their own ItemVisual, 225 enchants have one.
  Of 1,379 weapon models, 555 carry points 0-4, **312 carry none** (they
  never glow in stock), 233 only 2-4.

  | enchant | visual | effects |
  |---|---|---|
  | Crusader 1900 | 103 | WhiteGlow_Low ×5 |
  | Mongoose 2673 | 155 | MongooseGlow_High ×5 |
  | Berserking 3789 | 178 | RedGlow_High, RedGlow_High, SavageryGlow_High, RedGlow_High, SavageryGlow_High |
  | Executioner 3225 | 165 | DisintigrateGlow_High ×5 |
  | Fallen Crusader 3368 | 160 | SpellSurgeGlow_High ×5 |
  | Razorice 3370 | 1 | DeathKnight_FrozenRuneWeapon_State ×5 |
  | Flametongue 3/5 | 32 | slot 3 only, Shaman_Fire |
  | Windfury 283 | 81 | slot 3 only, Shaman_Wind |
  | Fiery 803 | 25 | RedFlame_Low ×5 |

- Spell "proc" effects have no item-side system: SpellVisualKit
  Left/RightWeaponEffect go to the unit's hand points (0x73A6C0, see
  spell-cast-pipeline.md). Temporary imbues are temporary enchants.

## 5. Creatures

- **Scale** (0x71C110 via 0x722AE0, `unit+0xB3C`):
  `extraRaceScale × CDI.CreatureModelScale × CMD.ModelScale`, ≤ 0 → 1.
  extraRaceScale (0x71C050) is the CDI scale of the Extra race's own display.
  A creature family with level scaling (`MinScale + (MaxScale - MinScale) *
  t`) wins when larger, or always for a pet. **Render scale** (0x71C0E0) =
  that × `unit+0x9C` × OBJECT_FIELD_SCALE_X × (mounted ? the mount display's
  scale : 1). Player displays: tauren male CDI 1.35, tauren female CMD 1.25,
  gnomes CDI 1.15.
- **Skins** (CDI_SetSkins 0x4F20C0, called for every CDI by the unit model
  setup 0x73FCC0, the mount 0x73D5D0 and the preview paths): copy
  CMD.ModelName, cut it after the **last backslash** (no backslash: the name
  alone), append CDI.TextureVariation[i], `CreateTexture` (0x4B9760), and
  `CM2Model::SetReplaceableTexture(11 + i, tex)` (0x825260), which stores it
  in every slot (`model+0xA4[slot]`) whose M2 texture type is 11 + i. Then
  0x4EA9E0 sets the three skin colours. Details:
  - An **empty** variation is skipped: the slot keeps what model init
    (0x832EA0) gave it, which for a non-zero type is **null**.
  - The name is never looked for anywhere but the model's directory, and no
    other display's skins are ever used.
  - **Extension**: 0x4B51C0 finds the last `.` after the last slash, and the
    loaders replace from there with `.blp` (0x4B8BE0, string 0x9F1018) or
    `.tga` (0x4B95B0, 0x9F1020); with no dot the extension is appended.
    `DraeneiMaleKid_01.` (display 19092) therefore asks for
    `DraeneiMaleKid_01.blp`; the shipped file is `DraeneiMaleKid_01..blp`.
  - A named file that is **not there** loads as the **default texture**
    (0x4B5280 fills every mip with 0xFF: solid white) and logs "Texture %s not
    loaded -- replaced with default." (0x4B5EE0).
  - A **null** slot is drawn anyway: the batch setup (0x81F450) passes the
    null to the texture stage (0x685F50), and the D3D device binds nothing
    (0x6A4900 `SetTexture(stage, 0)`). A sampler with nothing bound reads
    (0, 0, 0, 1): **opaque black**, and adds nothing on additive batches.
    The Saronite Animus (28992, WaterElemental.m2 with no variations) is the
    visible case: a black water elemental.
  - The character component never binds 11-13: it sets types 1 (0x4F1FC0),
    2 (0x4F21E0, 0x4EAA70), 6 (0x4EA150) and 8 (0x4EA0B0) only.
- **Sections**: CM2Model init (0x832EA0) turns **every** skin section on
  (`model+0x9C[i] = 1`). A creature model is drawn whole - capes (group 15)
  and eye glows (17) included - unless CreatureGeosetData below changes it;
  only the character component chooses geosets.
- **ParticleColor**: as for items, from CDI column 13 (`+0x34`); 560 displays
  (etherials, elementals, wisps, imps).
- **Collision** (0x6D78C0): CMD CollisionWidth `+0x38`, Height `+0x3C`;
  below 9.5e-7 there is no box; width < 0.4167 logs "collision width is too
  small" (0xA32178). Scale `max(OBJECT_FIELD_SCALE_X, 1) × display scale`.
  Mounted, the box top rises by MountHeight `+0x40` (0x71ED80).
- **CreatureModelAlpha** (CDI `+0x14`) / 255 is the unit's base alpha
  (0x715B50). 1,264 displays are below 255 (440 at 128).

### Creature skins in Wowee (audit, 2026-11-02)

`tools/creature_skin_audit.py` walks every CreatureDisplayInfo row, applies
the rule above to each M2 texture slot and each batch (stage 0 of its texture
combo, from the model's `00.skin`), and lists the displays that draw a batch
with no real file. On the 3.3.5a data (24,262 rows; 21,384 used by
creature_template_model):

| slots | count |
|---|---|
| type 0, the model's own file | 34,506 |
| 11-13, skin file found | 10,924 |
| 11-13, named but not shipped (stock: white) | 11 |
| 11-13, no variation (stock: black) | 60 |
| 11-13 on an Extra display (Wowee puts the bake there) | 517 |
| 1/2/6/8, the composite's | 45,013 |

Wowee had the rule in four places (the creature spawner, the portrait's
`getCreatureSkinPaths`, the local mount and other players' mounts) and between
them five fallbacks stock does not have. Against stock, batch by batch, on the
non-Extra displays (scratch `audit3.py`, which replays the old code):

| | before | after |
|---|---|---|
| batches that match stock | 28,517 | all |
| stock black, Wowee drew another display's skin or the body skin | 100 (29 used) | 0 |
| stock black, Wowee drew white | 11 (10 used) | 0 |
| stock draws a skin, Wowee hid the section (group 15/17/18 skipped) | 55 (34 used) | 0 |
| a different file | 1 | 0 |

The causes, all fixed (`core/creature_skin_rules.hpp`,
`EntitySpawner::bindCreatureSkins`):

1. **Borrowed skins**: a row with no Skin1 took the skins of the lowest
   display on the same model file (`buildCreatureDisplayLookups`), the mounts
   did the same by model id, and a mount with no skin bound got skin1 forced
   onto an empty hard-coded slot or a guessed `Gryphon_Skin.blp`. Saronite
   Animus drew blue, the Riding Wyvern display 19608 brown; the bear mount
   22463 took the brown armour over its own green.
2. **Body texture on an empty slot**: an empty skin slot stayed on the white
   placeholder, and the draw path replaces a white batch in groups 0-5, 7-9
   and 13 with the first type 1/11 texture - a quillboar's, a kodo's or a
   gryphon's skin1 on its armour slot. Empty slots are now bound to an
   explicit black texture (`CharacterRenderer::getUnboundTexture`) and
   missing files to the stock default (`getDefaultTexture`), neither of which
   the draw path replaces.
3. **Names looked for elsewhere**: a name not in the model's directory was
   tried at the root; the extension was appended, not replaced.
4. **Hidden sections**: with no geoset filter every model skipped groups 15,
   17 and 18, meant for character models. On creature models those are skin
   art: the capes of the Four Horsemen, Rexxar, fel orcs, eredar and blood
   elf NPC models. Now only a model with composite slots (types 1/2/6/8)
   skips them.
5. **CreatureGeosetData** (G4) is now applied to non-Extra displays: the 39
   iron dwarf displays drew every beard and armour variant at once.

In game (GM Island, 2026-11-02): the Saronite Animus draws as a huge black
elemental instead of a translucent blue one; its opaque batch is combo
[skin 11, WaterElementalReflect] with `combiners_opaque_add`, so stock adds a
reflection over the black, which Wowee now draws too (L9). Fel orc loincloths (1301/1501) now draw.

Not a skin fault: the "pale naked human" in the Firelord screenshot of
2026-11-02 is the test character (a naked level-4 human, falling after the
teleport); the Firelords (11668, display 12231, FireElemental.m2 +
FireElementalSkin) resolve and draw their skin.

Open: the type 1/2/6/8 slots of the 70 displays on a character-style model
without an Extra (the race displays 49-60 the server uses for players, Winter
Reveler 15713, Velen 17822 ...) are left as they were. Players get their
composite from their own fields; for an NPC on one of these stock's model
init leaves the slots null, but whether nothing else fills them was not
traced and not checked in game.

## 6. Shadow, highlight, translucency

### Unit shadow (0x7E4A40, 0x7E49E0)
- Init loads `Textures\ShadowBlob.blp` and makes the 64 × 8 ramps
  "ShadowAdd" (0x7E36E0) and "ShadowMod" (0x7E3820); cvar `shadowLOD`
  default **1**.
- The blob draws when the model is loaded, flag 0x4000 is clear, the bounds
  are valid, **extShadowQuality < 1** and **shadowLOD == 1**: 0x7E4480
  projects ShadowBlob.blp straight down onto terrain, WMO and liquid
  triangles in a box built from the model's bounds, with alpha = **model
  alpha** and a height ramp; the exact box (it is centred on the model's
  origin, not on the AABB) and ramp are in the full rule below.
- With default cvars (extShadowQuality 0) **stock draws only the blob**. From
  quality 1 up it draws shadow-map shadows from units and no blob.

#### Unit blob shadow: full rule

**Who gets one.** A world entity (made by 0x781A10) carries "casts blob"
flag 0x800 in `entity+0x7C` unless its creation flags have bit 0x2
(`~(flags << 10) & 0x800`). CGObject's registration (0x743760) passes, by
OBJECT_FIELD_TYPE:
- GameObject: 0xB, DynamicObject: 0xA: **no blob, ever**;
- Corpse: 2 (no blob) when the corpse block `+0x6C` bit 1 is set, else 0
  (blob). The field is CORPSE_FIELD_FLAGS and bit 1 CORPSE_FLAG_BONES
  (inferred from the 3.3.5 update-field layout): a body has a blob, bones do
  not;
- Unit (players and creatures): blob unless the creature cache type_flags
  (`[unit+0x964]+0xC`, 0x715DB0) has **0x02000000** (HAS_NO_SHADOW_BLOB).
  Players are always in.
- The only other writer, 0x7801C0 with mask 2, is the
  SMSG_COMMENTATOR_STATE_CHANGED (0x3B6) handler (0x56B8A0, 0x56B830): an
  arena commentator's own player loses its blob.

The other 0x781A10 callers (0x7015D0 flags 6, 0x7FE9E0 flags 0xA, 0x7FB5B0
flags 0xE) never cast one.

**The pass** (0x793980), called once per frame from the world render
0x79A870 (at 0x79ACC4). It walks the entities that survived this frame's
culling (0x793060: frustum 0x983D20, occluders 0x7CCE00 / 0x78FC40;
0x793270 inside WMOs). There is **no distance cut-off and no distance or fog
fade** of its own: a unit has a blob exactly when its model is in the
visible list. Per entity with a model (`+0x34`) whose model is ready
(0x824F00(0,0)) and `+0x7C & 4` clear:
- the unit lookup 0x4D4DB0(guid, typemask 8) succeeds: if 0x800 is set and
  the stand state (UNIT_FIELD_BYTES_1 byte 0, `[unit+0xD0]+0x110`) is not
  **9 (Submerged)**, 0x7E49E0(model, bounds 0x71ED80); then the unit's own
  render call (vtable +0x74). Dead units (stand state 7), ghosts, stealthed,
  swimming and flying units all get one; nothing here tests movement flags,
  water or passengers (vehicle passengers are units of their own, inferred);
- no unit: if 0x800, 0x7E49E0(model, the model's **current animation
  sequence bounds**, M2Sequence +0x20..+0x34 via 0x82CED0).

There is no first-person test. The blob's strength is the model alpha, so
it fades with the unit (camera-near fade, stealth 0.3, ghost 0.5; see
Translucency).

**Unit bounds** (0x71ED80): CreatureModelData **GeoBox** (columns 17-22,
record +0x44..+0x58) of the current display (`unit+0x970`, or 0x717A20
from UNIT_FIELD_DISPLAYID). **Mounted** (UNIT_FIELD_MOUNTDISPLAYID > 0): the
rider box is raised by `mount CMD.MountHeight (+0x40) - riderHeight/2`
(riderHeight = maxZ - minZ) and then unioned (0x715130) with the mount
display's GeoBox. The model whose matrix and alpha are used is the
entity's model, the unit's root model, which is the mount when mounted
(inferred from §7), so the rider part is scaled by the mount scale too.

**Gate** (0x7E49E0): model ready (0x824FC0), model flag `+0x10 & 0x4000`
clear, bounds valid (0x70BD20: min < max on all axes), extShadowQuality
(`[0xD38048]+0x30`) < 1, shadowLOD (0xAF3E08) == 1. Then 0x7E4480(bounds,
model, depth bias 0.4 = 0x9F98D8).

**Box** (0x7E4480; B = the local bounds, M = the model matrix `+0xB4`,
row-vector, translation T = M[12..14]):
1. `hx = (B.maxX - B.minX)/2`, `hy = (B.maxY - B.minY)/2`; return if either,
   or `B.maxZ - B.minZ`, is below 2.38e-7 (0x9EA27C).
2. Local box `(-hx, -hy, B.minZ)..(hx, hy, B.maxZ)` - **xy centred on the
   model origin**, z as in B - scaled by `s = |M row 0|` (0x5FECB0), then
   every component of both corners **clamped to [-5, 5]** (0x7E2CE0 min 5
   0x9EBF34, 0x7E2D20 max -5 0x9F267C). Call the results `Hx, Hy` and
   `h = (zmax' - zmin')/2`.
3. `R` = M's 3 × 3 divided by s (skipped when s == 1 within 2.38e-7). The
   four corners `(±Hx, ±Hy, 0)·R` and their xy AABB + T.xy give the query
   box in x and y; in z it is **`T.z - 1.6667 h .. T.z + h`** (0xAF3E14,
   0xAF3E18), independent of the bounds' z position.

**Texture matrices** (0x7E2D60 with the extra matrix E; both stages use
texgen 2, the camera-relative position, and the translation by `camera -
centre` makes the input `d = p - c`, c = box centre; inferred to be exact):
- stage 0 (ShadowBlob): `M0 = T(-c) · S(1/DX, 1/DY, 1) · Rz(-90°)
  (0x9F1FF4) · E`, then translation `+0.5, +0.5`, where DX, DY are the query
  box sizes and `E = S(DY, DX, 1) · Rᵀ · S(1/2Hy, 1/2Hx, 1)`. This reduces to
  `q = (d.y, -d.x, d.z)`, `u = 0.5 + dot(q, R row 0)/(2Hy)`,
  `v = 0.5 + dot(q, R row 1)/(2Hx)`; for an upright (yaw θ) model:
  **`u = 0.5 + ly/(2Hy)`, `v = 0.5 - lx/(2Hx)`**, with (lx, ly) = d in the
  model's own axes (lx forward). The blob is the rectangle ±Hx × ±Hy around
  the model's origin, **turned with the model's facing**, stretched to it
  (not square, not world-aligned);
- stage 1 (ramp): `u = (d.z)/dz + 0.5 = (z - zmin)/dz`, `v = 0.5`, with dz =
  zmax - zmin = 2.667 h (0x7E2D60 matrix rows 8/10 = 1/dz, 12-14 = 0.5).
- Both textures are created clamped and linear (0x681BE0(1, 0, 0, ...):
  filter 1, wrapU = wrapV = 0), so triangles that reach outside the box read
  the edge texels.

**Render state** (0x7E4480 then 0x7E3E80; EGxRenderState numbering checked
against 0x798DA0, EGxTexOp from the name table 0xB1D5CC: Mod 0, Mod2x 1,
Add 2, PassThru 3, Decal 4, Fade 5):
- state 6 blend = **4 (Mod: dst = src × dst)**; 7 alpha ref reset for that
  blend (0x408C30); 11 lighting 0; 12 **fog 0 (off)**; 15 **depth write 0**;
  depth test left on with its function unchanged (state 14 is only set, to
  1, when the bias is 0); state 0 polygon offset = `2 × 0.4 × 1/32768`
  (0xA41170) = 2.44e-5 (0x763C70); 10 fog colour 0xFFFFFFFF;
- texture 0 = ShadowBlob; 37 ColorOp0 = **5 Fade**, 45 AlphaOp0 = **3
  PassThru**; with blend 4 texture 1 = **ShadowMod** (0xD3803C), 38 ColorOp1
  = 46 AlphaOp1 = **2 Add** (with any other blend, such as the spell
  circle's, and no texture 1 bound: ShadowAdd with ops 0 Mod); 53/54
  TexGen = 2; 61/62 = 1;
- vertex colour `0xFFFFFF | A << 24`, `A = floor(clamp(model+0x178, 0, 1) ×
  255 + 0.5)`.
- Fade is `lerp(primary, tex, primary.a)` (combiners_fade: `rgb = tex ×
  a + col × (1 - a)`), so the pixel is

  `dst' = dst × min(1, lerp(1, blob.rgb, A) + (1 - ramp(u1)))`

  per channel: white where the blob is white, the alpha is zero or the ramp
  is zero.

**Ramps** (64 × 8, every row equal; i = 0..63, `x = 12 i / 63`, `r(x)` =
`x/2` for x < 2, 1 for 2 ≤ x < 10, `max(0, (12 - x)/2)` from 10):
- ShadowAdd (0x7E36E0): BGRA `(255, 255, 255, round(255 r))`;
- ShadowMod (0x7E3820): `g = round(255 (1 - r))`, BGRA `(g, g, g, g == 255 ?
  0 : 255)`.
In height: full shadow for `z - T.z` in `[-1.222 h, +0.556 h]`, linear to
none at `-1.667 h` and `+h`.

**ShadowBlob.blp** (wotlk `textures/shadowblob.blp`): 32 × 32, BLP2
palettised, 1-bit alpha, mips; neutral grey, value 160 (0.627) in a flat
core 12 texels across (radius 6, centre 16, 16), falling about 15 per texel
to 255 at radius 12, white outside. Only its colour is used (Fade reads
tex.rgb; AlphaOp is PassThru; Mod ignores alpha). At full strength the
ground under a unit is multiplied by 0.627.

**Receivers** (0x7E3E80 → 0x7E35F0; the blob passes query flags
**0x220122** and the M2 flag 0, so **M2 doodads are not receivers**;
0x7A6AF0 sends `& 0x300F0` to map objects 0x7A6940, `& 0x30100` to terrain
0x7A6830):
- **terrain** (0x100, 0x7A6260): every chunk cell in the box's xy range not
  under a low-resolution hole (MCNK holes, table 0xA3FAF0); each cell's 4
  triangles (corner, corner, centre) are kept unless all three vertices are
  outside the same box plane (outcodes 0x7A61D0). No facing test (the
  heights path of 0x7E32F0);
- **WMO** (0x20, 0x7AEF00 → 0x7CB7B0): groups whose bounds overlap the box,
  their collision BSP triangles in the box (0x7CA920 / 0x7C7AE0, with the
  MOPY mask 0x7AE140(flags) = 0x88; its meaning was not traced). At draw
  (0x7E32F0) a triangle is kept only if `(b-a).x (c-a).y - (c-a).x (b-a).y
  >= 0`: up-facing (normal.z ≥ 0 for counter-clockwise front faces;
  inferred), so walls and steep slopes still take it, ceilings do not;
- **liquids** (0x20000): terrain liquid (0x7CE960) and WMO liquid
  (0x7C94B0), all liquid types (0x10000 alone would keep only types with flag
  4). The blob is drawn on the liquid surface triangles before the WMO
  liquids (0x793D20) draw over them (inferred), so it shows through the
  water's transparency.
- Limits: 32 batches, 0xC000 indices (overflow sets 0xCB7528 bit 1).

**Order.** In 0x79A870 the pass comes after the terrain and map-object
passes (0x798DA0, 0x7964A0, 0x795F80 the low-detail terrain, 0x7968D0 /
0x796C10), 0x7F31C0 and the sky (0x7F09B0), and before 0x8A2F00 and the WMO
liquids (0x793D20). The unit's own model is handed to the M2 scene
(0x791240) and drawn later (inferred), so the blob is under the unit and
under every M2. There is **one draw per unit**, no second draw (the spell
circle's second, liquid-only draw is its own).

### Highlight (0x743C70 / 0x743BC0)
- `SetHighlight(kind)` sets bit `24+kind` in obj+0xBC (0 target, 1
  mouseover) and sets the model **emissive** = the world unit ambient
  `0xD38CAC` / 255 (a snapshot). Clearing returns it to 0.
- Callers: mouseover change 0x51F790, target change 0x524BF0, the cvar
  callback 0x513CF0; all gated on `unitHighlights` (0xBD09DC, default 1).
- Effect: `col = sat(sat(lit) + ambient)`: about one ambient's worth of light,
  uniform, on **both the mouseover and the target**.
- The selection circle (0x725980, size 0x720330, UnitSelectTexture.blp through
  the blob projector) is already matched by Wowee (overlay_system.cpp).

### Translucency
- Model alpha (0x71AC30) = `unit+0xCB/255 × unit+0xC8/255`. +0xC8 is the
  faded presentation alpha: SetAlpha 0x744030 fades a byte linearly over
  +0xC4 ms. +0xCB is the camera-near fade and vehicle sharing.
- The target (0x71ABE0, 0x71E930, 0x73EB50) = **CDI.CreatureModelAlpha/255**
  × the active effect's alpha (`unit+0x984`, +0xE8), faded over its +0xA0 ms
  or **1000 ms**.
- The effect alpha is the aura spell visual's **StateKit** (SpellVisual
  column 4) SpellVisualKit **CharProc 14** parameter 0:

  | spell | kit | alpha |
  |---|---|---|
  | Stealth 1784, Prowl 5215, Shadowmeld 58984 | 312 | **0.3** |
  | Invisibility 66 | 3450 | **0.5** |
  | Invisibility 32612 | 6859 | **0.35** |
  | Ghost 8326 | 989 | **0.5**, plus CharProc 1 colour 0x8CB9FD |

  (The writer into `unit+0x984` was not traced; the link rests on the data
  and the reader.)
- At reduced alpha every batch blends (m2-runtime.md §3): Opaque and AlphaKey
  become Alpha blending, the alpha-key reference becomes `alpha × 224/255`,
  batches below 1e-4 are skipped. The blob shadow uses the same alpha.

## 7. Mounts (0x73D5D0)

- The mount display's CDI.CreatureModelScale goes to `unit+0x990`; the mount
  model (CMD.ModelName, skins 11-13, ParticleColor, geoset nibbles) becomes
  the unit's root model (0x717910), drawn at the unit render scale times that
  mount scale.
- The rider (`unit+0xB4`) is attached at the mount's **attachment 0** with a
  local **uniform scale 1 / mountScale** (0x4E26F0): it keeps its own size, and
  the seat bone's full transform drives it, with no offsets.
- The mount takes the rider's tint and alpha; the rider gets alpha 1, so
  stealth and ghost alpha live on the mount.
- **Rider animation** 0x735820 with `unit+0xB7C`: **91 (Mount)** by default
  (0x71E9F4); an aura of type 78 sets it to the StateKit AnimID of its spell
  visual when ≥ 0 (0x724820), reset on removal (0x71E930). Seven mount spells
  carry one (anims 120, 127, 484; 61309 the flying carpet).
- CMD.MountHeight is not used for seating, only the can-mount headroom test
  (0x6D7720).

## 8. Wowee: gap table

"Fixed" means fixed in this change.

| # | Area | Stock | Wowee before | Now |
|---|---|---|---|---|
| L1 | Light equation | `tex × sat(amb + diff·N·L + points)` | ambient + sun + local lights added unclamped, overbright | **Fixed** (character.frag) |
| L2 | Unit ambient boost | ×(1 + 2·AreaTable.AmbientMultiplier), clamped, eased | none | **Fixed**: `setUnitAmbientArea`, `unit_light_rules.hpp` |
| L3 | Point lights | 4 nearest M2 lights, `1/(0.7d+0.03d²)`, N·L | 64 local lights with a 0.22 wrap and `(1-d/r)²` | **Fixed** (2026-11-03): with the "Torch and lamp light" enhancement off (stock) the renderer gathers only M2 type-1 lights (`M2Renderer::gatherStockPointLights`, colour = diffuseColor × diffuseIntensity at the first key) and units, doodads and WMOs take the 4 nearest, terrain the 3 nearest (`stockPointLights` in the four .frag). Differences left: chosen per pixel, not per object; first key only (no animated intensity, no model alpha); the lights of models drawn by CharacterRenderer (an NPC carrying a torch) are not gathered; the term is added after the clamp. On, the old 64-light look. |
| L4 | Interior light | per-unit MOCV×2 under the feet, 168/96 split, fixed direction | one indoor factor, building ambient, sun faded to 15% | **Partly**: the split and fixed direction applied to the group colour Wowee has; no per-unit MOCV ray (needs a WMO query) |
| L5 | Specular / normal map / POM | none | already 0 for batches | dead shader path removed |
| L6 | Unlit | `tex × M2Color` | `tex × 2`, and every blend ≥ 3 unlit | **Fixed**: flag 0x01 only, no doubling; M2Color RGB on characters still open (scenes use it) |
| L7 | Fog | world curve with exponent | linear distance | **Fixed** (`stock_fog.glsli`) |
| L8 | Highlight | emissive = unit ambient, mouseover and target | none | **Fixed** (`setHighlightedInstances`, Application::updateUnitHighlights); no cvar toggle |
| L9 | Env-mapped shine | sphere-map units on items | not reproduced | **Fixed**: stock shader selection and combiners, second texture stage with sphere-map coordinates (m2-runtime.md §3 "Shader selection") |
| L10 | Unit shadow | blob at default settings | shadow-map casters | **Fixed** (2026-11-03): "Sun shadows" off (the stock default and the Classic preset) draws the blob (`OverlaySystem::renderUnitBlobs`, `unit_blob.*.glsl`): ShadowBlob.blp in model axes, footprint ±half box ×scale clamped to 5, ShadowMod ramp over feet−1.667h..feet+h, Mod blend, model alpha. Differences left: draped over a lattice of terrain/WMO floor samples (≥0.5 yd) rather than the world's own triangles; liquids not receivers; the visual bind-pose box stands in for CreatureModelData's GeoBox; HAS_NO_SHADOW_BLOB and the Submerged stand state are not read; capped at 128 units within 150 yd; the shadow pass still runs with shadows off (see Renderer::renderShadowPass). |
| L11 | Terrain-shadow darkening, weather ×(1-0.35w) | yes | no / owned by lighting | open |
| T1 | Transparency values | kit CharProc 14: 0.3 / 0.5 / 0.35 / 0.5 | 0.5 for all | **Fixed** (kit lookup, fallbacks 0.3/0.5) |
| T2 | CreatureModelAlpha | base alpha | ignored | **Fixed** |
| T3 | Ghost tint 0x8CB9FD, 1000 ms fade | yes | no | open |
| C1 | Composite scalps | painted after facial hair | never read | **Fixed** (all paths) |
| C2 | Special skins (flag 0x8) | no face, no underwear | always | **Fixed** |
| C3 | Bald male hair texture | style 1 in colour | none | **Fixed** |
| C4 | Item suffix | `_U` then `_M/_F` | gendered first | **Fixed** |
| C5 | 2x upscale | averaging | nearest | **Fixed** (`character_composite_rules.hpp`) |
| C6 | Underwear conditions, dest alpha 255, guild emblem | stock rules above | always / max / none | open |
| G1 | Equipment geosets | FUN_004ED900, exact ids | four hand-written versions | **Fixed**: `core::stockEquipmentGeosets` + `buildStockCharacterGeosets` for the player, other players, Extra NPCs and the portrait |
| G2 | Helm masks | 7 race bitmasks | any non-zero column hides hair; ears, facial hair, glow never hidden | **Fixed** |
| G3 | DK eye glow 1703 | class 6 | missing | **Fixed** (player and other players) |
| G4 | CreatureGeosetData | nibbles | ignored | **Fixed**: applied to non-Extra displays at spawn (39 iron dwarf displays) |
| G5 | Creature sections | all on (0x832EA0) | groups 15/17/18 skipped on every unfiltered model | **Fixed**: skipped only on character models |
| K1 | Creature skins 11-13 | own TextureVariation, model dir, `.blp` extension | borrowed sibling skins, root lookup, appended extension | **Fixed** (`creature_skin_rules.hpp`, §5) |
| K2 | Empty / missing skin | null (black) / default white | white placeholder, then the body texture | **Fixed** (unbound and default textures) |
| A1 | Glows on other players and NPCs | yes | none | **Fixed** (`attachUnitWeapons`) |
| A2 | Glow priority | display, perm, temp | temp over perm, display ignored | **Fixed** (`chooseWeaponItemVisual`) |
| A3 | Missing weapon point | dropped | piled at the grip | **Fixed** (strict lookup) |
| A4 | Glow follows the weapon bone | yes | offset only | **Fixed** |
| A5 | Glow particles | drawn | **never drawn**: CharacterRenderer draws no emitters | **Fixed**: glows are M2Renderer instances driven by the weapon bone |
| A6 | Enchant field for other players | field 284+2n | not read | **Fixed** (`getOtherPlayerWeaponEnchants`) |
| A7 | Hand grip, ParticleColor, quivers | yes | no | ParticleColor **fixed** (particles.md §8); hand grip and quivers open |
| S1 | Player race scale | tauren 1.35/1.25, gnome 1.15 | 1.0 | **Fixed** (local and remote) |
| S2 | Mount scale | unit scale × mount CDI scale | 1.0 | **Fixed** |
| S3 | Rider seat | attachment 0, full transform, rider own size | remote: fixed height; local: offsets, 35% tilt | **Fixed** for remote riders, and for the local rider in mounts-taxi-vehicles.md (M5) |
| S4 | Rider anim from mount kit | yes | always 91 | **Fixed** (mounts-taxi-vehicles.md M6) |
| S5 | Creature particles in general | drawn | CharacterRenderer draws no emitters (fire elementals, wisps) | **Fixed**: emitter twins in M2Renderer, with the item models' own (particles.md §8) |

## 9. In game

- **Units' brightness**: outdoors, units are no longer brighter than the
  terrain; in zones with an AreaTable ambient multiplier (387 areas at 0.6)
  their shadowed side is visibly lifted. Compare a character's dark side at
  noon in Elwynn and in a 0.6 zone.
- **Indoors**: a unit in a building is lit from above (+x+y) rather than by a
  15% sun; inns should look like stock's warm top light.
- **Mouseover and target**: both glow slightly brighter, uniformly.
- **Weapon glows**: Crusader, Mongoose, Berserking, shaman imbues and epic
  weapons' own glows now show particles, on yourself, other players and NPCs
  with glowing weapons, attached to the right points; weapons without the
  points show none (as in stock).
- **Stealth**: a stealthed rogue is at 0.3, not 0.5; mage Invisibility 0.5;
  ghostly NPCs (CreatureModelAlpha) are see-through.
- **Geosets**: gloves over sleeves, robes without boot tops, legs with pant
  flaps (GeosetGroup[0]), belts on NPCs, a tauren's hair under a helm that
  stock leaves showing, ears hidden by the helms that hide them.
- **Size**: tauren and gnome players are their stock size; mounts carry their
  display scale; other players sit on the mount's seat bone and lean with it.
- **Hair**: scalp textures appear at the hairline; bald dwarves' and humans'
  beards take the hair colour.
