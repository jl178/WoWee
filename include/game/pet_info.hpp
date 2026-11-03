#pragma once

// The client tables a pet is described by, read the way Wow.exe 12340 reads
// them (docs/re/pets.md):
//
//   CreatureFamily.dbc  name (UnitCreatureFamily, the stable's family line),
//                       icon (GetPetIcon, the stable's slot icon), diet mask
//                       (GetPetFoodTypes), talent tree type (GetPetTalentTree,
//                       the pet talent tab), category bit (which pet talents a
//                       family is offered)
//   ItemPetFood.dbc     the diet names, by bit
//   PetPersonality.dbc  the happiness thresholds and damage multipliers
//
// Column indices are the 3.3.5 layout: CreatureFamily ID, MinScale,
// MinScaleLevel, MaxScale, MaxScaleLevel, SkillLine[2], PetFoodMask,
// PetTalentType, CategoryEnumID, Name (16 locales + flags), IconFile.

#include "game/pet_action.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace game {

struct CreatureFamilyInfo {
    uint32_t id = 0;
    std::string name;
    std::string icon;
    uint32_t petFoodMask = 0;
    int32_t petTalentType = -1;   ///< 0 ferocity, 1 tenacity, 2 cunning; -1 none
    int32_t categoryEnumId = -1;  ///< the bit a pet talent's CategoryMask tests
};

class PetDbc {
public:
    /// Reads the three tables once the assets are there; a no-op after.
    void ensureLoaded(pipeline::AssetManager* am);
    [[nodiscard]] bool loaded() const { return loaded_; }

    [[nodiscard]] const CreatureFamilyInfo* family(uint32_t id) const;
    /// The diet names a mask allows, in ItemPetFood order (bit = id - 1).
    [[nodiscard]] std::vector<std::string> foodNames(uint32_t mask) const;
    /// PetPersonality row 1, the only one the client reads.
    [[nodiscard]] const pet::HappinessRow& happiness() const { return happiness_; }
    /// StableSlotPrices.dbc: what slot `slot` (1-4) costs, 0 when there is no
    /// such row (GetNextStableSlotCost, 0x5A14D0).
    [[nodiscard]] uint32_t stableSlotPrice(uint32_t slot) const {
        auto it = slotPrices_.find(slot);
        return it != slotPrices_.end() ? it->second : 0;
    }

private:
    bool loaded_ = false;
    std::unordered_map<uint32_t, CreatureFamilyInfo> families_;
    std::vector<std::pair<uint32_t, std::string>> foods_;
    pet::HappinessRow happiness_;
    std::unordered_map<uint32_t, uint32_t> slotPrices_;
};

} // namespace game
} // namespace wowee
