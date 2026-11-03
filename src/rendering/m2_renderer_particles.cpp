#include "rendering/m2_renderer.hpp"
#include "rendering/m2_renderer_internal.h"
#include "rendering/m2_particle_look.hpp"
#include "rendering/m2_particle_stock.hpp"
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
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <random>
#include <cstring>
#include <limits>

// M2 particle and ribbon emitters, run and drawn the way the stock 3.3.5a
// client's CParticleEmitter2 and CRibbonEmitter do. docs/re/particles.md has
// the reverse engineering and the addresses; m2_particle_stock.hpp holds the
// pure arithmetic both this file and the tests use.

namespace wowee {
namespace rendering {

namespace {

namespace mp = m2_particle;

/// The lower key of a uint8 track: the stock never interpolates those
/// (0x82B270), so a visibility or enabled flag switches at its key.
float sampleStep(const pipeline::M2AnimationTrack& track, int seq, float animTime,
                 float globalTime, const std::vector<uint32_t>& gsd, float def) {
    const auto t = m2_track::resolveTime(track, seq, animTime, globalTime, gsd);
    const auto* keys = m2_track::keysFor(track, t);
    if (!keys) return def;
    const size_t count = std::min(keys->timestamps.size(), keys->floatValues.size());
    if (count == 0) return def;
    return keys->floatValues[m2_track::lowerKeyIndex(keys->timestamps, count, t.timeMs)];
}

float uniform01(std::mt19937& rng) {
    return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
}
float uniform11(std::mt19937& rng) {
    return std::uniform_real_distribution<float>(-1.0f, 1.0f)(rng);
}

glm::vec3 randomUnit(std::mt19937& rng) {
    const float z = uniform11(rng);
    const float a = uniform01(rng) * 6.2831853f;
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return {r * std::cos(a), r * std::sin(a), z};
}

/// A point on the emitter's spline (type 3), uniform along its length. The
/// stock walks its own spline object (0x981500); the points are the same.
glm::vec3 splinePoint(const std::vector<glm::vec3>& pts, float u) {
    if (pts.empty()) return glm::vec3(0.0f);
    if (pts.size() == 1) return pts[0];
    float total = 0.0f;
    for (size_t i = 1; i < pts.size(); ++i) total += glm::length(pts[i] - pts[i - 1]);
    float want = u * total;
    for (size_t i = 1; i < pts.size(); ++i) {
        const float seg = glm::length(pts[i] - pts[i - 1]);
        if (want <= seg || i + 1 == pts.size()) {
            const float t = seg > 0.0f ? std::clamp(want / seg, 0.0f, 1.0f) : 0.0f;
            return glm::mix(pts[i - 1], pts[i], t);
        }
        want -= seg;
    }
    return pts.back();
}

} // namespace

float M2Renderer::interpFloat(const pipeline::M2AnimationTrack& track, float animTime,
                              float globalTime, int seqIdx,
                              const std::vector<uint32_t>& globalSeqDurations) {
    return m2_track::sampleFloat(track, seqIdx, animTime, globalTime,
                                 globalSeqDurations, 0.0f);
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
            t.zSource = m2_particle::classifyTrack(em.deceleration, gsd);
        }
        gpu.particleTracks = std::move(tracks);
    }
    const size_t want = gpu.particleEmitters.size() * mp::kCurveVec4s;
    if (gpu.particleCurves.size() == want) return;
    std::vector<glm::vec4> curves(want);
    for (size_t e = 0; e < gpu.particleEmitters.size(); ++e) {
        mp::packCurve(gpu.particleEmitters[e], curves.data() + e * mp::kCurveVec4s);
    }
    gpu.particleCurves = std::move(curves);
}

