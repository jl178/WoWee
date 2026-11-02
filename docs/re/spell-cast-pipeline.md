# Spell casting presentation (stock 3.3.5a, build 12340)

What the stock client shows when any unit casts a spell: which SpellVisualKit
plays when, on whom, at which attachment, for how long, and what the caster's
body does. Read from `Wow.exe` (12340) with Ghidra. Every rule names the
function it comes from. Offsets on a unit are `CGUnit_C` fields. The unit's
descriptor block (`+0xD0`) starts at the unit fields, so `+0xD0 +0x40` is
`UNIT_CHANNEL_SPELL`.

The second half lists where Wowee differed, and what was changed.

## Terms

- **visual**: the SpellVisual row for a spell. `0x7FA290` picks it: Spell
  field 131 (`SpellVisual[0]`), or field 132 when the second is set and
  `0x7F3A60()` < 2. `CGUnit_C` vtable +0xBC (`0x720F80`) merges in the
  visual of the unit's shapeshift form for any kit the spell's own visual
  leaves empty.
- **kit**: a SpellVisualKit row. Its table is at `0xAD4A5C` (ids
  `0xAD4A4C`..`0xAD4A48`).
- **play a kit**: `CGUnit_C::PlaySpellVisualKit` (`0x745230`). It takes a
  13-dword request built by `0x6CF240(spell, kit, kitType)`: spell, kit,
  kit type, a world position (0 = ride the unit), a guid, "play the kit's
  animation" (1 by default), and timing words.
- **kit type** (the switch in `0x745230`, and `0x744BD0`, which stops kits
  by type): 0 cast, 1 impact, 2 state (and channel), 3 area (at a point),
  4 precast, 5 caster impact, 6 target impact, 7 missile targeting, 8 state
  done.
- **play an animation**: `CGUnit_C::PlayAnimation` (`0x7385C0`).
- **re-pick**: `0x73AC30(flags, -1)`, which plays whatever the unit should
  be doing now.

## 1. When kits play

### Cast start (`SMSG_SPELL_START`)

- `0x805330` handles a unit starting a cast. If the unit was casting
  another spell, `0x7FEC00` ends the old one first. When the unit has no
  current cast (`+0xA60`), it records the new one (`0x715A80`) and plays the
  build-up, `0x7FA2E0`.
- `0x7FA2E0` (precast):
  - Spell `Attributes` 0x2 (uses the ranged slot): the unit draws its
    ranged weapon (`SetSheathState(2)`, `0x736D30`).
  - A visual whose missile is the caster's weapon (`HasMissile` with
    `MissileModel` -1 or -2): the unit draws its melee weapons
    (`0x736D30(1)`).
  - If the precast kit names any animation (`StartAnimID` or `AnimID`
    above 0), `0x715AF0(1)` marks the unit as posing for a spell.
  - The **PrecastKit** plays on the caster as kit type 4.
  - SpellVisual `Flags` 0x20: the missile model is made now and held at the
    caster's `MissileAttachment` until it launches (`0x722950`).

### Cast end without a release

- `0x7FECC0` handles failure and interruption. `0x7FEC00` stops the
  caster's type 4 kits for that spell (`0x744BD0(spell, 4)`). If the precast
  kit named an animation, the unit re-picks (`0x73AC30(0, -1)`).
- A stopped kit fades out over 0.15 s (`CEffect` stop, `0x6F87C0`, constant
  `0x9F23D4`). Its emitters stop and the model goes when its particles die.

### Cast release (`SMSG_SPELL_GO`)

`0x80E1B0` parses the packet and plays, in order:

1. The **CastKit** on the caster, kit type 0. Skipped only when the client
   already showed this cast itself (a predicted throw: `+0x139`, set in
   `0x809F80`). Triggered spells (`CAST_FLAG_PENDING`) are not skipped.
2. With a destination (target flags 0x40): the **InstantAreaKit** at the
   point, kit type 3. Then the **ImpactAreaKit** at the point, at once when
   the spell has no missile (`HasMissile` 0 or `Speed` 0), else when the
   missile lands (`0x8022D0`, `0x700E20`).
3. `Speed` > 0: missiles to every hit and missed target (`0x7FFFB0`,
   `0x732FF0`). The impact waits for each missile.
4. `Speed` 0: the impact on every hit target at once (`0x801F10`).

### Which impact kit (`0x800D00`, `0x801F10`)

- On the caster itself: **CasterImpactKit**, else **ImpactKit**.
- On any other target: **TargetImpactKit**, else **ImpactKit**.
- One kit per unit. TargetImpactKit replaces ImpactKit; it does not add to it.

