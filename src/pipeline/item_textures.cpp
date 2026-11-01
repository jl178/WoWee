#include "pipeline/item_textures.hpp"

#include <algorithm>
#include <cctype>

#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"

namespace wowee {
namespace pipeline {



std::string resolveItemRegionTexture(AssetManager& assets, int region,
                                     const std::string& texName, bool isFemale) {
    if (texName.empty()) return {};
    const char* dir = itemComponentDir(region);
    if (dir[0] == '\0') return {};

    const std::string base = std::string("Item\\TextureComponents\\") + dir + "\\" + texName;
    std::string gendered = base + (isFemale ? "_F.blp" : "_M.blp");
    if (assets.fileExists(gendered)) return gendered;
    std::string unisex = base + "_U.blp";
    if (assets.fileExists(unisex)) return unisex;
    std::string plain = base + ".blp";
    if (assets.fileExists(plain)) return plain;
    return {};
}

ItemDisplayArt readItemDisplayArt(const DBCFile& itemDisplayInfo, uint32_t recordIndex) {
    const auto* layout = getActiveDBCLayout()
        ? getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    const uint32_t modelLeft   = layout ? (*layout)["LeftModel"]         : 1u;
    const uint32_t modelRight  = layout ? (*layout)["RightModel"]        : 2u;
    const uint32_t texLeft     = layout ? (*layout)["LeftModelTexture"]  : 3u;
    const uint32_t texRight    = layout ? (*layout)["RightModelTexture"] : 4u;

    ItemDisplayArt art;
    art.modelFile   = itemDisplayInfo.getString(recordIndex, modelLeft);
    art.textureName = itemDisplayInfo.getString(recordIndex, texLeft);
    if (art.modelFile.empty()) {
        art.modelFile   = itemDisplayInfo.getString(recordIndex, modelRight);
        art.textureName = itemDisplayInfo.getString(recordIndex, texRight);
    }
    if (art.modelFile.empty()) return art;

    // The tables name .mdx, the format these models were in before they were
    // converted. Every caller renamed it, and one of them forgot the case where
    // the name has no extension at all.
    const size_t dot = art.modelFile.rfind('.');
    if (dot != std::string::npos) art.modelFile.resize(dot);
    art.modelFile += ".m2";
    return art;
}

ItemRegionSource readItemRegionSource(const DBCFile& itemDisplayInfo, uint32_t recordIndex,
                                      CharComponentSlot slot) {
    const auto* layout = getActiveDBCLayout()
        ? getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    uint32_t texFields[kItemTextureRegionCount];
    getItemDisplayInfoTextureFields(itemDisplayInfo, layout, texFields);

    // 25-field files (TBC/WotLK) carry two inventory icons and so sit one
    // column right of the 23-field vanilla ones. Flags follows GeosetGroup3 in
    // both.
    const bool wide = itemDisplayInfo.getFieldCount() >= 25;
    uint32_t geoset1Field = layout ? layout->field("GeosetGroup1") : 0xFFFFFFFF;
    if (geoset1Field == 0xFFFFFFFF) geoset1Field = wide ? 7u : 6u;
    uint32_t geoset3Field = layout ? layout->field("GeosetGroup3") : 0xFFFFFFFF;
    if (geoset3Field == 0xFFFFFFFF) geoset3Field = wide ? 9u : 8u;
    const uint32_t flagsField = geoset3Field + 1;

    ItemRegionSource src;
    src.slot = slot;
    for (int region = 0; region < kItemTextureRegionCount; region++) {
        src.textures[region] = itemDisplayInfo.getString(recordIndex, texFields[region]);
    }
    src.geosetGroup1 = itemDisplayInfo.getUInt32(recordIndex, geoset1Field);
    src.geosetGroup3 = itemDisplayInfo.getUInt32(recordIndex, geoset3Field);
    if (flagsField < itemDisplayInfo.getFieldCount()) {
        src.flags = itemDisplayInfo.getUInt32(recordIndex, flagsField);
    }
    return src;
}

std::vector<std::pair<int, std::string>> resolveItemRegionLayers(
    AssetManager& assets, const std::vector<ItemRegionLayer>& layers, bool isFemale) {
    std::vector<std::pair<int, std::string>> out;
    out.reserve(layers.size());
    for (const auto& layer : layers) {
        std::string path = resolveItemRegionTexture(assets, layer.region, layer.texture, isFemale);
        if (!path.empty()) out.emplace_back(layer.region, std::move(path));
    }
    return out;
}

std::vector<std::pair<int, std::string>> buildItemRegionLayers(
    AssetManager& assets, const DBCFile& itemDisplayInfo,
    const std::vector<EquippedItemDisplay>& items, bool isFemale) {
    std::vector<ItemRegionSource> sources;
    sources.reserve(items.size());
    for (const auto& item : items) {
        if (item.displayId == 0 || item.slot == CharComponentSlot::None) continue;
        const int32_t recIdx = itemDisplayInfo.findRecordById(item.displayId);
        if (recIdx < 0) continue;
        sources.push_back(readItemRegionSource(itemDisplayInfo, static_cast<uint32_t>(recIdx), item.slot));
    }
    return resolveItemRegionLayers(assets, orderItemRegionLayers(sources), isFemale);
}


}  // namespace pipeline
}  // namespace wowee
