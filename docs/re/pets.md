# Pets against stock 3.3.5a (build 12340)

Hunter pets, warlock demons, the death knight's ghoul, a shaman's spirit
wolves, the mage's water elemental and the vehicles and possessed units that
borrow the pet bar: what the stock client does with them, read from `Wow.exe`
12340 with Ghidra where FrameXML and AzerothCore's handlers
(`~/acore-lab/azerothcore-wotlk`) do not settle it, and what Wowee does now.

The client side of a pet is small and mostly bookkeeping. The server moves the
pet, fights with it, levels it and decides what it knows; the client keeps a
ten-slot bar, a mode word, an attack flag, a talent tree, a stable list and a
few sounds, and answers FrameXML's questions from them. Almost every bug below
was a difference in that bookkeeping.

Decompiles and scripts: `scratchpad/pets/re` (a copy of the sound-re Ghidra
project), `scratchpad/pets/tools`.

## 1. Where things live

| | address |
|---|---|
| Pet info init, opcode registration | `0x5D6D90` |
| Pet guid list (count, guids) | `0xC234FC` / `0xC23500` (SMSG_PET_GUIDS `0x5D6550` fills it) |
| Bar, ten packed slots | `0xC23508` |
| Mode word: react low byte, command next, flags above | `0xC234C8` |
| Attack flag (IsPetAttackActive) | `0xC234DC` |
| Expiry time of a temporary pet | `0xC234CC` |
| Pet spellbook | `0xC23538`, count `0xBE8DA4` |
| Pet talent family | `0xC21000` |
| Stable list, current pet, slots, selection | `0xC0F4E0`, `0xC0F480`, `0xC0F6CC`, `0xC0F6D0` |
| Cursor kind / pet action / stable slot | `0xBD0748` (4 pet action, 10 stable pet) / `0xBD07E4` / `0xBD07EC` |

A slot is `spell | type << 24`. The type byte is AzerothCore's ActiveStates:
`0x01` passive, `0x81` castable (autocast off), `0xC1` autocast on, `0x06` a
stance, `0x07` a command. The client switches on `type & 0x3F`: 1 (and 8-17,
the kinds a vehicle bar uses) is a spell, 6 a stance, 7 a command. Bit `0x80`
says a spell can autocast, `0x40` that it does.

## 2. Packets