// ---------------------------------------------------------------------------
// Particle emitters (CParticleEmitter2)
// ---------------------------------------------------------------------------
void M2Renderer::stepParticles(M2Instance& inst, const M2ModelGPU& gpu, float dt,
                               ParticleWorker& worker) {
    const size_t numEm = gpu.particleEmitters.size();
    if (numEm == 0 || !(dt > 0.0f)) return;
    ensureParticleCurves(gpu);
    if (inst.emitterStates.size() != numEm) inst.emitterStates.assign(numEm, {});
    std::mt19937& rng = worker.rng;
    const auto& gsd = gpu.globalSequenceDurations;
    const glm::mat4 rz90 = mp::emitterRotation();
    const bool fastTracks = gpu.particleTracks.size() == numEm;

    // Per frame, as CM2Model feeds its emitters (0x82D2F0, 0x8309C0) and
    // CParticleEmitter2::Update starts (0x97EB10): the tracks, the emitter's
    // frame this frame, its motion since the last, and what that motion
    // gives the flags that use it.
    float longestLife = 0.0f;
    for (size_t e = 0; e < numEm; ++e) {
        const auto& em = gpu.particleEmitters[e];
        auto& st = inst.emitterStates[e];
        const auto track = [&](const m2_particle::TrackConst M2ModelGPU::ParticleTracks::* c,
                               const pipeline::M2AnimationTrack& t) {
            return fastTracks
                ? m2_particle::sampleTrack(gpu.particleTracks[e].*c, t, inst.currentSequenceIndex,
                                           inst.animTime, inst.globalSequenceTime, gsd)
                : interpFloat(t, inst.animTime, inst.globalSequenceTime,
                              inst.currentSequenceIndex, gsd);
        };
        using PT = M2ModelGPU::ParticleTracks;
        // The enabled track is a uint8; with no keys the emitter is on.
        st.enabled = em.enabled &&
            sampleStep(em.enabledTrack, inst.currentSequenceIndex, inst.animTime,
                       inst.globalSequenceTime, gsd, 1.0f) != 0.0f;
        if (st.enabled) {
            st.rate = std::max(track(&PT::rate, em.emissionRate), 0.0f);  // SetRate 0x97BD80
            st.life = track(&PT::life, em.lifespan);
            st.speed = track(&PT::speed, em.emissionSpeed);
            st.speedVar = track(&PT::speedVar, em.speedVariation);
            st.vRange = track(&PT::vRange, em.verticalRange);
            st.hRange = track(&PT::hRange, em.horizontalRange);
            st.gravity = track(&PT::gravity, em.gravity);
            st.areaL = track(&PT::areaL, em.emissionAreaLength);
            st.areaW = track(&PT::areaW, em.emissionAreaWidth);
            st.zSource = track(&PT::zSource, em.deceleration);
            if (std::abs(st.zSource) < 0.001f) st.zSource = 0.0f;  // 0x978DA0
        } else {
            st.rate = 0.0f;
        }
        longestLife = std::max(longestLife, std::max(st.life, 0.001f));

        const glm::mat4& bone = (em.bone < inst.boneMatrices.size())
            ? inst.boneMatrices[em.bone] : glm::mat4(1.0f);
        st.xf = inst.modelMatrix * bone * glm::translate(glm::mat4(1.0f), em.position) * rz90;
        const glm::vec3 pos(st.xf[3]);
        st.prevPos = st.havePos ? st.pos : pos;
        st.pos = pos;
        st.havePos = true;
        const glm::vec3 moved = st.pos - st.prevPos;

        // Flag 0x4000: live particles are dragged along by the emitter's
        // motion, by a factor of its speed (followSpeed/Scale 1 and 2 make a
        // line through two points, 0x978DD0), clamped to [0, 1].
        st.followMove = glm::vec3(0.0f);
        if (em.flags & mp::kFlagFollowEmitter) {
            const float ds = em.followSpeed2 - em.followSpeed1;
            float slope = 0.0f, intercept = 0.0f;
            if (std::abs(ds) >= 2.384185791015625e-07f) {
                slope = (em.followScale2 - em.followScale1) / ds;
                intercept = em.followScale1 - slope * em.followSpeed1;
            }
            const float speed = glm::length(moved) / dt;
            st.followMove = moved * std::clamp(slope * speed + intercept, 0.0f, 1.0f);
        }
        // Flag 0x40: what births inherit, refreshed every thirtieth of a
        // second from the last frame's motion. The stock scales it by
        // burst / (30 x the time since the refresh), not by the time the
        // motion took; that is kept.
        if (em.flags & mp::kFlagInheritVelocity) {
            st.inheritTimer += dt;
            if (st.inheritTimer > 0.033333335f) {
                const float f = st.inheritTimer * 29.999998f;
                st.inheritTimer = 0.0f;
                st.inheritVel = st.live == 0 ? glm::vec3(0.0f) : moved * (em.burstMultiplier / f);
            }
        }
        // Flag 0x8000: an emitter that bursts when it turns on, and never
        // emits otherwise (0x8309C0 sets runtime 0x40 on the rising edge).
        if (em.flags & mp::kFlagBurstOnEnable) {
            const bool on = st.enabled && st.rate > 0.0f && !inst.emissionStopped;
            if (on && !st.wasEmitting) st.acc = -1.0f;  // marks the burst for the first sub-step
            st.wasEmitting = on;
        }
        // The pool keeps room for the peak rate's worth over the longest
        // life (0x97EDF0, run on every emitting step); it never shrinks.
        if (st.enabled) {
            st.poolCap = std::max(st.poolCap,
                                  mp::poolSize(st.rate, em.emissionRateVary, st.life, em.lifespanVary));
        }
    }

    // A long step is cut into tenths of a second, no more of them than a
    // lifespan holds (0x97ACB0). The warm-up of an emitter that lay dormant
    // asks for as long as its life; a Wowee-only case (the stock never lets
    // a visible emitter sleep), so its tenths are capped at 40 steps, each
    // proportionally longer, to keep a wake from costing hundreds of steps.
    mp::Substeps sub = mp::substeps(dt, longestLife);
    float wholeStep = 0.1f;
    constexpr int kMaxWarmSteps = 40;
    if (sub.whole > kMaxWarmSteps) {
        wholeStep = 0.1f * static_cast<float>(sub.whole) / static_cast<float>(kMaxWarmSteps);
        sub.whole = kMaxWarmSteps;
    }
    const float followShare = 1.0f / static_cast<float>(sub.whole + 1);

    auto& ems = worker.ems;
    if (ems.size() < numEm) ems.resize(numEm);
    const auto runSubstep = [&](float s) {
        // Births first (0x97D8C0, through each emitter type's create), then
        // every particle, the new ones too, ages and moves by s (0x97DD20).
        worker.births.clear();
        for (size_t e = 0; e < numEm; ++e) {
            const auto& em = gpu.particleEmitters[e];
            auto& st = inst.emitterStates[e];
            const bool modelSpace = (em.flags & mp::kFlagModelSpace) != 0;

            float lod = 1.0f;
            if (!inst.spellVisual) lod = mp::distanceRateFactor(glm::length(cachedCamPos_ - st.pos));
            const float rate = particleDensity_ * (st.rate + em.emissionRateVary * uniform11(rng)) * lod;

            int count = 0;
            float birthDt = s;
            if (em.flags & mp::kFlagBurstOnEnable) {
                if (st.acc < 0.0f) {
                    st.acc = 0.0f;
                    count = std::max(0, static_cast<int>(rate));  // truncated (FPU in chop mode)
                    birthDt = 0.0f;
                }
            } else if (st.enabled && !inst.emissionStopped) {
                st.acc += rate * s;
                count = std::max(0, static_cast<int>(st.acc + 0.5f));
            }
            if (count == 0) continue;
            const uint32_t room = st.poolCap > st.live ? st.poolCap - st.live : 0u;
            const size_t instRoom = MAX_M2_PARTICLES > inst.particles.size() + worker.births.size()
                ? MAX_M2_PARTICLES - inst.particles.size() - worker.births.size() : 0;
            const int made = static_cast<int>(std::min<size_t>(
                std::min<uint32_t>(static_cast<uint32_t>(count), room), instRoom));
            if (!(em.flags & mp::kFlagBurstOnEnable)) st.acc -= static_cast<float>(made);
            st.live += static_cast<uint32_t>(made);

            const glm::mat3 rot(st.xf);
            const uint32_t tiles = static_cast<uint32_t>(std::max<uint16_t>(em.textureRows, 1)) *
                                   std::max<uint16_t>(em.textureCols, 1);
            const bool randomTile = (em.flags & mp::kFlagRandomTile) && tiles > 1 &&
                                    em.headCell.floatValues.empty();
            for (int k = 0; k < made; ++k) {
                M2Particle p{};
                p.emitterIndex = static_cast<uint16_t>(e);
                p.life = uniform01(rng) * birthDt;
                glm::vec3 lp(0.0f), lv(0.0f);
                const float speed = (uniform11(rng) * st.speedVar + 1.0f) * st.speed;  // 0x9792D0
                if (em.emitterType == mp::kEmitterSphere) {
                    // 0x981950: a shell between the two area radii.
                    const float radius = uniform01(rng) * (st.areaW - st.areaL) + st.areaL;
                    const glm::vec3 d = mp::sphereDirection(st.vRange, st.hRange, uniform11(rng),
                                                            uniform11(rng));
                    lp = d * radius;
                    glm::vec3 dir = d;
                    if (st.zSource != 0.0f) {
                        dir = lp - glm::vec3(0.0f, 0.0f, st.zSource);
                        const float l2 = glm::dot(dir, dir);
                        if (l2 > 2.384185791015625e-07f) dir *= 1.0f / std::sqrt(l2);
                    } else if (em.flags & mp::kFlagSphereVelocityUp) {
                        dir = glm::vec3(0.0f, 0.0f, 1.0f);
                    }
                    lv = dir * speed;
                } else if (em.emitterType == mp::kEmitterSpline) {
                    // 0x979870: on the spline, any direction.
                    lp = splinePoint(em.splinePoints, uniform01(rng));
                    lv = randomUnit(rng) * speed;
                } else {
                    // 0x9815C0: a length by width rectangle, centred.
                    lp = glm::vec3(uniform11(rng) * st.areaL * 0.5f, uniform11(rng) * st.areaW * 0.5f, 0.0f);
                    if (st.zSource != 0.0f) {
                        glm::vec3 dir = lp - glm::vec3(0.0f, 0.0f, st.zSource);
                        dir *= 1.0f / std::sqrt(glm::dot(dir, dir));
                        lv = dir * speed;
                    } else {
                        lv = mp::planeDirection(st.vRange, st.hRange, uniform11(rng), uniform11(rng)) * speed;
                    }
                }
                if (modelSpace) {
                    p.position = lp;
                    p.velocity = lv;
                } else {
                    p.position = glm::vec3(st.xf * glm::vec4(lp, 1.0f));
                    p.velocity = rot * lv;
                    // Flag 0x800: born anywhere along the emitter's last move.
                    if (em.flags & mp::kFlagSpawnAlongPath)
                        p.position += (st.prevPos - st.pos) * (1.0f - uniform01(rng));
                }
                if (em.flags & mp::kFlagInheritVelocity) {
                    if (em.emitterType == mp::kEmitterSpline) p.velocity += st.inheritVel;
                    else p.velocity += st.inheritVel * (uniform11(rng) * st.speedVar + 1.0f);
                }
                p.maxLife = std::max(st.life + uniform11(rng) * em.lifespanVary, 0.001f);
                p.sizeVary = mp::scaleVariation(em, uniform11(rng), uniform11(rng));
                float spinBase = em.baseSpin + uniform11(rng) * em.baseSpinVary;
                float spinSpeed = em.spin + uniform11(rng) * em.spinVary;
                if ((em.flags & mp::kFlagRandomSpinDirection) && (rng() & 1u)) {
                    spinBase = -spinBase;
                    spinSpeed = -spinSpeed;
                }
                p.spinBase = spinBase;
                p.spinSpeed = spinSpeed;
                p.tileIndex = randomTile
                    ? static_cast<uint16_t>(std::min<uint32_t>(
                          static_cast<uint32_t>(uniform01(rng) * static_cast<float>(tiles)), tiles - 1))
                    : 0;
                p.phase = static_cast<uint16_t>(rng() & 127u);
                worker.births.push_back(p);
            }
        }
        mp::mergeByEmitter(inst.particles, worker.births);

        for (size_t e = 0; e < numEm; ++e) {
            const auto& em = gpu.particleEmitters[e];
            const auto& st = inst.emitterStates[e];
            const bool modelSpace = (em.flags & mp::kFlagModelSpace) != 0;
            auto& se = ems[e];
            se.gravity = st.gravity;
            se.drag = em.drag;
            se.windTime = em.windTime;
            se.wind = em.windVector;
            se.follow = (!modelSpace && (em.flags & mp::kFlagFollowEmitter))
                ? st.followMove * followShare : glm::vec3(0.0f);
            se.center = modelSpace ? glm::vec3(0.0f) : st.pos;
            se.killPastCenter = em.emitterType == mp::kEmitterSphere &&
                                (em.flags & mp::kFlagSphereKillPastCenter);
        }
        const size_t left = mp::stepAndCompact(inst.particles.data(), inst.particles.size(),
                                               ems.data(), numEm, s);
        inst.particles.resize(left);
        for (auto& st : inst.emitterStates) st.live = 0;
        for (const auto& p : inst.particles)
            if (p.emitterIndex < numEm) ++inst.emitterStates[p.emitterIndex].live;
    };
    for (int i = 0; i < sub.whole; ++i) runSubstep(wholeStep);
    if (sub.last > 0.0f) runSubstep(sub.last);
}

