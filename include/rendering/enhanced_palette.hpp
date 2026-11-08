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

#include <glm/glm.hpp>

namespace wowee {
namespace rendering {
namespace enhanced_palette {

struct Trend {
    float luminance;
    float chroma;
};

inline constexpr Trend kDirect{1.03f, 0.81f};
inline constexpr Trend kAmbient{1.15f, 1.08f};
inline constexpr Trend kSkyTop{1.09f, 1.12f};
inline constexpr Trend kSkyMiddle{1.19f, 1.28f};
inline constexpr Trend kSkyFog{1.30f, 1.35f};
inline constexpr Trend kSun{1.11f, 0.72f};

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

}  // namespace enhanced_palette
}  // namespace rendering
}  // namespace wowee
