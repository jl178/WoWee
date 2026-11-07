// How the stock 3.3.5a client moves units it does not control: monster-move
// splines, other players' movement packets, the body's turn. The numbers are
// Wow.exe 12340's; docs/re/unit-motion.md has the addresses.
#include <catch_amalgamated.hpp>

#include "game/unit_motion.hpp"

#include <cmath>

using namespace wowee::game::motion;
using Catch::Approx;

namespace {

MonsterMoveSpec groundMove(const glm::vec3& start, std::vector<glm::vec3> points, uint32_t durationMs) {
    MonsterMoveSpec s;
    s.start = start;
    s.points = std::move(points);
    s.durationMs = durationMs;
    return s;
}

MoverSnapshot at(const glm::vec3& p, float facing = 0.0f) {
    MoverSnapshot m;
    m.position = p;
    m.facing = facing;
    return m;
}

}  // namespace

// ── The curve ────────────────────────────────────────────────────────────

TEST_CASE("A linear curve is uniform in path length, not in nodes", "[unit_motion][spline]") {
    SplineCurve c;
    // Real points (0,0,0) -> (1,0,0) -> (4,0,0): one yard then three.
    c.init({{-1, 0, 0}, {0, 0, 0}, {1, 0, 0}, {4, 0, 0}, {4, 0, 0}}, false);
    REQUIRE(c.segmentCount() == 2);
    REQUIRE(c.length() == Approx(4.0f));
    CHECK(c.evaluate(0.25f).position.x == Approx(1.0f));
    CHECK(c.evaluate(0.5f).position.x == Approx(2.0f));
    CHECK(c.evaluate(1.0f).position.x == Approx(4.0f));
    CHECK(c.evaluate(0.5f).direction.x == Approx(1.0f));
}

TEST_CASE("A Catmull-Rom curve passes through its nodes", "[unit_motion][spline]") {
    SplineCurve c;
    c.init({{-1, 0, 0}, {0, 0, 0}, {5, 5, 0}, {10, 0, 0}, {10, 0, 0}}, true);
    REQUIRE(c.segmentCount() == 2);
    const glm::vec3 start = c.evaluate(0.0f).position;
    const glm::vec3 end = c.evaluate(1.0f).position;
    CHECK(start.x == Approx(0.0f).margin(1e-4));
    CHECK(end.x == Approx(10.0f).margin(1e-4));
    CHECK(end.y == Approx(0.0f).margin(1e-4));
    // The middle node is where the first segment ends, by arc length.
    float acc = 0.0f, best = 1e9f;
    for (int i = 0; i <= 1000; ++i) {
        const glm::vec3 p = c.evaluate(static_cast<float>(i) / 1000.0f).position;
        best = std::min(best, glm::length(p - glm::vec3(5, 5, 0)));
        (void)acc;
    }
    CHECK(best < 0.05f);
    // Its length is the twenty-chord one, longer than the straight chords.
    CHECK(c.length() > 2.0f * std::sqrt(50.0f));
}

// ── Monster moves ────────────────────────────────────────────────────────

TEST_CASE("A ground spline starts where this client has the unit", "[unit_motion][monster_move]") {
    // The unit lags half way along the first leg; the path starts at its
    // foot on the leg, not at the packet's start.
    MonsterMoveSpec spec = groundMove({0, 0, 0}, {{10, 0, 0}, {10, 10, 0}}, 2857);
    const auto plan = planMonsterMove(spec, at({5, 0.5f, 0}), 1000);
    REQUIRE(plan.kind == MonsterMovePlan::Kind::Spline);
    const auto& pts = plan.spline->curve.points();
    REQUIRE(pts.size() == 5);  // pad, (5,0), (10,0), (10,10), pad
    CHECK(pts[1].x == Approx(5.0f));
    CHECK(pts[1].y == Approx(0.0f));
    // The pads: a reflection before, the end repeated after.
    CHECK(pts[0].x == Approx(0.0f));
    CHECK(pts[4] == pts[3]);
    CHECK_FALSE(plan.spline->curve.catmullRom());
}

