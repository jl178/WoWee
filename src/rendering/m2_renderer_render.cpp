#include "rendering/shadow_params.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/m2_renderer_internal.h"
#include "rendering/m2_sway.hpp"
#include "rendering/m2_blend_mode.hpp"
#include "rendering/m2_glow_card.hpp"
#include "rendering/animation/sequence_variation.hpp"
#include "core/thread_pool.hpp"
#include "rendering/m2_model_classifier.hpp"
#include "rendering/hiz_system.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "rendering/render_constants.hpp"
#include "rendering/m2_view_distance.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/logger.hpp"
#include "core/profiler.hpp"
#include <chrono>
#include <cctype>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <unordered_set>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>
#include <limits>
#include <future>
#include <thread>

#include <set>
#include <utility>

namespace wowee {
namespace rendering {

/// Starts a new instance's animation and gives it bones to draw with now.
///
/// Both spawn paths need this: the one that takes a position and the one that
/// takes a whole matrix, and each had its own copy.
///
/// The bone seed is what keeps a new instance from being invisible for a
/// frame. Bones are computed in update(), so an instance spawned mid-frame has
/// none until the next one; copying them from a sibling of the same model
/// draws it immediately. A seed entry pointing at an instance that has since
/// gone is dropped rather than followed.
void M2Renderer::seedInstanceAnimation(const M2ModelGPU& model, uint32_t modelId,
                                       M2Instance& instance) {
        if (!model.sequences.empty()) {
            instance.currentSequenceIndex = 0;
            instance.idleSequenceIndex = 0;
            instance.animDuration = static_cast<float>(model.sequences[0].duration);
            instance.animTime = static_cast<float>(randRange(std::max(1u, model.sequences[0].duration)));
            instance.variationTimer = randFloat(rendering::M2_VARIATION_TIMER_MIN_MS, rendering::M2_VARIATION_TIMER_MAX_MS);
        }

    auto seedIt = boneSeedInstanceByModel_.find(modelId);
    if (seedIt != boneSeedInstanceByModel_.end()) {
        auto idxIt = instanceIndexById.find(seedIt->second);
        if (idxIt != instanceIndexById.end() && idxIt->second < instances.size()) {
            const auto& existing = instances[idxIt->second];
            if (existing.modelId == modelId && !existing.boneMatrices.empty()) {
                instance.boneMatrices = existing.boneMatrices;
                instance.bonesDirty[0] = instance.bonesDirty[1] = true;
            } else {
                boneSeedInstanceByModel_.erase(seedIt);  // stale entry
            }
        } else {
            boneSeedInstanceByModel_.erase(seedIt);  // that instance is gone
        }
    }

    // No sibling to copy from, so pay for the bones now.
    if (instance.boneMatrices.empty()) {
        computeBoneMatrices(model, instance, &cachedCamPos_);
    }
    if (!instance.boneMatrices.empty()) {
        boneSeedInstanceByModel_.emplace(modelId, instance.id);
    }
}

// A model the bone loop does not step still plays its first sequence: its
// texture scroll and its emitters are keyed to it, and update() wraps the
// clock at this length. Left at zero, the length was unknown, the clock was
// wrapped at a fixed 3.3 s instead, and a waterfall whose scroll lasts 2.6 s
// stood still for the difference every lap.
void M2Renderer::seedUnanimatedSequence(const M2ModelGPU& model, M2Instance& instance) {
    if (model.sequences.empty() || model.sequences[0].duration == 0) return;
    instance.currentSequenceIndex = 0;
    instance.idleSequenceIndex = 0;
    instance.animDuration = static_cast<float>(model.sequences[0].duration);
    instance.animTime = static_cast<float>(randRange(model.sequences[0].duration));
}

uint32_t M2Renderer::createInstance(uint32_t modelId, const glm::vec3& position,
                                     const glm::vec3& rotation, float scale,
                                     bool allowPositionDedup) {
    // Reject NaN inputs at the boundary - std::round of NaN is implementation-
    // defined and a NaN instance position propagates into the GPU model matrix,
    // either tripping Vulkan validation or rendering at the world origin.
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z) || !std::isfinite(rotation.x) ||
        !std::isfinite(rotation.y) || !std::isfinite(rotation.z) ||
        !std::isfinite(scale) || scale <= 0.0f) {
        return 0;
    }
    auto modelIt = models.find(modelId);
    if (modelIt == models.end()) {
        LOG_WARNING("Cannot create instance: model ", modelId, " not loaded");
        return 0;
    }
    const auto& mdlRef = modelIt->second;
    modelUnusedSince_.erase(modelId);


