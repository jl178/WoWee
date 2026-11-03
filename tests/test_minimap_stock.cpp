// The stock 3.3.5a minimap rules in ui/minimap_stock.hpp: placement and its
// inverse, the 0.8 cut-off, the rim arrows, which cell a blip is, and how the
// three POI arrows are picked. Numbers are the client's (docs/re/maps.md).
#include <catch_amalgamated.hpp>

#include "ui/minimap_stock.hpp"
#include "rendering/wmo_minimap.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace wowee::ui::minimap_stock;
using Catch::Approx;

namespace {
constexpr float kPi = 3.14159265358979f;
}

TEST_CASE("a point due north is straight up, due west to the left", "[minimap_stock]") {
    const float r = 200.0f;  // zoom level 1, 400 yards across
    glm::vec2 n = mapOffset(100.0f, 0.0f, r, 0.0f);
    CHECK(n.x == Approx(0.0f).margin(1e-6));
    CHECK(n.y == Approx(0.25f));  // 100 of 400 yards, a quarter of the width
    glm::vec2 w = mapOffset(0.0f, 100.0f, r, 0.0f);
    CHECK(w.x == Approx(-0.25f));
    CHECK(w.y == Approx(0.0f).margin(1e-6));
}

TEST_CASE("rotating the map puts the facing up", "[minimap_stock]") {
    // Facing west (a quarter turn counter-clockwise from north): a point to the
    // west is ahead, so it is up; a point to the north is to the right.
    const float r = 100.0f;
    glm::vec2 w = mapOffset(0.0f, 50.0f, r, 0.5f * kPi);
    CHECK(w.x == Approx(0.0f).margin(1e-6));
    CHECK(w.y == Approx(0.25f));
    glm::vec2 n = mapOffset(50.0f, 0.0f, r, 0.5f * kPi);
    CHECK(n.x == Approx(0.25f));
    CHECK(n.y == Approx(0.0f).margin(1e-6));
}

TEST_CASE("PingLocation inverts the placement", "[minimap_stock]") {
    for (float bearing : {0.0f, 0.7f, 2.0f, -1.3f}) {
        const glm::vec2 f = mapOffset(37.0f, -81.0f, 166.0f, bearing);
        const glm::vec2 back = worldOffset(f.x, f.y, 166.0f, bearing);
        CHECK(back.x == Approx(37.0f).margin(1e-3));
        CHECK(back.y == Approx(-81.0f).margin(1e-3));
    }
}

TEST_CASE("blips stop at 0.8 of the radius", "[minimap_stock]") {
    CHECK(onMap(80.0f, 0.0f, 100.0f));
    CHECK_FALSE(onMap(80.1f, 0.0f, 100.0f));
    CHECK(onMap(48.0f, 64.0f, 100.0f));   // 80 yards on the diagonal
    CHECK_FALSE(onMap(49.0f, 64.0f, 100.0f));
}

TEST_CASE("rim arrows sit 56.32 units out, on the direction", "[minimap_stock]") {
    CHECK(rimAngle(10.0f, 0.0f) == Approx(0.0f));
    CHECK(rimAngle(0.0f, 10.0f) == Approx(0.5f * kPi));
    CHECK(rimAngle(-10.0f, 0.0f) == Approx(kPi));
    CHECK(rimAngle(0.0f, -10.0f) == Approx(1.5f * kPi));
    const glm::vec2 north = rimArrowOffset(0.0f, 0.0f);
    CHECK(north.x == Approx(0.0f).margin(1e-5));
    CHECK(north.y == Approx(56.32f));
    const glm::vec2 west = rimArrowOffset(0.5f * kPi, 0.0f);
    CHECK(west.x == Approx(-56.32f));
    // Facing west, something to the west is ahead.
    const glm::vec2 ahead = rimArrowOffset(0.5f * kPi, 0.5f * kPi);
    CHECK(ahead.x == Approx(0.0f).margin(1e-4));
    CHECK(ahead.y == Approx(56.32f));
    CHECK(kRimArrowArt == Approx(kRimArrowQuad / std::sqrt(2.0f)));
}

TEST_CASE("quest givers: only the five statuses the client draws", "[minimap_stock]") {
    CHECK(questStatusCell(10, false) == kQuestReward);
    CHECK(questStatusCell(8, false) == kQuestAvailable);
    CHECK(questStatusCell(7, false) == kRepeatableAvailable);
    CHECK(questStatusCell(2, false) == kNoBlip);
    CHECK(questStatusCell(2, true) == kQuestAvailable);
    CHECK(questStatusCell(4, true) == kRepeatableAvailable);
    CHECK(questStatusCell(5, true) == kNoBlip);
    CHECK(questStatusCell(6, false) == kNoBlip);
    CHECK(questStatusCell(9, false) == kNoBlip);
}

