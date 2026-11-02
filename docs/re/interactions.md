# Interacting with the world (stock 3.3.5a, build 12340)

What the stock client does when the player right-clicks an NPC, a game object,
a corpse or another player, read from `Wow.exe` 12340 with Ghidra. Every rule
names the function it comes from. Unit descriptor offsets are relative to the
unit block (`CGUnit_C +0xD0`): `+0x48` health, `+0xD4` `UNIT_FIELD_FLAGS`,
`+0xD8` `UNIT_FIELD_FLAGS_2`, `+0xF0` `UNIT_FIELD_COMBATREACH`, `+0x110`
`UNIT_FIELD_BYTES_1`, `+0x124` `UNIT_DYNAMIC_FLAGS`, `+0x130`
`UNIT_NPC_FLAGS`, `+0x134` `UNIT_NPC_EMOTESTATE`, `+0x1D0` `UNIT_FIELD_BYTES_2`
(byte 3 is the shapeshift form). Error names are the `GameError` table at
0xAC8AF0 (20-byte rows); their text is `GlobalStrings.lua`.

The pure parts of this live in `include/game/interaction_rules.hpp`
(tested by `tests/test_interaction_rules.cpp`).

## 1. The click

- **The world frame** (0x4F7880): a right-click acts on the **mouseover
  object only** (0x715C30). With nothing under the pointer it goes to the
  terrain path (click-to-move, 0x5278C0); the current target is never a
  fallback. `InteractUnit("unit")` (0x527F00) reaches the same function.
- **0x5277B0**: `CGGameUI::Target(guid)` (0x524BF0), then the object's
  `OnRightClick` (vtable +0xB0): `CGUnit_C` and `CGPlayer_C` 0x731260,
  `CGGameObject_C` 0x711140.
- **CVars** (registered at 0x51DB..., pointers 0xBD08F0..): `autoInteract`
  (0xBD08F4, click-to-move to the target; default 0 on PC), `autoStand`
  (0xBD08F8, 1), `autoDismount` (0xBD08FC, 1), `autoDismountFlying`
  (0xBD0900, 0), `autoUnshift` (0xBD0904, 1), `autoLootDefault` (0xBD0914,
  0), `lootUnderMouse` (0xBD0910, 1).
- **Auto-loot for a click** (0x513700): `autoLootDefault`, flipped while the
  `AUTOLOOTTOGGLE` binding (Shift) is down. It is latched on the player at
  the click (`+0x18E8`, 0x6CDE50), not read when the loot arrives; if a loot
  is already open and the latch turns on, it is looted at once (0x588F90).

## 2. Units: `CGUnit_C::OnRightClick` (0x731260)

In order:

1. No local player, or the player is dead (health < 1) and not a ghost
   (0x6DD060): nothing.
2. **Loot** - the unit is dead, not `UNIT_DYNFLAG_DEAD`-only, and lootable
   (0x717B60: health < 1, `UNIT_DYNFLAG_LOOTABLE`, its loot timer passed):
   - sitting (stand state, vtable +0x138, not 0): with `autoStand` stand up
     (0x6DCB40(0), `CMSG_STANDSTATECHANGE` 0x101), else
     `ERR_LOOT_NOTSTANDING`;
   - latch auto-loot (0x6CDE50), then **0x6DAD90** (below).
3. **Skin** - dead and `UNIT_FLAG_SKINNABLE` (0x04000000): the spell for the
   corpse (0x53BCE0: Skinning, or Herb Gathering / Mining for creatures with
   `CREATURE_TYPEFLAGS` 0x100 / 0x200 / 0x8000, 0x715E50). A player corpse
   (insignia) goes straight to the cast. A creature checks the skill first:
   required `level < 11 ? 1 : level < 20 ? 10*(level-10) : 5*level`; short of
   it, `ERR_USE_LOCKED_WITH_SPELL_KNOWN_SI`. Then 0x6D6F40: within the spell's
   range (or with `autoInteract` off) cast it at the corpse (0x80DA40); with
   `autoInteract` on and out of range, click-to-move action 9. Auto-loot is
   latched for the loot the skinning opens.
