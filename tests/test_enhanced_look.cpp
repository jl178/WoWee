// The Enhanced look (docs/re/enhanced-lighting.md): the colour grade's curves
// and hour weight (enhanced_grade.hpp, which fxaa.frag mirrors), and the
// Enhanced preset row.
#include <catch_amalgamated.hpp>

#include "rendering/enhanced_grade.hpp"
#include "rendering/enhanced_lights.hpp"
#include "rendering/enhanced_palette.hpp"
#include "rendering/enhanced_tonemap.hpp"
#include "ui/graphics_presets.hpp"

#include <cstring>
#include <cmath>
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
    CHECK(f.enh.grass);
    CHECK(f.enh.grassDistance == 200);
    CHECK(f.enh.shadows);
    CHECK(f.enh.lightShafts >= 2);
    CHECK(f.enh.mistDensity <= 1.0f);
    // The occlusion pass reads a one-sample depth: no multisampling.
    CHECK(f.antiAliasing == 0);
    CHECK(f.fxaa);
    // Enhanced's detailed range (horizonStart 1900) and its far field past
    // it to 7000 (far_field.hpp), with the fog over that reach.
    CHECK(f.viewDistance == 1900.0f);
    CHECK(f.shadowDistance >= ultra.shadowDistance);
    CHECK(f.enh.farField);
    // Neither new row is in the ladder: they are what the Enhanced row adds.
    for (int i = 0; i < kClassicPresetIndex; ++i) {
        CHECK_FALSE(kGraphicsPresets[i].enh.ambientOcclusion);
        CHECK_FALSE(kGraphicsPresets[i].enh.colorGrade);
        CHECK_FALSE(kGraphicsPresets[i].enh.enhancedDaylight);
        CHECK_FALSE(kGraphicsPresets[i].enh.fillLight);
        CHECK_FALSE(kGraphicsPresets[i].enh.sunHaze);
        CHECK(kGraphicsPresets[i].enh.bloom == 0.0f);
        CHECK_FALSE(kGraphicsPresets[i].enh.longView);
        CHECK_FALSE(kGraphicsPresets[i].enh.farField);
    }
}


namespace {

// A tiny made-up table in enhanced_lights.bin's layout: two keys, one profile
// authoring the direct colour and the fog end, one link.
std::vector<uint8_t> enhancedLightsFixture() {
    std::vector<uint32_t> w = {0x314c4657u /* "WFL1" */, 2, 1, 1};
    auto key = [&](uint32_t time, uint32_t direct, float fogEnd) {
        w.push_back(time);
        for (uint32_t c = 0; c < enhanced_lights::kColorChannels; ++c) w.push_back(c == 0 ? direct : 0);
        const float values[enhanced_lights::kFloatChannels] = {fogEnd, 0.25f, 1.0f, 0.5f};
        for (float v : values) {
            uint32_t bits = 0;
            std::memcpy(&bits, &v, 4);
            w.push_back(bits);
        }
    };
    key(360, 0x102030u, 0.0f);
    key(1440, 0xa0b0c0u, 18000.0f);
    for (uint32_t v : {42u, 0u, 2u, 0x1u, 0x1u}) w.push_back(v);  // profile
    for (uint32_t v : {7u, 0u, 0u}) w.push_back(v);                 // link
    std::vector<uint8_t> bytes(w.size() * 4);
    std::memcpy(bytes.data(), w.data(), bytes.size());
    return bytes;
}

}  // namespace

