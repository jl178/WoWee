#include "game/movement_handler.hpp"
#include "core/input_trace.hpp"
#include "game/taxi_rules.hpp"
#include "game/global_strings.hpp"
#include "game/stock_movement.hpp"
#include "game/unit_motion_glue.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/packet_parsers.hpp"
#include "game/spline_packet.hpp"
#include "game/transport_manager.hpp"
#include "game/entity.hpp"
#include "network/world_socket.hpp"
#include "network/packet.hpp"
#include "core/coordinates.hpp"
#include "core/application.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "core/logger.hpp"
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <zlib.h>
#include <array>
#include <unordered_set>

namespace wowee {
namespace game {

namespace sm = stock_movement;

namespace {

struct KnownAreaTriggerDestination {
    uint32_t triggerId;
    uint32_t mapId;
    float serverX;
    float serverY;
    float serverZ;
    float serverO;
};

constexpr KnownAreaTriggerDestination kKnownAreaTriggerDestinations[] = {
    {.triggerId = 2166, .mapId = 0,   .serverX = -4838.95f, .serverY = -1318.46f, .serverZ = 501.868f, .serverO = 1.42372f}, // Deeprun Tram -> Ironforge
    {.triggerId = 2171, .mapId = 0,   .serverX = -8364.57f,   .serverY = 535.981f, .serverZ = 91.7969f, .serverO = 2.24619f}, // Deeprun Tram -> Stormwind
    {.triggerId = 2173, .mapId = 369,    .serverX = 68.3006f, .serverY = 2490.91f, .serverZ = -4.29647f, .serverO = 3.12192f}, // Stormwind -> Deeprun Tram
    {.triggerId = 2175, .mapId = 369,    .serverX = 69.2542f,   .serverY = 10.257f, .serverZ = -4.29664f, .serverO = 3.09832f}, // Ironforge -> Deeprun Tram
};

const KnownAreaTriggerDestination* findKnownAreaTriggerDestination(uint32_t triggerId, uint32_t mapId) {
    for (const auto& dest : kKnownAreaTriggerDestinations) {
        if (dest.triggerId == triggerId && dest.mapId == mapId) return &dest;
    }
    return nullptr;
}

const KnownAreaTriggerDestination* findKnownAreaTriggerDestinationByTrigger(uint32_t triggerId) {
    for (const auto& dest : kKnownAreaTriggerDestinations) {
        if (dest.triggerId == triggerId) return &dest;
    }
    return nullptr;
}

uint32_t findKnownReturnAreaTrigger(uint32_t triggerId) {
    switch (triggerId) {
        case 2166: return 2175; // Deeprun Tram -> Ironforge, return via Ironforge portal
        case 2171: return 2173; // Deeprun Tram -> Stormwind, return via Stormwind portal
        case 2173: return 2171; // Stormwind -> Deeprun Tram, exit on Stormwind side
        case 2175: return 2166; // Ironforge -> Deeprun Tram, exit on Ironforge side
        default: return 0;
    }
}

} // namespace

/// Which of another unit's speeds a spline speed opcode sets, from the member
/// the same opcode sets for the local player.
SpeedKind MovementHandler::remoteSpeedKind(float MovementHandler::* member) {
    if (member == &MovementHandler::serverWalkSpeed_) return SpeedKind::Walk;
    if (member == &MovementHandler::serverRunBackSpeed_) return SpeedKind::RunBack;
    if (member == &MovementHandler::serverSwimSpeed_) return SpeedKind::Swim;
    if (member == &MovementHandler::serverSwimBackSpeed_) return SpeedKind::SwimBack;
    if (member == &MovementHandler::serverFlightSpeed_) return SpeedKind::Flight;
    if (member == &MovementHandler::serverFlightBackSpeed_) return SpeedKind::FlightBack;
    if (member == &MovementHandler::serverTurnRate_) return SpeedKind::TurnRate;
    if (member == &MovementHandler::serverPitchRate_) return SpeedKind::PitchRate;
    return SpeedKind::Run;
}

MovementHandler::MovementHandler(GameHandler& owner)
    : owner_(owner), movementInfo(owner.movementInfoRef()) {}

void MovementHandler::applyServerMovementSpeeds(float walk, float run, float runBack,
                                                 float swim, float swimBack, float flight,
                                                 float flightBack, float turn, float pitch) {
    auto apply = [](float value, float& destination) {
        if (std::isfinite(value) && value > 0.01f && value < 200.0f) {
            destination = value;
        }
    };
    if (movingOtherUnit()) {
        // The player's own speeds, kept aside while a vehicle is driven.
        apply(walk, playerState_.walk);
        apply(run, playerState_.run);
        apply(runBack, playerState_.runBack);
        apply(swim, playerState_.swim);
        apply(swimBack, playerState_.swimBack);
        apply(flight, playerState_.flight);
        apply(flightBack, playerState_.flightBack);
        apply(turn, playerState_.turn);
        apply(pitch, playerState_.pitch);
        return;
    }
    apply(walk, serverWalkSpeed_);
    apply(run, serverRunSpeed_);
    apply(runBack, serverRunBackSpeed_);
    apply(swim, serverSwimSpeed_);
    apply(swimBack, serverSwimBackSpeed_);
    apply(flight, serverFlightSpeed_);
    apply(flightBack, serverFlightBackSpeed_);
    apply(turn, serverTurnRate_);
    apply(pitch, serverPitchRate_);
    LOG_INFO("Movement speeds restored from object update: walk=", serverWalkSpeed_,
             " run=", serverRunSpeed_, " runBack=", serverRunBackSpeed_,
             " swim=", serverSwimSpeed_, " flight=", serverFlightSpeed_);
}

void MovementHandler::registerOpcodes(DispatchTable& table) {
    // Creature movement
    table[Opcode::SMSG_MONSTER_MOVE] = [this](network::Packet& packet) { handleMonsterMove(packet); };
    table[Opcode::SMSG_COMPRESSED_MOVES] = [this](network::Packet& packet) { handleCompressedMoves(packet); };
    table[Opcode::SMSG_MONSTER_MOVE_TRANSPORT] = [this](network::Packet& packet) { handleMonsterMoveTransport(packet); };

    // Spline move: consume-only (no state change)
    for (auto op : { Opcode::SMSG_SPLINE_MOVE_FEATHER_FALL,
                     Opcode::SMSG_SPLINE_MOVE_LAND_WALK,
                     Opcode::SMSG_SPLINE_MOVE_NORMAL_FALL,
                     Opcode::SMSG_SPLINE_MOVE_ROOT }) {
        table[op] = [](network::Packet& packet) {
            if (packet.hasRemaining(1))
                (void)packet.readPackedGuid();
        };
    }

    // Spline move: synth flags (each opcode produces different flags)
    {
        auto makeSynthHandler = [this](uint32_t synthFlags, uint32_t mask) {
            return [this, synthFlags, mask](network::Packet& packet) {
                if (!packet.hasRemaining(1)) return;
                uint64_t guid = packet.readPackedGuid();
                if (guid == 0 || guid == owner_.getPlayerGuid() || !owner_.unitMoveFlagsCallbackRef()) return;
                owner_.unitMoveFlagsCallbackRef()(guid, synthFlags, mask);
            };
        };
        // These are this client's own movement flags, not wire values: the
        // opcode carries no flag word and the handler invents one to describe
        // what the unit is now doing. Naming them keeps them following
        // MovementFlags, which is what every reader compares against.
        constexpr auto asFlag = [](MovementFlags f) { return static_cast<uint32_t>(f); };
        constexpr uint32_t kWalkBit = asFlag(MovementFlags::WALKING);
        constexpr uint32_t kSwimBit = asFlag(MovementFlags::SWIMMING);
        constexpr uint32_t kFlyBits =
            asFlag(MovementFlags::CAN_FLY) | asFlag(MovementFlags::FLYING);

        table[Opcode::SMSG_SPLINE_MOVE_SET_WALK_MODE] =
            makeSynthHandler(kWalkBit, kWalkBit);
        // Run mode says the unit is not walking. It says nothing about
        // swimming or flying, and clearing those took the flight animation off
        // anything that started moving.
        table[Opcode::SMSG_SPLINE_MOVE_SET_RUN_MODE]  = makeSynthHandler(0u, kWalkBit);
        // CAN_FLY because that is what the opcode means - AzerothCore sends it
        // from Unit::SetCanFly for units the client does not control - and
        // FLYING because that is the flag the animation reads.
        //
        // It used to synthesise CAN_FLY and DESCENDING. Nothing reads either:
        // the callback these flags reach keys swimming, walking and flying off
        // SWIMMING, WALKING and FLYING, so the opcode landed, was parsed, and
        // changed nothing. A gryphon told to fly kept its ground animation.
        table[Opcode::SMSG_SPLINE_MOVE_SET_FLYING]    = makeSynthHandler(kFlyBits, kFlyBits);
        table[Opcode::SMSG_SPLINE_MOVE_START_SWIM]    = makeSynthHandler(kSwimBit, kSwimBit);
        table[Opcode::SMSG_SPLINE_MOVE_STOP_SWIM]     = makeSynthHandler(0u, kSwimBit);
        // Gravity and hover decide whether a standing unit settles onto the
        // floor under its server position (Application's creature sync), so
        // they are state now rather than consumed and dropped.
        constexpr uint32_t kLevitateBit = asFlag(MovementFlags::LEVITATING);
        constexpr uint32_t kHoverBit = asFlag(MovementFlags::HOVER);
        table[Opcode::SMSG_SPLINE_MOVE_GRAVITY_DISABLE] = makeSynthHandler(kLevitateBit, kLevitateBit);
        table[Opcode::SMSG_SPLINE_MOVE_GRAVITY_ENABLE]  = makeSynthHandler(0u, kLevitateBit);
        table[Opcode::SMSG_SPLINE_MOVE_SET_HOVER]       = makeSynthHandler(kHoverBit, kHoverBit);
        table[Opcode::SMSG_SPLINE_MOVE_UNSET_HOVER]     = makeSynthHandler(0u, kHoverBit);
    }

    // Spline speed: all opcodes share the same PackedGuid+float format, differing
    // only in which member receives the value. Factory avoids 8 copy-pasted lambdas.
    auto makeSplineSpeedHandler = [this](float MovementHandler::* member) {
        return [this, member](network::Packet& packet) {
            if (!packet.hasRemaining(5)) return;
            uint64_t guid = packet.readPackedGuid();
            if (!packet.hasRemaining(4)) return;
            float speed = packet.readFloat();
            if (guid == owner_.getPlayerGuid() && std::isfinite(speed) && speed > 0.01f && speed < 200.0f)
                this->*member = speed;
            else if (guid != owner_.getPlayerGuid())
                applyRemoteSpeed(owner_, guid, remoteSpeedKind(member), speed);
        };
    };
    table[Opcode::SMSG_SPLINE_SET_RUN_SPEED]      = makeSplineSpeedHandler(&MovementHandler::serverRunSpeed_);
    table[Opcode::SMSG_SPLINE_SET_RUN_BACK_SPEED]  = makeSplineSpeedHandler(&MovementHandler::serverRunBackSpeed_);
    table[Opcode::SMSG_SPLINE_SET_SWIM_SPEED]      = makeSplineSpeedHandler(&MovementHandler::serverSwimSpeed_);

    // Force speed changes
    table[Opcode::SMSG_FORCE_RUN_SPEED_CHANGE] = [this](network::Packet& packet) { handleForceRunSpeedChange(packet); };
    table[Opcode::SMSG_FORCE_MOVE_ROOT] = [this](network::Packet& packet) { handleForceMoveRootState(packet, true); };
    table[Opcode::SMSG_FORCE_MOVE_UNROOT] = [this](network::Packet& packet) { handleForceMoveRootState(packet, false); };
    table[Opcode::SMSG_FORCE_WALK_SPEED_CHANGE] = [this](network::Packet& packet) {
        handleForceSpeedChange(packet, "WALK_SPEED", Opcode::CMSG_FORCE_WALK_SPEED_CHANGE_ACK, &serverWalkSpeed_);
    };
    table[Opcode::SMSG_FORCE_RUN_BACK_SPEED_CHANGE] = [this](network::Packet& packet) {
        handleForceSpeedChange(packet, "RUN_BACK_SPEED", Opcode::CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK, &serverRunBackSpeed_);
    };
    table[Opcode::SMSG_FORCE_SWIM_SPEED_CHANGE] = [this](network::Packet& packet) {
        handleForceSpeedChange(packet, "SWIM_SPEED", Opcode::CMSG_FORCE_SWIM_SPEED_CHANGE_ACK, &serverSwimSpeed_);
    };
    table[Opcode::SMSG_FORCE_SWIM_BACK_SPEED_CHANGE] = [this](network::Packet& packet) {
        handleForceSpeedChange(packet, "SWIM_BACK_SPEED", Opcode::CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK, &serverSwimBackSpeed_);
    };
    table[Opcode::SMSG_FORCE_FLIGHT_SPEED_CHANGE] = [this](network::Packet& packet) {
        handleForceSpeedChange(packet, "FLIGHT_SPEED", Opcode::CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK, &serverFlightSpeed_);
    };
    table[Opcode::SMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE] = [this](network::Packet& packet) {
        handleForceSpeedChange(packet, "FLIGHT_BACK_SPEED", Opcode::CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK, &serverFlightBackSpeed_);
    };
    table[Opcode::SMSG_FORCE_TURN_RATE_CHANGE] = [this](network::Packet& packet) {
        handleForceSpeedChange(packet, "TURN_RATE", Opcode::CMSG_FORCE_TURN_RATE_CHANGE_ACK, &serverTurnRate_);
    };
    table[Opcode::SMSG_FORCE_PITCH_RATE_CHANGE] = [this](network::Packet& packet) {
        handleForceSpeedChange(packet, "PITCH_RATE", Opcode::CMSG_FORCE_PITCH_RATE_CHANGE_ACK, &serverPitchRate_);
    };

    // Movement flag toggles
    table[Opcode::SMSG_MOVE_SET_CAN_FLY] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "SET_CAN_FLY", Opcode::CMSG_MOVE_SET_CAN_FLY_ACK,
            static_cast<uint32_t>(MovementFlags::CAN_FLY), true);
    };
    table[Opcode::SMSG_MOVE_UNSET_CAN_FLY] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "UNSET_CAN_FLY", Opcode::CMSG_MOVE_SET_CAN_FLY_ACK,
            static_cast<uint32_t>(MovementFlags::CAN_FLY), false);
    };
    table[Opcode::SMSG_MOVE_FEATHER_FALL] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "FEATHER_FALL", Opcode::CMSG_MOVE_FEATHER_FALL_ACK,
            static_cast<uint32_t>(MovementFlags::FEATHER_FALL), true);
    };
    table[Opcode::SMSG_MOVE_WATER_WALK] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "WATER_WALK", Opcode::CMSG_MOVE_WATER_WALK_ACK,
            static_cast<uint32_t>(MovementFlags::WATER_WALK), true);
    };
    table[Opcode::SMSG_MOVE_SET_HOVER] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "SET_HOVER", Opcode::CMSG_MOVE_HOVER_ACK,
            static_cast<uint32_t>(MovementFlags::HOVER), true);
    };
    table[Opcode::SMSG_MOVE_UNSET_HOVER] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "UNSET_HOVER", Opcode::CMSG_MOVE_HOVER_ACK,
            static_cast<uint32_t>(MovementFlags::HOVER), false);
    };
    table[Opcode::SMSG_MOVE_KNOCK_BACK] = [this](network::Packet& packet) { handleMoveKnockBack(packet); };

    // Teleport. Two opcodes, two different things (stock: 0xC5 goes to the
    // unit movement handler 0x741B60 with the heartbeats, 0xC7 to the mover
    // handler 0x732450 -> 0x72D2D0). MSG_MOVE_TELEPORT is the server telling
    // everyone near a unit that it was put somewhere - packed guid and a
    // movement block, no counter (AzerothCore Unit::SendTeleportPacket,
    // SendMessageToSet with self excluded) - so it is that unit's movement.
    // Read as the ack the first four bytes of the block went for a counter,
    // and the creature was moved to whatever the rest decoded to.
    // MSG_MOVE_TELEPORT_ACK is ours: the active mover is put somewhere and
    // acknowledges.
    table[Opcode::MSG_MOVE_TELEPORT] = [this](network::Packet& packet) {
        if (owner_.getState() != WorldState::IN_WORLD) return;
        if (!isActiveExpansion("wotlk")) { handleOtherPlayerMovement(packet); return; }
        // 0x7189A0: the unit is put where the block says, at once (a seat
        // change it carries is the passenger code's).
        if (!packet.hasFullPackedGuid()) return;
        const uint64_t guid = packet.readPackedGuid();
        MovementInfo info{};
        if (guid == 0 || guid == owner_.getPlayerGuid() || guid == activeMoverGuid() ||
            !readMovementBlockWotlk(packet, info)) return;
        auto entity = owner_.getEntityManager().getEntity(guid);
        if (!entity) return;
        const bool onTransport = (info.flags & static_cast<uint32_t>(MovementFlags::ONTRANSPORT)) &&
                                 info.transportGuid != 0 && owner_.getTransportManager();
        if (onTransport) { applyRemoteMovement(owner_, *entity, info, true); return; }
        const glm::vec3 c = core::coords::serverToCanonical(glm::vec3(info.x, info.y, info.z));
        entity->setPosition(c.x, c.y, c.z, core::coords::serverToCanonicalYaw(info.orientation));
        if (entity->getType() == ObjectType::UNIT && owner_.creatureMoveCallbackRef())
            owner_.creatureMoveCallbackRef()(guid, c.x, c.y, c.z, 0);
        LOG_INFO("MSG_MOVE_TELEPORT: 0x", std::hex, guid, std::dec, " to (", c.x, ", ", c.y, ", ", c.z, ")");
    };
    table[Opcode::MSG_MOVE_TELEPORT_ACK] = [this](network::Packet& packet) { handleTeleportAck(packet); };
    table[Opcode::SMSG_NEW_WORLD] = [this](network::Packet& packet) { handleNewWorld(packet); };

    // Taxi
    table[Opcode::SMSG_SHOWTAXINODES] = [this](network::Packet& packet) { handleShowTaxiNodes(packet); };
    table[Opcode::SMSG_ACTIVATETAXIREPLY] = [this](network::Packet& packet) { handleActivateTaxiReply(packet); };

    // MSG_MOVE_* relay (other player movement)
    for (auto op : { Opcode::MSG_MOVE_START_FORWARD, Opcode::MSG_MOVE_START_BACKWARD,
                     Opcode::MSG_MOVE_STOP, Opcode::MSG_MOVE_START_STRAFE_LEFT,
                     Opcode::MSG_MOVE_START_STRAFE_RIGHT, Opcode::MSG_MOVE_STOP_STRAFE,
                     Opcode::MSG_MOVE_JUMP, Opcode::MSG_MOVE_START_TURN_LEFT,
                     Opcode::MSG_MOVE_START_TURN_RIGHT, Opcode::MSG_MOVE_STOP_TURN,
                     Opcode::MSG_MOVE_SET_FACING, Opcode::MSG_MOVE_FALL_LAND,
                     Opcode::MSG_MOVE_HEARTBEAT, Opcode::MSG_MOVE_START_SWIM,
                     Opcode::MSG_MOVE_STOP_SWIM, Opcode::MSG_MOVE_SET_WALK_MODE,
                     Opcode::MSG_MOVE_SET_RUN_MODE, Opcode::MSG_MOVE_START_PITCH_UP,
                     Opcode::MSG_MOVE_START_PITCH_DOWN, Opcode::MSG_MOVE_STOP_PITCH,
                     Opcode::MSG_MOVE_START_ASCEND, Opcode::MSG_MOVE_STOP_ASCEND,
                     Opcode::MSG_MOVE_START_DESCEND, Opcode::MSG_MOVE_SET_PITCH,
                     Opcode::MSG_MOVE_GRAVITY_CHNG, Opcode::MSG_MOVE_UPDATE_CAN_FLY,
                     Opcode::MSG_MOVE_UPDATE_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY,
                     // The three that were left out of this list while their
                     // siblings were in it. AzerothCore answers a force-move
                     // ack by broadcasting the mover's movement info to
                     // everyone but the mover - MiscHandler's switch turns
                     // CMSG_MOVE_HOVER_ACK into MSG_MOVE_HOVER and so on - so
                     // these carry another player's position and flags at the
                     // moment they start hovering, feather-falling or walking
                     // on water, and dropping them left that player where they
                     // were last seen until their next heartbeat.
                     Opcode::MSG_MOVE_HOVER, Opcode::MSG_MOVE_FEATHER_FALL,
                     Opcode::MSG_MOVE_WATER_WALK,
                     Opcode::MSG_MOVE_ROOT, Opcode::MSG_MOVE_UNROOT }) {
        table[op] = [this](network::Packet& packet) {
            if (owner_.getState() == WorldState::IN_WORLD) handleOtherPlayerMovement(packet);
        };
    }

    // MSG_MOVE_SET_*_SPEED relay
    for (auto op : { Opcode::MSG_MOVE_SET_RUN_SPEED, Opcode::MSG_MOVE_SET_RUN_BACK_SPEED,
                     Opcode::MSG_MOVE_SET_WALK_SPEED, Opcode::MSG_MOVE_SET_SWIM_SPEED,
                     Opcode::MSG_MOVE_SET_SWIM_BACK_SPEED, Opcode::MSG_MOVE_SET_FLIGHT_SPEED,
                     Opcode::MSG_MOVE_SET_FLIGHT_BACK_SPEED }) {
        table[op] = [this](network::Packet& packet) {
            if (owner_.getState() == WorldState::IN_WORLD) handleMoveSetSpeed(packet);
        };
    }

    // ---- Client control & spline speed/flag changes ----

    // Client control update
    table[Opcode::SMSG_CLIENT_CONTROL_UPDATE] = [this](network::Packet& packet) {
        handleClientControlUpdate(packet);
    };

    // Spline move flag changes for other units (unroot/water_walk). UNSET_HOVER
    // is registered with the synthesised flags above.
    for (auto op : {Opcode::SMSG_SPLINE_MOVE_UNROOT,
                    Opcode::SMSG_SPLINE_MOVE_WATER_WALK}) {
        table[op] = [](network::Packet& packet) {
            // Minimal parse: PackedGuid only - no animation-relevant state change.
            if (packet.hasRemaining(1)) {
                (void)packet.readPackedGuid();
            }
        };
    }

    table[Opcode::SMSG_SPLINE_MOVE_UNSET_FLYING] = [this](network::Packet& packet) {
        // PackedGuid + synthesised move-flags=0 → clears flying animation.
        if (!packet.hasRemaining(1)) return;
        uint64_t guid = packet.readPackedGuid();
        if (guid == 0 || guid == owner_.getPlayerGuid() || !owner_.unitMoveFlagsCallbackRef()) return;
        // Only the flying bits: this opcode says nothing about walking or
        // swimming.
        owner_.unitMoveFlagsCallbackRef()(
            guid, 0u,
            static_cast<uint32_t>(MovementFlags::CAN_FLY) |
                static_cast<uint32_t>(MovementFlags::FLYING));
    };

    // Remaining spline speed opcodes - same factory as above.
    table[Opcode::SMSG_SPLINE_SET_FLIGHT_SPEED]      = makeSplineSpeedHandler(&MovementHandler::serverFlightSpeed_);
    table[Opcode::SMSG_SPLINE_SET_FLIGHT_BACK_SPEED]  = makeSplineSpeedHandler(&MovementHandler::serverFlightBackSpeed_);
    table[Opcode::SMSG_SPLINE_SET_SWIM_BACK_SPEED]    = makeSplineSpeedHandler(&MovementHandler::serverSwimBackSpeed_);
    table[Opcode::SMSG_SPLINE_SET_WALK_SPEED]          = makeSplineSpeedHandler(&MovementHandler::serverWalkSpeed_);
    table[Opcode::SMSG_SPLINE_SET_TURN_RATE]           = makeSplineSpeedHandler(&MovementHandler::serverTurnRate_);
    // Pitch rate not stored locally - consume packet to keep stream aligned.
    table[Opcode::SMSG_SPLINE_SET_PITCH_RATE] = [](network::Packet& packet) { packet.skipAll(); };

    // ---- Player movement flag changes (server-pushed) ----
    table[Opcode::SMSG_MOVE_GRAVITY_DISABLE] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "GRAVITY_DISABLE", Opcode::CMSG_MOVE_GRAVITY_DISABLE_ACK,
            static_cast<uint32_t>(MovementFlags::LEVITATING), true);
    };
    table[Opcode::SMSG_MOVE_GRAVITY_ENABLE] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "GRAVITY_ENABLE", Opcode::CMSG_MOVE_GRAVITY_ENABLE_ACK,
            static_cast<uint32_t>(MovementFlags::LEVITATING), false);
    };
    table[Opcode::SMSG_MOVE_LAND_WALK] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "LAND_WALK", Opcode::CMSG_MOVE_WATER_WALK_ACK,
            static_cast<uint32_t>(MovementFlags::WATER_WALK), false);
    };
    table[Opcode::SMSG_MOVE_NORMAL_FALL] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "NORMAL_FALL", Opcode::CMSG_MOVE_FEATHER_FALL_ACK,
            static_cast<uint32_t>(MovementFlags::FEATHER_FALL), false);
    };
    table[Opcode::SMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "SET_CAN_TRANSITION_SWIM_FLY",
            Opcode::CMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY_ACK, 0, true);
    };
    table[Opcode::SMSG_MOVE_UNSET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "UNSET_CAN_TRANSITION_SWIM_FLY",
            Opcode::CMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY_ACK, 0, false);
    };
    table[Opcode::SMSG_MOVE_SET_COLLISION_HGT] = [this](network::Packet& packet) {
        handleMoveSetCollisionHeight(packet);
    };
    table[Opcode::SMSG_MOVE_SET_FLIGHT] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "SET_FLIGHT", Opcode::CMSG_MOVE_FLIGHT_ACK,
            static_cast<uint32_t>(MovementFlags::FLYING), true);
    };
    table[Opcode::SMSG_MOVE_UNSET_FLIGHT] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "UNSET_FLIGHT", Opcode::CMSG_MOVE_FLIGHT_ACK,
            static_cast<uint32_t>(MovementFlags::FLYING), false);
    };
}

