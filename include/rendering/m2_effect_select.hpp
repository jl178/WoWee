#pragma once

/// Which vertex and pixel shader the stock client draws an M2 batch with, and
/// which textures, UV sources and texture transforms feed its two stages.
///
/// Reverse engineered from Wow.exe 3.3.5a (12340); docs/re/m2-runtime.md §3
/// "Shader selection" has the addresses. CM2Shared::Initialize (0x837A40)
/// runs three steps over the skin's batches when a model loads:
///
///  1. 0x836980 rewrites every batch's shader id (unless it carries 0x8000)
///     into a runtime id: bits 4-6 the first stage's combiner op, bit 7 that
///     stage's sphere environment map, bits 0-2 and 3 the same for the second
///     stage, 0x4000 "the last stage reads UV set 1". Without the model flag
///     0x08 the op is 0 (opaque) for blend mode 0 and 1 (modulate) otherwise;
///     with it, the ops come from the texture combiner combos the file's id
///     indexes, and the first stage of a blend-0 material is opaque anyway.
///  2. 0x837680 folds two known layer patterns into one two-texture batch and
///     hides the layer it absorbed (shader 0x8000):
///     - an opaque base with an env map at Mod2x or Mod2xNA on stage 2,
///       followed by an alpha layer of the same texture and weight: the base
///       becomes 0x8001 (Combiners_Opaque_Mod2xNA_Alpha);
///     - an opaque one-texture base followed by an additive (4) or Mod2x (6)
///       env-mapped layer with the same weight: the base takes the layer as
///       its second texture, 0x8002 (Opaque_AddAlpha) for 4 or 0x000E
///       (Opaque_Mod2xNA, env) for 6; a further alpha layer of the base's
///       texture turns those into 0x8003 (Opaque_AddAlpha_Alpha) and 0x8001.
///     A batch that shares its material with the batch before it copies that
///     batch's result.
///  3. 0x836C90 / 0x836600 turn the runtime id into a vertex shader
///     (Diffuse_T1, _T2, _Env, _T1_T2, _T1_Env, _Env_T2, _Env_Env) and a pixel
///     shader (Combiners_*). A combination with no shader falls back to id
///     0x11 (Mod / Mod_Mod).
///
/// The draw (0x81F450) binds texture lookup[combo + i] on stage i, with the
/// texture transform lookup[transformCombo + i] as matrix slot i (slot 1 for
/// a one-texture batch with 0x4000, the slot Diffuse_T2 reads); an env-mapped
/// stage gets sphere coordinates instead of a matrix.

#include "pipeline/m2_loader.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace wowee::rendering::m2fx {

/// Where a texture stage's coordinates come from.
enum class UvSource : uint8_t {
    Uv0 = 0,  ///< the vertex's first UV set, through the stage's transform
    Uv1 = 1,  ///< the second UV set, through the stage's transform
    Env = 2,  ///< sphere map of the view-space reflection (no transform)
};

/// The stock pixel shaders (shaders\pixel\*\combiners_*.bls). The values are
/// shared with assets/shaders/m2_combiner.glsli.
enum class Combiner : uint8_t {
    Opaque = 0,
    Mod = 1,
    Decal = 2,
    Add = 3,
    Mod2x = 4,
    Fade = 5,
    Opaque_Opaque = 6,
    Opaque_Mod = 7,
    Opaque_Add = 8,
    Opaque_Mod2x = 9,
    Opaque_Mod2xNA = 10,
    Opaque_AddNA = 11,
    Mod_Opaque = 12,
    Mod_Mod = 13,
    Mod_Add = 14,
    Mod_Mod2x = 15,
    Mod_Mod2xNA = 16,
    Mod_AddNA = 17,
    Add_Mod = 18,
    Mod2x_Mod2x = 19,
    Opaque_Mod2xNA_Alpha = 20,
    Opaque_AddAlpha = 21,
    Opaque_AddAlpha_Alpha = 22,
};

/// One batch as the stock client draws it.
struct BatchEffect {
    /// Shader id 0x8000: folded into an earlier batch, not drawn.
    bool hidden = false;
    /// No pixel shader for the runtime id; drawn with the 0x11 fallback.
    bool fallback = false;
    uint16_t runtimeShader = 0;
    /// Texture stages drawn (1 or 2).
    uint8_t stages = 1;
    Combiner combiner = Combiner::Mod;
    std::array<UvSource, 2> uv{UvSource::Uv0, UvSource::Uv0};
    /// Index into M2Model::textures per stage, -1 for none.
    std::array<int32_t, 2> texture{-1, -1};
    /// Index into M2Model::textureTransforms per stage, -1 for identity.
    std::array<int32_t, 2> transform{-1, -1};
};

