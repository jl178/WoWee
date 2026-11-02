// How the stock 3.3.5a client plays a sound: the numbers and rules read out
// of Wow.exe (docs/re/sound-engine.md), checked on their own.
#include <catch_amalgamated.hpp>
#include "audio/stock_sound_rules.hpp"

#include <map>
#include <set>
#include <vector>

namespace stock = wowee::audio::stock;
using stock::Category;
using stock::Voice;
using stock::VoiceDecision;

TEST_CASE("Rolloff: full inside MinDistance, silent at the cutoff", "[sound_rules]") {
    // The commonest row: MinDistance 8, DistanceCutoff 45.
    REQUIRE(stock::rolloffGain(0.0f, 8.0f, 45.0f) == 1.0f);
    REQUIRE(stock::rolloffGain(8.0f, 8.0f, 45.0f) == 1.0f);
    REQUIRE(stock::rolloffGain(45.0f, 8.0f, 45.0f) == 0.0f);
    REQUIRE(stock::rolloffGain(60.0f, 8.0f, 45.0f) == 0.0f);
}

TEST_CASE("Rolloff: inverse at scale 4 beyond MinDistance", "[sound_rules]") {
    // min / (min + 4 (d - min)): twice MinDistance is a fifth.
    REQUIRE(stock::rolloffGain(16.0f, 8.0f, 45.0f) == Catch::Approx(8.0f / 40.0f));
    REQUIRE(stock::rolloffGain(30.0f, 8.0f, 45.0f) == Catch::Approx(8.0f / 96.0f));
    // A wide MinDistance keeps a sound near full far out.
    REQUIRE(stock::rolloffGain(100.0f, 100.0f, 200.0f) == 1.0f);
    REQUIRE(stock::rolloffGain(110.0f, 100.0f, 200.0f) == Catch::Approx(100.0f / 140.0f));
}

TEST_CASE("Rolloff: a linear fade over the last tenth of the cutoff", "[sound_rules]") {
    // From 0.9 * 45 = 40.5 the inverse gain is scaled down to 0 at 45.
    const float atStart = 8.0f / (8.0f + 4.0f * (40.5f - 8.0f));
    REQUIRE(stock::rolloffGain(40.5f, 8.0f, 45.0f) == Catch::Approx(atStart));
    const float mid = 8.0f / (8.0f + 4.0f * (42.75f - 8.0f)) * 0.5f;
    REQUIRE(stock::rolloffGain(42.75f, 8.0f, 45.0f) == Catch::Approx(mid));
    // A MinDistance past the fade start: the fade alone.
    REQUIRE(stock::rolloffGain(95.0f, 100.0f, 100.0f) == 1.0f);
    // MinDistance 95 inside the fade (90..100): at 99, the inverse part
    // 95 / (95 + 16) times what is left of the fade, 1 - 9/10.
    REQUIRE(stock::rolloffGain(99.0f, 95.0f, 100.0f) == Catch::Approx(95.0f / 111.0f * 0.1f));
    // Monotone all the way down.
    float last = 2.0f;
    for (float d = 0.0f; d <= 46.0f; d += 0.25f) {
        const float g = stock::rolloffGain(d, 8.0f, 45.0f);
        REQUIRE(g <= last);
        last = g;
    }
}

TEST_CASE("Listener: at the character, back along the view and up", "[sound_rules]") {
    const glm::vec3 feet(100.0f, 200.0f, 50.0f);
    const glm::vec3 fwd(1.0f, 0.0f, 0.0f);
    const glm::vec3 l = stock::listenerPosition(feet, fwd);
    REQUIRE(l.x == Catch::Approx(98.0f));
    REQUIRE(l.y == Catch::Approx(200.0f));
    REQUIRE(l.z == Catch::Approx(54.0f));
    // Looking down at the character from above moves the listener up too.
    const glm::vec3 down = glm::vec3(0.6f, 0.0f, -0.8f);
    REQUIRE(stock::listenerPosition(feet, down).z == Catch::Approx(50.0f + 1.6f + 4.0f));
}

