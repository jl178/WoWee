// The stock 3.3.5a rules for the in-world interface the engine draws itself:
// nameplates, names over heads, the numbers over a target and chat bubbles.
// Every expected value here is read off Wow.exe 12340 (docs/re/world-ui.md).
#include <catch_amalgamated.hpp>
#include "ui/world_ui_rules.hpp"

using namespace wowee::ui::stockui;

TEST_CASE("DDC sizes are fractions of the screen diagonal", "[world_ui]") {
    // 4:3 at 768 tall: the diagonal is 1280, so the 0.1 DDC plate is 128 wide.
    REQUIRE(ddcPixels(0.1f, 1024.0f, 768.0f) == Catch::Approx(128.0f));
    REQUIRE(ddcPixels(0.025f, 1024.0f, 768.0f) == Catch::Approx(32.0f));
    // Widescreen plates are wider than the 4:3 ones at the same height.
    REQUIRE(ddcPixels(0.1f, 1920.0f, 1080.0f) == Catch::Approx(220.3f).margin(0.1f));
    REQUIRE(plateUnitPixels(1024.0f, 768.0f) == Catch::Approx(1.0f));
}

TEST_CASE("Nameplate bar colours follow reaction, with no tapped grey", "[world_ui]") {
    REQUIRE(nameplateBar(1, false, false) == PlateBar::Red);
    REQUIRE(nameplateBar(2, false, false) == PlateBar::Red);
    // Unfriendly is yellow on a plate (orange only on the name).
    REQUIRE(nameplateBar(3, false, false) == PlateBar::Yellow);
    REQUIRE(nameplateBar(4, false, false) == PlateBar::Yellow);
    REQUIRE(nameplateBar(5, false, false) == PlateBar::Green);
    REQUIRE(nameplateBar(5, true, false) == PlateBar::Blue);
    REQUIRE(nameplateBar(4, true, true) == PlateBar::Blue);
    // Class colours only for a player you can fight.
    REQUIRE(nameplateBar(2, true, true) == PlateBar::ClassColour);
    REQUIRE(nameplateBar(2, false, true) == PlateBar::Red);
}

TEST_CASE("Nameplate level colours are the plate's own table", "[world_ui]") {
    REQUIRE(nameplateLevelRgb(85, 80) == Rgb{255, 25, 25});
    REQUIRE(nameplateLevelRgb(83, 80) == Rgb{255, 127, 63});
    REQUIRE(nameplateLevelRgb(80, 80) == Rgb{255, 255, 0});
    REQUIRE(nameplateLevelRgb(78, 80) == Rgb{255, 255, 0});
    // Level 80: the green range is 8 (0xaa34b8[16]).
    REQUIRE(nameplateLevelRgb(72, 80) == Rgb{63, 178, 63});
    REQUIRE(nameplateLevelRgb(71, 80) == Rgb{127, 127, 127});
    // Level 10: the range is 5.
    REQUIRE(nameplateLevelRgb(5, 10) == Rgb{63, 178, 63});
    REQUIRE(nameplateLevelRgb(4, 10) == Rgb{127, 127, 127});
}

TEST_CASE("Nameplate threat glow, alpha and name colour", "[world_ui]") {
    REQUIRE_FALSE(nameplateThreatShown(0));
    REQUIRE(nameplateThreatShown(1));
    REQUIRE(nameplateThreatRgb(1) == Rgb{255, 255, 119});
    REQUIRE(nameplateThreatRgb(2) == Rgb{255, 153, 0});
    REQUIRE(nameplateThreatRgb(3) == Rgb{255, 0, 0});
    REQUIRE(nameplateAlpha(false, false) == 255);
    REQUIRE(nameplateAlpha(true, true) == 255);
    REQUIRE(nameplateAlpha(true, false) == 127);
    REQUIRE(nameplateNameRgb(false, false) == Rgb{255, 255, 255});
    REQUIRE(nameplateNameRgb(false, true) == Rgb{255, 255, 0});
    REQUIRE(nameplateNameRgb(true, true) == Rgb{255, 0, 0});
}

