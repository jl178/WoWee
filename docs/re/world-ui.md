# The in-world interface the engine draws

The parts of the 3.3.5a (build 12340) interface that `Wow.exe` draws in the
world itself, without FrameXML: nameplates, names over heads, the numbers over
a target, chat bubbles, the raid mark over a head, the selection circle and
the mouseover highlight. Read from `Wow.exe` 12340 with Ghidra. Each rule
names the function or data it came from.

The pure rules are in `include/ui/world_ui_rules.hpp` and tested by
`tests/test_world_ui_rules.cpp`. Wowee draws them in
`src/ui/game_screen_world_ui.cpp` (plates, names, head marks),
`src/ui/combat_ui.cpp` (world text) and
`src/ui/chat/chat_bubble_manager.cpp` (bubbles).

Not covered here:
- the selection circle (0x725980, its size 0x720330, UnitSelectTexture.blp
  through the blob projector) and the target/mouseover model highlight
  (0x743C70, `unitHighlights`): characters.md "Highlight", and
  combat-targeting-movement.md row 17 for the attack pulse;
- the floating combat text around the player: that is Blizzard_CombatText,
  FrameXML's, driven by COMBAT_TEXT_UPDATE.

## 0. Units

The engine's frame coordinates ("DDC") make the **screen diagonal 1.0**.
SetAspect 0x47bf90 stores, for aspect a: height `1/sqrt(a²+1)` (0xac0cb8),
width `a/sqrt(a²+1)` (0xac0cb4), `0.75a` (0xac0cbc). So a size the engine
gives in DDC is that fraction of the diagonal:

    pixels = ddc × sqrt(W² + H²)

FrameXML's 768-tall units relate as `ui = ddc × 768 × sqrt(a²+1) / scale`
(GetWidth 0x49d3b0, GetScreenHeight 0x51af50). At 1024×768 the diagonal is
1280 px; at 1920×1080 it is 2203 px, so engine-sized things (plates, world
text, bubble text) are about 1.15x larger, relative to the screen height, on
16:9 than on 4:3. The nameplate is a WorldFrame child, so the interface scale
does not touch it.

Anchor points (0x48a260 SetPoint): 0 TOPLEFT, 1 TOP, 2 TOPRIGHT, 3 LEFT,
4 CENTER, 5 RIGHT, 6 BOTTOMLEFT, 7 BOTTOM, 8 BOTTOMRIGHT. Offsets are y-up.
Texture coordinate rects passed to 0x4831d0 are `{top, left, bottom, right}`
(the raid icon grid in 0x98e740 shows it). Font flags (table 0xa44050):
OUTLINE 1, MONOCHROME 2, THICKOUTLINE 4; flag 8 implies 1 (0x6bde80).
Colours are stored B,G,R,A.

## 1. CVars

| CVar | Default | Registered |
|---|---|---|
| nameplateShowEnemies / nameplateShowFriends | 0 / 0 | 0x51d9b0 |
| nameplateShowEnemyPets, -Guardians, -Totems | 1, 1, 1 | 0x51d9b0 |
| nameplateShowFriendlyPets, -Guardians, -Totems | 1, 1, **1** | 0x51d9b0 |
| nameplateAllowOverlap | **1** ("0" = the old never-overlapping layout) | 0x51d9b0 |
| ShowClassColorInNameplate | 0 | 0x51d9b0 |
| showVKeyCastbar | 1 | 0x51d9b0 |
| chatBubbles / chatBubblesParty | 1 / **1** | 0x51d9b0 |
| CombatDamage, CombatHealing, CombatLogPeriodicSpells, PetMeleeDamage, PetSpellDamage | 1 | 0x51d9b0 |
| UnitNameOwn, UnitNameNPC, UnitNameNonCombatCreatureName | 0 | 0x7e6150 |
| UnitNamePlayerGuild, UnitNamePlayerPVPTitle | 1 | 0x7e6150 |
| UnitName{Enemy,Friendly}PlayerName, -PetName | 1 | 0x7e6150 |
| UnitName{Enemy,Friendly}GuardianName, -TotemName | 0 | 0x7e6150 |

