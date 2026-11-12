#pragma once

// Sunlight through leaves ("Sun through leaves (enhanced)", the leafLight
// row; docs/re/enhanced-lighting.md section 9c). The per-frame half: where the
// sun is in the sky and whether it is day, for m2.frag's foliage and the
// grass blades (grass.frag).
//
// The world's directional light is not the sun. The stock light direction
// stays 20-37 degrees up all night (LightingManager::stockLightDirection),
// so it can say neither where the low sun stands nor that the sun is down;
// the sun sprite's direction can. It goes to the shaders packed as signed
// bytes in GPUPerFrameData::localLightMeta.w: xyz the direction toward the
// sprite, w the day weight below.

#include <glm/geometric.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>

namespace wowee {
namespace rendering {
namespace leaf_light {

/// 0 with the sun at or below the horizon, 1 from 9 degrees up (z 0.15),
/// smooth between: the same ramp as the HDR scene's day sun
/// (enhanced_tonemap::sunLightScale).
inline float dayWeight(float sunZ) {
    float t = sunZ / 0.15f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

/// `toSun` (need not be unit length) and its day weight, as the shaders'
/// unpackSnorm4x8 reads them back.
inline uint32_t packSun(glm::vec3 toSun) {
    const float len = glm::length(toSun);
    const glm::vec3 d = len > 1e-6f ? toSun / len : glm::vec3(0.0f, 0.0f, 1.0f);
    return glm::packSnorm4x8(glm::vec4(d, dayWeight(d.z)));
}

}  // namespace leaf_light
}  // namespace rendering
}  // namespace wowee
