// The fields SMSG_SPELL_GO carries after its targets (Wow.exe 0x80E1B0):
// power, runes, the missile adjustment, then the ammo a shot flies.
#include <catch_amalgamated.hpp>

#include <cstdint>

#include "core/application.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}  // namespace wowee

using wowee::game::SpellGoData;
using wowee::game::readSpellGoTail;
using wowee::network::Packet;

TEST_CASE("the ammo follows power, runes and the missile adjustment", "[spell_go]") {
    Packet p(0);
    p.writeUInt32(4321);   // 0x800 power left
    p.writeUInt8(0x3);     // runes before: two ready
    p.writeUInt8(0x1);     // after: rune 1 used
    p.writeUInt8(200);     // its cooldown byte
    p.writeFloat(0.25f);   // 0x20000 elevation
    p.writeUInt32(150);    //         delay
    p.writeUInt32(5996);   // 0x20 ammo display
    p.writeUInt32(24);     //      inventory type
    SpellGoData d;
    REQUIRE(readSpellGoTail(p, 0x800 | 0x200000 | 0x20000 | 0x20, d));
    CHECK(d.ammoDisplayId == 5996);
    CHECK(d.ammoInventoryType == 24);
}

TEST_CASE("a wand shot names no ammo display, only the wand's type", "[spell_go]") {
    Packet p(0);
    p.writeUInt32(100);
    p.writeUInt32(0);
    p.writeUInt32(26);
    SpellGoData d;
    REQUIRE(readSpellGoTail(p, 0x800 | 0x20, d));
    CHECK(d.ammoDisplayId == 0);
    CHECK(d.ammoInventoryType == 26);
}

TEST_CASE("a short tail reads no ammo", "[spell_go]") {
    Packet p(0);
    p.writeUInt32(5998);
    SpellGoData d;
    CHECK_FALSE(readSpellGoTail(p, 0x20, d));
    CHECK(d.ammoDisplayId == 0);
}
