// Capes, tails and hair (E7) and smoother other units (E10):
// docs/enhancements-plan.md. Which bones the spring chains take, that the
// spring solver stays finite, inside its limits and at rest when nothing moves
// whatever the frame rate, and that a teleport or a hitch starts it over; and
// the remote smoothing's step, decay and snap rules.
#include <catch_amalgamated.hpp>

#include "rendering/remote_smoothing.hpp"
#include "rendering/secondary_motion.hpp"

#include <cmath>
#include <random>
#include <vector>

namespace sm = wowee::rendering::secondary_motion;
namespace rs = wowee::rendering::remote_smoothing;

namespace {

sm::BoneInfo bone(int key, int parent, glm::vec3 pivot, float body, float hair = 0, float cloth = 0,
                  float other = 0) {
    sm::BoneInfo b;
    b.keyBoneId = key;
    b.parent = parent;
    b.pivot = pivot;
    b.body = body;
    b.hair = hair;
    b.cloth = cloth;
    b.total = body + hair + cloth + other;
    return b;
}

// HumanMale's skeleton in miniature, with its pivots and weight classes
// (tools survey, docs/enhancements-plan.md E7).
std::vector<sm::BoneInfo> humanoid() {
    std::vector<sm::BoneInfo> b;
    b.push_back(bone(-1, -1, {0, 0, 0}, 0));              // 0 root
    b.push_back(bone(26, 0, {0, 0, 1.22f}, 100));         // 1 key Root
    b.push_back(bone(4, 1, {0, 0, 1.21f}, 100));          // 2 SpineLow
    b.push_back(bone(5, 1, {0.02f, 0, 1.16f}, 100));      // 3 Waist
    b.push_back(bone(-1, 2, {-0.07f, 0, 1.45f}, 100));    // 4 chest (has key below)
    b.push_back(bone(-1, 3, {-0.15f, 0, 0.94f}, 0, 0, 43));    // 5 back flap (cloak)
    b.push_back(bone(-1, 3, {0.16f, 0, 1.06f}, 9, 0, 192, 60)); // 6 front flap
    b.push_back(bone(-1, 3, {0.03f, 0.13f, 1.08f}, 396, 0, 77, 5000));  // 7 hip L
    b.push_back(bone(-1, 4, {-0.04f, 0, 1.73f}, 100));    // 8 neck
    b.push_back(bone(6, 8, {-0.01f, 0, 1.84f}, 100));     // 9 Head
    b.push_back(bone(-1, 5, {-0.16f, 0, 0.8f}, 0, 0, 61));     // 10 back flap 2
    b.push_back(bone(-1, 6, {0.19f, 0, 0.9f}, 0, 0, 61));      // 11 front flap 2
    b.push_back(bone(-1, 7, {-0.01f, 0.17f, 0.58f}, 500, 0, 0, 900));  // 12 knee L
    b.push_back(bone(-1, 12, {-0.09f, 0.2f, 0.1f}, 300));  // 13 ankle L
    b.push_back(bone(-1, 9, {-0.06f, 0, 1.89f}, 0, 900));  // 14 hair 1
    b.push_back(bone(-1, 14, {-0.15f, 0, 1.85f}, 0, 800)); // 15 hair 2
    b.push_back(bone(-1, 15, {-0.23f, 0, 1.62f}, 0, 700)); // 16 hair 3
    b.push_back(bone(7, 9, {0.08f, 0, 1.84f}, 100));       // 17 Jaw
    return b;
}

// A wolf: the tail bones all children of the pelvis (a fan), hind legs off
// the same pelvis, front legs and the head forward.
std::vector<sm::BoneInfo> wolf() {
    std::vector<sm::BoneInfo> b;
    b.push_back(bone(26, -1, {0.39f, 0, 1.32f}, 100));   // 0 Root
    b.push_back(bone(-1, 0, {0.39f, 0, 1.32f}, 100));    // 1
    b.push_back(bone(5, 1, {-0.16f, 0, 1.32f}, 100));    // 2 Waist
    b.push_back(bone(-1, 2, {-0.6f, 0, 1.27f}, 100));    // 3 pelvis
    for (float x : {-0.9f, -1.16f, -1.48f, -1.73f, -1.93f})
        b.push_back(bone(-1, 3, {x, 0, 1.2f}, 100));     // 4-8 tail fan
    b.push_back(bone(-1, 3, {-0.61f, 0.19f, 1.14f}, 100));  // 9 hip L
    b.push_back(bone(-1, 9, {-0.53f, 0.25f, 0.73f}, 100));  // 10
    b.push_back(bone(-1, 10, {-0.81f, 0.27f, 0.11f}, 100)); // 11 foot
    b.push_back(bone(6, 0, {1.02f, 0, 1.22f}, 100));     // 12 Head
    return b;
}

const sm::Chain* findKind(const std::vector<sm::Chain>& chains, sm::ChainKind k) {
    for (const auto& c : chains)
        if (c.kind == k) return &c;
    return nullptr;
}

float maxAbsSwing(const std::vector<sm::BoneState>& s) {
    float m = 0;
    for (const auto& b : s) m = std::max({m, std::abs(b.swing.x), std::abs(b.swing.y)});
    return m;
}

struct Sim {
    sm::Chain chain;
    sm::Params p;
    sm::ChainState cs;
    std::vector<sm::BoneState> bones;
    explicit Sim(const sm::Chain& c) : chain(c), p(sm::paramsFor(c.kind)), bones(c.bones.size()) {}
    bool step(glm::vec3 probe, float dt, glm::vec3 wind = glm::vec3(0)) {
        sm::Input in;
        in.probeWorld = probe;
        in.dt = dt;
        in.windWorld = wind;
        return sm::stepChain(chain, p, cs, bones.data(), in);
    }
    void checkLimits() const {
        const float share = 1.0f / static_cast<float>(bones.size());
        for (const auto& b : bones) {
            REQUIRE(std::isfinite(b.swing.x));
            REQUIRE(std::isfinite(b.swing.y));
            CHECK(b.swing.x <= p.outLimit * share + 1e-5f);
            CHECK(b.swing.x >= -p.inLimit * share - 1e-5f);
            CHECK(std::abs(b.swing.y) <= p.sideLimit * share + 1e-5f);
        }
    }
};

}  // namespace

