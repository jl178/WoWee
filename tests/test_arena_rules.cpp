// Stock 3.3.5a arena client rules (docs/re/pvp-instances.md §3): the
// opponents behind arena1-5 / arenapet1-5 and ARENA_OPPONENT_UPDATE (Wow.exe
// 0x54D100, 0x54D1C0, 0x54ABF0, 0x54AD10, 0x54CA50, 0x549630), the arena team
// records (0x635480, 0x5A3860, 0x5E7B00) and GetZonePVPInfo (0x51BA50).
// Packets are built in AzerothCore's order (ArenaTeam.cpp).
#include <catch_amalgamated.hpp>

#include "game/arena_rules.hpp"
#include "game/battlefield_rules.hpp"
#include "network/packet.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

using namespace wowee::game;
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

using Ev = arena::OpponentEvent;

struct World {
    std::set<uint64_t> inView;
    std::map<uint64_t, uint64_t> petOf;
    arena::Opponents::Present present() const {
        return [this](uint64_t g) { return inView.count(g) > 0; };
    }
    arena::Opponents::PetOf pets() const {
        return [this](uint64_t g) { auto it = petOf.find(g); return it != petOf.end() ? it->second : 0; };
    }
};

}  // namespace

TEST_CASE("Opponents take slots in the order they are seen", "[arena]") {
    arena::Opponents o;
    World w;
    w.inView = {0xA, 0xB};
    CHECK(o.count() == 0);
    CHECK(o.playerSeen(0xA, w.present()) == std::vector<Ev>{{"arena1", "seen"}});
    CHECK(o.playerSeen(0xB, w.present()) == std::vector<Ev>{{"arena2", "seen"}});
    // Seen again: the same slot, "seen" again.
    CHECK(o.playerSeen(0xA, w.present()) == std::vector<Ev>{{"arena1", "seen"}});
    CHECK(o.count() == 2);
    CHECK(o.player(0) == 0xA);
    CHECK(o.player(1) == 0xB);
    // Known from stats alone, with no object: "seen" then "unseen".
    CHECK(o.playerSeen(0xC, w.present()) ==
          std::vector<Ev>{{"arena3", "seen"}, {"arena3", "unseen"}});
}

TEST_CASE("Out of range is unseen, the arena's destroy is destroyed, slots stay", "[arena]") {
    arena::Opponents o;
    World w;
    w.inView = {0xA, 0xB};
    o.playerSeen(0xA, w.present());
    o.playerSeen(0xB, w.present());
    CHECK(o.unseen(0xA, w.present(), w.pets()) == std::vector<Ev>{{"arena1", "unseen"}});
    CHECK(o.destroyed(0xB, w.present(), w.pets()) == std::vector<Ev>{{"arena2", "destroyed"}});
    CHECK(o.player(1) == 0xB);
    CHECK(o.count() == 2);
    // Not an opponent: nothing.
    CHECK(o.unseen(0x99, w.present(), w.pets()).empty());
}

TEST_CASE("A pet joins its owner's slot and is found through the owner while in view", "[arena]") {
    arena::Opponents o;
    World w;
    w.inView = {0xA, 0xB, 0x1A};
    o.playerSeen(0xA, w.present());
    o.playerSeen(0xB, w.present());
    // 0x1B belongs to the second opponent; its object is not here.
    CHECK(o.petSeen(0x1B, 0xB, w.present()) ==
          std::vector<Ev>{{"arenapet2", "seen"}, {"arenapet2", "unseen"}});
    // arenapetN is the visible owner's CHARM/SUMMON before the table.
    w.petOf[0xB] = 0x2B;
    CHECK(o.pet(1, w.present(), w.pets()) == 0x2B);
    w.inView.erase(0xB);
    CHECK(o.pet(1, w.present(), w.pets()) == 0x1B);
    // A pet whose owner was never seen takes the first empty slot.
    CHECK(o.petSeen(0x1C, 0xC, w.present()) == std::vector<Ev>{{"arenapet3", "seen"}, {"arenapet3", "unseen"}});
    CHECK(o.count() == 3);
    // ...and when the owner turns up, the owner takes that slot.
    w.inView.insert(0xC);
    CHECK(o.playerSeen(0xC, w.present()) == std::vector<Ev>{{"arena3", "seen"}});
}

TEST_CASE("A destroyed pet leaves the pet table; one seen through its owner does not", "[arena]") {
    arena::Opponents o;
    World w;
    w.inView = {0xA};
    o.playerSeen(0xA, w.present());
    o.petSeen(0x1A, 0xA, w.present());
    w.petOf[0xA] = 0x1A;
    // Through the visible owner: destroyed, the slot kept.
    CHECK(o.destroyed(0x1A, w.present(), w.pets()) == std::vector<Ev>{{"arenapet1", "destroyed"}});
    CHECK(o.pet(0, World{}.present(), World{}.pets()) == 0x1A);
    // The owner gone from view: found in the table and dropped from it.
    w.inView.clear();
    CHECK(o.destroyed(0x1A, w.present(), w.pets()) == std::vector<Ev>{{"arenapet1", "destroyed"}});
    CHECK(o.pet(0, w.present(), w.pets()) == 0);
}

