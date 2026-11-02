#include "rendering/camera_controller.hpp"

#include "core/env_flag.hpp"
#include "rendering/swim_wall.hpp"
#include "core/click_drag.hpp"
#include "core/coordinates.hpp"
#include "ui/keybinding_manager.hpp"
#include "ui/framexml_takeover.hpp"
#include "rendering/movement_limits.hpp"
#include "game/stock_movement.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <future>
#include <sstream>
#include <string>
#include "core/thread_pool.hpp"
#include <imgui.h>
#include "rendering/terrain_manager.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "game/opcodes.hpp"
#include "core/logger.hpp"
#include <glm/glm.hpp>
#include <cmath>
#include <limits>

namespace wowee {
namespace rendering {

namespace {

constexpr float kMaxPhysicsDelta = 1.0f / 30.0f;

/// WoW's own key bone id for the head, as the M2 skeleton names it.
constexpr int32_t kKeyBoneHead = 6;

std::optional<float> selectReachableFloor(const std::optional<float>& terrainH,
                                          const std::optional<float>& wmoH,
                                          float refZ,
                                          float maxStepUp) {
    // Filter to reachable floors (not too far above)
    std::optional<float> reachTerrain;
    std::optional<float> reachWmo;
    if (terrainH && *terrainH <= refZ + maxStepUp) reachTerrain = terrainH;
    if (wmoH && *wmoH <= refZ + maxStepUp) reachWmo = wmoH;

    if (reachTerrain && reachWmo) {
        // Prefer the highest surface - prevents clipping through
        // WMO floors that sit above terrain.
        return (*reachWmo >= *reachTerrain) ? reachWmo : reachTerrain;
    }
    if (reachWmo) return reachWmo;
    if (reachTerrain) return reachTerrain;
    return std::nullopt;
}

std::optional<float> selectHighestFloor(const std::optional<float>& a,
                                        const std::optional<float>& b,
                                        const std::optional<float>& c) {
    std::optional<float> best;
    auto consider = [&](const std::optional<float>& h) {
        if (!h) return;
        if (!best || *h > *best) best = h;
    };
    consider(a);
    consider(b);
    consider(c);
    return best;
}

std::optional<float> selectClosestFloor(const std::optional<float>& a,
                                        const std::optional<float>& b,
                                        float refZ) {
    if (a && b) {
        float da = std::abs(*a - refZ);
        float db = std::abs(*b - refZ);
        return (da <= db) ? a : b;
    }
    if (a) return a;
    if (b) return b;
    return std::nullopt;
}

std::optional<float> selectReachableFloor3(const std::optional<float>& a,
                                           const std::optional<float>& b,
                                           const std::optional<float>& c,
                                           float refZ,
                                           float maxStepUp) {
    std::optional<float> best;
    auto consider = [&](const std::optional<float>& h) {
        if (!h) return;
        if (*h > refZ + maxStepUp) return;
        if (!best || *h > *best) best = h;
    };
    consider(a);
    consider(b);
    consider(c);
    return best;
}


/// How flat a surface has to be to stand on rather than slide off.
///
/// Three names for one number. They are kept apart because the surfaces are:
/// a WMO tunnel or bridge ramp is authored steeper than outdoor terrain, and if
/// that ever needs different limits this is where they part. Today they agree.
constexpr float MIN_WALKABLE_NORMAL_TERRAIN = movement::kMinWalkableNormalZ;
constexpr float MIN_WALKABLE_NORMAL_WMO = movement::kMinWalkableNormalZ;
constexpr float MIN_WALKABLE_NORMAL_M2 = movement::kMinWalkableNormalZ;

/// How far from the feet's centre line a floor still holds the player up in the
/// ordinary floor probes. Stock stands a character on a surface only when the
/// point under it is over a walkable triangle; a fifth of a yard bridges plank
/// gaps and tread seams without letting a ledge or a rail carry the player
/// past its edge. The rescue rings (which run only when nothing at all was
/// found) keep their wider footprints.
constexpr float kFloorFootprint = 0.2f;

/// The points around the feet a floor probe samples.
///
/// One ray straight down at the character's centre falls between two planks, or
/// misses a stair tread, and the character drops through a floor that is plainly
/// there. Sampling a ring around the feet instead is what stops it. Five points
/// for a cheap check, nine when something has already gone wrong and a wider
/// footprint is worth the queries.
///
/// The ring was written out five times, each with its own footprint constant
/// spelled into nine brace pairs.
std::array<glm::vec2, 5> feetCross(float footprint) {
    return {{ {0.0f, 0.0f},
              { footprint, 0.0f}, {-footprint, 0.0f},
              {0.0f,  footprint}, {0.0f, -footprint} }};
}

std::array<glm::vec2, 9> feetRing(float footprint) {
    return {{ {0.0f, 0.0f},
              { footprint, 0.0f}, {-footprint, 0.0f},
              {0.0f,  footprint}, {0.0f, -footprint},
              { footprint,  footprint}, { footprint, -footprint},
              {-footprint,  footprint}, {-footprint, -footprint} }};
}

/// The band a floor has to be in to be believed: high enough to step onto,
/// low enough not to be the ceiling or another storey.
struct FloorBand {
    float minZ;
    float maxZ;
};

/// The highest walkable floor among the sampled points, if any.
///
/// `query` is a renderer's getFloorHeight - it answers a height and writes the
/// surface normal's Z, and a normal flatter than `minNormalZ` is a wall rather
/// than something to stand on.
template <typename Offsets, typename Query>
std::optional<float> highestWalkableFloor(Query&& query, float x, float y,
                                          const Offsets& offsets, float probeZ,
                                          float minNormalZ, FloorBand band) {
    std::optional<float> best;
    for (const auto& o : offsets) {
        float normalZ = 1.0f;
        auto h = query(x + o.x, y + o.y, probeZ, &normalZ);
        if (!h) continue;
        if (normalZ < minNormalZ) continue;
        if (*h > band.maxZ || *h < band.minZ) continue;
        if (!best || *h > *best) best = h;
    }
    return best;
}

} // namespace

CameraController::CameraController(Camera* cam) : camera(cam) {
    yaw = defaultYaw;
    facingYaw = defaultYaw;
    pitch = defaultPitch;
    reset();
}

/// The floor dumps, which are a hunt rather than a fault.
///
/// Each is ten lines of every WMO group and doodad near a position, written
/// whenever a floor probe comes up empty - which happens in ordinary play and
/// is recovered from. In one six-minute session they were ninety-one lines of
/// eleven hundred, at the level these logs carry, describing something that
/// then worked. The one-line statement that a probe found nothing stays; the
/// inventory behind it is asked for.
///
/// F8 still dumps on demand, which is somebody asking for exactly this.
bool floorDiagEnabled() {
    static const bool on = core::envFlagEnabled("WOWEE_FLOOR_DIAG", false);
    return on;
}

void CameraController::startIntroPan(float durationSec, float orbitDegrees) {
    if (!camera) return;
    introActive = true;
    introTimer = 0.0f;
    idleTimer_ = 0.0f;
    introDuration = std::max(0.5f, durationSec);
    introStartYaw = yaw;
    introEndYaw = yaw - orbitDegrees;
    introOrbitDegrees = orbitDegrees;
    introStartPitch = pitch;
    introEndPitch = pitch;
    introStartDistance = currentDistance;
    introEndDistance = currentDistance;
    thirdPerson = true;
}
float CameraController::raymarchTerrainCameraLimit(const glm::vec3& pivot, const glm::vec3& camDir,
                                                   float maxDist) const {
    if (!terrainManager || maxDist <= MIN_DISTANCE) return maxDist;

    // Clearance the camera keeps above the terrain surface along the whole ray.
    // Enough to keep the near plane out of the ground and no more: the quarter
    // yard on top of the sphere that used to be here was felt as the ground
    // pushing the camera forward on every rise.
    constexpr float kClearance = CAM_SPHERE_RADIUS + 0.08f;

    // If the pivot itself sits below the heightfield (caves, WMO basements,
    // terrain holes under cities), the heightfield is not the relevant occluder
    // here - skip limiting rather than pinning the camera to first-person.
    auto pivotH = terrainManager->getHeightAt(pivot.x, pivot.y);
    if (pivotH && pivot.z < *pivotH - 0.2f) return maxDist;

    // Coarse march in ~1.25-unit steps (capped so worst case stays cheap),
    // then bisect between the last clear sample and the first blocked one.
    const int steps = std::clamp(static_cast<int>(maxDist / 1.25f) + 1, 4, 24);
    const float stepLen = maxDist / static_cast<float>(steps);
    float prevT = 0.0f;
    for (int i = 1; i <= steps; ++i) {
        float t = stepLen * static_cast<float>(i);
        glm::vec3 p = pivot + camDir * t;
        auto h = terrainManager->getHeightAt(p.x, p.y);
        if (h && p.z < *h + kClearance) {
            float lo = prevT;
            float hi = t;
            for (int j = 0; j < 4; ++j) {
                float mid = 0.5f * (lo + hi);
                glm::vec3 mp = pivot + camDir * mid;
                auto mh = terrainManager->getHeightAt(mp.x, mp.y);
                if (mh && mp.z < *mh + kClearance) hi = mid; else lo = mid;
            }
            return std::max(MIN_DISTANCE, lo);
        }
        prevT = t;
    }
    return maxDist;
}

std::optional<float> CameraController::cameraTerrainFloor(float x, float y,
                                                         const glm::vec3& targetPos) const {
    if (!terrainManager) return std::nullopt;
    auto feetH = terrainManager->getHeightAt(targetPos.x, targetPos.y);
    if (feetH && targetPos.z < *feetH - 0.2f) return std::nullopt;

    // The near plane is wider than a point. On a ridge or a bank the ground a
    // few hand-widths to the side can stand well above the ground straight
    // below, and a camera held clear of only the latter looks into the hill.
    constexpr float kFootprint = 0.35f;
    //
    // A hole-cut quad is a cave mouth or a way underground; the height across
    // it is interpolated and is not ground, so it holds nothing up.
    std::optional<float> best;
    for (const auto& [ox, oy] : {std::pair{0.0f, 0.0f},
                                 std::pair{kFootprint, 0.0f}, std::pair{-kFootprint, 0.0f},
                                 std::pair{0.0f, kFootprint}, std::pair{0.0f, -kFootprint}}) {
        if (terrainManager->isHoleAt(x + ox, y + oy)) continue;
        auto h = terrainManager->getHeightAt(x + ox, y + oy);
        if (h && (!best || *h > *best)) best = h;
    }
    return best;
}

void CameraController::triggerShake(float magnitude, float frequency, float duration) {
    // Scaled here rather than at each caller, so the setting covers the sources
    // that exist - a spell's shake and a thunderstorm's - and any added later.
    magnitude *= shakeScale_;
    // Allow stronger shake to override weaker; don't allow zero magnitude.
    if (magnitude <= 0.0f || duration <= 0.0f) return;
    if (magnitude > shakeMagnitude_ || shakeElapsed_ >= shakeDuration_) {
        shakeMagnitude_ = magnitude;
        shakeFrequency_ = frequency;
        shakeDuration_  = duration;
        shakeElapsed_   = 0.0f;
    }
}

glm::vec3 CameraController::sweepAgainstWalls(const glm::vec3& from, const glm::vec3& to,
                                              bool includeDoodads) {
    const glm::vec3 delta = to - from;
    const float distSq = glm::dot(delta, delta);
    if (distSq <= 1e-4f) return to;   // stationary: not worth a collision call

    const float dist = std::sqrt(distSq);
    // The character's own box (CreatureModelData, per race, form and mount):
    // a gnome is 1.06 tall and a draenei 0.43 wide where everyone was a 0.45
    // or 0.5 cylinder 2 yd tall.
    const WMORenderer::Body body{collider_.halfWidth, collider_.height, collider_.stepHeight};
    // Tighter steps indoors, where the geometry is closer together - and never
    // longer than the radius, or a sub-step could start outside a thin wall
    // and end past its plane without either end being inside the cylinder.
    const float stepSize = std::min(cachedInsideWMO ? 0.20f : 0.35f, 0.9f * body.radius);
    const int steps = std::max(1, std::min(8, static_cast<int>(std::ceil(dist / stepSize))));
    const glm::vec3 stepDelta = delta / static_cast<float>(steps);

    glm::vec3 stepPos = from;
    for (int i = 0; i < steps; i++) {
        glm::vec3 candidate = stepPos + stepDelta;

        if (wmoRenderer) {
            glm::vec3 adjusted;
            if (wmoRenderer->checkWallCollision(stepPos, candidate, adjusted, cachedInsideWMO, &body)) {
                candidate.x = adjusted.x;
                candidate.y = adjusted.y;
                // Up is a ramp; down would be a wall pulling the feet under the floor.
                candidate.z = std::max(candidate.z, adjusted.z);
            }
        }

        if (includeDoodads && m2Renderer && !externalFollow_) {
            glm::vec3 adjusted;
            if (m2Renderer->checkCollision(stepPos, candidate, adjusted, body.radius)) {
                candidate.x = adjusted.x;
                candidate.y = adjusted.y;
            }
        }

        stepPos = candidate;
    }
    return stepPos;
}

// The camera that orbits a character and moves it.
//
// Collision, grounding, swimming, flight, the zoom, and the pullback that keeps
// the camera out of walls. This was the larger half of a two-thousand-line
// update(), and everything it needs from the half before it arrives in `f`.
// Where the followed character wants to be this frame.
//
// The movement intent - walking, swimming, flying, jumping, falling - and the
// swept wall collision that keeps it out of geometry. Answers the position it
// arrived at; the floor has not been consulted yet.
void CameraController::setFlyingActive(bool active) {
    // Said when it changes, as take-off and landing are: whether space climbs
    // or jumps comes down to this one flag from the server, and a report that
    // the mount "will not fly" is otherwise a guess at whether it was sent.
    if (active != flyingActive_) {
        LOG_WARNING("Flight: ", active ? "the server allows flying here"
                                       : "flying is no longer allowed");
    }
    flyingActive_ = active;
    if (!active) flightAirborne_ = false;
}

glm::vec3 CameraController::moveFollowedCharacter(float /*deltaTime*/, FrameInput& f,
                                                  glm::vec3& prevTargetPos) {
    // Move the follow target (character position) instead of the camera
    prevTargetPos = *followTarget;
    glm::vec3 targetPos = *followTarget;
    if (!externalFollow_) {
        if (wmoRenderer) {
            wmoRenderer->setCollisionFocus(targetPos, COLLISION_FOCUS_RADIUS_THIRD_PERSON);
        }
        if (m2Renderer) {
            m2Renderer->setCollisionFocus(targetPos, COLLISION_FOCUS_RADIUS_THIRD_PERSON);
        }
    }

    if (!externalFollow_) {
        // Whether the character swims: the stock rule (0x730D10), the liquid
        // over the feet against the collision box's height - in deeper than
        // 0.75 h, out below 0.75 h - 1/36. A wader standing on the bottom has
        // the feet on the floor, so that is the water over the floor; a body
        // falling in is measured where it is, and not while it is still on the
        // way up out of a jump. The thresholds scale with the race: a gnome
        // swims in 0.79 yd of water, a human 1.52, a draenei 1.91. They were a
        // fixed 1.15 in / 0.85 out of water over the floor for everyone.
        namespace sm = game::stock_movement;
        const float colliderH = collider_.height;
        std::optional<float> waterH;
        if (waterRenderer) {
            waterH = waterRenderer->getWaterHeightAt(targetPos.x, targetPos.y);
        }
        constexpr float MAX_SWIM_DEPTH_FROM_SURFACE = 12.0f;
        bool inWater = false;
        surfaceHopThisFrame_ = false;
        // Water Walk: treat water surface as ground - player walks on top, not through.
        if (waterWalkActive_ && waterH && targetPos.z >= *waterH - 0.5f) {
            // Clamp to water surface so the player stands on it
            targetPos.z = *waterH;
            verticalVelocity = 0.0f;
            grounded = true;
            inWater = false;
        } else if (waterH && targetPos.z < *waterH) {
            std::optional<uint16_t> waterType;
            if (waterRenderer) {
                waterType = waterRenderer->getWaterTypeAt(targetPos.x, targetPos.y);
            }
            bool isOcean = false;
            if (waterType && *waterType != 0) {
                isOcean = (((*waterType - 1) % 4) == 1);
            }
            // Depth gate only applies when ENTERING swim (e.g. don't start
            // swimming in a dry cave beneath a lake's water plane). Once
            // swimming, any depth is fine - you can only get deep by
            // deliberately diving through the water column.
            bool depthAllowed = swimming || isOcean ||
                                ((*waterH - targetPos.z) <= MAX_SWIM_DEPTH_FROM_SURFACE);
            if (depthAllowed) {
                const float depthFromFeet = *waterH - targetPos.z;
                const bool risingInJump = !swimming && !grounded && verticalVelocity > 0.0f;
                inWater = swimming
                    ? sm::keepsSwimming(depthFromFeet, colliderH, true, true)
                    : sm::startsSwimming(depthFromFeet, colliderH, true, risingInJump);

                // Ledge exits. The stock swim trace has no step: a swimmer
                // meets a dock or a shelf as a wall and hops out at the surface
                // (below). Wowee's wall test leaves anything within a step of
                // the feet to the floor pass, which a swimmer does not run, so
                // a ledge in front of or under a swimmer heading for it ends
                // the swim and the ground mover's step takes it - when the
                // water over that ledge is shallower than the stock exit depth.
                const float exitDepth = sm::swimExitDepth(colliderH);
                if (swimming && inWater && f.nowForward && f.forward3D.z > -0.20f && !f.xDown) {
                    auto queryFloorAt = [&](float x, float y, float probeZ) -> std::optional<float> {
                        std::optional<float> best;
                        if (terrainManager) {
                            best = terrainManager->getHeightAt(x, y);
                        }
                        if (wmoRenderer) {
                            float nz = 1.0f;
                            auto wh = wmoRenderer->getFloorHeight(x, y, probeZ, &nz);
                            if (wh && nz >= 0.40f && (!best || *wh > *best)) best = wh;
                        }
                        if (m2Renderer && !externalFollow_) {
                            float nz = 1.0f;
                            auto mh = m2Renderer->getFloorHeight(x, y, probeZ, &nz);
                            if (mh && nz >= 0.35f && (!best || *mh > *best)) best = mh;
                        }
                        return best;
                    };
                    const float step = collider_.stepHeight;
                    const float probeZ = targetPos.z + step + 1.0f;
                    // Under the feet, then ahead along the swim.
                    std::optional<float> ledge;
                    if (auto h = queryFloorAt(targetPos.x, targetPos.y, probeZ)) ledge = h;
                    glm::vec2 fwd2(f.forward.x, f.forward.y);
                    float fwdLenSq = glm::dot(fwd2, fwd2);
                    if (fwdLenSq > 1e-8f) {
                        fwd2 *= glm::inversesqrt(fwdLenSq);
                        const float reach = std::max(collider_.halfWidth, step * sm::kTan50);
                        for (float d : {0.45f, 0.90f, reach}) {
                            auto h = queryFloorAt(targetPos.x + fwd2.x * d,
                                                  targetPos.y + fwd2.y * d, probeZ);
                            if (h && (!ledge || *h > *ledge)) ledge = h;
                        }
                    }
                    if (ledge) {
                        const float rise = *ledge - targetPos.z;
                        const bool nearSurface = depthFromFeet <= swimFloatDepth() + 0.2f;
                        const bool withinStep = rise >= -0.35f && rise <= step;
                        const bool shallowOverLedge = (*waterH - *ledge) < exitDepth;
                        if (nearSurface && withinStep && shallowOverLedge) inWater = false;
                    }
                }

                // Ascend held at the surface jumps out of the water (0x730D10:
                // ASCENDING within 1/3 yd of the float line -> 0x72EB80, a
                // JUMP from the water at 9.0967 yd/s). On foot always; mounted
                // only while moving or able to fly. Rooted refuses it as it
                // refuses any jump (0x9883F0).
                const bool moving = f.nowForward || f.nowBackward ||
                                    f.nowStrafeLeft || f.nowStrafeRight;
                if (swimming && inWater && useWoWSpeed && f.spaceHeld && !movementRooted_ &&
                    (!mounted_ || moving || flyingActive_) &&
                    sm::surfaceHopDue(true, depthFromFeet, colliderH)) {
                    inWater = false;
                    surfaceHopThisFrame_ = true;
                    verticalVelocity = sm::kWaterJumpVelocity;
                    grounded = false;
                }
            }
        }
        // Keep swimming through water-data gaps at chunk boundaries.
        if (!inWater && swimming && !waterH) {
            inWater = true;
        }


        if (inWater) {
        swimming = true;
        // Swim movement follows look pitch (f.forward/back), while strafe stays
        // lateral for stable control.
        // The swim pair is its own (0x987570): walking does not slow a
        // swimmer, and with no server value the defaults are 4.72 / 2.5.
        const game::stock_movement::Speeds stockSp = stockSpeeds();
        float swimSpeed = useWoWSpeed ? stockSp.swim
            : ((swimSpeedOverride_ > 0.0f && swimSpeedOverride_ < 100.0f && !std::isnan(swimSpeedOverride_))
                   ? swimSpeedOverride_ : f.speed * SWIM_SPEED_FACTOR);
        float waterSurfaceZ = waterH ? (*waterH - swimFloatDepth()) : targetPos.z;

        // For auto-run/auto-swim: use character facing (immune to camera pan)
        // For manual W key: use camera direction (swim where you look)
        glm::vec3 swimForward;
        if (autoRunning || (leftMouseDown && rightMouseDown)) {
            // Auto-running: use character's horizontal facing direction
            swimForward = f.forward;
        } else {
            // Manual control: use camera's 3D direction (swim where you look)
            swimForward = glm::normalize(f.forward3D);
            if (glm::dot(swimForward, swimForward) < 1e-8f) {
                swimForward = f.forward;
            }
        }
        // Use character's facing direction for strafe, not camera's right vector
        glm::vec3 swimRight = f.right;  // Character's right (horizontal facing), not camera's

        float swimBackSpeed = useWoWSpeed ? std::min(stockSp.swimBack, swimSpeed)
            : ((swimBackSpeedOverride_ > 0.0f && swimBackSpeedOverride_ < 100.0f
                && !std::isnan(swimBackSpeedOverride_))
                   ? swimBackSpeedOverride_ : swimSpeed * 0.5f);

        glm::vec3 swimMove(0.0f);
        if (f.nowForward) swimMove += swimForward;
        if (f.nowBackward) swimMove -= swimForward;
        if (f.nowStrafeLeft) swimMove += swimRight;
        if (f.nowStrafeRight) swimMove -= swimRight;

        // Spacebar = swim up, X = swim down (both continuous, not a jump)
        bool diveKey = f.xDown;
        bool diveIntent = diveKey || (f.nowForward && (f.forward3D.z < -0.28f));
        // Ascending or descending is a direction of its own, at the swim speed
        // (0x987B50): straight up or down alone (case 0x10/0x12), and with a
        // horizontal direction the two at 45 degrees - the flat direction
        // times 0.7071 and up or down times 0.7071 (0x987700) - so the total
        // is still the swim speed, not the swim speed plus a climb.
        const bool stockVertical = useWoWSpeed && (f.spaceHeld || diveKey);

        float swimMoveLenSq = glm::dot(swimMove, swimMove);
        // Use backward swim speed when moving backwards (with or without a strafe)
        float applySpeed = (f.nowBackward && !f.nowForward) ? swimBackSpeed : swimSpeed;
        if (swimMoveLenSq > 1e-6f) {
            if (stockVertical) {
                glm::vec3 flat(swimMove.x, swimMove.y, 0.0f);
                const float flatLenSq = glm::dot(flat, flat);
                if (flatLenSq > 1e-6f) {
                    flat *= glm::inversesqrt(flatLenSq) * game::stock_movement::kHalfSqrt2;
                    targetPos += flat * applySpeed * f.physicsDeltaTime;
                }
            } else {
                swimMove *= glm::inversesqrt(swimMoveLenSq);
                targetPos += swimMove * applySpeed * f.physicsDeltaTime;
            }
        }

        if (stockVertical) {
            const float climb = (swimMoveLenSq > 1e-6f)
                ? applySpeed * game::stock_movement::kHalfSqrt2 : swimSpeed;
            verticalVelocity = f.spaceHeld ? climb : -climb;
        } else if (f.spaceHeld) {
            verticalVelocity = SWIM_BUOYANCY;
        } else if (diveKey) {
            verticalVelocity = -SWIM_BUOYANCY;
        } else {
            // No vertical key held. Retail holds depth here rather than
            // pulling the swimmer back to the surface: a character can idle
            // underwater and drown doing it, which a surface spring would
            // make impossible. The old behaviour yanked you up unless you
            // were actively diving, so a dive only held while a key was
            // down. Bleed off whatever vertical momentum was carried in and
            // then hover.
            verticalVelocity *= std::max(0.0f, 1.0f - SWIM_VERTICAL_DAMPING * f.physicsDeltaTime);
            if (std::abs(verticalVelocity) < 0.05f) verticalVelocity = 0.0f;

            // Near the surface a body does float up to it, which is what
            // keeps a swimmer's head out of the water instead of drifting
            // just below. Only within a short band, and not while the look
            // direction says the player is diving.
            const float surfaceErr = waterSurfaceZ - targetPos.z;
            if (!diveIntent && surfaceErr > 0.0f && surfaceErr < SWIM_FLOAT_BAND) {
                verticalVelocity += surfaceErr * 6.0f * f.physicsDeltaTime;
                if (surfaceErr < 0.06f && std::abs(verticalVelocity) < 0.35f) {
                    verticalVelocity = 0.0f;
                }
            }
        }

        targetPos.z += verticalVelocity * f.physicsDeltaTime;

        // The surface is a ceiling to the top of a box 0.75 h tall (0x75F9D0),
        // so the feet stop at the float depth.
        if (waterH && targetPos.z > *waterH - swimFloatDepth()) {
            targetPos.z = *waterH - swimFloatDepth();
            if (verticalVelocity > 0.0f) verticalVelocity = 0.0f;
        }

        // Prevent sinking/clipping through world floor while swimming.
        // Cache floor queries (update every 3 frames or 1 unit f.movement)
        std::optional<float> floorH;
        float dx2D = targetPos.x - lastFloorQueryPos.x;
        float dy2D = targetPos.y - lastFloorQueryPos.y;
        float dist2DSq = dx2D * dx2D + dy2D * dy2D;
        constexpr float kFloorDistSq = FLOOR_QUERY_DISTANCE_THRESHOLD * FLOOR_QUERY_DISTANCE_THRESHOLD;
        bool updateFloorCache = (floorQueryFrameCounter++ >= FLOOR_QUERY_FRAME_INTERVAL) ||
                                 (dist2DSq > kFloorDistSq);

        if (updateFloorCache) {
            floorQueryFrameCounter = 0;
            lastFloorQueryPos = targetPos;
            constexpr float MAX_SWIM_FLOOR_ABOVE_FEET = 0.25f;
            constexpr float MIN_SWIM_CEILING_ABOVE_FEET = 0.30f;
            constexpr float MAX_SWIM_CEILING_ABOVE_FEET = 1.80f;
            std::optional<float> ceilingH;
            auto considerFloor = [&](const std::optional<float>& h) {
                if (!h) return;
                // Swim-floor guard: only accept surfaces at or very slightly above feet.
                if (*h <= targetPos.z + MAX_SWIM_FLOOR_ABOVE_FEET) {
                    if (!floorH || *h > *floorH) floorH = h;
                }
                // Swim-ceiling guard: detect structures just above feet so upward swim
                // can't clip through docks/platform undersides.
                float dz = *h - targetPos.z;
                if (dz >= MIN_SWIM_CEILING_ABOVE_FEET && dz <= MAX_SWIM_CEILING_ABOVE_FEET) {
                    if (!ceilingH || *h < *ceilingH) ceilingH = h;
                }
            };

            if (terrainManager) {
                considerFloor(terrainManager->getHeightAt(targetPos.x, targetPos.y));
            }
            if (wmoRenderer) {
                auto wh = wmoRenderer->getFloorHeight(targetPos.x, targetPos.y, targetPos.z + 2.0f);
                considerFloor(wh);
            }
            if (m2Renderer && !externalFollow_) {
                auto mh = m2Renderer->getFloorHeight(targetPos.x, targetPos.y, targetPos.z + 2.0f);
                considerFloor(mh);
            }

            // Terrain is not a surface a swimmer can be underneath.
            //
            // considerFloor only takes a floor at or just above the feet, so a
            // dive can pass under a dock rather than being held on top of it.
            // Terrain has no underside to pass under, and once the feet are
            // below the heightmap its sample is far above them: the guard drops
            // it, no floor is left, and nothing pushes back. One fast frame
            // through the surface and the swimmer is under the map for good.
            //
            // A structure floor found beneath the feet still owns the vertical,
            // so a cave or an interior that genuinely sits below the heightmap
            // keeps swimming as it did.
            if (terrainManager && !floorH) {
                auto th = terrainManager->getHeightAt(targetPos.x, targetPos.y);
                if (th && *th > targetPos.z) floorH = th;
            }

            if (ceilingH && verticalVelocity > 0.0f) {
                float ceilingLimit = *ceilingH - 0.35f;
                if (targetPos.z > ceilingLimit) {
                    targetPos.z = ceilingLimit;
                    verticalVelocity = 0.0f;
                }
            }

            cachedFloorHeight = floorH;
        } else {
            floorH = cachedFloorHeight;
        }
        // The bottom holds the box's feet, as any floor does (stock swims the
        // box through the same triangles it walks on). It held them half a
        // yard up, which a wader felt as 0.5 yd less water than there was:
        // with the stock exit depth that would end the swim in water deep
        // enough to start it again.
        if (floorH) {
            float swimFloor = *floorH;
            if (targetPos.z < swimFloor) {
                targetPos.z = swimFloor;
                if (verticalVelocity < 0.0f) verticalVelocity = 0.0f;
            }
        }

        // Enforce collision while swimming too (horizontal only), skip when stationary.
        {
            // Horizontal only: the swim clamp owns the vertical.
            glm::vec3 stepPos = sweepAgainstWalls(*followTarget, targetPos, true);

            // Terrain is a wall to a swimmer, not only a floor.
            //
            // sweepAgainstWalls knows WMO walls and doodad collision, and a
            // shoreline is neither: it is the heightmap, which the swim path
            // only ever consulted downward, for something to float above. So
            // nothing stopped a swimmer crossing into a hillside - and the
            // floor probe above will not push them out of it either, because
            // it takes a floor only at or just above the feet and a cliff face
            // ahead is far above them. Leaving the water inside the hill then
            // drops them through the world.
            //
            // Only a rise the swimmer could not float over counts. The clamp
            // holds them half a yard above the floor, so a beach shelving up
            // under them stays passable and they swim ashore as before.
            constexpr float SWIM_TERRAIN_WALL_CLEARANCE = 0.9f;
            if (terrainManager) {
                const SwimStep allowed = swimStepAgainstTerrain(
                    {followTarget->x, followTarget->y}, {stepPos.x, stepPos.y},
                    targetPos.z + SWIM_TERRAIN_WALL_CLEARANCE,
                    [&](float x, float y) { return terrainManager->getHeightAt(x, y); });
                stepPos.x = allowed.x;
                stepPos.y = allowed.y;
            }

            targetPos.x = stepPos.x;
            targetPos.y = stepPos.y;
        }

        grounded = false;
        } else {
        // Out of the water by wading or a ledge: on the ground, with the full
        // step. Stock has no lift here - the step height (1 yd) is what gets a
        // wader onto a stair lip, and the half-yard boost this used to give
        // only added a hop. A surface hop is a jump and keeps its arc.
        if (wasSwimming && !surfaceHopThisFrame_) {
            // Anchor lastGroundZ to current position so WMO floor probes
            // start from a sensible height instead of stale pre-swim values.
            lastGroundZ = targetPos.z;
            grounded = true;  // Treat as grounded so step-up budget is full
            if (verticalVelocity > 0.0f) verticalVelocity = 0.0f;
        }
        swimming = false;

        // Taking off: space, on something allowed to fly. It climbs from the
        // first frame rather than jumping first, which is what the key means
        // once a flying mount is under the player.
        if (flyingActive_ && !flightAirborne_ && f.spaceHeld) {
            flightAirborne_ = true;
            LOG_WARNING("Flight: took off");
        }

        // Player-controlled flight (flying mount / druid Flight Form):
        // Use 3D pitch-following movement with no gravity or grounding.
        if (flyingActive_ && flightAirborne_) {
            grounded = true;  // suppress fall-damage checks
            verticalVelocity = 0.0f;
            jumpBufferTimer = 0.0f;
            coyoteTimer = 0.0f;

            // Forward/back along the mount's facing at the flight pitch - see
            // flightForward. The camera's own look direction is not it.
            glm::vec3 flyFwd = f.flightForward;
            if (glm::dot(flyFwd, flyFwd) < 1e-8f) flyFwd = f.forward;
            const glm::vec3 flightFrom = targetPos;
            glm::vec3 flyMove(0.0f);
            if (f.nowForward)     flyMove += flyFwd;
            if (f.nowBackward)    flyMove -= flyFwd;
            if (f.nowStrafeLeft)  flyMove += f.right;
            if (f.nowStrafeRight) flyMove -= f.right;
            // Space = ascend, X = descend while airborne (works on foot too,
            // e.g. .gm fly, not just on a flying mount).
            bool flyDescend = !f.uiWantsKeyboard && f.xDown;
            if (f.spaceHeld)     flyMove.z += 1.0f;
            if (flyDescend)      flyMove.z -= 1.0f;
            float flyMoveLenSq = glm::dot(flyMove, flyMove);
            if (flyMoveLenSq > 1e-6f) {
                flyMove *= glm::inversesqrt(flyMoveLenSq);
                // The flight pair (0x987570); walking does not slow a flier,
                // and the defaults are 7.0 / 4.5.
                const game::stock_movement::Speeds flySp = stockSpeeds();
                float flyFwdSpeed = useWoWSpeed ? flySp.flight
                    : ((flightSpeedOverride_ > 0.0f && flightSpeedOverride_ < 200.0f
                        && !std::isnan(flightSpeedOverride_))
                           ? flightSpeedOverride_ : f.speed);
                float flyBackSpeed = useWoWSpeed ? std::min(flySp.flightBack, flyFwdSpeed)
                    : ((flightBackSpeedOverride_ > 0.0f && flightBackSpeedOverride_ < 200.0f
                        && !std::isnan(flightBackSpeedOverride_))
                           ? flightBackSpeedOverride_ : flyFwdSpeed * 0.5f);
                float flySpeed = (f.nowBackward && !f.nowForward) ? flyBackSpeed : flyFwdSpeed;
                targetPos += flyMove * flySpeed * f.physicsDeltaTime;
            }
            targetPos.z += verticalVelocity * f.physicsDeltaTime;

            // The ground holds a flying mount up - see flightStepAgainstGround.
            // Not where a WMO floor lies at the feet under the heightfield: that
            // is a tunnel burrowing under unholed ground, flown into, not over.
            if (terrainManager) {
                // Half a heightfield cell, as the slope reading in
                // sampleFloorUnderFeet uses, for the same reason.
                constexpr float kGroundNormalSpacing = 2.0f;
                const glm::vec3 slid = movement::flightStepAgainstGround(
                    [this](float x, float y) -> std::optional<float> {
                        if (terrainManager->isHoleAt(x, y)) return std::nullopt;
                        return terrainManager->getHeightAt(x, y);
                    },
                    flightFrom, targetPos, kGroundNormalSpacing);
                if (slid != targetPos &&
                    !(wmoRenderer && wmoRenderer->getFloorHeight(
                          targetPos.x, targetPos.y, targetPos.z + 1.5f))) {
                    targetPos = slid;
                }
            }
            // Skip all ground physics - go straight to collision/WMO sections
        } else {

        float moveLenSq = glm::dot(f.movement, f.movement);
        if (moveLenSq > 1e-6f) {
            f.movement *= glm::inversesqrt(moveLenSq);
            targetPos += f.movement * f.speed * f.physicsDeltaTime;
        }

        // Apply server-driven knockback horizontal velocity (decays over time).
        if (knockbackActive_) {
            targetPos.x += knockbackHorizVel_.x * f.physicsDeltaTime;
            targetPos.y += knockbackHorizVel_.y * f.physicsDeltaTime;
            // Exponential drag: reduce each frame so the player decelerates naturally.
            float drag = std::exp(-KNOCKBACK_HORIZ_DRAG * f.physicsDeltaTime);
            knockbackHorizVel_ *= drag;
            // Once negligible, clear the flag so collision/grounding work normally.
            if (glm::dot(knockbackHorizVel_, knockbackHorizVel_) < 0.0025f) {
                knockbackActive_ = false;
                knockbackHorizVel_ = glm::vec2(0.0f);
            }
        }

        // Sliding down ground too steep to stand on (see groundFollowedCharacter).
        if (slopeSlideVel_.x != 0.0f || slopeSlideVel_.y != 0.0f) {
            targetPos.x += slopeSlideVel_.x * f.physicsDeltaTime;
            targetPos.y += slopeSlideVel_.y * f.physicsDeltaTime;
        }

        // Jump with input buffering and coyote time
        if (f.nowJump) jumpBufferTimer = JUMP_BUFFER_TIME;
        if (grounded) coyoteTimer = COYOTE_TIME;

        // On the ground, or (with a grace time set) just off it. Not from
        // ground too steep to stand on: the stock client is falling there, and
        // hopping up a cliff a jump at a time is what that stops.
        // Not while rooted either: the stock jump refuses ROOT, FALLING and
        // FLYING (0x9883F0 tests 0x2001800).
        bool canJump = (grounded || coyoteTimer > 0.0f) && (jumpBufferTimer > 0.0f) &&
                       !mounted_ && !onSteepSlope_ && !movementRooted_;
        if (canJump) {
            verticalVelocity = f.jumpVel;
            grounded = false;
            jumpBufferTimer = 0.0f;
            coyoteTimer = 0.0f;
        }

        jumpBufferTimer -= f.physicsDeltaTime;
        coyoteTimer -= f.physicsDeltaTime;

        // Apply gravity (skip when server has disabled f.gravity, e.g. Levitate spell)
        //
        // ...and skip it while the ground has not arrived. An unloaded tile and
        // a hole both answer nothing when asked for a height, and only one of
        // them should be fallen through. Entering the world is faster than
        // streaming the tile under it on a slow device, so without this the
        // character falls from the spawn point until the server kills it.
        float fallDzThisFrame = 0.0f;
        bool fallDzSet = false;
        if (seatedInChair_) {
            // In a chair, and the chair's height is the server's to decide.
            //
            // Gravity ran anyway: the character fell off the seat toward the
            // floor, the server put them back, and they fell again. In the
            // Undercity barber shop that is a bob of about a yard - the seat
            // is at -42.07, the floor under it at -43.02, and the server holds
            // the character at -42.73 between them. Reported as the view
            // bobbing up and down in the chair; it is the character bobbing,
            // and the camera only following.
            //
            // Grounding already stands down for a seat. Falling had to as
            // well, or standing the one down just leaves the other to do it.
            verticalVelocity = 0.0f;
        } else if (groundNotStreamedYet(targetPos.x, targetPos.y)) {
            verticalVelocity = 0.0f;
        } else if (gravityDisabled_) {
            // Float in place: bleed off any downward velocity, allow upward to decay slowly
            if (verticalVelocity < 0.0f) verticalVelocity = 0.0f;
            else verticalVelocity *= std::max(0.0f, 1.0f - 3.0f * f.physicsDeltaTime);
        } else if (useWoWSpeed && f.gravity != 0.0f) {
            // The stock fall (0x986F00): constant gravity in closed form, so
            // the arc is the same at any frame rate, up to the fall speed cap
            // - 60.148 yd/s, or 7.0 under Feather Fall / Slow Fall (0x986E10).
            fallDzThisFrame = game::stock_movement::fallStep(
                verticalVelocity, f.physicsDeltaTime, featherFallActive_);
            fallDzSet = true;
        } else {
            verticalVelocity += f.gravity * f.physicsDeltaTime;
            // Feather Fall / Slow Fall: the stock cap is 7 yd/s (0xB2D9EC).
            if (featherFallActive_ &&
                verticalVelocity < -game::stock_movement::kSafeFallTerminalVelocity) {
                verticalVelocity = -game::stock_movement::kSafeFallTerminalVelocity;
            }
        }
        targetPos.z += fallDzSet ? fallDzThisFrame : verticalVelocity * f.physicsDeltaTime;
        } // end ground physics (not airborne in flight)
        } // end !inWater
    } else {
        // External follow (e.g., taxi): trust server position without grounding.
        swimming = false;
        grounded = true;
        verticalVelocity = 0.0f;
    }

    // Refresh inside-WMO state before collision/grounding so we don't use stale
    // terrain-first caches while entering enclosed tunnel/building spaces.
    if (wmoRenderer && !externalFollow_) {
        glm::vec3 insideDelta = targetPos - lastInsideStateCheckPos_;
        float insideDistSq = glm::dot(insideDelta, insideDelta);
        if (++insideStateCheckCounter_ >= 2 || insideDistSq > 0.1225f) {
            insideStateCheckCounter_ = 0;
            lastInsideStateCheckPos_ = targetPos;

            bool prevInside = cachedInsideWMO;
            bool prevInsideInterior = cachedInsideInteriorWMO;
            cachedInsideWMO = wmoRenderer->isInsideWMO(targetPos.x, targetPos.y, targetPos.z + 1.0f, nullptr);
            cachedInsideInteriorWMO = cachedInsideWMO &&
                wmoRenderer->isInsideInteriorWMO(targetPos.x, targetPos.y, targetPos.z + 1.0f);
            if (cachedInsideWMO != prevInside || cachedInsideInteriorWMO != prevInsideInterior) {
                hasCachedFloor_ = false;
                hasCachedCamFloor = false;
                cachedPivotLift_ = 0.0f;
            }
        }
    }

    // Sweep collisions in small steps to reduce tunneling through thin walls/floors.
    // Skip entirely when stationary to avoid wasting collision calls.
    // Use tighter steps when inside WMO for more precise collision.
    targetPos = sweepAgainstWalls(*followTarget, targetPos, true);

    return targetPos;
}

// Put the character on the floor, and publish where it ended up.
//
// The expensive half: terrain, WMO and doodad floor queries, the step-up
// budget, the slope limit, the cache that skips all of it when barely moving.
// Then the follow target is written, and a fall through the world is caught.
// What the floor queries answer directly under the character.
//
// The expensive part of grounding: three renderers asked where their surface
// is, two of them on other threads, and a cache that skips all of it when the
// character has barely moved. The three centre samples come back with the
// chosen floor because the recovery probes above need to know which surfaces
// existed at all, not just which one won.
CameraController::FloorSample CameraController::sampleFloorUnderFeet(const glm::vec3& targetPos,
                                                                     float stepUpBudget) {
    std::optional<float> groundH;
    std::optional<float> centerTerrainH;
    std::optional<float> centerWmoH;
    std::optional<float> centerM2H;
    // Collision cache: skip expensive checks if barely moved (15cm threshold)
    float dmx = targetPos.x - lastCollisionCheckPos_.x;
    float dmy = targetPos.y - lastCollisionCheckPos_.y;
    float distMovedSq = dmx * dmx + dmy * dmy;
    constexpr float kCollisionCacheDistSq = COLLISION_CACHE_DISTANCE * COLLISION_CACHE_DISTANCE;
    bool useCached = grounded && hasCachedFloor_ && distMovedSq < kCollisionCacheDistSq;
    if (useCached) {
        // Never trust cached ground while actively descending or when
        // vertical drift from cached floor is meaningful.
        float dzCached = std::abs(targetPos.z - cachedFloorHeight_);
        if (verticalVelocity < -0.4f || dzCached > 0.35f) {
            useCached = false;
        }
    }

    if (useCached) {
        groundH = cachedFloorHeight_;
    } else {
        // Full collision check - run terrain/WMO/M2 queries in parallel
        std::optional<float> terrainH;
        std::optional<float> wmoH;
        std::optional<float> m2H;
        // Airborne, the probe starts at the feet and reaches only as far as
        // the caller's budget, which is the span the feet crossed this frame
        // plus a little slack (see groundFollowedCharacter). It used to sit at
        // the last ground plus the budget plus half a yard, which offered
        // ledge tops above the feet as floors and lifted the player onto them
        // mid-jump. Nothing above the probe can be chosen, so roof geometry
        // overhead stays out of it just as it did.
        float wmoBaseZ = grounded ? std::max(targetPos.z, lastGroundZ) : targetPos.z;
        float wmoProbeZ = wmoBaseZ + stepUpBudget + (grounded ? 0.5f : 0.05f);
        float wmoNormalZ = 1.0f;

        // Launch WMO + M2 floor queries asynchronously while terrain runs on this thread.
        // Collision scratch buffers are thread_local so concurrent calls are safe.
        using FloorResult = std::pair<std::optional<float>, float>;
        std::future<FloorResult> wmoFuture;
        std::future<FloorResult> m2Future;
        bool wmoAsync = false, m2Async = false;
        float px = targetPos.x, py = targetPos.y;
        if (wmoRenderer) {
            wmoAsync = true;
            // Closest to the feet, not the highest below the probe.
            //
            // Under an overhang - the doorways and portals of
            // Undercity - the floor of the level above sits about a
            // metre over the one you stand on, both inside the
            // step-up window. The default highest-floor pick snapped
            // the player up to the level above even standing still,
            // the server pulled them back to the real floor, and the
            // two fought once a second. Anchoring the pick to the
            // player's own ground keeps them where they are; a real
            // step onto a ledge still wins once the feet are level
            // with it.
            //
            // The reference is the feet themselves, NOT wmoBaseZ:
            // wmoBaseZ is max(z, lastGroundZ) so the probe can reach
            // up for a step, but feeding that same raised value back
            // as the arbitration anchor ratchets the pick upward -
            // one stray upper-floor frame lifts lastGroundZ, and the
            // reference then stays on the level above even after the
            // server drags the player back down, which is the very
            // yo-yo this pick was meant to stop. Grounded, the true
            // feet are targetPos.z; airborne, aim at the last ground.
            //
            // Except that a player cannot be standing on a floor while
            // their feet are a long way under it, and the flag says
            // grounded anyway. At the Undercity elevator ramp the feet
            // were 2.81 below the landing they had been standing on -
            // still a candidate, at -41.4273 against a last ground of
            // -41.4245 - and 2.39 above the Trade Quarter floor five
            // yards further down. Measured from the feet the lower one
            // is nearer by 0.42, so the pick took it and the player
            // dropped through the ramp.
            //
            // Below the last ground by more than a step is falling,
            // whatever the flag says, so anchor where the airborne case
            // already anchors: the ground they were last on. That floor
            // then wins by 5.2 instead of losing by 0.42. It cannot pull
            // anyone up onto a floor they truly left, because a floor
            // they have walked off is not a candidate at their feet.
            //
            // Airborne the probe only spans what the feet crossed this frame,
            // so the floor wanted is the first one the feet met: the highest
            // under the probe, which is what anchoring to the probe picks.
            const float feetRef = grounded
                ? movement::floorArbitrationAnchor(grounded, targetPos.z, lastGroundZ)
                : wmoProbeZ;
            wmoFuture = core::ThreadPool::frameWorkers().submit(
                [this, px, py, wmoProbeZ, feetRef]() -> FloorResult {
                    float nz = 1.0f;
                    auto h = wmoRenderer->getFloorHeight(px, py, wmoProbeZ, &nz, feetRef);
                    return {h, nz};
                });
        }
        if (m2Renderer && !externalFollow_) {
            m2Async = true;
            m2Future = core::ThreadPool::frameWorkers().submit(
                [this, px, py, wmoProbeZ]() -> FloorResult {
                    float nz = 1.0f;
                    auto h = m2Renderer->getFloorHeight(px, py, wmoProbeZ, &nz);
                    return {h, nz};
                });
        }
        if (terrainManager) {
            terrainH = terrainManager->getHeightAt(targetPos.x, targetPos.y);
            // The slope limit does NOT remove the ground here.
            //
            // It used to: too steep, and terrainH was cleared. But a floor
            // that is not there is not a wall - the player does not fail to
            // climb the hill, they fall through it and keep going. The limit
            // lives on the movement instead (groundFollowedCharacter): the
            // uphill part of a step onto steep ground is refused and slid
            // along the contour, and a character left standing on steep
            // ground slides down it.
        }
        if (wmoAsync) {
            try { auto [h, nz] = wmoFuture.get(); wmoH = h; wmoNormalZ = nz; }
            catch (const std::exception& e) { LOG_ERROR("WMO floor query: ", e.what()); }
        }
        if (m2Async) {
            try {
                auto [h, nz] = m2Future.get();
                m2H = h;
                if (m2H && !ignoreSlopeLimit_ && nz < MIN_WALKABLE_NORMAL_M2) {
                    m2H = std::nullopt;
                }
            } catch (const std::exception& e) { LOG_ERROR("M2 floor query: ", e.what()); }
        }

        // A tunnel mouth can overlap the outdoor heightfield before the
        // player's eye point is inside the WMO bounds.  Treat a nearby WMO
        // floor below that terrain as transition space immediately; waiting
        // for cachedInsideWMO makes the outdoor terrain win one frame at a
        // time (climbing through the roof), while rejecting a steep entrance
        // ramp here makes the player fall through it.
        bool atTunnelSeam = false;
        if (terrainH && wmoH) {
            const float terrainAboveWmo = *terrainH - *wmoH;
            const float wmoDropFromPlayer = targetPos.z - *wmoH;
            atTunnelSeam = terrainAboveWmo > 1.2f && terrainAboveWmo < 12.0f &&
                           wmoDropFromPlayer >= -0.4f && wmoDropFromPlayer < 1.8f;
        }
        // A real tunnel mouth burrows into rising ground: the heightfield
        // just ahead climbs above head height (or stops in a hole cut for
        // the passage). WMO ramps that merely run beneath flat walkable
        // streets must not steal the player from the terrain above them -
        // that pulled players through the ground at the Stormwind gate
        // ramparts and down ramps into the void. Inside an interior WMO
        // group (tram entrance buildings) the heightfield under the city
        // is meaningless, so it gets no veto - otherwise entry becomes
        // dependent on approach angle.
        if (atTunnelSeam && !cachedInsideInteriorWMO && terrainManager) {
            glm::vec3 moveDir = targetPos - lastCollisionCheckPos_;
            moveDir.z = 0.0f;
            const float moveLen = glm::length(moveDir);
            if (moveLen < 1e-3f) {
                atTunnelSeam = false;  // stationary - nothing to enter
            } else {
                const glm::vec3 aheadPos = targetPos + moveDir * (2.5f / moveLen);
                auto terrainAhead = terrainManager->getHeightAt(aheadPos.x, aheadPos.y);
                atTunnelSeam = !terrainAhead ||
                               *terrainAhead > targetPos.z + 2.2f;
            }
        }

        // Reject steep WMO slopes. Tunnel ramps use the more permissive WMO
        // limit even at the boundary, where isInsideWMO is not reliable yet.
        float minWalkableWmo = (cachedInsideWMO || atTunnelSeam)
            ? MIN_WALKABLE_NORMAL_WMO : MIN_WALKABLE_NORMAL_TERRAIN;
        if (wmoH && !ignoreSlopeLimit_ && wmoNormalZ < minWalkableWmo) {
            wmoH = std::nullopt;  // Treat as unwalkable
        }

        // Reject WMO floors above what the feet crossed when airborne
        // (prevents snapping to roof/ceiling surfaces during jumps). Measured
        // from the feet, not the last ground: a ledge top a jump clears sits
        // above the ground it was jumped from and must still be landed on.
        if (wmoH && !grounded && *wmoH > targetPos.z + stepUpBudget + 0.05f) {
            wmoH = std::nullopt;
            centerWmoH = std::nullopt;
        }
        // An M2 doodad's collision sitting well below a valid WMO
        // floor is BENEATH that floor - a decoration or structural
        // base under the walkway, not a surface to stand on. Letting
        // it win drops the player through the WMO floor: in Undercity
        // the pick took an m2 floor ~6m below the wmo one (feet -48.8,
        // wmo -48.15, m2 -54.23) and fell. When a WMO floor is present
        // here, reject an m2 floor more than 1.5m below it. (If the
        // player were standing ON the m2 platform, the higher WMO
        // floor would be above the probe and not returned, so this
        // cannot strand them off a legitimate lower deck.)
        if (m2H && wmoH && *m2H < *wmoH - 1.5f) {
            m2H = std::nullopt;
        }
        // A terrain hole is the artist saying there is no ground
        // here - it is how cave mouths and sunken entrances get
        // opened up, and the mesh builder already skips those
        // quads, so nothing is drawn over them. getHeightAt does
        // not ask: it interpolates straight across the gap and
        // reports a surface right at the player's feet, which then
        // wins the floor selection against the real floor below.
        // At Gadgetzan's auction house that put the player out over
        // the stairwell on ground that is not there, unable to walk
        // down the steps and looking into a hole they were standing
        // on. Every seam guard below is downstream of this and none
        // of them could see it, because to them the terrain sample
        // was real.
        //
        // Only ever hand the floor to the building underneath - a
        // hole with nothing below it keeps the heightfield it has
        // always had, so this cannot open a pit anywhere new.
        if (terrainH && wmoH && terrainManager &&
            terrainManager->isHoleAt(targetPos.x, targetPos.y)) {
            terrainH = std::nullopt;
        }

        // Inside an interior WMO group - Undercity's halls, a
        // building's rooms - the outdoor heightfield is the roof far
        // overhead, never the floor. Veto it there so a brief gap in
        // the WMO floor query does not kick the player up to the
        // surface. (The seam case is handled above and keeps
        // cachedInsideInteriorWMO false at entrances.)
        //
        // Only when it really is overhead, though. isInsideInteriorWMO
        // is a bounding-box containment test, and an underground WMO's
        // interior box reaches up through the ground above it - so
        // standing on the grass over a cave counted as being inside
        // it. Vetoing terrain there left the WMO as the only
        // candidate, and the nearest WMO surface below is the cave's
        // ceiling: the player dropped through the hillside they were
        // walking on and stood on the roof of the room underneath.
        //
        // Higher than the player could step onto is the test, because
        // that is the whole of what the veto meant: ground you cannot
        // reach is not the ground you are standing on. Undercity's
        // surface sits ~113m over the halls and is still vetoed; the
        // hillside at your feet is not. The rule itself lives in
        // movement_limits.hpp, where it is pinned by a test.
        if (terrainH && movement::terrainIsOverheadRoof(
                cachedInsideInteriorWMO, *terrainH,
                targetPos.z, stepUpBudget)) {
            terrainH = std::nullopt;
        }

        centerTerrainH = terrainH;
        centerWmoH = wmoH;
        centerM2H = m2H;

        // Guard against extremely bad WMO void ramps, but keep normal tunnel
        // transitions valid. Only reject when the WMO sample is implausibly far
        // below terrain and player is not already descending.
        if (terrainH && wmoH && !cachedInsideWMO) {
            float terrainMinusWmo = *terrainH - *wmoH;
            if (terrainMinusWmo > 12.0f && verticalVelocity > -8.0f) {
                wmoH = std::nullopt;
                centerWmoH = std::nullopt;
            }
        }

        if ((cachedInsideWMO || atTunnelSeam) && wmoH) {
            // The terrain wins at a seam unless it is not really there.
            //
            // Two ways it can fail to be. The quad is cut by a hole, which is
            // how the map opens a cave mouth or a below-ground entrance - that
            // is vetoed further up, before any of this, so there is simply no
            // terrain candidate left here. Or it is standing overhead out of
            // reach, which is what the heightfield is once the player is
            // inside a passage burrowing under unholed ground: taking it would
            // lift them back out. See wmoFloorIsWayIn.
            //
            // Anything else is real ground under real feet, and a WMO floor
            // that happens to run beneath it is not a way in. Orgrimmar's
            // valley entrance is the case: its ramp passes about 1.3 yards
            // under the ground beside it, and there is no hole anywhere in
            // that tile.
            const bool preferWmoAtSeam =
                atTunnelSeam &&
                (!terrainH ||
                 movement::wmoFloorIsWayIn(*terrainH, targetPos.z, stepUpBudget));
            if (preferWmoAtSeam) {
                groundH = wmoH;
            } else if (terrainH) {
                // Both real and both reachable: whichever is actually under the
                // feet, rather than oscillating between the ground above and a
                // deep WMO floor below.
                groundH = selectClosestFloor(terrainH, wmoH, targetPos.z);
            } else {
                groundH = selectReachableFloor3(terrainH, wmoH, m2H, targetPos.z, stepUpBudget);
            }
        } else {
            groundH = selectReachableFloor3(terrainH, wmoH, m2H, targetPos.z, stepUpBudget);
        }

        // Nothing to stand on at all, with a building overhead.
        //
        // The jump dump below only fires when a floor is chosen and it is a
        // long way from the last one. Stepping onto an Undercity lift produces
        // no floor whatsoever - the deck is an M2 transport and the shaft has
        // no WMO floor under it - so the player simply falls, and the one dump
        // that would name the deck never ran. UndeadElevator ships 68
        // collision triangles and is not marked skipCollision, so what is
        // wanted here is which test the instance fails, or whether it is a
        // candidate at all.
        if (!groundH && m2Renderer) {
            static std::chrono::steady_clock::time_point lastNoFloorDump{};
            const auto nowNoFloor = std::chrono::steady_clock::now();
            if (nowNoFloor - lastNoFloorDump > std::chrono::seconds(5)) {
                lastNoFloorDump = nowNoFloor;
                // The centre pick's answer, not the final one: the recovery
                // passes in groundFollowedCharacter run after this and often
                // find a floor the one ray missed. Said plainly because the
                // first wording read as "the player is over a void" and is
                // what sent this hunt after the wrong thing.
                LOG_WARNING("Centre floor probe found nothing (feet ", targetPos.z,
                            ") - the recovery passes have not run yet");
                if (floorDiagEnabled()) {
                    if (wmoRenderer) {
                        wmoRenderer->debugDumpGroupsAtPosition(
                            targetPos.x, targetPos.y, targetPos.z);
                    }
                    m2Renderer->debugDumpFloorCandidatesAt(
                        targetPos.x, targetPos.y, targetPos.z);
                }
            }
        }

        // Just arrived, and the buildings have not.
        //
        // On entering a world the heightfield is there before the WMOs are, and
        // over anything that runs underground it lies well above where the
        // player actually is. Logging out in the Undercity entrance tunnel and
        // back in put them on its roof, and once the tunnel loaded its roof was
        // a perfectly good floor to keep them there. Nothing far above the
        // position the server gave is their floor while that is still true.
        //
        // Generous, because the server's Z and the floor under it disagree by a
        // little at the best of times; two yards admits that and refuses forty.
        if (entryFloorAnchorTimer_ > 0.0f && groundH &&
            *groundH > entryFloorAnchorZ_ + 2.0f) {
            groundH = std::nullopt;
        }

        // A storey down, with the floor still under the next step.
        //
        // One ray at the character's centre misses the lip of a surface, and
        // the answer it comes back with is not "no floor" but whatever lies at
        // the bottom of the room - at the top of the Undercity lift that is the
        // shaft floor, ninety-six yards down. groundFollowedCharacter samples a
        // foot cross afterwards and recovers, which is why this reads as almost
        // falling rather than falling, but the frame in between is a dip the
        // player sees.
        //
        // So before handing back a drop of more than a few yards, ask the four
        // points around the feet. Only on that frame, so the hot path still
        // casts one ray. A drop nothing around the feet can account for is a
        // real one and is passed through untouched - walking off a ledge still
        // works.
        if (groundH && grounded && wmoRenderer &&
            *groundH < lastGroundZ - 4.0f && targetPos.z > lastGroundZ - 4.0f) {
            const float lipProbeZ = std::max(targetPos.z, lastGroundZ) + stepUpBudget + 0.6f;
            auto lip = highestWalkableFloor(
                [this](float x, float y, float z, float* nz) {
                    return wmoRenderer->getFloorHeight(x, y, z, nz);
                },
                targetPos.x, targetPos.y, feetCross(0.35f), lipProbeZ,
                MIN_WALKABLE_NORMAL_WMO,
                {.minZ = lastGroundZ - 3.5f, .maxZ = targetPos.z + stepUpBudget});
            if (lip) groundH = lip;
        }

        // The local player's own floor pick, named when it jumps.
        //
        // The movingEntityFloor log covers creatures and other
        // players; this is the reporter's own character in
        // Undercity. It says which floor was chosen for the player
        // and the three candidates it chose among, so a jump that
        // survived the closest-to-feet change shows what is still
        // competing - and whether both floors are even offered here
        // or only one is, which decides whether the fix belongs in
        // the selection or deeper in the WMO query.
        // Threshold sits below a floor gap (~1m) on purpose: the
        // ratchet oscillation the feetRef fix targets stays under a
        // metre frame to frame, so a 1.0 gate saw nothing while the
        // player visibly bobbed. 0.4 catches the sub-metre bob.
        if (groundH && std::abs(*groundH - lastGroundZ) > 0.4f) {
            static std::chrono::steady_clock::time_point lastPlayerFloorLog{};
            const auto now = std::chrono::steady_clock::now();
            if (now - lastPlayerFloorLog > std::chrono::seconds(1)) {
                lastPlayerFloorLog = now;
                core::Logger::getInstance().warning(
                    "Player floor jump: feet=", targetPos.z,
                    " lastGround=", lastGroundZ, " -> chose ", *groundH,
                    " grounded=", grounded ? 1 : 0,
                    " (terrain=", terrainH ? *terrainH : -99999.0f,
                    " wmo=", wmoH ? *wmoH : -99999.0f,
                    " m2=", m2H ? *m2H : -99999.0f,
                    " seam=", atTunnelSeam ? 1 : 0, ")");
            }
            // A jump - the floor query landed a level away, not a
            // step - is the Undercity pull, and the one thing needed
            // to solve it is which WMO groups and floors exist under
            // the player when it happens. That is exactly what F8's
            // dump prints, so dump it here automatically on a jump
            // (rate-limited hard, it is verbose): the log then carries
            // the answer from ordinary play, with no key to remember.
            // Threshold matches the line above it, not a metre.
            //
            // 0.9 was chosen for a pull that crossed an overhang, and it
            // never fired for the one actually reported in Undercity: the
            // sink there is 0.41, 0.60, 0.56, 0.40 - the thickness of the
            // floor slab, the player dropping onto the ceiling of the room
            // beneath - and every one of those passed under the gate with
            // the jump line printed and no group data behind it. Whatever
            // is worth naming is worth dumping; the five second limiter is
            // what keeps it cheap, not the size of the drop.
            if (wmoRenderer && std::abs(*groundH - lastGroundZ) > 0.35f) {
                static std::chrono::steady_clock::time_point lastFloorDump{};
                if (floorDiagEnabled() && now - lastFloorDump > std::chrono::seconds(5)) {
                    lastFloorDump = now;
                    core::Logger::getInstance().warning(
                        "Player floor jump: dumping WMO groups at the "
                        "pull (feet ", targetPos.z, " -> ", *groundH, ")");
                    wmoRenderer->debugDumpGroupsAtPosition(
                        targetPos.x, targetPos.y, targetPos.z);
                    // And the doodads, which is where the Undercity pull
                    // actually lives: every one of these jumps happens while
                    // the player is attached to a lift, and m2 comes back
                    // empty each time even though UndeadElevator ships 68
                    // collision triangles. The WMO dump alone could never say
                    // why, because the deck it is looking for is not a WMO.
                    if (m2Renderer) {
                        m2Renderer->debugDumpFloorCandidatesAt(
                            targetPos.x, targetPos.y, targetPos.z);
                    }
                }
            }
        }

        // Update cache
        lastCollisionCheckPos_ = targetPos;
        if (groundH) {
            cachedFloorHeight_ = *groundH;
            hasCachedFloor_ = true;
            // Ground found - cancel gravity suspension (WMO floor loaded)
            if (gravitySuspendTimer_ > 0.0f) gravitySuspendTimer_ = 0.0f;
        } else {
            hasCachedFloor_ = false;
        }
    }
    return {.floor = groundH, .terrain = centerTerrainH, .wmo = centerWmoH, .m2 = centerM2H};
}

void CameraController::groundFollowedCharacter(float deltaTime, FrameInput& f,
                                               glm::vec3& targetPos,
                                               const glm::vec3& prevTargetPos) {
    // Seated: the server placed the character, and this pass would place them
    // again from whatever is underneath.
    //
    // A chair is an M2 with collision whose seat is about a metre off the
    // floor and nothing here can tell a seat from a step, so the feet went on
    // top of the chair and the character sat in the air above it. Dropping the
    // chair from the floor candidates was not enough: it only applied to the
    // stand states that name a chair, and a server that answers a chair with a
    // plain SIT - which is what the inn's chairs did - left the whole rule
    // switched off while the pose said otherwise.
    //
    // Nothing drifts while this stands down: movement is blocked for as long
    // as the character is seated, and the moment they stand up the pass runs
    // again from wherever the server left them.
    //
    // One thing is still enforced: the floor. Standing the pass down entirely
    // left nothing to catch a seat that ends up under the ground - a tile that
    // streamed in after the sit, a server seating the character at a Z the
    // floor here does not agree with - and a character below the floor is a
    // character who falls through it when they stand. So the floor under the
    // feet is still sampled and used one way only, upward, and never from the
    // chair's own M2, which is what standing on the furniture was.
    if (sitting) {
        // In a chair the height is the server's alone: it placed the character
        // at the chair's own Z and the sit animation lifts the hips onto the
        // seat. The upward clamp below lifted them onto any WMO-built bench or
        // seat within a step of that Z, which is the same floating it exists
        // to prevent. It stays for sitting on the ground.
        if (seatedInChair_) return;
        const FloorSample seated = sampleFloorUnderFeet(targetPos, movement::kMaxStepUp);
        const std::optional<float> under = selectReachableFloor3(
            seated.terrain, seated.wmo, std::nullopt, targetPos.z, movement::kMaxStepUp);
        if (under && targetPos.z < *under) {
            targetPos.z = *under;
            lastGroundZ = *under;
            hasRealGround_ = true;
            grounded = true;
            verticalVelocity = 0.0f;
        }
        return;
    }

    // Ground the character to terrain or WMO floor
    // Skip entirely while swimming - the swim floor clamp handles vertical bounds.
    if (!swimming) {
        // Airborne the player is a body on a ballistic arc. A surface is met
        // where the arc crosses it, never by lifting the body up onto it.
        //
        // This used to reach 1.2 yards above the feet while airborne, and any
        // floor found in that window snapped the player straight up onto it
        // and grounded them - halfway up a jump at a crate, a rock or a ledge
        // the character popped onto the top. The wall tests leave exactly
        // those ledges to this pass (they ignore a wall whose top is within
        // 1.2 of rising feet), so the pop was the only thing that ever
        // resolved them. Now a ledge top above the feet is a ledge side: the
        // horizontal step into it is refused (or slid along one axis) until
        // the arc clears it, and the landing comes on the way down, where the
        // feet cross the top.
        //
        // (First, on the ground: the slope limit.)
        //
        // Stock refuses ground steeper than 50 degrees (normal z below
        // cos 50 = 0.643, kMinWalkableNormalZ). On the ground, a step whose
        // destination is higher than the feet and on such a surface - terrain,
        // WMO or M2 alike - loses its uphill part and runs along the slope's
        // contour instead. Down and across are never touched, and a step onto
        // a walkable tread (a stair, a ledge within step height) is the step-up
        // it always was: only the surface the step would stand on is judged.
        if (grounded && !ignoreSlopeLimit_ && !hoverActive_ && !externalFollow_ &&
            !isFlightAirborne() && !sitting) {
            const glm::vec2 from(prevTargetPos.x, prevTargetPos.y);
            const glm::vec2 move(targetPos.x - from.x, targetPos.y - from.y);
            const float moveLen = glm::length(move);
            if (moveLen > 1e-4f && moveLen < 8.0f) {
                const float feetZ = prevTargetPos.z;
                // As far up as the step reaches: everything the floor pick
                // could stand the character on is judged for slope.
                const float reach = feetZ + collider_.stepHeight + 0.05f;
                // Uphill direction of the too-steep surface a step to p would
                // climb onto, or nothing if that step is allowed.
                const auto steepClimbAt = [&](const glm::vec2& p) -> std::optional<glm::vec2> {
                    std::optional<float> best;
                    float bestNz = 1.0f;
                    int source = 0;  // 1 terrain, 2 WMO, 3 M2
                    bool walkableStructure = false;
                    glm::vec3 terrainN(0.0f, 0.0f, 1.0f);
                    if (wmoRenderer) {
                        float nz = 1.0f;
                        auto h = wmoRenderer->getFloorHeight(p.x, p.y, reach, &nz);
                        if (h) {
                            if (nz >= MIN_WALKABLE_NORMAL_WMO && *h >= feetZ - movement::kMaxStepUp)
                                walkableStructure = true;
                            best = h; bestNz = nz; source = 2;
                        }
                    }
                    if (m2Renderer) {
                        float nz = 1.0f;
                        auto h = m2Renderer->getFloorHeight(p.x, p.y, reach, &nz);
                        if (h && *h <= reach) {
                            if (nz >= MIN_WALKABLE_NORMAL_M2 && *h >= feetZ - movement::kMaxStepUp)
                                walkableStructure = true;
                            if (!best || *h > *best) { best = h; bestNz = nz; source = 3; }
                        }
                    }
                    if (terrainManager && !cachedInsideWMO && !terrainManager->isHoleAt(p.x, p.y)) {
                        auto th = terrainManager->getHeightAt(p.x, p.y);
                        if (th && *th <= reach && (!best || *th > *best)) {
                            // Half a heightfield cell apart, so the difference
                            // reads the hillside, not the crease between the
                            // four triangles each cell is fanned into.
                            constexpr float kTerrainSlopeSpacing = 2.0f;
                            terrainN = movement::heightfieldNormal(
                                [this](float sx, float sy) { return terrainManager->getHeightAt(sx, sy); },
                                p.x, p.y, kTerrainSlopeSpacing);
                            best = th; bestNz = terrainN.z; source = 1;
                        }
                    }
                    if (!best || *best <= feetZ + 0.02f ||
                        bestNz >= movement::kMinWalkableNormalZ) {
                        return std::nullopt;
                    }
                    // At a tunnel mouth or a deck over a hillside, a walkable
                    // structure floor within reach is what is walked on, and
                    // the terrain beside it is not climbed.
                    if (source == 1 && walkableStructure) return std::nullopt;
                    glm::vec2 up(0.0f);
                    if (source == 1) {
                        up = -glm::vec2(terrainN.x, terrainN.y);
                    } else {
                        // A floor query answers a height and the normal's z,
                        // not which way the surface rises; difference it.
                        constexpr float d = 0.3f;
                        const float probe = *best + 0.6f;
                        const auto q = [&](float x, float y) -> std::optional<float> {
                            return source == 2 ? wmoRenderer->getFloorHeight(x, y, probe)
                                               : m2Renderer->getFloorHeight(x, y, probe);
                        };
                        const auto e = q(p.x + d, p.y), w = q(p.x - d, p.y);
                        const auto n = q(p.x, p.y + d), s = q(p.x, p.y - d);
                        if (e && w && n && s) up = glm::vec2(*e - *w, *n - *s);
                    }
                    const float upLen = glm::length(up);
                    return upLen > 1e-4f ? up / upLen : move / moveLen;
                };
                if (const auto up = steepClimbAt(from + move)) {
                    const glm::vec2 slid = movement::slideAlongContour(move, *up);
                    glm::vec2 dest = from + slid;
                    // In a crease the contour climbs too: no step at all.
                    if (glm::length(slid) <= 1e-4f || steepClimbAt(dest)) dest = from;
                    const glm::vec3 swept = sweepAgainstWalls(
                        prevTargetPos, glm::vec3(dest.x, dest.y, targetPos.z), true);
                    targetPos.x = swept.x;
                    targetPos.y = swept.y;
                }
            }
        }

        const bool airborne = !grounded && !hoverActive_ && !externalFollow_ &&
                              !isFlightAirborne();
        const float horizStep = glm::length(glm::vec2(targetPos.x - prevTargetPos.x,
                                                      targetPos.y - prevTargetPos.y));
        // What the feet may meet this frame: anything between where they
        // started and where they ended, plus a walkable slope's rise over the
        // horizontal step and a little contact noise.
        const auto crossTopFor = [&](float step) {
            return std::max(prevTargetPos.z, targetPos.z) + 0.10f + step * 1.2f;
        };
        float crossTop = crossTopFor(horizStep);
        if (airborne && horizStep > 1e-4f && horizStep < 8.0f) {
            const float reachZ = targetPos.z + AIR_LEDGE_REACH;
            // The highest surface at a column a rising body could run into:
            // structure tops within reach of the feet, and terrain however
            // high (a hillside face is a wall), unless a structure floor
            // there says the column is a tunnel or a cave under the ground.
            const auto surfaceTopAt = [&](float x, float y) -> std::optional<float> {
                std::optional<float> top;
                bool structure = false;
                if (wmoRenderer) {
                    float nz = 1.0f;
                    auto h = wmoRenderer->getFloorHeight(x, y, reachZ, &nz);
                    if (h) structure = true;
                    if (h && nz >= MIN_WALKABLE_NORMAL_WMO) top = h;
                }
                if (m2Renderer) {
                    float nz = 1.0f;
                    auto h = m2Renderer->getFloorHeight(x, y, reachZ, &nz);
                    // The M2 query answers hits up to 3 yd over its probe;
                    // only what is within reach is a ledge here.
                    if (h && *h > reachZ) h = std::nullopt;
                    if (h) structure = true;
                    if (h && nz >= MIN_WALKABLE_NORMAL_M2 && (!top || *h > *top)) top = h;
                }
                if (terrainManager && !structure && !cachedInsideWMO &&
                    !terrainManager->chunkHasHoles(x, y)) {
                    auto th = terrainManager->getHeightAt(x, y);
                    if (th && (!top || *th > *top)) top = th;
                }
                return top;
            };
            const auto blockedAt = [&](float x, float y, float limit) {
                const auto top = surfaceTopAt(x, y);
                return top && *top > limit ? top : std::optional<float>{};
            };
            if (const auto ahead = blockedAt(targetPos.x, targetPos.y, crossTop)) {
                // Already under or beside the same thing where the step began
                // (a table, an overhang, a tunnel roof): refusing the step
                // cannot help, and the floor pass below will not lift onto it.
                const bool alreadyUnder =
                    blockedAt(prevTargetPos.x, prevTargetPos.y, crossTopFor(0.0f)).has_value();
                if (!alreadyUnder) {
                    if (verticalVelocity <= 0.0f && *ahead <= targetPos.z + movement::kMaxStepUp) {
                        // Coming down against a lip no taller than a step:
                        // the feet catch it, as a step taken on the ground
                        // would. Physics takes the top; the picture eases up.
                        renderZOffset_ -= *ahead - targetPos.z;
                        targetPos.z = *ahead;
                        verticalVelocity = 0.0f;
                        grounded = true;
                        lastGroundZ = *ahead;
                    } else {
                        // A ledge side: keep the arc, give up the horizontal
                        // step - or the part of it along the ledge.
                        const glm::vec2 slideX(targetPos.x, prevTargetPos.y);
                        const glm::vec2 slideY(prevTargetPos.x, targetPos.y);
                        const float stepX = std::abs(targetPos.x - prevTargetPos.x);
                        const float stepY = std::abs(targetPos.y - prevTargetPos.y);
                        const glm::vec2 first = stepX >= stepY ? slideX : slideY;
                        const glm::vec2 second = stepX >= stepY ? slideY : slideX;
                        const float firstStep = std::max(stepX, stepY);
                        const float secondStep = std::min(stepX, stepY);
                        glm::vec2 kept(prevTargetPos.x, prevTargetPos.y);
                        if (firstStep > 1e-4f &&
                            !blockedAt(first.x, first.y, crossTopFor(firstStep))) {
                            kept = first;
                        } else if (secondStep > 1e-4f &&
                                   !blockedAt(second.x, second.y, crossTopFor(secondStep))) {
                            kept = second;
                        }
                        targetPos.x = kept.x;
                        targetPos.y = kept.y;
                        crossTop = crossTopFor(glm::length(
                            kept - glm::vec2(prevTargetPos.x, prevTargetPos.y)));
                    }
                }
            }
        }

        // Grounded: the stock step height - the object scale, at least 1 yd
        // (0x6E9520 via 0x761B00), which is also where the WMO and doodad wall
        // tests stop calling geometry a wall. It was 0.6, so a riser between
        // 0.6 and 1 yd was neither a wall nor a step: walked into rather than
        // onto. Airborne: only what the feet crossed.
        float stepUpBudget = grounded
            ? collider_.stepHeight
            : std::clamp(crossTop - targetPos.z, 0.10f, 3.0f);
        // 1. Center-only sample for terrain/WMO floor selection.
        //    Using only the center prevents tunnel entrances from snapping
        //    to terrain when offset samples miss the WMO floor geometry.
        // Slope limit: reject surfaces too steep to walk (prevent clipping).
        // WMO tunnel/bridge ramps are often steeper than outdoor terrain ramps.
        // The floor directly under the feet, and the surfaces that answered.
        FloorSample sample = sampleFloorUnderFeet(targetPos, stepUpBudget);
        std::optional<float> groundH = sample.floor;
        const std::optional<float> centerTerrainH = sample.terrain;
        const std::optional<float> centerWmoH = sample.wmo;
        const std::optional<float> centerM2H = sample.m2;

        // Transition safety: if no reachable floor was selected, choose the higher
        // of terrain/WMO center surfaces when it is still near the player.
        // This avoids dropping into void gaps at terrain<->WMO seams.
        // Whether there is structure here at all, which gates every recovery
        // below it - the multi-sample pass, the wider rescue rings, all of it.
        //
        // It rested on a ray at the character's centre, and a centre ray is the
        // one thing that misses the lip of a surface: at the top of the
        // Undercity lift the landing lies a third of a yard away in +y at the
        // height of the feet, the centre finds nothing, so this said there is
        // no structure nearby and shut off the passes that would have found it.
        // The player steps off the deck and dips into the gap.
        //
        // The hint asks the foot cross now, the same four points the pass it
        // guards would use. It is the last thing standing between the player
        // and a fall, so it should not fail for the reason those passes exist.
        const bool nearWmoSpace = cachedInsideWMO || centerWmoH.has_value();
        bool nearStructureSpace = nearWmoSpace || centerM2H.has_value();
        if (!nearStructureSpace && hasRealGround_) {
            const auto nearLastGround = [&](std::optional<float> h) {
                return h && std::abs(*h - lastGroundZ) <= 2.0f;
            };
            if (wmoRenderer) {
                if (nearLastGround(highestWalkableFloor(
                        [this](float x, float y, float z, float* nz) {
                            return wmoRenderer->getFloorHeight(x, y, z, nz);
                        },
                        targetPos.x, targetPos.y, feetCross(0.35f), lastGroundZ + 1.5f,
                        MIN_WALKABLE_NORMAL_WMO,
                        {.minZ = lastGroundZ - 2.0f, .maxZ = lastGroundZ + 2.0f}))) {
                    nearStructureSpace = true;
                }
            }
            if (!nearStructureSpace && m2Renderer && !externalFollow_) {
                if (nearLastGround(highestWalkableFloor(
                        [this](float x, float y, float z, float* nz) {
                            return m2Renderer->getFloorHeight(x, y, z, nz);
                        },
                        targetPos.x, targetPos.y, feetCross(0.35f), lastGroundZ + 1.5f,
                        MIN_WALKABLE_NORMAL_M2,
                        {.minZ = lastGroundZ - 2.0f, .maxZ = lastGroundZ + 2.0f}))) {
                    nearStructureSpace = true;
                }
            }
        }
        if (!groundH) {
            auto highestCenter = selectHighestFloor(centerTerrainH, centerWmoH, centerM2H);
            if (highestCenter) {
                float dz = targetPos.z - *highestCenter;
                // Keep this fallback narrow: only for WMO seam cases, or very short
                // transient misses while still almost touching the last floor.
                bool allowFallback = nearStructureSpace || (noGroundTimer_ < 0.10f && dz < 0.6f);
                if (allowFallback && dz >= -0.5f && dz < 2.0f) {
                    groundH = highestCenter;
                }
            }
        }

        // Continuity guard only for WMO seam overlap: avoid instantly switching to a
        // much lower floor sample at tunnel mouths (bad WMO ramp chains into void).
        if (groundH && hasRealGround_ && nearWmoSpace && !cachedInsideInteriorWMO) {
            float dropFromLast = lastGroundZ - *groundH;
            if (dropFromLast > 1.5f) {
                if (centerTerrainH && *centerTerrainH > *groundH + 1.5f) {
                    groundH = centerTerrainH;
                }
            }
        }

        // Seam stability: while overlapping WMO shells, cap how fast floor height can
        // step downward in a single frame to avoid following bad ramp samples into void.
        if (groundH && nearWmoSpace && !cachedInsideInteriorWMO && lastGroundZ > 1.0f) {
            float maxDropPerFrame = (verticalVelocity < -8.0f) ? 2.0f : 0.60f;
            float minAllowed = lastGroundZ - maxDropPerFrame;
            // Extra seam guard: outside interior groups, avoid accepting floors that
            // are far below nearby terrain. Keeps shark-mouth transitions from
            // following erroneous WMO ramps into void.
            // Not while standing inside a building. Terrain is the ground
            // *outside*, and a staircase descending below it - Gadgetzan's
            // auction house, any cellar - is a legitimate floor far under
            // nearby terrain, which is exactly what this refuses. The
            // player hovered over the steps and sank at the clamp's rate
            // instead of walking down them.
            //
            // The interior test above is not enough on its own: it wants
            // the point a metre over the player's head inside a group
            // flagged 0x2000, and at the top of a stairwell that group's
            // box has not started yet. Being inside the WMO at all is the
            // question this guard should have asked.
            if (centerTerrainH && !cachedInsideWMO) {
                // Never let terrain-based seam guard push floor above current feet;
                // it should only prevent excessive downward drops.
                float terrainGuard = std::min(*centerTerrainH - 1.0f, targetPos.z - 0.15f);
                minAllowed = std::max(minAllowed, terrainGuard);
            }
            if (*groundH < minAllowed) {
                *groundH = minAllowed;
            }
        }

        // Structure continuity guard: if a floor query suddenly jumps far below
        // recent support while near dock/bridge geometry, keep a conservative
        // support height to avoid dropping through sparse collision seams.
        if (groundH && hasRealGround_ && nearStructureSpace && !f.nowJump) {
            float dropFromLast = lastGroundZ - *groundH;
            // Only reject the lower sample while the feet are still up near
            // lastGroundZ - that is the transient bad-ramp case this guard is
            // for. Once the feet have actually descended below the clamp
            // target the player is standing on the lower floor, and pinning
            // groundH back up to lastGroundZ leaves the ground reference
            // hanging above the feet so gravity and the snap fight over the
            // gap forever. That is the Undercity overhang yo-yo: walk under
            // the level above, lastGroundZ stays stuck on it (-43) while the
            // real floor (-47) is refused, and the feet bob in between. If
            // the feet are already past the target, let the real floor
            // through so lastGroundZ can follow the player down onto it.
            if (dropFromLast > 1.0f && verticalVelocity > -6.0f &&
                targetPos.z > lastGroundZ - 0.20f) {
                *groundH = std::max(*groundH, lastGroundZ - 0.20f);
            }
        }

        // Void recovery: far beneath the terrain heightfield with no structure
        // floor anywhere below usually means a seam heuristic already failed.
        // Never use the heightfield as a rescue target while WMO containment says
        // the player is inside, though: Ironforge's valid interior floor is over
        // 200 units below the mountain terrain, and a transient floor-query miss
        // must not teleport the player onto the mountain above the city.
        if (!groundH && centerTerrainH && !cachedInsideWMO &&
            targetPos.z < *centerTerrainH - 60.0f) {
            LOG_WARNING("Void recovery: player at z=", targetPos.z,
                        " with terrain at ", *centerTerrainH, " and no floor below");
            targetPos.z = *centerTerrainH + 0.5f;
            verticalVelocity = 0.0f;
            groundH = centerTerrainH;
        }

        // WMO-only maps (Deeprun Tram, instances), and deep WMO interiors on
        // terrain maps (Ironforge), must recover to the last structural floor
        // rather than the unrelated outdoor heightfield above them.
        if (!groundH && (!centerTerrainH || cachedInsideWMO) && hasLastGroundedPos_ &&
            noGroundTimer_ > 2.5f && targetPos.z < lastGroundZ - 40.0f) {
            LOG_WARNING("Void recovery (WMO/ no heightfield): player at z=", targetPos.z,
                        " returning to last grounded pos (", lastGroundedPos_.x, ", ",
                        lastGroundedPos_.y, ", ", lastGroundedPos_.z, ")");
            targetPos = lastGroundedPos_ + glm::vec3(0.0f, 0.0f, 0.5f);
            verticalVelocity = 0.0f;
            groundH = lastGroundedPos_.z;
        }

        // 1b. Multi-sample WMO floors when in/near WMO space to avoid
        // falling through narrow board/plank gaps where center ray misses.
        if (wmoRenderer && nearWmoSpace) {
            // Airborne the budget is already only what the feet crossed this
            // frame, measured from the feet; anchoring to the last ground
            // instead hid a ledge top the arc had cleared, so the player fell
            // through a plank deck they jumped onto.
            const float wmoMultiBaseZ = grounded ? std::max(targetPos.z, lastGroundZ) : targetPos.z;
            const float wmoProbeZ = wmoMultiBaseZ + stepUpBudget + (grounded ? 0.6f : 0.05f);
            const float minWalkableWmo =
                cachedInsideWMO ? MIN_WALKABLE_NORMAL_WMO : MIN_WALKABLE_NORMAL_TERRAIN;
            const float maxZ = targetPos.z + stepUpBudget;

            auto wh = highestWalkableFloor(
                [this](float x, float y, float z, float* nz) {
                    return wmoRenderer->getFloorHeight(x, y, z, nz);
                },
                // The floor footprint, as the M2 ring below: a plank gap is
                // bridged, a ledge edge is not held onto from a step past it.
                targetPos.x, targetPos.y, feetCross(kFloorFootprint), wmoProbeZ, minWalkableWmo,
                {.minZ = lastGroundZ - 3.5f, .maxZ = maxZ});
            if (wh && (!groundH || *wh > *groundH)) groundH = wh;
        }

        // WMO recovery probe: when no floor is found while descending, do a wider
        // footprint sample around the player to catch narrow plank/stair misses.
        if (!groundH && wmoRenderer && hasRealGround_ && verticalVelocity <= 0.0f) {
            const float rescueProbeZ = std::max(lastGroundZ, targetPos.z) + stepUpBudget + 1.2f;
            auto rescueFloor = highestWalkableFloor(
                [this](float x, float y, float z, float* nz) {
                    return wmoRenderer->getFloorHeight(x, y, z, nz);
                },
                targetPos.x, targetPos.y, feetRing(0.65f), rescueProbeZ,
                MIN_WALKABLE_NORMAL_WMO,
                {.minZ = lastGroundZ - 6.0f, .maxZ = lastGroundZ + stepUpBudget + 0.75f});
            if (rescueFloor) groundH = rescueFloor;
        }

        // M2 recovery probe: Booty Bay-style wooden platforms can be represented
        // as M2 collision where center probes intermittently miss.
        if (!groundH && m2Renderer && !externalFollow_ && hasRealGround_ && verticalVelocity <= 0.0f) {
            const float rescueProbeZ = std::max(lastGroundZ, targetPos.z) + stepUpBudget + 1.4f;
            auto rescueFloor = highestWalkableFloor(
                [this](float x, float y, float z, float* nz) {
                    return m2Renderer->getFloorHeight(x, y, z, nz);
                },
                targetPos.x, targetPos.y, feetRing(0.75f), rescueProbeZ,
                MIN_WALKABLE_NORMAL_M2,
                {.minZ = lastGroundZ - 6.0f, .maxZ = lastGroundZ + stepUpBudget + 0.90f});
            if (rescueFloor) groundH = rescueFloor;
        }

        // Path recovery probe: sample structure floors along the movement segment
        // (prev -> current) to catch narrow plank gaps missed at endpoints.
        if (!groundH && hasRealGround_ && (wmoRenderer || (m2Renderer && !externalFollow_))) {
            std::optional<float> segmentFloor;
            const float probeZ = std::max(lastGroundZ, targetPos.z) + stepUpBudget + 1.2f;
            const float ts[] = {0.25f, 0.5f, 0.75f};
            for (float t : ts) {
                float sx = prevTargetPos.x + (targetPos.x - prevTargetPos.x) * t;
                float sy = prevTargetPos.y + (targetPos.y - prevTargetPos.y) * t;

                if (wmoRenderer) {
                    float nz = 1.0f;
                    auto wh = wmoRenderer->getFloorHeight(sx, sy, probeZ, &nz);
                    if (wh && nz >= MIN_WALKABLE_NORMAL_WMO &&
                        *wh <= lastGroundZ + stepUpBudget + 0.9f &&
                        *wh >= lastGroundZ - 3.0f) {
                        if (!segmentFloor || *wh > *segmentFloor) segmentFloor = wh;
                    }
                }
                if (m2Renderer && !externalFollow_) {
                    float nz = 1.0f;
                    auto mh = m2Renderer->getFloorHeight(sx, sy, probeZ, &nz);
                    if (mh && nz >= MIN_WALKABLE_NORMAL_M2 &&
                        *mh <= lastGroundZ + stepUpBudget + 0.9f &&
                        *mh >= lastGroundZ - 3.0f) {
                        if (!segmentFloor || *mh > *segmentFloor) segmentFloor = mh;
                    }
                }
            }
            if (segmentFloor) {
                groundH = segmentFloor;
            }
        }

        // 2. Multi-sample for M2 objects (rugs, planks, bridges, ships) -
        //    these are narrow and need offset probes to detect reliably.
        if (m2Renderer && !externalFollow_) {
            // Not highestWalkableFloor: this one prefers an M2 floor even when
            // it is slightly lower than the terrain, so a ship's deck wins over
            // the water under it.
            //
            // The ring is the floor footprint, not the body: stock stands the
            // player on a surface only when the point under the feet, give or
            // take a fraction of a yard, is over it. At 0.6 (the body radius)
            // a fence rail 0.27 thick held the player up anywhere within 0.7
            // of its centre line, floating beside it after walking off its top.
            const auto offsets = feetRing(kFloorFootprint);
            float m2ProbeZ = std::max(targetPos.z, lastGroundZ) + 6.0f;
            for (const auto& o : offsets) {
                float m2NormalZ = 1.0f;
                auto m2H = m2Renderer->getFloorHeight(
                    targetPos.x + o.x, targetPos.y + o.y, m2ProbeZ, &m2NormalZ);

                // Reject steep M2 slopes
                if (m2H && m2NormalZ < MIN_WALKABLE_NORMAL_TERRAIN) {
                    continue;  // Skip unwalkable M2 surface
                }

                // Prefer M2 floors (ships, platforms) even if slightly lower than terrain
                // to prevent falling through ship decks to water below
                if (m2H && *m2H <= targetPos.z + stepUpBudget) {
                    if (!groundH || *m2H > *groundH ||
                        (*m2H >= targetPos.z - 0.5f && *groundH < targetPos.z - 1.0f)) {
                        groundH = m2H;
                    }
                }
            }
        }

        // Outdoors the heightfield has exactly one surface per column, so feet
        // below it means the player is inside the hill, which is never a valid
        // position. Climbing a slope that rises faster than the step-up budget
        // - a steep hill, or an ordinary one crossed in a long frame - got the
        // terrain rejected as unreachable by selectReachableFloor3. Once inside,
        // every later sample was rejected the same way and the gap only widened
        // as the player fell, so nothing recovered until the void check fired 60
        // yards down. Push back out to the surface instead.
        //
        // Only where there is nothing else the player could be standing in or
        // under: a cave, a tunnel or Ironforge is legitimately beneath the
        // heightfield, and must never be yanked up onto the mountain above it.
        // Standing on something settles it before any of the probing below.
        // Grounding has already resolved a floor for this exact position; if
        // that floor sits under the heightfield and is right at the feet, the
        // player is on a structure beneath the terrain, not buried inside it.
        // That is better evidence than the probe further down, which asks
        // from a fixed height above the feet and can miss what grounding
        // already found.
        //
        // Stairs descending below ground are the case that needs it: a
        // stairwell cut into a keep passes under the heightfield within a step
        // or two, and being hauled back to the surface each time reads as not
        // being able to walk down them at all.
        const bool standingOnStructure =
            groundH && centerTerrainH &&
            *groundH < *centerTerrainH - 0.5f &&
            *groundH >= targetPos.z - 2.0f &&
            *groundH <= targetPos.z + 0.6f;

        // In flight as well: flightStepAgainstGround keeps a mount from flying
        // into a hill, and this is what brings one back out that is already in
        // it.
        if (!swimming && !hoverActive_ && !externalFollow_ &&
            !cachedInsideWMO && !nearStructureSpace && !standingOnStructure &&
            centerTerrainH && verticalVelocity <= 0.0f) {
            const float penetration = *centerTerrainH - targetPos.z;
            // Below the shallow bound is ordinary contact and sampling jitter;
            // above the deep bound is somewhere this heuristic cannot vouch for,
            // which the void recovery above already handles.
            constexpr float kMinPenetration = 0.10f;
            constexpr float kMaxPenetration = 12.0f;
            if (penetration > kMinPenetration && penetration < kMaxPenetration) {
                // Everywhere the player may legitimately stand below the
                // heightfield - the Darkshire crypts, the tunnel under the hill
                // into Booty Bay, any cave - is a structure sitting under the
                // terrain at this column. Probe for one directly instead of
                // trusting cachedInsideWMO and centerWmoH: the first lags by
                // design, and the second is cleared outright for floors more
                // than 12 yards below terrain while containment still reads
                // false, which is exactly what descending a tunnel mouth looks
                // like. Finding anything at all here means hands off - the
                // player belongs under the terrain, and lifting them out would
                // put them on the hillside above the entrance they just walked
                // into.
                // Probed from the player, not from the terrain surface: the
                // floor query culls any group whose top sits more than 4 yards
                // below the probe height, so probing from up at the heightfield
                // would skip the crypt or the tunnel entirely and report clear
                // ground - the exact opposite of the truth. From here the
                // structure the player is standing in is right above them.
                const float probeZ = targetPos.z + 1.5f;
                bool structureBelowTerrain = false;
                if (wmoRenderer &&
                    wmoRenderer->getFloorHeight(targetPos.x, targetPos.y, probeZ)) {
                    structureBelowTerrain = true;
                }
                if (!structureBelowTerrain && m2Renderer &&
                    m2Renderer->getFloorHeight(targetPos.x, targetPos.y, probeZ)) {
                    structureBelowTerrain = true;
                }
                // A hole-cut chunk is how a cave mouth or a below-ground
                // entrance is opened in the first place. getHeightAt
                // interpolates straight across the hole, so the surface this
                // rescue would push up to is not there at all - and the
                // structure underneath may still be too far below to have been
                // found by the probes above.
                if (!structureBelowTerrain && terrainManager &&
                    terrainManager->chunkHasHoles(targetPos.x, targetPos.y)) {
                    structureBelowTerrain = true;
                }

                if (!structureBelowTerrain) {
                    if (!terrainRescueActive_) {
                        terrainRescueActive_ = true;
                        LOG_INFO("Terrain penetration rescue: feet ", penetration,
                                 " below the heightfield at (", targetPos.x, ", ",
                                 targetPos.y, ") with no structure under it");
                    }
                    targetPos.z = *centerTerrainH;
                    verticalVelocity = 0.0f;
                    groundH = centerTerrainH;
                    lastGroundZ = *centerTerrainH;
                } else {
                    terrainRescueActive_ = false;
                }
            } else {
                terrainRescueActive_ = false;
            }
        } else {
            terrainRescueActive_ = false;
        }

        // On the ground under a building's floor.
        //
        // A raised floor sits a yard or so over the heightfield it is built on,
        // which is more than a step. Once the feet are down on that ground -
        // one frame at the doorway where the floor query misses the threshold
        // is enough - the floor is out of reach of the step-up, so it is never
        // chosen again, and the player walks about inside the building sunk to
        // the chest in its floor. Reported entering a house at an angle.
        //
        // Nowhere legitimate to stand has a walkable floor across the whole
        // footprint at waist height: the body would be inside it. Put the feet
        // back on it. The centre is asked first, so the four around it cost
        // nothing on the frames that matter least - every one where the answer
        // is no.
        //
        // Grounded only: a body in the air beside a raised floor is mid-jump,
        // and lifting it onto the floor is the pop the arc rule above removes.
        if (grounded && !isFlightAirborne() && !hoverActive_ && !externalFollow_ && wmoRenderer &&
            nearWmoSpace && groundH && centerTerrainH && *groundH == *centerTerrainH) {
            constexpr float kWaistHeight = 1.8f;
            const float feetZ = targetPos.z;
            const auto slabAt = [&](const glm::vec2& o) -> std::optional<float> {
                float nz = 1.0f;
                const auto h = wmoRenderer->getFloorHeight(
                    targetPos.x + o.x, targetPos.y + o.y, feetZ + kWaistHeight, &nz);
                if (!h || nz < MIN_WALKABLE_NORMAL_WMO) return std::nullopt;
                if (*h <= feetZ + stepUpBudget || *h >= feetZ + kWaistHeight) return std::nullopt;
                return h;
            };
            const auto cross = feetCross(0.35f);
            std::optional<float> slab = slabAt(cross[0]);
            for (std::size_t i = 1; slab && i < cross.size(); ++i) {
                if (!slabAt(cross[i])) slab = std::nullopt;
            }
            if (slab) {
                LOG_WARNING("Under a floor: feet ", feetZ, " on the ground at (", targetPos.x,
                            ", ", targetPos.y, ") with a WMO floor at ", *slab,
                            " across the footprint - back onto it");
                targetPos.z = *slab;
                groundH = slab;
                lastGroundZ = *slab;
                verticalVelocity = 0.0f;
            }
        }

        // WOWEE_FLOOR_DEBUG=1 - what every floor query answered and which
        // one won, four times a second.
        //
        // Two fixes for one report of not being able to walk down a
        // staircase were both aimed at the wrong thing, because from the
        // outside "standing on nothing" looks the same whether the floor
        // below was never found, was found and rejected, or was found and
        // lost a selection. Those are different bugs in different places
        // and this is the line that tells them apart.
        {
            static constexpr const char* kFloorDebugPath = "/tmp/wowee_floor.log";
            static const bool kFloorDebug = [] {
                const char* v = std::getenv("WOWEE_FLOOR_DEBUG");
                return v && v[0] && v[0] != '0';
            }();
            if (kFloorDebug) {
                floorDebugTimer_ += deltaTime;
                if (floorDebugTimer_ >= 0.25f) {
                    floorDebugTimer_ = 0.0f;
                    auto say = [](const std::optional<float>& v) {
                        return v ? std::to_string(*v) : std::string("-");
                    };
                    std::ostringstream line;
                    line << "FLOOR at (" << targetPos.x << ", " << targetPos.y
                         << ", " << targetPos.z << ")"
                         << " terrain=" << say(centerTerrainH)
                         << " wmo="     << say(centerWmoH)
                         << " m2="      << say(centerM2H)
                         << " chose="   << say(groundH)
                         << " hole="    << (terrainManager &&
                                terrainManager->isHoleAt(targetPos.x, targetPos.y))
                         << " insideWMO="      << cachedInsideWMO
                         << " insideInterior=" << cachedInsideInteriorWMO
                         << " grounded="       << grounded
                         << " vz="             << verticalVelocity
                         << " lastGroundZ="    << lastGroundZ;
                    LOG_WARNING(line.str());
                    // Also to a file. The console is where this used to go
                    // and it scrolled past - asking someone to reproduce a
                    // bug and then hand back terminal scrollback is asking
                    // twice. Truncated on the first line of a run so the
                    // file is always this session's.
                    static std::ofstream trace(kFloorDebugPath,
                                               std::ios::out | std::ios::trunc);
                    if (trace) trace << line.str() << '\n' << std::flush;
                }
            }
        }

        if (groundH) {
            hasRealGround_ = true;
            noGroundTimer_ = 0.0f;
            lastGroundedPos_ = glm::vec3(targetPos.x, targetPos.y, *groundH);
            hasLastGroundedPos_ = true;
            float feetZ = targetPos.z;
            float stepUp = stepUpBudget;
            stepUp += 0.05f;
            float fallCatch = 3.0f;
            float dz = *groundH - feetZ;

            if (airborne && !grounded) {
                // In the air: land where the arc meets the surface.
                //
                // The feet crossed it this frame when it lies between where
                // they started and where they ended (crossTop carries a little
                // slack for a slope met mid-step). Physics takes the surface
                // exactly, which is the intersection; nothing is eased, since
                // the picture was already moving down through that height.
                if (*groundH > crossTop + 0.01f) {
                    // Above anything the feet reached - a recovery probe's
                    // answer, not a floor met. Keep flying the arc.
                    grounded = false;
                } else if (dz >= 0.0f) {
                    // Only the part of the correction above both ends of the
                    // frame's motion is a jump the eye would see.
                    const float lift = *groundH - std::max(prevTargetPos.z, feetZ);
                    if (lift > 0.0f) renderZOffset_ -= lift;
                    targetPos.z = *groundH;
                    lastGroundZ = *groundH;
                    if (verticalVelocity <= 0.0f) {
                        verticalVelocity = 0.0f;
                        grounded = true;
                    } else {
                        // Rising up a slope met mid-jump: the feet ride its
                        // surface and the jump carries on.
                        grounded = false;
                    }
                } else if (verticalVelocity <= 0.0f && dz >= -0.02f) {
                    // Settled onto it within contact noise.
                    targetPos.z = *groundH;
                    verticalVelocity = 0.0f;
                    grounded = true;
                    lastGroundZ = *groundH;
                } else {
                    grounded = false;
                    lastGroundZ = *groundH;
                }
            } else {
            // On the ground (or hovering): follow it within the step height.
            //
            // 1. Near ground (within step-up range above) - a step up.
            // 2. Hovering and falling onto the hover height.
            // 3. Grounded with the ground just below - a slope or a step down.
            //    Walking, that reaches a full step so stairs and curbs are
            //    walked down rather than fallen down (the stock client keeps
            //    the feet on the ground the same way); standing, only the
            //    quarter yard that absorbs contact noise.
            bool nearGround = (dz >= 0.0f && dz <= stepUp);
            float airSnapRange = std::min(fallCatch, 0.05f);
            bool airFalling = (hoverActive_ && !grounded && verticalVelocity < -5.0f
                               && dz >= -airSnapRange);
            const bool walkingOnGround = horizStep > 1e-3f && !isFlightAirborne() &&
                                         !hoverActive_;
            const float stepDownReach = walkingOnGround ? movement::kMaxStepUp : 0.25f;
            // Not while climbing in flight: a frame's climb at a high frame
            // rate is less than the quarter yard this reaches down, so taking
            // off was pulled back to the ground every frame and never left it.
            // Up only as far as the step height: this reached half a step
            // beyond it (stepUp * 1.5), a second snap above the first.
            bool slopeGrace = (grounded && verticalVelocity > -1.0f &&
                               dz >= -stepDownReach && dz <= stepUp) &&
                              !(isFlightAirborne() && f.spaceHeld);

            if (dz >= -fallCatch && (nearGround || airFalling || slopeGrace)) {
                // HOVER: float at fixed height above ground instead of standing on it
                static constexpr float HOVER_HEIGHT = 4.0f;  // ~4 yards above ground
                const float snapH = hoverActive_ ? (*groundH + HOVER_HEIGHT) : *groundH;
                // A step: the physics feet move to it this frame, the picture
                // eases after them. Only a change sharper than walking a
                // slope is a step - easing every slope frame would leave the
                // drawn feet trailing the ground on every hillside.
                const float snapDz = snapH - targetPos.z;
                const float slopeDz = horizStep * 1.0f + 0.05f;
                if (!hoverActive_ && !isFlightAirborne() && std::abs(snapDz) > slopeDz) {
                    renderZOffset_ -= snapDz;
                }
                targetPos.z = snapH;
                verticalVelocity = 0.0f;
                grounded = true;
                lastGroundZ = *groundH;
            } else {
                grounded = false;
                lastGroundZ = *groundH;
            }
            }
            } else {
                hasRealGround_ = false;
                noGroundTimer_ += f.physicsDeltaTime;

                float dropFromLastGround = lastGroundZ - targetPos.z;
                bool seamSizedGap = dropFromLastGround <= (nearStructureSpace ? 2.5f : 0.35f);
                if (noGroundTimer_ < NO_GROUND_GRACE && seamSizedGap) {
                    // Near WMO floors, prefer continuity over falling on transient
                    // floor-query misses (stairs/planks/portal seams).
                    float maxSlip = nearStructureSpace ? 1.0f : 0.10f;
                    targetPos.z = std::max(targetPos.z, lastGroundZ - maxSlip);
                    if (nearStructureSpace && verticalVelocity < -2.0f) {
                        verticalVelocity = -2.0f;
                    }
                    grounded = false;
                } else if (nearStructureSpace && noGroundTimer_ < 1.0f && dropFromLastGround <= 3.0f) {
                    // Extended WMO rescue window: hold close to last valid floor so we
                    // do not tunnel through walkable geometry during short hitches.
                    targetPos.z = std::max(targetPos.z, lastGroundZ - 0.35f);
                    if (verticalVelocity < -1.5f) {
                        verticalVelocity = -1.5f;
                    }
                    grounded = false;
                } else if (nearStructureSpace && noGroundTimer_ < 1.20f && dropFromLastGround <= 4.0f && !f.nowJump) {
                    // Extended adhesion for sparse dock/bridge collision: keep us on the
                    // last valid support long enough for adjacent structure probes to hit.
                    targetPos.z = std::max(targetPos.z, lastGroundZ - 0.10f);
                    if (verticalVelocity < -0.5f) verticalVelocity = -0.5f;
                    grounded = true;
                } else {
                    grounded = false;
                }
            }
        }

    // Left standing on ground steeper than the limit - landed on a cliff from
    // a jump or a fall, or pushed there - the character slides down it under
    // gravity until the ground is walkable again, as stock does. Terrain only:
    // a WMO or M2 surface that steep is never chosen as a floor in the first
    // place (MIN_WALKABLE_NORMAL_WMO / _M2 in sampleFloorUnderFeet), and a
    // walkable structure floor at the feet always wins over the hillside under
    // it, which keeps tunnel ramps and decks out of this.
    {
        bool steep = false;
        float steepNz = 1.0f;
        glm::vec2 downhill(0.0f);
        const bool slideApplies = !swimming && !ignoreSlopeLimit_ && !hoverActive_ &&
                                  !externalFollow_ && !isFlightAirborne() && !sitting &&
                                  !gravityDisabled_;
        if (slideApplies && grounded && terrainManager && !cachedInsideInteriorWMO &&
            !terrainManager->isHoleAt(targetPos.x, targetPos.y)) {
            const auto th = terrainManager->getHeightAt(targetPos.x, targetPos.y);
            if (th && std::abs(*th - targetPos.z) < 0.15f) {
                constexpr float kTerrainSlopeSpacing = 2.0f;  // as the uphill rule
                const glm::vec3 n = movement::heightfieldNormal(
                    [this](float sx, float sy) { return terrainManager->getHeightAt(sx, sy); },
                    targetPos.x, targetPos.y, kTerrainSlopeSpacing);
                const glm::vec2 d(n.x, n.y);
                if (n.z < movement::kMinWalkableNormalZ && glm::length(d) > 1e-4f) {
                    bool onStructure = false;
                    if (wmoRenderer) {
                        float nz = 1.0f;
                        auto h = wmoRenderer->getFloorHeight(targetPos.x, targetPos.y,
                                                             targetPos.z + 0.3f, &nz);
                        onStructure = h && std::abs(*h - targetPos.z) < 0.15f &&
                                      nz >= MIN_WALKABLE_NORMAL_WMO;
                    }
                    if (!onStructure && m2Renderer) {
                        float nz = 1.0f;
                        auto h = m2Renderer->getFloorHeight(targetPos.x, targetPos.y,
                                                            targetPos.z + 0.3f, &nz);
                        onStructure = h && std::abs(*h - targetPos.z) < 0.15f &&
                                      nz >= MIN_WALKABLE_NORMAL_M2;
                    }
                    if (!onStructure) {
                        steep = true;
                        steepNz = n.z;
                        downhill = glm::normalize(d);  // the normal leans downhill
                    }
                }
            }
        }
        onSteepSlope_ = steep;
        const float dt = f.physicsDeltaTime;
        if (!slideApplies) {
            slopeSlideVel_ = glm::vec2(0.0f);
        } else if (steep) {
            // Only the downhill part is kept, so momentum from another face
            // does not carry the character sideways off this one.
            const float along = std::max(0.0f, glm::dot(slopeSlideVel_, downhill));
            const float accel = movement::slopeSlideAccel(std::abs(f.gravity), steepNz);
            const float speed = std::min(along + accel * dt, SLOPE_SLIDE_MAX_SPEED);
            slopeSlideVel_ = downhill * speed;
        } else if (grounded) {
            // Walkable ground again: the slide stops within a tenth of a second.
            slopeSlideVel_ *= std::exp(-25.0f * dt);
            if (glm::dot(slopeSlideVel_, slopeSlideVel_) < 0.01f) slopeSlideVel_ = glm::vec2(0.0f);
        }
        // Airborne (slid off an edge): the momentum carries until a landing.
    }

    // Update follow target position
    *followTarget = targetPos;

    // --- Safe position caching + void fall detection ---
    if (grounded && hasRealGround_ && !swimming && verticalVelocity >= 0.0f) {
        // Player is safely on real geometry - save periodically
        continuousFallTime_ = 0.0f;
        autoUnstuckFired_ = false;
        safePosSaveTimer_ += f.physicsDeltaTime;
        if (safePosSaveTimer_ >= SAFE_POS_SAVE_INTERVAL) {
            safePosSaveTimer_ = 0.0f;
            lastSafePos_ = targetPos;
            hasLastSafe_ = true;
        }
    } else if (!grounded && !swimming && !externalFollow_) {
        // Falling (or standing on nothing past grace period) - accumulate fall time
        continuousFallTime_ += f.physicsDeltaTime;
        if (continuousFallTime_ >= AUTO_UNSTUCK_FALL_TIME && !autoUnstuckFired_) {
            autoUnstuckFired_ = true;
            if (autoUnstuckCallback_) {
                autoUnstuckCallback_();
            }
        }
    }
}

// The camera itself: where it sits relative to the character it follows.
//
// Pivot at the neck, zoom smoothing, the raycast that pulls the camera in when
// a wall is behind it, the floor clearance that stops it sinking through a
// tunnel roof, and the first-person threshold where the character is hidden.
void CameraController::updateOrbitCamera(float deltaTime, FrameInput& f,
                                         const glm::vec3& targetPos) {
    // ===== WoW-style orbit camera =====
    // Pivot point at upper chest/neck.
    float mountedOffset = mounted_ ? mountHeightOffset_ : 0.0f;
    float pivotLift = 0.0f;
    if (terrainManager && !externalFollow_ && !cachedInsideInteriorWMO) {
        float plx = targetPos.x - lastPivotLiftQueryPos_.x;
        float ply = targetPos.y - lastPivotLiftQueryPos_.y;
        float movedSq = plx * plx + ply * ply;
        constexpr float kPivotLiftPosSq = PIVOT_LIFT_POS_THRESHOLD * PIVOT_LIFT_POS_THRESHOLD;
        float distDelta = std::abs(currentDistance - lastPivotLiftDistance_);
        bool queryLift = (++pivotLiftQueryCounter_ >= PIVOT_LIFT_QUERY_INTERVAL) ||
                         (movedSq >= kPivotLiftPosSq) ||
                         (distDelta >= PIVOT_LIFT_DIST_THRESHOLD);
        if (queryLift) {
            pivotLiftQueryCounter_ = 0;
            lastPivotLiftQueryPos_ = targetPos;
            lastPivotLiftDistance_ = currentDistance;

            // Estimate where camera sits horizontally and ensure enough terrain clearance.
            glm::vec3 probeCam = targetPos + (-f.forward3D) * currentDistance;
            auto terrainAtCam = terrainManager->getHeightAt(probeCam.x, probeCam.y);
            auto terrainAtPivot = terrainManager->getHeightAt(targetPos.x, targetPos.y);

            float desiredLift = 0.0f;
            if (terrainAtCam) {
                // Keep the camera high enough that its ray does not cut
                // through a hill behind the character.
                //
                // Measured at the camera, which is the thing that has to
                // clear the ground. It was measured at the pivot, against a
                // fixed two metres above the terrain - and an absolute height
                // undoes any attempt to place the pivot on the character, by
                // exactly the amount that attempt moved it. On flat ground a
                // gnome's pivot at 0.65 was lifted 1.35 and a human's at 1.60
                // was lifted 0.40, both landing on 2.00 above the feet. Every
                // race got the same pivot, every time, and which character was
                // being followed made no difference to it.
                //
                // That is why zooming in on a gnome put them at the bottom of
                // the frame: the camera was aiming at a point nearly twice
                // their height, on level ground, with no hill in sight.
                const float basePivotZ = targetPos.z + followedPivotHeight() + mountedOffset;
                // The camera sits behind the character along the look
                // direction, which carries the pitch with it.
                desiredLift = terrainPivotLift(basePivotZ, (-f.forward3D).z,
                                               currentDistance, *terrainAtCam);
            }
            // If character is already below local terrain sample, avoid lifting aggressively.
            if (terrainAtPivot && targetPos.z < *terrainAtPivot - 0.2f) {
                desiredLift = 0.0f;
            }
            // Said when it starts and when it stops, because a lift is the
            // camera being moved by the ground rather than by the player and
            // there was no way to tell the two apart from outside. This one
            // was applying on flat ground in every session ever played.
            const bool lifting = desiredLift > 0.01f;
            if (lifting != pivotLiftActive_) {
                pivotLiftActive_ = lifting;
                LOG_WARNING("Camera pivot: terrain lift ",
                            lifting ? "on, raising the pivot by " : "off, was ",
                            desiredLift, " (pivot ", followedPivotHeight(),
                            " above the feet, ground at the camera ",
                            terrainAtCam ? *terrainAtCam - targetPos.z : 0.0f,
                            " relative to them)");
            }
            cachedPivotLift_ = desiredLift;
        }
        pivotLift = cachedPivotLift_;
    } else if (cachedInsideInteriorWMO) {
        // Inside WMO volumes (including tunnel/cave shells): terrain-above samples
        // are not relevant for camera pivoting.
        cachedPivotLift_ = 0.0f;
    }
    glm::vec3 pivot = targetPos + glm::vec3(0.0f, 0.0f, followedPivotHeight() + mountedOffset + pivotLift);

    // Camera direction from yaw/pitch (already computed as forward3D)
    glm::vec3 camDir = -f.forward3D;  // Camera looks at pivot, so it's behind

    // Zoom travels linearly at cameraDistanceMoveSpeed (stock FUN_006000e0).
    stepZoomTowardTarget(deltaTime);

    // Keep the active WMO group current for the collision and floor queries.
    // Throttle: only recheck every 10 frames or when position changes >2 units.
    //
    // No indoor zoom cap: the stock camera has a single maximum and lets the
    // collision pull-in deal with walls (see MAX_DISTANCE_NORMAL).
    if (wmoRenderer) {
        glm::vec3 wmoCheckDelta = targetPos - lastInsideWMOCheckPos;
        float distFromLastCheckSq = glm::dot(wmoCheckDelta, wmoCheckDelta);
        if (++insideWMOCheckCounter >= 10 || distFromLastCheckSq > 4.0f) {
            wmoRenderer->updateActiveGroup(targetPos.x, targetPos.y, targetPos.z + 1.0f);
            insideWMOCheckCounter = 0;
            lastInsideWMOCheckPos = targetPos;
        }
    }

    // ===== Camera collision: the stock CGCamera pull-in =====
    // Wow.exe 12340. FUN_00605d60 casts from the pivot back along the view
    // (world intersect FUN_0077f310, flags 0x100171, | 0x20000 for water with
    // cameraWaterCollision on) and refines the hit with a near-plane test
    // (FUN_006059e0), backing off 1/9 yd (0xa1e354). Then FUN_00606f90:
    //
    //   if (dist(+0x118) - hit > 1/9) {          // 0xa1e354
    //       dist = hit + 0.111112;               // 0xa1ea80
    //       start smoothing dist toward the zoom (+0x1e8) over 2.0 s  // 0xa4040c
    //   }
    //   camera at min(hit, dist)
    //
    // and FUN_00603d30 eases dist back out with the cosine curve
    // (FUN_008ca080). So the pull-in is instant, the recovery takes two
    // seconds and starts slowly, and the zoom the player chose is never
    // changed by a wall. Here the near-plane margin is CAM_SPHERE_RADIUS +
    // CAM_EPSILON off the WMO hit, and the terrain is the heightfield march.
    collisionDistance = currentDistance;

    if ((wmoRenderer || terrainManager) && currentDistance > MIN_DISTANCE) {
        float hitLimit = currentDistance;
        if (wmoRenderer) {
            const float hit = wmoRenderer->raycastBoundingBoxes(pivot, camDir, currentDistance);
            // hit == currentDistance means no hit (returns maxDistance on miss)
            if (hit < currentDistance) {
                hitLimit = std::max(MIN_DISTANCE, hit - CAM_SPHERE_RADIUS - CAM_EPSILON);
            }
        }
        if (!cachedInsideInteriorWMO) {
            hitLimit = std::min(hitLimit,
                                raymarchTerrainCameraLimit(pivot, camDir, currentDistance));
        }

        // Advance the recovery ease (FUN_00603d30's +0x118 slot). It ends when
        // the time is up or the zoom has come in to meet it.
        if (collisionShownDist_ >= 0.0f) {
            collisionEaseElapsed_ += deltaTime;
            const float t = collisionEaseElapsed_ / kCollisionRecoverSeconds;
            if (t >= 1.0f || currentDistance <= collisionShownDist_) {
                collisionShownDist_ = -1.0f;
            } else {
                const float k = (1.0f - std::cos(t * 3.14159265f)) * 0.5f;
                collisionShownDist_ = collisionEaseFrom_ + (currentDistance - collisionEaseFrom_) * k;
            }
        }
        const float allowed = (collisionShownDist_ >= 0.0f) ? collisionShownDist_ : currentDistance;
        if (allowed - hitLimit > kCollisionSnap) {
            collisionShownDist_ = hitLimit + kCollisionSnapOffset;
            collisionEaseFrom_ = collisionShownDist_;
            collisionEaseElapsed_ = 0.0f;
        }
        collisionDistance = std::min(hitLimit, allowed);
    } else {
        collisionShownDist_ = -1.0f;   // No collision sources: nothing to pull in
    }

    // Camera collision: terrain-only floor clamping
    auto getTerrainFloorAt = [&](float x, float y) -> std::optional<float> {
        if (terrainManager) {
            return terrainManager->getHeightAt(x, y);
        }
        return std::nullopt;
    };

    // Use collision distance (don't exceed user target)
    float actualDist = std::min(currentDistance, collisionDistance);

    // Compute actual camera position
    glm::vec3 actualCam;
    if (actualDist < MIN_DISTANCE + 0.1f) {
        // First-person: position camera at pivot (player's eyes)
        actualCam = pivot + f.forward3D * 0.1f;  // Slightly forward to not clip head
    } else {
        actualCam = pivot + camDir * actualDist;
    }

    // Prefer a view clearly above the water or clearly below it.
    //
    // Sitting exactly on the surface is the one place the water has nothing
    // sensible to draw: the near plane cuts it, and what is left is a seam
    // across the middle of the view. Lean out of a band either side - whichever
    // side the camera is already nearer - and let it settle there.
    //
    // Applied to the target, not to the smoothed position. Nudging the smoothed
    // position after the fact fought the follow, which re-aims at this target
    // every frame: the two pulled opposite ways and the camera hovered on the
    // waterline crossing it several times a second, which is exactly the state
    // this is meant to avoid. Moving the target instead lets the existing
    // smoothing carry it, and it stays where it is put.
    //
    // The band is the near plane's half-height, because that is the depth range
    // over which the near plane can be half in the water. Lifting eases the
    // camera back a little and dropping draws it in, so the character stays
    // framed rather than sliding down the screen.
    if (waterRenderer && actualDist >= MIN_DISTANCE + 0.1f) {
        const float band = std::max(0.05f, camera->getNearPlane()
                                           * std::tan(glm::radians(camera->getFovDegrees()) * 0.5f));
        auto surfaceZ = waterRenderer->getNearestWaterHeightAt(
            actualCam.x, actualCam.y, actualCam.z, 30.0f);
        // Clear of the band, not resting on its edge.
        //
        // Parking exactly at the edge puts the camera on the same threshold the
        // underwater overlay switches at, so the smallest movement flipped it on
        // and off and the view changed brightness with it. Overshooting settles
        // the camera on one side of that decision.
        float wantOffset = 0.0f;
        if (surfaceZ) {
            const float above = actualCam.z - *surfaceZ;
            if (std::abs(above) < band) {
                const float clear = band * 1.35f;
                wantOffset = ((above >= 0.0f) ? (*surfaceZ + clear) : (*surfaceZ - clear))
                           - actualCam.z;
            }
        }
        // Eased, and eased back to nothing when there is no water under the
        // camera at all. Applying it the instant the query finds a surface, and
        // dropping it the instant it does not, made the camera jump at the
        // water's edge - which is where a player crossing in and out of a lake
        // spends their time.
        const float ease = 1.0f - std::exp(-deltaTime / 0.18f);
        waterNudgeZ_ += (wantOffset - waterNudgeZ_) * ease;
        if (std::abs(waterNudgeZ_) > 1e-3f) {
            actualCam.z += waterNudgeZ_;
            glm::vec2 outward(actualCam.x - pivot.x, actualCam.y - pivot.y);
            const float outLen = glm::length(outward);
            if (outLen > 1e-3f) {
                outward /= outLen;
                actualCam.x += outward.x * waterNudgeZ_ * 0.6f;
                actualCam.y += outward.y * waterNudgeZ_ * 0.6f;
            }
        }
    } else {
        waterNudgeZ_ = 0.0f;
    }

    // Smooth camera position to avoid jitter
    if (glm::dot(smoothedCamPos, smoothedCamPos) < 1e-4f) {
        smoothedCamPos = actualCam;  // Initialize
    }
    bool activelyRotating = mouseButtonDown || f.nowTurnLeft || f.nowTurnRight;
    float camLerp = (activelyRotating && !smoothCameraFollow_)
        ? 1.0f : (1.0f - std::exp(-camSmoothSpeed_ * deltaTime));
    smoothedCamPos += (actualCam - smoothedCamPos) * camLerp;

    // ===== Final floor clearance check =====
    // Use WMO-aware floor so the camera doesn't pop above tunnels/caves.
    constexpr float MIN_FLOOR_CLEARANCE = 0.35f;
    if (!cachedInsideWMO) {
        std::optional<float> camTerrainH;
        if (!cachedInsideInteriorWMO) {
            camTerrainH = getTerrainFloorAt(smoothedCamPos.x, smoothedCamPos.y);
        }
        std::optional<float> camWmoH;
        if (wmoRenderer) {
            // Skip expensive WMO floor query if camera barely moved
            float cdx = smoothedCamPos.x - lastCamFloorQueryPos.x;
            float cdy = smoothedCamPos.y - lastCamFloorQueryPos.y;
            float camDeltaSq = cdx * cdx + cdy * cdy;
            if (camDeltaSq < 0.09f && hasCachedCamFloor) {
                camWmoH = cachedCamWmoFloor;
            } else {
                float camFloorProbeZ = smoothedCamPos.z;
                if (cachedInsideInteriorWMO) {
                    // Inside tunnels/buildings, probe near player height so roof
                    // triangles above the camera don't get treated as floor.
                    camFloorProbeZ = std::min(smoothedCamPos.z, targetPos.z + 1.0f);
                }
                camWmoH = wmoRenderer->getFloorHeight(
                    smoothedCamPos.x, smoothedCamPos.y, camFloorProbeZ);

                if (cachedInsideInteriorWMO && camWmoH) {
                    // Never let camera floor clamp latch to tunnel ceilings / upper decks.
                    float maxValidIndoorFloor = targetPos.z + 0.9f;
                    if (*camWmoH > maxValidIndoorFloor) {
                        camWmoH = std::nullopt;
                    }
                }
                cachedCamWmoFloor = camWmoH;
                hasCachedCamFloor = true;
                lastCamFloorQueryPos = smoothedCamPos;
            }
        }
        // When camera/character are inside a WMO, force WMO floor usage for camera
        // clearance to avoid snapping toward terrain above enclosed tunnels/caves.
        std::optional<float> camFloorH;
        if (cachedInsideWMO && camWmoH && camTerrainH) {
            // Transition seam: avoid terrain-above clamp near tunnel entrances.
            float camDropFromPlayer = targetPos.z - *camWmoH;
            if ((*camTerrainH - *camWmoH) > 1.2f &&
                (*camTerrainH - *camWmoH) < 8.0f &&
                camDropFromPlayer >= -0.4f &&
                camDropFromPlayer < 1.8f) {
                camFloorH = camWmoH;
            } else {
                camFloorH = selectClosestFloor(camTerrainH, camWmoH, smoothedCamPos.z);
            }
        } else {
            camFloorH = selectReachableFloor(
                camTerrainH, camWmoH, smoothedCamPos.z, 0.5f);
        }
        // Terrain has no underside. The reachability filter above is for
        // structure floors, where something half a yard over the camera may be
        // a deck it is under; applied to the heightfield it meant that a
        // camera which had sunk more than half a yard into a hill - coming in
        // from a taxi, or on a sharp rise - was left there, the ground now
        // counting as a ceiling. While the character stands on or above the
        // terrain, the camera belongs above it too.
        if (!cachedInsideInteriorWMO) {
            auto groundH = cameraTerrainFloor(smoothedCamPos.x, smoothedCamPos.y, targetPos);
            if (groundH && (!camFloorH || *groundH > *camFloorH)) camFloorH = groundH;
        }
        if (camFloorH && smoothedCamPos.z < *camFloorH + MIN_FLOOR_CLEARANCE) {
            smoothedCamPos.z = *camFloorH + MIN_FLOOR_CLEARANCE;
        }
    }
    // Never let camera sink below the character's feet plane.
    smoothedCamPos.z = std::max(smoothedCamPos.z, targetPos.z + 0.15f);

    camera->setPosition(smoothedCamPos);

    // Hide player model when in first-person (camera too close)
    // WoW fades between ~1.0m and ~0.5m, hides fully below 0.5m
    // For now, just hide below first-person threshold
    if (characterRenderer && playerInstanceId > 0) {
        // How tall whoever is being followed actually is, so the pivot lands
        // on them rather than at a fixed distance above their feet. Measured
        // here because it changes with the model: a race change, a mount, a
        // shapeshift. Kept if it ever fails to measure, so a frame that could
        // not answer does not snap the camera.
        float height = 0.0f;
        if (characterRenderer->getInstanceHeight(playerInstanceId, height) && height > 0.01f) {
            // The head bone, which is what the pivot is placed against. A
            // skeleton that does not name one leaves this zero and falls back
            // to the height.
            //
            // A frame that cannot answer keeps the last answer. The height
            // above is already guarded that way by the enclosing `if`; the
            // head bone was not, and zeroing it dropped the pivot to a lower
            // tier of pivotHeightFor - a gnome went 0.65 to 0.88, or to the
            // flat 1.6 when the height was missing too. Entering a building is
            // where this showed: the WMO transition re-registers the instance
            // and the bone query returns false for a frame, so the camera rose
            // on the doorstep and stayed up.
            float headZ = 0.0f;
            const bool measuredHead =
                characterRenderer->getInstanceKeyBonePivotZ(playerInstanceId, kKeyBoneHead, headZ)
                && headZ > 0.01f;
            if (!measuredHead) headZ = followedHeadZ_;
            if (std::abs(height - followedHeight_) > 0.01f ||
                std::abs(headZ - followedHeadZ_) > 0.01f) {
                // Said when it changes, which is when a character is first
                // drawn and whenever its model does. These numbers settle
                // whether the camera is pivoting where it should, and none of
                // them was knowable from outside: a report of "still too high"
                // can mean nothing was measured, or that it was and the
                // proportion is wrong, and they tell the two apart.
                LOG_WARNING("Camera pivot: character is ", height,
                            " tall with its head bone at ", headZ,
                            ", pivoting at ", pivotHeightFor(pivotHeight_, height, headZ),
                            " (setting ", pivotHeight_, " against a ",
                            kPivotReferenceHeadZ, " head reference)");
            }
            followedHeight_ = height;
            if (measuredHead) followedHeadZ_ = headZ;
        }

        // Hide only on first-person *intent* (the user's zoom), not on a
        // collision-squeezed distance. The `actualDist < MIN_DISTANCE + 0.1`
        // term fired whenever geometry pushed the camera close in third
        // person - under Undercity's overhangs, or backing into a wall -
        // but the renderer's visibility hardening forces the player visible
        // again in third person on the very same frame. So the two wrote
        // opposite values every frame, toggling the instance and its weapon
        // attachments on and off (visible in the log as a rapid
        // setInstanceVisible flip) while what actually drew never changed.
        // isFirstPersonView already covers the anti-clip-pushback case the
        // extra term was meant for, since it reads the target distance, not
        // the squeezed one.
        bool shouldHidePlayer = isFirstPersonView();
        characterRenderer->setInstanceVisible(playerInstanceId, !shouldHidePlayer);

        // Note: the Renderer's CharAnimState machine drives player character animations
        // (Run, Walk, Jump, Swim, etc.) - no additional animation driving needed here.
    }
}

void CameraController::updateThirdPersonCamera(float deltaTime, FrameInput& f) {
    glm::vec3 prevTargetPos(0.0f);
    glm::vec3 targetPos = moveFollowedCharacter(deltaTime, f, prevTargetPos);
    groundFollowedCharacter(deltaTime, f, targetPos, prevTargetPos);
    // Landing: flying down onto real ground, or skimming it, without climbing.
    // Grounding has just put the feet on the floor if they were within reach
    // of it; on a floor that was actually found, that is touching down.
    if (isFlightAirborne() && grounded && hasRealGround_ && !f.spaceHeld &&
        std::abs(targetPos.z - lastGroundZ) < 0.3f) {
        flightAirborne_ = false;
        LOG_WARNING("Flight: landed");
    }
    // The picture eases after a grounded step; physics stays where it is.
    // Nothing to ease where something else owns the height.
    if (swimming || externalFollow_ || isFlightAirborne() || hoverActive_ || sitting ||
        !std::isfinite(renderZOffset_)) {
        renderZOffset_ = 0.0f;
    } else {
        constexpr float kMaxRenderZLag = movement::kMaxStepUp + 0.25f;
        renderZOffset_ = std::clamp(renderZOffset_, -kMaxRenderZLag, kMaxRenderZLag);
        renderZOffset_ *= std::exp(-std::max(deltaTime, 0.0f) / RENDER_Z_SMOOTH_TAU);
        if (std::abs(renderZOffset_) < 1e-3f) renderZOffset_ = 0.0f;
    }
    updateOrbitCamera(deltaTime, f, targetPos + glm::vec3(0.0f, 0.0f, renderZOffset_));
}

// The camera that flies itself, used when nothing is being followed.
void CameraController::updateFreeFlyCamera(float /*deltaTime*/, FrameInput& f) {
    // Free-fly camera mode (original behavior)
    glm::vec3 newPos = camera->getPosition();
    if (wmoRenderer) {
        wmoRenderer->setCollisionFocus(newPos, COLLISION_FOCUS_RADIUS_FREE_FLY);
    }
    if (m2Renderer) {
        m2Renderer->setCollisionFocus(newPos, COLLISION_FOCUS_RADIUS_FREE_FLY);
    }
    float feetZ = newPos.z - eyeHeight;

    // Check for water at feet position
    std::optional<float> waterH;
    if (waterRenderer) {
        waterH = waterRenderer->getWaterHeightAt(newPos.x, newPos.y);
    }
    constexpr float MAX_SWIM_DEPTH_FROM_SURFACE = 12.0f;
    bool inWater = false;
    if (waterH && feetZ < *waterH) {
        std::optional<uint16_t> waterType;
        if (waterRenderer) {
            waterType = waterRenderer->getWaterTypeAt(newPos.x, newPos.y);
        }
        bool isOcean = false;
        if (waterType && *waterType != 0) {
            isOcean = (((*waterType - 1) % 4) == 1);
        }
        bool depthAllowed = isOcean || ((*waterH - feetZ) <= MAX_SWIM_DEPTH_FROM_SURFACE);
        if (!depthAllowed) {
            inWater = false;
        } else {
        // The stock rule, as the followed character's (0x730D10): the liquid
        // over the feet against the collision height.
        const float depthFromFeet = *waterH - feetZ;
        inWater = swimming
            ? game::stock_movement::keepsSwimming(depthFromFeet, collider_.height, true, true)
            : game::stock_movement::startsSwimming(depthFromFeet, collider_.height, true,
                                                   !grounded && verticalVelocity > 0.0f);
        }
    }


    if (inWater) {
        swimming = true;
        float swimSpeed = useWoWSpeed ? stockSpeeds().swim
            : ((swimSpeedOverride_ > 0.0f && swimSpeedOverride_ < 100.0f && !std::isnan(swimSpeedOverride_))
                   ? swimSpeedOverride_ : f.speed * SWIM_SPEED_FACTOR);
        float waterSurfaceCamZ = waterH ? (*waterH - swimFloatDepth() + eyeHeight) : newPos.z;
        bool diveIntent = f.nowForward && (f.forward3D.z < -0.28f);

        float movLenSq = glm::dot(f.movement, f.movement);
        if (movLenSq > 1e-6f) {
            f.movement *= glm::inversesqrt(movLenSq);
            newPos += f.movement * swimSpeed * f.physicsDeltaTime;
        }

        if (f.nowJump) {
            verticalVelocity = SWIM_BUOYANCY;
        } else {
            verticalVelocity += SWIM_GRAVITY * f.physicsDeltaTime;
            if (verticalVelocity < SWIM_SINK_SPEED) {
                verticalVelocity = SWIM_SINK_SPEED;
            }
            if (!diveIntent) {
                float surfaceErr = (waterSurfaceCamZ - newPos.z);
                verticalVelocity += surfaceErr * 7.0f * f.physicsDeltaTime;
                verticalVelocity *= std::max(0.0f, 1.0f - 3.2f * f.physicsDeltaTime);
                if (std::abs(surfaceErr) < 0.06f && std::abs(verticalVelocity) < 0.35f) {
                    verticalVelocity = 0.0f;
                }
            }
        }

        newPos.z += verticalVelocity * f.physicsDeltaTime;

        // Don't rise above water surface (feet at water level)
        if (waterH && (newPos.z - eyeHeight) > *waterH - swimFloatDepth()) {
            newPos.z = *waterH - swimFloatDepth() + eyeHeight;
            if (verticalVelocity > 0.0f) verticalVelocity = 0.0f;
        }

        grounded = false;
    } else {
        swimming = false;

        float movLenSq2 = glm::dot(f.movement, f.movement);
        if (movLenSq2 > 1e-6f) {
            f.movement *= glm::inversesqrt(movLenSq2);
            newPos += f.movement * f.speed * f.physicsDeltaTime;
        }

        // Jump with input buffering and coyote time
        if (f.nowJump) jumpBufferTimer = JUMP_BUFFER_TIME;
        if (grounded) coyoteTimer = COYOTE_TIME;

        if ((grounded || coyoteTimer > 0.0f) && jumpBufferTimer > 0.0f && !mounted_) {
            verticalVelocity = f.jumpVel;
            grounded = false;
            jumpBufferTimer = 0.0f;
            coyoteTimer = 0.0f;
        }

        jumpBufferTimer -= f.physicsDeltaTime;
        coyoteTimer -= f.physicsDeltaTime;

        // Apply f.gravity, unless the ground under the character has not
        // streamed in yet. See the note at the other gravity site.
        if (groundNotStreamedYet(newPos.x, newPos.y)) {
            verticalVelocity = 0.0f;
        } else {
            verticalVelocity += f.gravity * f.physicsDeltaTime;
            newPos.z += verticalVelocity * f.physicsDeltaTime;
        }
    }

    // Wall sweep collision before grounding (skip when stationary).
    {
        // Swept at the feet, not the eyes. Doodads are left out here, which
        // is the one thing this camera does differently.
        const glm::vec3 eyeOffset(0, 0, eyeHeight);
        newPos = sweepAgainstWalls(camera->getPosition() - eyeOffset,
                                   newPos - eyeOffset, false) + eyeOffset;
    }

    // Ground to terrain or WMO floor
    {
        auto sampleGround = [&](float x, float y) -> std::optional<float> {
            std::optional<float> terrainH;
            std::optional<float> wmoH;
            std::optional<float> m2H;
            if (terrainManager) {
                terrainH = terrainManager->getHeightAt(x, y);
            }
            float feetZ = newPos.z - eyeHeight;
            float wmoProbeZ = std::max(feetZ, lastGroundZ) + 1.5f;
            float m2ProbeZ = std::max(feetZ, lastGroundZ) + 6.0f;
            if (wmoRenderer) {
                wmoH = wmoRenderer->getFloorHeight(x, y, wmoProbeZ);
            }
            if (m2Renderer && !externalFollow_) {
                m2H = m2Renderer->getFloorHeight(x, y, m2ProbeZ);
            }
            auto base = selectReachableFloor(terrainH, wmoH, feetZ, 1.0f);
            if (m2H && *m2H <= feetZ + 1.0f && (!base || *m2H > *base)) {
                base = m2H;
            }
            return base;
        };

        // Single center probe.
        std::optional<float> groundH = sampleGround(newPos.x, newPos.y);

        if (groundH) {
            float feetZ = newPos.z - eyeHeight;
            float stepUp = 1.0f;
            float fallCatch = 3.0f;
            float dz = *groundH - feetZ;

            // Only snap when:
            // 1. Near ground (within step-up range above) - handles walking
            // 2. Actually falling from height (was airborne + falling fast)
            // 3. Was grounded + ground is close (grace for slopes)
            bool nearGround = (dz >= 0.0f && dz <= stepUp);
            // Only at the surface; this caught a falling player up to three
            // yards above the ground and dropped them onto it.
            bool airFalling = (!grounded && verticalVelocity < -5.0f && dz >= -0.05f);
            bool slopeGrace = (grounded && dz >= -1.0f && dz <= stepUp * 2.0f);

            if (dz >= -fallCatch && (nearGround || airFalling || slopeGrace)) {
                newPos.z = *groundH + eyeHeight;
                verticalVelocity = 0.0f;
                grounded = true;
                lastGroundZ = *groundH;
                swimming = false;
            } else if (!swimming) {
                grounded = false;
                lastGroundZ = *groundH;
            }
        } else if (!swimming) {
            newPos.z = lastGroundZ + eyeHeight;
            verticalVelocity = 0.0f;
            grounded = true;
        }
    }

    camera->setPosition(newPos);
}

void CameraController::update(float deltaTime) {
    if (!enabled || !camera) {
        return;
    }
    // Keep physics integration stable during render hitches to avoid floor tunneling.
    const float physicsDeltaTime = std::min(deltaTime, kMaxPhysicsDelta);
    intoxicationTime_ += deltaTime;

    // During taxi flights, skip movement logic but keep camera orbit/zoom controls.
    if (externalFollow_) {
        // Cancel any active intro/idle orbit so mouse panning works during taxi.
        // The intro handling code (below) is unreachable during externalFollow_.
        introActive = false;
        idleOrbit_ = false;
        idleTimer_ = 0.0f;

        camera->setRotation(yaw, pitch);
        stepZoomTowardTarget(deltaTime);
        collisionDistance = currentDistance;
        collisionShownDist_ = -1.0f;

        // Position camera behind character during taxi
        if (thirdPerson && followTarget) {
            glm::vec3 targetPos = *followTarget;
            glm::vec3 forward3D = camera->getForward();

            // Pivot point at upper chest/neck
            float mountedOffset = mounted_ ? mountHeightOffset_ : 0.0f;
            glm::vec3 pivot = targetPos + glm::vec3(0.0f, 0.0f, followedPivotHeight() + mountedOffset);

            // Camera direction from yaw/pitch
            glm::vec3 camDir = -forward3D;

            // Use current distance
            float actualDist = std::min(currentDistance, collisionDistance);

            // Compute camera position
            glm::vec3 actualCam;
            // Small offset prevents the camera from clipping into the character
            // model when collision pushes it to near-minimum distance.
            constexpr float kCameraClipEpsilon = 0.1f;
            if (actualDist < MIN_DISTANCE + kCameraClipEpsilon) {
                actualCam = pivot + forward3D * kCameraClipEpsilon;
            } else {
                actualCam = pivot + camDir * actualDist;
            }

            // Smooth camera position (1:1 while actively dragging, see main update()).
            if (glm::dot(smoothedCamPos, smoothedCamPos) < 1e-4f) {
                smoothedCamPos = actualCam;
            }
            float camLerp = (mouseButtonDown && !smoothCameraFollow_)
                ? 1.0f : (1.0f - std::exp(-camSmoothSpeed_ * deltaTime));
            smoothedCamPos += (actualCam - smoothedCamPos) * camLerp;

            // A flight path skims ridges and lands on hillsides.
            if (auto groundH = cameraTerrainFloor(smoothedCamPos.x, smoothedCamPos.y, targetPos)) {
                constexpr float kTaxiFloorClearance = 0.35f;
                smoothedCamPos.z = std::max(smoothedCamPos.z, *groundH + kTaxiFloorClearance);
            }

            camera->setPosition(smoothedCamPos);
        }

        // No collision pull-in is carried across a flight (collisionShownDist_
        // was cleared above), so the landing starts from the ground as it is.
        return;
    }

    auto& input = core::Input::getInstance();

    // Don't process keyboard input when UI text input (e.g. chat box) has focus.
    // Both interfaces are asked: the flag below is ImGui's own and says nothing
    // about a chat box FrameXML draws, so on its own it let the character walk
    // away while someone was typing.
    bool uiWantsKeyboard = ImGui::GetIO().WantTextInput ||
                           ui::interfaceTakingTypedInput();

    // Suppress movement input after teleport/portal (keys may still be held)
    if (movementSuppressTimer_ > 0.0f) {
        movementSuppressTimer_ -= deltaTime;
    }
    bool movementSuppressed = movementSuppressTimer_ > 0.0f;

    auto& bindings = ui::KeybindingManager::getInstance();
    if (bindings.isCommandPressed("TOGGLERUN")) walking_ = !walking_;
    if (bindings.isCommandPressed("CAMERAZOOMIN")) processMouseWheel(1.0f);
    if (bindings.isCommandPressed("CAMERAZOOMOUT")) processMouseWheel(-1.0f);
    if (bindings.isCommandPressed("WOWEERESETCAMERA")) resetAngles();

    // Determine current key states
    // Arrow keys mirror WASD the way the original client maps them:
    // Up/Down move, Left/Right turn (strafe stays on Q/E).
    bool keyW = !uiWantsKeyboard && !sitting && !movementSuppressed && bindings.isCommandDown("MOVEFORWARD");
    bool keyS = !uiWantsKeyboard && !sitting && !movementSuppressed && bindings.isCommandDown("MOVEBACKWARD");
    bool keyA = !uiWantsKeyboard && !sitting && !movementSuppressed && bindings.isCommandDown("TURNLEFT");
    bool keyD = !uiWantsKeyboard && !sitting && !movementSuppressed && bindings.isCommandDown("TURNRIGHT");
    bool keyQ = !uiWantsKeyboard && !sitting && !movementSuppressed && bindings.isCommandDown("STRAFELEFT");
    bool keyE = !uiWantsKeyboard && !sitting && !movementSuppressed && bindings.isCommandDown("STRAFERIGHT");
    bool shiftDown = !uiWantsKeyboard && (input.isKeyPressed(SDL_SCANCODE_LSHIFT) || input.isKeyPressed(SDL_SCANCODE_RSHIFT));
    bool ctrlDown = !uiWantsKeyboard && (input.isKeyPressed(SDL_SCANCODE_LCTRL) || input.isKeyPressed(SDL_SCANCODE_RCTRL));
    // Ctrl temporarily forces walking without changing the bindable toggle.
    const bool walking = walking_ || ctrlDown;
    bool nowJump = !uiWantsKeyboard && !sitting && !movementSuppressed && bindings.isCommandPressed("JUMP");
    // Swimming and flying need the held state, not the press edge: on land
    // space is a one-shot jump, but in water and in the air it is continuous
    // ascent, and an edge gave a single impulse that then bled away - or, in
    // flight, a single frame of climb per press.
    bool spaceHeld = !uiWantsKeyboard && !sitting && !movementSuppressed && bindings.isCommandDown("JUMP");

    // Idle camera: any input resets the timer; timeout triggers a slow orbit pan
    bool anyInput = leftMouseDown || rightMouseDown || keyW || keyS || keyA || keyD || keyQ || keyE || nowJump;
    if (anyInput) {
        idleTimer_ = 0.0f;
    } else if (!introActive && idleOrbitEnabled_) {
        idleTimer_ += deltaTime;
        if (idleTimer_ >= IDLE_TIMEOUT) {
            idleTimer_ = 0.0f;
            startIntroPan(30.0f, 360.0f); // Slow casual orbit over 30 seconds
            idleOrbit_ = true;
        }
    } else if (!idleOrbitEnabled_) {
        idleTimer_ = 0.0f;
    }

    if (introActive) {
        if (anyInput) {
            introActive = false;
            idleOrbit_ = false;
            idleTimer_ = 0.0f;
        } else {
            introTimer += deltaTime;
            if (idleOrbit_) {
                // Continuous smooth rotation - no lerp endpoint, just constant angular velocity
                float degreesPerSec = introOrbitDegrees / introDuration;
                yaw -= degreesPerSec * deltaTime;
                camera->setRotation(yaw, pitch);
                facingYaw = yaw;
            } else {
                float t = (introDuration > 0.0f) ? std::min(introTimer / introDuration, 1.0f) : 1.0f;
                yaw = introStartYaw + (introEndYaw - introStartYaw) * t;
                pitch = introStartPitch + (introEndPitch - introStartPitch) * t;
                currentDistance = introStartDistance + (introEndDistance - introStartDistance) * t;
                userTargetDistance = introEndDistance;
                camera->setRotation(yaw, pitch);
                facingYaw = yaw;
                if (t >= 1.0f) {
                    introActive = false;
                }
            }
        }
        // Suppress player movement/input during intro.
        keyW = keyS = keyA = keyD = keyQ = keyE = nowJump = spaceHeld = false;
    }

    // The same action the keybinding panel rebinds; forward/backward cancels it.
    if (!uiWantsKeyboard && ui::KeybindingManager::getInstance().isActionPressed(
            ui::KeybindingManager::Action::TOGGLE_AUTORUN)) {
        autoRunning = !autoRunning;
    }
    // Helper: cancel auto-follow and notify game handler
    auto doCancelAutoFollow = [&]() {
        if (autoFollowTarget_) {
            autoFollowTarget_ = nullptr;
            if (autoFollowCancelCallback_) autoFollowCancelCallback_();
        }
    };

    // Pressing forward or backward ends autorun (CGInputControl 0x5FA170
    // clears 0x1000 on the key going down, not while it is held, so autorun
    // can still be toggled on with forward held).
    const bool movePressEdge = !uiWantsKeyboard && !sitting && !movementSuppressed &&
        (bindings.isCommandPressed("MOVEFORWARD") || bindings.isCommandPressed("MOVEBACKWARD"));
    if (movePressEdge) {
        autoRunning = false;
        doCancelAutoFollow();
    }
    if (keyW || keyS) {
        doCancelAutoFollow();
    }

    bool mouseAutorun = !uiWantsKeyboard && !sitting && leftMouseDown && rightMouseDown;
    if (mouseAutorun) {
        autoRunning = false;
        doCancelAutoFollow();
    }

    // Auto-follow: face target and run toward them when within range
    bool autoFollowMove = false;
    if (autoFollowTarget_ && followTarget && !movementRooted_) {
        glm::vec3 myPos = *followTarget;
        glm::vec3 tgtPos = *autoFollowTarget_;
        float dx = tgtPos.x - myPos.x;
        float dy = tgtPos.y - myPos.y;
        float distSq2D = dx * dx + dy * dy;

        if (distSq2D > FOLLOW_MAX_DIST * FOLLOW_MAX_DIST) {
            doCancelAutoFollow();
        } else if (distSq2D > FOLLOW_STOP_DIST * FOLLOW_STOP_DIST) {
            // Face target (render-space yaw: atan2(-dx, -dy) -> degrees)
            float targetYawRad = std::atan2(-dx, -dy);
            float targetYawDeg = targetYawRad * 180.0f / core::coords::PI;
            facingYaw = targetYawDeg;
            yaw = targetYawDeg;
            autoFollowMove = true;
        }
        // else: within stop distance, stay put

        // Cancel on strafe/turn keys
        if (keyA || keyD || keyQ || keyE) {
            doCancelAutoFollow();
            autoFollowMove = false;
        }
    }

    // When the server has rooted the player, suppress all horizontal movement input.
    const bool movBlocked = movementRooted_;
    // Auto-follow uses run speed (same as auto-run), not walk speed
    if (autoFollowMove) autoRunning = true;
    // The stock votes (CGInputControl 0x5FAE70 / 0x5FAFB0 / 0x5FB0B0): forward,
    // autorun and both mouse buttons count one way and backward the other, so
    // forward and backward held together stand still - and send nothing -
    // rather than setting both flags; the same for the two strafes and the
    // two turns. With the right button steering the turn keys strafe.
    // Turning is allowed even while rooted; only positional movement is blocked.
    namespace sm = game::stock_movement;
    const bool rightButtonSteering = !thirdPerson || rightMouseDown;
    const int forwardVote = sm::resolveForwardBack(keyW, keyS, autoRunning, mouseAutorun);
    const int strafeVote = sm::resolveStrafe(keyQ, keyE, keyA, keyD, rightButtonSteering);
    const int turnVote = sm::resolveTurn(keyA, keyD, rightButtonSteering);
    bool nowForward = !movBlocked && forwardVote > 0;
    bool nowBackward = !movBlocked && forwardVote < 0;
    bool nowStrafeLeft = !movBlocked && strafeVote > 0;
    bool nowStrafeRight = !movBlocked && strafeVote < 0;
    bool nowTurnLeft = turnVote > 0;
    bool nowTurnRight = turnVote < 0;

    // Mid-air the stock client keeps the movement it left the ground with:
    // a standing jump lands where it started whatever is pressed, a running
    // jump carries on if the key is released, and only the facing can change.
    // The keys are latched on the first airborne frame and released on the
    // ground. Swimming and flight steer freely and are not latched.
    //
    // One exception (0x988A20 / 0x988B00): a fall that started with no
    // direction at all takes the first direction pressed in the air, at walk
    // speed, and keeps that one. airSpeed_ below says which case this is.
    bool airLatchedNow = false;   // the latch was (re)taken this frame
    bool airDriftStart = false;   // ...from a standing fall, at walk speed
    if (!grounded && !swimming && !isFlightAirborne()) {
        const bool anyKeyDir = nowForward || nowBackward || nowStrafeLeft || nowStrafeRight;
        const bool latchedNoDir = airMoveLatched_ && !airForward_ && !airBackward_ &&
                                  !airStrafeLeft_ && !airStrafeRight_;
        if (!airMoveLatched_ || (latchedNoDir && anyKeyDir)) {
            airDriftStart = airMoveLatched_;
            airMoveLatched_ = true;
            airLatchedNow = true;
            airForward_ = nowForward;
            airBackward_ = nowBackward;
            airStrafeLeft_ = nowStrafeLeft;
            airStrafeRight_ = nowStrafeRight;
        }
        nowForward = airForward_;
        nowBackward = airBackward_;
        nowStrafeLeft = airStrafeLeft_;
        nowStrafeRight = airStrafeRight_;
    } else {
        airMoveLatched_ = false;
        airSpeed_ = 0.0f;
    }

    // Keyboard turning turns the character, and the camera with it.
    //
    // The stock camera keeps its yaw relative to the facing (CGCamera +0x11c,
    // with the facing added in FUN_00604490 while the lock count +0xac is 0),
    // so a turn key carries the camera round rigidly and an orbit offset the
    // player set survives the turn. Holding the left button locks the yaw to
    // the world instead (CGInputControl FUN_005fa170 -> FUN_006047e0 ->
    // FUN_00601ff0 -> FUN_006019b0 increments +0xac), so the character turns
    // under a camera that stays put. It used to set facingYaw = yaw, which
    // snapped the character round to wherever the camera had been orbited.
    // Use server turn rate (rad/s) when set; otherwise fall back to WOW_TURN_SPEED (deg/s).
    const float activeTurnSpeedDeg = (turnRateOverride_ > 0.0f && turnRateOverride_ < 20.0f
                                       && !std::isnan(turnRateOverride_))
                                         ? glm::degrees(turnRateOverride_)
                                         : WOW_TURN_SPEED;
    // Three quarters of the rate while moving, falling, ascending or
    // descending (0x987770: kMovingMask, unless flags2 FULL_SPEED_TURNING).
    uint32_t stockMoveFlags = 0;
    if (nowForward) stockMoveFlags |= sm::kForward;
    if (nowBackward) stockMoveFlags |= sm::kBackward;
    if (nowStrafeLeft) stockMoveFlags |= sm::kStrafeLeft;
    if (nowStrafeRight) stockMoveFlags |= sm::kStrafeRight;
    if (!grounded && !swimming && !isFlightAirborne()) stockMoveFlags |= sm::kFalling;
    if ((swimming || isFlightAirborne()) && spaceHeld) stockMoveFlags |= sm::kAscending;
    const float turnRateDeg = glm::degrees(
        sm::keyboardTurnRate(glm::radians(activeTurnSpeedDeg), stockMoveFlags));
    float turnDeltaDeg = 0.0f;
    if (nowTurnLeft && !nowTurnRight) {
        turnDeltaDeg = turnRateDeg * deltaTime;
    } else if (nowTurnRight && !nowTurnLeft) {
        turnDeltaDeg = -turnRateDeg * deltaTime;
    }
    if (turnDeltaDeg != 0.0f) {
        facingYaw += turnDeltaDeg;
        if (!leftMouseDown) yaw += turnDeltaDeg;
        camera->setRotation(yaw, pitch);
    }

    // Tick down gravity suspension timer (used after world entry to prevent
    // falling through WMO floors before collision is loaded)
    if (entryFloorAnchorTimer_ > 0.0f) entryFloorAnchorTimer_ -= deltaTime;
    if (serverPlacementAge_ < kServerPlacementWindow) serverPlacementAge_ += deltaTime;
    if (gravitySuspendTimer_ > 0.0f) {
        gravitySuspendTimer_ -= deltaTime;
    }

    // Select physics constants based on mode
    float gravity = useWoWSpeed ? WOW_GRAVITY : GRAVITY;
    float jumpVel = useWoWSpeed ? WOW_JUMP_VELOCITY : JUMP_VELOCITY;

    // Suspend gravity after world entry - hold Z position until timer expires
    // OR a floor is detected. This prevents falling through unloaded WMO floors.
    if (gravitySuspendTimer_ > 0.0f) {
        gravity = 0.0f;
        verticalVelocity = 0.0f;
    }

    // Calculate movement speed based on direction and modifiers
    float speed;
    if (useWoWSpeed) {
        // CMovement::GetSpeed (0x987570, stock_movement::selectSpeed): walking
        // is min(walk, run) whichever way the character goes - a walker backs
        // up at walk speed, not at the run-back speed - and backing up at a
        // run is min(runBack, run).
        uint32_t groundFlags = stockMoveFlags & (sm::kForward | sm::kBackward |
                                                 sm::kStrafeLeft | sm::kStrafeRight);
        if (walking) groundFlags |= sm::kWalking;
        speed = sm::selectSpeed(groundFlags, stockSpeeds());
    } else {
        // Exploration mode (original behavior)
        speed = movementSpeed;
        if (shiftDown) {
            speed *= sprintMultiplier;
        }
        if (ctrlDown) {
            speed *= slowMultiplier;
        }
    }

    bool hasMoveInput = nowForward || nowBackward || nowStrafeLeft || nowStrafeRight;
    if (useWoWSpeed) {
        // "Sprinting" flag drives run animation/stronger footstep set.
        // In WoW mode this means running pace (not walk/backpedal), not Shift.
        runPace = hasMoveInput && !walking && !nowBackward;
    } else {
        runPace = hasMoveInput && shiftDown;
    }

    // Swing the camera back behind a moving character (cameraSmoothStyle).
    // Before forward3D is read, so the frame is drawn with the eased angles.
    updateStockFollow(deltaTime, nowForward, nowBackward, nowStrafeLeft, nowStrafeRight,
                      nowTurnLeft, nowTurnRight,
                      rightMouseDown || mouseAutorun || steering_ || (swimming && hasMoveInput));

    // Get camera axes - project forward onto XY plane for walking
    glm::vec3 forward3D = camera->getForward();
    // In water the camera always steers, moving or turning, the way holding the
    // right button steers on land.
    //
    // Swimming took its forward from the camera's 3D direction but its strafe
    // and its facing from the character's own, so the two disagreed the moment
    // the camera was panned: the swimmer went one way, sidled another, and kept
    // facing a third, with the stroke animation pointing wherever the body
    // happened to be left. Retail turns the swimmer to the camera and swims
    // them where they look.
    //
    // Only while there is movement input. Treading on the spot and orbiting the
    // camera to look around should not spin the character on the water.
    // steering_ is the touch controls saying a finger is dragging the view.
    // On a phone that is what the right mouse button is on a desktop: the
    // character turns to face where the player is looking, so that walking
    // forward goes where the screen points.
    bool cameraDrivesFacing = rightMouseDown || mouseAutorun || steering_ ||
                              (swimming && hasMoveInput);
    // During taxi flights, orientation is controlled by the flight path, not player input
    if (cameraDrivesFacing && !externalFollow_) {
        facingYaw = yaw;
    }
    float moveYaw = cameraDrivesFacing ? yaw : facingYaw;
    if (intoxication_ > 0.34f) {
        // Drunk and smashed characters weave while trying to walk. Keep facing
        // authoritative; only the travel direction stumbles side to side.
        moveYaw += std::sin(intoxicationTime_ * 2.1f) * 11.0f * intoxication_;
    }
    float moveYawRad = glm::radians(moveYaw);
    glm::vec3 forward(std::cos(moveYawRad), std::sin(moveYawRad), 0.0f);
    glm::vec3 right(-std::sin(moveYawRad), std::cos(moveYawRad), 0.0f);

    // Flight goes where the mount faces, not where the camera looks. Orbiting
    // the camera with the left button is looking around, and it took the
    // flight direction with it: W flew toward the camera's view, whatever the
    // mount was pointing at. The pitch follows the camera only while the
    // camera steers, as the facing does, and holds otherwise.
    if (cameraDrivesFacing && !externalFollow_) {
        const float len = glm::length(forward3D);
        if (len > 1e-4f) {
            flightPitchDeg_ = glm::degrees(std::asin(std::clamp(forward3D.z / len, -1.0f, 1.0f)));
        }
    }
    if (!isFlightAirborne()) flightPitchDeg_ = 0.0f;
    const float flightPitchRad = glm::radians(flightPitchDeg_);
    const glm::vec3 flightForward(std::cos(moveYawRad) * std::cos(flightPitchRad),
                                  std::sin(moveYawRad) * std::cos(flightPitchRad),
                                  std::sin(flightPitchRad));

    // Retain short taps/wheel pulses for sitting; diving still needs held input.
    bool prevSitting = sitting;
    bool xDown = bindings.isCommandDown("SITORSTAND");
    if (bindings.isCommandPressed("SITORSTAND") && !mounted_ && !swimming) {
        sitting = !sitting;
    }
    if (mounted_) sitting = false;

    // Stand up on any movement input or jump while sitting (WoW behaviour)
    //
    // Both mouse buttons is a way of walking forward, and it was missing from
    // this list while every key that does the same was on it. Sitting gates
    // the autorun it produces, so the two together meant that after eating
    // there was no way to move with the mouse at all: the input that would
    // have stood the character up was the one input that could not.
    if (sitting && !movementSuppressed) {
        const bool anyMoveKey = bindings.isCommandDown("MOVEFORWARD") ||
            bindings.isCommandDown("MOVEBACKWARD") || bindings.isCommandDown("TURNLEFT") ||
            bindings.isCommandDown("TURNRIGHT") || bindings.isCommandDown("STRAFELEFT") ||
            bindings.isCommandDown("STRAFERIGHT") || bindings.isCommandDown("JUMP");
        // Not gated on uiWantsKeyboard, which is a different question, and not
        // on the interface wanting the mouse either: both flags are already
        // cleared at the press when it did.
        const bool mouseWalk = leftMouseDown && rightMouseDown;
        if (anyMoveKey || mouseWalk) sitting = false;
    }

    // Notify server when the player stands up via local input
    if (prevSitting && !sitting && standUpCallback_) {
        standUpCallback_();
    }

    // Notify server when the player sits down via local input
    if (!prevSitting && sitting && sitDownCallback_) {
        sitDownCallback_();
    }

    // Update eye height based on crouch state (smooth transition)
    float targetEyeHeight = sitting ? CROUCH_EYE_HEIGHT : STAND_EYE_HEIGHT;
    float heightLerpSpeed = 10.0f * deltaTime;
    eyeHeight = eyeHeight + (targetEyeHeight - eyeHeight) * std::min(1.0f, heightLerpSpeed);

    // Calculate horizontal movement vector
    glm::vec3 movement(0.0f);

    if (nowForward) movement += forward;
    if (nowBackward) movement -= forward;
    if (nowStrafeLeft) movement += right;
    if (nowStrafeRight) movement -= right;

    // In the air the path is the one the fall started on (see the latch
    // above): a world direction and a speed, fixed until the ground is back.
    if (airMoveLatched_ && useWoWSpeed) {
        if (airLatchedNow) {
            const float len = glm::length(glm::vec2(movement));
            if (len > 1e-4f) {
                airDirX_ = movement.x / len;
                airDirY_ = movement.y / len;
                // A drift started from a standing fall is priced as walking
                // (GetSpeed with its force-walk argument, 0x987570).
                airSpeed_ = airDriftStart
                    ? std::min(stockSpeeds().walk, stockSpeeds().run)
                    : speed;
            } else {
                airDirX_ = forward.x;
                airDirY_ = forward.y;
                airSpeed_ = 0.0f;
            }
        }
        movement = (airSpeed_ > 0.0f) ? glm::vec3(airDirX_, airDirY_, 0.0f) : glm::vec3(0.0f);
        speed = airSpeed_;
    }

    if (glm::dot(movement, movement) > 0.0001f) {
        travelYaw_ = glm::degrees(std::atan2(movement.y, movement.x));
    } else {
        travelYaw_ = facingYaw;
    }

    // Everything the two camera modes need from the work above.
    FrameInput f;
    f.physicsDeltaTime = physicsDeltaTime;
    f.gravity = gravity;
    f.jumpVel = jumpVel;
    f.speed = speed;
    f.forward = forward;
    f.right = right;
    f.forward3D = forward3D;
    f.flightForward = flightForward;
    f.movement = movement;
    f.nowForward = nowForward;
    f.nowBackward = nowBackward;
    f.nowStrafeLeft = nowStrafeLeft;
    f.nowStrafeRight = nowStrafeRight;
    f.nowTurnLeft = nowTurnLeft;
    f.nowTurnRight = nowTurnRight;
    f.nowJump = nowJump;
    f.spaceHeld = spaceHeld;
    f.xDown = xDown;
    f.uiWantsKeyboard = uiWantsKeyboard;

    // Two cameras: one orbits a character and moves it, one flies itself.
    if (thirdPerson && followTarget) {
        updateThirdPersonCamera(deltaTime, f);
    } else {
        updateFreeFlyCamera(deltaTime, f);
    }

    // The modes adjust the move - the swim clamp and the flight path both do -
    // and the movement opcodes below are decided from what actually happened.
    movement = f.movement;

    // --- Edge-detection: send movement opcodes on state transitions ---
    if (movementCallback) {
        // Forward/backward
        if (nowForward && !wasMovingForward) {
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_START_FORWARD));
        }
        if (nowBackward && !wasMovingBackward) {
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_START_BACKWARD));
        }
        if ((!nowForward && wasMovingForward) || (!nowBackward && wasMovingBackward)) {
            if (!nowForward && !nowBackward) {
                movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_STOP));
            }
        }

        // Strafing
        if (nowStrafeLeft && !wasStrafingLeft) {
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_START_STRAFE_LEFT));
        }
        if (nowStrafeRight && !wasStrafingRight) {
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_START_STRAFE_RIGHT));
        }
        if ((!nowStrafeLeft && wasStrafingLeft) || (!nowStrafeRight && wasStrafingRight)) {
            if (!nowStrafeLeft && !nowStrafeRight) {
                movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_STOP_STRAFE));
            }
        }

        // Turning
        if (nowTurnLeft && !wasTurningLeft) {
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_START_TURN_LEFT));
        }
        if (nowTurnRight && !wasTurningRight) {
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_START_TURN_RIGHT));
        }
        if ((!nowTurnLeft && wasTurningLeft) || (!nowTurnRight && wasTurningRight)) {
            if (!nowTurnLeft && !nowTurnRight) {
                movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_STOP_TURN));
            }
        }

        // Jump. Not in flight, where space climbs - and where grounded is held
        // true only to keep the fall-damage checks quiet, so every press sent
        // the server a jump from mid-air.
        if (nowJump && !wasJumping && grounded && !isFlightAirborne() && !onSteepSlope_ &&
            !movementRooted_) {
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_JUMP));
        }
        // A swimmer's hop at the surface is a jump from the water (0x730D10 ->
        // 0x72EB80 -> event 0xA): sent while SWIMMING is still set, so it
        // carries the water jump's 9.0967, and the jump clears SWIMMING and
        // ASCENDING itself (0x988370) - no STOP_SWIM or STOP_ASCEND follows.
        if (surfaceHopThisFrame_) {
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_JUMP));
        }

        // Walk/run mode. The stock toggle sends SET_WALK_MODE / SET_RUN_MODE
        // and sets WALKING (0x100) in the flags (0x98B0E0), so the server and
        // every other client know the character walks; without it a walker
        // was a runner at walk speed to everyone else.
        if (walking != wasWalkMode_) {
            movementCallback(static_cast<uint32_t>(walking ? game::Opcode::MSG_MOVE_SET_WALK_MODE
                                                           : game::Opcode::MSG_MOVE_SET_RUN_MODE));
            wasWalkMode_ = walking;
        }

        // Fall landing, for a fall that actually happened.
        //
        // A bump at riding speed breaks ground contact for a frame, and the
        // frame after it this read as a landing: the packet went out, and
        // every client in range played the landing animation on the mount.
        // Crossing open ground did that several times a second, which is what
        // other players see as a mount behaving erratically.
        //
        // Long enough to be a fall rather than a step. Well under the shortest
        // real drop - a jump is airborne for about a second - so nothing a
        // player would call a fall is lost.
        constexpr float kFallReportSeconds = 0.15f;
        if (wasFalling && grounded && airborneSeconds_ >= kFallReportSeconds) {
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_FALL_LAND));
        }
    }

    // Swimming state transitions
    if (movementCallback) {
        if (swimming && !wasSwimming) {
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_START_SWIM));
        } else if (!swimming && wasSwimming && !surfaceHopThisFrame_) {
            movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_STOP_SWIM));
        }
    }

    // Flight ascend/descend transitions (Space = ascend, X = descend while mounted+flying)
    if (movementCallback && !externalFollow_) {
        // In water too: the stock JumpOrAscendStart ascends a swimmer (event
        // 6, START_ASCEND 0x359), and the server and other clients see the
        // climb from the flag.
        const bool airborne = isFlightAirborne() || swimming;
        const bool nowAscending = airborne && spaceHeld;
        const bool nowDescending = airborne && xDown && (mounted_ || swimming);

        if (airborne) {
            if (nowAscending && !wasAscending_) {
                movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_START_ASCEND));
            } else if (!nowAscending && wasAscending_) {
                movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_STOP_ASCEND));
            }
            if (nowDescending && !wasDescending_) {
                movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_START_DESCEND));
            } else if (!nowDescending && wasDescending_) {
                // No separate STOP_DESCEND opcode; STOP_ASCEND ends all vertical movement
                movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_STOP_ASCEND));
            }
        } else {
            // Left flight mode: clear any lingering vertical movement states
            // (a surface hop's jump already cleared them).
            if (surfaceHopThisFrame_) {
            } else if (wasAscending_) {
                movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_STOP_ASCEND));
            } else if (wasDescending_) {
                movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_STOP_ASCEND));
            }
        }
        wasAscending_ = nowAscending;
        wasDescending_ = nowDescending;
    }

    // Update previous-frame state
    wasSwimming = swimming;
    wasMovingForward = nowForward;
    wasMovingBackward = nowBackward;
    wasStrafingLeft = nowStrafeLeft;
    wasStrafingRight = nowStrafeRight;
    moveForwardActive = nowForward;
    moveBackwardActive = nowBackward;
    strafeLeftActive = nowStrafeLeft;
    strafeRightActive = nowStrafeRight;
    turningLeftActive = nowTurnLeft;
    turningRightActive = nowTurnRight;
    wasTurningLeft = nowTurnLeft;
    wasTurningRight = nowTurnRight;
    wasJumping = nowJump;
    wasFalling = !grounded && verticalVelocity <= 0.0f;
    // Counted while airborne and zeroed the moment the ground is back, so the
    // landing test above reads the length of the fall that just ended.
    airborneSeconds_ = grounded ? 0.0f : (airborneSeconds_ + deltaTime);

    // R key is now handled above with chat safeguard (WantTextInput check)

    // Camera shake (SMSG_CAMERA_SHAKE): apply sinusoidal offset to final camera position.
    if (shakeElapsed_ < shakeDuration_) {
        shakeElapsed_ += deltaTime;
        float t = shakeElapsed_ / shakeDuration_;
        // Envelope: fade out over the last 30% of shake duration
        float envelope = (t < 0.7f) ? 1.0f : (1.0f - (t - 0.7f) / 0.3f);
        float theta = shakeElapsed_ * shakeFrequency_ * core::coords::TWO_PI;
        glm::vec3 offset(
            shakeMagnitude_ * envelope * std::sin(theta),
            shakeMagnitude_ * envelope * std::cos(theta * 1.3f),
            shakeMagnitude_ * envelope * std::sin(theta * 0.7f) * 0.5f
        );
        if (camera) {
            camera->setPosition(camera->getPosition() + offset);
        }
    }

    // Scaled by the same setting as the shake above, because it is the same
    // thing from the player's side: the view moving without being asked to.
    //
    // The stumble in the travel direction is deliberately not scaled with it.
    // That one is what being drunk does to the character rather than to the
    // picture, and turning it off would be an advantage rather than a comfort.
    if (intoxication_ > 0.0f && camera) {
        const float swayYaw = std::sin(intoxicationTime_ * 1.35f) * 2.5f * intoxication_ * shakeScale_;
        const float swayPitch = std::sin(intoxicationTime_ * 1.8f + 0.7f) * 1.6f * intoxication_ * shakeScale_;
        camera->setRotation(yaw + swayYaw,
                            glm::clamp(pitch + swayPitch, MIN_PITCH, MAX_PITCH));
    }
}

