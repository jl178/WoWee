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

Still different (not changed here; each needs in-game checking):

- **Step height.** Stock steps up 1.0 yd (scale-1 player) only when a wall
  blocks; Wowee's floor picker accepts any floor within `kMaxStepUp` = 0.6
  of the feet every frame and has many heuristics built on that number
  (`movement_limits.hpp`). Raising it to 1.0 would also lift the player onto
  low overhead floors in WMOs; it needs the wall-triggered step to be safe.
- **Collision shape.** Stock is an axis-aligned box of r = CollisionWidth/2
  and h = CollisionHeight per model; Wowee sweeps a fixed radius against WMO
  walls and M2s. Per-race sizes are in the table in section 7.
- **Swim depth.** Stock starts swimming at 0.75 h of water over the feet
  (1.52 yd for a human, 0.79 for a gnome) and floats there; Wowee uses fixed
  1.15 / 0.85 water-over-floor thresholds and a fixed 1.45 surface offset,
  plus ramp-exit assists. Needs the model's collision height in the camera
  controller.
- **Jumping out of the water** (ascend into the surface jumps with 9.0967):
  Wowee clamps at the surface.
- **Air keys**: stock sends START_FORWARD etc. in the air (with the pending
  flag) and applies them on landing; Wowee sends them on landing.
- **FALL_LAND** after any fall over 1/36 yd; Wowee needs 0.15 s airborne
  (kept to avoid landings from ground-contact flicker at speed).
- **Mouse-look scale**: the stock formula (section 4) is in normalised
  screen units; Wowee's is a pixel sensitivity slider. Not reconciled.
- Click-to-move, cameraDive / submerge pitch and the first-person fade were
  not traced.
