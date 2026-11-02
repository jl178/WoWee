// Vehicles and their passengers: who is a vehicle, who sits where.
// Stock 3.3.5a: docs/re/mounts-taxi-vehicles.md section 5.
#include "game/game_handler.hpp"

#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "game/entity.hpp"
#include "game/handler_types.hpp"
#include "game/movement_handler.hpp"
#include "game/pet_action.hpp"
#include "network/world_socket.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"

#include <cmath>
#include <random>

namespace wowee::game {

void GameHandler::setUnitVehicleId(uint64_t guid, uint32_t vehicleId) {
    if (guid == 0) return;
    const uint32_t before = unitVehicleId(guid);
    if (vehicleId == 0) unitVehicleIds_.erase(guid);
    else unitVehicleIds_[guid] = vehicleId;
    // The player became (or stopped being) a vehicle - a multi-seat mount
    // (0x74C750 -> 0x74BBD0): PLAYER_GAINS_VEHICLE_DATA with the vehicle's
    // seat indicator, PLAYER_LOSES_VEHICLE_DATA.
    if (guid == playerGuid && before != vehicleId) {
        if (vehicleId != 0) {
            uint32_t indicator = 0;
            if (vehicleData_.ensureLoaded(services().assetManager))
                if (const auto* v = vehicleData_.vehicle(vehicleId)) indicator = v->uiIndicatorId;
            fireAddonEvent("PLAYER_GAINS_VEHICLE_DATA", {"player", std::to_string(indicator)});
        } else {
            fireAddonEvent("PLAYER_LOSES_VEHICLE_DATA", {"player"});
        }
    }
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
    // The UI sound a seat names, by SoundEntries name (the events carry it).
    auto soundName = [this](uint32_t soundId) -> std::string {
        auto* am = services().assetManager;
        if (soundId == 0 || !am || !am->isInitialized()) return "";
        auto dbc = am->loadDBC("SoundEntries.dbc");
        if (!dbc || !dbc->isLoaded()) return "";
        const int32_t row = dbc->findRecordById(soundId);
        return row >= 0 ? dbc->getString(static_cast<uint32_t>(row), 2) : std::string();
    };
    if (nowSeated) {
        // 0x748810: unit, has the vehicle UI (seat flag 0x20000000), the
        // skin, the seat's enter sound, the control seat (0x800), the
        // vehicle's seat indicator.
        const auto* seatRec = vehicleSeatOf(playerGuid);
        const bool hasUI = seatRec && (seatRec->flags & vehicle::kSeatHasVehicleUI);
        const bool control = seatRec && (seatRec->flags & vehicle::kSeatControlSeat);
        uint32_t indicator = 0;
        if (const auto* v = vehicleData_.vehicle(vehicleId_)) indicator = v->uiIndicatorId;
        const std::vector<std::string> args = {
            "player", hasUI ? "1" : kEventNil,
            seatRec ? vehicle::uiSkinName(seatRec->uiSkin) : "",
            seatRec ? soundName(seatRec->enterUISoundId) : "",
            control ? "1" : kEventNil, std::to_string(indicator)};
        fireAddonEvent("UNIT_ENTERING_VEHICLE", args);
        fireAddonEvent("UNIT_ENTERED_VEHICLE", args);
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
    // The unit the controls moved went out of view: the player moves
    // itself again (the server's control update may never come).
    if (movementHandler_ && guid != playerGuid && guid == movementHandler_->activeMoverGuid())
        movementHandler_->setActiveMover(0);
    unitVehicleIds_.erase(guid);
    if (guid != playerGuid) vehiclePassengers_.erase(guid);
    // Whoever sat in it is on foot as far as this client can tell.
    for (auto it = vehiclePassengers_.begin(); it != vehiclePassengers_.end();) {
        if (it->second.vehicleGuid == guid && it->first != playerGuid) it = vehiclePassengers_.erase(it);
        else ++it;
    }
}

// ============================================================
// Driving and the vehicle interface
// ============================================================

uint64_t GameHandler::activeMoverGuid() const {
    return movementHandler_ ? movementHandler_->activeMoverGuid() : playerGuid;
}

bool GameHandler::isMovingOtherUnit() const {
    return movementHandler_ && movementHandler_->movingOtherUnit();
}

uint64_t GameHandler::vehicleSeatOccupant(uint64_t vehicleGuid, int seat) const {
    for (const auto& [guid, p] : vehiclePassengers_) {
        if (p.vehicleGuid == vehicleGuid && p.seat == seat) return guid;
    }
    return 0;
}

uint64_t GameHandler::rootVehicleOf(uint64_t guid) const {
    uint64_t at = guid;
    for (int depth = 0; depth < 8; ++depth) {
        auto it = vehiclePassengers_.find(at);
        if (it == vehiclePassengers_.end()) break;
        at = it->second.vehicleGuid;
    }
    return at == guid && !unitVehicleIds_.count(guid) ? 0 : at;
}

bool GameHandler::vehicleVirtualSeat(uint64_t root, int virtualIndex, uint64_t& outVehicle, int& outSeat) {
    if (!vehicleData_.ensureLoaded(services().assetManager)) return false;
    // Depth-first in seat order (0x757550): a seat holding an accessory
    // vehicle (a unit that is no player) stands for that vehicle's seats.
    std::function<bool(uint64_t, int&, int)> walk = [&](uint64_t veh, int& left, int depth) -> bool {
        const auto* v = vehicleData_.vehicle(unitVehicleId(veh));
        if (!v || depth > 4) return false;
        for (int i = 0; i < 8; ++i) {
            const uint64_t occ = vehicleSeatOccupant(veh, i);
            const bool accessory = occ != 0 && unitVehicleId(occ) != 0 &&
                                   static_cast<uint16_t>(occ >> 48) != 0x0000;
            if (accessory) {
                if (walk(occ, left, depth + 1)) return true;
                continue;
            }
            if (v->seatIds[i] == 0) continue;
            if (left == 0) { outVehicle = veh; outSeat = i; return true; }
            --left;
        }
        return false;
    };
    int left = virtualIndex;
    return virtualIndex >= 0 && walk(root, left, 0);
}

int GameHandler::vehicleVirtualSeatCount(uint64_t root) {
    int n = 0;
    uint64_t v = 0;
    int seat = 0;
    while (n < 64 && vehicleVirtualSeat(root, n, v, seat)) ++n;
    return n;
}

const vehicle::VehicleRecord* GameHandler::aimVehicleRecord() {
    if (!vehicleData_.ensureLoaded(services().assetManager)) return nullptr;
    uint64_t unit = activeMoverGuid();
    // A passenger mover in a control seat aims its vehicle (0x5F9D20).
    if (unitVehicleId(unit) == 0) {
        const auto* seat = vehicleSeatOf(unit);
        if (seat && (seat->flags & vehicle::kSeatControlSeat)) unit = playerVehicleGuid();
    }
    return vehicleData_.vehicle(unitVehicleId(unit));
}

float GameHandler::vehicleAimPitch() const {
    return movementHandler_ ? movementHandler_->getMovementInfo().pitch : 0.0f;
}

namespace {
/// VEHICLE_ANGLE_UPDATE (0x5FA910): the normalized angle and the angle,
/// for a vehicle whose aim angle the player adjusts.
void fireAngleUpdate(GameHandler& gh, const vehicle::VehicleRecord* v, float pitch) {
    if (!v || !(v->flags & vehicle::kVehicleAdjustAimAngle)) return;
    gh.fireAddonEvent("VEHICLE_ANGLE_UPDATE",
                      {std::to_string(vehicle::aimNormAngle(v, pitch)), std::to_string(pitch)});
}
}  // namespace

void GameHandler::requestVehicleAimPitch(float pitch) {
    if (!movementHandler_ || !isMovingOtherUnit()) return;
    const auto* v = aimVehicleRecord();
    pitch = vehicle::clampAim(v, pitch);
    auto& mi = movementHandler_->getMovementInfoMut();
    if (mi.flags2 & 0x0010) {
        // FULL_SPEED_PITCHING: pitch toward it at the pitch rate (0x72D470 ->
        // 0x6EF7A0: START_PITCH_UP/DOWN now, STOP_PITCH on arrival).
        if (std::fabs(pitch - mi.pitch) < 1.0e-4f) return;
        vehicleAimTarget_ = pitch;
        vehicleAimSeeking_ = true;
        sendMovement(pitch > mi.pitch ? Opcode::MSG_MOVE_START_PITCH_UP : Opcode::MSG_MOVE_START_PITCH_DOWN);
        return;
    }
    // Otherwise at once (0x72EAD0 -> 0x6EE460 -> 0x6E9380): MSG_MOVE_SET_PITCH.
    mi.pitch = pitch;
    sendMovement(Opcode::MSG_MOVE_SET_PITCH);
    fireAngleUpdate(*this, v, pitch);
}

void GameHandler::setVehicleAimDirection(int direction) {
    if (!movementHandler_ || !isMovingOtherUnit()) return;
    if (direction == vehicleAimDirection_) return;
    vehicleAimDirection_ = direction;
    vehicleAimSeeking_ = false;
    sendMovement(direction > 0 ? Opcode::MSG_MOVE_START_PITCH_UP
                 : direction < 0 ? Opcode::MSG_MOVE_START_PITCH_DOWN
                                 : Opcode::MSG_MOVE_STOP_PITCH);
}

bool GameHandler::playerSeatHasFlag(uint32_t seatFlag) {
    const auto* seat = vehicleSeatOf(playerGuid);
    return seat && (seat->flags & seatFlag) != 0;
}

void GameHandler::vehicleExit() {
    if (state != WorldState::IN_WORLD || !socket) return;
    const uint64_t veh = playerVehicleGuid();
    if (veh == 0) return;
    if (isMovingOtherUnit() && activeMoverGuid() == veh) {
        // The driver gets off with the vehicle's movement (0x5D46F0 ->
        // 0x74BB20, event 0x34): CMSG_DISMISS_CONTROLLED_VEHICLE.
        if (!playerSeatHasFlag(vehicle::kSeatCanExit)) return;
        movementHandler_->sendMoverPacket(Opcode::CMSG_DISMISS_CONTROLLED_VEHICLE, nullptr);
        LOG_INFO("Vehicle: CMSG_DISMISS_CONTROLLED_VEHICLE for 0x", std::hex, veh, std::dec);
        return;
    }
    network::Packet pkt(wireOpcode(Opcode::CMSG_REQUEST_VEHICLE_EXIT));
    socket->send(pkt);
    LOG_INFO("Vehicle: CMSG_REQUEST_VEHICLE_EXIT");
}

void GameHandler::vehicleChangeSeat(bool next) {
    if (state != WorldState::IN_WORLD || !socket || playerVehicleGuid() == 0) return;
    if (isMovingOtherUnit() && activeMoverGuid() == playerVehicleGuid()) {
        // 0x74C8B0 / 0x74C9A0 while driving: the vehicle's movement, no
        // accessory, seat -1 (previous) or 1 (next).
        movementHandler_->sendMoverPacket(Opcode::CMSG_CHANGE_SEATS_ON_CONTROLLED_VEHICLE,
            [next](network::Packet& p) {
                p.writePackedGuid(0);
                p.writeUInt8(next ? 1u : 0xFFu);
            });
        return;
    }
    network::Packet pkt(wireOpcode(next ? Opcode::CMSG_REQUEST_VEHICLE_NEXT_SEAT
                                        : Opcode::CMSG_REQUEST_VEHICLE_PREV_SEAT));
    socket->send(pkt);
}

bool GameHandler::switchToVehicleSeat(uint64_t vehicleGuid, int seat) {
    if (state != WorldState::IN_WORLD || !socket || vehicleGuid == 0 || seat < 0 || seat > 7) return false;
    if (!vehicleData_.ensureLoaded(services().assetManager)) return false;
    // 0x74CA90: the seat is there, free, and one a passenger may switch to
    // (0x4000000); the player's own seat lets it switch.
    const auto* target = vehicleData_.seatOf(unitVehicleId(vehicleGuid), seat);
    if (!target || vehicleSeatOccupant(vehicleGuid, seat) != 0) return false;
    if (!(target->flags & vehicle::kSeatCanSwitch) || !playerSeatHasFlag(vehicle::kSeatCanSwitch)) return false;
    if (isMovingOtherUnit() && activeMoverGuid() == playerVehicleGuid()) {
        movementHandler_->sendMoverPacket(Opcode::CMSG_CHANGE_SEATS_ON_CONTROLLED_VEHICLE,
            [vehicleGuid, seat](network::Packet& p) {
                p.writePackedGuid(vehicleGuid);
                p.writeUInt8(static_cast<uint8_t>(seat));
            });
    } else {
        network::Packet pkt(wireOpcode(Opcode::CMSG_REQUEST_VEHICLE_SWITCH_SEAT));
        pkt.writePackedGuid(vehicleGuid);
        pkt.writeUInt8(static_cast<uint8_t>(seat));
        socket->send(pkt);
    }
    LOG_INFO("Vehicle: switch to seat ", seat, " of 0x", std::hex, vehicleGuid, std::dec);
    return true;
}

bool GameHandler::vehicleBarActive() {
    if (petGuid_ == 0) return false;
    // The unit the player controls (UNIT_FIELD_CHARM) is the one whose bar
    // SMSG_PET_SPELLS sent, and it has one (0x5D4AD0 -> 0x5D35B0).
    const uint16_t idx = fieldIndex(UF::UNIT_FIELD_CHARM);
    if (idx == 0xFFFF) return false;
    auto me = getEntityManager().getEntity(playerGuid);
    if (!me) return false;
    const uint64_t charm = (static_cast<uint64_t>(me->getField(static_cast<uint16_t>(idx + 1))) << 32) |
                           me->getField(idx);
    if (charm == 0 || charm != petGuid_) return false;
    bool any = false;
    for (int i = 0; i < PET_ACTION_BAR_SLOTS; ++i) any = any || petActionSlots_[i] != 0;
    if (!any) return false;
    // A vehicle's spells take the bar only from a seat that shows them
    // (VehicleSeat.VehicleAbilityDisplay == 1).
    if (unitVehicleId(charm) != 0) {
        const auto* seat = vehicleSeatOf(playerGuid);
        return seat && vehicle::seatShowsVehicleBar(*seat);
    }
    return true;
}

uint32_t GameHandler::vehicleBarAction(int slot0) {
    // Slots 120..131 are pet bar slots 0..9 (0x5AB800 with 0x5D3240), the
    // last two empty.
    if (slot0 < 120 || slot0 >= 130 || !vehicleBarActive()) return 0;
    const uint32_t packed = petActionSlots_[slot0 - 120];
    const uint32_t type = (packed >> 24) & 0x3F;
    // 0x5A80E0: a spell is type 1 or 8..17 (a vehicle's slots).
    if ((packed & 0xFFFFFF) == 0) return 0;
    if (type == 1 || (type >= 8 && type < 18) || type == 6 || type == 7) return packed;
    return 0;
}

void GameHandler::useVehicleBarAction(int slot0, uint64_t target) {
    const uint32_t packed = vehicleBarAction(slot0);
    if (packed == 0 || state != WorldState::IN_WORLD || !socket) return;
    const uint32_t type = (packed >> 24) & 0x3F;
    const uint32_t spellId = packed & 0xFFFFFF;
    if (type == 6 || type == 7) {  // a command or a stance: CMSG_PET_ACTION
        sendPetAction(packed, target);
        return;
    }
    const uint64_t caster = petGuid_;
    static uint8_t castCount = 0;
    if (++castCount == 0) ++castCount;

    // A spell with a missile trajectory goes from its source to where the
    // missile lands at the vehicle's aim (0x80AC90 -> 0x809F80 -> 0x6FCD60),
    // castFlags 0x2 with the elevation, the speed and the caster's movement.
    // The client only works that out for the unit it moves ("Missile
    // trajectory does not have active mover caster").
    const auto* missile = vehicleData_.spellMissile(services().assetManager, spellId);
    const bool trajectory = missile && (missile->flags & 0x1) && caster == activeMoverGuid();

    network::Packet pkt(wireOpcode(Opcode::CMSG_PET_CAST_SPELL));
    pkt.writeUInt64(caster);
    pkt.writeUInt8(castCount);
    pkt.writeUInt32(spellId);
    pkt.writeUInt8(trajectory ? 0x02 : 0x00);
    if (trajectory) {
        auto casterEntity = getEntityManager().getEntity(caster);
        const auto& mi = movementHandler_->getMovementInfo();
        // Canonical -> server axes for the flight (z up in both).
        glm::vec3 srcCanon(mi.x, mi.y, mi.z + 1.5f);
        if (!casterEntity) srcCanon = glm::vec3(mi.x, mi.y, mi.z + 1.5f);
        const glm::vec3 src = core::coords::canonicalToServer(srcCanon);
        const glm::vec2 es = vehicle::missileElevationSpeed(*missile, aimVehicleRecord(),
                                                            mi.pitch, vehicleAimPower_);
        float elevation = es.x, speed = es.y;
        float facing = core::coords::canonicalToServerYaw(mi.orientation);
        // The missile's randomization (0x6FCD60 with param_10 = 0).
        static std::mt19937 rng{std::random_device{}()};
        auto between = [&](float a, float b) {
            return a + (b - a) * std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
        };
        elevation += between(missile->randomizePitchMin, missile->randomizePitchMax);
        speed += between(missile->randomizeSpeedMin, missile->randomizeSpeedMax);
        facing += between(missile->randomizeFacingMin, missile->randomizeFacingMax);
        auto hits = [this](const glm::vec3& a, const glm::vec3& b, float& frac) {
            if (!worldSegmentHit_) return false;
            return worldSegmentHit_(core::coords::serverToCanonical(a),
                                    core::coords::serverToCanonical(b), frac);
        };
        float seconds = 0.0f;
        const glm::vec3 dest = vehicle::simulateMissile(*missile, src, facing, elevation, speed, hits, &seconds);
        pkt.writeUInt32(0x20 | 0x40);  // TARGET_FLAG_SOURCE_LOCATION | TARGET_FLAG_DEST_LOCATION
        pkt.writePackedGuid(0);
        pkt.writeFloat(src.x); pkt.writeFloat(src.y); pkt.writeFloat(src.z);
        pkt.writePackedGuid(0);
        pkt.writeFloat(dest.x); pkt.writeFloat(dest.y); pkt.writeFloat(dest.z);
        pkt.writeFloat(elevation);
        pkt.writeFloat(speed);
        // The caster's movement as a MSG_MOVE_STOP (0x71F060 with 0xB7).
        pkt.writeUInt8(1);
        pkt.writeUInt32(wireOpcode(Opcode::MSG_MOVE_STOP));
        movementHandler_->writeMoverMovement(pkt);
        LOG_INFO("Vehicle cast ", spellId, " (trajectory): elev=", elevation, " speed=", speed,
                 " src=(", src.x, ",", src.y, ",", src.z, ") dest=(", dest.x, ",", dest.y, ",",
                 dest.z, ") flight=", seconds, "s");
    } else if (target != 0) {
        pkt.writeUInt32(0x02);  // TARGET_FLAG_UNIT
        pkt.writePackedGuid(target);
        LOG_INFO("Vehicle cast ", spellId, " at 0x", std::hex, target, std::dec);
    } else {
        pkt.writeUInt32(0x00);
        LOG_INFO("Vehicle cast ", spellId);
    }
    socket->send(pkt);
}

void GameHandler::onActiveMoverChanged() {
    // 0x747F40: VEHICLE_UPDATE; then (0x74C5A0) VEHICLE_ANGLE_SHOW and
    // VEHICLE_POWER_SHOW with 1 for a vehicle whose aim angle / power the
    // player sets, and the angle as it is.
    vehicleAimDirection_ = 0;
    vehicleAimSeeking_ = false;
    fireAddonEvent("VEHICLE_UPDATE", {});
    const vehicle::VehicleRecord* v = isMovingOtherUnit() ? aimVehicleRecord() : nullptr;
    if (v && (v->flags & vehicle::kVehicleAdjustAimAngle)) fireAddonEvent("VEHICLE_ANGLE_SHOW", {"1"});
    else fireAddonEvent("VEHICLE_ANGLE_SHOW", {});
    if (v && (v->flags & vehicle::kVehicleAdjustAimPower)) fireAddonEvent("VEHICLE_POWER_SHOW", {"1"});
    else fireAddonEvent("VEHICLE_POWER_SHOW", {});
    fireAngleUpdate(*this, v, vehicleAimPitch());
    fireAddonEvent("UPDATE_BONUS_ACTIONBAR", {});
}

void GameHandler::updateVehicle(float deltaTime) {
    // The bonus bar follows the vehicle bar (0x5A83C0: offset 5 while it is up).
    const bool bar = vehicleBarActive();
    if (bar != vehicleBarWasActive_) {
        vehicleBarWasActive_ = bar;
        fireAddonEvent("UPDATE_BONUS_ACTIONBAR", {});
        fireAddonEvent("ACTIONBAR_PAGE_CHANGED", {});
        for (int slot = 121; slot <= 132; ++slot)
            fireAddonEvent("ACTIONBAR_SLOT_CHANGED", {std::to_string(slot)});
    }
    if (!movementHandler_ || !isMovingOtherUnit()) {
        vehicleAimDirection_ = 0;
        vehicleAimSeeking_ = false;
        return;
    }
    if (vehicleAimDirection_ == 0 && !vehicleAimSeeking_) return;
    // Pitching at the mover's pitch rate, held to the vehicle's limits.
    auto& mi = movementHandler_->getMovementInfoMut();
    const auto* v = aimVehicleRecord();
    const float rate = movementHandler_->getServerPitchRate() * deltaTime;
    float pitch = mi.pitch;
    if (vehicleAimSeeking_) {
        const float d = vehicleAimTarget_ - pitch;
        if (std::fabs(d) <= rate) {
            pitch = vehicleAimTarget_;
            vehicleAimSeeking_ = false;
        } else {
            pitch += d > 0.0f ? rate : -rate;
        }
    } else {
        pitch += static_cast<float>(vehicleAimDirection_) * rate;
    }
    pitch = vehicle::clampAim(v, pitch);
    if (pitch != mi.pitch) {
        mi.pitch = pitch;
        fireAngleUpdate(*this, v, pitch);
    }
    if (!vehicleAimSeeking_ && vehicleAimDirection_ == 0) sendMovement(Opcode::MSG_MOVE_STOP_PITCH);
}

}  // namespace wowee::game
