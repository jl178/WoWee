# M2 particles and ribbons (stock 3.3.5a build 12340)

How the stock client runs and draws an M2's particle emitters
(`CParticleEmitter2`) and ribbon emitters (`CRibbonEmitter`). Reverse
engineered from `Wow.exe` with Ghidra; addresses are virtual addresses in that
binary. The particle code is `.\ParticleSystem2.cpp`, roughly 0x976000-0x983000;
CM2Model feeds it from 0x828A00, 0x82D2F0, 0x8309C0 and sets it up in
0x832EA0. The last sections compare this with Wowee and say what changed.

Offsets: `em+0x..` is the in-memory CParticleEmitter2, `file+0x..` the
on-disk M2Particle record (0x1DC bytes), `rb+0x..` a CRibbonEmitter, `p+0x..`
a particle (0x20 bytes: age, position, velocity, two 16-bit seeds).

## 1. Setup: file record to emitter (CM2Model 0x833C80..0x834100)

Every file field the runtime reads, and where it goes:

| file | field | runtime |
|---|---|---|
| 0x04 | flags | `em+0x134` runtime flags, translated (table below) |
| 0x08 | position | translated in front of the bone each frame |
| 0x14 | bone | the emitter's bone |
| 0x16 | texture | texture `H+0x174[i]` (0x978BF0) |
| 0x28 | blending type | material blend, mapped: 0→Opaque, 1→AlphaKey, 2→Alpha, **3→NoAlphaAdd (One, One)**, 4→Add (SrcAlpha, One), 5→Mod, 6→Mod2x. Depth write only for 0 and 1 |
| 0x29 | emitter type | 1 plane (vtable 0xAA2D30), 2 sphere (0xAA2D5C), 3 spline (base 0xAA2CC0) |
| 0x2A | particle colour index | ParticleColor.dbc override (rt 0x10, colours at `em+0xF4`) |
| 0x2C, 0x2D | particle type, head/tail | **not read**: flags 0x20000/0x40000 decide head and tail |
| 0x2E | texture tile rotation | `em+0x18`, not used by the quad builder |
| 0x30, 0x32 | rows, columns | 0x978C70: both must be powers of two; `em+0x120` rows, `+0x124` cols, `+0xC` log2(cols), `+0x10` 1/cols, `+0x14` 1/rows |
| 0x34..0xF0 | speed, speed var, vertical range, horizontal range, gravity, lifespan, rate, area length, area width, zSource | animated tracks, fed each frame (§3) |
| 0xAC | lifespan vary | `em+0xA8` |
| 0xC4 | rate vary | `em+0xA0` |
| 0x104, 0x114, 0x124 | colour, alpha, scale FBlocks | `em+0xD8`, `+0xDC`, `+0xE0` |
| 0x134 | scale vary (x, y) | `em+0xE4`, `+0xE8` |
| 0x13C, 0x14C | head cell, tail cell FBlocks | `em+0xEC`, `+0xF0` |
| 0x15C | tail length | `em+0xAC` (0x978D00) |
| 0x160, 0x164 | twinkle speed, percent | `em+0x13C`, `+0x140` |
| 0x168 | twinkle scale min, max | `em+0x144` = min, `+0x148` = max - min (0x97AC00) |
| 0x170 | burst multiplier | `em+0x14C` |
| 0x174 | drag | `em+0x168` |
| 0x178..0x184 | base spin, base spin vary, spin, spin vary | `em+0xC0`, `+0xC4`, `+0xC8`, `+0xCC` |
| 0x188 | tumble box | `em+0x150..0x164` (model particles only) |
| 0x1A0, 0x1AC | wind vector, wind time | `em+0x16C`, `+0x178` |
| 0x1B0..0x1BC | follow speed/scale 1 and 2 | a line, slope `em+0x180`, intercept `+0x17C` (0x978DD0) |
| 0x1C0 | spline points | spline emitter (0x981500) |
| 0x1C8 | enabled (uint8 track) | instance `+0x80` (0x82D2F0) |
| 0x18, 0x20 | geometry / recursion model names | model particles (0x978B30), child emitters (0x97AEB0) |

File flag → runtime flag (0x833C80..0x833EA5):

