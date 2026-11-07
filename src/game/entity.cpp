#include "game/entity.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <chrono>

namespace wowee {
namespace game {

void EntityManager::addEntity(uint64_t guid, std::shared_ptr<Entity> entity) {
    if (!entity) {
        LOG_WARNING("Attempted to add null entity with GUID: 0x", std::hex, guid, std::dec);
        return;
    }

    const int type = static_cast<int>(entity->getType());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entities[guid] = std::move(entity);
        spatialDirty_ = true;
    }

    LOG_DEBUG("Added entity: GUID=0x", std::hex, guid, std::dec,
              ", Type=", type);
}

void EntityManager::removeEntity(uint64_t guid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entities.find(guid);
    if (it != entities.end()) {
        LOG_DEBUG("Removed entity: GUID=0x", std::hex, guid, std::dec);
        entities.erase(it);
        spatialDirty_ = true;
    }
}

std::shared_ptr<Entity> EntityManager::getEntity(uint64_t guid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entities.find(guid);
    return (it != entities.end()) ? it->second : nullptr;
}

bool EntityManager::hasEntity(uint64_t guid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entities.find(guid) != entities.end();
}

std::vector<std::shared_ptr<Entity>> EntityManager::getEntitiesNear(
        float x, float y, float radius) const {
    std::vector<std::shared_ptr<Entity>> result;
    getEntitiesNear(x, y, radius, result);
    return result;
}

void EntityManager::getEntitiesNear(float x, float y, float radius,
                                    std::vector<std::shared_ptr<Entity>>& result) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (spatialDirty_ || lastSpatialRebuild_.time_since_epoch().count() == 0 ||
        now - lastSpatialRebuild_ >= std::chrono::milliseconds(250)) {
        spatialCells_.clear();
        spatialCells_.reserve(entities.size() / 4 + 1);
        auto cellKey = [](int32_t cx, int32_t cy) -> int64_t {
            return static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) |
                                        static_cast<uint32_t>(cy));
        };
        for (const auto& [guid, entity] : entities) {
            (void)guid;
            if (!entity) continue;
            const int32_t cx = static_cast<int32_t>(std::floor(entity->getLatestX() / kSpatialCellSize));
            const int32_t cy = static_cast<int32_t>(std::floor(entity->getLatestY() / kSpatialCellSize));
            spatialCells_[cellKey(cx, cy)].push_back(entity);
        }
        lastSpatialRebuild_ = now;
        spatialDirty_ = false;
    }

    const int32_t minX = static_cast<int32_t>(std::floor((x - radius) / kSpatialCellSize));
    const int32_t maxX = static_cast<int32_t>(std::floor((x + radius) / kSpatialCellSize));
    const int32_t minY = static_cast<int32_t>(std::floor((y - radius) / kSpatialCellSize));
    const int32_t maxY = static_cast<int32_t>(std::floor((y + radius) / kSpatialCellSize));
    const float radiusSq = radius * radius;
    result.clear();
    result.reserve(std::min(entities.size(), size_t(256)));
    auto cellKey = [](int32_t cx, int32_t cy) -> int64_t {
        return static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) |
                                    static_cast<uint32_t>(cy));
    };
    for (int32_t cy = minY; cy <= maxY; ++cy) {
        for (int32_t cx = minX; cx <= maxX; ++cx) {
            auto it = spatialCells_.find(cellKey(cx, cy));
            if (it == spatialCells_.end()) continue;
            for (const auto& entity : it->second) {
                const float dx = entity->getLatestX() - x;
                const float dy = entity->getLatestY() - y;
                if (dx * dx + dy * dy <= radiusSq) result.push_back(entity);
            }
        }
    }
}

// ── Stock unit motion ────────────────────────────────────────────────────

namespace {
uint32_t (*g_motionClockOverride)() = nullptr;
}  // namespace

uint32_t Entity::motionClockMs() {
    if (g_motionClockOverride) return g_motionClockOverride();
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now() - t0).count());
}

void Entity::setMotionClockOverride(uint32_t (*clock)()) { g_motionClockOverride = clock; }

void Entity::publishMotionFrame(const glm::vec3& serverPos, float serverFacing) {
    motionFramePos_ = serverPos;
    motionFrameFacing_ = serverFacing;
    motionFrameValid_ = true;
    if (motionTransportFrame_) return;  // the transport places it
    const glm::vec3 c = core::coords::serverToCanonical(serverPos);
    x = c.x;
    y = c.y;
    z = c.z;
    orientation = core::coords::serverToCanonicalYaw(serverFacing);
}

namespace {

motion::MoverSnapshot snapshotFor(const glm::vec3& framePos, float frameFacing, float run) {
    motion::MoverSnapshot m;
    m.position = framePos;
    m.facing = frameFacing;
    m.runSpeed = run;
    return m;
}

}  // namespace

