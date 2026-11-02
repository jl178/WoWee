// Vehicles and their passengers: who is a vehicle, who sits where.
// Stock 3.3.5a: docs/re/mounts-taxi-vehicles.md section 5.
#include "game/game_handler.hpp"

#include "core/logger.hpp"
#include "game/handler_types.hpp"

namespace wowee::game {

void GameHandler::setUnitVehicleId(uint64_t guid, uint32_t vehicleId) {
    if (guid == 0) return;
    if (vehicleId == 0) unitVehicleIds_.erase(guid);
    else unitVehicleIds_[guid] = vehicleId;
    // The player's own vehicle changing seats under it (a mammoth's id
    // arriving after its passengers): refresh what "in a vehicle" means.
    if (auto it = vehiclePassengers_.find(playerGuid);
        it != vehiclePassengers_.end() && it->second.vehicleGuid == guid) {
        vehicleId_ = vehicleId;
    }
}

uint32_t GameHandler::unitVehicleId(uint64_t guid) const {
    auto it = unitVehicleIds_.find(guid);
    return it != unitVehicleIds_.end() ? it->second : 0u;
}

bool GameHandler::isVehicleTransport(uint64_t transportGuid) const {
    if (transportGuid == 0) return false;
    // 0x74B8B0: a vehicle's high type (0xF150) or a player.
    const uint16_t high = static_cast<uint16_t>(transportGuid >> 48);
    if (high == 0xF150) return true;
    if (unitVehicleIds_.count(transportGuid)) return true;
    if (high == 0x0000) {
        auto e = entityController_ ? getEntityManager().getEntity(transportGuid) : nullptr;
        return e && e->getType() == ObjectType::PLAYER;
    }
    return false;
}

void GameHandler::setVehiclePassenger(uint64_t guid, uint64_t vehicleGuid, int8_t seat) {
    if (guid == 0) return;
    const bool wasPlayerSeated = vehiclePassengers_.count(playerGuid) != 0;
    if (vehicleGuid == 0) {
        if (!vehiclePassengers_.erase(guid)) return;
    } else {
        auto& p = vehiclePassengers_[guid];
        if (p.vehicleGuid == vehicleGuid && p.seat == seat) return;
        p.vehicleGuid = vehicleGuid;
        p.seat = seat;
        LOG_INFO("Vehicle passenger: 0x", std::hex, guid, " in 0x", vehicleGuid, std::dec,
                 " seat ", static_cast<int>(seat), " vehicleId=", unitVehicleId(vehicleGuid));
    }
    if (guid != playerGuid) return;

    const bool nowSeated = vehicleGuid != 0;
    vehicleId_ = nowSeated ? unitVehicleId(vehicleGuid) : 0u;
    // A seat with no Vehicle row known yet still counts as being in one.
    if (nowSeated && vehicleId_ == 0) vehicleId_ = 0xFFFFFFFFu;
    if (wasPlayerSeated == nowSeated) {
        fireAddonEvent("VEHICLE_PASSENGERS_CHANGED", {});
        return;
    }
    if (vehicleStateCallback_) vehicleStateCallback_(nowSeated, vehicleId_);
    fireAddonEvent("VEHICLE_UPDATE", {});
    if (nowSeated) {
        const auto* seatRec = vehicleSeatOf(playerGuid);
        const bool hasUI = seatRec && (seatRec->flags & vehicle::kSeatHasVehicleUI);
        fireAddonEvent("UNIT_ENTERING_VEHICLE",
                       {"player", hasUI ? "1" : kEventNil,
                        seatRec ? vehicle::uiSkinName(seatRec->uiSkin) : ""});
        fireAddonEvent("UNIT_ENTERED_VEHICLE", {"player", hasUI ? "1" : kEventNil});
    } else {
        fireAddonEvent("UNIT_EXITING_VEHICLE", {"player"});
        fireAddonEvent("UNIT_EXITED_VEHICLE", {"player"});
    }
    fireAddonEvent("UPDATE_BONUS_ACTIONBAR", {});
}

const vehicle::SeatRecord* GameHandler::vehicleSeatOf(uint64_t guid) {
    auto it = vehiclePassengers_.find(guid);
    if (it == vehiclePassengers_.end()) return nullptr;
    if (!vehicleData_.ensureLoaded(services().assetManager)) return nullptr;
    return vehicleData_.seatOf(unitVehicleId(it->second.vehicleGuid), it->second.seat);
}

void GameHandler::forgetVehicleUnit(uint64_t guid) {
    unitVehicleIds_.erase(guid);
    if (guid != playerGuid) vehiclePassengers_.erase(guid);
    // Whoever sat in it is on foot as far as this client can tell.
    for (auto it = vehiclePassengers_.begin(); it != vehiclePassengers_.end();) {
        if (it->second.vehicleGuid == guid && it->first != playerGuid) it = vehiclePassengers_.erase(it);
        else ++it;
    }
}

}  // namespace wowee::game