| file | meaning | runtime |
|---|---|---|
| 0x1 | **unlit** (material bit 0 = lit when clear) | vertex format with a normal when lit |
| 0x2 | sort particles back to front within the emitter (0x97E580) | 0x20 |
| 0x4 | head quad stretched along the velocity | 0x200000 |
| 0x8 | **unfogged** (material bit 1) | |
| 0x10 | model space: positions kept in emitter space, drawn through its current matrix | 0x200 |
| 0x20 | sizes times the emitter matrix's scale | 0x400 |
| 0x40 | births inherit the emitter's motion × burst multiplier | 0x800 |
| 0x80 (sphere) | a particle dies once it moves away from the centre | 0x1000 |
| 0x100 (sphere) | velocity +Z instead of radial | 0x8000 |
| 0x200 | random spin direction (by pool address bit 5) | 0x10000 |
| 0x400 | tail length clamped to the particle's age | 0x20000 |
| 0x800 | births spread along the emitter's last move | 0x2000 |
| 0x1000 | XY quad: in the particle space's XY plane, not facing the camera | 0x4000 |
| 0x2000 | births snapped to the ground (0x979740) | 0x40000 |
| 0x4000 | live particles follow the emitter's motion | 0x80000 |
| 0x8000 | burst on enable: `rate` particles at once when turned on, never continuously | clears bit 0 |
| 0x10000 | random head cell when there is no head track | 0x100000 (0x978E30) |
| 0x20000 | draw the head quad | 4 |
| 0x40000 | draw the tail quad | 8 |
| 0x80000 | x and y scale vary independently | 0x800000 |

Runtime 0x400000 (no distance thinning) is set by the spell code for kit and
missile models (0x6F7652, 0x701E88).

Data: 25440 emitters in the 3.3.5 client; every one has 0x20000 or 0x40000.
60 % spin, 49 % have a head cell track, 69 % are unlit, 84 % blend 4.

## 2. Lifetime curves (FBlocks)

- Timestamps are fixed16 life fractions, `s16 / 32767` (0x9793B0); they are
  not normalised by the last key.
- Segment choice (0x9793B0, search 0x979330):
  - one key: that key (the evaluators test the key count first);
  - two keys: the ratio itself, the timestamps ignored;
  - three keys: split at the middle key's time, `r/mid` or
    `(r-mid)/(1-mid)`;
  - more: the last key at or before the ratio.
- Values: colour (0..255 floats, rounded to bytes, 0x9795D0), alpha (s16 /
  32767, signed, 0x9794F0) × model alpha (`em+0x130`, the model's alpha
  clamped, set in 0x8309C0), scale **x and y** (C2Vector, 0x979480), cells
  (u16, rounded, 0x979560).
- The life ratio is `age / max(lifespan + seed × lifespanVary / 32767,
  0.001)` with the particle's `p+0x1C` seed. Plane and sphere births never
  write `p+0x1C` (0x9815C0, 0x981950), so the variation there depends on
  what the slot held; the spline birth writes it (0x978AD0).
- 0x97D370 precomputes emitters whose curves have at most three keys with a
  shared middle time, no lifespan vary, no scale vary and square scale keys
  (rt 0x1000000, read by 0x979D60). It gives the same values.
- Per particle, from its seed `p+0x1E` (0x979E90, 0x97A130):
  - size factor `max(1 + r × scaleVary.x, 1e-4)` for both axes, or one each
    with flag 0x80000;
  - spin: `angle = age × (spin + r × spinVary) + baseSpin + r ×
    baseSpinVary`, negated with flag 0x200 for odd pool slots. **No rotation
    at all unless spin or spin vary is non-zero.**
  - random head cell `rand × cells >> 32` (flag 0x10000, no head track).
- Twinkle (0x97BE80 head): `idx = (slot + round(twinkleSpeed × age)) & 127`
  into a 128-entry random table filled at startup (0x81C0D0, `0xDCE690`,
  values in [0, 1)). The particle is not drawn when `percent < 1` and
  `percent < table[idx]`; its size is multiplied by `min + table[idx] ×
  (max - min)` (always, so `min` alone when the range is 0).

## 3. Each frame

CM2Model (0x82D2F0, then 0x8309C0 per emitter):
- The enabled track (uint8, lower key) is sampled; with no keys the emitter
  is on. The other tracks are sampled only while enabled (or with no
  sequence): defaults 0 for a track with no keys. Rate goes through SetRate
  (0x97BD80: negative → 0); a disabled emitter gets rate 0.
