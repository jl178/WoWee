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

### Tab and the other targeting keys

TARGETNEARESTENEMY and its siblings are plain Bindings.xml scripts
(`TargetNearestEnemy()`), not `runOnUp`: they run from the key-down event
and 0x524FC0 selects at once (`CMSG_SET_SELECTION`, `PLAYER_TARGET_CHANGED`),
before the frame's world update. Which unit is picked is in
`combat-targeting-movement.md` section 1.

### Moving and casting

The client refuses a cast-time spell while moving itself (0x739650,
`SPELL_FAILED_MOVING`, nothing sent), and starting to move cancels a cast
from the key event before the movement packet (0x5FAA40: `CMSG_CANCEL_CAST`,
then `MSG_MOVE_START_*`). Rules in `combat-targeting-movement.md` section 3.
`CMSG_CANCEL_CAST` is a byte of cast count and the spell id (0x806200).

### Facing with a start

`MoveForwardStart` sends `MSG_MOVE_START_FORWARD` with the current facing;
a `MSG_MOVE_SET_FACING` follows only when the facing then moves by 0.1 rad
(0x71AE80). Starting to move does not turn the character.

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
| Tab target | unchanged by this pass (7.2 ms after the pump); see section 5 |

The time a press now waits is the time it is held: a release is acted on at
the top of the next frame, as stock acts on it from the event.

## 5. Second pass: targeting, clicks, moving casts, facing

Changes:

- **Tab runs from the key event**: TARGETNEARESTENEMY is an interface-first
  binding (`LuaEngine::interfaceRunsFirst`), so its Bindings.xml script runs
  in the pump; GameScreen's poll is left for a pad button or no interface.
- **One TargetNearest** (`CombatHandler::targetNearest`, rules in
  `game/tab_target.hpp`) behind TargetNearest, ...Enemy, ...EnemyPlayer,
  ...Friend, ...FriendPlayer, ...PartyMember, ...RaidMember and the Tab
  poll, with 0x524FC0's candidates, order and list. The on-screen test is
  the unit's model sphere against the camera frustum
  (`CombatHandler::setUnitInViewProvider`, set in Application).
- **World clicks at the top of the frame**: `GameScreen::
  processWorldClicksAtInput` runs the left/right click handling after the
  pump and before the world update; the draw only covers a frame that
  skipped it. Scripted clicks (`WoweeDebug_Mouse`) now reach Input too
  (`Input::setVirtualMousePosition`).
- **Moving and casting by 0x739650/0x5FAA40** (`stock_cast::
  refusedWhileMoving`, `SpellHandler::refusedForMoving`,
  `interruptForMoveStart`), with the casting-time spell mods per
  SpellFamilyFlags bit (`castTimeAfterMods`; SMSG_SET_*_SPELL_MODIFIER bits
  6-95 were being dropped). The refusal is checked before the dismount.
- **CMSG_CANCEL_CAST** carried a 4-byte count: AzerothCore read the spell id
  one byte late and its InterruptNonMeleeSpells, which matches the id, left
  the cast running - `SpellStopCasting` and the move cancel stopped the bar
  here while the server finished the spell. Now a byte, as stock.
- **The facing SET_FACING after a start**: the login camera pan
  (`CameraController::startIntroPan`) turned the controller's facing with
  the camera while the drawn character kept the server's; the first step
  snapped the character 140 degrees and sent `MSG_MOVE_SET_FACING` (in the
  earlier runs, 8-16 ms after `START_FORWARD`, facing 2.41 -> -0.03). The pan
  now ends behind the character and leaves the facing alone; the idle
  orbit likewise.

Measured (same lab server, ~60 fps, scripted keys; "into frame" is from the
top of the frame that took the event):

| Step | Before | After |
|---|---|---|
| Tab -> `CMSG_SET_SELECTION` | 7.2-21.8 ms after the pump (14.4 ms into the frame, inside the UI draw) | 0.09-0.6 ms into the frame, 0 frames (in the pump; the larger figure with the candidate debug walk on) |
| Left click on empty ground -> `CMSG_SET_SELECTION 0` | in the UI draw, 5-20 ms into the frame | 0.54-0.61 ms into the frame, 0 frames |
| Holy Light pressed while running | refused by Wowee's own rule (horizontal flags, any cast time), after the dismount | refused at the release, "Can't do that while moving", nothing sent; again 0.4 s later says nothing (throttle) |
| Seal (instant) while running | sent | sent, 0.13 ms into the frame |
| Holy Light while jumping | sent (falling was not "moving") | refused (FALLING is in 0xC0100F) |
| Holy Light while turning in place | sent | sent, and the cast completes |
| MoveForwardStart during Holy Light | cancel, packet unread by the server; the server interrupted on movement | `CMSG_CANCEL_CAST` then `MSG_MOVE_START_FORWARD` in the same ms, `UNIT_SPELLCAST_INTERRUPTED`; server `SMSG_SPELL_FAILURE` 250 ms later |
| SpellStopCasting during Holy Light | bar stopped here; server cast unaffected | server answers `SMSG_CAST_FAILED`/`SMSG_SPELL_FAILURE` 190 ms later, no `SMSG_SPELL_GO` |
| First MoveForwardStart after login | START_FORWARD facing 2.41, SET_FACING -0.03 16 ms later | START_FORWARD facing 0.045, no SET_FACING |