void CameraController::processMouseMotion(const SDL_MouseMotionEvent& event) {
    if (!enabled || !camera) {
        return;
    }
    if (introActive) {
        return;
    }

    if (!mouseButtonDown) {
        return;
    }
    if (rotationSuppressed_) {
        return;
    }

    // Hold rotation until the cursor has actually gone somewhere, so a click
    // that means "target this" does not swing the view on the way.
    //
    // The same distance the click test uses, measured the same way: the net
    // offset from the press, not the length of the path taken to get there.
    if (!rotateArmed_) {
        dragOffsetX_ += static_cast<float>(event.xrel);
        dragOffsetY_ += static_cast<float>(event.yrel);
        const float threshold = core::clickDragThreshold();
        if (dragOffsetX_ * dragOffsetX_ + dragOffsetY_ * dragOffsetY_ <
            threshold * threshold) {
            return;
        }
        rotateArmed_ = true;  // past the dead-zone: this is a deliberate drag
    }

    // Directly update stored yaw/pitch (no lossy forward-vector derivation)
    yaw -= event.xrel * mouseSensitivity;
    // SDL yrel > 0 = mouse moved DOWN. In WoW, mouse-down = look down = pitch decreases.
    // invertMouse flips to flight-sim style (mouse-down = look up).
    float invert = invertMouse ? 1.0f : -1.0f;
    pitch += event.yrel * mouseSensitivity * invert;

    // WoW-style pitch limits: can look almost straight down, limited upward
    pitch = glm::clamp(pitch, MIN_PITCH, MAX_PITCH);

    camera->setRotation(yaw, pitch);
}