namespace detail {

/// A batch's working state through the three steps.
struct Work {
    uint16_t shader = 0;
    uint16_t textureCount = 0;
    std::array<int32_t, 2> texture{-1, -1};
    std::array<int32_t, 2> transform{-1, -1};
};

inline int32_t lookupAt(const std::vector<uint16_t>& table, uint32_t i) {
    return i < table.size() ? static_cast<int32_t>(table[i]) : -1;
}

/// The texture coordinate combo, read unsigned as the client does (-1 is
/// 0xFFFF, so "above 2" is the env map).
inline uint16_t coordCombo(const pipeline::M2Model& m, uint32_t i) {
    return i < m.textureCoordCombos.size() ? static_cast<uint16_t>(m.textureCoordCombos[i]) : 0;
}

inline pipeline::M2Material materialOf(const pipeline::M2Model& m, const pipeline::M2Batch& b) {
    if (b.materialIndex < m.materials.size()) return m.materials[b.materialIndex];
    return pipeline::M2Material{0, 0};
}

inline int32_t weightOf(const pipeline::M2Model& m, const pipeline::M2Batch& b) {
    return lookupAt(m.textureWeightLookup, b.transparencyIndex);
}

}  // namespace detail

/// Step 1 (0x836980): the runtime shader id of one batch.
inline uint16_t runtimeShaderId(const pipeline::M2Model& m, const pipeline::M2Batch& b) {
    if (b.shader & 0x8000) return b.shader;
    const uint16_t blend = detail::materialOf(m, b).blendMode;
    if ((m.globalFlags & 0x08) == 0) {
        const uint16_t tc = detail::coordCombo(m, b.textureUnit);
        uint16_t stage0 = blend != 0 ? 1 : 0;
        if (tc > 2) stage0 |= 8;
        uint16_t id = static_cast<uint16_t>(stage0 << 4);
        if (tc == 1) id |= 0x4000;
        return id;
    }
    std::array<uint16_t, 2> ops{0, 0};
    uint16_t id = 0;
    const uint32_t n = std::min<uint32_t>(b.textureCount, 2);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t ci = static_cast<uint32_t>(b.shader) + i;
        uint16_t op = ci < m.textureCombinerCombos.size() ? m.textureCombinerCombos[ci] : 0;
        if (i == 0 && blend == 0) op = 0;
        const uint16_t tc = detail::coordCombo(m, b.textureUnit + i);
        if (tc > 2) op |= 8;
        if (tc == 1 && i + 1 == b.textureCount) id |= 0x4000;
        ops[i] = op;
    }
    return static_cast<uint16_t>(id | (ops[0] << 4) | ops[1]);
}

/// Step 3 (0x836600): the pixel shader for a runtime id, or false when the
/// stock table has none for it.
inline bool combinerFor(uint32_t stages, uint16_t id, Combiner& out) {
    const uint16_t s0 = (id >> 4) & 7;
    const uint16_t s1 = id & 7;
    if (stages == 1) {
        switch (s0) {
            case 0: out = Combiner::Opaque; break;
            case 2: out = Combiner::Decal; break;
            case 3: out = Combiner::Add; break;
            case 4: out = Combiner::Mod2x; break;
            case 5: out = Combiner::Fade; break;
            default: out = Combiner::Mod; break;
        }
        return true;
    }
    if (s0 == 0) {
        switch (s1) {
            case 0: out = Combiner::Opaque_Opaque; break;
            case 3: out = Combiner::Opaque_Add; break;
            case 4: out = Combiner::Opaque_Mod2x; break;
            case 6: out = Combiner::Opaque_Mod2xNA; break;
            case 7: out = Combiner::Opaque_AddNA; break;
            default: out = Combiner::Opaque_Mod; break;
        }
        return true;
    }
    if (s0 == 1) {
        switch (s1) {
            case 0: out = Combiner::Mod_Opaque; break;
            case 3: out = Combiner::Mod_Add; break;
            case 4: out = Combiner::Mod_Mod2x; break;
            case 6: out = Combiner::Mod_Mod2xNA; break;
            case 7: out = Combiner::Mod_AddNA; break;
            default: out = Combiner::Mod_Mod; break;
        }
        return true;
    }
    if (s0 == 3 && s1 == 1) { out = Combiner::Add_Mod; return true; }
    // Mod2x over Mod reuses Combiners_Mod_Mod2x (the jump at 0x836721).
    if (s0 == 4 && s1 == 1) { out = Combiner::Mod_Mod2x; return true; }
    if (s0 == 4 && s1 == 4) { out = Combiner::Mod2x_Mod2x; return true; }
    return false;
}

