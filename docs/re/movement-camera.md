# Local-player movement and camera (stock 3.3.5a, build 12340)

How the stock client moves the character it controls, what it tells the
server, and how its camera follows. Read from `Wow.exe` 12340 with Ghidra
(project copy in the session scratchpad, `move-re/`). Every rule names the
function it comes from. The movement-animation side (which animation a moving
unit plays, upper/lower body) is in `combat-targeting-movement.md` section 3
and is not repeated here.

Terms:

- **CMovement** is the movement status block a `CGUnit_C` points at with
  `+0xD8`. Its fields, as this client uses them:

  | Offset | Field |
  |---|---|
  | +0x10/+0x14/+0x18 | position |
  | +0x20 / +0x24 | facing / pitch (radians) |
  | +0x44 / +0x48 | movement flags / flags2 |
  | +0x4C..+0x54, +0x58, +0x5C, +0x60 | where the current leg of movement started, its facing and pitch, ms since then |
  | +0x64..+0x6C | unit 3D direction of travel |
  | +0x70/+0x74 | 2D direction of travel (cos, sin) |
  | +0x78/+0x7C | cos/sin of pitch |
  | +0x80 / +0x84 | fall time (ms) / z where the fall started |
  | +0x8C | the speed the current leg moves at |
  | +0x90..+0xB0 | walk, run, run-back, swim, swim-back, flight, flight-back, turn rate, pitch rate |
  | +0xB8 | vertical speed, **positive down** |
  | +0xC8 / +0xCC | collision half-width / height |
  | +0x144 | the owning unit |

- **Moving** means the flags hold any of `0x00C0100F`: a direction, FALLING,
  ASCENDING or DESCENDING. Turning, pitching and SWIMMING on their own do not
  count. Several rules below test this mask.
- The code is `game/stock_movement.hpp`; every number below that has a
  constant there names it.

## 1. The movement model

Movement is driven by an **event queue** (`CMovement_C` +0x140, events
allocated by 0x6EBC70 as `"AUCPlayerMoveEvent"`), processed by 0x6EF860.
An input handler does not move the character; it queues an event with a
timestamp. 0x6F09F0 (per frame) advances time up to the next event or the
next heartbeat, whichever is first, runs the physics for that slice
(0x6EAC40), then executes the event (0x6EF860), which changes the flags and
sends the packet.

Within a leg (the stretch between two events) the position is **closed form**:
0x987B50 computes the displacement from the leg's start (+0x4C) for the time
since it (+0x60):

- straight: `dir * speed * t` (0x9876B0);
- moving while keyboard-turning: the exact arc of a constant turn
  (0x987A00, 0x987820, 0x987950: `speed/omega * (sin, 1-cos)`);
- straight up/down while ascending/descending (case 0x10/0x12).

The collision step (0x762E00) then moves the unit from where it is to that
point, sliding it along what it hits. Catch-up after a stall runs in slices
of at most 250 ms (0x6EA550).

## 2. Speeds (0x987570, `selectSpeed`)

| Flags | Speed |
|---|---|
| no direction | 0 |
| FLYING | flight (+0xA4), or flight-back (+0xA8) when BACKWARD and it is the slower |
| SWIMMING | swim (+0x9C), or swim-back (+0xA0) when BACKWARD and slower |
| WALKING (or a forced walk) | **min(walk, run)** - in every direction, backward included |
| BACKWARD | min(run-back, run) |
| anything else (forward, strafe, diagonal) | run |

Server defaults: walk 2.5, run 7.0, run-back 4.5, swim 4.722222, swim-back
2.5, flight 7.0, flight-back 4.5, turn and pitch pi rad/s. There is **no
acceleration**: a new leg starts at full speed (the speed is chosen when the
flags change, 0x9880C0).

## 3. Direction (0x987EF0 / 0x988490, `moveDirection`)

From the facing `f`: forward `(cos f, sin f)`; backward the negation; strafe
left `f + 90`, right `f - 90` (the two components swapped and one negated);
forward or backward **with** a strafe: the two summed and scaled by
`0.70710677` (0x9E8D48), a unit vector at 45 degrees. **Diagonal speed is the
same as straight**, the forward/strafe speed or, with backward, the backward
one. While swimming or flying with a pitch of at least 1e-6 rad the forward
part is 3D (`cos p` on the plane, `sin p` up); a strafe never is.

The direction and speed are recomputed only when not FALLING, or when a
caller forces it (`0x987EF0(1)`).

## 4. Input (CGInputControl, 0x5F9xxx-0x5FDxxx)

CGInputControl keeps a bit per held control (+4): 0x10 forward, 0x20 back,
0x40/0x80 strafe left/right, 0x100/0x200 turn left/right, 0x1000 autorun,
0x1 right button (mouse steering), 0x2 left button. On every change
0x5FBBC0 re-runs three votes:

- **Forward/back** (0x5FAE70): forward key, autorun and both buttons count
  +1 each, the back key -1. Positive goes forward (`0x72E5D0(t,1)`), negative
  backward, zero stops (`0x71AE10`). So **W+S stands still**, autorun+S
  stands still, autorun+W+S goes forward.
- **Strafe** (0x5FAFB0): strafe left +1, right -1, and with the right button
  held the turn keys count as strafes too.
- **Turn** (0x5FB0B0): the turn keys when the right button is not held.

Autorun: pressing forward or back (the key going down) clears 0x1000
(0x5FA170); so does both buttons going down. Holding a key does not.

The right button steers: on its press, and on every mouse move while it is
held, 0x6023D0 sets the facing to the camera's yaw (0x5FB260 ->
`CGPlayer_C::SetFacing` 0x72EA50 -> event 0x13).

**Mouse look** (0x6020B0, from the mouse-move handler 0x5FBA60 and, per
frame for held analogue input, 0x5FAB70):

- **Delta units.** While the cursor is captured the window procedure
  (0x86A210, WM_MOUSEMOVE with 0xD4156C = 1) reads the cursor with
  GetPhysicalCursorPos (0x868C10), posts OS event 0xC with the integer
  **pixel** difference from the window centre (0xD413EC / 0xD413F0) and
  recentres the cursor (0x869DB0). 0x47FA60 turns event 0xC into the input
  event 0xE carrying the two deltas as floats - the event's +0x24 / +0x28 -
  unscaled. (0x568560 rescales them only in a vehicle-aim mode, 0xB7436C.)