void CameraController::processMouseButton(const SDL_MouseButtonEvent& event) {
    if (!enabled && event.down) {
        return;
    }

    // Don't capture mouse when ImGui wants it (hovering UI windows), or when
    // FrameXML does. FrameXML draws into ImGui's background draw list, so
    // WantCaptureMouse is false over every frame it owns - pressing a bag item
    // turned the camera as well as pressing the item, and dragging one swung
    // the view around.
    bool uiWantsMouse = ImGui::GetIO().WantCaptureMouse || ui::frameXmlOwnsMouse();

    if (event.button == SDL_BUTTON_LEFT) {
        leftMouseDown = (event.down) && !uiWantsMouse;
        if (event.down && event.clicks >= 2) {
            autoRunning = false;
        }
    }
    if (event.button == SDL_BUTTON_RIGHT) {
        rightMouseDown = (event.down) && !uiWantsMouse;
    }

    bool anyDown = leftMouseDown || rightMouseDown;
    if (anyDown && !mouseButtonDown) {
        // SDL3 captures per window rather than globally. The focused window is the one the player is pointing at, and relative mode means nothing for any other - so that is the one to ask.
        SDL_SetWindowRelativeMouseMode(SDL_GetKeyboardFocus(), true);
        // Throw away the delta the switch itself makes. Entering relative
        // mode hands over the movement since the last relative read, which on
        // a press is everything the cursor did on its way to the thing being
        // clicked - a whole screen's worth, delivered as one motion event
        // that cleared any dead-zone on its own.
        SDL_GetRelativeMouseState(nullptr, nullptr);
        // Arm the rotation dead-zone fresh for this press.
        rotateArmed_ = false;
        dragOffsetX_ = 0.0f;
        dragOffsetY_ = 0.0f;
    } else if (!anyDown && mouseButtonDown) {
        SDL_SetWindowRelativeMouseMode(SDL_GetKeyboardFocus(), false);
        rotateArmed_ = false;
    }
    mouseButtonDown = anyDown;
}

