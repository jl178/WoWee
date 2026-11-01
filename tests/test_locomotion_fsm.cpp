// LocomotionFSM unit tests
#include <catch_amalgamated.hpp>
#include "rendering/animation/locomotion_fsm.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/unit_locomotion.hpp"

using namespace wowee::rendering;
namespace anim = wowee::rendering::anim;

// Helper: create a capability set with basic locomotion resolved
static AnimCapabilitySet makeLocoCaps() {
    AnimCapabilitySet caps;
    caps.resolvedStand = anim::STAND;
    caps.resolvedWalk = anim::WALK;
    caps.resolvedRun = anim::RUN;
    caps.resolvedSprint = anim::SPRINT;
    caps.resolvedWalkBackwards = anim::WALK_BACKWARDS;
    caps.resolvedJumpStart = anim::JUMP_START;
    caps.resolvedJump = anim::JUMP;
    caps.resolvedJumpEnd = anim::JUMP_END;
    caps.resolvedSwimIdle = anim::SWIM_IDLE;
    caps.resolvedSwim = anim::SWIM;
    caps.hasStand = true;
    caps.hasWalk = true;
    caps.hasRun = true;
    caps.hasSprint = true;
    caps.hasWalkBackwards = true;
    caps.hasJump = true;
    caps.hasSwim = true;
    return caps;
}

static LocomotionFSM::Input idle() {
    LocomotionFSM::Input in;
    in.deltaTime = 0.016f;
    return in;
}

TEST_CASE("LocomotionFSM: IDLE → WALK on move start (non-sprinting)", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    auto in = idle();
    in.moving = true;
    auto out = fsm.resolve(in, caps);

    REQUIRE(out.valid);
    REQUIRE(out.animId == anim::WALK);
    REQUIRE(out.loop == true);
    CHECK(fsm.getState() == LocomotionFSM::State::WALK);
}

TEST_CASE("LocomotionFSM: IDLE → RUN on move start (sprinting)", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    auto in = idle();
    in.moving = true;
    in.sprinting = true;
    auto out = fsm.resolve(in, caps);

    REQUIRE(out.valid);
    REQUIRE(out.animId == anim::RUN);
    CHECK(fsm.getState() == LocomotionFSM::State::RUN);
}

TEST_CASE("LocomotionFSM: WALK → IDLE on move stop (after grace)", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    // Start walking
    auto in = idle();
    in.moving = true;
    fsm.resolve(in, caps);
    CHECK(fsm.getState() == LocomotionFSM::State::WALK);

    // Stop moving - grace timer keeps walk for a bit
    in.moving = false;
    in.deltaTime = 0.2f; // > grace period (0.12s)
    auto out = fsm.resolve(in, caps);

    CHECK(fsm.getState() == LocomotionFSM::State::IDLE);
}

TEST_CASE("LocomotionFSM: WALK → JUMP_START on jump", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    // Start walking
    auto in = idle();
    in.moving = true;
    fsm.resolve(in, caps);

    // Jump
    in.jumping = true;
    in.grounded = false;
    auto out = fsm.resolve(in, caps);

    REQUIRE(out.valid);
    REQUIRE(out.animId == anim::JUMP_START);
    CHECK(fsm.getState() == LocomotionFSM::State::JUMP_START);
}

TEST_CASE("LocomotionFSM: SWIM_IDLE → SWIM on move start while swimming", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    // Enter swim idle
    auto in = idle();
    in.swimming = true;
    fsm.resolve(in, caps);
    CHECK(fsm.getState() == LocomotionFSM::State::SWIM_IDLE);

    // Start swimming
    in.moving = true;
    auto out = fsm.resolve(in, caps);

    REQUIRE(out.valid);
    REQUIRE(out.animId == anim::SWIM);
    CHECK(fsm.getState() == LocomotionFSM::State::SWIM);
}

TEST_CASE("LocomotionFSM: backward walking resolves WALK_BACKWARDS", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    auto in = idle();
    in.moving = true;
    in.movingBackward = true;
    auto out = fsm.resolve(in, caps);

    REQUIRE(out.valid);
    // Should use WALK_BACKWARDS when available
    CHECK(out.animId == anim::WALK_BACKWARDS);
}

TEST_CASE("LocomotionFSM: STAY when WALK_BACKWARDS missing from caps", "[locomotion]") {
    LocomotionFSM fsm;
    AnimCapabilitySet caps;
    caps.resolvedStand = anim::STAND;
    caps.resolvedWalk = anim::WALK;
    caps.hasStand = true;
    caps.hasWalk = true;
    // No WALK_BACKWARDS in caps

    auto in = idle();
    in.moving = true;
    in.movingBackward = true;
    auto out = fsm.resolve(in, caps);

    // Should still resolve something - falls back to walk
    REQUIRE(out.valid);
}

