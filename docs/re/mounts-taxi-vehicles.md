# Mounts, flight paths and vehicles (stock 3.3.5a, build 12340)

How `Wow.exe` 12340 seats a rider, animates and sounds a mount, flies a taxi
and seats vehicle passengers, and where Wowee differed. Read with headless
Ghidra; every rule names its function. Related: `docs/re/characters.md`
section 7 (mount model, scale, rider seat), `docs/re/unit-motion.md` (the
spline every unit runs, the walk/run pick), `docs/re/movement-camera.md`
(jumping, flight physics, the camera).

The code: `include/rendering/animation/mount_rules.hpp` and `mount_fsm.*`
(mount animation), `include/game/taxi_rules.hpp` / `src/game/taxi_rules.cpp`
(route and errors), `include/game/vehicle_rules.hpp`, `vehicle_data.*` and
`src/game/game_handler_vehicles.cpp` (seats, the vehicle bar, aim, seat
requests, trajectory casts), `MovementHandler::setActiveMover` (driving),
`syncVehiclePassengers` / `applyVehicleCamera` in `src/core/application.cpp`
(seats, arcs, camera, fade), the vehicle Lua in `src/addons/lua_*_api.cpp`.
Tests:
`tests/test_mount_rules.cpp`, `test_taxi_rules.cpp`, `test_vehicle_rules.cpp`.

## 1. Mounting (0x73D5D0)

`CGUnit_C` offsets used here: `+0xB4` the unit's own (rider) model, `+0x98C`
the mount model, `+0x990` the mount display's CreatureModelScale, `+0x978`
the creature-sound record in use, `+0xB7C` the rider animation, `+0xB80` the
animation tier, `+0xA38` state flags (`0x10` = mounted).

- `UNIT_FIELD_MOUNTDISPLAYID` -> 0x73D5D0: the mount display's model is
  created (0x81F8F0), skinned (0x4E7790, 0x4F20C0: CDI skins 11-13) and made
  the unit's root model (0x717910); `+0x990` = CDI scale.
- The rider (`+0xB4`) hangs off the mount's **attachment 0** at
  `1 / mountScale` (0x4E26F0): its own size, the seat bone's whole transform.
- The rider plays `+0xB7C` on its whole body (0x735820 with slot -1):
  **Mount (91)**, or - 0x724820 - the AnimID of the StateKit of the spell
  visual of the unit's SPELL_AURA_MOUNTED (78) aura when that is >= 0. In the
  3.3.5a data: the flying carpets 120, Mechano-hog / Mekgineer's Chopper 484
  (ReclinedMount), Summon Mouth Tentacle 127; the other 389 mount auras none.
- `+0x978` = 0x7195D0: the **mount display's** CreatureSoundData
  (CDI.SoundID, else CMD's). 0x71A3F0 prefers it, so while mounted every
  creature sound of the unit - jump start (kind 11), jump end (12), wing
  flap (7), wing glide (10) - is the mount's.
- Flags: `+0xA38 |= 0x10`; tint and alpha go to the mount (characters.md).
- If the mount model's CMD flags have 0x10 and the caller asks, 0x6D7720
  checks head room (MountHeight) and dismounts (0x7412E0) if there is none.
- Then the animation is re-picked (0x73AC30).
- There is **no mount or dismount animation** of either model; the summon
  swirl is the mount spell's own cast kit.

## 2. What a mounted unit plays (0x724500 -> 0x7385C0)

The unit's animation is chosen exactly as on foot, by 0x724500's chain
(0x724060 death, 0x71DFF0 stand state, **0x724200 falling** -> Fall (40)
unless a jump animation is playing, **0x717050 moving**, 0x724330, 0x71E0D0
combat, 0x71E180 shuffles, 0x714F90, 0x71E1F0, 0x7171C0, **0x71E340 idle**).
0x7385C0 then plays it: mounted (`+0x98C` set), a lower-body/locomotion
animation (0x71D6B0: BehaviorIDs 0,1,3-6,8-13,37-45,92-95,119,120,127,131,
132,135,143,187,193) is resolved **against the mount model** (0x7176F0) and
played on the mount (0x735820); the rider keeps `+0xB7C`; actions (casts,
swings, 0x71D800) go to the rider.

**0x717050** (any of FORWARD/BACKWARD/STRAFE, `flags & 0xF`):

| Case | Animation |
|---|---|
| SWIMMING or FLYING (`0x2200000`) | strafe: SwimLeft 43 (`flags & 4`) / SwimRight 44; back: SwimBackwards 45; else **Swim 42** |
| on a **Flying spline** (0x4F5290: spline flags `0x2000` without `0x400`) - a taxi | **Fly 135** |
| BACKWARD | WalkBackwards 13 |
| stealthed (`UNIT_FIELD_BYTES_1` byte 2 & 2) | StealthWalk 119 |
| speed (0x987570) **> 11** (0xA34B20) | **Sprint 143** |
| speed > 2 x walk speed (`+0x818`) | Run 5 |
| otherwise | Walk 4 |

**0x71E340** (idle): SWIMMING or FLYING -> **SwimIdle 41**; stealthed ->
StealthStand 120; hovering (0x71C6C0: a Flying spline, flags2 & 4,
DISABLE_GRAVITY 0x400 or HOVER 0x40000000) -> Hover 193; else Stand 0.

So **a flying mount flies on its swim set**: forward Swim 42, hovering
SwimIdle 41, backing SwimBackwards 45, strafing SwimLeft/Right. Ascending or
descending alone sets no direction flag and is the idle (SwimIdle). The
flying mounts are made that way: `gryphon_mount.m2` carries 41 (two
variations), 42, 43, 44, 45, 135, 193, 143, 187; `drakemount.m2` 41-45. A
60% ground mount (run 11.2) and a 100% one (14) both **sprint**; a model
without Sprint (`ridinghorse.m2`) falls back to Run through AnimationData.

**Tiers.** 0x7176F0 resolves an animation by the unit's tier (`+0xB80`:
0 ground, 2 hover, 3 fly) through AnimationData's BehaviorID/BehaviorTier
(0x717540: the row with BehaviorID = anim and BehaviorTier = tier), then the
Fallback chain, then the tier's Stand (0x717600), then the next tier (table
0xA349B4: 2 -> 3, 3 -> 0). Only tiers 0 and 3 exist in the data (the Fly*
rows 229-505). A player's tier stays 0 on a flying mount - the tier changes
only with a spline's Animation flag (0x73AF00: ToFly 458, ToGround 460).

