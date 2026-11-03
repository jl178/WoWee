// The logic behind the phase 2 enhancements (docs/enhancements-plan.md):
// E4 soft particles (the fade the late pass's shaders compute, and where the
// water draws around their pass), E2 fade-in for streamed scenery (the clock,
// the byte the M2 draw packs and the dither), E5 rain ripples (the strength
// the renderer eases), E8 corpses settling to the slope (the plane fit and the
// ease) and E21 footfall dust (which ground, how much). Each has a row in
// kEnhancementRows whose stock value turns it off; test_enhancements_stock
// holds those, and the "off is stock" checks here hold the parts that run
// with the row off.
#include <catch_amalgamated.hpp>

#include "rendering/corpse_settle.hpp"
#include "rendering/footfall_dust_rules.hpp"
#include "rendering/rain_ripples.hpp"
#include "rendering/water_pass_order.hpp"
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
TEST_CASE("the phase 2 rows are off in Classic and on in the recommended set", "[enhancements]") {
    using namespace wowee::ui;
    for (const char* key : {"softparticles", "streamfadein", "rainripples", "corpsesettle", "mountdust"}) {
        INFO(key);
        const EnhancementRow* r = findEnhancementRow(key);
        REQUIRE(r != nullptr);
        CHECK(r->stockValue == 0.0f);
        CHECK(r->recommendedValue == 1.0f);
        CHECK(enhancementValue(Enhancements{}, *r) == 0.0f);
        CHECK(enhancementValue(recommendedEnhancements(), *r) == 1.0f);
    }
}

// ---------------------------------------------------------------- E4 order
TEST_CASE("soft particles: off, the water draws where it always did", "[enhancements][soft]") {
    using namespace water_pass_order;
    // Every combination of the passes, with the late pass off: the old rule.
    for (int bits = 0; bits < 32; ++bits) {
        Frame f;
        f.msaa = bits & 1;
        f.offscreenScene = bits & 2;
        f.water1xPass = bits & 4;
        f.water1xKeepDepth = bits & 8;
        f.sceneContinuePass = bits & 16;
        const bool old = f.msaa ? (!f.offscreenScene && f.water1xPass) : f.sceneContinuePass;
        INFO("bits " << bits);
        CHECK(waterAfterScene(f) == old);
        CHECK_FALSE(weatherAfterScene(f));
    }
}

TEST_CASE("soft particles: on, the water and weather come after the late pass", "[enhancements][soft]") {
    using namespace water_pass_order;
    // The default: MSAA into the off-screen glow/FXAA target. The water used
    // to stay in the scene pass there, under the late particles.
    Frame f{.msaa = true, .offscreenScene = true, .softParticles = true,
            .water1xPass = true, .water1xKeepDepth = true, .sceneContinuePass = false};
    CHECK(waterAfterScene(f));
    CHECK(weatherAfterScene(f));
    // Without MSAA the water was already after them; the weather now is too.
    Frame g{.msaa = false, .offscreenScene = true, .softParticles = true, .sceneContinuePass = true};
    CHECK(waterAfterScene(g));
    CHECK(weatherAfterScene(g));
    // No pass to move into: everything stays in the scene pass.
    f.water1xKeepDepth = false;
    CHECK_FALSE(waterAfterScene(f));
    CHECK_FALSE(weatherAfterScene(f));
}