// ============================================================
// handleClientControlUpdate
// ============================================================

void MovementHandler::handleClientControlUpdate(network::Packet& packet) {
    // PackedGuid + uint8 allowMovement.
    //
    // Through readPackedGuid rather than the byte loop that used to be written
    // out here. It decoded correctly, but it was a second copy of the packed
    // guid rule, and the layout sweep - which reads the widths a handler asks
    // for - saw a handler taking a uint8 where the server writes a guid and
    // called it a misparse. A second copy of a wire rule costs that twice
    // over: it can drift, and it hides the handler from the check.
    if (!packet.hasFullPackedGuid()) {
        LOG_WARNING("SMSG_CLIENT_CONTROL_UPDATE malformed (truncated packed guid)");
        packet.skipAll();
        return;
    }
    const uint64_t controlGuid = packet.readPackedGuid();
    if (!packet.hasRemaining(1)) {
        LOG_WARNING("SMSG_CLIENT_CONTROL_UPDATE too short: ", packet.getSize(), " bytes");
        return;
    }
    bool allowMovement = (packet.readUInt8() != 0);
    LOG_INFO("SMSG_CLIENT_CONTROL_UPDATE: guid=0x", std::hex, controlGuid, std::dec,
             " allow=", allowMovement ? 1 : 0, " mover=0x", std::hex, activeMoverGuid(), std::dec);
    // Another unit (a vehicle's control seat, a possessed creature): it
    // becomes the active mover, or stops being it (stock 0x72CCA0 ->
    // 0x729010). AzerothCore sends (vehicle, 1) on taking the control seat
    // and (vehicle, 0) then (player, 1) on leaving it.
    if (controlGuid != 0 && controlGuid != owner_.getPlayerGuid()) {
        if (allowMovement) {
            setActiveMover(controlGuid);
            serverMovementAllowed_ = true;
        } else if (controlGuid == activeMoverGuid()) {
            setActiveMover(0);
        }
        return;
    }
    if (movingOtherUnit()) {
        // The player's own control while it drives something else: it
        // applies when the player is the mover again.
        playerState_.allowed = allowMovement;
        return;
    }
    if (controlGuid == 0 || controlGuid == owner_.getPlayerGuid()) {
        bool changed = (serverMovementAllowed_ != allowMovement);
        serverMovementAllowed_ = allowMovement;
        if (changed && !allowMovement) {
            // Force-stop local movement immediately when server revokes control.
            movementInfo.flags &= ~(static_cast<uint32_t>(MovementFlags::FORWARD) |
                                    static_cast<uint32_t>(MovementFlags::BACKWARD) |
                                    static_cast<uint32_t>(MovementFlags::STRAFE_LEFT) |
                                    static_cast<uint32_t>(MovementFlags::STRAFE_RIGHT) |
                                    static_cast<uint32_t>(MovementFlags::TURN_LEFT) |
                                    static_cast<uint32_t>(MovementFlags::TURN_RIGHT));
            owner_.sendMovement(Opcode::MSG_MOVE_STOP);
            owner_.sendMovement(Opcode::MSG_MOVE_STOP_STRAFE);
            owner_.sendMovement(Opcode::MSG_MOVE_STOP_TURN);
            owner_.sendMovement(Opcode::MSG_MOVE_STOP_SWIM);
            // The events alone; the stock client has no line for it.
            owner_.fireAddonEvent("PLAYER_CONTROL_LOST", {});
        } else if (changed && allowMovement) {
            owner_.fireAddonEvent("PLAYER_CONTROL_GAINED", {});
        }
    }
}

// ============================================================
// Movement Timestamp
// ============================================================

uint32_t MovementHandler::nextMovementTimestampMs() {
    auto now = std::chrono::steady_clock::now();
    uint64_t elapsed = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - movementClockStart_).count()) + 1ULL;
    if (elapsed > std::numeric_limits<uint32_t>::max()) {
        movementClockStart_ = now;
        elapsed = 1ULL;
    }

    uint32_t candidate = static_cast<uint32_t>(elapsed);
    if (candidate <= lastMovementTimestampMs_) {
        candidate = lastMovementTimestampMs_ + 1U;
        if (candidate == 0) {
            movementClockStart_ = now;
            candidate = 1U;
        }
    }

    lastMovementTimestampMs_ = candidate;
    return candidate;
}

bool MovementHandler::restoreWorldTransferFallbackIfNearOrigin(const char* context) {
    if (!worldTransferFallbackValid_ || owner_.getCurrentMapId() != worldTransferFallbackMapId_) {
        return false;
    }
    if (!isCorruptOriginPosition(owner_.getCurrentMapId(),
                                movementInfo.x, movementInfo.y, movementInfo.z)) {
        return false;
    }

    LOG_WARNING(context,
                ": correcting near-origin map 0 position using area trigger ",
                worldTransferFallbackTriggerId_,
                " fallback canonical=(",
                worldTransferFallbackCanonicalPos_.x, ", ",
                worldTransferFallbackCanonicalPos_.y, ", ",
                worldTransferFallbackCanonicalPos_.z, ")");

    movementInfo.x = worldTransferFallbackCanonicalPos_.x;
    movementInfo.y = worldTransferFallbackCanonicalPos_.y;
    movementInfo.z = worldTransferFallbackCanonicalPos_.z;
    movementInfo.orientation = worldTransferFallbackCanonicalO_;
    movementInfo.transportGuid = 0;
    movementInfo.transportSeat = -1;
    movementInfo.flags &= ~static_cast<uint32_t>(MovementFlags::ONTRANSPORT);
    owner_.clearPlayerTransport();

    if (auto player = owner_.getEntityManager().getEntity(owner_.getPlayerGuid())) {
        player->setPosition(movementInfo.x, movementInfo.y, movementInfo.z, movementInfo.orientation);
    }

    return true;
}

// ============================================================
// sendMovement
// ============================================================

void MovementHandler::setFlightAirborne(bool airborne) {
    const uint32_t canFly = static_cast<uint32_t>(MovementFlags::CAN_FLY);
    const uint32_t flying = static_cast<uint32_t>(MovementFlags::FLYING);
    if (!(movementInfo.flags & canFly)) return;
    const bool now = (movementInfo.flags & flying) != 0;
    if (airborne == now) return;
    if (airborne) {
        movementInfo.flags |= flying;
        return;
    }
    // Landed. Said with a heartbeat, which carries the flags and is never
    // held back: the landing is otherwise only visible in the next movement
    // packet, and a player who lands and stands still sends none.
    movementInfo.flags &= ~(flying | static_cast<uint32_t>(MovementFlags::ASCENDING) |
                            static_cast<uint32_t>(MovementFlags::DESCENDING));
    sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
}

bool MovementHandler::inStockHeartbeatMotion() const {
    return (movementInfo.flags & stock_movement::kMovingMask) != 0;
}

bool MovementHandler::facingChangeWorthSending(float canonicalYaw) const {
    if (!hasLastPacketOrientation_) return true;
    return stock_movement::facingChangeWorthSending(lastPacketOrientation_, canonicalYaw);
}

