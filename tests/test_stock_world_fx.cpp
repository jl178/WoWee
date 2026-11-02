// The stock client's water, sky, WMO and screen-effect arithmetic, held to
// Wow.exe 3.3.5a at the addresses the headers name:
//   rendering/stock_liquid.hpp, stock_sky.hpp, stock_wmo_light.hpp,
//   stock_screen_fx.hpp. Header-only.
#include <catch_amalgamated.hpp>

#include "rendering/stock_liquid.hpp"
#include "rendering/stock_screen_fx.hpp"
#include "rendering/stock_sky.hpp"
#include "rendering/stock_wmo_light.hpp"

#include <cmath>

using Catch::Approx;
namespace liq = wowee::rendering::stock_liquid;
namespace sky = wowee::rendering::stock_sky;
namespace wmo = wowee::rendering::stock_wmo;
namespace fx = wowee::rendering::stock_screen_fx;

// ---------------------------------------------------------------- water ----

TEST_CASE("a liquid's frames run once per LiquidType period", "[stock][water]") {
    // 30 frames over 1250 ms: 24 a second.
    CHECK(liq::frameIndex(0, 1250, 30) == 0);
    CHECK(liq::frameIndex(625, 1250, 30) == 15);
    CHECK(liq::frameIndex(1249, 1250, 30) == 29);
    CHECK(liq::frameIndex(1250, 1250, 30) == 0);
    CHECK(liq::frameIndex(1000, 1250, 0) == 0);
}

TEST_CASE("magma takes 1250 ms whatever the row says", "[stock][water]") {
    CHECK(liq::framePeriodMs(800, true) == 1250u);
    CHECK(liq::framePeriodMs(0, false) == 1250u);
    CHECK(liq::framePeriodMs(2000, false) == 2000u);
}

TEST_CASE("magma scrolls by repeats per second", "[stock][water]") {
    // 0.025 a second: a 40 s period.
    CHECK(liq::scrollOffset(20000, 0.025f) == Approx(0.5f));
    CHECK(liq::scrollOffset(40000, 0.025f) == Approx(0.0f));
    CHECK(liq::scrollOffset(1234, 0.0f) == 0.0f);
}

TEST_CASE("the water's light saturates per channel", "[stock][water]") {
    const glm::vec3 l = liq::surfaceLight({0.5f, 0.4f, 0.3f}, {0.8f, 0.4f, 0.2f}, 1.0f);
    CHECK(l.r == Approx(1.0f));
    CHECK(l.g == Approx(0.8f));
    CHECK(l.b == Approx(0.5f));
    CHECK(liq::surfaceLight({0.2f, 0.2f, 0.2f}, {1, 1, 1}, -1.0f).r == Approx(0.2f));
}

TEST_CASE("the depth ramp steps a sixty-fourth a row", "[stock][water]") {
    const glm::vec3 close(0.0f), far(1.0f);
    const glm::vec4 r0 = liq::depthRampRow(close, far, 0.5f, 1.0f, 0, false);
    CHECK(r0.r == Approx(0.0f));
    CHECK(r0.a == Approx(0.5f));
    const glm::vec4 r32 = liq::depthRampRow(close, far, 0.5f, 1.0f, 32, false);
    CHECK(r32.r == Approx(0.5f));
    CHECK(r32.a == Approx(0.75f));
    // The last row stops a step short of the far end, and the ocean's is
    // darkened to 0.9.
    const glm::vec4 r63 = liq::depthRampRow(close, far, 0.5f, 1.0f, 63, false);
    CHECK(r63.r == Approx(63.0f / 64.0f));
    CHECK(liq::depthRampRow(close, far, 0.5f, 1.0f, 63, true).r == Approx(0.9f * 63.0f / 64.0f));
}

TEST_CASE("the water shimmers a quarter of the texture's alpha", "[stock][water]") {
    CHECK(liq::shimmer(glm::vec3(0.0f), 0.8f).r == Approx(0.2f));
}

// ------------------------------------------------------------------ sky ----

TEST_CASE("the dome's horizon is a quarter turn round the lowered sphere", "[stock][sky]") {
    CHECK(sky::domePolarFraction(1.0f) == Approx(0.0f).margin(1e-4));
    CHECK(sky::domePolarFraction(0.0f) == Approx(0.25f).margin(1e-4));
    // The first ring, 0.17 of a turn, sits about 16.8 degrees up.
    const float z = std::sin(16.8f * 3.14159265f / 180.0f);
    CHECK(sky::domePolarFraction(z) == Approx(0.17f).margin(0.003));
}

