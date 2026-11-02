// The sphere character animation culls a unit's skeleton with: the unit, and
// everywhere its shadow can fall. A sphere too small freezes a shadow that is
// on screen mid-stride; these pin that the body and the ground shadow are
// inside it for the sun angles the shadow projection allows.
#include <catch_amalgamated.hpp>

#include "rendering/anim_cull.hpp"

#include <glm/glm.hpp>

using namespace wowee::rendering;

namespace {

bool contains(const AnimCullSphere& s, const glm::vec3& p) {
    return glm::length(p - s.center) <= s.radius + 1e-4f;
}

}  // namespace

TEST_CASE("with shadows off the unit's own sphere comes back", "[animcull]") {
    const glm::vec3 feet(10.0f, -20.0f, 5.0f);
    const AnimCullSphere s = animCullSphere(feet, 4.0f, 2.5f, glm::vec3(0.0f), 40.0f);
    CHECK(s.center == feet);
    CHECK(s.radius == 4.0f);
}

TEST_CASE("the body and its ground shadow are inside the sphere", "[animcull]") {
    const glm::vec3 feet(100.0f, 50.0f, 20.0f);
    const float cullRadius = 4.0f;
    const float height = 2.4f;
    // High sun, 45 degrees, and the shallowest the shadow projection allows.
    const glm::vec3 dirs[] = {
        glm::normalize(glm::vec3(0.0f, 0.0f, -1.0f)),
        glm::normalize(glm::vec3(1.0f, 0.0f, -1.0f)),
        glm::normalize(glm::vec3(-0.3f, 0.8f, -0.4f)),
        glm::normalize(glm::vec3(0.6f, -0.7f, -0.15f)),
    };
    for (const auto& dir : dirs) {
        INFO("dir " << dir.x << "," << dir.y << "," << dir.z);
        const AnimCullSphere s = animCullSphere(feet, cullRadius, height, dir, 40.0f);
        // The body: the main pass's sphere and the top of the head.
        CHECK(contains(s, feet + glm::vec3(cullRadius, 0.0f, 0.0f)));
        CHECK(contains(s, feet + glm::vec3(0.0f, 0.0f, height)));
        // The head's shadow on level ground, and a point of the body's width
        // beside it.
        const float t = height / -dir.z;
        const glm::vec3 tip = feet + glm::vec3(0.0f, 0.0f, height) + dir * t;
        CHECK(contains(s, tip));
        CHECK(contains(s, tip + glm::vec3(0.0f, cullRadius, 0.0f)));
        // Halfway down the ray, where a wall between the unit and the ground
        // would catch it.
        CHECK(contains(s, feet + glm::vec3(0.0f, 0.0f, height) + dir * (t * 0.5f)));
    }
}

TEST_CASE("the shadow run is capped", "[animcull]") {
    // A giant at a grazing sun would otherwise grow a sphere over the whole
    // view and never be culled.
    const glm::vec3 dir = glm::normalize(glm::vec3(1.0f, 0.0f, -0.15f));
    const AnimCullSphere s = animCullSphere(glm::vec3(0.0f), 4.0f, 30.0f, dir, 40.0f);
    CHECK(s.radius <= 4.0f + 30.0f + 20.0f + 1e-3f);
    CHECK(s.center.x == Catch::Approx(20.0f));
}
