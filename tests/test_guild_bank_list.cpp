// SMSG_GUILD_BANK_LIST, whose tab-name block is optional.
//
// AzerothCore's GuildBankQueryResults::Write puts the tab names on the wire
// only for `!Tab && FullUpdate` (Guild::_SendBankList fills them under
// `sendAllSlots && !tabId`, as TrinityCore's 3.3.5 SendBankList did). A full
// list of any other tab - what clicking a tab asks for - has none, and reading
// names out of it took its item count for a tab count.
//
// The shapes are pinned here because nothing about a mis-parse raises - the
// bytes are all valid, they are just read a block early or late.
#include <catch_amalgamated.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/application.hpp"
#include "game/world_packets.hpp"

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}

using namespace wowee::game;

namespace {

struct Writer {
    std::vector<uint8_t> bytes;
    void u8(uint8_t v) { bytes.push_back(v); }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    void cstr(const std::string& s) {
        bytes.insert(bytes.end(), s.begin(), s.end());
        bytes.push_back(0);
    }
    /// One occupied slot, in the order the reader expects it.
    void item(uint8_t slotId, uint32_t entry, uint32_t stack) {
        u8(slotId);
        u32(entry);
        u32(0);        // flags
        u32(0);        // randomPropertyId - zero, so no seed follows
        u32(stack);
        u32(0);        // permanent enchantment
        u8(0);         // charges
        u8(0);         // sockets
    }
};

/// money, tabId, remaining withdrawals, the full-update flag, then the tab
/// block - which the server writes only for a full list of tab 0.
Writer header(uint8_t tabId, bool fullUpdate,
              const std::vector<std::string>& tabNames) {
    Writer w;
    w.u64(123456);
    w.u8(tabId);
    w.u32(7);                    // withdrawals remaining
    w.u8(fullUpdate ? 1 : 0);
    if (fullUpdate && tabId == 0) {
        w.u8(static_cast<uint8_t>(tabNames.size()));
        for (const std::string& name : tabNames) {
            w.cstr(name);
            w.cstr("Interface\\Icons\\INV_Misc_Bag_08");
        }
    }
    return w;
}

GuildBankData parse(Writer& w) {
    wowee::network::Packet packet(0, std::move(w.bytes));
    GuildBankData data;
    REQUIRE(GuildBankListParser::parse(packet, data));
    return data;
}

}  // namespace

TEST_CASE("the opening list carries the tabs and the first tab's items") {
    Writer w = header(0, true, {"Tab One", "Tab Two"});
    w.u8(2);
    w.item(0, 6948, 1);
    w.item(3, 4306, 20);

    const GuildBankData data = parse(w);
    CHECK(data.money == 123456u);
    CHECK(data.tabId == 0);
    CHECK(data.withdrawalsRemaining == 7);
    CHECK(data.tabs[0].withdrawalsRemaining == 7);
    REQUIRE(data.tabs[0].items.size() == 2);   // the tab's own contents
    CHECK(data.tabsIncluded);
    REQUIRE(data.tabs.size() == 2);
    CHECK(data.tabs[0].tabName == "Tab One");
    CHECK(data.tabs[1].tabName == "Tab Two");
    REQUIRE(data.tabItems.size() == 2);
    CHECK(data.tabItems[0].slotId == 0);
    CHECK(data.tabItems[0].itemEntry == 6948u);
    CHECK(data.tabItems[1].slotId == 3);
    CHECK(data.tabItems[1].itemEntry == 4306u);
    CHECK(data.tabItems[1].stackCount == 20u);
}

TEST_CASE("a full list of a later tab carries no names") {
    // Clicking tab 3 asks for a full list of it. Reading names out of that
    // took the item count for a tab count and the first item for a name.
    Writer first = header(0, true, {"Tab One", "Tab Two", "Tab Three"});
    first.u8(0);
    wowee::network::Packet firstPacket(0, std::move(first.bytes));
    GuildBankData data;
    REQUIRE(GuildBankListParser::parse(firstPacket, data));

    Writer w = header(2, true, {});
    w.u8(1);
    w.item(5, 2589, 12);
    wowee::network::Packet packet(0, std::move(w.bytes));
    REQUIRE(GuildBankListParser::parse(packet, data));

    CHECK(data.tabId == 2);
    CHECK_FALSE(data.tabsIncluded);
    REQUIRE(data.tabs.size() == 3);        // the names from tab 0's list
    CHECK(data.tabs[2].tabName == "Tab Three");
    REQUIRE(data.tabItems.size() == 1);
    CHECK(data.tabItems[0].slotId == 5);
    CHECK(data.tabItems[0].itemEntry == 2589u);
    CHECK(data.tabItems[0].stackCount == 12u);
    REQUIRE(data.tabs[2].items.size() == 1);
    CHECK(data.tabs[2].items[0].itemEntry == 2589u);
}