    // Deduplicate: skip if same model already at nearly the same position.
    // Uses hash map for O(1) lookup instead of O(N) scan.
    // Spell effects are exempt - transient visuals must always create fresh instances.
    if (allowPositionDedup && !mdlRef.isGroundDetail && !mdlRef.isSpellEffect) {
        DedupKey dk{.modelId = modelId,
                    .qx = static_cast<int32_t>(std::round(position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(position.z * 10.0f))};
        auto dit = instanceDedupMap_.find(dk);
        if (dit != instanceDedupMap_.end()) {
            return dit->second;
        }
    }

    M2Instance instance;
    instance.id = nextInstanceId++;
    instance.modelId = modelId;
    instance.position = position;
    if (mdlRef.isGroundDetail) {
        instance.position.z -= computeGroundDetailDownOffset(mdlRef, scale);
    }
    instance.rotation = rotation;
    instance.scale = scale;
    instance.updateModelMatrix();
    glm::vec3 localMin, localMax;
    getTightCollisionBounds(mdlRef, localMin, localMax);
    transformAABB(instance.modelMatrix, localMin, localMax, instance.worldBoundsMin, instance.worldBoundsMax);

    // Cache model flags on instance to avoid per-frame hash lookups
    instance.cachedHasAnimation = mdlRef.hasAnimation;
    instance.cachedDisableAnimation = mdlRef.disableAnimation;
    instance.cachedIsSmoke = mdlRef.isSmoke;
    instance.cachedHasParticleEmitters = !mdlRef.particleEmitters.empty();
    instance.cachedBoundRadius = mdlRef.boundRadius;
    instance.cachedIsGroundDetail = mdlRef.isGroundDetail;
    instance.cachedIsInvisibleTrap = mdlRef.isInvisibleTrap;
    instance.cachedIsInstancePortal = mdlRef.isInstancePortal;
    instance.cachedIsSkyBird = mdlRef.isSkyBird;
    instance.cachedIsLightBeam = mdlRef.isLightBeam;
    instance.cachedIsTransportDoodad = mdlRef.isTransportDoodad;
    instance.cachedIsBoat = mdlRef.isBoat;
    instance.cachedIsValid = mdlRef.isValid();
    instance.cachedModel = &mdlRef;
    instance.recomputeCachedCullFactors();

    // Initialize animation: play first sequence (usually Stand/Idle)
    const auto& mdl = mdlRef;
    if (mdl.hasAnimation && !mdl.disableAnimation) {
        seedInstanceAnimation(mdlRef, modelId, instance);
    } else {
        seedUnanimatedSequence(mdlRef, instance);
    }

    // Register in dedup map before pushing (uses original position, not ground-adjusted)
    // Spell effects are exempt from dedup tracking (transient, overlapping allowed).
    if (allowPositionDedup && !mdlRef.isGroundDetail && !mdlRef.isSpellEffect) {
        DedupKey dk{.modelId = modelId,
                    .qx = static_cast<int32_t>(std::round(position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(position.z * 10.0f))};
        instanceDedupMap_[dk] = instance.id;
    }

    // WOWEE_M2_CENSUS: what the model is drawn at, beside what it was
    // authored at. The load-time census gives the authored height; a game
    // object's scale comes from the server and is only known here.
    censusInstance(instance);
    instances.push_back(instance);
    size_t idx = instances.size() - 1;
    // A possible shadow caster the candidate list has not seen, appended to
    // it rather than rebuilding it; clutter and spell kits never cast.
    if (!mdlRef.isGroundDetail && !mdlRef.isSpellEffect) noteShadowCandidate(idx);
    // Track special instances for fast-path iteration
    if (mdlRef.isSmoke) {
        smokeInstanceIndices_.push_back(idx);
    }
    if (mdlRef.isInstancePortal) {
        portalInstanceIndices_.push_back(idx);
    }
    if (!mdlRef.particleEmitters.empty()) {
        particleInstanceIndices_.push_back(idx);
    }
    if (mdlRef.hasAnimation && !mdlRef.disableAnimation) {
        animatedInstanceIndices_.push_back(idx);
    } else if (!mdlRef.particleEmitters.empty()) {
        particleOnlyInstanceIndices_.push_back(idx);
    }
    if (mdlRef.isBoat) boatInstanceIndices_.push_back(idx);
    if (modelGivesLight(mdlRef)) lightInstanceIndices_.push_back(idx);
    instanceIndexById[instance.id] = idx;
    // The grid is for collision queries, and ground clutter never collides
    // (collisionNoBlock, as in the stock client); see m2GridHolds.
    if (m2GridHolds(instance))
        insertBounds(spatialGrid, instance.worldBoundsMin, instance.worldBoundsMax, instance.id);

    return instance.id;
}

uint32_t M2Renderer::createInstanceWithMatrix(uint32_t modelId, const glm::mat4& modelMatrix,
                                                const glm::vec3& position) {
    // Reject NaN inputs at the boundary. position feeds the dedup hash
    // (std::round of NaN is implementation-defined); the matrix goes
    // straight to the GPU UBO and would crash validation.
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) {
        return 0;
    }
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            if (!std::isfinite(modelMatrix[c][r])) return 0;
    if (models.find(modelId) == models.end()) {
        LOG_WARNING("Cannot create instance: model ", modelId, " not loaded");
        return 0;
    }
    modelUnusedSince_.erase(modelId);

    // Deduplicate: O(1) hash lookup
    {
        DedupKey dk{.modelId = modelId,
                    .qx = static_cast<int32_t>(std::round(position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(position.z * 10.0f))};
        auto dit = instanceDedupMap_.find(dk);
        if (dit != instanceDedupMap_.end()) {
            return dit->second;
        }
    }

    M2Instance instance;
    instance.id = nextInstanceId++;
    instance.modelId = modelId;
    instance.position = position;  // Used for frustum culling
    instance.rotation = glm::vec3(0.0f);
    instance.scale = 1.0f;
    instance.modelMatrix = modelMatrix;
    instance.invModelMatrix = glm::inverse(modelMatrix);
    glm::vec3 localMin, localMax;
    getTightCollisionBounds(models[modelId], localMin, localMax);
    transformAABB(instance.modelMatrix, localMin, localMax, instance.worldBoundsMin, instance.worldBoundsMax);
    // Cache model flags on instance to avoid per-frame hash lookups
    const auto& mdl2 = models[modelId];
    instance.cachedHasAnimation = mdl2.hasAnimation;
    instance.cachedDisableAnimation = mdl2.disableAnimation;
    instance.cachedIsSmoke = mdl2.isSmoke;
    instance.cachedHasParticleEmitters = !mdl2.particleEmitters.empty();
    instance.cachedBoundRadius = mdl2.boundRadius;
    instance.cachedIsGroundDetail = mdl2.isGroundDetail;
    instance.cachedIsInvisibleTrap = mdl2.isInvisibleTrap;
    instance.cachedIsSkyBird = mdl2.isSkyBird;
    instance.cachedIsLightBeam = mdl2.isLightBeam;
    instance.cachedIsTransportDoodad = mdl2.isTransportDoodad;
    instance.cachedIsBoat = mdl2.isBoat;
    instance.cachedIsValid = mdl2.isValid();
    instance.cachedModel = &mdl2;
    instance.recomputeCachedCullFactors();

    // Initialize animation
    if (mdl2.hasAnimation && !mdl2.disableAnimation) {
        seedInstanceAnimation(mdl2, modelId, instance);
    } else {
        // A model with no skeleton can still have particle emitters, and their
        // rate and lifespan tracks are sampled at animTime. Starting every
        // instance at zero puts a courtyard of identical torches in lockstep,
        // so the phase is spread.
        //
        // createInstance above now does the same.
        instance.animTime = randFloat(0.0f, 10000.0f);
        seedUnanimatedSequence(mdl2, instance);
    }

    // Register in dedup map
    {
        DedupKey dk{.modelId = modelId,
                    .qx = static_cast<int32_t>(std::round(position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(position.z * 10.0f))};
        instanceDedupMap_[dk] = instance.id;
    }

    // WOWEE_M2_CENSUS: what the model is drawn at, beside what it was
    // authored at. The load-time census gives the authored height; a game
    // object's scale comes from the server and is only known here.
    censusInstance(instance);
    instances.push_back(instance);
    size_t idx = instances.size() - 1;
    if (!mdl2.isGroundDetail && !mdl2.isSpellEffect) noteShadowCandidate(idx);
    if (mdl2.isSmoke) {
        smokeInstanceIndices_.push_back(idx);
    }
    if (!mdl2.particleEmitters.empty()) {
        particleInstanceIndices_.push_back(idx);
    }
    if (mdl2.hasAnimation && !mdl2.disableAnimation) {
        animatedInstanceIndices_.push_back(idx);
    } else if (!mdl2.particleEmitters.empty()) {
        particleOnlyInstanceIndices_.push_back(idx);
    }
    if (mdl2.isBoat) boatInstanceIndices_.push_back(idx);
    if (modelGivesLight(mdl2)) lightInstanceIndices_.push_back(idx);
    instanceIndexById[instance.id] = idx;
    if (m2GridHolds(instance))
        insertBounds(spatialGrid, instance.worldBoundsMin, instance.worldBoundsMax, instance.id);

    return instance.id;
}

// WOWEE_SKY_M2_MAX_BATCH=<n> draws only the sky model's first n layers.
//
// Thirty-four of them, and every measurement says the set drawn is the same
// every frame - so if the flicker is one layer's doing, bisecting the count
// finds it in about five runs. That has worked three times on this fault where
// reading the code has worked none.
static bool skyBatchAllowed(bool skyMode, std::size_t index) {
    if (!skyMode) return true;
    static const int maxBatch = [] {
        const char* set = std::getenv("WOWEE_SKY_M2_MAX_BATCH");
        return set ? std::atoi(set) : -1;
    }();
    return maxBatch < 0 || static_cast<int>(index) < maxBatch;
}

void M2Renderer::update(float deltaTime, const glm::vec3& cameraPos, const glm::mat4& viewProjection) {
    ZoneScopedN("M2Renderer::update");
    static const bool updateProfile = core::envFlagEnabled("WOWEE_FRAME_PROFILE", false);
    using profClock = std::chrono::steady_clock;
    const profClock::time_point profStart = updateProfile ? profClock::now() : profClock::time_point{};
    const bool rebuiltIndex = spatialIndexDirty_;  // for the spike line
    if (spatialIndexDirty_) {
        rebuildSpatialIndex();
    }

    float dtMs = deltaTime * 1000.0f;

    // Cache camera state for frustum-culling bone computation
    cachedCamPos_ = cameraPos;
    // Never past the ground. The density constants are how far models are
    // worth drawing, not how far there is anything to draw them on: the
    // terrain and the WMOs stop at the view distance itself, so a doodad
    // beyond it is a tree standing on nothing.
    const float maxRenderDistance = std::min(
        cappedViewDistance(),
        viewDistanceScale_ *
            ((instances.size() > rendering::M2_HIGH_DENSITY_INSTANCE_THRESHOLD)
                 ? rendering::M2_MAX_RENDER_DISTANCE_HIGH_DENSITY
                 : rendering::M2_MAX_RENDER_DISTANCE_LOW_DENSITY));
    cachedMaxRenderDistSq_ = maxRenderDistance * maxRenderDistance;

    // Build frustum for culling bones
    Frustum updateFrustum;
    updateFrustum.extractFromMatrix(viewProjection);
    cachedFrustum_ = updateFrustum;

    // --- Smoke particle spawning (only iterate tracked smoke instances) ---
    std::uniform_real_distribution<float> distXY(rendering::SMOKE_OFFSET_XY_MIN, rendering::SMOKE_OFFSET_XY_MAX);
    std::uniform_real_distribution<float> distVelXY(-0.3f, 0.3f);
    std::uniform_real_distribution<float> distVelZ(rendering::SMOKE_VEL_Z_MIN, rendering::SMOKE_VEL_Z_MAX);
    std::uniform_real_distribution<float> distLife(rendering::SMOKE_LIFETIME_MIN, rendering::SMOKE_LIFETIME_MAX);
    std::uniform_real_distribution<float> distDrift(-0.2f, 0.2f);

    smokeEmitAccum += deltaTime;
    constexpr float emitInterval = kSmokeEmitInterval;  // 48 particles per second per emitter

    if (smokeEmitAccum >= emitInterval &&
        static_cast<int>(smokeParticles.size()) < MAX_SMOKE_PARTICLES) {
        for (size_t si : smokeInstanceIndices_) {
            if (si >= instances.size()) continue;
            auto& instance = instances[si];

            glm::vec3 emitWorld = glm::vec3(instance.modelMatrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            bool spark = (smokeRng() % rendering::SPARK_PROBABILITY_DENOM == 0);

            SmokeParticle p;
            p.position = emitWorld + glm::vec3(distXY(smokeRng), distXY(smokeRng), 0.0f);
            if (spark) {
                p.velocity = glm::vec3(distVelXY(smokeRng) * 2.0f, distVelXY(smokeRng) * 2.0f, distVelZ(smokeRng) * 1.5f);
                p.maxLife = rendering::SPARK_LIFE_BASE + static_cast<float>(smokeRng() % 100) / 100.0f * rendering::SPARK_LIFE_RANGE;
                p.size = 0.5f;
                p.isSpark = 1.0f;
            } else {
                p.velocity = glm::vec3(distVelXY(smokeRng), distVelXY(smokeRng), distVelZ(smokeRng));
                p.maxLife = distLife(smokeRng);
                p.size = 1.0f;
                p.isSpark = 0.0f;
            }
            p.life = 0.0f;
            p.instanceId = instance.id;
            smokeParticles.push_back(p);
            if (static_cast<int>(smokeParticles.size()) >= MAX_SMOKE_PARTICLES) break;
        }
        smokeEmitAccum = 0.0f;
    }

    // --- Update existing smoke particles (swap-and-pop for O(1) removal) ---
    for (size_t i = 0; i < smokeParticles.size(); ) {
        auto& p = smokeParticles[i];
        p.life += deltaTime;
        if (p.life >= p.maxLife) {
            smokeParticles[i] = smokeParticles.back();
            smokeParticles.pop_back();
            continue;
        }
        p.position += p.velocity * deltaTime;
        p.velocity.z *= rendering::SMOKE_Z_VEL_DAMPING;  // Slight deceleration
        p.velocity.x += distDrift(smokeRng) * deltaTime;
        p.velocity.y += distDrift(smokeRng) * deltaTime;
        // Grow from 1.0 to 3.5 over lifetime
        float t = p.life / p.maxLife;
        p.size = rendering::SMOKE_SIZE_START + t * rendering::SMOKE_SIZE_GROWTH;
        ++i;
    }

    // Instance portals used to be spun here about world Z, overwriting the
    // placement yaw - which turned the vortex edge-on twice a lap. The spin
    // is authored in the model (two bones turning about its X axis, carrying
    // the emitters), so they now animate like any other doodad.

    // --- Normal M2 animation update ---
    // Advance animTime for ALL instances (needed for texture UV animation on static doodads).
    // This is a tight loop touching only one float per instance - no hash lookups.
    //
    // An instance the bone loop below does not step is wrapped here, at the
    // length of its own sequence. A texture track is sampled against this
    // time and holds its last key once the time runs past the end, so a
    // waterfall with no bones of its own scrolled once after it loaded and
    // then stood still, and one with emitters - wrapped at a fixed 3.3 s
    // below, whatever its sequence - stopped for the rest of each lap.
    sceneTimeMs_ += dtMs;
    // The clocks used to advance here for every instance, 70k of them a
    // frame; see advanceClock. Only the boats need a look every frame.
    for (size_t bi : boatInstanceIndices_) {
        if (bi >= instances.size()) continue;
        auto& instance = instances[bi];
        if (instance.cachedIsBoat && instance.afloat < 0 && waterHeightAt_) {
            instance.afloatRecheck -= deltaTime;
            if (instance.afloatRecheck <= 0.0f) {
                instance.afloatRecheck = 1.0f;
                const auto water = waterHeightAt_(instance.position.x, instance.position.y,
                                                  instance.position.z);
                if (water && std::abs(*water - instance.position.z) < 1.5f) {
                    instance.afloat = 1;
                } else if (++instance.afloatTries >= 15) {
                    instance.afloat = 0;
                }
            }
        }
    }

    // The sky model's clock, when this is the renderer that draws one.
    //
    // A report of the sky "playing an animation that brightens and dims, faster
    // when walking" survived three fixes to what chooses the sky, and the
    // lighting diagnostic then showed every input to it holding still while it
    // happened - the zone, the volumes, the model and the target colour. So
    // what is moving is this, and the two things worth telling apart are
    // whether the instance is being rebuilt (animTime back to zero) and
    // whether the clock runs at wall speed (animTime should advance by dtMs and
    // by nothing else).
    //
    // Only this renderer has skyMode_, and only on a change worth seeing, so it
    // is quiet unless asked for with WOWEE_LOG_LEVEL=info.
    if (skyMode_ && !instances.empty()) {
        const auto& sky = instances.front();
        const bool restarted = sky.animTime < skyDiagAnimTime_;
        if (restarted || sky.id != skyDiagInstanceId_ ||
            sky.animTime - skyDiagAnimTime_ > 1000.0f) {
            LOG_INFO("skyM2: instance=", sky.id, " instances=", instances.size(),
                     " animTime=", sky.animTime, " duration=", sky.animDuration,
                     " gsTime=", sky.globalSequenceTime, " dtMs=", dtMs,
                     restarted ? " RESTARTED" : "");
            skyDiagInstanceId_ = sky.id;
            skyDiagAnimTime_ = sky.animTime;
        }
    }
    // Wrap animTime for particle-only instances so emission rate tracks keep looping.
    // 3333ms chosen as a safe wrap period: long enough to cover the longest known M2
    // particle emission cycle (~3s for torch/campfire effects) while preventing float
    // precision loss that accumulates over hours of runtime.
    static constexpr float kParticleWrapMs = 3333.0f;
    // Only for those with no sequence length to wrap at; the rest were
    // wrapped at their own above.
    for (size_t idx : particleOnlyInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& instance = instances[idx];
        advanceClock(instance);
        if (instance.animDuration > 0.0f) continue;
        // Use iterative subtraction instead of fmod() to preserve precision
        while (instance.animTime > kParticleWrapMs) {
            instance.animTime -= kParticleWrapMs;
        }
    }

    const profClock::time_point profAnimStart = updateProfile ? profClock::now() : profClock::time_point{};
    boneWorkIndices_.clear();
    boneWorkIndices_.reserve(animatedInstanceIndices_.size());

    // Update animated instances (full animation state + bone computation culling)
    // Note: animTime was already advanced by dtMs in the global loop above.
    // Here we apply the speed factor: subtract the base dtMs and add dtMs*speed.
    // Ground clutter stops being stepped once it is past the distance it draws
    // at. There are hundreds of tufts to a tile and each one plays a sequence of
    // its own, so this list is mostly grass that nothing can see; the sequences
    // loop, so one that resumes from a stale time is indistinguishable from one
    // that never stopped.
    const float clutterAnimCutoffSq = (groundDetailMaxDistance_ > 0.0f)
        ? (groundDetailMaxDistance_ * groundDetailMaxDistance_) : 0.0f;

    for (size_t idx : animatedInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& instance = instances[idx];
        advanceClock(instance);

        if (clutterAnimCutoffSq > 0.0f && instance.cachedIsGroundDetail) {
            const glm::vec3 toCam = instance.position - cachedCamPos_;
            if (glm::dot(toCam, toCam) > clutterAnimCutoffSq) continue;
        }

        instance.animTime += dtMs * (instance.animSpeed - 1.0f);

        // For animation looping/variation, we need the actual model data.
        if (!instance.cachedModel) continue;
        const M2ModelGPU& model = *instance.cachedModel;

        // The reversing clock, kept beside the ordinary one rather than in
        // place of it: animTime keeps looping for the rest of the skeleton.
        if (model.pingPongAnim) {
            const float dur = std::max(1.0f, instance.animDuration);
            instance.animTimeAlt += dtMs * instance.animDir;
            if (instance.animTimeAlt >= dur) {
                instance.animTimeAlt = dur;
                instance.animDir = -1.0f;
            } else if (instance.animTimeAlt <= 0.0f) {
                instance.animTimeAlt = 0.0f;
                instance.animDir = 1.0f;
            }
        }

        // Cross-fade clock: the sequence being faded out keeps playing until
        // the fade is done, holding its last frame if it runs out (a finished
        // one-shot must not jump back to its first). See beginM2SequenceBlend.
        if (instance.blendDurationMs > 0.0f) {
            instance.blendElapsedMs += dtMs;
            if (instance.blendElapsedMs >= instance.blendDurationMs ||
                instance.blendFromSequence < 0 ||
                instance.blendFromSequence >= static_cast<int>(model.sequences.size())) {
                instance.blendDurationMs = 0.0f;
            } else {
                instance.blendFromTime += dtMs;
                const float fromDur =
                    static_cast<float>(model.sequences[instance.blendFromSequence].duration);
                if (fromDur > 0.0f && instance.blendFromTime > fromDur) {
                    instance.blendFromTime = fromDur;
                }
            }
        }

        // Validate sequence index
        if (instance.currentSequenceIndex < 0 ||
            instance.currentSequenceIndex >= static_cast<int>(model.sequences.size())) {
            instance.currentSequenceIndex = 0;
            if (!model.sequences.empty()) {
                instance.animDuration = static_cast<float>(model.sequences[0].duration);
            }
        }

        // Handle animation looping / variation transitions
        if (instance.animDuration <= 0.0f && instance.cachedHasParticleEmitters) {
            instance.animDuration = rendering::M2_DEFAULT_PARTICLE_ANIM_MS;
        }
        if (instance.animDuration > 0.0f && instance.animTime >= instance.animDuration) {
            if (instance.holdAtEnd && instance.thenLoopSequence >= 0 &&
                instance.thenLoopSequence < static_cast<int>(model.sequences.size())) {
                // Open ran once; Opened loops from here (Close/Closed,
                // Destroy/Destroyed). thenLoopSequence stays set while it
                // loops so idle variations keep out of it.
                instance.holdAtEnd = false;
                beginM2SequenceBlend(model, instance, instance.thenLoopSequence);
                instance.currentSequenceIndex = instance.thenLoopSequence;
                instance.animDuration =
                    static_cast<float>(model.sequences[instance.thenLoopSequence].duration);
                instance.animTime = 0.0f;
                instance.animSpeed = 1.0f;
            } else if (instance.holdAtEnd) {
                // Stay on the last frame. A door's open sequence ends with the
                // door open, and that is the pose the server is describing.
                instance.animTime = instance.animDuration;
                instance.animSpeed = 0.0f;
            } else if (instance.playingVariation) {
                instance.playingVariation = false;
                beginM2SequenceBlend(model, instance, instance.idleSequenceIndex);
                instance.currentSequenceIndex = instance.idleSequenceIndex;
                if (instance.idleSequenceIndex < static_cast<int>(model.sequences.size())) {
                    instance.animDuration = static_cast<float>(model.sequences[instance.idleSequenceIndex].duration);
                }
                instance.animTime = 0.0f;
                instance.variationTimer = randFloat(rendering::M2_LOOP_VARIATION_TIMER_MIN_MS, rendering::M2_LOOP_VARIATION_TIMER_MAX_MS);
            } else {
                // Use iterative subtraction instead of fmod() to preserve precision
                float duration = std::max(1.0f, instance.animDuration);
                while (instance.animTime >= duration) {
                    instance.animTime -= duration;
                }
                // The next lap, as the stock client does it: the laps the
                // variation still owes (its replay range), else a variation
                // of the same animation rolled by the sequences' frequency
                // weights. Not inside a state loop (Opened, Destroyed).
                if (instance.thenLoopSequence < 0 &&
                    instance.currentSequenceIndex >= 0 &&
                    instance.currentSequenceIndex < static_cast<int>(model.sequences.size())) {
                    const uint32_t animId = model.sequences[instance.currentSequenceIndex].id;
                    if (instance.variationLapsLeft > 0) {
                        --instance.variationLapsLeft;
                    } else if (anim::hasSequenceVariations(model.sequences, animId)) {
                        int next = anim::pickSequenceVariation(model.sequences, model.bones, animId);
                        if (next >= 0) {
                            instance.variationLapsLeft = anim::pickSequenceExtraLaps(model.sequences[next]);
                            next = anim::resolveSequenceAlias(model.sequences, next);
                        }
                        if (next >= 0 && next < static_cast<int>(model.sequences.size()) &&
                            next != instance.currentSequenceIndex) {
                            beginM2SequenceBlend(model, instance, next);
                            instance.currentSequenceIndex = next;
                            instance.animDuration = static_cast<float>(model.sequences[next].duration);
                            const float nextDur = std::max(1.0f, instance.animDuration);
                            while (instance.animTime >= nextDur) instance.animTime -= nextDur;
                        }
                    }
                }
            }
        }

        // Already in the one pose this sequence has. The clock and the
        // variation timer above still ran, so the sequence changes when it
        // would have, and that change is what poses it again.
        if (model.staticPose &&
            instance.posedSequence == instance.currentSequenceIndex &&
            !instance.boneMatrices.empty()) {
            ++updateProf_.staticPoseSkips;
            continue;
        }

        // Frustum + distance cull: skip expensive bone computation for off-screen instances.
        // Both effectiveMaxDistSq and paddedRadius are precomputed per instance in
        // recomputeCachedCullFactors(); we only need the per-frame distance and frustum test.
        glm::vec3 toCam = instance.position - cachedCamPos_;
        float distSq = glm::dot(toCam, toCam);
        float effectiveMaxDistSq = rendering::m2InstanceMaxDistSq(
            cachedMaxRenderDistSq_, instance.cachedEffectiveMaxDistSqFactor,
            false, 0.0f, cappedViewDistance(),
            instance.cachedIsGroundDetail, groundDetailMaxDistance_);
        if (instance.cachedIsSkyBird) {
            constexpr float kBirdMaxDistSq =
                rendering::M2_SKY_BIRD_MAX_RENDER_DISTANCE *
                rendering::M2_SKY_BIRD_MAX_RENDER_DISTANCE;
            effectiveMaxDistSq = std::min(effectiveMaxDistSq, kBirdMaxDistSq);
        }
        if (distSq > effectiveMaxDistSq) continue;
        float paddedRadius = instance.cachedPaddedRadius;
        if (paddedRadius > 0.0f && !updateFrustum.intersectsSphere(instance.cachedCullCenter, paddedRadius)) continue;

        // LOD 3 skip: models beyond 150 units use the lowest LOD mesh which has
        // no visible skeletal animation.  Keep their last-computed bone matrices
        // (always valid - seeded on spawn) and avoid the expensive per-bone work.
        // Sky birds, light beams, and ship machinery are exempt: their visible
        // motion is baked entirely into bone animation.
        constexpr float kLOD3DistSq = rendering::M2_LOD3_DISTANCE * rendering::M2_LOD3_DISTANCE;
        const bool needsDistantBones = instance.cachedIsSkyBird || instance.cachedIsLightBeam ||
                                       instance.cachedIsTransportDoodad;
        if (distSq > kLOD3DistSq && !needsDistantBones) continue;

        // Distance-based frame skipping: update distant bones less frequently
        uint32_t boneInterval = 1;
        if (!needsDistantBones) {
            if (distSq > rendering::M2_BONE_SKIP_DIST_FAR * rendering::M2_BONE_SKIP_DIST_FAR) boneInterval = 4;
            else if (distSq > rendering::M2_BONE_SKIP_DIST_MID * rendering::M2_BONE_SKIP_DIST_MID) boneInterval = 2;
        }
        instance.frameSkipCounter++;
        if ((instance.frameSkipCounter % boneInterval) != 0) continue;

        if (model.staticPose) {
            instance.posedSequence = static_cast<int16_t>(instance.currentSequenceIndex);
        }
        boneWorkIndices_.push_back(idx);
    }
    const profClock::time_point profBonesStart = updateProfile ? profClock::now() : profClock::time_point{};

    // Compute bone matrices (expensive, parallel if enough work)
    const size_t animCount = boneWorkIndices_.size();
    if (animCount > 0) {
        static const size_t minParallelAnimInstances = std::max<size_t>(
            8, envSizeOrDefault("WOWEE_M2_ANIM_MT_MIN", 96));
        if (animCount < minParallelAnimInstances || numAnimThreads_ <= 1) {
            // Sequential - not enough work to justify thread overhead
            for (size_t i : boneWorkIndices_) {
                if (i >= instances.size()) continue;
                auto& inst = instances[i];
                if (!inst.cachedModel) continue;
                computeBoneMatrices(*inst.cachedModel, inst, &cachedCamPos_);
            }
        } else {
            // Parallel - dispatch across worker threads
            static const size_t minAnimWorkPerThread = std::max<size_t>(
                16, envSizeOrDefault("WOWEE_M2_ANIM_WORK_PER_THREAD", 64));
            const size_t maxUsefulThreads = std::max<size_t>(
                1, (animCount + minAnimWorkPerThread - 1) / minAnimWorkPerThread);
            const size_t numThreads = std::min(static_cast<size_t>(numAnimThreads_), maxUsefulThreads);
            if (numThreads <= 1) {
                for (size_t i : boneWorkIndices_) {
                    if (i >= instances.size()) continue;
                    auto& inst = instances[i];
                    if (!inst.cachedModel) continue;
                    computeBoneMatrices(*inst.cachedModel, inst, &cachedCamPos_);
                }
            } else {
                const size_t chunkSize = animCount / numThreads;
                const size_t remainder = animCount % numThreads;

                auto processRange = [this](size_t begin, size_t end) {
                    for (size_t j = begin; j < end; ++j) {
                        size_t idx = boneWorkIndices_[j];
                        if (idx >= instances.size()) continue;
                        auto& inst = instances[idx];
                        if (!inst.cachedModel) continue;
                        computeBoneMatrices(*inst.cachedModel, inst, &cachedCamPos_);
                    }
                };

                // Reuse persistent futures vector to avoid allocation
                animFutures_.clear();
                if (animFutures_.capacity() < numThreads) {
                    animFutures_.reserve(numThreads);
                }

                // Dispatch all but the last chunk to the shared pool; process the
                // last chunk on this thread so this call always makes progress even
                // when it itself runs on a pool worker (see ThreadPool docs).
                size_t start = 0;
                for (size_t t = 0; t + 1 < numThreads; ++t) {
                    size_t end = start + chunkSize + (t < remainder ? 1 : 0);
                    animFutures_.push_back(core::ThreadPool::frameWorkers().submit(
                        [processRange, start, end]() { processRange(start, end); }));
                    start = end;
                }
                processRange(start, animCount);

                for (auto& f : animFutures_) {
                    f.get();
                }
            }
        }
    }

    const profClock::time_point profParticlesStart = updateProfile ? profClock::now() : profClock::time_point{};
    // Particle update (sequential - uses RNG, not thread-safe)
    // Only iterate instances that have particle emitters (pre-built list).
    //
    // An emitter the particle pass will not draw - out of range, or outside
    // the frustum padded as that pass pads it - is dormant: its particles are
    // dropped and it neither emits nor steps. Stepping them anyway (every
    // frame within 80 yards, one in four beyond) kept ~110k particles alive
    // for ~20k drawn, and was most of this loop's 2-3 ms.
    //
    // Its clock keeps running (advanceClock above), and the step that wakes
    // it emits over a window as long as each emitter's lifespan, every
    // particle aged by when in that window it was born (emitParticles). So
    // the fire the camera turns back to is already burning, at the standing
    // population and spread of ages it would have had - and the wake happens
    // in the update of the same frame, against the same frustum, before the
    // pass that draws it. A wider pad to fall asleep than to wake keeps an
    // emitter at the edge from being dropped and rebuilt frame after frame.
    //
    // Spell effects and emitters told to stop are left as they were, stepped
    // whenever in range: a kit's end is timed by its live particles.
    ++particleFrame_;
    constexpr float kParticleViewPad = 30.0f;   // = renderM2Particles' pad
    constexpr float kParticleSleepPad = 60.0f;  // wake at 30, sleep beyond 60
    constexpr float kParticleWarmWindow = 3600.0f;  // "since enhanced": capped at each lifespan
    uint64_t particlesFull = 0, particlesDormant = 0, particlesLive = 0;
    for (size_t idx : particleInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& instance = instances[idx];
        advanceClock(instance);
        if (!instance.cachedModel) continue;
        const M2ModelGPU& pmodel = *instance.cachedModel;
        glm::vec3 toCam = instance.position - cachedCamPos_;
        float distSq = glm::dot(toCam, toCam);
        const bool inRange = distSq <= cachedMaxRenderDistSq_;
        const bool keepStepping = pmodel.isSpellEffect || instance.emissionStopped;
        if (keepStepping) {
            if (!inRange) continue;
            instance.particlesDormant = false;
            updateParticles(instance, deltaTime);
            emitParticles(instance, pmodel, deltaTime);
            ++particlesFull;
        } else {
            const bool inView = inRange &&
                (instance.cachedPaddedRadius <= 0.0f ||
                 updateFrustum.intersectsSphere(instance.cachedCullCenter,
                     instance.cachedPaddedRadius +
                     (instance.particlesDormant ? kParticleViewPad : kParticleSleepPad)));
            if (!inView) {
                if (!instance.particlesDormant) {
                    instance.particles.clear();  // keeps capacity for the wake
                    instance.particlesDormant = true;
                    instance.particleDtDebt = 0.0f;
                }
                instance.particleDtDebt += deltaTime;
                ++particlesDormant;
                // Out of range is as before: no ribbon step either.
                if (!inRange) continue;
            } else if (instance.particlesDormant) {
                instance.particlesDormant = false;
                instance.particleDtDebt = 0.0f;
                emitParticles(instance, pmodel, kParticleWarmWindow);
                ++particlesFull;
            } else {
                // Step the living, then add what was born during the step,
                // each already aged by its share of it.
                updateParticles(instance, deltaTime);
                emitParticles(instance, pmodel, deltaTime);
                ++particlesFull;
            }
        }
        particlesLive += instance.particles.size();
        // Ribbons every frame: a trail stepped in leaps lays its edges down in
        // clumps on one spot, and there are few of them.
        if (!pmodel.ribbonEmitters.empty()) {
            updateRibbons(instance, pmodel, deltaTime);
        }
    }
    updateProf_.particleFull += particlesFull;
    updateProf_.particleDormant += particlesDormant;
    updateProf_.particleLive += particlesLive;

    // Where update/m2 join goes. Reported by the world renderer only - the
    // sky has an M2Renderer of its own and a handful of instances.
    if (updateProfile && !skyMode_) {
        const auto profEnd = profClock::now();
        const auto ms = [](profClock::time_point a, profClock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        auto& p = updateProf_;
        p.preMs += ms(profStart, profAnimStart);
        p.animLoopMs += ms(profAnimStart, profBonesStart);
        p.bonesMs += ms(profBonesStart, profParticlesStart);
        p.particlesMs += ms(profParticlesStart, profEnd);
        p.animatedListed += animatedInstanceIndices_.size();
        p.boneWork += boneWorkIndices_.size();
        p.particleListed += particleInstanceIndices_.size();
        // Collision queries since the renderer's per-frame reset - the camera
        // controller's floor probes, wall sweeps and camera rays, mostly.
        p.queryMs += queryTimeMs.load();
        p.queryCalls += queryCallCount.load();
        ++p.frames;
        // A spike in update/m2 join, named by stage as it happens; the 10 s
        // average below cannot say which stage the worst frame was in.
        {
            const double frameTotal = ms(profStart, profEnd);
            if (frameTotal > 12.0) {
                static profClock::time_point lastSpikeLog{};
                if (profEnd - lastSpikeLog > std::chrono::seconds(1)) {
                    lastSpikeLog = profEnd;
                    LOG_WARNING("m2 update spike: ", frameTotal, "ms - pre ",
                                ms(profStart, profAnimStart),
                                rebuiltIndex ? "ms (spatial index rebuilt)" : "ms",
                                ", anim loop ", ms(profAnimStart, profBonesStart),
                                "ms, bones ", ms(profBonesStart, profParticlesStart),
                                "ms over ", boneWorkIndices_.size(),
                                ", particles ", ms(profParticlesStart, profEnd),
                                "ms; ", instances.size(), " instances");
                }
            }
        }
        if (p.since == profClock::time_point{}) p.since = profEnd;
        if (profEnd - p.since > std::chrono::seconds(10) && p.frames > 0) {
            const double n = p.frames;
            LOG_WARNING("  m2 update: pre ", p.preMs / n, "ms, anim loop ", p.animLoopMs / n,
                        "ms, bones ", p.bonesMs / n, "ms, particles ", p.particlesMs / n,
                        "ms per frame; ", p.animatedListed / p.frames, " animated listed, ",
                        p.boneWork / p.frames, " posed, ", p.staticPoseSkips / p.frames,
                        " static-pose skips, ", p.particleListed / p.frames,
                        " with emitters, of ", instances.size(), " instances");
            LOG_WARNING("  m2 shadow: ", p.shadowCandidates / p.frames, " candidates, ",
                        p.shadowMovers / p.frames, " movers, ",
                        p.shadowCasters / p.frames, " casters drawn per frame; candidate list rebuilt ",
                        p.shadowRebuilds, " times in ", p.frames, " frames");
            LOG_WARNING("  m2 collision queries: ", p.queryMs / n, "ms in ",
                        p.queryCalls / p.frames, " calls per frame (camera, movement)");
            LOG_WARNING("  m2 particles: ", p.particleFull / p.frames, " emitters stepped (in view), ",
                        p.particleDormant / p.frames, " dormant (off screen or out of range, none live), ",
                        p.particleLive / p.frames, " live particles, ",
                        particleVertsDrawn_.load(std::memory_order_relaxed),
                        " drawn last frame");
            // Which models those emitters belong to: the six most numerous -
            // a count in the thousands is a placed doodad (lamps, fireflies,
            // lava), not spell effects - and the six with the most particles
            // alive, which is where the step's time goes. Once per window,
            // over the short list.
            {
                struct EmitterCensus { uint32_t count = 0, inRange = 0, awake = 0; size_t live = 0; };
                std::unordered_map<const M2ModelGPU*, EmitterCensus> census;
                for (size_t idx : particleInstanceIndices_) {
                    if (idx >= instances.size() || !instances[idx].cachedModel) continue;
                    const auto& inst = instances[idx];
                    auto& c = census[inst.cachedModel];
                    ++c.count;
                    const glm::vec3 d = inst.position - cachedCamPos_;
                    if (glm::dot(d, d) <= cachedMaxRenderDistSq_) ++c.inRange;
                    if (!inst.particlesDormant) ++c.awake;
                    c.live += inst.particles.size();
                }
                std::vector<std::pair<const M2ModelGPU*, EmitterCensus>> top(census.begin(), census.end());
                const size_t keep = std::min<size_t>(6, top.size());
                const auto say = [](const char* tag, const M2ModelGPU& m, const EmitterCensus& c) {
                    float maxLife = 0.0f;
                    for (const auto& em : m.particleEmitters)
                        for (const auto& s : em.lifespan.sequences)
                            for (float v : s.floatValues) maxLife = std::max(maxLife, v);
                    LOG_WARNING(tag, c.count, " x '", m.name, "' (",
                                c.inRange, " in range, ", c.awake, " awake, ", c.live,
                                " live particles, ", m.particleEmitters.size(), " emitters, longest life ",
                                maxLife, " s, ", m.ribbonEmitters.size(), " ribbons",
                                m.isGroundDetail ? ", ground clutter" : "",
                                m.isFireflyEffect ? ", fireflies" : "", ")");
                };
                std::partial_sort(top.begin(), top.begin() + keep, top.end(),
                                  [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
                for (size_t k = 0; k < keep; ++k) say("    emitters: ", *top[k].first, top[k].second);
                std::partial_sort(top.begin(), top.begin() + keep, top.end(),
                                  [](const auto& a, const auto& b) { return a.second.live > b.second.live; });
                for (size_t k = 0; k < keep; ++k) {
                    if (top[k].second.live == 0) break;
                    say("    by live particles: ", *top[k].first, top[k].second);
                }
            }
            p = UpdateProfile{};
            p.since = profEnd;
        }
    }
}

// Diagnostic: WOWEE_M2_NO_SKINNING=1 renders every M2 in its bind pose by
// telling the shader to ignore bones, separating a skinning artifact from one
// drawn by the particle or ribbon systems.
static const bool kM2NoSkinning = envFlagEnabled("WOWEE_M2_NO_SKINNING");

void M2Renderer::prepareRender(uint32_t frameIndex, const Camera& camera) {
    if (!initialized_ || instances.empty()) return;
    (void)camera;  // reserved for future frustum-based culling

    // --- Mega bone SSBO: assign ranges and upload all animated instance bones ---
    // Offset 0 is reserved as the identity/no-bones sentinel; animated instances
    // are packed after it at their own bone count, so a 300-bone creature gets
    // all 300 matrices instead of being truncated into a fixed stride and
    // reading into its neighbour's range.
    uint32_t nextOffset = 1;
    for (size_t idx : animatedInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& instance = instances[idx];

        if (instance.boneMatrices.empty()) {
            instance.megaBoneOffset = 0;  // Use identity slot
            continue;
        }

        // A bone that has left the model behind.
        //
        // The vertex shader skins by these matrices, so one that translates far
        // outside the model's own bounds drags every vertex weighted to it out
        // with it. What that looks like on screen is not a bone problem: it is
        // a long flat triangle with the skin smeared across it, which reads as
        // a missing texture or a stray card. The Elemental Slave's white sheets
        // were reported three times as missing textures, and the texture paths,
        // the display skins, the particle emitters and the ribbons were all
        // measured and found correct before the geometry was suspected.
        //
        // Judged against the other bones, not against the model's origin, and
        // against the model's own bounding radius so a large creature is not
        // accused for being large. Measuring from the origin flags every
        // flying critter: a bird's flight path is authored into its root bone,
        // so the whole skeleton is sixteen yards out and nothing is stretched.
        // What a stretch looks like is one bone leaving the set behind.
        if (instance.cachedModel != nullptr) {
            const M2ModelGPU& m = *instance.cachedModel;
            const glm::vec3 extent = m.boundMax - m.boundMin;
            const float radius = 0.5f * glm::length(extent);
            // Examined once per model, not once per frame: this walks every
            // bone, and there are models with three hundred of them drawn
            // dozens at a time.
            static std::unordered_set<uint32_t> boneRangeChecked;
            if (radius > 0.01f && boneRangeChecked.size() < 64 &&
                boneRangeChecked.insert(instance.modelId).second) {
                glm::vec3 centre(0.0f);
                for (const auto& bone : instance.boneMatrices) centre += glm::vec3(bone[3]);
                centre /= static_cast<float>(instance.boneMatrices.size());

                const float limit = radius * 4.0f;
                for (size_t bi = 0; bi < instance.boneMatrices.size(); ++bi) {
                    const glm::vec3 t(instance.boneMatrices[bi][3]);
                    const float stray = glm::length(t - centre);
                    if (stray <= limit) continue;
                    LOG_WARNING("M2 '", m.name, "' bone ", bi, " of ",
                                instance.boneMatrices.size(), " sits ", stray,
                                " from where the rest of its skeleton is, in a model whose"
                                " bounding radius is ", radius,
                                " - anything weighted to it is drawn stretched");
                    break;
                }
            }
        }

        const uint32_t boneCount = static_cast<uint32_t>(instance.boneMatrices.size());
        if (boneCount > MEGA_BONE_MATRIX_CAPACITY - nextOffset) {
            instance.megaBoneOffset = 0;  // Overflow - use identity
            continue;
        }

        instance.megaBoneOffset = nextOffset;

        // Upload bone matrices to mega buffer - only when they were recomputed
        // since the last upload into this frame's buffer, or the instance's
        // slot moved (animated set changed). Most animated instances are
        // distance/frustum/frame-skip culled and keep their previous bones, so
        // skipping their memcpy avoids megabytes of redundant writes per frame.
        if (megaBoneMapped_[frameIndex] &&
            (instance.bonesDirty[frameIndex] ||
             instance.megaBoneUploadedSlot[frameIndex] != instance.megaBoneOffset)) {
            auto* dst = static_cast<glm::mat4*>(megaBoneMapped_[frameIndex]) + instance.megaBoneOffset;
            memcpy(dst, instance.boneMatrices.data(), boneCount * sizeof(glm::mat4));
            instance.bonesDirty[frameIndex] = false;
            instance.megaBoneUploadedSlot[frameIndex] = instance.megaBoneOffset;
        }

        nextOffset += boneCount;
    }
}

// Dispatch GPU frustum culling compute shader into the primary frame command
// buffer. render() consumes the completed output left in this frame slot from
// its previous use; this dispatch produces results for the slot's next reuse.
void M2Renderer::dispatchCullCompute(VkCommandBuffer cmd, uint32_t frameIndex, const Camera& camera) {
    if (!cullPipeline_ || instances.empty()) return;

    const uint32_t numInstances = std::min(static_cast<uint32_t>(instances.size()), MAX_CULL_INSTANCES);

    // --- Compute per-instance adaptive distances (same formula as old CPU cull) ---
    const float targetRenderDist = viewDistanceScale_ *
        ((instances.size() > 2000) ? 300.0f
         : (instances.size() > 1000) ? 500.0f
                                     : 1000.0f);
    const float shrinkRate = 0.005f;
    const float growRate   = 0.05f;
    float blendRate = (targetRenderDist < smoothedRenderDist_) ? shrinkRate : growRate;
    smoothedRenderDist_ = glm::mix(smoothedRenderDist_, targetRenderDist, blendRate);
    const float maxRenderDistance = smoothedRenderDist_;
    const float maxRenderDistanceSq = maxRenderDistance * maxRenderDistance;
    // The shader rejects on this bound before it ever reads the per-instance
    // distance, so it has to clear the game-object floor as well - otherwise
    // that floor is silently capped at 2x the ambient doodad distance.
    const float maxPossibleDistSq = std::max(
        maxRenderDistanceSq * 4.0f,  // 2x safety margin
        rendering::M2_GAME_OBJECT_MIN_RENDER_DISTANCE *
        rendering::M2_GAME_OBJECT_MIN_RENDER_DISTANCE);

    // --- Upload frustum planes + camera (UBO, binding 0) ---
    const glm::mat4 vp = camera.getProjectionMatrix() * camera.getViewMatrix();
    Frustum frustum;
    frustum.extractFromMatrix(vp);
    const glm::vec3 camPos = camera.getPosition();

    if (cullUniformMapped_[frameIndex]) {
        auto* ubo = static_cast<CullUniformsGPU*>(cullUniformMapped_[frameIndex]);
        for (int i = 0; i < 6; i++) {
            const auto& p = frustum.getPlane(static_cast<Frustum::Side>(i));
            ubo->frustumPlanes[i] = glm::vec4(p.normal, p.distance);
        }
        ubo->cameraPos = glm::vec4(camPos, maxPossibleDistSq);
        ubo->instanceCount = numInstances;

        // HiZ occlusion culling fields
        const bool hizReady = hizSystem_ && hizSystem_->isReady();

        // Auto-disable HiZ when the camera has moved/rotated significantly.
        // Large VP changes make the depth pyramid unreliable because the
        // reprojected screen positions diverge from the actual pyramid data.
        bool hizSafe = hizReady;
        if (hizReady) {
            // Compare current VP against previous VP - Frobenius-style max diff.
            float maxDiff = 0.0f;
            const float* curM  = &vp[0][0];
            const float* prevM = &prevVP_[0][0];
            for (int k = 0; k < 16; ++k)
                maxDiff = std::max(maxDiff, std::abs(curM[k] - prevM[k]));
            // Threshold: typical tracking-camera motion (following a walking
            // character) produces diffs of 0.05–0.25.  A fast rotation or
            // zoom easily exceeds 0.5.  The previous threshold (0.15) caused
            // the HiZ pass to toggle on/off every other frame during normal
            // gameplay, which produced global M2 doodad flicker.
            if (maxDiff > rendering::HIZ_VP_DIFF_THRESHOLD) hizSafe = false;
        }

        ubo->hizEnabled = hizSafe ? 1u : 0u;
        ubo->hizMipLevels = hizReady ? hizSystem_->getMipLevels() : 0u;
        ubo->_pad2 = 0;
        if (hizReady) {
            ubo->hizParams = glm::vec4(
                static_cast<float>(hizSystem_->getPyramidWidth()),
                static_cast<float>(hizSystem_->getPyramidHeight()),
                camera.getNearPlane(),
                0.0f
            );
            ubo->viewProj = vp;
            // Use previous frame's VP for HiZ reprojection - the HiZ pyramid
            // was built from the previous frame's depth, so we must project
            // into the same screen space to sample the correct depths.
            ubo->prevViewProj = prevVP_;
        } else {
            ubo->hizParams = glm::vec4(0.0f);
            ubo->viewProj = glm::mat4(1.0f);
            ubo->prevViewProj = glm::mat4(1.0f);
        }

        // Save current VP for next frame's temporal reprojection
        prevVP_ = vp;
    }

    // Rotate this slot's ID log: the dispatch recorded below replaces the one
    // whose results render() is about to consume, so the IDs it was built from
    // become the readable set.  Done before the upload loop overwrites them, and
    // after the early-out above, so "readable" always describes the last dispatch
    // actually recorded on this slot.
    uint32_t* submittedIds = nullptr;
    if (frameIndex < 2) {
        cullReadableIds_[frameIndex].swap(cullSubmittedIds_[frameIndex]);
        cullSubmittedIds_[frameIndex].resize(numInstances);
        submittedIds = cullSubmittedIds_[frameIndex].data();
    }

    // --- Upload per-instance cull data (SSBO, binding 1) ---
    // The per-instance radius math used to be recomputed here every frame; it's
    // now precomputed once by recomputeCachedCullFactors() since it depends only
    // on static instance state (scale, bound radius, animation/ground flags).
    //
    // Split across the frame workers: 85k entries, each a read of the
    // instance's first cache line and a 32-byte write, was 1.1 ms on the main
    // thread in beginFrame. A dirty-range upload would need the per-frame
    // distance bound moved into the shader and the HiZ bit (which changes
    // with every visibility flip) tracked per slot; ranges get the same
    // result without touching the shaders.
    auto* input = static_cast<CullInstanceGPU*>(cullInputMapped_[frameIndex]);
    const auto uploadRange = [&](uint32_t begin, uint32_t end) {
        if (submittedIds) {
            for (uint32_t i = begin; i < end; i++) submittedIds[i] = instances[i].id;
        }
        if (!input) return;
        for (uint32_t i = begin; i < end; i++) {
            const auto& inst = instances[i];
            float effectiveMaxDistSq = rendering::m2InstanceMaxDistSq(
                maxRenderDistanceSq, inst.cachedEffectiveMaxDistSqFactor,
                inst.isGameObject, rendering::M2_GAME_OBJECT_MIN_RENDER_DISTANCE,
                cappedViewDistance(),
                inst.cachedIsGroundDetail, groundDetailMaxDistance_);
            if (inst.cachedIsSkyBird && inst.cachedHasAnimation && !inst.cachedDisableAnimation) {
                constexpr float kBirdMaxDistSq =
                    rendering::M2_SKY_BIRD_MAX_RENDER_DISTANCE *
                    rendering::M2_SKY_BIRD_MAX_RENDER_DISTANCE;
                effectiveMaxDistSq = std::min(effectiveMaxDistSq, kBirdMaxDistSq);
            }

            uint32_t flags = 0;
            if (inst.cachedIsValid)          flags |= 1u;
            if (inst.cachedIsSmoke)           flags |= 2u;
            if (inst.cachedIsInvisibleTrap)   flags |= 4u;
            // Bit 3: previouslyVisible - the shader runs the HiZ occlusion test
            // ONLY when this bit is set (an object with no depth in last frame's
            // pyramid can't be tested reliably). Hysteresis: keep it set unless
            // culled for 2+ consecutive frames, preventing single-frame false-cull
            // flicker. The counter lives on the instance, so streaming churn can
            // never pair it with a different object's history.
            //
            // Server game objects (mailboxes, chests, ...) opt out of HiZ entirely
            // by never setting this bit: they are small gameplay props that sit
            // flush against walls and doorframes, exactly where the coarse depth
            // pyramid reports false occlusions. Such a false-cull would persist
            // (the prop is then not rendered, so it never regains depth to clear
            // itself) - the "mailbox went invisible in place" report. Frustum +
            // distance culling still bound them; only the unreliable occlusion
            // test is waived.
            if (inst.hizPrevCulledFrames < 2 && !inst.isGameObject)
                flags |= 8u;

            input[i].sphere = glm::vec4(inst.cachedCullCenter, inst.cachedPaddedRadius);
            input[i].effectiveMaxDistSq = effectiveMaxDistSq;
            input[i].flags = flags;
        }
    };
    {
        const uint32_t ranges = numInstances >= 16384
            ? std::min<uint32_t>(
                  static_cast<uint32_t>(core::ThreadPool::frameWorkers().threadCount()) + 1, 8u)
            : 1u;
        if (ranges <= 1) {
            uploadRange(0, numInstances);
        } else {
            const uint32_t per = (numInstances + ranges - 1) / ranges;
            std::vector<std::future<void>> uploadFutures;
            uploadFutures.reserve(ranges - 1);
            for (uint32_t r = 0; r + 1 < ranges; ++r) {
                const uint32_t b = r * per;
                const uint32_t e = std::min(numInstances, b + per);
                uploadFutures.push_back(core::ThreadPool::frameWorkers().submit(
                    [&uploadRange, b, e]() { uploadRange(b, e); }));
            }
            uploadRange(std::min(numInstances, (ranges - 1) * per), numInstances);
            for (auto& f : uploadFutures) f.get();
        }
    }

    // --- Dispatch compute shader ---
    const bool useHiZ = (cullHiZPipeline_ != VK_NULL_HANDLE)
                     && hizSystem_ && hizSystem_->isReady();
    if (useHiZ) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullHiZPipeline_);
        // Set 0: cull UBO + input/output SSBOs
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                cullHiZPipelineLayout_, 0, 1, &cullSet_[frameIndex], 0, nullptr);
        // Set 1: HiZ pyramid sampler
        VkDescriptorSet hizSet = hizSystem_->getDescriptorSet(frameIndex);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                cullHiZPipelineLayout_, 1, 1, &hizSet, 0, nullptr);
    } else {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                cullPipelineLayout_, 0, 1, &cullSet_[frameIndex], 0, nullptr);
    }

    const uint32_t groupCount = (numInstances + 63) / 64;
    vkCmdDispatch(cmd, groupCount, 1, 1);

    // Make writes available to the host after this frame's fence signals. The
    // CPU invalidates and reads them when this frame slot is reused.
    VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_HOST_BIT;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    cmdPipelineBarrier2(cmd, dep);
}

void M2Renderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera) {
    if (instances.empty() || !opaquePipeline_) {
        return;
    }

    // Debug: log once when we start rendering
    static bool loggedOnce = false;
    if (!loggedOnce) {
        loggedOnce = true;
        LOG_INFO("M2 render: ", instances.size(), " instances, ", models.size(), " models");
    }

    // Periodic diagnostic: report render pipeline stats every 10 seconds
    static int diagCounter = 0;
    if (++diagCounter == 600) { // ~10s at 60fps
        diagCounter = 0;
        uint32_t totalValid = 0, totalAnimated = 0, totalBonesReady = 0, totalMegaBoneOk = 0;
        for (const auto& inst : instances) {
            if (inst.cachedIsValid) totalValid++;
            if (inst.cachedHasAnimation && !inst.cachedDisableAnimation) {
                totalAnimated++;
                if (!inst.boneMatrices.empty()) totalBonesReady++;
                if (inst.megaBoneOffset != 0) totalMegaBoneOk++;
            }
        }
        LOG_INFO("M2 diag: total=", instances.size(),
                 " valid=", totalValid,
                 " animated=", totalAnimated,
                 " bonesReady=", totalBonesReady,
                 " megaBoneOk=", totalMegaBoneOk,
                 " visible=", sortedVisible_.size(),
                 " draws=", lastDrawCallCount);
    }

    // Reuse persistent buffers (clear instead of reallocating)
    glowSprites_.clear();

    lastDrawCallCount = 0;
    const float lavaAnimSeconds = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - kLavaAnimStart).count();

