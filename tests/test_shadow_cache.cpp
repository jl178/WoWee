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

namespace {

// The camera's frustum as Frustum::extractFromMatrix makes it (Vulkan depth
// 0..1), inward normals, normalised.
ShadowReceiverView viewFor(const glm::mat4& vp, const glm::vec3& sunRay, float floorZ) {
    ShadowReceiverView v;
    const auto row = [&](int r) { return glm::vec4(vp[0][r], vp[1][r], vp[2][r], vp[3][r]); };
    const glm::vec4 p[6] = {row(3) + row(0), row(3) - row(0), row(3) + row(1),
                            row(3) - row(1), row(2), row(3) - row(2)};
    for (int i = 0; i < 6; ++i) v.planes[i] = p[i] / glm::length(glm::vec3(p[i]));
    v.sunRay = sunRay;
    v.floorZ = floorZ;
    return v;
}

bool inClip(const glm::mat4& vp, const glm::vec3& q) {
    const glm::vec4 c = vp * glm::vec4(q, 1.0f);
    return c.w > 0.0f && std::abs(c.x) <= c.w && std::abs(c.y) <= c.w && c.z >= 0.0f && c.z <= c.w;
}

}  // namespace

TEST_CASE("Unit shadow view cull: a culled caster's shadow is never in view", "[shadow_cache]") {
    const glm::vec3 eye(10.0f, -20.0f, 12.0f);
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.27f, 0.5f, 2000.0f);
    proj[1][1] *= -1.0f;
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    int culled = 0, kept = 0;
    for (int view = 0; view < 40; ++view) {
        const glm::vec3 look = glm::normalize(glm::vec3(u(rng), u(rng), -0.2f + 0.2f * u(rng)));
        const glm::mat4 vp = proj * glm::lookAt(eye, eye + look, glm::vec3(0, 0, 1));
        const glm::vec3 sun = glm::normalize(glm::vec3(u(rng), u(rng), -0.15f - 0.85f * std::abs(u(rng))));
        const float floorZ = eye.z - 40.0f;
        const ShadowReceiverView v = viewFor(vp, sun, floorZ);
        for (int i = 0; i < 200; ++i) {
            const glm::vec3 c = eye + glm::vec3(70.0f * u(rng), 70.0f * u(rng), 5.0f * u(rng));
            const float r = 1.0f + 2.0f * std::abs(u(rng));
            if (shadowCasterSeen(v, c, r)) { ++kept; continue; }
            ++culled;
            // Points the caster can shade: its sphere carried down the ray to
            // the floor. None of them may be in view.
            const float reach = shadowReachAlongRay(c, r, sun, floorZ, v.maxReach);
            for (int s = 0; s <= 64; ++s) {
                const glm::vec3 along = c + sun * (reach * static_cast<float>(s) / 64.0f);
                for (int k = 0; k < 12; ++k) {
                    const glm::vec3 off = glm::normalize(glm::vec3(u(rng), u(rng), u(rng))) * r * std::abs(u(rng));
                    REQUIRE_FALSE(inClip(vp, along + off));
                }
            }
        }
    }
    // It does cull: units behind the camera under a high sun.
    REQUIRE(culled > 1000);
    REQUIRE(kept > 1000);
}

TEST_CASE("Unit shadow view cull: a shadow thrown into view is kept", "[shadow_cache]") {
    // The camera looks north; a unit five yards behind it, the sun low in
    // the south: its shadow runs forward under the camera into the view.
    const glm::vec3 eye(0.0f, 0.0f, 10.0f);
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.27f, 0.5f, 2000.0f);
    proj[1][1] *= -1.0f;
    const glm::mat4 vp = proj * glm::lookAt(eye, eye + glm::vec3(0, 1, -0.3f), glm::vec3(0, 0, 1));
    const glm::vec3 lowSouthSun = glm::normalize(glm::vec3(0.0f, 1.0f, -0.3f));
    const ShadowReceiverView v = viewFor(vp, lowSouthSun, eye.z - 40.0f);
    REQUIRE(shadowCasterSeen(v, glm::vec3(0.0f, -5.0f, 1.0f), 1.5f));
    // The same unit under a sun in the north: its shadow falls behind it.
    const glm::vec3 lowNorthSun = glm::normalize(glm::vec3(0.0f, -1.0f, -0.3f));
    REQUIRE_FALSE(shadowCasterSeen(viewFor(vp, lowNorthSun, eye.z - 40.0f), glm::vec3(0.0f, -5.0f, 1.0f), 1.5f));
}

