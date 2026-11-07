// The weather enhancements' rules (rendering/weather_fx.hpp,
// docs/enhancements-plan.md E23-E31): the intensity mapping is stock at 1,
// the preview's rows, the zone climates, the lightning's timing and thunder,
// snow settling, the sandstorm's haze and the mist sheets' fade.
#include <catch_amalgamated.hpp>

#include "rendering/weather_fx.hpp"
#include "rendering/stock_weather.hpp"
#include "rendering/enhancements.hpp"
#include "rendering/stock_screen_fx.hpp"
#include "game/weather_state.hpp"
#include "ui/enhancement_rows.hpp"

#include <cstring>
#include <string>

namespace wfx = wowee::rendering::weather_fx;
namespace sw = wowee::rendering::stock_weather;
using wowee::rendering::Enhancements;

TEST_CASE("weather intensity 1 is the stock count, and only ever adds", "[weather_fx]") {
    // The stock target, as Weather::update computed it before the slider.
    for (float level : {0.0f, 0.1f, 0.5f, 1.0f}) {
        for (float density : {0.1f, 0.66f, 1.0f}) {
            for (float typeScale : {0.7f, 1.0f}) {
                const int stock = static_cast<int>(3000 * typeScale * level * density);
                CHECK(wfx::particleTarget(3000, typeScale, level, density, 1.0f) == stock);
                // Below the slider's bottom is still stock, never less.
                CHECK(wfx::particleTarget(3000, typeScale, level, density, 0.5f) == stock);
                CHECK(wfx::particleTarget(3000, typeScale, level, density, 2.0f) >= stock);
            }
        }
    }
    CHECK(wfx::countScale(1.0f) == 1.0f);
    CHECK(wfx::countScale(1.75f) == Catch::Approx(1.75f));
    CHECK(wfx::countScale(9.0f) == wfx::kMaxIntensity);
    CHECK(wfx::sideScale(1.0f) == 1.0f);
    CHECK(wfx::sideScale(2.5f) < wfx::countScale(2.5f));
    // Nothing falls below the stock quarter-grade threshold, at any setting.
    const float level = wowee::rendering::stock_screen_fx::particleLevel(0.2f);
    CHECK(wfx::particleTarget(3000, 1.0f, level, 1.0f, 2.5f) == 0);
}

TEST_CASE("the weather intensity row's stock value is the identity", "[weather_fx]") {
    const auto* row = wowee::ui::findEnhancementRow("weatherintensity");
    REQUIRE(row != nullptr);
    CHECK(row->stockValue == wfx::kStockIntensity);
    CHECK(row->minValue == wfx::kStockIntensity);
    CHECK(row->maxValue == wfx::kMaxIntensity);
    CHECK(Enhancements{}.weatherIntensity == wfx::kStockIntensity);
}

TEST_CASE("the preview forces real weather rows and is off at stock", "[weather_fx]") {
    using wowee::game::WeatherKind;
    using wowee::game::weatherKind;
    CHECK_FALSE(wfx::previewWeather(0).active);
    CHECK_FALSE(wfx::previewWeather(-1).active);
    CHECK_FALSE(wfx::previewWeather(wfx::kPreviewCount).active);
    CHECK_FALSE(wfx::previewWeather(Enhancements{}.weatherPreview).active);

    CHECK(weatherKind(wfx::previewWeather(1).state) == WeatherKind::RAIN);
    CHECK(weatherKind(wfx::previewWeather(3).state) == WeatherKind::RAIN);
    CHECK(weatherKind(wfx::previewWeather(4).state) == WeatherKind::THUNDERSTORM);
    CHECK(weatherKind(wfx::previewWeather(5).state) == WeatherKind::SNOW);
    CHECK(weatherKind(wfx::previewWeather(7).state) == WeatherKind::SNOW);
    CHECK(weatherKind(wfx::previewWeather(8).state) == WeatherKind::SANDSTORM);
    CHECK(weatherKind(wfx::previewWeather(9).state) == WeatherKind::SANDSTORM);
    for (int i = 1; i < wfx::kPreviewCount; ++i) {
        INFO("preview " << i);
        const auto f = wfx::previewWeather(i);
        CHECK(f.active);
        // Every preview is heavy enough for the stock emitters to draw.
        CHECK(wowee::rendering::stock_screen_fx::particleLevel(f.grade) > 0.0f);
    }

    // The row's choices are the rule's, one for one.
    const auto* row = wowee::ui::findEnhancementRow("weatherpreview");
    REQUIRE(row != nullptr);
    CHECK(std::string(row->choices) == wfx::kPreviewChoices);
    CHECK(static_cast<int>(row->maxValue) == wfx::kPreviewCount - 1);
}

