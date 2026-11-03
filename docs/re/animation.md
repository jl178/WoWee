# Unit animation: selection, fallbacks, variations, blending, emotes (stock 3.3.5a, build 12340)

How `Wow.exe` 12340 decides what a character or creature plays and how the
model plays it, beyond what the earlier notes cover. Read with:

- `combat-targeting-movement.md` - the chooser (0x724500), Ready stances,
  swings, wounds, parries, the upper/lower body split (0x723E30);
- `unit-motion.md` - moving units, speeds, the body-facing spring;
- `mounts-taxi-vehicles.md` - mounted units, tiers (0x7176F0);
- `m2-runtime.md` - CM2Model time, blending and track evaluation;
- `spell-cast-pipeline.md` - kits, cast animations (0x73B140).

Terms: **play** is `CGUnit_C::PlayAnimation` (0x7385C0, `(animId, flags)`),
**re-pick** is 0x73AC30 (ask the chooser 0x724500 and play its answer),
**SetBoneSequence** is `CM2Model` 0x832AB0 `(boneKey, animId, variation,
startOffset, speed, blend, primary)`. AnimationData.dbc record: +0 ID, +4
Name, +8 WeaponFlags, +0xC BodyFlags, +0x10 Flags, +0x14 Fallback, +0x18
BehaviorID, +0x1C BehaviorTier (table base `0xAD30E8`, min/max id
`0xAD30D8`/`0xAD30D4`). M2Sequence (0x40 bytes): +0 id, +2 variation, +4
duration, +8 MoveSpeed, +0xC flags, +0x10 frequency, +0x14/+0x18 replay
min/max, +0x1C blendTime, +0x3C variationNext, +0x3E aliasNext.

Code: `include/rendering/animation/anim_selection.hpp` (pure rules, tested
in `tests/test_anim_selection.cpp`), `CharacterRenderer::playAnimation`,
`LocomotionFSM`/`CharacterAnimator`, `ActivityFSM`, `EmoteRegistry`.

## 1. From an id to a sequence

### PlayAnimation (0x7385C0)

- Play flags: 1 = no blend, 2 = use the id as given, 4 = no model fallback
  (kit flags, see spell-cast-pipeline.md), 8 = no Special1H/2H weapon swap,
  0x10 = from the animation-end handler.
- Special1H/2H (57/58) without flag 8 become SpecialUnarmed (118) with no
  weapons, Special2H with a two-hander, else Special1H.
- Without flag 4: 0x7176F0 maps the id through the unit's tier
  (BehaviorID/BehaviorTier rows, 0x717540) and asks the model whether it has
  the id **or anything on its Fallback chain** (0x826050 -> 0x825F40). If
  so the requested id is passed on unchanged; otherwise the tier's Stand
  (0x717600) or the next tier (table 0xA349B4: `0,0,3,0`).
- **Variation -1 for every play** (`local_50 = -1`): loops and one-shots
  alike roll a variation in SetBoneSequence. 0x71DBC0 (BehaviorIDs 6, 132,
  0x1D3-0x1D4, 0x1D8: Dead, Drowned and the ghost/spirit set) keeps the
  playing variation instead.
- **Pinned stances** (0x737EF0): when the unit has an attack target
  (`+0xA20`), channels (`UNIT_FIELD_CHANNEL_SPELL`, `+0xD0`+0x40), has
  `+0xA38 & 0x400`, casts (0x71DC70) or channels (0x71DD30), and the id is
  Stand or has BehaviorID 25-29 (the Ready stances), variation 0 is forced.
  An explicit variation is not "random" (`bs+0x4B = 0`), so it is not
  re-rolled at the lap either: no fidgets in a fight.
- **Same id already playing** (0x737EF0): nothing restarts; only the speed
  is updated if it differs by more than 2.4e-7. So re-picks every lap are
  harmless.
- **Rate**: a moving animation (0x714E80, below) with a MoveSpeed on a unit
  with movement flags `0xC0000F` plays at `speed(0x987570) / |MoveSpeed|`.
- **Phase carry**: in the same case, if the sequence playing has a
  MoveSpeed too, the new one starts at `oldTime * newLen / oldLen % newLen`.
