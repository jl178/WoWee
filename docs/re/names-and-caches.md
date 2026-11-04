# Names and the query caches

How the stock 3.3.5a client (build 12340) names units and objects: which query
it sends, when, where the answer is kept, what it shows until then and what it
tells the interface when the answer arrives. Read from `Wow.exe` with Ghidra;
every rule names the function it comes from. Wowee's differences and what was
changed follow in §5 and §6.

## 1. The caches (DBCache instances)

Every query answer lives in a `DBCache`, built by the static initializers at
0x9CB680..0x9CB7C0 (constructors 0x675C80, 0x675DB0, 0x676010, 0x676140,
0x676270, 0x6763A0, all alike). Constructor arguments, stored at:

| Offset | Meaning |
|---|---|
| +0x30 | four-character signature (the WDB file's first dword) |
| +0x34 | file name |
| +0x3C | request opcode |
| +0x44 | written to disk (Cache/WDB/<locale>/<file>) |
| +0x45 | kept when the world is left |
| +0x48 | most queries in flight: `n * 30000 / 60000`, 0 = no limit; more wait in a queue (0x6DED60) |

| Instance | Signature | File | Request | +0x44 / +0x45 | In flight |
|---|---|---|---|---|---|
| 0xC5D690 creatures | `WMOB` | creaturecache.wdb | CMSG_CREATURE_QUERY 0x60 | 1 / 1 | no limit |
| 0xC5D718 game objects | `WGOB` | gameobjectcache.wdb | CMSG_GAMEOBJECT_QUERY 0x5E | 1 / 1 | no limit |
| 0xC5D828 items | `WIDB` | itemcache.wdb | 0x56 | 0 / 1 | 256 |
| 0xC5D8B0 NPC text | `WNPC` | npccache.wdb | 0x17F | 1 / 1 | no limit |
| 0xC5D938 player names | `NAME` | namecache.wdb | CMSG_NAME_QUERY 0x50 | **0 / 0** | 128 |
| 0xC5D9C0 guilds | `DLGW` | guildcache | 0x54 | 0 / 0 | no limit |

- **Creature and game object records persist** across sessions and zones:
  the stock install has `Cache/WDB/enUS/creaturecache.wdb` and
  `gameobjectcache.wdb` (header `BOMW`, build 0x3034 = 12340, locale `SUne`).
  A name seen once is known at the next login before any query.
- **The name cache is neither written nor kept**: there is no
  `namecache.wdb`, and 0x635C70 (from the world-leave path 0x52B550) clears
  every cache whose +0x45 is clear (0x66CC90). Player names are asked for again
  each session.
- Response handlers are registered by 0x635B40: 0x61 -> 0x635190 -> 0x67B840
  (creatures), 0x5F -> 0x6351B0 -> 0x67BEE0 (game objects), 0x51 -> 0x6357D0
  (names).

### GetRecord (0x67B6A0 creatures, 0x67D770 names, 0x67EA30 pets, 0x67CA30 items)

`GetRecord(id, guid, callback, arg, ...)`:

- A valid record (+0x88 set for creatures, +0x178 for names): returned at once.
- A pending record: the callback is appended to its waiters (`AUDBCACHECALLBACK`
  nodes); **no second query is sent**.
- No record: one is made, and the query is sent (0x668970 / 0x668CE0) unless
  the in-flight limit is reached, in which case it is queued (0x6DED60).
  Returns 0.

Stock never re-asks a pending record on its own: a lost answer would stay
pending for the session.

### The answers

- **Creatures (0x67B840)**: `entry` with its high bit set is "not found": the
  waiters are called with success 0 and the record is deleted, so the next
  sight asks again. Otherwise the record is filled, marked valid and every
  waiter is called with success 1.
- **Names (0x6357D0)**: packed guid, status byte.
  - 0: name (0x30 bytes max), realm name (0x100), race, gender, class bytes,
    a declined-names flag and, when set, five declined names (0x40 each).
    Stored by 0x680C60, which calls every waiter with success 1.
  - 2: the record is put back in the queue (0x67A360 -> 0x6DED60): try again.
  - 3: stored with a placeholder name (`DAT_00A26F20`), realm, race etc. empty.
  - anything else (1, "unknown"): 0x67A2D0 calls the waiters with success 0
    and deletes the record.

## 2. When the queries are sent

- **Creatures, on create** (0x72D940, the unit's display set-up, also run on a
  display change by 0x73E410): for an object whose type field is 9 (a unit
  that is not a player), `GetRecord(entry, guid, 0x72CDE0)` and the record goes
  to `unit+0x964`.
- **Creatures, when OBJECT_FIELD_ENTRY changes**: 0x741D00 registers
  `0x4D5BA0(block 0, offset 0xC, 4, 0x72CEB0)`; 0x72CEB0 fetches the new
  entry's record with the same callback and, if it is cached, sets +0x964 and
  fires the events below at once.
- **Players, on first use**: there is no query at create. `GetUnitName`
  (0x72A000), `UnitName` (0x60E740), chat, the roster and the rest call
  `GetRecord` on the name cache with their callback, which sends the query the
  first time anything asks.
- **Pets, on first use**: a unit with `UNIT_FIELD_PETNUMBER` set (unit field
  +0x114) is named from the pet name cache by pet number (0x67EA30, callback
  0x728CA0); the record answers only while its +0x58 equals
  `UNIT_FIELD_PET_NAME_TIMESTAMP` (+0x118), else it is deleted and asked for
  again (CMSG_PET_NAME_QUERY with the pet number and guid).
- **Game objects, on create**, from the game object cache (0xC5D718).

## 3. What a unit is called until then

`CGUnit_C::GetUnitName` (0x72A000), in order:

1. A player (type flag 0x10): the name cache record (0x67D770, callback
   0x728CA0); its realm (record +0x34) when one is set.
2. A unit with a pet number: the pet name record, when its stamp matches.
3. A creature: the record at `unit+0x964`, name at +0x5C.
4. Otherwise `UNKNOWNOBJECT` ("Unknown"; "Unknown Being" if even that string
   is missing).

`UnitName(token)` (0x60E740): "player" is the player's own name. A token that
names no one (guid 0 or -2) returns **nil, nil**. A guid with no object: the
party roster's name (0x513CB0), else the pet or name cache (asking, callback
0x608370). An object: the unit's GetUnitName, a game object's name (0x70CDF0),
an item's, a corpse's owner. Then `UNKNOWNOBJECT`. Two returns: the name and
the realm.