**Jump** (0x73ED10, event 0xBB): the jump-start sound (kind 11 - the
mount's when mounted) and JumpStart 37; while airborne the jump animation
holds, else Fall 40 (0x724200); landing (event 0x346) plays the land sound
(kind 12) and JumpLandRun 187; with FLYING set at that moment (a flying
mount taking off) JumpStart again. **A standing jump is a jump**: there is
no rear-up; MountSpecial 94 plays only for `SMSG_MOUNTSPECIAL_ANIM`
(/mountspecial).

**Rate** (0x7385C0): a moving animation with a MoveSpeed plays at
`speed / |MoveSpeed|` - a horse's Run (MoveSpeed 9.03) at 14 yd/s plays at
1.55.

**Sounds** come from the mount model's own events with the mount display's
CreatureSoundData: `$FSD` footfalls through FootstepTerrainLookup (0x4CF170),
`$WNG` wing flaps (kind 7), `$WGG` wing glide (kind 10, `gryphon_mount.m2`
has it), plus kinds 11/12 on jump and landing (100%, table 0xADBA74). There
is no timer-driven mount sound.

## 3. Flight paths

### The flight is the server's spline

- AzerothCore's FlightPathMovementGenerator::DoReset sends the **player**
  an `SMSG_MONSTER_MOVE`: the player's position then the TaxiPathNode points
  of the route (shortened: nodes within 40 yd of the previous dropped), spline
  flag Flying (0x2000: twelve-byte nodes, **Catmull-Rom**), at TaxiFlightSpeed
  (32; the lab does not change it); per map, a teleport and a new spline. It
  sets UNIT_FLAG_TAXI_FLIGHT (**0x100000**) and DISABLE_MOVE, and mounts the
  player (UNIT_FIELD_MOUNTDISPLAYID). Order: SMSG_ACTIVATETAXIREPLY(0), the
  spline at once, the fields later in the tick.
- The client has **no active-player branch** in 0x73F590 -> 0x73C8E0 ->
  0x6EB680, and 0x98CA00 runs it each frame as any unit's (unit-motion.md
  section 1). 32 yd/s is under the smooth-spline cap (50): the server's
  duration stands. **TaxiPathNode is never flown by the client.**
- While a spline (without No_Spline) runs, 0x5FA060 makes can-move and
  can-turn false: the keys do nothing (0x5FBBC0 also stops autorun/follow).
  The flag change itself (0x728F70) stops autofollow; clearing it rejoins
  the chat channels held back.
- **Attitude** (0x98CA00, a Flying spline on a unit without SWIMMING/FLYING
  - the taxi player): the facing is the path tangent; the model takes the
  tangent's pitch and banks by **twice the signed angle** from the tangent to
  the point one second ahead (`t = (passed + 1000) / duration`), clamped to
  +-pi/2 (0x9F1FF4 / 0x9E8D88), into CMovement +0x38 (0x5FED20). A unit with
  SWIMMING/FLYING pitches (`asin dir.z`) and does not bank.
- **Animation**: the spline has no Animation flag (tier 0): the mount moves
  in **Fly 135** (0x717050 via 0x4F5290), resolved through the fallback (135
  -> 42 -> 4 -> 0) on models without it; the rider sits in Mount.
- **Camera**: smart follow (0x5FFD20 / 0x601B70) is off on a taxi, so the
  camera keeps its yaw and pitch; the player may orbit and zoom (mouse look
  is blocked only with the taxi-benchmark player flag 0x20000).
