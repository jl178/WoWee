#pragma once

// The stock 3.3.5a (12340) particle and ribbon arithmetic, as pure functions:
// the parts of CParticleEmitter2 and CRibbonEmitter that decide what a
// particle or a trail looks like, written so the renderer, the glue scene and
// the tests share one copy. docs/re/particles.md has the reverse engineering;
// the addresses below are virtual addresses in Wow.exe.

#include "pipeline/m2_loader.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <cstdint>

namespace wowee::rendering::m2_particle {

/// Emitter flags (M2Particle +0x04) as CM2Model's emitter setup turns them
/// into CParticleEmitter2 state (0x833C80..0x833EA5). The runtime bit each
/// one sets is in the comment.
enum : uint32_t {
    kFlagUnlit = 0x1,               // material bit 0 cleared: drawn without lighting
    kFlagSortByDepth = 0x2,         // rt 0x20: drawn back to front within the emitter (0x97E580)
    kFlagVelocityQuad = 0x4,        // rt 0x200000: the head is stretched along the velocity
    kFlagUnfogged = 0x8,            // material bit 1 cleared: no fog
    kFlagModelSpace = 0x10,         // rt 0x200: kept in emitter space, drawn through its current matrix
    kFlagInheritScale = 0x20,       // rt 0x400: sizes times the emitter matrix's scale
    kFlagInheritVelocity = 0x40,    // rt 0x800: the emitter's motion x burst multiplier at birth
    kFlagSphereKillPastCenter = 0x80,  // sphere only, rt 0x1000
    kFlagSphereVelocityUp = 0x100,  // sphere only, rt 0x8000: +Z instead of radial
    kFlagRandomSpinDirection = 0x200,  // rt 0x10000
    kFlagClampTailToAge = 0x400,    // rt 0x20000 (0x978D00)
    kFlagSpawnAlongPath = 0x800,    // rt 0x2000: births spread along the emitter's last move
    kFlagXYQuad = 0x1000,           // rt 0x4000: the quad lies in the particle space's XY plane
    kFlagGroundSnap = 0x2000,       // rt 0x40000 (0x979740): not done here
    kFlagFollowEmitter = 0x4000,    // rt 0x80000: live particles dragged along by the emitter
    kFlagBurstOnEnable = 0x8000,    // `rate` particles at once when enabled, none continuously
    kFlagRandomTile = 0x10000,      // rt 0x100000: a random head cell when there is no head track
    kFlagHead = 0x20000,            // rt 4: draw the head quad
    kFlagTail = 0x40000,            // rt 8: draw the tail quad
    kFlagScaleVaryXY = 0x80000,     // rt 0x800000: x and y size vary independently
};

inline constexpr uint8_t kEmitterPlane = 1;
inline constexpr uint8_t kEmitterSphere = 2;
inline constexpr uint8_t kEmitterSpline = 3;

// ---------------------------------------------------------------------------
// Lifetime curves (FBlocks)
// ---------------------------------------------------------------------------

/// The pair of keys a life ratio falls between and how far it is from the
/// first. 0x9793B0 with the search of 0x979330:
/// - one key: that key;
/// - two keys: the ratio itself, whatever the timestamps say;
/// - three keys: split at the middle key's time;
/// - more: the last key whose time is at or before the ratio.
struct Segment {
    uint32_t i0 = 0;
    uint32_t i1 = 0;
    float f = 0.0f;
};

inline Segment fblockSegment(const float* ts, uint32_t n, float r) {
    if (n <= 1) return {};
    if (n == 2) return {0, 1, r};
    if (n == 3) {
        const float mid = ts[1];
        if (r < mid) return {0, 1, r / mid};
        return {1, 2, (r - mid) / (1.0f - mid)};
    }
    uint32_t i = 0;
    for (uint32_t k = 1; k + 1 < n; ++k) {
        if (ts[k] <= r) i = k;
    }
    const float d = ts[i + 1] - ts[i];
    return {i, i + 1, d != 0.0f ? (r - ts[i]) / d : 0.0f};
}

/// Keys a curve is evaluated with: the shorter of its two lists.
template <class V>
inline uint32_t fblockCount(const pipeline::M2FBlock& fb, const std::vector<V>& values) {
    return static_cast<uint32_t>(std::min(fb.timestamps.size(), values.size()));
}

template <class V>
inline V fblockEval(const pipeline::M2FBlock& fb, const std::vector<V>& values, float r,
                    const V& fallback) {
    if (values.empty()) return fallback;
    // One key, or no timestamps for the rest: the first key (the stock tests
    // the key count, +8 of the block, before anything else).
    if (values.size() == 1 || fb.timestamps.size() < 2) return values[0];
    const uint32_t n = fblockCount(fb, values);
    const Segment s = fblockSegment(fb.timestamps.data(), n, r);
    return values[s.i0] + (values[s.i1] - values[s.i0]) * s.f;
}

inline glm::vec3 particleColor(const pipeline::M2ParticleEmitter& em, float r) {
    return fblockEval(em.particleColor, em.particleColor.vec3Values, r, glm::vec3(1.0f));
}
inline float particleAlpha(const pipeline::M2ParticleEmitter& em, float r) {
    return fblockEval(em.particleAlpha, em.particleAlpha.floatValues, r, 1.0f);
}
inline glm::vec2 particleScale(const pipeline::M2ParticleEmitter& em, float r) {
    if (!em.particleScale.vec2Values.empty())
        return fblockEval(em.particleScale, em.particleScale.vec2Values, r, glm::vec2(1.0f));
    const float x = fblockEval(em.particleScale, em.particleScale.floatValues, r, 1.0f);
    return glm::vec2(x);
}

/// A head or tail cell from its curve, rounded to the nearest (0x979560).
/// Returns -1 when the curve is empty: the caller then falls back to the
/// random cell or 0.
inline int32_t particleCell(const pipeline::M2FBlock& fb, float r) {
    if (fb.floatValues.empty()) return -1;
    const float v = fblockEval(fb, fb.floatValues, r, 0.0f);
    return static_cast<int32_t>(std::lround(v));
}

// ---------------------------------------------------------------------------
// Quads (0x97BE80, CParticleEmitter2's quad builder)
// ---------------------------------------------------------------------------

/// The corner table at 0xB2D5B4 and its texture coordinates at 0xB2D5D4.
/// The scale is a half-size: a corner sits a whole `scale` from the centre,
/// so a particle of scale s is a 2s by 2s quad. Corner 0 is the top left,
/// at texture (0, 0); in strip order the four make two triangles.
inline constexpr std::array<glm::vec2, 4> kCornerOffset = {
    glm::vec2(-1.0f, 1.0f), glm::vec2(-1.0f, -1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(1.0f, -1.0f)};
inline constexpr std::array<glm::vec2, 4> kCornerUV = {
    glm::vec2(0.0f, 0.0f), glm::vec2(0.0f, 1.0f), glm::vec2(1.0f, 0.0f), glm::vec2(1.0f, 1.0f)};

/// A camera-facing corner in the view plane (x right, y up). With spin the
/// corner is turned by the angle, counter-clockwise (0x97C2xx; the sine and
/// cosine come from 0x6F7A60).
inline glm::vec2 billboardCorner(int i, glm::vec2 size, bool spin, float angle) {
    const glm::vec2 o = kCornerOffset[static_cast<size_t>(i)] * size;
    if (!spin) return o;
    const float c = std::cos(angle), s = std::sin(angle);
    return {c * o.x - s * o.y, s * o.x + c * o.y};
}

/// A velocity-aligned corner (flag 0x4) in the view plane. `v` is the
/// negated velocity in view space. The quad's x runs along v's projection on
/// the screen, shortened by how much v points at the camera; y runs across.
inline glm::vec2 velocityCorner(int i, glm::vec2 size, const glm::vec3& v) {
    constexpr float kEps = 2.384185791015625e-07f;  // 0x9EA27C
    const float len2 = v.x * v.x + v.y * v.y;
    const float inv2 = len2 <= kEps ? 0.0f : 1.0f / std::sqrt(len2);
    float sx = size.x;
    if (inv2 > kEps) sx = size.x * (1.0f / std::sqrt(glm::dot(v, v))) / inv2;
    const float a = kCornerOffset[static_cast<size_t>(i)].x * sx;
    const float b = kCornerOffset[static_cast<size_t>(i)].y * size.y;
    const glm::vec2 d(v.x * inv2, v.y * inv2);
    return {a * d.x - b * d.y, b * d.x + a * d.y};
}

/// A tail corner relative to the head, in view space. `t` is the tail in view
/// space: the negated velocity times the tail length. Corners 0 and 1 sit
/// either side of the head, 2 and 3 either side of its end, `size` across.
/// Too short on screen and the stock draws a plain quad at the head instead
/// (returns false).
inline bool tailCorner(int i, glm::vec2 size, const glm::vec3& t, glm::vec3& out) {
    constexpr float kMinLen2 = 0.0007716049440205097f;  // 0xAA2CEC
    const float len2 = t.x * t.x + t.y * t.y;
    if (!(len2 > kMinLen2)) return false;
    const float inv = 1.0f / std::sqrt(len2);
    const glm::vec3 p(-size.y * t.y * inv, size.x * t.x * inv, 0.0f);
    switch (i) {
        case 0: out = p; break;
        case 1: out = -p; break;
        case 2: out = t + p; break;
        default: out = t - p; break;
    }
    return true;
}

/// The texture rectangle of cell `cell`: columns across, rows down. The
/// stock needs both to be powers of two and takes the column with a mask
/// and the row with a shift (0x978C70); a row past the last wraps through
/// the sampler.
inline glm::vec2 cellUV(uint32_t cell, uint32_t rows, uint32_t cols, glm::vec2 cornerUV) {
    cols = std::max(cols, 1u);
    rows = std::max(rows, 1u);
    const float col = static_cast<float>(cell % cols);
    const float row = static_cast<float>(cell / cols);
    return {(col + cornerUV.x) / static_cast<float>(cols), (row + cornerUV.y) / static_cast<float>(rows)};
}

// ---------------------------------------------------------------------------
// Per particle
// ---------------------------------------------------------------------------

/// The 128-entry random table the stock fills once at startup (0x81C0D0,
/// at 0xDCE690), values in [0, 1). Its contents are arbitrary; these come
/// from a fixed generator so a run is repeatable.
inline const std::array<float, 128>& randomTable() {
    static const std::array<float, 128> table = [] {
        std::array<float, 128> t{};
        uint32_t x = 0x2545F491u;
        for (float& v : t) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            v = static_cast<float>(x >> 8) * (1.0f / 16777216.0f);
        }
        return t;
    }();
    return table;
}

/// Twinkle (0x97BE80 head): the particle shows only while a table entry,
/// stepped along at twinkleSpeed per second, is within twinklePercent, and
/// its size is scaled by twinkleScale's range at that entry. `phase` is the
/// particle's own offset into the table (the stock's is its pool address).
struct Twinkle {
    bool visible = true;
    float scale = 1.0f;
};

inline Twinkle twinkle(const pipeline::M2ParticleEmitter& em, uint32_t phase, float age) {
    const float range = em.twinkleScaleMax - em.twinkleScaleMin;
    uint32_t idx = 0;
    if (em.twinklePercent < 1.0f || range != 0.0f) {
        idx = (phase + static_cast<uint32_t>(static_cast<int32_t>(std::lround(em.twinkleSpeed * age)))) & 127u;
    }
    const float rnd = randomTable()[idx];
    Twinkle t;
    t.visible = !(em.twinklePercent < 1.0f && em.twinklePercent < rnd);
    t.scale = rnd * range + em.twinkleScaleMin;
    return t;
}

/// The size factors a particle draws at its birth for scaleVary
/// (0x979E90's tail): one factor for both axes, or one each with flag
/// 0x80000, never below 1e-4. `r1`, `r2` are uniform in [-1, 1].
inline glm::vec2 scaleVariation(const pipeline::M2ParticleEmitter& em, float r1, float r2) {
    constexpr float kMin = 9.999999747378752e-05f;  // 0x9E8CD0
    if (em.flags & kFlagScaleVaryXY) {
        const float x = std::max(r2 * em.scaleVary.x + 1.0f, kMin);
        const float y = std::max(r1 * em.scaleVary.y + 1.0f, kMin);
        return {x, y};
    }
    const float f = std::max(r1 * em.scaleVary.x + 1.0f, kMin);
    return {f, f};
}

/// Whether a particle turns at all: the stock skips the rotation, base angle
/// included, unless the emitter has a spin speed or a spin variation.
inline bool emitterSpins(const pipeline::M2ParticleEmitter& em) {
    return em.spin != 0.0f || em.spinVary != 0.0f;
}

// ---------------------------------------------------------------------------
// Emission
// ---------------------------------------------------------------------------

/// The emitter's local frame is its bone's turned 90 degrees about Z: the
/// stock multiplies a fixed matrix in front of bone * T(position) * world
/// (0x8309C0, data at 0xD411E0). Emitter +X is bone +Y and emitter +Y is
/// bone -X. Column-major, for M * v.
inline glm::mat4 emitterRotation() {
    return glm::mat4(0.0f, 1.0f, 0.0f, 0.0f,
                     -1.0f, 0.0f, 0.0f, 0.0f,
                     0.0f, 0.0f, 1.0f, 0.0f,
                     0.0f, 0.0f, 0.0f, 1.0f);
}

/// Rate thinning with distance (0x97D8C0): full rate within 50 yards, a
/// fiftieth less per yard beyond, never below a quarter. Spell kits and
/// missiles are exempt (rt 0x400000, set by 0x6F7652 and 0x701E88).
inline float distanceRateFactor(float distance) {
    const float f = 1.0f - (distance - 50.0f) * 0.02f;
    return std::clamp(f, 0.25f, 1.0f);
}

/// The particles an emitter keeps room for (0x97EDF0): its peak rate times
/// its longest life, plus 15 percent. The pool only grows; a birth with no
/// free slot is skipped and its count stays owed.
inline uint32_t poolSize(float rate, float rateVary, float life, float lifeVary) {
    const float n = (rateVary + rate) * (lifeVary + life) * 1.15f;
    if (!(n > 0.0f)) return 0;
    return static_cast<uint32_t>(std::nearbyint(std::min(n, 1.0e6f)));
}

/// How CParticleEmitter2 splits a long step (0x97ACB0): steps of a tenth of
/// a second, no more of them than the lifespan holds, then what is left of
/// the whole tenths. The count is `nearbyint(min(floor(dt*10), floor(life*10))
/// - 0.5)` under the FPU's round-to-nearest-even, which drops a tenth for
/// some counts; that is stock and kept.
struct Substeps {
    int whole = 0;        // steps of 0.1 s
    float last = 0.0f;    // the final step
};

inline Substeps substeps(float dt, float lifespan) {
    Substeps s;
    if (!(dt > 0.0f)) return s;
    if (!(dt > 0.1f)) {
        s.last = dt;
        return s;
    }
    const float whole = std::floor(dt * 10.0f);
    const float cap = std::min(whole, std::floor(lifespan * 10.0f));
    s.whole = std::max(0, static_cast<int>(std::nearbyint(cap - 0.5f)));
    s.last = dt - whole * 0.1f;
    return s;
}

/// The direction a plane emitter sends a particle, in emitter space, before
/// the speed (0x9815C0): tilted from +Z by the vertical range, turned by the
/// horizontal range. `r1`, `r2` are uniform in [-1, 1].
inline glm::vec3 planeDirection(float vRange, float hRange, float r1, float r2) {
    const float polar = r1 * vRange;
    const float azimuth = r2 * hRange;
    const float sp = std::sin(polar);
    return {std::cos(azimuth) * sp, std::sin(azimuth) * sp, std::cos(polar)};
}

/// A sphere emitter's birth point direction (0x981950): elevation from the
/// XY plane within the vertical range, azimuth within the horizontal range.
inline glm::vec3 sphereDirection(float vRange, float hRange, float r1, float r2) {
    const float elevation = r1 * vRange;
    const float azimuth = r2 * hRange;
    const float ce = std::cos(elevation);
    return {std::cos(azimuth) * ce, std::sin(azimuth) * ce, std::sin(elevation)};
}

// ---------------------------------------------------------------------------
// Ribbons (CRibbonEmitter)
// ---------------------------------------------------------------------------

/// A ribbon edge between the previous and the current frame (0x97FEF0):
/// each end of the edge is a curve from the previous end to the current one,
/// leaving along the previous frame's Z axis and arriving along the current
/// one's, scaled by how far the emitter moved. For an emitter that does not
/// turn it is the straight line between the two.
inline glm::vec3 ribbonEdgePoint(const glm::vec3& prevEnd, const glm::vec3& curEnd,
                                 const glm::vec3& prevZd, const glm::vec3& curZd, float t) {
    const float u = 1.0f - t;
    return (prevEnd + prevZd * t) * u + (curEnd - curZd * u) * t;
}

/// How far a ribbon edge has risen by age `age` (0x980356): each step adds
/// (2 age + dt) gravity dt, so an edge sits gravity * age^2 above where it
/// was laid. Positive gravity lifts.
inline float ribbonGravityStep(float age, float dt, float gravity) {
    return (age * 2.0f + dt) * gravity * dt;
}

} // namespace wowee::rendering::m2_particle
