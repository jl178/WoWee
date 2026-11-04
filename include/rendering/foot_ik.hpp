#pragma once

/// "Foot placement on slopes" (docs/enhancements-plan.md E9).
///
/// The stock client poses a unit's legs from the animation alone, on a flat
/// plane through its position: on a hillside the uphill foot sinks into the
/// ground and the downhill one hangs in the air, and on a stair one foot stands
/// on nothing. With the row on, a standing or walking unit's feet are put on
/// the ground under them: the floor is read under each foot (as the corpse
/// settle reads it, E8), the uphill knee bends (two-bone IK on hip, knee and
/// ankle), the body is lowered when a foot needs to reach further down than
/// the leg allows, and the feet tilt a little with the slope. Every correction
/// is limited and eased, and fades out as the unit runs, and for anything that
/// is not standing on its feet: mounted, swimming, flying, jumping, sitting,
/// dead. Flat ground asks for nothing, and the pose is then the stock pose
/// exactly.
///
/// The legs are found from the skeleton by a rule (findLegs), not a list: two
/// mirrored chains below the Waist key bone that reach the floor. That covers
/// the playable races (plantigrade and the Draenei and Tauren hooves, whose
/// lower leg turns as one piece) and humanoid creatures; anything else - four
/// legs, no Waist, an odd rig - has no rig and is left alone.
///
/// Pure functions, for CharacterRenderer and test_phase2_motion.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace wowee {
namespace rendering {
namespace foot_ik {

// ---- Limits -----------------------------------------------------------------
/// The most a foot is raised, as a fraction of the leg's length: about a
/// stair's rise for a human.
inline constexpr float kMaxRaise = 0.30f;
/// The most a foot reaches down and the body is lowered for it, as a fraction
/// of the leg. More reads as a crouch.
inline constexpr float kMaxLower = 0.22f;
/// A floor this far (fraction of the leg) above or below the unit's own is a
/// ledge or a wall under the foot, not ground to stand on: ignored.
inline constexpr float kLedge = 0.75f;
/// The steepest the feet are tilted with the slope.
inline constexpr float kMaxFootTiltDegrees = 20.0f;
/// Ground steeper than this between the samples is steps, not a slope: the
/// feet stay level on their treads.
inline constexpr float kStepSlopeDegrees = 40.0f;
/// Full correction up to a walk; none from a jog. Walk is 2.5 yd/s, run 7.
inline constexpr float kFullWeightSpeed = 3.0f;
inline constexpr float kZeroWeightSpeed = 5.5f;
/// Time constants of the eases, seconds: the whole correction's weight, the
/// heights each foot reaches for, and the unit's measured speed.
inline constexpr float kWeightSeconds = 0.15f;
inline constexpr float kHeightSeconds = 0.07f;
inline constexpr float kSpeedSeconds = 0.15f;
/// Units further than this from the camera are left alone; at that range a
/// foot is a few pixels.
inline constexpr float kMaxDistance = 30.0f;
/// A foot is read again once it has moved this far (yards) from where it was
/// last read; a still unit costs no floor reads.
inline constexpr float kResampleDistance = 0.12f;

// ---- The rig ----------------------------------------------------------------
/// One bone of a model, for findLegs: its parent, key bone id, rest pivot
/// (model space, +X forward, +Y left, +Z up) and how much vertex weight it
/// carries (0: drives no vertex).
struct BoneInfo {
    int parent = -1;
    int keyBoneId = -1;
    glm::vec3 pivot{0.0f};
    float weight = 0.0f;
};

struct Leg {
    int hip = -1;    ///< the thigh's bone, a child of the Waist
    int knee = -1;   ///< the next bone down
    int end = -1;    ///< the ankle (or the hoof's joint): the bone put on the floor
    /// The three bones' rest pivots (model space).
    glm::vec3 hipPivot{0.0f}, kneePivot{0.0f}, endPivot{0.0f};
    /// Per bone of the model: 1 below the hip (and the hip), 2 below the knee,
    /// 4 below the end. Bones are in parent-first order.
    std::vector<uint8_t> member;
};

struct Rig {
    bool valid = false;
    Leg legs[2];          ///< [0] the left leg (+Y), [1] the right
    float legLength = 0;  ///< hip to knee to end at rest, model units (mean of the two)
};

/// The two legs of a skeleton, or an invalid rig. Rule: the Waist key bone
/// (5); for each child of it to one side of the middle and not above it, whose
/// weighted descendants reach near the lowest point of the body, walk down
/// taking the child that reaches lowest until within a fifth of the Waist's
/// height of that point - that is the end; the chain needs a knee between.
/// Exactly two such chains, one each side, of like length.
inline Rig findLegs(const std::vector<BoneInfo>& bones) {
    Rig rig;
    const int n = static_cast<int>(bones.size());
    if (n < 4) return rig;
    int waist = -1;
    for (int i = 0; i < n; ++i) {
        if (bones[i].keyBoneId == 5) { waist = i; break; }
    }
    if (waist < 0) return rig;
    const float waistZ = bones[waist].pivot.z;
    if (waistZ <= 0.05f) return rig;
    // Parents must precede children (M2 order); a model that breaks it is odd.
    for (int i = 0; i < n; ++i) {
        if (bones[i].parent >= i) return rig;
    }
    // The lowest weighted pivot in each bone's subtree.
    std::vector<float> lowest(n, 1e9f);
    for (int i = n - 1; i >= 0; --i) {
        if (bones[i].weight > 0.0f) lowest[i] = std::min(lowest[i], bones[i].pivot.z);
        if (bones[i].parent >= 0) lowest[bones[i].parent] = std::min(lowest[bones[i].parent], lowest[i]);
    }
    std::vector<std::vector<int>> children(n);
    for (int i = 0; i < n; ++i) {
        if (bones[i].parent >= 0) children[bones[i].parent].push_back(i);
    }
    std::vector<std::vector<int>> chains;
    for (int c : children[waist]) {
        const BoneInfo& b = bones[c];
        if (b.weight <= 0.0f || std::abs(b.pivot.y) < 0.03f * waistZ) continue;
        if (b.pivot.z > waistZ + 0.05f * waistZ) continue;
        const float foot = lowest[c];
        if (foot > 0.35f * waistZ) continue;
        std::vector<int> chain{c};
        int x = c;
        while (bones[x].pivot.z > foot + 0.2f * waistZ) {
            int best = -1;
            for (int k : children[x]) {
                if (lowest[k] > 1e8f) continue;
                if (best < 0 || lowest[k] < lowest[best] ||
                    (lowest[k] == lowest[best] && bones[k].weight > bones[best].weight)) best = k;
            }
            if (best < 0) break;
            x = best;
            chain.push_back(x);
        }
        if (bones[x].pivot.z <= foot + 0.2f * waistZ && chain.size() >= 3) chains.push_back(chain);
    }
    if (chains.size() != 2) return rig;
    const float y0 = bones[chains[0][0]].pivot.y, y1 = bones[chains[1][0]].pivot.y;
    if (y0 * y1 >= 0.0f) return rig;  // both on one side
    if (y0 < 0.0f) std::swap(chains[0], chains[1]);
    float lengths[2];
    for (int l = 0; l < 2; ++l) {
        const auto& ch = chains[l];
        Leg& leg = rig.legs[l];
        leg.hip = ch.front();
        leg.knee = ch[1];
        leg.end = ch.back();
        const glm::vec3 h = bones[leg.hip].pivot, k = bones[leg.knee].pivot, e = bones[leg.end].pivot;
        leg.hipPivot = h;
        leg.kneePivot = k;
        leg.endPivot = e;
        lengths[l] = glm::length(k - h) + glm::length(e - k);
        if (glm::length(k - h) < 1e-3f || glm::length(e - k) < 1e-3f) return Rig{};
        leg.member.assign(n, 0);
        for (int i = 0; i < n; ++i) {
            const int p = bones[i].parent;
            uint8_t m = p >= 0 ? leg.member[p] : 0;
            if (i == leg.hip) m |= 1;
            if (i == leg.knee) m |= 2;
            if (i == leg.end) m |= 4;
            leg.member[i] = m;
        }
    }
    if (std::abs(lengths[0] - lengths[1]) > 0.2f * std::max(lengths[0], lengths[1])) return rig;
    rig.legLength = 0.5f * (lengths[0] + lengths[1]);
    rig.valid = rig.legLength > 0.05f;
    return rig;
}

// ---- Weights and limits -----------------------------------------------------
/// Whether a body animation stands on its feet: everything up to the stock
/// list's no-sheathe emotes but the deaths, rise, jumps and falls, swims,
/// sitting, sleeping and kneeling, mounts, knockdown, birth, drowning and
/// flight. Later ids (the flying and vehicle sets, custom spells) are left out.
inline bool standsOnFeet(uint32_t animId) {
    if (animId > 138) return false;
    switch (animId) {
        case 1: case 6: case 7:                      // Death, Dead, Rise
        case 37: case 38: case 39: case 40:          // jumps, Fall
        case 41: case 42: case 43: case 44: case 45: // swims
        case 71: case 72: case 75:                   // emote sleep, sit, kneel
        case 91: case 94:                            // Mount, MountSpecial
        case 96: case 97: case 98: case 99: case 100: case 101:
        case 102: case 103: case 104:                // sit, sleep, chairs
        case 114: case 115: case 116:                // kneel
        case 121: case 127: case 131: case 132:      // knockdown, birth, drown
        case 135:                                    // fly
            return false;
        default:
            return true;
    }
}

/// The correction's weight at a horizontal speed (yd/s): 1 standing and
/// walking, 0 at a run, smooth between.
inline float speedWeight(float speed) {
    const float t = std::clamp((speed - kFullWeightSpeed) / (kZeroWeightSpeed - kFullWeightSpeed), 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

/// An exponential ease toward `target` with time constant `seconds`; exact for
/// any frame time, never overshooting.
inline float ease(float current, float target, float dt, float seconds) {
    if (dt <= 0.0f) return current;
    const float k = 1.0f - std::exp(-dt / std::max(seconds, 1e-4f));
    return current + (target - current) * k;
}

/// How far (world yards, up positive) a foot is to be moved: the floor under
/// it less the floor under the unit, held to the limits; nothing for a
/// missing floor or a ledge. `legWorld` is the leg's length in yards.
inline float footOffset(std::optional<float> footFloor, std::optional<float> unitFloor, float legWorld) {
    if (!footFloor || !unitFloor || legWorld <= 0.0f) return 0.0f;
    const float d = *footFloor - *unitFloor;
    if (std::abs(d) > kLedge * legWorld) return 0.0f;
    return std::clamp(d, -kMaxLower * legWorld, kMaxRaise * legWorld);
}

/// How far to lower the body for these two foot offsets: enough that the
/// lower foot reaches its floor, within kMaxLower.
inline float bodyDrop(float offLeft, float offRight, float legWorld) {
    const float need = std::max(0.0f, -std::min(offLeft, offRight));
    return std::min(need, kMaxLower * std::max(legWorld, 0.0f));
}

/// The feet's tilt (a ground normal, model space) from the floor's rise per
/// yard along the facing and to the left; level for steps or no data, and
/// held to kMaxFootTiltDegrees.
inline glm::vec3 footNormal(float riseForward, float riseLeft) {
    const float step = std::tan(glm::radians(kStepSlopeDegrees));
    if (!std::isfinite(riseForward) || !std::isfinite(riseLeft)) return {0.0f, 0.0f, 1.0f};
    if (std::abs(riseForward) > step || std::abs(riseLeft) > step) return {0.0f, 0.0f, 1.0f};
    glm::vec3 nrm = glm::normalize(glm::vec3(-riseForward, -riseLeft, 1.0f));
    const float maxTilt = glm::radians(kMaxFootTiltDegrees);
    const float tilt = std::acos(std::clamp(nrm.z, -1.0f, 1.0f));
    if (tilt > maxTilt) {
        const glm::vec2 dir = glm::normalize(glm::vec2(nrm.x, nrm.y));
        nrm = glm::vec3(dir * std::sin(maxTilt), std::cos(maxTilt));
    }
    return nrm;
}

// ---- The solve --------------------------------------------------------------
/// The shortest rotation taking direction a to direction b.
inline glm::quat arc(const glm::vec3& a, const glm::vec3& b) {
    const float la = glm::length(a), lb = glm::length(b);
    if (la < 1e-6f || lb < 1e-6f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::vec3 u = a / la, v = b / lb;
    const float d = glm::dot(u, v);
    if (d > 0.999999f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (d < -0.999999f) {
        glm::vec3 axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), u);
        if (glm::dot(axis, axis) < 1e-6f) axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), u);
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    const glm::vec3 c = glm::cross(u, v);
    return glm::normalize(glm::quat(1.0f + d, c.x, c.y, c.z));
}

struct TwoBone {
    glm::quat upper{1.0f, 0.0f, 0.0f, 0.0f};  ///< about the hip
    glm::quat lower{1.0f, 0.0f, 0.0f, 0.0f};  ///< about the new knee, after `upper`
    glm::vec3 knee{0.0f};                     ///< the knee's new place
    glm::vec3 end{0.0f};                      ///< where the end lands (the target, or as near as the leg reaches)
};

/// Two-bone IK: hip h, knee k and end e as posed; put the end on `target`,
/// keeping the knee bending the way it bends now (toward `forward` if the leg
/// is straight). The reach is held inside the leg's length so the knee never
/// locks straight or folds flat; an unreachable target is met as nearly as it
/// can be along the line to it.
inline TwoBone solveTwoBone(const glm::vec3& h, const glm::vec3& k, const glm::vec3& e,
                            const glm::vec3& target, const glm::vec3& forward) {
    TwoBone out;
    const float l1 = glm::length(k - h), l2 = glm::length(e - k);
    out.knee = k;
    out.end = e;
    if (l1 < 1e-5f || l2 < 1e-5f) return out;
    glm::vec3 d = target - h;
    float dist = glm::length(d);
    if (dist < 1e-5f) return out;
    const glm::vec3 dir = d / dist;
    const float minReach = std::abs(l1 - l2) + 0.02f * (l1 + l2);
    const float maxReach = 0.999f * (l1 + l2);
    dist = std::clamp(dist, minReach, maxReach);
    // The bend's direction: the knee's offset from the hip-end line as posed.
    glm::vec3 pole = (k - h) - dir * glm::dot(k - h, dir);
    if (glm::dot(pole, pole) < 1e-8f * l1 * l1) {
        pole = forward - dir * glm::dot(forward, dir);
        if (glm::dot(pole, pole) < 1e-8f) return out;
    }
    pole = glm::normalize(pole);
    const float along = (l1 * l1 - l2 * l2 + dist * dist) / (2.0f * dist);
    const float up = std::sqrt(std::max(l1 * l1 - along * along, 0.0f));
    out.knee = h + dir * along + pole * up;
    out.end = h + dir * dist;
    out.upper = arc(k - h, out.knee - h);
    const glm::vec3 endAfterUpper = h + out.upper * (e - h);
    out.lower = arc(endAfterUpper - out.knee, out.end - out.knee);
    return out;
}

/// What the main thread decided for a unit, read by the pose: per leg the
/// end's height change (model units, up positive), the body's drop, and the
/// feet's ground normal (model space). Zero everything (or weight 0) is the
/// stock pose.
struct Targets {
    float weight = 0.0f;
    float offset[2] = {0.0f, 0.0f};
    float drop = 0.0f;
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
};

/// What to reach for, from the floor reads (world yards): the floor under
/// the unit, under each foot, and `frontDist` yards ahead of the unit;
/// `leftSpan` is how far the left foot stands to the left of the right one
/// (yards, along the unit's left). `legWorld` the leg's length in yards and
/// `scale` the model's. In model units; the weight is the caller's.
inline Targets aim(std::optional<float> unitFloor, std::optional<float> leftFloor,
                   std::optional<float> rightFloor, std::optional<float> frontFloor, float frontDist,
                   float leftSpan, float legWorld, float scale) {
    Targets t;
    if (scale <= 0.0f || legWorld <= 0.0f) return t;
    const float offL = footOffset(leftFloor, unitFloor, legWorld);
    const float offR = footOffset(rightFloor, unitFloor, legWorld);
    t.offset[0] = offL / scale;
    t.offset[1] = offR / scale;
    t.drop = bodyDrop(offL, offR, legWorld) / scale;
    float riseForward = 0.0f, riseLeft = 0.0f;
    if (unitFloor && frontFloor && frontDist > 1e-3f && std::abs(*frontFloor - *unitFloor) <= kLedge * legWorld) {
        riseForward = (*frontFloor - *unitFloor) / frontDist;
    }
    if (leftFloor && rightFloor && std::abs(leftSpan) > 0.05f && unitFloor &&
        std::abs(*leftFloor - *unitFloor) <= kLedge * legWorld &&
        std::abs(*rightFloor - *unitFloor) <= kLedge * legWorld) {
        riseLeft = (*leftFloor - *rightFloor) / leftSpan;
    }
    t.normal = footNormal(riseForward, riseLeft);
    return t;
}

/// Ease the applied targets toward `want` (offsets, drop and normal; the
/// weight is eased by the caller).
inline void stepTargets(Targets& cur, const Targets& want, float dt) {
    for (int l = 0; l < 2; ++l) cur.offset[l] = ease(cur.offset[l], want.offset[l], dt, kHeightSeconds);
    cur.drop = ease(cur.drop, want.drop, dt, kHeightSeconds);
    const float k = dt > 0.0f ? 1.0f - std::exp(-dt / kHeightSeconds) : 0.0f;
    const glm::vec3 nrm = cur.normal + (want.normal - cur.normal) * k;
    cur.normal = glm::dot(nrm, nrm) > 1e-8f ? glm::normalize(nrm) : glm::vec3(0.0f, 0.0f, 1.0f);
}

/// One unit's state, kept by CharacterRenderer: the targets the pose reads,
/// the ends the last pose put the feet at (before any correction, model
/// space; written by the pose), the floor reads and where they were taken,
/// and the unit's eased speed.
struct State {
    Targets targets;
    glm::vec3 ends[2] = {glm::vec3(0.0f), glm::vec3(0.0f)};
    bool endsValid = false;
    bool read = false;            ///< the reads below are for this unit's place
    glm::vec2 unitReadAt{0.0f};
    glm::vec2 footReadAt[2] = {glm::vec2(0.0f), glm::vec2(0.0f)};
    std::optional<float> unitFloor, frontFloor;
    std::optional<float> footFloor[2];
    glm::vec3 lastPosition{0.0f};
    bool havePosition = false;
    float speed = 0.0f;
};

inline bool isIdentity(const Targets& t) {
    if (t.weight <= 0.0f) return true;
    const float s = t.weight;
    return std::abs(t.offset[0] * s) < 1e-4f && std::abs(t.offset[1] * s) < 1e-4f &&
           std::abs(t.drop * s) < 1e-4f && t.normal.z > 0.99999f;
}

inline glm::mat4 about(const glm::vec3& centre, const glm::quat& q) {
    glm::mat4 m = glm::mat4_cast(q);
    m[3] = glm::vec4(centre - glm::mat3(m) * centre, 1.0f);
    return m;
}

/// Apply the targets to a pose: `bones` are the model-space bone matrices
/// (M_i, taking a rest-space vertex to model space).
/// The whole body drops, then each leg's end is put `offset` above where the
/// animation had it, its knee bending, and its foot keeps the animation's
/// turn with the slope's tilt added. Returns the pre-IK end positions (for
/// the next floor reads) in `endsOut` when given.
inline void apply(std::vector<glm::mat4>& bones, const Rig& rig, const Targets& t,
                  glm::vec3* endsOut = nullptr) {
    if (!rig.valid) return;
    const size_t n = bones.size();
    for (const Leg& leg : rig.legs) {
        if (leg.member.size() != n || static_cast<size_t>(leg.end) >= n) return;
    }
    auto at = [&](int i, const glm::vec3& pivot) { return glm::vec3(bones[i] * glm::vec4(pivot, 1.0f)); };
    if (endsOut) {
        endsOut[0] = at(rig.legs[0].end, rig.legs[0].endPivot);
        endsOut[1] = at(rig.legs[1].end, rig.legs[1].endPivot);
    }
    if (isIdentity(t)) return;
    const float w = std::clamp(t.weight, 0.0f, 1.0f);
    const float drop = std::max(0.0f, t.drop) * w;
    if (drop > 0.0f) {
        for (auto& m : bones) m[3].z -= drop;
    }
    // The foot's slope, weighted.
    glm::quat slope(1.0f, 0.0f, 0.0f, 0.0f);
    if (t.normal.z < 0.99999f) {
        slope = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), arc(glm::vec3(0, 0, 1), t.normal), w);
    }
    for (int l = 0; l < 2; ++l) {
        const Leg& leg = rig.legs[l];
        const glm::vec3 h = at(leg.hip, leg.hipPivot), k = at(leg.knee, leg.kneePivot),
                        e = at(leg.end, leg.endPivot);
        // Where the animation had the end before the drop, moved by the offset.
        const glm::vec3 target = e + glm::vec3(0.0f, 0.0f, drop + t.offset[l] * w);
        const TwoBone s = solveTwoBone(h, k, e, target, glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::mat4 upper = about(h, s.upper);
        const glm::mat4 lower = about(s.knee, s.lower);
        // The foot keeps the turn the animation gave it, plus the slope's.
        const glm::quat footTurn = slope * glm::inverse(s.lower * s.upper);
        const glm::mat4 foot = about(s.end, footTurn);
        for (size_t i = 0; i < n; ++i) {
            const uint8_t m = leg.member[i];
            if (!m) continue;
            glm::mat4 d = upper;
            if (m & 2) d = lower * d;
            if (m & 4) d = foot * d;
            bones[i] = d * bones[i];
        }
    }
}

}  // namespace foot_ik
}  // namespace rendering
}  // namespace wowee
