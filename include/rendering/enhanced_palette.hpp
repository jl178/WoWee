#pragma once

/// the reference client's daylight, as a trend over 1.12's: the "Enhanced daylight"
/// enhancement (docs/re/enhanced-lighting.md section 2).
///
/// Enhanced re-authored every outdoor light of the two continents by hand, so
/// no formula turns a 1.12 light into its Enhanced one. Over all of them
/// though - every Light row of maps 0 and 1 matched by ID and position
/// between Classic Era 1.15.9 and Enhanced 1.60.1, both sampled each hour from
/// 8:00 to 17:00, 1950 samples a colour - the change is consistent:
///
///   colour          luminance  saturation (chroma)
///   DirectColor       x1.03      x0.81   a whiter sun
///   AmbientColor      x1.15      x1.08
///   SkyTopColor       x1.09      x1.12
///   SkyMiddleColor    x1.19      x1.28
///   SkyFogColor       x1.30      x1.35   the fog and horizon
///   SunColor          x1.11      x0.72
///
/// These are applied to the light 3.3.5's Light.dbc gives (the old-world
/// lights are 1.12's), by day only, on maps 0 and 1. No renderer includes.

#include "rendering/enhanced_lights.hpp"

#include <glm/glm.hpp>

#include <cstdlib>

