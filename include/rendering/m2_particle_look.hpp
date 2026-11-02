#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace wowee::rendering::m2_particle {

/// How a particle looks at a given point of its life: colour, alpha and size,
/// from its emitter's lifetime curves and the model's class.
///
/// The particle pass used to work this out on the CPU for every particle and
/// write nine floats per particle into the vertex buffer. The GPU path writes
/// a 24-byte record (ParticleRecord) instead, and m2_particle_gpu.vert does
/// the curve lookup and the class adjustments. The functions here are both
/// sides of that: lookFromCurve is what the CPU path computes, and
/// lookFromPacked is the shader's arithmetic in C++, reading the curve from
/// the layout packCurve writes into the storage buffer. Keep lookFromPacked
/// and the shader in step; the tests hold lookFromPacked to lookFromCurve.

/// Curve table resolution; the same as M2ModelGPU::ParticleCurve::kSteps.
inline constexpr int kCurveSteps = 64;

/// One curve in the storage buffer, in vec4s: the kSteps + 1 colour+alpha
/// entries, then the kSteps + 1 scales four to a vec4, the last one padded.
inline constexpr uint32_t kCurveVec4s =
    static_cast<uint32_t>(kCurveSteps + 1) + static_cast<uint32_t>(kCurveSteps + 1 + 3) / 4;

/// The model's class and the emitter's blend, as the look depends on them.
/// Packed into the top byte of ParticleRecord::tileFlags.
enum LookFlags : uint32_t {
    kLookPlainDoodad   = 1u << 0,  // not spell effect, firefly or flame fixture
    kLookFlameFixture  = 1u << 1,  // lantern, torch, brazier, kobold flame
    kLookSpellEffect   = 1u << 2,
    kLookFirefly       = 1u << 3,
    kLookAdditiveBlend = 1u << 4,  // emitter blending type 3 or 4
};

inline constexpr uint32_t kTileBits = 24;
inline constexpr uint32_t kTileMask = (1u << kTileBits) - 1u;

inline uint32_t lookFlagsFor(bool spellEffect, bool firefly, bool flameFixture,
                             uint16_t blendType) {
    uint32_t f = 0;
    if (!spellEffect && !firefly && !flameFixture) f |= kLookPlainDoodad;
    if (flameFixture) f |= kLookFlameFixture;
    if (spellEffect) f |= kLookSpellEffect;
    if (firefly) f |= kLookFirefly;
    if (blendType == 3 || blendType == 4) f |= kLookAdditiveBlend;
    return f;
}

/// What one particle hands the vertex shader. Position and life ratio are
/// the particle's; curveBase (in vec4s, into the frame's curve buffer) and
/// the flags in tileFlags' top byte are its emitter's; the low 24 bits are
/// the atlas tile, animation frame already added.
struct ParticleRecord {
    glm::vec3 position;
    float lifeRatio;
    uint32_t curveBase;
    uint32_t tileFlags;
};
static_assert(sizeof(ParticleRecord) == 24);

struct Look {
    glm::vec3 color;
    float alpha;
    float scale;
};

/// The model-class adjustments the particle pass makes to the curve values.
/// See renderM2Particles for why each one is there.
inline Look adjustLook(glm::vec3 color, float alpha, float rawScale, uint32_t flags) {
    if (flags & kLookPlainDoodad) {
        color = glm::mix(color, glm::vec3(1.0f), 0.7f);
        if (rawScale > 2.0f) alpha *= 0.02f;
        if (flags & kLookAdditiveBlend) alpha *= 0.05f;
    }
    if (flags & kLookFlameFixture) {
        color = glm::max(color, glm::vec3(0.50f, 0.26f, 0.09f));
        alpha = std::max(alpha, 0.30f);
    }
    float scale = rawScale;
    if (flags & kLookSpellEffect) {
        scale = std::max(rawScale, 0.15f);
    } else if (!(flags & kLookFirefly)) {
        scale = std::min(rawScale, 1.5f);
        if (flags & kLookFlameFixture) scale = std::max(scale, 0.15f);
    }
    return {color, alpha, scale};
}

/// The CPU path: the curve tables as M2ModelGPU::ParticleCurve holds them.
inline Look lookFromCurve(const glm::vec4* colorAlpha, const float* scale,
                          float lifeRatio, uint32_t flags) {
    const float f = glm::clamp(lifeRatio, 0.0f, 1.0f) * static_cast<float>(kCurveSteps);
    const int k = std::min(static_cast<int>(f), kCurveSteps - 1);
    const float t = f - static_cast<float>(k);
    const glm::vec4 ca = glm::mix(colorAlpha[k], colorAlpha[k + 1], t);
    const float rawScale = glm::mix(scale[k], scale[k + 1], t);
    return adjustLook(glm::vec3(ca), ca.a, rawScale, flags);
}

/// Writes one curve in the storage buffer's layout: kCurveVec4s vec4s.
inline void packCurve(const glm::vec4* colorAlpha, const float* scale, glm::vec4* out) {
    constexpr size_t n = kCurveSteps + 1;
    std::memcpy(static_cast<void*>(out), colorAlpha, n * sizeof(glm::vec4));
    float tail[(n + 3) / 4 * 4] = {};
    std::memcpy(tail, scale, n * sizeof(float));
    std::memcpy(static_cast<void*>(out + n), tail, sizeof(tail));
}

/// m2_particle_gpu.vert's arithmetic: the curve read from the packed layout
/// at `base`, then the same adjustments.
inline Look lookFromPacked(const glm::vec4* data, uint32_t base, float lifeRatio,
                           uint32_t flags) {
    const float f = glm::clamp(lifeRatio, 0.0f, 1.0f) * static_cast<float>(kCurveSteps);
    const int k = std::min(static_cast<int>(f), kCurveSteps - 1);
    const float t = f - static_cast<float>(k);
    const uint32_t uk = static_cast<uint32_t>(k);
    const glm::vec4 ca = glm::mix(data[base + uk], data[base + uk + 1u], t);
    const uint32_t sb = base + static_cast<uint32_t>(kCurveSteps + 1);
    const float s0 = data[sb + uk / 4u][static_cast<int>(uk & 3u)];
    const float s1 = data[sb + (uk + 1u) / 4u][static_cast<int>((uk + 1u) & 3u)];
    const float rawScale = glm::mix(s0, s1, t);
    return adjustLook(glm::vec3(ca), ca.a, rawScale, flags);
}

/// The atlas tile a particle draws: its own tile, advanced by the emitter's
/// animation frame and wrapped, when the emitter is tiled. The CPU path did
/// this in float with a subtract loop; for tiles below totalTiles the result
/// is the same integer.
inline uint32_t particleTile(uint32_t ownTile, bool tiled, uint32_t animFrame,
                             uint32_t totalTiles) {
    if (!tiled || totalTiles == 0) return ownTile;
    uint32_t t = ownTile + animFrame;
    while (t >= totalTiles) t -= totalTiles;
    return t;
}

inline uint32_t packTileFlags(uint32_t tile, uint32_t flags) {
    return (std::min(tile, kTileMask)) | (flags << kTileBits);
}

} // namespace wowee::rendering::m2_particle
