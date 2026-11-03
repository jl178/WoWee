# Everyday gameplay flows against stock 3.3.5a (build 12340)

An audit of the flows a player uses every session - questing, vendors, the bank,
trainers, flight masters, mail, the auction house, guilds, groups and the
dungeon finder, trade, inspect, talents and glyphs, the spellbook, macros, key
bindings, achievements, the character pane, dual spec, the equipment manager and
the calendar - comparing what Wowee answers and fires with what the stock
interface (FrameXML, which Wowee runs unchanged) expects, the packets with
AzerothCore's handlers (`~/acore-lab/azerothcore-wotlk`), and, where FrameXML
does not settle it, `Wow.exe` 12340 read with Ghidra.

How it was done:

- **A static pass** (`scratchpad/flows/tools/static.py`): every global function
  each flow's FrameXML calls and every event it registers, against what Wowee
  defines and fires. It found the calendar's 26 missing functions and the events
  nothing fired (glyph, trade-slot, calendar, multisell, swap, currency).
- **Six read-only reviews**, one per flow group, each checking the bindings and
  handlers against FrameXML call sites and AzerothCore's packet writers, then six
  fix passes over what they found (every item re-verified before it was changed).
- **A scripted play-through** with a test client of its own (`WOWEE_DEBUG_LUA`
  plus a small coroutine harness, `scratchpad/flows/lua/lib.lua`): it records
  every event a flow should fire with its arguments, calls the flow's API the way
  FrameXML would, and drives the server with GM commands. One client
  (`quest.lua`, `city.lua`, `misc.lua`) or two at once (`duoA.lua` /
  `duoB.lua`, Wptester and Wptestb) for party, raid, trade, guild and mail.
  `WoweeDebug_UseObject(name)` was added to the debug hook to open mailboxes and
  guild vaults.

The tables at the end of each section say what was wrong, what the stock client
does (with its evidence), and whether it is fixed.

## 0. Cross-cutting findings

These were the general causes behind many of the individual bugs.

### 0.1 Where a message goes: the GameError table

`CGGameUI::DisplayError` (`0x5216F0`) takes an index into the GameError table at
`0xAC8AF0` (730 rows of 20 bytes: GlobalStrings key, *kind*, sound, ...). The kind
decides where the text goes:

| kind | destination | count |
|---|---|---|
| 0 | the chat frame, as a system line (`0x509DD0`) | 286 |
| 1 | `UI_INFO_MESSAGE` (event `0xBC`), the yellow centre text | 31 |
| 2 | `UI_ERROR_MESSAGE` (event `0xBB`), the red centre text | 413 |
| 3 | the console | - |

Wowee sent every "game error" to the red error frame (`raiseGameError`), printed
many results in chat that the stock client shows in the centre and the other way
round, and invented sentences of its own for many actions. The table is now
`include/game/game_error_table.hpp` (generated from `Wow.exe`), and
`GameHandler::displayGameMessage(key, fallback, args)` formats the GlobalStrings
text (`formatGlobalString`, positional `%N$s` included) and sends it where the
table says, with its sound. Examples the stock table settles:
`ERR_QUEST_ACCEPTED_S`, `ERR_QUEST_FAILED_S`, `ERR_QUEST_PUSH_*`,
`ERR_TRADE_REQUEST_S`, `ERR_INITIATE_TRADE_S`, `ERR_SET_LOOT_*`,
`ERR_GROUP_DISBANDED`, `ERR_GUILD_INVITE_S`,
`ERR_RAID_LEADER_READY_CHECK_START_S` are chat lines; `ERR_QUEST_ADD_KILL_SII`,
`ERR_QUEST_ADD_ITEM_SII`, `ERR_QUEST_OBJECTIVE_COMPLETE_S`, `ERR_TRADE_COMPLETE`,
`ERR_MAIL_SENT` are info text. Tested by `tests/test_game_messages.cpp`.