TEST_CASE("zones have the climate their art shows", "[weather_fx]") {
    // Snowfields.
    for (uint32_t z : {1u, 618u, 67u, 210u, 3537u, 495u, 65u}) {
        INFO("zone " << z);
        CHECK((wfx::climateForZone(z) & wfx::kCold) != 0);
        CHECK((wfx::climateForZone(z) & wfx::kFireflies) == 0);
    }
    // Deserts.
    for (uint32_t z : {440u, 1377u, 3u}) {
        INFO("zone " << z);
        CHECK((wfx::climateForZone(z) & wfx::kDry) != 0);
        CHECK((wfx::climateForZone(z) & (wfx::kCold | wfx::kFireflies)) == 0);
    }
    // Woods and meadows; Grizzly Hills is both green and cold at night.
    for (uint32_t z : {12u, 331u, 141u, 3430u, 33u}) {
        INFO("zone " << z);
        CHECK(wfx::climateForZone(z) == wfx::kFireflies);
    }
    CHECK(wfx::climateForZone(394) == (wfx::kCool | wfx::kFireflies));
    // Cities and anything unknown: nothing.
    CHECK(wfx::climateForZone(1519) == wfx::kTemperate);  // Stormwind City
    CHECK(wfx::climateForZone(0) == wfx::kTemperate);
    // Sholazar Basin is Northrend's jungle, not a snowfield.
    CHECK((wfx::climateForZone(3711) & wfx::kCold) == 0);
}

TEST_CASE("night and fireflies follow the clock and the climate", "[weather_fx]") {
    CHECK(wfx::nightAmount(12.0f) == 0.0f);
    CHECK(wfx::nightAmount(0.0f) == 1.0f);
    CHECK(wfx::nightAmount(23.0f) == 1.0f);
    CHECK(wfx::nightAmount(24.0f + 12.0f) == 0.0f);
    CHECK(wfx::nightAmount(20.25f) > 0.0f);
    CHECK(wfx::nightAmount(20.25f) < 1.0f);
    CHECK(wfx::nightAmount(5.75f) > 0.0f);
    CHECK(wfx::nightAmount(5.75f) < 1.0f);

    CHECK(wfx::fireflyAmount(wfx::kFireflies, 1.0f, false, false) == 1.0f);
    CHECK(wfx::fireflyAmount(wfx::kFireflies, 12.0f, false, false) == 0.0f);
    CHECK(wfx::fireflyAmount(wfx::kFireflies, 1.0f, true, false) == 0.0f);   // rain
    CHECK(wfx::fireflyAmount(wfx::kFireflies, 1.0f, false, true) == 0.0f);   // indoors
    CHECK(wfx::fireflyAmount(wfx::kCold, 1.0f, false, false) == 0.0f);
}

