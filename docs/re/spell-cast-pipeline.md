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
  visual of the unit's ranged weapon for a spell that uses the ranged slot
  (section 6, "Ranged attacks").
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
  (`0x732100`); a missing model is `Spells\ErrorCube.mdx`. Details in
  "Item missiles" below. An effect model is made at its
  SpellVisualEffectName `Scale` (column 4, record +0x10, kept in the missile
  at +0xF4): 30% of the missile rows are not 1 (a boss's Frostbolt 2.5 or 5,
  `missile_thorns` 2, a small Fireball 0.5).
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
  launched (`0x7022D0`): on the target unit as a state kit (type 2), and
  when the missile has no unit target, on the caster at the point as type 7
  (only the base and world effects show). The missile keeps the unit's guid
  (+0x170) and launch time (+0x178); landing on a unit (`0x702DB0`), at a
  point (`0x700E20`) or being destroyed (`0x703730`) calls `0x6FF410`,
  which stops that kit on that unit (`0x744BD0` type 7 matches by the kit,
  the spell and the stamp, whatever type it was played as). 17 visuals, all
  creature spells: a hunter's-mark marker under the target of the Argent
  and Scarlet cannons and the practice arrows, Malleable Goo's puddle.

### Ranged attacks (`0x720F80`)

Auto Shot (75), Shoot (5019 wands, 3018 bows and guns) and Throw (2764)
have SpellVisual 0. Their presentation is the ranged weapon's:
ItemDisplayInfo `SpellVisualID` (column 11, record +0x2C) names a
SpellVisual row for every ranged weapon and wand. `0x720F80` (the unit's
vtable +0xBC, used everywhere a kit or missile is looked up: `0x7FA2E0`,
`0x80E1B0`, `0x744BD0`) builds the visual a cast plays:

- The spell's own row (or zeros when it has none).
- If the spell's `Attributes` has 0x2 (uses the ranged slot), the unit's
  ranged item display (vtable +0x134 with slot 2) and its SpellVisualID.
- Each of these columns the spell leaves 0 is taken from the weapon's row:
  Precast, Cast, Impact, CasterImpact, TargetImpact, MissileAttachment,
  MissileDestination, the four MissileFollowGround columns, MissileMotion,
  MissileSound, AnimEventSound. HasMissile and MissileModel go together,
  and only when the weapon's row has HasMissile.
- Nothing else: Flags, the channel, state and area kits are the spell's.

The weapon rows by kind (ItemDisplayInfo counts over Item.dbc):

| SpellVisualID | Weapons | Precast / Cast | Missile |
|---|---|---|---|
| 5 | bows, crossbows | LoadBow / AttackBow, sound 1146; impact CombatWound | none (the arrow is the ammo), MissileSound 4222 |
| 224 | guns | HoldRifle / AttackRifle, sound 1148 | none (the bullet) |
| 225, 226, 229, 2797, 2798, 2799, 228 | wands: fire, shadow, frost, nature (lightning), nature (goo), arcane, holy | HoldThrown / AttackThrown with the school's sound (5415, 5419, 5417, 5435, 5434, 5414) | FireBolt_Missile_Low, Shadowbolt_Missile, Frostbolt, LightningBolt_Missile, GooBolt_Missile_Low, Arcane_Missile_Lvl1, Holy_Missile_Low |
| 2026, 2027, 98, 4081 | thrown daggers, axes, torches, spears | ReadyThrown / AttackThrown, sound 7140, a swing trail (CharProc 8) | none (the weapon is the ammo), MissileSound 3318 |

So a wand's bolt is the wand's, not the school of the damage; a hunter's
Arcane Shot keeps its own missile and impact and takes the bow's draw and
release.

### The ammo (`0x80E1B0`, `0x7FFFB0`)

SMSG_SPELL_GO carries, after the targets and in this order: castFlags
0x800 the caster's power (uint32); 0x200000 the runes (two bytes, before
and after, then one byte for each rune set before and clear after);
0x20000 the missile's elevation (float) and delay (uint32); 0x20 the
**ammo**: an ItemDisplayInfo id and an InventoryType; 0x80000 two uint32s.
AzerothCore fills the ammo from the ranged weapon: a thrown weapon names
itself (InventoryType 25), a bow or gun its equipped ammo (24), a wand its
own type (26) and no display; creatures 5996 (arrow) or 5998 (bullet) by
their virtual ranged item.

`0x7FFFB0` flies a missile to each target when the visual has HasMissile
**or** the ammo display is set. A bow's visual has none; the arrow flies
because SPELL_GO named it.

### Item missiles (`0x732FF0`, `0x732400`, `0x732100`, `0x4EAB50`)