TEST_CASE("Day runs 05:30 to 21:00", "[sound_rules]") {
    REQUIRE_FALSE(stock::isDaytime(5.49f));
    REQUIRE(stock::isDaytime(5.5f));
    REQUIRE(stock::isDaytime(12.0f));
    REQUIRE(stock::isDaytime(20.99f));
    REQUIRE_FALSE(stock::isDaytime(21.0f));
    REQUIRE_FALSE(stock::isDaytime(0.0f));
}

TEST_CASE("Kit bag: every file once before any repeats, never twice running", "[sound_rules]") {
    std::mt19937 rng(3);
    stock::KitBag bag;
    const std::vector<uint32_t> freq(5, 1);
    int last = -1;
    for (int round = 0; round < 40; ++round) {
        std::set<int> seen;
        for (int i = 0; i < 5; ++i) {
            const int f = stock::pickKitFile(bag, freq, rng);
            REQUIRE(f >= 0);
            REQUIRE(f < 5);
            REQUIRE(f != last);
            last = f;
            seen.insert(f);
        }
        // A whole bag is a permutation - except that a refill may not start
        // on the file that ended the last bag, which is held back one draw.
        REQUIRE(seen.size() >= 4);
    }
}

TEST_CASE("Kit bag: weights are copies in the bag", "[sound_rules]") {
    std::mt19937 rng(9);
    stock::KitBag bag;
    const std::vector<uint32_t> freq{3, 1};
    std::map<int, int> counts;
    int last = -1;
    for (int i = 0; i < 4000; ++i) {
        const int f = stock::pickKitFile(bag, freq, rng);
        REQUIRE(f >= 0);
        // Two files: no two the same in a row, whatever the weights.
        REQUIRE(f != last);
        last = f;
        ++counts[f];
    }
    // Strict alternation is all two files without repeats allow.
    REQUIRE(counts[0] == counts[1]);
}

TEST_CASE("Kit bag: one file always, no weight never", "[sound_rules]") {
    std::mt19937 rng(1);
    stock::KitBag one;
    for (int i = 0; i < 10; ++i) REQUIRE(stock::pickKitFile(one, {1}, rng) == 0);
    stock::KitBag none;
    REQUIRE(stock::pickKitFile(none, {0, 0, 0}, rng) == -1);
    stock::KitBag empty;
    REQUIRE(stock::pickKitFile(empty, {}, rng) == -1);
}

TEST_CASE("Kit flags: random level and pitch only when the row asks", "[sound_rules]") {
    std::mt19937 rng(5);
    REQUIRE(stock::kitVolume(0.8f, 0, 0.5f, rng) == Catch::Approx(0.4f));
    REQUIRE(stock::kitVolume(0.9f, 0, 2.0f, rng) == 1.0f);  // clamped
    REQUIRE(stock::kitPitch(0, rng) == 1.0f);
    for (int i = 0; i < 200; ++i) {
        const float v = stock::kitVolume(0.5f, stock::kFlagRandomVolume, 1.0f, rng);
        REQUIRE(v >= 0.35f - 1e-6f);
        REQUIRE(v <= 0.65f + 1e-6f);
        const float p = stock::kitPitch(stock::kFlagRandomPitch, rng);
        REQUIRE(p >= 0.85f);
        REQUIRE(p < 1.15f + 1e-6f);
    }
}

