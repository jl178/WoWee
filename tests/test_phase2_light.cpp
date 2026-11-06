// Room light indoors (E12) and contact shadows (E14): the probe grid's bake
// and lookup (indoor_probes.hpp) and the contact ray's limits and colour
// factor (contact_shadows.hpp, which contact_shadow.frag mirrors).
#include <catch_amalgamated.hpp>

#include "rendering/contact_shadows.hpp"
#include "rendering/indoor_probes.hpp"

#include <cmath>
#include <vector>

using namespace wowee::rendering;
namespace ip = indoor_probe;
namespace cs = contact_shadow;

namespace {
bool near3(const glm::vec3& a, const glm::vec3& b, float eps) {
    return std::abs(a.x - b.x) <= eps && std::abs(a.y - b.y) <= eps && std::abs(a.z - b.z) <= eps;
}

// A room's walls as samples: an even colour on each face of a box.
std::vector<ip::Sample> boxRoom(const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& colour,
                                float step = 1.0f) {
    std::vector<ip::Sample> s;
    for (float x = lo.x; x <= hi.x + 1e-3f; x += step)
        for (float y = lo.y; y <= hi.y + 1e-3f; y += step) {
            s.push_back({{x, y, lo.z}, colour, 1.0f});
            s.push_back({{x, y, hi.z}, colour, 1.0f});
        }
    for (float z = lo.z; z <= hi.z + 1e-3f; z += step)
        for (float t = 0.0f; t <= 1.0f + 1e-3f; t += step / 12.0f) {
            s.push_back({{lo.x, lo.y + (hi.y - lo.y) * t, z}, colour, 1.0f});
            s.push_back({{hi.x, lo.y + (hi.y - lo.y) * t, z}, colour, 1.0f});
            s.push_back({{lo.x + (hi.x - lo.x) * t, lo.y, z}, colour, 1.0f});
            s.push_back({{lo.x + (hi.x - lo.x) * t, hi.y, z}, colour, 1.0f});
        }
    return s;
}
}  // namespace

TEST_CASE("probes: cells are about three yards and never more than 16 an axis", "[enhancements][probes]") {
    CHECK(ip::cellsFor(0.0f) == 1);
    CHECK(ip::cellsFor(-4.0f) == 1);
    CHECK(ip::cellsFor(2.9f) == 1);
    CHECK(ip::cellsFor(3.1f) == 2);
    CHECK(ip::cellsFor(12.0f) == 4);
    CHECK(ip::cellsFor(48.0f) == 16);
    CHECK(ip::cellsFor(5000.0f) == 16);

    const auto g = ip::bake(boxRoom({0, 0, 0}, {300, 20, 6}, glm::vec3(0.5f), 4.0f), {0, 0, 0}, {300, 20, 6});
    REQUIRE_FALSE(g.empty());
    CHECK(g.dims.x == 16);
    CHECK(g.dims.y == 7);
    CHECK(g.dims.z == 2);
    CHECK(g.rgba.size() == static_cast<size_t>(16 * 7 * 2));
}

TEST_CASE("probes: no samples, no grid; an empty grid reads black", "[enhancements][probes]") {
    const auto g = ip::bake({}, {0, 0, 0}, {10, 10, 4});
    CHECK(g.empty());
    CHECK(ip::sample(g, {5, 5, 2}) == glm::vec3(0.0f));
    CHECK(ip::lean(g, {5, 5, 2}) == glm::vec2(0.0f));
    // Samples of no weight are no samples.
    const auto z = ip::bake({{{1, 1, 1}, glm::vec3(1.0f), 0.0f}}, {0, 0, 0}, {10, 10, 4});
    CHECK(z.empty());
}

