// Bounce light (enhanced): the grid's bake (bounce_grid.hpp), its copy of
// the constants in ambient_occlusion.frag, and the row and preset.
#include <catch_amalgamated.hpp>

#include "rendering/bounce_grid.hpp"
#include "rendering/sky_field.hpp"
#include "rendering/enhancements.hpp"
#include "ui/enhancement_rows.hpp"
#include "ui/graphics_presets.hpp"

#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace bg = wowee::rendering::bounce_grid;
using wowee::rendering::Enhancements;

namespace {
int cellOf(float world, float origin) { return static_cast<int>((world - origin) / bg::kCellYards); }
}  // namespace

TEST_CASE("bounce grid: the ground keeps the fill's brightness and takes the minimap's hue", "[light][enhanced]") {
    // A minimap at the reference brightness gives the fill's own luminance.
    const glm::vec3 grass(0.18f, 0.32f, 0.12f);
    const glm::vec3 g = bg::groundAlbedo(grass * (bg::kMinimapLumRef / bg::luminance(grass)));
    CHECK(bg::luminance(g) == Catch::Approx(bg::luminance(bg::kFillGroundAlbedo)).margin(1e-3));
    // ...and greener than the fill's yellow-grey.
    CHECK(g.g / g.r > bg::kFillGroundAlbedo.g / bg::kFillGroundAlbedo.r);
    // A very dark or very bright picture is held within the band.
    const glm::vec3 dark = bg::groundAlbedo(glm::vec3(0.02f));
    const glm::vec3 bright = bg::groundAlbedo(glm::vec3(0.9f));
    CHECK(bg::luminance(dark) >= bg::luminance(bg::kFillGroundAlbedo) * bg::kGroundLumMin - 1e-3f);
    CHECK(bg::luminance(bright) <= bg::luminance(bg::kFillGroundAlbedo) * bg::kGroundLumMax + 1e-3f);
    // Black (no picture) is the fill's own.
    CHECK(bg::groundAlbedo(glm::vec3(0.0f)) == bg::kFillGroundAlbedo);
}

TEST_CASE("bounce grid: snapped to cells, rebuilt only when the camera leaves its middle", "[light][enhanced]") {
    const glm::vec2 o = bg::originFor(-9465.3f, 61.7f);
    CHECK(std::fmod(std::abs(o.x), bg::kCellYards) == Catch::Approx(0.0f));
    CHECK(std::fmod(std::abs(o.y), bg::kCellYards) == Catch::Approx(0.0f));
    CHECK_FALSE(bg::needsRecentre(o, -9465.3f, 61.7f));
    CHECK_FALSE(bg::needsRecentre(o, -9465.3f + bg::kRecentreYards * 0.9f, 61.7f));
    CHECK(bg::needsRecentre(o, -9465.3f + bg::kRecentreYards * 1.2f, 61.7f));
    // The camera's walk inside the middle stays inside the faded edge.
    CHECK(bg::kRecentreYards + 50.0f < bg::kSpan * 0.5f * (1.0f - 0.1f * 2.0f));
}

TEST_CASE("bounce grid: a crown covers the ground under it and carries its heights past its edge",
          "[light][enhanced]") {
    bg::Inputs in;
    in.origin = glm::vec2(1000.0f, 2000.0f);
    bg::Canopy c;
    c.centre = in.origin + glm::vec2(100.0f, 100.0f);
    c.radius = 8.0f;
    c.bottom = 60.0f;
    c.top = 72.0f;
    c.leaf = glm::vec3(0.2f, 0.5f, 0.1f);
    c.density = 0.85f;
    in.canopies.push_back(c);
    const bg::Grid g = bg::bake(in);
    REQUIRE_FALSE(g.empty());
    const int cx = cellOf(c.centre.x, in.origin.x), cy = cellOf(c.centre.y, in.origin.y);
    const float* mid0 = g.at(0, cx, cy);
    const float* mid1 = g.at(1, cx, cy);
    const float* mid2 = g.at(2, cx, cy);
    CHECK(mid0[3] > 0.6f);
    CHECK(mid0[3] <= 0.85f + 1e-4f);
    CHECK(mid1[1] == Catch::Approx(0.5f).margin(1e-4));
    CHECK(mid2[0] == Catch::Approx(60.0f).margin(1e-3));
    CHECK(mid2[1] == Catch::Approx(72.0f).margin(1e-3));
    // Far from it, open sky.
    CHECK(g.at(0, cx + 40, cy)[3] == Catch::Approx(0.0f).margin(1e-5));
    // Just past its rim the cover is (near) gone but the heights carry on,
    // so a filtered read across the rim fades the cover, not the heights.
    const int rim = cx + static_cast<int>(c.radius / bg::kCellYards) + 2;
    CHECK(g.at(2, rim, cy)[0] == Catch::Approx(60.0f).margin(1e-3));
    // With no grass picture the ground is the fill's own colour.
    CHECK(g.at(0, cx + 40, cy)[0] == Catch::Approx(bg::kFillGroundAlbedo.r).margin(1e-4));
}