TEST_CASE("A unit short of the path walks to its start", "[unit_motion][monster_move]") {
    MonsterMoveSpec spec = groundMove({0, 0, 0}, {{10, 0, 0}}, 2000);
    const auto plan = planMonsterMove(spec, at({-3, 0, 0}), 0);
    REQUIRE(plan.kind == MonsterMovePlan::Kind::Spline);
    const auto& pts = plan.spline->curve.points();
    REQUIRE(pts.size() == 5);
    CHECK(pts[1].x == Approx(-3.0f));
    CHECK(pts[2].x == Approx(0.0f));
    CHECK(pts[3].x == Approx(10.0f));
}

TEST_CASE("A unit past the whole path goes straight to its end", "[unit_motion][monster_move]") {
    const auto trimmed = trimPathToMover({{0, 0, 0}, {10, 0, 0}}, {12, 1, 0});
    REQUIRE(trimmed.size() == 2);
    CHECK(trimmed[0].x == Approx(12.0f));
    CHECK(trimmed[1].x == Approx(10.0f));
}

TEST_CASE("The server's duration is kept unless it is faster than the cap", "[unit_motion][monster_move]") {
    // 7 yd/s for ten yards: the server's 1428 ms stands.
    auto plan = planMonsterMove(groundMove({0, 0, 0}, {{10, 0, 0}}, 1428), at({0, 0, 0}), 0);
    REQUIRE(plan.spline);
    CHECK(plan.spline->durationMs == 1428);
    // A hundred yards in a second is capped at max(28, 4 * run) = 28 yd/s.
    plan = planMonsterMove(groundMove({0, 0, 0}, {{100, 0, 0}}, 1000), at({0, 0, 0}), 0);
    REQUIRE(plan.spline);
    CHECK(plan.spline->durationMs == 3571);
    // A fast runner's cap is four times its run speed.
    MoverSnapshot fast = at({0, 0, 0});
    fast.runSpeed = 14.0f;
    plan = planMonsterMove(groundMove({0, 0, 0}, {{100, 0, 0}}, 1000), fast, 0);
    CHECK(plan.spline->durationMs == 1786);
    // A smooth (flying) one is capped at 50.
    MonsterMoveSpec fly = groundMove({0, 0, 0}, {{100, 0, 0}}, 1000);
    fly.splineFlags = SplineFlags::Flying;
    plan = planMonsterMove(fly, at({0, 0, 0}), 0);
    CHECK(plan.spline->durationMs == 2000);
    // No duration: the cap is the speed.
    plan = planMonsterMove(groundMove({0, 0, 0}, {{56, 0, 0}}, 0), at({0, 0, 0}), 0);
    CHECK(plan.spline->durationMs == 2000);
}

TEST_CASE("A smooth spline leaves along the unit's facing", "[unit_motion][monster_move]") {
    MonsterMoveSpec spec = groundMove({0, 0, 10}, {{10, 0, 10}, {20, 10, 10}}, 3000);
    spec.splineFlags = SplineFlags::Flying;
    const float facing = kPi * 0.5f;  // facing +y
    const auto plan = planMonsterMove(spec, at({0, 0, 10}, facing), 0);
    REQUIRE(plan.spline);
    const auto& pts = plan.spline->curve.points();
    CHECK(plan.spline->curve.catmullRom());
    CHECK(pts[0].x == Approx(0.0f).margin(1e-5));
    CHECK(pts[0].y == Approx(-1.0f));
    CHECK(pts[1] == glm::vec3(0, 0, 10));
    CHECK(pts.back() == pts[pts.size() - 2]);
}

TEST_CASE("A path too short to walk places the unit and turns it", "[unit_motion][monster_move]") {
    MonsterMoveSpec spec = groundMove({0, 0, 0}, {{0.1f, 0, 0}}, 100);
    spec.moveType = 4;
    spec.facingAngle = 1.0f;
    const auto plan = planMonsterMove(spec, at({0, 0, 0}), 0);
    CHECK(plan.kind == MonsterMovePlan::Kind::Place);
    CHECK(plan.immediateFacing == FinalFacing::Angle);
    CHECK(plan.immediateAngle == Approx(1.0f));
    // Within three yards of the end it is not moved.
    CHECK(plan.position == glm::vec3(0, 0, 0));
}

TEST_CASE("A stop near the unit leaves it; a far one slides it there", "[unit_motion][monster_move]") {
    MonsterMoveSpec stop;
    stop.moveType = 1;
    stop.start = {0.5f, 0, 0};
    CHECK(planMonsterMove(stop, at({0, 0, 0}), 0).kind == MonsterMovePlan::Kind::Stop);
    stop.start = {14, 0, 0};
    const auto plan = planMonsterMove(stop, at({0, 0, 0}), 0);
    REQUIRE(plan.kind == MonsterMovePlan::Kind::Spline);
    // At the ground cap: 14 yards at 28 yd/s.
    CHECK(plan.spline->durationMs == 500);
}

