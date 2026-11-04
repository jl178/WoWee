# Chat and the combat log

## Chat

Wow.exe 3.3.5a (12340), read with Ghidra; compared with FrameXML
(`ChatFrame.lua`) and AzerothCore (`ChatHandler::BuildChatPacket`,
`WorldSession::HandleMessagechatOpcode`, `Player::Whisper`). The pure rules are
in `include/game/chat_rules.hpp`, the packet layout in
`MessageChatParser::parse` (`src/game/world_packets_social.cpp`), the flow in
`ChatHandler` (`src/game/chat_handler.cpp`).

### 1. SMSG_MESSAGECHAT / SMSG_GM_MESSAGECHAT (0x50BE70)

One handler; the second argument says GM message. Common head: `uint8 type,
int32 language, uint64 sender, uint32 flags`. Then by type:

| Types | Fields |
|---|---|
| 0x0C-0x10, 0x29, 0x2A, 0x2F (monster/boss, BN) | `uint32 len, name` (sender); `uint64 receiver`; when the receiver is neither a player (high nibble 0, non-zero) nor a pet (`hi & 0xF0F00000 == 0xF0400000`): `uint32 len, receiver name` |
| 0x08 WHISPER_FOREIGN | `uint32 len, name`, `uint64 receiver`; the client then treats it as type 7 |
| 0x24-0x26 BG_SYSTEM_* | `uint64 guid`; a name when the guid is not a player. **The type stays** - FrameXML prints `CHAT_MSG_BG_SYSTEM_*` |
| 0x30, 0x31 achievement | `uint64 receiver`; after the tag `uint32 achievement` |
| everything else | GM message only: `uint32 len, GM name`; channel (0x11) only: channel name (C string); `uint64 receiver` |

Then `uint32 length, text` (read as a C string - it ends at its first NUL),
`uint8 chat tag`. Before the line goes on:

- Whisper (7, 0x2F): the sender goes to the front of the sixteen recent
  whisperers (0x4FB710). Whisper inform (9): the receiver into a 128-entry
  list (0x4FB820).
- Ignore (0x6B52E0, 50 entries): a line from an ignored sender is dropped
  unless the tag has GM (4); for a whisper not in the addon language the
  client sends `CMSG_CHAT_IGNORED` (`uint64 guid, uint8 0`, 0x500180). The
  only types that still show are WHISPER_INFORM, IGNORED and RESTRICTED.
  Monster lines are checked against the *receiver*. BG and achievement lines
  are not checked.
- A whisper with tag 0x08 from someone not in the whispered-to list and not a
  friend is refused with `CMSG_CHAT_IGNORED(guid, 1)` and dropped (not
  implemented: AzerothCore sets 0x08 only for commentators).

The flag word, arg6 (0x50BE70): tag 0x10 "DEV", else 0x04 "GM", else 0x02
"DND", else 0x01 "AFK", else "". `chat_rules::chatFlagName`.

### 2. The line (0x509DD0)

In this order:

1. **Names.** A player sender with no name is looked up in the name cache
   (async, callback 0x503C80), a pet's in the pet-name cache; the same for the
   receiver (which, for AzerothCore's say/yell/whisper, is the sender again).
   If anything is already queued (0xAC7A38) or a name is missing, the line is
   queued (0x502D00) - **every later line waits behind it**, so the order is
   the server's. Wowee: `ChatHandler::queueChatLine` / `pumpChatQueue`, with
   a one-second give-up.
2. **Addon traffic** is language -1 and nothing else: the text splits at its
   first tab (no tab: all prefix, empty body) and `CHAT_MSG_ADDON(prefix,
   body, dist, sender)` fires with dist PARTY (2), RAID (3), GUILD (4),
   WHISPER (7, 8, 0x2F), BATTLEGROUND (0x2C), else UNKNOWN - officer, raid
   leader and party leader are UNKNOWN. Nothing else happens to it.
3. **UTF-8**: a line that is not valid UTF-8 (0x7E0EC0 over the decoder
   0x47BD40) is dropped.
4. **$-tokens** for monster/boss, BG system and achievement types (0x579590 ->
   0x578F20). `$a` is the achievement link (0x61BFF0):
   `|cffffff00|Hachievement:%d:%016I64X:%d:%d:%d:%d:%u:%u:%u:%u|h[%s]|h|r`
   with the receiver's guid, completed 1 and the date.
5. **Talking animation** for say (1), yell (6), party (2) and party leader
   (0x33) from a unit in the world that is not feigning death
   (UNIT_FIELD_FLAGS 0x20000000): if the whole line is one of
   `LAUGH_WORD1..n` (case-insensitive) the laugh; otherwise say only: '?' at the
   end question, '!' exclamation, else talk; yell shout. The emote is the first
   Emotes.dbc row with flag 0x8 / 0x10 / 0x20 / 0x40 / 0x100 (0x7165D0): emotes
   1, 6, 5, 22, 11 (0x73AD70).
6. Language forced to 0 for EMOTE (0x0A), MONSTER_EMOTE (0x10), SYSTEM (0) and
   RAID_BOSS_EMOTE (0x29).
7. **Empty text**: say -> `CHAT_SAY_UNKNOWN`, yell -> `CHAT_YELL_UNKNOWN` (the
   `_FEMALE` form for a female sender), both become type EMOTE; emote ->
   `CHAT_EMOTE_UNKNOWN`.
8. **Quest links** are recoloured for the reader: the ten bytes before every
   `|Hquest:` not preceded by `|` become the difficulty colour of the link's
   level (-1 = the reader's), 0x61E540: diff > 4 `ff2020`, > 2 `ff8040`,
   >= -2 `ffff00`, else grey `808080` when the player is more than
   GetQuestGreenRange levels above, otherwise green `40c040`.
   **GetQuestGreenRange** (0x5E2860) is a table by level / 5 (0xA1C4A0):
   4 4 5 5 6 6 7 7 then 8 - not AzerothCore's grey-level formula, which the
   Lua binding used before.
9. **Language** (0x505930): a reader with PLAYER_FLAGS 0x8 (GM) sees every
   line as sent, language 0. Otherwise comprehension is the skill of the
   language (0x6E0640: the language spell's SkillLineAbility skill, value plus
   bonus; no spell -> 0, unless an aura 244 COMPREHEND_LANGUAGE names it ->
   300). Language 0 or comprehension >= 300: untouched. Otherwise:
   - markup stripped (0x7E1680): `|cXXXXXXXX`, `|r`, `|H...|h` and the
     brackets of the link text, `|T...|t`;
   - each run of separators (not a letter 0x7E0F30, digit, `'`, or a code point
     >= 0x100) becomes **one space**;
   - each word: `h = SStrHash(word)` (0x76F340, case-folded, seed 0x7FED7FED,
     table 0xA39538); kept when `h % 300 < comprehension`; else replaced by
     LanguageWords.dbc's words for that language of `min(bytes, 18)` bytes -
     the one at `h % count`, or the next shorter length that has any; none at
     all drops the word (table built at 0x50EDD0, keyed language + strlen);
   - the replacement copies the original's case one code point at a time
     (0x4FB410): upper where the original is not lower case.
   Checked against the game: "lol" reads "kek" in Orcish and "bur" in Common.
   (zhCN/zhTW/koKR split words differently; not implemented.)
10. **Channel**: a joined channel becomes `"%d. %s"` (number, name); the
    profanity filter is applied to the channel name.
11. **Spam filter** (cvar `spamFilter`, 0x509BD0): not for FILTERED (0x2B),
    not for a GM or DEV flag, not for a GM reader, not for SYSTEM: the
    processed text against SpamMessages.dbc's 133 regular expressions, the
    verdict cached per text. A match drops the line; a whisper is reported
    with `CMSG_CHAT_FILTERED(guid)`.
12. **Profanity filter** (cvar `profanityFilter`) for the types at 0x9FA510
    (say, party, party leader, raid, raid leader, guild, officer, yell,
    whisper, whisper inform, emote, channel, AFK, DND, raid warning,
    battleground, battleground leader): ChatProfanity.dbc patterns replaced by
    `!@#$%^&*` cycling. **Not implemented** (5222 patterns).
13. **Line id**: a global counter, +1 per line (0xBCF000).
14. `CHAT_MSG_<type>` (0x4FDBC0, event id from 0x9FBE80), thirteen arguments
    `"%s%s%s%s%s%s%d%d%s%d%d%s%u"`:

| # | Value |
|---|---|
| 1 | text |
| 2 | sender name ("" when none) |
| 3 | Languages.dbc name of the language - **"" for 0** (the file has no row 0) |
| 4 | channel as "N. Name", or the raw name, or "" |
| 5 | receiver name |
| 6 | flag word (DEV/GM/DND/AFK) |
| 7 | the channel's ChatChannels.dbc id (0 for custom/none) |
| 8 | channel number (0) |
| 9 | channel name without the number (the stored one; "" when not a joined channel) |
| 10 | the channel's instance (0) |
| 11 | line id |
| 12 | the sender's guid as `0x%016X` - **players only**, "" for anything else |
| 13 | BN conversation id (0) |

15. Whisper (7, 0x35): the name goes to the reply list (0x84A530).
16. **Bubble** (0x720010) when the sender is in the world; monster say/yell have
    `%%` collapsed first.
17. The chat log file when LoggingChat.

A monster line's `%s` is *not* filled by the client - arg1 keeps it and
FrameXML formats `CHAT_MONSTER_*_GET..arg1` with the name. Wowee fills it only
in its own history for its own panel.

### 3. SendChatMessage (0x50D170)

`SendChatMessage(text [,type] [,language] [,target])`:

- Type word via 0x4FBB20: SAY 1, PARTY 2, RAID 3, GUILD 4, OFFICER 5, YELL 6,
  WHISPER 7, EMOTE 0x0A, CHANNEL 0x11, AFK 0x17, DND 0x18, RAID_WARNING 0x28,
  BATTLEGROUND 0x2C, BN 0x2F, BN_WHISPER 0x35... ; unknown -> Lua error
  "Unknown chat type".
- Empty text ends the call, except AFK and DND.
- Not UTF-8 -> "Chat message must be UTF-8 text".
- Language: the race's (ChrRaces BaseLanguage, 0x6D5E70); a given name is
  looked up in Languages.dbc ("Unknown language") and must be known, else
  ERR_CANT_SPEAK_LANGAGE.
- WHISPER without target -> "Whisper message missing target player!".
- PARTY outside a group -> ERR_NOT_IN_GROUP; the leader sends PARTY_LEADER
  (0x33).
- RAID_WARNING: in a party, fine; in a raid, leader or assistant only
  (0x572740) else ERR_NOT_LEADER; neither -> ERR_NOT_IN_GROUP.
- RAID / BATTLEGROUND outside a raid -> ERR_NOT_IN_RAID / ERR_NOT_IN_BATTLEGROUND;
  the leader sends RAID_LEADER (0x27) / BATTLEGROUND_LEADER (0x2D).
- CHANNEL: target is the channel *number*; no number -> Lua error; a number
  with no channel or a suspended one ends silently; the name goes out.
- AFK (0x6DC640 / 0x6D52D0): empty while away clears (CLEARED_AFK); otherwise
  marks away (CLEARED_DND first if busy, MARKED_AFK_MESSAGE, default
  DEFAULT_AFK_MESSAGE), language 0. Any other send clears away when
  `autoClearAFK` is on. DND: MARKED_DND / CLEARED_DND then the DND packet.
- Dead or ghost (player vfunc +0x128): only whisper, party, raid, raid leader,
  raid warning, guild, officer, channel, AFK, DND, battleground kinds;
  otherwise ERR_CHAT_WHILE_DEAD.
- Text: `%t`/`%n` target name, `%f` focus name (`TARGET_TOKEN_NOT_FOUND`,
  `FOCUS_TOKEN_NOT_FOUND`), 255 bytes (0x577D00); drunk slurring
  (0x577F60, "s" -> "sh", `SLURRED_SPEECH`) - not implemented; cut at the
  first `\r`, `\n`, `\\n` or `|n` (0x7E1640).
- Escapes: every `|` must be `||` or `|c`; a `|c` runs to its `|r` (links
  inside are not inspected); no `|r` cuts the line at the `|c`; anything else
  -> "Invalid escape code in chat message". Links are otherwise not validated
  by the client; AzerothCore validates them (`ValidateHyperlinksAndMaybeKick`).
- Throttles (not for addon traffic): a whisper to someone not among the
  sixteen recent whisperers, and a channel line, use rings of ten timestamps
  (whispers 0x4FB8F0, per channel 0x502F60/0x5097D0): the eleventh within
  ten seconds is **held** (`THROTTLEDCHAT`, 0x502FA0) and sent later
  (0x5098B0, each held line in order).
- Packet: `CMSG_MESSAGECHAT uint32 type, uint32 language, [target C string for
  whisper and channel], text`.

### 4. Channels

`SMSG_CHANNEL_NOTIFY` (0x50E120) -> `CHAT_MSG_CHANNEL_NOTICE` /
`CHAT_MSG_CHANNEL_NOTICE_USER` with the notice token in arg1, the channel as
"N. Name" in arg4, number in arg8, base name in arg9 (see the existing handler
for each notify type). They take line ids like any line.
ZONE_UNDER_ATTACK (0x50C3C0) posts to every joined channel whose DBC flags have
0x10000 and either lack 0x2 or match the zone. Joining by name (0x507A80): a
ChatChannels.dbc short name ("General") is expanded with the row's pattern and
the zone name (for flag 0x20 in a city area, the city's name), and
`CMSG_JOIN_CHANNEL uint32 dbcId, uint8, uint8, name, password` goes out.
The default chat window gets the rows with flag 0x1 (0x501F00).

### 5. Wowee changes

- Parser: BG system lines keep their type and read their guid (they were turned
  into SYSTEM and read one field short); WHISPER_FOREIGN and the BN type are
  read; a GM message's name is read rather than cut out; the text ends at its
  terminator.
- Removed as non-stock: the "queue announcer" filter, the officer-chat filter,
  the addon-prefix whisper filter, the client-side AFK/DND auto-reply (the
  server answers for an away player), the repeat-line spam filter (replaced by
  SpamMessages.dbc), the tab-envelope guess for addon traffic in a spoken
  language.
- Added: ordered name queue, ignore drop + CMSG_CHAT_IGNORED, UTF-8 drop,
  language scrambling, GM reader, empty-speech emote, quest-link colour,
  talking animation, spam filter + CMSG_CHAT_FILTERED, the thirteen event
  arguments (arg3 "" for language 0, arg12 only for players, DEV>GM flag
  order), CHAT_MSG_ADDON's distribution words, SendChatMessage's checks and
  throttles, GetQuestGreenRange's table.

### 6. Commands, emotes, filters, zone channels (second pass)

Read with Ghidra the same way; the pure rules are in
`include/game/chat_social_rules.hpp` (`chat_rules::`), pinned in
`tests/test_chat_rules.cpp`. Lua function table: `0xAD94A8` (who),
`0xAC7A58..` (chat), `0xAC8248..` (game UI).

**/who.** FrameXML's `/who` calls `SendWho(msg)` (an empty one first sets the
panel's default and shows it). `SendWho` (0x6B6B50 -> 0x6B5630) parses the
filter: at most 127 bytes and a space appended; split on white space outside
double quotes (the quotes go; an unclosed quote loses the last word). Per word:
`WHO_TAG_NAME` "n-" name (47 bytes), "g-" guild, "z-" zones (each top-level
AreaTable row, ParentAreaID 0, whose name contains the rest - case-insensitive,
0x76F7E0 - at most ten; none found sends zone 0), "r-" / "c-" masks of every
ChrRaces / ChrClasses row whose name contains the rest (the mask starts at 0 on
the first such word), a word starting with a digit is a level ("10" 10-10,
"10-" 10-100, "10-20"), "-20" sets the top only; anything else is a free string,
and the fourth string ends the parse. `CMSG_WHO`: `min, max, name, guild,
raceMask (-1), classMask (-1), zoneCount, zones..., stringCount, strings...`.
Wowee sent the whole text as the name ("/who 70 orc" asked for a player named
"70 orc").
`SMSG_WHO` (0x6B8720): `count, online`, rows `name, guild, level, class, race,
gender u8, zone`; fifty kept. With `SetWhoToUI` off and count <= 3 (the
threshold's cvar slot 0xC7BFE8 is never written: 3) each row is printed
(0x6B5030) as `WHO_LIST_FORMAT` / `WHO_LIST_GUILD_FORMAT`, SYSTEM type, then
`WHO_NUM_RESULTS`; otherwise only `WHO_LIST_UPDATE`. In a battleground the
client adds the cross-realm players it can see as "Name-Realm" (0x6B8290) -
not relevant to a single-realm server. `GetWhoInfo` (0x6B4A80): name, guild,
level, race and class by gender (0x715970: the gendered name, else the other,
else the plain one - all empty in enUS), the zone's AreaTable name ("Unknown"
when none), class file; past the list `nil, nil, 0, nil x4`. Wowee returned ""
for the zone.

**/played** (0x515880): `CMSG_PLAYED_TIME` with 1; `SMSG_PLAYED_TIME`
(0x401390) stores the total and, when the byte is set, writes the times to the
console and fires `TIME_PLAYED_MSG(total, level)`; the chat lines are
FrameXML's. **/time** is FrameXML alone (`ChatFrame_DisplayGameTime`).

**/logout, /camp, /quit.** `Logout()` and `Quit()` are 0x6B1930 with the quit
flag 0 / 1: unless a logout is pending, `CMSG_LOGOUT_REQUEST`, marking it
pending. `SMSG_LOGOUT_RESPONSE` (vtable +0x60, 0x6B08B0): result non-zero ->
`ERR_LOGOUT_FAILED` and not pending; accepted with a countdown ->
`PLAYER_CAMPING` (or `PLAYER_QUITING`), instant -> nothing (COMPLETE follows).
The 20 s and "instant in an inn or city" are the server's (AzerothCore:
resting and not in combat, or on a taxi, is instant; it roots, stuns and sits
the player for the countdown). `SMSG_LOGOUT_COMPLETE` (0x6B2180) leaves the
world, and exits for a quit. `CancelLogout()` (0x51AC90 -> 0x6B18C0) sends
`CMSG_LOGOUT_CANCEL` always and clears pending, so its
`SMSG_LOGOUT_CANCEL_ACK` (0x6B0900) fires `LOGOUT_CANCEL` only if the logout
was still pending - i.e. one the server called off. No client code cancels a
logout on movement (the player is rooted); the popup's Cancel is the way.
`ForceQuit` (0x510A00) closes the program at once; `ForceLogout` sends
`CMSG_PLAYER_LOGOUT` (0x6B1930 forced). Separately the world tick (0x52AF40)
sits and marks AFK after 5 idle minutes and logs out after 30
(`IDLE_MESSAGE`) - not mirrored.

**/random** (0x516610): both arguments required (Lua error "Usage:
RandomRoll(\"max\") or RandomRoll(\"min\", \"max\")"), read as text; 0-0, a
negative low end, low > high or high > 1,000,000 send nothing; otherwise
`MSG_RANDOM_ROLL min, max` as given. The answer (0x6E2E90 case 0x1FB: `min,
max, roll, guid`) is `RANDOM_ROLL_RESULT` (name, roll, min, max) as a SYSTEM
line once the name is known, nothing from someone ignored.

**Text emotes.** `DoEmote(token [,target])` (0x500980): EmotesText rows
searched by name; the target is the current target when the text is empty or
starts with '%', else a *player* found by unit token or name (anything else:
none). 0x6DD9E0: Emotes.dbc's EmoteFlags against the player (0x4F5410:
0x400 never, 0x1 only standing, 0x80 not swimming, 0x8000 not flying/mounted,
not asleep or dead unless 0x200, 0x4000 moving -> `ERR_NOEMOTEWHILERUNNING`);
yourself as target is none; a SpecProc 1 emote sets the stand state, else the
animation plays at once; `CMSG_TEXT_EMOTE id, soundIndex, guid` - the index is
a random file of the race/gender's EmotesTextSound entry, or -1 (0x4CEB70).
`SMSG_TEXT_EMOTE` (0x504070): `guid, id, soundIndex, len, target name`;
ignored -> dropped; otherwise queued like any line until the name is known
(0x503660). The wording: the sixteen EmotesTextData slots by bits 1 target is
the reader (name, any case; only for someone else's emote), 2 sender is the
reader, 4 no target, 8 female sender (from the name cache); tried as is, then
without 8, then with 1 cleared and 4 set (with and without 8); none - no line.
`%s` is the target for the sender's own line, the sender for one at the
reader, both otherwise. `CHAT_MSG_TEXT_EMOTE` with arg5 "" and **line id 0**,
arg12 the guid; then the voice (0x747000, `Sound_EnableEmoteSounds`).
Wowee printed "performs an emote" for a row with no text, dropped the line for
an unknown sender, ignored the female forms and did not check the ignore list.

**/follow** (0x5224C0): the text (empty = target, 0x60ABF0) as a player, then
any unit; not found -> `ERR_UNIT_NOT_FOUND`, or `ERR_GENERIC_NO_TARGET` for
nothing / "target". 0x72B4A0 action 3: not yourself; 0x729BD0 - a player, not
charmed, same faction group, not hostile -> else `ERR_INVALID_FOLLOW_TARGET`;
dead `ERR_PLAYER_DEAD`, stunned `ERR_GENERIC_STUNNED`, channelling
`ERR_TOOBUSYTOFOLLOW`; then `AUTOFOLLOW_BEGIN(name)`. Wowee followed only the
target and anything.

**/invite** (0x51A6E0): `InviteUnit(name)` takes a name, not a unit token; over
48 bytes is the Lua error "Name too long"; 0x6D4300: your own name ->
`ERR_INVITE_SELF`, else `CMSG_GROUP_INVITE name, 0`. **/inspect** is
FrameXML's `InspectUnit` (UIParent.lua) over `NotifyInspect`.

**Drunk speech** is client-side (0x577F60, from SendChatMessage after the `%t`
tokens, before the line-break cut) when PLAYER_BYTES_3 byte 1 (drunk) or
PLAYER_FAKE_INEBRIATION (0x142) is non-zero; factor = max of the two, at most
100, x 0.01 (0x4F7290). An 's' starting a word and followed by anything but a
space or s becomes "sh" with chance factor - unless "sh"+rest-of-word is caught
by the profanity filter (forced on); an 's' before a space likewise; `|c` runs
are copied to their `|r`. Then with chance factor/4 the line becomes
`SLURRED_SPEECH` ("%s ...hic!").

**Language comprehension** (0x6E0640): a language whose spell the reader knows
(table 0xBE8DE0 by language) is its skill; one not known is 300 under an aura
244 naming it, looked for only while UNIT_FIELD_FLAGS_2 has 0x8; else 0. In
3.3.5's Spell.dbc aura 244 exists for languages 36, 9, 37, 38 only (e.g.
44311 "Test Comprehend Language", Titan) - Gift of the Naaru or Babel are not
it.

**Mature language filter** (cvar `profanityFilter`, "1" as registered at
0x51E118): ChatProfanity.dbc (5222 rows; field 2 the language group; enUS is
group 0, -1 counts for all: 305 rows), compiled caseless by PCRE through its
POSIX wrapper (0x7E2250 -> 0x83DF20, options 0x2801). PCRE has no `\<`/`\>`:
they match the characters, so most English rows only ever match text in angle
brackets; "shit", "fuck", "bia?ts?ch", "whore"... do the work. Applied
(0x509A50; inline in 0x509DD0) to incoming lines of the 0x9FA510 types and to
the channel's "N. Name": per pattern, while it matches at an offset not inside
a `|c` run (0x4FCF40), the matched bytes become `!@#$%^&*` in turn - the
position in that string persists across replacements - and the search resumes
after them; results cached per text (0x617C50). Outgoing text is not filtered.
SendChatMessage does **not** validate links beyond the escape rule (re-read:
0x50D170's only loop is the `|`-escape scan).

**Zone channels.** At world entry (0x508090, once) the client joins each
ChatChannels row whose bit is in its joined mask - initially the rows with
flag 0x1 (0x501F00): General, Trade, LocalDefense, LookingForGroup (not
WorldDefense, not GuildRecruitment); the mask is saved per character. The name
(0x507A80): the row's pattern with the zone's name, or for a row with flag
0x20 (Trade, GuildRecruitment) in a zone with AreaTable flag 0x100 the name of
the AreaTable row with flag 0x200, "City"; no zone name - no join.
`CMSG_JOIN_CHANNEL id, 0, 0, name, password`. GuildRecruitment (0x507D30):
joined while its setting is on and PLAYER_GUILDID is 0, left on joining a guild.
Zone changes after that are the server's (AzerothCore
`Player::UpdateLocalChannels`): YOU_JOINED for the new zone's General, which the
client matches by channel id to the old one -> "YOU_CHANGED" (0x50E120: any
existing, not suspended channel with that id - or that name for id 0); leaving
a city sends YOU_LEFT with the constant byte, which the client shows as
"SUSPENDED" unless the player asked to leave.

**Cross-realm names**: only the battleground /who rows above; nothing in chat on
a single-realm server.

**Wowee changes (second pass)**: SendWho's filter parser and the CMSG_WHO
lists; GetWhoInfo's zone and past-the-end values; fifty who rows, the chat
threshold on the packet's count; RandomRoll's range (no swap, no 10000 cap);
PLAYER_LOGOUT no longer fired by an accepted logout request;
ERR_LOGOUT_FAILED; CancelLogout always sent, LOGOUT_CANCEL only for a server
cancel; ForceQuit closes, ForceLogout sends CMSG_PLAYER_LOGOUT; text emotes
through the chat queue with the sixteen slots, gender and ignore list, line id
0, no invented text; DoEmote's target rules and EmoteFlags refusals;
FollowUnit by name or token with 0x72B4A0's checks; InviteUnit's length and
self checks; drunk slurring; aura-244 comprehension; the profanity filter;
login channel joins by flag 0x1, "City" by AreaTable flags, GuildRecruitment
only without a guild, LFG asked for regardless of the queue; YOU_CHANGED for a
join of a channel already held. Also: `resolveUnitOrName` cast items and
corpses to units (a /follow at an unknown name crashed with bad_alloc).

**Checked in game** (AzerothCore lab; Wptestb, human paladin on WPTEST2, and a
throwaway troll warrior "Wphorde" put on WPTEST with the GM `.pdump copy` of a
bot and removed afterwards):
- `/who n-Wptestb`: one row, printed as `|Hplayer:Wptestb|h[Wptestb]|h: Level
  10 Human Paladin - Elwynn Forest` and `1 |4player:players; total`;
  `z-Elwynn 1-80` (22-30 rows) and `r-orc c-hunter` went to WHO_LIST_UPDATE
  only, GetWhoInfo giving the zone; past the end `nil nil 0 nil...`.
- `/played` -> TIME_PLAYED_MSG; `RandomRoll("1","100")` -> "Wptestb rolls 10
  (1-100)"; 5-3 and 0-0 sent nothing; no arguments raised the usage error.
- DoEmote WAVE with no target, at oneself, ANGRY at an unknown name: "You
  wave." / "You wave." / "You raise your fist in anger." with arg11 0.
- FollowUnit "" with no target -> "You have no target."; an unknown name ->
  "Unknown unit." (this crashed before: bad_alloc in resolveUnitOrName);
  following the Horde character -> "You can't follow that unit.".
- InviteUnit("wptestb") -> "You can't invite yourself to a group."; 49 bytes ->
  Lua error "Name too long".
- "oh shit, that bitch of a boss" came back as "oh !@#$, that %^&*! of a boss".
- `.modify drunk 100`, then "this is so sweet, sis says yes" went out as "thish
  ish sho shweet, shish shaysh yesh", one of three with " ...hic!" (the GM
  command typed while drunk was slurred too, as stock would).
- Logout in Northshire was instant (resting): no PLAYER_CAMPING, LOGOUT_COMPLETE.
- Cross-faction: the troll's "hello there alliance friend, lol" read for the
  human as "kazum no'ku ragath'a raznos kek" [Orcish], the yell "kil mog ro'th";
  the human's "hello there horde friend, lol" read for the troll as "majis ergin
  majis waldir bur" [Common]; text emotes cross: "Wptestb waves at you." /
  "Wphorde raises his fist in anger at you.".
- Comprehension: under `.aura 44311` (aura 244, language 9) the server sends
  the holder's speech in Titan; the holder reads its own Titan line plainly,
  the other character, without the aura, as "lok rethul gular karaman no".
- Zone change by `.appear`: "YOU_CHANGED" for 1. General - Elwynn Forest and 2.
  LocalDefense; the server also joins WorldDefense on its own.
- A dead character's `.` command typed into SAY is refused with "You can't chat
  when you're dead!" (stock's dead check runs before the server sees it).

