#include "core/helm_visual.hpp"

#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/item_textures.hpp"
#include "pipeline/m2_asset_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/character_renderer.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace wowee {
namespace core {

namespace {

constexpr const char* kHeadDir = "Item\\ObjectComponents\\Head\\";

std::string stripExtension(std::string name) {
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos) name.resize(dot);
    return name;
}

} // namespace

HelmVisual resolveHelmVisual(pipeline::AssetManager& assets,
                             uint32_t itemDisplayInfoId,
                             uint8_t raceId,
                             uint8_t genderId) {
    HelmVisual out;
    if (itemDisplayInfoId == 0) return out;

    auto dbc = assets.loadDBC("ItemDisplayInfo.dbc");
    if (!dbc) return out;
    const int32_t row = dbc->findRecordById(itemDisplayInfoId);
    if (row < 0) return out;

    const auto* layout = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    const uint32_t modelField = layout ? (*layout)["LeftModel"] : 1u;
    const uint32_t textureField = layout ? (*layout)["LeftModelTexture"] : 3u;

    const std::string modelName =
        stripExtension(dbc->getString(static_cast<uint32_t>(row), modelField));
    if (modelName.empty()) return out;

    const std::string suffix = raceGenderSuffix(raceId, genderId);
    if (!suffix.empty()) out.racialModelPath = kHeadDir + modelName + suffix + ".m2";
    out.baseModelPath = kHeadDir + modelName + ".m2";
    out.particleColorId = pipeline::readItemDisplayParticleColor(*dbc, static_cast<uint32_t>(row));

    const std::string textureName = dbc->getString(static_cast<uint32_t>(row), textureField);
    if (!textureName.empty()) {
        if (!suffix.empty()) {
            const std::string racial = kHeadDir + textureName + suffix + ".blp";
            if (assets.fileExists(racial)) out.texturePath = racial;
        }
        if (out.texturePath.empty()) out.texturePath = kHeadDir + textureName + ".blp";
    }
    return out;
}


namespace {

/// Which ItemDisplayInfo columns hold HelmetGeosetVis[male] and [female].
///
/// They move with the file: 12 and 13 in the 23-field build that ships in the
/// expansion overlays, 13 and 14 in the 25-field one. Rather than keep a table
/// of indices per expansion and get it wrong the way the facial-feature columns
/// were, find them once by asking which columns actually reference the
/// visibility table.
struct VisColumns { uint32_t male = 0xFFFFFFFFu; uint32_t female = 0xFFFFFFFFu; };

VisColumns findVisColumns(pipeline::DBCFile& displayInfo, pipeline::DBCFile& visData) {
    VisColumns out;
    std::unordered_set<uint32_t> ids;
    for (uint32_t r = 0; r < visData.getRecordCount(); ++r) {
        ids.insert(visData.getUInt32(r, 0));
    }
    if (ids.empty()) return out;

    const uint32_t rows = displayInfo.getRecordCount();
    const uint32_t step = std::max(1u, rows / 400u);
    for (uint32_t c = 0; c < displayInfo.getFieldCount(); ++c) {
        uint32_t nonZero = 0, valid = 0;
        for (uint32_t r = 0; r < rows; r += step) {
            const uint32_t v = displayInfo.getUInt32(r, c);
            if (v == 0) continue;
            ++nonZero;
            if (ids.count(v)) ++valid;
        }
        // A column of ids is a column where every value present is an id.
        if (nonZero >= 8 && valid == nonZero) {
            if (out.male == 0xFFFFFFFFu) out.male = c;
            else if (out.female == 0xFFFFFFFFu) { out.female = c; break; }
        }
    }
    return out;
}

} // namespace

