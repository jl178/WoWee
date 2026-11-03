// minimap_stock.hpp - what the 3.3.5a minimap puts on the map, and where.
//
// Rebuilt from Wow.exe 12340 (MinimapFrame.cpp and Minimap.cpp in the client's
// own source paths); docs/re/maps.md has the addresses and the reasoning. Pure
// functions only, so the rules can be tested without a client: the marker pass
// (game_screen_minimap.cpp), the Lua bindings (Minimap:PingLocation,
// GetPingPosition, MINIMAP_PING) and the tracking menu all read them from here.
//
// Positions are in the client's world order, which is also this engine's
// render order and the order on the wire: x north, y west. A distance on the
// map is a fraction of the Minimap frame's width; the frame spans 2 * radius
// yards, the radius being the zoom table's (Minimap::kZoomRadius).
#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdint>

namespace wowee::ui::minimap_stock {

// ---- Sizes, in interface units ----------------------------------------------
//
// 0x0057DCA0 builds the blip quads from four .rdata fractions of the screen
// height basis (0x00A11F88..94) times 5/3 (0x00A0B634): 0.0125, 0.0375, 0.045
// and 0.055 of a 0.6-high 4:3 screen, which is 768 interface units - so the
// sizes are fixed in interface units and scale only with the frame's own
// scale, whatever the resolution.

/// Every ObjectIcons and PartyRaidBlips blip (0.0125, 0x00A11F94).
inline constexpr float kBlipSize = 16.0f;
/// The rotating vehicle icons (0.0375, 0x00A11F90).
inline constexpr float kVehicleQuad = 48.0f;
/// The rim arrows' quad (0.045, 0x00A11F8C). The arrow texture is mapped onto
/// it turned 45 degrees with its corners on the quad's edges, so the texture
/// itself shows kRimArrowQuad / sqrt(2) across.
inline constexpr float kRimArrowQuad = 57.6f;
inline constexpr float kRimArrowArt = 40.729351f;
/// The rim arrows sit 0.8 (0x00A11F84, 0x00A11C4C) of 70.4 (0.055,
/// 0x00A11F88) from the centre, whatever the frame's size.
inline constexpr float kRimArrowRing = 70.4f * 0.8f;
/// The arrow art points along its diagonal; 0x00ACEC64 turns it onto the
/// direction (3/4 of pi).
inline constexpr float kRimArrowArtTurn = 2.35619449f;

// ---- Ranges --------------------------------------------------------------

/// Nothing is drawn further out than 0.8 of the map's radius (0x00A11C4C for
/// blips, 0x00A34C18 for the group and the POIs); past it a party member or a
/// POI becomes a rim arrow instead.
inline constexpr float kOnMapFraction = 0.8f;
/// An AreaPOI further than this gets no rim arrow (0x00A41E00).
inline constexpr float kPoiArrowRange = 694.44446f;
/// At most this many POI arrows at once, the lowest Importance first.
inline constexpr int kPoiArrowCount = 3;
/// With the player standing still the blips are gathered again once a second
/// (0x00581E80: 999 < now - last); while the player moves, every frame.
inline constexpr uint32_t kBlipRefreshMs = 1000;

// ---- Placement ----------------------------------------------------------

/// Where a point dNorth / dWest yards from the player lands, as a fraction of
/// the frame's width from its centre, +x to the right (east on a north-up map)
/// and +y up. 0x0057C660 places it on the unturned map; with rotateMinimap the
/// frame then turns the whole layer by minus the player's facing (0x004C2300 with
/// the matrix from 0x004C3460), so the way the player faces is up.
inline glm::vec2 mapOffset(float dNorth, float dWest, float radius, float bearing) {
    if (radius <= 0.0f) return {0.0f, 0.0f};
    const float k = 0.5f / radius;
    const float x = -dWest * k;
    const float y = dNorth * k;
    const float c = std::cos(bearing), s = std::sin(bearing);
    return {x * c + y * s, -x * s + y * c};
}

/// The other way: a point on the map, as a fraction of the width from the
/// centre (+y up), back to yards north and west of the player. What
/// Minimap:PingLocation does to the click (0x0057ED70). Returns (north, west).
inline glm::vec2 worldOffset(float fx, float fy, float radius, float bearing) {
    const float c = std::cos(bearing), s = std::sin(bearing);
    const float x = fx * c - fy * s;
    const float y = fx * s + fy * c;
    return {y * 2.0f * radius, -x * 2.0f * radius};
}

/// Close enough to be drawn on the map rather than as an arrow on its rim.
inline bool onMap(float dNorth, float dWest, float radius) {
    if (radius <= 0.0f) return false;
    return std::sqrt(dNorth * dNorth + dWest * dWest) / radius <= kOnMapFraction;
}

/// The direction from the player to a point, as 0x004F5130 has it: 0 north, a
/// quarter turn west, the axes' special cases kept.
inline float rimAngle(float dNorth, float dWest) {
    constexpr float kEps = 2.384185791015625e-07f;  // 0x009EA27C
    constexpr float kPi = 3.14159265358979f;
    if (std::fabs(dNorth) < kEps) return dWest < 0.0f ? 1.5f * kPi : 0.5f * kPi;
    if (std::fabs(dWest) < kEps) return dNorth < 0.0f ? kPi : 0.0f;
    return std::atan2(dWest, dNorth);
}

/// A rim arrow's centre, in interface units from the map's centre (+y up), and
/// the direction its tip points (the same direction, outward).
inline glm::vec2 rimArrowOffset(float angle, float bearing) {
    const float a = angle - bearing;
    return {-kRimArrowRing * std::sin(a), kRimArrowRing * std::cos(a)};
}

// ---- What a blip is --------------------------------------------------------
//
// 0x0057F7F0 sorts every object near the player into one of the lists below,
// and the list is the cell of Interface\Minimap\ObjectIcons (8 x 2, 0x005832F0)
// it is drawn with. List 15 is the empty sixteenth cell, so the player's own
// pet is classified and then draws nothing.

enum Cell : int {
    kNoBlip = -1,
    kHostilePlayer = 2,       // red, silver rim
    kNeutralPlayer = 3,       // gold, silver rim: the other faction, not attackable
    kFriendlyPlayer = 4,      // green, silver rim
    kHostileNpc = 5,          // red, gold rim
    kNeutralNpc = 6,          // gold, gold rim
    kFriendlyNpc = 7,         // green, gold rim
    kTracked = 8,             // gold: a tracked herb, vein or fishing pool, a
                              // tracked town service, a mailbox
    kQuestAvailable = 9,      // yellow !
    kQuestReward = 10,        // yellow ?
    kRepeatableAvailable = 11,// blue !
    kTaxiUndiscovered = 13,   // green !: a flight master whose node is unknown
    kOwnPet = 15,             // the empty cell
};

/// A quest giver's status (SMSG_QUESTGIVER_STATUS) as a cell, or kNoBlip.
/// Only these five are on the map: 10 a reward, 8 a quest, 7 a repeatable one,
/// and 2 / 4 - a low-level quest and a low-level repeatable - only while
/// "Low Level Quests" is the tracking. Incomplete (5), the repeatable reward
/// (6) and 9 are not drawn at all.
inline int questStatusCell(uint32_t status, bool trivialTracking) {
    switch (status) {
        case 10: return kQuestReward;
        case 8: return kQuestAvailable;
        case 2: return trivialTracking ? kQuestAvailable : kNoBlip;
        case 7: return kRepeatableAvailable;
        case 4: return trivialTracking ? kRepeatableAvailable : kNoBlip;
        default: return kNoBlip;
    }
}

/// What 0x0057F7F0 asks of a unit or a player.
struct UnitFacts {
    bool isPlayer = false;
    /// In the player's party or raid: drawn from PartyRaidBlips instead.
    bool groupMember = false;
    /// Charmed or summoned by the player (UNIT_FIELD_CHARMEDBY, then
    /// SUMMONEDBY): the empty cell.
    bool ownPet = false;
    /// Health 0, and the creature is not flagged visible to ghosts (type flag
    /// 0x80, 0x00715E30) - a spirit healer is the exception that stays.
    bool deadAndHidden = false;
    /// The town tracking in force names one of the unit's NPC flags and the
    /// unit is one the player could deal with (0x0071ACF0, 0x00729530).
    bool townTracked = false;
    uint32_t questStatus = 0;
    /// SMSG_TAXINODE_STATUS said the node is not known (0x006D5FC0 stores 7).
    bool taxiUndiscovered = false;
    /// Creature tracking covers it, or the unit carries UNIT_DYNFLAG_TRACK_UNIT
    /// (0x006DCA00). Without this a plain NPC or player is not drawn.
    bool creatureTracked = false;
    /// Players: can be attacked (0x00729740), and on the player's own team.
    bool attackable = false;
    bool sameTeam = false;
    /// NPCs: the reaction to the player, 1 hated .. 8 exalted (UnitReaction).
    int reaction = 4;
};

inline int classifyUnit(const UnitFacts& u, bool trivialTracking) {
    if (u.isPlayer && u.groupMember) return kNoBlip;
    if (u.ownPet) return kOwnPet;
    if (u.deadAndHidden) return kNoBlip;
    if (u.townTracked) return kTracked;
    if (!u.isPlayer) {
        const int quest = questStatusCell(u.questStatus, trivialTracking);
        if (quest != kNoBlip) return quest;
        if (u.taxiUndiscovered) return kTaxiUndiscovered;
    }
    if (!u.creatureTracked) return kNoBlip;
    if (u.isPlayer) {
        if (u.attackable) return kHostilePlayer;
        return u.sameTeam ? kFriendlyPlayer : kNeutralPlayer;
    }
    // 0x007251C0's 0-based reaction: 4 and up friendly, 2-3 neutral, 0-1 hostile.
    if (u.reaction >= 5) return kFriendlyNpc;
    if (u.reaction >= 3) return kNeutralNpc;
    return kHostileNpc;
}

/// What 0x0057F7F0 asks of a game object.
struct GameObjectFacts {
    /// The tracking in force is the mailbox (type 2, GO type 19) and the
    /// object is one (0x0070F850, 0x0071F8B0).
    bool mailboxTracked = false;
    uint32_t questStatus = 0;
    /// A skill lock on it (Lock.dbc type 2) is of a LockType the player's
    /// PLAYER_TRACK_RESOURCES has a bit for (0x006DCA90): Find Herbs, Find
    /// Minerals, Find Fish's pools, Find Treasure.
    bool resourceTracked = false;
};

inline int classifyGameObject(const GameObjectFacts& g, bool trivialTracking) {
    if (g.mailboxTracked) return kTracked;
    const int quest = questStatusCell(g.questStatus, trivialTracking);
    if (quest != kNoBlip) return quest;
    if (g.resourceTracked) return kTracked;
    return kNoBlip;
}

// ---- Town tracking ----------------------------------------------------------
//
// The fifteen entries the tracking menu offers after the player's tracking
// spells (0x00A11C50, 0x14 bytes each). Type 1 matches UNIT_NPC_FLAGS against
// the mask, type 2 a game object type, type 3 is "Low Level Quests". The class
// mask is 1 << class; zero means every class.

struct TownTracking {
    int type;
    uint32_t mask;
    const char* globalString;   ///< the menu text, a GlobalStrings key
    const char* texture;        ///< under Interface\Minimap\Tracking
    uint32_t classMask;
};

inline constexpr std::array<TownTracking, 15> kTownTracking{{
    {1, 0x00001000, "MINIMAP_TRACKING_REPAIR", "Repair", 0},
    {1, 0x00000200, "MINIMAP_TRACKING_VENDOR_FOOD", "Food", 0},
    {1, 0x00000400, "MINIMAP_TRACKING_VENDOR_POISON", "Poisons", 0x10},
    {1, 0x00000100, "MINIMAP_TRACKING_VENDOR_AMMO", "Ammunition", 0x1a},
    {1, 0x00000800, "MINIMAP_TRACKING_VENDOR_REAGENT", "Reagents", 0},
    {1, 0x00010000, "MINIMAP_TRACKING_INNKEEPER", "Innkeeper", 0},
    {1, 0x00002000, "MINIMAP_TRACKING_FLIGHTMASTER", "FlightMaster", 0},
    {1, 0x00400000, "MINIMAP_TRACKING_STABLEMASTER", "StableMaster", 0x8},
    {1, 0x00100000, "MINIMAP_TRACKING_BATTLEMASTER", "BattleMaster", 0},
    {1, 0x00000020, "MINIMAP_TRACKING_TRAINER_CLASS", "Class", 0},
    {1, 0x00000040, "MINIMAP_TRACKING_TRAINER_PROFESSION", "Profession", 0},
    {1, 0x00200000, "MINIMAP_TRACKING_AUCTIONEER", "Auctioneer", 0},
    {1, 0x00020000, "MINIMAP_TRACKING_BANKER", "Banker", 0},
    {2, 19, "MINIMAP_TRACKING_MAILBOX", "Mailbox", 0},
    {3, 0, "MINIMAP_TRACKING_TRIVIAL_QUESTS", "TrivialQuests", 0},
}};

/// Whether a class (1-based, as UNIT_FIELD_BYTES_0 has it) is offered an entry.
inline bool townTrackingOffered(const TownTracking& t, uint32_t classId) {
    return t.classMask == 0 || (classId < 32 && (t.classMask & (1u << classId)) != 0);
}

// ---- The POI list -----------------------------------------------------------
//
// AreaPOI rows of the current map with flag 0x1 (0x007F6730) and four slots of
// the client's own: 0 the selected quest's point, 1 a gossip POI, 2 the corpse
// (CORPSE_RED), 3 the spirit healer to release at. Within 0.8 of the radius a
// POI with flag 0x2 is drawn as its POIIcons icon; anything further out that
// is within 694 yards competes for the three rim arrows (0x007F44A0).

/// AreaPOI.dbc flags the minimap reads.
inline constexpr uint32_t kPoiOnMinimap = 0x1;
inline constexpr uint32_t kPoiIconOnMinimap = 0x2;

/// POIIcons.blp: 14 to a row (0x004C4E10 of 196) on an 18-texel pitch
/// (0x00A120B0), each sampled from one texel in to the pitch's end.
inline constexpr int kPoiIconColumns = 14;
inline constexpr float kPoiIconPitch = 18.0f;
inline constexpr int kPoiIconCount = 196;

struct PoiArrowCandidate {
    int importance = 0;
    float distance = 0.0f;
    bool always = false;  ///< the corpse: no distance limit
};

/// Which of the candidates get the three arrows, as indices, by the client's
/// own selection: the lowest Importance wins, a tie goes to the nearer one,
/// and the first three seen fill the slots before anything is compared.
template <typename Vec>
inline std::array<int, kPoiArrowCount> pickPoiArrows(const Vec& candidates, int& count) {
    std::array<int, kPoiArrowCount> slot{-1, -1, -1};
    std::array<int, kPoiArrowCount> imp{};
    std::array<float, kPoiArrowCount> dist{};
    count = 0;
    int worst = -1;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        const auto& c = candidates[static_cast<size_t>(i)];
        if (!(c.distance <= kPoiArrowRange || c.always)) continue;
        if (worst < 0) {
            slot[count] = i; imp[count] = c.importance; dist[count] = c.distance;
            ++count;
        } else if (c.importance < imp[worst] ||
                   (c.importance == imp[worst] && c.distance < dist[worst])) {
            slot[worst] = i; imp[worst] = c.importance; dist[worst] = c.distance;
        }
        if (count == kPoiArrowCount) {
            worst = 0;
            for (int k = 1; k < kPoiArrowCount; ++k) {
                if (imp[worst] < imp[k] || (imp[k] == imp[worst] && dist[worst] < dist[k])) worst = k;
            }
        }
    }
    return slot;
}

}  // namespace wowee::ui::minimap_stock
