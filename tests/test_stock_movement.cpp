// The stock 3.3.5a movement maths (game/stock_movement.hpp): the jump arc and
// fall, the slope limit and the speed a set of keys moves at. Values are the
// ones read from Wow.exe 12340; docs/re/movement-camera.md has the addresses.
#include <catch_amalgamated.hpp>
#include "game/stock_movement.hpp"

#include <cmath>

namespace sm = wowee::game::stock_movement;
using Catch::Approx;

TEST_CASE("the jump rises 1.64 yards and is back down 0.82 s later") {
    // v^2 / 2g with the stock 7.955547 and 19.291105.
    CHECK(sm::jumpApexHeight() == Approx(1.6404).margin(1e-3));
    const float tApex = sm::kJumpVelocity / sm::kGravity;
    CHECK(tApex == Approx(0.4124).margin(1e-3));
    // fallDistance is down-positive and starts at -v for a jump.
    CHECK(sm::fallDistance(tApex, -sm::kJumpVelocity, false) ==
          Approx(-sm::jumpApexHeight()).margin(1e-4));
    CHECK(sm::fallDistance(2.0f * tApex, -sm::kJumpVelocity, false) ==
          Approx(0.0f).margin(1e-4));
    // From the water the jump is 9.096748: 2.14 yards.
    CHECK(sm::jumpApexHeight(sm::kWaterJumpVelocity) == Approx(2.1448).margin(1e-3));
}

TEST_CASE("the per-frame fall step lands on the closed-form arc at any frame rate") {
    for (float dt : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f, 0.1f}) {
        float v = sm::kJumpVelocity;  // Wowee's sign: up positive
        float z = 0.0f;
        float t = 0.0f;
        float peak = 0.0f;
        for (int i = 0; i < 200; ++i) {
            z += sm::fallStep(v, dt, false);
            t += dt;
            peak = std::max(peak, z);
            INFO("dt " << dt << " t " << t);
            REQUIRE(z == Approx(-sm::fallDistance(t, -sm::kJumpVelocity, false)).epsilon(1e-5).margin(2e-3));
        }
        // The highest sample can only fall short of the apex by the frame's
        // own sampling, never overshoot it.
        CHECK(peak <= sm::jumpApexHeight() + 1e-4f);
        CHECK(peak > sm::jumpApexHeight() - 0.5f * sm::kGravity * dt * dt - 1e-4f);
    }
}

TEST_CASE("a fall is capped at 60.148 yd/s, and at 7 under feather fall") {
    CHECK(sm::fallSpeed(10.0f, 0.0f, false) == Approx(sm::kTerminalVelocity));
    CHECK(sm::fallSpeed(10.0f, 0.0f, true) == Approx(7.0f));
    CHECK(sm::fallSpeed(0.1f, 0.0f, false) == Approx(1.929110f).margin(1e-4));

    // Past the cap the distance grows linearly at the cap.
    const float tCap = sm::kTerminalVelocity / sm::kGravity;  // 3.118 s
    CHECK(tCap == Approx(3.1179).margin(1e-3));
    const float dCap = sm::fallDistance(tCap, 0.0f, false);
    CHECK(dCap == Approx(93.77).margin(0.05));
    CHECK(sm::fallDistance(tCap + 1.0f, 0.0f, false) ==
          Approx(dCap + sm::kTerminalVelocity).margin(1e-2));

    // Feather fall reaches its cap in 0.36 s and drifts at 7 yd/s after.
    CHECK(sm::fallDistance(2.0f, 0.0f, true) ==
          Approx(7.0f * 7.0f / (2.0f * sm::kGravity) + (2.0f - 7.0f / sm::kGravity) * 7.0f)
              .margin(1e-3));

    // The fall step obeys the cap too.
    float v = 0.0f;
    for (int i = 0; i < 1000; ++i) sm::fallStep(v, 0.016f, false);
    CHECK(v == Approx(-sm::kTerminalVelocity));
    v = 0.0f;
    for (int i = 0; i < 1000; ++i) sm::fallStep(v, 0.016f, true);
    CHECK(v == Approx(-7.0f));
}

