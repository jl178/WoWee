# PvP and instance flows against stock 3.3.5a (build 12340)

This audit covers:

- the battleground queue and the battlemaster;
- life inside a battleground: world states, the scoreboard, the spirit guide and leaving;
- arenas, Wintergrasp and world PvP;
- PvE instances: lockouts, difficulty, the bind prompt, boss frames and raid warnings.

Each was compared three ways: against what the stock FrameXML (which Wowee runs unchanged) expects, against AzerothCore's packet writers (`~/acore-lab/azerothcore-wotlk`), and, where those do not settle it, against `Wow.exe` 12340 read with Ghidra.

How it was done:

- `scratchpad/pvp/re/ophandlers_named.txt` maps every opcode to its `Wow.exe` handler. It was built by scanning for the `push handler; push opcode; call 0x6B0B80` registrations, then naming the opcodes from AzerothCore's `Opcodes.h`.
- Lua functions were found by name through the `{name, function}` tables, using `map-re/luafuncs.py`.
- Two read-only reviews ran beside the main pass: arena, Wintergrasp and world PvP in one, PvE instances in the other. Their findings were checked before anything was changed; §7 lists what was left.
- The rules are in `include/game/battlefield_rules.hpp`, a pure module tested by `tests/test_battlefield_rules.cpp`. The state lives in `SocialHandler`, and the Lua face is `src/addons/lua_battlefield_api.cpp`, registered after the other API sets. The bindings it replaced were removed from `lua_unit_api.cpp`, `lua_system_api.cpp` and `lua_social_api.cpp`.

## 0. Cross-cutting findings

### 0.1 Map.dbc was read at the wrong field

3.3.5's Map.dbc has 66 fields:

| Field | Contents |
|---|---|
| 0 | ID |
| 1 | Directory |
| 2 | InstanceType |
| 3 | Flags |
| 4 | PVP |
| 5 | MapName (17 locales) |
| 23 | MapDescription0, the Alliance text |
| 40 | MapDescription1, the Horde text |
| 65 | MaxPlayers |

Wowee read field 4 as the name, which caused two different failures:

- On most maps PVP is 0, so the string read was empty and the internal directory name was used instead. `GetInstanceInfo` answered "Ulduar80" and "Azeroth".
- On the battleground maps PVP is 1, which is the offset of "Azeroth" in the string block, so Warsong Gulch was named "Azeroth".

Every chat line that names a map was affected: instance reset, raid lock warnings and the loading screen. The name now comes from field 5, or field 4 in older files with fewer fields (`GameHandler::loadMapNameCache`). The Flags field is now kept too.

### 0.2 Instance types

`IsInInstance` (`0x5156A0`) and `GetInstanceInfo` (`0x51A8C0`) read Map.dbc's InstanceType through the table at `0xAC8074`: "none", "party", "raid", "pvp", "arena". Wowee answered "party" for every instance and "none" elsewhere. That made a battleground and a raid both "party", so every FrameXML branch that tests for "pvp", "arena" or "raid" took the wrong path.

### 0.3 PLAYER_ENTERING_BATTLEGROUND and the battleground reset on world entry

`0x528010` runs on every arrival in a world, whether login or teleport, and does two things:

- It calls `0x54CA50`, which empties the scoreboard, the battlemaster selection and both queue slots' status, then sends `CMSG_BATTLEFIELD_STATUS` (0x2D3) so the server restates the queues.
- If the map's InstanceType is 3 or 4 it fires `PLAYER_ENTERING_BATTLEGROUND`.

Wowee had three problems here:

- It fired the event only from the login path, never after the `SMSG_NEW_WORLD` teleport that is how a player actually enters a battleground.
- It decided from the battleground list, which left arenas out.
- It never asked for the statuses again.

`GameHandler::battlefieldWorldEntered` now does all of this and is called from both paths.

## 1. The battleground queue

Stock state, `CGBattlefieldInfo`:

- Two queue slots of 0x40 bytes at `0xBEA4B8`.
- The active battleground at `0xBEA564`..`0xBEA578`.
- The active slot at `0xACD16C`.

### 1.1 SMSG_BATTLEFIELD_STATUS (`0x54AE40`)