Invented lines removed (each had no stock counterpart, or a different one):
"You joined a group with: X", "Loot method changed to X" (now
`ERR_SET_LOOT_*`), "X has invited you to a group" (in chat *and* as a red
error), "Your party has been disbanded" (chat *and* red error; now
`ERR_GROUP_DISBANDED` once), "Requesting trade with target", "Trade opened",
"Trade partner accepted", "X wants to trade with you", "Inviting X to guild...",
"Guild: <name>", "Guild MOTD updated.", "You have new mail.", "X killed Y."
(now the combat log's `PARTY_KILL`), "Reputation with X increased by N" as a
system line beside the stock `CHAT_MSG_COMBAT_FACTION_CHANGE` one (printed twice),
"X is Ready." / "Ready check complete: ..." / "You are ready.", "Sharing quest:
X", "Accepted: X", "X has shared the quest", "Quest Complete: X", "Purchased:
...", "Bought back: ...", the second "You have learned X." at a trainer,
"Equipment set "X" saved.", and the Dungeon Finder's own sentences ("Dungeon
Finder: Joined the queue for X", "... A group has been found ...", "... Group
proposal failed" in chat *and* red) - now `ERR_LFG_JOINED_QUEUE` /
`ERR_LFG_LEFT_QUEUE` where `0x55BDC0` says them (with their sounds), the
join-result `ERR_LFG_*` errors, and the role-check and proposal updates.

Items arriving in the bags were "Received item: [X] x3" system lines. The stock
client sends them to `CHAT_MSG_LOOT` with `LOOT_ITEM_SELF` ("You receive loot:
%s."), `LOOT_ITEM_PUSHED_SELF` ("You receive item: %s.") or
`LOOT_ITEM_CREATED_SELF` ("You create: %s."), the `_MULTIPLE` forms ("%sx%d")
for stacks, picked by `SMSG_ITEM_PUSH_RESULT`'s received/created words, and
nothing when its display-in-chat word is zero.

### 0.2 Events fired twice, so FrameXML acted twice

- `GOSSIP_SHOW` fired on `SMSG_GOSSIP_MESSAGE` and again when the greeting's npc
  text arrived. `GossipFrame_OnEvent` answers every `GOSSIP_SHOW` of a one-option
  NPC with `SelectGossipOption(1)`, so the first visit to every vendor, banker
  and flight master sent the option twice and opened the window twice
  (`MERCHANT_SHOW` x2, `TAXIMAP_OPENED` x3). Now held until the text answers (or
  one second) and raised once (`QuestHandler::handleGossipMessage`).
- `selectGossipOption` also sent its own `CMSG_LIST_INVENTORY`,
  `CMSG_BANKER_ACTIVATE`, `MSG_AUCTION_HELLO` and `MSG_LIST_STABLED_PETS` beside
  the select; the server opens those windows itself
  (`Player::OnGossipSelect`). Kept only for Wowee's own (non-FrameXML) gossip
  window.
- `TAXIMAP_OPENED`: `0x5932E0` returns at once when the map is already built for
  this flight master's guid, so a second `SHOWTAXINODES` raises nothing.
- `PLAYER_ENTERING_WORLD` fired twice on every teleport and instance entry:
  once from the `SMSG_NEW_WORLD` handler and again when the world finished
  loading. The second is gone.
- `INSPECT_TALENT_READY` fired every couple of seconds in a crowd: Wowee inspects
  nearby players on its own for their gear's look, and every answer replaced the
  inspect frame's data. Only the inspect the interface asked for
  (`NotifyInspect` / `InspectUnit`) reaches the frame now.

### 0.3 Index spaces

The quest log has two: display rows (zone headers count) and the raw list. Many
"selected quest" readers subscripted the raw list with a display row, so they
answered for the quest one or more rows below - the share button, XP, honor,
reputation, title, timer, watch index, special item, reward counts. All go
through the row list now (`questAtLogRow`), and the game layer's quest events
(`QUEST_ACCEPTED`, `QUEST_WATCH_UPDATE`) carry rows through a resolver the
interface layer installs. The raid roster had the same shape (the player was not
in it; fixed in §9).

### 0.4 The quest log follows the server's slots

Quests entered the local log only behind a pending accept, and never left it
unless the client itself removed them. A GM `.quest remove`, an escort's party
accept, a script grant or a shared quest left the log wrong until relog (a
removed quest kept showing its old progress). `applyQuestStateFromFields` now
adds any quest a slot takes and removes any a slot drops, and raises
`QUEST_ACCEPTED(row)` there - which is where `Wow.exe` raises it (`0x6DF2B0` /
`0x6DF370` on a slot taking a new quest id, after `ERR_QUEST_ACCEPTED_S`).

### 0.5 Smaller general fixes

- `GameTooltipTextLeftN:GetText()` read nothing: the lines live in the tooltip's
  own list and the FontStrings stay empty. It now answers the line, so the stock
  idiom for reading a tooltip works.
- `OnCursorChanged` handlers were compiled with `width, height` as parameter
  names; Blizzard's inline bodies use `w, h`
  (`ScrollingEdit_OnCursorChanged(self, x, y-10, w, h)` in mailframe.xml), so
  `cursorHeight` was nil and `ScrollingEdit_OnUpdate` raised on the mail body
  (`uipaneltemplates.lua:380`) on typing and after every mail sent.
- AzerothCore re-joins the constant channels on every zone change and refuses
  `LookingForGroup` to a player not queued (`CHAT_NOT_IN_LFG_NOTICE`, notify 33).
  3.3.5 has no `CHAT_NOT_IN_LFG_NOTICE` string, so the stock client shows
  nothing; Wowee printed "You must be in a LFG queue to join 'LookingForGroup'."
  at every teleport. The simple channel notices now go through
  `CHAT_MSG_CHANNEL_NOTICE` with their stock tokens, so FrameXML words them.
- `ClearTarget` never told the server: `CMSG_SET_SELECTION` went out for a new
  target but not for none, so the server kept the old selection (what other
  players see as this player's target, and what GM commands act on).
- The player's own trade offer was never shown: AzerothCore's
  `TradeData::SetItem`/`SetMoney` send `SMSG_TRADE_STATUS_EXTENDED` to the
  *trader* only, so the stock client draws its own side from what it put down.
  Wowee waited for the server; its own trade slots and gold stayed empty.
- Lua bindings that popped the stack between their return values
  (`GetGuildRosterInfo`, `GetRaidRosterInfo`, after the guild fixes) were
  restructured so `tools/lua_return_window_check.py` reads zero again.

## 1. Questing

**Status: works like stock in the scripted run** (gossip accept, item-started
quest, kill and item objectives with their messages, tracker, progress panel,
choice turn-in, POI icons). Explore-complete text is implemented from `Wow.exe`
but could not be triggered from a script (a GM `.quest complete` does not send
`SMSG_QUESTUPDATE_COMPLETE`, and a teleport into an area trigger is held back by
Wowee's post-teleport portal guard).

Stock evidence used:

- Gossip lists (`0x58A6C0`, `0x58A750`, `0x58A5D0`, `0x58A660`): a quest is
  *active* when its icon is 3 or 4, *available* otherwise; nothing else.
  `GetGossipActiveQuests` (`0x58B490`) takes isComplete from the quest log
  (`0x5DEEB0`, `0x5E0EA0`), not the icon; isTrivial from `0x58ABA0`.
  AzerothCore sends 4 for every taken quest and 0 for auto-complete offers.
- `CompleteQuest` (`0x58D610` -> `0x58CF40` -> `0x6D4DD0`) sends
  `CMSG_QUESTGIVER_REQUEST_REWARD` (0x18C), once, while the progress panel is up.
- Progress text (`0x6D8030`, the quest-update opcode handler): ADD_KILL prints the
  objective's own ObjectiveText with `ERR_QUEST_ADD_FOUND_SII` when it has one,
  else the creature (`ERR_QUEST_ADD_KILL_SII`) or object name;
  `SMSG_QUESTUPDATE_COMPLETE` prints "<AreaDescription> (Complete)"
  (`ERR_QUEST_OBJECTIVE_COMPLETE_S`) or `ERR_QUEST_UNKNOWN_COMPLETE`; FAILED and
  FAILEDTIMER print `ERR_QUEST_FAILED_S`; then `QUEST_WATCH_UPDATE(logIndex)`.
- Item objectives (`0x6D7C30`): AzerothCore sends `SMSG_QUESTUPDATE_ADD_ITEM`
  empty; the client counts its bags and prints "%s: %d/%d"
  (`ERR_QUEST_ADD_ITEM_SII`, info) while the count was still short, capped at the
  requirement.
- `GetContainerItemQuestInfo` (`0x5D9400`): isQuestItem is bonding 4, questId the
  item's StartQuest, isActive whether that quest is in the log.
- `QUEST_ACCEPTED(logIndex)` and `ERR_QUEST_ACCEPTED_S` from the slot update
  (`0x6DF2B0`).
- `ToggleWorldMap` and `GetQuestID` do not exist in 3.3.5 (not in `Wow.exe`'s
  Lua tables) - not gaps.

| Bug | Fixed |
|---|---|
| Selected-quest readers used the raw log index with a display row (share, XP, honor, rep, title, timer, watch index, special item, reward counts) | yes |
| `QUEST_WATCH_UPDATE` / `QUEST_ACCEPTED` carried the raw position / the quest id; FrameXML hands them to `AddQuestWatch` | yes |
| Auto-tracking done in C++ on every accept, ignoring `AUTO_QUEST_WATCH` and the cap | yes (FrameXML tracks on `QUEST_ACCEPTED`) |
| Quest log did not follow the server's slots (removed quests stayed, script/escort quests never appeared) | yes |
| Gossip classified by icons AzerothCore never sends (2/7/8, 5/6/10): auto-complete offers in no list, every turn-in drawn incomplete, finished quests flipped to incomplete on talking to the NPC | yes |
| `GetActiveTitle` returned no isComplete | yes |
| Progress panel skipped automatically for finished quests; `CompleteQuest` sent COMPLETE_QUEST again | yes |
| Turn-in overwrote the log's objectives with "Required items: - X x10" | yes |
| No item-objective progress text; complete quests read "0/8" | yes |
| ObjectiveText and AreaDescription not parsed (kill-credit quests showed "<bunny> slain") | yes |
| "Quest accepted: X" never printed (sound only) | yes |
| FAILED as nonstock chat; COMPLETE as nonstock "Quest Complete: X" | yes |
| `MSG_QUEST_PUSH_RESULT` unhandled | yes |
| `QuestLogPushQuest` / `GetQuestLogPushable` ignored their index | yes |
| `GetContainerItemQuestInfo` always false/nil | yes |
| `GetQuestLogTitle` questTag, suggestedGroup, isDaily always empty; `GetQuestLogGroupNum` 0 | yes (QuestInfo.dbc, SuggestedPlayers, flag 0x1000) |
| `GetQuestSortIndex` nil | yes |
| `QUEST_ITEM_UPDATE` fired for every item answer while a gossip window was open | yes |
| Gossip trivial flag always false | yes |
| Gossip option icons naming textures that do not exist (spiritguide, innkeeper) | no |
| A quest frame open blocks re-talking to the same NPC (`interactWithNpc` refuses the guid whose window is open) | not changed (unverified against stock) |

In game (`quest.lua`): McBride's gossip lists "A Threat Within" as active and
complete, the turn-in goes straight to the reward panel, the chained "Kobold Camp
Cleanup" detail opens and accepting it prints "Quest accepted: Kobold Camp
Cleanup" and raises `QUEST_ACCEPTED 4` (its row); Arcane Explosion kills raise
"Kobold Vermin slain: 1/10", "2/10"; looted wolf meat raises "Tough Wolf Meat:
1/8", "2/8", "3/8", "8/8"; Eagan's progress panel shows the required meat, Continue
opens the choice panel, `GetQuestReward(2)` gives Wolfskin Bracers; the POI icons
answer for every quest. Screenshots: `scratchpad/flows/shots/quest/`.

## 2. Vendors, repair, buyback, stack splitting, tooltips

**Status: works** (buy, sell by right-click, buyback, split, repair, compare
tooltip with its "Currently Equipped" header).

| Bug | Fixed |
|---|---|
| `CMSG_SPLIT_ITEM` count written as uint8 (AzerothCore reads uint32): every stack split was dropped by the server | yes (uint8 kept for 1.12/2.4.3) |
| A held split ignored by `PutItemInBag`, `PutItemInBackpack`, `PickupBagFromSlot` (moved the whole stack) | yes |
| `PutItemInBag/Backpack` chose a slot client-side (no stack merging, no "bag full") | yes (`CMSG_AUTOSTORE_BAG_ITEM`) |
| `GameTooltip:SetBagItem` knew containers 0-4 only and returned one value; `SetInventoryItem` no repair cost | yes |
| A bootstrap Lua `SetBagItem` override shadowed the C binding and added a "Count:" line | yes |
| `KeyRingButtonIDToInvSlotID` identity (key 1 showed the helmet); `ContainerIDToInventoryID` wrong for bank bags | yes |
| `GetRepairAllCost` canRepair meant "can afford" (guild-bank repair button disabled when poor) | yes |
| Repair estimate included bank items | yes |
| Bank bag slot prices hard-coded, both tables wrong | yes (BankBagSlotPrices.dbc) |
| Merchant/buyback isUsable always true | yes |
| `GetMerchantItemMaxStack` ignored the vendor's BuyCount | yes |
| Buyback list mirrored client-side; sales outside the right-click path missing | yes (VENDORBUYBACK_SLOT fields; price/timestamp fields still not read) |
| "Sell Price:" line on every tooltip | yes (through `OnTooltipAddMoney`, at a merchant only) |
| Compare tooltip lacked `CURRENTLY_EQUIPPED` | yes |
| Duplicate `MERCHANT_SHOW` on first visit | yes (§0.2) |

In game (`city.lua`): Edna Mullby's list (37 items), buying 5 Coarse Thread for
45c, splitting 2 off, selling 3 back for 6c and buying them back; after a fall
death (durability 35->32 and 25->23) Gunther Weller's `GetRepairAllCost` is 2c,
`RepairAllItems` pays it and durability is full; the compare tooltip reads
"Currently Equipped / Bent Staff".

## 3. Bank and bags

**Status: works** (bank frame, moving items in and out, slot ids, slot cost).
Sorting does not exist in 3.3.5; Wowee's `SortBags`/`IsSortingBags` globals stay
because the bundled `WoweeAllBags` addon uses them (no stock code calls them).

## 4. Trainers

**Status: works.** Filter defaults are now stock (available 1, unavailable 1,
used 0, `blizzard_trainerui.lua:9-11`); a rank gated on another ability is
re-checked after a purchase.

## 5. Flight master

**Status: works.** `TAXIMAP_OPENED` once (§0.2). Not a bug: the GM test
character sees Horde nodes as REACHABLE because GM taxi-cheat marks every node
known; the stock client's node list (`0x5932E0`) filters known nodes by
continent and map expansion only, not by faction.

## 6. Mail

**Status: works** (send with an attachment, receive, read without
auto-delete, delete). Taking a COD item was not exercised end to end (the
second client ran out of GPU memory in the last run).

| Bug | Fixed |
|---|---|
| Return to Sender lacked AzerothCore's trailing uint64: the packet was dropped | yes |
| `InboxItemCanDelete` always true (Delete offered where stock offers Return) | yes |
| textCreated/wasReturned/isTakeable computed wrongly: plain letters deleted on close | yes |
| `MSG_QUERY_NEXT_MAIL_TIME` never sent (no envelope at login, empty sender tooltip) | yes |
| A failed send dropped the attachments | yes |
| `GetSendMailPrice` constant 30 | yes |
| "You have new mail." chat line, "Mail sent." in chat (stock: info text) | yes |
| `OnCursorChanged` arg names broke the mail body (§0.5) | yes |

## 7. Auction house

**Status: fixed from review; not exercised in game.**

| Bug | Fixed |
|---|---|
| `GetAuctionSellItemInfo` returned one nil when empty (Lua error on every post) | yes |
| `StartAuction` ignored numStacks and could not build a stack from several bag stacks | yes (multi-piece sell, `AUCTION_MULTISELL_*`) |
| Owner/bidder lists not refreshed after sell, cancel, bid | yes |
| Auction error codes mapped wrongly | yes |
| List results routed by shared state | yes |

## 8. Trade

**Status: works in game** (request, item and gold on both sides, accept,
complete).

| Bug | Fixed |
|---|---|
| Declining a request sent nothing (the other player waited enhanced) | yes |
| Items/gold locked after the partner accepted | yes |
| Status 12 (CLOSE_WINDOW) / 10 treated as the end of the trade | yes |
| Old contents shown in the next trade | yes |
| `GetTradeTargetItemInfo` shape wrong (partner items always red) | yes |
| `UnitName("npc")` not the partner | yes |
| `TRADE_REQUEST_CANCEL`, `TRADE_*_ITEM_CHANGED` never fired | yes |
| Nonstock trade chat lines | yes (§0.1) |
| The player's own offer never drawn (the server does not echo it) | yes (§0.5) |

## 9. Guild and guild bank

**Status: create, invite, accept, MOTD, roster, bank open, buy tab, deposit
gold work in game.**

| Bug | Fixed |
|---|---|
| Tab names parsed on every full update (garbled every tab but the first) | yes |
| A partial slot update wiped the tab; another member's move switched your tab | yes |
| Item-withdrawal count used as the gold limit; gold limit never asked for | yes |
| `GetGuildBankTabInfo` hard-coded rights | yes; and the guild master now always full (a tab bought after the roster read 0) |
| Moving items inside the guild bank impossible | yes |
| `GUILDBANK_ITEM_LOCK_CHANGED` never fired | yes |
| Roster status a number 0 ("0" shown for online members) | yes |
| `GuildRoster()` unthrottled | yes (10 s, no stock figure found) |
| `GetGuildInfo(unit)` ignored the unit | yes |
| `GetGuildRosterMOTD` stale after `GUILD_MOTD` | yes |
| `GUILD_ROSTER_UPDATE` etc. | correct already |

## 10. Party, raid, ready check

**Status: invite, accept, loot method, ready check, convert to raid, raid
roster work in game.**

Ready check from `Wow.exe`: `MSG_RAID_READY_CHECK` (`0x574D50`) starts a
thirty-second check; the initiator is not asked and is counted ready by their
own client (`GetReadyCheckStatus` `0x574180` answers "ready" for the
initiator's guid) - no packet; everyone else gets
`ERR_RAID_LEADER_READY_CHECK_START_S`; `READY_CHECK(initiator, secondsLeft)`
(`0x5747D0`). When the thirty seconds run out the *initiator's client* prints
`RAID_MEMBERS_AFK` or `READY_CHECK_ALL_READY` and sends
`MSG_RAID_READY_CHECK_FINISHED` (`0x573390`), which AzerothCore broadcasts.
`DoReadyCheck` during one is `ERR_READY_CHECK_IN_PROGRESS` (`0x5732B0`).

| Bug | Fixed |
|---|---|
| `ConvertToRaid` a no-op | yes |
| Raid roster left out the player (last index nil; Lua errors in the raid tab) | yes |
| `GetRaidRosterInfo` role "NONE" and class "Unknown" crashed `blizzard_raidui.lua` | yes |
| `GetReadyCheckStatus` always nil | yes |
| `READY_CHECK_CONFIRM` passed a guid string | yes |
| Ready check: 35 s, initiator auto-confirmed with a packet (from the first fix pass) | replaced by the stock rules above |
| `RAID_ROSTER_UPDATE` not fired on leaving a raid | yes |
| `PARTY_INVITE_CANCEL` never fired | yes (AzerothCore never sends it) |
| `GetLootMethod` indices; event on master/threshold change | yes |

## 11. Dungeon finder

**Status: queue, proposal, accept, teleport in and `LFGTeleport(true)` out work
in game** (with the server's `.debug lfg` one-player test mode, switched back
off after).

| Bug | Fixed |
|---|---|
| `LFGTeleport` direction inverted | yes |
| Role check: accept left the popup up, decline sent nothing, leader bit wrong | yes |
| `GetLFGProposal` hasResponded always false | yes |
| Role-check end states left a stale "rolecheck" | yes |
| The queue restatement AzerothCore sends while a proposal is up (queued flag set) dropped the proposal: `GetLFGProposal` went nil, the dialog had nothing to accept and the proposal timed out | yes |
| Being taken out of the queue on entering the dungeon set the state to none | yes |
| `IsPartyLFG` / `IsInLFGDungeon` from the queue state only, not the group's finder flags (0x04/0x08) | yes (not verifiable with a one-player debug group) |
| Nonstock Dungeon Finder chat lines | yes (§0.1) |

In game (`misc.lua`): `JoinLFG` prints "You are now queued in the Dungeon
Finder.", `GetLFGMode` answers "queued", then "proposal", "unaccepted" with
`GetLFGProposal` answering the random Lich King dungeon as DAMAGER;
`AcceptProposal` teleports to the dungeon (`GetInstanceInfo` "party"), and
`LFGTeleport(true)` brings the player back out.

## 12. Inspect

**Status: works** (`INSPECT_TALENT_READY`, gear, talents of the inspected
class). Auto-inspects no longer reach the frame (§0.2).

## 13. Talents, glyphs, dual spec, spellbook

| Bug | Fixed |
|---|---|
| Dual-spec switch left the old spec's buttons (`SMSG_ACTION_BUTTONS` mode 2 ignored, zeros skipped) | yes |
| The talentGroup argument ignored (inactive spec tab showed the active spec) | yes |
| Inactive spec's unspent points stale or zero | yes |
| Inspect half-handled in prereqs, meetsPrereq, unspent points, tooltip | yes |
| Glyph socket types hard-coded wrongly; sockets never locked | yes (GlyphSlot.dbc order: major, minor, minor, major, minor, major; `PLAYER_GLYPHS_ENABLED`) |
| Placing a glyph sent spell 0 / item guid 0; using one put it in socket 1 | yes |
| `GLYPH_ADDED/REMOVED/UPDATED`, `USE_GLYPH` never fired; `GlyphMatchesSocket` false | yes |
| `SMSG_SUPERCEDED_SPELL` did not refresh the spellbook | yes |
| `GetSpellAutocast`, `IsSelectedSpell`, GCD on every spell | yes |
| `LearnPreviewTalents` sent one packet per rank | yes (`CMSG_LEARN_PREVIEW_TALENTS`) |
| Pet talents | no (feature gap) |

## 14. Macros and key bindings

| Bug | Fixed |
|---|---|
| A macro made by the stock UI was invisible (nil body erased it) and stored its icon as "37" | yes |
| Deleting a macro left action-bar slots pointing at the wrong macro | yes |
| Account macros stored per character | yes |
| `UPDATE_MACROS` never fired; `GetMacroBody`/`GetMacroIndexByName` stubs | yes |
| Binding sets: `GetCurrentBindingSet` fixed at 1; `SaveBindings(2)` saved account-wide | yes |
| `SetBindingSpell/Macro/Item/Click` missing | yes |
| `GetMacroSpell`/`GetMacroItem` | no |

## 15. Character pane, equipment manager, achievements

| Bug | Fixed |
|---|---|
| Spell crit 0.00% (school index off by one) | yes |
| Stat buffs, AP mods, armour/resistance buffs, weapon damage and speed from items instead of the unit fields | yes |
| Currency tab missed the token slots and honor/arena | yes |
| Equipment manager: ignored slots stripped gear (guid 0 instead of 1) | yes |
| `EQUIPMENT_SWAP_FINISHED` fired before the result; delete fired nothing | yes |
| Achievements of the other faction listed; totals inflated by statistics; reward text empty | yes |
| Own achievement printed twice | yes |
| Duplicate reputation lines | yes (§0.1) |

## 16. Calendar

**Status: fixed from review** - 26 functions Blizzard_Calendar calls were
missing, the add-event packet lacked its invite list (so AzerothCore never made
the event), nine incoming packets were discarded, `SEND_EVENT` raised no
`CALENDAR_OPEN_EVENT`/`CALENDAR_NEW_EVENT`, `COMMAND_RESULT` and `EVENT_STATUS`
were misparsed, the locked flag was 0x1000 instead of 0x10, removing yourself
could delete someone else's event, holiday rows were wrong, and raid lockout
add/remove were misparsed. Tested by `tests/test_calendar_packet.cpp`.

## 17. In-game runs

All on the lab server (Wptester, level 80 mage, GM; Wptestb for the second
player), logs and screenshots under `scratchpad/flows/logs` and
`scratchpad/flows/shots`.

- `quest.lua`: §1. Every step answers as stock; the only timeouts are the
  explore objective (not scriptable) and the script's own waits.
- `city.lua`: §2-§5. One `GOSSIP_SHOW`, one `MERCHANT_SHOW`, one
  `TAXIMAP_OPENED` per visit; no LFG channel line on teleport.
- `duoA.lua`/`duoB.lua`: §8-§10. Party invite and accept, loot method
  ("Looting changed to Master Looter."), ready check ("Everyone is Ready" from
  the initiator after thirty seconds, `READY_CHECK_CONFIRM "party1", 1`),
  convert to raid with both players in `GetRaidRosterInfo`, inspect, trade with
  an item and 1g (both sides drawn), guild create, invite, accept, MOTD, roster,
  guild bank tab purchase and gold deposit, mail with an attachment and a plain
  letter that survives being read.
- `misc.lua`: §11, §13-§16. Stats, factions, currency, an equipment set saved,
  worn and deleted with `EQUIPMENT_SWAP_PENDING`/`FINISHED(1, "WpFlow")`, the
  spellbook tabs, talents and glyph sockets (types major, minor, minor, major,
  minor, major), a macro created and deleted with `UPDATE_MACROS`, 283
  bindings, achievements and statistics, a calendar event created (creator
  CONFIRMED/OWNER in its invite list), opened (`CALENDAR_OPEN_EVENT "PLAYER"`)
  and removed (`CALENDAR_CLOSE_EVENT`), and the dungeon finder.

## 18. Gaps

- About three hundred `addSystemChatMessage` calls remain in the handlers
  (115 in social_handler.cpp alone). Many are Wowee's own sentences; the ones on
  the audited flows were converted, and `displayGameMessage` plus the GameError
  table are there for the rest.
- `CURRENCY_DISPLAY_UPDATE` fires on every bag update (more often than stock).
- Seen while testing, outside these flows: a GM teleport into a building just
  after a resurrection fell through the floor before the WMO loaded; two test
  clients at once on this GPU ran out of device memory (`ImGui Vulkan error: -2`)
  and one crashed.
- Explore-objective completion text not triggered in game.
- Pet talents; `GetMacroSpell`/`GetMacroItem`; buyback price/timestamp fields;
  `CHANNEL_NOTICE_USER` notices (kicks, bans, owner changes) still printed as
  Wowee's own sentences; gossip option icons for spirit guide and innkeeper.
- Choices made without stock evidence: guild roster throttle 10 s; raid order by
  subgroup with the player first; calendar creator status CONFIRMED.
