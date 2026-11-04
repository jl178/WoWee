# Spell presentation audit, all classes (3.3.5a, build 12340)

A scripted pass over every class's spells in the running client against the
stock rules in `spell-cast-pipeline.md`, `particles.md` and `m2-runtime.md`
and the DBC data. Each spell was learned on the lab character, cast at a
training dummy or on the caster, and photographed at its precast, release,
flight, impact and aura (`a` 0.35 s into the cast, `m` mid-channel, `b` just
before release, `c`..`f` 0.25 to 3 s after). The audit found that most of
what was missing was not one spell's data but four stock systems Wowee never
had: the kit **character procedures** (chain effects, model colour, swing
trails, held poses), the **area shards** of ground spells, **camera shakes**
and **weapon missiles**. They are described in `spell-cast-pipeline.md`
sections 8-13 and now implemented; the tables below say what each class's
spells use and what was seen.

## Method

- Harness: a `WOWEE_DEBUG_LUA` script per class (scratchpad
  `spellaudit/run/*.body` on top of `common.lua`): `.levelup`, `.learn all my
  class`, `.cheat power`, `.cheat cooldown`, `.tele Nagrand`, an Advanced
  Training Dummy (`.npc add temp 24792`), a side camera
  (`WoweeDebug_Camera`, added to the debug hook), then `CastSpellByID` or,
  for ground spells, `.cast dest` at the dummy's `.gps`.
- The class is set in the lab DB while the character is logged out; race
  too where the server refuses the pair (night elf for druid and hunter,
  draenei for shaman - with human it lists no character).
- DBC side: `spellaudit/py/au.py` prints a spell's visual, every kit with
  its animations, models, sound, ShakeID, CharProcs and attached models;
  `ce.py` reads SpellChainEffects with its byte columns.
- Stock side: Wow.exe in Ghidra (functions named in the pipeline doc).

## What the class spells use (DBC)

Of the ~150 class spells checked (and the spells they trigger):

| Feature | Spells | Before | Now |
|---|---|---|---|
| CharProc 0/12 chain effect | Chain Lightning, Chain Heal, Drain Life, Mind Flay, Death Grip | no beam | bolt per hop (section 9) |
| CharProc 1 model colour | Frostbolt and Frost Nova chill, Immolate, Flamestrike, Ice Block, Shadowform, Drain Life, Plague Strike, Death and Decay | none | diffuse tint |
| CharProc 8 swing trail | Heroic Strike, Mortal Strike, Cleave, Crusader Strike, Raptor Strike, Sinister Strike, Eviscerate, Kidney Shot, Charge, Whirlwind, Heroic Throw, Plague/Blood Strike, Obliterate | none | `$WTB/$WTT` trail |
| CharProc 9 area shards | Blizzard, Rain of Fire, Hurricane, Volley, Death and Decay | cast/impact kits only | shards over the radius |
| CharProc 11 held pose | Ice Block | none | animation held |
| CharProc 13 flash | Chain Lightning impact, Execute | none | flash |
| CharProc 14 transparency | Stealth, Evasion, Shadowform, Dispersion, Killing Spree | done before | unchanged |
| Kit ShakeID | Thunder Clap | none | CameraShakes rows |
| MissileModel -1 | Heroic Throw | nothing flew | weapon copy spins to the target |

## Per class

Legend: OK = matches the stock rules; FIXED = was wrong, fixed (general
cause); GAP = still differs.

### Mage

| Spell | Checked | Result |
|---|---|---|
| Fireball, Frostbolt, Pyroblast | precast hands, release from `$CSL`, missile, impact, CombatWound | OK (release and flight from the earlier missile work) |
| Frostbolt chill | state kit 3531 (Ice_Precast_Uber_Head + CharProc 1 0x3C3CFF) | FIXED: tint applied (the state kit spawns, log); neither the tint nor the head model was visible on the wooden dummy in the shots |
| Arcane Missiles | channel kit, bolts per tick | OK |
| Blizzard | InstantArea ring, ImpactArea Blizzard_Spawn, persistent kit 9350 CharProc 9 (5 a second of Blizzard_Impact_Base) | FIXED: shards now fall over the radius; before only the ring and the cloud showed |
| Flamestrike | persistent Flamestrike_Impact_Base | OK |
| Ice Armor, Mana Shield, Frost Nova, Arcane Explosion | state and cast kits | OK |
| Ice Block | state kit 3709: icebarrier_state + CharProc 1 + CharProc 11 | FIXED: pose held, tint; the cube model renders faint (additive, mostly dark texture) |
| Channel animation during Blizzard | ticks are triggered SPELL_GOs | FIXED (general): a triggered SPELL_GO no longer ends the caster's channel state and pose |

