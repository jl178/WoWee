# Death, the ghost world, corpses and loot

How the stock 3.3.5a client (build 12340) handles the player's death, the
release, the ghost world, the corpse run, resurrection, other units' deaths,
lootable corpses and loot rolls. Read from `Wow.exe` 12340 with Ghidra. Each
rule names the function it came from. The pure rules are in
`include/game/stock_death_rules.hpp` and tested by
`tests/test_stock_death_rules.cpp`.

Some of this is covered elsewhere and is not repeated here:
- the right-click and cursor rules for corpses, loot reach, auto-loot and the
  Loot animation: interactions.md §2, §5, §7;
- the ghost world's ffxDeath pass and the death LightParams: world-lighting.md
  §2 and §8;
- transparency (Ghost 0.5) and the mount: characters.md §6 and §7;
- the music and ambience slots: sound-engine.md §6.

The stock client's FrameXML (`Interface\FrameXML`) draws every popup named
here. The client fires the events and answers the Lua calls; the popups,
their texts and their buttons are FrameXML's. Wowee runs that same FrameXML,
so most of the work is making the events and the Lua answers stock.

Event ids: FireEvent is 0x81b530. Its event-name table is written at runtime
from 0xc24ee0 (the first twelve entries by other instructions), so an id is
the table index plus 12: PLAYER_ALIVE 0x101, PLAYER_DEAD 0x102,
RESURRECT_REQUEST 0x106, CONFIRM_XP_LOSS 0x183, CORPSE_IN_RANGE 0x184,
CORPSE_IN_INSTANCE 0x185, CORPSE_OUT_OF_RANGE 0x186, PLAYER_UNGHOST 0x188,
START_LOOT_ROLL 0x1ba, CANCEL_LOOT_ROLL 0x1bb, CONFIRM_LOOT_ROLL 0x1bc,
CONFIRM_DISENCHANT_ROLL 0x1bd. (0x5216f0, which looks similar, is the
game-error printer: it takes ERR_* indices.)

## 1. The player dies

### The moment of death (0x729220 → 0x6dc0f0)
0x729220 is a unit's death handler. It runs from a health drop to zero, and
from SMSG_DESTROY_OBJECT with its death byte set on a unit still alive
(0x4d7610). For the active player it calls 0x6dc0f0 unless
UNIT_DYNAMIC_FLAGS has 0x20 (UNIT_DYNFLAG_DEAD, which a feign death sets).
0x6dc0f0 then:
- starts the release timer (0x6dc070, below);
- clears the target (0x524bf0(0, 0));
- closes the NPC interaction (0x518d50 on 0xbd07a8);
- releases an open loot (0x523640(1, 0, 0), CMSG_LOOT_RELEASE);
- calls 0x809ac0;
- releases at once (RepopMe, 0x6d2950) when UNIT_FIELD_FLAGS has 0x100000.

For another unit it prints error 0x55 with the unit's name instead.

The body plays Death (1) once and holds its last frame. The base-animation
chooser (0x724060) answers Dead (6) for a dead unit, or Drowned (132). Dead
falls back to Death in AnimationData (row 6, fallback 1), and only one
3.3.5a model has a Dead sequence (`krakken.m2`). So a dead body is Death,
held. There is no screen fade, no desaturation and no light change while the
player lies dead: the ghost effect starts at the release (§2).

The death popup is FrameXML's. UIParent_OnEvent PLAYER_DEAD runs
`CloseAllWindows(1)` and shows StaticPopup "DEATH" when
`GetReleaseTimeRemaining()` is above 0 or exactly -1.

### The release timer (0x6dc070, GetReleaseTimeRemaining 0x516210)
The timer is decided at the death from PLAYER_FIELD_BYTES (0x4AD) byte 0:

| bits | timer |
|---|---|
| 0x10 NO_RELEASE_WINDOW | 0: release at once (0x513a30(0)) |
| 0x08 RELEASE_TIMER, and PLAYER_FLAGS without 0x4000 (out of bounds) | **360000 ms** (0x513a30(360000)) |
| otherwise | none (0x513a60: 0xbd084c = 1) |