TEST_CASE("Names over heads follow the UnitName bits", "[world_ui]") {
    const uint32_t defaults = kNamePlayerGuild | kNamePlayerPvpTitle | kNameEnemyPlayer |
                              kNameEnemyPet | kNameFriendlyPlayer | kNameFriendlyPet;
    REQUIRE_FALSE(unitNameShown(NameKind::Self, false, true, false, defaults));
    REQUIRE_FALSE(unitNameShown(NameKind::Npc, false, true, false, defaults));
    REQUIRE(unitNameShown(NameKind::Player, false, true, false, defaults));
    REQUIRE(unitNameShown(NameKind::Player, false, false, false, defaults));
    REQUIRE(unitNameShown(NameKind::PlayerPet, false, false, false, defaults));
    REQUIRE_FALSE(unitNameShown(NameKind::PlayerTotem, false, true, false, defaults));
    REQUIRE_FALSE(unitNameShown(NameKind::NonCombatCreature, false, true, false, defaults));
    // The target is always named; a stealthed enemy never is.
    REQUIRE(unitNameShown(NameKind::Npc, true, false, false, defaults));
    REQUIRE_FALSE(unitNameShown(NameKind::Player, false, false, true, defaults));
    // The stock quirk: a friendly guardian answers to the enemy bit.
    REQUIRE(unitNameShown(NameKind::PlayerGuardian, false, true, false, kNameEnemyGuardian));
    REQUIRE_FALSE(unitNameShown(NameKind::PlayerGuardian, false, true, false, kNameFriendlyGuardian));
}

TEST_CASE("Name size and the raid mark's fade", "[world_ui]") {
    REQUIRE(unitNameLineYards(2.0f) == Catch::Approx(0.2f));
    REQUIRE(unitNameLineYards(4.0f) == Catch::Approx(0.2f));
    REQUIRE(unitNameLineYards(8.0f) == Catch::Approx(0.6f));
    REQUIRE(headMarkAlpha(2.0f) == 34);
    REQUIRE(headMarkAlpha(6.0f) == 145);
    REQUIRE(headMarkAlpha(20.0f) == 255);
    REQUIRE(reactionNameRgb(3) == Rgb{255, 128, 0});
}

TEST_CASE("World text timings", "[world_ui]") {
    // Damage: in over 150 ms, out from 760 to 1500, two yards of rise.
    REQUIRE(worldTextAlpha(kWtDamage, 75.0f) == Catch::Approx(0.5f));
    REQUIRE(worldTextAlpha(kWtDamage, 500.0f) == Catch::Approx(1.0f));
    REQUIRE(worldTextAlpha(kWtDamage, 1130.0f) == Catch::Approx(0.5f));
    REQUIRE(worldTextAlpha(kWtDamage, 1500.0f) == Catch::Approx(0.0f));
    REQUIRE(worldTextRise(kWtDamage, 750.0f) == Catch::Approx(1.0f));
    REQUIRE(worldTextHeight(kWtDamage, 400.0f) == Catch::Approx(0.018333f));
    // A crit does not rise, pops to twice its size at 150 ms, settles by 300.
    REQUIRE(worldTextRise(kWtCritDamage, 750.0f) == Catch::Approx(0.0f));
    REQUIRE(worldTextHeight(kWtCritDamage, 0.0f) == Catch::Approx(0.00275f));
    REQUIRE(worldTextHeight(kWtCritDamage, 150.0f) == Catch::Approx(0.055f));
    REQUIRE(worldTextHeight(kWtCritDamage, 300.0f) == Catch::Approx(0.0275f));
    REQUIRE(worldTextHeight(kWtCritDamage, 1000.0f) == Catch::Approx(0.0275f));
    // Misses fade late; an absorb starts fading at once.
    REQUIRE(worldTextAlpha(kWtMiss, 900.0f) == Catch::Approx(1.0f));
    REQUIRE(worldTextAlpha(kWtAbsorb, 795.0f) == Catch::Approx(0.5f));
}

TEST_CASE("Chat bubbles: which lines, how long", "[world_ui]") {
    REQUIRE(chatBubbleGate(0x01) == 1);  // say
    REQUIRE(chatBubbleGate(0x06) == 1);  // yell
    REQUIRE(chatBubbleGate(0x0C) == 1);  // monster say
    REQUIRE(chatBubbleGate(0x0E) == 1);  // monster yell
    REQUIRE(chatBubbleGate(0x02) == 2);  // party
    REQUIRE(chatBubbleGate(0x33) == 2);  // party leader
    REQUIRE(chatBubbleGate(0x03) == 0);  // raid
    REQUIRE(chatBubbleGate(0x0D) == 0);  // monster party
    REQUIRE(chatBubbleGate(0x07) == 0);  // whisper
    REQUIRE(chatBubbleBreaks("hello") == 0);
    REQUIRE(chatBubbleBreaks("hello there") == 1);
    REQUIRE(chatBubbleBreaks("hello there ") == 2);
    REQUIRE(chatBubbleBreaks("a  b") == 1);
    REQUIRE(chatBubbleLifetimeMs("hello there friend", false) == 2750 + 750 * 2);
    REQUIRE(chatBubbleLifetimeMs("hello there friend", true) == 1500 + 500 * 2);
}
