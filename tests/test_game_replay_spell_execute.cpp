// SMSG_SPELLLOGEXECUTE through GameHandler's own dispatch, built as
// AzerothCore's Spell::SendLogExecute (WotLK) and cmangos's SpellLog (classic
// and TBC) write it: the caster packed everywhere, a target packed on TBC and
// WotLK and whole on classic. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <algorithm>
#include <string>

using replay::Session;

namespace {

using wowee::game::CombatTextEntry;
using wowee::game::Opcode;
using wowee::game::wireOpcode;
using wowee::network::Packet;

constexpr uint64_t kPlayer = Session::kPlayerGuid;
constexpr uint64_t kMob = 0xF130000000000123ull;

// The servers' SpellEffects numbers.
constexpr uint32_t kPowerDrain = 8;
constexpr uint32_t kPowerBurn = 62;
constexpr uint32_t kInterruptCast = 68;
constexpr uint32_t kFeedPet = 101;

Packet executeLog(uint64_t caster, uint32_t spellId, uint32_t effect) {
    Packet p(wireOpcode(Opcode::SMSG_SPELLLOGEXECUTE));
    p.writePackedGuid(caster);
    p.writeUInt32(spellId);
    p.writeUInt32(1);
    p.writeUInt32(effect);
    p.writeUInt32(1);
    return p;
}

void writeTarget(Packet& p, bool classic, uint64_t guid) {
    if (classic) p.writeUInt64(guid);
    else p.writePackedGuid(guid);
}

// Spell::ExecuteLogEffectTakeTargetPower.
Packet takePower(bool classic, uint32_t effect) {
    Packet p = executeLog(kPlayer, 5138, effect);
    writeTarget(p, classic, kMob);
    p.writeUInt32(250);
    p.writeUInt32(0);
    p.writeFloat(effect == kPowerDrain ? 1.0f : 0.0f);
    return p;
}

bool hasText(Session& s, CombatTextEntry::Type type, uint32_t spellId) {
    const auto& texts = s.handler().getCombatText();
    return std::any_of(texts.begin(), texts.end(), [&](const CombatTextEntry& e) {
        return e.type == type && e.spellId == spellId;
    });
}

}  // namespace

TEST_CASE("an interrupt in the execute log is effect 68", "[replay][execute]") {
    for (const char* expansion : {"wotlk", "tbc", "classic"}) {
        CAPTURE(expansion);
        Session s(expansion);
        // On the target, where the panel shows spell mechanics by default.
        s.handler().setTargetGuidRaw(kMob);
        Packet p = executeLog(kPlayer, 2139, kInterruptCast);
        writeTarget(p, std::string(expansion) == "classic", kMob);
        p.writeUInt32(133);
        s.receive(p);
        CHECK(hasText(s, CombatTextEntry::INTERRUPT, 133));
    }
}

TEST_CASE("a drain is effect 8 and a burn is effect 62", "[replay][execute]") {
    for (const char* expansion : {"wotlk", "tbc", "classic"}) {
        for (uint32_t effect : {kPowerDrain, kPowerBurn}) {
            CAPTURE(expansion, effect);
            Session s(expansion);
            s.receive(takePower(std::string(expansion) == "classic", effect));
            const auto& texts = s.handler().getCombatText();
            const auto drain = std::find_if(texts.begin(), texts.end(), [](const auto& e) {
                return e.type == CombatTextEntry::POWER_DRAIN;
            });
            REQUIRE(drain != texts.end());
            CHECK(drain->spellId == 5138);
            CHECK(drain->amount == 250);
        }
    }
}

TEST_CASE("feeding a pet is effect 101, with a packed caster on TBC too", "[replay][execute]") {
    for (const char* expansion : {"wotlk", "tbc"}) {
        CAPTURE(expansion);
        Session s(expansion);
        Packet p = executeLog(kPlayer, 6991, kFeedPet);
        p.writeUInt32(4540);
        s.receive(p);
        // The combat log's alone in the stock client, never a chat line;
        // read whole, the packet leaves nothing behind.
        CHECK(s.handler().getChatHistory().empty());
    }
}
