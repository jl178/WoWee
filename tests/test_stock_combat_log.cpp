// The 3.3.5a combat log as Wow.exe keeps it (game/stock_combat_log.hpp,
// docs/re/chat-combatlog.md "Combat log"): the argument lists 0x74E290 builds,
// what each packet builder writes or leaves out, the unit flags of 0x74DCB0,
// CombatLog_Object_IsA, the filters and the history's walks.
#include <catch_amalgamated.hpp>
#include "game/stock_combat_log.hpp"

using namespace wowee::game;
namespace scl = wowee::game::stock_combat_log;

namespace {
constexpr uint64_t kMe = 0x0000000000000011ULL;
constexpr uint64_t kOrc = 0xF130000BB8000123ULL;   // a creature
constexpr const char* kNil = "\x01";

scl::Resolved names() {
    scl::Resolved r;
    r.timestamp = 1700000000.25;
    r.srcName = "Wptestb";
    r.dstName = "Orc";
    r.srcFlags = 0x511;
    r.dstFlags = 0xA48;
    return r;
}

scl::SpellBrief spellLookup(uint32_t id) {
    scl::SpellBrief b;
    if (id == 133) { b.known = true; b.name = "Fireball"; b.schoolMask = 4; }
    if (id == 139) { b.known = true; b.name = "Renew"; b.schoolMask = 2; }
    if (id == 2139) { b.known = true; b.name = "Counterspell"; b.schoolMask = 64; }
    return b;
}

scl::SpellRules spell() {
    scl::SpellRules s;
    s.known = true;
    return s;
}

std::vector<std::string> fmt(const scl::Record& r) { return scl::formatArgs(r, names(), spellLookup); }
}  // namespace

TEST_CASE("the event table is the client's fifty names in order", "[stockcombatlog]") {
    CHECK(std::string(scl::eventName(scl::ENVIRONMENTAL_DAMAGE)) == "ENVIRONMENTAL_DAMAGE");
    CHECK(std::string(scl::eventName(scl::SPELL_AURA_APPLIED)) == "SPELL_AURA_APPLIED");
    CHECK(std::string(scl::eventName(scl::UNIT_DISSIPATES)) == "UNIT_DISSIPATES");
    CHECK(scl::kEventCount == 50);
    CHECK(scl::eventIndex("PARTY_KILL") == 43);
    CHECK(scl::eventIndex("party_kill") == -1);
    CHECK(std::string(scl::missTypeName(10)) == "ABSORB");
    CHECK(std::string(scl::missTypeName(8)) == "IMMUNE");
    CHECK(std::string(scl::environmentalTypeName(2)) == "FALLING");
}

TEST_CASE("a swing is the eight unit fields and the nine damage fields", "[stockcombatlog]") {
    scl::SwingInput in;
    in.attacker = kMe; in.victim = kOrc;
    in.victimState = 1; in.damage = 42; in.overkill = 3; in.firstSchool = 1;
    in.hitInfo = 0x200;  // critical
    const auto r = scl::buildSwing(in);
    REQUIRE(r);
    const auto a = fmt(*r);
    REQUIRE(a.size() == 17);
    CHECK(a[0] == std::string("\x02") + "1700000000.250");
    CHECK(a[1] == "SWING_DAMAGE");
    CHECK(a[2] == "0x0000000000000011");
    CHECK(a[3] == "Wptestb");
    CHECK(a[4] == "1297");          // 0x511
    CHECK(a[5] == "0xF130000BB8000123");
    CHECK(a[7] == "2632");          // 0xA48
    CHECK(a[8] == "42");
    CHECK(a[9] == "3");             // overkill
    CHECK(a[10] == "1");            // school
    CHECK(a[11] == kNil);           // resisted
    CHECK(a[12] == kNil);           // blocked
    CHECK(a[13] == kNil);           // absorbed
    CHECK(a[14] == "1");            // critical
    CHECK(a[15] == kNil);
    CHECK(a[16] == kNil);
}