TEST_CASE("LocomotionFSM: reset restores IDLE", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    auto in = idle();
    in.moving = true;
    fsm.resolve(in, caps);
    CHECK(fsm.getState() == LocomotionFSM::State::WALK);

    fsm.reset();
    CHECK(fsm.getState() == LocomotionFSM::State::IDLE);
}

// The stock client's airborne phases: a jump is JumpStart, Jump, JumpEnd;
// walking off an edge is Fall; landing while running is JumpLandRun.

static AnimCapabilitySet makeFullAirCaps() {
    auto caps = makeLocoCaps();
    caps.resolvedFall = anim::FALL;
    caps.resolvedJumpLandRun = anim::JUMP_LAND_RUN;
    return caps;
}

TEST_CASE("LocomotionFSM: leaving the ground without a jump is FALL", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeFullAirCaps();
    auto in = idle();
    in.moving = true;
    in.sprinting = true;
    fsm.resolve(in, caps);
    in.grounded = false;
    in.jumping = false;
    // A blip off the ground is not a fall.
    in.deltaTime = 0.1f;
    fsm.resolve(in, caps);
    REQUIRE(fsm.getState() == LocomotionFSM::State::RUN);
    in.deltaTime = 0.2f;
    auto out = fsm.resolve(in, caps);
    REQUIRE(out.animId == anim::FALL);
    REQUIRE(out.loop);
    CHECK(fsm.getState() == LocomotionFSM::State::FALL);
    // Landing from a fall still plays the landing one-shot.
    in.grounded = true;
    out = fsm.resolve(in, caps);
    CHECK(fsm.getState() == LocomotionFSM::State::JUMP_END);
}

TEST_CASE("LocomotionFSM: a model without Fall uses the Jump loop", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();
    auto in = idle();
    in.grounded = false;
    in.deltaTime = 0.3f;
    auto out = fsm.resolve(in, caps);
    REQUIRE(out.animId == anim::JUMP);
    CHECK(fsm.getState() == LocomotionFSM::State::FALL);
}

TEST_CASE("LocomotionFSM: landing while running plays JumpLandRun through, then runs", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeFullAirCaps();
    auto in = idle();
    in.moving = true;
    in.sprinting = true;
    fsm.resolve(in, caps);
    in.grounded = false;
    in.jumping = true;
    fsm.resolve(in, caps);
    REQUIRE(fsm.getState() == LocomotionFSM::State::JUMP_START);
    // Past the apex and down.
    in.jumping = false;
    in.haveAnimState = true;
    in.currentAnimId = anim::JUMP_START;
    in.currentAnimDuration = 0.5f;
    in.currentAnimTime = 0.5f;
    fsm.resolve(in, caps);
    REQUIRE(fsm.getState() == LocomotionFSM::State::JUMP_MID);
    in.grounded = true;
    auto out = fsm.resolve(in, caps);
    REQUIRE(out.animId == anim::JUMP_LAND_RUN);
    REQUIRE_FALSE(out.loop);
    CHECK(fsm.getState() == LocomotionFSM::State::JUMP_END);
    // Still landing while the one-shot runs...
    in.currentAnimId = anim::JUMP_LAND_RUN;
    in.currentAnimDuration = 0.4f;
    in.currentAnimTime = 0.1f;
    out = fsm.resolve(in, caps);
    CHECK(fsm.getState() == LocomotionFSM::State::JUMP_END);
    // ...and running once it is done.
    in.currentAnimTime = 0.4f;
    out = fsm.resolve(in, caps);
    REQUIRE(out.animId == anim::RUN);
    CHECK(fsm.getState() == LocomotionFSM::State::RUN);
}

TEST_CASE("LocomotionFSM: landing while running without JumpLandRun runs at once", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();
    auto in = idle();
    in.moving = true;
    in.sprinting = true;
    in.grounded = false;
    in.deltaTime = 0.3f;
    fsm.resolve(in, caps);
    in.grounded = true;
    in.deltaTime = 0.016f;
    fsm.resolve(in, caps);  // the landing frame itself
    auto out = fsm.resolve(in, caps);
    REQUIRE(out.animId == anim::RUN);
    CHECK(fsm.getState() == LocomotionFSM::State::RUN);
}