void MovementHandler::sendMovement(Opcode opcode) {
    if (owner_.getState() != WorldState::IN_WORLD) {
        LOG_WARNING("Cannot send movement in state: ", (int)owner_.getState());
        return;
    }

    // Block manual movement while taxi is active/mounted, but always allow
    // stop/heartbeat opcodes so stuck states can be recovered.
    bool taxiAllowed =
        (opcode == Opcode::MSG_MOVE_HEARTBEAT) ||
        (opcode == Opcode::MSG_MOVE_STOP) ||
        (opcode == Opcode::MSG_MOVE_STOP_STRAFE) ||
        (opcode == Opcode::MSG_MOVE_STOP_TURN) ||
        (opcode == Opcode::MSG_MOVE_STOP_SWIM);
    // Held back, and said: a packet not sent leaves the server with the last
    // position it was told, while the player walks on here - so every NPC the
    // player then walks up to is out of the server's reach, and refuses in
    // silence. Once per stretch of it, not per packet.
    const char* heldReason = nullptr;
    if (isWorldPortHeld()) {
        // Every opcode, stop and heartbeat included: the stock client sends
        // nothing between SMSG_NEW_WORLD and the end of the load (0x403B70).
        heldReason = "the new map is loading";
    } else if (!taxiAllowed) {
        if (!serverMovementAllowed_) heldReason = "the server has not given control of the character";
        else if (movingOtherUnit()) heldReason = nullptr;
        else if (onTaxiFlight_) heldReason = "a taxi flight is in progress";
        else if (taxiMountActive_) heldReason = "a taxi mount is on";
        else if (owner_.resurrectPendingRef()) heldReason = "a resurrection is pending";
    }
    if (heldReason) {
        if (heldReason != movementHeldReason_) {
            movementHeldReason_ = heldReason;
            LOG_WARNING("Movement: not sent while ", heldReason,
                        " - the server keeps the last position it was told");
        }
        return;
    }

    // Always send a strictly increasing non-zero client movement clock value.
    const uint32_t movementTime = nextMovementTimestampMs();
    movementInfo.time = movementTime;

    if (opcode == Opcode::MSG_MOVE_SET_FACING &&
        isPreWotlk()) {
        const float facingDelta = core::coords::normalizeAngleRad(
            movementInfo.orientation - lastFacingSentOrientation_);
        const uint32_t sinceLastFacingMs =
            lastFacingSendTimeMs_ != 0 && movementTime >= lastFacingSendTimeMs_
                ? (movementTime - lastFacingSendTimeMs_)
                : std::numeric_limits<uint32_t>::max();
        if (std::abs(facingDelta) < 0.02f && sinceLastFacingMs < 200U) {
            return;
        }
    }

    // Track movement state transition for PLAYER_STARTED/STOPPED_MOVING events
    const uint32_t kMoveMask = static_cast<uint32_t>(MovementFlags::FORWARD) |
                               static_cast<uint32_t>(MovementFlags::BACKWARD) |
                               static_cast<uint32_t>(MovementFlags::STRAFE_LEFT) |
                               static_cast<uint32_t>(MovementFlags::STRAFE_RIGHT);
    const bool wasMoving = (movementInfo.flags & kMoveMask) != 0;

    // Starting to move: CGInputControl 0x5FAA40, before the movement packet.
    // Casts whose InterruptFlags have movement are cancelled, a channel whose
    // ChannelInterruptFlags have it is asked to stop (SpellHandler::
    // interruptForMoveStart). Run by MoveForward/Backward/Strafe starts,
    // JumpOrAscendStart and SitStandOrDescendStart, autorun and two-button
    // running - here, the start opcodes they send. Turning is not one.
    if (!movingOtherUnit() && owner_.getSpellHandler() &&
        owner_.getSpellHandler()->moveStartHasWork()) {
        const bool isMoveStart =
            opcode == Opcode::MSG_MOVE_START_FORWARD  ||
            opcode == Opcode::MSG_MOVE_START_BACKWARD ||
            opcode == Opcode::MSG_MOVE_START_STRAFE_LEFT  ||
            opcode == Opcode::MSG_MOVE_START_STRAFE_RIGHT ||
            opcode == Opcode::MSG_MOVE_JUMP ||
            opcode == Opcode::MSG_MOVE_START_ASCEND ||
            opcode == Opcode::MSG_MOVE_START_DESCEND;
        if (isMoveStart && owner_.getSpellHandler()) {
            owner_.getSpellHandler()->interruptForMoveStart();
        }
    }

    // Update movement flags based on opcode
    switch (opcode) {
        // The direction keys by the stock rules (0x988A20, 0x988B00,
        // 0x98C8D0, 0x98BF80): on the ground at once; in a fall that has a
        // direction, a PENDING flag the landing applies.
        case Opcode::MSG_MOVE_START_FORWARD:
            movementInfo.flags = sm::startForwardBackward(movementInfo.flags, true);
            break;
        case Opcode::MSG_MOVE_START_BACKWARD:
            movementInfo.flags = sm::startForwardBackward(movementInfo.flags, false);
            break;
        case Opcode::MSG_MOVE_STOP:
            movementInfo.flags = sm::stopForwardBackward(movementInfo.flags);
            break;
        case Opcode::MSG_MOVE_START_STRAFE_LEFT:
            movementInfo.flags = sm::startStrafe(movementInfo.flags, true);
            break;
        case Opcode::MSG_MOVE_START_STRAFE_RIGHT:
            movementInfo.flags = sm::startStrafe(movementInfo.flags, false);
            break;
        case Opcode::MSG_MOVE_STOP_STRAFE:
            movementInfo.flags = sm::stopStrafe(movementInfo.flags);
            break;
        case Opcode::MSG_MOVE_JUMP:
            movementInfo.flags |= static_cast<uint32_t>(MovementFlags::FALLING);
            isFalling_ = true;
            fallStartMs_ = movementInfo.time;
            movementInfo.fallTime = 0;
            {
                // The stock jump block (CMovement 0x9883F0, written by
                // 0x9872C0's packet builder): the vertical speed with the
                // stock sign, DOWN positive - a jump goes out as -7.955547 (or
                // -9.096748 from the water) - then the direction of travel and
                // the speed along it, as CMovement holds them: +0x70/+0x74 is
                // the movement direction (backward, strafe and diagonal
                // included; the facing for a standing jump), +0x8C the speed
                // GetSpeed picked (0 standing, walk or run-back included).
                const bool fromWater =
                    (movementInfo.flags & static_cast<uint32_t>(MovementFlags::SWIMMING)) != 0;
                movementInfo.jumpVelocity = -(fromWater ? sm::kWaterJumpVelocity : sm::kJumpVelocity);
                // The jump leaves the water (0x988370 clears SWIMMING,
                // ASCENDING and DESCENDING), so the packet goes out without
                // them and no STOP_SWIM follows.
                movementInfo.flags &= ~(static_cast<uint32_t>(MovementFlags::SWIMMING) |
                                        static_cast<uint32_t>(MovementFlags::ASCENDING) |
                                        static_cast<uint32_t>(MovementFlags::DESCENDING));
                // Worked out in the wire's own frame (the stock facing), and
                // laid down in the stock order: x first. The writers put
                // jumpSinAngle first on the wire (AzerothCore's names for the
                // two), so x goes there.
                const sm::Dir2 dir = sm::moveDirection(
                    movementInfo.flags, core::coords::canonicalToServerYaw(movementInfo.orientation));
                movementInfo.jumpSinAngle = dir.x;
                movementInfo.jumpCosAngle = dir.y;
                sm::Speeds sp;
                sp.walk = serverWalkSpeed_;
                sp.run = serverRunSpeed_;
                sp.runBack = serverRunBackSpeed_;
                sp.swim = serverSwimSpeed_;
                sp.swimBack = serverSwimBackSpeed_;
                sp.flight = serverFlightSpeed_;
                sp.flightBack = serverFlightBackSpeed_;
                movementInfo.jumpXYSpeed = sm::selectSpeed(movementInfo.flags, sp);
            }
            break;
        case Opcode::MSG_MOVE_SET_WALK_MODE:
            movementInfo.flags |= static_cast<uint32_t>(MovementFlags::WALKING);
            break;
        case Opcode::MSG_MOVE_SET_RUN_MODE:
            movementInfo.flags &= ~static_cast<uint32_t>(MovementFlags::WALKING);
            break;
        case Opcode::MSG_MOVE_START_TURN_LEFT:
            movementInfo.flags |= static_cast<uint32_t>(MovementFlags::TURN_LEFT);
            break;
        case Opcode::MSG_MOVE_START_TURN_RIGHT:
            movementInfo.flags |= static_cast<uint32_t>(MovementFlags::TURN_RIGHT);
            break;
        case Opcode::MSG_MOVE_STOP_TURN:
            movementInfo.flags &= ~(static_cast<uint32_t>(MovementFlags::TURN_LEFT) |
                                    static_cast<uint32_t>(MovementFlags::TURN_RIGHT));
            break;
        case Opcode::MSG_MOVE_FALL_LAND:
            // FALLING goes, and the keys pressed in the air take effect
            // (0x988490 -> 0x6EB3B0); the packet carries the result.
            movementInfo.flags = sm::landWithPendingKeys(movementInfo.flags);
            isFalling_ = false;
            fallStartMs_ = 0;
            movementInfo.fallTime = 0;
            movementInfo.jumpVelocity = 0.0f;
            movementInfo.jumpSinAngle = 0.0f;
            movementInfo.jumpCosAngle = 0.0f;
            movementInfo.jumpXYSpeed = 0.0f;
            break;
        case Opcode::MSG_MOVE_HEARTBEAT:
            timeSinceLastMoveHeartbeat_ = 0.0f;
            break;
        case Opcode::MSG_MOVE_START_ASCEND:
            movementInfo.flags |= static_cast<uint32_t>(MovementFlags::ASCENDING);
            movementInfo.flags &= ~static_cast<uint32_t>(MovementFlags::DESCENDING);
            // A climb with flight allowed is a take-off, and the client is
            // the one that says so: FLYING goes out on this packet, not a
            // frame later. See setFlightAirborne for the landing.
            // A swimmer's climb is an ascent in the water (the camera sends
            // START_ASCEND for it too), not a take-off.
            if ((movementInfo.flags & static_cast<uint32_t>(MovementFlags::CAN_FLY)) &&
                !(movementInfo.flags & static_cast<uint32_t>(MovementFlags::SWIMMING))) {
                movementInfo.flags |= static_cast<uint32_t>(MovementFlags::FLYING);
            }
            break;
        case Opcode::MSG_MOVE_STOP_ASCEND:
            movementInfo.flags &= ~static_cast<uint32_t>(MovementFlags::ASCENDING);
            movementInfo.flags &= ~static_cast<uint32_t>(MovementFlags::DESCENDING);
            break;
        case Opcode::MSG_MOVE_START_SWIM:
            // The camera controller has sent this pair on every water entry
            // and exit all along; nothing here listened, so no movement packet
            // this client sends has ever carried SWIMMING. The server reads
            // pitch only when the flag is set, which is why a swimming
            // character looked flat to everyone else, and isSwimming() - which
            // reads this same word - answered no while the player swam.
            movementInfo.flags |= static_cast<uint32_t>(MovementFlags::SWIMMING);
            // Entering water ends a fall, and nothing else would: FALL_LAND is
            // sent on touching ground and a swimmer never touches it, so a
            // jump into a lake left FALLING set with fallTime still climbing.
            // The keys pressed on the way down take effect as on a landing.
            if (movementInfo.flags & static_cast<uint32_t>(MovementFlags::FALLING))
                movementInfo.flags = sm::landWithPendingKeys(movementInfo.flags);
            isFalling_ = false;
            fallStartMs_ = 0;
            movementInfo.fallTime = 0;
            movementInfo.jumpVelocity = 0.0f;
            movementInfo.jumpSinAngle = 0.0f;
            movementInfo.jumpCosAngle = 0.0f;
            movementInfo.jumpXYSpeed = 0.0f;
            break;
        case Opcode::MSG_MOVE_STOP_SWIM:
            movementInfo.flags &= ~static_cast<uint32_t>(MovementFlags::SWIMMING);
            // Out of the water level again, so the next packet that does carry
            // a pitch - a flying mount - starts from horizontal rather than
            // from however the swimmer was last angled.
            movementInfo.pitch = 0.0f;
            break;
        case Opcode::MSG_MOVE_START_PITCH_UP:
            movementInfo.flags &= ~static_cast<uint32_t>(MovementFlags::PITCH_DOWN);
            movementInfo.flags |= static_cast<uint32_t>(MovementFlags::PITCH_UP);
            break;
        case Opcode::MSG_MOVE_START_PITCH_DOWN:
            movementInfo.flags &= ~static_cast<uint32_t>(MovementFlags::PITCH_UP);
            movementInfo.flags |= static_cast<uint32_t>(MovementFlags::PITCH_DOWN);
            break;
        case Opcode::MSG_MOVE_STOP_PITCH:
            movementInfo.flags &= ~(static_cast<uint32_t>(MovementFlags::PITCH_UP) |
                                    static_cast<uint32_t>(MovementFlags::PITCH_DOWN));
            break;
        case Opcode::MSG_MOVE_START_DESCEND:
            // Must set DESCENDING so outgoing movement packets carry the correct
            // flag during flight descent. Only clearing ASCENDING left the flag
            // field ambiguous (neither ascending nor descending).
            movementInfo.flags &= ~static_cast<uint32_t>(MovementFlags::ASCENDING);
            movementInfo.flags |= static_cast<uint32_t>(MovementFlags::DESCENDING);
            break;
        default:
            break;
    }

    // Fire PLAYER_STARTED/STOPPED_MOVING on movement state transitions
    {
        const bool isMoving = (movementInfo.flags & kMoveMask) != 0;
        if (isMoving && !wasMoving && owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("PLAYER_STARTED_MOVING", {});
        else if (!isMoving && wasMoving && owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("PLAYER_STOPPED_MOVING", {});
    }

    if (opcode == Opcode::MSG_MOVE_SET_FACING) {
        lastFacingSendTimeMs_ = movementInfo.time;
        lastFacingSentOrientation_ = movementInfo.orientation;
    }

    // Keep fallTime current
    if (isFalling_ && movementInfo.hasFlag(MovementFlags::FALLING)) {
        uint32_t elapsed = (movementInfo.time >= fallStartMs_)
                               ? (movementInfo.time - fallStartMs_)
                               : 0u;
        movementInfo.fallTime = elapsed;
    } else if (!movementInfo.hasFlag(MovementFlags::FALLING)) {
        if (isFalling_) {
            isFalling_ = false;
            fallStartMs_ = 0;
        }
        movementInfo.fallTime = 0;
    }

    if (!movingOtherUnit() &&
        (onTaxiFlight_ || taxiMountActive_ || taxiActivatePending_ || taxiClientActive_)) {
        sanitizeMovementForTaxi();
    }

    // A driven vehicle moves in the world; the ship or tram the player
    // stands on is the player's.
    bool includeTransportInWire = !movingOtherUnit() && owner_.isOnTransport();
    if (includeTransportInWire && owner_.getTransportManager()) {
        if (auto* tr = owner_.getTransportManager()->getTransport(owner_.playerTransportGuidRef()); tr && tr->isM2) {
            includeTransportInWire = false;
        }
    }

    // Add transport data if player is on a server-recognized transport
    if (includeTransportInWire) {
        bool transportResolved = false;
        if (owner_.getTransportManager()) {
            auto* tr = owner_.getTransportManager()->getTransport(owner_.playerTransportGuidRef());
            if (tr) {
                transportResolved = true;
                glm::vec3 composed = owner_.getTransportManager()->getPlayerWorldPosition(owner_.playerTransportGuidRef(), owner_.playerTransportOffsetRef());
                movementInfo.x = composed.x;
                movementInfo.y = composed.y;
                movementInfo.z = composed.z;
            }
        }
        if (!transportResolved) {
            // Transport not tracked - don't send ONTRANSPORT to the server.
            // Sending stale transport GUID + local offset causes the server to
            // compute a bad world position and teleport us to map origin.
            LOG_WARNING("sendMovement: transport 0x", std::hex, owner_.playerTransportGuidRef(),
                        std::dec, " not found - clearing transport state");
            includeTransportInWire = false;
            owner_.clearPlayerTransport();
        }
    }
    if (includeTransportInWire) {
        movementInfo.flags |= static_cast<uint32_t>(MovementFlags::ONTRANSPORT);
        movementInfo.transportGuid = owner_.playerTransportGuidRef();
        movementInfo.transportX = owner_.playerTransportOffsetRef().x;
        movementInfo.transportY = owner_.playerTransportOffsetRef().y;
        movementInfo.transportZ = owner_.playerTransportOffsetRef().z;
        movementInfo.transportTime = movementInfo.time;
        movementInfo.transportSeat = -1;
        movementInfo.transportTime2 = movementInfo.time;

        float transportYawCanonical = 0.0f;
        if (owner_.getTransportManager()) {
            if (auto* tr = owner_.getTransportManager()->getTransport(owner_.playerTransportGuidRef()); tr) {
                if (tr->hasServerYaw) {
                    transportYawCanonical = tr->serverYaw;
                } else {
                    transportYawCanonical = glm::eulerAngles(tr->rotation).z;
                }
            }
        }

        movementInfo.transportO =
            core::coords::normalizeAngleRad(movementInfo.orientation - transportYawCanonical);
    } else {
        movementInfo.flags &= ~static_cast<uint32_t>(MovementFlags::ONTRANSPORT);
        movementInfo.transportGuid = 0;
        movementInfo.transportSeat = -1;
    }

    if (opcode == Opcode::MSG_MOVE_HEARTBEAT && isClassicLikeExpansion()) {
        const bool stationaryIdle =
            !onTaxiFlight_ &&
            !taxiMountActive_ &&
            !taxiActivatePending_ &&
            !taxiClientActive_ &&
            !includeTransportInWire &&
            (movementInfo.flags & kLocomotionFlags) == 0;
        const uint32_t sinceLastHeartbeatMs =
            lastHeartbeatSendTimeMs_ != 0 && movementTime >= lastHeartbeatSendTimeMs_
                ? (movementTime - lastHeartbeatSendTimeMs_)
                : std::numeric_limits<uint32_t>::max();
        const bool unchangedState =
            std::abs(movementInfo.x - lastHeartbeatX_) < 0.01f &&
            std::abs(movementInfo.y - lastHeartbeatY_) < 0.01f &&
            std::abs(movementInfo.z - lastHeartbeatZ_) < 0.01f &&
            movementInfo.flags == lastHeartbeatFlags_ &&
            movementInfo.transportGuid == lastHeartbeatTransportGuid_;
        if (stationaryIdle && unchangedState && sinceLastHeartbeatMs < 1500U) {
            timeSinceLastMoveHeartbeat_ = 0.0f;
            return;
        }
        const uint32_t sinceLastNonHeartbeatMoveMs =
            lastNonHeartbeatMoveSendTimeMs_ != 0 && movementTime >= lastNonHeartbeatMoveSendTimeMs_
                ? (movementTime - lastNonHeartbeatMoveSendTimeMs_)
                : std::numeric_limits<uint32_t>::max();
        if (sinceLastNonHeartbeatMoveMs < 350U) {
            timeSinceLastMoveHeartbeat_ = 0.0f;
            return;
        }
    }

    LOG_DEBUG("Sending movement packet: opcode=0x", std::hex,
              wireOpcode(opcode), " flags=0x", movementInfo.flags, std::dec,
              (includeTransportInWire ? " ONTRANSPORT" : ""));

    // Block heartbeats from a near-origin position on Eastern Kingdoms (map 0).
    // These positions are almost certainly bugs (area trigger misfire, corrupted
    // save). Sending them tells the server the player is there, which persists
    // the bad position across sessions and creates a teleport loop.
    if (isCorruptOriginPosition(owner_.getCurrentMapId(),
                               movementInfo.x, movementInfo.y, movementInfo.z)) {
        if (!restoreWorldTransferFallbackIfNearOrigin("sendMovement")) {
            LOG_WARNING("sendMovement: BLOCKED near-origin heartbeat canonical=(",
                        movementInfo.x, ", ", movementInfo.y, ", ", movementInfo.z,
                        ") onTransport=", owner_.isOnTransport(),
                        " transportGuid=0x", std::hex, owner_.playerTransportGuidRef(), std::dec,
                        " flags=0x", std::hex, movementInfo.flags, std::dec);
            return;
        }
    }

    // Convert canonical → server coordinates for the wire
    MovementInfo wireInfo = movementInfo;
    glm::vec3 serverPos = core::coords::canonicalToServer(glm::vec3(wireInfo.x, wireInfo.y, wireInfo.z));
    wireInfo.x = serverPos.x;
    wireInfo.y = serverPos.y;
    wireInfo.z = serverPos.z;

    // Periodic position audit - log every ~60 heartbeats (~30s) to trace position drift.
    if (opcode == Opcode::MSG_MOVE_HEARTBEAT && ++heartbeatLogCount_ % 60 == 0) {
        LOG_DEBUG("HEARTBEAT #", heartbeatLogCount_, " canonical=(",
                    movementInfo.x, ",", movementInfo.y, ",", movementInfo.z,
                    ") server=(", wireInfo.x, ",", wireInfo.y, ",", wireInfo.z,
                    ") flags=0x", std::hex, movementInfo.flags, std::dec);
    }

    wireInfo.orientation = core::coords::canonicalToServerYaw(wireInfo.orientation);

    if (includeTransportInWire) {
        glm::vec3 serverTransportPos = core::coords::canonicalToServer(
            glm::vec3(wireInfo.transportX, wireInfo.transportY, wireInfo.transportZ));
        wireInfo.transportX = serverTransportPos.x;
        wireInfo.transportY = serverTransportPos.y;
        wireInfo.transportZ = serverTransportPos.z;
        wireInfo.transportO = core::coords::normalizeAngleRad(-wireInfo.transportO);
    }

    // Build and send movement packet (expansion-specific format)
    auto packet = owner_.getPacketParsers()
        ? owner_.getPacketParsers()->buildMovementPacket(opcode, wireInfo, activeMoverGuid())
        : MovementPacket::build(opcode, wireInfo, activeMoverGuid());
    owner_.getSocket()->send(packet);
    // The facing each movement packet carried, under WOWEE_INPUT_TRACE: a
    // SET_FACING that follows a start says by how much the facing moved.
    if (core::input_trace::enabled()) {
        LOG_WARNING("[input] move 0x", std::hex, static_cast<uint32_t>(wireOpcode(opcode)),
                    std::dec, " facing ", movementInfo.orientation, " flags 0x", std::hex,
                    movementInfo.flags, std::dec);
    }
    lastSentPos_ = glm::vec3(movementInfo.x, movementInfo.y, movementInfo.z);
    hasSentMovement_ = true;
    // Every movement packet restarts the heartbeat's 500 ms (0x71F0C0 ->
    // 0x6E9B70) and is the reference a later facing change is measured from.
    timeSinceLastMoveHeartbeat_ = 0.0f;
    lastPacketOrientation_ = movementInfo.orientation;
    hasLastPacketOrientation_ = true;
    if (movementHeldReason_) {
        LOG_WARNING("Movement: sending again (was held while ", movementHeldReason_, ")");
        movementHeldReason_ = nullptr;
    }

    if (opcode == Opcode::MSG_MOVE_HEARTBEAT) {
        lastHeartbeatSendTimeMs_ = movementInfo.time;
        lastHeartbeatX_ = movementInfo.x;
        lastHeartbeatY_ = movementInfo.y;
        lastHeartbeatZ_ = movementInfo.z;
        lastHeartbeatFlags_ = movementInfo.flags;
        lastHeartbeatTransportGuid_ = movementInfo.transportGuid;
    } else {
        lastNonHeartbeatMoveSendTimeMs_ = movementInfo.time;
    }
}

// ============================================================
// The active mover (stock 0x729010)
// ============================================================

uint64_t MovementHandler::activeMoverGuid() const {
    return moverGuid_ != 0 ? moverGuid_ : owner_.getPlayerGuid();
}

bool MovementHandler::movingOtherUnit() const {
    return moverGuid_ != 0 && moverGuid_ != owner_.getPlayerGuid();
}

MovementHandler::MoverState MovementHandler::captureMoverState() const {
    MoverState s;
    s.info = movementInfo;
    s.walk = serverWalkSpeed_;
    s.run = serverRunSpeed_;
    s.runBack = serverRunBackSpeed_;
    s.swim = serverSwimSpeed_;
    s.swimBack = serverSwimBackSpeed_;
    s.flight = serverFlightSpeed_;
    s.flightBack = serverFlightBackSpeed_;
    s.turn = serverTurnRate_;
    s.pitch = serverPitchRate_;
    s.allowed = serverMovementAllowed_;
    return s;
}

void MovementHandler::loadMoverState(const MoverState& s) {
    movementInfo = s.info;
    serverWalkSpeed_ = s.walk;
    serverRunSpeed_ = s.run;
    serverRunBackSpeed_ = s.runBack;
    serverSwimSpeed_ = s.swim;
    serverSwimBackSpeed_ = s.swimBack;
    serverFlightSpeed_ = s.flight;
    serverFlightBackSpeed_ = s.flightBack;
    serverTurnRate_ = s.turn;
    serverPitchRate_ = s.pitch;
    serverMovementAllowed_ = s.allowed;
}

MovementHandler::ForceTarget MovementHandler::forceTargetOf(uint64_t guid) const {
    if (guid == activeMoverGuid()) return ForceTarget::Mover;
    if (movingOtherUnit() && guid == owner_.getPlayerGuid()) return ForceTarget::KeptPlayer;
    return ForceTarget::None;
}

void MovementHandler::setActiveMover(uint64_t guid) {
    const uint64_t player = owner_.getPlayerGuid();
    if (guid == player) guid = 0;
    if (guid == moverGuid_) return;
    const uint64_t oldMover = activeMoverGuid();

    // The old mover stops where it is (0x6EE920): its direction keys go.
    constexpr uint32_t kSteer =
        static_cast<uint32_t>(MovementFlags::FORWARD) | static_cast<uint32_t>(MovementFlags::BACKWARD) |
        static_cast<uint32_t>(MovementFlags::STRAFE_LEFT) | static_cast<uint32_t>(MovementFlags::STRAFE_RIGHT) |
        static_cast<uint32_t>(MovementFlags::TURN_LEFT) | static_cast<uint32_t>(MovementFlags::TURN_RIGHT) |
        static_cast<uint32_t>(MovementFlags::PITCH_UP) | static_cast<uint32_t>(MovementFlags::PITCH_DOWN) |
        static_cast<uint32_t>(MovementFlags::ASCENDING) | static_cast<uint32_t>(MovementFlags::DESCENDING);
    movementInfo.flags &= ~kSteer;
    if (moverGuid_ == 0) {
        playerState_ = captureMoverState();
        // The player hands its movement over (0x729010): with no spline of
        // its own running, CMSG_MOVE_NOT_ACTIVE_MOVER with where it stands.
        auto self = owner_.getEntityManager().getEntity(player);
        const bool splineRunning = self && self->hasStockMotion() && self->stockMotionIsSpline();
        if (!splineRunning && owner_.getSocket() && owner_.getState() == WorldState::IN_WORLD &&
            wireOpcode(Opcode::CMSG_MOVE_NOT_ACTIVE_MOVER) != 0xFFFF) {
            network::Packet p(wireOpcode(Opcode::CMSG_MOVE_NOT_ACTIVE_MOVER));
            writeMoverMovement(p);
            owner_.getSocket()->send(p);
        }
    }

    if (guid == 0) {
        // The player again, where the seat had them: the vehicle's spot,
        // its facing. The exit spline that follows takes them off it.
        const glm::vec3 at(movementInfo.x, movementInfo.y, movementInfo.z);
        const float facing = movementInfo.orientation;
        loadMoverState(playerState_);
        movementInfo.x = at.x;
        movementInfo.y = at.y;
        movementInfo.z = at.z;
        movementInfo.orientation = facing;
        movementInfo.flags &= ~(kSteer | static_cast<uint32_t>(MovementFlags::FALLING));
    } else {
        // The unit's own movement and speeds, as its create block and the
        // packets since gave them: a vehicle moves at its own run speed and
        // turns at its own rate.
        MoverState v;
        v.info = MovementInfo{};
        v.info.x = movementInfo.x;
        v.info.y = movementInfo.y;
        v.info.z = movementInfo.z;
        v.info.orientation = movementInfo.orientation;
        if (auto e = owner_.getEntityManager().getEntity(guid)) {
            v.info.x = e->getX();
            v.info.y = e->getY();
            v.info.z = e->getZ();
            v.info.orientation = e->getOrientation();
            v.info.flags = e->baseMoveFlags() &
                (static_cast<uint32_t>(MovementFlags::WALKING) | static_cast<uint32_t>(MovementFlags::SWIMMING) |
                 static_cast<uint32_t>(MovementFlags::FLYING) | static_cast<uint32_t>(MovementFlags::CAN_FLY) |
                 static_cast<uint32_t>(MovementFlags::HOVER));
            v.info.flags2 = e->baseMoveFlags2() &
                static_cast<uint16_t>(~0x0400u);  // not the interpolation bits a packet set
            const auto& sp = e->moveSpeeds();
            v.walk = sp.walk; v.run = sp.run; v.runBack = sp.runBack;
            v.swim = sp.swim; v.swimBack = sp.swimBack;
            v.flight = sp.flight; v.flightBack = sp.flightBack;
            v.turn = sp.turnRate; v.pitch = sp.pitchRate;
        }
        v.allowed = true;
        loadMoverState(v);
    }
    isFalling_ = false;
    fallStartMs_ = 0;
    moverGuid_ = guid;
    hasLastPacketOrientation_ = false;
    timeSinceLastMoveHeartbeat_ = 0.0f;
    LOG_INFO("Active mover: 0x", std::hex, oldMover, " -> 0x", activeMoverGuid(), std::dec,
             " run=", serverRunSpeed_, " turn=", serverTurnRate_, " flags=0x", std::hex,
             movementInfo.flags, " flags2=0x", movementInfo.flags2, std::dec);
    if (owner_.getSocket() && owner_.getState() == WorldState::IN_WORLD) {
        owner_.getSocket()->send(SetActiveMoverPacket::build(activeMoverGuid()));
    }
    owner_.onActiveMoverChanged();
}

MovementInfo MovementHandler::moverWireInfo() {
    MovementInfo wire = movementInfo;
    wire.time = nextMovementTimestampMs();
    if (!movingOtherUnit() && wire.hasFlag(MovementFlags::ONTRANSPORT)) {
        wire.transportTime = wire.time;
        wire.transportTime2 = wire.time;
        const glm::vec3 t = core::coords::canonicalToServer(
            glm::vec3(wire.transportX, wire.transportY, wire.transportZ));
        wire.transportX = t.x;
        wire.transportY = t.y;
        wire.transportZ = t.z;
    } else {
        wire.flags &= ~static_cast<uint32_t>(MovementFlags::ONTRANSPORT);
    }
    const glm::vec3 p = core::coords::canonicalToServer(glm::vec3(wire.x, wire.y, wire.z));
    wire.x = p.x;
    wire.y = p.y;
    wire.z = p.z;
    wire.orientation = core::coords::canonicalToServerYaw(wire.orientation);
    return wire;
}

void MovementHandler::writeMoverMovement(network::Packet& packet) {
    packet.writePackedGuid(activeMoverGuid());
    const MovementInfo wire = moverWireInfo();
    if (owner_.getPacketParsers()) owner_.getPacketParsers()->writeMovementPayload(packet, wire);
    else MovementPacket::writeMovementPayload(packet, wire);
}

void MovementHandler::sendMoverPacket(Opcode opcode,
                                      const std::function<void(network::Packet&)>& extra) {
    if (!owner_.getSocket() || wireOpcode(opcode) == 0xFFFF) return;
    network::Packet packet(wireOpcode(opcode));
    writeMoverMovement(packet);
    if (extra) extra(packet);
    owner_.getSocket()->send(packet);
}

// ============================================================
// sanitizeMovementForTaxi
// ============================================================

void MovementHandler::sanitizeMovementForTaxi() {
    constexpr uint32_t kClearTaxiFlags =
        static_cast<uint32_t>(MovementFlags::FORWARD) |
        static_cast<uint32_t>(MovementFlags::BACKWARD) |
        static_cast<uint32_t>(MovementFlags::STRAFE_LEFT) |
        static_cast<uint32_t>(MovementFlags::STRAFE_RIGHT) |
        static_cast<uint32_t>(MovementFlags::TURN_LEFT) |
        static_cast<uint32_t>(MovementFlags::TURN_RIGHT) |
        static_cast<uint32_t>(MovementFlags::PITCH_UP) |
        static_cast<uint32_t>(MovementFlags::PITCH_DOWN) |
        static_cast<uint32_t>(MovementFlags::FALLING) |
        static_cast<uint32_t>(MovementFlags::FALLINGFAR) |
        static_cast<uint32_t>(MovementFlags::SWIMMING);

    movementInfo.flags &= ~kClearTaxiFlags;
    movementInfo.fallTime = 0;
    movementInfo.jumpVelocity = 0.0f;
    movementInfo.jumpSinAngle = 0.0f;
    movementInfo.jumpCosAngle = 0.0f;
    movementInfo.jumpXYSpeed = 0.0f;
    movementInfo.pitch = 0.0f;
}

// ============================================================
// forceClearTaxiAndMovementState
// ============================================================

void MovementHandler::forceClearTaxiAndMovementState() {
    taxiActivatePending_ = false;
    taxiActivateTimer_ = 0.0f;
    taxiClientActive_ = false;
    taxiRecoverPending_ = false;
    taxiStartGrace_ = 0.0f;
    onTaxiFlight_ = false;

    const bool clearingTaxiMount = taxiMountActive_;
    if (clearingTaxiMount && owner_.mountCallbackRef()) {
        owner_.mountCallbackRef()(0);
    }
    taxiMountActive_ = false;
    // A normal mount is owned by its aura/update-field path. Do not silently
    // zero it here without firing the mount callback: that leaves the rendered
    // mount behind and reconciliation spawns a duplicate. Unstuck explicitly
    // dismounts normal mounts before calling this movement reset.
    if (clearingTaxiMount) {
        owner_.currentMountDisplayIdRef() = 0;
    }
    owner_.vehicleIdRef() = 0;
    // Death/resurrect state is intentionally NOT cleared here.
    // Previously this method reset 10 death-related fields despite being named
    // "forceClearTaxiAndMovementState", which could cancel pending resurrections
    // or clear death state on taxi dismount. Death state is managed by
    // entity_controller (markPlayerDead) and the resurrect packet handlers.

    movementInfo.flags = 0;
    movementInfo.flags2 = 0;
    movementInfo.transportGuid = 0;
    owner_.clearPlayerTransport();

    if (owner_.getSocket() && owner_.getState() == WorldState::IN_WORLD) {
        sendMovement(Opcode::MSG_MOVE_STOP);
        sendMovement(Opcode::MSG_MOVE_STOP_STRAFE);
        sendMovement(Opcode::MSG_MOVE_STOP_TURN);
        sendMovement(Opcode::MSG_MOVE_STOP_SWIM);
        sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
    }

    LOG_INFO("Force-cleared taxi/movement state");
}

// ============================================================
// setPosition / setOrientation
// ============================================================

void MovementHandler::endWorldPortHold() {
    if (worldPortHold_) LOG_INFO("World port: the new map is placed, movement resumes");
    worldPortHold_ = false;
}

bool MovementHandler::isWorldPortHeld() const {
    if (!worldPortHold_) return false;
    constexpr auto kWorldPortHoldLimit = std::chrono::seconds(60);
    return std::chrono::steady_clock::now() - worldPortHoldSince_ < kWorldPortHoldLimit;
}

void MovementHandler::setPosition(float x, float y, float z) {
    // The renderer's position is the old map's until the world loader has
    // placed the character on the new one; taking it put the old coordinates
    // in the next packet, and the server moved the player there on the new
    // map (".gps" after ".tele" to another continent read the new map with
    // the old x, y, z).
    if (isWorldPortHeld()) return;
    movementInfo.x = x;
    movementInfo.y = y;
    movementInfo.z = z;
    // The player's own entity goes with it. The server never echoes the
    // player's movement back, so nothing else moved it: it stayed where the
    // player logged in, landed or was teleported, and every distance measured
    // from it - range on the action bar, IsSpellInRange, interact distance,
    // tab targeting - was measured from there.
    //
    // Not during a client taxi flight, where the flight simulation moves the
    // entity and the character is placed from it.
    if (taxiClientActive_ || onTaxiFlight_) return;
    if (auto player = owner_.getEntityManager().getEntity(owner_.getPlayerGuid())) {
        player->setPosition(x, y, z, movementInfo.orientation);
    }
}

void MovementHandler::setOrientation(float orientation) {
    if (isWorldPortHeld()) return;
    movementInfo.orientation = orientation;
}

// ============================================================
// dismount
// ============================================================

void MovementHandler::dismount() {
    if (!owner_.getSocket()) return;
    // The player's mount field goes on reading its old value for a few frames
    // after the request goes out. Believing it re-mounts the player behind their
    // back, and that stale value then makes the server's own SMSG_DISMOUNT look
    // transient and get thrown away - the mount blinked off, back on, and off
    // again over about two hundred milliseconds, with the character caught
    // holding the seated rider pose in between.
    constexpr float kDismountGraceSeconds = 1.5f;
    dismountGraceRemaining_ = kDismountGraceSeconds;
    uint32_t savedMountAura = owner_.mountAuraSpellIdRef();
    if (owner_.currentMountDisplayIdRef() != 0 || taxiMountActive_) {
        if (owner_.mountCallbackRef()) {
            owner_.mountCallbackRef()(0);
        }
        owner_.currentMountDisplayIdRef() = 0;
        taxiMountActive_ = false;
        owner_.mountAuraSpellIdRef() = 0;
        LOG_INFO("Dismount: cleared local mount state, onTaxiFlight=", onTaxiFlight_);
    }
    uint16_t cancelMountWire = wireOpcode(Opcode::CMSG_CANCEL_MOUNT_AURA);
    if (cancelMountWire != 0xFFFF) {
        network::Packet pkt(cancelMountWire);
        owner_.getSocket()->send(pkt);
        LOG_INFO("Sent CMSG_CANCEL_MOUNT_AURA");
    } else if (savedMountAura != 0) {
        auto pkt = CancelAuraPacket::build(savedMountAura);
        owner_.getSocket()->send(pkt);
        LOG_INFO("Sent CMSG_CANCEL_AURA (mount spell ", savedMountAura, ") - Classic fallback");
    } else {
        for (const auto& a : owner_.getPlayerAuras()) {
            if (!a.isEmpty() && a.maxDurationMs < 0 && a.casterGuid == owner_.getPlayerGuid()) {
                auto pkt = CancelAuraPacket::build(a.spellId);
                owner_.getSocket()->send(pkt);
                LOG_INFO("Sent CMSG_CANCEL_AURA (spell ", a.spellId, ") - brute force dismount");
            }
        }
    }
}

// ============================================================
// Force Speed / Root / Flag Change Handlers
// ============================================================

// Shared force-ACK packet builder. All server-forced movement changes (speed,
// root, flag, collision-height, knockback) require the same ACK structure:
// GUID + counter + movement payload with server-space coordinates. Centralised
// here so transport coordinate conversion can't diverge between handlers.
network::Packet MovementHandler::buildForceAck(Opcode ackOpcode, uint32_t counter) {
    return buildForceAckFor(ackOpcode, counter, activeMoverGuid(), movementInfo);
}

network::Packet MovementHandler::buildForceAckFor(Opcode ackOpcode, uint32_t counter, uint64_t guid,
                                                  const MovementInfo& info) {
    network::Packet ack(wireOpcode(ackOpcode));
    const bool legacyGuid =
        isActiveExpansion("classic") || isActiveExpansion("tbc") || isActiveExpansion("turtle");
    if (legacyGuid) {
        ack.writeUInt64(guid);
    } else {
        ack.writePackedGuid(guid);
    }
    ack.writeUInt32(counter);

    MovementInfo wire = info;
    wire.time = nextMovementTimestampMs();
    if (wire.hasFlag(MovementFlags::ONTRANSPORT)) {
        wire.transportTime = wire.time;
        wire.transportTime2 = wire.time;
    }
    glm::vec3 serverPos = core::coords::canonicalToServer(glm::vec3(wire.x, wire.y, wire.z));
    wire.x = serverPos.x;
    wire.y = serverPos.y;
    wire.z = serverPos.z;
    if (wire.hasFlag(MovementFlags::ONTRANSPORT)) {
        glm::vec3 serverTransport =
            core::coords::canonicalToServer(glm::vec3(wire.transportX, wire.transportY, wire.transportZ));
        wire.transportX = serverTransport.x;
        wire.transportY = serverTransport.y;
        wire.transportZ = serverTransport.z;
    }
    if (owner_.getPacketParsers()) {
        owner_.getPacketParsers()->writeMovementPayload(ack, wire);
    } else {
        MovementPacket::writeMovementPayload(ack, wire);
    }
    return ack;
}

void MovementHandler::handleForceSpeedChange(network::Packet& packet, const char* name,
                                              Opcode ackOpcode, float* speedStorage) {
    const bool fscTbcLike = isPreWotlk();
    uint64_t guid = fscTbcLike
        ? packet.readUInt64() : packet.readPackedGuid();
    uint32_t counter = packet.readUInt32();

    size_t remaining = packet.getRemainingSize();
    if (remaining >= 8) {
        packet.readUInt32();
    } else if (remaining >= 5) {
        packet.readUInt8();
    }
    float newSpeed = packet.readFloat();

    LOG_INFO("SMSG_FORCE_", name, "_CHANGE: guid=0x", std::hex, guid, std::dec,
             " counter=", counter, " speed=", newSpeed);

    // The active mover's (a driven vehicle's included), or the player's own
    // while it drives something else (kept aside, acknowledged as its own).
    const ForceTarget target = forceTargetOf(guid);
    if (target == ForceTarget::None) return;

    // Validate BEFORE sending ACK - if we echo a bad speed back to the server
    // but don't apply it locally, the client and server desync on movement speed.
    if (std::isnan(newSpeed) || newSpeed < 0.1f || newSpeed > 100.0f) {
        LOG_WARNING("Ignoring invalid ", name, " speed: ", newSpeed);
        return;
    }

    if (owner_.getSocket()) {
        auto ack = target == ForceTarget::Mover
            ? buildForceAck(ackOpcode, counter)
            : buildForceAckFor(ackOpcode, counter, guid, playerState_.info);
        ack.writeFloat(newSpeed);
        owner_.getSocket()->send(ack);
    }

    if (!speedStorage) return;
    if (target == ForceTarget::Mover) {
        *speedStorage = newSpeed;
    } else {
        float* kept = speedStorage == &serverWalkSpeed_ ? &playerState_.walk
                    : speedStorage == &serverRunSpeed_ ? &playerState_.run
                    : speedStorage == &serverRunBackSpeed_ ? &playerState_.runBack
                    : speedStorage == &serverSwimSpeed_ ? &playerState_.swim
                    : speedStorage == &serverSwimBackSpeed_ ? &playerState_.swimBack
                    : speedStorage == &serverFlightSpeed_ ? &playerState_.flight
                    : speedStorage == &serverFlightBackSpeed_ ? &playerState_.flightBack
                    : speedStorage == &serverTurnRate_ ? &playerState_.turn
                    : speedStorage == &serverPitchRate_ ? &playerState_.pitch : nullptr;
        if (kept) *kept = newSpeed;
    }
}

void MovementHandler::handleForceRunSpeedChange(network::Packet& packet) {
    // A speed change is not a dismount signal. Mount ownership is updated by
    // SMSG_DISMOUNT / UNIT_FIELD_MOUNTDISPLAYID instead.
    handleForceSpeedChange(packet, "RUN_SPEED", Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK, &serverRunSpeed_);
}

void MovementHandler::handleForceMoveRootState(network::Packet& packet, bool rooted) {
    const bool rootTbc = isPreWotlk();
    if (packet.getRemainingSize() < (rootTbc ? 8u : 2u)) return;
    uint64_t guid = rootTbc
        ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(4)) return;
    uint32_t counter = packet.readUInt32();

    LOG_INFO(rooted ? "SMSG_FORCE_MOVE_ROOT" : "SMSG_FORCE_MOVE_UNROOT",
             ": guid=0x", std::hex, guid, std::dec, " counter=", counter);

    const ForceTarget target = forceTargetOf(guid);
    if (target == ForceTarget::None) return;
    MovementInfo& info = target == ForceTarget::Mover ? movementInfo : playerState_.info;

    if (rooted) {
        info.flags |= static_cast<uint32_t>(MovementFlags::ROOT);
    } else {
        info.flags &= ~static_cast<uint32_t>(MovementFlags::ROOT);
    }

    if (!owner_.getSocket()) return;
    Opcode ackOp = rooted ? Opcode::CMSG_FORCE_MOVE_ROOT_ACK : Opcode::CMSG_FORCE_MOVE_UNROOT_ACK;
    if (wireOpcode(ackOp) == 0xFFFF) return;
    owner_.getSocket()->send(target == ForceTarget::Mover ? buildForceAck(ackOp, counter)
                                                          : buildForceAckFor(ackOp, counter, guid, info));
}

void MovementHandler::handleForceMoveFlagChange(network::Packet& packet, const char* name,
                                                  Opcode ackOpcode, uint32_t flag, bool set) {
    const bool fmfTbcLike = isPreWotlk();
    if (packet.getRemainingSize() < (fmfTbcLike ? 8u : 2u)) return;
    uint64_t guid = fmfTbcLike
        ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(4)) return;
    uint32_t counter = packet.readUInt32();

    LOG_INFO("SMSG_FORCE_", name, ": guid=0x", std::hex, guid, std::dec, " counter=", counter);

    const ForceTarget target = forceTargetOf(guid);
    if (target == ForceTarget::None) return;
    MovementInfo& info = target == ForceTarget::Mover ? movementInfo : playerState_.info;

    if (flag != 0) {
        if (set) {
            info.flags |= flag;
        } else {
            info.flags &= ~flag;
        }
    }
    // Flight taken away in the air ends it there (movement event 0x30 ->
    // 0x98C240 clears FLYING, ASCENDING, DESCENDING and the pitch keys before
    // the ack). Left set, FLYING outlived CAN_FLY for good: the landing that
    // clears it (setFlightAirborne) only runs while flight is allowed.
    if (!set && flag == static_cast<uint32_t>(MovementFlags::CAN_FLY)) {
        info.flags &= ~(static_cast<uint32_t>(MovementFlags::FLYING) |
                        static_cast<uint32_t>(MovementFlags::ASCENDING) |
                        static_cast<uint32_t>(MovementFlags::DESCENDING) |
                        static_cast<uint32_t>(MovementFlags::PITCH_UP) |
                        static_cast<uint32_t>(MovementFlags::PITCH_DOWN));
    }

    if (!owner_.getSocket()) return;
    if (wireOpcode(ackOpcode) == 0xFFFF) return;
    auto ack = target == ForceTarget::Mover ? buildForceAck(ackOpcode, counter)
                                            : buildForceAckFor(ackOpcode, counter, guid, info);
    // The flag-change acks end with whether the change applied (the server's
    // HandleMoveFlagChangeOpcode reads a uint32 after the movement block for
    // all of them but the two gravity acks). Without it every water-walk,
    // hover, feather-fall and can-fly ack was a ByteBufferException and was
    // skipped: the ghost's water walking on release was never acknowledged,
    // nor a flying mount's can-fly (its pending flight change never cleared).
    if (ackOpcode != Opcode::CMSG_MOVE_GRAVITY_DISABLE_ACK &&
        ackOpcode != Opcode::CMSG_MOVE_GRAVITY_ENABLE_ACK) {
        ack.writeUInt32(set ? 1u : 0u);
    }
    owner_.getSocket()->send(ack);
}

