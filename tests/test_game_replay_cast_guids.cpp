// The caster guid of SMSG_SPELL_FAILURE, SMSG_SPELL_FAILED_OTHER and
// SMSG_SPELL_DELAYED through GameHandler's own dispatch: packed on TBC and
// WotLK, as cmangos-tbc and OregonCore write it, and whole on classic. See
// game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <string>
#include <utility>
#include <vector>

using replay::Session;

namespace {

using wowee::game::Opcode;
using wowee::game::wireOpcode;
using wowee::network::Packet;

constexpr uint64_t kMob = 0xF130000000000123ull;

// The names of the events the session fired whose first argument is "target".
struct TargetEvents {
    std::vector<std::string> names;

    explicit TargetEvents(Session& s) {
        s.handler().setTargetGuidRaw(kMob);
        s.handler().setAddonEventCallback([this](const std::string& name, const std::vector<std::string>& args) {
            if (!args.empty() && args.front() == "target") names.push_back(name);
        });
    }
};

void writeCaster(Packet& p, bool classic) {
    if (classic) p.writeUInt64(kMob);
    else p.writePackedGuid(kMob);
}

}  // namespace

TEST_CASE("an interrupted cast names its caster as the server writes it", "[replay][castguid]") {
    for (const char* expansion : {"wotlk", "tbc", "classic"}) {
        CAPTURE(expansion);
        const std::string e = expansion;
        Session s(expansion);
        TargetEvents events(s);
        // Spell::SendInterrupted's SMSG_SPELL_FAILURE: WotLK adds the cast
        // count. Classic's whole guid is the layout this client has always
        // read; vmangos never sends the packet and cmangos-classic packs it.
        Packet p(wireOpcode(Opcode::SMSG_SPELL_FAILURE));
        writeCaster(p, e == "classic");
        if (e == "wotlk") p.writeUInt8(0);
        p.writeUInt32(133);
        p.writeUInt8(0);  // result
        s.receive(p);
        // 0x809C70 -> 0x7FECC0: the event by result (0 is a plain failure),
        // and no STOP for a cast the client was not showing.
        CHECK(events.names == std::vector<std::string>{"UNIT_SPELLCAST_FAILED"});
    }
}

TEST_CASE("the player's own interrupt reads as one in each expansion's numbering", "[replay][castguid]") {
    // SPELL_FAILED_INTERRUPTED: 40 on WotLK, 0x25 in cmangos-tbc's enum.
    for (const auto& [expansion, result] : {std::pair<const char*, uint8_t>{"wotlk", 40},
                                            std::pair<const char*, uint8_t>{"tbc", 0x25}}) {
        CAPTURE(expansion);
        Session s(expansion);
        std::vector<std::string> names;
        s.handler().setAddonEventCallback([&names](const std::string& name, const std::vector<std::string>& args) {
            if (!args.empty() && args.front() == "player") names.push_back(name);
        });
        const size_t chatBefore = s.handler().getChatHistory().size();
        Packet p(wireOpcode(Opcode::SMSG_SPELL_FAILURE));
        p.writePackedGuid(Session::kPlayerGuid);
        if (std::string(expansion) == "wotlk") p.writeUInt8(0);
        p.writeUInt32(133);
        p.writeUInt8(result);
        s.receive(p);
        // 0x809C70 says nothing (the text is SMSG_CAST_FAILED's) and fires
        // UNIT_SPELLCAST_INTERRUPTED - read as an interrupt in either numbering.
        CHECK(s.handler().getChatHistory().size() == chatBefore);
        CHECK(names == std::vector<std::string>{"UNIT_SPELLCAST_INTERRUPTED"});
    }
}

TEST_CASE("another unit's failed cast names its caster as the server writes it", "[replay][castguid]") {
    for (const char* expansion : {"wotlk", "tbc", "classic"}) {
        CAPTURE(expansion);
        Session s(expansion);
        TargetEvents events(s);
        Packet p(wireOpcode(Opcode::SMSG_SPELL_FAILED_OTHER));
        writeCaster(p, std::string(expansion) == "classic");
        p.writeUInt32(133);
        s.receive(p);
        // The spell alone, as the older cores send it for an interrupt.
        CHECK(events.names == std::vector<std::string>{"UNIT_SPELLCAST_INTERRUPTED"});
    }
}

TEST_CASE("a pushed-back cast names its caster as the server writes it", "[replay][castguid]") {
    for (const char* expansion : {"wotlk", "tbc", "classic"}) {
        CAPTURE(expansion);
        Session s(expansion);
        TargetEvents events(s);
        // Spell::Delayed's SMSG_SPELL_DELAYED.
        Packet p(wireOpcode(Opcode::SMSG_SPELL_DELAYED));
        writeCaster(p, std::string(expansion) == "classic");
        p.writeUInt32(500);
        s.receive(p);
        CHECK(events.names == std::vector<std::string>{"UNIT_SPELLCAST_DELAYED"});
    }
}