- **End**: the spline ends on its last point (0x6EB0B0, snapped only from 3
  yd), MSG_MOVE_STOP's re-pick: Stand. The server then dismounts and clears
  the flags. No landing animation.

### The map and the route (Lua table 0xACF258)

- `SMSG_SHOWTAXINODES` (0x6D6050): u32 has-node, npc guid, current node, the
  mask. **Fewer than two known nodes: ERR_TAXINOPATHS** and the gossip
  closes. Otherwise the map opens (0x5932E0: known nodes of this continent,
  `TAXIMAP%d.blp`), TAXIMAP_OPENED and sound 0x22, and **a mounted player is
  dismounted** (CMSG_CANCEL_MOUNT_AURA, 0x7412E0), a disallowed shapeshift
  cancelled (0x726CE0).
- **Route** (0x592BB0 / 0x5926C0): over known nodes, the **shortest total
  flight distance** - the 3D lengths of each TaxiPath's TaxiPathNode points
  (0x592580) - not the fewest hops. Prices are kept in five reputation tiers
  (1.0, 0.95, 0.90, 0.85, 0.80 x price; TaxiNodeCost 0x591350).
- **TakeTaxiNode** (0x5914B0): mounted -> ERR_TAXIPLAYERALREADYMOUNTED;
  same node -> ERR_TAXISAMENODE; a direct TaxiPath -> **CMSG_ACTIVATETAXI**
  (npc, from, to; 0x6D63A0); else money check (ERR_TAXINOTENOUGHMONEY) and
  **CMSG_ACTIVATETAXIEXPRESS** (npc, count, the nodes; 0x6D6430); no route
  -> ERR_TAXINOSUCHPATH.
- **SMSG_ACTIVATETAXIREPLY** (0x6CEE90): 0 closes the map (TAXIMAP_CLOSED);
  1-12 raise ERR_TAXIUNSPECIFIEDSERVERERROR, NOSUCHPATH, NOTENOUGHMONEY,
  TOOFARAWAY, NOVENDORNEARBY, NOTVISITED, PLAYERBUSY, PLAYERALREADYMOUNTED,
  PLAYERSHAPESHIFTED, PLAYERMOVING, SAMENODE, NOTSTANDING (table 0xAD9C94).
- `SMSG_NEW_TAXI_PATH`: ERR_NEWTAXIPATH. No "discovered" line from masks.
- Lua: SetTaxiMap 0x590D10, NumTaxiNodes 0x590710, TaxiNodeName 0x590D60,
  TaxiNodePosition 0x590E00, CloseTaxiMap 0x590EC0, TaxiNodeGetType
  0x590ED0, TaxiNodeSetCurrent 0x591E60, TaxiGetSrcX/Y 0x590F40, TaxiGetDestX/Y
  0x591080, GetNumRoutes 0x5911C0, TaxiNodeCost 0x5915E0, TakeTaxiNode
  0x591680, UnitOnTaxi 0x60F350 (UNIT_FIELD_FLAGS & 0x100000).
  TaxiRequestEarlyLanding does not exist in 12340.

## 4. Vehicles and passengers

Unit `+0xF5C` = Vehicle_C (the unit is a vehicle), `+0xF60` =
VehiclePassenger_C (the unit rides one), the seat byte at `+0x7D2`
(CMovement +0x4A). Vehicle.dbc (40 fields) and VehicleSeat.dbc (58) in the
standard 3.3.5 column order; seat i is `VehicleSeat[veh + 0x18 + 4i]`
(0x756EC0).