### 7. Gaps

- The tag-0x08 whisper refusal; CJK word splitting in the scrambler.
- The achievement link's date is the local date and the criteria are zero.
- The dead check uses Wowee's dead/ghost state for the player vfunc +0x128,
  not read further.
- The talking animation is not played on the player's own model or on a
  mounted unit (0x73AD70's movement-state and descriptor checks were not read
  to the end).
- The joined-channel mask is not saved per character; Wowee's own switches
  stand in for it.
- `CMSG_TEXT_EMOTE`'s sound index is sent as 0 and the receiver plays its own
  pick (0x4CEB70 / 0x747000 not mirrored); the DANCE dance-studio path of
  DoEmote; EmoteFlags 0x8000's mount half; the vfunc +0x138 check in 0x6DD9E0.
- /camp's 20 s countdown was not seen in game (the test character logs out
  instantly where it stands).
- Follow's faction test uses Wowee's hostility map, not FactionTemplate's
  group (0x729BD0) and 0x729740 was not read.
- The idle sit / AFK / 30-minute logout of 0x52AF40.
- The filter's regexes run on std::regex (ECMAScript) after turning `\<` `\>`
  into literal brackets; PCRE-only syntax in a row would be skipped.
- WHO rows' race and class words are Wowee's English tables, not ChrRaces'
  gendered names (identical in enUS).