TEST_CASE("fallTimeForDistance inverts fallDistance from rest") {
    for (float d : {0.5f, 5.0f, 13.48f, 50.0f, 93.0f, 200.0f, 1000.0f}) {
        for (bool safe : {false, true}) {
            const float t = sm::fallTimeForDistance(d, safe);
            INFO("d " << d << " safe " << safe);
            CHECK(sm::fallDistance(t, 0.0f, safe) == Approx(d).epsilon(1e-4));
        }
    }
    CHECK(sm::fallTimeForDistance(0.0f, false) == 0.0f);
}

TEST_CASE("players stand on 50 degrees, other units on 80") {
    const float n50 = std::cos(50.0f * 3.14159265f / 180.0f);
    const float n80 = std::cos(80.0f * 3.14159265f / 180.0f);
    CHECK(sm::kWalkableNormalZPlayer == Approx(n50));
    CHECK(sm::kWalkableNormalZOther == Approx(n80));
    CHECK(sm::isWalkableNormalZ(std::cos(49.0f * 3.14159265f / 180.0f), true));
    CHECK_FALSE(sm::isWalkableNormalZ(std::cos(51.0f * 3.14159265f / 180.0f), true));
    CHECK(sm::isWalkableNormalZ(std::cos(70.0f * 3.14159265f / 180.0f), false));
    CHECK_FALSE(sm::isWalkableNormalZ(std::cos(81.0f * 3.14159265f / 180.0f), false));
    CHECK(sm::kTan50 == Approx(std::tan(50.0f * 3.14159265f / 180.0f)));
    CHECK(sm::stepHeight(true) == 1.0f);
    CHECK(sm::stepHeight(true, 1.5f) == 1.5f);
    CHECK(sm::stepHeight(true, 0.5f) == 1.0f);
    CHECK(sm::stepHeight(false) == 2.0f);
}

TEST_CASE("speed for a set of keys") {
    const sm::Speeds s;
    CHECK(sm::selectSpeed(0, s) == 0.0f);
    CHECK(sm::selectSpeed(sm::kForward, s) == 7.0f);
    CHECK(sm::selectSpeed(sm::kBackward, s) == 4.5f);
    CHECK(sm::selectSpeed(sm::kStrafeLeft, s) == 7.0f);
    CHECK(sm::selectSpeed(sm::kForward | sm::kStrafeRight, s) == 7.0f);
    CHECK(sm::selectSpeed(sm::kBackward | sm::kStrafeLeft, s) == 4.5f);
    // Walking: walk speed in every direction, backward included.
    CHECK(sm::selectSpeed(sm::kWalking | sm::kForward, s) == 2.5f);
    CHECK(sm::selectSpeed(sm::kWalking | sm::kBackward, s) == 2.5f);
    CHECK(sm::selectSpeed(sm::kWalking | sm::kStrafeLeft, s) == 2.5f);
    // A forced-walk recompute (a drift started in the air).
    CHECK(sm::selectSpeed(sm::kForward, s, true) == 2.5f);
    // Swimming and flying have their own pairs, and walking does not matter.
    CHECK(sm::selectSpeed(sm::kSwimming | sm::kForward | sm::kWalking, s) == Approx(4.72222f));
    CHECK(sm::selectSpeed(sm::kSwimming | sm::kBackward, s) == 2.5f);
    CHECK(sm::selectSpeed(sm::kFlying | sm::kForward, s) == 7.0f);
    CHECK(sm::selectSpeed(sm::kFlying | sm::kBackward, s) == 4.5f);
    CHECK(sm::selectSpeed(sm::kFlying | sm::kAscending, s) == 7.0f);
    // A back speed above the forward one is not used (min).
    sm::Speeds slowed;
    slowed.run = 3.0f;
    CHECK(sm::selectSpeed(sm::kBackward, slowed) == 3.0f);
    CHECK(sm::selectSpeed(sm::kWalking | sm::kForward, slowed) == 2.5f);
    slowed.run = 2.0f;
    CHECK(sm::selectSpeed(sm::kWalking | sm::kForward, slowed) == 2.0f);
}

