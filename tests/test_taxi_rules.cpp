// Stock 3.3.5a taxi rules (docs/re/mounts-taxi-vehicles.md section 4):
// the route TakeTaxiNode flies, its price, the reply's error, and the bank
// of a taxi's Flying spline.
#include <catch_amalgamated.hpp>

#include "game/taxi_rules.hpp"
#include "game/unit_motion.hpp"

#include <cmath>
#include <cstring>

using namespace wowee::game;

TEST_CASE("Taxi route: the shortest flight, not the fewest hops (0x592BB0)", "[taxi]") {
    // 1 -> 4 directly is a long way round (900 yd); 1 -> 2 -> 3 -> 4 flies 300.
    std::vector<taxi::Edge> edges = {
        {1, 4, 500, 900.0f},
        {1, 2, 100, 100.0f}, {2, 3, 100, 100.0f}, {3, 4, 100, 100.0f},
    };
    auto all = [](uint32_t) { return true; };
    const auto route = taxi::shortestRoute(edges, 1, 4, all);
    REQUIRE(route == std::vector<uint32_t>{1, 2, 3, 4});
    CHECK(taxi::routePrice(edges, route) == 300u);

    // Through known nodes only: without 3 the long edge is the route.
    auto no3 = [](uint32_t n) { return n != 3; };
    CHECK(taxi::shortestRoute(edges, 1, 4, no3) == std::vector<uint32_t>{1, 4});
    // Nothing to go to.
    CHECK(taxi::shortestRoute(edges, 1, 9, all).empty());
    CHECK(taxi::shortestRoute(edges, 1, 1, all).empty());
}

TEST_CASE("Taxi reply errors name the stock strings (0x6CEE90)", "[taxi]") {
    CHECK(taxi::replyErrorKey(0) == nullptr);
    CHECK(std::strcmp(taxi::replyErrorKey(1), "ERR_TAXIUNSPECIFIEDSERVERERROR") == 0);
    CHECK(std::strcmp(taxi::replyErrorKey(3), "ERR_TAXINOTENOUGHMONEY") == 0);
    CHECK(std::strcmp(taxi::replyErrorKey(8), "ERR_TAXIPLAYERALREADYMOUNTED") == 0);
    CHECK(std::strcmp(taxi::replyErrorKey(12), "ERR_TAXINOTSTANDING") == 0);
    CHECK(taxi::replyErrorKey(13) == nullptr);
}

TEST_CASE("A Flying spline banks into the turn ahead (0x98CA00)", "[taxi]") {
    const glm::vec3 east(1.0f, 0.0f, 0.0f);
    const glm::vec3 pos(0.0f);
    // Straight on: level.
    CHECK(motion::flyingSplineBank(east, pos, glm::vec3(30.0f, 0.0f, 0.0f)) == Catch::Approx(0.0f));
    // Ahead 20 degrees to the left: twice that, positive.
    const float a = 20.0f * 3.14159265f / 180.0f;
    const glm::vec3 left(30.0f * std::cos(a), 30.0f * std::sin(a), 5.0f);
    CHECK(motion::flyingSplineBank(east, pos, left) == Catch::Approx(2.0f * a).margin(1e-4));
    // To the right: negative.
    const glm::vec3 right(30.0f * std::cos(a), -30.0f * std::sin(a), 0.0f);
    CHECK(motion::flyingSplineBank(east, pos, right) == Catch::Approx(-2.0f * a).margin(1e-4));
    // Clamped to a quarter turn.
    CHECK(motion::flyingSplineBank(east, pos, glm::vec3(-1.0f, 10.0f, 0.0f)) ==
          Catch::Approx(3.14159265f * 0.5f));
}

TEST_CASE("A taxi spline gives the mover its attitude, not a flier", "[taxi]") {
    using namespace motion;
    MonsterMoveSpec spec;
    spec.start = glm::vec3(0.0f, 0.0f, 0.0f);
    spec.splineFlags = SplineFlags::Flying;
    spec.durationMs = 10000;
    spec.points = {glm::vec3(100.0f, 0.0f, 0.0f), glm::vec3(200.0f, 100.0f, 20.0f),
                   glm::vec3(200.0f, 300.0f, 20.0f)};
    MoverSnapshot mover;
    mover.position = spec.start;
    mover.facing = 0.0f;
    auto plan = planMonsterMove(spec, mover, 1000);
    REQUIRE(plan.spline);
    UnitSpline sp = *plan.spline;
    // A unit without SWIMMING/FLYING (the taxi player): attitude and bank.
    auto f = sp.update(1000 + 3000, 0);
    CHECK(f.hasAttitude);
    CHECK(f.bank > 0.0f);            // the path turns left ahead
    // A flier pitches instead and does not bank.
    UnitSpline sp2 = *plan.spline;
    auto g = sp2.update(1000 + 3000, MoveFlags::Flying);
    CHECK(g.hasPitch);
    CHECK_FALSE(g.hasAttitude);
}
