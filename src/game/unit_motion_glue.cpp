// src/game/unit_motion_glue.cpp
// Packets in, motion events out, for the stock unit motion. See
// include/game/unit_motion_glue.hpp and docs/re/unit-motion.md.
#include "game/unit_motion_glue.hpp"

#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/transport_manager.hpp"
#include "game/unit_motion.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"

#include <cmath>

namespace wowee::game {

namespace {

constexpr uint32_t kWalkBit = static_cast<uint32_t>(MovementFlags::WALKING);
constexpr uint32_t kFallingBit = static_cast<uint32_t>(MovementFlags::FALLING);
constexpr uint32_t kFlyingBit = static_cast<uint32_t>(MovementFlags::FLYING);
constexpr uint32_t kHoverBit = static_cast<uint32_t>(MovementFlags::HOVER);
constexpr uint32_t kJumpAnim = 38;  // Jump: the locomotion takes it as "a jump began"

motion::MonsterMoveSpec specFrom(const MonsterMoveData& d) {
    motion::MonsterMoveSpec spec;
    spec.start = glm::vec3(d.x, d.y, d.z);
    spec.moveType = d.moveType;
    spec.facingSpot = glm::vec3(d.facingSpotX, d.facingSpotY, d.facingSpotZ);
    spec.facingTarget = d.facingTarget;
    spec.facingAngle = d.facingAngle;
    spec.splineFlags = d.splineFlags;
    spec.durationMs = d.duration;
    spec.hasAnimation = d.hasAnimation;
    spec.animationType = d.animationType;
    spec.animationStartMs = d.animationStartTime;
    spec.hasParabolic = d.hasParabolic;
    spec.verticalAcceleration = d.verticalAcceleration;
    spec.parabolicStartMs = d.parabolicStartTime;
    if (d.hasDest) {
        spec.points.reserve(d.waypoints.size() + 1);
        for (const auto& wp : d.waypoints) spec.points.emplace_back(wp.x, wp.y, wp.z);
        spec.points.emplace_back(d.destX, d.destY, d.destZ);
    }
    return spec;
}

/// The facing (canonical yaw) from `from` (canonical) to a unit, if both are known.
std::optional<float> canonicalYawToEntity(GameHandler& gh, const Entity& from, uint64_t target) {
    if (target == 0) return std::nullopt;
    glm::vec3 tp;
    if (target == gh.getPlayerGuid()) {
        const auto& mi = gh.getMovementInfo();
        tp = glm::vec3(mi.x, mi.y, mi.z);
    } else {
        auto t = gh.getEntityManager().getEntity(target);
        if (!t) return std::nullopt;
        tp = glm::vec3(t->getX(), t->getY(), t->getZ());
    }
    const float dx = tp.x - from.getX(), dy = tp.y - from.getY();
    if (dx * dx + dy * dy < 1e-4f) return std::nullopt;
    return std::atan2(-dy, dx);
}

/// What a monster move tells the animation and the renderer.
void afterMonsterMove(GameHandler& gh, Entity& e, const motion::MonsterMovePlan& plan) {
    const uint64_t guid = e.getGuid();
    if (plan.kind == motion::MonsterMovePlan::Kind::Place &&
        plan.immediateFacing == motion::FinalFacing::Target) {
        if (auto yaw = canonicalYawToEntity(gh, e, plan.immediateTarget)) e.setOrientation(*yaw);
    }
    uint32_t durationMs = 0;
    if (e.hasStockMotion() && e.stockMotionIsSpline()) {
        // Walk or run is the pace (0x717050): faster than twice the unit's
        // walk speed runs. The spline flags' WalkMode only chose the
        // server's speed.
        const float speed = e.stockMotionSpeed();
        const bool walking = !(speed > e.moveSpeeds().walk + e.moveSpeeds().walk);
        if (auto& cb = gh.unitMoveFlagsCallbackRef()) cb(guid, walking ? kWalkBit : 0u, kWalkBit);
        if (speed > 0.0f) {
            durationMs = 1;  // "moving": the callback only needs to know that
        }
    }
    if (auto& cb = gh.creatureMoveCallbackRef()) {
        cb(guid, e.getX(), e.getY(), e.getZ(), durationMs);
    }
}

void syncTransportAttachment(GameHandler& gh, Entity& e, uint64_t transportGuid) {
    auto* tm = gh.getTransportManager();
    if (!tm || transportGuid == 0) return;
    const glm::vec3 local = tm->serverToTransportLocal(transportGuid, e.stockMotionFramePosition());
    const float localYaw = core::coords::serverToCanonicalYaw(e.stockMotionFrameFacing());
    gh.setTransportAttachment(e.getGuid(), e.getType(), transportGuid, local, true, localYaw);
    if (tm->getTransport(transportGuid)) {
        const glm::vec3 world = tm->getPlayerWorldPosition(transportGuid, local);
        e.setWorldPositionFromFrame(world.x, world.y, world.z, e.getOrientation());
    }
}

}  // namespace

void applyStockMonsterMove(GameHandler& gh, Entity& e, const MonsterMoveData& data) {
    // 0xDD carries no transport: whatever the unit stood on, it has left
    // (0x73C8E0 -> 0x6F0C70 with no transport guid) - a vehicle's seat too
    // (the exit spline, TransportExit).
    gh.clearTransportAttachment(e.getGuid());
    if (gh.vehiclePassenger(e.getGuid())) gh.setVehiclePassenger(e.getGuid(), 0, -1);
    const motion::MonsterMovePlan plan = e.startMonsterMove(specFrom(data), false);
    afterMonsterMove(gh, e, plan);
}

bool handleStockMonsterMoveTransport(GameHandler& gh, network::Packet& packet) {
    if (!packet.hasRemaining(2)) return false;
    MonsterMoveData data;
    data.guid = packet.readPackedGuid();
    if (!packet.hasRemaining(2)) return false;
    const uint64_t transportGuid = packet.readPackedGuid();
    if (!packet.hasRemaining(1)) return false;
    const int8_t seat = static_cast<int8_t>(packet.readUInt8());
    if (data.guid == 0 || transportGuid == 0) return false;
    if (!MonsterMoveParser::parseBody(packet, data)) return false;
    // Into a vehicle's seat (the boarding spline, TransportEnter): the seat
    // places the passenger (VehiclePassenger_C); the boarding arc is not
    // drawn.
    if (gh.isVehicleTransport(transportGuid)) {
        gh.setVehiclePassenger(data.guid, transportGuid, seat);
        return true;
    }
    auto entity = gh.getEntityManager().getEntity(data.guid);
    if (!entity) return true;
    const motion::MonsterMovePlan plan = entity->startMonsterMove(specFrom(data), true);
    syncTransportAttachment(gh, *entity, transportGuid);
    afterMonsterMove(gh, *entity, plan);
    return true;
}

bool readMovementBlockWotlk(network::Packet& p, MovementInfo& info) {
    if (!p.hasRemaining(4 + 2 + 4 + 16)) return false;
    info.flags = p.readUInt32();
    info.flags2 = p.readUInt16();
    info.time = p.readUInt32();
    info.x = p.readFloat();
    info.y = p.readFloat();
    info.z = p.readFloat();
    info.orientation = p.readFloat();
    if (info.flags & static_cast<uint32_t>(MovementFlags::ONTRANSPORT)) {
        if (!p.hasRemaining(1)) return false;
        info.transportGuid = p.readPackedGuid();
        if (!p.hasRemaining(16 + 4 + 1)) return false;
        info.transportX = p.readFloat();
        info.transportY = p.readFloat();
        info.transportZ = p.readFloat();
        info.transportO = p.readFloat();
        info.transportTime = p.readUInt32();
        info.transportSeat = static_cast<int8_t>(p.readUInt8());
        // MOVEMENTFLAG2_INTERPOLATED_MOVEMENT (0x400), not 0x200 (0x4F4D40).
        if (info.flags2 & 0x0400) {
            if (!p.hasRemaining(4)) return false;
            info.transportTime2 = p.readUInt32();
        }
    }
    if ((info.flags & (static_cast<uint32_t>(MovementFlags::SWIMMING) |
                       static_cast<uint32_t>(MovementFlags::FLYING))) ||
        (info.flags2 & 0x0020)) {
        if (!p.hasRemaining(4)) return false;
        info.pitch = p.readFloat();
    }
    if (!p.hasRemaining(4)) return false;
    info.fallTime = p.readUInt32();
    if (info.flags & kFallingBit) {
        if (!p.hasRemaining(16)) return false;
        info.jumpVelocity = p.readFloat();
        info.jumpSinAngle = p.readFloat();   // the x of the jump's direction
        info.jumpCosAngle = p.readFloat();   // its y
        info.jumpXYSpeed = p.readFloat();
    }
    if (info.flags & 0x04000000) {  // SPLINE_ELEVATION
        if (!p.hasRemaining(4)) return false;
        /*float splineElevation =*/ p.readFloat();
    }
    return true;
}

void applyRemoteMovement(GameHandler& gh, Entity& e, const MovementInfo& info, bool onTransport) {
    motion::MovementSample s;
    s.time = info.time;
    s.flags = info.flags;
    s.flags2 = info.flags2;
    if (onTransport) {
        s.position = glm::vec3(info.transportX, info.transportY, info.transportZ);
        s.facing = info.transportO;
    } else {
        s.position = glm::vec3(info.x, info.y, info.z);
        s.facing = info.orientation;
    }
    s.pitch = info.pitch;
    s.fallTimeMs = info.fallTime;
    s.jumpVelocity = info.jumpVelocity;
    // On the wire the jump's direction is (x, y) in that order - AzerothCore
    // calls them sin and cos, the client stores them as its +0x70/+0x74
    // direction (0x988990).
    s.jumpCos = info.jumpSinAngle;
    s.jumpSin = info.jumpCosAngle;
    s.jumpXYSpeed = info.jumpXYSpeed;

    // On a vehicle's seat (a unit, 0x74B8B0): the seat places it; the
    // packet's world position is where it is meanwhile.
    if (onTransport && gh.isVehicleTransport(info.transportGuid)) {
        gh.setVehiclePassenger(e.getGuid(), info.transportGuid, info.transportSeat);
        gh.clearTransportAttachment(e.getGuid());
        s.position = glm::vec3(info.x, info.y, info.z);
        s.facing = info.orientation;
        e.receiveRemoteMovement(s, false);
        return;
    }
    if (gh.vehiclePassenger(e.getGuid())) gh.setVehiclePassenger(e.getGuid(), 0, -1);
    if (!onTransport) gh.clearTransportAttachment(e.getGuid());
    e.receiveRemoteMovement(s, onTransport);
    if (onTransport) {
        if (e.stockMotionFrameValidInTransport()) {
            syncTransportAttachment(gh, e, info.transportGuid);
        } else if (auto* tm = gh.getTransportManager()) {
            // Not applied yet (held for its time): stand where the packet says.
            const glm::vec3 local = tm->serverToTransportLocal(
                info.transportGuid, glm::vec3(info.transportX, info.transportY, info.transportZ));
            gh.setTransportAttachment(e.getGuid(), e.getType(), info.transportGuid, local, true,
                                      core::coords::serverToCanonicalYaw(info.transportO));
        }
    }
}

void applyRemoteSpeed(GameHandler& gh, uint64_t guid, SpeedKind kind, float speed) {
    if (guid == 0 || guid == gh.getPlayerGuid()) return;
    if (!std::isfinite(speed) || speed <= 0.0f || speed > 200.0f) return;
    auto e = gh.getEntityManager().getEntity(guid);
    if (!e) return;
    motion::MoveSpeeds& sp = e->moveSpeeds();
    switch (kind) {
        case SpeedKind::Walk: sp.walk = speed; break;
        case SpeedKind::Run: sp.run = speed; break;
        case SpeedKind::RunBack: sp.runBack = speed; break;
        case SpeedKind::Swim: sp.swim = speed; break;
        case SpeedKind::SwimBack: sp.swimBack = speed; break;
        case SpeedKind::Flight: sp.flight = speed; break;
        case SpeedKind::FlightBack: sp.flightBack = speed; break;
        case SpeedKind::TurnRate: sp.turnRate = speed; break;
        case SpeedKind::PitchRate: sp.pitchRate = speed; break;
    }
}

void applyCreateSpeeds(Entity& e, const float s[9]) {
    auto ok = [](float v) { return std::isfinite(v) && v > 0.0f && v < 200.0f; };
    motion::MoveSpeeds& sp = e.moveSpeeds();
    if (ok(s[0])) sp.walk = s[0];
    if (ok(s[1])) sp.run = s[1];
    if (ok(s[2])) sp.runBack = s[2];
    if (ok(s[3])) sp.swim = s[3];
    if (ok(s[4])) sp.swimBack = s[4];
    if (ok(s[5])) sp.flight = s[5];
    if (ok(s[6])) sp.flightBack = s[6];
    if (ok(s[7])) sp.turnRate = s[7];
    if (ok(s[8])) sp.pitchRate = s[8];
}

void applyCreateMovement(Entity& e, const UpdateBlock& block) {
    if (!block.hasMovement) return;
    if (block.runSpeed > 0.0f) {
        const float speeds[9] = {block.walkSpeed, block.runSpeed, block.runBackSpeed,
                                 block.swimSpeed, block.swimBackSpeed, block.flightSpeed,
                                 block.flightBackSpeed, block.turnRate, block.pitchRate};
        applyCreateSpeeds(e, speeds);
    }
    e.setBaseMoveFlags(block.moveFlags, motion::MoveFlags::Walking | motion::MoveFlags::Swimming |
                                            motion::MoveFlags::Flying | motion::MoveFlags::CanFly |
                                            motion::MoveFlags::Hover);
    e.setBaseMoveFlags2(block.moveFlags2);
    if (!isActiveExpansion("wotlk")) return;
    if (block.hasSpline && (block.moveFlags & motion::MoveFlags::SplineEnabled)) {
        motion::CreateSplineSpec spec;
        spec.splineFlags = block.splineFlags;
        spec.timePassedMs = block.splineTimePassed;
        spec.durationMs = block.splineDuration;
        spec.durationMod = block.splineDurationMod;
        spec.durationModNext = block.splineDurationModNext;
        spec.verticalAcceleration = block.splineVerticalAccel;
        spec.effectStartMs = block.splineEffectStart;
        spec.nodes = block.splineNodes;
        spec.mode = block.splineMode;
        spec.endPoint = block.splineEndPoint;
        switch (block.splineFacingKind) {
            case 1: spec.finalFacing = motion::FinalFacing::Spot;
                    spec.facingSpot = block.splineFacingSpot; break;
            case 2: spec.finalFacing = motion::FinalFacing::Target;
                    spec.facingTarget = block.splineFacingTarget; break;
            case 3: spec.finalFacing = motion::FinalFacing::Angle;
                    spec.facingAngle = block.splineFacingAngle; break;
            default: break;
        }
        if (e.startCreateSpline(spec, block.onTransport)) return;
    }
    if ((block.moveFlags & motion::MoveFlags::Moving) != 0 &&
        (block.moveFlags & motion::MoveFlags::SplineEnabled) == 0) {
        motion::MovementSample s;
        s.time = block.moveTime;
        s.flags = block.moveFlags;
        s.flags2 = block.moveFlags2;
        if (block.onTransport) {
            s.position = glm::vec3(block.transportX, block.transportY, block.transportZ);
            s.facing = block.transportO;
        } else {
            s.position = glm::vec3(block.x, block.y, block.z);
            s.facing = block.orientation;
        }
        s.pitch = block.pitch;
        s.fallTimeMs = block.fallTime;
        s.jumpVelocity = block.jumpVelocity;
        s.jumpCos = block.jumpDirX;
        s.jumpSin = block.jumpDirY;
        s.jumpXYSpeed = block.jumpXYSpeed;
        e.receiveRemoteMovement(s, block.onTransport);
    }
}

void processStockMotionFrame(GameHandler& gh, Entity& e) {
    const uint32_t ev = e.takeMotionEvents();
    const uint64_t guid = e.getGuid();
    if (ev != 0) {
        auto& flagsCb = gh.unitMoveFlagsCallbackRef();
        if (ev & Entity::MOTION_JUMP_STARTED) {
            // A parabolic spline (MoveJump, a charge's leap): the client
            // raises MSG_MOVE_JUMP for it (0x6E9C30 -> event 0xBB).
            if (flagsCb) flagsCb(guid, kFallingBit, kFallingBit);
            if (auto& hint = gh.unitAnimHintCallbackRef()) hint(guid, kJumpAnim);
        }
        if (ev & Entity::MOTION_LANDED) {
            if (flagsCb) flagsCb(guid, 0u, kFallingBit);
        }
        if (ev & Entity::MOTION_REMOTE_APPLIED) {
            // The flags of the packet that just took effect: direction,
            // walking, swimming, flying, falling (0x6ED990 -> 0x73ED10).
            if (flagsCb) flagsCb(guid, e.stockMotionFlags(), ~0u);
        }
        if (ev & Entity::MOTION_REMOTE_JUMPED) {
            if (auto& hint = gh.unitAnimHintCallbackRef()) hint(guid, kJumpAnim);
        }
        if (ev & Entity::MOTION_TIER_CHANGED) {
            // The animation tier (UNIT_FIELD_BYTES_1 byte 3, 0x73AF00):
            // 0 ground, 2 hover, 3 fly.
            const uint8_t tier = e.stockMotionAnimTier();
            const uint32_t bits = (tier == 3 ? kFlyingBit : 0u) | (tier == 2 ? kHoverBit : 0u);
            if (flagsCb) flagsCb(guid, bits, kFlyingBit | kHoverBit);
        }
    }
    if (auto fin = e.takeSplineFinish(); fin && fin->kind == motion::FinalFacing::Target) {
        if (auto yaw = canonicalYawToEntity(gh, e, fin->target)) e.setOrientation(*yaw);
    }
    if (e.stockMotionFrameValidInTransport()) {
        auto& attachments = gh.transportAttachmentsRef();
        auto it = attachments.find(guid);
        if (it != attachments.end() && gh.getTransportManager()) {
            it->second.localOffset = gh.getTransportManager()->serverToTransportLocal(
                it->second.transportGuid, e.stockMotionFramePosition());
            it->second.localOrientation =
                core::coords::serverToCanonicalYaw(e.stockMotionFrameFacing());
            it->second.hasLocalOrientation = true;
            it->second.offsetNeedsTransportResolution = false;
        }
    }
}

}  // namespace wowee::game