TEST_CASE("the cloak and front flaps, the ponytail and not the legs", "[secondary_motion]") {
    const auto chains = sm::findChains(humanoid(), true);
    std::vector<std::vector<uint16_t>> cloth, hair;
    for (const auto& c : chains) {
        if (c.kind == sm::ChainKind::Cloth) cloth.push_back(c.bones);
        if (c.kind == sm::ChainKind::Hair) hair.push_back(c.bones);
        CHECK(c.kind != sm::ChainKind::Tail);
    }
    CHECK(cloth == std::vector<std::vector<uint16_t>>{{5, 10}, {6, 11}});
    CHECK(hair == std::vector<std::vector<uint16_t>>{{14, 15, 16}});
    // Behind the body the cloak swings out backward; the front flap forward.
    for (const auto& c : chains) {
        if (c.bones.front() == 5) CHECK(c.outward.x < -0.9f);
        if (c.bones.front() == 6) CHECK(c.outward.x > 0.9f);
        CHECK(std::abs(glm::length(c.restDir) - 1.0f) < 1e-4f);
        CHECK(std::abs(glm::dot(c.restDir, c.outward)) < 1e-4f);
        CHECK(std::abs(glm::dot(c.restDir, c.lateral)) < 1e-4f);
    }
    // A creature model has no cloth or hair: the same skeleton read as one
    // has nothing that hangs back from the waist.
    for (const auto& c : sm::findChains(humanoid(), false)) CHECK(c.kind == sm::ChainKind::Tail);
}