TEST_CASE("direction for a set of keys: diagonals are unit length at full speed") {
    const float f = 0.6f;  // any facing
    const float c = std::cos(f), s = std::sin(f);
    auto len = [](sm::Dir2 d) { return std::sqrt(d.x * d.x + d.y * d.y); };

    sm::Dir2 d = sm::moveDirection(sm::kForward, f);
    CHECK(d.x == Approx(c));
    CHECK(d.y == Approx(s));
    d = sm::moveDirection(sm::kBackward, f);
    CHECK(d.x == Approx(-c));
    CHECK(d.y == Approx(-s));
    d = sm::moveDirection(sm::kStrafeLeft, f);  // facing + 90
    CHECK(d.x == Approx(std::cos(f + 1.5707963f)));
    CHECK(d.y == Approx(std::sin(f + 1.5707963f)));
    d = sm::moveDirection(sm::kStrafeRight, f);  // facing - 90
    CHECK(d.x == Approx(std::cos(f - 1.5707963f)));
    CHECK(d.y == Approx(std::sin(f - 1.5707963f)));
    d = sm::moveDirection(sm::kForward | sm::kStrafeLeft, f);  // facing + 45
    CHECK(d.x == Approx(std::cos(f + 0.7853982f)));
    CHECK(d.y == Approx(std::sin(f + 0.7853982f)));
    CHECK(len(d) == Approx(1.0f));
    d = sm::moveDirection(sm::kBackward | sm::kStrafeRight, f);  // facing + 180 + 45
    CHECK(d.x == Approx(std::cos(f + 3.1415927f + 0.7853982f)));
    CHECK(d.y == Approx(std::sin(f + 3.1415927f + 0.7853982f)));
    CHECK(len(d) == Approx(1.0f));
    // No direction: the facing, which is what a standing jump reports.
    d = sm::moveDirection(0, f);
    CHECK(d.x == Approx(c));
    CHECK(d.y == Approx(s));
}

TEST_CASE("the input votes cancel opposite keys") {
    CHECK(sm::resolveForwardBack(true, false, false, false) == 1);
    CHECK(sm::resolveForwardBack(false, true, false, false) == -1);
    CHECK(sm::resolveForwardBack(true, true, false, false) == 0);   // W+S stands
    CHECK(sm::resolveForwardBack(false, true, true, false) == 0);   // autorun+S stands
    CHECK(sm::resolveForwardBack(true, true, true, false) == 1);
    CHECK(sm::resolveForwardBack(false, true, false, true) == 0);   // both buttons + S
    CHECK(sm::resolveStrafe(true, true, false, false, false) == 0);
    CHECK(sm::resolveStrafe(false, false, true, false, false) == 0);  // A turns
    CHECK(sm::resolveStrafe(false, false, true, false, true) == 1);   // A strafes with RMB
    CHECK(sm::resolveStrafe(true, false, false, true, true) == 0);   // Q + D(RMB)
    CHECK(sm::resolveTurn(true, false, false) == 1);
    CHECK(sm::resolveTurn(true, true, false) == 0);
    CHECK(sm::resolveTurn(true, false, true) == 0);
}

TEST_CASE("keyboard turning is three quarters while moving") {
    const float pi = 3.14159265f;
    CHECK(sm::keyboardTurnRate(pi, 0) == Approx(pi));
    CHECK(sm::keyboardTurnRate(pi, sm::kTurnLeft) == Approx(pi));
    CHECK(sm::keyboardTurnRate(pi, sm::kForward) == Approx(0.75f * pi));
    CHECK(sm::keyboardTurnRate(pi, sm::kFalling) == Approx(0.75f * pi));
    CHECK(sm::keyboardTurnRate(pi, sm::kSwimming) == Approx(pi));  // treading water
    CHECK(sm::keyboardTurnRate(pi, sm::kForward, sm::kFlag2FullSpeedTurning) == Approx(pi));
}

