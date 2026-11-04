# Professions (3.3.5a, build 12340)

The trade skill window, recipes and their colours, crafting runs, trade links,
gathering, skinning and fishing, against the stock client. Stock FrameXML
(`Blizzard_TradeSkillUI`) is run unchanged, so the work is in the functions it
calls and the events it waits for; where FrameXML does not settle a question
Wow.exe does (headless Ghidra, addresses below), and the server side is
AzerothCore's (`~/acore-lab/azerothcore-wotlk`).

Event ids are FireEvent (0x81B530) indices: TRADE_SKILL_SHOW 0x12B,
TRADE_SKILL_UPDATE 0x12C, TRADE_SKILL_CLOSE 0x12D, UPDATE_TRADESKILL_RECAST
0x1B7, TRADE_SKILL_FILTER_UPDATE 0x27D. GameError rows (DisplayError 0x5216F0,
table 0xAC8AF0) used here: 0xE8 ERR_CHEST_IN_USE, 0xE9 ERR_USE_CANT_OPEN,
0xED ERR_USE_LOCKED_WITH_ITEM_S, 0xEE ERR_USE_LOCKED_WITH_SPELL_S, 0xEF
ERR_USE_LOCKED_WITH_SPELL_KNOWN_SI (all red), 0x151 ERR_FISH_NOT_HOOKED, 0x152
ERR_FISH_ESCAPED (both yellow info lines).

The model is `include/game/stock_trade_skill.hpp` (header-only, pinned by
`tests/test_stock_trade_skill.cpp`); `SpellHandler` feeds it
(`spell_handler.cpp`, "Trade skill window") and `lua_quest_api.cpp` binds it.

## 1. Opening the window

### Stock
- **A spell whose first effect is TRADE_SKILL (47)** is not cast. 0x80CCE0
  (the cast entry under CastSpell 0x80DA40) tests `Effect[0] == 0x2F` and calls
  **0x5DE120** with the player's guid, and returns - no CMSG_CAST_SPELL. Only
  effect 0: Fishing's first effect is 50 and is cast as usual.
- 0x5DE120: any repeat stops (UPDATE_TRADESKILL_RECAST); the skill line is the
  SkillLineAbility row's for (race, class, spell) (0x5DA5F0 -> 0x812410);
  **the same line already open, and not a link, closes it** (0x5DA5A0: stop,
  TRADE_SKILL_CLOSE). A line other than the last one opened (0xC235F0) clears
  the name and level filters, "Have Materials", the subclass/slot/fold masks,
  the slot list and the selection; reopening the same line keeps them all.
  Then the list is built (0x5DD5A0) and **TRADE_SKILL_SHOW fires only when it
  has rows** - the window is open either way.
- CloseTradeSkill (0x5DA8B0): forget the line, stop a repeat, TRADE_SKILL_CLOSE.
  Leaving the world closes it (0x528F00 -> 0x5DA5A0).

### Wowee before
`tradeskillOpenerSkillLine` accepted effect 47 *or* a spell named after its
line, and only with at least one recipe known (that was what kept Fishing from
opening a window); pressing the profession again re-sent TRADE_SKILL_SHOW
instead of closing; filters and selection were Lua statics reset never.

### Now
`openCraftingWindow(opener, line)` / `closeCraftingWindow` as above; effect 0
== 47 is the only test; the model keeps the last line's filters.

## 2. The list (0x5DD5A0, 0x5DCC40)

### Which recipes
The player's own: every known spell with **SPELL_ATTR0_TRADESPELL (0x20)** in
the line - the learn handler (0x6E7B00) appends such a spell to its line's list
and rebuilds; unlearn (0x6E71D0) removes and rebuilds. Not "creates an item or
takes reagents" (Wowee's old rule), which let passive ranks and the openers in
and was how Enchanting's non-item recipes were nearly lost.

A linked list (section 6): the line's SkillLineAbility rows whose bit is set
in the link, with the same 0x20 test.

### Colour (difficulty)
From the player's skill **value only** (no bonus; 0x5DD8D4 reads the ushort at
skill field +0x7A4) and the SkillLineAbility row's TrivialSkillLineRankHigh
(+0x28, column 10) and TrivialSkillLineRankLow (+0x2C, column 11); a Low of 0
means High - 25:

| rank | type |
|---|---|
| < Low | `optimal` (orange) |
| < (Low + High) / 2 | `medium` (yellow) |
| < High | `easy` (green) |
| otherwise | `trivial` (grey) - also a row with neither threshold |

The same bands AzerothCore rolls skill-ups with (`Player::UpdateCraftSkill`:
`SkillGainChance(value, High, (High + Low) / 2, Low)`). Wowee's old bands were
grey >= High, green >= Low, yellow >= MinSkillRank + (Low - Min) / 2 - every
yellow and green boundary wrong, and a recipe with no thresholds orange.

### numAvailable
min over reagents (item and count both set) of `count in bags / needed`
(0x754D00); **-1 when the recipe uses nothing up**. DoTradeSkill clamps the
repeat to it unless it is -1; "Have Materials" hides 0 but not -1;
`TradeSkillFrame.numAvailable = math.abs(numAvailable)` makes Create All of a
reagent-less recipe one craft.

### Headers
A recipe whose effect 0 is CREATE_ITEM (24), CREATE_RANDOM_ITEM (59) or
CREATE_ITEM_2 (157) is filed under its item's **(class, subclass)** from the
item cache (+4, +8); anything else under **(-1, -1)**. A header's name is
ItemSubClass.dbc's VerboseName, else its DisplayName (0x5DA6B0, the table at
0xAD3F60, 13-int rows, +0x2C / +0x28); (-1, -1) is named by the line's
**SkillLine AlternateVerb** (+0x18, column 38) - Enchanting's "Enchant".
While any made item is not in the cache the list is held: no headers, nothing
shown, no TRADE_SKILL_UPDATE; the reply that completes it rebuilds (0x5D9F80).
Wowee had grouped by the item query's `subclassName` (the display name),
sorted headers by name, put unknown items last under no header, and churned the
list as replies landed.

### Slot bits (the slot dropdown)
Created item: the InventoryType table at 0xA2D288 (ring -> FINGER0, trinket ->
TRINKET0, bag -> BAGSLOT, nothing -> NONEQUIPSLOT). Enchant: EquippedItemClass
2 -> ENCHSLOT_WEAPON when SubClassMask bit 0 is set, else ENCHSLOT_2HWEAPON;
class 4 -> the first InventoryTypeMask bit's slot, or SHIELDSLOT for subclass
bit 6 with no mask. Names are the 27 GlobalStrings keys at 0xAD0E10;
GetTradeSkillInvSlots lists the bits seen since the line opened (0xC235D0),
narrowed to the picked subclass's.

### Showing and order (0x5DCC40, 0x5DAE60, 0x5DA6B0)
A recipe is hidden when its header's subclass filter is off, its slot bit is
not in the slot filter, "Have Materials" and numAvailable == 0, "Skill ups"
(TradeSkillOnlyShowSkillUps) and grey, or the search fails. A header shows
when one of its recipes does; a folded header hides them and stays.
Headers sort by item class, then name. Rows: shown first, by header; the
header before its recipes; **orange to grey, then higher TrivialHigh, then
higher item level, then name** (Wowee sorted by name).

Search (0x5DC110): a level range tests the made item's RequiredLevel (+0x38;
no item passes); a name matches (case-folded) the item's stat and spell lines
and description, the spell name, reagent and tool names, the spell focus, the
spell's description. Wowee matched the recipe name and filtered the level on
ItemLevel.

## 3. The functions (table at 0xAD0E80)