- **Blend** is `!(flags & 1)`: every chooser play fades.

0x714E80, the moving animations (by id): 4 Walk, 5 Run, 11/12 Shuffles, 13
Walkbackwards, 37/38/39 JumpStart/Jump/JumpEnd, 42-45 the swim strokes, 119
StealthWalk, 135 Fly, 143 Sprint, 187 JumpLandRun, 223 StealthRun.

### SetBoneSequence (0x832AB0) and the fallback chain (0x826350)

0x826350 resolves the requested id against the model and returns
`(id, mode)`:

```
dir = 1; acc = 0
while model lacks id:
    row = AnimationData[id]; stop at a loop or Fallback == id
    if row.Flags & 0x10: acc += dir; dir = -dir      # reverse
    if row.Flags & 0x20: acc += dir; dir = 0         # hold
    id = row.Fallback
found:     mode = dir > 0 ? 0 : dir < 0 ? 1 : 2 + (acc > 0)
not found: id = Stand if the model has it, else Closed (147), else the
           first sequence's id; mode 0
```

The mode reaches the bone state (0x826B00): **1 plays backwards from the
end, 2 holds the first frame, 3 holds the last**. AnimationData.Flags rows
with these bits in 3.3.5a:

| Requested | Flags | Falls back to | Plays |
|---|---|---|---|
| SitGround 97 | 0x62 | SitGroundDown 96 | held on its last frame |
| SitGroundUp 98 | 0x52 | SitGroundDown 96 | backwards |
| Sleep 100 | 0x63 | SleepDown 99 | held on its last frame |
| SleepUp 101 | 0x52 | SleepDown 99 | backwards |
| KneelLoop 115 | 0x22 | KneelStart 114 | held on its last frame |
| KneelEnd 116 | 0x10 | KneelStart 114 | backwards |
| UseStandingLoop 123 | 0x62 | UseStandingStart 128 | held |
| UseStandingEnd 129 | 0x52 | UseStandingStart 128 | backwards |
| Walkbackwards 13 | 0x11 | Walk 4 | backwards |
| Closed 147 | 0x20 | 146 | held |

So a creature with SitGroundDown only sits down, stays down and stands up
by sitting down in reverse. The other AnimationData.Flags bits (0x1-0x8,
0x40) are read nowhere in the client's animation code (all 44 readers of
`0xAD30E8` were checked); BodyFlags is read once, bit 0x80 of the playing
animation (0x71B000).

The bone state also remembers the **requested** id (`bs+0x90`), which is
what the unit later reports as its current animation (0x717260 ->
0x8267E0), not the fallback.

### Variations (0x826E60)

`rand()` (0..0x7FFF) walks the chain from variation 0 along variationNext,
subtracting each frequency (read as a uint32 at +0x10); the first variation
the roll falls inside plays. **A roll past every weight keeps variation 0.**
HumanMale: Stand 9830/2184/2184/18569 (sum 32767), AttackUnarmed, Attack1H
and Attack2H two swings each (16383), Attack2HL three (10922), EmoteDance
five (6553), Mount two.

### Laps, ends and replay (0x826B00, 0x832260, 0x831FC0)

- `loops = replayMin + (rand() * (replayMax - replayMin) >> 15)`, at least
  one. A sequence **with flag 0x01** plays `loops` laps, fires its end once
  and holds its last frame; a sequence **without 0x01 loops enhanced and
  fires its end at every lap**.
- The default end handler (0x831FC0), after the owner's callback, rolls a
  new variation at the lap of a looping sequence whose variation was rolled
  at random. Every lap of Stand therefore rolls again (HumanMale's Stand
  variation 1 has replay 2-7, which counts for nothing because Stand does
  not carry 0x01). The new variation starts with the overshoot as its
  offset and fades in over its blendTime.
- "Loop" in the stock sense is this flag: 0x01 off loops. 0x20 means the
  keys are in the .m2 (otherwise in an `.anim` file, loaded first by
  0x831C30 - the low-priority sequences), 0x40 an alias (aliasNext). No
  "LowDef" handling exists in 12340's animation code.