### Missile landing on a unit (`0x702DB0`)

- A living target flinches: `0x736640(0)`. That plays StandWound (8), or
  CombatWound (9) when the target has an attack target, as an overlay that
  does not stop what the unit was doing. SpellVisual `Flags` 0x40 turns
  this off. A dead target gets `0x73AF80(0)` instead.
- The impact kit (as above) plays on the target, kit type 1.
- The **StateKit** (kit type 2) plays on the target too, if the target is
  alive, the spell applies an aura there (`0x7282A0`), and the kit is not
  flagged 0x80 without `0x728300` agreeing.
- A missile landing at a point plays the ImpactAreaKit there, then the
  impacts on the hit targets (`0x700E20`).

### Channels

- Channel start (`UNIT_CHANNEL_SPELL` set, `0x72BC70`): the **ChannelKit**
  plays as kit type 2. Its animation plays unless the unit is already in
  the cast kit's animation (request word 6 = 0).
- Channel end (`0x73A520`) stops the visual's state, impact, caster impact
  and target impact kits on the unit (`0x744BD0` types 2, 1, 5, 6).

### Server-sent kits

- `SMSG_PLAY_SPELL_VISUAL` (`0x800610`): a kit by id on a unit, kit type 0.
- `SMSG_PLAY_SPELL_IMPACT` (`0x8006C0`): a kit by id on a unit, kit type 1.

## 2. Where a kit's effects go (`0x745230`)

| Kit column | M2 attachment |
|---|---|
| HeadEffect (3) | 20 Head |
| ChestEffect (4) | 34 Chest |
| BaseEffect (5) | 19 Base |
| LeftHandEffect (6) | 21 SpellLeftHand |
| RightHandEffect (7) | 22 SpellRightHand |
| BreathEffect (8) | 17 Breath |
| LeftWeaponEffect (9) | 2 HandLeft (`0x73A6C0`) |
| RightWeaponEffect (10) | 1 HandRight (`0x73A6C0`) |
| SpecialEffect0..2 (11-13) | 23, 24, 25 (Special1-3) |
| WorldEffect (14) | in the world, at the request's position or the unit's |

- Chest is 34, not 15 (ChestBloodFront). 15 is a different bone, turned for
  the blood spray. On HumanMale 15 is bone 98 at (0.13, 0, 1.43) and 34 is
  bone 105 at (0.18, 0, 1.43).
- The weapon columns make the unit put its weapons away (`0x736D30(0)`)
  unless the spell has `SPELL_ATTR0_DO_NOT_SHEATH` (0x40000).
- **At a point** (kit types 3 and 7 with a position: flag 0x2000), only the
  BaseEffect and the WorldEffect play. A head, chest or hand effect has no
  unit to sit on and is skipped.
- Kits may add models through SpellVisualKitModelAttach (`0x7FA9F0`,
  `0x7FAA20`): an effect, an M2 attachment id (-1 = the world point), an
  offset and a rotation. 591 rows, mostly raid bosses.
- Kit `Flags` 0x1 (`0x72AF60`, vtable +0x104): the kit is queued on the unit
  for up to 10 s (`0x728050`) instead of playing now.

## 3. How long a kit's models live

The model's end callback depends on the kit type (`0x745230`):

- **One-shot** (types 0, 1, 5, 6, 8: `0x744870`): the birth sequence plays
  once. At its end the model plays Decay (159) if it has it, then goes.
  Otherwise it holds its last frame and fades out.
- **State and channel** (type 2: `0x7449C0`): after the birth, Hold (158)
  loops. Kit `Flags` 0x20 loops Stand (the birth) instead.
- **Precast and missile targeting** (types 4, 7: `0x7435A0`): the birth
  sequence replays. A precast kit never goes to Hold.
- Stopping a kit (`0x6F87C0`): alpha to 0 over 0.15 s, emitters off, the
  model goes when its particles are gone.
- Every spell model plays its alpha tracks (M2Color alpha, texture weight)
  per frame. The Judgement hammer (`judgement_impact_chest.m2`) is a mesh
  5.3 yd above the attachment. It falls 4.7 yd in 267 ms. Its texture
  weight is 0 until 134 ms, full from 267 to 400 ms, 0 again by 900 ms.

## 4. Sounds

- The kit's `SoundID` (kit +0x3C) plays with the kit. When the kit made no
  model it plays on its own at the unit (`0x73B140` -> `0x6F7E40`).