void MovementHandler::handleMoveSetCollisionHeight(network::Packet& packet) {
    const bool legacyGuid = isPreWotlk();
    if (packet.getRemainingSize() < (legacyGuid ? 8u : 2u)) return;
    uint64_t guid = legacyGuid ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(8)) return;
    uint32_t counter = packet.readUInt32();
    float height = packet.readFloat();

    LOG_INFO("SMSG_MOVE_SET_COLLISION_HGT: guid=0x", std::hex, guid, std::dec,
             " counter=", counter, " height=", height);

    if (forceTargetOf(guid) != ForceTarget::Mover) return;
    if (!owner_.getSocket()) return;

    if (wireOpcode(Opcode::CMSG_MOVE_SET_COLLISION_HGT_ACK) == 0xFFFF) return;
    // buildForceAck now handles transport coordinate conversion, fixing the
    // previous omission that caused desync when riding boats/zeppelins.
    auto ack = buildForceAck(Opcode::CMSG_MOVE_SET_COLLISION_HGT_ACK, counter);
    ack.writeFloat(height);
    owner_.getSocket()->send(ack);
}

void MovementHandler::handleMoveKnockBack(network::Packet& packet) {
    const bool mkbTbc = isPreWotlk();
    if (packet.getRemainingSize() < (mkbTbc ? 8u : 2u)) return;
    uint64_t guid = mkbTbc
        ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(20)) return;
    uint32_t counter = packet.readUInt32();
    float vcos    = packet.readFloat();
    float vsin    = packet.readFloat();
    float hspeed  = packet.readFloat();
    float vspeed  = packet.readFloat();

    LOG_INFO("SMSG_MOVE_KNOCK_BACK: guid=0x", std::hex, guid, std::dec,
             " counter=", counter, " vcos=", vcos, " vsin=", vsin,
             " hspeed=", hspeed, " vspeed=", vspeed);

    if (forceTargetOf(guid) != ForceTarget::Mover) return;

    if (owner_.knockBackCallbackRef()) {
        owner_.knockBackCallbackRef()(vcos, vsin, hspeed, vspeed);
    }

    if (!owner_.getSocket()) return;
    if (wireOpcode(Opcode::CMSG_MOVE_KNOCK_BACK_ACK) == 0xFFFF) return;
    owner_.getSocket()->send(buildForceAck(Opcode::CMSG_MOVE_KNOCK_BACK_ACK, counter));
}

// ============================================================
// Other Player / Creature Movement Handlers
// ============================================================

void MovementHandler::handleMoveSetSpeed(network::Packet& packet) {
    const bool useFull = isPreWotlk();
    uint64_t moverGuid = useFull
        ? packet.readUInt64() : packet.readPackedGuid();

    const size_t remaining = packet.getRemainingSize();
    if (remaining < 4) return;
    if (remaining > 4) {
        packet.setReadPos(packet.getSize() - 4);
    }

    float speed = packet.readFloat();
    if (!std::isfinite(speed) || speed <= 0.01f || speed > 200.0f) return;

    const uint16_t wireOp = packet.getOpcode();
    if (moverGuid != activeMoverGuid()) {
        // Someone else's: their speed drives their motion and animation.
        SpeedKind kind = SpeedKind::Run;
        if      (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_RUN_BACK_SPEED))    kind = SpeedKind::RunBack;
        else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_WALK_SPEED))        kind = SpeedKind::Walk;
        else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_SWIM_SPEED))        kind = SpeedKind::Swim;
        else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_SWIM_BACK_SPEED))   kind = SpeedKind::SwimBack;
        else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_FLIGHT_SPEED))      kind = SpeedKind::Flight;
        else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_FLIGHT_BACK_SPEED)) kind = SpeedKind::FlightBack;
        applyRemoteSpeed(owner_, moverGuid, kind, speed);
        return;
    }
    if      (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_RUN_SPEED))        serverRunSpeed_      = speed;
    else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_RUN_BACK_SPEED))   serverRunBackSpeed_  = speed;
    else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_WALK_SPEED))       serverWalkSpeed_     = speed;
    else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_SWIM_SPEED))       serverSwimSpeed_     = speed;
    else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_SWIM_BACK_SPEED))  serverSwimBackSpeed_ = speed;
    else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_FLIGHT_SPEED))     serverFlightSpeed_   = speed;
    else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_FLIGHT_BACK_SPEED))serverFlightBackSpeed_= speed;
}

void MovementHandler::handleOtherPlayerMovement(network::Packet& packet) {
    // Same packed-guid mismatch found and fixed for MSG_MOVE_TELEPORT_ACK:
    // CMaNGOS sends a packed guid here on TBC too, not a raw UInt64. Reading
    // it as raw corrupted moverGuid, so getEntity(moverGuid) below always
    // missed - the entity still existed (created fine), but its position
    // never updated from these packets. Live-observed as another player's
    // tracked position staying frozen at wherever they were first spotted,
    // even while they kept walking.
    uint64_t moverGuid = packet.readPackedGuid();
    if (moverGuid == owner_.getPlayerGuid() || moverGuid == activeMoverGuid() || moverGuid == 0) {
        return;
    }

    // WotLK: the stock client's own handling (Wow.exe 0x740D30 -> 0x6ED990):
    // the packet is applied when its sender spaced it, and the unit runs on
    // its flags and speeds in between. See game/unit_motion_glue.hpp.
    if (isActiveExpansion("wotlk")) {
        MovementInfo stockInfo = {};
        if (!readMovementBlockWotlk(packet, stockInfo)) return;
        auto mover = owner_.getEntityManager().getEntity(moverGuid);
        if (!mover) return;
        const bool onTransport =
            (stockInfo.flags & static_cast<uint32_t>(MovementFlags::ONTRANSPORT)) != 0 &&
            stockInfo.transportGuid != 0 && owner_.getTransportManager() != nullptr;
        // Its flags and its jump reach the animation when the packet takes
        // effect (processStockMotionFrame), not when it arrives.
        applyRemoteMovement(owner_, *mover, stockInfo, onTransport);
        return;
    }

    MovementInfo info = {};
    info.flags = packet.readUInt32();
    uint8_t flags2Size = owner_.getPacketParsers() ? owner_.getPacketParsers()->movementFlags2Size() : 2;
    if (flags2Size == 2) info.flags2 = packet.readUInt16();
    else if (flags2Size == 1) info.flags2 = packet.readUInt8();
    info.time = packet.readUInt32();
    info.x = packet.readFloat();
    info.y = packet.readFloat();
    info.z = packet.readFloat();
    info.orientation = packet.readFloat();

    const uint32_t wireTransportFlag = owner_.getPacketParsers() ? owner_.getPacketParsers()->wireOnTransportFlag() : 0x00000200;
    const bool onTransport = (info.flags & wireTransportFlag) != 0;
    uint64_t transportGuid = 0;
    float tLocalX = 0, tLocalY = 0, tLocalZ = 0, tLocalO = 0;
    if (onTransport) {
        transportGuid = packet.readPackedGuid();
        tLocalX = packet.readFloat();
        tLocalY = packet.readFloat();
        tLocalZ = packet.readFloat();
        tLocalO = packet.readFloat();
        if (flags2Size >= 1) {
            /*uint32_t transportTime =*/ packet.readUInt32();
        }
        if (flags2Size >= 2) {
            /*int8_t transportSeat =*/ packet.readUInt8();
            if (info.flags2 & 0x0200) {
                /*uint32_t transportTime2 =*/ packet.readUInt32();
            }
        }
    }

    auto entity = owner_.getEntityManager().getEntity(moverGuid);
    if (!entity) {
        return;
    }

    glm::vec3 canonical = core::coords::serverToCanonical(glm::vec3(info.x, info.y, info.z));
    float canYaw = core::coords::serverToCanonicalYaw(info.orientation);

    if (onTransport && transportGuid != 0 && owner_.getTransportManager()) {
        glm::vec3 localCanonical = owner_.getTransportManager()->serverToTransportLocal(
            transportGuid, glm::vec3(tLocalX, tLocalY, tLocalZ));
        owner_.setTransportAttachment(moverGuid, entity->getType(), transportGuid, localCanonical, true,
                               core::coords::serverToCanonicalYaw(tLocalO));
        glm::vec3 worldPos = owner_.getTransportManager()->getPlayerWorldPosition(transportGuid, localCanonical);
        canonical = worldPos;
    } else if (!onTransport) {
        owner_.clearTransportAttachment(moverGuid);
    }

    uint32_t durationMs = 120;
    auto itPrev = otherPlayerMoveTimeMs_.find(moverGuid);
    if (itPrev != otherPlayerMoveTimeMs_.end()) {
        uint32_t rawDt = info.time - itPrev->second;
        if (rawDt >= 20 && rawDt <= 2000) {
            float fDt = static_cast<float>(rawDt);
            auto& smoothed = otherPlayerSmoothedIntervalMs_[moverGuid];
            if (smoothed < 1.0f) smoothed = fDt;
            smoothed = 0.7f * smoothed + 0.3f * fDt;
            float clamped = std::max(60.0f, std::min(500.0f, smoothed));
            durationMs = static_cast<uint32_t>(clamped);
        }
    }
    otherPlayerMoveTimeMs_[moverGuid] = info.time;

    const uint16_t wireOp = packet.getOpcode();
    const bool isStopOpcode =
        (wireOp == wireOpcode(Opcode::MSG_MOVE_STOP)) ||
        (wireOp == wireOpcode(Opcode::MSG_MOVE_STOP_STRAFE)) ||
        (wireOp == wireOpcode(Opcode::MSG_MOVE_STOP_TURN)) ||
        (wireOp == wireOpcode(Opcode::MSG_MOVE_STOP_SWIM)) ||
        (wireOp == wireOpcode(Opcode::MSG_MOVE_FALL_LAND));
    const bool isJumpOpcode  = (wireOp == wireOpcode(Opcode::MSG_MOVE_JUMP));

    // Whether the unit is still travelling after this packet: the low four
    // movement flags (forward, backward, strafe left, strafe right) mean the
    // same in every expansion. A stop of turning or of strafing while still
    // running forward is not a stop, and used to halt the model mid-run.
    const bool stillTranslating = (info.flags & 0x0Fu) != 0;
    const bool halts = isStopOpcode && !stillTranslating;

    float entityDuration = durationMs / 1000.0f;
    if (halts) {
        // Glide to where it stopped rather than appear there. The model runs
        // about one packet interval behind the server, so the stop position
        // is up to a few yards ahead of it, and the snap at every stop was
        // the jump players saw. Travelled at the unit's own recent speed,
        // capped short; a real disagreement (6+ yards) still snaps.
        const float sdx = canonical.x - entity->getX();
        const float sdy = canonical.y - entity->getY();
        const float sdz = canonical.z - entity->getZ();
        const float dist = std::sqrt(sdx * sdx + sdy * sdy + sdz * sdz);
        const float speed = std::max(entity->planarSpeed(), 2.5f);
        entityDuration = dist < 6.0f ? std::min(dist / speed, 0.35f) : 0.0f;
    }
    // A sample of a unit in motion is dead-reckoned past if the next one is
    // late, instead of halting at it and jumping on; see Entity::startMoveTo.
    entity->startMoveTo(canonical.x, canonical.y, canonical.z, canYaw, entityDuration,
                        /*deadReckon=*/stillTranslating);

    if (owner_.creatureMoveCallbackRef()) {
        // The same glide for the renderer: a zero here set the instance at
        // the stop position outright, a frame before the sync loop pulled it
        // back toward the entity.
        const uint32_t notifyDuration = static_cast<uint32_t>(entityDuration * 1000.0f);
        owner_.creatureMoveCallbackRef()(moverGuid, canonical.x, canonical.y, canonical.z, notifyDuration);
    }

    if (owner_.unitAnimHintCallbackRef() && isJumpOpcode) {
        owner_.unitAnimHintCallbackRef()(moverGuid, 38u);
    }

    if (owner_.unitMoveFlagsCallbackRef()) {
        owner_.unitMoveFlagsCallbackRef()(moverGuid, info.flags, ~0u);
    }
}