TEST_CASE("a swing that did not land names how, with an amount for three kinds", "[stockcombatlog]") {
    scl::SwingInput in;
    in.attacker = kOrc; in.victim = kMe;
    in.hitInfo = 0x10;
    auto a = fmt(*scl::buildSwing(in));
    CHECK(a[1] == "SWING_MISSED");
    REQUIRE(a.size() == 9);
    CHECK(a[8] == "MISS");

    in = {}; in.attacker = kOrc; in.victim = kMe; in.victimState = 2;
    CHECK(fmt(*scl::buildSwing(in))[8] == "DODGE");
    in.victimState = 3;
    CHECK(fmt(*scl::buildSwing(in))[8] == "PARRY");
    in.victimState = 8;
    CHECK(fmt(*scl::buildSwing(in))[8] == "DEFLECT");
    in.victimState = 5; in.blocked = 17;
    a = fmt(*scl::buildSwing(in));
    REQUIRE(a.size() == 10);
    CHECK(a[8] == "BLOCK");
    CHECK(a[9] == "17");
    in = {}; in.attacker = kOrc; in.victim = kMe; in.hitInfo = 0x20; in.absorbed = 30;
    a = fmt(*scl::buildSwing(in));
    CHECK(a[8] == "ABSORB");
    CHECK(a[9] == "30");
    in.victimState = 4;  // an interrupt says nothing
    in.hitInfo = 0;
    CHECK_FALSE(scl::buildSwing(in));
}

TEST_CASE("spell damage: prefix, then periodic or ranged as the spell says", "[stockcombatlog]") {
    scl::SpellDamageInput in;
    in.caster = kMe; in.victim = kOrc; in.spellId = 133;
    in.damage = 300; in.school = 4; in.resisted = 20; in.crit = true;
    auto r = scl::buildSpellDamage(in, spell());
    auto a = fmt(*r);
    REQUIRE(a.size() == 20);
    CHECK(a[1] == "SPELL_DAMAGE");
    CHECK(a[8] == "133");
    CHECK(a[9] == "Fireball");
    CHECK(a[10] == "4");
    CHECK(a[11] == "300");
    CHECK(a[14] == "20");    // resisted
    CHECK(a[17] == "1");     // critical
    in.periodic = true;
    CHECK(fmt(*scl::buildSpellDamage(in, spell()))[1] == "SPELL_PERIODIC_DAMAGE");
    in.periodic = false;
    auto ranged = spell();
    ranged.attrEx3 = 0x8000;
    CHECK(fmt(*scl::buildSpellDamage(in, ranged))[1] == "RANGE_DAMAGE");
    // A spell the log hides (SPELL_ATTR0 0x100) or does not know writes nothing.
    auto hidden = spell();
    hidden.attr = 0x100;
    CHECK_FALSE(scl::buildSpellDamage(in, hidden));
    CHECK_FALSE(scl::buildSpellDamage(in, scl::SpellRules{}));
}

TEST_CASE("a spell that did no damage is absorbed, blocked or resisted - or nothing", "[stockcombatlog]") {
    scl::SpellDamageInput in;
    in.caster = kOrc; in.victim = kMe; in.spellId = 133;
    in.absorbed = 50; in.resisted = 9;
    auto a = fmt(*scl::buildSpellDamage(in, spell()));
    CHECK(a[1] == "SPELL_MISSED");
    REQUIRE(a.size() == 13);
    CHECK(a[11] == "ABSORB");
    CHECK(a[12] == "50");
    in.absorbed = 0;
    a = fmt(*scl::buildSpellDamage(in, spell()));
    CHECK(a[11] == "RESIST");
    CHECK(a[12] == "9");
    in.resisted = 0;
    CHECK_FALSE(scl::buildSpellDamage(in, spell()));
    in.periodic = true; in.absorbed = 5;
    CHECK(fmt(*scl::buildSpellDamage(in, spell()))[1] == "SPELL_PERIODIC_MISSED");
}

TEST_CASE("a miss with no amount: the three that carry one carry it anyway", "[stockcombatlog]") {
    auto a = fmt(*scl::buildSpellMiss(kMe, kOrc, 133, 7, spell()));
    REQUIRE(a.size() == 12);
    CHECK(a[11] == "IMMUNE");
    a = fmt(*scl::buildSpellMiss(kMe, kOrc, 133, 2, spell()));
    REQUIRE(a.size() == 13);
    CHECK(a[11] == "RESIST");
    CHECK(a[12] == "0");
    CHECK_FALSE(scl::buildSpellMiss(kMe, kOrc, 133, 0, spell()));
}

