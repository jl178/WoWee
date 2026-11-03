// CMSG_SPLIT_ITEM and CMSG_AUTOSTORE_BAG_ITEM, as AzerothCore reads them.
//
// SplitItem::Read takes four uint8 positions and a uint32 count in 3.3.5;
// classic and TBC servers read the count as one byte. Sent narrow to a WotLK
// server the read ran three bytes short and the split never happened.
// AutoStoreBagItem::Read is three uint8s in every expansion.
#include <catch_amalgamated.hpp>

#include <cstdint>
#include <vector>

#include "core/application.hpp"
#include "game/world_packets.hpp"

// Same stub the other packet-layout tests carry: the builders are linked from
// translation units that reach the Application singleton; the overloads under
// test never call it.
namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}  // namespace wowee

using wowee::game::AutoStoreBagItemPacket;
using wowee::game::SplitItemPacket;

TEST_CASE("CMSG_SPLIT_ITEM count is a uint32 from WotLK on", "[split_item]") {
    auto p = SplitItemPacket::build(0xFF, 23, 19, 4, 300, /*narrowCount=*/false);
    const std::vector<uint8_t>& b = p.getData();
    REQUIRE(b.size() == 8);
    CHECK(b[0] == 0xFF);
    CHECK(b[1] == 23);
    CHECK(b[2] == 19);
    CHECK(b[3] == 4);
    const uint32_t count = b[4] | (b[5] << 8) | (b[6] << 16) | (uint32_t(b[7]) << 24);
    CHECK(count == 300);
}

TEST_CASE("CMSG_SPLIT_ITEM count stays one byte before WotLK", "[split_item]") {
    auto p = SplitItemPacket::build(0xFF, 23, 0xFF, 24, 7, /*narrowCount=*/true);
    const std::vector<uint8_t>& b = p.getData();
    REQUIRE(b.size() == 5);
    CHECK(b[4] == 7);
}

TEST_CASE("CMSG_AUTOSTORE_BAG_ITEM is source bag, source slot, destination bag",
          "[autostore]") {
    auto p = AutoStoreBagItemPacket::build(0xFF, 30, 20);
    const std::vector<uint8_t>& b = p.getData();
    REQUIRE(b.size() == 3);
    CHECK(b[0] == 0xFF);
    CHECK(b[1] == 30);
    CHECK(b[2] == 20);
}