- The kit's `ShakeID` (kit +0x40) shakes the camera (`0x7FA620`, up to three
  SpellEffectCameraShakes rows) unless the kit was queued.
- Spell models' own `$SND` events play from the model. `0x7F9D80` lets
  every fourth one through (a global counter).
- `MissileSound` plays on the missile (`0x7022D0`, `0x4C6A40`).

## 5. The caster's body

### Which animation (`CGUnit_C` vtable +0xFC, `0x73B140`)

The kit's play flags first:

- Kit `Flags` 0x4 -> play flag 2: play the id as given.
- Kit `Flags` 0x8 -> 4: no model fallback.
- Kit `Flags` 0x10 -> 8: no Special1H/2H weapon swap.
- Kit `Flags` 0x100, spell `SPELL_ATTR0_DO_NOT_SHEATH`, or a weapon
  missile (-1/-2) -> 0x20: keep the weapons where they are.

Then:

- **Precast** (type 4) with `StartAnimID` >= 0: play `StartAnimID` and mark
  the unit (`+0xA38 |= 0x10000`). While the mark lasts, the unit's "spell
  pose" (`0x7224D0`) is `StartAnimID`; otherwise it is `AnimID`. The data
  pairs a one-shot start with a loop: LoadThrown -> HoldThrown, Submerge ->
  Submerged, Open -> Opened, SpellCastOmni -> ChannelCastOmni.
- Otherwise `AnimID` >= 0:
  - A wound (`BehaviorID` 8-10, `0x71D510`) is a flinch: `0x736640`
    (critical for 10). Frostbolt's impact kit names CombatWound (9).
  - State and channel (type 2): play `AnimID` unless already playing it,
    then re-pick.
  - Anything else: play `AnimID` (`0x7385C0`). A cast kit (type 0)
    records it at `+0xFA4` (`[0x3E9]`), which the missile launch waits on.
- `AnimID` -1 means no animation. 0 is Stand.
- After the kit, the sheath is re-checked (`0x738180`, below).

### Choosing the sequence (`0x7385C0`)

- Special1H/2H (57/58) without play flag 8 becomes SpecialUnarmed (118)
  with no weapons, Special2H with a two-hander, else Special1H.
- Without play flag 4, `0x7176F0` resolves the id against the model: the
  id, then AnimationData's `Fallback` chain, then a per-model-type table
  (`0xA349B4`).
- The unit keeps three tracks: full body, lower body and upper body
  (`0x7173F0`). Moving animations (Walk, Run, the shuffles, the jumps,
  swimming, StealthWalk, Fly, Sprint, JumpLandRun: `0x714E80`) keep the legs
  while another animation plays above (`0x71E400`, `0x737EF0`).

### Weapons during a cast (`0x738180`)

- The animation playing has AnimationData `WeaponFlags` 0x4 (all spell
  casts, ReadySpell, Channel): weapons put away (`SetSheathState(0)`)
  unless play flag 0x20.
- The unit is casting (`+0xA60`) a spell without `DO_NOT_SHEATH`: put away.
- `WeaponFlags` 0x10: put away; 0x20: drawn.
- Otherwise drawn while the unit has an attack target, else its stored
  sheath state.
- So a rogue casting with daggers out puts them away for the cast.

### After the release

- A one-shot cast animation ends and the unit re-picks: the Ready stance
  of its weapons in combat, else Stand. The cast kit animation is not held.

## 6. Missiles

- **Model**: `MissileModel` (`0x732FF0`). -1, -2, -3 are the caster's main
  hand, off hand and ranged weapon models (`0x732400`); -4/-5 an item
  (`0x732100`); a missing model is `Spells\ErrorCube.mdx`.
- **Attachments**: `MissileAttachment` and `MissileDestinationAttachment`
  are indices into a table at `0xADAA20` unless SpellVisual `Flags` 0x200
  makes them raw attachment ids:
  0 Head 20, 1 Chest 34, 2 Base 19, 3 SpellLeftHand 21, 4 SpellRightHand 22,
  5 Breath 17, 6-8 Special1-3 (23-25), 9 ChestBloodFront 15,
  10 ChestBloodBack 16, 11 SpellHandOmni 37, 12 SpellHandDirected 38,
  13 SpellRightHand 22, 14/15 attachments 47/48.
- **Destination** (`0x7022D0`): that attachment on the target; if the model
  lacks it, 15, then 19, then the unit's origin. `MissileImpactOffset`
  (SpellVisual 29-31) is added.
- **Source** (`0x7FAA40`): the caster's `MissileAttachment` if set and
  present; else the model's `$CSL` event point; else the unit's position
  plus `MissileCastOffset`, at 3/4 of its height.