/// Step 3: vertex shader stage sources for a runtime id.
inline std::array<UvSource, 2> uvSourcesFor(const pipeline::M2Model& m, const pipeline::M2Batch& b,
                                            uint32_t stages, uint16_t id) {
    const bool env0 = ((id >> 4) & 8) != 0;
    const bool env1 = (id & 8) != 0;
    if (stages == 1) {
        if (env0) return {UvSource::Env, UvSource::Uv0};
        // Diffuse_T2 when the stage's coordinate combo is not 0.
        return {detail::coordCombo(m, b.textureUnit) == 0 ? UvSource::Uv0 : UvSource::Uv1,
                UvSource::Uv0};
    }
    if (!env0) return {UvSource::Uv0, env1 ? UvSource::Env : UvSource::Uv1};
    // Diffuse_Env_T2 reads the first UV set for its second stage
    // (vertex.attrib[6] through c6/c7), whatever the name says.
    return {UvSource::Env, env1 ? UvSource::Env : UvSource::Uv0};
}

/// All three steps over a model's batches, in the skin's order.
inline std::vector<BatchEffect> resolveBatchEffects(const pipeline::M2Model& m) {
    using detail::Work;
    const auto& batches = m.batches;
    std::vector<Work> w(batches.size());

    // Step 1, and each batch's own textures and transforms (0x835E90).
    for (size_t i = 0; i < batches.size(); ++i) {
        const auto& b = batches[i];
        w[i].shader = runtimeShaderId(m, b);
        w[i].textureCount = b.textureCount;
        for (uint32_t s = 0; s < std::min<uint32_t>(b.textureCount, 2); ++s) {
            w[i].texture[s] = detail::lookupAt(m.textureLookup, b.textureIndex + s);
            const int32_t t = detail::lookupAt(m.textureTransformLookup, b.textureAnimIndex + s);
            // The lookup holds -1 (0xFFFF) for "no transform".
            w[i].transform[s] = (t < 0 || t == 0xFFFF) ? -1 : t;
        }
    }

    // Step 2 (0x837680), only when some batch is a second layer.
    bool layered = false;
    for (const auto& b : batches) layered = layered || b.materialLayer != 0;
    if (layered) {
        int stateA = 0, stateB = 0;
        int base = -1;
        int prevMaterial = -1;
        for (size_t i = 0; i < batches.size(); ++i) {
            const auto& b = batches[i];
            if (static_cast<int>(b.materialIndex) == prevMaterial) continue;
            const auto mat = detail::materialOf(m, b);
            const uint16_t stage1Op = w[i].shader & 7;
            if (b.materialLayer == 0) {
                stateA = 0;
                stateB = 0;
                if (w[i].textureCount != 0 && mat.blendMode == 0) w[i].shader &= 0xFF8F;
                base = static_cast<int>(i);
            }
            prevMaterial = static_cast<int>(b.materialIndex);
            if (base < 0) continue;
            const auto& bb = batches[static_cast<size_t>(base)];
            const auto baseMat = detail::materialOf(m, bb);
            const bool sameUnlit = ((baseMat.flags ^ mat.flags) & 1) == 0;
            const bool sameWeight = detail::weightOf(m, bb) == detail::weightOf(m, b);
            const bool alphaLayer = mat.blendMode == 1 || mat.blendMode == 2;

            // Pattern A: env shine at Mod2x(NA) on stage 2, then an alpha layer.
            bool done = false;
            if (stateA == 1) {
                if (alphaLayer && w[i].textureCount == 1 && sameUnlit &&
                    w[i].texture[0] == w[static_cast<size_t>(base)].texture[0] && sameWeight) {
                    w[i].shader = 0x8000;
                    w[static_cast<size_t>(base)].shader = 0x8001;
                    stateA = 3;
                    done = true;
                } else {
                    stateA = 0;
                }
            }
            if (done) continue;
            if (stateA == 0 && mat.blendMode == 0 && w[i].textureCount == 2 &&
                (stage1Op == 4 || stage1Op == 6) && detail::coordCombo(m, b.textureUnit) == 0 &&
                detail::coordCombo(m, b.textureUnit + 1) > 2) {
                stateA = 1;
            }

            // Pattern B: an env layer added or Mod2x'd over a one-texture base.
            if (stateB == 1) {
                if ((mat.blendMode == 4 || mat.blendMode == 6) && w[i].textureCount == 1 &&
                    detail::coordCombo(m, b.textureUnit) >= 3 && sameWeight) {
                    auto& wb = w[static_cast<size_t>(base)];
                    w[i].shader = 0x8000;
                    wb.shader = mat.blendMode == 4 ? 0x8002 : 0x000E;
                    wb.textureCount = 2;
                    wb.texture[1] = w[i].texture[0];
                    wb.transform[1] = w[i].transform[0];
                    stateB = 2;
                    continue;
                }
                stateB = 0;
            } else if (stateB == 2) {
                if (alphaLayer && w[i].textureCount == 1 && sameUnlit &&
                    w[i].texture[0] == w[static_cast<size_t>(base)].texture[0] && sameWeight) {
                    auto& wb = w[static_cast<size_t>(base)];
                    w[i].shader = 0x8000;
                    wb.shader = wb.shader == 0x8002 ? 0x8003 : 0x8001;
                    stateB = 3;
                    continue;
                }
                stateB = 0;
            }
            if (stateB == 0 && mat.blendMode == 0 && w[i].textureCount == 1 &&
                detail::coordCombo(m, b.textureUnit) == 0) {
                stateB = 1;
            }
        }
        // A batch on the same material as the one before it takes its result.
        for (size_t i = 1; i < batches.size(); ++i) {
            if (batches[i].materialIndex == batches[i - 1].materialIndex) w[i] = w[i - 1];
        }
    }

    // Step 3.
    std::vector<BatchEffect> out(batches.size());
    for (size_t i = 0; i < batches.size(); ++i) {
        const auto& b = batches[i];
        BatchEffect& e = out[i];
        e.runtimeShader = w[i].shader;
        e.texture = w[i].texture;
        e.transform = w[i].transform;
        const uint16_t id = w[i].shader;
        if (id == 0x8000) {
            e.hidden = true;
            continue;
        }
        const uint32_t stages = w[i].textureCount >= 2 ? 2 : 1;
        e.stages = static_cast<uint8_t>(stages);
        if (id & 0x8000) {
            switch (id & 0x7FFF) {
                case 1: e.combiner = Combiner::Opaque_Mod2xNA_Alpha; break;
                case 2: e.combiner = Combiner::Opaque_AddAlpha; break;
                case 3: e.combiner = Combiner::Opaque_AddAlpha_Alpha; break;
                default: e.hidden = true; continue;
            }
            // Diffuse_T1_Env, two stages.
            e.stages = 2;
            e.uv = {UvSource::Uv0, UvSource::Env};
            continue;
        }
        if (!combinerFor(stages, id, e.combiner)) {
            e.fallback = true;
            combinerFor(stages, 0x11, e.combiner);
            e.uv = uvSourcesFor(m, b, stages, 0x11);
            continue;
        }
        e.uv = uvSourcesFor(m, b, stages, id);
    }
    // Matrices: stage i's transform goes to matrix slot i (slot 1 for a
    // one-texture batch on UV set 1, which is the slot Diffuse_T2 reads). An
    // env stage takes no matrix, and Diffuse_Env_T2's second stage reads slot
    // 0 - the env stage's, which carries none - so it is untransformed.
    for (auto& e : out) {
        if (e.hidden) continue;
        for (int s = 0; s < 2; ++s)
            if (e.uv[s] == UvSource::Env) e.transform[s] = -1;
        if (e.stages == 2 && e.uv[0] == UvSource::Env && e.uv[1] != UvSource::Env)
            e.transform[1] = -1;
        if (e.stages == 1) {
            e.texture[1] = -1;
            e.transform[1] = -1;
        }
    }
    return out;
}

/// Stock material colour for Mod (5) and Mod2x (6) batches (0x81FE90):
/// diffuse 0 and emissive 1 or 0.5, so the batch is unlit and untinted.
inline float modulateEmissive(uint16_t blendMode) {
    return blendMode == 6 ? 0.5f : 1.0f;
}

}  // namespace wowee::rendering::m2fx