### Blending (0x826C40)

As m2-runtime.md: the old primary is copied to the secondary and fades out
over the **incoming** sequence's blendTime with weight
`smoothstep(fraction left)`; 0 cuts; if a fade is running whose outgoing
weight is still above 0.5 the old secondary and its clock are kept and
only the primary changes. The secondary **keeps playing on its own clock**
(looping, or held after its end).

## 2. The chooser's answers (0x724500)

Order: death/kit state loops (0x724060), custom stand state (0x716FD0:
stand state 10 plays Birth 127 on a model that has it), the vehicle seat,
submerge (0x71DFF0: stand state 9 Submerged 202, Submerge 201 on the way,
Emerge 224 or Birth 127 out), falling (0x724200: Fall 40 unless a
jump animation plays), moving (0x717050), loot (0x724280), cast ready
(0x724330), attack ready (0x71E0D0), turning (0x71E180), ranged loading
(0x714F90), stand states (0x71E1F0), the emote state (0x7171C0), and the
idle (0x71E340).

**0x717050** (a direction flag `& 0xF`): swimming/flying (`0x2200000`) the
swim strokes (SwimLeft/Right for strafes, SwimBackwards, Swim); a Flying
spline Fly 135; backward Walkbackwards 13; **stealthed (UNIT_FIELD_BYTES_1
byte 2 & 2) StealthWalk 119 at any pace** - StealthRun 223 is never asked
for; speed > 11 (`0xA34B20`) Sprint 143; speed > twice the walk speed
(`+0x818`) Run 5; else Walk 4. Strafing has no animation of its own
(RunLeft/RunRight 92/93 exist in AnimationData but nothing asks for them):
the body turns 45/90 degrees (0x73DAB0, `body_facing.hpp`).

**0x71E340** (idle): swimming/flying SwimIdle 41; stealthed StealthStand
120; hovering Hover 193; else Stand 0. There is **no fidget timer**: the
idle fidgets are Stand's low-frequency variations rolled at each lap.

**0x71E180** (turning in place, with turn flags 0x10/0x20 or the body
catching up): ShuffleRight 12 unless turning left (`+0x7CC & 0x10`) or
`+0xA38 & 0x800`, then ShuffleLeft 11.

### The animation-end handler (0x73B510)

Called at a sequence's end (every lap for a looping one), by BehaviorID:

| Ended | Then |
|---|---|
| Death 1 | Dead 6, held (variation kept) |
| JumpStart 37, Jump 38 | Jump 38 (unless flying) |
| JumpEnd 39 | clears the jump flag, re-pick |
| Fall 40 | Fall again |
| Loot 50 | LootHold 188 |
| SitGroundDown 96 / SitGround 97 | SitGround while stand state 1, else SitGroundUp 98 |
| SleepDown 99 / Sleep 100 | Sleep while stand state 3, else SleepUp 101 |
| SitChairLow/Med/High 102-104 | again while the chair state holds |
| LoadBow 105 / LoadRifle 106 | HoldBow 109 / HoldRifle 110 |
| KneelStart 114 / KneelLoop 115 | KneelLoop while stand state 8, else KneelEnd 116 |
| Birth 127 | clears its flag, re-pick |
| Drown 131 | Drowned 132 |
| ToFly/ToGround 458-460 | Settle 464 |
| 133 FishingCast | FishingLoop 134 |
| Submerge 201 | Submerged 202 |
| anything else | re-pick (`0x73AC30(0x10, -1)`) |

So **a one-shot emote ends after one lap** and the unit goes back to what
the chooser says - its Ready stance, its emote state, Stand.

### Stand states (0x71E1F0)

`+0x9F8` is the stand state before the change:

| Now | Before | Plays |
|---|---|---|
| 0 stand | 1 / 3 / 8 | SitGroundUp 98 / SleepUp 101 / KneelEnd 116 |
| 1 sit | 0 (or SitGroundDown playing) | SitGroundDown 96, else SitGround 97 |
| 3 sleep | 0 | SleepDown 99, else Sleep 100 |
| 4 / 5 / 6 chairs | - | SitChairLow / Med / High 102-104 |
| 8 kneel | 0 | KneelStart 114, else KneelLoop 115 |
| **2 SIT_CHAIR** | - | **nothing**: the unit stands |
| 7 dead | - | the death path |