## 4. When the answer arrives

The creature callback **0x72CDE0** (success 1, the unit still in view):

1. `unit+0x964` = the record; 0x725840 drops the unit's cached name text
   (+0xC38) so the name above its head is rebuilt.
2. **0x512B00(guid)**: the object's own refresh (0x512AB0, 0x715500), then
   **UNIT_NAME_UPDATE** (event 0x90) once for every token naming the unit
   (0x60BF10 -> 0x60BB70: player, pet, party1-4, partypet, raid, target,
   focus, mouseover, arena, boss, vehicle).
3. **UNIT_CLASSIFICATION_CHANGED** (0x94) for every token.
4. A totem slot naming the unit (0x5139B0): its name refreshed and
   PLAYER_TOTEM_UPDATE (0xA5) for the slot.

The player and pet name callback **0x728CA0** (success 1): steps 1, 2 and 4.
`UnitName`'s own callback 0x608370 does the same for a guid not in view.

Event ids are indexes into the table 0x608880 fills from 0xC24EB0
(UNIT_NAME_UPDATE at 0xC250F0).

## 5. Wowee before

| # | Wowee | Stock |
|---|---|---|
| 1 | Creature, game object and name queries were dropped unless the state was IN_WORLD; nothing re-asked creatures or objects. Anything created between CMSG_PLAYER_LOGIN and SMSG_LOGIN_VERIFY_WORLD could stay "Unknown" for the session | asked from the create on |
| 2 | A query whose answer never came stayed pending for the session (players were re-asked every 5 s only if not pending) | the same - but stock's answers are not lost to a dropped packet path |
| 3 | A creature answer set the name and fired no UNIT_NAME_UPDATE; UNIT_CLASSIFICATION_CHANGED went to one token only | UNIT_NAME_UPDATE then CLASSIFICATION for every token |
| 4 | A player name answer fired UNIT_NAME_UPDATE only for target, focus or player - a party or raid member, arena opponent or mouseover never heard it | every token |
| 5 | OBJECT_FIELD_ENTRY was read only in the create: a creature the server turned into another kept the first one's name, and an entry first seen in a values update was never asked about | 0x72CEB0 |
| 6 | Other players' pets (bots' hunter and warlock pets) were named by their template ("Wolf", "Imp"); the player's own pet was asked about with a private counter instead of its pet number | the pet name cache by pet number, "Unknown" until it arrives |
| 7 | No cache on disk: every session asked for every creature and object again. Within a session the caches did survive teleports and zones (cleared only at login and disconnect) | creaturecache.wdb, gameobjectcache.wdb |
| 8 | `UnitName` of a token naming nobody (no target) answered "Unknown", one value | nil, nil |

The reply handling itself was sound: an answer for a unit already out of range
still filled the cache (the next create of that entry used it), and
duplicates were not sent while one was pending.