`GetReleaseTimeRemaining` returns -1 with no timer, else
`(deadline - now) / 1000` as an integer (truncated), and 0 once the time is
up. AzerothCore sets RELEASE_TIMER when the map is not instanceable and no
prevent-resurrection aura is up (Player.cpp `ApplyModFlag(PLAYER_FIELD_BYTES,
RELEASE_TIMER, ...)`). So a death in the open world shows
`DEATH_RELEASE_TIMER` "6 Min until release" counting down, and a death in a
dungeon shows `DEATH_RELEASE_NOTIMER`. The automatic release at zero is the
server's.

- `IsOutOfBounds` (0x6123c0): PLAYER_FLAGS & 0x4000.
- `HasSoulstone` (0x51acd0): while dead, the PLAYER_SELF_RES_SPELL spell's
  name, else a soulstone item's text (0x6d6640), else nil.
- `RepopMe` (0x51aa90 → 0x6d2950): if dead, CMSG_REPOP_REQUEST 0x15a with a
  byte 0.
- `GetCorpseRecoveryDelay` (0x516280): the deadline from
  SMSG_CORPSE_RECLAIM_DELAY (0x269 → 0x513a80) minus now, whole seconds.

## 2. The ghost world

### The screen effect (0x4f88b0 → 0x4f7020)
The active player's screen effect is recomputed on each aura and flag change:
1. The auras from last to first: the first carrying SPELL_AURA_SCREEN_EFFECT
   (260) gives its misc value as the ScreenEffect.dbc row.
2. Else, PLAYER_FLAGS 0x10 (ghost), and the game state 0xbea570 not 4: row
   **1**, "Ghost Screen Effect".
3. Else PLAYER_FIELD_BYTES2 (0x4CD) byte 3 & 0x40 (the invisibility glow):
   row **81**, "Invis Screen Effect".
4. Else none: `0x4f7020(0)`. That drops the light override, picks the glow
   pass, and clears the music and ambience slot.

0x4f7020 with a row:
- Effect 0 glow, 1 **death** (the ffxDeath chain), 2 nether (a colour wash),
  3 special.
- `0x7ecec0(LightParamsID)`: the Light.dbc slot override (0xd38b58); values
  over 7 mean none.
- `0x4c8fa0(SoundAmbienceID, ZoneMusicID)`: the top music and ambience slot,
  10.

| row | name | effect | light slot | ambience | music |
|---|---|---|---|---|---|
| 1 | Ghost Screen Effect | 1 death | 4 death | 462 GhostState.wav | 36 GhostMusic03.mp3 |
| 81 | Invis Screen Effect | 2 nether | 4 | 462 | 36 |

49 spells carry aura 260 in 3.3.5a (Zombie-Vision, Arthas memories, the Ebon
Hold skyboxes, the Ulduar worm hole and others); they use the same path.

**A dead player who has not released has no screen effect.** The world keeps
its own light, colour and music until the release.

### The music and ambience slot 10
- Music (0x4c88b0 walks the slots from 10 down to 0): slot 10 comes before
  everything, including an intro that is playing. The silence after a zone
  track (0x4c87d0) applies to slots 0 to 2 only, so the ghost music starts at
  once.
- 0x4c8d80: setting the slot to a sound other than the one playing clears
  the silence (0xb4aaa0).
- No end callback is registered past slot 7 (0x4c9910), so when the ghost
  track ends nothing waits and it starts again.
- Clearing the slot hands back to the area. Its music, chosen anew, crossfades
  in at once, because the silence was cleared when the ghost music started.
- Ambience (0x4c86f0): the top slot's day or night SoundAmbience, with the
  same 5 s crossfade as any ambience change.

### Corpse range (0x51f5c0 → 0x512c40)
- Every frame, when the corpse map (0xbd0818) is known: the squared 3D
  distance from the player to the corpse position must be under **1600**
  (0xac8a78, 40 yards). The position is relative to a transport when the
  corpse lies on one (0x51f430).
- The edge fires CORPSE_IN_RANGE, or **CORPSE_IN_INSTANCE** when the corpse's
  map (0xbd081c) differs from the map of the position (0xbd0818: an instance
  corpse is reached at the entrance). Leaving fires CORPSE_OUT_OF_RANGE.
  FrameXML shows RECOVER_CORPSE or RECOVER_CORPSE_INSTANCE.
