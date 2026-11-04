#pragma once

// The stock 3.3.5a (build 12340) battleground client, CGBattlefieldInfo in
// Wow.exe's BattlefieldInfo.cpp, as rules this client can test without a
// server: the queue slots SMSG_BATTLEFIELD_STATUS fills (0x54AE40), the
// battlemaster's list (0x54E390), the scoreboard MSG_PVP_LOG_DATA fills and
// its sort (0x54D280, 0x54AA30, 0x54A740), and the WorldStateUI.dbc lines and
// scoreboard columns (0x5487D0, 0x548D40, 0x5770D0). docs/re/pvp-instances.md;
// tests/test_battlefield_rules.cpp.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wowee::network { class Packet; }

namespace wowee::game::bf {

/// A monotonic millisecond clock, never zero (zero means "not set" here).
int64_t clockMs();

/// Two queues. 0x54AE40 drops a status for any slot past the second.
inline constexpr uint32_t kQueueSlots = 2;
/// BattlemasterList id of the random battleground.
inline constexpr uint32_t kRandomBattleground = 32;
/// The scoreboard holds eighty rows (0x54E720 allocates 0x140 / 4).
inline constexpr size_t kMaxScores = 80;

// ---------------------------------------------------------------------------
// SMSG_BATTLEFIELD_STATUS
// ---------------------------------------------------------------------------

/// One status as AzerothCore's BuildBattlegroundStatusPacket writes it.
struct StatusPacket {
    uint32_t queueSlot = 0;
    /// The eight bytes after the slot, kept whole: arenaType, isArena,
    /// bgTypeId, 0x1F90. CMSG_BATTLEFIELD_PORT and CMSG_LEAVE_BATTLEFIELD send
    /// them back as they came (0x54DA10, 0x54C250). Zero is STATUS_NONE.
    uint64_t header = 0;
    uint32_t bgTypeId = 0;
    uint8_t teamSize = 0;          ///< arenaType & 0x7F
    uint8_t minLevel = 0;
    uint8_t maxLevel = 0;
    uint32_t instanceId = 0;
    bool rated = false;
    uint32_t status = 0;           ///< 0 none, 1 queued, 2 confirm, 3 active
    uint32_t avgWaitMs = 0;        ///< 1
    uint32_t timeInQueueMs = 0;    ///< 1
    uint32_t mapId = 0;            ///< 2, 3
    uint32_t inviteTimeoutMs = 0;  ///< 2
    uint32_t autoLeaveMs = 0;      ///< 3: 0 while it runs, 120000 after the end
    uint32_t elapsedMs = 0;        ///< 3: since the battle started
    bool arenaFaction = false;     ///< 3: 1 Alliance (gold), 0 Horde (green)
};

/// Reads the WotLK layout. False only for a packet too short for its slot or
/// header; the tail of each status is read as far as it goes.
bool parseStatus(network::Packet& packet, StatusPacket& out);

/// What one queue slot holds (the 0x40-byte rows at 0xBEA4B8).
struct QueueSlot {
    uint64_t header = 0;
    uint32_t status = 0;
    uint32_t bgTypeId = 0;
    uint8_t teamSize = 0;
    uint8_t minLevel = 0;
    uint8_t maxLevel = 0;
    uint32_t instanceId = 0;
    bool rated = false;
    uint32_t mapId = 0;            ///< the invitation's map
    int64_t portExpiresAtMs = 0;   ///< 0: none
    uint32_t avgWaitMs = 0;
    int64_t queuedSinceMs = 0;     ///< 0: none
};

/// The battleground the player stands in (0xBEA564..0xBEA578, 0xACD16C).
struct ActiveBattlefield {
    int slot = -1;
    uint32_t mapId = 0;
    uint32_t instanceType = 0;     ///< Map.dbc InstanceType: 3 battleground, 4 arena
    int64_t expiresAtMs = 0;       ///< 0: no shutdown pending
    int64_t startedAtMs = 0;       ///< 0: not started
    bool arenaFaction = false;
};

/// What applying a status asks of the caller.
struct StatusEffect {
    int slot = -1;                 ///< -1: ignored; else fire UPDATE_BATTLEFIELD_STATUS(slot + 1)
    bool invited = false;          ///< status 2 arrived
    bool enteredActive = false;    ///< status 3: reset the scoreboard filter and columns
    bool leftActive = false;       ///< the active battleground's slot went away
};

class Queues {
public:
    /// 0x54AE40. `mapInstanceType` answers Map.dbc's InstanceType.
    StatusEffect apply(const StatusPacket& s, int64_t nowMs,
                       const std::function<uint32_t(uint32_t)>& mapInstanceType);
    void reset();

