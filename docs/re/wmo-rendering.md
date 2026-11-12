# WMO rendering (stock 3.3.5a build 12340)

How the stock client draws a WMO - buildings, caves, dungeons - apart from
the light, which `world-lighting.md` §6 covers (the MOCV fix, the portal
attenuation of trans vertices, the four light modes, F_SIDN, F_UNFOGGED).
This part is the rest of what a batch is drawn with: its effect, blend,
alpha test, culling and texture wrap; the second MOTV/MOCV; which groups are
drawn from inside and outside; the doodads and their light; MOLT; the liquids;
the MOSB skybox. Reverse engineered from `Wow.exe` with Ghidra; addresses are
virtual addresses in that binary. The stock programs were read as text from
`Data/expansions/wotlk/shaders/{vertex/arbvp1,pixel/arbfp1,effects}`.

The pure rules are in header-only files with unit tests:

| Header | What | Test |
|---|---|---|
| `include/rendering/stock_wmo_render.hpp` | EGxBlend factors, alpha reference, culling, wrap, effect table, env coordinate, Env/EnvMetal/Composite | `tests/test_stock_wmo_render.cpp` |
| `include/pipeline/stock_wmo_liquid.hpp` | the MLIQ LiquidType | `tests/test_stock_wmo_render.cpp` |
| `include/rendering/stock_wmo_light.hpp` | (earlier) MOCV fix, portal attenuation, light modes | `tests/test_stock_world_fx.cpp` |
| `include/rendering/stock_wmo_portal.hpp` | the portal walk (rectangles, clip, inside/outside seeds), rectangle frustum planes, WMO doodad light from MODD colour | `tests/test_stock_wmo_portal.cpp` |

Each section ends with what Wowee did before and does now. §9 is the gap
list.

## 1. Loading

### Root (0x7d7470)
The root's chunks are taken in a fixed order, by size, without looking at
the magic: MOHD (+0x120), MOTX (+0x124), MOMT (+0x160, count = size >> 6 at
+0x19c), MOGN, MOGI (+0x130, 0x20 each), MOSB (+0x12c), MOPV, MOPT (NaN planes
replaced by (0,0,1), d), MOPR, MOVV, MOVB, MOLT (+0x148, 0x30 each), MODS
(+0x14c, 0x20 each), MODN, MODD (+0x154, 0x28 each), MFOG, and MCVP when it
follows.

- **An empty MOSB clears MOGI flag 0x40000 on every group** (0x7d7470), and
  0x7d82e0 does the same on the group's own flags. So a group shows the
  skybox only when the root names one.
- `0x7d7710` binds each material's textures: MOTX + texture1 (an empty name
  becomes `createcrappygreentexture.blp`) and, only when vertex shaders are
  on, MOTX + texture2, at MOMT +0x38 / +0x3c.
- `0x7d7eb0` copies the MOHD ambient (+0x1a0) and bounds.

### Group (0x7d82e0, 0x7d7c30)
- Flags (+0x30) are the **MOGP** flags.
- The sub-chunks follow MOVT/MONR/MOTV/MOBA in flag order: MOLR (0x200),
  MODR (0x800), MOBN+MOBR (0x1), MPBV/MPBP/MPBI/MPBG (0x400), MOCV (0x4,
  fixed by 0x7d7380 unless MOHD 0x8), MLIQ (0x1000), MORI/MORB (0x20000),
  **a second MOTV (0x2000000, +0xf4) and a second MOCV (0x1000000, +0x10c)**.
  In the wotlk data 31 groups carry both (Icecrown Citadel's exterior and
  the Icecrown dungeons), every one with MapObjUComposite materials.
- A group named "antiportal" is flagged and not drawn (0x7d82e0).
- The MOBA batches' material ids, start and end are copied, and the first
  batch whose material has a blend other than 0 clears group bit 4 (the
  group is "all opaque" otherwise).

**Wowee before:** the second MOTV and MOCV were read over the first, so those
31 groups drew their second layer's coordinates and blend weights as their
colours. **After:** read into `texCoord2` / `color2Alpha`, the vertex gains a
`layer2` attribute (80 bytes instead of 64).

## 2. Effects (MOMT shader)

`0x7afee0` loads two effect tables from `shaders\effects\mapobj.wfx` and
`mapobju.wfx`:

