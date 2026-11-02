// SoundEntriesAdvanced as Wow.exe 3.3.5a applies it (SoundInterface2
// AdvancedKitProperties.cpp, docs/re/sound-engine.md section 2), and the
// unit sound odds and categories (0x747310, 0x7631a0).
#include <catch_amalgamated.hpp>
#include "audio/stock_sound_advanced.hpp"

#include <map>

namespace stock = wowee::audio::stock;
using stock::AdvancedKit;
using stock::AdvancedStep;
using stock::AdvancedUsage;
using stock::Category;
using stock::DuckGroup;
using stock::DuckMixer;

namespace {
// The boss and narrator shape: SFX to 0.3, music to 0.75, a second to duck,
// three to come back.
AdvancedKit bossLine() {
    AdvancedKit k;
    k.usage = AdvancedUsage::Once;
    k.duck[0] = 0.3f;
    k.duck[1] = 0.75f;
    k.duck[2] = 1.0f;
    k.timeToDuckMs = 1000;
    k.timeToUnduckMs = 3000;
    k.innerRadiusOfInfluence = 8.0f;
    k.outerRadiusOfInfluence = 45.0f;
    return stock::normaliseAdvancedKit(k);
}
}  // namespace

TEST_CASE("Advanced: the duck groups follow the volume callback", "[sound_advanced]") {
    REQUIRE(stock::duckGroup(Category::Sfx) == DuckGroup::Sfx);
    REQUIRE(stock::duckGroup(Category::Music) == DuckGroup::Music);
    REQUIRE(stock::duckGroup(Category::Ambience) == DuckGroup::Ambience);
    for (auto c : {Category::Cinematic, Category::ScriptSound, Category::ScriptMusic, Category::RacialCinematic})
        REQUIRE(stock::duckGroup(c) == DuckGroup::None);
    // Every capped category is SFX.
    for (int c = 7; c < static_cast<int>(Category::Count); ++c)
        REQUIRE(stock::duckGroup(static_cast<Category>(c)) == DuckGroup::Sfx);
}

TEST_CASE("Advanced: the row is normalised as the instance copies it", "[sound_advanced]") {
    AdvancedKit raw;
    // Waterfall turbulence (row 542): 4h, 3h, 2h, 1h - every later time is
    // the next day.
    raw.time[0] = 14400000;
    raw.time[1] = 10800000;
    raw.time[2] = 7200000;
    raw.time[3] = 3600000;
    raw.duck[0] = -1.0f;
    raw.duck[1] = 2.0f;
    raw.duck[2] = 0.5f;
    raw.innerRadiusOfInfluence = 60.0f;
    raw.outerRadiusOfInfluence = 50.0f;
    raw.insideAngle = 300.0f;
    raw.outsideAngle = 270.0f;
    raw.usage = AdvancedUsage::Interval;
    const AdvancedKit k = stock::normaliseAdvancedKit(raw);
    REQUIRE(k.time[0] == 14400000);
    REQUIRE(k.time[1] == 10800000 + stock::kDayMs);
    REQUIRE(k.time[2] == 7200000 + stock::kDayMs);
    REQUIRE(k.time[3] == 3600000 + stock::kDayMs);
    REQUIRE(k.duck[0] == 1.0f);
    REQUIRE(k.duck[1] == 1.0f);
    REQUIRE(k.duck[2] == 0.5f);
    REQUIRE(k.innerRadiusOfInfluence == 50.0f);
    REQUIRE(k.insideAngle == 270.0f);
    REQUIRE(k.usage == AdvancedUsage::Interval);
    // A caller forcing a loop makes it usage 0.
    REQUIRE(stock::normaliseAdvancedKit(raw, true).usage == AdvancedUsage::Loop);
}

TEST_CASE("Advanced: ducks only with a level strictly between 0 and 1", "[sound_advanced]") {
    AdvancedKit k;
    REQUIRE_FALSE(k.ducks());
    k.duck[1] = 0.0f;  // Dark Moon Test: music to 0 - which 0x4ca1e0 does not count
    REQUIRE_FALSE(k.ducks());
    k.duck[2] = 0.75f;
    REQUIRE(k.ducks());
}