TEST_CASE("a wolf's flat tail is a fan; its legs and head are not chains", "[secondary_motion]") {
    const auto chains = sm::findChains(wolf(), false);
    REQUIRE(chains.size() == 1);
    CHECK(chains[0].kind == sm::ChainKind::Tail);
    CHECK(chains[0].fan);
    CHECK(chains[0].bones == std::vector<uint16_t>{4, 5, 6, 7, 8});  // body out
    CHECK(chains[0].outward.z > 0.9f);                                // a tail swings up and down
}

TEST_CASE("a tail is a chain off the waist pointing back; wings and legs are not", "[secondary_motion]") {
    std::vector<sm::BoneInfo> b;
    b.push_back(bone(26, -1, {0, 0, 2}, 100));          // 0
    b.push_back(bone(5, 0, {-0.5f, 0, 2}, 100));        // 1 Waist
    b.push_back(bone(-1, 1, {-1.1f, 0, 2.05f}, 100));   // 2 tail
    b.push_back(bone(-1, 2, {-1.8f, 0, 2.0f}, 100));    // 3
    b.push_back(bone(-1, 3, {-2.5f, 0, 1.95f}, 100));   // 4
    b.push_back(bone(-1, 1, {-0.5f, 0.2f, 2.1f}, 100)); // 5 leg
    b.push_back(bone(-1, 5, {-0.6f, 0.25f, 1.0f}, 100));// 6
    b.push_back(bone(-1, 6, {-0.9f, 0.3f, 0.1f}, 100)); // 7 foot on the ground
    b.push_back(bone(4, 0, {0.5f, 0, 2.5f}, 100));      // 8 SpineLow
    b.push_back(bone(-1, 8, {0.4f, 0.5f, 3.0f}, 100));  // 9 wing, off the chest
    b.push_back(bone(-1, 9, {-0.5f, 2.5f, 3.2f}, 100)); // 10
    b.push_back(bone(-1, 10, {-1.5f, 4.0f, 3.0f}, 100));// 11
    const auto chains = sm::findChains(b, false);
    REQUIRE(chains.size() == 1);
    CHECK(chains[0].bones == std::vector<uint16_t>{2, 3, 4});
    CHECK_FALSE(chains[0].fan);
}

TEST_CASE("chains stay within the bone caps", "[secondary_motion]") {
    // A tail of 20 links is longer than a chain may be: it is left alone.
    std::vector<sm::BoneInfo> b;
    b.push_back(bone(26, -1, {0, 0, 2}, 100));
    b.push_back(bone(5, 0, {-0.3f, 0, 2}, 100));
    for (int i = 0; i < 20; ++i)
        b.push_back(bone(-1, static_cast<int>(b.size()) - 1, {-0.5f - 0.2f * i, 0, 2}, 100));
    CHECK(sm::findChains(b, false).empty());
    // Any model's chains together hold at most kMaxModelBones.
    size_t total = 0;
    for (const auto& c : sm::findChains(humanoid(), true)) {
        CHECK(c.bones.size() <= sm::kMaxChainBones);
        total += c.bones.size();
    }
    CHECK(total <= sm::kMaxModelBones);
}

TEST_CASE("the levels: off is nothing, 1 is cloth only", "[secondary_motion]") {
    for (auto k : {sm::ChainKind::Cloth, sm::ChainKind::Hair, sm::ChainKind::Tail})
        CHECK_FALSE(sm::kindEnabled(k, sm::kLevelOff));
    CHECK(sm::kindEnabled(sm::ChainKind::Cloth, sm::kLevelCloth));
    CHECK_FALSE(sm::kindEnabled(sm::ChainKind::Hair, sm::kLevelCloth));
    CHECK_FALSE(sm::kindEnabled(sm::ChainKind::Tail, sm::kLevelCloth));
    CHECK(sm::kindEnabled(sm::ChainKind::Tail, sm::kLevelAll));
}

