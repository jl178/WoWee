// The Enhanced look (docs/re/enhanced-lighting.md): the colour grade's curves
// and hour weight (enhanced_grade.hpp, which fxaa.frag mirrors), and the
// Enhanced preset row.
#include <catch_amalgamated.hpp>

#include "rendering/enhancements.hpp"
#include "rendering/enhanced_grade.hpp"
#include "rendering/enhanced_lights.hpp"
#include "rendering/enhanced_palette.hpp"
#include "rendering/enhanced_tonemap.hpp"
#include "rendering/leaf_light.hpp"
#include "rendering/stock_day_night.hpp"
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
    // A grey only changes brightness - and for the sky's fog band, whose
    // x1.30 is Enhanced's sky exposure (kSkyExposure), not even that.
    const glm::vec3 g = fp::apply(glm::vec3(0.4f), fp::kSkyFog, 1.0f);
    CHECK(g.x == Catch::Approx(0.4f));
    CHECK(g.z == Catch::Approx(0.4f));
    const glm::vec3 m = fp::apply(glm::vec3(0.4f), fp::kSkyMiddle, 1.0f);
    CHECK(m.x == Catch::Approx(0.4f * 1.19f / 1.3f));
    CHECK(fp::appliesToMap(0));
    CHECK(fp::appliesToMap(1));
    CHECK_FALSE(fp::appliesToMap(530));
}

TEST_CASE("Enhanced daylight: the sun and sky added as light, never past white", "[enhanced]") {
    namespace fp = enhanced_palette;
    // Elwynn's noon (Light 1): Enhanced's sun #eabd8a over the ambient #68829a.
    const glm::vec3 amb(0x68 / 255.0f, 0x82 / 255.0f, 0x9a / 255.0f);
    const glm::vec3 sun(0xea / 255.0f, 0xbd / 255.0f, 0x8a / 255.0f);
    const glm::vec3 d = fp::linearLightDirect(amb, sun, 1.0f);
    const glm::vec3 lit = amb + d;
    // Summed raw the lit face clips to white in every channel...
    CHECK(glm::all(glm::greaterThan(amb + sun, glm::vec3(1.0f))));
    // ...added as light it stays under it, warm (red over green over blue).
    CHECK(lit.x == Catch::Approx(0.98f).margin(0.01f));
    CHECK(lit.y == Catch::Approx(0.87f).margin(0.01f));
    CHECK(lit.z == Catch::Approx(0.79f).margin(0.01f));
    CHECK(lit.x > lit.y);
    CHECK(lit.y > lit.z);
    // No sun, no light added; weight 0 leaves the colour alone.
    CHECK(glm::length(fp::linearLightDirect(amb, glm::vec3(0.0f), 1.0f)) == Catch::Approx(0.0f).margin(1e-5));
    const glm::vec3 same = fp::linearLightDirect(amb, sun, 0.0f);
    CHECK(same.x == Catch::Approx(sun.x));
    CHECK(same.z == Catch::Approx(sun.z));
    // Never negative, never past one in sum.
    const glm::vec3 big = fp::linearLightDirect(glm::vec3(0.9f), glm::vec3(1.0f), 1.0f);
    CHECK(glm::all(glm::lessThanEqual(glm::vec3(0.9f) + big, glm::vec3(1.0f + 1e-5f))));
    CHECK(glm::all(glm::greaterThanEqual(big, glm::vec3(0.0f))));
}