TEST_CASE("lightning: thunderstorms only, thunder after the flash", "[weather_fx]") {
    CHECK_FALSE(wfx::stormHasLightning(false, 1.0f));
    CHECK_FALSE(wfx::stormHasLightning(true, 0.2f));
    CHECK(wfx::stormHasLightning(true, 0.6f));

    // A heavier storm strikes more often; the slider a little more.
    CHECK(wfx::strikeInterval(1.0f, 0.5f) < wfx::strikeInterval(0.3f, 0.5f));
    CHECK(wfx::strikeInterval(1.0f, 0.5f, 2.5f) < wfx::strikeInterval(1.0f, 0.5f));
    CHECK(wfx::strikeInterval(1.0f, 0.0f) > 0.0f);

    // Distances, delays and levels: far is later, fainter and quieter.
    CHECK(wfx::strikeDistance(0.0f) == Catch::Approx(250.0f));
    CHECK(wfx::strikeDistance(1.0f) == Catch::Approx(3000.0f));
    CHECK(wfx::thunderDelay(375.0f) == Catch::Approx(1.0f));
    CHECK(wfx::thunderDelay(3000.0f) == Catch::Approx(8.0f));
    CHECK(wfx::flashStrength(250.0f) == Catch::Approx(1.0f));
    CHECK(wfx::flashStrength(3000.0f) == Catch::Approx(0.25f));
    CHECK(wfx::thunderVolume(3000.0f) < wfx::thunderVolume(300.0f));

    // The flash: dark before, a first pulse, gone by the end.
    CHECK(wfx::flashEnvelope(-0.1f, 0.5f) == 0.0f);
    CHECK(wfx::flashEnvelope(0.02f, 0.5f) == Catch::Approx(1.0f));
    CHECK(wfx::flashEnvelope(0.95f, 0.5f) == 0.0f);
    for (float t = 0.0f; t < 1.0f; t += 0.01f) {
        const float v = wfx::flashEnvelope(t, 0.3f);
        CHECK(v >= 0.0f);
        CHECK(v <= 1.0f);
    }
}

TEST_CASE("snow settles over minutes and melts slower", "[weather_fx]") {
    CHECK(wfx::snowCoverTarget(false, 1.0f) == 0.0f);
    CHECK(wfx::snowCoverTarget(true, 0.2f) == 0.0f);  // too light to draw, too light to settle
    CHECK(wfx::snowCoverTarget(true, 1.0f) == Catch::Approx(1.0f));
    CHECK(wfx::snowCoverTarget(true, 0.6f) < wfx::snowCoverTarget(true, 1.0f));

    float c = 0.0f;
    for (int i = 0; i < 600; ++i) c = wfx::easeSnowCover(c, 1.0f, 0.1f);  // a minute
    CHECK(c > 0.25f);
    CHECK(c < 0.45f);
    for (int i = 0; i < 12000; ++i) c = wfx::easeSnowCover(c, 1.0f, 0.1f);
    CHECK(c > 0.99f);
    float m = c;
    for (int i = 0; i < 600; ++i) m = wfx::easeSnowCover(m, 0.0f, 0.1f);  // a minute of melt
    CHECK(c - m < 0.4f);
    for (int i = 0; i < 40000; ++i) m = wfx::easeSnowCover(m, 0.0f, 0.1f);
    CHECK(m == 0.0f);  // lands on bare, so the shaders' early-out is taken
    CHECK(wfx::easeSnowCover(0.0f, 0.0f, 0.016f) == 0.0f);
}

TEST_CASE("sandstorm haze and mist sheets", "[weather_fx]") {
    CHECK(wfx::sandFogScale(0.0f) == 1.0f);
    CHECK(wfx::sandFogScale(1.0f) == Catch::Approx(0.6f));
    CHECK(wfx::sandFogScale(1.0f, 2.5f) >= 0.4f);  // never a wall

    // Rain and snow sheets only past half their level, as the stock mist.
    CHECK(wfx::mistSheetTarget(false, 0.5f, 1.0f, 40) == 0);
    CHECK(wfx::mistSheetTarget(false, 1.0f, 1.0f, 40) > 0);
    CHECK(wfx::mistSheetTarget(true, 0.3f, 1.0f, 40) > 0);
    CHECK(wfx::mistSheetTarget(false, 1.0f, 2.5f, 40) <= 40);
    // The stock mist's fade: none inside 6 yd, all past 18.
    CHECK(wfx::mistDistanceFade(5.0f) == 0.0f);
    CHECK(wfx::mistDistanceFade(12.0f) == Catch::Approx(0.5f));
    CHECK(wfx::mistDistanceFade(30.0f) == 1.0f);
}