- **Scale.** 0x47C020 divides by the DDC rectangle, the screen with a unit
  diagonal (0x47BF90: width `a / sqrt(a^2 + 1)` at 0xAC0CB4, height
  `1 / sqrt(a^2 + 1)` at 0xAC0CB8; 0.8 x 0.6 at 4:3, 0.872 x 0.490 at 16:9).
  Then `yaw -= dx / ddcW x 1/800 (0xA1E904) x cameraYawMoveSpeed x pi/180`
  (0x5FE5F0 adds its argument, given negated) and `pitch += dy / ddcH x
  1/600 (0xA1E900) x cameraPitchMoveSpeed x pi/180` (0x5FFC20, clamped to
  +-1.5533 rad, 0xAD1B4C/0xAD1B50; a positive stock pitch looks down).
  mouseInvertYaw / mouseInvertPitch (0xC24E70 / 0xC24E6C) multiply by -1
  (0x9E2EF4). At the defaults a pixel turns **0.28125 deg** and tilts
  **0.25 deg** at 4:3 (0.258 / 0.306 at 16:9); 800 pixels across turn 225 deg.
  The held-analogue path uses the same function with `+0x64 x 800 x dt`
  (0xA1DF2C) and `+0x68 x 600 x dt` (0x9E8990), the reference screen.
