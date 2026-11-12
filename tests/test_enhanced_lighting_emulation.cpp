// Wowee's own enhanced lighting table (enhanced_lighting_emulation.hpp): the
// committed parameter file parses, a light's zone class comes from the
// WorldMapArea box it stands in, an identity grade gives 3.3.5's colours back
// (the sky with the table's exposure), the keys stay within a band, and the
// table round-trips through the WFL2 layout. All data here is made up.
#include <catch_amalgamated.hpp>

#include "rendering/enhanced_lighting_emulation.hpp"
#include "rendering/enhanced_lighting_table.hpp"

#include <fstream>
#include <sstream>
#include <string>

using namespace wowee::rendering;
namespace em = enhanced_lighting_emulation;
namespace fl = enhanced_lighting_table;

namespace {

std::string identityParams(const char* hours = "[0, 12]", int bands = 2) {
    std::string g, fog, sprite;
    for (int i = 0; i < bands; ++i) {
        g += std::string(i ? "," : "") + "[1,1,0,0]";
        fog += std::string(i ? "," : "") + "[1,0]";
        sprite += std::string(i ? "," : "") + "[0]";
    }
    const std::string cls = "\"sun\":[" + g + "],\"ambient\":[" + g + "],\"sky\":[" + g + "],\"fogColor\":[" +
                            g + "],\"fogRange\":[" + fog + "],\"airDensity\":0.003";
    return std::string("// a comment line\n{\"version\":1,\"bandHours\":") + hours +
           ",\"classes\":{\"default\":{\"zones\":[]," + cls + "},\"desert\":{\"zones\":[17]," + cls +
           "}},\"shared\":{\"sprite\":[" + sprite + "],\"clouds\":[" + g + "]}}";
}

struct FakeSet {
    uint16_t times[16] = {0, 720, 1440};
    uint32_t sun[16] = {0x102030, 0xf08010, 0x405060};
    uint32_t sky[16] = {0x000010, 0x30a0c4, 0x000020};
    uint16_t fogTimes[16] = {0};
    float fogEnd[16] = {12345.0f};
};

}  // namespace

TEST_CASE("Enhanced lighting emulation: the committed parameter file parses", "[enhanced]") {
    std::ifstream f(std::string(WOWEE_SOURCE_DIR) + "/" + em::kParamsFile);
    REQUIRE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    em::Params p;
    std::string error;
    REQUIRE(em::parseParams(ss.str(), p, error));
    CHECK(p.classes.front().name == "default");
    CHECK(p.classes.size() >= 7);
    CHECK(p.spriteWhite.size() == p.bandHours.size());
    for (const auto& c : p.classes) {
        CHECK(c.bands.size() == p.bandHours.size());
        CHECK(c.airDensity >= 0.0f);
    }
}

TEST_CASE("Enhanced lighting emulation: bad parameter files are refused", "[enhanced]") {
    em::Params p;
    std::string error;
    CHECK_FALSE(em::parseParams("not json", p, error));
    CHECK_FALSE(em::parseParams("{\"bandHours\":[0]}", p, error));
    // A grade list of the wrong length.
    CHECK_FALSE(em::parseParams(identityParams("[0, 6, 12]", 2), p, error));
    CHECK(em::parseParams(identityParams(), p, error));
}

TEST_CASE("Enhanced lighting emulation: a light's class is its zone box's", "[enhanced]") {
    em::Params p;
    std::string error;
    REQUIRE(em::parseParams(identityParams(), p, error));
    REQUIRE(p.classes[1].name == "desert");
    const std::vector<em::ZoneBox> boxes = {
        {1, 17, 2000.0f, -2000.0f, 1000.0f, -1000.0f},  // a big one
        {1, 99, 100.0f, -100.0f, 100.0f, -100.0f},      // a small one inside it
    };
    em::Light l;
    l.mapId = 1;
    l.worldX = 500.0f;
    l.worldY = 500.0f;
    CHECK(em::zoneOf(l, boxes) == 17);
    CHECK(em::classOf(l, boxes, p) == 1);
    l.worldX = l.worldY = 0.0f;  // the smaller box wins; zone 99 is in no list
    CHECK(em::zoneOf(l, boxes) == 99);
    CHECK(em::classOf(l, boxes, p) == 0);
    l.worldX = 1500.0f;  // outside both: the nearest
    CHECK(em::zoneOf(l, boxes) == 17);
    l.isMapDefault = true;
    CHECK(em::classOf(l, boxes, p) == 0);
}

