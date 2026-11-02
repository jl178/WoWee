#pragma once

// How the stock 3.3.5a client (build 12340) lights a unit, for the parts that
// are pure arithmetic. character.frag.glsl does the same per pixel; these are
// the reference the tests hold it to. Addresses are Wow.exe VAs; the full
// account is docs/re/characters.md.

#include <algorithm>
#include <glm/glm.hpp>

namespace wowee {
namespace rendering {
namespace unit_light {

/// The unit-only ambient boost (0x783429..0x7834DF): 1 + 2 x
/// AreaTable.AmbientMultiplier of the player's area. Dun Morogh 0.3 gives 1.6,
/// Durotar 0.4 gives 1.8, 387 areas at 0.6 give 2.2.
constexpr float ambientMultiplier(float areaAmbientMultiplier) {
    return 1.0f + 2.0f * areaAmbientMultiplier;
}

/// Which AreaTable row's multiplier applies: a subarea without flag 0x2000
/// uses its parent zone's.
constexpr bool subareaUsesParent(uint32_t parentAreaId, uint32_t areaFlags) {
    return parentAreaId != 0 && (areaFlags & 0x2000u) == 0;
}

/// The boosted outdoor ambient, each channel clamped to 1 (0x7C1730).
inline glm::vec3 boostedAmbient(const glm::vec3& ambient, float multiplier) {
    return glm::min(ambient * multiplier, glm::vec3(1.0f));
}

struct InteriorLight {
    glm::vec3 diffuse;
    glm::vec3 ambient;
};

/// 0x7C1AD0(colour, diffuse, 168, ambient, 96): the colour sampled under a
/// unit indoors, split into a diffuse whose brightest channel is raised to
/// 168/255 (an HSV value scale, which is a uniform RGB scale) and an ambient
/// whose brightest channel is capped at 96/255.
inline InteriorLight interiorSplit(const glm::vec3& c) {
    const float m = std::max({c.r, c.g, c.b});
    constexpr float kDiffuseFloor = 168.0f / 255.0f;
    constexpr float kAmbientCap = 96.0f / 255.0f;
    InteriorLight out{c, c};
    if (m > 0.0f && m < kDiffuseFloor) out.diffuse = c * (kDiffuseFloor / m);
    if (m > kAmbientCap) out.ambient = c * (kAmbientCap / m);
    return out;
}

/// The fixed direction an interior's light travels (0xAEEDF0), world space.
inline glm::vec3 interiorLightDirection() { return glm::vec3(-0.30822f, -0.30822f, -0.9f); }

/// A point light's falloff on a unit: 1 / (0 + 0.7 d + 0.03 d^2), the
/// attenuation every CM2Light gets from its constructor (0x834A40). The M2
/// attenuation start/end tracks are never read.
constexpr float pointAttenuation(float d) {
    return 1.0f / (0.7f * d + 0.03f * d * d);
}

/// The vertex colour a lit unit ends with: saturate(saturate(lit) + emissive),
/// emissive being the world unit ambient while highlighted (0x743C70).
inline glm::vec3 finalLight(const glm::vec3& lit, const glm::vec3& emissive) {
    return glm::clamp(glm::clamp(lit, 0.0f, 1.0f) + emissive, 0.0f, 1.0f);
}

}  // namespace unit_light
}  // namespace rendering
}  // namespace wowee
