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

**Wowee:** portal culling exists (`getVisibleGroupsViaPortals`) but is **off
unless `WOWEE_WMO_CULL=1`**, so every group is drawn; with no depth-hidden
geometry in view that only costs time. Wowee also hides some exterior
"shell" groups within 196 yards by name and size (`isLOD`: MOGP 0x10000
groups under 5000 vertices in big WMOs, "facade" and "city.." groups,
Stormwind's 0x80 interior cathedral shell). **The client has no such rule**:
0x10000 means "draw whenever in the frustum". Left as it is (it hides
duplicate shells Wowee would otherwise draw because it does not walk the
portals); a gap.

## 6. Doodads

- **Sets** (0x7bf740, per group's MODR list): a doodad is placed when its
  set (0x7aec30: the MODS range holding its MODD index) is 0, the
  instance's MODF set (+0x100), or one of three more set ids at +0x150.
  Wowee places set 0 plus the MODF set (terrain_manager), the same.
- **Creation** (0x7bef40): the MODD position, quaternion and scale; MODD
  flag 0x1 (byte 3) sets def flag 0x1000 (AcceptProjTex); the M2 starts
  its default sequence at offset 0.
- **Light** (0x7c1ad0 at creation, 0x7c1730 each frame): the MODD colour
  becomes two colours: `+0x88` the colour raised in value to at least
  0x70/255 (HSV, 0x984f60..0x9851a0), `+0x84` the colour scaled down so its
  largest channel is at most 0x60/255. A doodad in an interior group
  (instance group flags & 0x48 == 0, def flag 2) is lit with **ambient =
  +0x84** and **diffuse = +0x88 x +0x8c** from a fixed direction
  (0xaeedf0..f8); with def flag 0x1000 the direction leans toward the
  DayNight light by the MODD alpha. A doodad of an exterior group (flag 4)
  takes the DayNight diffuse/ambient (0xce04a8 +0x7c..+0x9c). AreaTable
  flag 0x8000 brightens the ambient by 0xadeebc, clamped.

**Wowee:** WMO doodads take the outdoor light (or the renderer's indoor
ambient for units); MODD colour is not read. **Not changed here** - it is
M2 lighting (m2_renderer, being edited by others). A gap.

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
flagged group seen through a portal from an unflagged one does not count);
`LightingManager::setWmoSkybox` puts it first in the sky model list and
drops the second, as 0x7f31c0 does with slots 0 and 1.

## 9. Gaps and checks

### Gaps that remain
- **Portal culling** is off by default (`WOWEE_WMO_CULL`), and the walk that
  exists does not follow 0x7ac060 (no screen-rectangle clipping, both camera
  and character seed it). Every group is drawn; the 196-yard `isLOD` hiding
  of exterior shells and Stormwind's cathedral shell is Wowee's, not the
  client's.
- **Doodad light** from MODD colour (§6) is not applied - M2 territory.
- **Specular**: the Specular and Metal vertex programs compute a specular
  term that no pixel program reads; nothing to do.
- **The Opaque substitution** (shader 0, blend 0, texture without alpha ->
  alpha = vertex alpha) is not made; it changes only the alpha written by an
  opaque batch, which nothing reads.
- **Env under the shadow**: the shadow programs scale the whole colour; the
  env term is added after the shadow in wmo.frag.
- **Per-batch frustum culling** (0x7a7630, MOBA boxes) is not done; Wowee
  culls whole groups.
- **ConstantAlpha** (blend 11) draws as Alpha; no WMO uses it.
- **MOSB** is chosen by the camera's own group (box test), not by every
  group the portal walk reaches.
- **WMO liquid**: the per-vertex depth and colour attributes of the water
  shader are as world-lighting §7 says (unresolved), and the hard-coded
  Stormwind stair mask in `WaterRenderer::loadFromWMO` stays.
- **MFOG** (+0x158) and the second fog set (group flag 0x8000, 0x7c1730)
  are not used.


### What to look at in game
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
