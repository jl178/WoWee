#pragma once

/// The weather enhancements' rules (docs/enhancements-plan.md E23-E31, the
/// "Enhancements: Weather" page): how much more the "Weather intensity" slider
/// draws, the client-only weather preview, which zones are cold, dry or
/// firefly country, when lightning strikes and its thunder follows, how fast
/// snow settles and how thick a sandstorm's haze is.
///
/// None of this is stock. The stock client draws the server's rain, snow and
/// sand, their mist and the cold breath (Weather, stock_screen_fx.hpp,
/// stock_weather.hpp) and nothing else; every rule here returns
/// "nothing" at the row's stock value, and the renderer asks none of them
/// while the rows are off (Renderer::updateWeatherFx).
///
/// Pure and header-only, for Weather, Renderer and test_weather_fx.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wowee {
namespace rendering {
namespace weather_fx {

// ---------------------------------------------------------------- Intensity

/// "Weather intensity": 1 is stock, the top of the slider about two and a
/// half times as many drops and flakes.
inline constexpr float kStockIntensity = 1.0f;
inline constexpr float kMaxIntensity = 2.5f;

/// How many times the stock count of drops or flakes to keep in the air. Only
/// ever more than stock - the slider's bottom is the stock rain - and never
/// past its top, so the vertex buffer's size is bounded.
inline float countScale(float intensity) {
    if (!(intensity > kStockIntensity)) return 1.0f;
    return std::min(intensity, kMaxIntensity);
}

/// The particle target: the stock count (Weather::update: the cap times the
/// type's share, the stock level and the Weather Detail density) times the
/// slider. At intensity 1 this is the stock expression exactly.
inline int particleTarget(int stockCap, float typeScale, float level, float density, float intensity) {
    const float stock = static_cast<float>(stockCap) * typeScale * level * density;
    return static_cast<int>(stock * countScale(intensity));
}

/// The rest of the weather - mist sheets, splashes, a sandstorm's haze - is
/// lifted less than the count, on the square root, so the strongest setting
/// is a heavier rain rather than a wall.
inline float sideScale(float intensity) { return std::sqrt(countScale(intensity)); }

// ------------------------------------------------------------------ Preview

/// What the client-only preview forces: a Weather.dbc row (the SMSG_WEATHER
/// state, numbered as the server's WeatherState) and a grade. Never sent to
/// the server; the renderer reads it in place of the server's weather while
/// it is on, for the picture, the storm light and the weather's sound.
struct Forced {
    bool active = false;
    uint32_t state = 0;
    float grade = 0.0f;
};

inline constexpr int kPreviewCount = 11;
/// The last choice: always clear - the server's rain or snow is not drawn,
/// heard or lit on this client.
inline constexpr int kPreviewClear = 10;
inline constexpr const char* kPreviewChoices =
    "Off (the server's weather)|Light rain|Rain|Heavy rain|Thunderstorm|"
    "Light snow|Snow|Heavy snow|Light sandstorm|Sandstorm|Always clear (sunny)";

/// The preview choice's weather. 0 (and anything out of range) is off.
inline Forced previewWeather(int choice) {
    // Weather.dbc: 3-5 rain (light, medium, heavy), 86 thunderstorm, 6-8 snow,
    // 41/42/22 sandstorm light, medium, heavy (ambience 8556-8558).
    struct Row { uint32_t state; float grade; };
    static constexpr Row kRows[kPreviewCount] = {
        {0, 0.0f}, {3, 0.45f}, {4, 0.7f}, {5, 1.0f}, {86, 1.0f},
        {6, 0.45f}, {7, 0.7f}, {8, 1.0f}, {41, 0.5f}, {22, 1.0f},
        {0, 0.0f},  // kPreviewClear
    };
    Forced f;
    if (choice <= 0 || choice >= kPreviewCount) return f;
    f.active = true;
    f.state = kRows[choice].state;
    f.grade = kRows[choice].grade;
    return f;
}

// ------------------------------------------------------------------ Climate

/// What a zone's air is like, for the effects the server's weather does not
/// carry. Keyed on the AreaTable zone (the top-level area), from the zones'
/// own art: snowfields, deserts, and the woods and marshes where fireflies
/// would be at home.
enum Climate : uint8_t {
    kTemperate = 0,
    kCold      = 1u << 0,  ///< snowfields (the breath follows AreaTable instead, stock_weather)
    kCool      = 1u << 1,  ///< cold nights
    kDry       = 1u << 2,  ///< desert and badlands: a sandstorm's country
    kFireflies = 1u << 3,  ///< woods, meadows and marshes: fireflies at night
};

inline uint8_t climateForZone(uint32_t zoneId) {
    switch (zoneId) {
        // Snowfields: Dun Morogh, Alterac Mountains and Valley, Winterspring;
        // Northrend but Sholazar Basin.
        case 1: case 36: case 2597: case 618:
        case 3537: case 495: case 65: case 66: case 67: case 210: case 2817:
        case 4197: case 4742: case 4395:
            return kCold;
        // Grizzly Hills: cold nights under the pines.
        case 394:
            return kCool | kFireflies;
        // Deserts and badlands.
        case 440: case 1377: case 3: case 51: case 14: case 17: case 400:
        case 405: case 3483: case 3522:
            return kDry;
        // Woods, meadows and marshes.
        case 12: case 10: case 331: case 141: case 148: case 357: case 3430:
        case 267: case 44: case 38: case 33: case 490: case 3711: case 493:
        case 3519: case 3518: case 8: case 11: case 15: case 47: case 3524:
        case 215: case 45:
            return kFireflies;
        default:
            return kTemperate;
    }
}

/// How dark it is for the night effects, 0 by day to 1 in the dead of night,
/// from the sky's clock in hours: dusk from 19:30 to 21:00, dawn from 5:00
/// to 6:30.
inline float nightAmount(float hours) {
    float h = std::fmod(hours, 24.0f);
    if (h < 0.0f) h += 24.0f;
    auto ramp = [](float x, float a, float b) { return std::clamp((x - a) / (b - a), 0.0f, 1.0f); };
    if (h >= 12.0f) return ramp(h, 19.5f, 21.0f);
    return 1.0f - ramp(h, 5.0f, 6.5f);
}

/// How many fireflies to keep around the camera, as a share of the most:
/// firefly country, at night, out of doors, not in rain or snow.
inline float fireflyAmount(uint8_t climate, float hours, bool falling, bool indoors) {
    if (!(climate & kFireflies) || falling || indoors) return 0.0f;
    return nightAmount(hours);
}

// --------------------------------------------------------------- Lightning

/// Only a thunderstorm (Weather.dbc 86) throws lightning, and only once the
/// rain is falling (the stock particle level is above 0 past a quarter grade).
inline bool stormHasLightning(bool thunderstorm, float grade) {
    return thunderstorm && grade > 0.25f;
}

/// Seconds to the next strike: about every 24 s at the storm's lightest and
/// every 8 s at its heaviest, `roll` (0-1) spreading each from half to one
/// and a half of that. The intensity slider shortens it a little.
inline float strikeInterval(float grade, float roll, float intensity = 1.0f) {
    const float g = std::clamp((grade - 0.25f) / 0.75f, 0.0f, 1.0f);
    const float mean = 24.0f - 16.0f * g;
    return mean * (0.5f + std::clamp(roll, 0.0f, 1.0f)) / sideScale(intensity);
}

/// Where a strike falls, yards from the camera: mostly far off, now and then
/// close. `roll` 0-1.
inline float strikeDistance(float roll) {
    const float r = std::clamp(roll, 0.0f, 1.0f);
    return 250.0f + 2750.0f * r * r;
}

/// Sound's speed in air, 343 m/s, in yards a second.
inline constexpr float kSoundSpeedYd = 375.0f;
inline float thunderDelay(float distanceYd) { return std::max(distanceYd, 0.0f) / kSoundSpeedYd; }

/// How much a strike lights the land, 0-1: a near one fully, one at the far
/// edge of the storm a quarter.
inline float flashStrength(float distanceYd) {
    const float d = std::clamp((distanceYd - 250.0f) / 2750.0f, 0.0f, 1.0f);
    return 1.0f - 0.75f * std::sqrt(d);
}

/// The flash over time since the strike, 0-1: a first pulse, a dark gap and
/// a second flicker, as a stroke and its return strokes look, gone by 0.6 s.
/// `seed` 0-1 moves the second pulse.
inline float flashEnvelope(float t, float seed) {
    if (t < 0.0f) return 0.0f;
    auto pulse = [](float x, float at, float rise, float fall) {
        if (x < at) return 0.0f;
        const float u = x - at;
        if (u < rise) return u / rise;
        return std::exp(-(u - rise) / fall);
    };
    const float second = 0.14f + 0.12f * std::clamp(seed, 0.0f, 1.0f);
    const float v = std::max(pulse(t, 0.0f, 0.02f, 0.05f), 0.7f * pulse(t, second, 0.02f, 0.09f));
    return t > 0.9f ? 0.0f : std::clamp(v, 0.0f, 1.0f);
}

/// The thunder's level for a strike this far: a near crack loud, a far
/// rumble quiet.
inline float thunderVolume(float distanceYd) {
    const float d = std::clamp((distanceYd - 250.0f) / 2750.0f, 0.0f, 1.0f);
    return 0.9f - 0.6f * d;
}

/// A near strike cracks (the sharper sample); past this it only rumbles.
inline constexpr float kCrackDistance = 700.0f;

// --------------------------------------------------------------- Snow cover

/// Snow settling on the ground and roofs while it snows: how white it heads
/// for under this snow (0 not snowing), and the ease toward it - a few
/// minutes to settle, longer to melt.
struct SnowCover {
    static constexpr float kSettleSeconds = 150.0f;   ///< time constant while snowing
    static constexpr float kMeltSeconds = 300.0f;    ///< time constant after
    static constexpr float kEpsilon = 0.002f;
};

inline float snowCoverTarget(bool snowing, float grade) {
    if (!snowing) return 0.0f;
    const float level = std::max(0.0f, (std::clamp(grade, 0.0f, 1.0f) - 0.25f) * (4.0f / 3.0f));
    if (level <= 0.0f) return 0.0f;
    return 0.45f + 0.55f * level;
}

inline float easeSnowCover(float current, float target, float dt) {
    const float tau = target > current ? SnowCover::kSettleSeconds : SnowCover::kMeltSeconds;
    const float k = 1.0f - std::exp(-std::max(dt, 0.0f) / tau);
    float next = current + (target - current) * k;
    if (target <= 0.0f && next < SnowCover::kEpsilon) next = 0.0f;
    return std::clamp(next, 0.0f, 1.0f);
}

// ---------------------------------------------------------------- Sandstorm

/// A sandstorm's haze: the fog's start and end pulled in by this factor at
/// the storm's level (0 none .. 1 heaviest). Its colour stays Light.dbc's -
/// the stock sand is drawn in the fog colour too - so the zone's palette is
/// the zone's.
inline float sandFogScale(float level, float intensity = 1.0f) {
    const float l = std::clamp(level, 0.0f, 1.0f);
    const float k = std::clamp(0.40f * sideScale(intensity), 0.0f, 0.55f);
    return 1.0f - k * l;
}

// -------------------------------------------------------------- Mist sheets

/// How many of the row's taller distant sheets to keep in the air, on top of
/// the stock mist (stock_weather.hpp): rain and snow past half their level,
/// as the stock mist's spawn rate (level - 0.5) * 2, sand at any level.
inline int mistSheetTarget(bool sand, float level, float intensity, int cap) {
    const float l = std::clamp(level, 0.0f, 1.0f);
    const float share = sand ? l : std::max(0.0f, (l - 0.5f) * 2.0f);
    return static_cast<int>(static_cast<float>(cap) * std::min(1.0f, share * sideScale(intensity) * 0.6f));
}

/// A sheet's opacity by its distance from the eye, as the stock mist fades
/// (0x786e10): nothing inside 6 yd, all of it past 18.
inline float mistDistanceFade(float distanceYd) {
    return 1.0f - std::clamp(1.5f - distanceYd / 12.0f, 0.0f, 1.0f);
}

}  // namespace weather_fx
}  // namespace rendering
}  // namespace wowee
