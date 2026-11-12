// How big a creature draws (core/creature_scale.hpp, stock 0x71C110).
//
// A hunter's level 80 Frostworg - a 3.0 display - drew three times the size
// of a wolf: the client multiplied the display's size by the server's object
// scale and never read CreatureFamily, which is what sizes a pet.
#include <catch_amalgamated.hpp>
#include "core/creature_scale.hpp"

using namespace wowee::core;

namespace {
// CreatureFamily.dbc rows of 3.3.5a (ID 1 Wolf, 3 Spider, 15 Felhunter,
// 28 Remote Control).
constexpr CreatureFamilyScale kWolf{0.7f, 1, 1.0f, 60};
constexpr CreatureFamilyScale kSpider{0.4f, 1, 0.6f, 60};
constexpr CreatureFamilyScale kFelhunter{0.7f, 1, 0.7f, 60};
constexpr CreatureFamilyScale kRemoteControl{0.0f, 0, 0.0f, 0};
}  // namespace

TEST_CASE("the family scale grows from MinScale to MaxScale over its levels", "[creature-scale]") {
    CHECK(creatureFamilyScaleAt(kWolf, 1) == Catch::Approx(0.7f));
    CHECK(creatureFamilyScaleAt(kWolf, 0) == Catch::Approx(0.7f));      // below: clamped
    CHECK(creatureFamilyScaleAt(kWolf, 30) == Catch::Approx(0.7f + 0.3f * 29.0f / 59.0f));
    CHECK(creatureFamilyScaleAt(kWolf, 60) == Catch::Approx(1.0f));
    CHECK(creatureFamilyScaleAt(kWolf, 80) == Catch::Approx(1.0f));     // above: clamped
    // No span: MinScale (t stays 0).
    CHECK(creatureFamilyScaleAt(CreatureFamilyScale{0.5f, 10, 0.9f, 10}, 80) == Catch::Approx(0.5f));
}

TEST_CASE("a pet is sized by its family, never by the display it was tamed with",
          "[creature-scale]") {
    // Frostworg, display 26331 at 3.0, as a level 80 hunter's wolf.
    CHECK(creatureUnitScale(3.0f, &kWolf, 80, true) == Catch::Approx(1.0f));
    // A small display grows to the family's size too.
    CHECK(creatureUnitScale(0.5f, &kFelhunter, 80, true) == Catch::Approx(0.7f));
    // And a low level pet is smaller.
    CHECK(creatureUnitScale(3.0f, &kWolf, 1, true) == Catch::Approx(0.7f));
}

TEST_CASE("a wild creature takes its family's size only when that is larger", "[creature-scale]") {
    // The wild Frostworg keeps its 3.0.
    CHECK(creatureUnitScale(3.0f, &kWolf, 80, false) == Catch::Approx(3.0f));
    // A 0.3 spider display at level 60 is brought up to the family's 0.6.
    CHECK(creatureUnitScale(0.3f, &kSpider, 60, false) == Catch::Approx(0.6f));
    // No family: the display alone.
    CHECK(creatureUnitScale(1.75f, nullptr, 80, true) == Catch::Approx(1.75f));
}

TEST_CASE("an unset display size is 1 and a zero family never hides a unit", "[creature-scale]") {
    CHECK(creatureUnitScale(0.0f, nullptr, 1, false) == Catch::Approx(1.0f));
    CHECK(creatureUnitScale(1.2f, &kRemoteControl, 80, true) == Catch::Approx(1.2f));
}
