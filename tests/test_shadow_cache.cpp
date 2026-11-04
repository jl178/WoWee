// The sun's shadow in two maps (rendering/shadow_cache.hpp): when the kept
// world map is drawn again, that it always covers the shadow distance round
// the player between redraws, that its texels do not crawl as the player
// walks, and that the unit map's mask marks every texel a caster can shade.

#include <catch_amalgamated.hpp>
#include "rendering/shadow_cache.hpp"

#include <random>

using namespace wowee::rendering;

namespace {

glm::vec3 noonSun() { return glm::normalize(glm::vec3(0.3f, -0.4f, -0.85f)); }

WorldShadowCacheKey keyAt(const glm::vec3& c, float dist = 500.0f, uint64_t content = 7, double t = 100.0) {
    return WorldShadowCacheKey{.valid = true, .center = c, .sunDir = noonSun(), .shadowDistance = dist,
                               .content = content, .drawnAt = t};
}

}  // namespace

TEST_CASE("World shadow map: standing still never redraws", "[shadow_cache]") {
    const glm::vec3 p(-9465.0f, 47.0f, 56.6f);
    const auto key = keyAt(p);
    for (double t = 100.0; t < 1000.0; t += 13.7)
        REQUIRE(worldShadowRedrawNeeded(key, p, noonSun(), 500.0f, 7, t) == WorldShadowRedraw::None);
}

TEST_CASE("World shadow map: redraw reasons", "[shadow_cache]") {
    const glm::vec3 p(100.0f, 200.0f, 30.0f);
    const WorldShadowCachePolicy pol{};
    const auto key = keyAt(p);
    // Nothing kept yet, a new distance, a teleport: at once.
    REQUIRE(worldShadowRedrawNeeded(WorldShadowCacheKey{}, p, noonSun(), 500.0f, 7, 0.0) == WorldShadowRedraw::Now);
    REQUIRE(worldShadowRedrawNeeded(key, p, noonSun(), 350.0f, 7, 101.0) == WorldShadowRedraw::Now);
    REQUIRE(worldShadowRedrawNeeded(key, p + glm::vec3(400.0f, 0, 0), noonSun(), 500.0f, 7, 101.0) ==
            WorldShadowRedraw::Now);
    // A walk: spread over frames once past the drift, not before.
    const float d = pol.driftFraction * 500.0f;
    REQUIRE(worldShadowRedrawNeeded(key, p + glm::vec3(d * 0.99f, 0, 0), noonSun(), 500.0f, 7, 101.0) ==
            WorldShadowRedraw::None);
    REQUIRE(worldShadowRedrawNeeded(key, p + glm::vec3(0, d * 1.01f, 0), noonSun(), 500.0f, 7, 101.0) ==
            WorldShadowRedraw::Spread);
    // The sun: a tenth of a degree is kept, half a degree is not.
    const auto turned = [](float deg) {
        const float a = glm::radians(deg);
        const glm::vec3 s = noonSun();
        return glm::normalize(glm::vec3(s.x * std::cos(a) - s.y * std::sin(a),
                                        s.x * std::sin(a) + s.y * std::cos(a), s.z));
    };
    REQUIRE(worldShadowRedrawNeeded(key, p, turned(0.1f), 500.0f, 7, 101.0) == WorldShadowRedraw::None);
    REQUIRE(worldShadowRedrawNeeded(key, p, turned(0.5f), 500.0f, 7, 101.0) == WorldShadowRedraw::Spread);
    // A world change: not sooner than the interval after the last draw.
    REQUIRE(worldShadowRedrawNeeded(key, p, noonSun(), 500.0f, 8, 100.0 + pol.contentInterval * 0.5) ==
            WorldShadowRedraw::None);
    REQUIRE(worldShadowRedrawNeeded(key, p, noonSun(), 500.0f, 8, 100.0 + pol.contentInterval) ==
            WorldShadowRedraw::Spread);
}

TEST_CASE("World shadow map covers the shadow distance until the redraw lands", "[shadow_cache]") {
    // Between redraws the player is within the drift of where the map was
    // drawn (plus the few yards a fast mount covers in the three frames a
    // spread redraw takes); every point of the shadow distance's square round
    // the player in light space must still be on the map.
    const WorldShadowCachePolicy pol{};
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (float dist : {100.0f, 200.0f, 350.0f, 500.0f}) {
        const glm::vec3 origin(-8829.0f, 640.0f, 94.0f);
        const ShadowProjection proj = worldShadowProjection(origin, noonSun(), dist, 4096, pol.extentScale, pol.extentPad);
        glm::vec3 right, up, worldUp;
        shadowLightAxes(noonSun(), right, up, worldUp);
        for (int i = 0; i < 2000; ++i) {
            glm::vec3 dir(u(rng), u(rng), u(rng) * 0.2f);
            if (glm::length(dir) < 1e-3f) continue;
            const float drift = pol.driftFraction * dist + 3.0f * 2.0f;  // three frames at 60 yd/s and 30 fps
            const glm::vec3 player = origin + glm::normalize(dir) * drift * std::abs(u(rng));
            // A corner of the player's square in light space, at ground depth.
            const glm::vec3 corner = player + right * dist * (u(rng) > 0 ? 1.0f : -1.0f) +
                                     up * dist * (u(rng) > 0 ? 1.0f : -1.0f);
            const glm::vec4 clip = proj.matrix * glm::vec4(corner, 1.0f);
            INFO("dist " << dist << " clip " << clip.x << " " << clip.y);
            REQUIRE(std::abs(clip.x) <= 1.0f);
            REQUIRE(std::abs(clip.y) <= 1.0f);
            REQUIRE(clip.z >= 0.0f);
            REQUIRE(clip.z <= 1.0f);
        }
    }
}