TEST_CASE("Enhanced lighting emulation: a grade's parameters", "[enhanced]") {
    const std::array<float, 3> c{200.0f, 100.0f, 50.0f};
    const auto same = em::applyGrade(c, {});
    for (int i = 0; i < 3; ++i) CHECK(same[i] == Catch::Approx(c[i]));
    const auto grey = em::applyGrade(c, {1.0f, 0.0f, 0.0f, 0.0f});
    CHECK(grey[0] == Catch::Approx(grey[2]));
    // Warmth and tint leave the luminance alone.
    const auto warm = em::applyGrade(c, {1.0f, 1.0f, 0.2f, -0.1f});
    const float y0 = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    const float y1 = 0.2126f * warm[0] + 0.7152f * warm[1] + 0.0722f * warm[2];
    CHECK(y1 == Catch::Approx(y0).margin(1e-3));
    CHECK(warm[0] > c[0]);
    CHECK(warm[2] < c[2]);
}

TEST_CASE("Enhanced lighting emulation: identity gives 3.3.5 back and round-trips", "[enhanced]") {
    em::Params p;
    std::string error;
    REQUIRE(em::parseParams(identityParams(), p, error));
    FakeSet fake;
    std::vector<em::Light> lights(2);
    lights[0] = {1, 0, true, 0.0f, 0.0f, 12, 0};
    lights[1] = {2, 530, false, 0.0f, 0.0f, 12, 0};  // not on maps 0 or 1
    auto stock = [&](uint32_t id, em::StockSet& s) {
        if (id != 12) return false;
        s.colors[0] = {3, fake.times, fake.sun};
        s.colors[2] = {3, fake.times, fake.sky};
        s.floats[0] = {1, fake.fogTimes, fake.fogEnd};
        return true;
    };
    const fl::Table t = em::build(p, lights, {}, stock);
    REQUIRE(t.links.size() == 1);
    CHECK(t.links[0].lightId == 1);
    REQUIRE(t.profiles.size() == 1);
    const fl::Profile& prof = t.profiles[0];
    CHECK(prof.colorMask == ((1u << 0) | (1u << 2)));
    CHECK(prof.floatMask == 1u);
    CHECK(prof.keyCount == 3);  // 0:00, 6:00 and 12:00, the band times among them
    CHECK(t.keys[prof.firstKey + 1].color[0] == 0xf08010);
    const fl::Key& noon = t.keys[prof.firstKey + 2];
    CHECK(noon.time == 1440);
    CHECK(noon.color[0] == 0x405060);
    CHECK(fl::withoutSkyExposure(noon.color[2]) == 0x000020);
    CHECK(noon.value[0] == 12345.0f);
    CHECK(prof.airDensity == Catch::Approx(0.003f));

    fl::Table back;
    REQUIRE(fl::parse(em::serialize(t), back));
    CHECK(back.keys.size() == t.keys.size());
    CHECK(back.links.size() == 1);
    CHECK(back.keys[2].color[0] == 0x405060);
}

TEST_CASE("Enhanced lighting emulation: never more than sixteen keys", "[enhanced]") {
    em::Params p;
    std::string error;
    REQUIRE(em::parseParams(identityParams("[0, 6, 12, 18]", 4), p, error));
    uint16_t times[16];
    uint32_t cols[16];
    uint16_t times2[16];
    for (int i = 0; i < 16; ++i) {
        times[i] = static_cast<uint16_t>(i * 170 + 7);
        times2[i] = static_cast<uint16_t>(i * 170 + 90);
        cols[i] = 0x010101u * static_cast<uint32_t>(i * 10);
    }
    auto stock = [&](uint32_t, em::StockSet& s) {
        s.colors[0] = {16, times, cols};
        s.colors[1] = {16, times2, cols};
        return true;
    };
    const fl::Table t = em::build(p, {{1, 0, false, 0.0f, 0.0f, 5, 0}}, {}, stock);
    REQUIRE(t.profiles.size() == 1);
    CHECK(t.profiles[0].keyCount == fl::kMaxKeys);
    fl::Table back;
    CHECK(fl::parse(em::serialize(t), back));
    // The band times are always kept.
    int anchors = 0;
    for (uint32_t k = 0; k < t.profiles[0].keyCount; ++k) {
        const uint32_t time = t.keys[k].time;
        if (time == 0 || time == 720 || time == 1440 || time == 2160) ++anchors;
    }
    CHECK(anchors == 4);
}