TEST_CASE("below the horizon the dome is the fog colour", "[stock][sky]") {
    sky::DomeColors c{};
    c.top = {0, 0, 1};
    c.middle = {0, 1, 0};
    c.fog = {1, 0, 0};
    CHECK(sky::domeColor({0.0f, 0.6f, -0.8f}, c, 0.0f).r == Approx(1.0f));
    CHECK(sky::domeColor({0.0f, 0.0f, 1.0f}, c, 0.0f).b == Approx(1.0f));
}

TEST_CASE("the dawn and dusk highlight is off through the day", "[stock][sky]") {
    CHECK(sky::highlight(0.5f, 1.0f) == Approx(0.0f));
    CHECK(sky::highlight(0.2708333f, 1.0f) == Approx(1.0f));
    CHECK(sky::highlight(0.2708333f, 0.0f) == 0.0f);
}

TEST_CASE("the sun is twice its size at sunrise", "[stock][sky]") {
    CHECK(sky::sunSize(0.25f) == Approx(2.0f));
    CHECK(sky::sunSize(0.5f) == Approx(1.0f));
}

TEST_CASE("the stars keep their own clock", "[stock][sky]") {
    CHECK(sky::starsAlpha(0.0f) == Approx(1.0f));
    CHECK(sky::starsAlpha(0.1f) == Approx(1.0f));   // 02:24
    CHECK(sky::starsAlpha(0.5f) == 0.0f);           // noon
    CHECK(sky::starsAlpha(0.19f) == 0.0f);          // past 04:30
}

TEST_CASE("a storm hides the sun at a quarter grade", "[stock][sky]") {
    CHECK(sky::stormFactor(0.1f) == Approx(0.4f));
    CHECK(sky::stormFactor(0.25f) == Approx(1.0f));
    CHECK(sky::stormFactor(0.9f) == Approx(1.0f));
}

TEST_CASE("the glare grows as the camera faces the sun", "[stock][sky]") {
    CHECK(sky::glareAlignment(0.5f, sky::kSunGlare) == 0.0f);
    CHECK(sky::glareAlignment(0.85f, sky::kSunGlare) == Approx(0.5f));
    CHECK(sky::glareAlignment(1.0f, sky::kSunGlare) == Approx(1.0f));
    // It eases up at 4 a second and down at 1.515.
    CHECK(sky::easeGlare(0.0f, 1.0f, 0.1f, sky::kSunGlare) == Approx(0.4f));
    CHECK(sky::easeGlare(1.0f, 0.0f, 0.1f, sky::kSunGlare) == Approx(1.0f - 0.15151515f));
}

TEST_CASE("cloud cover clears the sky below the noise", "[stock][sky]") {
    CHECK(sky::cloudAlpha(0.0f, 0.0f) == 0.0f);            // no cover: 128 - 255 < 0
    CHECK(sky::cloudAlpha(0.0f, 1.0f) == Approx(1.0f - std::pow(0.96f, 128.0f)));
    CHECK(sky::cloudRingAlpha(0.0f) == Approx(1.0f));
    CHECK(sky::cloudRingAlpha(11.0f) == Approx(0.0f));
}

// ------------------------------------------------------------------ WMO ----

TEST_CASE("MOCV is fixed on load as 0x7d7380 does", "[stock][wmo]") {
    const auto lit = wmo::fixColorVertexAlpha({200, 100, 50, 255}, false);
    CHECK(int(lit.r) == 255);
    CHECK(int(lit.g) == 249);
    CHECK(int(lit.b) == 124);
    CHECK(int(lit.a) == 255);
    const auto dark = wmo::fixColorVertexAlpha({200, 100, 50, 0}, false);
    CHECK(int(dark.r) == 100);
    CHECK(int(dark.b) == 25);
    const auto trans = wmo::fixColorVertexAlpha({200, 100, 50, 77}, true);
    CHECK(int(trans.r) == 100);
    CHECK(int(trans.a) == 77);
}

TEST_CASE("a group without MOCV is neutral grey, black when unified", "[stock][wmo]") {
    CHECK(int(wmo::noMocvColor(false).r) == 127);
    CHECK(int(wmo::noMocvColor(true).r) == 0);
}

TEST_CASE("the doorway fades over six and two thirds yards", "[stock][wmo]") {
    bool blocked = false;
    CHECK(wmo::portalTerm(0.0f, true, blocked) == Approx(1.0f));
    CHECK(wmo::portalTerm(2.0f, true, blocked) == Approx(0.7f));
    CHECK(wmo::portalTerm(7.0f, true, blocked) == 0.0f);
    CHECK_FALSE(blocked);
    CHECK(wmo::portalTerm(0.5f, false, blocked) == 0.0f);
    CHECK(blocked);
    CHECK(wmo::finishWeight(0.0005f) == 0.0f);
    CHECK(wmo::finishWeight(1.7f) == 1.0f);
    const auto v = wmo::attenTransVertex({100, 50, 25, 255}, 0.5f);
    CHECK(int(v.r) == 114);
    CHECK(int(v.a) == 128);
}