4. **NPC** - 0x729530 says it may be spoken to (NPC flags set, not
   `UNIT_FLAG_NOT_SELECTABLE`, reaction > Unfriendly both ways or
   `UNIT_FLAG2_ALLOW_ENEMY_INTERACT`): **0x6DDBB0** (section 3).
5. **Attack** - `CanAttack` (0x729A70) and 0x72BDB0: start the attack
   (0x6E4950; see `combat-targeting-movement.md`). With `autoInteract` on, the
   player is also turned or walked to it (0x72EE20 -> 0x72B660, 0x727400).
   Without it the player is **not** turned.
6. Otherwise, a friendly unit that is none of these: 0x72A680 plays one of
   the creature's "clicked" sounds in turn (0x746900 over 0x4C6A40). Nothing
   is sent.

A dead unit that is neither lootable nor skinnable gets nothing - it is never
attacked.

## 3. NPCs: `CGPlayer_C::OnNpcInteract` (0x6DDBB0)

- **Shapeshift**: in a form (0x721CA0: byte 3 of `UNIT_FIELD_BYTES_2`, and
  `SpellShapeshiftForm` flag 1 not set) whose form lacks flag 8: with
  `autoUnshift` (0x71B130) cancel it (0x726CE0, cancels the
  `SPELL_AURA_MOD_SHAPESHIFT` aura), else `ERR_CANT_INTERACT_SHAPESHIFTED`.
- **No dismount, no stand-up, no stop, no facing.** A mounted player talks
  from the saddle.
- **Reach**: `|player - npc|^2 <= (npc combat reach + 4.0)^2`, 3D, centre to
  centre (`+0xF0` plus the float at 0x9E8D2C). The player's own reach plays no
  part. Out of reach: with `autoInteract` click-to-move action 5
  (0x72B4A0), else `ERR_TOO_FAR_TO_INTERACT` and nothing is sent. The server
  (AzerothCore `GetNPCIfCanInteractWith`) allows 5.5 + both combat reaches.
- **Already the interact target** (0xBD07A8 == guid): nothing.
- **One packet**, by the first match, each followed by 0x73AD70(0):

| Check | Sends | Function |
|---|---|---|
| `GOSSIP` 0x1 | `CMSG_GOSSIP_HELLO` 0x17B | 0x6D1E20 |
| quest status not NONE/UNAVAILABLE (0x6D1DE0) | `CMSG_QUESTGIVER_HELLO` 0x184 | 0x6D1EA0 |
| `FLIGHTMASTER` 0x2000 | `CMSG_TAXIQUERYAVAILABLENODES` 0x1AC | 0x6D6320 |
| `VENDOR` 0x80 | `CMSG_LIST_INVENTORY` 0x19E | 0x6DAF60 |
| `TRAINER` 0x10 | `CMSG_TRAINER_LIST` 0x1B0 | 0x6D1F20 |
| `SPIRITHEALER` 0x4000 | `CONFIRM_XP_LOSS` popup; on accept `CMSG_SPIRIT_HEALER_ACTIVATE` 0x21C | 0x6DB010 |
| `SPIRITGUIDE` 0x8000 | area spirit healer query | 0x6DB180 |
| `INNKEEPER` 0x10000 | `CONFIRM_BINDER` popup; on accept `CMSG_BINDER_ACTIVATE` 0x1B5 | 0x6D2120 |
| `BANKER` 0x20000 | `CMSG_BANKER_ACTIVATE` 0x1B7 | 0x6D2340 |
| `PETITIONER` 0x40000 (or with `TABARDDESIGNER`) | `CMSG_PETITION_SHOWLIST` 0x1BB | 0x6D2480 |
| `TABARDDESIGNER` 0x80000 | `MSG_TABARDVENDOR_ACTIVATE` 0x1F2 | 0x6D2500 |
| `BATTLEMASTER` 0x100000 | `CMSG_BATTLEMASTER_HELLO` 0x2D7 | 0x6D25C0 |
| `AUCTIONEER` 0x200000 | `MSG_AUCTION_HELLO` 0x255 | 0x6D2640 |
| `STABLEMASTER` 0x400000 | `MSG_LIST_STABLED_PETS` 0x26F | 0x6D26C0 |
| `GUILD_BANKER` 0x800000 | `CMSG_GUILD_BANKER_ACTIVATE` 0x3E6 | 0x6D23C0 |
| `SPELLCLICK` 0x1000000 (0x71C570) | `CMSG_SPELLCLICK` 0x3F8 | 0x6D2740 |
| a vehicle the player may board (0x723050) | `CMSG_PLAYER_VEHICLE_ENTER` 0x4A8 | 0x6D27C0 |
| `MAILBOX` 0x4000000 | opens mail locally (`MAIL_SHOW`) | 0x56DA60 |

  The confirming ones (innkeeper, spirit healer) also re-check the reach
  before sending the activate.