- MissileModel -1, -2, -3: `0x732400(slot)` asks the caster for the item in
  slot 0, 1, 2 (vtable +300: its InventoryType at +4; +0x134: its display)
  and builds it with `0x732100`.
- -4 and -5: `0x732100` with the ammo display and type from SPELL_GO.
- 0 with an ammo display: `0x4EAB50` builds the ammo's flight model.
- Anything else above 0: the SpellVisualEffectName model.
- `0x732100(display, type)`: folder by InventoryType - 1 Head, 3 Shoulder,
  14 Shield, 24 Ammo, 27 Quiver, anything else Weapon - then
  `ModelName[0]`, or `ModelName[1]` when the first is empty, with
  `ModelTexture` of the same index as replaceable texture 2 (item skin). A
  head item's file name gets the wearer's race and sex
  (`%s%s_%s%s.mdx`). The display's `ItemVisual` (column 23) goes on the
  model (`0x4EA8F0`): an enchanted weapon flies with its glow.
- `0x4EAB50(display, type)`: only type 24 (Ammo folder) or 25 (Weapon
  folder), only `ModelName[1]` and `ModelTexture[1]`, both required. Arrows
  (5996: `ArrowFlight_01`, `Arrow_A_01Brown`) and bullets (5998:
  `BulletFlight_01`) name only the second; thrown weapons both. The five
  thrown displays with only a first model fail here and the stock flies its
  ErrorCube.
- All of them fly as any missile: MissileMotion's script (Heroic Throw's
  "Forward Spin + Parabola" turns the weapon end over end; the thrown
  weapons' visuals name no motion, so a thrown axe flies as its model
  animates), InFlight (144) if the model has it, the release wait and the
  speed rule above.

## 8. Character procedures (SpellVisualKit CharProc)

A kit has four `CharProc` slots (kit +0x44..+0x50) and four parameters per
slot, `CharParamZero..Three` (kit +0x54, +0x64, +0x74, +0x84, one column per
slot). `CGUnit_C` vtable +0xFC (`0x73B140`) runs them after the kit's
animation, through `0x7265C0`, on the unit the kit plays on. -1 is an empty
slot.

| Proc | Kits | What it does (function) |
|---|---|---|
| 0, 12 | 5 class spells, 935 rows of chain effects | A chain effect (`0x7FC950` -> `0x7FC5F0`): ParamZero the SpellChainEffects row, ParamTwo != 0 it lasts as long as the kit, ParamThree != 0 every hop starts at the source. Section 9. |
| 1 | 335 | The unit's model colour (`0x7178E0` drops the same spell's older one, `0x6F76C0` puts the new one at the head of the unit's list +0xAFC). ParamZero is 0xRRGGBB. |
| 6 | 6 | A full-screen colour (`0x7FA450`, section 8a). |
| 8 | 130 | A swing trail on the weapons in hand (`0x715BA0` -> `0x7E4FF0`): colour ParamZero, alpha ParamThree, ParamTwo ms. Section 12. |
| 9 | 55 | An area's falling shards. Read by `DynamicObject_C` (`0x704F60`), not by `0x7265C0`. Section 10. |
| 11 | 90 | Hold the unit's pose (`0x6F80B0`): the animation clock of the unit model (+0xB4) and its mount (+0x98C) stops, at ParamZero seconds into the animation when that is set. Ice Block. |
| 13 | 36 | A flash of the model colour (`0x7265C0` case 13 -> unit +0xB10..+0xB1C): ParamZero for ParamOne s, back to white over ParamTwo s (`0x71A9A0`, `0x6ACC50` lerps by 255 x the time left). |
| 14 | 93 | Transparency, ParamZero (`0x71AB80`, `0x71ABE0`). Already in Wowee (`EntitySpawner::auraKitTransparency`). |
| 15 | 7 | A timed fade of the unit's alpha, not a scale (`0x71A940`, section 8a). |
| 16 | 1 | The mount's creature model from the spell's SPELL_AURA_MOUNTED (78) effect (`0x6F9670`, section 8a). |
| 17 | 14 | An item shown in its equipment slot on the unit, ParamOne the item id (`0x6F8650` -> `0x6F8600` -> `0x6F82D0`, section 8a). |
| 2, 3, 5, 7, 10 | 51 | Not read by `0x7265C0`. |

The model colour (`0x720DB0`, every frame) is the flash while one runs,
else the newest CharProc 1 colour, else white. It goes to the model's
+0x180..+0x188, which `CM2Model` (`0x82E140`) multiplies into the sun's
diffuse light only; the ambient (+0x18C) is added untinted. Frostbolt's
chill is 0x3C3CFF, Immolate's 0xE75641, Frost Nova's 0x6666FF.

### 8a. CharProc 6, 15, 16, 17

