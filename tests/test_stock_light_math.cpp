// The stock client's light arithmetic (rendering/stock_light_math.hpp), held
// to what Wow.exe 3.3.5a does at the addresses the header names.
#include <catch_amalgamated.hpp>

#include "rendering/stock_light_math.hpp"

#include <cmath>
#include <cstdint>

namespace sl = wowee::rendering::stock_light;
using Catch::Approx;

// ---------------------------------------------------------------- bands ----

TEST_CASE("the band key is the half-minute, rounded as the client rounds it", "[light][stock]") {
    CHECK(sl::bandTime(0.0f) == 0);
    CHECK(sl::bandTime(0.5f) == 1440);           // noon
    CHECK(sl::bandTime(0.25f) == 720);           // six in the morning
    // round(x - 0.5): just under a whole half-minute stays on the one before.
    CHECK(sl::bandTime(100.9f / 2880.0f) == 100);
    CHECK(sl::bandTime(1.0f) == 0);              // midnight wraps
}

TEST_CASE("a band interpolates between the keys around the time", "[light][stock]") {
    const uint16_t times[] = {0, 720, 1440, 2160};
    const float values[] = {0.0f, 10.0f, 20.0f, 30.0f};
    CHECK(sl::sampleFloatBand(times, values, 4, 0) == Approx(0.0f));
    CHECK(sl::sampleFloatBand(times, values, 4, 360) == Approx(5.0f));
    CHECK(sl::sampleFloatBand(times, values, 4, 1440) == Approx(20.0f));
    // Past the last key: between it and the first, through midnight.
    CHECK(sl::sampleFloatBand(times, values, 4, 2520) == Approx(15.0f));
}

TEST_CASE("keys that do not start at midnight wrap from the last to the first", "[light][stock]") {
    const uint16_t times[] = {600, 1800};
    const float values[] = {1.0f, 3.0f};
    // 1800 -> 600 is 1680 half-minutes long, through midnight.
    CHECK(sl::sampleFloatBand(times, values, 2, 0) == Approx(3.0f + (1.0f - 3.0f) * (1080.0f / 1680.0f)));
    CHECK(sl::sampleFloatBand(times, values, 2, 2400) == Approx(3.0f + (1.0f - 3.0f) * (600.0f / 1680.0f)));
    CHECK(sl::sampleFloatBand(times, values, 2, 1200) == Approx(2.0f));
}

TEST_CASE("one key is that value all day", "[light][stock]") {
    const uint16_t times[] = {1000};
    const float values[] = {7.0f};
    for (int t : {0, 999, 1000, 1001, 2879}) CHECK(sl::sampleFloatBand(times, values, 1, t) == Approx(7.0f));
}

TEST_CASE("a band with no keys is black or zero", "[light][stock]") {
    const uint16_t times[1] = {0};
    const uint32_t colors[1] = {0x123456};
    const float values[1] = {9.0f};
    CHECK(sl::sampleColorBand(times, colors, 0, 100) == 0u);
    CHECK(sl::sampleFloatBand(times, values, 0, 100) == 0.0f);
}

TEST_CASE("a colour band is interpolated byte by byte and rounded", "[light][stock]") {
    const uint16_t times[] = {0, 100};
    const uint32_t colors[] = {0x000000, 0xFF0A01};
    // Halfway: 127.5 -> 128, 5 -> 5, 0.5 -> 0 - the FPU's default
    // rounding, ties to even.
    CHECK(sl::sampleColorBand(times, colors, 2, 50) == 0x800500u);
    CHECK(sl::lerpByte(0, 3, 0.5f) == 2);
    CHECK(sl::lerpByte(0, 255, 0.5f) == 128);
    CHECK(sl::lerpByte(200, 100, 0.25f) == 175);
}

// --------------------------------------------------------- light volumes ----

