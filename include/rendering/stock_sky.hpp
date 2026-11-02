#pragma once

/// The stock client's procedural sky, read out of Wow.exe 3.3.5a (12340).
///
/// DayNight draws it in 0x7f09b0, every frame the procedural sky is up:
///
///   stars.mdl (0x9abd50), the sun and both moons (0x9ac660), the sky dome
///   (0x9acb00) and the cloud layer (0x9acd40), in that order, at depth range
///   0.999..1.0 - then the LightSkybox models (0x7f08c0) at their weights.
///
/// The procedural part is skipped as a whole when a sky model is up at more
/// than 0.99 and its LightSkybox flags lack bit 2 (or the death/override sky
/// is), which is what a full-weight zone sky does.
///
/// The dome is drawn with GxBlend_Add (blend 3), so it adds onto the stars,
/// sun and moons drawn before it on the cleared frame: the sun and moons read
/// as sprite + sky, and a dark night sky lets the stars through.
///
/// Everything here is pure maths so it can be unit tested; the shaders
/// (skybox.frag, celestial.*, clouds.frag) carry the same numbers.

#include "rendering/stock_day_night.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <array>
#include <cmath>
#include <span>

namespace wowee::rendering::stock_sky {

using stock_day_night::Key;
using stock_day_night::sample;

// ---------------------------------------------------------------------------
// The dome (built by 0x7f2470, coloured by 0x7f0530 each frame)
// ---------------------------------------------------------------------------

/// A unit sphere of 7 rings x 24 segments whose centre sits cos(pi/4) below
/// the eye, so the eye is on its 45-degree ring. Ring polar angles as
/// fractions of pi (0xa41a90): the zenith, five rings crowded into the last
/// 30 degrees above the horizon, and the nadir.
inline constexpr std::array<float, 7> kDomeRingTheta{0.0f, 0.17f, 0.20f, 0.23f,
                                                     0.24f, 0.25f, 1.0f};
inline constexpr float kDomeCentreDrop = 0.70710678f;  // cos(pi/4), 0x9eaf48
inline constexpr int kDomeSegments = 24;

/// The sphere's polar angle, as a fraction of pi, where the eye ray `dirZ`
/// (the z of a unit view direction, +Z up) meets the dome. 0.25 is the
/// horizon; the rings above sit at about 16.8, 9.8, 3.7 and 1.8 degrees of
/// elevation.
inline float domePolarFraction(float dirZ) {
    const float h = kDomeCentreDrop;
    const float dz = glm::clamp(dirZ, -1.0f, 1.0f);
    const float t = -h * dz + std::sqrt(glm::max(h * h * dz * dz - h * h + 1.0f, 0.0f));
    const float cosTheta = glm::clamp(t * dz + h, -1.0f, 1.0f);
    return std::acos(cosTheta) / glm::pi<float>();
}

/// The dome ring a polar fraction falls in and how far towards the next:
/// x = ring (0..5), y = 0..1 between it and ring x+1.
inline glm::vec2 domeRingPosition(float polarFraction) {
    const float p = glm::clamp(polarFraction, 0.0f, 1.0f);
    for (int i = 0; i + 1 < static_cast<int>(kDomeRingTheta.size()); ++i) {
        if (p <= kDomeRingTheta[i + 1]) {
            const float span = kDomeRingTheta[i + 1] - kDomeRingTheta[i];
            return {static_cast<float>(i), span > 0.0f ? (p - kDomeRingTheta[i]) / span : 0.0f};
        }
    }
    return {5.0f, 1.0f};
}

/// 0xaf4b7c: the "highlight" by time of day, times LightParams.HighlightSky
/// (0 or 1; 369 of the 850 rows set it). A flash at dawn (peak 06:30) and
/// dusk (21:30); zero from 07:00 to 20:30.
inline constexpr std::array<Key, 6> kHighlightKeys{{{0.125f, 0.0f},
                                                    {0.2708333f, 1.0f},
                                                    {0.2916667f, 0.0f},
                                                    {0.8541666f, 0.0f},
                                                    {0.8958333f, 1.0f},
                                                    {0.9993055f, 0.0f}}};

/// 0xaf4bac: the highlight round the horizon, by world azimuth as a fraction
/// of a turn (atan2(y, x) / 2pi, +X north, +Y west). 0x7f0530 indexes it from
/// the camera's yaw plus the vertex's place on the yaw-aligned dome, which
/// comes out as the vertex's world azimuth.
inline constexpr std::array<Key, 6> kHighlightAzimuthKeys{{{0.125f, 1.0f},
                                                           {0.375f, 0.0f},
                                                           {0.5f, -0.5f},
                                                           {0.625f, -0.7f},
                                                           {0.75f, -0.5f},
                                                           {0.875f, 0.0f}}};

inline float highlight(float dayFraction, float highlightSky) {
    return sample(kHighlightKeys, dayFraction) * highlightSky;
}

/// The six LightIntBand sky colours, channels 2..7.
struct DomeColors {
    glm::vec3 top, middle, band1, band2, smog, fog;
};

/// One dome vertex's colour, as 0x7f0530 writes it: ring 0 the zenith (top),
/// rings 1-4 middle/band1/band2/smog with the dawn/dusk highlight, ring 5 and
/// the nadir the fog colour. `azimuthFraction` is the vertex's world azimuth
/// over 2pi, `h` the highlight above.
inline glm::vec3 domeRingColor(int ring, float azimuthFraction, const DomeColors& c, float h) {
    if (ring <= 0) return c.top;
    if (ring >= 5) return c.fog;
    const glm::vec3 normal = ring == 1 ? c.middle : ring == 2 ? c.band1 : ring == 3 ? c.band2 : c.smog;
    if (h <= 0.0f) return normal;
    // Each band pulled towards the middle colour by the highlight.
    const glm::vec3 lit = glm::mix(normal, c.middle, h);
    float a = azimuthFraction - std::floor(azimuthFraction);
    const float g = sample(kHighlightAzimuthKeys, a);
    if (g >= 0.0f) return glm::mix(normal, lit, (1.0f - g) * h);
    const glm::vec3 towardTop = glm::mix(lit, c.top, h * 0.7f);
    return glm::mix(lit, towardTop, -g * h);
}

/// The dome colour along a unit view direction in render coordinates
/// (renderX = wowY, renderY = wowX, +Z up), interpolated between rings in
/// polar angle, as the Gouraud-shaded mesh is.
inline glm::vec3 domeColor(const glm::vec3& dir, const DomeColors& c, float h) {
    const glm::vec2 rp = domeRingPosition(domePolarFraction(dir.z));
    const int ring = static_cast<int>(rp.x);
    // WoW's atan2(y, x) in render components.
    const float az = std::atan2(dir.x, dir.y) / (2.0f * glm::pi<float>());
    return glm::mix(domeRingColor(ring, az, c, h), domeRingColor(ring + 1, az, c, h), rp.y);
}

// ---------------------------------------------------------------------------
// Sun, moons and their glare (0x7eecc0, 0x7ef6e0, 0x7edbe0/0x7edee0)
// ---------------------------------------------------------------------------

/// The sprites are quads `size` yards on a side, 12 yards from the eye
/// (0xa1047c), facing it.
inline constexpr float kSpriteDistance = 12.0f;

/// The sun's size by time of day (0xd39128): twice as large from 21:00 to
/// 06:00 and over the hour either side of sunrise and sunset.
inline constexpr std::array<Key, 4> kSunSizeKeys{{{0.25f, 2.0f},
                                                  {0.28125f, 1.0f},
                                                  {0.84375f, 1.0f},
                                                  {0.875f, 2.0f}}};
/// Both moons (0xd39108), times 1.75 for the White Lady and 1.0 for the
/// Blue Child.
inline constexpr std::array<Key, 4> kMoonSizeKeys{{{0.0416667f, 1.0f},
                                                   {0.1666667f, 1.5f},
                                                   {0.9166667f, 1.5f},
                                                   {0.9993056f, 1.0f}}};
inline constexpr float kSunBaseSize = 1.0f;
inline constexpr float kWhiteLadyBaseSize = 1.75f;
inline constexpr float kBlueChildBaseSize = 1.0f;
/// The Blue Child goes round once every 1.7 days (0xd38e84).
inline constexpr float kBlueChildPeriodDays = 1.7f;

inline float sunSize(float dayFraction) { return sample(kSunSizeKeys, dayFraction) * kSunBaseSize; }
inline float whiteLadySize(float dayFraction) {
    return sample(kMoonSizeKeys, dayFraction) * kWhiteLadyBaseSize;
}

/// Half the angle a quad `size` yards across subtends at the sprite distance.
inline float spriteHalfAngle(float size) { return std::atan(0.5f * size / kSpriteDistance); }

/// 0x7edee0: a sprite is clipped at the eye's height and fades over the last
/// 0.4 yards above it. `heightAboveEye` is the vertex's z relative to the
/// camera, in yards, on the 12-yard quad.
inline float horizonFade(float heightAboveEye) { return glm::clamp(heightAboveEye * 2.5f, 0.0f, 1.0f); }

/// The storm's hold on the sky: 0xd38b88 = weather intensity x 4, at most 1.
inline float stormFactor(float weatherIntensity) { return glm::clamp(weatherIntensity * 4.0f, 0.0f, 1.0f); }

/// The glare sprites (Textures\sunGlare.blp, moonGlare.blp), 0x7ef6e0.
struct GlareSpec {
    float baseScale;      ///< +0x20
    float riseRate;       ///< visibility per second towards a higher target
    float fallRate;       ///< ... towards a lower one
    float sizeMin, sizeMax;   ///< by view alignment, times baseScale
    float alphaMin, alphaMax; ///< by view alignment
    float alignStart;     ///< dot(view, dir) where alignment starts (0.7)
};
/// 0x7ee150: size 3 to 20 yards, alpha 0.5 to 1.
inline constexpr GlareSpec kSunGlare{1.0f, 4.0f, 1.5151515f, 3.0f, 20.0f, 0.5f, 1.0f, 0.7f};
/// 0x7ee230: twice the moon's own size, alpha 0.1 to 1. sizeMin/Max are
/// replaced by the moon's size each frame.
inline constexpr GlareSpec kMoonGlare{2.0f, 3.0303030f, 1.5151515f, 0.0f, 0.0f, 0.1f, 1.0f, 0.7f};

/// When each glare is up at all (0xd38f18 sun, 0xd38fc8 moon).
inline constexpr std::array<Key, 4> kSunGlareDayKeys{{{0.2708333f, 0.0f},
                                                      {0.3125f, 1.0f},
                                                      {0.8125f, 1.0f},
                                                      {0.875f, 0.0f}}};
inline constexpr std::array<Key, 4> kMoonGlareDayKeys{{{0.0833333f, 1.0f},
                                                       {0.1354167f, 0.0f},
                                                       {0.9479167f, 0.0f},
                                                       {0.9993056f, 1.0f}}};

/// How squarely the camera looks at the body: 0 below dot 0.7, 1 dead on.
inline float glareAlignment(float viewDotDir, const GlareSpec& g) {
    if (viewDotDir <= g.alignStart) return 0.0f;
    return glm::clamp((viewDotDir - g.alignStart) / (1.0f - g.alignStart), 0.0f, 1.0f);
}

/// One frame of the glare's eased visibility towards `target`.
inline float easeGlare(float current, float target, float dt, const GlareSpec& g) {
    if (target > current) return glm::min(target, current + g.riseRate * dt);
    if (target < current) return glm::max(target, current - g.fallRate * dt);
    return current;
}

// ---------------------------------------------------------------------------
// Stars (0x7ee0d0 -> 0x9abd50)
// ---------------------------------------------------------------------------

/// stars.mdl's alpha (0xaf4c20): full to 03:00, gone by 04:30, back from
/// 22:30 to midnight. The same on every map; only a full-weight sky model
/// (which takes the whole procedural sky away) hides them.
inline constexpr std::array<Key, 4> kStarsKeys{{{0.125f, 1.0f},
                                                {0.1875f, 0.0f},
                                                {0.9375f, 0.0f},
                                                {1.0f, 1.0f}}};
inline float starsAlpha(float dayFraction) {
    // Stored as round(v * 254 + 1) and drawn only above 1.
    const float a = sample(kStarsKeys, dayFraction);
    return a * 254.0f + 1.0f > 1.5f ? a : 0.0f;
}

// ---------------------------------------------------------------------------
// Clouds (0x7f20e0 mesh, 0x7efd00 texture, 0x7efae0 light)
// ---------------------------------------------------------------------------

/// The cloud dome: the same lowered unit sphere, 12 rings x 16 segments down
/// to the horizon. Texture coordinates are polar: radius ring/22 about the
/// centre of a 128x128 tiled noise texture.
inline constexpr std::array<float, 12> kCloudRingTheta{0.0f,  0.025f, 0.05f, 0.075f,
                                                       0.1f,  0.125f, 0.15f, 0.175f,
                                                       0.205f, 0.23f, 0.245f, 0.25f};
/// clouds.frag.glsl walks these per pixel (its kRing / kRingAlpha) to find the
/// fractional ring a ray meets the dome at; nothing on the CPU needs it.
/// Vertex alpha per ring (0xa41b04): full to ring 8 (about 8.8 degrees up),
/// half at ring 9, none on the last two.
inline constexpr std::array<float, 12> kCloudRingAlpha{1, 1, 1, 1, 1, 1, 1, 1, 1, 128.0f / 255.0f, 0, 0};

inline float cloudRingAlpha(float ringIndex) {
    const float r = glm::clamp(ringIndex, 0.0f, 11.0f);
    const int i = glm::min(static_cast<int>(r), 10);
    return glm::mix(kCloudRingAlpha[i], kCloudRingAlpha[i + 1], r - static_cast<float>(i));
}

/// A texel's opacity: the noise sum `n` (5 octaves of 3D value noise at
/// weights 1, 1/2 .. 1/16) to a byte round(n*64 + 128), less the clear sky
/// (1 - cover) * 255 where cover is LightFloatBand 3; then the 0xd38588
/// ramp, 1 - 0.96^v.
inline float cloudAlpha(float noiseSum, float cover) {
    const float v = std::round(noiseSum * 64.0f + 128.0f) -
                    std::round((1.0f - glm::clamp(cover, 0.0f, 1.0f)) * 255.0f);
    if (v <= 0.0f) return 0.0f;
    return 1.0f - std::pow(0.96f, glm::min(v, 255.0f));
}

/// A texel's colour: LightIntBand 12 plus 11 times a shade that is brighter
/// where the cloud is thin, plus the cloud sun colour (10) on the sunward
/// slopes of the noise. `nDotL` against a light 64 texels above the
/// texture (64 + 192 x storm), `sunFactor` = 1 - 0.75 x storm.
inline glm::vec3 cloudColor(float alpha, float nDotL, const glm::vec3& layerAmbient,
                            const glm::vec3& emissive, const glm::vec3& sun, float sunFactor) {
    const float a255 = std::round(alpha * 255.0f);
    const float shade = (std::floor((255.0f - a255) * 0.5f) + 64.0f) / 255.0f;
    glm::vec3 c = emissive * shade + layerAmbient;
    if (nDotL > 0.0f) c += sun * (nDotL * sunFactor);
    return glm::min(c, glm::vec3(1.0f));
}

}  // namespace wowee::rendering::stock_sky