void CameraController::releaseMouseCapture() {
    leftMouseDown = false;
    rightMouseDown = false;
    mouseButtonDown = false;
    rotateArmed_ = false;
    dragOffsetX_ = 0.0f;
    dragOffsetY_ = 0.0f;
    SDL_SetWindowRelativeMouseMode(SDL_GetKeyboardFocus(), false);
    SDL_ShowCursor();
}

void CameraController::resetAngles() {
    if (!camera) return;
    yaw = defaultYaw;
    facingYaw = defaultYaw;
    pitch = defaultPitch;
    followYaw_.active = false;
    followPitch_.active = false;
    camera->setRotation(yaw, pitch);
}

void CameraController::reset() {
    if (!camera) {
        return;
    }

    yaw = defaultYaw;
    facingYaw = defaultYaw;
    pitch = defaultPitch;
    followYaw_.active = false;
    followPitch_.active = false;
    collisionShownDist_ = -1.0f;
    verticalVelocity = 0.0f;
    grounded = true;
    swimming = false;
    sitting = false;
    renderZOffset_ = 0.0f;
    slopeSlideVel_ = glm::vec2(0.0f);
    onSteepSlope_ = false;
    autoRunning = false;
    noGroundTimer_ = 0.0f;
    autoUnstuckFired_ = false;

    // Clear edge-state so movement packets can re-start cleanly after respawn.
    wasMovingForward = false;
    wasMovingBackward = false;
    wasStrafingLeft = false;
    wasStrafingRight = false;
    wasTurningLeft = false;
    wasTurningRight = false;
    wasJumping = false;
    wasFalling = false;
    wasSwimming = false;
    moveForwardActive = false;
    moveBackwardActive = false;
    strafeLeftActive = false;
    strafeRightActive = false;
    turningLeftActive = false;
    turningRightActive = false;

    glm::vec3 spawnPos = defaultPosition;

    auto evalFloorAt = [&](float x, float y, float refZ) -> std::optional<float> {
        std::optional<float> terrainH;
        std::optional<float> wmoH;
        std::optional<float> m2H;
        if (terrainManager) {
            terrainH = terrainManager->getHeightAt(x, y);
        }
        // Probe from the highest of terrain, refZ (server position), and defaultPosition.z
        // so we don't miss WMO floors above terrain (e.g. Stormwind city surface).
        float floorProbeZ = std::max(terrainH.value_or(refZ), refZ);
        if (wmoRenderer) {
            wmoH = wmoRenderer->getFloorHeight(x, y, floorProbeZ + 4.0f);
        }
        if (m2Renderer && !externalFollow_) {
            m2H = m2Renderer->getFloorHeight(x, y, floorProbeZ + 4.0f);
        }
        auto h = selectReachableFloor(terrainH, wmoH, refZ, 16.0f);
        if (!h) {
            h = selectHighestFloor(terrainH, wmoH, m2H);
        }
        return h;
    };

    // In online mode, try to snap to a nearby floor but fall back to the server
    // position when no WMO floor is found (e.g. WMO not loaded yet in cities).
    // This prevents spawning under WMO cities like Stormwind.
    if (onlineMode) {
        auto h = evalFloorAt(spawnPos.x, spawnPos.y, spawnPos.z);
        if (h && std::abs(*h - spawnPos.z) < 16.0f) {
            spawnPos.z = *h + 0.05f;
        }
        // else: keep server Z as-is
        lastGroundZ = spawnPos.z - 0.05f;

        camera->setRotation(yaw, pitch);
        glm::vec3 forward3D = camera->getForward();

        if (thirdPerson && followTarget) {
            *followTarget = spawnPos;
            currentDistance = userTargetDistance;
            collisionDistance = currentDistance;
            float mountedOffset = mounted_ ? mountHeightOffset_ : 0.0f;
            glm::vec3 pivot = spawnPos + glm::vec3(0.0f, 0.0f, followedPivotHeight() + mountedOffset);
            glm::vec3 camDir = -forward3D;
            glm::vec3 camPos = pivot + camDir * currentDistance;
            smoothedCamPos = camPos;
            camera->setPosition(camPos);
        } else {
            spawnPos.z += eyeHeight;
            smoothedCamPos = spawnPos;
            camera->setPosition(spawnPos);
        }

        LOG_INFO("Camera reset to server position (online mode)");
        return;
    }

    // Search nearby for a stable, non-steep spawn floor to avoid waterfall/ledge spawns.
    float bestScore = std::numeric_limits<float>::max();
    glm::vec3 bestPos = spawnPos;
    bool foundBest = false;
    constexpr float radiiOffline[] = {0.0f, 6.0f, 12.0f, 18.0f, 24.0f, 32.0f};
    const float* radii = radiiOffline;
    const int radiiCount = 6;
    constexpr int ANGLES = 16;
    constexpr float PI = core::coords::PI;
    for (int ri = 0; ri < radiiCount; ri++) {
        float r = radii[ri];
        int steps = (r <= 0.01f) ? 1 : ANGLES;
        for (int i = 0; i < steps; i++) {
            float a = (2.0f * PI * static_cast<float>(i)) / static_cast<float>(steps);
            float x = defaultPosition.x + r * std::cos(a);
            float y = defaultPosition.y + r * std::sin(a);
            auto h = evalFloorAt(x, y, defaultPosition.z);
            if (!h) continue;

            // Allow large downward snaps, but avoid snapping onto high roofs/odd geometry.
            constexpr float MAX_SPAWN_SNAP_UP = 16.0f;
            if (*h > defaultPosition.z + MAX_SPAWN_SNAP_UP) continue;

            float score = r * 0.02f;
            if (terrainManager) {
                // Penalize steep/unstable spots.
                int slopeSamples = 0;
                float slopeAccum = 0.0f;
                constexpr float off = 2.5f;
                const float dx[4] = {off, -off, 0.0f, 0.0f};
                const float dy[4] = {0.0f, 0.0f, off, -off};
                for (int s = 0; s < 4; s++) {
                    auto hn = terrainManager->getHeightAt(x + dx[s], y + dy[s]);
                    if (!hn) continue;
                    slopeAccum += std::abs(*hn - *h);
                    slopeSamples++;
                }
                if (slopeSamples > 0) {
                    score += (slopeAccum / static_cast<float>(slopeSamples)) * 2.0f;
                }
            }
            if (waterRenderer) {
                auto wh = waterRenderer->getWaterHeightAt(x, y);
                if (wh && *h < *wh - 0.2f) {
                    score += 8.0f;
                }
            }
            if (wmoRenderer) {
                const glm::vec3 from(x, y, *h + 0.20f);
                const bool insideWMO = wmoRenderer->isInsideWMO(x, y, *h + 1.5f, nullptr);

                // Prefer outdoors for default hearth-like spawn points (offline only).
                // In online mode, trust the server position even if inside a WMO.
                if (insideWMO && !onlineMode) {
                    score += 120.0f;
                }

                // Reject points embedded in nearby walls by probing tiny cardinal moves.
                int wallHits = 0;
                constexpr float probeStep = 0.85f;
                const glm::vec3 probes[4] = {
                    glm::vec3(x + probeStep, y, *h + 0.20f),
                    glm::vec3(x - probeStep, y, *h + 0.20f),
                    glm::vec3(x, y + probeStep, *h + 0.20f),
                    glm::vec3(x, y - probeStep, *h + 0.20f),
                };
                for (const auto& to : probes) {
                    glm::vec3 adjusted;
                    if (wmoRenderer->checkWallCollision(from, to, adjusted)) {
                        wallHits++;
                    }
                }
                if (wallHits >= 2) {
                    continue; // Likely wedged in geometry.
                }
                if (wallHits == 1) {
                    score += 30.0f;
                }

                // If the point is inside a WMO, ensure there is an easy escape path.
                // If almost all directions are blocked, treat it as invalid spawn.
                if (insideWMO) {
                    int blocked = 0;
                    constexpr int radialChecks = 12;
                    constexpr float radialDist = 2.2f;
                    for (int ri = 0; ri < radialChecks; ri++) {
                        float ang = (2.0f * PI * static_cast<float>(ri)) / static_cast<float>(radialChecks);
                        glm::vec3 to(
                            x + std::cos(ang) * radialDist,
                            y + std::sin(ang) * radialDist,
                            *h + 0.20f
                        );
                        glm::vec3 adjusted;
                        if (wmoRenderer->checkWallCollision(from, to, adjusted)) {
                            blocked++;
                        }
                    }
                    if (blocked >= 9) {
                        continue; // Enclosed by interior/wall geometry.
                    }
                    score += static_cast<float>(blocked) * 3.0f;
                }
            }

            if (score < bestScore) {
                bestScore = score;
                bestPos = glm::vec3(x, y, *h + 0.05f);
                foundBest = true;
            }
        }
    }
    if (foundBest) {
        spawnPos = bestPos;
        lastGroundZ = spawnPos.z - 0.05f;
    }

    camera->setRotation(yaw, pitch);
    glm::vec3 forward3D = camera->getForward();

    if (thirdPerson && followTarget) {
        // In follow mode, respawn the character (feet position), then place camera behind it.
        *followTarget = spawnPos;

        currentDistance = userTargetDistance;
        collisionDistance = currentDistance;

        float mountedOffset = mounted_ ? mountHeightOffset_ : 0.0f;
        glm::vec3 pivot = spawnPos + glm::vec3(0.0f, 0.0f, followedPivotHeight() + mountedOffset);
        glm::vec3 camDir = -forward3D;
        glm::vec3 camPos = pivot + camDir * currentDistance;
        smoothedCamPos = camPos;
        camera->setPosition(camPos);
    } else {
        // Free-fly mode keeps camera eye-height above ground.
        if (foundBest) {
            spawnPos.z += eyeHeight;
        }
        smoothedCamPos = spawnPos;
        camera->setPosition(spawnPos);
    }

    LOG_INFO("Camera reset to default position");
}