## 6. Changes

- **Queries go out from CMSG_PLAYER_LOGIN on** (`EntityController::
  canSendQueries`: a socket and ENTERING_WORLD or IN_WORLD).
- **Re-asking** (`EntityController::resyncNames`, every 2 s from the update
  loop): a player, creature, pet or game object in view with no name is asked
  about; a query unanswered for 10 s (`name_pipeline::kQueryResendMs`) is sent
  again, with a warning in the log naming it. An entry the server said it does
  not have is not re-asked by the resync (stock deletes the record; the next
  create asks again). Not stock - stock never re-asks - but a lost answer
  otherwise leaves the unit "Unknown" for the session.
- **UNIT_NAME_UPDATE for every token** (`fireNameUpdate`, using
  `GameHandler::unitTokensForGuid`) on a player name, creature record or pet
  name arriving, and on an entry or pet stamp change that renames a unit;
  UNIT_CLASSIFICATION_CHANGED for every token after a creature answer and an
  entry change.
- **Entry changes** in a values update re-name the unit from the new record
  (or keep the old name until the answer, as 0x72CEB0 does when the record is
  not cached).
- **Pet names** (`queryPetName`, `handlePetNameQueryResponse`): any unit with
  `UNIT_FIELD_PETNUMBER` set is named from the pet name cache, keyed by pet
  number and checked against `UNIT_FIELD_PET_NAME_TIMESTAMP`; "Unknown" until
  it arrives; AzerothCore's empty answer (no such pet) falls back to the
  template name. The response handler moved from SpellHandler to
  EntityController; the player's own pet still gets PET_UI_UPDATE.
- **Caches on disk** (`name_pipeline.hpp`, `load/savePersistentCaches`):
  creature and game object records are written to
  `<user data dir>/cache/<host>_<port>_<realmId>/creaturecache.tsv` and
  `gameobjectcache.tsv` (`$HOME/.wowee/...`, `%APPDATA%\wowee\...` on Windows)
  when the world is left or the client disconnects, and every 60 s while there
  are new records; read back at character login. A file of another build is
  ignored. **Per realm, not per locale as stock**: Wowee talks to private
  servers whose templates differ. A record from disk is used at once and asked
  for once more the first time it is needed in the session, so a changed
  template is corrected (stock would keep the stale record until the WDB is
  deleted). The name cache is not written, as stock.
- **`UnitName`** of a token naming nobody returns nil, nil; otherwise a second
  return (realm, nil).

Tests: `tests/test_name_pipeline.cpp` (name source, pet stamp, resend rule,
realm key, both file formats round-tripped, damaged lines and other builds).

## 7. In game

Goldshire, the human paladin Wptestb, AzerothCore lab server:

- `UnitName("target")` with no target: nil, 2 returns (was "Unknown").
- Two temporary creatures (`.npc add temp 299`, `1922`): TargetNearestEnemy
  names them "Young Wolf" and "Gray Forest Wolf" at once. No query went
  unanswered in the run (no "asking again" lines).
- `.npc set entry 113` on the targeted wolf (Creature::UpdateEntry, a values
  update of OBJECT_FIELD_ENTRY): `UNIT_NAME_UPDATE target` with UnitName
  "Stonetusk Boar" 150 ms later (before: never renamed).
- Leaving the world wrote 42 creatures and 64 game objects to
  `$HOME/.wowee/cache/192.168.1.121_8085_1/`; the next login read them back
  ("42 creatures, 64 game objects") and a fresh Young Wolf was named by
  TargetNearestEnemy with no answer needed.
- The "Unknown" unit the harness once picked near Goldshire was not
  reproduced; the causes above (dropped pre-login queries, a lost answer, an
  entry change, a pet) are the ways the code could produce it.

## 8. Gaps

- The name-cache flight limit (128) and its queue are not modelled; Wowee
  sends every query at once.
- Name answers with status 2 (retry) and 3 (placeholder) are not told apart
  from "not found"; AzerothCore sends only 0 and 1.
- Declined names and the realm name are read past, not kept; `UnitName` gives
  nil for the realm always.
- `UnitName` does not answer game objects, items or corpses (0x60E740's other
  branches), and a guid with no object and no roster entry does not start a
  name query from `UnitName` itself (the resync and the handlers that ask for
  names cover the cases seen).
- `unitTokensForGuid` has no partypet, boss or vehicle tokens.
- The totem slot refresh (PLAYER_TOTEM_UPDATE on a name arriving) is not done.
- The name text above a unit is drawn each frame from the entity's name, so
  there is nothing to drop (0x725840).
