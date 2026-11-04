# Input timing: from a key to a cast, a target, a step (stock 3.3.5a, build 12340)

How quickly the stock client turns a key press or a click into a packet and
into feedback on the screen, read from `Wow.exe` 12340 with Ghidra, and what
Wowee did at each step, measured with `WOWEE_INPUT_TRACE=1`
(`include/core/input_trace.hpp`). The spell presentation itself is in
`spell-cast-pipeline.md`, melee in `combat-targeting-movement.md`, movement in
`movement-camera.md`; this file is only about *when*.

## 1. Stock

### Action buttons fire on the key's release

- `Interface\FrameXML\Bindings.xml`: `ACTIONBUTTON1..12`,
  `MULTIACTIONBAR1..4BUTTONn` and `BONUSACTIONBUTTONn` are `runOnUp="true"`.
  The script calls `ActionButtonDown(id)` on the press and
  `ActionButtonUp(id)` on the release (`ActionButton.lua`,
  `MultiActionBars.lua`).
- `ActionButtonDown` only sets the button `PUSHED` (the pressed texture). 
  `ActionButtonUp` sets it `NORMAL` and calls
  `SecureActionButton_OnClick(button, "LeftButton")` -> `UseAction`. **The
  cast goes out on the key-up.**
- There is no `ActionButtonUseKeyDown` CVar in 3.3.5a: the string is not in
  `Wow.exe` and no FrameXML file reads it (it arrived in a later client).
  Neither is there `SpellQueueWindow`.
- `SHAPESHIFTBUTTONn` and `ACTIONPAGEn` have no `runOnUp`: they act on the
  press.
- A binding runs from the key event itself, inside the frame's input
  processing, so `UseAction` -> `CMSG_CAST_SPELL` happens at the top of the
  frame that sees the event, before the world is updated or drawn.

### UseAction -> the cast (Lua `UseAction` 0x5AC000 -> 0x5ABBC0)

- 0x5ABBC0: a held cursor item is placed instead (0x5AB120). A spell action
  goes through 0x80DA80:
  - the spell is `Attack` (0x6D71E0): toggle the swing (0x72C2B0);
  - **the spell is the running auto-repeat (`0xD397D0`): turn it off**
    (0x807560(1): `CMSG_CANCEL_AUTO_REPEAT_SPELL` 0x26D) - Auto Shot and
    Shoot are toggles;
  - the spell waiting for a ground point: put it away (0x809A60);
  - otherwise cast (0x80DA40 -> 0x80CCE0).
- A macro action runs the macro (0x5D4210) with the `GAMEABILITYACTIVATE`
  sound.
- `CastSpellByName("!name")` goes straight to 0x80DA40, skipping the toggles,
  which is why `/cast !Auto Shot` never turns Auto Shot off.

### The checks before the packet (0x80CCE0 -> 0x739650)

- `0x739650` asks the cooldown list (0x809000 -> 0x807980). An entry holds
  the spell's own cooldown (+0x10 start, +0x14 duration), its category's
  (+0x18, +0x1C, +0x20) and **the global cooldown of its
  StartRecoveryCategory (+0x28, +0x2C)**. Any of them running: the cast is
  refused with `SPELL_FAILED_NOT_READY` (67), nothing is sent.
- **No check for a cast already running.** Pressing another spell while a
  cast bar fills sends it; the server answers `SMSG_CAST_FAILED`
  `SPELL_FAILED_SPELL_IN_PROGRESS` ("Another action is in progress"). During a
  channel AzerothCore starts the new spell and the channel ends. There is no
  queue: a press before the cast ends is simply refused.
- The client checks only one in-flight case: the same spell already pending
  (0x805F60) and on cooldown fails silently.
- Standing up (sitting, `Attributes & 0x08000000` clear) and dismounting are
  done at the send (0x80AC90 -> 0x71A360, 0x7412E0), after the cooldown check.
- **Facing:** nothing is turned and no movement packet is sent with a cast.
  The server has the facing of the last movement packet (mouse steering
  sends one every 0.1 rad) and refuses a target behind with
  `SPELL_FAILED_UNIT_NOT_INFRONT`.

### Refusals: once per burst (0x808200)

Every cast failure, the client's own and `SMSG_CAST_FAILED` (0x809AF0) alike,
goes through 0x808200. It keeps the last spell (`0xD397C4`), result
(`0xD397C0`) and time (`0xD397BC`, `OsGetAsyncTimeMs`): **the same spell
refused for the same reason within 3000 ms of its previous refusal shows
nothing.** The time is updated on every refusal, so a mashed key reports once
and then stays quiet until the player has left it alone for 3 s.