    // GPU cull results - dispatchCullCompute() already updated smoothedRenderDist_.
    // Use the cached value (set by dispatchCullCompute or fallback below).
    const uint32_t frameIndex = vkCtx_->getCurrentFrame();
    const uint32_t numInstances = std::min(static_cast<uint32_t>(instances.size()), MAX_CULL_INSTANCES);
    const uint32_t* visibility = static_cast<const uint32_t*>(cullOutputMapped_[frameIndex]);
    const bool gpuCullAvailable = (cullPipeline_ != VK_NULL_HANDLE && visibility != nullptr);

    // Scatter the GPU visibility results back onto the instances they were
    // computed for.  The results belong to the dispatch recorded on this slot
    // ~2 frames ago, so they are keyed by that dispatch's instance IDs, never by
    // the current array index: createInstance() appends and removeInstance()
    // swap-removes, so a respawned game object lands at the volatile tail of the
    // array and would otherwise inherit the verdict of whatever transient object
    // (spell visual, streamed doodad, creature) held that index two frames back
    // - leaving it culled for as long as the churn continued.
    //
    // Instances with no entry in the readable set were created after that
    // dispatch and keep their defaults: visible, and exempt from the HiZ test.
    //
    // hizPrevCulledFrames is a hysteresis counter rather than a binary flag: an
    // object must be culled for 2 consecutive frames before it stops counting as
    // "previously visible", which prevents the 1-frame-on / 1-frame-off
    // oscillation that showed up as doodad flicker near moving characters.
    if (gpuCullAvailable) {
        const auto& ids = cullReadableIds_[frameIndex < 2 ? frameIndex : 0];
        const auto scatterRange = [&](size_t begin, size_t end) {
            for (size_t k = begin; k < end; ++k) {
                // Fast path: with no churn since that dispatch the ordering still
                // matches, so skip the hash lookup.
                M2Instance* inst = nullptr;
                if (k < instances.size() && instances[k].id == ids[k]) {
                    inst = &instances[k];
                } else {
                    auto idxIt = instanceIndexById.find(ids[k]);
                    if (idxIt == instanceIndexById.end() || idxIt->second >= instances.size())
                        continue;  // instance was removed since the dispatch
                    inst = &instances[idxIt->second];
                }
                if (visibility[k]) {
                    inst->lastCullVisible = 1;
                    inst->hizPrevCulledFrames = 0;
                } else {
                    inst->lastCullVisible = 0;
                    inst->hizPrevCulledFrames =
                        std::min<uint8_t>(inst->hizPrevCulledFrames + 1, 3);
                }
            }
        };
        // 88k verdicts read back from mapped memory and scattered, which ran
        // on this one thread ahead of the timed cull. Split as the
        // classification below is: ids are unique, so each instance is
        // written by exactly one range, and the id map is only read. The
        // caller takes the last range so nesting on the pool cannot deadlock.
        const size_t total = ids.size();
        const size_t ranges = total >= 16384
            ? std::min<size_t>(core::ThreadPool::frameWorkers().threadCount() + 1, 8)
            : 1;
        if (ranges <= 1) {
            scatterRange(0, total);
        } else {
            const size_t per = (total + ranges - 1) / ranges;
            std::vector<std::future<void>> scatterFutures;
            scatterFutures.reserve(ranges - 1);
            for (size_t r = 0; r + 1 < ranges; ++r) {
                const size_t b = r * per;
                const size_t e = std::min(total, b + per);
                scatterFutures.push_back(core::ThreadPool::frameWorkers().submit(
                    [&scatterRange, b, e]() { scatterRange(b, e); }));
            }
            scatterRange((ranges - 1) * per, total);
            for (auto& f : scatterFutures) f.get();
        }
    } else {
        // No GPU cull data - conservatively treat everything as visible.
        for (auto& inst : instances) {
            inst.lastCullVisible = 1;
            inst.hizPrevCulledFrames = 0;
        }
    }