None of the 28 kits with these is a player spell: they are boss and quest
spells, holiday costumes and test rows. Wowee does not run them (see the
gaps); this is what the stock code does.

- **6, screen colour** (`0x7265C0` case 6 -> `0x7FA450`): ParamZero is
  0xRRGGBB (alpha forced to 0xFF), ParamOne a fraction. With the spell's
  cast time T (`0x7FF180`) and the kit's start t0 it sets one global fade:
  up from 0 to full by t0 + T x ParamOne, held to t0 + T, down to 0 by
  t0 + T + 100 ms. Each frame `0x7F9E10` works out the fraction and
  `0x7EE300` stores the colour and alpha (0xD38B50) for the world's
  full-screen pass. It is global: any unit's kit tints the screen. Users:
  Finger of Death 0xA81317 (impact), Brain Damage (state), Denouement Wisp,
  Darkness of a Thousand Souls, Force Prison 0x33F1.
- **15, alpha fade** (`0x71A940(alpha, mode, 500, hold, back)`): the
  arguments are ftol(baseAlpha x ParamZero), ParamOne, 500, ParamThree and
  ParamTwo, where baseAlpha is vtable +0xE8 (`0x715B50`: the display's
  model alpha / 255, else 1). It stores the byte, ParamOne (< 12, else
  nothing happens), now, 500 + ParamThree and ParamTwo at unit
  +0xB20..+0xB30 and fades the model's alpha to the byte over 500 ms
  (`0x744030`: target byte x 255, over the ms given). `0x73DAB0` (the unit's
  frame update) fades it back to baseAlpha over ParamTwo ms once
  500 + ParamThree ms have passed. The ftol makes the target 0 or 1, so the
  unit fades out (ParamZero below about 0.5) or stays: Teleport Visual
  (Evil) and the Cannon Port teleports vanish and come back, Coalesce and
  Materialize do nothing visible. The earlier "scale" reading was wrong.
- **16, mount model** (`0x6F9670`): for each of the spell's effects that is
  APPLY_AURA (6) with aura 78 (MOUNTED), the creature entry in its
  MiscValue is looked up in the creature cache (`0x67B6A0`) and its model
  made as an effect on the unit (`0x6F83D0`, CreatureDisplayInfo ->
  CreatureModelData). One kit, the precast of "Mount (Test Anim)" (39949).
- **17, item in a slot** (`0x6F8650`): ParamOne is an item id. The item
  cache (`0x67CA30`) gives its display (+0x14) and InventoryType (+0x18,
  turned into an equipment slot by `0x4E7A30`); `0x6F82D0` then shows that
  display in that slot on the unit's character model (`0x4EE460`,
  `0x4F2640`) for the kit's life - only on character models, or the head
  slot on anything. ParamZero is not read. Jack-o'-Lanterned! (the pumpkin
  head 33292), the Winter Veil hats, Mohawked!, the Gnomeregan, Orgrimmar
  and Darkspear Overcloaks, Cultist Hood, Mind Control Helm, Lovely Rose.

## 9. Chain effects (SpellChainEffects)

**Record.** 3.3.5a's file is 48 columns in 177 bytes: 39 four-byte
columns, then Alpha, Red, Green, Blue and BlendMode as single bytes
(+0x9C..+0xA0), then Combo (a string), RenderLayer, TextureLength and
WavePhase. Read as 48 four-byte columns the tail is garbage. Combo is up to
twelve four-byte groups, each another row id packed seven bits a byte so the
string has no zero byte (`0x9A8CE0`); every row named draws its own bolts
along the same hops (Drain Life 719 adds 720, 731-735).

**Points and hops** (`0x7265C0` case 0, `0x7FC5F0`, `0x7FA4D0`):

- The source is the unit the kit plays on. The targets are the unit's
  channel object when the kit's spell is the unit's channel spell
  (`UNIT_FIELD_CHANNEL_OBJECT`, +0x38 of the unit block), else the unit's
  stored target list. `0x724F50` stores SPELL_GO's hit list backwards and
  `0x7FA4D0` reads it backwards, so the hops run caster -> first hit ->
  second hit. The unit itself is left out.
- Hop i: from point i (point 0 with ParamThree), to point i+1, from
  `SegDelay + i * DelayBetweenEffects` ms to that plus `SegDuration`. With
  ParamTwo the hops never end; the object goes with the kit's effect.
