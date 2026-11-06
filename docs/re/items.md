# Items: use, equip, ammo, cooldowns, reading (3.3.5a, build 12340)

An audit of every item-use and equip flow against the stock client, started
from one bug: Wowee never sent CMSG_SET_AMMO, so a hunter could not load
arrows. That turned out to be one of a class - flows where Wowee had a
shortcut of its own (use the item "by id", send a swap with no question
asked) where Wow.exe has a specific message and a specific prompt. Each
section gives the stock behaviour (Wow.exe address, the FrameXML that
drives it, the AzerothCore handler that answers it), what Wowee did, and
what it does now.

Addresses are Wow.exe 3.3.5a (12340) in the headless Ghidra project. Event
ids are FireEvent (0x81B530) indices; the name table is built at runtime
from 0xC24EE0 (see `death-ghost-loot.md`) and the ones used here are
AUTOEQUIP_BIND_CONFIRM 0x110, EQUIP_BIND_CONFIRM 0x10F, USE_BIND_CONFIRM
0x111, CURSOR_UPDATE 0x113, ITEM_TEXT_BEGIN 0x114, BIND_ENCHANT 0x189,
REPLACE_ENCHANT 0x18A, NEW_AUCTION_UPDATE 0x1A1, USE_GLYPH 0x268,
END_REFUND 0x28C, END_BOUND_TRADEABLE 0x28D.

Item cache records (0x67CA30) used below: +0x18 template Flags, +0x28
InventoryType, +0x178 Bonding, +0x180 PageText, +0x18C StartQuest. Item.dbc
via 0x707220 (Class, record +4) and 0x707280 (InventoryType, +0x18). The
item object's descriptor (+0xD4) has ITEM_FIELD_FLAGS at +0x3C.

## 1. Ammunition

### Stock
Ammunition is never moved into a slot. `PLAYER_AMMO_ID` (UNIT_END + 0x41A,
field 1198 in 3.3.5; Wow.exe reads it at player descriptor +0x1068 in
0x5E8300) names the loaded entry, and CMSG_SET_AMMO (0x268, one uint32 entry,
0 to unload) sets it. AzerothCore `WorldSession::HandleSetAmmoOpcode` checks
the player carries one and calls `Player::SetAmmo` (CanUseAmmo: level, class,
skill) or `RemoveAmmo`. CMSG_AUTOEQUIP_ITEM with ammo fails: `FindEquipSlot`
has no case for INVTYPE_AMMO.

Three places send it:
- **AutoEquipCursorItem** (Lua 0x51A680 -> 0x6DFC40): cursor kind 9 (an
  ammo entry, see below) sends SET_AMMO with that entry; a cursor item whose
  Item.dbc class is 6 (Projectile) sends SET_AMMO with its entry; anything
  else is CMSG_AUTOEQUIP_ITEM (0x10A, bag, slot) after the bind check.
- **UseContainerItem** (0x5D8650): an item with an InventoryType and no
  StartQuest goes to 0x6DFEE0, which picks the item up and runs the body of
  AutoEquipCursorItem - so a right-click on arrows is SET_AMMO.
- **PickupInventoryItem(0)**, the ammo slot (0x5EA5F0 -> 0x5E85D0, internal
  slot -1): with ammo on the cursor (kind 9) SET_AMMO with it; with a
  projectile item on the cursor SET_AMMO with its entry, any other item
  AutoEquipCursorItem; with nothing held and ammo loaded, the loaded entry
  goes on the cursor as kind 9 (0x520D30) and SET_AMMO 0 unloads it.

Cursor kind 9 is an entry with no slot behind it. GetCursorInfo (0x515200)
answers `"item", id, link` for kinds 1, 7, 9 and 11; CursorHasItem
(0x515100) is true for 1 and 9; CursorCanGoInSlot(0) (0x5EA880) is true for
kind 9 or a held item that can go in the slot; PickupContainerItem
(0x5D7FF0) with kind 9 falls through to ClearCursor - the ammo stays in the
bags, so dropping it anywhere just puts it down; ClearCursor (0x519280)
does not reload it.

The slot is drawn from the field: GetInventoryItemID(0) (0x5EA3E0) is
PLAYER_AMMO_ID; GetInventoryItemTexture(0) (0x5E9BC0) is its icon while the
bags hold at least one; GetInventoryItemCount(0) (0x5E9E40) is how many the
bags hold (0x754D00); UseInventoryItem(0) does nothing (0x5EA630 skips -1).
paperdollframe.lua redraws the slot on UNIT_INVENTORY_CHANGED.

