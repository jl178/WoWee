# How other units move (stock 3.3.5a, build 12340)

How `Wow.exe` 12340 moves and animates every unit it does not control -
creatures, other players, pets, playerbots - from `SMSG_MONSTER_MOVE` splines
and from other clients' `MSG_MOVE_*`, and where Wowee differed. Read with
Ghidra; every rule names its function. The code is
`include/game/unit_motion.hpp` (the pure model), `src/game/unit_motion_glue.cpp`
(packets in, events out) and `Entity::startMonsterMove` /
`receiveRemoteMovement` (`src/game/entity.cpp`). Tests: `tests/test_unit_motion.cpp`.

Coordinates in this document are the server's (x north, y west, z up; facing
counter-clockwise from +x, the wire orientation).

## 0. Where things are

`CGUnit_C` embeds its `CMovement` at `+0x788` (`unit[0x36]`, `+0xD8`, points
to it). The fields used below, as offsets into `CMovement`:

| Offset | What |
|---|---|
| `+0x08` | transport guid |
| `+0x10` | position |
| `+0x20` / `+0x24` | facing / pitch |
| `+0x44` / `+0x48` | movement flags / flags2 (`0x80000000` of `+0x44` is "timing initialised") |
| `+0x4C` / `+0x58` / `+0x5C` | position, facing, pitch at the current motion's start |
| `+0x60` | ms since that start |
| `+0x64` | 3D direction of travel; `+0x70` its horizontal part; `+0x78`/`+0x7C` pitch cos/sin |
| `+0x80` / `+0x84` | fall time / z the fall started from |
| `+0x88` | spline elevation |
| `+0x8C` | current speed (0x987570) |
| `+0x90..+0xB0` | walk, run, run back, swim, swim back, flight, flight back, turn rate, pitch rate |
| `+0xB8` | z speed of a jump or fall (negative is up: a jump starts at -7.955547) |
| `+0xBC` | the MoveSpline (below) |
| `+0xC0` / `+0xC4` | local time the last remote packet was applied / its sender timestamp |
| `+0xE8` / `+0x128` / `+0x12C` | jitter ring (32 shorts), its index, the held jitter |
| `+0x134` | ms since the last remote packet |
| `+0x140` | queue of movement events waiting for their time |

The MoveSpline (`+0xBC`): `+0x20` flags, `+0x24` start time, `+0x28` time
passed, `+0x2C` duration, `+0x30` id, `+0x34` the curve object (vtable
`0x9E2F28`), `+0x1F4` "smooth" (Catmull-Rom), `+0x1F8` final point, `+0x204` /
`+0x208` duration mod / next mod, `+0x20C` vertical acceleration, `+0x210`
effect start time.

Handlers (registered at `0x742200`-`0x742A00` through `0x6B0B80`):

| Opcodes | Handler | Work |
|---|---|---|
| `SMSG_MONSTER_MOVE` 0xDD, `_TRANSPORT` 0x2AE | 0x73F590 | 0x73C8E0 |
| `MSG_MOVE_*` (0xB5-0xC5, 0xC9-0xCB, 0xDA, 0xDB, 0xEE, ...) | 0x741B60 | 0x740D30 |
| `MSG_MOVE_SET_*_SPEED` | 0x741B00 | 0x7406A0 |
| `SMSG_FORCE_*_SPEED_CHANGE` etc. | 0x732450 | 0x7307A0 |
| `SMSG_SPLINE_MOVE_*` (0x304-0x30E, 0x31A, 0x422..) | 0x741C30 | 0x740BA0 |
| `SMSG_SPLINE_SET_*_SPEED` (0x2FE-0x303, 0x385, 0x386, 0x45E) | 0x741BC0 | 0x740A60 |
| `SMSG_COMPRESSED_MOVES` 0x2FB | 0x716AD0 | 0x7169A0 (each sub-packet through the table) |

The movement, spline-move and speed handlers end in `0x73ED10(opcode)` (the
movement event: jump grunt, JumpStart, landing -
`docs/re/combat-targeting-movement.md` section 3) and `0x73AC30` (re-pick the
animation) - for a remote packet only once it has been applied (section 2);
the monster move ends in the re-pick.