## Combat log

Read from `Wow.exe` (UnitCombatLog_C.cpp, 0x74D000-0x757000, and the packet
handlers that feed it). Wowee's implementation is
`include/game/stock_combat_log.hpp` (rules, builders, formatter, flags,
filters, history), `CombatHandler::logCombatEvent` / `flushCombatLog` /
`combatLogUnitFlags` (src/game/combat_handler.cpp), the packet hooks in
combat_handler.cpp / spell_handler.cpp / social_handler.cpp /
game_handler_packets.cpp, and the Lua side in lua_engine.cpp (`fireEvent`) and
lua_system_api.cpp (`CombatLog*`). Tests: `tests/test_stock_combat_log.cpp`.

### C.1 The entry and its life

| Step | Address | Rule |
|---|---|---|
| allocate | 0x750400 | Takes the oldest entry for reuse if it is older than `combatLogRetentionTime` seconds (CVar, default `"300"`, registered at 0x51EFE1), else allocates. **At most one entry is let go per new one.** Sets the event, source and destination guids, and **computes both units' flags now** (0x74DCB0). A spell prefix is set from the spell record. |
| reset | 0x74D920 | event -1, flags `0x80000000` (COMBATLOG_OBJECT_NONE) for both units, guids 0. |
| names | 0x74FD40 / 0x74F720 | Each unit's name is asked of the right cache: players from the name cache (type 1), creatures and vehicles by entry from the creature cache (type 3) unless they carry a pet number (type 2, pet names), pets (F140) from the pet name cache, game objects (F110) from the game object cache (type 4). A pending answer sets a bit in the entry's +0x10; the entry waits. A failed lookup is `UNKNOWNOBJECT` ("Unknown"). Items (0x750210) the same way, type 5. |
| fire | 0x74F910 | Entries go to Lua **in arrival order**: the oldest entry still waiting for a name holds every later one back (0xCA1394). For each: the arguments (0x74E290), then `COMBAT_LOG_EVENT` **only when the filters pass** (event 0x236, 0x74F6C0), then `COMBAT_LOG_EVENT_UNFILTERED` (0x237) always, then the WoWCombatLog.txt line when `LoggingCombat` is on. |
| timestamp | 0x74E290 | `(entry tick - login tick) * 0.001 + time() at login` (0xCA1388/0xCA138C set at 0x7538A0): seconds **with milliseconds**, a Lua number. |

