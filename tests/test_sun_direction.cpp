// Where the sun is, and in particular where it is not.
//
// SkySystem::getSunPosition mirrored a sun below the horizon back up into the
// sky - `sunDir = dir` instead of `-dir` - and the lens flare is the only
// thing that ever asks it. So the mirror did nothing but invent a sun for the
// flare to draw around, at a position no sun was at, and the flare's own
// height attenuation read the mirrored height as a sun climbing the sky and
// let it through at full strength.
//
// Reported as a lens flare with no sun in sight.
#include <catch_amalgamated.hpp>

#include "rendering/stock_day_night.hpp"
#include "rendering/sun_direction.hpp"

#include <glm/gtc/matrix_transform.hpp>

using wowee::rendering::stockLightDirection;
using wowee::rendering::stockMoonDirection;
using wowee::rendering::stockSunDirection;
using wowee::rendering::sunDirectionFromLightDir;
using wowee::rendering::sunScreenPosition;

TEST_CASE("the sun is opposite the direction its light travels", "[sky][sun]") {
    // Light going down means a sun overhead.
    const glm::vec3 up = sunDirectionFromLightDir(glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(up.z == Catch::Approx(1.0f));

    // And the answer is a unit vector whatever length it is handed.
    const glm::vec3 scaled = sunDirectionFromLightDir(glm::vec3(0.0f, 0.0f, -57.0f));
    CHECK(glm::length(scaled) == Catch::Approx(1.0f));
    CHECK(scaled.z == Catch::Approx(1.0f));
}

TEST_CASE("a sun below the horizon stays below it", "[sky][sun]") {
    // Light travelling upward is light from under the ground. The sun is down
    // there with it, and this is the assertion the mirror would fail.
    const glm::vec3 night = sunDirectionFromLightDir(glm::vec3(0.0f, 0.0f, 1.0f));
    CHECK(night.z == Catch::Approx(-1.0f));
    CHECK(night.z < 0.0f);

    // Just under the horizon, which is where the mirror did the most damage:
    // a flare at full strength opposite a sun that had already set.
    const glm::vec3 dusk = sunDirectionFromLightDir(glm::vec3(0.6f, 0.0f, 0.08f));
    CHECK(dusk.z < 0.0f);
}

TEST_CASE("no sun given reads as straight up", "[sky][sun]") {
    // Everything downstream gates on height, and straight up is the value that
    // leaves those gates to decide rather than producing a sideways sun.
    const glm::vec3 none = sunDirectionFromLightDir(glm::vec3(0.0f));
    CHECK(none.z == Catch::Approx(1.0f));
    CHECK(glm::length(none) == Catch::Approx(1.0f));
}

// The sun shafts stream from where this puts the sun, and every full-screen pass
// samples with (0,0) at the top left. A sun that came out mirrored top to bottom
// would pour its rays up out of the ground.
TEST_CASE("the sun lands on screen where it is in the sky", "[sky][sun]") {
    const glm::vec3 eye(-8913.0f, 554.0f, 94.0f);
    const glm::vec3 forward(1.0f, 0.0f, 0.0f);
    const glm::mat4 view = glm::lookAt(eye, eye + forward, glm::vec3(0.0f, 0.0f, 1.0f));
    // As the camera builds it: Y flipped for Vulkan.
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.5f, 30000.0f);
    proj[1][1] *= -1.0f;

    // Dead ahead is the middle of the screen, wherever the camera stands.
    const auto ahead = sunScreenPosition(view, proj, forward);
    REQUIRE(ahead.inFront);
    CHECK(ahead.uv.x == Catch::Approx(0.5f).margin(1e-5f));
    CHECK(ahead.uv.y == Catch::Approx(0.5f).margin(1e-5f));

    // Ahead and a little up is above the middle - smaller v, toward the top.
    const auto raised = sunScreenPosition(view, proj, glm::normalize(glm::vec3(1.0f, 0.0f, 0.2f)));
    REQUIRE(raised.inFront);
    CHECK(raised.uv.y < 0.5f);
    CHECK(raised.uv.x == Catch::Approx(0.5f).margin(1e-5f));

    // Behind the camera it has no place on screen at all.
    CHECK_FALSE(sunScreenPosition(view, proj, -forward).inFront);
}