- MSG_CORPSE_QUERY (0x216, in 0x526530) stores the position only while the
  player is a ghost. A "not found" answer clears it and fires OUT_OF_RANGE.

### Retrieving the corpse (RetrieveCorpse 0x51b800)
It first **declines a resurrection on offer** (0x6d1d30(0):
CMSG_RESURRECT_RESPONSE 0x15c with 0, sent only when an offerer is
remembered). Then it sends CMSG_RECLAIM_CORPSE 0x1d2 with the corpse guid
(0xbd0828).

### The spirit healer (0x6db010, CheckSpiritHealerDist 0x6d1fc0)
- Only for a ghost. The healer guid comes from the gossip (SPIRITHEALER
  npcflag) or from SMSG_SPIRIT_HEALER_CONFIRM 0x222.
- The ghost must be within the healer's UNIT_FIELD_COMBATREACH + 4 yards.
  Then the guid is remembered and CONFIRM_XP_LOSS fires (FrameXML: XP_LOSS,
  XP_LOSS_NO_DURABILITY, or the NO_SICKNESS variants).
- `AcceptXPLoss` (0x51b6a0) calls the same function with a zero guid. It
  re-checks the distance and sends CMSG_SPIRIT_HEALER_ACTIVATE 0x21c.
- `CheckSpiritHealerDist` is the same distance test, strictly inside. The
  popup hides itself when the ghost walks off.
- `GetResSicknessDuration` (0x6118c0): ChrRaces[race].ResSicknessSpellID
  (default 15007). Its SpellDuration is `base + perLevel * level`, capped at
  max. The string is INT_GENERAL_DURATION when it is 1 s or more, else nil.

## 3. Resurrection by others

### SMSG_RESURRECT_REQUEST (0x6dbd00)
The stock reader:

```
uint64 offerer guid
uint32 name length         (including its terminator)
char[length] name
uint8  -> 0xc9eac0  ResurrectHasSickness
uint8  -> 0xc9eac4  ResurrectHasTimer   (also false while 0xbea570 == 4)
```

Nothing after these is read. AzerothCore writes `name, uint8 0, uint8
(caster is not a player)`, and the server comments call the first byte a
terminator. To the client:
- a player's resurrection has no sickness and no timer, so FrameXML's
  `ShowResurrectRequest` shows **RESURRECT_NO_TIMER**;
- a creature's has the timer and shows RESURRECT_NO_SICKNESS.

RESURRECT_REQUEST fires only while the player is dead or a ghost (vtable
+0x128). An empty name is looked up in the name cache first, and the event
follows the answer.

- `AcceptResurrect` (0x51aac0 → 0x6d1d30(1)): CMSG_RESURRECT_RESPONSE with 1.
  The offerer is then forgotten.
- `DeclineResurrect` does the same with 0.
- Being resurrected restores the body. The ghost flag and the screen effect
  go together (§2): the light, colour, music and ambience return when
  PLAYER_FLAGS loses 0x10. Then PLAYER_UNGHOST fires (0x6df710), or
  PLAYER_ALIVE when the player had not released (0x73d530 / 0x520f70).

## 4. Other units' deaths and corpses

- **Death, then down.** 0x729220 plays Death. When Death (1), Drown (131) or
  DeathEnd (468) finishes (0x73bbd0, the animation-end callback), the unit's
  +0xac0 is set to now. The same happens at creation of a unit that is
  already dead (0x7402b0). It is cleared when the unit lives again (0x73d530).
- **Lootable** (0x717b60): health < 1, UNIT_DYNFLAG_LOOTABLE, and +0xac0 set
  and passed. So **a corpse can be looted only once it has finished
  falling**, and the cursor (0x7207e0) offers loot only then.