motion::MonsterMovePlan Entity::startMonsterMove(const motion::MonsterMoveSpec& spec,
                                                 bool transportFrame) {
    const uint32_t now = motionClockMs();
    // Where this client has the unit now, in the spline's frame.
    if (motionKind_ != MotionKind::None) updateStockMotion();
    glm::vec3 pos;
    float facing;
    if (!transportFrame) {
        pos = core::coords::canonicalToServer(glm::vec3(x, y, z));
        facing = core::coords::canonicalToServerYaw(orientation);
    } else if (motionTransportFrame_ && motionFrameValid_) {
        pos = motionFramePos_;
        facing = motionFrameFacing_;
    } else {
        // Newly on this transport: where it is on the deck is the packet's word.
        pos = spec.start;
        facing = core::coords::canonicalToServerYaw(orientation);
    }

    // Leaving any other motion; the new one starts from here.
    isMoving_ = false;
    usePathMode_ = false;
    deadReckon_ = false;
    activeSpline_.reset();
    if (remoteMover_) remoteMover_->reset();
    unitSpline_.reset();
    motionKind_ = MotionKind::None;
    motionTransportFrame_ = transportFrame;
    motionTranslating_ = false;
    motionAirborne_ = false;
    motionSpeed_ = 0.0f;
    motionFlags_ = 0;

    motion::MonsterMovePlan plan =
        motion::planMonsterMove(spec, snapshotFor(pos, facing, moveSpeeds_.run), now);
    switch (plan.kind) {
        case motion::MonsterMovePlan::Kind::Stop:
            publishMotionFrame(pos, facing);
            break;
        case motion::MonsterMovePlan::Kind::Place: {
            float f = facing;
            if (plan.immediateFacing == motion::FinalFacing::Angle) {
                f = plan.immediateAngle;
            } else if (plan.immediateFacing == motion::FinalFacing::Spot) {
                const glm::vec3 d = plan.immediateSpot - plan.position;
                if (d.x * d.x + d.y * d.y > 1e-6f) f = std::atan2(d.y, d.x);
            }
            publishMotionFrame(plan.position, f);
            motionCorrection_ += plan.position - pos;
            break;
        }
        case motion::MonsterMovePlan::Kind::Spline: {
            unitSpline_ = std::move(plan.spline);
            plan.spline.reset();
            motionKind_ = MotionKind::Spline;
            motionFlags_ = motion::MoveFlags::Forward |
                           (baseMoveFlags_ & (motion::MoveFlags::Walking |
                                              motion::MoveFlags::Swimming |
                                              motion::MoveFlags::Flying));
            // An effect that starts at once starts now (0x6E96C0, 0x6E9780).
            if (unitSpline_->effectStartMs == 0) {
                if (unitSpline_->flags & motion::SplineFlags::Parabolic) {
                    unitSpline_->effectStarted = true;
                    motionEvents_ |= MOTION_JUMP_STARTED;
                } else if (unitSpline_->flags & motion::SplineFlags::Animation) {
                    unitSpline_->effectStarted = true;
                    motionAnimTier_ = static_cast<uint8_t>(unitSpline_->flags &
                                                           motion::SplineFlags::AnimationMask);
                    motionEvents_ |= MOTION_TIER_CHANGED;
                }
            }
            motionFramePos_ = pos;
            motionFrameFacing_ = facing;
            updateStockMotion();
            // The path starts at the unit, but a ground path it stands beside
            // starts at its foot on it (0x7180C0): that step is the only jump.
            motionCorrection_ += motionFramePos_ - pos;
            break;
        }
    }
    return plan;
}

bool Entity::startCreateSpline(const motion::CreateSplineSpec& spec, bool transportFrame) {
    auto sp = motion::splineFromCreate(spec, motionClockMs());
    if (!sp) return false;
    isMoving_ = false;
    usePathMode_ = false;
    deadReckon_ = false;
    activeSpline_.reset();
    if (remoteMover_) remoteMover_->reset();
    unitSpline_ = std::move(sp);
    motionKind_ = MotionKind::Spline;
    motionTransportFrame_ = transportFrame;
    motionFlags_ = motion::MoveFlags::Forward |
                   (baseMoveFlags_ & (motion::MoveFlags::Walking | motion::MoveFlags::Swimming |
                                      motion::MoveFlags::Flying));
    motionFramePos_ = unitSpline_->curve.evaluate(0.0f).position;
    motionFrameFacing_ = core::coords::canonicalToServerYaw(orientation);
    updateStockMotion();
    return true;
}