    // If GPU culling was not dispatched, fallback: compute distances on CPU
    float maxRenderDistanceSq;
    if (!gpuCullAvailable) {
        const float targetRenderDist = viewDistanceScale_ *
            ((instances.size() > 2000) ? 300.0f
             : (instances.size() > 1000) ? 500.0f
                                         : 1000.0f);
        const float shrinkRate = 0.005f;
        const float growRate = 0.05f;
        float blendRate = (targetRenderDist < smoothedRenderDist_) ? shrinkRate : growRate;
        smoothedRenderDist_ = glm::mix(smoothedRenderDist_, targetRenderDist, blendRate);
        maxRenderDistanceSq = smoothedRenderDist_ * smoothedRenderDist_;
    } else {
        maxRenderDistanceSq = smoothedRenderDist_ * smoothedRenderDist_;
    }

    const float fadeStartFraction = 0.75f;
    const glm::vec3 camPos = camera.getPosition();

    // Where this pass's three milliseconds go.
    //
    // Recording the M2 secondary is the largest single piece of CPU work in
    // the frame - renderWorld waits on the slowest worker and this is always
    // it - and the stage around it cannot see whether that is the cull, the
    // sort or the recording itself.
    static const bool m2Profile = core::envFlagEnabled("WOWEE_FRAME_PROFILE", false);
    const auto m2T0 = std::chrono::steady_clock::now();

    // Build sorted visible instance list
    sortedVisible_.clear();
    transparentVisible_.clear();
    skyDiagDrawsOpaque_ = 0;
    skyDiagDrawsTransparent_ = 0;
    const size_t expectedVisible = std::min(instances.size() / 3, size_t(600));
    if (sortedVisible_.capacity() < expectedVisible) {
        sortedVisible_.reserve(expectedVisible);
    }
    if (transparentVisible_.capacity() < expectedVisible / 4)
        transparentVisible_.reserve(expectedVisible / 4);

    // GPU frustum culling - build frustum for CPU fallback path and overflow instances
    Frustum frustum;
    {
        const glm::mat4 vp = camera.getProjectionMatrix() * camera.getViewMatrix();
        frustum.extractFromMatrix(vp);
    }
    // Matches the bound uploaded to the cull shader, including the headroom the
    // game-object floor needs (see dispatchCullCompute).
    const float maxPossibleDistSq = std::max(
        maxRenderDistanceSq * 4.0f,
        rendering::M2_GAME_OBJECT_MIN_RENDER_DISTANCE *
        rendering::M2_GAME_OBJECT_MIN_RENDER_DISTANCE);

    const uint32_t totalInstances = static_cast<uint32_t>(instances.size());
    struct VisibleChunk {
        std::vector<VisibleEntry> opaque;
        std::vector<VisibleEntry> transparent;
    };

    // Visibility classification is independent per instance and was the
    // remaining monolithic M2 CPU pass. Split dense scenes across a few pool
    // workers; the caller handles the final chunk so nested use from the M2
    // render worker cannot deadlock the shared pool.
    // One chunk per worker plus this thread. Four was the cap, and on a
    // twenty-core machine it left the visibility walk of tens of thousands
    // of instances to four threads while the rest idled.
    const uint32_t workerChunks = static_cast<uint32_t>(core::ThreadPool::frameWorkers().threadCount()) + 1;
    const uint32_t chunkCount = totalInstances >= 2048
        ? std::min<uint32_t>(std::max(4u, workerChunks), (totalInstances + 1023) / 1024)
        : 1;
    std::vector<VisibleChunk> chunks(chunkCount);
    auto classifyRange = [&](uint32_t chunk, uint32_t begin, uint32_t end) {
        auto& out = chunks[chunk];
        out.opaque.reserve((end - begin) / 3);
        out.transparent.reserve((end - begin) / 12);
        for (uint32_t i = begin; i < end; ++i) {
            const auto& instance = instances[i];
            // The GPU's verdict first: ~80k of 88k are culled, and for them
            // the distance bound below is work thrown away.
            if (!forceNoCull_ && gpuCullAvailable && i < numInstances &&
                !instance.lastCullVisible) continue;
            float distSq;
            float effectiveMaxDistSq;

            // Server game objects keep a distance floor instead of following the
            // ambient doodad distance down; it also feeds the fade curve below,
            // so a mailbox doesn't fade out at the doodad boundary either.
            const float instanceMaxDistSq = rendering::m2InstanceMaxDistSq(
                maxRenderDistanceSq, instance.cachedEffectiveMaxDistSqFactor,
                instance.isGameObject, rendering::M2_GAME_OBJECT_MIN_RENDER_DISTANCE,
                cappedViewDistance(),
                instance.cachedIsGroundDetail, groundDetailMaxDistance_);

            if (forceNoCull_) {
                if (!instance.cachedIsValid) continue;
                glm::vec3 toCam = instance.position - camPos;
                distSq = glm::dot(toCam, toCam);
                effectiveMaxDistSq = instanceMaxDistSq;
            } else if (gpuCullAvailable && i < numInstances) {
                // Per-instance verdict scattered above - indexing visibility[]
                // directly here would read the slot of whichever instance held
                // this array position when the dispatch was recorded.
                if (!instance.lastCullVisible) continue;
                glm::vec3 toCam = instance.position - camPos;
                distSq = glm::dot(toCam, toCam);
                effectiveMaxDistSq = instanceMaxDistSq;
            } else {
                if (!instance.cachedIsValid || instance.cachedIsSmoke || instance.cachedIsInvisibleTrap) continue;
                glm::vec3 toCam = instance.position - camPos;
                distSq = glm::dot(toCam, toCam);
                if (distSq > maxPossibleDistSq) continue;
                effectiveMaxDistSq = instanceMaxDistSq;
                if (distSq > effectiveMaxDistSq) continue;
                float paddedRadius = instance.cachedPaddedRadius;
                if (paddedRadius > 0.0f && !frustum.intersectsSphere(instance.cachedCullCenter, paddedRadius)) continue;
            }

            if (instance.cachedIsSkyBird && instance.cachedHasAnimation && !instance.cachedDisableAnimation) {
                constexpr float kBirdMaxDistSq =
                    rendering::M2_SKY_BIRD_MAX_RENDER_DISTANCE *
                    rendering::M2_SKY_BIRD_MAX_RENDER_DISTANCE;
                effectiveMaxDistSq = std::min(effectiveMaxDistSq, kBirdMaxDistSq);
                if (distSq > effectiveMaxDistSq) continue;
            }

            VisibleEntry visible{.index = i, .modelId = instance.modelId, .distSq = distSq, .effectiveMaxDistSq = effectiveMaxDistSq};
            // A faded instance is drawn blended, all of it - see M2Instance::fade.
            const bool faded = instance.fade < 0.999f;
            if (!faded) out.opaque.push_back(visible);
            if (faded || (instance.cachedModel &&
                (instance.cachedModel->hasTransparentBatches || instance.cachedModel->isSpellEffect))) {
                out.transparent.push_back(visible);
            }
        }
        // Each worker sorts its own share by model; the shares are merged
        // below rather than the whole list sorted on this thread, which was
        // 0.3-0.9 ms of the M2 worker for ~9k visible instances.
        std::sort(out.opaque.begin(), out.opaque.end(),
                  [](const VisibleEntry& a, const VisibleEntry& b) { return a.modelId < b.modelId; });
    };

