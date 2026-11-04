// TargetNearestEnemy and its siblings as Wow.exe 12340 picks (0x524FC0,
// 0x524440, 0x518E40, 0x5131D0): which units are candidates, in what order,
// and how a press steps through them. docs/re/combat-targeting-movement.md.
#include <catch_amalgamated.hpp>

#include "game/tab_target.hpp"

#include <map>

using namespace wowee::game;
namespace tt = wowee::game::tab_target;

namespace {

tt::UnitFacts livingEnemy() {
    tt::UnitFacts f;
    f.canAttack = true;
    f.health = 100;
    return f;
}

// A press against a fixed world: the units, their distances and cone, and
// which of them still pass when stepped onto.
struct World {
    std::vector<tt::Candidate> units;
    std::map<uint64_t, bool> gone;
    tt::Cycle cycle;
    uint64_t target = 0;

    uint64_t press(uint32_t now, bool reverse = false,
                   TargetNearestMode mode = TargetNearestMode::Enemy) {
        const uint64_t pick = cycle.press(
            mode, reverse, now, target, [&] { return units; },
            [&](uint64_t g) { return !gone[g]; });
        if (pick) target = pick;
        return pick;
    }
};

} // namespace

TEST_CASE("the cone is 30 degrees either side of the facing", "[tab-target]") {
    CHECK(tt::inFrontCone(0.0f, 0.0f));
    CHECK(tt::inFrontCone(0.0f, 0.52f));
    CHECK(tt::inFrontCone(0.0f, -0.52f));
    CHECK_FALSE(tt::inFrontCone(0.0f, 0.53f));
    CHECK_FALSE(tt::inFrontCone(0.0f, 3.14f));
    // Across the wrap: facing just under 2pi, the unit just over 0.
    CHECK(tt::inFrontCone(6.2f, 0.1f));
    CHECK(tt::inFrontCone(-3.0f, -3.3f));
}

TEST_CASE("41 yards in the cone, 10 anywhere else", "[tab-target]") {
    CHECK(tt::withinReach(41.0f * 41.0f, true));
    CHECK_FALSE(tt::withinReach(41.1f * 41.1f, true));
    CHECK(tt::withinReach(10.0f * 10.0f, false));
    CHECK_FALSE(tt::withinReach(10.1f * 10.1f, false));
}

TEST_CASE("critters, companions and gas clouds are never candidates", "[tab-target]") {
    CHECK(tt::creatureTypeIgnoredByTab(8));
    CHECK(tt::creatureTypeIgnoredByTab(12));
    CHECK(tt::creatureTypeIgnoredByTab(13));
    CHECK_FALSE(tt::creatureTypeIgnoredByTab(11));  // totems are
    CHECK_FALSE(tt::creatureTypeIgnoredByTab(7));
    CHECK_FALSE(tt::creatureTypeIgnoredByTab(0));
}

TEST_CASE("the enemy modes want something attackable and alive", "[tab-target]") {
    CHECK(tt::passesMode(TargetNearestMode::Enemy, livingEnemy()));
    auto f = livingEnemy();
    f.canAttack = false;
    CHECK_FALSE(tt::passesMode(TargetNearestMode::Enemy, f));
    f = livingEnemy();
    f.dead = true;
    CHECK_FALSE(tt::passesMode(TargetNearestMode::Enemy, f));
    f = livingEnemy();
    f.dynFlagDead = true;  // feigning death
    CHECK_FALSE(tt::passesMode(TargetNearestMode::Enemy, f));
    f = livingEnemy();
    f.standState = 7;
    CHECK_FALSE(tt::passesMode(TargetNearestMode::Enemy, f));
    CHECK_FALSE(tt::passesMode(TargetNearestMode::EnemyPlayer, livingEnemy()));
    f = livingEnemy();
    f.isPlayer = true;
    CHECK(tt::passesMode(TargetNearestMode::EnemyPlayer, f));
}

TEST_CASE("the friend modes want something assistable with health", "[tab-target]") {
    tt::UnitFacts f;
    f.canAssist = true;
    f.health = 1;
    CHECK(tt::passesMode(TargetNearestMode::Friend, f));
    CHECK_FALSE(tt::passesMode(TargetNearestMode::FriendPlayer, f));
    f.isPlayer = true;
    CHECK(tt::passesMode(TargetNearestMode::FriendPlayer, f));
    f.health = 0;
    CHECK_FALSE(tt::passesMode(TargetNearestMode::Friend, f));
}

TEST_CASE("party and raid modes ask only membership, dead or alive", "[tab-target]") {
    tt::UnitFacts f;
    CHECK_FALSE(tt::passesMode(TargetNearestMode::Party, f));
    f.raidMember = true;
    CHECK_FALSE(tt::passesMode(TargetNearestMode::Party, f));
    CHECK(tt::passesMode(TargetNearestMode::Raid, f));
    f.partyMember = true;
    CHECK(tt::passesMode(TargetNearestMode::Party, f));
    CHECK(tt::passesMode(TargetNearestMode::Any, tt::UnitFacts{}));
}

TEST_CASE("PvP between players by the flags", "[tab-target]") {
    CHECK(tt::playersMayFight(0, tt::kPvpFlagPvp));
    CHECK_FALSE(tt::playersMayFight(0, 0));
    CHECK_FALSE(tt::playersMayFight(tt::kPvpFlagPvp, 0));  // only theirs counts
    CHECK(tt::playersMayFight(tt::kPvpFlagFfa, tt::kPvpFlagFfa));
    CHECK_FALSE(tt::playersMayFight(tt::kPvpFlagSanctuary, tt::kPvpFlagPvp));
    CHECK_FALSE(tt::playersMayFight(0, tt::kPvpFlagPvp | tt::kPvpFlagSanctuary));
}

