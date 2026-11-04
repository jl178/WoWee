// SMSG_MESSAGECHAT's type-specific fields, as AzerothCore's
// ChatHandler::BuildChatPacket writes them and Wow.exe 0x50BE70 reads them.
#include <catch_amalgamated.hpp>

#include <cstdint>
#include <string>

#include "core/application.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}  // namespace wowee

using namespace wowee;
using game::ChatType;
using game::MessageChatData;
using game::MessageChatParser;

namespace {
void sized(network::Packet& p, const std::string& s) {
    p.writeUInt32(static_cast<uint32_t>(s.size() + 1));
    p.writeString(s);
}
network::Packet header(uint8_t type, uint32_t lang, uint64_t sender) {
    network::Packet p(0);
    p.writeUInt8(type);
    p.writeUInt32(lang);
    p.writeUInt64(sender);
    p.writeUInt32(0);
    return p;
}
void tail(network::Packet& p, const std::string& text, uint8_t tag) {
    sized(p, text);
    p.writeUInt8(tag);
}
}  // namespace

TEST_CASE("Say: receiver guid, text, tag", "[messagechat]") {
    auto p = header(0x01, 7, 0x42);
    p.writeUInt64(0x42);
    tail(p, "hello", 0x01);
    MessageChatData d;
    REQUIRE(MessageChatParser::parse(p, d));
    CHECK(d.type == ChatType::SAY);
    CHECK(d.message == "hello");
    CHECK(d.receiverGuid == 0x42);
    CHECK(d.chatTag == 0x01);
}

TEST_CASE("Channel: the name before the receiver", "[messagechat]") {
    auto p = header(0x11, 7, 0x42);
    p.writeString("General - Elwynn Forest");
    p.writeUInt64(0);
    tail(p, "hi all", 0);
    MessageChatData d;
    REQUIRE(MessageChatParser::parse(p, d));
    CHECK(d.channelName == "General - Elwynn Forest");
    CHECK(d.message == "hi all");
}

TEST_CASE("Monster say to a creature carries both names", "[messagechat]") {
    auto p = header(0x0C, 0, 0xF130000000000010ULL);
    sized(p, "Hogger");
    p.writeUInt64(0xF130000000000020ULL);
    sized(p, "Gnoll");
    tail(p, "%s growls.", 0);
    MessageChatData d;
    REQUIRE(MessageChatParser::parse(p, d));
    CHECK(d.senderName == "Hogger");
    CHECK(d.receiverName == "Gnoll");
    CHECK(d.message == "%s growls.");
}

TEST_CASE("Monster whisper to a player has no receiver name", "[messagechat]") {
    auto p = header(0x0F, 0, 0xF130000000000010ULL);
    sized(p, "Hogger");
    p.writeUInt64(0x42);
    tail(p, "psst", 0);
    MessageChatData d;
    REQUIRE(MessageChatParser::parse(p, d));
    CHECK(d.receiverGuid == 0x42);
    CHECK(d.receiverName.empty());
    CHECK(d.message == "psst");
}

TEST_CASE("Battleground lines keep their type and read the guid", "[messagechat]") {
    auto p = header(0x25, 0, 0);
    p.writeUInt64(0);
    tail(p, "The Alliance has taken the flag!", 0);
    MessageChatData d;
    REQUIRE(MessageChatParser::parse(p, d));
    CHECK(d.type == ChatType::BG_SYSTEM_ALLIANCE);
    CHECK(d.message == "The Alliance has taken the flag!");

    auto q = header(0x24, 0, 0);
    q.writeUInt64(0xF130000000000099ULL);
    sized(q, "Flag Carrier");
    tail(q, "$n picked it up", 0);
    MessageChatData e;
    REQUIRE(MessageChatParser::parse(q, e));
    CHECK(e.receiverName == "Flag Carrier");
    CHECK(e.message == "$n picked it up");
}

TEST_CASE("A foreign whisper names its sender and reads as a whisper", "[messagechat]") {
    auto p = header(0x08, 0, 0x42);
    sized(p, "Stranger");
    p.writeUInt64(0x43);
    tail(p, "hey", 0);
    MessageChatData d;
    REQUIRE(MessageChatParser::parse(p, d));
    CHECK(d.type == ChatType::WHISPER);
    CHECK(d.senderName == "Stranger");
    CHECK(d.message == "hey");
}

TEST_CASE("Achievement lines end with the achievement id", "[messagechat]") {
    auto p = header(0x30, 0, 0x42);
    p.writeUInt64(0x42);
    tail(p, "%s has earned the achievement $a!", 0);
    p.writeUInt32(6);
    MessageChatData d;
    REQUIRE(MessageChatParser::parse(p, d));
    CHECK(d.achievementId == 6);
}

TEST_CASE("A GM's line names the GM before the channel", "[messagechat]") {
    auto p = header(0x11, 7, 0x42);
    sized(p, "Gamemaster");
    p.writeString("World");
    p.writeUInt64(0);
    tail(p, "hello", 0x04);
    MessageChatData d;
    REQUIRE(MessageChatParser::parse(p, d, true));
    CHECK(d.senderName == "Gamemaster");
    CHECK(d.channelName == "World");
    CHECK(d.chatTag == 0x04);
}

TEST_CASE("The text stops at its terminator", "[messagechat]") {
    auto p = header(0x01, 7, 0x42);
    p.writeUInt64(0x42);
    p.writeUInt32(8);
    p.writeString("abc");   // 4 bytes
    p.writeUInt32(0);       // 4 more counted bytes
    p.writeUInt8(0);
    MessageChatData d;
    REQUIRE(MessageChatParser::parse(p, d));
    CHECK(d.message == "abc");
}
