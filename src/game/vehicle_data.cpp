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
        s.enterPreDelay = seatDbc->getFloat(r, 6);
        s.enterSpeed = seatDbc->getFloat(r, 7);
        s.enterGravity = seatDbc->getFloat(r, 8);
        s.enterMinDuration = seatDbc->getFloat(r, 9);
        s.enterMaxDuration = seatDbc->getFloat(r, 10);
        s.enterMinArcHeight = seatDbc->getFloat(r, 11);
        s.enterMaxArcHeight = seatDbc->getFloat(r, 12);
        s.exitPreDelay = seatDbc->getFloat(r, 19);
        s.exitSpeed = seatDbc->getFloat(r, 20);
        s.exitGravity = seatDbc->getFloat(r, 21);
        s.exitMinDuration = seatDbc->getFloat(r, 22);
        s.exitMaxDuration = seatDbc->getFloat(r, 23);
        s.exitMinArcHeight = seatDbc->getFloat(r, 24);
        s.exitMaxArcHeight = seatDbc->getFloat(r, 25);
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
        s.vehicleAbilityDisplay = seatDbc->getUInt32(r, 41);
        s.enterUISoundId = seatDbc->getUInt32(r, 42);
        s.exitUISoundId = seatDbc->getUInt32(r, 43);
        s.uiSkin = seatDbc->getUInt32(r, 44);
        s.flagsB = seatDbc->getUInt32(r, 45);
        s.cameraOffset = {seatDbc->getFloat(r, 50), seatDbc->getFloat(r, 51), seatDbc->getFloat(r, 52)};
        s.cameraPosChaseRate = seatDbc->getFloat(r, 53);
        s.cameraFacingChaseRate = seatDbc->getFloat(r, 54);
        seats_[s.id] = s;
    }
    for (uint32_t r = 0; r < vehDbc->getRecordCount(); ++r) {
        vehicle::VehicleRecord v;
        v.id = vehDbc->getUInt32(r, 0);
        v.flags = vehDbc->getUInt32(r, 1);
        v.turnSpeed = vehDbc->getFloat(r, 2);
        v.pitchSpeed = vehDbc->getFloat(r, 3);
        v.pitchMin = vehDbc->getFloat(r, 4);
        v.pitchMax = vehDbc->getFloat(r, 5);
        for (int i = 0; i < 8; ++i) v.seatIds[i] = vehDbc->getUInt32(r, 6 + static_cast<uint32_t>(i));
        v.mouseLookOffsetPitch = vehDbc->getFloat(r, 14);
        v.cameraFadeDistScalarMin = vehDbc->getFloat(r, 15);
        v.cameraFadeDistScalarMax = vehDbc->getFloat(r, 16);
        v.cameraPitchOffset = vehDbc->getFloat(r, 17);
        v.facingLimitRight = vehDbc->getFloat(r, 18);
        v.facingLimitLeft = vehDbc->getFloat(r, 19);
        v.cameraYawOffset = vehDbc->getFloat(r, 33);
        v.uiIndicatorId = vehDbc->getUInt32(r, 36);
        vehicles_[v.id] = v;
    }
    loaded_ = true;
    LOG_INFO("Vehicles: ", vehicles_.size(), " vehicles, ", seats_.size(), " seats");
    return true;
}

const vehicle::MissileRecord* VehicleData::spellMissile(pipeline::AssetManager* assets, uint32_t spellId) {
    if (!missilesTried_) {
        if (!assets || !assets->isInitialized()) return nullptr;
        missilesTried_ = true;
        auto spellDbc = assets->loadDBC("Spell.dbc");
        auto missDbc = assets->loadDBC("SpellMissile.dbc");
        if (spellDbc && spellDbc->isLoaded() && spellDbc->getFieldCount() >= 228 &&
            missDbc && missDbc->isLoaded() && missDbc->getFieldCount() >= 15) {
            for (uint32_t r = 0; r < spellDbc->getRecordCount(); ++r) {
                const uint32_t m = spellDbc->getUInt32(r, 227);
                if (m != 0) spellMissileIds_[spellDbc->getUInt32(r, 0)] = m;
            }
            for (uint32_t r = 0; r < missDbc->getRecordCount(); ++r) {
                vehicle::MissileRecord m;
                m.id = missDbc->getUInt32(r, 0);
                m.flags = missDbc->getUInt32(r, 1);
                m.defaultPitchMin = missDbc->getFloat(r, 2);
                m.defaultPitchMax = missDbc->getFloat(r, 3);
                m.defaultSpeedMin = missDbc->getFloat(r, 4);
                m.defaultSpeedMax = missDbc->getFloat(r, 5);
                m.randomizeFacingMin = missDbc->getFloat(r, 6);
                m.randomizeFacingMax = missDbc->getFloat(r, 7);
                m.randomizePitchMin = missDbc->getFloat(r, 8);
                m.randomizePitchMax = missDbc->getFloat(r, 9);
                m.randomizeSpeedMin = missDbc->getFloat(r, 10);
                m.randomizeSpeedMax = missDbc->getFloat(r, 11);
                m.gravity = missDbc->getFloat(r, 12);
                m.maxDuration = missDbc->getFloat(r, 13);
                m.collisionRadius = missDbc->getFloat(r, 14);
                missiles_[m.id] = m;
            }
        } else {
            LOG_WARNING("Spell.dbc / SpellMissile.dbc not readable: no missile trajectories");
        }
    }
    auto it = spellMissileIds_.find(spellId);
    if (it == spellMissileIds_.end()) return nullptr;
    auto m = missiles_.find(it->second);
    return m != missiles_.end() ? &m->second : nullptr;
}

const VehicleData::UiIndicator* VehicleData::uiIndicator(pipeline::AssetManager* assets, uint32_t id) {
    if (!indicatorsTried_) {
        if (!assets || !assets->isInitialized()) return nullptr;
        indicatorsTried_ = true;
        auto ind = assets->loadDBC("VehicleUIIndicator.dbc");
        auto seats = assets->loadDBC("VehicleUIIndSeat.dbc");
        if (ind && ind->isLoaded() && ind->getFieldCount() >= 2) {
            for (uint32_t r = 0; r < ind->getRecordCount(); ++r)
                indicators_[ind->getUInt32(r, 0)].background = ind->getString(r, 1);
        }
        if (seats && seats->isLoaded() && seats->getFieldCount() >= 5) {
            for (uint32_t r = 0; r < seats->getRecordCount(); ++r) {
                auto it = indicators_.find(seats->getUInt32(r, 1));
                if (it == indicators_.end()) continue;
                UiIndicatorSeat s;
                s.virtualSeatIndex = seats->getUInt32(r, 2);
                s.x = seats->getFloat(r, 3);
                s.y = seats->getFloat(r, 4);
                it->second.seats.push_back(s);
            }
        }
    }
    auto it = indicators_.find(id);
    return it != indicators_.end() ? &it->second : nullptr;
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