// ---------------------------------------------------------------------------
// Ribbon emitters (CRibbonEmitter)
// ---------------------------------------------------------------------------
void M2Renderer::resetRibbons(M2Instance& inst) {
    for (auto& edges : inst.ribbonEdges) edges.clear();
    for (auto& track : inst.ribbonTracks) track = M2Instance::RibbonTrack{};
}

void M2Renderer::updateRibbons(M2Instance& inst, const M2ModelGPU& gpu, float dt) {
    const auto& emitters = gpu.ribbonEmitters;
    if (emitters.empty()) return;
    if (inst.ribbonEdges.size() != emitters.size()) inst.ribbonEdges.resize(emitters.size());
    if (inst.ribbonTracks.size() != emitters.size()) inst.ribbonTracks.resize(emitters.size());
    const auto& gsd = gpu.globalSequenceDurations;
    const int seq = inst.currentSequenceIndex;

    for (size_t ri = 0; ri < emitters.size(); ri++) {
        const auto& em = emitters[ri];
        auto& edges = inst.ribbonEdges[ri];
        auto& st = inst.ribbonTracks[ri];

        // The emitter's frame: bone * T(position) * world, no turn (0x828A00).
        // A bone past the skeleton means the model's root.
        glm::mat4 xf = inst.modelMatrix;
        if (em.bone < inst.boneMatrices.size()) xf = xf * inst.boneMatrices[em.bone];
        else if (!inst.boneMatrices.empty()) xf = xf * inst.boneMatrices[0];
        xf = xf * glm::translate(glm::mat4(1.0f), em.position);
        const glm::vec3 pos(xf[3]);
        if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) continue;

        // Tracks, each frame, in the playing sequence (0x828A00).
        const float visibility = sampleStep(em.visibilityTrack, seq, inst.animTime,
                                            inst.globalSequenceTime, gsd, 1.0f);
        const glm::vec3 color = m2_track::sampleVec3(em.colorTrack, seq, inst.animTime,
                                                     inst.globalSequenceTime, gsd, glm::vec3(1.0f));
        // The model's alpha is applied when the trail is drawn, to all of it
        // (a ribbon element's alpha is the model's times the track's), so a
        // missile fading out at impact takes its whole trail with it.
        const float alpha = m2_track::sampleFloat(em.alphaTrack, seq, inst.animTime,
                                                  inst.globalSequenceTime, gsd, 1.0f);
        // With no keys the heights stay at the 10 the emitter starts with (0x9808A0).
        const float above = m2_track::sampleFloat(em.heightAboveTrack, seq, inst.animTime,
                                                  inst.globalSequenceTime, gsd, 10.0f);
        const float below = m2_track::sampleFloat(em.heightBelowTrack, seq, inst.animTime,
                                                  inst.globalSequenceTime, gsd, 10.0f);
        st.texSlot = static_cast<uint32_t>(std::max(0.0f, sampleStep(
            em.texSlotTrack, seq, inst.animTime, inst.globalSequenceTime, gsd,
            static_cast<float>(st.texSlot))));
        const bool visible = visibility != 0.0f && !inst.emissionStopped;

        // SetMatrix (0x97F940): the previous frame becomes the one before; a
        // ribbon just shown (or new) starts with the previous frame equal to
        // this one, so it never joins a spot it was not at.
        const glm::vec3 axisY(xf[1]), axisZ(xf[2]);
        if (visible) {
            if (!st.hasPrev) {
                st.prevPos = pos; st.prevY = axisY; st.prevZ = axisZ;
                st.hasPrev = true;
            } else {
                st.prevPos = st.curPos; st.prevY = st.curY; st.prevZ = st.curZ;
            }
            st.curPos = pos; st.curY = axisY; st.curZ = axisZ;
        } else {
            st.hasPrev = false;  // 0x97F570
        }

        // Update (0x980090).
        const float rate = std::ceil(em.edgesPerSecond);
        const float life = em.edgeLifetime;
        float step = dt;
        if (!st.stepped && rate > 0.0f) step = 1.0f / rate + 1e-4f;  // the first lays one edge
        step = std::clamp(step, 0.0f, life);
        while (!edges.empty() && edges.front().age + step > life) edges.pop_front();

        if (visible && st.hasPrev) {
            const float total = step * rate + st.frac;
            const float dist = glm::length(st.curPos - st.prevPos);
            const glm::vec3 prevBottom = st.prevPos - st.prevY * below;
            const glm::vec3 prevTop = st.prevPos + st.prevY * above;
            const glm::vec3 curBottom = st.curPos - st.curY * below;
            const glm::vec3 curTop = st.curPos + st.curY * above;
            const glm::vec3 prevZd = st.prevZ * dist, curZd = st.curZ * dist;
            const auto edgeAt = [&](float t, float age) {
                M2Instance::RibbonEdge e;
                e.bottom = mp::ribbonEdgePoint(prevBottom, curBottom, prevZd, curZd, t);
                e.top = mp::ribbonEdgePoint(prevTop, curTop, prevZd, curZd, t);
                e.color = color;
                e.alpha = alpha;
                e.age = age;
                return e;
            };
            if (total >= 1.0f) {
                const int n = static_cast<int>(std::floor(total - 1.0f)) + 1;
                for (int k = 1; k <= n; ++k) {
                    // When in the step edge k was due; it is that much older
                    // than the step's end (the stock lays it at -t dt and the
                    // ageing below adds the step).
                    const float t = (static_cast<float>(k) - st.frac) / (total - st.frac);
                    edges.push_back(edgeAt(t, -t * step));
                }
            }
            st.frac = total - std::floor(total);
            st.head = edgeAt(1.0f, 0.0f);
            st.hasHead = true;
        }
        for (auto& e : edges) {
            const float rise = mp::ribbonGravityStep(e.age, step, em.gravity);
            e.bottom.z += rise;
            e.top.z += rise;
            e.age += step;
        }
        // A ring as long as the life holds at the rate, plus two (0x9808A0).
        const size_t ringMax = static_cast<size_t>(std::ceil(life * rate)) + 2;
        while (edges.size() > ringMax) edges.pop_front();
        st.stepped = true;
    }
}

