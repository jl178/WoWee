// Feet on the ground (E9), rain and snow in the wind (E19), smooth zoom
// (E20) and the lit horizon (E11): docs/enhancements-plan.md. The leg rule
// and the IK's limits - reach, knee side, the body's drop, the feet's tilt,
// the fade with speed - and that zero targets leave the pose bit for bit; the
// zoom's ease never overshoots and is the same at any frame rate; the wind's
// push is nothing at zero strength; the horizon's shading is exactly the fog
// colour on level ground and at the near limit.
#include <catch_amalgamated.hpp>

#include "rendering/camera_zoom.hpp"
#include "rendering/foot_ik.hpp"
#include "rendering/horizon_detail.hpp"
#include "rendering/weather_wind.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstring>
#include <vector>

namespace fk = wowee::rendering::foot_ik;
namespace cz = wowee::rendering::camera_zoom;
namespace ww = wowee::rendering::weather_wind;
namespace hd = wowee::rendering::horizon_detail;
namespace st = wowee::rendering::stock_terrain;

namespace {

fk::BoneInfo bone(int key, int parent, glm::vec3 pivot, float weight = 1.0f) {
    fk::BoneInfo b;
    b.keyBoneId = key;
    b.parent = parent;
    b.pivot = pivot;
    b.weight = weight;
    return b;
}

// HumanMale's skeleton below the spine, with its pivots (tools survey,
// docs/enhancements-plan.md E9).
std::vector<fk::BoneInfo> humanLegs() {
    std::vector<fk::BoneInfo> b;
    b.push_back(bone(-1, -1, {0, 0, 0}, 0));             // 0 root
    b.push_back(bone(26, 0, {0, 0, 1.22f}));             // 1 key Root
    b.push_back(bone(4, 1, {0, 0, 1.21f}));              // 2 SpineLow
    b.push_back(bone(5, 1, {0.02f, 0, 1.16f}));          // 3 Waist
    b.push_back(bone(-1, 3, {-0.15f, 0, 0.94f}));        // 4 skirt back
    b.push_back(bone(-1, 3, {0.16f, 0, 1.06f}));         // 5 skirt front
    b.push_back(bone(-1, 3, {0.03f, 0.13f, 1.08f}));     // 6 hip L
    b.push_back(bone(-1, 3, {0.03f, -0.13f, 1.08f}));    // 7 hip R
    b.push_back(bone(-1, 6, {-0.01f, 0.17f, 0.58f}));    // 8 knee L
    b.push_back(bone(-1, 7, {-0.01f, -0.17f, 0.58f}));   // 9 knee R
    b.push_back(bone(-1, 8, {-0.09f, 0.2f, 0.1f}));      // 10 ankle L
    b.push_back(bone(-1, 9, {-0.09f, -0.2f, 0.1f}));     // 11 ankle R
    b.push_back(bone(-1, 10, {0.13f, 0.21f, 0.02f}));    // 12 toe L
    b.push_back(bone(-1, 11, {0.13f, -0.2f, 0.02f}));    // 13 toe R
    return b;
}

// The rest pose as bone matrices: identity (pivot brackets at rest).
std::vector<glm::mat4> restPose(size_t n) { return std::vector<glm::mat4>(n, glm::mat4(1.0f)); }

glm::vec3 at(const std::vector<glm::mat4>& m, const std::vector<fk::BoneInfo>& b, int i) {
    return glm::vec3(m[i] * glm::vec4(b[i].pivot, 1.0f));
}

}  // namespace

// ---------------------------------------------------------------- E9 rig

TEST_CASE("E9: the legs are the two mirrored chains below the Waist", "[foot_ik]") {
    const auto b = humanLegs();
    const fk::Rig rig = fk::findLegs(b);
    REQUIRE(rig.valid);
    CHECK(rig.legs[0].hip == 6);   // left (+Y)
    CHECK(rig.legs[0].knee == 8);
    CHECK(rig.legs[0].end == 10);
    CHECK(rig.legs[1].hip == 7);
    CHECK(rig.legs[1].end == 11);
    // The toe follows the end; the skirt and the spine follow nothing.
    CHECK((rig.legs[0].member[12] & 4) != 0);
    CHECK(rig.legs[0].member[4] == 0);
    CHECK(rig.legs[0].member[2] == 0);
    CHECK(rig.legs[1].member[10] == 0);
    CHECK(rig.legLength == Catch::Approx(0.5f + 0.49f).margin(0.03f));
}

