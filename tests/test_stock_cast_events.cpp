// How the stock 3.3.5a client ends a cast (game/stock_cast_events.hpp): the
// event for each result, when STOP goes with it, which cancels send
// CMSG_CANCEL_CAST, and the event sequences of an interrupted cast as the
// client and the server each report it. docs/re/combat-targeting-movement.md
// section 3 ("Casts ending") has the addresses.
#include <catch_amalgamated.hpp>
#include "game/stock_cast_events.hpp"

#include <string>
#include <vector>

namespace sc = wowee::game::stock_cast;

namespace {

/// The events 0x7FECC0 fires for one report, in order.
std::vector<std::string> report(sc::UnitCast& unit, sc::CastRef cast, uint8_t result) {
    const auto steps = sc::castEndSteps(unit, cast, result);
    std::vector<std::string> out{sc::castEndEventName(steps.event)};
    if (steps.stop) out.emplace_back("UNIT_SPELLCAST_STOP");
    return out;
}

const std::vector<std::string> kInterrupted{"UNIT_SPELLCAST_INTERRUPTED"};
const std::vector<std::string> kInterruptedStop{"UNIT_SPELLCAST_INTERRUPTED", "UNIT_SPELLCAST_STOP"};

constexpr uint32_t kHolyLight = 635;

sc::UnitCast castingHolyLight(uint8_t count) {
    return sc::UnitCast{.castCount = count, .spellId = kHolyLight, .barSpellId = kHolyLight};
}

} // namespace

TEST_CASE("0x7FECC0 picks the event by result") {
    CHECK(sc::castEndEvent(sc::kResultInterrupted) == sc::CastEndEvent::Interrupted);
    CHECK(sc::castEndEvent(sc::kResultInterruptedCombat) == sc::CastEndEvent::Interrupted);
    CHECK(sc::castEndEvent(sc::kResultDontReport) == sc::CastEndEvent::FailedQuiet);
    CHECK(sc::castEndEvent(sc::kResultSpellInProgress) == sc::CastEndEvent::FailedQuiet);
    CHECK(sc::castEndEvent(sc::kResultCharmed) == sc::CastEndEvent::FailedQuiet);
    CHECK(sc::castEndEvent(sc::kResultSucceeded) == sc::CastEndEvent::Succeeded);
    CHECK(sc::castEndEvent(97) == sc::CastEndEvent::Failed);   // out of range
    CHECK(sc::castEndEvent(67) == sc::CastEndEvent::Failed);   // not ready
    CHECK(std::string(sc::castEndEventName(sc::CastEndEvent::Failed)) == "UNIT_SPELLCAST_FAILED");
}

TEST_CASE("a move start's cancel, then the server's three reports") {
    // Holy Light, count 7, its bar up. The move start (0x806620 -> 0x806200
    // with INTERRUPTED) ends it: the event, then STOP.
    auto unit = castingHolyLight(7);
    const sc::CastRef cast{7, kHolyLight};
    CHECK(report(unit, cast, sc::kResultInterrupted) == kInterruptedStop);
    CHECK(unit.spellId == 0);
    CHECK(unit.barSpellId == 0);
    // SMSG_CAST_FAILED, SMSG_SPELL_FAILURE, SMSG_SPELL_FAILED_OTHER: the
    // event each time, never a second STOP.
    CHECK(report(unit, cast, sc::kResultInterrupted) == kInterrupted);
    CHECK(report(unit, cast, sc::kResultInterrupted) == kInterrupted);
    CHECK(report(unit, cast, sc::kResultInterrupted) == kInterrupted);
}

TEST_CASE("SpellStopCasting cancels quietly; the server's reports say interrupted") {
    auto unit = castingHolyLight(3);
    const sc::CastRef cast{3, kHolyLight};
    // 0x809A80 -> 0x806200 with DONT_REPORT.
    CHECK(report(unit, cast, sc::kResultDontReport) ==
          std::vector<std::string>{"UNIT_SPELLCAST_FAILED_QUIET", "UNIT_SPELLCAST_STOP"});
    CHECK(report(unit, cast, sc::kResultInterrupted) == kInterrupted);
    CHECK(report(unit, cast, sc::kResultInterrupted) == kInterrupted);
    CHECK(report(unit, cast, sc::kResultInterrupted) == kInterrupted);
}

TEST_CASE("a kick or a pushback cancel: the server's first report ends the cast") {
    auto unit = castingHolyLight(9);
    const sc::CastRef cast{9, kHolyLight};
    // SMSG_CAST_FAILED (the caster's own) first, then the two broadcasts.
    CHECK(report(unit, cast, sc::kResultInterrupted) == kInterruptedStop);
    CHECK(report(unit, cast, sc::kResultInterrupted) == kInterrupted);
    CHECK(report(unit, cast, sc::kResultInterrupted) == kInterrupted);
}

