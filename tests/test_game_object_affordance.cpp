// Which game objects the mouse may find and use, as the stock client decides.
//
// Hovering a dungeon entrance named its purple aura - "Instance Portal" - with
// the interact cursor, because the picker offered every object it met. The
// stock client asks each object's type handler first, and a GENERIC object is
// only found when its template sets "highlight" (data1). These are the cases
// that rule has to keep apart.
#include <catch_amalgamated.hpp>

#include <cstdint>

#include "ui/game_object_affordance.hpp"

using namespace wowee::ui;

namespace {
GameObjectAffordanceInput ofType(uint32_t type, const uint32_t* data) {
    GameObjectAffordanceInput in;
    in.type = type;
    in.typeKnown = true;
    in.data = data;
    return in;
}
}  // namespace

TEST_CASE("generic scenery is found only when it asks to be") {
    uint32_t portal[24] = {};  // 19527 "Instance Portal": data1 = 0
    auto a = gameObjectAffordance(ofType(5, portal));
    CHECK_FALSE(a.highlight);
    CHECK_FALSE(a.interact);

    uint32_t sign[24] = {};    // 89 "Northshire Abbey" signpost: data1 = 1
    sign[1] = 1;
    a = gameObjectAffordance(ofType(5, sign));
    CHECK(a.highlight);
    CHECK_FALSE(a.interact);   // named, under the plain pointer

    // No record yet: nothing to read the field from, so not found.
    CHECK_FALSE(gameObjectAffordance(ofType(5, nullptr)).highlight);
}

TEST_CASE("a usable object is found exactly when it can be used") {
    uint32_t data[24] = {};
    auto in = ofType(0, data);  // a door
    CHECK(gameObjectAffordance(in).interact);

    in.flags = kGoFlagNotSelectable;
    CHECK_FALSE(gameObjectAffordance(in).highlight);
    in.flags = kGoFlagInUse;
    CHECK_FALSE(gameObjectAffordance(in).highlight);

    // INTERACT_COND: closed until the server activates it for this player.
    in.flags = kGoFlagInteractCond;
    CHECK_FALSE(gameObjectAffordance(in).highlight);
    in.dynamicFlags = kGoDynActivate;
    CHECK(gameObjectAffordance(in).interact);

    in.flags = 0;
    in.dynamicFlags = kGoDynNoInteract;
    CHECK_FALSE(gameObjectAffordance(in).highlight);

    in.dynamicFlags = 0;
    in.hostile = true;  // a door that fights for the other side
    CHECK_FALSE(gameObjectAffordance(in).highlight);
}

TEST_CASE("types that are named but never used, and types never found") {
    uint32_t data[24] = {};
    for (uint32_t type : {8u, 16u, 25u, 30u}) {  // anvil, duel flag, fish pool, aura
        INFO(type);
        const auto a = gameObjectAffordance(ofType(type, data));
        CHECK(a.highlight);
        CHECK_FALSE(a.interact);
    }
    for (uint32_t type : {11u, 14u, 15u, 20u, 21u, 28u, 31u, 35u}) {
        INFO(type);
        CHECK_FALSE(gameObjectAffordance(ofType(type, data)).highlight);
    }
}

TEST_CASE("a quest object is there only while the quest is in the log") {
    uint32_t chest[24] = {};
    chest[8] = 1234;  // CHEST data8: questID
    auto in = ofType(3, chest);
    CHECK_FALSE(gameObjectAffordance(in).highlight);
    in.questInLog = true;
    CHECK(gameObjectAffordance(in).interact);

    uint32_t goober[24] = {};
    goober[1] = 99;   // GOOBER data1: questID
    in = ofType(10, goober);
    CHECK_FALSE(gameObjectAffordance(in).highlight);
    in.questInLog = true;
    CHECK(gameObjectAffordance(in).highlight);
}

TEST_CASE("bobbers, traps and private chairs belong to someone") {
    uint32_t data[24] = {};
    auto bobber = ofType(17, data);
    CHECK_FALSE(gameObjectAffordance(bobber).highlight);
    bobber.playersChannelObject = true;
    CHECK(gameObjectAffordance(bobber).interact);

    auto trap = ofType(6, data);
    trap.hostile = true;
    CHECK_FALSE(gameObjectAffordance(trap).highlight);  // nothing to disarm
    uint32_t locked[24] = {};
    locked[0] = 7;
    trap.data = locked;
    CHECK(gameObjectAffordance(trap).interact);
    trap.hostile = false;
    CHECK_FALSE(gameObjectAffordance(trap).highlight);  // a friend's trap

    uint32_t chairData[24] = {};
    chairData[2] = 1;  // onlyCreatorUse
    auto chair = ofType(7, chairData);
    CHECK_FALSE(gameObjectAffordance(chair).highlight);
    chair.createdByPlayer = true;
    CHECK(gameObjectAffordance(chair).interact);
}