// ---------------------------------------------------------------------------
// Ribbon rendering (0x980B70)
// ---------------------------------------------------------------------------
void M2Renderer::renderM2Ribbons(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (!ribbonPipeline_ || !ribbonAdditivePipeline_ || !ribbonVB_ || !ribbonVBMapped_) return;
    // Diagnostic: WOWEE_M2_NO_RIBBONS=1 drops every M2 ribbon trail draw.
    static const bool kNoRibbons = envFlagEnabled("WOWEE_M2_NO_RIBBONS");
    if (kNoRibbons) return;

    float* dst = static_cast<float*>(ribbonVBMapped_);
    size_t written = 0;
    ribbonDraws_.clear();
    auto& draws = ribbonDraws_;

    // Only the instances the update steps ribbons for - those on the emitter
    // list; nothing else ever has an edge.
    for (size_t instIdx : particleInstanceIndices_) {
        if (instIdx >= instances.size()) continue;
        const auto& inst = instances[instIdx];
        if (!inst.cachedModel) continue;
        const auto& gpu = *inst.cachedModel;
        if (gpu.ribbonEmitters.empty() || inst.ribbonEdges.empty()) continue;
        const bool instOnScreen = inst.cachedPaddedRadius <= 0.0f ||
            cachedFrustum_.intersectsSphere(inst.cachedCullCenter, inst.cachedPaddedRadius + 30.0f);

        for (size_t ri = 0; ri < gpu.ribbonEmitters.size() && ri < inst.ribbonEdges.size(); ri++) {
            const auto& edges = inst.ribbonEdges[ri];
            if (edges.empty()) continue;  // the stock draws nothing with no laid edge
            const auto& st = inst.ribbonTracks[ri];
            const auto& em = gpu.ribbonEmitters[ri];
            if (!instOnScreen) {
                const glm::vec3 head = 0.5f * (edges.back().bottom + edges.back().top);
                const glm::vec3 tail = 0.5f * (edges.front().bottom + edges.front().top);
                const float width = glm::length(edges.back().top - edges.back().bottom);
                if (!cachedFrustum_.intersectsSphere(0.5f * (head + tail),
                        0.5f * glm::length(head - tail) + width + 10.0f)) continue;
            }
            // The material's blend (the model's material table, not a batch).
            const bool additive = em.blendMode == 3 || em.blendMode == 4;
            VkPipeline pipe = additive ? ribbonAdditivePipeline_ : ribbonPipeline_;
            VkDescriptorSet texSet = (ri < gpu.ribbonTexSets.size()) ? gpu.ribbonTexSets[ri] : VK_NULL_HANDLE;
            if (!texSet) continue;

            // The texture cell and its rectangle (0x97F510): u runs along the
            // trail by age over the edge life, one cell wide; the bottom
            // edge is v = 0, the top v = 1, one cell tall.
            const uint32_t cols = std::max<uint16_t>(em.textureCols, 1);
            const uint32_t rows = std::max<uint16_t>(em.textureRows, 1);
            const float cellW = 1.0f / static_cast<float>(cols);
            const float cellH = 1.0f / static_cast<float>(rows);
            const float u0 = static_cast<float>(st.texSlot % cols) * cellW;
            const float v0 = static_cast<float>(st.texSlot / cols) * cellH;
            const float invLife = em.edgeLifetime > 0.0f ? 1.0f / em.edgeLifetime : 0.0f;
            const float modelAlpha = std::clamp(inst.fade, 0.0f, 1.0f);
            if (!(modelAlpha > 0.0f)) continue;

            const uint32_t firstVert = static_cast<uint32_t>(written);
            const auto put = [&](const M2Instance::RibbonEdge& e) {
                if (written + 2 > MAX_RIBBON_VERTS) return;
                const float u = e.age * cellW * invLife + u0;
                const glm::vec3* ends[2] = {&e.bottom, &e.top};
                const float vs[2] = {v0, v0 + cellH};
                for (int k = 0; k < 2; ++k) {
                    float* d = dst + written * 9;
                    d[0] = ends[k]->x; d[1] = ends[k]->y; d[2] = ends[k]->z;
                    d[3] = e.color.r; d[4] = e.color.g; d[5] = e.color.b;
                    d[6] = e.alpha * modelAlpha;
                    d[7] = u;
                    d[8] = vs[k];
                    ++written;
                }
            };
            // Oldest first, then the live head at the emitter.
            for (const auto& e : edges) put(e);
            if (st.hasHead) put(st.head);
            const uint32_t vertCount = static_cast<uint32_t>(written) - firstVert;
            if (vertCount >= 4) {
                draws.push_back({.texSet = texSet, .pipeline = pipe, .firstVertex = firstVert,
                                 .vertexCount = vertCount, .blendMode = em.blendMode,
                                 .unfogged = (em.materialFlags & 0x2u) ? 1u : 0u});
            } else {
                written = firstVert;
            }
        }
    }
    // The strips built elsewhere: spell chain effects and swing trails.
    for (const auto& strip : effectStrips_) {
        const size_t n = strip.verts.size() / 9;
        if (n < 4 || written + n > MAX_RIBBON_VERTS) continue;
        VkTexture* tex = strip.texture ? strip.texture : whiteTexture_.get();
        VkDescriptorSet texSet = effectTextureSet(tex);
        if (!texSet) continue;
        // EGxBlend: 3 Add, 6 ModAdd, 7 InvSrcAlphaAdd, 10 NoAlphaAdd add;
        // the rest blend by alpha. The fog push takes the M2 blend that
        // matches (3 additive, 2 alpha).
        const bool additive = strip.gxBlend == 3 || strip.gxBlend == 6 || strip.gxBlend == 7 ||
                              strip.gxBlend == 10;
        std::memcpy(dst + written * 9, strip.verts.data(), n * 9 * sizeof(float));
        draws.push_back({.texSet = texSet,
                         .pipeline = additive ? ribbonAdditivePipeline_ : ribbonPipeline_,
                         .firstVertex = static_cast<uint32_t>(written),
                         .vertexCount = static_cast<uint32_t>(n),
                         .blendMode = additive ? 3u : 2u,
                         .unfogged = 0u});
        written += n;
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
        const uint32_t material[2] = {dc.blendMode, dc.unfogged};
        vkCmdPushConstants(cmd, ribbonPipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(material), material);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &ribbonVB_, &offset);
        vkCmdDraw(cmd, dc.vertexCount, 1, dc.firstVertex, 0);
    }
}

