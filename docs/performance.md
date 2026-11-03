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
  two. Check `uptime` and other clients before trusting a small difference.

`WOWEE_LIGHT_TILES=0` turns the local-light grid off (every pixel walks
every light, as before it), and `WOWEE_SHADOW_CASTERS=terrain,wmo,m2,characters`
draws only the named shadow casters.

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

The shadow pass records one draw per batch per caster on the main
thread; instancing the casters per model would remove most of it. The
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