void CameraController::teleportTo(const glm::vec3& pos) {
    if (!camera) return;

    verticalVelocity = 0.0f;
    grounded = true;
    swimming = false;
    sitting = false;
    renderZOffset_ = 0.0f;
    slopeSlideVel_ = glm::vec2(0.0f);
    onSteepSlope_ = false;
    lastGroundZ = pos.z;
    noGroundTimer_ = 0.0f;  // Reset grace period so terrain has time to stream
    autoUnstuckFired_ = false;
    continuousFallTime_ = 0.0f;

    // Invalidate active WMO group so it's re-detected at new position
    if (wmoRenderer) {
        wmoRenderer->updateActiveGroup(pos.x, pos.y, pos.z + 1.0f);
    }

    if (thirdPerson && followTarget) {
        *followTarget = pos;
        camera->setRotation(yaw, pitch);
        glm::vec3 forward3D = camera->getForward();
        float mountedOffset = mounted_ ? mountHeightOffset_ : 0.0f;
        glm::vec3 pivot = pos + glm::vec3(0.0f, 0.0f, followedPivotHeight() + mountedOffset);
        glm::vec3 camDir = -forward3D;
        glm::vec3 camPos = pivot + camDir * currentDistance;
        smoothedCamPos = camPos;
        camera->setPosition(camPos);
    } else {
        glm::vec3 camPos = pos + glm::vec3(0.0f, 0.0f, eyeHeight);
        smoothedCamPos = camPos;
        camera->setPosition(camPos);
    }

    LOG_INFO("Teleported to (", pos.x, ", ", pos.y, ", ", pos.z, ")");
}

