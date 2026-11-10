# Performance

How to measure a frame, what the numbers mean, and where the time went in
the scene this was tuned on. Companion to `perf_baseline.md`, which lists
the environment switches.

## Measuring

```
WOWEE_FRAME_PROFILE=1 WOWEE_LOG_LEVEL=info \
WOWEE_AUTO_LOGIN=1 WOWEE_AUTO_CHARACTER=<name> ./wowee
```

Every ten seconds the log carries one block:

- `frame budget over 10s: N frames, F fps, M ms each; median/p95 frame A B
  ms, cpu C D ms`: the mean, then the median and 95th percentile of the
  frames' wall time and of their CPU time - the wall time less the frame
  cap's sleep and the three places a frame waits on something else
  (beginFrame's frame fence and swapchain acquire, endFrame's present)
- `N queue submits; VMA B device memory blocks, A allocations, M MiB; U UI
  textures with their own device memory`: how often the frame and the
  uploads went to the driver in the window, and what every submit hands it
- one line per stage, worst first: the application's update and render
  stages, the game handler's sections (`gameHandler/network`,
  `/timers`, `/world/...`, `/interpolation`), the renderer's
  (`beginFrame/uploads+swapchain`, `/acquire`, `/record+ubo`,
  `/m2 cull upload`, `/grass+misc`, `/graph`), each render graph pass by
  name (`shadow_pass`, `volumetric_fog`, `minimap_composite`, ...) and the
  shadow pass by renderer (`shadow/terrain`, `/wmo`, `/m2`, `/characters`)
- `beginFrame waits: frame fence X ms, swapchain acquire Y ms`: the two
  places a frame waits on something outside the CPU
- `GPU, last frame`: the GPU's own pass timings, with the caveat the line
  prints when a driver resolves timestamps per render pass. Timestamps are
  written only under `WOWEE_FRAME_PROFILE`. In order: `culling +
  composites`, `shadows`, `volumetric fog inject` / `integrate`, `water
  reflection`, `scene pass` (terrain, sky, WMO, characters, doodads - split
  it with `WOWEE_PASS_ABLATION=1`), `rt_lighting`, `water refraction copy`,
  `water`, the post-process sub-passes that are on (`fsr2 motion vectors`,
  `fsr2 upscale`, `fsr2 sharpen`, `fxaa`, `fsr1 upscale`), `sun shafts`
  and `interface` (which includes the sun shaft composite)
- `renderWorld: prepare ... workers terrain T wmo W m2 M`: the parallel
  command recording, whose wall time is the slowest worker
- `m2 record: cull C, sort S, draws D over V visible of N instances`
- `M2 diag: total=N ... visible=V draws=D` every ten seconds at info

A stage is wall time on the main thread. When the main thread blocks on a
worker or a lock, the stage that blocked carries the cost, which is how
`gameHandler/world/transport` came to show 16 ms for a loop whose own CPU
work was microseconds.

`perf record -g -p <pid>` with `perf report --no-children` attributes CPU
across threads, which the stage table cannot: it found the M2 visibility
walk and the particle pass on the worker threads.

## Measuring an enhancement's cost

`WOWEE_ENHANCE_AB=<row>:<seconds>[:<label>]` flips one enhancement row
between its recommended and stock values on that period, in place, so both
states are measured on the same view in the same run; the row `all` flips
the whole set between Recommended and stock. A test script can do the same
row after row with `WoweeSetSetting("enhanceab", "<row>:<s>:<label>")` and
`"off"` (the scripts used are in the A/B notes of `docs/enhancements-plan.md`
section "Cost").

With `WOWEE_FRAME_PROFILE=1` as well, every flip closes a window and the
log gets one line per state instead of the ten-second block:

```
PROFILE_AB [<label> <row> on|off] frames=N frame_ms=F fence_ms= acquire_ms= cpu_ms= gpu_ms=G gpu_frames= frame_med= frame_p95= cpu_med= cpu_p95= | gpu: <pass>=ms; ... | cpu: <stage>=ms; ...
```

A `WOWEE_DEBUG_LUA` script can open such a window without flipping
anything: `WoweeDebug_ProfileTag("<label>")` closes the current window and
opens one named `<label>`, so a tour can measure "stormwind still",
"stormwind walk" and so on as one line each. The city numbers below were
taken that way.

The window opens 1.5 s after the flip (the flip itself may make an image or
a pipeline) and closes at the next one; the GPU passes are averaged over
every frame in it, not the last one. Compare adjacent on/off windows
rather than the means of each - the scene drifts (units walk, tiles stream
in) and an adjacent pair cancels that.

Two cautions from doing this on the Iris Xe:

- A GPU time is only a cost while the frame waits on the GPU. When the
  frame is CPU-bound the iGPU clocks down and every pass reads longer;
  the GPU column then over-states, and `frame_ms` is the number that
  matters. A long `endFrame` was read here as the GPU wait; it was not -
  it was the driver's CPU work inside vkQueueSubmit (see "Cities in stock
  mode" below). `cpu_med` now leaves the present call out, so the two can
  be told apart.
