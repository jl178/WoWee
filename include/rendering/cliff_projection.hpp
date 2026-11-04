#pragma once

/// Unstretched cliffs (E16, the "Unstretched cliffs" enhancement row).
///
/// The stock ground texture coordinates run straight down the world z axis
/// (terrain_mesh.cpp: world -y and -x at eight repeats a chunk), so a slope of
/// angle a stretches every layer by 1 / cos a along its fall line - twice its
/// length at 60 degrees. terrain_cliff.frag reads each layer from the side as
/// well (along world x, and along world y, at the same scale) and blends the
/// three reads by the slope. These are its weights, kept here so the rule can
/// be tested; the shader (cliffSetup in terrain.frag) has the same numbers.
///
/// The rule: ground whose normal is within kFlatDeg of straight up takes the
/// top-down read with weight exactly 1 and the side reads with 0 - the shader
/// then takes the stock branch, so that ground is the stock pixel. Between
/// kFlatDeg and kSteepDeg the side reads take over on a smoothstep; past
/// kSteepDeg the top read is gone. The side reads split between themselves by
/// the eighth power of the normal's x and y, so a face looking along x takes
/// the x projection and corners blend over a narrow band. A weight under kSkip
/// is folded into the others, so most steep pixels read a layer once or
/// twice rather than three times (the cost, enhancements-plan.md 3.13).

#include <algorithm>
#include <cmath>

namespace wowee {
namespace rendering {
namespace cliff_projection {

inline constexpr float kFlatNormalZ = 0.707f;   ///< cos 45 degrees
inline constexpr float kSteepNormalZ = 0.47f;   ///< cos 62 degrees
/// A weight smaller than this is folded into the others and not read.
inline constexpr float kSkip = 0.12f;

struct Weights {
    float sideX = 0.0f;  ///< the read projected along world x (plane y-z)
    float sideY = 0.0f;  ///< along world y (plane x-z)
    float top = 1.0f;    ///< the stock top-down read
};

/// The weights for a unit normal, as cliffSetup computes them.
inline Weights weightsFor(float nx, float ny, float nz) {
    Weights w;
    const float t = std::clamp((std::fabs(nz) - kSteepNormalZ) / (kFlatNormalZ - kSteepNormalZ), 0.0f, 1.0f);
    const float steep = 1.0f - t * t * (3.0f - 2.0f * t);
    if (steep <= 0.0f) return w;
    float sx = std::pow(std::fabs(nx), 8.0f);
    float sy = std::pow(std::fabs(ny), 8.0f);
    const float sum = std::max(sx + sy, 1e-6f);
    sx /= sum;
    sy /= sum;
    w.sideX = sx * steep;
    w.sideY = sy * steep;
    w.top = 1.0f - steep;
    if (w.sideX < kSkip) { w.sideY += w.sideX; w.sideX = 0.0f; }
    if (w.sideY < kSkip) { w.sideX += w.sideY; w.sideY = 0.0f; }
    if (w.top < kSkip) {
        if (w.sideX >= w.sideY) w.sideX += w.top; else w.sideY += w.top;
        w.top = 0.0f;
    }
    return w;
}

/// How far the stock top-down read stretches a texture on a slope with this
/// normal z: 1 / nz along the fall line.
inline float topDownStretch(float nz) { return 1.0f / std::max(std::fabs(nz), 1e-3f); }

}  // namespace cliff_projection
}  // namespace rendering
}  // namespace wowee