TEST_CASE("every weather row is off in Classic and on the weather page", "[weather_fx]") {
    const Enhancements stock{};
    for (const char* key : {"weatherintensity", "variedrain", "softsnow", "weathersheets", "snowcover",
                            "stormlightning", "sandstorms", "coldbreath", "fireflies", "rainindoors",
                            "weatherpreview", "wetground", "rainripples", "weatherwind"}) {
        INFO(key);
        const auto* row = wowee::ui::findEnhancementRow(key);
        REQUIRE(row != nullptr);
        CHECK(std::strcmp(row->category, wowee::ui::kEnhPageWeather) == 0);
        CHECK(wowee::ui::enhancementValue(stock, *row) == row->stockValue);
    }
    // The preview never starts on, whatever the set.
    CHECK(wowee::ui::recommendedEnhancements().weatherPreview == 0);
}

TEST_CASE("stock: the weather draws where the sky does", "[weather_fx][stock_weather]") {
    // With the portal walk: only a sealed interior hides it - a town's
    // street (an exterior group's box: Gadgetzan, K3) does not.
    CHECK(sw::weatherDrawsFromCamera(true, false, false));
    CHECK(sw::weatherDrawsFromCamera(true, false, true));   // a doorway open to the sky
    CHECK_FALSE(sw::weatherDrawsFromCamera(true, true, true));
    // Without it, an interior group's box.
    CHECK(sw::weatherDrawsFromCamera(false, false, false));
    CHECK_FALSE(sw::weatherDrawsFromCamera(false, false, true));
}

TEST_CASE("stock: IsOutdoors", "[weather_fx][stock_weather]") {
    CHECK(sw::playerOutdoors(true, false, 0));                // exterior group
    CHECK_FALSE(sw::playerOutdoors(true, true, 0x4000000u));  // interior group wins
    CHECK(sw::playerOutdoors(false, false, 0));
    CHECK_FALSE(sw::playerOutdoors(false, false, 0x2000000u));
    CHECK(sw::playerOutdoors(false, false, 0x6000000u));
}

TEST_CASE("stock: the mist's rates, fades and texture", "[weather_fx][stock_weather]") {
    // Rain and snow only past half the level; sand at any.
    CHECK(sw::mistRate(sw::kRainMist, 0.5f, 1.0f) == 0.0f);
    CHECK(sw::mistRate(sw::kRainMist, 1.0f, 1.0f) == Catch::Approx(38.0f));
    CHECK(sw::mistRate(sw::kSnowMist, 1.0f, 0.66f) == Catch::Approx(48.0f * 0.66f));
    CHECK(sw::mistRate(sw::kSandMist, 0.25f, 1.0f) == Catch::Approx(16.0f));
    CHECK(std::string(sw::kSandMist.texture).find("WeatherMistGrainy01") != std::string::npos);
    // 0.4 s in and out.
    CHECK(sw::mistLifeAlpha(0.0f, 2.7f) == 0.0f);
    CHECK(sw::mistLifeAlpha(0.2f, 2.7f) == Catch::Approx(0.5f));
    CHECK(sw::mistLifeAlpha(1.3f, 2.7f) == 1.0f);
    CHECK(sw::mistLifeAlpha(2.7f, 2.7f) == 0.0f);
    CHECK(sw::mistNearFade(6.0f) == 0.0f);
    CHECK(sw::mistNearFade(18.0f) == 1.0f);
}

