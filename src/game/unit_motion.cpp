// src/game/unit_motion.cpp
// The stock client's movement of other units. See include/game/unit_motion.hpp
// and docs/re/unit-motion.md for where each rule comes from.
#include "game/unit_motion.hpp"

#include <algorithm>
#include <cmath>

namespace wowee::game::motion {

namespace {

constexpr float kHalfSqrt2 = 0.70710677f;  // 0x9E8D48

float distSq(const glm::vec3& a, const glm::vec3& b) {
    const glm::vec3 d = a - b;
    return glm::dot(d, d);
}

float distSqXY(const glm::vec3& a, const glm::vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

/// The x87 rounding the client uses for milliseconds (fistp, round to nearest).
uint32_t roundMs(float v) {
    if (!(v > 0.0f)) return 0;
    return static_cast<uint32_t>(std::lround(v));
}

/// Catmull-Rom basis rows (0xAC37B8): the weights of p0..p3 at u.
void catmullWeights(float u, float w[4]) {
    const float u2 = u * u, u3 = u2 * u;
    w[0] = -0.5f * u3 + 1.0f * u2 - 0.5f * u;
    w[1] = 1.5f * u3 - 2.5f * u2 + 1.0f;
    w[2] = -1.5f * u3 + 2.0f * u2 + 0.5f * u;
    w[3] = 0.5f * u3 - 0.5f * u2;
}

/// Their derivative (0xAC37F8).
void catmullDerivWeights(float u, float w[4]) {
    const float u2 = u * u;
    w[0] = -1.5f * u2 + 2.0f * u - 0.5f;
    w[1] = 4.5f * u2 - 5.0f * u;
    w[2] = -4.5f * u2 + 4.0f * u + 0.5f;
    w[3] = 1.5f * u2 - 1.0f * u;
}

}  // namespace

float wrapTwoPi(float a) {
    if (!std::isfinite(a)) return 0.0f;
    a = std::fmod(a, kTwoPi);
    if (a < 0.0f) a += kTwoPi;
    return a;
}

// ── Speeds and small rules ────────────────────────────────────────────────

float selectSpeed(const MoveSpeeds& s, uint32_t f) {
    if ((f & (MoveFlags::Directions | MoveFlags::Ascending | MoveFlags::Descending)) == 0) {
        return 0.0f;
    }
    const bool back = (f & MoveFlags::Backward) != 0;
    if (f & MoveFlags::Flying) {
        return (back && s.flightBack <= s.flight) ? s.flightBack : s.flight;
    }
    if (f & MoveFlags::Swimming) {
        return (back && s.swimBack <= s.swim) ? s.swimBack : s.swim;
    }
    if (f & MoveFlags::Walking) {
        return std::min(s.walk, s.run);
    }
    if (back && s.runBack <= s.run) return s.runBack;
    return s.run;
}

float fallDistance(float t, float v0, bool safeFall) {
    const float terminal = safeFall ? kSafeFallTerminalVelocity : kTerminalVelocity;
    v0 = std::min(v0, terminal);
    if (kGravity * t + v0 >= terminal) {
        const float tt = (terminal - v0) / kGravity;
        return (0.5f * kGravity * tt + v0) * tt + (t - tt) * terminal;
    }
    return (0.5f * kGravity * t + v0) * t;
}

uint32_t groundLocomotionAnim(float speed, float walkSpeed, bool backward) {
    if (backward) return 13;          // Walkbackwards
    if (speed > kSprintSpeed) return 143;  // Sprint
    if (speed > walkSpeed + walkSpeed) return 5;  // Run
    return 4;                         // Walk
}

float locomotionPlaybackRate(float unitSpeed, float sequenceMoveSpeed) {
    if (sequenceMoveSpeed == 0.0f || !(unitSpeed > 0.0f)) return 1.0f;
    return unitSpeed / std::abs(sequenceMoveSpeed);
}

float stepBodyFacing(float body, float target, float turnRate, float dt) {
    float diff = std::remainder(target - body, kTwoPi);
    // Beyond a quarter turn the excess is taken at once.
    if (std::abs(diff) > kBodyMaxLag) {
        const float excess = std::copysign(std::abs(diff) - kBodyMaxLag, diff);
        body += excess;
        diff -= excess;
    }
    const float step = std::max(0.0f, dt) * turnRate * kBodyTurnRateFactor;
    body += (std::abs(diff) <= step) ? diff : std::copysign(step, diff);
    return std::remainder(body, kTwoPi);
}

// ── SplineCurve ───────────────────────────────────────────────────────────

void SplineCurve::init(std::vector<glm::vec3> points, bool catmullRom) {
    points_ = std::move(points);
    catmull_ = catmullRom;
    segmentLengths_.clear();
    length_ = 0.0f;
    const size_t n = segmentCount();
    segmentLengths_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const float l = segmentLength(i);
        segmentLengths_.push_back(l);
        length_ += l;
    }
}

glm::vec3 SplineCurve::position(size_t seg, float u) const {
    if (!catmull_) {
        const glm::vec3& a = points_[seg + 1];
        const glm::vec3& b = points_[seg + 2];
        return a + (b - a) * u;
    }
    float w[4];
    catmullWeights(u, w);
    return points_[seg] * w[0] + points_[seg + 1] * w[1] +
           points_[seg + 2] * w[2] + points_[seg + 3] * w[3];
}

float SplineCurve::segmentLength(size_t seg) const {
    if (!catmull_) return glm::length(points_[seg + 2] - points_[seg + 1]);
    // Twenty chords (0x4C3B10, step 0.05 at 0x9F1958).
    float len = 0.0f;
    glm::vec3 prev = position(seg, 0.0f);
    for (int i = 1; i <= 20; ++i) {
        const glm::vec3 p = position(seg, static_cast<float>(i) * 0.05f);
        len += glm::length(p - prev);
        prev = p;
    }
    return len;
}

SplineCurve::Sample SplineCurve::evaluate(float t) const {
    Sample out;
    const size_t n = segmentCount();
    if (n == 0) {
        if (!points_.empty()) out.position = points_.size() > 1 ? points_[1] : points_[0];
        return out;
    }
    t = std::clamp(t, 0.0f, 1.0f);
    // Segment by arc length (0x4C3BD0).
    size_t seg = 0;
    float u = t;
    if (n >= 2) {
        const float target = length_ * t;
        float acc = 0.0f;
        while (true) {
            const float next = acc + segmentLengths_[seg];
            if (next > target || seg + 1 >= n) break;
            acc = next;
            ++seg;
        }
        const float sl = segmentLengths_[seg];
        u = sl > 0.0f ? (target - acc) / sl : 0.0f;
    }
    out.position = position(seg, u);

    // Direction (0x4C43B0): the chord's, or for a curve its derivative when
    // that is within 60 degrees of the chord.
    glm::vec3 chord = points_[seg + 2] - points_[seg + 1];
    const float cl = glm::dot(chord, chord);
    if (cl > 2.3841858e-07f) chord *= 1.0f / std::sqrt(cl);
    out.direction = chord;
    if (catmull_) {
        float w[4];
        catmullDerivWeights(u, w);
        glm::vec3 d = points_[seg] * w[0] + points_[seg + 1] * w[1] +
                      points_[seg + 2] * w[2] + points_[seg + 3] * w[3];
        const float dl = glm::dot(d, d);
        if (dl > 1e-4f) {
            d *= 1.0f / std::sqrt(dl);
            if (glm::dot(d, chord) >= 0.5f) out.direction = d;
        }
    }
    return out;
}

// ── Monster moves ─────────────────────────────────────────────────────────

std::vector<glm::vec3> trimPathToMover(const std::vector<glm::vec3>& P, const glm::vec3& M) {
    const size_t n = P.size();
    if (n == 0) return {};
    if (n == 1) {
        if (distSq(P[0], M) >= kSamePointDistSq) return {M, P[0]};
        return {};
    }
    const size_t segs = n - 1;
    size_t ahead = 0, behind = 0;
    for (size_t i = 0; i < segs; ++i) {
        const glm::vec3 d = P[i + 1] - P[i];
        const float off = glm::dot(d, M);
        const float a = glm::dot(d, P[i]) - off;
        const float b = glm::dot(d, P[i + 1]) - off;
        if (a < 0.0f || b < 0.0f) {
            if (a > 0.0f || b > 0.0f) break;  // the mover is within this one
            ++behind;
        } else {
            ++ahead;
        }
    }
    if (ahead == segs) {
        // Not yet on the path: walk to its start from here.
        if (distSqXY(P[0], M) >= kTrimKeepXYDistSq) {
            std::vector<glm::vec3> out;
            out.reserve(n + 1);
            out.push_back(M);
            out.insert(out.end(), P.begin(), P.end());
            return out;
        }
        return P;
    }
    if (behind == segs) {
        // Past all of it: straight to the end.
        if (distSq(P.back(), M) >= kSamePointDistSq) return {M, P.back()};
        return {};
    }
    for (size_t i = 0; i < segs; ++i) {
        glm::vec3 d = P[i + 1] - P[i];
        const float dl = glm::length(d);
        if (dl > 0.0f) d /= dl;
        const float off = glm::dot(d, M);
        const float a = glm::dot(d, P[i]) - off;
        const float b = glm::dot(d, P[i + 1]) - off;
        if (a <= 0.0f && b >= 0.0f) {
            // The mover's foot on this segment.
            const float f = (a != b) ? a / (a - b) : 0.0f;
            const glm::vec3 q = P[i] + (P[i + 1] - P[i]) * f;
            std::vector<glm::vec3> out;
            if (distSq(P[i + 1], q) >= kSamePointDistSq) {
                out.push_back(q);
                out.insert(out.end(), P.begin() + static_cast<long>(i) + 1, P.end());
                return out;
            }
            if (distSq(P[i + 1], M) >= kSamePointDistSq) {
                out.push_back(M);
                out.insert(out.end(), P.begin() + static_cast<long>(i) + 1, P.end());
                return out;
            }
            if (i + 1 == segs) return {};
            out.assign(P.begin() + static_cast<long>(i) + 1, P.end());
            return out;
        }
        if (a >= 0.0f && b >= 0.0f) {
            std::vector<glm::vec3> out;
            if (distSqXY(P[i], M) >= kTrimKeepXYDistSq) out.push_back(M);
            out.insert(out.end(), P.begin() + static_cast<long>(i), P.end());
            return out;
        }
    }
    return P;
}

namespace {

void setFacingFromSpec(const MonsterMoveSpec& spec, FinalFacing& kind, float& angle,
                       glm::vec3& spot, uint64_t& target) {
    switch (spec.moveType) {
        case 2: kind = FinalFacing::Spot; spot = spec.facingSpot; break;
        case 3: kind = FinalFacing::Target; target = spec.facingTarget; break;
        case 4: kind = FinalFacing::Angle; angle = spec.facingAngle; break;
        default: kind = FinalFacing::None; break;
    }
}

}  // namespace

MonsterMovePlan planMonsterMove(const MonsterMoveSpec& spec, const MoverSnapshot& mover,
                                uint32_t nowMs) {
    MonsterMovePlan plan;
    plan.position = mover.position;

    uint32_t flags = spec.splineFlags;
    uint32_t serverDuration = spec.durationMs;
    std::vector<glm::vec3> pts;  // padded
    glm::vec3 lastPoint = spec.points.empty() ? spec.start : spec.points.back();

    if (spec.moveType == 1) {
        // Stop. Within pathDistTol (a yard, CVar at 0xCA1194) the unit stays
        // where this client has it; further off it slides there (0x73C8E0).
        if (distSq(spec.start, mover.position) <= 1.0f) {
            plan.kind = MonsterMovePlan::Kind::Stop;
            return plan;
        }
        flags = SplineFlags::WalkMode;
        serverDuration = 0;
        lastPoint = spec.start;
        const glm::vec3 back(std::cos(mover.facing), std::sin(mover.facing), 0.0f);
        pts = {mover.position - back, mover.position, spec.start, spec.start};
    } else if ((flags & SplineFlags::Smooth) == 0) {
        // A ground path: the packet's start, its nodes, then trimmed to where
        // this unit is (0x7180C0).
        std::vector<glm::vec3> real;
        real.reserve(spec.points.size() + 1);
        real.push_back(spec.start);
        if (spec.points.size() >= 2) {
            real.insert(real.end(), spec.points.begin(), spec.points.end());
        } else if (spec.points.size() == 1 &&
                   distSq(spec.start, spec.points[0]) > kSamePointDistSq) {
            real.push_back(spec.points[0]);
        }
        std::vector<glm::vec3> trimmed = trimPathToMover(real, mover.position);
        if (trimmed.size() >= 2) {
            pts.reserve(trimmed.size() + 2);
            pts.push_back(trimmed[0] - (trimmed[1] - trimmed[0]));
            pts.insert(pts.end(), trimmed.begin(), trimmed.end());
            pts.push_back(trimmed.back());
        }
    } else {
        // A smooth path starts at this unit, leaving along its facing.
        const glm::vec3 back(std::cos(mover.facing), std::sin(mover.facing), 0.0f);
        pts.push_back(mover.position - back);
        pts.push_back(mover.position);
        for (size_t i = 0; i < spec.points.size(); ++i) {
            if (i == 0 && distSq(spec.points[0], mover.position) < kSamePointDistSq) continue;
            pts.push_back(spec.points[i]);
        }
        if (flags & SplineFlags::Cyclic) {
            if (pts.size() >= 4) {
                const glm::vec3 a = pts[2], b = pts[3];
                pts.push_back(a);
                pts.push_back(b);
            } else {
                pts.push_back(pts.back());
            }
        } else {
            pts.push_back(pts.back());
        }
    }

    // Facing the server asked for.
    FinalFacing facingKind = FinalFacing::None;
    float facingAngle = 0.0f;
    glm::vec3 facingSpot(0.0f);
    uint64_t facingTarget = 0;
    if (spec.moveType != 1) {
        setFacingFromSpec(spec, facingKind, facingAngle, facingSpot, facingTarget);
    }

    auto placeInstead = [&]() {
        plan.kind = MonsterMovePlan::Kind::Place;
        plan.immediateFacing = facingKind;
        plan.immediateAngle = facingAngle;
        plan.immediateSpot = facingSpot;
        plan.immediateTarget = facingTarget;
        // 0x6F11B0 -> 0x6E9470: placed at the end only from 3 yards off.
        plan.position = distSq(lastPoint, mover.position) >= kEndSnapDistSq ? lastPoint
                                                                            : mover.position;
        return plan;
    };

    const size_t n = pts.size();
    if (n <= 3) return placeInstead();

    // The chord length of the real segments.
    float length = 0.0f;
    for (size_t i = 1; i + 2 < n; ++i) {
        length += glm::length(pts[i + 1] - pts[i]);
    }
    if (length <= kMinSplineLength) return placeInstead();

    float speed = (flags & SplineFlags::Smooth)
        ? kSplineSmoothSpeedCap
        : std::max(kSplineGroundSpeedFloor, mover.runSpeed * kSplineRunSpeedFactor);
    if (serverDuration != 0) {
        speed = std::min(speed, length / (static_cast<float>(serverDuration) * 0.001f));
    }
    if (speed <= 9.5367432e-07f) return placeInstead();
    const uint32_t duration = std::max<uint32_t>(1, roundMs(length / speed * 1000.0f));

    // 0x6EB680: a transport enter or exit spline is not run.
    if (flags & (SplineFlags::TransportEnter | SplineFlags::TransportExit)) return placeInstead();

    UnitSpline sp;
    sp.flags = flags & ~(SplineFlags::Done | SplineFlags::NoSpline);
    sp.startMs = nowMs;
    sp.durationMs = duration;
    sp.finalPoint = pts.back();
    sp.finalFacing = facingKind;
    sp.facingAngle = facingAngle;
    sp.facingSpot = facingSpot;
    sp.facingTarget = facingTarget;
    sp.fallStartZ = mover.position.z;
    sp.curve.init(std::move(pts), (flags & SplineFlags::Smooth) != 0);
    if ((flags & SplineFlags::Parabolic) && spec.hasParabolic) {
        sp.verticalAcceleration = spec.verticalAcceleration;
        sp.effectStartMs = spec.parabolicStartMs;
    } else if ((flags & SplineFlags::Animation) && spec.hasAnimation) {
        sp.flags = (sp.flags & ~SplineFlags::AnimationMask) | spec.animationType;
        sp.effectStartMs = spec.animationStartMs;
    }
    plan.kind = MonsterMovePlan::Kind::Spline;
    plan.spline = std::move(sp);
    return plan;
}

std::optional<UnitSpline> splineFromCreate(const CreateSplineSpec& spec, uint32_t nowMs) {
    if (spec.nodes.size() < 4) return std::nullopt;
    UnitSpline sp;
    sp.flags = spec.splineFlags & ~(SplineFlags::Done | SplineFlags::NoSpline);
    sp.startMs = nowMs - spec.timePassedMs;
    sp.durationMs = spec.durationMs;
    sp.durationMod = spec.durationMod > 0.0f ? spec.durationMod : 1.0f;
    sp.durationModNext = spec.durationModNext > 0.0f ? spec.durationModNext : 1.0f;
    sp.verticalAcceleration = spec.verticalAcceleration;
    sp.effectStartMs = spec.effectStartMs;
    sp.finalFacing = spec.finalFacing;
    sp.facingAngle = spec.facingAngle;
    sp.facingSpot = spec.facingSpot;
    sp.facingTarget = spec.facingTarget;
    sp.fallStartZ = spec.nodes[1].z;
    // Effects already past are not replayed.
    sp.effectStarted = spec.timePassedMs > spec.effectStartMs &&
                       (sp.flags & (SplineFlags::Parabolic | SplineFlags::Animation));
    const bool smooth = (sp.flags & SplineFlags::Smooth) != 0;
    sp.finalPoint = (sp.flags & SplineFlags::Cyclic) ? spec.nodes.back() : spec.endPoint;
    sp.curve.init(spec.nodes, smooth);
    if (!sp.curve.valid()) return std::nullopt;
    return sp;
}

float UnitSpline::speed() const {
    if (durationMs == 0) return 0.0f;
    return curve.length() / static_cast<float>(durationMs) * 1000.0f;
}

UnitSpline::Frame UnitSpline::update(uint32_t nowMs, uint32_t moveFlags) {
    Frame f;
    if (!curve.valid()) {
        finished = true;
        f.finished = true;
        f.position = finalPoint;
        return f;
    }
    if (flags & SplineFlags::Frozen) {
        f.position = curve.evaluate(0.0f).position;
        return f;
    }
    int32_t passed = static_cast<int32_t>(nowMs - startMs);
    float effDur = std::floor(static_cast<float>(durationMs) * durationMod + 0.5f);
    float t = 0.0f;
    if (effDur <= 0.0f) {
        finished = true;
        t = 1.0f;
    } else if (passed >= 0) {
        if (static_cast<float>(passed) < effDur) {
            t = static_cast<float>(passed) / effDur;
        } else if ((flags & SplineFlags::Cyclic) == 0) {
            finished = true;
            t = 1.0f;
        } else {
            // A lap is done (0x98CA00): the next starts now, without the
            // way in if the path had one (0x98C940).
            passed -= static_cast<int32_t>(effDur);
            startMs = nowMs - static_cast<uint32_t>(passed);
            if ((flags & SplineFlags::EnterCycle) && curve.points().size() > 4) {
                std::vector<glm::vec3> pts(curve.points().begin() + 1, curve.points().end());
                curve.init(std::move(pts), curve.catmullRom());
                flags &= ~SplineFlags::EnterCycle;
            }
            durationMod = durationModNext;
            durationModNext = 1.0f;
            effDur = std::floor(static_cast<float>(durationMs) * durationMod + 0.5f);
            t = effDur > 0.0f ? std::min(1.0f, static_cast<float>(passed) / effDur) : 1.0f;
        }
    }

    const SplineCurve::Sample s = curve.evaluate(t);
    f.position = s.position;
    if ((flags & (SplineFlags::Falling | SplineFlags::OrientationFixed)) == 0) {
        const float xy = s.direction.x * s.direction.x + s.direction.y * s.direction.y;
        if (xy > kFacingMinXYSq) {
            float facing = std::atan2(s.direction.y, s.direction.x);
            if (flags & SplineFlags::OrientationInversed) facing -= kPi;
            f.facing = wrapTwoPi(facing);
        }
    }
    if (moveFlags & (MoveFlags::Swimming | MoveFlags::Flying)) {
        f.hasPitch = true;
        f.pitch = std::asin(std::clamp(s.direction.z, -1.0f, 1.0f));
    }

    const uint32_t passedU = passed > 0 ? static_cast<uint32_t>(passed) : 0u;
    if (flags & SplineFlags::Animation) {
        if (!effectStarted && effectStartMs < passedU) {
            effectStarted = true;
            f.animationStarted = true;
            f.animationTier = static_cast<uint8_t>(flags & SplineFlags::AnimationMask);
        }
    } else if (flags & SplineFlags::Parabolic) {
        if (!effectStarted && effectStartMs < passedU) {
            effectStarted = true;
            f.jumpStarted = true;
        }
        // 0x987D20: z + a/2 * t * (D - t) from the effect's start.
        if (passedU != 0 && passedU < durationMs && effectStartMs < passedU) {
            const float ts = static_cast<float>(passedU - effectStartMs) * 0.001f;
            const float d = static_cast<float>(durationMs - effectStartMs) * 0.001f;
            f.position.z += 0.5f * verticalAcceleration * ts * (d - ts);
        }
    } else if (flags & SplineFlags::Falling) {
        if (passedU != 0 && passedU < durationMs) {
            const float z = fallStartZ - fallDistance(static_cast<float>(passedU) * 0.001f, 0.0f, false);
            f.position.z = std::max(z, finalPoint.z);
            if (z <= finalPoint.z) {
                f.landed = true;
                finished = true;
            }
        } else if (finished) {
            f.position.z = finalPoint.z;
            f.landed = true;
        }
    }
    f.finished = finished;
    if (finished && (flags & (SplineFlags::Falling | SplineFlags::Parabolic))) f.landed = true;
    return f;
}

// ── Remote movement (CMovement) ──────────────────────────────────────────

void MoveState::applySample(const MovementSample& s) {
    flags = s.flags & ~MoveFlags::SplineEnabled;
    flags2 = s.flags2;
    position = s.position;
    startPosition = s.position;
    facing = startFacing = s.facing;
    pitch = startPitch = s.pitch;
    elapsedMs = 0;
    splineElevation = s.splineElevation;
    if (flags & MoveFlags::Falling) {
        fallTimeMs = s.fallTimeMs;
        jumpVelocity = s.jumpVelocity;
        fallStartZ = s.position.z +
            fallDistance(static_cast<float>(s.fallTimeMs) * 0.001f, s.jumpVelocity,
                         (flags & MoveFlags::FeatherFall) != 0);
        jumpCos = s.jumpCos;
        jumpSin = s.jumpSin;
        jumpXYSpeed = s.jumpXYSpeed;
    } else {
        fallTimeMs = 0;
        jumpVelocity = 0.0f;
        fallStartZ = s.position.z;
    }
}

void MoveState::rebase() {
    startPosition = position;
    startFacing = facing;
    startPitch = pitch;
    elapsedMs = 0;
}

namespace {

struct Directions {
    glm::vec3 dir{0.0f};    // +0x64: the 3D direction of travel
    glm::vec2 hdir{0.0f};   // +0x70: its horizontal part
    float pitchCos = 1.0f;  // +0x78
    float pitchSin = 0.0f;  // +0x7C
};

/// 0x987E30 + 0x987EF0: the direction a state's flags move it in.
Directions travelDirections(const MoveState& st) {
    Directions d;
    const float c = std::cos(st.startFacing), s = std::sin(st.startFacing);
    d.hdir = {c, s};
    if ((st.flags & (MoveFlags::Swimming | MoveFlags::Flying)) &&
        std::abs(st.startPitch) >= 9.5367432e-07f) {
        d.pitchCos = std::cos(st.startPitch);
        d.pitchSin = std::sin(st.startPitch);
        d.dir = {d.pitchCos * c, d.pitchCos * s, d.pitchSin};
    } else {
        d.dir = {c, s, 0.0f};
    }
    const uint32_t f = st.flags;
    const bool fb = (f & (MoveFlags::Forward | MoveFlags::Backward)) != 0;
    const bool strafe = (f & (MoveFlags::StrafeLeft | MoveFlags::StrafeRight)) != 0;
    auto side = [&](glm::vec2 h) {
        // Left is a quarter turn counter-clockwise, right clockwise.
        return (f & MoveFlags::StrafeLeft) ? glm::vec2(-h.y, h.x) : glm::vec2(h.y, -h.x);
    };
    if (!fb) {
        if (strafe) {
            d.hdir = side(d.hdir);
            d.dir = {d.hdir.x, d.hdir.y, 0.0f};
        }
        return d;
    }
    if (strafe) {
        glm::vec2 l = d.hdir;
        glm::vec3 dd = d.dir;
        if (f & MoveFlags::Backward) { l = -l; dd = -dd; }
        const glm::vec2 h = side(d.hdir);
        d.dir = glm::vec3(h.x + dd.x, h.y + dd.y, dd.z) * kHalfSqrt2;
        d.hdir = (h + l) * kHalfSqrt2;
        return d;
    }
    if (f & MoveFlags::Backward) {
        d.dir = -d.dir;
        d.hdir = -d.hdir;
    }
    return d;
}

float turnRateFor(const MoveState& st, const MoveSpeeds& sp) {
    float w = 0.0f;
    if (st.flags & MoveFlags::TurnLeft) w = sp.turnRate;
    else if (st.flags & MoveFlags::TurnRight) w = -sp.turnRate;
    if ((st.flags & MoveFlags::Translating) && !(st.flags2 & MoveFlags2::FullSpeedTurning)) {
        w *= kMovingTurnFactor;
    }
    return w;
}

float pitchRateFor(const MoveState& st, const MoveSpeeds& sp) {
    float w = 0.0f;
    if (st.flags & MoveFlags::PitchUp) w = sp.pitchRate;
    else if (st.flags & MoveFlags::PitchDown) w = -sp.pitchRate;
    if ((st.flags & MoveFlags::Translating) && !(st.flags2 & MoveFlags2::FullSpeedPitching)) {
        w *= kMovingTurnFactor;
    }
    return w;
}

}  // namespace

Displacement displacementAt(const MoveState& st, const MoveSpeeds& sp, uint32_t ms) {
    Displacement out;
    out.facing = st.startFacing;
    out.pitch = st.startPitch;
    if (ms == 0) return out;
    const float t = static_cast<float>(ms) * 0.001f;
    const uint32_t f = st.flags;

    // Facing and pitch (0x987770, 0x9877D0).
    if (f & (MoveFlags::TurnLeft | MoveFlags::TurnRight)) {
        out.facing = wrapTwoPi(st.startFacing + turnRateFor(st, sp) * t);
    }
    if (f & (MoveFlags::PitchUp | MoveFlags::PitchDown)) {
        out.pitch = st.startPitch + pitchRateFor(st, sp) * t;
    }

    if (f & MoveFlags::Falling) {
        // In the air: straight along the jump at its speed, and the fall.
        if (f & MoveFlags::Directions) {
            out.offset.x = st.jumpCos * st.jumpXYSpeed * t;
            out.offset.y = st.jumpSin * st.jumpXYSpeed * t;
        }
        const bool safe = (f & MoveFlags::FeatherFall) != 0;
        const float t0 = static_cast<float>(st.fallTimeMs) * 0.001f;
        const float z = st.fallStartZ - fallDistance(t0 + t, st.jumpVelocity, safe);
        out.offset.z = z - st.startPosition.z;
        return out;
    }

    const float speed = selectSpeed(sp, f);
    uint32_t kind = 0;
    if (f & (MoveFlags::TurnLeft | MoveFlags::TurnRight)) kind |= 2;
    if (f & (MoveFlags::PitchUp | MoveFlags::PitchDown)) {
        if (!(f & (MoveFlags::Ascending | MoveFlags::Descending))) kind |= 8;
    }
    if (f & (MoveFlags::Ascending | MoveFlags::Descending)) kind |= 0x10;
    if (f & (MoveFlags::Forward | MoveFlags::Backward)) kind |= 1;
    if (f & (MoveFlags::StrafeLeft | MoveFlags::StrafeRight)) kind |= 4;

    const Directions d = travelDirections(st);
    switch (kind) {
        case 1: case 4: case 5: case 0xC:   // 0x9876B0
            out.offset = d.dir * (speed * t);
            break;
        case 3: case 6: case 7: case 0xE: case 0x13: case 0x16: case 0x17: {  // 0x987A00
            const float w = turnRateFor(st, sp);
            if (w == 0.0f) { out.offset = d.dir * (speed * t); break; }
            const float r = speed / w;
            const float s = std::sin(w * t) * r;
            const float c = r - std::cos(w * t) * r;
            const float dx = d.hdir.x * s - d.hdir.y * c;
            const float dy = d.hdir.x * c + d.hdir.y * s;
            if (f & MoveFlags::Ascending) {
                out.offset = {dx * kHalfSqrt2, dy * kHalfSqrt2, speed * t * kHalfSqrt2};
            } else if (f & MoveFlags::Descending) {
                out.offset = {dx * kHalfSqrt2, dy * kHalfSqrt2, -speed * t * kHalfSqrt2};
            } else if (f & (MoveFlags::Forward | MoveFlags::Backward)) {
                out.offset = {dx * d.pitchCos, dy * d.pitchCos, d.pitchSin * speed * t};
            } else {
                out.offset = {dx, dy, 0.0f};
            }
            break;
        }
        case 9: case 0xD: {  // 0x987950
            const float wp = pitchRateFor(st, sp);
            if (wp == 0.0f) { out.offset = d.dir * (speed * t); break; }
            const float r = speed / wp;
            const float s = std::sin(wp * t) * r;
            const float c = r - std::cos(wp * t) * r;
            const float h = s * d.pitchCos - c * d.pitchSin;
            out.offset = {h * d.hdir.x, h * d.hdir.y, s * d.pitchSin + c * d.pitchCos};
            break;
        }
        case 0xB: case 0xF: {  // 0x987820
            const float w = turnRateFor(st, sp);
            const float wp = pitchRateFor(st, sp);
            const float v = speed * kHalfSqrt2;
            if (w == 0.0f || wp == 0.0f) { out.offset = d.dir * (speed * t); break; }
            const float r = v / w, rp = v / wp;
            const float c = r - std::cos(w * t) * r;
            const float s = std::sin(w * t) * r;
            out.offset.x = d.hdir.x * s - d.hdir.y * c;
            out.offset.y = c * d.hdir.x + d.hdir.y * s;
            out.offset.z = std::sin(wp * t) * rp * d.pitchSin +
                           (rp - std::cos(wp * t) * rp) * d.pitchCos;
            break;
        }
        case 0x10: case 0x12:
            out.offset = {0.0f, 0.0f, (f & MoveFlags::Ascending) ? speed * t : -speed * t};
            break;
        case 0x11: case 0x14: case 0x15: {  // 0x987700
            const float vz = (f & MoveFlags::Ascending) ? kHalfSqrt2 : -kHalfSqrt2;
            out.offset = {d.hdir.x * kHalfSqrt2 * t * speed, d.hdir.y * kHalfSqrt2 * t * speed,
                          t * vz * speed};
            break;
        }
        default:
            break;
    }
    return out;
}

uint32_t stalledElapsed(uint32_t elapsed, uint32_t since) {
    if (since <= kStallFullSpeedMs) return elapsed;
    const int32_t frozen = std::max<int32_t>(0, static_cast<int32_t>(since) -
                                                 static_cast<int32_t>(kStallFullSpeedMs + kStallRampMs));
    const int32_t ramp = std::min<int32_t>(static_cast<int32_t>(since - kStallFullSpeedMs),
                                           static_cast<int32_t>(kStallRampMs));
    int32_t e = static_cast<int32_t>(elapsed) - frozen;
    if (e < 0) e = 0;
    e -= (ramp * ramp) / 2000;
    return e > 0 ? static_cast<uint32_t>(e) : 0u;
}

int32_t RemoteMover::pushJitter(int32_t dtPacket, int32_t dtLocal) {
    int32_t v = jitterHold_ - dtPacket + dtLocal;
    v = std::clamp(v, -32768, 32767);
    jitter_[jitterIndex_] = static_cast<int16_t>(v);
    jitterIndex_ = (jitterIndex_ + 1) & 31u;
    int32_t m = v;
    for (int16_t j : jitter_) m = std::max<int32_t>(m, j);
    return m;
}

void RemoteMover::place(const glm::vec3& position, float facing) {
    state_.flags &= ~MoveFlags::Moving;
    state_.position = position;
    state_.facing = facing;
    state_.rebase();
}

void RemoteMover::receive(const MovementSample& s, uint32_t now) {
    if (!init_) {
        init_ = true;
        lastPacketTime_ = s.time;
        lastApplyMs_ = now;
        stateClockMs_ = now;
    }
    int32_t dtPacket = static_cast<int32_t>(s.time - lastPacketTime_);
    if (dtPacket < 1) {
        dtPacket = 0;
    } else {
        lastPacketTime_ = s.time;
    }
    const int32_t dtLocal = static_cast<int32_t>(now - lastApplyMs_);
    int32_t delay = dtPacket - dtLocal;
    const int32_t jitter = pushJitter(dtPacket, dtLocal);
    if (!state_.moving() && queue_.empty()) {
        // Setting off from a standstill: hold the move back by the worst
        // lateness seen lately, so the rest of it plays out evenly.
        delay += jitter - jitterHold_;
        delay = std::clamp(delay, kScheduleEarliestMs, kScheduleLatestMs);
        if (static_cast<int32_t>(now + static_cast<uint32_t>(delay) - lastApplyMs_) < 0) {
            delay = static_cast<int32_t>(lastApplyMs_ - now);
        }
        jitterHold_ = jitter;
    }
    delay = std::clamp(delay, kScheduleEarliestMs, kScheduleLatestMs);
    const uint32_t applyAt = now + static_cast<uint32_t>(delay);
    lastApplyMs_ = applyAt;
    if (static_cast<int32_t>(now - applyAt) < 0 || !queue_.empty()) {
        queue_.push_back({s, applyAt});
        return;
    }
    apply(s, applyAt, now);
}

void RemoteMover::apply(const MovementSample& s, uint32_t applyAtMs, uint32_t now) {
    state_.applySample(s);
    lastApplied_ = s;
    ++appliedCount_;
    sincePacketMs_ = 0;
    // The state is as the packet says at its own time; a late one is run on
    // to now (0x6EA550).
    stateClockMs_ = static_cast<int32_t>(now - applyAtMs) > 0 ? applyAtMs : now;
}

void RemoteMover::advanceTo(uint32_t now) {
    const int32_t dt = static_cast<int32_t>(now - stateClockMs_);
    if (dt <= 0) return;
    stateClockMs_ = now;
    if (!state_.moving()) return;
    state_.elapsedMs += static_cast<uint32_t>(dt);
    uint32_t eff = state_.elapsedMs;
    if (state_.translating()) {
        sincePacketMs_ += static_cast<uint32_t>(dt);
        eff = stalledElapsed(state_.elapsedMs, sincePacketMs_);
    }
    const Displacement d = displacementAt(state_, speeds, eff);
    state_.position = state_.startPosition + d.offset;
    state_.facing = d.facing;
    state_.pitch = d.pitch;
}

void RemoteMover::update(uint32_t now) {
    while (!queue_.empty() && static_cast<int32_t>(now - queue_.front().applyAtMs) >= 0) {
        const Pending p = queue_.front();
        queue_.pop_front();
        advanceTo(p.applyAtMs);
        apply(p.sample, p.applyAtMs, now);
        // Run it on to the next one, or to now.
        const uint32_t until = queue_.empty() ? now
            : (static_cast<int32_t>(now - queue_.front().applyAtMs) >= 0 ? queue_.front().applyAtMs : now);
        advanceTo(until);
    }
    advanceTo(now);
}

}  // namespace wowee::game::motion