- **mouseSpeed** (registered 0x401B60 with the OS pointer speed x 0.1 as its
  default; set by 0x4015E0 -> 0x86A070) is clamped to [0.1, 2.0] (0x9E3004,
  0xA4040C), times 10 (0x9E30CC), rounded and handed to
  SystemParametersInfo(SPI_SETMOUSESPEED) while the window is active (0x86A210
  WM_ACTIVATE restores the desktop's on deactivation). The look code never
  reads it: it scales the cursor the delta is measured from, by Windows'
  pointer-speed table (1/32, 1/16, 1/8, then eighths to 1 at 10, quarters to
  3.5 at 20, enhance pointer precision off). Default 1.0 = speed 10 = x1.
- **The sliders** (InterfaceOptionsPanels): Mouse Sensitivity is mouseSpeed,
  0.5..1.5 step 0.05; Mouse Look Speed is cameraYawMoveSpeed, 90..270 step
  10, and its OnValueChanged also sets cameraPitchMoveSpeed to half.
- The CVar globals (0x5FD910): cameraYawMoveSpeed 0xC24E50, cameraPitchMoveSpeed
  0xC24E54, cameraDistanceMoveSpeed 0xC24E58; cameraPivotDXMax 0xC249A8 and
  cameraPivotDYMin 0xC249A4 gate the pivot branch of 0x6020B0.

Click-to-move (`autoInteract`) and its pathing were not traced.

## 5. Turning

Keyboard turning changes the facing by `turnRate * t` (0x987770), **times
0.75 (0x9E9EE4) while moving or falling**, unless flags2 has
FULL_SPEED_TURNING (0x8). Pitching (0x9877D0) is the same with
FULL_SPEED_PITCHING (0x10). Turning in place is the full rate. The mouse
sets the facing directly, at any rate. The player's pitch is clamped to
+-pi/2 (0x9F1FF4 / 0x9E8D88) or a vehicle seat's limits (0x5FB3A0).

**The player's pitch** (CMovement +0x24) is what a swimmer or a flier moves
along (section 3); three things set it:

- **Mouse steering** (input bits 0x2000001: the right button, or mouselook):
  every mouse move 0x6023D0 hands the camera's pitch to 0x5FBE70, which sets
  the player's to it negated (`-(+0x130 + +0x120)`, clamped to +-1.5533) via
  0x5FB3A0 - an event 0x14, **MSG_MOVE_SET_PITCH**, at 0.1 rad from the pitch
  the last packet carried (section 9). The facing goes the same way
  (0x5FB260). Nothing else makes the camera steer: 0x5FA110 / 0x5FA6B0 test
  only those bits, so a swimmer with the keys alone keeps its own facing and
  pitch, and orbiting with the left button turns neither - in water as on
  land.
- **PitchUp / PitchDown** (CGInputControl bits 0x400 / 0x800, Lua
  0x5FC8E0 / 0x5FC570 and 0x5FC920 / 0x5FC5C0): the re-vote 0x5FBBC0 runs
  0x5FB1A0 only while swimming or flying (flags 0x2200000, or flags2 0x20),
  and 0x5FB1A0 does nothing while the mouse steers (0x2000001). Up +1, down -1:
  a positive vote starts **START_PITCH_UP** (0x72E900(t, 1) -> 0x6F1310, event
  0xE), a negative one **START_PITCH_DOWN** (event 0xF), zero **STOP_PITCH**
  (0x72E9B0 -> 0x6EECA0, event 0x10); input bit 0x80000 remembers that a
  pitch is running. Leaving the water or the air with it running stops it
  (0x5FBBC0's other branch). The physics then moves the pitch at the pitch
  rate (+0xB0, pi rad/s), x0.75 while moving unless FULL_SPEED_PITCHING
  (0x9877D0), to +-pi/2.
- Otherwise it holds.

**Drunk drift** (0x6DEB30, run for the active player from the WorldFrame
event-6 handler 0x4F8730, inferred every frame; drunk from 0x4F7290, see
world-lighting.md §8 "Drunk"):

```
tau  = timeMs · (pi/180) · drunk                         // 0x9E2B40
sway = drunk · 0.0130900 · (cos(0.154 tau) + cos(0.195 tau) + cos(0.093 tau)) · 0.333333
                                                         // 0x6CF3D0; 0xA31F90..A0
```
- Only when drunk > 0, the player has control (0x5FA0D0) and moveflags &
  0xF (forward/back/strafe). While a turn key (0x30) is held it also needs
  0x5FA6B0 (mouse steering, input flags 0x2000001).
- `facing = wrap(facing + sway)` (0x72EA50, a real facing change that is
  sent to the server). It is **added every frame**, so the drift rate is
  `sway · fps` (stock is frame-rate dependent: at drunk 1 and 60 fps up to
  about 45°/s); sway itself swings with periods of about 1.8, 2.3 and 3.9 s
  divided by drunk (three cosines beating), so the player weaves rather
  than spins.
- Swimming or flying (moveflags & 0x2200000, or flags2 & 0x20):
  `pitch = clamp(pitch + 4 · sway, -pi/2, pi/2)` (0x9E8D2C, 0x6CF3A0, 0x72EAD0).
- Click-to-move active (0x721F90): 0x7317A0 adds the sway to the CTM's
  facing instead.
- No random strafing, no speed change, no stumble animation.

## 6. Jumping and falling

**Jump** (0x9883F0 -> 0x988370): refused while FLYING, FALLING or ROOT
(0x2001800), with flags2 NO_JUMPING (0x2), or with HOVER unless swimming. It
sets FALLING, clears SWIMMING/ASCENDING/DESCENDING (and the pitch flags unless
flags2 0x20), fall time 0, fall start z, and the vertical speed:

- **-7.955547** (0xAA33DC; down positive, so up 7.955547);
- **-9.096748** (0xAA33E0) when it starts from the water.

**Fall** (0x986F00): distance after t s from vertical speed v0 is
`(g/2 t + v0) t`, g = **19.291105** (0xA32F74), until the speed reaches the
cap, then linear at the cap: **60.148003** (0xB2D9E8), or **7.0** under
SAFE_FALL (Feather Fall, Slow Fall; 0xB2D9EC). 0x988220 inverts it. A jump
rises **1.6404 yd** in 0.4124 s and lands 0.8248 s after it started; from the
water 2.145 yd. Walking off an edge starts a fall with v0 = 0 (0x988370(0)).

**In the air** the direction and speed of the leg are frozen (section 3).
A key pressed in the air does not change them: 0x988A20 / 0x988B00 set a
PENDING flag (0x10000 forward, 0x20000 back, 0x40000/0x80000 strafe;
releasing sets 0x4000/0x8000 pending stop) and 0x6EB3B0 applies them on
landing. **One exception**: a fall that started with no direction flag at all
(a standing jump) takes the first direction pressed, recomputed with the
force-walk speed, **min(walk, run)**. Turning in the air turns the character,
not the path (the arc functions are only used when not falling).

**Landing** (0x988490): clears FALLING, applies the pending flags (0x6EB3B0),
recomputes direction and speed from what is held now.
`MSG_MOVE_FALL_LAND` (0xC9) is sent only when the fall height
(0x986FB0: start z - z, plus `v^2/2g` for a jump) is above **1/36 yd**
(0xA3F854), which is every jump (0x73D4A0). Above 13 or 70 yd the landing also
plays the fall sound and the hard-landing effect (0x73D3D0).

**Fall damage** is the server's (AzerothCore `Player::HandleFall` on
FALL_LAND): from 13.48 yd of `fallStartZ - z`, `0.018 * (dz - safeFall) -
0.2426` of max health (lethal from about 69 yd), none with Feather Fall,
hover or fly auras.

## 7. Collision

**Shape** (0x75CD00): an **axis-aligned box**, not a cylinder: `x,y +- r`,
`z` from the feet to `feet + h`. r = +0xC8, h = +0xCC, set by 0x6E9570:
`r = CollisionWidth * scale / 2`, `h = CollisionHeight * scale` from
CreatureModelData (+0x38 / +0x3C, fields 14/15; 0x6D78C0). Mounted, the box
comes from the mount (0x6D7720). Default before a model: r = 1/3 (0xA12098),
h = 2.0278 (0xA32848). Player models:

| Model | CollisionWidth | r | h |
|---|---|---|---|
| HumanMale | 0.611 | 0.306 | 2.031 |
| HumanFemale | 0.417 | 0.208 | 1.913 |
| OrcMale / Troll / BloodElf | 0.611 | 0.306 | 2.031 |
| DwarfMale | 0.611 | 0.306 | 1.607 |
| NightElfMale | 0.778 | 0.389 | 2.438 |
| ScourgeMale | 0.694 | 0.347 | 1.861 |
| TaurenMale (x display scale) | 0.590 | 0.295 | 1.653 |
| GnomeMale/Female | 0.694 | 0.347 | 1.056 |
| DraeneiMale | 0.867 | 0.433 | 2.541 |

**Query** (0x75FF90): the triangles (WMO, M2, terrain, with liquid as a
separate set while swimming) in the box swept along the step, extended
**below the feet by `dist * tan 50 + step`** and above the head by
`max(dist, 2 * step)`. That downward reach is the ground snap: a walker stays
on any walkable descent.

**Ground walking** (0x7620F0) walks the box across the triangles under it:
it traces along the current triangle (0x75F9D0), and on reaching an edge
asks whether the next triangle is walkable (0x75D1C0):

- **walkable** if its normal z > **cos 50 = 0.6428** (0xA37F0C) for a
  player-controlled unit, > **cos 80 = 0.1736** (0xA37F10) for anything else
  (0x716710: a player not confused, fleeing or server-controlled);
- otherwise it is a wall, and the **step routine** (0x761B00) runs: trace up
  by the **step height** (0x6E9520: the object scale, at least 1, for a
  player-controlled unit - **1.0 yd** normally - and 2.0 otherwise), forward
  by `max(r, step * tan 50)`, and down again; if that lands on walkable
  ground the unit is on the step, else it slides along the wall (0x75E250 /
  0x75E0C0 project the rest of the move onto the wall plane);
- **no triangle under the box**: the unit starts falling with v0 = 0
  (0x988370(0)), keeping its direction and speed.

On ground too steep to stand on the unit is therefore falling, and the air
step (0x7618B0) slides it down. More than five consecutive zero-progress
slices stop the step.

### 7.1 Box size in detail (0x725F50, 0x6D78C0, 0x6D7720, 0x6E9570)

- **Who sizes it.** 0x725F50 runs on a display change (form, morph, mount).
  A player (type mask 0x10) goes to 0x6D78C0, which reads the display's
  CreatureModelData; a model whose width or height is under 9.5e-7 logs
  `NOCOLLISIONBOX` and keeps the old box. A creature without model data gets
  the defaults (width 2/3 at 0x9FC830, so r = 1/3; height 2.0278).
- **Scale** (0x6CF350): `full = (CreatureDisplayInfo.CreatureModelScale x
  CreatureModelData.ModelScale, 0x71C110) x max(1, object scale)` - the
  object scale (OBJECT_FIELD_SCALE_X, vfunc +0x3C) is **clamped to at least
  1**, so a shrink never shrinks the box. 0x6E9570 then sets
  `r = CollisionWidth x full / 2`, `h = CollisionHeight x full` and
  **+0xD0 = full / model scale = max(1, object scale)**, which is the step
  height 0x6E9520 / 0x6E8FC0 return for a player-controlled unit (2.0,
  0xA4040C, for anything else).
- The local player's box only grows when it fits: unless forced, 0x6E8FF0 ->
  0x75E500 tests the new box against the world first (0x6D78C0 returns 0 and
  the old box stays). For a player-controlled unit 0x6E9570 writes h only when
  forced (param 6).
- **Mounted** (0x73D5D0 stores the mount display's CreatureModelScale at
  CGUnit +0x990 and calls 0x6D7720 with the mount model's **MountHeight**,
  CreatureModelData field 16 at +0x40):
  `h = max(riderH, riderH / 2 + mountDisplayScale x MountHeight)`, times the
  rider's full scale; the width is the rider's current one. A human on a
  riding horse (MountHeight 1.866) is 2.88 tall.
- **Hover / step in progress** (0x6E9520): with SPLINE_ELEVATION (0x4000000)
  set the step budget shrinks by `z - +0x88`.

### 7.2 The step (0x761B00)

Called by the ground walker when the next triangle is not walkable:

1. If the blocking surface's normal z is between 0 and cos 50 (a steep slope,
   not a wall), the probe direction is into it: `-(n.x, n.y)` normalised.
