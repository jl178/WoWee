#pragma once

// Vehicle.dbc and VehicleSeat.dbc, read once (WotLK column order; see
// game/vehicle_rules.hpp for the fields).

#include "game/vehicle_rules.hpp"

#include <unordered_map>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace game {

class VehicleData {
public:
    /// Reads both files the first time; false when they are not there.
    bool ensureLoaded(pipeline::AssetManager* assets);
    [[nodiscard]] const vehicle::VehicleRecord* vehicle(uint32_t id) const;
    [[nodiscard]] const vehicle::SeatRecord* seat(uint32_t id) const;
    /// Seat `index` (0..7) of vehicle `vehicleId`: VehicleSeat[veh+0x18+4i]
    /// (0x756EC0). nullptr when the vehicle or the seat is not there.
    [[nodiscard]] const vehicle::SeatRecord* seatOf(uint32_t vehicleId, int index) const;

private:
    bool tried_ = false;
    bool loaded_ = false;
    std::unordered_map<uint32_t, vehicle::VehicleRecord> vehicles_;
    std::unordered_map<uint32_t, vehicle::SeatRecord> seats_;
};

} // namespace game
} // namespace wowee