## 3. Emotes

- **Typed emote** (0x6DD9E0, `/wave`, `DoEmote`): EmotesText -> Emotes row.
  Gated by its EmoteFlags (0x4F5410: 0x1 needs standing, 0x80 not
  swimming, 0x200 allowed asleep or dead - otherwise refused while stand
  state 3 or 7 -, 0x4000 "can't do that while moving", 0x8000 mounted
  checks). While asleep only stand-state emotes run. Then
  **`SetSheathState(0, animate, no send)` - every emote puts the weapons
  away**; then EmoteSpecProc 1 with a stand state sends
  CMSG_STANDSTATECHANGE (0x6DCB40: `/sit` 1, `/sleep` `/laydown` 3,
  `/kneel` 8, `/stand` 0); otherwise the AnimID plays at once only when
  0x724410 allows it, which needs a direction flag set - **an emote on the
  move plays immediately** (on the upper body, 0x723E30), one standing waits
  for the server's SMSG_EMOTE. Then CMSG_TEXT_EMOTE.
- **SMSG_EMOTE** (0x73AB90): the Emotes row's AnimID (0..505) if it is not
  already playing and the unit is not channelling: play it, once.
- **State emotes** (EmoteSpecProc 2: `/dance` STATE_DANCE 69, `/cower` 225)
  loop because the server keeps them as `UNIT_NPC_EMOTESTATE`, which the
  chooser plays as the idle (0x7171C0) - except for the player talking to
  the unit when the row has flag 0x2000.
- `/eat` (61), `/train` (195) are one-shots; `/work`, `/fart`, `/stink`,
  `/agree` and the rest of ONESHOT_NONE play nothing.
- NPC chat emotes are queued (0x71A260/0x73ADC0): up to four, each played
  `500 ms + the previous one's length` after it.

## 4. Combat, hits, control