2. Trace **up** by the step height (0x6E9520).
3. Trace **forward** by `max(r + 1/720, stepScale x tan 50)` (`stepScale` =
   +0xD0, so 1.19 yd for a scale-1 player, always more than r).
4. If that went the full distance or further than r, trace **down** by the
   climb and require walkable ground there (0x75B690 / 0x7619C0).
5. On success the position is put **back** where it started and SPLINE_
   ELEVATION (0x4000000) is set with +0x88 = the start z: the climb is then
   taken by the ground walker over the following slices, and 0x6E9520 charges
   it against the remaining step. On failure the flag is cleared and the unit
   slides along the wall (0x75E0C0).

So a player climbs anything up to **1.0 yd** (its object scale if larger)
with walkable ground within 1.19 yd beyond the edge, and the box must fit
(the up trace is the box). Nothing in the step depends on race; only r does,
and r is under 1.19 for every playable model.

### 7.3 The ground walker, in detail (0x7620F0)

Per physics slice of `dist` yards and `ms` milliseconds:

1. Trace the box along the current triangle's plane (0x75F9D0 against the
   world set 0xADBA34). On a continuous walkable surface it simply follows
   the plane: there is **no snap and no step** going up or down a ramp, at
   any speed.
2. **It hits a triangle** (index below the set's count 0xADBA38): if that
   triangle is walkable (0x75D1C0: normal z over cos 50 for a
   player-controlled unit) the walk carries on along it; if not, it is the
   **only place a step happens**: 0x761B00 (section 7.2), and on failure the
   rest of the move is projected onto the wall (0x75E250 / 0x75E0C0). More
   than five slices in a row that make under a millisecond of progress end
   the walk. A unit standing still, or moving along a ledge without
   pushing into its side, never steps onto it.
3. **It runs the full distance without a hit** - the box has gone past the
   edge of what it stood on: it is traced **straight down by `dist x
   1.8494`** (0xA32830). A triangle there is landed on (0x6EC7B0; 0x75B690
   decides whether a SPLINE_ELEVATION climb carries on); **nothing starts a
   fall** with v0 = 0 (0x988370(0), unless SPLINE_ELEVATION is climbing a
   step). So the snap is 0.22 yd at a run at 60 fps and grows
   with the slice: a sheer drop deeper than that - a curb, a stair tread
   whose collision has risers - is a short fall, sent as one (section 9,
   event 9), with FALL_LAND when it is over 1/36 yd.
4. No collision set at all (0xADBA38 = 0): a fall.

Wowee's floor pick is not a triangle walker: it moves the feet horizontally
and picks a floor under the centre each frame (with a 0.2 yd footprint in
WMO space), so it reproduces these by thresholds - a rise within the step
height (1.0) is climbed when the centre comes within 0.2 yd of the top's
edge, which is the move pushing into the riser, and a drop is followed
within `max(0.6, dist x 1.8494)` (`stock_movement::walkOffSnapReach`). The
0.6 is not stock (stock falls off a 0.3 yd curb at a run); it is kept so the
stairs Wowee's WMO collision builds with risers are walked down.

### 8.1 The float line and the surface hop (0x75F9D0, 0x760B40, 0x730D10)

- **Float depth.** The swim mover (0x760B40) traces the box against the world
  set (0xADBA34) and, unless the unit may leave the water (0x6E9AD0: flags2
  0x4, or LEVITATING 0x400, or a spline that allows it), against the liquid
  set (0xADBA54, the liquid surface with its normals flipped by 0x75FF90).
  0x75F9D0 shortens the box to **0.75 h** (0x9E9EE4 x +0xCC) when the unit is
  SWIMMING and the set is the liquid one. The surface therefore stops the top
  of a 0.75 h box: **the feet float 0.75 h under the surface** - exactly the
  depth that starts swimming, and 0.028 yd inside the band that keeps it.
  A human's feet float 1.52 under, a gnome's 0.79, a draenei's 1.91. The model
  is drawn at the feet: HumanMale's SwimIdle / Swim root bone moves z by only
  -0.05..-0.17, so the neck pivot (1.6) sits just above the water.
- **Surface hop.** Every frame, a swimmer with ASCENDING whose
  `depth - 0.75 h < 1/3` (0xA12098) calls 0x72EB80 (local player only,
  0x4D43C0). Not mounted it queues a JUMP event (0x6ECC20, event 0xA);
  mounted, only if the mount model has flag 0x400, the unit has CAN_FLY or
  unit flag 0x10000000, or it is moving with a direction. The jump starts
  from the water (0x9883F0(1): 9.096748) and clears SWIMMING / ASCENDING.
  0x760B40 does the same when an ascending swimmer's trace hits the surface.
  Holding space at the surface therefore hops 2.14 yd - enough to land on a
  ledge 0.6 yd above the water - and falls back in.
- **Re-entry** waits for the top of the arc: 0x986EA0 is "FALLING, vertical
  speed nonzero and fall time x 0.001 < speed x -0.0518 (1/g)", i.e. still
  rising; 0x730D10 starts swimming only when it is not.
- **Stop** also needs "not FLYING" (CGUnit +0x7CC & 0x2000000): a flier is
  never taken out of the water by this rule.

## 8. Water (0x730D10, 0x760B40)

Every frame the unit compares the liquid level to its feet (depth `d`) and its
collision height h (unit +0x854):

- not swimming: **start** when `d > 0.75 h` (0x9E9EE4) and the liquid is
  swimmable, unless the fall is still on its way up (0x986EA0);
- swimming: **stop** when `d < 0.75 h - 1/36` (0xA3F854) or there is no
  liquid;
- crossing `d = 0.4 h` (0x9F98D8) plays the splash (0x746720).

A human male (h 2.031) starts swimming in 1.52 yd of water and floats with
that much under the surface. While swimming, the liquid surface is a ceiling
in the collision set (0x75FF90 flips its normals) unless the unit may leave
the water upward; **a swimmer that reaches it while ASCENDING (space held)
jumps** (0x760B40 -> 0x9883F0(1)) with 9.096748, which is how one climbs out
onto a ledge. Breath (the mirror timer) is server-driven (SMSG_START_MIRROR_TIMER)
and was not traced in the client.

## 9. Movement packets

Event -> opcode in 0x6EF860, sent through 0x7413F0 / `SendMovement` 0x71F0C0:

| Event | Opcode |
|---|---|
| 0 / 1 / 2 | START_FORWARD 0xB5 / START_BACKWARD 0xB6 / STOP 0xB7 |
| 3 / 4 / 5 | START_STRAFE_LEFT 0xB8 / RIGHT 0xB9 / STOP_STRAFE 0xBA |
| 0xB / 0xC / 0xD | START_TURN_LEFT 0xBC / RIGHT 0xBD / STOP_TURN 0xBE |
| 0xE / 0xF / 0x10 | START_PITCH_UP 0xBF / DOWN 0xC0 / STOP_PITCH 0xC1 (only swimming or flying) |
| 0x11 / 0x12 | SET_RUN_MODE 0xC2 / SET_WALK_MODE 0xC3 |
| 0xA | JUMP 0xBB |
| 0x13 / 0x14 | SET_FACING 0xDA / SET_PITCH 0xDB |
| 0x15 / 0x16 | START_SWIM 0xCA / STOP_SWIM 0xCB |
| 6 / 7 / 8 | START_ASCEND 0x359 / START_DESCEND 0x3A7 / STOP_ASCEND 0x35A |
| 9 | (walked off an edge) HEARTBEAT 0xEE with FALLING |
| landing | FALL_LAND 0xC9 (section 6) |
| 0x17..0x1F | the speed-change ACKs |

- **Heartbeat** (0x6F09F0): while moving (the 0xC0100F mask), 500 ms after
  the **last movement packet of any kind** - every send resets the timer
  (0x71F0C0 -> 0x6E9B70 `next = now + 500`). The physics is stepped to the
  heartbeat's exact time first. Turning in place, pitching and treading water
  send none.
- **SET_FACING / SET_PITCH** (0x7413F0 case 0xDA/0xDB -> 0x71AE80): dropped
  unless the facing differs by **>= 0.1 rad** (0xA349F0) from the facing the
  last movement packet carried (CGUnit +0xA50, recorded by every send), or,
  swimming or flying, the pitch from +0xA54. No time limit: mouse steering
  sends one every 5.7 degrees. With flags2 FULL_SPEED_TURNING the turn
  start/stop is replaced by a SET_FACING (0x7219F0).
- **Jump block** (0x9872C0 packet builder, flags & FALLING): fall time
  (+0x80), the vertical speed **as stored (-7.955547 for a jump)**, the
  2D direction of travel (+0x70, +0x74: backward/strafe/diagonal included,
  the facing for a standing jump), and the leg's speed (+0x8C: 0 standing,
  walk, run-back...).