- Runtime bit 0 ("emitting") = enabled. With file flag 0x8000 instead: on
  the rising edge of (enabled and rate > 0), runtime 0x40 is set once.
- The emitter matrix: `Rz90 × T(position) × bone × world` (row vectors).
  **Rz90** is a fixed 90° turn about Z (data at 0xD411E0): emitter +X is bone
  +Y, emitter +Y is bone -X. Its row 0 length is the scale (`em+0x1EC`).

CParticleEmitter2::Update (0x97EB10, `dt` in seconds):
1. Camera distance to the emitter into `0xDCE68C`.
2. Previous position kept (`em+0x1D0`), current set (`em+0x1B4`).
3. Flag 0x4000: `move = (pos - prev) × clamp(slope × |pos - prev| / dt +
   intercept, 0, 1)`.
4. Flag 0x40: a timer; past 1/30 s, `inherit = (pos - prev) × burst /
   (timer × 30)`, 0 if no particle is alive.
5. The step (0x97ACB0): dt above 0.1 s is split into tenths, at most
   `nearbyint(min(floor(10 dt), floor(10 life)) - 0.5)` of them (the
   round-to-nearest of the FPU, so some counts lose a tenth), then the rest
   `dt - floor(10 dt)/10`. The follow move is shared out over the steps.

Each sub-step (0x97DD20):
- If emitting (or the burst flag): the pool grows to
  `round((rate + rateVary)(life + lifeVary) × 1.15)` (0x97EDF0; only grows).
- Births (0x97D8C0):
  - `rate' = density × (rate + rateVary × r) × lod`, `r` in [-1, 1];
    density is the `particleDensity` CVar (0x78D860, 0.1..1, read by
    0x980ED0); `lod = clamp(1 - (distance - 50) × 0.02, 0.25, 1)` unless
    runtime 0x400000.
  - Burst: `trunc(rate')` particles now, age 0, then 0x40 cleared.
  - Continuous: `acc += rate' × dt; n = trunc(acc + 0.5)`; born while a pool
    slot is free; `acc -= born` (so births owed to a full pool stay owed).
  - Flag 0x800: each birth's emitter position is lerped from the previous.
- Each type's create (0x9815C0 plane, 0x981950 sphere, 0x979870 spline):
  - age = `rand01 × dt`;
  - speed = `(1 + r × speedVar) × speed` (0x9792D0);
  - **plane:** position `(r × length/2, r × width/2, 0)`; direction
    `(cos h × sin v, sin h × sin v, cos v)` with `v = r × verticalRange`,
    `h = r × horizontalRange`;
  - **sphere:** radius `length + rand01 × (width - length)`; direction
    `(cos h × cos e, sin h × cos e, sin e)` with **elevation from the XY
    plane** `e = r × verticalRange`; velocity along it (or +Z with flag
    0x100);
  - zSource ≠ 0 (both): direction `normalize(position - (0, 0, zSource))`;
  - spline: a point on the spline, a random direction;
  - world space (no flag 0x10): position and velocity through the emitter
    matrix (scale included);
  - flag 0x40: `+ inherit × (1 + r × speedVar)`.
- Every particle, new ones included (0x97DD20, 0x979BB0):
  - `age += dt`; dead at `age >= lifespan` (above);
  - wind: `vel += wind × dt` while `age < windTime`;
  - flag 0x4000: `pos += move share` once `age > 2 dt`;
  - `pos += vel × dt; pos.z -= gravity × dt² / 2; vel.z -= gravity × dt` —
    gravity is a scalar down the particle space's Z;
  - drag: `vel -= vel × min(drag × dt, 1)`;
  - components below 1e-8 flushed (0x978B70);
  - flag 0x80 (sphere): dead once `dot(pos - centre, vel × dt) > 0`.
