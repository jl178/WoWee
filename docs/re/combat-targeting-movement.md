# Combat feel: targeting, auto-attack and movement (stock 3.3.5a, build 12340)

What the stock client does when a player selects and attacks something, how it
animates every unit's swings, blows, stances and moves, and where Wowee
differs. Read from `Wow.exe` (12340) with Ghidra; every rule names the function
it comes from. Offsets on a unit are `CGUnit_C` fields; descriptor fields are
named by their update field (`UNIT_FIELD_*`).

Terms used below:

- **attack target**: `CGUnit_C +0xA20` (guid), with `+0xA28` "requested, not
  yet confirmed" and `+0xA2C` "a stop is in flight". Every unit has one, not
  only the player.
- **pending blow**: the last `SMSG_ATTACKERSTATEUPDATE` a unit dealt, kept on
  the attacker at `+0xB98` (0xA0 bytes) until its animation lands it.
- **play** means `CGUnit_C::PlayAnimation` (0x7385C0); **re-pick** means
  `0x73AC30(flags, -1)`, which asks the base-animation chooser (0x724500)
  what the unit should be doing now and plays that.
- Animation ids and their WeaponFlags / Fallback / BehaviorID are
  AnimationData.dbc's. The record's BehaviorID is at +0x18, Fallback +0x14,
  WeaponFlags +0x08.

## 1. Targeting

### Clicks

- **Left click on a unit** (0x5274F0): handles the cursor modes (a spell
  waiting for a target, item use, trade) and then only selects:
  `CGGameUI::Target` (0x524BF0). It never starts an attack.
- **Right click on a unit** (0x5277B0): `Target(guid)`, then the unit's
  `OnRightClick` (vtable +0xB0; `CGUnit_C` 0x731260). That loots a lootable
  corpse, skins, opens gossip/vendor for an NPC (0x729530 -> 0x6DDBB0), and for
  anything the player can attack (`CanAttack` 0x729A70) asks to attack it
  through 0x6E4950 -> `CGPlayer_C::StartAttack` (0x6E2610). With
  `autoRangedCombat` on and a ranged auto-attack known, 0x6E4950 starts the
  ranged attack instead.
- `CGGameUI::Target` (0x524BF0): sends `CMSG_SET_SELECTION` (0x13D, via
  0x518DC0), fires `PLAYER_TARGET_CHANGED` (event 0x9D), plays the selection
  sound/highlight. If the player is auto-attacking and the CVar
  `stopAutoAttackOnTargetChange` (0xBD0924) is 0, it calls 0x6E4950 for the
  new target. Clearing the target (guid 0) goes through 0x5241B0, which sends
  `CMSG_SET_SELECTION` 0 and cancels an auto-repeat spell.

CVar pointers registered at 0x51DBxx: 0xBD0918 `assistAttack` (used by
`AssistUnit`, 0x525EB0: assisting starts an attack on the friend's target),
0xBD091C `autoRangedCombat` (0x6E4950 swaps melee for Auto Shot), 0xBD0920
`autoSelfCast`, 0xBD0924 `stopAutoAttackOnTargetChange`.

### TargetNearest: Tab and its siblings (0x524FC0)

The Lua functions (table at 0xAC82C0) all end in 0x524FC0(reverse, mode):

| Function | Address | Mode |
|---|---|---|
| `TargetNearest` | 0x525A90 | 0 any unit |
| `TargetNearestEnemy` | 0x525AD0 | 1 |
| `TargetNearestEnemyPlayer` | 0x525B10 | 2 |
| `TargetNearestFriend` | 0x525B50 | 3 |
| `TargetNearestFriendPlayer` | 0x525B90 | 4 |
| `TargetNearestPartyMember` | 0x525BD0 | 5 |
| `TargetNearestRaidMember` | 0x525C00 | 6 |

The argument is "reverse" (`lua_toboolean`). Bindings.xml binds
TARGETNEARESTENEMY (Tab) to `TargetNearestEnemy()` and TARGETPREVIOUSENEMY to
`TargetNearestEnemy(1)`, the same for the friend and player pairs; there is
no party or raid binding (TARGETPARTYMEMBERn is `TargetUnit("partyN")`).
None is `runOnUp`: the script runs from the key-down event. Modes 1-4 and
0 (not 5/6) run once per macro: inside a macro (0xBEAF4C, set by the macro
runner 0x564DB0) the second call finds bit 0x20 of 0xBEAF44 cleared by the
first and does nothing. Called from tainted code it is blocked (0xD4139C ->
0x513530, ADDON_ACTION_BLOCKED).

**Candidates** (0x524440, run on every unit of the object manager; the
reference is the player, or the vehicle it drives, 0x6DD060):

1. not the reference, and the mode's test (0x518E40, EAX = mode):
   - 1 enemy: `CanAttack` (0x729740) and not dead (vtable +0x128: health
     below 1, a player also with `PLAYER_FLAGS_GHOST`), not
     `UNIT_DYNFLAG_DEAD` (feigning death too), stand state not 7 (dead);
   - 2: a player, then as 1;
   - 3 friend: `CanAssist` (0x7293D0) and health at least 1;
   - 4: a player, then as 3;
   - 5 party: the player or one of party1-4 (0x52C680, guids at 0xBD1948);
   - 6 raid: party, or a raid member (0x512A00 -> 0x5726F0);
   - 0: anything.
   Dead party and raid members count.
2. Distance squared, 3D, from the reference: **at most 1681 (41 yards) when
   the unit is within 30 degrees (0xA02D20 = pi/6) either side of the
   reference's facing, at most 100 (10 yards) anywhere else** (0x9FE7F8,
   0x9FE7FC). The angle is `fmod(atan2(to unit) - facing + 4pi, 2pi)`
   (0xA02D24 = 4pi, 0x9F1FE8 = 2pi); in the cone when below pi/6 or above
   2pi - pi/6.
3. Its creature type (0x71F300: the shapeshift form's, else the creature's,
   else the race's) is not one CreatureType.dbc flags 1: Critter (8),
   Non-combat Pet (12), Gas Cloud (13). Totems (11) are candidates.
4. **On screen**: its map entity (+0xB8) has +0x25 == 0 (0x77F0B0). The
   entity walk 0x799980 sets +0x25 to 1 each frame and clears it when the
   entity's bounding sphere passes the view frustum test (0x791120 ->
   0x983D20); so a unit is a candidate only if its model was inside the
   camera's frustum on the last frame drawn. Horizon occlusion (0x78FC40)
   does not set it. **There is no line of sight test.**
5. A player who is a commentator (`PLAYER_FLAGS` 0x80000 with 0x400000 or
   in an arena map, 0x6DE980) is not.