void MovementHandler::handleCompressedMoves(network::Packet& packet) {
    std::vector<uint8_t> decompressedStorage;
    const std::vector<uint8_t>* dataPtr = &packet.getData();

    const auto& rawData = packet.getData();
    const bool hasCompressedWrapper =
        rawData.size() >= 6 &&
        rawData[4] == 0x78 &&
        (rawData[5] == 0x01 || rawData[5] == 0x9C ||
         rawData[5] == 0xDA || rawData[5] == 0x5E);
    if (hasCompressedWrapper) {
        uint32_t decompressedSize = static_cast<uint32_t>(rawData[0]) |
                                    (static_cast<uint32_t>(rawData[1]) << 8) |
                                    (static_cast<uint32_t>(rawData[2]) << 16) |
                                    (static_cast<uint32_t>(rawData[3]) << 24);
        if (decompressedSize == 0 || decompressedSize > 65536) {
            LOG_WARNING("SMSG_COMPRESSED_MOVES: bad decompressedSize=", decompressedSize);
            return;
        }

        decompressedStorage.resize(decompressedSize);
        uLongf destLen = decompressedSize;
        int ret = uncompress(decompressedStorage.data(), &destLen,
                             rawData.data() + 4, rawData.size() - 4);
        if (ret != Z_OK) {
            LOG_WARNING("SMSG_COMPRESSED_MOVES: zlib error ", ret);
            return;
        }

        decompressedStorage.resize(destLen);
        dataPtr = &decompressedStorage;
    }

    const auto& data = *dataPtr;
    const size_t dataLen = data.size();

    uint16_t monsterMoveWire          = wireOpcode(Opcode::SMSG_MONSTER_MOVE);
    uint16_t monsterMoveTransportWire = wireOpcode(Opcode::SMSG_MONSTER_MOVE_TRANSPORT);

    const std::array<uint16_t, 29> kMoveOpcodes = {
        wireOpcode(Opcode::MSG_MOVE_START_FORWARD),
        wireOpcode(Opcode::MSG_MOVE_START_BACKWARD),
        wireOpcode(Opcode::MSG_MOVE_STOP),
        wireOpcode(Opcode::MSG_MOVE_START_STRAFE_LEFT),
        wireOpcode(Opcode::MSG_MOVE_START_STRAFE_RIGHT),
        wireOpcode(Opcode::MSG_MOVE_STOP_STRAFE),
        wireOpcode(Opcode::MSG_MOVE_JUMP),
        wireOpcode(Opcode::MSG_MOVE_START_TURN_LEFT),
        wireOpcode(Opcode::MSG_MOVE_START_TURN_RIGHT),
        wireOpcode(Opcode::MSG_MOVE_STOP_TURN),
        wireOpcode(Opcode::MSG_MOVE_SET_FACING),
        wireOpcode(Opcode::MSG_MOVE_FALL_LAND),
        wireOpcode(Opcode::MSG_MOVE_HEARTBEAT),
        wireOpcode(Opcode::MSG_MOVE_START_SWIM),
        wireOpcode(Opcode::MSG_MOVE_STOP_SWIM),
        wireOpcode(Opcode::MSG_MOVE_SET_WALK_MODE),
        wireOpcode(Opcode::MSG_MOVE_SET_RUN_MODE),
        wireOpcode(Opcode::MSG_MOVE_START_PITCH_UP),
        wireOpcode(Opcode::MSG_MOVE_START_PITCH_DOWN),
        wireOpcode(Opcode::MSG_MOVE_STOP_PITCH),
        wireOpcode(Opcode::MSG_MOVE_START_ASCEND),
        wireOpcode(Opcode::MSG_MOVE_STOP_ASCEND),
        wireOpcode(Opcode::MSG_MOVE_START_DESCEND),
        wireOpcode(Opcode::MSG_MOVE_SET_PITCH),
        wireOpcode(Opcode::MSG_MOVE_GRAVITY_CHNG),
        wireOpcode(Opcode::MSG_MOVE_UPDATE_CAN_FLY),
        wireOpcode(Opcode::MSG_MOVE_UPDATE_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY),
        wireOpcode(Opcode::MSG_MOVE_ROOT),
        wireOpcode(Opcode::MSG_MOVE_UNROOT),
    };

    struct CompressedMoveSubPacket {
        uint16_t opcode = 0;
        std::vector<uint8_t> payload;
    };
    struct DecodeResult {
        bool ok = false;
        bool overrun = false;
        bool usedPayloadOnlySize = false;
        size_t endPos = 0;
        size_t recognizedCount = 0;
        size_t subPacketCount = 0;
        std::vector<CompressedMoveSubPacket> packets;
    };

    auto isRecognizedSubOpcode = [&](uint16_t subOpcode) {
        return subOpcode == monsterMoveWire ||
               subOpcode == monsterMoveTransportWire ||
               std::find(kMoveOpcodes.begin(), kMoveOpcodes.end(), subOpcode) != kMoveOpcodes.end();
    };

    auto decodeSubPackets = [&](bool payloadOnlySize) -> DecodeResult {
        DecodeResult result;
        result.usedPayloadOnlySize = payloadOnlySize;
        size_t pos = 0;
        while (pos < dataLen) {
            if (pos + 1 > dataLen) break;
            uint8_t subSize = data[pos];
            if (subSize == 0) {
                result.ok = true;
                result.endPos = pos + 1;
                return result;
            }

            const size_t payloadLen = payloadOnlySize
                ? static_cast<size_t>(subSize)
                : (subSize >= 2 ? static_cast<size_t>(subSize) - 2 : 0);
            if (!payloadOnlySize && subSize < 2) {
                result.endPos = pos;
                return result;
            }

            const size_t packetLen = 1 + 2 + payloadLen;
            if (pos + packetLen > dataLen) {
                result.overrun = true;
                result.endPos = pos;
                return result;
            }

            uint16_t subOpcode = static_cast<uint16_t>(data[pos + 1]) |
                                 (static_cast<uint16_t>(data[pos + 2]) << 8);
            size_t payloadStart = pos + 3;

            CompressedMoveSubPacket subPacket;
            subPacket.opcode = subOpcode;
            subPacket.payload.assign(data.begin() + payloadStart,
                                     data.begin() + payloadStart + payloadLen);
            result.packets.push_back(std::move(subPacket));
            ++result.subPacketCount;
            if (isRecognizedSubOpcode(subOpcode)) {
                ++result.recognizedCount;
            }

            pos += packetLen;
        }
        result.ok = (result.endPos == 0 || result.endPos == dataLen);
        result.endPos = dataLen;
        return result;
    };

    DecodeResult decoded = decodeSubPackets(false);
    if (!decoded.ok || decoded.overrun) {
        DecodeResult payloadOnlyDecoded = decodeSubPackets(true);
        const bool preferPayloadOnly =
            payloadOnlyDecoded.ok &&
            (!decoded.ok || decoded.overrun || payloadOnlyDecoded.recognizedCount > decoded.recognizedCount);
        if (preferPayloadOnly) {
            decoded = std::move(payloadOnlyDecoded);
            static uint32_t payloadOnlyFallbackCount = 0;
            ++payloadOnlyFallbackCount;
            if (payloadOnlyFallbackCount <= 10 || (payloadOnlyFallbackCount % 100) == 0) {
                LOG_WARNING("SMSG_COMPRESSED_MOVES decoded via payload-only size fallback",
                            " (occurrence=", payloadOnlyFallbackCount, ")");
            }
        }
    }

    if (!decoded.ok || decoded.overrun) {
        LOG_WARNING("SMSG_COMPRESSED_MOVES: sub-packet overruns buffer at pos=", decoded.endPos);
        return;
    }

    std::unordered_set<uint16_t> unhandledSeen;

    for (const auto& entry : decoded.packets) {
        network::Packet subPacket(entry.opcode, entry.payload);

        if (entry.opcode == monsterMoveWire) {
            handleMonsterMove(subPacket);
        } else if (entry.opcode == monsterMoveTransportWire) {
            handleMonsterMoveTransport(subPacket);
        } else if (owner_.getState() == WorldState::IN_WORLD &&
                   std::find(kMoveOpcodes.begin(), kMoveOpcodes.end(), entry.opcode) != kMoveOpcodes.end()) {
            handleOtherPlayerMovement(subPacket);
        } else {
            if (unhandledSeen.insert(entry.opcode).second) {
                LOG_INFO("SMSG_COMPRESSED_MOVES: unhandled sub-opcode 0x",
                         std::hex, entry.opcode, std::dec, " payloadLen=", entry.payload.size());
            }
        }
    }
}

// ============================================================
// Monster Move Handlers
// ============================================================

void MovementHandler::handleMonsterMove(network::Packet& packet) {
    if (isActiveExpansion("classic") || isActiveExpansion("turtle")) {
        constexpr uint32_t kMaxMonsterMovesPerTick = 256;
        ++monsterMovePacketsThisTick_;
        if (monsterMovePacketsThisTick_ > kMaxMonsterMovesPerTick) {
            ++monsterMovePacketsDroppedThisTick_;
            if (monsterMovePacketsDroppedThisTick_ <= 3 ||
                (monsterMovePacketsDroppedThisTick_ % 100) == 0) {
                LOG_WARNING("SMSG_MONSTER_MOVE: per-tick cap exceeded, dropping packet",
                            " (processed=", monsterMovePacketsThisTick_,
                            " dropped=", monsterMovePacketsDroppedThisTick_, ")");
            }
            return;
        }
    }

    MonsterMoveData data;
    auto logMonsterMoveParseFailure = [&](const std::string& msg) {
        static uint32_t failCount = 0;
        ++failCount;
        if (failCount <= 10 || (failCount % 100) == 0) {
            LOG_WARNING(msg, " (occurrence=", failCount, ")");
        }
    };
    auto logWrappedUncompressedFallbackUsed = [&]() {
        static uint32_t wrappedUncompressedFallbackCount = 0;
        ++wrappedUncompressedFallbackCount;
        if (wrappedUncompressedFallbackCount <= 10 || (wrappedUncompressedFallbackCount % 100) == 0) {
            LOG_WARNING("SMSG_MONSTER_MOVE parsed via uncompressed wrapped-subpacket fallback",
                        " (occurrence=", wrappedUncompressedFallbackCount, ")");
        }
    };
    auto stripWrappedSubpacket = [&](const std::vector<uint8_t>& bytes, std::vector<uint8_t>& stripped) -> bool {
        if (bytes.size() < 3) return false;
        uint8_t subSize = bytes[0];
        if (subSize < 2) return false;
        size_t wrappedLen = static_cast<size_t>(subSize) + 1;
        if (wrappedLen != bytes.size()) return false;
        size_t payloadLen = static_cast<size_t>(subSize) - 2;
        if (3 + payloadLen > bytes.size()) return false;
        stripped.assign(bytes.begin() + 3, bytes.begin() + 3 + payloadLen);
        return true;
    };

    const auto& rawData = packet.getData();
    const bool allowTurtleMoveCompression = isActiveExpansion("turtle");
    bool isCompressed = allowTurtleMoveCompression &&
                        rawData.size() >= 6 &&
                        rawData[4] == 0x78 &&
                        (rawData[5] == 0x01 || rawData[5] == 0x9C ||
                         rawData[5] == 0xDA || rawData[5] == 0x5E);
    if (isCompressed) {
        uint32_t decompSize = static_cast<uint32_t>(rawData[0]) |
                              (static_cast<uint32_t>(rawData[1]) << 8) |
                              (static_cast<uint32_t>(rawData[2]) << 16) |
                              (static_cast<uint32_t>(rawData[3]) << 24);
        if (decompSize == 0 || decompSize > 65536) {
            LOG_WARNING("SMSG_MONSTER_MOVE: bad decompSize=", decompSize);
            return;
        }
        std::vector<uint8_t> decompressed(decompSize);
        uLongf destLen = decompSize;
        int ret = uncompress(decompressed.data(), &destLen,
                             rawData.data() + 4, rawData.size() - 4);
        if (ret != Z_OK) {
            LOG_WARNING("SMSG_MONSTER_MOVE: zlib error ", ret);
            return;
        }
        decompressed.resize(destLen);
        std::vector<uint8_t> stripped;
        bool hasWrappedForm = stripWrappedSubpacket(decompressed, stripped);

        bool parsed = false;
        if (hasWrappedForm) {
            network::Packet wrappedPacket(packet.getOpcode(), stripped);
            if (owner_.getPacketParsers()->parseMonsterMove(wrappedPacket, data)) {
                parsed = true;
            }
        }
        if (!parsed) {
            network::Packet decompPacket(packet.getOpcode(), decompressed);
            if (owner_.getPacketParsers()->parseMonsterMove(decompPacket, data)) {
                parsed = true;
            }
        }

        if (!parsed) {
            if (hasWrappedForm) {
                logMonsterMoveParseFailure("Failed to parse SMSG_MONSTER_MOVE (decompressed " +
                                           std::to_string(destLen) + " bytes, wrapped payload " +
                                           std::to_string(stripped.size()) + " bytes)");
            } else {
                logMonsterMoveParseFailure("Failed to parse SMSG_MONSTER_MOVE (decompressed " +
                                           std::to_string(destLen) + " bytes)");
            }
            return;
        }
    } else if (!owner_.getPacketParsers()->parseMonsterMove(packet, data)) {
        std::vector<uint8_t> stripped;
        if (stripWrappedSubpacket(rawData, stripped)) {
            network::Packet wrappedPacket(packet.getOpcode(), stripped);
            if (owner_.getPacketParsers()->parseMonsterMove(wrappedPacket, data)) {
                logWrappedUncompressedFallbackUsed();
            } else {
                logMonsterMoveParseFailure("Failed to parse SMSG_MONSTER_MOVE");
                return;
            }
        } else {
            logMonsterMoveParseFailure("Failed to parse SMSG_MONSTER_MOVE");
            return;
        }
    }

    auto entity = owner_.getEntityManager().getEntity(data.guid);
    if (!entity) {
        return;
    }

    // WotLK: the stock client's spline (Wow.exe 0x73C8E0): it starts where
    // this client has the unit, runs linearly or as Catmull-Rom by its
    // flags, faces along the path and turns at the end, jumps and falls.
    if (isActiveExpansion("wotlk") && entity->isUnit()) {
        applyStockMonsterMove(owner_, *entity, data);
        return;
    }

    // Classic/TBC creature patrols encode ground locomotion in the spline:
    // RUNMODE set means Run, while a clear bit means Walk. Publish it before
    // the render callback. WotLK uses the separate spline mode opcodes already
    // registered above, and 0x100 means DONE there, so do not reinterpret it.
    const bool usesPreWotlkSplineFlags = isActiveExpansion("classic") ||
                                          isActiveExpansion("turtle") ||
                                          isActiveExpansion("tbc");
    if (usesPreWotlkSplineFlags && entity->getType() == ObjectType::UNIT &&
        owner_.unitMoveFlagsCallbackRef()) {
        const uint32_t locomotionFlags =
            isPreWotlkSplineWalking(data.splineFlags)
                ? static_cast<uint32_t>(MovementFlags::WALKING)
                : 0u;
        owner_.unitMoveFlagsCallbackRef()(data.guid, locomotionFlags, ~0u);
    } else if (!usesPreWotlkSplineFlags && entity->getType() == ObjectType::UNIT &&
               owner_.unitMoveFlagsCallbackRef() && data.hasDest) {
        // WotLK: walk or run is the pace of the move. AzerothCore's monster
        // moves carry no Walkmode bit (every one arrives with flags 0), and
        // SET_WALK_MODE only comes when the mode flips in view, so patrols
        // that spawned walking played Run for their whole path. The path
        // length over the spline duration says which it is: stock walk speed
        // is 2.5 yd/s and run 7, so anything under the midpoint walks.
        const uint32_t walk = static_cast<uint32_t>(MovementFlags::WALKING);
        bool walking = (data.splineFlags & SplineFlagWotlk::WALKMODE) != 0;
        if (!walking && data.duration > 0) {
            glm::vec3 prev(data.x, data.y, data.z);
            float length = 0.0f;
            for (const auto& wp : data.waypoints) {
                const glm::vec3 p(wp.x, wp.y, wp.z);
                length += glm::length(p - prev);
                prev = p;
            }
            length += glm::length(glm::vec3(data.destX, data.destY, data.destZ) - prev);
            const float speed = length / (static_cast<float>(data.duration) / 1000.0f);
            constexpr float kWalkRunMidpoint = (2.5f + 7.0f) * 0.5f;
            walking = length > 0.5f && speed < kWalkRunMidpoint;
        }
        owner_.unitMoveFlagsCallbackRef()(data.guid, walking ? walk : 0u, walk);
    }

    if (data.hasDest) {
        glm::vec3 destCanonical = core::coords::serverToCanonical(
            glm::vec3(data.destX, data.destY, data.destZ));

        float orientation = entity->getOrientation();
        if (data.moveType == 4) {
            orientation = core::coords::serverToCanonicalYaw(data.facingAngle);
        } else if (data.moveType == 3) {
            auto target = owner_.getEntityManager().getEntity(data.facingTarget);
            if (target) {
                float dx = target->getX() - entity->getX();
                float dy = target->getY() - entity->getY();
                if (std::abs(dx) > 0.01f || std::abs(dy) > 0.01f) {
                    orientation = std::atan2(-dy, dx);
                }
            }
        } else {
            float dx = destCanonical.x - entity->getX();
            float dy = destCanonical.y - entity->getY();
            if (std::abs(dx) > 0.01f || std::abs(dy) > 0.01f) {
                orientation = std::atan2(-dy, dx);
            }
        }

        if (data.moveType != 3) {
            glm::vec3 startCanonical = core::coords::serverToCanonical(
                glm::vec3(data.x, data.y, data.z));
            float travelDx = destCanonical.x - startCanonical.x;
            float travelDy = destCanonical.y - startCanonical.y;
            float travelLen = std::sqrt(travelDx * travelDx + travelDy * travelDy);
            if (travelLen > 0.5f) {
                float travelAngle = std::atan2(-travelDy, travelDx);
                float diff = orientation - travelAngle;
                while (diff >  static_cast<float>(M_PI)) diff -= 2.0f * static_cast<float>(M_PI);
                while (diff < -static_cast<float>(M_PI)) diff += 2.0f * static_cast<float>(M_PI);
                if (std::abs(diff) > static_cast<float>(M_PI) * 0.5f) {
                    orientation = travelAngle;
                }
            }
        }

        // Build full path: start → waypoints → destination (all in canonical coords)
        if (!data.waypoints.empty()) {
            glm::vec3 startCanonical = core::coords::serverToCanonical(
                glm::vec3(data.x, data.y, data.z));
            std::vector<std::array<float, 3>> path;
            // Path = start + waypoints + dest. Reserve once to avoid the
            // 2–3 reallocations push_back would trigger as it doubles.
            path.reserve(data.waypoints.size() + 2);
            path.push_back({startCanonical.x, startCanonical.y, startCanonical.z});
            for (const auto& wp : data.waypoints) {
                glm::vec3 wpCanonical = core::coords::serverToCanonical(
                    glm::vec3(wp.x, wp.y, wp.z));
                path.push_back({wpCanonical.x, wpCanonical.y, wpCanonical.z});
            }
            path.push_back({destCanonical.x, destCanonical.y, destCanonical.z});
            entity->startMoveAlongPath(path, orientation, data.duration / 1000.0f);
        } else {
            entity->startMoveTo(destCanonical.x, destCanonical.y, destCanonical.z,
                                orientation, data.duration / 1000.0f);
        }

        if (owner_.creatureMoveCallbackRef()) {
            owner_.creatureMoveCallbackRef()(data.guid,
                destCanonical.x, destCanonical.y, destCanonical.z,
                data.duration);
        }
    } else if (data.moveType == 1) {
        glm::vec3 posCanonical = core::coords::serverToCanonical(
            glm::vec3(data.x, data.y, data.z));
        entity->setPosition(posCanonical.x, posCanonical.y, posCanonical.z,
                            entity->getOrientation());

        if (owner_.creatureMoveCallbackRef()) {
            owner_.creatureMoveCallbackRef()(data.guid,
                posCanonical.x, posCanonical.y, posCanonical.z, 0);
        }
    } else if (data.moveType == 4) {
        float orientation = core::coords::serverToCanonicalYaw(data.facingAngle);
        glm::vec3 posCanonical = core::coords::serverToCanonical(
            glm::vec3(data.x, data.y, data.z));
        entity->setPosition(posCanonical.x, posCanonical.y, posCanonical.z, orientation);
        if (owner_.creatureMoveCallbackRef()) {
            owner_.creatureMoveCallbackRef()(data.guid,
                posCanonical.x, posCanonical.y, posCanonical.z, 0);
        }
    } else if (data.moveType == 3 && data.facingTarget != 0) {
        auto target = owner_.getEntityManager().getEntity(data.facingTarget);
        if (target) {
            float dx = target->getX() - entity->getX();
            float dy = target->getY() - entity->getY();
            if (std::abs(dx) > 0.01f || std::abs(dy) > 0.01f) {
                float orientation = std::atan2(-dy, dx);
                entity->setOrientation(orientation);
            }
        }
    }
}

void MovementHandler::handleMonsterMoveTransport(network::Packet& packet) {
    // WotLK: packed mover and transport guids and the seat, then the common
    // monster-move body, run in the transport's frame (0x73F590).
    if (isActiveExpansion("wotlk")) {
        (void)handleStockMonsterMoveTransport(owner_, packet);
        return;
    }
    if (packet.getRemainingSize() < 8 + 1 + 8 + 12) return;
    uint64_t moverGuid = packet.readUInt64();
    /*uint8_t unk =*/ packet.readUInt8();
    uint64_t transportGuid = packet.readUInt64();

    float localX = packet.readFloat();
    float localY = packet.readFloat();
    float localZ = packet.readFloat();

    auto entity = owner_.getEntityManager().getEntity(moverGuid);
    if (!entity) return;

    if (packet.getReadPos() + 5 > packet.getSize()) {
        if (owner_.getTransportManager()) {
            glm::vec3 localCanonical = owner_.getTransportManager()->serverToTransportLocal(
                transportGuid, glm::vec3(localX, localY, localZ));
            owner_.setTransportAttachment(moverGuid, entity->getType(), transportGuid, localCanonical, false, 0.0f);
            glm::vec3 worldPos = owner_.getTransportManager()->getPlayerWorldPosition(transportGuid, localCanonical);
            entity->setPosition(worldPos.x, worldPos.y, worldPos.z, entity->getOrientation());
            if (entity->getType() == ObjectType::UNIT && owner_.creatureMoveCallbackRef())
                owner_.creatureMoveCallbackRef()(moverGuid, worldPos.x, worldPos.y, worldPos.z, 0);
        }
        return;
    }

    /*uint32_t splineId =*/ packet.readUInt32();
    uint8_t moveType = packet.readUInt8();

    if (moveType == 1) {
        if (owner_.getTransportManager()) {
            glm::vec3 localCanonical = owner_.getTransportManager()->serverToTransportLocal(
                transportGuid, glm::vec3(localX, localY, localZ));
            owner_.setTransportAttachment(moverGuid, entity->getType(), transportGuid, localCanonical, false, 0.0f);
            glm::vec3 worldPos = owner_.getTransportManager()->getPlayerWorldPosition(transportGuid, localCanonical);
            entity->setPosition(worldPos.x, worldPos.y, worldPos.z, entity->getOrientation());
            if (entity->getType() == ObjectType::UNIT && owner_.creatureMoveCallbackRef())
                owner_.creatureMoveCallbackRef()(moverGuid, worldPos.x, worldPos.y, worldPos.z, 0);
        }
        return;
    }

    float facingAngle = entity->getOrientation();
    if (moveType == 2) {
        if (packet.getReadPos() + 12 > packet.getSize()) return;
        float sx = packet.readFloat(), sy = packet.readFloat(), sz = packet.readFloat();
        facingAngle = std::atan2(-(sy - localY), sx - localX);
        (void)sz;
    } else if (moveType == 3) {
        if (packet.getReadPos() + 8 > packet.getSize()) return;
        uint64_t tgtGuid = packet.readUInt64();
        if (auto tgt = owner_.getEntityManager().getEntity(tgtGuid)) {
            float dx = tgt->getX() - entity->getX();
            float dy = tgt->getY() - entity->getY();
            if (std::abs(dx) > 0.01f || std::abs(dy) > 0.01f)
                facingAngle = std::atan2(-dy, dx);
        }
    } else if (moveType == 4) {
        if (packet.getReadPos() + 4 > packet.getSize()) return;
        facingAngle = core::coords::serverToCanonicalYaw(packet.readFloat());
    }

    if (packet.getReadPos() + 4 > packet.getSize()) return;
    uint32_t splineFlags = packet.readUInt32();

    // Consolidated spline body parser
    SplineBlockData spline;
    if (!parseMonsterMoveSplineBody(packet, spline, splineFlags,
                                    glm::vec3(localX, localY, localZ),
                                    /*useTbcUncompressedMask=*/false,
                                    isPreWotlk() ? SplineFlagSet::PreWotlk
                                                 : SplineFlagSet::Wotlk)) {
        return;
    }
    uint32_t duration = spline.duration;
    float destLocalX = spline.hasDest ? spline.destination.x : localX;
    float destLocalY = spline.hasDest ? spline.destination.y : localY;
    float destLocalZ = spline.hasDest ? spline.destination.z : localZ;
    bool hasDest = spline.hasDest;

    const bool usesPreWotlkSplineFlags = isActiveExpansion("classic") ||
                                          isActiveExpansion("turtle") ||
                                          isActiveExpansion("tbc");
    if (usesPreWotlkSplineFlags && entity->getType() == ObjectType::UNIT &&
        owner_.unitMoveFlagsCallbackRef()) {
        const uint32_t locomotionFlags =
            isPreWotlkSplineWalking(splineFlags)
                ? static_cast<uint32_t>(MovementFlags::WALKING)
                : 0u;
        owner_.unitMoveFlagsCallbackRef()(moverGuid, locomotionFlags, ~0u);
    } else if (!usesPreWotlkSplineFlags && entity->getType() == ObjectType::UNIT &&
               owner_.unitMoveFlagsCallbackRef() && hasDest) {
        const uint32_t walk = static_cast<uint32_t>(MovementFlags::WALKING);
        owner_.unitMoveFlagsCallbackRef()(moverGuid,
            (splineFlags & SplineFlagWotlk::WALKMODE) ? walk : 0u, walk);
    }

    if (!owner_.getTransportManager()) {
        LOG_WARNING("SMSG_MONSTER_MOVE_TRANSPORT: TransportManager not available for mover 0x",
                    std::hex, moverGuid, std::dec);
        return;
    }

    glm::vec3 startLocalCanonical = owner_.getTransportManager()->serverToTransportLocal(
        transportGuid, glm::vec3(localX, localY, localZ));

    if (hasDest && duration > 0) {
        glm::vec3 destLocalCanonical = owner_.getTransportManager()->serverToTransportLocal(
            transportGuid, glm::vec3(destLocalX, destLocalY, destLocalZ));
        glm::vec3 destWorld  = owner_.getTransportManager()->getPlayerWorldPosition(transportGuid, destLocalCanonical);

        if (moveType == 0) {
            float dx = destLocalCanonical.x - startLocalCanonical.x;
            float dy = destLocalCanonical.y - startLocalCanonical.y;
            if (std::abs(dx) > 0.01f || std::abs(dy) > 0.01f)
                facingAngle = std::atan2(-dy, dx);
        }

        owner_.setTransportAttachment(moverGuid, entity->getType(), transportGuid, destLocalCanonical, false, 0.0f);
        entity->startMoveTo(destWorld.x, destWorld.y, destWorld.z, facingAngle, duration / 1000.0f);

        if (entity->getType() == ObjectType::UNIT && owner_.creatureMoveCallbackRef())
            owner_.creatureMoveCallbackRef()(moverGuid, destWorld.x, destWorld.y, destWorld.z, duration);

        LOG_DEBUG("SMSG_MONSTER_MOVE_TRANSPORT: mover=0x", std::hex, moverGuid,
                  " transport=0x", transportGuid, std::dec,
                  " dur=", duration, "ms dest=(", destWorld.x, ",", destWorld.y, ",", destWorld.z, ")");
    } else {
        glm::vec3 startWorld = owner_.getTransportManager()->getPlayerWorldPosition(transportGuid, startLocalCanonical);
        owner_.setTransportAttachment(moverGuid, entity->getType(), transportGuid, startLocalCanonical, false, 0.0f);
        entity->setPosition(startWorld.x, startWorld.y, startWorld.z, facingAngle);
        if (entity->getType() == ObjectType::UNIT && owner_.creatureMoveCallbackRef())
            owner_.creatureMoveCallbackRef()(moverGuid, startWorld.x, startWorld.y, startWorld.z, 0);
    }
}

