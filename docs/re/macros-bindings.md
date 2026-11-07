# Macro options and the binding functions (stock 3.3.5a, build 12340)

What `SecureCmdOptionParse` does with `[mod:shift,@focus] A; B`, which C
functions `Bindings.xml` calls, and what each of those does in `Wow.exe`
12340, read with Ghidra (headless project copied from the camera work).
Addresses are virtual addresses in the stock executable. Wowee's side is in
`src/ui/chat/macro_evaluator.cpp` (the parser), `src/addons/lua_macro_api.cpp`
(the Lua functions) and `CameraController` (the views).

## 1. Where the Lua functions live

The C functions are registered from `{name, function}` tables; a scan of
`.rdata`/`.data` for runs of (string pointer, code pointer) finds 2765 names.
The binding ones sit in two tables:

- **0xAD1938** (input control): `JumpOrAscendStart` 0x5FBF80, `ToggleRun`
  0x5FAAE0, `ToggleAutoRun` 0x5FC190, the Move/Turn/Strafe pairs,
  `PitchUpStart` 0x5FC8E0 / `PitchUpStop` 0x5FC570, `PitchDownStart` 0x5FC920 /
  `PitchDownStop` 0x5FC5C0, `TurnOrActionStart` 0x5FC610 / `Stop` 0x5FC680,
  `CameraOrSelectOrMoveStart` 0x5FC6C0 / `Stop` 0x5FC730, `MoveAndSteerStart`
  0x5FC780 / `Stop` 0x5FC830, mouselook, the vehicle aim set.
- **0xAD2060** (camera): `CameraZoomIn` 0x6017E0, `CameraZoomOut` 0x601840,
  `MoveView{In,Out,Left,Right,Up,Down}{Start,Stop}` 0x5FF080..0x5FF230,
  `SetView` 0x6039B0, `SaveView` 0x5FF260, `ResetView` 0x604C80, `NextView`
  0x604CE0, `PrevView` 0x604D10, `FlipCameraYaw` 0x5FF2C0,
  `VehicleCameraZoomIn/Out` 0x6018A0 / 0x6018B0.
- The game table **0xAC80B0**: `ToggleSheath` 0x51A6B0, `Screenshot`
  0x5150E0 (the glue one is 0x4DC520), `IsEquippedItemType` 0x51C740,
  `GetMouseButtonClicked` 0x50F950, and the debug bindings.

### The 45 names

`Bindings.xml` bodies call 39 functions this client did not define (the tour
counted 45 with the Mac-only ones). The interface-wide count against the
stock table is in section 6.