TEST_CASE("heartbeats and facing updates") {
    CHECK_FALSE(sm::heartbeatDue(0, 10000));
    CHECK_FALSE(sm::heartbeatDue(sm::kTurnLeft, 10000));
    CHECK_FALSE(sm::heartbeatDue(sm::kSwimming, 10000));
    CHECK_FALSE(sm::heartbeatDue(sm::kForward, 499));
    CHECK(sm::heartbeatDue(sm::kForward, 500));
    CHECK(sm::heartbeatDue(sm::kFalling, 500));
    CHECK(sm::heartbeatDue(sm::kAscending, 500));

    CHECK_FALSE(sm::facingChangeWorthSending(1.0f, 1.09f));
    CHECK(sm::facingChangeWorthSending(1.0f, 1.11f));
    CHECK(sm::facingChangeWorthSending(1.0f, 0.89f));
    // Across the wrap.
    CHECK_FALSE(sm::facingChangeWorthSending(6.25f, 0.02f));
    CHECK(sm::facingChangeWorthSending(6.25f, 0.1f));
}

TEST_CASE("swimming starts three quarters of the way up the body") {
    const float human = 2.0313f;  // HumanMale CreatureModelData collision height
    CHECK(sm::swimEnterDepth(human) == Approx(1.5235f).margin(1e-3));
    CHECK(sm::swimExitDepth(human) == Approx(1.5235f - 1.0f / 36.0f).margin(1e-3));
}

TEST_CASE("the collision box comes from CreatureModelData, halved across, scaled up only") {
    // HumanMale: CollisionWidth 0.611, CollisionHeight 2.031 (fields 14 / 15).
    const sm::Collider human = sm::colliderFromModel(0.6111111f, 2.0312655f);
    CHECK(human.halfWidth == Approx(0.3056f).margin(1e-3));
    CHECK(human.height == Approx(2.0313f).margin(1e-3));
    CHECK(human.stepHeight == 1.0f);
    // A gnome is as wide as a human and half as tall; a draenei the widest.
    CHECK(sm::colliderFromModel(0.6944444f, 1.0555556f).height == Approx(1.0556f).margin(1e-3));
    CHECK(sm::colliderFromModel(0.8666667f, 2.5413f).halfWidth == Approx(0.4333f).margin(1e-3));
    // The display's model scale scales the box (0x71C110 -> 0x6E9570).
    const sm::Collider tauren = sm::colliderFromModel(0.59f, 1.653f, 1.35f);
    CHECK(tauren.halfWidth == Approx(0.59f * 1.35f * 0.5f));
    CHECK(tauren.height == Approx(1.653f * 1.35f));
    CHECK(tauren.stepHeight == 1.0f);
    // The object scale only counts when it enlarges (0x6CF350 clamps it to 1),
    // and then it is the step height too.
    const sm::Collider shrunk = sm::colliderFromModel(0.611f, 2.031f, 1.0f, 0.5f);
    CHECK(shrunk.height == Approx(2.031f));
    CHECK(shrunk.stepHeight == 1.0f);
    const sm::Collider grown = sm::colliderFromModel(0.611f, 2.031f, 1.0f, 1.5f);
    CHECK(grown.height == Approx(2.031f * 1.5f));
    CHECK(grown.halfWidth == Approx(0.611f * 1.5f * 0.5f));
    CHECK(grown.stepHeight == 1.5f);
    // Players step 1 yd, everything else 2 (0x6E8FC0).
    CHECK(sm::stepHeight(true) == 1.0f);
    CHECK(sm::stepHeight(false) == 2.0f);
}

TEST_CASE("mounted, the box is half the rider plus the mount's MountHeight") {
    // A human on a riding horse (MountHeight 1.866, display scale 1).
    CHECK(sm::mountedCollisionHeight(2.0313f, 1.0f, 1.866f) == Approx(2.8817f).margin(1e-3));
    // Never shorter than the rider on foot: a mount with no MountHeight.
    CHECK(sm::mountedCollisionHeight(2.0313f, 1.0f, 0.0f) == Approx(2.0313f));
    // The mount display's scale scales its MountHeight.
    CHECK(sm::mountedCollisionHeight(1.0556f, 2.0f, 1.0f) == Approx(0.5278f + 2.0f).margin(1e-3));
}