TEST_CASE("A spline turns the unit along the path, inverted when asked", "[unit_motion][monster_move]") {
    auto plan = planMonsterMove(groundMove({0, 0, 0}, {{0, 10, 0}}, 1000), at({0, 0, 0}), 0);
    REQUIRE(plan.spline);
    auto f = plan.spline->update(500, 0);
    REQUIRE(f.facing);
    CHECK(*f.facing == Approx(kPi * 0.5f));
    CHECK(f.position.y == Approx(5.0f));

    MonsterMoveSpec back = groundMove({0, 0, 0}, {{0, 10, 0}}, 1000);
    back.splineFlags = SplineFlags::OrientationInversed;
    plan = planMonsterMove(back, at({0, 0, 0}), 0);
    f = plan.spline->update(500, 0);
    REQUIRE(f.facing);
    CHECK(*f.facing == Approx(kPi * 1.5f));

    MonsterMoveSpec fixed = groundMove({0, 0, 0}, {{0, 10, 0}}, 1000);
    fixed.splineFlags = SplineFlags::OrientationFixed;
    plan = planMonsterMove(fixed, at({0, 0, 0}), 0);
    CHECK_FALSE(plan.spline->update(500, 0).facing);
}

TEST_CASE("A spline ends on time and carries its final facing", "[unit_motion][monster_move]") {
    MonsterMoveSpec spec = groundMove({0, 0, 0}, {{10, 0, 0}}, 1000);
    spec.moveType = 4;
    spec.facingAngle = 2.0f;
    auto plan = planMonsterMove(spec, at({0, 0, 0}), 100);
    REQUIRE(plan.spline);
    CHECK(plan.spline->finalFacing == FinalFacing::Angle);
    CHECK_FALSE(plan.spline->update(1099, 0).finished);
    const auto f = plan.spline->update(1100, 0);
    CHECK(f.finished);
    CHECK(f.position.x == Approx(10.0f));
}

TEST_CASE("MoveJump rises and falls on a parabola", "[unit_motion][monster_move]") {
    MonsterMoveSpec spec = groundMove({0, 0, 0}, {{20, 0, 0}}, 2000);
    spec.splineFlags = SplineFlags::Parabolic;
    spec.hasParabolic = true;
    spec.verticalAcceleration = 10.0f;
    spec.parabolicStartMs = 0;
    auto plan = planMonsterMove(spec, at({0, 0, 0}), 0);
    REQUIRE(plan.spline);
    auto f = plan.spline->update(1000, 0);
    // a/2 * t * (D - t) at t = D/2 = 1 s: 5.
    CHECK(f.position.z == Approx(5.0f));
    CHECK(f.jumpStarted);
    f = plan.spline->update(500, 0);
    CHECK(f.position.z == Approx(0.5f * 10.0f * 0.5f * 1.5f));
    f = plan.spline->update(2000, 0);
    CHECK(f.finished);
    CHECK(f.landed);
    CHECK(f.position.z == Approx(0.0f));
}

TEST_CASE("A falling spline drops by gravity to its end height", "[unit_motion][monster_move]") {
    MonsterMoveSpec spec = groundMove({0, 0, 50}, {{0, 0.5f, 0}}, 2300);
    spec.splineFlags = SplineFlags::Falling;
    MoverSnapshot m = at({0, 0, 50});
    auto plan = planMonsterMove(spec, m, 0);
    REQUIRE(plan.spline);
    const auto f = plan.spline->update(1000, 0);
    CHECK(f.position.z == Approx(50.0f - 0.5f * kGravity));
    CHECK_FALSE(f.facing);  // a fall does not turn the unit
    const auto g = plan.spline->update(2299, 0);
    CHECK(g.position.z == Approx(0.0f));
    CHECK(g.landed);
}

