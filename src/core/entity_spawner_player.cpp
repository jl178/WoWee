#include "core/entity_spawner.hpp"
#include "core/hitch_legacy.hpp"
#include "core/helm_visual.hpp"
#include "core/character_geosets.hpp"
#include "core/geoset_rules.hpp"
#include "core/character_paths.hpp"
#include "pipeline/char_sections.hpp"

// M2 attachment 11 is the helm. 0 is the shield mount, which is where head gear
// was going: it attached, reported success, and hung off the forearm.
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/game_object_sounds.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/wmo_group_path.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/item_textures.hpp"
#include "core/guild_tabard.hpp"
#include "pipeline/m2_asset_loader.hpp"
#include "game/game_handler.hpp"
#include "game/update_field_table.hpp"
#include "game/game_services.hpp"
#include "game/transport_manager.hpp"

#include <cmath>
#include <algorithm>
#include "pipeline/hd_characters.hpp"
#include <cctype>
#include <sstream>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace wowee {
namespace core {

namespace {
// The bare geoset ids, the group arithmetic and the appearance key all live in
// core/geoset_rules.hpp. This file used to carry its own copy of the constants,
// and the copy went stale: it named 2002 as "the" bare feet and never learned
// about 2001, which is how an HD model that spells its feet the other way lost
// them here while keeping them in the portrait.

// The head of a character's bare geoset set: the body, the one scalp it wears,
// and its facial hair.
//
// Two places build a player's geosets - one for a player seen across the world,
// one for the equipped composition - and they had already drifted: only one of
// them knew about the second bare-feet id, and only one of them treated a zero
// facial variant as "none" rather than as geoset x00.
std::unordered_set<uint16_t> bareGeosetsFor(uint16_t scalp,
                                            const EntitySpawner::FacialHairGeosets* facial,
                                            uint8_t raceId) {
    // No row for this character: the first variant of each facial group, which
    // on most models is the absence of the feature.
    const uint16_t f100 = facial ? facial->geoset100 : 1;
    const uint16_t f200 = facial ? facial->geoset200 : 1;
    const uint16_t f300 = facial ? facial->geoset300 : 1;
    const uint16_t f1700 = facial ? facial->geoset1700 : 0;
    return bareCharacterGeosets(scalp, f100, f200, f300, raceId, f1700);
}

uint16_t selectHairScalpGeoset(const std::unordered_map<uint32_t, uint16_t>& hairGeosets,
                               uint8_t raceId,
                               uint8_t genderId,
                               uint8_t hairStyleId) {
    const uint32_t key = appearanceKey(raceId, genderId, hairStyleId);
    auto it = hairGeosets.find(key);
    if (it != hairGeosets.end() && it->second > 0) {
        return it->second;
    }
    return 1;
}
} // namespace

std::shared_ptr<pipeline::M2Model> EntitySpawner::parsePlayerModel(pipeline::AssetManager& assets,
                                                                   const std::string& m2Path) {
    auto m2Data = assets.readFile(m2Path);
    if (m2Data.empty()) {
        LOG_WARNING("spawnOnlinePlayer: failed to read M2: ", m2Path);
        return nullptr;
    }

    auto model = std::make_shared<pipeline::M2Model>(pipeline::M2Loader::load(m2Data));
    if (model->vertices.empty()) {
        LOG_WARNING("spawnOnlinePlayer: failed to parse M2: ", m2Path);
        return nullptr;
    }

    // Skin file (only for WotLK M2s - vanilla has embedded skin)
    std::string skinPath = pipeline::skinPathForM2(m2Path);
    auto skinData = assets.readFile(skinPath);
    if (!skinData.empty() && model->version >= 264) {
        pipeline::M2Loader::loadSkin(skinData, *model);
    }

    // After skin loading, full model must be valid (vertices + indices)
    if (!model->isValid()) {
        LOG_WARNING("spawnOnlinePlayer: failed to load skin for M2: ", m2Path);
        return nullptr;
    }

    // Only the three animations a standing player needs, plus the vocal
    // emotes: their keyframes and $CSD voice timestamps are in the .anim
    // files. Loading every external sequence of a character model stalls
    // the frame; this runs once per race and gender (playerParsedModels_).
    pipeline::loadExternalAnimations(
        assets, m2Path, m2Data, *model,
        {rendering::anim::STAND, rendering::anim::WALK, rendering::anim::RUN,
         rendering::anim::EMOTE_LAUGH, rendering::anim::EMOTE_KISS,
         rendering::anim::EMOTE_CRY, rendering::anim::EMOTE_CHICKEN,
         rendering::anim::EMOTE_APPLAUD});
    return model;
}

std::string EntitySpawner::playerModelPathFor(uint8_t raceId, uint8_t genderId) const {
    const std::string path = game::getPlayerModelPath(
        static_cast<game::Race>(raceId), genderId == 1 ? game::Gender::FEMALE : game::Gender::MALE);
    if (path.empty() || !assetManager_) return path;
    return pipeline::hdCharacterModelPath(*assetManager_, path);
}

EntitySpawner::PlayerLook EntitySpawner::resolvePlayerLook(uint8_t raceId, uint8_t genderId,
                                                           uint32_t appearanceBytes,
                                                           uint8_t facialFeatures) {
    PlayerLook out;
    // The character's textures, through the one reader in
    // pipeline/char_sections.hpp - the same scan the local player, the NPCs and
    // the portrait use. This path used to carry a fourth copy of it, and the
    // copy did not read the skin row's second texture, which is the head detail
    // an HD model draws its ears and eyelashes from. Every other player in the
    // world had skin-coloured eyelashes for exactly that reason.
    const std::string defaultSkin = defaultBodySkinPath(raceId, genderId);
    out.pelvisPath = defaultPelvisPath(raceId, genderId);
    const AppearanceBytes look = unpackAppearanceBytes(appearanceBytes);

    std::string& bodySkinPath = out.bodySkinPath;
    bodySkinPath = defaultSkin;
    std::string& skinExtraPath = out.skinExtraPath;
    std::string& hairTexturePath = out.hairTexturePath;
    std::string& faceLowerPath = out.faceLowerPath;
    std::string& faceUpperPath = out.faceUpperPath;
    std::string& facialLowerPath = out.facialLowerPath;
    std::string& facialUpperPath = out.facialUpperPath;
    std::vector<std::string>& underwearPaths = out.underwearPaths;

    // Enhanced's HD model takes its textures from the table beside it.
    if (const std::string m2 = playerModelPathFor(raceId, genderId);
        pipeline::isHdCharacterModelPath(m2)) {
        if (auto hd = pipeline::loadHdCharacterLook(*assetManager_, m2)) {
            pipeline::CharacterAppearance who;
            who.raceId = raceId;
            who.sexId = genderId;
            who.skinId = look.skinId;
            who.faceId = look.faceId;
            who.hairStyleId = look.hairStyleId;
            who.hairColorId = look.hairColorId;
            who.facialHairId = facialFeatures;
            const auto sections = pipeline::resolveHdCharacterTextures(*hd, who);
            if (!sections.bodySkin.empty()) bodySkinPath = sections.bodySkin;
            hairTexturePath = sections.hair;
            underwearPaths = sections.underwear;
        }
    } else if (auto charSectionsDbc = assetManager_->loadDBC("CharSections.dbc");
        charSectionsDbc && charSectionsDbc->isLoaded()) {
        const auto* csL = pipeline::getActiveDBCLayout()
            ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
        const auto csF = pipeline::detectCharSectionsFields(charSectionsDbc.get(), csL);

        pipeline::CharacterAppearance who;
        who.raceId = raceId;
        who.sexId = genderId;
        who.skinId = look.skinId;
        who.faceId = look.faceId;
        who.hairStyleId = look.hairStyleId;
        who.hairColorId = look.hairColorId;
        who.facialHairId = facialFeatures;

        // The underwear rows name art that was never shipped for some skin
        // colours - Draenei 10 to 16 among them - and this caller can check.
        const auto sections = pipeline::resolveCharacterSections(
            charSectionsDbc.get(), csF, who,
            [](const std::string& path, void* ctx) {
                return static_cast<pipeline::AssetManager*>(ctx)->fileExists(path);
            },
            assetManager_);

        if (!sections.bodySkin.empty()) bodySkinPath = sections.bodySkin;
        skinExtraPath = sections.skinExtra;
        faceLowerPath = sections.faceLower;
        faceUpperPath = sections.faceUpper;
        facialLowerPath = sections.facialLower;
        facialUpperPath = sections.facialUpper;
        hairTexturePath = sections.hair;
        // Scalps ride with the base overlays (head regions, after the facial
        // hair; stock 0x4F0AD0 / 0x4F0B70).
        underwearPaths.clear();
        for (const std::string* sc : {&sections.scalpLower, &sections.scalpUpper})
            if (!sc->empty()) underwearPaths.push_back(*sc);
        underwearPaths.insert(underwearPaths.end(), sections.underwear.begin(), sections.underwear.end());

        if (!sections.exactFace) {
            LOG_WARNING("spawnOnlinePlayer: no DBC face match for face=",
                        static_cast<int>(look.faceId), " skin=", static_cast<int>(look.skinId),
                        // Cast, because both are uint8_t and the stream writes
                        // one as a character: race 9 came out as a tab and sex
                        // 0 as a NUL, which is unreadable and puts a NUL in the
                        // log file - grep then takes the whole log for binary
                        // and prints nothing at all for any pattern in it.
                        " race=", static_cast<int>(raceId),
                        " sex=", static_cast<int>(genderId),
                        sections.haveFace ? " - using the nearest face instead"
                                          : " - this player will render with no face");
        }
    }

    return out;
}

void EntitySpawner::spawnOnlinePlayer(uint64_t guid,
                                    uint8_t raceId,
                                    uint8_t genderId,
                                    uint32_t appearanceBytes,
                                    uint8_t facialFeatures,
                                    float x, float y, float z, float orientation,
                                    const PlayerLook* lookIn) {
    if (!renderer_ || !renderer_->getCharacterRenderer() || !assetManager_ || !assetManager_->isInitialized()) return;
    if (playerInstances_.count(guid)) return;

    // Skip local player - already spawned as the main character
    if (gameHandler_) {
        uint64_t localGuid = gameHandler_->getPlayerGuid();
        uint64_t activeGuid = gameHandler_->getActiveCharacterGuid();
        if ((localGuid != 0 && guid == localGuid) ||
            (activeGuid != 0 && guid == activeGuid) ||
            (spawnedPlayerGuid_ != 0 && guid == spawnedPlayerGuid_)) {
            return;
        }
    }
    auto* charRenderer = renderer_->getCharacterRenderer();

    // Base geometry model: cache by (race, gender)
    const std::string playerM2Path = playerModelPathFor(raceId, genderId);
    const bool hdModel = pipeline::isHdCharacterModelPath(playerM2Path);
    uint32_t cacheKey = playerModelKey(raceId, genderId, hdModel);
    uint32_t modelId = 0;
    auto itCache = playerModelCache_.find(cacheKey);
    if (itCache != playerModelCache_.end()) {
        modelId = itCache->second;
        if (!charRenderer->getModelData(modelId)) {
            LOG_WARNING("spawnOnlinePlayer: cached player model missing after world reload, reloading modelId=",
                        modelId, " race=", static_cast<int>(raceId),
                        " gender=", static_cast<int>(genderId));
            playerTextureSlotsByModelId_.erase(modelId);
            playerModelCache_.erase(itCache);
            modelId = 0;
        }
    }
    if (modelId == 0) {
        const std::string& m2Path = playerM2Path;
        if (m2Path.empty()) {
            LOG_WARNING("spawnOnlinePlayer: unknown race/gender for guid 0x", std::hex, guid, std::dec,
                        " race=", static_cast<int>(raceId), " gender=", static_cast<int>(genderId));
            return;
        }

        // Parsed by the spawn's worker (processPlayerSpawnQueue), or kept from
        // the last time this race was in view: the model is unloaded with the
        // last player of the race, and parsing it again was the frame's
        // longest stall when one came back.
        std::shared_ptr<pipeline::M2Model> parsed;
        if (auto pit = playerParsedModels_.find(cacheKey);
            pit != playerParsedModels_.end() && !core::hitchLegacy()) {
            parsed = pit->second;
        } else {
            parsed = parsePlayerModel(*assetManager_, m2Path);
            if (!parsed) return;
            playerParsedModels_[cacheKey] = parsed;
        }

        modelId = nextPlayerModelId_++;
        if (!charRenderer->loadModel(*parsed, modelId)) {
            LOG_WARNING("spawnOnlinePlayer: failed to load model to GPU: ", m2Path);
            return;
        }

        playerModelCache_[cacheKey] = modelId;
    }

    // Determine texture slots once per model
    {
        auto [slotIt, inserted] = playerTextureSlotsByModelId_.try_emplace(modelId);
        if (inserted) {
            PlayerTextureSlots slots;
            if (const auto* md = charRenderer->getModelData(modelId)) {
                for (size_t ti = 0; ti < md->textures.size(); ti++) {
                    uint32_t t = md->textures[ti].type;
                    if (t == 1 && slots.skin < 0) slots.skin = static_cast<int>(ti);
                    else if (t == 6 && slots.hair < 0) slots.hair = static_cast<int>(ti);
                    else if (t == 8 && slots.skinExtra < 0) slots.skinExtra = static_cast<int>(ti);
                }
            }
            slotIt->second = slots;
        }
    }

    // Create instance at server position
    glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
    float renderYaw = orientation + glm::radians(90.0f);
    // Drawn at the race display's size times OBJECT_FIELD_SCALE_X (0x71C0E0).
    float unitScale = playerRaceDisplayScale(raceId, genderId);
    if (gameHandler_) {
        if (auto e = gameHandler_->getEntityManager().getEntity(guid)) {
            const uint16_t scaleIdx = game::fieldIndex(game::UF::OBJECT_FIELD_SCALE_X);
            if (scaleIdx != 0xFFFF) {
                const uint32_t raw = e->getField(scaleIdx);
                float s = 0.0f;
                std::memcpy(&s, &raw, sizeof(s));
                if (raw != 0 && s > 0.01f && s < 100.0f) unitScale *= s;
            }
        }
    }
    uint32_t instanceId = charRenderer->createInstance(modelId, renderPos, glm::vec3(0.0f, 0.0f, renderYaw), unitScale);
    if (instanceId == 0) return;

    // The character's textures (resolvePlayerLook), already resolved when a
    // worker prepared this spawn.
    const AppearanceBytes look = unpackAppearanceBytes(appearanceBytes);
    const uint8_t hairStyleId = look.hairStyleId;
    const PlayerLook resolvedHere =
        lookIn ? PlayerLook{} : resolvePlayerLook(raceId, genderId, appearanceBytes, facialFeatures);
    const PlayerLook& pl = lookIn ? *lookIn : resolvedHere;
    const std::string& bodySkinPath = pl.bodySkinPath;
    const std::string& skinExtraPath = pl.skinExtraPath;
    const std::string& hairTexturePath = pl.hairTexturePath;
    const std::string& faceLowerPath = pl.faceLowerPath;
    const std::string& faceUpperPath = pl.faceUpperPath;
    const std::string& facialLowerPath = pl.facialLowerPath;
    const std::string& facialUpperPath = pl.facialUpperPath;
    const std::string& pelvisPath = pl.pelvisPath;
    const std::vector<std::string>& underwearPaths = pl.underwearPaths;

    // Composite base skin + face + underwear overlays
    rendering::VkTexture* compositeTex = nullptr;
    {
        std::vector<std::string> layers;
        layers.push_back(bodySkinPath);
        if (!faceLowerPath.empty()) layers.push_back(faceLowerPath);
        if (!faceUpperPath.empty()) layers.push_back(faceUpperPath);
        if (!facialLowerPath.empty()) layers.push_back(facialLowerPath);
        if (!facialUpperPath.empty()) layers.push_back(facialUpperPath);
        for (const auto& up : underwearPaths) layers.push_back(up);
        if (layers.size() > 1) {
            compositeTex = charRenderer->compositeTextures(layers);
        } else {
            compositeTex = charRenderer->loadTexture(bodySkinPath);
        }
    }

    rendering::VkTexture* hairTex = nullptr;
    if (!hairTexturePath.empty()) {
        hairTex = charRenderer->loadTexture(hairTexturePath);
    }
    // Texture type 8 is Skin Extra: the head detail sheet an HD model draws its
    // ears, eyes and eyelashes from. CharSections names it in the skin row's
    // second texture, which the tables the game shipped leave blank - so on a
    // stock model this still falls through to the underwear art it always used.
    rendering::VkTexture* skinExtraTex = nullptr;
    if (!skinExtraPath.empty()) skinExtraTex = charRenderer->loadTexture(skinExtraPath);
    else if (!underwearPaths.empty()) skinExtraTex = charRenderer->loadTexture(underwearPaths[0]);
    else skinExtraTex = charRenderer->loadTexture(pelvisPath);

    const PlayerTextureSlots& slots = playerTextureSlotsByModelId_[modelId];
    if (slots.skin >= 0 && compositeTex) {
        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(slots.skin), compositeTex);
    }
    if (slots.hair >= 0 && hairTex) {
        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(slots.hair), hairTex);
    }
    if (slots.skinExtra >= 0 && skinExtraTex) {
        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(slots.skinExtra), skinExtraTex);
    }

    // Geosets: body + selected hair/facial hair. Do not enable every group-0
    // submesh; that activates all hair scalp variants at once.
    const auto hdLook = hdModel ? pipeline::loadHdCharacterLook(*assetManager_, playerM2Path) : nullptr;
    uint16_t selectedHairScalp = selectHairScalpGeoset(hairGeosetMap_, raceId, genderId, hairStyleId);
    auto itFacial = facialHairGeosetMap_.find(appearanceKey(raceId, genderId, facialFeatures));
    FacialHairGeosets hdFacial;
    std::vector<uint16_t> hdExtra;
    if (hdLook) {
        // The HD model's own hair, beards (meshes) and extra groups.
        pipeline::CharacterAppearance who;
        who.skinId = look.skinId;
        who.faceId = look.faceId;
        who.hairStyleId = hairStyleId;
        who.hairColorId = look.hairColorId;
        who.facialHairId = facialFeatures;
        const auto hd = pipeline::resolveHdCharacterGeosets(*hdLook, who);
        selectedHairScalp = hd.hairScalp > 0 ? hd.hairScalp : 1;
        hdFacial.geoset100 = hd.facial100;
        hdFacial.geoset200 = hd.facial200;
        hdFacial.geoset300 = hd.facial300;
        hdExtra = hd.extra;
    }
    std::unordered_set<uint16_t> activeGeosets = bareGeosetsFor(
        selectedHairScalp,
        hdLook ? &hdFacial : itFacial != facialHairGeosetMap_.end() ? &itFacial->second : nullptr, raceId);
    pipeline::applyHdExtraGeosets(activeGeosets, hdExtra);
    if (hdLook) {
        // The face's shape: its .bone adjustments.
        pipeline::CharacterAppearance who;
        who.skinId = look.skinId;
        who.faceId = look.faceId;
        who.hairStyleId = look.hairStyleId;
        who.hairColorId = look.hairColorId;
        who.facialHairId = facialFeatures;
        std::vector<std::pair<uint16_t, glm::mat4>> bones;
        for (const auto& a : pipeline::loadHdCharacterBones(*assetManager_, *hdLook, who))
            bones.emplace_back(a.bone, a.matrix);
        charRenderer->setBoneAdjustments(instanceId, bones);
    }
    // This one is drawn before equipment is known and wants the no-cloak panel;
    // the shared set leaves group 15 alone so the equipment pass can decide.
    activeGeosets.insert(kGeosetNoCape);
    charRenderer->setActiveGeosets(instanceId, activeGeosets);

    if (deadCreatureGuids_.count(guid)) {
        // A dead player (or a corpse object) coming into view is already down.
        charRenderer->playDeath(instanceId, /*alreadyDead=*/true);
    } else {
        // Already seated, asleep or kneeling when they came into view.
        uint32_t standAnim = 0;
        if (gameHandler_) {
            if (auto unit = std::dynamic_pointer_cast<game::Unit>(
                    gameHandler_->getEntityManager().getEntity(guid))) {
                standAnim = rendering::anim::standStateLoopAnim(unit->getStandState());
            }
        }
        charRenderer->playAnimation(instanceId,
                                    standAnim != 0 && charRenderer->hasAnimation(instanceId, standAnim)
                                        ? standAnim : rendering::anim::STAND,
                                    true);
    }
    playerInstances_[guid] = instanceId;

    // The mount field may have arrived before this render instance, or the
    // player may be re-created without another values update. Reconcile from
    // retained entity state so already-mounted players are never left on foot.
    if (gameHandler_) {
        auto entity = gameHandler_->getEntityManager().getEntity(guid);
        auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
        if (unit && unit->getMountDisplayId() != 0) {
            setRemotePlayerMountDisplayId(guid, unit->getMountDisplayId());
        }
    }

    OnlinePlayerAppearanceState st;
    st.instanceId = instanceId;
    st.modelId = modelId;
    st.raceId = raceId;
    st.genderId = genderId;
    st.appearanceBytes = appearanceBytes;
    st.facialFeatures = facialFeatures;
    st.bodySkinPath = bodySkinPath;
    st.hdLook = hdLook;
    // Include face textures so compositeWithRegions can rebuild the full base
    if (!faceLowerPath.empty()) st.underwearPaths.push_back(faceLowerPath);
    if (!faceUpperPath.empty()) st.underwearPaths.push_back(faceUpperPath);
    if (!facialLowerPath.empty()) st.underwearPaths.push_back(facialLowerPath);
    if (!facialUpperPath.empty()) st.underwearPaths.push_back(facialUpperPath);
    for (const auto& up : underwearPaths) st.underwearPaths.push_back(up);
    onlinePlayerAppearance_[guid] = std::move(st);
}