| Function | Address | Stock answer |
|---|---|---|
| GetNumTradeSkills | 0x5DA120 | shown rows (headers included) |
| GetTradeSkillInfo(i) | 0x5DB2A0 | header: name, "header", 0, expanded and 1 or nil, nil. Recipe: name, difficulty, numAvailable, nil, altVerb - the line's AlternateVerb when effect 0 makes no item, else nil. 5 values |
| SelectTradeSkill(i) | 0x5DA900 | remembers the **recipe's spell** (a header selects nothing) |
| GetTradeSkillSelectionIndex | 0x5DA970 | that spell's row + 1, 0 |
| GetFirstTradeSkill | 0x5DAB70 | first shown recipe, **0** when none |
| GetTradeSkillCooldown(i) | 0x5DA9A0 | seconds left or nil |
| GetTradeSkillIcon(i) | 0x5DB550 | made item's icon (Item.dbc display), else the spell's |
| GetTradeSkillNumMade(i) | 0x5DB6E0 | effect 0's points: basePoints + 1 .. basePoints + dieSides, at least 1 |
| GetTradeSkillLine | 0x5DB810 | line name, value + permanent bonus, max + permanent bonus; a link's rank and max; "UNKNOWN", 0, 0 |
| GetTradeSkillItemLink(i) | 0x5DBA70 | the made item's link (nothing until cached), else `\|cffffd000\|Henchant:id\|h[Name]\|h\|r` |
| GetTradeSkillRecipeLink(i) | 0x5DB920 | attr 0x20: `\|Henchant:id\|h[Line: Name]\|h` |
| GetTradeSkillNumReagents(i) | 0x5DBBA0 | reagent slots with an item |
| GetTradeSkillReagentInfo(i, n) | 0x5DE400 | name and texture (nil until cached), needed, carried |
| GetTradeSkillReagentItemLink | 0x5DBCC0 | the reagent's link |
| GetTradeSkillTools(i) | 0x5DE620 | pairs: spell focus name and **1 always**; AreaGroup's first area and 1; each Totem item and carried (0x754A20) or nil; each TotemCategory and whether a carried tool serves it (0x7548F0) |
| GetTradeSkillDescription(i) | 0x5DBE10 | formatted description, nil when empty |
| Get/SetTradeSkillSubClassFilter | 0x5DA1B0 / 0x5DD0F0 | bit per header; 0 = all; `(i, on, exclusive)`; errors "Bad sub class", "Missing on//off parameter" |
| Get/SetTradeSkillInvSlotFilter | 0x5DBF90 / 0x5DD230 | bit per listed slot; Get(i) is nil while the filter is "all" |
| Set/GetTradeSkillItemNameFilter | 0x5DCF60 / 0x5DA150 | **TRADE_SKILL_FILTER_UPDATE** on a change |
| Set/GetTradeSkillItemLevelFilter | 0x5DD090 / 0x5DA170 | TRADE_SKILL_UPDATE on a change |
| TradeSkillOnlyShowMakeable / SkillUps | 0x5DD3E0 / 0x5DD420 | TRADE_SKILL_UPDATE on a change |
| Collapse/ExpandTradeSkillSubClass(i) | 0x5DD460 / 0x5DD500 | by row (a header row; 0 = all); TRADE_SKILL_UPDATE |
| DoTradeSkill(i [, n]) | 0x5DAC00 | section 4 |
| GetTradeskillRepeatCount | 0x5DA270 | the selected recipe's run, at least 1 |
| StopTradeSkillRepeat | 0x5DA2E0 | stop, UPDATE_TRADESKILL_RECAST |
| GetTradeSkillListLink | 0x5DC0B0 | section 6 |
| IsTradeSkillLinked | 0x5DA310 | 1, owner's name; nil, nil |
| CloseTradeSkill | 0x5DA8B0 | section 1 |

GameTooltip:SetTradeSkillItem(i [, reagent]) shows the made item's (or the
reagent's) item tooltip, and the spell's for an enchant. Wowee had shown the
spell name and description for everything.

The whole subsystem had been a Wowee model: rows grouped by subclass display
name, index-based selection, single-pick dropdown state, numSkillUps as a sixth
value, nil for both links, an empty GetTradeSkillTools, NumMade always 1.

## 4. Crafting runs (0x5DAC00, 0x5D9FA0, 0x5DA0F0, 0x5DA070, 0x5DA020, 0x5D9FF0)

State: the running recipe and how many are left (0xC235D4/D8), and a
requested one not started yet (0xC235DC/E0).

1. **DoTradeSkill(i, n)**: n rounded, clamped to numAvailable unless -1 (so
   n may become 0). If that recipe is not the one running, it is cast
   (0x80DA40) - a refused cast records nothing. Then 0x5D9FA0: the running
   recipe takes the new count with **no second cast**; anything else is
   requested. UPDATE_TRADESKILL_RECAST.
2. The cast going out (0x80AC90 -> 0x805330 -> 0x5DA0F0): a requested recipe
   becomes the running one.
3. The player's cast going off (SMSG_SPELL_GO, 0x80F61F -> 0x5DA070): the
   running recipe, with some left, counts one down; at zero the run ends **with
   no event**; else it is cast again and UPDATE_TRADESKILL_RECAST fires. Any
   other spell going off ends the run, with the event.
