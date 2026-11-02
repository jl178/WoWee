// Stock 3.3.5a vehicle seats (docs/re/mounts-taxi-vehicles.md section 5).
#include <catch_amalgamated.hpp>

#include "game/vehicle_rules.hpp"

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