// ---------------------------------------------------------------------------
// Particle records (0x97E730 / 0x97BE80)
// ---------------------------------------------------------------------------
size_t M2Renderer::writeParticleRecordsGpu(uint32_t frame) {
    using mp::ParticleRecord;
    auto& groups = particleGroups_;
    auto* const recBase = static_cast<ParticleRecord*>(particleRecordMapped_[frame]);
    auto* const curveData = static_cast<glm::vec4*>(particleCurveMapped_[frame]);
    const uint32_t budget = particleRecordBudget_;
    uint32_t written = 0;
    uint32_t curvesUsed = 0;
    particleCurveSlots_.clear();
    ParticleGroup* runGroup = nullptr;
    const glm::vec3 camPos = cachedCamPos_;

    for (size_t idx : particleDrawOrder_) {
        if (written >= budget) break;
        const auto& inst = instances[idx];
        const auto& gpu = *inst.cachedModel;
        const size_t numEm = gpu.particleEmitters.size();
        if (gpu.particleCurves.size() != numEm * mp::kCurveVec4s) continue;
        // A ParticleColor.dbc override's curves (setInstanceParticleColors),
        // else the model's.
        const glm::vec4* curveSet = gpu.particleCurves.data();
        if (inst.particleCurveOverride && inst.particleCurveOverride->size() == numEm * mp::kCurveVec4s)
            curveSet = inst.particleCurveOverride->data();
        if (inst.emitterStates.size() != numEm) continue;
        const float modelAlpha = std::clamp(inst.fade, 0.0f, 1.0f);

        int lastEmitter = -1;
        bool emitterDrawn = false;
        ParticleGroup* group = nullptr;
        const pipeline::M2ParticleEmitter* em = nullptr;
        const M2Instance::EmitterState* st = nullptr;
        uint32_t curveBase = 0;
        uint32_t emitterFirst = 0;
        uint32_t emitterCount = 0;
        uint32_t emitterCap = 0;
        bool modelSpace = false, head = false, tail = false, spins = false, lit = false,
             unfogged = false, sortDepth = false;
        mp::RecordKind headKind = mp::kKindBillboard;
        glm::vec3 xyQuat(0.0f);
        float sizeScale = 1.0f;
        glm::mat3 rot(1.0f);

        const auto closeEmitter = [&]() {
            // Flag 0x2: back to front within the emitter (0x97E580).
            if (sortDepth && written > emitterFirst + 1) {
                std::sort(recBase + emitterFirst, recBase + written,
                          [&](const ParticleRecord& a, const ParticleRecord& b) {
                              const glm::vec3 da = a.position - camPos, db = b.position - camPos;
                              return glm::dot(da, da) > glm::dot(db, db);
                          });
            }
        };

        for (const auto& p : inst.particles) {
            if (p.emitterIndex >= numEm) continue;
            if (p.emitterIndex != lastEmitter) {
                closeEmitter();
                lastEmitter = p.emitterIndex;
                em = &gpu.particleEmitters[p.emitterIndex];
                st = &inst.emitterStates[p.emitterIndex];
                emitterFirst = written;
                emitterCount = 0;

                VkTexture* tex = whiteTexture_.get();
                if (p.emitterIndex < static_cast<int>(gpu.particleTextures.size()))
                    tex = gpu.particleTextures[p.emitterIndex];
                const uint16_t tilesX = std::max<uint16_t>(em->textureCols, 1);
                const uint16_t tilesY = std::max<uint16_t>(em->textureRows, 1);
                const uint8_t blendType = static_cast<uint8_t>(std::min<uint8_t>(em->blendingType, 6));
                ParticleGroupKey key{.texture = tex, .blendType = blendType, .tilesX = tilesX, .tilesY = tilesY};
                group = &groups[key];
                group->texture = tex;
                group->blendType = blendType;
                group->tilesX = tilesX;
                group->tilesY = tilesY;
                if (group->preAllocSet == VK_NULL_HANDLE &&
                    p.emitterIndex < static_cast<int>(gpu.particleTexSets.size())) {
                    group->preAllocSet = gpu.particleTexSets[p.emitterIndex];
                }

                const uint32_t f = em->flags;
                modelSpace = (f & mp::kFlagModelSpace) != 0;
                head = (f & mp::kFlagHead) != 0;
                tail = (f & mp::kFlagTail) != 0;
                spins = mp::emitterSpins(*em);
                lit = !(f & mp::kFlagUnlit);
                unfogged = (f & mp::kFlagUnfogged) != 0;
                sortDepth = (f & mp::kFlagSortByDepth) != 0;
                // The stock draws at most 0x4000 vertices an emitter, four a quad.
                {
                    const uint32_t quads = (head ? 1u : 0u) + (tail ? 1u : 0u);
                    emitterCap = quads ? 0x4000u / (4u * quads) : 0u;
                }
                rot = glm::mat3(st->xf);
                // Flag 0x20: sizes grow with the emitter matrix's scale (+0x1EC).
                sizeScale = (f & mp::kFlagInheritScale) ? glm::length(glm::vec3(st->xf[0])) : 1.0f;
                headKind = (f & mp::kFlagXYQuad) ? mp::kKindXYQuad : mp::kKindBillboard;
                if (headKind == mp::kKindXYQuad) {
                    // The quad lies in the particle space's XY plane: the
                    // world's for world-space particles, the emitter's
                    // (normalised) for model-space ones.
                    glm::quat q(1.0f, 0.0f, 0.0f, 0.0f);
                    if (modelSpace) {
                        const glm::vec3 ax = glm::normalize(glm::vec3(st->xf[0]));
                        const glm::vec3 ay = glm::normalize(glm::vec3(st->xf[1]));
                        const glm::vec3 az = glm::normalize(glm::cross(ax, ay));
                        q = glm::quat_cast(glm::mat3(ax, glm::cross(az, ax), az));
                    }
                    if (q.w < 0.0f) q = -q;
                    xyQuat = glm::vec3(q.x, q.y, q.z);
                }

                // This emitter's curves, uploaded the first time this frame
                // anything uses them.
                const glm::vec4* curve = curveSet + p.emitterIndex * mp::kCurveVec4s;
                auto [slot, fresh] = particleCurveSlots_.try_emplace(curve, 0u);
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
                    slot->second = curvesUsed * mp::kCurveVec4s;
                    std::memcpy(static_cast<void*>(curveData + slot->second), curve,
                                sizeof(glm::vec4) * mp::kCurveVec4s);
                    ++curvesUsed;
                }
                curveBase = slot->second;
                emitterDrawn = head || tail;
            }
            if (!emitterDrawn || emitterCount >= emitterCap) continue;

            const float age = p.life;
            const mp::Twinkle tw = mp::twinkle(*em, p.phase, age);
            if (!tw.visible) continue;
            const float r = age / std::max(p.maxLife, 0.001f);
            const glm::vec2 size = p.sizeVary * (tw.scale * sizeScale);
            const glm::vec3 wpos = modelSpace ? glm::vec3(st->xf * glm::vec4(p.position, 1.0f)) : p.position;
            const glm::vec3 wvel = modelSpace ? rot * p.velocity : p.velocity;
            const float angle = spins ? age * p.spinSpeed + p.spinBase : 0.0f;

            const uint32_t need = (head ? 1u : 0u) + (tail ? 1u : 0u);
            if (written + need > budget) break;
            if (group != runGroup) {
                runGroup = group;
                particleRuns_.push_back({.group = group, .first = written, .count = 0});
            }
            if (head) {
                ParticleRecord& rec = recBase[written++];
                rec.position = wpos;
                rec.lifeRatio = r;
                rec.sizeX = size.x;
                rec.sizeY = size.y;
                rec.angle = angle;
                rec.curveBase = curveBase;
                // A head cell from its curve, else the particle's random cell.
                int32_t cell = mp::particleCell(em->headCell, r);
                if (cell < 0) cell = p.tileIndex;
                mp::RecordKind kind = headKind;
                rec.vec = xyQuat;
                // Flag 0x4: stretched along the velocity, while it has one.
                if ((em->flags & mp::kFlagVelocityQuad) &&
                    glm::dot(p.velocity, p.velocity) > 2.384185791015625e-07f) {
                    kind = mp::kKindVelocity;
                    rec.vec = -wvel;
                }
                rec.packed = mp::packRecord(static_cast<uint32_t>(std::max(cell, 0)), kind,
                                            spins && kind != mp::kKindVelocity, lit, unfogged, modelAlpha);
                ++particleRuns_.back().count;
            }
            if (tail) {
                ParticleRecord& rec = recBase[written++];
                float tailLen = em->tailLength;
                if ((em->flags & mp::kFlagClampTailToAge) && age < tailLen) tailLen = age;
                rec.position = wpos;
                rec.lifeRatio = r;
                rec.vec = -wvel * tailLen;
                rec.sizeX = size.x;
                rec.sizeY = size.y;
                rec.angle = 0.0f;
                rec.curveBase = curveBase;
                const int32_t cell = std::max(mp::particleCell(em->tailCell, r), 0);
                rec.packed = mp::packRecord(static_cast<uint32_t>(cell), mp::kKindTail, false, lit,
                                            unfogged, modelAlpha);
                ++particleRuns_.back().count;
            }
            ++emitterCount;
        }
        closeEmitter();
    }
    return written;
}