TEST_CASE("A cyclic spline laps, dropping its way in after the first", "[unit_motion][monster_move]") {
    MonsterMoveSpec spec = groundMove({0, 0, 20}, {{10, 0, 20}, {10, 10, 20}, {0, 10, 20}}, 4000);
    spec.splineFlags = SplineFlags::Flying | SplineFlags::Cyclic | SplineFlags::EnterCycle;
    auto plan = planMonsterMove(spec, at({-10, 0, 20}), 0);
    REQUIRE(plan.spline);
    const size_t before = plan.spline->curve.points().size();
    const uint32_t lap = plan.spline->durationMs;
    auto f = plan.spline->update(lap + 10, 0);
    CHECK_FALSE(f.finished);
    CHECK(plan.spline->curve.points().size() == before - 1);
    CHECK((plan.spline->flags & SplineFlags::EnterCycle) == 0);
    // A plain cyclic one keeps going without changing.
    f = plan.spline->update(lap * 5, 0);
    CHECK_FALSE(f.finished);
}

TEST_CASE("A transport enter spline is not run", "[unit_motion][monster_move]") {
    MonsterMoveSpec spec = groundMove({0, 0, 0}, {{10, 0, 0}}, 1000);
    spec.splineFlags = SplineFlags::TransportEnter;
    const auto plan = planMonsterMove(spec, at({0, 0, 0}), 0);
    CHECK(plan.kind == MonsterMovePlan::Kind::Place);
    CHECK(plan.position.x == Approx(10.0f));  // more than 3 yards: placed at the end
}

// ── Physics ──────────────────────────────────────────────────────────────

TEST_CASE("Fall distance: the jump's arc and terminal speed", "[unit_motion][physics]") {
    const float apexT = -kJumpVelocity / kGravity;
    const float apex = -fallDistance(apexT, kJumpVelocity, false);
    CHECK(apexT == Approx(0.4124f).margin(1e-3));
    CHECK(apex == Approx(1.6404f).margin(1e-3));
    // Long falls reach 60.148 yd/s.
    const float a = fallDistance(10.0f, 0.0f, false);
    const float b = fallDistance(11.0f, 0.0f, false);
    CHECK(b - a == Approx(kTerminalVelocity));
    CHECK(fallDistance(10.0f, 0.0f, true) - fallDistance(9.0f, 0.0f, true) == Approx(7.0f));
}

TEST_CASE("The speed the flags select", "[unit_motion][physics]") {
    MoveSpeeds s;
    CHECK(selectSpeed(s, MoveFlags::Forward) == Approx(7.0f));
    CHECK(selectSpeed(s, MoveFlags::Backward) == Approx(4.5f));
    CHECK(selectSpeed(s, MoveFlags::Forward | MoveFlags::Walking) == Approx(2.5f));
    CHECK(selectSpeed(s, MoveFlags::Forward | MoveFlags::Swimming) == Approx(4.722222f));
    CHECK(selectSpeed(s, MoveFlags::Forward | MoveFlags::Flying) == Approx(7.0f));
    CHECK(selectSpeed(s, MoveFlags::TurnLeft) == 0.0f);
}

namespace {
MoveState stateWith(uint32_t flags, float facing = 0.0f) {
    MovementSample s;
    s.flags = flags;
    s.facing = facing;
    MoveState st;
    st.applySample(s);
    return st;
}
}  // namespace

TEST_CASE("Dead reckoning: straight, strafing, backing up", "[unit_motion][physics]") {
    MoveSpeeds sp;
    auto d = displacementAt(stateWith(MoveFlags::Forward), sp, 1000);
    CHECK(d.offset.x == Approx(7.0f));
    CHECK(d.offset.y == Approx(0.0f).margin(1e-5));
    d = displacementAt(stateWith(MoveFlags::StrafeLeft), sp, 1000);
    CHECK(d.offset.x == Approx(0.0f).margin(1e-5));
    CHECK(d.offset.y == Approx(7.0f));
    d = displacementAt(stateWith(MoveFlags::StrafeRight), sp, 1000);
    CHECK(d.offset.y == Approx(-7.0f));
    d = displacementAt(stateWith(MoveFlags::Forward | MoveFlags::StrafeLeft), sp, 1000);
    CHECK(d.offset.x == Approx(7.0f * 0.70710677f));
    CHECK(d.offset.y == Approx(7.0f * 0.70710677f));
    d = displacementAt(stateWith(MoveFlags::Backward), sp, 1000);
    CHECK(d.offset.x == Approx(-4.5f));
}