TEST_CASE("probes: an even room reads its own colour everywhere", "[enhancements][probes]") {
    const glm::vec3 warm(0.9f, 0.6f, 0.3f);
    const auto g = ip::bake(boxRoom({0, 0, 0}, {15, 9, 5}, warm), {0, 0, 0}, {15, 9, 5});
    REQUIRE_FALSE(g.empty());
    CHECK(near3(g.mean, warm, 1.0f / 255.0f));
    for (float x = -5.0f; x <= 20.0f; x += 2.5f)
        for (float y = -2.0f; y <= 11.0f; y += 2.0f)
            for (float z = -1.0f; z <= 6.0f; z += 1.5f) {
                INFO(x << "," << y << "," << z);
                CHECK(near3(ip::sample(g, {x, y, z}), warm, 1.5f / 255.0f));
                // Nothing to lean toward.
                CHECK(glm::length(ip::lean(g, {x, y, z})) < 0.02f);
            }
}

TEST_CASE("probes: a bright corner is brighter near it, and the light leans to it", "[enhancements][probes]") {
    // A dim crypt with a hearth at its +x end.
    auto s = boxRoom({0, 0, 0}, {24, 9, 5}, glm::vec3(0.15f, 0.18f, 0.25f));
    for (float y = 2.0f; y <= 7.0f; y += 0.5f)
        for (float z = 0.0f; z <= 3.0f; z += 0.5f) s.push_back({{24.0f, y, z}, glm::vec3(1.0f, 0.7f, 0.35f), 6.0f});
    const auto g = ip::bake(s, {0, 0, 0}, {24, 9, 5});
    REQUIRE_FALSE(g.empty());
    const glm::vec3 byHearth = ip::sample(g, {22, 4.5f, 1.5f});
    const glm::vec3 farEnd = ip::sample(g, {2, 4.5f, 1.5f});
    CHECK(ip::luminance(byHearth) > ip::luminance(farEnd) + 0.15f);
    CHECK(byHearth.r > byHearth.b);       // warm by the fire
    CHECK(farEnd.b > farEnd.r);           // cold at the far end
    const glm::vec2 l = ip::lean(g, {18, 4.5f, 1.5f});
    CHECK(l.x > 0.05f);                   // toward the hearth
    CHECK(std::abs(l.y) < 0.5f * l.x);
    // Held to the most it may lean, anywhere.
    for (float x = 0.0f; x <= 24.0f; x += 1.0f) CHECK(glm::length(ip::lean(g, {x, 4.5f, 1.5f})) <= ip::kMaxLean + 1e-4f);
}

TEST_CASE("probes: sampling is continuous and clamped at the bounds", "[enhancements][probes]") {
    std::vector<ip::Sample> s;
    for (float x = 0.0f; x <= 30.0f; x += 1.0f)
        s.push_back({{x, 5, 2}, glm::vec3(x / 30.0f, 0.5f, 1.0f - x / 30.0f), 1.0f});
    const auto g = ip::bake(s, {0, 0, 0}, {30, 10, 4});
    glm::vec3 prev = ip::sample(g, {-1, 5, 2});
    for (float x = -1.0f; x <= 31.0f; x += 0.1f) {
        const glm::vec3 c = ip::sample(g, {x, 5, 2});
        CHECK(near3(c, prev, 0.03f));  // no step between cells
        CHECK(c.r >= 0.0f);
        CHECK(c.r <= 1.0f);
        prev = c;
    }
    CHECK(ip::sample(g, {-100, 5, 2}) == ip::sample(g, {0, 5, 2}));
    CHECK(ip::sample(g, {100, 5, 2}) == ip::sample(g, {30, 5, 2}));
    CHECK(ip::sample(g, {29, 5, 2}).r > ip::sample(g, {1, 5, 2}).r + 0.5f);
}