TEST_CASE("E9: no Waist, one leg, or four legs is no rig", "[foot_ik]") {
    auto b = humanLegs();
    auto noWaist = b;
    noWaist[3].keyBoneId = -1;
    CHECK_FALSE(fk::findLegs(noWaist).valid);

    auto oneLeg = b;
    for (int i : {7, 9, 11, 13}) oneLeg[i].weight = 0.0f;
    CHECK_FALSE(fk::findLegs(oneLeg).valid);

    auto four = b;
    four.push_back(bone(-1, 3, {-0.4f, 0.13f, 1.0f}));   // 14 hind hip L
    four.push_back(bone(-1, 14, {-0.4f, 0.17f, 0.5f}));  // 15
    four.push_back(bone(-1, 15, {-0.4f, 0.2f, 0.08f}));  // 16
    CHECK_FALSE(fk::findLegs(four).valid);
}

TEST_CASE("E9: a hoofed leg turns its lower part as one piece", "[foot_ik]") {
    // Draenei female's left leg: hip, knee, hock, hoof.
    std::vector<fk::BoneInfo> b;
    b.push_back(bone(-1, -1, {0, 0, 0}, 0));
    b.push_back(bone(26, 0, {0.07f, 0, 1.39f}));
    b.push_back(bone(5, 1, {0.07f, 0, 1.37f}));
    b.push_back(bone(-1, 2, {0.05f, 0.1f, 1.34f}));      // 3 hip L
    b.push_back(bone(-1, 2, {0.05f, -0.1f, 1.34f}));     // 4 hip R
    b.push_back(bone(-1, 3, {0.12f, 0.15f, 0.76f}));     // 5 knee
    b.push_back(bone(-1, 4, {0.12f, -0.15f, 0.76f}));
    b.push_back(bone(-1, 5, {-0.06f, 0.18f, 0.48f}));    // 7 hock
    b.push_back(bone(-1, 6, {-0.06f, -0.18f, 0.48f}));
    b.push_back(bone(-1, 7, {0.06f, 0.22f, 0.06f}));     // 9 hoof
    b.push_back(bone(-1, 8, {0.06f, -0.22f, 0.06f}));
    const fk::Rig rig = fk::findLegs(b);
    REQUIRE(rig.valid);
    CHECK(rig.legs[0].knee == 5);
    CHECK(rig.legs[0].end == 9);
    CHECK((rig.legs[0].member[7] & 2) != 0);  // the hock turns with the knee's part
}

// ---------------------------------------------------------------- E9 solve

TEST_CASE("E9: two-bone IK keeps the bones' lengths and the knee's side", "[foot_ik]") {
    const glm::vec3 h(0, 0.13f, 1.08f), k(0.02f, 0.17f, 0.58f), e(-0.09f, 0.2f, 0.1f);
    const float l1 = glm::length(k - h), l2 = glm::length(e - k);
    for (float dz : {-0.15f, -0.05f, 0.0f, 0.1f, 0.2f, 0.3f}) {
        const glm::vec3 target = e + glm::vec3(0, 0, dz);
        const fk::TwoBone s = fk::solveTwoBone(h, k, e, target, {1, 0, 0});
        CHECK(glm::length(s.knee - h) == Catch::Approx(l1).margin(1e-4));
        CHECK(glm::length(s.end - s.knee) == Catch::Approx(l2).margin(1e-4));
        // The rotations take the bones where the solve says.
        CHECK(glm::length(h + s.upper * (k - h) - s.knee) < 1e-4f);
        const glm::vec3 eUp = h + s.upper * (e - h);
        CHECK(glm::length(s.knee + s.lower * (eUp - s.knee) - s.end) < 1e-3f);
        // The knee still bends forward (+X), as it did.
        const glm::vec3 dir = glm::normalize(s.end - h);
        const glm::vec3 off = (s.knee - h) - dir * glm::dot(s.knee - h, dir);
        CHECK(off.x > 0.0f);
        if (glm::length(target - h) < 0.999f * (l1 + l2)) {
            CHECK(glm::length(s.end - target) < 1e-4f);
        }
    }
}