TEST_CASE("Entering the world clears all ten, interleaved", "[arena]") {
    arena::Opponents o;
    World w;
    o.playerSeen(0xA, w.present());
    const auto ev = o.clear();
    REQUIRE(ev.size() == 10);
    CHECK(ev[0] == Ev{"arena1", "cleared"});
    CHECK(ev[1] == Ev{"arenapet1", "cleared"});
    CHECK(ev[9] == Ev{"arenapet5", "cleared"});
    CHECK(o.count() == 0);
    CHECK(o.player(0) == 0);
}

TEST_CASE("Five opponents at most", "[arena]") {
    arena::Opponents o;
    World w;
    for (uint64_t g = 1; g <= 5; ++g) o.playerSeen(g, w.present());
    CHECK(o.count() == 5);
    CHECK(o.playerSeen(6, w.present()).empty());
}

TEST_CASE("SMSG_ARENA_TEAM_QUERY_RESPONSE as ArenaTeam::Query writes it", "[arena]") {
    Bytes b;
    b.u32(7).str("Murloc Lords").u32(3).u32(0xFF102030).u32(42).u32(0xFF405060).u32(5).u32(0xFF708090);
    Packet p = b.packet(0x34C);
    arena::TeamQuery q;
    REQUIRE(arena::parseTeamQuery(p, q));
    CHECK(q.teamId == 7);
    CHECK(q.name == "Murloc Lords");
    CHECK(q.type == 3);
    CHECK(q.emblemStyle == 42);
    CHECK(q.borderStyle == 5);
    const auto c = arena::colorComponents(q.backgroundColor);
    CHECK(c[0] == Catch::Approx(0x10 / 255.0f));
    CHECK(c[1] == Catch::Approx(0x20 / 255.0f));
    CHECK(c[2] == Catch::Approx(0x30 / 255.0f));
}

TEST_CASE("MSG_INSPECT_ARENA_TEAMS as ArenaTeam::Inspect writes it", "[arena]") {
    Bytes b;
    b.u64(0x1234).u8(1).u32(9).u32(1650).u32(40).u32(25).u32(30).u32(1700);
    Packet p = b.packet(0x377);
    uint64_t guid = 0;
    arena::InspectTeam t;
    REQUIRE(arena::parseInspectTeam(p, guid, t));
    CHECK(guid == 0x1234);
    CHECK(t.slot == 1);
    CHECK(t.teamId == 9);
    CHECK(t.rating == 1650);
    CHECK(t.seasonGames == 40);
    CHECK(t.seasonWins == 25);
    CHECK(t.playerSeasonGames == 30);
    CHECK(t.personalRating == 1700);
}

TEST_CASE("PLAYER_FIELD_ARENA_TEAM_INFO's seven fields", "[arena]") {
    const auto t = arena::teamInfoFromFields({11, 2, 1, 4, 30, 18, 1550});
    CHECK(t.teamId == 11);
    CHECK(t.type == 2);
    CHECK(t.member == 1);
    CHECK(t.gamesWeek == 4);
    CHECK(t.gamesSeason == 30);
    CHECK(t.winsSeason == 18);
    CHECK(t.personalRating == 1550);
}

TEST_CASE("GetZonePVPInfo follows the area, the realm and the zone's mask", "[arena][zonepvp]") {
    using bf::AreaPvp;
    auto names = [](uint32_t mask) -> std::string {
        if (mask & 2) return "Alliance";
        if (mask & 4) return "Horde";
        return {};
    };
    const AreaPvp elwynn{0x40, 2};
    const AreaPvp goldshire{0x40300040, 0};
    const AreaPvp stormwind{0x200138, 2};
    const AreaPvp dalaran{0x20004d28, 6};
    const AreaPvp wintergrasp{0x9004441, 0};
    const AreaPvp amaniPass{0x40040, 0};
    // A realm without player killing: nothing outside the capitals.
    auto z = bf::zonePvpInfo(&goldshire, &elwynn, false, true, 2, 12, names);
    CHECK(z.count == 0);
    z = bf::zonePvpInfo(&stormwind, &stormwind, false, true, 2, 12, names);
    CHECK(z.count == 3);
    CHECK(z.type == "friendly");
    CHECK(z.faction == "Alliance");
    CHECK_FALSE(z.fromArea);
    z = bf::zonePvpInfo(&stormwind, &stormwind, false, true, 4, 10, names);
    CHECK(z.type == "hostile");
    // On a PvP realm the zone's mask answers everywhere.
    z = bf::zonePvpInfo(&goldshire, &elwynn, true, true, 2, 12, names);
    CHECK(z.type == "friendly");
    CHECK(z.faction == "Alliance");
    // The area's flags: sanctuary, combat; with the area row's 1.
    z = bf::zonePvpInfo(&dalaran, &dalaran, false, true, 2, 12, names);
    CHECK(z.count == 2);
    CHECK(z.type == "sanctuary");
    CHECK(z.fromArea);
    z = bf::zonePvpInfo(&wintergrasp, &wintergrasp, false, true, 2, 12, names);
    CHECK(z.type == "combat");
    z = bf::zonePvpInfo(nullptr, &dalaran, false, true, 2, 12, names);
    CHECK(z.type == "sanctuary");
    CHECK_FALSE(z.fromArea);
    // 0x40000 on the area is contested whatever the mask.
    z = bf::zonePvpInfo(&amaniPass, &elwynn, true, true, 2, 12, names);
    CHECK(z.type == "contested");
}
