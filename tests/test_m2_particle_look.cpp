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
