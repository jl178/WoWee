#pragma once

// Stock 3.3.5a (build 12340) vehicles and passengers, reverse engineered in
// docs/re/mounts-taxi-vehicles.md section 5. The records are Vehicle.dbc and
// VehicleSeat.dbc as the client reads them (Vehicle_C 0x757FA0, the seat
// table at Vehicle +0x18); the rules are pure and tested in
// tests/test_vehicle_rules.cpp.

#include <cstdint>
#include <glm/glm.hpp>

namespace wowee::game::vehicle {

/// VehicleSeat.dbc (58 fields) - the columns the client reads.
struct SeatRecord {
    uint32_t id = 0;
    uint32_t flags = 0;               // 1
    int32_t attachmentId = -1;        // 2: a seat attachment id, see seatM2Attachment
    glm::vec3 attachmentOffset{0.0f}; // 3..5
    int32_t enterAnimStart = -1;      // 13
    int32_t enterAnimLoop = -1;       // 14
    int32_t rideAnimStart = -1;       // 15
    int32_t rideAnimLoop = -1;        // 16
    int32_t rideUpperAnimStart = -1;  // 17
    int32_t rideUpperAnimLoop = -1;   // 18
    int32_t exitAnimStart = -1;       // 26
    int32_t exitAnimLoop = -1;        // 27
    float passengerYaw = 0.0f;        // 29
    float passengerPitch = 0.0f;      // 30
    float passengerRoll = 0.0f;       // 31
    int32_t passengerAttachmentId = -1; // 32: a raw M2 attachment on the passenger
    int32_t vehicleRideAnimLoop = -1; // 35
    uint32_t uiSkin = 0;              // 44: 0 "Natural", 1 "Mechanical"
    uint32_t flagsB = 0;              // 45
    glm::vec3 cameraOffset{0.0f};     // 50..52
};

/// Vehicle.dbc (40 fields) - what the client reads of it.
struct VehicleRecord {
    uint32_t id = 0;
    uint32_t flags = 0;               // 1
    float pitchMin = 0.0f;            // 4
    float pitchMax = 0.0f;            // 5
    uint32_t seatIds[8] = {};         // 6..13
    uint32_t uiIndicatorId = 0;       // 36
};

// VehicleSeat.flags the client tests.
inline constexpr uint32_t kSeatHasEnterAnim = 0x1;          // 0x747B20
inline constexpr uint32_t kSeatHasRideAnim = 0x2;           // 0x747B20
inline constexpr uint32_t kSeatHasUpperAnim = 0x4;          // 0x747BD0
inline constexpr uint32_t kSeatControlSeat = 0x800;         // UnitInVehicleControlSeat
inline constexpr uint32_t kSeatVehicleRideAnim = 0x20000;   // 0x7489C0
inline constexpr uint32_t kSeatUsesVehicleControls = 0x800000;  // IsUsingVehicleControls
inline constexpr uint32_t kSeatCanExit = 0x2000000;         // CanExitVehicle
inline constexpr uint32_t kSeatCanSwitch = 0x4000000;       // CanSwitchVehicleSeats
inline constexpr uint32_t kSeatHasVehicleUI = 0x20000000;   // UnitHasVehicleUI
// VehicleSeat.flagsB.
inline constexpr uint32_t kSeatBCameraOnPassenger = 0x100;  // 0x759200
inline constexpr uint32_t kSeatBCanEject = 0x20;            // UnitVehicleSeatInfo canEject

/// The M2 attachment a seat's AttachmentID names (the table at 0xA2D3F0,
/// read by 0x7493B0): seat ids are not M2 attachment ids. -1 for none
/// (an id of 22 or more, or negative): the seat is then relative to the
/// vehicle's base.
inline int seatM2Attachment(int32_t seatAttachmentId) {
    static constexpr int kMap[22] = {20, 34, 19, 21, 22, 17, 23, 24, 25, 15, 16,
                                     37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 0};
    if (seatAttachmentId < 0 || seatAttachmentId >= 22) return -1;
    return kMap[seatAttachmentId];
}

/// A passenger's state (VehiclePassenger_C +0x14).
enum class PassengerState : uint8_t { None = 0, EnterDelay = 1, Entering = 2, Seated = 3,
                                      ExitDelay = 4, Exiting = 5 };

/// No animation (AnimationData's count, 0x1FA).
inline constexpr uint32_t kNoAnim = 506;

/// What a passenger's whole body plays (0x747B20): seated on a seat with
/// flag 0x2, RideAnimStart once (if it names one) then RideAnimLoop;
/// entering with 0x1 the enter pair. Otherwise kNoAnim: the ordinary stand
/// chain runs - there is no fallback to Mount (91).
struct PassengerAnim {
    uint32_t start = kNoAnim;   // one-shot first, or kNoAnim
    uint32_t loop = kNoAnim;    // then this, or kNoAnim
};
inline PassengerAnim passengerAnim(const SeatRecord& seat, PassengerState state) {
    auto id = [](int32_t a) { return (a >= 0 && a < static_cast<int32_t>(kNoAnim))
                                         ? static_cast<uint32_t>(a) : kNoAnim; };
    PassengerAnim p;
    if (state == PassengerState::Seated && (seat.flags & kSeatHasRideAnim)) {
        p.start = id(seat.rideAnimStart);
        p.loop = id(seat.rideAnimLoop);
    } else if ((state == PassengerState::EnterDelay || state == PassengerState::Entering) &&
               (seat.flags & kSeatHasEnterAnim)) {
        p.start = id(seat.enterAnimStart);
        p.loop = id(seat.enterAnimLoop);
    } else if (state == PassengerState::ExitDelay || state == PassengerState::Exiting) {
        p.start = id(seat.exitAnimStart);
        p.loop = id(seat.exitAnimLoop);
    }
    return p;
}

/// The vehicle's own loop while this seat is taken (0x7489C0): seat flag
/// 0x20000 and a VehicleRideAnimLoop below 506; kNoAnim otherwise.
inline uint32_t vehicleRideAnim(const SeatRecord& seat) {
    if (!(seat.flags & kSeatVehicleRideAnim)) return kNoAnim;
    if (seat.vehicleRideAnimLoop < 0 || seat.vehicleRideAnimLoop >= static_cast<int32_t>(kNoAnim))
        return kNoAnim;
    return static_cast<uint32_t>(seat.vehicleRideAnimLoop);
}

/// UnitVehicleSkin's answer for a seat's uiSkin.
inline const char* uiSkinName(uint32_t uiSkin) {
    return uiSkin == 1 ? "Mechanical" : "Natural";
}

}  // namespace wowee::game::vehicle