bool CameraController::groundNotStreamedYet(float x, float y) const {
    return terrainManager != nullptr && !terrainManager->isTileLoadedAt(x, y);
}

void CameraController::applyLookDelta(float dxPixels, float dyPixels) {
    if (!enabled || !camera || introActive) return;

    // A deliberate look, like a mouse drag: it ends any follow swing, which
    // would otherwise override it for the rest of its duration.
    followYaw_.active = false;
    followPitch_.active = false;

    yaw -= dxPixels * mouseSensitivity;
    const float invert = invertMouse ? 1.0f : -1.0f;
    pitch += dyPixels * mouseSensitivity * invert;
    pitch = glm::clamp(pitch, MIN_PITCH, MAX_PITCH);
    camera->setRotation(yaw, pitch);
}

void CameraController::processMouseWheel(float delta) {
    // A notch is a yard, at every distance: CameraZoomIn(1.0) (FUN_006017e0)
    // asks FUN_005ff950 for 1 yd, travelled at cameraDistanceMoveSpeed - see
    // stepZoomTowardTarget. It used to be 15% of the distance.
    userTargetDistance -= delta * kZoomStepYards;
    // A multiple of the original client's limit rather than a choice between
    // two numbers, so the game's own Max Camera Distance slider means something
    // at every position instead of only at its ends. Stock: min(15 * factor,
    // 50), FUN_006000e0; there is no separate indoor limit.
    float maxDist = std::min(MAX_DISTANCE_NORMAL * maxDistanceFactor_, MAX_DISTANCE_EXTENDED);
    userTargetDistance = glm::clamp(userTargetDistance, MIN_DISTANCE, maxDist);
}