void Entity::receiveRemoteMovement(const motion::MovementSample& sample, bool transportFrame) {
    const uint32_t now = motionClockMs();
    if (!remoteMover_) remoteMover_.emplace();
    if (motionKind_ != MotionKind::Remote || motionTransportFrame_ != transportFrame) {
        remoteMover_->reset();
        remoteAppliedSeen_ = 0;
        if (motionTransportFrame_ != transportFrame) motionFrameValid_ = false;
    }
    isMoving_ = false;
    usePathMode_ = false;
    deadReckon_ = false;
    activeSpline_.reset();
    unitSpline_.reset();
    motionKind_ = MotionKind::Remote;
    motionTransportFrame_ = transportFrame;
    remoteMover_->speeds = moveSpeeds_;
    remoteMover_->receive(sample, now);
    updateStockMotion();
}

void Entity::updateStockMotion() {
    const uint32_t now = motionClockMs();
    if (motionKind_ == MotionKind::Spline && unitSpline_) {
        const motion::UnitSpline::Frame f =
            unitSpline_->update(now, motionFlags_ | baseMoveFlags_);
        float facing = f.facing.value_or(motionFrameFacing_);
        if (f.hasPitch) motionPitch_ = f.pitch;
        motionHasAttitude_ = f.hasAttitude && !f.finished;
        motionAttitudePitch_ = f.attitudePitch;
        motionBank_ = f.bank;
        motionSpeed_ = unitSpline_->speed();
        const uint32_t sf = unitSpline_->flags;
        motionAirborne_ = !f.finished &&
            (((sf & motion::SplineFlags::Parabolic) && unitSpline_->effectStarted) ||
             (sf & motion::SplineFlags::Falling));
        if (f.jumpStarted) motionEvents_ |= MOTION_JUMP_STARTED;
        if (f.animationStarted) {
            motionAnimTier_ = f.animationTier;
            motionEvents_ |= MOTION_TIER_CHANGED;
        }
        if (!f.finished) {
            motionTranslating_ = motionSpeed_ > 0.0f;
            publishMotionFrame(f.position, facing);
            return;
        }
        // The end (0x6EB0B0): on the end point, the facing it asked for.
        glm::vec3 endPos = f.position;
        SplineFinish fin;
        fin.kind = unitSpline_->finalFacing;
        fin.serverAngle = unitSpline_->facingAngle;
        fin.spot = unitSpline_->facingSpot;
        fin.target = unitSpline_->facingTarget;
        if (fin.kind == motion::FinalFacing::Angle) {
            facing = fin.serverAngle;
        } else if (fin.kind == motion::FinalFacing::Spot) {
            const glm::vec3 d = fin.spot - endPos;
            if (d.x * d.x + d.y * d.y > 1e-6f) facing = std::atan2(d.y, d.x);
        }
        if (fin.kind != motion::FinalFacing::None) splineFinish_ = fin;
        motionEvents_ |= MOTION_SPLINE_DONE;
        if (f.landed) motionEvents_ |= MOTION_LANDED;
        publishMotionFrame(endPos, facing);
        unitSpline_.reset();
        motionKind_ = MotionKind::None;
        motionTranslating_ = false;
        motionAirborne_ = false;
        motionHasAttitude_ = false;
        motionSpeed_ = 0.0f;
        motionFlags_ = 0;
        isMoving_ = false;
        return;
    }
    if (motionKind_ == MotionKind::Remote && remoteMover_) {
        remoteMover_->speeds = moveSpeeds_;
        const uint32_t prevFlags = remoteMover_->state().flags;
        remoteMover_->update(now);
        const motion::MoveState& st = remoteMover_->state();
        if (remoteMover_->appliedCount() != remoteAppliedSeen_) {
            // A packet took effect: its flags reach the animation now, not
            // when it arrived (the stock movement event fires at apply time).
            remoteAppliedSeen_ = remoteMover_->appliedCount();
            motionEvents_ |= MOTION_REMOTE_APPLIED;
            const auto& applied = remoteMover_->lastApplied();
            if ((applied.flags & motion::MoveFlags::Falling) &&
                !(prevFlags & motion::MoveFlags::Falling) && applied.jumpVelocity < 0.0f &&
                applied.fallTimeMs < 100) {
                motionEvents_ |= MOTION_REMOTE_JUMPED;
            }
        }
        motionFlags_ = st.flags;
        motionPitch_ = st.pitch;
        motionAirborne_ = st.falling();
        motionTranslating_ = (st.flags & (motion::MoveFlags::Directions |
                                          motion::MoveFlags::Ascending |
                                          motion::MoveFlags::Descending)) != 0;
        motionSpeed_ = !motionTranslating_ ? 0.0f
                     : st.falling() ? st.jumpXYSpeed
                                    : motion::selectSpeed(moveSpeeds_, st.flags);
        motionCorrection_ += remoteMover_->takeCorrection();
        if (remoteMover_->appliedCount() > 0) publishMotionFrame(st.position, st.facing);
        return;
    }
    motionKind_ = MotionKind::None;
}

} // namespace game
} // namespace wowee
