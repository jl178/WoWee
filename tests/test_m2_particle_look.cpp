// The particle record and packed curves (rendering/m2_particle_look.hpp).
//
// The particle pass writes one 48-byte record per quad and leaves colour,
// alpha and size to m2_particle_gpu.vert, which reads the emitter's curves
// from the layout packCurve writes. lookFromPacked is that shader's read in
// C++; here it is held to the stock evaluation straight off the emitter
// (m2_particle::particleColor and friends) for real curve shapes and random
// ones, at every life ratio boundary.
#include <catch_amalgamated.hpp>

#include "rendering/m2_particle_look.hpp"

#include <cstddef>
#include <random>
#include <vector>

using namespace wowee::rendering;
using wowee::pipeline::M2FBlock;
using wowee::pipeline::M2ParticleEmitter;

namespace {

/// Fireball_Missile_Low.m2 emitter 1, as the loader reads it.
M2ParticleEmitter fireballEmitter1() {
    M2ParticleEmitter em{};
    const auto t = [](int v) { return static_cast<float>(v) / 32767.0f; };
    em.particleColor.timestamps = {t(0), t(1079), t(9816), t(32767)};
    em.particleColor.vec3Values = {glm::vec3(1.0f), glm::vec3(1.0f),
                                   glm::vec3(1.0f, 109.0f / 255.0f, 46.0f / 255.0f),
                                   glm::vec3(1.0f, 109.0f / 255.0f, 46.0f / 255.0f)};
    em.particleAlpha.timestamps = {t(0), t(6553), t(32767)};
    em.particleAlpha.floatValues = {0.0f, 31830.0f / 32767.0f, 0.0f};
    em.particleScale.timestamps = {t(0), t(16895), t(32767)};
    em.particleScale.vec2Values = {glm::vec2(0.2222222f), glm::vec2(0.2222222f), glm::vec2(0.16694446f)};
    em.particleScale.floatValues = {0.2222222f, 0.2222222f, 0.16694446f};
    return em;
}

M2FBlock randomBlock(std::mt19937& rng, int n, int dims, bool sortedTimes) {
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    M2FBlock fb;
    std::vector<float> ts(static_cast<size_t>(n));
    for (float& v : ts) v = u(rng);
    if (sortedTimes) std::sort(ts.begin(), ts.end());
    if (n > 0) ts[0] = 0.0f;
    if (n > 1) ts.back() = 1.0f;
    fb.timestamps = ts;
    for (int k = 0; k < n; ++k) {
        if (dims == 1) fb.floatValues.push_back(u(rng));
        if (dims == 2) fb.vec2Values.emplace_back(u(rng) * 3.0f, u(rng) * 3.0f);
        if (dims == 3) fb.vec3Values.emplace_back(u(rng), u(rng), u(rng));
    }
    if (dims == 2) for (const auto& v : fb.vec2Values) fb.floatValues.push_back(v.x);
    return fb;
}

void checkPackedMatches(const M2ParticleEmitter& em) {
    std::vector<glm::vec4> buf(3 * m2_particle::kCurveVec4s, glm::vec4(-7.0f));
    const uint32_t base = m2_particle::kCurveVec4s;  // not at 0, as in a shared buffer
    m2_particle::packCurve(em, buf.data() + base);
    for (int i = 0; i <= 1000; ++i) {
        const float r = static_cast<float>(i) / 1000.0f * 0.9999f;
        const auto want = m2_particle::lookFromEmitter(em, r);
        const auto got = m2_particle::lookFromPacked(buf.data(), base, r);
        INFO("r " << r);
        REQUIRE(got.color.r == Catch::Approx(want.color.r).margin(1e-5));
        REQUIRE(got.color.g == Catch::Approx(want.color.g).margin(1e-5));
        REQUIRE(got.color.b == Catch::Approx(want.color.b).margin(1e-5));
        REQUIRE(got.alpha == Catch::Approx(want.alpha).margin(1e-5));
        REQUIRE(got.scale.x == Catch::Approx(want.scale.x).margin(1e-5));
        REQUIRE(got.scale.y == Catch::Approx(want.scale.y).margin(1e-5));
    }
}

} // namespace

TEST_CASE("the record is the layout the pipeline's attributes read", "[m2][particles]") {
    CHECK(sizeof(m2_particle::ParticleRecord) == 48);
    CHECK(offsetof(m2_particle::ParticleRecord, position) == 0);
    CHECK(offsetof(m2_particle::ParticleRecord, lifeRatio) == 12);
    CHECK(offsetof(m2_particle::ParticleRecord, vec) == 16);
    CHECK(offsetof(m2_particle::ParticleRecord, sizeX) == 28);
    CHECK(offsetof(m2_particle::ParticleRecord, angle) == 32);
    CHECK(offsetof(m2_particle::ParticleRecord, sizeY) == 36);
    CHECK(offsetof(m2_particle::ParticleRecord, curveBase) == 40);
    CHECK(offsetof(m2_particle::ParticleRecord, packed) == 44);
}