- Another client on the same GPU, or a build on the CPU (the iGPU shares
  the power budget and the memory bus), moves a window by a millisecond or
  two - and a `-j4` compile can take a 15 ms frame to 60-90 ms. Check
  `uptime` and other clients before trusting a small difference; the
  2026-11-04 runs (enhancements-plan.md 3.11) logged both every 10 s and
  dropped the windows within 10 s of either.
- Pair many short windows rather than few long ones. Goldshire's road view
  swings by 3 ms on a ~16 s cycle of its own, so three 4 s cycles read
  rows that cannot cost anything at +-1.5 ms; only the whole set's flip
  stands clear of that there.
- Set the camera again right before the first window: a teleport still
  loading when the script's first camera call lands resets the camera, and
  the view - not the build - then moves the numbers.

`WOWEE_LIGHT_TILES=0` turns the local-light grid off (every pixel walks
every light, as before it), and `WOWEE_SHADOW_CASTERS=terrain,wmo,m2,characters`
draws only the named shadow casters. `WOWEE_SHADOW_SPLIT=0` draws the sun's
shadow as one map with every caster every frame, as before the split below;
`WOWEE_UNIT_SHADOW_CAP=<n>` changes how many units the unit map takes (64,
0 for all); `WOWEE_UNIT_SHADOW_VIEW_CULL=0` draws into it the units whose
shadow cannot be seen as well; `WOWEE_FOG_HALF_RATE=0` lights every cell of
the mist every frame. `WOWEE_UNIT_SHADOW_WORKER=0` records the unit map's
units on the main thread again (below); `WOWEE_LIGHT_TILE_MARGIN_TILE=1`
culls the local lights to tiles by their box and a tile of margin, as before
enhancements-plan.md 3.12; `WOWEE_SHADOW_D16=1` makes both shadow maps
16-bit depth (an experiment: half the bytes behind every filter tap).
`WOWEE_SHADOW_WORLD_SIDE_LEVEL=1` keeps the kept world map at the quality
level's side instead of twice it (below).

`WOWEE_SINGLE_THREAD_RECORD=1` splits the scene pass on Linux: each world
pass gets its own timestamp (`terrain`, `wmo`, `characters`, `m2`, `water`),
which is what says where a whole-set difference goes. It costs main-thread
time, so CPU figures are taken without it.

What the iGPU's clock and memory are doing decides a GPU figure as much as
the frame does (enhancements-plan.md 3.12). Log `gt_act_freq_mhz` and
`throttle_reason_pl1` (`/sys/class/drm/card1/` and `.../gt/gt0/`) beside
every window: with a compile or a busy server on the CPU the package hits
its power limit and the iGPU runs at 1.05-1.25 GHz instead of 1.5, both
states alike, and every millisecond reads 25-40% longer. Even at a full
1.5 GHz the same view drew stock in 6.2 ms in one run and 11.7 in another,
and the set's difference scaled with it (+3.3 and +5.7): the iGPU's memory
traffic goes through the CPU's ring, whose clock follows the cores, so a
mostly idle CPU (a GPU-bound frame waiting on its fence) slows it. Compare
on/off pairs only at the same clock and in the same run, and say which
regime a figure was taken in.

## The sun's shadow in two maps

`rendering/shadow_cache.hpp`, `Renderer::planShadowFrame` and
`renderShadowPass`, `shadow_filter.glsli`. One map used to hold terrain,
buildings, doodads and units out to the shadow distance and was drawn again
every frame: 2.8-4.3 ms of GPU and 0.7-1.5 ms of main-thread recording at the
Ultra distance on the Iris Xe, for a picture in which almost nothing moves.

- **The world map** (terrain, buildings that are not transports, doodads
  that have not moved for 600 shadow frames) is drawn into one of two images
  and kept. It is centred where the player was and reaches 1.05 of the shadow
  distance and 8 yards more, so it is drawn again only when the player has
  walked 5% of the distance from that centre, the sun has turned a quarter of
  a degree, or the world has changed (terrain chunks, buildings or the
  doodads' content count moved; at most every 2 s) - and a redraw goes into
  the other image a part a frame (terrain, buildings, doodads), the two
  swapping when it is done. A teleport or a new distance draws it whole at
  once. Its texels are snapped to the light's grid, so a redraw lands static
  shadows on the same texels and nothing shimmers; a part a frame keeps a
  redraw to about a third of what the one map cost every frame.
- **The unit map** (units, the 64 nearest whose shadow can be seen;
  doodads and transports that moved lately) is drawn every frame into a
  per-slot image half the quality level's side (a quarter of the world map's), over a fifth of the shadow
  distance either way (32-90 yd), centred on the player and snapped to its
  own grid. A unit's shadow lies in the capsule its bounds sweep down the
  sun's rays to 40 yards below the lowest of camera, player and unit; one
  whose capsule is wholly behind a plane of the camera's frustum is not
  drawn and gets no blob either (`shadowCasterSeen`; not with water
  reflections on). In the Trade District that is 19-31 of the 64. A 64x64 mask of the
  cells a caster's footprint can reach rides in the per-frame block, and a
  pixel reads the unit map (five taps) only in a marked cell and only if
  the world map has not already shaded it wholly, taking the darker of the
  two.