- **A unit becomes a vehicle** by the create block's UPDATEFLAG_VEHICLE
  (0x80: vehicle id, aim; 0x73C260 -> 0x74C750) or **opcode 0x4A7**
  (Wowee's SMSG_PLAYER_VEHICLE_DATA; 0x716DB0): packed guid + id **for that
  unit**, 0 removes it. AzerothCore sends it to everyone in range when a
  player mounts a multi-seat mount. It does **not** mean "the player is in a
  vehicle".
- **A unit is a passenger** when its movement is ONTRANSPORT with a
  transport guid that is a unit (0x74B8B0: high type 0xF150 or a player),
  with its seat byte. States (passenger +0x14): 1 enter delay, 2 entering
  arc, **3 seated**, 4 exit delay, 5 exiting arc (0x7489C0, 0x74AF70; arcs by
  EnterSpeed/Gravity/arc heights, easing 0x747D70).
- **Seat transform** (0x7493B0 -> 0x7490F0): the seat's AttachmentID is
  **not** an M2 id; table 0xA2D3F0 maps 0-21 to {20, 34, 19, 21, 22, 17, 23,
  24, 25, 15, 16, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 0} (22+: none).
  With an attachment: its transform, the seat AttachmentOffset in its space,
  the passenger at its own size, the passenger's own PassengerAttachmentID
  point brought onto it. Without: relative to the vehicle base, the offset
  times the vehicle scale. Rotation -PassengerYaw, roll, pitch.
- **Passenger animation** (0x747B20): seated with seat flag 0x2,
  RideAnimStart once then **RideAnimLoop**; entering with 0x1 the enter
  pair; exiting the exit pair; upper body (flag 0x4) RideUpperAnimStart /
  Loop (0x747BD0). Otherwise none: the ordinary stand chain, **no fallback
  to Mount (91)**.
- **The vehicle's own** (0x7489C0): seat flag 0x20000 and
  VehicleRideAnimLoop < 506 play on the vehicle (bone slot
  VehicleRideAnimLoopBone, 0x756F80 -> 0x735820).
- **Camera** (VehicleCamera_C 0x758480..): the target is the seat point with
  flagsB 0x100, else the vehicle; plus CameraOffset; spring chases by
  CameraPos/FacingChaseRate; pitch limits Vehicle.PitchMin/Max.
- **Lua** (0xAD19F0 / 0xAD2658): UnitInVehicle = seated; UnitUsingVehicle =
  any state; UnitControllingVehicle = seated and UNIT_FIELD_CHARM is the
  vehicle; UnitInVehicleControlSeat = seat flag 0x800; UnitHasVehicleUI =
  0x20000000; CanExitVehicle = 0x2000000; CanSwitchVehicleSeats = 0x4000000;
  IsUsingVehicleControls = 0x800000; UnitVehicleSkin = uiSkin ("Natural",
  "Mechanical"); UnitVehicleSeatInfo canEject = flagsB 0x20; aim functions
  read Vehicle.flags 0x400 / 0x800; VehicleExit = CMSG 0x476; seat changes
  0x477-0x479. The action bar comes from SMSG_PET_SPELLS with the vehicle
  guid (AzerothCore Player::VehicleSpellInitialize).

| Vehicle | Used by | Seats |
|---|---|---|
| 349, 486, 369 | Argent Tournament warhorses / battleworgs | seat 0: attachment 21 (M2 0), ride 476 ReadyJoust |
| 529, 636 | other Argent mounts | seat 0: attachment 21, ride 91 |
| 79 | Wintergrasp Catapult | seat 0: attachment 13 (M2 39), ride 91, Mechanical |
| 106 | Wintergrasp Demolisher | seat 0 att 13 ride 91; seats 2/3 att 15/16, no ride anim |
| 117 / 324 | Wintergrasp Siege Engine | seat 0 att 13 offset (0.2,0,0) ride 91; seats 1/2 att 14/15; seat 7 att 20 (turret) ride 102 |
| 116 | Siege Turret | no attachment, ride 102, passenger hidden (flag 0x200) |
| 312 / 313 | Traveler's / Grand Caravan Mammoth | seat 0 empty (the driver rides as a mount); seats 1/2 att 14/13 ride 91 |
| 318 | Mechano-hog / Chopper | seat 1 att 14 ride 500 (ReclinedMountPassenger) |

### Driving: the active mover (0x729010, 0x72CCA0)

The client moves one unit, the **active mover** (`0xCA1238`). The input
control (0x5F95D0 / 0x5FBBC0) steers that unit's CMovement, at that unit's
own speeds and turn rate.

- **SMSG_CLIENT_CONTROL_UPDATE** (0x159, handler 0x72D0B0 -> 0x72CCA0):
  packed guid + u8 allow. 0x71C930 sets the unit's "client controls me" bit
  (`+0xA30 & 0x400`). If the unit is the one the player should control, an
  allow makes it the mover; a revoke of the mover makes the other controlled
  unit the mover, or nobody. AzerothCore sends `(vehicle, 1)` when the
  player takes a control seat (Unit::SetCharmedBy, CHARM_TYPE_VEHICLE) and
  `(vehicle, 0)` then `(player, 1)` when it leaves (RemoveCharmedBy). There
  is no SMSG_CONTROL_VEHICLE in 3.3.5a.
- **The switch** (0x729010): the old mover stops (0x6EE920 clears its
  direction keys); if the old mover was the player and has no spline running,
  `CMSG_MOVE_NOT_ACTIVE_MOVER` (0x2D1) with its movement; then
  **CMSG_SET_ACTIVE_MOVER** (0x26A, plain u64 guid); then 0x747F40.
- **0x747F40**: `VEHICLE_UPDATE`; `VEHICLE_ANGLE_SHOW` (1 for Vehicle.flags
  0x400), `VEHICLE_POWER_SHOW` (1 for 0x800) (0x74C5A0);
  `VEHICLE_ANGLE_UPDATE` (normalized angle, angle) (0x5FA910).
- Every movement packet then carries the **mover's guid** and movement; the
  forced changes (0x7307A0: root 0xE8/0xEA, speeds 0xE2.., knockback 0xEF,
  teleport ack 0xC7 -> 0x72D2D0) are applied to and acknowledged for the
  mover. The vehicle runs at its own speeds (its create block, then the
  SMSG_FORCE_*_SPEED_CHANGE the server sends the controller with the
  vehicle's guid). Vehicle.dbc TurnSpeed / PitchSpeed are not read by the
  movement code; the unit's turn and pitch rates are.
- The vehicle's MOVEMENTFLAG2 (AzerothCore Vehicle::InitMovementInfoForBase
  from Vehicle.flags): NO_STRAFE 0x1, NO_JUMPING 0x2, FULL_SPEED_TURNING 0x8,
  ALWAYS_ALLOW_PITCHING 0x20 (pitch written in every movement block, not only
  swimming/flying), FULL_SPEED_PITCHING 0x10.
- **FIXED_POSITION** (Vehicle.flags 0x200000, 0x6EAA50): the facing is held
  to the create-block aim -FacingLimitRight .. +FacingLimitLeft.

### Aim (InputControl 0x5FB3A0 ...)

The aim angle **is the mover's pitch** (vtable +0x14C).

- Limits (0x5FB3A0): with CUSTOM_PITCH (0x40) Vehicle.PitchMin..PitchMax,
  else -pi/2..pi/2 (0x9F1FF4 / 0x9E8D88).
- Set (0x5FB3A0): without FULL_SPEED_PITCHING (flags2 0x10) at once,
  `MSG_MOVE_SET_PITCH` (0x72EAD0 -> 0x6EE460 -> 0x6E9380, opcode 0xDB); with
  it, pitching up or down at the pitch rate (`MSG_MOVE_START_PITCH_UP/DOWN`)
  and a scheduled `MSG_MOVE_STOP_PITCH` (0x72D470 -> 0x6EF490 / 0x6EF7A0).
  Vehicle.flags 0x400 then fires `VEHICLE_ANGLE_UPDATE` (norm, angle).
- Lua: VehicleAimIncrement/Decrement(delta = 0.1) (0x5FB770), RequestAngle
  (0x5FB820, only with 0x400), RequestNormAngle (0x5FB8C0: min + (max-min) x
  n), GetAngle (0x5F9E10), GetNormAngle (0x5F9E60: (pitch-min)/(max-min), 0
  when equal), Up/DownStart/Stop (input bits 0x400 / 0x800: pitching while
  held), SetNormPower / GetNormPower (a global 0..1, 0xC24958). Mouselook in
  a vehicle sets the aim to -camera pitch + Vehicle.MouseLookOffsetPitch
  (0x5FA9B0).

### The vehicle's spells (0x5D4AD0, 0x5A83C0, 0x5D4210)

- SMSG_PET_SPELLS with the vehicle's guid (AzerothCore
  Player::VehicleSpellInitialize: slot i = spell | (i + 8) << 24) is the
  "pet" bar. When the unit the player controls is that pet and it has a bar,
  and - for a vehicle - the player's seat has **VehicleSeat.
  VehicleAbilityDisplay (field 41) == 1**, the bonus bar flag is set:
  **GetBonusBarOffset = 5** (0x5A83C0) and action slots **121-130 are pet
  slots 0-9** (0x5AB800 with 0x5D3240 = `i | 0x10000000`), 131-132 empty.
  PetHasActionBar is false meanwhile (0x5D3720).
