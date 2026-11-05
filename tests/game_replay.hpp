#pragma once

// A GameHandler fed server packets the way the socket feeds it, with no server,
// window or renderer behind it.
//
// Most of what goes wrong between this client and a realm is a byte read from
// the wrong place: a flag tested against the wrong bit, a field index from the
// wrong expansion, a guid read packed where the server wrote it whole. None of
// that shows in a unit test of a helper, because the helper is right and the
// call site is not. Here the packet goes in through the same dispatch the
// realm's would, with one expansion active and its own opcode and update field
// tables and packet parsers, and the test reads back what the client concluded.
//
// The packets are built by hand, field by field as the server writes them;
// each test says which server function it copies.
//
// The socket is never connected; it only keeps what the client sends, for a
// test to read back.

#include <catch_amalgamated.hpp>

#include "core/application.hpp"
#include "game/expansion_profile.hpp"
#include "game/game_handler.hpp"
#include "game/game_services.hpp"
#include "game/opcode_table.hpp"
#include "game/packet_parsers.hpp"
#include "game/update_field_table.hpp"
#include "network/packet.hpp"
#include "network/world_socket.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wowee {
namespace game {

struct GameHandlerTestAccess {
    // Queued and drained the way update() drains the socket's packets, frame
    // after frame, since update object blocks are applied after their packet
    // rather than inside it. Bounded, so work that never drains fails here
    // instead of hanging the run or leaving the test half-applied state.
    static void receive(GameHandler& gh, const network::Packet& packet) {
        gh.enqueueIncomingPacket(packet);
        auto pending = [&] {
            return !gh.pendingIncomingPackets_.empty() ||
                   gh.entityController_->hasPendingUpdateObjectWork();
        };
        for (int frame = 0; frame < 1000 && pending(); ++frame) gh.processQueuedIncomingPackets();
        REQUIRE_FALSE(pending());
    }
    static void enterWorld(GameHandler& gh) { gh.setState(WorldState::IN_WORLD); }
    static void setSocket(GameHandler& gh, std::unique_ptr<network::WorldSocket> socket) {
        gh.socket = std::move(socket);
    }
    static void setExpansions(core::Application& app, std::unique_ptr<ExpansionRegistry> registry) {
        app.expansionRegistry_ = std::move(registry);
    }
};

}  // namespace game
}  // namespace wowee

namespace replay {

using wowee::game::Opcode;
using wowee::game::UF;

// SMSG_UPDATE_OBJECT, one block per call, in WotLK's layout. Fields are keyed by name and
// resolved through the loaded update field table, so a test states which field
// it sets rather than which index that is this expansion.
class UpdateObject {
public:
    using Fields = std::map<UF, uint32_t>;

    // An object appearing, laid out as Object::BuildCreateUpdateBlockForPlayer
    // writes it, but with the movement part cut down to a stationary position
    // (UPDATEFLAG_STATIONARY_POSITION): where it stands does not matter to
    // anything these tests read. `self` is UPDATEFLAG_SELF, the mark on the
    // receiving player's own block, which adds nothing to the payload.
    UpdateObject& create(uint64_t guid, wowee::game::ObjectType type, const Fields& fields,
                         bool self = false) {
        ++blocks_;
        body_.writeUInt8(static_cast<uint8_t>(wowee::game::UpdateType::CREATE_OBJECT2));
        body_.writePackedGuid(guid);
        body_.writeUInt8(static_cast<uint8_t>(type));
        body_.writeUInt16(static_cast<uint16_t>(kStationaryPosition | (self ? kSelf : 0)));
        for (int i = 0; i < 4; ++i) body_.writeFloat(0.0f);
        writeFields(fields);
        return *this;
    }

    // Fields changing on something already there.
    UpdateObject& values(uint64_t guid, const Fields& fields) {
        ++blocks_;
        body_.writeUInt8(static_cast<uint8_t>(wowee::game::UpdateType::VALUES));
        body_.writePackedGuid(guid);
        writeFields(fields);
        return *this;
    }

    [[nodiscard]] wowee::network::Packet packet() const {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_UPDATE_OBJECT));
        p.writeUInt32(blocks_);
        const auto& body = body_.getData();
        p.writeBytes(body.data(), body.size());
        return p;
    }

