// The pet bar, pet talents and the pet tables as the stock client keeps them.
// Wow.exe 12340: PetInfo.cpp (0x5D2FF0..0x5D6E60), TalentInfo.cpp
// (0x5C5A20..0x5C9F10), Unit_C.cpp's pet field callbacks (0x73C330,
// 0x72CF70, 0x72CFF0). docs/re/pets.md has the addresses and the reasoning.

#include "game/pet_info.hpp"

#include "core/logger.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "game/pet_action.hpp"
#include "game/spell_classification.hpp"
#include "network/world_socket.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"

#include <chrono>

namespace wowee {
namespace game {

// ---------------------------------------------------------------------------
// PetDbc
// ---------------------------------------------------------------------------

namespace {
// CreatureFamily.dbc (3.3.5): ID, MinScale, MinScaleLevel, MaxScale,
// MaxScaleLevel, SkillLine[2], PetFoodMask, PetTalentType, CategoryEnumID,
// Name[16]+flags, IconFile.
constexpr uint32_t kCfFields = 28, kCfFood = 7, kCfTalentType = 8, kCfCategory = 9, kCfName = 10,
                   kCfIcon = 27;
// ItemPetFood.dbc: ID, Name[16]+flags.
constexpr uint32_t kIpfName = 1;
// PetPersonality.dbc: ID, Name[16]+flags, HappinessThreshold[3], HappinessDamage[3].
constexpr uint32_t kPpFields = 24, kPpThreshold = 18, kPpDamage = 21;

int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

void PetDbc::ensureLoaded(pipeline::AssetManager* am) {
    if (loaded_ || !am || !am->isInitialized()) return;
    loaded_ = true;

    if (auto cf = am->loadDBC("CreatureFamily.dbc"); cf && cf->isLoaded()) {
        if (cf->getFieldCount() >= kCfFields) {
            for (uint32_t i = 0; i < cf->getRecordCount(); ++i) {
                CreatureFamilyInfo f;
                f.id = cf->getUInt32(i, 0);
                if (f.id == 0) continue;
                f.petFoodMask = cf->getUInt32(i, kCfFood);
                f.petTalentType = cf->getInt32(i, kCfTalentType);
                f.categoryEnumId = cf->getInt32(i, kCfCategory);
                f.name = cf->getString(i, kCfName);
                f.icon = cf->getString(i, kCfIcon);
                families_[f.id] = std::move(f);
            }
        } else {
            LOG_WARNING("PetDbc: CreatureFamily.dbc has ", cf->getFieldCount(),
                        " fields, the 3.3.5 layout has ", kCfFields, "; skipped");
        }
    }
    if (auto food = am->loadDBC("ItemPetFood.dbc"); food && food->isLoaded()) {
        for (uint32_t i = 0; i < food->getRecordCount(); ++i) {
            const uint32_t id = food->getUInt32(i, 0);
            if (id != 0) foods_.emplace_back(id, food->getString(i, kIpfName));
        }
    }
    if (auto pp = am->loadDBC("PetPersonality.dbc"); pp && pp->isLoaded() &&
        pp->getFieldCount() >= kPpFields) {
        // The client reads the row with the lowest id, which is 1 (0x71F390).
        for (uint32_t i = 0; i < pp->getRecordCount(); ++i) {
            if (pp->getUInt32(i, 0) != 1) continue;
            for (int k = 0; k < 3; ++k) {
                happiness_.threshold[k] = pp->getUInt32(i, kPpThreshold + k);
                happiness_.damage[k] = pp->getFloat(i, kPpDamage + k);
            }
        }
    }
    if (auto ssp = am->loadDBC("StableSlotPrices.dbc"); ssp && ssp->isLoaded() && ssp->getFieldCount() >= 2) {
        for (uint32_t i = 0; i < ssp->getRecordCount(); ++i)
            slotPrices_[ssp->getUInt32(i, 0)] = ssp->getUInt32(i, 1);
    }
    LOG_INFO("PetDbc: ", families_.size(), " creature families, ", foods_.size(), " diets");
}

const CreatureFamilyInfo* PetDbc::family(uint32_t id) const {
    auto it = families_.find(id);
    return it != families_.end() ? &it->second : nullptr;
}

std::vector<std::string> PetDbc::foodNames(uint32_t mask) const {
    std::vector<std::string> out;
    for (const auto& [id, name] : foods_) {
        if (id >= 1 && id <= 32 && (mask & (1u << (id - 1)))) out.push_back(name);
    }
    return out;
}

// ---------------------------------------------------------------------------
// GameHandler: the pet bar
// ---------------------------------------------------------------------------

const PetDbc& GameHandler::petDbc() {
    if (!petDbc_) petDbc_ = std::make_unique<PetDbc>();
    petDbc_->ensureLoaded(services().assetManager);
    return *petDbc_;
}

uint32_t GameHandler::petCreatureFamily() const {
    if (petGuid_ == 0) return 0;
    auto u = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(petGuid_));
    if (!u) return petFamily_;
    const uint32_t fam = getCreatureFamily(u->getEntry());
    return fam ? fam : petFamily_;
}

void GameHandler::setPetAttacking(bool on, bool fromFlag) {
    // PET_ATTACK_START / STOP belong to the flag (0x73C330 -> 0x5D30A0); a
    // PetAttack the client sends sets the state silently and redraws the bar.
    if (fromFlag && on != petAttacking_) {
        fireAddonEvent(on ? "PET_ATTACK_START" : "PET_ATTACK_STOP", {});
    }
    petAttacking_ = on;
}

void GameHandler::setPetDuration(uint32_t ms) {
    petExpireMs_ = ms ? steadyMs() + ms : 0;
}

double GameHandler::getPetTimeRemaining() const {
    if (petExpireMs_ == 0 || petGuid_ == 0) return -1.0;
    const int64_t left = petExpireMs_ - steadyMs();
    return left > 0 ? static_cast<double>(left) : 0.0;
}

bool GameHandler::isHunterPet() const {
    if (petGuid_ == 0) return false;
    auto pet = getEntityManager().getEntity(petGuid_);
    if (!pet) return false;
    const uint16_t petNumber = fieldIndex(UF::UNIT_FIELD_PETNUMBER);
    if (petNumber != 0xFFFF && pet->getField(petNumber) == 0) return false;
    const uint16_t createdBy = fieldIndex(UF::UNIT_FIELD_CREATEDBY_LO);
    if (createdBy == 0xFFFF) return getPlayerClass() == 3;
    const uint64_t creator = (static_cast<uint64_t>(pet->getField(createdBy + 1)) << 32) |
                             pet->getField(createdBy);
    if (creator == 0) return false;
    if (creator == playerGuid) return getPlayerClass() == 3;
    // Someone else's - only ever asked about the player's own, but the client
    // reads the creator's class either way.
    auto owner = getEntityManager().getEntity(creator);
    const uint16_t bytes0 = fieldIndex(UF::UNIT_FIELD_BYTES_0);
    return owner && bytes0 != 0xFFFF && ((owner->getField(bytes0) >> 8) & 0xFF) == 3;
}

bool GameHandler::petCanTakeOrders(bool forSpell) const {
    if (petGuid_ == 0) return false;
    auto me = getEntityManager().getEntity(playerGuid);
    auto pet = getEntityManager().getEntity(petGuid_);
    if (!me || !pet) return false;
    auto guidAt = [](const Entity& e, uint16_t idx) -> uint64_t {
        if (idx == 0xFFFF) return 0;
        return (static_cast<uint64_t>(e.getField(static_cast<uint16_t>(idx + 1))) << 32) | e.getField(idx);
    };
    // A charmed player gives no orders.
    if (guidAt(*me, fieldIndex(UF::UNIT_FIELD_CHARMEDBY)) != 0) return false;
    // Controlling something else (a possessed unit, a vehicle) leaves only that.
    const uint64_t charm = guidAt(*me, fieldIndex(UF::UNIT_FIELD_CHARM));
    if (charm != 0 && charm != petGuid_) return false;
    // The pet is the player's: charmed by, else summoned by, the player.
    uint64_t owner = guidAt(*pet, fieldIndex(UF::UNIT_FIELD_CHARMEDBY));
    if (owner == 0) owner = guidAt(*pet, fieldIndex(UF::UNIT_FIELD_SUMMONEDBY_LO));
    if (owner != playerGuid) return false;
    if (!forSpell) {
        auto unit = std::dynamic_pointer_cast<Unit>(pet);
        if (unit && unit->getHealth() == 0 && unit->getMaxHealth() > 0) return false;
    }
    return true;
}

bool GameHandler::petCanPayFor(uint32_t spellId) const {
    if (!spellDataResolver_ || petGuid_ == 0) return true;
    auto unit = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(petGuid_));
    if (!unit) return true;
    const SpellDataInfo info = spellDataResolver_(spellId);
    if (info.powerType > 6) return true;
    uint32_t cost = info.manaCost;
    if (info.manaCostPct != 0) {
        constexpr uint8_t kPowerMana = 0;
        const uint16_t baseMana = fieldIndex(UF::UNIT_FIELD_BASE_MANA);
        const uint32_t pool = (info.powerType == kPowerMana && baseMana != 0xFFFF)
            ? unit->getField(baseMana) : unit->getMaxPowerByType(info.powerType);
        cost += spellclass::percentPowerCost(pool, info.manaCostPct);
    }
    return cost == 0 || unit->getPowerByType(info.powerType) >= cost;
}

void GameHandler::petCommand(uint32_t command, uint64_t target) {
    if (petGuid_ == 0 || !petCanTakeOrders(false)) return;
    if (command < pet::kAttack) {
        petMode_ = (petMode_ & 0x080000FFu) | (command << 8);
        fireAddonEvent("PET_BAR_UPDATE", {});
    } else if (command == pet::kAttack) {
        // Nothing is sent for a target the pet cannot attack (0x5D4210 ->
        // 0x72BDB0): the click is simply not taken.
        auto t = target ? getEntityManager().getEntity(target) : nullptr;
        auto unit = std::dynamic_pointer_cast<Unit>(t);
        const bool attackable = unit && !(unit->getHealth() == 0 && unit->getMaxHealth() > 0) &&
                                (unit->isHostile() || !isFriendlyFaction(unit->getFactionTemplate()));
        if (!attackable) {
            LOG_DEBUG("PetAttack: target 0x", std::hex, target, std::dec, " cannot be attacked");
            return;
        }
        petAttacking_ = true;
        fireAddonEvent("PET_BAR_UPDATE", {});
    }
    sendPetAction(pet::packPetAction(pet::ActionType::Command, command), target);
}

void GameHandler::petSetReact(uint32_t react) {
    if (petGuid_ == 0 || !petCanTakeOrders(false)) return;
    petMode_ = (petMode_ & 0xFFFFFF00u) | (react & 0xFFu);
    fireAddonEvent("PET_BAR_UPDATE", {});
    sendPetAction(pet::packPetAction(pet::ActionType::Reaction, react), 0);
}

void GameHandler::petStopAttack() {
    if (!petAttacking_ || petGuid_ == 0 || state != WorldState::IN_WORLD || !socket) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_PET_STOP_ATTACK));
    pkt.writeUInt64(petGuid_);
    socket->send(pkt);
    petAttacking_ = false;
}

