// The stock client's water vertex values and underwater particulates, held to
// Wow.exe 3.3.5a at the addresses docs/re/water.md names:
//   rendering/stock_liquid.hpp (depth ramp row, uv scales, texture rotation)
//   rendering/stock_particulates.hpp (the waterParticulates motes).
#include <catch_amalgamated.hpp>

#include "rendering/stock_liquid.hpp"
#include "rendering/stock_particulates.hpp"

#include <cmath>

using Catch::Approx;
namespace liq = wowee::rendering::stock_liquid;
namespace sp = wowee::rendering::stock_particulates;

TEST_CASE("the depth byte picks the ramp row by the river or ocean table", "[stock][water]") {
    // LiquidType Int[0]: 0 river (Water, Slow/Fast Water...), 1 ocean and WMO.
    CHECK(liq::depthDivisor(0) == 42.0f);
    CHECK(liq::depthDivisor(1) == 255.0f);
    // The river table 0x79e3c0: x = d/9, x <= 14/3 ? x*3/14 : 1.
    for (int d = 0; d < 256; ++d) {
        const float x = static_cast<float>(d) / 9.0f;
        const float table = x <= 14.0f / 3.0f ? x * 3.0f / 14.0f : 1.0f;
        CHECK(liq::rampCoordinate(static_cast<uint8_t>(d), 42.0f) == Approx(table).margin(1e-5));
    }
    CHECK(liq::rampCoordinate(16, 42.0f) == Approx(16.0f / 42.0f));
    CHECK(liq::rampCoordinate(255, 255.0f) == 1.0f);
    CHECK(liq::rampCoordinate(0, 255.0f) == 0.0f);
}

TEST_CASE("liquid uv scales and the WMO Ocean rotation slip", "[stock][water]") {
    CHECK(1.0f / liq::kTerrainUVPerYard == Approx(16.6667f).epsilon(1e-4));
    // One repeat per MLIQ cell (25/6 yards).
    CHECK(liq::kWmoUVPerYard * 25.0f / 6.0f == Approx(1.0f));
    CHECK(liq::kVertexUVScale == Approx(0.01171875f));
    // Float[1] = 180 becomes 10313.24 rad: cos -0.82054, sin 0.57159.
    const float a = liq::textureRotationRadians(180.0f);
    CHECK(std::cos(a) == Approx(-0.82054).margin(2e-3));
    CHECK(std::sin(a) == Approx(0.57159).margin(2e-3));
    CHECK(liq::textureRotationRadians(0.0f) == 0.0f);
}

TEST_CASE("particulate sizes, box and draw cap", "[stock][water][particulates]") {
    CHECK(sp::kMoteCount == 4000);
    CHECK(sp::kMaxDrawn == 666);
    // Water (ParticleScale 1): 0.0139..0.0417 yd; magma (4): 0.056..0.167.
    CHECK(sp::moteSize(0.0f, 1.0f) == Approx(0.5f / 36.0f));
    CHECK(sp::moteSize(1.0f, 1.0f) == Approx(1.5f / 36.0f));
    CHECK(sp::moteSize(1.0f, 4.0f) == Approx(6.0f / 36.0f));
}

TEST_CASE("motes stay in the world and wrap into the camera's box", "[stock][water][particulates]") {
    // The camera moves 2 yd along x: a mote's offset drops by 2.
    glm::vec3 p = sp::step(glm::vec3(0.0f), glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(0.0f));
    CHECK(p.x == Approx(-2.0f));
    // Past the box it comes back in on the other side.
    p = sp::step(glm::vec3(-14.5f, 0.0f, 14.9f), glm::vec3(1.0f, 0.0f, -0.2f), glm::vec3(0.0f));
    CHECK(p.x == Approx(14.5f));
    CHECK(p.z == Approx(-14.9f));
    CHECK(sp::wrap(15.0f) == 15.0f);
    CHECK(sp::wrap(-46.0f) == Approx(14.0f));
}

TEST_CASE("the drift by ParticleMovement", "[stock][water][particulates]") {
    sp::Current c = sp::makeCurrent(0.5f, 0.5f, 0.0f, 0.0f);  // a = 0, b = 0: straight up
    CHECK(c.dir.z == Approx(1.0f));
    CHECK(c.rate == Approx(0.0125f));
    CHECK(c.amp == Approx(0.005f));
    // Rate and amplitude run to twice their floor.
    const sp::Current hi = sp::makeCurrent(0.1f, 0.9f, 0.999f, 0.999f);
    CHECK(hi.rate == Approx(0.025f).margin(1e-4));
    CHECK(hi.amp == Approx(0.01f).margin(1e-4));
    CHECK(hi.dir.z >= 0.0f);  // never down
    // A quarter of the way through the push it is at its peak, per frame.
    c.phase = 0.25f / c.rate;
    CHECK(sp::drift(0, c, 1.0f / 60.0f).z == Approx(0.005f));
    CHECK_FALSE(sp::currentSpent(c));
    c.phase = 0.51f / c.rate;
    CHECK(sp::currentSpent(c));
    // Magma sinks, movement 2 rises, at 0.02 yd/s.
    CHECK(sp::drift(1, c, 0.5f).z == Approx(-0.01f));
    CHECK(sp::drift(2, c, 0.5f).z == Approx(0.01f));
}

TEST_CASE("only motes in the 90 degree cone ahead are drawn", "[stock][water][particulates]") {
    CHECK(sp::inViewCone(0.0f, 0.0f, 1.0f));
    CHECK(sp::inViewCone(0.9f, -0.9f, 1.0f));
    CHECK_FALSE(sp::inViewCone(1.1f, 0.0f, 1.0f));
    CHECK_FALSE(sp::inViewCone(0.0f, 0.0f, -1.0f));
}

TEST_CASE("the WaterPoop02 atlas cells", "[stock][water][particulates]") {
    // Table 0xa3f970 by ParticleTexSlots: water 0..7, magma 8..11.
    CHECK(sp::atlasCell(0, 0) == 8);  // mote 0 always cell 8
    CHECK(sp::atlasCell(0, 1) == 0);
    CHECK(sp::atlasCell(0, 8) == 7);
    CHECK(sp::atlasCell(0, 9) == 0);
    CHECK(sp::atlasCell(1, 5) == 8);
    CHECK(sp::atlasCell(1, 4) == 11);
    CHECK(sp::atlasCell(3, 4) == 16);
    // Table 0xadf838: rows of four, then the fifth column.
    const float k = 51.0f / 256.0f;
    CHECK(sp::cellOrigin(0) == glm::vec2(0.0f));
    CHECK(sp::cellOrigin(7).x == Approx(3 * k));
    CHECK(sp::cellOrigin(7).y == Approx(k));
    CHECK(sp::cellOrigin(11).y == Approx(2 * k));
    CHECK(sp::cellOrigin(12).x == Approx(4 * k));
    CHECK(sp::cellOrigin(12).y == Approx(0.0f));
    CHECK(sp::cellOrigin(16).y == Approx(4 * k));
    CHECK(sp::cellOrigin(17).y == Approx(3 * k));
}
