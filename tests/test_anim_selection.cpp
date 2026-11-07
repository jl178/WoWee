// Stock 3.3.5a animation selection rules (docs/re/animation.md): the
// AnimationData fallback chain and its play modes, the variation roll, the
// replay laps, the phase carry and rate of moving animations, the pace and
// stealth rules of 0x717050, and the stand-state transitions.
#include <catch_amalgamated.hpp>

#include "anim_test_support.hpp"
#include "rendering/animation/anim_selection.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/character_animator.hpp"

#include <initializer_list>
#include <vector>

using namespace wowee::rendering;
namespace a = wowee::rendering::anim;
using wowee::pipeline::M2Sequence;

namespace {

// The AnimationData rows the rules below read, as the 3.3.5a file has them
// (ID, Fallback, Flags): enough of the table for the chains under test.
void loadAnimationDataRows() {
    struct Row { uint32_t id, fallback, flags; };
    static const Row rows[] = {
        {0, 147, 0x3},   {4, 0, 0x6},      {5, 0, 0x6},      {13, 4, 0x11},
        {96, 0, 0x42},   {97, 96, 0x62},   {98, 96, 0x52},   {99, 0, 0x42},
        {100, 99, 0x63}, {101, 99, 0x52},  {114, 0, 0x0},    {115, 114, 0x22},
        {116, 114, 0x10}, {119, 4, 0x46},  {123, 128, 0x62}, {128, 0, 0x42},
        {129, 128, 0x52}, {143, 5, 0x0},   {147, 146, 0x20}, {57, 17, 0x3},
        {17, 16, 0x3},   {16, 0, 0x3},
    };
    for (const auto& r : rows) a::setDbcFallbackRow(r.id, r.fallback, r.flags);
}

using anim_test::hasIds;

M2Sequence seq(uint32_t id, uint32_t variation, int16_t freq, int16_t next, uint32_t flags = 0x20) {
    M2Sequence s{};
    s.id = id;
    s.variationIndex = variation;
    s.duration = 1000;
    s.flags = flags;
    s.frequency = freq;
    s.nextAnimation = next;
    s.aliasNext = 0;
    return s;
}

}  // namespace

TEST_CASE("Fallback: the id itself plays forward", "[anim_selection]") {
    loadAnimationDataRows();
    const auto r = a::resolveFallback(hasIds({0, 96, 97}), 97, 0);
    CHECK(r.found);
    CHECK(r.id == 97);
    CHECK(r.mode == a::PlayMode::Forward);
}

TEST_CASE("Fallback: Flags 0x20 holds the fallback on its last frame", "[anim_selection]") {
    loadAnimationDataRows();
    // SitGround without its own sequence: SitGroundDown, held where it ends.
    auto r = a::resolveFallback(hasIds({0, 96}), a::SITTING, 0);
    CHECK(r.found);
    CHECK(r.id == a::SIT_GROUND_DOWN);
    CHECK(r.mode == a::PlayMode::HoldLast);
    // Sleep -> SleepDown, KneelLoop -> KneelStart, UseStandingLoop -> UseStandingStart.
    r = a::resolveFallback(hasIds({0, 99}), a::SLEEP, 0);
    CHECK((r.id == a::SLEEP_DOWN && r.mode == a::PlayMode::HoldLast));
    r = a::resolveFallback(hasIds({0, 114}), a::KNEEL_LOOP, 0);
    CHECK((r.id == a::KNEEL_START && r.mode == a::PlayMode::HoldLast));
    r = a::resolveFallback(hasIds({0, 128}), 123, 0);
    CHECK((r.id == 128 && r.mode == a::PlayMode::HoldLast));
}