TEST_CASE("probes: cells no surface reaches grow in from their neighbours", "[enhancements][probes]") {
    // One lit patch in a big empty box: every cell ends up near its colour,
    // none black.
    std::vector<ip::Sample> s;
    for (float x = 0.0f; x <= 2.0f; x += 0.5f) s.push_back({{x, 1, 1}, glm::vec3(0.4f, 0.5f, 0.6f), 1.0f});
    const auto g = ip::bake(s, {0, 0, 0}, {45, 45, 12});
    REQUIRE(g.dims.x == 15);
    for (int z = 0; z < g.dims.z; ++z)
        for (int y = 0; y < g.dims.y; ++y)
            for (int x = 0; x < g.dims.x; ++x) CHECK(near3(g.at(x, y, z), glm::vec3(0.4f, 0.5f, 0.6f), 2.0f / 255.0f));
}

TEST_CASE("probes: a unit takes the room gently, and re-reads only when it moves or waits",
          "[enhancements][probes]") {
    const glm::vec3 stock(0.35f);
    const glm::vec3 room(0.9f, 0.6f, 0.3f);
    const glm::vec3 c = ip::unitColour(stock, room);
    const glm::vec3 muted = glm::vec3(ip::luminance(room)) + (room - glm::vec3(ip::luminance(room))) * ip::kRoomSaturation;
    CHECK(near3(c, stock + (muted - stock) * ip::kRoomMix, 1e-5f));
    // A grey room is the grey; the hue is muted, never turned.
    CHECK(near3(ip::unitColour(glm::vec3(0.4f), glm::vec3(0.4f)), glm::vec3(0.4f), 1e-6f));
    CHECK(c.r > c.g);
    CHECK(c.g > c.b);
    CHECK(ip::kRoomMix < 1.0f);

    CHECK_FALSE(ip::needsResample({0, 0, 0}, {0.2f, 0.2f, 0}, 0.1f));
    CHECK(ip::needsResample({0, 0, 0}, {0.3f, 0.3f, 0}, 0.1f));
    CHECK(ip::needsResample({0, 0, 0}, {0, 0, 0}, 0.5f));
    CHECK(ip::easeFactor(0.0f) == 0.0f);
    CHECK(ip::easeFactor(-1.0f) == 0.0f);
    CHECK(ip::easeFactor(1.0f / 60.0f) > 0.05f);
    CHECK(ip::easeFactor(1.0f / 60.0f) < 0.1f);
    CHECK(ip::easeFactor(2.0f) > 0.99f);
}

TEST_CASE("contact: the ray stays within its reach and never starts at the pixel", "[enhancements][contact]") {
    for (int i = -2; i < cs::kSteps + 3; ++i)
        for (float j = -0.5f; j <= 1.5f; j += 0.25f) {
            const float d = cs::stepDistance(i, j);
            CHECK(d > 0.0f);
            CHECK(d <= cs::kRayYards + 1e-6f);
        }
    // Steps move outward, and jitter never reorders them.
    for (int i = 0; i + 1 < cs::kSteps; ++i) CHECK(cs::stepDistance(i, 1.0f) <= cs::stepDistance(i + 1, 0.0f) + 1e-6f);
    CHECK(cs::stepDistance(0, 0.0f) == Catch::Approx(cs::kRayYards * 0.5f / cs::kSteps));
    CHECK(cs::stepDistance(cs::kSteps - 1, 1.0f) == Catch::Approx(cs::kRayYards));
    CHECK(cs::kSteps <= 12);
    CHECK(cs::kRayYards <= 1.0f);
}

TEST_CASE("contact: the quad's four pixels take every step once", "[enhancements][contact]") {
    int seen[cs::kSteps] = {};
    for (int lane = 0; lane < 4; ++lane)
        for (int k = 0; k < cs::kStepsPerPixel; ++k) {
            const int i = cs::quadStep(lane, k);
            REQUIRE(i >= 0);
            REQUIRE(i < cs::kSteps);
            ++seen[i];
        }
    for (int i = 0; i < cs::kSteps; ++i) CHECK(seen[i] == 1);
    // A neighbour a few inches off in depth is the same surface; one a foot
    // or more off is not, near the camera.
    CHECK(cs::quadDepthTolerance(5.0f) < 0.3f);
    CHECK(cs::quadDepthTolerance(30.0f) > cs::quadDepthTolerance(5.0f));
}