TEST_CASE("standing still the spring stays exactly at the authored pose", "[secondary_motion]") {
    const auto chains = sm::findChains(humanoid(), true);
    Sim s(*findKind(chains, sm::ChainKind::Cloth));
    for (int i = 0; i < 600; ++i) s.step({10, 20, 30}, 1.0f / 60.0f);
    CHECK(maxAbsSwing(s.bones) == 0.0f);
}

TEST_CASE("running off swings the cloak out behind, then it settles", "[secondary_motion]") {
    const auto chains = sm::findChains(humanoid(), true);
    const sm::Chain* cape = nullptr;
    for (const auto& c : chains)
        if (c.bones.front() == 5) cape = &c;
    REQUIRE(cape);
    Sim s(*cape);
    glm::vec3 p(0);
    const float dt = 1.0f / 60.0f;
    s.step(p, dt);
    // Accelerate along +x (the model faces +x) to 7 yd/s.
    float v = 0, peak = 0;
    for (int i = 0; i < 30; ++i) {
        v = std::min(7.0f, v + 50.0f * dt);
        p.x += v * dt;
        s.step(p, dt);
        peak = std::max(peak, s.bones.back().swing.x);
        s.checkLimits();
    }
    CHECK(peak > 0.02f);  // out, behind the back
    // Steady run: drag holds it a little out; then stop and stand.
    for (int i = 0; i < 120; ++i) { p.x += v * dt; s.step(p, dt); }
    CHECK(s.bones.back().swing.x > 0.0f);
    for (int i = 0; i < 240; ++i) s.step(p, dt);
    CHECK(maxAbsSwing(s.bones) < 0.002f);
}

TEST_CASE("the spring is stable and inside its limits at any frame rate", "[secondary_motion]") {
    const auto human = sm::findChains(humanoid(), true);
    const auto beast = sm::findChains(wolf(), false);
    std::vector<sm::Chain> all(human.begin(), human.end());
    all.insert(all.end(), beast.begin(), beast.end());
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (float dt : {1.0f / 240.0f, 1.0f / 60.0f, 1.0f / 20.0f, 0.1f, 0.2f}) {
        for (const auto& chain : all) {
            INFO("dt " << dt << " chain from bone " << chain.bones.front());
            Sim s(chain);
            glm::vec3 p(0), vel(0);
            for (int i = 0; i < 2000; ++i) {
                // Violent, jittering motion: sprinting, stopping dead,
                // jumping, turning, frame-time noise.
                vel += glm::vec3(u(rng), u(rng), u(rng)) * 30.0f * dt;
                if (i % 97 == 0) vel = glm::vec3(u(rng), u(rng), u(rng)) * 15.0f;
                p += vel * dt + glm::vec3(u(rng), u(rng), u(rng)) * 0.02f;
                const float jitter = dt * (1.0f + 0.3f * u(rng));
                s.step(p, jitter, glm::vec3(u(rng), u(rng), 0) * 8.0f);
                s.checkLimits();
            }
        }
    }
}

TEST_CASE("a teleport or a long hitch starts the chain over from rest", "[secondary_motion]") {
    const auto chains = sm::findChains(humanoid(), true);
    Sim s(*findKind(chains, sm::ChainKind::Cloth));
    glm::vec3 p(0);
    s.step(p, 1.0f / 60.0f);
    for (int i = 0; i < 20; ++i) { p.y += 0.2f; s.step(p, 1.0f / 60.0f); }
    REQUIRE(maxAbsSwing(s.bones) > 0.0f);
    // 200 yards in a frame: a teleport, not a shove.
    CHECK_FALSE(s.step(p + glm::vec3(200, 0, 0), 1.0f / 60.0f));
    CHECK(maxAbsSwing(s.bones) == 0.0f);
    for (int i = 0; i < 20; ++i) { p.y += 0.2f; s.step(p + glm::vec3(200, 0, 0), 1.0f / 60.0f); }
    // A second-long hitch (out of view, a loading stall).
    CHECK_FALSE(s.step(p + glm::vec3(205, 0, 0), 1.0f));
    CHECK(maxAbsSwing(s.bones) == 0.0f);
    // A non-finite position never reaches the bones.
    CHECK_FALSE(s.step(glm::vec3(NAN, 0, 0), 1.0f / 60.0f));
    CHECK(maxAbsSwing(s.bones) == 0.0f);
}