## 1. SMSG_MONSTER_MOVE (0x73C8E0)

### Reading

0x73F590: packed mover guid; for 0x2AE a packed transport guid and a seat
byte; a byte (`0x98B5B0`: sets or clears flags2 0x40); then 0x74C040, which in
some states (0x74BCB0 decides; not traced further) keeps the packet to replay
later through 0x747990, else 0x73C8E0. That one first puts the unit on (or off) the transport
(`0x6F0C70`; `SMSG_MONSTER_MOVE` means no transport), then reads the start
position, the spline id, the move type and the facing data, the flags, the
animation (`flags & 0x200000`: type byte, start time), the duration, the
parabolic part (`0x800`: vertical acceleration, start time) and the nodes.

Nodes are twelve bytes each exactly when `flags & 0x42000` (Catmull-Rom or
Flying); otherwise the destination comes first and the middle nodes are packed
deltas from the midpoint of start and destination (`0x7152B0`: 11/11/10-bit
signed, times 0.25, subtracted). AzerothCore writes them under the same test
(`Mask_CatmullRom`).

### The path (the client builds its own)

- **Stop (type 1).** Within `pathDistTol` of the unit (CVar at `0xCA1194`,
  default 1 yard) it is stopped where this client has it (`0x6F11B0`: the
  end-snap below never fires). Further off it slides there: a spline
  `[unit - facing dir, unit, stop, stop]` with flags `0x1000`, no duration -
  so at the speed cap.
- **Ground (linear).** `[start, nodes..., destination]` (one node: start and
  destination, or start alone when they are within 1/36 yard). Then
  **0x7180C0 trims it to the unit**: segments the unit has passed (by planes
  through it normal to each segment) are dropped and the path starts at the
  unit's foot on the segment it is on - or at the unit itself, prepended, when
  it has not reached the path (more than a yard away on the map), or
  `[unit, last node]` when it is past all of it. Then padded: one point before
  (the first reflected through the second), the last repeated after.