- 0x73AD70(0): an emote animation on the player from the Emotes row latched
  at 0xCA1224 (0x7165D0 picks the rows by Emotes flags), unless swimming or
  asleep. It is cosmetic and was not identified further.
- The quest status with `UNAVAILABLE` (1) triggers tutorial 0x2A (0x530840).

## 4. The interact target and walking away

The client keeps one **interact target** (0xBD07A8):

- **Set** (0x51FCE0) when a window opens for a unit or object: gossip
  (0x58A550 via 0x6D6050), quest (0x58CA70), vendor (0x584600), taxi
  (0x590BA0), trainer (0x5940E0), bank (0x57B8D0), guild bank (0x5A4270),
  registrar (0x599E90), tabard (0x598EC0), mail (0x56DA60), auction
  (0x59F150), stable (0x5A0F10), petition (0x5A2880), battlefield list
  (0x5496D0), trade (0x5870A0), item text (0x58A1A0). It stores the limit:
  - a creature: `(combat reach + 4)^2`;
  - another player: 123.45678 (11.11 yards, 0xAC8A68);
  - a game object: its type handler's own check (below).
  A **new** target re-picks the NPC's idle (0x73AC30) and says its
  **greeting** (0x720860(1): `CreatureDisplayInfo.NPCSoundID` ->
  `NPCSounds` greeting, at the NPC, unless it is still speaking).
- **Polled every frame** (0x51FDE0, from CGWorldFrame 0x4FA040). It is let go
  when the object is gone, or `+0xBC & 0x10000`, or:
  - a unit: out of the limit (squared 3D distance from the active player,
    0x4F61D0), or no longer one to talk to (0x729530 / 0x729B30);
  - an item (quest from a bag): never;
  - a game object: hostile (0x71F8B0) or out of its use box (type handler
    +0x20, 0x70FA70).
- **Let go** (0x518D50 -> 0x512E60): closes **every** window bound to that
  guid (the list above, by the guid each frame stored) and says the NPC's
  **farewell** (0x720860(0)) - but not when it is replaced by another target.
  The player dying, releasing or being reset also lets it go (0x6DC0F0,
  0x729220).

**Loot is not part of it.** The loot window closes when the player starts to
move, turn, pitch, rise or sink: every movement start (0x72E430...0x72EB80)
calls 0x523640, which sends `CMSG_LOOT_RELEASE` 0x15F; a loot response that
arrives while moving (flags & 0xC000FF) is closed on arrival (0x6D53B0).

