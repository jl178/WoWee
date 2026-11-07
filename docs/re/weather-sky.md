# Weather and sky (stock 3.3.5a build 12340)

Where the stock client draws its weather, the mist and sand it draws with
it, what makes a unit breathe out in the cold, and whether a thunderstorm
lights the sky. Read from `Wow.exe` with headless Ghidra (MapWeather.cpp,
Unit_C.cpp, DayNight); addresses are virtual addresses in that binary. The
sand shader was read as ARB text from
`Data/expansions/wotlk/shaders/vertex/arbvp1/sand.bls`. This follows
world-lighting.md section 8 (rain, snow, splashes, grade and storm light),
which is not repeated here.

The pure rules are in `include/rendering/stock_weather.hpp`, tested in
`tests/test_weather_fx.cpp` (`[stock_weather]`).

## 1. Where the weather is drawn

**Stock.** The three emitters' draws (rain 0x78ae20, snow 0x78ba60, sand
0x78c3e0, all from 0x78ca50 <- 0x77f030 <- WorldFrame 0x4f8ea0) first update
their particles, then return without drawing when `0xadf580 < 0`. That float
is part of the outdoor rectangle the world draw (0x79a870) keeps for the
sky:

- camera outside (`0xcd87a4 == 0`, the camera's group from 0x795d40 is
  none or an exterior one): the rectangle is the whole screen, `0xadf580 = 0`;
- camera in an interior group: the rectangle is reset to empty
  (`0xadf580 = -1`, 0x9e2ef4) and the portal walk from the camera's group
  (0x7b3b20 -> 0x7ad1f0) opens it through any portal into a group with
  0x50148 (0x7a8f20 -> 0x795d00); a camera group with 0x40140 opens it at
  once (0x795d40, 0x790570).

So the weather draws exactly when the sky does (0x79a870 tests the same
float before 0x7f09b0). There is no test of the camera against group boxes,
and no MOGP 0x2000 test of its own: a street of an exterior group is
outside. The particles go on being simulated indoors.

**Wowee before.** `Renderer` turned the weather off whenever the camera was
inside any group's bounding box (`insideWmoArea`), exterior groups included.
Gadgetzan's and K3's streets are exterior groups, so no weather drew there,
stock rain included. `Weather::setEnabled(false)` also threw the particles
away.

**After.** `stock_weather::weatherDrawsFromCamera`: drawn unless the portal
walk (`WMORenderer::computeVisibility`, wmo-rendering.md 10) reports the
camera sealed in an interior; without the walk (`WOWEE_WMO_PORTALS=0`), unless
the camera is in an interior (0x2000) group's box. The previous frame's walk
is used (the weather updates before the frame's walk). Off, the weather is
still simulated and only not built or drawn.

**Same class, IsIndoors.** `[indoors]`, `IsIndoors()`/`IsOutdoors()` and the
fireflies read `playerIndoors_`, which was the same camera-box test. Stock
(0x71b7f0 -> 0x77fbf0) asks the player's map object group: outdoors when it
has the exterior flag 0x8 (WMOAreaTable flags 4 / 2 force it either way);
out of a group, the area's AreaTable flags: 0x4000000 outdoors, else
0x2000000 indoors, else outdoors. Now `stock_weather::playerOutdoors` on the
player's own group (the tightest group round the player, when that group's
floor decides the area as for the zone text) or the live area's flags.
The WMOAreaTable row's Flags (column 9, +0x24; 0x4 on 1157 rows such as
Stormwind's streets, 0x2 on 73) are loaded with the rows (`WmoAreaRow::flags`,
`WmoAreaInfo::flags`) and override the group's flag: 0x4 outdoors, else 0x2
indoors.

## 2. The mist and the sand

### Mist sheets (every weather)

Each emitter owns a mist emitter (ctor 0x78c420, called from the rain ctor
0x78ca90, snow 0x78cd30, sand 0x78cf20), updated by 0x786e10 at the end of
its draw.

| | Rain (and thunderstorm) | Snow | Sand |
|---|---|---|---|
| Texture | `textures\Weather\SnowMist01.blp` | the same | `textures\Weather\WeatherMistGrainy01.blp` |
| Speed (+-half of spread) | 5 +- 0.6 yd/s | 9 +- 1.5 | 15 +- 2.25 |
| Sheets a second (shader path) | `max(0, (level - 0.5) * 2) * density * 38` (0xa3e9b8; 18 fixed function, 0xa120b0) | the same with 48 (0xa3e9c8; 24) | `64 * density * level` (0x9e8dd8; 32) |