Quivers and ammo pouches are bags (INVTYPE_BAG/QUIVER, class 11 subclass
2/3); their ranged-haste spell is an on-equip spell the server applies when
the bag is worn (AzerothCore applies equip spells for bag slots 19-22). The
client's part is equipping the bag; nothing ammo-specific.

### Wowee before
No PLAYER_AMMO_ID in the update field table, no CMSG_SET_AMMO anywhere. A
right-click on arrows sent CMSG_AUTOEQUIP_ITEM (refused), dragging to the
ammo slot (inventory slot 0) returned at once (`slot < 1`), and the slot
read empty.

### Now
- `UF::PLAYER_AMMO_ID` (wotlk `update_fields.json` 1198); a change rebuilds
  the inventory picture and fires UNIT_INVENTORY_CHANGED("player").
- `Inventory::getAmmoSlot()`: the loaded entry with the bags' count as its
  stack, empty when none is carried - `inventorySlotItem(inv, 0)` returns it,
  so GetInventoryItemID/Texture/Link/Count/Quality and the tooltip answer for
  slot 0 the stock way.
- `InventoryHandler::setAmmo` sends CMSG_SET_AMMO; `autoEquipFromWire`
  (the 0x6DFC40 body) sends it for class 6 instead of an equip, so
  UseContainerItem, EquipItemByName and AutoEquipCursorItem all load ammo.
- The Lua cursor has an AMMO kind (stock kind 9): `PickupInventoryItem(0)`
  lifts/loads as above, CursorHasItem/GetCursorInfo/CursorCanGoInSlot(0)
  answer for it, any other drop target just puts it down.

## 2. Equipping

### Stock
- **AutoEquipCursorItem** (0x6DFC40) sends CMSG_AUTOEQUIP_ITEM naming where
  the held item is. The server chooses the slot (`Player::FindEquipSlot`):
  the first empty ring or trinket slot else the first, a one-hander into the
  empty hand, a two-hander into the main hand with the off hand moved to the
  bags (`HandleAutoEquipItemOpcode` stores the off-hand item). The client
  does none of that choosing.
- Before it, for an unbound item whose Bonding is 2 (BoE) and which can be
  equipped (0x6DC3F0), it parks the move and fires AUTOEQUIP_BIND_CONFIRM;
  EquipPendingItem sends it.
- **A drag onto a slot** (PickupInventoryItem with an item held, 0x5E85D0,
  and PickupContainerItem onto a bag square, 0x5D7FF0) goes to 0x6DF890:
  CMSG_SWAP_INV_ITEM (0x10D) when both ends are the player's own slots,
  CMSG_SWAP_ITEM (0x10C) otherwise - but first, if either item would land in
  an equipment slot (0-22, or the bank bag slots) unbound and BoE, it parks
  the swap and fires EQUIP_BIND_CONFIRM. Both directions: dragging a worn
  item onto a BoE item in a bag asks about the bag item.
- **EquipCursorItem(slot)** (0x51A3D0): the same swap into that slot when
  0x6D4A00 says the item can go there; otherwise AutoEquipCursorItem.
- **UseInventoryItem(slot)** (0x5EA630 -> 0x5E8A60) uses the item worn in
  that slot, with an optional unit.
- **EquipItemByName** finds the item and auto-equips it (or the named slot).

### Wowee before
- AutoEquipCursorItem called `useItemById(cursor entry)`: the *first* bag
  item with that entry was *used* (CMSG_USE_ITEM), not equipped - a sword
  dropped on the character model went nowhere, and with two of an item the
  wrong one was acted on.
- UseInventoryItem also went by entry, so a trinket with a copy in the bags
  sent a use for the bag copy (refused: not worn).
- Drags onto the paperdoll sent the swap with no EQUIP_BIND_CONFIRM - a BoE
  item dragged onto its slot bound silently.
- EquipCursorItem was not defined (addons raised).
- Right-click equips worked (AUTOEQUIP_BIND_CONFIRM included), via two
  functions that each knew one container kind.