TEST_CASE("another unit's interrupt: STOP with SMSG_SPELL_FAILURE only") {
    // A creature's cast, count 0 as the server's own are.
    sc::UnitCast unit{.castCount = 0, .spellId = 9053, .barSpellId = 9053};
    const sc::CastRef cast{0, 9053};
    CHECK(report(unit, cast, sc::kResultInterrupted) == kInterruptedStop);   // SPELL_FAILURE
    CHECK(report(unit, cast, sc::kResultInterrupted) == kInterrupted);       // FAILED_OTHER
}

TEST_CASE("a refused press during a cast leaves the cast and its bar alone") {
    auto unit = castingHolyLight(4);
    // The same spell pressed again: count 5, "Another action is in progress".
    CHECK(report(unit, sc::CastRef{5, kHolyLight}, sc::kResultSpellInProgress) ==
          std::vector<std::string>{"UNIT_SPELLCAST_FAILED_QUIET"});
    CHECK(unit.spellId == kHolyLight);
    CHECK(unit.barSpellId == kHolyLight);
    CHECK(unit.castCount == 4);
    // Then its own end still finds it.
    CHECK(report(unit, sc::CastRef{4, kHolyLight}, sc::kResultInterrupted) == kInterruptedStop);
}

TEST_CASE("a press with no bar yet ends its cast without a STOP") {
    // Sent, refused before SMSG_SPELL_START: +0xA60 set by the send, no bar.
    sc::UnitCast unit{.castCount = 2, .spellId = kHolyLight, .barSpellId = 0};
    const auto steps = sc::castEndSteps(unit, sc::CastRef{2, kHolyLight}, 97);
    CHECK(steps.endsCast);
    CHECK_FALSE(steps.stop);
    CHECK(steps.event == sc::CastEndEvent::Failed);
}

TEST_CASE("0x806200 tells the server only about a cast with a cast time") {
    CHECK(sc::cancelSendsPacket(2500, 0x10000));         // Holy Light
    CHECK_FALSE(sc::cancelSendsPacket(0, 0x50012));      // Auto Shot (cast time -1000000 -> none)
    CHECK_FALSE(sc::cancelSendsPacket(-1000000, 0x50012));
    CHECK_FALSE(sc::cancelSendsPacket(0, 0x12));         // Shoot (wand)
    CHECK(sc::cancelSendsPacket(0, 0x400));              // Heroic Strike: next swing
    CHECK(sc::cancelSendsPacket(0, 0x4));
}

TEST_CASE("the player's casts in flight") {
    sc::PendingCasts list;
    list.add({1, 635});
    list.add({2, 75});       // Auto Shot's press: never answered while it repeats
    list.add({3, 20271});
    CHECK(list.size() == 3);
    CHECK(list.contains({2, 75}));
    // An Auto Shot shot (count 0) does not take the press's record.
    CHECK_FALSE(list.remove({0, 75}));
    CHECK(list.remove({1, 635}));
    CHECK_FALSE(list.contains({1, 635}));
    // Walking a copy while removing, as 0x806620 does.
    for (const auto& c : list.snapshot()) list.remove(c);
    CHECK(list.size() == 0);
    // A cap, so casts never answered do not pile up.
    for (int i = 0; i < 100; ++i) list.add({static_cast<uint8_t>(i), 100u + static_cast<uint32_t>(i)});
    CHECK(list.size() == sc::PendingCasts::kMax);
    CHECK(list.contains({99, 199}));
    CHECK_FALSE(list.contains({0, 100}));
    // Nothing for spell 0.
    list.clear();
    list.add({1, 0});
    CHECK(list.size() == 0);
}

TEST_CASE("the fizzle: the first school bit, never for DONT_REPORT") {
    CHECK(sc::fizzleSchool(0x02) == 1);   // holy
    CHECK(sc::fizzleSchool(0x04) == 2);   // fire
    CHECK(sc::fizzleSchool(0x14) == 2);   // frostfire: fire first
    CHECK(sc::fizzleSchool(0x01) == 0);   // physical (Resistances row 0 has no sound)
    CHECK(sc::fizzleSchool(0) == -1);
    CHECK(sc::castFailurePlaysFizzle(sc::kResultInterrupted));
    CHECK_FALSE(sc::castFailurePlaysFizzle(sc::kResultDontReport));
}

TEST_CASE("SMSG_SPELL_FAILURE stops the auto-repeat for want of ammunition") {
    CHECK(sc::spellFailureStopsAutoRepeat(sc::kResultNeedAmmo));
    CHECK(sc::spellFailureStopsAutoRepeat(sc::kResultNeedAmmoPouch));
    CHECK(sc::spellFailureStopsAutoRepeat(sc::kResultNeedExoticAmmo));
    CHECK_FALSE(sc::spellFailureStopsAutoRepeat(sc::kResultInterrupted));
}
