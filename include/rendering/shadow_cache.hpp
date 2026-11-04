#pragma once

/// The sun's shadow in two maps (E3, docs/enhancements-plan.md).
///
/// One map held everything - terrain, buildings, doodads and units - over the
/// whole shadow distance, and was drawn again every frame: at the Ultra
/// distance 2.8-4.3 ms of GPU and 0.7-1.5 ms of main-thread recording on the
/// Iris Xe, for a picture in which almost nothing moves.
///
/// The world map (terrain, buildings, doodads that stand still) is now drawn
/// once and kept. It is centred where the player was when it was drawn and
/// reaches a little past the shadow distance, so the player can walk a few
/// dozen yards before it has to be drawn again; the sun, which follows the
/// clock, has to turn by a fraction of a degree first. A redraw goes into the
/// second of two images, a part a frame (terrain, then buildings, then
/// doodads), and the two swap when it is done, so a redraw is never one long
/// frame. What changes in the world (a tile streams in, a doodad is placed or
/// removed) asks for a redraw too, no more than one every couple of seconds.
///
/// The unit map holds what moves - units, and doodads and buildings that have
/// moved lately (doors, lifts, boats) - near the player, at a finer texel,
/// drawn every frame. A pixel reads it only where a 64x64 mask over it says a
/// caster's footprint lies, so the ground at large pays one map's filter, as
/// before, and only the ground round a unit pays two.
///
/// Everything here is arithmetic, kept apart for tests/test_shadow_cache.cpp.

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace wowee {
namespace rendering {

/// Which casters a renderer's shadow pass draws.
enum class ShadowCasterSet : uint8_t {
    All,     ///< everything, the one-map pass (WOWEE_SHADOW_SPLIT=0)
    Static,  ///< what stands still: the world map
    Moving,  ///< what moves, or has lately: the unit map
};

/// How long something stays in the unit map after it last moved, in shadow
/// frames: a door that swung or a lift that stopped is put back in the world
/// map once it has been still this long, at the next redraw of that map.
inline constexpr uint32_t kShadowMoverHoldFrames = 600;

/// Whether an instance last moved at `movedFrame` (0: never) still counts as
/// moving at shadow frame `now`. Wraps with the counter.
inline bool shadowRecentlyMoved(uint32_t now, uint32_t movedFrame) {
    return movedFrame != 0 && now - movedFrame < kShadowMoverHoldFrames;
}

/// An orthographic sun projection.
struct ShadowProjection {
    glm::mat4 matrix{0.0f};
    glm::vec3 center{0.0f};  ///< the centre after snapping to the texel grid
    float halfExtent = 0.0f;
    float texelWorld = 0.0f;  ///< yards per texel
    float depthRange = 0.0f;  ///< yards from the near plane to the far one
};

/// The light's right and up axes for a direction (stable, independent of the
/// centre), as the one-map pass always built them.
inline void shadowLightAxes(const glm::vec3& sunDir, glm::vec3& right, glm::vec3& up,
                            glm::vec3& worldUp) {
    worldUp = glm::vec3(0.0f, 0.0f, 1.0f);
    if (std::abs(glm::dot(sunDir, worldUp)) > 0.99f) worldUp = glm::vec3(0.0f, 1.0f, 0.0f);
    right = glm::normalize(glm::cross(sunDir, worldUp));
    up = glm::normalize(glm::cross(right, sunDir));
}

/// The projection centred on `desiredCenter`, looking along `sunDir` (rays
/// pointing down), `halfExtent` yards each way across a `mapSize` map, with
/// the eye `lightDistance` back toward the sun and the far plane at
/// `farPlane`. The centre is snapped to the texel grid along the light's
/// right and up axes, so the same world point always lands on the same texel
/// whatever the centre - no shimmer while the player walks.
inline ShadowProjection shadowProjection(const glm::vec3& desiredCenter, const glm::vec3& sunDir,
                                         float halfExtent, uint32_t mapSize, float lightDistance,
                                         float farPlane) {
    constexpr float kNearPlane = 1.0f;
    ShadowProjection p;
    p.halfExtent = halfExtent;
    p.texelWorld = (2.0f * halfExtent) / static_cast<float>(std::max(mapSize, 1u));
    p.depthRange = farPlane - kNearPlane;
    glm::vec3 right, up, worldUp;
    shadowLightAxes(sunDir, right, up, worldUp);
    float dotR = glm::dot(desiredCenter, right);
    float dotU = glm::dot(desiredCenter, up);
    dotR = std::floor(dotR / p.texelWorld) * p.texelWorld;
    dotU = std::floor(dotU / p.texelWorld) * p.texelWorld;
    const float dotD = glm::dot(desiredCenter, sunDir);
    p.center = right * dotR + up * dotU + sunDir * dotD;
    const glm::mat4 view = glm::lookAt(p.center - sunDir * lightDistance, p.center, worldUp);
    glm::mat4 proj = glm::ortho(-halfExtent, halfExtent, -halfExtent, halfExtent, kNearPlane, farPlane);
    proj[1][1] *= -1.0f;  // Vulkan's y
    p.matrix = proj * view;
    return p;
}

/// The world map's projection for a shadow distance: the one-map pass's
/// depth (eye three distances back, far plane at six and a half), reaching
/// `extentScale` of the distance and `padYards` more, so the player can drift
/// before a redraw and keep moving while its parts are drawn.
inline ShadowProjection worldShadowProjection(const glm::vec3& center, const glm::vec3& sunDir,
                                              float shadowDistance, uint32_t mapSize,
                                              float extentScale, float padYards = 0.0f) {
    return shadowProjection(center, sunDir, shadowDistance * extentScale + padYards, mapSize,
                            shadowDistance * 3.0f, shadowDistance * 6.5f);
}

/// The unit map's reach either side of the player: units further than this
/// cast no map shadow. A fifth of the shadow distance, 32 to 90 yards: a
/// unit's shadow at ninety yards is a few pixels, and every unit drawn into
/// the map is main-thread time (bones and a draw per batch).
inline float nearShadowHalfExtent(float shadowDistance) {
    return std::clamp(shadowDistance * 0.2f, 32.0f, 90.0f);
}

/// The unit map's projection: its depth only has to hold units and what
/// moves near the player, so it is short, and the bias the shaders take from
/// it is finer.
inline ShadowProjection nearShadowProjection(const glm::vec3& center, const glm::vec3& sunDir,
                                             float halfExtent, uint32_t mapSize) {
    const float lightDistance = std::max(2.0f * halfExtent, 300.0f);
    return shadowProjection(center, sunDir, halfExtent, mapSize, lightDistance, 2.0f * lightDistance);
}

/// When the world map must be drawn again.
struct WorldShadowCachePolicy {
    float extentScale = 1.05f;      ///< the map reaches this much of the shadow distance
    float extentPad = 8.0f;         ///< and this many yards more: a redraw's three frames
    float driftFraction = 0.05f;    ///< of the distance, before a redraw starts
    float teleportFraction = 0.5f;  ///< of the distance: the old map is no use, draw now
    float maxAngleDegrees = 0.25f;  ///< the sun turning further than this
    double contentInterval = 2.0;   ///< seconds between redraws for world changes
};

/// What the kept map was drawn for.
struct WorldShadowCacheKey {
    bool valid = false;
    glm::vec3 center{0.0f};  ///< the player's position (unsnapped)
    glm::vec3 sunDir{0.0f};
    float shadowDistance = 0.0f;
    uint64_t content = 0;
    double drawnAt = 0.0;  ///< seconds, when the redraw started
};

enum class WorldShadowRedraw : uint8_t {
    None,
    Spread,  ///< a part a frame, the kept map meanwhile
    Now,     ///< all of it this frame: nothing usable is kept
};

/// Whether the map drawn for `key` will do for the player at `player` under a
/// sun along `sunDir`, at `shadowDistance`, with the world's content at
/// `content`, at time `now` (seconds).
///
/// The map covers the shadow distance round the player as long as the player
/// is within (extentScale - 1) of the distance of where it was drawn; the
/// redraw starts at driftFraction, below that, so the parts drawn over the
/// next frames land before the edge comes into reach.
inline WorldShadowRedraw worldShadowRedrawNeeded(const WorldShadowCacheKey& key,
                                                 const glm::vec3& player, const glm::vec3& sunDir,
                                                 float shadowDistance, uint64_t content, double now,
                                                 const WorldShadowCachePolicy& p = {}) {
    if (!key.valid) return WorldShadowRedraw::Now;
    if (std::abs(key.shadowDistance - shadowDistance) > 0.01f) return WorldShadowRedraw::Now;
    const float drift = glm::length(player - key.center);
    if (drift > p.teleportFraction * shadowDistance) return WorldShadowRedraw::Now;
    if (drift > p.driftFraction * shadowDistance) return WorldShadowRedraw::Spread;
    const float cosLimit = std::cos(glm::radians(p.maxAngleDegrees));
    if (glm::dot(glm::normalize(key.sunDir), glm::normalize(sunDir)) < cosLimit)
        return WorldShadowRedraw::Spread;
    if (content != key.content && now - key.drawnAt >= p.contentInterval)
        return WorldShadowRedraw::Spread;
    return WorldShadowRedraw::None;
}

/// The parts of a redraw, one a frame.
enum WorldShadowPart : uint8_t {
    kWorldShadowTerrain = 0,
    kWorldShadowBuildings = 1,
    kWorldShadowDoodads = 2,
    kWorldShadowPartCount = 3,
};

/// The cells of the unit map a caster's footprint can reach: 64x64 bits over
/// its uv square, row-major, 32 bits a word, four words a uvec4. The shaders
/// (shadow_filter.glsli) read the unit map only in a marked cell.
inline constexpr uint32_t kNearShadowMaskSide = 64;
inline constexpr uint32_t kNearShadowMaskWords = kNearShadowMaskSide * kNearShadowMaskSide / 32;

struct NearShadowMask {
    uint32_t words[kNearShadowMaskWords] = {};

    void clear() { std::memset(words, 0, sizeof(words)); }
    bool empty() const {
        for (uint32_t w : words)
            if (w) return false;
        return true;
    }
    bool testCell(uint32_t x, uint32_t y) const {
        const uint32_t bit = y * kNearShadowMaskSide + x;
        return (words[bit >> 5] >> (bit & 31u)) & 1u;
    }
    /// The cell a uv lands in, as the shader finds it.
    bool testUv(const glm::vec2& uv) const {
        if (uv.x < 0.0f || uv.y < 0.0f || uv.x >= 1.0f || uv.y >= 1.0f) return false;
        const auto cell = [](float v) {
            return std::min(static_cast<uint32_t>(v * kNearShadowMaskSide), kNearShadowMaskSide - 1);
        };
        return testCell(cell(uv.x), cell(uv.y));
    }
    /// Marks every cell the square round (uv, radius) touches.
    void addSquare(const glm::vec2& uv, float radius) {
        const float s = static_cast<float>(kNearShadowMaskSide);
        const float x0 = (uv.x - radius) * s, x1 = (uv.x + radius) * s;
        const float y0 = (uv.y - radius) * s, y1 = (uv.y + radius) * s;
        if (x1 < 0.0f || y1 < 0.0f || x0 >= s || y0 >= s) return;
        const int maxCell = static_cast<int>(kNearShadowMaskSide) - 1;
        const int cx0 = std::clamp(static_cast<int>(std::floor(x0)), 0, maxCell);
        const int cx1 = std::clamp(static_cast<int>(std::floor(x1)), 0, maxCell);
        const int cy0 = std::clamp(static_cast<int>(std::floor(y0)), 0, maxCell);
        const int cy1 = std::clamp(static_cast<int>(std::floor(y1)), 0, maxCell);
        for (int y = cy0; y <= cy1; ++y)
            for (int x = cx0; x <= cx1; ++x) {
                const uint32_t bit = static_cast<uint32_t>(y) * kNearShadowMaskSide + static_cast<uint32_t>(x);
                words[bit >> 5] |= 1u << (bit & 31u);
            }
    }
    /// Marks the footprint of a caster bounded by the sphere (centre, radius)
    /// under the projection: in an orthographic light, whatever the caster
    /// shades lies under its own outline in light space - a disc of its
    /// radius round its projected centre - grown by `marginTexels` of a
    /// `mapSize` map for the filter's reach and the receivers' offsets.
    void addCaster(const ShadowProjection& proj, uint32_t mapSize, const glm::vec3& centre,
                   float radius, float marginTexels = 4.0f) {
        const glm::vec4 clip = proj.matrix * glm::vec4(centre, 1.0f);
        const glm::vec2 uv = glm::vec2(clip.x, clip.y) * 0.5f + 0.5f;
        const float r = radius / (2.0f * proj.halfExtent) +
                        marginTexels / static_cast<float>(std::max(mapSize, 1u));
        addSquare(uv, r);
    }
};

}  // namespace rendering
}  // namespace wowee