TEST_CASE("Dead reckoning: running while turning goes round a circle", "[unit_motion][physics]") {
    MoveSpeeds sp;
    const MoveState st = stateWith(MoveFlags::Forward | MoveFlags::TurnLeft);
    // Turning at 0.75 * pi rad/s while moving: a full circle in 8/3 s.
    const uint32_t lapMs = static_cast<uint32_t>(std::lround(2.0f / 0.75f * 1000.0f));
    auto d = displacementAt(st, sp, lapMs);
    CHECK(glm::length(d.offset) < 0.02f);
    d = displacementAt(st, sp, lapMs / 2);
    const float radius = 7.0f / (0.75f * kPi);
    CHECK(d.offset.y == Approx(2.0f * radius).margin(0.02f));
    CHECK(d.facing == Approx(kPi).margin(1e-3));
    // Full-speed turning (flags2 0x8) is not slowed.
    MoveState fast = st;
    fast.flags2 = MoveFlags2::FullSpeedTurning;
    CHECK(displacementAt(fast, sp, 1000).facing == Approx(kPi));
    // Turning on the spot turns at the full rate and stays put.
    d = displacementAt(stateWith(MoveFlags::TurnRight), sp, 250);
    CHECK(glm::length(d.offset) == Approx(0.0f));
    CHECK(d.facing == Approx(kTwoPi - kPi * 0.25f));
}

TEST_CASE("Dead reckoning: a jump's arc", "[unit_motion][physics]") {
    MoveSpeeds sp;
    MovementSample s;
    s.flags = MoveFlags::Forward | MoveFlags::Falling;
    s.position = {0, 0, 10};
    s.jumpVelocity = kJumpVelocity;
    s.jumpCos = 1.0f;
    s.jumpSin = 0.0f;
    s.jumpXYSpeed = 7.0f;
    MoveState st;
    st.applySample(s);
    const auto d = displacementAt(st, sp, 412);
    CHECK(d.offset.x == Approx(7.0f * 0.412f));
    CHECK(d.offset.z == Approx(1.6404f).margin(2e-3));
    // A heartbeat mid-fall carries the fall time: the arc goes on, not again.
    s.fallTimeMs = 412;
    s.position = {0, 0, 11.64f};
    MoveState mid;
    mid.applySample(s);
    CHECK(displacementAt(mid, sp, 200).offset.z < 0.0f);
}

TEST_CASE("A silent mover keeps going, then slows to a stop", "[unit_motion][remote]") {
    CHECK(stalledElapsed(800, 500) == 800);
    CHECK(stalledElapsed(1300, 1000) == 1300 - 125);
    CHECK(stalledElapsed(1800, 1500) == 1800 - 500);
    CHECK(stalledElapsed(2300, 2000) == 2300 - 500 - 500);
    CHECK(stalledElapsed(5300, 5000) == stalledElapsed(4300, 4000));
}

// ── Remote movers ────────────────────────────────────────────────────────

TEST_CASE("Remote packets play out at their sender's spacing", "[unit_motion][remote]") {
    RemoteMover m;
    MovementSample a;
    a.time = 10000;
    a.flags = MoveFlags::Forward;
    a.position = {0, 0, 0};
    m.receive(a, 5000);
    CHECK(m.appliedCount() == 1);  // the first is taken as it comes
    // Two more the sender spaced 100 ms apart arrive together, 40 ms later.
    MovementSample b = a, c = a;
    b.time = 10100; b.position = {0.7f, 0, 0};
    c.time = 10200; c.position = {1.4f, 0, 0};
    m.receive(b, 5040);
    m.receive(c, 5040);
    CHECK(m.pendingCount() == 2);
    m.update(5099);
    CHECK(m.appliedCount() == 1);
    m.update(5100);
    CHECK(m.appliedCount() == 2);
    CHECK(m.state().position.x == Approx(0.7f));
    m.update(5150);
    CHECK(m.state().position.x == Approx(0.7f + 0.35f));
    m.update(5200);
    CHECK(m.appliedCount() == 3);
    CHECK(m.state().position.x == Approx(1.4f));
}

TEST_CASE("A late packet is applied at once and run on", "[unit_motion][remote]") {
    RemoteMover m;
    MovementSample a;
    a.time = 0;
    a.flags = MoveFlags::Forward;
    m.receive(a, 1000);
    MovementSample b = a;
    b.time = 100;
    b.position = {0.7f, 0, 0};
    // 300 ms late.
    m.receive(b, 1400);
    m.update(1400);
    CHECK(m.appliedCount() == 2);
    CHECK(m.state().position.x == Approx(0.7f + 7.0f * 0.3f));
}