TEST_CASE("stock: sand grains and the heading", "[weather_fx][stock_weather]") {
    CHECK(sw::sandRate(1.0f, 0.66f) == Catch::Approx(21120.0f));
    CHECK(sw::sandSizeScale(5.0f) == 1.0f);
    CHECK(sw::sandSizeScale(20.0f) == Catch::Approx(0.5f));
    CHECK(sw::sandSizeScale(30.0f) == 0.0f);
    CHECK(sw::sandAlpha(0.1f, 3.2f) == Catch::Approx(0.5f));
    CHECK(sw::sandAlpha(1.0f, 3.2f) == 1.0f);
    // Slower than a yard a second: the facing; else the way it moves.
    CHECK(sw::heading(0.5f, 0.0f, 1.0f) == 1.0f);
    CHECK(sw::heading(0.0f, 7.0f, 1.0f) == Catch::Approx(1.5707963f));
    CHECK(sw::heading(0.0f, -7.0f, 1.0f) == Catch::Approx(4.712389f));
}

TEST_CASE("stock: what makes a place cold", "[weather_fx][stock_weather]") {
    // AreaTable flag 0x1: Dun Morogh (1, 0x41) is; its areas take it.
    CHECK(sw::areaIsCold(0x41u, false, 0));
    CHECK(sw::areaIsCold(0x40u, true, 0x41u));
    // An area with 0x2 keeps its own flags, whatever its zone.
    CHECK_FALSE(sw::areaIsCold(0x2u, true, 0x41u));
    CHECK(sw::areaIsCold(0x3u, true, 0x40u));
    // Grizzly Hills (394) has no 0x1.
    CHECK_FALSE(sw::areaIsCold(0x40u, false, 0));
}

TEST_CASE("stock: Weather.dbc picks the emitter, WMOAreaTable forces indoors", "[weather_fx][stock_weather]") {
    sw::WeatherRow sand;
    sand.known = true;
    sand.effect = 3;
    // State 106 is a sand row in 3.3.5's Weather.dbc, whatever its name.
    CHECK(sw::effectFor(&sand, 106) == sw::WeatherEffect::SAND);
    CHECK(sw::effectFor(nullptr, 106) == sw::WeatherEffect::SNOW);
    CHECK(sw::effectFor(nullptr, 86) == sw::WeatherEffect::RAIN);
    sw::WeatherRow fog;
    fog.known = true;
    fog.effect = 0;
    CHECK(sw::effectFor(&fog, 1) == sw::WeatherEffect::NONE);
    // In a group: the row's 0x4 makes it outdoors, 0x2 indoors.
    CHECK(sw::playerOutdoors(true, true, 0, 0x4u));
    CHECK_FALSE(sw::playerOutdoors(true, false, 0, 0x2u));
    CHECK(sw::playerOutdoors(true, false, 0, 0));
}

TEST_CASE("stock: light scale easing, the mist's steep cut, which breath", "[weather_fx][stock_weather]") {
    // (|change| * 4 + 0.001) * 5 s: a full change takes 20 s.
    CHECK(sw::lightScaleSeconds(1.0f, 0.0f) == Catch::Approx(20.005f));
    CHECK(sw::lightScaleSeconds(0.5f, 0.5f) == Catch::Approx(0.005f));
    // Level ground: no cut. A step of more than half a yard: cut there.
    const float flat[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(sw::mistSteepCut(flat, 8) == -1);
    const float step[8] = {0, 0, 0, 0.6f, 0.6f, 0.6f, 0.6f, 0.6f};
    CHECK(sw::mistSteepCut(step, 8) == 2);   // the half-yard step is the one sample ahead of 2
    const float slope[8] = {0, 0.3f, 0.6f, 0.9f, 1.2f, 1.5f, 1.8f, 2.1f};
    CHECK(sw::mistSteepCut(slope, 8) == -1);  // 0.3 a sample, 0.9 over three
    const float wall[8] = {0, 0, 0, 0, 0, 2.0f, 2.0f, 2.0f};
    CHECK(sw::mistSteepCut(wall, 8) == 2);
    CHECK(sw::breathFor(true, 0.6f, true, true) == sw::Breath::DRUNK);
    CHECK(sw::breathFor(false, 0.9f, true, true) == sw::Breath::UNDERWATER);
    CHECK(sw::breathFor(true, 0.4f, false, true) == sw::Breath::COLD);
    CHECK(sw::breathFor(false, 0.0f, false, false) == sw::Breath::NONE);
}
