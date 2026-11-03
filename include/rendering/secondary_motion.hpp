#pragma once

/// "Capes, tails and hair" (docs/enhancements-plan.md E7): one spring chain
/// system that adds a little secondary motion on top of the authored
/// animation.
///
/// The stock client poses every bone from its animation tracks and nothing
/// else; a cape swings as the run animation swings it and hangs still the
/// moment the unit stops, however hard it turned or landed. With the row on, a
/// few chosen bones get a small extra rotation about their own pivot after the
/// stock pose is sampled - the animation still drives the base pose - from a
/// damped spring per bone, driven by how the unit's body moves through the air
/// (its acceleration, turning, jumping and falling, the drag of running) and
/// by the foliage wind. Off, nothing here runs and the bones are the sampled
/// animation exactly.
///
/// WHICH BONES (findChains; the rule, worked out against every character and
/// creature model in 3.3.5a - see the E7 entry in the plan for the survey):
///
///   A bone is *free* when it is not a key bone, has a parent, has no key bone
///   anywhere below it (so moving it never moves a hand, a head or the spine
///   that carries them) and some vertex is weighted to it.
///
///   A *chain* is a run of free bones, parent to child, each with exactly one
///   free child, that starts where the free tree starts or branches and ends
///   at a leaf; two bones at least. A *fan* is three or more free leaf bones
///   of one parent that lie on the midline behind it, ordered from the body
///   out: the flat tail rigs of the wolf and the great dragons, whose tail
///   bones all hang off the pelvis.
///
///   Each chain is then one of three kinds, or nothing:
///   - Cloth (capes, tabards, robe flaps; character models): at least 60% of
///     its vertex weight is in the cloak (15xx), tabard (12xx) or robe (13xx)
///     geosets and at most 10% in the body skin (geoset 0). On HumanMale these
///     are the back flap 5-12 the cloak hangs from and the front flap 6-13-19.
///   - Hair (character models): its root is below the Head key bone and at
///     least 60% of its weight is in hair or facial hair geosets (1-399):
///     ponytails, braids, long beards.
///   - Tail (any model): its nearest key-bone ancestor is the Waist (key bone
///     5), it is not below the Head, at least 60% of its weight is body, it
///     points backward (the tip at least half as far back as it is down, and
///     not more than a third as far sideways), it starts within a tenth of the
///     model's height of the midline, it is at least 8% of that height long
///     and, unless a fan, it stays clear of the ground (above 12% of the
///     height) - which leaves the legs out. Wings, which spread sideways or
///     hang off the chest, never pass.
///
/// THE SPRING (stepChain): each bone has a two-way swing angle - out from the
/// body (for a tail: up) and to the side - pulled back to the authored pose by
/// a damped spring, whose steady answer to a push f is K f (K per kind, shared
/// along the chain). The push is the pseudo-force on the chain's probe point
/// (a point out behind its root) as the model moves: minus its acceleration,
/// plus air drag against its velocity relative to the wind. Hanging kinds also
/// take a share of the vertical push, so a fall floats a cape up and a landing
/// drops it. The tip bones are softer than the root ones, so a swing travels
/// down the chain. Hard limits per bone keep everything small and stop a cape
/// swinging into the legs (a few degrees toward the body only).
///
/// STABILITY: the step is semi-implicit Euler in substeps of at most 1/120 s;
/// a frame longer than 50 ms simulates only 50 ms, one longer than 250 ms (a
/// hitch, a unit coming back into view) or a jump of the probe of more than
/// 4 yd (a teleport) starts over from rest; velocities and accelerations are
/// clamped and any non-finite value resets the chain.
///
/// Pure functions and plain data, for CharacterRenderer and
/// test_secondary_motion.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wowee {
namespace rendering {
namespace secondary_motion {

enum class ChainKind : uint8_t { Cloth = 0, Hair = 1, Tail = 2 };

/// The row's values: off, capes and robes only, everything.
inline constexpr int kLevelOff = 0;
inline constexpr int kLevelCloth = 1;
inline constexpr int kLevelAll = 2;

inline bool kindEnabled(ChainKind k, int level) {
    return level >= kLevelAll || (level == kLevelCloth && k == ChainKind::Cloth);
}

/// What findChains needs of one bone. The weights are the summed vertex
/// weights (0-255 each) of the bone over the model's skin, split by the kind
/// of geoset the vertices are in; for a creature everything is body.
struct BoneInfo {
    int32_t keyBoneId = -1;
    int32_t parent = -1;
    glm::vec3 pivot{0.0f};
    float body = 0.0f;
    float hair = 0.0f;
    float cloth = 0.0f;
    float total = 0.0f;
};

/// Geoset id to weight class for a character model: 0 body, 1-399 hair and
/// facial hair, 1200-1399 tabard and robe, 1500-1599 cloak, else other.
enum class GeosetClass : uint8_t { Body, Hair, Cloth, Other };
inline GeosetClass classifyGeoset(uint16_t id, bool characterModel) {
    if (!characterModel || id == 0) return GeosetClass::Body;
    if (id < 400) return GeosetClass::Hair;
    if ((id >= 1200 && id < 1400) || (id >= 1500 && id < 1600)) return GeosetClass::Cloth;
    return GeosetClass::Other;
}

struct Chain {
    ChainKind kind = ChainKind::Cloth;
    bool fan = false;              ///< siblings of one parent, not parent to child
    std::vector<uint16_t> bones;   ///< root (body end) first
    glm::vec3 restDir{0, 0, -1};   ///< unit, root to tip, model space
    glm::vec3 outward{-1, 0, 0};   ///< unit, perpendicular to restDir: swing.x moves the tip this way
    glm::vec3 lateral{0, 1, 0};    ///< unit: swing.y moves the tip this way
    glm::vec3 probe{0.0f};         ///< model-space point whose motion drives the chain
    float length = 0.0f;           ///< summed segment lengths, model units
};

/// Most bones a chain or a model keeps (the longest found is 10 and the most
/// on one model 23, DraeneiMale).
inline constexpr size_t kMaxChainBones = 12;
inline constexpr size_t kMaxModelBones = 32;

/// Tuning of one kind. Limits are for the whole chain, radians, shared evenly
/// among its bones; K is the steady swing per unit push (rad per yd/s^2) of
/// the whole chain, also shared.
struct Params {
    float freqRootHz;
    float freqTipHz;
    float damping;     ///< ratio of critical
    float gainK;
    float outLimit;    ///< away from the body (tail: up)
    float inLimit;     ///< toward the body (tail: down)
    float sideLimit;
    float vertical;    ///< share of the vertical push taken as an outward one (hanging kinds)
};

inline Params paramsFor(ChainKind k) {
    switch (k) {
        // A cloak: about 7 degrees more trail at a run, up to 29 when
        // setting off or landing, 4 toward the legs, 14 to the side.
        case ChainKind::Cloth: return {2.4f, 1.5f, 0.45f, 0.018f, 0.50f, 0.07f, 0.24f, 0.5f};
        case ChainKind::Hair:  return {3.0f, 2.0f, 0.50f, 0.012f, 0.26f, 0.05f, 0.17f, 0.4f};
        case ChainKind::Tail:  return {2.0f, 1.2f, 0.35f, 0.030f, 0.21f, 0.17f, 0.35f, 0.0f};
    }
    return {2.0f, 1.5f, 0.5f, 0.01f, 0.2f, 0.05f, 0.2f, 0.0f};
}

/// A long chain swings slower, as a pendulum does: past 1.5 model units of
/// length (a dragon's or a kodo's tail) the frequencies fall with the square
/// root of the length, to no less than 0.3 of their value.
inline float lengthFrequencyScale(float length) {
    return std::clamp(std::sqrt(1.5f / std::max(length, 1.5f)), 0.3f, 1.0f);
}

namespace detail {

inline glm::vec3 safeNormalize(const glm::vec3& v, const glm::vec3& fallback) {
    const float l2 = glm::dot(v, v);
    return l2 > 1e-10f ? v / std::sqrt(l2) : fallback;
}

inline glm::vec3 perpendicularTo(const glm::vec3& v, const glm::vec3& d, const glm::vec3& fallback) {
    const glm::vec3 p = v - d * glm::dot(d, v);
    return glm::dot(p, p) > 0.0009f ? glm::normalize(p) : fallback;
}

}  // namespace detail

/// Fill restDir, outward, lateral, probe and length from the bones' pivots.
inline void frameChain(Chain& c, const std::vector<BoneInfo>& bones) {
    using detail::perpendicularTo;
    using detail::safeNormalize;
    const glm::vec3 root = bones[c.bones.front()].pivot;
    const glm::vec3 tip = bones[c.bones.back()].pivot;
    c.length = 0.0f;
    for (size_t i = 0; i + 1 < c.bones.size(); ++i)
        c.length += glm::distance(bones[c.bones[i]].pivot, bones[c.bones[i + 1]].pivot);
    c.restDir = safeNormalize(tip - root, glm::vec3(0, 0, -1));
    glm::vec3 out;
    if (c.kind == ChainKind::Tail) {
        out = glm::vec3(0, 0, 1);  // a tail swings up and down, and sideways
    } else {
        // Away from the body: from the model's vertical axis out through the
        // root (behind for a cloak and a ponytail, in front for a front flap).
        const glm::vec3 h(root.x, root.y, 0.0f);
        out = glm::dot(h, h) > 0.0009f ? glm::normalize(h) : glm::vec3(-1, 0, 0);
    }
    c.outward = perpendicularTo(out, c.restDir,
                                perpendicularTo(glm::vec3(0, 0, 1), c.restDir, glm::vec3(1, 0, 0)));
    c.lateral = safeNormalize(glm::cross(c.restDir, c.outward), glm::vec3(0, 1, 0));
    // The point whose motion drives it: out behind the root by a lever, so a
    // turn on the spot swings a cloak; along a tail, its middle.
    if (c.kind == ChainKind::Tail)
        c.probe = root + c.restDir * (0.6f * c.length);
    else
        c.probe = root + c.outward * 0.3f + c.restDir * (0.5f * c.length);
}

/// The chains of a skeleton, by the rule at the top of this file.
inline std::vector<Chain> findChains(const std::vector<BoneInfo>& bones, bool characterModel) {
    std::vector<Chain> out;
    const int n = static_cast<int>(bones.size());
    if (n < 2) return out;
    auto validParent = [&](int i) { return bones[i].parent >= 0 && bones[i].parent < n && bones[i].parent != i; };

    // Key bones below: walk children before parents (parents precede children
    // in an M2; a bone out of order is simply not marked through).
    std::vector<uint8_t> keyBelow(n, 0), underHead(n, 0), free(n, 0);
    for (int i = n - 1; i >= 0; --i) {
        if (bones[i].keyBoneId >= 0) keyBelow[i] = 1;
        if (keyBelow[i] && validParent(i) && bones[i].parent < i) keyBelow[bones[i].parent] = 1;
    }
    for (int i = 0; i < n; ++i) {
        underHead[i] = bones[i].keyBoneId == 6 ||
                       (validParent(i) && bones[i].parent < i && underHead[bones[i].parent]);
        free[i] = bones[i].keyBoneId < 0 && validParent(i) && !keyBelow[i] && bones[i].total > 0.0f;
    }
    std::vector<std::vector<int>> freeKids(n), allKids(n);
    for (int i = 0; i < n; ++i) {
        if (!validParent(i)) continue;
        allKids[bones[i].parent].push_back(i);
        if (free[i] && free[bones[i].parent]) freeKids[bones[i].parent].push_back(i);
    }
    float minZ = 1e30f, maxZ = -1e30f;
    for (const auto& b : bones) {
        if (b.total <= 0.0f) continue;
        minZ = std::min(minZ, b.pivot.z);
        maxZ = std::max(maxZ, b.pivot.z);
    }
    if (maxZ < minZ) return out;
    const float height = std::max(maxZ - minZ, 0.01f);

    std::vector<Chain> found;
    // Linear chains.
    for (int i = 0; i < n; ++i) {
        if (!free[i]) continue;
        const int p = bones[i].parent;
        if (free[p] && freeKids[p].size() == 1) continue;  // inside a chain, not its start
        Chain c;
        int at = i;
        c.bones.push_back(static_cast<uint16_t>(at));
        while (freeKids[at].size() == 1 && c.bones.size() <= kMaxChainBones) {
            at = freeKids[at][0];
            c.bones.push_back(static_cast<uint16_t>(at));
        }
        if (c.bones.size() < 2 || c.bones.size() > kMaxChainBones || !freeKids[at].empty()) continue;
        found.push_back(std::move(c));
    }
    // Fans: free leaf siblings on the midline behind their parent.
    for (int p = 0; p < n; ++p) {
        std::vector<int> mid;
        for (int c : allKids[p]) {
            if (!free[c] || !allKids[c].empty()) continue;
            if (std::abs(bones[c].pivot.y - bones[p].pivot.y) > 0.06f * height) continue;
            if (!(bones[c].pivot.x < bones[p].pivot.x)) continue;
            mid.push_back(c);
        }
        if (mid.size() < 3 || mid.size() > kMaxChainBones) continue;
        std::sort(mid.begin(), mid.end(),
                  [&](int a, int b) { return bones[a].pivot.x > bones[b].pivot.x; });
        Chain c;
        c.fan = true;
        for (int b : mid) c.bones.push_back(static_cast<uint16_t>(b));
        found.push_back(std::move(c));
    }

    size_t total = 0;
    for (auto& c : found) {
        float body = 0, hair = 0, cloth = 0, all = 0;
        float lowest = 1e30f;
        for (uint16_t b : c.bones) {
            body += bones[b].body;
            hair += bones[b].hair;
            cloth += bones[b].cloth;
            all += bones[b].total;
            lowest = std::min(lowest, bones[b].pivot.z);
        }
        if (all <= 0.0f) continue;
        const int root = c.bones.front();
        const glm::vec3 r = bones[root].pivot;
        const glm::vec3 d = bones[c.bones.back()].pivot - r;
        float length = 0.0f;
        for (size_t i = 0; i + 1 < c.bones.size(); ++i)
            length += glm::distance(bones[c.bones[i]].pivot, bones[c.bones[i + 1]].pivot);
        int keyAncestor = bones[root].parent;
        while (keyAncestor >= 0 && keyAncestor < n && bones[keyAncestor].keyBoneId < 0)
            keyAncestor = bones[keyAncestor].parent;
        const bool fromWaist = keyAncestor >= 0 && keyAncestor < n && bones[keyAncestor].keyBoneId == 5;

        bool ok = true;
        if (characterModel && cloth >= 0.6f * all && body <= 0.1f * all) {
            c.kind = ChainKind::Cloth;
        } else if (characterModel && underHead[root] && hair >= 0.6f * all) {
            c.kind = ChainKind::Hair;
        } else if (fromWaist && !underHead[root] && body >= 0.6f * all && d.x < 0.0f &&
                   -d.x >= 0.5f * std::abs(d.z) && std::abs(d.y) <= 0.35f * -d.x &&
                   std::abs(r.y) <= 0.1f * height && length >= 0.08f * height &&
                   (c.fan || lowest > minZ + 0.12f * height)) {
            c.kind = ChainKind::Tail;
        } else {
            ok = false;
        }
        if (!ok || (c.fan && c.kind != ChainKind::Tail)) continue;  // a fan is only ever a tail
        if (total + c.bones.size() > kMaxModelBones) continue;
        total += c.bones.size();
        frameChain(c, bones);
        out.push_back(std::move(c));
    }
    return out;
}

/// One bone's swing: x out from the body (tail: up), y to the side; radians.
struct BoneState {
    glm::vec2 swing{0.0f};
    glm::vec2 rate{0.0f};
};

/// One chain's view of its probe's motion, world space.
struct ChainState {
    glm::vec3 lastProbe{0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 accel{0.0f};
    bool primed = false;
};

/// What moves a chain this frame.
struct Input {
    glm::vec3 probeWorld{0.0f};       ///< the probe through the model matrix
    glm::mat3 worldToModel{1.0f};     ///< rotation only
    glm::vec3 windWorld{0.0f};        ///< the air's velocity, yd/s
    float dt = 0.0f;                  ///< seconds since the chain last stepped
    float freqScale = 1.0f;           ///< lengthFrequencyScale of the drawn length
};

inline constexpr float kSubstep = 1.0f / 120.0f;
inline constexpr float kMaxSimulatedDt = 0.05f;
inline constexpr float kResetDt = 0.25f;
inline constexpr float kTeleport = 4.0f;
inline constexpr float kMaxAccel = 40.0f;
inline constexpr float kMaxVelocity = 60.0f;
inline constexpr float kMaxRate = 8.0f;
inline constexpr float kDrag = 1.0f;          ///< 1/s: air drag on the probe
inline constexpr float kVelocitySmooth = 0.05f;
inline constexpr float kAccelSmooth = 0.04f;

inline void resetChain(ChainState& cs, BoneState* bones, size_t count, const glm::vec3& probe) {
    cs.lastProbe = probe;
    cs.velocity = glm::vec3(0.0f);
    cs.accel = glm::vec3(0.0f);
    cs.primed = true;
    for (size_t i = 0; i < count; ++i) bones[i] = BoneState{};
}

inline bool finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

/// The push on the chain in model space (yd/s^2): the probe's pseudo-force and
/// the drag of the air, from the chain state after this frame's motion.
inline glm::vec3 modelPush(const ChainState& cs, const Input& in) {
    const glm::vec3 world = -cs.accel + kDrag * (in.windWorld - cs.velocity);
    return in.worldToModel * world;
}

/// Advance one chain by in.dt. Returns false when it started over (the bones
/// are at rest).
inline bool stepChain(const Chain& chain, const Params& p, ChainState& cs, BoneState* bones,
                      const Input& in) {
    const size_t n = chain.bones.size();
    if (n == 0) return false;
    if (!finite(in.probeWorld)) {
        resetChain(cs, bones, n, glm::vec3(0.0f));
        cs.primed = false;
        return false;
    }
    if (!cs.primed || !(in.dt >= 0.0f) || in.dt > kResetDt ||
        glm::distance(in.probeWorld, cs.lastProbe) > kTeleport) {
        resetChain(cs, bones, n, in.probeWorld);
        return false;
    }
    if (in.dt <= 0.0f) return true;
    const float dt = in.dt;

    // The probe's velocity and acceleration, smoothed over a few frames so a
    // ragged frame time is not read as a shove.
    const glm::vec3 vRaw = (in.probeWorld - cs.lastProbe) / dt;
    cs.lastProbe = in.probeWorld;
    glm::vec3 v = cs.velocity + (vRaw - cs.velocity) * (1.0f - std::exp(-dt / kVelocitySmooth));
    const float vLen = glm::length(v);
    if (vLen > kMaxVelocity) v *= kMaxVelocity / vLen;
    glm::vec3 aRaw = (v - cs.velocity) / dt;
    cs.velocity = v;
    cs.accel += (aRaw - cs.accel) * (1.0f - std::exp(-dt / kAccelSmooth));
    const float aLen = glm::length(cs.accel);
    if (aLen > kMaxAccel) cs.accel *= kMaxAccel / aLen;

    const glm::vec3 f = modelPush(cs, in);
    glm::vec2 push(glm::dot(f, chain.outward) + p.vertical * f.z, glm::dot(f, chain.lateral));
    if (!std::isfinite(push.x) || !std::isfinite(push.y)) {
        resetChain(cs, bones, n, in.probeWorld);
        return false;
    }

    const float share = 1.0f / static_cast<float>(n);
    const float k = p.gainK * share;
    const float outLim = p.outLimit * share, inLim = p.inLimit * share, sideLim = p.sideLimit * share;
    const float simulated = std::min(dt, kMaxSimulatedDt);
    const int steps = std::max(1, static_cast<int>(std::ceil(simulated / kSubstep)));
    const float h = simulated / static_cast<float>(steps);
    for (size_t b = 0; b < n; ++b) {
        const float t = n > 1 ? static_cast<float>(b) / static_cast<float>(n - 1) : 0.0f;
        const float w = 2.0f * 3.14159265f * (p.freqRootHz + (p.freqTipHz - p.freqRootHz) * t) * in.freqScale;
        const glm::vec2 target = push * k;
        BoneState& s = bones[b];
        for (int i = 0; i < steps; ++i) {
            const glm::vec2 acc = w * w * (target - s.swing) - 2.0f * p.damping * w * s.rate;
            s.rate += acc * h;
            s.rate = glm::clamp(s.rate, glm::vec2(-kMaxRate), glm::vec2(kMaxRate));
            s.swing += s.rate * h;
            // The limits: a stop, not a bounce.
            if (s.swing.x > outLim) { s.swing.x = outLim; s.rate.x = std::min(s.rate.x, 0.0f); }
            if (s.swing.x < -inLim) { s.swing.x = -inLim; s.rate.x = std::max(s.rate.x, 0.0f); }
            if (s.swing.y > sideLim) { s.swing.y = sideLim; s.rate.y = std::min(s.rate.y, 0.0f); }
            if (s.swing.y < -sideLim) { s.swing.y = -sideLim; s.rate.y = std::max(s.rate.y, 0.0f); }
        }
        if (!std::isfinite(s.swing.x) || !std::isfinite(s.swing.y)) {
            resetChain(cs, bones, n, in.probeWorld);
            return false;
        }
    }
    return true;
}

/// A bone's extra rotation in model space, scaled by `weight` (the distance
/// fade): the rotation that moves its tip along outward by swing.x and along
/// lateral by swing.y.
inline glm::vec3 rotationVector(const Chain& chain, const BoneState& s, float weight) {
    // Turning about a = d x o moves a point along d toward o.
    return weight * (s.swing.x * glm::cross(chain.restDir, chain.outward) +
                     s.swing.y * glm::cross(chain.restDir, chain.lateral));
}

/// That rotation as a quaternion about an axis given in another frame:
/// `toFrame` maps model-space directions into it (the inverse of the parent
/// bone's linear part).
inline glm::quat rotationInFrame(const glm::vec3& rotVec, const glm::mat3& toFrame) {
    const float angle = glm::length(rotVec);
    if (angle < 1e-6f) return glm::quat(1, 0, 0, 0);
    const glm::vec3 axis = detail::safeNormalize(toFrame * (rotVec / angle), rotVec / angle);
    return glm::angleAxis(angle, axis);
}

/// How much of the effect a unit this far from the camera gets: all of it to
/// 30 yd, fading to none at 45, where its chains stop stepping.
inline constexpr float kFullDistance = 30.0f;
inline constexpr float kMaxDistance = 45.0f;
inline float distanceWeight(float distance) {
    if (distance <= kFullDistance) return 1.0f;
    if (distance >= kMaxDistance) return 0.0f;
    return 1.0f - (distance - kFullDistance) / (kMaxDistance - kFullDistance);
}

/// The wind's gusting, the foliage's own (m2.vert: 0.55 + 0.45 sin(...)),
/// with a faster flutter on top so a cloak in a steady breeze is not still.
inline float gust(const glm::vec2& worldXY, const glm::vec2& windDir, float time, float phase) {
    return 0.55f + 0.45f * std::sin(glm::dot(worldXY, windDir) * 0.11f - time * 0.7f) +
           0.2f * std::sin(time * 2.3f + phase);
}

}  // namespace secondary_motion
}  // namespace rendering
}  // namespace wowee
