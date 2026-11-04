// Stock 3.3.5a battleground client rules (docs/re/pvp-instances.md): the
// queue slots SMSG_BATTLEFIELD_STATUS fills (Wow.exe 0x54AE40), the
// battlemaster list (0x54E390), MSG_PVP_LOG_DATA and the scoreboard sort
// (0x54D280, 0x54A740, 0x54CEE0), WorldStateUI (0x5487D0, 0x5770D0) and the
// encounter units behind bossN (0x5EDDD0). Packets are built in
// AzerothCore's order, not from our own writers.
#include <catch_amalgamated.hpp>

#include "game/battlefield_rules.hpp"
#include "network/packet.hpp"

#include <string>
#include <vector>

using namespace wowee::game::bf;
using wowee::network::Packet;

namespace {

struct Bytes {
    std::vector<uint8_t> b;
    Bytes& u8(uint8_t v) { b.push_back(v); return *this; }
    Bytes& u16(uint16_t v) { u8(uint8_t(v)); return u8(uint8_t(v >> 8)); }
    Bytes& u32(uint32_t v) { u16(uint16_t(v)); return u16(uint16_t(v >> 16)); }
    Bytes& u64(uint64_t v) { u32(uint32_t(v)); return u32(uint32_t(v >> 32)); }
    Bytes& str(const std::string& s) { for (char c : s) u8(uint8_t(c)); return u8(0); }
    Packet packet(uint16_t op) const { return Packet(op, b); }
};

// BattlegroundMgr::BuildBattlegroundStatusPacket's header.
Bytes statusHeader(uint32_t slot, uint8_t arenaType, uint32_t bgType, uint8_t minL, uint8_t maxL,
                   uint32_t instance, bool rated, uint32_t status) {
    Bytes b;
    b.u32(slot).u8(arenaType).u8(arenaType ? 0xE : 0).u32(bgType).u16(0x1F90)
     .u8(minL).u8(maxL).u32(instance).u8(rated ? 1 : 0).u32(status);
    return b;
}

uint32_t noMap(uint32_t) { return 0; }

}  // namespace

TEST_CASE("Battlefield status: STATUS_NONE is eight zero bytes and empties the slot", "[battlefield]") {
    Queues q;
    StatusPacket s;
    auto queued = statusHeader(0, 0, 2, 10, 19, 0, false, 1).u32(120000).u32(5000).packet(0x2D4);
    REQUIRE(parseStatus(queued, s));
    CHECK(s.status == 1);
    CHECK(s.bgTypeId == 2);
    auto fx = q.apply(s, 100000, noMap);
    CHECK(fx.slot == 0);
    CHECK(q.slot(0)->status == 1);
    CHECK(q.estimatedWaitMs(0) == 120000);
    CHECK(q.timeWaitedMs(0, 107000) == 12000);

    // AzerothCore's STATUS_NONE: the slot and a uint64 zero, nothing else.
    // The old reader asked for seven more bytes and dropped it, so a queue
    // left or a battleground quit stayed on the minimap.
    auto none = Bytes().u32(0).u64(0).packet(0x2D4);
    REQUIRE(parseStatus(none, s));
    CHECK(s.header == 0);
    fx = q.apply(s, 110000, noMap);
    CHECK(fx.slot == 0);
    CHECK(q.slot(0)->status == 0);
    CHECK(q.timeWaitedMs(0, 110000) == 0);
    CHECK(std::string(statusToken(q.slot(0)->status)) == "none");
}

TEST_CASE("Battlefield status: slots past the second are dropped (0x54AE40)", "[battlefield]") {
    Queues q;
    StatusPacket s;
    auto p = statusHeader(2, 0, 2, 10, 19, 0, false, 1).u32(0).u32(0).packet(0x2D4);
    REQUIRE(parseStatus(p, s));
    CHECK(q.apply(s, 1, noMap).slot == -1);
}

TEST_CASE("Battlefield status: the invitation's countdown in whole seconds", "[battlefield]") {
    Queues q;
    StatusPacket s;
    auto p = statusHeader(1, 0, 3, 20, 29, 4, false, 2).u32(529).u64(0).u32(80000).packet(0x2D4);
    REQUIRE(parseStatus(p, s));
    CHECK(s.mapId == 529);
    CHECK(s.inviteTimeoutMs == 80000);
    auto fx = q.apply(s, 1000, noMap);
    CHECK(fx.invited);
    CHECK(q.hasInvite());
    CHECK(q.portExpirationSec(1, 1000) == 80);
    CHECK(q.portExpirationSec(1, 1999) == 79);     // (ms left) / 1000, truncated
    CHECK(q.portExpirationSec(1, 90000) == 0);
    // Echoed whole by CMSG_BATTLEFIELD_PORT.
    CHECK(q.slot(1)->header == (uint64_t(0x1F90) << 48 | uint64_t(3) << 16));
}