TEST_CASE("Fallback: Flags 0x10 plays the fallback backwards", "[anim_selection]") {
    loadAnimationDataRows();
    // Standing up is sitting down played in reverse.
    auto r = a::resolveFallback(hasIds({0, 96}), a::SIT_GROUND_UP, 0);
    CHECK((r.found && r.id == a::SIT_GROUND_DOWN && r.mode == a::PlayMode::Reverse));
    r = a::resolveFallback(hasIds({0, 99}), a::SLEEP_UP, 0);
    CHECK((r.id == a::SLEEP_DOWN && r.mode == a::PlayMode::Reverse));
    r = a::resolveFallback(hasIds({0, 114}), a::KNEEL_END, 0);
    CHECK((r.id == a::KNEEL_START && r.mode == a::PlayMode::Reverse));
}

TEST_CASE("Fallback: plain chains play forward", "[anim_selection]") {
    loadAnimationDataRows();
    // Special1H -> Attack1H -> AttackUnarmed; StealthWalk -> Walk; Sprint -> Run.
    auto r = a::resolveFallback(hasIds({0, 16}), a::SPECIAL_1H, 0);
    CHECK((r.found && r.id == a::ATTACK_UNARMED && r.mode == a::PlayMode::Forward));
    r = a::resolveFallback(hasIds({0, 4}), a::STEALTH_WALK, 0);
    CHECK((r.id == a::WALK && r.mode == a::PlayMode::Forward));
    r = a::resolveFallback(hasIds({0, 5}), a::SPRINT, 0);
    CHECK((r.id == a::RUN && r.mode == a::PlayMode::Forward));
    // Walkbackwards' own Flags (0x11) carry 0x10: Walk played backwards.
    r = a::resolveFallback(hasIds({0, 4}), a::WALK_BACKWARDS, 0);
    CHECK((r.id == a::WALK && r.mode == a::PlayMode::Reverse));
}

TEST_CASE("Fallback: nothing in the chain is Stand, then Closed, then the first sequence",
          "[anim_selection]") {
    loadAnimationDataRows();
    // SitGroundUp's chain ends at Stand itself (SitGroundDown's Fallback is
    // 0), reached backwards: what a model with neither plays.
    auto r = a::resolveFallback(hasIds({0, 5}), a::SIT_GROUND_UP, 5);
    CHECK((r.found && r.id == a::STAND && r.mode == a::PlayMode::Reverse));
    // An id with no row: the defaults.
    a::setDbcFallbackRow(300, 300, 0);
    r = a::resolveFallback(hasIds({0, 5}), 300, 5);
    CHECK_FALSE(r.found);
    CHECK(r.id == a::STAND);
    CHECK(r.mode == a::PlayMode::Forward);
    r = a::resolveFallback(hasIds({a::CLOSED, 5}), 300, 5);
    CHECK(r.id == a::CLOSED);
    r = a::resolveFallback(hasIds({42}), 300, 42);
    CHECK(r.id == 42);
}

TEST_CASE("Fallback: a cycle in the table ends", "[anim_selection]") {
    a::setDbcFallbackRow(400, 401, 0);
    a::setDbcFallbackRow(401, 400, 0);
    const auto r = a::resolveFallback(hasIds({0}), 400, 0);
    CHECK_FALSE(r.found);
    CHECK(r.id == a::STAND);
}

TEST_CASE("Play modes sample the right time", "[anim_selection]") {
    CHECK(a::playModeSampleTime(a::PlayMode::Forward, 250.0f, 1000.0f) == 250.0f);
    CHECK(a::playModeSampleTime(a::PlayMode::Reverse, 250.0f, 1000.0f) == 750.0f);
    CHECK(a::playModeSampleTime(a::PlayMode::Reverse, 1500.0f, 1000.0f) == 0.0f);
    CHECK(a::playModeSampleTime(a::PlayMode::HoldFirst, 250.0f, 1000.0f) == 0.0f);
    CHECK(a::playModeSampleTime(a::PlayMode::HoldLast, 250.0f, 1000.0f) == 1000.0f);
}

