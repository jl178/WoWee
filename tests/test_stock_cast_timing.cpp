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

namespace {
// Spell.dbc rows of 3.3.5a (columns 5, 6, 16, 31, 32, 33) with their cast time.
sc::MovingCastSpell spellRow(uint32_t ex, uint32_t ex2, uint32_t targets, uint32_t interrupt,
                             uint32_t aura, uint32_t channel, int32_t castMs) {
    return {.attributesEx = ex, .attributesEx2 = ex2, .targets = targets,
            .interruptFlags = interrupt, .auraInterruptFlags = aura,
            .channelInterruptFlags = channel, .castTimeMs = castMs};
}
constexpr uint32_t kForward = 0x1, kStrafe = 0x4, kTurnLeft = 0x10, kFalling = 0x1000;
} // namespace

TEST_CASE("moving refuses a cast-time spell with the movement interrupt flag", "[moving]") {
    const auto holyLight = spellRow(0, 0, 0, 0xF, 0, 0, 2500);
    CHECK(sc::refusedWhileMoving(kForward, holyLight));
    CHECK(sc::refusedWhileMoving(kStrafe, holyLight));
    CHECK(sc::refusedWhileMoving(kFalling, holyLight));
    CHECK(sc::refusedWhileMoving(0x400000, holyLight));  // ascending
    // Turning in place is not moving.
    CHECK_FALSE(sc::refusedWhileMoving(kTurnLeft, holyLight));
    CHECK_FALSE(sc::refusedWhileMoving(0, holyLight));
}

TEST_CASE("instant, auto-repeat and ground spells are not refused for moving", "[moving]") {
    // Seal of Righteousness: instant; also a cast time a spell mod brought
    // to nothing (Presence of Mind).
    CHECK_FALSE(sc::refusedWhileMoving(kForward, spellRow(0, 0, 0, 0xF, 0, 0, 0)));
    // Shoot: auto-repeat (AttributesEx2 0x20), a cast time all the same.
    CHECK_FALSE(sc::refusedWhileMoving(kForward, spellRow(0, 0x20, 0, 0xF, 0, 0x2AC0C, 500)));
    // Blizzard: aimed at a location (Targets 0x40), checked when it lands.
    CHECK_FALSE(sc::refusedWhileMoving(kForward, spellRow(0x1000008C, 0x400000, 0x40, 0xF, 0, 0x7C2C, 0)));
    // A cast time but no movement interrupt flag (Blink's 0x8 is not it).
    CHECK_FALSE(sc::refusedWhileMoving(kForward, spellRow(0, 0, 0, 0x8, 0, 0, 1500)));
}

TEST_CASE("a channel that movement breaks is refused while moving", "[moving]") {
    // Arcane Missiles: AttributesEx 0x4 channelled, ChannelInterruptFlags 0x7C0C.
    CHECK(sc::refusedWhileMoving(kForward, spellRow(0x14004484, 0, 0, 0xF, 0, 0x7C0C, 0)));
    // First Aid's bandage: no InterruptFlags, refused by the channel's 0x8.
    CHECK(sc::refusedWhileMoving(kForward, spellRow(0x4044, 0, 0, 0, 0x2, 0x3C0E, 0)));
    // A channel movement does not break.
    CHECK_FALSE(sc::refusedWhileMoving(kForward, spellRow(0x4, 0, 0, 0, 0, 0x4, 0)));
}

TEST_CASE("an instant whose aura breaks on moving is refused while moving", "[moving]") {
    CHECK(sc::refusedWhileMoving(kForward, spellRow(0, 0, 0, 0x1, 0x8, 0, 0)));
    // Food's aura breaks on standing, not moving: the server answers that one.
    CHECK_FALSE(sc::refusedWhileMoving(kForward, spellRow(0, 0, 0, 0x1, 0x40080, 0, 0)));
}

TEST_CASE("spell mods as 0x7FDB50 applies them", "[moving]") {
    CHECK(sc::applySpellModStock(2500, 0, 0) == 2500);
    CHECK(sc::applySpellModStock(2500, -500, 0) == 2000);
    CHECK(sc::applySpellModStock(2500, 0, -100) == 0);   // Presence of Mind
    CHECK(sc::applySpellModStock(2500, 0, -150) == 0);   // held at zero
    CHECK(sc::applySpellModStock(3000, -500, -20) == 2000);
}

TEST_CASE("what starting to move cancels", "[moving]") {
    CHECK(sc::castCancelledByMoveStart(0xF));
    CHECK_FALSE(sc::castCancelledByMoveStart(0x8));
    CHECK(sc::channelCancelledByMoveStart(0x7C0C));
    CHECK_FALSE(sc::channelCancelledByMoveStart(0x4));
}
