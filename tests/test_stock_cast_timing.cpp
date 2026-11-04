// The stock 3.3.5a timing rules for a press becoming a cast
// (game/stock_cast_timing.hpp): the predicted global cooldown, the
// once-per-burst error text, the "not ready" line and the world click.
// docs/re/input-timing.md has the addresses and the measured numbers.
#include <catch_amalgamated.hpp>
#include "game/stock_cast_timing.hpp"

#include <string>

namespace sc = wowee::game::stock_cast;

TEST_CASE("the global cooldown comes from the spell, not a fixed 1.5 s") {
    // Holy Light: category 133, 1500 ms, magic, no attributes that stop haste.
    CHECK(sc::predictedGcdMs(133, 1500, 1, 0x10000, 1.0f) == 1500);
    // Divine Protection, Avenging Wrath, Auto Shot: off the GCD.
    CHECK(sc::predictedGcdMs(0, 0, 1, 0x50000, 1.0f) == 0);
    CHECK(sc::predictedGcdMs(0, 0, 3, 0x50012, 1.0f) == 0);
    // A rogue's 1 s GCD is not scaled.
    CHECK(sc::predictedGcdMs(133, 1000, 2, 0x10, 0.5f) == 1000);
    // A category with no time starts nothing (Shoot: 133, 0).
    CHECK(sc::predictedGcdMs(133, 0, 1, 0x12, 1.0f) == 0);
}

TEST_CASE("haste shortens a spell's 1.5 s GCD to no less than 1 s") {
    // UNIT_MOD_CAST_SPEED 1/1.2: round(1250 - 0.5) = 1250.
    CHECK(sc::predictedGcdMs(133, 1500, 1, 0, 1.0f / 1.2f) == 1250);
    // Past the floor.
    CHECK(sc::predictedGcdMs(133, 1500, 1, 0, 0.5f) == 1000);
    // Slowed past 1.5 s is held at 1.5 s.
    CHECK(sc::predictedGcdMs(133, 1500, 1, 0, 1.3f) == 1500);
    // Melee and ranged spells, and abilities, keep 1.5 s.
    CHECK(sc::predictedGcdMs(133, 1500, 2, 0, 0.5f) == 1500);
    CHECK(sc::predictedGcdMs(133, 1500, 3, 0, 0.5f) == 1500);
    CHECK(sc::predictedGcdMs(133, 1500, 1, 0x10, 0.5f) == 1500);
    CHECK(sc::predictedGcdMs(133, 1500, 1, 0x2, 0.5f) == 1500);
    // A field that never arrived reads 0 and means unhasted.
    CHECK(sc::predictedGcdMs(133, 1500, 1, 0, 0.0f) == 1500);
}

TEST_CASE("a cast error repeats only after 3 s of quiet") {
    sc::CastErrorThrottle t;
    CHECK(t.shouldShow(21084, 67, 1000));
    CHECK_FALSE(t.shouldShow(21084, 67, 1300));
    // Each refusal restarts the 3 s: a mashed key says it once.
    CHECK_FALSE(t.shouldShow(21084, 67, 4200));
    CHECK(t.shouldShow(21084, 67, 7300));
    // Another reason or another spell is new.
    CHECK(t.shouldShow(21084, 105, 7400));
    CHECK(t.shouldShow(635, 105, 7500));
    CHECK(t.shouldShow(21084, 105, 7600));
}

TEST_CASE("not ready reads as the cooldown line, not 'Not yet recovered'") {
    CHECK(std::string(sc::notReadyErrorName(67, 0, 0x50000)) == "ERR_SPELL_COOLDOWN");
    CHECK(std::string(sc::notReadyErrorName(67, 0, 0x10)) == "ERR_ABILITY_COOLDOWN");
    CHECK(std::string(sc::notReadyErrorName(67, 4, 0)) == "ERR_POTION_COOLDOWN");
    CHECK(std::string(sc::notReadyErrorName(67, 11, 0)) == "ERR_FOOD_COOLDOWN");
    CHECK(std::string(sc::notReadyErrorName(45, 0, 0)) == "ERR_ITEM_COOLDOWN");
    CHECK(sc::notReadyErrorName(105, 0, 0) == nullptr);
    CHECK(std::string(sc::notReadyErrorText("ERR_SPELL_COOLDOWN")) == "Spell is not ready yet.");
    CHECK(std::string(sc::notReadyErrorText("ERR_ABILITY_COOLDOWN")) == "Ability is not ready yet.");
}

TEST_CASE("a world click is a quick release") {
    CHECK(sc::isWorldClick(120.0, false));
    CHECK(sc::isWorldClick(799.0, false));
    CHECK_FALSE(sc::isWorldClick(800.0, false));
    // Moved: only a flick under 200 ms.
    CHECK(sc::isWorldClick(150.0, true));
    CHECK_FALSE(sc::isWorldClick(250.0, true));
}
