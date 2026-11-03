// The logic behind three phase 2 enhancements (docs/enhancements-plan.md):
// E4 soft particles (the fade the late pass's shaders compute), E2 fade-in
// for streamed scenery (the clock, the byte the M2 draw packs and the dither)
// and E5 rain ripples (the strength the renderer eases). Each has a row in
// kEnhancementRows whose stock value turns it off; test_enhancements_stock
// holds those, and the "off is stock" checks here hold the parts that run
// with the row off.
#include <catch_amalgamated.hpp>

#include "rendering/rain_ripples.hpp"
#include "rendering/soft_particle_fade.hpp"
#include "rendering/stream_fade.hpp"
#include "rendering/enhancements.hpp"
#include "ui/enhancement_rows.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <string>

using namespace wowee::rendering;

namespace {
// The camera's projection (camera.cpp): stock fov 0.6 * 90 degrees.
glm::mat4 cameraProjection() {
    return glm::perspective(glm::radians(54.0f), 1251.0f / 986.0f, 0.5f, 2400.0f);
}
float depthAt(const glm::mat4& p, float eye) {
    const glm::vec4 c = p * glm::vec4(0.0f, 0.0f, -eye, 1.0f);
    return c.z / c.w;
}
}  // namespace

// ---------------------------------------------------------------- E4
TEST_CASE("soft particles: the depth buffer turns back into yards", "[enhancements][soft]") {
    const glm::mat4 p = cameraProjection();
    for (float eye : {0.5f, 1.0f, 7.5f, 40.0f, 300.0f, 2400.0f}) {
        INFO("eye distance " << eye);
        CHECK(soft_particle::eyeDistance(p, depthAt(p, eye)) == Catch::Approx(eye).epsilon(1e-3));
    }
}

TEST_CASE("soft particles: the fade runs over the range in front of the scene", "[enhancements][soft]") {
    const glm::mat4 p = cameraProjection();
    const float wall = depthAt(p, 20.0f);
    const float range = 0.5f;
    // Behind the wall: discarded, as the depth test would.
    CHECK(soft_particle::fade(p, wall, depthAt(p, 20.2f), range) < 0.0f);
    // At the wall: nothing left, so no line where the quad cuts it.
    CHECK(soft_particle::fade(p, wall, wall, range) == Catch::Approx(0.0f).margin(1e-3));
    // Half way: half, and smooth.
    CHECK(soft_particle::fade(p, wall, depthAt(p, 19.75f), range) == Catch::Approx(0.5f).margin(0.02));
    // A range or more in front: the stock particle, untouched.
    CHECK(soft_particle::fade(p, wall, depthAt(p, 19.5f), range) == Catch::Approx(1.0f).margin(1e-3));
    CHECK(soft_particle::fade(p, wall, depthAt(p, 5.0f), range) == 1.0f);
    // Flat world quads keep the hard test and no fade.
    CHECK(soft_particle::fade(p, wall, wall, 0.0f) == 1.0f);
}

TEST_CASE("soft particles: the fade depth follows the quad's size", "[enhancements][soft]") {
    CHECK(soft_particle::fadeRange(0.1f, 0.1f, false) == 0.15f);   // a spark: inches
    CHECK(soft_particle::fadeRange(1.0f, 0.6f, false) == 0.5f);    // a flame
    CHECK(soft_particle::fadeRange(8.0f, 8.0f, false) == 1.5f);    // a smoke column, held
    CHECK(soft_particle::fadeRange(4.0f, 4.0f, true) == 0.0f);     // a ground glow
}

// ---------------------------------------------------------------- E2
TEST_CASE("stream fade: unstamped things are never hidden", "[enhancements][streamfade]") {
    // The row off stamps nothing (-1): the draw's flags are the stock ones.
    for (float now : {0.0f, 0.1f, 100.0f}) {
        CHECK(stream_fade::hidden(-1.0f, now) == 0.0f);
        CHECK(stream_fade::m2Flags(-1.0f, now) == 0);
    }
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) CHECK_FALSE(stream_fade::pixelHidden(0.0f, x, y));
}