- **Smooth (Catmull-Rom or Flying).** `[unit - (cos f, sin f, 0), unit,`
  nodes`...]` - the curve leaves the unit along its facing - skipping the
  first node when it is within 1/36 yard of the unit; then the last node
  repeated, or for a cyclic spline the first two nodes after the unit (the
  closing segment's controls).

So **a new spline never snaps the unit**: it always starts from where this
client draws it (except the sideways step onto a ground segment it is beside).

### Duration and speed cap

The chord length `L` of the real segments; nothing under 1/6 yard is run. The
speed is `min(cap, L / server duration)` where the cap is
`max(28, 4 * run speed)` for a ground spline and 50 for a smooth one (`0xA34EE8`,
`0x9E8D2C`, `0x9F22EC`; AzerothCore limits its own speed the same way). The
client's duration is `round(L / speed * 1000)`, at least 1: the server's own
unless it implies more than the cap, and the cap's when the server gave none.

A path that is not run (too short, a `TransportEnter`/`TransportExit` spline -
0x6EB680 refuses those - or a rooted unit's non-jump) turns the unit to the
asked facing at once and leaves it, putting it on the last node only when that
is 3 yards or more away (`0x6E9470`).

0x6EB680 then: `0x9870F0` (final point = the last padded point), `0x98C770`
(flags, start time = now, time passed 0, duration, smooth = `flags & 0x42000`,
curve init, mods 1.0; a Falling spline sets the unit falling with z speed 0 from
where it is - `0x988370`). The facing data goes in the spline (`0x6E9640` spot
flag 0x8000, `0x6E9670` target 0x10000, `0x6E96A0` angle 0x20000). Parabolic
(`0x6E96C0`): acceleration and start time; start 0 raises the jump at once.
Animation (`0x6E9780`): the tier and its start time; start 0 changes the tier
at once (`0x73AF00`). Landing from flight or hover (tier 3 with type 0 or 2, or
tier 2 with 0) moves the start so FlyToGround (463) ends with the spline.

Finally the unit stands up if it sat, its bow pull is cleared and the
animation is re-picked.

### The curve (vtable 0x9E2F28)

Points are stored with the virtual ones; segment `i` runs from point `i+1` to
point `i+2`. A segment's length is its chord (linear) or twenty chords of the
curve (0x4C3B10). Evaluation (`0x4C43B0`) takes `t` in [0,1] over the **whole
length**: the segment is found by accumulated length (0x4C3BD0) and `u` is
linear within it. Position: `lerp` (linear) or Catmull-Rom with tension 0.5
(basis at `0xAC37B8`; derivative basis `0xAC37F8`). Direction: the chord's,
or for a curve its derivative when that is within 60 degrees of the chord
(dot >= 0.5).

### Each frame (0x98CA00, called from 0x6E9C30 in CMovement::Update 0x6EAC40)

- Frozen (`0x400000`): nothing moves.
- `passed = now - start`; `d = floor(duration * mod + 0.5)`. `d == 0`: done.
  `passed < 0`: t = 0. `passed < d`: `t = passed / d`. Otherwise done, or
  for a cyclic spline the next lap starts (`passed -= d`, start moved on); with
  Enter_Cycle the first point is dropped once (0x98C940: the spline is re-made
  from point 1 on, same duration) and the flag cleared; the mod becomes the
  next mod.
- Facing: unless Falling or OrientationFixed (`0x4200`), the travel direction
  when its XY length is over 0.043 (`0xAA33E4`); OrientationInversed adds pi.
  So **a moving unit faces along its path every frame**, curves included.
- Swimming or flying units pitch to the direction; a Flying spline on the
  ground-flag unit banks by twice the turn ahead (1 s look-ahead), clamped to
  +-90 degrees.
- Animation: at `passed > start`, flags2 0x100 is set and 0x6E9C30 changes the
  tier (0x73AF00: ground -> fly or hover plays ToFly 458; fly or hover -> ground
  plays ToGround 460).
- Parabolic: at `passed > start` the jump begins (flags2 0x80; 0x6E9C30 raises
  `MSG_MOVE_JUMP`, so JumpStart and the grunt play). z is the curve's plus
  `a/2 * ts * (D - ts)` with `ts`, `D` the seconds since and the duration after
  the effect start (0x987D20; AzerothCore's computeParabolicElevation).
- Falling: z is `fall start z - fall distance(passed)` (gravity 19.2911,
  terminal 60.148), not below the final point's z; reaching it ends the spline.
- 0x6E9C30 then moves the unit there: with no collision (0x6E9E20 sets the
  position directly for a spline mover) - **there is no ground snap of a
  spline**: z is the server's path. A step over 60 yd/s or more than 3 yards a
  frame is a teleport, not a move.

### The end (0x6EB0B0)

Forward is cleared (0x98BD10), the unit is put on the final point only from 3
yards off, the final facing applied - an angle as is, a spot by the angle to
it, a target by the angle to the unit (`0x98C480`) - the spline marked
finished (0x6EAE70; a parabolic one lands, `0x73D3D0`), and `MSG_MOVE_STOP`
raised (re-pick: Stand), or the landing for a falling/parabolic one.

**Facing target is applied at the end, not the start**: a chasing mob runs
facing along its path and turns to its target when it arrives.

### A new spline mid-move

Nothing is blended: the old one is replaced, and because the new path starts
at the unit (above) there is no jump in position. The facing changes at once
to the new path's direction (the body then follows at its own rate, section 4).

### UPDATE_OBJECT

A create block with `MOVEMENTFLAG_SPLINE_ENABLED` carries the server's spline
as it runs: flags, facing data, time passed, duration, id, duration mods,
vertical acceleration and effect start, the padded nodes, mode and end point.
The unit continues it from `now - time passed`.

## 2. Other players' movement (MSG_MOVE_*)

### Reading (0x4F4D40)

flags (u32), flags2 (u16), time (u32), x, y, z, facing; with
`ONTRANSPORT` 0x200 a packed transport guid, local x, y, z, facing, time
(u32), seat (i8), and with flags2 `0x400` (interpolated movement) a second
time; pitch when swimming or flying (`0x2200000`) or flags2 `0x20`; fall time
(u32); with `FALLING` 0x1000 z speed, then the jump direction's x and y (the
client stores them as its `+0x70`/`+0x74` direction; AzerothCore calls them
sin and cos), and the horizontal speed; with `0x4000000` the spline elevation.

### When a packet is applied (0x6ED990 -> 0x6EB730)

Remote packets are **not applied when they arrive**: each is scheduled on
the sender's own spacing.

- The first one initialises: last sender time, last local apply time = now.
- `dtPacket` = sender time - last sender time (0 if not later); `dtLocal` =
  now - last apply time; `delay = dtPacket - dtLocal`. So the packet is due at
  `last apply + dtPacket`: the gap the sender left.
- A ring of the last 32 values of `hold - dtPacket + dtLocal` (`0x6E97D0`) -
  how late each packet was - and its maximum. When the unit is standing still
  and nothing is queued (a new start of movement), the delay grows by
  `max - hold` and `hold = max`: **a jitter buffer** sized by the worst recent
  lateness, taken on at the start of each move.
- The delay is clamped to [-500, +1000] ms. Due in the future: queued
  (`0x6EC8B0`) and applied at its time (`0x6ED0F0`). Otherwise applied now,
  and the motion is run on from its due time to now (`0x6EA550`, 250 ms steps
  through collision).

Applying (`0x988920`, `0x988990`): position, facing, pitch, flags (all but the
spline and transport bits), flags2, spline elevation - **set, not blended**:
the unit is put where the packet says at the packet's time; and it becomes the
start of the motion (`+0x4C`, `+0x58`, time 0). Falling: the fall time, z
speed, jump direction and horizontal speed, and the z the fall started from.
`+0x134` (time since packet) is zeroed.

There is no smoothing of corrections in position. Because the packet is
applied at its sender's time, the correction is the sender's own error, which
is small; a packet that was late is run on to now.

Flags2 `0x400`/`0x800`/`0x1000` (interpolated movement/turning/pitching, set by
the sender) make 0x6EA7E0 blend toward the next queued packet instead; players
do not set them.

### Between packets (0x6EAC40 -> 0x6E9F50 -> 0x987B50)

The unit runs on its flags. The time used is `+0x60` (since the last start),
except that **after 500 ms without a packet a translating remote unit slows
down**: 0x6E9F50 subtracts `max(0, s - 1500) + min(s - 500, 1000)^2 / 2000`
(`s` = ms since the packet), i.e. full speed for 500 ms, then a linear slowdown
to rest over the next second. It keeps its flags (its run animation keeps
playing in place until the next packet).

Displacement from the start, by flags (0x987B50):

- Speed (0x987570): flying - flight (flight back when backward and slower);
  swimming - swim (swim back); walking - min(walk, run); backward - run back
  when not faster than run; else run. Falling: the jump's horizontal speed.
- Direction (0x987E30, 0x987EF0): the start facing (with pitch when swimming or
  flying); strafing alone turns it a quarter left or right; forward or back with
  a strafe is the diagonal (sum times 0.7071); backward negates.
- Straight: direction * speed * t.
- **Turning while moving goes round a circle** (0x987A00): angular rate
  `w = +-turn rate`, times **0.75 while translating** unless flags2 `0x8` (full
  speed turning); radius `speed / w`; ascending/descending tilt it 45 degrees.
  Facing is `start + w t` (0x987770) - turning is continuous, not stepped.
- Pitching (0x987950) and turning while pitching (0x987820) are the same in
  the vertical; ascending or descending alone move straight up or down at the
  speed, with a direction at 45 degrees.
- **Falling**: horizontally the jump's direction times its speed; vertically
  `z = fall start z - fall distance(fall time + t)` with the packet's z speed
  (0x986F00: `v0 t + g/2 t^2` until terminal velocity 60.148, or 7 with
  feather fall). A jump (`0x9883F0`) starts at -7.955547 yd/s (from the water
  -9.096748), so it peaks 1.64 yards up after 0.41 s.

The result goes through collision (0x6E9E20 -> 0x762E00), which keeps the
unit on the ground and stops a fall on the floor.

### Heartbeats

A moving stock client sends a heartbeat 500 ms after its last movement packet
(0x6F09F0, while the `0xC0100F` mask is set) and a packet on every change of
state (the local-player side is `docs/re/movement-camera.md`). With the 500 ms
grace above, a remote runner never visibly slows between heartbeats.

## 3. Speeds

- `SMSG_SPLINE_SET_*_SPEED` (0x740A60 -> 0x6E9110..): sets the speed (e.g.
  0x98A0B0: run at `+0x94`), re-bases the motion, recomputes `+0x8C`; then the
  movement event and a re-pick. A unit on a spline keeps its duration: the
  speed changes the animation only.
- `MSG_MOVE_SET_*_SPEED` for another player (0x7406A0): queued with the
  packet's timing like any movement packet.
- `SMSG_FORCE_*_SPEED_CHANGE` (0x6EDB30): queued as an event (0x17..) and
  applied in order.
- `SMSG_SPLINE_MOVE_SET_RUN_MODE` / `WALK_MODE` (0x6E90D0 -> 0x98B0E0): clear /
  set `WALKING`.

## 4. Animation of a moving unit

- **Walk, run or sprint** (0x717050, after swim/fly/backward/stealth): by the
  speed 0x987570 gives - a spline's being its length over its duration -
  above 11 yd/s Sprint (143), above **twice the unit's walk speed** Run (5),
  otherwise Walk (4). A spline's WalkMode flag only chose the server's speed;
  the animation follows the pace. A slowed player (run speed under 5) walks.
- **Playback rate** (0x7385C0): a moving unit's animation with a MoveSpeed
  plays at `speed / |MoveSpeed|`; switching between two moving animations
  keeps the phase (`time * newLength / oldLength`). A speed change re-picks,
  which re-applies the rate.
- **The body turns** (0x73DAB0) toward the movement facing at
  `8 * turn rate` rad/s, and is never more than a quarter turn behind (the
  excess is taken at once); the remaining twist goes to the spine and head
  bones. Turning in place plays ShuffleLeft/Right (0x71E180) - for a unit whose
  body is turning with no direction flag.
- Animation tier changes (0x73AF00) play ToFly (458) and ToGround (460).

## 5. Transports

A unit on a transport keeps its CMovement in the transport's frame: the
monster move's nodes and a remote packet's position are transport-local, the
path trim transforms the unit's position into that frame (0x7180C0 via
0x74B4C0), and the world position is composed each frame from the
transport's. Its motion therefore follows the deck while the ship moves.

## 6. Wowee against this

| # | Gap (before) | Stock | Fix |
|---|---|---|---|
| 1 | Flying-only splines (`0x2000`, every flying creature's move: `EnableFlying` clears Catmullrom) were read as packed deltas - the whole path misread | 12-byte nodes iff `flags & 0x42000` | `SplineFlagWotlk::UNCOMPRESSED_MASK = CATMULLROM \| FLYING` |
| 2 | A multi-node ground path was drawn as a Catmull-Rom curve, cutting corners (into walls, off bridges) | linear unless `0x42000` | `SplineCurve` linear/Catmull by flags |
| 3 | Key times by distance per node, then Catmull `u` per segment | uniform over the whole arc length (20-chord segment lengths for curves) | same |
| 4 | A new spline started at the server's start unless the model was within 4 yards, else snapped; no trim of passed nodes | always from the unit: trim (0x7180C0) for ground paths, the unit + its facing pad for smooth ones | `planMonsterMove` |
| 5 | Server duration always used | `min(cap, L/duration)`, cap `max(28, 4*run)` / 50 | same |
| 6 | Facing set once to the destination at the start (and to the target at the start for type 3; flipped toward travel if over 90 degrees off) | every frame along the path; final facing (spot/target/angle) at the end; OrientationFixed/Inversed honoured | `UnitSpline::update`, `Entity::updateStockMotion`, `processStockMotionFrame` |
| 7 | MoveJump (parabolic) flat; no jump animation | `a/2 t (D-t)`, JumpStart at the effect start, landing at the end | same; FALLING flag and the Jump hint for the locomotion |
| 8 | Falling splines flat | gravity to the end z | same |
| 9 | Cyclic and Enter_Cycle ignored (the spline just ended) | laps; way in dropped after the first | same |
| 10 | Stop: snapped to the packet | stays within 1 yard; slides at cap speed beyond | same |
| 11 | Walk/run from the WalkMode flag or a 4.75 yd/s guess | above twice the unit's walk speed runs, above 11 sprints | `afterMonsterMove`, `driveUnitLocomotion` |
| 12 | Animations at rate 1 whatever the speed | speed / sequence MoveSpeed | `syncLocomotionRate` |
| 13 | Ground creatures on splines projected to the floor every frame | no ground test for spline movers | sync loop skips it on a stock spline |
| 14 | Remote players: linear interpolation over an estimated packet interval, 250 ms extrapolation, then a renderer glide on top (two interpolations) | jitter-buffered, applied on the sender's spacing, dead-reckoned by flags/speeds (turn arcs at 0.75, strafe diagonals, backpedal speed), 500 ms full speed then a 1 s slowdown | `RemoteMover`; renderer draws the entity position |
| 15 | Remote jumps: z interpolated between heartbeats (flat, or floor-projected away) | the packet's z speed and fall time on gravity | `displacementAt` falling branch; no floor projection in the air except as a floor |
| 16 | `MSG_MOVE_*` on a transport read the second transport time under flags2 `0x200` | `0x400` | `readMovementBlockWotlk` |
| 17 | Remote speeds ignored (`MSG_MOVE_SET_*_SPEED`, `SMSG_SPLINE_SET_*_SPEED` for anyone but the local player) | they drive the unit's motion and pace | `applyRemoteSpeed`, `Entity::moveSpeeds()` |
| 18 | Units entering view mid-spline or mid-run stood still until their next packet | continue from the create block | `applyCreateMovement` |
| 19 | `SMSG_MONSTER_MOVE_TRANSPORT` read the guids as raw 8-byte values (WotLK sends them packed, with a seat byte) and set a world-space destination that the deck attachment then overwrote every frame (deck NPCs teleported to their destination) | packed guids; the spline runs in the transport frame | `handleStockMonsterMoveTransport`; the attachment follows the motion's frame position |
| 20 | Body turn: creatures 2*pi rad/s; players an exponential ease with a snap past 150 degrees | 8 * turn rate, at most a quarter turn behind | `stepBodyFacing` |
| 21 | A remote packet's flags and jump reached the animation when it arrived | when it is applied (0x6ED990 then 0x73ED10, or later from the queue) | `MOTION_REMOTE_APPLIED` / `MOTION_REMOTE_JUMPED` -> `processStockMotionFrame` |
| 22 | A jump straight up (or any z change) of a remote unit counted as walking (position-change locomotion) | flags decide | the sync loops ask the stock motion |

Kept for Classic/TBC/Turtle: the old interpolation (their flag values and
packet layouts differ); the stock model is gated on `wotlk`.

## 7. Not done / open

- The banking of a Flying spline (roll from the turn 1 s ahead) and the
  spine/head twist that takes the rest of a body lag are not drawn.
- The tier transitions' own animations (ToFly 458, ToGround 460, FlyToGround
  463 timing) are not played; the tier only switches the flying/hover state.
- Collision: the stock client runs remote dead reckoning through world
  collision (0x762E00). Wowee approximates it with the existing floor
  projection (ground) and a floor clamp (in the air); walls do not stop a
  remote runner between packets.
- The phase carry between moving animations (0x7385C0) is not reproduced.
- Interpolated movement flags2 (0x400/0x800/0x1000, 0x6EA7E0) are read but
  not acted on (vehicles and some scripted movers set them).
- The local player's jump writes z speed +7.96 (Wowee's
  `movement_handler.cpp`); stock writes -7.955 (negative is up), so a stock
  client - and this model - would draw a Wowee player's jump going down. That
  is the local-movement side's to change.
