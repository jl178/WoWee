// The Enhanced look (docs/re/enhanced-lighting.md): the colour grade's curves
// and hour weight (enhanced_grade.hpp, which fxaa.frag mirrors), and the
// Enhanced preset row.
#include <catch_amalgamated.hpp>

#include "rendering/enhanced_grade.hpp"
#include "rendering/enhanced_palette.hpp"
#include "ui/graphics_presets.hpp"

#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace wowee::rendering;

TEST_CASE("Enhanced grade: the curves rise from 0 to 1 and never fall", "[enhanced]") {
    for (const float* c : {enhanced_grade::kEasternKingdoms, enhanced_grade::kKalimdor}) {
        CHECK(c[0] == 0.0f);
        CHECK(c[enhanced_grade::kCurveSize - 1] == 1.0f);
        for (int i = 1; i < enhanced_grade::kCurveSize; ++i) CHECK(c[i] >= c[i - 1]);
    }
    // The grade brightens the mid-tones by about a tenth and leaves black alone.
    CHECK(enhanced_grade::apply(enhanced_grade::kEasternKingdoms, 0.5f) == Catch::Approx(0.547f).margin(0.01f));
    CHECK(enhanced_grade::apply(enhanced_grade::kEasternKingdoms, 0.0f) == 0.0f);
    CHECK(enhanced_grade::apply(enhanced_grade::kEasternKingdoms, 1.0f) == 1.0f);
    // Kalimdor's is the gentler of the two.
    CHECK(enhanced_grade::apply(enhanced_grade::kKalimdor, 0.7f) <
          enhanced_grade::apply(enhanced_grade::kEasternKingdoms, 0.7f));
}

TEST_CASE("Enhanced grade: only the two continents, and only around noon", "[enhanced]") {
    CHECK(enhanced_grade::curveForMap(0) == enhanced_grade::kEasternKingdoms);
    CHECK(enhanced_grade::curveForMap(1) == enhanced_grade::kKalimdor);
    CHECK(enhanced_grade::curveForMap(530) == nullptr);
    CHECK(enhanced_grade::curveForMap(36) == nullptr);
    CHECK(enhanced_grade::timeWeight(12.0f / 24.0f) == Catch::Approx(1.0f));
    CHECK(enhanced_grade::timeWeight(10.0f / 24.0f) == Catch::Approx(0.5f));
    CHECK(enhanced_grade::timeWeight(13.5f / 24.0f) == Catch::Approx(0.5f));
    CHECK(enhanced_grade::timeWeight(7.0f / 24.0f) == 0.0f);
    CHECK(enhanced_grade::timeWeight(16.0f / 24.0f) == 0.0f);
    CHECK(enhanced_grade::timeWeight(0.0f) == 0.0f);
}

TEST_CASE("Enhanced daylight: the measured trend, and nothing at weight 0", "[enhanced]") {
    namespace fp = enhanced_palette;
    const glm::vec3 sun(1.0f, 0.533f, 0.0f);  // 1.12's noon DirectColor, #ff8800
    const glm::vec3 same = fp::apply(sun, fp::kDirect, 0.0f);
    CHECK(same.x == Catch::Approx(sun.x));
    CHECK(same.y == Catch::Approx(sun.y));
    CHECK(same.z == Catch::Approx(sun.z));
    // A whiter sun of about the same brightness.
    const glm::vec3 lum(0.2126f, 0.7152f, 0.0722f);
    const glm::vec3 white = fp::apply(sun, fp::kDirect, 1.0f);
    CHECK(glm::dot(white, lum) == Catch::Approx(glm::dot(sun, lum) * 1.03f).margin(1e-3));
    CHECK(white.x - white.z < sun.x - sun.z);
    // A grey only changes brightness.
    const glm::vec3 g = fp::apply(glm::vec3(0.4f), fp::kSkyFog, 1.0f);
    CHECK(g.x == Catch::Approx(0.52f));
    CHECK(g.z == Catch::Approx(0.52f));
    CHECK(fp::appliesToMap(0));
    CHECK(fp::appliesToMap(1));
    CHECK_FALSE(fp::appliesToMap(530));
}

#ifdef WOWEE_SOURCE_DIR
TEST_CASE("Enhanced grade: fxaa.frag carries the same curves", "[enhanced]") {
    std::ifstream f(std::string(WOWEE_SOURCE_DIR) + "/assets/shaders/fxaa.frag.glsl");
    REQUIRE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    auto readArray = [&](const std::string& name) {
        std::vector<float> v;
        const size_t at = src.find("const float " + name + "[32] = float[](");
        if (at == std::string::npos) return v;
        const size_t end = src.find(");", at);
        const std::string body = src.substr(at, end - at);
        std::regex num(R"((\d+\.\d+))");
        for (auto it = std::sregex_iterator(body.begin(), body.end(), num); it != std::sregex_iterator(); ++it) {
            v.push_back(std::stof(it->str()));
        }
        return v;
    };
    const auto ek = readArray("kGradeEK");
    const auto kal = readArray("kGradeKal");
    REQUIRE(ek.size() == 32);
    REQUIRE(kal.size() == 32);
    for (int i = 0; i < 32; ++i) {
        CHECK(ek[i] == Catch::Approx(enhanced_grade::kEasternKingdoms[i]));
        CHECK(kal[i] == Catch::Approx(enhanced_grade::kKalimdor[i]));
    }
}
#endif

TEST_CASE("Enhanced preset: Ultra plus the Enhanced rows, on one-sample depth", "[enhanced]") {
    using namespace wowee::ui;
    const GraphicsPresetValues& f = kGraphicsPresets[kEnhancedPresetIndex];
    const GraphicsPresetValues& ultra = kGraphicsPresets[kClassicPresetIndex - 1];
    CHECK(f.enh.ambientOcclusion);
    CHECK(f.enh.colorGrade);
    CHECK(f.enh.enhancedDaylight);
    CHECK(f.enh.fillLight);
    CHECK(f.enh.sunHaze);
    CHECK(f.enh.bloom > 0.0f);
    CHECK(f.enh.longView);
    CHECK(f.enh.grassDistance == 200);
    CHECK(f.enh.shadows);
    CHECK(f.enh.lightShafts >= 2);
    CHECK(f.enh.mistDensity <= 1.0f);
    // The occlusion pass reads a one-sample depth: no multisampling.
    CHECK(f.antiAliasing == 0);
    CHECK(f.fxaa);
    CHECK(f.viewDistance >= ultra.viewDistance);
    // Neither new row is in the ladder: they are what the Enhanced row adds.
    for (int i = 0; i < kClassicPresetIndex; ++i) {
        CHECK_FALSE(kGraphicsPresets[i].enh.ambientOcclusion);
        CHECK_FALSE(kGraphicsPresets[i].enh.colorGrade);
        CHECK_FALSE(kGraphicsPresets[i].enh.enhancedDaylight);
        CHECK_FALSE(kGraphicsPresets[i].enh.fillLight);
        CHECK_FALSE(kGraphicsPresets[i].enh.sunHaze);
        CHECK(kGraphicsPresets[i].enh.bloom == 0.0f);
        CHECK_FALSE(kGraphicsPresets[i].enh.longView);
    }
}