The UnitName CVars feed one bit mask (0xd380a0) through 0x7e60e0: Own 0x1,
NPC 0x2, PlayerGuild 0x4, PVPTitle 0x8, EnemyPlayer 0x10, EnemyPet 0x20,
EnemyTotem 0x40, FriendlyPlayer 0x80, FriendlyPet 0x100, FriendlyTotem 0x200,
NonCombatCreature 0x400, EnemyGuardian 0x1000, FriendlyGuardian 0x2000.

V and Shift-V are the NAMEPLATES / FRIENDNAMEPLATES bindings, which flip
nameplateShowEnemies / nameplateShowFriends.

## 2. Nameplates (CGNamePlateFrame, NamePlateFrame.cpp)

**Who gets one** (0x72b060, called per unit by 0x72b350 from the world-frame
object walk 0x4f6a40):
- health > 0 (unit field +0x48), not NOT_SELECTABLE (flags bit 25), a model;
- "friendly" is `!CanAttack` (0x729740) for a creature and 0x729b30 for a
  player; a hostile unit in stealth (BYTES_1 byte 2 bit 0x02) gets none;
- creature cache type_flags 0x100000 (NO_NAME_PLATE): none;
- nameplateShowEnemies for the unfriendly, nameplateShowFriends for the
  friendly;
- creature type 8 (critter) and 12 (non-combat pet): none;
- a dead unit (dynamic flag 0x20): none (unless 0x512a30, not traced);
- a unit whose owner (CHARMEDBY, else CREATEDBY) is a player: type 11 → the
  Totems CVar; otherwise pet or guardian by 0x71b600 → the Pets / Guardians
  CVars, friendly or enemy;
- **within 41 yd of the active player** (dist² < 1681, 0xadaa7c), not the
  camera; skipped when 0x65c290(0xbd088c)+8 == 4 (not traced);
- its anchor point projects onto the screen (0x715720 → 0x4f6d20).
- **No line-of-sight test**: a plate shows through walls.

**Where** (0x715720, 0x725890): the unit's name position (vtable +0x20,
0x71fef0: M2 attachment 18 "PlayerName", 29 when mounted; without a model,
the position + height × scale × 1.25) **plus 2/3 yd** (0x9fc830), projected.
With nameplateAllowOverlap on, the plate's **TOP** is set at that point
(SetPoint TOP, WorldFrame BOTTOMLEFT) and its frame depth from the view z, so
nearer plates draw over further ones. With it off, 0x615e10 places the plate
by the old stacking walk and keeps it on screen.

**Layout** (constructor 0x98f790, layout 0x98f390). Plate 0.1 × 0.025 DDC
(0xb2da74/0xb2da70), i.e. 128 × 32 at the 1280-units-per-DDC scale used here:

| Region | Art | Size (DDC) | Anchor |
|---|---|---|---|
| threat glow | UI-TargetingFrame-Flash, tex (0..0.555, 0.53..0.6) | 0.11 × 0.029 | TOP at plate TOP +(-0.001, -0.0065) |
| border | Nameplate-Border | plate | all points |
| health bar | UI-TargetingFrame-BarFill (status bar) | 0.804W × 0.281H | BOTTOMLEFT at plate +(0.031W, 0.125H) |
| cast border | Nameplate-Border mirrored (0..1 → 1..0) | plate | CENTER at plate CENTER −H/2 |
| shield | Nameplate-CastBar-Shield | plate | below the cast border |
| cast bar | BarFill | 0.804W × 0.281H | BOTTOMRIGHT at cast border +(−4, ±4 units) |
| spell icon | spell icon | 0.01 | CENTER at cast border BOTTOMLEFT +(0.0092, 0.0071) |
| highlight | Nameplate-Glow, blend ADD (3), layer HIGHLIGHT | plate | all points |
| name | NAMEPLATE_FONT, 0.01, no outline, shadow black (0.001, −0.001) | | BOTTOM at plate CENTER |
| level | NAMEPLATE_FONT, 0.009, same shadow | | CENTER at plate BOTTOMRIGHT +(−0.0092, +0.0071) |
| skull | UI-TargetingFrame-Skull | 0.01 | CENTER at the level |
| raid mark | UI-RaidTargetingIcons quarter | 0.02 | RIGHT at the border's LEFT |
| elite | EliteNameplateIcon, tex (0..0.578, 0..0.844) | 0.0294 × 0.0215 | CENTER at the level +(0.003, −0.001) |

