#pragma once

#include "rendering/m2_particle_stock.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace wowee::rendering::m2_particle {

/// What the particle pass hands m2_particle_gpu.vert, one record per quad:
/// a particle with a head and a tail (flags 0x20000 and 0x40000) is two.
/// The shader expands each record into the four corners of the stock quad
/// (CParticleEmitter2 0x97BE80) and reads the colour, alpha and size from the
/// emitter's lifetime curves, packed into the frame's curve buffer at
/// curveBase. lookFromPacked is that read in C++; keep the two in step, the
/// tests hold lookFromPacked to the curves.
///
/// `vec` depends on the kind:
/// - billboard: unused;
/// - velocity: the negated velocity, world space;
/// - XY quad: the quad's rotation as a unit quaternion's xyz (w >= 0);
/// - tail: the tail, world space: negated velocity times the tail length.
struct ParticleRecord {
    glm::vec3 position;  // world
    float lifeRatio;
    glm::vec3 vec;
    float sizeX;         // size factor: scale variation, twinkle, inherited scale
    float angle;         // spin, radians
    float sizeY;
    uint32_t curveBase;  // in vec4s
    uint32_t packed;     // see kPacked*
};
static_assert(sizeof(ParticleRecord) == 48);

enum RecordKind : uint32_t {
    kKindBillboard = 0,
    kKindVelocity = 1,
    kKindXYQuad = 2,
    kKindTail = 3,
};

inline constexpr uint32_t kPackedCellMask = 0xFFFFu;
inline constexpr uint32_t kPackedKindShift = 16;
inline constexpr uint32_t kPackedSpin = 1u << 18;
inline constexpr uint32_t kPackedLit = 1u << 19;
inline constexpr uint32_t kPackedUnfogged = 1u << 20;
inline constexpr uint32_t kPackedAlphaShift = 24;

inline uint32_t packRecord(uint32_t cell, RecordKind kind, bool spin, bool lit, bool unfogged,
                           float modelAlpha) {
    const uint32_t a = static_cast<uint32_t>(std::lround(std::clamp(modelAlpha, 0.0f, 1.0f) * 255.0f));
    return (std::min(cell, kPackedCellMask)) | (static_cast<uint32_t>(kind) << kPackedKindShift) |
           (spin ? kPackedSpin : 0u) | (lit ? kPackedLit : 0u) | (unfogged ? kPackedUnfogged : 0u) |
           (a << kPackedAlphaShift);
}

/// One curve in the storage buffer, in vec4s: a header with the three key
/// counts, then kCurveKeys colour keys (r, g, b, time), kCurveKeys alpha keys
/// (alpha, time, -, -) and kCurveKeys scale keys (x, y, time, -). The data
/// has at most seven keys a curve.
inline constexpr uint32_t kCurveKeys = 8;
inline constexpr uint32_t kCurveVec4s = 1 + 3 * kCurveKeys;

/// The key count the shader evaluates with: 0 for no keys (the default),
/// 1 when the first key holds (one key, or no times for the rest), else the
/// shorter list, at most kCurveKeys.
template <class V>
inline uint32_t packedCount(const pipeline::M2FBlock& fb, const std::vector<V>& values) {
    if (values.empty()) return 0;
    if (values.size() == 1 || fb.timestamps.size() < 2) return 1;
    return std::min<uint32_t>(fblockCount(fb, values), kCurveKeys);
}

inline void packCurve(const pipeline::M2ParticleEmitter& em, glm::vec4* out) {
    std::memset(static_cast<void*>(out), 0, sizeof(glm::vec4) * kCurveVec4s);
    const auto& c = em.particleColor;
    const auto& a = em.particleAlpha;
    const auto& s = em.particleScale;
    const uint32_t nc = packedCount(c, c.vec3Values);
    const uint32_t na = packedCount(a, a.floatValues);
    const bool scale2 = !s.vec2Values.empty();
    const uint32_t ns = scale2 ? packedCount(s, s.vec2Values) : packedCount(s, s.floatValues);
    out[0] = glm::vec4(static_cast<float>(nc), static_cast<float>(na), static_cast<float>(ns), 0.0f);
    const auto ts = [](const pipeline::M2FBlock& fb, uint32_t k) {
        return k < fb.timestamps.size() ? fb.timestamps[k] : 0.0f;
    };
    for (uint32_t k = 0; k < nc; ++k) out[1 + k] = glm::vec4(c.vec3Values[k], ts(c, k));
    for (uint32_t k = 0; k < na; ++k) out[1 + kCurveKeys + k] = glm::vec4(a.floatValues[k], ts(a, k), 0, 0);
    for (uint32_t k = 0; k < ns; ++k) {
        const glm::vec2 v = scale2 ? s.vec2Values[k] : glm::vec2(s.floatValues[k]);
        out[1 + 2 * kCurveKeys + k] = glm::vec4(v, ts(s, k), 0.0f);
    }
}

struct Look {
    glm::vec3 color;
    float alpha;
    glm::vec2 scale;
};

namespace detail {
/// fblockSegment over a packed track whose time is component `tc` of each key.
inline Segment packedSegment(const glm::vec4* keys, uint32_t n, int tc, float r) {
    float ts[kCurveKeys];
    for (uint32_t k = 0; k < n && k < kCurveKeys; ++k) ts[k] = keys[k][tc];
    return fblockSegment(ts, n, r);
}
}  // namespace detail

/// m2_particle_gpu.vert's curve read: the stock evaluation of each track
/// from the packed layout at `base`.
inline Look lookFromPacked(const glm::vec4* data, uint32_t base, float r) {
    const glm::vec4 h = data[base];
    const uint32_t nc = static_cast<uint32_t>(h.x), na = static_cast<uint32_t>(h.y),
                   ns = static_cast<uint32_t>(h.z);
    const glm::vec4* ck = data + base + 1;
    const glm::vec4* ak = ck + kCurveKeys;
    const glm::vec4* sk = ak + kCurveKeys;
    Look l{glm::vec3(1.0f), 1.0f, glm::vec2(1.0f)};
    if (nc == 1) l.color = glm::vec3(ck[0]);
    if (nc > 1) {
        const Segment s = detail::packedSegment(ck, nc, 3, r);
        l.color = glm::mix(glm::vec3(ck[s.i0]), glm::vec3(ck[s.i1]), s.f);
    }
    if (na == 1) l.alpha = ak[0].x;
    if (na > 1) {
        const Segment s = detail::packedSegment(ak, na, 1, r);
        l.alpha = ak[s.i0].x + (ak[s.i1].x - ak[s.i0].x) * s.f;
    }
    if (ns == 1) l.scale = glm::vec2(sk[0]);
    if (ns > 1) {
        const Segment s = detail::packedSegment(sk, ns, 2, r);
        l.scale = glm::vec2(sk[s.i0]) + (glm::vec2(sk[s.i1]) - glm::vec2(sk[s.i0])) * s.f;
    }
    return l;
}

/// The curves read straight from the emitter, as the stock evaluator does
/// (0x979E90): what lookFromPacked has to agree with.
inline Look lookFromEmitter(const pipeline::M2ParticleEmitter& em, float r) {
    return {particleColor(em, r), particleAlpha(em, r), particleScale(em, r)};
}

} // namespace wowee::rendering::m2_particle