### C.2 The arguments (0x74E290)

`timestamp, event, sourceGUID, sourceName, sourceFlags, destGUID, destName,
destFlags` - eight, always. GUIDs are `"0x"` and sixteen upper-case hex
digits (0x74D0D0); a unit with no guid is `0x0000000000000000`, its name nil,
its flags `0x80000000`. A player from another realm is `"Name-Realm"`.

Then, when the entry has a spell (+0x44 non-zero): `spellId, spellName,
spellSchool` (nil and 0 when the spell is not in Spell.dbc). Then the suffix
groups, **in this order**, by the entry's bits at +0x54:

| Bit | Values | Used by |
|---|---|---|
| 0x0001 | amount | SPELL_EXTRA_ATTACKS |
| 0x0002 | failedType (text) | SPELL_CAST_FAILED |
| 0x0004 | missType (0xADBFAC: NONE MISS RESIST DODGE PARRY BLOCK EVADE IMMUNE IMMUNE DEFLECT ABSORB REFLECT) | *_MISSED |
| 0x0008 | environmentalType (0xAD671C: FATIGUE DROWNING FALLING LAVA SLIME FIRE) | ENVIRONMENTAL_DAMAGE |
| 0x0010 | amount, overkill, school, resisted, blocked, absorbed, critical, glancing, crushing - resisted/blocked/absorbed **nil when 0**, the three flags 1 or nil | *_DAMAGE, DAMAGE_SHIELD, DAMAGE_SPLIT |
| 0x2000 / 0x4000 / 0x8000 | amountMissed for ABSORB / RESIST / BLOCK (always pushed, even 0) | *_MISSED |
| 0x0020 | amount, overhealing, absorbed (numbers), critical (1/nil) | *_HEAL |
| 0x0040 | amount, powerType | *_ENERGIZE |
| 0x0080 | amount, powerType, extraAmount (nil when 0) | *_DRAIN, *_LEECH |
| 0x0100 | extraSpellId, extraSpellName, extraSchool | INTERRUPT, DISPEL, STOLEN, DISPEL_FAILED, AURA_BROKEN_SPELL |
| 0x1000 | spellName (text) | ENCHANT_APPLIED/REMOVED |
| 0x0200 | itemId, itemName | ENCHANT_*, DURABILITY_DAMAGE |
| 0x0400 | auraType `"BUFF"` / `"DEBUFF"` | aura events, DISPEL, STOLEN, AURA_BROKEN* |
| 0x0800 | amount (the new stack count) | AURA_APPLIED_DOSE / REMOVED_DOSE |