TEST_CASE("E9: an out of reach target never locks the knee straight", "[foot_ik]") {
    const glm::vec3 h(0, 0, 1), k(0.05f, 0, 0.5f), e(0, 0, 0);
    const float l = glm::length(k - h) + glm::length(e - k);
    const fk::TwoBone reach = fk::solveTwoBone(h, k, e, glm::vec3(0, 0, -2), {1, 0, 0});
    CHECK(glm::length(reach.end - h) <= 0.999f * l + 1e-5f);
    CHECK(reach.end.z < e.z);  // as near the target as it reaches
    const fk::TwoBone fold = fk::solveTwoBone(h, k, e, h + glm::vec3(0, 0, -0.001f), {1, 0, 0});
    CHECK(glm::length(fold.end - h) >= 0.02f * l - 1e-5f);
    // A straight leg bends toward the forward hint.
    const fk::TwoBone straight = fk::solveTwoBone(h, glm::vec3(0, 0, 0.5f), e, glm::vec3(0, 0, 0.2f), {1, 0, 0});
    CHECK(straight.knee.x > 0.0f);
}

TEST_CASE("E9: offsets are limited, a ledge is ignored, the drop reaches the lower foot", "[foot_ik]") {
    const float leg = 1.0f;
    CHECK(fk::footOffset(1.1f, 1.0f, leg) == Catch::Approx(0.1f));
    CHECK(fk::footOffset(1.5f, 1.0f, leg) == Catch::Approx(fk::kMaxRaise * leg));
    CHECK(fk::footOffset(0.6f, 1.0f, leg) == Catch::Approx(-fk::kMaxLower * leg));
    CHECK(fk::footOffset(3.0f, 1.0f, leg) == 0.0f);  // a wall
    CHECK(fk::footOffset(-2.0f, 1.0f, leg) == 0.0f); // a drop off an edge
    CHECK(fk::footOffset(std::nullopt, 1.0f, leg) == 0.0f);
    CHECK(fk::footOffset(1.0f, std::nullopt, leg) == 0.0f);
    CHECK(fk::bodyDrop(0.1f, 0.05f, leg) == 0.0f);
    CHECK(fk::bodyDrop(0.1f, -0.1f, leg) == Catch::Approx(0.1f));
    CHECK(fk::bodyDrop(-0.5f, -0.6f, leg) == Catch::Approx(fk::kMaxLower * leg));
}

TEST_CASE("E9: the feet tilt with a slope, not on steps, within the limit", "[foot_ik]") {
    CHECK(fk::footNormal(0.0f, 0.0f).z == 1.0f);
    const glm::vec3 up = fk::footNormal(0.2f, 0.0f);  // rising ahead
    CHECK(up.x < 0.0f);
    CHECK(std::acos(fk::footNormal(0.7f, 0.0f).z) <= glm::radians(fk::kMaxFootTiltDegrees) + 1e-4f);
    CHECK(fk::footNormal(1.5f, 0.0f).z == 1.0f);  // steeper than kStepSlopeDegrees: stairs
}

TEST_CASE("E9: full on standing and walking, none at a run or off the feet", "[foot_ik]") {
    CHECK(fk::speedWeight(0.0f) == 1.0f);
    CHECK(fk::speedWeight(2.5f) == 1.0f);  // walk
    CHECK(fk::speedWeight(7.0f) == 0.0f);  // run
    float last = 1.0f;
    for (float v = 0.0f; v < 8.0f; v += 0.1f) {
        const float w = fk::speedWeight(v);
        CHECK(w <= last + 1e-6f);
        last = w;
    }
    CHECK(fk::standsOnFeet(0));    // Stand
    CHECK(fk::standsOnFeet(4));    // Walk
    CHECK(fk::standsOnFeet(69));   // dance
    CHECK_FALSE(fk::standsOnFeet(1));    // Death
    CHECK_FALSE(fk::standsOnFeet(38));   // Jump
    CHECK_FALSE(fk::standsOnFeet(42));   // Swim
    CHECK_FALSE(fk::standsOnFeet(91));   // Mount
    CHECK_FALSE(fk::standsOnFeet(97));   // Sitting
    CHECK_FALSE(fk::standsOnFeet(135));  // Fly
    CHECK_FALSE(fk::standsOnFeet(300));  // the flying set
}

TEST_CASE("E9: the eases are the same at any frame rate", "[foot_ik]") {
    float a = 0.0f, b = 0.0f;
    for (int i = 0; i < 30; ++i) a = fk::ease(a, 1.0f, 1.0f / 30.0f, fk::kHeightSeconds);
    for (int i = 0; i < 240; ++i) b = fk::ease(b, 1.0f, 1.0f / 240.0f, fk::kHeightSeconds);
    CHECK(a == Catch::Approx(b).margin(1e-4));
    CHECK(a <= 1.0f);
}

