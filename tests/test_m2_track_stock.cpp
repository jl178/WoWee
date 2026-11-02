// M2 track evaluation as the stock 3.3.5a client does it (docs/re/m2-runtime.md).
//
// - Which key array a sample reads: the playing sequence's, array 0 when the
//   track has fewer arrays (key search 0x8284d0), array 0 for a global
//   sequence.
// - Interpolation: 0 holds the lower key, anything else is linear on plain
//   tracks (0x82af40/0x82b0a0/0x82b340); Hermite and Bezier exist only for
//   camera spline keys (0x82b460/0x82b8a0).
// - Rotation keys: lerp then normalise, with no turn to the shorter arc
//   (0x982630).
// - The ends: the last key holds after the last timestamp.
// - The eased weight two blended sequences get (0x82f0f0).
// - The sequence a model starts on (0x834540) and constant-track detection.
#include <catch_amalgamated.hpp>

#include "rendering/animation/sequence_variation.hpp"
#include "rendering/m2_track_sampler.hpp"

#include <glm/gtc/quaternion.hpp>

#include <vector>

using namespace wowee::rendering;
using wowee::pipeline::M2AnimationTrack;

namespace {

M2AnimationTrack::SequenceKeys floatKeys(std::vector<uint32_t> ts, std::vector<float> vs) {
    M2AnimationTrack::SequenceKeys k;
    k.timestamps = std::move(ts);
    k.floatValues = std::move(vs);
    return k;
}

}  // namespace

TEST_CASE("a sequence past the track's arrays reads array 0", "[m2][track][stock]") {
    M2AnimationTrack t;
    t.interpolationType = 1;
    t.sequences = {floatKeys({0, 100}, {0.0f, 1.0f})};
    // Sequence 7 on a one-array track: the stock key search falls back to
    // array 0 (a hidden texture weight stays hidden in every sequence).
    CHECK(m2_track::sampleFloat(t, 7, 50.0f, 0.0f, {}, -1.0f) == Catch::Approx(0.5f));
    // No sequence at all is the caller's default.
    CHECK(m2_track::sampleFloat(t, -1, 50.0f, 0.0f, {}, -1.0f) == -1.0f);
    // An empty array (an .anim sequence not loaded) is the default too.
    t.sequences.push_back(floatKeys({}, {}));
    CHECK(m2_track::sampleFloat(t, 1, 50.0f, 0.0f, {}, -1.0f) == -1.0f);
}

TEST_CASE("Hermite and Bezier plain tracks interpolate linearly", "[m2][track][stock]") {
    M2AnimationTrack t;
    t.sequences = {floatKeys({0, 100, 300}, {0.0f, 1.0f, 3.0f})};
    for (uint16_t type : {1, 2, 3}) {
        t.interpolationType = type;
        CHECK(m2_track::sampleFloat(t, 0, 25.0f, 0.0f, {}, -1.0f) == Catch::Approx(0.25f));
        CHECK(m2_track::sampleFloat(t, 0, 200.0f, 0.0f, {}, -1.0f) == Catch::Approx(2.0f));
    }
    t.interpolationType = 0;
    CHECK(m2_track::sampleFloat(t, 0, 99.0f, 0.0f, {}, -1.0f) == 0.0f);
    CHECK(m2_track::sampleFloat(t, 0, 100.0f, 0.0f, {}, -1.0f) == 1.0f);
}

TEST_CASE("the last key holds past the end", "[m2][track][stock]") {
    M2AnimationTrack t;
    t.interpolationType = 1;
    // Frost Armor's crest weight: in by 134 ms, out from 2167 to 3000 ms.
    t.sequences = {floatKeys({0, 134, 2167, 3000}, {0.0f, 1.0f, 1.0f, 0.0f})};
    CHECK(m2_track::sampleFloat(t, 0, 0.0f, 0.0f, {}, -1.0f) == 0.0f);
    CHECK(m2_track::sampleFloat(t, 0, 67.0f, 0.0f, {}, -1.0f) == Catch::Approx(0.5f));
    CHECK(m2_track::sampleFloat(t, 0, 134.0f, 0.0f, {}, -1.0f) == 1.0f);
    CHECK(m2_track::sampleFloat(t, 0, 2000.0f, 0.0f, {}, -1.0f) == 1.0f);
    CHECK(m2_track::sampleFloat(t, 0, 3000.0f, 0.0f, {}, -1.0f) == 0.0f);
    CHECK(m2_track::sampleFloat(t, 0, 5000.0f, 0.0f, {}, -1.0f) == 0.0f);
}