- Starting to move clears the sit (any move opcode not in 0x990420 stands the
  player up, 0x7413F0 -> 0x6DCB40(0)); a facing change stands a sleeping
  player up (0x72EA50). Casts: see `combat-targeting-movement.md` 3.

## 10. Camera

Verified (the earlier camera work; constants in `camera_controller.hpp`):
vertical FOV 54 deg (pi/2 * 0.6); start view B 5.55 yd / 10 deg; max zoom
`min(cameraDistanceMax 15 * cameraDistanceMaxFactor, 50)` (0x6000E0); min
zoom 0 (the first-person end, 0x6000E0 local_8); one notch = 1 yd travelled
at cameraDistanceMoveSpeed 8.33 yd/s, linear; pitch +-89 deg (0xAD1B4C);
collision pull-in snaps to hit + 1/9 yd and eases back out over 2 s with
the cosine curve (0x606F90); following = cameraSmoothStyle 4 ("Smarter")
with the 180 / 45 deg/s yaw/pitch speeds, pitch band 0..30 deg, times
0.1..2.0 s.

The other CVars, for reference (0x5FD910): cameraBobbing **0** (no head
bob by default), cameraBobbingSmoothSpeed 0.8, cameraYawMoveSpeed 180,
cameraPitchMoveSpeed 90 (mouse-look, section 4), cameraHeightSmoothSpeed
1.2, cameraGroundSmoothSpeed 7.5, cameraTargetSmoothSpeed 90,
cameraFoVSmoothSpeed 0.5, cameraFlyingMountHeightSmoothSpeed 2.0,
cameraWaterCollision 1, cameraPivot 1 (DXMax 0.05, DYMin 0.00),
cameraDive 1, cameraSurfacePitch 0, cameraSubmergePitch 18,
cameraSurfaceFinalPitch 5, cameraSubmergeFinalPitch 5, cameraTerrainTilt 0,
mouseInvertYaw/Pitch 0.

**The water line** (0x6049C0, every frame from 0x606F90): the liquid level at
the camera's target less its feet, against its collision height h (+0x854).
At most `h - 2/9` (0xA1EA24) sets CGCamera +0x98 bit 0x100000 (at the
surface: the head is out; a swimmer floating 0.75 h under is here), deeper
0x200000 (submerged); out of liquid neither. The CVars (0x5FE010):
cameraWaterCollision 0xC249B4, cameraDive 0xC249A0, cameraSurfacePitch 0
(0xC2499C), cameraSubmergePitch 18 (0xC24998), cameraSurfaceFinalPitch 5
(0xC24994), cameraSubmergeFinalPitch 5 (0xC24990).

- **cameraDive** (0x606F90 for a SWIMMING player whose pitch is not already 0,
  not drunk, no click-to-move; and 0x6023D0 on every steering mouse move):
  at the surface, with cameraSubmergePitch > 0, the player's pitch is set to 0
  (0x5FBE70(0)) unless the mouse steers with the camera looking down by at
  least cameraSubmergePitch - so the keys alone (pitch keys included) and a
  glance down under 18 degrees keep a swimmer on the surface, and looking 18
  or more down dives. Submerged, a negative cameraSurfacePitch does the same
  for looking up (0 by default: off).
- **The final pitches** (0x606F90 with cameraWaterCollision): when the bits
  go from submerged to the surface the camera's pitch eases to
  cameraSurfaceFinalPitch, from the surface to submerged to
  cameraSubmergeFinalPitch (5 degrees down each; 0 is off), with the pitch
  blend 0x601190 over `|change| / cameraPitchSmoothSpeed` (0xC24E38, 45
  deg/s). Walking into water is not a crossing (no bit before).