Each candidate is {guid, distance squared, in the cone}.

**Order** (`qsort` with 0x5131D0): those in the cone first, then nearest
first.

**Cycling** (state: list 0xBD0C04/count 0xBD0C00, cursor 0xBD08C8, built
at 0xBD08CC, last press 0xBD08D0, mode 0xBD08D4):

- The list is built again when it is empty, never built, the mode changed,
  or **3000 ms after the last press**. A fresh list's cursor is 0: the
  nearest unit in the cone is selected, reverse or not, **even if it is
  the current target already** (a Tab after a pause can leave the target
  where it is).
- Otherwise, if the current target is the list's current entry, the cursor
  steps: forward +1; past the end it goes back to 0, and if the list is
  **1000 ms old** it is built again first. Reverse steps back, wrapping to
  the end. If the target is something else (a click, an assist), the cursor
  stays and that entry is selected again.
- From the cursor, the first entry whose unit still exists and still passes
  the mode's test is selected (`CGGameUI::Target` 0x524BF0:
  `CMSG_SET_SELECTION` and `PLAYER_TARGET_CHANGED` at once); distance,
  cone and screen are not asked again. Wrapping in this walk also rebuilds a
  list a second old. No entry left: the list is emptied and the target is
  left alone. **Nothing is printed** when there is nothing to select.
- `ClearTarget` (0x525FC0) and `TargetLastTarget` with no last target
  (0x525D70) zero 0xBD08CC: the next press builds afresh. Units appearing or
  leaving do not.

### Assist, focus, last target