- Each frame (`0x7FAE90`): the source end is `0x7FAA40` for point 0 (the
  visual's MissileAttachment, else the model's `$CSL` point, else 3/4 of the
  unit's height) and `0x7FABF0` otherwise (the visual's
  MissileDestinationAttachment plus MissileImpactOffset). Unlike a missile,
  `0x7FABF0` has no 15/19 fallback: a model without that attachment gets
  its position plus 0.75 x its height (`0x717AD0`: the model box's height
  times the unit's scale) - Mind Flay names the Breath, which a training
  dummy lacks. An end that cannot be placed hides the bolt (`0x9A9770`) but
  keeps it.
- Drawing (`0x9AB070`): fog off (GxRs_Fog 0), culling off, the bolts by
  RenderLayer 0 to 3; per bolt (`0x9AA210`) the blend is the row's, depth
  write only for blend 2 (alpha), GxRs_MatEmissive the row's RGB, the vertex
  colours the row's ARGB with the pulse in alpha.

**The bolt** (`CLightning`, Common\Lightning.cpp):

- Joints (`0x9AB2E0`): `round(length / AvgSegLen) + 2`, or `JointCount`
  with flag 0x1, plus one, 2..1000. Each is rolled (`0x9A90C0`) to an offset
  of up to `NoiseScale * length + JointOffsetRadius` in the bolt's side/up
  plane, a drift of up to `JointMoveSpeed`, and a timer. Flag 0x4: a joint
  re-rolls when its timer runs out; else it drifts.
- Laying (`0x9A9CA0`; flag 0x2 `0x9A9DC0`): each joint is put off the
  straight line by its offset, eased by `JointSmoothness`. With flag 0x2
  every `JointsPerMinorJoint * MinorJointsPerMajorJoint`th joint is a major
  (offset x MajorJointScale off the whole line), every `JointsPerMinorJoint`th
  a minor (x MinorJointScale, off the line between its majors), the rest off
  the line between their minors.
- Wave (`0x9A9B30`): `sin(WaveFreq * t * length + phase)`, the phase
  falling at `WaveSpeed` a second, along a direction at `WaveAngle` round the
  line (rolled between Min/MaxWaveAngle, turning at Min/MaxWaveSpin); flag
  0x8 turns the wave round the line instead. Arc: ArcHeight at ArcAngle.
  Both are scaled by `1 - 4 (t - 0.5)^2`, so the ends stay put.
- Strip (`0x9AA210`): two vertices a joint, `Width` either side across
  `segment x (mid - camera)`; a joint takes the mean of its two segments'.
  Flag 0x80: one width direction for the whole bolt. Both ends come to a
  point. u runs 0..1 along it, scaled to `length / TextureLength` with flag
  0x100, shifted by a random offset and `TexCoordScale` turns a second
  (`fmod`, `0x9AB3B0`); v is 0..1 across.
- Flicker (flag 0x10): on for Min..MaxFlickerOn s, off for
  Min..MaxFlickerOff, re-rolled as it comes back. Pulse (flag 0x20): only a
  stretch PulseOnLength long, faded over PulseFadeLength at each end, shows,
  PulsePos yards down the bolt, moving at PulseSpeed; flag 0x40 (or a
  negative speed) loops it.
- Colour: the row's ARGB bytes on every vertex, the pulse writing alpha;
  blend `BlendMode` (EGxBlend: 2 alpha, 3 add). Drawn by RenderLayer 0..3.

## 10. Area shards (SpellVisualKitAreaModel)

A DynamicObject (`0x704F60`, at creation) looks at its spell's
PersistentAreaKit: a CharProc 9 makes a "BlizzardObject" (`0x7FBD70`,
`0x7FB5B0`) with the SpellVisualKitAreaModel row ParamZero (by place in
the table; past the end it is Blizzard_Impact_Base), the object's radius,
and ParamOne a second scaled by the spell detail slider
(`(n * 1/9 * 0.9) + 0.1`, 1.0 at the top). The kit's SoundID loops at the
object. Each frame (`0x7FC220`) the rate (the clock capped at 1/15 s)
spawns shards: a uniform fraction of the radius along a uniform direction,
dropped to the ground between 16.7 yd above and 33.3 below, shown 0..510 ms
later, played once and removed. The object going stops the spawning; shards
in the air finish. Blizzard 5 a second of Blizzard_Impact_Base, Rain of
Fire 5 of RainOfFire_Impact_Base, Hurricane 0.7 of CallLightning_Impact,
Volley 5 of ArcaneShot_Area, Death and Decay 5 of DeathAndDecay.

## 11. Camera shakes

- A kit's ShakeID (`0x73B140` -> `0x7FA620`) and a spell model's `$SHK`
  event (`0x704C30`, data = the id) name a SpellEffectCameraShakes row, whose
  three columns are CameraShakes rows. Each goes on the camera's list
  (`0x606330`) with the place it went off.
- Each frame (`0x606970`): `t = elapsed + Phase`; past `Duration` it goes.
  Its size is `Amplitude / 36` yd, full within 9 yd of the player,
  `0.7^((d - 9) / 9)` out to 80, nothing beyond (`0x6004B0`). Only the
  strongest of each direction plays: `sin(2 pi Frequency t)` x size, times
  `exp(-t Coefficient)` for ShakeType 1, along the player's facing
  (Direction 0), across it (1) or up (2) (`0x5FE6C0`).

## 12. Swing trails

CharProc 8 calls `0x7E4FF0` on each weapon model in the hand. The weapon's
`$WTB` and `$WTT` events (1106 of 1379 weapon models) are laid down every
frame for ParamTwo ms (`0x7E4F50`). The trail (`0x7E4CE0`) keeps
`alpha / step` edges, `step = frame ms / 300 x alpha` (about 300 ms of
blade); from oldest to newest each edge is a step dimmer; past half the
duration the alpha drops a step a frame; under two edges it is gone. An
untextured, alpha-blended strip in the colour ParamZero.

## 13. More kit models (SpellVisualKitModelAttach)

`0x745230` walks the kit's attach rows (`0x7FA9F0`, `0x7FAA20`): each is an
effect model at an M2 attachment id (-1: the kit's point, played as its
world effect) with an offset and a yaw/pitch/roll. At a point only the -1
rows have a place.

## 14. Gaps in Wowee, and what changed

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
| 14 | Chain effects (CharProc 0/12, section 9) | none: Chain Lightning, Chain Heal, Drain Life, Mind Flay, Death Grip drew no beam | `LightningBolt` (stock CLightning) per hop, along SPELL_GO's hit list or the channel object, drawn as strips with the ribbons |
| 15 | Model colour (CharProc 1, 13) | none | per-instance diffuse tint in `character.frag`, newest tint per unit, flashes over it |
| 16 | Area shards (CharProc 9, section 10) | Blizzard, Rain of Fire, Hurricane, Volley, Death and Decay showed only their cast and impact kits | shards at the stock rate over the spell's radius, on the ground, for the area's life |
| 17 | Kit ShakeID and model `$SHK` (section 11) | none (SMSG_CAMERA_SHAKE only, with a guessed size) | CameraShakes rows on the camera, stock attenuation and strongest-per-axis |
| 18 | Swing trails (CharProc 8, section 12) | none | `$WTB/$WTT` edges on the weapons in hand, stock fade |
| 19 | Held pose (CharProc 11) | none | the unit's animation clock stops for the kit's life |
| 20 | SpellVisualKitModelAttach | not loaded | loaded; models at the attachment with offset and turn |
| 21 | Weapon missiles (`MissileModel` -1/-2) | nothing flew | a copy of the hand's weapon spins to the target, impact on landing |
| 22 | A triggered spell's SPELL_GO under another cast (Hellfire's and Blizzard's ticks) | ended the caster's cast state and its animation at the first tick | leaves the cast alone |
| 23 | Ranged attacks play the ranged weapon's SpellVisual (`0x720F80`): a wand's school bolt and sound, a bow's draw, a gun's report | Auto Shot, Shoot and Throw had no visual; the local player alone got a guessed arrow or bullet (`launchRangedWeaponProjectile`), a wand fired a bullet, nobody else's shots flew | the merge, for every caster (`SpellVisualSystem::rangedItemVisual`, `missilerules::mergeRangedItemVisual`) |
| 24 | SPELL_GO's ammo (castFlags 0x20) names the arrow, bullet or thrown weapon; it flies when the visual has no missile (`0x7FFFB0`, `0x4EAB50`) | not parsed | parsed (`readSpellGoTail`), flown as its item model |
| 25 | Item missiles (-1..-5) are the item's model by display (`0x732100`), flown as any missile: motion script, release wait, speed rule | -1/-2 a copy of the hand's weapon on a straight line with a made-up spin; -3/-4/-5 nothing | `itemMissileModel`: the display's model and texture through the stock folder rules, a character-renderer instance on the spell missile path |
| 26 | The missile model is made at SpellVisualEffectName.Scale | always 1 | `VisualRow::missileScale` |
| 27 | The missile targeting kit plays from launch to landing (`0x7022D0`, `0x6FF410`) | not played | on the target (state, Hold) or at the point (birth replayed), retired as its missile lands |
| 28 | Chain effects draw with fog off and by RenderLayer 0..3 (`0x9AB070`) | fogged like a ribbon, in creation order | unfogged, sorted by layer |

Not changed:

- **CharProc 6** (screen colour), **15** (alpha fade), **16** (mount
  model), **17** (item in a slot): read (section 8a), not run. All 28 kits
  are creature, holiday and test spells.
- **An item missile's ItemVisual** (`0x4EA8F0`: an enchanted thrown weapon's
  glow) and a head item's race/sex file name are not applied; where the
  stock flies its ErrorCube (no model) nothing flies.
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

