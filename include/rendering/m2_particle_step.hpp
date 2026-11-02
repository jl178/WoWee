#pragma once

#include "rendering/m2_track_sampler.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace wowee::rendering::m2_particle {

/// What one of a particle emitter's float tracks can give, found once per
/// model so the per-frame emitter setup can skip the key search.
///
/// Most emitter tracks (rate, lifespan, speed, spread, gravity) hold one key
/// per sequence with the same value in every one, or no keys at all. Sampled
/// every frame for every awake emitter, each still paid the global-sequence
/// fmod and the key search to return that one value. The values here are the
/// ones m2_track::sampleFloat returns with a default of 0, bit for bit:
/// - Always: every sample gives `value` (no keys anywhere, or a global
///   sequence whose keys are a single value).
/// - PerSequence: every sequence holds one key of the same value; a sequence
///   index past the track reads array 0, the same value, as the sampler does
///   (and the stock client, 0x8284d0); no sequence (index < 0) gives 0.
/// - Sampled: anything else, sampled as before.
/// One key per sequence is required, not merely equal values: glm::mix of a
/// value with itself is not always the value to the last bit.
struct TrackConst {
    enum class Kind : uint8_t { Sampled, Always, PerSequence };
    Kind kind = Kind::Sampled;
    float value = 0.0f;
    uint32_t sequenceCount = 0;
};

inline TrackConst classifyTrack(const pipeline::M2AnimationTrack& track,
                                const std::vector<uint32_t>& globalSequenceDurations) {
    using Keys = pipeline::M2AnimationTrack::SequenceKeys;
    const auto keyCount = [](const Keys& keys) {
        return std::min(keys.timestamps.size(), keys.floatValues.size());
    };
    TrackConst c;
    if (track.globalSequence >= 0 &&
        static_cast<size_t>(track.globalSequence) < globalSequenceDurations.size()) {
        // A global sequence always reads sequence 0.
        if (track.sequences.empty() || keyCount(track.sequences[0]) == 0) {
            c.kind = TrackConst::Kind::Always;
            c.value = 0.0f;
        } else if (keyCount(track.sequences[0]) == 1) {
            c.kind = TrackConst::Kind::Always;
            c.value = track.sequences[0].floatValues[0];
        }
        return c;
    }
    bool allEmpty = true, allOne = true, have = false;
    float v = 0.0f;
    for (const auto& keys : track.sequences) {
        const size_t n = keyCount(keys);
        if (n != 0) allEmpty = false;
        if (n != 1) {
            allOne = false;
            continue;
        }
        if (!have) {
            v = keys.floatValues[0];
            have = true;
        } else if (std::memcmp(&v, &keys.floatValues[0], sizeof(float)) != 0) {
            allOne = false;
        }
    }
    if (allEmpty) {
        c.kind = TrackConst::Kind::Always;
        c.value = 0.0f;
    } else if (allOne) {
        c.kind = TrackConst::Kind::PerSequence;
        c.value = v;
        c.sequenceCount = static_cast<uint32_t>(track.sequences.size());
    }
    return c;
}

/// m2_track::sampleFloat(track, ..., 0.0f), through what classifyTrack found.
inline float sampleTrack(const TrackConst& c, const pipeline::M2AnimationTrack& track,
                         int sequenceIndex, float animTimeMs, float globalTimeMs,
                         const std::vector<uint32_t>& globalSequenceDurations) {
    switch (c.kind) {
        case TrackConst::Kind::Always:
            return c.value;
        case TrackConst::Kind::PerSequence:
            return (sequenceIndex >= 0 && c.sequenceCount > 0) ? c.value : 0.0f;
        case TrackConst::Kind::Sampled:
            break;
    }
    return m2_track::sampleFloat(track, sequenceIndex, animTimeMs, globalTimeMs,
                                 globalSequenceDurations, 0.0f);
}

/// What the step needs of one emitter for one sub-step; filled per emitter,
/// read for each of its particles.
struct StepEmitter {
    float gravity = 0.0f;      // down the particle space's Z
    float drag = 0.0f;         // per second, linear
    float windTime = 0.0f;     // wind blows on particles younger than this
    glm::vec3 wind{0.0f};
    glm::vec3 follow{0.0f};    // emitter motion this sub-step (flag 0x4000)
    glm::vec3 center{0.0f};    // emitter position, particle space
    bool killPastCenter = false;  // sphere flag 0x80
};