void M2Renderer::renderM2Particles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    // Diagnostic: WOWEE_M2_NO_PARTICLES=1 drops every M2 particle draw, which
    // tells a particle artifact apart from a skinned-geometry one.
    static const bool kNoParticles = envFlagEnabled("WOWEE_M2_NO_PARTICLES");
    if (kNoParticles) return;

    const uint32_t frame = vkCtx_->getCurrentFrame() % 2;
    bool ready = particleRecordMapped_[frame] && particleCurveMapped_[frame] && particleCurveSet_[frame];
    for (VkPipeline p : particleGpuPipelines_) ready = ready && p != VK_NULL_HANDLE;
    static bool saidPath = false;
    if (!saidPath) {
        saidPath = true;
        if (ready) LOG_INFO("M2 particles: stock quads from records (budget ", particleRecordBudget_, ")");
        else LOG_WARNING("M2 particles: quad pipelines unavailable (m2_particle_gpu.*.spv missing?) - not drawn");
    }
    if (!ready) return;

    // Reuse persistent map - keep the bucket structure, drop last frame's set.
    for (auto& [k, g] : particleGroups_) g.preAllocSet = VK_NULL_HANDLE;
    particleRuns_.clear();

    // Only the instances that carry emitters, in two passes over the fixed
    // record budget: emitters near the camera - spell kits, missiles, a
    // campfire - claim it first.
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
        if (inst.cachedPaddedRadius > 0.0f &&
            !cachedFrustum_.intersectsSphere(inst.cachedCullCenter, inst.cachedPaddedRadius + 30.0f)) continue;
        particleDrawOrder_.push_back(idx);
        if (d2 <= kNearFirstSq) std::swap(particleDrawOrder_[nearCount++], particleDrawOrder_.back());
    }
    const size_t totalRecords = writeParticleRecordsGpu(frame);
    particleVertsDrawn_.store(static_cast<uint32_t>(totalRecords), std::memory_order_relaxed);
    particleRunsDrawn_.store(static_cast<uint32_t>(particleRuns_.size()), std::memory_order_relaxed);
    if (totalRecords == 0) return;

    const VkPipelineLayout layout = particleGpuPipelineLayout_;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &perFrameSet, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, 1, &particleCurveSet_[frame], 0, nullptr);
    VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &particleRecordBuf_[frame], &vbOffset);

    VkPipeline currentPipeline = VK_NULL_HANDLE;
    for (auto& run : particleRuns_) {
        if (run.count == 0 || !run.group) continue;
        ParticleGroup& group = *run.group;
        const uint32_t blendType = std::min<uint32_t>(group.blendType, 6);
        VkPipeline desired = particleGpuPipelines_[blendType];
        if (desired != currentPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desired);
            currentPipeline = desired;
        }

        // Use pre-allocated stable descriptor set; fall back to per-frame alloc only if unavailable
        VkDescriptorSet texSet = group.preAllocSet;
        if (texSet == VK_NULL_HANDLE) {
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
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &texSet, 0, nullptr);
        }

        struct { float tileX, tileY; uint32_t blend, pad; } pc = {
            .tileX = static_cast<float>(group.tilesX), .tileY = static_cast<float>(group.tilesY),
            .blend = blendType, .pad = 0};
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(pc), &pc);
        // Four strip corners per record; the records are this run's instances.
        vkCmdDraw(cmd, 4, run.count, 0, run.first);
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


VkDescriptorSet M2Renderer::effectTextureSet(VkTexture* tex) {
    if (!tex || !tex->isValid()) tex = whiteTexture_.get();
    if (!tex || !tex->isValid() || !particleTexLayout_ || !materialDescPool_) return VK_NULL_HANDLE;
    if (auto it = effectTexSets_.find(tex); it != effectTexSets_.end()) return it->second;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = materialDescPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &particleTexLayout_;
    if (vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorImageInfo imgInfo = tex->descriptorInfo();
    VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imgInfo;
    vkUpdateDescriptorSets(vkCtx_->getDevice(), 1, &write, 0, nullptr);
    effectTexSets_[tex] = set;
    return set;
}

} // namespace rendering
} // namespace wowee