### Now
`InventoryHandler::autoEquipFromWire(bag, slot)` is the one auto-equip (ammo,
bind prompt, CMSG_AUTOEQUIP_ITEM) for any container; `swapWithBindCheck`
is the one bind-checked move. AutoEquipCursorItem, EquipCursorItem (new),
PickupInventoryItem and the bag-square drop (`dropHeldItemInto`) go through
them; UseInventoryItem uses the worn slot. `PendingEquip` remembers either
form, so EquipPendingItem sends the right one.

CursorCanGoInSlot also lights the ranged slot for a relic and the bag bar for
bags and quivers, and slot 0 for ammunition.

## 3. Using an item with a target

### Stock
UseContainerItem (0x5D8650), in order: while a spell waits for an item
target (0x7FD620), the click is SpellTargetItem (0x5210D0); repair cursor;
from the bank, CMSG_AUTOSTORE_BANK_ITEM; bank open, deposit; merchant, sell;
mail compose, attach; **auction house, the sell box** (0x59A280,
NEW_AUCTION_UPDATE); guild bank, deposit; **trade open, the first free trade
slot** (0x5A56B0 -> 0x5A50C0); otherwise equip (InventoryType and no
StartQuest) or CGItem::Use (0x708C20): wrapping paper, StartQuest offer,
PageText or the readable flag -> read, level/skill checks, template flag 0x4
-> CMSG_OPEN_ITEM, glyph -> USE_GLYPH, then the spell (0x80DA40), which for
a spell with an item target arms the targeting cursor.

SpellTargetItem's checks (0x5210D0), for each ENCHANT_ITEM (53),
ENCHANT_ITEM_TEMPORARY (54) or ENCHANT_ITEM_PRISMATIC (156) effect:
1. BIND_ENCHANT if the enchantment's SpellItemEnchantment Flags bit 0 is set
   (it binds - AzerothCore ENCHANTMENT_CAN_SOULBOUND), the item is not bound
   and has an InventoryType. Not every BoE item: most enchants bind nothing.
2. END_REFUND (data 1) for a refundable item, unless the effect is
   temporary; END_BOUND_TRADEABLE("itemenchant") for a bound item still
   tradeable.
3. REPLACE_ENCHANT(old name, new name) if the slot the effect writes - the
   permanent for 53, the temporary for 54 - already holds an enchantment.
   A poison over a poison asks.
Prospecting (127) needs template flag 0x40000 and milling (158) flag
0x20000000, then the item's skill and a stack of five (SPELL_FAILED_
CANT_BE_PROSPECTED / _MILLED / NEED_MORE_ITEMS, shown before anything is
sent). Disenchanting and everything else is cast with no question.
BindEnchant/EndRefund(1)/EndBoundTradeable("itemenchant") re-enter with the
answer, which still meets the replace question; ReplaceEnchant casts.

### Wowee before
- The item-target cursor, SpellTargetItem, stones, oils and poisons on a
  worn weapon (PickupInventoryItem/UseInventoryItem complete the target),
  disenchanting, prospecting and milling all worked.
- BIND_ENCHANT fired for *every* unbound BoE target; REPLACE_ENCHANT only
  for the permanent slot, naming the stone item instead of the enchantment;
  END_REFUND / END_BOUND_TRADEABLE never fired and EndRefund,
  EndBoundTradeable and ReplaceTradeEnchant were undefined (the stock popups
  raised on Okay).
- UseContainerItem had no auction-house or trade branch: a right-click there
  used the item.
- Disenchanting asked a confirmation of Wowee's own
  (WOWEE_DISENCHANT_CONFIRM), with a local "Item cannot be disenchanted"
  check in front of it. Stock asks nothing.

### Now
The three questions above, in stock order and with stock arguments
(`InventoryHandler::completeItemUseOnItem`, staged through
`PendingEnchant::nextStage`); EndRefund/EndBoundTradeable answer them;
ReplaceTradeEnchant is defined. Auction and trade right-clicks as stock.
Disenchanting is cast with no question, as stock: Wowee's prompt, its
event, `__WoweeConfirmItemSpell` and the local check went (Jered, Nov
2026); the server answers an item that cannot be taken apart.

## 4. Item cooldowns

### Stock
The client starts an item's cooldown itself when its spell goes; the server
never sends one for an ordinary use (AzerothCore
`Player::AddSpellAndCategoryCooldowns` applies them server-side only). The
numbers come from the item cache: per spell SpellCooldown, SpellCategory and
SpellCategoryCooldown, used when either cooldown is set (>= 0), else the
spell's own. The category is shared: every spell in it (a healing and a mana
potion, category 4) starts the category's time. SMSG_SPELL_GO names the item
that cast as its first guid (AzerothCore `Spell::SendSpellGo`), which is how
the client knows. GetContainerItemCooldown / GetInventoryItemCooldown /
GetItemCooldown (0x510FC0 -> 0x809030) read it.

