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