namespace wowee {
namespace rendering {
namespace enhanced_palette {

struct Trend {
    float luminance;
    float chroma;
};

inline constexpr Trend kDirect{1.03f, 0.81f};
inline constexpr Trend kAmbient{1.15f, 1.08f};
// The sky's luminance trend is mostly Enhanced's sky exposure
// (enhanced_lights::kSkyExposure, x1.3 on most keys), which its linear
// pipeline takes back and 3.3.5's 8-bit sky cannot: it is divided out, as it
// is from Enhanced's own keyframes, and the change of colour kept.
inline constexpr Trend kSkyTop{1.09f / enhanced_lights::kSkyExposure, 1.12f};
inline constexpr Trend kSkyMiddle{1.19f / enhanced_lights::kSkyExposure, 1.28f};
inline constexpr Trend kSkyFog{1.30f / enhanced_lights::kSkyExposure, 1.35f};
inline constexpr Trend kSun{1.11f, 0.72f};

/// The sun's intensity in linear light with the HDR scene, by day (weighted
/// by the day intensity): Enhanced's LightData F052 on Light 1 (1.35; 1.35-1.5
/// elsewhere, 1 at night). The renderer's enhanced_tonemap::kSunLight (1.25)
/// still scales the result on the display side; the two together were
/// chosen side by side against Enhanced's Goldshire and Northshire captures.
inline constexpr float kHdrSunIntensity = 1.35f;

/// kHdrSunIntensity, or for tuning side by side WOWEE_ENHANCED_SUN=<value>
/// (read once).
inline float hdrSunIntensity() {
    static const float v = [] {
        const char* e = std::getenv("WOWEE_ENHANCED_SUN");
        return (e && *e) ? static_cast<float>(std::atof(e)) : kHdrSunIntensity;
    }();
    return v;
}

/// How much of 3.3.5's stock full-screen glow (LightParams Glow) a light with
/// Enhanced keyframes keeps. Enhanced's own Glow is 0 on 106 of the 112 linked
/// profiles - its bloom is a pass of its own, the preset's bloom row - where
/// 3.3.5's same slots carry 0.5-1, whose blur squared lit Enhanced's brighter
/// sky into a white sheet. With none at all the picture fell to 0.21-0.23
/// mean luminance against Enhanced's captures' 0.32 (Goldshire 15:23); half
/// keeps most of the mid-tones' lift and the sky round the sun under white.
/// WOWEE_ENHANCED_GLOW=<scale> overrides it for tuning.
inline constexpr float kGlowScale = 0.5f;
inline float glowScale() {
    static const float v = [] {
        const char* e = std::getenv("WOWEE_ENHANCED_GLOW");
        return (e && *e) ? static_cast<float>(std::atof(e)) : kGlowScale;
    }();
    return v;
}

/// The HDR scene's lift of the lit mid-tones under the Enhanced daylight:
/// light between 0.2 and 0.8 luminance times up to this (fxaa.frag), so the
/// sunlit ground gains on the shade without the shade lifting or the top
/// reaching white. Against Jered's live Enhanced captures (Goldshire 9:22)
/// the lit third read 0.37-0.41 here against Enhanced's 0.44-0.59 at 1; a
/// straight exposure (1.25-1.4) took the Barrens' noon dirt to a glowing
/// orange, and a lift of the low end alone greyed the shade.
/// WOWEE_ENHANCED_EXPOSURE=<value> overrides it for tuning.
inline constexpr float kHdrExposure = 1.3f;
inline float hdrExposure() {
    static const float v = [] {
        const char* e = std::getenv("WOWEE_ENHANCED_EXPOSURE");
        return (e && *e) ? static_cast<float>(std::atof(e)) : kHdrExposure;
    }();
    return v;
}
/// The HDR scene's chroma scale under the Enhanced daylight, on lit surfaces
/// (faded in from 0.25 to 0.45 luminance: Enhanced's shade is a near-neutral
/// green, chroma 0.46-0.57, under sunlit ground at 0.62-0.65), easing to 0.8
/// at the top of the tone curve (0.85-1) so a sunlit Barrens road stays under
/// white and Duskwood's noon sky does not keep its full cyan. Against Jered's
/// live Goldshire 9:22 captures, with kHdrExposure: lit chroma 0.63-0.69
/// (Enhanced 0.62-0.65), shade luminance 0.20-0.24 (0.21-0.26), lit/shade
/// 2.14-2.27 (2.07-2.27); docs/re/enhanced-lighting.md section 2c.
/// WOWEE_ENHANCED_SAT overrides it.
inline constexpr float kHdrSaturation = 1.35f;
inline float hdrSaturation() {
    static const float v = [] {
        const char* e = std::getenv("WOWEE_ENHANCED_SAT");
        return (e && *e) ? static_cast<float>(std::atof(e)) : kHdrSaturation;
    }();
    return v;
}

inline bool appliesToMap(unsigned mapId) { return mapId == 0 || mapId == 1; }

/// A colour's luminance scaled by t.luminance and its distance from that grey
/// by t.chroma, blended in by `weight` (0 leaves it as it was). Not clamped:
/// the lit shaders clamp the sum of their light at one, as stock does.
inline glm::vec3 apply(const glm::vec3& c, const Trend& t, float weight) {
    const float y = glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    const glm::vec3 grey(y);
    const glm::vec3 shifted = grey * t.luminance + (c - grey) * t.chroma;
    return glm::max(c + (shifted - c) * weight, glm::vec3(0.0f));
}

/// The sun's colour such that the sky and the sun, added as the lit shaders
/// add them (display values, clamped at one per channel), give a fully lit
/// face what Enhanced's linear pipeline gives it: the two added as light and
/// encoded back for the screen,
///
///   direct' = encode(decode(ambient) + decode(direct)) - ambient
///
/// with encode/decode the 2.2 display power. Enhanced authors its colours for
/// that pipeline (docs/re/enhanced-lighting.md section 2c): its noon sun over
/// Elwynn is `#eabd8a` against 1.12's `#ff8800` at the same ambient, and
/// summed raw on an 8-bit path the two clip a lit face to white
/// (1.33, 1.25, 1.15 -> 1, 1, 1) where 1.12's stays warm (1, 1, 0.6). Added
/// as light the face is (0.98, 0.87, 0.79): Enhanced's own warm, unclipped
/// sunlit ground. A face in shade (N.L 0) keeps the ambient exactly; in
/// between, the shaders' straight line runs a little under the curve.
/// Weighted by `weight` (0 returns `direct` unchanged).
///
/// With the HDR scene (`hdr`) the sum is not held at one - the tone map takes
/// what passes it - and the sun may carry an intensity in linear light
/// (`sunIntensity`, kHdrSunIntensity: Enhanced's LightData F052).
inline glm::vec3 linearLightDirect(const glm::vec3& ambient, const glm::vec3& direct, float weight,
                                   float sunIntensity = 1.0f, bool hdr = false) {
    constexpr float kGamma = 2.2f;
    const glm::vec3 a = glm::clamp(ambient, glm::vec3(0.0f), glm::vec3(1.0f));
    const glm::vec3 d = glm::max(direct, glm::vec3(0.0f));
    const glm::vec3 sum = glm::pow(a, glm::vec3(kGamma)) + glm::pow(d, glm::vec3(kGamma)) * sunIntensity;
    const glm::vec3 lit = glm::pow(hdr ? sum : glm::min(sum, glm::vec3(1.0f)), glm::vec3(1.0f / kGamma));
    const glm::vec3 out = glm::max(lit - a, glm::vec3(0.0f));
    return glm::mix(direct, out, glm::clamp(weight, 0.0f, 1.0f));
}

}  // namespace enhanced_palette
}  // namespace rendering
}  // namespace wowee