## 15. Spells cast at a point (spell targeting mode)

Blizzard, Flamestrike, Rain of Fire, Volley, Hurricane, Death and Decay,
Flare, Mass Dispel and the engineering bombs are aimed at a point on the
ground. Consecration, Arcane Explosion and Goblin Sapper Charge are not:
their Spell.dbc `Targets` is 0 and they go out at once. Freezing Trap is
not either in 3.3.5a (Trap Launcher came later).

### What the spell waits for (`0x80C790`)

Pressing the spell (`Spell_C::CastSpell` `0x80CCE0`) builds the pending
spell object and asks `0x80C790` what it still needs. The answer
(`0xD3F4E0`) starts as Spell.dbc `Targets` (column 16) and is adjusted by
the **first** effect's `EffectImplicitTargetA` (column 86) only:

| ImplicitTargetA[0] | Effect on the mask |
|---|---|
| 1 UNIT_CASTER | clears 0x400 |
| 5 UNIT_PET | clears 0x8000 |
| 6, 53 | sets 0x80 |
| 21, 45 | sets 0x100 |
| 23 | sets 0x800 |
| 25, 63-71, 74, 75 | sets 0x2 (UNIT) |
| 26 | sets 0x4000 |
| 35 | sets 0x8 |
| 57, 61 | sets 0x4 |
| 89 DEST_TRAJ | clears 0x40 when the spell's missile row has flag 1 (vehicle aim, section V8 of mounts-taxi-vehicles.md) |
| 90 | sets 0x10000 |

