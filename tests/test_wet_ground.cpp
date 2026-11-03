// How wet the ground gets in rain, and which presets ask for the two water
// enhancements.
//
// The wetness is eased rather than read off the weather: the ground should
// soak over ten-odd seconds once rain starts and dry over a little longer once
// it stops, never jump, and come back to exactly 0 so the shaders' dry path is
// the stock picture again. The presets are pinned because "on from Medium up,
// off on Low and in Classic" is a promise the table could quietly stop
// keeping.
#include <catch_amalgamated.hpp>

#include "rendering/wet_ground.hpp"
#include "ui/graphics_presets.hpp"

using wowee::rendering::easeWetGround;
using wowee::rendering::wetGroundTarget;

namespace {

/// Seconds of steady weather until the wetness passes `level`, at 60 fps.
float secondsToReach(float from, float target, float level) {
    constexpr float kStep = 1.0f / 60.0f;
    float w = from;
    float t = 0.0f;
    while (t < 120.0f) {
        if (target > from ? w >= level : w <= level) return t;
        w = easeWetGround(w, target, kStep);
        t += kStep;
    }
    return t;
}

}  // namespace

TEST_CASE("only falling water wets the ground", "[wetground]") {
    CHECK(wetGroundTarget(false, false, 1.0f) == 0.0f);  // fine weather, or snow
    CHECK(wetGroundTarget(true, false, 0.0f) == 0.0f);   // rain at nothing is no rain
    CHECK(wetGroundTarget(true, false, 0.2f) > 0.5f);    // a drizzle still soaks it
    CHECK(wetGroundTarget(true, false, 1.0f) == Catch::Approx(1.0f));
    CHECK(wetGroundTarget(false, true, 0.5f) > wetGroundTarget(true, false, 0.5f));
    // Out-of-range intensities from the server are held to the range.
    CHECK(wetGroundTarget(true, false, 5.0f) == Catch::Approx(1.0f));
    CHECK(wetGroundTarget(true, false, -1.0f) == 0.0f);
}

TEST_CASE("the ground soaks and dries over ten to twenty seconds", "[wetground]") {
    const float soak = secondsToReach(0.0f, 1.0f, 0.95f);
    const float dry = secondsToReach(1.0f, 0.0f, 0.05f);
    INFO("soaks in " << soak << " s, dries in " << dry << " s");
    CHECK(soak >= 10.0f);
    CHECK(soak <= 20.0f);
    CHECK(dry >= 10.0f);
    CHECK(dry <= 20.0f);
    CHECK(dry > soak);  // drying is the slower of the two
}

TEST_CASE("the ease never jumps, overshoots or lingers", "[wetground]") {
    SECTION("a long frame lands on the target, not past it") {
        CHECK(easeWetGround(0.0f, 0.8f, 1000.0f) == Catch::Approx(0.8f));
        CHECK(easeWetGround(0.8f, 0.0f, 1000.0f) == 0.0f);
    }
    SECTION("no time, no change; negative time is no time") {
        CHECK(easeWetGround(0.4f, 1.0f, 0.0f) == 0.4f);
        CHECK(easeWetGround(0.4f, 1.0f, -1.0f) == 0.4f);
    }
    SECTION("one frame moves it a little") {
        const float next = easeWetGround(0.0f, 1.0f, 1.0f / 60.0f);
        CHECK(next > 0.0f);
        CHECK(next < 0.01f);
    }
    SECTION("drying ends at exactly zero") {
        float w = 1.0f;
        for (int i = 0; i < 60 * 60; ++i) w = easeWetGround(w, 0.0f, 1.0f / 60.0f);
        CHECK(w == 0.0f);
    }
}

TEST_CASE("shoreline foam and wet ground are on from Medium up", "[wetground][settings]") {
    using wowee::ui::kClassicPresetIndex;
    using wowee::ui::kGraphicsPresets;
    // Low, Medium, High, Ultra, Classic.
    CHECK_FALSE(kGraphicsPresets[0].enh.shorelineFoam);
    CHECK_FALSE(kGraphicsPresets[0].enh.wetGround);
    for (int i = 1; i < kClassicPresetIndex; ++i) {
        INFO("preset " << i);
        CHECK(kGraphicsPresets[i].enh.shorelineFoam);
        CHECK(kGraphicsPresets[i].enh.wetGround);
    }
    // Classic is the stock picture: neither.
    CHECK_FALSE(kGraphicsPresets[kClassicPresetIndex].enh.shorelineFoam);
    CHECK_FALSE(kGraphicsPresets[kClassicPresetIndex].enh.wetGround);
}