TEST_CASE("a swimmer floats with three quarters of its height under the surface") {
    const float human = 2.0313f;
    const float gnome = 1.0556f;
    CHECK(sm::swimFloatDepth(human) == Approx(1.5235f).margin(1e-3));
    CHECK(sm::swimFloatDepth(gnome) == Approx(0.7917f).margin(1e-3));
    // Floating is a stable state: deep enough to keep swimming, and exactly
    // at the depth that starts it.
    CHECK(sm::keepsSwimming(sm::swimFloatDepth(human), human, true, true));
    CHECK(sm::swimFloatDepth(human) == sm::swimEnterDepth(human));
}

TEST_CASE("start and stop swimming") {
    const float h = 2.0313f;
    // Wading in: deeper than 0.75 h, not before.
    CHECK_FALSE(sm::startsSwimming(1.50f, h, true, false));
    CHECK(sm::startsSwimming(1.53f, h, true, false));
    // Not on the way up out of a jump, not in liquid that is not swimmable.
    CHECK_FALSE(sm::startsSwimming(3.0f, h, true, true));
    CHECK_FALSE(sm::startsSwimming(3.0f, h, false, false));
    // Walking out: the 1/36 yd of hysteresis keeps a wader from flickering.
    CHECK(sm::keepsSwimming(1.50f, h, true, true));
    CHECK_FALSE(sm::keepsSwimming(1.49f, h, true, true));
    CHECK_FALSE(sm::keepsSwimming(5.0f, h, false, true));
    CHECK_FALSE(sm::keepsSwimming(5.0f, h, true, false));
    // A flier is not stopped by the water rule.
    CHECK(sm::keepsSwimming(0.0f, h, false, false, true));
    // A gnome swims in less than half the water a human needs.
    CHECK(sm::startsSwimming(0.80f, 1.0556f, true, false));
    CHECK_FALSE(sm::startsSwimming(0.80f, h, true, false));
}

TEST_CASE("holding ascend at the surface jumps out of the water") {
    const float h = 2.0313f;
    // At the float line, or a little under it.
    CHECK(sm::surfaceHopDue(true, sm::swimFloatDepth(h), h));
    CHECK(sm::surfaceHopDue(true, sm::swimFloatDepth(h) + 0.3f, h));
    // A third of a yard down is diving, not surfacing.
    CHECK_FALSE(sm::surfaceHopDue(true, sm::swimFloatDepth(h) + 0.34f, h));
    // Only while ascending.
    CHECK_FALSE(sm::surfaceHopDue(false, sm::swimFloatDepth(h), h));
    // The water jump clears a dock 0.6 yd above the surface from the float
    // line: it rises 2.14 yd from 1.52 under.
    CHECK(sm::jumpApexHeight(sm::kWaterJumpVelocity) - sm::swimFloatDepth(h) > 0.6f);
}

TEST_CASE("keys pressed in the air wait for the landing (0x988A20, 0x6EB3B0)") {
    const uint32_t runJump = sm::kForward | sm::kFalling;
    // Back pressed mid-jump: a pending flag, the path unchanged.
    uint32_t f = sm::startForwardBackward(runJump, /*forward=*/false);
    CHECK((f & sm::kForward) != 0);
    CHECK((f & sm::kBackward) == 0);
    CHECK((f & sm::kPendingBackward) != 0);
    // Landing: the back key takes over.
    f = sm::landWithPendingKeys(f);
    CHECK(f == sm::kBackward);

    // Forward released mid-jump: the stop waits (PENDING_STOP), applied on landing.
    f = sm::stopForwardBackward(runJump);
    CHECK((f & sm::kForward) != 0);
    CHECK((f & sm::kPendingStop) != 0);
    CHECK(sm::landWithPendingKeys(f) == 0u);

    // Released and pressed again in the air: nothing left to do on landing.
    f = sm::startForwardBackward(sm::stopForwardBackward(runJump), true);
    CHECK((f & sm::kPendingMask) == 0);
    CHECK(sm::landWithPendingKeys(f) == sm::kForward);

    // A strafe pressed and let go in the air leaves nothing behind.
    f = sm::stopStrafe(sm::startStrafe(runJump, true));
    CHECK((f & sm::kPendingMask) == 0);
    // A strafe held through the landing is applied there.
    f = sm::startStrafe(runJump, false);
    CHECK((f & sm::kStrafeRight) == 0);
    CHECK(sm::landWithPendingKeys(f) == (sm::kForward | sm::kStrafeRight));
    // NO_STRAFE refuses it outright.
    CHECK(sm::startStrafe(sm::kFalling, true, sm::kFlag2NoStrafe) == sm::kFalling);
}