### Wowee before
The item query's three cooldown words were read and discarded; the cooldown
came from Spell.dbc alone (`seedCooldownFromSpellInfo`) - 10 s for a healing
potion (spell 439, CategoryRecoveryTime 10000) instead of the template's, and
nothing for the mana potion beside it. GetItemCooldown was undefined.

### Now
`ItemQueryResponseData::ItemSpell` keeps charges, cooldown, category and
category cooldown (both parsers; pinned in `test_item_query_layout`).
`SpellHandler::seedItemSpellCooldown`, from SMSG_SPELL_GO's cast-item guid,
starts the template cooldown and the category's on every carried item spell
in that category, before the Spell.dbc seed. GetItemCooldown is defined.

## 5. Reading

### Stock
CGItem::Use reads an item whose template has PageText, or whose
ITEM_FIELD_FLAGS has 0x200 (a letter copied out of the mail: AzerothCore
`HandleMailCreateTextItem` sets ITEM_FLAG_MAIL_TEXT_MASK and the creator).
0x58A1A0 fires ITEM_TEXT_BEGIN and fetches the text: the page text cache
(CMSG_PAGE_TEXT_QUERY) for PageText, the item text cache (CMSG_ITEM_TEXT_QUERY
with the item guid) for a letter; CMSG_READ_ITEM is sent only for the first.
SMSG_ITEM_TEXT_QUERY_RESPONSE is `uint8 0, guid, text` or `uint8 1`.
ItemTextGetCreator is the letter's creator.

### Wowee before
Books worked. A letter was never readable: UseContainerItem knew only
PageText, and the item text reply was read as one string from its first
byte, so the leading 0 made it empty. ItemTextGetCreator was always nil.

### Now
UseContainerItem treats the 0x200 flag as readable; `readItemText` sends
CMSG_ITEM_TEXT_QUERY; the reply is parsed as the server sends it;
ItemTextGetCreator names ITEM_FIELD_CREATOR.

## 6. The rest

| Flow | Stock | Wowee |
|---|---|---|
| Item query functions | IsUsableItem 0x51C450, IsConsumableItem 0x5173E0 (INVTYPE 24/25 or a charge-spending spell), IsCurrentItem 0x5110D0, ItemHasRange/IsItemInRange, GetInventoryItemQuality 0x5EA040 (-1 with no InventoryType) | all undefined; now defined (IsUsableItem's noMana is not modelled) |
| GetCursorInfo | `"item", id, link` | no link; now three values |
| Bind on use | USE_BIND_CONFIRM, ConfirmBindOnUse | already stock |
| Delete | world drop -> DELETE_ITEM_CONFIRM(name, quality); uiparent picks DELETE_GOOD_ITEM ("type DELETE") at quality >= 3; DeleteCursorItem -> CMSG_DESTROYITEM | already stock |
| Splitting | SplitContainerItem puts the part on the cursor, the drop names the destination (CMSG_SPLIT_ITEM; NULL_SLOT into a bag) | already stock |
| Bag swaps | worn bag onto worn bag, bag into bank bag slot | already stock |
| Keyring | keys used from the keyring; containers 87+ | already stock |
| Quest items | StartQuest item offers its quest (CMSG_QUESTGIVER_QUERY_QUEST on the item); UseQuestLogSpecialItem | already stock |
| Containers | template flag 0x4 -> CMSG_OPEN_ITEM; a locked one is refused by the server (EQUIP_ERR_ITEM_LOCKED) until unlocked by Pick Lock (spell on the item) or a key | already stock |
| Refund | ContainerRefundItemPurchase -> CMSG_ITEM_REFUND | already stock |
| Gems | SocketContainerItem/SocketInventoryItem, ClickSocketButton, AcceptSockets -> CMSG_SOCKET_GEMS | already in |

Also found on the way, same class (the item-side answer was a placeholder):
- **SMSG_COOLDOWN_EVENT** means "start the timer now" (AzerothCore
  `Player::SendCooldownEvent`; a potion's comes at once out of combat, on
  leaving combat otherwise; a SPELL_ATTR0_DISABLED_WHILE_ACTIVE spell's when
  its effect ends). Wowee *erased* the cooldown on it, so a potion lost its
  cooldown the moment the event followed the cast. Now a potion's or such a
  spell's cooldown is held until the event (GetContainerItemCooldown answers
  the length with enable 0, which containerframe.lua greys) and started by it.