TEST_CASE("Enhanced lights: the table file round-trips and refuses bad input", "[enhanced]") {
    namespace fl = enhanced_lights;
    const std::vector<uint8_t> good = enhancedLightsFixture();
    fl::Table t;
    REQUIRE(fl::parse(good, t));
    REQUIRE(t.keys.size() == 2);
    REQUIRE(t.profiles.size() == 1);
    REQUIRE(t.links.size() == 1);
    CHECK(t.keys[1].time == 1440);
    CHECK(t.keys[1].color[0] == 0xa0b0c0u);
    CHECK(t.keys[0].value[0] == 0.0f);   // a key without a fog end
    CHECK(t.keys[1].value[0] == 18000.0f);
    CHECK(t.profiles[0].enhancedParamsId == 42);
    CHECK(t.profiles[0].keyCount == 2);
    CHECK(t.links[0].lightId == 7);

    auto broken = [&](size_t word, uint32_t value) {
        std::vector<uint8_t> b = good;
        std::memcpy(b.data() + word * 4, &value, 4);
        return b;
    };
    const size_t keyWords = 1 + fl::kColorChannels + fl::kFloatChannels;
    const size_t profileAt = 4 + 2 * keyWords;
    const size_t linkAt = profileAt + 5;
    CHECK_FALSE(fl::parse(broken(0, 0x58585858u), t));          // magic
    CHECK(t.links.empty());
    CHECK_FALSE(fl::parse(broken(1, 3), t));                    // size
    CHECK_FALSE(fl::parse(broken(4 + keyWords, 100), t));       // keys out of order
    CHECK_FALSE(fl::parse(broken(4, 2880), t));                 // past midnight
    CHECK_FALSE(fl::parse(broken(profileAt + 2, 3), t));        // past the keys
    CHECK_FALSE(fl::parse(broken(linkAt + 2, 1), t));           // past the profiles
    CHECK_FALSE(fl::parse(broken(linkAt + 1, 1), t));           // not clear or storm
    CHECK_FALSE(fl::parse(std::vector<uint8_t>(good.begin(), good.end() - 4), t));
}

TEST_CASE("HDR tone curve: the 8-bit picture below the knee, no clip above", "[enhanced]") {
    namespace tm = enhanced_tonemap;
    // Below the knee, exactly what the 8-bit scene showed.
    for (float x = 0.0f; x <= tm::kKnee; x += 0.05f) CHECK(tm::toDisplay(x) == Catch::Approx(x));
    // Above it, rising, under 1, and continuous with slope 1 at the knee.
    float prev = tm::toDisplay(tm::kKnee);
    for (float x = tm::kKnee + 0.01f; x < 64.0f; x *= 1.1f) {
        const float d = tm::toDisplay(x);
        CHECK(d > prev);
        CHECK(d < 1.0f);
        prev = d;
    }
    const float e = 1e-3f;
    CHECK((tm::toDisplay(tm::kKnee + e) - tm::toDisplay(tm::kKnee)) / e == Catch::Approx(1.0f).margin(0.01f));
    // White in the 8-bit scene keeps most of its brightness.
    CHECK(tm::toDisplay(1.0f) > 0.88f);
    // Negatives and NaN show as black, as an UNORM target clamps them.
    CHECK(tm::toDisplay(-1.0f) == 0.0f);
    CHECK(tm::toDisplay(std::nanf("")) == 0.0f);
    // The inverse undoes it (the bloom adds in scene light and maps back).
    for (float x : {0.1f, 0.5f, 0.8f, 0.9f, 1.0f, 1.7f, 4.0f}) {
        CHECK(tm::toScene(tm::toDisplay(x)) == Catch::Approx(x).epsilon(0.01));
    }
}

TEST_CASE("HDR lighting row: on its page, in the Enhanced preset only", "[enhanced]") {
    using namespace wowee::ui;
    const EnhancementRow* row = findEnhancementRow("hdrscene");
    REQUIRE(row != nullptr);
    CHECK(std::string(row->category) == kEnhPageEnhanced);
    CHECK(row->stockValue == 0.0f);
    CHECK(row->recommendedValue == 0.0f);
    CHECK(Enhancements{}.hdrScene == false);
    CHECK(kGraphicsPresets[kEnhancedPresetIndex].enh.hdrScene);
    for (int i = 0; i < kClassicPresetIndex; ++i) CHECK_FALSE(kGraphicsPresets[i].enh.hdrScene);
}

TEST_CASE("HDR sun: brighter by day only", "[enhanced]") {
    namespace tm = enhanced_tonemap;
    // Sun below the horizon (its light travels upward): Light.dbc's own.
    CHECK(tm::sunLightScale(0.3f, tm::kSunLight) == 1.0f);
    CHECK(tm::sunLightScale(0.0f, tm::kSunLight) == 1.0f);
    // High sun: the full gain, which stays modest.
    CHECK(tm::sunLightScale(-0.9f, tm::kSunLight) == Catch::Approx(tm::kSunLight));
    CHECK(tm::kSunLight >= 1.0f);
    CHECK(tm::kSunLight <= 1.5f);  // Enhanced's F052 tops out at 1.5
}
