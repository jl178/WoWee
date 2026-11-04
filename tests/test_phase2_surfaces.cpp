// Unstretched cliffs (E16), sunlight under water (E15) and soft highlights
// (E18): the rules each shader carries, held here on the CPU copies
// (cliff_projection.hpp, water_caustics.hpp, highlight_rolloff.hpp).
#include <catch_amalgamated.hpp>

#include "rendering/cliff_projection.hpp"
#include "rendering/highlight_rolloff.hpp"
#include "rendering/water_caustics.hpp"

#include <cmath>

using namespace wowee::rendering;

namespace {
// A unit normal tilted `deg` from straight up, its horizontal part turned
// `azimuthDeg` from world x.
void tilted(float deg, float azimuthDeg, float& nx, float& ny, float& nz) {
    const float a = deg * 3.14159265f / 180.0f;
    const float b = azimuthDeg * 3.14159265f / 180.0f;
    nx = std::sin(a) * std::cos(b);
    ny = std::sin(a) * std::sin(b);
    nz = std::cos(a);
}
}  // namespace

TEST_CASE("cliffs: ground up to 45 degrees is read top-down only, as stock", "[enhancements][cliff]") {
    for (float deg = 0.0f; deg <= 44.9f; deg += 0.5f) {
        for (float az = 0.0f; az < 360.0f; az += 15.0f) {
            float nx, ny, nz;
            tilted(deg, az, nx, ny, nz);
            const auto w = cliff_projection::weightsFor(nx, ny, nz);
            INFO(deg << " deg, azimuth " << az);
            // Exactly: the shader takes the stock branch only when top == 1.
            CHECK(w.top == 1.0f);
            CHECK(w.sideX == 0.0f);
            CHECK(w.sideY == 0.0f);
        }
    }
}

TEST_CASE("cliffs: the side reads take over between 45 and 62 degrees", "[enhancements][cliff]") {
    float prevTop = 1.0f;
    for (float deg = 45.0f; deg <= 90.0f; deg += 1.0f) {
        float nx, ny, nz;
        tilted(deg, 20.0f, nx, ny, nz);
        const auto w = cliff_projection::weightsFor(nx, ny, nz);
        INFO(deg << " deg");
        CHECK(w.top + w.sideX + w.sideY == Catch::Approx(1.0f).margin(1e-5));
        CHECK(w.top <= prevTop + 1e-6f);  // never more top-down as it steepens
        prevTop = w.top;
        if (deg >= 62.5f) CHECK(w.top == 0.0f);
    }
    // A face looking along x takes the x projection, along y the y one.
    float nx, ny, nz;
    tilted(75.0f, 0.0f, nx, ny, nz);
    CHECK(cliff_projection::weightsFor(nx, ny, nz).sideX == Catch::Approx(1.0f));
    tilted(75.0f, 90.0f, nx, ny, nz);
    CHECK(cliff_projection::weightsFor(nx, ny, nz).sideY == Catch::Approx(1.0f));
    // Where it starts, the stock read stretches a texture by no more than about 40%.
    CHECK(cliff_projection::topDownStretch(cliff_projection::kFlatNormalZ) < 1.42f);
}

TEST_CASE("cliffs: a weight too small to see is not read", "[enhancements][cliff]") {
    for (float az = 0.0f; az <= 90.0f; az += 1.0f) {
        float nx, ny, nz;
        tilted(70.0f, az, nx, ny, nz);
        const auto w = cliff_projection::weightsFor(nx, ny, nz);
        CHECK((w.sideX == 0.0f || w.sideX >= cliff_projection::kSkip));
        CHECK((w.sideY == 0.0f || w.sideY >= cliff_projection::kSkip));
        CHECK((w.top == 0.0f || w.top >= cliff_projection::kSkip));
        CHECK(w.top + w.sideX + w.sideY == Catch::Approx(1.0f).margin(1e-5));
    }
}

TEST_CASE("soft highlights: off, or with no glow, the composite is untouched", "[enhancements][highlight]") {
    const auto off = highlight_rolloff::curveForGlow(false, 0.6f);
    const auto noGlow = highlight_rolloff::curveForGlow(true, 0.0f);
    CHECK(off.white == 0.0f);
    CHECK(noGlow.white == 0.0f);
    for (float x = 0.0f; x <= 2.0f; x += 0.01f) {
        CHECK(highlight_rolloff::apply(x, off) == x);
        CHECK(highlight_rolloff::apply(x, noGlow) == x);
    }
}