The client never loads Nameplate-CastBar.blp.

**Health bar colour** (0x98ee30, from the reaction 0x7251c0 of the unit to
the player, 0-based): reaction < 2 (hated, hostile) red 0xb2da18; otherwise a
player blue 0xb2da0c; a creature green 0xb2da10 when 0x514050 (reaction ≥ 4
or cache flag 0x4000000), else **yellow** 0xb2da14 — unfriendly is yellow on a
plate. With ShowClassColorInNameplate, a player with reaction < 2 takes
RAID_CLASS_COLORS (0xb2da20 + class × 4). **There is no tapped grey**; the
TidyPlates of the era also read only these colours.

**Level** (0x98ef10): the level text, or the skull for a boss (0x715d70) and
for a hostile unit (reaction < 2) ten or more levels above. Colour 0x98e5f0,
its own table (d = level − player level): d > 4 (255,25,25); d > 2
(255,127,63); d > −3 (255,255,0); else green (63,178,63) unless the player is
more than the green range (0xaa34b8[player level / 5] = 4,4,5,5,6,6,7,7,8…)
above, then grey (127,127,127).

**Elite icon** (0x98e6e0): classification elite or rare elite, or a boss.

**Alpha and order** (per-frame update 0x98e9f0): the target's plate, and
every plate when there is no target, at alpha 255; the rest **127**. Frame
level 20 for the target, 10 for the rest (the target draws on top).

**Threat glow** (0x98e9f0 → 0x7374c0): shown while the player's threat status
on the unit is 1 or more, in GetThreatStatusColor's colours (0xad2d70: 1
(255,255,119), 2 (255,153,0), 3 (255,0,0)).

**Highlight** (0x7271d0): the plate under the cursor nearest the camera that
is **not the target's**. It shows Nameplate-Glow (0x98e910) and turns the
name **yellow**; off (0x98e980) back to white.

**Name colour**: white; yellow while highlighted; **red for 5 s after the unit
takes damage** (0x98e5b0, from the damage log handlers 0x751150, 0x751c40 and
0x73f330), then back.

**Cast bar** (0x720e50 → 0x98f040): **only the current target's**, and only
with showVKeyCastbar. Colour (1, 0.7, 0) while casting or channelling; a
channel drains. The shielded border when the spell cannot be interrupted.
On finishing it holds and fades (0x2f8 / 0x2fc in 0x98e9f0).

**Mouse**: plates are buttons; a click targets the unit.

## 3. Names over heads (CPlayerName, PlayerName.cpp)

Each unit with a model has a name object (0x7e5f60) drawn from the model's
render callback (0x7e6090 → 0x7e5640), so a name is up while the model is
drawn.

**Who is named** (CGUnit_C vtable +0xD0, 0x729c70, with the mask above):
- a unit with a nameplate: **never** (the plate carries the name);
- the active player: UnitNameOwn;
- **the current target: always**;
- a creature with no player owner: critter or non-combat pet →
  NonCombatCreatureName, otherwise UnitNameNPC; a hostile stealthed one never;
- a player's totem: FriendlyTotem if it cannot attack you, else EnemyTotem;
  a player's guardian (0x71b600): **EnemyGuardianName whichever side** (the
  FriendlyGuardian bit is never read); a player's pet: FriendlyPet or
  EnemyPet;
- a player: FriendlyPlayerName when 0x729b30; else EnemyPlayerName, and not
  while stealthed.