- GetActionInfo on such a slot (0x5A8F10): "spell", id, "pet", id.
- UseAction (0x5ABBC0 -> 0x5D4210): a spell type (1, 8-17) is cast **from
  the vehicle**: CMSG_PET_CAST_SPELL (0x1F0: u64 caster, u8 castCount, u32
  spell, u8 castFlags, targets) - the caster is not the player (0x80AC90);
  commands and stances (6, 7) go as CMSG_PET_ACTION.
- **Trajectory spells** (Spell.dbc field 227 SpellMissileID, SpellRec
  +0x28C; SpellMissile.dbc flag 0x1): only for the active mover ("Missile
  trajectory does not have active mover caster"). 0x809F80 -> 0x6FCD60:
  elevation = the vehicle's pitch with ALLOW_PITCHING (0x10, held to the
  limits with 0x40), else the middle of DefaultPitch; speed = DefaultSpeedMin
  + (Max - Min) x power with ADJUST_AIM_POWER (0x800), else the middle; plus
  RandomizeFacing/Pitch/Speed and a jitter of 0.01; then the flight in 0.1 s
  steps (velocity x 0.1, Gravity x 0.01 off z a step, half on the first), at
  most 200 steps, each segment tested against the world (0x77F310, flags
  0x100111). The cast carries **targets 0x60** (source + destination
  location: the launch point and where the flight met the world),
  **castFlags 0x2**, then elevation, speed, `1`, u32 MSG_MOVE_STOP (0xB7) and
  the mover's packed guid and movement (0x71F060). AzerothCore reads these in
  HandleClientCastFlags and aims TARGET_DEST_TRAJ (89) with them.
  Hurl Boulder (Wintergrasp Demolisher 50896, Salvaged Demolisher 62306):
  missile 526 / 1682, pitch 0.52..1.57, speed 65, gravity 30; Plague Barrel
  (catapult) 1022: speed 45; Fire Cannon (turret) 1023: gravity 40.

### Seats: exit and switch (0x74C7F0, 0x74C8B0, 0x74C9A0, 0x74CA90)

