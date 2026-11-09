#include "core/character_geosets.hpp"

#include "core/helm_visual.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/hd_characters.hpp"
#include "pipeline/dbc_layout.hpp"

namespace wowee {
namespace core {

int geosetKindForInventoryType(uint8_t inventoryType) {
    using K = StockGeosetInput;
    switch (inventoryType) {
        case 1:  return K::Head;
        case 3:  return K::Shoulder;
        case 4:  return K::Shirt;
        case 5:
        case 20: return K::Chest;
        case 6:  return K::Belt;
        case 7:  return K::Legs;
        case 8:  return K::Boots;
        case 9:  return K::Wrist;
        case 10: return K::Gloves;
        case 16: return K::Cape;
        case 19: return K::Tabard;
        default: return -1;
    }
}

std::unordered_set<uint16_t> buildStockCharacterGeosets(pipeline::AssetManager& assets,
                                                        const CharacterGeosetQuery& q) {
    // Per-group values before equipment: the defaults (0x4dfda0), then the
    // hair and facial hair the character chose (0x4ea050, 0x4ea590).
    std::array<uint16_t, 19> base = stockDefaultGeosetValues();
    if (q.hairScalp != 0) base[0] = q.hairScalp;
    if (q.haveFacialRow) {
        base[1] = static_cast<uint16_t>(100 + q.facial100);
        base[2] = static_cast<uint16_t>(200 + q.facial200);
        base[3] = static_cast<uint16_t>(300 + q.facial300);
        base[17] = static_cast<uint16_t>(1700 + q.facial1700);
    }

    // What the helm hides, by race (HelmetGeosetVisData, 0x4ef0d0).
    const uint32_t head = q.displays[StockGeosetInput::Head];
    if (head != 0 && q.helmShown) {
        HelmGeosetVis vis;
        if (readHelmGeosetVis(assets, head, q.gender, vis.mask)) applyHelmGeosetVis(base, vis, q.race);
    }

    StockGeosetInput in;
    in.deathKnight = q.deathKnight;
    in.guildTabard = q.guildTabard;
    auto displayDbc = assets.loadDBC("ItemDisplayInfo.dbc");
    if (displayDbc && displayDbc->isLoaded()) {
        const auto* idiL = pipeline::getActiveDBCLayout()
            ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
        const uint32_t gg0 = idiL ? (*idiL)["GeosetGroup1"] : 7u;
        const uint32_t armLowerField = idiL ? (*idiL)["TextureArmLower"] : 16u;
        const uint32_t flagsField = gg0 + 3;  // Flags follows the three geoset groups
        for (int k = 0; k < StockGeosetInput::KindCount; ++k) {
            uint32_t did = q.displays[k];
            if (k == StockGeosetInput::Cape && !q.cloakShown) did = 0;
            if (did == 0) continue;
            const int32_t row = displayDbc->findRecordById(did);
            if (row < 0) continue;
            const auto r = static_cast<uint32_t>(row);
            for (int i = 0; i < 3; ++i) in.gg[k][i] = displayDbc->getUInt32(r, gg0 + i);
            // Chest flag 4 hides the legs (component flag 0x20).
            if (k == StockGeosetInput::Chest && flagsField < displayDbc->getFieldCount())
                in.chestHidesLegs = (displayDbc->getUInt32(r, flagsField) & 4u) != 0;
            // Anything over the shirt that paints the lower arm hides the
            // shirt's own sleeves.
            if (k != StockGeosetInput::Shirt && k != StockGeosetInput::Cape &&
                armLowerField < displayDbc->getFieldCount() &&
                !displayDbc->getString(r, armLowerField).empty()) {
                in.armLowerCoveredAboveShirt = true;
            }
        }
    }

    std::unordered_set<uint16_t> on = stockEquipmentGeosets(base, in);
    // Replacement (HD) models split the feet into group 20; ask for both
    // spellings so the model draws whichever it carries.
    on.insert(kGeosetBareFeet);
    on.insert(kGeosetBareFeetAlt);
    pipeline::applyHdExtraGeosets(on, q.extra);
    return on;
}

}  // namespace core
}  // namespace wowee