| MOMT shader | classic (0xd1c3f0) | unified, MOHD 0x2 (0xd1c3d4) |
|---|---|---|
| 0 | MapObjDiffuse | MapObjUDiffuse |
| 1 | MapObjSpecular | MapObjUSpecular |
| 2 | MapObjMetal | MapObjUMetal |
| 3 | MapObjEnv | MapObjUEnv |
| 4 | MapObjOpaque | MapObjUOpaque |
| 5 | MapObjEnvMetal | MapObjUEnvMetal |
| 6 | (null) | MapObjUComposite |

The draw loops (`0x7ac7d5` in 0x7ac6a0, the same in 0x7ac9f0 and 0x7a9380)
replace shader 0 by **4 (Opaque)** when the material's blend is 0 and its
texture has no alpha (`0x4b54f0`); then `effect = table[shader]` and
0x872f90 binds it. A classic root with shader 6 would bind a null effect;
no classic root in the data has one.

In the data (MOMT, all wotlk WMOs): shader 0 with blend 0/1 almost
everywhere; 1 (Specular) 517, 2 (Metal) 90, 3 (Env) 192, 5 (EnvMetal) 1273
(Silvermoon, Dalaran, Karazhan, the Nexus), 6 (Composite) 52, all in
Icecrown.

The pixel programs (`pixel/arbfp1/mapobj*.bls`, first variant), `C` the
vertex program's lit colour, `t0` texture 0 at UV set 0, `t1` texture 1:

| program | rgb | alpha |
|---|---|---|
| Diffuse | 2 C t0 | C.a t0.a |
| Opaque, Specular, Metal | 2 C t0 | C.a |
| Env | 2 C t0 + t0.a t1 | C.a |
| EnvMetal | 2 C t0 + t0.rgb t0.a t1 | C.a |
| Composite | 2 C lerp(t1, t0, S.a) | C.a lerp(t1.a, t0.a, S.a) |

- **No program reads the secondary colour except Composite**, so the
  Specular and Metal effects draw exactly like Opaque (§6 of
  world-lighting has the rest of the lighting).
- **Env coordinates** (`MapObjDiffuse_T1_Refl`, `MapObjUDiffuse_T1_Refl`):
  `p` the view-space position, normalised; `n` the view-space normal,
  normalised; `texcoord1 = p.xy - 2 n.xy (n . p)`, which is
  `reflect(p, n).xy` - **no 0.5 scale and bias**, unlike the M2 sphere map.
  `t1` is the material's texture 2 with the default (repeat) wrap.
- **Composite** (`MapObjUDiffuse_Comp`): UV set 0 for t0, the second MOTV
  (attribute 7) for t1, the second MOCV (attribute 5) as the secondary
  colour; its alpha `S.a` blends the layers. The light is the unified one
  (`MOCV` added to `0.498 L`).

**Wowee before:** one texture; Env/EnvMetal drew as Diffuse, Composite drew
the first layer only (with the wrong UVs, see §1). **After:** binding 3 of
the material set holds MOMT texture 2; wmo.frag adds `t0.a env` /
`t0 t0.a env` for Env/EnvMetal with the unscaled reflection coordinate from
wmo.vert, and mixes the two layers for Composite. A batch whose second
texture is missing draws as Metal (2 C t0).

## 3. Blend, alpha test, culling, wrap (per batch)

Every draw loop sets, per batch (0x7ac6a0, 0x7ac9f0, 0x7a9380):

- **Render state 0x11, culling** = `!(MOMT flags & 0x4)`. The GL device
  (`0x69d1b7`) turns 1 into `glEnable(GL_CULL_FACE)` with
  `glFrontFace(GL_CCW)` and the default back-face cull. **Every material
  without F_UNCULLED is one-sided.** (1019 of 22,000 materials have it.)
- **Render state 6, blend** = MOMT `blendMode`, an EGxBlend, straight. The
  GL device (0x69cefa) disables GL_BLEND for 0 and 1 and otherwise calls
  `glBlendFunc(src[b], dst[b])` from 0xad8d90 / 0xad8dc0:

  | b | name | src, dst |
  |---|---|---|
  | 0 | Opaque | blend off |
  | 1 | AlphaKey | blend off |
  | 2 | Alpha | SrcAlpha, 1-SrcAlpha |
  | 3 | Add | SrcAlpha, One |
  | 4 | Mod | DstColor, Zero |
  | 5 | Mod2x | DstColor, SrcColor |
  | 6 | ModAdd | DstColor, One |
  | 7 | InvSrcAlphaAdd | 1-SrcAlpha, One |
  | 8 | InvSrcAlphaOpaque | 1-SrcAlpha, Zero |
  | 9 | SrcAlphaOpaque | SrcAlpha, Zero |
  | 10 | NoAlphaAdd | One, One |
  | 11 | ConstantAlpha | ConstAlpha, 1-ConstAlpha |

  In the data: blend 0 (21,800), 1 (1,104), 2 (12), 3 (3), 5 (1).
