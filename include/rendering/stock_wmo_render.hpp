#pragma once

/// How the stock 3.3.5a client (12340) draws a WMO batch, apart from its light
/// (stock_wmo_light.hpp has that): the blend, the alpha test, the culling, the
/// texture wrap, which effect a material gets and the env-map coordinates
/// (include/pipeline/stock_wmo_liquid.hpp has the MLIQ type). Pure functions; docs/re/wmo-rendering.md has
/// the walk-through and the addresses.
///
/// - The draw loops (0x7ac6a0 no MOCV, 0x7ac9f0 MOCV, 0x7a9380 MOHD 0x2) set,
///   per batch: render state 0x11 (culling) = !(MOMT flags & 0x4), render
///   state 6 (blend) = MOMT blendMode as an EGxBlend - except a trans batch,
///   which is drawn SrcAlphaOpaque (9) then InvSrcAlphaAdd (7) - the effect
///   from MOMT shader, and texture 0 with its wrap from flags 0x40 / 0x80.
/// - The alpha reference goes with the blend (0x873ee0, table 0xad8b7c):
///   0 (off) for Opaque, 224/255 for AlphaKey, 1/255 for Alpha..ModAdd, off
///   for the rest. So a trans batch's two passes test nothing.
/// - The GL blend factors are 0xad8d90 (source) / 0xad8dc0 (destination).

#include <glm/glm.hpp>

#include <cstdint>

namespace wowee::rendering::stock_wmo {

/// EGxBlend, the value MOMT blendMode holds.
enum class GxBlend : uint8_t {
    Opaque = 0,
    AlphaKey = 1,
    Alpha = 2,
    Add = 3,              ///< SrcAlpha, One
    Mod = 4,              ///< DstColor, Zero
    Mod2x = 5,            ///< DstColor, SrcColor
    ModAdd = 6,           ///< DstColor, One
    InvSrcAlphaAdd = 7,   ///< 1-SrcAlpha, One
    InvSrcAlphaOpaque = 8,///< 1-SrcAlpha, Zero
    SrcAlphaOpaque = 9,   ///< SrcAlpha, Zero
    NoAlphaAdd = 10,      ///< One, One
    ConstantAlpha = 11,   ///< ConstantAlpha, 1-ConstantAlpha
};
inline constexpr uint32_t kGxBlendCount = 12;

/// A blend factor, as the GL device's tables at 0xad8d90 / 0xad8dc0 hold them.
enum class BlendFactor : uint8_t {
    Zero, One, SrcColor, SrcAlpha, OneMinusSrcAlpha, DstColor, ConstantAlpha,
    OneMinusConstantAlpha,
};
struct BlendFactors {
    BlendFactor src;
    BlendFactor dst;
    bool enabled;  ///< false: the device turns blending off (Opaque, AlphaKey)
};

/// 0x69cefa: blend 0 and 1 disable GL_BLEND; the rest glBlendFunc(src, dst).
inline constexpr BlendFactors gxBlendFactors(uint32_t blend) {
    using F = BlendFactor;
    switch (blend) {
        case 2: return {F::SrcAlpha, F::OneMinusSrcAlpha, true};
        case 3: return {F::SrcAlpha, F::One, true};
        case 4: return {F::DstColor, F::Zero, true};
        case 5: return {F::DstColor, F::SrcColor, true};
        case 6: return {F::DstColor, F::One, true};
        case 7: return {F::OneMinusSrcAlpha, F::One, true};
        case 8: return {F::OneMinusSrcAlpha, F::Zero, true};
        case 9: return {F::SrcAlpha, F::Zero, true};
        case 10: return {F::One, F::One, true};
        case 11: return {F::ConstantAlpha, F::OneMinusConstantAlpha, true};
        default: return {F::One, F::Zero, false};  // 0, 1 and anything unknown
    }
}

/// 0x873ee0 with table 0xad8b7c: the alpha reference the pixel stage tests
/// against (alpha >= ref passes); 0 means no test.
inline constexpr float gxAlphaRef(uint32_t blend) {
    switch (blend) {
        case 1: return 224.0f / 255.0f;
        case 2: case 3: case 4: case 5: case 6: return 1.0f / 255.0f;
        default: return 0.0f;
    }
}

/// Render state 0x11 per batch: culling on unless F_UNCULLED (MOMT 0x4). The
/// GL device (0x69d1b7) maps 1 to glFrontFace(GL_CCW) with back faces culled.
inline constexpr bool cullsBackFaces(uint32_t momtFlags) { return (momtFlags & 0x4u) == 0; }

/// 0x681450: texture 0 wraps unless F_CLAMP_S (0x40) / F_CLAMP_T (0x80).
struct Wrap { bool s; bool t; };
inline constexpr Wrap textureWrap(uint32_t momtFlags) {
    return {(momtFlags & 0x40u) == 0, (momtFlags & 0x80u) == 0};
}

/// The pixel program a batch runs (table 0xd1c3f0, or 0xd1c3d4 for MOHD 0x2).
enum class Effect : uint8_t { Diffuse, Specular, Metal, Env, Opaque, EnvMetal, Composite, None };

/// 0x7ac7d5: MOMT shader picks the effect; a shader-0 material with blend 0
/// whose texture has no alpha (0x4b54f0) is drawn with MapObjOpaque (4).
/// Shader 6 exists only in the unified table (MapObjUComposite); the classic
/// table's slot 6 is empty, and no classic root in the data uses it.
inline constexpr Effect effectFor(uint32_t shader, uint32_t blend, bool textureHasAlpha,
                                  bool unifiedRoot) {
    if (shader == 0 && blend == 0 && !textureHasAlpha) return Effect::Opaque;
    if (shader <= 5) return static_cast<Effect>(shader);
    if (shader == 6) return unifiedRoot ? Effect::Composite : Effect::None;
    return Effect::None;
}

/// The pixel program's output alpha (pixel/arbfp1/mapobj*.bls): Diffuse is the
/// vertex alpha times the texture's, Composite the vertex alpha times the
/// layers' blended alpha, the rest the vertex alpha alone.
inline constexpr bool alphaFromTexture(Effect e) {
    return e == Effect::Diffuse || e == Effect::Composite;
}

/// MapObjDiffuse_T1_Refl / MapObjUDiffuse_T1_Refl: the env texture's
/// coordinate is reflect(normalize(view-space position), view-space normal).xy,
/// with no scale or bias. Both inputs are in the camera's space.
inline glm::vec2 envCoord(const glm::vec3& viewPos, const glm::vec3& viewNormal) {
    const glm::vec3 p = glm::normalize(viewPos);
    const glm::vec3 n = glm::normalize(viewNormal);
    const glm::vec3 r = p - 2.0f * glm::dot(n, p) * n;
    return {r.x, r.y};
}

/// MapObjEnv: 2 * C * t0 + t0.a * env. MapObjEnvMetal: 2 * C * t0 + t0 * t0.a * env.
inline glm::vec3 envColor(Effect e, const glm::vec3& lit2C, const glm::vec4& t0,
                          const glm::vec3& env) {
    const glm::vec3 base = lit2C * glm::vec3(t0);
    if (e == Effect::Env) return base + t0.a * env;
    if (e == Effect::EnvMetal) return base + glm::vec3(t0) * t0.a * env;
    return base;
}

/// MapObjComposite: lerp(t1, t0, MOCV2 alpha), times the doubled vertex light.
inline glm::vec4 compositeTexel(const glm::vec4& t0, const glm::vec4& t1, float blend) {
    return t1 + (t0 - t1) * blend;
}

}  // namespace wowee::rendering::stock_wmo
