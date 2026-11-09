#pragma once

/// The tone curve of the HDR scene (the "HDR lighting (the reference client)" row,
/// docs/re/enhanced-lighting.md section 9b), and the few constants that let
/// light past 1 into it.
///
/// the reference client renders in linear light into RGBA16F (its `linear` CVar) with
/// a sun up to 1.5 (LightData F052) and tone maps before its colour grade.
/// Wowee's lit shaders still write 3.3.5's display-encoded values, which sit
/// in 0..1 nearly everywhere; what passes 1 is the sky round the sun, the sun
/// itself, the haze toward it and the glow. So the curve keeps everything the
/// 8-bit scene could show below a knee exactly as it was, and only above it
/// eases the rest toward 1 instead of clipping: Reinhard on the excess,
/// u / (1 + u) over the headroom left, slope 1 at the knee so there is no
/// crease, 1 approached and never reached.
///
/// fxaa.frag (hdrToDisplay, displayToHdr) runs the same curve with the knee
/// pushed from here. No renderer includes, so a test can read it.

#include <cstdio>
#include <cstdlib>

namespace wowee {
namespace rendering {
namespace enhanced_tonemap {

/// Where the curve leaves the identity. Below it the HDR scene shows exactly
/// what the 8-bit one did.
inline constexpr float kKnee = 0.8f;

/// Scene light to display, one channel.
inline float toDisplay(float x, float knee = kKnee) {
    if (!(x > 0.0f)) return 0.0f;  // negatives and NaN
    if (x <= knee) return x;
    const float u = (x - knee) / (1.0f - knee);
    return knee + (1.0f - knee) * u / (1.0f + u);
}

/// Its inverse on 0..1 (1 itself is held just under, at 0.999).
inline float toScene(float d, float knee = kKnee) {
    d = d < 0.0f ? 0.0f : (d > 0.999f ? 0.999f : d);
    if (d <= knee) return d;
    const float f = (d - knee) / (1.0f - knee);
    return knee + (1.0f - knee) * f / (1.0f - f);
}

/// The sun's disc and glare, drawn additive over the dome, at this many
/// times their stock colour: in the 8-bit scene they clipped to a flat disc
/// at about 1, here they carry the brightness the bloom spreads.
inline constexpr float kSunGain = 2.0f;

/// The share of the light above 1 the bloom adds back (times the Bloom row).
inline constexpr float kHdrBloom = 0.5f;

/// The Enhanced haze's light is capped at 1.3 in the 8-bit scene, where more
/// only blew the sky out; under the tone curve it may reach this
/// (enhanced_haze.frag, flag 8).
inline constexpr float kHazeCap = 3.0f;

/// The values in use: the constants above, or for tuning side by side
/// WOWEE_HDR_TUNE="knee,bloom,sunGain" (read once; any left out keep
/// their constant). The haze cap is the shader's own (enhanced_haze.frag).
struct Tuning {
    float knee = kKnee;
    float hdrBloom = kHdrBloom;
    float sunGain = kSunGain;
};
inline const Tuning& tuning() {
    static const Tuning t = [] {
        Tuning r;
        if (const char* e = std::getenv("WOWEE_HDR_TUNE"); e && *e) {
            float v[3] = {r.knee, r.hdrBloom, r.sunGain};
            std::sscanf(e, "%f,%f,%f", &v[0], &v[1], &v[2]);
            r.knee = v[0] < 0.0f ? 0.0f : (v[0] > 0.95f ? 0.95f : v[0]);
            r.hdrBloom = v[1];
            r.sunGain = v[2];
        }
        return r;
    }();
    return t;
}

}  // namespace enhanced_tonemap
}  // namespace rendering
}  // namespace wowee