4. A failed cast of it (0x808200 -> 0x5DA020, before the 3 s message throttle)
   ends it; a cancelled cast (0x806200 -> 0x5D9FF0 - a move, SpellStopCasting)
   ends any run; both with the event. Opening or closing a window too.

Wowee's queue counted on SPELL_GO the same way but recast on a second press,
answered GetTradeskillRepeatCount for any recipe, cleared the run on every
"cast state reset" (the client-side timer, which needed a 2 s grace to survive)
and on any failure while not casting. Now SpellHandler drives the model from
castSpell, SPELL_GO, showCastFailure and cancelPendingCast.

## 5. Skill-ups, learning (already stock)
- **ERR_SKILL_UP_SI** "Your skill in %s has increased to %d." (chat, from the
  skill field changing, 0x6DA4A0) and ERR_SKILL_GAINED_S - `spell_handler.cpp`
  `handleSkillFields`, unchanged; the change now also rebuilds an open list
  (recolour).
- Learning: ERR_LEARN_RECIPE_S "You have learned how to create a new item: %s."
  for attr 0x20 (0x542030), unchanged; the learn/unlearn now rebuild the list.
- Recipe items: CMSG_USE_ITEM -> the item's LEARN spell (483) on the server;
  the client only shows the line above.

## 6. Trade links (0x5DB060, 0x5DE300, 0x812030, 0x8104A0)

`|cffffd000|Htrade:<opener>:<rank>:<max>:<GUID hex, %I64X>:<bits>|h[<opener name>]|h|r`

- The bits: SkillLineAbility.dbc rows grouped at load (0x812030) into runs of
  consecutive rows of one **CanLink** line (SkillLine column 55); a line's
  first run is its list (0x8104A0). Bit i = the player knows row i's spell and
  the row applies to their race and class. Six bits a character, first row in
  the lowest bit, alphabet `A-Za-z0-9+/` (0x5DB060).
- GetTradeSkillListLink: only for the player's own open line, opened by an
  effect-47 spell, CanLink set; rank and max with the permanent bonus.
- Clicking one: SetItemRef -> ItemRefTooltip:SetHyperlink("trade:...")
  (0x62DAE0 -> 0x5DE300): parse, look up the owner's name (waiting for it),
  then 0x5DE120 with the data, which must be exactly ceil(rows / 6) long. The
  list is built from the link's bits at the link's rank; IsTradeSkillLinked
  answers 1 and the name; FrameXML hides the create buttons. numAvailable
  still counts the viewer's bags.

Wowee before: GetTradeSkillListLink nil, IsTradeSkillLinked false, links
unreadable. Now: as above (`SpellHandler::tradeSkillListLink`,
`openLinkedTradeSkill`, `lua_Tooltip_SetHyperlink` "trade").

**Guild craft lists**: not in 3.3.5 (Cataclysm's guild professions).

## 7. Gathering nodes (0x7116C0, 0x70F160, 0x7FF770, 0x6DC330)

Right-clicking a chest-type node runs the generic Use (interactions.md §6).
The lock (Lock.dbc via data0) is evaluated slot by slot (0x70F160):

- **LOCK_KEY_SKILL (2)**: a known spell with an OPEN_LOCK (33) effect whose
  misc value is the slot's LockType (Herbalism 2, Mining 3, Lockpicking 1,
  ...). Its points must reach the slot's Skill, or 5 x GAMEOBJECT_LEVEL when
  the Skill is 0. **The points are basePoints + 1 + perLevel x level where the
  "level" is the spell's own skill / 5** (0x7FF070 -> the player's vfunc
  0x6DC330: value + temporary + permanent bonus of the line the spell's
  SkillLineAbility row names, capped at MaxLevel x 5, less SpellLevel). Herb
  Gathering and Mining are -1 and 5 per level, so the points are the skill
  rounded down to a five: Tin (65) needs 65+ Mining on the client already.
- LOCK_KEY_ITEM (1): the key carried. LOCK_KEY_SPELL (3): that spell.
- Opened by a spell: **cast it at the object** (the Herb Gathering / Mining /
  Pick Lock cast and its bar and animation); nothing needed: CMSG_GAMEOBJ_USE.