private:
    static constexpr uint16_t kSelf = 0x0001;
    static constexpr uint16_t kStationaryPosition = 0x0040;

    void writeFields(const Fields& fields) {
        std::map<uint16_t, uint32_t> byIndex;
        for (const auto& [field, value] : fields) {
            const uint16_t index = wowee::game::fieldIndex(field);
            REQUIRE(index != 0xFFFF);
            byIndex[index] = value;
        }
        const uint8_t maskBlocks =
            byIndex.empty() ? 0 : static_cast<uint8_t>(byIndex.rbegin()->first / 32 + 1);
        body_.writeUInt8(maskBlocks);
        for (uint8_t b = 0; b < maskBlocks; ++b) {
            uint32_t mask = 0;
            for (const auto& entry : byIndex)
                if (entry.first / 32 == b) mask |= 1u << (entry.first % 32);
            body_.writeUInt32(mask);
        }
        for (const auto& entry : byIndex) body_.writeUInt32(entry.second);
    }

    uint32_t blocks_ = 0;
    wowee::network::Packet body_;
};

// Keeps what the client sends instead of writing it anywhere.
class SentPackets : public wowee::network::WorldSocket {
public:
    void send(const wowee::network::Packet& packet) override { sent_.push_back(packet); }
    // Not connected unless a test says so: a session that stays up past a
    // logout is one that asks for this.
    [[nodiscard]] bool isConnected() const override { return connected_; }
    void setConnected(bool on) { connected_ = on; }

    // Everything sent with this opcode, oldest first.
    [[nodiscard]] std::vector<wowee::network::Packet> of(Opcode opcode) const {
        std::vector<wowee::network::Packet> found;
        for (const auto& p : sent_)
            if (p.getOpcode() == wowee::game::wireOpcode(opcode)) found.push_back(p);
        return found;
    }

private:
    std::vector<wowee::network::Packet> sent_;
    bool connected_ = false;
};

// A client in the world as one character, ready for packets. WotLK unless
// another expansion is named.
class Session {
public:
    static constexpr uint64_t kPlayerGuid = 0x0000000000000042ull;

    // The expansion is chosen and its tables loaded the way the client does it
    // at startup, from the profiles under Data/expansions in this repository.
    explicit Session(const char* expansion = "wotlk") : handler_(services_) {
        auto registry = std::make_unique<wowee::game::ExpansionRegistry>();
        registry->initialize(std::string(WOWEE_SOURCE_DIR) + "/Data");
        REQUIRE(registry->setActive(expansion));
        const std::string dir = registry->getActive()->dataPath;
        wowee::game::GameHandlerTestAccess::setExpansions(app_, std::move(registry));
        REQUIRE(handler_.getOpcodeTable().loadFromJson(dir + "/opcodes.json"));
        REQUIRE(handler_.getUpdateFieldTable().loadFromJson(dir + "/update_fields.json"));
        handler_.setPacketParsers(wowee::game::createPacketParsers(expansion));
        handler_.setPlayerGuid(kPlayerGuid);
        auto socket = std::make_unique<SentPackets>();
        sent_ = socket.get();
        wowee::game::GameHandlerTestAccess::setSocket(handler_, std::move(socket));
        wowee::game::GameHandlerTestAccess::enterWorld(handler_);
    }

    wowee::game::GameHandler& handler() { return handler_; }
    const SentPackets& sent() const { return *sent_; }
    SentPackets& socket() { return *sent_; }

    void receive(const wowee::network::Packet& packet) {
        wowee::game::GameHandlerTestAccess::receive(handler_, packet);
    }
    void receive(const UpdateObject& update) { receive(update.packet()); }

    // The player's own create block, as the server sends it on login.
    void spawnPlayer(const UpdateObject::Fields& fields) {
        receive(UpdateObject().create(kPlayerGuid, wowee::game::ObjectType::PLAYER, fields,
                                      true));
    }

private:
    // Never initialized: it is here because the game code asks the
    // application's expansion registry which protocol it is speaking.
    wowee::core::Application app_;
    wowee::game::GameServices services_;
    wowee::game::GameHandler handler_;
    SentPackets* sent_ = nullptr;
};

}  // namespace replay
