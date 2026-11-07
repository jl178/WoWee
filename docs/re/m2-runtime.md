# M2 model runtime (stock 3.3.5a build 12340)

How the stock client's CM2Model / CM2Shared / CM2Scene play and draw an M2.
This covers sequence timing, track evaluation, render state and draw order.
It is reverse engineered from `Wow.exe` with Ghidra; addresses are virtual
addresses in that binary. The M2 code sits in roughly 0x81a000-0x840000,
covering the `.\M2Scene.cpp`, `.\M2Cache.cpp`, `.\M2Model.cpp`, `.\M2Light.cpp`
and `.\M2Shared.cpp` units. The last section compares this with Wowee.

Offsets below are into the in-memory CM2Model (`model+0x..`) and the
per-bone sequence state (`bs+0x..`: 0xAC bytes per bone, array at
`model+0x94`). The scene clock in ms is `*(model+0x28)+0xC`, called `now`.
`H` is the MD20 header the shared model keeps at `*(model+0x2C)+0x150`.

## 1. Sequences and time

### Starting a sequence
- `CM2Model::SetBoneSequence` (0x832AB0) takes (boneKey, animId, variation,
  startOffsetMs, speed, blend, primary). It resolves the id through the
  animation lookup hash (quadratic probing, 0x8260C0). Variation -1 means
  roll one: 0x826E60 walks the variationNext chain (`seq+0x3C`), subtracting
  each `frequency` from `rand()` (0..0x7FFF), and remembers that the roll
  was random (`bs+0x4B`). A sequence without flag 0x20 (keys in an .anim
  file) is loaded first (0x831C30) and then starts the same way.
- The state is filled by 0x826B00:
  - `loops = replayMin + rand()*(replayMax-replayMin)>>15`, at least 1;
  - `start = now - round(startOffset / |speed|)`;
  - `end = start + duration*loops/|speed|`;
  - a negative speed (play flags 1 or 3) starts at offset `duration*loops`.
- **Start phase:** every caller found passes `startOffset = 0`. That covers:
  - ADT doodads, CMapDoodadDef from MDDF (0x7BECD0);
  - WMO doodads from MODD (0x7BEF40);
  - the model's own default when it finishes loading (0x834540: Stand if the
    model has it, else the first sequence's id, variation rolled);
  - units, game objects and UI models (0x7FB2D0, 0x95F5E0 and about 40
    others listed in m2-re/callers2.txt);
  - spell-kit models, which get the same default sequence.

  **Nothing randomises the phase.** Two copies of a model differ only by
  when they were created and by the variation they rolled. The only non-zero
  offsets are the time-scaled one in 0x7ECF20 and SetSequenceTime
  (0x826ED0). The callers of 0x826ED0 that freeze a model on its last frame
  (a corpse) are 0x5249C0 and 0x744870.
- **Global sequences:** `model+0x74 = now` when the model is created
  (0x834810). Each frame, Animate sets
  `gsTime[i] = (now - model+0x74) % gsDuration[i]`, as unsigned ms
  (0x82F0F0). The phase is relative to the model's creation, not to the
  world clock.

### Per-frame time: CM2Model::Animate (0x82F0F0)
- Runs at most once per scene frame (`model+0x3C` holds the frame number).
- Per bone, the primary time is `t = round((now - start) * speed) + offset`.
  - Sequence **flag 0x01 not set:** `t % duration`. It loops for ever.
  - **Flag 0x01 set:** while `end` is in the future it loops as above. Once
    `end` has passed, it holds at `clamp(round((end-start)*speed)+offset, 0,
    duration)`. So it plays `loops` laps and then holds the last frame.
