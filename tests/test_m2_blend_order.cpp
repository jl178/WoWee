// The stock order of an M2 scene's blended elements (0x821A20 / 0x81EF30),
// cull and depth from render flags (0x81FE90), and the map doodad distance
// fade (0x791CB0). docs/re/m2-runtime.md #24, #25, §4.
#include <catch_amalgamated.hpp>

#include <algorithm>
#include <vector>

#include "rendering/m2_blend_order.hpp"
#include "rendering/m2_raster_state.hpp"
#include "rendering/m2_view_distance.hpp"

using namespace wowee::rendering;

namespace {
struct El {
    float primary = 0, secondary = 0;
    uint32_t instance = 0, item = 0;
    int16_t priority = 0;
    uint16_t layer = 0;
    uint8_t type = 0;
};
}  // namespace

TEST_CASE("blended elements sort far first", "[m2][blendorder]") {
    std::vector<El> v = {{.primary = 4, .item = 0}, {.primary = 9, .item = 1}, {.primary = 1, .item = 2}};
    std::sort(v.begin(), v.end(), m2BlendOrderLess<El>);
    REQUIRE(v[0].item == 1);
    REQUIRE(v[1].item == 0);
    REQUIRE(v[2].item == 2);
}

TEST_CASE("at one distance priorityPlane goes first, ascending", "[m2][blendorder]") {
    std::vector<El> v = {{.primary = 5, .item = 0, .priority = 2},
                         {.primary = 5, .item = 1, .priority = -1},
                         {.primary = 5, .item = 2, .priority = 0}};
    std::sort(v.begin(), v.end(), m2BlendOrderLess<El>);
    REQUIRE(v[0].priority == -1);
    REQUIRE(v[1].priority == 0);
    REQUIRE(v[2].priority == 2);
}

TEST_CASE("then the own distance, then batches before particles, then layer", "[m2][blendorder]") {
    // A flag-0x10 model: every element at the model's distance.
    std::vector<El> v = {{.primary = 5, .secondary = 1, .item = 0},
                         {.primary = 5, .secondary = 3, .item = 1},
                         {.primary = 5, .secondary = 2, .item = 2, .type = 4},
                         {.primary = 5, .secondary = 2, .item = 3, .layer = 1},
                         {.primary = 5, .secondary = 2, .item = 4, .layer = 0}};
    std::sort(v.begin(), v.end(), m2BlendOrderLess<El>);
    REQUIRE(v[0].item == 1);
    REQUIRE(v[1].item == 4);  // batch, layer 0
    REQUIRE(v[2].item == 3);  // batch, layer 1
    REQUIRE(v[3].item == 2);  // particles after the batches
    REQUIRE(v[4].item == 0);
}

TEST_CASE("the sort radius moves the point along the view ray", "[m2][blendorder]") {
    const glm::vec3 fwd(0, 1, 0);
    const glm::vec3 v(0, 10, 0);
    REQUIRE(m2BatchSortDistanceSq(v, 2.0f, 0x0, fwd) == Catch::Approx(100.0f));
    REQUIRE(m2BatchSortDistanceSq(v, 2.0f, 0x1, fwd) == Catch::Approx(64.0f));
    REQUIRE(m2BatchSortDistanceSq(v, 2.0f, 0x2, fwd) == Catch::Approx(144.0f));
    // Pulled through the camera: behind it, so negative and drawn last.
    REQUIRE(m2BatchSortDistanceSq(v, 12.0f, 0x1, fwd) == Catch::Approx(-4.0f));
    // Flag 0x1 wins over 0x2.
    REQUIRE(m2BatchSortDistanceSq(v, 2.0f, 0x3, fwd) == Catch::Approx(64.0f));
}