**The player model fade** (0x606F90's tail -> 0x4F8660): with d the camera's
distance as drawn (after the collision pull-in) less the near clip (CGCamera
+0x38, 0.2 - 0x9E8D84, set by 0x607B00 each frame), the model is opaque from
d = 1.8315 (0xA1E308), clear at d = 1/360 (0xA1E344), and between at
`255 x (1 - cos(pi t)) / 2` (0x8CA080). With the camera nearer than the pivot
height (+0x128) and its pitch below -1.1868 rad (0xA1EA7C: under the
character looking up) the start distance is divided by `((-pi/2 - pitch) x
-2.6043 (0xA1EA78))^2`, so the model fades from further out. 0x4F8660 hides
the model at alpha 0 (0x6CEE50) and draws it at alpha / 255 otherwise
(0x737390). A vehicle's seat model fades by the Vehicle CameraFadeDistScalar
fields instead (same loop; `vehicle_rules.hpp`).

**Held view motion** (MoveView*Start / Stop, 0x5FF000 sets bit `1 << 2i`
in CGCamera +0x160, the start time +0x164 + 4i and the speed argument
+0x1AC + 4i; Stop sets bit `2 << 2i` and the stop time). The per-frame
consumer is **0x6000E0** (from the camera update 0x606F90 while +0x160 is
nonzero), which advances each held motion by the milliseconds since it last
ran, x 0.001 (0xA1E34C):

| i | motion | change |
|---|---|---|
| 0 / 1 | In / Out | target distance +0x1E8 -/+ `ms x cameraDistanceMoveSpeed x speed`, held to the zoom limits |
| 2 / 3 | Right / Left | yaw +0x260 +/- `ms x cameraYawMoveSpeed x speed x pi/180`, wrapped to 2 pi |
| 4 / 5 | Up / Down | pitch +0x230 +/- `ms x cameraPitchMoveSpeed x speed x pi/180`, held to +-1.5533 |

So the turn and tilt run at the **cameraYawMoveSpeed / cameraPitchMoveSpeed
CVars** (180 / 90 deg/s by default, and whatever Mouse Look Speed sets).
Right **raises** the yaw, which a mouse moved right lowers (0x6020B0
subtracts): the camera goes round to the character's right. Up **raises** the
stock pitch, which looks further down: the camera goes up.

**Drunk** (world-lighting.md §8): the camera's FOV offset `+0x138` eases
toward `drunk · 89°` at cameraFoVSmoothSpeed (0.5 °/s; 0x604940, 0x6030E0,
0x603D30); FOV = `clamp(0x40 + 0x138, 0, 179°)` (0x5FF500). A drunk target
also skips the cameraDive surface/submerge pitch limits (0x6023D0,
0x606F90). No other camera effect: no wobble, roll or shake.

## 11. Wowee against this

| # | Stock | Wowee before | Now |
|---|---|---|---|
| 1 | Walking backward is walk speed (min(walk, run)) | run-back 4.5 whenever S was held | `selectSpeed` (camera_controller update) |
| 2 | W+S, autorun+S, both buttons+S stand still; Q+E, A+D cancel | both flags set, zero vector, opcodes for both sent | stock votes (`resolveForwardBack/Strafe/Turn`) |
| 3 | Autorun cleared by the forward/back key going down | cleared every frame the key was held | press edge |
| 4 | Keyboard turn x0.75 while moving or falling | full rate always | `keyboardTurnRate` |
| 5 | In the air the path is fixed in the world; turning turns only the body | the latched keys moved along the current facing, so a turn curved the jump | direction and speed frozen at take-off (`airDir/airSpeed_`) |
| 6 | A standing jump takes the first key pressed in the air, at walk speed | nothing until landing | as stock |
| 7 | Gravity 19.291105, jump 7.955547, closed form | 19.29 / 7.96, semi-implicit Euler (arc depended on frame rate) | exact constants, `fallStep` |
| 8 | Fall speed capped at 60.148 | no cap | capped |
| 9 | Feather/Slow Fall cap 7.0 yd/s | 2.0 | 7.0 |
| 10 | Swim / flight speeds ignore walking; defaults 4.72/2.5, 7.0/4.5 | ground speed x 0.67 (walking slowed a swimmer), back = half | stock pairs |
| 11 | No jump while rooted | jumped | refused |
| 12 | JUMP packet: vertical speed -7.955547, direction of travel, leg speed | +7.96 (stock clients saw the jump start downward), facing (backward/strafe jumps reported forward), run speed for a backpedal | stock fields, in the wire frame |
| 13 | Heartbeat only on the 0xC0100F mask, 500 ms after any movement packet | also while turning in place / treading water; timer reset only by heartbeats | stock mask, reset on every packet |
| 14 | SET_FACING at >= 0.1 rad from the last packet's facing, no time limit | > 3 deg from the last SET_FACING, max 10 Hz | stock rule on WotLK (pre-WotLK keeps the old pacing) |
| 15 | The walk toggle sends SET_WALK_MODE / SET_RUN_MODE and sets WALKING (0x100) | never sent, WALKING never set: the server and other clients saw a walker as a runner | sent on the toggle, flag kept |
| 16 | A swimmer's space/descend is ASCENDING/DESCENDING: START_ASCEND sent, straight up at the swim speed, or 45 degrees at the swim speed with a direction (0x987700) | 8 yd/s vertical on top of the full swim speed, no packet | stock split and packets (no FLYING added in water) |
| 17 | Collision box per model: r = CollisionWidth/2, h = CollisionHeight, times scale; mounted h from MountHeight (7.1) | a 0.45 / 0.5 cylinder 2 yd tall for everyone | `colliderFromModel` / `mountedCollisionHeight`, resolved in `Application::applyServerMovementState`, used by the WMO wall test (radius, height, step) and the doodad test (radius) |
| 18 | Step height max(1, object scale) = 1.0 yd (7.2) | floor pick reached 0.6, while the wall tests ignored anything under 1.0: a 0.6-1.0 riser was walked into | grounded floor pick and slope check reach `collider_.stepHeight` |
| 19 | Swim in at depth over feet > 0.75 h, out < 0.75 h - 1/36, not while rising (8) | water over the floor, 1.15 in / 0.85 out, any race | `startsSwimming` / `keepsSwimming` |
| 20 | Float with the feet 0.75 h under (8.1) | fixed 1.45 | `swimFloatDepth()`; in game a human treads at z -1.524 in sea-level water |
| 21 | Bottom holds the feet; walking out has no lift | swim floor at bottom + 0.5; +1.5 yd/s boost on leaving the water | bottom at the feet; no boost |
| 22 | Ascend held within 1/3 yd of the float line jumps out at 9.0967 (8.1), JUMP sent from the water, no STOP_SWIM | clamped at the surface | the hop, its JUMP, flags cleared by the jump; in game held space hops once a second to feet z +0.616 (rise 2.14) |
| 28 | A key pressed or released in the air sends its packet at once (0x6EF860 sends whatever 0x988A20 / 0x988B00 / 0x98C8D0 / 0x98BF80 did); falling with a direction it only sets PENDING_FORWARD / BACKWARD / STRAFE_LEFT / RIGHT (0x10000..0x80000) or PENDING_STOP / STRAFE_STOP (0x4000 / 0x8000), a pending start cancelled by its stop; the landing (0x6EB3B0) applies them and clears 0x1FC000 | the keys latched at take-off and sent at the landing | for a jump (JUMP sent, the server holds FALLING): sent as pressed; `stock_movement::startForwardBackward / startStrafe / stopForwardBackward / stopStrafe / landWithPendingKeys` keep the server's copy of the flags, FALL_LAND (and START_SWIM) applying the pending ones. In game (debug log of the packets, Goldshire): run, jump, W released in the air: JUMP 0x1001, STOP 0x5001, FALL_LAND 0x0; standing jump, W in the air: JUMP 0x1000, START_FORWARD 0x1001, FALL_LAND 0x1; strafe pressed and released in the air: 0x41001 then 0x1001 |
| 29 | FALL_LAND for a fall height over 1/36 yd (0x73D4A0: 0x986FB0 = start z - z + v^2/2g of an upward start) | after 0.15 s in the air | the height: z + v^2/2g on the first airborne frame (`fallApexZ_`) less the landing z, `stock_movement::fallLandDue`; a reported jump always lands with FALL_LAND. 18 s of running and turning round Goldshire on foot and 24 s mounted across the Elwynn slopes there (the case the 0.15 s was for) sent no FALL_LAND |
| 30 | Every jump sends MSG_MOVE_JUMP (event 0xA, 0x6EF860) as it leaves the ground | none went out for a jump from the ground: the send tested `grounded` after the physics had already cleared it for the take-off, so the server saw only the FALL_LAND (found in game 2026-11-04, debug log of the movement packets) | sent when the physics takes off (`jumpedThisFrame_`) |
| 31 | A knockback (SMSG_MOVE_KNOCK_BACK, event 0x22 -> 0x6E9FF0) is a fall on the packet's direction at its speed, held to the landing like a jump's, FORWARD \| PENDING_STOP; the ack carries that fall; no JUMP (`unit-motion.md` section 6) | a horizontal push that decayed at 4.5/s, plus a MSG_MOVE_JUMP with a normal jump's fields before the ack | `stock_movement::knockBackFlags`, the ack's fall fields from the packet, the jump's latched path (`CameraController::applyKnockBack`) |
| 32 | A server spline of the player's own (a charge, a leap) moves the player as any unit's; the controls and every movement packet wait; `CMSG_MOVE_SPLINE_DONE` at its end; the held keys start again (`unit-motion.md` section 6) | Charge was a dash of the client's own at 25 yd/s, ending elsewhere than the server's spline, with a heartbeat from there | followed from the spline (`MovementHandler::updateServerSpline`) |
| 33 | Mouse look turns `dx / ddcW / 800 x cameraYawMoveSpeed` and tilts `dy / ddcH / 600 x cameraPitchMoveSpeed` degrees per cursor pixel, the cursor scaled by the Windows pointer speed mouseSpeed sets (section 4); two sliders, mouseSpeed 0.5..1.5 and cameraYawMoveSpeed 90..270 (pitch at half) | one "sensitivity" of 0.2 deg per pixel on both axes (0.15 from the renderer until settings loaded), which both stock sliders wrote, over ranges redefined to 0.05..1 | `stock_camera::mouseLook` in `turnByMouse`; settings `mousespeed` (mouseSpeed, default 1.0) and `mouselookspeed` (cameraYawMoveSpeed 180, pitch speed written at half, `mouselookpitchspeed` for cameraPitchMoveSpeed), bound to the three CVars at their stock ranges; the old `mouse_sensitivity` key is not read. The pad turns in degrees (`applyLookDegrees`) |
| 34 | PitchUp / PitchDown, swimming or flying and not mouse-steering, move the player's pitch at the pitch rate (x0.75 moving) and send START_PITCH_UP / DOWN / STOP_PITCH (section 5) | the binding set a command nothing read | `stock_movement::pitchVote / keyboardPitchRate / stepPitch` in `CameraController::update`; the vote's changes send the three opcodes; the server's pitch rate reaches the camera |
| 35 | The camera sets the player's pitch (and facing) only while the mouse steers (0x2000001); a swimmer moves along its own facing and pitch; a steered pitch change goes out as SET_PITCH at 0.1 rad | in water the camera steered facing and pitch whenever a key was held, autorun swam level; SET_PITCH was never sent (the pitch rode along on the next packet) | `movePitchDeg_` for swimming and flight alike: the camera's while steering, the keys' otherwise, held; `MovementHandler::pitchChangeWorthSending` |
| 36 | MoveView turns at cameraYawMoveSpeed and tilts at cameraPitchMoveSpeed (0x6000E0); Right goes round to the character's right, Up goes up | 180 / 90 by name, Right and Up the other way round | `stock_camera::viewMoveStep` at the controller's look speeds |
| 37 | At the surface a swimmer stays level unless the mouse steers it 18 degrees or more down (cameraDive, cameraSubmergePitch); crossing the water line eases the camera to 5 degrees down (section 10) | a swimmer at the surface followed the camera's 3D direction and a float spring pulled it back unless it looked 16 degrees down; the camera kept its pitch at the line | `stock_camera::waterLine / diveHoldsLevel / waterLineFinalPitch`: the dive holds `movePitchDeg_` level, the crossing starts the pitch ease |
| 38 | The player's model fades by the drawn camera distance (1.83 yd to the near clip, cosine), is hidden at 0, and the mount with it | hidden only at a zoom target of 0.65 yd or less, never faded; SetView(1) (0.5 yd) still drew it in the in-game check | `stock_camera::playerModelAlpha` each frame, `isFirstPersonView` is alpha 0, the alpha multiplied into the player's presented alpha (stealth, ghost) and given to the mount (`EntitySpawner::syncUnitPresentation`) |
| 39 | After running off what it stood on, the walker snaps down `move x 1.8494` (0x7620F0), so a walkable descent stays grounded at any speed and frame rate | the walking step-down reached a fixed 0.6: on a steep walkable slope a fast mount, or a low frame rate, lost the ground and fell | `max(0.6, stock_movement::walkOffSnapReach(move))`; the 0.6 floor is Wowee's (7.3) |
| 40 | A fall that starts without a jump (walked off an edge) is reported at once: HEARTBEAT 0xEE with FALLING, v0 0, the leg's direction and speed (event 9, 0x988370(0)); keys in the air then go out pending, as in a jump | not reported (no FALLING went out), so its keys stayed latched and were sent at the landing, and its landing was a FALL_LAND by height alone | `MovementHandler::beginWalkOffFall` (from `CameraController`'s fall tracking once the fall has dropped 1/36; the fall time backdated to the edge), the jump's key rules from then on |
| 41 | No FALL_LAND for a landing within 1/36 yd of the fall's apex (0x73D4A0), a jump onto a ledge at the top of its arc included; the landing still clears FALLING and applies the pending keys | a reported jump always sent FALL_LAND (it was what cleared FALLING here), and a jump that met a slope while still rising sent nothing and left FALLING set | `fallLandDue` for every reported fall, `MovementHandler::landWithoutPacket` otherwise; the landing is taken the frame the fall ends, rising or not |