### Warlock

| Spell | Checked | Result |
|---|---|---|
| Shadow Bolt, Incinerate, Death Coil, Immolate, Corruption | missiles, impacts, state kits | OK; Immolate's state tint (0xE75641) applied |
| Drain Life | channel kit 11762 CharProc 0 row 719 + 6 combo rows | FIXED: green spiral beam from the hand to the target's chest (screenshot `warlock_drainlife`) |
| Rain of Fire | persistent kit 9352 CharProc 9 (RainOfFire_Impact_Base, 5/s) | FIXED: fireballs land over the area |
| Hellfire | channel kit 4809 (ChannelCastOmni + hand fire), ring | Ring OK; channel pose and hands dropped at the first tick - FIXED by the triggered-SPELL_GO change (not re-shot after the fix, see gaps) |
| Shadowfury | InstantArea kit | OK |
| Metamorphosis | form display | OK (demon model) |

### Priest

| Spell | Checked | Result |
|---|---|---|
| Smite, Holy Fire, Mind Blast, SW:Pain, Penance | cast, impacts | OK |
| Mind Flay | channel kit 11744 CharProc 0 row 750 (+747, 748, 746, 751) | Chain created along the channel target (log), bolts build (offline check: 10-54 vertices each); the beam was not visible in the shots (thin, alpha 0.2 additive and dark alpha-blended rows) - unconfirmed, see gaps |
| PW:Shield, Renew, Flash Heal, Holy Nova, Inner Fire, Dispersion | state/cast kits | OK |
| Shadowform | state kit 3129: base particles, CharProc 1 0x270042, CharProc 14 0.65 | Tint applied (log); with the outdoor ambient the diffuse-only tint is hard to see - stock applies it to the sun's diffuse only too (0x82E140) |

### Shaman

| Spell | Checked | Result |
|---|---|---|
| Lightning Bolt, shocks, Healing Wave | missiles, impacts | OK |
| Chain Lightning | cast kit 321 CharProc 0 row 743 | FIXED: jagged bolt caster -> first hit -> second hit for SegDuration 1 s (`cl_cl2` frame b) |
| Chain Heal | cast kit 3169 CharProc 0 row 736 | FIXED (same path); on self no hop |
| Lightning/Water Shield | state kits | OK (water shield orb renders very bright) |
| Totems | summons | OK |
| Windfury/Flametongue Weapon | cast kits; enchant glow | cast OK; glow not judged at the shot size |
| Ghost Wolf | form display | OK |

### Druid (night elf)

| Spell | Checked | Result |
|---|---|---|
| Wrath, Starfire, Moonfire, Insect Swarm, Roots | missiles, impacts, states | OK |
| Hurricane | persistent kit 9354 (cloud) + CharProc 9 CallLightning_Impact 0.7/s | FIXED (shards). The white-out in the shots came from ~12 dummies stacked by earlier runs, each taking the tick's impact |
| Starfall, Typhoon | cast/missiles | OK |
| Forms (cat, moonkin, tree, travel) | display swap | OK; bear's shot came during a Starfall packet backlog |

### Paladin, Warrior, Rogue, Hunter, Death Knight

Cast and aura kits OK where the spells went off (Devotion Aura,
Consecration's persistent kit, Holy Light, Divine Shield, Battle/Berserker
stances, Thunder Clap's world effect, Death Grip's chain - FIXED, log shows
the bolt caster -> target). The melee specials, shots and Heroic Throw did
not go off in the recorded runs: the bags were full from earlier runs, so
the weapons were never equipped ("Must have a %s equipped"). The swing
trail, Heroic Throw's weapon missile and Thunder Clap's camera shake are
therefore implemented from the stock code but not seen in game.