- **Slot range.** A slot greater than 1 is ignored.
- **Header and STATUS_NONE.** The eight bytes after the slot are read as one uint64 (arenaType, isArena, bgTypeId, 0x1F90) and kept whole. If they are zero, the packet is STATUS_NONE and the slot is emptied; if it was the active battleground, that ends too. AzerothCore writes STATUS_NONE as `uint32 slot, uint64 0` and nothing more.
- **Status 1, queued.** The average wait in ms, and the queue start computed as now minus the time already queued.
- **Status 2, invited.** The map, 8 unused bytes, and the invitation's timeout. The port expiration is set to now plus that timeout.
- **Status 3, in progress.** The map, 8 unused bytes, the auto-leave ms (shutdown at now plus that), the elapsed ms (start at now minus that), and the arena faction byte. This slot becomes the active one, and the scoreboard's columns and filter are reset.
- **Event.** Every accepted status fires `UPDATE_BATTLEFIELD_STATUS(slot+1)`.

| Bug | Fixed |
|---|---|
| STATUS_NONE dropped: the parser wanted seven more bytes after the eight zeros. A queue that was left, an invitation that was declined or a battleground that was quit stayed on the minimap and in `GetBattlefieldStatus` | yes |
| Status 3's timers and arena faction not read; `GetBattlefieldInstanceExpiration` and `GetBattlefieldInstanceRunTime` were constant 0; `GetBattlefieldArenaFaction` missing | yes |
| Three slots | yes (two) |
| Invented names ("Battleground #N", "2v2 Arena (Nagrand Arena)") | yes, see 1.2 |

### 1.2 The queue's Lua functions