- **GetBagName** answered "Bag" for every bag (container frame titles);
  now the bag's item name. **GetContainerNumFreeSlots** answered bagType 0
  for every bag; now the bag's BagFamily (1 quiver, 2 ammo pouch, ...), so the
  bag bar's free count no longer counts a quiver's slots.
- **CloseItemText** on a book fired nothing (only letters set the open flag),
  so the stock frame stayed up; it now fires ITEM_TEXT_CLOSED for both.

## Tests

In game, lab server, character Wptestb (account WPTEST2), own client copy,
`WOWEE_DEBUG_LUA` scripts (scratchpad `items-tb/pal.body`, `hunt.body`,
`hunt2.body` on `common.lua`):

Paladin (as the character is):
- right-click club, then shield (16, 17), then a two-hander: two-hander in
  16, 17 empty, shield back in the bags;
- three rings by right-click: 11, 12, then the third replaces 11;
- BoE cloak right-click: AUTOEQUIP_BIND_CONFIRM, popup AUTOEQUIP_BIND,
  accept -> worn; a second BoE cloak dragged onto slot 15:
  EQUIP_BIND_CONFIRM, popup EQUIP_BIND up with the cloak still on the
  cursor, accept -> worn, old cloak in the bag;
- AutoEquipCursorItem (club over the two-hander), EquipCursorItem(12);
- weightstone on the worn club (SpellIsTargeting, PickupInventoryItem(16)):
  enchant applied; second stone: REPLACE_ENCHANT "Weighted (+2 Damage)" with
  "Weighted (+2 Damage)", accept -> applied, stone spent;
- healing potion: its own and the mana potion's cooldown both 60 s
  (GetContainerItemCooldown, GetItemCooldown);
- IsUsableItem / IsConsumableItem / GetInventoryItemQuality / ItemHasRange;
- clam opened (Clam Meat looted); Simple Scroll read (ITEM_TEXT_BEGIN,
  READY, text, title); SplitContainerItem 20 off 200 dropped in an empty
  slot -> 180 + 20; DeleteCursorItem on the scroll -> gone.

Dwarf hunter (race 3 class 3 set in SQL while logged out; `.learn` Auto
Shot and Bows):
- right-click arrows: PLAYER_AMMO_ID 2512, ammo slot icon and count 400;
- PickupInventoryItem(0): cursor `item, 2512, link`, CursorHasItem,
  CursorCanGoInSlot(0); slot empties (SET_AMMO 0); dropped back: loaded;
- bullets dragged from the bag to the slot: 2516 loaded;
  AutoEquipCursorItem with arrows: 2512; EquipItemByName(2516) and
  ("Rough Arrow"); ammo lifted and dropped in a bag square: unloaded,
  arrows untouched;
- Light Quiver right-clicked into bag slot 20: ranged speed 2.30 -> 2.09
  (its haste), GetBagName "Light Quiver", bagType 1; arrows moved into it;
- relog keeps the ammo; Auto Shot at a temporary training dummy
  (`.npc add temp 24792`, deleted after): shots fire, ammo 400 -> 395 on the
  paperdoll slot.

ctest: `item_query_layout` pins the three item-spell cooldown words.

## Gaps

- A right-click equip does not lift the item onto the cursor while
  AUTOEQUIP_BIND is up (stock 0x6DFEE0 does); the prompt and the equip are
  stock.
- Prospecting/milling preconditions (flags, five of a stack) are left to the
  server's refusal instead of the client's own cast failure line.
- END_REFUND(2) / END_BOUND_TRADEABLE for socketing a refundable or
  tradeable item, and the trade window's "will not be traded" enchant slot
  (TRADE_REPLACE_ENCHANT) are not modelled.
- IsUsableItem's noMana is always nil; GetSpellCooldown does not report a
  cooldown waiting on its event.
- Ammo on the cursor let go over the world stays on the cursor (stock puts it
  down); the delete prompt correctly does not fire for it.
- Mail-letter reading (CMSG_ITEM_TEXT_QUERY) is implemented to the
  AzerothCore wire but was not exercised in game (it needs a mailbox and
  "Make copy").

