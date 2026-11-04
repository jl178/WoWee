#pragma once

/// Highlight roll-off (E18, the "Soft highlights" enhancement row).
///
/// Where stock clips. The scene is drawn into an 8-bit target, so every
/// surface reaches the last pass at or below 1. The stock full-screen glow
/// (ffxGlow, fxaa.frag stockScreenEffect) then adds blur^2 * glow on top, with
/// glow LightParams.Glow of the light around the camera (0..1): sunlit snow at
/// 0.9 under a 0.9 blur and a glow of 0.5 comes to 1.3, and the swapchain
/// write clips it - the flat white of Storm Peaks or Winterspring at noon, and
/// the flat cores of bright spells. Nothing else in the composite passes 1.
///
/// The shoulder. Each channel is left exactly as it is up to the knee (0.8).
/// Above it the composite's range [knee, white] is eased onto [knee, 1] by
///
///     f(x) = knee + (1 - knee) * (1 - exp(-c t)) / (1 - exp(-c)),
///     t = (x - knee) / (white - knee),
///
/// with c chosen so that f's slope at the knee is 1 (no kink where the curve
/// takes over) and white the most the composite can reach, 1 + glow. So no
/// channel ever clips, the curve bends only as far as this zone's glow needs,
/// and with no glow white is 1 and c is 0: f is the identity and the pass is
/// skipped. Per channel, as the stock clip is, so a hue goes toward white the
/// way stock's does, only gradually. No colour grading, no contrast change
/// below the knee (test_highlight_rolloff).
///
/// fxaa.frag carries the same curve (highlightRollOff); PostProcessPipeline
/// pushes white and c, white 0 for off.

#include <algorithm>
#include <cmath>

namespace wowee {
namespace rendering {
namespace highlight_rolloff {

/// Below this every channel is the stock value, bit for bit.
inline constexpr float kKnee = 0.8f;

struct Curve {
    float white = 0.0f;  ///< the input that maps to 1; 0 = off (identity)
    float c = 0.0f;      ///< the exponential's rate; 0 = the straight line
};

/// The rate c for which f'(knee) = 1, i.e. c / (1 - exp(-c)) = a, where
/// a = (white - knee) / (1 - knee) >= 1. That left side rises from 1 (c -> 0)
/// and is about c for large c, so bisection over [0, a + 1] finds it.
inline float solveRate(float a) {
    if (!(a > 1.0001f)) return 0.0f;
    float lo = 0.0f, hi = a + 1.0f;
    for (int i = 0; i < 48; ++i) {
        const float mid = 0.5f * (lo + hi);
        const float h = mid / (1.0f - std::exp(-mid));
        (h < a ? lo : hi) = mid;
    }
    return 0.5f * (lo + hi);
}

/// The curve for the composite under a glow strength (the push constant the
/// glow is drawn with, 0..1). Off, or with no glow to overflow, the identity.
inline Curve curveForGlow(bool enabled, float glowAmount) {
    Curve k;
    const float g = std::clamp(glowAmount, 0.0f, 1.0f);
    if (!enabled || g <= 0.0f) return k;
    k.white = 1.0f + g;
    k.c = solveRate((k.white - kKnee) / (1.0f - kKnee));
    return k;
}

/// One channel through the curve; fxaa.frag's highlightRollOff.
inline float apply(float x, const Curve& k) {
    if (k.white <= 0.0f || !(x > kKnee)) return x;
    const float t = std::clamp((x - kKnee) / (k.white - kKnee), 0.0f, 1.0f);
    const float s = k.c > 1e-4f ? (1.0f - std::exp(-k.c * t)) / (1.0f - std::exp(-k.c)) : t;
    return kKnee + (1.0f - kKnee) * s;
}

}  // namespace highlight_rolloff
}  // namespace rendering
}  // namespace wowee