TEST_CASE("Voices: a category at its cap refuses the newcomer", "[sound_rules]") {
    std::vector<Voice> active(6, Voice{stock::kPriorityDefault, 0.5f, 0, Category::Footstep});
    const Voice step{stock::kPriorityDefault, 1.0f, 77, Category::Footstep};
    VoiceDecision d = stock::decideVoice(active, step, 32);
    REQUIRE_FALSE(d.admit);
    REQUIRE(d.reason == VoiceDecision::Reason::CategoryFull);
    active.pop_back();
    REQUIRE(stock::decideVoice(active, step, 32).admit);
    // Error speech: one line at a time.
    std::vector<Voice> speaking{Voice{stock::kPriorityDefault, 1.0f, 5, Category::ErrorSpeech}};
    REQUIRE_FALSE(stock::decideVoice(speaking, Voice{stock::kPriorityDefault, 1.0f, 6, Category::ErrorSpeech}, 32).admit);
    REQUIRE(stock::categoryCap(Category::Sfx) == 0);
    REQUIRE(stock::categoryCap(Category::CreatureExertion) == 1);
    REQUIRE(stock::categoryCap(Category::WeaponImpact) == 4);
    REQUIRE(stock::volumeGroup(Category::Footstep) == Category::Sfx);
    REQUIRE(stock::volumeGroup(Category::Ambience) == Category::Ambience);
}

TEST_CASE("Voices: a fourth copy of a kit steals the least of three", "[sound_rules]") {
    std::vector<Voice> active{
        {stock::kPriorityHeard, 0.9f, 42, Category::Sfx},
        {stock::kPriorityHeard, 0.2f, 42, Category::Sfx},   // quietest of the kit
        {stock::kPriorityHeard, 0.6f, 42, Category::Sfx},
        {stock::kPriorityHeard, 0.1f, 43, Category::Sfx},   // another kit: untouched
    };
    VoiceDecision d = stock::decideVoice(active, Voice{stock::kPriorityDefault, 0.5f, 42, Category::Sfx}, 32);
    REQUIRE(d.admit);
    REQUIRE(d.evict == std::vector<size_t>{1});
    // No-duplicate kits are refused while one plays.
    d = stock::decideVoice(active, Voice{stock::kPriorityDefault, 0.5f, 43, Category::Sfx}, 32, true);
    REQUIRE_FALSE(d.admit);
    REQUIRE(d.reason == VoiceDecision::Reason::Duplicate);
}

TEST_CASE("Voices: all real voices taken - outrank the weakest or go without", "[sound_rules]") {
    // 31 one-shots heard (127) and a quiet loop at 128.
    std::vector<Voice> active(31, Voice{stock::kPriorityHeard, 0.5f, 0, Category::Sfx});
    active.push_back(Voice{stock::kPriorityDefault, 0.05f, 0, Category::Sfx});
    // A new default sound outranks the quiet loop (same priority, louder).
    VoiceDecision d = stock::decideVoice(active, Voice{stock::kPriorityDefault, 0.5f, 0, Category::Sfx}, 32);
    REQUIRE(d.admit);
    REQUIRE(d.evict == std::vector<size_t>{31});
    // Quieter than everything at its priority: it is the one left out.
    d = stock::decideVoice(active, Voice{stock::kPriorityDefault, 0.01f, 0, Category::Sfx}, 32);
    REQUIRE_FALSE(d.admit);
    REQUIRE(d.reason == VoiceDecision::Reason::NoVoice);
    // Without the loop, a default newcomer cannot take a heard one-shot's voice.
    active.pop_back();
    active.push_back(Voice{stock::kPriorityHeard, 0.5f, 0, Category::Sfx});
    d = stock::decideVoice(active, Voice{stock::kPriorityDefault, 1.0f, 0, Category::Sfx}, 32);
    REQUIRE_FALSE(d.admit);
    // Zone music and ambience (100) and the player's own sounds (110) do.
    d = stock::decideVoice(active, Voice{stock::kPriorityOwnCharacter, 0.1f, 0, Category::Sfx}, 32);
    REQUIRE(d.admit);
    REQUIRE(d.evict.size() == 1);
    // Fewer voices than sounds: the count is what is free, not 32.
    REQUIRE_FALSE(stock::decideVoice(active, Voice{stock::kPriorityDefault, 1.0f, 0, Category::Sfx}, 30).admit);
    std::vector<Voice> few(3, Voice{stock::kPriorityHeard, 0.5f, 0, Category::Sfx});
    REQUIRE(stock::decideVoice(few, Voice{}, 4).admit);
}