`level` is the particle level `(grade - 0.25) * 4/3` (0x784850) and
`density` the weatherDensity factor (0x784040: 0.10 / 0.33 / 0.66 / 1.0).
Common to all three (0x78c420 arguments, the spawn 0x786560, the draw
0x786e10):

- **Size**: a square 12 yd on a side (0xa1047c), facing the camera (its
  corners are the camera's right and up vectors times 6, 0x407f80).
- **Spawn** (0x786560): a direction `-1.57 + (r - 0.5) * 0.349` rad
  (0xa3ec48, 0xa3ea3c: 90 degrees off, +-10) and the speed above, as
  `(sin a, cos a) * speed`, rising `1/3 + (r - 0.5)/30` yd/s (0xa12098,
  0xa19ce0); a position anywhere in a 44 x 44 x 25 yd box (0xa3ec4c,
  0xa2e868) round the camera (0xcd8f5c); both turned by the heading (below,
  0x4c3290 with `-heading`, 0x4c2210); the position then moved back by 1.5 s
  of the velocity (0xa41b18) and lifted to `max(ground, z) + 6` (half the
  size). An acceleration `(r - 0.5) * 3.33` (0xa3ea48) on every axis; a life
  of `2.7 + (r - 0.5) * 0.3` s (0xa3ea44, 0x9e2ebc).
- **Heading** (0x78d170 -> weather +0x18c): `atan2` of the player's velocity,
  or its facing (vtable +0x34) when it moves slower than 1 yd/s (0x9e1130) or
  has movement flag bit 20. So the sheets come from 22 yd either side of the
  player and blow back past it, 90 degrees across its way.
- **Path**: 0x786330 samples the ground along the way it will travel (up to
  63 samples) and cuts the life short where the ground rises more than 1 /
  0.75 / 0.5 yd over three / two / one samples. Each frame (0x786e10) a sheet
  under `ground + 6` there is lifted by up to 3 yd (a quarter of its size) and
  its acceleration grows by 1.667 (0xa0b634).
- **Cap**: 128 alive (the 0x80 argument).
- **Look**: vertex colour the fog colour (DayNight +0x8c), alpha
  `fade * (1 - clamp(1.5 - d/12))` per corner (d the corner's distance from
  the camera: none inside 6 yd, all past 18), `fade = min(t/0.4, 1) *
  min((life - t)/0.4, 1)` (0x9f98d8); GxBlend_Alpha (alpha-ref table
  0xad8b7c entry 2), depth tested, not written, unfogged. Drawn after its
  emitter's particles.

### Sand grains

The sand emitter (rates 0x784580, packets 0x788660 / 0x786210, spawn
0x785ea0, draw 0x78bee0 with `sand.bls`, fixed function 0x78bb00):

- **Rate**: `32000 * density * level` grains a second (0xa3e9d8; 6000 fixed
  function, 0xa3e9d4), in packets of at most 6144 (0x1800) built on the CPU
  once and then moved by the vertex shader.
- **Spawn** (0x785ea0): in the emitter's box 40 x 40 x 25 (the ctor's 40, 25
  halved into +4..+0x18), at `x = (0.85 + 0.15 r) * 40` ahead (0x9f23d0,
  0x9f23d4), `y = (r - 0.5) * 40`, `z = (r - 0.5) * 25`; velocity
  `(sin a, cos a) * speed` with `a = -1.57 + (r - 0.5) * 0.349` (0xa2419c) and
  `speed = 18.667 + (r - 0.5) * 0.667` (0xa3ea38, 0x9fc830), rising
  `0.833 + (r - 0.5) * 0.167` (0xa3ea34, 0xa33b7c); both turned by the heading;
  plus the player's velocity (weather +0x164) times 1.75 (0xadf22c) and the
  camera position. A life of `3.2 + (r - 0.5) * 0.3` s (0xa3ea30). Born under
  the ground (vtable +0xc), it is dropped; otherwise its path is cast against
  the world (vtable +8) and its life ends where it hits.
- **Draw** (`sand.bls`): `age = clamp(t - birth, 0, life)`,
  `pos = origin + velocity * age`; a point sprite (render states 0x53 on,
  0x51 1.0, max size `min(caps, 24)`) of
  `clamp(1.5 - 0.05 d) * viewport width * 0.0025` pixels (c9 = (-0.05, 1.5,
  W * 0.0025), 0xa3ebd8, 0xa3ebec) - full to 10 yd, none past 30; colour the
  fog colour (c0, DayNight +0x8c) with alpha
  `min(5 age, 1) * min(5 (life - age), 1)`; no texture; GxBlend_Alpha.
  With 0.66 density about 67000 grains are alive.