**NPC facing** (0x735F60, every unit's facing update): a unit that is not the
active player, stands still (no direction flag), stands (stand state 0), is
not stunned (0x40000), possessed (0x01000000) or `FLAGS_2` 0x8000, and whose
`NPC_EMOTESTATE` is zero or has Emotes flag 0x2000, turns to its
`UNIT_FIELD_TARGET` (or channel object); with neither, **it turns to the
active player while it is the interact target**. Client side only. The same
0x2000 emote is suppressed while it is the interact target (0x7171C0), so an
NPC at an anvil stops working to talk if its emote allows it.

**No sheathing.** Nothing on the interact path changes the player's sheath.
Only the Loot animation's `WeaponFlags` 4 puts the weapons away while it plays
(0x738180).

## 5. Loot: 0x6DAD90, `SMSG_LOOT_RESPONSE` 0x6D53B0

- **Can loot** (0x6D5A60): within reach - a unit `max(reachA + reachB + 4/3,
  5)` (0x4F5F40, 4/3 at 0x9F987C, 5.0 at 0x9EBF34), anything else 5.0 - not
  already looting (`+0x18E0` zero), not dead (vtable +0x128), not falling
  (0x1000), not stunned, and not in a shapeshift form with flag 0x100. With
  `autoInteract` the reach is not checked (click-to-move action 6 instead).
  Refused **silently**: the cursor already says Unable.
- **Moving** (movement flags & 0xC010FF: direction, turn, pitch, falling,
  ascending, descending): nothing.
- **Mounted**: with `autoDismount` (0x71B0C0) dismount (0x7412E0), else
  `ERR_NOT_WHILE_MOUNTED`.
- In a vehicle: 0x74CCE0.
- Release any open loot (0x523640), send **`CMSG_LOOT` 0x15D**, remember the
  guid (`+0x18E0`), and play **Loot (50)** at once (0x7385C0(0x32)).
- The loot animation while it is open (0x724280, in the base-animation
  chooser): keep Loot (50) or LootHold (188) playing; replay Loot if
  something else took over; when the loot is gone and LootHold is playing,
  LootUp (189). **No 3.3.5a character model has 188 or 189** (they fall back
  to Loot through AnimationData), so in practice: Loot, held while the window
  is open, then the idle. KneelLoop / KneelEnd are never used.
- `SMSG_LOOT_RESPONSE`: a failure byte maps to `ERR_LOOT_*` (0x8A..0x8F,
  0x1BC, 0x1E9, 0x1FC..0x1FE, 0x1C1); `ERR_LOOT_DIDNT_KILL` etc. The window
  opens (0x5264F0 -> 0x5896B0) and targets the corpse; if the player is moving
  it closes again at once.
- **Auto-loot** (0x588F90, only when the latch is set): takes the coin
  (`CMSG_LOOT_MONEY`, 0x6D4B90) and every item that needs no roll, then
  releases the loot when nothing needs the window. **Without auto-loot the coin
  stays in the window** until clicked.
- Closing (0x523640): `CMSG_LOOT_RELEASE` 0x15F, `LOOT_CLOSED`, and a corpse
  with nothing left loses its selection (0x5241B0).
- Sounds: `LootFrame.lua` plays `LOOTWINDOWOPENEMPTY` for an empty loot and
  `FISHING REEL IN` for a fishing one; nothing else on open or close. The
  client's `LOOTWINDOWCOINSOUND` (0x519280 case 2) is the money-on-cursor
  sound; the coin loot's sound was not traced past it.

## 6. Game objects

### Right-click (0x711140) and Use (0x70F680)

1. The type handler (`+0x1A0`, built by 0x714250 per `GameObjectType`) must
   say **IsInteractable** (vtable +0x18; `game_object_affordance.hpp`).
2. Release any open loot (0x523640); latch auto-loot (0x6CDE50).
3. **CanUseNow** (handler +0x1C, 0x711470 for most types), in order:
   player dead `ERR_PLAYER_DEAD`; stunned `ERR_GENERIC_STUNNED` (or the
   stunning aura's text); with no lock: mounted and the object does not
   allow it (0x710390) and no `autoDismount` -> `ERR_NOT_WHILE_MOUNTED`;
   shapeshifted (form flag 8 clear) without `autoUnshift` ->
   `ERR_NOT_WHILE_SHAPESHIFTED`; in combat (`UNIT_FLAG_IN_COMBAT`) and the
   chest says not in combat (data12, 0x710340) -> `ERR_NOT_IN_COMBAT`;
   `UNIT_FLAG` 0x80000000 -> `ERR_USE_CANT_IMMUNE`; a locked object
   (`GAMEOBJECT_FLAGS` 0x2) whose lock the player cannot open ->
   `ERR_USE_LOCKED` / `ERR_DOOR_LOCKED` / `ERR_BUTTON_LOCKED`; then **the
   reach** (handler +0x20, 0x70FA70) -> `ERR_USE_TOO_FAR`.
4. Too far with `autoInteract` and not a bobber: click-to-move to it
   (0x72B730). Otherwise the error.
5. The handler's **Use** (+0x24), then **`CMSG_GAMEOBJ_REPORT_USE` 0x481**.

No stop, no facing, no stand-up and no stop-attack on this path.

### Reach (0x70FA70)

The reach is the handler's float `+0xC`, or the max range of the spell that
opens the lock (0x70F160 -> 0x802C30):

| Type | Reach | Constructor |
|---|---|---|
| most | 5.0 | 0x7124B0 (0xA33574) |
| QUESTGIVER 2, TEXT 9, FLAGSTAND 24, FLAGDROP 26, MINI_GAME 27 | 5.5556 | 0xA33570 |
| BINDER 4 | 10.0 | 0x70C4E0 |
| CHAIR 7, BARBER_CHAIR 32 | 3.0, from the seat slots (0x711DB0) | 0x70C6A0, 0x70C7F0 |
| AREADAMAGE 12 | 0 | 0x712590 |
| FISHINGNODE 17 | 100 | 0x712700 |

SPELL_FOCUS (8) tests a sphere of that radius around the object; a door its
collision box; everything else the display's model box (`GameObjectDisplay
Info` GeoBox, scaled) grown by the reach on every axis, in the object's own
frame.

### Use, per type (handler +0x24)

- **Generic** (0x7116C0): evaluate the lock (0x70F160, Lock.dbc rows:
  `LOCK_KEY_ITEM` needs the item, `LOCK_KEY_SKILL` a known spell with
  `SPELL_EFFECT_OPEN_LOCK` (33) of that LockType and enough skill - required
  is the Lock row's skill or `GAMEOBJECT_LEVEL * 5`, `LOCK_KEY_SPELL` the
  spell). A spell is needed: **cast it at the object** (0x80DA40, the herb /
  mine / pick-lock / open spell), so gathering is a cast, not a USE. No spell
  needed: dismount (unless the object allows the saddle) and unshift, then
  **`CMSG_GAMEOBJ_USE` 0xB1**. Failures: `ERR_CHEST_IN_USE`,
  `ERR_USE_LOCKED_WITH_ITEM_S`, `ERR_USE_LOCKED_WITH_SPELL_S`,
  `ERR_USE_LOCKED_WITH_SPELL_KNOWN_SI`, `ERR_USE_CANT_OPEN`.
- **Mailbox** 19 (0x70C9D0): opens mail locally (0x56DA60, `MAIL_SHOW`);
  **no `CMSG_GAMEOBJ_USE`**. Usable mounted.
- **Text** 9 (0x70C850): opens the page locally (0x58A1A0, `ITEM_TEXT_BEGIN`);
  no USE.
- **Guild bank** 34 (0x70ECB0): `CMSG_GUILD_BANKER_ACTIVATE` (0x6D23C0); no
  USE.
- **Meeting stone** 23 (0x712270): needs a party (`ERR_MEETING_STONE_NEED_
  PARTY`), a target (`ERR_GENERIC_NO_TARGET`), the level
  (`ERR_MEETING_STONE_INVALID_LEVEL`) and the target in the party
  (`ERR_MEETING_STONE_TARGET_NOT_IN_PARTY`), then the generic use.
- **Summoning ritual** 18: its CanUse (0x712080) checks the ritual spell.
- **Chair** 7 / barber chair 32: generic use; interactable only when the
  player is not already sitting on it (stand state 4..6 within the seat
  reach, 0x711CE0). The sit comes back as the server's stand state.
- **Fishing bobber** 17: interactable only as the player's own channel object
  (0x712030); reach 100.
- Transport 11, MO transport 15, spell focus 8, aura generator 30, area
  damage, map object, duel arbiter, trap door, destructible: not
  interactable (+0x18 = 0x427A90).

### Getting down

0x710390: a mailbox may be used mounted, a barber chair never, anything else
when its template's "allow mounted" field is set (QUESTGIVER data8, TEXT
data3, GOOBER data17, SPELLCASTER data3; meaning 91 in the table at
0xA36F30). The data field holding the lock is meaning 4 (DOOR/BUTTON data1,
most others data0, FISHINGHOLE data4).

## 7. Cursors

Names, table at 0xAD280C (1-based): Point, Cast, Buy, Attack, Interact,
Speak, Inspect, Pickup, Taxi, Trainer, Mine, Skin, GatherHerbs, PickLock,
Mail, LootAll, Repair, RepairNPC, Item, SkinHorde, SkinAlliance, Innkeeper,
Quest, QuestRepeatable, QuestTurnIn, vehichleCursor; then the same 26 with
`Unable` in front. A name not in the table is loaded as
`Interface\Cursor\<name>.blp` (0x70CEC0, 0x715F20).

### Units (CGWorldFrame 0x4F7A50)

- **An NPC** (0x729620, NPC flags set): "out of reach" is beyond `combat reach
  + 4`, and every choice below takes `Unable` then. First the quest status
  (0x744640: AVAILABLE and the low-level/incomplete ones Quest, REWARD_REP
  QuestRepeatable, REWARD/REWARD2 QuestTurnIn), then the template's
  `IconName` (0x715EA0), then by flag: REPAIR RepairNPC, INNKEEPER
  Innkeeper, FLIGHTMASTER Taxi, TRAINER Trainer, SPIRITHEALER/GUIDE Speak,
  BANKER/GUILD_BANKER Buy, PETITIONER/TABARD/BATTLEMASTER Speak, AUCTIONEER
  Buy, STABLEMASTER Speak, **VENDOR Pickup**, GOSSIP Speak, SPELLCLICK
  Interact, a boardable vehicle vehichleCursor.
- **A lootable corpse**: LootAll with auto-loot (the latch as it would be now,
  so holding Shift flips it), else Pickup; Unable when it cannot be looted
  from here (0x6D5A60) and is not the open loot.
- **Skinnable**: Skin, GatherHerbs (type flag 0x100), Mine (0x200, 0x8000);
  a player corpse SkinHorde/SkinAlliance; Unable out of the spell's range.
- **An enemy** (CanAttack, the player not pacified): Attack; UnableAttack
  beyond melee reach.
- Anything else: the plain pointer.

### Game objects (0x4F59F0 -> 0x70CE10)

- Not interactable: the plain pointer (it may still be highlighted and named).
- The template's `IconName` when it has one, `Unable`+name when CanUseNow
  fails.
- Else the type handler (+0x14):
  - default (0x70F9B0): the lock's LockType `CursorName` (Lock `Index[0]`;
    PickLock, GatherHerbs, Mine, FishingCursor), `Unable` in front when it
    cannot be used now - **except PickLock**, which never says Unable; with no
    lock, Interact / UnableInteract;
  - QUESTGIVER (0x70FDE0): the quest status cursor, else the default;
  - TEXT (0x70B540): Inspect / UnableInspect;
  - MAILBOX (0x70B680): Mail / UnableMail;
  - GUILD_BANK (0x70B910): Buy / UnableBuy.

**An object out of reach keeps its highlight and its name.** Only the cursor
changes; the click is refused with `ERR_USE_TOO_FAR`.

## 8. Other players

- **Right-click** on another player goes through 0x731260 too: a friendly one
  is targeted and nothing else; an enemy is attacked. Trade and inspect are
  never started by a click.
- **Trade** (`InitiateTrade`, 0x522600): dead `ERR_PLAYER_DEAD`, self
  `ERR_TRADE_SELF`, other faction `ERR_PLAYER_WRONG_FACTION`, then
  `CMSG_INITIATE_TRADE` (0x703CF0). No distance check in the client; the
  unit menu offers "Trade" only when `CheckInteractDistance(unit, 2)`. The
  open trade is an interact target with the 11.11-yard limit, and closes past
  it.
- **CheckInteractDistance** (0x51B240): squared, strict: 1 inspect 30 yd
  (900), 2 trade 11.11 (123.4568), 3 duel 10 (100), 4 follow 30 (900).

## 9. Sounds

- **NPC greeting / farewell**: `NPCSounds` via
  `CreatureDisplayInfo.NPCSoundID` (record +0x30), greeting when the NPC
  becomes the interact target (its window opened), farewell when it stops
  being one (0x720860). Nothing on the click itself.
- **Clicking a friendly unit with nothing to offer** (0x72A680): one of its
  creature sounds, cycling.
- **Window open/close**: FrameXML's, not the client's: `GossipFrame.xml`
  igQuestListOpen / igQuestListClose, `MerchantFrame.lua`
  igCharacterInfoOpen / igCharacterInfoClose, and so on per frame.
- **Errors**: the `GameError` row's sound column (most are "NONE"; the
  red-text error sound comes from `UIErrorsFrame`).

## 10. Wowee against this

Fixed in this change:

| # | Gap | Fix |
|---|---|---|
| 1 | Windows closed at 15 yards, 2D, and only gossip, vendor, taxi and trainer; a mailbox, bank, auction house, quest window, stable or trade stayed open at any distance | One interact target, `GameHandler::updateInteractTarget`: the stock limit per kind (NPC reach + 4 in 3D, 11.11 for a player, the object's use box), and letting go closes every window bound to it (`game_handler_callbacks.cpp`) |
| 2 | Greeting on the click (even when the server refused), farewell only on `SMSG_GOSSIP_COMPLETE`, plus a second "vendor" line on every vendor list | Greeting when the window opens, farewell when the interact target is let go, not when replaced (`updateInteractTarget`; removed from `quest_handler.cpp`, `inventory_handler.cpp`) |
| 3 | The NPC played NPCWelcome (34) on every right-click | Removed: the stock client plays only the sound (`npc_interaction_callback_handler.cpp`) |
| 4 | Every NPC got `CMSG_GOSSIP_HELLO`, from any distance | Reach checked first (`ERR_TOO_FAR_TO_INTERACT`), a second click on the open NPC ignored, and one packet by the stock flag order (`interactWithNpc`, `interaction::npcHelloFor`) |
| 5 | Right-click with nothing under the pointer acted on the current target, and a targeted game object was re-used instead of what was clicked | Only the mouseover is acted on; a touch tap still uses its target (`game_screen.cpp`) |
| 6 | Corpses: no reach, moving, stand or mount rule; no skinning at all | `GameHandler::rightClickCorpse`: loot within melee reach, not while moving, stand with `autoStand`, dismount with `autoDismount`; skinnable corpses cast Skinning / Herb Gathering / Mining |
| 7 | Auto-loot only from the option; Shift did nothing; the coin was always taken | Latched per click (option XOR Shift); the coin is taken only when auto-looting (`inventory_handler.cpp`) |
| 8 | The loot window stayed open while walking | Closed (and released) when the player moves, turns, pitches or flies up/down (`game_handler.cpp`) |
| 9 | Loot played KneelLoop/KneelEnd | Loot, then LootHold (falls back to Loot on 3.3.5a models), LootUp only after a real LootHold (`activity_fsm.cpp`) |
| 10 | Using an object stopped the player, turned them to it and stopped the auto-attack; a running auto-attack cancelled the object's cast with "Interrupted." | None of those (`performGameObjectInteractionNow`, `interactWithGameObject`, `GameHandler::update`) |
| 11 | Any object within 12 yards dismounted the player, mailboxes included | 0x710390: mailbox from the saddle, barber chair never, others by template; `autoDismount` off refuses with `ERR_NOT_WHILE_MOUNTED` |
| 12 | Object reach a fixed 5 (10 without a box) | Per type (5, 5.56, 10, 3, 100), `interaction::gameObjectUseRange` |
| 13 | Mailbox, guild vault and signs also sent `CMSG_GAMEOBJ_USE` | Not sent; their own use opens the window, `CMSG_GAMEOBJ_REPORT_USE` still follows |
| 14 | Out-of-reach objects vanished from the pick: no highlight, no name, no cursor | Kept; only the cursor turns Unable (`scene_pick.cpp`) |
| 15 | Cursors: only vendors (with the bank's Buy bag) and objects by type (chest LootAll, everything else Interact), no Unable | The stock choice for units and objects, IconName and lock first, Unable out of reach (`drawUnitCursor`, `drawWorldObjectCursor`, `interaction::unitCursorName` / `gameObjectCursorName`; the GO query keeps `IconName`) |
| 16 | The NPC being talked to never turned to the player | It does, when it has no target, stands, and has no emote state (`interactTargetFacesPlayer`, `application.cpp`) |
| 17 | `CheckInteractDistance`: inspect/follow 28, duel 9.9, non-strict | 30 / 11.11 / 10 / 30, strict (`lua_unit_api.cpp`); the binder and talent-wipe dialogs use the NPC reach, not 5 yards |
| 18 | A chest marked "not in combat" was tried in combat | `ERR_NOT_IN_COMBAT` |

Still different:

- **Shapeshift**: talking to an NPC or using an object in a form should cancel
  it with `autoUnshift` (or refuse with the error); nothing does.
- **Emote 0x2000**: the talking NPC's emote state should stop while it talks
  (0x7171C0) and it should then turn; Wowee keeps it working and only turns
  NPCs with no emote state.
- **`autoInteract`** (click-to-move to the target) is not implemented.
- **Skinning skill check** before the cast (`ERR_USE_LOCKED_WITH_SPELL_KNOWN_SI`
  with the required value) is left to the server; engineering-loot creatures
  use Mining.
- **Gathering** still picks Herb Gathering / Mining by the node's name
  (`gatherSpellForGameObject`) before the lock path; stock reads only the lock.
- **Game object loot** is still auto-stored without auto-loot
  (`autoStoreGameObjectLoot`, a workaround for quest containers), and Wowee
  still sends delayed `CMSG_LOOT` probes after a USE; stock sends only the
  USE and waits for the server.
- **Friendly units with no NPC flags** are attacked by Wowee's fallback for
  doubtful faction data; stock does nothing (and plays a "clicked" sound).
- **Loot hold**: what 3.3.5a shows while the window is open (Loot repeated,
  or held) depends on how the animation system treats a finished one-shot the
  chooser keeps; Wowee loops it.
- **The tutorial and the 0x73AD70 player emote** on NPC interact are not
  done.
- **Vehicle boarding** (`CMSG_PLAYER_VEHICLE_ENTER`) and the NPC-flag
  `CMSG_SPELLCLICK` path are sent but untested on AzerothCore.
- **Quest giver cursors** read Wowee's quest status table; a game object quest
  giver's status may not be tracked there.

## 11. Testing in game

- Walk away from a vendor, a trainer, a banker, a mailbox (game object) and
  an auctioneer with the window open: each closes at the NPC's reach + 4
  (about 5.5 yards for a normal NPC, more for a large one), and the NPC says
  its farewell; the mailbox closes when leaving its box + 5 yards.
- Right-click an NPC from 8 yards: "You need to be closer to interact with
  that target." and nothing opens. Right-click it again with its window open:
  nothing.
- Right-click a plain vendor (no gossip): the merchant window opens directly
  (`CMSG_LIST_INVENTORY`); a gossip vendor: the gossip menu.
- The NPC turns to face you while its window is open (one without an emote).
- Hover: vendor shows the bag (Pickup), banker the coin bag (Buy), flight
  master the boot (Taxi), quest NPCs the "!"/"?" cursors; step back past
  reach and each turns grey (Unable...). Herbs show GatherHerbs, ore Mine,
  locked chests PickLock, mailboxes Mail - all grey out of reach, but the
  name stays.
- Loot a corpse while running: nothing; stand still: the Loot animation, the
  window; start moving: the window closes. With auto-loot off, the coin stays
  in the window. Hold Shift on the click: everything is taken.
- Right-click a skinned-able corpse after looting with Skinning known: the
  skinning cast starts.
- Mounted: click a mailbox (stays mounted), a herb (dismounts), a chest
  (dismounts); with Auto Dismount off, the chest says "You can't do that while
  mounted."
- Auto-attack a mob and right-click a chest: the attack keeps going.
