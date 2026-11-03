#pragma once

/// The stock 3.3.5a (12340) liquid surface, the parts that are plain maths.
///
/// Read out of Wow.exe; see docs/re/world-lighting.md (Water) for the whole
/// path. In short, the default material for water and ocean (LiquidMaterial 1,
/// psLiquidWater, built at 0x8a3f70 and drawn by 0x8a5590) is
///
///   colour = clamp(ambient + diffuse * max(N.L, 0), 0, 1) * depthRamp.rgb
///            + liquidTexture.rgb
///            + (specular + 0.25) * liquidTexture.a
///   alpha  = depthRamp.a
///
/// where depthRamp is an 8x64 texture the client rebuilds from the light
/// (0x8a2bf0 rivers and oceans, 0x8a2ac0 WMO water) and liquidTexture is one
/// frame of LiquidType's texture sequence (lake_a.%d.blp and the rest).
/// The specular colour is the lighting object's local-light term (0x834f60);
/// the sun adds nothing to it, so outdoors only the 0.25 shimmer remains.

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace wowee::rendering::stock_liquid {

/// 0x8a1d60: which frame of an N-frame sequence shows at `timeMs`. The
/// sequence runs once every `periodMs` (LiquidType Int[1] - 1250 for every
/// water and ocean; the magma material asks for 1250 itself), and the frame is
/// round(N * phase - 0.5), i.e. the floor.
inline size_t frameIndex(uint64_t timeMs, uint32_t periodMs, size_t frameCount) {
    if (frameCount == 0) return 0;
    if (periodMs == 0) periodMs = 1;
    const double phase = static_cast<double>(timeMs % periodMs) / static_cast<double>(periodMs);
    const auto frame = static_cast<size_t>(std::floor(static_cast<double>(frameCount) * phase));
    return frame < frameCount ? frame : frameCount - 1;
}

/// The period the client uses for a liquid's texture sequence: LiquidType
/// Int[1] (field 42) for the water material, 1250 ms when that is absent or
/// for magma and slime (0x8a6090 passes 0x4e2).
inline uint32_t framePeriodMs(uint32_t dbcInt1, bool magmaMaterial) {
    if (magmaMaterial || dbcInt1 == 0) return 1250;
    return dbcInt1;
}

/// 0x8a34b0: the magma texture's scroll, one axis. LiquidType Float[0] / [1]
/// are repeats per second; the offset is the fraction of the period
/// round(1000 / speed) ms that has gone by. 0 is still.
inline float scrollOffset(uint64_t timeMs, float repeatsPerSecond) {
    if (repeatsPerSecond == 0.0f) return 0.0f;
    const auto periodMs = static_cast<uint64_t>(std::llround(1000.0 / std::fabs(repeatsPerSecond)));
    if (periodMs == 0) return 0.0f;
    const float f = static_cast<float>(timeMs % periodMs) / static_cast<float>(periodMs);
    return repeatsPerSecond < 0.0f ? -f : f;
}

/// vsLiquidWater: the light on the surface. The vertex colour register clamps
/// to 0..1, so the sum saturates per channel before it multiplies the ramp.
inline glm::vec3 surfaceLight(const glm::vec3& ambient, const glm::vec3& diffuse, float nDotL) {
    return glm::clamp(ambient + diffuse * std::max(nDotL, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f));
}

/// One row of the depth ramp (0x8a2bf0): close colour and shallow alpha at
/// row 0, stepping by (far - close) / 64 per row - so row 63 stops one step
/// short of the far end. The ocean's last row is further darkened to 0.9 of
/// its HSV value (constant 0xa02a34). `row` is 0..63.
inline glm::vec4 depthRampRow(const glm::vec3& closeColor, const glm::vec3& farColor,
                              float shallowAlpha, float deepAlpha, int row, bool ocean) {
    const float t = static_cast<float>(row) / 64.0f;
    glm::vec4 c(glm::mix(closeColor, farColor, t), shallowAlpha + (deepAlpha - shallowAlpha) * t);
    if (ocean && row == 63) c = glm::vec4(glm::vec3(c) * 0.9f, c.a);
    return c;
}

/// The depth byte's divisor (0x79b870): LiquidType Int[0] 0 picks the river
/// table 0xcdf7d0, 1 the ocean table 0xcdfbd0 (ocean, WMO water, WMO ocean).
inline float depthDivisor(uint32_t dbcInt0) { return dbcInt0 == 0 ? 42.0f : 255.0f; }

/// The ramp coordinate (attrib 7's v) of a vertex's depth byte. The river
/// table is built by 0x79e3c0 as x = d / 9, x <= 14/3 ? x * 3/14 : 1, which
/// is min(d / 42, 1); the ocean table is d / 255.
inline float rampCoordinate(uint8_t depthByte, float divisor) {
    return std::min(static_cast<float>(depthByte) / divisor, 1.0f);
}

/// Yards to texture repeats before the texture matrix: ADT liquid 0.06
/// (0xa40410, a repeat every 16.67 yards), WMO MLIQ 0.24 (0xa3fab4, one a
/// 25/6-yard cell). A vertex uv in the data (magma) is its u16 x 3/256.
inline constexpr float kTerrainUVPerYard = 0.06f;
inline constexpr float kWmoUVPerYard = 0.24f;
inline constexpr float kVertexUVScale = 3.0f / 256.0f;

/// The stage 1 rotation as the client builds it (0x8a5590): LiquidType
/// Float[1] times 57.29578 (0x9ed910) handed to cos and sin as radians - the
/// degrees-to-radians factor upside down. Only WMO Ocean sets Float[1] (180),
/// which turns its texture about 145.14 degrees rather than 180.
inline float textureRotationRadians(float dbcFloat1) { return dbcFloat1 * 57.29578f; }

/// psLiquidWater's shimmer: (specular + 0.25) * texture alpha, added to rgb.
inline glm::vec3 shimmer(const glm::vec3& specular, float textureAlpha) {
    return (specular + glm::vec3(0.25f)) * textureAlpha;
}

}  // namespace wowee::rendering::stock_liquid