    std::vector<std::future<void>> visibilityFutures;
    visibilityFutures.reserve(chunkCount > 0 ? chunkCount - 1 : 0);
    const uint32_t chunkSize = (totalInstances + chunkCount - 1) / chunkCount;
    for (uint32_t chunk = 0; chunk + 1 < chunkCount; ++chunk) {
        const uint32_t begin = chunk * chunkSize;
        const uint32_t end = std::min(totalInstances, begin + chunkSize);
        visibilityFutures.push_back(core::ThreadPool::frameWorkers().submit(
            [&, chunk, begin, end]() { classifyRange(chunk, begin, end); }));
    }
    const uint32_t lastChunk = chunkCount - 1;
    classifyRange(lastChunk, lastChunk * chunkSize, totalInstances);
    for (auto& future : visibilityFutures) future.get();

    // Where each chunk's sorted run ends in sortedVisible_, for the merge.
    std::vector<size_t> visibleRunBounds;
    visibleRunBounds.reserve(chunks.size() + 1);
    visibleRunBounds.push_back(0);
    for (auto& chunk : chunks) {
        sortedVisible_.insert(sortedVisible_.end(),
                              std::make_move_iterator(chunk.opaque.begin()),
                              std::make_move_iterator(chunk.opaque.end()));
        visibleRunBounds.push_back(sortedVisible_.size());
        transparentVisible_.insert(transparentVisible_.end(),
                                   std::make_move_iterator(chunk.transparent.begin()),
                                   std::make_move_iterator(chunk.transparent.end()));
    }
    // Texture scrolls and colour tracks read the clocks of whatever is drawn.
    for (const auto& v : sortedVisible_) advanceClock(instances[v.index]);
    for (const auto& v : transparentVisible_) advanceClock(instances[v.index]);

    // The furthest doodad actually drawn, for the diagnostic that sets it
    // against the furthest terrain chunk. See Renderer::logViewDistanceDiag.
    furthestDrawnSq_ = 0.0f;
    for (const auto& e : sortedVisible_)
        if (e.distSq > furthestDrawnSq_) furthestDrawnSq_ = e.distSq;
    for (const auto& e : transparentVisible_)
        if (e.distSq > furthestDrawnSq_) furthestDrawnSq_ = e.distSq;

    // Whether the sky model survived culling this frame, when this is the
    // renderer that draws one.
    //
    // Reported as flickering while the camera turns and steady while it does
    // not, with everything upstream measured and holding still: the lighting
    // inputs, the model's clock, the frame time. What that leaves is the dome
    // being drawn on some frames and not others, and this says so in one line
    // per change rather than one per frame.
    // WOWEE_SKY_M2_OPAQUE_ONLY=1 drops the sky model's blended layers.
    //
    // The model is the culprit - the flicker goes with WOWEE_NO_SKY_M2 - and it
    // is not a small one: hellfireskybox.m2 carries 23 textures over 34 render
    // flags, with five transparency tracks and seven UV animations driven by
    // six global sequences. The brightening and dimming is those alpha tracks.
    // This says whether the flicker is in the blended layers or in the base the
    // opaque pass draws, which halves what is left to read.
    static const bool skyOpaqueOnly = std::getenv("WOWEE_SKY_M2_OPAQUE_ONLY") != nullptr;
    if (skyMode_ && skyOpaqueOnly) transparentVisible_.clear();

    if (skyMode_) {
        const bool drawn = !sortedVisible_.empty() || !transparentVisible_.empty();
        if (drawn != skyDiagWasDrawn_) {
            skyDiagWasDrawn_ = drawn;
            LOG_INFO("skyM2 cull: ", drawn ? "DRAWN" : "CULLED",
                     " opaque=", sortedVisible_.size(),
                     " transparent=", transparentVisible_.size(),
                     " instances=", instances.size());
        }
    }

    // Two-pass rendering: opaque/alpha-test first (depth write ON), then transparent/additive
    // (depth write OFF, sorted back-to-front) so transparent geometry composites correctly
    // against all opaque geometry rather than only against what was rendered before it.