TEST_CASE("Advanced: the 2D/3D blend by distance", "[sound_advanced]") {
    AdvancedKit k;
    // No outer radius: always full 3D.
    REQUIRE(stock::advancedPanLevel(k, 0.0f) == 1.0f);
    k.innerRadius2D = 8.0f;  // the commonest, 8 to 20
    k.outerRadius2D = 20.0f;
    REQUIRE(stock::advancedPanLevel(k, 0.0f) == 0.0f);
    REQUIRE(stock::advancedPanLevel(k, 8.0f) == 0.0f);
    REQUIRE(stock::advancedPanLevel(k, 14.0f) == Catch::Approx(0.5f));
    REQUIRE(stock::advancedPanLevel(k, 20.0f) == Catch::Approx(1.0f));
    REQUIRE(stock::advancedPanLevel(k, 21.0f) == 1.0f);
}

TEST_CASE("Advanced: distance ducking starts only for rows with a 2D radius", "[sound_advanced]") {
    AdvancedKit k = bossLine();
    // 0x4ca7b0 looks at the radius of influence inside the OuterRadius2D
    // branch alone.
    REQUIRE_FALSE(stock::advancedDuckStarts(k, 1.0f));
    k.outerRadius2D = 20.0f;
    REQUIRE(stock::advancedDuckStarts(k, 8.0f));
    REQUIRE_FALSE(stock::advancedDuckStarts(k, 8.5f));
}

TEST_CASE("Advanced: the time window and its volume", "[sound_advanced]") {
    AdvancedKit k;
    k.usage = AdvancedUsage::Loop;
    // 06:00 up, 08:00 full, 18:00 down, 20:00 silent.
    k.time[0] = 6 * 3600000;
    k.time[1] = 8 * 3600000;
    k.time[2] = 18 * 3600000;
    k.time[3] = 20 * 3600000;
    k = stock::normaliseAdvancedKit(k);
    REQUIRE_FALSE(stock::advancedInTimeWindow(k, 0, 5 * 3600000));
    REQUIRE(stock::advancedInTimeWindow(k, 0, 6 * 3600000));
    REQUIRE(stock::advancedInTimeWindow(k, 0, 20 * 3600000));
    REQUIRE_FALSE(stock::advancedInTimeWindow(k, 0, 20 * 3600000 + 1));
    REQUIRE(stock::advancedTimeVolume(k, 0, 5 * 3600000) == 0.0f);
    REQUIRE(stock::advancedTimeVolume(k, 0, 7 * 3600000) == Catch::Approx(0.5f));
    REQUIRE(stock::advancedTimeVolume(k, 0, 12 * 3600000) == 1.0f);
    REQUIRE(stock::advancedTimeVolume(k, 0, 19 * 3600000) == Catch::Approx(0.5f));
    REQUIRE(stock::advancedTimeVolume(k, 0, 21 * 3600000) == 0.0f);
    // The offset shifts where the window opens and closes; the ramps are
    // read from the unshifted times, as 0x4ca7b0 reads them.
    REQUIRE_FALSE(stock::advancedInTimeWindow(k, 3600000, 6 * 3600000 + 1800000));
    REQUIRE(stock::advancedTimeVolume(k, 3600000, 8 * 3600000) == Catch::Approx(1.0f));
    // A row with no window is always in it at full volume; a usage-2 row's
    // volume is never touched.
    AdvancedKit none;
    REQUIRE(stock::advancedInTimeWindow(none, 0, 123));
    REQUIRE(stock::advancedTimeVolume(none, 0, 123) == 1.0f);
    AdvancedKit once = k;
    once.usage = AdvancedUsage::Once;
    REQUIRE(stock::advancedTimeVolume(once, 0, 21 * 3600000) == 1.0f);
}

TEST_CASE("Advanced: a window past midnight", "[sound_advanced]") {
    AdvancedKit k;
    k.usage = AdvancedUsage::Loop;
    // 22:00 up, 23:00 full, 03:00 down, 04:00 off.
    k.time[0] = 22 * 3600000;
    k.time[1] = 23 * 3600000;
    k.time[2] = 3 * 3600000;
    k.time[3] = 4 * 3600000;
    k = stock::normaliseAdvancedKit(k);
    REQUIRE(stock::advancedInTimeWindow(k, 0, 1 * 3600000));
    REQUIRE(stock::advancedTimeVolume(k, 0, 1 * 3600000) == 1.0f);
    REQUIRE(stock::advancedTimeVolume(k, 0, 3 * 3600000 + 1800000) == Catch::Approx(0.5f));
    REQUIRE_FALSE(stock::advancedInTimeWindow(k, 0, 12 * 3600000));
    REQUIRE(stock::advancedTimeVolume(k, 0, 22 * 3600000 + 1800000) == Catch::Approx(0.5f));
}