So the stock sandstorm is fog-coloured grains blowing at the player from
34-40 yd ahead at about 19 yd/s, and grainy mist squares blowing across,
both turned with the way the player runs.

### Wowee before and after

| | Stock | Wowee before | Wowee after |
|---|---|---|---|
| Rain and snow mist | SnowMist01 squares, above | none (stock path); the `weathersheets` row drew its own taller distant sheets | stock (`Weather::updateStockMist`), with the rows off and in Classic |
| Sand | grains and WeatherMistGrainy01 | nothing without the `sandstorms` row; with it, Wowee's own dashes | stock grains and mist with the rows off and in Classic |
| `weathersheets` | - | the only mist | adds its 16-70 yd tall sheets on top of the stock mist |
| `sandstorms` | - | the only sand | adds the haze (fog pulled in up to 40%), the foliage and cloth wind and the volumetric mist on top of the stock sand |

Differences left:

- **Ground along the path**: three samples (start, middle, end), lifted to
  `ground + 6` between them; the steep-rise cut of 0x786330 is not done.
- **Sand on the GPU, as stock**: each grain is written once, at birth, into
  a 131072-slot ring (`Weather::grainRing_`, 32 bytes a grain) and moved,
  sized, faded and discarded by `weather_sand.vert` from its
  birth, velocity and life, as `sand.bls` does; one or two instanced draws
  of six vertices a grain. The stock rate is kept: about 67000 alive at
  weatherDensity 2, 107000 at 3 (seven eighths of the ring; the rest keeps
  the slots the frames in flight read clear of new writes).
- **Sand path**: ended where the straight line between the ground at both
  ends is crossed, not cast against the world (WMOs, doodads). The ground
  is read from a 2 yd grid filled on demand (`Weather::sandGround`), as
  tens of thousands of grains a second would be as many ground queries.
- **Point size**: a square of the stock pixel size, at the camera's FOV and
  aspect; no `min(caps, 24)` limit (it never reaches it).
- **Heading**: the player's averaged velocity from its position, and its
  facing below 1 yd/s; movement flag bit 20 is not read.
- `useWeatherShaders 0` (the fixed-function rates) is not offered.

## 3. Breath in the cold

**Stock.** The unit model event `$BTH` (0x48544224) in the unit's event
handler 0x732650:

1. nothing in the glue screens (0x4d37c0 == 1) or without a model;
2. a player at drunk >= 0.5 (0x4f7290, 0x9e2ec4): effect 7, "HARDCODED
   Inebriated Bubbles";
3. else unit +0xa30 bit 0x20 (under water): effect 2, "HARDCODED Breath
   Underwater" (`Particles\Bubbles.mdl`, SpellVisualEffectName 108);
4. else bit 0x40 (cold): effect 3, "HARDCODED Breath Cold"
   (`Particles\ColdBreath.mdl`, SpellVisualEffectName 107);
5. else nothing.

The effect is played by 0x6f9260 at the attachment in 0xa32af4 (17, Breath,
for 2, 3 and 7; 19 if the model has no 17). The ids come from looking the
"HARDCODED ..." names up in SpellVisualEffectName at start-up
(0xca050c, names at 0xada764).

The bits are set by 0x71fa90, every 10 s per unit (`+0x9bc = now + 10000`):
clear 0x60; 0x20 if the liquid's surface over the unit is more than its
height + 5 above it (0x77f1e0); else 0x40 if **0x78f1f0** says the unit's
place is cold: its area - the WMOAreaTable row's AreaTable id when it is in a
map object group (0x77f090, 0x77f1b0), else the terrain's (0x782560) - has
AreaTable flag **0x1**, taking the parent zone's flags instead when the area
lacks flag 0x2 (AreaTable +0x10 flags, +8 parent).

AreaTable flag 0x1 is on Dun Morogh, Winterspring, Razorfen Downs,
Naxxramas, the Storm Peaks, Icecrown, the Howling Fjord, the Borean Tundra,
Wintergrasp, the Frozen Sea, the North Sea, Hrothgar's Landing, the Icecrown
Citadel and Frozen Halls instances, and Acherus - 381 areas with their
subzones. Not on Alterac, Grizzly Hills, Dragonblight, Zul'Drak, Crystalsong
or Sholazar. The timing is the model's: when its animations carry `$BTH`.