What is shown (0x807F10 -> 0x5216F0, the game error table at 0xAC8AF0):

| Result | Shown as | Voice |
|---|---|---|
| NOT_READY (67), spell | ERR_SPELL_COOLDOWN "Spell is not ready yet." | VocalUISounds 12 |
| NOT_READY, `Attributes & 0x10` (ability) | ERR_ABILITY_COOLDOWN "Ability is not ready yet." | 50 |
| NOT_READY / ITEM_NOT_READY, Spell Category 4 or 9 | ERR_POTION_COOLDOWN | 47 |
| ... Category 10 or 11 | ERR_FOOD_COOLDOWN | 7 |
| ITEM_NOT_READY (45) | ERR_ITEM_COOLDOWN | 5 |
| anything else | ERR_SPELL_FAILED_S with the SPELL_FAILED_* text | none |

`SPELL_FAILED_NOT_READY`'s own string "Not yet recovered" is never what the
client prints for it.

### The send (0x80AC90)

In this order, all before any reply:

1. `CMSG_CAST_SPELL` (0x12E) built and sent.
2. `UNIT_SPELLCAST_SENT` ("player", spell, rank, target).
3. 0x805330(spell, castTime 0, predicted 1): **the precast kit and its pose
   start on the caster now**, unless the caster already has a cast (+0xA60).
   `SMSG_SPELL_START` finds them playing and does not start them again.
4. 0x805D70: **the global cooldown starts now**, from Spell.dbc
   StartRecoveryCategory/StartRecoveryTime (`ACTIONBAR_UPDATE_COOLDOWN`), see
   below.

The cast bar does not start here: `UNIT_SPELLCAST_START` (event 0x144) comes
from 0x805330 only with the server's cast time, i.e. from
`SMSG_SPELL_START`.

### The global cooldown (0x805D70)

- None when StartRecoveryCategory and StartRecoveryTime are both 0 (Divine
  Protection, Avenging Wrath, Auto Shot, Auto Attack), or the time is 0
  (Shoot).
- The time, after SPELLMOD_GLOBAL_COOLDOWN (21) talents.
- Category 133 with 1500 ms, DmgClass not 2 (melee) or 3 (ranged), and
  `Attributes & 0x12` clear (not RANGED, not ABILITY): times
  `UNIT_MOD_CAST_SPEED` (unit field 0x4A, wire index 80), `round(x - 0.5)`
  (constant 0xAF523C = 0.5, x87 round-to-even), held to 1000..1500 ms. So
  spell haste shortens a caster's GCD on the client before the server says
  anything; rogues' and warriors' do not move.

### Clicks (CGInputControl 0x5FA450 -> 0x5F9600 -> 0x4F7880)

A world click is decided on the button's **release**: not a click once held
800 ms; a press whose pointer moved (8 units, 0xA1DE4C) counts only if it was
released within 200 ms. Then left -> 0x5274F0 -> `CGGameUI::Target` (0x524BF0:
`CMSG_SET_SELECTION` and `PLAYER_TARGET_CHANGED` at once), right -> 0x5277B0.

### SpellStopCasting (0x809EA0)

Returns 1 or nil. In order: with a current cast or channel (0x7FFDE0,
`+0xA60`) it is cancelled (0x809A80); otherwise a running auto-repeat is
cancelled (0x807560). (A first branch, 0x800BA0 on `player +0x1858 & 4`,
calls 0x6E1660; what sets that bit was not traced.)

### Melee and movement

- The swing animation and the attacker's voice play when
  `SMSG_ATTACKERSTATEUPDATE` arrives; nothing is predicted; the blow's
  sounds and the victim's reaction wait for the swing's `$CPP`/`$CAH`/`$CSS`
  events (`combat-targeting-movement.md` section 2). `CMSG_ATTACKSWING` and the
  drawn weapons/Ready stance go out on the click.
- A movement key changes CGInputControl's bits and the start opcode is sent
  from the key event; a heartbeat follows 500 ms after the last movement
  packet of any kind; facing goes out when it differs by 0.1 rad
  (`movement-camera.md` section 9). The player's own position is never
  interpolated: it is simulated locally and the server corrects only by
  teleport, knockback or speed/root changes.
- Click-to-move is the `autoInteract` CVar, 0 by default.

## 2. Wowee before

Measured on the lab server (local, RTT ~1 ms), paladin Wptestb, a
`WOWEE_DEBUG_LUA` script pressing keys through `WoweeDebug_PushKey`, ~55 fps
(18 ms frames). Key events pushed from a script wait for the next frame's
pump, which is the same wait a real key has on average half of; the numbers
below separate that wait ("event age") from what the client does after the
pump.

