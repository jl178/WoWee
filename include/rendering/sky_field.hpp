#pragma once

/// The sky height field's geometry (sky_height_field.hpp): where it stands
/// and how it projects. Header-only, so the tests and the shader's copy of
/// the constants (ambient_occlusion.frag) can be checked without a device.

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>

namespace wowee {
namespace rendering {
namespace sky_field {

constexpr uint32_t kTexels = 512;       // a side; half a yard a texel
constexpr float kSpan = 256.0f;         // yards a side
constexpr float kAbove = 200.0f;        // depth 0 this far above the middle
constexpr float kDepth = 400.0f;        // yards from depth 0 to depth 1
constexpr float kRecentre = 32.0f;      // yards from the middle before a redraw
constexpr double kRefreshSeconds = 2.0;

/// The middle for a camera: snapped to whole texels, so a redraw puts every
/// texel back on the same ground and nothing shimmers as it moves.
inline glm::vec3 snappedCentre(const glm::vec3& camera) {
    const float texel = kSpan / static_cast<float>(kTexels);
    return glm::vec3(std::floor(camera.x / texel + 0.5f) * texel, std::floor(camera.y / texel + 0.5f) * texel,
                     std::floor(camera.z + 0.5f));
}

/// From straight above: world x and y to clip -1..1 across the span (the
/// image's u along +x, v along +y), depth (centre.z + kAbove - z) / kDepth.
/// Vulkan's clip conventions; the corner (the shader's origin) is
/// centre.xy - kSpan / 2.
/// `fromBelow`: the same square looked at from under it, depth
/// (z - (centre.z + kAbove - kDepth)) / kDepth, so the nearest surface the
/// depth test keeps is the lowest - the underside of what stands there.
inline glm::mat4 projection(const glm::vec3& centre, bool fromBelow = false) {
    glm::mat4 m(0.0f);
    const float s = 2.0f / kSpan;
    m[0][0] = s;
    m[3][0] = -centre.x * s;
    m[1][1] = s;
    m[3][1] = -centre.y * s;
    if (fromBelow) {
        m[2][2] = 1.0f / kDepth;
        m[3][2] = -(centre.z + kAbove - kDepth) / kDepth;
    } else {
        m[2][2] = -1.0f / kDepth;
        m[3][2] = (centre.z + kAbove) / kDepth;
    }
    m[3][3] = 1.0f;
    return m;
}

}  // namespace sky_field
}  // namespace rendering
}  // namespace wowee