TEST_CASE("Detail shadow map: when it is drawn again and when it is not read", "[shadow_cache]") {
    const glm::vec3 p(-9465.0f, 47.0f, 56.6f);
    const float he = kDetailShadowHalfExtent;
    const DetailShadowCachePolicy pol{};
    auto key = keyAt(p, he);
    // Nothing kept: drawn now, not read until it is.
    REQUIRE(detailShadowRedrawNeeded(WorldShadowCacheKey{}, p, noonSun(), he, 7, 0.0) == WorldShadowRedraw::Now);
    // Standing still, nothing changes.
    for (double t = 100.0; t < 400.0; t += 9.1)
        REQUIRE(detailShadowRedrawNeeded(key, p, noonSun(), he, 7, t) == WorldShadowRedraw::None);
    // Walking: kept below the drift, redrawn past it; height alone never.
    const float drift = pol.driftFraction * he;
    REQUIRE(detailShadowRedrawNeeded(key, p + glm::vec3(drift * 0.9f, 0, 0), noonSun(), he, 7, 101.0) ==
            WorldShadowRedraw::None);
    REQUIRE(detailShadowRedrawNeeded(key, p + glm::vec3(0, 0, 40.0f), noonSun(), he, 7, 101.0) ==
            WorldShadowRedraw::None);
    REQUIRE(detailShadowRedrawNeeded(key, p + glm::vec3(0, drift * 1.1f, 0), noonSun(), he, 7, 101.0) ==
            WorldShadowRedraw::Spread);
    // Far enough that the kept square no longer serves the view: not read.
    REQUIRE(detailShadowRedrawNeeded(key, p + glm::vec3(0.6f * he, 0, 0), noonSun(), he, 7, 101.0) ==
            WorldShadowRedraw::Now);
    // The sun: a fraction of a degree redraws it, a jump of the clock stops
    // it being read until it is drawn again.
    const auto turned = [&](float deg) {
        const glm::mat3 r = glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(deg), glm::vec3(0, 0, 1)));
        return glm::normalize(r * noonSun());
    };
    REQUIRE(detailShadowRedrawNeeded(key, p, turned(0.1f), he, 7, 101.0) == WorldShadowRedraw::None);
    REQUIRE(detailShadowRedrawNeeded(key, p, turned(0.5f), he, 7, 101.0) == WorldShadowRedraw::Spread);
    REQUIRE(detailShadowRedrawNeeded(key, p, turned(5.0f), he, 7, 101.0) == WorldShadowRedraw::Now);
    // A world change: no sooner than the interval.
    REQUIRE(detailShadowRedrawNeeded(key, p, noonSun(), he, 8, 101.0) == WorldShadowRedraw::None);
    REQUIRE(detailShadowRedrawNeeded(key, p, noonSun(), he, 8, 103.0) == WorldShadowRedraw::Spread);
}

TEST_CASE("Detail shadow map: fine texels on the world map's depth, snapped", "[shadow_cache]") {
    const glm::vec3 c(-9465.0f, 47.0f, 56.6f);
    const ShadowProjection d = detailShadowProjection(c, noonSun(), 500.0f);
    const ShadowProjection w = worldShadowProjection(c, noonSun(), 500.0f, 4096, 1.05f, 8.0f);
    REQUIRE(d.texelWorld < 0.1f);
    REQUIRE(d.texelWorld < w.texelWorld / 3.0f);
    // The same depth toward the sun as the world map: what shades the world
    // map's square from far along the light shades the detail square too.
    REQUIRE(d.depthRange == Catch::Approx(w.depthRange));
    // The point the player stands on lands inside, well clear of the edge.
    const glm::vec4 clip = d.matrix * glm::vec4(c, 1.0f);
    REQUIRE(std::abs(clip.x) < 0.01f);
    REQUIRE(std::abs(clip.y) < 0.01f);
    REQUIRE(clip.z > 0.0f);
    REQUIRE(clip.z < 1.0f);
    // Texel-snapped like the others: a still shadow edge does not crawl as
    // the player walks from one redraw to the next.
    const glm::vec3 point(-9450.0f, 60.0f, 58.0f);
    float first[2] = {-1.0f, -1.0f};
    for (int i = 0; i < 50; ++i) {
        const ShadowProjection p =
            detailShadowProjection(c + glm::vec3(0.37f * i, -0.21f * i, 0.0f), noonSun(), 500.0f);
        const glm::vec4 q = p.matrix * glm::vec4(point, 1.0f);
        const float tx = (q.x * 0.5f + 0.5f) * kDetailShadowMapSize;
        const float ty = (q.y * 0.5f + 0.5f) * kDetailShadowMapSize;
        const float fx = tx - std::floor(tx), fy = ty - std::floor(ty);
        if (first[0] < 0.0f) { first[0] = fx; first[1] = fy; continue; }
        const auto near = [](float a, float b) { const float e = std::abs(a - b); return std::min(e, 1.0f - e) < 0.03f; };
        REQUIRE(near(fx, first[0]));
        REQUIRE(near(fy, first[1]));
    }
}