- **VehicleExit** (0x5FB660): the seat must have 0x2000000 (else
  SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW). The driver - the player's vehicle is
  the mover - sends **CMSG_DISMISS_CONTROLLED_VEHICLE** (0x46D: the vehicle's
  packed guid and movement; 0x5D46F0 -> 0x74BB20, event 0x34); anyone else
  CMSG_REQUEST_VEHICLE_EXIT (0x476, empty).
- **Prev / Next seat** (seat flag 0x4000000): the driver sends
  **CMSG_CHANGE_SEATS_ON_CONTROLLED_VEHICLE** (0x49B: vehicle packed guid,
  movement, packed accessory guid 0, i8 -1 / 1; event 0x37); a passenger
  CMSG_REQUEST_VEHICLE_PREV_SEAT / NEXT_SEAT (0x477 / 0x478, empty).
- **UnitSwitchToVehicleSeat("player", i)** (0x6139B0 -> 0x74CA90): the
  virtual seat i of the root vehicle (0x757550: seats in order, a seat
  holding an accessory vehicle - a unit that is no player - replaced by that
  vehicle's seats); free, with 0x4000000, the player's seat with 0x4000000.
  The driver: 0x49B with the accessory guid and the seat; a passenger:
  CMSG_REQUEST_VEHICLE_SWITCH_SEAT (0x479: packed vehicle guid, u8 seat).
- UnitVehicleSeatCount (0x613830 -> 0x757470), UnitVehicleSeatInfo (0x7579E0):
  controlType "Root" (seat 0x800 on the root vehicle), "Child" (0x800 on an
  accessory), "None"; occupant name and realm; canEject flagsB 0x20;
  canSwitchSeats flags 0x4000000. GetVehicleUIIndicator(id) (0x614E60):
  VehicleUIIndicator background, seat count; GetVehicleUIIndicatorSeat
  (0x614EF0): VehicleUIIndSeat virtual seat index, x, y (file order).
- **Events** (0x748810): UNIT_ENTERING / UNIT_ENTERED_VEHICLE = (unit, has
  vehicle UI (seat 0x20000000), skin, EnterUISound name, control seat (0x800),
  Vehicle.VehicleUIIndicatorID) - VehicleSeatIndicator reads the sixth;
  UNIT_EXITED_VEHICLE = (unit, ExitUISound name); PLAYER_GAINS_VEHICLE_DATA
  (unit, indicator) when the player becomes a vehicle (0x74BBD0).

### Boarding and leaving (VehiclePassenger_C 0x74A200, 0x74A7F0, 0x747D70)

States 1 (enter pre-delay: EnterPreDelay, <= 10 s), 2 (the arc), 3 (seated),
4 (ExitPreDelay), 5 (the arc out). The arc runs from where the passenger was
(+0x84) to the seat point (0x749E40 -> 0x7493B0), or from the seat to the
passenger's own position:

- T = distance / (EnterSpeed - the vehicle's speed toward it), or distance /
  ExitSpeed, clamped to Min..MaxDuration (MaxDuration capped at 10, 1.5 when
  unset).
- Progress p = t / T eased by the seat flags: entering 0x20 = p^2, 0x40 =
  1-(1-p)^2, both = 0.5 - 0.5 cos(pi p); leaving 0x80 / 0x100.
- Position lerp(start, end, p) raised by 0.5 g t (T - t) x k, t = p T,
  g = Enter/ExitGravity (0.1 when zero and an arc height is asked), k so the
  top g T^2 / 8 sits in Min..MaxArcHeight.
- Animations: the enter pair during 1-2 with seat flag 0x1, the exit pair
  during 4-5 (0x747B20).

### The vehicle camera (VehicleCamera_C 0x759200, 0x759D80, 0x759580)

The camera orbits the vehicle - or, with flagsB 0x100, the passenger's seat
point (0x749D80) - moved by the seat's **CameraOffset built as (Y, -X, Z)
and turned by the vehicle's facing**. CameraPosChaseRate > 0 chases the
target with a spring of stiffness 20 / rate (0x6FF4C0); CameraFacingChaseRate
the facing. The target height above a non-player unit is min(its collision
height, 2.0278) - 0.1667 (0x718080). Only 21 of the 720 seats have an
offset, 19 a chase rate; none of the Argent, Wintergrasp, Ulduar, mammoth or
Skytalon seats do.

The camera does not keep out of a big vehicle; the **vehicle fades** instead
(Camera.cpp, the alpha pass around 0x6077A0): for the followed unit's vehicle
with Vehicle.flags **0x80000**, alpha 255 beyond CameraFadeDistScalarMax x
the Stand sequence's bounds radius (0x82CED0, M2Sequence +0x38, unscaled) between
the camera and the point it looks at (0x5FECF0; the zoom), 0 within CameraFadeDistScalarMin x it,
eased (0x8CA080) between (FUN_004F8660 sets it). The siege engines,
demolishers and Argent mounts are 1.0 / 1.5.

### A creature's MSG_MOVE_TELEPORT (0xC5)

Registered with the unit movement handler 0x741B60 (with 0xC3 and the other
MSG_MOVE_* relays) -> 0x740D30 case 0xC5 -> 0x7189A0: a **packed guid and a
movement block** for that unit, which is placed there. MSG_MOVE_TELEPORT_ACK
(0xC7) is the mover's (0x732450 -> 0x7307A0 -> 0x72D2D0: placed, then
`0xC7` back with the guid, counter and time). AzerothCore sends 0xC5 to the
units' viewers (Unit::SendTeleportPacket, self excluded) - Dalaran's Gold
Mages (32341, guid 0xF130007E55...) on every blink.

## 5. Wowee against this

| # | Area | Stock | Wowee before | Now |
|---|---|---|---|---|
| M1 | Mount movement anims | the unit's: Walk/Run/Sprint by pace, WalkBackwards, jump/fall, shuffles; swim set in water **and in the air**; Fly on a taxi spline | Run for everything moving on the ground (no Walk, no Sprint, no backpedal), Fly/Hover ids in the air, a scored "jump discovery" guessing sequences by duration | **Fixed**: MountFSM is LocomotionFSM on the mount's own capabilities, flying = swim set, taxi = Fly (local and remote riders) |
| M2 | Procedural motion | none | bob, lean into turns, rider tilt 35% | **Fixed**: removed |
| M3 | Fidgets / idle sounds / rear-up | Stand variations only; no rear-up on a jump | 20-45 s fidget timer, idle and rear-up sounds, rear-up when jumping standing | **Fixed**: removed; MountSpecial on SMSG_MOUNTSPECIAL_ANIM (was ignored) |
| M4 | Playback rate | speed / MoveSpeed | rate 1 | **Fixed** on the ground for the local mount; remote mounts through driveUnitLocomotion |
| M5 | Local rider seat | attachment 0, full transform, own size | seat attachment probe {0,5,6,7,8}, +0.08 offset, smoothing, 35% tilt | **Fixed**: the remote riders' rule (setInstanceMountedOn) |
| M6 | Rider pose from the mount aura kit | carpets 120, choppers 484 | always 91 | **Fixed** (characters.md S4) |
| M7 | Mount sounds | $FSD/$WNG of the mount model with the mount display's CreatureSoundData; jump/land kinds 11/12 | hand-made family banks, synthetic gait beats, timed wing loops; mount events silent | **Fixed**: mount events sound by the mount display; the hand banks are no longer started; jump/land from the mount's CSD; $WGG not yet (CreatureSounds has no glide column) |
| M9 | Rider skeleton | the rider animates (Mount, casts) like any unit | CharacterRenderer skipped animating every instance with an override matrix - meant for weapons - so a seated rider (local or remote) kept the pose it had when it mounted, standing on the saddle | **Fixed**: seated instances animate |
| M8 | Mount .anim files | all | Stand/Walk/Run only (the rest would not animate if external) | **Fixed**: the locomotion set |
| T1 | The flight | the server's Catmull-Rom spline, run by the unit code | TaxiPathNode flown client-side at 32 with its own pacing, server spline cancelled every frame, snaps and deferred dismounts | **Fixed**: the player's entity runs the server spline; movementInfo follows it |
| T2 | Attitude | tangent pitch, bank 2x the turn 1 s ahead | eased facing, roll -2.5 x facing error | **Fixed** (`flyingSplineBank`) |
| T3 | Taxi mount | the mount field | guessed display from TaxiNodes and fallbacks, applied on the reply | **Fixed**: the field (reconciled on a taxi too) |
| T4 | Route | shortest flight distance | fewest hops (BFS) | **Fixed** (`taxi::shortestRoute`) |
| T5 | Errors and map | specific ERR_TAXI*; <2 nodes ERR_TAXINOPATHS; dismount on map open | "Cannot take that flight path."; "Requesting flight" and "Discovered flight path" chat lines | **Fixed** |
| T6 | Prices | reputation tiers | plain sums | open (tier 1.0 used) |
| V1 | 0x4A7 | that unit is a vehicle | "the player is in a vehicle" (a nearby mammoth put the player "in a vehicle") | **Fixed** |
| V2 | Vehicle id, seat byte | read | dropped | **Fixed** (UpdateBlock::vehicleId / transportSeat) |
| V3 | Passengers | seated by VehicleSeat | left where they were, run animations | **Fixed**: seat attachment (remapped), offset, own size, yaw, passenger point; ride anims; vehicle ride loop; flag 0x200 hides |
| V4 | Player in a vehicle | seated; visible | hidden; "in vehicle" from 0x4A7 | **Fixed**: seated and visible; the camera follows the vehicle |
| V5 | Lua | seat flags | stubs | **Fixed**: UnitHasVehicleUI (0x20000000), UnitInVehicleControlSeat, UnitControllingVehicle (UNIT_FIELD_CHARM is the vehicle), UnitVehicleSeatCount / SeatInfo / SwitchToVehicleSeat over virtual seats (accessories expanded), CanSwitchVehicleSeat(s), IsUsingVehicleControls, CanExitVehicle, IsVehicleAim*Adjustable (1 / nil, as stock - VehicleMenuBar_SetSkin adds 1 to it), the VehicleAim* family, GetVehicleUIIndicator(Seat), Can/EjectPassengerFromSeat, UnitTargetsVehicleInRaidUI, the "vehicle" unit token; the events with the stock arguments (UNIT_ENTERING/ENTERED_VEHICLE's six, VEHICLE_ANGLE_SHOW/UPDATE, VEHICLE_POWER_SHOW, PLAYER_GAINS/LOSES_VEHICLE_DATA) |
| V6 | Driving | the client moves the vehicle it controls | not done | **Fixed**: SMSG_CLIENT_CONTROL_UPDATE makes the vehicle the active mover (CMSG_MOVE_NOT_ACTIVE_MOVER, CMSG_SET_ACTIVE_MOVER); movement, force acks (speeds, root, flags, knockback, teleport ack) for the mover's guid; the player's own state kept aside and acknowledged as the player's; the vehicle's speeds, turn rate, flags2 (NO_STRAFE / NO_JUMPING honoured, pitch written with ALWAYS_ALLOW_PITCHING), collision box; the vehicle's entity and locomotion follow the controls |
| V7 | Vehicle bar | pet bar on 121-130, bonus offset 5 (VehicleAbilityDisplay 1) | pet bar only | **Fixed**: GetBonusBarOffset 5, the action functions answer slots 121-132 from the pet bar, UseAction casts CMSG_PET_CAST_SPELL from the vehicle; PetHasActionBar false meanwhile |
| V8 | Trajectory spells | src + dest locations, castFlags 0x2, elevation, speed, movement (0x6FCD60) | none | **Fixed**: SpellMissile.dbc flight in 0.1 s steps against the terrain and building floors; elevation from the aim, speed from the power |
| V9 | Aim | the mover's pitch, limits, SET_PITCH / pitching at the pitch rate | none | **Fixed** |
| V10 | Seats | DISMISS_CONTROLLED_VEHICLE / CHANGE_SEATS_ON_CONTROLLED_VEHICLE for the driver, the 0x476-0x479 requests otherwise | exit request only | **Fixed** |
| V11 | Boarding / leaving | arcs (speed, duration, gravity, arc heights, easing) and the enter/exit animations | snap | **Fixed** for the model (CharacterRenderer seat arc); the local player rides its TransportExit spline after leaving |
| V12 | Vehicle camera | seat point (flagsB 0x100), CameraOffset (Y, -X, Z) turned by facing, chase rate; fade (0x80000) | the vehicle's position | **Fixed** |
| V13 | MSG_MOVE_TELEPORT (0xC5) | another unit's movement block, applied (0x7189A0) | read as our TELEPORT_ACK: a counter out of the movement flags, the creature placed at garbage and logged as "MSG_MOVE_TELEPORT_ACK: guid=0xF130..." | **Fixed**: snapped to the block; only 0xC7 is acknowledged, and for a driven vehicle with its guid. Not a regression from the passenger work - the two opcodes shared a handler since the file was written |
| T7 | Taxi camera | the camera's collision runs on a flight | only the ground under the camera tested; a ridge or rock between rider and camera put the view inside it | **Fixed**: WMO, doodad collision meshes and the terrain march pull the camera in at once |

## 6. Open

- FIXED_POSITION facing limits (Vehicle.flags 0x200000, 0x6EAA50) are not
  applied: a turret turns freely under the controls.
- Mouselook does not drive the aim (0x5FA9B0: -camera pitch +
  Vehicle.MouseLookOffsetPitch); the aim keys, the pitch buttons and the
  slider do.
- A trajectory's launch point is the vehicle's position + 1.5 yd, not the
  spell visual's missile attachment (0x720BF0); the 0.01 random jitter is not
  applied; the flight is tested against the terrain and WMO floors, not
  doodads.
- The camera's entering/exiting blend (VehicleCamera_C states 1-4,
  CameraEntering/ExitingDelay/Duration) and CameraFacingChaseRate are not
  done; the target jumps to the seat. At the default zoom (5.55) a vehicle
  whose Stand radius is above 5.55 / FadeMin is fully faded - a Wyrmrest
  Skytalon (5.77) is invisible until the player zooms out. That follows the
  stock rule as read; it was not compared against the stock client.
- Vehicle UI sounds (EnterUISoundID / ExitUISoundID) are passed in the events,
  not played.
- The exit arc of other units ends at their entity position, which their
  server spline moves meanwhile.
- The roll sign of a taxi's bank is taken as into the turn; the decompile
  (0x98CA00, 0x4C5820) was not followed to the quaternion's axis.
- Price tiers by reputation (T6); the hard-coded faction gate on taxi nodes.
- Mount wing glide `$WGG` (needs CreatureSoundData's glide column in
  audio::CreatureSounds); the local mount's own sounds are 3D at the unit,
  not 2D at 0.65 (sound-engine.md).
- The rider's upper-body split while mounted (casts on the rider) follows
  the existing ActivityFSM; not re-checked against 0x7385C0's mounted branch.
- The camera height on a taxi (0x604E00 uses Fly's bounds) is not applied.