TEST_CASE("heals carry overhealing and absorbed as numbers, critical as 1 or nil", "[stockcombatlog]") {
    auto a = fmt(*scl::buildHeal(kMe, kMe, 139, 100, 40, 0, false, true, spell()));
    REQUIRE(a.size() == 15);
    CHECK(a[1] == "SPELL_PERIODIC_HEAL");
    CHECK(a[11] == "100");
    CHECK(a[12] == "40");
    CHECK(a[13] == "0");
    CHECK(a[14] == kNil);
    CHECK_FALSE(scl::buildHeal(kMe, kMe, 139, 0, 0, 0, false, false, spell()));
    auto periodicByAttr = spell();
    periodicByAttr.attrEx3 = 0x2000000;  // SPELL_ATTR3_TREAT_AS_PERIODIC
    CHECK(fmt(*scl::buildHeal(kMe, kMe, 139, 5, 0, 0, true, false, periodicByAttr))[1] ==
          "SPELL_PERIODIC_HEAL");
}

TEST_CASE("energize shows rage in whole points; drain becomes leech with a gain", "[stockcombatlog]") {
    auto a = fmt(*scl::buildEnergize(kMe, kMe, 133, 1, 150, false, spell()));
    CHECK(a[1] == "SPELL_ENERGIZE");
    CHECK(a[11] == "15");
    CHECK(a[12] == "1");
    CHECK_FALSE(scl::buildEnergize(kMe, kMe, 133, 0, 0, false, spell()));
    a = fmt(*scl::buildDrain(kMe, kOrc, 133, 0, 200, 0.0f, false, spell()));
    CHECK(a[1] == "SPELL_DRAIN");
    REQUIRE(a.size() == 14);
    CHECK(a[13] == kNil);
    a = fmt(*scl::buildDrain(kMe, kOrc, 133, 0, 200, 0.5f, true, spell()));
    CHECK(a[1] == "SPELL_PERIODIC_LEECH");
    CHECK(a[13] == "100");
    CHECK_FALSE(scl::buildDrain(kMe, kOrc, 133, 4, 200, 1.0f, false, spell()));
}

TEST_CASE("environmental damage: no source, the upper-case type, the school it hurts with", "[stockcombatlog]") {
    auto r = scl::buildEnvironmental(kMe, 3, 120, 0, 10);
    auto a = fmt(*r);
    REQUIRE(a.size() == 18);
    CHECK(a[1] == "ENVIRONMENTAL_DAMAGE");
    CHECK(a[2] == "0x0000000000000000");
    CHECK(a[8] == "LAVA");
    CHECK(a[9] == "120");
    CHECK(a[10] == "0");
    CHECK(a[11] == "4");   // fire
    CHECK(a[12] == "10");  // resisted
    CHECK_FALSE(scl::buildEnvironmental(kMe, 2, 0, 0, 0));
}

TEST_CASE("dispels and interrupts name the second spell and the aura kind", "[stockcombatlog]") {
    auto a = fmt(*scl::buildDispel(false, kMe, kOrc, 2139, 133, true, spell(), spell()));
    REQUIRE(a.size() == 15);
    CHECK(a[1] == "SPELL_DISPEL");
    CHECK(a[11] == "133");
    CHECK(a[12] == "Fireball");
    CHECK(a[13] == "4");
    CHECK(a[14] == "DEBUFF");
    CHECK(fmt(*scl::buildDispel(true, kMe, kOrc, 2139, 133, false, spell(), spell()))[1] == "SPELL_STOLEN");
    a = fmt(*scl::buildInterrupt(kMe, kOrc, 2139, 133, spell(), spell()));
    REQUIRE(a.size() == 14);
    CHECK(a[1] == "SPELL_INTERRUPT");
    CHECK(a[12] == "Fireball");
    auto hideCaster = spell();
    hideCaster.attrEx4 = 0x2000;
    auto broken = *scl::buildAuraBroken(kMe, kOrc, 133, 2139, true, spell(), hideCaster);
    CHECK(broken.event == scl::SPELL_AURA_BROKEN_SPELL);
    CHECK(broken.src == 0);
}

