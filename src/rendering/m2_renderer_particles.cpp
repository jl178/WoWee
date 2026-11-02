#include "rendering/m2_renderer.hpp"
#include <unordered_set>
#include "rendering/m2_renderer_internal.h"
#include "rendering/m2_particle_look.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/camera.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <random>
#include <limits>
#include <mutex>
#include <set>

namespace wowee {
namespace rendering {

// --- M2 Particle Emitter Helpers ---

float M2Renderer::interpFloat(const pipeline::M2AnimationTrack& track, float animTime,
                                float globalTime, int seqIdx,
                                const std::vector<uint32_t>& globalSeqDurations) {
    return m2_track::sampleFloat(track, seqIdx, animTime, globalTime,
                                 globalSeqDurations, 0.0f);
}

// Interpolate an M2 FBlock (particle lifetime curve) at a given life ratio [0..1].
// FBlocks store per-lifetime keyframes for particle color, alpha, and scale.
// NOTE: interpFBlockFloat and interpFBlockVec3 share identical interpolation logic -
// if you fix a bug in one, update the other to match.
float M2Renderer::interpFBlockFloat(const pipeline::M2FBlock& fb, float lifeRatio) {
    if (fb.floatValues.empty()) return 1.0f;
    if (fb.floatValues.size() == 1 || fb.timestamps.empty()) return fb.floatValues[0];
    lifeRatio = glm::clamp(lifeRatio, 0.0f, 1.0f);
    for (size_t i = 0; i < fb.timestamps.size() - 1; i++) {
        if (lifeRatio <= fb.timestamps[i + 1]) {
            float t0 = fb.timestamps[i];
            float t1 = fb.timestamps[i + 1];
            float dur = t1 - t0;
            float frac = (dur > 0.0f) ? (lifeRatio - t0) / dur : 0.0f;
            size_t v0 = std::min(i, fb.floatValues.size() - 1);
            size_t v1 = std::min(i + 1, fb.floatValues.size() - 1);
            return glm::mix(fb.floatValues[v0], fb.floatValues[v1], frac);
        }
    }
    return fb.floatValues.back();
}

glm::vec3 M2Renderer::interpFBlockVec3(const pipeline::M2FBlock& fb, float lifeRatio) {
    if (fb.vec3Values.empty()) return glm::vec3(1.0f);
    if (fb.vec3Values.size() == 1 || fb.timestamps.empty()) return fb.vec3Values[0];
    lifeRatio = glm::clamp(lifeRatio, 0.0f, 1.0f);
    for (size_t i = 0; i < fb.timestamps.size() - 1; i++) {
        if (lifeRatio <= fb.timestamps[i + 1]) {
            float t0 = fb.timestamps[i];
            float t1 = fb.timestamps[i + 1];
            float dur = t1 - t0;
            float frac = (dur > 0.0f) ? (lifeRatio - t0) / dur : 0.0f;
            size_t v0 = std::min(i, fb.vec3Values.size() - 1);
            size_t v1 = std::min(i + 1, fb.vec3Values.size() - 1);
            return glm::mix(fb.vec3Values[v0], fb.vec3Values[v1], frac);
        }
    }
    return fb.vec3Values.back();
}

std::vector<glm::vec3> M2Renderer::getWaterVegetationPositions(const glm::vec3& camPos, float maxDist) const {
    std::vector<glm::vec3> result;
    float maxDistSq = maxDist * maxDist;
    // The same list, for the same reason; see renderM2Particles. A ribbon
    // emitter is a particle emitter as far as this list is concerned.
    for (size_t idx : particleInstanceIndices_) {
        if (idx >= instances.size()) continue;
        const auto& inst = instances[idx];
        if (!inst.cachedModel || !inst.cachedModel->isWaterVegetation) continue;
        glm::vec3 diff = inst.position - camPos;
        if (glm::dot(diff, diff) <= maxDistSq) {
            result.push_back(inst.position);
        }
    }
    return result;
}

void M2Renderer::ensureParticleCurves(const M2ModelGPU& gpu) {
    if (gpu.particleTracks.size() != gpu.particleEmitters.size()) {
        std::vector<M2ModelGPU::ParticleTracks> tracks(gpu.particleEmitters.size());
        const auto& gsd = gpu.globalSequenceDurations;
        for (size_t e = 0; e < tracks.size(); ++e) {
            const auto& em = gpu.particleEmitters[e];
            auto& t = tracks[e];
            t.rate = m2_particle::classifyTrack(em.emissionRate, gsd);
            t.life = m2_particle::classifyTrack(em.lifespan, gsd);
            t.speed = m2_particle::classifyTrack(em.emissionSpeed, gsd);
            t.speedVar = m2_particle::classifyTrack(em.speedVariation, gsd);
            t.vRange = m2_particle::classifyTrack(em.verticalRange, gsd);
            t.hRange = m2_particle::classifyTrack(em.horizontalRange, gsd);
            t.gravity = m2_particle::classifyTrack(em.gravity, gsd);
            t.areaL = m2_particle::classifyTrack(em.emissionAreaLength, gsd);
            t.areaW = m2_particle::classifyTrack(em.emissionAreaWidth, gsd);
        }
        gpu.particleTracks = std::move(tracks);
    }
    if (gpu.particleCurves.size() == gpu.particleEmitters.size()) return;
    using Curve = M2ModelGPU::ParticleCurve;
    std::vector<Curve> curves(gpu.particleEmitters.size());
    for (size_t e = 0; e < curves.size(); ++e) {
        const auto& em = gpu.particleEmitters[e];
        for (int k = 0; k <= Curve::kSteps; ++k) {
            const float r = static_cast<float>(k) / static_cast<float>(Curve::kSteps);
            const glm::vec3 c = interpFBlockVec3(em.particleColor, r);
            const float a = std::min(interpFBlockFloat(em.particleAlpha, r), 1.0f);
            curves[e].colorAlpha[k] = glm::vec4(c, a);
            curves[e].scale[k] = interpFBlockFloat(em.particleScale, r);
        }
    }
    gpu.particleCurves = std::move(curves);
}

namespace {
// Moves a particle t seconds along under gravity g (down world or bone Z) and
// drag k, in closed form: what the per-frame step does in many small steps,
// for a particle born part way through a step or warmed up on waking.
inline void advanceParticle(glm::vec3& pos, glm::vec3& vel, float g, float k, float t) {
    if (t <= 0.0f) return;
    if (k > 1e-4f) {
        const float e = std::exp(-k * t);
        const glm::vec3 vInf(0.0f, 0.0f, -g / k);
        pos += vInf * t + (vel - vInf) * ((1.0f - e) / k);
        vel = vInf + (vel - vInf) * e;
    } else {
        pos += vel * t;
        pos.z -= 0.5f * g * t * t;
        vel.z -= g * t;
    }
}
} // namespace

void M2Renderer::emitParticles(M2Instance& inst, const M2ModelGPU& gpu, float dt,
                               ParticleWorker* worker) {
    // Instance portals were skipped here and drawn as a glow sprite instead,
    // but the portal IS its emitters: InstancePortal.m2's only mesh has a
    // texture weight of 0, and the blue vortex is two sphere emitters on
    // bones spinning about the model X axis. Skipping them left nothing but
    // the sprite on the ground.
    if (inst.emissionStopped) return;
    if (!(dt > 0.0f)) return;
    ensureParticleCurves(gpu);

    if (inst.emitterAccumulators.size() != gpu.particleEmitters.size()) {
        inst.emitterAccumulators.resize(gpu.particleEmitters.size(), 0.0f);
    }

    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    std::uniform_real_distribution<float> distN(-1.0f, 1.0f);
    std::uniform_int_distribution<int> distTile;
    const glm::mat4 identity(1.0f);

    // Where births go and whose random stream they draw from. Without a
    // worker, straight onto the instance from the renderer's stream; with
    // one, onto the worker's list, merged in by emitter at the end. The
    // count against MAX_M2_PARTICLES is the instance's either way.
    std::mt19937& rng = worker ? worker->rng : particleRng_;
    std::vector<M2Particle>& out = worker ? worker->births : inst.particles;
    if (worker) worker->births.clear();
    const size_t outBase = worker ? inst.particles.size() : 0;
    const bool fastTracks = worker && gpu.particleTracks.size() == gpu.particleEmitters.size();

    for (size_t ei = 0; ei < gpu.particleEmitters.size(); ei++) {
        const auto& em = gpu.particleEmitters[ei];
        if (!em.enabled) continue;

        // The same value either way; the classified track skips the key
        // search when the track holds one value (m2_particle::TrackConst).
        const auto* tracks = fastTracks ? &gpu.particleTracks[ei] : nullptr;
        const auto sampleTrack = [&](const m2_particle::TrackConst M2ModelGPU::ParticleTracks::* c,
                                     const pipeline::M2AnimationTrack& t) {
            return tracks
                ? m2_particle::sampleTrack(tracks->*c, t, inst.currentSequenceIndex,
                                           inst.animTime, inst.globalSequenceTime,
                                           gpu.globalSequenceDurations)
                : interpFloat(t, inst.animTime, inst.globalSequenceTime,
                              inst.currentSequenceIndex, gpu.globalSequenceDurations);
        };
        using PT = M2ModelGPU::ParticleTracks;
        float rate = sampleTrack(&PT::rate, em.emissionRate);
        float life = sampleTrack(&PT::life, em.lifespan);
        // What the player asked to see of it, before the floor below. The order
        // is the whole point: thinning first and flooring second lets a low
        // setting take smoke, dust and spell effects down while a candle is
        // pulled back up to the handful of particles that still reads as fire.
        rate *= particleDensity_;

        // A flame reads as a flame only when enough particles are alive at once.
        // Authored rates vary wildly for the same visual intent - a candle asks
        // for 40/s over half a second, CHANDELIER01 for 1/s over six seconds,
        // which sustains a single speck per candle and looks like a bare glow.
        // Steady-state population is rate x lifespan, so floor the rate against
        // the lifespan to hold every fixture at a comparable density. Do not lower
        // this: at 7 the flames disappear entirely, as they do under any change
        // that reduces how many particles are alive or how far they travel. The
        // effect is only visible because a scattering of particles reaches open
        // air, so thinning it drops the whole thing below the threshold.
        if (rate > 0.0f && life > 0.0f &&
            (gpu.isLanternLike || gpu.isTorch || gpu.isBrazierOrFire || gpu.isKoboldFlame)) {
            constexpr float kMinLiveParticles = 15.0f;
            rate = std::max(rate, kMinLiveParticles / std::max(life, 0.1f));
        }

        if (rate <= 0.0f || life <= 0.0f) {
            // Diagnostic: a lamp or flame whose emitter never fires produces a
            // fixture that glows but shows no flame. Report each model once so a
            // default-level log says whether emission is the cause.
            if (gpu.isLanternLike || gpu.isTorch || gpu.isBrazierOrFire) {
                // Locked: the step runs on more than one thread.
                static std::mutex reportedMutex;
                static std::unordered_set<std::string> reported;
                bool first;
                {
                    std::lock_guard<std::mutex> lock(reportedMutex);
                    first = reported.insert(gpu.name).second;
                }
                if (first) {
                    LOG_WARNING("Flame emitter idle: '", gpu.name, "' emitter=", ei,
                                " rate=", rate, " life=", life,
                                " animTime=", inst.animTime,
                                " seqIdx=", inst.currentSequenceIndex,
                                " gsTime=", inst.globalSequenceTime,
                                " rateSeqs=", em.emissionRate.sequences.size(),
                                " lifeSeqs=", em.lifespan.sequences.size(),
                                " rateGlobalSeq=", em.emissionRate.globalSequence);
                }
            }
            continue;
        }

        // Only the last lifespan of the window can still be alive at its end,
        // so a long window (a dormant emitter waking) spawns that and no more:
        // the standing population, at evenly spread ages.
        const float window = std::min(dt, life);
        float& acc = inst.emitterAccumulators[ei];
        acc += rate * window;
        if (acc < 1.0f) continue;  // nothing due: skip the spawn-track sampling

        // Everything below depends on the emitter and the clock, not on the
        // particle: sampled once per emitter step, not once per particle.
        const float baseSpeed = sampleTrack(&PT::speed, em.emissionSpeed);
        const float vRange = sampleTrack(&PT::vRange, em.verticalRange);
        const float hRange = sampleTrack(&PT::hRange, em.horizontalRange);
        const float speedVar = sampleTrack(&PT::speedVar, em.speedVariation);
        const bool isSphere = (em.emitterType == kParticleEmitterSphere);
        float rMax = 0.0f, rMin = 0.0f;
        if (isSphere) {
            const float areaL = sampleTrack(&PT::areaL, em.emissionAreaLength);
            const float areaW = sampleTrack(&PT::areaW, em.emissionAreaWidth);
            rMax = std::max(areaL, areaW);
            rMin = std::min(areaL, areaW);
        }
        const float gravity = sampleTrack(&PT::gravity, em.gravity);
        const float drag = em.drag;
        const glm::mat4& boneXform = (em.bone < inst.boneMatrices.size())
            ? inst.boneMatrices[em.bone] : identity;
        const glm::mat4 xf = inst.modelMatrix * boneXform;
        const glm::mat3 rotMat(xf);
        const bool noTrail = (em.flags & kParticleFlagNoTrail) != 0;
        const uint32_t tilesX = std::max<uint16_t>(em.textureCols, 1);
        const uint32_t tilesY = std::max<uint16_t>(em.textureRows, 1);
        const uint32_t totalTiles = tilesX * tilesY;
        const bool randomTile = (em.flags & kParticleFlagTiled) && totalTiles > 1 &&
                                (em.flags & kParticleFlagRandomized);
        if (randomTile) {
            distTile = std::uniform_int_distribution<int>(0, static_cast<int>(totalTiles - 1));
        }
        const float invRate = 1.0f / rate;

        while (acc >= 1.0f && outBase + out.size() < MAX_M2_PARTICLES) {
            acc -= 1.0f;
            // Born when the accumulator crossed this integer: what is left
            // of it is how far into the window that was from the end.
            const float age = std::min(acc * invRate, window);

            M2Particle p;
            p.emitterIndex = static_cast<uint16_t>(ei);
            p.life = age;
            p.maxLife = life;
            p.tileIndex = 0;

            // Position: emitter position transformed by bone matrix
            glm::vec3 localPos = em.position;
            p.position = glm::vec3(xf * glm::vec4(localPos, 1.0f));

            // Velocity: emission speed in upward direction + random spread
            float speed = baseSpeed * (1.0f + distN(rng) * speedVar);

            glm::vec3 dir;
            if (isSphere) {
                // Sphere emitter: born on a shell around the emitter and sent
                // out along the radius (in, for a negative speed). The polar
                // angle from bone +Z spans +-verticalRange and the azimuth
                // +-horizontalRange, azimuth 0 being bone +Y. That zero is
                // fixed by InstancePortal.m2: vRange pi, hRange 0, a 4.17
                // shell, speed -3.3, on bones spinning about X - a ring in
                // the YZ plane of the portal's own frame, drawn inward and
                // spun into the vortex. Any other zero puts the ring edge-on.
                const float theta = distN(rng) * vRange;
                const float phi = distN(rng) * hRange;
                dir = glm::vec3(std::sin(theta) * std::sin(phi),
                                std::sin(theta) * std::cos(phi),
                                std::cos(theta));
                const float radius = (rMin > 0.0f)
                    ? rMin + (rMax - rMin) * dist01(rng)
                    : rMax;
                if (radius > 0.0f) {
                    localPos += dir * radius;
                    p.position = glm::vec3(xf * glm::vec4(localPos, 1.0f));
                }
            } else {
                // Emitter +Z tilted by a random polar angle within verticalRange
                // (radians) at a random azimuth. vRange 0 = straight up the bone Z.
                const float theta = distN(rng) * vRange;
                const float phi = dist01(rng) * 6.2831853f;
                dir = glm::vec3(std::sin(theta) * std::cos(phi),
                                std::sin(theta) * std::sin(phi),
                                std::cos(theta));
            }

            glm::vec3 localVel = dir * speed;
            // When emission speed is ~0 and bone animation isn't loaded (.anim files),
            // particles pile up at the same position. Give them a drift so they
            // spread outward like a mist/spray effect instead of clustering.
            if (std::abs(speed) < 0.01f) {
                // Authored zero speed means the particle stays where it was
                // born (candle/chandelier glow cards). Only fireflies drift.
                if (gpu.isFireflyEffect) {
                    localVel = glm::vec3(
                        distN(rng) * 0.6f,
                        distN(rng) * 0.6f,
                        distN(rng) * 0.3f
                    );
                } else {
                    localVel = glm::vec3(0.0f);
                }
            }

            // "Do not trail": the particle lives in its emitter bone's frame and
            // is carried by it, instead of being left behind in the world. The
            // portal's swirl is exactly this - particles drawn inward while the
            // bones turn them, so each one traces a spiral.
            if (noTrail) {
                // Velocity stays in the bone's frame; see M2Particle.
                advanceParticle(localPos, localVel, gravity, drag, age);
                p.localPos = localPos;
                p.velocity = localVel;
                p.position = glm::vec3(xf * glm::vec4(localPos, 1.0f));
            } else {
                // Transform direction by bone + model orientation (rotation only)
                p.velocity = rotMat * localVel;
                advanceParticle(p.position, p.velocity, gravity, drag, age);
            }

            if (randomTile) {
                p.tileIndex = static_cast<uint16_t>(distTile(rng));
            }

            out.push_back(p);

            // Diagnostic: log first particle birth per spell effect instance
            if (inst.spellVisual && outBase + out.size() == 1) {
                LOG_INFO("SpellEffect: first particle for '", gpu.name,
                         "' pos=(", p.position.x, ",", p.position.y, ",", p.position.z,
                         ") rate=", rate, " life=", life,
                         " bone=", em.bone, " boneCount=", inst.boneMatrices.size(),
                         " globalSeqs=", gpu.globalSequenceDurations.size());
            }
        }
        // Cap accumulator to avoid bursts after lag
        if (inst.emitterAccumulators[ei] > 2.0f) {
            inst.emitterAccumulators[ei] = 0.0f;
        }
    }
    if (worker) m2_particle::mergeByEmitter(inst.particles, worker->births);
}

void M2Renderer::updateParticles(M2Instance& inst, float dt, ParticleWorker* worker) {
    if (!inst.cachedModel || inst.particles.empty()) return;
    const auto& gpu = *inst.cachedModel;

    // Everything per emitter hoisted out of the per-particle loop: gravity
    // (a track sample), the drag factor (an exp) and, for "do not trail"
    // emitters, the model * bone matrix. Each was being worked out again for
    // every particle, though hundreds share one emitter. The scratch array is
    // the renderer's (or the worker's), so the step never allocates.
    const size_t numEm = gpu.particleEmitters.size();
    auto& ems = worker ? worker->ems : particleStepScratch_;
    if (ems.size() < numEm) ems.resize(numEm);
    const bool fastTracks = worker && gpu.particleTracks.size() == numEm;
    const glm::mat4 identity(1.0f);
    for (size_t e = 0; e < numEm; ++e) {
        const auto& pem = gpu.particleEmitters[e];
        auto& s = ems[e];
        // Authored gravity as-is: 0 means no gravity (candle flames,
        // chandelier glows), never a substituted fall.
        s.gravity = fastTracks
            ? m2_particle::sampleTrack(gpu.particleTracks[e].gravity, pem.gravity,
                                       inst.currentSequenceIndex, inst.animTime,
                                       inst.globalSequenceTime, gpu.globalSequenceDurations)
            : interpFloat(pem.gravity,
                          inst.animTime, inst.globalSequenceTime,
                          inst.currentSequenceIndex, gpu.globalSequenceDurations);
        s.drag = pem.drag;
        s.dragFactor = (pem.drag > 0.0f) ? std::exp(-pem.drag * dt) : 1.0f;
        s.noTrail = (pem.flags & kParticleFlagNoTrail) != 0;
        if (s.noTrail) {
            const glm::mat4& boneXform = (pem.bone < inst.boneMatrices.size())
                ? inst.boneMatrices[pem.bone] : identity;
            s.xf = inst.modelMatrix * boneXform;
        }
    }

    if (worker) {
        // In order, so the emitters stay grouped for the particle pass.
        const size_t left = m2_particle::stepAndCompact(
            inst.particles.data(), inst.particles.size(), ems.data(), numEm, dt);
        inst.particles.resize(left);
        return;
    }

    M2Particle* const ps = inst.particles.data();
    size_t n = inst.particles.size();
    for (size_t i = 0; i < n; ) {
        M2Particle& p = ps[i];
        p.life += dt;
        if (p.life >= p.maxLife) {
            // Swap-and-pop removal; the vector is shrunk once at the end.
            p = ps[n - 1];
            --n;
            continue;
        }
        const size_t e = p.emitterIndex;
        if (e < numEm) {
            const auto& s = ems[e];
            if (s.noTrail) {
                // Stepped in the emitter bone's frame, then placed through the
                // bone as it is now - see kParticleFlagNoTrail and M2Particle.
                p.velocity.z -= s.gravity * dt;
                p.velocity *= s.dragFactor;
                p.localPos += p.velocity * dt;
                p.position = glm::vec3(s.xf * glm::vec4(p.localPos, 1.0f));
                ++i;
                continue;
            }
            p.velocity.z -= s.gravity * dt;
            p.velocity *= s.dragFactor;
        }
        p.position += p.velocity * dt;
        ++i;
    }
    inst.particles.resize(n);
}

// ---------------------------------------------------------------------------
// Ribbon emitter simulation
// ---------------------------------------------------------------------------
void M2Renderer::updateRibbons(M2Instance& inst, const M2ModelGPU& gpu, float dt) {
    const auto& emitters = gpu.ribbonEmitters;
    if (emitters.empty()) return;

    // Grow per-instance state arrays if needed
    if (inst.ribbonEdges.size() != emitters.size()) {
        inst.ribbonEdges.resize(emitters.size());
    }
    if (inst.ribbonEdgeAccumulators.size() != emitters.size()) {
        inst.ribbonEdgeAccumulators.resize(emitters.size(), 0.0f);
    }

    for (size_t ri = 0; ri < emitters.size(); ri++) {
        const auto& em = emitters[ri];
        auto& edges    = inst.ribbonEdges[ri];
        auto& accum    = inst.ribbonEdgeAccumulators[ri];

        // Determine bone world position for spine
        glm::vec3 spineWorld = inst.position;
        // Use referenced bone; fall back to bone 0 if out of range (common for spell effects
        // where ribbon bone fields may be unset/garbage, e.g. bone=4294967295)
        uint32_t boneIdx = em.bone;
        if (boneIdx >= inst.boneMatrices.size() && !inst.boneMatrices.empty()) {
            boneIdx = 0;
        }
        if (boneIdx < inst.boneMatrices.size()) {
            glm::vec4 local(em.position.x, em.position.y, em.position.z, 1.0f);
            spineWorld = glm::vec3(inst.modelMatrix * inst.boneMatrices[boneIdx] * local);
        } else {
            glm::vec4 local(em.position.x, em.position.y, em.position.z, 1.0f);
            spineWorld = glm::vec3(inst.modelMatrix * local);
        }

        // Skip emitters that produce NaN positions (garbage bone/position data)
        if (std::isnan(spineWorld.x) || std::isnan(spineWorld.y) || std::isnan(spineWorld.z))
            continue;

        // Evaluate animated tracks (use first available sequence key, or fallback value)
        auto getFloatVal = [&](const pipeline::M2AnimationTrack& track, float fallback) -> float {
            for (const auto& seq : track.sequences) {
                if (!seq.floatValues.empty()) return seq.floatValues[0];
            }
            return fallback;
        };
        auto getVec3Val = [&](const pipeline::M2AnimationTrack& track, glm::vec3 fallback) -> glm::vec3 {
            for (const auto& seq : track.sequences) {
                if (!seq.vec3Values.empty()) return seq.vec3Values[0];
            }
            return fallback;
        };

        float visibility  = getFloatVal(em.visibilityTrack, 1.0f);
        float heightAbove = getFloatVal(em.heightAboveTrack, 0.5f);
        float heightBelow = getFloatVal(em.heightBelowTrack, 0.5f);
        glm::vec3 color   = getVec3Val(em.colorTrack, glm::vec3(1.0f));
        float alpha       = getFloatVal(em.alphaTrack, 1.0f);

        // Age existing edges and remove expired ones
        for (auto& e : edges) {
            e.age += dt;
            // Apply gravity
            if (em.gravity != 0.0f) {
                e.worldPos.z -= em.gravity * dt * dt * 0.5f;
            }
        }
        while (!edges.empty() && edges.front().age >= em.edgeLifetime) {
            edges.pop_front();
        }

        // Emit new edges based on edgesPerSecond
        if (visibility > 0.5f && !inst.emissionStopped) {
            accum += em.edgesPerSecond * dt;
            while (accum >= 1.0f) {
                accum -= 1.0f;
                M2Instance::RibbonEdge e;
                e.worldPos    = spineWorld;
                e.color       = color;
                e.alpha       = alpha;
                // Scaled into world units here, where the instance is at hand.
                // The spine is a world position the model matrix produced, and
                // these two were model-space lengths added straight to it - so
                // a doodad placed at any scale but 1 got a trail the right
                // length and the wrong width.
                e.heightAbove = heightAbove * inst.scale;
                e.heightBelow = heightBelow * inst.scale;
                e.age         = 0.0f;
                edges.push_back(e);

                // Diagnostic: log first ribbon edge per spell effect instance+emitter
                if (gpu.isSpellEffect && edges.size() == 1) {
                    LOG_INFO("SpellEffect: ribbon edge[0] for '", gpu.name,
                             "' emitter=", ri, " pos=(", spineWorld.x, ",", spineWorld.y,
                             ",", spineWorld.z, ") hA=", heightAbove, " hB=", heightBelow,
                             " vis=", visibility, " eps=", em.edgesPerSecond,
                             " edgeLife=", em.edgeLifetime, " bone=", em.bone);
                }

                // Cap trail length
                if (edges.size() > 128) edges.pop_front();
            }
        } else {
            accum = 0.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// Ribbon rendering
// ---------------------------------------------------------------------------
void M2Renderer::renderM2Ribbons(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (!ribbonPipeline_ || !ribbonAdditivePipeline_ || !ribbonVB_ || !ribbonVBMapped_) return;
    // Diagnostic: WOWEE_M2_NO_RIBBONS=1 drops every M2 ribbon trail draw.
    static const bool kNoRibbons = envFlagEnabled("WOWEE_M2_NO_RIBBONS");
    if (kNoRibbons) return;

    // A ribbon's width runs across the trail and across the view, and it has to
    // be computed per edge from the direction the trail is actually going.
    //
    // This used a fixed world Z, which is only right for a trail travelling
    // horizontally. A flame licks upward, so its spine ran along the very axis
    // the strip was being widened on: the top and bottom vertices of every
    // edge landed on the spine itself, the quad collapsed, and the whole trail
    // drew as one tall thin sliver with the fire texture smeared up it. That
    // is the bonfire at Grom'gol standing four times the height of the huts
    // behind it.
    const glm::vec3 camPos = cachedCamPos_;
    const glm::vec3 upWorld(0.0f, 0.0f, 1.0f);

    float* dst     = static_cast<float*>(ribbonVBMapped_);
    size_t written = 0;

    ribbonDraws_.clear();
    auto& draws = ribbonDraws_;

    // Only the instances the update steps ribbons for - those on the emitter
    // list; nothing else ever has an edge. This walked all 88k instances in
    // the world, for a few dozen trails: 1.5-2.3 ms of the M2 worker, which is
    // the critical path of renderWorld.
    for (size_t instIdx : particleInstanceIndices_) {
        if (instIdx >= instances.size()) continue;
        const auto& inst = instances[instIdx];
        if (!inst.cachedModel) continue;
        const auto& gpu = *inst.cachedModel;
        if (gpu.ribbonEmitters.empty()) continue;
        if (inst.ribbonEdges.empty()) continue;
        // On screen if the instance is (padded as the particle pass pads it),
        // or if the line from the trail's head to its tail is - a missile's
        // trail can reach well past its carrier's own sphere.
        const bool instOnScreen = inst.cachedPaddedRadius <= 0.0f ||
            cachedFrustum_.intersectsSphere(inst.cachedCullCenter, inst.cachedPaddedRadius + 30.0f);

        for (size_t ri = 0; ri < gpu.ribbonEmitters.size(); ri++) {
            if (ri >= inst.ribbonEdges.size()) continue;
            const auto& edges = inst.ribbonEdges[ri];
            if (edges.size() < 2) continue;
            if (!instOnScreen) {
                const glm::vec3& head = edges.back().worldPos;
                const glm::vec3& tail = edges.front().worldPos;
                const float width = std::max(std::abs(edges.back().heightAbove),
                                             std::abs(edges.back().heightBelow));
                if (!cachedFrustum_.intersectsSphere(0.5f * (head + tail),
                        0.5f * glm::length(head - tail) + width + 10.0f)) continue;
            }

            const auto& em = gpu.ribbonEmitters[ri];

            // Select blend pipeline based on material blend mode
            bool additive = false;
            if (em.materialIndex < gpu.batches.size()) {
                additive = (gpu.batches[em.materialIndex].blendMode >= 3);
            }
            VkPipeline pipe = additive ? ribbonAdditivePipeline_ : ribbonPipeline_;

            // Descriptor set for texture
            VkDescriptorSet texSet = (ri < gpu.ribbonTexSets.size())
                                     ? gpu.ribbonTexSets[ri] : VK_NULL_HANDLE;
            if (!texSet) {
                if (gpu.isSpellEffect) {
                    static bool ribbonTexWarn = false;
                    if (!ribbonTexWarn) {
                        LOG_WARNING("SpellEffect: ribbon[", ri, "] for '", gpu.name,
                                    "' has null texSet - descriptor pool may be exhausted");
                        ribbonTexWarn = true;
                    }
                }
                continue;
            }

            uint32_t firstVert = static_cast<uint32_t>(written);

            // Emit triangle strip: 2 verts per edge (top + bottom)
            for (size_t ei = 0; ei < edges.size(); ei++) {
                if (written + 2 > MAX_RIBBON_VERTS) break;
                const auto& e = edges[ei];
                float t = (em.edgeLifetime > 0.0f)
                          ? 1.0f - (e.age / em.edgeLifetime) : 1.0f;
                float a = e.alpha * t;
                float u = static_cast<float>(ei) / static_cast<float>(edges.size() - 1);

                // The trail's own direction here, from the edges either side.
                const glm::vec3& prev = edges[ei > 0 ? ei - 1 : ei].worldPos;
                const glm::vec3& next = edges[ei + 1 < edges.size() ? ei + 1 : ei].worldPos;
                glm::vec3 spineDir = next - prev;
                float spineLen = glm::length(spineDir);
                // Widen across the trail and across the line of sight. Where
                // the trail doubles back on itself or points straight at the
                // eye there is no such direction, and world up is as good an
                // answer as any.
                glm::vec3 side = upWorld;
                if (spineLen > 1e-5f) {
                    glm::vec3 toEye = camPos - e.worldPos;
                    glm::vec3 cross = glm::cross(spineDir / spineLen, toEye);
                    float crossLen = glm::length(cross);
                    if (crossLen > 1e-5f) side = cross / crossLen;
                }

                // Top vertex (one side of the spine)
                glm::vec3 top = e.worldPos + side * e.heightAbove;
                dst[written * 9 + 0] = top.x;
                dst[written * 9 + 1] = top.y;
                dst[written * 9 + 2] = top.z;
                dst[written * 9 + 3] = e.color.r;
                dst[written * 9 + 4] = e.color.g;
                dst[written * 9 + 5] = e.color.b;
                dst[written * 9 + 6] = a;
                dst[written * 9 + 7] = u;
                dst[written * 9 + 8] = 0.0f; // v = top
                written++;

                // Bottom vertex (the other side)
                glm::vec3 bot = e.worldPos - side * e.heightBelow;
                dst[written * 9 + 0] = bot.x;
                dst[written * 9 + 1] = bot.y;
                dst[written * 9 + 2] = bot.z;
                dst[written * 9 + 3] = e.color.r;
                dst[written * 9 + 4] = e.color.g;
                dst[written * 9 + 5] = e.color.b;
                dst[written * 9 + 6] = a;
                dst[written * 9 + 7] = u;
                dst[written * 9 + 8] = 1.0f; // v = bottom
                written++;
            }

            uint32_t vertCount = static_cast<uint32_t>(written) - firstVert;
            if (vertCount >= 4) {
                draws.push_back({.texSet = texSet, .pipeline = pipe, .firstVertex = firstVert, .vertexCount = vertCount});
            } else {
                // Rollback if too few verts
                written = firstVert;
            }
        }
    }

    // Periodic diagnostic: spell ribbon draw count
    {
        static uint32_t ribbonDiagFrame_ = 0;
        if (++ribbonDiagFrame_ % 300 == 1) {
            size_t spellRibbonDraws = 0;
            size_t spellRibbonVerts = 0;
            for (size_t instIdx : particleInstanceIndices_) {
                if (instIdx >= instances.size()) continue;
                const auto& inst = instances[instIdx];
                if (!inst.cachedModel || !inst.cachedModel->isSpellEffect) continue;
                for (const auto& ribbonEdge : inst.ribbonEdges) {
                    if (ribbonEdge.size() >= 2) {
                        spellRibbonDraws++;
                        spellRibbonVerts += ribbonEdge.size() * 2;
                    }
                }
            }
            if (spellRibbonDraws > 0 || !draws.empty()) {
                LOG_INFO("SpellEffect: ", spellRibbonDraws, " spell ribbon strips (",
                         spellRibbonVerts, " verts), total draws=", draws.size(),
                         " written=", written);
            }
        }
    }

    if (draws.empty() || written == 0) return;

    VkExtent2D ext = vkCtx_->getSwapchainExtent();
    VkViewport vp{};
    vp.x = 0; vp.y = 0;
    vp.width  = static_cast<float>(ext.width);
    vp.height = static_cast<float>(ext.height);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    VkRect2D sc{};
    sc.offset = {.x = 0, .y = 0};
    sc.extent = ext;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    VkPipeline lastPipe = VK_NULL_HANDLE;
    for (const auto& dc : draws) {
        if (dc.pipeline != lastPipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dc.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    ribbonPipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
            lastPipe = dc.pipeline;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                ribbonPipelineLayout_, 1, 1, &dc.texSet, 0, nullptr);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &ribbonVB_, &offset);
        vkCmdDraw(cmd, dc.vertexCount, 1, dc.firstVertex, 0);
    }
}

size_t M2Renderer::writeParticleVertsCpu() {
    // Collect all particles from all instances, grouped by texture+blend.
    auto& groups = particleGroups_;

    // Written straight into the mapped buffer rather than accumulated into a
    // vector per group and copied in afterwards; see ParticleRun.
    float* const vbBase = static_cast<float*>(m2ParticleVBMapped_);
    uint32_t vbWritten = 0;
    ParticleGroup* runGroup = nullptr;

    size_t totalParticles = 0;

    for (size_t idx : particleDrawOrder_) {
        auto& inst = instances[idx];
        const auto& gpu = *inst.cachedModel;


        // Cache the last emitter's per-emitter state so adjacent particles
        // sharing an emitter (the common case - particles from one source
        // cluster together) skip the texture/key/map-lookup work entirely.
        int lastEmitterIdx = -1;
        VkTexture* cachedTex = nullptr;
        uint16_t cachedTilesX = 1, cachedTilesY = 1;
        uint32_t cachedTotalTiles = 1;
        uint16_t cachedBlendType = 0;
        const pipeline::M2ParticleEmitter* cachedEm = nullptr;
        ParticleGroup* cachedGroup = nullptr;
        // animFrame depends only on inst.animTime + totalTiles, so it's also
        // emitter-stable within one frame.
        uint32_t cachedAnimFrame = 0;
        float cachedTilesFloat = 1.0f;
        bool cachedIsTiled = false;
        float invAnimMs = 1.0f / 1000.0f;
        // The emitter's lifetime curves as tables (see M2ModelGPU::ParticleCurve);
        // null only if the update never emitted for this model, when the
        // FBlocks are walked as before.
        using Curve = M2ModelGPU::ParticleCurve;
        const bool haveCurves = gpu.particleCurves.size() == gpu.particleEmitters.size();
        const Curve* cachedCurve = nullptr;
        // The model's class, the same for every particle it has.
        const bool isFlameFixture = gpu.isLanternLike || gpu.isTorch ||
                                    gpu.isBrazierOrFire || gpu.isKoboldFlame;
        const bool isPlainDoodad = !gpu.isSpellEffect && !gpu.isFireflyEffect && !isFlameFixture;

        // How far this instance's particles actually reach, against how far
        // the model says it extends. A fire twice the height of the hut behind
        // it is either particles outliving their authored lifespan, flying at
        // the wrong speed, or drawn at the wrong size - and none of those says
        // which model it is. This does.
        float highestParticleZ = -std::numeric_limits<float>::max();
        float widestParticle = 0.0f;

        for (const auto& p : inst.particles) {
            if (p.emitterIndex >= gpu.particleEmitters.size()) continue;

            if (p.emitterIndex != lastEmitterIdx) {
                lastEmitterIdx = p.emitterIndex;
                cachedEm = &gpu.particleEmitters[p.emitterIndex];
                cachedCurve = haveCurves ? &gpu.particleCurves[p.emitterIndex] : nullptr;

                cachedTex = whiteTexture_.get();
                if (p.emitterIndex < static_cast<int>(gpu.particleTextures.size())) {
                    cachedTex = gpu.particleTextures[p.emitterIndex];
                }
                cachedTilesX = std::max<uint16_t>(cachedEm->textureCols, 1);
                cachedTilesY = std::max<uint16_t>(cachedEm->textureRows, 1);
                cachedTotalTiles = static_cast<uint32_t>(cachedTilesX) *
                                   static_cast<uint32_t>(cachedTilesY);
                cachedBlendType = cachedEm->blendingType;
                ParticleGroupKey key{.texture = cachedTex, .blendType = static_cast<uint8_t>(cachedBlendType), .tilesX = cachedTilesX, .tilesY = cachedTilesY};
                cachedGroup = &groups[key];
                cachedGroup->texture = cachedTex;
                cachedGroup->blendType = cachedBlendType;
                cachedGroup->tilesX = cachedTilesX;
                cachedGroup->tilesY = cachedTilesY;
                if (cachedGroup->preAllocSet == VK_NULL_HANDLE &&
                    p.emitterIndex < static_cast<int>(gpu.particleTexSets.size())) {
                    cachedGroup->preAllocSet = gpu.particleTexSets[p.emitterIndex];
                }

                cachedIsTiled = (cachedEm->flags & kParticleFlagTiled) && cachedTotalTiles > 1;
                if (cachedIsTiled) {
                    float animSeconds = inst.animTime * invAnimMs;
                    cachedAnimFrame = static_cast<uint32_t>(std::floor(animSeconds * cachedTotalTiles))
                                      % cachedTotalTiles;
                    cachedTilesFloat = static_cast<float>(cachedTotalTiles);
                }
            }

            const auto& em = *cachedEm;
            float lifeRatio = p.life / std::max(p.maxLife, 0.001f);
            glm::vec3 color;
            float alpha, rawScale;
            if (cachedCurve) {
                const float f = glm::clamp(lifeRatio, 0.0f, 1.0f) * static_cast<float>(Curve::kSteps);
                const int k = std::min(static_cast<int>(f), Curve::kSteps - 1);
                const float t = f - static_cast<float>(k);
                const glm::vec4 ca = glm::mix(cachedCurve->colorAlpha[k], cachedCurve->colorAlpha[k + 1], t);
                color = glm::vec3(ca);
                alpha = ca.a;
                rawScale = glm::mix(cachedCurve->scale[k], cachedCurve->scale[k + 1], t);
            } else {
                color = interpFBlockVec3(em.particleColor, lifeRatio);
                alpha = std::min(interpFBlockFloat(em.particleAlpha, lifeRatio), 1.0f);
                rawScale = interpFBlockFloat(em.particleScale, lifeRatio);
            }

            if (isPlainDoodad) {
                color = glm::mix(color, glm::vec3(1.0f), 0.7f);
                if (rawScale > 2.0f) alpha *= 0.02f;
                if (cachedBlendType == 3 || cachedBlendType == 4) alpha *= 0.05f;
            }
            // Flame fixtures: the authored curves can leave a particle with
            // effectively no colour or alpha for most of its life. CHANDELIER01
            // ramps scale from zero over a six second life and its candles spend
            // nearly all of that time contributing nothing - and because these
            // draw additively, a near-black particle adds literally nothing to
            // the frame. Floor colour and alpha so a lit fixture always shows
            // flame. Floors only lift the dim end, leaving torches and candles
            // that already read correctly untouched.
            if (isFlameFixture) {
                color = glm::max(color, glm::vec3(0.50f, 0.26f, 0.09f));
                alpha = std::max(alpha, 0.30f);
            }

            float scale = rawScale;
            if (gpu.isSpellEffect) {
                // The authored size, as every other emitter draws it. This
                // was raised by half on top, which nothing in the data asks
                // for: kits on the caster's hands and missiles came out a
                // size larger than the stock client's. Only the sub-pixel
                // floor the flame fixtures also get stays.
                scale = std::max(rawScale, 0.15f);
            } else if (!gpu.isFireflyEffect) {
                scale = std::min(rawScale, 1.5f);
                // Candle flames are authored at a fraction of a unit, which lands
                // sub-pixel at any normal viewing distance - the fixture glows
                // with no visible flame. Small effect-heavy models dodge this by
                // being classified as spell effects (three or more emitters and
                // few vertices) and picking up that path's floor, but a
                // chandelier is a real fixture at 370 vertices and misses the
                // cut, so its five candles rendered as nothing. Give flame
                // fixtures the same floor without making them spell effects,
                // which would also change how they blend.
                if (isFlameFixture) {
                    scale = std::max(scale, 0.15f);
                }
            }

            if (vbWritten >= MAX_M2_PARTICLE_VERTS) break;
            // A run per stretch of particles sharing a group. The group only
            // changes when the emitter does, and particles from one emitter
            // are adjacent, so this closes a run about once per emitter.
            if (cachedGroup != runGroup) {
                runGroup = cachedGroup;
                particleRuns_.push_back({.group = cachedGroup, .first = vbWritten, .count = 0});
            }
            highestParticleZ = std::max(highestParticleZ, p.position.z);
            widestParticle = std::max(widestParticle, scale);

            float* vd = vbBase + static_cast<size_t>(vbWritten) * 9;
            vd[0] = p.position.x;
            vd[1] = p.position.y;
            vd[2] = p.position.z;
            vd[3] = color.r;
            vd[4] = color.g;
            vd[5] = color.b;
            vd[6] = alpha;
            vd[7] = scale;
            float tileIndex = p.tileIndex;
            if (cachedIsTiled) {
                tileIndex = p.tileIndex + static_cast<float>(cachedAnimFrame);
                while (tileIndex >= cachedTilesFloat) {
                    tileIndex -= cachedTilesFloat;
                }
            }
            vd[8] = tileIndex;
            ++vbWritten;
            ++particleRuns_.back().count;
            totalParticles++;
        }

        // Said once per model, on an absolute reach rather than a ratio
        // against the model's own bounds. A bonfire's authored box is 25 to 32
        // yards tall - it has to hold the flame's full extent - so a ratio
        // test can never trip on the one model anybody is complaining about.
        // Nothing that stands on the ground should be throwing particles eight
        // yards into the air.
        if (highestParticleZ > -std::numeric_limits<float>::max()) {
            const float reach = highestParticleZ - inst.position.z;
            if (reach > 8.0f) {
                static std::set<std::string> saidTall;
                if (saidTall.insert(gpu.name).second) {
                    LOG_WARNING("Particles reach ", reach, " yd above '", gpu.name,
                                "' (model bound ", gpu.boundMax.z, " yd, scale ", inst.scale,
                                ", ", gpu.particleEmitters.size(), " emitters, ",
                                inst.particles.size(), " live, largest ", widestParticle, " yd)");
                }
            }
        }
    }
    return totalParticles;
}

size_t M2Renderer::writeParticleRecordsGpu(uint32_t frame) {
    // The same walk as writeParticleVertsCpu - same emitters, same order,
    // same runs and groups - writing 24 bytes per particle instead of 36,
    // with nothing worked out per particle but its life ratio and tile.
    // Colour, alpha and size are m2_particle_gpu.vert's, from the emitter's
    // curve table uploaded here once per frame per distinct curve (instances
    // of one model share theirs) and the class flags in the record.
    using Curve = M2ModelGPU::ParticleCurve;
    using m2_particle::ParticleRecord;
    static_assert(Curve::kSteps == m2_particle::kCurveSteps,
                  "the shader's curve layout assumes ParticleCurve's resolution");
    auto& groups = particleGroups_;
    auto* const recBase = static_cast<ParticleRecord*>(particleRecordMapped_[frame]);
    auto* const curveData = static_cast<glm::vec4*>(particleCurveMapped_[frame]);
    const uint32_t budget = particleRecordBudget_;
    uint32_t written = 0;
    uint32_t curvesUsed = 0;
    particleCurveSlots_.clear();
    ParticleGroup* runGroup = nullptr;
    constexpr float invAnimMs = 1.0f / 1000.0f;

    for (size_t idx : particleDrawOrder_) {
        if (written >= budget) break;
        const auto& inst = instances[idx];
        const auto& gpu = *inst.cachedModel;
        // Made by the model's first emit, so a model with live particles has
        // them; the CPU path's FBlock fallback has nothing to do here.
        if (gpu.particleCurves.size() != gpu.particleEmitters.size()) continue;
        const bool isFlameFixture = gpu.isLanternLike || gpu.isTorch ||
                                    gpu.isBrazierOrFire || gpu.isKoboldFlame;

        int lastEmitterIdx = -1;
        bool emitterDrawn = false;
        ParticleGroup* cachedGroup = nullptr;
        uint32_t cachedCurveBase = 0;
        uint32_t cachedFlags = 0;
        uint32_t cachedTotalTiles = 1;
        uint32_t cachedAnimFrame = 0;
        bool cachedIsTiled = false;
        float highestParticleZ = -std::numeric_limits<float>::max();

        for (const auto& p : inst.particles) {
            if (p.emitterIndex >= gpu.particleEmitters.size()) continue;

            if (p.emitterIndex != lastEmitterIdx) {
                lastEmitterIdx = p.emitterIndex;
                const auto& em = gpu.particleEmitters[p.emitterIndex];

                VkTexture* tex = whiteTexture_.get();
                if (p.emitterIndex < static_cast<int>(gpu.particleTextures.size())) {
                    tex = gpu.particleTextures[p.emitterIndex];
                }
                const uint16_t tilesX = std::max<uint16_t>(em.textureCols, 1);
                const uint16_t tilesY = std::max<uint16_t>(em.textureRows, 1);
                cachedTotalTiles = static_cast<uint32_t>(tilesX) * static_cast<uint32_t>(tilesY);
                const uint16_t blendType = em.blendingType;
                ParticleGroupKey key{.texture = tex, .blendType = static_cast<uint8_t>(blendType), .tilesX = tilesX, .tilesY = tilesY};
                cachedGroup = &groups[key];
                cachedGroup->texture = tex;
                cachedGroup->blendType = blendType;
                cachedGroup->tilesX = tilesX;
                cachedGroup->tilesY = tilesY;
                if (cachedGroup->preAllocSet == VK_NULL_HANDLE &&
                    p.emitterIndex < static_cast<int>(gpu.particleTexSets.size())) {
                    cachedGroup->preAllocSet = gpu.particleTexSets[p.emitterIndex];
                }

                cachedIsTiled = (em.flags & kParticleFlagTiled) && cachedTotalTiles > 1;
                if (cachedIsTiled) {
                    float animSeconds = inst.animTime * invAnimMs;
                    cachedAnimFrame = static_cast<uint32_t>(std::floor(animSeconds * cachedTotalTiles))
                                      % cachedTotalTiles;
                }
                cachedFlags = m2_particle::lookFlagsFor(gpu.isSpellEffect, gpu.isFireflyEffect,
                                                        isFlameFixture, blendType);

                // This emitter's curve, uploaded the first time this frame
                // anything uses it.
                const Curve& curve = gpu.particleCurves[p.emitterIndex];
                auto [slot, fresh] = particleCurveSlots_.try_emplace(&curve, 0u);
                if (fresh) {
                    if (curvesUsed >= kMaxParticleCurvesPerFrame) {
                        particleCurveSlots_.erase(slot);
                        emitterDrawn = false;
                        static bool saidFull = false;
                        if (!saidFull) {
                            saidFull = true;
                            LOG_WARNING("Particle curve buffer full (", kMaxParticleCurvesPerFrame,
                                        " curves): emitters past it are not drawn");
                        }
                        continue;
                    }
                    slot->second = curvesUsed * m2_particle::kCurveVec4s;
                    m2_particle::packCurve(curve.colorAlpha, curve.scale, curveData + slot->second);
                    ++curvesUsed;
                }
                cachedCurveBase = slot->second;
                emitterDrawn = true;
            }
            if (!emitterDrawn) continue;

            if (written >= budget) break;
            if (cachedGroup != runGroup) {
                runGroup = cachedGroup;
                particleRuns_.push_back({.group = cachedGroup, .first = written, .count = 0});
            }
            highestParticleZ = std::max(highestParticleZ, p.position.z);

            ParticleRecord& r = recBase[written];
            r.position = p.position;
            r.lifeRatio = p.life / std::max(p.maxLife, 0.001f);
            r.curveBase = cachedCurveBase;
            r.tileFlags = m2_particle::packTileFlags(
                m2_particle::particleTile(p.tileIndex, cachedIsTiled, cachedAnimFrame, cachedTotalTiles),
                cachedFlags);
            ++written;
            ++particleRuns_.back().count;
        }

        // The same once-per-model report as the CPU path's. The size is not
        // worked out here any more, so the largest is found only when the
        // report is actually made.
        if (highestParticleZ > -std::numeric_limits<float>::max()) {
            const float reach = highestParticleZ - inst.position.z;
            if (reach > 8.0f) {
                static std::set<std::string> saidTall;
                if (saidTall.insert(gpu.name).second) {
                    float widestParticle = 0.0f;
                    for (const auto& p : inst.particles) {
                        if (p.emitterIndex >= gpu.particleEmitters.size()) continue;
                        const auto& c = gpu.particleCurves[p.emitterIndex];
                        const uint32_t flags = m2_particle::lookFlagsFor(
                            gpu.isSpellEffect, gpu.isFireflyEffect, isFlameFixture,
                            gpu.particleEmitters[p.emitterIndex].blendingType);
                        widestParticle = std::max(widestParticle, m2_particle::lookFromCurve(
                            c.colorAlpha, c.scale, p.life / std::max(p.maxLife, 0.001f), flags).scale);
                    }
                    LOG_WARNING("Particles reach ", reach, " yd above '", gpu.name,
                                "' (model bound ", gpu.boundMax.z, " yd, scale ", inst.scale,
                                ", ", gpu.particleEmitters.size(), " emitters, ",
                                inst.particles.size(), " live, largest ", widestParticle, " yd)");
                }
            }
        }
    }
    return written;
}

void M2Renderer::renderM2Particles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (!particlePipeline_ || !m2ParticleVB_) return;
    // Diagnostic: WOWEE_M2_NO_PARTICLES=1 drops every M2 particle draw, which
    // tells a particle artifact apart from a skinned-geometry one.
    static const bool kNoParticles = envFlagEnabled("WOWEE_M2_NO_PARTICLES");
    if (kNoParticles) return;

    // Two ways to fill the frame's particles. By default one compact record
    // per particle, colour and size worked out in m2_particle_gpu.vert;
    // WOWEE_PARTICLE_CPU_QUADS=1 is the path that worked them out here and
    // wrote nine floats each, kept for A/B. Both draw the same point sprites
    // through the same fragment shader. The GPU path also falls back to the
    // CPU one if its shader or buffers are missing.
    static const bool kCpuQuads = envFlagEnabled("WOWEE_PARTICLE_CPU_QUADS");
    const uint32_t frame = vkCtx_->getCurrentFrame() % 2;
    const bool gpuPath = !kCpuQuads && particleGpuPipeline_ && particleGpuAdditivePipeline_ &&
                         particleRecordMapped_[frame] && particleCurveMapped_[frame] &&
                         particleCurveSet_[frame];
    static bool saidPath = false;
    if (!saidPath) {
        saidPath = true;
        LOG_INFO("M2 particles: ", gpuPath ? "compact records, look in the vertex shader (budget "
                                           : "CPU-built vertices (budget ",
                 gpuPath ? particleRecordBudget_ : static_cast<uint32_t>(MAX_M2_PARTICLE_VERTS), ")",
                 (!gpuPath && !kCpuQuads) ? " - GPU path unavailable, m2_particle_gpu.vert.spv missing?" : "");
    }
    if (!gpuPath && !m2ParticleVBMapped_) return;

    // Reuse persistent map - keep the bucket structure, drop last frame's set.
    for (auto& [k, g] : particleGroups_) {
        g.preAllocSet = VK_NULL_HANDLE;
    }
    particleRuns_.clear();

    // Only the instances that carry emitters, not every instance in the world.
    //
    // particleInstanceIndices_ is built and maintained for exactly this and
    // the update path above already uses it; this pass walked all 66211
    // instances instead, asking each whether its particle vector was empty.
    // That walk was 2.0ms of a 16ms frame - two thirds of the M2 worker,
    // which is the critical path of renderWorld. The smoke pass beside this
    // one does the same kind of work against a flat list and costs 0.003ms.
    // Two passes over the fixed vertex budget: emitters near the camera -
    // spell kits, missiles, a campfire - claim it first. The budget was full
    // of distant portals and lava every frame, and spell effects, created
    // last and so at the end of the list, got no vertices at all.
    //
    // One walk sorts the drawable emitters into near and far, so each
    // instance's cache lines are pulled in once rather than once per pass.
    constexpr float kNearFirstSq = 80.0f * 80.0f;
    particleDrawOrder_.clear();
    size_t nearCount = 0;
    for (size_t idx : particleInstanceIndices_) {
        if (idx >= instances.size()) continue;
        const auto& inst = instances[idx];
        if (inst.particles.empty() || !inst.cachedModel) continue;
        // Out of range: the update pass stopped stepping these, so what is
        // left in the vector is stale and would be drawn frozen.
        const glm::vec3 toCam = inst.position - cachedCamPos_;
        const float d2 = glm::dot(toCam, toCam);
        const bool fullRange = inst.spellVisual || inst.emissionStopped;
        if (d2 > (fullRange ? cachedMaxRenderDistSq_ : particleRangeSq(inst, *inst.cachedModel))) continue;
        // Off screen: no vertices for what cannot be seen.
        if (inst.cachedPaddedRadius > 0.0f &&
            !cachedFrustum_.intersectsSphere(inst.cachedCullCenter, inst.cachedPaddedRadius + 30.0f)) continue;
        particleDrawOrder_.push_back(idx);
        if (d2 <= kNearFirstSq) std::swap(particleDrawOrder_[nearCount++], particleDrawOrder_.back());
    }
    const size_t totalParticles = gpuPath ? writeParticleRecordsGpu(frame) : writeParticleVertsCpu();

    // Periodic diagnostic: spell effect particle count
    {
        static uint32_t spellParticleDiagFrame_ = 0;
        if (++spellParticleDiagFrame_ % 300 == 1) {
            size_t spellPtc = 0;
            for (size_t instIdx : particleInstanceIndices_) {
                if (instIdx >= instances.size()) continue;
                const auto& inst = instances[instIdx];
                if (inst.cachedModel && inst.cachedModel->isSpellEffect)
                    spellPtc += inst.particles.size();
            }
            if (spellPtc > 0) {
                LOG_INFO("SpellEffect: rendering ", spellPtc, " spell particles (",
                         totalParticles, " total)");
            }
        }
    }

    particleVertsDrawn_.store(static_cast<uint32_t>(totalParticles), std::memory_order_relaxed);
    particleRunsDrawn_.store(static_cast<uint32_t>(particleRuns_.size()), std::memory_order_relaxed);
    if (totalParticles == 0) return;

    // Bind per-frame set (set 0) for particle pipeline, and on the GPU path
    // the frame's curves (set 2), which stay bound across the runs.
    const VkPipelineLayout layout = gpuPath ? particleGpuPipelineLayout_ : particlePipelineLayout_;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            layout, 0, 1, &perFrameSet, 0, nullptr);
    if (gpuPath) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                layout, 2, 1, &particleCurveSet_[frame], 0, nullptr);
    }

    VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, gpuPath ? &particleRecordBuf_[frame] : &m2ParticleVB_, &vbOffset);
    VkPipeline const alphaPipe = gpuPath ? particleGpuPipeline_ : particlePipeline_;
    VkPipeline const additivePipe = gpuPath ? particleGpuAdditivePipeline_ : particleAdditivePipeline_;

    VkPipeline currentPipeline = VK_NULL_HANDLE;

    for (auto& run : particleRuns_) {
        if (run.count == 0 || !run.group) continue;
        ParticleGroup& group = *run.group;

        uint8_t blendType = group.blendType;
        VkPipeline desiredPipeline = (blendType == 3 || blendType == 4)
            ? additivePipe : alphaPipe;
        if (desiredPipeline != currentPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
            currentPipeline = desiredPipeline;
        }

        // Use pre-allocated stable descriptor set; fall back to per-frame alloc only if unavailable
        VkDescriptorSet texSet = group.preAllocSet;
        if (texSet == VK_NULL_HANDLE) {
            // Fallback: allocate per-frame (pool exhaustion risk - should not happen in practice)
            VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = materialDescPool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &particleTexLayout_;
            if (vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &texSet) == VK_SUCCESS) {
                VkTexture* tex = (group.texture && group.texture->isValid())
                    ? group.texture : whiteTexture_.get();
                if (!tex || !tex->isValid()) continue;
                VkDescriptorImageInfo imgInfo = tex->descriptorInfo();
                VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = texSet;
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &imgInfo;
                vkUpdateDescriptorSets(vkCtx_->getDevice(), 1, &write, 0, nullptr);
            }
        }
        if (texSet != VK_NULL_HANDLE) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    layout, 1, 1, &texSet, 0, nullptr);
        }

        // Push constants: tileCount + alphaKey
        struct { float tileX, tileY; int alphaKey; } pc = {
            .tileX = static_cast<float>(group.tilesX), .tileY = static_cast<float>(group.tilesY),
            .alphaKey = (blendType == 1) ? 1 : 0
        };
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(pc), &pc);

        // The vertices are already in the buffer, at this run's own offset.
        // Both used to be wrong: every group copied to offset zero and drew
        // from vertex zero, so with more than one group up they all drew
        // whatever had been copied last.
        vkCmdDraw(cmd, run.count, 1, run.first, 0);
    }
}

void M2Renderer::renderSmokeParticles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (smokeParticles.empty() || !smokePipeline_ || !smokeVB_) return;

    // Build vertex data: pos(3) + lifeRatio(1) + size(1) + isSpark(1) per particle
    size_t count = std::min(smokeParticles.size(), static_cast<size_t>(MAX_SMOKE_PARTICLES));
    float* dst = static_cast<float*>(smokeVBMapped_);
    for (size_t i = 0; i < count; i++) {
        const auto& p = smokeParticles[i];
        *dst++ = p.position.x;
        *dst++ = p.position.y;
        *dst++ = p.position.z;
        *dst++ = p.life / p.maxLife;
        *dst++ = p.size;
        *dst++ = p.isSpark;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, smokePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            smokePipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);

    // Push constant: screenHeight
    float screenHeight = static_cast<float>(vkCtx_->getSwapchainExtent().height);
    vkCmdPushConstants(cmd, smokePipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(float), &screenHeight);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &smokeVB_, &offset);
    vkCmdDraw(cmd, static_cast<uint32_t>(count), 1, 0, 0);
}

} // namespace rendering
} // namespace wowee
