#include "game/vehicle_data.hpp"

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"

namespace wowee::game {

bool VehicleData::ensureLoaded(pipeline::AssetManager* assets) {
    if (tried_) return loaded_;
    if (!assets || !assets->isInitialized()) return false;
    tried_ = true;

    auto seatDbc = assets->loadDBC("VehicleSeat.dbc");
    auto vehDbc = assets->loadDBC("Vehicle.dbc");
    if (!seatDbc || !seatDbc->isLoaded() || !vehDbc || !vehDbc->isLoaded()) {
        LOG_WARNING("Vehicle.dbc / VehicleSeat.dbc not available: vehicles seat nobody");
        return false;
    }
    if (seatDbc->getFieldCount() < 58 || vehDbc->getFieldCount() < 40) {
        LOG_WARNING("VehicleSeat.dbc / Vehicle.dbc are not the 3.3.5 layout (",
                    seatDbc->getFieldCount(), "/", vehDbc->getFieldCount(), " fields)");
        return false;
    }
    for (uint32_t r = 0; r < seatDbc->getRecordCount(); ++r) {
        vehicle::SeatRecord s;
        s.id = seatDbc->getUInt32(r, 0);
        s.flags = seatDbc->getUInt32(r, 1);
        s.attachmentId = seatDbc->getInt32(r, 2);
        s.attachmentOffset = {seatDbc->getFloat(r, 3), seatDbc->getFloat(r, 4), seatDbc->getFloat(r, 5)};
        s.enterAnimStart = seatDbc->getInt32(r, 13);
        s.enterAnimLoop = seatDbc->getInt32(r, 14);
        s.rideAnimStart = seatDbc->getInt32(r, 15);
        s.rideAnimLoop = seatDbc->getInt32(r, 16);
        s.rideUpperAnimStart = seatDbc->getInt32(r, 17);
        s.rideUpperAnimLoop = seatDbc->getInt32(r, 18);
        s.exitAnimStart = seatDbc->getInt32(r, 26);
        s.exitAnimLoop = seatDbc->getInt32(r, 27);
        s.passengerYaw = seatDbc->getFloat(r, 29);
        s.passengerPitch = seatDbc->getFloat(r, 30);
        s.passengerRoll = seatDbc->getFloat(r, 31);
        s.passengerAttachmentId = seatDbc->getInt32(r, 32);
        s.vehicleRideAnimLoop = seatDbc->getInt32(r, 35);
        s.uiSkin = seatDbc->getUInt32(r, 44);
        s.flagsB = seatDbc->getUInt32(r, 45);
        s.cameraOffset = {seatDbc->getFloat(r, 50), seatDbc->getFloat(r, 51), seatDbc->getFloat(r, 52)};
        seats_[s.id] = s;
    }
    for (uint32_t r = 0; r < vehDbc->getRecordCount(); ++r) {
        vehicle::VehicleRecord v;
        v.id = vehDbc->getUInt32(r, 0);
        v.flags = vehDbc->getUInt32(r, 1);
        v.pitchMin = vehDbc->getFloat(r, 4);
        v.pitchMax = vehDbc->getFloat(r, 5);
        for (int i = 0; i < 8; ++i) v.seatIds[i] = vehDbc->getUInt32(r, 6 + static_cast<uint32_t>(i));
        v.uiIndicatorId = vehDbc->getUInt32(r, 36);
        vehicles_[v.id] = v;
    }
    loaded_ = true;
    LOG_INFO("Vehicles: ", vehicles_.size(), " vehicles, ", seats_.size(), " seats");
    return true;
}

const vehicle::VehicleRecord* VehicleData::vehicle(uint32_t id) const {
    auto it = vehicles_.find(id);
    return it != vehicles_.end() ? &it->second : nullptr;
}

const vehicle::SeatRecord* VehicleData::seat(uint32_t id) const {
    auto it = seats_.find(id);
    return it != seats_.end() ? &it->second : nullptr;
}

const vehicle::SeatRecord* VehicleData::seatOf(uint32_t vehicleId, int index) const {
    if (index < 0 || index >= 8) return nullptr;
    const auto* v = vehicle(vehicleId);
    if (!v || v->seatIds[index] == 0) return nullptr;
    return seat(v->seatIds[index]);
}

}  // namespace wowee::game
