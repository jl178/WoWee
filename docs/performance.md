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

- `frame budget over 10s: N frames, F fps, M ms each`
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
