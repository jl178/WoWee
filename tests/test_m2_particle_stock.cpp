// The stock particle arithmetic (rendering/m2_particle_stock.hpp), against
// what the 3.3.5a client's CParticleEmitter2 and CRibbonEmitter do
// (docs/re/particles.md): quad corners and sizes, lifetime curve segments,
// cells, twinkle, emission shapes, sub-steps and ribbon edges.
#include <catch_amalgamated.hpp>

#include "rendering/m2_particle_stock.hpp"

#include <cmath>

using namespace wowee::rendering;
using wowee::pipeline::M2FBlock;
using wowee::pipeline::M2ParticleEmitter;

namespace {
constexpr float kPi = 3.14159265f;
float fx(int v) { return static_cast<float>(v) / 32767.0f; }
}  // namespace

TEST_CASE("a particle's scale is a half size: the quad is twice as wide", "[particles][stock]") {
    // 0xB2D5B4: the corners sit a whole scale from the centre.
    const glm::vec2 size(0.5f, 0.25f);
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (int i = 0; i < 4; ++i) {
        const glm::vec2 c = m2_particle::billboardCorner(i, size, false, 0.0f);
        minX = std::min(minX, c.x); maxX = std::max(maxX, c.x);
        minY = std::min(minY, c.y); maxY = std::max(maxY, c.y);
    }
    CHECK(maxX - minX == Catch::Approx(1.0f));
    CHECK(maxY - minY == Catch::Approx(0.5f));
}

TEST_CASE("corner 0 is the top left at texture (0, 0), in strip order", "[particles][stock]") {
    using m2_particle::kCornerOffset;
    using m2_particle::kCornerUV;
    CHECK(kCornerOffset[0] == glm::vec2(-1.0f, 1.0f));
    CHECK(kCornerUV[0] == glm::vec2(0.0f, 0.0f));
    // Each corner's texture coordinate is its offset mapped to [0, 1] with
    // v growing downwards.
    for (int i = 0; i < 4; ++i) {
        CHECK(kCornerUV[i].x == Catch::Approx((kCornerOffset[i].x + 1.0f) * 0.5f));
        CHECK(kCornerUV[i].y == Catch::Approx((1.0f - kCornerOffset[i].y) * 0.5f));
    }
    // Strip triangles (0,1,2) and (1,2,3) cover the quad: 0 and 3 opposite.
    CHECK(kCornerOffset[0] == -kCornerOffset[3]);
    CHECK(kCornerOffset[1] == -kCornerOffset[2]);
}

TEST_CASE("spin turns the corners counter-clockwise", "[particles][stock]") {
    const glm::vec2 size(1.0f);
    const glm::vec2 c = m2_particle::billboardCorner(2, size, true, kPi * 0.5f);  // (1, 1) turned 90
    CHECK(c.x == Catch::Approx(-1.0f).margin(1e-5));
    CHECK(c.y == Catch::Approx(1.0f).margin(1e-5));
    // No spin flag: the angle is ignored.
    CHECK(m2_particle::billboardCorner(2, size, false, 1.0f) == glm::vec2(1.0f, 1.0f));
}

TEST_CASE("a velocity quad runs along the velocity on screen", "[particles][stock]") {
    const glm::vec2 size(2.0f, 0.5f);
    // Velocity across the screen: full length along x.
    glm::vec2 c = m2_particle::velocityCorner(2, size, glm::vec3(0.0f, 3.0f, 0.0f));
    // x axis of the quad is +y on screen; corner (1, 1) -> along (0,1)*2 + across (-1,0)*0.5.
    CHECK(c.x == Catch::Approx(-0.5f));
    CHECK(c.y == Catch::Approx(2.0f));
    // Pointing half at the camera: the length shrinks with the projection.
    c = m2_particle::velocityCorner(2, size, glm::vec3(3.0f, 0.0f, 3.0f));
    CHECK(c.x == Catch::Approx(2.0f * std::sqrt(0.5f)));
    // Straight at the camera: collapses to nothing.
    c = m2_particle::velocityCorner(2, size, glm::vec3(0.0f, 0.0f, 3.0f));
    CHECK(c == glm::vec2(0.0f));
}