/// One particle's sub-step, CParticleEmitter2 0x97DD20 then 0x979BB0:
/// - age; past the lifespan it dies;
/// - wind while younger than windTime;
/// - followed along with the emitter, once older than two sub-steps;
/// - moved under gravity in closed form over the step;
/// - drag takes min(drag * dt, 1) of the velocity, after the move;
/// - a sphere particle with flag 0x80 dies once it moves away from the
///   centre (one shot inward dies on crossing it).
/// Returns false when the particle dies.
template <class Particle>
inline bool stepParticle(Particle& p, const StepEmitter& s, float dt) {
    p.life += dt;
    if (p.life >= p.maxLife) return false;
    if (p.life < s.windTime) p.velocity += s.wind * dt;
    if (p.life > dt + dt) p.position += s.follow;
    const glm::vec3 v0 = p.velocity;
    p.position += v0 * dt;
    p.position.z -= s.gravity * dt * dt * 0.5f;
    p.velocity.z -= s.gravity * dt;
    if (s.drag != 0.0f) p.velocity -= p.velocity * std::min(dt * s.drag, 1.0f);
    // 0x978B70: components below 1e-8 are flushed to zero.
    for (int k = 0; k < 3; ++k)
        if (std::abs(p.velocity[k]) < 1e-8f) p.velocity[k] = 0.0f;
    if (s.killPastCenter && glm::dot(p.position - s.center, v0 * dt) > 0.0f) return false;
    return true;
}

/// Steps every particle by dt and closes the gaps in order, returning how
/// many are left. Order is kept so particles stay grouped by emitter: the
/// draw closes a run at every change of emitter. A particle whose emitter is
/// past `numEm` only ages and moves.
template <class Particle>
size_t stepAndCompact(Particle* ps, size_t n, const StepEmitter* ems, size_t numEm, float dt) {
    static const StepEmitter kNone{};
    size_t w = 0;
    for (size_t i = 0; i < n; ++i) {
        Particle p = ps[i];
        const size_t e = p.emitterIndex;
        if (!stepParticle(p, e < numEm ? ems[e] : kNone, dt)) continue;
        ps[w++] = p;
    }
    return w;
}

/// Appends `births` to `ps` with each birth placed after the last particle
/// of its own emitter, so a list grouped by ascending emitter index stays so.
///
/// Both lists ascend by emitter (the emitters spawn in order and the step
/// keeps the order), so this is the back half of a merge: it walks from the
/// end and stops once the births are placed. Particles of emitters before
/// the first one that spawned are not touched; a model whose last emitter is
/// the only one spawning moves nothing.
template <class Particle>
void mergeByEmitter(std::vector<Particle>& ps, const std::vector<Particle>& births) {
    if (births.empty()) return;
    size_t i = ps.size();
    size_t j = births.size();
    ps.resize(i + j);
    size_t k = i + j;
    while (j > 0) {
        if (i > 0 && births[j - 1].emitterIndex < ps[i - 1].emitterIndex) {
            ps[--k] = ps[--i];
        } else {
            ps[--k] = births[--j];
        }
    }
}

/// [begin, end) ranges that split `weights` into `parts` runs of about equal
/// total weight, in order. Ranges can be empty. `out` holds parts + 1 bounds.
inline void splitByWeight(const std::vector<uint32_t>& weights, size_t parts,
                          std::vector<size_t>& out) {
    out.assign(parts + 1, weights.size());
    out[0] = 0;
    if (parts == 0) return;
    uint64_t total = 0;
    for (uint32_t w : weights) total += w;
    uint64_t acc = 0;
    size_t part = 1;
    for (size_t i = 0; i < weights.size() && part < parts; ++i) {
        const uint64_t before = acc;
        acc += weights[i];
        // Part `part` ends at item i once the running total passes its share:
        // before item i or after it, whichever lands nearer the share.
        while (part < parts && acc * parts >= total * part) {
            const double share = static_cast<double>(total) * static_cast<double>(part) /
                                 static_cast<double>(parts);
            const bool beforeNearer = share - static_cast<double>(before) <
                                      static_cast<double>(acc) - share;
            const size_t at = (beforeNearer && i > out[part - 1]) ? i : i + 1;
            out[part] = std::max(at, out[part - 1]);
            ++part;
        }
    }
}

} // namespace wowee::rendering::m2_particle
