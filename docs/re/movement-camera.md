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
`CGPlayer_C::SetFacing` 0x72EA50 -> event 0x13). Mouse-look rotates the
camera by `dx / 0.8 * 0.00125 * cameraYawMoveSpeed (180) deg` and
`dy / 0.6 * 0.0016667 * cameraPitchMoveSpeed (90) deg` (0x6020B0; dx/dy are
the event's normalised deltas, +0x24/+0x28), inverted by mouseInvertYaw /
mouseInvertPitch.

Click-to-move (`autoInteract`) and its pathing were not traced.

## 5. Turning

Keyboard turning changes the facing by `turnRate * t` (0x987770), **times
0.75 (0x9E9EE4) while moving or falling**, unless flags2 has
FULL_SPEED_TURNING (0x8). Pitching (0x9877D0) is the same with
FULL_SPEED_PITCHING (0x10). Turning in place is the full rate. The mouse
sets the facing directly, at any rate. The player's pitch is clamped to
+-pi/2 (0x9F1FF4 / 0x9E8D88) or a vehicle seat's limits (0x5FB3A0).

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
`MSG_MOVE_FALL_LAND` (0xC9) is sent when the fall height
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
mouseInvertYaw/Pitch 0. Not traced: what cameraDive and the
surface/submerge pitches do to the view at the water line, and the
first-person model fade.

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

Still different (not changed here; each needs in-game checking):

- **Step mechanics.** Stock steps only when a wall blocks and climbs over the
  next slices (7.2); Wowee's floor pick now reaches the stock 1.0 yd every
  frame, which also lifts the player onto a walkable top within 1 yd that it
  stands beside rather than walks into. The heuristics around the floor pick
  (`movement_limits.hpp`, `kMaxStepUp` = 0.6) still use 0.6 as their ground
  tolerance.
- **Box against cylinder.** Stock is an axis-aligned box (corners reach
  r x 1.41); Wowee sweeps a cylinder of radius r. The doodad test
  (`m2_renderer_instance.cpp`) still uses a fixed 2 yd height and 1 yd step;
  only its radius follows the model.
- **Swim ledge exits.** Stock swimming has no step: a swimmer gets onto a
  ledge only by the hop. Wowee keeps an exit to walking when a ledge within
  the step lies under or ahead of a swimmer at the surface and the water over
  it is shallower than the stock exit depth, because its wall tests leave
  such a ledge to the floor pass, which a swimmer does not run.
- **Steep ground.** Stock is FALLING on it and the air step slides the box
  down; Wowee slides terrain only, with its own acceleration.
- **Air keys**: stock sends START_FORWARD etc. in the air (with the pending
  flag) and applies them on landing; Wowee sends them on landing.
- **FALL_LAND** after any fall over 1/36 yd; Wowee needs 0.15 s airborne
  (kept to avoid landings from ground-contact flicker at speed).
- **Mouse-look scale**: the stock formula (section 4) is in normalised
  screen units; Wowee's is a pixel sensitivity slider. Not reconciled.
- Click-to-move, cameraDive / submerge pitch and the first-person fade were
  not traced.

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
