// The compact particle record (rendering/m2_particle_look.hpp).
//
// The GPU particle path writes one 24-byte record per particle and leaves
// colour, alpha and size to m2_particle_gpu.vert. lookFromPacked is that
// shader's arithmetic in C++, reading the curve from the storage-buffer
// layout packCurve writes. Here it is held to the CPU path - the code
// renderM2Particles runs under WOWEE_PARTICLE_CPU_QUADS=1, copied below as it
// stands - for random curves, every life ratio boundary and every model class
// and blend; and the tile packing to the CPU path's float tile loop.
#include <catch_amalgamated.hpp>

#include "rendering/m2_particle_look.hpp"

#include <cstddef>
#include <random>
#include <vector>

using namespace wowee::rendering;
using m2_particle::kCurveSteps;

namespace {

struct Curve {
    glm::vec4 colorAlpha[kCurveSteps + 1];
    float scale[kCurveSteps + 1];
};

Curve randomCurve(std::mt19937& rng) {
    std::uniform_real_distribution<float> c(0.0f, 1.0f);
    // Scales across the thresholds the classes test: 0.15, 1.5 and 2.
    std::uniform_real_distribution<float> s(0.0f, 3.0f);
    Curve curve{};
    for (int k = 0; k <= kCurveSteps; ++k) {
        curve.colorAlpha[k] = glm::vec4(c(rng), c(rng), c(rng), c(rng));
        curve.scale[k] = s(rng);
    }
    return curve;
}

// renderM2Particles' CPU path, per particle, from the curve tables on.
m2_particle::Look cpuPathLook(const Curve& curve, float lifeRatio, bool isSpellEffect,
                              bool isFireflyEffect, bool isFlameFixture,
                              uint16_t cachedBlendType) {
    const bool isPlainDoodad = !isSpellEffect && !isFireflyEffect && !isFlameFixture;
    const float f = glm::clamp(lifeRatio, 0.0f, 1.0f) * static_cast<float>(kCurveSteps);
    const int k = std::min(static_cast<int>(f), kCurveSteps - 1);
    const float t = f - static_cast<float>(k);
    const glm::vec4 ca = glm::mix(curve.colorAlpha[k], curve.colorAlpha[k + 1], t);
    glm::vec3 color = glm::vec3(ca);
    float alpha = ca.a;
    const float rawScale = glm::mix(curve.scale[k], curve.scale[k + 1], t);

    if (isPlainDoodad) {
        color = glm::mix(color, glm::vec3(1.0f), 0.7f);
        if (rawScale > 2.0f) alpha *= 0.02f;
        if (cachedBlendType == 3 || cachedBlendType == 4) alpha *= 0.05f;
    }
    if (isFlameFixture) {
        color = glm::max(color, glm::vec3(0.50f, 0.26f, 0.09f));
        alpha = std::max(alpha, 0.30f);
    }
    float scale = rawScale;
    if (isSpellEffect) {
        scale = std::max(rawScale, 0.15f);
    } else if (!isFireflyEffect) {
        scale = std::min(rawScale, 1.5f);
        if (isFlameFixture) {
            scale = std::max(scale, 0.15f);
        }
    }
    return {color, alpha, scale};
}

// The CPU path's tile, in float as it was written into the vertex.
float cpuPathTile(uint16_t ownTile, bool cachedIsTiled, uint32_t cachedAnimFrame,
                  uint32_t cachedTotalTiles) {
    const float cachedTilesFloat = static_cast<float>(cachedTotalTiles);
    float tileIndex = ownTile;
    if (cachedIsTiled) {
        tileIndex = ownTile + static_cast<float>(cachedAnimFrame);
        while (tileIndex >= cachedTilesFloat) {
            tileIndex -= cachedTilesFloat;
        }
    }
    return tileIndex;
}

void requireSame(const m2_particle::Look& a, const m2_particle::Look& b) {
    constexpr float eps = 1e-6f;
    REQUIRE(a.color.r == Catch::Approx(b.color.r).margin(eps));
    REQUIRE(a.color.g == Catch::Approx(b.color.g).margin(eps));
    REQUIRE(a.color.b == Catch::Approx(b.color.b).margin(eps));
    REQUIRE(a.alpha == Catch::Approx(b.alpha).margin(eps));
    REQUIRE(a.scale == Catch::Approx(b.scale).margin(eps));
}

std::vector<float> lifeRatios(std::mt19937& rng) {
    std::vector<float> rs = {0.0f, 1.0f, -0.25f, 1.25f, 0.5f, 0.999999f, 1e-7f};
    for (int k = 0; k <= kCurveSteps; ++k) {
        rs.push_back(static_cast<float>(k) / kCurveSteps);
    }
    std::uniform_real_distribution<float> u(-0.1f, 1.1f);
    for (int i = 0; i < 200; ++i) rs.push_back(u(rng));
    return rs;
}

} // namespace

TEST_CASE("the record is the layout the pipeline's attributes read", "[m2][particles]") {
    using m2_particle::ParticleRecord;
    STATIC_REQUIRE(sizeof(ParticleRecord) == 24);
    STATIC_REQUIRE(offsetof(ParticleRecord, position) == 0);
    STATIC_REQUIRE(offsetof(ParticleRecord, lifeRatio) == 12);
    STATIC_REQUIRE(offsetof(ParticleRecord, curveBase) == 16);
    STATIC_REQUIRE(offsetof(ParticleRecord, tileFlags) == 20);
    // 65 colour+alpha vec4s and 65 scales in 17 vec4s.
    STATIC_REQUIRE(m2_particle::kCurveVec4s == 82);
}