TEST_CASE("a fall floats the cloak out and a turn swings it aside", "[secondary_motion]") {
    const auto chains = sm::findChains(humanoid(), true);
    const sm::Chain* cape = nullptr;
    for (const auto& c : chains)
        if (c.bones.front() == 5) cape = &c;
    REQUIRE(cape);
    {
        Sim s(*cape);
        glm::vec3 p(0, 0, 100);
        float vz = 0;
        const float dt = 1.0f / 60.0f;
        s.step(p, dt);
        for (int i = 0; i < 30; ++i) {  // free fall
            vz -= 19.29f * dt;
            p.z += vz * dt;
            s.step(p, dt);
        }
        CHECK(s.bones.back().swing.x > 0.0f);
    }
    {
        // Sideways acceleration (a turn): a side swing, held to the limit.
        Sim s(*cape);
        glm::vec3 p(0);
        float vy = 0;
        const float dt = 1.0f / 60.0f;
        s.step(p, dt);
        for (int i = 0; i < 30; ++i) {
            vy += 40.0f * dt;
            p.y += vy * dt;
            s.step(p, dt);
            s.checkLimits();
        }
        CHECK(std::abs(s.bones.back().swing.y) > 0.01f);
    }
}

TEST_CASE("the wind stirs a cloak standing still", "[secondary_motion]") {
    const auto chains = sm::findChains(humanoid(), true);
    Sim s(*findKind(chains, sm::ChainKind::Cloth));
    float peak = 0;
    for (int i = 0; i < 600; ++i) {
        const float g = sm::gust({0, 0}, {0.8f, 0.6f}, i / 60.0f, 0.0f);
        s.step({0, 0, 0}, 1.0f / 60.0f, glm::vec3(-5.0f * g, 0, 0));  // blowing toward -x: behind
        peak = std::max(peak, s.bones.back().swing.x);
        s.checkLimits();
    }
    CHECK(peak > 0.005f);
}

TEST_CASE("rotationVector moves a hanging bone's tip along its outward direction", "[secondary_motion]") {
    const auto chains = sm::findChains(humanoid(), true);
    for (const auto& c : chains) {
        sm::BoneState s;
        s.swing = {0.1f, 0.0f};
        const glm::vec3 rv = sm::rotationVector(c, s, 1.0f);
        const glm::quat q = glm::angleAxis(glm::length(rv), glm::normalize(rv));
        const glm::vec3 tip = q * c.restDir;
        CHECK(glm::dot(tip - c.restDir, c.outward) > 0.09f);
        // The distance fade scales it; zero is none.
        CHECK(glm::length(sm::rotationVector(c, s, 0.0f)) == 0.0f);
    }
    CHECK(sm::distanceWeight(10.0f) == 1.0f);
    CHECK(sm::distanceWeight(60.0f) == 0.0f);
    CHECK(sm::distanceWeight(37.5f) == Catch::Approx(0.5f));
}