**Wowee before.** No stock breath. The `coldbreath` row drew its own puffs
(`FootfallDust`) from attachment 17 every 3.5-5 s, in a hand-made zone list
(Alterac and every Northrend zone but Sholazar always, Grizzly Hills at
night) and wherever it snowed.

**After.** The stock path, rows off: `$BTH` rides the model's per-sequence
events (`pipeline::isM2SoundEventTag`) to
`NPCInteractionCallbackHandler::handleUnitAnimEvent` ->
`Renderer::onUnitBreathEvent`, which asks `Renderer::unitInColdArea` (the
unit's area by the same WMO-floor-or-terrain rule as the player's, its
AreaTable flags through `stock_weather::areaIsCold`, cached 10 s per
instance) and plays `Particles\ColdBreath.m2` at the breath slot
(`SpellVisualSystem::playEffectModel(..., SLOT_BREATH)`). The `coldbreath`
row now only adds the same stock breath wherever snow is falling (the
player's weather past the quarter grade); its own puffs and zone list are
gone.

Differences left: the underwater bubbles (bit 0x20) and the drunk bubbles
(effect 7) are not played; a camera under water skips the breath; units over
100 yd from the camera are skipped; with no attachment 17 the fallback is the
spell system's (20, 11) rather than 19.

## 4. Lightning

**Stock has no lightning flash.** The sky has the code for one: DayNight's
update (0x7f16f0, from 0x7816f0) tests bit 0 of 0xd38184 and, while it is
set, computes `f = 1 / f(...)` over 0xd38aa0..0xd38a98 and has 0x7ee9b0 scale
each sky colour by `0xd38a88 = 1 - f` and blend toward 0xd38d44 (the dome's
0x7f0530 tests the same bit). But nothing in the binary sets that bit:
0xd38184 is zeroed at start-up (0x7f2790) and only ever cleared (0x7ecf00,
0x7f16f0), and the flash colour 0xd38d44 is zero-initialised (0x9d0720) and
never written. No other code reads a thunderstorm: MapWeather has no
lightning, Weather.dbc 86 is a plain rain row (EffectType 1) whose ambience
(4994) carries the thunder sounds, and `Lightning.cpp` (0x9aafb0, CLightning)
is the spell chain-lightning effect, drawn from 0x7fca30.

So the stock dome never flashes, and Wowee's stock path draws none. The
`stormlightning` row's land and haze flash stays an enhancement; it still
does not light the dome or the clouds.

## 5. Checked in game and measured

Lab AzerothCore, the weather preview row, every enhancement row off unless
named, a 1251x986 window on the Iris Xe (Jered's machine under load):

- Gadgetzan (heavy rain) and K3 (heavy snow): the weather is drawn in the
  streets (`enabled 1` in `WOWEE_WEATHER_LOG`); before, nothing.
- Ironforge, the Great Forge, rain forced: not drawn (sealed interior,
  `enabled 0`), still simulated.
- Silithus sandstorm, rows off: grains (5000-5600 alive, many cut short by
  the dunes) and 126-128 grainy mist squares; Goldshire heavy rain and
  Kharanos heavy snow: 100-128 mist squares.
- Kharanos, dry, camera at the face: the ColdBreath puff at the player's
  mouth on its $BTH frames.
- Cost (`WOWEE_FRAME_PROFILE=1`, main-thread stage
  `update/pre/sky+weather+water+fx`, 10 s windows, Silithus): clear 0.012
  ms/frame; sandstorm 0.22-0.24; heavy rain 0.31-0.35 and heavy snow
  0.27-0.28 (both with their 3000 / 2100 drops and flakes). The GPU's
  last-frame figure (13-20 ms, vsync-bound and loaded) did not separate.

Second round (GPU sand, Weather.dbc, sky order), the same setup: Silithus
sandstorm with the rows off reached 56000 grains a few seconds after the
full grade (the stock rate at Weather Detail 3, less those the dunes end
at birth), the weather stage 0.23 ms/frame with them (spawning and the
ground grid; the GPU moves them); `Weather.dbc: 32 rows` read; Goldshire at
23:00 keeps its stars under the moon; the Storm Peaks' sky as before;
`IsIndoors()` false in Gadgetzan's and Stormwind's streets.

## 6. Weather.dbc

**Stock** (0x7846a0, from the SMSG_WEATHER handler 0x526530 with the row
0x65c290 found): the row's EffectType (+8) picks the emitter (0x78d170 makes
the rain, snow or sand one); its light scale (+0xc) is eased into weather
+0x28 and multiplies the storm light (0x784850); its colour (+0x10..0x18) is
packed with alpha 1 into weather +0x34 (0x48bd20) and set as the vertex
colour of the rain (0x78a640, c7), the splashes (0x78a030, c7) and the snow
(0x78aee0, c8) - not the sand or the mist, which take the fog colour; its
EffectTexture (+0x1c) is copied into weather +0x38 when it names one, else
the kind's own (0x783b90: RainDrop01, SnowFlake01), and the rain and snow
emitters load their particle texture from there (0x78ca90, 0x78cd30).

3.3.5's rows: 83 and 96-98 SnowFlake01, 90 `Particles\LeafBrown.blp` (the
server's black rain), 100 and 108 `RainDropRed01.blp`; 104 SnowMist01 on a
sand row (unused: sand reads no texture); colours all white but 103
(1, 0.5, 0) and 104 (0, 0, 0), both sand; light scale 0 on 103 and 105-107.
State 106 (the server's "black snow") is a sand row.

**Wowee now.** `Renderer::weatherRowFor` reads Weather.dbc once (8 fields;
another width falls back to the state number). The kind is the row's
EffectType (`stock_weather::effectFor`; 86 stays `Type::STORM` for the
enhancement rows and is drawn as the stock rain, its own slant and gusts
only with the "Rain and snow in the wind" row); the storm light is the grade
times the row's scale (not eased over the stock 5 s); `Weather::setRowLook`
tints the rain, splashes and snow by the colour and draws the row's texture,
loaded once into one of four spare sprite slots.

## 7. Sky order

**Stock** (0x7f09b0): the stars (stars.mdl), the sun and moons, the additive
dome and the clouds; then the LightSkybox models at their weights; the
glares later. **Wowee before**: the dome, every sky model (the stars among
them, as a LightSkybox-like layer), then the sun, moons and clouds - so a
zone's sky model was drawn under the clouds and sun. **Now**: the stars
have their own sky-mode M2Renderer (`Renderer::skyStarsRenderer_`), drawn
right after the dome; then the sun, moons and clouds; then the other sky
models. The dome is opaque here and drawn first, the stars and the sprites
add to it, which comes to what stock's additive dome over them gives.

## 8. ScreenEffect 2 and 3, the underwater glow wave (traced, not drawn)

0x4f7020 picks the full-screen effect by ScreenEffect.dbc's Effect (+8):

- **2, nether** ("Invis Screen Effect" 81, 121; "Echo of Ymiron" 221;
  "World of Shadows" 342): 0x7ed870(0.7, 150, colour, 0) saves the current
  fog (0xd38c1c, 0xd38bf4, 0xd38c20, 0xd38c24) and puts in its place a fog
  colour 0xff4c4c63 (0xffffffff when 0xd45774 +0x30 is set; not identified) with the end 150 yd and the start 0.7 of it; leaving it
  restores them (0x7ed820). The chain (0x7ea470, cvar `ffxNetherWorld`):
  `ffxnetherblur` (four taps averaged, x0.25) into the quarter target, then
  `ffxnethercombine`: `m = scene * 0.5 + blur; g = dot(m, 1/6);
  m = m * 0.5 + g; out = scene + c0.w * (m * c0.rgb - scene)`.
- **3, special** (21, 41, 43, 61, ...): the row's colour (+0xc) and
  parameters (+0x10..) go to the special chain's vtable +0x10 (0x7ea5f0,
  cvar `ffxSpecial`, "full screen test effect"); only with shaders (0x682d20).
- Both then set the row's light slot (+0x1c, 0x7ecec0) and sound (+0x20,
  +0x24, 0x4c8fa0) as types 0 and 1 do.
- **Under liquid** the glow chain's last pass is `ffxglowwave`: texture 0 is
  a wave map whose xy, scaled by c0 and c1, offsets the scene's and the
  blur's coordinates before the glow combine
  (`lerp(scene, blur, z) + blur^2 * w`).

Wowee draws none of these yet (the nether fog override is the easiest
start: it needs only the screen effect's type from the game handler).

## 9. Not done here

- **Weather.dbc**: the light scale is applied at once, not eased over 5 s.
- **ScreenEffect 2 and 3** and the **underwater glow wave**: traced above,
  not drawn.