TEST_CASE("rotation keys lerp and normalise without a hemisphere flip", "[m2][track][stock]") {
    const glm::quat a(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::quat b = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::quat mid = m2_track::nlerpKeys(a, b, 0.5f);
    const glm::quat want = glm::normalize(glm::quat(a.w + b.w, a.x + b.x, a.y + b.y, a.z + b.z));
    CHECK(mid.w == Catch::Approx(want.w));
    CHECK(mid.z == Catch::Approx(want.z));
    CHECK(glm::length(mid) == Catch::Approx(1.0f));

    // Keys on opposite hemispheres: slerp would turn to -b and take the short
    // way; the stock lerp goes between the stored values as they are.
    const glm::quat c(-0.6f, 0.0f, 0.0f, 0.8f);
    const glm::quat q = m2_track::nlerpKeys(a, c, 0.25f);
    const glm::quat raw(1.0f + (-1.6f) * 0.25f, 0.0f, 0.0f, 0.8f * 0.25f);
    const glm::quat n = glm::normalize(raw);
    CHECK(q.w == Catch::Approx(n.w));
    CHECK(q.z == Catch::Approx(n.z));

    M2AnimationTrack t;
    t.interpolationType = 1;
    M2AnimationTrack::SequenceKeys k;
    k.timestamps = {0, 100};
    k.quatValues = {a, b};
    t.sequences = {k};
    const glm::quat s = m2_track::sampleQuat(t, 0, 50.0f, 0.0f, {});
    CHECK(s.w == Catch::Approx(want.w));
    CHECK(s.z == Catch::Approx(want.z));
}

TEST_CASE("camera spline segments: Bezier and Hermite as the client has them", "[m2][track][stock]") {
    const float p0 = 1.0f, out0 = 2.0f, in1 = 5.0f, p1 = 4.0f;
    for (int type : {1, 2, 3}) {
        CHECK(m2_track::splineSegment(type, p0, out0, in1, p1, 0.0f) == Catch::Approx(p0));
        CHECK(m2_track::splineSegment(type, p0, out0, in1, p1, 1.0f) == Catch::Approx(p1));
    }
    // Bezier (type 2) with control points out0 and in1: at 0.5 it is
    // (p0 + 3 out0 + 3 in1 + p1) / 8.
    CHECK(m2_track::splineSegment(2, p0, out0, in1, p1, 0.5f) ==
          Catch::Approx((p0 + 3.0f * out0 + 3.0f * in1 + p1) / 8.0f));
    // Hermite (type 3) with tangents out0 and in1: at 0.5 it is
    // (p0 + p1) / 2 + (out0 - in1) / 8.
    CHECK(m2_track::splineSegment(3, p0, out0, in1, p1, 0.5f) ==
          Catch::Approx((p0 + p1) / 2.0f + (out0 - in1) / 8.0f));
    CHECK(m2_track::splineSegment(1, p0, out0, in1, p1, 0.25f) == Catch::Approx(1.75f));
    CHECK(m2_track::splineSegment(0, p0, out0, in1, p1, 0.75f) == p0);
    const glm::vec3 v = m2_track::splineSegment(2, glm::vec3(0.0f), glm::vec3(1.0f),
                                                glm::vec3(1.0f), glm::vec3(0.0f), 0.5f);
    CHECK(v.x == Catch::Approx(0.75f));
}

TEST_CASE("sequence blending is eased", "[m2][track][stock]") {
    CHECK(m2_track::sequenceBlendWeight(-1.0f) == 0.0f);
    CHECK(m2_track::sequenceBlendWeight(0.0f) == 0.0f);
    CHECK(m2_track::sequenceBlendWeight(0.25f) == Catch::Approx(0.15625f));
    CHECK(m2_track::sequenceBlendWeight(0.5f) == Catch::Approx(0.5f));
    CHECK(m2_track::sequenceBlendWeight(1.0f) == 1.0f);
    CHECK(m2_track::sequenceBlendWeight(2.0f) == 1.0f);
}

TEST_CASE("constant alpha tracks are told apart from playing ones", "[m2][track][stock]") {
    M2AnimationTrack none;
    CHECK_FALSE(m2_track::trackVaries(none, 1.0f));
    CHECK(m2_track::trackConstant(none, 1.0f) == 1.0f);

    M2AnimationTrack zero;
    zero.sequences = {floatKeys({0}, {0.0f})};
    CHECK_FALSE(m2_track::trackVaries(zero, 1.0f));
    CHECK(m2_track::trackConstant(zero, 1.0f) == 0.0f);  // hidden, as stock hides it

    M2AnimationTrack fade;
    fade.sequences = {floatKeys({0, 100}, {0.0f, 1.0f})};
    CHECK(m2_track::trackVaries(fade, 1.0f));

    M2AnimationTrack perSeq;
    perSeq.sequences = {floatKeys({0}, {1.0f}), floatKeys({0}, {0.0f})};
    CHECK(m2_track::trackVaries(perSeq, 1.0f));

    M2AnimationTrack gap;  // a keyed sequence beside an empty one (default 1)
    gap.sequences = {floatKeys({0}, {0.5f}), floatKeys({}, {})};
    CHECK(m2_track::trackVaries(gap, 1.0f));
}

TEST_CASE("a model starts on Stand, or on its first sequence", "[m2][sequence][stock]") {
    using wowee::pipeline::M2Sequence;
    std::vector<M2Sequence> seqs(3);
    seqs[0].id = 5;
    seqs[1].id = 0;
    seqs[1].frequency = 0x7FFF;
    seqs[1].flags = anim::kSeqFlagEmbedded;
    seqs[2].id = 4;
    seqs[0].flags = seqs[2].flags = anim::kSeqFlagEmbedded;
    CHECK(anim::defaultSequenceIndex(seqs, {}) == 1);

    seqs[1].id = 9;
    CHECK(anim::defaultSequenceIndex(seqs, {}) == 0);

    CHECK(anim::defaultSequenceIndex({}, {}) == 0);
}