TEST_CASE("LocomotionFSM: landing still plays JumpEnd through", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeFullAirCaps();
    auto in = idle();
    in.grounded = false;
    in.jumping = true;
    fsm.resolve(in, caps);
    in.jumping = false;
    in.grounded = true;
    in.haveAnimState = true;
    in.currentAnimId = anim::JUMP_START;
    in.currentAnimDuration = 0.5f;
    in.currentAnimTime = 0.5f;
    auto out = fsm.resolve(in, caps);
    REQUIRE(out.animId == anim::JUMP_END);
    in.currentAnimId = anim::JUMP_END;
    in.currentAnimTime = 0.0f;
    out = fsm.resolve(in, caps);
    CHECK(fsm.getState() == LocomotionFSM::State::JUMP_END);
    in.currentAnimTime = 0.5f;
    out = fsm.resolve(in, caps);
    REQUIRE(out.animId == anim::STAND);
    CHECK(fsm.getState() == LocomotionFSM::State::IDLE);
}

TEST_CASE("LocomotionFSM: jumping during the landing restarts JumpStart", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeFullAirCaps();
    auto in = idle();
    in.moving = true;
    in.sprinting = true;
    in.grounded = false;
    in.jumping = true;
    fsm.resolve(in, caps);
    in.jumping = false;
    in.grounded = true;
    in.haveAnimState = true;
    in.currentAnimId = anim::JUMP_START;
    in.currentAnimDuration = 0.5f;
    in.currentAnimTime = 0.5f;
    fsm.resolve(in, caps);
    REQUIRE(fsm.getState() == LocomotionFSM::State::JUMP_END);
    // Mid-landing, off the ground again on a new jump.
    in.currentAnimId = anim::JUMP_LAND_RUN;
    in.currentAnimDuration = 0.4f;
    in.currentAnimTime = 0.1f;
    in.grounded = false;
    in.jumping = true;
    auto out = fsm.resolve(in, caps);
    REQUIRE(out.animId == anim::JUMP_START);
    CHECK(fsm.getState() == LocomotionFSM::State::JUMP_START);
}

TEST_CASE("LocomotionFSM: a new jump seen without its landing restarts JumpStart", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeFullAirCaps();
    auto in = idle();
    in.grounded = false;
    in.jumping = true;
    fsm.resolve(in, caps);
    in.jumping = false;
    in.haveAnimState = true;
    in.currentAnimId = anim::JUMP_START;
    in.currentAnimDuration = 0.5f;
    in.currentAnimTime = 0.5f;
    fsm.resolve(in, caps);
    REQUIRE(fsm.getState() == LocomotionFSM::State::JUMP_MID);
    // Landed and jumped between two updates: rising again, never grounded.
    in.currentAnimId = anim::JUMP;
    in.jumping = true;
    auto out = fsm.resolve(in, caps);
    REQUIRE(out.animId == anim::JUMP_START);
    CHECK(fsm.getState() == LocomotionFSM::State::JUMP_START);
}

// ── Turning in place (stock CGUnit_C 0x0073E2C0) ──────────────────────────

static AnimCapabilitySet makeShuffleCaps() {
    auto caps = makeLocoCaps();
    caps.resolvedShuffleLeft = anim::SHUFFLE_LEFT;
    caps.resolvedShuffleRight = anim::SHUFFLE_RIGHT;
    return caps;
}

TEST_CASE("LocomotionFSM: turning in place shuffles, left before right", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeShuffleCaps();
    auto in = idle();
    in.turnLeft = true;
    in.turnRight = true;
    auto out = fsm.resolve(in, caps);
    REQUIRE(out.valid);
    CHECK(out.animId == anim::SHUFFLE_LEFT);
    CHECK(out.loop);
    CHECK(fsm.getState() == LocomotionFSM::State::IDLE);

    in.turnLeft = false;
    out = fsm.resolve(in, caps);
    CHECK(out.animId == anim::SHUFFLE_RIGHT);
}

TEST_CASE("LocomotionFSM: a shuffle plays its lap out after the turn stops", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeShuffleCaps();
    auto in = idle();
    in.turnLeft = true;
    REQUIRE(fsm.resolve(in, caps).animId == anim::SHUFFLE_LEFT);

    // The turn (a one-frame body step) is over; the lap is 500 ms.
    in.turnLeft = false;
    in.haveAnimState = true;
    in.currentAnimId = anim::SHUFFLE_LEFT;
    in.currentAnimDuration = 500.0f;
    in.currentAnimTime = 16.0f;
    CHECK(fsm.resolve(in, caps).animId == anim::SHUFFLE_LEFT);
    in.currentAnimTime = 250.0f;
    CHECK(fsm.resolve(in, caps).animId == anim::SHUFFLE_LEFT);
    // Within a frame of its end: back to Stand.
    in.currentAnimTime = 495.0f;
    CHECK(fsm.resolve(in, caps).animId == anim::STAND);
}