TEST_CASE("Variation roll walks the chain by frequency (0x826E60)", "[anim_selection]") {
    // HumanMale's Stand: 9830, 2184, 2184, 18569 along variationNext 0->1->2->3.
    std::vector<M2Sequence> seqs = {
        seq(0, 0, 9830, 2), seq(4, 0, 32767, -1), seq(0, 1, 2184, 3), seq(0, 2, 2184, 4),
        seq(0, 3, 18569, -1),
    };
    const std::vector<wowee::pipeline::M2Bone> noBones;
    CHECK(a::rollVariationStock(seqs, noBones, 0, 0) == 0);
    CHECK(a::rollVariationStock(seqs, noBones, 0, 9829) == 0);
    CHECK(a::rollVariationStock(seqs, noBones, 0, 9830) == 2);
    CHECK(a::rollVariationStock(seqs, noBones, 0, 12013) == 2);
    CHECK(a::rollVariationStock(seqs, noBones, 0, 12014) == 3);
    CHECK(a::rollVariationStock(seqs, noBones, 0, 14198) == 4);
    CHECK(a::rollVariationStock(seqs, noBones, 0, 32766) == 4);
    // Past every weight (they sum to 32767): variation 0 stays.
    CHECK(a::rollVariationStock(seqs, noBones, 0, 32767) == 0);
    // One variation: always it. No such id: -1.
    CHECK(a::rollVariationStock(seqs, noBones, 4, 32767) == 1);
    CHECK(a::rollVariationStock(seqs, noBones, 99, 0) == -1);
}

TEST_CASE("Attack variations are rolled too", "[anim_selection]") {
    // HumanMale Attack1H: two swings at 16383 each.
    std::vector<M2Sequence> seqs = {seq(17, 0, 16383, 1, 0xa1), seq(17, 1, 16383, -1, 0xa1)};
    const std::vector<wowee::pipeline::M2Bone> noBones;
    CHECK(a::rollVariationStock(seqs, noBones, 17, 100) == 0);
    CHECK(a::rollVariationStock(seqs, noBones, 17, 20000) == 1);
}

TEST_CASE("Replay laps (0x826B00)", "[anim_selection]") {
    M2Sequence s = seq(0, 1, 2184, -1);
    s.replayMin = 2;
    s.replayMax = 7;
    CHECK(a::stockReplayLaps(s, 0) == 2);
    CHECK(a::stockReplayLaps(s, 0x7FFF) == 6);
    s.replayMin = 0;
    s.replayMax = 0;
    CHECK(a::stockReplayLaps(s, 12345) == 1);
}

TEST_CASE("Moving animations keep their phase and play at speed / MoveSpeed", "[anim_selection]") {
    CHECK(a::isMovingAnimBehavior(a::WALK));
    CHECK(a::isMovingAnimBehavior(a::RUN));
    CHECK(a::isMovingAnimBehavior(a::SPRINT));
    CHECK(a::isMovingAnimBehavior(a::WALK_BACKWARDS));
    CHECK(a::isMovingAnimBehavior(a::SWIM));
    CHECK(a::isMovingAnimBehavior(a::STEALTH_WALK));
    CHECK_FALSE(a::isMovingAnimBehavior(a::STAND));
    CHECK_FALSE(a::isMovingAnimBehavior(a::ATTACK_1H));
    CHECK_FALSE(a::isMovingAnimBehavior(a::EMOTE_DANCE));

    // Walk (1000 ms) half-way becomes Run (667 ms) half-way.
    CHECK(a::carriedPhaseMs(500.0f, 1000.0f, 667.0f) == Catch::Approx(333.5f));
    CHECK(a::carriedPhaseMs(0.0f, 1000.0f, 667.0f) == 0.0f);
    CHECK(a::carriedPhaseMs(999.0f, 1000.0f, 0.0f) == 0.0f);

    // A horse's Run (MoveSpeed 9.03) at 14 yd/s plays at 1.55.
    CHECK(a::moveSpeedRate(14.0f, 9.03f) == Catch::Approx(1.5504f).epsilon(0.001));
    CHECK(a::moveSpeedRate(7.0f, 0.0f) == 1.0f);
    CHECK(a::moveSpeedRate(4.5f, -3.0f) == Catch::Approx(1.5f));
}