TEST_CASE("E9: zero targets leave the pose as it was, bit for bit", "[foot_ik]") {
    const auto b = humanLegs();
    const fk::Rig rig = fk::findLegs(b);
    REQUIRE(rig.valid);
    std::vector<glm::mat4> pose = restPose(b.size());
    // Some animated turn on the legs.
    pose[6] = glm::rotate(glm::mat4(1.0f), 0.2f, glm::vec3(0, 1, 0));
    for (int i : {8, 10, 12}) pose[i] = pose[6];
    const auto before = pose;
    fk::Targets t;  // weight 0
    glm::vec3 ends[2];
    fk::apply(pose, rig, t, ends);
    t.weight = 1.0f;  // weight 1, nothing asked
    fk::apply(pose, rig, t, ends);
    for (size_t i = 0; i < pose.size(); ++i) {
        CHECK(std::memcmp(&pose[i], &before[i], sizeof(glm::mat4)) == 0);
    }
    CHECK(glm::length(ends[0] - at(before, b, 10)) < 1e-6f);
}

TEST_CASE("E9: a raised foot lands on its floor, keeps its turn, and nothing else moves", "[foot_ik]") {
    const auto b = humanLegs();
    const fk::Rig rig = fk::findLegs(b);
    REQUIRE(rig.valid);
    std::vector<glm::mat4> pose = restPose(b.size());
    // A little knee bend so the solve has a side.
    const glm::mat4 bend = glm::translate(glm::mat4(1.0f), b[8].pivot) *
                           glm::rotate(glm::mat4(1.0f), -0.15f, glm::vec3(0, 1, 0)) *
                           glm::translate(glm::mat4(1.0f), -b[8].pivot);
    for (int i : {8, 10, 12}) pose[i] = bend;
    const auto before = pose;
    fk::Targets t;
    t.weight = 1.0f;
    t.offset[0] = 0.15f;   // the left foot's floor is higher
    t.offset[1] = -0.08f;  // the right one's lower
    t.drop = fk::bodyDrop(t.offset[0], t.offset[1], rig.legLength);
    fk::apply(pose, rig, t);
    const glm::vec3 endL0 = at(before, b, 10), endL1 = at(pose, b, 10);
    const glm::vec3 endR0 = at(before, b, 11), endR1 = at(pose, b, 11);
    CHECK(endL1.z - endL0.z == Catch::Approx(0.15f).margin(2e-3));
    CHECK(endR1.z - endR0.z == Catch::Approx(-0.08f).margin(2e-3));
    // The bones keep their lengths.
    CHECK(glm::length(at(pose, b, 8) - at(pose, b, 6)) ==
          Catch::Approx(glm::length(at(before, b, 8) - at(before, b, 6))).margin(1e-4));
    CHECK(glm::length(at(pose, b, 10) - at(pose, b, 8)) ==
          Catch::Approx(glm::length(at(before, b, 10) - at(before, b, 8))).margin(1e-4));
    // The foot keeps the animation's turn: the toe sits where it did from the ankle.
    const glm::vec3 toeFromAnkle0 = at(before, b, 12) - endL0;
    const glm::vec3 toeFromAnkle1 = at(pose, b, 12) - endL1;
    CHECK(glm::length(toeFromAnkle0 - toeFromAnkle1) < 1e-3f);
    // The body only dropped: the spine moved straight down by the drop.
    const glm::vec3 spine = at(pose, b, 2) - at(before, b, 2);
    CHECK(spine.z == Catch::Approx(-t.drop).margin(1e-5));
    CHECK(std::abs(spine.x) + std::abs(spine.y) < 1e-6f);
}

TEST_CASE("E9: aim turns floor reads into model-space targets", "[foot_ik]") {
    // A unit of scale 2 on a 15% slope across it: left foot higher.
    const float scale = 2.0f, leg = 1.0f * scale;
    const fk::Targets t = fk::aim(10.0f, 10.06f, 9.94f, 10.0f, 0.5f, 0.4f, leg, scale);
    CHECK(t.offset[0] == Catch::Approx(0.03f).margin(1e-5));
    CHECK(t.offset[1] == Catch::Approx(-0.03f).margin(1e-5));
    CHECK(t.drop == Catch::Approx(0.03f).margin(1e-5));
    CHECK(t.normal.y < 0.0f);  // leaning toward the right, downhill
    // Flat: exactly nothing.
    const fk::Targets flat = fk::aim(10.0f, 10.0f, 10.0f, 10.0f, 0.5f, 0.4f, leg, scale);
    CHECK(flat.offset[0] == 0.0f);
    CHECK(flat.offset[1] == 0.0f);
    CHECK(flat.drop == 0.0f);
    CHECK(flat.normal.z == 1.0f);
}

