#pragma once

#include <algorithm>
#include <cmath>

namespace wowee::rendering {

/// How wet the outdoor ground is while it rains, as the renderer eases it and
/// terrain.frag and wmo.frag read it (rtParams.w; see wet_surface.glsli).
///
/// Not stock: the 3.3.5 client draws the same dry ground in a downpour. The
/// "Wet ground in rain" setting turns it on. It is a number from 0, dry, to 1,
/// soaked, that follows the weather slowly in both directions - ground does
/// not go dark the instant the first drop falls, nor dry the moment the rain
/// stops - so it is eased here rather than read off the weather directly.
///
/// Header-only and pure, so the curve can be tested without a renderer.
struct WetGround {
    /// Time constants of the exponential ease, in seconds. Ninety-five
    /// percent of the way takes three of them: about 12 s to soak once rain
    /// starts and about 18 s to dry once it stops.
    static constexpr float kWettingSeconds = 4.0f;
    static constexpr float kDryingSeconds = 6.0f;
    /// Below this the ground counts as dry and the value snaps to 0, so the
    /// shaders' early-out is taken rather than a tail that never quite ends.
    static constexpr float kDryEpsilon = 0.002f;
};

/// What the wetness heads toward under this weather. `rain` and `storm` are
/// the falling-water kinds (Weather::Type::RAIN and STORM); snow and fine
/// weather are dry. Light rain still wets the ground most of the way, since a
/// steady drizzle soaks it as surely as a shower, only less glossily.
inline float wetGroundTarget(bool rain, bool storm, float intensity) {
    const float i = std::clamp(intensity, 0.0f, 1.0f);
    if (i <= 0.0f) return 0.0f;
    if (storm) return 0.8f + 0.2f * i;
    if (rain) return 0.6f + 0.4f * i;
    return 0.0f;
}

/// One step of the ease from `current` toward `target` over `dt` seconds:
/// faster getting wet than drying out. Never overshoots, and lands on 0 once
/// close enough to dry.
inline float easeWetGround(float current, float target, float dt) {
    const float tau = target > current ? WetGround::kWettingSeconds : WetGround::kDryingSeconds;
    const float k = 1.0f - std::exp(-std::max(dt, 0.0f) / tau);
    float next = current + (target - current) * k;
    if (target <= 0.0f && next < WetGround::kDryEpsilon) next = 0.0f;
    return std::clamp(next, 0.0f, 1.0f);
}

}  // namespace wowee::rendering