| Step | Wowee before | Stock |
|---|---|---|
| Action-bar key (ACTIONBUTTONn, MULTIACTIONBAR1BUTTONn) | cast on the **press**, from GameScreen's poll inside the UI draw; the main bar on the press but the other bars (Lua bindings) on the release | pushed on the press, cast on the release |
| Pump -> `CMSG_CAST_SPELL` | **13.3-20.6 ms** later, in the same frame (0 frames), after the world update and the world draw (`renderer->beginFrame`/`renderWorld` run before `uiManager->render`). Instant spell: 20.6 ms; Holy Light 14.1 ms and 8.6 ms (frames 18-20 ms) | at the event, before the frame's update |
| Button highlight on the press | none: the button never showed `PUSHED` for a key | `PUSHED` on the press |
| Press during the GCD | sent; the server answered `SMSG_CAST_FAILED` 17-35 ms (2-3 frames) later with "Not yet recovered", every press | refused by the client, "Spell is not ready yet.", once per burst, nothing sent |
| Press during a cast (after its GCD), on or off the GCD | dropped silently (or queued if within 400 ms of the end) | sent; "Another action is in progress" from the server; a channel is replaced |
| GCD swirl | started with the send (`ACTIONBAR_UPDATE_COOLDOWN` 0.05 ms after `CMSG_CAST_SPELL`), always **1.5 s**, also for spells off the GCD (Divine Protection, Avenging Wrath, Auto Shot) | with the send, the spell's StartRecoveryTime, hasted, none off the GCD |
| Cast bar | `UNIT_SPELLCAST_START` on `SMSG_SPELL_START`: 16-18 ms after the send here, 300-500 ms when the lab server is busy | the same |
| Precast kit and pose | on `SMSG_SPELL_START` | with the send |
| Movement packet with a targeted cast | an extra `MSG_MOVE_HEARTBEAT` before every `CMSG_CAST_SPELL` with a target | none |
| Auto Shot / Shoot pressed again | sent again (the server ignores it) | turns it off |
| `SpellStopCasting()` | cancelled a cast only; returned true/false | cast, else auto-repeat; 1 or nil |
| World click | a click whenever the pointer moved less than the drag threshold, however long held | release within 800 ms; moved only within 200 ms |
| Tab target (`TARGETNEARESTENEMY`) | `CMSG_SET_SELECTION` 7.2-21.8 ms after the pump, same frame (GameScreen) | at the event |
| `MoveForwardStart()` -> `MSG_MOVE_START_FORWARD` | inside the call, same ms | the same |
| Heartbeat | 503-505 ms after the last movement packet of any kind (START 0, SET_FACING +8, heartbeats +504, +1008, +1510) | 500 ms after the last packet |

The server's own reply time, which neither client controls: 16-34 ms from
`CMSG_CAST_SPELL` to `SMSG_SPELL_START` on a quiet lab server, 150-510 ms while
its 2000 bots were busy - which is exactly when a reply-driven precast is
seen to lag.

## 3. Changes

- **Action-bar keys run their Bindings.xml scripts in the input pump**: a key
  or mouse button bound to `ACTIONBUTTONn`, `MULTIACTIONBAR1BUTTONn` or
  `SHAPESHIFTBUTTONn` runs the interface's script first
  (`LuaEngine::interfaceRunsFirst` in `dispatchBindingInput`): the press
  pushes the button, the release casts, at the top of the frame. GameScreen's
  poll still casts them when no interface binding took the key, and a pad
  button keeps the client's own path (cast on the press, with the
  shoulder-button page fallback).
  `ActionButtonUseKeyDown` (a later client's CVar, default 0) makes the
  press cast instead: down and up run back to back (`LuaEngine::
  dispatchBindingInput`). Both new CVars answer GetCVar with 0.
- **Cooldown and GCD checked before sending** (`SpellHandler::
  isSpellNotReady`, before the dismount and the ground cursor): refused
  with the table's line (`ERR_SPELL_COOLDOWN`/`ERR_ABILITY_COOLDOWN`/
  potion/food/item) and its voice; `UseAction` no longer drops a cooling
  spell silently.
- **0x808200's throttle** for every cast failure, local or from the server
  (`stock_cast::CastErrorThrottle`): same spell and result within 3 s of the
  last says nothing. The "routine rejection" rule that silenced the voice of
  every cooldown/range/power refusal is gone; the throttle does that job the
  stock way.
- **No spell queue by default.** A press during a cast is sent; the refusal of
  that press (a different cast count) no longer tears down the running cast,
  its bar, its build-up or its GCD (`handleCastFailed`). `SpellQueueWindow`
  (ms, default 0) brings the old queue back as an option. A gathering timer
  still swallows presses (no server cast to answer).