- `AssistUnit(unit)` (0x525EB0): the unit is resolved by token or name. Not
  found: `ERR_UNIT_NOT_FOUND` "Unknown unit." when a name other than
  "target" was given, else `ERR_GENERIC_NO_TARGET` "You have no target.".
  Found: its `UNIT_FIELD_TARGET`; 0 is silent. Otherwise it is selected
  (0x5259E0: when the object exists or is a party/raid member's guid), and
  with `assistAttack` (0xBD0918) on, 0x6E4950 asks to attack it
  (StartAttack, which stops a running attack instead when it may not be
  attacked).
- `FocusUnit(unit)` (0x522480): resolves the unit and sets the focus
  (0x51FF20); `ClearFocus` 0x5225E0.
- `TargetLastTarget` (0x525D70) selects the previous target (0xBD07B8);
  `TargetLastEnemy` (0x525DF0) the last enemy (0xBD07C0) when it exists or
  is known; `TargetLastFriend` 0x525E50.

### StartAttack (0x6E2610)

1. Gives up unless in world, local player, not pacified
   (`UNIT_FLAG_PACIFIED` 0x20000) and the target exists. A target it may not
   attack (0x729A70 false) stops a running attack (0x6E1660) instead.
2. Stands the player up if sitting (0x6DCB40(0)) and selects the target
   (0x524BF0).
3. **No range check.** `CMSG_ATTACKSWING` (0x141, the target guid) is sent
   when the unit has no attack target yet, or when a stop is in flight
   (`+0xA2C`); the attack target is set at once with the "requested" flag
   (0x98E540: guid, `+0xA28` = 1). An attack already running on another unit
   is *not* re-sent: the stock client relies on the server to follow the
   selection, which AzerothCore does not do (its `HandleSetSelectionOpcode`
   only moves an auto-repeat spell).
4. Always: `SetSheathState(MELEE, animate, send)` (0x736D30(1,1,0)): the
   weapons are drawn with the sheath reach and `CMSG_SET_SHEATHED` (0x1E0)
   is sent; an auto-repeat spell (wand, Auto Shot) is cancelled (0x7FE140(0),
   0x807560(1) -> `CMSG_CANCEL_AUTO_REPEAT_SPELL` 0x26D); and the base
   animation is re-picked (0x73AC30(0x40, -1)). Because the attack target is
   already set, that re-pick lands on the Ready stance (section 2). This is the
   "anticipation": the right-click alone draws the weapon and raises the
   guard, before any server reply.

### StopAttack (0x6E1660)

Sends `CMSG_ATTACKSTOP` (0x142, 0x7559E0, which also sets `+0xA2C`) when the
player was attacking or had asked to, cancels auto-repeat, clears the
click-to-move attack. The attack target itself is cleared by the server's
`SMSG_ATTACKSTOP`.

### Duels (DuelInfo.cpp, handlers registered by 0x5CFDD0)

The client keeps no duel state of its own beyond the flag's guid
(0xC22B58), the challenger (0xC22B60) and the countdown. Whether two players
may fight is read off two public player fields every time it is asked:
`PLAYER_DUEL_ARBITER` (UNIT_END + 0, the flag game object's guid, set on both
by the server's duel spell) and `PLAYER_DUEL_TEAM` (UNIT_END + 8, 1 and 2,
set by AzerothCore when the countdown ends and cleared with the arbiter when
the duel is over).

- **UnitReaction (0x7251C0)** between two units both flagged
  `UNIT_FLAG_PVP_ATTACKABLE` (0x8): each is taken to its player (0x718B70:
  the unit, else its `UNIT_FIELD_CHARMEDBY`, else its `UNIT_FIELD_CREATEDBY`,
  a player at the first or second step). When both players have a non-zero
  `PLAYER_DUEL_TEAM` and the same `PLAYER_DUEL_ARBITER`, the answer is
  **1 (hostile) for different teams, 4 (neutral) for the same team** - before
  the party, FFA and faction rules. So the opponent's nameplate, target frame
  and selection colour turn hostile only once the countdown is over.
- **CanAttack (0x729740)**, both player-controlled: the reaction test first
  (0x514050: 4 or better refuses), then the duel test - same arbiter, both
  teams set - answers **yes** whatever the PvP flags; only then the PvP and
  FFA bits. So TargetNearestEnemy, right-click attack and `UnitCanAttack`
  take the opponent (and its pet) for the duel and not before or after.

Messages (0x5CFCD0 .. 0x5CFB20; events 0x171-0x174 in the table at
0xC24EB0):

| Opcode | What the client does |
|---|---|
| `SMSG_DUEL_REQUESTED` 0x167 (flag, challenger) | the flag is kept. The challenger is the player: `ERR_DUEL_REQUESTED` ("You have requested a duel.", 0x5216F0(0x148)) and `CMSG_DUEL_ACCEPTED` for it (0x5CFBD0). A challenger on the ignore list (0x6B52E0): `CMSG_DUEL_CANCELLED` and the flag is dropped. Otherwise `DUEL_REQUESTED` with the unit's name (0x72A000) - only when the challenger is an object the client has; the challenger is kept either way |
| `SMSG_DUEL_OUTOFBOUNDS` 0x168 | `DUEL_OUTOFBOUNDS` and nothing else: UIParent shows the `DUEL_OUTOFBOUNDS` popup (`DUEL_OUTOFBOUNDS_TIMER`, 10 s timeout) |
| `SMSG_DUEL_INBOUNDS` 0x169 | `DUEL_INBOUNDS` (the popup is hidden) |
| `SMSG_DUEL_COUNTDOWN` 0x2B7 (ms) | count = ms / 1000; 0x5CF870 prints `DUEL_COUNTDOWN` ("Duel starting: %d") as a system chat line (0x509DD0), then again every 1000 ms (a 0x403370 timer, "ProcessCountdown") with one less, until the count reaches 0: "Duel starting: 3", "2", "1". No banner, no sound |
| `SMSG_DUEL_COMPLETE` 0x16A (started) | with a flag known: `ERR_DUEL_CANCELLED` (0x149, the info text) when the duel never started, the flag cleared, `DUEL_FINISHED`; the countdown timer is unregistered and the challenger cleared whatever |
| `SMSG_DUEL_WINNER` 0x16B (type, names) | `DUEL_WINNER_KNOCKOUT` or `DUEL_WINNER_RETREAT` as a system chat line |

The flag itself is an ordinary game object (type 16, DUEL_ARBITER, not
interactable) the server spawns between the two.

Wowee (`stock_duel.hpp`; `GameHandler::duelFieldsOf`, `duelReactionToPlayer`,
`unitHostileToPlayer`, `refreshDuelHostility`; `SocialHandler`'s duel
handlers) measured on the lab server with two clients, paladin Wptestb
challenging mage Wptester in Elwynn:

| Moment | Wptestb's view of Wptester |
|---|---|
| Before (no duel fields) | `UnitCanAttack` false, `UnitIsEnemy` false, `UnitReaction` 5, `UnitCanAssist` true; Tab selected nothing |
| Challenged and counting down (arbiter set, teams 0) | the same; "You have requested a duel." for the challenger, `DUEL_REQUESTED "Wptestb"` for the other, "Duel starting: 3", "2", "1" as system lines on both, 999-1000 ms apart |
| Duel on (teams 1 and 2) | `UnitCanAttack` true, `UnitIsEnemy` true, `UnitReaction` 1, `UnitCanAssist` false; Tab selected Wptester |
| Over (Grovel, `DUEL_FINISHED`, "Wptester has defeated Wptestb in a duel") | back to reaction 5, not attackable |

Wptester's Counterspell on Wptestb's Holy Light and Fire Blast on its
Hearthstone (InterruptFlags 0x10) gave the sequences of the table in section
3 on both clients. The name above the opponent was still the unflagged
player's blue in that run: 0x521BF0 answers red when each may attack the
other (0x729740 both ways), which Wowee's colour rule asked only through the
PvP flag; it now asks the duel too (`stockUnitColor`), not yet seen live.
Not run live: the out-of-bounds popup, a forfeit, the right-click attack -
the second account was taken by another agent's long test chain.

### Server messages (handler 0x756800, registered by 0x756BD0)

| Opcode | What the client does |
|---|---|
| `SMSG_ATTACKSTART` 0x143 | attacker's attack target = victim (0x754FF0), `+0xA28` = 0; `PLAYER_ENTER_COMBAT` (0x9B) for the active player |
| `SMSG_ATTACKSTOP` 0x144 | attacker's attack target and flags cleared, its pending blow played out (0x756770 -> 0x756180); `PLAYER_LEAVE_COMBAT` (0x9C) |
| `SMSG_ATTACKSWING_NOTINRANGE` 0x145 / `BADFACING` 0x146 | only records an error state with a time (0x6CEE70(1) / (2)) for the UI; the attack stays on |
| `SMSG_ATTACKSWING_DEADTARGET` 0x148 / `CANT_ATTACK` 0x149 | `StopAttack` (0x6E1660) |
| `SMSG_ATTACKERSTATEUPDATE` 0x14A | section 2 |

On a swing by the active player (0x6D4AD0) the error state is reset; if the
victim is farther than `4 * max(reachA + reachB + 4/3, 5)` yards the client
sends `CMSG_ATTACKSTOP` itself.

When something hits the local player and nothing is selected, the attacker is
selected (0x755020).

## 2. Auto-attack presentation

### A swing arrives (0x756800 case 0x14A)

1. Parsed by 0x755630 (hitInfo, attacker, victim, damage, sub-damages,
   victimState, attackerState, meleeSpellId, blocked, ...).
2. If the attacker is in view:
   - its weapons go to MELEE with the reach if they are not there
     (0x736D30(1,1,0) - any attacker, nothing sent);
   - its previous pending blow is played out now (0x756180) and this swing
     becomes the pending blow (0x755050);
   - the swing animation (0x755130, below);
   - if it was the local player's swing: 0x6D4AD0 (above).
   If only the victim is in view, the blow is acted out at once (0x755E40).

### Which swing (0x755130)

Nothing if the attacker is dead, `HITINFO_NO_ANIMATION` (0x40000) is set, or
the swing has a meleeSpellId (an ability's swing is the spell's animation).
Otherwise, by the swinging hand's item (vtable +0x12C):

| Hand (from `HITINFO_OFFHAND` 0x4) | Item | Animation |
|---|---|---|
| main | axe, mace, sword, exotic, misc (0,4,7,11,14) | Attack1H (17) |
| main | 2H axe/mace/sword, exotic2 (1,5,8,12) | Attack2H (18); Attack1H when the off-hand slot holds an item |
| main | polearm, staff, spear, fishing pole (6,10,17,20) | Attack2HL (19) |
| main | dagger (15) | Attack1HPierce (85) |
| main | fist weapon (13), anything else, nothing | AttackUnarmed (16) |
| off | dagger | AttackOffPierce (88) |
| off | any other weapon (fist too) | AttackOff (87) |
| off | no weapon | AttackUnarmedOff (117) |

There is no client-side alternation: the hand is the server's flag on each
swing. A model without the animation walks AnimationData's Fallback chain
(0x7176F0), e.g. AttackUnarmedOff -> AttackOff -> AttackOffPierce ->
AttackUnarmed. Then the attacker's attack voice plays (vtable +0x118 sound
type 0, or 1 on a crit) when victimState is not 0.

The swing is played when the packet arrives. Nothing is predicted.

### The blow lands (0x756240, the attacker's M2 event handler)

The pending blow (`+0xB98` attacker guid, `+0xBA0` victim guid, `+0xBA8`
hitInfo, `+0xBAC` damage, `+0xBEC` victimState, `+0xBF8` blocked) is acted
out by events in the **attacker's** animation - any animation it plays, not
only the swing:

- `$CPP` (0x50504324): the victim's defence. Skipped if the victim is dead or
  its current spell (`victim +0x964`, flag 0x200000 at +0xC) forbids it.
  victimState 3 (parry): the victim's parry (0x73B050); 2 (dodge) or 8
  (deflect): Dodge (30); otherwise, with a blocked amount: ShieldBlock (24).
  In each of those three cases it then **clears `HITINFO_AFFECTS_VICTIM`
  (0x2) from the pending blow** (`+0xBA8 &= ~2`) - even when the spell check
  skipped the animation. A hit with nothing blocked does nothing here.
- `$AH0`-`$AH3`: with damage, the attacker's CreatureSoundData custom attack
  sound (`+0x974` record +0x4C..+0x58), and hitInfo gets 0x200000. Then as
  `$CAH`.
- `$CAH` (0x48414324): with no victim in view, a creature's melee sound or a
  player's spell sound. Then, on a miss, dodge or evade (victimState 0, 2, 6)
  the rest of the animation plays at half speed (0x735CC0(model, -1, 0.5):
  0x827000 rescales track 0, the whole-body one, from now on; the next play
  resets it). Then the victim side (0x755E40) and the pending blow is
  cleared.
- `$CSS` (0x53534324): the weapon swoosh (0x746360 -> 0x4D0720,
  WeaponSwingSounds2 by the hand's swing size and crit), **only when the
  blow connects** - not for victimState 0, 2 or 6.
- `$BWP`: the bow pull (below); `$DTH`: death.

The defence comes early: HumanMale's `$CPP` is 67 ms into Attack1H (`$CSS`
300, `$CAH` 400), 0-333 ms across its attacks; Wolf's Attack 34/66 ms;
Kobold's 0-167 ms.

A blow whose events never come is played out when the attacker swings again
or stops (0x756180) - but that only shows its combat text (0x755A60): no
defence, no flinch, no blood.

### The victim side (0x755E40)

- Combat text, and with it the blow's sounds (0x755A60): a dodge or a miss
  (no damage) plays "(DONOTRENAME)Combat Miss 1H" or "2H" (SoundEntries 7080
  / 7081, MissWhoosh1Handed/2Handed) at the attacker, 2H when its main hand
  is a two-hander (0x714C80: subclasses 1, 5, 6, 8, 10, 12, 17, 20); a hit
  with damage plays the weapon's impact on the victim's armour (0x4D0660).
  Parry, block, evade, immune and deflect show text only here.