    [[nodiscard]] const QueueSlot* slot(uint32_t index) const {
        return index < kQueueSlots ? &slots_[index] : nullptr;
    }
    [[nodiscard]] const ActiveBattlefield& active() const { return active_; }
    [[nodiscard]] bool hasInvite() const;

    // The Lua timers, as the stock bindings compute them.
    /// GetBattlefieldPortExpiration(i): whole seconds left (0x549B80).
    [[nodiscard]] uint32_t portExpirationSec(uint32_t index, int64_t nowMs) const;
    /// GetBattlefieldTimeWaited(i): milliseconds since the queue began (0x549DD0).
    [[nodiscard]] int64_t timeWaitedMs(uint32_t index, int64_t nowMs) const;
    /// GetBattlefieldEstimatedWaitTime(i): milliseconds (0x549D30).
    [[nodiscard]] uint32_t estimatedWaitMs(uint32_t index) const;
    /// GetBattlefieldInstanceExpiration(): milliseconds to the shutdown (0x549C40).
    [[nodiscard]] int64_t instanceExpirationMs(int64_t nowMs) const;
    /// GetBattlefieldInstanceRunTime(): milliseconds since the start (0x549CD0).
    [[nodiscard]] int64_t instanceRunTimeMs(int64_t nowMs) const;

private:
    std::array<QueueSlot, kQueueSlots> slots_{};
    ActiveBattlefield active_{};
};

/// GetBattlefieldStatus's first return (0x54BC30).
const char* statusToken(uint32_t status);

// ---------------------------------------------------------------------------
// SMSG_BATTLEFIELD_LIST
// ---------------------------------------------------------------------------

struct ListPacket {
    uint64_t battlemaster = 0;
    bool fromAnywhere = false;     ///< the "from where" byte; the PvP frame's request
    uint32_t bgTypeId = 0;
    uint8_t minLevel = 0;          ///< both written as zero by AzerothCore
    uint8_t maxLevel = 0;
    bool hasWin = false;           ///< the day's first win already had
    uint32_t winHonor = 0;
    uint32_t winArena = 0;
    uint32_t lossHonor = 0;
    bool isRandom = false;
    bool hasWinRandom = false;
    uint32_t winHonorRandom = 0;
    uint32_t winArenaRandom = 0;
    uint32_t lossHonorRandom = 0;
    std::vector<uint32_t> instanceIds;
};

bool parseList(network::Packet& packet, ListPacket& out);

/// Which event a list raises (0x54E390): the random battleground from a
/// battlemaster is NPC_PVPQUEUE_ANYWHERE, a list asked for from anywhere is
/// PVPQUEUE_ANYWHERE_SHOW, a battlemaster's list BATTLEFIELDS_SHOW, and a list
/// with no battlemaster closes the frame (BATTLEFIELDS_CLOSED).
const char* listEvent(const ListPacket& list);

// ---------------------------------------------------------------------------
// MSG_PVP_LOG_DATA
// ---------------------------------------------------------------------------

struct ScoreRow {
    uint64_t guid = 0;
    uint32_t killingBlows = 0;
    uint32_t honorableKills = 0;
    uint32_t deaths = 0;
    uint32_t honorGained = 0;
    uint32_t damageDone = 0;
    uint32_t healingDone = 0;
    /// The arena's team byte; in a battleground the race's faction once the
    /// name is known (0x54AA30): 0 Horde, 1 Alliance, -1 neither.
    int faction = -1;
    std::array<uint32_t, 8> stats{};
    // From the name cache, for the sort and GetBattlefieldScore.
    std::string name;
    uint32_t raceId = 0;
    uint32_t classId = 0;
};

struct ArenaTeamInfo {
    std::string name;
    /// The three numbers as sent; GetBattlefieldTeamInfo answers them raw
    /// (0x54A180) and the frame subtracts the first from the second.
    uint32_t rating1 = 0;
    uint32_t rating2 = 0;
    uint32_t mmr = 0;
};

struct PvpLogPacket {
    bool isArena = false;
    std::array<ArenaTeamInfo, 2> teams{};
    bool ended = false;
    uint8_t winner = 0;
    std::vector<ScoreRow> rows;
};

bool parsePvpLog(network::Packet& packet, PvpLogPacket& out);

/// The sort SortBattlefieldScoreData keeps (0x54CEE0): seventeen keys, the
/// last chosen first, each remembering its own direction.
class ScoreSort {
public:
    ScoreSort();
    /// SortBattlefieldScoreData(name) (0x54DE00). Unknown names re-sort only.
    void select(const std::string& column);
    void selectKey(int key);
    /// a before b? Ties fall through to the next key.
    [[nodiscard]] bool before(const ScoreRow& a, const ScoreRow& b, int factionFilter,
                              size_t statColumns) const;
    /// -1 no such column; 0 kills, 1 deaths, 2 cp, 3 name, 4 class, 5 hk,
    /// 6 damage, 7 healing, 8 team, 9.. stat1..
    static int keyOf(const std::string& column);

private:
    struct Key { int id; bool reversed; };
    std::array<Key, 17> keys_{};
};

/// The scoreboard as the bindings read it: sorted, the filtered faction first
/// (0x54AA30, 0x54A740), with the count the filter passes.
struct Scoreboard {
    PvpLogPacket log;
    bool hasData = false;
    int factionFilter = -1;        ///< SetBattlefieldScoreFaction: -1 both
    ScoreSort sort;
    size_t shown = 0;              ///< GetNumBattlefieldScores