TEST_CASE("a standing jump takes the first key at once") {
    // FALLING with no direction: the key sets the direction, not a pending flag.
    const uint32_t f = sm::startForwardBackward(sm::kFalling, true);
    CHECK(f == (sm::kFalling | sm::kForward));
    CHECK(sm::startStrafe(sm::kFalling, true) == (sm::kFalling | sm::kStrafeLeft));
    // On the ground the keys act at once.
    CHECK(sm::startForwardBackward(sm::kBackward, true) == sm::kForward);
    CHECK(sm::stopForwardBackward(sm::kForward | sm::kStrafeLeft) == sm::kStrafeLeft);
    CHECK(sm::stopStrafe(sm::kStrafeLeft) == 0u);
}

TEST_CASE("FALL_LAND goes out for any fall over 1/36 yd (0x73D4A0)") {
    // Every jump: its rise alone is 1.64 yd.
    CHECK(sm::fallLandDue(10.0f + sm::jumpApexHeight(), 10.0f));
    // A 3 cm drop is a fall; a 2 cm one is not.
    CHECK(sm::fallLandDue(10.03f, 10.0f));
    CHECK_FALSE(sm::fallLandDue(10.02f, 10.0f));
    // Landing higher than the start (a jump onto a ledge) still counts its rise.
    CHECK(sm::fallLandDue(10.0f + sm::jumpApexHeight(), 11.0f));
}

TEST_CASE("A knockback is a fall on the packet's path", "[stock_movement][knockback]") {
    // 0x6E9FF0: running forward and strafing, knocked back: falling, along
    // the packet's direction (FORWARD) until the landing stops it
    // (PENDING_STOP); the strafe stays as it was.
    const uint32_t f = sm::knockBackFlags(sm::kForward | sm::kStrafeLeft, 0);
    CHECK((f & sm::kFalling) != 0);
    CHECK((f & sm::kForward) != 0);
    CHECK((f & sm::kPendingStop) != 0);
    CHECK((f & sm::kStrafeLeft) != 0);
    // Backpedalling: the path is the packet's, forward along it.
    CHECK((sm::knockBackFlags(sm::kBackward, 0) & (sm::kBackward | sm::kForward)) == sm::kForward);
    // Out of the water and the air; a root waits as PENDING_ROOT; CAN_FLY goes.
    const uint32_t w = sm::knockBackFlags(sm::kSwimming | sm::kAscending | sm::kFlying | 0x800u | 0x1000000u, 0);
    CHECK((w & (sm::kSwimming | sm::kAscending | sm::kFlying | 0x800u | 0x1000000u)) == 0);
    CHECK((w & 0x100000u) != 0);
    // The landing stops it (FALL_LAND with the pending stop applied).
    CHECK((sm::landWithPendingKeys(f) & (sm::kForward | sm::kFalling | sm::kPendingStop)) == 0);
}

TEST_CASE("a swimmer has no step and steep ground is a fall") {
    // 0x760B40: the swim trace has no step; 0x75D1C0: past 50 degrees a
    // player is falling, past 80 anything else.
    CHECK(sm::kSwimStepHeight == 0.0f);
    CHECK(sm::groundIsFall(0.5f));
    CHECK_FALSE(sm::groundIsFall(0.7f));
    CHECK_FALSE(sm::groundIsFall(0.5f, false));
    CHECK(sm::groundIsFall(0.1f, false));
}

