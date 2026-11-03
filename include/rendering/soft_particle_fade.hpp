#pragma once

/// The soft particle fade (docs/enhancements-plan.md E4) as the late
/// particle pass's shaders compute it - soft_particle.glsli and the
/// SOFT_PARTICLES builds of m2_particle_gpu.vert / m2_particle.vert. The C++
/// twin, for test_soft_particles; keep the two in step.

#include <glm/glm.hpp>

#include <algorithm>

namespace wowee::rendering::soft_particle {

/// View-space distance of a depth-buffer value under a perspective
/// projection with depth 0..1 (GLM_FORCE_DEPTH_ZERO_TO_ONE):
/// z_ndc = P[2][2] + P[3][2] / e.
inline float eyeDistance(const glm::mat4& projection, float depth) {
    return projection[3][2] / (depth + projection[2][2]);
}

/// How many yards in front of the scene a quad fades over: about half its
/// size, held to 0.15-1.5; 0 for quads laid flat in the world (ground
/// glows, ripples), which keep a hard test.
inline float fadeRange(float sizeX, float sizeY, bool flatInWorld) {
    if (flatInWorld) return 0.0f;
    return std::clamp(0.5f * std::max(sizeX, sizeY), 0.15f, 1.5f);
}

/// 0..1, how much of the particle shows; negative where the shader discards
/// (the scene is in front of it - the depth test the late pass does itself).
inline float fade(const glm::mat4& projection, float sceneDepth, float fragDepth, float range) {
    if (fragDepth > sceneDepth) return -1.0f;
    if (range <= 0.0f) return 1.0f;
    const float gap = eyeDistance(projection, sceneDepth) - eyeDistance(projection, fragDepth);
    const float f = std::clamp(gap / range, 0.0f, 1.0f);
    return f * f * (3.0f - 2.0f * f);
}

}  // namespace wowee::rendering::soft_particle