Power amounts are divided by the power's divisor (0x7FDE00: rage and runic
power by 10). The 50 event names are the table at 0xADB758.

### C.3 What writes an entry

A spell is written only when it is in Spell.dbc with a non-empty name and has
neither SPELL_ATTR0 0x80 nor 0x100 (every builder; `loggable`).

| Packet | Handler | Events |
|---|---|---|
| SMSG_ATTACKERSTATEUPDATE | 0x756800 -> 0x7512B0 | Victim state 1 or 5 with damage: SWING_DAMAGE (school = first sub-damage's, absorbed/resisted summed over at most two sub-damages, blocked, crit = hitInfo 0x200, glancing 0x10000, crushing 0x20000). Else SWING_MISSED: hitInfo 0x10 MISS; 0x20 ABSORB+amount; 0x80 RESIST+amount; state 2 DODGE, 3 PARRY, 5 BLOCK+amount, 6 EVADE, 7 IMMUNE, 8 DEFLECT; anything else (4, interrupt) nothing. Every unit's swings. |
| SMSG_SPELLNONMELEEDAMAGELOG | 0x6D3840 -> 0x751C40 | Damage: SPELL_DAMAGE, or SPELL_PERIODIC_DAMAGE when the byte after resisted is set, or RANGE_DAMAGE for SPELL_ATTR3_NORMAL_RANGED_ATTACK (0x8000); crit = hit type 0x2. No damage: *_MISSED ABSORB, else BLOCK, else RESIST, else nothing. Hit type 0x8: DAMAGE_SPLIT (0x752DE0); 0x10: the target is the source. |
| SMSG_PERIODICAURALOG | 0x7531F0 | Per aura type: 3/89 damage (periodic), 8/20 heal (periodic), 21/24 energize (periodic), 64 drain/leech (periodic); **an unknown type ends the packet**. Nothing when the spell or the victim unit is unknown. |
| SMSG_SPELLHEALLOG | 0x6D3C30 -> 0x751F70 | SPELL_HEAL, or SPELL_PERIODIC_HEAL for SPELL_ATTR3_TREAT_AS_PERIODIC (0x2000000). Nothing when amount, overhealing and absorbed are all 0. |
| SMSG_SPELLENERGIZELOG | 0x6D3DF0 -> 0x752040 | SPELL_ENERGIZE (periodic by the same attribute); nothing for 0. |
| SMSG_SPELLLOGMISS | 0x6D3090 -> 0x751B80 -> 0x750500 | SPELL_MISSED / RANGE_MISSED per target, unless the debug byte is set (then two floats per target and no entry). |
| SMSG_SPELL_GO miss list | 0x80E1B0 | The same, at once for a spell with Speed 0 and without SPELL_ATTR2 0x80; source is the casting object when it is a unit or game object, else the caster unit. |
| SMSG_SPELLORDAMAGE_IMMUNE | 0x6CE3D0 | SPELL_MISSED IMMUNE (two **whole** guids, caster then victim). |
| SMSG_PROCRESIST | 0x750D40 | SPELL_MISSED RESIST, amount 0 (two whole guids). |
| SMSG_SPELLDAMAGESHIELD | 0x6D3750 -> 0x752C90 | DAMAGE_SHIELD from the shield's owner (first whole guid) to the attacker: amount, overkill, school. |
| SMSG_ENVIRONMENTALDAMAGELOG | 0x756800 -> 0x751150 | ENVIRONMENTAL_DAMAGE when any of damage/absorbed/resisted; school `1 << {0,0,0,2,3,2}[type]`. |
| SMSG_SPELLLOGEXECUTE | 0x6D3090 loop | Effect 8/62 drain or leech; 18/113 SPELL_RESURRECT; 19 SPELL_EXTRA_ATTACKS; 28, 50, 56, 76, 81, 83, 104-107 SPELL_SUMMON (SPELL_CREATE when the target is a game object); 68 SPELL_INTERRUPT; 111 SPELL_DURABILITY_DAMAGE (item) or _ALL (item and slot -1). 24/59/157, 33, 101, 102 are read and write nothing. |
| SMSG_SPELLINSTAKILLLOG | 0x6CE260 -> 0x752210 | SPELL_INSTAKILL. |
| SMSG_SPELLDISPELLOG / SPELLSTEALLOG / SPELLBREAKLOG | 0x6CE2B0 | Packed victim, packed caster, spell, byte, count, then spell + byte per aura (the byte is taken as "debuff"). SPELL_DISPEL / SPELL_STOLEN per aura (the opcode decides); BREAKLOG: SPELL_AURA_BROKEN, or _SPELL with the header spell as extra spell, and no source when that spell has SPELL_ATTR4 0x2000. |
| SMSG_DISPEL_FAILED | 0x750EA0 | SPELL_DISPEL_FAILED per spell. |
| SMSG_ENCHANTMENTLOG | 0x751050 | ENCHANT_APPLIED (caster set) / ENCHANT_REMOVED, unless the enchantment's flags have 0x2. |
| SMSG_PARTYKILLLOG | 0x750C90 | PARTY_KILL, killer to victim. |
| SMSG_DESTRUCTIBLE_BUILDING_DAMAGE | 0x753730 | SPELL_BUILDING_DAMAGE / _HEAL (not built in Wowee: no siege content tested). |
| SMSG_SPELL_START | 0x806700 -> 0x751920 | SPELL_CAST_START, no destination, unless CAST_FLAG_PENDING (0x1), the cast is suppressed (0x74D3F0: not loggable, a learn-spell effect 36, SPELL_ATTR4 0x1) - **and only when the caster's cast time for the spell is not zero** (vtable +0x148 = 0x71AD20, GetSpellCastTime). |
| SMSG_SPELL_GO | 0x80E1B0 -> 0x7519E0 | SPELL_CAST_SUCCESS when not pending (or SPELL_ATTR7 0x80000000), not suppressed, no OPEN_LOCK effect (33, 0x74D3D0) - **and only when the cast time is zero**. A spell with a cast time is written once, as CAST_START. |
| cast refusal | 0x808200 -> 0x751AD0 | SPELL_CAST_FAILED, the player as source, failedType the text shown, after the same 3 s throttle. |
| SMSG_AURA_UPDATE (not _ALL) | 0x72F5D0 | Per slot: an aura gone (slot empty or another spell) SPELL_AURA_REMOVED **only while the unit's health is above 0** (0x752710); a new one SPELL_AURA_APPLIED; the same spell with another stack count *_DOSE when its StackAmount > 1; the same with a new duration SPELL_AURA_REFRESH. Never for passives (SPELL_ATTR0 0x40) or SPELL_ATTR6 0x400. |
| health to 0 | 0x7489C0 / 0x729220 -> 0x7561E0 -> 0x752ED0 | UNIT_DIED (destination only); UNIT_DISSIPATES for creature type 13; UNIT_DESTROYED for a totem. Not for a creature without a cache record or with type flag 0x400 (0x718A90). |

### C.4 Unit flags (0x74DCB0)

- No guid: `0x80000000`.
- The **controller**: the unit itself; for a unit its CHARMEDBY, else CREATEDBY,
  when set; for a game object its CREATED_BY. A controller that is not a player
  and is itself charmed or created is followed one more step.
- **Type**: a player, creature, vehicle or pet guid controlled by itself is
  PLAYER 0x400 / NPC 0x800 / PET 0x1000 (pet guid); controlled by another it is
  PET 0x1000 when it has CHARMEDBY or SUMMONEDBY, else GUARDIAN 0x2000. Any
  other guid is OBJECT 0x4000.
- **Control**: PLAYER 0x100 when the controller is a player, else NPC 0x200.
- **Affiliation and reaction**: controller = the active player: MINE|FRIENDLY
  (0x11). Otherwise, for a unit, its reaction to the player: 4 and up FRIENDLY,
  2-3 nothing yet, 0-1 HOSTILE (and no group test). Then, in a group, PARTY
  (0x12) when the controller is in the player's party, else RAID (0x14) when in
  the raid. Empty affiliation is OUTSIDER 0x8; empty reaction NEUTRAL 0x20.
- TARGET 0x10000 and FOCUS 0x20000 by the unit's own guid.
- A player in the group (or the player in a group): MAINTANK 0x40000 for group
  member flag 0x2, else MAINASSIST 0x80000 for 0x4.
- In a group, the raid target icon i (0-7): `0x100000 << i`.

So the player is 0x511, a hostile creature 0xA48, a neutral one 0xA28, the
player's pet 0x1111, a party member 0x512.

### C.5 The Lua API (0x74D5B0-0x751120, table 0xADB8E0)

| Function | Address | Rule |
|---|---|---|
| CombatLogGetNumEntries([ignoreFilter]) | 0x74FA70 | Entries passing the filters (all with the flag). |
| CombatLogSetCurrentEntry(index [,ignoreFilter]) | 0x74FAE0 | index >= 1 counts from the oldest; 0 is the newest, -1 the one before. Only matching entries count. true (1) or nil; nil clears the cursor. |
| CombatLogAdvanceEntry(count [,ignoreFilter]) | 0x74FC20 | Moves by `count` matching entries, positive toward newer; nil (and no cursor) past either end. |
| CombatLogGetCurrentEntry() | 0x74F2B0 | The entry's arguments, nothing when there is none. |
| CombatLogAddFilter(events, src, dst, spell) | 0x74FF70 -> 0x74E050 | events: nil all, else names split on spaces and commas (case-sensitive). src/dst: number = mask (refused as "incomplete filter for srcMask/dstMask" when it has no special bit and an empty category), string = guid, nil = 0xFFFFFFFF. spell: number id or string name. An entry passes when **any** filter matches; with **no filters every entry passes**. |
| CombatLogResetFilter() | 0x74FA60 | Removes all filters. |
| CombatLogClearEntries() | 0x751120 | Empties the history. |
| CombatLogSetRetentionTime(s) / GetRetentionTime() | 0x74D5B0 / 0x74D9E0 | The combatLogRetentionTime CVar. |
| CombatLog_Object_IsA(flags, mask) | 0x74D600 | `m = flags & mask`; true when `m & 0xFFFF0000`, else when each of `m & 0xF`, `0xF0`, `0x300`, `0xFC00` is non-zero. An empty category in the mask matches nothing. |

Blizzard_CombatLog's `COMBATLOG_MESSAGE_LIMIT = 300` is the window's line
count, FrameXML's own; the client keeps what arrived in the last 300 seconds.

### C.6 What Wowee did before, and what changed

- Every combat log event came from the floating-combat-text path
  (`addCombatText`), only for fights involving the player, with school always
  physical, overkill 0, resisted/blocked/absorbed always nil, partial
  absorbs/resists dropped, ENVIRONMENTAL_DAMAGE as "Fatigue"/"Falling", a
  whole-second timestamp, and no aura, cast, summon, interrupt, extra-attack,
  durability, death or break events at all. Now each packet builds its own
  entry as listed in C.3, for every unit the client is told about.
- Unit flags were a guess (creature hostile, other player friendly, never
  party/raid/neutral/pet-of-others/target/focus/raid icon). Now 0x74DCB0.
- COMBAT_LOG_EVENT was fired for every entry and CombatLogAddFilter /
  ResetFilter did nothing, so the window showed everyone's fights; and the
  refilter walked an unfiltered history. Now the filters are kept and applied
  to the event and to every walk.
- CombatLog_Object_IsA treated an empty category as "any"; stock treats it as
  no match.
- History was 1000 entries by count; now 300 seconds, one expired entry per add.
- SetCurrentEntry clamped and returned nothing; AdvanceEntry stopped at the
  end; both now follow 0x74FAE0/0x74FC20. CombatLogClearEntries and the
  retention functions were missing.
- Entries now wait for their units' names, in order, as the stock client does
  (up to 3 s here, then "Unknown").
- SMSG_SPELLDAMAGESHIELD, SMSG_SPELLORDAMAGE_IMMUNE and SMSG_PROCRESIST were
  read with packed guids on WotLK; the client and AzerothCore use whole ones.
  SMSG_SPELLBREAKLOG was dropped; the "stolen" byte of SPELLDISPELLOG was taken
  as a steal (it is unused; the opcode decides).

### C.7 Checked in game (Wptestb, Northshire, AzerothCore lab)

- Other players' and their pets' fights arrive as COMBAT_LOG_EVENT_UNFILTERED
  with flags 0x518 (outsider friendly player), 0x1118 (their pet), 0xA28 /
  0xA48 (neutral / hostile creature); SPELL_CAST_START for Shadow Bolt and
  Firebolt, SPELL_CAST_SUCCESS for instants (Sinister Strike, Stealth),
  SPELL_AURA_APPLIED with BUFF/DEBUFF, SPELL_PERIODIC_HEAL, UNIT_DIED with no
  source.
- The player's own: SWING_DAMAGE both ways (target bit 0x10000 on the wolf),
  SWING_MISSED MISS, SPELL_DAMAGE (Seal of Righteousness), SPELL_CAST_START for
  Holy Light then SPELL_HEAL with its overhealing, SPELL_AURA_APPLIED and
  _REMOVED (Dazed), PARTY_KILL, ENVIRONMENTAL_DAMAGE FALLING from a 40-yard
  drop (school 1, overkill 0).
- COMBAT_LOG_EVENT came only for what the window's "Self" filter lets through
  (2-3 of ~100 unfiltered), CombatLogGetNumEntries 17 against 121 with
  ignoreFilter, and Blizzard_CombatLog's own CombatLog_OnEvent printed
  "Wptestb's Holy Light heals Wptestb for 0.(57 Overhealed)" from
  CombatLogGetCurrentEntry; the Combat Log tab showed the same lines.

### C.8 Gaps

- WoWCombatLog.txt (`LoggingCombat`) is not written.
- SMSG_DESTRUCTIBLE_BUILDING_DAMAGE (SPELL_BUILDING_DAMAGE/_HEAL) is not
  built; DAMAGE_AURA_BROKEN has no builder in the client either.
- UNIT_DESTROYED is decided by creature type 11 (totem); the client asks
  whether the creating spell's summon filled a totem slot.
- A name that never arrives is replaced by "Unknown" after 3 s; the client
  waits for the cache's answer however long it takes.
- The "realm" suffix of cross-realm names is not added.
- The GO miss list of a spell with a missile is written at once rather than
  at impact (the client's missile path was not followed).
- SPELL_CAST_SUCCESS's destination is the cast's unit target; for
  SPELL_CAST_FAILED the 0x81 result's special text path is not mirrored.
- CombatLogSetRetentionTime keeps the value in the history, not the CVar.
- TBC keeps the old combat-text-derived layout; 1.12 its chat lines.
