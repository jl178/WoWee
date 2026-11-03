// Pure helpers behind the macro, talent-group and glyph bindings:
// macro_slots.hpp (DeleteMacro's renumbering of action-bar slots and the
// config escaping) and talent_points.hpp (the inactive group's unspent points
// and the GLYPH_ADDED / REMOVED / UPDATED diff).
#include <catch_amalgamated.hpp>

#include "game/macro_slots.hpp"
#include "game/talent_points.hpp"

using namespace wowee::game;

TEST_CASE("Deleting a macro renumbers action slots in its own half only", "[macro]") {
    // Account macros 1-36: deleting 5 clears a slot on 5, moves 6.. down one,
    // leaves 1-4 and every character macro alone.
    CHECK(macroIdAfterDelete(5, 5) == 0u);
    CHECK(macroIdAfterDelete(6, 5) == 5u);
    CHECK(macroIdAfterDelete(36, 5) == 35u);
    CHECK(macroIdAfterDelete(4, 5) == 4u);
    CHECK(macroIdAfterDelete(37, 5) == 37u);
    // Character macros 37-54.
    CHECK(macroIdAfterDelete(40, 38) == 39u);
    CHECK(macroIdAfterDelete(38, 38) == 0u);
    CHECK(macroIdAfterDelete(20, 38) == 20u);
}

TEST_CASE("Macro bodies survive the one-line config form", "[macro]") {
    const std::string body = "/cast Fireball\n/say a\\b";
    CHECK(escapeMacroBody(body) == "/cast Fireball\\n/say a\\\\b");
    CHECK(unescapeMacroBody(escapeMacroBody(body)) == body);
    CHECK(unescapeMacroBody(escapeMacroBody("")).empty());
}

TEST_CASE("The inactive talent group's unspent points come from the shared total", "[talent]") {
    std::unordered_map<uint32_t, uint8_t> active{{1, 5}, {2, 3}};     // 8 spent
    std::unordered_map<uint32_t, uint8_t> other{{3, 2}};              // 2 spent
    CHECK(talentPointsSpent(active) == 8u);
    // Level 80: 71 points. Active has 8 spent, 63 unspent; the other spec has
    // spent 2 of the same 71.
    CHECK(otherGroupUnspent(talentPointsSpent(active), 63, talentPointsSpent(other)) == 69u);
    CHECK(otherGroupUnspent(0, 0, 5) == 0u);   // never negative
}

TEST_CASE("Glyph socket changes name the socket from one", "[glyph]") {
    std::array<uint16_t, 6> before{0, 21, 22, 0, 0, 0};
    std::array<uint16_t, 6> after{81, 21, 0, 0, 0, 0};
    const auto changes = glyphChanges(before, after);
    REQUIRE(changes.size() == 2u);
    CHECK(changes[0] == std::make_pair(GlyphChange::Added, 1));
    CHECK(changes[1] == std::make_pair(GlyphChange::Removed, 3));
    after[1] = 61;
    CHECK(glyphChanges(before, after)[1] == std::make_pair(GlyphChange::Updated, 2));
    CHECK(glyphChanges(before, before).empty());
}