void CameraController::stepZoomTowardTarget(float deltaTime) {
    // Linear, not eased: FUN_006000e0 moves the zoom distance (+0x1e8) by
    // elapsed_ms * cameraDistanceMoveSpeed * 0.001 each frame until the
    // requested travel time is used up, and the camera distance (+0x118)
    // follows it directly unless a collision recovery is easing it.
    const float diff = userTargetDistance - currentDistance;
    const float step = kZoomMoveSpeed * deltaTime;
    if (std::abs(diff) <= step) {
        currentDistance = userTargetDistance;
    } else {
        currentDistance += (diff > 0.0f) ? step : -step;
    }
}

void CameraController::updateStockFollow(float deltaTime, bool forward, bool backward,
                                         bool strafeLeft, bool strafeRight,
                                         bool turnLeft, bool turnRight,
                                         bool cameraDrivesFacing) {
    // Stock 3.3.5a following, CGCamera FUN_00602760 (Wow.exe 12340).
    //
    // It runs when the movement keys or mouse buttons change, not each frame.
    // It works out the situation - the highest of Fear 6, Turn 5, Strafe 4,
    // Move 3, Track 2, Stop 1, Idle 0 that applies - and reads two CVars for
    // it under the current style: cameraSmooth<Style><Situation>Delay and
    // ...Factor. Then per component cameraSmoothViewData<Style><Comp>Delay
    // and ...Factor; the component's factor is the two multiplied. Defaults:
    //
    //   style      Idle Stop Move Strafe Turn | ViewData Dist Pitch Yaw
    //   Never(0)    0    0    0     0     0   |           0     0    0
    //   Smart(1)    0    0    1     1     1   |           0     0    1
    //   Always(2)   1    1    1     1     1   |           0     1    1
    //   Smarter(4)  0    0    1     1     1   |           0     1    1
    //
    // (delays all 0; Track and Fear are 0.4 s / 10 under Smart and Smarter).
    // A component whose factor is 0 stops where it is (FUN_005fef40 /
    // FUN_005feee0). Otherwise:
    //
    //   yaw   (FUN_00602680 gate: cameraSmoothYaw, offset from facing != 0)
    //         target = facing + view yaw (0); FUN_00601410:
    //         duration = |delta| / cameraYawSmoothSpeed(180 deg/s) * factor
    //   pitch (FUN_005ffeb0 gate: cameraSmoothPitch, not swimming/flying,
    //         pitch outside [cameraPitchSmoothMin 0, Max 30] deg down)
    //         target = the nearer edge of that band; FUN_00601190:
    //         duration = |delta| / cameraPitchSmoothSpeed(45 deg/s) * factor
    //
    // The longest duration of the components started, clamped to
    // [cameraSmoothTimeMin 0.1, cameraSmoothTimeMax 2.0] s, is given to all of
    // them, and FUN_00603d30 runs each with the cosine curve of FUN_008ca080.
    // A component already easing toward the same target keeps going rather
    // than restarting (the early return in FUN_00601410 / FUN_00601190).
    auto wrap180 = [](float d) {
        d = std::fmod(d + 180.0f, 360.0f);
        if (d < 0.0f) d += 360.0f;
        return d - 180.0f;
    };

    // Not while the camera is someone else's: the left button holds the yaw
    // to the world and the player is placing it; the right button, autorun on
    // both buttons, a steering finger or swimming make the facing follow the
    // camera, so there is no offset to take back.
    if (!thirdPerson || externalFollow_ || introActive || barberView_ ||
        leftMouseDown || cameraDrivesFacing) {
        followYaw_.active = false;
        followPitch_.active = false;
        // Evaluated again when released, as FUN_005fa450 does on button up.
        followInputSig_ = 0xFFFFFFFFu;
        return;
    }

    const uint32_t sig = (forward ? 1u : 0u) | (backward ? 2u : 0u) |
                         (strafeLeft ? 4u : 0u) | (strafeRight ? 8u : 0u) |
                         (turnLeft ? 16u : 0u) | (turnRight ? 32u : 0u);
    if (sig != followInputSig_) {
        followInputSig_ = sig;

        // Situation factor; the delays of these situations are all zero.
        const bool moving = forward || backward || strafeLeft || strafeRight ||
                            turnLeft || turnRight;
        float situationFactor = 0.0f;
        switch (followStyle_) {
            case 0:  situationFactor = 0.0f; break;                    // Never
            case 1:                                                    // Smart
            case 2:  situationFactor = moving ? 1.0f : 0.0f; break;    // Smarter
            default: situationFactor = 1.0f; break;                    // Always
        }
        const float yawFactor = situationFactor * (followStyle_ == 0 ? 0.0f : 1.0f);
        const float pitchFactor = situationFactor * (followStyle_ >= 2 ? 1.0f : 0.0f);

        float duration = 0.0f;
        bool yawRuns = false;
        bool pitchRuns = false;

        // Pitch first, as FUN_00602760 does it. Pitch here is up-positive;
        // the stock band is in degrees looking down.
        const bool flying = isFlightAirborne();
        const float down = -pitch;
        if (!swimming && !flying &&
            (down < kFollowPitchDownMinDeg || down > kFollowPitchDownMaxDeg)) {
            if (pitchFactor == 0.0f) {
                followPitch_.active = false;
            } else {
                const float targetPitch =
                    -std::clamp(down, kFollowPitchDownMinDeg, kFollowPitchDownMaxDeg);
                if (!(followPitch_.active && std::abs(followPitch_.to - targetPitch) < 0.06f)) {
                    followPitch_.active = true;
                    followPitch_.from = pitch;
                    followPitch_.to = targetPitch;
                    followPitch_.elapsed = 0.0f;
                    followPitch_.duration =
                        std::abs(targetPitch - pitch) / kFollowPitchSpeedDeg * pitchFactor;
                }
                pitchRuns = true;
                duration = std::max(duration, followPitch_.duration);
            }
        }

        const float offset = wrap180(yaw - facingYaw);
        if (std::abs(offset) > 0.06f) {   // 0.001 rad, the stock epsilon (0xa1e34c)
            if (yawFactor == 0.0f) {
                followYaw_.active = false;
            } else {
                if (!followYaw_.active) {
                    followYaw_.active = true;
                    followYaw_.from = offset;
                    followYaw_.to = 0.0f;
                    followYaw_.elapsed = 0.0f;
                    followYaw_.duration = std::abs(offset) / kFollowYawSpeedDeg * yawFactor;
                }
                yawRuns = true;
                duration = std::max(duration, followYaw_.duration);
            }
        } else if (yawFactor == 0.0f) {
            followYaw_.active = false;
        }

        duration = std::clamp(duration, kFollowTimeMin, kFollowTimeMax);
        if (yawRuns) followYaw_.duration = duration;
        if (pitchRuns) followPitch_.duration = duration;
    }

    auto ease = [](FollowEase& e, float dt) {
        e.elapsed += dt;
        const float t = (e.duration > 0.0f) ? e.elapsed / e.duration : 1.0f;
        if (t >= 1.0f) {
            e.active = false;
            return e.to;
        }
        const float k = (1.0f - std::cos(t * 3.14159265f)) * 0.5f;
        return e.from + (e.to - e.from) * k;
    };
    bool changed = false;
    if (followYaw_.active) {
        // Relative to the facing, so a turn key during the swing carries it
        // round with the character, as the stock relative yaw does.
        yaw = facingYaw + ease(followYaw_, deltaTime);
        changed = true;
    }
    if (followPitch_.active) {
        pitch = glm::clamp(ease(followPitch_, deltaTime), MIN_PITCH, MAX_PITCH);
        changed = true;
    }
    if (changed) camera->setRotation(yaw, pitch);
}

