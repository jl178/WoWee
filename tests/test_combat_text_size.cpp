#include <catch_amalgamated.hpp>

#include "ui/combat_text_size.hpp"
#include "ui/settings_schema.hpp"

#include <cstring>

using namespace wowee::ui;

// Jered asked for a way to size the combat text. One percentage over the
// world text, this client's stand-in floating text and Blizzard_CombatText.

TEST_CASE("100% is the stock size", "[combat_text_size]") {
    REQUIRE(combatTextScale(100) == 1.0f);
    const auto h = blizzardCombatTextHeights(100);
    REQUIRE(h.normal == 25.0f);   // COMBAT_TEXT_HEIGHT
    REQUIRE(h.critMin == 30.0f);  // COMBAT_TEXT_CRIT_MINHEIGHT
    REQUIRE(h.critMax == 60.0f);  // COMBAT_TEXT_CRIT_MAXHEIGHT
}

TEST_CASE("the size scales every height together", "[combat_text_size]") {
    const auto half = blizzardCombatTextHeights(50);
    REQUIRE(half.normal == 12.5f);
    REQUIRE(half.critMin == 15.0f);
    REQUIRE(half.critMax == 30.0f);
    const auto twice = blizzardCombatTextHeights(200);
    REQUIRE(twice.normal == 50.0f);
    REQUIRE(twice.critMax == 120.0f);
}

TEST_CASE("the size is held to 50%-200%", "[combat_text_size]") {
    REQUIRE(combatTextScale(10) == 0.5f);
    REQUIRE(combatTextScale(500) == 2.0f);
    REQUIRE(blizzardCombatTextHeights(0).normal == 12.5f);
}

TEST_CASE("the setting is on the Combat Text page with the setting's range", "[combat_text_size]") {
    std::size_t count = 0;
    const SettingDesc* rows = clientSettingsSchema(count);
    const SettingDesc* row = nullptr;
    for (std::size_t i = 0; i < count; ++i)
        if (std::strcmp(rows[i].key, "combattextscale") == 0) row = &rows[i];
    REQUIRE(row != nullptr);
    REQUIRE(std::strcmp(row->category, "Combat Text") == 0);
    REQUIRE(row->kind == SettingKind::Int);
    REQUIRE(row->minValue == static_cast<float>(kCombatTextScaleMinPct));
    REQUIRE(row->maxValue == static_cast<float>(kCombatTextScaleMaxPct));
    REQUIRE(row->defaultValue == static_cast<float>(kCombatTextScaleDefaultPct));
}