TEST_CASE("the cone sorts first, then distance", "[tab-target]") {
    std::vector<tt::Candidate> list = {
        {.guid = 1, .distSq = 4.0f, .inFront = false},
        {.guid = 2, .distSq = 900.0f, .inFront = true},
        {.guid = 3, .distSq = 25.0f, .inFront = true},
        {.guid = 4, .distSq = 1.0f, .inFront = false},
    };
    tt::sortCandidates(list);
    CHECK(list[0].guid == 3);
    CHECK(list[1].guid == 2);
    CHECK(list[2].guid == 4);
    CHECK(list[3].guid == 1);
}

TEST_CASE("presses step through the list and wrap", "[tab-target]") {
    World w;
    w.units = {{.guid = 10, .distSq = 9, .inFront = true},
               {.guid = 20, .distSq = 16, .inFront = true},
               {.guid = 30, .distSq = 36, .inFront = false}};
    CHECK(w.press(1000) == 10);
    CHECK(w.press(1100) == 20);
    CHECK(w.press(1200) == 30);
    // Off the end of a list under a second old: back to the top, same list.
    CHECK(w.press(1300) == 10);
    CHECK(w.cycle.list().size() == 3);
}

TEST_CASE("reverse steps back, and a fresh list starts at the nearest", "[tab-target]") {
    World w;
    w.units = {{.guid = 10, .distSq = 9, .inFront = true},
               {.guid = 20, .distSq = 16, .inFront = true},
               {.guid = 30, .distSq = 36, .inFront = true}};
    // A fresh list selects its first entry, reverse or not.
    CHECK(w.press(1000, true) == 10);
    CHECK(w.press(1100, true) == 30);
    CHECK(w.press(1200, true) == 20);
}

TEST_CASE("three seconds after the last press the list is built again", "[tab-target]") {
    World w;
    w.units = {{.guid = 10, .distSq = 9, .inFront = true},
               {.guid = 20, .distSq = 16, .inFront = true}};
    CHECK(w.press(1000) == 10);
    CHECK(w.press(2000) == 20);
    // A newcomer nearer than both, and a pause: the nearest again, even
    // though it is not the next one.
    w.units.insert(w.units.begin(), {.guid = 5, .distSq = 1, .inFront = true});
    CHECK(w.press(5000) == 5);
    // Not yet expired: the list is kept, the newcomer would not appear.
    w.units.push_back({.guid = 1, .distSq = 0.5f, .inFront = true});
    CHECK(w.press(5500) == 10);
}

TEST_CASE("a fresh list may select the current target again", "[tab-target]") {
    World w;
    w.units = {{.guid = 10, .distSq = 9, .inFront = true},
               {.guid = 20, .distSq = 16, .inFront = true}};
    w.target = 10;
    // No list yet: built, and its first entry is the target already held.
    CHECK(w.press(1000) == 10);
    CHECK(w.press(1200) == 20);
}

TEST_CASE("wrapping a list a second old builds it again", "[tab-target]") {
    World w;
    w.units = {{.guid = 10, .distSq = 9, .inFront = true},
               {.guid = 20, .distSq = 16, .inFront = true}};
    CHECK(w.press(1000) == 10);
    CHECK(w.press(1900) == 20);
    w.units.insert(w.units.begin(), {.guid = 5, .distSq = 1, .inFront = true});
    CHECK(w.press(2100) == 5);  // wrapped at 1.1 s: rebuilt, newcomer first
}

TEST_CASE("a target picked by other means keeps the cursor where it was", "[tab-target]") {
    World w;
    w.units = {{.guid = 10, .distSq = 9, .inFront = true},
               {.guid = 20, .distSq = 16, .inFront = true},
               {.guid = 30, .distSq = 36, .inFront = true}};
    CHECK(w.press(1000) == 10);
    w.target = 30;  // clicked
    // The list's current entry is not the target: it is selected again.
    CHECK(w.press(1100) == 10);
}

TEST_CASE("an entry that died is stepped over; none left changes nothing", "[tab-target]") {
    World w;
    w.units = {{.guid = 10, .distSq = 9, .inFront = true},
               {.guid = 20, .distSq = 16, .inFront = true},
               {.guid = 30, .distSq = 36, .inFront = true}};
    CHECK(w.press(1000) == 10);
    w.gone[20] = true;
    CHECK(w.press(1100) == 30);
    w.gone[10] = true;
    w.gone[30] = true;
    CHECK(w.press(1200) == 0);
    CHECK(w.target == 30);
    CHECK(w.cycle.list().empty());
}

TEST_CASE("nobody to pick leaves the target alone", "[tab-target]") {
    World w;
    w.target = 77;
    CHECK(w.press(1000) == 0);
    CHECK(w.target == 77);
}

TEST_CASE("another mode builds its own list", "[tab-target]") {
    World w;
    w.units = {{.guid = 10, .distSq = 9, .inFront = true},
               {.guid = 20, .distSq = 16, .inFront = true}};
    CHECK(w.press(1000) == 10);
    CHECK(w.press(1100, false, TargetNearestMode::Friend) == 10);
}
