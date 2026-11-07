#include <catch_amalgamated.hpp>

#include "game/stock_aura_display.hpp"

using namespace wowee::game::stock_aura;

// Which auras the stock client lists (Wow.exe 0x72C510 / 0x53D580). Jered's
// "Unknown" buff was Phase Shift: IPP (89511), a realm module's aura the
// client's Spell.dbc has no row for; "Victorious" (32216) on his mage has a
// row whose Attributes carry 0x80.

namespace {
AuraDisplayRow buff() {
    AuraDisplayRow r;
    r.hasRow = true;
    r.effects[0] = 6; // APPLY_AURA
    return r;
}
constexpr uint8_t kEffect0 = 0x01;
constexpr uint8_t kSelf = kAuraFlagSelfCast;
} // namespace

TEST_CASE("an aura with a row and nothing hiding it is listed", "[stock_aura]") {
    REQUIRE(listed(buff(), kEffect0 | kSelf, true, true));
    REQUIRE(listed(buff(), kEffect0, true, false));
}

TEST_CASE("an aura with no Spell.dbc row is not listed", "[stock_aura]") {
    AuraDisplayRow none;
    REQUIRE_FALSE(listed(none, kEffect0 | kSelf, true, true));
    REQUIRE_FALSE(listed(none, kEffect0, false, true));
}

TEST_CASE("an aura with no effect mask is not listed", "[stock_aura]") {
    REQUIRE_FALSE(listed(buff(), kSelf, true, true));
    // The older flags mean something else in those bits.
    REQUIRE(listed(buff(), 0, false, true));
}

TEST_CASE("Attributes 0x80 and AttributesEx 0x10000000 hide an aura", "[stock_aura]") {
    auto r = buff();
    r.attributes = 0x180; // Victorious (32216)
    REQUIRE_FALSE(listed(r, kEffect0 | kSelf, true, true));
    r = buff();
    r.attributesEx = 0x10000000;
    REQUIRE_FALSE(listed(r, kEffect0 | kSelf, true, true));
    r = buff();
    r.attributes = 0x100; // DO_NOT_LOG alone hides nothing
    REQUIRE(listed(r, kEffect0 | kSelf, true, true));
}

TEST_CASE("a passive aura is listed only with an area-aura effect", "[stock_aura]") {
    auto r = buff();
    r.attributes = kAttrPassive;
    REQUIRE_FALSE(listed(r, kEffect0 | kSelf, true, true));
    for (uint32_t e : {27u, 35u, 65u, 119u, 128u, 129u, 143u}) {
        r.effects[1] = e;
        REQUIRE(listed(r, kEffect0 | kSelf, true, true));
    }
    r.effects[1] = 6;
    REQUIRE_FALSE(listed(r, kEffect0 | kSelf, true, true));
}

TEST_CASE("AttributesEx5 hides by who cast it", "[stock_aura]") {
    auto r = buff();
    r.attributesEx5 = kAttrEx5HideIfSelfCast;
    REQUIRE_FALSE(listed(r, kEffect0 | kSelf, true, true));
    REQUIRE(listed(r, kEffect0, true, true));
    r.attributesEx5 = kAttrEx5HideIfNotSelfCast;
    REQUIRE(listed(r, kEffect0 | kSelf, true, true));
    REQUIRE_FALSE(listed(r, kEffect0, true, true));
}

TEST_CASE("AttributesEx6 0x100000 shows only the player's own", "[stock_aura]") {
    auto r = buff();
    r.attributesEx6 = kAttrEx6OnlyVisibleToCaster;
    REQUIRE(listed(r, kEffect0, true, true));
    REQUIRE_FALSE(listed(r, kEffect0, true, false));
}

TEST_CASE("RequiredAuraVision needs the player's vision bit", "[stock_aura]") {
    auto r = buff();
    r.requiredAuraVision = 3;
    REQUIRE_FALSE(listed(r, kEffect0, true, true, 0));
    REQUIRE_FALSE(listed(r, kEffect0, true, true, 0x1));
    REQUIRE(listed(r, kEffect0, true, true, 0x4));
}