TEST_CASE("aura events: removal only from the living, passives never, doses only for stacking spells", "[stockcombatlog]") {
    CHECK(fmt(*scl::buildAuraChange(true, kMe, kOrc, 139, false, false, spell()))[11] == "BUFF");
    CHECK_FALSE(scl::buildAuraChange(false, kMe, kOrc, 139, false, false, spell()));
    CHECK(scl::buildAuraChange(false, kMe, kOrc, 139, false, true, spell())->event == scl::SPELL_AURA_REMOVED);
    auto passive = spell();
    passive.attr = 0x40;
    CHECK_FALSE(scl::buildAuraChange(true, kMe, kOrc, 139, false, true, passive));
    auto noLog = spell();
    noLog.attrEx6 = 0x400;
    CHECK_FALSE(scl::buildAuraChange(true, kMe, kOrc, 139, false, true, noLog));
    CHECK_FALSE(scl::buildAuraDose(kMe, kOrc, 139, 1, 2, false, spell()));
    auto stacks = spell();
    stacks.stackAmount = 5;
    auto a = fmt(*scl::buildAuraDose(kMe, kOrc, 139, 1, 2, true, stacks));
    CHECK(a[1] == "SPELL_AURA_APPLIED_DOSE");
    CHECK(a[11] == "DEBUFF");
    CHECK(a[12] == "2");
    CHECK(scl::buildAuraDose(kMe, kOrc, 139, 3, 2, true, stacks)->event == scl::SPELL_AURA_REMOVED_DOSE);
}

TEST_CASE("cast events: a cast time starts, an instant succeeds, a triggered cast is neither", "[stockcombatlog]") {
    auto withCast = spell();
    withCast.castTime = true;
    CHECK(scl::logsCastStart(0, withCast));
    CHECK_FALSE(scl::logsCastSuccess(0, withCast));
    CHECK_FALSE(scl::logsCastStart(0, spell()));
    CHECK(scl::logsCastSuccess(0, spell()));
    CHECK_FALSE(scl::logsCastSuccess(0x1, spell()));
    auto attr7 = spell();
    attr7.attrEx7 = 0x80000000u;
    CHECK(scl::logsCastSuccess(0x1, attr7));
    auto learn = spell();
    learn.effects[1] = 36;
    CHECK_FALSE(scl::logsCastSuccess(0, learn));
    auto lock = spell();
    lock.effects[0] = 33;
    CHECK_FALSE(scl::logsCastSuccess(0, lock));
}

TEST_CASE("unit flags as 0x74DCB0 works them out", "[stockcombatlog]") {
    scl::UnitFacts me;
    me.guid = kMe; me.me = kMe; me.controller = kMe; me.objectKnown = me.isUnit = true; me.reaction = 4;
    CHECK(scl::unitFlags(me) == 0x511);

    scl::UnitFacts pet = me;
    pet.guid = 0xF140000123000456ULL;
    pet.charmedOrSummoned = true;
    CHECK(scl::unitFlags(pet) == (0x1u | 0x10u | 0x100u | 0x1000u));  // mine, a pet (charmed or summoned)
    pet.charmedOrSummoned = false;
    CHECK((scl::unitFlags(pet) & 0xFC00u) == 0x2000u);                  // created only: a guardian

    scl::UnitFacts orc;
    orc.guid = kOrc; orc.me = kMe; orc.controller = kOrc; orc.objectKnown = orc.isUnit = true;
    orc.reaction = 1;  // unfriendly
    CHECK(scl::unitFlags(orc) == 0xA48);
    orc.reaction = 3;  // neutral
    CHECK(scl::unitFlags(orc) == 0xA28);
    orc.reaction = 4;  // friendly
    CHECK(scl::unitFlags(orc) == 0xA18);
    orc.isTarget = true;
    CHECK(scl::unitFlags(orc) == (0xA18u | 0x10000u));
    orc.raidIcon = 7;  // skull - only in a group
    CHECK((scl::unitFlags(orc) & 0x8000000u) == 0);
    orc.inGroup = true;
    CHECK((scl::unitFlags(orc) & 0x8000000u) != 0);

    scl::UnitFacts mate;
    mate.guid = 0x22; mate.me = kMe; mate.controller = 0x22; mate.objectKnown = mate.isUnit = true;
    mate.reaction = 4; mate.inGroup = true; mate.controllerInParty = true; mate.roleFlags = 2;
    CHECK(scl::unitFlags(mate) == (0x512u | 0x40000u));   // party, friendly, player; main tank

    scl::UnitFacts none;
    CHECK(scl::unitFlags(none) == 0x80000000u);
    scl::UnitFacts go;
    go.guid = 0xF110000123000001ULL; go.me = kMe; go.objectKnown = true; go.controller = 0;
    CHECK(scl::unitFlags(go) == (0x4000u | 0x200u | 0x8u | 0x20u));
}