- **GCD from the spell** (`stock_cast::predictedGcdMs`, `startPredictedGcd`):
  StartRecoveryCategory/Time, hasted by `UNIT_MOD_CAST_SPEED` (field 80) for
  the 1.5 s magic GCD and held to 1.0-1.5 s, none for spells off the GCD.
  The cooldown check uses the GCD's category.
- **Precast kit and pose with the send** (`castSpell` -> `triggerCastVisual`
  and the cast animation; `handleSpellStart` skips them for the predicted
  spell), not over a running cast.
- **No movement packet with a cast** unless the debug auto-face turned the
  player.
- **Auto Shot/Shoot toggle off** when pressed while repeating
  (`cancelAutoRepeat`); `CastSpellByName("!Auto Shot")` does not.
- **SpellStopCasting** cancels the cast, else the auto-repeat; returns 1/nil.
- **World clicks** use 0x5F9600's 800 ms / 200 ms rule
  (`stock_cast::isWorldClick`, left and right).
- `WOWEE_INPUT_TRACE=1`: the timing trace these numbers come from
  (`include/core/input_trace.hpp`; hooks in the pump, `WorldSocket::send`,
  packet dispatch and `LuaEngine::fireEvent`).

Tests: `tests/test_stock_cast_timing.cpp` (GCD, haste, throttle, not-ready
line, click rule) and a case in `tests/test_input_integration.cpp` (action
keys are interface bindings: down then up).

## 4. Wowee after

Same script, same server, after the change:

| Step | Wowee after |
|---|---|
| Key press | button `PUSHED` (read back by the script on the next frame); nothing sent |
| Key release -> `CMSG_CAST_SPELL` | **0.12-0.20 ms** after the pump (instant 0.13, Holy Light 0.12, presses during a cast 0.17-0.20), 0 frames: the top of the frame, before the world update |
| `UNIT_SPELLCAST_SENT`, GCD swirl | 0.07 / 0.15 ms after the packet |
| Precast kit | in the same call as the send (`SpellVisual: build-up spell=635` logged in the same ms as `CMSG_CAST_SPELL`); `SMSG_SPELL_START` 508 ms later (busy server) did not start it again |
| Press during the GCD | "Spell is not ready yet." 3.5 ms after the pump, nothing sent; the second press 0.3 s later said nothing (throttle) |
| Press during a cast (on GCD, then off GCD) | both sent; two "Another action is in progress" when the server answered; Holy Light went on to `UNIT_SPELLCAST_SUCCEEDED` |
| `SpellStopCasting()` | cancelled the cast, returned 1 |
| Tab target | unchanged, 7.2 ms after the pump in this run (frame-rate dependent) |

The time a press now waits is the time it is held: a release is acted on at
the top of the next frame, as stock acts on it from the event.

## 5. Gaps

- **Pad buttons** bound to the action bar still cast on the press, from
  GameScreen's poll in the UI draw.
- **Key-up casting is the stock timing and feels slower** than the press
  casting Wowee had, by however long the key is held (here 80-120 ms).
  `/console ActionButtonUseKeyDown 1` (SetCVar) restores press casting.
- **SPELLMOD_GLOBAL_COOLDOWN** talents are not applied to the predicted GCD;
  the server's own GCD (`SMSG_SPELL_COOLDOWN` spell 0) still corrects it.
  Haste is read only on the 3.3.5a field layout.
- **Category cooldowns** are not part of the client-side check; only the
  spell's own cooldown and the GCD are. The server still refuses those.
- **Tab target and world clicks** are still handled by GameScreen during the
  UI draw, after the world draw: the packet goes out in the same frame but
  5-20 ms after the pump, where stock sends it from the event. The frame the
  target frame updates in is the same.
- **Moving and casting**: Wowee still refuses a cast-time spell while moving
  locally ("Can't do that while moving"); where stock checks that was not
  traced.
- The melee range pre-check (8 yd "Out of range.") and Charge's checks are
  Wowee's own and were left.
- `SpellStopCasting`'s first branch (0x800BA0, `+0x1858 & 4` -> 0x6E1660) is
  not reproduced.
- `UNIT_SPELLCAST_*` castID is still the spell id, not a per-cast counter;
  a refused press during a cast fires no `UNIT_SPELLCAST_FAILED`, so the bar
  is not touched, where stock fires it with the new cast's id.
- Stock was not measured live (no instrumented stock client); its timing is
  from the code paths above.
- `MSG_MOVE_SET_FACING` 8 ms after `MSG_MOVE_START_FORWARD` on a scripted
  `MoveForwardStart` was seen in both runs and not chased.