TEST_CASE("remote smoothing: a step glides, motion does not, a teleport snaps", "[remote_smoothing]") {
    const float dt = 1.0f / 60.0f;
    {
        // Running steadily at 7 yd/s: nothing is offset.
        rs::State s;
        glm::vec3 p(0);
        for (int i = 0; i < 120; ++i) {
            p.x += 7.0f * dt;
            rs::step(s, p, 0.0f, dt);
        }
        CHECK(glm::length(s.offset) < 1e-3f);
    }
    {
        // Standing, then a correction of 1 yd: drawn where it was, then glides.
        rs::State s;
        glm::vec3 p(0);
        for (int i = 0; i < 30; ++i) rs::step(s, p, 0.0f, dt);
        p.x += 1.0f;
        rs::step(s, p, 0.0f, dt);
        CHECK(glm::length(p + s.offset) < 0.9f);       // still near the old spot
        for (int i = 0; i < 30; ++i) rs::step(s, p, 0.0f, dt);
        CHECK(glm::length(s.offset) < 0.01f);          // there within half a second
    }
    {
        // A teleport snaps, as stock.
        rs::State s;
        glm::vec3 p(0);
        for (int i = 0; i < 10; ++i) rs::step(s, p, 0.0f, dt);
        p.x += 40.0f;
        rs::step(s, p, 0.0f, dt);
        CHECK(glm::length(s.offset) == 0.0f);
    }
    {
        // A facing snap of 180 degrees turns over a moment; the body ease's
        // own small turns are left alone.
        rs::State s;
        for (int i = 0; i < 10; ++i) rs::step(s, glm::vec3(0), 0.0f, dt);
        rs::step(s, glm::vec3(0), 0.2f, dt);
        CHECK(s.yawOffset == 0.0f);
        rs::step(s, glm::vec3(0), 3.0f, dt);
        CHECK(std::abs(s.yawOffset) > 2.0f);
        for (int i = 0; i < 60; ++i) rs::step(s, glm::vec3(0), 3.0f, dt);
        CHECK(std::abs(s.yawOffset) < 1e-3f);
    }
    {
        // A long gap starts over: no offset from what happened meanwhile.
        rs::State s;
        rs::step(s, glm::vec3(0), 0.0f, dt);
        rs::step(s, glm::vec3(3, 0, 0), 0.0f, 1.0f);
        CHECK(glm::length(s.offset) == 0.0f);
    }
}

TEST_CASE("remote smoothing: reported corrections glide, a charge does not", "[remote_smoothing]") {
    const float dt = 1.0f / 60.0f;
    {
        // A charge: standing, 0.6 s at the 28 yd/s cap, standing. Motion
        // throughout - the motion reports no correction - so the model is
        // drawn where the unit is at every frame: no lag at the start, no
        // overshoot at the end.
        rs::State s;
        glm::vec3 p(0);
        float worst = 0.0f;
        for (int i = 0; i < 30; ++i) rs::stepReported(s, p, 0.0f, dt, glm::vec3(0));
        for (int i = 0; i < 36; ++i) {
            p.x += 28.0f * dt;
            rs::stepReported(s, p, 0.0f, dt, glm::vec3(0));
            worst = std::max(worst, glm::length(s.offset));
        }
        for (int i = 0; i < 30; ++i) {
            rs::stepReported(s, p, 0.0f, dt, glm::vec3(0));
            worst = std::max(worst, glm::length(s.offset));
        }
        CHECK(worst == 0.0f);
    }
    {
        // The guess from the velocity, for the same charge, lagged at the
        // start and overshot at the end - why the stock motion reports.
        rs::State s;
        glm::vec3 p(0);
        for (int i = 0; i < 30; ++i) rs::step(s, p, 0.0f, dt);
        for (int i = 0; i < 36; ++i) { p.x += 28.0f * dt; rs::step(s, p, 0.0f, dt); }
        rs::step(s, p, 0.0f, dt);
        CHECK(glm::length(s.offset) > 0.25f);
    }
    {
        // A packet that put the unit 1 yd from where dead reckoning had it:
        // drawn near the old spot, then there within half a second.
        rs::State s;
        glm::vec3 p(0);
        for (int i = 0; i < 30; ++i) rs::stepReported(s, p, 0.0f, dt, glm::vec3(0));
        p.x += 1.0f;
        rs::stepReported(s, p, 0.0f, dt, glm::vec3(1, 0, 0));
        CHECK(glm::length(p + s.offset) < 0.9f);
        for (int i = 0; i < 30; ++i) rs::stepReported(s, p, 0.0f, dt, glm::vec3(0));
        CHECK(glm::length(s.offset) < 0.01f);
    }
    {
        // A long correction snaps, as stock.
        rs::State s;
        glm::vec3 p(0);
        for (int i = 0; i < 10; ++i) rs::stepReported(s, p, 0.0f, dt, glm::vec3(0));
        p.x += 12.0f;
        rs::stepReported(s, p, 0.0f, dt, glm::vec3(12, 0, 0));
        CHECK(glm::length(s.offset) == 0.0f);
    }
}