void GameHandler::petDismiss() {
    if (petGuid_ == 0 || !petCanTakeOrders(false)) return;
    sendPetAction(pet::packPetAction(pet::ActionType::Command, pet::kAbandon), 0);
    // The cast bar's ten-second countdown (0x5D46F0 -> event 0x176).
    fireAddonEvent("PET_DISMISS_START", {"10000"});
}

namespace {
void sendSetAction(GameHandler& gh, network::WorldSocket* socket, uint64_t petGuid,
                   const pet::SlotWrite* writes, int count) {
    if (!socket || petGuid == 0 || count <= 0) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_PET_SET_ACTION));
    pkt.writeUInt64(petGuid);
    for (int i = 0; i < count; ++i) {
        pkt.writeUInt32(writes[i].slot);
        pkt.writeUInt32(writes[i].value);
    }
    socket->send(pkt);
    (void)gh;
}
} // namespace

void GameHandler::petToggleAutocastSlot(int slot) {
    if (slot < 0 || slot >= PET_ACTION_BAR_SLOTS || petGuid_ == 0 || !petCanTakeOrders(false)) return;
    uint32_t& v = petActionSlots_[slot];
    if (pet::slotAutocastAllowed(v)) {
        v ^= pet::kSlotAutocastOn;
        const uint32_t spellId = v & 0x00FFFFFFu;
        // The spellbook's autocast shine reads the same state.
        if (pet::slotAutocastOn(v)) petAutocastSpells_.insert(spellId);
        else petAutocastSpells_.erase(spellId);
        const pet::SlotWrite w{static_cast<uint32_t>(slot), v};
        if (state == WorldState::IN_WORLD) sendSetAction(*this, socket.get(), petGuid_, &w, 1);
    }
    fireAddonEvent("PET_BAR_UPDATE", {});
}