// ============================================================
// Teleport Handlers
// ============================================================

void MovementHandler::handleTeleportAck(network::Packet& packet) {
    if (packet.getRemainingSize() < 1u) {
        LOG_WARNING("MSG_MOVE_TELEPORT_ACK too short");
        return;
    }

    // CMaNGOS sends a packed guid here on every expansion, including TBC -
    // confirmed by live packet capture: reading a raw UInt64 instead
    // misaligned every field after it, so the guid never matched the local
    // player and the position update was silently dropped as "remote entity".
    uint64_t guid = packet.readPackedGuid();
    if (!packet.hasRemaining(4)) return;
    uint32_t counter = packet.readUInt32();

    // Three layouts, not two.
    //
    // WotLK writes moveFlags2 as a uint16. TBC writes it as a single byte -
    // that is the "unknown byte, constant 0x04" a live capture found here, and
    // reading it is what made a ground-truth teleport (.go xyz to
    // ironforge-city) decode to the expected x/y/z. Vanilla has no moveFlags2
    // field at all.
    //
    // isPreWotlk() is true for vanilla and TBC alike, so gating the byte on it
    // read one byte too many on vanilla and shifted x, y, z and orientation by
    // a byte each. The coordinates that came out were garbage derived from the
    // same misalignment every time, which is why .tele to three different
    // places all arrived somewhere in Stonetalon. Reported as issue 113.
    const bool taNoFlags2 = isPreWotlk();          // no uint16 moveFlags2
    const bool taByteFlags2 = isActiveExpansion("tbc");  // ...but TBC has a uint8
    const size_t minMoveSz = taNoFlags2
        ? (4 + (taByteFlags2 ? 1u : 0u) + 4 + 4 * 4)
        : (4 + 2 + 4 + 4 * 4);
    if (packet.getRemainingSize() < minMoveSz) {
        LOG_WARNING("MSG_MOVE_TELEPORT_ACK: not enough data for movement info");
        return;
    }

    packet.readUInt32();  // moveFlags
    if (!taNoFlags2)
        packet.readUInt16();  // moveFlags2 (WotLK only)
    uint32_t moveTime = packet.readUInt32();
    if (taByteFlags2)
        packet.readUInt8();  // TBC's uint8 moveFlags2 (see comment above)
    float serverX = packet.readFloat();
    float serverY = packet.readFloat();
    float serverZ = packet.readFloat();
    float orientation = packet.readFloat();

    LOG_WARNING("MSG_MOVE_TELEPORT_ACK: guid=0x", std::hex, guid, std::dec,
                " counter=", counter,
                " pos=(", serverX, ", ", serverY, ", ", serverZ, ")",
                " currentPos=(", movementInfo.x, ", ", movementInfo.y, ", ", movementInfo.z, ")");

    if (guid != owner_.getPlayerGuid()) {
        glm::vec3 canonical = core::coords::serverToCanonical(glm::vec3(serverX, serverY, serverZ));
        auto entity = owner_.getEntityManager().getEntity(guid);
        if (entity) {
            entity->setPosition(canonical.x, canonical.y, canonical.z,
                                core::coords::serverToCanonicalYaw(orientation));
        }
        if (guid != activeMoverGuid()) {
            LOG_INFO("MSG_MOVE_TELEPORT_ACK for 0x", std::hex, guid, std::dec,
                     ", not the active mover - placed, not acknowledged");
            return;
        }
        // The vehicle this client drives was put somewhere (stock 0x72D2D0
        // takes it as the mover's teleport): it goes there and says so.
        movementInfo.x = canonical.x;
        movementInfo.y = canonical.y;
        movementInfo.z = canonical.z;
        movementInfo.orientation = core::coords::serverToCanonicalYaw(orientation);
        movementInfo.flags &= ~stock_movement::kMovingMask;
        if (owner_.getSocket()) {
            network::Packet ack(wireOpcode(Opcode::MSG_MOVE_TELEPORT_ACK));
            ack.writePackedGuid(guid);
            ack.writeUInt32(counter);
            ack.writeUInt32(moveTime);
            owner_.getSocket()->send(ack);
        }
        return;
    }

    glm::vec3 canonical = core::coords::serverToCanonical(glm::vec3(serverX, serverY, serverZ));

    // A destination that is the map origin itself, which is a server-side
    // area-trigger misfire rather than anywhere anyone goes.
    //
    // This used to be a two-thousand-yard box - |x| and |y| under a thousand
    // on map 0 - and returned from here without acknowledging. Both halves
    // were wrong, and together they are what desynced the player from the
    // server for the rest of the session.
    //
    // The box: canonical swaps x and y off the wire, so that test was a square
    // two thousand yards on a side centred on the origin of Eastern Kingdoms,
    // and Hillsbrad Foothills sits on it. AzerothCore's own game_tele puts
    // Southshore at -853, -533 - inside it. So the hearthstone to the
    // Southshore inn, the Southshore graveyard and the inn's own tavern
    // trigger were all "near-origin misfires".
    //
    // The silence: the server sets a teleport semaphore when it sends this and
    // clears it when the acknowledgement comes back. Until then
    // WorldSession::HandleMovementOpcodes returns at its first line and throws
    // away every movement packet we send. So the note this replaces - that
    // heartbeats from the real position would eventually convince the server -
    // had it exactly backwards: no heartbeat could be heard at all. The client
    // walked on from where it thought it was, the server kept the player where
    // it had put them, no creatures arrived anywhere near the player, and a
    // relog put them back at the server's copy.
    //
    // The acknowledgement is now unconditional. Only the position is refused,
    // and only for a destination within a few yards of the origin, which no
    // legitimate teleport is.
    constexpr float kOriginMisfireRadius = 50.0f;
    const bool originMisfire = owner_.getCurrentMapId() == 0 &&
        std::abs(canonical.x) < kOriginMisfireRadius &&
        std::abs(canonical.y) < kOriginMisfireRadius;

    if (originMisfire) {
        LOG_WARNING("MSG_MOVE_TELEPORT to the map origin canonical=(",
                    canonical.x, ", ", canonical.y, ", ", canonical.z,
                    ") - acknowledging so the server stops discarding our "
                    "movement, then telling it where we really are");
    } else {
        movementInfo.x = canonical.x;
        movementInfo.y = canonical.y;
        movementInfo.z = canonical.z;
        movementInfo.orientation = core::coords::serverToCanonicalYaw(orientation);
        // What the player is doing ends here; what the player may do does not.
        // The stock teleport (0x6ECF80, run from the 0x2C movement event)
        // relocates the mover and leaves CMovement's flags (+0x44) as they
        // were. Zeroing them all threw away CAN_FLY with the rest, so after a
        // ".gm fly on" any teleport - ".go", ".tele" on the same map, a
        // summon - left space jumping instead of climbing, and the character
        // fell out of the sky. Water walking, feather fall, hover, levitate,
        // root and walk mode went the same way, each without a word from the
        // server to put it back.
        constexpr uint32_t kKeptAcrossTeleport =
            static_cast<uint32_t>(MovementFlags::WALKING) |
            static_cast<uint32_t>(MovementFlags::LEVITATING) |
            static_cast<uint32_t>(MovementFlags::ROOT) |
            static_cast<uint32_t>(MovementFlags::CAN_FLY) |
            static_cast<uint32_t>(MovementFlags::WATER_WALK) |
            static_cast<uint32_t>(MovementFlags::FEATHER_FALL) |
            static_cast<uint32_t>(MovementFlags::HOVER);
        movementInfo.flags &= kKeptAcrossTeleport;
    }

    // Clear cast bar on teleport - SpellHandler owns the casting_ flag
    if (owner_.getSpellHandler()) owner_.getSpellHandler()->resetCastState();

    // Suppress area triggers briefly after teleport. A one-shot flag is not
    // enough - the player can leave and re-enter a trigger within seconds and
    // get teleported again before the world has finished loading. Deeprun Tram
    // (map 369) is a narrow hallway with portal triggers close to the spawn,
    // so keep its grace window short enough that exits remain usable.
    const bool deeprunTram = owner_.getCurrentMapId() == 369;
    owner_.activeAreaTriggersRef().clear();
    owner_.areaTriggerCheckTimerRef() = deeprunTram ? -1.0f : -5.0f;
    owner_.areaTriggerSuppressFirstRef() = true;
    owner_.areaTriggerCooldownRef() = deeprunTram ? 1.5f : 10.0f;

    if (owner_.getSocket()) {
        network::Packet ack(wireOpcode(Opcode::MSG_MOVE_TELEPORT_ACK));
        const bool legacyGuidAck =
            isActiveExpansion("classic") || isActiveExpansion("tbc") || isActiveExpansion("turtle");
        if (legacyGuidAck) {
            ack.writeUInt64(owner_.getPlayerGuid());
        } else {
            ack.writePackedGuid(owner_.getPlayerGuid());
        }
        ack.writeUInt32(counter);
        ack.writeUInt32(moveTime);
        owner_.getSocket()->send(ack);
        LOG_INFO("Sent MSG_MOVE_TELEPORT_ACK response");

        // Immediately tell the server our position so it doesn't keep using
        // stale pre-teleport coordinates for area-trigger / anti-cheat checks.
        sendMovement(Opcode::MSG_MOVE_STOP);
        sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
    }

    // Only where the destination was taken. A misfire leaves the player where
    // they are, so there is no world entry to announce.
    if (!originMisfire && owner_.worldEntryCallbackRef()) {
        owner_.worldEntryCallbackRef()(owner_.currentMapIdRef(), serverX, serverY, serverZ, false);
    }
}

void MovementHandler::handleNewWorld(network::Packet& packet) {
    if (!packet.hasRemaining(20)) {
        LOG_WARNING("SMSG_NEW_WORLD too short");
        return;
    }

    uint32_t mapId = packet.readUInt32();
    float serverX = packet.readFloat();
    float serverY = packet.readFloat();
    float serverZ = packet.readFloat();
    float orientation = packet.readFloat();
    const bool crossMapTransportTransfer = owner_.isOnTransport() &&
        mapId != owner_.getCurrentMapId();
    if (crossMapTransportTransfer) {
        // While riding a cross-continent transport, SMSG_NEW_WORLD carries the
        // player's transport-local coordinates. They are not a world position
        // near map origin. Hold them until the same ship is registered on the
        // destination map, then compose them through its new transform.
        owner_.beginPlayerTransportWorldTransfer(
            mapId, core::coords::serverToCanonical(glm::vec3(serverX, serverY, serverZ)));
    }
    const uint32_t transferAreaTriggerId = lastAreaTriggerId_;
    lastAreaTriggerId_ = 0;
    postTransferReturnAreaTriggerId_ = findKnownReturnAreaTrigger(transferAreaTriggerId);
    postTransferReturnAreaTriggerSawNear_ = false;
    const bool hasPendingAreaTriggerDestination = pendingAreaTriggerDestinationValid_;
    const uint32_t pendingAreaTriggerDestinationMapId = pendingAreaTriggerDestinationMapId_;
    const glm::vec3 pendingAreaTriggerDestinationServerPos = pendingAreaTriggerDestinationServerPos_;
    const float pendingAreaTriggerDestinationServerO = pendingAreaTriggerDestinationServerO_;
    pendingAreaTriggerDestinationValid_ = false;
    const auto* knownTransferDestination = findKnownAreaTriggerDestination(transferAreaTriggerId, mapId);

    bool transferFallbackValid = false;
    glm::vec3 transferFallbackServerPos(0.0f);
    float transferFallbackServerO = orientation;
    if (hasPendingAreaTriggerDestination && pendingAreaTriggerDestinationMapId == mapId) {
        transferFallbackValid = true;
        transferFallbackServerPos = pendingAreaTriggerDestinationServerPos;
        transferFallbackServerO = pendingAreaTriggerDestinationServerO;
    } else if (knownTransferDestination) {
        transferFallbackValid = true;
        transferFallbackServerPos =
            glm::vec3(knownTransferDestination->serverX,
                      knownTransferDestination->serverY,
                      knownTransferDestination->serverZ);
        transferFallbackServerO = knownTransferDestination->serverO;
    }
    if (transferFallbackValid) {
        worldTransferFallbackValid_ = true;
        worldTransferFallbackMapId_ = mapId;
        worldTransferFallbackTriggerId_ = transferAreaTriggerId;
        worldTransferFallbackCanonicalPos_ = core::coords::serverToCanonical(transferFallbackServerPos);
        worldTransferFallbackCanonicalO_ = core::coords::serverToCanonicalYaw(transferFallbackServerO);
        orientation = transferFallbackServerO;
        LOG_WARNING("World transfer fallback armed: trigger=", transferAreaTriggerId,
                    " map=", mapId,
                    " canonical=(",
                    worldTransferFallbackCanonicalPos_.x, ", ",
                    worldTransferFallbackCanonicalPos_.y, ", ",
                    worldTransferFallbackCanonicalPos_.z, ")");
    } else {
        worldTransferFallbackValid_ = false;
    }

    LOG_INFO("SMSG_NEW_WORLD: mapId=", mapId,
             " pos=(", serverX, ", ", serverY, ", ", serverZ, ")",
             " orient=", orientation);

    glm::vec3 receivedCanonical = core::coords::serverToCanonical(glm::vec3(serverX, serverY, serverZ));
    const bool badEasternKingdomsNearOrigin =
        isCorruptOriginPosition(mapId, receivedCanonical.x, receivedCanonical.y,
                                receivedCanonical.z);
    if (badEasternKingdomsNearOrigin) {
        if (hasPendingAreaTriggerDestination && pendingAreaTriggerDestinationMapId == mapId) {
            LOG_WARNING("Correcting bad SMSG_NEW_WORLD near-origin destination for area trigger ",
                        transferAreaTriggerId,
                        ": received server=(", serverX, ", ", serverY, ", ", serverZ,
                        ") using pending server=(", pendingAreaTriggerDestinationServerPos.x, ", ",
                        pendingAreaTriggerDestinationServerPos.y, ", ",
                        pendingAreaTriggerDestinationServerPos.z, ")");
            serverX = pendingAreaTriggerDestinationServerPos.x;
            serverY = pendingAreaTriggerDestinationServerPos.y;
            serverZ = pendingAreaTriggerDestinationServerPos.z;
            orientation = pendingAreaTriggerDestinationServerO;
            receivedCanonical = core::coords::serverToCanonical(glm::vec3(serverX, serverY, serverZ));
        } else if (knownTransferDestination) {
            LOG_WARNING("Correcting bad SMSG_NEW_WORLD near-origin destination for area trigger ",
                        transferAreaTriggerId,
                        ": received server=(", serverX, ", ", serverY, ", ", serverZ,
                        ") using server=(", knownTransferDestination->serverX, ", ",
                        knownTransferDestination->serverY, ", ", knownTransferDestination->serverZ, ")");
            serverX = knownTransferDestination->serverX;
            serverY = knownTransferDestination->serverY;
            serverZ = knownTransferDestination->serverZ;
            orientation = knownTransferDestination->serverO;
            receivedCanonical = core::coords::serverToCanonical(glm::vec3(serverX, serverY, serverZ));
        } else {
            LOG_WARNING("SMSG_NEW_WORLD near-origin destination on map 0 without known area trigger fallback: trigger=",
                        transferAreaTriggerId,
                        " server=(", serverX, ", ", serverY, ", ", serverZ, ")");
        }
    }

    const bool isSameMap       = (mapId == owner_.currentMapIdRef());
    const bool isResurrection  = owner_.resurrectPendingRef();
    if (isSameMap && isResurrection) {
        LOG_INFO("SMSG_NEW_WORLD same-map resurrection - skipping world reload");

        glm::vec3 canonical = receivedCanonical;
        movementInfo.x = canonical.x;
        movementInfo.y = canonical.y;
        movementInfo.z = canonical.z;
        movementInfo.orientation = core::coords::serverToCanonicalYaw(orientation);
        movementInfo.flags  = 0;
        movementInfo.flags2 = 0;

        owner_.resurrectPendingRef()       = false;
        owner_.resurrectRequestPendingRef() = false;
        owner_.releasedSpiritRef()         = false;
        owner_.playerDeadRef()             = false;
        owner_.repopPendingRef()           = false;
        owner_.pendingSpiritHealerGuidRef() = 0;
        owner_.resurrectCasterGuidRef()    = 0;
        owner_.corpseMapIdRef()            = 0;
        owner_.corpsePositionValidRef()    = false;
        owner_.corpseGuidRef()             = 0;
        owner_.clearHostileAttackers();
        owner_.stopAutoAttack();
        owner_.resetCastState();

        const bool deeprunTram = owner_.getCurrentMapId() == 369;
        owner_.activeAreaTriggersRef().clear();
        owner_.areaTriggerCheckTimerRef() = deeprunTram ? -1.0f : -5.0f;
        owner_.areaTriggerSuppressFirstRef() = true;
        owner_.areaTriggerCooldownRef() = deeprunTram ? 1.5f : 10.0f;

        if (owner_.getSocket()) {
            network::Packet ack(wireOpcode(Opcode::MSG_MOVE_WORLDPORT_ACK));
            owner_.getSocket()->send(ack);
            LOG_INFO("Sent MSG_MOVE_WORLDPORT_ACK (resurrection)");

            sendMovement(Opcode::MSG_MOVE_STOP);
            sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
        }
        return;
    }

    owner_.currentMapIdRef() = mapId;
    owner_.inInstanceRef() = false;
    // Held until the world loader has the character on the new map: see
    // endWorldPortHold. Set before movementInfo takes the destination so
    // nothing in between can put the old map's position back.
    worldPortHold_ = true;
    worldPortHoldSince_ = std::chrono::steady_clock::now();
    if (owner_.getSocket()) {
        owner_.getSocket()->tracePacketsFor(std::chrono::seconds(12), "new_world");
    }

    glm::vec3 canonical = receivedCanonical;
    movementInfo.x = canonical.x;
    movementInfo.y = canonical.y;
    movementInfo.z = canonical.z;
    movementInfo.orientation = core::coords::serverToCanonicalYaw(orientation);
    movementInfo.flags = 0;
    movementInfo.flags2 = 0;
    serverMovementAllowed_ = true;
    owner_.resurrectPendingRef() = false;
    owner_.resurrectRequestPendingRef() = false;
    onTaxiFlight_ = false;
    taxiMountActive_ = false;
    taxiActivatePending_ = false;
    taxiClientActive_ = false;
    taxiRecoverPending_ = false;
    taxiStartGrace_ = 0.0f;
    owner_.currentMountDisplayIdRef() = 0;
    if (owner_.mountCallbackRef()) {
        owner_.mountCallbackRef()(0);
    }

    for (const auto& [guid, entity] : owner_.getEntityManager().getEntities()) {
        if (guid == owner_.getPlayerGuid()) continue;
        if (entity->getType() == ObjectType::UNIT && owner_.creatureDespawnCallbackRef()) {
            owner_.creatureDespawnCallbackRef()(guid);
        } else if (entity->getType() == ObjectType::PLAYER && owner_.playerDespawnCallbackRef()) {
            owner_.playerDespawnCallbackRef()(guid);
        } else if (entity->getType() == ObjectType::GAMEOBJECT && owner_.gameObjectDespawnCallbackRef()) {
            owner_.gameObjectDespawnCallbackRef()(guid);
        }
    }
    owner_.otherPlayerVisibleItemEntriesRef().clear();
    owner_.otherPlayerWeaponEnchantsRef().clear();
    owner_.otherPlayerVisibleDirtyRef().clear();
    otherPlayerMoveTimeMs_.clear();
    if (owner_.getSpellHandler()) owner_.getSpellHandler()->clearUnitCastStates();
    owner_.unitAurasCacheRef().clear();
    owner_.clearCombatText();
    owner_.getEntityManager().clear();
    owner_.clearHostileAttackers();
    owner_.worldStatesRef().clear();
    owner_.clearGossipPois();
    owner_.worldStateMapIdRef() = mapId;
    owner_.worldStateZoneIdRef() = 0;
    owner_.activeAreaTriggersRef().clear();
    const bool deeprunTram = mapId == 369;
    owner_.areaTriggerCheckTimerRef() = deeprunTram ? -1.0f : -5.0f;
    owner_.areaTriggerSuppressFirstRef() = true;
    owner_.areaTriggerCooldownRef() = deeprunTram ? 1.5f : 10.0f;
    owner_.stopAutoAttack();
    owner_.resetCastState();

    if (owner_.getSocket()) {
        // The acknowledgement alone, as the stock client sends it (0x403B70:
        // an empty 0xDC once the map is loaded). The server places the player
        // at the destination itself on receiving it, so there is no position
        // to tell it - and a STOP or HEARTBEAT here is a movement packet the
        // stock client never sends.
        network::Packet ack(wireOpcode(Opcode::MSG_MOVE_WORLDPORT_ACK));
        owner_.getSocket()->send(ack);
        LOG_INFO("Sent MSG_MOVE_WORLDPORT_ACK");
    }

    owner_.timeSinceLastPingRef() = 0.0f;
    if (owner_.getSocket()) {
        LOG_WARNING("World transfer keepalive: sending immediate ping after MSG_MOVE_WORLDPORT_ACK");
        owner_.sendPing();
    }

    if (owner_.worldEntryCallbackRef()) {
        owner_.worldEntryCallbackRef()(mapId, serverX, serverY, serverZ, isSameMap);
    }

    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("PLAYER_ENTERING_WORLD", {"0"});
        owner_.addonEventCallbackRef()("ZONE_CHANGED_NEW_AREA", {});
    }
    owner_.battlefieldWorldEntered();
}

// ============================================================
// Taxi / Flight Path Handlers
// ============================================================

