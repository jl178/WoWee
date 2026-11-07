#pragma once

/// The stock client's weather rules that are not the rain and snow
/// particles themselves (those are in Weather and stock_screen_fx.hpp):
/// where the weather is drawn, the mist sheets every weather draws, the sand
/// grains and when a unit breathes out in the cold. Traced from Wow.exe
/// 3.3.5a build 12340 (MapWeather.cpp); docs/re/weather-sky.md has the
/// addresses.
///
/// Pure and header-only, for Weather, Renderer and test_weather_fx.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wowee {
namespace rendering {
namespace stock_weather {

// ------------------------------------------------------------ Where it draws

/// Whether the weather is drawn this frame (docs/re/weather-sky.md 1). Stock
/// draws it (0x78ae20, 0x78ba60, 0x78c3e0) unless 0xadf580 < 0, and that is
/// the sky's own test (0x79a870): the camera's group (0x795d40, a ray down)
/// is an exterior one - outside - or the portal walk from its interior group
/// opened the outdoors. A camera in the box of an exterior group (a town's
/// street) is outside. Without the portal walk (WOWEE_WMO_PORTALS=0) the
/// camera inside an interior group's box stands in for "sealed".
inline bool weatherDrawsFromCamera(bool portalWalkActive, bool outdoorSealed, bool inInteriorGroup) {
    if (portalWalkActive) return !outdoorSealed;
    return !inInteriorGroup;
}

/// IsOutdoors (0x71B7F0 -> 0x77FBF0): in a map object group, outdoors when
/// that group has the exterior flag 0x8 (WMOAreaTable's flags 4 / 2 can
/// force it, not read here); out of one, the area's AreaTable flags: 0x4000000
/// outdoors, else 0x2000000 indoors, else outdoors.
inline bool playerOutdoors(bool inWmoGroup, bool groupLacksExterior, uint32_t areaFlags) {
    if (inWmoGroup) return !groupLacksExterior;
    if (areaFlags & 0x4000000u) return true;
    return (areaFlags & 0x2000000u) == 0;
}

// -------------------------------------------------------------- Mist sheets

/// The mist every weather draws (0x78c420, drawn by 0x786e10): camera-facing
/// squares 12 yd on a side (0xa1047c) in the fog colour (DayNight +0x8c),
/// alpha-blended, that blow back past the player.
struct MistSpec {
    const char* texture;   ///< SnowMist01 for rain and snow, WeatherMistGrainy01 for sand
    float speed;           ///< yd/s (0x34), spread +-half of speedSpread (0x38)
    float speedSpread;
    float shaderRate;      ///< sheets a second at level 1, density 1 (0x7840b0 / 0x7844f0 / 0x784580)
    bool sand;             ///< sand's rate is level * rate, rain's and snow's (level - 0.5) * 2 * rate
};
inline constexpr MistSpec kRainMist{"Textures\\Weather\\SnowMist01.blp", 5.0f, 1.2f, 38.0f, false};
inline constexpr MistSpec kSnowMist{"Textures\\Weather\\SnowMist01.blp", 9.0f, 3.0f, 48.0f, false};
inline constexpr MistSpec kSandMist{"Textures\\Weather\\WeatherMistGrainy01.blp", 15.0f, 4.5f, 64.0f, true};

inline constexpr float kMistSize = 12.0f;          ///< 0xa1047c, the side of the square
inline constexpr float kMistBoxXY = 44.0f;         ///< 0xa3ec4c, spawn box (centred), yd
inline constexpr float kMistBoxZ = 25.0f;          ///< 0xa2e868
inline constexpr float kMistYaw = -1.57f;          ///< 0xa3ec48: across the heading, back
inline constexpr float kMistYawSpread = 0.349066f; ///< 0xa3ea3c (20 degrees, +-10)
inline constexpr float kMistRise = 1.0f / 3.0f;    ///< 0xa12098 yd/s up, +-1/60 (0xa19ce0)
inline constexpr float kMistLife = 2.7f;           ///< 0xa3ea44 s, +-0.15 (0x9e2ebc 0.3)
inline constexpr float kMistLifeSpread = 0.3f;
inline constexpr float kMistFade = 0.4f;           ///< 0x9f98d8: fade in and out, s
inline constexpr float kMistLead = 1.5f;           ///< 0xa41b18: spawned 1.5 s of travel upwind
inline constexpr float kMistAccel = 3.33333f;      ///< 0xa3ea48: +-half, yd/s^2 on every axis
inline constexpr int kMistCap = 128;               ///< 0x80

/// Sheets spawned a second (0x7840b0, 0x7844f0, 0x784580): `level` is the
/// particle level (grade - 0.25) * 4/3, `density` the weatherDensity factor.
inline float mistRate(const MistSpec& m, float level, float density) {
    const float l = std::max(level, 0.0f);
    if (m.sand) return m.shaderRate * density * l;
    return std::max(0.0f, (l - 0.5f) * 2.0f) * density * m.shaderRate;
}

/// Opacity of a sheet `age` seconds into a `life`-second life (0x786e10): in
/// over 0.4 s and out over the last 0.4 s.
inline float mistLifeAlpha(float age, float life) {
    const float in = std::clamp(age / kMistFade, 0.0f, 1.0f);
    const float out = std::clamp((life - age) / kMistFade, 0.0f, 1.0f);
    return in * out;
}

/// Opacity of a sheet's corner `distance` yd from the eye (0x786e10,
/// 0xa41b18, 0xa37f38): none inside 6 yd, all past 18.
inline float mistNearFade(float distance) {
    return 1.0f - std::clamp(1.5f - distance / 12.0f, 0.0f, 1.0f);
}

// --------------------------------------------------------------- Sand grains

/// The sand (0x784580 rates, 0x785ea0 spawn, sand.bls draw): point sprites
/// in the fog colour that blow at the player from 34-40 yd ahead.
inline constexpr float kSandShaderRate = 32000.0f; ///< 0xa3e9d8 a second at level 1, density 1
inline constexpr float kSandBoxX = 40.0f;          ///< 0xa0ff3c: ahead, from 0.85 to 1 of it (0x9f23d0, 0x9f23d4)
inline constexpr float kSandBoxY = 40.0f;
inline constexpr float kSandBoxZ = 25.0f;          ///< 0xa2e868
inline constexpr float kSandAheadMin = 0.85f;
inline constexpr float kSandSpeed = 18.6667f;      ///< 0xa3ea38, +-1/3 (0x9fc830)
inline constexpr float kSandSpeedSpread = 0.666667f;
inline constexpr float kSandYaw = -1.57f;          ///< 0xa2419c, as the mist's
inline constexpr float kSandYawSpread = 0.349066f;
inline constexpr float kSandRise = 0.833333f;      ///< 0xa3ea34, +-1/12 (0xa33b7c)
inline constexpr float kSandRiseSpread = 0.166667f;
inline constexpr float kSandLife = 3.2f;           ///< 0xa3ea30, +-0.15
inline constexpr float kSandLifeSpread = 0.3f;
inline constexpr float kSandLead = 1.75f;          ///< 0xadf22c: the player's velocity times this

/// Grains spawned a second (0x784580).
inline float sandRate(float level, float density) {
    return kSandShaderRate * density * std::max(level, 0.0f);
}

/// sand.bls: a grain `distance` yd away is clamp(1.5 - 0.05 d) of its full
/// size (c9 = -0.05, 1.5), which is 0.0025 of the viewport's width in pixels
/// (0xa3ebec): full to 10 yd, gone at 30.
inline float sandSizeScale(float distance) {
    return std::clamp(1.5f - 0.05f * distance, 0.0f, 1.0f);
}
inline constexpr float kSandPixelsPerWidth = 0.0025f;

/// sand.bls alpha: in over the first 0.2 s, out over the last 0.2 s.
inline float sandAlpha(float age, float life) {
    const float in = std::clamp(5.0f * age, 0.0f, 1.0f);
    const float out = std::clamp(5.0f * (life - age), 0.0f, 1.0f);
    return in * out;
}

// ------------------------------------------------------------------ Heading

/// The direction the mist and sand blow from (0x78d170 -> weather +0x18c):
/// the player's direction of travel, or its facing when it moves slower than
/// a yard a second (0x9e1130).
inline float heading(float velX, float velY, float facing) {
    if (std::sqrt(velX * velX + velY * velY) < 1.0f) return facing;
    float h = std::atan2(velY, velX);
    if (h < 0.0f) h += 6.2831853f;
    return h;
}

// ------------------------------------------------------------------ Breath

/// Whether a unit in this area breathes out in the cold (0x78F1F0): the
/// area's AreaTable flag 0x1, or its parent zone's when the area lacks 0x2.
/// Stock asks it every 10 s (0x71FA90) and the model's $BTH event then plays
/// "HARDCODED Breath Cold" (SpellVisualEffectName 107,
/// Particles\ColdBreath.m2) at attachment 17 (0x732650, 0x6F9260).
inline bool areaIsCold(uint32_t areaFlags, bool hasParent, uint32_t parentFlags) {
    const uint32_t f = ((areaFlags & 0x2u) == 0 && hasParent) ? parentFlags : areaFlags;
    return (f & 0x1u) != 0;
}
inline constexpr const char* kColdBreathModel = "Particles\\ColdBreath.m2";
inline constexpr float kColdRecheckSeconds = 10.0f;

}  // namespace stock_weather
}  // namespace rendering
}  // namespace wowee