- Removal swaps the last live index into the hole (the pool's order).

## 4. Drawing (0x97E730, 0x97A390, quad builder 0x97BE80)

- Set up per emitter (0x97A390): the particle-to-view matrix (`0xB2D550`) is
  `[emitter matrix if model space] × T(-camera) × view`; the vertex normal
  (`0xB2D540`) is the view's third row; for an XY quad the first two rows
  of that matrix (divided by the emitter's scale in model space) are the
  quad axes (`0xB2D590`), the third the spin axis.
- At most `0x4000 / (4 per quad)` vertices an emitter (4096 head-only
  particles, 2048 with heads and tails). With flag 0x2 the particles are
  sorted by view depth first (0x7A0F50 / 0x97E080).
- **Corners** (`0xB2D5B4`): `(-1, 1), (-1, -1), (1, 1), (1, -1)`, times the
  size, in view x and y. **The scale is a half size**: a scale of s is a 2s
  × 2s quad. Texture coordinates (`0xB2D5D4`): `(0,0), (0,1), (1,0), (1,1)`
  — corner 0 top left.
- Size: curve scale (x, y) × size factor × twinkle × (emitter scale with
  flag 0x20). In world units, so its size on screen follows the projection.
- **Head** (flag 0x20000):
  - velocity quad (flag 0x4, `|v|² > 2.4e-7`): x along the view-space
    `-velocity`'s screen direction, its length × `|v.xy| / |v|`; y across;
    no spin;
  - else no spin (spin and spin vary 0): the corner table;
  - else spin: the corners turned by the angle, counter-clockwise;
  - XY quad (flag 0x1000): the same offsets along the particle space's X
    and Y instead of the screen's (with spin: turned about its Z). For a
    world-space emitter that is the world's XY plane.
- **Tail** (flag 0x40000): `T = view(-velocity) × tailLength` (× clamped to
  age with flag 0x400). If `|T.xy|² > 0.00077`: corners `P ± p`, `P + T ±
  p` with `p = (-size.y T.y, size.x T.x) / |T.xy|`; texture u along the
  tail. Otherwise a plain quad at the head.
- **Cells**: head cell from its curve, else the random cell, else 0; tail
  cell from its curve, else 0. Column `cell & (cols-1)`, row `cell >>
  log2(cols)`; `uv = (col + u) / cols, (row + v) / rows`.
- **Material** (0x8214E0): two-sided; unlit with file flag 0x1; no fog with
  0x8; no depth write unless blend 0 or 1. The blend table and alpha
  reference are the batches' (docs/re/m2-runtime.md §3): alpha key at
  224/255, other blends 1/255. Particles draw as transparent elements,
  sorted with the model's batches (M2ForceAdditiveParticleSort,
  M2BatchParticles CVars at 0x402811/0x402839).

## 5. Ribbons (CRibbonEmitter)

- Setup (0x9808A0, from 0x833900): rate `ceil(edgesPerSecond)`, life
  `max(lifetime, 0.25)`, a ring of `ceil(life × rate) + 2` edges, texture
  cell size `1/cols` by `1/rows`, heights default **10** (overwritten by
  the tracks), "has previous frame" cleared. Texture and material indices
  are direct indices into the model's textures and material table.
- Per frame (0x828A00): colour, alpha × model alpha, above, below, texture
  slot (uint16) and visibility (uint8) from their tracks. Only when the
  model has a world transform (`+0x10 & 0x8000`, set by 0x8251D0): matrix
  `T(position) × bone × world`, no turn.
- SetMatrix (0x97F940), while visible: the previous frame becomes this
  one's predecessor; **with no previous frame (new, or just shown) the
  previous is set equal to the current**, so a trail never joins a spot it
  was not at. Kept: position, the matrix's Y and Z rows (scaled).
- Update (0x980090):
  - the first update ever uses `dt = 1/rate + 1e-4` (lays exactly one edge);
  - `dt` clamped to `[0, life]`; edges with `age + dt > life` dropped;
  - while visible: `total = dt × rate + frac`; for k = 1..⌊total⌋ an edge
    at `t = (k - frac) / (dt × rate)` between the frames, age `-t dt`;
    `frac = total - ⌊total⌋`; then the live head at t = 1, not committed;
  - an edge's two ends are `pos ∓ Y × below/above` on each frame, joined by
    `(prevEnd + prevZ d t)(1-t) + (curEnd - curZ d (1-t)) t` with `d =
    |cur - prev|` (0x97FEF0): the straight line unless the emitter turns;
  - every committed edge: `z += (2 age + dt) gravity dt` (so `+gravity ×
    age²`, positive lifts), `age += dt`, `u = age / life / cols + cell u`,
    the "below" end at `v = cell v`, the "above" end one cell lower.
- Draw (0x980B70): a strip of all committed edges plus the head slot, each
  of the ribbon's material layers (all 3.3.5 ribbons have one). The strip
  is **flat along the emitter's Y axis**, not turned to the camera.
- Missile impact (0x703410): the missile model is moved onto the impact
  point, then its alpha goes to 0 over 0.15 s (0x9F23D4). The ribbon's
  alpha is the model's × the track's at draw, so the trail fades with it.

## 6. Wowee before, and the gap table

Files: `include/rendering/m2_particle_stock.hpp` (new, the pure arithmetic),
`m2_particle_look.hpp` (record, curve packing), `m2_particle_step.hpp`,
`src/rendering/m2_renderer_particles.cpp`, `m2_renderer.cpp` (pipelines),
`m2_renderer_render.cpp` (update loop), `src/pipeline/m2_loader.cpp`,
`glue_scene_particles.cpp`, `spell_visual_system.cpp`, shaders
`m2_particle_gpu.vert/.frag`, `glue_particle.vert/.frag`.

| # | Item | Stock | Wowee before | Now |
|---|---|---|---|---|
| 1 | Primitive | quads, world-unit size | point sprites, `scale × 500 / dist` px, clamp 1..128 px | **Fixed**: instanced quads, 4 strip corners per record |
| 2 | Size | scale is the half size (2s quad) | scale ≈ point size; 128 px cap | **Fixed** |
| 3 | Scale x/y | C2Vector, both used | x only (round sprite) | **Fixed** |
| 4 | Size clamps | none | spell ≥ 0.15, doodads ≤ 1.5, flames ≥ 0.15 | **Removed** |
| 5 | Colour/alpha hacks | none | plain doodads whitened 70 %, alpha × 0.02/0.05; flames floored | **Removed** |
| 6 | Flame rate floor | none | rate ≥ 15 live particles for fixtures | **Removed** |
| 7 | Firefly drift | none | random drift at speed 0 | **Removed** |
| 8 | Spin | baseSpin, spin and their varies; random direction 0x200 | none | **Fixed** |
| 9 | Velocity quads (0x4) | stretched along velocity | none | **Fixed** |
| 10 | XY quads (0x1000) | in the particle space's XY plane | none | **Fixed** |
| 11 | Tails (0x40000), tail length, clamp 0x400 | tail quad | none | **Fixed** |
| 12 | Head flag (0x20000) | head drawn only with it | always | **Fixed** |
| 13 | Head/tail cell tracks | per life | ignored; tile stepped by model anim time | **Fixed** |
| 14 | Random tile flag | 0x10000 | 0x40 & 0x80 (wrong bits) | **Fixed** |
| 15 | FBlock times | s16/32767 | normalised by the last key | **Fixed** |
| 16 | FBlock 2/3-key rules | ratio / middle split | generic search | **Fixed** (same for sorted keys, differs for 2-key) |
| 17 | Alpha sign | signed s16 | unsigned | **Fixed** |
| 18 | Twinkle | speed, percent, scale range | none | **Fixed** (phase per particle instead of pool slot) |
| 19 | Scale vary | per particle, 1 or 2 factors | none | **Fixed** |
| 20 | Rate vary, lifespan vary | per sub-step / per particle | none | **Fixed** |
| 21 | Emitter frame Rz90 | bone frame turned 90° about Z | bone frame | **Fixed** |
| 22 | Plane area | length × width rectangle | ignored (point) | **Fixed** |
| 23 | Plane azimuth | ± horizontal range | uniform 0..2π | **Fixed** |
| 24 | Sphere angles | elevation from XY | polar from Z | **Fixed** (same ring for InstancePortal) |
| 25 | zSource | radial from (0,0,z) | ignored | **Fixed** |
| 26 | Gravity | closed form per sub-step | semi-implicit | **Fixed** |
| 27 | Drag | linear `min(drag dt, 1)` | `exp(-drag dt)` | **Fixed** |
| 28 | Wind, follow (0x4000), inherit (0x40), along-path (0x800), sphere kill (0x80), sphere up (0x100), burst (0x8000) | as §3 | none | **Fixed** |
| 29 | Enabled track | uint8 | ignored | **Fixed** |
| 30 | Sub-steps | ≤ 0.1 s, ≤ a lifespan | one step, births aged in closed form | **Fixed**; Wowee's dormant-emitter warm-up is capped at 40 steps |
| 31 | Pool cap | 1.15 × peak rate × life per emitter | 4000 per instance | **Fixed** (4000 kept as an outer cap) |
| 32 | Distance thinning | 50..87.5 yd, floor 0.25; not spell models | none | **Fixed** |
| 33 | Density CVar | multiplier 0.1..1 | multiplier then flame floor | **Fixed** |
| 34 | Blend 3 | One + One | SrcAlpha + One | **Fixed** (7 pipelines, one per blend) |
| 35 | Blend 0/1 | depth write, alpha ref 224/255 | alpha blend, luminance discard | **Fixed** |
| 36 | Mod / Mod2x | DstColor blends | alpha blend | **Fixed** |
| 37 | Lit particles | lit unless file 0x1 | never lit | **Fixed** (ambient + sun with a camera-facing normal; the normal's sign is not confirmed) |
| 38 | Unfogged 0x8 | no fog | fogged | **Fixed** |
| 39 | Model alpha | particle alpha × model alpha | ignored | **Fixed** |
| 40 | Sort 0x2 | back to front within the emitter | none | **Fixed** (by distance) |
| 41 | Per-emitter draw cap | 0x4000 vertices | none | **Fixed** |
| 42 | Soft round sprite edge | none | smoothstep circle | **Removed** |
| 43 | Draw order with batches | sorted with the model's transparent batches | separate pass after | open |
| 44 | ParticleColor.dbc override (0x2A) | per creature display | ignored | open |
| 45 | Ground snap 0x2000, model particles, child emitters | §1 | none | open (157, 54, few emitters) |
| 46 | Spline emitters | spline object | uniform on the polyline | approximated (133 emitters) |
| 47 | RNG | Blizzard's generator, per-slot seeds | std::mt19937, per-particle draws | statistical match only |
| 48 | Stepped emitters | animated models (drawn, or within 10 yd) | in range and in view, warm-up on waking | Wowee-only, kept for performance |

Ribbons:

| # | Item | Stock | Wowee before | Now |
|---|---|---|---|---|
| R1 | Strip orientation | flat along the emitter's Y axis | camera-facing | **Fixed** |
| R2 | v | below end v = 0 | top end v = 0 | **Fixed** |
| R3 | Tracks | per frame, playing sequence (heights default 10) | first key of any sequence (default 0.5) | **Fixed** |
| R4 | Texture slot track | cell per frame | ignored | **Fixed** |
| R5 | Blend | material table | `gpu.batches[materialIndex]` (a skin batch) | **Fixed** |
| R6 | Edge interpolation | curve along Z axes | straight line | **Fixed** |
| R7 | Gravity | `+g age²` | `-g dt²/2` per step, no accumulation | **Fixed** |
| R8 | First update | lays one edge | none | **Fixed** |
| R9 | Model alpha | at draw, whole trail | baked per edge (none at all before) | **Fixed** |
| R10 | Jump break | none (joins) | new strip past max(12 yd, 500 yd/s) | **Removed** |
| R11 | Missile impact | moved onto the impact point, fades in 0.15 s | stopped short, trail stayed 0.6 s at full alpha | **Fixed** |
| R12 | Ring size | `ceil(life × rate) + 2` | 128 | **Fixed** |

## 7. In game

- **Fireball**: the flame (emitter 0) is a tail of 1.2 s of travel behind
  each particle, the sparks spin through a 2 × 2 sheet at random cells, and
  the glow (emitter 3) is a 7.5 × 3.9 yd quad stretched along the flight.
  Before, all of it was round dots capped at 128 px.
- **Frostbolt**: the two trails are flat strips (one turned by bone 7),
  fading with u along their 0.6 s; at impact they fade with the missile in
  0.15 s instead of hanging between caster and target.
- **Campfires, torches, Hellfire braziers**: flames at their authored size
  and count, without the floors; smoke (lit) darkens at night.
- **Waterfalls / spray**: XY quads and velocity quads lie as authored.
- **Dalaran**: many lit, sorted and tiled emitters; check the frame rate.