std::vector<std::string> EntitySpawner::capeCandidatesFor(const pipeline::DBCFile& displayInfoDbc,
                                                          uint32_t capeDid, uint8_t genderId) {
    std::vector<std::string> capeCandidates;
    const int32_t capeRecIdx = displayInfoDbc.findRecordById(capeDid);
    if (capeRecIdx < 0) return capeCandidates;
    const auto* idiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    const uint32_t leftTexField = idiL ? (*idiL)["LeftModelTexture"] : 3u;
    // RightModelTexture is the field right after LeftModelTexture.
    // Some cloaks (e.g. Jaina's Radiance) carry their texture only in
    // the right field; the character-preview screen checks both, so
    // match it here - otherwise the world model shows the cape mesh
    // untextured even though the paperdoll preview looks correct.
    const uint32_t rightTexField = leftTexField + 1;
    std::string leftName = displayInfoDbc.getString(static_cast<uint32_t>(capeRecIdx), leftTexField);
    std::string rightName = displayInfoDbc.getString(static_cast<uint32_t>(capeRecIdx), rightTexField);

    std::vector<std::string> capeNames;
    auto addCapeName = [&](const std::string& n) {
        if (!n.empty() && std::find(capeNames.begin(), capeNames.end(), n) == capeNames.end())
            capeNames.push_back(n);
    };
    if (genderId == 1) { addCapeName(rightName); addCapeName(leftName); }
    else               { addCapeName(leftName);  addCapeName(rightName); }

    // Where a cape's art might be, in the order to try it -
    // pipeline/item_textures.hpp. Written out here, in the NPC
    // path and in the portrait, identically, which is the only
    // reason the three agreed.
    for (const auto& capeName : capeNames) {
        for (auto& c : pipeline::capeTextureCandidates(capeName, genderId == 1)) {
            if (std::find(capeCandidates.begin(), capeCandidates.end(), c) == capeCandidates.end()) {
                capeCandidates.push_back(std::move(c));
            }
        }
    }
    return capeCandidates;
}