- With `HITINFO_AFFECTS_VICTIM` (0x2): the **wound** (0x736640(crit)); with
  damage (or fake damage) and victimState 1 or 4, blood (0x755380, from
  behind or in front by the attacker's side).
- Parry and block sounds (0x746540(0 / 1)); deflect: `ShieldWoodImpact`.
- Unless absorbed/resisted (0xA0) or immune: the victim's injury voice
  (vtable +0x118: CGUnit_C 0x747310, CGPlayer_C 0x7631A0) - crushing sound 9,
  crit 3, hit 2; a full absorb plays `AbsorbGetHit`. Each type has a chance
  out of 100 (a roll of `rand * 101 >> 32` against 0xADB5D8 for creatures,
  0xADBA74 for players): creatures hit 60, crit 100, crushing 100, attack
  cry 70; players hit 30, crit 100, attack cry 35. No time spacing.

**Wound on a dodge or parry.** AzerothCore sets `HITINFO_AFFECTS_VICTIM` on
every swing that is not a miss (Unit.cpp `CalculateMeleeDamage`: dodge, parry
and blocks included; an evade is a miss, an immune swing returns before it).
So the stock client's answer depends on `$CPP`: the defence plays there and
clears the bit, and `$CAH` then plays no flinch. Only an attack animation
without `$CPP` leaves the bit set, and then a dodge or a parry is a flinch at
`$CAH`. A partial block (victimState 1 with a blocked amount) is a
ShieldBlock and no flinch, the same way.

### Wound (0x736640)

Nothing if the unit is dead, has no model, or an animation layer with flag 4
is playing. Otherwise:

- crit: CombatCritical (10);
- the victim has an attack target: CombatWound (9);
- else StandWound (8).

Track: the whole body when the unit stands idle or in its Ready stance (and is
not mounted, moving, sitting or swimming); the upper-body bone set when it
moves or is mid-swing. Played once (0x735820, speed 1).

### Parry (0x73B050)

From the main hand: one-handers (0,4,7,11,14,15,20) Parry1H (21); 2H axe,
mace, sword, exotic2 Parry2H (22), Parry1H when gripped with an off-hand
item; polearm/staff/spear Parry2HL (23); a fist weapon, no weapon, or weapons
sheathed (`UNIT_FIELD_BYTES_2` byte 0 = 0, descriptor +0x1D0) ParryUnarmed
(20). Any other main-hand subclass plays nothing.

### Ready stance (0x724500 -> 0x71E0D0, 0x714DD0)

The base-animation chooser (0x724500) asks in order: death and spell-effect
loops (0x724060), 0x716FD0, stand-state emotes (0x71DFF0), **falling**
(0x724200: Fall 40), **movement** (0x717050), loot (0x724280), **cast
ready** (0x724330), **attack ready** (0x71E0D0), turning (0x71E180),
**ranged loading** (0x714F90), stand states (0x71E1F0: sit, sleep, kneel,
chairs), the NPC emote state (0x7171C0, `UNIT_NPC_EMOTESTATE`), and last the
idle (0x71E340: SwimIdle in water, StealthStand when stealthed, else Stand).

- 0x71E0D0 answers **only while the unit has an attack target**: the current
  combat move if one is playing, else a forced ready (`+0xB88`), SwimIdle in
  water, else the weapon stance (0x714DD0). Being in combat, being hit, or a
  creature's `UNIT_FLAG_IN_COMBAT` play no part.
- 0x714DD0, from the main hand's weapon, or the off hand's when the main hand
  holds none: one-handers including fist, misc, dagger and fishing pole
  (0,4,7,11,13,14,15,20) Ready1H (26); 2H axe/mace/sword/exotic2 Ready2H (27),
  Ready1H with an off-hand item; polearm/staff/spear Ready2HL (28); nothing
  ReadyUnarmed (25).
- 0x714F90 (sheath state RANGED, `+0xB5C` = 2, and an auto-repeat shot
  running, `+0xA38` & 0x200): LoadBow (105) for a bow, LoadRifle (106) for a
  gun or crossbow, LoadThrown (112) for thrown, HoldThrown (111) for a wand,
  else ReadyUnarmed. Once the weapon is loaded (`+0xA38` & 0x4000, set by the
  `$BWP` event, which also attaches the arrow) it keeps what plays.
  Starting to move clears the loaded state (0x72AFE0).
- When those end (0x73B510, the animation-finished handler): LoadBow plays
  HoldBow (109), LoadRifle HoldRifle (110); HoldBow, HoldRifle, HoldThrown
  and LoadThrown replay their hold while the shot stays on, else re-pick.
  Auto Shot sets no attack target, so 0x71E0D0 does not answer before it:
  the stance between shots is Load then Hold, never ReadyBow.
- Movement comes before both, so a moving unit runs; the stance returns when
  it stops (each movement message re-picks, section 3).
- While attacking or casting, Stand and the Ready stances play their first
  variation only - no idle fidgets (0x737EF0: an attack target, a cast
  (0x71DC70) or channel (0x71DD30), and the requested animation is Stand or
  has BehaviorID 25-29).

After `SMSG_ATTACKSTOP` the unit goes back to Stand. Its weapons stay drawn:
nothing sheathes them at the end of a fight (0x738180 below).

### Weapons and animations (0x738180, run after every play)

Covered in detail by the NPC sheath work (`weapon_anim_select.hpp`,
`presentedSheath`): an animation with WeaponFlags 0x04 (spell casts, swim,
mount, loot) puts the weapons away; 0x20 (attacks, Ready1H/2H, specials)
draws the melee weapons; 0x10 (emotes, ReadyUnarmed) puts them away unless
the unit has an attack target and the animation is a combat move (0x71D590);
a unit with an attack target has them drawn; otherwise another unit follows
its `UNIT_FIELD_BYTES_2` byte 0. Every change plays the reach.

## 3. Movement interplay

### Movement messages (0x73ED10)

`CGUnit_C`'s movement event handler, for the local player and for every other
unit's `MSG_MOVE_*`:

- START_FORWARD / BACKWARD / STRAFE_LEFT / STRAFE_RIGHT, START_SWIM,
  START_ASCEND / DESCEND: clear the bow-pull state (0x72AFE0), re-pick.
- STOP, STOP_STRAFE, START/STOP_TURN, STOP_SWIM, FALL_LAND and the run/walk
  mode changes (when moving): re-pick.
- JUMP (0xBB): the jump grunt (sound type 0xB), JumpStart (37).
- Falling far (`MOVEMENTFLAG_FALLING_FAR` 0x2000, 0x73AD00): Fall (40).
- Landing (internal event 0x346): the land sound (0xC) and JumpLandRun (187)
  on the ground; JumpStart again when still flying.

A re-pick replaces whatever plays on the whole body, so **starting to move
cuts a whole-body swing**: the run takes over.

### Which locomotion (0x717050)

With any direction flag (0xF):

- swimming or flying (0x2200000): strafing SwimLeft (43) / SwimRight (44),
  backward SwimBackwards (45), else Swim (42);
- on a flying mount (0x4F5290): Fly (135);
- backward: Walkbackwards (13);
- stealthed (`UNIT_FIELD_BYTES_1` byte 2 & 2): StealthWalk (119);
- speed above 11: Sprint (143); above twice the walk speed: Run (5); else
  Walk (4).

Strafing has no animation of its own on land: the run or walk plays with the
body turned 45 degrees (90 with no forward/back) toward the strafe
(0x73DAB0, 0x719660; `body_facing.hpp`). Turning in place plays ShuffleLeft
(11) / ShuffleRight (12) (0x71E180, 0x73E2C0).

### Upper and lower body (0x7385C0, 0x723E30)

`PlayAnimation` can put an animation on the upper-body bone set (`+0xB84`)
and leave the legs on what they play. 0x723E30 decides: the animation is an
upper-body one (0x71D800, by BehaviorID: 2, 8-10, 14-36, 46-49, 51-74, 76-78,
80-90, 105-113, 117, 118, 122-125, 128-130, 133, 134, 136-138, 153-156, 185,
186, 195, 213-222, 225 - attacks, parries, wounds, Ready stances, spell
casts, ranged shots, emotes, the sheath reach) **and** the unit moves
(movement flags 0x2E000FF: a direction, a turn, pitch, swimming, ascending,
descending, flying), sits, or its Ready/cast state asks for it. So:

- a unit that swings while running keeps running and swings with its arms;
- a unit hit while running flinches with its upper body;
- a cast started on the move plays on the upper body;
- Kick (95), Whirlwind (126) and other whole-body moves take the whole body
  wherever the unit is.

When mounted, the mount plays the locomotion (0x71D6B0 list) and the rider
the upper-body part.

### Mounted and swimming combat

- **Mounted.** StartAttack (0x6E2610) and the right-click path (0x6E4950)
  never dismount: they set the attack target, draw the weapons and send
  `CMSG_ATTACKSWING` as on foot. (0x806550, called there, cancels a pending
  spell with SPELL_ATTR0 0x2, not the mount.) AzerothCore's `Unit::Attack`
  refuses a mounted player and answers `SMSG_ATTACKSTOP`, which clears the
  attack target. The player stays mounted and has to get down to fight.
- **Swimming.** 0x71E0D0 answers SwimIdle in water, so there is no Ready
  stance; a swing in water is an upper-body one over the swim (0x723E30:
  swimming is in the movement mask).

### Backpedal, strafe, turn, jump

- Backward: Walkbackwards (13) at any speed (0x717050); the speed is the
  server's run-back speed.
- Strafing: the run or walk with the body turned (above).
- Turning in place: ShuffleLeft/ShuffleRight (0x71E180), after attack ready
  in the chooser, so a unit in its Ready stance turning in place keeps the
  stance.
- Jump: JumpStart replaces the whole body; a swing in the air is upper body
  (falling and jumping are in 0x723E30's movement mask).

### Casts and movement

**Pressing a spell while moving** (0x739650, after the cooldown check, before
anything is sent or the mount is touched) fails with `SPELL_FAILED_MOVING`
(51) through 0x808200 (shown as ERR_SPELL_FAILED_S "Can't do that while
moving", once per burst) when the caster's movement flags (CMovement +0x44)
have any of **0xC0100F** - forward, backward, strafe, falling, ascending,
descending; turning and pitching are not moving - and:

- the spell is channelled (AttributesEx & 0x44) and its
  ChannelInterruptFlags have movement or turning (& 0x18), and is not aimed
  at a location (Targets & 0x60); or
- its InterruptFlags have movement (& 0x1), it is not aimed at a location,
  not auto-repeat (AttributesEx2 & 0x20: Auto Shot and Shoot are never
  refused), and either its cast time after the spell mods (0x7FF180, with
  0x7FDB50 for SPELLMOD_CASTING_TIME) is above zero or its
  AuraInterruptFlags have moving or turning (& 0x18).

So instants are cast on the move, Presence of Mind's Pyroblast too (its
-100% casting-time mod makes 0), and Blizzard or Flamestrike are not
refused at the press (they wait for the ground click). Spell mods apply
when the spell's SpellFamilyName is the player class's (0xD397B4) and
AttributesEx3 0x20000000 is clear: for each of the 96 SpellFamilyFlags bits
the spell has, the flat and percent mods of that bit are summed (0x7FD970),
then `(base + flat) * (100 + pct) / 100`, the factor held at zero.

**Starting to move** (CGInputControl 0x5FAA40, called by MoveForwardStart
0x5FC200, MoveBackwardStart, StrafeLeft/RightStart, JumpOrAscendStart
0x5FBF80, SitStandOrDescendStart, ToggleAutoRun, TurnOrActionStart and
CameraOrSelectOrMoveStart with both buttons down, MoveAndSteerStart - not
TurnLeft/RightStart) runs before the movement packet is sent:

- every cast still in flight (the list at 0xAF5254, the ground-targeting one
  excepted) whose InterruptFlags have movement is cancelled (0x806620 ->
  0x806200): `CMSG_CANCEL_CAST` (0x12F: a byte of cast count, then the
  spell id) and `UNIT_SPELLCAST_INTERRUPTED` with SPELL_FAILED_INTERRUPTED,
  then the stop;
- the channel (`UNIT_CHANNEL_SPELL`) whose ChannelInterruptFlags have
  movement (& 0x8) is asked to stop: `CMSG_CANCEL_CHANNELLING` (0x13B, the
  spell id, 0x8008D0). The channel itself ends when the server says so.

0x5FAA40 does not touch the auto-repeat itself (0xD397D0); only a shot
still in flight whose InterruptFlags have movement (Auto Shot's are 0x21)
is cancelled with the rest.

### Casts ending: events and messages (0x7FECC0)

The client keeps a record of every cast of the player's in flight (the list
at 0xAF5254: 0x805010 adds one at the send 0x80AC90, 0x805180 finds one by
count, spell and caster, 0x805100 frees it). An answer that finds none -
because the client already ended that cast, or it is another unit's -
works on a fresh copy (the player's) or a stack record (anyone else's) and
does the same. An Auto Shot or Shoot press is answered by `SMSG_SPELL_START`
but never by a `SMSG_SPELL_GO` (each shot is the server's own cast, count 0),
so its record stays as long as the repeat does.

Every way a cast ends goes through **0x7FECC0(unit, spell, result)** with a
record (count, spell):

1. When the record is the unit's own cast (`+0xA5C` count, `+0xA60` spell):
   0x7FEC00 puts the cast and its pose away (0x72E430 zeroes both).
2. The event, for every token naming the unit (0x60BF60 -> 0x60BB70:
   player, vehicle, pet, partyN, partypetN, raidN, raidpetN, arenaN,
   arenapetN, bossN, target, focus, mouseover), with (unit, name, rank,
   **the record's count**) - the cast id is that count:
   - result 187 (the client's "it went off"): `UNIT_SPELLCAST_SUCCEEDED`;
   - DONT_REPORT 27, SPELL_IN_PROGRESS 105, CHARMED 24: `UNIT_SPELLCAST_FAILED_QUIET`;
   - INTERRUPTED 40, INTERRUPTED_COMBAT 41: `UNIT_SPELLCAST_INTERRUPTED`;
   - anything else: `UNIT_SPELLCAST_FAILED`.
3. When the record's count is the unit's count as it was on entry and its
   spell is the one the bar shows (`+0xA6C`, set by `SMSG_SPELL_START`):
   0x726200 clears the bar and `UNIT_SPELLCAST_STOP` follows.

So a cast the client ended itself and the server then reports again fires
its event again and never a second STOP.

Who calls it:

| Path | Record | Result | Also |
|---|---|---|---|
| Move start (0x5FAA40 -> 0x806620 -> 0x806200(rec, 0, 1, 40)) | every record whose InterruptFlags & 1, but the spell waiting for its ground point | INTERRUPTED | `CMSG_CANCEL_CAST` (the record's count, the spell) only when the spell has a cast time after mods (caster vtable +0x148 > 0) or waits for the next swing (Attributes & 0x404). Auto Shot (cast time -1000000), an instant, a wand's Shoot: no packet |
| SpellStopCasting (0x809EA0 -> 0x809A80 -> 0x806200(rec, 1, 1, 27)) | the current cast's (`+0xA5C`, `+0xA60`) | DONT_REPORT | the same packet rule; then 0x5D9FF0 |
| `SMSG_CAST_FAILED` (0x809AF0 -> 0x808200, the caster only) | by count | the packet's | first the school's fizzle (Resistances.dbc FizzleSoundID by the first SchoolMask bit, 0x4CFF30; not for DONT_REPORT), then the text through the 3 s throttle (`ERR_SPELL_FAILED_S`, "Interrupted"), a combat log `SPELL_CAST_FAILED` (0x751AD0), then 0x7FECC0 |
| `SMSG_SPELL_FAILURE` (0x809C70) | by count | the packet's | the fizzle at the caster; the player's auto-repeat off for NEED_AMMO 52-54 (0x807560); 0x72ED80 ends the unit's cast if it is this one; then 0x7FECC0. **No text, no chat line** |
| `SMSG_SPELL_FAILED_OTHER` (0x806AD0) | by count | the packet's | only 0x7FECC0 |
| `SMSG_SPELL_GO` (0x80E1B0) | by count | 187 | |

AzerothCore's `Spell::cancel` for a cast still being prepared sends
`SMSG_CAST_FAILED` (INTERRUPTED, the caster only), then `SMSG_SPELL_FAILURE`
and `SMSG_SPELL_FAILED_OTHER` to everyone in range, the caster included. The
sequences that make, for Holy Light (holy: fizzle 1428):

| Case | At the key | `SMSG_CAST_FAILED` | `SMSG_SPELL_FAILURE` | `SMSG_SPELL_FAILED_OTHER` |
|---|---|---|---|---|
| Own cast, moving | `CMSG_CANCEL_CAST`, INTERRUPTED, STOP | fizzle, "Interrupted", INTERRUPTED | fizzle, INTERRUPTED | INTERRUPTED |
| Own cast, `/stopcasting` | `CMSG_CANCEL_CAST`, FAILED_QUIET, STOP | fizzle, "Interrupted", INTERRUPTED | fizzle, INTERRUPTED | INTERRUPTED |
| Own cast, kicked / damage cancel (Hearthstone, InterruptFlags 0x10) | - | fizzle, "Interrupted", INTERRUPTED, STOP | fizzle, INTERRUPTED | INTERRUPTED |
| Another unit's | - | (not sent) | fizzle, INTERRUPTED, STOP | INTERRUPTED |
| A press refused during a cast ("Another action is in progress") | - | fizzle, the text, FAILED_QUIET with the press's count; the running cast and its bar go on | | |

The cast bar (CastingBarFrame.lua) acts on the first INTERRUPTED whose cast
id is its own and ignores the rest (`fadeOut`).

**Food and drink** are auras with AuraInterruptFlags NOT_SEATED (0x40000):
0x5FAA40 sends nothing for them. The movement packet is the whole message:
AzerothCore's movement handler stands a sitting player up and the aura goes
with the stand.

## 4. Other units

All of the above is `CGUnit_C`, shared by every unit. For a unit this client
does not control:

- `SMSG_ATTACKSTART` / `SMSG_ATTACKSTOP` set and clear its attack target:
  Ready stance and drawn weapons between them;
- `SMSG_ATTACKERSTATEUPDATE`: its swing (weapon table above), its weapons
  drawn, its blow pending until its own `$CAH`; the victim reacts then;
- its `MSG_MOVE_*` go through 0x73ED10 (jump, land, re-pick) and the same
  locomotion choice;
- `UNIT_FIELD_BYTES_2` byte 0 is its sheath when it has no attack target and
  plays nothing that moves the weapons (0x738180);
- `UNIT_NPC_EMOTESTATE` is its idle when it stands with nothing better to do
  (0x7171C0), except toward the player who is talking to it when the emote
  has flag 0x2000.

## 5. Wowee against this

Fixed in this change:

| # | Gap | Fix |
|---|---|---|
| 1 | `startAutoAttack` refused targets beyond 8 yards ("Target is too far away."), so a right-click from across a clearing only selected; and an attack was dropped after 2 s beyond 9 yards | No range gate; the attack stays on until stopped (`combat_handler.cpp`) |
| 2 | The Ready stance came from "in combat" (being attacked, or `UNIT_FLAG_IN_COMBAT` for others), and for the local player only after the server's `SMSG_ATTACKSTART` | Ready follows the attack target: set on the click (predicted, as 0x6E2610), on `SMSG_ATTACKSTART`, cleared on `SMSG_ATTACKSTOP`, stop, `SMSG_CANCEL_COMBAT`, `DEADTARGET` (`combat_handler.cpp`, `character_animator`, `game_screen.cpp`, `EntitySpawner::unitIdleAnim`) |
| 3 | A swing on the move took the whole body (the character slid in its attack pose); a flinch on the move was dropped | Upper-body swing and flinch over the run when the animation is an upper-body one; starting to move cuts a whole-body swing (`AnimationController`, `AnimationCallbackHandler` for other units) |
| 4 | The victim reacted the moment the packet arrived, half a swing before the weapon landed | Reaction held per attacker until its `$CAH` (450 ms fallback, flushed by the attacker's next swing) |
| 5 | Every flinch was CombatWound | StandWound when the victim is not attacking, CombatWound when it is, CombatCritical on a crit |
| 6 | Deflect showed a flinch; an ability's blow (meleeSpellId, `HITINFO_NO_ANIMATION`) showed nothing on the victim | Deflect is a Dodge; ability blows make the victim react |
| 7 | Weapon tables: polearms swung Attack2HLoosePierce; an off-hand fist weapon swung AttackUnarmedOff; Titan's Grip two-handers used the 2H swing/stance/parry; a fist weapon stood in ReadyUnarmed; an off-hand-only character stood unarmed | Tables match 0x755130 / 0x714DD0 / 0x73B050 (`weapon_anim_select.hpp`, `melee_anim_chains.hpp`, `combat_fsm.cpp`, capability probe) |
| 8 | `SMSG_ATTACKSWING_DEADTARGET` left the attack intent on | Stops the attack, as 0x6E1660 |

Fixed in the second pass:

| # | Gap | Fix |
|---|---|---|
| 9 | Dodge, parry and block played at `$CAH` (the loader dropped `$CPP`) | `$CPP` is kept with the tagged M2 events (`isM2SoundEventTag`); the defence plays at the attacker's `$CPP`, and the blow's `$CAH` then makes no flinch, as 0x756240 clears `HITINFO_AFFECTS_VICTIM` (`AnimationCallbackHandler::onMeleeAnimEvent`) |
| 10 | Wound on dodge/parry undecided | As stock against AzerothCore: none when the defence played at `$CPP`; a flinch at `$CAH` when the attack animation has no `$CPP` |
| 11 | A parry with weapons put away used the weapon's parry | ParryUnarmed when the victim's weapons are sheathed: the local character's `UNIT_FIELD_BYTES_2` byte 0, the presented sheath for others (AzerothCore leaves the byte at 0 for playerbots that fight with drawn weapons) |
| 12 | No follow-through slowdown | A miss, dodge or evade slows the rest of the attacker's whole-body swing to half speed from its `$CAH` (`CharacterRenderer::setAnimationSpeed`; the local character's swing state is lengthened to match, `AnimationController::slowMeleeSwing`) |
| 13 | Auto Shot stood in ReadyBow/ReadyRifle, and only with a melee attack target; LoadBow was switched away from on its first frame | While an auto-repeat shot runs with the ranged weapon out: Load (LoadBow; LoadRifle for guns and crossbows; LoadThrown; none for a wand) to its end, then the looping Hold (HoldBow, HoldRifle, HoldThrown); moving drops it and standing loads again (`CombatFSM` RANGED_LOAD / RANGED_HOLD) |
| 14 | The renderer turned the idle local character toward its target in combat | Removed (`Renderer`); the character turns only by the player's input |
| 15 | Idle fidgets while attacking or casting | Stand and the Ready stances hold their first variation for a unit with an attack target, a cast or a channel (`CharacterRenderer::setFirstVariationPredicate`, set in `Application`) |
| 16 | An upper-body swing's (or flinch's) events never fired; the reaction waited for the 450 ms fallback and the blow sounds played at the packet | The overlay's torso sequence fires its events like the whole body's; the blow sounds also look for them there |
| 17 | The target's selection circle did not show the attack | Red, with its alpha running 0-128-0 each second, while the player attacks a target it can attack; for 500 ms after the attack is ordered, a second ring closes from 1.6R+2.3 to R (`OverlaySystem`, `GameScreen`) |
| 18 | A miss or a dodge played the weapon swoosh; every local swing also played a fixed 2D whoosh at the packet | Swoosh at `$CSS` only for a swing that connects; a miss or dodge plays Combat Miss 1H/2H at the attacker at `$CAH`; the fixed whoosh is gone (`CombatSoundManager`, `AnimationController`) |
| 19 | Injury voice one hit in three, at most once a second | The stock odds: creatures 60, players 30, crits always; no spacing |
| 20 | Attacking from a mount dismounted the player | No dismount; AzerothCore's refusal (`SMSG_ATTACKSTOP`) ends the attack and the attack is not re-asked from the saddle |

The selection circle (UnitSelectTexture, its size from the GeoBox, the
UnitSelectionColor colours, locked target only) was already the stock one;
the attack colour and the click ring above were what was left. Found in
0x73DAB0: when the locked target (0xBD07B0) is a unit the active player can
attack (0x729740) and the player is attacking (`player +0xA20`, or the
auto-ranged flag `+0x1858 & 2`), the target's `+0xA30` gets bit 0x10; every
500 ms 0xCA12BC toggles and the alpha byte of the colour at 0xADAA98 (BGRA
00 00 FF xx: red) is set to `remaining * 0.002 * 128` or its complement. The
colour getter (vtable +0x78, 0x718AC0) returns that colour while the bit is
set, the UnitSelectionColor rules (0x521BF0) otherwise. 0x725980 then draws
the decal again, for 500 ms (0x6CF2D0) after `player +0x1934` (stamped by
0x6E4950 when an attack is ordered on a new target), over a box grown by
`(1.6R + 2.3 - R)(1 - t)^3` with alpha times `1 - (1 - t)^3`, and past half
way times `1 - (2t - 1)^2` (constants 0xA34C5C, 0xA34C58, 0x9E2EC4).

Movement audit (section 3): backpedal (Walkbackwards at the run-back speed),
strafing (body turn), turning in place (Shuffle, after the Ready stance),
jumping and swimming while attacking (upper-body swings) were already as the
stock client has them.

Third pass (targeting and casting on the move, `input-timing.md` section 5):

| # | Gap | Fix |
|---|---|---|
| 21 | Two Tab implementations: 40 yards all round, nearest first, a list rebuilt on every spawn and after 4 s, corpses with loot at the end, a cleared target when nothing qualified; the Lua TargetNearest* never kept a list and Friend took players only | One `CombatHandler::targetNearest` with 0x524FC0's rules (`tab_target.hpp`): 41 yd in a 30 degree cone, 10 yd elsewhere, on screen, CreatureType flags, cone first then distance, the 3 s / 1 s list, nothing changed when nothing qualifies; `TargetNearest` (mode 0) added |
| 22 | `AssistUnit` said nothing for an unknown unit and ignored `assistAttack` | Stock errors and the attack (`CombatHandler::assistUnit`) |
| 23 | Wowee refused any cast-time spell on horizontal movement, after dismounting | 0x739650's rule and place; move starts cancel by InterruptFlags/ChannelInterruptFlags (0x5FAA40) |

Fourth pass (casts ending, duels, macros; `input-timing.md` section 6):

| # | Gap | Fix |
|---|---|---|
| 24 | The server's report of an interrupt the client had already shown: "Interrupted" a second time, "Holy Light: Interrupted" with a system chat line, and a second INTERRUPTED/STOP with no spell | 0x7FECC0 for every end (`stock_cast_events.hpp`): `SMSG_SPELL_FAILURE` and `SMSG_SPELL_FAILED_OTHER` say nothing and fire the event with the packet's spell and count; the text is `SMSG_CAST_FAILED`'s, once through the throttle; STOP only for the bar's cast |
| 25 | `SpellStopCasting`, `/stopcasting` and Escape cleared the cast with no event | DONT_REPORT: `UNIT_SPELLCAST_FAILED_QUIET` and STOP, then the server's reports |
| 26 | Duels unknown: the opponent was never attackable for Tab, right-click or `UnitCanAttack`, never hostile | `PLAYER_DUEL_ARBITER`/`PLAYER_DUEL_TEAM` read (`stock_duel.hpp`): reaction 1 for the other side, CanAttack yes before the PvP rule, for the duellists and what they control; hostility refreshed when the fields change; right-click on an attackable player attacks |
| 27 | `SMSG_DUEL_OUTOFBOUNDS` printed "You are out of the duel area!" twice (error text and raised error); the countdown was a "3, 2, 1, Fight!" banner of Wowee's; `DUEL_REQUESTED` fired with a hex guid for an unknown challenger; `SMSG_DUEL_COMPLETE` fired `DUEL_FINISHED` with no duel | The events alone for out of / in bounds; "Duel starting: N" system chat lines a second apart, stopped by `SMSG_DUEL_COMPLETE`; `DUEL_REQUESTED` only for a challenger the client has; `DUEL_FINISHED` and `ERR_DUEL_CANCELLED` only with a flag known |
| 28 | An Auto Shot or Shoot press in flight was untouched by a move start | Its record is cancelled with INTERRUPTED and no packet (cast time 0, not next-swing); the repeat goes on |
| 29 | Food and drink: `CMSG_CANCEL_AURA` and `CMSG_STANDSTATECHANGE` on a move start | Nothing sent; the movement packet ends them on the server |
| 30 | Every TargetNearest* in a macro ran | Once per macro (0xBEAF44 bit 0x20), modes 0-4 |

Still different:

- **A blow whose events never come** still plays its reaction (450 ms
  fallback, or when the attacker swings again); the stock client shows only
  the combat text then. Kept because an event can also be lost here to a
  model swap or animation culling mid-swing.
- **Parry with weapons sheathed** reads the presented sheath for other units,
  not the raw byte (see 11).
- **Half speed** is applied only to a whole-body swing. The stock call
  rescales the model's track 0 whatever plays there, so an upper-body swing
  on a running unit would slow its run for the rest of that animation.
- **Turning in place during Auto Shot** keeps the Load/Hold stance; stock
  shuffles (0x71E180 is asked before 0x714F90).
- **The attack colour's CanAttack** is approximated from the reaction and
  the PvP rule; the auto-ranged flag is approximated by an auto-repeat spell.
- **The 4x-reach self-stop** (0x6D4AD0) is replaced by Wowee's own 40-yard
  leash in `CombatHandler::update`.
- Target change while attacking: the stock client re-asks to attack but sends
  nothing while an attack runs; on AzerothCore the swing therefore stays on
  the old target, as it does in Wowee. Wowee does re-send `CMSG_ATTACKSWING`
  on a right-click of another unit, which does switch.
- Tab/TargetNearest: see `input-timing.md` section 7 for what is
  approximated (CanAttack/CanAssist, duels, vehicles, once per macro).
- Wowee keeps the attack request through an `SMSG_ATTACKSTOP` on foot and
  asks again; the stock client's attack target is simply cleared.