TEST_CASE("Advanced: random offset and interval ranges", "[sound_advanced]") {
    std::mt19937 rng(7);
    AdvancedKit k;
    k.randomOffsetRange = 7000;
    k.intervalMinMs = 20000;
    k.intervalMaxMs = 60000;
    for (int i = 0; i < 2000; ++i) {
        const int32_t o = stock::advancedRandomOffset(k, rng);
        REQUIRE(o >= -7000);
        REQUIRE(o < 7000);
        const int32_t t = stock::advancedRandomInterval(k, rng);
        REQUIRE(t >= 20000);
        REQUIRE(t < 60000);
    }
    // max <= min answers max (0x4c9d90).
    k.intervalMinMs = 5000;
    k.intervalMaxMs = 5000;
    REQUIRE(stock::advancedRandomInterval(k, rng) == 5000);
    k.randomOffsetRange = 0;
    REQUIRE(stock::advancedRandomOffset(k, rng) == 0);
}

TEST_CASE("Advanced: a usage-1 instance waits an interval between one-shots", "[sound_advanced]") {
    std::mt19937 rng(1);
    AdvancedKit k;
    k.usage = AdvancedUsage::Interval;
    k.intervalMinMs = 3000;
    k.intervalMaxMs = 5000;
    stock::AdvancedSchedule s;
    s.countdownMs = 3000;
    // Counting down while nothing plays.
    REQUIRE(stock::advancedStep(k, s, false, true, 3, 2000, rng) == AdvancedStep::Nothing);
    REQUIRE(stock::advancedStep(k, s, false, true, 3, 999, rng) == AdvancedStep::Nothing);
    REQUIRE(stock::advancedStep(k, s, false, true, 3, 2, rng) == AdvancedStep::Start);
    REQUIRE(s.countdownMs >= 3000);
    // While it plays the countdown stands still.
    const int32_t left = s.countdownMs;
    REQUIRE(stock::advancedStep(k, s, true, true, 3, 10000, rng) == AdvancedStep::Nothing);
    REQUIRE(s.countdownMs == left);
    // Outside the window: nothing starts, and a sound playing is stopped.
    s.countdownMs = -1;
    REQUIRE(stock::advancedStep(k, s, false, false, 3, 100, rng) == AdvancedStep::Nothing);
    REQUIRE(stock::advancedStep(k, s, true, false, 3, 100, rng) == AdvancedStep::Stop);
}

TEST_CASE("Advanced: a usage-0 loop starts at once and changes file every interval", "[sound_advanced]") {
    std::mt19937 rng(2);
    AdvancedKit k;
    k.usage = AdvancedUsage::Loop;
    k.intervalMinMs = 20000;
    k.intervalMaxMs = 60000;
    stock::AdvancedSchedule s;
    s.countdownMs = 20000;
    REQUIRE(stock::advancedStep(k, s, false, true, 4, 16, rng) == AdvancedStep::Start);
    REQUIRE(stock::advancedStep(k, s, true, true, 4, 19999, rng) == AdvancedStep::Nothing);
    REQUIRE(stock::advancedStep(k, s, true, true, 4, 2, rng) == AdvancedStep::Restart);
    REQUIRE(s.countdownMs >= 20000);
    // One file: the loop just plays on.
    s.countdownMs = -5;
    REQUIRE(stock::advancedStep(k, s, true, true, 1, 100, rng) == AdvancedStep::Nothing);
    // The window closes: stopped, and the instance is over.
    REQUIRE(stock::advancedStep(k, s, true, false, 4, 16, rng) == AdvancedStep::Stop);
    REQUIRE(stock::advancedStep(k, s, false, true, 4, 16, rng) == AdvancedStep::Nothing);
    // Usage 2 is never scheduled here.
    AdvancedKit once;
    stock::AdvancedSchedule o;
    REQUIRE(stock::advancedStep(once, o, false, true, 2, 16, rng) == AdvancedStep::Nothing);
}