void CameraController::setFollowTarget(glm::vec3* target) {
    followTarget = target;
    if (target) {
        thirdPerson = true;
        LOG_INFO("Third-person camera enabled");
    } else {
        thirdPerson = false;
        LOG_INFO("Free-fly camera enabled");
    }
}

bool CameraController::isMoving() const {
    if (!enabled || !camera) {
        return false;
    }
    if (externalMoving_) return true;
    return moveForwardActive || moveBackwardActive || strafeLeftActive || strafeRightActive || autoRunning;
}

void CameraController::clearMovementInputs() {
    moveForwardActive = false;
    moveBackwardActive = false;
    strafeLeftActive = false;
    strafeRightActive = false;
    turningLeftActive = false;
    turningRightActive = false;
    autoRunning = false;
}

bool CameraController::isSprinting() const {
    return enabled && camera && runPace;
}

void CameraController::triggerMountJump() {
    // Apply physics-driven mount jump: vz = sqrt(2 * g * h)
    // Desired height and gravity are configurable constants
    // The stock client jumps a mount exactly as it jumps a character.
    if ((grounded || coyoteTimer > 0.0f) && !onSteepSlope_ && !movementRooted_) {
        verticalVelocity = useWoWSpeed ? WOW_JUMP_VELOCITY : JUMP_VELOCITY;
        grounded = false;
        coyoteTimer = 0.0f;
    }
}

void CameraController::applyKnockBack(float vcos, float vsin, float hspeed, float vspeed) {
    // The server sends (vcos, vsin) as the 2D direction vector in server/wire
    // coordinate space.  After the server→canonical→render swaps, the direction
    // in render space is simply (vcos, vsin) - the two swaps cancel each other.
    knockbackHorizVel_ = glm::vec2(vcos, vsin) * hspeed;
    knockbackActive_ = true;

    // vspeed in the wire packet is negative when the server wants to launch the
    // player upward (matches TrinityCore: data << float(-speedZ)).  Negate it
    // here to obtain the correct upward initial velocity.
    verticalVelocity = -vspeed;
    grounded = false;
    coyoteTimer = 0.0f;
    jumpBufferTimer = 0.0f;

    // Notify the server that the player left the ground so the FALLING flag is
    // set in subsequent movement heartbeats.  The normal jump detection
    // (nowJump && grounded) does not fire during a server-driven knockback.
    if (movementCallback) {
        movementCallback(static_cast<uint32_t>(game::Opcode::MSG_MOVE_JUMP));
    }
}

} // namespace rendering
} // namespace wowee