TEST_CASE("remote smoothing: the locomotion blend floor and nothing else", "[remote_smoothing]") {
    CHECK(rs::blendMs(0, true, true, true) == rs::kLocomotionBlendFloorMs);
    CHECK(rs::blendMs(500, true, true, true) == 500);  // a longer stock blend stays
    CHECK(rs::blendMs(0, false, true, true) == 0);     // off: stock
    CHECK(rs::blendMs(0, true, true, false) == 0);     // into an attack, a cast, an emote
    CHECK(rs::smoothableLocomotion(0, false));         // Stand
    CHECK(rs::smoothableLocomotion(5, true));          // Run
    CHECK(rs::smoothableLocomotion(41, false));        // SwimIdle
    CHECK_FALSE(rs::smoothableLocomotion(37, true));   // JumpStart keeps its timing
    CHECK_FALSE(rs::smoothableLocomotion(187, true));
    CHECK_FALSE(rs::smoothableLocomotion(16, false));  // Attack
}

TEST_CASE("the strength scales the swing; 0 holds every bone to its tracks", "[secondary_motion]") {
    const auto chains = sm::findChains(humanoid(), true);
    const sm::Chain* cape = nullptr;
    for (const auto& c : chains)
        if (c.bones.front() == 5) cape = &c;
    REQUIRE(cape);
    // Run off at 7 yd/s and hold it; the steady trail at each strength.
    auto trail = [&](float strength) {
        sm::ChainState cs;
        std::vector<sm::BoneState> b(cape->bones.size());
        glm::vec3 p(0);
        float v = 0;
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 240; ++i) {
            v = std::min(7.0f, v + 50.0f * dt);
            p.x += v * dt;
            sm::Input in;
            in.probeWorld = p;
            in.dt = dt;
            in.strength = strength;
            sm::stepChain(*cape, sm::paramsFor(cape->kind), cs, b.data(), in);
            const float lim = sm::limitScale(strength) / static_cast<float>(b.size());
            for (const auto& s : b) CHECK(s.swing.x <= sm::paramsFor(cape->kind).outLimit * lim + 1e-5f);
        }
        float out = 0;
        for (const auto& s : b) out += s.swing.x;
        return out;
    };
    CHECK(trail(0.0f) == 0.0f);
    const float one = trail(1.0f), half = trail(0.5f);
    // About 15 degrees of trail at a run at the tuned strength.
    CHECK(glm::degrees(one) > 10.0f);
    CHECK(glm::degrees(one) < 22.0f);
    CHECK(half == Catch::Approx(0.5f * one).epsilon(0.05));  // eased into the limit, so nearly linear
    CHECK(trail(2.0f) > one);
    // The setting itself is clamped and never non-finite.
    sm::setStrength(5.0f);
    CHECK(sm::strength() == 2.0f);
    sm::setStrength(NAN);
    CHECK(sm::strength() == 0.0f);
    sm::setStrength(1.0f);
}

