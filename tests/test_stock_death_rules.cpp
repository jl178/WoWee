// The stock 3.3.5a death, ghost, corpse and loot-roll rules
// (game/stock_death_rules.hpp). Values are the ones read from Wow.exe 12340;
// docs/re/death-ghost-loot.md has the addresses.
#include <catch_amalgamated.hpp>
#include "game/stock_death_rules.hpp"

namespace sd = wowee::game::stock_death;

TEST_CASE("a death in the open world releases itself after six minutes") {
    // RELEASE_TIMER set, in bounds: 360 s.
    CHECK(sd::releaseDelayMs(sd::kFieldByteReleaseTimer, 0) == 360000);
    CHECK(sd::releaseSecondsRemaining(360000, 1000, 1000) == 360);
    // Truncated to whole seconds, and 0 once run out.
    CHECK(sd::releaseSecondsRemaining(360000, 1000, 1000 + 1500) == 358);
    CHECK(sd::releaseSecondsRemaining(360000, 1000, 1000 + 360000) == 0);
    CHECK(sd::releaseSecondsRemaining(360000, 1000, 1000 + 999999) == 0);
}

TEST_CASE("no timer in an instance, none out of bounds, at once with no window") {
    // Neither flag (instances and battlegrounds): -1, the "no timer" text.
    CHECK(sd::releaseDelayMs(0, 0) == -1);
    CHECK(sd::releaseSecondsRemaining(-1, 0, 5000) == -1);
    // Fell through the world: the timer flag is ignored.
    CHECK(sd::releaseDelayMs(sd::kFieldByteReleaseTimer, sd::kPlayerFlagOutOfBounds) == -1);
    // NO_RELEASE_WINDOW wins over everything: 0.
    CHECK(sd::releaseDelayMs(sd::kFieldByteNoReleaseWindow | sd::kFieldByteReleaseTimer, 0) == 0);
    CHECK(sd::releaseSecondsRemaining(0, 1000, 1000) == 0);
}

TEST_CASE("the ghost's screen effect is row 1; a corpse has none; auras win") {
    CHECK(sd::screenEffectId({}, 0, 0) == 0);
    CHECK(sd::screenEffectId({}, sd::kPlayerFlagGhost, 0) == sd::kScreenEffectGhost);
    CHECK(sd::screenEffectId({}, 0, sd::kBytes2InvisibilityGlow) == sd::kScreenEffectInvisGlow);
    // The last SCREEN_EFFECT aura in slot order, even on a ghost.
    CHECK(sd::screenEffectId({43, 482}, sd::kPlayerFlagGhost, 0) == 482);
}

TEST_CASE("the ghost row forces the death light slot; rows past 7 force none") {
    const sd::ScreenEffectRow ghost{1, 1, 4, 462, 36};
    CHECK(sd::lightSlotOverride(ghost) == sd::kLightSlotDeath);
    CHECK(sd::lightSlotOverride(sd::ScreenEffectRow{43, 3, -1, 0, 0}) == -1);
    CHECK(sd::lightSlotOverride(sd::ScreenEffectRow{9, 0, 8, 0, 0}) == -1);
    CHECK(sd::lightSlotOverride(sd::ScreenEffectRow{}) == -1);
}

TEST_CASE("the corpse is in range under 40 yards") {
    CHECK(sd::corpseInRange(39.9f * 39.9f));
    CHECK_FALSE(sd::corpseInRange(40.0f * 40.0f));
}

TEST_CASE("a player's resurrection offers neither sickness nor a timer") {
    // AzerothCore writes 0 then (caster is a creature): the client reads the
    // first as sickness and the second as the timer.
    const auto fromPlayer = sd::resurrectOffer(0, 0);
    CHECK_FALSE(fromPlayer.sickness);
    CHECK_FALSE(fromPlayer.timer);
    const auto fromCreature = sd::resurrectOffer(0, 1);
    CHECK_FALSE(fromCreature.sickness);
    CHECK(fromCreature.timer);
}

