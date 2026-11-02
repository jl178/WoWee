// The key cursor the character renderer samples bone tracks with.
//
// Searching from where the last frame's sample landed is only an optimisation
// if it answers what the binary search answers, bit for bit: a different key
// is a different pose. These sweep times forward, backward, past the ends,
// onto key times exactly, across duplicate timestamps and through NaN, with
// cursors left anywhere, and compare against the plain search and the plain
// samplers.
#include <catch_amalgamated.hpp>

#include "rendering/m2_track_sampler.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

using namespace wowee::rendering;
using wowee::pipeline::M2AnimationTrack;

namespace {

bool sameBits(const glm::vec3& a, const glm::vec3& b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

bool sameBits(const glm::quat& a, const glm::quat& b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

/// Times to try against timestamps: every key, either side of it, between
/// neighbours, before the start and past the end.
std::vector<float> probeTimes(const std::vector<uint32_t>& ts) {
    std::vector<float> out{-100.0f, -0.5f, 0.0f};
    for (uint32_t t : ts) {
        const float f = static_cast<float>(t);
        out.push_back(f);
        out.push_back(std::nextafter(f, -1e9f));
        out.push_back(std::nextafter(f, 1e9f));
        out.push_back(f + 0.5f);
    }
    out.push_back(static_cast<float>(ts.empty() ? 0 : ts.back()) + 1000.0f);
    return out;
}

}  // namespace

TEST_CASE("the cursor search lands on the key the binary search finds", "[m2track]") {
    const std::vector<std::vector<uint32_t>> tracks = {
        {},
        {0},
        {500},
        {0, 100},
        {0, 33, 66, 100, 133, 166, 200, 233, 266, 300, 1000, 1001, 5000},
        {10, 10, 10, 20, 20, 30},   // duplicate timestamps
        {0, 16777217u, 16777218u},  // past float precision: two keys convert alike
    };
    for (const auto& ts : tracks) {
        const auto times = probeTimes(ts);
        for (size_t count : {ts.size(), ts.size() / 2, ts.size() + 3}) {
            for (uint32_t start = 0; start < ts.size() + 3; ++start) {
                for (float t : times) {
                    uint32_t cursor = start;
                    INFO("keys " << ts.size() << " count " << count << " cursor " << start
                                 << " t " << t);
                    CHECK(m2_track::lowerKeyIndexFrom(ts, count, t, cursor) ==
                          m2_track::lowerKeyIndex(ts, count, t));
                }
            }
        }
    }
}

TEST_CASE("the cursor follows a clock forward and back", "[m2track]") {
    const std::vector<uint32_t> ts{0, 40, 80, 120, 160, 200, 240, 280, 320, 360, 400};
    uint32_t cursor = 0;
    // A frame at a time, including a loop wrap and a jump backwards.
    std::vector<float> clock;
    for (float t = 0.0f; t < 420.0f; t += 16.6f) clock.push_back(t);
    for (float t = 3.0f; t < 200.0f; t += 7.3f) clock.push_back(t);
    clock.push_back(390.0f);
    clock.push_back(5.0f);
    for (float t : clock) {
        INFO("t " << t);
        const size_t want = m2_track::lowerKeyIndex(ts, ts.size(), t);
        CHECK(m2_track::lowerKeyIndexFrom(ts, ts.size(), t, cursor) == want);
        CHECK(cursor == want);  // left on the answer for the next frame
    }
}

TEST_CASE("a NaN time finds the key the binary search finds", "[m2track]") {
    // upper_bound with a NaN key compares false against everything and runs to
    // the end; the cursor checks are written with the same predicate.
    const std::vector<uint32_t> ts{0, 10, 20, 30};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (uint32_t start = 0; start < 6; ++start) {
        uint32_t cursor = start;
        CHECK(m2_track::lowerKeyIndexFrom(ts, ts.size(), nan, cursor) ==
              m2_track::lowerKeyIndex(ts, ts.size(), nan));
    }
}

TEST_CASE("sampling through a cursor gives the same bits", "[m2track]") {
    // Two sequences, one track pair each, plus a global-sequence track whose
    // clock wraps on its own.
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> val(-2.0f, 2.0f);

    auto makeTrack = [&](int16_t globalSeq, uint16_t interp) {
        M2AnimationTrack track;
        track.interpolationType = interp;
        track.globalSequence = globalSeq;
        for (int s = 0; s < 2; ++s) {
            M2AnimationTrack::SequenceKeys keys;
            const int n = 3 + s * 9;
            for (int k = 0; k < n; ++k) {
                keys.timestamps.push_back(static_cast<uint32_t>(k * 37 + (k % 3)));
                keys.vec3Values.emplace_back(val(rng), val(rng), val(rng));
                keys.quatValues.emplace_back(val(rng), val(rng), val(rng), val(rng));
            }
            track.sequences.push_back(std::move(keys));
        }
        return track;
    };
    const std::vector<uint32_t> globalDurations{250};
    const M2AnimationTrack tracks[] = {makeTrack(-1, 1), makeTrack(-1, 0), makeTrack(0, 1)};

    for (const auto& track : tracks) {
        for (int seq : {0, 1, 5, -1}) {
            uint32_t vecCursor = 0;
            uint32_t quatCursor = 0;
            for (float t = -10.0f; t < 600.0f; t += 3.7f) {
                const float global = t * 1.9f;
                INFO("seq " << seq << " t " << t << " global seq " << track.globalSequence);
                const glm::vec3 v0 = m2_track::sampleVec3(track, seq, t, global, globalDurations,
                                                          glm::vec3(0.0f));
                const glm::vec3 v1 = m2_track::sampleVec3(track, seq, t, global, globalDurations,
                                                          glm::vec3(0.0f), &vecCursor);
                CHECK(sameBits(v0, v1));
                const glm::quat q0 = m2_track::sampleQuat(track, seq, t, global, globalDurations);
                const glm::quat q1 = m2_track::sampleQuat(track, seq, t, global, globalDurations,
                                                          &quatCursor);
                CHECK(sameBits(q0, q1));
            }
        }
    }
}