TEST_CASE("a light volume weighs one inside, falling linearly to zero at the edge", "[light][stock]") {
    CHECK(sl::volumeWeight(0.0f, 10.0f, 30.0f) == 1.0f);
    CHECK(sl::volumeWeight(10.0f, 10.0f, 30.0f) == 1.0f);
    CHECK(sl::volumeWeight(20.0f, 10.0f, 30.0f) == Approx(0.5f));
    CHECK(sl::volumeWeight(25.0f, 10.0f, 30.0f) == Approx(0.25f));
    CHECK(sl::volumeWeight(30.0f, 10.0f, 30.0f) == 0.0f);
    CHECK(sl::volumeWeight(45.0f, 10.0f, 30.0f) == 0.0f);
}

TEST_CASE("the farther light is painted first, so the nearer ends on top", "[light][stock]") {
    const sl::VolumeKey near{.distanceSq = 100.0f, .innerRadius = 5.0f, .centre = {0, 0, 0}};
    const sl::VolumeKey far{.distanceSq = 900.0f, .innerRadius = 50.0f, .centre = {100, 0, 0}};
    CHECK(sl::paintedBefore(far, near));
    CHECK_FALSE(sl::paintedBefore(near, far));
}

TEST_CASE("lights on one spot are painted biggest first", "[light][stock]") {
    // Within a third of a yard of each other the distance is no guide; the
    // smaller inner radius, the more particular light, goes on top.
    const sl::VolumeKey big{.distanceSq = 400.0f, .innerRadius = 80.0f, .centre = {10, 10, 0}};
    const sl::VolumeKey small{.distanceSq = 401.0f, .innerRadius = 20.0f, .centre = {10.2f, 10, 0}};
    CHECK(sl::paintedBefore(big, small));
    CHECK_FALSE(sl::paintedBefore(small, big));
}

TEST_CASE("painting three lights is not a normalised average", "[light][stock]") {
    // The default light, then a far light at half weight, then a near one
    // at a quarter: each moves the running result toward itself.
    float v = 0.0f;                          // default light
    v = v + (100.0f - v) * 0.5f;             // far light, weight 0.5
    v = v + (200.0f - v) * 0.25f;            // near light, weight 0.25
    CHECK(v == Approx(87.5f));
}

// ------------------------------------------------------------------ fog ----

TEST_CASE("Azeroth fog is linear from start to the band's end", "[light][stock][fog]") {
    const auto lf = sl::lightFog(300.0f, 0.25f, 1277.0f, false);
    CHECK(lf.end == Approx(300.0f));
    CHECK(lf.scaler == Approx(0.25f));
    CHECK(lf.exponent == 1.0f);
    const auto fog = sl::frameFog(lf, 1277.0f, false, false);
    CHECK(fog.start == Approx(75.0f));
    CHECK(fog.end == Approx(300.0f));
    CHECK(sl::fogVisibility(50.0f, fog) == 1.0f);
    CHECK(sl::fogVisibility(187.5f, fog) == Approx(0.5f));
    CHECK(sl::fogVisibility(400.0f, fog) == 0.0f);
}

TEST_CASE("the fog ends by the far clip", "[light][stock][fog]") {
    const auto lf = sl::lightFog(2000.0f, 0.5f, 800.0f, false);
    const auto fog = sl::frameFog(lf, 800.0f, false, false);
    CHECK(fog.end == Approx(800.0f));
    CHECK(fog.start == Approx(400.0f));
}

TEST_CASE("the band can bring the fog no closer than ten yards", "[light][stock][fog]") {
    CHECK(sl::lightFog(0.0f, 0.5f, 1000.0f, false).end == Approx(10.0f));
    CHECK(sl::lightFog(3.0f, 0.5f, 1000.0f, false).end == Approx(10.0f));
}

TEST_CASE("the start fraction is clamped to -1..1", "[light][stock][fog]") {
    CHECK(sl::lightFog(300.0f, 4.0f, 1000.0f, false).scaler == Approx(1.0f));
    CHECK(sl::lightFog(300.0f, -3.0f, 1000.0f, false).scaler == Approx(-1.0f));
    // On a far-clip map a negative start is pulled to zero.
    CHECK(sl::lightFog(300.0f, -0.5f, 1000.0f, true).scaler == 0.0f);
}

