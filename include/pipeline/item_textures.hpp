#pragma once

/**
 * item_textures.hpp - where the art for a worn item lives.
 *
 * Two conventions, both of them written out at every call site until now:
 *
 * The eight body regions an item paints onto a character each have their own
 * folder under Item\TextureComponents, and the file in it carries a suffix for
 * who is wearing it - _M, _F, or _U for art that serves both. Which of the three
 * exists is not recorded anywhere; it has to be asked of the filesystem, in that
 * order, and a caller that asks in a different order gets a man's arm on a
 * woman.
 *
 * A cape is filed under either ObjectComponents or TextureComponents, with or
 * without the suffix, and sometimes the DBC already names the folder. Six
 * candidates, in an order that matters, spelled out in three places.
 *
 * The region walk appeared six times and the cape list three, in the local
 * player's composition, in every other player's, in the NPC path twice, in the
 * portrait, and in the HUD. They agreed, which is the only reason nothing was
 * visibly wrong - but the ordering is a rule, and a rule kept in six places is
 * kept by luck.
 */

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wowee {
namespace pipeline {

class AssetManager;
class DBCFile;

/// The eight texture regions of ItemDisplayInfo, in the order its columns run.
constexpr int kItemTextureRegionCount = 8;

/// The folder under Item\TextureComponents for one region. Out-of-range answers
/// an empty string rather than reading past the table.
///
/// Inline, with the cape list below it, so the rules can be tested without an
/// asset tree to point at - the same reason m2_loader is kept apart from the
/// thing that reads files.
inline const char* itemComponentDir(int region) {
    // In ItemDisplayInfo's own column order. Region 0 is the upper arm and
    // region 7 the foot; the numbering indexes a character's composite atlas,
    // so the order is not free.
    static const char* const kDirs[kItemTextureRegionCount] = {
        "ArmUpperTexture", "ArmLowerTexture", "HandTexture",
        "TorsoUpperTexture", "TorsoLowerTexture",
        "LegUpperTexture", "LegLowerTexture", "FootTexture",
    };
    if (region < 0 || region >= kItemTextureRegionCount) return "";
    return kDirs[region];
}

/// The path to one region's art for this wearer, or empty if none of the three
/// spellings is on disk.
///
/// Asks for the gendered file first, then the unisex one, then the bare name.
/// `texName` is the value out of ItemDisplayInfo, without a folder or extension.
std::string resolveItemRegionTexture(AssetManager& assets, int region,
                                     const std::string& texName, bool isFemale);

/// The model and texture an item display names.
///
/// ItemDisplayInfo carries two pairs, left and right. The left one is the whole
/// item and the right one is often just a hilt, so the left is asked for first
/// and the right taken only when there is no left - a rule three of the five
/// readers had and two did not, which is a weapon that renders for an NPC and
/// not for the player holding the same one.
///
/// `modelFile` comes back with the extension normalised to .m2: the tables name
/// .mdx, which is the format the models were in before they were converted, and
/// every caller renamed it itself.
struct ItemDisplayArt {
    std::string modelFile;
    std::string textureName;   ///< no folder and no extension, as the table has it
};

ItemDisplayArt readItemDisplayArt(const DBCFile& itemDisplayInfo, uint32_t recordIndex);

/// Every path a cape's texture might be at, in the order to try them.
///
/// A name that already carries a folder is taken as given. Otherwise both
/// component folders are tried, unsuffixed first - which is what the shipped art
/// mostly uses - and then with the wearer's suffix and the unisex one.
inline std::vector<std::string> capeTextureCandidates(const std::string& rawName, bool isFemale) {
    std::vector<std::string> candidates;
    if (rawName.empty()) return candidates;

    std::string name = rawName;
    std::replace(name.begin(), name.end(), '/', '\\');
    const bool hasDir = name.find('\\') != std::string::npos;
    bool hasExt = false;
    if (name.size() >= 4) {
        std::string tail = name.substr(name.size() - 4);
        std::transform(tail.begin(), tail.end(), tail.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        hasExt = (tail == ".blp");
    }

    if (hasDir) {
        candidates.push_back(hasExt ? name : name + ".blp");
        return candidates;
    }

    const std::string baseObj = "Item\\ObjectComponents\\Cape\\" + name;
    const std::string baseTex = "Item\\TextureComponents\\Cape\\" + name;
    if (hasExt) {
        candidates.push_back(baseObj);
        candidates.push_back(baseTex);
    } else {
        candidates.push_back(baseObj + ".blp");
        candidates.push_back(baseTex + ".blp");
    }
    const char* suffix = isFemale ? "_F.blp" : "_M.blp";
    candidates.push_back(baseObj + suffix);
    candidates.push_back(baseObj + "_U.blp");
    candidates.push_back(baseTex + suffix);
    candidates.push_back(baseTex + "_U.blp");
    return candidates;
}

// ---------------------------------------------------------------------------
// Layering of item textures on the character skin.
//
// The stock 3.3.5a client (CCharacterComponent, Wow.exe build 12340) does not
// paint items in equipment slot order. Each of the ten skin regions keeps seven
// layer slots, and every item kind owns a fixed layer in each region it may
// paint. The table below is DAT_009f6a00 (10 item kinds x 10 regions of int,
// -1 = the item does not paint that region); regions 8 and 9 are face regions
// no item writes and are left out here.
//
// The per-region setters (installed at 0x004f1a20 into 0x00b6b80c..0x00b6b830)
// read the table and adjust a few cells from the display row:
//   ArmLower (0x004f0200): a chest with GeosetGroup1 moves to layer 5, gloves
//     with GeosetGroup1 to layer 6 - flared sleeves and gauntlet cuffs cover
//     bracers and plain gloves.
//   LegLower (0x004effa0 case 6): a robe (chest with GeosetGroup3) moves to
//     layer 4; trousers with GeosetGroup3 move to layer 4, or 3 under a robe;
//     boots with GeosetGroup1 move to layer 3.
// A texture is only accepted when the table cell is not -1 (0x004f2640), so a
// belt never paints the arms and a shirt never paints the legs.
//
// The region painters (0x00b6b834..0x00b6b858, e.g. 0x004f0ca0 for TorsoLower)
// blit the base skin and then layers 0 upwards, so a higher layer is on top.
// The LegUpper and LegLower painters (0x004f0d00, 0x004f0db0) narrow the range
// when the chest's ItemDisplayInfo Flags has bit 2 (component flag 0x20, set at
// 0x004f2640 case 3): only layer 1 in LegUpper and layers 1-3 in LegLower,
// which hides the trousers and the belt's leg part under that chest.
//
// Item kinds are mapped from equipment slots at 0x004ee6d0: head 0, shoulder 1,
// shirt 3, chest 4, waist 5, legs 6, feet 7, wrists 8, hands 9, tabard 18.
// ---------------------------------------------------------------------------

/// The item kinds the stock compositor keeps texture layers for. The values are
/// its own indices, which are also the order of CreatureDisplayInfoExtra's
/// equipment columns (that list adds the cape as 10).
enum class CharComponentSlot : uint8_t {
    Head = 0, Shoulder, Shirt, Chest, Belt, Legs, Boots, Wrist, Hands, Tabard,
    None
};

/// Kind of the item in a 0-based equipment slot (EquipSlot / the 19-entry
/// visible item list). Slots that paint no skin answer None.
inline CharComponentSlot componentSlotForEquipSlot(int equipSlot) {
    switch (equipSlot) {
        case 0:  return CharComponentSlot::Head;
        case 2:  return CharComponentSlot::Shoulder;
        case 3:  return CharComponentSlot::Shirt;
        case 4:  return CharComponentSlot::Chest;
        case 5:  return CharComponentSlot::Belt;
        case 6:  return CharComponentSlot::Legs;
        case 7:  return CharComponentSlot::Boots;
        case 8:  return CharComponentSlot::Wrist;
        case 9:  return CharComponentSlot::Hands;
        case 18: return CharComponentSlot::Tabard;
        default: return CharComponentSlot::None;
    }
}

/// Kind of an item by its InventoryType, for callers that only have that (the
/// character list sends no slot). Chest and robe share the chest kind.
inline CharComponentSlot componentSlotForInventoryType(uint8_t invType) {
    switch (invType) {
        case 1:  return CharComponentSlot::Head;
        case 3:  return CharComponentSlot::Shoulder;
        case 4:  return CharComponentSlot::Shirt;
        case 5:
        case 20: return CharComponentSlot::Chest;
        case 6:  return CharComponentSlot::Belt;
        case 7:  return CharComponentSlot::Legs;
        case 8:  return CharComponentSlot::Boots;
        case 9:  return CharComponentSlot::Wrist;
        case 10: return CharComponentSlot::Hands;
        case 19: return CharComponentSlot::Tabard;
        default: return CharComponentSlot::None;
    }
}

/// What the layering needs from one equipped item's ItemDisplayInfo row.
struct ItemRegionSource {
    CharComponentSlot slot = CharComponentSlot::None;
    std::array<std::string, kItemTextureRegionCount> textures;  ///< as the table names them
    uint32_t geosetGroup1 = 0;
    uint32_t geosetGroup3 = 0;
    uint32_t flags = 0;
};

/// One texture to paint: the region and the name out of ItemDisplayInfo.
struct ItemRegionLayer {
    int region;
    std::string texture;
};

/// Layer an item kind owns in a region before the per-item adjustments, or -1.
inline int stockItemRegionLayer(CharComponentSlot slot, int region) {
    // DAT_009f6a00, columns ArmUpper ArmLower Hand TorsoUpper TorsoLower
    // LegUpper LegLower Foot.
    static const int8_t kLayers[10][kItemTextureRegionCount] = {
        {-1, -1, -1, -1, -1, -1, -1, -1},  // head
        {-1, -1, -1, -1, -1, -1, -1, -1},  // shoulder
        { 0,  0, -1,  0,  0, -1, -1, -1},  // shirt
        { 1,  1, -1,  1,  1,  1,  1, -1},  // chest
        {-1, -1, -1, -1,  5,  2, -1, -1},  // belt
        {-1, -1, -1, -1, -1,  0,  0, -1},  // legs
        {-1, -1, -1, -1, -1, -1,  2,  0},  // boots
        {-1,  2, -1, -1, -1, -1, -1, -1},  // wrist
        {-1,  3,  0, -1, -1, -1, -1, -1},  // hands
        {-1, -1, -1,  4,  4, -1, -1, -1},  // tabard
    };
    const int s = static_cast<int>(slot);
    if (s < 0 || s >= 10 || region < 0 || region >= kItemTextureRegionCount) return -1;
    return kLayers[s][region];
}

/// The equipped items' region textures in the order the stock client paints
/// them: by region, and within a region from the lowest layer up, so each entry
/// goes on top of the ones before it in the same region.
///
/// Items may be given in any order. When two items land on the same layer of a
/// region (high boots and robe-length trousers under a robe both take LegLower
/// layer 3) the later equipment slot wins, as it does in the stock client,
/// which applies the slots in ascending order and replaces the layer.
inline std::vector<ItemRegionLayer> orderItemRegionLayers(const std::vector<ItemRegionSource>& items) {
    constexpr int kLayerCount = 7;
    std::vector<const ItemRegionSource*> sorted;
    sorted.reserve(items.size());
    for (const auto& it : items) {
        if (it.slot < CharComponentSlot::None) sorted.push_back(&it);
    }
    std::stable_sort(sorted.begin(), sorted.end(),
        [](const ItemRegionSource* a, const ItemRegionSource* b) { return a->slot < b->slot; });

    const ItemRegionSource* chest = nullptr;
    for (const auto* it : sorted) {
        if (it->slot == CharComponentSlot::Chest) chest = it;
    }
    const bool robe = chest && chest->geosetGroup3 != 0;
    const bool chestHidesLegs = chest && (chest->flags & 0x4u) != 0;

    const ItemRegionSource* cells[kItemTextureRegionCount][kLayerCount] = {};
    for (const auto* it : sorted) {
        for (int region = 0; region < kItemTextureRegionCount; region++) {
            if (it->textures[region].empty()) continue;
            int layer = stockItemRegionLayer(it->slot, region);
            if (layer < 0) continue;
            if (region == 1 && it->geosetGroup1 != 0) {
                if (it->slot == CharComponentSlot::Chest) layer = 5;
                else if (it->slot == CharComponentSlot::Hands) layer = 6;
            } else if (region == 6) {
                if (it->slot == CharComponentSlot::Chest && it->geosetGroup3 != 0) layer = 4;
                else if (it->slot == CharComponentSlot::Legs && it->geosetGroup3 != 0) layer = robe ? 3 : 4;
                else if (it->slot == CharComponentSlot::Boots && it->geosetGroup1 != 0) layer = 3;
            }
            cells[region][layer] = it;
        }
    }

    std::vector<ItemRegionLayer> out;
    for (int region = 0; region < kItemTextureRegionCount; region++) {
        int first = 0;
        int last = kLayerCount;
        if (region == 5 && chestHidesLegs) { first = 1; last = 2; }
        else if (region == 6 && chestHidesLegs) { first = 1; last = 4; }
        for (int layer = first; layer < last; layer++) {
            if (const ItemRegionSource* it = cells[region][layer]) {
                out.push_back({region, it->textures[region]});
            }
        }
    }
    return out;
}

/// One row's worth of ItemRegionSource, read through the active DBC layout.
ItemRegionSource readItemRegionSource(const DBCFile& itemDisplayInfo, uint32_t recordIndex,
                                      CharComponentSlot slot);

/// Turn ordered layers into the (region, path) list compositeWithRegions takes,
/// dropping any whose art is not on disk. The order is kept.
std::vector<std::pair<int, std::string>> resolveItemRegionLayers(
    AssetManager& assets, const std::vector<ItemRegionLayer>& layers, bool isFemale);

/// An equipped item by kind and ItemDisplayInfo ID.
struct EquippedItemDisplay {
    CharComponentSlot slot;
    uint32_t displayId;
};

/// The whole walk for a set of equipped items: read each display row, order
/// the layers as the stock client does and resolve the paths.
std::vector<std::pair<int, std::string>> buildItemRegionLayers(
    AssetManager& assets, const DBCFile& itemDisplayInfo,
    const std::vector<EquippedItemDisplay>& items, bool isFemale);

}  // namespace pipeline
}  // namespace wowee