- **The sparkle** (0x717ba0): at the same two moments, when the corpse has
  UNIT_DYNFLAG_LOOTABLE, an effect object (0x110 bytes, flag 8 "loot art")
  is attached to the unit. Its model is SpellVisualEffectName 14,
  "HARDCODED Loot Art", `Particles\LootFX.mdl`, at attachment 19 (Base). It
  goes when the flag goes (the corpse is emptied or someone else's).
- **Despawn** (SMSG_DESTROY_OBJECT 0x4d7610): the object is deleted at once
  unless something still holds it (+0xbc). There is **no fade-out** of a
  despawning corpse.
- A corpse model with Decay (159) plays it (0x744870). Without it, the model
  is paused at its sequence's last frame.

## 5. Looting

The loot window flow, reach, auto-loot and animations are in
interactions.md §5. Added here:

- **Money** (SMSG_LOOT_MONEY_NOTIFY 0x163 → 0x6ceca0): `uint32 amount, uint8
  soleLooter`. CHAT_MSG_MONEY (0x1c) with YOU_LOOT_MONEY ("You loot %s") when
  the byte is set, else LOOT_MONEY_SPLIT ("Your share of the loot is %s.").
  The amount is spelled out by 0x7e7d80: the non-zero coins, highest first,
  as GOLD_AMOUNT / SILVER_AMOUNT / COPPER_AMOUNT ("%d Gold") joined with ", ".
  No sound here: the clink is the coinage field's LOOTWINDOWCOINSOUND.
- **SMSG_LOOT_CLEAR_MONEY** (0x165 → 0x589100): marks the coin gone and fires
  LOOT_SLOT_CLEARED 1. When nothing else is left, it closes the loot.

### Loot rolls (LootRoll.cpp)
- **SMSG_LOOT_START_ROLL** (0x6fb2c0) reads: guid, map id, slot, item,
  suffix, random property, count, countdown, vote mask.
  - A roll for another map is ignored.
  - Each roll gets the next client roll id (0xca0690).
  - **At most four rolls are shown** (0xca0694 < 4): START_LOOT_ROLL(id,
    countdown). A fifth waits.
  - No chat line.
- **RollOnLoot** (0x588d60 → 0x6fbc70):
  - A BoP item asks CONFIRM_LOOT_ROLL first; disenchant asks
    CONFIRM_DISENCHANT_ROLL.
  - The vote must be in the mask.
  - Sends CMSG_LOOT_ROLL 0x2a0 (guid, slot, type), fires CANCEL_LOOT_ROLL(id),
    and frees a window. 0x6fb180 then shows the next waiting roll with its
    original countdown.
- **SMSG_LOOT_ROLL** (0x6fae40 → 0x6fa840) reads: guid, slot, roller, item,
  suffix, property, roll number (signed byte), type, auto-pass. It prints
  CHAT_MSG_LOOT (0x1b). The roll number is 128 for a choice not yet rolled.
  AzerothCore sends need as number 0 with type 0.

  | number | type | self | other |
  |---|---|---|---|
  | <0 | 2 | LOOT_ROLL_GREED_SELF | LOOT_ROLL_GREED |
  | <0 | 3 | LOOT_ROLL_DISENCHANT_SELF | LOOT_ROLL_DISENCHANT |
  | <0 | 0/1 | LOOT_ROLL_PASSED_SELF(_AUTO) | LOOT_ROLL_PASSED, or LOOT_ROLL_PASSED_AUTO(_FEMALE) |
  | ≥0 | 0 | LOOT_ROLL_NEED_SELF | LOOT_ROLL_NEED |
  | ≥0 | 1/2/3 | LOOT_ROLL_ROLLED_NEED / GREED / DE ("Need Roll - %d for %s by %s") | same |

  With showLootSpam off (cvar, default **1**, registered at 0x51ddbc), only
  the player's own automatic pass is printed.
- **SMSG_LOOT_ROLL_WON** (0x6fbb10 → 0x6fb700): LOOT_ROLL_YOU_WON /
  LOOT_ROLL_WON. With showLootSpam off it uses the `_NO_SPAM_NEED / GREED /
  DE` forms with the roll. It then closes the roll's window (CANCEL_LOOT_ROLL)
  and shows the next waiting one.
- **SMSG_LOOT_ALL_PASSED** (0x6fb600 → 0x6fb470): LOOT_ROLL_ALL_PASSED
  "Everyone passed on: %s", the same close.

## 6. Wowee: gap table

| # | Area | Stock | Wowee before | Now |
|---|---|---|---|---|
| D1 | Light while dead (not released) | the world's own; the death set only for the ghost's screen effect | death light and DeathSkybox from the moment of death | **Fixed**: light slot from the screen effect (`GameHandler::getScreenEffectLightSlot`) |
| D2 | Release timer | 6 min in the open world, none in instances (PLAYER_FIELD_BYTES) | `GetReleaseTimeRemaining` always -1: "Release to the nearest graveyard?" with no countdown | **Fixed** (`stock_death::releaseDelayMs`, PLAYER_FIELD_BYTES/BYTES2 in update_fields.json) |
| D3 | IsOutOfBounds | PLAYER_FLAGS 0x4000 | always false | **Fixed** |
| D4 | Target at death | cleared, loot released | kept (the target frame showed a stale self) | **Fixed** (`updateDeathState`) |
| G1 | Ghost music and ambience | GhostMusic (ZoneMusic 36) and GhostState.wav (SoundAmbience 462) in slot 10 | the zone's music and ambience | **Fixed** (`AudioCoordinator::setScreenEffectSounds`, `ZoneMusicInput::screenEffectMusic`) |
| G2 | Screen effect source | aura 260's row, else ghost row 1, else invis glow row 81 | ghost flag only, hard-wired | **Fixed** (`stock_death::screenEffectId`, ScreenEffect.dbc). Nether/special passes (types 2, 3) still not drawn (world-lighting.md) |
| G3 | CORPSE_IN_INSTANCE | when the corpse is inside an instance | never; IN_RANGE instead | **Fixed** |
| G4 | RetrieveCorpse | declines a pending resurrection first | did not | **Fixed** |
| G5 | CheckSpiritHealerDist | healer combat reach + 4 yd | true while the offer stood | **Fixed** |
| R1 | Resurrect popup choice | byte 1 sickness, byte 2 timer: a player's resurrection is RESURRECT_NO_TIMER | byte 2 as sickness, timer from a trailing word: a player's was RESURRECT_NO_SICKNESS | **Fixed** |
| R2 | RESURRECT_REQUEST while alive | not fired | fired | **Fixed** |
| C1 | Loot and sparkle timing | after the Death animation ends | as soon as the flag arrived, mid-fall | **Fixed** (`Unit::isCorpseSettled`, `CharacterRenderer::isDeathAnimationDone`) |
| L1 | Loot money line | CHAT_MSG_MONEY "You loot 1 Silver, 5 Copper" / "Your share of the loot is ..." | system "Looted: 1s 5c" | **Fixed** |
| L2 | Roll start | silent; four windows at most, the rest wait | "Loot roll started for ..." in chat; every roll raised a window | **Fixed** |
| L3 | Roll, win, all-passed lines | the GlobalStrings above, CHAT_MSG_LOOT, showLootSpam | "Name rolled Need - 0", "X won Y (Need - 54)", "Everyone passed on Y." as system lines | **Fixed** |
| T1 | Ghost tint | kit 989 CharProc 1 colour 0x8CB9FD | none | open (characters.md T3) |
| T2 | Corpse decay anim (159) | on corpse models that have it | not played | open |
| T3 | The ghost's spectral gryphon (mount display 26691 the realm sets) | drawn as a mount | mount loaded, the ghost drawn standing | open, not investigated |
| T4 | Auto-release on UNIT_FIELD_FLAGS 0x100000 | RepopMe at death | none | open (no 3.3.5a server path seen to set it) |

## 7. In game

Checked with the test character (Wptestb, level 8, Elwynn), `.die` at
Goldshire:
- Lying dead: the world keeps its daylight colours. The popup reads
  "6 Minutes until release" and counts down (`GetReleaseTimeRemaining` 359,
  355, 350...). The log shows `Death: release timer 360 s
  (PLAYER_FIELD_BYTES byte0=0x8)`. The target is cleared.
- Release: screen effect 1 comes in (grey ghost world, death light and
  DeathSkybox), with GhostState.wav and GhostMusic03 starting at once. The
  ghost music restarts as soon as it ends, with no silence.
- At the corpse: CORPSE_IN_RANGE, then "N Minutes/Seconds until
  resurrection", then "Resurrect now?". After the corpse is retrieved,
  PLAYER_UNGHOST fires and screen effect 0 hands the forest music and
  ambience back immediately.
- Not exercised in game: creature loot timing and the sparkle (the test
  character could not kill the spawned creature from a script), loot rolls
  (no group) and another player's resurrection. These are covered by
  `test_stock_death_rules` and by reading the code only.