    // Pass 1: sort by modelId for minimum buffer rebinds (opaque batches)
    //
    // Ordering the groups front-to-back was tried, on the theory that cutout
    // foliage cannot be rejected before it is shaded so the near trees should
    // lay depth down first. It moved nothing: doodads measured 14.66ms against
    // 16.55 and 13.38 either side of it, which is the middle of the spread.
    // What it did cost was a hash map over every visible instance, two index
    // vectors and a copy of the whole list, every frame, in the pass that
    // turned out to be the largest single piece of CPU work in the frame.
    const auto m2T1 = std::chrono::steady_clock::now();
    {
        // Pairwise merges of the per-chunk sorted runs, log2(chunks) passes.
        const auto byModel = [](const VisibleEntry& a, const VisibleEntry& b) {
            return a.modelId < b.modelId;
        };
        std::vector<size_t> next;
        while (visibleRunBounds.size() > 2) {
            next.clear();
            const size_t runs = visibleRunBounds.size() - 1;
            for (size_t r = 0; r < runs; r += 2) {
                if (r + 1 < runs) {
                    std::inplace_merge(sortedVisible_.begin() + visibleRunBounds[r],
                                       sortedVisible_.begin() + visibleRunBounds[r + 1],
                                       sortedVisible_.begin() + visibleRunBounds[r + 2],
                                       byModel);
                }
                next.push_back(visibleRunBounds[r]);
            }
            next.push_back(visibleRunBounds.back());
            visibleRunBounds.swap(next);
        }
    }
    const auto m2T2 = std::chrono::steady_clock::now();
    // Reported at the end of the pass; see m2Profile above.
    struct M2Phases { double cull, sort, record; std::size_t visible, instances; };
    const auto sayPhases = [&](const M2Phases& p) {
        static auto lastSaid = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - lastSaid <= std::chrono::seconds(10)) return;
        lastSaid = now;
        LOG_WARNING("  m2 record: cull ", p.cull, "ms, sort ", p.sort,
                    "ms, draws ", p.record, "ms over ", p.visible,
                    " visible of ", p.instances, " instances");
    };

    uint32_t currentModelId = UINT32_MAX;
    const M2ModelGPU* currentModel = nullptr;
    bool currentModelValid = false;

    // State tracking
    VkPipeline currentPipeline = VK_NULL_HANDLE;
    VkDescriptorSet currentMaterialSet = VK_NULL_HANDLE;

    // Push constants now carry per-batch data only; per-instance data is in instance SSBO.
    struct M2PushConstants {
        int32_t texCoordSet;        // UV set index (0 or 1)
        int32_t isFoliage;          // -1 = sky, 0 = none, 1 = wind foliage, 2 = ground clutter
        int32_t instanceDataOffset; // Base index into instance SSBO for this draw group
        float swayRefHeight;        // Model-space height the wind normalises against
        float swayAmp;              // Wind amplitude scale; 1.0 = the tree-sized default
        float plantHeight;          // The model's own height, for the player brush
    };

    // Fill the sway half of the push constants for one model.
    //
    // Two modes, and the split is about who owns the idle motion. Ground clutter
    // plays a sequence of its own, so mode 2 asks the shader for the player
    // brush and no wind - two swings of one plant at two rates reads as a
    // glitch. Everything else the wind picks up has its animation disabled by
    // the classifier and gets mode 1, wind and brush both.
    //
    // The wind itself was written for trees: it normalised height against 20
    // yards and displaced by an absolute number of model units, so a one-yard
    // tuft travelled a fraction of a millimetre. Every model normalises against
    // its own height now, with an amplitude interpolated between the two ends
    // rather than switched at a threshold - a bush a foot taller than its
    // neighbour should not sway ten times less. Both ends reproduce the numbers
    // that were there: a 20-yard tree still throws 0.35 model units at the tip.
    auto fillSway = [](M2PushConstants& pc, const M2ModelGPU& mdl, bool sky) {
        const M2Sway sway = m2SwayFor(sky, mdl.isHangingCloth, mdl.shadowWindFoliage,
                                      mdl.isGroundDetail, mdl.boundMin.z, mdl.boundMax.z,
                                      mdl.isStandingCloth);
        pc.isFoliage = sway.mode;
        pc.swayRefHeight = sway.refHeight;
        pc.swayAmp = sway.amp;
        pc.plantHeight = sway.plantHeight;
    };

    // Validate per-frame descriptor set before any Vulkan commands
    if (!perFrameSet) {
        LOG_ERROR("M2Renderer::render: perFrameSet is VK_NULL_HANDLE - skipping M2 render");
        return;
    }

    // Bind per-frame descriptor set (set 0) - shared across all draws
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);

    // Start with opaque pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaquePipeline_);
    currentPipeline = opaquePipeline_;

    // Bind dummy bone set (set 2) so non-animated draws have a valid binding.
    // Bind mega bone SSBO instead - all instances index into one buffer via boneBase.
    if (megaBoneSet_[frameIndex]) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 2, 1, &megaBoneSet_[frameIndex], 0, nullptr);
    } else if (dummyBoneSet_) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 2, 1, &dummyBoneSet_, 0, nullptr);
    }

    // Bind instance data SSBO (set 3) - per-instance transforms, fade, bones
    if (instanceSet_[frameIndex]) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 3, 1, &instanceSet_[frameIndex], 0, nullptr);
    }

    // Reset instance SSBO write cursor for this frame
    instanceDataCount_ = 0;
    auto* instSSBO = static_cast<M2InstanceGPU*>(instanceMapped_[frameIndex]);

    // =====================================================================
    // Opaque pass - instanced draws grouped by (modelId, LOD)
    // =====================================================================
    // sortedVisible_ is already sorted by modelId so consecutive entries share
    // the same vertex/index buffer.  Within each model group we sub-group by
    // targetLOD to guarantee all instances in one vkCmdDrawIndexed use the
    // same batch set.  Per-instance data (model matrix, fade, bones) is
    // written to the instance SSBO; the shader reads it via gl_InstanceIndex.
    {
        struct PendingInstance {
            uint32_t instanceIdx;
            float fadeAlpha;
            bool useBones;
            uint16_t targetLOD;
        };
        std::vector<PendingInstance> pending;
        pending.reserve(128);

        size_t visStart = 0;
        while (visStart < sortedVisible_.size()) {
            // Find group of consecutive entries with same modelId
            uint32_t groupModelId = sortedVisible_[visStart].modelId;
            size_t groupEnd = visStart;
            while (groupEnd < sortedVisible_.size() && sortedVisible_[groupEnd].modelId == groupModelId)
                groupEnd++;

            // Pull the model through the first entry's instance.cachedModel pointer
            // (set at addInstance) instead of doing models.find(groupModelId) per group.
            const auto& firstEntry = sortedVisible_[visStart];
            if (firstEntry.index >= instances.size() || !instances[firstEntry.index].cachedModel) {
                visStart = groupEnd;
                continue;
            }
            const M2ModelGPU& model = *instances[firstEntry.index].cachedModel;
            if (skipGroundDetail_ && model.isGroundDetail) {
                visStart = groupEnd;
                continue;
            }
            if (!model.vertexBuffer || !model.indexBuffer) {
                visStart = groupEnd;
                continue;
            }

            bool modelNeedsAnimation = model.hasAnimation && !model.disableAnimation;
            const bool foliageLikeModel = model.isFoliageLike;
            const bool particleDominantEffect = model.isSpellEffect &&
                !model.particleEmitters.empty() && model.batches.size() <= 2;

            // Collect per-instance data for this model group
            pending.clear();
            for (size_t vi = visStart; vi < groupEnd; vi++) {
                const auto& entry = sortedVisible_[vi];
                if (entry.index >= instances.size()) continue;
                auto& instance = instances[entry.index];

                // Distance-based fade alpha
                float fadeFrac = model.disableAnimation ? 0.55f : fadeStartFraction;
                float fadeStartDistSq = entry.effectiveMaxDistSq * fadeFrac * fadeFrac;
                float fadeAlpha = 1.0f;
                if (entry.distSq > fadeStartDistSq) {
                    fadeAlpha = std::clamp((entry.effectiveMaxDistSq - entry.distSq) /
                                          (entry.effectiveMaxDistSq - fadeStartDistSq), 0.0f, 1.0f);
                }
                // Ground detail used to be held at 0.82 here. This is the
                // opaque pass: nothing blended it, so it was a number with no
                // effect - and now that the cutout pipeline turns alpha into
                // coverage, keeping it would punch a fifth of the pixels out
                // of every tuft of grass in reach.
                float instanceFadeAlpha = fadeAlpha;

                // Bone readiness check
                if (modelNeedsAnimation && instance.boneMatrices.empty()) continue;
                bool needsBones = modelNeedsAnimation && !instance.boneMatrices.empty();
                if (needsBones && instance.megaBoneOffset == 0) continue;

                // LOD selection
                uint16_t desiredLOD = 0;
                if (entry.distSq > 150.0f * 150.0f) desiredLOD = 3;
                else if (entry.distSq > 80.0f * 80.0f) desiredLOD = 2;
                else if (entry.distSq > 40.0f * 40.0f) desiredLOD = 1;
                // Down to the nearest level the model actually has, not all
                // the way back to full detail. A model carrying 0 and 1 and
                // asked for 3 was drawn at 0 - the most expensive level there
                // is - at the distance where it mattered least.
                uint16_t targetLOD = desiredLOD;
                while (targetLOD > 0 && !(model.availableLODs & (1u << targetLOD))) --targetLOD;

                pending.push_back({.instanceIdx = entry.index, .fadeAlpha = instanceFadeAlpha, .useBones = needsBones, .targetLOD = targetLOD});
            }

            if (pending.empty()) { visStart = groupEnd; continue; }

            // Sort by targetLOD so each sub-group occupies a contiguous SSBO range
            std::sort(pending.begin(), pending.end(),
                      [](const PendingInstance& a, const PendingInstance& b) { return a.targetLOD < b.targetLOD; });

            // Bind vertex/index buffers once per model group
            VkDeviceSize vbOffset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &model.vertexBuffer, &vbOffset);
            vkCmdBindIndexBuffer(cmd, model.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

            // Write base instance data to SSBO (uvOffset=0 - overridden for tex-anim batches)
            uint32_t baseSSBOOffset = instanceDataCount_;
            size_t writtenInstances = 0;
            for (const auto& p : pending) {
                if (instanceDataCount_ >= MAX_INSTANCE_DATA) break;
                auto& inst = instances[p.instanceIdx];
                auto& e = instSSBO[instanceDataCount_];
                e.model = inst.modelMatrix;
                e.uvOffset = glm::vec2(0.0f);
                e.fadeAlpha = p.fadeAlpha;
                e.useBones = (p.useBones && !kM2NoSkinning) ? 1 : 0;
                e.boneBase = p.useBones ? static_cast<int32_t>(inst.megaBoneOffset) : 0;
                e.boneCount = static_cast<int32_t>(inst.boneMatrices.size());
                e.highlight = inst.highlight;
                e.flags = (inst.afloat > 0 ? 1 : 0) | inst.groundNormalFlags;
                instanceDataCount_++;
                ++writtenInstances;
            }

            // Drop what did not fit. The loop above stops writing at the cap,
            // but the LOD sub-groups below are ranges over `pending` and were
            // still being drawn in full - groupSSBOOffset then runs past the
            // end of the buffer and the vertex shader reads instance data that
            // is not there. That is a real out-of-bounds read on the GPU, not a
            // missing model: it fires once per instance past the cap, hundreds
            // of times a frame, and the device is lost seconds later.
            //
            // Truncating is safe here precisely because `pending` was sorted by
            // LOD before the write: the sub-groups are contiguous and in the
            // same order, so cutting the tail cuts whole instances rather than
            // splitting a range.
            if (writtenInstances < pending.size()) {
                static bool warnedInstanceCap = false;
                if (!warnedInstanceCap) {
                    warnedInstanceCap = true;
                    LOG_WARNING("M2Renderer: instance buffer full at ", MAX_INSTANCE_DATA,
                                "; dropping ", pending.size() - writtenInstances,
                                " instances of '", model.name, "' this frame");
                }
                pending.resize(writtenInstances);
            }

            // Process LOD sub-groups within this model group
            size_t lodIdx = 0;
            while (lodIdx < pending.size()) {
                uint16_t lod = pending[lodIdx].targetLOD;
                size_t lodEnd = lodIdx + 1;
                while (lodEnd < pending.size() && pending[lodEnd].targetLOD == lod) lodEnd++;
                uint32_t groupSize = static_cast<uint32_t>(lodEnd - lodIdx);
                uint32_t groupSSBOOffset = baseSSBOOffset + static_cast<uint32_t>(lodIdx);

                // What each batch of a named model does at draw time, once.
                //
                // WOWEE_M2_BATCH_DIAG says what a batch IS, at load. It could
                // not say whether the batch was drawn, and a tree reported as
                // missing a section of trunk is exactly that question: four
                // readings of the model data in a row said every batch was
                // present and correct, which it is, so the answer has to come
                // from the frame rather than from the file.
                static const std::string kDrawDiag = [] {
                    const char* v = std::getenv("WOWEE_M2_BATCH_DIAG");
                    std::string t = v ? v : "";
                    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
                    return t;
                }();
                bool diagThisModel = false;
                if (!kDrawDiag.empty()) {
                    std::string lowerName = model.name;
                    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    diagThisModel = lowerName.find(kDrawDiag) != std::string::npos;
                }
                // WOWEE_M2_ONLY_BATCH=<n> draws just that batch of the named
                // model and hides the rest.
                //
                // Everything checkable has now been checked on these trees and
                // every answer came back correct: the trunk mesh is continuous
                // from root to tip, its submesh level matches, its texture
                // loads, its mips preserve coverage, and the draw diagnostic
                // says every batch reaches the frame. A section still looks
                // missing. Seeing the trunk by itself is the difference
                // between the trunk being wrong and the canopy eating it, and
                // no amount of reading the files can tell those apart.
                static const int kOnlyBatch = [] {
                    const char* v = std::getenv("WOWEE_M2_ONLY_BATCH");
                    return (v && *v) ? std::atoi(v) : -1;
                }();
                for (size_t bi = 0; bi < model.batches.size(); bi++) {
                    const auto& batch = model.batches[bi];
                    const char* skipped = nullptr;
                    if (kOnlyBatch >= 0 && diagThisModel &&
                        bi != static_cast<size_t>(kOnlyBatch)) {
                        skipped = "WOWEE_M2_ONLY_BATCH";
                    }
                    else if (batch.indexCount == 0) skipped = "no indices";
                    else if (!model.isGroundDetail && batch.submeshLevel != lod)
                        skipped = "submeshLevel is not this LOD";
                    else if (batch.batchOpacity < 0.01f) skipped = "opacity is zero";
                    else if (!skyBatchAllowed(skyMode_, bi)) skipped = "sky batch rule";
                    else if (suppressBakedStars_ && batch.starLayer) skipped = "baked star layer";
                    if (diagThisModel) {
                        static std::set<std::pair<const void*, size_t>> saidDraw;
                        if (saidDraw.size() < 64 && saidDraw.insert({&model, bi}).second) {
                            LOG_WARNING("M2 DRAW '", model.name, "' batch ", bi,
                                        " lod=", lod, " submeshLevel=", batch.submeshLevel,
                                        " idx=", batch.indexCount,
                                        " opacity=", batch.batchOpacity,
                                        (skipped ? "  SKIPPED: " : "  drawn"),
                                        (skipped ? skipped : ""));
                        }
                    }
                    if (skipped) continue;
                    const bool batchUnlit = (batch.materialFlags & 0x01) != 0;
                    M2GlowCardBatch glowCard;
                    glowCard.glowSize = batch.glowSize;
                    glowCard.blendMode = batch.blendMode;
                    glowCard.lanternGlowHint = batch.lanternGlowHint;
                    glowCard.glowCardLike = batch.glowCardLike;
                    glowCard.colorKeyBlack = batch.colorKeyBlack;
                    glowCard.unlit = batchUnlit;
                    glowCard.preserveGlowMesh = batch.preserveGlowMesh;
                    glowCard.modelIsElvenLike = model.isElvenLike;
                    glowCard.modelIsLanternLike = model.isLanternLike;
                    glowCard.modelIsTorch = model.isTorch;
                    glowCard.modelIsBrazierOrFire = model.isBrazierOrFire;
                    glowCard.modelIsSpellEffect = model.isSpellEffect;
                    glowCard.modelIsKoboldFlame = model.isKoboldFlame;
                    const bool shouldUseGlowSprite = m2WantsGlowSprite(glowCard);
                    if (shouldUseGlowSprite) {
                        // Generate glow sprites for each instance in the group
                        for (size_t j = lodIdx; j < lodEnd; j++) {
                            auto& inst = instances[pending[j].instanceIdx];
                            glm::vec3 worldPos;
                            if (model.isGroundFire &&
                                !model.particleEmitters.empty()) {
                                worldPos = glm::vec3(std::numeric_limits<float>::max());
                                for (const auto& emitter : model.particleEmitters) {
                                    glm::mat4 boneXform(1.0f);
                                    if (emitter.bone < inst.boneMatrices.size()) {
                                        boneXform = inst.boneMatrices[emitter.bone];
                                    }
                                    const glm::vec3 emitterWorld = glm::vec3(
                                        inst.modelMatrix * boneXform * glm::vec4(emitter.position, 1.0f));
                                    if (emitterWorld.z < worldPos.z) worldPos = emitterWorld;
                                }
                            } else {
                                worldPos = animatedBatchWorldCenter(inst, batch);
                            }
                            // Preserved emissive glass writes opaque depth before
                            // this additive point sprite. Move only the visual
                            // halo just beyond the camera-facing glass surface so
                            // depth testing does not reject it; the associated
                            // local light remains at the true batch center.
                            if (batch.preserveGlowMesh) {
                                const glm::vec3 towardCamera = camPos - worldPos;
                                const float lenSq = glm::dot(towardCamera, towardCamera);
                                if (lenSq > 0.0001f) {
                                    worldPos += towardCamera * glm::inversesqrt(lenSq) *
                                        (batch.glowSize * inst.scale * 1.25f);
                                }
                            }
                            GlowSprite gs;
                            gs.worldPos = worldPos;
                            if (batch.glowTint == 1 || model.isElvenLike)
                                gs.color = glm::vec4(0.48f, 0.72f, 1.0f, 1.05f);
                            else if (batch.glowTint == 2)
                                gs.color = glm::vec4(1.0f, 0.28f, 0.22f, 1.10f);
                            else
                                gs.color = glm::vec4(1.0f, 0.82f, 0.46f, 1.15f);
                            // Match the parent M2's distance fade instead of a separate
                            // hard 180-unit cutoff, which made tunnel lights pop on.
                            gs.color.a *= pending[j].fadeAlpha;
                            gs.size = batch.glowSize * inst.scale *
                                (batch.preserveGlowMesh ? 2.0f : 1.45f);
                            if (batch.preserveGlowMesh) gs.color.a *= 1.25f;

                            // A fixture with real particle flames should read as
                            // flames with a halo behind them. The sprite is sized
                            // from its glow card's geometric radius, which on a
                            // chandelier spans the whole fixture - an additive
                            // blob about a unit across, against candle flames of
                            // 0.15, so the glow swallowed them entirely. Cap it
                            // just above what a small glow card already produces
                            // (the 0.5 floor times 1.45), so candles, lanterns
                            // and torches are untouched and only oversized cards
                            // are clamped.
                            if (!model.particleEmitters.empty() && model.isLanternLike) {
                                constexpr float kMaxHaloRadius = 0.75f;
                                gs.size = std::min(gs.size, kMaxHaloRadius * inst.scale);
                            }

                            // Fire burning inside a hearth. The sprite is a point
                            // billboard carrying one depth value for the whole
                            // quad, so as soon as its centre shows through the
                            // fireplace opening the entire square draws - brick
                            // surround included, which reads as the fire glowing
                            // through the masonry. Sized from the glow card's
                            // geometric radius these spheres are wider than the
                            // opening, so keep them inside it.
                            const bool hearthFire = model.isBrazierOrFire ||
                                                    model.isGroundFire ||
                                                    model.isForge;
                            if (hearthFire) {
                                constexpr float kMaxFireGlowRadius = 0.5f;
                                gs.size = std::min(gs.size, kMaxFireGlowRadius * inst.scale);
                            }

                            // Flame guttering. The phase comes from the lamp's own
                            // world position, so two lanterns on the same street
                            // never pulse together - a synchronised row of lamps
                            // reads as a rendering artifact, not firelight. Two
                            // detuned sines keep any single lamp from looping
                            // visibly. Alpha and size move together, since a
                            // brighter flame also looks slightly larger.
                            {
                                // Matches the phase used for this lamp's local
                                // light, so the sprite and the pool of light it
                                // casts breathe together.
                                // Same clock and parameters as the local light in
                                // gatherLocalLights, so the sprite and the pool of
                                // light it casts rise and fall together.
                                const float flicker = lampFlicker(
                                    inst.position, lampFlickerClockSeconds(),
                                    0.82f, 0.12f, 0.06f);
                                gs.color.a *= flicker;
                                gs.size    *= 0.98f + 0.02f * flicker;
                            }
                            glowSprites_.push_back(gs);
                            GlowSprite halo = gs;
                            halo.color.a *= batch.preserveGlowMesh ? 0.34f : 0.42f;
                            halo.size *= batch.preserveGlowMesh ? 2.2f : 1.8f;
                            // The halo is nearly twice the sprite, so capping only
                            // the sprite would leave the bleed to the halo.
                            if (hearthFire) {
                                constexpr float kMaxFireHaloRadius = 0.85f;
                                halo.size = std::min(halo.size, kMaxFireHaloRadius * inst.scale);
                            }
                            glowSprites_.push_back(halo);
                        }
                        if (m2GlowSpriteReplacesMesh(glowCard)) continue;
                    }

                    // Opaque gate - transparent glow cards were handled above so their
                    // sprites are generated before the mesh moves to pass 2.
                    const bool rawTransparent = (batch.blendMode >= 2) || model.isSpellEffect;
                    if (rawTransparent) continue;

                    // Particle-dominant effects: emission geometry - skip opaque
                    if (particleDominantEffect && batch.blendMode <= 1) continue;

                    // Handle texture animation: if this batch has per-instance uvOffset,
                    // write a separate SSBO range with the correct offsets.
                    bool hasBatchTexAnim = (batch.textureAnimIndex != 0xFFFF && model.hasTextureAnimation)
                                           || model.isLavaModel;
                    uint32_t drawOffset = groupSSBOOffset;
                    if (hasBatchTexAnim && instanceDataCount_ + groupSize <= MAX_INSTANCE_DATA) {
                        drawOffset = instanceDataCount_;
                        // Hoist per-batch lookups: the transform pointer is fixed for
                        // every instance in this group; only the sampled translation
                        // varies (per-instance animTime).
                        const pipeline::M2TextureTransform* tt = nullptr;
                        if (batch.textureAnimIndex != 0xFFFF && model.hasTextureAnimation) {
                            uint16_t lookupIdx = batch.textureAnimIndex;
                            if (lookupIdx < model.textureTransformLookup.size()) {
                                uint16_t transformIdx = model.textureTransformLookup[lookupIdx];
                                if (transformIdx < model.textureTransforms.size()) {
                                    tt = &model.textureTransforms[transformIdx];
                                }
                            }
                        }
                        for (size_t j = lodIdx; j < lodEnd; j++) {
                            const auto& p = pending[j];
                            auto& inst = instances[p.instanceIdx];
                            glm::vec2 uvOffset(0.0f);
                            if (tt) {
                                glm::vec3 trans = m2_track::sampleVec3(
                                    tt->translation, inst.currentSequenceIndex,
                                    inst.animTime, inst.globalSequenceTime,
                                    model.globalSequenceDurations, glm::vec3(0.0f));
                                uvOffset = glm::vec2(trans.x, trans.y);
                            }
                            if (model.isLavaModel && uvOffset == glm::vec2(0.0f)) {
                                uvOffset = glm::vec2(lavaAnimSeconds * 0.03f,
                                                     -lavaAnimSeconds * 0.08f);
                            }
                            // Rebuild the entry from CPU-side data rather than copying it
                            // out of the base entry. instSSBO lives in write-combined
                            // upload memory: writing it is cheap, but reading it back is
                            // uncached and costs microseconds per instance.
                            auto& e = instSSBO[instanceDataCount_];
                            e.model = inst.modelMatrix;
                            e.uvOffset = uvOffset;
                            e.fadeAlpha = p.fadeAlpha;
                            e.useBones = (p.useBones && !kM2NoSkinning) ? 1 : 0;
                            e.boneBase = p.useBones ? static_cast<int32_t>(inst.megaBoneOffset) : 0;
                            e.boneCount = static_cast<int32_t>(inst.boneMatrices.size());
                            e.highlight = inst.highlight;
                            e.flags = (inst.afloat > 0 ? 1 : 0) | inst.groundNormalFlags;
                            instanceDataCount_++;
                        }
                    }

                    // Pipeline selection (per-model/batch, not per-instance)
                    // A batch the artist marked opaque is not asking for
                    // any of this. Blend mode 0 means the alpha channel goes
                    // unread, and M2 textures are routinely atlases whose
                    // alpha belongs to another layer or another model - so
                    // keying on it eats surfaces that were painted solid.
                    //
                    // Silverpine's trees are the case that found it. Their
                    // trunk is one opaque batch sampling the right-hand panel
                    // of SilverPineTree01TrunkSkin, and that panel carries a
                    // stale alpha hole across its middle - 24% of the texels
                    // the trunk's own triangles cover, with unbroken bark
                    // painted underneath. Forced to the cutout pipeline, where
                    // alpha becomes coverage, the trunk lost its midsection
                    // and left the canopy floating over the stump. 133 foliage
                    // models were holed the same way - Tirisfal's canopytree07,
                    // the Plaguelands and Stonetalon pines, Eversong and
                    // Silvermoon, Nagrand, Crystalsong, Ruby Sanctum, the
                    // Storm Peaks, and every Zangarmarsh mushroom cap.
                    //
                    // Where the alpha really is the silhouette - a card drawn
                    // on a black backing, or DXT1 punch-through, which stores
                    // no colour under a transparent texel at all - the cutout
                    // still stands, over the 15 models that need it: Swamp of
                    // Sorrows' canopies, the quilboar thorn cards, Loch Modan's
                    // leaves and the lamps whose glass is keyed out. Measured
                    // from the texture by alphaIsSilhouette, not guessed from
                    // its name.
                    const bool opaqueByMaterial = batch.blendMode == M2_BLEND_OPAQUE;
                    const bool foliageCutout = foliageLikeModel && !model.isSpellEffect &&
                                               batch.blendMode <= 3 &&
                                               (!opaqueByMaterial || batch.alphaIsSilhouette);
                    // The fire burning in the hearth is an effect overlay on a
                    // black background, the same shape as a spell visual; drawn
                    // opaque it fills the forge opening with a black rectangle
                    // instead of flame. That is true of the flame cards only -
                    // treating the whole model this way turned the masonry and
                    // ironwork additive, which is to say translucent.
                    const bool fireEffectModel = batch.forgeFireCard;
                    // A batch the artist marked additive is already doing
                    // what the cutout and the colour key are approximations
                    // of: black adds nothing, so it disappears on its own.
                    // Forcing it opaque and then keying the black out leaves
                    // the bright middle of a glow card as a solid disc, which
                    // is what Orgrimmar's bonfires were.
                    const bool forceCutout =
                        !model.isSpellEffect && !fireEffectModel &&
                        !m2BlendIsAdditive(batch.blendMode) &&
                        (model.isGroundDetail || foliageCutout ||
                         m2BatchNeedsAlphaTest(batch.blendMode, batch.hasAlpha) ||
                         batch.colorKeyBlack);

                    uint8_t effectiveBlendMode = batch.blendMode;
                    if (model.isSpellEffect || fireEffectModel) {
                        if (effectiveBlendMode <= 1) effectiveBlendMode = 3;
                        else if (effectiveBlendMode == 4 || effectiveBlendMode == 5) effectiveBlendMode = 3;
                    }
                    if (forceCutout) effectiveBlendMode = 1;

                    VkPipeline desiredPipeline;
                    if (forceCutout) {
                        desiredPipeline = cutoutPipeline_;
                    } else {
                        switch (effectiveBlendMode) {
                            case 0: desiredPipeline = opaquePipeline_; break;
                            case 1: desiredPipeline = alphaTestPipeline_; break;
                            case 2: desiredPipeline = alphaPipeline_; break;
                            default: desiredPipeline = additivePipeline_; break;
                        }
                    }
                    if (desiredPipeline != currentPipeline) {
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
                        currentPipeline = desiredPipeline;
                    }

                    // Update material UBO
                    if (batch.materialUBOMapped) {
                        auto* mat = static_cast<M2MaterialUBO*>(batch.materialUBOMapped);
                        // interiorDarken is a camera-based flag - it darkens ALL M2s (incl.
                        // outdoor trees) when the camera is inside a WMO.  Disable it; indoor
                        // M2s already look correct from the darker ambient/lighting.
                        mat->interiorDarken = 0.0f;
                        if (batch.colorKeyBlack)
                            mat->colorKeyThreshold = (effectiveBlendMode == 4 || effectiveBlendMode == 5) ? 0.7f : 0.08f;
                        if (forceCutout) {
                            mat->alphaTest = model.isGroundDetail ? 3 : (foliageCutout ? 2 : 1);
                            if (model.isGroundDetail) mat->unlit = 0;
                        }
                        mat->volumetricBeam =
                            (model.isVolumetricBeam || batch.volumetricBeam) ? 1 : 0;
                        mat->fireCard = batch.forgeFireCard ? 1 : 0;
                    }

                    // Bind material descriptor set (set 1)
                    if (!batch.materialSet) continue;
                    if (batch.materialSet != currentMaterialSet) {
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                pipelineLayout_, 1, 1, &batch.materialSet, 0, nullptr);
                        currentMaterialSet = batch.materialSet;
                    }

                    // Push constants + instanced draw
                    M2PushConstants pc;
                    pc.texCoordSet = static_cast<int32_t>(batch.textureUnit);
                    fillSway(pc, model, skyMode_);
                    pc.instanceDataOffset = static_cast<int32_t>(drawOffset);
                    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
                    vkCmdDrawIndexed(cmd, batch.indexCount, groupSize, batch.indexStart, 0, 0);
                    if (skyMode_) ++skyDiagDrawsOpaque_;
                    lastDrawCallCount++;
                }

                lodIdx = lodEnd;
            }

            visStart = groupEnd;
        }
    }

    // =====================================================================
    // Pass 2: Transparent/additive batches - back-to-front per instance
    // =====================================================================
    // Transparent geometry must be drawn individually per instance in back-to-
    // front order for correct alpha compositing.  Each draw writes one
    // M2InstanceGPU entry and issues a single-instance indexed draw.
    std::sort(transparentVisible_.begin(), transparentVisible_.end(),
              [](const VisibleEntry& a, const VisibleEntry& b) { return a.distSq > b.distSq; });

    currentModelId = UINT32_MAX;
    currentModel = nullptr;
    currentModelValid = false;
    currentPipeline = opaquePipeline_;
    currentMaterialSet = VK_NULL_HANDLE;

    for (const auto& entry : transparentVisible_) {
        if (entry.index >= instances.size()) continue;
        auto& instance = instances[entry.index];

        // Model boundary: read cachedModel off the instance - was doing a
        // per-boundary models.find() even though every instance already has
        // the pointer cached at addInstance time.
        if (entry.modelId != currentModelId) {
            currentModelId = entry.modelId;
            currentModelValid = false;
            currentModel = instance.cachedModel;
            if (!currentModel) continue;
            if (!currentModel->hasTransparentBatches && !currentModel->isSpellEffect) continue;
            if (!currentModel->vertexBuffer || !currentModel->indexBuffer) continue;
            currentModelValid = true;
            VkDeviceSize vbOff = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &currentModel->vertexBuffer, &vbOff);
            vkCmdBindIndexBuffer(cmd, currentModel->indexBuffer, 0, VK_INDEX_TYPE_UINT16);
        }
        if (!currentModelValid) continue;

        const M2ModelGPU& model = *currentModel;

        // Fade alpha
        float fadeAlpha = 1.0f;
        float fadeFrac = model.disableAnimation ? 0.55f : fadeStartFraction;
        float fadeStartDistSq = entry.effectiveMaxDistSq * fadeFrac * fadeFrac;
        if (entry.distSq > fadeStartDistSq) {
            fadeAlpha = std::clamp((entry.effectiveMaxDistSq - entry.distSq) /
                                  (entry.effectiveMaxDistSq - fadeStartDistSq), 0.0f, 1.0f);
        }
        float instanceFadeAlpha = fadeAlpha * instance.fade;
        const bool instanceFaded = instance.fade < 0.999f;
        if (model.isGroundDetail) instanceFadeAlpha *= 0.82f;

        bool modelNeedsAnimation = model.hasAnimation && !model.disableAnimation;
        if (modelNeedsAnimation && instance.boneMatrices.empty()) continue;
        bool needsBones = modelNeedsAnimation && !instance.boneMatrices.empty();
        if (needsBones && instance.megaBoneOffset == 0) continue;

        uint16_t desiredLOD = 0;
        if (entry.distSq > 150.0f * 150.0f) desiredLOD = 3;
        else if (entry.distSq > 80.0f * 80.0f) desiredLOD = 2;
        else if (entry.distSq > 40.0f * 40.0f) desiredLOD = 1;
        // The nearest level the model has; see the note in the batched path.
        uint16_t targetLOD = desiredLOD;
        while (targetLOD > 0 && !(model.availableLODs & (1u << targetLOD))) --targetLOD;

        const bool particleDominantEffect = model.isSpellEffect &&
            !model.particleEmitters.empty() && model.batches.size() <= 2;

        for (const auto& batch : model.batches) {
            if (batch.indexCount == 0) continue;
            if (!model.isGroundDetail && batch.submeshLevel != targetLOD) continue;
            if (batch.batchOpacity < 0.01f) continue;
            if (!skyBatchAllowed(skyMode_, static_cast<std::size_t>(&batch - model.batches.data()))) continue;

            // Pass 2 gate: only transparent/additive batches - or, for a faded
            // instance, every batch, since the opaque pass left it out.
            {
                const bool rawTransparent = (batch.blendMode >= 2) || model.isSpellEffect ||
                                            instanceFaded;
                if (!rawTransparent) continue;
            }

            // WOWEE_SKY_M2_SKIP_BLEND=<n> drops the sky's layers of one blend
            // mode. Its thirty-four batches are six opaque, ten alpha-blended
            // (2) and eighteen additive (4), and the two behave differently in
            // a way that matters: additive is order-independent and alpha
            // blending is not. Dropping ten or dropping eighteen says which
            // half the flicker lives in, the same way NO_SKY_M2 and
            // OPAQUE_ONLY narrowed it to the blended layers at all.
            if (skyMode_) {
                static const int skipBlend = [] {
                    const char* set = std::getenv("WOWEE_SKY_M2_SKIP_BLEND");
                    return set ? std::atoi(set) : -1;
                }();
                if (skipBlend >= 0 && batch.blendMode == skipBlend) continue;
            }

            // Skip glow sprites (handled in opaque pass)
            const bool batchUnlit = (batch.materialFlags & 0x01) != 0;
            M2GlowCardBatch glowCard;
            glowCard.glowSize = batch.glowSize;
            glowCard.blendMode = batch.blendMode;
            glowCard.lanternGlowHint = batch.lanternGlowHint;
            glowCard.glowCardLike = batch.glowCardLike;
            glowCard.colorKeyBlack = batch.colorKeyBlack;
            glowCard.unlit = batchUnlit;
            glowCard.preserveGlowMesh = batch.preserveGlowMesh;
            glowCard.modelIsElvenLike = model.isElvenLike;
            glowCard.modelIsLanternLike = model.isLanternLike;
            glowCard.modelIsTorch = model.isTorch;
            glowCard.modelIsBrazierOrFire = model.isBrazierOrFire;
            glowCard.modelIsSpellEffect = model.isSpellEffect;
            glowCard.modelIsKoboldFlame = model.isKoboldFlame;
            if (m2WantsGlowSprite(glowCard) && m2GlowSpriteReplacesMesh(glowCard)) {
                continue;
            }

            if (particleDominantEffect) continue; // emission-only mesh

            // Compute UV offset for this instance + batch
            //
            // WOWEE_SKY_M2_NO_TEXANIM=1 freezes the sky's scrolling. Nine of
            // its eighteen additive layers carry a texture animation, and
            // those offsets are the one thing about those layers that changes
            // between frames at all - so if the flicker survives freezing them
            // it is not in what the layers sample, it is in the geometry they
            // are drawn on.
            static const bool skyNoTexAnim =
                std::getenv("WOWEE_SKY_M2_NO_TEXANIM") != nullptr;
            glm::vec2 uvOffset(0.0f);
            if (batch.textureAnimIndex != 0xFFFF && model.hasTextureAnimation &&
                !(skyMode_ && skyNoTexAnim)) {
                uint16_t lookupIdx = batch.textureAnimIndex;
                if (lookupIdx < model.textureTransformLookup.size()) {
                    uint16_t transformIdx = model.textureTransformLookup[lookupIdx];
                    if (transformIdx < model.textureTransforms.size()) {
                        const auto& tt = model.textureTransforms[transformIdx];
                        glm::vec3 trans = m2_track::sampleVec3(
                            tt.translation, instance.currentSequenceIndex,
                            instance.animTime, instance.globalSequenceTime,
                            model.globalSequenceDurations, glm::vec3(0.0f));
                        uvOffset = glm::vec2(trans.x, trans.y);
                    }
                }
            }
            if (model.isLavaModel && uvOffset == glm::vec2(0.0f)) {
                uvOffset = glm::vec2(lavaAnimSeconds * 0.03f,
                                     -lavaAnimSeconds * 0.08f);
            }

            // Write single instance entry to SSBO
            if (instanceDataCount_ >= MAX_INSTANCE_DATA) continue;
            uint32_t drawOffset = instanceDataCount_;
            auto& e = instSSBO[instanceDataCount_];
            e.model = instance.modelMatrix;
            e.uvOffset = uvOffset;
            e.fadeAlpha = instanceFadeAlpha;
            e.useBones = (needsBones && !kM2NoSkinning) ? 1 : 0;
            e.boneBase = needsBones ? static_cast<int32_t>(instance.megaBoneOffset) : 0;
            e.boneCount = static_cast<int32_t>(instance.boneMatrices.size());
            e.highlight = instance.highlight;
            e.flags = (instance.afloat > 0 ? 1 : 0) | instance.groundNormalFlags;
            instanceDataCount_++;

            // Pipeline selection
            uint8_t effectiveBlendMode = batch.blendMode;
            if (model.isSpellEffect || batch.forgeFireCard) {
                // Matches the opaque pass: a forge's flame cards are additive,
                // the forge itself is not.
                if (effectiveBlendMode <= 1) effectiveBlendMode = 3;
                else if (effectiveBlendMode == 4 || effectiveBlendMode == 5) effectiveBlendMode = 3;
            }

            VkPipeline desiredPipeline;
            switch (effectiveBlendMode) {
                case 2: desiredPipeline = alphaPipeline_; break;
                default: desiredPipeline = additivePipeline_; break;
            }
            // An opaque layer of a faded instance: blended by its fade.
            if (instanceFaded && effectiveBlendMode <= 1) desiredPipeline = alphaPipeline_;
            if (desiredPipeline != currentPipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
                currentPipeline = desiredPipeline;
            }

            if (batch.materialUBOMapped) {
                auto* mat = static_cast<M2MaterialUBO*>(batch.materialUBOMapped);
                // Here as well as in the opaque pass. A searchlight beam is
                // additive, so this is the pass that actually draws it - and
                // setting the flag only in the other one meant the softening
                // never ran on the one model it was written for.
                mat->volumetricBeam =
                    (model.isVolumetricBeam || batch.volumetricBeam) ? 1 : 0;
                mat->fireCard = batch.forgeFireCard ? 1 : 0;
                mat->interiorDarken = 0.0f;
                if (batch.colorKeyBlack)
                    mat->colorKeyThreshold = (effectiveBlendMode == 4 || effectiveBlendMode == 5) ? 0.7f : 0.08f;

                // A fire's own glow breathes. The same clock and parameters as
                // the lamp sprites and the local light they cast, so a brazier
                // and the pool of light under it rise and fall together
                // instead of beating against each other.
                //
                // Never for a sky. lampFlicker is keyed on the instance
                // position and says a seed that drifts re-rolls its phase every
                // frame and strobes; a sky dome's position IS the camera's,
                // rewritten every frame, so it is the one instance that can
                // never be a valid seed. The classifier no longer calls
                // HellfireSkyBox a brazier, and this makes sure the next model
                // that gets miscalled one cannot strobe the sky either.
                if (!skyMode_ && m2BlendIsAdditive(batch.blendMode) &&
                    (model.isBrazierOrFire || model.isTorch || model.isLanternLike)) {
                    const float flicker = lampFlicker(
                        instance.position, lampFlickerClockSeconds(),
                        0.82f, 0.12f, 0.06f);
                    mat->tintR = batch.tint.r * flicker;
                    mat->tintG = batch.tint.g * flicker;
                    mat->tintB = batch.tint.b * flicker;
                }
            }

            if (!batch.materialSet) continue;
            if (batch.materialSet != currentMaterialSet) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipelineLayout_, 1, 1, &batch.materialSet, 0, nullptr);
                currentMaterialSet = batch.materialSet;
            }

            // Push constants + single-instance draw
            M2PushConstants pc;
            pc.texCoordSet = static_cast<int32_t>(batch.textureUnit);
            fillSway(pc, model, skyMode_);
            pc.instanceDataOffset = static_cast<int32_t>(drawOffset);
            vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
            vkCmdDrawIndexed(cmd, batch.indexCount, 1, batch.indexStart, 0, 0);
            if (skyMode_) ++skyDiagDrawsTransparent_;
            lastDrawCallCount++;
        }
    }

    // Render glow sprites as billboarded additive point lights
    if (!glowSprites_.empty() && particleAdditivePipeline_ && glowVB_ && glowTexDescSet_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particleAdditivePipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                particlePipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                particlePipelineLayout_, 1, 1, &glowTexDescSet_, 0, nullptr);

        // Push constants for particle: tileCount(vec2) + alphaKey(int)
        struct { float tileX, tileY; int alphaKey; } particlePush = {.tileX = 1.0f, .tileY = 1.0f, .alphaKey = 0};
        vkCmdPushConstants(cmd, particlePipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(particlePush), &particlePush);

        // Write glow vertex data directly to mapped buffer (no temp vector)
        size_t uploadCount = std::min(glowSprites_.size(), MAX_GLOW_SPRITES);
        float* dst = static_cast<float*>(glowVBMapped_);
        for (size_t gi = 0; gi < uploadCount; gi++) {
            const auto& gs = glowSprites_[gi];
            *dst++ = gs.worldPos.x;
            *dst++ = gs.worldPos.y;
            *dst++ = gs.worldPos.z;
            *dst++ = gs.color.r;
            *dst++ = gs.color.g;
            *dst++ = gs.color.b;
            *dst++ = gs.color.a;
            *dst++ = gs.size;
            *dst++ = 0.0f;
        }

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &glowVB_, &offset);
        vkCmdDraw(cmd, static_cast<uint32_t>(uploadCount), 1, 0, 0);
    }

    // How many of the sky's layers were actually drawn this frame.
    //
    // The instance-level report says DRAWN every frame, which is a different
    // question: it counts instances that survived culling, not batches that
    // reached a draw call. Every per-batch gate between the two is meant to be
    // constant here - the material flags are static, the LOD is chosen by a
    // distance that does not change for a dome centred on the camera, and the
    // batch opacity is baked at load - so this number should never move. If it
    // moves while the camera turns, the flicker is layers appearing and
    // disappearing and one of those gates is not as constant as it reads.
    if (skyMode_ && (skyDiagDrawsOpaque_ != skyDiagLastOpaque_ ||
                     skyDiagDrawsTransparent_ != skyDiagLastTransparent_)) {
        LOG_INFO("skyM2 draws: opaque=", skyDiagDrawsOpaque_,
                 " transparent=", skyDiagDrawsTransparent_,
                 " (was ", skyDiagLastOpaque_, "/", skyDiagLastTransparent_, ")");
        skyDiagLastOpaque_ = skyDiagDrawsOpaque_;
        skyDiagLastTransparent_ = skyDiagDrawsTransparent_;
    }

    if (m2Profile) {
        const auto m2T3 = std::chrono::steady_clock::now();
        const auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        sayPhases({.cull = ms(m2T0, m2T1),
                   .sort = ms(m2T1, m2T2),
                   .record = ms(m2T2, m2T3),
                   .visible = sortedVisible_.size(),
                   .instances = instances.size()});
    }
}