TEST_CASE("render flags set cull and depth per batch", "[m2][rasterstate]") {
    const M2RasterState plain = m2RasterStateForFlags(0x00, false, false, false);
    REQUIRE(plain.cullBack);
    REQUIRE(plain.depthTest);
    REQUIRE(plain.depthWrite);  // stock: a blended layer without 0x10 writes depth
    REQUIRE_FALSE(m2RasterStateForFlags(0x04, false, true, false).cullBack);
    REQUIRE_FALSE(m2RasterStateForFlags(0x08, false, true, false).depthTest);
    REQUIRE_FALSE(m2RasterStateForFlags(0x10, false, true, false).depthWrite);
    // Unlit and unfogged leave them alone.
    REQUIRE(m2RasterStateForFlags(0x03, false, true, false) == plain);
    // The sky never writes depth.
    REQUIRE_FALSE(m2RasterStateForFlags(0x00, true, true, false).depthWrite);
    // Legacy: two-sided, depth by the pipeline.
    const M2RasterState legacy = m2RasterStateForFlags(0x18, false, false, true);
    REQUIRE_FALSE(legacy.cullBack);
    REQUIRE(legacy.depthTest);
    REQUIRE_FALSE(legacy.depthWrite);
}

TEST_CASE("map doodads take a size class by their largest extent", "[m2][doodadfade]") {
    REQUIRE(stockDoodadSizeClass(0.5f) == 0);
    REQUIRE(stockDoodadSizeClass(1.0f) == 0);
    REQUIRE(stockDoodadSizeClass(1.01f) == 1);
    REQUIRE(stockDoodadSizeClass(4.0f) == 1);
    REQUIRE(stockDoodadSizeClass(15.0f) == 2);
    REQUIRE(stockDoodadSizeClass(99.0f) == 3);
    REQUIRE(stockDoodadSizeClass(500.0f) == 4);
}

TEST_CASE("the stock end distances, and what scales them", "[m2][doodadfade]") {
    REQUIRE(stockDoodadEndDistance(0, 1.0f, 1.0f) == Catch::Approx(30.0f));
    REQUIRE(stockDoodadEndDistance(1, 1.0f, 1.0f) == Catch::Approx(100.0f));
    REQUIRE(stockDoodadEndDistance(2, 1.0f, 1.0f) == Catch::Approx(200.0f));
    REQUIRE(stockDoodadEndDistance(3, 1.0f, 1.0f) == Catch::Approx(750.0f));
    REQUIRE(stockDoodadEndDistance(4, 1.0f, 1.0f) == Catch::Approx(1250.0f));
    // environmentDetail scales the middle three, clamped to 0.5..1.5.
    REQUIRE(stockDoodadEndDistance(2, 1.5f, 1.0f) == Catch::Approx(300.0f));
    REQUIRE(stockDoodadEndDistance(2, 0.25f, 1.0f) == Catch::Approx(100.0f));
    REQUIRE(stockDoodadEndDistance(0, 1.5f, 1.0f) == Catch::Approx(30.0f));
    REQUIRE(stockDoodadEndDistance(4, 1.5f, 1.0f) == Catch::Approx(1250.0f));
    // The view distance slider multiplies every class but the smallest.
    REQUIRE(stockDoodadEndDistance(3, 1.0f, 2.0f) == Catch::Approx(1500.0f));
    REQUIRE(stockDoodadEndDistance(4, 1.0f, 2.0f) == Catch::Approx(2500.0f));
    REQUIRE(stockDoodadEndDistance(0, 1.0f, 2.0f) == Catch::Approx(30.0f));
}

TEST_CASE("a map doodad fades linearly over its class's last yards", "[m2][doodadfade]") {
    // Class 2: ends at 200, fades over 15.
    REQUIRE(stockDoodadFadeAlpha(2, 200.0f, 100.0f) == 1.0f);
    REQUIRE(stockDoodadFadeAlpha(2, 200.0f, 185.0f) == 1.0f);
    REQUIRE(stockDoodadFadeAlpha(2, 200.0f, 192.5f) == Catch::Approx(0.5f));
    REQUIRE(stockDoodadFadeAlpha(2, 200.0f, 185.1f) == 1.0f);   // above 0.99
    REQUIRE(stockDoodadFadeAlpha(2, 200.0f, 199.95f) == 0.0f);  // below 0.01
    REQUIRE(stockDoodadFadeAlpha(2, 200.0f, 201.0f) == 0.0f);
}