void EntitySpawner::setOnlinePlayerEquipment(uint64_t guid,
                                          const std::array<uint32_t, 19>& displayInfoIds,
                                          const std::array<uint8_t, 19>& inventoryTypes) {
    if (!renderer_ || !renderer_->getCharacterRenderer() || !assetManager_ || !assetManager_->isInitialized()) return;

    // Skip local player - equipment handled by GameScreen::updateCharacterGeosets/Textures
    // via consumeOnlineEquipmentDirty(), which fires on the same server update.
    if (gameHandler_) {
        uint64_t localGuid = gameHandler_->getPlayerGuid();
        if (localGuid != 0 && guid == localGuid) return;
    }

    // If the player isn't spawned yet, store equipment until spawn.
    auto appIt = onlinePlayerAppearance_.find(guid);
    if (!playerInstances_.count(guid) || appIt == onlinePlayerAppearance_.end()) {
        pendingOnlinePlayerEquipment_[guid] = {displayInfoIds, inventoryTypes};
        return;
    }

    const OnlinePlayerAppearanceState& st = appIt->second;
    lastOnlinePlayerEquipment_[guid] = {displayInfoIds, inventoryTypes};

    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer) return;
    if (st.instanceId == 0 || st.modelId == 0) return;

    if (st.bodySkinPath.empty()) {
        LOG_DEBUG("setOnlinePlayerEquipment: bodySkinPath empty for guid=0x", std::hex, guid, std::dec,
                    " instanceId=", st.instanceId, " - skipping equipment");
        return;
    }

    int nonZeroDisplay = 0;
    for (uint32_t d : displayInfoIds) if (d != 0) nonZeroDisplay++;
    LOG_DEBUG("setOnlinePlayerEquipment: guid=0x", std::hex, guid, std::dec,
                " instanceId=", st.instanceId, " nonZeroDisplayIds=", nonZeroDisplay,
                " head=", displayInfoIds[0], " chest=", displayInfoIds[4],
                " legs=", displayInfoIds[6], " mainhand=", displayInfoIds[15]);

    auto displayInfoDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) return;

    auto hasInvType = [&](std::initializer_list<uint8_t> types) -> bool {
        for (int s = 0; s < 19; s++) {
            uint8_t inv = inventoryTypes[s];
            if (inv == 0) continue;
            for (uint8_t t : types) {
                if (inv == t) return true;
            }
        }
        return false;
    };

    // --- Geosets ---
    // Stock's rule (FUN_004ed900 with HelmetGeosetVisData), shared with the
    // local player, the NPCs and the portrait: core::buildStockCharacterGeosets.
    uint8_t hairStyleId = static_cast<uint8_t>((st.appearanceBytes >> 16) & 0xFF);
    core::CharacterGeosetQuery gq;
    for (int s = 0; s < 19; ++s) {
        const int kind = core::geosetKindForInventoryType(inventoryTypes[s]);
        if (kind >= 0 && displayInfoIds[s] != 0 && gq.displays[kind] == 0) gq.displays[kind] = displayInfoIds[s];
    }
    gq.hairScalp = selectHairScalpGeoset(hairGeosetMap_, st.raceId, st.genderId, hairStyleId);
    if (st.hdLook) {
        const AppearanceBytes stLook = unpackAppearanceBytes(st.appearanceBytes);
        pipeline::CharacterAppearance who;
        who.skinId = stLook.skinId;
        who.faceId = stLook.faceId;
        who.hairStyleId = hairStyleId;
        who.hairColorId = stLook.hairColorId;
        who.facialHairId = st.facialFeatures;
        const auto hd = pipeline::resolveHdCharacterGeosets(*st.hdLook, who);
        gq.hairScalp = hd.hairScalp > 0 ? hd.hairScalp : 1;
        gq.haveFacialRow = true;
        gq.facial100 = hd.facial100;
        gq.facial200 = hd.facial200;
        gq.facial300 = hd.facial300;
        gq.extra = hd.extra;
    } else if (auto itFacial = facialHairGeosetMap_.find(appearanceKey(st.raceId, st.genderId, st.facialFeatures));
        itFacial != facialHairGeosetMap_.end()) {
        gq.haveFacialRow = true;
        gq.facial100 = itFacial->second.geoset100;
        gq.facial200 = itFacial->second.geoset200;
        gq.facial300 = itFacial->second.geoset300;
        gq.facial1700 = itFacial->second.geoset1700;
    }
    gq.race = st.raceId;
    gq.gender = st.genderId;
    if (gameHandler_) {
        if (auto e = gameHandler_->getEntityManager().getEntity(guid)) {
            const uint32_t bytes0 = e->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
            gq.deathKnight = ((bytes0 >> 8) & 0xFF) == 6;
        }
    }
    std::unordered_set<uint16_t> geosets = core::buildStockCharacterGeosets(*assetManager_, gq);

    charRenderer->setActiveGeosets(st.instanceId, geosets);

    // --- Helmet model attachment ---
    // HEAD slot is index 0 in the 19-element equipment array.
    // Helmet M2s are race/gender-specific (e.g. Helm_Plate_B_01_HuM.m2 for Human Male).
    const core::ItemModelIdAllocator itemModelId =
        [this](const std::string& m2Path, const std::string& texturePath) {
            return itemModelIdFor(m2Path, texturePath);
        };
    // The shared helper (core/helm_visual.hpp), as the NPCs use: the per-race
    // cut first, then the base model, and no helm detaches it. This was a copy
    // of it that read and parsed the model again for every player.
    core::attachHelm(*charRenderer, *assetManager_, st.instanceId, displayInfoIds[0],
                     st.raceId, st.genderId, itemModelId);

    // --- Shoulder model attachment ---
    // SHOULDERS slot is index 2 in the 19-element equipment array. The shared
    // helper hangs LeftModel on 6 (ShoulderLeft) and RightModel on 5
    // (ShoulderRight); this used to have them the other way round, which
    // crossed both pads in toward the neck.
    core::attachShoulders(*charRenderer, *assetManager_, st.instanceId, displayInfoIds[2],
                          itemModelId);

    // --- Cape texture (group 15 / texture type 2) ---
    // The geoset above enables the cape mesh, but without a texture it renders blank.
    if (hasInvType({16})) {
        // Back/cloak is WoW equipment slot 14 (BACK) in the 19-element array.
        uint32_t capeDid = displayInfoIds[14];
        if (capeDid != 0) {
            {
                const std::vector<std::string> capeCandidates =
                    capeCandidatesFor(*displayInfoDbc, capeDid, st.genderId);
                if (!capeCandidates.empty()) {
                    const rendering::VkTexture* whiteTex = charRenderer->loadTexture("");
                    rendering::VkTexture* capeTexture = nullptr;
                    for (const auto& candidate : capeCandidates) {
                        rendering::VkTexture* tex = charRenderer->loadTexture(candidate);
                        if (tex && tex != whiteTex) {
                            capeTexture = tex;
                            break;
                        }
                    }

                    if (capeTexture) {
                        charRenderer->setGroupTextureOverride(st.instanceId, 15, capeTexture);
                        if (const auto* md = charRenderer->getModelData(st.modelId)) {
                            for (size_t ti = 0; ti < md->textures.size(); ti++) {
                                if (md->textures[ti].type == 2) {
                                    charRenderer->setTextureSlotOverride(
                                        st.instanceId, static_cast<uint16_t>(ti), capeTexture);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // --- Textures (skin atlas compositing) ---

    // Layered in the stock client's order, not slot order -
    // pipeline/item_textures.hpp.
    const bool isFemale = (st.genderId == 1);
    std::vector<pipeline::EquippedItemDisplay> equipped;
    for (int s = 0; s < 19; s++) {
        if (displayInfoIds[s] == 0) continue;
        equipped.push_back({pipeline::componentSlotForEquipSlot(s), displayInfoIds[s]});
    }
    std::vector<std::pair<int, std::string>> regionLayers =
        pipeline::buildItemRegionLayers(*assetManager_, *displayInfoDbc, equipped, isFemale);
    if (gameHandler_) core::applyGuildTabardFor(*gameHandler_, guid, regionLayers);

    const auto slotsIt = playerTextureSlotsByModelId_.find(st.modelId);
    if (slotsIt == playerTextureSlotsByModelId_.end()) return;
    const PlayerTextureSlots& slots = slotsIt->second;
    if (slots.skin < 0) return;

    rendering::VkTexture* newTex = charRenderer->compositeWithRegions(st.bodySkinPath, st.underwearPaths, regionLayers);
    if (newTex) {
        charRenderer->setTextureSlotOverride(st.instanceId, static_cast<uint16_t>(slots.skin), newTex);
    }

    // --- Weapon model attachment ---
    // Main hand, off hand and ranged slot (15, 16, 17), where the player's
    // sheath state (UNIT_FIELD_BYTES_2 byte 0) puts them: drawn in the hands,
    // or put away on the back and hips by each item's sheathe type, with the
    // ranged weapon on the back until it is the one drawn. This used to put
    // the main and off hand in the hands, always, and the ranged weapon
    // nowhere - so no other player ever sheathed, and a shield was held in
    // the left hand like a sword.
    UnitWeaponDisplays& slots3 = onlinePlayerWeaponSlots_[guid];
    for (int i = 0; i < 3; ++i) {
        slots3.displayIds[i] = displayInfoIds[15 + i];
        slots3.inventoryTypes[i] = inventoryTypes[15 + i];
    }
    attachUnitWeapons(guid, st.instanceId, slots3.displayIds, unitWeaponItems(guid));
}

void EntitySpawner::despawnPlayer(uint64_t guid) {
    if (!renderer_ || !renderer_->getCharacterRenderer()) return;
    pendingRemotePlayerMounts_.erase(guid);
    removeRemotePlayerMount(guid);
    auto* charRenderer = renderer_->getCharacterRenderer();
    // A form being built for this player, or drawn for it. The drawn one is
    // removed here and the character's own instance put back in the map, so
    // what follows frees the composite and never the shared creature model.
    pendingPlayerMorphs_.erase(guid);
    despawnCreature(morphSpawnKey(guid));
    if (auto morph = playerMorphs_.find(guid); morph != playerMorphs_.end()) {
        if (morph->second.instanceId != 0) charRenderer->removeInstance(morph->second.instanceId);
        if (auto pit = playerInstances_.find(guid); pit != playerInstances_.end())
            pit->second = morph->second.nativeInstanceId;
        playerMorphs_.erase(morph);
    }
    unitPresentedAlpha_.erase(guid);
    // A corpse's bones, drawn under the corpse guid.
    if (auto bones = corpseBonesInstances_.find(guid); bones != corpseBonesInstances_.end()) {
        charRenderer->removeInstance(bones->second);
        corpseBonesInstances_.erase(bones);
    }
    auto it = playerInstances_.find(guid);
    if (it == playerInstances_.end()) return;
    // Player composites get a fresh model id per spawn (unlike displayId-keyed
    // creature models, which stay cached), so free the model with the instance.
    const uint32_t compositeModelId = charRenderer->getInstanceModelId(it->second);
    charRenderer->removeInstance(it->second);
    if (compositeModelId != 0) charRenderer->unloadModelIfUnused(compositeModelId);
    playerInstances_.erase(it);
    onlinePlayerAppearance_.erase(guid);
    pendingOnlinePlayerEquipment_.erase(guid);
    onlinePlayerWeaponSlots_.erase(guid);
    unitSheathAttached_.erase(guid);
    unitSheathTransitions_.erase(guid);
    unitCastHolds_.erase(guid);
    deadCreatureGuids_.erase(guid);
    creatureRenderPosCache_.erase(guid);
    creatureSwimmingState_.erase(guid);
    creatureWalkingState_.erase(guid);
    creatureFlyingState_.erase(guid);
    unitMoveDirFlags_.erase(guid);
    unitGravityFreeFlags_.erase(guid);
    creatureRestingFloor_.erase(guid);
    unitBodyFacing_.erase(guid);
    unitLocomotion_.erase(guid);
    creatureWasMoving_.erase(guid);
    creatureWasSwimming_.erase(guid);
    creatureWasFlying_.erase(guid);
    creatureWasWalking_.erase(guid);
}

// ---------------------------------------------------------------------------
// Which hull a transport is drawn with
//
// Entry and displayId are different numbering spaces and this table used to mix
// them, which is how an elevator became a zeppelin. Every ship and zeppelin
// entry here shares one of five displayIds -- 3015, 3031, 7087, 7446, 7546 --
// and the numbers that were being compared against displayId were entries:
// 164871, 175080 and 176495 are the three vanilla zeppelins, and no displayId
// reaches six figures, so those three could never match.
//
// What did match was worse. 807 and 808 are the displayIds of Gnomeregan lifts
// (the "Vator" and the "Plunger"), 2454 belongs to the Searing Gorge scaffold
// cars and 1587 to a GameObject named, plainly, "Elevator" -- so every one of
// them was being drawn as an airship. Elevators are transports too, which is
// why the caller's guard lets them in.
//
// Values verified against gameobject_template.
std::string EntitySpawner::transportModelPath(uint32_t entry, uint32_t displayId) {
    struct TransportModel { uint32_t entry; uint32_t displayId; const char* path; };
    static constexpr const char* kShip     = "World\\wmo\\transports\\transport_ship\\transportship.wmo";
    static constexpr const char* kZeppelin = "World\\wmo\\transports\\transport_zeppelin\\transport_zeppelin.wmo";
    static constexpr const char* kHordeZep = "World\\wmo\\transports\\transport_horde_zeppelin\\Transport_Horde_Zeppelin.wmo";
    static constexpr const char* kIceship  = "World\\wmo\\transports\\icebreaker\\Transport_Icebreaker_ship.wmo";
    // NOLINTBEGIN(modernize-use-designated-initializers) - a table whose
    // columns are its field names, with the struct in view directly above.
    static constexpr TransportModel kTransportModels[] = {
        // Ships (display 3015)
        {  20808, 3015, kShip },      // The Maiden's Fancy
        { 176231, 3015, kShip },      // The Lady Mehley
        { 176310, 3015, kShip },      // The Bravery
        // Zeppelins (display 3031)
        { 164871, 3031, kZeppelin },  // The Thundercaller
        { 175080, 3031, kZeppelin },  // The Iron Eagle
        { 176495, 3031, kZeppelin },  // The Purple Princess
        { 186371, 3031, kZeppelin },
        { 190549, 3031, kZeppelin },  // The Zephyr
        // Horde zeppelins (display 7546)
        { 181689, 7546, kHordeZep },  // Cloudkisser
        { 186238, 7546, kHordeZep },  // The Mighty Wind
        { 201834, 7546, kHordeZep },
        // Icebreakers (display 7446)
        { 181688, 7446, kIceship },   // Northspear
        { 190536, 7446, kIceship },   // Stormwind's Pride
    };
    // NOLINTEND(modernize-use-designated-initializers)

    for (const TransportModel& t : kTransportModels) {
        if (entry == t.entry || displayId == t.displayId) return t.path;
    }
    // The Deeprun Tram car, which is an M2 rather than a WMO and is keyed on a
    // displayId that is genuinely a displayId: entries 176080-176086 all carry
    // 3831.
    if (displayId == 3831) {
        return "World\\Generic\\Gnome\\Passive Doodads\\Subway\\SubwayCar.m2";
    }
    return "";
}

void EntitySpawner::spawnOnlineGameObject(uint64_t guid, uint32_t entry, uint32_t displayId, float x, float y, float z, float orientation, float scale,
                                          const PreparedGameObjectM2* preparedM2) {
    if (!renderer_ || !assetManager_) return;

    if (!gameObjectLookupsBuilt_) {
        buildGameObjectDisplayLookups();
    }
    if (!gameObjectLookupsBuilt_) return;

    LOG_DEBUG("GO spawn attempt: guid=0x", std::hex, guid, std::dec,
             " displayId=", displayId, " entry=", entry,
             " pos=(", x, ", ", y, ", ", z, ")");

    auto goIt = gameObjectInstances_.find(guid);
    if (goIt != gameObjectInstances_.end()) {
        // A tracked instance ID is only meaningful while the renderer still holds
        // it. Renderer-wide clears (map change, device reset) drop instances
        // without going through despawnGameObject(), which used to leave this map
        // pointing at a dead handle - every later server CREATE for that GUID then
        // took the position-update path below and the object stayed invisible for
        // the rest of the session. Treat a dead handle as "not spawned".
        bool instanceAlive = false;
        if (renderer_) {
            if (goIt->second.isWmo) {
                auto* wr = renderer_->getWMORenderer();
                instanceAlive = wr && wr->hasInstance(goIt->second.instanceId);
            } else {
                auto* mr = renderer_->getM2Renderer();
                instanceAlive = mr && mr->hasInstance(goIt->second.instanceId);
            }
        }
        if (!instanceAlive) {
            LOG_WARNING("GO render instance vanished - respawning: guid=0x", std::hex, guid, std::dec,
                        " displayId=", displayId, " instanceId=", goIt->second.instanceId);
            gameObjectInstances_.erase(goIt);
            goIt = gameObjectInstances_.end();
        }
    }
    if (goIt != gameObjectInstances_.end()) {
        if (gameHandler_ && gameHandler_->isTransportGuid(guid)) {
            if (auto* transportManager = gameHandler_->getTransportManager()) {
                if (transportManager->getTransport(guid)) {
                    transportManager->rebindTransportInstance(
                        guid, goIt->second.instanceId, !goIt->second.isWmo, displayId);
                    transportManager->updateServerTransport(
                        guid, glm::vec3(x, y, z), orientation);
                } else {
                    gameHandler_->notifyTransportSpawned(guid, entry, displayId, x, y, z, orientation);
                }
            } else {
                gameHandler_->notifyTransportSpawned(guid, entry, displayId, x, y, z, orientation);
            }
            return;
        }

        // Already have a render instance - update its position (e.g. transport re-creation)
        auto& info = goIt->second;
        glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
        LOG_DEBUG("GameObject position update: displayId=", displayId, " guid=0x", std::hex, guid, std::dec,
                 " pos=(", x, ", ", y, ", ", z, ")");
        if (renderer_) {
            if (info.isWmo) {
                if (auto* wr = renderer_->getWMORenderer()) {
                    glm::mat4 transform(1.0f);
                    transform = glm::translate(transform, renderPos);
                    transform = glm::rotate(transform, orientation, glm::vec3(0, 0, 1));
                    wr->setInstanceTransform(info.instanceId, transform);
                }
            } else {
                if (auto* mr = renderer_->getM2Renderer()) {
                    glm::mat4 transform(1.0f);
                    transform = glm::translate(transform, renderPos);
                    mr->setInstanceTransform(info.instanceId, transform);
                }
            }
        }
        return;
    }

    std::string modelPath;

    if (gameHandler_ && gameHandler_->isTransportGuid(guid)) {
        modelPath = transportModelPath(entry, displayId);
        if (!modelPath.empty()) {
            LOG_INFO("Transport entry/display ", entry, "/", displayId, " → ", modelPath);
        }
    }

    // Fallback to normal displayId lookup if not a transport or no override matched
    if (modelPath.empty()) {
        modelPath = getGameObjectModelPathForDisplayId(displayId);
    }

    if (modelPath.empty()) {
        LOG_WARNING("No model path for gameobject displayId ", displayId, " (guid 0x", std::hex, guid, std::dec, ")");
        return;
    }

    // Log spawns to help debug duplicate objects (e.g., cathedral issue)
    LOG_DEBUG("GameObject spawn: displayId=", displayId, " guid=0x", std::hex, guid, std::dec,
             " model=", modelPath, " pos=(", x, ", ", y, ", ", z, ")");

    std::string lowerPath = modelPath;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    bool isWmo = lowerPath.size() >= 4 && lowerPath.substr(lowerPath.size() - 4) == ".wmo";

    glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
    const float renderYawWmo = orientation;
    // M2 game objects: model default faces +renderX. renderYaw = canonical + 90° = server_yaw
    // (same offset as creature/character renderer_ so all M2 models face consistently)
    const float renderYawM2go = orientation + glm::radians(90.0f);

    bool loadedAsWmo = false;
    if (isWmo) {
        auto* wmoRenderer = renderer_->getWMORenderer();
        if (!wmoRenderer) return;

        uint32_t modelId = 0;
        auto itCache = gameObjectDisplayIdWmoCache_.find(displayId);
        if (itCache != gameObjectDisplayIdWmoCache_.end()) {
            modelId = itCache->second;
            // Only use cached entry if the model is still resident in the renderer_
            if (wmoRenderer->isModelLoaded(modelId)) {
                loadedAsWmo = true;
            } else {
                gameObjectDisplayIdWmoCache_.erase(itCache);
                modelId = 0;
            }
        }
        if (!loadedAsWmo && modelId == 0) {
            auto wmoData = assetManager_->readFile(modelPath);
            if (!wmoData.empty()) {
                pipeline::WMOModel wmoModel = pipeline::WMOLoader::load(wmoData);
                LOG_DEBUG("Gameobject WMO root loaded: ", modelPath, " nGroups=", wmoModel.nGroups);
                int loadedGroups = 0;
                if (wmoModel.nGroups > 0) {
                    for (uint32_t gi = 0; gi < wmoModel.nGroups; gi++) {
                        bool loaded = false;
                        for (const std::string& groupPath :
                             pipeline::wmoGroupCandidates(modelPath, gi)) {
                            std::vector<uint8_t> groupData =
                                assetManager_->readFile(groupPath);
                            if (groupData.empty()) continue;
                            pipeline::WMOLoader::loadGroup(groupData, wmoModel, gi);
                            loadedGroups++;
                            loaded = true;
                            break;
                        }
                        if (!loaded) {
                            LOG_WARNING("  Failed to load WMO group ", gi, " for: ", modelPath);
                        }
                    }
                }

                if (loadedGroups > 0 || wmoModel.nGroups == 0) {
                    modelId = nextGameObjectWmoModelId_++;
                    if (wmoRenderer->loadModel(wmoModel, modelId)) {
                        gameObjectDisplayIdWmoCache_[displayId] = modelId;
                        loadedAsWmo = true;
                    } else {
                        LOG_WARNING("Failed to load gameobject WMO model: ", modelPath);
                    }
                } else {
                    LOG_WARNING("No WMO groups loaded for gameobject: ", modelPath,
                                " - falling back to M2");
                }
            } else {
                LOG_WARNING("Failed to read gameobject WMO: ", modelPath, " - falling back to M2");
            }
        }

        if (loadedAsWmo) {
            uint32_t instanceId = wmoRenderer->createInstance(modelId, renderPos,
                glm::vec3(0.0f, 0.0f, renderYawWmo), scale);
            if (instanceId == 0) {
                LOG_WARNING("Failed to create gameobject WMO instance for guid 0x", std::hex, guid, std::dec);
                return;
            }

            gameObjectInstances_[guid] = {.modelId = modelId, .instanceId = instanceId, .isWmo = true};
            // Fade in new scenery: a server object coming into range
            // dissolves in as streamed scenery does (no-op with it off).
            wmoRenderer->markStreamedIn(instanceId);
            LOG_DEBUG("Spawned gameobject WMO: guid=0x", std::hex, guid, std::dec,
                     " displayId=", displayId, " at (", x, ", ", y, ", ", z, ")");

            // Spawn transport WMO doodads (chairs, furniture, etc.) as child M2 instances
            bool isTransport = false;
            if (gameHandler_) {
                std::string lowerModelPath = modelPath;
                std::transform(lowerModelPath.begin(), lowerModelPath.end(), lowerModelPath.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                isTransport = (lowerModelPath.find("transport") != std::string::npos);
            }

            auto* m2Renderer = renderer_->getM2Renderer();
            if (m2Renderer && isTransport) {
                const auto* doodadTemplates = wmoRenderer->getDoodadTemplates(modelId);
                if (doodadTemplates && !doodadTemplates->empty()) {
                    constexpr size_t kMaxTransportDoodads = 192;
                    const size_t doodadBudget = std::min(doodadTemplates->size(), kMaxTransportDoodads);
                    LOG_DEBUG("Queueing ", doodadBudget, "/", doodadTemplates->size(),
                             " transport doodads for WMO instance ", instanceId);
                    pendingTransportDoodadBatches_.push_back(PendingTransportDoodadBatch{
                        guid,
                        modelId,
                        instanceId,
                        0,
                        doodadBudget,
                        0,
                        x, y, z,
                        orientation
                    });
                } else {
                LOG_DEBUG("Transport WMO has no doodads or templates not available");
            }
            }

            // Transport GameObjects are not always named "transport" in their WMO path
            // (e.g. elevators/lifts). If the server marks it as a transport, always
            // notify so TransportManager can animate/carry passengers.
            bool isTG = gameHandler_ && gameHandler_->isTransportGuid(guid);
            LOG_DEBUG("WMO GO spawned: guid=0x", std::hex, guid, std::dec,
                       " entry=", entry, " displayId=", displayId,
                       " isTransport=", isTG,
                       " pos=(", x, ", ", y, ", ", z, ")");
            if (isTG) {
                gameHandler_->notifyTransportSpawned(guid, entry, displayId, x, y, z, orientation);
            }

            return;
        }

        // WMO failed - fall through to try as M2
        // Convert .wmo path to .m2 for fallback
        modelPath = modelPath.substr(0, modelPath.size() - 4) + ".m2";
    }

    {
        auto* m2Renderer = renderer_->getM2Renderer();
        if (!m2Renderer) return;

        // Skip displayIds that permanently failed to load (e.g. empty/unsupported M2s).
        // Without this guard the same empty model is re-parsed every frame, causing
        // sustained log spam and wasted CPU.
        if (gameObjectDisplayIdFailedCache_.count(displayId)) return;

        uint32_t modelId = 0;
        auto itCache = gameObjectDisplayIdModelCache_.find(displayId);
        if (itCache != gameObjectDisplayIdModelCache_.end()) {
            modelId = itCache->second;
            if (!m2Renderer->hasModel(modelId)) {
                LOG_WARNING("GO M2 cache hit but model gone: displayId=", displayId,
                            " modelId=", modelId, " path=", modelPath,
                            " - reloading");
                gameObjectDisplayIdModelCache_.erase(itCache);
                itCache = gameObjectDisplayIdModelCache_.end();
            }
        }
        if (itCache == gameObjectDisplayIdModelCache_.end() && preparedM2 && preparedM2->model &&
            preparedM2->modelPath == modelPath) {
            // Read, parsed and decoded by a worker (launchGameObjectM2Loads):
            // only the upload is left.
            modelId = nextGameObjectModelId_++;
            m2Renderer->setPredecodedBLPCache(
                const_cast<std::unordered_map<std::string, pipeline::BLPImage>*>(
                    &preparedM2->predecodedTextures));
            const bool loaded = m2Renderer->loadModel(*preparedM2->model, modelId);
            m2Renderer->setPredecodedBLPCache(nullptr);
            if (!loaded) {
                LOG_WARNING("Failed to load gameobject model: ", modelPath);
                gameObjectDisplayIdFailedCache_.insert(displayId);
                return;
            }
            m2Renderer->setModelPinned(modelId, true);
            gameObjectDisplayIdModelCache_[displayId] = modelId;
            itCache = gameObjectDisplayIdModelCache_.find(displayId);
        }
        if (itCache == gameObjectDisplayIdModelCache_.end()) {
            modelId = nextGameObjectModelId_++;

            auto m2Data = assetManager_->readFile(modelPath);
            if (m2Data.empty()) {
                LOG_WARNING("Failed to read gameobject M2: ", modelPath);
                gameObjectDisplayIdFailedCache_.insert(displayId);
                return;
            }

            pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
            // Collision classification needs the asset path. Embedded M2 names
            // are often generic and caused herb/grass gameobjects to be treated
            // as solid props.
            model.name = modelPath;
            if (model.vertices.empty()) {
                LOG_WARNING("Failed to parse gameobject M2: ", modelPath);
                gameObjectDisplayIdFailedCache_.insert(displayId);
                return;
            }

            std::string skinPath = pipeline::skinPathForM2(modelPath);
            auto skinData = assetManager_->readFile(skinPath);
            if (!skinData.empty() && model.version >= 264) {
                pipeline::M2Loader::loadSkin(skinData, model);
            } else if (skinData.empty() && model.version >= 264) {
                LOG_WARNING("GO skin file MISSING for WotLK M2 (no indices/batches): ", skinPath);
            }

            LOG_DEBUG("GO model: ", modelPath, " v=", model.version,
                     " verts=", model.vertices.size(),
                     " idx=", model.indices.size(),
                     " batches=", model.batches.size(),
                     " bones=", model.bones.size(),
                     " skin=", (skinData.empty() ? "MISSING" : "ok"));

            if (!m2Renderer->loadModel(model, modelId)) {
                LOG_WARNING("Failed to load gameobject model: ", modelPath);
                gameObjectDisplayIdFailedCache_.insert(displayId);
                return;
            }

            // Keep game object models resident across the away-and-back cycle.
            // Leaving town drops every instance of them, and the 60s reaper then
            // evicted the model - the log showed PostBoxHuman.m2 (the mailbox)
            // going through exactly that reap/reload churn on every return trip.
            m2Renderer->setModelPinned(modelId, true);
            gameObjectDisplayIdModelCache_[displayId] = modelId;
        }

        uint32_t instanceId = m2Renderer->createInstance(modelId, renderPos,
            glm::vec3(0.0f, 0.0f, renderYawM2go), scale);
        if (instanceId == 0) {
            LOG_WARNING("Failed to create gameobject instance for guid 0x", std::hex, guid, std::dec);
            return;
        }

        // A tilted object - the Eye of the Storm's start domes lean 20 degrees -
        // is placed by its full rotation, as stock places every game object;
        // the facing alone stood the dome upright, its walls and floor where
        // nothing is drawn. The world axes here are the server's, so the
        // rotation's tilt is what is left of it once the facing is taken out,
        // applied about the object's position.
        if (gameHandler_) {
            auto ent = gameHandler_->getEntityManager().getEntity(guid);
            const auto* goEnt = dynamic_cast<const game::GameObject*>(ent.get());
            if (goEnt && goEnt->hasRotation()) {
                const float* r = goEnt->rotation();
                const glm::quat full(r[3], r[0], r[1], r[2]);
                // Only the lean is taken from the rotation: its twist about
                // the vertical is split off (swing-twist) and the facing stays
                // the orientation's, as before. Taking the facing out by angle
                // instead turned every object by the difference between the
                // two yaw conventions - every shop sign in Stormwind.
                const float twistLen = std::sqrt(full.w * full.w + full.z * full.z);
                const glm::quat twist = twistLen > 1e-6f
                    ? glm::quat(full.w / twistLen, 0.0f, 0.0f, full.z / twistLen)
                    : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
                const glm::quat tilt = glm::normalize(full * glm::inverse(twist));
                // Within about a degree of upright: leave the placement alone.
                if (std::abs(tilt.w) < 0.99996f) {
                    glm::mat4 base(1.0f);
                    if (m2Renderer->getInstanceModelMatrix(instanceId, base)) {
                        const glm::mat4 tilted = glm::translate(glm::mat4(1.0f), renderPos) *
                                                 glm::mat4_cast(tilt) *
                                                 glm::translate(glm::mat4(1.0f), -renderPos) * base;
                        m2Renderer->setInstanceTransform(instanceId, tilted);
                    }
                }
            }
        }

        // Server game objects are gameplay props, not scenery: exempt them from the
        // adaptive doodad render distance, which drops to ~200 units in a city and
        // was hiding mailboxes/chests well inside the range the server still
        // considers them visible.
        m2Renderer->setInstanceIsGameObject(instanceId, true);
        // Fade in new scenery (no-op with it off), as the WMO objects above.
        m2Renderer->markStreamedIn(instanceId);

        // Deeprun Tram cars: riding never used real mesh collision to begin with (Z is
        // fully code-locked to the transport's simulated position while boarded, not
        // derived from a floor query), so the solid SubwayCar.m2 body was only ever in
        // the way - reported live as getting physically stuck walking back across a car
        // after crossing it once. Skip collision so the model is purely visual/decorative
        // for movement purposes, matching how the boarding logic already treats it (a
        // proximity/footprint check, not a physical block).
        if (displayId == 3831u) {
            m2Renderer->setSkipCollision(instanceId, true);
        }

        // Transports are driven by the transport system rather than a looping
        // idle. Everything else plays its default sequence until its state
        // arrives (in the create block, applied below), as the stock model
        // does before the handler's first kind; no freezing by type.
        bool isTransportGO = gameHandler_ && gameHandler_->isTransportGuid(guid);

        gameObjectInstances_[guid] = {.modelId = modelId, .instanceId = instanceId, .isWmo = false};
        gameObjectInstances_[guid].displayId = displayId;
        // Its model's events sound (0x70cac0 -> 0x70c050). A transport's
        // handler fires nothing (types 11 and 15: 0x6b1b80).
        if (!isTransportGO) {
            ensureGameObjectSounds();
            goSounds_->addObject(instanceId, displayId);
            m2Renderer->setInstanceFiresObjectEvents(instanceId, true);
        }

        // ...and the pose the server described before this model existed:
        // the state that arrived ahead of it, or else the one the entity
        // already holds. A model built again for a known object (its render
        // instance dropped by a renderer clear, a display change) had no
        // pending state left and came back in its default pose, so a door
        // the server had opened stood shut until the server spoke again.
        if (auto pending = gameObjectPendingState_.find(guid);
            pending != gameObjectPendingState_.end()) {
            applyGameObjectState(guid, pending->second);
        } else if (gameHandler_) {
            const uint16_t bytes1 = game::fieldIndex(game::UF::GAMEOBJECT_BYTES_1);
            auto entity = gameHandler_->getEntityManager().getEntity(guid);
            if (bytes1 != 0xFFFF && entity && entity->hasField(bytes1)) {
                const auto state = static_cast<uint8_t>(entity->getField(bytes1) & 0xFF);
                gameObjectPendingState_[guid] = state;  // straight to its pose
                applyGameObjectState(guid, state);
            }
        }

        // Notify transport system for M2 transports (e.g. Deeprun Tram cars)
        if (gameHandler_ && gameHandler_->isTransportGuid(guid)) {
            LOG_DEBUG("M2 transport spawned: guid=0x", std::hex, guid, std::dec,
                       " entry=", entry, " displayId=", displayId,
                       " instanceId=", instanceId);
            gameHandler_->notifyTransportSpawned(guid, entry, displayId, x, y, z, orientation);
        }
    }

    LOG_DEBUG("Spawned gameobject: guid=0x", std::hex, guid, std::dec,
             " displayId=", displayId, " at (", x, ", ", y, ", ", z, ")");
}

// ---------------------------------------------------------------------------
// Player morphs: shapeshift forms, polymorph, costumes
//
// The server says a unit looks like something else by putting another display
// in UNIT_FIELD_DISPLAYID while UNIT_FIELD_NATIVEDISPLAYID keeps the character's
// own. Creatures were already re-spawned on that; players - the local one and
// everyone else - were drawn as themselves whatever it said, so there was no
// bear, cat, travel or flight form, no ghost wolf, no sheep and no costume.
// ---------------------------------------------------------------------------

bool EntitySpawner::isLocalPlayer(uint64_t guid) const {
    return gameHandler_ && guid != 0 && guid == gameHandler_->getPlayerGuid();
}

void EntitySpawner::setPlayerMorphDisplayId(uint64_t guid, uint32_t displayId) {
    if (guid == 0) return;
    auto current = playerMorphs_.find(guid);
    const uint32_t drawn = current != playerMorphs_.end() ? current->second.displayId : 0;
    if (displayId == drawn) {
        // Back to what is already drawn before the other one was built
        // (cat, then straight back to caster): drop that build.
        if (pendingPlayerMorphs_.erase(guid)) despawnCreature(morphSpawnKey(guid));
        return;
    }
    pendingPlayerMorphs_[guid] = displayId;
}

void EntitySpawner::applyPlayerMorph(uint64_t guid, uint32_t displayId, uint32_t morphInstanceId,
                                     uint32_t nativeInstanceId, bool isLocal) {
    auto* cr = renderer_->getCharacterRenderer();
    // What is drawn now: the character, or the form this one replaces.
    uint32_t shownInstance = nativeInstanceId;
    uint32_t oldMorphInstance = 0;
    if (auto old = playerMorphs_.find(guid); old != playerMorphs_.end()) {
        oldMorphInstance = old->second.instanceId;
        shownInstance = oldMorphInstance;
    }

    // Where the drawn model is, so the form appears in its place; the player
    // sync (or the renderer, for the local player) keeps it there from now.
    glm::vec3 pos(0.0f);
    if (cr->getInstancePosition(shownInstance, pos)) cr->setInstancePosition(morphInstanceId, pos);
    // At once, as the stock client does. The spawn path fades a creature in,
    // and a form that faded in over a hidden character would blink out.
    cr->setInstanceOpacity(morphInstanceId, 1.0f);

    // Carry on with what the unit was doing: the same sequence where the form
    // has it (Run, Stand, a swing), Stand where it does not, the death pose
    // held.
    uint32_t curAnim = 0;
    float animTime = 0.0f, animDuration = 0.0f;
    if (cr->isDeathLocked(shownInstance)) {
        cr->playDeath(morphInstanceId, /*alreadyDead=*/true);
    } else {
        const bool known = cr->getAnimationState(shownInstance, curAnim, animTime, animDuration);
        const uint32_t anim = (known && cr->hasAnimation(morphInstanceId, curAnim))
            ? curAnim : static_cast<uint32_t>(rendering::anim::STAND);
        cr->playAnimation(morphInstanceId, anim, /*loop=*/true);
    }

    if (isLocal) {
        renderer_->setCharacterMorphInstance(morphInstanceId);
    } else {
        playerInstances_[guid] = morphInstanceId;
        cr->setInstanceVisible(nativeInstanceId, false);
        cr->setInstanceVisible(morphInstanceId, true);
    }
    if (oldMorphInstance != 0) cr->removeInstance(oldMorphInstance);
    playerMorphs_[guid] = {.displayId = displayId, .instanceId = morphInstanceId,
                           .nativeInstanceId = nativeInstanceId};
    LOG_INFO("Player morph: guid=0x", std::hex, guid, std::dec, " displayId=", displayId,
             " model=", getModelPathForDisplayId(displayId), isLocal ? " (local)" : "");
}

void EntitySpawner::clearPlayerMorph(uint64_t guid) {
    auto it = playerMorphs_.find(guid);
    if (it == playerMorphs_.end()) return;
    const PlayerMorph morph = it->second;
    playerMorphs_.erase(it);
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!cr) return;

    const bool local = isLocalPlayer(guid);
    const bool nativeAlive = local
        ? (renderer_->getNativeCharacterInstanceId() == morph.nativeInstanceId)
        : (playerInstances_.count(guid) != 0);
    if (nativeAlive && morph.nativeInstanceId != 0) {
        glm::vec3 pos(0.0f);
        if (cr->getInstancePosition(morph.instanceId, pos))
            cr->setInstancePosition(morph.nativeInstanceId, pos);
        uint32_t curAnim = 0;
        float animTime = 0.0f, animDuration = 0.0f;
        if (cr->isDeathLocked(morph.instanceId)) {
            cr->playDeath(morph.nativeInstanceId, /*alreadyDead=*/true);
        } else if (!cr->isDeathLocked(morph.nativeInstanceId)) {
            const bool known = cr->getAnimationState(morph.instanceId, curAnim, animTime, animDuration);
            const uint32_t anim = (known && cr->hasAnimation(morph.nativeInstanceId, curAnim))
                ? curAnim : static_cast<uint32_t>(rendering::anim::STAND);
            cr->playAnimation(morph.nativeInstanceId, anim, /*loop=*/true);
        }
        if (local) {
            renderer_->setCharacterMorphInstance(0);
        } else {
            playerInstances_[guid] = morph.nativeInstanceId;
            cr->setInstanceVisible(morph.nativeInstanceId, true);
        }
    }
    if (morph.instanceId != 0) cr->removeInstance(morph.instanceId);
    LOG_INFO("Player morph cleared: guid=0x", std::hex, guid, std::dec,
             " displayId=", morph.displayId, local ? " (local)" : "");
}

void EntitySpawner::processPendingPlayerMorphs() {
    if (!renderer_ || !gameHandler_) return;
    auto* cr = renderer_->getCharacterRenderer();
    if (!cr) return;

    // The local character's instance is replaced on a relog, a teleport
    // reload or a race change. The form drawn for the old one is gone with
    // it (the renderer handed its slot to the new character): drop the
    // stale instance and build the form again for the new one.
    if (const uint64_t self = gameHandler_->getPlayerGuid(); self != 0) {
        auto local = playerMorphs_.find(self);
        if (local != playerMorphs_.end() &&
            renderer_->getNativeCharacterInstanceId() != local->second.nativeInstanceId) {
            const PlayerMorph stale = local->second;
            playerMorphs_.erase(local);
            if (renderer_->getCharacterInstanceId() == stale.instanceId)
                renderer_->setCharacterMorphInstance(0);
            cr->removeInstance(stale.instanceId);
            if (!pendingPlayerMorphs_.count(self)) pendingPlayerMorphs_[self] = stale.displayId;
        }
    }

    // A drawn form keeps the character hidden underneath. Equipment changes
    // re-attach weapons to it, and new attachments start out visible.
    for (const auto& [guid, morph] : playerMorphs_) {
        (void)guid;
        if (morph.nativeInstanceId != 0) cr->setInstanceVisible(morph.nativeInstanceId, false);
    }

    for (auto it = pendingPlayerMorphs_.begin(); it != pendingPlayerMorphs_.end(); ) {
        const uint64_t guid = it->first;
        const uint32_t want = it->second;
        const uint64_t key = morphSpawnKey(guid);
        const bool local = isLocalPlayer(guid);

        if (want == 0) {
            despawnCreature(key);
            clearPlayerMorph(guid);
            it = pendingPlayerMorphs_.erase(it);
            continue;
        }
        if (auto cur = playerMorphs_.find(guid);
            cur != playerMorphs_.end() && cur->second.displayId == want) {
            despawnCreature(key);
            it = pendingPlayerMorphs_.erase(it);
            continue;
        }

        // The character has to be drawn first: its spawn can still be
        // queued when the fields that say it is shapeshifted arrive.
        uint32_t nativeInstance = 0;
        if (local) {
            nativeInstance = renderer_->getNativeCharacterInstanceId();
        } else if (auto app = onlinePlayerAppearance_.find(guid); app != onlinePlayerAppearance_.end()) {
            nativeInstance = app->second.instanceId;
        }
        if (nativeInstance == 0) { ++it; continue; }

        // Built: take it out of the creature maps and draw it for the player.
        if (auto built = creatureDisplayIds_.find(key);
            built != creatureDisplayIds_.end() && built->second == want &&
            creatureInstances_.count(key)) {
            const uint32_t morphInstance = releaseCreatureInstance(key);
            if (morphInstance != 0) applyPlayerMorph(guid, want, morphInstance, nativeInstance, local);
            it = pendingPlayerMorphs_.erase(it);
            continue;
        }
        // Not drawable (no model, a helper display): stay as the character.
        if (nonRenderableCreatureDisplayIds_.count(want) || creaturePermanentFailureGuids_.count(key)) {
            LOG_WARNING("Player morph display ", want, " has no drawable model; guid=0x",
                        std::hex, guid, std::dec);
            despawnCreature(key);
            clearPlayerMorph(guid);
            it = pendingPlayerMorphs_.erase(it);
            continue;
        }
        // Not asked for yet (or asked for another display): queue the build.
        if (auto req = requestedCreatureDisplayIds_.find(key);
            req == requestedCreatureDisplayIds_.end() || req->second != want) {
            if (creatureInstances_.count(key)) despawnCreature(key);
            float x = 0.0f, y = 0.0f, z = 0.0f, o = 0.0f, scale = 1.0f;
            if (auto entity = gameHandler_->getEntityManager().getEntity(guid)) {
                x = entity->getLatestX(); y = entity->getLatestY(); z = entity->getLatestZ();
                o = entity->getOrientation();
                // OBJECT_FIELD_SCALE_X; the display's own scale is applied by
                // the creature path on top, as for any creature.
                const uint16_t scaleIdx = game::fieldIndex(game::UF::OBJECT_FIELD_SCALE_X);
                if (scaleIdx != 0xFFFF) {
                    const uint32_t raw = entity->getField(scaleIdx);
                    float s = 0.0f;
                    std::memcpy(&s, &raw, sizeof(s));
                    if (raw != 0 && s > 0.01f && s < 100.0f) scale = s;
                }
            }
            queueCreatureSpawn(key, want, x, y, z, o, scale);
        }
        ++it;
    }
}

// ---------------------------------------------------------------------------
// Corpse bones
// ---------------------------------------------------------------------------

void EntitySpawner::spawnCorpseBones(uint64_t guid, uint8_t raceId, uint8_t genderId,
                                     float x, float y, float z, float orientation) {
    if (!renderer_ || !assetManager_ || guid == 0) return;
    auto* cr = renderer_->getCharacterRenderer();
    if (!cr || corpseBonesInstances_.count(guid)) return;
    const auto race = static_cast<game::Race>(raceId);
    if (!game::hasPlayerModel(race)) return;

    // wow.exe: "World\Generic\PassiveDoodads\DeathSkeletons\%s%sDeathSkeleton.mdx",
    // the race's file name and the sex - the same two words that name the
    // character model, which is where they are taken from here.
    const std::string charPath = game::getPlayerModelPath(
        race, genderId == 1 ? game::Gender::FEMALE : game::Gender::MALE);
    const size_t slash = charPath.find_last_of('\\');
    const size_t dot = charPath.find_last_of('.');
    if (slash == std::string::npos || dot == std::string::npos || dot <= slash) return;
    const std::string path = "World\\Generic\\PassiveDoodads\\DeathSkeletons\\" +
        charPath.substr(slash + 1, dot - slash - 1) + "DeathSkeleton.m2";

    uint32_t modelId = 0;
    if (auto cached = corpseBonesModels_.find(path); cached != corpseBonesModels_.end()) {
        modelId = cached->second;
    } else {
        pipeline::M2Model model;
        if (pipeline::loadM2WithSkin(*assetManager_, path, model) && model.isValid()) {
            modelId = nextCreatureModelId_++;
            if (!cr->loadModel(model, modelId)) modelId = 0;
        }
        if (modelId == 0) LOG_WARNING("Corpse bones model missing: ", path);
        corpseBonesModels_[path] = modelId;
    }
    if (modelId == 0) return;

    const glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
    const uint32_t instanceId = cr->createInstance(
        modelId, renderPos, glm::vec3(0.0f, 0.0f, orientation + glm::radians(90.0f)), 1.0f);
    if (instanceId == 0) return;
    cr->playAnimation(instanceId, rendering::anim::STAND, /*loop=*/true);
    corpseBonesInstances_[guid] = instanceId;
    LOG_DEBUG("Corpse bones: guid=0x", std::hex, guid, std::dec, " model=", path);
}

} // namespace core
} // namespace wowee
