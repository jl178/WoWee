# the reference client: combat, targeting and animation feel

What the official "the reference client" client (build `WOW-70862patch1.60.1`,
product `wowf`, launched 2026-11-04: the 1.12 game on Blizzard's current
engine) does differently from stock 3.3.5a in combat, targeting and
animation, how that was established, and what Wowee took from it. Read with
`animation.md`, `combat-targeting-movement.md`, `unit-motion.md`,
`input-timing.md` and `world-ui.md` (the stock side).

## 0. Evidence and its limits

- **The code is not readable from disk.** `Wow.exe` (80.6 MB) has its whole
  `.text` section (61 MB) encrypted: every 4 MB chunk has an entropy of 8.00
  bits/byte, and a scan for RIP-relative `lea`/`mov` found 23 thousand hits
  where a plain x64 binary of that size has millions, none of them to the
  CVar names. Disassembly would need a dump of the running process on
  Jered's PC, which was out of reach. So nothing below quotes Enhanced's
  machine code; where a rule needs code (Tab ordering weights, the
  directional damage numbers' curve, blend-tree weights) it is marked as not
  known.
- **What is readable**: `.rdata` (CVar names with their descriptions, RTTI
  type names, Lua API usage strings), the game data in CASC (models, DB2s,
  textures) and the whole interface (Lua/XML), Jered's `Config.wtf`, and his
  Moonlight stream (one capture).
- **How the data was read.** The share has the CASC store but serves no
  byte ranges, so files were found through Enhanced's own build config
  (`Data/config/5a/da/5ada954...`), encoding and root (`wowf` root, 1.25 M
  files), the local `.idx` files, and fetched by byte range from the CDN
  archives the CDN config's archive-group index names. Tools in the session
  scratchpad (`fc-scripts/casc.py`, `addon.py`, `pget.py`, `m2seq.py`,
  `m2keys.py`, `blp.py`, `xr.py`); paths from the community listfile.
- **Enhanced's interface is the current "Mainline" one** with a `Camelot`
  game type: tocs carry `[AllowLoadGameType camelot]`, and `Camelot\`
  overrides sit beside `Mainline\` ones (nameplates, settings). `Camelot`
  is Enhanced's internal name (`wow_camelot` telemetry, `CamelotCombat`
  trait config type).

## 1. Animation

### Models: the HD characters, same timing as 3.3.5

`character/human/male/humanmale.m2` (FileDataID 119940) and
`humanmale_hd.m2` (1011653) are the same file in Enhanced: the HD model, M2
version 272, 416 sequences (3.3.5: 156), 215 bones (138). The sequences
that matter in a fight have the 3.3.5 timing exactly (`m2seq.py`):

| Sequence | 3.3.5 dur / speed / blend | Enhanced dur / speed / blendIn |
|---|---|---|
| Stand 0 | 2667 ms / - / **500** | 2667 / - / **150** |
| Walk 4 | 1000 / 2.50 / 150 | 1000 / 2.50 / 150 |
| Run 5 | 667 / 6.94 / 150 | 667 / 6.94 / 150 |
| Attack1H 17, 2H 18, Unarmed 16 | 1000-1333 / 150 | the same / 150 |
| SpellCastDirected 53, SpellCastOmni 54, BattleRoar 55 | 1000 / 150 | 1000 / 150 |
| Special1H 57 (Heroic Strike and kin) | 1000 / 150 | 1000 / **75** |
| Sprint 143 | 666 / - | 666 / 11.0 |

The 3.3.5 `blendTime` (u32 at +0x1C) became `blendTimeIn`/`blendTimeOut`
(two u16); every Enhanced sequence read has `blendTimeOut` 0. So the
cross-fade lengths are the 3.3.5 ones; the change is that Stand fades in
over 150 ms rather than 500.

What is denser is the animation itself (`m2keys.py`, rotation keys of the
bones a sequence moves):

| | 3.3.5 HumanMale | Enhanced HumanMale |
|---|---|---|
| Run: bones animated | 53 | 87 |
| Run: keys per bone per second | 18.6 | 24.3 |
| Stand: bones animated | 25 | 121 |
| Stand: keys per bone per second | 11.4 | 16.1 |
| Attack1H: bones animated | 31 | 60 |

The HD rigs move fingers, face, cloth and armour bones the 3.3.5 rigs do not
have, and key them more densely. That is art, not engine: Wowee draws the
3.3.5 models and cannot have it. The nearest it has are E7 (capes, tails and
hair, secondary motion) and E9 (feet on the ground).

### Blending in the engine

The strings name the systems: `Animation Blend Tree`, `BlendAxisTypes`,
`Node [%d] has no weights assigned for blending.`, `AnimKitBlendType`,
`blendOverrideMS`, `blendOutOverrideMS`, `blendWeightOverride`,
`noFacingBlend`, `GlideEventBlendTimes` (a DB2). The current engine mixes
any number of animations through a tree with weights per node, where 3.3.5's
bone state has one primary and one secondary (0x826C40, `animation.md`):
there, a switch during a fade either keeps the outgoing sequence and swaps
the incoming one at once (outgoing weight above 0.5) or drops the outgoing
one. Either drop is a pop of up to half the pose, and it happens exactly in
a fight: a swing cut by a flinch, a parry, a cast or a run, all within the
150 ms of the previous fade.

**Ported (E36, row `poseblend`, "Smoother animation changes", Combat page,
"Feel (the reference client)", Enhanced set on, stock off).** `CharacterRenderer::beginSequenceBlend`: a play that
starts while a fade is running fades from the pose as last drawn
(`localPose`, both sides of the running fade in it) over the new sequence's
blendTime. A one-slot stand-in for the tree: nothing pops, the fading pose
is held rather than played on for those 150 ms. Off, the stock swap.
A sequence with blendTime 0 still cuts.

### Turning, other units' motion

- `movementMaxEventEarlyTime` / `movementMaxEventLateTime`: "Used as part of
  the system that adjusts the timing on movement messages to try and smooth
  out jitter (our own and other players). This CVar sets the upper/lower
  bound of the adjustments the system can make." The same scheme as the
  3.3.5 client's (packets applied on the sender's own spacing, a jitter
  buffer, the delay clamped to [-500, +1000] ms; `unit-motion.md` section 2),
  with its clamp made a setting. Wowee already does the stock scheme
  (`RemoteMover`) and E10 glides what corrections are left; the defaults of
  the two CVars are in the encrypted code, so nothing was changed.
- `unitsLookAtPlayers`, `noHeadMove`, `noFacingBlend`, `TurnHead`-style
  strings: the current engine turns heads; 3.3.5 has the look twist Wowee
  already draws (`body_facing.hpp`). Not ported: the rules are code.

## 2. Targeting

- **Selection circle: the same art.** Enhanced's
  `textures/unitselecttexture.blp` (186224) is a 256x256 DXT5 of a white disk
  whose alpha rises from 0 at a quarter of the radius to 34 at 0.8 and peaks
  at 77 on the rim (0.85); 3.3.5's is a paletted one with the same profile
  (rim 95, inner 51). Jered's stream shows it under the target, coloured by
  reaction, as in 3.3.5. Nothing to port.
- **Outlines.** `OutlineEngineMode` (Jered: 2), `graphicsOutlineMode`,
  `outlineMouseOverFadeDuration`, `outlineSelectionFadeDuration`,
  `targetHighlightAlwaysShown` ("If enabled, targeted units have a highlight
  applied regardless of Outline Mode settings"), the `OutlineEffect` DB2.
  Enhanced's Camelot settings override `RegisterOutlineSettings` with an empty
  function: the outline options are hidden, and the stream shows no outline
  on the selected target. Not ported.
- **Soft targeting and Tab priority.** 34 `SoftTarget*` and 22
  `TargetPriority*` CVars (none in 3.3.5): soft enemy/friend/interact
  targets with icons and nameplates, the interact key (`INTERACTTARGET`),
  Tab ordered by a priority over an in-view test with the screen's edges
  pulled in (`TargetPriorityFrustumPullIn*`), combat and PvP locks, a 
  "values bank". Enhanced's settings default the KBM forms off: "Enable
  Action Targeting" sets `softTargetEnemy` 3 (any) or 1 (gamepad) and
  defaults to false; "Enable Interact Key" the same for
  `softTargetInteract` (`Blizzard_SettingsDefinitions_Frame/Combat.lua`,
  `Controls.lua`). With the defaults a keyboard player has the 3.3.5-like
  hard target and Tab. The priority weights are code. Not ported.
- Nameplates are the current Mainline ones with Camelot constants (classic
  style sizes `CLASSIC_NAME_PLATE_WIDTH` 152, a centred name above the bar,
  outlined font). Interface work, not ported here.

## 3. Combat feedback and input

### Spell queue window

`SpellQueueWindow`: "Sets how early you can pre-activate/queue a
spell/ability. (In Milliseconds)", with `GetSpellQueueWindow` in the Lua
API. 3.3.5 has neither (`input-timing.md`: a press during the GCD is
refused with "Spell is not ready yet", one during a cast is sent and refused
by the server). The current client's default is 400 ms (its code; the
interface never sets it).

**Ported (E37, row `spellqueue`, "Spell queue window (ms)", 0-400, Enhanced
set 400, stock 0).** `SpellHandler`:

- a press refused locally only by the GCD or the spell's own cooldown, with
  at most the window left (`notReadyWaitSec`), is kept, one at a time, a
  newer press replacing it, and sent by `updateTimers` the frame the spell
  is ready, at the target it had when pressed;
- a press during a cast with at most the window left is kept and sent at the
  cast's end (the existing cast queue, which read only the CVar);
- forgotten a second after it should have gone, on a world change, or when
  the window goes to 0.

0 reads the `SpellQueueWindow` CVar as before (default 0: stock).

The server side: AzerothCore has a queue of its own (`SpellQueue.Enabled =
1`, `SpellQueue.Window = 400` in worldserver.conf.dist;
`Player::CanRequestSpellCast` keeps a CMSG_CAST_SPELL that arrives with at
most 400 ms of GCD, cooldown or cast left and runs it when it can). A stock
client never uses it for the GCD: it refuses the press locally and sends
nothing. With the window on, Wowee sends at its own GCD's end; the server's
GCD began half a round trip later and the packet arrives half a round trip
later, so it is due on arrival, and a few milliseconds of jitter land in the
server's queue rather than in a refusal.

### Swing timer

`Blizzard_SwingTimer` (`## AllowLoadGameType: camelot`: Enhanced only), with
`C_SwingTimer` (`IsTargetWithinSwingRange`, `EnableRangeCheck`) and the
events `PLAYER_SWING(swingDuration, swingType)` and
`PLAYER_SWING_RANGE_UPDATE`. A bar per hand (main, off, ranged) in the
bottom managed frames over the action bars, Edit Mode visibility Always / In
combat / Hidden, CVar `showSwingTimer`. On each swing of that hand the bar
starts from empty and fills to its end over `swingDuration`, a pip at the
fill's end, the type on the left and the seconds left on the right ("%.1f");
at the end it clears. Out of melee range of the target the frame, bar and
labels go to 0.4 alpha and the labels red - but not while a swing runs.
Atlas `uiswingtimerbar2xc60.blp`: dark frame (20,19,17 at 60%), brown border
(86,66,43), main hand fill a light stone (186,175,156 to 112,100,97), off
hand blue-grey (105,113,118), ranged green-grey (135,144,128).

**Ported (E38, row `swingtimer`, "Swing timer", Enhanced set on, stock
off).** `CombatHandler` keeps the player's last swing per hand from
`SMSG_ATTACKERSTATEUPDATE` (off hand by `HITINFO_OFFHAND` 0x4) and that
hand's `UNIT_FIELD_BASEATTACKTIME`; `GameScreen::renderSwingTimer` draws the
bars while in combat with the colours above, 200 x 13 interface units, 125
units up. Not done: the ranged bar, Edit Mode placement, a swing reset by an
on-next-swing ability (Heroic Strike's blow comes as a spell).

### Floating combat text

`classicStyleWorldText`, `WorldTextScale_v2`, `WorldTextMinAlpha_v2`,
`WorldTextRampDuration_v2`, `floatingCombatTextCombatDamageDirectionalScale_v2`
("Directional damage numbers movement scale (0 = no directional numbers)")
and `..._DirectionalOffset_v2`: the current engine's numbers can fly
outward from the hit. Enhanced's combat settings offer the float mode only
when `classicStyleWorldText` is 0; its default and the curves are code.
Not ported (3.3.5's world text is what `world-ui.md` section 5 describes and
Wowee draws).

## 4. Frame pacing

`M2UseThreads` ("multithread model animations"), `GxMaxFrameLatency`,
`LowLatencyMinInterval` and Reflex strings: animation on worker threads and
a bounded frame queue. Wowee already poses characters on worker threads
(`CharacterRenderer`, `M2Renderer`) and presents FIFO. Measured below.

## 5. Measurements

Lab server, level 10 human mage, Arcane Explosion (instant, 1.5 s GCD),
`enhanced-combat/test/fc1.lua` in the session scratchpad, 1280x720:

| Case | Stock (`spellqueue` 0) | Enhanced (`spellqueue` 400) |
|---|---|---|
| One press 1.2 s into the GCD, nothing after | refused ("Spell is not ready yet."), never cast | sent 1.511 s after the first (11 ms after the GCD's end) |
| A press every 250 ms for 9 s | 6 casts, mean gap 1.516 s | 7 casts, mean gap 1.509 s |
| A press with more than 400 ms left | refused | refused (the same message) |

Holding a key or spamming a button already gets close to the GCD's end;
the window is what makes one early press count, which is how a rotation
is played. The swing timer was seen filling and counting down ("Main Hand
0.4") over a young wolf. Frame rate in the fight 53.7-59.5 fps (vsync 60)
with all three on; nothing measurable is added (two rectangles, a pose
copy per interrupted fade, one compare per frame for the queue).

## 6. Status

| # | Item | Where | Toggle |
|---|---|---|---|
| E36 | Fade from the drawn pose during a fade | `CharacterRenderer::beginSequenceBlend` | `poseblend` |
| E37 | Spell queue window, GCD and cooldowns too | `SpellHandler::castSpell`, `updateTimers` | `spellqueue` |
| E38 | Swing timer | `CombatHandler::getPlayerSwing`, `GameScreen::renderSwingTimer` | `swingtimer` |

The Enhanced values of these rows (and E10, `remotesmoothing`, which is the
other units' half of the same feel) are `kEnhancedCombatValues` in
`include/ui/enhancement_rows.hpp`, for the "Enhanced" preset.

Not ported, for want of the code: Tab priority targeting and soft targets,
the interact key, directional damage numbers, outlines, head look-at. Not
portable: the HD models' denser animation.
