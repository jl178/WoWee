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
  4. `CM2Lighting::Resolve` **0x835280**, then CameraSpace 0x8350A0, then
     the GPU setup **0x8353D0**: light 0 is directional, lights 1-3 the point
     lights.

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
- **Skins** (0x4F20C0): the CMD model's directory + CDI.TextureVariation[i] →
  M2 texture type 11 + i; empty names skip; character models skip this.
- **ParticleColor**: as for items, from CDI column 13 (`+0x34`); 560 displays
  (etherials, elementals, wisps, imps).
- **Collision** (0x6D78C0): CMD CollisionWidth `+0x38`, Height `+0x3C`;
  below 9.5e-7 there is no box; width < 0.4167 logs "collision width is too
  small" (0xA32178). Scale `max(OBJECT_FIELD_SCALE_X, 1) × display scale`.
  Mounted, the box top rises by MountHeight `+0x40` (0x71ED80).
- **CreatureModelAlpha** (CDI `+0x14`) / 255 is the unit's base alpha
  (0x715B50). 1,264 displays are below 255 (440 at 128).

## 6. Shadow, highlight, translucency

### Unit shadow (0x7E4A40, 0x7E49E0)
- Init loads `Textures\ShadowBlob.blp` and makes the 64 × 8 ramps
  "ShadowAdd" (0x7E36E0) and "ShadowMod" (0x7E3820); cvar `shadowLOD`
  default **1**.
- The blob draws when the model is loaded, flag 0x4000 is clear, the bounds
  are valid, **extShadowQuality < 1** and **shadowLOD == 1**: 0x7E4480
  projects ShadowBlob.blp straight down onto terrain chunks and up-facing WMO
  triangles in a box from the model's AABB, depth `zmin - 1.667 h/2 ..
  zmax + h/2`, with alpha = **model alpha**, ramp `t*0.5` for t < 2, 1 to 10,
  `(12-t)*0.5` after.
- With default cvars (extShadowQuality 0) **stock draws only the blob**. From
  quality 1 up it draws shadow-map shadows from units and no blob.

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
| L3 | Point lights | 4 nearest M2 lights, `1/(0.7d+0.03d²)`, N·L | 64 local lights with a 0.22 wrap and `(1-d/r)²` | **Fixed** falloff and N·L; still every gathered light within its radius, not the 4 nearest, and the gather (M2Renderer) includes non-stock lights |
| L4 | Interior light | per-unit MOCV×2 under the feet, 168/96 split, fixed direction | one indoor factor, building ambient, sun faded to 15% | **Partly**: the split and fixed direction applied to the group colour Wowee has; no per-unit MOCV ray (needs a WMO query) |
| L5 | Specular / normal map / POM | none | already 0 for batches | dead shader path removed |
| L6 | Unlit | `tex × M2Color` | `tex × 2`, and every blend ≥ 3 unlit | **Fixed**: flag 0x01 only, no doubling; M2Color RGB on characters still open (scenes use it) |
| L7 | Fog | world curve with exponent | linear distance | **Fixed** (`stock_fog.glsli`) |
| L8 | Highlight | emissive = unit ambient, mouseover and target | none | **Fixed** (`setHighlightedInstances`, Application::updateUnitHighlights); no cvar toggle |
| L9 | Env-mapped shine | sphere-map units on items | not reproduced | open |
| L10 | Unit shadow | blob at default settings | shadow-map casters | open (Wowee's maps match stock quality ≥ 1) |
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
| G4 | CreatureGeosetData | nibbles | ignored | pure rule added and tested; not wired for non-Extra creatures |
| A1 | Glows on other players and NPCs | yes | none | **Fixed** (`attachUnitWeapons`) |
| A2 | Glow priority | display, perm, temp | temp over perm, display ignored | **Fixed** (`chooseWeaponItemVisual`) |
| A3 | Missing weapon point | dropped | piled at the grip | **Fixed** (strict lookup) |
| A4 | Glow follows the weapon bone | yes | offset only | **Fixed** |
| A5 | Glow particles | drawn | **never drawn**: CharacterRenderer draws no emitters | **Fixed**: glows are M2Renderer instances driven by the weapon bone |
| A6 | Enchant field for other players | field 284+2n | not read | **Fixed** (`getOtherPlayerWeaponEnchants`) |
| A7 | Hand grip, ParticleColor, quivers | yes | no | ParticleColor **fixed** (particles.md §8); hand grip and quivers open |
| S1 | Player race scale | tauren 1.35/1.25, gnome 1.15 | 1.0 | **Fixed** (local and remote) |
| S2 | Mount scale | unit scale × mount CDI scale | 1.0 | **Fixed** |
| S3 | Rider seat | attachment 0, full transform, rider own size | remote: fixed height; local: offsets, 35% tilt | **Fixed for remote riders**; the local rider (animation_controller) is unchanged |
| S4 | Rider anim from mount kit | yes | always 91 | open |
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