bool readHelmGeosetVis(pipeline::AssetManager& assets, uint32_t itemDisplayInfoId,
                       uint8_t genderId, uint32_t (&masks)[7]) {
    for (auto& m : masks) m = 0;
    if (itemDisplayInfoId == 0) return false;
    auto displayInfo = assets.loadDBC("ItemDisplayInfo.dbc");
    auto visData = assets.loadDBC("HelmetGeosetVisData.dbc");
    if (!displayInfo || !visData) return false;

    static VisColumns columns = findVisColumns(*displayInfo, *visData);
    const uint32_t column = (genderId == 0) ? columns.male : columns.female;
    if (column == 0xFFFFFFFFu) return false;

    const int32_t row = displayInfo->findRecordById(itemDisplayInfoId);
    if (row < 0) return false;
    const uint32_t visId = displayInfo->getUInt32(static_cast<uint32_t>(row), column);
    if (visId == 0) return false;
    const int32_t visRow = visData->findRecordById(visId);
    if (visRow < 0) return false;
    for (uint32_t c = 0; c < 7 && c + 1 < visData->getFieldCount(); ++c)
        masks[c] = visData->getUInt32(static_cast<uint32_t>(visRow), c + 1);
    return true;
}

bool helmHidesHair(pipeline::AssetManager& assets,
                   uint32_t itemDisplayInfoId,
                   uint8_t genderId,
                   uint8_t raceId) {
    if (itemDisplayInfoId == 0) return false;
    uint32_t masks[7];
    if (!readHelmGeosetVis(assets, itemDisplayInfoId, genderId, masks)) return false;
    // Column 1, the hair, tested for this race the way stock does.
    if (raceId != 0 && raceId < 32) return (masks[0] & (1u << raceId)) != 0;
    // Every mask zero is the row circlets, tiaras and crowns point at.
    for (uint32_t m : masks) if (m != 0) return true;
    return false;
}

namespace {

struct ShoulderSide {
    const char* modelColumn;
    const char* textureColumn;
    uint32_t defaultModelField;
    uint32_t defaultTextureField;
    uint32_t attachmentId;
};
constexpr const char* kShoulderDir = "Item\\ObjectComponents\\Shoulder\\";
constexpr ShoulderSide kShoulderSides[] = {
    {"LeftModel",  "LeftModelTexture",  1u, 3u, kAttachShoulderLeft},
    {"RightModel", "RightModelTexture", 2u, 4u, kAttachShoulderRight},
};

/// The model and texture of each side of a shoulder display that has one.
struct ShoulderPiece { uint32_t attachmentId; std::string modelPath; std::string texturePath; };
std::vector<ShoulderPiece> shoulderPieces(pipeline::DBCFile& dbc, uint32_t row) {
    const auto* layout = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    std::vector<ShoulderPiece> out;
    for (const auto& side : kShoulderSides) {
        const uint32_t modelField = layout ? (*layout)[side.modelColumn] : side.defaultModelField;
        const uint32_t textureField = layout ? (*layout)[side.textureColumn] : side.defaultTextureField;
        const std::string modelName = stripExtension(dbc.getString(row, modelField));
        if (modelName.empty()) continue;
        const std::string textureName = dbc.getString(row, textureField);
        out.push_back({side.attachmentId, std::string(kShoulderDir) + modelName + ".m2",
                       textureName.empty() ? std::string{}
                                           : std::string(kShoulderDir) + textureName + ".blp"});
    }
    return out;
}

} // namespace

void attachShoulders(rendering::CharacterRenderer& renderer,
                     pipeline::AssetManager& assets,
                     uint32_t instanceId,
                     uint32_t itemDisplayInfoId,
                     const ModelIdAllocator& allocModelId) {
    attachShoulders(renderer, assets, instanceId, itemDisplayInfoId,
                    [&](const std::string& m2Path, const std::string&) { return allocModelId(m2Path); });
}