TEST_CASE("the packed word keeps cell, kind, flags and alpha apart", "[m2][particles]") {
    using namespace m2_particle;
    const uint32_t w = packRecord(0x1234u, kKindTail, true, false, true, 0.5f);
    CHECK((w & kPackedCellMask) == 0x1234u);
    CHECK(((w >> kPackedKindShift) & 3u) == kKindTail);
    CHECK((w & kPackedSpin) != 0u);
    CHECK((w & kPackedLit) == 0u);
    CHECK((w & kPackedUnfogged) != 0u);
    CHECK((w >> kPackedAlphaShift) == 128u);
    CHECK((packRecord(0x12345u, kKindBillboard, false, false, false, 1.0f) & kPackedCellMask) == 0xFFFFu);
    CHECK((packRecord(0, kKindBillboard, false, false, false, 1.0f) >> kPackedAlphaShift) == 255u);
}

TEST_CASE("the shader's curve read is the stock evaluation", "[m2][particles]") {
    SECTION("Fireball's spark emitter") { checkPackedMatches(fireballEmitter1()); }
    SECTION("empty curves are the defaults") {
        M2ParticleEmitter em{};
        std::vector<glm::vec4> buf(m2_particle::kCurveVec4s);
        m2_particle::packCurve(em, buf.data());
        const auto l = m2_particle::lookFromPacked(buf.data(), 0, 0.3f);
        CHECK(l.color == glm::vec3(1.0f));
        CHECK(l.alpha == 1.0f);
        CHECK(l.scale == glm::vec2(1.0f));
    }
    SECTION("random curves of every length") {
        std::mt19937 rng(11);
        for (int iter = 0; iter < 200; ++iter) {
            M2ParticleEmitter em{};
            const int n = 1 + static_cast<int>(rng() % 7);
            em.particleColor = randomBlock(rng, n, 3, true);
            em.particleAlpha = randomBlock(rng, 1 + static_cast<int>(rng() % 7), 1, true);
            em.particleScale = randomBlock(rng, 1 + static_cast<int>(rng() % 7), 2, true);
            checkPackedMatches(em);
        }
    }
}

// ParticleColor.dbc (stock 0x4EA9E0 -> 0x825410 -> 0x97A990 / 0x9795D0): an
// emitter whose colour index is 11..13 takes the row's slot index-11 colours
// at its own keys' times; any other index, and a one-key block, keep the file.
TEST_CASE("ParticleColor override recolours the keys, keeps the timing",
          "[m2_particle_look][particle_color]") {
    // Row 282 of the 3.3.5 table (an etherial display's).
    const auto ov = m2_particle::particleColorFromRow({0xff000766u, 0xff005966u, 0xff006266u},
                                                      {0xff6d72c6u, 0xff008ea3u, 0xff00badd},
                                                      {0xff385bb6u, 0xff00e8d4u, 0xffb5fdffu});
    REQUIRE(ov.colors[0][0].r == Catch::Approx(0.0f));
    REQUIRE(ov.colors[0][0].g == Catch::Approx(7.0f / 255.0f));
    REQUIRE(ov.colors[0][0].b == Catch::Approx(0x66 / 255.0f));
    REQUIRE(ov.colors[2][2].r == Catch::Approx(0xb5 / 255.0f));  // 0xffb5fdff: B G R A bytes

    M2ParticleEmitter em{};
    em.particleColor.timestamps = {0.0f, 0.25f, 1.0f};
    em.particleColor.vec3Values = {glm::vec3(1.0f), glm::vec3(0.5f), glm::vec3(0.0f)};
    std::vector<glm::vec4> buf(m2_particle::kCurveVec4s);

    // Index 12: slot 1's start, mid, end, split at the file's 0.25.
    em.particleColorIndex = 12;
    m2_particle::packCurve(em, buf.data(), &ov);
    const auto mid = m2_particle::lookFromPacked(buf.data(), 0, 0.125f).color;
    const glm::vec3 want = glm::mix(ov.colors[1][0], ov.colors[1][1], 0.5f);
    CHECK(mid.r == Catch::Approx(want.r));
    CHECK(mid.g == Catch::Approx(want.g));
    CHECK(mid.b == Catch::Approx(want.b));
    const auto end = m2_particle::lookFromPacked(buf.data(), 0, 1.0f).color;
    CHECK(end.b == Catch::Approx(ov.colors[1][2].b));

    // Index 0: untouched.
    em.particleColorIndex = 0;
    m2_particle::packCurve(em, buf.data(), &ov);
    CHECK(m2_particle::lookFromPacked(buf.data(), 0, 1.0f).color.r == Catch::Approx(0.0f));
    CHECK(m2_particle::lookFromPacked(buf.data(), 0, 0.0f).color.r == Catch::Approx(1.0f));

    // One key: the file's colour, override or not (0x9795D0's count test).
    em.particleColorIndex = 11;
    em.particleColor.timestamps = {0.0f};
    em.particleColor.vec3Values = {glm::vec3(0.3f)};
    m2_particle::packCurve(em, buf.data(), &ov);
    CHECK(m2_particle::lookFromPacked(buf.data(), 0, 0.5f).color.g == Catch::Approx(0.3f));

    // Two keys: start to mid - the keys are indices into the three colours.
    em.particleColor.timestamps = {0.0f, 1.0f};
    em.particleColor.vec3Values = {glm::vec3(1.0f), glm::vec3(1.0f)};
    m2_particle::packCurve(em, buf.data(), &ov);
    CHECK(m2_particle::lookFromPacked(buf.data(), 0, 1.0f).color.b == Catch::Approx(ov.colors[0][1].b));

    // No row for a non-zero id: green.
    const auto green = m2_particle::particleColorMissingRow();
    CHECK(green.colors[2][1] == glm::vec3(0.0f, 1.0f, 0.0f));
}