void MovementHandler::loadTaxiDbc() {
    if (taxiDbcLoaded_) return;

    auto* am = owner_.services().assetManager;
    // Not an attempt: the assets are not there to read yet, and a
    // caller can reach this before they are. Marking it loaded here
    // meant one early call disabled this file for the whole session.
    if (!am || !am->isInitialized()) return;
    taxiDbcLoaded_ = true;

    auto nodesDbc = am->loadDBC("TaxiNodes.dbc");
    if (nodesDbc && nodesDbc->isLoaded()) {
        const auto* tnL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("TaxiNodes") : nullptr;
        // Cache field indices before the loop
        const uint32_t tnIdField   = tnL ? (*tnL)["ID"]    : 0;
        const uint32_t tnMapField  = tnL ? (*tnL)["MapID"] : 1;
        const uint32_t tnXField    = tnL ? (*tnL)["X"]     : 2;
        const uint32_t tnYField    = tnL ? (*tnL)["Y"]     : 3;
        const uint32_t tnZField    = tnL ? (*tnL)["Z"]     : 4;
        const uint32_t tnNameField = tnL ? (*tnL)["Name"]  : 5;
        // TaxiNodes.dbc ends with the two mount creature ids and nothing else.
        // There used to be a "fallback" pair read when both were zero, at 20
        // and 21 - which are the last name locale and the name flags word, so
        // a node with no mount took 16712190 as a display id. There is no
        // second pair to fall back to; a node without a mount has none.
        const uint32_t mountAllianceField = tnL ? (*tnL)["MountDisplayIdAlliance"] : 22;
        const uint32_t mountHordeField    = tnL ? (*tnL)["MountDisplayIdHorde"]    : 23;
        uint32_t fieldCount = nodesDbc->getFieldCount();
        for (uint32_t i = 0; i < nodesDbc->getRecordCount(); i++) {
            TaxiNode node;
            node.id = nodesDbc->getUInt32(i, tnIdField);
            node.mapId = nodesDbc->getUInt32(i, tnMapField);
            node.x = nodesDbc->getFloat(i, tnXField);
            node.y = nodesDbc->getFloat(i, tnYField);
            node.z = nodesDbc->getFloat(i, tnZField);
            node.name = nodesDbc->getString(i, tnNameField);
            if (fieldCount > mountHordeField) {
                node.mountDisplayIdAlliance = nodesDbc->getUInt32(i, mountAllianceField);
                node.mountDisplayIdHorde = nodesDbc->getUInt32(i, mountHordeField);
            }
            uint32_t nodeId = node.id;
            if (nodeId > 0) {
                taxiNodes_[nodeId] = std::move(node);
            }
            if (nodeId == 195) {
                std::string fields;
                for (uint32_t f = 0; f < fieldCount; f++) {
                    fields += std::to_string(f) + ":" + std::to_string(nodesDbc->getUInt32(i, f)) + " ";
                }
                LOG_INFO("TaxiNodes[195] fields: ", fields);
            }
        }
        LOG_INFO("Loaded ", taxiNodes_.size(), " taxi nodes from TaxiNodes.dbc");
    } else {
        LOG_WARNING("Could not load TaxiNodes.dbc");
    }

    auto pathDbc = am->loadDBC("TaxiPath.dbc");
    if (pathDbc && pathDbc->isLoaded()) {
        const auto* tpL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("TaxiPath") : nullptr;
        const uint32_t tpIdField   = tpL ? (*tpL)["ID"]       : 0;
        const uint32_t tpFromField = tpL ? (*tpL)["FromNode"] : 1;
        const uint32_t tpToField   = tpL ? (*tpL)["ToNode"]   : 2;
        const uint32_t tpCostField = tpL ? (*tpL)["Cost"]     : 3;
        for (uint32_t i = 0; i < pathDbc->getRecordCount(); i++) {
            TaxiPathEdge edge;
            edge.pathId = pathDbc->getUInt32(i, tpIdField);
            edge.fromNode = pathDbc->getUInt32(i, tpFromField);
            edge.toNode = pathDbc->getUInt32(i, tpToField);
            edge.cost = pathDbc->getUInt32(i, tpCostField);
            taxiPathEdges_.push_back(edge);
        }
        LOG_INFO("Loaded ", taxiPathEdges_.size(), " taxi path edges from TaxiPath.dbc");
    } else {
        LOG_WARNING("Could not load TaxiPath.dbc");
    }

    auto pathNodeDbc = am->loadDBC("TaxiPathNode.dbc");
    if (pathNodeDbc && pathNodeDbc->isLoaded()) {
        const auto* tpnL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("TaxiPathNode") : nullptr;
        const uint32_t tpnIdField    = tpnL ? (*tpnL)["ID"]        : 0;
        const uint32_t tpnPathField  = tpnL ? (*tpnL)["PathID"]    : 1;
        const uint32_t tpnIndexField = tpnL ? (*tpnL)["NodeIndex"] : 2;
        const uint32_t tpnMapField   = tpnL ? (*tpnL)["MapID"]     : 3;
        const uint32_t tpnXField     = tpnL ? (*tpnL)["X"]         : 4;
        const uint32_t tpnYField     = tpnL ? (*tpnL)["Y"]         : 5;
        const uint32_t tpnZField     = tpnL ? (*tpnL)["Z"]         : 6;
        for (uint32_t i = 0; i < pathNodeDbc->getRecordCount(); i++) {
            TaxiPathNode node;
            node.id = pathNodeDbc->getUInt32(i, tpnIdField);
            node.pathId = pathNodeDbc->getUInt32(i, tpnPathField);
            node.nodeIndex = pathNodeDbc->getUInt32(i, tpnIndexField);
            node.mapId = pathNodeDbc->getUInt32(i, tpnMapField);
            node.x = pathNodeDbc->getFloat(i, tpnXField);
            node.y = pathNodeDbc->getFloat(i, tpnYField);
            node.z = pathNodeDbc->getFloat(i, tpnZField);
            taxiPathNodes_[node.pathId].push_back(node);
        }
        for (auto& [pathId, nodes] : taxiPathNodes_) {
            std::sort(nodes.begin(), nodes.end(),
                [](const TaxiPathNode& a, const TaxiPathNode& b) {
                    return a.nodeIndex < b.nodeIndex;
                });
        }
        LOG_INFO("Loaded ", pathNodeDbc->getRecordCount(), " taxi path waypoints from TaxiPathNode.dbc");
    } else {
        LOG_WARNING("Could not load TaxiPathNode.dbc");
    }
}

