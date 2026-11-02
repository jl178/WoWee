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
`src/game/game_handler_vehicles.cpp` (seats). Tests:
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
| V5 | Lua | seat flags | stubs | UnitInVehicle/UsingVehicle for any unit, UnitControllingVehicle, UnitVehicleSkin, CanExitVehicle; UnitHasVehicleUI still false (VehicleMenuBar's readers unbound) |
| V6 | Driving | the client moves the vehicle it controls | not done | open |

## 6. Open

- Driving a vehicle (the player as its mover: SMSG_CLIENT_CONTROL_UPDATE,
  CMSG_SET_ACTIVE_MOVER, movement for the vehicle guid), the vehicle action
  bar (VehicleMenuBar: GetVehicleUIIndicator, the aim functions), seat
  switching, the enter/exit arcs and their animations, the vehicle camera
  offsets and chase.
- The roll sign of a taxi's bank is taken as into the turn; the decompile
  (0x98CA00, 0x4C5820) was not followed to the quaternion's axis.
- Price tiers by reputation (T6); the hard-coded faction gate on taxi nodes.
- Mount wing glide `$WGG` (needs CreatureSoundData's glide column in
  audio::CreatureSounds); the local mount's own sounds are 3D at the unit,
  not 2D at 0.65 (sound-engine.md).
- The rider's upper-body split while mounted (casts on the rider) follows
  the existing ActivityFSM; not re-checked against 0x7385C0's mounted branch.
- The camera height on a taxi (0x604E00 uses Fly's bounds) is not applied.
