// Particle density as the stock client applies it.
//
// The particleDensity CVar (0x78D860, 0.1..1) is a straight multiplier on
// every emitter's rate (0x980ED0 reads it in 0x97D8C0), applied with the
// distance thinning and the rate variation. There is no floor for flames or
// any other class: Wowee had one, holding fixtures at 15 live particles,
// because its point sprites drew flames too small to read; the stock quads
// do not need it and it is gone.
#include <catch_amalgamated.hpp>

#include "rendering/m2_particle_stock.hpp"

using namespace wowee::rendering;

namespace {

/// The rate an emitter spawns at: density x (rate + vary x r) x distance factor.
float stockRate(float rate, float vary, float r, float density, float distance, bool spellVisual) {
    const float lod = spellVisual ? 1.0f : m2_particle::distanceRateFactor(distance);
    return density * (rate + vary * r) * lod;
}

} // namespace

TEST_CASE("density thins every emitter alike", "[particles][density]") {
    CHECK(stockRate(40.0f, 0.0f, 0.0f, 0.1f, 10.0f, false) == Catch::Approx(4.0f));
    // A candle at a tenth is a tenth: no floor.
    CHECK(stockRate(1.0f, 0.0f, 0.0f, 0.1f, 10.0f, false) == Catch::Approx(0.1f));
}

TEST_CASE("distance thins doodads but not spell effects", "[particles][density]") {
    CHECK(stockRate(40.0f, 0.0f, 0.0f, 1.0f, 100.0f, false) == Catch::Approx(10.0f));
    CHECK(stockRate(40.0f, 0.0f, 0.0f, 1.0f, 100.0f, true) == Catch::Approx(40.0f));
}

TEST_CASE("rate variation spreads around the track's rate", "[particles][density]") {
    CHECK(stockRate(10.0f, 5.0f, 1.0f, 1.0f, 0.0f, false) == Catch::Approx(15.0f));
    CHECK(stockRate(10.0f, 5.0f, -1.0f, 1.0f, 0.0f, false) == Catch::Approx(5.0f));
}