TEST_CASE("bounce grid: walls mark their cells with their foot and top", "[light][enhanced]") {
    bg::Inputs in;
    in.origin = glm::vec2(0.0f, 0.0f);
    bg::Building b;
    b.lo = glm::vec2(50.0f, 50.0f);
    b.hi = glm::vec2(62.0f, 58.0f);
    b.bottom = 40.0f;
    b.top = 49.0f;
    in.buildings.push_back(b);
    in.groundAt = [](float, float, glm::vec3& rgb) {
        rgb = glm::vec3(0.2f, 0.3f, 0.1f);
        return true;
    };
    const bg::Grid g = bg::bake(in);
    CHECK(g.at(1, 28, 27)[3] == Catch::Approx(1.0f));
    CHECK(g.at(2, 28, 27)[2] == Catch::Approx(40.0f));
    CHECK(g.at(2, 28, 27)[3] == Catch::Approx(49.0f));
    CHECK(g.at(1, 10, 10)[3] == Catch::Approx(0.0f));
    // Next to it: no wall, but its heights (for the filtered read).
    CHECK(g.at(1, 32, 27)[3] == Catch::Approx(0.0f));
    CHECK(g.at(2, 32, 27)[3] == Catch::Approx(49.0f));
    // Far from it: heights nothing can be under.
    CHECK(g.at(2, 100, 100)[3] < -1000.0f);
    // The ground took the picture's hue.
    CHECK(g.at(0, 100, 100)[1] > g.at(0, 100, 100)[0]);
}

TEST_CASE("bounce grid: halves", "[light]") {
    CHECK(bg::toHalf(0.0f) == 0x0000);
    CHECK(bg::toHalf(1.0f) == 0x3c00);
    CHECK(bg::toHalf(-2.0f) == 0xc000);
    CHECK(bg::toHalf(0.5f) == 0x3800);
    CHECK(bg::toHalf(65504.0f) == 0x7bff);
    CHECK(bg::toHalf(1e6f) == 0x7bff);
    CHECK(bg::toHalf(-20000.0f) == 0xf4e2);
}

#ifdef WOWEE_SOURCE_DIR
TEST_CASE("bounce grid: the shader reads the grid as it is baked", "[light][enhanced]") {
    std::ifstream f(std::string(WOWEE_SOURCE_DIR) + "/assets/shaders/ambient_occlusion.frag.glsl");
    REQUIRE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    std::smatch m;
    REQUIRE(std::regex_search(src, m, std::regex(R"(const float kGridSpan = ([\d.]+);)")));
    CHECK(std::stof(m[1].str()) == Catch::Approx(bg::kSpan));
    REQUIRE(std::regex_search(src, m, std::regex(R"(const vec3 kGroundAlbedo = vec3\(([\d.]+), ([\d.]+), ([\d.]+)\);)")));
    CHECK(std::stof(m[1].str()) == Catch::Approx(bg::kFillGroundAlbedo.r));
    CHECK(std::stof(m[2].str()) == Catch::Approx(bg::kFillGroundAlbedo.g));
    CHECK(std::stof(m[3].str()) == Catch::Approx(bg::kFillGroundAlbedo.b));
    // The stock build has none of it.
    CHECK(src.find("#ifdef BOUNCE_GRID") != std::string::npos);
}

TEST_CASE("sky field: the shader reads the height field as it is drawn", "[light][enhanced]") {
    std::ifstream f(std::string(WOWEE_SOURCE_DIR) + "/assets/shaders/ambient_occlusion.frag.glsl");
    REQUIRE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    std::smatch m;
    REQUIRE(std::regex_search(src, m, std::regex(R"(const float kSkySpan = ([\d.]+);)")));
    CHECK(std::stof(m[1].str()) == Catch::Approx(wowee::rendering::sky_field::kSpan));
    REQUIRE(std::regex_search(src, m, std::regex(R"(const float kSkyDepth = ([\d.]+);)")));
    CHECK(std::stof(m[1].str()) == Catch::Approx(wowee::rendering::sky_field::kDepth));
}
#endif