bool GameHandler::petDropAction(int slot, uint32_t action, uint32_t& toCursor) {
    toCursor = 0;
    if (slot < 0 || slot >= PET_ACTION_BAR_SLOTS || petGuid_ == 0 || !petCanTakeOrders(false)) return false;
    const auto drop = pet::dropOnSlot(petActionSlots_, static_cast<uint32_t>(slot), action);
    if (!drop.accepted) return false;
    if (state == WorldState::IN_WORLD) sendSetAction(*this, socket.get(), petGuid_, drop.writes, drop.writeCount);
    fireAddonEvent("PET_BAR_UPDATE", {});
    toCursor = drop.toCursor;
    return true;
}

void GameHandler::petClearSlotSpell(int slot) {
    if (slot < 0 || slot >= PET_ACTION_BAR_SLOTS || petGuid_ == 0) return;
    const uint32_t emptied = petActionSlots_[slot] & 0xFF000000u;
    uint32_t ignored = 0;
    petDropAction(slot, emptied, ignored);
}

// ---------------------------------------------------------------------------
// Pet talents
// ---------------------------------------------------------------------------

void GameHandler::learnPetTalent(uint32_t talentId, uint32_t rank0) {
    if (petGuid_ == 0 || state != WorldState::IN_WORLD || !socket) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_PET_LEARN_TALENT));
    pkt.writeUInt64(petGuid_);
    pkt.writeUInt32(talentId);
    pkt.writeUInt32(rank0);
    socket->send(pkt);
    LOG_INFO("Sent CMSG_PET_LEARN_TALENT talent=", talentId, " rank=", rank0);
}

void GameHandler::learnPreviewPetTalents(const std::vector<std::pair<uint32_t, uint32_t>>& talents) {
    if (petGuid_ == 0 || talents.empty() || state != WorldState::IN_WORLD || !socket) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_LEARN_PREVIEW_TALENTS_PET));
    pkt.writeUInt64(petGuid_);
    pkt.writeUInt32(static_cast<uint32_t>(talents.size()));
    for (const auto& [id, rank0] : talents) {
        pkt.writeUInt32(id);
        pkt.writeUInt32(rank0);
    }
    socket->send(pkt);
    LOG_INFO("Sent CMSG_LEARN_PREVIEW_TALENTS_PET count=", talents.size());
}

} // namespace game
} // namespace wowee