TEST_CASE("Outland and Northrend fog runs to the far clip on a curve", "[light][stock][fog]") {
    CHECK(sl::usesFarClipFog(530));
    CHECK(sl::usesFarClipFog(571));
    CHECK_FALSE(sl::usesFarClipFog(0));
    CHECK_FALSE(sl::usesFarClipFog(1));

    // Band fog 75..300 yards: 225 wide against a 500-yard span (min(700,
    // far) - 200) -> (1 - 225/500) * 5.5 + 1.5 = 4.525.
    const auto lf = sl::lightFog(300.0f, 0.25f, 1277.0f, true);
    CHECK(lf.end == Approx(1277.0f));
    CHECK(lf.exponent == Approx(4.525f));
    const auto fog = sl::frameFog(lf, 1277.0f, true, false);
    CHECK(fog.end == Approx(1277.0f));
    CHECK(fog.start == Approx(1277.0f * 0.25f));
    // Halfway through the fog only 0.5^4.525 of the surface still shows
    // (c30 = (-k, k*end, exponent), 0x873210): the power keeps a short band
    // fog thick although its end moved out to the far clip.
    const float mid = (fog.start + fog.end) * 0.5f;
    CHECK(sl::fogVisibility(mid, fog) == Approx(std::pow(0.5f, 4.525f)));
    CHECK(sl::fogVisibility(fog.start, fog) == 1.0f);
    CHECK(sl::fogVisibility(fog.end, fog) == 0.0f);
}

TEST_CASE("a wide band fog keeps the gentlest curve", "[light][stock][fog]") {
    CHECK(sl::fogExponent(0.0f, 900.0f, 1277.0f) == Approx(1.5f));
    CHECK(sl::fogExponent(100.0f, 100.0f, 1277.0f) == Approx(7.0f));
    // A short far clip narrows the span: min(700, 450) - 200 = 250.
    CHECK(sl::fogExponent(0.0f, 125.0f, 450.0f) == Approx(0.5f * 5.5f + 1.5f));
}

TEST_CASE("a band fog end under 1000/36 yards stays where it is", "[light][stock][fog]") {
    const auto lf = sl::lightFog(20.0f, 0.5f, 1277.0f, true);
    CHECK(lf.end == Approx(20.0f));
    CHECK(lf.exponent == 1.0f);
}

TEST_CASE("under liquid on a far-clip map the curve doubles", "[light][stock][fog]") {
    const sl::LightFog lf{.end = 1000.0f, .scaler = 0.1f, .exponent = 2.0f};
    CHECK(sl::frameFog(lf, 1000.0f, true, true).exponent == Approx(4.0f));
    CHECK(sl::frameFog(lf, 1000.0f, true, false).exponent == Approx(2.0f));
    CHECK(sl::frameFog(lf, 1000.0f, false, true).exponent == Approx(2.0f));
}

// -------------------------------------------------------------- terrain ----

TEST_CASE("the terrain light clamps each channel at one", "[light][stock][terrain]") {
    const glm::vec3 amb(0.4f, 0.3f, 0.5f), diff(0.9f, 0.8f, 0.3f);
    const glm::vec3 l = sl::terrainLight(amb, diff, 1.0f);
    CHECK(l.r == Approx(1.0f));
    CHECK(l.g == Approx(1.0f));
    CHECK(l.b == Approx(0.8f));
    // Facing away from the sun is the ambient alone.
    const glm::vec3 back = sl::terrainLight(amb, diff, -0.6f);
    CHECK(back.r == Approx(0.4f));
    CHECK(back.b == Approx(0.5f));
}

TEST_CASE("a terrain shadow leaves seventy percent", "[light][stock][terrain]") {
    CHECK(sl::terrainShadowScale(1.0f) == Approx(1.0f));
    CHECK(sl::terrainShadowScale(0.0f) == Approx(0.7f));
    CHECK(sl::terrainShadowScale(0.5f) == Approx(0.85f));
}