bool M2Renderer::initializeShadow(VkRenderPass shadowRenderPass) {
    if (!vkCtx_ || shadowRenderPass == VK_NULL_HANDLE) return false;
    VkDevice device = vkCtx_->getDevice();

    // The set the shadow pass binds, built the same way for all four renderers.
    if (!createShadowParamsSet(device, vkCtx_->getAllocator(), sizeof(ShadowParamsUBO),
                               whiteTexture_->getImageView(),
                              whiteTexture_->getSampler(), "M2Renderer", shadowParams_)) {
        return false;
    }

    // Per-frame pools for foliage shadow texture sets (one per frame-in-flight, reset each frame)
    {
        VkDescriptorPoolSize texPoolSizes[2]{};
        texPoolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texPoolSizes[0].descriptorCount = 256;
        texPoolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        texPoolSizes[1].descriptorCount = 256;
        VkDescriptorPoolCreateInfo texPoolCI{};
        texPoolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        texPoolCI.maxSets = 256;
        texPoolCI.poolSizeCount = 2;
        texPoolCI.pPoolSizes = texPoolSizes;
        for (uint32_t f = 0; f < kShadowTexPoolFrames; ++f) {
            if (vkCreateDescriptorPool(device, &texPoolCI, nullptr, &shadowTexPool_[f]) != VK_SUCCESS) {
                LOG_ERROR("M2Renderer: failed to create shadow texture pool ", f);
                return false;
            }
        }
    }

    // Create shadow pipeline layout: set 1 = shadowParams_.layout, push constants = 128 bytes
    VkPushConstantRange pc{};
    // The fragment stage reads the alpha-test flags out of the same block.
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = sizeof(ShadowPush);  // one combined matrix, plus the sway slot
    // The casters' model matrices, one buffer per frame in flight, bound as
    // set 1 with the same layout the main pass gives its instance data.
    {
        const VkDeviceSize casterBufSize = MAX_SHADOW_CASTERS * sizeof(glm::mat4);
        VkDescriptorPoolSize poolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2};
        VkDescriptorPoolCreateInfo poolCi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolCi.maxSets = 2;
        poolCi.poolSizeCount = 1;
        poolCi.pPoolSizes = &poolSize;
        if (vkCreateDescriptorPool(device, &poolCi, nullptr, &casterDescPool_) != VK_SUCCESS) {
            LOG_ERROR("M2Renderer: failed to create shadow caster pool");
            return false;
        }
        for (int i = 0; i < 2; i++) {
            VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = casterBufSize;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo allocInfo{};
            if (vmaCreateBuffer(vkCtx_->getAllocator(), &bci, &aci,
                                &casterBuffer_[i], &casterAlloc_[i], &allocInfo) != VK_SUCCESS) {
                LOG_ERROR("M2Renderer: failed to create shadow caster buffer ", i);
                return false;
            }
            casterMapped_[i] = allocInfo.pMappedData;
            VkDescriptorSetAllocateInfo setAi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            setAi.descriptorPool = casterDescPool_;
            setAi.descriptorSetCount = 1;
            setAi.pSetLayouts = &instanceSetLayout_;
            vkAllocateDescriptorSets(device, &setAi, &casterSet_[i]);
            VkDescriptorBufferInfo bufInfo{.buffer = casterBuffer_[i], .offset = 0, .range = casterBufSize};
            VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = casterSet_[i];
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &bufInfo;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
    }
    shadowPipelineLayout_ = createPipelineLayout(device, {shadowParams_.layout, instanceSetLayout_}, {pc});
    if (!shadowPipelineLayout_) {
        LOG_ERROR("M2Renderer: failed to create shadow pipeline layout");
        return false;
    }

    // Load shadow shaders
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/m2_shadow.vert.spv")) {
        LOG_ERROR("M2Renderer: failed to load shadow vertex shader");
        return false;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/shadow.frag.spv")) {
        LOG_ERROR("M2Renderer: failed to load shadow fragment shader");
        return false;
    }

    // M2 vertex layout: 18 floats = 72 bytes stride
    // loc0=pos(off0), loc1=normal(off12), loc2=texCoord0(off24), loc5=texCoord1(off32),
    // loc3=boneWeights(off40), loc4=boneIndices(off56)
    // Shadow shader locations: 0=aPos, 1=aTexCoord, 2=aBoneWeights, 3=aBoneIndicesF
    // useBones=0 so locations 2,3 are never used
    VkVertexInputBindingDescription vertBind{};
    vertBind.binding = 0;
    vertBind.stride = 18 * sizeof(float);
    vertBind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::vector<VkVertexInputAttributeDescription> vertAttrs = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,    .offset = 0},                     // aPos       -> position
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,       .offset = 6 * sizeof(float)},     // aTexCoord  -> texCoord0
        {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 10 * sizeof(float)},    // aBoneWeights
        {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 14 * sizeof(float)},    // aBoneIndicesF
    };

    shadowPipeline_ = buildShadowPipeline(
        device, vkCtx_->getPipelineCache(),
        vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
        fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT),
        vertBind, vertAttrs, shadowPipelineLayout_, shadowRenderPass,
        vkCtx_->useDynamicRendering());

    vertShader.destroy();
    fragShader.destroy();

    if (!shadowPipeline_) {
        LOG_ERROR("M2Renderer: failed to create shadow pipeline");
        return false;
    }
    LOG_INFO("M2Renderer shadow pipeline initialized");
    return true;
}