// ---------------------------------------------------------------- E20

TEST_CASE("E20: the zoom eases onto the target without passing it", "[camera_zoom]") {
    for (float start : {5.55f, 15.0f, 0.5f}) {
        for (float target : {6.55f, 2.0f, 30.0f}) {
            float d = start, v = 0.0f;
            const float sign = target > start ? 1.0f : -1.0f;
            float last = d;
            for (int i = 0; i < 600; ++i) {
                cz::step(d, v, target, 1.0f / 120.0f);
                CHECK((d - last) * sign >= -1e-6f);       // monotonic
                CHECK((target - d) * sign >= -1e-6f);     // never past
                CHECK(std::abs(d - last) <= cz::kMaxSpeed / 120.0f + 1e-4f);
                last = d;
            }
            CHECK(d == target);
            CHECK(v == 0.0f);
        }
    }
}

TEST_CASE("E20: the zoom's path is the same at 30 and 240 fps", "[camera_zoom]") {
    float a = 5.55f, va = 0.0f, b = 5.55f, vb = 0.0f;
    for (int i = 0; i < 6; ++i) cz::step(a, va, 6.55f, 1.0f / 30.0f);
    for (int i = 0; i < 48; ++i) cz::step(b, vb, 6.55f, 1.0f / 240.0f);
    CHECK(a == Catch::Approx(b).margin(2e-3));
    // A notch is most of the way there in a quarter second.
    CHECK(a > 6.35f);
}

TEST_CASE("E20: a notch turned while moving carries its speed on", "[camera_zoom]") {
    float d = 5.0f, v = 0.0f;
    for (int i = 0; i < 6; ++i) cz::step(d, v, 6.0f, 1.0f / 60.0f);
    REQUIRE(v > 0.0f);
    const float before = d;
    cz::step(d, v, 7.0f, 1.0f / 60.0f);
    CHECK(d > before);
}

// ---------------------------------------------------------------- E19

TEST_CASE("E19: no wind adds nothing; the wind blows the trees' way", "[weather_wind]") {
    CHECK(ww::push(0.0f, {10, 20}, 3.0f, false, 0.0f) == glm::vec2(0.0f));
    CHECK(ww::push(0.0f, {10, 20}, 3.0f, true, 1.0f) == glm::vec2(0.0f));
    const glm::vec2 rain = ww::push(1.0f, {10, 20}, 3.0f, false, 0.0f);
    CHECK(glm::dot(glm::normalize(rain), glm::normalize(glm::vec2(0.8f, 0.6f))) > 0.999f);
    // The gust band stays within m2.vert's 0.1-1.
    for (float x = -500.0f; x < 500.0f; x += 37.0f) {
        const float g = ww::gust({x, x * 0.3f}, x * 0.01f);
        CHECK(g >= 0.1f - 1e-6f);
        CHECK(g <= 1.0f + 1e-6f);
    }
    // Snow takes less of the wind than rain (plus its eddy).
    const glm::vec2 snow = ww::push(1.0f, {10, 20}, 3.0f, true, 0.0f);
    CHECK(glm::length(snow) < glm::length(rain));
    // A storm's stronger wind pushes harder.
    CHECK(glm::length(ww::push(1.8f, {10, 20}, 3.0f, false, 0.0f)) > glm::length(rain));
}

// ---------------------------------------------------------------- E11

