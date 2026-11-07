// The damage meter's arithmetic, kept apart from the window that draws it:
// who a number belongs to once pets are folded into their owners, what a
// per-second figure is divided by, and where one fight ends and the next one
// begins.
#include <catch_amalgamated.hpp>
#include "game/damage_meter.hpp"

using namespace wowee::game;

namespace {

constexpr uint64_t kMe = 0x10;
constexpr uint64_t kFriend = 0x20;
constexpr uint64_t kMyPet = 0xF140000000000030ull;
constexpr uint64_t kWolf = 0xF130000000000040ull;
constexpr uint64_t kBoar = 0xF130000000000050ull;

MeterEvent hit(uint64_t src, uint64_t credited, uint64_t dst, int32_t amount,
               uint32_t spell = 0, bool crit = false) {
    MeterEvent e;
    e.kind = MeterEvent::Damage;
    e.src = src;
    e.srcCombatant = credited;
    e.dst = dst;
    e.amount = amount;
    e.spellId = spell;
    e.crit = crit;
    return e;
}

MeterEvent heal(uint64_t src, uint64_t dst, int32_t amount, int32_t over, uint32_t spell = 2050) {
    MeterEvent e;
    e.kind = MeterEvent::Heal;
    e.src = src;
    e.srcCombatant = src;
    e.dst = dst;
    e.dstCombatant = dst;
    e.amount = amount;
    e.over = over;
    e.spellId = spell;
    return e;
}

MeterEvent taken(uint64_t src, uint64_t dst, int32_t amount, uint32_t spell = 0) {
    MeterEvent e;
    e.kind = MeterEvent::Damage;
    e.src = src;
    e.dst = dst;
    e.dstCombatant = dst;
    e.amount = amount;
    e.spellId = spell;
    return e;
}

MeterEvent death(uint64_t who) {
    MeterEvent e;
    e.kind = MeterEvent::Death;
    e.dst = who;
    e.dstCombatant = who;
    return e;
}

}  // namespace

TEST_CASE("a pet's damage is its owner's, and its abilities say whose they were", "[meter]") {
    DamageMeter m;
    m.update(0.0, true);
    m.record(1.0, hit(kMe, kMe, kWolf, 300, 133));
    m.record(2.0, hit(kMyPet, kMe, kWolf, 200, 0));
    m.record(3.0, hit(kFriend, kFriend, kWolf, 100, 0));
    const auto& v = m.view(DamageMeter::kCurrent, MeterMode::DamageDone, 10.0);
    REQUIRE(v.count == 2);
    CHECK(v.rows[0].combatant->guid == kMe);
    CHECK(v.rows[0].total == Catch::Approx(500));
    REQUIRE(v.rows[0].combatant->pets.size() == 1);
    CHECK(v.rows[0].combatant->pets[0] == kMyPet);
    CHECK(v.rows[1].combatant->guid == kFriend);
    CHECK(v.rows[0].percent == Catch::Approx(500.0 * 100.0 / 600.0));

    const auto& a = m.abilities(DamageMeter::kCurrent, MeterMode::DamageDone, kMe, 5);
    REQUIRE(a.count == 2);
    CHECK(a.rows[0].spellId == 133);
    CHECK(a.rows[0].pet == 0);
    CHECK(a.rows[1].pet == kMyPet);
    CHECK(a.rows[1].percent == Catch::Approx(40.0));
}

TEST_CASE("per second is over the fight's length, and never under a second", "[meter]") {
    DamageMeter m;
    m.update(100.0, true);
    m.record(100.2, hit(kMe, kMe, kWolf, 50));
    // A fifth of a second in: divided by one second, not by 0.2.
    CHECK(m.view(DamageMeter::kCurrent, MeterMode::Dps, 100.2).rows[0].perSecond ==
          Catch::Approx(50.0));
    m.record(105.0, hit(kMe, kMe, kWolf, 950));
    m.update(110.0, false);
    const auto& v = m.view(DamageMeter::kCurrent, MeterMode::Dps, 500.0);
    REQUIRE(v.count == 1);
    // Ten seconds of fight: the time after it ended does not count.
    CHECK(v.duration == Catch::Approx(10.0));
    CHECK(v.rows[0].perSecond == Catch::Approx(100.0));
    CHECK(v.rows[0].value == Catch::Approx(100.0));
    CHECK(v.rows[0].total == Catch::Approx(1000.0));
}

