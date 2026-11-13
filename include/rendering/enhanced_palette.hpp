#pragma once

/// WoW enhanced daylight, as a trend over 1.12's: the "Enhanced daylight"
/// enhancement (docs/re/enhanced-lighting.md section 2).
///
/// The reference client re-authored every outdoor light of the two continents by hand, so
/// no formula turns a 1.12 light into its reference one. Over all of them
/// though - every Light row of maps 0 and 1 matched by ID and position
/// between Classic Era 1.15.9 and the reference client 1.60.1, both sampled each hour from
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

#include "rendering/enhanced_lighting_table.hpp"

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
// The sky's luminance trend is mostly the reference client's sky exposure
// (enhanced_lighting_table::kSkyExposure, x1.3 on most keys), which its linear
// pipeline takes back and 3.3.5's 8-bit sky cannot: it is divided out, as it
// is from the reference client's own keyframes, and the change of colour kept.
inline constexpr Trend kSkyTop{1.09f / enhanced_lighting_table::kSkyExposure, 1.12f};
inline constexpr Trend kSkyMiddle{1.19f / enhanced_lighting_table::kSkyExposure, 1.28f};
inline constexpr Trend kSkyFog{1.30f / enhanced_lighting_table::kSkyExposure, 1.35f};
inline constexpr Trend kSun{1.11f, 0.72f};

/// The sun's intensity in linear light with the HDR scene, by day (weighted
/// by the day intensity): the reference client's LightData F052 on Light 1 (1.35; 1.35-1.5
/// elsewhere, 1 at night). The renderer's enhanced_tonemap::kSunLight (1.25)
/// still scales the result on the display side; the two together were
/// chosen side by side against the reference client's Goldshire and Northshire captures.
inline constexpr float kHdrSunIntensity = 1.35f;

/// The reference client's ambient on models and buildings (material3_mesh_ps and
/// material3_wmo_ps, docs/re/enhanced-lighting.md section 6d): the hemisphere
/// ambient times a half-Lambert ramp toward the sun,
/// mix(kAmbientAway, kAmbientToward, 0.5 + 0.5 N.L), N.L signed. The reference client's
/// shaders carry the ramp twice, 0.4562..1.2333 for its linear path and
/// 0.7..1.1 for display-encoded light - the same ramp, 0.4562^(1/2.2) = 0.70
/// and 1.2333^(1/2.2) = 1.10. Wowee's lit shaders work in display-encoded
/// light, so they take the second. Mirrored in m2.frag, wmo.frag and
/// character.frag (enh_bits::kEnhancedAmbient).
inline constexpr float kAmbientAway = 0.7f;
inline constexpr float kAmbientToward = 1.1f;
/// The sun's light scaled by how much higher or lower the reference client's sun stands
/// than 3.3.5's light at this hour (the ratio of their vertical
/// components, held to 0.5..1.5): the ground keeps the light it had under
/// the stock direction - which every other exposure in this file was set
/// against - while the shadows and the sides lit come from the sun. At
/// noon the reference client's sun is 65 degrees up against the stock light's 37 (x0.67),
/// at 9:22 37 against 30 (x0.82), at 17:51 17 against 20 (x1.2).
/// docs/re/enhanced-lighting.md 2e.
inline float sunIrradianceScale(const glm::vec3& stockTravel, const glm::vec3& travel) {
    if (travel.z > -1e-3f || stockTravel.z > -1e-3f) return 1.0f;
    const float r = stockTravel.z / travel.z;
    return r < 0.5f ? 0.5f : (r > 1.5f ? 1.5f : r);
}

/// The light's fog end (yards) under which the sun glow is off - a zone fog
/// that takes the sky, as Darkshore's (306) and Tirisfal's (181) do - and
/// over which it is whole (Goldshire and the Barrens 500, Westfall 444).
inline constexpr float kGlowFogDense = 320.0f;
inline constexpr float kGlowFogClear = 430.0f;

