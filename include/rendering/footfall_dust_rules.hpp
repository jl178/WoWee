#pragma once

/// "Dust kicked up" (docs/enhancements-plan.md E21): what a footfall on dry
/// ground throws up, for players, creatures and mounts alike.
///
/// The footfalls are the models' own $FSD events - the ones that already play
/// each unit's footstep sound through FootstepTerrainLookup - and the ground is
/// read the way that sound reads it: TerrainType.SoundID of the dominant layer
/// (1 dirt, 8 sand), water and building floors first. Only dirt and sand
/// raise dust; grass, leaves, snow, mud, stone, wood, metal and water never
/// do, nor any ground while it rains or snows. How much is thrown up follows
/// the walker's speed between footfalls and its size: nothing standing or
/// shuffling, a faint wisp at a walk, a small puff at a run, a little more
/// under a mount.
///
/// Pure functions, for Renderer::updateFootfallDust and
/// test_phase2_enhancements. The renderer is FootfallDust.

#include <glm/glm.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wowee {
namespace rendering {
namespace footfall_dust {

enum class Ground : uint8_t { None, Dirt, Sand };

/// Footfalls further from the camera than this raise nothing: a puff a few
/// pixels wide would only be noise.
inline constexpr float kMaxDistance = 35.0f;

/// The ground's dust from its footstep type (TerrainType.SoundID), or, for a
/// layer with no ground effect, from its texture's name. `inWater` and
/// `onBuildingFloor` come first, as they do for the footstep sound.
inline Ground groundFor(std::optional<uint32_t> terrainSoundId, std::string_view texture,
                        bool inWater, bool onBuildingFloor) {
    if (inWater || onBuildingFloor) return Ground::None;
    if (terrainSoundId) {
        if (*terrainSoundId == 1) return Ground::Dirt;
        if (*terrainSoundId == 8) return Ground::Sand;
        return Ground::None;
    }
    std::string t(texture);
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto has = [&](const char* token) { return t.find(token) != std::string::npos; };
    // Anything wet, green or cold first: a "dirt and grass" blend is grass.
    for (const char* no : {"grass", "moss", "leaf", "snow", "ice", "mud", "swamp", "marsh",
                           "water", "flower", "fern"}) {
        if (has(no)) return Ground::None;
    }
    if (has("sand") || has("desert") || has("dune")) return Ground::Sand;
    for (const char* yes : {"dirt", "dust", "road", "path", "trail", "barren", "cracked", "dry"}) {
        if (has(yes)) return Ground::Dirt;
    }
    return Ground::None;
}

/// The dust's base colour (before the scene's light): a pale dry earth, and
/// a paler sand - dust in the air reads lighter than the ground it came off.
inline glm::vec3 colourFor(Ground g) {
    return g == Ground::Sand ? glm::vec3(0.88f, 0.81f, 0.66f) : glm::vec3(0.72f, 0.64f, 0.52f);
}

/// The walker's ground speed from its last footfall, or 0 when there is none
/// to compare (first step, a pause of over a second and a half, a teleport).
inline float stepSpeed(const glm::vec3& prevPos, float prevTime, const glm::vec3& pos, float time) {
    const float dt = time - prevTime;
    if (dt < 0.05f || dt > 1.5f) return 0.0f;
    const float d = glm::length(glm::vec2(pos.x - prevPos.x, pos.y - prevPos.y));
    const float v = d / dt;
    return v > 60.0f ? 0.0f : v;  // a jump in position, not a stride
}

/// What one footfall throws up.
struct Puff {
    int sprites = 0;     ///< 0: nothing
    float alpha = 0.0f;  ///< peak opacity of each sprite
    float size = 0.0f;   ///< starting radius, yd; each grows to about 3.5x
    float life = 0.0f;   ///< seconds
};

/// The puff for a footfall at `speed` yd/s by a body of bounding radius
/// `bodyRadius` yd. `roll` is a uniform 0-1 draw, which thins a walk's dust
/// to every other step or so.
inline Puff puffFor(float speed, float bodyRadius, float roll) {
    Puff p;
    // A walk is about 2.5 yd/s, a run 7, a mount 14 or more.
    const float k = std::clamp((speed - 1.8f) / 5.2f, 0.0f, 1.0f);
    if (k <= 0.0f) return p;
    if (k < 0.3f && roll > 0.5f) return p;  // walking: a wisp on some steps
    const float size = std::clamp(bodyRadius / 1.1f, 0.7f, 2.2f);
    const float fast = std::clamp((speed - 7.0f) / 7.0f, 0.0f, 1.0f);  // past a run
    p.sprites = k < 0.3f ? 1 : (fast > 0.5f ? 3 : 2);
    p.alpha = 0.22f + 0.18f * k + 0.06f * fast;
    p.size = 0.18f * size * (1.0f + 0.25f * fast);
    p.life = 0.9f + 0.5f * k;
    return p;
}

/// A sprite's opacity through its life (t 0-1): in over the first tenth,
/// then thinning out as it spreads.
inline float alphaOverLife(float t) {
    if (t <= 0.0f || t >= 1.0f) return 0.0f;
    if (t < 0.1f) return t / 0.1f;
    const float u = (t - 0.1f) / 0.9f;
    return (1.0f - u) * (1.0f - u);
}

}  // namespace footfall_dust
}  // namespace rendering
}  // namespace wowee