Tab order checked live: with the player facing a cluster, the list was the
units in the cone nearest first (10, 10, 10, 10, 10, 11, 17, 21 yd); with the
camera swung round (`WoweeDebug_Camera(180)`) the same units were off screen
and Tab selected nothing and left the target alone; a pause of 3.5 s went
back to the nearest; `TargetNearestEnemy(1)` stepped back one.

Tests: `tests/test_tab_target.cpp` (cone, reach, creature types, modes,
PvP, order, stepping, wrapping and its 1 s rebuild, the 3 s expiry, a
fresh list re-selecting the target, a clicked target, dead entries,
another mode), `tests/test_stock_cast_timing.cpp` (moving refusal, spell
mods, move-start cancels), `tests/test_input_integration.cpp` (Tab runs
from the key event).

## 6. Third pass: how a cast ends, duels, macros, the pad

Read in `Wow.exe` this pass (details in `combat-targeting-movement.md`
section 1 "Duels" and section 3 "Casts ending"):

- Every end of a cast goes through 0x7FECC0 with a record of the player's
  casts in flight (0xAF5254): the event by result (SUCCEEDED / FAILED_QUIET /
  INTERRUPTED / FAILED) with **the record's count as the cast id**, for every
  token naming the unit, and `UNIT_SPELLCAST_STOP` only when the bar showed
  that cast. The client's own cancel (move start: INTERRUPTED; SpellStopCasting:
  FAILED_QUIET) ends the cast and its bar; the server's three reports that
  follow (`SMSG_CAST_FAILED`, `SMSG_SPELL_FAILURE`, `SMSG_SPELL_FAILED_OTHER`)
  each fire INTERRUPTED again, without a STOP.
- Text: only `SMSG_CAST_FAILED` (0x808200) shows anything - "Interrupted",
  through the 3 s throttle - and only to the caster. `SMSG_SPELL_FAILURE` and
  `SMSG_SPELL_FAILED_OTHER` show nothing and write no chat line.
- The school's fizzle (Resistances.dbc FizzleSoundID) plays on
  `SMSG_CAST_FAILED` (not for DONT_REPORT) and on `SMSG_SPELL_FAILURE`.
- 0x806200's `CMSG_CANCEL_CAST` goes out only for a spell with a cast time or
  a next-swing one; an Auto Shot or Shoot press in flight is dropped with its
  INTERRUPTED and no packet; the repeat goes on.
- Food and drink: 0x5FAA40 sends nothing; the movement packet stands the
  player up on the server and the aura goes.
- TargetNearest, ...Enemy, ...EnemyPlayer, ...Friend, ...FriendPlayer run
  once per macro (0x564DB0 sets 0xBEAF44 to all ones for the outermost
  macro; 0x525AD0 and its siblings run only with bit 0x20 and clear it).
  RunMacroText and RunMacro use the same runner.

Changes:

- `stock_cast::PendingCasts` (game/stock_cast_events.hpp): the record of
  every press from the send to its `SMSG_SPELL_GO` or failure, in
  `SpellHandler::pendingCasts_`. `castEndSteps` is 0x7FECC0;
  `SpellHandler::endPlayerCast` / `endUnitCast` / `fireCastEnd` apply it.