TEST_CASE("the body collider turns a cloth bone out of a leg, and leaves a clear one", "[secondary_motion]") {
    // A cloak bone hanging from (-0.15, 0, 0.94) to (-0.16, 0, 0.5), outward -x;
    // a thigh kicked back so it passes through the cloth.
    const glm::vec3 pivot(-0.15f, 0.0f, 0.94f), tip(-0.16f, 0.0f, 0.5f);
    const glm::vec3 out(-1, 0, 0);
    const sm::Capsule kicked{{0.0f, 0.0f, 1.0f}, {-0.2f, 0.0f, 0.55f}, 0.1f};
    const glm::vec3 pts[2] = {tip, 0.5f * (pivot + tip)};
    const glm::vec3 turn = sm::clearanceTurn(pivot, pts, 2, out, &kicked, 1);
    REQUIRE(glm::length(turn) > 0.0f);
    CHECK(glm::length(turn) <= sm::kMaxCollisionTurn + 1e-5f);
    // Turned, the tip has moved out (-x) and is outside the capsule.
    const glm::quat q = glm::angleAxis(glm::length(turn), glm::normalize(turn));
    const glm::vec3 moved = pivot + q * (tip - pivot);
    CHECK(moved.x < tip.x);
    CHECK(sm::clearanceAlong(moved, out, kicked) < 0.02f);
    // A leg well in front of the cloth asks for nothing.
    const sm::Capsule front{{0.05f, 0.13f, 1.0f}, {0.05f, 0.15f, 0.55f}, 0.1f};
    CHECK(glm::length(sm::clearanceTurn(pivot, pts, 2, out, &front, 1)) == 0.0f);
    // A radius fitted to the rest pose never reaches a rest point.
    const std::vector<glm::vec3> rest = {tip, 0.5f * (pivot + tip)};
    const float r = sm::fitRadius(0.5f, front.a, front.b, rest);
    const sm::Capsule fitted{front.a, front.b, r};
    CHECK(glm::length(sm::clearanceTurn(pivot, pts, 2, out, &fitted, 1)) == 0.0f);
    CHECK(r < 0.5f);
}

TEST_CASE("a robe's hem: no lag at a steady pace, a bounded swing with the stride", "[secondary_motion]") {
    const float dt = 1.0f / 60.0f;
    // Standing still: exactly where the shin is.
    {
        sm::HemState s;
        for (int i = 0; i < 300; ++i) CHECK(sm::stepHem(s, sm::HemParams{}, {1, 2, 3}, {}, dt) == glm::vec3(0.0f));
    }
    // Moving steadily with no air drag: no lag once settled.
    {
        sm::HemState s;
        sm::HemParams p;
        p.drag = 0.0f;
        glm::vec3 t(0);
        glm::vec3 o(0);
        for (int i = 0; i < 600; ++i) { t.x += 7.0f * dt; o = sm::stepHem(s, p, t, {}, dt); }
        CHECK(glm::length(o) < 1e-3f);
    }
    // A running stride: the shin swings 0.3 yd fore and aft at 1.3 Hz while
    // the body runs at 7 yd/s. The hem swings with a lag, stays finite, and
    // within the soft limit.
    for (float step : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f}) {
        sm::HemState s;
        float peak = 0.0f;
        for (int i = 0; i < static_cast<int>(6.0f / step); ++i) {
            const float time = static_cast<float>(i) * step;
            const glm::vec3 t(7.0f * time + 0.3f * std::sin(2.0f * 3.14159265f * 1.3f * time), 0, 0.3f);
            const glm::vec3 o = sm::stepHem(s, sm::HemParams{}, t, {}, step);
            REQUIRE(std::isfinite(o.x));
            const glm::vec3 c = sm::softClamp(o, 0.07f);
            CHECK(glm::length(c) <= 0.07f + 1e-5f);
            if (time > 2.0f) peak = std::max(peak, glm::length(o));
        }
        CHECK(peak > 0.02f);
        CHECK(peak < 0.5f);
    }
    // A teleport starts it over at the target.
    {
        sm::HemState s;
        sm::stepHem(s, sm::HemParams{}, {0, 0, 0}, {}, dt);
        sm::stepHem(s, sm::HemParams{}, {0.1f, 0, 0}, {}, dt);
        CHECK(sm::stepHem(s, sm::HemParams{}, {100, 0, 0}, {}, dt) == glm::vec3(0.0f));
    }
    CHECK(sm::softClamp(glm::vec3(0.0f), 0.07f) == glm::vec3(0.0f));
    CHECK(glm::length(sm::softClamp(glm::vec3(10, 0, 0), 0.07f)) <= 0.07f);
}