Ready stances by weapon, when the stance is dropped (on `SMSG_ATTACKSTOP`,
not at the end of combat), swings, Parry/ShieldBlock/**Dodge (30)** at the
attacker's `$CPP`, StandWound/CombatWound/CombatCritical: see
combat-targeting-movement.md section 2. Stun, Knockdown and the like have
**no rule of their own**: `UNIT_FLAG_STUNNED` only stops facing changes;
the animation is the aura's SpellVisual StateKit AnimID (Stun 14 on 59 kits,
Knockdown 121 on 58), played as a state kit (0x724060 walks the unit's kit
list for a state animation). A knockback is a fall (0x724200).

## 5. Head and torso

There is **no look-at toward the target or the camera** in 12340. What
turns:

- the **whole unit** toward its target when it stands still (0x735F60):
  not moving (`flags & 0xF`), stand state 0, no emote state without flag
  0x2000, not stunned (0x40000) and not possessed (0x01000000), alive -
  facing `UNIT_FIELD_TARGET`, or `UNIT_FIELD_CHANNEL_OBJECT` while
  channelling, or the active player when the unit is the player's
  interaction target and has no target. Not only in combat;
- the **SpineLow and Head key bones** only by the strafe/turn twist of the
  body-facing spring (0x73DAB0, `body_facing.hpp`, unit-motion.md).

## 6. Wowee against this

| # | Was | Stock | Now |
|---|---|---|---|
| A1 | Fallback chain ignored Flags; nothing found played sequence 0 | 0x826350: 0x10 backwards, 0x20 held; nothing found Stand, Closed, first | `anim::resolveFallback`, `PlayMode` in `CharacterRenderer` (sampled time, events off for non-forward) |
| A2 | The fallback's id was reported as playing, so state machines saw a "finished" one-shot and loops re-asserted every frame | the requested id is reported (bs+0x90) | `currentAnimationId` = requested id; `getPlayingSequence` for MoveSpeed lookups |
| A3 | One-shots always played variation 0 (one swing animation) | every play rolls | `rollVariationStock` for loops, one-shots and the upper-body overlay |
| A4 | Variation roll proportional over the weights | chain walk, leftover keeps variation 0 | `rollVariationStock` (character and M2 renderers) |
| A5 | A looping variation with a replay range repeated 2-7 laps | replay counts only for flag-0x01 sequences | loops roll every lap; flag-0x01 one-shots play their laps |
| A6 | Cross-fade from a frozen snapshot | the outgoing sequence keeps playing; a fade under half way keeps its secondary | `blendFromSeq/Time/Speed`, `beginSequenceBlend` |
| A7 | No phase carry between moving animations | `old * newLen / oldLen % newLen` | `carriedPhaseMs` in `playAnimation` |
| A8 | Local character at rate 1 on foot | speed / MoveSpeed | `AnimationController::syncLocalMoveRate` |
| A9 | Local Walk/Run from the walk toggle, Sprint from a Sprint aura | pace from the speed: >11 Sprint, >2 x walk Run | `FrameInput::speed`, `mountGroundPace` |
| A10 | Stealth: StealthRun where the model had it, StealthWalk replaced Walkbackwards and swings | StealthWalk at any forward pace only | `CharacterAnimator::applyOverlays`, `EntitySpawner::unitMoveAnim` |
| A11 | `/eat`, `/train`, `/work` looped; `/fart`, `/stink` played EmoteTalk; `/sleep` played EmoteSleep standing | loop only for EmoteSpecProc 2; stand-state emotes change the stand state | `EmoteRegistry` (specProc), `standStateFor` in chat and `DoEmote` |
| A12 | Emotes left the weapons out | every emote puts them away | `requestWeaponsSheathed(true, "emote")` (Wowee also tells the server) |
| A13 | An emote on the move was dropped | upper body over the run | `AnimationController::playEmote` |
| A14 | Other units snapped into and out of sit/sleep/kneel | Down/Start then the loop; Up/End | `standStateTransitionAnim` in `EntityController`, retained loop for players |
| A15 | Stand state 2 sat in a chair | nothing | `standStateLoopAnim(2) = 0`, ActivityFSM |
| A16 | A creature faced its target only with UNIT_FLAG_IN_COMBAT | any idle standing unit, not stunned/possessed/at an emote | `Application` creature sync |

Open:

- Emote gating by EmoteFlags (0x4F5410) is not applied; Wowee sends the
  text emote and plays the animation whatever the stand state.
- Wowee tells the server about the emote sheath (CMSG_SETSHEATHED); stock
  keeps it local.
- Facing a channel object while channelling (0x735F60) and the flag-0x2000
  emote-state exception are not reproduced; any emote state blocks the turn.
- The swing timer of the local character is measured on variation 0; a
  longer rolled variation (Attack2HL's third, 1500 ms against 1333) is cut
  by the stance at the timer.
- Stun/knockdown come from `setStunned` (CombatFSM Stun), not from the
  aura's state kit; a stun whose kit names another animation shows Stun.
- Stand state 10 (Birth) and 9 (Submerge/Emerge) are not reproduced.
- The upper/lower split and the per-track variations follow
  combat-targeting-movement.md; the mount tier table is
  mounts-taxi-vehicles.md.

## 7. Tests

- `tests/test_anim_selection.cpp` (ctest `anim_selection`): the fallback
  chain and its play modes, the defaults, variation rolls (HumanMale Stand
  and Attack1H weights), replay laps, phase carry, MoveSpeed rate, the pace
  and stealth rules through `CharacterAnimator`, the stand-state
  transitions.
- In game (scripted, `WOWEE_DEBUG_LUA`, Wptester in Nagrand): stand, walk,
  run, backpedal, strafe, sprint at 11.9 yd/s, a slowed run that walks,
  ShuffleLeft, JumpStart; /wave, /eat (one-shot, back to Stand), /dance
  (loops, cut by moving), /sit down-loop-up, /sleep, /kneel, a wave while
  running (upper body); ReadyUnarmed, Ready1H and a swing, Ready2HL with a
  staff, Stand after the attack; a Stormwind guard's dance state, one-shot
  bow, and its run after the player. Script and shots in the session
  scratchpad (`animfull-test/`).