TEST_CASE("GlobalStrings formats expand in order and by position") {
    CHECK(sd::formatGlobal("%s won: %s", {"Bob", "[Axe]"}) == "Bob won: [Axe]");
    CHECK(sd::formatGlobal("Need Roll - %d for %s by %s", {"54", "[Axe]", "Bob"}) ==
          "Need Roll - 54 for [Axe] by Bob");
    CHECK(sd::formatGlobal("%1$s won: %3$s |cff818181(Need - %2$d)|r", {"Bob", "54", "[Axe]"}) ==
          "Bob won: [Axe] |cff818181(Need - 54)|r");
    CHECK(sd::formatGlobal("100%% done", {}) == "100% done");
}

TEST_CASE("the looted coin is spelled out and the share is said as a share") {
    auto lookup = [](const char* key) { return std::string(sd::englishGlobal(key)); };
    CHECK(sd::lootMoneyLine(105, true, lookup) == "You loot 1 Silver, 5 Copper");
    CHECK(sd::lootMoneyLine(20003, true, lookup) == "You loot 2 Gold, 3 Copper");
    CHECK(sd::lootMoneyLine(7, false, lookup) == "Your share of the loot is 7 Copper.");
}

TEST_CASE("a loot roll's choice, roll and win say what the stock client says") {
    const std::string item = "[Axe]";
    // Choices come with number 128 (-128 as the signed byte).
    CHECK(sd::lootRollVoteLine(false, -128, 0, false, false, true, "Bob", item).key == "LOOT_ROLL_PASSED");
    CHECK(sd::lootRollVoteLine(true, -128, 0, false, false, true, "Me", item).key == "LOOT_ROLL_PASSED_SELF");
    CHECK(sd::lootRollVoteLine(true, -128, 0, true, false, true, "Me", item).key == "LOOT_ROLL_PASSED_SELF_AUTO");
    CHECK(sd::lootRollVoteLine(false, -128, 0, true, true, true, "Ann", item).key == "LOOT_ROLL_PASSED_AUTO_FEMALE");
    CHECK(sd::lootRollVoteLine(false, -128, 2, false, false, true, "Bob", item).key == "LOOT_ROLL_GREED");
    CHECK(sd::lootRollVoteLine(true, -128, 3, false, false, true, "Me", item).key == "LOOT_ROLL_DISENCHANT_SELF");
    // Need is sent as number 0, type 0.
    CHECK(sd::lootRollVoteLine(false, 0, 0, false, false, true, "Bob", item).key == "LOOT_ROLL_NEED");
    // The rolls themselves.
    const auto need = sd::lootRollVoteLine(false, 54, 1, false, false, true, "Bob", item);
    CHECK(need.key == "LOOT_ROLL_ROLLED_NEED");
    CHECK(sd::formatGlobal(sd::englishGlobal(need.key), need.args) == "Need Roll - 54 for [Axe] by Bob");
    CHECK(sd::lootRollVoteLine(false, 12, 2, false, false, true, "Bob", item).key == "LOOT_ROLL_ROLLED_GREED");
    // showLootSpam off: only the player's own automatic pass.
    CHECK(sd::lootRollVoteLine(false, 54, 1, false, false, false, "Bob", item).key.empty());
    CHECK(sd::lootRollVoteLine(true, -128, 0, true, false, false, "Me", item).key == "LOOT_ROLL_PASSED_SELF_AUTO");
    // Wins.
    CHECK(sd::lootRollWonLine(true, 54, 1, true, "Me", item).key == "LOOT_ROLL_YOU_WON");
    const auto quiet = sd::lootRollWonLine(false, 54, 2, false, "Bob", item);
    CHECK(sd::formatGlobal(sd::englishGlobal(quiet.key), quiet.args) ==
          "Bob won: [Axe] |cff818181(Greed - 54)|r");
}

TEST_CASE("four loot windows at most") {
    CHECK(sd::kMaxShownLootRolls == 4);
}

TEST_CASE("a death on a flight releases at once (0x6dc0f0)") {
    using namespace wowee::game::stock_death;
    CHECK(releasesAtDeath(0x100000));
    CHECK(releasesAtDeath(0x100000 | 0x8));
    CHECK_FALSE(releasesAtDeath(0x8));
    CHECK_FALSE(releasesAtDeath(0));
}