TEST_CASE("a jump onto a ledge at its apex lands with no FALL_LAND") {
    // 0x73D4A0: the fall height is the apex less the landing z; within 1/36
    // nothing is sent, however long the jump took.
    const float apex = 10.0f + sm::jumpApexHeight();
    CHECK_FALSE(sm::fallLandDue(apex, apex - 0.02f));
    CHECK(sm::fallLandDue(apex, apex - 0.03f));
    // A walk off an edge starts at v0 = 0, so its apex is the edge: it is
    // reported (and lands with FALL_LAND) once it has dropped 1/36.
    CHECK_FALSE(sm::fallLandDue(10.0f, 10.0f - sm::kFallLandMinHeight * 0.5f));
    CHECK(sm::fallLandDue(10.0f, 10.0f - sm::kFallLandMinHeight * 1.5f));
}

TEST_CASE("the walker snaps 1.85 x the move down after running off an edge") {
    // 0x7620F0 / 0xA32830: enough for a walkable 50 degree descent at any
    // speed, never a step - a sheer drop deeper than it is a fall.
    CHECK(sm::walkOffSnapReach(1.0f) == Approx(1.8494f).margin(1e-4));
    CHECK(sm::walkOffSnapReach(0.5f) > 0.5f * sm::kTan50);
    // Running at 60 fps (7 yd/s): a fifth of a yard.
    CHECK(sm::walkOffSnapReach(7.0f / 60.0f) == Approx(0.2158f).margin(1e-3));
    CHECK(sm::walkOffSnapReach(-1.0f) == 0.0f);
}

TEST_CASE("the pitch keys vote only swimming or flying and not while the mouse steers") {
    // 0x5FB1A0 / 0x5FBBC0.
    CHECK(sm::pitchVote(true, false, false, true) == 1);
    CHECK(sm::pitchVote(false, true, false, true) == -1);
    CHECK(sm::pitchVote(true, true, false, true) == 0);
    CHECK(sm::pitchVote(true, false, true, true) == 0);   // right button steers
    CHECK(sm::pitchVote(true, false, false, false) == 0); // on the ground
}

TEST_CASE("keyboard pitching runs at the pitch rate, three quarters while moving") {
    // 0x9877D0, as 0x987770 for turning.
    CHECK(sm::keyboardPitchRate(3.14159274f, 0) == Approx(3.14159274f));
    CHECK(sm::keyboardPitchRate(3.14159274f, sm::kForward) == Approx(3.14159274f * 0.75f));
    CHECK(sm::keyboardPitchRate(3.14159274f, sm::kForward, sm::kFlag2FullSpeedPitching) ==
          Approx(3.14159274f));
    CHECK(sm::stepPitch(0.0f, 1, 3.14159274f, 0.25f) == Approx(0.785398f));
    CHECK(sm::stepPitch(1.5f, 1, 3.14159274f, 0.5f) == Approx(sm::kMaxPitch));
    CHECK(sm::stepPitch(-1.5f, -1, 3.14159274f, 0.5f) == Approx(-sm::kMaxPitch));
    CHECK(sm::stepPitch(0.3f, 0, 3.14159274f, 0.5f) == Approx(0.3f));
}

TEST_CASE("SET_PITCH goes out at 0.1 rad from the last packet's pitch, swimming or flying") {
    // 0x71AE80 case 0xDB.
    CHECK_FALSE(sm::pitchChangeWorthSending(0.0f, 0.09f, sm::kSwimming));
    CHECK(sm::pitchChangeWorthSending(0.0f, 0.1f, sm::kSwimming));
    CHECK(sm::pitchChangeWorthSending(0.2f, 0.05f, sm::kFlying));
    CHECK_FALSE(sm::pitchChangeWorthSending(0.0f, 0.5f, 0));  // on foot: no pitch
}