- Move start (`interruptForMoveStart`): every record whose InterruptFlags
  have movement is cancelled (`cancelPendingCast`: the packet by
  `cancelSendsPacket`, then INTERRUPTED and the STOP for the bar's cast).
  The movement handler asks `moveStartHasWork()`, so a pending Auto Shot
  press counts without a cast bar.
- `SpellStopCasting`, `/stopcasting` and Escape (`spellStopCasting`,
  `cancelCast`): the current cast with DONT_REPORT - FAILED_QUIET and STOP.
- `handleCastFailed`: fizzle, text through the throttle, then the event for
  the refused cast (its count; a press refused during a cast gets its own
  event and leaves the running bar); STOP only for the bar's cast.
- `handleSpellFailure` / `handleSpellFailedOther`: no text, no chat line;
  the fizzle (the player's), NEED_AMMO stops the auto-repeat, the event with
  the packet's spell and count for every token, the unit's cast ended only
  when it is that cast.
- The cast id of every `UNIT_SPELLCAST_*` event and of `UnitCastingInfo` is
  now the cast count (`spellcastArgs(unit, spell, count)`); START and
  SUCCEEDED fire for every token naming the caster; the bar's STOP after a
  cast goes off follows SUCCEEDED, and only for the bar's cast.
- Food and drink on a move start: no `CMSG_CANCEL_AURA` and no
  `CMSG_STANDSTATECHANGE`; only this client's seated presentation ends.
- Once per macro: `tab_target::MacroOnce` in `CombatHandler`, begun and ended
  by `ChatPanel::executeMacroText` (macro actions, RunMacro, RunMacroText).
- Pad Tab: `GameScreen::processTargetKeysAtInput`, called by Application
  after the input pump with the world clicks; the draw's poll only covers a
  frame that skipped it. `WoweeDebug_PadCommand(cmd, down)` drives a virtual
  (keyless) command from a test script.

Measured (lab server, paladin Wptestb, Holy Light, `WOWEE_INPUT_TRACE=1`):

| Step | Before | After |
|---|---|---|
| MoveForwardStart during Holy Light | `CMSG_CANCEL_CAST`, INTERRUPTED, STOP with no spell; on the server's answer "Interrupted" twice (CAST_FAILED and SPELL_FAILURE), "Holy Light: Interrupted" with a chat line, INTERRUPTED/STOP with no spell | `CMSG_CANCEL_CAST` 0.72 ms into the frame, INTERRUPTED + STOP (count 1) in the same ms, `MSG_MOVE_START_FORWARD` 0.19 ms later; 487 ms later `SMSG_CAST_FAILED` and `SMSG_SPELL_FAILURE` in one frame: the fizzle twice, "Interrupted" once, INTERRUPTED three times (CAST_FAILED, FAILURE, FAILED_OTHER), no STOP, no chat line |
| `/stopcasting` (RunMacroText) during Holy Light | cast state cleared, no event | `CMSG_CANCEL_CAST`, FAILED_QUIET + STOP in the same ms; the server's answer 27 ms later: "Interrupted" once, INTERRUPTED three times |
| Escape during Holy Light | as `/stopcasting` before | as `/stopcasting` after (13.5 ms after the key, from the draw's Escape chain) |
| Pad Tab (`TARGETNEARESTENEMY` with no key behind it) | `CMSG_SET_SELECTION` in the UI draw | 0.11 ms into the frame, from the input step |
| Keyboard Tab | 0.09-0.6 ms into the frame | 0.12 ms into the frame (unchanged path) |
| MoveForwardStart with Auto Shot on (paladin given a bow, kobold target) | nothing | the press's record (count 1) dropped: `UNIT_SPELLCAST_INTERRUPTED` Auto Shot in the same ms as `MSG_MOVE_START_FORWARD`, no `CMSG_CANCEL_CAST`, the repeat still on; a second start does nothing; turning it off later: `CMSG_CANCEL_AUTO_REPEAT_SPELL`, the server's `SMSG_CAST_FAILED`/`SMSG_SPELL_FAILURE` for count 1: "Interrupted" and INTERRUPTED |
| MoveForwardStart while eating (Tough Hunk of Bread, seated) | `CMSG_CANCEL_AURA` and `CMSG_STANDSTATECHANGE` before the start | `MSG_MOVE_START_FORWARD` alone; 348 ms later the server's aura removal and stand state 0 |
| Two TargetNearestEnemy in one macro, target cleared between | both select | the first selects, the second does nothing; the same from plain Lua selects twice; `/targetenemy` lines likewise |

## 7. Gaps

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
- The melee range pre-check (8 yd "Out of range.") and Charge's checks are
  Wowee's own and were left.
- `SpellStopCasting`'s first branch (0x800BA0, `+0x1858 & 4` -> 0x6E1660) is
  not reproduced, nor 0x5D9FF0 after its cancel.
- **Refusals made by the client** (cooldown, moving) fire no
  `UNIT_SPELLCAST_FAILED`; stock's 0x808200 ends them through 0x7FECC0 too.
  The record such a refusal carries was not traced.
- **The combat log** `SPELL_CAST_FAILED` entry (0x751AD0) is not written.
- **The fizzle** plays only for the player's own failures; another unit's
  `SMSG_SPELL_FAILURE` plays none (stock plays it at that unit).
- **Event tokens**: player, pet, party1-4, target, focus, mouseover; the
  vehicle, partypet, raid, arena and boss tokens of 0x60BB70 are not given.
- **The older clients** (classic, TBC) carry no count in these messages; the
  record of that spell stands in for it.
- Stock was not measured live (no instrumented stock client); its timing is
  from the code paths above.
- **CanAttack/CanAssist** are still approximations from the faction reaction,
  the unit flags and, for players, the PvP byte - now with the duel rule in
  front; the vehicle as reference (0x6DD060) and the commentator rule are
  not done.
- **The on-screen test** uses the camera at the time of the press and the
  character renderer's bounds; a unit with no character instance (not yet
  drawn) is never a candidate, as an undrawn model is not in stock.
- **SitStandOrDescendStart** cancels only through START_DESCEND; a stand-up
  move key while sitting is not a start in Wowee's opcode list.
- **Turning while channelling** (ChannelInterruptFlags 0x10) is left to the
  server, as stock leaves it.