TEST_CASE("an NPC needs tracking unless it is a quest giver or a new flight master",
          "[minimap_stock]") {
    UnitFacts npc;
    CHECK(classifyUnit(npc, false) == kNoBlip);
    npc.questStatus = 8;
    CHECK(classifyUnit(npc, false) == kQuestAvailable);
    npc.questStatus = 0;
    npc.taxiUndiscovered = true;
    CHECK(classifyUnit(npc, false) == kTaxiUndiscovered);
    npc.taxiUndiscovered = false;
    npc.creatureTracked = true;
    npc.reaction = 2;
    CHECK(classifyUnit(npc, false) == kHostileNpc);
    npc.reaction = 4;
    CHECK(classifyUnit(npc, false) == kNeutralNpc);
    npc.reaction = 5;
    CHECK(classifyUnit(npc, false) == kFriendlyNpc);
    npc.townTracked = true;
    CHECK(classifyUnit(npc, false) == kTracked);
    npc.ownPet = true;
    CHECK(classifyUnit(npc, false) == kOwnPet);
    UnitFacts dead;
    dead.creatureTracked = true;
    dead.deadAndHidden = true;
    CHECK(classifyUnit(dead, false) == kNoBlip);
}

TEST_CASE("players: silver-rimmed by hostility and team, never the group", "[minimap_stock]") {
    UnitFacts p;
    p.isPlayer = true;
    p.creatureTracked = true;
    p.attackable = true;
    CHECK(classifyUnit(p, false) == kHostilePlayer);
    p.attackable = false;
    p.sameTeam = true;
    CHECK(classifyUnit(p, false) == kFriendlyPlayer);
    p.sameTeam = false;
    CHECK(classifyUnit(p, false) == kNeutralPlayer);
    p.groupMember = true;
    CHECK(classifyUnit(p, false) == kNoBlip);
    UnitFacts untracked;
    untracked.isPlayer = true;
    CHECK(classifyUnit(untracked, false) == kNoBlip);
}

TEST_CASE("game objects: mailbox, quest, tracked resource", "[minimap_stock]") {
    GameObjectFacts g;
    CHECK(classifyGameObject(g, false) == kNoBlip);
    g.resourceTracked = true;
    CHECK(classifyGameObject(g, false) == kTracked);
    g.questStatus = 10;
    CHECK(classifyGameObject(g, false) == kQuestReward);
    g.mailboxTracked = true;
    CHECK(classifyGameObject(g, false) == kTracked);
}

TEST_CASE("town tracking: fifteen entries, a few by class", "[minimap_stock]") {
    CHECK(kTownTracking.size() == 15);
    const auto& poisons = kTownTracking[2];
    CHECK(townTrackingOffered(poisons, 4));      // rogue
    CHECK_FALSE(townTrackingOffered(poisons, 1));
    const auto& ammo = kTownTracking[3];
    CHECK(townTrackingOffered(ammo, 1));          // warrior
    CHECK(townTrackingOffered(ammo, 3));          // hunter
    CHECK_FALSE(townTrackingOffered(ammo, 8));    // mage
    CHECK(townTrackingOffered(kTownTracking[7], 3));   // stable master, hunter
    CHECK_FALSE(townTrackingOffered(kTownTracking[7], 2));
    CHECK(townTrackingOffered(kTownTracking[0], 11));  // repair, anyone
}

TEST_CASE("POI arrows: three, lowest importance, then nearest", "[minimap_stock]") {
    std::vector<PoiArrowCandidate> c = {
        {3, 300.0f, false},  // 0 a town
        {3, 200.0f, false},  // 1 a nearer town
        {0, 650.0f, false},  // 2 a tower
        {3, 100.0f, false},  // 3 the nearest town
        {0, 800.0f, false},  // 4 out of range
        {-1, 5000.0f, true}, // 5 the corpse, any distance
    };
    int n = 0;
    auto picked = pickPoiArrows(c, n);
    REQUIRE(n == 3);
    std::vector<int> got(picked.begin(), picked.end());
    std::sort(got.begin(), got.end());
    CHECK(got == std::vector<int>{2, 3, 5});
}

TEST_CASE("the indoor map: an indoor group that is not exterior", "[minimap_stock]") {
    using wowee::rendering::wmo_minimap::groupIsIndoorForMinimap;
    CHECK(groupIsIndoorForMinimap(0x2000));          // Ironforge's halls
    CHECK_FALSE(groupIsIndoorForMinimap(0x2000 | 0x40));  // Stormwind's streets
    CHECK_FALSE(groupIsIndoorForMinimap(0x2000 | 0x8));
    CHECK_FALSE(groupIsIndoorForMinimap(0x8));
}