void MovementHandler::handleShowTaxiNodes(network::Packet& packet) {
    ShowTaxiNodesData data;
    if (!ShowTaxiNodesParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_SHOWTAXINODES");
        return;
    }

    loadTaxiDbc();

    // A new node is announced by SMSG_NEW_TAXI_PATH (ERR_NEWTAXIPATH), not
    // by comparing masks here.
    for (uint32_t i = 0; i < TLK_TAXI_MASK_SIZE; ++i) {
        knownTaxiMask_[i] = data.nodeMask[i];
    }
    taxiMaskInitialized_ = true;

    // Fewer than two known nodes: no map, ERR_TAXINOPATHS, and the gossip
    // closes (0x6D6050).
    {
        uint32_t known = 0;
        for (uint32_t i = 0; i < TLK_TAXI_MASK_SIZE; ++i)
            known += static_cast<uint32_t>(__builtin_popcount(data.nodeMask[i]));
        if (known < 2) {
            owner_.raiseUiError(globalString(owner_.services().assetManager, "ERR_TAXINOPATHS"));
            owner_.closeGossip();
            return;
        }
    }
    // Opening the map dismounts the player (0x6D6050 -> 0x7412E0,
    // CMSG_CANCEL_MOUNT_AURA): TakeTaxiNode refuses a mounted player.
    if (owner_.isMounted() && !onTaxiFlight_) dismount();

    // The map already open for this flight master: Wow.exe 0x5932e0 returns
    // at once when the npc guid is the one it built the map for, so a second
    // SHOWTAXINODES raises no second TAXIMAP_OPENED (FrameXML would redraw
    // and replay the open sound each time).
    const bool alreadyOpen = taxiWindowOpen_ && taxiNpcGuid_ == data.npcGuid;
    currentTaxiData_ = data;
    taxiNpcGuid_ = data.npcGuid;
    taxiWindowOpen_ = true;
    owner_.closeGossip();
    buildTaxiCostMap();
    auto it = taxiNodes_.find(data.nearestNode);
    if (it != taxiNodes_.end()) {
        LOG_INFO("Taxi node ", data.nearestNode, " mounts: A=", it->second.mountDisplayIdAlliance,
                 " H=", it->second.mountDisplayIdHorde);
    }
    LOG_INFO("Taxi window opened, nearest node=", data.nearestNode);
    // The flight map opens on this and closes on its pair. Neither was fired,
    // so the original interface's map could not appear however complete the
    // functions behind it were.
    if (!alreadyOpen && owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()("TAXIMAP_OPENED", {});
}

// The flight itself is the server's. AzerothCore's FlightPathMovementGenerator
// sends the player an SMSG_MONSTER_MOVE with the path from where they stand
// (spline flag Flying: twelve-byte nodes and a Catmull-Rom curve, at
// TaxiFlightSpeed, 32), and the stock client runs it with the same code as
// every other unit's spline (0x73C8E0 -> 0x6EB680, 0x98CA00 each frame): the
// player's entity runs it here (Entity::startMonsterMove). TaxiPathNode is
// never flown - the client reads it only for the map and the route choice -
// and the mount is whatever UNIT_FIELD_MOUNTDISPLAYID says. While the spline
// runs the player cannot move (0x5FA060). See docs/re/mounts-taxi-vehicles.md.
std::string MovementHandler::taxiReplyErrorText(uint32_t result) const {
    const char* key = taxi::replyErrorKey(result);
    if (!key) key = "ERR_TAXIUNSPECIFIEDSERVERERROR";
    std::string text = globalString(owner_.services().assetManager, key);
    return text.empty() ? std::string(key) : text;
}

void MovementHandler::endTaxiFlight() {
    taxiClientActive_ = false;
    onTaxiFlight_ = false;
    taxiLandingCooldown_ = 2.0f;
    taxiMountActive_ = false;
    taxiRecoverPending_ = false;
    taxiSplineSeen_ = false;
    movementInfo.flags = 0;
    movementInfo.flags2 = 0;
    if (owner_.taxiOrientationCallbackRef()) {
        // Level again: no path pitch or bank on the ground.
        owner_.taxiOrientationCallbackRef()(
            glm::radians(core::coords::canonicalToCharacterYawDeg(movementInfo.orientation)), 0.0f, 0.0f);
    }
    if (owner_.getSocket()) {
        sendMovement(Opcode::MSG_MOVE_STOP);
        sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
    }
    LOG_INFO("Taxi flight landed (server spline)");
}

void MovementHandler::updateClientTaxi(float deltaTime) {
    if (dismountGraceRemaining_ > 0.0f) {
        dismountGraceRemaining_ -= deltaTime;
    }
    if (taxiStartGrace_ > 0.0f) taxiStartGrace_ -= deltaTime;

    // A CMSG_ACTIVATETAXI the server cannot make sense of can go unanswered;
    // without a timeout the pending guard would refuse every later flight.
    if (taxiActivatePending_) {
        taxiActivateTimer_ += deltaTime;
        if (taxiActivateTimer_ > kTaxiActivateReplyTimeoutSeconds) {
            LOG_WARNING("Taxi activation reply timed out after ",
                        kTaxiActivateReplyTimeoutSeconds, "s - clearing pending state");
            taxiActivatePending_ = false;
            taxiActivateTimer_ = 0.0f;
            onTaxiFlight_ = false;
        }
    }

    if (!onTaxiFlight_) return;
    auto playerEntity = owner_.getEntityManager().getEntity(owner_.getPlayerGuid());
    if (playerEntity && playerEntity->hasStockMotion() && playerEntity->stockMotionIsSpline()) {
        // Where the spline has the player: the position the client reports
        // and draws, facing along the path; the model takes the path's pitch
        // and banks into the turn one second ahead (0x98CA00).
        taxiSplineSeen_ = true;
        taxiClientActive_ = true;
        movementInfo.x = playerEntity->getX();
        movementInfo.y = playerEntity->getY();
        movementInfo.z = playerEntity->getZ();
        movementInfo.orientation = playerEntity->getOrientation();
        if (owner_.taxiOrientationCallbackRef()) {
            const float renderYaw =
                glm::radians(core::coords::canonicalToCharacterYawDeg(playerEntity->getOrientation()));
            const bool att = playerEntity->stockMotionHasAttitude();
            const float pitch = att ? playerEntity->stockMotionAttitudePitch() : 0.0f;
            // The renderer's roll is positive for a right bank.
            const float roll = att ? -playerEntity->stockMotionBank() : 0.0f;
            owner_.taxiOrientationCallbackRef()(renderYaw, pitch, roll);
        }
        return;
    }
    taxiClientActive_ = false;

    // No spline: the flight is over once the server has cleared
    // UNIT_FLAG_TAXI_FLIGHT (what UnitOnTaxi reads, 0x60F350), or, should the
    // flag never have come, once the spline has run and the start grace is
    // spent. The server dismounts through the mount field.
    bool serverTaxi = false;
    if (auto unit = std::dynamic_pointer_cast<Unit>(playerEntity))
        serverTaxi = (unit->getUnitFlags() & UNIT_FLAG_TAXI_FLIGHT) != 0;
    if (serverTaxi || taxiStartGrace_ > 0.0f) return;
    if (!taxiSplineSeen_ && taxiActivatePending_) return;
    endTaxiFlight();
}

void MovementHandler::handleActivateTaxiReply(network::Packet& packet) {
    ActivateTaxiReplyData data;
    if (!ActivateTaxiReplyParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_ACTIVATETAXIREPLY");
        return;
    }

    // Whether a reply arrived at all is the first thing worth knowing when a
    // flight does not happen, and it was only ever said at info level.
    LOG_WARNING("SMSG_ACTIVATETAXIREPLY: result=", data.result,
                " pending=", taxiActivatePending_);

    if (!taxiActivatePending_) {
        LOG_DEBUG("Ignoring stray taxi reply: result=", data.result);
        return;
    }

    if (data.result == 0) {
        if (onTaxiFlight_ && !taxiActivatePending_) {
            return;
        }
        onTaxiFlight_ = true;
        taxiStartGrace_ = std::max(taxiStartGrace_, 2.0f);
        sanitizeMovementForTaxi();
        // Closed here rather than through closeTaxi, which would clear the taxi
        // mount that is about to be applied - but the event has to be sent all
        // the same, with closeTaxi's own guard: the flight map hides on this,
        // and setting the flag alone left FrameXML's open for the whole flight.
        const bool taxiWasOpen = taxiWindowOpen_;
        taxiWindowOpen_ = false;
        if (taxiWasOpen && owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("TAXIMAP_CLOSED", {});
        }
        taxiActivatePending_ = false;
        taxiActivateTimer_ = 0.0f;
        taxiSplineSeen_ = false;
        // The server's spline (SMSG_MONSTER_MOVE for the player) and mount
        // field follow this reply; updateClientTaxi reads the spline.
        LOG_INFO("Taxi flight started!");
    } else {
        if (onTaxiFlight_ || taxiClientActive_) {
            LOG_WARNING("Ignoring stale taxi failure reply while flight is active: result=", data.result);
            taxiActivatePending_ = false;
            taxiActivateTimer_ = 0.0f;
            return;
        }
        LOG_WARNING("Taxi activation failed, result=", data.result);
        // The error the reply names (0x6CEE90, table 0xAD9C94).
        owner_.raiseUiError(taxiReplyErrorText(data.result));
        taxiActivatePending_ = false;
        taxiActivateTimer_ = 0.0f;
        // Clear the pending path data built speculatively by activateTaxi() -
        // it was never committed (taxiClientActive_ never went true), but drop it
        // explicitly so a retry can't somehow pick up stale waypoints.
        taxiMountActive_ = false;
        onTaxiFlight_ = false;
    }
}

void MovementHandler::closeTaxi() {
    const bool wasOpen = taxiWindowOpen_;
    taxiWindowOpen_ = false;
    // Only when it was actually open: closeTaxi is also reached on paths that
    // never opened a window, and the flight map hides itself on this.
    if (wasOpen && owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("TAXIMAP_CLOSED", {});
    }

    if (taxiActivatePending_ || onTaxiFlight_ || taxiClientActive_) {
        return;
    }

    if (taxiMountActive_ && owner_.mountCallbackRef()) {
        LOG_INFO("closeTaxi: clearing taxi mount");
        owner_.mountCallbackRef()(0);
    }
    taxiMountActive_ = false;

    taxiActivatePending_ = false;
    onTaxiFlight_ = false;

    taxiLandingCooldown_ = 2.0f;
}

void MovementHandler::buildTaxiCostMap() {
    taxiCostMap_.clear();
    taxiPrevMap_.clear();
    uint32_t startNode = currentTaxiData_.nearestNode;
    if (startNode == 0) return;

    // Each TaxiPath's flown length: the 3D distances between its
    // TaxiPathNode points (0x592580). Built once.
    if (taxiRouteEdges_.size() != taxiPathEdges_.size()) {
        taxiRouteEdges_.clear();
        taxiRouteEdges_.reserve(taxiPathEdges_.size());
        for (const auto& edge : taxiPathEdges_) {
            taxi::Edge e;
            e.from = edge.fromNode;
            e.to = edge.toNode;
            e.price = edge.cost;
            auto pit = taxiPathNodes_.find(edge.pathId);
            if (pit != taxiPathNodes_.end()) {
                for (size_t i = 1; i < pit->second.size(); ++i) {
                    const auto& p0 = pit->second[i - 1];
                    const auto& p1 = pit->second[i];
                    e.length += glm::length(glm::vec3(p1.x - p0.x, p1.y - p0.y, p1.z - p0.z));
                }
            }
            taxiRouteEdges_.push_back(e);
        }
    }

    // The route to each known node is the shortest flight (0x592BB0 /
    // 0x5926C0: total TaxiPathNode distance, through known nodes only), not
    // the fewest hops; its price the direct TaxiPath's when there is one, else
    // the sum of its TaxiPaths' (taxi::routePrice).
    auto known = [this](uint32_t node) { return currentTaxiData_.isNodeKnown(node); };
    // The fares by the flight master's regard for the player (0x591350):
    // Friendly to Exalted take 5% to 20% off.
    int tier = 0;
    if (auto npc = std::dynamic_pointer_cast<Unit>(owner_.getEntityManager().getEntity(taxiNpcGuid_))) {
        tier = taxi::reputationTier(owner_.unitReactionToPlayer(*npc),
                                    // A faction with a standing (Faction.dbc ReputationListID,
                                    // 0x718B30), once the standings are in.
                                    owner_.factionAtWarLive(npc->getFactionTemplate()) >= 0);
    }
    taxiCostMap_[startNode] = 0;
    for (const auto& [nodeId, node] : taxiNodes_) {
        (void)node;
        if (nodeId == startNode || !known(nodeId)) continue;
        const std::vector<uint32_t> route = taxi::shortestRoute(taxiRouteEdges_, startNode, nodeId, known);
        if (route.size() < 2) continue;
        taxiCostMap_[nodeId] = taxi::routePrice(taxiRouteEdges_, route, tier);
        for (size_t i = 1; i < route.size(); ++i) taxiPrevMap_[route[i]] = route[i - 1];
    }
}

uint32_t MovementHandler::getTaxiCostTo(uint32_t destNodeId) const {
    auto it = taxiCostMap_.find(destNodeId);
    return (it != taxiCostMap_.end()) ? it->second : 0;
}

bool MovementHandler::hasTaxiRouteTo(uint32_t destNodeId) const {
    return destNodeId != currentTaxiData_.nearestNode &&
           taxiCostMap_.find(destNodeId) != taxiCostMap_.end();
}

std::vector<uint32_t> MovementHandler::getTaxiRouteTo(uint32_t destNodeId) const {
    std::vector<uint32_t> route;
    if (!hasTaxiRouteTo(destNodeId)) return route;

    uint32_t startNode = currentTaxiData_.nearestNode;
    uint32_t cur = destNodeId;
    while (cur != startNode) {
        route.push_back(cur);
        auto it = taxiPrevMap_.find(cur);
        if (it == taxiPrevMap_.end() || route.size() > taxiPrevMap_.size()) {
            return {};  // broken chain - should not happen with a consistent BFS
        }
        cur = it->second;
    }
    route.push_back(startNode);
    std::reverse(route.begin(), route.end());
    return route;
}

void MovementHandler::activateTaxi(uint32_t destNodeId) {
    if (!owner_.getSocket() || owner_.getState() != WorldState::IN_WORLD) return;

    if (taxiActivatePending_ || onTaxiFlight_) {
        return;
    }

    uint32_t startNode = currentTaxiData_.nearestNode;
    if (startNode == 0 || destNodeId == 0 || startNode == destNodeId) return;


    if (owner_.isMounted()) {
        LOG_INFO("Taxi activate: dismounting current mount");
        if (owner_.mountCallbackRef()) owner_.mountCallbackRef()(0);
        owner_.currentMountDisplayIdRef() = 0;
        dismount();
    }

    if (auto destIt = taxiNodes_.find(destNodeId); destIt != taxiNodes_.end())
        taxiDestName_ = destIt->second.name;
    else
        taxiDestName_.clear();

    // The route the cost map already worked out, rather than a second search of
    // the same graph.
    //
    // The one here searched every edge, including those reaching nodes the
    // player has never been to. The server refuses a flight the moment one
    // appears in the list - ERR_TAXINOTVISITED, checked node by node - so a
    // route through an undiscovered stop is turned down outright rather than
    // flown badly. buildTaxiCostMap already skips those, which is why the price
    // quoted came from a route this then declined to use.
    std::vector<uint32_t> path = getTaxiRouteTo(destNodeId);

    if (path.size() < 2) {
        LOG_WARNING("No taxi route through discovered nodes from ", startNode,
                    " to ", destNodeId);
        owner_.raiseUiError(taxiReplyErrorText(2));  // ERR_TAXINOSUCHPATH (0x5914B0)
        return;
    }

    LOG_INFO("Taxi path: ", path.size(), " nodes, from ", startNode, " to ", destNodeId);

    // At warning level: the log carries nothing below it, and a flight that
    // goes wrong is otherwise reported with the request that caused it
    // invisible. A guid of zero here means no flight master was recorded, and
    // the server has nothing to answer.
    LOG_WARNING("Taxi activate: npc=0x", std::hex, taxiNpcGuid_, std::dec,
                " start=", startNode, " dest=", destNodeId,
                " pathLen=", path.size());
    if (!path.empty()) {
        std::string pathStr;
        for (size_t i = 0; i < path.size(); i++) {
            pathStr += std::to_string(path[i]);
            if (i + 1 < path.size()) pathStr += "->";
        }
        LOG_INFO("Taxi path nodes: ", pathStr);
    }

    uint32_t totalCost = getTaxiCostTo(destNodeId);
    LOG_INFO("Taxi activate: start=", startNode, " dest=", destNodeId, " cost=", totalCost);

    // A route of more than two nodes has to go as ACTIVATETAXIEXPRESS with the
    // whole path. CMSG_ACTIVATETAXI carries only a source and a destination,
    // and the server answers it by looking for a single TaxiPath joining the
    // two - so anything needing an intermediate stop has no such path, gets no
    // reply, and times out. Stormwind to Westfall is one hop and worked;
    // everything further did not.
    if (path.size() > 2) {
        auto expressPkt =
            ActivateTaxiExpressPacket::build(taxiNpcGuid_, path);
        owner_.getSocket()->send(expressPkt);
        LOG_WARNING("Taxi activate: sent EXPRESS for ", path.size(),
                    " nodes (a multi-hop route has no single path to ask for)");
    } else {
        auto basicPkt = ActivateTaxiPacket::build(taxiNpcGuid_, startNode, destNodeId);
        owner_.getSocket()->send(basicPkt);
    }

    // The window closes on the request, not on the reply, and FrameXML's taxi
    // frame only hides on the event. Clearing the flag alone closes this
    // client's own map and node list, which poll it, and leaves TaxiFrame up
    // for the whole flight - the reply's own send of this event cannot cover it,
    // because its guard reads the flag this line has already cleared.
    const bool taxiWasOpen = taxiWindowOpen_;
    taxiWindowOpen_ = false;
    if (taxiWasOpen && owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("TAXIMAP_CLOSED", {});
    }
    taxiActivatePending_ = true;
    taxiActivateTimer_ = 0.0f;
    // Nothing starts here: CMSG_ACTIVATETAXI can be refused. The flight is
    // the server's spline, which follows a successful reply.
    if (owner_.getSocket()) {
        sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
    }

    if (owner_.taxiPrecacheCallbackRef()) {
        std::vector<glm::vec3> previewPath;
        for (size_t i = 0; i + 1 < path.size(); i++) {
            uint32_t fromNode = path[i];
            uint32_t toNode = path[i + 1];
            uint32_t pathId = 0;
            for (const auto& edge : taxiPathEdges_) {
                if (edge.fromNode == fromNode && edge.toNode == toNode) {
                    pathId = edge.pathId;
                    break;
                }
            }
            if (pathId == 0) continue;
            auto pathIt = taxiPathNodes_.find(pathId);
            if (pathIt != taxiPathNodes_.end()) {
                for (const auto& wpNode : pathIt->second) {
                    glm::vec3 serverPos(wpNode.x, wpNode.y, wpNode.z);
                    glm::vec3 canonical = core::coords::serverToCanonical(serverPos);
                    previewPath.push_back(canonical);
                }
            }
        }
        if (previewPath.size() >= 2) {
            owner_.taxiPrecacheCallbackRef()(previewPath);
        }
    }

    if (owner_.taxiFlightStartCallbackRef()) {
        owner_.taxiFlightStartCallbackRef()();
    }
}

// ============================================================
// Area Trigger Detection (moved from GameHandler)
// ============================================================

void MovementHandler::loadAreaTriggerDbc() {
    if (owner_.areaTriggerDbcLoadedRef()) return;

    auto* am = owner_.services().assetManager;
    // Don't latch "loaded" until the asset manager is actually ready - the
    // first checkAreaTriggers() tick can land well before asset manager
    // init finishes (observed in headless mode), and latching here first
    // would permanently strand areaTriggersRef() empty with no retry and
    // no warning, silently disabling every area trigger for the process's
    // lifetime.
    if (!am || !am->isInitialized()) return;
    owner_.areaTriggerDbcLoadedRef() = true;

    auto dbc = am->loadDBC("AreaTrigger.dbc");
    if (!dbc || !dbc->isLoaded()) {
        LOG_WARNING("Failed to load AreaTrigger.dbc");
        return;
    }

    owner_.areaTriggersRef().reserve(dbc->getRecordCount());
    for (uint32_t i = 0; i < dbc->getRecordCount(); i++) {
        GameHandler::AreaTriggerEntry at;
        at.id     = dbc->getUInt32(i, 0);
        at.mapId  = dbc->getUInt32(i, 1);
        // DBC stores positions in server/wire format (X=west, Y=north) - swap to canonical
        at.x = dbc->getFloat(i, 3);  // canonical X (north) = DBC field 3 (Y_wire)
        at.y = dbc->getFloat(i, 2);  // canonical Y (west)  = DBC field 2 (X_wire)
        at.z = dbc->getFloat(i, 4);
        at.radius    = dbc->getFloat(i, 5);
        at.boxLength = dbc->getFloat(i, 6);
        at.boxWidth  = dbc->getFloat(i, 7);
        at.boxHeight = dbc->getFloat(i, 8);
        at.boxYaw    = dbc->getFloat(i, 9);
        owner_.areaTriggersRef().push_back(at);
    }

    LOG_DEBUG("Loaded ", owner_.areaTriggersRef().size(), " area triggers from AreaTrigger.dbc");
}

void MovementHandler::checkAreaTriggers() {
    if (!owner_.isInWorld()) return;
    if (onTaxiFlight_ || taxiClientActive_) return;

    loadAreaTriggerDbc();
    if (owner_.areaTriggersRef().empty()) return;

    const float px = movementInfo.x;
    const float py = movementInfo.y;
    const float pz = movementInfo.z;

    // Sanity: if position is near map origin on Eastern Kingdoms (map 0),
    // something has corrupted movementInfo - skip area trigger check to
    // avoid firing Alterac/Hillsbrad triggers and causing a rogue teleport.
    if (isCorruptOriginPosition(owner_.getCurrentMapId(), px, py, pz)) {
        if (!restoreWorldTransferFallbackIfNearOrigin("checkAreaTriggers")) {
            LOG_WARNING("checkAreaTriggers: position near map origin (", px, ", ", py, ", ", pz,
                        ") on map 0 - skipping to avoid rogue teleport. onTransport=",
                        owner_.isOnTransport(), " transportGuid=0x", std::hex,
                        owner_.playerTransportGuidRef(), std::dec);
            return;
        }
    }

    const float checkedPx = movementInfo.x;
    const float checkedPy = movementInfo.y;
    const float checkedPz = movementInfo.z;

    // Tested every frame at the current position only, as the stock client
    // does. The server (AzerothCore HandleAreaTriggerOpcode) re-tests the
    // trigger against ITS copy of the player position with zero tolerance
    // for non-tavern triggers, and rejects silently ("too far", debug level).
    // The old 0.25s poll plus swept samples could fire on a point the player
    // had already passed, or before the 0.5s heartbeat had told the server
    // where the player was; the server refused, the trigger was latched
    // active here, and nothing happened until the player walked out and back
    // in. That is "dungeon entrances only work sometimes".

    // Time-based cooldown after teleport/world entry - suppress ALL trigger
    // firing (not just the first check) to prevent re-entry from immediately
    // sending us to a wrong destination.
    const bool cooldownActive = owner_.areaTriggerCooldownRef() > 0.0f;

    // On first check after map transfer, just mark which triggers we're inside
    // without firing them - prevents exit portal from immediately sending us back
    bool suppressFirst = owner_.areaTriggerSuppressFirstRef();
    if (suppressFirst) {
        owner_.areaTriggerSuppressFirstRef() = false;
    }

    for (const auto& at : owner_.areaTriggersRef()) {
        if (at.mapId != owner_.currentMapIdRef()) continue;

        auto insideTrigger = [&at](float qx, float qy, float qz) -> bool {
            if (at.radius > 0.0f) {
                // Sphere trigger - use actual DBC radius
                float dx = qx - at.x;
                float dy = qy - at.y;
                float dz = qz - at.z;
                float distSq = dx * dx + dy * dy + dz * dz;
                return distSq <= at.radius * at.radius;
            }
            if (at.boxLength > 0.0f || at.boxWidth > 0.0f || at.boxHeight > 0.0f) {
                // Box trigger. AreaTrigger.dbc stores box axes in server-space
                // X/Y. The trigger center is cached in canonical space, so swap
                // deltas back to server-space before applying length/width/yaw.
                float serverDx = qy - at.y;
                float serverDy = qx - at.x;
                float dz = qz - at.z;

                // Rotate into box-local space
                float cosYaw = std::cos(-at.boxYaw);
                float sinYaw = std::sin(-at.boxYaw);
                float localX = serverDx * cosYaw - serverDy * sinYaw;
                float localY = serverDx * sinYaw + serverDy * cosYaw;

                return std::abs(localX) <= at.boxLength * 0.5f &&
                       std::abs(localY) <= at.boxWidth * 0.5f &&
                       std::abs(dz) <= at.boxHeight * 0.5f;
            }
            return false;
        };

        const bool inside = insideTrigger(checkedPx, checkedPy, checkedPz);

        // Near misses, once per trigger. A trigger that does not fire looks
        // exactly like a trigger that is not there, and the difference is a
        // number nobody can see: the sphere test is three-dimensional, so
        // standing on top of something with the right x and y and the wrong z
        // is outside it. Quest objectives that complete by walking somewhere
        // fail this way and say nothing at all.
        if (!inside && !nearMissLogged_.count(at.id)) {
            const float dx = checkedPx - at.x;
            const float dy = checkedPy - at.y;
            const float dz = checkedPz - at.z;
            const float flat = std::sqrt(dx * dx + dy * dy);
            const float extent = (at.radius > 0.0f)
                ? at.radius
                : std::max(at.boxLength, at.boxWidth) * 0.5f;
            // Walked past, not stood on. The margin used to be zero - the
            // player had to be inside the trigger's own flat extent for this
            // to say anything - and a portal box is three yards across, so
            // "I walked into it and nothing happened" was never the case this
            // reported. A few yards either side is what walking through one
            // looks like when the test refuses it.
            constexpr float kNearMissMargin = 5.0f;
            if (extent > 0.0f && flat <= extent + kNearMissMargin) {
                nearMissLogged_.insert(at.id);
                if (at.radius > 0.0f) {
                    LOG_WARNING("AreaTrigger near miss: AT", at.id,
                                " sphere r=", at.radius,
                                " flat=", flat, " dz=", dz,
                                " dist=", std::sqrt(dx * dx + dy * dy + dz * dz),
                                " (player=(", checkedPx, ", ", checkedPy, ", ", checkedPz,
                                ") trigger=(", at.x, ", ", at.y, ", ", at.z, "))");
                } else {
                    // The same rotation the test itself makes, so the numbers
                    // printed are the ones it compared: which of the three
                    // axes refused it, and by how much.
                    const float serverDx = checkedPy - at.y;
                    const float serverDy = checkedPx - at.x;
                    const float cosYaw = std::cos(-at.boxYaw);
                    const float sinYaw = std::sin(-at.boxYaw);
                    const float localX = serverDx * cosYaw - serverDy * sinYaw;
                    const float localY = serverDx * sinYaw + serverDy * cosYaw;
                    LOG_WARNING("AreaTrigger near miss: AT", at.id,
                                " box=(", at.boxLength, ", ", at.boxWidth, ", ", at.boxHeight,
                                ") yaw=", at.boxYaw,
                                " |local|=(", std::abs(localX), ", ", std::abs(localY),
                                ", ", std::abs(dz), ")",
                                " half=(", at.boxLength * 0.5f, ", ", at.boxWidth * 0.5f,
                                ", ", at.boxHeight * 0.5f, ")",
                                " (player=(", checkedPx, ", ", checkedPy, ", ", checkedPz,
                                ") trigger=(", at.x, ", ", at.y, ", ", at.z, "))");
                }
            }
        }

        if (inside) {
            if (owner_.activeAreaTriggersRef().count(at.id) == 0) {
                const bool suppressCurrentTrigger =
                    suppressFirst || cooldownActive || postTransferReturnAreaTriggerId_ == at.id;
                if (suppressFirst) {
                    // If we spawn already inside a portal trigger, arm it as
                    // active so cooldown expiry cannot immediately bounce us
                    // back through the destination portal. The player must
                    // leave and re-enter before it can fire.
                    if (postTransferReturnAreaTriggerId_ == at.id) {
                        postTransferReturnAreaTriggerSawNear_ = true;
                    }
                    owner_.activeAreaTriggersRef().insert(at.id);
                    LOG_INFO("AreaTrigger armed on post-transfer spawn: AT", at.id,
                                " cooldown=", owner_.areaTriggerCooldownRef());
                } else if (postTransferReturnAreaTriggerId_ == at.id) {
                    // After a map transfer, the destination can place or
                    // nudge the player into the paired return trigger after
                    // the normal grace window has elapsed. Arm that trigger
                    // until the player leaves it once; a later re-entry can
                    // then intentionally fire it.
                    postTransferReturnAreaTriggerSawNear_ = true;
                    owner_.activeAreaTriggersRef().insert(at.id);
                    LOG_WARNING("AreaTrigger armed for post-transfer return portal: AT", at.id,
                                " cooldown=", owner_.areaTriggerCooldownRef());
                } else if (cooldownActive) {
                    // Do not mark ordinary cooldown-suppressed triggers active.
                    // If the player walks into a trigger during the cooldown,
                    // it should fire once the grace window ends.
                    //
                    // Said out loud rather than at debug level. From the chair
                    // a suppressed portal and a portal that does not work are
                    // the same thing - the player walks in and nothing
                    // happens - and the window is ten seconds, which is long
                    // enough to walk into one after arriving.
                    LOG_WARNING("AreaTrigger cooldown suppressed: AT", at.id,
                                " cooldown=", owner_.areaTriggerCooldownRef(),
                                "s left - the player is inside it and it will "
                                "not fire until they leave and re-enter");
                } else {
                    owner_.activeAreaTriggersRef().insert(at.id);
                }

                if (suppressCurrentTrigger) {
                    // Suppressed above; if suppressFirst inserted the trigger,
                    // it will not fire until the player leaves/re-enters.
                } else {
                    // Tell the server where we are first. It re-tests the
                    // trigger against its own position for the player with no
                    // tolerance, and packets on a session are handled in
                    // order, so this heartbeat lands before the trigger does.
                    // Without it the server's copy can be up to one heartbeat
                    // (0.5s, ~3.5 yards at run speed) behind - more than half
                    // the depth of most portal boxes.
                    sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
                    network::Packet pkt(wireOpcode(Opcode::CMSG_AREATRIGGER));
                    pkt.writeUInt32(at.id);
                    lastAreaTriggerId_ = at.id;
                    if (const auto* dest = findKnownAreaTriggerDestinationByTrigger(at.id)) {
                        pendingAreaTriggerDestinationValid_ = true;
                        pendingAreaTriggerDestinationMapId_ = dest->mapId;
                        pendingAreaTriggerDestinationServerPos_ =
                            glm::vec3(dest->serverX, dest->serverY, dest->serverZ);
                        pendingAreaTriggerDestinationServerO_ = dest->serverO;
                    }
                    owner_.getSocket()->send(pkt);
                    const float dx = checkedPx - at.x;
                    const float dy = checkedPy - at.y;
                    LOG_WARNING("Fired CMSG_AREATRIGGER: id=", at.id,
                                " pos=(", checkedPx, ", ", checkedPy, ", ", checkedPz, ")",
                                " trigger=(", at.x, ", ", at.y, ", ", at.z, ")",
                                " dist2d=", std::sqrt(dx * dx + dy * dy));
                }
            }
        } else {
            if (postTransferReturnAreaTriggerId_ == at.id) {
                const float dx = checkedPx - at.x;
                const float dy = checkedPy - at.y;
                const float dz = checkedPz - at.z;
                const float distSq = dx * dx + dy * dy + dz * dz;
                const float triggerExtent = std::max({
                    at.radius,
                    at.boxLength * 0.5f,
                    at.boxWidth * 0.5f,
                    at.boxHeight * 0.5f,
                    12.0f
                });
                const float clearDistance = triggerExtent + 8.0f;
                if (distSq <= clearDistance * clearDistance) {
                    postTransferReturnAreaTriggerSawNear_ = true;
                    // A single outside sample near a destination portal is not
                    // enough to prove the player intentionally left it. Deeprun
                    // can report one outside tick and then drift back inside,
                    // which causes an immediate bounce. Keep the return portal
                    // armed until the player is comfortably clear.
                    owner_.activeAreaTriggersRef().insert(at.id);
                    LOG_DEBUG("AreaTrigger post-transfer return portal retained near trigger: AT", at.id,
                              " dist=", std::sqrt(distSq),
                              " clear=", clearDistance);
                    continue;
                }

                // Only while the post-transfer window is still open. The
                // distance test alone never expires: a player who has not been
                // near the portal and is far from it stays that way, so the
                // guard held for the rest of the session and that portal could
                // never be used again.
                //
                // Dying inside is exactly that shape. The release teleports the
                // player to a graveyard nowhere near the portal they came
                // through, without ever standing at it, so walking back as a
                // ghost was suppressed every time. The cooldown is ten seconds
                // and a destination position arrives in one, so anything still
                // far away after it has genuinely left.
                if (!postTransferReturnAreaTriggerSawNear_ && distSq > 1000.0f * 1000.0f &&
                    owner_.areaTriggerCooldownRef() > 0.0f) {
                    // During map transfer there can be a tick where the current
                    // map has switched but movementInfo still contains the old
                    // map's coordinates. That looks thousands of yards away
                    // from the destination trigger and must not clear the guard
                    // before the destination position arrives.
                    owner_.activeAreaTriggersRef().insert(at.id);
                    // Fires every frame for the brief window between a map switch and
                    // the destination position arriving - routine, not exceptional.
                    LOG_DEBUG("AreaTrigger post-transfer return portal waiting for destination position: AT",
                                at.id, " dist=", std::sqrt(distSq));
                    continue;
                }

                postTransferReturnAreaTriggerId_ = 0;
                postTransferReturnAreaTriggerSawNear_ = false;
                LOG_INFO("AreaTrigger post-transfer return portal cleared after leaving: AT", at.id,
                            " dist=", std::sqrt(distSq));
            }

            // Player left the trigger - allow re-fire on re-entry
            owner_.activeAreaTriggersRef().erase(at.id);
        }
    }
}

// ============================================================
// Transport Attachments (moved from GameHandler)
// ============================================================

void MovementHandler::setTransportAttachment(uint64_t childGuid, ObjectType type, uint64_t transportGuid,
                                             const glm::vec3& localOffset, bool hasLocalOrientation,
                                             float localOrientation,
                                             bool offsetNeedsTransportResolution) {
    if (childGuid == 0 || transportGuid == 0) {
        return;
    }

    GameHandler::TransportAttachment& attachment = owner_.transportAttachmentsRef()[childGuid];
    attachment.type = type;
    attachment.transportGuid = transportGuid;
    attachment.localOffset = localOffset;
    attachment.hasLocalOrientation = hasLocalOrientation;
    attachment.localOrientation = localOrientation;
    attachment.offsetNeedsTransportResolution = offsetNeedsTransportResolution;
}

void MovementHandler::clearTransportAttachment(uint64_t childGuid) {
    if (childGuid == 0) {
        return;
    }
    owner_.transportAttachmentsRef().erase(childGuid);
}

void MovementHandler::updateAttachedTransportChildren(float /*deltaTime*/) {
    if (!owner_.getTransportManager() || owner_.transportAttachmentsRef().empty()) {
        return;
    }

    constexpr float kPosEpsilonSq = 0.0001f;
    constexpr float kOriEpsilon = 0.001f;
    std::vector<uint64_t> stale;
    stale.reserve(8);

    for (auto& [childGuid, attachment] : owner_.transportAttachmentsRef()) {
        auto entity = owner_.getEntityManager().getEntity(childGuid);
        if (!entity) {
            stale.push_back(childGuid);
            continue;
        }

        ActiveTransport* transport = owner_.getTransportManager()->getTransport(attachment.transportGuid);
        if (!transport) {
            continue;
        }

        if (attachment.offsetNeedsTransportResolution) {
            attachment.localOffset = owner_.getTransportManager()->serverToTransportLocal(
                attachment.transportGuid, attachment.localOffset);
            attachment.offsetNeedsTransportResolution = false;
        }

        glm::vec3 composed = owner_.getTransportManager()->getPlayerWorldPosition(
            attachment.transportGuid, attachment.localOffset);

        float composedOrientation = entity->getOrientation();
        if (attachment.hasLocalOrientation) {
            // Client-owned ship routes do not retain hasServerYaw, but their
            // live quaternion still carries the tangent/dock rotation. Using
            // zero made attached NPCs face in a fixed world direction while
            // their deck turned beneath them.
            float baseYaw = 2.0f * std::atan2(transport->rotation.z, transport->rotation.w);
            composedOrientation = baseYaw + attachment.localOrientation;
        }

        glm::vec3 oldPos(entity->getX(), entity->getY(), entity->getZ());
        float oldOrientation = entity->getOrientation();
        glm::vec3 delta = composed - oldPos;
        const bool positionChanged = glm::dot(delta, delta) > kPosEpsilonSq;
        const bool orientationChanged = std::abs(composedOrientation - oldOrientation) > kOriEpsilon;
        if (!positionChanged && !orientationChanged) {
            continue;
        }

        // A unit whose own motion runs in the transport's frame is placed
        // without ending that motion.
        if (entity->hasStockMotion()) {
            entity->setWorldPositionFromFrame(composed.x, composed.y, composed.z, composedOrientation);
        } else {
            entity->setPosition(composed.x, composed.y, composed.z, composedOrientation);
        }

        if (attachment.type == ObjectType::UNIT) {
            if (owner_.creatureMoveCallbackRef()) {
                owner_.creatureMoveCallbackRef()(childGuid, composed.x, composed.y, composed.z, 0);
            }
        } else if (attachment.type == ObjectType::GAMEOBJECT) {
            if (owner_.gameObjectMoveCallbackRef()) {
                owner_.gameObjectMoveCallbackRef()(childGuid, composed.x, composed.y, composed.z, composedOrientation);
            }
        }
    }

    for (uint64_t guid : stale) {
        owner_.transportAttachmentsRef().erase(guid);
    }
}

// ============================================================
// Follow target (moved from GameHandler)
// ============================================================

void MovementHandler::followTarget() {
    if (owner_.getTargetGuid() == 0) {
        owner_.displayGameMessage("ERR_GENERIC_NO_TARGET", "You have no target.");
        return;
    }
    followUnit(owner_.getTargetGuid());
}

void MovementHandler::followUnit(uint64_t guid) {
    if (owner_.getState() != WorldState::IN_WORLD) {
        LOG_WARNING("Cannot follow: not in world");
        return;
    }
    // Wow.exe 0x72B4A0 with action 3: not yourself, and a unit in the world.
    if (guid == 0 || guid == owner_.getPlayerGuid()) return;
    auto target = owner_.getEntityManager().getEntity(guid);
    auto unit = target ? std::dynamic_pointer_cast<Unit>(target) : nullptr;
    if (!unit) return;

    // 0x729BD0: only a player, not charmed, of the follower's own side.
    const uint64_t charmedBy = [&]() -> uint64_t {
        const uint16_t idx = fieldIndex(UF::UNIT_FIELD_CHARMEDBY);
        if (idx == 0xFFFF) return 0;
        return static_cast<uint64_t>(unit->getField(idx)) |
               (static_cast<uint64_t>(unit->getField(static_cast<uint16_t>(idx + 1))) << 32);
    }();
    const bool validTarget = target->getType() == ObjectType::PLAYER && charmedBy == 0 &&
                             !owner_.isHostileFactionPublic(unit->getFactionTemplate());
    if (!validTarget) {
        owner_.displayGameMessage("ERR_INVALID_FOLLOW_TARGET", "You can't follow that unit.");
        return;
    }
    // The follower: alive, not stunned (UNIT_FLAG_STUNNED 0x40000), not
    // channelling.
    if (owner_.isPlayerDead() || owner_.isPlayerGhost()) {
        owner_.displayGameMessage("ERR_PLAYER_DEAD", "You can't do that when you're dead.");
        return;
    }
    if (auto me = owner_.getEntityManager().getEntity(owner_.getPlayerGuid())) {
        if (auto meUnit = std::dynamic_pointer_cast<Unit>(me); meUnit && (meUnit->getUnitFlags() & 0x40000u)) {
            owner_.displayGameMessage("ERR_GENERIC_STUNNED", "You are stunned");
            return;
        }
    }
    if (owner_.isChanneling()) {
        owner_.displayGameMessage("ERR_TOOBUSYTOFOLLOW", "You're too busy to follow anything!");
        return;
    }

    owner_.followTargetGuidRef() = guid;
    owner_.followRenderPosRef() = core::coords::canonicalToRender(
        glm::vec3(target->getX(), target->getY(), target->getZ()));
    if (owner_.autoFollowCallbackRef()) {
        owner_.autoFollowCallbackRef()(&owner_.followRenderPosRef());
    }
    std::string targetName = std::static_pointer_cast<Player>(target)->getName();
    if (targetName.empty()) targetName = owner_.lookupName(guid);
    // AUTOFOLLOW_BEGIN alone, whose status frame says it (Wow.exe 0x72B4A0).
    LOG_INFO("Following ", targetName, " (GUID: 0x", std::hex, guid, std::dec, ")");
    owner_.fireAddonEvent("AUTOFOLLOW_BEGIN", {targetName});
}

void MovementHandler::cancelFollow() {
    if (owner_.followTargetGuidRef() == 0) {
        return;
    }
    owner_.followTargetGuidRef() = 0;
    if (followMoveMoving_) {
        sendMovement(Opcode::MSG_MOVE_STOP);
        followMoveMoving_ = false;
    }
    if (owner_.autoFollowCallbackRef()) {
        owner_.autoFollowCallbackRef()(nullptr);
    }
    owner_.fireAddonEvent("AUTOFOLLOW_END", {});
}

// Same straight-line step used by the headless client's single-shot
// /movement/goto (updateMovementTask() in tools/headless_client/main.cpp),
// but the target position is read fresh from the live entity every call
// instead of a stored waypoint, and stops at kFollowStopDistance rather
// than snapping onto the target.
void MovementHandler::updateFollowMovement(float deltaTime) {
    const uint64_t guid = owner_.followTargetGuidRef();
    if (guid == 0) {
        return;
    }
    if (owner_.getState() != WorldState::IN_WORLD) {
        return;
    }
    if (isPlayerRooted() || !isServerMovementAllowed() || isOnTaxiFlight()) {
        if (followMoveMoving_) {
            sendMovement(Opcode::MSG_MOVE_STOP);
            followMoveMoving_ = false;
        }
        return;
    }

    auto target = owner_.getEntityManager().getEntity(guid);
    if (!target) {
        // cancelFollow() (called from GameHandler::update()'s
        // followRenderPos_ refresh) handles the disappeared-target case;
        // just stay quiet here rather than duplicate that check.
        return;
    }

    constexpr float kFollowStopDistance = 1.0f;
    const float dx = target->getX() - movementInfo.x;
    const float dy = target->getY() - movementInfo.y;
    const float dz = target->getZ() - movementInfo.z;
    const float horizontalDist = std::sqrt(dx * dx + dy * dy);
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

    if (distance <= kFollowStopDistance) {
        if (followMoveMoving_) {
            sendMovement(Opcode::MSG_MOVE_STOP);
            followMoveMoving_ = false;
        }
        return;
    }

    if (horizontalDist > 0.001f) {
        setOrientation(std::atan2(-dy, dx));
        sendMovement(Opcode::MSG_MOVE_SET_FACING);
    }
    if (!followMoveMoving_) {
        sendMovement(Opcode::MSG_MOVE_START_FORWARD);
        followMoveMoving_ = true;
    }

    const float speed = std::max(0.1f, getServerRunSpeed());
    const float step = std::min(distance - kFollowStopDistance, speed * std::max(0.0f, deltaTime));
    const float t = distance > 0.001f ? (step / distance) : 0.0f;
    setPosition(movementInfo.x + dx * t, movementInfo.y + dy * t, movementInfo.z + dz * t);
    sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
}

} // namespace game
} // namespace wowee