TEST_CASE("a tail runs from the head back along its length", "[particles][stock]") {
    const glm::vec2 size(0.25f);
    const glm::vec3 t(2.0f, 0.0f, 1.0f);  // view space, mostly to the right
    glm::vec3 c[4];
    for (int i = 0; i < 4; ++i) REQUIRE(m2_particle::tailCorner(i, size, t, c[i]));
    CHECK(c[0] == glm::vec3(0.0f, 0.25f, 0.0f));
    CHECK(c[1] == glm::vec3(0.0f, -0.25f, 0.0f));
    CHECK(c[2] == glm::vec3(2.0f, 0.25f, 1.0f));
    CHECK(c[3] == glm::vec3(2.0f, -0.25f, 1.0f));
    // Too short on screen: the stock falls back to a plain quad.
    glm::vec3 o;
    CHECK_FALSE(m2_particle::tailCorner(0, size, glm::vec3(0.01f, 0.01f, 5.0f), o));
}

TEST_CASE("lifetime curve segments follow 0x9793B0", "[particles][stock]") {
    SECTION("two keys use the ratio, whatever the times") {
        const float ts[2] = {0.3f, 0.6f};
        const auto s = m2_particle::fblockSegment(ts, 2, 0.8f);
        CHECK(s.i0 == 0);
        CHECK(s.i1 == 1);
        CHECK(s.f == Catch::Approx(0.8f));
    }
    SECTION("three keys split at the middle key") {
        const float ts[3] = {0.0f, fx(11468), 1.0f};
        auto s = m2_particle::fblockSegment(ts, 3, 0.2f);
        CHECK(s.i0 == 0);
        CHECK(s.f == Catch::Approx(0.2f / fx(11468)));
        s = m2_particle::fblockSegment(ts, 3, 0.9f);
        CHECK(s.i0 == 1);
        CHECK(s.f == Catch::Approx((0.9f - fx(11468)) / (1.0f - fx(11468))));
    }
    SECTION("four keys with a repeated time, as Fireball's cells") {
        const float ts[4] = {0.0f, fx(11468), fx(11468), 1.0f};
        auto s = m2_particle::fblockSegment(ts, 4, 0.1f);
        CHECK(s.i0 == 0);
        s = m2_particle::fblockSegment(ts, 4, 0.5f);
        CHECK(s.i0 == 2);
        CHECK(s.i1 == 3);
    }
}

TEST_CASE("Fireball's flame curves evaluate as stock", "[particles][stock]") {
    // Fireball_Missile_Low.m2 emitter 0: three keys at 0, 11468, 32767.
    M2ParticleEmitter em{};
    em.particleAlpha.timestamps = {0.0f, fx(11468), 1.0f};
    em.particleAlpha.floatValues = {fx(17604), fx(12850), 0.0f};
    em.particleScale.timestamps = em.particleAlpha.timestamps;
    em.particleScale.vec2Values = {glm::vec2(0.27777779f), glm::vec2(0.13888889f), glm::vec2(0.06944445f)};
    CHECK(m2_particle::particleAlpha(em, 0.0f) == Catch::Approx(fx(17604)));
    CHECK(m2_particle::particleAlpha(em, fx(11468)) == Catch::Approx(fx(12850)));
    CHECK(m2_particle::particleScale(em, fx(11468) * 0.5f).x ==
          Catch::Approx((0.27777779f + 0.13888889f) * 0.5f));
    // Empty curves: colour, alpha and scale 1.
    M2ParticleEmitter none{};
    CHECK(m2_particle::particleColor(none, 0.5f) == glm::vec3(1.0f));
    CHECK(m2_particle::particleAlpha(none, 0.5f) == 1.0f);
    CHECK(m2_particle::particleScale(none, 0.5f) == glm::vec2(1.0f));
}