## Ranged, item and beam pass (spellfx harness)

A second pass on what the first left open, on the lab character Wptestb
(human; priest, warrior), harness `spellfx/run/*.body` with
`burst(tag, n, dt)` series of shots and a camera that keeps caster and
dummy in frame. Before = the build without this work, after = with it.

| What | Stock (Wow.exe) | Before | After |
|---|---|---|---|
| Wand Shoot (5019), five schools (fire 5069, shadow 5071, nature 5244, arcane 4547, holy 22254) | the wand's ItemDisplayInfo.SpellVisualID (225, 226, 2797, 2799, 228) merged into the spell's empty visual (`0x720F80`): HoldThrown precast, AttackThrown cast with the school's sound, the school's bolt | a grey bullet streak (the old `launchRangedWeaponProjectile` guess) for the local player only | FireBolt / Shadowbolt / LightningBolt / Arcane_Missile / Holy_Missile flying from the hand, the precast sparks; every caster |
| Shoot with a bow (3018), arrows as ammo | bow visual 5 + SPELL_GO's ammo 5996: `ArrowFlight_01` (`0x4EAB50`) | nothing (3018 was not on the guessed list) | the arrow flies (log: `missile ... model=item 0`); too fast at 5 yd for the 0.8 s screenshot cadence |
| Throw (2764), thrown axe 5856 | visual 2027 + the axe as ammo (type 25) | a copy of the hand's weapon, local player only | the axe's own display model on the missile path |
| Heroic Throw (57755, -1) | the main hand's display (`0x732400`), motion 38 | a copy of the weapon on a straight line with a fixed spin | the display model with motion 38 (spin + parabola), release wait and speed rule |
| Mind Flay (48156) beam | rows 750 + 747, 748, 746, 751 as read; the bolt ends at the destination attachment or, when the model has none, 3/4 of the unit's height (`0x7FABF0`) | ended at the dummy's feet (missile fallback 15 -> 19), fogged | ends at 3/4 of the dummy's height, unfogged, by RenderLayer; colour, alpha, width read as stock (see below) |
| Weapon enchant procs | the proc's SPELL_GO plays its kits like any triggered spell: Fiery Weapon 13897's impact (kit 728, Fire_ImpactDD_Chest) on the target; Crusader 20007 and Mongoose 28093 (visual 4345, kit 146 BlessingofStrength_Base) and Berserking 59620 (kits 81, 12178, state tint) on the wielder | played | unchanged: a real Fiery Weapon enchant (803 on the item, auto-attack at the dummy) put kit 728 on the dummy at each of its procs (`procs_07`); Crusader and Berserking by `.cast` |

Mind Flay's rows were checked against the stock bolt (`0x9A9900` copies the
row's bytes +0x9C..+0x9F into the bolt as A, R, G, B; `0x9AA210` offsets each
side of the strip by `Width`; the vertex colour is that ARGB with the pulse
writing alpha). Wowee reads all three the same way. The beam is thin and
dark by its data: 750 is ManaBeam at alpha 51 added, the three spiral rows
are a 0.2-yard dark purple (55, 0, 53) alpha-blended, 751 a white pulse 8
yards long. What was wrong was where the beam ended (the ground under a
model without a Breath attachment) and the fog.

## Gaps

- Not confirmed in game: swing trails, camera shakes, the Hellfire
  channel-pose fix; the bow and thrown missiles were seen only in the log
  (they cross 5 yards in under 0.15 s).
- Wowee does not send CMSG_SET_AMMO: ammo cannot be put in the ammo slot
  from the client (the harness sets `characters.ammoId`).
- The stock lightning's GxRs_MatEmissive is set to the row's colour per bolt
  (`0x9AA210`); whether lighting is on for it was not settled, so a lit bolt
  could be up to the sun's diffuse brighter than Wowee's.
- Ground-targeted spells cannot be cast by a player: Wowee has no
  targeting circle and sends no destination. The audit used `.cast dest`.
- "Must have a %s equipped": the UI error is not formatted with the item
  class.
- CharProc 6/15/16/17 are read but not run (`spell-cast-pipeline.md`
  section 8a); an item missile's ItemVisual glow is not applied.
