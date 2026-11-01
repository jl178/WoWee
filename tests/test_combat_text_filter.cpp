// Which Combat Text panel checkbox covers each floating line.
//
// Six of those checkboxes named a CVar that nothing read, so the panel
// remembered a choice the screen ignored. These cases pin the mapping that
// replaced that, including the two that are easy to get backwards: a pet's
// swing must not be answered by the player's own damage row, and effects are
// read against the unit they landed on rather than who caused them.
#include <catch_amalgamated.hpp>
#include "game/combat_text_filter.hpp"

using namespace wowee::game;
using T = CombatTextEntry;

namespace {
constexpr uint64_t kPlayer = 0x1000;
constexpr uint64_t kPet    = 0x2000;
constexpr uint64_t kTarget = 0x3000;
constexpr uint64_t kOther  = 0x4000;

CombatTextFilterRule ruleFor(CombatTextEntry::Type type, bool playerSource,
                             uint64_t src, uint64_t dst) {
    return combatTextFilterFor(type, playerSource, src, dst, kPet, kTarget);
}
}  // namespace

TEST_CASE("damage the player deals answers to the damage row", "[combattext]") {
    const auto r = ruleFor(T::MELEE_DAMAGE, true, kPlayer, kTarget);
    REQUIRE(r.cvar != nullptr);
    CHECK(std::string(r.cvar) == "CombatDamage");
    CHECK(std::string(r.fallback) == "1");
}

TEST_CASE("a pet's swing answers to the pet row, not the player's", "[combattext]") {
    const auto r = ruleFor(T::MELEE_DAMAGE, false, kPet, kTarget);
    REQUIRE(r.cvar != nullptr);
    CHECK(std::string(r.cvar) == "PetMeleeDamage");
}

TEST_CASE("damage arriving at the player is not filtered here", "[combattext]") {
    // "Target Damage" does not mean "damage taken"; incoming hits have their
    // own rows elsewhere in the panel and must keep showing.
    CHECK(ruleFor(T::MELEE_DAMAGE, false, kOther, kPlayer).cvar == nullptr);
    CHECK(ruleFor(T::CRIT_DAMAGE, false, kOther, kPlayer).cvar == nullptr);
    CHECK(ruleFor(T::HEAL, false, kOther, kPlayer).cvar == nullptr);
}

TEST_CASE("periodic and healing have their own rows", "[combattext]") {
    CHECK(std::string(ruleFor(T::PERIODIC_DAMAGE, true, kPlayer, kTarget).cvar)
          == "CombatLogPeriodicSpells");
    CHECK(std::string(ruleFor(T::PERIODIC_HEAL, true, kPlayer, kPlayer).cvar)
          == "CombatLogPeriodicSpells");
    CHECK(std::string(ruleFor(T::HEAL, true, kPlayer, kPlayer).cvar) == "CombatHealing");
    CHECK(std::string(ruleFor(T::CRIT_HEAL, true, kPlayer, kPlayer).cvar) == "CombatHealing");
}

TEST_CASE("effects are read against the unit they landed on", "[combattext]") {
    // On your target, and on anything else, are two different rows - and the
    // second is off in the real client, which is the whole reason the fallback
    // travels with the name.
    const auto onTarget = ruleFor(T::IMMUNE, true, kPlayer, kTarget);
    REQUIRE(onTarget.cvar != nullptr);
    CHECK(std::string(onTarget.cvar) == "fctSpellMechanics");
    CHECK(std::string(onTarget.fallback) == "1");

    const auto elsewhere = ruleFor(T::IMMUNE, true, kPlayer, kOther);
    REQUIRE(elsewhere.cvar != nullptr);
    CHECK(std::string(elsewhere.cvar) == "fctSpellMechanicsOther");
    CHECK(std::string(elsewhere.fallback) == "0");

    // Who caused it does not enter into it.
    CHECK(std::string(ruleFor(T::RESIST, false, kOther, kTarget).cvar) == "fctSpellMechanics");
}

TEST_CASE("lines these six rows do not describe are left alone", "[combattext]") {
    CHECK(ruleFor(T::XP_GAIN, true, kPlayer, kPlayer).cvar == nullptr);
    CHECK(ruleFor(T::HONOR_GAIN, true, kPlayer, kPlayer).cvar == nullptr);
    CHECK(ruleFor(T::ENERGIZE, true, kPlayer, kPlayer).cvar == nullptr);
    CHECK(ruleFor(T::MISS, true, kPlayer, kTarget).cvar == nullptr);
}

// COMBAT_TEXT_UPDATE: one message per hit, in Blizzard_CombatText's terms.
TEST_CASE("a hit on the player is one COMBAT_TEXT_UPDATE message", "[combattext]") {
    using V = std::vector<std::string>;
    CHECK(combatTextUpdateForHit(false, false, 120, 0, 0, 0) == V{"DAMAGE", "120"});
    CHECK(combatTextUpdateForHit(true, false, 300, 0, 0, 0) == V{"SPELL_DAMAGE", "300"});
    CHECK(combatTextUpdateForHit(false, true, 500, 0, 40, 0) == V{"DAMAGE_CRIT", "500"});
    CHECK(combatTextUpdateForHit(true, true, 900, 0, 0, 0) == V{"SPELL_DAMAGE_CRIT", "900"});
    // A partial reduction is the damage tagged with what was taken off.
    CHECK(combatTextUpdateForHit(true, false, 600, 200, 0, 0) == V{"SPELL_RESIST", "600", "200"});
    CHECK(combatTextUpdateForHit(false, false, 80, 0, 30, 0) == V{"BLOCK", "80", "30"});
    CHECK(combatTextUpdateForHit(false, false, 50, 0, 0, 25) == V{"ABSORB", "50", "25"});
    // A whole one is the bare word, which the addon prints from GlobalStrings.
    CHECK(combatTextUpdateForHit(false, false, 0, 0, 0, 400) == V{"ABSORB"});
    CHECK(combatTextUpdateForHit(true, false, 0, 350, 0, 0) == V{"SPELL_RESIST"});
    // Nothing at all is nothing to say.
    CHECK(combatTextUpdateForHit(false, false, 0, 0, 0, 0).empty());
}

TEST_CASE("spell misses and power tokens use the addon's names", "[combattext]") {
    CHECK(std::string(combatTextSpellMissType(T::RESIST)) == "SPELL_RESIST");
    CHECK(std::string(combatTextSpellMissType(T::REFLECT)) == "SPELL_REFLECT");
    CHECK(combatTextSpellMissType(T::MELEE_DAMAGE) == nullptr);
    CHECK(std::string(combatTextPowerToken(0)) == "MANA");
    CHECK(std::string(combatTextPowerToken(6)) == "RUNIC_POWER");
    CHECK(combatTextPowerToken(4) == nullptr);
}