TEST_CASE("CombatLog_Object_IsA: a special bit is enough, otherwise all four categories", "[stockcombatlog]") {
    CHECK(scl::objectIsA(0x511, 0x511));                // COMBATLOG_FILTER_ME
    CHECK_FALSE(scl::objectIsA(0x511, 0x0F));           // affiliation alone is not a match
    CHECK(scl::objectIsA(0xA48, 0xFFFF));               // everything in the four
    CHECK(scl::objectIsA(0x80000000u, 0xFFFFFFFFu));    // a missing unit, through its special bit
    CHECK_FALSE(scl::objectIsA(0x80000000u, 0xFFFF));
    CHECK(scl::objectIsA(0xA48 | 0x10000u, 0x10000u));  // the target
    CHECK_FALSE(scl::maskComplete(0x0F));
    CHECK(scl::maskComplete(0x10000u));
}

TEST_CASE("filters: events by name, units by mask or guid, a spell by id or name", "[stockcombatlog]") {
    scl::EntryKey k;
    k.event = scl::SPELL_DAMAGE; k.src = kMe; k.srcFlags = 0x511; k.dst = kOrc; k.dstFlags = 0xA48;
    k.spellId = 133; k.spellName = "Fireball";
    scl::Filter f;
    scl::setFilterEvents(f, "SPELL_DAMAGE,SWING_DAMAGE");
    f.srcMask = 0x511;
    CHECK(scl::filterMatches(f, k));
    scl::setFilterEvents(f, "SWING_DAMAGE SPELL_HEAL");
    CHECK_FALSE(scl::filterMatches(f, k));
    scl::setFilterEvents(f, "");
    CHECK_FALSE(scl::filterMatches(f, k));
    scl::setFilterEvents(f, nullptr);
    f.srcMask = 0xFFFFFFFFu;
    f.dstGuid = scl::parseGuid("0xF130000BB8000123");
    CHECK(scl::filterMatches(f, k));
    f.dstGuid = 1;
    CHECK_FALSE(scl::filterMatches(f, k));
    f.dstGuid = 0;
    f.spellName = "Frostbolt";
    CHECK_FALSE(scl::filterMatches(f, k));
    f.spellName.clear();
    f.spellId = 133;
    CHECK(scl::filterMatches(f, k));
}

TEST_CASE("the history: filtered walks, newest first by zero, one expired entry let go per add", "[stockcombatlog]") {
    auto args = [](const char* event, uint32_t srcFlags, const char* tag) {
        return std::vector<std::string>{std::string("\x02") + "1.0", event, "0x0000000000000011", tag,
                                        std::to_string(srcFlags), "0x0000000000000000", "\x01",
                                        "2147483648"};
    };
    scl::History h;
    h.add(args("PARTY_KILL", 0x511, "a"), 1000);
    h.add(args("PARTY_KILL", 0xA48, "b"), 2000);
    h.add(args("PARTY_KILL", 0x511, "c"), 3000);
    CHECK(h.numEntries(false) == 3);
    scl::Filter mine;
    mine.srcMask = 0x511;
    h.addFilter(mine);
    CHECK(h.numEntries(false) == 2);
    CHECK(h.numEntries(true) == 3);
    REQUIRE(h.setCurrent(0, false));
    CHECK(h.current()->args[3] == "c");
    REQUIRE(h.advance(-1, false));
    CHECK(h.current()->args[3] == "a");      // "b" is filtered out
    CHECK_FALSE(h.advance(-1, false));       // off the end clears the cursor
    CHECK(h.current() == nullptr);
    REQUIRE(h.setCurrent(1, true));
    CHECK(h.current()->args[3] == "a");
    REQUIRE(h.advance(1, true));
    CHECK(h.current()->args[3] == "b");
    CHECK(h.setCurrent(-1, false));
    CHECK(h.current()->args[3] == "a");
    CHECK_FALSE(h.setCurrent(-2, false));
    h.resetFilters();
    // Retention: 300 s. One expired entry goes per new one.
    h.add(args("UNIT_DIED", 0x511, "d"), 302000);
    CHECK(h.size() == 3);                    // "a" (1 s) went; "b" (2 s) is also old and stays
    h.add(args("UNIT_DIED", 0x511, "e"), 302001);
    CHECK(h.size() == 3);
    h.clear();
    CHECK(h.numEntries(true) == 0);
}