TEST_CASE("window glass glows only at night", "[stock][wmo]") {
    CHECK(wmo::sidnNightFactor(0.5f) == Approx(0.0f));
    CHECK(wmo::sidnNightFactor(0.0f) == Approx(1.0f));
    CHECK(wmo::sidnNightFactor(0.2708333f) == Approx(0.5f).margin(0.01));
}

TEST_CASE("the WMO light clamps before it doubles", "[stock][wmo]") {
    const glm::vec3 l = wmo::vertexLight({0.6f, 0.6f, 0.6f}, {0.8f, 0.2f, 0.0f}, 1.0f);
    CHECK(l.r == Approx(1.0f));
    CHECK(l.g == Approx(0.8f));
    const glm::vec3 c = wmo::mapObjLit({0.5f, 0.5f, 0.5f}, l, glm::vec3(0.0f));
    CHECK(c.r == Approx(1.0f));
    CHECK(c.g == Approx(0.8f));
    // Unified roots add the vertex colour to half the light.
    CHECK(wmo::mapObjULit(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(0.0f)).r ==
          Approx(2.0f * 127.0f / 255.0f));
    const auto w = wmo::windowLight({1, 0, 0}, {0, 0, 1});
    CHECK(w.diffuse.r == Approx(0.5f));
    CHECK(w.ambient.g == Approx(16.0f / 255.0f));
}

// -------------------------------------------------------- weather / glow ----

TEST_CASE("no rain falls below a quarter grade", "[stock][weather]") {
    CHECK(fx::particleLevel(0.2f) == 0.0f);
    CHECK(fx::particleLevel(0.25f) == 0.0f);
    CHECK(fx::particleLevel(1.0f) == Approx(1.0f));
    CHECK(fx::particleLevel(0.625f) == Approx(0.5f));
}

TEST_CASE("the storm light is whole at a quarter grade", "[stock][weather]") {
    CHECK(fx::stormLight(0.1f) == Approx(0.4f));
    CHECK(fx::stormLight(0.5f) == Approx(1.0f));
    CHECK(fx::stormLight(0.5f, 0.0f) == 0.0f);   // the coloured sandstorms
    CHECK(fx::stormTransitionSeconds(0.0f, 0.25f) == Approx(10.01f));
}

TEST_CASE("weatherDensity 2 is two thirds", "[stock][weather]") {
    CHECK(fx::densityScale(fx::kDefaultDensityLevel) == Approx(0.66f));
    CHECK(fx::emitRate(fx::WeatherKind::Rain, 1.0f, 0.66f) == Approx(35000.0f * 0.66f));
    CHECK(fx::mistRate(fx::WeatherKind::Rain, 0.5f, 1.0f) == 0.0f);
    CHECK(fx::mistDistanceFade(6.0f) == Approx(0.0f));
    CHECK(fx::mistDistanceFade(18.0f) == Approx(1.0f));
}

TEST_CASE("the glow is the blur squared times LightParams.Glow", "[stock][glow]") {
    CHECK(fx::glowAmount(0.5f) == Approx(127.0f / 255.0f));
    CHECK(fx::glowBlend(true, 0.0f) == Approx(84.0f / 255.0f));
    CHECK(fx::glowBlend(false, 0.0f) == 0.0f);
    const glm::vec3 o = fx::glowCombine(glm::vec3(0.2f), glm::vec3(0.8f), 0.5f, 0.0f);
    CHECK(o.r == Approx(0.2f + 0.64f * 0.5f));
    float sum = 0.0f;
    for (float w : fx::kGaussWeights) sum += w;
    CHECK(sum == Approx(1.0f));
}

TEST_CASE("the ghost world is grey with a teal mid-tone", "[stock][glow]") {
    const glm::vec3 black = fx::deathCombine(glm::vec3(0.0f), glm::vec3(0.0f), 0.8f);
    CHECK(black.r == Approx(0.0f));
    const glm::vec3 mid = fx::deathCombine(glm::vec3(0.5f), glm::vec3(0.0f), 0.0f);
    const float grey = 0.5f * (0.299f + 0.587f + 0.144f);
    const float t = 4.0f * grey * (1.0f - grey);
    CHECK(mid.r == Approx(grey + fx::kDeathTint.r * t));
    CHECK(mid.b == Approx(grey + fx::kDeathTint.b * t));
}