TEST_CASE("x and y scale come from the curve's two components", "[particles][stock]") {
    // Fireball emitter 3: a 3.77 x 1.94 glow.
    M2ParticleEmitter em{};
    em.particleScale.timestamps = {0.0f, 1.0f};
    em.particleScale.vec2Values = {glm::vec2(3.7705986f, 1.9398619f), glm::vec2(3.77059f, 1.9398534f)};
    const glm::vec2 s = m2_particle::particleScale(em, 0.5f);
    CHECK(s.x == Catch::Approx(3.7706f).margin(1e-3));
    CHECK(s.y == Catch::Approx(1.9399f).margin(1e-3));
}

TEST_CASE("cells come from the head curve, rounded, and map to the grid", "[particles][stock]") {
    M2FBlock cells;
    cells.timestamps = {0.0f, 1.0f};
    cells.floatValues = {0.0f, 3.0f};
    CHECK(m2_particle::particleCell(cells, 0.0f) == 0);
    CHECK(m2_particle::particleCell(cells, 0.49f) == 1);  // 1.47 rounds to 1
    CHECK(m2_particle::particleCell(cells, 0.51f) == 2);  // 1.53 rounds to 2
    CHECK(m2_particle::particleCell(M2FBlock{}, 0.5f) == -1);

    // 2 x 2: cell 3 is the bottom right; columns across, rows down.
    const glm::vec2 uv0 = m2_particle::cellUV(3, 2, 2, glm::vec2(0.0f));
    const glm::vec2 uv1 = m2_particle::cellUV(3, 2, 2, glm::vec2(1.0f));
    CHECK(uv0 == glm::vec2(0.5f, 0.5f));
    CHECK(uv1 == glm::vec2(1.0f, 1.0f));
    // 4 columns, 2 rows: cell 5 is column 1 of row 1.
    CHECK(m2_particle::cellUV(5, 2, 4, glm::vec2(0.0f)) == glm::vec2(0.25f, 0.5f));
}

TEST_CASE("twinkle hides and scales as the table says", "[particles][stock]") {
    M2ParticleEmitter em{};
    // The defaults: always shown at twinkleScale.min.
    em.twinklePercent = 1.0f;
    em.twinkleScaleMin = em.twinkleScaleMax = 0.7f;
    auto t = m2_particle::twinkle(em, 5, 1.0f);
    CHECK(t.visible);
    CHECK(t.scale == Catch::Approx(0.7f));
    // A zero percent never shows.
    em.twinklePercent = 0.0f;
    int shown = 0;
    for (uint32_t ph = 0; ph < 128; ++ph) shown += m2_particle::twinkle(em, ph, 0.0f).visible ? 1 : 0;
    CHECK(shown == 0);
    // Half shows about half the time, and the scale spans the range.
    em.twinklePercent = 0.5f;
    em.twinkleScaleMin = 1.0f;
    em.twinkleScaleMax = 2.0f;
    shown = 0;
    for (uint32_t ph = 0; ph < 128; ++ph) {
        const auto tw = m2_particle::twinkle(em, ph, 0.0f);
        shown += tw.visible ? 1 : 0;
        CHECK(tw.scale >= 1.0f);
        CHECK(tw.scale < 2.0f);
    }
    CHECK(shown > 40);
    CHECK(shown < 88);
}

TEST_CASE("scale variation: one factor, or two with flag 0x80000", "[particles][stock]") {
    M2ParticleEmitter em{};
    em.scaleVary = glm::vec2(0.5f, 0.25f);
    glm::vec2 f = m2_particle::scaleVariation(em, 1.0f, -1.0f);
    CHECK(f.x == Catch::Approx(1.5f));
    CHECK(f.y == Catch::Approx(1.5f));
    em.flags = m2_particle::kFlagScaleVaryXY;
    f = m2_particle::scaleVariation(em, 1.0f, -1.0f);
    CHECK(f.x == Catch::Approx(0.5f));
    CHECK(f.y == Catch::Approx(1.25f));
    em.scaleVary = glm::vec2(3.0f);
    f = m2_particle::scaleVariation(em, -1.0f, -1.0f);
    CHECK(f.x == Catch::Approx(1e-4f));  // floored at 0.0001
}