// The stock client's light and sun, from Wow.exe 3.3.5a: DayNight 0x7eea90
// (light) and 0x7eecc0 (sun and moon sprites). The expected values are what
// those functions compute - their cubic cosine included - normalised, and
// with x and y swapped into render space (they are equal here anyway: both
// azimuths are diagonals). Wowee had sin/cos of the hour scaled by 0.6, which
// put the noon light, and the sun drawn along it, 31 degrees up.
namespace {
void checkDir(const glm::vec3& got, const glm::vec3& want) {
    CHECK(got.x == Catch::Approx(want.x).margin(1e-4f));
    CHECK(got.y == Catch::Approx(want.y).margin(1e-4f));
    CHECK(got.z == Catch::Approx(want.z).margin(1e-4f));
    CHECK(glm::length(got) == Catch::Approx(1.0f).margin(1e-5f));
}
constexpr float kMidnight = 0.0f;
constexpr float kDawn = 720.0f;   // 06:00
constexpr float kNoon = 1440.0f;
constexpr float kDusk = 2160.0f;  // 18:00
}  // namespace

TEST_CASE("the light follows the stock client's table, not the sun", "[sky][sun][stock]") {
    // 37.5 degrees down out of the north-west at noon and at midnight alike,
    // 20 degrees at six in the morning and the evening.
    checkDir(stockLightDirection(kNoon), glm::vec3(-0.56131f, -0.56131f, -0.60817f));
    checkDir(stockLightDirection(kMidnight), glm::vec3(-0.56131f, -0.56131f, -0.60817f));
    checkDir(stockLightDirection(kDawn), glm::vec3(-0.66488f, -0.66488f, -0.34041f));
    checkDir(stockLightDirection(kDusk), glm::vec3(-0.66488f, -0.66488f, -0.34041f));
    // Halfway between keys is a straight blend of the angles.
    checkDir(stockLightDirection(360.0f), glm::vec3(-0.62072f, -0.62072f, -0.47897f));

    // It never turns over: downward at every hour, so shadows never flip at
    // night and never lift off the ground. And it wraps through midnight.
    for (float t = 0.0f; t < 2880.0f; t += 7.5f) {
        INFO("half-minute " << t);
        CHECK(stockLightDirection(t).z < -0.3f);
    }
    checkDir(stockLightDirection(2879.99f), stockLightDirection(0.0f));
}

TEST_CASE("the sun sprite climbs to the zenith and sets", "[sky][sun][stock]") {
    // Five degrees off the zenith at noon, north-west at 25 degrees at six in
    // the evening, just under the horizon at six in the morning and ten under
    // it all night.
    checkDir(stockSunDirection(kNoon), glm::vec3(0.05730f, 0.05730f, 0.99671f));
    checkDir(stockSunDirection(kDusk), glm::vec3(0.63913f, 0.63913f, 0.42781f));
    checkDir(stockSunDirection(kDawn), glm::vec3(0.70641f, 0.70641f, -0.04452f));
    checkDir(stockSunDirection(kMidnight), glm::vec3(0.69666f, 0.69666f, -0.17125f));

    // The sun comes from the side the light does: the light travels away
    // from it horizontally.
    const glm::vec3 sun = stockSunDirection(kDusk);
    const glm::vec3 light = stockLightDirection(kDusk);
    CHECK(sun.x * light.x + sun.y * light.y < 0.0f);
}

TEST_CASE("the moon is up around midnight", "[sky][moon][stock]") {
    checkDir(stockMoonDirection(kMidnight), glm::vec3(0.39402f, 0.39402f, 0.83036f));
    checkDir(stockMoonDirection(kNoon), glm::vec3(0.69666f, 0.69666f, -0.17125f));
}