| Function | Stock rule (address) | Wowee before |
|---|---|---|
| `GetBattlefieldStatus(i)` | Returns status ("none", "queued", "confirm", "active", or "error" for anything else) and mapName. mapName is the Map.dbc name for an invitation (except the random battleground's) and for an active battle; otherwise it is the BattlemasterList name. Then instanceID, minLevel, maxLevel, teamSize (arenaType & 0x7F), and registeredMatch as 1 or nil. An index other than 1 or 2 returns `nil, nil, 0,0,0,0, nil` (`0x54BC30`) | invented names; three slots |
| `GetBattlefieldPortExpiration(i)` | Whole seconds left, (expiry - now) / 1000 (`0x549B80`) | constant 0, so the popup's timer was dead |
| `GetBattlefieldTimeWaited(i)` | ms since the queue began (`0x549DD0`) | similar |
| `GetBattlefieldEstimatedWaitTime(i)` | ms (`0x549D30`) | correct |
| `GetBattlefieldInstanceExpiration()` | ms to the shutdown (`0x549C40`) | constant 0 |
| `GetBattlefieldInstanceRunTime()` | ms since the start (`0x549CD0`) | constant 0 |
| `IsActiveBattlefieldArena()` | isArena = the active map's InstanceType is 4; isRegistered = the active slot's rated flag; each 1 or nil (`0x549AD0`) | second return always nil |
| `GetBattlefieldArenaFaction()` | 1 or nil (`0x54A280`) | missing |
| `AcceptBattlefieldPort(i, accept)` | `CMSG_BATTLEFIELD_PORT` with the slot's eight header bytes and the answer. Declining the battleground the battlemaster's frame shows also fires `BATTLEFIELDS_CLOSED` (`0x54DA10`) | rebuilt the header; byte 2 was 0 instead of isArena |
| `LeaveBattlefield()` | `CMSG_LEAVE_BATTLEFIELD` with the active slot's eight header bytes (`0x54C250`) | **sent an empty packet**. AzerothCore's `HandleBattlefieldLeaveOpcode` reads eight bytes, so the packet was thrown away and the Leave Battleground button did nothing |

### 1.3 The battlemaster and the PvP frame

`SMSG_BATTLEFIELD_LIST` (`0x54E390`, AzerothCore `BuildBattlegroundListPacket`) carries, in order:

- the battlemaster guid
- the from-anywhere byte
- the type
- two level bytes
- hasWin, then winHonor, winArena and lossHonor
- isRandom; for the random battleground, a second reward block follows
- the instance count and the instance ids

The list then raises one event:

- `NPC_PVPQUEUE_ANYWHERE` for the random battleground from an NPC
- `PVPQUEUE_ANYWHERE_SHOW` when the list came from anywhere
- `BATTLEFIELDS_SHOW` when there is a battlemaster
- `BATTLEFIELDS_CLOSED` otherwise

Wowee read the type first, losing the guid and every reward, so nothing after it was right, and it never fired any of these events. A battlemaster's gossip therefore never opened BattlefieldFrame.

| Function | Stock |
|---|---|
| `GetBattlefieldInfo()` | name, the Map.dbc description for the player's side, maxGroup, then canEnter, isHoliday and isRandom as 1 or nil (`0x54D770`). canEnter comes from PvpDifficulty.dbc: a bracket on the map that contains the player's level; for the random battleground it is level 80 (`0x54BA00`) |
| `GetNumBattlefields()` / `GetBattlefieldInstanceInfo(i)` | the list's instances; nothing past the end |
| `Get/SetSelectedBattlefield` | the selection is kept as an instance id (`0x54BBD0`, `0x54BB40`) |
| `JoinBattlefield(i, asGroup, isRated)` (`0x54CC70`) | For an arena row (InstanceType 4): `CMSG_BATTLEMASTER_JOIN_ARENA` with guid, i-1, asGroup and isRated. Otherwise: `CMSG_BATTLEMASTER_JOIN` with guid, type, the i-th instance (0 without a battlemaster) and asGroup. It refuses a group larger than MaxGroupSize (`ERR_GROUP_JOIN_BATTLEGROUND_TOO_MANY`) and a non-arena queue from inside a battleground (`ERR_BATTLEGROUND_NOT_IN_BATTLEGROUND`) |
| `RequestBattlegroundInstanceInfo(i)` | `CMSG_BATTLEFIELD_LIST` with the type, from-anywhere 1, and the XP-off flag; this makes the type the selection (`0x54E5B0`) |
| `GetNumBattlegroundTypes` / `GetBattlegroundInfo(i)` | BattlemasterList rows of InstanceType 3, sorted once at start-up (`0x54E720` -> `0x54A650`): random first, then holiday, then MinLevel, then id. Returns name, canEnter, isHoliday (the row's HolidayWorldState is set), isRandom and id |
| `CanJoinBattlefieldAsGroup()` | the row's GroupsAllowed field (`0x54C6E0`) |
| `GetHolidayBGHonorCurrencyBonuses` / `GetRandomBGHonorCurrencyBonuses` | the list's two reward blocks plus a 0 (`0x54A300`, `0x54A370`). Wowee had answered constant 30/25/5 |
| `CloseBattlefield()` | fires `BATTLEFIELDS_CLOSED` and lets the NPC go |

Other fixes here:

- `SMSG_GROUP_JOINED_BATTLEGROUND`'s name now comes from BattlemasterList.
- The ImGui queue indicator reads the new state.

## 2. Inside a battleground

### 2.1 World states (`0x526530` cases 0x2C2, 0x2C3, 0x4F7)

**`SMSG_INIT_WORLD_STATES`**

- Stores the map, zone and area. A change rebuilds the WorldStateUI line list (`0x548970` -> `0x5487D0`).
- Sets each pair without clearing the others (`0x549440`).
- Fires `UPDATE_WORLD_STATES` only when something changed.
- Wowee used to clear every value and never fire the event.

**`SMSG_UPDATE_WORLD_STATE`** sets the value and fires `UPDATE_WORLD_STATES`. World state 3191 (0xC77, the arena season) also fires `ARENA_SEASON_WORLD_STATE` when it changes.

**`SMSG_WORLD_STATE_UI_TIMER_UPDATE`** keeps the server clock as an offset from the local one (`0x5486E0`; `0x548700` adds it back). It fires no event. Wowee had invented a `WORLD_STATE_UI_TIMER_UPDATE` event.

**`GetNumWorldStateUI` / `GetWorldStateUIInfo(i)`** (`0x548720`, `0x548D40`) come from WorldStateUI.dbc. A row is listed when all of these hold:

- its MapID is -1 or the current map
- its AreaID is 0, the zone or the area
- its PhaseShift is 0 or matches the player's phase
- its Type is 0, 1 or 3

The returns, in order:

- uiType
- state: the row's StateVariable world state, or 1 when it has none
- text, with world states filled in by `0x5770D0`: `%<id>w` is the value; `%<id>k` is the time left to the value read as a server timestamp, written MM:SS, or H:MM:SS from one hour up
- icon, dynamicIcon, tooltip, dynamicTooltip, extendedUI
- the three ExtendedUI world states

These three values drive the capture bars.

Wowee's version was invented: an Alliance/Horde score line per map from a hand-written table (`bg_score_defs.hpp`, now removed). Capture bars, Wintergrasp, the world PvP towers and every other line were missing.

WorldStateUI.dbc's 3.3.5 layout:

| Field | Contents |
|---|---|
| 0 | ID |
| 1 | MapID |
| 2 | AreaID |
| 3 | PhaseShift |
| 4 | Icon |
| 5 | String |
| 22 | Tooltip |
| 39 | StateVariable |
| 40 | Type |
| 41 | DynamicIcon |
| 42 | DynamicTooltip |
| 59 | ExtendedUI |
| 60..62 | ExtendedUIStateVariable |

### 2.2 The scoreboard: MSG_PVP_LOG_DATA (`0x54D280`)

AzerothCore's `BuildPvPLogDataPacket` writes:

- the type byte
- for an arena: both teams' rating blocks (three uint32 each), **then** both team names
- ended, plus the winner when ended
- the row count, then the rows

A battleground row is guid, KB, HK, deaths, honor, damage, healing, then the objective count and its values. An arena row is guid, KB, a team byte, damage, healing, then the objectives.

Wowee read the arena header as name-after-numbers for each team in turn, with five uint32 per team, so every arena scoreboard was misparsed.

The stock client:

- **Waits for names.** It holds `UPDATE_BATTLEFIELD_SCORE` until every row's name has come back from the name cache (`0x54CA30`).
- **Battleground faction.** It sets each row's faction from the race's FactionTemplate group: Horde 0, Alliance 1 (`0x54AA30`). The packet has no team in a battleground.
- **Counting and sorting.** It counts the rows that pass `SetBattlefieldScoreFaction`, sorts, and fires `UPDATE_BATTLEFIELD_SCORE`.

The sort (`0x54A740`, `0x54CEE0`):

- There are seventeen keys: kills, deaths, cp, name, class, hk, damage, healing, team, stat1..8. They start in that order.
- `SortBattlefieldScoreData(name)` moves that key to the front, keeping its own direction. Choosing the front key again reverses it.
- Numbers sort high first, names `strcmp` ascending, team Horde first.
- With a faction filter, that faction's rows come first.
- `GetBattlefieldScore(i)` indexes the sorted list. `GetNumBattlefieldScores()` is the filtered count.

| Function | Stock |
|---|---|
| `GetBattlefieldScore(i)` | name, KB, HK, deaths, honor, faction, rank 0, race, class, classToken, damage, healing. Before, class and race were never filled, and classToken was always nil |
| `GetNumBattlefieldStats` / `GetBattlefieldStatInfo(i)` | the battleground map's WorldStateUI rows of Type 2 (the first run of them), returning text, icon and tooltip. Wowee had a hand-written table; the stock text is "Flag Captures", "Flag Returns" and so on, with the column icons |
| `GetBattlefieldStatData(row, col)` | the row's objective value |
| `GetBattlefieldTeamInfo(0/1)` | name and the three numbers raw (`0x54A180`); the frame subtracts them itself |
| `GetBattlefieldWinner()` | nil until the battle has ended |

### 2.3 The area spirit guide (`0x524010`, every frame)

**Choosing a guide.** While the player is a ghost, the client picks the nearest unit with `UNIT_NPC_FLAG_SPIRITGUIDE` (0x8000) within 20 yards (distance² ≤ 400) and sends `CMSG_AREA_SPIRIT_HEALER_QUERY` (0x2E2) for it. Past 22 yards (484) the guide is let go; if a wave was counting down, the Waiting to Resurrect aura (2584) is cancelled, which leaves the queue (`0x523EB0`, `0x51F710`).

**The time packet.** `SMSG_AREA_SPIRIT_HEALER_TIME` (`0x524B10`) counts only for that guide and only when its time is positive. It sets the wave time and fires `AREA_SPIRIT_HEALER_IN_RANGE`.

**Lua functions:**

- `GetAreaSpiritHealerTime()` returns whole seconds (`0x516B90`).
- `AcceptAreaSpiritHeal()` sends `CMSG_AREA_SPIRIT_HEALER_QUEUE`.
- `CancelAreaSpiritHeal()` removes aura 2584.

Wowee asked only on a right-click, so the dialog never came up on its own. Its time never counted down, and Cancel did nothing.

## 3. Arena, Wintergrasp, world PvP

| Item | Stock | Status |
|---|---|---|
| `GetCurrentArenaSeason` / `GetPreviousArenaSeason` | world states 3191 / 3901 (`0x5A2A40`, `0x5A2A70`). Wowee answered 0, which disabled every rated arena button | fixed |
| `SMSG_ARENA_TEAM_STATS` | fires `ARENA_TEAM_UPDATE` (`0x5A2D50`) | fixed |
| `SMSG_ARENA_TEAM_EVENT` | after its line, `ARENA_TEAM_ROSTER_UPDATE("1")` so the frame asks again (`0x6CC980`) | fixed |
| `GetWorldPVPQueueStatus(1)` | status, zone name, battle id, and seconds left on an invitation (`0x54C8A0`). The state is kept from the battlefield manager packets: entry invite = confirm (until the server time sent), queue response = queued or none, entered = none, ejected for reasons 1/2/4/5/7-11 = none (`0x549500`) | fixed |
| `CanQueueForWintergrasp()` | world state 4375 (`0x543600`) | fixed |
| `GetWintergraspWaitTime()` | while world state 3801 is set, the seconds to world state 4354 (a server time); nothing during the battle (`0x5435A0`) | fixed |
| `SMSG_PVP_CREDIT` (`0x50B6D0`) | No victim: `COMBATLOG_HONORAWARD`. A victim it can see, with honor ≥ 1: `COMBATLOG_HONORGAIN` with `PVP_RANK_<rank>_<side>`, or `_NO_RANK` for a negative rank. Otherwise nothing. Wowee always printed "You gain N honor points." | fixed |
| `SMSG_ZONE_UNDER_ATTACK` (`0x50C3C0`) | no event (3.3.5 has no `ZONE_UNDER_ATTACK` event; the string is GlobalStrings'). The formatted line goes to WorldDefense always, and to LocalDefense for the current zone. Wowee fired the event, which chatframe printed to every window | fixed (zone match is the zone or area id; the AreaTable parent is not walked) |
| `SMSG_DEFENSE_MESSAGE` (`0x50C850`) | zone, then a uint32 length and that many bytes. Wowee read a C string, so the text was lost | fixed |
| `PLAYER_PVP_KILLS_CHANGED("player")` | on PLAYER_FIELD_KILLS, TODAY_CONTRIBUTION or LIFETIME_HONORABLE_KILLS changing | fixed |
| `HONOR_CURRENCY_UPDATE` | then `CURRENCY_DISPLAY_UPDATE`, on the honor or arena currency changing | fixed |
| `GetPVPSessionStats` / `Yesterday` / `Lifetime` | from those fields; Lifetime returns 2 values, with the rank 0 below 5 (`0x611A20`, `0x611AD0`, `0x611B80`). Wowee returned zeros, and 3 values for Lifetime | fixed |
| `UnitIsPVP` / `UnitIsPVPFreeForAll` / `UnitIsPVPSanctuary` | UNIT_FIELD_BYTES_2 byte 1 bits 0x01 / 0x04 / 0x08, 1 or nil. Wowee read UNIT_FLAG_PVP and PLAYER_FLAGS 0x80; Sanctuary was missing | fixed |

## 4. PvE instances

### 4.1 Saved instances: SMSG_RAID_INSTANCE_INFO (`0x501030`)

AzerothCore's `SendRaidInfo` writes, per entry: map, difficulty, the instance's **uint64 id**, locked, extended, then the **seconds** to the reset. Wowee read the id as the reset time and the reset as part of the next entry, and never fired `UPDATE_INSTANCE_INFO`.

`GetSavedInstanceInfo(i)` (`0x4FF2C0`) returns:

- name
- the id's low dword
- the reset in seconds, as sent
- difficulty+1
- locked and extended (booleans)
- the id's high dword
- isRaid (Map InstanceType 2)
- maxPlayers (MapDifficulty.dbc)
- the difficulty's name (MapDifficulty's GlobalStrings key, e.g. `RAID_DIFFICULTY_10PLAYER`)

Wowee had guessed isRaid and maxPlayers from the difficulty number.

`GetInstanceInfo()` (`0x51A8C0`) returns:

- name
- type
- difficulty+1
- the MapDifficulty name
- maxPlayers
- the dynamic-difficulty flag (the second uint32 of `SMSG_INSTANCE_DIFFICULTY`)
- isDynamic (Map flag 0x100)

### 4.2 Difficulty

The stock client keeps these values:

| Address | Value |
|---|---|
| `0xBD0894` | the instance's difficulty |
| `0xBD1980` | its dynamic flag |
| `0xBD0898` / `0xBD1978` | the dungeon setting, personal and group copies |
| `0xBD089C` / `0xBD197C` | the raid setting, personal and group copies |

**`SMSG_INSTANCE_DIFFICULTY`** stores the first two silently. AzerothCore sends it at every login, so it says nothing about being inside an instance.

**`MSG_SET_DUNGEON_DIFFICULTY` and `MSG_SET_RAID_DIFFICULTY`** (difficulty, a personal flag, a group flag; `0x525530`, `0x5255A0`):

- Each updates the copies its flags name.
- If the setting in force changed, it prints `ERR_DUNGEON_DIFFICULTY_CHANGED_S` or `ERR_RAID_DIFFICULTY_CHANGED_S`. Inside a raid map the raid line uses the MapDifficulty name.
- The setting in force is the group copy in a party (dungeon) or in a raid (raid), and the personal copy otherwise.

**`Set{Dungeon,Raid}Difficulty(n)`** (`0x526050`, `0x5261A0`):

- Ranges are 1-2 for a dungeon and 1-4 for a raid.
- A group member who is not the leader gets `ERR_NOT_LEADER`.
- Otherwise both copies change at once, the line is printed, and then the request is sent.

**The getters** (`0x515790`, `0x515810`): `GetDungeonDifficulty` and `GetRaidDifficulty` return the setting in force and the personal setting, each +1.

What Wowee did wrong:

- It kept one variable for all of this.
- It had no raid handler.
- It set "in an instance" from `SMSG_INSTANCE_DIFFICULTY`.
- Its two getters each returned one value, and both returned the instance's difficulty: inside a 25-man heroic, `GetDungeonDifficulty()` answered 4.
- It fired `PLAYER_DIFFICULTY_CHANGED` and `UPDATE_INSTANCE_INFO`, which stock does not fire here.

### 4.3 The bind prompt and resets

**The prompt** is `0x513B20`: a positive time fires `INSTANCE_LOCK_START`, zero fires `INSTANCE_LOCK_STOP`.

- `SMSG_INSTANCE_LOCK_WARNING_QUERY` calls it with the packet's time.
- `SMSG_INSTANCE_SAVE_CREATED(0)` prints `INSTANCE_SAVED` and calls it with zero (`0x52BB10`), so a bind the server makes itself takes the dialog down.
- `RespondInstanceLock` always sends its answer and fires nothing (`0x613ED0`).
- The timer running out fires nothing.
- `GetInstanceLockTimeRemaining` returns whole seconds.

Wowee started the prompt even for a zero time, left it up after a save, refused to answer once its own timer had run out, and fired STOP itself.

**`SMSG_RESET_FAILED_NOTIFY`** prints the GlobalString `RESET_FAILED_NOTIFY` as a chat line (`0x50CEE0`). Wowee printed invented English text as a red error.

### 4.4 Boss frames: SMSG_UPDATE_INSTANCE_ENCOUNTER_UNIT (`0x5EDDD0`)

The stock client keeps up to sixteen units, packed and sorted by frame priority, then by engage order (`0x5ED750`). `bossN` is the N-th unit (`0x5ED710`).

| Type | Stock behaviour |
|---|---|
| Engage | a unit already present changes nothing and fires nothing |
| Disengage | the unit is removed and the rest close up |
| Update-priority | applies only to a unit already present |
| Refresh | re-sorts |

`INSTANCE_ENCOUNTER_ENGAGE_UNIT` follows each change.

Wowee instead used the priority as a fixed slot, left holes when a unit disengaged, engaged unknown units on a priority update, and had only five slots.

### 4.5 Raid warnings and boss emotes

The event names, arguments and the absence of a client line were already right. The colours were not. `UPDATE_CHAT_COLOR` used an invented palette, which left `RAID_BOSS_EMOTE` and `RAID_BOSS_WHISPER` white and had MONSTER_SAY, MONSTER_WHISPER, the BG_SYSTEM_* types and RAID_LEADER wrong. The palette is now the clean client's own COLORS block (`WTF/.../chat-cache.txt`): boss emotes are 255/221/0.

## 5. Tests

`tests/test_battlefield_rules.cpp` builds each packet in AzerothCore's byte order. It covers:

- STATUS_NONE, and the drop of slots past the second
- the invitation's countdown in whole seconds
- the in-progress timers and arena faction
- the list's layout and the event it raises
- the arena and battleground scoreboards
- the seventeen-key sort with its faction filter
- WorldStateUI line and column selection
- `%w` / `%k` text
- the encounter-unit ordering

## 6. In game

These runs used Wptestb (level 10 Alliance paladin, HOME `scratchpad/testhome2`) on the lab realm, which has playerbots. The test binary was `build-nix/bin/wowee-test-pvp`, driven by `WOWEE_DEBUG_LUA` scripts in `scratchpad/pvp/lua` (`bg.lua`, `inst.lua`, `wpvp.lua`). Logs are in `scratchpad/pvp/logs` and screenshots in `scratchpad/pvp/shots`.

**Warsong Gulch (`bg.lua`)**

- **Setup.** `.debug bg` was switched on for the run and off again at the end ("Battlegrounds are set to normal playercount.").
- **Joining from the PvP frame.** `RequestBattlegroundInstanceInfo` led to `PVPQUEUE_ANYWHERE_SHOW`. `GetBattlefieldInfo` gave Warsong Gulch, the Alliance description, 10, 1. The holiday rewards read 465/25/78.
- **Queued.** `JoinBattlefield(0)` led to `UPDATE_BATTLEFIELD_STATUS 1` and "queued", "Warsong Gulch", 0, 10, 80, 0. The minimap battlefield button was shown, parented to Minimap.
- **Invited.** "confirm", "Warsong Gulch", 1, 10, 19, 0. `GetBattlefieldPortExpiration` counted 58, then 45. The popup read "You are now eligible to enter Warsong Gulch 1" (`shots/bg/02_invite.png`).
- **Entering.** `AcceptBattlefieldPort(1, 1)` led to `PLAYER_ENTERING_WORLD`, then `PLAYER_ENTERING_BATTLEGROUND`, then "active".
  - `IsInInstance` gave 1, "pvp". `GetInstanceInfo` gave "Warsong Gulch", "pvp".
  - `GetBattlefieldArenaFaction` gave 1. `GetBattlefieldInstanceRunTime` gave 631953 (the bots' game was ten minutes in).
- **World state lines.** There were three:
  - "Time Remaining: 17 min."
  - "0/3" with the Horde flag dynamic icon and the state 2 while the Alliance flag was carried
  - "1/3"
- **Scoreboard.** It had twenty rows with names, races, classes and class tokens, and the two columns "Flag Captures" and "Flag Returns" with their icons (`shots/bg/04_score.png`). Sorting by name reordered it.
- **Spirit guide.** After `.die` and `RepopMe`, `AREA_SPIRIT_HEALER_IN_RANGE` came on its own. `GetAreaSpiritHealerTime` counted 11, then 9; the dialog read "Resurrection in 9 Seconds" (`shots/bg/06_spirit.png`). The wave resurrected the character.
- **Leaving.** `LeaveBattlefield` led to `UPDATE_BATTLEFIELD_STATUS 1`, then the transfer, then "none" in both slots, then `IsInInstance` nil, "none".
- **Before the fixes**, the same run showed a "Group 1..8" grid on screen in the battleground. This is the FrameXML emitter bug in the next section, now fixed.

**Instances and difficulty (`inst.lua`)**

- **Open world.** `GetInstanceInfo` gave "Eastern Kingdoms", "none" (it used to give "Azeroth").
- **Difficulty settings.** `SetDungeonDifficulty(2)` printed "Dungeon Difficulty set to 5 Player (Heroic)." and `GetDungeonDifficulty` gave 2, 2. Setting it back printed "... 5 Player.". `SetRaidDifficulty(2)` and `(1)` printed "Raid Difficulty set to 25 Player." and "... 10 Player." The server's echoes printed nothing more.
- **Deadmines.** `IsInInstance` gave 1, "party". `GetInstanceInfo` gave "Deadmines", "party", 1, "", 5, 0, false. `RequestRaidInfo` led to `UPDATE_INSTANCE_INFO`.
- **Arena season.** `GetCurrentArenaSeason` gave 1 and `GetPreviousArenaSeason` gave 8.

**World PvP (`wpvp.lua`)**

- **Eastern Plaguelands at Crown Guard Tower.** The two "Controlled: 0" tower lines had their icons, plus the CAPTUREPOINT line (50, 100) (`shots/wpvp/01_epl.png`).
- **Silithus.** The two "0/200" silithyst lines.

**Not reached in game**

- **Wintergrasp, Naxxramas and Outland.** The lab's progression module refuses Northrend and Outland to a character at Wptestb's tier ("Progression Level Required = 13"). The level 80 character, Wptester, was in use by another agent's client.
- **Arena.** A skirmish queue needs a battlemaster.
- **Boss frames.** The Deadmines sends no encounter frames.

All four are covered by the unit tests.

### 6.1 A FrameXML engine bug found here

**The bug.** A top-level frame declared without `parent=` takes its template's `parent=` in WoW. `RaidGroup1..8` are declared bare in Blizzard_RaidUI and inherit `RaidGroupTemplate parent="RaidFrame"`.

**Cause.** The emitter hangs a parentless top-level frame on UIParent and then treated that as the frame's own parent, so the template's parent was never applied. The raid group panes drew on screen as soon as Blizzard_RaidUI loaded, which joining any battleground's raid does.

**Fix.** Frames given UIParent by default are now marked (`__WoweeDefaultParent`), and a template's `parent=` applies to them (`framexml_emitter.cpp`).

## 7. Gaps

- **Arena opponents.** These are not implemented: the `arena1-5` / `arenapet1-5` tokens, `ARENA_OPPONENT_UPDATE` (seen / unseen / destroyed / cleared), `GetNumArenaOpponents`, and `SMSG_ARENA_UNIT_DESTROYED` (`0x54B5E0`, `0x54D100`, `0x54ABF0`). Blizzard_ArenaUI's enemy frames stay empty.
- **Arena team data.**
  - `CMSG_ARENA_TEAM_QUERY` is never sent. Stock sends it when PLAYER_FIELD_ARENA_TEAM_INFO changes and on a `GetArenaTeam` cache miss, and AzerothCore sends stats only in reply to it.
  - `GetArenaTeam` is indexed by arrival instead of by slot, and has no tabard.
  - `IsArenaTeamCaptain` is hard-coded.
  - `GetArenaTeamRosterInfo`'s rank, level and class are not filled.
  - `MSG_INSPECT_ARENA_TEAMS` is misparsed and never requested.
  - The review's addresses are `0x5A3860`, `0x5A3700`, `0x5E7B00` and `0x60DC70`.
- **GetZonePVPInfo** does not follow stock (`0x51BA50`): there is no subzone flag pass, no "combat", and the realm's PvP type is ignored. `IsPVPTimerRunning` and `GetPVPTimer` are stubs.
- **Difficulty.**
  - The two lines of a dynamic-difficulty raid (Map flag 0x100: ERR_RAID_LOCKOUT_CHANGED_S and ERR_PLAYER_DIFFICULTY_CHANGED_S) are not printed.
  - `SMSG_CHANGEPLAYER_DIFFICULTY_RESULT` case 6 is not handled. AzerothCore does not send it.
- **Instance resets.** `SMSG_UPDATE_LAST_INSTANCE` and `SMSG_UPDATE_INSTANCE_OWNERSHIP` are skipped and `CanShowResetInstances` is missing. FrameXML does not call it.
- **Bind prompt.** `GetInstanceLockTimeRemaining`'s total and completed counts still come from Wowee's encounter table, not from stock's `0x553830`.
- **AcceptBattlefieldPort** does not make the client-side refusals stock makes before sending (deserter, dead and similar checks in `0x54DA10`). The server refuses instead.
- **AREA_SPIRIT_HEALER_OUT_OF_RANGE.** No place in `Wow.exe` was found that raises it, so it is not raised.
- **LocalDefense.** The zone match compares the attack's area with the current zone or area. It does not walk AreaTable parents.
- **Scoreboard sort, equal values.** On equal class or stat values, stock's comparator answers "after" rather than "equal", so its qsort order there is unstable. Wowee treats them as equal and falls through to the next key.
- **Battleground flag carriers.** `MSG_BATTLEGROUND_PLAYER_POSITIONS`' flag-carrier order and `GetBattlefieldVehicleInfo` were not reviewed.
- **Seen in the battleground but outside this audit.**
  - The party frames of the battleground raid showed "Unknown" and "Dead" for the bots in the player's subgroup.
  - One nameplate's name was drawn very large.
