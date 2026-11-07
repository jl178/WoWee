// Stock 3.3.5a vehicle seats (docs/re/mounts-taxi-vehicles.md section 5).
#include <catch_amalgamated.hpp>

#include "game/vehicle_rules.hpp"

#include <cmath>
#include <cstring>

using namespace wowee::game::vehicle;

TEST_CASE("Seat attachment ids are remapped to M2 attachments (0xA2D3F0)", "[vehicle]") {
    CHECK(seatM2Attachment(21) == 0);    // the mount point: Argent Tournament mounts
    CHECK(seatM2Attachment(13) == 39);   // Wintergrasp siege engine driver
    CHECK(seatM2Attachment(14) == 40);   // the mammoths' passenger seats
    CHECK(seatM2Attachment(0) == 20);
    CHECK(seatM2Attachment(22) == -1);   // none: relative to the vehicle base
    CHECK(seatM2Attachment(-1) == -1);
}

TEST_CASE("Passenger animation from the seat, no Mount fallback (0x747B20)", "[vehicle]") {
    SeatRecord argent;                   // VehicleSeat 3129: ReadyJoust
    argent.flags = 0xE210880B;
    argent.rideAnimStart = -1;
    argent.rideAnimLoop = 476;
    auto a = passengerAnim(argent, PassengerState::Seated);
    CHECK(a.start == kNoAnim);
    CHECK(a.loop == 476u);

    SeatRecord noRide;                   // a turret seat without flag 0x2
    noRide.flags = 0;
    noRide.rideAnimLoop = 91;
    auto b = passengerAnim(noRide, PassengerState::Seated);
    CHECK(b.start == kNoAnim);
    CHECK(b.loop == kNoAnim);            // its own stand, not Mount

    SeatRecord start;
    start.flags = kSeatHasRideAnim;
    start.rideAnimStart = 96;
    start.rideAnimLoop = 97;
    auto c = passengerAnim(start, PassengerState::Seated);
    CHECK(c.start == 96u);
    CHECK(c.loop == 97u);
}

TEST_CASE("The vehicle's ride loop needs flag 0x20000 (0x7489C0)", "[vehicle]") {
    SeatRecord s;
    s.vehicleRideAnimLoop = 158;         // the mammoths' Hold, flag clear
    CHECK(vehicleRideAnim(s) == kNoAnim);
    s.flags = kSeatVehicleRideAnim;
    CHECK(vehicleRideAnim(s) == 158u);
    s.vehicleRideAnimLoop = 506;
    CHECK(vehicleRideAnim(s) == kNoAnim);
}

TEST_CASE("UnitVehicleSkin names", "[vehicle]") {
    CHECK(std::strcmp(uiSkinName(0), "Natural") == 0);
    CHECK(std::strcmp(uiSkinName(1), "Mechanical") == 0);
}

TEST_CASE("Aim limits: CUSTOM_PITCH holds Vehicle.PitchMin..Max (0x5FB3A0)", "[vehicle]") {
    VehicleRecord demolisher;                 // Vehicle 106 (Wintergrasp Demolisher)
    demolisher.flags = 0x5f0df677;
    demolisher.pitchMin = 0.09f;
    demolisher.pitchMax = 0.79f;
    CHECK(clampAim(&demolisher, 0.0f) == Catch::Approx(0.09f));
    CHECK(clampAim(&demolisher, 1.2f) == Catch::Approx(0.79f));
    CHECK(aimNormAngle(&demolisher, 0.09f) == Catch::Approx(0.0f));
    CHECK(aimNormAngle(&demolisher, 0.79f) == Catch::Approx(1.0f));
    CHECK(aimFromNorm(&demolisher, 0.5f) == Catch::Approx(0.44f));

    VehicleRecord free;                       // no 0x40: +-pi/2
    free.flags = 0;
    CHECK(clampAim(&free, 3.0f) == Catch::Approx(1.5707964f));
    CHECK(clampAim(nullptr, -3.0f) == Catch::Approx(-1.5707964f));
    VehicleRecord flat;                       // limits one value: norm 0
    flat.flags = kVehicleCustomPitch;
    CHECK(aimNormAngle(&flat, 0.3f) == 0.0f);
}

TEST_CASE("Boarding arc: duration, height and easing (0x74A200 / 0x747D70)", "[vehicle]") {
    SeatRecord s;                             // VehicleSeat 3129: speed 7, gravity 19.3, arc 1..4
    s.enterSpeed = 7.0f;
    s.enterGravity = 19.3f;
    s.enterMinDuration = 0.0f;
    s.enterMaxDuration = 0.0f;                // unset: 1.5 s at most
    s.enterMinArcHeight = 1.0f;
    s.enterMaxArcHeight = 4.0f;
    SeatArc a = seatArc(s, true, 3.5f);
    CHECK(a.duration == Catch::Approx(0.5f));  // 3.5 / 7
    // g T^2 / 8 = 0.603 < 1: lifted to the minimum height.
    CHECK(seatArcLift(a, 0.5f) == Catch::Approx(1.0f).margin(1e-4));
    SeatArc far = seatArc(s, true, 70.0f);
    CHECK(far.duration == Catch::Approx(1.5f)); // capped at the default maximum
    // g T^2 / 8 = 5.43 > 4: held to the maximum height.
    CHECK(seatArcLift(far, 0.5f) == Catch::Approx(4.0f).margin(1e-4));
    CHECK(seatArcLift(far, 0.0f) == 0.0f);
    CHECK(seatArcLift(far, 1.0f) == Catch::Approx(0.0f).margin(1e-5));

    // Progress: linear, eased in (0x20), out (0x40), both (cosine).
    CHECK(seatArcProgress(a, 0.25f) == Catch::Approx(0.5f));
    a.flags = 0x20;
    CHECK(seatArcProgress(a, 0.25f) == Catch::Approx(0.25f));
    a.flags = 0x40;
    CHECK(seatArcProgress(a, 0.25f) == Catch::Approx(0.75f));
    a.flags = 0x60;
    CHECK(seatArcProgress(a, 0.25f) == Catch::Approx(0.5f));
    CHECK(seatArcProgress(a, 10.0f) == 1.0f);
    // Leaving uses the exit fields and the 0x80/0x100 flags.
    s.exitSpeed = 10.0f;
    SeatArc out = seatArc(s, false, 5.0f);
    CHECK(out.duration == Catch::Approx(0.5f));
    CHECK(out.easeIn == 0x80u);
    CHECK(out.easeOut == 0x100u);
}