TEST_CASE("sky field: from straight above, the corner at uv 0 and depth down from the top", "[light][enhanced]") {
    namespace sf = wowee::rendering::sky_field;
    const glm::vec3 centre = sf::snappedCentre(glm::vec3(-9465.3f, 61.7f, 56.4f));
    // Snapped to whole texels.
    const float texel = sf::kSpan / static_cast<float>(sf::kTexels);
    CHECK(std::fmod(std::abs(centre.x), texel) == Catch::Approx(0.0f).margin(1e-3));
    CHECK(std::fmod(std::abs(centre.y), texel) == Catch::Approx(0.0f).margin(1e-3));
    CHECK(centre.z == 56.0f);
    const glm::mat4 p = sf::projection(centre);
    const glm::vec2 corner = glm::vec2(centre) - 0.5f * sf::kSpan;
    // The shader's uv is (xy - corner) / span; clip is uv * 2 - 1.
    for (const glm::vec3 w : {glm::vec3(corner, 0.0f), glm::vec3(corner + sf::kSpan, 0.0f),
                              glm::vec3(corner.x + 40.0f, corner.y + 200.0f, 0.0f)}) {
        const glm::vec4 c = p * glm::vec4(w.x, w.y, centre.z + 3.0f, 1.0f);
        const glm::vec2 uv = (glm::vec2(w) - corner) / sf::kSpan;
        CHECK(c.w == Catch::Approx(1.0f));
        CHECK(c.x == Catch::Approx(uv.x * 2.0f - 1.0f).margin(1e-4));
        CHECK(c.y == Catch::Approx(uv.y * 2.0f - 1.0f).margin(1e-4));
    }
    // Depth 0 kAbove over the middle, 1 kDepth under that; the shader's
    // height is top - depth * kDepth.
    const float top = centre.z + sf::kAbove;
    CHECK((p * glm::vec4(glm::vec2(centre), top, 1.0f)).z == Catch::Approx(0.0f).margin(1e-5));
    CHECK((p * glm::vec4(glm::vec2(centre), top - sf::kDepth, 1.0f)).z == Catch::Approx(1.0f));
    const float z = centre.z + 12.5f;
    CHECK(top - (p * glm::vec4(glm::vec2(centre), z, 1.0f)).z * sf::kDepth == Catch::Approx(z));
    // From below: the same square, depth up from the bottom (the shader's
    // underside is top - kDepth + depth * kDepth), so the lowest surface wins.
    const glm::mat4 b = sf::projection(centre, true);
    const glm::vec4 cb = b * glm::vec4(corner.x + 40.0f, corner.y + 200.0f, z, 1.0f);
    const glm::vec4 ca = p * glm::vec4(corner.x + 40.0f, corner.y + 200.0f, z, 1.0f);
    CHECK(cb.x == Catch::Approx(ca.x));
    CHECK(cb.y == Catch::Approx(ca.y));
    CHECK(top - sf::kDepth + cb.z * sf::kDepth == Catch::Approx(z));
    CHECK((b * glm::vec4(glm::vec2(centre), z - 1.0f, 1.0f)).z < cb.z);
}

TEST_CASE("bounce light: an enhanced row, off in stock and the ladder, on in the Enhanced preset",
          "[enhanced][enhancements]") {
    using namespace wowee::ui;
    const EnhancementRow* row = findEnhancementRow("bouncelight");
    REQUIRE(row != nullptr);
    CHECK(std::string(row->category) == kEnhPageEnhanced);
    CHECK(row->stockValue == 0.0f);
    CHECK(Enhancements{}.bounceLight == false);
    CHECK(kGraphicsPresets[kEnhancedPresetIndex].enh.bounceLight);
    for (int i = 0; i < kClassicPresetIndex; ++i) CHECK_FALSE(kGraphicsPresets[i].enh.bounceLight);
}

TEST_CASE("sky shade: an enhanced row, off in stock and the ladder, on in the Enhanced preset",
          "[enhanced][enhancements]") {
    using namespace wowee::ui;
    const EnhancementRow* row = findEnhancementRow("skyshade");
    REQUIRE(row != nullptr);
    CHECK(std::string(row->category) == kEnhPageEnhanced);
    CHECK(row->stockValue == 0.0f);
    CHECK(std::string(row->saveKey) == "sky_shade");
    CHECK(Enhancements{}.skyShade == false);
    CHECK(kGraphicsPresets[kEnhancedPresetIndex].enh.skyShade);
    CHECK(kGraphicsPresets[kEnhancedPresetIndex].enh.bounceLight);
    for (int i = 0; i < kClassicPresetIndex; ++i) CHECK_FALSE(kGraphicsPresets[i].enh.skyShade);
    // Its own shader bit, set by the renderer, not by the row's switch.
    CHECK((wowee::rendering::shaderBits(kGraphicsPresets[kEnhancedPresetIndex].enh) &
           wowee::rendering::enh_bits::kSkyShade) == 0u);
}
