#include "ui/scene_pick.hpp"
#include "ui/game_object_affordance.hpp"

#include "core/application.hpp"
#include "core/coordinates.hpp"
#include "game/game_handler.hpp"
#include "game/entity.hpp"
#include "game/update_field_table.hpp"
#include "rendering/camera.hpp"
#include "rendering/renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace wowee {
namespace ui {

namespace {

/// A wide forge or anvil would otherwise swallow the NPCs standing at it.
constexpr float kMaxGameObjectPickRadius = 3.0f;
/// ...and a mug is still worth a click at arm's length. Small enough that
/// scenery beside the player does not stand in front of what is being aimed
/// at, large enough that a small object is not a pixel hunt.
constexpr float kMinGameObjectPickRadius = 0.6f;
/// GAMEOBJECT_TYPE_CHAIR.
constexpr uint32_t kGoTypeChair = 7;

/// How far the pick ray looks for walls and ground at all. Bounded because
/// the hover runs this every frame and the WMO test walks every group the
/// ray's box reaches; past this, nothing occludes.
constexpr float kOcclusionReach = 120.0f;

/// What the stock client lets the mouse do with this object - see
/// game_object_affordance.hpp for the rule; this only gathers its inputs.
GameObjectAffordance affordanceOf(game::GameHandler& gameHandler, uint64_t guid,
                                  const game::GameObject& go,
                                  const game::GameObjectQueryResponseData* info) {
    GameObjectAffordanceInput in;
    if (info) {
        in.type = info->type;
        in.typeKnown = true;
        if (info->hasData) in.data = info->data;
    }

    auto field = [&](game::UF f, uint32_t fallback) {
        const uint16_t idx = game::fieldIndex(f);
        return idx != 0xFFFF && go.hasField(idx) ? go.getField(idx) : fallback;
    };
    in.flags = field(game::UF::GAMEOBJECT_FLAGS, 0);
    // An expansion whose layout does not name the dynamic field is taken as
    // activated, so an INTERACT_COND object is not shut for want of a field
    // this client cannot read.
    in.dynamicFlags = game::fieldIndex(game::UF::GAMEOBJECT_DYNAMIC) != 0xFFFF
                          ? field(game::UF::GAMEOBJECT_DYNAMIC, 0) & 0xFFFF
                          : kGoDynActivate;

    const uint64_t myGuid = gameHandler.getPlayerGuid();
    const uint64_t creator =
        (static_cast<uint64_t>(field(game::UF::GAMEOBJECT_FIELD_CREATED_BY_HI, 0)) << 32) |
        field(game::UF::GAMEOBJECT_FIELD_CREATED_BY_LO, 0);
    in.createdByPlayer = creator != 0 && creator == myGuid;
    // A bobber lives exactly as long as the fishing channel, so the player's
    // own bobber is their channel object.
    in.playersChannelObject =
        in.createdByPlayer || guid == gameHandler.getHookedFishingBobberGuid();

    // Whose side it is on: its maker's, if that is a unit in view, else its
    // own faction's, else nobody's.
    auto maker = creator != 0 && creator != myGuid
                     ? gameHandler.getEntityManager().getEntity(creator) : nullptr;
    if (maker && maker->isUnit()) {
        in.hostile = std::static_pointer_cast<game::Unit>(maker)->isHostile();
    } else if (const uint32_t faction = field(game::UF::GAMEOBJECT_FACTION, 0); faction != 0) {
        in.hostile = gameHandler.isHostileFactionPublic(faction);
    }

    const int questField = gameObjectQuestField(in.type);
    if (in.data && questField >= 0 && static_cast<int32_t>(in.data[questField]) > 0) {
        const uint32_t questId = in.data[questField];
        for (const auto& q : gameHandler.getQuestLog()) {
            if (q.questId == questId) { in.questInLog = true; break; }
        }
    }
    return gameObjectAffordance(in);
}

bool isDeadUnit(const std::shared_ptr<game::Entity>& entity) {
    auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
    return unit && unit->getHealth() == 0 && unit->getMaxHealth() > 0;
}

/// Where the ray first meets the terrain, or `reach` if it does not.
///
/// Marched in one-yard steps and refined by halving, which is far finer than
/// any hill. Skipped when the ray starts below the ground at its own origin -
/// a camera in a cave or a building cellar has terrain overhead that is not
/// in its way.
float terrainHitT(const rendering::TerrainManager* terrain, const rendering::Ray& ray,
                  float reach) {
    if (!terrain) return reach;
    auto below = [&](float t) {
        const glm::vec3 p = ray.origin + ray.direction * t;
        const auto h = terrain->getHeightAt(p.x, p.y);
        // A hole is open: getHeightAt interpolates straight across one, and
        // a cave or crypt entrance cut out of the ground is not a wall.
        return h && p.z < *h && !terrain->isHoleAt(p.x, p.y);
    };
    if (below(0.0f)) return reach;
    constexpr float kStep = 1.0f;
    float prev = 0.0f;
    for (float t = kStep; t <= reach; t += kStep) {
        if (!below(t)) { prev = t; continue; }
        float lo = prev, hi = t;
        for (int i = 0; i < 8; ++i) {
            const float mid = 0.5f * (lo + hi);
            (below(mid) ? hi : lo) = mid;
        }
        return hi;
    }
    return reach;
}

} // namespace

std::optional<glm::vec3> pickGround(const rendering::Ray& ray, float reach) {
    auto* renderer = core::Application::getInstance().getRenderer();
    if (!renderer) return std::nullopt;
    float t = terrainHitT(renderer->getTerrainManager(), ray, reach);
    if (auto* wmo = renderer->getWMORenderer()) {
        t = std::min(t, wmo->raycastVisibleSolid(ray.origin, ray.direction, reach, nullptr));
    }
    if (auto* m2 = renderer->getM2Renderer()) {
        t = std::min(t, m2->raycastCollisionMeshes(ray.origin, ray.direction, t, nullptr));
    }
    if (!(t < reach)) return std::nullopt;
    return ray.origin + ray.direction * t;
}

ScenePick pickScene(game::GameHandler& gameHandler,
                    const rendering::Ray& ray,
                    const ScenePickParams& params,
                    const ScenePickHit& onHit) {
    ScenePick pick;
    const uint64_t myGuid = gameHandler.getPlayerGuid();

    // The world in the ray's way. The stock client's pick stops at the first
    // opaque surface, so nothing behind a wall or a hill can be hovered or
    // clicked: not the bench inside Northshire Abbey seen from the yard.
    //
    // Three occluders, each found once per pick and kept apart so a rejected
    // click can say which one it was:
    //   terrain - the ground, holes open;
    //   WMO     - visible, solid building triangles only (rendered and not
    //             detail): no collision-only hulls, no railings;
    //   M2      - doodads that ship an authored collision mesh, tested per
    //             triangle: no ground clutter, no soft or box-only doodads.
    // The rule for every entity is then the same: it is pickable when the ray
    // enters its pick shape before it meets the first occluder that is not
    // the entity's own ground or its own model (see occluderIgnored below).
    float terrainT = kOcclusionReach;
    float wmoT = kOcclusionReach;
    float m2T = kOcclusionReach;
    std::string wmoDesc, m2Name;
    if (auto* renderer = core::Application::getInstance().getRenderer()) {
        terrainT = terrainHitT(renderer->getTerrainManager(), ray, kOcclusionReach);
        if (auto* wmo = renderer->getWMORenderer()) {
            wmoT = wmo->raycastVisibleSolid(ray.origin, ray.direction, kOcclusionReach, &wmoDesc);
        }
        if (auto* m2 = renderer->getM2Renderer()) {
            m2T = m2->raycastCollisionMeshes(ray.origin, ray.direction,
                                             std::min(terrainT, wmoT), &m2Name);
        }
    }
    const glm::vec3 terrainHit = ray.origin + ray.direction * terrainT;
    const glm::vec3 wmoHit = ray.origin + ray.direction * wmoT;
    const glm::vec3 m2Hit = ray.origin + ray.direction * m2T;

    for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
        if (!entity) continue;
        const auto type = entity->getType();
        if (type != game::ObjectType::UNIT &&
            type != game::ObjectType::PLAYER &&
            type != game::ObjectType::GAMEOBJECT) continue;
        if (guid == myGuid) continue;  // never target yourself

        // Nor the scenery. A trigger is an ordinary unit on the wire - it has
        // health, a name and a model - and UNIT_FLAG_NOT_SELECTABLE is the
        // server saying it is not there to be clicked.
        if (type == game::ObjectType::UNIT) {
            auto unit = std::static_pointer_cast<game::Unit>(entity);
            if (unit->getUnitFlags() & game::UNIT_FLAG_NOT_SELECTABLE) continue;
        }

        const game::GameObjectQueryResponseData* goInfo = nullptr;
        GameObjectAffordance goAffordance;
        if (type == game::ObjectType::GAMEOBJECT) {
            auto go = std::static_pointer_cast<game::GameObject>(entity);
            goInfo = gameHandler.getCachedGameObjectInfo(go->getEntry());
            if (params.skipChairs && goInfo && goInfo->type == kGoTypeChair) continue;
            // An object the mouse may not find is not there for it at all: no
            // hit, no name, no cursor, and the ray carries on to whatever
            // stands behind it. An instance portal's aura, a decorative column,
            // a ship. The stock pick drops these before it tests a single one.
            goAffordance = affordanceOf(gameHandler, guid, *go, goInfo);
            if (!goAffordance.highlight) continue;
        }

        glm::vec3 hitCenter;
        float hitRadius = 0.0f;
        const bool hasBounds =
            core::Application::getInstance().getRenderBoundsForGuid(guid, hitCenter, hitRadius);
        if (!hasBounds) {
            // Match the hover-cursor sizes, so the reticle agrees with the
            // cursor affordance - otherwise NPCs feel hard to click.
            float heightOffset = params.unitHeightOffset;
            hitRadius = params.unitHitRadius;
            if (type == game::ObjectType::UNIT) {
                auto unit = std::static_pointer_cast<game::Unit>(entity);
                // Critters have very low max health.
                if (unit->getMaxHealth() > 0 && unit->getMaxHealth() < 100) {
                    hitRadius = 0.5f;
                    heightOffset = 0.3f;
                }
            } else if (type == game::ObjectType::GAMEOBJECT) {
                hitRadius = 2.5f;
                heightOffset = 1.2f;
            }
            hitCenter = core::coords::canonicalToRender(
                glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
            hitCenter.z += heightOffset;
        } else if (type == game::ObjectType::GAMEOBJECT) {
            // An object's own size, a little forgiving - not a minimum of two
            // and a half yards.
            //
            // Every object was inflated to that floor whatever it was, so a mug
            // on a table and a chair beside the player wore the same sphere a
            // forge does. Standing near small scenery, the ray entered its
            // sphere on the way to whatever was actually being pointed at, and
            // the near thing won for being near: the click landed on the chair
            // rather than the door across the room.
            hitRadius = std::clamp(hitRadius * 1.15f, kMinGameObjectPickRadius,
                                   kMaxGameObjectPickRadius);
        } else {
            hitRadius = std::max(hitRadius * 1.25f, 1.0f);
        }

        float hitT = 0.0f;
        if (!raySphereIntersect(ray, hitCenter, hitRadius, hitT)) continue;
        const float centerT = glm::dot(hitCenter - ray.origin, ray.direction);

        // Occlusion: the ray must enter the pick shape before the first
        // occluder, give or take a hand's width. An occluder hit is ignored
        // when it is the entity's own footing - terrain or floor within its
        // pick radius of where it stands and no higher than its middle, which
        // is where a ray aimed at the legs meets the ground - or, for a
        // doodad, when it lies inside the entity's own pick sphere (a game
        // object's own model, a unit standing in a bush).
        {
            constexpr float kTolerance = 0.25f;
            const glm::vec3 foot = core::coords::canonicalToRender(
                glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
            auto ownFooting = [&](const glm::vec3& p) {
                const glm::vec2 d(p.x - foot.x, p.y - foot.y);
                return glm::dot(d, d) <= hitRadius * hitRadius && p.z <= hitCenter.z;
            };
            const char* blockedBy = nullptr;
            float blockT = 0.0f;
            if (terrainT < kOcclusionReach && hitT > terrainT + kTolerance &&
                !ownFooting(terrainHit)) {
                blockedBy = "terrain"; blockT = terrainT;
            } else if (wmoT < kOcclusionReach && hitT > wmoT + kTolerance &&
                       !ownFooting(wmoHit)) {
                blockedBy = "WMO"; blockT = wmoT;
            } else if (m2T < kOcclusionReach && hitT > m2T + kTolerance &&
                       glm::length(m2Hit - hitCenter) > hitRadius + 0.5f) {
                blockedBy = "M2"; blockT = m2T;
            }
            if (blockedBy) {
                if (params.logRejections) {
                    // Rate-limited: a click that misses says why, a hover
                    // sweep across a crowd does not flood the log.
                    using Clock = std::chrono::steady_clock;
                    static Clock::time_point lastLog{};
                    const auto now = Clock::now();
                    if (now - lastLog > std::chrono::milliseconds(250)) {
                        lastLog = now;
                        const std::string what =
                            std::string(blockedBy) == "WMO" ? " (" + wmoDesc + ")"
                            : std::string(blockedBy) == "M2" ? " ('" + m2Name + "')" : "";
                        LOG_WARNING("Pick: click on '", game::entityDisplayName(entity),
                                    "' (guid 0x", std::hex, guid, std::dec,
                                    ") rejected - occluded by ", blockedBy, what,
                                    " at t=", blockT, ", shape entered at t=", hitT,
                                    " centre t=", centerT, " radius=", hitRadius,
                                    " [terrain t=", terrainT, " wmo t=", wmoT,
                                    " m2 t=", m2T, "]");
                    }
                }
                continue;
            }
        }

        // Reach is not the picker's business. The stock client finds an
        // object wherever the pointer is on it and keeps its highlight and
        // name out of reach; only the cursor turns "Unable" (CanUseNow
        // 0x70FA70 behind 0x70CE10), and a click from there is refused with
        // "You are too far away." (0x70F680).

        if (type == game::ObjectType::UNIT || type == game::ObjectType::PLAYER) {
            // Rank the living ahead of the dead rather than by distance alone. A
            // corpse lies at ground level while a unit's sphere sits a metre up,
            // so looking down at someone standing on a body put the body nearer.
            if (isDeadUnit(entity)) {
                if (centerT < pick.deadUnitCenterT) {
                    pick.deadUnitCenterT = centerT;
                    pick.deadUnitGuid = guid;
                }
            } else {
                if (centerT < pick.livingUnitCenterT) {
                    pick.livingUnitCenterT = centerT;
                    pick.livingUnitGuid = guid;
                }
                if (type == game::ObjectType::UNIT) {
                    auto unit = std::static_pointer_cast<game::Unit>(entity);
                    // A corpse still answers isHostile(), and a hostile is picked
                    // ahead of everything - hence the liveness check above it.
                    const bool hostile = unit->isHostile() ||
                                         gameHandler.isAggressiveTowardPlayer(guid);
                    if (hostile && hitT < pick.hostileUnitT) {
                        pick.hostileUnitT = hitT;
                        pick.hostileUnitGuid = guid;
                    }
                }
            }
        } else if (type == game::ObjectType::GAMEOBJECT) {
            // Found, but perhaps not usable: a signpost or an anvil is named
            // under the plain pointer and takes no click.
            const bool interactive = goAffordance.interact;
            // How well the ray is aimed at this one, as a fraction of its own
            // size: nought is dead centre and one is a graze. Ranked by that
            // rather than by which centre is nearest along the ray, because
            // "nearest" is the question that made scenery at the player's elbow
            // beat the thing they were pointing at - a ray that grazes the edge
            // of a near sphere is still a nearer hit than one straight through
            // the middle of a far one.
            const float centreOffset = glm::length(hitCenter - (ray.origin + ray.direction * centerT));
            const float aim = hitRadius > 0.0f ? centreOffset / hitRadius : 1.0f;
            if (interactive && aim < pick.objectAim) {
                pick.objectAim = aim;
                pick.objectCenterT = centerT;
                pick.objectGuid = guid;
            }
            // Named whether or not it can be used. A sign takes no click and
            // still says what it is when the pointer is on it.
            if (aim < pick.namedObjectAim) {
                pick.namedObjectAim = aim;
                pick.namedObjectGuid = guid;
            }
        }

        if (hitT < pick.closestT) {
            pick.closestT = hitT;
            pick.closestGuid = guid;
        }

        if (onHit) onHit(guid, entity, hitT, centerT);
    }

    return pick;
}

} // namespace ui
} // namespace wowee
