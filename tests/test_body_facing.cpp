// Stock 3.3.5a body yaw and look twist (rendering/animation/body_facing.hpp).
// Angles are CCW positive: a left strafe turns the body to +90 degrees and the
// spine and head back by -45 each.
#include <catch_amalgamated.hpp>
#include "rendering/animation/body_facing.hpp"

#include <cmath>
#include <cstdio>

namespace anim = wowee::rendering::anim;

namespace {

constexpr float kDeg = 180.0f / 3.14159265358979f;

struct Settled {
    float bodyMinusFacingDeg;
    float spineDeg;
    float headDeg;
};

float wrapDeg(float d) { return std::remainder(d, 360.0f); }

// Runs two seconds at 60 fps from a body at rest on the facing.
Settled settle(const char* name, float facingDeg, bool fwd, bool back, bool left, bool right,
               bool activePlayer = true) {
    anim::BodyFacingState s;
    anim::BodyFacingInput in;
    in.facing = facingDeg / kDeg;
    in.activePlayer = activePlayer;
    in.dt = 1.0f / 60.0f;
    in.catchUpElapsed = activePlayer ? 0.0f : 1.0e9f;
    anim::updateBodyFacing(s, in);  // standing first, as on spawn
    in.forward = fwd;
    in.backward = back;
    in.strafeLeft = left;
    in.strafeRight = right;
    anim::BodyFacingResult r;
    for (int i = 0; i < 120; ++i) r = anim::updateBodyFacing(s, in);
    Settled out{wrapDeg(r.body * kDeg - facingDeg), r.spineYaw * kDeg, r.headYaw * kDeg};
    std::printf("%-22s facing %8.2f  body %8.2f (facing %+7.2f)  spine %+7.2f  head %+7.2f\n",
                name, facingDeg, r.body * kDeg, out.bodyMinusFacingDeg, out.spineDeg, out.headDeg);
    return out;
}

void check(const Settled& s, float body, float spine, float head) {
    CHECK(s.bodyMinusFacingDeg == Catch::Approx(body).margin(0.5f));
    CHECK(s.spineDeg == Catch::Approx(spine).margin(0.5f));
    CHECK(s.headDeg == Catch::Approx(head).margin(0.5f));
}

}  // namespace

TEST_CASE("Active player body yaw and twist per movement", "[body_facing]") {
    // Several turns of accumulated camera yaw, as Wowee's facing carries.
    for (float facing : {30.0f, 1110.0f, -725.0f}) {
        check(settle("forward", facing, true, false, false, false), 0.0f, 0.0f, 0.0f);
        check(settle("strafe left", facing, false, false, true, false), 90.0f, -45.0f, -45.0f);
        check(settle("strafe right", facing, false, false, false, true), -90.0f, 45.0f, 45.0f);
        check(settle("forward-left", facing, true, false, true, false), 45.0f, -45.0f, 0.0f);
        check(settle("forward-right", facing, true, false, false, true), -45.0f, 45.0f, 0.0f);
        check(settle("back-left", facing, false, true, true, false), -45.0f, 45.0f, 0.0f);
        check(settle("back-right", facing, false, true, false, true), 45.0f, -45.0f, 0.0f);
        check(settle("back", facing, false, true, false, false), 0.0f, 0.0f, 0.0f);
    }
}

TEST_CASE("Other players split the twist between spine and head", "[body_facing]") {
    check(settle("remote strafe left", 200.0f, false, false, true, false, false), 90.0f, -45.0f, -45.0f);
    check(settle("remote forward-left", 200.0f, true, false, true, false, false), 45.0f, -22.5f, -22.5f);
    check(settle("remote back-left", 200.0f, false, true, true, false, false), -45.0f, 22.5f, 22.5f);
    check(settle("remote forward", 200.0f, true, false, false, false, false), 0.0f, 0.0f, 0.0f);
}

TEST_CASE("Walking forward out of a strafe returns the legs to the facing", "[body_facing]") {
    anim::BodyFacingState s;
    anim::BodyFacingInput in;
    in.facing = 1110.0f / kDeg;
    in.activePlayer = true;
    in.dt = 1.0f / 60.0f;
    in.catchUpElapsed = 0.0f;
    in.strafeLeft = true;
    for (int i = 0; i < 60; ++i) anim::updateBodyFacing(s, in);
    in.strafeLeft = false;
    in.forward = true;
    anim::BodyFacingResult r;
    float t = 0.0f;
    for (int i = 0; i < 60; ++i) {
        t += in.dt;
        in.catchUpElapsed = t;  // the key change stamped the catch-up clock
        r = anim::updateBodyFacing(s, in);
    }
    CHECK(wrapDeg(r.body * kDeg - 1110.0f) == Catch::Approx(0.0f).margin(0.1f));
    CHECK(r.spineYaw == 0.0f);
    CHECK(r.headYaw == 0.0f);
}