TEST_CASE("overall is every fight added up, over the time spent fighting", "[meter]") {
    DamageMeter m;
    m.update(0.0, true);
    m.record(1.0, hit(kMe, kMe, kWolf, 400));
    m.update(4.0, false);
    m.update(60.0, true);
    m.record(61.0, hit(kMe, kMe, kBoar, 200));
    m.update(66.0, false);
    const auto& cur = m.view(DamageMeter::kCurrent, MeterMode::DamageDone, 100.0);
    REQUIRE(cur.count == 1);
    CHECK(cur.rows[0].total == Catch::Approx(200));
    const auto& all = m.view(DamageMeter::kOverall, MeterMode::DamageDone, 100.0);
    REQUIRE(all.count == 1);
    CHECK(all.rows[0].total == Catch::Approx(600));
    // Four seconds and six, not the hundred since the first pull.
    CHECK(all.duration == Catch::Approx(10.0));
    CHECK(all.rows[0].perSecond == Catch::Approx(60.0));
    REQUIRE(m.previousCount() == 1);
    const MeterFight* prev = m.fight(DamageMeter::kPrevious);
    REQUIRE(prev != nullptr);
    CHECK(prev->mainTarget == kWolf);
    CHECK(m.fight(DamageMeter::kCurrent)->mainTarget == kBoar);
}

TEST_CASE("combat starting and stopping makes the fights", "[meter]") {
    DamageMeter m;
    SECTION("a combat flag with nothing in it is not kept") {
        m.update(0.0, true);
        m.update(3.0, false);
        CHECK(m.fight(DamageMeter::kCurrent) == nullptr);
        CHECK(m.previousCount() == 0);
    }
    SECTION("a DoT's last tick after combat drops is the same fight") {
        m.update(0.0, true);
        m.record(1.0, hit(kMe, kMe, kWolf, 100));
        m.update(5.0, false);
        m.record(6.5, hit(kMe, kMe, kWolf, 30, 172));
        CHECK(m.previousCount() == 0);
        CHECK(m.view(DamageMeter::kCurrent, MeterMode::DamageDone, 7.0).rows[0].total ==
              Catch::Approx(130));
        // Well after, it is a fight of its own.
        m.record(20.0, hit(kMe, kMe, kBoar, 10));
        CHECK(m.inFight());
        CHECK(m.previousCount() == 1);
    }
    SECTION("fighting no flag announced ends when the fighting does") {
        m.record(10.0, hit(kFriend, kFriend, kWolf, 100));
        CHECK(m.inFight());
        m.record(12.0, hit(kFriend, kFriend, kWolf, 100));
        m.update(14.0, false);
        CHECK(m.inFight());
        m.update(12.0 + DamageMeter::kIdleEnd, false);
        CHECK_FALSE(m.inFight());
        CHECK(m.view(DamageMeter::kCurrent, MeterMode::DamageDone, 50.0).duration ==
              Catch::Approx(2.0));
    }
    SECTION("a heal out of combat starts nothing") {
        m.record(1.0, heal(kMe, kMe, 500, 0));
        CHECK_FALSE(m.inFight());
        CHECK(m.fight(DamageMeter::kCurrent) == nullptr);
    }
    SECTION("the history keeps the last few fights and drops the oldest") {
        for (int i = 0; i < 10; ++i) {
            const double t = i * 100.0;
            m.update(t, true);
            m.record(t + 1, hit(kMe, kMe, kWolf, 10 * (i + 1)));
            m.update(t + 2, false);
        }
        CHECK(m.previousCount() == DamageMeter::kHistory);
        CHECK(m.view(DamageMeter::kCurrent, MeterMode::DamageDone, 2000).rows[0].total ==
              Catch::Approx(100));
        CHECK(m.view(DamageMeter::kPrevious, MeterMode::DamageDone, 2000).rows[0].total ==
              Catch::Approx(90));
        CHECK(m.view(DamageMeter::kOverall, MeterMode::DamageDone, 2000).rows[0].total ==
              Catch::Approx(550));
        m.reset();
        CHECK(m.fight(DamageMeter::kCurrent) == nullptr);
        CHECK(m.view(DamageMeter::kOverall, MeterMode::DamageDone, 2000).count == 0);
    }
}

