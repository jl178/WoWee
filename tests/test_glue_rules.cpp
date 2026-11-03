// The stock glue screens' rules (game/glue_rules.hpp), checked against the
// numbers read out of GlueXML and Wow.exe 3.3.5a (docs/re/glue-screens.md).
#include <catch_amalgamated.hpp>

#include "game/glue_rules.hpp"

#include <cmath>
#include <set>

using namespace wowee::game::glue;

TEST_CASE("glue scene keys follow SetBackgroundModel", "[glue]") {
    CHECK(sceneKey("UI_NightElf") == "NIGHTELF");
    CHECK(sceneKey("UI_DeathKnight") == "DEATHKNIGHT");
    CHECK(sceneKey("ui_scourge") == "SCOURGE");
    CHECK(sceneKey("Human") == "HUMAN");
}

TEST_CASE("RaceLights gives the orc scene its two suns", "[glue]") {
    // The orc M2 carries only an ambient and a point light; the stock client
    // lights it from GlueParent.lua's table instead, which adds a cool and a
    // warm directional light. Trolls share the scene and the light.
    const auto& orc = raceLights("ORC");
    REQUIRE(orc.size() == 3);
    CHECK(orc[0].ambIntensity * orc[0].ambColor.r == Catch::Approx(0.15f));
    CHECK(orc[2].dirIntensity == Catch::Approx(2.0f));
    CHECK(orc[2].dirColor.r == Catch::Approx(0.55f));
    CHECK(raceLights("characterselect").size() == 3);
    CHECK(raceLights("MAINMENU_NORTHREND").empty());
}

TEST_CASE("glue ambience by scene", "[glue]") {
    CHECK(ambienceForScene("HUMAN") == "GlueScreenHuman");
    CHECK(ambienceForScene("orc") == "GlueScreenOrcTroll");
    CHECK(ambienceForScene("DEATHKNIGHT") == "GlueScreenIntro");
    CHECK(ambienceForScene("DARKPORTAL") == "GlueScreenIntro");
    CHECK(ambienceForScene("GNOME").empty());
}

TEST_CASE("ModelFFX field of view from the diagonal", "[glue]") {
    const float diag = 1.3962634f;  // UI_Human's camera, 80 degrees
    // 4:3: over 5/3.
    CHECK(verticalFovFromDiagonal(diag, 4.0f / 3.0f) == Catch::Approx(diag * 0.6f));
    // 16:9 and anything wider: GlueParent pillarboxes to 16:9.
    const float at169 = diag / std::sqrt(1.0f + (16.0f / 9.0f) * (16.0f / 9.0f));
    CHECK(verticalFovFromDiagonal(diag, 16.0f / 9.0f) == Catch::Approx(at169));
    CHECK(verticalFovFromDiagonal(diag, 21.0f / 9.0f) == Catch::Approx(at169));
}

TEST_CASE("wide loading art past 4:3", "[glue][loading]") {
    CHECK_FALSE(useWideLoadingArt(4.0f / 3.0f));
    CHECK_FALSE(useWideLoadingArt(1.25f));
    CHECK(useWideLoadingArt(1.4f));
    CHECK(useWideLoadingArt(16.0f / 10.0f));
}

TEST_CASE("loading art keeps its aspect", "[glue][loading]") {
    // 4:3 art on a 16:9 window: pillarboxed.
    Rect r = loadingImageRect(1920.0f, 1080.0f, false);
    CHECK(r.width() == Catch::Approx(1440.0f));
    CHECK(r.x0 == Catch::Approx(240.0f));
    CHECK(r.height() == Catch::Approx(1080.0f));
    // The 1.6 wide art on 16:9: pillarboxed a little.
    r = loadingImageRect(1920.0f, 1080.0f, true);
    CHECK(r.width() == Catch::Approx(1728.0f));
    // The wide art on 4:3 would be letterboxed (it is not chosen there).
    r = loadingImageRect(1024.0f, 768.0f, true);
    CHECK(r.width() == Catch::Approx(1024.0f));
    CHECK(r.height() == Catch::Approx(640.0f));
    CHECK(r.y0 == Catch::Approx(64.0f));
}

TEST_CASE("loading bar sits in the art", "[glue][loading]") {
    const Rect image{0.0f, 0.0f, 1024.0f, 768.0f};
    LoadingBar bar = loadingBarRects(image, 0.0f);
    CHECK(bar.border.width() == Catch::Approx(614.4f));
    CHECK(bar.border.height() == Catch::Approx(38.4f));
    CHECK((bar.border.y0 + bar.border.y1) * 0.5f == Catch::Approx(768.0f - 57.6f));
    CHECK(bar.fill.width() == Catch::Approx(0.0f));
    bar = loadingBarRects(image, 0.5f);
    CHECK(bar.fill.x0 == Catch::Approx(512.0f - 268.8f));
    CHECK(bar.fill.width() == Catch::Approx(268.8f));
    CHECK(bar.fill.height() == Catch::Approx(19.2f));
    CHECK(advanceLoadingProgress(0.6f, 0.3f) == Catch::Approx(0.6f));
    CHECK(advanceLoadingProgress(0.6f, 0.7f) == Catch::Approx(0.7f));
    CHECK(advanceLoadingProgress(0.6f, 1.7f) == Catch::Approx(1.0f));
}

TEST_CASE("random names come from the race's NameGen chain", "[glue][names]") {
    NameChain chain;
    chain.build({"Aldric", "Aldwin", "Brenna", "Brandt", "Corwin", "Doran", "Edric"});
    REQUIRE_FALSE(chain.empty());
    std::mt19937 rng(12345);
    std::set<std::string> seen;
    for (int i = 0; i < 200; ++i) {
        const std::string n = chain.generate(rng, plausibleCharacterName);
        REQUIRE_FALSE(n.empty());
        CHECK(n.size() >= 4);
        CHECK(n.size() <= 10);
        CHECK(std::isupper(static_cast<unsigned char>(n[0])));
        seen.insert(n);
    }
    // A chain, not a pick from the list: it makes names the list lacks.
    bool novel = false;
    for (const auto& n : seen) {
        if (n != "Aldric" && n != "Aldwin" && n != "Brenna" && n != "Brandt" && n != "Corwin" &&
            n != "Doran" && n != "Edric") {
            novel = true;
        }
    }
    CHECK(novel);
}

TEST_CASE("the name check the generator's names pass", "[glue][names]") {
    CHECK(plausibleCharacterName("Thrall"));
    CHECK_FALSE(plausibleCharacterName("Thrrrall"));
    CHECK_FALSE(plausibleCharacterName("Th'rall"));
    CHECK_FALSE(plausibleCharacterName(""));
}