TEST_CASE("The vehicle bar is the seat's VehicleAbilityDisplay (0x5D4AD0)", "[vehicle]") {
    SeatRecord s;
    CHECK_FALSE(seatShowsVehicleBar(s));
    s.vehicleAbilityDisplay = 1;
    CHECK(seatShowsVehicleBar(s));
}

TEST_CASE("Missile trajectories land where the flight meets the ground (0x6FCD60)", "[vehicle]") {
    MissileRecord hurl;                       // SpellMissile 526: Hurl Boulder
    hurl.flags = 0x1;
    hurl.defaultPitchMin = 0.524f;
    hurl.defaultPitchMax = 1.571f;
    hurl.defaultSpeedMin = 65.0f;
    hurl.defaultSpeedMax = 65.0f;
    hurl.gravity = 30.0f;
    VehicleRecord demolisher;
    demolisher.flags = 0x5f0df677;            // ALLOW_PITCHING | CUSTOM_PITCH, no ADJUST_AIM_POWER
    demolisher.pitchMin = 0.09f;
    demolisher.pitchMax = 0.79f;
    glm::vec2 es = missileElevationSpeed(hurl, &demolisher, 0.5f, 0.0f);
    CHECK(es.x == Catch::Approx(0.5f));
    CHECK(es.y == Catch::Approx(65.0f));
    es = missileElevationSpeed(hurl, &demolisher, 2.0f, 0.0f);
    CHECK(es.x == Catch::Approx(0.79f));      // held to the vehicle's limits
    es = missileElevationSpeed(hurl, nullptr, 2.0f, 0.0f);
    CHECK(es.x == Catch::Approx((0.524f + 1.571f) * 0.5f));  // the default pitch's middle

    MissileRecord thrown;                     // SpellMissile 1342: Throw Passenger, power 25..90
    thrown.defaultSpeedMin = 25.0f;
    thrown.defaultSpeedMax = 90.0f;
    VehicleRecord power;
    power.flags = kVehicleAdjustAimPower;
    CHECK(missileElevationSpeed(thrown, &power, 0.0f, 0.5f).y == Catch::Approx(57.5f));

    // Flat ground at z = 0: the landing comes close to the ballistic range
    // v^2 sin(2a) / g, stepped at 0.1 s.
    auto ground = [](const glm::vec3& a, const glm::vec3& b, float& frac) {
        if (b.z > 0.0f) return false;
        frac = a.z > 0.0f ? a.z / (a.z - b.z) : 0.0f;
        return true;
    };
    float t = 0.0f;
    const glm::vec3 land = simulateMissile(hurl, glm::vec3(0.0f), 0.0f, 0.5f, 65.0f, ground, &t);
    const float range = 65.0f * 65.0f * std::sin(1.0f) / 30.0f;
    CHECK(land.x == Catch::Approx(range).epsilon(0.02));
    CHECK(land.y == Catch::Approx(0.0f).margin(1e-3));
    CHECK(land.z == Catch::Approx(0.0f).margin(1e-3));
    CHECK(t == Catch::Approx(2.0f * 65.0f * std::sin(0.5f) / 30.0f).epsilon(0.03));
    // Nothing to meet: 200 steps (20 s) of flight.
    simulateMissile(hurl, glm::vec3(0.0f), 0.0f, 0.5f, 65.0f, nullptr, &t);
    CHECK(t == Catch::Approx(20.0f));
}

TEST_CASE("A fading vehicle clears as the camera nears it (Vehicle.flags 0x80000)", "[vehicle]") {
    VehicleRecord v;                          // the Wintergrasp and Ulduar engines: 1.0 .. 1.5
    v.cameraFadeDistScalarMin = 1.0f;
    v.cameraFadeDistScalarMax = 1.5f;
    CHECK(cameraFadeAlpha(v, 10.0f, 20.0f) == 1.0f);
    CHECK(cameraFadeAlpha(v, 10.0f, 5.0f) == 0.0f);
    // Measured beyond the 0.2 near clip, as 0x606F90 measures it.
    CHECK(cameraFadeAlpha(v, 10.0f, 12.7f) == Catch::Approx(0.5f));
    CHECK(cameraFadeAlpha(v, 10.0f, 12.5f) < 0.5f);
    CHECK(cameraFadeAlpha(v, 10.0f, 12.5f, 0.0f) == Catch::Approx(0.5f));
    CHECK(cameraFadeAlpha(v, 10.0f, 15.2f) == 1.0f);
    CHECK(cameraFadeAlpha(v, 10.0f, 15.15f) < 1.0f);
}
