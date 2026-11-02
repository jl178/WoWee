#pragma once

/// The stock 3.3.5a (12340) client's weather amounts and full-screen glow,
/// as pure maths so they can be tested without a device. Addresses are
/// Wow.exe's; the write-up is docs/re/world-lighting.md (weather and glow).

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace wowee::rendering::stock_screen_fx {

// ---------------------------------------------------------------------------
// Weather (MapWeather.cpp)
// ---------------------------------------------------------------------------

/// How much of the falling particles a SMSG_WEATHER grade asks for
/// (0x784850): nothing below a quarter, all of it at one -
/// (grade - 0.25) * 4/3 (0x9e8ce4, 0x9f987c), floored at zero.
inline float particleLevel(float grade) {
    return std::max(0.0f, (std::clamp(grade, 0.0f, 1.0f) - 0.25f) * (4.0f / 3.0f));
}

/// The weatherDensity CVar (0x787780, default "2"), as the factor 0x784040
/// writes to 0xadf1f8: 0.10, 0.33, 0.66, 1.0.
inline float densityScale(int level) {
    static constexpr std::array<float, 4> kScale{0.10f, 0.33f, 0.66f, 1.0f};
    return kScale[static_cast<size_t>(std::clamp(level, 0, 3))];
}
inline constexpr int kDefaultDensityLevel = 2;

/// How far the light leans toward Light.dbc's storm set (DayNight 0xd38b4c,
/// times 4 into 0xd38b88): the weather grade capped at a quarter (0x7846a0),
/// times four, times Weather.dbc's fourth field (1.0 on every rain and snow
/// row, 0 on the coloured sandstorms 103 and 105-107). Full storm light at a
/// grade of 0.25 already.
inline float stormLight(float grade, float weatherScale = 1.0f) {
    return std::min(std::clamp(grade, 0.0f, 1.0f), 0.25f) * 4.0f * weatherScale;
}

/// How long a change of grade takes (0x784850): the particles move at ten
/// seconds per unit of grade, the storm light at ten seconds per unit of its
/// quarter-capped value times four - so from clear to a full storm the sky
/// takes ten seconds. The 0.001 keeps a no-op change from dividing by zero.
inline float particleTransitionSeconds(float fromGrade, float toGrade) {
    return (std::abs(toGrade - fromGrade) + 0.001f) * 10.0f;
}
inline float stormTransitionSeconds(float fromLight, float toLight) {
    // fromLight/toLight are the quarter-capped grades (0..0.25).
    return (std::abs(toLight - fromLight) * 4.0f + 0.001f) * 10.0f;
}

/// Particles per second the emitters ask for at a particle level p
/// (0x7840b0 rain, 0x7844f0 snow, 0x784580 sand), shader path
/// (useWeatherShaders=1): 35000, 14000, 32000 times the density factor.
enum class WeatherKind { Rain, Snow, Sand };
inline float emitRate(WeatherKind kind, float level, float density) {
    const float base = kind == WeatherKind::Rain ? 35000.0f
                     : kind == WeatherKind::Snow ? 14000.0f : 32000.0f;
    return base * density * level;
}

/// The mist sheets (SnowMist01.blp, 0x78c420/0x786e10) spawned per second:
/// rain and snow only past half the particle level, (p - 0.5) * 2, times 38
/// (rain) or 48 (snow); sand all along, times 64. At most 128 alive.
inline float mistRate(WeatherKind kind, float level, float density) {
    if (kind == WeatherKind::Sand) return 64.0f * density * level;
    const float k = kind == WeatherKind::Rain ? 38.0f : 48.0f;
    return std::max(0.0f, (level - 0.5f) * 2.0f) * density * k;
}
inline constexpr int kMaxMist = 128;
/// A mist sheet is 12 yards across, faded out within 6 yards of the camera
/// and in fully by 18: alpha *= 1 - clamp(1.5 - d/12, 0, 1) (0xa41b18,
/// 0xa37f38).
inline float mistDistanceFade(float distance) {
    return 1.0f - std::clamp(1.5f - distance / 12.0f, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// Full-screen glow (EffectGlow.cpp, PassGlow.cpp, ffxglow.bls) and the ghost
// world (FFXEffects.cpp, ffxdeath.bls)
// ---------------------------------------------------------------------------

/// The glow strength the quad's vertex alpha carries (0x4f8770): the light's
/// LightParams.Glow (DNLight [0x16], base+0x12c) truncated to a byte.
inline float glowAmount(float lightParamsGlow) {
    return std::floor(std::clamp(lightParamsGlow, 0.0f, 1.0f) * 255.0f) / 255.0f;
}

/// How far the picture itself is swapped for its blur (vertex blue,
/// 0x4f8770): 0x54/255 with the camera in liquid, the drunk level (0..1)
/// when that is more, else zero.
inline float glowBlend(bool cameraInLiquid, float drunk) {
    const float water = cameraInLiquid ? 84.0f / 255.0f : 0.0f;
    return std::max(water, std::clamp(drunk, 0.0f, 1.0f));
}

/// The blur: a 4x4 box down to quarter size (FFXBox4, taps at -1.5/+0.5,
/// 0xa94280), then FFXGauss4 across and down - four bilinear taps at
/// -2.5, -0.5, +0.5, +2.5 quarter texels (0xa94268, 0x9e8d30) weighted
/// 1/8, 3/8, 3/8, 1/8.
inline constexpr std::array<float, 4> kGaussOffsets{-2.5f, -0.5f, 0.5f, 2.5f};
inline constexpr std::array<float, 4> kGaussWeights{0.125f, 0.375f, 0.375f, 0.125f};

/// ffxglow.bls: lerp(scene, blur, blend) + blur^2 * amount.
inline glm::vec3 glowCombine(const glm::vec3& scene, const glm::vec3& blur,
                             float amount, float blend) {
    return glm::mix(scene, blur, blend) + blur * blur * amount;
}

/// ffxdeath.bls, with the tint 0x7e87b0 hands it: (0x53, 0x93, 0xa8). The
/// blue weight is the shader's 0.144, not Rec.601's 0.114.
inline constexpr glm::vec3 kDeathTint{83.0f / 255.0f, 147.0f / 255.0f, 168.0f / 255.0f};
inline glm::vec3 deathCombine(const glm::vec3& scene, const glm::vec3& blur, float amount) {
    const glm::vec3 lit = scene + blur * blur * amount;
    const float grey = std::clamp(glm::dot(lit, glm::vec3(0.299f, 0.587f, 0.144f)), 0.0f, 1.0f);
    const float t = std::clamp(4.0f * grey * (1.0f - grey), 0.0f, 1.0f);
    return glm::vec3(grey) + kDeathTint * t;
}

}  // namespace wowee::rendering::stock_screen_fx