TEST_CASE("Enhanced lights: the sky's exposure is taken out, the sun's light is not", "[enhanced]") {
    namespace fl = enhanced_lights;
    // Westfall's noon SkyMiddle: 3.3.5's #3c919e is Enhanced's #4ebdcd / 1.3.
    CHECK(fl::withoutSkyExposure(0x4ebdcdu) == 0x3c919eu);
    CHECK(fl::withoutSkyExposure(0xffffffu) == 0xc4c4c4u);
    CHECK(fl::withoutSkyExposure(0u) == 0u);
    for (uint32_t c : {2u, 3u, 4u, 5u, 6u, 7u, 10u, 11u}) CHECK(fl::isSkyChannel(c));
    for (uint32_t c : {0u, 1u, 8u, 9u, 12u, 13u, 14u, 17u}) CHECK_FALSE(fl::isSkyChannel(c));
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

#ifdef WOWEE_SOURCE_DIR
TEST_CASE("Enhanced ambient: Enhanced's half-Lambert ramp, in every lit model shader", "[enhanced]") {
    // material3_mesh_ps / material3_wmo_ps: 0.4562..1.2333 in linear light,
    // the same ramp as 0.7..1.1 display-encoded.
    CHECK(std::pow(0.4562f, 1.0f / 2.2f) == Catch::Approx(enhanced_palette::kAmbientAway).margin(0.005f));
    CHECK(std::pow(1.2333f, 1.0f / 2.2f) == Catch::Approx(enhanced_palette::kAmbientToward).margin(0.005f));
    CHECK(enhanced_palette::ambientRamp(-1.0f) == Catch::Approx(0.7f));
    CHECK(enhanced_palette::ambientRamp(0.0f) == Catch::Approx(0.9f));
    CHECK(enhanced_palette::ambientRamp(1.0f) == Catch::Approx(1.1f));
    CHECK(enhanced_palette::ambientRamp(-5.0f) == Catch::Approx(0.7f));
    // The shaders branch on the bit and carry the same ends.
    for (const char* name : {"m2.frag.glsl", "wmo.frag.glsl", "character.frag.glsl"}) {
        std::ifstream f(std::string(WOWEE_SOURCE_DIR) + "/assets/shaders/" + name);
        REQUIRE(f.good());
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string src = ss.str();
        INFO(name);
        CHECK(src.find("(localLightMeta.z & 128) != 0") != std::string::npos);
        CHECK(src.find("mix(0.7, 1.1, 0.5 + 0.5 * clamp(") != std::string::npos);
    }
    CHECK(enh_bits::kEnhancedAmbient == 128u);
}
#endif

TEST_CASE("Enhanced preset: Ultra plus the Enhanced rows, on one-sample depth", "[enhanced]") {
    using namespace wowee::ui;
    const GraphicsPresetValues& f = kGraphicsPresets[kEnhancedPresetIndex];
    const GraphicsPresetValues& ultra = kGraphicsPresets[kClassicPresetIndex - 1];
    CHECK_FALSE(f.enh.ambientOcclusion);  // off until clean (graphics_presets.hpp)
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

TEST_CASE("Enhanced lights: WFL2 carries the air's density", "[enhanced]") {
    namespace fl = enhanced_lights;
    // The WFL1 fixture with a density after the profile.
    const std::vector<uint8_t> v1 = enhancedLightsFixture();
    std::vector<uint32_t> w(v1.size() / 4);
    std::memcpy(w.data(), v1.data(), v1.size());
    const size_t keyWords = 1 + fl::kColorChannels + fl::kFloatChannels;
    const size_t profileAt = 4 + 2 * keyWords;
    std::vector<uint32_t> out(w.begin(), w.begin() + profileAt + 5);
    out[0] = 0x324c4657u;  // "WFL2"
    const float density = 0.001f;
    uint32_t bits = 0;
    std::memcpy(&bits, &density, 4);
    out.push_back(bits);
    out.insert(out.end(), w.begin() + profileAt + 5, w.end());
    std::vector<uint8_t> bytes(out.size() * 4);
    std::memcpy(bytes.data(), out.data(), bytes.size());

    fl::Table t;
    REQUIRE(fl::parse(bytes, t));
    CHECK(t.keys[1].color[0] == 0xa0b0c0u);
    CHECK(t.profiles[0].airDensity == Catch::Approx(0.001f));
    CHECK(t.links[0].lightId == 7);
    // A WFL1 file has no air; a negative density, or the WFL1 size under the
    // WFL2 magic, is refused.
    REQUIRE(fl::parse(v1, t));
    CHECK(t.profiles[0].airDensity == 0.0f);
    const float bad = -1.0f;
    std::memcpy(&bits, &bad, 4);
    std::vector<uint8_t> broken = bytes;
    std::memcpy(broken.data() + (profileAt + 5) * 4, &bits, 4);
    CHECK_FALSE(fl::parse(broken, t));
    std::vector<uint8_t> mislabelled = v1;
    std::memcpy(mislabelled.data(), "WFL2", 4);
    CHECK_FALSE(fl::parse(mislabelled, t));
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

TEST_CASE("Enhanced sun: stock bearing, lower noon, earlier dusk", "[enhanced][sun]") {
    // Wow.exe 1.60.1: the sun's phi table 95 -> 25 -> 95 degrees, theta 45
    // all day (stock_day_night::kEnhancedSunPhi).
    const glm::vec3 noon = enhancedSunDirection(1440.0f);
    CHECK(std::asin(noon.z) * 57.29578f == Catch::Approx(65.0f).margin(1.5f));
    // Same bearing as the stock sprite at any hour it is up.
    for (float h : {8.0f, 9.37f, 15.4f, 18.0f}) {
        const glm::vec3 f = enhancedSunDirection(h * 120.0f);
        const glm::vec3 st = stockSunDirection(h * 120.0f);
        CHECK(std::atan2(f.y, f.x) == Catch::Approx(std::atan2(st.y, st.x)).margin(0.01f));
    }
    // Up a little before 06:00, down a little before 20:00 (stock 20:30).
    CHECK(enhancedSunDirection(5.8f * 120.0f).z < 0.0f);
    CHECK(enhancedSunDirection(6.1f * 120.0f).z > 0.0f);
    CHECK(enhancedSunDirection(19.8f * 120.0f).z > 0.0f);
    CHECK(enhancedSunDirection(20.0f * 120.0f).z < 0.0f);
    CHECK(stockSunDirection(20.0f * 120.0f).z > 0.0f);
    // 9:22, Jered's capture: about 37 degrees up.
    CHECK(std::asin(enhancedSunDirection(9.37f * 120.0f).z) * 57.29578f == Catch::Approx(37.2f).margin(1.5f));
}

TEST_CASE("Enhanced light: from the sun by day, stock at night", "[enhanced][sun]") {
    namespace fp = enhanced_palette;
    const glm::vec3 stockTravel = stockLightDirection(9.37f * 120.0f);
    const glm::vec3 toSun = enhancedSunDirection(9.37f * 120.0f);
    const glm::vec3 day = fp::sunLightDirection(stockTravel, toSun);
    CHECK(glm::dot(day, -toSun) == Catch::Approx(1.0f).margin(1e-4f));
    // The sun under the horizon: the stock light, which stays up all night.
    const glm::vec3 nightStock = stockLightDirection(1.0f * 120.0f);
    const glm::vec3 nightSun = enhancedSunDirection(1.0f * 120.0f);
    REQUIRE(nightSun.z < 0.0f);
    const glm::vec3 night = fp::sunLightDirection(nightStock, nightSun);
    CHECK(glm::dot(night, nightStock) == Catch::Approx(1.0f).margin(1e-4f));
    CHECK(night.z < 0.0f);  // still travelling down
    // Unit length while it eases in at dawn.
    const glm::vec3 dawn = fp::sunLightDirection(stockLightDirection(6.05f * 120.0f),
                                                 enhancedSunDirection(6.05f * 120.0f));
    CHECK(glm::length(dawn) == Catch::Approx(1.0f).margin(1e-4f));
}

TEST_CASE("HDR sun: brighter by day only", "[enhanced]") {
    namespace tm = enhanced_tonemap;
    // Sun below the horizon (its light travels upward): Light.dbc's own.
    CHECK(tm::sunLightScale(0.3f, tm::kSunLight) == 1.0f);
    CHECK(tm::sunLightScale(0.0f, tm::kSunLight) == 1.0f);
    // High sun: the full gain, which stays modest.
    CHECK(tm::sunLightScale(-0.9f, tm::kSunLight) == Catch::Approx(tm::kSunLight));
    CHECK(tm::kSunLight >= 1.0f);
    // Enhanced's F052 (1.5 at most) is a linear multiplier; this one also
    // stands in for the mid-tone lift it replaced (section 2d).
    CHECK(tm::kSunLight <= 2.0f);
    // With it, the softer shoulder under Enhanced's daylight: the identity
    // below half of white, the Barrens' bright road compressed above it.
    CHECK(tm::kEnhancedKnee < tm::kKnee);
    CHECK(tm::kEnhancedKnee >= 0.4f);
    CHECK(tm::toDisplay(0.45f, tm::kEnhancedKnee) == Catch::Approx(0.45f));
    CHECK(tm::toDisplay(1.2f, tm::kEnhancedKnee) < tm::toDisplay(1.2f));
}

TEST_CASE("Enhanced water row: on its page, in the Enhanced preset only, with HD water", "[enhanced][water]") {
    using namespace wowee::ui;
    const EnhancementRow* row = findEnhancementRow("enhancedwater");
    REQUIRE(row != nullptr);
    CHECK(std::string(row->category) == kEnhPageEnhanced);
    CHECK(row->stockValue == 0.0f);
    CHECK(Enhancements{}.enhancedWater == false);
    const auto& f = kGraphicsPresets[kEnhancedPresetIndex].enh;
    CHECK(f.enhancedWater);
    CHECK(f.hdWater);
    for (int i = 0; i < kClassicPresetIndex; ++i) CHECK_FALSE(kGraphicsPresets[i].enh.enhancedWater);
}

TEST_CASE("Enhanced water: water_hd.frag carries LiquidType 1.60.1's columns", "[enhanced][water]") {
    std::ifstream f(std::string(WOWEE_SOURCE_DIR) + "/assets/shaders/water_hd.frag.glsl");
    REQUIRE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    // Float_0..2 (absorption) and Float_3 (density) of 1324 (Generic - Lake
    // - Forest), 1250 (Generic - Ocean) and 1344 (Stormwind - Canals), and
    // their Color_1 / Color_2.
    CHECK(src.find("vec3(0.283, 0.477, 2.657), 2.0") != std::string::npos);
    CHECK(src.find("vec3(0.25, 0.75, 6.0), 8.0") != std::string::npos);
    CHECK(src.find("vec3(0.3, 1.0, 1.0), 0.5") != std::string::npos);
    CHECK(src.find("vec3(0x9b, 0xbf, 0xec)") != std::string::npos);
    CHECK(src.find("vec3(0x48, 0xaa, 0xb0)") != std::string::npos);
    CHECK(src.find("vec3(0x1b, 0xb2, 0xe6)") != std::string::npos);
    // Enhanced's mode is its own build of the file (ENHANCED_WATER), so HD
    // water alone compiles none of it.
    CHECK(src.find("#ifdef ENHANCED_WATER\n    const bool fw = true;") != std::string::npos);
    CHECK(src.find("    const bool fw = false;") != std::string::npos);
    std::ifstream cm(std::string(WOWEE_SOURCE_DIR) + "/CMakeLists.txt");
    REQUIRE(cm.good());
    std::stringstream cms;
    cms << cm.rdbuf();
    CHECK(cms.str().find("water_hd.frag.glsl|water_fw.frag.spv|-DENHANCED_WATER") != std::string::npos);
}

TEST_CASE("Sun through leaves: its row, the Enhanced preset, the shader bit", "[enhanced]") {
    using namespace wowee::ui;
    const EnhancementRow* row = findEnhancementRow("leaflight");
    REQUIRE(row != nullptr);
    CHECK(std::string(row->category) == kEnhPageEnhanced);
    CHECK(row->stockValue == 0.0f);
    CHECK(Enhancements{}.leafLight == false);
    CHECK(kGraphicsPresets[kEnhancedPresetIndex].enh.leafLight);
    for (int i = 0; i < kClassicPresetIndex; ++i) CHECK_FALSE(kGraphicsPresets[i].enh.leafLight);
    // Off adds no bit, so m2.frag draws foliage exactly as before.
    CHECK((shaderBits(Enhancements{}) & enh_bits::kLeafLight) == 0u);
    Enhancements e;
    e.leafLight = true;
    CHECK((shaderBits(e) & enh_bits::kLeafLight) == 64u);

    std::ifstream f(std::string(WOWEE_SOURCE_DIR) + "/assets/shaders/m2.frag.glsl");
    REQUIRE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    CHECK(ss.str().find("(localLightMeta.z & 64) != 0") != std::string::npos);
}

TEST_CASE("Sun through leaves: the sun's direction and the day weight", "[enhanced]") {
    namespace ll = leaf_light;
    CHECK(ll::dayWeight(-0.3f) == 0.0f);
    CHECK(ll::dayWeight(0.0f) == 0.0f);
    CHECK(ll::dayWeight(0.15f) == 1.0f);
    CHECK(ll::dayWeight(0.9f) == 1.0f);
    CHECK(ll::dayWeight(0.075f) == Catch::Approx(0.5f));
    // Packed as the shaders' unpackSnorm4x8 reads it: xyz toward the sun, w
    // the day weight; a low morning sun 6 degrees up in the east.
    const glm::vec4 back = glm::unpackSnorm4x8(ll::packSun(glm::vec3(0.0f, 9.5f, 1.0f)));
    const glm::vec3 d = glm::normalize(glm::vec3(0.0f, 9.5f, 1.0f));
    CHECK(back.y == Catch::Approx(d.y).margin(0.01f));
    CHECK(back.z == Catch::Approx(d.z).margin(0.01f));
    CHECK(back.w == Catch::Approx(ll::dayWeight(d.z)).margin(0.01f));
    // The sun down: no weight, whatever the light direction says.
    CHECK(glm::unpackSnorm4x8(ll::packSun(glm::vec3(0.2f, 0.1f, -0.5f))).w == 0.0f);
}

TEST_CASE("Enhanced shade: less chroma, a cooler grey, by day only", "[enhanced]") {
    namespace fp = enhanced_palette;
    CHECK(fp::kHdrShadeChroma > 0.4f);
    CHECK(fp::kHdrShadeChroma < 0.8f);
    // Night and the horizon: none; from 0.15 of the way up: all.
    CHECK(fp::dayShare(-0.2f) == 0.0f);
    CHECK(fp::dayShare(0.0f) == 0.0f);
    CHECK(fp::dayShare(0.5f) == 1.0f);
    // The tint: white at weight 0; otherwise unit luminance, bluer than
    // white and leaning neither red nor green (Goldshire 9:22's ambient).
    const glm::vec3 amb(0.40f, 0.51f, 0.60f);
    CHECK(fp::shadeTint(amb, 0.0f) == glm::vec3(1.0f));
    const glm::vec3 t = fp::shadeTint(amb, 1.0f);
    CHECK(glm::dot(t, glm::vec3(0.2126f, 0.7152f, 0.0722f)) == Catch::Approx(1.0f).margin(1e-4f));
    CHECK(t.r == Catch::Approx(t.g));
    CHECK(t.b > t.r);
    CHECK(t.b < 1.5f);
}

#ifdef WOWEE_SOURCE_DIR
TEST_CASE("Enhanced shade: fxaa.frag reads its tint and chroma as the push block's seventh vec4", "[enhanced]") {
    std::ifstream f(std::string(WOWEE_SOURCE_DIR) + "/assets/shaders/fxaa.frag.glsl");
    REQUIRE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    // After enhancedSat (offset 92): a whole vec4 at 96, then the corner
    // blur's at 112, 128 bytes in all, as post_process_pipeline.cpp pushes
    // them.
    const auto sat = src.find("float enhancedSat;");
    const auto shade = src.find("vec4  enhancedShade;");
    const auto corner = src.find("vec4  cornerBlur;");
    REQUIRE(sat != std::string::npos);
    REQUIRE(shade != std::string::npos);
    REQUIRE(corner != std::string::npos);
    CHECK(sat < shade);
    CHECK(shade < corner);
    CHECK(src.find("pc.enhancedShade.a") != std::string::npos);
    CHECK(src.find("pc.enhancedShade.rgb") != std::string::npos);
}
#endif

TEST_CASE("Enhanced's sun shadows: its row, the Enhanced preset, the shader bit", "[enhanced]") {
    using namespace wowee::ui;
    const EnhancementRow* row = findEnhancementRow("enhancedshadows");
    REQUIRE(row != nullptr);
    CHECK(std::string(row->category) == kEnhPageEnhanced);
    CHECK(row->stockValue == 0.0f);
    CHECK(Enhancements{}.enhancedShadows == false);
    CHECK(kGraphicsPresets[kEnhancedPresetIndex].enh.enhancedShadows);
    for (int i = 0; i <= kClassicPresetIndex; ++i) CHECK_FALSE(kGraphicsPresets[i].enh.enhancedShadows);
    // Off adds no bit, so the grass is lit exactly as before.
    CHECK((shaderBits(Enhancements{}) & enh_bits::kEnhancedShadows) == 0u);
    Enhancements e;
    e.enhancedShadows = true;
    CHECK((shaderBits(e) & enh_bits::kEnhancedShadows) == 512u);

    std::ifstream f(std::string(WOWEE_SOURCE_DIR) + "/assets/shaders/grass.frag.glsl");
    REQUIRE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    CHECK(ss.str().find("(localLightMeta.z & 512) != 0") != std::string::npos);
}