TEST_CASE("Ducking: down over TimeToDuck, back over TimeToUnduck", "[sound_advanced]") {
    DuckMixer m;
    const AdvancedKit k = bossLine();
    auto twoD = [](uint32_t) { return -1.0f; };
    m.duck(1, k);
    REQUIRE(m.size() == 1);
    REQUIRE(m.level(DuckGroup::Sfx) == 1.0f);
    m.update(500, twoD);
    REQUIRE(m.level(DuckGroup::Sfx) == Catch::Approx(0.5f));
    REQUIRE(m.level(DuckGroup::Music) == Catch::Approx(0.75f));  // reached and held
    REQUIRE(m.level(DuckGroup::Ambience) == 1.0f);
    m.update(500, twoD);
    REQUIRE(m.level(DuckGroup::Sfx) == Catch::Approx(0.3f));
    m.update(5000, twoD);
    REQUIRE(m.level(DuckGroup::Sfx) == Catch::Approx(0.3f));
    // The ducker's own sound is never pulled down by it.
    REQUIRE(m.level(DuckGroup::Sfx, 1) == 1.0f);
    REQUIRE(m.level(DuckGroup::None) == 1.0f);
    // The line ends: up by a third of the way per second.
    m.release(1);
    m.update(1500, twoD);
    REQUIRE(m.level(DuckGroup::Sfx) == Catch::Approx(0.8f));
    REQUIRE(m.level(DuckGroup::Music) == Catch::Approx(1.0f));
    m.update(1500, twoD);
    REQUIRE(m.level(DuckGroup::Sfx) == 1.0f);
    REQUIRE(m.empty());
}

TEST_CASE("Ducking: the lowest of all duckers wins; one ducker per instance", "[sound_advanced]") {
    DuckMixer m;
    AdvancedKit a = bossLine();
    AdvancedKit b = bossLine();
    b.duck[0] = 0.2f;
    b.timeToDuckMs = 0;  // at once
    auto twoD = [](uint32_t) { return -1.0f; };
    m.duck(1, a);
    m.duck(1, a);
    REQUIRE(m.size() == 1);
    m.duck(2, b);
    m.update(16, twoD);
    REQUIRE(m.level(DuckGroup::Sfx) == Catch::Approx(0.2f));
    // Instance 2 hears only instance 1's ducker.
    REQUIRE(m.level(DuckGroup::Sfx, 2) == Catch::Approx(1.0f - 16.0f / 1000.0f));
    // A row that pulls nothing down makes no ducker.
    AdvancedKit plain;
    m.duck(3, plain);
    REQUIRE(m.size() == 2);
}

TEST_CASE("Ducking: by distance, with the outer radius as hysteresis", "[sound_advanced]") {
    DuckMixer m;
    const AdvancedKit k = bossLine();  // inner 8, outer 45
    float dist = 5.0f;
    auto at = [&](uint32_t) { return dist; };
    m.duck(7, k);
    m.update(2000, at);
    REQUIRE(m.level(DuckGroup::Sfx) == Catch::Approx(0.3f));
    // Between the radii: still ducking, having started.
    dist = 30.0f;
    m.update(100, at);
    REQUIRE(m.level(DuckGroup::Sfx) == Catch::Approx(0.3f));
    // Out past the outer radius: it lets go and comes back up.
    dist = 50.0f;
    m.update(100, at);
    m.update(1500, at);
    REQUIRE(m.level(DuckGroup::Sfx) > 0.3f);
    m.update(3000, at);
    REQUIRE(m.empty());
}

TEST_CASE("Ducking: a positioned line with no radius lets go at once", "[sound_advanced]") {
    // Most boss lines name no radius of influence (0, 0): played at a unit
    // they duck nothing; played 2D (SMSG_PLAY_SOUND) they duck while they last.
    DuckMixer m;
    AdvancedKit k = bossLine();
    k.innerRadiusOfInfluence = 0.0f;
    k.outerRadiusOfInfluence = 0.0f;
    m.duck(1, k);
    m.update(16, [](uint32_t) { return 3.0f; });
    m.update(16, [](uint32_t) { return 3.0f; });
    REQUIRE(m.empty());
}

TEST_CASE("Unit sounds: the stock odds and categories", "[sound_advanced]") {
    using K = stock::UnitSoundKind;
    REQUIRE(stock::unitSoundChance(K::Exertion, false) == 70);
    REQUIRE(stock::unitSoundChance(K::Exertion, true) == 35);
    REQUIRE(stock::unitSoundChance(K::Injury, false) == 60);
    REQUIRE(stock::unitSoundChance(K::Injury, true) == 30);
    REQUIRE(stock::unitSoundChance(K::InjuryCritical, true) == 100);
    REQUIRE(stock::unitSoundChance(K::Stand, false) == 40);
    REQUIRE(stock::unitSoundChance(K::Stand, true) == 40);
    REQUIRE(stock::unitSoundCategory(K::Exertion, true) == Category::CreatureExertion);
    REQUIRE(stock::unitSoundCategory(K::Injury, false) == Category::CreatureInjury);
    REQUIRE(stock::unitSoundCategory(K::Injury, true) == Category::PlayerInjury);
    REQUIRE(stock::categoryCap(Category::PlayerInjury) == 1);
    REQUIRE(stock::kOwnCharacterVolume == Catch::Approx(0.65f));
}