- **The unit map's units are recorded on a frame worker**
  (`Renderer::startUnitShadowRecording`): right after `planShadowFrame` a
  job records the character draws into a secondary command buffer that
  continues the unit map's pass, while the main thread fills the per-frame
  block (the light gather), records the M2 and grass culls and the
  composites; the shadow pass records the moved doodads and buildings into
  a second secondary, joins the job and executes the two. Nothing on the
  main thread touches the character renderer in between. The profile shows
  `shadow/characters (worker)` (its own time) and `shadow/characters wait`
  (what the main thread still waits); `WOWEE_UNIT_SHADOW_WORKER=0` records
  them inline as before.
- Cost (enhancements-plan.md 3.9): the shadow pass is 0.05-0.1 ms of GPU
  and 0.2-0.4 ms of main-thread recording standing still (was 1.5-3.7 and
  0.6-2.7), the second filter 0.2-0.3 ms of the scene pass.
- Swaying trees hold still in the kept map (their shadows do not sway); the
  bias is far larger than the sway, so the canopies do not crawl.
- Every 30 s the log says how often the world map was drawn again and why,
  and how many casters the unit map took and how many mask cells they marked
  (`Shadow world map redraws: ...`).

## Shadows at a player's window (2026-11-06)

The figures above are from a 1251x986 test window at the Ultra columns. A
player's 2560x1080 window has 2.2 times the pixels, and every per-pixel cost
of the shadow rows scales with it: at Jered's settings (the default 2048
quality level, 500 yd, FSR2 at native scale) the `shadows` row was +5.1 ms of
GPU in Goldshire's road view and +4.5 in the Trade District at about 1 GHz,
of which the contact shadow pass alone was 1.2 ms (enhancements-plan.md
3.15). What it took:

- **Bias in yards** (`sunShadow` in `shadow_filter.glsli`): the normal offset
  was a uv texel used as a world length (a thousandth of a yard), the depth
  bias a fixed share of the depth range. Both now come from the map's own
  matrix: 2.5 texels x sin(angle) along the geometric normal, a quarter texel
  of depth.
- **The kept world map at twice the level's side** (up to 4096): it is drawn
  only on a redraw, so its size costs memory (64 MB more at the default
  level) and a redraw's fill, not a frame's. The unit map keeps half the
  level's side.
- **Soft is four bilinear compares** in a 1.5-texel square, no per-pixel
  noise; **far pixels take one** where a screen pixel spans more than 1.5
  texels of the world map (the pixel's angle rides in
  `localLightTileScale.z`).
- **Contact shadows march a quarter of the depth reads**: each pixel of a
  2x2 quad takes two steps of eight and the quad pools them through fine
  derivatives with a depth check; the reach is 24 yd. The pass was still
  0.8 ms at 2560x1080 and 1.05 GHz, so the row is recommended off.

After the first round (bias, map, filter, quad march): `shadows` +3.4 ms in
Goldshire's road view and +3.0 in the Trade District at the same clock and
window, the contact pass 0.8-1.0 ms. Measure at the player's window size, not
only the test window: the A/B scripts in scratch `sh2/` (`pair.sh` runs the
build before and after back to back, the "before" from a copy of its
shaders) take the window the compositor gives, and a fullscreen run (the
`fullscreen=1` setting) is the only way to get 2560x1080 here.

## Ray traced lighting on the Iris Xe (2026-11-06)

The `raytracedlighting` row (enhancements-plan.md 3.16) has no ray tracing
hardware to use on the Iris Xe (Mesa ANV exposes `VK_KHR_ray_query` only from
Xe-HPG), so it runs the compute tracer: a two-level BVH walked in a shader,
built on the CPU. Its costs, and how to read them:

- GPU: the `rt_lighting` timestamp (prepare, trace, temporal, three filter
  passes, at half the scene's resolution each way). It scales with the
  scene's pixels.
- CPU: `rt_lighting/sync` (the WMO and M2 renderers bringing their
  placements into `RtScene`) and `rt_lighting/record` (BVH builds, the top
  level, uploads), stages of their own in the profile block and in
  `PROFILE_AB` lines.
- `RtScene: N frames, R top-level rebuilds (instances added, removed,
  moved), meshes (triangles) built, pending; instances, traced; meshes,
  triangles` every ten seconds at info: why the record stage costs what it
  does. A rebuild every frame with a still camera means some renderer is
  churning instances inside the focus.

The row stays off on an integrated GPU without ray tracing hardware;
`WOWEE_RT_ALLOW_SOFTWARE=1` runs it anyway, which is how it was measured.
`WOWEE_RT_FORCE_SOFTWARE=1` keeps the compute tracer on hardware that has
ray queries. The row's two values are both 0 in the stock and recommended
sets, so `WOWEE_ENHANCE_AB` cannot flip it: the A/B script (scratch `rt/`,
`ab_body.lua`, `pair.sh`, `abpp.py`) sets the row from Lua and opens a
window per state with `WoweeDebug_ProfileTag`, a warm-up window after
switching on (the scene's meshes are built then) and steady ones after.
Do not name a script's own globals after anything the interface might
define (`MODE`, `W`, `VARIANTS`): the missing-API fallback answers an
unknown global with a no-op function, so `MODE or 1` is a function and the
row is set to 0 without a word.

What was slow, before the fixes (Goldshire, 1251x986, about 1.05 GHz):

| Stage | Before | After |
|---|---|---|
| `rt_lighting` GPU | 5.7-6.8 ms | 8.5 ms (see below) |
| `rt_lighting/sync` | 7.4 ms | 2.6-2.7 ms |
| `rt_lighting/record`, first 8 s after switching on | 52-62 ms a frame | 0.06 ms (the 0.9 million triangles near the camera build in the first six frames) |
| `rt_lighting/record`, steady | 3.3-4.8 ms (a top-level rebuild every frame) | 0.04 ms (no rebuild with the camera still) |

- **The whole loaded world was in the scene.** Every terrain chunk, WMO
  and doodad out to the view distance - 60k meshes, 18 million triangles at
  2400 yd - was built into a BVH at 150k triangles a frame (tens of seconds
  of 50-100 ms frames after switching on, and again whenever the view
  streamed), and every placement was in the top level, rebuilt on the CPU
  each time any of them changed. The lighting traces only 100 yd now, with
  250 yd rays, so `RtScene::setFocus` keeps the instances within 350 yd of
  the camera in the top level (rebuilt when the camera has moved an eighth
  of that, `RtFocus`), changes outside it do not rebuild it, and only the
  meshes those instances place are built.
- **The M2 sync walked and hashed all ~70k placements a frame**: it now
  drops what is out of reach on the instance's first cache line before any
  lookup.
- **Shadow rays were 1500 yd long**, walking the top level across half the
  loaded world; 250 yd now, the shadow map (500 yd) supplying the far
  casters by the darker of the two.

The GPU pass reads *more* than before (8.5 against 5.7-6.8 ms, paired
runs at 1.05-1.1 GHz with another client on the GPU): the old trace skipped
every pixel whose depth-rebuilt normal faced away from the sun - and
shadowed it, which was a good part of the noise on foliage - and those
pixels now trace (only what faces away by more than a quarter skips). The
100 yd range takes off what lies beyond it, which in a close view like
Goldshire's road is little.

What remains is the trace itself: at Jered's 2560x1080 the pass reads
about 19 ms of GPU (8.5 ms at 1251x986, scaled by the pixels; a fullscreen run could not be had - the compositor tiles the test window), several times the whole enhancement set's 3 ms budget,
for shadows that look about like the shadow map's. That is why the row is
off on integrated GPUs.

## The Enhanced preset's own passes (Iris Xe, 2026-11-10)

Lab server, 1280x540 window, vsync off, `WOWEE_FREEZE_CLOCK=100`, time held
at noon, the GPU at 1.5 GHz with no build running. Five fixed views (the
Goldshire inn, the Goldshire crossroads, the Trade District, Crystal Lake,
the Crossroads in the Barrens), the build before and after taken in turn,
two runs each, three 5.5 s windows a view; per-pass medians in ms.

| pass | inn | Goldshire | Trade | Crystal Lake | Barrens |
|---|---|---|---|---|---|
| final pass (`fxaa`) | 1.59 -> 1.06 | 2.29 -> 1.45 | 1.65 -> 0.91 | 1.49 -> 0.83 | 1.64 -> 0.90 |
| fill and bounce light (`ambient occlusion`) | 0.51 -> 0 | 0.61 / 0.69 | 0.50 | 0.53 | 0.49 |
| `culling + composites` (the grass cull) | 0.26 -> 0.17 | 0.23 -> 0.16 | 0.06 | 0.39 -> 0.22 | 0.19 -> 0.13 |
| frame's GPU | 5.8 -> 5.4 | 11.9 -> 11.8 | 9.2 -> 8.4 | 10.7 -> 9.9 | 8.4 -> 7.5 |

(The Goldshire road and the inn have walking units and a scene pass that
moves 0.5 ms between runs of the same build; the other three are steady.)
The Classic preset on the same views, same session: 5.7, 8.9, 11.0, 8.5 and
8.4 ms, its glow pass 0.27-0.42 ms. Classic's shorter view distance leaves
it more terrain in the Trade District; elsewhere Enhanced's own passes are
now about 1-1.5 ms over it, most of that the final pass's FXAA and the
fill and bounce pass.

What the final pass spent, by taking pieces out of it: FXAA's edge search
~0.7-0.85, the colour grade ~0.35, the glow's 16-tap blur ~0.25, the bloom
~0.6 (most of it the 8-bit halo's tone map at nine taps, worked out and
discarded under the HDR scene). Each change keeps the picture:

- The pixel and its eight neighbours are fetched, not filtered: sober they
  sit on texel centres, where the bilinear filter returns the texel.
- The HDR scene's bloom works out only its own half.
- The grade reads a segment's two ends as one load, one continent's table
  (the select read both), and nothing where its weight is 0. A table
  indexed per pixel is a memory read on this GPU, not a register.
- The glow's blur is bilinear between the quarter-size texels' corners
  (every tap is half a texel off), so `fxaa_blur.comp` works out the
  corner sums once a frame (0.09 ms) and the pass takes one tap of them.
  Their alpha is the brightest texel round each corner, and under the HDR
  bloom's floor the bloom's taps are skipped (0.18-0.2 ms on its own).

The fill and bounce pass is skipped wholly indoors, where every term of it
is outdoor light, and the grass cull reads a 32-byte copy of each blade
instead of the draw's 64-byte one. Screenshots of the five views before and
after differ by 0.12-0.21 of a level on average where two runs of the old
build agree with each other (the glow's sums are kept at half precision).

Tried and left: neighbours' luma through quad derivatives (slower, 1.38 ->
1.58 ms), skipping the water reflection and refraction copy with no water
surface in the frustum (at the Enhanced view distance some always is), the
fog inject (already half rate).

## The enhancement set's budget (2026-11-04)

enhancements-plan.md 3.11 has the tables. In short, what moved the set's
cost and how:

- The mist's inject (`volumetric_fog_inject.comp.glsl`): a group takes four
  slices of one parity out of eight (`fogInjectCell`), so on the half-rate
  frames a carried group returns before gathering lights, and the gather
  is a 64-bit mask built with shared atomics. Same terms per cell. Inject
  0.57-0.69 -> 0.27-0.34 ms.
- The unit map leaves out units whose shadow cannot be seen (above).
- The light gather walks a per-model list of light batches and fetches
  instances and models ahead; it was waiting on memory. 0.17 -> 0.12 ms
  (Stormwind), 0.31 -> 0.20 (Dalaran).
- Unit blobs are collected in view only (cheaper in stock as well).
- Parallax eases out between 25 and 45 yards; its cost never depended on
  the step count.
- Soft particles and parallax are off in the recommended set.

The set against stock, after 3.11: Stormwind +1.34 ms GPU / +0.48 ms CPU,
Dalaran +1.37 / +0.55, Goldshire's road view +6.73 / +0.34.

After 3.12 (the water copy takes depth only, the light tiles bound each
light by its sphere with two pixels of margin, the unit map's units record
on a worker): Stormwind +1.02 / +0.23, Dalaran -0.16 / +0.37, Goldshire's
road view +6.33 / +0.22 with the machine otherwise idle and +3.36 when the
memory runs fast. The iGPU's clock is not what Goldshire was missing: it
stays at 1.5 GHz in both states unless the CPU side pushes the package to its
power limit, and then it drops in both; what changes the figure at a full
clock is the regime the memory runs in (Measuring, above). Still over:
Goldshire's road view, by about the local lights' share (+3.3).

## Where the time went (Northshire, 2000 playerbots, Ultra preset)

| Cause | Fix | Frame before -> after |
|---|---|---|
| 70k ground clutter instances over the whole view radius while clutter draws within 300 yards | clutter placements stay with the tile and become instances only within range | 56 ms -> 46 ms |
| `WMORenderer::setInstanceTransform` rebuilt the whole spatial grid, and every transport called it every frame | refile the one instance | 46 -> 40 |
| FrameXML layout re-marked already hidden subtrees (97% of 28k widgets) every frame; `findByName` scanned all widgets for frames that do not exist yet | skip marked subtrees; name index | 40 -> 37 |
| 70k animation clocks stepped every frame | clocks advance when read | 37 -> 30 |
| cull fields scattered over a 600-byte instance | the fields the walk reads sit on one cache line | M2 worker 10 ms -> 4 ms |
| the M2 shadow pass walked all instances, then drew grass and sub-texel doodads | distance, kind and projected-size rejection before the model is touched | see log |

Not changed: the Ultra preset (view distance 2400, clutter 150%, shadow
distance 500, 8x antialiasing). These are the player's settings; the work
above makes them cheaper, not quieter.

## What is left

In the same scene, at about 35 fps:

| Stage | ms |
|---|---|
| renderWorld (parallel record; the M2 worker is the pole) | 7.4 |
| beginFrame, of which the shadow pass | 7.2 / 4.1 |
| addonWidgets (FrameXML layout 4.0, engine 2.0) | 6.6 |
| renderer->update (M2 bones on workers, characters) | 3.9 |
| record+ubo (light gathering walks instances) | 1.8 |

The shadow pass recorded one draw per batch per caster on the main
thread; the M2 casters have since been instanced per model and the split
above draws them only on a redraw of the world map. The
FrameXML sizing and visibility passes visit all 28k widgets each frame;
dirty lists would cut them to the handful that changed.

## Cities in stock mode (Iris Xe, 2026-11-03)

Stock enhancement set (`WOWEE_ENHANCE=stock`), the test account's saved
settings (2400 view distance, MSAA, no frame cap, no vsync), 1260x990
window, the lab server with its playerbots. The tour script stands 30 s
still and walks 30 s (forward, a half turn every 4 s) at Stormwind's Trade
District (-8829 640), Dalaran (5804 624) and Orgrimmar's Valley of Strength
(1633 -4439), 75 s after each teleport, each phase one `WoweeDebug_ProfileTag`
window. `cpu` is the frame less its waits (see Measuring), median / p95 in
ms over the window.

### Where the time went

`perf record -g` on the client at the Stormwind spot, main thread:

| | share of the main thread |
|---|---|
| `vkQueueSubmit` (endFrame's submit, the upload batches') | 70% |
| ... of which Mesa's `setup_execbuf_for_cmd_buffers`, one loop | 55-62% |
| everything the game itself does | 30% |

That loop is the i915 back end of Mesa's Intel driver handing the kernel
the buffer of every image that has a private binding, at every submit
(`image_private_objects` in `anv_batch_chain.c`). Every colour image the
driver compresses (CCS) gets one, for its clear colour. The terrain keeps a
64x64 alpha map and shadow map per chunk, ~22k chunks at this view
distance, so every frame's submit walked ~100k images, one cache miss
each; and every upload batch did the same, which is why a teleport took
two to four minutes to stop stuttering. VMA also gave each of those small
images a 64 KB-aligned slot for the compression: 7.5 GB of device memory in
Stormwind before, 2.4 GB after.

| Cause | Fix | Main thread |
|---|---|---|
| Every submit walks every compressed image (above) | The terrain's maps - single-level R8 textures up to 512x512 that are only sampled - are created with `VK_IMAGE_CREATE_ALIAS_BIT` on Intel, which keeps the driver from compressing them, so they never join the list. Every other image keeps its compression: with the small RGBA8 textures uncompressed too (1x1 white and flat-normal textures, minimap tiles) a Trade District shot moved 30% of its pixels by one level; with the R8 maps alone it matches the old path (`rendering/image_create_policy.hpp`, `test_image_create_policy`; `WOWEE_VK_SMALL_IMAGE_CCS=1` restores the old path) | Stormwind 22.4 -> 8.9 ms, Orgrimmar 32 -> 8.4, Dalaran 40 -> 16 (medians, below) |
| Two walks over every placed sound emitter per frame to find the periodic ones and the live advanced ones | Lists of both kept in the map's own iteration order, so the random stream is drawn in the same order (`WorldSoundEmitters::refreshOrder`) | Dalaran -0.9 ms |
| A ray across a WMO group gathered every grid cell of the rectangle its entry and exit span: a diagonal across a 64x64 grid is 4096 cells for ~190 it crosses (mouse-over pick, unit-name sight lines, floor rays) | Only the cells within the same one-yard margin of the segment, row by row (`segmentBandColumns`, `getTrianglesAlongSegment`; tested against the rectangle in `test_spatial_grid`) | WMO pick ray 3.9% -> 1.0% of the thread |
| The unit blob floor cache was dropped whole every 240 frames, so every sample of every blob was asked again in one frame | Each sample is asked again on its own age, 120-239 calls, spread by its key; samples nothing asks for are swept | renderWorld spikes of 45-55 ms every few seconds in Dalaran: gone |

The profile now also prints the median and p95 of each window's frame and
CPU time, the number of queue submits and device memory objects, and
`WoweeDebug_ProfileTag` lets a script name its own windows.

### Before and after

CPU ms per frame, median / p95, each a 30 s window. "Before" is the
committed build with `WOWEE_VK_SMALL_IMAGE_CCS=1` (the driver path as it
was; the smaller fixes still in), run back to back with "after" at the
same load; the first column is the build before any of this (its Dalaran
windows were still loading). Load is the median 1-minute load average over
the run: other agents' builds share the machine, and a run at load 10-20
reads 2-3 ms higher than one at 5.

| Place | Old build (load 9) | Before (load 5.6; Dalaran: 4) | After (load 5.4; Dalaran: 10) | Earlier after-runs, load 6-7 |
|---|---|---|---|---|
| Stormwind still | 25.6 / 101.0 | 22.4 / 25.0 | 8.9 / 10.6 | 7.9 / 9.7, 8.0 / 10.1 |
| Stormwind walk | 39.8 / 82.6 | 22.0 / 24.8 | 8.7 / 10.3 | 7.6 / 9.4, 7.6 / 9.7 |
| Dalaran still | (loading) | 42.4 / 308.7 (still loading) | 17.0 / 27.3 | 14.0 / 29.4, 13.4 / 17.7 |
| Dalaran walk | | 40.3 / 66.2 | 15.9 / 23.0 | 13.2 / 18.6, 13.0 / 16.4 |
| Orgrimmar still | | 32.3 / 35.2 | 8.4 / 10.5 | 8.4 / 10.4, 9.8 / 11.8 |
| Orgrimmar walk | | 32.6 / 35.9 | 8.2 / 10.1 | 9.2 / 11.0, 9.4 / 12.1 |

The earlier after-runs had every small sampled texture uncompressed (see
Pixels below); the committed rule, R8 maps only, leaves a few thousand
small RGBA8 images on the list, and the submit is back to ~4% of the main
thread in Dalaran (0.8% with all of them, 70% before). Device memory in
Stormwind: 8.0 GB before, 2.4 GB after.

From a teleport to the first frame the tour could measure (the gap window
less its 77 s of waiting): Stormwind to Dalaran 154-198 s before, 7-21 s
after; Stormwind to Orgrimmar 79 s before, 4 s after; Dalaran to Orgrimmar
183 s before, 7-9 s after.

With the CPU at 8-17 ms the frame now often waits on the GPU in Dalaran and
Stormwind (fence 2-5 ms a frame).

The recommended enhancement set (load ~6, every small texture uncompressed):
CPU 8.5 / 10.6 Stormwind, 13.4 / 17.7 Dalaran, 10.7 / 12.6 Orgrimmar -
within the scatter of stock - and Stormwind is GPU-bound there (frame 21
ms, fence 13 ms).

Pixels: the Trade District shot with `WOWEE_FREEZE_CLOCK=1000` at noon,
with and without `WOWEE_VK_SMALL_IMAGE_CCS=1`. Two runs of the same build
agree on 97-99.5% of pixels (the rest are units and particles that moved).
With every small sampled texture uncompressed, 30% of the pixels moved by
one level against the old path, run after run, frame cap or not; with only
the R8 maps uncompressed (what is committed) the shot agrees with the old
path as closely as two runs of it agree with each other (98.9%). Goldshire
is no test of this: its weather comes from the server, and two runs of one
build differ by a level on half the screen.

### What is left (Dalaran, main thread, after)

| | share |
|---|---|
| unit blobs' floor samples for walking units (`WMORenderer::getFloorHeight`, its triangle tests: a vertical ray's cell holds hundreds in Dalaran's big groups; an XY index of the group boxes was tried and bought nothing) | ~9% |
| mouse-over pick (`pickScene`: character bounds, M2 collision rays) | ~8% |
| `syncCreatureRenderInstances` | ~7% |
| `CharacterRenderer::update` (bones) | ~7% |
| FrameXML layout + draw + OnUpdate | ~14% |
| transports' doodads moved every frame (`M2Renderer::setInstanceTransform`, its dedup map) | ~4% |
| WMO portal visibility, M2 bone upload walk | ~7% |

The spikes left in Dalaran are the spawn queue (a player's model, equipment
and textures loaded in one frame, 20-37 ms) and the GPU (beginFrame's fence).

## Hitches (Iris Xe, 2026-11-03)

The medians above were fine; the frames that felt bad were the long ones. The
profile now reports p99 and max beside the median and p95
(`frame_med/p95/p99/max`, `cpu_*`, and `hitch25` / `hitch33`, the frames whose
CPU time passed 25 / 33 ms), and under `WOWEE_FRAME_PROFILE` every frame whose
CPU time passes `WOWEE_HITCH_MS` (25) logs one `HITCH cpu X wall Y [window]:`
line naming the stages that took it, longest first. The spawn queues report
as `spawn/<queue>` stages, the terrain finalize prints its phases every ten
seconds, and `renderer->update`'s untimed tail is now `update/footfall+sfx`,
`update/zone audio` and `update/model cleanup`. `WOWEE_LAYOUT_DIRTY_PROFILE=1`
names which FrameXML setter, on which frame, moved the layout generation.

`WOWEE_HITCH_LEGACY=1` puts back every loading path below as it was, so a
before and an after come from one build at the same load. The test hook's
`WoweeDebug_PushKey` now also holds the key and tells the bindings the window
has focus: a client a script drives in the background used to ignore pushed
keys, so the "walk" windows of the earlier tours stood still.

### What made them

| Hitch | Cause | Fix |
|---|---|---|
| 15-55 ms, any frame that uploaded anything | beginFrame waited on every async upload batch's fence (`waitAllUploads`). The batches go to the graphics queue, behind the previous frame, so the wait drained the GPU: the CPU and GPU ran in lock step on every frame a texture, tile or skin went up. Counted as CPU (it was neither the frame fence nor the acquire) | Batches on the graphics queue are ordered by submission; only a barrier is recorded (now to all stages, vertex and index reads included). The wait stays for `WOWEE_VK_ASYNC_UPLOAD_QUEUE=1` (`vk_context.cpp`) |
| 40-60 ms, a player coming into view | `spawnOnlinePlayer` read and parsed the race's model (again whenever the last of a race had left) and decoded every skin, face, hair and underwear texture on the main thread, three players a frame, each upload a separate submit-and-wait | A worker reads, parses and decodes; the main thread finishes them in order under a 3 ms budget. The parsed race models are kept. The look (CharSections) is resolved on the main thread as before. A player still appears whole, with the same base composite, a few frames later (`processPlayerSpawnQueue`) |
| 20-40 ms, a player's gear | The prefetch decoded only the composite's region textures; the helm, shoulders and weapons were read and parsed (`loadM2WithSkin`) and their textures and the cloak's decoded on the main thread, and every ready result was applied in the same frame, in any order | The worker also parses the item models into a shared cache and decodes their textures and the cloak candidates; results apply in the order asked for (an older change can no longer land on a newer one), under a budget |
| 30-55 ms, NPCs (guards in Dalaran) | Each guard's helm and shoulders, and every unit's weapons on every draw and sheathe, read and parsed the item model again and uploaded it under a fresh model id; the NPC prefetch dropped its decoded textures before the per-instance hair and head detail asked for them | `pipeline::loadM2WithSkinShared` keeps parsed models (LRU, 512); item pieces share one renderer model per mesh and texture (`itemModelIdFor`); the creature worker parses the helm and shoulder models; the decoded textures are kept until the instance overrides have used them |
| a game object's M2 (mailbox, brazier) | read, parsed and decoded in the frame it spawned | the same worker pattern the WMO objects use, in queue order (`launchGameObjectM2Loads`) |
| a crowd arriving | each queue had its own budget and always took one item, so their sum could land in one frame | `SharedFrameBudget`: 6 ms for the frame's spawn work, a queue refused three frames running gets one item anyway (`core/frame_work_budget.hpp`, `test_frame_work_budget`) |
| 40-160 ms, crossing a map tile | the minimap decoded its tiles on the main thread and uploaded each with a submit-and-wait while the frame was being recorded | the 5x5 around the player is decoded on workers ahead of need; uploads go in a batch with a barrier before the composite samples them |
| 35-70 ms, a terrain step | with the terrain texture cache at its budget, every alpha map asked for room, and every ask walked all ~22k chunks and ~100k textures and sorted them | eviction frees a sixteenth of the budget at a time, and a pass that found nothing is not repeated until a chunk unloads; a TERRAIN step uploads 4 chunks, not 16, so the 8 ms budget can stop between them |
| 10-30 ms every 5 s | `M2Renderer::cleanupUnusedModels` drained the device (`vkDeviceWaitIdle`) before freeing a model | the frees wait for the frames in flight instead (not with ray tracing on) |
| zone line | a zone's music was read out of the archive on the main thread | read on a worker, as file tracks already were; it fades in when it lands |

FrameXML: the layout generation moved on every frame because
`UIParent_ManageFramePositions` sets BuffFrame's point each OnUpdate (about
a thousand times in ten seconds); setting a point to the value it already has
no longer invalidates every rect resolved since the pass. The full layout
pass still runs every frame - visibility, text and scale changes are written
straight into the widgets without moving the generation, so a pass skipped
on an unchanged generation would miss them. OnUpdate handlers themselves are
0.15 ms a frame in Dalaran; the rest of the interface's ~2 ms is the layout
walk, the draw and the size/scroll checks over every frame.

### Before and after

CPU ms per frame, median / p95 / p99 / max, and frames over 25 ms, one 30-45 s
window each, legacy (`WOWEE_HITCH_LEGACY=1`) and new back to back on one
build, load average 5-7, the other client (shadow work) on the same GPU.

| Window | Legacy (run 1 / run 2) | New (run 1 / run 2) |
|---|---|---|
| Dalaran arrival | 12.7 / 17.6 / 41.5 / 111.9, 51 over 25 ms; 16.2 / 42.1 / 56.9 / 71.9, 328 | 12.6 / 16.5 / 21.1 / 43.8, 2; 12.8 / 15.5 / 19.6 / 27.6, 1 |
| Dalaran walk | 12.1 / 22.3 / 47.7 / 453.1, 82; 15.0 / 24.2 / 36.1 / 60.3, 82 | 13.1 / 18.8 / 24.0 / 49.4, 11; 13.3 / 18.0 / 23.2 / 57.9, 9 |
| Elwynn arrival | 7.6 / 9.6 / 12.3 / 153.1, 10; 9.7 / 11.2 / 12.9 / 26.9, 1 | 8.5 / 10.1 / 11.1 / 13.7, 0; 8.4 / 10.0 / 11.2 / 15.2, 0 |
| Elwynn flight | 8.6 / 10.3 / 14.9 / 22.1, 0; 9.9 / 11.6 / 15.2 / 24.5, 0 | 9.2 / 11.2 / 12.7 / 21.3, 0; 8.5 / 10.1 / 11.2 / 14.7, 0 |
| Stormwind arrival | 8.6 / 10.2 / 11.7 / 15.9, 0; 9.7 / 11.3 / 12.9 / 15.6, 0 | 10.2 / 11.8 / 13.0 / 19.9, 0; 8.5 / 10.0 / 11.1 / 18.8, 0 |
| Orgrimmar arrival | 9.2 / 10.9 / 12.0 / 16.3, 0; 10.3 / 28.9 / 33.9 / 58.5, 394 (load 15-23) | 10.5 / 11.9 / 13.6 / 16.2, 0; 9.4 / 11.2 / 14.7 / 21.0, 0 |
| Orgrimmar walk | 8.0 / 10.0 / 11.2 / 15.1, 0; 23.7 / 35.6 / 41.9 / 65.5, 382 (load 15-23) | 9.2 / 11.8 / 13.1 / 18.0, 0; 7.8 / 10.8 / 11.8 / 14.9, 0 |

The windows the teleport lands in (loading screen to the first window) went
from 34-112 ms worst frames to 50-69 ms; the terrain finalize still overruns
its 8 ms there now and then (a TERRAIN step of 60 ms after a teleport, a WMO
group upload of 10-25 ms). One run in another had the tile finalize spill
into Orgrimmar's arrival window (p95 16.9): with the steps smaller it keeps
to its budget, so a city's tiles can take a few seconds longer to finish
after the loading screen, at 8 ms a frame rather than 20-80.

### What is left

- `endFrame` 12-30 ms of CPU on some Dalaran frames: the queue submit itself,
  in the Intel driver; and the spawn batch's own submit once at 32 ms. Both
  builds show it.
- The minimap's indoor (WMO) composite still decodes its tiles when a building
  is entered (one 42 ms frame in Dalaran); only the outdoor ring is prefetched.
- A player's skin composite and an equipment change still blend on the main
  thread (4-8 ms each, one or two a frame under the budget); the blend lives
  in `CharacterRenderer` and would move with a worker-side composite.
- Item visual effect models (weapon glows) load on first use; music aside,
  sound files are read on first play (small).