// ---------------------------------------------------------------- E8
TEST_CASE("corpse settle: level ground leaves the body exactly as stock", "[enhancements][corpse]") {
    namespace cs = corpse_settle;
    cs::FootprintHeights h{10.0f, 10.0f, 10.0f, 10.0f, 10.0f};
    const glm::vec3 n = cs::groundNormal(h, 0.7f, 1.0f);
    CHECK(n == glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::quat q = cs::tilt(n, 1.0f);
    CHECK(q == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    // And nothing at all with no settle amount, on any slope.
    CHECK(cs::tilt(glm::normalize(glm::vec3(0.3f, 0.0f, 1.0f)), 0.0f) == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
}

TEST_CASE("corpse settle: the plane follows the slope, whichever way the body faces", "[enhancements][corpse]") {
    namespace cs = corpse_settle;
    // Ground rising 0.2 yd per yd toward +X (about 11 degrees).
    auto groundZ = [](float x, float) { return 0.2f * x; };
    for (float yaw : {0.0f, 0.6f, 1.5708f, 3.0f, -2.2f}) {
        INFO("yaw " << yaw);
        const float half = 1.0f;
        const glm::vec2 ax(std::cos(yaw), std::sin(yaw)), ay(-std::sin(yaw), std::cos(yaw));
        cs::FootprintHeights h;
        h.centre = groundZ(0, 0);
        h.front = groundZ(ax.x * half, ax.y * half);
        h.back = groundZ(-ax.x * half, -ax.y * half);
        h.left = groundZ(ay.x * half, ay.y * half);
        h.right = groundZ(-ay.x * half, -ay.y * half);
        const glm::vec3 n = cs::groundNormal(h, yaw, half);
        const glm::vec3 want = glm::normalize(glm::vec3(-0.2f, 0.0f, 1.0f));
        CHECK(n.x == Catch::Approx(want.x).margin(1e-4));
        CHECK(n.y == Catch::Approx(want.y).margin(1e-4));
        CHECK(n.z == Catch::Approx(want.z).margin(1e-4));
        // The full tilt turns straight up onto that normal.
        const glm::vec3 up = cs::tilt(n, 1.0f) * glm::vec3(0.0f, 0.0f, 1.0f);
        CHECK(glm::length(up - n) < 1e-4f);
    }
}

TEST_CASE("corpse settle: a cliff is held to the steepest tilt, a missing end uses the centre", "[enhancements][corpse]") {
    namespace cs = corpse_settle;
    cs::FootprintHeights cliff{10.0f, 0.0f, 5.0f, 5.0f, 5.0f};  // 10 yd over 2 yd
    const glm::vec3 n = cs::groundNormal(cliff, 0.0f, 1.0f);
    CHECK(glm::degrees(std::acos(n.z)) == Catch::Approx(cs::kMaxTiltDegrees).margin(0.01));
    CHECK(n.x < 0.0f);  // leaning back down the slope
    // Front missing (off a ledge): the slope from the back to the centre.
    cs::FootprintHeights ledge{std::nullopt, 0.0f, 0.1f, 0.1f, 0.1f};
    const glm::vec3 m = cs::groundNormal(ledge, 0.0f, 1.0f);
    CHECK(m == cs::groundNormal(cs::FootprintHeights{0.2f, 0.0f, 0.1f, 0.1f, 0.1f}, 0.0f, 1.0f));
    // Nothing anywhere: level.
    CHECK(cs::groundNormal(cs::FootprintHeights{}, 0.0f, 1.0f) == glm::vec3(0.0f, 0.0f, 1.0f));
}

TEST_CASE("corpse settle: eases in over the end of the fall and out on rising", "[enhancements][corpse]") {
    namespace cs = corpse_settle;
    CHECK_FALSE(cs::settling(false, -1.0f));
    CHECK_FALSE(cs::settling(true, 900.0f));      // still well into the fall
    CHECK(cs::settling(true, 300.0f));            // its last 0.3 s
    CHECK(cs::settling(true, -1.0f));             // lying there
    float a = 0.0f;
    for (int i = 0; i < 18; ++i) a = cs::step(a, true, 1.0f / 60.0f);  // 0.3 s
    CHECK(a == Catch::Approx(1.0f));
    for (int i = 0; i < 18; ++i) a = cs::step(a, false, 1.0f / 60.0f);
    CHECK(a == Catch::Approx(0.0f).margin(1e-5));
    CHECK(cs::footprintHalfExtent(2.0f) == Catch::Approx(0.9f));
    CHECK(cs::footprintHalfExtent(0.1f) == 0.4f);
    CHECK(cs::footprintHalfExtent(30.0f) == 4.0f);
}

// ---------------------------------------------------------------- E21
TEST_CASE("footfall dust: only dry dirt and sand", "[enhancements][dust]") {
    namespace fd = footfall_dust;
    using G = fd::Ground;
    // TerrainType.SoundID: 1 dirt, 2 metal, 3 stone, 4 snow, 5 wood, 6 grass,
    // 7 leaves, 8 sand, 9 soggy, 10 water.
    CHECK(fd::groundFor(1u, "", false, false) == G::Dirt);
    CHECK(fd::groundFor(8u, "", false, false) == G::Sand);
    for (uint32_t id : {2u, 3u, 4u, 5u, 6u, 7u, 9u, 10u}) {
        INFO("sound id " << id);
        CHECK(fd::groundFor(id, "Tileset\\Barrens\\BarrensDirt.blp", false, false) == G::None);
    }
    // Water and building floors first, whatever the ground says.
    CHECK(fd::groundFor(1u, "", true, false) == G::None);
    CHECK(fd::groundFor(8u, "", false, true) == G::None);
    // No ground effect: the texture's name, the wet and green ones never.
    CHECK(fd::groundFor(std::nullopt, "Tileset\\Tanaris\\TanarisSandBase.blp", false, false) == G::Sand);
    CHECK(fd::groundFor(std::nullopt, "Tileset\\Elwynn\\ElwynnDirtBase.blp", false, false) == G::Dirt);
    CHECK(fd::groundFor(std::nullopt, "Tileset\\Elwynn\\ElwynnGrassBase.blp", false, false) == G::None);
    CHECK(fd::groundFor(std::nullopt, "Tileset\\Elwynn\\ElwynnDirtGrass.blp", false, false) == G::None);
    CHECK(fd::groundFor(std::nullopt, "Tileset\\Wetlands\\WetlandsMudDirt.blp", false, false) == G::None);
    CHECK(fd::groundFor(std::nullopt, "Tileset\\Expansion02\\Snow.blp", false, false) == G::None);
    CHECK(fd::groundFor(std::nullopt, "Tileset\\Generic\\Rock.blp", false, false) == G::None);
}

TEST_CASE("footfall dust: more the faster, nothing standing", "[enhancements][dust]") {
    namespace fd = footfall_dust;
    CHECK(fd::puffFor(0.0f, 1.0f, 0.0f).sprites == 0);
    CHECK(fd::puffFor(1.5f, 1.0f, 0.0f).sprites == 0);   // shuffling
    // A walk: a wisp on some steps only.
    CHECK(fd::puffFor(2.5f, 1.0f, 0.2f).sprites == 1);
    CHECK(fd::puffFor(2.5f, 1.0f, 0.8f).sprites == 0);
    const auto walk = fd::puffFor(2.5f, 1.0f, 0.2f);
    const auto run = fd::puffFor(7.0f, 1.0f, 0.9f);
    const auto mount = fd::puffFor(14.0f, 1.8f, 0.9f);
    CHECK(run.sprites == 2);
    CHECK(mount.sprites == 3);
    CHECK(walk.alpha < run.alpha);
    CHECK(run.alpha < mount.alpha);
    CHECK(run.size < mount.size);
    // Subtle: never as much as half opaque, even at a gallop.
    CHECK(fd::puffFor(40.0f, 10.0f, 0.0f).alpha < 0.5f);
    // Speed from two footfalls; nothing across a pause or a teleport.
    CHECK(fd::stepSpeed({0, 0, 0}, 1.0f, {3.5f, 0, 0}, 1.5f) == Catch::Approx(7.0f));
    CHECK(fd::stepSpeed({0, 0, 0}, 1.0f, {3.5f, 0, 0}, 3.0f) == 0.0f);
    CHECK(fd::stepSpeed({0, 0, 0}, 1.0f, {100.0f, 0, 0}, 1.3f) == 0.0f);
    // The opacity over a sprite's life: in fast, out slowly, zero at the ends.
    CHECK(fd::alphaOverLife(0.0f) == 0.0f);
    CHECK(fd::alphaOverLife(0.1f) == Catch::Approx(1.0f));
    CHECK(fd::alphaOverLife(0.5f) < fd::alphaOverLife(0.2f));
    CHECK(fd::alphaOverLife(1.0f) == 0.0f);
}