Nothing left: the cast goes out (`0x80AC90`). Otherwise the current target
is offered to the unit flags (`0x80BC80`); what is still missing makes the
spell the **pending spell** (`0xD3F4E4 = spell`, `0x80D37B`), with:

- every other pending spell put down first (`0x809A60`, `0x80D3CE`) - a
  second press restarts targeting rather than stacking;
- `0x616270(2)` and the cursor **UnableCast** (`0x616800(0x1C)`, table
  `0xAD2808`: 2 Cast, 0x1C UnableCast);
- `CURRENT_SPELL_CAST_CHANGED` (`0x53B480` posts event 0xF3);
- InputControl `+0x58 |= 0x10` (`0x5FA890` keeps the cursor shown);
- for a spell that summons an object (effects 50, 76, 81, 104-107) a
  preview model of the object's display is loaded (`0x67BD40` ->
  `0x7FFE20`, `0xD3F570`).

All Blizzard/Flamestrike-type spells have `Targets` 0x40
(TARGET_FLAG_DEST_LOCATION) and an implicit target 16/28/31, so the mask
stays 0x40 and they wait for a point.

### The cursor over the world (`0x4FA040`, `0x4F66C0`)

While the mouse is over WorldFrame, its update (`0x4FA040`) picks:

- **no object at all** when the pending spell waits on a location only
  (`0x7FD620` pending, `0x7FD750` mask & 0x60, and not `0x7FD650`
  mask & 0x1878E): `0x715C40` clears the mouseover, so the circle goes on
  the ground under a creature, not on it;
- then `0x4F66C0` with the terrain point under the cursor
  (`{transport guid, x, y, z}`; the guid only for an MO_TRANSPORT, high
  `0x1FC`).

`0x4F66C0` stores the point (`0xB74380`, converted out of the transport's
frame when there is one) and runs the range check `0x803EE0`:

- 3D distance squared from the caster (`0x4F61D0`) against the spell's
  min/max range (`0x802C30` -> `0x801650`; the first SpellRange pair when
  there is no unit);
- `min^2 <= d^2 <= max^2`: **0**, in range;
- `d^2 > 16 max^2` (`0x9E8CCC` = 16): **2**, too far to show;
- otherwise **1**, out of range.

| Result | Circle (`0xAC79A4`) | Half-width (`0xB74370`) | Cursor |
|---|---|---|---|
| 0 | 0, Spell-Shadow-Acceptable | min(spell radius, 20) (`0x9F281C`) | 2 **Cast** |
| 1 | 1, Spell-Shadow-Unacceptable | 0 -> 1.3888889 (`0x9F98DC`) | 0x1C **UnableCast** |
| 2 | 3, none | 0 | 0x1C **UnableCast** |

**Spell radius** (`0x8019C0`): SpellRadius.dbc of `EffectRadiusIndex[0]`
and `[1]` (columns 92, 93), each `Radius + RadiusPerLevel x caster level`
(RadiusMax is not read), the larger of the two, then SPELLMOD_RADIUS
(`0x7FD970` op 6: `(r + flat) x pct x 0.01`).

### The circle (`0x4F8A40`)

