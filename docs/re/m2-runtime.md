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
- **Material colour:**
  - Diffuse is `modelDiffuse × colour.rgb` (the animated M2Color RGB).
    Its alpha is the batch alpha.
  - Unlit adds the diffuse to the emissive and zeroes the diffuse.
  - Mod and Mod2x use diffuse 0 and emissive 1 or 0.5. Fog does not tint
    them.
- **Transparent order** (comparator 0x81EF30): sort by distance, far first.
  - The distance is the batch's: its skin section's sortCenter, through its
    center bone, pushed towards or away from the camera by sortRadius ×
    bone scale (batch flags 0x1 and 0x2). Some models are sorted whole by
    their centre instead.
  - Ties go to **priorityPlane ascending**, then batch distance, then
    model, then **materialLayer ascending**.
  - Opaque elements sort by texture and shader state (0x81EA90).
- **Ribbons** are type-3 elements, transparent, with alpha
  `model+0x198 × ribbon alpha`, sorted by their priorityPlane at
  `ribbon+0xAC`.

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
- **Doodad fade:** the alpha `+0x178` is a distance fade per doodad size
  class (tables at 0xADF38C..0xADF3DC). Below 0.99999 every batch goes to
  the blended pass, with opaque and alpha-key switched to Alpha blending.

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
| 18 | Animated colour RGB | per instance, every frame | baked first key into a per-batch UBO tint | open; needs a per-instance tint |
| 19 | UV rotation and scale | full matrix: translate, then scale and rotate about (0.5, 0.5) | translation only | open; the instance SSBO carries a vec2 offset |
| 20 | Flag 0x01 sequences | play `loops` laps, then hold the last frame | loop, unless the caller asked to hold | open for doodads. Spell kits hold one-shots through `holdInstanceAnimationAtEnd`, which matches for one lap |
| 21 | Variation on a looping sequence | re-rolled every lap (CM2Model handler; replay ignored) | honours the replay range on laps | open (CGUnit's callback not traced) |
| 22 | Blend table | 3 = One+One, 4 = SrcAlpha+One, 5 Mod, 6 Mod2x | every mode ≥ 3 additive (SrcAlpha, One); no Mod or Mod2x pipelines; spell effects force blend 0/1/4/5 to additive | open; visual risk, needs in-game comparison |
| 23 | Alpha-key reference | `alpha × 224/255` | shader cutoff 0.4/0.5, sharpened with alpha-to-coverage | open (a deliberate foliage choice) |
| 24 | Render flags 0x04/0x08/0x10 | cull, depth test, depth write | every M2 pipeline is cull-none; depth write by pass | open |
| 25 | Transparent order | distance, then priorityPlane, then layer | per instance, back to front | open |
| 26 | Animation when culled | stops beyond 10 yd; time stays absolute | bones skipped off-screen and past LOD 3 (150 yd); clocks absolute | close enough; the LOD-3 bone skip is not stock |

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