TEST_CASE("Shadow projections snap to their texel grid", "[shadow_cache]") {
    // A world point lands at the same place within a texel whatever the
    // centre: the map slides by whole texels as the player walks, so a static
    // shadow edge never crawls.
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-40.0f, 40.0f);
    const glm::vec3 point(-9450.0f, 60.0f, 58.0f);
    const uint32_t size = 2048;
    float firstFrac[2] = {-1.0f, -1.0f};
    for (int i = 0; i < 200; ++i) {
        const glm::vec3 c(-9465.0f + u(rng), 47.0f + u(rng), 56.0f + u(rng) * 0.1f);
        const ShadowProjection p = nearShadowProjection(c, noonSun(), 120.0f, size);
        const glm::vec4 clip = p.matrix * glm::vec4(point, 1.0f);
        const float tx = (clip.x * 0.5f + 0.5f) * size, ty = (clip.y * 0.5f + 0.5f) * size;
        const float fx = tx - std::floor(tx), fy = ty - std::floor(ty);
        if (firstFrac[0] < 0.0f) { firstFrac[0] = fx; firstFrac[1] = fy; continue; }
        const auto near = [](float a, float b) { const float d = std::abs(a - b); return std::min(d, 1.0f - d) < 0.02f; };
        REQUIRE(near(fx, firstFrac[0]));
        REQUIRE(near(fy, firstFrac[1]));
    }
}

TEST_CASE("Unit shadow mask marks every texel a caster can shade", "[shadow_cache]") {
    std::mt19937 rng(99);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    const glm::vec3 centre(5804.0f, 624.0f, 647.0f);
    const float half = nearShadowHalfExtent(500.0f);
    const uint32_t size = 2048;
    const ShadowProjection p = nearShadowProjection(centre, noonSun(), half, size);
    for (int c = 0; c < 200; ++c) {
        NearShadowMask mask;
        const glm::vec3 caster = centre + glm::vec3(u(rng) * half, u(rng) * half, u(rng) * 20.0f);
        const float radius = 0.5f + 6.0f * std::abs(u(rng));
        mask.addCaster(p, size, caster, radius);
        // Any point of the caster's sphere, and so anything it shades, which
        // lies under it along the light: the same light-space xy.
        for (int i = 0; i < 200; ++i) {
            glm::vec3 d(u(rng), u(rng), u(rng));
            if (glm::length(d) > 1.0f || glm::length(d) < 1e-3f) continue;
            const glm::vec3 q = caster + d * radius;
            const glm::vec4 clip = p.matrix * glm::vec4(q, 1.0f);
            const glm::vec2 uv = glm::vec2(clip.x, clip.y) * 0.5f + 0.5f;
            if (uv.x < 0.0f || uv.y < 0.0f || uv.x >= 1.0f || uv.y >= 1.0f) continue;
            REQUIRE(mask.testUv(uv));
        }
        // And not the whole map: no more than the cells of its square.
        int marked = 0;
        for (uint32_t y = 0; y < kNearShadowMaskSide; ++y)
            for (uint32_t x = 0; x < kNearShadowMaskSide; ++x) marked += mask.testCell(x, y);
        const float cell = 2.0f * half / static_cast<float>(kNearShadowMaskSide);
        const float side = 2.0f * (radius + 4.0f * 2.0f * half / size) / cell + 2.0f;
        REQUIRE(marked <= static_cast<int>(side * side));
    }
    NearShadowMask empty;
    REQUIRE(empty.empty());
    // A caster off the map marks nothing.
    NearShadowMask off;
    off.addCaster(p, size, centre + glm::vec3(10.0f * half, 0, 0), 3.0f);
    REQUIRE(off.empty());
}

TEST_CASE("Movers stay movers for a while after they stop", "[shadow_cache]") {
    REQUIRE_FALSE(shadowRecentlyMoved(100, 0));
    REQUIRE(shadowRecentlyMoved(100, 100));
    REQUIRE(shadowRecentlyMoved(100 + kShadowMoverHoldFrames - 1, 100));
    REQUIRE_FALSE(shadowRecentlyMoved(100 + kShadowMoverHoldFrames, 100));
    // Across the counter's wrap.
    REQUIRE(shadowRecentlyMoved(5u, 0xFFFFFFF0u));
}