- Flag 0x01 is the "non-looping" bit; for the secondary state the copy is
  0x80. Nearly every emote and death sequence carries it (HumanMale Death
  is 0xA3), as do 1028 of the 1565 default sequences under `spells\`.
  0x20 means the keys are in the .m2, 0x40 marks an alias.
- **Pause:** when `model+0x64` is non-zero, every start and end time is
  shifted by `now - model+0x64`. This freezes the model where it was (a
  corpse, through 0x5249C0 and 0x744870).
- **Speed:** a per-bone float (`bs+0x54`), set by SetSpeed (0x827000),
  which re-bases start so the current frame is kept. Distance and LOD
  never scale it.
- **Bones** with sequence 0xFFFF copy their parent's state; bone 0 keeps
  its own.

### Ends, variations, replay (0x832260, 0x831FC0)
- When the model has callbacks or at least two sequences (flag 0x400000),
  0x832260 fires the M2 events (0x830FB0) and the end of a sequence.
  - Looping sequence: the end fires **every lap**.
  - Flag-0x01 sequence: the end fires once, when `end` passes.
- The default handler (0x831FC0) runs after the owner's callback, if any.
  If the callback did not change the sequence, the sequence loops, its
  variation was rolled at random and it has variations, it **rolls a new
  variation at every lap boundary**. The new variation starts with the
  overshoot as its offset, at the same speed, with blending on.
- `replayMin/Max` only sets `loops`, so it matters only to flag-0x01
  sequences. A unit's callback (CGUnit) may use it differently; that was
  not traced.

### Blending between sequences (0x826C40, 0x82F0F0)
- When a new primary sequence starts with blend on, the old state is copied
  to the secondary (`bs+0x64`) and the fade runs over the new sequence's
  `blendTime`.
  - `blendTime == 0` is a **cut**: the fade ends now.
  - There is no cap on the length.
  - If a fade is already running and the outgoing weight is above 0.5, the
    old secondary and its timer are kept.
- The outgoing weight is `smoothstep(x) = (3-2x)x^2`, where x is the
  fraction of the fade left. Removing a bone's sequence (0x832840) fades
  over a fixed 150 ms.
- The secondary keeps advancing on its own clock (looping, or held if it
  has flag 0x01).
- Every track evaluator blends: rotations use slerp along the shorter arc
  (0x982460), everything else a lerp. This includes colours, texture
  weights, UVs, lights and ribbons, but not global-sequence tracks.

## 2. Track evaluation

- **Key search** (0x8284D0) works on a track `{interp u16, globalSeq u16,
  timestamp arrays, value arrays}`.
  - **Which array:** a global-sequence track reads array 0 at `gsTime`.
    Otherwise it reads the playing sequence's array, or **array 0 when the
    track has fewer arrays** than the sequence index.
  - **Hint:** the search starts from a cached key index per track per
    instance.
  - **Ends:** fewer than 2 keys gives key 0. At or past the last key, the
    last key holds. Before key 0 the fraction is computed from unsigned
    time and extrapolates; no multi-key track in the 3.3.5 data starts
    after 0, so in practice the first key holds.
- **Empty values** for the sequence's array: the default for the property
  (alpha 1, colour 1, translation 0, scale 1, identity rotation).
- **Static tracks:** Animate skips a track whose only array has at most
  `model+0x90` keys once it has been evaluated, so a static track is
  evaluated once.
- **Interpolation,** by evaluator:
  - Plain tracks are bone translation and scale, colour RGB, light, UV and
    particle (0x82B0A0 vec3, 0x82B340 float, 0x82AF40 fixed16).
    Interpolation 0 holds the lower key. **1, 2 and 3 are all linear**:
    these tracks hold plain values with no tangents, and there is no
    Hermite or Bezier case.
  - fixed16 values are `s16 / 32767`, not clamped.
  - Rotations: compressed (0x828680, `u16/32767 - 1`) or float (0x82AD50,
    UV rotation). Type 0 holds; otherwise **lerp then normalise, with no
    sign flip** (0x982630). Blending uses slerp.
  - uint8 tracks (visibility; 0x82B270) always take the lower key, never
    interpolated.
  - **Camera spline keys** (`{value, inTan, outTan}`; 0x82B460 vec3,
    0x82B8A0 float) are the only Hermite/Bezier tracks, and only cameras use
    them (position and target are vec3, roll is float).
    - Type 2 is **Bezier**: control points are the lower key's outTan and
      the upper key's inTan.
    - Type 3 is **Hermite**, with those tangents taken unscaled.
- In the asset set, every non-camera track is type 0 or 1. Type 2 appears
  only on cameras (82 tracks); type 3 is never used.

| Property | Stock evaluator | Default | Sequence state used |
|---|---|---|---|
| bone T / R / S | 0x82B0A0 / 0x828680 / 0x82B0A0 | 0 / id / 1 | the bone's own |
| colour RGB / alpha (`H+0x48`, 0x28 stride) | 0x82B0A0 / 0x82AF40 | 1 / 1 | bone 0 |
| texture weight (`H+0x58`) | 0x82AF40 | 1 | bone 0 |
| UV translation / rotation / scale (`H+0x60`, 0x82D6F0) | 0x82B0A0 / 0x82AD50 / 0x82B0A0 | 0 / id / 1 | bone 0 |
| attachment visibility (`H+0xF0`, 0x82E550) | 0x82B270 | 1 | the attachment's bone |
| light colours, intensities, attenuation, visibility (`H+0x108`) | 0x82B0A0 / 0x82B340 / 0x82B270 | 0 | the light's bone |
| camera position / target / roll (`H+0x110`) | 0x82B460 / 0x82B460 / 0x82B8A0 | 0 | bone 0 |
| ribbon colour, alpha, above, below, texture slot, visibility (`H+0x120`) | 0x82B0A0, 0x82AF40, 0x82B340, 0x82B340, 0x82BB50, 0x82B270 | 1, 1, 0, 0, 0, 1 | the ribbon's bone |
| particle emitter tracks (`H+0x128`, 0x82D2F0) | 0x82B340 ×10, 0x82B270 | 0 | the emitter's bone |

- **Texture matrix** (0x82D6F0): `M = T(t) · [T(-c) S T(c)] · [T(-c) R T(c)]`,
  with `c = (0.5, 0.5, 0)` and row vectors. A UV is translated first, then
  scaled about the centre, then rotated about it. Each step is applied
  only if its track has arrays.
- **An attachment that is not visible** does not animate its child model
  (0x82E550).

## 3. Render state

- **Draw elements** (0x821A20). For each skin batch whose geoset is on and
  whose shader is not 0x8000:
  - `alpha = modelAlpha(model+0x19C) × colour.alpha[colorIndex] (if
    colorIndex < nColors) × textureWeight[transLookup[combo]] (if
    textureCount != 0)`.
  - **Not drawn if alpha < 1e-4.**
  - The batch is **transparent if its base layer's blend mode is above 1
    or alpha < 0.99999.**
- **Blend table** at 0xA453B0, indexed `[row*7 + m2Blend]`:

  | M2 blend | row 0 | rows 1, 2 |
  |---|---|---|
  | 0 | Opaque | Alpha |
  | 1 | AlphaKey | Alpha |
  | 2 | Alpha | Alpha |
  | 3 | NoAlphaAdd (One, One) | NoAlphaAdd |
  | 4 | Add (SrcAlpha, One) | Add |
  | 5 | Mod (DstColor, Zero) | Mod |
  | 6 | Mod2x (DstColor, SrcColor) | Mod2x |

  The z-fill pass (state flag 1) uses only Opaque or AlphaKey and writes no
  colour.
- **State setup** (0x81FE90):
  - **Alpha test reference:** 0 for blend 0, **`alpha × 224/255`** for blend
    1, and **1/255** for all other modes. The alpha-key cutoff therefore
    drops as the batch fades.
  - Render flag 0x04 → two-sided (no culling).
  - Flag 0x08 → no depth test.
  - Flag 0x10 → no depth write.
  - Flags 0x01 (unlit) and 0x02 (unfogged) work through the material.
  - The three are GxRs states 0x11, 0x0D and 0x0F, each set from
    `~(materialFlags >> n) & 1` whenever the material changes (0x81FE90).
    **Depth write follows the flag alone, whatever the blend:** an
    alpha-blended or additive layer without 0x10 writes depth.
- **Material colour:**
  - Diffuse is `modelDiffuse × colour.rgb` (the animated M2Color RGB).
    Its alpha is the batch alpha.
  - Unlit adds the diffuse to the emissive and zeroes the diffuse.
  - Mod and Mod2x use diffuse 0 and emissive 1 or 0.5. Fog does not tint
    them.
- **Transparent order.** CM2Scene's element builder (0x821A20) makes one
  0x44-byte element per drawn batch, ribbon, particle emitter and model
  callback. Three lists are then sorted with 0x83DCF0: the opaque list
  (`scene+0x54`, comparator 0x81EEA0, then 0x81EA90 by state) and two
  transparent lists (`scene+0x64`, `+0x74`, comparator **0x81EF30**). The
  element:

  | offset | field |
  |---|---|
  | +0x00 | type: 0 batch, 1 batch with flag 0x4 and a projected texture, 2 instanced opaque batch, 3 ribbon, 4 particle emitter, 5 model callback (`model+0x1BC`) |
  | +0x04 | model |
  | +0x08 | flags: 0x1 z-fill copy, 0x2 straddles the scene plane, 0x4 from a runtime skin (`model+0x2D0`) |
  | +0x0C | alpha |
  | +0x10 | **primary distance** (squared) |
  | +0x14 | **own distance** (squared) |
  | +0x18 | batch index / ribbon index / emitter |
  | +0x24 | **priority**: the batch's priorityPlane (s8), the ribbon's `+0xAC` (s16), the emitter's `+0x18` (file field 0x2E, the int16 the format calls textureTileRotation) |
  | +0x28, +0x2C, +0x30 | skin batch, skin section, material |

  - **Distances** are squared view-space lengths. `model+0x88` is the
    model's: `|model-view translation|²` (`model+0x124`; 0x82E140,
    0x82F0F0), or **its parent's** when attached (`model+0x48`), so a
    weapon or helm sorts with its wearer.
    - A blended batch's own distance is its skin section's sortCenter
      (`+0x20`) through bone `centerBoneIndex` (`+0x12`). Batch flag 0x1
      pulls it `sortRadius` (`+0x2C`) × the bone's scale towards the
      camera, else flag 0x2 pushes it away; with either, a point that ends
      up behind the camera (view z < 0) is negative. Without them it is
      `|p|²`.
    - Primary = own distance, except: MD20 global flag **0x10** (header
      `+0x10`) sorts the whole model by `model+0x88` (own distance kept,
      without the radius push); and a z-fill model (below) keeps
      `model+0x88` for its depth-writing layers.
    - An opaque element has both at `model+0x88`.
    - Particles (0x821930): primary `model+0x88`, own distance the
      emitter's position through its bone; child emitters (`em+0x6C`
      count, `+0x70` list) get the same keys.
    - Ribbons and callbacks: both at `model+0x88`.
  - **Comparator 0x81EF30:** primary far first; then z-fill copies (flag
    0x1) first; priority **ascending**; own distance far first; (with
    scene flag 0x4000, texture then shader grouping across models); model
    pointer; type ascending (batches, ribbons, particles); materialLayer
    ascending for batches; then the state compare 0x81EEA0.
  - **Particle placement:** an emitter with blend 0 or 1 at alpha ≥
    0.99999 is an **opaque** element. Others go to the first transparent
    list, or the second when the model is wholly on the far side of the
    scene plane or the emitter has flag 0x40000 (tails). The two lists are
    drawn by "Model2 Pass 1" and "Pass 2" (0x823CB0). The world frame
    (0x4F8EA0) draws Pass 0 (opaque), then, with the camera above water
    (`0xCD8794 == 0`, 0x780620): **Pass 2, the liquids (0x790A80), the
    weather (0x77F030), 0x77F980, then Pass 1**; under water the order is
    0x77F980, Pass 1, the weather, the liquids, Pass 2. So the transparent
    elements on the camera's side of the water plane are drawn after the
    water and the weather, and their depth hides neither. With scene flag 0x80 (M2BatchParticles), 0x81F9E0 then
    re-sorts runs of consecutive additive elements by texture (0x81F0E0).
  - **Z-fill** (CVar `objectFadeZFill`, 0xA3F3E4, default 0): a model with
    `model+0x10 & 0x40` and scene flag 1 gets, for each blended layer that
    writes depth, a copy with flag 0x1 drawn first (depth only).
  - **Ribbons** are type-3 elements, transparent, with alpha
    `model+0x198 × ribbon alpha`, priority their `ribbon+0xAC`.

### Shader selection (CM2Shared::Initialize 0x837A40)

There is no table of effects for M2 batches in the data:
`shaders\effects\model2.wfx` holds only the four `Projected_*` effects.
The client builds an effect per batch at load from a vertex shader name and
a pixel shader name (strings at 0xA45664..0xA458A8) and caches it by the
concatenated name (0x876530 find, 0x876BE0 create). Three steps, all on the
skin's batches (`skin+0x28`, 0x18 bytes each: +2 shader id, +0xA material,
+0xC materialLayer, +0xE textureCount, +0x10 texture combo, +0x12 texture
coordinate combo, +0x14 weight combo, +0x16 transform combo):

1. **Runtime shader id** (0x836980), for every batch whose id lacks 0x8000.
   The id is rewritten: bits 4-6 the first stage's combiner op and bit 7 its
   env flag, bits 0-2 and 3 the same for the second stage, 0x4000 "the last
   stage reads UV set 1". `tc[i]` below is the texture coordinate combo
   (header 0x88, `H+0x8C`) at the batch's index + i, read unsigned, so the
   env map's -1 is "above 2".
   - **Model flag 0x08 clear** (no combiner combos): op = 0 (Opaque) for
     blend 0, 1 (Mod) otherwise; env if `tc[0] > 2`; 0x4000 if `tc[0] == 1`.
     One stage.
   - **Flag 0x08 set**: for each of the batch's textures,
     op = `textureCombinerCombos[fileShaderId + i]` (header 0x130, `H+0x134`),
     forced to 0 for stage 0 of a blend-0 material; env if `tc[i] > 2`.
2. **Layer folding** (0x837680), only when a batch has materialLayer != 0.
   The textures and transforms are first resolved per batch (0x835E90;
   lookup tables rebuilt by 0x837250/0x8374A0). Walking the batches in skin
   order, a layer-0 batch is the base and resets two state machines (a
   blend-0 base with textures also has its stage-0 op cleared):
   - **A**: base is blend 0, two textures, stage-2 op Mod2x (4) or Mod2xNA
     (6), `tc = (0, env)`. If the next layer is blend 1 or 2, one texture,
     the same unlit bit (material flags & 1), the base's first texture and
     the same texture weight, it is hidden (id 0x8000) and the base becomes
     **0x8001**.
   - **B**: base is blend 0, one texture, `tc = 0`. If the next layer is
     blend 4 or 6, one texture, `tc > 2` (env) and the same weight, it is
     hidden and the base takes its texture and transform as stage 2: id
     **0x8002** for blend 4, **0x000E** (Opaque_Mod2xNA, stage 2 env) for
     blend 6, textureCount 2. If the layer after that is blend 1 or 2, one
     texture, the same unlit bit, the base's texture and weight, it is hidden
     too and the base becomes **0x8003** (from 0x8002) or **0x8001**.
   - A batch on the same material as the one before it skips the machines
     and afterwards copies that batch's id, count, textures and transforms.
3. **Effect** (0x836C90 -> 0x836600). 0x8000 draws nothing; 0x8001/2/3 are
   `Diffuse_T1_Env` with `Combiners_Opaque_Mod2xNA_Alpha`, `_AddAlpha`,
   `_AddAlpha_Alpha`. Otherwise, with s0/s1 the two stage ops:

   | stages | vertex shader | pixel shader by op |
   |---|---|---|
   | 1 | Env if bit 7, else T1 if `tc[0] == 0`, else T2 | 0 Opaque, 2 Decal, 3 Add, 4 Mod2x, 5 Fade, else Mod |
   | 2 | T1_T2, T1_Env, Env_T2, Env_Env by the two env bits | s0 0: s1 0 Opaque_Opaque, 3 Opaque_Add, 4 Opaque_Mod2x, 6 Opaque_Mod2xNA, 7 Opaque_AddNA, else Opaque_Mod |
   | | | s0 1: the same with Mod_ (Mod_Opaque ... Mod_AddNA, else Mod_Mod) |
   | | | s0 3, s1 1: Add_Mod. s0 4, s1 1: **Mod_Mod2x**; s0 4, s1 4: Mod2x_Mod2x |
   | | | anything else: no shader; the effect is rebuilt with id **0x11** (Mod / Mod_Mod) |

   A two-texture effect also sets the fixed-function stage ops from tables
   0xAF5A08/0xAF59E8 (op -> colour op, alpha op), used only without shaders.
4. **The draw** (0x81F450) binds texture `lookup[combo + i]` on stage i,
   with its M2Texture wrap flags, and the texture matrix
   `transformLookup[transformCombo + i]` into slot i (identity past the
   transform count). An env stage of a normal id gets the env texgen
   (0x873550) instead. A one-texture batch with 0x4000 puts its matrix in
   slot 1, the one Diffuse_T2 reads; without vertex shaders (`0xD43020 == 0`)
   the 0x800x ids bind only their first texture.

**The shaders** (`shaders\vertex\arbvp1\diffuse_*.bls`, first variant):
- T1: UV set 0 through matrix slot 0 (c6/c7). T2: UV set 1 through slot 1
  (c8/c9). T1_T2: both. **Env_T2 reads UV set 0 through slot 0 for its second
  stage**, despite the name.
- Env: `p` the view-space position, `n` the view-space normal, both
  normalised; `r = -reflect(p, n)`; `uv = r.xy / |r + (0,0,1)| * 0.5 + 0.5`.
  The view space looks down +z (the map's centre is a surface facing the
  camera), so in a -z view space the map is OpenGL's sphere map turned half
  round: `uv = 0.5 - r.xy / (2 |r + (0,0,1)|)` with `r = reflect(p, n)`.
- The pixel combiners (`shaders\pixel\arbfp1\combiners_*.bls`), with `c`
  the vertex colour (lit material colour; alpha the batch alpha), all of the
  form `rgb = M * c.rgb + A`, `a = Am * c.a + Aa`:

  | combiner | rgb | alpha |
  |---|---|---|
  | Opaque | t0 c | c.a |
  | Mod | t0 c | t0.a c.a |
  | Mod2x | 2 t0 c | 2 t0.a c.a |
  | Add | c + t0 | c.a + t0.a |
  | Decal / Fade | lerp(t0, c, c.a) / lerp(c, t0, c.a) | c.a |
  | Opaque_Opaque / _Mod | t0 t1 c | c.a / t1.a c.a |
  | Opaque_Mod2x / _Mod2xNA | 2 t0 t1 c | 2 t1.a c.a / c.a |
  | Opaque_Add / _AddNA | t0 c + t1 | c.a + t1.a / c.a |
  | Mod_Opaque / Mod_Mod | t0 t1 c | t0.a c.a / t0.a t1.a c.a |
  | Mod_Add / Mod_AddNA | t0 c + t1 | t0.a c.a + t1.a / t0.a c.a |
  | Mod_Mod2x / _Mod2xNA | 2 t0 t1 c | 2 t0.a t1.a c.a / t0.a c.a |
  | Add_Mod | (t0 + c) t1 | (t0.a + c.a) t1.a |
  | Mod2x_Mod2x | 4 t0 t1 c | 4 t0.a t1.a c.a |
  | Opaque_Mod2xNA_Alpha | lerp(2 t0 t1, t0, t0.a) c | c.a |
  | Opaque_AddAlpha | t0 c + t1 t1.a | c.a |
  | Opaque_AddAlpha_Alpha | (t0 + t1 t1.a (1 - t0.a)) c | c.a |

  The second stage of the env combiners is never lit: Opaque_AddNA adds the
  water elemental's reflection on top of the lit body.
- **Mod and Mod2x batches** get diffuse 0 and emissive 1 or 0.5 (0x81FE90),
  so `c = (1,1,1,a)` or `(0.5,0.5,0.5,a)`; Combiners_Mod then gives
  `src = t0 / 2` for a Mod2x batch and the (DstColor, SrcColor) blend
  `2 src dst = t0 dst`. Their fog colour is white / grey (table 0xA45390), so
  a fogged texel leaves the framebuffer as it was.
- **Alpha test** for every blend above 1 is 1/255 against the combiner's
  alpha.

**In the 3.3.5 data** (22,112 models, skin 00; the same counts from a Python
transcription and from `m2fx::resolveBatchEffects`): 21,883 one-texture Mod
and 20,597 Opaque batches; 7,248 layers hidden by folding; 4,521
Opaque_Mod2xNA_Alpha (armour and creature shine: the northrend fire giant,
warhorses, the gryphon pet); 898 Opaque_Mod2xNA and 646 Opaque_AddAlpha; 981
one-texture Env Mod; 154 Opaque_AddNA and 11 Mod_AddNA (water elementals,
the Ulduar train glass); 1,482 T1_T2 Mod_Mod2x and 767 Mod_Mod, almost all
on sky domes; 64 one-texture T2 (the frost lord). No batch in the set falls
back to 0x11.

## 4. Per instance, per model, and what is skipped

- **Per instance:** all sequence state, the cached key indices, the global
  sequence times, the evaluated track values (colours `model+0xA0`,
  weights `model+0xA8`, UV matrices `model+0xB0`), the bone matrices, the
  model alpha `+0x178/0x19C`, and the particle and ribbon emitters.
  **Shared per model:** the keys, the skin, the materials and the lookups.
- **Animating:** the scene animates only models on its animate list
  (0x823F10 SetAnimating) or ones asked directly (0x830DC0, for example
  through an attachment or an event).
  - A doodad that is drawn is put on the list (0x791CB0).
  - A doodad that is culled stays on it only within **10 yd** of the camera
    or if its flag 0x400 is set (0x7987A0, 0x799980).
  - Particles and ribbons are stepped only for animated models (0x828A00).
- **Off-screen:** time is absolute (`now - start`), so a model taken off
  the list jumps to its current frame when it returns, unless it was
  paused through `model+0x64`. No distance or LOD changes the speed or
  skips frames.
- **Doodad fade** (CMapDoodadDef, 0x791CB0): the alpha `+0x178` is a
  distance fade per doodad size class. Below 0.99999 every batch goes to
  the blended pass, with opaque and alpha-key switched to Alpha blending.
  - **Size class** (0x7BDB10, `doodad+0x24`): the largest extent of the
    doodad's world-space bounding box against 0xADF350 = {1, 4, 15, 100}
    yd: the first class it does not exceed, else 4. With scene flag 0x8000
    clear, classes 0-2 also lose `model+0x10 & 0x40`.
  - **Tables** (0x78F570, rebuilt when the CVar changes):

    | class | 0 | 1 | 2 | 3 | 4 |
    |---|---|---|---|---|---|
    | largest extent ≤ (0xADF350) | 1 | 4 | 15 | 100 | any |
    | end, yd (0xADF364 → 0xADF3A0) | 30 | 100 × d | 200 × d | 750 × d | 1250 |
    | fade length (0xADF38C) | 5 | 10 | 15 | 20 | 50 |

    `d` is the CVar **environmentDetail** (0xA3F3BC, default "1.0",
    clamped to 0.5..1.5 by its handler 0x78DC60). 0xADF3B4 holds end²,
    0xADF3C8 the fade start (end − length), 0xADF3DC its square.
    Classes 0 and 4 are never scaled. Farclip does not enter it.
  - **Rule:** with the world flag `0xCD774C & 0x4000` and without doodad
    flag 0x800, `d²` from the camera to the doodad's bounding-sphere centre
    (`+0x38`) past end² is not drawn; past start² the alpha is
    `1 − (d − start) / length`, snapped to 1 above 0.99 and not drawn below
    0.01. 0x78FB60 is the same band test, used to pick a class for a
    distance.
  - Only map (MDDF) doodads; WMO doodads and game objects are drawn by
    their own paths.

## 5. Wowee: gap table

"Fixed" means fixed in this change. Files:
- `include/rendering/m2_track_sampler.hpp`
- `include/rendering/m2_particle_step.hpp`
- `include/rendering/animation/sequence_variation.hpp`
- `src/rendering/m2_renderer{.cpp,_render.cpp,_instance.cpp,_internal.h}`
- `src/rendering/character_renderer.cpp`
- `tests/test_m2_track_stock.cpp`

| # | Item | Stock | Wowee before | Now |
|---|---|---|---|---|
| 1 | Instance clock at spawn | starts at creation | `clockStampMs` never set: the first `advanceClock` added the whole session's scene time to animTime and gsTime. Frost Armor was at 3000 ms 64 ms after the cast. | **Fixed:** stamped at creation; setters bring the clock up to date before resetting |
| 2 | Start phase | 0 for doodads, objects, units and effects | random for doodads and particle-only models (0 for spell effects since tonight) | **Fixed:** 0 for all |
| 3 | Starting sequence | Stand (else the first id), variation rolled by frequency | sequence index 0 | **Fixed:** `anim::defaultSequenceIndex` |
| 4 | Global sequence phase | `(now - creation) % dur` | per-instance float from creation (but wrong because of #1) | fixed with #1; still a float, so it loses ms precision after about 4.6 h |
| 5 | Track array for a sequence past the track's arrays | array 0 | default value (texture weights, colour alpha, ribbon colours read 1) | **Fixed** in the sampler and in the particle const path |
| 6 | Interpolation 2 and 3 on plain tracks | linear | linear | same; comment corrected. Camera spline evaluation added (`splineSegment`), not wired: Wowee loads no camera tangents |
| 7 | Rotation key interpolation | lerp + normalise, no flip | slerp along the shorter arc | **Fixed** (`nlerpKeys`) |
| 8 | Blend time 0 | cut | 150 ms | **Fixed** in M2Renderer and CharacterRenderer |
| 9 | Blend cap | none | 1000 ms | **Fixed** |
| 10 | Blend curve | smoothstep | linear | **Fixed** |
| 11 | Re-blend while the old fade is above half | keep the old fade | restart | **Fixed** (M2Renderer) |
| 12 | Blend of non-bone tracks | all tracks | bones only | open |
| 13 | Alpha tracks per frame | every model | spell effects only, with the first key baked otherwise | **Fixed:** every batch whose colour alpha or texture weight can change is played per instance in the blended pass; constant ones are baked |
| 14 | Texture weight bake | through `transLookup`, only when textureCount > 0 | indexed the track table with the lookup index | **Fixed** |
| 15 | Constant alpha of 0 | not drawn | skipped and drawn at 1 | **Fixed:** hidden |
| 16 | Skip threshold | alpha < 1e-4 | < 0.004 / 0.01 | 1e-4 for played tracks; the baked path still uses 0.01 |
| 17 | Opaque or alpha-key batch with alpha < 1 | Alpha blend | alpha pipeline only for a faded instance | **Fixed** for animated-alpha batches. Above 0.99999 they still blend; stock goes back to opaque |
| 18 | Animated colour RGB | per instance, every frame | baked first key into a per-batch UBO tint | **Fixed:** the RGB track is loaded (`colorRgbTracks`); a batch whose colour can change is played per instance into the instance entry (flag 0x20) and replaces the material tint in m2.frag. CharacterRenderer still bakes |
| 19 | UV rotation and scale | full matrix: translate, then scale and rotate about (0.5, 0.5) | translation only; the rotation track was read as a compressed quaternion | **Fixed:** float-quaternion keys (`QUAT_FLOAT`, 16 bytes as 0x82AD50 reads them); the 2x2 of scale then rotation per stage in the instance entry (flag 0x40, `stageMatrix` in m2.vert). `WOWEE_M2_UV_TRANSLATION_ONLY=1` for the old. CharacterRenderer still translation only. The direction checked against the code: 0x4C1C40 is the row-vector (D3D) quaternion matrix and 0x4C1B30/0x4C1B90/0x4C33C0 pre-multiply, so `uv' = Rc(S(uv + t − c)) + c` with Rc glm's `mat3_cast`, which m2.vert reproduces |
| 20 | Flag 0x01 sequences | play `loops` laps, then hold the last frame | loop, unless the caller asked to hold | **Fixed** for the default sequence of every instance: `loops = replayMin + rand × (replayMax − replayMin) >> 15`, at least 1, then the last frame holds (`stockLapsLeft`); a sequence set by a caller keeps its own rule. `WOWEE_M2_LOOP_ONE_SHOTS=1` loops them |
| 21 | Variation on a looping sequence | re-rolled every lap (CM2Model handler; replay ignored) | honours the replay range on laps | open (CGUnit's callback not traced) |
| 22 | Blend table | 3 = One+One, 4 = SrcAlpha+One, 5 Mod, 6 Mod2x | every mode ≥ 3 additive (SrcAlpha, One); no Mod or Mod2x pipelines; spell effects force blend 0/1/4/5 to additive | **Fixed:** one pipeline per mode in M2Renderer and CharacterRenderer; Mod/Mod2x unlit at emissive 1/0.5 and fogged to white/grey, additive to black; spell effects draw their own blend (a forge's keyed flame card stays additive); Mod/Mod2x never colour keyed; alpha reference 1/255 above blend 1 |
| 23 | Alpha-key reference | `alpha × 224/255` | shader cutoff 0.4/0.5, sharpened with alpha-to-coverage | **Fixed** as the "Smooth leaf edges" enhancement: off, M2 and character alpha keys discard below 224/255 with no fringe repair or mip boost (the model-alpha factor is not applied); on, the old ramp |
| 24 | Render flags 0x04/0x08/0x10 | cull, depth test, depth write, the write by the flag alone | every M2 pipeline is cull-none; depth write by pass | **Fixed** in M2Renderer: cull mode, depth test and depth write are dynamic state (core in Vulkan 1.3) on the nine M2 pipelines, set per draw from the material flags (`m2_raster_state.hpp`); no new pipelines. The opaque pass takes the flags as they are. In the blended pass nothing drawn through a blending pipeline writes depth: every blended batch of the world models carries 0x10 (7934 of 7934), so the only ones that would are opaque layers of a fading instance or with playing alpha, and stock draws those (above water) after the liquids and the weather, where Wowee draws both after the whole M2 pass. `WOWEE_M2_LEGACY_RENDER_FLAGS=1` for the old state. CharacterRenderer not changed |
| 25 | Transparent order | distance, then priorityPlane, then own distance, model, type, layer; particles in the same sort | per instance, back to front; particles in a separate pass after | **Fixed:** one element list per frame (batches and emitter runs) with the stock keys, sorted by `m2BlendOrderLess` (`m2_blend_order.hpp`); blend 0/1 particles drawn ahead as stock's opaque elements. Not done: ribbons (still after), the two-list split, the z-fill copies, attached children taking the parent's distance. With Soft particles on, blended particles keep the late pass. `WOWEE_M2_LEGACY_BLEND_ORDER=1` for the old order |
| 26 | Animation when culled | stops beyond 10 yd; time stays absolute | bones skipped off-screen and past LOD 3 (150 yd); clocks absolute | culled instances within 10 yd keep their bones stepping; doodad flag 0x400 not carried; the LOD-3 bone skip is not stock |
| 27 | Shader per batch | runtime id, layer folding and Combiners_* by stage (§3 "Shader selection") | one texture, `texture × light` | **Fixed:** `m2fx::resolveBatchEffects` (`include/rendering/m2_effect_select.hpp`), `assets/shaders/m2_combiner.glsli`; folded layers are not drawn |
| 28 | Second texture stage | bound with its own wrap flags and matrix | never bound | **Fixed** (binding 3 of the material set in both renderers) |
| 29 | Env coordinates | sphere map of the view-space reflection | none | **Fixed** (`m2SphereEnvUv`); stock's +z view space handled |
| 30 | UV set 1 and per-stage transforms | T2 / T1_T2 read UV set 1 through the stage's own matrix | UV set 0 only, one translation per batch | **Fixed** (translation earlier; rotation and scale with #19) |
| 31 | Weapon attachment FX batches | drawn | CharacterRenderer drops blend ≥ 3 batches on attached weapons | **Fixed:** drawn; `WOWEE_CHAR_DROP_WEAPON_FX=1` drops them again. Thunderfury (display 30606) checked against stock Wow.exe: the orb and lightning draw in both. Stock sheathes it across the back, Wowee at the hip (an attachment difference, open) |
| 32 | Map doodad distance and fade | five size classes, end 30/100/200/750/1250 yd (middle three × environmentDetail), linear fade over 5/10/15/20/50 yd, blended while fading | `m2InstanceMaxDistSq`: a density-adaptive 300-1000 yd × size factor (2.6× for foliage), faded over the last 25% (45%) in the opaque pass | **Fixed** for MDDF doodads (`setInstanceMapDoodad`, `m2_view_distance.hpp`): the stock classes and fade, the fading instance in the blended pass. The view-distance slider multiplies classes 1-4 (×1 at its 1200 default), capped at the view distance. WMO doodads, game objects and clutter keep Wowee's rule. Doodad flag 0x800 (no fade) not carried. `WOWEE_M2_LEGACY_DOODAD_FADE=1` for the old rule. Checked against stock Wow.exe at environmentDetail 1 (view distance 1200): Goldshire's JAR03 (class 0) shows at 24 yd and is gone at 27.5 and 32 yd in both |
| 33 | Glow card drawn as a solid square | the card blends by its texture's alpha; stock keys nothing | the flame-name hint (glow, lamp, torch, ...) put the texture on the black key, and m2.frag writes the fade, not the texture's alpha, for a keyed batch: GENERALHANGINGLANTERN01 and TORCH.M2 cards (Glow32/GlowOrange32, DXT5, blend 2) drew as yellow and orange squares with Soft lamp halos off | fixed for blend 2 by 20ef014 (another lane). Still keyed with real alpha: about 166 alpha-key and 182 opaque world batches by name match (e.g. StormwindStreetLamp01's STREETLAMP.BLP, blend 0, forced to the cutout pipeline by the key) |

## 6. In game

- **Frost Armor** (spell 168, kit 222, `FrostArmor_Low_Head.m2`): the crest
  batch should fade in over 0-134 ms, stay at weight 1 to 2167 ms, then
  fade out by 3000 ms. Before this change it sat at its last key from the
  first frame.
- **Rows of identical torches or banners** that load together now animate
  in step, as in stock. Particles still differ.
- **Doodads whose texture weight or colour alpha is constant 0** (hidden
  helper geometry) no longer draw.
- **Glowing or fading doodads** with animated alpha are now drawn in the
  blended pass.
