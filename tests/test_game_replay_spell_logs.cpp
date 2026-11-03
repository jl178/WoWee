// Dispel, spellsteal and instakill logs through GameHandler's own dispatch,
// built as the servers write them: AzerothCore for WotLK, cmangos for classic
// and TBC. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <algorithm>
#include <string>

using replay::Session;

namespace {

using wowee::game::Opcode;
using wowee::game::wireOpcode;
using wowee::network::Packet;

constexpr uint64_t kPlayer = Session::kPlayerGuid;
constexpr uint64_t kMob = 0xF130000000000123ull;
constexpr uint32_t kDispelMagic = 527;
constexpr uint32_t kPowerWordFortitude = 1243;

using wowee::game::CombatTextEntry;

// The entry a log left for the floating text and the combat log; the stock
// client writes none of these to chat.
const CombatTextEntry* findText(Session& s, CombatTextEntry::Type type) {
    const auto& texts = s.handler().getCombatText();
    const auto it = std::find_if(texts.begin(), texts.end(),
                                 [&](const auto& e) { return e.type == type; });
    return it == texts.end() ? nullptr : &*it;
}

bool chatEmpty(Session& s) { return s.handler().getChatHistory().empty(); }

// Spell::EffectDispel: victim, then caster. Classic has neither the dispel
// spell nor the byte after each dispelled one.
Packet dispelLog(bool classic, uint64_t victim, uint64_t caster) {
    Packet p(wireOpcode(Opcode::SMSG_SPELLDISPELLOG));
    p.writePackedGuid(victim);
    p.writePackedGuid(caster);
    if (!classic) {
        p.writeUInt32(kDispelMagic);
        p.writeUInt8(0);
    }
    p.writeUInt32(1);
    p.writeUInt32(kPowerWordFortitude);
    if (!classic) p.writeUInt8(0);
    return p;
}

// Spell::EffectInstaKill. Classic sends the victim alone.
Packet instaKillLog(bool classic, uint64_t caster, uint64_t victim) {
    Packet p(wireOpcode(Opcode::SMSG_SPELLINSTAKILLLOG));
    if (!classic) p.writeUInt64(caster);
    p.writeUInt64(victim);
    p.writeUInt32(5);
    return p;
}

Packet dispelFailed(bool classic, uint64_t caster, uint64_t victim) {
    Packet p(wireOpcode(Opcode::SMSG_DISPEL_FAILED));
    p.writeUInt64(caster);
    p.writeUInt64(victim);
    if (!classic) p.writeUInt32(kDispelMagic);
    p.writeUInt32(kPowerWordFortitude);
    return p;
}

}  // namespace

TEST_CASE("a dispel log names its victim before its caster", "[replay][dispel]") {
    for (const char* expansion : {"wotlk", "tbc", "classic"}) {
        CAPTURE(expansion);
        const bool classic = std::string(expansion) == "classic";

        {
            Session s(expansion);
            // On the target, where the panel shows spell mechanics by default.
            s.handler().setTargetGuidRaw(kPlayer);
            s.receive(dispelLog(classic, kPlayer, kMob));
            const CombatTextEntry* e = findText(s, CombatTextEntry::DISPEL);
            REQUIRE(e);
            CHECK(e->spellId == kPowerWordFortitude);
            CHECK(e->srcGuid == kMob);
            CHECK(e->dstGuid == kPlayer);
            CHECK_FALSE(e->isPlayerSource);
            CHECK(chatEmpty(s));
        }
        {
            Session s(expansion);
            s.handler().setTargetGuidRaw(kMob);
            s.receive(dispelLog(classic, kMob, kPlayer));
            const CombatTextEntry* e = findText(s, CombatTextEntry::DISPEL);
            REQUIRE(e);
            CHECK(e->spellId == kPowerWordFortitude);
            CHECK(e->srcGuid == kPlayer);
            CHECK(e->isPlayerSource);
            CHECK(chatEmpty(s));
        }
    }
}

TEST_CASE("a spellsteal log has packed guids on TBC", "[replay][dispel]") {
    Session s("tbc");
    s.handler().setTargetGuidRaw(kMob);
    Packet p(wireOpcode(Opcode::SMSG_SPELLSTEALLOG));
    p.writePackedGuid(kMob);
    p.writePackedGuid(kPlayer);
    p.writeUInt32(30449);
    p.writeUInt8(0);
    p.writeUInt32(1);
    p.writeUInt32(kPowerWordFortitude);
    p.writeUInt8(0);
    s.receive(p);
    const CombatTextEntry* e = findText(s, CombatTextEntry::STEAL);
    REQUIRE(e);
    CHECK(e->spellId == kPowerWordFortitude);
    CHECK(e->srcGuid == kPlayer);
    CHECK(e->dstGuid == kMob);
    CHECK(chatEmpty(s));
}

TEST_CASE("an instakill log has whole guids", "[replay][instakill]") {
    for (const char* expansion : {"wotlk", "tbc", "classic"}) {
        CAPTURE(expansion);
        Session s(expansion);
        s.receive(instaKillLog(std::string(expansion) == "classic", kMob, kPlayer));
        const CombatTextEntry* e = findText(s, CombatTextEntry::INSTAKILL);
        REQUIRE(e);
        CHECK(e->spellId == 5);
        CHECK(e->dstGuid == kPlayer);
        CHECK_FALSE(e->isPlayerSource);
        CHECK(chatEmpty(s));
    }
}

TEST_CASE("a failed dispel has whole guids, and classic has no dispel spell", "[replay][dispel]") {
    SECTION("wotlk") {
        Session s;
        s.receive(dispelFailed(false, kPlayer, kMob));
        CHECK(chatEmpty(s));
    }
    SECTION("classic") {
        Session s("classic");
        s.receive(dispelFailed(true, kPlayer, kMob));
        CHECK(chatEmpty(s));
    }
}