TEST_CASE("emission shapes", "[particles][stock]") {
    SECTION("plane: straight up the emitter Z with no ranges") {
        const glm::vec3 d = m2_particle::planeDirection(0.0f, 0.0f, 0.7f, -0.3f);
        CHECK(d == glm::vec3(0.0f, 0.0f, 1.0f));
    }
    SECTION("plane: tilted by the vertical range, turned by the horizontal") {
        const glm::vec3 d = m2_particle::planeDirection(kPi * 0.5f, kPi * 0.5f, 1.0f, 1.0f);
        CHECK(d.x == Catch::Approx(0.0f).margin(1e-6));
        CHECK(d.y == Catch::Approx(1.0f));
        CHECK(d.z == Catch::Approx(0.0f).margin(1e-6));
    }
    SECTION("sphere: elevation from the XY plane") {
        CHECK(m2_particle::sphereDirection(1.0f, 1.0f, 0.0f, 0.0f) == glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::vec3 d = m2_particle::sphereDirection(kPi * 0.5f, 0.0f, 1.0f, 0.0f);
        CHECK(d.z == Catch::Approx(1.0f));
    }
    SECTION("the emitter frame is the bone's turned 90 degrees about Z") {
        const glm::mat4 r = m2_particle::emitterRotation();
        CHECK(glm::vec3(r * glm::vec4(1, 0, 0, 0)) == glm::vec3(0, 1, 0));
        CHECK(glm::vec3(r * glm::vec4(0, 1, 0, 0)) == glm::vec3(-1, 0, 0));
        CHECK(glm::vec3(r * glm::vec4(0, 0, 1, 0)) == glm::vec3(0, 0, 1));
    }
}

TEST_CASE("rate thinning with distance and the pool", "[particles][stock]") {
    CHECK(m2_particle::distanceRateFactor(10.0f) == 1.0f);
    CHECK(m2_particle::distanceRateFactor(50.0f) == 1.0f);
    CHECK(m2_particle::distanceRateFactor(75.0f) == Catch::Approx(0.5f));
    CHECK(m2_particle::distanceRateFactor(500.0f) == 0.25f);
    // Fireball's flame: 130 a second for 0.4 s, plus 15 percent.
    CHECK(m2_particle::poolSize(130.0f, 0.0f, 0.4f, 0.0f) == 60u);
    CHECK(m2_particle::poolSize(0.0f, 0.0f, 1.0f, 0.0f) == 0u);
}

TEST_CASE("long steps split into tenths, no more than a lifespan", "[particles][stock]") {
    auto s = m2_particle::substeps(0.016f, 1.0f);
    CHECK(s.whole == 0);
    CHECK(s.last == Catch::Approx(0.016f));
    s = m2_particle::substeps(0.25f, 5.0f);  // floor 2 -> nearbyint(1.5) = 2
    CHECK(s.whole == 2);
    CHECK(s.last == Catch::Approx(0.05f));
    s = m2_particle::substeps(0.35f, 5.0f);  // floor 3 -> nearbyint(2.5) = 2 (even)
    CHECK(s.whole == 2);
    s = m2_particle::substeps(3600.0f, 0.6f);  // capped at the lifespan's 6
    CHECK(s.whole == 6);
}

TEST_CASE("ribbon edges between frames", "[ribbons][stock]") {
    const glm::vec3 a(0.0f), b(10.0f, 0.0f, 0.0f);
    // An emitter that does not turn: the straight line.
    const glm::vec3 z(0.0f, 0.0f, 1.0f);
    for (float t : {0.0f, 0.25f, 0.5f, 1.0f}) {
        const glm::vec3 p = m2_particle::ribbonEdgePoint(a, b, z * 10.0f, z * 10.0f, t);
        CHECK(p.x == Catch::Approx(10.0f * t));
        CHECK(p.z == Catch::Approx(0.0f).margin(1e-5));
    }
    // Gravity: an edge rises gravity * age^2 over its life.
    float rise = 0.0f, age = 0.0f;
    for (int i = 0; i < 10; ++i) {
        rise += m2_particle::ribbonGravityStep(age, 0.1f, 2.0f);
        age += 0.1f;
    }
    CHECK(rise == Catch::Approx(2.0f * 1.0f * 1.0f));
}