- **Release** (`0x734390`, `0x732650`, `0x715A50`): a missile does not leave
  at `SMSG_SPELL_GO`. It waits on the caster until the cast animation fires
  its release event (`$CSL`, `$CSR` or `$CST`). It then leaves from that
  event's point. It also leaves after 2 s, or at once when the caster is
  not in the cast kit's animation. HumanMale's SpellCastDirected has `$CSL`
  at 200 of 1000 ms: Frostbolt leaves the left hand 0.2 s after the cast
  bar fills. SpellCastOmni has `$CST` at 300 ms.
- **Speed** (`0x7022D0`): the time left is the distance over `Speed`, less
  the time already waited. The speed is what covers the distance in that
  time, at most twice `Speed`. The impact therefore lands when the server's
  flight time says.
- The missile model plays InFlight (144) if it has it, scaled to the flight
  time when the sequence is flagged so.
- **Path**: `MissileMotion` (SpellMissileMotion.dbc Lua) sets the shape and
  `MissileCount`. `MissileFollowGround*` are thousandths (`0x732FF0`):
  height `x * 0.001` (at least 0.25), drop speed `x * 0.001` (at least
  0.01), approach `x * 0.001` clamped to 0..1.
- **Missile targeting kit** (SpellVisual 22) plays as the missile is
  launched (`0x7022D0`): on a unit target as a state kit (type 2), at a
  point as type 7.

## 7. Gaps in Wowee, and what changed

| # | Stock | Wowee before | Now |
|---|---|---|---|
| 1 | Chest effects at attachment 34 (`0x745230`) | 15 first | 34 first, 15 as fallback |
| 2 | Specials at 23/24/25 | at the base (19) | 23/24/25, base as fallback |
| 3 | Missile attachment columns index `0xADAA20` | read as Wowee's kit slot enum, which has the weapon columns in the middle: index 6 meant LeftWeapon, 9+ were invalid | resolved through the stock table; `Flags` 0x200 raw |
| 4 | Destination fallback 15, 19 | the slot's fallbacks | 15, then 19 |
| 5 | One impact kit per unit: TargetImpactKit or CasterImpactKit replaces ImpactKit | ImpactKit and TargetImpactKit both | the stock choice |
| 6 | Missile waits for the cast animation's `$CSL/$CSR/$CST`, leaves from that point, speeds up to land on time | left at SPELL_GO from between the hands | waits for the release event of the caster's cast kit animation, from its point, with the speed rule |
| 7 | Target flinches when the missile lands (unless `Flags` 0x40) | flinched at the damage log, which comes with SPELL_GO - before the bolt arrived | flinch on landing; the damage log leaves missile spells alone |
| 8 | Spell models play their alpha tracks per frame | first key baked, so every layer showed from frame 0 (the hammer and its flashes at once) | played per frame for spell effect models |
| 9 | Area kits at a point show only Base and World effects | every slot, raised off the ground | Base and World only |
| 10 | Precast kits replay the birth; only state/channel kits go to Hold; kit `Flags` 0x20 loops Stand | Hold for any kit outliving its birth | as stock |
| 11 | Stopped kits fade over 0.15 s | 0.25 s | 0.15 s |
| 12 | A kit's AnimID plays as given (UseStandingLoop, ReadyThrown, ChannelCastOmni...) | precast/channel animations kept only if ReadySpell*/ChannelCast* | any animation the model has, except wounds and deaths |
| 13 | Weapons put away for spell-cast animations (`WeaponFlags` 0x4) | weapons stayed in hand | done by the melee work (`presentedSheath`, `0x738180`) |

Not changed:

- **Weapon missiles** (`MissileModel` -1/-2/-3): the caster's weapon model as
  the missile. Wowee flies nothing for these.
- **SpellVisualKitModelAttach** (591 rows): not loaded.
- **MissileFollowGround units**: Wowee reads hundredths of a yard and the
  approach as yards. The stock reads thousandths and the approach as a
  fraction of the path. The stock update loop that uses them was not read,
  so the units of the drop speed are not settled.
- **Missile cast and impact offsets** (SpellVisual 26-31): not applied.
  Nearly all rows are zero.
- **The do-not-sheath play flag** (kit `Flags` 0x100, `DO_NOT_SHEATH`,
  weapon missiles): `presentedSheath` does not see it, so those casts put
  the weapons away too.
- **Queued kits** (kit `Flags` 0x1) play at once.
- **Upper-body casting while moving** belongs to the movement work.
