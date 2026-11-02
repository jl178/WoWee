#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace wowee::rendering {

/// A sphere that holds a unit and every place its shadow can fall.
///
/// Character animation skips the skeleton of a unit the frame cannot show,
/// and "cannot show" has to include its shadow: the shadow pass draws every
/// unit within a few hundred yards of the player, so a guard standing behind
/// the camera still lays a shadow across the street in front of it, and a
/// frozen skeleton would freeze that shadow mid-stride.
///
/// `feet` and `cullRadius` are the sphere the main pass culls the unit with;
/// `height` is how tall the unit can be. A point `h` above the feet lands on
/// level ground h / -dir.z down the ray, which is a horizontal run of
/// h * tan(zenith) from the feet; at the shadow projection's shallowest sun
/// (ray z clamped to -0.15) that is 6.6 h. So the shadow on the ground lies
/// within cullRadius of the segment from the feet to feet + run, and a wall
/// or a crate catching it nearer lies between the body and that segment. The
/// sphere returned is centred on the segment's midpoint and also reaches the
/// full height above every point of it. The run is capped: a unit on a cliff
/// lays its shadow far below, but by then it is a smudge a frame stale, not a
/// figure.
///
/// A zero direction (shadows off) gives the unit's own sphere back.
struct AnimCullSphere {
    glm::vec3 center;
    float radius;
};

inline AnimCullSphere animCullSphere(const glm::vec3& feet, float cullRadius, float height,
                                     const glm::vec3& shadowRayDir, float maxRun) {
    const float dz = -shadowRayDir.z;
    if (dz <= 1e-4f) return {.center = feet, .radius = cullRadius};
    const glm::vec2 horiz(shadowRayDir.x, shadowRayDir.y);
    const float horizLen = glm::length(horiz);
    float run = 0.0f;
    glm::vec2 runDir(0.0f);
    if (horizLen > 1e-6f) {
        run = std::min(std::max(height, 0.0f) * horizLen / std::max(dz, 0.15f),
                       std::max(maxRun, 0.0f));
        runDir = horiz / horizLen;
    }
    const glm::vec3 mid = feet + glm::vec3(runDir * (run * 0.5f), 0.0f);
    return {.center = mid, .radius = cullRadius + std::max(height, 0.0f) + run * 0.5f};
}

}  // namespace wowee::rendering
