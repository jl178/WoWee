#pragma once

// The stock 3.3.5a (build 12340) arena client as rules this client can test
// without a server: the opponents behind "arena1".."arena5" and
// "arenapet1".."arenapet5" (CGBattlefieldInfo, BattlefieldInfo.cpp: 0x54D100,
// 0x54D1C0, 0x54B840, 0x54B920, 0x54ABF0, 0x54AD10, 0x54CA50), and the arena
// team records the PvP frame reads (ArenaTeamInfo.cpp: 0x5A3700, 0x5A3860,
// 0x5A3E10, 0x5E7B00). docs/re/pvp-instances.md §3; tests/test_arena_rules.cpp.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wowee::network { class Packet; }

namespace wowee::game::arena {

/// Five of each (the 0x50-byte pet table at 0xBE9EF8, the five guids at 0xBE9F48).
inline constexpr size_t kMaxOpponents = 5;

/// One ARENA_OPPONENT_UPDATE: the unit token and "seen", "unseen",
/// "destroyed" or "cleared".
struct OpponentEvent {
    std::string unit;
    std::string what;
    bool operator==(const OpponentEvent& o) const { return unit == o.unit && what == o.what; }
};

/// The opponents of an arena match. A player slot holds a guid; a pet slot
/// holds the pet and the owner it was seen with. Slots are never compacted:
/// "arena2" stays the same player for the whole match.
class Opponents {
public:
    /// Whether an object with this guid is in view (0x4D4DB0), and the pet a
    /// visible opponent has out: its CHARM, else its SUMMON (0x60A2F0).
    using Present = std::function<bool(uint64_t)>;
    using PetOf = std::function<uint64_t(uint64_t)>;

    /// An opponent player was seen (0x54D100): the slot whose pet belongs to
    /// them, else the first slot with no pet that is theirs or empty. "seen",
    /// then "unseen" when the object is not in view (0x54B840). Nothing when
    /// all five are taken.
    std::vector<OpponentEvent> playerSeen(uint64_t guid, const Present& present);

    /// An opponent's pet was seen (0x54D1C0): the owner's player slot, else
    /// a pet slot already holding that owner, else the first slot with
    /// neither a player nor a pet. "seen", then "unseen" without the object
    /// (0x54B920).
    std::vector<OpponentEvent> petSeen(uint64_t pet, uint64_t owner, const Present& present);

    /// An opponent or their pet left view (out-of-range block, 0x54ABF0):
    /// "unseen"; the slot keeps them.
    std::vector<OpponentEvent> unseen(uint64_t guid, const Present& present, const PetOf& petOf) const;

    /// SMSG_ARENA_UNIT_DESTROYED (0x54AD10): "destroyed". A player keeps their
    /// slot; a pet found only through the pet table is dropped from it.
    std::vector<OpponentEvent> destroyed(uint64_t guid, const Present& present, const PetOf& petOf);

    /// The world was entered (0x54CA50): every slot emptied and "cleared"
    /// for arena1, arenapet1, arena2 ... arenapet5.
    std::vector<OpponentEvent> clear();

    /// GetNumArenaOpponents (0x54A2C0 -> 0x549630): the first slot with
    /// neither a player nor a pet, counted from one.
    [[nodiscard]] uint32_t count() const;

    /// "arenaN" (0x608180).
    [[nodiscard]] uint64_t player(size_t index) const {
        return index < kMaxOpponents ? players_[index] : 0;
    }
    /// "arenapetN" (0x60A2F0): the opponent's own pet while they are in view,
    /// else the pet table's.
    [[nodiscard]] uint64_t pet(size_t index, const Present& present, const PetOf& petOf) const;

    /// The slot of a player or pet guid, or -1.
    [[nodiscard]] int slotOf(uint64_t guid) const;

private:
    struct PetSlot { uint64_t pet = 0; uint64_t owner = 0; };
    std::vector<OpponentEvent> assignPlayer(uint64_t guid, size_t slot, const Present& present);
    std::vector<OpponentEvent> assignPet(uint64_t pet, uint64_t owner, size_t slot,
                                         const Present& present);
    std::array<uint64_t, kMaxOpponents> players_{};
    std::array<PetSlot, kMaxOpponents> pets_{};
};

// ---------------------------------------------------------------------------
// Arena teams
// ---------------------------------------------------------------------------

/// Three slots: 2v2, 3v3, 5v5.
inline constexpr size_t kTeamSlots = 3;
/// PLAYER_FIELD_ARENA_TEAM_INFO_1_1: seven fields a slot (AzerothCore's
/// ArenaTeamInfoType), read at 0x1150 + slot * 0x1C into the player block.
inline constexpr size_t kTeamInfoFields = 7;

/// One slot of PLAYER_FIELD_ARENA_TEAM_INFO.
struct TeamInfo {
    uint32_t teamId = 0;
    uint32_t type = 0;
    uint32_t member = 0;           ///< 0 captain, 1 member
    uint32_t gamesWeek = 0;
    uint32_t gamesSeason = 0;
    uint32_t winsSeason = 0;
    uint32_t personalRating = 0;
};

/// The seven fields of one slot, in order.
TeamInfo teamInfoFromFields(const std::array<uint32_t, kTeamInfoFields>& f);

/// SMSG_ARENA_TEAM_QUERY_RESPONSE, the team cache record (0x635480): the
/// name at +4, the type at +0x64, then the tabard at +0x68..+0x78.
struct TeamQuery {
    uint32_t teamId = 0;
    std::string name;
    uint32_t type = 0;
    uint32_t backgroundColor = 0;
    uint32_t emblemStyle = 0;
    uint32_t emblemColor = 0;
    uint32_t borderStyle = 0;
    uint32_t borderColor = 0;
};

bool parseTeamQuery(network::Packet& packet, TeamQuery& out);

/// A colour as GetArenaTeam hands it to Lua: bytes 2, 1, 0 over 255
/// (0x5A3860, 0xA45564 = 1/255).
std::array<float, 3> colorComponents(uint32_t color);

/// MSG_INSPECT_ARENA_TEAMS (0x5E7B00): the inspected player's guid, the slot
/// and six numbers. A reply for anyone but the inspected player, or for a
/// slot past 3, is dropped.
struct InspectTeam {
    uint8_t slot = 0;
    uint32_t teamId = 0;
    uint32_t rating = 0;
    uint32_t seasonGames = 0;
    uint32_t seasonWins = 0;
    uint32_t playerSeasonGames = 0;
    uint32_t personalRating = 0;
};

bool parseInspectTeam(network::Packet& packet, uint64_t& guid, InspectTeam& out);

}  // namespace wowee::game::arena
