// include/game/unit_motion_glue.hpp
//
// Where the packets meet the stock unit motion (game/unit_motion.hpp): the
// movement handlers hand a monster move, a remote player's movement packet or
// a speed change to these, and the per-frame entity update hands each unit's
// motion events back out to the callbacks the renderer listens on.
#pragma once

#include <cstdint>

namespace wowee::network { class Packet; }

namespace wowee::game {

class GameHandler;
class Entity;
struct MonsterMoveData;
struct MovementInfo;
struct UpdateBlock;

/// SMSG_MONSTER_MOVE (WotLK) for a unit or player in view.
void applyStockMonsterMove(GameHandler& gh, Entity& entity, const MonsterMoveData& data);

/// SMSG_MONSTER_MOVE_TRANSPORT (WotLK). Reads the whole packet: packed mover
/// guid, packed transport guid, seat, then the common monster-move body.
/// Answers false when the packet is not one it can read.
bool handleStockMonsterMoveTransport(GameHandler& gh, network::Packet& packet);

/// Another unit's MSG_MOVE_* (or a compressed one). `info` holds everything
/// the movement block carried: on a transport, x/y/z and orientation are the
/// world ones and transportX.. the local ones.
void applyRemoteMovement(GameHandler& gh, Entity& entity, const MovementInfo& info,
                         bool onTransport);

/// Parse a WotLK movement block (after the guid) the way the client does
/// (0x4F4D40): flags, flags2, time, position, transport, pitch, fall time,
/// jump, spline elevation. False if it runs out.
bool readMovementBlockWotlk(network::Packet& packet, MovementInfo& info);

enum class SpeedKind : uint8_t {
    Walk, Run, RunBack, Swim, SwimBack, Flight, FlightBack, TurnRate, PitchRate
};

/// A speed for a unit this client does not control (MSG_MOVE_SET_*_SPEED,
/// SMSG_SPLINE_SET_*_SPEED, SMSG_FORCE_*_SPEED_CHANGE for someone else).
void applyRemoteSpeed(GameHandler& gh, uint64_t guid, SpeedKind kind, float speed);

/// The speeds a unit's create block carried.
void applyCreateSpeeds(Entity& entity, const float speeds[9]);

/// A unit (not the local player) came into view: its speeds and flags, and
/// if it was already under way - on a spline, or a player running - the
/// motion it was in the middle of (the stock client picks either up from
/// the create block; Wowee used to leave it standing until its next packet).
void applyCreateMovement(Entity& entity, const UpdateBlock& block);

/// After Entity::updateMovement: jumps, landings, tier changes and the
/// facing a finished spline asked for; and a unit moving on a transport
/// keeps its attachment where its motion is.
void processStockMotionFrame(GameHandler& gh, Entity& entity);

}  // namespace wowee::game