TEST_CASE("A remote packet reports where it corrected dead reckoning", "[unit_motion][remote]") {
    // Stock sets the position a packet carries (0x988920); the difference
    // from where dead reckoning had the unit at the packet's time is the
    // correction "Smoother other units" glides over - and the only thing it
    // glides over.
    RemoteMover m;
    MovementSample a;
    a.time = 0;
    a.flags = MoveFlags::Forward;
    m.receive(a, 1000);
    CHECK(glm::length(m.takeCorrection()) == 0.0f);  // the first: nothing to correct
    MovementSample b = a;
    b.time = 500;
    b.position = {3.5f, 0, 0};                         // where 500 ms at 7 yd/s puts it
    m.receive(b, 1500);
    m.update(1500);
    CHECK(glm::length(m.takeCorrection()) == Approx(0.0f).margin(1e-3f));
    MovementSample c = b;
    c.time = 1000;
    c.position = {8.0f, 0, 0};                         // a yard further than that
    m.receive(c, 2000);
    m.update(2000);
    CHECK(m.takeCorrection().x == Approx(1.0f).margin(1e-3f));
    CHECK(glm::length(m.takeCorrection()) == 0.0f);    // taken
}

TEST_CASE("A remote runner whose packets stop comes to rest", "[unit_motion][remote]") {
    RemoteMover m;
    MovementSample a;
    a.flags = MoveFlags::Forward;
    m.receive(a, 0);
    m.update(3000);
    // Half a second at full speed, then a second slowing evenly to rest.
    CHECK(m.state().position.x == Approx(7.0f * 1.0f).margin(0.01f));
    m.update(6000);
    CHECK(m.state().position.x == Approx(7.0f).margin(0.01f));
}

TEST_CASE("A stop packet ends the run where it says", "[unit_motion][remote]") {
    RemoteMover m;
    MovementSample a;
    a.time = 0;
    a.flags = MoveFlags::Forward;
    m.receive(a, 0);
    MovementSample stop;
    stop.time = 1000;
    stop.flags = 0;
    stop.position = {6.9f, 0, 0};
    m.receive(stop, 1000);
    m.update(1000);
    CHECK(m.state().position.x == Approx(6.9f));
    m.update(2000);
    CHECK(m.state().position.x == Approx(6.9f));
    CHECK_FALSE(m.state().moving());
}

// ── Presentation ─────────────────────────────────────────────────────────

TEST_CASE("Walk, run or sprint by pace", "[unit_motion][anim]") {
    CHECK(groundLocomotionAnim(2.5f, 2.5f, false) == 4u);
    CHECK(groundLocomotionAnim(5.0f, 2.5f, false) == 4u);   // not above twice walk
    CHECK(groundLocomotionAnim(5.1f, 2.5f, false) == 5u);
    CHECK(groundLocomotionAnim(11.5f, 2.5f, false) == 143u);
    CHECK(groundLocomotionAnim(7.0f, 2.5f, true) == 13u);
    CHECK(locomotionPlaybackRate(14.0f, 7.0f) == Approx(2.0f));
    CHECK(locomotionPlaybackRate(5.0f, 0.0f) == 1.0f);
}

TEST_CASE("The body turns at eight times the turn rate, never a quarter behind", "[unit_motion][facing]") {
    // A half turn: the quarter beyond the lag limit at once, then 8*pi rad/s.
    float body = stepBodyFacing(0.0f, kPi * 0.999f, kPi, 0.0f);
    CHECK(body == Approx(kPi * 0.999f - kPi * 0.5f).margin(1e-4));
    body = stepBodyFacing(body, kPi * 0.999f, kPi, 0.01f);
    CHECK(body == Approx(kPi * 0.999f - kPi * 0.5f + 8.0f * kPi * 0.01f).margin(1e-4));
    // Small ones close within a frame or two.
    CHECK(stepBodyFacing(0.0f, 0.1f, kPi, 1.0f / 60.0f) == Approx(0.1f));
    // The short way round.
    CHECK(stepBodyFacing(0.1f, kTwoPi - 0.1f, kPi, 1.0f) == Approx(-0.1f));
}