TEST_CASE("stream fade: a stamped thing dissolves in over kSeconds", "[enhancements][streamfade]") {
    const float t0 = 50.0f;
    CHECK(stream_fade::hidden(t0, t0) == 1.0f);
    CHECK(stream_fade::hidden(t0, t0 + stream_fade::kSeconds * 0.5f) == Catch::Approx(0.5f));
    CHECK(stream_fade::hidden(t0, t0 + stream_fade::kSeconds) == 0.0f);
    CHECK(stream_fade::hidden(t0, t0 + 10.0f) == 0.0f);
    float last = 1.0f;
    for (int i = 1; i <= 20; ++i) {
        const float h = stream_fade::hidden(t0, t0 + stream_fade::kSeconds * i / 20.0f);
        CHECK(h <= last);
        last = h;
    }
    // Once in, the flags are the stock ones again.
    CHECK(stream_fade::m2Flags(t0, t0 + 1.0f) == 0);
}

TEST_CASE("stream fade: the byte rides above the M2 flags the shader reads", "[enhancements][streamfade]") {
    // m2.vert reads bits 0-23 (afloat, ground normal bytes, skinned, WMO
    // light); the fade owns 24-31 and leaves the rest alone.
    const int32_t f = stream_fade::m2Flags(0.0f, 0.0f);
    CHECK((f & 0x00FFFFFF) == 0);
    CHECK(((static_cast<uint32_t>(f) >> 24) & 0xFF) == 255u);
    CHECK(stream_fade::hideByte(0.0f) == 0u);
    CHECK(stream_fade::hideByte(1.0f) == 255u);
    // Rounded up: never shown whole before its time.
    CHECK(stream_fade::hideByte(0.001f) == 1u);
    for (float h = 0.0f; h <= 1.0f; h += 0.05f) {
        const float back = static_cast<float>(stream_fade::hideByte(h)) / 255.0f;
        CHECK(back >= h - 1e-6f);
        CHECK(back - h < 1.0f / 255.0f + 1e-6f);
    }
}

TEST_CASE("stream fade: the dither drops the hidden share of a 4x4 block", "[enhancements][streamfade]") {
    for (int k = 0; k <= 16; ++k) {
        const float hide = k / 16.0f;
        int dropped = 0;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) dropped += stream_fade::pixelHidden(hide, x, y) ? 1 : 0;
        INFO("hide " << hide);
        CHECK(dropped == k);
    }
}

// ---------------------------------------------------------------- E5
TEST_CASE("rain ripples: only falling water rings the surface", "[enhancements][ripples]") {
    CHECK(rainRippleTarget(false, false, 1.0f) == 0.0f);   // fine weather or snow
    CHECK(rainRippleTarget(true, false, 0.0f) == 0.0f);
    CHECK(rainRippleTarget(true, false, 0.2f) > 0.0f);
    CHECK(rainRippleTarget(true, false, 1.0f) > rainRippleTarget(true, false, 0.2f));
    CHECK(rainRippleTarget(false, true, 1.0f) == 1.0f);
    CHECK(rainRippleTarget(false, true, 0.1f) > rainRippleTarget(true, false, 0.1f));
}

TEST_CASE("rain ripples: eased in and out, and dry lands on zero", "[enhancements][ripples]") {
    float s = 0.0f;
    for (int i = 0; i < 60; ++i) s = easeRainRipple(s, 1.0f, 1.0f / 60.0f);  // one second
    CHECK(s > 0.5f);
    CHECK(s < 0.8f);
    for (int i = 0; i < 300; ++i) s = easeRainRipple(s, 1.0f, 1.0f / 60.0f);
    CHECK(s > 0.95f);
    CHECK(s <= 1.0f);
    // Stops: dies away, then is exactly 0 so the shaders skip the block.
    for (int i = 0; i < 60 * 12; ++i) s = easeRainRipple(s, 0.0f, 1.0f / 60.0f);
    CHECK(s == 0.0f);
    // No overshoot on a long frame.
    CHECK(easeRainRipple(0.0f, 0.7f, 100.0f) <= 0.7f);
}

// ---------------------------------------------------------------- rows
TEST_CASE("the three rows are off in Classic and on in the recommended set", "[enhancements]") {
    using namespace wowee::ui;
    for (const char* key : {"softparticles", "streamfadein", "rainripples"}) {
        INFO(key);
        const EnhancementRow* r = findEnhancementRow(key);
        REQUIRE(r != nullptr);
        CHECK(r->stockValue == 0.0f);
        CHECK(r->recommendedValue == 1.0f);
        CHECK(enhancementValue(Enhancements{}, *r) == 0.0f);
        CHECK(enhancementValue(recommendedEnhancements(), *r) == 1.0f);
    }
}