TEST_CASE("E11: level ground and the near limit are exactly the fog colour", "[horizon]") {
    const glm::vec3 toSun = glm::normalize(glm::vec3(0.3f, 0.2f, 0.8f));
    const glm::vec3 amb(0.4f, 0.4f, 0.45f), sun(0.6f, 0.55f, 0.5f);
    CHECK(hd::relief({0, 0, 1}, toSun, amb, sun, 0.35f) == glm::vec3(1.0f));
    const glm::vec3 toward = glm::normalize(glm::vec3(0.5f, 0.3f, 0.8f));
    const glm::vec3 away = glm::normalize(glm::vec3(-0.6f, -0.4f, 0.7f));
    CHECK(hd::relief(toward, toSun, amb, sun, 0.35f).x > 1.0f);
    CHECK(hd::relief(away, toSun, amb, sun, 0.35f).x < 1.0f);
    CHECK(hd::relief(away, toSun, amb, sun, 0.0f) == glm::vec3(1.0f));
    // The haze: none at the near limit, none at the far, none in a fog that
    // ends well short of the far clip, never more than the most.
    const float nearL = 1850.0f, farL = 7600.0f, farClip = 1900.0f;
    CHECK(hd::reliefWeight(nearL, nearL, farL, farClip, farClip) == 0.0f);
    CHECK(hd::reliefWeight(farL, nearL, farL, farClip, farClip) == 0.0f);
    CHECK(hd::reliefWeight(2500.0f, nearL, farL, 0.3f * farClip, farClip) == 0.0f);
    for (float z = nearL; z <= farL; z += 50.0f) {
        CHECK(hd::reliefWeight(z, nearL, farL, farClip, farClip) <= hd::kMaxRelief + 1e-6f);
    }
    CHECK(hd::reliefWeight(2400.0f, nearL, farL, farClip, farClip) > 0.1f);
}

TEST_CASE("E11: WDL normals are up on flat ground and lean off a slope", "[horizon]") {
    st::WdlTile flat;
    flat.tileX = 30;
    flat.tileY = 30;
    std::vector<uint32_t> n;
    hd::wdlTileNormals(flat, 533.3333f, n);
    REQUIRE(n.size() == 17 * 17 + 16 * 16);
    for (uint32_t p : n) CHECK(hd::unpackNormal(p).z > 0.99f);
    // Heights rising along the columns (toward -y in render space): the
    // normal leans toward +y.
    st::WdlTile slope = flat;
    for (int r = 0; r < 17; ++r)
        for (int c = 0; c < 17; ++c) slope.heights[r * 17 + c] = static_cast<int16_t>(c * 10);
    for (int r = 0; r < 16; ++r)
        for (int c = 0; c < 16; ++c) slope.heights[289 + r * 16 + c] = static_cast<int16_t>(c * 10 + 5);
    n.clear();
    hd::wdlTileNormals(slope, 533.3333f, n);
    const glm::vec3 mid = hd::unpackNormal(n[8 * 17 + 8]);
    CHECK(mid.y > 0.2f);
    CHECK(std::abs(mid.x) < 0.02f);
    CHECK(glm::length(hd::unpackNormal(n[289 + 5 * 16 + 5]) - mid) < 0.03f);
}

TEST_CASE("E11: the WDL's MWMO/MWID/MODF placements are read", "[horizon]") {
    std::vector<uint8_t> d;
    auto chunk = [&](const char (&tag)[5], const std::vector<uint8_t>& body) {
        for (int i = 3; i >= 0; --i) d.push_back(static_cast<uint8_t>(tag[i]));
        const uint32_t n = static_cast<uint32_t>(body.size());
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<uint8_t>(n >> (8 * i)));
        d.insert(d.end(), body.begin(), body.end());
    };
    chunk("MVER", {18, 0, 0, 0});
    const std::string a = "WORLD\\WMO\\A_LOW.WMO", b = "WORLD\\WMO\\B_LOW.WMO";
    std::vector<uint8_t> names(a.begin(), a.end());
    names.push_back(0);
    const uint32_t bOff = static_cast<uint32_t>(names.size());
    names.insert(names.end(), b.begin(), b.end());
    names.push_back(0);
    chunk("MWMO", names);
    std::vector<uint8_t> ids(8, 0);
    std::memcpy(ids.data() + 4, &bOff, 4);
    chunk("MWID", ids);
    std::vector<uint8_t> modf(64, 0);
    const uint32_t nameId = 1;
    const float pos[3] = {12218.0f, 57.0f, 9524.0f}, rot[3] = {0.0f, -178.0f, 0.0f};
    std::memcpy(modf.data(), &nameId, 4);
    std::memcpy(modf.data() + 8, pos, 12);
    std::memcpy(modf.data() + 20, rot, 12);
    chunk("MODF", modf);
    chunk("MAOF", std::vector<uint8_t>(64 * 64 * 4, 0));
    const auto w = hd::parseWdlWmos(d);
    REQUIRE(w.size() == 1);
    CHECK(w[0].path == b);
    CHECK(w[0].position[0] == 12218.0f);
    CHECK(w[0].rotation[1] == -178.0f);
    CHECK(w[0].scale == 1.0f);
}