    void resort(size_t statColumns);
};

/// A battleground row's faction from its race (0x54AA30 reads ChrRaces'
/// FactionTemplate and its group mask: Horde bit 4 -> 0, Alliance bit 2 -> 1,
/// otherwise -1). For the ten playable races that is this table.
int factionOfRace(uint32_t raceId);

// ---------------------------------------------------------------------------
// WorldStateUI.dbc
// ---------------------------------------------------------------------------

struct WorldStateUiRow {
    uint32_t id = 0;
    int32_t mapId = -1;
    uint32_t areaId = 0;
    uint32_t phaseMask = 0;
    std::string icon;
    std::string text;
    std::string tooltip;
    uint32_t stateVariable = 0;
    uint32_t type = 0;             ///< 0/1/3 the frame's lines, 2 a scoreboard column
    std::string dynamicIcon;
    std::string dynamicTooltip;
    std::string extendedUI;
    std::array<uint32_t, 3> extendedUIStateVariable{};
};

/// The rows GetNumWorldStateUI counts (0x5487D0): the map's (or every map's),
/// the zone's or area's (or no area's), the phase's, of type 0, 1 or 3, in
/// file order.
std::vector<size_t> visibleRows(const std::vector<WorldStateUiRow>& rows, uint32_t mapId,
                                uint32_t zoneId, uint32_t areaId, uint32_t phaseMask);

/// The scoreboard's own columns for a battleground (0x54AE40): type 2 rows of
/// its map, the first contiguous run.
std::vector<size_t> scoreColumns(const std::vector<WorldStateUiRow>& rows, uint32_t mapId);

/// The text with its world states filled in (0x5770D0): %<id>w the value,
/// %<id>k the time left to the value as a server timestamp (MM:SS, or
/// H:MM:SS from an hour). `serverNow` is the server's clock in seconds.
std::string formatText(const std::string& text,
                       const std::function<uint32_t(uint32_t)>& worldState,
                       int64_t serverNow);

// ---------------------------------------------------------------------------
// SMSG_UPDATE_INSTANCE_ENCOUNTER_UNIT (0x5EDDD0)
// ---------------------------------------------------------------------------

/// The encounter's units behind "boss1".."boss4": up to sixteen, kept packed
/// and sorted by frame priority, then by when they engaged (0x5ED750,
/// 0x5ED5F0); "bossN" is the N-th (0x5ED710).
class EncounterUnits {
public:
    static constexpr size_t kMax = 16;
    /// Engage (0x5ED7B0): a unit already present changes nothing (false).
    bool engage(uint64_t guid, uint8_t priority);
    /// Disengage: the unit leaves and the rest close up.
    bool disengage(uint64_t guid);
    /// Update priority (0x5ED930): only for a unit already present.
    bool updatePriority(uint64_t guid, uint8_t priority);
    void clear() { units_.clear(); }
    [[nodiscard]] uint64_t guidAt(size_t index) const {
        return index < units_.size() ? units_[index].guid : 0;
    }
    [[nodiscard]] size_t size() const { return units_.size(); }

private:
    struct Unit { uint64_t guid; uint8_t priority; uint32_t order; };
    void sort();
    std::vector<Unit> units_;
    uint32_t nextOrder_ = 0;
};

}  // namespace wowee::game::bf