TEST_CASE("Stand-state transitions (0x71E1F0)", "[anim_selection]") {
    CHECK(a::standStateTransitionAnim(0, 1) == a::SIT_GROUND_DOWN);
    CHECK(a::standStateTransitionAnim(1, 0) == a::SIT_GROUND_UP);
    CHECK(a::standStateTransitionAnim(0, 3) == a::SLEEP_DOWN);
    CHECK(a::standStateTransitionAnim(3, 0) == a::SLEEP_UP);
    CHECK(a::standStateTransitionAnim(0, 8) == a::KNEEL_START);
    CHECK(a::standStateTransitionAnim(8, 0) == a::KNEEL_END);
    // From a chair to the ground: straight to the loop.
    CHECK(a::standStateTransitionAnim(4, 1) == 0);
    CHECK(a::standStateTransitionAnim(0, 5) == 0);
    CHECK(a::standStateLoopAnim(1) == a::SITTING);
    CHECK(a::standStateLoopAnim(3) == a::SLEEP);
    CHECK(a::standStateLoopAnim(8) == a::KNEEL_LOOP);
    // UNIT_STAND_STATE_SIT_CHAIR has no animation: the unit stands.
    CHECK(a::standStateLoopAnim(2) == 0);
}

namespace {
AnimCapabilitySet humanCaps() {
    AnimCapabilitySet c;
    c.resolvedStand = a::STAND;
    c.resolvedWalk = a::WALK;
    c.resolvedRun = a::RUN;
    c.resolvedSprint = a::SPRINT;
    c.resolvedWalkBackwards = a::WALK_BACKWARDS;
    c.resolvedStealthIdle = a::STEALTH_STAND;
    c.resolvedStealthWalk = a::STEALTH_WALK;
    c.resolvedSwim = a::SWIM;
    c.resolvedSwimIdle = a::SWIM_IDLE;
    return c;
}

uint32_t moveAnim(CharacterAnimator& ca, float speed, bool backward = false) {
    CharacterAnimator::FrameInput fi;
    fi.moving = true;
    fi.movingForward = !backward;
    fi.movingBackward = backward;
    fi.sprinting = true;  // the old run-pace flag: the speed overrides it
    fi.speed = speed;
    fi.walkSpeed = 2.5f;
    ca.setFrameInput(fi);
    ca.update(0.016f);
    return ca.getLastOutput().animId;
}
}  // namespace

TEST_CASE("The local character's pace follows its speed", "[anim_selection]") {
    CharacterAnimator ca;
    ca.setCapabilities(humanCaps());
    CHECK(moveAnim(ca, 7.0f) == a::RUN);
    CHECK(moveAnim(ca, 11.9f) == a::SPRINT);  // Sprint, Dash: over 11 yd/s
    CHECK(moveAnim(ca, 9.1f) == a::RUN);      // Aspect of the Cheetah: still a run
    CHECK(moveAnim(ca, 3.5f) == a::WALK);     // slowed to half: a walk
    CHECK(moveAnim(ca, 4.5f, true) == a::WALK_BACKWARDS);
}

TEST_CASE("Stealth walks at every pace, never backwards", "[anim_selection]") {
    CharacterAnimator ca;
    ca.setCapabilities(humanCaps());
    ca.setStealthed(true);
    CHECK(moveAnim(ca, 7.0f) == a::STEALTH_WALK);
    CHECK(moveAnim(ca, 12.0f) == a::STEALTH_WALK);
    CHECK(moveAnim(ca, 4.5f, true) == a::WALK_BACKWARDS);
}