/// How much of the low-sun glow the air allows: the dome's halo round the
/// sun (skybox.frag), the sun's light on the sky and the dusk glow
/// (enhanced_haze.frag) all add light as if a clear sky lay behind the far
/// air. Under a dense zone fog that sky is the fog, and together they took
/// Darkshore's coast toward an evening sun to a white sheet (luminance
/// 0.93, saturation 0.05) where the reference still is a dim pink mist
/// (0.37-0.47, saturation 0.29). docs/re/enhanced-lighting.md 9f.
inline float clearAirGlow(float fogEnd) {
    float t = (fogEnd - kGlowFogDense) / (kGlowFogClear - kGlowFogDense);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

/// WOWEE_ENHANCED_AMBIENT=0 leaves the ambient flat, for side-by-side
/// comparison (read once).
inline bool ambientRampOn() {
    static const bool v = [] {
        const char* e = std::getenv("WOWEE_ENHANCED_AMBIENT");
        return !(e && *e == '0');
    }();
    return v;
}
inline float ambientRamp(float nDotL) {
    const float t = 0.5f + 0.5f * (nDotL < -1.0f ? -1.0f : (nDotL > 1.0f ? 1.0f : nDotL));
    return kAmbientAway + (kAmbientToward - kAmbientAway) * t;
}

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
/// reference keyframes keeps. The reference client's own Glow is 0 on 106 of the 112 linked
/// profiles - its bloom is a pass of its own, the preset's bloom row - where
/// 3.3.5's same slots carry 0.5-1, whose blur squared lit the reference client's brighter
/// sky into a white sheet. With none at all the picture fell to 0.21-0.23
/// mean luminance against the reference captures' 0.32 (Goldshire 15:23); half
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

/// The HDR scene's lift of the lit mid-tones under the enhanced daylight:
/// light between 0.2 and 0.8 luminance times up to this (fxaa.frag), so the
/// sunlit ground gains on the shade without the shade lifting or the top
/// reaching white. Against Jered's live reference captures (Goldshire 9:22)
/// the lit third read 0.37-0.41 here against the reference client's 0.44-0.59 at 1; a
/// straight exposure (1.25-1.4) took the Barrens' noon dirt to a glowing
/// orange, and a lift of the low end alone greyed the shade.
/// Now 1 (none): the lift took every mid-tone, the tree crowns lit by the
/// sky alone with the sunlit road, and left them 29% brighter than
/// the reference client's; the sun's own light (enhanced_tonemap::kSunLight) brightens
/// what the sun reaches instead (docs/re/enhanced-lighting.md section 2d).
/// WOWEE_ENHANCED_EXPOSURE=<value> overrides it for tuning.
inline constexpr float kHdrExposure = 1.0f;
inline float hdrExposure() {
    static const float v = [] {
        const char* e = std::getenv("WOWEE_ENHANCED_EXPOSURE");
        return (e && *e) ? static_cast<float>(std::atof(e)) : kHdrExposure;
    }();
    return v;
}
/// The HDR scene's chroma scale under the enhanced daylight, on lit surfaces
/// (faded in from 0.25 to 0.45 luminance: the reference client's shade is a near-neutral
/// green, chroma 0.46-0.57, under sunlit ground at 0.62-0.65), easing to 0.8
/// at the top of the tone curve (0.85-1) so a sunlit Barrens road stays under
/// white and Duskwood's noon sky does not keep its full cyan. Against Jered's
/// live Goldshire 9:22 captures, with kHdrExposure: lit chroma 0.63-0.69
/// (reference 0.62-0.65), shade luminance 0.20-0.24 (0.21-0.26), lit/shade
/// 2.14-2.27 (2.07-2.27); docs/re/enhanced-lighting.md section 2c.
/// Now 1 (none; the ease to 0.8 at the top stays): with the stronger sun
/// (section 2d) the sunlit ground reaches the reference client's chroma (0.70 0.62 0.70
/// against 0.70 0.63 0.58) without it, and the crowns' chroma gap falls
/// from +38% to +18%. WOWEE_ENHANCED_SAT overrides it.
inline constexpr float kHdrSaturation = 1.0f;
inline float hdrSaturation() {
    static const float v = [] {
        const char* e = std::getenv("WOWEE_ENHANCED_SAT");
        return (e && *e) ? static_cast<float>(std::atof(e)) : kHdrSaturation;
    }();
    return v;
}

/// The shade's chroma scale under the same tone (full below 0.18 luminance,
/// none from 0.38, by day, outdoors, on warm shade only). 3.3.5's leaf, grass and dirt
/// textures carry almost no blue, so under the reference client's own sky-blue ambient
/// (Goldshire 9:22: 0.40, 0.51, 0.60) the shade stays a saturated
/// yellow-green - blue/luminance 0.41-0.52 against the reference client's 0.71-0.73,
/// chroma 0.67-0.72 against 0.46-0.57. Tinting the shade's light toward the
/// sky's colour did not close it: the sky bands are cyan beside the ambient,
/// and turned the shade green (red/luminance 1.05 -> 0.78). The reference client's shade
/// is this one with a little over half its chroma; docs/re/enhanced-lighting.md
/// section 6c. WOWEE_ENHANCED_SHADE overrides it.
inline constexpr float kHdrShadeChroma = 0.6f;
inline float hdrShadeChroma() {
    static const float v = [] {
        const char* e = std::getenv("WOWEE_ENHANCED_SHADE");
        return (e && *e) ? static_cast<float>(std::atof(e)) : kHdrShadeChroma;
    }();
    return v;
}
/// How far it is day for the shade's chroma: 0 with the sun's sprite at the
/// horizon, 1 from 0.15 of the way up (the HDR sun's own easing,
/// enhanced_tonemap::sunLightScale), so the night keeps its moonlit blue.
inline float dayShare(float sunDirZ) {
    float t = sunDirZ / 0.15f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

/// How much of the sunlit colour a full shadow takes under the enhanced daylight
/// (the renderer's shadowParams.y): measured on Jered's 09:23 capture, where
/// a tree's shade on the crossroads' flagstones is (0.37, 0.31, 0.16) against
/// (0.77, 0.55, 0.27) in the sun beside it - 0.48-0.59 a channel, 0.54 in
/// luminance. 3.3.5's terrain keeps 0.7 (its 77/255 shadow grey). The
/// shaders' floor is 1 - this (terrain.frag; WMO, M2 and units by
/// mix(1, shadow, this)). docs/re/enhanced-lighting.md 2e.
/// Held at 3.3.5's 0.3 for now: 0.46 took the road's lit/shade from 1.57 to
/// 2.2 (the reference client 1.9-2.3) but the evening and forest views 25% darker.
inline constexpr float kShadowOpacity = 0.30f;
/// kShadowOpacity, or WOWEE_ENHANCED_SHADOW_OPACITY=<value> (read once).
inline float shadowOpacity() {
    static const float v = [] {
        const char* e = std::getenv("WOWEE_ENHANCED_SHADOW_OPACITY");
        return (e && *e) ? static_cast<float>(std::atof(e)) : kShadowOpacity;
    }();
    return v;
}

/// The reference client's sun lights the world from where it stands (its shadows fall
/// from the sun; docs/re/enhanced-lighting.md 2e): the way the light travels
/// is -toSun by day, eased from 3.3.5's `stockTravel` over the sun's first
/// 9 degrees (dayShare), so the night keeps the stock moonlight direction.
inline glm::vec3 sunLightDirection(const glm::vec3& stockTravel, const glm::vec3& toSun) {
    const float w = dayShare(toSun.z);
    const glm::vec3 d = glm::mix(stockTravel, -toSun, w);
    const float len = glm::length(d);
    return len > 1e-4f ? d / len : stockTravel;
}

/// How far the grey the shade keeps takes the ambient's hue (its luminance
/// kept): the reference client's ambient is its sky light, and its shade, against 3.3.5's
/// orange dirt and yellow leaves, a little bluer and less red.
/// WOWEE_ENHANCED_SHADE_SKY overrides it.
inline constexpr float kHdrShadeSky = 0.6f;
inline float hdrShadeSky() {
    static const float v = [] {
        const char* e = std::getenv("WOWEE_ENHANCED_SHADE_SKY");
        return (e && *e) ? static_cast<float>(std::atof(e)) : kHdrShadeSky;
    }();
    return v;
}
/// The tint for that grey: the ambient's blue against its red and green
/// (their mean, so it leans neither red nor green), at unit luminance,
/// kHdrShadeSky of the way from white, scaled by `weight` (0 = white, none).
/// The ambient's own hue leans green beside its red (0.40, 0.51, 0.60 at
/// Goldshire 9:22) and turned the shade a green grey (red/luminance 0.98
/// against the reference client's 1.05).
inline glm::vec3 shadeTint(const glm::vec3& ambient, float weight) {
    const float warm = 0.5f * (ambient.r + ambient.g);
    const glm::vec3 cool(warm, warm, ambient.b);
    const float y = glm::dot(cool, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    if (y < 1e-3f || weight <= 0.0f) return glm::vec3(1.0f);
    return glm::mix(glm::vec3(1.0f), cool / y, hdrShadeSky() * weight);
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
/// face what the reference client's linear pipeline gives it: the two added as light and
/// encoded back for the screen,
///
///   direct' = encode(decode(ambient) + decode(direct)) - ambient
///
/// with encode/decode the 2.2 display power. The reference client authors its colours for
/// that pipeline (docs/re/enhanced-lighting.md section 2c): its noon sun over
/// Elwynn is `#eabd8a` against 1.12's `#ff8800` at the same ambient, and
/// summed raw on an 8-bit path the two clip a lit face to white
/// (1.33, 1.25, 1.15 -> 1, 1, 1) where 1.12's stays warm (1, 1, 0.6). Added
/// as light the face is (0.98, 0.87, 0.79): the reference client's own warm, unclipped
/// sunlit ground. A face in shade (N.L 0) keeps the ambient exactly; in
/// between, the shaders' straight line runs a little under the curve.
/// Weighted by `weight` (0 returns `direct` unchanged).
///
/// With the HDR scene (`hdr`) the sum is not held at one - the tone map takes
/// what passes it - and the sun may carry an intensity in linear light
/// (`sunIntensity`, kHdrSunIntensity: the reference client's LightData F052).
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
