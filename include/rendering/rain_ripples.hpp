#pragma once

#include <algorithm>
#include <cmath>

namespace wowee::rendering {

/// How hard the rain falls for the rain ripples (docs/enhancements-plan.md
/// E5), as the renderer eases it and water.frag (push.rainRipple) and the
/// wet ground (rtParams.z, wet_surface.glsli) read it.
///
/// Not stock: the 3.3.5 client's rain leaves water and ground alone. The
/// "Rain ripples" setting turns it on. Unlike the wet ground it follows the
/// rain itself, eased over a second or two, so rings start with the first
/// drops and stop soon after the last. The value is also each ripple cell's
/// chance of a drop per cycle (rain_ripples.glsli), so a drizzle rings
/// sparsely and a storm everywhere.
///
/// Header-only and pure, so the curve can be tested without a renderer.
struct RainRipple {
    /// Time constants of the ease: rings come in over about 3 s (three time
    /// constants) and die away over about 4.5.
    static constexpr float kRisingSeconds = 1.0f;
    static constexpr float kFallingSeconds = 1.5f;
    /// Below this it is dry and snaps to 0, so the shaders skip the block.
    static constexpr float kDryEpsilon = 0.01f;
};

/// What the strength heads toward under this weather: nothing in fine weather
/// or snow, a sparse drizzle to most cells in a downpour, every cell in a
/// storm.
inline float rainRippleTarget(bool rain, bool storm, float intensity) {
    const float i = std::clamp(intensity, 0.0f, 1.0f);
    if (i <= 0.0f) return 0.0f;
    if (storm) return 0.6f + 0.4f * i;
    if (rain) return 0.25f + 0.6f * i;
    return 0.0f;
}

/// One step of the ease from `current` toward `target` over `dt` seconds.
/// Never overshoots, and lands on 0 once close enough to dry.
inline float easeRainRipple(float current, float target, float dt) {
    const float tau = target > current ? RainRipple::kRisingSeconds : RainRipple::kFallingSeconds;
    const float k = 1.0f - std::exp(-std::max(dt, 0.0f) / tau);
    float next = current + (target - current) * k;
    if (target <= 0.0f && next < RainRipple::kDryEpsilon) next = 0.0f;
    return std::clamp(next, 0.0f, 1.0f);
}

}  // namespace wowee::rendering