WorldFrame's constructor (`0x4FABD0`) loads the two textures the table
`0xAC799C` names into `0xB74350/0xB74354`:

- `Interface\SpellShadow\Spell-Shadow-Acceptable.blp` (256x256, a green
  ring 0x18FA00 with a pale centre mark),
- `Interface\SpellShadow\Spell-Shadow-Unacceptable.blp` (64x64, red).

The world render (`0x4F8EA0`) draws it twice when `0xAC79A4 < 2`, once in
the terrain pass (`0x4F8A40(0x200122)`) and once with the WMOs
(`0x20000`). Each draw is the shadow-blob projector `0x7E4370` - the one
the selection circle uses (`0x725980`) - over the box
`centre +/- R` in x and y and `+/- 2` (`0xA4040C`) in z, with:

- no texture matrix (third argument 0): the texture lies along the world's
  axes, unlike the selection circle, which turns with the camera;
- colour 0xFFFFFFFF;
- render state 6 (blend) = 2, **alpha blending** (the selection circle is
  additive), fog and lighting states off;
- the second stage the "ShadowAdd" height ramp (`0x7E2C60`, `0xD38040`);
- depth bias 0.4 (`0x9F98D8`).

The `Projected_*` effects of `model2.wfx` are what the projector's
redrawn terrain and WMO triangles render with.

### Clicks and cancelling

- **Left click on the ground** (WorldFrame `0x4F7880` -> `0x527830` ->
  `0x527360`, button 1): `HandleTerrainClick` (`0x80C340`) first. With
  0x20 still wanted it stores the point as the source (`+0x48` transport
  guid, `+0x58..0x60` xyz), else with 0x40 as the destination (`+0x50`,
  `+0x64..0x6C`), sets the flag in the targets (`+0x28`) and clears it from
  the wanted mask; with nothing left the cast goes out (`0x80AC90`). The
  click then does nothing else (no target, no click-to-move). **There is
  no range check here**: an out-of-range point is sent and the server
  answers SPELL_FAILED_OUT_OF_RANGE.
- **Right button down in the world** (`0x51FB00`, button 4): the pending
  spell is put down (`0x809A60` -> `0x806200(spell, 1, 1, 0x1B)`); the
  press then goes on to its usual handling.
- **Escape**: FrameXML's `ToggleGameMenu` calls `SpellStopTargeting()`.
- **A new cast** (`0x80D3CE`), a world change (`0x403B70`), death...:
  `0x809A60`.

Putting it down uses SPELL_FAILED_DONT_REPORT (0x1B): no error text,
`UNIT_SPELLCAST_FAILED_QUIET` (event 0x147, `0x7FECC0`), the pending spell
cleared, `0x616270(1)`, the cursor back to the pointer.

### Lua

| Function | Address | Stock |
|---|---|---|
| `SpellIsTargeting()` | `0x7FDCD0` | 1 when a spell is pending (`0xD3F4E4`), else nil |
| `SpellStopTargeting()` | `0x809E30` | puts the pending spell down (DONT_REPORT); 1 if there was one, else nil - `ToggleGameMenu` relies on it |
| `SpellTargetUnit("unit")` | `0x80DC00` | offers the unit to the pending spell (`0x80BC80`); a pure location spell takes nothing from it |
| `SpellCanTargetUnit("unit")` | `0x804190` | `0x803A50(guid, 1)` |
| `SpellTargetItem(...)` | `0x8007E0` | only while 0x4010 (item) is wanted |
| `CameraOrSelectOrMoveStart/Stop` | `0x5FC6C0` / `0x5FC730` | the left button through InputControl (`0x5FA170` / `0x5FA450`); the click itself is resolved on release by the WorldFrame pick above |

### The packet (`0x80AC90`, `0x9AB8B0`)

`CMSG_CAST_SPELL` (0x12E; 0x1F0 for a pet): `uint8 castCount` (spell
`+0x24`), `uint32 spellId`, `uint8 castFlags` (0, or 2 with the
trajectory block), then SpellCastTargets (`0x9AB8B0`):

```
uint32 targetMask
if mask & 0x18A02   packed guid  unit / corpse / game object / minipet (+0x30)
if mask & 0x1010    packed guid  item / trade item               (+0x38)
if mask & 0x20      packed guid  source transport (+0x48), float x, y, z (+0x58)
if mask & 0x40      packed guid  dest transport   (+0x50), float x, y, z (+0x64)
if mask & 0x2000    string       (+0x90, 128 bytes)
```

A point on the ground is `0x40`, a zero byte (no transport) and the point
in server coordinates. On a ship (MO_TRANSPORT) the guid is the ship's and
the point is in its frame. After it: `UNIT_SPELLCAST_SENT` (event 0x143,
"player", name, rank, target name), the pending spell cleared, the cursor
back.