TEST_CASE("contact: limited to the near field, with a soft end", "[enhancements][contact]") {
    CHECK(cs::distanceFade(0.0f) == 1.0f);
    CHECK(cs::distanceFade(cs::kMaxDistance - cs::kFadeYards) == 1.0f);
    CHECK(cs::distanceFade(cs::kMaxDistance) == 0.0f);
    CHECK(cs::distanceFade(1000.0f) == 0.0f);
    float prev = 1.0f;
    for (float d = 0.0f; d <= cs::kMaxDistance + 5.0f; d += 0.5f) {
        const float f = cs::distanceFade(d);
        CHECK(f <= prev + 1e-6f);
        prev = f;
    }
    for (int i = 0; i < cs::kSteps; ++i) {
        CHECK(cs::stepWeight(i) > 0.0f);
        CHECK(cs::stepWeight(i) <= 1.0f);
        if (i > 0) CHECK(cs::stepWeight(i) < cs::stepWeight(i - 1));
    }
    CHECK_FALSE(cs::onSilhouette(0.05f, 5.0f));
    CHECK(cs::onSilhouette(2.0f, 5.0f));
    CHECK(cs::onSilhouette(-2.0f, 5.0f));
    CHECK_FALSE(cs::onSilhouette(0.5f, 40.0f));  // a slope seen far off is not an edge
    CHECK(cs::startBias(0.0f) > 0.0f);
    CHECK(cs::startBias(40.0f) > cs::startBias(5.0f));
    CHECK(cs::thickness(40.0f) > cs::thickness(5.0f));
}

TEST_CASE("contact: only the sun's share is taken, never more than a full shadow", "[enhancements][contact]") {
    // Unblocked: nothing changes.
    CHECK(cs::colourFactor(0.4f, 0.8f, 0.9f, 0.7f, 1.0f, 0.0f) == 1.0f);
    // Turned from the sun: nothing to take.
    CHECK(cs::colourFactor(0.4f, 0.8f, 0.0f, 0.7f, 1.0f, 1.0f) == 1.0f);
    CHECK(cs::colourFactor(0.4f, 0.8f, -0.5f, 0.7f, 1.0f, 1.0f) == 1.0f);
    // Already in the map's full shadow: no darker.
    CHECK(cs::colourFactor(0.4f, 0.8f, 0.9f, 0.7f, 0.0f, 1.0f) == 1.0f);
    // Fully blocked in the sun: exactly what the map's full shadow gives.
    const float a = 0.4f, d = 0.8f, n = 0.9f, k = 0.7f;
    const float full = (a + d * n * (1.0f - k)) / (a + d * n);
    CHECK(cs::colourFactor(a, d, n, k, 1.0f, 1.0f) == Catch::Approx(full));
    // Partial: in between, and more blocking is darker.
    float prev = 1.0f;
    for (float c = 0.0f; c <= 1.0f; c += 0.1f) {
        const float f = cs::colourFactor(a, d, n, k, 1.0f, c);
        CHECK(f <= prev + 1e-6f);
        CHECK(f >= full - 1e-6f);
        prev = f;
    }
    // A pixel half in the map's shadow only loses what the ray adds.
    CHECK(cs::colourFactor(a, d, n, k, 0.5f, 0.3f) == 1.0f);
    CHECK(cs::colourFactor(a, d, n, k, 0.5f, 0.8f) < 1.0f);
    // Shadow darkness 0 (Light.dbc's band at night, say): nothing.
    CHECK(cs::colourFactor(a, d, n, 0.0f, 1.0f, 1.0f) == 1.0f);
    // Black light: left alone.
    CHECK(cs::colourFactor(0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f) == 1.0f);
}