TEST_CASE("the shader's look from the packed curve is the CPU path's", "[m2][particles]") {
    std::mt19937 rng(0x5eed);
    // Several curves in one buffer, as a frame packs them, so a base other
    // than zero is read too.
    constexpr int kCurves = 4;
    std::vector<Curve> curves;
    std::vector<glm::vec4> buffer(kCurves * m2_particle::kCurveVec4s);
    for (int c = 0; c < kCurves; ++c) {
        curves.push_back(randomCurve(rng));
        m2_particle::packCurve(curves.back().colorAlpha, curves.back().scale,
                               buffer.data() + c * m2_particle::kCurveVec4s);
    }
    const std::vector<float> ratios = lifeRatios(rng);

    // Every class a model can be (spell effect and firefly are exclusive of
    // each other in practice but not here) and every blend type.
    for (int cls = 0; cls < 8; ++cls) {
        const bool spell = cls & 1, firefly = cls & 2, flame = cls & 4;
        for (uint16_t blend = 0; blend < 8; ++blend) {
            const uint32_t flags = m2_particle::lookFlagsFor(spell, firefly, flame, blend);
            for (int c = 0; c < kCurves; ++c) {
                const uint32_t base = static_cast<uint32_t>(c) * m2_particle::kCurveVec4s;
                for (float r : ratios) {
                    const auto want = cpuPathLook(curves[c], r, spell, firefly, flame, blend);
                    requireSame(m2_particle::lookFromPacked(buffer.data(), base, r, flags), want);
                    requireSame(m2_particle::lookFromCurve(curves[c].colorAlpha,
                                                           curves[c].scale, r, flags), want);
                }
            }
        }
    }
}

TEST_CASE("the packed curve holds the tables unchanged", "[m2][particles]") {
    std::mt19937 rng(7);
    const Curve curve = randomCurve(rng);
    std::vector<glm::vec4> buffer(m2_particle::kCurveVec4s, glm::vec4(-1.0f));
    m2_particle::packCurve(curve.colorAlpha, curve.scale, buffer.data());
    for (int k = 0; k <= kCurveSteps; ++k) {
        REQUIRE(buffer[k] == curve.colorAlpha[k]);
        REQUIRE(buffer[kCurveSteps + 1 + k / 4][k % 4] == curve.scale[k]);
    }
    // The padding after the last scale is written, not left as it was.
    const auto& tail = buffer[m2_particle::kCurveVec4s - 1];
    REQUIRE(tail[1] == 0.0f);
    REQUIRE(tail[2] == 0.0f);
    REQUIRE(tail[3] == 0.0f);
}

TEST_CASE("the packed tile is the CPU path's tile", "[m2][particles]") {
    for (uint32_t total = 1; total <= 64; ++total) {
        for (uint32_t frame = 0; frame < total; ++frame) {
            for (uint32_t own = 0; own < total; ++own) {
                for (bool tiled : {false, true}) {
                    const bool isTiled = tiled && total > 1;
                    const uint32_t tile = m2_particle::particleTile(own, isTiled, frame, total);
                    REQUIRE(static_cast<float>(tile) ==
                            cpuPathTile(static_cast<uint16_t>(own), isTiled, frame, total));
                }
            }
        }
    }
}

TEST_CASE("tile and flags share a word without overlapping", "[m2][particles]") {
    const uint32_t allFlags = m2_particle::kLookPlainDoodad | m2_particle::kLookFlameFixture |
                              m2_particle::kLookSpellEffect | m2_particle::kLookFirefly |
                              m2_particle::kLookAdditiveBlend;
    REQUIRE((allFlags >> 8) == 0u);  // fits the top byte
    for (uint32_t tile : {0u, 1u, 63u, 4095u, m2_particle::kTileMask}) {
        const uint32_t w = m2_particle::packTileFlags(tile, allFlags);
        REQUIRE((w & m2_particle::kTileMask) == tile);
        REQUIRE((w >> m2_particle::kTileBits) == allFlags);
    }
    // A tile past 24 bits is held at the largest, not spilled into the flags.
    const uint32_t w = m2_particle::packTileFlags(1u << 25, 0u);
    REQUIRE((w >> m2_particle::kTileBits) == 0u);
}

TEST_CASE("the class flags are the CPU path's class tests", "[m2][particles]") {
    using namespace m2_particle;
    REQUIRE(lookFlagsFor(false, false, false, 0) == kLookPlainDoodad);
    REQUIRE(lookFlagsFor(false, false, false, 3) == (kLookPlainDoodad | kLookAdditiveBlend));
    REQUIRE(lookFlagsFor(false, false, false, 4) == (kLookPlainDoodad | kLookAdditiveBlend));
    REQUIRE(lookFlagsFor(false, false, false, 2) == kLookPlainDoodad);
    REQUIRE(lookFlagsFor(true, false, false, 1) == kLookSpellEffect);
    REQUIRE(lookFlagsFor(false, true, false, 0) == kLookFirefly);
    REQUIRE(lookFlagsFor(false, false, true, 4) == (kLookFlameFixture | kLookAdditiveBlend));
}
