// Stock 3.3.5a mount rules (docs/re/mounts-taxi-vehicles.md): the mount's
// movement animation, the pace, Fly on a taxi spline, the rider's pose.
#include <catch_amalgamated.hpp>

#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/mount_fsm.hpp"
#include "rendering/animation/mount_rules.hpp"

using namespace wowee::rendering;
namespace a = wowee::rendering::anim;

namespace {
// What gryphon_mount.m2 carries (Stand, Walk, Run, Sprint, the swim set,
// JumpStart, Fly, Hover, ...): the probe's answers for it.
AnimCapabilitySet flyingMountCaps() {
    AnimCapabilitySet c;
    c.resolvedStand = a::STAND;
    c.resolvedWalk = a::WALK;
    c.resolvedRun = a::RUN;
    c.resolvedSprint = a::SPRINT;
    c.resolvedWalkBackwards = a::WALK_BACKWARDS;
    c.resolvedJumpStart = a::JUMP_START;
    c.resolvedFall = a::FALL;
    c.resolvedJumpLandRun = a::JUMP_LAND_RUN;
    c.resolvedSwimIdle = a::SWIM_IDLE;
    c.resolvedSwim = a::SWIM;
    c.resolvedSwimLeft = a::SWIM_LEFT;
    c.resolvedSwimRight = a::SWIM_RIGHT;
    c.resolvedSwimBackwards = a::SWIM_BACKWARDS;
    return c;
}

uint32_t settle(MountFSM& fsm, MountFSM::Input in, int frames = 30) {
    uint32_t last = 0;
    in.deltaTime = 0.05f;
    for (int i = 0; i < frames; ++i) {
        auto out = fsm.evaluate(in);
        if (out.anim.valid) {
            last = out.anim.animId;
            in.currentAnimId = last;
            in.haveAnimState = true;
            in.currentAnimDuration = 1000.0f;
            in.currentAnimTime = 0.0f;
        }
    }
    return last;
}
}  // namespace

TEST_CASE("Mount pace: Sprint above 11, Run above twice walk (0x717050)", "[mount]") {
    CHECK(a::mountGroundPace(14.0f, 2.5f) == a::MountPace::Sprint);   // 100% mount
    CHECK(a::mountGroundPace(11.2f, 2.5f) == a::MountPace::Sprint);   // 60% mount
    CHECK(a::mountGroundPace(11.0f, 2.5f) == a::MountPace::Run);      // not above 11
    CHECK(a::mountGroundPace(7.0f, 2.5f) == a::MountPace::Run);
    CHECK(a::mountGroundPace(5.0f, 2.5f) == a::MountPace::Walk);      // not above 2x walk
    CHECK(a::mountGroundPace(2.5f, 2.5f) == a::MountPace::Walk);
    CHECK(a::mountGroundPace(-1.0f, 2.5f) == a::MountPace::Run);      // unknown
}

TEST_CASE("Flying spline means Fly (0x4F5290)", "[mount]") {
    CHECK(a::onFlyingSpline(0x2000));
    CHECK(a::onFlyingSpline(0x2000 | 0x40000));
    CHECK_FALSE(a::onFlyingSpline(0x2000 | 0x400));   // No_Spline
    CHECK_FALSE(a::onFlyingSpline(0x40000));          // Catmull-Rom alone
    CHECK_FALSE(a::onFlyingSpline(0));
}

TEST_CASE("Rider pose: Mount unless the mount aura's state kit names one", "[mount]") {
    CHECK(a::mountRiderAnim(-1) == a::MOUNT);
    CHECK(a::mountRiderAnim(484) == a::RECLINED_MOUNT);  // Mechano-hog
    CHECK(a::mountRiderAnim(120) == 120u);               // flying carpets
}

TEST_CASE("MountFSM: ground pace, backpedal and standing", "[mount]") {
    MountFSM fsm;
    fsm.configure(flyingMountCaps());
    MountFSM::Input in;
    in.moving = true;
    in.speed = 14.0f;
    CHECK(settle(fsm, in) == a::SPRINT);
    in.speed = 7.0f;
    CHECK(settle(fsm, in) == a::RUN);
    in.speed = 2.5f;
    CHECK(settle(fsm, in) == a::WALK);
    in.speed = 4.5f;
    in.movingBackward = true;
    CHECK(settle(fsm, in) == a::WALK_BACKWARDS);
    MountFSM::Input still;
    CHECK(settle(fsm, still) == a::STAND);
}

TEST_CASE("MountFSM: a flying mount flies on the swim set", "[mount]") {
    MountFSM fsm;
    fsm.configure(flyingMountCaps());
    MountFSM::Input in;
    in.flying = true;
    in.grounded = false;
    CHECK(settle(fsm, in) == a::SWIM_IDLE);     // hovering in place
    in.moving = true;
    in.speed = 14.0f;
    CHECK(settle(fsm, in) == a::SWIM);          // forward
    in.strafeLeft = true;
    CHECK(settle(fsm, in) == a::SWIM_LEFT);
    in.strafeLeft = false;
    in.movingBackward = true;
    CHECK(settle(fsm, in) == a::SWIM_BACKWARDS);
}

TEST_CASE("MountFSM: Fly on a taxi spline, Hover standing on one", "[mount]") {
    MountFSM fsm;
    fsm.configure(flyingMountCaps());
    MountFSM::Input in;
    in.flyingSpline = true;
    in.moving = true;
    auto out = fsm.evaluate(in);
    REQUIRE(out.anim.valid);
    CHECK(out.anim.animId == a::FLY);
    CHECK(out.anim.loop);
    in.moving = false;
    CHECK(fsm.evaluate(in).anim.animId == a::HOVER);
}

TEST_CASE("MountFSM: jump is the unit's jump, no rear-up", "[mount]") {
    MountFSM fsm;
    fsm.configure(flyingMountCaps());
    MountFSM::Input in;
    in.deltaTime = 0.05f;
    // Standing jump: JumpStart, flowing into the jump loop - never MountSpecial.
    in.grounded = false;
    in.jumping = true;
    auto out = fsm.evaluate(in);
    REQUIRE(out.anim.valid);
    CHECK(out.anim.animId == a::JUMP_START);
    CHECK_FALSE(out.anim.loop);
}

TEST_CASE("MountFSM: MountSpecial once, standing, on request", "[mount]") {
    MountFSM fsm;
    fsm.configure(flyingMountCaps());
    MountFSM::Input in;
    settle(fsm, in);
    fsm.requestSpecial();
    auto out = fsm.evaluate(in);
    REQUIRE(out.anim.valid);
    CHECK(out.anim.animId == a::MOUNT_SPECIAL);
    CHECK_FALSE(out.anim.loop);
    // Playing: stays.
    in.haveAnimState = true;
    in.currentAnimId = a::MOUNT_SPECIAL;
    in.currentAnimDuration = 1000.0f;
    in.currentAnimTime = 300.0f;
    CHECK_FALSE(fsm.evaluate(in).anim.valid);
    // Done: back to Stand.
    in.currentAnimTime = 999.0f;
    CHECK(settle(fsm, in, 3) == a::STAND);
}
