#pragma once

// Vehicle.dbc and VehicleSeat.dbc, read once (WotLK column order; see
// game/vehicle_rules.hpp for the fields).

#include "game/vehicle_rules.hpp"

#include <string>
#include <unordered_map>
#include <vector>

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

    /// The SpellMissile row of a spell (Spell.dbc field 227, the client's
    /// SpellRec +0x28C), nullptr when it has none.
    [[nodiscard]] const vehicle::MissileRecord* spellMissile(pipeline::AssetManager* assets, uint32_t spellId);

    /// GetVehicleUIIndicator / GetVehicleUIIndicatorSeat (0x614E60, 0x614EF0;
    /// VehicleUIIndicator.dbc, VehicleUIIndSeat.dbc in file order).
    struct UiIndicatorSeat { uint32_t virtualSeatIndex = 0; float x = 0.0f, y = 0.0f; };
    struct UiIndicator { std::string background; std::vector<UiIndicatorSeat> seats; };
    [[nodiscard]] const UiIndicator* uiIndicator(pipeline::AssetManager* assets, uint32_t id);

private:
    bool missilesTried_ = false;
    std::unordered_map<uint32_t, uint32_t> spellMissileIds_;
    std::unordered_map<uint32_t, vehicle::MissileRecord> missiles_;
    bool indicatorsTried_ = false;
    std::unordered_map<uint32_t, UiIndicator> indicators_;
    bool tried_ = false;
    bool loaded_ = false;
    std::unordered_map<uint32_t, vehicle::VehicleRecord> vehicles_;
    std::unordered_map<uint32_t, vehicle::SeatRecord> seats_;
};

} // namespace game
} // namespace wowee