| packet | handler | what the client does |
|---|---|---|
| SMSG_PET_SPELLS 0x179 | `0x5D6B90` | clears the bar; guid; if non-zero: family u16, duration u32, mode u32, ten slots, spell count u8 + spells u32, cooldown count u8 + (spell u32, category u16, cooldown u32, category cooldown u32, top bit = infinite). Then `0x5D6900`: guid changed -> attack flag from the pet's in-combat flag, `UNIT_PET("player")`, `PET_UI_CLOSE` when the new pet is a hunter's; talent family `0x5C9DF0`; `PET_BAR_UPDATE` (`0x5D4AD0`, which also hands a vehicle's bar to the bonus bar); `RAISED_AS_GHOUL` when the player is dead and a pet appeared. A guid of zero clears the pet. |
| SMSG_PET_MODE 0x17A | `0x5D3630` | guid, mode u32: replaces the mode word for that pet, `PET_BAR_UPDATE`. |
| SMSG_PET_ACTION_FEEDBACK 0x2C6 | `0x5D4DA0` | 1 ERR_PET_SPELL_DEAD, 2 ERR_NO_ATTACK_TARGET (or ERR_GENERIC_NO_VALID_TARGETS), 3 ERR_INVALID_ATTACK_TARGET, 4 ERR_PET_SPELL_NOPATH. |
| SMSG_PET_BROKEN 0x2AF | `0x5D3120` | ERR_PET_BROKEN only. |
| SMSG_PET_RENAMEABLE 0x475 | `0x5D3140` | `PET_RENAMEABLE`. |
| SMSG_PET_LEARNED_SPELL / UNLEARNED 0x499/0x49A | `0x5D4C30` | ERR_PET_LEARN_SPELL_S (ERR_PET_LEARN_ABILITY_S for Attributes 0x10) / ERR_PET_SPELL_UNLEARNED_S, "%s (%s)" with the rank. |
| SMSG_PET_GUIDS 0x4AA | `0x5D6550` | the list of controlled units. |
| SMSG_PET_UPDATE_COMBO_POINTS 0x492 | `0x5D36A0` | `PET_BAR_UPDATE_USABLE`. |
| SMSG_PET_NAME_INVALID 0x178 | `0x6E2E90` | result u32 -> `0x5218C0` (2 NO_NAME, 3 TOO_SHORT, 4 TOO_LONG, 6 MIXED_LANGUAGES, 7 PROFANE, 8 RESERVED, 11 THREE_CONSECUTIVE, 12 INVALID_SPACE, 13 CONSECUTIVE_SPACES, 14-16 Russian/declension, else ERR_NAME_INVALID); name; declension flag u8, five declined names and `PET_FORCE_NAME_DECLENSION` when set. |
| SMSG_PET_ACTION_SOUND 0x324 | `0x716B90` | guid, action u32: 0 -> the unit's PetOrder sound, 1 -> PetAttack (`0x7474B0` kinds 1, 2). |
| SMSG_PET_DISMISS_SOUND 0x325 | `0x716C00` | CreatureModelData id, position: that model's CreatureSoundData PetDismiss sound there. |
| SMSG_TALENTS_INFO 0x4C0 type 1 | `0x6CD770` -> `0x5C9F10` | unspent u32, count u8, (talent u32, rank u8 from zero); recount, `PET_TALENT_UPDATE`. |
| MSG_LIST_STABLED_PETS 0x26F | `0x5A11A0` | master guid; count u8; slots u8; per pet: number, entry, level u32, name, flag u8. A flag without 0x02 is the pet that is out. Resets the selection, `PET_STABLE_SHOW`. |
| SMSG_STABLE_RESULT 0x273 | `0x5A17F0` | 1 ERR_NOT_ENOUGH_MONEY; 8, 9, 10 ask for the list again (10 also adds a slot); 11 `PET_STABLE_UPDATE`; 12 the exotic tame error. |

Sent:

| packet | from |
|---|---|
| CMSG_PET_ACTION (pet, slot value, target) | commands, stances, bar spells (`0x5D4210`) |
| CMSG_PET_SET_ACTION (pet, then one or two (slot, value) pairs) | a drop on the bar, TogglePetAutocast (`0x5D6020`, `0x5D5D20`) |
| CMSG_PET_SPELL_AUTOCAST (pet, spell, u8) | the spellbook's autocast (`0x5D5E10`) |
| CMSG_PET_STOP_ATTACK (pet) | PetStopAttack, only while attacking (`0x5D4650`) |
| CMSG_PET_ABANDON (pet) | PetAbandon (`0x5D4900`), then MSG_LIST_STABLED_PETS at a stable |
| CMSG_PET_RENAME (pet, name, declined u8 [+5 names]) | PetRename (`0x5D4A00`) |
| CMSG_PET_LEARN_TALENT (pet, talent, rank from zero) | LearnTalent(..., true) (`0x5C80E0`) |
| CMSG_LEARN_PREVIEW_TALENTS_PET (pet, count u32, (talent, rank)) | LearnPreviewTalents(true) (`0x5C6A10`) |
| CMSG_STABLE_PET (master) | StablePet, a drag of the current pet onto an empty slot |
| CMSG_UNSTABLE_PET (master, number) | UnstablePet, a drag of a stabled pet onto the current slot with no pet out |
| CMSG_STABLE_SWAP_PET (master, number) | a drag between the current slot and an occupied one |
| CMSG_BUY_STABLE_SLOT (master) | BuyStableSlot, below four slots and with StableSlotPrices.dbc's price |

## 3. The bar's API

`GetPetActionInfo(i)` (`0x5D4EC0`) answers seven values:

| slot | name | subtext | texture | isToken | isActive | autoCastAllowed | autoCastEnabled |
|---|---|---|---|---|---|---|---|
| spell | spell name | rank | icon | nil | nil | `0x80` bit | `0x40` bit |
| stance | `PET_MODE_PASSIVE`/`DEFENSIVE`/`AGGRESSIVE` | nil | `PET_<word>_TEXTURE` | true | stance == mode's react | nil | nil |
| command | `PET_ACTION_WAIT`/`FOLLOW`/`ATTACK`/`DISMISS` | nil | `PET_<word>_TEXTURE` | true | Follow/Wait: command == mode's; Attack: the attack flag | nil | nil |

Booleans are true or nil, never false. The command words come from the table
at `0xA1A8C0`.

- **Commands and stances are predicted.** PetWait/PetFollow set the command
  byte at once (`0x5D3020`), the stances the react byte (`0x5D2FF0`), and both
  fire `PET_BAR_UPDATE` before CMSG_PET_ACTION goes. PetAttack sets the attack
  flag only for a target the pet can attack (`0x72BDB0`) and sends nothing for
  one it cannot.
- **The attack flag** (`IsPetAttackActive`, `0x5D3160`) follows the pet's
  `UNIT_FLAG_PET_IN_COMBAT` (0x800): its field callback (`0x73C330`, for a unit
  charmed or summoned by the player) fires `PET_ATTACK_START` / `PET_ATTACK_STOP`
  (`0x5D30A0`). PetStopAttack clears it.
- **Usable** (`0x5D3FD0`): commands while the pet takes orders (`0x5D3E80`:
  the player is not charmed, controls nothing else, the pet is the player's and
  alive); spells also while the pet is not stunned, confused or fleeing (unless
  AttributesEx5 allows it) and can pay. True or nil.
- **Cooldown** (`0x5D5280`): spell kinds only, from the pet's cooldowns.
- **Autocast**: a right click (`TogglePetAutocast`, `0x5D5D20`) flips the
  slot's 0x40 bit, sends the slot back with CMSG_PET_SET_ACTION and redraws;
  only an autocastable slot changes. The spellbook's toggle (`0x5D5E10`)
  sends CMSG_PET_SPELL_AUTOCAST and sets the bit on every slot with that spell.
- **Dragging** (`0x5D65C0`): PickupPetAction puts the slot on the cursor
  (`0x520A80`: cursor kind 4, `PET_BAR_SHOWGRID`, the grab sound); a spell (kind
  1) also leaves the bar (`0x5D64A0`), a command or stance is copied. Holding a
  pet action, PickupPetAction, CastPetAction and TogglePetAutocast on a slot
  are the drop (`0x5D6020`): the same action is nothing; one already elsewhere
  on the bar moves there and that slot takes what the target held; a command or
  stance in the target moves to the first empty slot, and with none the drop is
  refused; otherwise the target's old action goes onto the cursor. A passive
  spell (Attributes 0x40) never goes on the bar. ClearCursor with a pet action
  fires `PET_BAR_HIDEGRID` instead of `ACTIONBAR_HIDEGRID` (`0x519280`). A
  pet spell picked up from the spellbook is a pet action too. GetCursorInfo has
  no answer for either kind.
- **PetDismiss** (`0x5D46F0`) sends COMMAND_DISMISS (`0x07000003`) and fires
  `PET_DISMISS_START` with 10000.
- **PetHasActionBar** (`0x5D3720`): a pet, the bonus bar not in use, and the
  pet's bar (`0x5D35B0`).
- **GetPetTimeRemaining** (`0x5D3180`): milliseconds to the duration
  SMSG_PET_SPELLS gave, nil for a permanent pet.

## 4. The pet frame

- **HasPetUI** (`0x5D3960`): a pet unit that is not a player and has a pet
  number -> true and *isHunterPet*; otherwise nil, nil.
- **isHunterPet** (`0x71B630`): the unit has a pet number and its creator
  (UNIT_FIELD_CREATEDBY) is a hunter. It gates the rest of this list.
- **GetPetHappiness** (`0x5D3B00`, `0x71F390`/`0x71F400`): happiness (power 5)
  against PetPersonality.dbc row 1: thresholds 0, 333000, 666000, damage 75%,
  100%, 125%. Returns the face 1-3 and the percent; nil, 100 for anyone else's
  pet. (3.3.5 has no loyalty.)
- **GetPetExperience** (`0x5D3A20`): the pet's two experience fields; 0, 0
  for a pet that is not a hunter's.
- **GetPetIcon / GetPetFoodTypes / GetPetTalentTree** (`0x5D3CE0`, `0x5D3BD0`,
  `0x5D3D80`): CreatureFamily.dbc of the pet's template - IconFile (col 27),
  the ItemPetFood names whose bit (id - 1) is in PetFoodMask (col 7), and the
  TalentTab whose PetTalentMask has the bit of PetTalentType (col 8).
- **PetCanBeRenamed / PetCanBeAbandoned** (`0x5D38C0` / `0x5D3780`): the pet's
  creator is the player and UNIT_FIELD_BYTES_2 byte 2 has 0x01 / 0x02.
- **PetRename** (`0x5D5670`): ERR_NO_PET, ERR_NOTYOURPET, ERR_PET_NOT_RENAMEABLE,
  ERR_NAME_NO_NAME locally; the server judges the name.
- **UnitCreatureFamily** (`0x611820`): CreatureFamily's name.
- Field callbacks on units (`0x741D00`): UNIT_FIELD_PETNUMBER on the pet ->
  `PET_UI_UPDATE` (`0x72CFF0`); UNIT_FIELD_PET_NAME_TIMESTAMP on a unit the
  player owns -> `LOCALPLAYER_PET_RENAMED` (`0x72CF70`); UNIT_FIELD_AURASTATE
  on the player or the target -> `PET_BAR_UPDATE_USABLE` (`0x716810`).

## 5. Pet talents

- The tree (`0x5C64D0`) is the TalentTab whose PetTalentMask (col 21) holds
  `1 << CreatureFamily.PetTalentType` - Ferocity 410, Tenacity 409, Cunning 411
  - for the family SMSG_PET_SPELLS sent. A family with PetTalentType -1
  (every demon, the ghoul) has none.
- `GetNumTalentGroups(false, true)` is 1 for a pet with a tree and 0 otherwise
  (`0x5C5DF0`) - which shows or hides the talent frame's pet tab;
  `GetActiveTalentGroup(false, true)` is 1.
- `GetNumTalents(tab, false, true)` counts every talent of the tab. A talent
  whose CategoryMask (Talent cols 21-22) is non-zero and lacks the family's
  CategoryEnumID bit (CreatureFamily col 9) answers the empty row
  (nil, nil, 1, 1, 0, 0, 0, nil, 0, nil) and draws nothing (`0x5C5A20`) -
  that is how Dash and Dive share a square.
- `GetTalentInfo(..., isPet)`'s eighth and tenth returns are the *required
  spell* checks, which a pet talent does not have, so both are true; the
  arrows are GetTalentPrereqs'. isExceptional is Talent.dbc Flags bit 1.
- `GetUnspentTalentPoints(false, true)` is the packet's unspent count.
- `AddPreviewTalentPoints(tab, i, n, true)` stages, and fires
  `PREVIEW_PET_TALENT_POINTS_CHANGED` with (talent, tab, group, change)
  (`0x5C9590`); `ResetGroupPreviewTalentPoints(true)` the same with the
  negative total (`0x5C7200`); `LearnPreviewTalents(true)` sends
  CMSG_LEARN_PREVIEW_TALENTS_PET; `LearnTalent(tab, i, true)` sends
  CMSG_PET_LEARN_TALENT for the next rank, counted from zero.

## 6. The stable

- `GetStablePetInfo(i)` (`0x5A1330`): slot 0 the pet that is out, 1-4 the
  stabled ones. icon (the family's), name, level, family name and - only for a
  family with a tree - the tree's name. The family comes from the pet's
  creature template; while that is not cached the slot answers nil, nil, 0, a
  query goes out and its answer fires `PET_STABLE_UPDATE` (`0x5A0E80`).
- `GetSelectedStablePet` (`0x5A1060`): 0 the current pet, 1-4 a stabled one,
  -1 none; every list resets it to none.
- Moving pets is a drag: `PickupStablePet(i)` (`0x5A0FE0` -> `0x520C60`, cursor
  kind 10) and `ClickStablePet(j)` (`0x5A1CA0`) on another slot. Without a
  carried pet ClickStablePet selects and answers true.
- `GetNextStableSlotCost` is StableSlotPrices.dbc's row for the next slot (5s,
  50s, 50g, 150g).
- `SetPetStablePaperdoll` (`0x5A1530`): the pet that is out from its own unit
  when it is a hunter's pet in view, otherwise the stable entry's template.

## 7. Motion, animation, sounds and models

- **Following and the run-in are the server's.** The client has no pet
  movement of its own: the pet is a unit like any other, moved by
  SMSG_MONSTER_MOVE splines (unit-motion.md). AzerothCore's follow is
  `PET_FOLLOW_DIST` 2.0 yards at `PET_FOLLOW_ANGLE` π/2 (to the owner's left),
  re-pathed as the owner moves; attacking is its chase generator. The
  animation is the unit's (walk/run by speed, combat stances by the attack
  target), as for any creature.
- **Pet voice** (`0x7474B0`): kind 1 PetOrder (CreatureSoundData col 29), 2
  PetAttack (col 28), 0 Aggro (col 10), 4 Death; for a unit with a pet number
  only while `Sound_EnablePetSounds` is on. The unit's CreatureSoundData is the
  display's SoundID, else the model's.
- **Demon models** are ordinary creature displays (Imp 4449, Voidwalker 1132,
  Succubus 4162, Felhunter 850, Felguard 14255 and their variations); nothing
  pet-specific.

## 8. What was wrong, and what changed

| # | Wowee | stock | fixed in |
|---|---|---|---|
| 1 | GetPetActionInfo: commands never active; DISMISS named `PET_DISMISS`; tokens' subtext ""; every spell `autoCastAllowed` true; autocast from a side set; false instead of nil | §3 table | `lua_action_api.cpp` |
| 2 | SMSG_PET_MODE read the command from the low byte (react and command swapped) and raised PET_ATTACK_START/STOP from the command | low byte react; attack from the pet's in-combat flag | `combat_handler.cpp`, `entity_controller.cpp` |
| 3 | IsPetAttackActive was "command == attack" | the attack flag | `game_handler_pets.cpp` |
| 4 | Commands and stances waited for the server to redraw (and AzerothCore never sends SMSG_PET_MODE, so the checked button never moved) | predicted, PET_BAR_UPDATE | `game_handler_pets.cpp` |
| 5 | PetAttack needed a target and sent for anything | target or none; nothing for one that cannot be attacked | |
| 6 | PetStopAttack, GetPetActionsUsable missing | §3 | |
| 7 | TogglePetAutocast sent CMSG_PET_SPELL_AUTOCAST for any slot | CMSG_PET_SET_ACTION with the slot, autocastable slots only | |
| 8 | PickupPetAction a no-op; no pet action cursor; no PET_BAR_SHOWGRID/HIDEGRID; a pet spell from the book was a player spell on the cursor | §3 dragging | `lua_action_api.cpp`, `game_handler_pets.cpp` |
| 9 | SMSG_PET_SPELLS: cooldowns not read; duration dropped; UNIT_PET/PET_UI_UPDATE on every packet; no PET_UI_CLOSE, RAISED_AS_GHOUL | §2 | `spell_handler.cpp` |
| 10 | SMSG_PET_BROKEN cleared the pet, so the SMSG_PET_SPELLS that follows looked like no change | error only | `combat_handler.cpp` |
| 11 | HasPetUI's second return always nil (no happiness face for a hunter); GetPetHappiness thirds of the maximum and no second value for other pets; GetPetExperience for any pet | §4 | |
| 12 | GetPetIcon a constant; GetPetFoodTypes, GetPetTalentTree nil; UnitCreatureFamily a hand table off by one from Raptor | CreatureFamily.dbc | `game_handler_pets.cpp`, `lua_quest_api.cpp`, `lua_unit_api.cpp` |
| 13 | Pet talents: none | §5 | `spell_handler.cpp`, `lua_quest_api.cpp`, `lua_engine.cpp` (tooltip) |
| 14 | Talent.dbc's prerequisites read from columns 9 and 12 (rank spells 6-9) - no talent of any tree had a prerequisite: no arrows, deep talents stageable early | columns 13 and 16 | `dbc_layouts.json`, `spell_handler.cpp` |
| 15 | Talent isExceptional always false | Flags bit 1 | |
| 16 | Stable: family, tree and diet blank; icon a constant; ClickStablePet only selected; PickupStablePet a no-op - pets could not be moved; StablePet/UnstablePet missing; CMSG_STABLE_PET carried an extra slot byte; slot price 0; selection not reset; list refreshed on result 1-8 and not 9-10 | §6 | `lua_quest_api.cpp`, `spell_handler.cpp`, `game_handler_packets.cpp` |
| 17 | SMSG_PET_ACTION_SOUND / DISMISS_SOUND dropped | §7 | `game_handler_packets.cpp`, `audio_callback_handler.cpp`, `sound_data.cpp` |
| 18 | SMSG_PET_NAME_INVALID an invented sentence, twice | the ERR_NAME_* line; PET_FORCE_NAME_DECLENSION | |
| 19 | PetCanBeRenamed/Abandoned without the creator check; PetRename silent; names over 12 dropped before sending | §4 | |
| 20 | PET_UI_UPDATE / LOCALPLAYER_PET_RENAMED / PET_BAR_UPDATE_USABLE not raised from the fields | §4 | `entity_controller.cpp` |
| 21 | A Lua bootstrap defined `GetPetActionsUsable` as `return false`, over the binding | §3 | `lua_engine.cpp` |
| 22 | PetCanBeAbandoned answered false rather than nil; HasPetSpells, PetAbandon at a stable did not re-list | §4, §2 | |

The pet fields (UNIT_FIELD_CREATEDBY, PETNUMBER, PET_NAME_TIMESTAMP) are in
`Data/expansions/wotlk/update_fields.json`, and derived beside SUMMONEDBY and
PETEXPERIENCE when an older data directory's copy does not name them. The
Talent columns are in `dbc_layouts.json` and also taken from the file itself
when it has the 3.3.5 width (23 columns; TalentTab 24), so a data directory
extracted before this change reads them right. Pure rules (slot kinds, mode
word, isActive, happiness faces, the bar drop, stable drags, the family
filter) are in `include/game/pet_action.hpp` with `tests/test_pet_action.cpp`.

## 9. AzerothCore differences that show

- AzerothCore puts `UNIT_FLAG_PET_IN_COMBAT` on the **owner** while a minion
  fights (`Unit::UpdatePetCombatState`) and never on the pet, where the stock
  client looks for it. So against this server PET_ATTACK_START / STOP never
  fire and IsPetAttackActive stays at what PetAttack predicted until
  PetStopAttack or a new pet - the stock client would behave the same here.
- AzerothCore never sends SMSG_PET_MODE; the predicted mode is all the client
  has after a click, as in stock.

## 10. In game

Lab server, Wptester (GM, 80), logs and screenshots under
`scratchpad/pets/logs` and `scratchpad/pets/shots`.

- **Hunter** (`hunter.lua`, a dwarf for the run - a human cannot be a hunter
  and the server will not log one in): `.pet create` on a level 79 tundra wolf.
  HasPetUI true, true; happiness 1, 75 (the face drawn on the pet frame);
  icon, diet "Meat", tree "Ferocity", UnitCreatureFamily "Wolf". The bar: Attack,
  Follow (checked), Wait, Bite, Growl, Furious Howl, Cower, Aggressive,
  Defensive, Passive (checked); stances and Wait/Follow checked at once on the
  click. Autocast on Bite on and off through CMSG_PET_SET_ACTION. A drag of Bite
  onto Growl swapped them with PET_BAR_SHOWGRID / HIDEGRID. Pet talents:
  one tab "Ferocity", 21 talents (Dive and the other family-only squares
  empty), 16 points; a preview point on Cobra Reflexes
  (PREVIEW_PET_TALENT_POINTS_CHANGED 1, 1, 1, 1) learned by
  LearnPreviewTalents(true) -> PET_TALENT_UPDATE, rank 1, 15 points; Dash by
  LearnTalent(1, 2, true) -> 14 points, link `talent:2109:0`. Rename to
  "Wolfie" -> LOCALPLAYER_PET_RENAMED, the name on the frame. PetAttack on a
  neutral wolf set the attack flag (see §9 for the event). Stable: World
  Stable Master, PET_STABLE_SHOW, the current pet with family icon, "Wolf",
  "Ferocity" and its model; the slot bought for 5s; a drag of the current pet
  onto slot 1 stabled it (UNIT_PET, slot 1 filled), and back unstabled it.
  PetDismiss -> PET_DISMISS_START 10000, UNIT_PET.
- **Warlock** (`warlock.lua`): imp, voidwalker, succubus, felhunter and felguard
  summoned; HasPetUI true, nil; GetPetHappiness nil, 100; no talent groups;
  HasPetSpells "DEMON"; each demon's bar; Firebolt autocast toggled; the
  voidwalker and succubus models in view (screenshots).
- **Mage**: the water elemental's bar (Waterbolt autocast on, Freeze),
  HasPetUI nil (no pet number), PetHasActionBar true.

## 11. Gaps

- The talent frame's pet tab and the pet paperdoll were not caught on screen
  (the script hid each frame in the same step it asked for the screenshot);
  their API answers were checked.
- The death knight's ghoul and a shaman's spirit wolves were not run; they take
  the same paths (SMSG_PET_SPELLS, SMSG_PET_GUIDS).
- Tame Beast was interrupted in the run (the wolf hit back), so the
  first-tame naming dialog (PET_RENAMEABLE) was not seen; Feed Pet was not run
  with a pet out.
- A pet spell that needs a target the player has not chosen goes out with the
  current target or none; the stock client's targeting cursor for it
  (`0x805D70`) is not modelled.
- `Sound_EnablePetSounds` is not read; pet sounds always play.
- The pet frame's AttackModeTexture and the attack button's flash depend on
  PET_ATTACK_START, which AzerothCore never causes (§9).
- Classic and TBC Talent layouts still name columns 9 and 12 for the
  prerequisites; not checked against those files.