void attachShoulders(rendering::CharacterRenderer& renderer,
                     pipeline::AssetManager& assets,
                     uint32_t instanceId,
                     uint32_t itemDisplayInfoId,
                     const ItemModelIdAllocator& allocModelId) {
    renderer.detachWeapon(instanceId, kAttachShoulderRight);
    renderer.detachWeapon(instanceId, kAttachShoulderLeft);
    if (itemDisplayInfoId == 0) return;

    auto dbc = assets.loadDBC("ItemDisplayInfo.dbc");
    if (!dbc) return;
    const int32_t row = dbc->findRecordById(itemDisplayInfoId);
    if (row < 0) return;
    const auto r = static_cast<uint32_t>(row);

    for (const auto& piece : shoulderPieces(*dbc, r)) {
        // Parsed once and shared (m2_asset_loader.hpp); this read and parsed
        // the file again for every unit wearing it.
        const auto model = pipeline::loadM2WithSkinShared(assets, piece.modelPath);
        if (!model) continue;
        if (renderer.attachWeapon(instanceId, piece.attachmentId, *model,
                                  allocModelId(piece.modelPath, piece.texturePath),
                                  piece.texturePath)) {
            renderer.setAttachmentParticleColor(instanceId, piece.attachmentId,
                                                pipeline::readItemDisplayParticleColor(*dbc, r));
        }
    }
}

bool attachHelm(rendering::CharacterRenderer& renderer,
                pipeline::AssetManager& assets,
                uint32_t instanceId,
                uint32_t itemDisplayInfoId,
                uint8_t raceId,
                uint8_t genderId,
                const ModelIdAllocator& allocModelId) {
    return attachHelm(renderer, assets, instanceId, itemDisplayInfoId, raceId, genderId,
                      [&](const std::string& m2Path, const std::string&) { return allocModelId(m2Path); });
}

bool attachHelm(rendering::CharacterRenderer& renderer,
                pipeline::AssetManager& assets,
                uint32_t instanceId,
                uint32_t itemDisplayInfoId,
                uint8_t raceId,
                uint8_t genderId,
                const ItemModelIdAllocator& allocModelId) {
    // Only the helm point - detaching 0 as well would drop the shield.
    renderer.detachWeapon(instanceId, kAttachHelm);
    if (itemDisplayInfoId == 0) return false;

    const HelmVisual helm = resolveHelmVisual(assets, itemDisplayInfoId, raceId, genderId);
    if (!helm.valid()) return false;

    std::string modelPath = helm.racialModelPath;
    std::shared_ptr<const pipeline::M2Model> model;
    if (!modelPath.empty()) model = pipeline::loadM2WithSkinShared(assets, modelPath);
    if (!model) {
        modelPath = helm.baseModelPath;
        model = pipeline::loadM2WithSkinShared(assets, modelPath);
        if (!model) return false;
    }
    if (!renderer.attachWeapon(instanceId, kAttachHelm, *model,
                               allocModelId(modelPath, helm.texturePath), helm.texturePath)) {
        return false;
    }
    renderer.setAttachmentParticleColor(instanceId, kAttachHelm, helm.particleColorId);
    return true;
}

void collectHelmAndShoulderFiles(pipeline::AssetManager& assets,
                                 uint32_t helmDisplayId, uint8_t raceId, uint8_t genderId,
                                 uint32_t shoulderDisplayId,
                                 std::vector<std::string>& modelPaths,
                                 std::vector<std::string>& texturePaths) {
    if (helmDisplayId != 0) {
        const HelmVisual helm = resolveHelmVisual(assets, helmDisplayId, raceId, genderId);
        if (helm.valid()) {
            if (!helm.racialModelPath.empty()) modelPaths.push_back(helm.racialModelPath);
            modelPaths.push_back(helm.baseModelPath);
            if (!helm.texturePath.empty()) texturePaths.push_back(helm.texturePath);
        }
    }
    if (shoulderDisplayId != 0) {
        auto dbc = assets.loadDBC("ItemDisplayInfo.dbc");
        const int32_t row = dbc ? dbc->findRecordById(shoulderDisplayId) : -1;
        if (row >= 0) {
            for (const auto& piece : shoulderPieces(*dbc, static_cast<uint32_t>(row))) {
                modelPaths.push_back(piece.modelPath);
                if (!piece.texturePath.empty()) texturePaths.push_back(piece.texturePath);
            }
        }
    }
}

} // namespace core
} // namespace wowee