Still different (not changed here; each needs in-game checking):

- **Step mechanics** (7.2, 7.3). Stock steps only when the move hits an
  unwalkable triangle, and climbs over the next slices; Wowee's floor pick
  climbs a top within 1.0 yd when the centre comes within 0.2 yd of its edge
  (its WMO footprint), which in practice is the move pushing into the riser,
  but it does so strafing along a ledge closer than 0.2 yd too, and it does
  not need the stock's walkable ground 1.19 yd beyond the edge. Going down,
  Wowee keeps a 0.6 yd floor under the stock snap (7.3), so curbs and stair
  treads are walked down where stock takes a short fall. The other 0.6
  tolerances (`movement_limits.hpp` `kMaxStepUp`: the floor-arbitration
  anchor, the walkable-structure test, the airborne lip catch, the transport
  floor test) are Wowee's seam heuristics with no stock counterpart; they
  were left as they are.
- **Box against cylinder.** Stock is an axis-aligned box (corners reach
  r x 1.41); Wowee sweeps a cylinder of radius r. The doodad test
  (`m2_renderer_instance.cpp`) still uses a fixed 2 yd height and 1 yd step;
  only its radius follows the model.
- **Swim ledge exits.** Stock swimming has no step: a swimmer gets onto a
  ledge only by the hop. Wowee keeps an exit to walking when a ledge within
  the step lies under or ahead of a swimmer at the surface and the water over
  it is shallower than the stock exit depth, because its wall tests leave
  such a ledge to the floor pass, which a swimmer does not run. Removing the
  exit alone would let a swimmer drift into the ledge rather than stop at
  it: the stock behaviour needs `sweepAgainstWalls` to treat a ledge side as
  a wall while swimming (step 0, as 0x760B40's swim trace has none), which
  changes collision for every swimmer and was not done here.
- **Steep ground.** Stock is FALLING on it and the air step slides the box
  down; Wowee slides terrain only, with its own acceleration.
- **When a walk-off fall is reported**: stock sends its HEARTBEAT the slice
  the walker finds no ground (7.3); Wowee sends it once the fall has dropped
  1/36 yd (54 ms from rest), so a frame's loss of contact that drops less -
  which Wowee's floor pick has more of than the stock walker - says nothing.
  The fall time it carries counts from the edge either way (#40).
- Click-to-move was not traced.

## 12. Flight allowed, teleports and the world port

**CAN_FLY** (0x01000000) is an ability, FLYING (0x02000000) the state.
SMSG_MOVE_SET_CAN_FLY / UNSET_CAN_FLY queue movement events 0x2F / 0x30
(run by 0x6EF860): 0x2F sets CAN_FLY; 0x30 clears it and, when FLYING was
set, first stops the flight (0x98C240: clears 0x02C000C0 - FLYING,
ASCENDING, DESCENDING, the pitch keys - and starts a fall, 0x988370). Both
then acknowledge with CMSG_MOVE_SET_CAN_FLY_ACK (0x345). `.gm fly on`
(AzerothCore `Unit::SetCanFly`) is exactly this packet, the same one a
flying mount's aura sends; nothing else distinguishes the two.

**A teleport on the same map** (MSG_MOVE_TELEPORT, 0x72D2D0 for the active
mover) reads the movement block and queues event 0x2C; its handler takes
only flags2 0x2040 from the packet and relocates the mover (0x6ECF80,
0x6EC400 for the transport). CMovement's flags (+0x44) are **kept** -
CAN_FLY, water walk, feather fall and the rest survive a `.go` or a summon.
The acknowledgement is MSG_MOVE_TELEPORT_ACK (0xC7) from the event.

**A world port** (SMSG_NEW_WORLD to another map): the client loads the new
map and only then (0x403B70, the load's completion) puts the pending spell
down (0x809A60), resets the world, moves the camera to the destination and
sends **MSG_MOVE_WORLDPORT_ACK (0xDC), empty**. No movement packet goes with
it: the server (HandleMoveWorldportAck) places the player at the teleport
destination itself, and the next movement packet the client sends carries
the new position. AzerothCore also drops CAN_FLY across a map change unless
a fly aura holds it, and says so with SMSG_MOVE_UNSET_CAN_FLY after the ack.

**SitStandOrDescendStart / DescendStop** (the SITORSTAND binding, X):
descend while swimming or flying, sit or stand otherwise.

| # | Stock | Wowee before | Now |
|---|---|---|---|
| 23 | A teleport keeps CMovement's flags | MSG_MOVE_TELEPORT_ACK set them to 0: after `.gm fly on`, any `.go`/same-map `.tele` lost CAN_FLY and Space jumped | moving bits cleared; WALKING, LEVITATING, ROOT, CAN_FLY, WATER_WALK, FEATHER_FALL, HOVER kept |
| 24 | An unmounted flier plays the swim set (SWIMMING or FLYING) | the on-foot animation had no flight: a `.gm fly` character hung in the fall pose | `fi.swimming` covers flight in the air |
| 25 | X descends in the air: START_DESCEND (0x3A7), for any flier | X toggled a sit in mid-air, and the sit held every movement key; and START_DESCEND went out only for a mounted flier or a swimmer, so a `.gm fly` character sank on screen while the server kept it where it was | no sit while swimming or flying; START_DESCEND for any flier; `SitStandOrDescendStart` / `DescendStop` added to Lua |
| 26 | UNSET_CAN_FLY in the air stops the flight (0x98C240) | only CAN_FLY cleared: FLYING stayed set for good, since the landing that clears it runs only while flight is allowed | FLYING, ASCENDING, DESCENDING and the pitch keys cleared with it, before the ack |
| 27 | WORLDPORT_ACK alone, after the load; no movement until the player moves | ACK, then STOP and HEARTBEAT; the frame after, the per-frame sync copied the old map's render position and facing into movementInfo and a SET_FACING went out with them - the server moved the player to the old x, y, z on the new map (`.gps` after `.tele` to another continent) | ACK alone; movement held and renderer positions ignored from SMSG_NEW_WORLD until the world loader has placed the character (`MovementHandler::endWorldPortHold`, a minute at most) |