- Nothing opens it - the **first** slot decides the red line: item ->
  ERR_USE_LOCKED_WITH_ITEM_S (key name); skill with no matching spell known ->
  ERR_USE_LOCKED_WITH_SPELL_S (LockType name, "Requires Herbalism"); skill with
  a spell too weak -> ERR_USE_LOCKED_WITH_SPELL_KNOWN_SI ("Requires Mining
  65"); anything else ERR_USE_CANT_OPEN. Nothing is sent.

The cursor is the LockType's CursorName (GatherHerbs, Mine), already stock.

Wowee before: herbs and ores were recognised **by name** (a list of herb names
and "vein"/"deposit"), cast the highest known rank of a hard-coded table (which
lacked Grand Master), never checked the skill, and said "Requires Mining" /
"Requires Herbalism" for an unknown spell only; the open-lock path for other
locks computed the points from the player's *level*, and fell through to a USE
when nothing opened it. Now every lock goes through `planGameObjectOpen` with
the skill-based points and the stock refusals, and `isGatherGameObject` (the
minimap's node filter) reads the lock's LockType.

## 8. Skinning and creature gathering (0x731260, 0x53BCE0, 0x542030)

A dead skinnable unit: the spell is the player's **SKINNING (95)** spell for
the corpse's kind (0x715E50: type flag 0x100 -> 1, 0x200 -> 2, 0x8000 -> 3,
else 0), filed by misc value as each is learned (0x542030): Skinning (8613..),
**Herb Gathering 32605**, **Mining 32606**, **Engineering 49383** - not the
node spells. None known: nothing at all. Its effect's points (as above, from
its own line's skill) against the creature's level: 1 under 11, 10 x (level -
10) under 20, else 5 x level; short of it ERR_USE_LOCKED_WITH_SPELL_KNOWN_SI
(spell name, required). Then cast at the corpse.

Wowee had cast the node spells (OPEN_LOCK) at herb and ore corpses, which the
server refuses, mapped engineering corpses to Mining, checked no skill and
said "Requires Skinning" when none was known.

## 9. Fishing

- Fishing (7620...) is a channel; the server places the bobber (FISHINGNODE,
  type 17, reach 100, usable only by its owner - interactions.md).
- The bite: AzerothCore's bobber turns ready 5 s before it despawns, sends the
  GO's values and SMSG_GAMEOBJECT_CUSTOM_ANIM with its anim progress (0).
  Stock (0x70BE90) plays custom animation `n` for n < 4 on any object (state
  table 0xA335E8: 8..11, the model's Custom0..3); Wowee plays Custom0 (153)
  and the "Fishing Hooked" sound for its own bobber.
- Clicking it: CMSG_GAMEOBJ_USE; in time -> loot (LootFrame plays FISHING
  REEL IN), too early -> **SMSG_FISH_NOT_HOOKED -> ERR_FISH_NOT_HOOKED** "No
  fish are hooked.", too late -> **SMSG_FISH_ESCAPED -> ERR_FISH_ESCAPED**
  "Your fish got away!" (0x6E2E90 cases 0x1C8 / 0x1C9), **yellow info lines**.
  Wowee had raised both as red errors with words of its own ("Your fish got
  away." for not hooked, "Your fish escaped!").

## 10. Cooking fires, tools, enchanting, lockpicking, first aid

- Spell focus (Cooking Fire, Anvil, Forge): GetTradeSkillTools lists it as
  present always; the cast check (0x80DA40 -> 0x8091D0) or the server refuses
  with SPELL_FAILED_REQUIRES_SPELL_FOCUS "Requires %s" (stock cast failure
  text, already in `stockCastFailureText`).
- Tools: Totem items (Blacksmith Hammer, Mining Pick) by carried count, tool
  categories by TotemCategory.dbc type and mask (a Gnomish Army Knife serves
  all of its kinds) - the item query's TotemCategory is now kept for it.
- Enchanting: recipes under the "Enchant" header with altVerb "Enchant" (the
  frame then hides Create All and the count); the cast arms the item-target
  cursor (items.md §3). Disenchant: items.md.
- Lockpicking: the same lock path; Pick Lock's line is Lockpicking (633),
  which AzerothCore keeps at level x 5 (SKILL_RANGE_LEVEL).
- First Aid, Cooking: ordinary item recipes ("Bandage" / "Food & Drink"...).
- Archaeology: not in 3.3.5.

## Tests

ctest: `test_stock_trade_skill` (bands, slot bits, headers and order, the
pending list, folding and the subclass filter across a rebuild, slot /
makeable / skill-up / name / level filters, selection by recipe, line-switch
reset, DoTradeSkill's run, link bits and parse).

In game (lab server, Wptestb - a level 10 dwarf hunter - with its own client
copy, `WOWEE_DEBUG_LUA` scripts in scratchpad `prof-tb/`, rows restored after):

- Tailoring at 40 (`.learn 3908` and six recipes): pressing Tailoring fires
  TRADE_SKILL_UPDATE then TRADE_SKILL_SHOW, no cast goes out; rows "Cloth"
  (Vest orange, Pants / Cloak yellow), "Miscellaneous" (two shirts yellow),
  "Cloth" (Bolt green, [10]); Create All box 1; the window drawn as stock
  (header "-" boxes, coloured rows, link button).
- Bolt: reagent Linen Cloth 2 / 20, NumMade 1 1, no tools, item link, recipe
  link `[Tailoring: Bolt of Linen Cloth]`, list link
  `trade:3908:40:75:9CE:5IAAAAAAgAAAAAAAg...`.
- Subclass filter 2 -> only "Miscellaneous"; slot filter 2 -> only the vest;
  Have Materials -> only the bolt; fold header 1 -> its rows gone, the box nil,
  the selection index followed the bolt from 9 to 6 and back. (A name typed
  below rank 75 is cleared by TradeSkillFrame_Update itself - stock FrameXML
  hides the box there.)
- Create 3 bolts with the frame's own button: three casts back to back,
  GetTradeskillRepeatCount 3 -> 2 -> ..., UPDATE_TRADESKILL_RECAST per craft,
  "Your skill in Tailoring has increased to 41.", 3 bolts, linen 20 -> 14, the
  counts redrawn. Create All (7): 6 left after one; StopTradeSkillRepeat -> 1,
  the cast in progress finishes, no more.
- The list link through SetItemRef: TRADE_SKILL_SHOW, IsTradeSkillLinked 1
  "Wptestb", the same rows at the link's rank, Create hidden.
- Pressing Tailoring with the window open closes it (TRADE_SKILL_CLOSE); the
  selection survives reopening.
- Peacebloom without Herbalism: "Requires Herbalism", nothing sent; learned:
  the Herb Gathering cast, loot, "...Herbalism has increased to 2". Tin Vein at
  Mining 1: "Requires Mining 65"; at 70: cast, 3 Tin Ore.
- Cooking, Spice Bread: GetTradeSkillTools "Cooking Fire", 1; Create away
  from a fire: "Requires Cooking Fire"; beside a temporary campfire: crafted,
  skill 2, [4] -> [3].
- Fishing at Crystal Lake: casts that miss water got "Face open water..." /
  "The water there is too shallow." (Wowee's wording, since replaced by the
  stock SPELL_FAILED_* lines); the bobber clicked 3 s in (before this
  change the click was dropped as "already casting"): **"No fish are hooked."**
  as an info line; clicked after the bite (anim progress 0 from AzerothCore,
  the splash and sound): the loot; left alone: **"Your fish got away!"** info.

## Gaps

- Skinning (§8) is built from the RE but was not exercised in game: the
  harness could not get a lootable corpse into melee reach (temporary NPCs
  despawn on death, `.die` leaves no loot rights).
- The search box's stat lines (ITEM_MOD_*, resistances, item spell triggers,
  sockets) are not in the name match; names, descriptions, reagents, tools and
  focus are.
- An open list is rebuilt on every bag update (each fires
  TRADE_SKILL_UPDATE); stock rebuilds on item count changes, which is close
  but not proven identical in count.
- CloseTradeSkill fires TRADE_SKILL_CLOSE only when a window was open (stock
  fires it on every call; the frame's OnHide calls it).
- A link's owner name is asked for but the list opens at once rather than
  after the name arrives; IsTradeSkillLinked answers nil for the name until
  then.
- The TRADE_SKILL_SHOW with headers depends on the made items' cache; while
  waiting the list shows nothing (stock shows nothing too, but keeps the old
  count).
- "Face open water" showed twice per refused fishing cast: AzerothCore sends
  two SMSG_CAST_FAILED for the one press (NOT_HERE, then
  BAD_IMPLICIT_TARGETS). Fixed since: one text per refused press, and the
  stock SPELL_FAILED_* lines (input-timing.md, "One press, two refusals").
- TargetNearestEnemy in the harness picked an unnamed unit ("Unknown") near
  Goldshire: see names-and-caches.md for the name pipeline's fixes.