TEST_CASE("Battlefield status: in progress carries the two timers and the arena faction", "[battlefield]") {
    Queues q;
    StatusPacket s;
    // Map 559 (Nagrand Arena), shutdown in 120 s, started 30 s ago, gold team.
    auto p = statusHeader(0, 2, 4, 80, 80, 0, true, 3).u32(559).u64(0).u32(120000).u32(30000).u8(1)
                 .packet(0x2D4);
    REQUIRE(parseStatus(p, s));
    CHECK(s.teamSize == 2);
    CHECK(s.autoLeaveMs == 120000);
    CHECK(s.elapsedMs == 30000);
    CHECK(s.arenaFaction);
    auto fx = q.apply(s, 500000, [](uint32_t m) { return m == 559 ? 4u : 0u; });
    CHECK(fx.enteredActive);
    CHECK(q.active().slot == 0);
    CHECK(q.active().instanceType == 4);
    CHECK(q.active().arenaFaction);
    CHECK(q.instanceExpirationMs(510000) == 110000);
    CHECK(q.instanceRunTimeMs(510000) == 40000);
    // Leaving: STATUS_NONE for that slot ends the active battleground.
    auto none = Bytes().u32(0).u64(0).packet(0x2D4);
    REQUIRE(parseStatus(none, s));
    fx = q.apply(s, 520000, noMap);
    CHECK(fx.leftActive);
    CHECK(q.active().slot == -1);
    CHECK(q.instanceRunTimeMs(520000) == 0);
}

TEST_CASE("Battlefield list: guid first, rewards, and the event it raises (0x54E390)", "[battlefield]") {
    // BattlegroundMgr::BuildBattlegroundListPacket for Warsong Gulch.
    auto p = Bytes().u64(0xF1300000AB000001ull).u8(0).u32(2).u8(0).u8(0)
                 .u8(1).u32(1500).u32(0).u32(500).u8(0)
                 .u32(2).u32(7).u32(9).packet(0x23D);
    ListPacket l;
    REQUIRE(parseList(p, l));
    CHECK(l.battlemaster == 0xF1300000AB000001ull);
    CHECK(l.bgTypeId == 2);
    CHECK(l.hasWin);
    CHECK(l.winHonor == 1500);
    CHECK(l.lossHonor == 500);
    CHECK(l.instanceIds == std::vector<uint32_t>{7, 9});
    CHECK(std::string(listEvent(l)) == "BATTLEFIELDS_SHOW");

    // The random battleground carries a second block.
    auto r = Bytes().u64(5).u8(0).u32(32).u8(0).u8(0)
                 .u8(0).u32(1).u32(2).u32(3).u8(1)
                 .u8(1).u32(4).u32(5).u32(6).u32(0).packet(0x23D);
    REQUIRE(parseList(r, l));
    CHECK(l.isRandom);
    CHECK(l.winHonorRandom == 4);
    CHECK(l.lossHonorRandom == 6);
    CHECK(std::string(listEvent(l)) == "NPC_PVPQUEUE_ANYWHERE");

    l.bgTypeId = 2; l.fromAnywhere = true;
    CHECK(std::string(listEvent(l)) == "PVPQUEUE_ANYWHERE_SHOW");
    l.fromAnywhere = false; l.battlemaster = 0;
    CHECK(std::string(listEvent(l)) == "BATTLEFIELDS_CLOSED");
}

TEST_CASE("PvP log: arena teams are both rating blocks, then both names", "[battlefield]") {
    // Battleground::BuildPvPLogDataPacket for an arena: rating blocks x2,
    // names x2, ended + winner, then ArenaScore rows (team byte).
    auto p = Bytes().u8(1)
                 .u32(0).u32(16).u32(1500)
                 .u32(16).u32(0).u32(1480)
                 .str("Gold").str("Green")
                 .u8(1).u8(1)
                 .u32(1)
                 .u64(42).u32(3).u8(1).u32(9000).u32(400).u32(0)
                 .packet(0x2E0);
    PvpLogPacket log;
    REQUIRE(parsePvpLog(p, log));
    CHECK(log.isArena);
    CHECK(log.teams[0].rating2 == 16);
    CHECK(log.teams[1].mmr == 1480);
    CHECK(log.teams[0].name == "Gold");
    CHECK(log.teams[1].name == "Green");
    CHECK(log.ended);
    CHECK(log.winner == 1);
    REQUIRE(log.rows.size() == 1);
    CHECK(log.rows[0].faction == 1);
    CHECK(log.rows[0].damageDone == 9000);
}

TEST_CASE("PvP log: battleground rows and their objective values", "[battlefield]") {
    auto p = Bytes().u8(0).u8(0).u32(1)
                 .u64(7).u32(2).u32(5).u32(1).u32(120).u32(3000).u32(800).u32(2).u32(3).u32(1)
                 .packet(0x2E0);
    PvpLogPacket log;
    REQUIRE(parsePvpLog(p, log));
    REQUIRE(log.rows.size() == 1);
    const auto& r = log.rows[0];
    CHECK(r.killingBlows == 2);
    CHECK(r.honorableKills == 5);
    CHECK(r.deaths == 1);
    CHECK(r.honorGained == 120);
    CHECK(r.stats[0] == 3);
    CHECK(r.stats[1] == 1);
    CHECK(r.faction == -1);   // from the race, once the name is known
    CHECK(factionOfRace(2) == 0);
    CHECK(factionOfRace(11) == 1);
}