TEST_CASE("soft highlights: below the knee every value is the stock one", "[enhancements][highlight]") {
    for (float g = 0.05f; g <= 1.0f; g += 0.05f) {
        const auto k = highlight_rolloff::curveForGlow(true, g);
        // Every 8-bit level at or under the knee, exactly.
        for (int i = 0; i <= 255; ++i) {
            const float x = static_cast<float>(i) / 255.0f;
            if (x > highlight_rolloff::kKnee) break;
            CHECK(highlight_rolloff::apply(x, k) == x);
        }
    }
}

TEST_CASE("soft highlights: a smooth shoulder that reaches white and never clips", "[enhancements][highlight]") {
    for (float g = 0.05f; g <= 1.0f; g += 0.05f) {
        const auto k = highlight_rolloff::curveForGlow(true, g);
        INFO("glow " << g << " white " << k.white << " rate " << k.c);
        CHECK(k.white == Catch::Approx(1.0f + g));
        // The brightest the glow composite can make a channel lands on 1.
        CHECK(highlight_rolloff::apply(k.white, k) == Catch::Approx(1.0f).margin(1e-5));
        CHECK(highlight_rolloff::apply(k.white + 0.5f, k) <= 1.0f);
        // Slope 1 at the knee: no kink where the curve takes over.
        const float e = 1e-3f;
        const float slope = (highlight_rolloff::apply(highlight_rolloff::kKnee + e, k) -
                             highlight_rolloff::kKnee) / e;
        CHECK(slope == Catch::Approx(1.0f).margin(0.02));
        // Rising all the way, and below 1 until the white point.
        float prev = highlight_rolloff::kKnee;
        for (float x = highlight_rolloff::kKnee + 0.01f; x < k.white; x += 0.01f) {
            const float y = highlight_rolloff::apply(x, k);
            CHECK(y > prev);
            // Strictly under white until the last few hundredths, where it
            // meets 1 within a float's rounding.
            CHECK(y <= 1.0f);
            if (x < k.white - 0.05f) CHECK(y < 1.0f);
            prev = y;
        }
    }
}

TEST_CASE("sunlight under water: only by day, only in shallow water, only near", "[enhancements][caustics]") {
    // Night and the sun on the horizon: nothing.
    CHECK(water_caustics::dayStrength(-0.5f) == 0.0f);
    CHECK(water_caustics::dayStrength(0.0f) == 0.0f);
    CHECK(water_caustics::dayStrength(1.0f) == 1.0f);
    CHECK(water_caustics::dayStrength(0.1f) > 0.0f);
    CHECK(water_caustics::dayStrength(0.1f) < 1.0f);

    // The waterline focuses nothing, the net is full a little under, and
    // gone by four yards.
    CHECK(water_caustics::depthFade(0.0f) == 0.0f);
    CHECK(water_caustics::depthFade(-1.0f) == 0.0f);
    CHECK(water_caustics::depthFade(water_caustics::kDepthPeak) == Catch::Approx(1.0f));
    CHECK(water_caustics::depthFade(water_caustics::kDepthOut) == 0.0f);
    CHECK(water_caustics::depthFade(10.0f) == 0.0f);
    float prev = 1.0f;
    for (float d = water_caustics::kDepthPeak; d <= water_caustics::kDepthOut; d += 0.1f) {
        const float f = water_caustics::depthFade(d);
        CHECK(f <= prev + 1e-6f);
        prev = f;
    }

    CHECK(water_caustics::distanceFade(10.0f) == 1.0f);
    CHECK(water_caustics::distanceFade(water_caustics::kFarEnd) == 0.0f);

    // The most it brightens the ground stays a gentle lift, not a glare.
    CHECK(water_caustics::kMaxGain <= 0.4f);

    // A surface wholly past the net's reach is not drawn again; one the eye
    // stands over, or reaching to within it, is.
    const glm::vec3 eye(0.0f, 0.0f, 10.0f);
    CHECK(water_caustics::beyondReach({200, 200, 0}, {50, 0, 0}, {0, 50, 0}, 0, 0, eye));
    CHECK_FALSE(water_caustics::beyondReach({-100, -100, 0}, {200, 0, 0}, {0, 200, 0}, 0, 0, eye));
    CHECK_FALSE(water_caustics::beyondReach({60, 0, 0}, {50, 0, 0}, {0, 50, 0}, 0, 0, eye));

    // Water and ocean, outdoors; never magma, slime or water in a building.
    CHECK(water_caustics::liquidTakesCaustics(0, false));
    CHECK(water_caustics::liquidTakesCaustics(1, false));
    CHECK_FALSE(water_caustics::liquidTakesCaustics(2, false));
    CHECK_FALSE(water_caustics::liquidTakesCaustics(3, false));
    CHECK_FALSE(water_caustics::liquidTakesCaustics(0, true));
}