**Text** (vtable +0xCC, 0x72d4f0): status prefixes (vtable +0x108..+0x114),
the name with the title when PVPTitle (0x72a290), then for a player
`\n<Guild>` when PlayerGuild; for a creature `\n<SubName>` (creature cache,
when the unit has no pet number) and the owner line from 0x61e830
(UNITNAME_SUMMON_TITLE1 "%s's Pet" for a beast, TITLE3 for the rest, or the
summon properties' title).

**Font and size**: UNIT_NAME_FONT at glyph height 0.99, **THICKOUTLINE**
(0x7e64d0). The text is **3D world text**: each line is
`0.2 yd × (model height > 4 ? height × 0.375 : 1)` tall (0x7e5420 × 0.2), so
it shrinks with distance. There is **no distance fade**.

**Placement**: the name position (attachment 18). The block is raised by its
extra lines (0x7e5640 +0x30), so the last line sits on the name position and
the name line on top.

**Depth**: the PlayerNames string batch is 3D with depth test and depth write
on (0x6bf160(1,1), rendered by 0x6c53a0): a wall in front hides a name.

**Colour** (vtable +0x78, 0x718ac0 → 0x521bf0 with the "extended" flag — the
selection circle's colour too):
- the target the player is attacking, while it can be attacked: red
  (0xa30 bit 0x10, 0xadaa98);
- a creature: dead grey (127,127,127); else by reaction hated/hostile red,
  unfriendly **orange** (255,128,0), neutral yellow, friendly up green
  (0xbd0c94 table);
- a player or a player's pet: red when it can attack you and you it; light
  blue (96,96,255) when it can attack you and you not; yellow when you can
  attack it; your party (0x52d310) (170,170,255), or (170,255,170) PvP
  flagged; a friend (0x6b3510) (83,201,255); PvP flagged by reaction; else
  (96,96,255).

## 4. The raid mark over a head

Drawn with the name (0x7e5640) when the unit has no plate (0x7e5340): a
**1 yd square** billboard (vertices 0xaf46ac), bottom on the name position
plus the text's height when a name is shown, from UI-RaidTargetingIcons
quarters. Render states: lighting, fog and culling off, alpha blend, **depth
test and write off** (it shows through walls). Alpha by the camera distance
(0x7e52a0): 34 within 4 yd, `(d − 4) × 55.25 + 34` to 8 yd, 255 beyond.
On a plate the mark is the plate's own region (above).

## 5. Numbers over a target (WorldText.cpp)

Engine-side, separate from Blizzard_CombatText. Added through 0x7e6030 →
0x7e5c30 → 0x7e6dc0, animated by 0x7e70d0 / 0x7e6cc0 / 0x7e6850.

**Which lines**:
- damage and misses (0x722340, 0x71f990) on a unit other than the player,
  from the player or the player's pet/guardian (0x7190a0); CombatDamage;
  periodic also CombatLogPeriodicSpells; pet damage also PetMeleeDamage or
  PetSpellDamage;
- heals (0x722440): the player's own only, on others, CombatHealing; `+%d`;
- miss words: COMBAT_TEXT_<kind> (MISS, RESIST, DODGE, PARRY, BLOCK, EVADE,
  IMMUNE, DEFLECT, ABSORB, REFLECT; table 0xadbfdc).

**Types** (table 0xaf4748, stride 0x1c: rise yd, fade-in ms, fade-out start
ms, duration ms, height start/end DDC):

| # | Use | Rise | In | Out from | Life | Height |
|---|---|---|---|---|---|---|
| 0 | damage | 2 | 150 | 760 | 1500 | 0.018333 |
| 1 | absorb | 2 | 150 | 90 | 1500 | 0.018333 |
| 2 | crit damage | 0 | 150 | 1000 | 1500 | 0.0275 × curve |
| 3 | miss kinds | 2 | 150 | 1000 | 1500 | 0.018333 |
| 6 | heal (green) | 2 | 150 | 760 | 1500 | 0.018333 |
| 7 | crit heal | 0 | 150 | 1000 | 1500 | 0.0275 × curve |
| 8/9 | special damage/heal | 6 | 150 | 760 | 1500 | 0.0275 |
| 10 | threat (orange) | 2 | 150 | 760 | 1500 | 0.018333 |

The crit curve (0xa41618) over t/life: 0.1 → 2.0 by 0.1, back to 1.0 by 0.2,
then 1.0: it pops to twice its size at 150 ms and settles by 300 ms.

**Colour**: the type's (white, green for heals), overridden by the source:
the player's spells and every pet spell (255,222,0) (0xadaa6c), a pet's
swing (255,132,0) (0xadaa70).

**Font**: DAMAGE_TEXT_FONT, no outline; shadow black at half the alpha,
0.002 DDC right and down (0xaf487c).

**Alpha** (0x7e6850): 255 × t / fade-in during the fade-in; after the
fade-out start, linear to 0 at the end of life.

**Motion**: from the name position − 1/3 yd (0x7e5c30), lowered under the
top of the view when it would start above it; z += rise × t / life in world
yards; projected each frame and clamped so the text stays whole on screen.

**Stacking**: four texts per unit at most; a fifth is dropped. No offsets:
concurrent numbers overlap.

**Size (not stock)**: the Combat Text page's *Text size* (`combattextscale`,
50-200 %, 100 = the heights above) multiplies the world text's height and its
shadow offset, this client's stand-in floating text, and Blizzard_CombatText's
`COMBAT_TEXT_HEIGHT` / `COMBAT_TEXT_CRIT_MINHEIGHT` / `_MAXHEIGHT` (25 / 30 /
60), which the addon reads each time it sizes a line and resets when its file
runs (`ui/combat_text_size.hpp`, `addons::applyCombatTextHeights`).

## 6. Chat bubbles (CGChatBubbleFrame, ChatBubbleFrame.cpp)

**Which lines** (0x720010, from the chat handler 0x509dd0 / 0x50a970, for any
visible speaker including the player): SAY, YELL, MONSTER_SAY, MONSTER_YELL
under chatBubbles; PARTY and PARTY_LEADER under chatBubblesParty. Nothing
else. A monster line has `%%` collapsed first. **No bubble for a unit with a
nameplate**, nor for a speaker more than **25 yd** from the player. One per
speaker; a new line replaces it.

**Art** (0x56cad0): backdrop bg ChatBubble-Background, edge
ChatBubble-Backdrop, tile 16, edge 16, insets 16 (interface units). Tail
ChatBubble-Tail 16 × 16, TOPRIGHT at the bubble's BOTTOM +(0, 4).

**Text**: NAMEPLATE_FONT 0.01 DDC, no outline, the chat type's colour
(ChatTypeInfo, 0xb74728). Padding 0.01 DDC each side. Width = clamp(natural,
32 units, 0.2 DDC); longer text wraps at 0.2 DDC.

**Time** (0x56c1c0): a "break" is white space after a non-space. Others'
lines last 2750 + 750 × breaks ms, the player's own 1500 + 500 × breaks.
Fade in 250 ms; after the life + 250 ms a 250 ms fade out, then freed. Out of
range or out of view it fades out without being freed and comes back.

**Where** (0x56c500 / 0x56c340): BOTTOM at the projection of the speaker's
position raised to the name attachment's height + 0.7 yd. No overlap
avoidance; newer bubbles draw on top (0x56d050).

## 7. What Wowee had wrong, and what it does now

| # | Stock | Wowee before | Now |
|---|---|---|---|
| 1 | Plates sized in DDC (diagonal) | 128 × 32 per 768 of height | `ddcPixels`; wider on 16:9 |
| 2 | Plate TOP at name attachment + 2/3 yd | bottom at feet + 2.3 yd | attachment 18 + 2/3 yd |
| 3 | No line-of-sight test on plates | hidden behind WMO walls | shown through walls |
| 4 | No distance fade on plates | faded over the last 5 yd | none |
| 5 | nameplateAllowOverlap default 1 | 0 | 1; overlap draws far to near, target on top |
| 6 | Unfriendly plate yellow, no tapped grey | red; grey tapped | yellow; no grey |
| 7 | Class colour for hostile players only | any player | hostile only |
| 8 | Threat: Flash outline, status 1-3 colours | Nameplate-Glow red when targeted | stock |
| 9 | Highlight: nearest non-target plate, glow + yellow name | glow on target and hover | stock |
| 10 | Name red 5 s after damage | — | from combat text |
| 11 | Name/level: shadow, 0.01 / 0.009 DDC, level at (116.2, 22.9) | outline, 12 / 9, (114.5, 23.5) | stock |
| 12 | Level colours: plate table | GetQuestDifficultyColor | stock |
| 13 | Elite icon cropped 37 × 27, raid mark 0.02 at the left | whole 64 × 32; 22 px | stock |
| 14 | Cast bar: target only | any friendly, or any with the CVar | target only |
| 15 | V / Shift-V default off | enemies on | off; nameplateShowFriends bound |
| 16 | Names over heads by UnitName bits, 3D, thick outline | none (own name only) | drawn; depth test stood in for by the WMO sight query |
| 17 | Raid mark over head: 1 yd world square, camera fade | 32 px fixed | stock |
| 18 | World text: types, timings, crit pop, no stagger, 4 per unit | 1.2 yd/s rise, random x, all kinds | stock table |
| 19 | Bubbles: types, lifetime, fade, 25 yd, sizes, tail side | 8 s + 0.06 s/char, raid too, 60 yd | stock |
| 20 | UnitName/nameplate CVar defaults | several wrong | stock (§1) |

## 8. Not done / not verified

- Plates: the non-overlap stacking walk 0x615e10 is approximated by lifting
  clear of nearer plates; the cast bar's hold-and-fade after a cast ends is
  not drawn; the 0xbd088c map-type exception and the arena flag check in
  0x72b060 are not traced.
- Names: depth is a collision query against WMOs, not the depth buffer
  (terrain and M2s do not hide a name); the status prefixes (vtable
  +0x108..+0x114) and the summon properties' owner titles are not drawn;
  other players' titles come from PLAYER_CHOSEN_TITLE when the server sends
  it.
- Pet vs guardian uses the summoner and PLAYER_CONTROLLED, not 0x71b600; the
  owner is SUMMONEDBY, where stock reads CHARMEDBY then CREATEDBY.
- World text: types 4, 5 (honor with rank badge), 8/9 and the threat
  floaters (threatWorldText, type 10) are not produced; the "lowered under
  the top of the view" clamp is not done. combat_text_filter.hpp now gates
  periodic heals by CombatHealing and a pet's spells by PetSpellDamage.
- Bubbles: ChatTypeInfo colours are the defaults, not the player's recolours.
- The red name after damage only sees damage this client logs (the player's
  and the player's pet's, and damage to the player), not every unit's.

## 9. Which auras are listed (UnitAura, 0x614D40)

UnitAura, UnitBuff and UnitDebuff (0x614D40 / 0x614CA0 / 0x614CF0 ->
0x6147C0) count their index into the unit's visible-aura list (CGUnit_C
+0xDD4 helpful, +0xE58 harmful; index lookup 0x72C9B0), not into its aura
slots. 0x72C510 rebuilds that list, and an aura is in it only when:

- its slot has a spell and an effect mask (`flags & 7`);
- Spell.dbc has a row for the spell: the table at 0xAD49D4 is read directly,
  and an id outside it or a hole in it is skipped;
- 0x53D580 passes the row:
  - `Attributes & 0x80` (DO_NOT_DISPLAY) or `AttributesEx & 0x10000000`
    (DONT_DISPLAY_IN_AURA_BAR): never;
  - `Attributes & 0x40` (passive): only with an area-aura effect (0x76BED0:
    27, 35, 65, 119, 128, 129, 143);
  - `AttributesEx5 & 0x10000000` hides it when self-cast (aura flag 0x08),
    `& 0x08000000` when someone else cast it;
  - `AttributesEx6 & 0x100000`: only when the player or its active pet (the
    PetInfo list at 0xC23500, entry 0) cast it;
  - `RequiredAuraVision` (column 221): only when the player has that vision
    bit (0x4F72E0, the player's +0x10E7 byte).

An aura that fails is not drawn as "Unknown" or blank: it is not there, and
the auras after it move up an index. A realm's own spells - mod-individual-
progression's Phase Shift: IPP (89511) on the lab, which only the server's
spell_dbc has - are the common missing row. Victorious (32216) has
Attributes 0x180, so a 3.3.5 warrior never sees it as a buff either; the
Victory Rush button lights from the aura state instead.

Wowee: `game::stock_aura::listed`, through `GameHandler::isAuraListed`, in
UnitAura/UnitBuff/UnitDebuff, GetPlayerBuff and CancelUnitBuff's index.
Before, every slot with a spell id was listed, and a missing row was an
"Unknown" buff with no icon.