- **The alpha reference goes with the blend** (0x873ee0, table 0xad8b7c,
  times 1/255): 0 (no test) for Opaque; **224** for AlphaKey; **1** for
  Alpha, Add, Mod, Mod2x, ModAdd; 0 for 7-11. It is tested against the
  pixel program's alpha (`alpha >= ref`), which is `C.a t0.a` only for
  Diffuse and Composite.
- **A trans (A) batch ignores its material's blend**: its outdoor pass is
  drawn with SrcAlphaOpaque (9) and its room pass with InvSrcAlphaAdd (7)
  (§6 of world-lighting). Neither has an alpha reference, so **a trans
  batch is never alpha-tested** - 48 alpha-key A batches in the data
  (Arakkoa and Draenei hut trims, Drak'Tharon) draw their cut-outs solid,
  in the room's light.
- **Depth**: the WMO loops never change the depth write; blended batches
  write depth like the rest, in MOBA order.
- **Fog** stays the DayNight fog for every blend (no per-blend fog colour as
  M2 has at 0xA45390).
- **Texture 0's wrap** (0x681450): `S` repeats unless F_CLAMP_S (0x40), `T`
  unless F_CLAMP_T (0x80). Texture 1 keeps the default.
- **Batch culling** (0x7a7630): each batch is tested on its own (MOBA's
  short bounding box) before it is drawn; a group's batches are walked in
  file order.

**Wowee before:** no culling at all; blend 0/1 opaque, everything else the
same alpha blend without depth write; alpha key at 0.5 on texture alpha, for
trans batches too; repeat always; batches in hash order. **After:** a
pipeline per EGxBlend x culling (stock factors, depth written,
`VK_CULL_MODE_BACK_BIT` with CCW front unless F_UNCULLED); the alpha
reference of the blend tested in wmo.frag against the program's alpha; trans
batches opaque and untested; F_CLAMP_S/T samplers; merged batches drawn in
the order of their first MOBA batch. ConstantAlpha (unused) draws as Alpha.

## 4. Shadow map

`0x8745d0(x)` binds the sun shadow map to sampler 0x19 for x = 0, a white
stand-in for x = 1. The draw loops pass:

| path | gets the shadow map |
|---|---|
| classic, no MOCV (0x7ac6a0) | exterior groups (flags & 0x48) |
| classic, MOCV (0x7ac9f0) | **only a trans batch's outdoor pass**; B and C batches never |
| unified (0x7a9380) | a trans batch's outdoor pass; the other batches of exterior groups |

The shadow programs scale the whole colour by 0.7 + 0.3 lit, lit or unlit.

**Wowee before:** every batch in light mode 1 or 2. **After:** the table
above (`kWmoMatShadowed`), applied to the outdoor pass whatever its mode.

## 5. Which groups are drawn

- **Camera outside** (0x7b3a10, per group of each visible instance): a group
  whose box is in the frustum (0x78fb20) and not occluded (0x78fdc0)
  is drawn at once if it has **MOGP 0x10000**; an **exterior** group (0x8)
  starts a portal walk (0x7ad350 -> 0x7ac060) from itself. Interior groups
  are only ever reached through portals.
- **Camera inside** (0x7ad1f0): the walk starts from each group that holds
  the camera (with a full-screen rectangle), and sets 0xcd8620 when one of
  them is interior. Groups with 0x10000 in the frustum are added as well.
- **The walk** (0x7ac060, recursion depth at most 0xd1bee4): skips groups
  with 0x10000 (already drawn), draws the group, then for each of its MOPR
  refs whose portal faces the viewer (signed plane distance times the ref's
  side >= 0) and whose projected rectangle (0x7a9090, cached per frame)
  overlaps the current screen rectangle, clips the rectangle to it and
  recurses into the group behind - **except into a group with 0x8 or
  0x10000**. From inside, a portal into a group with any of 0x50148
  (exterior, exterior-lit, 0x100, 0x10000, 0x40000) also opens the outdoor
  view through it (0x7a8f20). A portal whose two groups are both interior
  is marked 0x10 for that test.
- **The skybox** is chosen inside this walk: with the camera inside
  (0xcfbec0 set), any group reached that has 0x40000 puts the root's MOSB
  name into 0xcd861c (see §8).

**Wowee before:** portal culling existed (`getVisibleGroupsViaPortals`, a
breadth-first walk seeded from the camera's and the character's box-tested
groups, every portal tested against the full frustum, no rectangles) but was
off unless `WOWEE_WMO_CULL=1`, so every group was drawn. It also hid some
exterior "shell" groups within 196 yards by name and size (`isLOD`). The
client has no such rule. **After:** §10.

## 6. Doodads

- **Sets** (0x7bf740, per group's MODR list): a doodad is placed when its
  set (0x7aec30: the MODS range holding its MODD index) is 0, the
  instance's MODF set (+0x100), or one of three more set ids at +0x150.
  Wowee places set 0 plus the MODF set (terrain_manager), the same.
- **Creation** (0x7bef40): the MODD position, quaternion and scale; MODD
  flag 0x1 (byte 3) sets def flag 0x1000 (AcceptProjTex); the M2 starts
  its default sequence at offset 0.
- **Light** (0x7c1ad0 at creation, 0x7c1150 each frame): the MODD colour
  becomes two colours: `+0x88` the colour with its HSV value raised to
  0x70/255 when its brightest channel is below 0x70 (0x984f60..0x9851a0),
  `+0x84` the colour with every channel times `round(0x60 * 255 / max -
  0.5)`, plus 0xff, shifted down 8, when its brightest channel is above
  0x60. 0x7bf740 marks a doodad interior (def flag 2) unless its group is
  exterior or exterior-lit (0x48) or the def is already flag 4; an exterior
  doodad gets flag 4 and intensity +0x8c = 1.0.
- 0x7bef40 hangs the light on the model through callback 0x780cd0, which
  calls the def's vtable: for a WMO doodad def (vtable 0xa40318) that is
  **0x7c1150**, not the entity version 0x7c1730 the earlier pass read.
  0x7c1150, with 0x834d90 adding an ambient and 0x834dc0 a directional
  light to the model:
  - interior: ambient = `+0x84`, directional colour = `+0x88` (no
    intensity, no lean toward the sun), direction fixed at 0xaeedf0 =
    (-0.30822, -0.30822, -0.9), the same sense as the DayNight direction
    (light travelling down);
  - exterior: ambient = the Gx light's ambient (0xce04a8 +0x88), directional
    = its diffuse (+0x94) x `+0x8c` along its direction (+0x7c) - the
    outdoor light;
  - the fog is DayNight's first set, or the second (+0xa0) when def flag
    0x8000 is set (0x799310 sets it on groups reached from inside before any
    exterior group); interior doodads get model flag 8.
  0x7c1730 (entity base: units, game objects) is the variant with the
  intensity, the lean by MODD alpha under flag 0x1000 and the AreaTable
  ambient multiplier.

**Wowee before:** WMO doodads took the outdoor light; the MODD colour was not
read. **After:** §10.

## 7. MOLT

- Parsed (+0x148, 0x30 each). Each placed instance gets an array of
  `CMapLight*` sized to the MOLT count (0x7b5d00, 0x7b4f10), **zeroed and
  never filled**: 0x7b64f0 only moves the lights that exist, through each
  group's MOLR list. Nothing creates them.
- The `mapObjLightLOD` CVar (0x78e588, handler 0x78ded0 -> 0xd1c414) is
  written and never read.
- So **MOLT lights nothing in 3.3.5a**; the interiors' light is the baked
  MOCV. Wowee does not use MOLT either (its point lights are its own
  extra; `world-lighting.md` §9).

## 8. Liquids (MLIQ) and the skybox (MOSB)

### MLIQ
- Layout (0x7d7c30): xVerts, yVerts, xTiles, yTiles, the corner (3 floats),
  a uint16 **MOMT material** (+0x130), then xVerts x yVerts 8-byte
  vertices, then xTiles x yTiles tile bytes.
- **The sheet** (0x7c8c60): vertex (i, j) is `corner + (i, j) * 25/6`
  (0xa403a0) with **z = the vertex's float** at +4. Per-vertex heights.
- A tile is drawn unless `(flags & 0xf) == 0xf` (0x7c8bf0, 0x7c94b0).
- **The type** (0x7d82e0): with MOHD flag 0x4 the MOGP groupLiquid is a
  LiquidType id; without it 15 means none, else basic type +1. 0x7d7310
  maps basic types (< 21) to the WMO ones: water 13 (14 with group flag
  0x80000), ocean 14, magma 19, slime 20. A group with MLIQ and no type
  takes the first non-empty tile's `(flags & 0xf) + 1` (0x7c8d80).
- **At draw** (0x793d20): unknown id -> water (warning "WMO: Liquid type
  [%d] not found, defaulting to water!"). Water (`(t-1)&3 == 0`, t < 21) in
  a group that is not exterior (flags & 0x48 == 0, or instance flag 2), on
  a type without LiquidType flag 0x200, is drawn as **17, "WMO Water -
  Interior"**, lit as interior (`0x7d5120(!exterior)`, no light: MOHD
  colour 0x7a6d70 +0x1c, intensity 1). The material is LiquidType's
  (`0x8a1fa0`, `0x8a28f0`).

**Wowee before:** the sheet was flat, a yard under the corner's z; the type
was MLIQ's MOMT index; interior water and ocean were not drawn at all
(terrain_manager skipped them). **After:** per-vertex heights through the
instance transform; the stock type with the interior rule
(`stock_wmo_liquid`); interior water drawn (as 17). The hard-coded
Stormwind stair mask in `loadFromWMO` is left in place.

### MOSB
- `0x7ac060`: while the camera is inside (0xcfbec0), a group reached with
  MOGI/MOGP flag 0x40000 sets `0xcd861c` = the root's MOSB name.
- `0x79a870` (world render): when 0xcd861c is set, `0x7f31c0(0, name, 0,
  DayNight+0x9c)` puts that model in **sky model slot 0** and clears slot 1;
  the dome is still drawn as usual. 0x795400 clears 0xcd861c each frame.
- In the data: Stratholme (`STRATHOLMESKYBOX.MDX`, 61 of 83 groups), the
  Caverns of Time (`CAVERNSOFTIMESKY.MDX`), the Icecrown raid's Frostmourne
  and precipice (`ICECROWNRAIDSKY01.MDX`), Dire Maul (no flagged group),
  Auchindoun's Shadow Council wing (no flagged group).

**Wowee before:** MOSB was not read. **After:** the loader reads MOSB (the
name from "Environments\" on, since the Icecrown roots carry the artist's
whole path; a name that is not a model path, like Sunken Temple's, is
ignored); `WMORenderer::skyboxAt` returns it while the camera stands in a
0x40000 group (by the group's box - Wowee does not walk the portals, so a
flagged group seen through a portal from an unflagged one does not count -
since §10 the walk decides);
`LightingManager::setWmoSkybox` puts it first in the sky model list and
drops the second, as 0x7f31c0 does with slots 0 and 1.

## 9. Gaps and checks

### Gaps that remain
- **Specular**: the Specular and Metal vertex programs compute a specular
  term that no pixel program reads; nothing to do.
- **The Opaque substitution** (shader 0, blend 0, texture without alpha ->
  alpha = vertex alpha) is not made; it changes only the alpha written by an
  opaque batch, which nothing reads.
- **Env under the shadow**: the shadow programs scale the whole colour; the
  env term is added after the shadow in wmo.frag.
- **ConstantAlpha** (blend 11) draws as Alpha; no WMO uses it.
- **WMO liquid**: the per-vertex depth and colour attributes of the water
  shader are as world-lighting §7 says (unresolved), and the hard-coded
  Stormwind stair mask in `WaterRenderer::loadFromWMO` stays.
- **MFOG** (+0x158) and the second fog set (def/group flag 0x8000,
  0x7c1150 / 0x799310) are not used.
- The portal walk's own gaps are listed in §10.


### What to look at in game
- **Doorways** (§10): walking through a building's door, a city gate or a
  dungeon hall, nothing appears or vanishes as the camera crosses the
  portal; interiors are not drawn from outside unless a door shows them;
  with the camera deep inside (Ironforge, a dungeon) the terrain and the
  sky are not drawn at all until a gate comes into view.
- **Interior doodads** (§10) take their MODD colour as light: tavern
  furniture warm, crypt props dim, instead of the outdoor sun.
- **One-sided walls**: floors and walls seen from behind or below are gone
  (Silvermoon's platforms from under the city, the backs of facades) where
  they used to draw their undersides.
- **Silvermoon, Dalaran, Karazhan, the Nexus**: EnvMetal/Env materials
  carry their reflection map (gold trims, crystal, metal).
- **Icecrown Citadel**: the composite walls blend their two layers by the
  second MOCV, on the right UVs.
- **Arakkoa/Draenei huts**: trims on doorway (trans) batches draw solid in
  the room's light; elsewhere alpha-key cut-outs are tighter (224/255).
- **Interior water**: pools and canals inside groups (Undercity, Dalaran's
  sewers, dungeons) are drawn, at their own heights.
- **Caverns of Time, Stratholme, the Frostmourne room**: the MOSB sky model
  while standing in a flagged group.

### Measured
Same location, same build otherwise (before = the 09:51 binary with the old
WMO shaders, after = this work), WOWEE_FRAME_PROFILE 10-second averages on a
loaded machine (the world server was at 300% CPU and other builds ran):

| place | before fps | after fps |
|---|---|---|
| Cathedral of Light | 27.5-33.6 | 28.8-34.9 |
| Silvermoon | 44.8-46.2 | 44.3-45.7 |
| Dalaran (Violet Gate) | 11-18 (loading) | 22-24 |

No measurable cost; the vertex grew from 64 to 80 bytes, the pipelines
from 4 to 24 (built once), and back faces now cull. Screenshots of both
binaries are in the session scratchpad (`wmo-re/shots/before|after`).

## 10. The portal walk, as Wowee now does it

Follow-up to §5 and §6. The pure rules are in
`include/rendering/stock_wmo_portal.hpp` (tests:
`tests/test_stock_wmo_portal.cpp`); `WMORenderer::computeVisibility` runs
them once a frame on the main thread, before the WMO, M2 and terrain culls.

### Stock, in more detail
- **The camera's group** (0x795d40, each frame): a segment from the camera
  1760 yards (0xa3e6e8) straight down. 0x7a39f0 casts it against the world
  first; 0x7d59b0 then takes the nearest map-object floor *nearer than that
  hit* (the hit is passed in as the farthest distance), over the groups
  whose MOGI box the segment crosses and whose flags lack 0x410080. A floor
  of an exterior group (0x8) means "outside" (0xcd87a4 = 0). The group and
  (from 0x7af280) a second one go into the list 0x7ad1f0 seeds from; if
  either has 0x40140 the outdoor view opens over the whole screen at once.
- **0x79a870** then draws: camera outside - everything through 0x79a790
  with the full screen, each map object through 0x7b3a10 (its exterior
  groups start walks, its 0x10000 groups are drawn, interior groups only
  through portals). Camera inside - 0x7b3b20 -> 0x7ad1f0 walks from the
  camera's group; then, **only if a portal opened the outdoors**
  (0xadf59c >= 0), 0x79a790 draws terrain, sky and every map object
  (including this one, from its exterior groups) restricted to the union of
  the opened portals' rectangles. With nothing opened the sky is not drawn
  (the clear colour is DayNight +0xa0) and no terrain or other map object is.
- **The walk** (0x7ac060), with the details §5 left out:
  - the ref leading back to the group the walk came from is skipped;
  - a portal's rectangle (0x7a9090, cached per portal per frame by
    0xd1c424): the whole screen when the camera is within 0.01 of its plane
    (0x9f1968) and over its polygon (0x7a7210, flag 2); otherwise the
    polygon (at most 12 points) is moved to world space, clipped (0x7a72a0)
    and projected (0x7a85e0, w floored at 1e-4); fewer than three points
    left is an empty rectangle (flag 1). A portal touching an exterior group
    is also tested against the occluders (0x7ccfa0) - Wowee has none;
  - the clip of the current rectangle by the portal's keeps one maximum
    unclamped: 0x7ac32a..0x7ac363 compare the other axis' maximum twice. A
    result thinner than 0.001 (0x9e1134, 0x482870) is dropped. Wowee does
    the same (`clipStock`);
  - the frustum is narrowed to the clipped rectangle (0x7a6da0, 0x790e20)
    for the group behind, and every batch (0x7a7630, MOBA box) and every
    doodad of that group is culled against it;
  - from inside, a portal into a group with any of 0x50148 opens the outdoor
    view over its own (unclipped) rectangle (0x7a8f20 -> 0x795d00) and the
    walk goes on into that group only when it has neither 0x8 nor 0x10000;
  - the depth limit is 0xd1bee4, copied at start-up from 0xadfe40 = 10.
- **Doodads**: drawn by the group callback (0x799310 -> the group's doodad
  list), so a doodad is drawn only when its group is, culled to that
  group's rectangle; the 0x8000 fog flag marks groups reached from inside
  before any exterior group.
- **MOPT** is `{u16 first, u16 count, vec3 n, float d}`, plane n.p + d = 0;
  MOPR side +1 means the owning group is on the positive side (checked
  against the vertices near each portal: 795 of 810 refs in Stormwind and
  Karazhan agree, the rest being straddling groups). The portal is walked
  through when `(n.c + d) * side >= 0`.

### What Wowee does now
- The root loader reads MOPT's plane (it used to skip it and rebuild one
  from the polygon's winding) and MODR (each group's doodads).
- `computeVisibility` (`src/rendering/wmo_renderer.cpp`):
  - finds the camera's group as 0x795d40 does, with the terrain height under
    the camera standing in for 0x7a39f0 and Wowee's collision triangles
    (MOPY 0x08, or drawn and not detail) for the BSP. Not where the terrain
    is cut by a hole: the world ray meets no ground there, but the
    heightfield runs on across it, and under Goldshire's inn it stands 0.2
    yards over the floor. Taken as the ground, it called a camera over the
    inn's floor outside, the walk went from outside with the camera within
    the walls, and the inn was not drawn for a second or so after a
    teleport in, until the camera's ease brought it further in. Over a hole
    with no map object floor under it the camera is still outside (not the
    smallest interior box round it). An interior group's floor counts when
    the camera is within the building's interior (any of its interior
    groups' boxes, the floor within 12 yards below), not only within that
    group's own box: over a stairwell the camera is in the upper room's box
    and the floor met is the room below's. And with no map object floor
    under it at all, the camera is inside when the building encloses it
    (in an interior group's box, its surfaces overhead and on three of four
    sides): a house set on the terrain has none of its own under its ground
    floor, and the Culling of Stratholme's first house flipped between
    drawn and see-through as the camera moved a hand's width on its stairs;
  - seeds the walk from two groups, as stock does with 0x7af280's second
    one: the camera's floor's, and the smallest interior group whose box
    holds the camera (the room it is in, which over a stairwell is not the
    floor's);
  - walks the camera's instance from inside (`walkFromInside`), then every
    other instance in the frustum from outside (`walkFromOutside`) - through
    the outdoor rectangle when the camera is inside - and adds the camera's
    instance's own exterior seen through that rectangle;
  - reports "sealed" when the camera is in an interior and no portal opens
    the outdoors; the renderer then skips terrain, grass and the sky, and
    every other WMO instance is left out (0x79a870's branch);
  - the MOSB sky comes from the walk (any 0x40000 group it reaches with the
    camera inside), replacing the box test of §8.
- `cullForRecording` draws a group only when the walk reached it, tests its
  box against the planes of its rectangle, and hands those planes to the
  recording, which culls each draw range on its own box (the per-batch
  culling of 0x7a7630; merged batches keep a box per joined range).
- The `isLOD` hiding by name and size (§5) is gone: 0x10000 groups draw
  whenever they are in the frustum, the rest only when walked to.
- **Doodads**: terrain_manager and the instance-map loader tell WMORenderer
  which MODD index each placed M2 is (`attachDoodad`,
  `attachDoodadByUniqueId`); each gets a visibility slot, set every frame
  from its group's walk and a sphere test against the group's rectangle,
  and M2Renderer's cull skips a doodad whose slot is clear.
- **Doodad light**: a doodad of an interior group (flags & 0x48 == 0) is
  drawn with `stock_wmo_portal::doodadLight` - ambient `+0x84`, diffuse
  `+0x88` - from the fixed direction 0xaeedf0, with no sun shadow and no
  specular (M2InstanceGPU grew to 112 bytes for the two packed colours;
  flags bit 3). Exterior doodads keep the outdoor light, as stock does.
- `WOWEE_WMO_PORTALS=0` turns the walk off (every group drawn, as before);
  `WOWEE_WMO_PORTAL_DEBUG=1` logs per instance what the walk found;
  `WOWEE_FRAME_PROFILE=1` logs a summary every ten seconds.

### Not stock (yet)
- The ray under the camera starts half a yard above the eye: Wowee's
  third-person camera, pushed against a wall, can sit a hand's width under
  the floor it looks across (Karazhan's gate passage: 0.24 yards), and
  from there the ray finds the group below and calls the camera outside -
  which hid the passage overhead.
- When nothing at all lies under the camera (no terrain, no WMO floor -
  a third-person camera pushed through a floor), Wowee takes the smallest
  interior group box around it rather than calling the camera outside, so
  a dungeon does not vanish for that frame.
- A model with no portals and no exterior or 0x10000 group is drawn whole
  from outside (stock would draw nothing of it).
- Only terrain, grass and the sky are skipped when sealed; outdoor M2s
  (ADT doodads) and water are still culled by the camera frustum alone, and
  when the outdoors is open through a doorway the terrain is not narrowed
  to the portal rectangle (0x79a790 narrows everything).
- No occluders (0x7ccfa0), the second seed group is found by where the camera is, not read from 0x7af280, and the 0x8000
  second fog set is not used.
- Units and game objects are not culled by the walk.

### Measured
`WOWEE_FRAME_PROFILE=1`, one fresh client per place (the client's memory
grows across teleports and the machine was swapping on long tours), median
of the 10-second windows over a minute after a minute's settle, 1251x986
window. "before" is the build before this work; "walk off" is this build
with `WOWEE_WMO_PORTALS=0` (MODD light and the `isLOD` removal still in);
"walk on" is this build. GPU is the profiler's last-frame GPU time, CPU is
`renderWorld`, and the workers are the terrain / WMO / M2 recording threads.
Other agents' clients and builds were running throughout; take +-10%.

| place | before fps / GPU / CPU | walk off | walk on | workers t/w/m, before -> on |
|---|---|---|---|---|
| Stormwind, Trade District | 32.6 / 29.0 / 5.06 ms | 32.3 / 30.7 / 5.38 | **37.7 / 26.1 / 3.96** | 3.1/4.2/2.6 -> 3.2/1.3/1.8 |
| Ironforge, the Commons | 34.0 / 28.8 / 5.85 | 36.0 / 26.1 / 6.05 | **63.2 / 15.2 / 2.05** | 3.1/5.2/2.1 -> 0/0.6/1.5 |
| Undercity, Trade Quarter | 32.6 / 30.6 / 4.89 | 34.2 / 29.8 / 4.13 | **57.3 / 16.5 / 2.12** | 3.9/4.4/2.7 -> 0/0.3/1.5 |
| Dalaran, the Violet Gate | 20.8 / 46.6 / 4.83 | 25.3 / 37.5 / 5.20 | **62.9 / 15.6 / 3.34** | 3.9/3.2/4.0 -> 0/0.2/2.6 |
| Icecrown Citadel, Light's Hammer | 40.3 / 23.7 / 1.96 | 40.2 / 23.7 / 1.93 | **73.8 / 12.7 / 0.37** | 1.8/0.9/0.3 -> 0/0.1/0.2 |
| Karazhan, outside the gate | 39.9 / 22.4 / 1.83 | 53.7 / 17.9 / 1.73 | **64.5 / 15.4 / 1.27** | 0.8/0.6/1.4 -> 0.6/0.4/0.9 |

Ironforge, Undercity, Dalaran and ICC stand in sealed interiors: no
terrain, sky or other building is drawn (terrain worker 0), and only the
few groups the walk reaches are. In Stormwind the street groups are
exterior-lit interiors (0x2040) whose seed opens the outdoors, so terrain
stays and the gain is the shops and houses no longer drawn from the street
(213 of 1161 groups, 406 of 19,531 city doodads). The Karazhan camera
faced a different way in each run (the camera's yaw after a teleport is
not deterministic), so that row is indicative only.

Same-session checks (`WOWEE_WMO_PORTALS_ALTERNATE=2`, a shot every half
second, so consecutive shots straddle the switch with the same camera): at
Stormwind, Ironforge, Undercity, ICC and Karazhan the walk-on and walk-off
frames differ by 0.01-0.04% of pixels at 10% fuzz, the same as two
walk-on frames (NPCs, fog). Stepping through the Gates of Ironforge
(15 `.go` steps across the exterior/interior portal, a shot each) showed
whole geometry on both sides; the camera's direction differed from the old
binary's run, so that one is not a pixel comparison. The
Karazhan case found the camera-under-floor problem noted above.