TEST_CASE("a partial list changes only the slots it names") {
    // _SendBankContentUpdate sends the slots a move touched, empties
    // included, to everyone at the vault. It used to wipe the tab down to
    // those slots.
    Writer first = header(0, true, {"Tab One"});
    first.u8(3);
    first.item(0, 6948, 1);
    first.item(1, 4306, 20);
    first.item(2, 858, 5);
    wowee::network::Packet firstPacket(0, std::move(first.bytes));
    GuildBankData data;
    REQUIRE(GuildBankListParser::parse(firstPacket, data));

    Writer update = header(0, false, {});
    update.u8(2);
    update.u8(1);
    update.u32(0);              // slot 1 emptied
    update.item(7, 4306, 20);   // and its stack landed in slot 7
    wowee::network::Packet updatePacket(0, std::move(update.bytes));
    REQUIRE(GuildBankListParser::parse(updatePacket, data));

    REQUIRE(data.tabs.size() == 1);        // a partial list has no names
    const auto& items = data.tabs[0].items;
    REQUIRE(items.size() == 3);
    auto at = [&](uint8_t slot) -> uint32_t {
        for (const auto& it : items) if (it.slotId == slot) return it.itemEntry;
        return 0;
    };
    CHECK(at(0) == 6948u);
    CHECK(at(1) == 0u);
    CHECK(at(2) == 858u);
    CHECK(at(7) == 4306u);
}

TEST_CASE("a move within the bank is the BankOnly shape") {
    // GuildBankSwapItems::Read: BankOnly, then dest tab/slot/item, src
    // tab/slot/item, autoStore and the count.
    const auto p = GuildBankSwapItemsPacket::buildBankToBank(
        0x0102030405060708ull, 1, 9, 0, 0, 4, 4306, 5);
    const auto& b = p.getData();
    REQUIRE(b.size() == 8 + 1 + 1 + 1 + 4 + 1 + 1 + 4 + 1 + 4);
    CHECK(b[8] == 1);     // BankOnly
    CHECK(b[9] == 1);     // dest tab
    CHECK(b[10] == 9);    // dest slot
    CHECK(b[15] == 0);    // src tab
    CHECK(b[16] == 4);    // src slot
    CHECK((b[17] | (b[18] << 8)) == 4306);
    CHECK(b[21] == 0);    // autoStore
    CHECK(b[22] == 5);    // count
}

TEST_CASE("a slots-only update leaves the tabs it found") {
    Writer first = header(0, true, {"Tab One", "Tab Two"});
    first.u8(0);
    wowee::network::Packet firstPacket(0, std::move(first.bytes));
    GuildBankData data;
    REQUIRE(GuildBankListParser::parse(firstPacket, data));
    REQUIRE(data.tabs.size() == 2);

    Writer second = header(1, false, {});
    second.u8(1);
    second.item(9, 1234, 3);
    wowee::network::Packet secondPacket(0, std::move(second.bytes));
    REQUIRE(GuildBankListParser::parse(secondPacket, data));

    CHECK_FALSE(data.tabsIncluded);
    CHECK(data.tabs.size() == 2);          // kept, not cleared
    CHECK(data.tabId == 1);
    REQUIRE(data.tabItems.size() == 1);
    CHECK(data.tabItems[0].itemEntry == 1234u);
}

TEST_CASE("an empty slot carries nothing after its entry") {
    Writer w = header(0, true, {"Only Tab"});
    w.u8(2);
    w.u8(4);
    w.u32(0);          // empty: no flags, no stack, no enchant
    w.item(6, 858, 5);

    const GuildBankData data = parse(w);
    REQUIRE(data.tabItems.size() == 2);
    CHECK(data.tabItems[0].itemEntry == 0u);
    CHECK(data.tabItems[1].slotId == 6);
    CHECK(data.tabItems[1].itemEntry == 858u);
    CHECK(data.tabItems[1].stackCount == 5u);
}