TEST_CASE("LocomotionFSM: a shuffle ends when the clock wraps", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeShuffleCaps();
    auto in = idle();
    in.turnRight = true;
    in.haveAnimState = true;
    in.currentAnimId = anim::SHUFFLE_RIGHT;
    in.currentAnimDuration = 500.0f;
    in.currentAnimTime = 400.0f;
    REQUIRE(fsm.resolve(in, caps).animId == anim::SHUFFLE_RIGHT);
    in.turnRight = false;
    in.currentAnimTime = 10.0f;  // wrapped past the end between two frames
    CHECK(fsm.resolve(in, caps).animId == anim::STAND);
}

TEST_CASE("LocomotionFSM: no shuffle on a model without one", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();
    auto in = idle();
    in.turnLeft = true;
    CHECK(fsm.resolve(in, caps).animId == anim::STAND);
}

TEST_CASE("LocomotionFSM: moving off cancels the shuffle", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeShuffleCaps();
    auto in = idle();
    in.turnLeft = true;
    REQUIRE(fsm.resolve(in, caps).animId == anim::SHUFFLE_LEFT);
    in.turnLeft = false;
    in.moving = true;
    in.sprinting = true;
    CHECK(fsm.resolve(in, caps).animId == anim::RUN);
    in.moving = false;
    in.sprinting = false;
    in.deltaTime = 1.0f;  // past the stop grace
    CHECK(fsm.resolve(in, caps).animId == anim::STAND);
}

// ── Remote units from their movement flags (unit_locomotion.hpp) ─────────────

TEST_CASE("UnitLocomotion: a backpedalling player walks backwards", "[locomotion]") {
    anim::UnitLocomotion u;
    u.caps = makeFullAirCaps();
    anim::UnitLocomotionFrame f;
    f.moveFlags = 0x2;  // BACKWARD
    f.moving = true;
    f.dt = 0.016f;
    auto out = u.fsm.resolve(anim::unitLocomotionInput(u, f), u.caps);
    CHECK(out.animId == anim::WALK_BACKWARDS);
}

TEST_CASE("UnitLocomotion: a remote jump plays JumpStart, Jump, JumpLandRun", "[locomotion]") {
    anim::UnitLocomotion u;
    u.caps = makeFullAirCaps();
    anim::UnitLocomotionFrame f;
    f.moving = true;
    f.dt = 0.016f;
    f.moveFlags = 0x1;  // running forward
    REQUIRE(u.fsm.resolve(anim::unitLocomotionInput(u, f), u.caps).animId == anim::RUN);

    u.jumpPending = true;  // MSG_MOVE_JUMP
    f.moveFlags = 0x1 | 0x1000;
    auto in = anim::unitLocomotionInput(u, f);
    CHECK(in.jumping);
    CHECK_FALSE(in.grounded);
    REQUIRE(u.fsm.resolve(in, u.caps).animId == anim::JUMP_START);

    in = anim::unitLocomotionInput(u, f);
    in.haveAnimState = true;
    in.currentAnimId = anim::JUMP_START;
    in.currentAnimDuration = 400.0f;
    in.currentAnimTime = 100.0f;
    u.fsm.resolve(in, u.caps);
    in.currentAnimId = anim::JUMP;  // the one-shot handed over to the loop
    CHECK(u.fsm.resolve(in, u.caps).animId == anim::JUMP);

    f.moveFlags = 0x1;  // MSG_MOVE_FALL_LAND, still running
    in = anim::unitLocomotionInput(u, f);
    CHECK(in.grounded);
    CHECK_FALSE(u.jumpPending);
    CHECK(u.fsm.resolve(in, u.caps).animId == anim::JUMP_LAND_RUN);
}

TEST_CASE("UnitLocomotion: turn flags shuffle a unit standing still", "[locomotion]") {
    anim::UnitLocomotion u;
    u.caps = makeShuffleCaps();
    anim::UnitLocomotionFrame f;
    f.dt = 0.016f;
    f.moveFlags = 0x20;  // TURN_RIGHT
    auto in = anim::unitLocomotionInput(u, f);
    CHECK(in.turnRight);
    CHECK_FALSE(in.turnLeft);
    CHECK(u.fsm.resolve(in, u.caps).animId == anim::SHUFFLE_RIGHT);

    // A counter-clockwise body step is a turn to the left.
    anim::UnitLocomotion v;
    v.caps = makeShuffleCaps();
    v.lastBodyStep = 0.05f;
    f.moveFlags = 0;
    in = anim::unitLocomotionInput(v, f);
    CHECK(in.turnLeft);

    // Not while moving, and not over an emote or a swing.
    f.moveFlags = 0x1 | 0x10;
    f.moving = true;
    in = anim::unitLocomotionInput(v, f);
    CHECK_FALSE(in.turnLeft);
    f.moveFlags = 0x10;
    f.moving = false;
    f.idleFree = false;
    in = anim::unitLocomotionInput(v, f);
    CHECK_FALSE(in.turnLeft);
}