### SMSG_CAST_FAILED arguments (`0x808200`)

The error report fills the GlobalStrings format for the result from the
ids the server appended (AzerothCore's `Spell::WriteCastResultInfo`):

| Result | Appended | Filled with |
|---|---|---|
| 0x1D-0x1F EQUIPPED_ITEM_CLASS[_MAINHAND/_OFFHAND] | class, subclass mask | ItemSubClassMask.dbc name for exactly (class, mask) (`0xAD3F3C`), else the first ItemSubClass row of the class with its bit in the mask, DisplayName (`0x7FEA30`, +0x28) |
| 0x36 NEED_EXOTIC_AMMO | value | ItemSubClass of class 6, `1 << value` |
| 0x37 NEED_MORE_ITEMS | item, count | "%d %s" count, item name; an unknown item reports nothing |
| 0x5E ONLY_SHAPESHIFT | - | the spell's own Stances, SpellShapeshiftForm names joined with ", " |
| 0x64 REAGENTS, 0x83 TOTEMS | item | item name |
| 0x65 REQUIRES_AREA | area | AreaTable AreaName (+0x2C) |
| 0x66 REQUIRES_SPELL_FOCUS | focus | SpellFocusObject name (+4) |
| 0x81 TOO_MANY_OF_ITEM | limit category | ItemLimitCategory quantity and name (its own error id) |
| 0x82 TOTEM_CATEGORY | category | TotemCategory name (+4) |
| 0x93 PREVENTED_BY_MECHANIC | mechanic | SpellMechanic name (+4) |
| 0x96 MIN_SKILL | skill, rank | SkillLine name (+0xC), rank |
| 0xAC CUSTOM_ERROR | id | `SPELL_FAILED_CUSTOM_ERROR_<id>` |
| 0xB5 FISHING_TOO_LOW | skill | the number |
| 4 ALREADY_AT_FULL_POWER | - | the unit's power token (MANA, RAGE, ... `0x7FE4B0`) |

### Wowee against this

| # | Stock | Wowee before | Now |
|---|---|---|---|
| G1 | A location spell waits for a point (`0x80C790`) | sent at once with an empty target (TARGET_FLAG_SELF): the server refused Blizzard and Flamestrike | `spell_targeting::needsGroundTarget` (Targets + ImplicitTargetA[0]); the spell waits (`SpellHandler::groundTargeting_`) |
| G2 | The circle: Spell-Shadow-Acceptable/Unacceptable, alpha blend, +/- R x +/- 2, unrotated | none | `OverlaySystem::setSpellTargetDecal`: the selection circle's lattice projector with its own textures, alpha pipeline and +/- 2 yd height ramp |
| G3 | Radius from SpellRadius (effects 0, 1, per level), capped at 20; 1.389 out of range; none past 4x range | - | `decalHalfWidth`, `destRangeState` |
| G4 | Cursor Cast in range, UnableCast otherwise; no object hover | hover cursors | as stock while targeting |
| G5 | Left click on the ground casts there; right button down cancels; SpellStopTargeting / Escape cancel; a new cast replaces it | - | as stock (`GameScreen::processTargetInput`) |
| G6 | `CMSG_CAST_SPELL` 0x40 + packed transport guid + xyz | no builder | `CastSpellPacket::buildDestTarget` (TBC/Classic without the guid) |
| G6b | An item whose spell waits on a point (Saronite Bomb, Iron Grenade: `Targets` 0x40) waits the same way and goes out as `CMSG_USE_ITEM` (bag, slot, castCount, spell, item guid, glyph 0, flags 0, the same target block) | used at once with an empty target | `SpellHandler::beginItemGroundTargeting`, `UseItemPacket::buildDestTarget` (WotLK) |
| G7 | `SpellIsTargeting` / `SpellStopTargeting` answer for any pending spell | item cursor only, booleans | the location spell too; 1 / nil |
| G8 | SMSG_CAST_FAILED arguments filled into the format | only focus and totem ids read, the rest shown as "Must have a %s equipped" | every argument the server sends is read and formatted as above (`stockCastFailureText`) |

Not done:

- **Ground under a ship**: the point always goes out in world coordinates
  with no transport guid (the click does not know it hit the ship's hull).
- **SPELLMOD_RADIUS** and range modifiers are not applied to the circle.
- **The summon preview model** (`0xD3F570`) is not shown.
- **Unit-flag pending spells** (a heal pressed with no target and auto
  self-cast off, which stock also leaves on the cursor) still resolve at
  press time; `SpellTargetUnit` stays a no-op.
- A location spell pressed **while casting** is dropped or queued like any
  other; stock may let the circle come up during a cast.