TEST_CASE("Scoreboard sort: last key first, its own direction, filtered faction first", "[battlefield]") {
    std::vector<ScoreRow> rows(3);
    rows[0].name = "Bob";  rows[0].killingBlows = 1; rows[0].deaths = 9; rows[0].faction = 0;
    rows[1].name = "Ann";  rows[1].killingBlows = 5; rows[1].deaths = 2; rows[1].faction = 1;
    rows[2].name = "Cid";  rows[2].killingBlows = 3; rows[2].deaths = 4; rows[2].faction = 0;
    Scoreboard sb;
    sb.log.rows = rows;
    sb.hasData = true;
    sb.resort(0);
    // The initial order is kills first, highest first.
    CHECK(sb.log.rows[0].name == "Ann");
    CHECK(sb.log.rows[2].name == "Bob");
    CHECK(sb.shown == 3);

    sb.sort.select("name");          // ascending
    sb.resort(0);
    CHECK(sb.log.rows[0].name == "Ann");
    CHECK(sb.log.rows[1].name == "Bob");
    sb.sort.select("name");          // the same key again reverses
    sb.resort(0);
    CHECK(sb.log.rows[0].name == "Cid");

    sb.sort.select("deaths");        // a new key keeps its own (unreversed) direction
    sb.resort(0);
    CHECK(sb.log.rows[0].name == "Bob");

    sb.factionFilter = 1;            // Alliance tab: its rows first, counted
    sb.resort(0);
    CHECK(sb.shown == 1);
    CHECK(sb.log.rows[0].name == "Ann");

    CHECK(ScoreSort::keyOf("stat1") == 9);
    CHECK(ScoreSort::keyOf("hk") == 5);
    CHECK(ScoreSort::keyOf("bogus") == -1);
}

TEST_CASE("WorldStateUI: which rows are lines, which are columns (0x5487D0)", "[battlefield]") {
    std::vector<WorldStateUiRow> rows(5);
    rows[0].mapId = 489; rows[0].type = 2;             // WSG column
    rows[1].mapId = 489; rows[1].type = 2;             // WSG column
    rows[2].mapId = 489; rows[2].type = 0;             // WSG score line
    rows[3].mapId = 0; rows[3].areaId = 1519; rows[3].type = 0;   // a Stormwind line
    rows[4].mapId = -1; rows[4].type = 3;               // every map
    CHECK(visibleRows(rows, 489, 3277, 3277, 1) == std::vector<size_t>{2, 4});
    CHECK(visibleRows(rows, 0, 1519, 5150, 1) == std::vector<size_t>{3, 4});
    CHECK(visibleRows(rows, 0, 12, 12, 1) == std::vector<size_t>{4});
    CHECK(scoreColumns(rows, 489) == std::vector<size_t>{0, 1});
    CHECK(scoreColumns(rows, 529).empty());
}

TEST_CASE("WorldStateUI text: %Nw values and %Nk timers (0x5770D0)", "[battlefield]") {
    auto ws = [](uint32_t id) -> uint32_t {
        if (id == 1581) return 2;
        if (id == 1601) return 3;
        if (id == 4248) return 1000 + 125;      // a server timestamp
        if (id == 9) return 1000 + 3 * 3600 + 61;
        return 0;
    };
    CHECK(formatText("%1581w/%1601w", ws, 1000) == "2/3");
    CHECK(formatText("Time Remaining: %4248k", ws, 1000) == "Time Remaining: 02:05");
    CHECK(formatText("%9k", ws, 1000) == "3:01:01");
    CHECK(formatText("%4248k", ws, 5000) == "00:00");
    CHECK(formatText("50%x", ws, 0) == "50%x");
    CHECK(formatText("plain", ws, 0) == "plain");
}

TEST_CASE("Encounter units: packed, by priority then engage order (0x5ED750)", "[battlefield]") {
    EncounterUnits u;
    CHECK(u.engage(10, 1));
    CHECK(u.engage(20, 0));
    CHECK(u.engage(30, 1));
    CHECK_FALSE(u.engage(20, 2));           // already there: nothing changes
    CHECK(u.guidAt(0) == 20);               // priority 0 first
    CHECK(u.guidAt(1) == 10);
    CHECK(u.guidAt(2) == 30);
    CHECK(u.disengage(10));                 // the rest close up
    CHECK(u.guidAt(0) == 20);
    CHECK(u.guidAt(1) == 30);
    CHECK(u.guidAt(2) == 0);
    CHECK_FALSE(u.updatePriority(99, 0));   // unknown unit: ignored
    CHECK(u.updatePriority(30, 0));
    CHECK(u.guidAt(0) == 20);               // same priority: engage order
    for (uint64_t g = 100; g < 120; ++g) u.engage(g, 3);
    CHECK(u.size() == EncounterUnits::kMax);
}