TEST_CASE("healing is what landed; the rest is overhealing", "[meter]") {
    DamageMeter m;
    m.update(0.0, true);
    m.record(1.0, heal(kMe, kFriend, 1000, 400));
    m.record(2.0, heal(kFriend, kFriend, 500, 0));
    const auto& h = m.view(DamageMeter::kCurrent, MeterMode::HealingDone, 10.0);
    REQUIRE(h.count == 2);
    CHECK(h.rows[0].combatant->guid == kMe);
    CHECK(h.rows[0].total == Catch::Approx(600));
    CHECK(h.rows[0].percent == Catch::Approx(600.0 * 100.0 / 1100.0));
    const auto& o = m.view(DamageMeter::kCurrent, MeterMode::Overhealing, 10.0);
    REQUIRE(o.count == 1);
    // Overhealing's percent is of the healer's own casting.
    CHECK(o.rows[0].percent == Catch::Approx(40.0));
}

TEST_CASE("damage taken, friendly fire and deaths", "[meter]") {
    DamageMeter m;
    m.update(0.0, true);
    m.record(1.0, taken(kWolf, kMe, 120, 0));
    m.record(2.0, taken(kWolf, kMe, 80, 6603));
    // Life Tap: the warlock hurting himself is taken, not done.
    MeterEvent tap = hit(kFriend, kFriend, kFriend, 300, 1454);
    tap.dstCombatant = kFriend;
    m.record(3.0, tap);
    m.record(4.0, death(kMe));
    CHECK(m.view(DamageMeter::kCurrent, MeterMode::DamageDone, 5.0).count == 0);
    const auto& t = m.view(DamageMeter::kCurrent, MeterMode::DamageTaken, 5.0);
    REQUIRE(t.count == 2);
    CHECK(t.rows[0].combatant->guid == kFriend);
    const auto& d = m.view(DamageMeter::kCurrent, MeterMode::Deaths, 5.0);
    REQUIRE(d.count == 1);
    CHECK(d.rows[0].total == Catch::Approx(1));
    const MeterDeath& cause = d.rows[0].combatant->lastDeath;
    CHECK(cause.killer == kWolf);
    CHECK(cause.spellId == 6603);
    CHECK(cause.amount == 80);
    CHECK(cause.at == Catch::Approx(4.0));
    const auto& by = m.abilities(DamageMeter::kCurrent, MeterMode::DamageTaken, kMe, 8);
    REQUIRE(by.count == 2);
    CHECK(by.rows[0].amount == Catch::Approx(120));
}

TEST_CASE("crits and hits are counted per ability", "[meter]") {
    DamageMeter m;
    m.update(0.0, true);
    m.record(1.0, hit(kMe, kMe, kWolf, 100, 133, false));
    m.record(2.0, hit(kMe, kMe, kWolf, 200, 133, true));
    m.record(3.0, hit(kMe, kMe, kWolf, 50, 0, false));
    const auto& a = m.abilities(DamageMeter::kCurrent, MeterMode::DamageDone, kMe, 1);
    REQUIRE(a.count == 1);
    CHECK(a.rows[0].spellId == 133);
    CHECK(a.rows[0].hits == 2);
    CHECK(a.rows[0].crits == 1);
    // The percent is of everything, though only the top one was asked for.
    CHECK(a.rows[0].percent == Catch::Approx(300.0 * 100.0 / 350.0));
}

TEST_CASE("a view of twenty-five does not grow its storage", "[meter]") {
    DamageMeter m;
    m.update(0.0, true);
    for (uint64_t i = 0; i < 25; ++i)
        for (int k = 0; k < 4; ++k)
            m.record(1.0 + k, hit(0x100 + i, 0x100 + i, kWolf, static_cast<int32_t>(10 * i + k),
                                  static_cast<uint32_t>(k)));
    const MeterRow* first = m.view(DamageMeter::kCurrent, MeterMode::Dps, 5.0).rows;
    for (int frame = 0; frame < 100; ++frame) {
        const auto& v = m.view(DamageMeter::kCurrent, MeterMode::Dps, 5.0 + frame);
        REQUIRE(v.count == 25);
        CHECK(v.rows == first);
        CHECK(v.rows[0].combatant->guid == 0x100 + 24);
    }
}