| Function | Stock | Wowee now |
|---|---|---|
| `ToggleAutoRun` | 0x5FC190: toggles control bit 0x1000 (set: 0x5FA170 then 0x5FAA40, a player-side update not traced further; clear: 0x5FA450), then the re-vote 0x5FBBC0 | a press of TOGGLEAUTORUN, which the movement code toggles exactly as the key does |
| `ToggleRun` | 0x5FAAE0: not on a taxi, not mid-air, then 0x71AE50 walk/run toggle | a press of TOGGLERUN |
| `ToggleSheath` | 0x51A6B0: active player's 0x6E23A0 | a press of TOGGLESHEATH |
| `PitchUp/DownStart/Stop` | bits 0x400 / 0x800 set and cleared, re-vote 0x5FBBC0 | held PITCHUP / PITCHDOWN commands (nothing reads them yet, see gaps) |
| `TurnOrActionStart/Stop` | bit 0x1 (right button); if no button was down, +0x18 = 2 and 0x4FA570 (captures the cursor); both buttons down: 0x5FAA40 | `CameraController::setScriptedMouseButton(right)`: steering and two-button running |
| `CameraOrSelectOrMoveStart/Stop` | bit 0x2 (left button), +0x18 = 1; Stop(sticky) takes `toboolean(arg1)` (0x815500) | the left button the same way |
| `MoveAndSteerStart/Stop` | both of the above in one call | both |
| `CameraZoomIn/Out(n=1)` | n yards (×0.2 while DAT_BD19B8, the barber shop), 0x5FF950 / 0x5FFA60: n / cameraDistanceMoveSpeed of travel | `processMouseWheel(±n)`, the same 1 yd per unit at 8.33 yd/s |
| `VehicleCameraZoomIn/Out` | tail calls to the two above | the same functions |
| `MoveView*Start(speed=1)` / `Stop` | 0x5FF000 with ESI = 0 in, 1 out, 2 right, 3 left, 4 up, 5 down: bit `1 << 2i` in CGCamera +0x160, start time +0x164+4i, speed +0x1AC+4i; not while +0x9C bit 0x40; Stop sets the stop bit `2 << 2i` with its time | held motion, in/out at 8.33 yd/s × speed, turn at cameraYawMoveSpeed and tilt at cameraPitchMoveSpeed × speed (180 / 90 °/s by default; consumer 0x6000E0, Right goes round to the character's right and Up goes up - movement-camera.md section 10) |
| `SetView(n)` | "Usage: SetView(viewModeIndex)" without a number; n in 1..5 (`n - 1 < 5`), 0x603330(n, cameraViewBlendStyle) | `CameraController::setView` |
| `SaveView(n)` | n in 1..5: 0x5FE440 stores the zoom target (+0x1E8), pitch (+0x230), yaw (+0x260) into view n and writes the three CVars | `saveView` (kept for the session only) |
| `ResetView(n)` | n in 1..5: 0x6048A0 reloads view n from the default table; the current view is set again | `resetView` |
| `NextView` / `PrevView` | current (+0xB4) < 5: SetView(+1); > 1: SetView(-1) | the same bounds |
| `FlipCameraYaw(deg)` | "Usage: FlipCameraYaw(degrees)"; +0x12C += deg × π/180 (0x9E2B40) | `flipYaw` |
| `TogglePerformanceDisplay`, `TogglePerformancePause`, `TogglePerformanceValues`, `ResetPerformanceValues`, `ToggleCollision`, `ToggleCollisionDisplay`, `TogglePlayerBounds`, `TogglePortals`, `ToggleTris`, `TeleportToDebugObject`, `debuginfo` | all one function, 0x8E5250: `return 0` | the same no-op |
| `ToggleCommentatorMode`, `MusicPlayer_*`, `MovieRecording_*` | **not in the table**: nil in the shipped Windows client too (the music and movie bindings are `platform="mac"`) | left undefined |

### The camera views (CGCamera)

- Eight view slots at **+0xB8 + 12·i** (distance, pitch, yaw); the current
  view index at +0xB4. Lua reaches 1..5.
- Defaults (string table **0xAD1BA8**, three per view; pitch and yaw are
  degrees, converted by the 0xAD1B80 flags [0, 1, 1]):

  | view | name (0xAD213C) | distance | pitch | yaw |
  |---|---|---|---|---|
  | 0 | FIRST_PERSON | 0 | 0 | 0 |
  | 1 | THIRD_PERSON_A | 0 | 0 | 0 |
  | 2 | THIRD_PERSON_B | 5.55 | 10 | 0 |
  | 3 | THIRD_PERSON_C | 5.55 | 20 | 0 |
  | 4 | THIRD_PERSON_D | 13.88 | 30 | 0 |
  | 5 | THIRD_PERSON_E | 13.88 | 10 | 0 |
  | 6 | VIEW_COMMENTATOR | 0 | 0 | 0 |
  | 7 | VIEW_BARBER_SHOP | 5.0 | 10 | 0 |

  `cameraView` defaults to "2"; `cameraViewBlendStyle` to "1".
- **0x603330** (set view): logs "Camera view %s"; a change of view writes
  `cameraView`; view 6 only for a commentator. Blend 1 starts the distance
  (0x600E00), pitch (0x601190), yaw (0x601410) and a fourth slot (0x6012D0)
  smoothings and gives them all the longest duration clamped to
  [cameraSmoothTimeMin, cameraSmoothTimeMax]; blend 2 (forced when the view
  is already the current one) snaps. The view's yaw is relative to the
  character's facing.
- Wowee: `setView` eases yaw and pitch through the follow-swing slots with the
  same smoothing speeds and clamp, the distance at the zoom speed, and snaps
  when the view is current. View 1's distance 0 clamps to the first-person
  minimum (0.5).

### Screenshot

- `Screenshot()` (0x5150E0) only arms a callback (0x4A8550); the frame's
  present path runs **0x4A8570**: creates `Screenshots`, names the file
  `WoWScrnShot_%m%d%y_%H%M%S` (0x86D490, strftime of local time), writes a TGA
  when `screenshotFormat` is "tga", otherwise a JPEG (`screenshotFormat`
  default "jpeg", 0x401B60). JPEG quality = round(q × 5.5 + 45) for
  `screenshotQuality` q clamped to 1..10, default "3" -> 62 (0x4A84A0).
- The callback 0x512E20 fires **SCREENSHOT_SUCCEEDED** (event 0xAB) or
  **SCREENSHOT_FAILED** (0xAC); `worldframe.lua` shows SCREENSHOT_SUCCESS
  ("Screen Captured") or SCREENSHOT_FAILURE in ActionStatus. No chat line.
- Wowee did a PNG under `~/.wowee/screenshots/WoWee_YYYYMMDD_HHMMSS.png` and a
  "Screenshot saved" chat line. Now: `<config root>/Screenshots/
  WoWScrnShot_MMDDYY_HHMMSS.jpg` (or .tga) at the stock quality, and the two
  events; the chat line only when no interface is loaded.

## 2. SecureCmdOptionParse (0x564AE0)

Lua: `action, target = SecureCmdOptionParse(options)`. "Usage:
SecureCmdOptionParse(\"options\")" without a string. **No values at all**
when nothing matches; otherwise the action and the target (nil when the
matching bracket named none or an empty one).

### The walk (0x5F0DF0)

- Scan the string. `[`: skip spaces, find `]` (or the end), trim trailing
  spaces; if no earlier bracket of this clause passed, evaluate it; the
  action starts after the `]`; remember that the clause had a bracket.
- `;` or the end: if the clause had **no bracket**, or one **passed**, the
  action is the text from its start, trimmed of **spaces only**; done.
  Otherwise the next clause (or no match at the end).
- So several brackets are ORed, the first passing one gives the target,
  text before a bracket is dropped, a `;` inside a bracket does not split,
  and a bracket left open leaves an empty action.
- A bracket's parse is cached by its text (0x617C50 / 0x5F0BB0 into a hash),
  which is why an unknown option is reported once per session.

### A bracket (0x5F0BB0)

Split on `,`, each piece trimmed of spaces. `target=` (exactly, 7 bytes,
case-sensitive `strncmp`) or a leading `@` gives the unit, trimmed; anything
else is a condition (0x5F0040). The last unit given wins.

### A condition (0x5F0040)

- `no` (case-sensitive, and the piece at least 3 long) negates.
- Split at `:`; the name is compared **exactly** (case-sensitive) with the
  33-entry table at **0xAD14D8**; the handlers are at **0xAD1560**.
- Arguments only when something follows the colon: split on `/`, each
  trimmed of spaces.
- Unknown name: `ERR_UNKNOWN_MACRO_OPTION_S` ("Unknown macro option: %s",
  message 0x23E, a system chat line through 0x5216F0 type 0), and the entry
  keeps index 0, the empty name, whose handler (0x8C8DE0) returns 1. So an
  unknown option holds and a negated unknown one fails.

### Evaluation (0x5EF5C0)

The unit (at most 31 characters) is resolved once (0x60ABF0, with "empty
means the current target"); every condition is called with it and its
arguments, the result flipped when negated, and the first false one fails
the bracket.

### The conditions (0xAD1560)

"Number" below is the leading digits of an argument (0x5EEB00; none reads 0).
A yes/no condition with an argument compares its state with the **first**
argument's number, so `[combat:0]` is out of combat.

| Name | Handler | Holds when |
|---|---|---|
| (empty) | 0x8C8DE0 | always |
| `combat` | 0x5EF710 | player **or pet** has UNIT_FLAG_IN_COMBAT (0x80000) |
| `exists` | 0x5EED70 | the unit's object exists, or 0x512A30 knows it |
| `help` | 0x5EF7B0 | player can assist the unit (0x7293D0) |
| `harm` | 0x5EF830 | player can attack the unit (0x729740) |
| `party` | 0x5EEDE0 | the unit is in the party (0x52C8C0) |
| `raid` | 0x5EEE20 | in the party, or in the raid (0x573200) |
| `dead` | 0x5EF8B0 | the unit is dead or a ghost (health 0, or the corpse/ghost checks) |
| `flyable` | 0x5EEE70 | IsFlyableArea: AreaTable flags of the area, else the zone, have 0x400 and not 0x20000000 (0x51A1D0); in Northrend (0x23B) a player also needs Cold Weather Flying 54197 (0x7261B0) |
| `indoors` / `outdoors` | 0x5EF980 / 0x5EF9E0 | not / is outdoors (0x71B7F0) |
| `swimming` | 0x5EFA40 | movement flag 0x200000 |
| `flying` | 0x5EFAA0 | movement flag 0x2000000 |
| `mounted` | 0x5EFB00 | a mount display id, and not flag 0x10000000 |
| `stealth` | 0x5EFB70 | the stealth visibility byte |
| `group` | 0x5EEED0 | no argument: party or raid; arguments, last first: `party` -> in a party, `raid` -> in a raid (exact words) |
| `stance`, `form` | 0x5EEFB0 | no argument: a form (GetShapeshiftForm != 0); else any argument's number == the form |
| `pet` | 0x5EFBD0 | a pet; with arguments, one equal (case-insensitive) to its name or its creature family |
| `modifier`, `mod` | 0x5EF040 -> 0x55F940 | no argument: any of the six modifier keys; else any argument: a run of LSHIFT/RSHIFT/SHIFT/LCTRL/RCTRL/CTRL/LALT/RALT/ALT (case-insensitive, `-` allowed between) all held, SHIFT meaning either side (0x55D860); a name that is none of these is a modified click (IsModifiedClick) |
| `button`, `btn` | 0x5EF0D0 | **no argument: false**; any argument equal (case-insensitive) to the clicked button (+0x1234, LeftButton when none) or the remapped one; a number n is the button name of bit n (1 LeftButton, 2 RightButton, 3 MiddleButton, 4 Button4, 5 Button5; 0x5EEC20) |
| `actionbar`, `bar` | 0x5EF200 | **no argument: false**; a number == the page (0xC1E598 + 1) |
| `bonusbar` | 0x5EF270 | **no argument: false**; a number == the bonus bar offset (0xC1E59C) |
| `equipped`, `worn` | 0x5EFD70 | no argument: false; IsEquippedItemType of any argument |
| `channeling` | 0x5EFEB0 | a channel running (+0xA80, end time +0xA88); with arguments, its spell name equal to one (case-insensitive) |
| `spec` | 0x5EF2E0 | no argument: false; a number == active talent group + 1 |
| `vehicleui` | 0x5EF430 | the player's vehicle has flag 0x20000000 (0x5EF360) |
| `unithasvehicleui` | 0x5EF470 | the same for the unit |
| `cursor` | 0x5EF490 | the cursor holds item (types 1, 7, 9, 11), money, spell, merchant, macro, guildbankmoney or equipmentset (a pet action or companion is none); with arguments, the kind equal to one |

Not in the table, so unknown (and true) in 3.3.5a: `buff`, `debuff`,
`casting`, `stealthed`, `vehicle`, `nomod`-as-a-word (it is `no` + `mod`),
`mod:none`, `possessbar`, `overridebar`, `extrabar`, `petbattle`, `known`,
`talent`. Wowee's evaluator had `buff/debuff/casting/stealthed/vehicle` and
`mod:none`; they are gone.

### IsEquippedItemType (0x51C740)

The type is first matched (case-insensitive) against the INVTYPE tokens
(0xAC7FD8, 29 of them in InventoryType order) and their global strings; a
match tests the equipped items' inventory type. Otherwise an equipped item
matches by ItemClass.dbc name, ItemSubClass.dbc display or verbose name, or
an ItemSubClassMask.dbc name covering its subclass (0x517740). Equipped
items only (0x7546F0 with 1). Returns 1 or nil.

## 3. The secure slash commands

`chatframe.lua` runs every secure command's argument through
SecureCmdOptionParse (`SecureCmdList`, dispatched through
`hash_SecureCmdList` before `SlashCmdList`), so the C parser is the whole of
the conditional support. The unit rules per command, which Wowee's own
commands now follow:

- CAST/USE, CASTRANDOM, CASTSEQUENCE: action and target as parsed.
- TARGET (and TARGET_EXACT): the target unless it is nil or "target", then the
  action; TargetUnit.
- FOCUS: empty message: FocusUnit(); else target unless nil or "focus".
- ASSIST: empty message: AssistUnit(); else target, else action.
- STARTATTACK: target unless nil or "target", else the action.
- PET_ATTACK: target unless nil or "pettarget", else the action.
- STOPATTACK, STOPCASTING, CANCELFORM, CLEARTARGET, CLEARFOCUS, PET_FOLLOW,
  PET_STAY, the pet stances, STOPMACRO, and SlashCmdList DISMOUNT and
  LEAVEVEHICLE: act when the options match at all (an empty message matches).
- CANCELAURA, EQUIP, the pet autocast trio: the action.

## 4. What Wowee does now

- One parser, `ui::MacroEvaluator::parse` (`macro_evaluator.cpp`), with the
  walk, the bracket and condition rules, the 33 names and their argument
  rules above, and the bracket cache with the once-per-session unknown report.
- `SecureCmdOptionParse` in Lua is that parser (`lua_macro_api.cpp`), asking
  the Lua API for each answer (`LuaMacroState`: UnitExists, UnitCanAssist,
  GetShapeshiftForm, GetActionBarPage, GetBonusBarOffset, IsEquippedItemType,
  UnitChannelInfo, GetCursorInfo, GetMouseButtonClicked, IsModifiedClick, the
  Is*KeyDown set ...), so `securestatedriver.lua`'s state drivers and the
  secure commands see what the rest of the interface sees. The two earlier
  copies (a Lua stub in `lua_engine.cpp` that returned the text after the last
  `;`, and a C stub in `lua_social_api.cpp` that returned the whole string)
  are removed.
- This client's own commands (`/cast`, `/use`, `/castsequence` with
  `reset=`, `/equip`, `/target`, `/cleartarget`, `/focus`, `/assist`,
  `/startattack`, `/stopattack`, `/stopcasting`, `/stopmacro`, `/dismount`,
  `/cancelform`, `/cancelaura`, `/petattack`, `/petfollow`, `/petstay`, the
  pet stances) run every argument through `ui::evaluateMacroOptions`, which is
  the same parser over `LuaMacroState` whenever an interface is loaded
  (GameStateAdapter otherwise), and apply the unit rules of section 3. Before,
  they parsed only an argument starting with `[`, so `/cast A; B` cast a spell
  called "A; B".
- `IsEquippedItemType` and `IsFlyableArea` (was a flat false) are implemented
  as above.

### Checked live (lab account WPTEST4, Wptestd, Stormwind)

- SecureCmdOptionParse from Lua: `[combat] A` -> no values; `[@player,help] Heal`
  -> "Heal", "player"; `[bogusoption] A; B` -> "A" and one "Unknown macro
  option: bogusoption" system line; "" -> "", nil.
- RunMacroText: `/cast [combat] Frost Armor` and `/stopmacro [nocombat]` +
  `/cast` cast nothing; `/cast [nocombat] Frost Armor; Fireball` cast Frost
  Armor; `/cast [@player] Arcane Intellect` cast it.
- ToggleAutoRun / RunBinding("TOGGLEAUTORUN") start and stop the run.
  ToggleRun and RunBinding("TOGGLERUN") walk: 2 s of autorun covered 0.0046
  map units running and 0.00165 walking (2.5 / 7 yd/s), and back to 0.0046.
- `ToggleRunWalk` is not a 3.3.5a function (no such string in Wow.exe; the
  stock name is ToggleRun, 0x5FAAE0). Here it reads as the missing-API
  fallback's no-op, so a script calling it walks nothing and raises nothing
  (it is listed in missing_api.txt); in the stock client it raises.
- SetView, SaveView, NextView, PrevView, FlipCameraYaw, CameraZoomOut,
  MoveViewLeft/Out move the camera (screenshots); the usage errors match.
  SetView(1) holds the zoom target at 0.5 yd, but the player model was still
  drawn there in one run (first-person hiding is the camera lane's).
- Screenshot() and the SCREENSHOT key wrote `Screenshots/WoWScrnShot_110626_
  154309.jpg` and fired SCREENSHOT_SUCCEEDED.
- missing_api.txt after the run: no real gaps.

## 5. Gaps

- Macros run from an action button still go through this client's command
  registry rather than FrameXML's `hash_SecureCmdList`; the parser and the
  unit rules are now the same, but the commands themselves (spell lookup,
  item use) are two implementations. `StopMacro()` in Lua is still a no-op,
  so FrameXML's /stopmacro (typed, not in a macro) stops nothing.
- `GetMouseButtonClicked` answers nil, so `[button:2]` from a right click on
  an action button is false (stock: the clicked button). The secure action
  button's remapped button (DAT_BEAF48) is not modelled.
- `PitchUp/Down` are read by the movement now (movement-camera.md section 5,
  row 34).
- MoveView turn/tilt: the consumer is 0x6000E0 (movement-camera.md section
  10, row 36), at the cameraYaw/PitchMoveSpeed CVars. Saved views are not written to
  CVars across sessions. The barber shop's 0.2 zoom factor is not applied.
- `[dead]`'s feign-death and corpse details (0x5EF8B0) are UnitIsDeadOrGhost
  here.

## 6. The rest of the interface

Against the stock table, FrameXML and the Blizzard addons call 196 stock C
functions the client did not define; 150-odd are GlueXML's, which this client
does not run (glue-screens.md section 8). The FrameXML ones besides the
bindings are now bound as the stock client's behaviour allows here: the debug
hooks (`debuginfo`, `FrameXML_Debug`, `debuglocals`), voice chat's loopback
test and `SetActiveVoiceChannel` / `GetVoiceSessionMemberInfoBySessionID`,
the tic-tac-toe minigame, the GM survey submits, `BNAcceptFriendInvite`, the
pre-2.0 skill-point buttons (`AcceptSkillUps`, `AddSkillUp`, `RemoveSkillUp`,
`BuySkillTier`), `StopCinematic`, `GameMovieFinished`; all inert. Still
missing: `AcceptLevelGrant` / `DeclineLevelGrant` (Recruit-a-Friend),
`GetTabardInfo` (guild registrar), `PickupEquipmentSet` (secure handlers).