void M2Renderer::renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix, float globalTime,
                              const glm::vec3& shadowCenter, float shadowRadius) {
    if (!shadowPipeline_ || !shadowParams_.set) return;
    if (instances.empty() || models.empty()) return;

    // Reset this frame slot's texture descriptor pool (safe: fence was waited on in beginFrame)
    const uint32_t frameIdx = vkCtx_->getCurrentFrame();
    VkDescriptorPool curShadowTexPool = shadowTexPool_[frameIdx];
    if (curShadowTexPool) {
        vkResetDescriptorPool(vkCtx_->getDevice(), curShadowTexPool, 0);
    }
    // Cache: texture imageView -> allocated descriptor set (avoids duplicates within frame)
    // Reuse persistent map - pool reset already invalidated the sets.
    shadowTexSetCache_.clear();
    auto& texSetCache = shadowTexSetCache_;

    auto getTexDescSet = [&](VkTexture* tex) -> VkDescriptorSet {
        VkImageView iv = tex->getImageView();
        auto cacheIt = texSetCache.find(iv);
        if (cacheIt != texSetCache.end()) return cacheIt->second;

        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = curShadowTexPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &shadowParams_.layout;
        if (vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &set) != VK_SUCCESS) {
            return shadowParams_.set; // fallback to white texture
        }
        VkDescriptorImageInfo imgInfo{};
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfo.imageView = iv;
        imgInfo.sampler = tex->getSampler();
        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = shadowParams_.ubo;
        bufInfo.offset = 0;
        bufInfo.range = sizeof(ShadowParamsUBO);
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &imgInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &bufInfo;
        vkUpdateDescriptorSets(vkCtx_->getDevice(), 2, writes, 0, nullptr);
        texSetCache[iv] = set;
        return set;
    };

    // How many casters each pass drew, so a shadow that comes and goes can say
    // whether its caster was culled or its texture was missing.
    uint32_t castersDrawn[2] = {0, 0};

    // Cull once, into the two passes, grouped by model.
    //
    // This walked every instance in the world twice - once per pass - doing a
    // light-space transform for each, then drew them in whatever order they
    // sat in: the vertex and index buffers were rebound whenever consecutive
    // instances came from different models, which unsorted is most of them,
    // and a batch's texture was bound once per instance rather than once per
    // batch. With tens of thousands of instances resident that is the cost of
    // this pass, and none of it is drawing.
    shadowCasters_[0].clear();
    shadowCasters_[1].clear();

    // Who is worth asking. Walking every instance in the world here was
    // 3.2 ms a frame at 85k instances, almost all of it rejecting things far
    // outside the shadow disc. So the walk runs a few times a second, keeps
    // whatever could reach the disc with a margin to spare, and each frame
    // asks only those - with the same tests as before, on their current
    // positions, so nothing inside the list is culled differently.
    //
    // What could reach the disc without being on the list, and what keeps
    // each from needing a rebuild:
    // - something moved since the build: setInstancePosition/Transform put it
    //   on shadowMovers_, asked every frame wherever it is now;
    // - something created since: createInstance appends it (noteShadowCandidate);
    // - something removed: removeInstance patches the indices in place;
    // - the centre wandering off: an instance left out was more than
    //   radius + margin from the old centre, so it is still out of reach until
    //   the centre has drifted by the margin. Rebuilt at 90% of it.
    // Only a wholesale reorder (casterLayoutGen_), a new radius, the drift and
    // a two-second backstop rebuild it. It was rebuilt ~8 times a second -
    // every clutter removal and spell visual - at a walk of 85k instances each.
    {
        constexpr float kCandidateMargin = 48.0f;
        constexpr float kRebuildDrift = 0.9f * kCandidateMargin;
        const auto now = std::chrono::steady_clock::now();
        const glm::vec3 drift = shadowCenter - shadowCandCenter_;
        const bool rebuild =
            shadowCandLayoutGen_ != casterLayoutGen_ ||
            std::abs(shadowCandRadius_ - shadowRadius) > 0.01f ||
            glm::dot(drift, drift) > kRebuildDrift * kRebuildDrift ||
            now - shadowCandBuiltAt_ > std::chrono::milliseconds(2000);
        if (rebuild) {
            shadowCandidates_.clear();
            shadowMovers_.clear();
            ++shadowCandStamp_;
            if (shadowCandStamp_ == 0) ++shadowCandStamp_;  // 0 is "never moved"
            for (uint32_t i = 0; i < instances.size(); ++i) {
                const auto& instance = instances[i];
                if (!instance.cachedIsValid || instance.cachedIsSmoke ||
                    instance.cachedIsInvisibleTrap || instance.cachedIsGroundDetail) continue;
                const glm::vec3 d = instance.position - shadowCenter;
                const float reach = shadowRadius + instance.cachedPaddedRadius + kCandidateMargin;
                if (glm::dot(d, d) > reach * reach) continue;
                shadowCandidates_.emplace_back(i, instance.id);
            }
            shadowCandCenter_ = shadowCenter;
            shadowCandRadius_ = shadowRadius;
            shadowCandMargin_ = kCandidateMargin;
            shadowCandLayoutGen_ = casterLayoutGen_;
            shadowCandBuiltAt_ = now;
            ++updateProf_.shadowRebuilds;
        }
        updateProf_.shadowCandidates += shadowCandidates_.size();
        updateProf_.shadowMovers += shadowMovers_.size();
    }

    // The movers join the candidates as (index, id) pairs looked up afresh;
    // a candidate that has since moved is skipped in the list below so it is
    // asked once, here.
    shadowMoverPairs_.clear();
    for (uint32_t moverId : shadowMovers_) {
        const auto it = instanceIndexById.find(moverId);
        if (it == instanceIndexById.end() || it->second >= instances.size()) continue;
        shadowMoverPairs_.emplace_back(static_cast<uint32_t>(it->second), moverId);
    }

    const auto considerCaster = [&](uint32_t i, uint32_t candidateId, bool fromMovers) {
        if (i >= instances.size()) return;
        const auto& instance = instances[i];
        if (instance.id != candidateId) return;
        if (!fromMovers && instance.shadowMoverStamp == shadowCandStamp_) return;
        if (!instance.cachedIsValid || instance.cachedIsSmoke ||
            instance.cachedIsInvisibleTrap) return;
        // A squared distance against the shadow disc, read from the
        // instance's first cache line, settles the far ones before the model
        // pointer and the light-space transform are touched.
        // Grass never cast a shadow in the stock client - doodad shadows were
        // baked into the terrain - and a doodad smaller than a texel of the
        // shadow map at its distance draws for nothing. Both were most of the
        // casters, and each caster is a push constant and a draw per batch.
        if (instance.cachedIsGroundDetail || !instance.castsShadow) return;
        {
            const glm::vec3 d = instance.position - shadowCenter;
            const float reach = shadowRadius + instance.cachedPaddedRadius;
            const float distSq = glm::dot(d, d);
            if (distSq > reach * reach) return;
            // The projected-size cap: a caster is kept while its radius over
            // its distance from the player (whom the camera follows) exceeds
            // 1/ratio - at 150, about six pixels of radius on a 1080p screen.
            // WOWEE_SHADOW_CASTER_RATIO lowers it to drop more small casters.
            static const float kTexelsPerYardOfRadius = [] {
                const char* v = std::getenv("WOWEE_SHADOW_CASTER_RATIO");
                const float r = (v && *v) ? static_cast<float>(std::atof(v)) : 0.0f;
                return r > 1.0f ? r : 150.0f;
            }();
            if (instance.cachedPaddedRadius * kTexelsPerYardOfRadius < std::sqrt(distSq)) return;
        }
        if (!instance.cachedModel) return;
        const M2ModelGPU& model = *instance.cachedModel;
        // A spell kit or missile is drawn additive and unlit - glow cards and
        // particles round a small carrier mesh - and the stock client casts
        // no shadow from one. Drawn here it cast the cards as solid quads,
        // and every cast added casters that followed their bones each frame.
        if (model.isSpellEffect) return;

        // Cull casters against the light-space ortho footprint, not a
        // world-space sphere around the player. The shadow frustum extends
        // ~2000 units toward the sun, so a distant tree can legitimately
        // cast across the whole view while sitting far outside any player
        // sphere - sphere culling made such shadows pop on/off at the cull
        // boundary as the player moved (large-area flicker at low sun).
        const glm::vec4 clip = lightSpaceMatrix * glm::vec4(instance.position, 1.0f);
        // Orthographic projection: w == 1, NDC directly comparable.
        // Inflate by the model's bounding sphere converted to NDC
        // (shadowRadius ≈ frustum half-extent; overshoot is harmless).
        const float margin = (model.boundRadius * instance.scale) / shadowRadius * 1.5f;
        if (std::abs(clip.x) > 1.0f + margin || std::abs(clip.y) > 1.0f + margin) return;
        if (clip.z < -margin || clip.z > 1.0f + margin) return;

        shadowCasters_[model.shadowWindFoliage ? 1 : 0].emplace_back(instance.modelId, i);
    };
    for (const auto& [i, candidateId] : shadowCandidates_) considerCaster(i, candidateId, false);
    for (const auto& [i, moverId] : shadowMoverPairs_) considerCaster(i, moverId, true);
    for (auto& bucket : shadowCasters_) {
        std::sort(bucket.begin(), bucket.end());
    }

    // Helper lambda to draw instances with a given foliageSway setting
    glm::mat4* casterMatrices = static_cast<glm::mat4*>(casterMapped_[frameIdx]);
    uint32_t castersWritten = 0;
    auto drawPass = [&](bool foliagePass) {
        // What this pass is, carried with each draw rather than written into a
        // buffer both passes share. The uniform buffer this used to be is read
        // when a draw executes, not when it is recorded, so writing it again
        // for the foliage pass decided what the solid pass's draws saw as
        // well - and one mapped copy across frames in flight let a CPU write
        // land inside the previous frame's reads. A foliage batch that lost
        // its alpha test that way casts the whole leaf quad, which is the
        // canopy's outline in solid black instead of its cutout.
        const glm::ivec4 passFlags{foliagePass ? 1 : 0, foliagePass ? 1 : 0,
                                   foliagePass ? 1 : 0, 0};
        const glm::vec4 wind{globalTime, 0.0f, 0.0f, 0.0f};

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_,
            0, 1, &shadowParams_.set, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_,
            1, 1, &casterSet_[frameIdx], 0, nullptr);

        const auto& casters = shadowCasters_[foliagePass ? 1 : 0];
        for (std::size_t g = 0; g < casters.size();) {
            const uint32_t modelId = casters[g].first;
            std::size_t groupEnd = g;
            while (groupEnd < casters.size() && casters[groupEnd].first == modelId) ++groupEnd;

            const auto& firstInstance = instances[casters[g].second];
            if (!firstInstance.cachedModel) { g = groupEnd; continue; }
            const M2ModelGPU& model = *firstInstance.cachedModel;

            // Once per model, not once per instance: the bend comes from the
            // model's own bounds and its kind, and so do the buffers.
            const M2Sway sway = m2SwayFor(false, model.isHangingCloth,
                                          model.shadowWindFoliage, model.isGroundDetail,
                                          model.boundMin.z, model.boundMax.z,
                                          model.isStandingCloth);
            const glm::vec2 modelSwayZW(sway.refHeight, sway.amp);
            // The group's matrices go into the caster buffer once; every batch
            // below draws them all with a single instanced draw.
            const uint32_t groupSize = static_cast<uint32_t>(groupEnd - g);
            if (!casterMatrices || castersWritten + groupSize > MAX_SHADOW_CASTERS) { g = groupEnd; continue; }
            const uint32_t casterBase = castersWritten;
            for (std::size_t k = g; k < groupEnd; ++k) {
                casterMatrices[castersWritten++] = instances[casters[k].second].modelMatrix;
            }
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &model.vertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, model.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

            for (const auto& batch : model.batches) {
                if (batch.submeshLevel > 0) continue;
                // A leaf card with no texture is a shadow with no caster.
                //
                // The else below binds the white set, whose alpha test passes
                // everywhere, so a batch that arrived without its texture casts
                // the shadow of a solid quad - while the main pass skips that
                // same batch for want of a material set. The result is a black
                // rectangle on the ground under a tree that looks fine, coming
                // and going as the texture cache lets the sheet back in.
                if (foliagePass && !batch.texture) {
                    if (!warnedShadowNoTexture_) {
                        warnedShadowNoTexture_ = true;
                        LOG_WARNING("Shadow pass: a foliage batch of ", model.name,
                                    " has no texture; skipping it rather than casting a "
                                    "solid quad. The texture cache is the usual reason.");
                    }
                    continue;
                }
                // Once per batch, where it used to be once per batch per
                // instance: the texture is the batch's, and every instance of
                // this model shares it.
                if (foliagePass && batch.hasAlpha && batch.texture) {
                    VkDescriptorSet texSet = getTexDescSet(batch.texture);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        shadowPipelineLayout_, 0, 1, &texSet, 0, nullptr);
                } else if (foliagePass) {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        shadowPipelineLayout_, 0, 1, &shadowParams_.set, 0, nullptr);
                }

                ShadowPush push{
                    .lightSpaceModel = lightSpaceMatrix,  // light space only; the model is per caster
                    .sway = glm::vec4(0.0f, 0.0f, modelSwayZW.x, modelSwayZW.y),
                    .flags = glm::ivec4(passFlags.x, passFlags.y, passFlags.z, static_cast<int>(casterBase)),
                    .wind = wind};
                vkCmdPushConstants(cmd, shadowPipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(ShadowPush), &push);
                vkCmdDrawIndexed(cmd, batch.indexCount, groupSize, batch.indexStart, 0, 0);
            }
            castersDrawn[foliagePass ? 1 : 0] += static_cast<uint32_t>(groupEnd - g);
            g = groupEnd;
        }
    };

    // Pass 1: non-foliage (no wind displacement)
    drawPass(false);
    // Pass 2: foliage (wind displacement enabled, per-batch alpha-tested textures)
    drawPass(true);

    // A shadow that flickers is a caster that was drawn last frame and is not
    // drawn this one. The cull is in light space and the light turns with the
    // hour, so a tree on the boundary can cross it while the player stands
    // still - and from the ground that reads as the shadow blinking. Said at
    // most once a second, and only when the count actually swings, so an
    // ordinary walk through a forest stays quiet.
    updateProf_.shadowCasters += castersDrawn[0] + castersDrawn[1];
    {
        const uint32_t foliageNow = castersDrawn[1];
        const uint32_t foliageWas = lastFoliageCasters_;
        lastFoliageCasters_ = foliageNow;
        const uint32_t larger = std::max(foliageNow, foliageWas);
        const uint32_t delta = larger - std::min(foliageNow, foliageWas);
        if (foliageWas != 0 && larger >= 8 && delta * 10 > larger) {
            static std::chrono::steady_clock::time_point lastLog{};
            const auto now = std::chrono::steady_clock::now();
            if (now - lastLog > std::chrono::seconds(1)) {
                lastLog = now;
                LOG_WARNING("Shadow casters swung ", foliageWas, " -> ", foliageNow,
                            " foliage instances in one frame (", castersDrawn[0],
                            " solid). A shadow that blinks with the player standing "
                            "still is one of these crossing the light-space cull.");
            }
        }
    }
}

} // namespace rendering
} // namespace wowee
