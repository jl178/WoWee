#include "ui/game_screen.hpp"
#include "ui/sight_cache.hpp"
#include "addons/lua_api_registrations.hpp"
#include "ui/ui_texture_load.hpp"
#include "ui/ui_upload_budget.hpp"
#include "core/helm_visual.hpp"
#include "ui/ui_raid_icons.hpp"
#include "ui/ui_colors.hpp"
#include "ui/ui_helpers.hpp"
#include "ui/nameplate_stacking.hpp"
#include "ui/interface_fonts.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/window.hpp"
#include "rendering/vk_context.hpp"
#include "core/application.hpp"
#include "core/appearance_composer.hpp"
#include "ui/map_window.hpp"
#include "addons/addon_manager.hpp"
#include "core/coordinates.hpp"
#include "core/input.hpp"
#include "rendering/renderer.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/minimap.hpp"
#include "rendering/world_map.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/music_manager.hpp"
#include "game/zone_manager.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/ambient_sound_manager.hpp"
#include "audio/ui_sound_manager.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/spell_sound_manager.hpp"
#include "audio/movement_sound_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "core/geoset_rules.hpp"
#include "pipeline/item_textures.hpp"

#include "game/expansion_profile.hpp"
#include "game/character.hpp"
#include "core/logger.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <chrono>
#include <ctime>
#include <limits>

#include <unordered_set>
#include "ui/framexml_takeover.hpp"
#include "addons/lua_api_helpers.hpp"
#include "core/local_time.hpp"
#include "pipeline/spell_icon_paths.hpp"

namespace {
    using namespace wowee::ui::colors;
    using namespace wowee::ui::helpers;



}

namespace wowee { namespace ui {



void GameScreen::updateCharacterGeosets(game::Inventory& inventory) {
    auto& app = core::Application::getInstance();
    auto* renderer = app.getRenderer();
    if (!renderer) return;

    // The character's own model, also while a form is drawn in its place.
    uint32_t instanceId = renderer->getNativeCharacterInstanceId();
    if (instanceId == 0) return;

    auto* charRenderer = renderer->getCharacterRenderer();
    if (!charRenderer) return;

    auto* assetManager = app.getAssetManager();

    // Load ItemDisplayInfo.dbc for geosetGroup lookup
    std::shared_ptr<pipeline::DBCFile> displayInfoDbc;
    if (assetManager) {
        displayInfoDbc = assetManager->loadDBC("ItemDisplayInfo.dbc");
    }
    const auto* idiL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    const uint32_t geosetGroup1Field = idiL ? (*idiL)["GeosetGroup1"] : 7;
    const uint32_t geosetGroup3Field = idiL ? (*idiL)["GeosetGroup3"] : 9;

    auto getGeosetGroup = [&](uint32_t displayInfoId, uint32_t fieldIdx) -> uint32_t {
        if (!displayInfoDbc || displayInfoId == 0) return 0;
        int32_t recIdx = displayInfoDbc->findRecordById(displayInfoId);
        if (recIdx < 0) return 0;
        return displayInfoDbc->getUInt32(static_cast<uint32_t>(recIdx), fieldIdx);
    };

    // Helper: find first equipped item matching inventoryType, return its displayInfoId
    auto findEquippedDisplayId = [&](std::initializer_list<uint8_t> types) -> uint32_t {
        for (int s = 0; s < game::Inventory::NUM_EQUIP_SLOTS; s++) {
            const auto& slot = inventory.getEquipSlot(static_cast<game::EquipSlot>(s));
            if (!slot.empty()) {
                for (uint8_t t : types) {
                    if (slot.item.inventoryType == t)
                        return slot.item.displayInfoId;
                }
            }
        }
        return 0;
    };

    // Helper: check if any equipment slot has the given inventoryType
    auto hasEquippedType = [&](std::initializer_list<uint8_t> types) -> bool {
        for (int s = 0; s < game::Inventory::NUM_EQUIP_SLOTS; s++) {
            const auto& slot = inventory.getEquipSlot(static_cast<game::EquipSlot>(s));
            if (!slot.empty()) {
                for (uint8_t t : types) {
                    if (slot.item.inventoryType == t) return true;
                }
            }
        }
        return false;
    };

    std::unordered_set<uint16_t> geosets;
    if (appearanceComposer_) {
        if (auto* gh = app.getGameHandler()) {
            if (const auto* ch = gh->getActiveCharacter()) {
                const uint8_t raceId = static_cast<uint8_t>(ch->race);
                const uint8_t sexId = static_cast<uint8_t>(ch->gender);
                const uint8_t hairStyleId = static_cast<uint8_t>((ch->appearanceBytes >> 16) & 0xFF);
                const uint8_t facialId = ch->facialFeatures;
                geosets = appearanceComposer_->buildDefaultPlayerGeosets(raceId, sexId, hairStyleId, facialId);
            }
        }
    }
    if (geosets.empty()) {
        // The same bare set the character itself is built from, rather than a
        // shorter one written out here. This listed six ids and the real one
        // names twelve: no bare forearms, shins, sleeves, kneepads or pants,
        // and only one of the two feet spellings - so a portrait that fell
        // back to it drew a body missing the parts those groups carry. The
        // builder's own comment records the portrait and the player
        // disagreeing this way once before.
        //
        // Zeros for the hair and facial variants: this branch is reached when
        // there is no character to read them from, and a zero variant adds
        // nothing rather than guessing one.
        geosets = core::bareCharacterGeosets(0, 0, 0, 0, 0);
    }

    auto eraseGroup = [&](uint16_t group) {
        for (auto it = geosets.begin(); it != geosets.end();) {
            if ((*it / 100) == group) it = geosets.erase(it);
            else ++it;
        }
    };

    // Build set of geoset IDs present in the model for validation.
    // Races like Gnome (no 501) and Tauren (only 505) need fallback.
    std::unordered_set<uint16_t> modelGeosets;
    if (const auto* modelData = charRenderer->getInstanceModelData(instanceId)) {
        for (const auto& batch : modelData->batches) {
            modelGeosets.insert(batch.submeshId);
        }
    }

    // The same rule the player, NPC and portrait paths use, from
    // core::geoset_rules - ask for a variant, get the one this model has.
    auto pickGeoset = [&](uint16_t preferred) {
        return core::resolveGeoset(preferred, modelGeosets);
    };

    eraseGroup(4);
    eraseGroup(5);
    eraseGroup(8);
    eraseGroup(13);
    eraseGroup(15);
    eraseGroup(12);

    // CharGeosets mapping (verified via vertex bounding boxes):
    //   Group 4 (401+) = GLOVES (forearm area, Z~1.1-1.4)
    //   Group 5 (501+) = BOOTS  (shin area, Z~0.1-0.6)
    //   Group 8 (801+) = WRISTBANDS/SLEEVES (controlled by chest armor)
    //   Group 9 (901+) = KNEEPADS
    //   Group 13 (1301+) = TROUSERS/PANTS
    //   Group 15 (1501+) = CAPE/CLOAK
    //   Group 20 (2002) = FEET

    // Gloves: inventoryType 10 → group 4 (forearms)
    {
        uint32_t did = findEquippedDisplayId({10});
        uint32_t gg = getGeosetGroup(did, geosetGroup1Field);
        geosets.insert(pickGeoset(gg > 0 ? core::equippedGeoset(core::equipment::kGlovesBare, gg)
                                          : core::kGeosetBareForearms));
    }

    // Boots: inventoryType 8 → group 5 (shins/lower legs)
    {
        uint32_t did = findEquippedDisplayId({8});
        uint32_t gg = getGeosetGroup(did, geosetGroup1Field);
        uint16_t selectedShin = pickGeoset(gg > 0 ? core::equippedGeoset(core::equipment::kBootsBare, gg)
                                                  : core::kGeosetBareShins);
        geosets.insert(selectedShin);
    }

    // Chest/Shirt: inventoryType 4 (shirt), 5 (chest), 20 (robe)
    // Controls group 8 (wristbands/sleeve length): 801=bare wrists, 802+=sleeve styles
    // Also controls group 13 (trousers) via GeosetGroup[2] for robes
    {
        // The chest or robe wins over the shirt under it: a shirt sits in an
        // earlier slot and was found first, so robes never set the skirt.
        uint32_t did = findEquippedDisplayId({5, 20});
        if (did == 0) did = findEquippedDisplayId({4});
        uint32_t gg = getGeosetGroup(did, geosetGroup1Field);
        geosets.insert(gg > 0 ? core::equippedGeoset(core::equipment::kChestBare, gg)
                              : core::kGeosetBareSleeves);
        uint32_t gg3 = getGeosetGroup(did, geosetGroup3Field);
        if (gg3 > 0) {
            geosets.insert(core::equippedGeoset(core::equipment::kRobeKiltBare, gg3));
        }
        LOG_INFO("Player geosets: chest display ", did, " skirt group ", gg3,
                 " model has 1302=", modelGeosets.count(1302));
    }

    // Kneepads: group 9 from the legs item's GeosetGroup[1] (902, 903); none
    // otherwise. A robe or dress skirt covers them.
    {
        const uint32_t legsDid = findEquippedDisplayId({7});
        const uint32_t kneeGG = getGeosetGroup(legsDid, geosetGroup3Field - 1);
        const uint32_t chestDid = findEquippedDisplayId({5, 20});
        const bool skirt = getGeosetGroup(chestDid, geosetGroup3Field) > 0 ||
                           getGeosetGroup(legsDid, geosetGroup3Field) > 0;
        if (kneeGG > 0 && !skirt) geosets.insert(pickGeoset(static_cast<uint16_t>(901 + kneeGG)));
        if (skirt) {
            // Boot tops would poke through the skirt: bare shins under it.
            for (auto it = geosets.begin(); it != geosets.end();) {
                if (*it / 100 == 5) it = geosets.erase(it); else ++it;
            }
            geosets.insert(pickGeoset(core::kGeosetBareShins));
        }
    }

    // Legs/Pants: inventoryType 7 → group 13 (trousers/thighs)
    // 1301=bare legs, 1302+=pant/kilt styles
    {
        uint32_t did = findEquippedDisplayId({7});
        uint32_t gg = getGeosetGroup(did, geosetGroup1Field);
        // Only add if robe hasn't already set a kilt geoset
        // Only when the robe above has not already put a kilt on the legs.
        // 1302 and 1303 are the first two kilt variants; anything in group 13
        // beyond the bare one means something is already covering them.
        const bool kiltAlreadySet = std::any_of(
            geosets.begin(), geosets.end(), [](uint16_t g) {
                return core::geosetGroup(g) == core::geosetGroup(core::kGeosetBarePants) &&
                       !core::geosetMeansNone(g);
            });
        if (!kiltAlreadySet) {
            // Legs GeosetGroup[2] is a dress or kilt; [0] is group 11 pant
            // flaps and never the skirt.
            const uint32_t legsGG3 = getGeosetGroup(did, geosetGroup3Field);
            geosets.insert(legsGG3 > 0 ? core::equippedGeoset(core::equipment::kRobeKiltBare, legsGG3)
                                       : core::kGeosetBarePants);
        }
        (void)gg;
    }

    // Whether a helm and a cloak are *shown*, which is not the same question as
    // whether one is worn. Both toggles hide the model and neither was reaching
    // the geosets, so turning the cloak off left the cloak's own body geoset on
    // and turning the helm off left the player bald under nothing.
    bool helmShown = true, cloakShown = true;
    if (auto* gh = app.getGameHandler()) {
        helmShown = gh->isHelmVisible();
        cloakShown = gh->isCloakVisible();
    }

    // Back/Cloak: inventoryType 16 → group 15
    uint32_t capeGG = getGeosetGroup(findEquippedDisplayId({16}), geosetGroup1Field);
    geosets.insert((hasEquippedType({16}) && cloakShown) ? core::cloakGeoset(capeGG)
                                                         : core::kGeosetNoCape);

    // Tabard: inventoryType 19 → group 12
    if (hasEquippedType({19})) {
        geosets.insert(core::kGeosetDefaultTabard);
    }

    // Hide hair under a helm: drop the style scalp and put the bald cap on.
    // Group 0 holds the body plus one scalp; 0 itself is the body and stays.
    // The other-player path has always done this, so a helm covered their hair
    // and not yours.
    // Only while it is actually on the head. The show-helm toggle takes the
    // model away and left this branch running, so hiding a helm hid the hair
    // with it and the player stood there bald wearing nothing.
    if (hasEquippedType({1}) && helmShown) {
        const uint32_t headDisplayId = findEquippedDisplayId({1});
        uint8_t genderId = 0;
        if (auto* gh = app.getGameHandler()) {
            if (const auto* ch = gh->getActiveCharacter()) {
                genderId = static_cast<uint8_t>(ch->gender);
            }
        }
        // A circlet, tiara or crown sits over the hair rather than covering it,
        // and the data says which does what - see core::helmHidesHair.
        if (auto* assets = app.getAssetManager();
            assets && core::helmHidesHair(*assets, headDisplayId, genderId)) {
            for (auto it = geosets.begin(); it != geosets.end();) {
                if (*it != 0 && (*it / 100) == 0) it = geosets.erase(it);
                else ++it;
            }
            geosets.insert(1);
        }
    }

    // Bald with nothing on your head, which is the reported bug and the one
    // thing this function can say for certain about it.
    //
    // Group 0 holds the body (geoset 0) and one scalp. Wearing a helm that
    // covers hair replaces the style scalp with 1, the bald cap - that is the
    // branch above. With no helm equipped there is nothing to do that, so a
    // set whose only group-0 members are 0 and 1 means the scalp was never
    // selected rather than removed, and the fault is upstream in
    // buildDefaultPlayerGeosets or the hair map it reads.
    //
    // Silent whenever it is working: it can only fire when the character is
    // bare-headed and bald. The measurement that ruled out the DBC is at
    // entity_spawner.cpp, and what remains unproven is which of the two
    // possible answers this is.
    if (!hasEquippedType({1})) {
        bool hasStyleScalp = false;
        for (uint16_t g : geosets) {
            if (g / 100 == 0 && g != 0 && g != 1) { hasStyleScalp = true; break; }
        }
        if (!hasStyleScalp && geosets.count(1) > 0) {
            LOG_WARNING("Player geosets carry the bald cap with no helm equipped - "
                        "the hair scalp was never selected. Hair style byte and "
                        "CharHairGeosets lookup are the two places to look.");
        }
    }

    // Groups 17 and 18 are the Death Knight / Night Elf eye glow. Nothing here
    // should ever select one, and the renderer only auto-skips them when no
    // geoset filter is applied - with a filter, whatever is in this set is what
    // gets drawn. Glowing eyes on a character that should not have them come
    // from exactly this, so say so rather than leaving it to be spotted.
    for (uint16_t g : geosets) {
        const uint16_t group = g / 100;
        if (group == 17 || group == 18) {
            LOG_WARNING("Player geosets include eye-glow geoset ", g,
                        " (group ", group, ") - this will draw glowing eyes");
        }
    }

    charRenderer->setActiveGeosets(instanceId, geosets);
}

void GameScreen::updateCharacterTextures(game::Inventory& inventory) {
    auto& app = core::Application::getInstance();
    auto* renderer = app.getRenderer();
    if (!renderer) return;

    auto* charRenderer = renderer->getCharacterRenderer();
    if (!charRenderer) return;

    auto* assetManager = app.getAssetManager();
    if (!assetManager) return;

    const auto& bodySkinPath = app.getBodySkinPath();
    const auto& underwearPaths = app.getUnderwearPaths();
    uint32_t skinSlot = app.getSkinTextureSlotIndex();

    if (bodySkinPath.empty()) return;

    // Component directory names indexed by region

    // Load ItemDisplayInfo.dbc
    auto displayInfoDbc = assetManager->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) return;
    const auto* idiL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    uint32_t texRegionFields[8];
    pipeline::getItemDisplayInfoTextureFields(*displayInfoDbc, idiL, texRegionFields);

    // Collect equipment texture regions from all equipped items
    std::vector<std::pair<int, std::string>> regionLayers;
    std::vector<std::pair<int, std::string>> robeLegs;  // drawn last

    for (int s = 0; s < game::Inventory::NUM_EQUIP_SLOTS; s++) {
        const auto& slot = inventory.getEquipSlot(static_cast<game::EquipSlot>(s));
        if (slot.empty() || slot.item.displayInfoId == 0) continue;

        int32_t recIdx = displayInfoDbc->findRecordById(slot.item.displayInfoId);
        if (recIdx < 0) continue;
        const bool robeRec = pipeline::isRobeDisplay(*displayInfoDbc, idiL, recIdx);

        for (int region = 0; region < 8; region++) {
            std::string texName = displayInfoDbc->getString(
                static_cast<uint32_t>(recIdx), texRegionFields[region]);
            if (texName.empty()) continue;

            // Which of _M, _F, _U exists is not recorded anywhere, so the
            // order they are asked in is the rule - pipeline/item_textures.hpp.
            bool isFemale = false;
            if (auto* gh = app.getGameHandler()) {
                if (auto* ch = gh->getActiveCharacter()) {
                    isFemale = (ch->gender == game::Gender::FEMALE) ||
                               (ch->gender == game::Gender::NONBINARY && ch->useFemaleModel);
                }
            }
            const std::string fullPath = pipeline::resolveItemRegionTexture(
                *assetManager, region, texName, isFemale);
            if (fullPath.empty()) continue;
            if (robeRec && pipeline::isRobeLegRegion(region)) robeLegs.emplace_back(region, fullPath);
            else regionLayers.emplace_back(region, fullPath);
        }
    }
    regionLayers.insert(regionLayers.end(), robeLegs.begin(), robeLegs.end());

    // Re-composite: base skin + underwear + equipment regions
    // Clear composite cache first to prevent stale textures from being reused
    charRenderer->clearCompositeCache();
    // Use per-instance texture override (not model-level) to avoid deleting cached composites.
    uint32_t instanceId = renderer->getNativeCharacterInstanceId();
    auto* newTex = charRenderer->compositeWithRegions(bodySkinPath, underwearPaths, regionLayers);
    if (newTex != nullptr && instanceId != 0) {
        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(skinSlot), newTex);
    }

    // Cloak cape texture - separate from skin atlas, uses texture slot type-2 (Object Skin)
    uint32_t cloakSlot = app.getCloakTextureSlotIndex();
    if (cloakSlot > 0 && instanceId != 0) {
        // Find equipped cloak (inventoryType 16)
        uint32_t cloakDisplayId = 0;
        for (int s = 0; s < game::Inventory::NUM_EQUIP_SLOTS; s++) {
            const auto& slot = inventory.getEquipSlot(static_cast<game::EquipSlot>(s));
            if (!slot.empty() && slot.item.inventoryType == 16 && slot.item.displayInfoId != 0) {
                cloakDisplayId = slot.item.displayInfoId;
                break;
            }
        }

        if (cloakDisplayId > 0) {
            int32_t recIdx = displayInfoDbc->findRecordById(cloakDisplayId);
            if (recIdx >= 0) {
                // DBC field 3 = modelTexture_1 (cape texture name)
                const auto* dispL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
                std::string capeName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), dispL ? (*dispL)["LeftModelTexture"] : 3);
                if (!capeName.empty()) {
                    // This asked for one path only - ObjectComponents, no
                    // suffix - and a cape whose art is filed anywhere else
                    // showed white. The full list, in order, is in
                    // pipeline/item_textures.hpp, and the other three places
                    // that load a cape have always used all of it.
                    bool isFemale = false;
                    if (auto* gh = app.getGameHandler()) {
                        if (auto* ch = gh->getActiveCharacter()) {
                            isFemale = (ch->gender == game::Gender::FEMALE) ||
                                       (ch->gender == game::Gender::NONBINARY && ch->useFemaleModel);
                        }
                    }
                    const rendering::VkTexture* whiteTex = charRenderer->loadTexture("");
                    for (const auto& capePath : pipeline::capeTextureCandidates(capeName, isFemale)) {
                        auto* capeTex = charRenderer->loadTexture(capePath);
                        if (capeTex == nullptr || capeTex == whiteTex) continue;
                        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(cloakSlot), capeTex);
                        LOG_INFO("Cloak texture applied: ", capePath);
                        break;
                    }
                }
            }
        } else {
            // No cloak equipped - clear override so model's default (white) shows
            charRenderer->clearTextureSlotOverride(instanceId, static_cast<uint16_t>(cloakSlot));
        }
    }
}

// ============================================================
// World Map
// ============================================================

// Everything the map shows besides the land: the zone the player is in, what
// they have explored, their party, flight points, quests, their corpse and the
// rares nearby. For the in-game map and the second-window one alike, which is
// why it is on its own - the second window's map is fed every frame it is open,
// whatever the in-game one is doing.
void GameScreen::feedWorldMap(game::GameHandler& gameHandler,
                              rendering::world_map::WorldMapFacade& targetMap,
                              const std::function<bool(uint32_t)>& questAreaShown) {
    auto* renderer = core::Application::getInstance().getRenderer();
    if (!renderer) return;
    auto* wm = &targetMap;

    // Keep map name in sync with minimap's map name
    auto* minimap = renderer->getMinimap();
    if (minimap) {
        wm->setMapName(minimap->getMapName());
    }
    wm->setServerExplorationMask(
        gameHandler.getPlayerExploredZoneMasks(),
        gameHandler.hasPlayerExploredZoneMasks());
    // Which zone the player is actually in, rather than which WorldMapArea box
    // they happen to sit deepest inside. The boxes are axis-aligned rectangles
    // around irregular zones and overlap their neighbours heavily, so opening
    // the map could land on a zone the player was only near.
    wm->setPlayerZoneId(gameHandler.getWorldStateZoneId());

    // Party member dots on world map
    {
        std::vector<rendering::WorldMapPartyDot> dots;
        if (gameHandler.isInGroup()) {
            const auto& partyData = gameHandler.getPartyData();
            for (const auto& member : partyData.members) {
                if (!member.isOnline || !member.hasPartyStats) continue;
                if (member.posX == 0 && member.posY == 0) continue;
                // posY → canonical X (north), posX → canonical Y (west)
                float wowX = static_cast<float>(member.posY);
                float wowY = static_cast<float>(member.posX);
                glm::vec3 rpos = core::coords::canonicalToRender(glm::vec3(wowX, wowY, 0.0f));
                auto ent = gameHandler.getEntityManager().getEntity(member.guid);
                uint8_t cid = entityClassId(ent.get());
                ImU32 col = (cid != 0)
                    ? classColorU32(cid, 230)
                    : (member.guid == partyData.leaderGuid
                       ? IM_COL32(255, 210, 0, 230)
                       : IM_COL32(100, 180, 255, 230));
                dots.push_back({ .renderPos = rpos, .color = col, .name = member.name });
            }
        }
        // Battleground team positions, which this client had only ever drawn
        // on the minimap.
        //
        // FrameXML draws them on its own world map - WorldMapRaid1..40, placed
        // from GetNumBattlefieldPositions and GetBattlefieldPosition - but that
        // is exactly the area this client's map surface covers, so nothing
        // FrameXML puts there can be seen. Handing the map over made the
        // interface responsible for a layer it cannot show, so the surface
        // that hides it has to draw them instead.
        //
        // The same two group colours the minimap uses, so a flag carrier is
        // the same colour on both.
        {
            for (const auto& bp : gameHandler.getBgPlayerPositions()) {
                // Packet coords are canonical: wowX north, wowY west.
                const glm::vec3 rpos =
                    core::coords::canonicalToRender(glm::vec3(bp.wowX, bp.wowY, 0.0f));
                dots.push_back({ .renderPos = rpos, .color = ui::bgGroupColor(bp.group),
                                 .name = gameHandler.lookupName(bp.guid) });
            }
        }
        wm->setPartyDots(std::move(dots));
    }

    // Taxi node markers on world map
    {
        std::vector<rendering::WorldMapTaxiNode> taxiNodes;
        const auto& nodes = gameHandler.getTaxiNodes();
        uint32_t currentTaxiNode = gameHandler.getTaxiCurrentNode();
        const bool playerAlliance = gameHandler.isPlayerAlliance();
        taxiNodes.reserve(nodes.size());
        for (const auto& [id, node] : nodes) {
            const bool known = gameHandler.isKnownTaxiNode(id);
            // Undiscovered nodes are shown so the player can see where flight
            // paths exist, but only ones their faction can actually use. A node's
            // faction is inferred from which taxi mount TaxiNodes.dbc lists:
            // own-faction mount → show; both mounts → neutral flight point, show;
            // opposite-faction-only OR no mount at all (boat/zeppelin/script
            // nodes) → hide. Known nodes are always shown.
            if (!known) {
                const bool hasAlliance = node.mountDisplayIdAlliance != 0;
                const bool hasHorde    = node.mountDisplayIdHorde != 0;
                const bool bothFactions = hasAlliance && hasHorde;   // neutral hub
                const bool ownFaction   = playerAlliance ? hasAlliance : hasHorde;
                if (!bothFactions && !ownFaction) continue;
            }
            rendering::WorldMapTaxiNode wtn;
            wtn.id    = node.id;
            wtn.mapId = node.mapId;
            // TaxiNodes.dbc stores server/wire-order coordinates - convert to
            // canonical (X=north, Y=west) like the taxi flight path code does,
            // or the markers land transposed on the map.
            glm::vec3 canonical = core::coords::serverToCanonical(
                glm::vec3(node.x, node.y, node.z));
            wtn.wowX  = canonical.x;
            wtn.wowY  = canonical.y;
            wtn.wowZ  = canonical.z;
            wtn.name  = node.name;
            wtn.known = known;
            wtn.costCopper = gameHandler.getTaxiCostTo(id);
            wtn.current    = (id == currentTaxiNode);
            wtn.reachable  = gameHandler.hasTaxiRouteTo(id);
            taxiNodes.push_back(std::move(wtn));
        }
        wm->setTaxiNodes(std::move(taxiNodes));
    }

    // Quest objective and quest-giver markers on the world map.
    {
        std::vector<rendering::WorldMap::QuestPoi> qpois;
        const auto& questStatuses = gameHandler.getNpcQuestStatuses();
        // Add authoritative NPC statuses first. Some servers also emit a
        // generic POI at the NPC's position; that duplicate is filtered below
        // so it cannot leave a teal objective circle on a quest giver.
        // The stock world map marks no quest givers; ! and ? live on the
        // minimap and over heads only.
        constexpr bool kQuestGiverMapMarkers = false;
        if (kQuestGiverMapMarkers) for (const auto& [guid, status] : questStatuses) {
            auto entity = gameHandler.getEntityManager().getEntity(guid);
            if (!entity || entity->getType() != game::ObjectType::UNIT) continue;

            rendering::WorldMap::QuestPoi qp;
            qp.wowX = entity->getX();
            qp.wowY = entity->getY();
            qp.name = std::static_pointer_cast<game::Unit>(entity)->getName();
            const auto marker = game::questGiverMarker(status);
            if (!marker.symbol) continue;
            if (marker.symbol[0] == '!') {
                qp.kind = marker.dim ? rendering::WorldMap::QuestPoi::Kind::AVAILABLE_LOW
                                     : rendering::WorldMap::QuestPoi::Kind::AVAILABLE;
            } else {
                qp.kind = marker.dim ? rendering::WorldMap::QuestPoi::Kind::INCOMPLETE
                                     : rendering::WorldMap::QuestPoi::Kind::REWARD;
            }
            qpois.push_back(std::move(qp));
        }

        constexpr float kQuestGiverPoiMergeDistance = 15.0f;
        constexpr float kQuestGiverPoiMergeDistanceSq =
            kQuestGiverPoiMergeDistance * kQuestGiverPoiMergeDistance;
        // Which quests the player is actually on. A quest POI outlives the
        // quest that asked for it - the points are kept until the next answer
        // for that quest, and abandoning one never comes with an answer - so
        // without this an abandoned quest's objectives stay on the map.
        std::unordered_set<uint32_t> questsInLog;
        for (const auto& quest : gameHandler.getQuestLog()) {
            if (quest.questId != 0) questsInLog.insert(quest.questId);
        }
        size_t objectivePois = 0;
        for (const auto& poi : gameHandler.getGossipPois()) {
            // Keep ordinary gossip navigation POIs, and quest points for a
            // quest in the log.
            //
            // This asked isQuestShownOnMap, a per-quest opt-in set by a
            // checkbox in the client's own quest tracker. That tracker was
            // handed to FrameXML and the checkbox went with it, leaving three
            // readers of a set nothing could add to: no objective has been
            // drawn on either map since.
            if (poi.questObjectiveIndex != -2 && !questsInLog.count(poi.data)) {
                continue;
            }
            if (poi.questObjectiveIndex >= 0) ++objectivePois;
            bool duplicatesQuestGiver = false;
            for (const auto& existing : qpois) {
                if (existing.kind == rendering::WorldMap::QuestPoi::Kind::OBJECTIVE) continue;
                const float dx = existing.wowX - poi.x;
                const float dy = existing.wowY - poi.y;
                if (dx * dx + dy * dy <= kQuestGiverPoiMergeDistanceSq) {
                    duplicatesQuestGiver = true;
                    break;
                }
            }
            if (duplicatesQuestGiver) continue;

            rendering::WorldMap::QuestPoi qp;
            qp.wowX = poi.x;
            qp.wowY = poi.y;
            qp.name = poi.name;
            // The shaded area, for the quest the map has selected. Every
            // objective of that quest gets its own, which is what the real
            // client shades: DrawQuestBlob names a quest, not an objective.
            if (poi.questObjectiveIndex >= 0 && !poi.area.empty() &&
                questAreaShown(poi.data)) {
                qp.area.reserve(poi.area.size());
                for (const auto& pt : poi.area) qp.area.emplace_back(pt.first, pt.second);
            }
            if (poi.questObjectiveIndex == -1) {
                // A quest POI with no objective index is the quest endpoint,
                // not an objective area. Completed quests use a yellow ?,
                // while in-progress endpoints use a gray ?.
                qp.kind = rendering::WorldMap::QuestPoi::Kind::INCOMPLETE;
                for (const auto& quest : gameHandler.getQuestLog()) {
                    if (quest.questId != poi.data) continue;
                    if (quest.complete) {
                        qp.kind = rendering::WorldMap::QuestPoi::Kind::REWARD;
                    }
                    break;
                }
            }
            qpois.push_back(std::move(qp));
        }
        // Said once, when the map has quests to show objectives for and no
        // points to show: the server answers CMSG_QUEST_POI_QUERY from its own
        // quest_poi table, and an empty one looks exactly like a client that
        // is not drawing them.
        static bool reportedNoObjectivePois = false;
        if (objectivePois == 0 && !questsInLog.empty() && !reportedNoObjectivePois) {
            reportedNoObjectivePois = true;
            LOG_WARNING("World map: ", questsInLog.size(), " quest(s) in the log and no "
                        "objective points for any of them - the server sent no quest POI "
                        "data, so only quest givers and endpoints can be drawn");
        }
        wm->setQuestPois(std::move(qpois));
    }

    // Corpse marker: show skull X on world map when ghost with unclaimed corpse
    {
        float corpseCanX = 0.0f, corpseCanY = 0.0f;
        bool ghostWithCorpse = gameHandler.isPlayerGhost() &&
                               gameHandler.getCorpseCanonicalPos(corpseCanX, corpseCanY);
        glm::vec3 corpseRender = ghostWithCorpse
            ? core::coords::canonicalToRender(glm::vec3(corpseCanX, corpseCanY, 0.0f))
            : glm::vec3{};
        wm->setCorpsePos(ghostWithCorpse, corpseRender);

        // And where releasing would put them. Shown while dead either way:
        // before releasing it is the choice being offered, and after it is the
        // place to walk back from.
        uint32_t healerMap = 0;
        glm::vec3 healerCanonical(0.0f);
        const bool haveHealer = gameHandler.isPlayerDead() &&
                                gameHandler.getDeathReleaseLocation(healerMap, healerCanonical) &&
                                healerMap == gameHandler.getCurrentMapId();
        wm->setGraveyardPos(haveHealer,
                            haveHealer ? core::coords::canonicalToRender(healerCanonical)
                                       : glm::vec3{});
    }

    // Rare tracker: mark every spawned rare / rare-elite the client currently has loaded.
    // Entities only exist while near the player, so a marker means that rare is out now.
    // Opt-in via the Interface setting; when off, feed an empty list so markers clear.
    {
        std::vector<rendering::WorldMapRareMark> rares;
        if (settingsPanel_.showRareTracker_)
        for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
            if (!entity || entity->getType() != game::ObjectType::UNIT) continue;
            auto unit = std::static_pointer_cast<game::Unit>(entity);
            const int rank = gameHandler.getCreatureRank(unit->getEntry());
            if (rank != 2 && rank != 4) continue;      // 2 = Rare Elite, 4 = Rare
            if (unit->getHealth() == 0) continue;       // skip dead/looted rares
            rendering::WorldMapRareMark m;
            m.renderPos = core::coords::canonicalToRender(
                glm::vec3(unit->getX(), unit->getY(), unit->getZ()));
            m.name = unit->getName();
            m.rank = rank;
            rares.push_back(std::move(m));
        }
        wm->setRares(std::move(rares));
    }

}

void GameScreen::renderWorldMap(game::GameHandler& gameHandler) {
    auto& app = core::Application::getInstance();
    auto* renderer = app.getRenderer();
    if (!renderer) return;

    // The map on the second window, under its own ImGui context: a setter can
    // free a texture, and that goes back through the context that made it.
    if (auto* mapWindow = app.getMapWindow(); mapWindow && mapWindow->map()) {
        mapWindow->withContext([&] {
            feedWorldMap(gameHandler, *mapWindow->map(), [mapWindow](uint32_t questId) {
                return questId != 0 && questId == mapWindow->selectedQuest();
            });
        });
    }

    auto* wm = renderer->getWorldMap();
    if (!wm) return;

    // Flight master window drives the world map's flight-map (taxi selection)
    // mode: opening SMSG_SHOWTAXINODES opens the map, activating a flight or
    // closing the gossip closes it. A user-dismissed map (Escape / X) closes
    // the flight master window through the onClose handler.
    // Not while FrameXML is drawing the flight map itself. The legacy taxi
    // list a few lines up already stands aside for that element; this mode did
    // not, so talking to a flight master put both on screen at once - TaxiFrame
    // over this client's own map, each with its own set of pins.
    const bool taxiWanted = gameHandler.isTaxiWindowOpen() &&
                            !frameXmlOwns(UiElement::Taxi);
    if (taxiWanted && !wm->isTaxiMapOpen()) {
        auto* gh = &gameHandler;
        wm->openTaxiMap(
            [gh](uint32_t dest) { return gh->getTaxiRouteTo(dest); },
            [gh](uint32_t dest) { gh->activateTaxi(dest); },
            [gh]() { gh->closeTaxi(); });
    } else if (!taxiWanted && wm->isTaxiMapOpen()) {
        wm->closeTaxiMap();
    }

    // Who says the map is wanted depends on who owns it. FrameXML's world map
    // is a frame it shows and hides, and application.cpp gives this one that
    // frame's rect while it is visible - so a rect being set is the same
    // statement as showWorldMap_ is for this client's own window.
    const bool frameXmlDrivesMap = frameXmlOwns(UiElement::WorldMap);
    const bool wanted = frameXmlDrivesMap
        ? (wm->hasFrameRect() || wm->isTaxiMapOpen())
        : (showWorldMap_ || wm->isTaxiMapOpen());
    if (!wanted) return;

    feedWorldMap(gameHandler, *wm, [&gameHandler](uint32_t questId) {
        return gameHandler.isQuestBlobShown(questId);
    });

    glm::vec3 playerPos = renderer->getCharacterPosition();
    float playerYaw = renderer->getCharacterYaw();
    auto* window = app.getWindow();
    int screenW = window ? window->getWidth() : 1280;
    int screenH = window ? window->getHeight() : 720;
    wm->render(playerPos, screenW, screenH, playerYaw);

    // Sync showWorldMap_ if the map closed itself (e.g. ESC key inside the overlay).
    // Only where that flag is what opened it: under FrameXML the frame's own
    // visibility is the state, and clearing this would say nothing.
    if (!frameXmlDrivesMap && !wm->isOpen()) showWorldMap_ = false;
}

// ============================================================
// Action Bar
// ============================================================

VkDescriptorSet GameScreen::getSpellIcon(uint32_t spellId, pipeline::AssetManager* am) {
    if (spellId == 0 || !am) return VK_NULL_HANDLE;

    // Check cache first
    auto cit = spellIconCache_.find(spellId);
    if (cit != spellIconCache_.end()) return cit->second;

    // Lazy-load SpellIcon.dbc and Spell.dbc icon IDs
    if (!spellIconDbLoaded_) {
        spellIconDbLoaded_ = true;

        // Load SpellIcon.dbc: field 0 = ID, field 1 = icon path
        pipeline::loadSpellIconPaths(am, spellIconPaths_);

        // Load Spell.dbc: SpellIconID field
        auto spellDbc = am->loadDBC("Spell.dbc");
        const auto* spellL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("Spell") : nullptr;
        if (spellDbc && spellDbc->isLoaded()) {
            uint32_t fieldCount = spellDbc->getFieldCount();
            // Helper to load icons for a given field layout
            auto tryLoadIcons = [&](uint32_t idField, uint32_t iconField) {
                spellIconIds_.clear();
                if (iconField >= fieldCount) return;
                for (uint32_t i = 0; i < spellDbc->getRecordCount(); i++) {
                    uint32_t id = spellDbc->getUInt32(i, idField);
                    uint32_t iconId = spellDbc->getUInt32(i, iconField);
                    if (id > 0 && iconId > 0) {
                        spellIconIds_[id] = iconId;
                    }
                }
            };

            // Use the active expansion layout when its fields are present in
            // the loaded DBC. TBC/WotLK/Classic place IconID in different
            // columns, so reading the WotLK default for every client leaves
            // action bars and spell UI without icons.
            uint32_t iconField = 133; // WotLK default
            uint32_t idField = 0;
            if (spellL) {
                try {
                    uint32_t layoutId = (*spellL)["ID"];
                    uint32_t layoutIcon = (*spellL)["IconID"];
                    if (layoutId < fieldCount && layoutIcon < fieldCount) {
                        iconField = layoutIcon;
                        idField = layoutId;
                    }
                } catch (...) {}
            }
            tryLoadIcons(idField, iconField);
        }
    }

    // Rate-limit GPU uploads per frame to prevent stalls when many icons are uncached
    // (e.g., first login, after loading screen, or many new auras appearing at once).
    if (!claimUiTextureUpload()) return VK_NULL_HANDLE;  // defer - do NOT cache null here

    // Look up spellId -> SpellIconID -> icon path
    auto iit = spellIconIds_.find(spellId);
    if (iit == spellIconIds_.end()) {
        spellIconCache_[spellId] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    auto pit = spellIconPaths_.find(iit->second);
    if (pit == spellIconPaths_.end()) {
        spellIconCache_[spellId] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    // Path from DBC has no extension - append .blp
    std::string iconPath = pit->second + ".blp";
    // Cached either way, failures included: the HUD asks for this every frame
    // an aura is up, so a missing icon must not be retried each time.
    VkDescriptorSet ds =
        uploadUiTextureFromBlp(am, iconPath, services_.window);
    spellIconCache_[spellId] = ds;
    return ds;
}

// ============================================================
// Cooldown Tracker - floating panel showing all active spell CDs
// ============================================================

// ============================================================
// Quest Objective Tracker (right-side HUD)
// ============================================================

// ============================================================
// Nameplates - world-space health bars projected to screen
// ============================================================

namespace {

/// The stock 3.3.5 nameplate art, uploaded once.
///
/// Every piece is a 128x32 (or smaller) texture laid out against the same
/// origin, the way the client stacks the plate's regions: the border with its
/// bar hole and level box, the cast bar border (plain or shielded) below it,
/// and the glow around the bar.
struct NameplateArt {
    VkDescriptorSet border = VK_NULL_HANDLE;      // Tooltips\Nameplate-Border
    VkDescriptorSet castBorder = VK_NULL_HANDLE;  // Tooltips\Nameplate-CastBar
    VkDescriptorSet castShield = VK_NULL_HANDLE;  // Tooltips\Nameplate-CastBar-Shield
    VkDescriptorSet glow = VK_NULL_HANDLE;        // Tooltips\Nameplate-Glow (additive)
    VkDescriptorSet elite = VK_NULL_HANDLE;       // Tooltips\EliteNameplateIcon
    VkDescriptorSet barFill = VK_NULL_HANDLE;     // TargetingFrame\UI-TargetingFrame-BarFill
    VkDescriptorSet skull = VK_NULL_HANDLE;       // TargetingFrame\UI-TargetingFrame-Skull
    int attempts = 0;
};

/// One plate texture. `luminanceAsAlpha` is for the art the client draws with
/// ADD blending (the glow has no alpha channel, only a bright shape on black):
/// the brightness becomes the coverage, so ordinary blending draws the shape
/// and not a black slab around it.
VkDescriptorSet loadNameplateTexture(pipeline::AssetManager* am, core::Window* window,
                                     const char* path, bool luminanceAsAlpha) {
    if (!am || !window || !window->getVkContext()) return VK_NULL_HANDLE;
    auto bytes = am->readFile(path);
    if (bytes.empty()) {
        LOG_WARNING("Nameplates: missing texture ", path);
        return VK_NULL_HANDLE;
    }
    auto image = pipeline::BLPLoader::load(bytes);
    if (!image.isValid() || image.data.empty()) {
        LOG_WARNING("Nameplates: could not decode ", path);
        return VK_NULL_HANDLE;
    }
    if (luminanceAsAlpha) {
        for (size_t i = 0; i + 3 < image.data.size(); i += 4) {
            const uint8_t m = std::max({image.data[i], image.data[i + 1], image.data[i + 2]});
            image.data[i] = image.data[i + 1] = image.data[i + 2] = 255;
            image.data[i + 3] = m;
        }
    }
    return window->getVkContext()->uploadImGuiTexture(image.data.data(), image.width,
                                                      image.height);
}

NameplateArt& nameplateArt(pipeline::AssetManager* am, core::Window* window) {
    static NameplateArt art;
    // Retried a few times only: before the Vulkan context is up nothing loads,
    // and a file that is not in the data will not appear later.
    if (!art.border && art.attempts < 8 && am && window && window->getVkContext()) {
        ++art.attempts;
        art.border     = loadNameplateTexture(am, window, "Interface\\Tooltips\\Nameplate-Border.blp", false);
        art.castBorder = loadNameplateTexture(am, window, "Interface\\Tooltips\\Nameplate-CastBar.blp", false);
        art.castShield = loadNameplateTexture(am, window, "Interface\\Tooltips\\Nameplate-CastBar-Shield.blp", false);
        art.glow       = loadNameplateTexture(am, window, "Interface\\Tooltips\\Nameplate-Glow.blp", true);
        art.elite      = loadNameplateTexture(am, window, "Interface\\Tooltips\\EliteNameplateIcon.blp", false);
        art.barFill    = loadNameplateTexture(am, window, "Interface\\TargetingFrame\\UI-TargetingFrame-BarFill.blp", false);
        art.skull      = loadNameplateTexture(am, window, "Interface\\TargetingFrame\\UI-TargetingFrame-Skull.blp", false);
    }
    return art;
}

/// Text with a one-pixel black outline, the way the plate's FRIZQT name is
/// drawn ("OUTLINE" font flag).
void addOutlinedText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col,
                     int alpha, const char* text) {
    const ImU32 black = IM_COL32(0, 0, 0, alpha);
    static constexpr float kOff[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (const auto& o : kOff) {
        dl->AddText(font, size, ImVec2(pos.x + o[0], pos.y + o[1]), black, text);
    }
    dl->AddText(font, size, pos, col, text);
}

} // namespace

void GameScreen::renderNameplates(game::GameHandler& gameHandler) {
    if (gameHandler.getState() != game::WorldState::IN_WORLD) return;

    // Reset mouseover each frame; we'll set it below when the cursor is over a nameplate
    gameHandler.setMouseoverGuid(0);

    auto* appRenderer = services_.renderer;
    if (!appRenderer) return;
    rendering::Camera* camera = appRenderer->getCamera();
    if (!camera) return;

    auto* window = services_.window;
    if (!window) return;
    const float screenW = static_cast<float>(window->getWidth());
    const float screenH = static_cast<float>(window->getHeight());

    const glm::mat4 viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();
    const glm::vec3 camPos   = camera->getPosition();
    const uint64_t  playerGuid = gameHandler.getPlayerGuid();
    const uint64_t  targetGuid = gameHandler.getTargetGuid();

    // Raid marks drawn on a plate this frame, one bit per mark. A marked unit
    // with no plate up still shows its mark over its head - see below.
    uint32_t raidMarksOnPlates = 0;

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    static thread_local std::vector<std::shared_ptr<game::Entity>> nameplateEntities;
    glm::vec3 playerCanonical(0.0f);
    if (auto player = gameHandler.getEntityManager().getEntity(playerGuid))
        playerCanonical = glm::vec3(player->getX(), player->getY(), player->getZ());
    gameHandler.getEntityManager().getEntitiesNear(
        playerCanonical.x, playerCanonical.y, 150.0f, nameplateEntities);

    // Allow Nameplate Overlap. Off in the real client, and the boxes already
    // placed this frame are what "off" needs to know: a plate that would land
    // on one of them is lifted until it clears, so two units at the same
    // distance do not print one bar through another.
    //
    // Nearest first, so the plate that gets moved is the one further away. The
    // list is sorted by distance below for that reason and no other.
    const bool allowPlateOverlap =
        addons::storedCVarValue("nameplateAllowOverlap", "0") != "0";
    static thread_local std::vector<ui::PlateBox> placedPlates;
    placedPlates.clear();
    if (!allowPlateOverlap) {
        std::sort(nameplateEntities.begin(), nameplateEntities.end(),
                  [&](const std::shared_ptr<game::Entity>& a,
                      const std::shared_ptr<game::Entity>& b) {
                      if (!a || !b) return a != nullptr;
                      const glm::vec3 da(a->getX() - playerCanonical.x,
                                         a->getY() - playerCanonical.y,
                                         a->getZ() - playerCanonical.z);
                      const glm::vec3 db(b->getX() - playerCanonical.x,
                                         b->getY() - playerCanonical.y,
                                         b->getZ() - playerCanonical.z);
                      return glm::dot(da, da) < glm::dot(db, db);
                  });
    }

    for (const auto& entityPtr : nameplateEntities) {
        if (!entityPtr) continue;
        const uint64_t guid = entityPtr->getGuid();
        if (!entityPtr || guid == playerGuid) continue;

        if (!entityPtr->isUnit()) continue;
        auto* unit = static_cast<game::Unit*>(entityPtr.get());
        if (unit->getMaxHealth() == 0) continue;
        // A trigger carries health and a name like any unit, so without this
        // the invisible ones drew a bar over empty ground.
        if (unit->getUnitFlags() & game::UNIT_FLAG_NOT_SELECTABLE) continue;

        bool isPlayer = (entityPtr->getType() == game::ObjectType::PLAYER);
        bool isTarget = (guid == targetGuid);
        const bool isHostile = unit->isHostile();

        // Totems are their own row in both the Names and the Nameplates panel,
        // and creature type 11 is the whole of what this client can tell them
        // by. The pet and guardian rows next to them stay unread on purpose:
        // separating those needs to know who summoned a unit, and no
        // expansion's update fields here carry SUMMONEDBY or CREATEDBY.
        //
        // An entry whose creature info has not arrived yet answers 0, which is
        // not 11, so an unknown unit keeps its plate rather than losing it.
        const bool isTotem =
            !isPlayer && gameHandler.getCreatureType(unit->getEntry()) == 11;

        // Pets and guardians, which the panels separate and which separate the
        // same way the game does: both are summons carrying a summoner, and
        // only the one a player steers is a pet. A totem is neither - it has a
        // summoner too, and its own rows above.
        //
        // A unit nobody summoned, or an expansion whose table has no entry for
        // the field, reads as neither and keeps its plate.
        const bool isSummon = !isPlayer && !isTotem &&
                              game::unitSummonedByGuid(*entityPtr) != 0;
        const bool isPet = isSummon &&
            (unit->getUnitFlags() & game::UNIT_FLAG_PLAYER_CONTROLLED) != 0;
        const bool isGuardian = isSummon && !isPet;

        // Shift-V (nameplateShowFriends) owns every friendly unit, players and
        // NPCs alike; V (nameplateShowEnemies) owns the rest - hostile,
        // unfriendly and neutral. As in the stock client, the target gets no
        // plate of its own when its kind is toggled off: the target frame is
        // what says what is selected.
        const int reaction = isPlayer ? (isHostile ? 2 : 5)
                                      : gameHandler.unitReactionToPlayer(*unit);
        const bool isFriendly = !isHostile && reaction >= 5;
        if (isFriendly && !settingsPanel_.showFriendlyNameplates_) continue;
        if (!isFriendly && !settingsPanel_.showEnemyNameplates_) continue;

        // Totems answer to their own two settings on top of the above, which
        // is how the real client has it: the defaults show an enemy's totems
        // and not your own side's, so a field of friendly totems does not bury
        // the fight. The target keeps its plate either way.
        if (isTotem && !isTarget) {
            const char* totemPlateCVar = isHostile ? "nameplateShowEnemyTotems"
                                                   : "nameplateShowFriendlyTotems";
            if (addons::storedCVarValue(totemPlateCVar, isHostile ? "1" : "0") == "0") continue;
        }

        // The same question for the other two summon categories. Every one of
        // these four is on in the real client; they are here to be turned off
        // by someone who does not want a warlock's minions in the way.
        if ((isPet || isGuardian) && !isTarget) {
            const char* platecVar =
                isPet ? (isHostile ? "nameplateShowEnemyPets" : "nameplateShowFriendlyPets")
                      : (isHostile ? "nameplateShowEnemyGuardians"
                                   : "nameplateShowFriendlyGuardians");
            if (addons::storedCVarValue(platecVar, "1") == "0") continue;
        }

        // The dead have no plate in the stock client, selected or not.
        if (unit->getHealth() == 0) continue;

        // Prefer the renderer's actual instance position so the nameplate tracks the
        // rendered model exactly (avoids drift from the parallel entity interpolator).
        glm::vec3 renderPos;
        if (!core::Application::getInstance().getRenderPositionForGuid(guid, renderPos)) {
            renderPos = core::coords::canonicalToRender(
                glm::vec3(unit->getX(), unit->getY(), unit->getZ()));
        }
        renderPos.z += 2.3f;

        // The stock client's plate range: 41 yards from the character (not
        // from the camera, so zooming out does not take plates away), the
        // same for every kind of unit.
        const glm::vec3 plateDelta(unit->getX() - playerCanonical.x,
                                   unit->getY() - playerCanonical.y,
                                   unit->getZ() - playerCanonical.z);
        const float distSq = glm::dot(plateDelta, plateDelta);
        constexpr float cullDist = 41.0f;
        if (distSq > cullDist * cullDist) continue;

        // Behind a building is out of sight, and a name that reads through a
        // wall is worse than no name: it puts a player in a room they are not
        // in. Distance and the frustum were the only tests here, so every
        // nameplate drew over whatever stood in front of it.
        //
        // Cached per unit, because this is a collision query and there can be
        // a dozen plates up. Recomputed when either end has moved far enough
        // to change the answer, and every so often regardless, since the
        // geometry between them streams in and out - the same treatment the
        // selection circle gives its floor query.
        static SightCache sight;
        if (sight.blocked(services_.renderer ? services_.renderer->getWMORenderer() : nullptr,
                          guid, camPos, renderPos)) {
            continue;
        }

        // Project to clip space
        glm::vec4 clipPos = viewProj * glm::vec4(renderPos, 1.0f);
        if (clipPos.w <= 0.01f) continue;  // Behind camera

        glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
        if (ndc.x < -1.2f || ndc.x > 1.2f || ndc.y < -1.2f || ndc.y > 1.2f) continue;

        // NDC → screen pixels.
        // The camera bakes the Vulkan Y-flip into the projection matrix, so
        // NDC y = -1 is the top of the screen and y = 1 is the bottom.
        // Map directly: sy = (ndc.y + 1) / 2 * screenH  (no extra inversion).
        float sx = (ndc.x * 0.5f + 0.5f) * screenW;
        float sy = (ndc.y * 0.5f + 0.5f) * screenH;

        // The stock 3.3.5 plate, region for region. Everything below is laid
        // out in the 128x32 texel space of Nameplate-Border.blp, which is the
        // plate's own size in interface units:
        //   health bar hole  x 5..104, y 20..26
        //   level box        x 106..123, y 18..29 (centre 114.5, 23.5)
        //   name             above the border, centred on the bar
        // The cast bar art (Nameplate-CastBar / -Shield) sits under it, shifted
        // 17 texels left so its bar hole (x 22..121) lines up with the health
        // bar's, which puts its spell icon box just left of the plate.
        const NameplateArt& art = nameplateArt(services_.assetManager, services_.window);

        // Fade over the last five yards. With a target, every other plate is
        // drawn at half strength: that is how the stock client marks the
        // target's plate (the addons of the era found it by its alpha of 1).
        const float dist = std::sqrt(distSq);
        float alpha = dist < cullDist - 5.0f
            ? 1.0f : std::clamp(1.0f - (dist - (cullDist - 5.0f)) / 5.0f, 0.0f, 1.0f);
        if (targetGuid != 0 && !isTarget) alpha *= 0.5f;
        auto A = [&](int v) { return static_cast<int>(static_cast<float>(v) * alpha); };
        const ImU32 white = IM_COL32(255, 255, 255, A(255));

        // Health bar colour, the stock set: red hostile or unfriendly, yellow
        // neutral, green friendly NPC, blue friendly player, grey when someone
        // else has tapped it. Class colours for players are an option.
        int react = reaction;
        if (!isPlayer && gameHandler.isAggressiveTowardPlayer(guid)) react = std::min(react, 2);
        const uint32_t dynFlags = unit->getDynamicFlags();
        // TAPPED but not TAPPED_BY_ALL_THREAT_LIST: someone else's mob.
        const bool tappedByOther = !isPlayer && (dynFlags & 0x0004) != 0 && (dynFlags & 0x0008) == 0;
        const uint8_t classId = isPlayer ? entityClassId(entityPtr.get()) : 0;
        ImU32 barColor;
        if (tappedByOther) {
            barColor = IM_COL32(128, 128, 128, A(255));
        } else if (classId != 0 &&
                   addons::storedCVarValue("showClassColorInNameplate", "0") != "0") {
            barColor = classColorU32(classId, A(255));
        } else if (isPlayer && !isHostile) {
            barColor = IM_COL32(0, 0, 255, A(255));
        } else if (react <= 3) {
            barColor = IM_COL32(255, 0, 0, A(255));
        } else if (react == 4) {
            barColor = IM_COL32(255, 255, 0, A(255));
        } else {
            barColor = IM_COL32(0, 255, 0, A(255));
        }

        // A hostile unit with the player as its target gets the red threat
        // glow, as the stock plate's threat region shows aggro.
        bool isTargetingPlayer = false;
        if (isHostile) {
            const uint64_t unitTarget = game::unitTargetGuid(*entityPtr);
            isTargetingPlayer = unitTarget != 0 && unitTarget == playerGuid;
        }
        const int creatureRank = isPlayer ? -1 : gameHandler.getCreatureRank(unit->getEntry());

        // Plate geometry, in pixels: interface units scale with the window
        // height over the interface's 768, and the panel's plate scale on top.
        const float s = settingsPanel_.nameplateScale_ * screenH / 768.0f;
        const float plateW = 128.0f * s;
        const float plateH = 32.0f * s;
        float px = sx - 55.0f * s;     // the bar's centre over the unit
        float py = sy - plateH;        // the plate's bottom on the point over its head

        // Allow Nameplate Overlap off: lift this plate clear of the nearer
        // ones already placed. The box is the whole plate, name area included.
        if (!allowPlateOverlap) {
            py = ui::plateTopClearOf(placedPlates, px, px + plateW, py, plateH, 0.0f);
            placedPlates.push_back({.x0 = px, .y0 = py, .x1 = px + plateW, .y1 = py + plateH});
        }
        auto P = [&](float tx, float ty) { return ImVec2(px + tx * s, py + ty * s); };

        // Hover, decided before drawing because the highlight is part of the
        // picture. The plate frame is the hit rect, as in the stock client.
        bool hovered = false;
        if (!ImGui::GetIO().WantCaptureMouse) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            hovered = mouse.x >= px && mouse.x <= px + plateW &&
                      mouse.y >= py && mouse.y <= py + plateH;
        }

        // Guard against division by zero when maxHealth hasn't been populated yet.
        const float healthPct = (unit->getMaxHealth() > 0)
            ? std::clamp(static_cast<float>(unit->getHealth()) / static_cast<float>(unit->getMaxHealth()), 0.0f, 1.0f)
            : 0.0f;

        // Glow behind the border: red threat, the target's own glow, and the
        // mouseover highlight. Nameplate-Glow is drawn additive in the client;
        // its brightness was turned into coverage when it was loaded.
        if (art.glow) {
            ImU32 glowColor = 0;
            if (isTargetingPlayer)  glowColor = IM_COL32(255, 0, 0, A(255));
            else if (isTarget)      glowColor = IM_COL32(255, 255, 255, A(230));
            else if (hovered)       glowColor = IM_COL32(255, 255, 255, A(140));
            if (glowColor != 0) {
                drawList->AddImage((ImTextureID)(uintptr_t)art.glow, P(0, 0), P(128, 32),
                                   ImVec2(0, 0), ImVec2(1, 1), glowColor);
            }
        }

        // Health fill: UI-TargetingFrame-BarFill tinted by reaction, cut off
        // at the health fraction rather than squeezed.
        {
            const ImVec2 b0 = P(5, 20);
            const ImVec2 b1 = P(105, 27);
            const float fillX = b0.x + (b1.x - b0.x) * healthPct;
            if (art.barFill) {
                drawList->AddImage((ImTextureID)(uintptr_t)art.barFill, b0, ImVec2(fillX, b1.y),
                                   ImVec2(0, 0), ImVec2(healthPct, 1), barColor);
            } else {
                drawList->AddRectFilled(b0, ImVec2(fillX, b1.y), barColor);
            }
        }

        // The border, with its level box.
        if (art.border) {
            drawList->AddImage((ImTextureID)(uintptr_t)art.border, P(0, 0), P(128, 32),
                               ImVec2(0, 0), ImVec2(1, 1), white);
        } else {
            drawList->AddRect(P(4, 19), P(106, 28), IM_COL32(200, 160, 40, A(255)));
        }

        // Level in the box, or the skull for ?? and bosses. Coloured by
        // difficulty, as GetQuestDifficultyColor does.
        ImFont* plateFont = interfaceFaceOrDefault("Fonts\\FRIZQT__.TTF");
        const uint32_t level = unit->getLevel();
        const uint32_t playerLevel = gameHandler.getPlayerLevel();
        // Stock hides a level only for units you can fight that are ten or
        // more levels above you (UnitLevel returns -1), and for bosses. A
        // friendly guard or player shows his number however high he is.
        const bool skullLevel = creatureRank == 3 || level == 0 ||
                                (isHostile && playerLevel > 0 && level >= playerLevel + 10);
        if (skullLevel) {
            if (art.skull) {
                drawList->AddImage((ImTextureID)(uintptr_t)art.skull, P(106.5f, 15.5f), P(122.5f, 31.5f),
                                   ImVec2(0, 0), ImVec2(1, 1), white);
            }
        } else {
            const int diff = static_cast<int>(level) - static_cast<int>(playerLevel);
            ImU32 levelColor;
            if (diff >= 5)       levelColor = IM_COL32(255, 26, 26, A(255));
            else if (diff >= 3)  levelColor = IM_COL32(255, 128, 64, A(255));
            else if (diff >= -2) levelColor = IM_COL32(255, 209, 0, A(255));
            else if (!addons::questIsTrivial(static_cast<int>(playerLevel), static_cast<int>(level)))
                                 levelColor = IM_COL32(64, 191, 64, A(255));
            else                 levelColor = IM_COL32(128, 128, 128, A(255));
            char levelBuf[8];
            snprintf(levelBuf, sizeof(levelBuf), "%u", level);
            const float levelSize = 9.0f * s;
            const ImVec2 lsz = plateFont->CalcTextSizeA(levelSize, FLT_MAX, 0.0f, levelBuf);
            const ImVec2 c = P(114.5f, 23.5f);
            addOutlinedText(drawList, plateFont, levelSize,
                            ImVec2(std::round(c.x - lsz.x * 0.5f), std::round(c.y - lsz.y * 0.5f)),
                            levelColor, A(200), levelBuf);
        }

        // The elite dragon, curled round the level box.
        if ((creatureRank == 1 || creatureRank == 2 || creatureRank == 3) && art.elite) {
            drawList->AddImage((ImTextureID)(uintptr_t)art.elite, P(102.5f, 11.5f), P(166.5f, 43.5f),
                               ImVec2(0, 0), ImVec2(1, 1), white);
        }

        // Cast bar under the plate: the plain border when it can be
        // interrupted, the shielded one when it cannot, the spell's icon in
        // the box at the left. No text: the stock plate shows none.
        {
            const bool castBarAllowed =
                isTarget || !isHostile ||
                addons::storedCVarValue("showVKeyCastBar", "1") != "0";
            const auto* cs = castBarAllowed ? gameHandler.getUnitCastState(guid) : nullptr;
            if (cs && cs->casting && cs->timeTotal > 0.0f) {
                const float castPct = std::clamp(
                    (cs->timeTotal - cs->timeRemaining) / cs->timeTotal, 0.0f, 1.0f);
                // Cast-bar texel (x, y) in plate space.
                auto C = [&](float tx, float ty) { return P(tx - 17.0f, ty + 26.0f); };
                const ImVec2 c0 = C(22, 12);
                const ImVec2 c1 = C(122, 19);
                const ImU32 castColor = IM_COL32(255, 179, 0, A(255));
                if (art.barFill) {
                    drawList->AddImage((ImTextureID)(uintptr_t)art.barFill, c0,
                                       ImVec2(c0.x + (c1.x - c0.x) * castPct, c1.y),
                                       ImVec2(0, 0), ImVec2(castPct, 1), castColor);
                } else {
                    drawList->AddRectFilled(c0, ImVec2(c0.x + (c1.x - c0.x) * castPct, c1.y), castColor);
                }
                const VkDescriptorSet castArt = cs->interruptible ? art.castBorder : art.castShield;
                if (castArt) {
                    drawList->AddImage((ImTextureID)(uintptr_t)castArt, C(0, 0), C(128, 32),
                                       ImVec2(0, 0), ImVec2(1, 1), white);
                }
                auto* castAm = services_.assetManager;
                const VkDescriptorSet castIcon = (cs->spellId && castAm)
                    ? getSpellIcon(cs->spellId, castAm) : VK_NULL_HANDLE;
                if (castIcon) {
                    const ImVec2 i0 = cs->interruptible ? C(4, 8) : C(5, 9);
                    const ImVec2 i1 = cs->interruptible ? C(17, 21) : C(18, 22);
                    drawList->AddImage((ImTextureID)(uintptr_t)castIcon, i0, i1,
                                       ImVec2(0.07f, 0.07f), ImVec2(0.93f, 0.93f), white);
                }
            }
        }

        // Name above the border: FRIZQT with an outline, white.
        const std::string& unitName = unit->getName();
        if (isPlayer && unitName.empty()) gameHandler.queryPlayerName(unit->getGuid());
        if (!unitName.empty()) {
            const float nameSize = 12.0f * s;
            const ImVec2 nsz = plateFont->CalcTextSizeA(nameSize, FLT_MAX, 0.0f, unitName.c_str());
            const ImVec2 anchor = P(55.0f, 16.0f);
            addOutlinedText(drawList, plateFont, nameSize,
                            ImVec2(std::round(anchor.x - nsz.x * 0.5f), std::round(anchor.y - nsz.y)),
                            white, A(255), unitName.c_str());
        }

        // Raid mark at the left of the plate.
        {
            const uint8_t raidMark = gameHandler.getEntityRaidMark(guid);
            if (raidMark < game::GameHandler::kRaidMarkCount) {
                VkDescriptorSet markTex = ui::getRaidTargetIcon(raidMark, services_.assetManager);
                if (markTex) {
                    drawList->AddImage((ImTextureID)(uintptr_t)markTex, P(-20.0f, 7.0f), P(2.0f, 29.0f),
                                       ImVec2(0, 0), ImVec2(1, 1), white);
                    raidMarksOnPlates |= 1u << raidMark;
                }
            }
        }

        // Click to target, right-click for the unit menu; hover is mouseover.
        if (hovered) {
            gameHandler.setMouseoverGuid(guid);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                gameHandler.setTarget(guid);
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                nameplateCtxGuid_ = guid;
                nameplateCtxPos_  = ImGui::GetIO().MousePos;
                ImGui::OpenPopup("##NameplateCtx");
            }
        }
    }

    // Raid marks over heads with no plate to sit on. The stock client shows a
    // skull over its unit whether or not nameplates are on - and over your own
    // head - but the only place this drew a mark was inside a plate, so with
    // plates toggled off (the default) a marked mob carried nothing at all.
    for (uint32_t mark = 0; mark < game::GameHandler::kRaidMarkCount; ++mark) {
        if (raidMarksOnPlates & (1u << mark)) continue;
        const uint64_t markGuid = gameHandler.getRaidMarkGuid(mark);
        if (markGuid == 0) continue;
        auto markEntity = gameHandler.getEntityManager().getEntity(markGuid);
        if (!markEntity || !markEntity->isUnit()) continue;
        if (static_cast<const game::Unit*>(markEntity.get())->getHealth() == 0) continue;

        glm::vec3 headPos;
        auto& app = core::Application::getInstance();
        if (!app.getRenderPositionForGuid(markGuid, headPos)) {
            headPos = core::coords::canonicalToRender(
                glm::vec3(markEntity->getX(), markEntity->getY(), markEntity->getZ()));
        }
        glm::vec3 boundsCenter;
        float boundsRadius = 0.0f;
        if (app.getRenderBoundsForGuid(markGuid, boundsCenter, boundsRadius)) {
            headPos.z = std::max(headPos.z + 0.5f, boundsCenter.z + boundsRadius);
        } else {
            headPos.z += 2.3f;
        }
        const glm::vec3 toMark = headPos - camPos;
        if (glm::dot(toMark, toMark) > 100.0f * 100.0f) continue;

        const glm::vec4 clip = viewProj * glm::vec4(headPos, 1.0f);
        if (clip.w <= 0.01f) continue;
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.x < -1.1f || ndc.x > 1.1f || ndc.y < -1.1f || ndc.y > 1.1f) continue;
        VkDescriptorSet markTex = ui::getRaidTargetIcon(static_cast<uint8_t>(mark),
                                                        services_.assetManager);
        if (!markTex) continue;
        // The 64 px art drawn at 32, bottom edge on the top of the head.
        constexpr float kMarkSize = 32.0f;
        const float mx = (ndc.x * 0.5f + 0.5f) * screenW;
        const float my = (ndc.y * 0.5f + 0.5f) * screenH;
        drawList->AddImage((ImTextureID)(uintptr_t)markTex,
                           ImVec2(mx - kMarkSize * 0.5f, my - kMarkSize),
                           ImVec2(mx + kMarkSize * 0.5f, my),
                           ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                           IM_COL32(255, 255, 255, 255));
    }

    // My Name. Its own block rather than a case inside the loop above, because
    // what this draws is a name and not a nameplate: no bar, no border, no
    // click target. The loop skips the player outright for that reason, and
    // threading an exception through every draw in it to reach one label would
    // cost more than the label is worth.
    if (addons::storedCVarValue("unitNameOwn", "0") != "0") {
        auto self = gameHandler.getEntityManager().getEntity(playerGuid);
        if (self && self->isUnit()) {
            const std::string& ownName = static_cast<game::Unit*>(self.get())->getName();
            if (!ownName.empty()) {
                glm::vec3 ownPos;
                if (!core::Application::getInstance().getRenderPositionForGuid(playerGuid, ownPos)) {
                    ownPos = core::coords::canonicalToRender(
                        glm::vec3(self->getX(), self->getY(), self->getZ()));
                }
                ownPos.z += 2.3f;
                const glm::vec4 clip = viewProj * glm::vec4(ownPos, 1.0f);
                if (clip.w > 0.01f) {
                    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                    if (ndc.x >= -1.2f && ndc.x <= 1.2f && ndc.y >= -1.2f && ndc.y <= 1.2f) {
                        const float ox = (ndc.x * 0.5f + 0.5f) * screenW;
                        const float oy = (ndc.y * 0.5f + 0.5f) * screenH;
                        const ImVec2 sz = ImGui::CalcTextSize(ownName.c_str());
                        const float tx = ox - sz.x * 0.5f;
                        drawList->AddText(ImVec2(tx + 1.0f, oy + 1.0f),
                                          IM_COL32(0, 0, 0, 160), ownName.c_str());
                        drawList->AddText(ImVec2(tx, oy),
                                          IM_COL32(220, 220, 220, 230), ownName.c_str());
                    }
                }
            }
        }
    }

    // Render nameplate context popup (uses a tiny overlay window as host)
    if (nameplateCtxGuid_ != 0) {
        ImGui::SetNextWindowPos(nameplateCtxPos_, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);
        ImGuiWindowFlags ctxHostFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                         ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing |
                                         ImGuiWindowFlags_AlwaysAutoResize;
        if (ImGui::Begin("##NameplateCtxHost", nullptr, ctxHostFlags)) {
            if (ImGui::BeginPopup("##NameplateCtx")) {
                auto entityPtr = gameHandler.getEntityManager().getEntity(nameplateCtxGuid_);
                std::string ctxName = entityPtr ? game::entityDisplayName(entityPtr) : "";
                if (!ctxName.empty()) {
                    ImGui::TextDisabled("%s", ctxName.c_str());
                    ImGui::Separator();
                }
                if (ImGui::MenuItem("Target"))
                    gameHandler.setTarget(nameplateCtxGuid_);
                if (ImGui::MenuItem("Set Focus"))
                    gameHandler.setFocus(nameplateCtxGuid_);
                bool isPlayer = entityPtr && entityPtr->getType() == game::ObjectType::PLAYER;
                if (isPlayer && !ctxName.empty()) {
                    ImGui::Separator();
                    if (ImGui::MenuItem("Whisper")) {
                        chatPanel_.setWhisperTarget(ctxName);
                    }
                    if (ImGui::MenuItem("Invite to Group"))
                        gameHandler.inviteToGroup(ctxName);
                    if (ImGui::MenuItem("Trade"))
                        gameHandler.initiateTrade(nameplateCtxGuid_);
                    if (ImGui::MenuItem("Duel"))
                        gameHandler.proposeDuel(nameplateCtxGuid_);
                    if (ImGui::MenuItem("Inspect")) {
                        gameHandler.setTarget(nameplateCtxGuid_);
                        gameHandler.inspectTarget();
                        openInspectWindow(gameHandler);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Add Friend"))
                        gameHandler.addFriend(ctxName);
                    if (ImGui::MenuItem("Ignore"))
                        gameHandler.addIgnore(ctxName);
                }
                ImGui::EndPopup();
            } else {
                nameplateCtxGuid_ = 0;
            }
        }
        ImGui::End();
    }
}

// ============================================================
// Durability Warning (equipment damage indicator)
// ============================================================

// The settings panel keeps brightness as 0-100 with 50 neutral, and the post
// process pipeline wants that over 50 - so the number the video options call
// gamma is exactly what the pipeline is already given, and this converts
// between the two rather than introducing a third scale.
float GameScreen::getGamma() const {
    return static_cast<float>(settingsPanel_.pendingBrightness) / 50.0f;
}

void GameScreen::setGamma(float gamma) {
    // WoW's own slider runs 0.3 to 2.8; clamped to what the 0-100 setting can
    // hold so a value from outside cannot push the slider off its own track.
    const float clamped = std::clamp(gamma, 0.0f, 2.0f);
    const int stored = static_cast<int>(std::lround(clamped * 50.0f));
    // Saved here, because nothing else was going to.
    //
    // Every other route into these settings goes through the settings window,
    // which writes the file when it is done. The video options' Gamma slider
    // reaches this directly from Lua instead, so the value applied, looked
    // right for the rest of the session, and was gone the next time the client
    // started - the file had never been written.
    //
    // Only when the stored number actually moves. The slider reports every
    // frame it is dragged, and the setting is a whole number out of a hundred,
    // so this is a handful of writes across a drag rather than one per frame.
    const bool changed = (stored != settingsPanel_.pendingBrightness);
    settingsPanel_.pendingBrightness = stored;
    if (auto* renderer = services_.renderer) {
        renderer->getPostProcessPipeline()->setBrightness(clamped);
    }
    if (changed) saveSettings();
}

namespace {

/// ~/.wowee/<folder>/WoWee_YYYYMMDD_HHMMSS.<extension>, the name a screenshot
/// or a recording is saved under.
std::string capturePath(const char* folder, const char* extension) {
    const char* home = std::getenv("HOME");
    if (!home) home = std::getenv("USERPROFILE");
    if (!home) home = "/tmp";
    std::string dir = std::string(home) + "/.wowee/" + folder;

    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    tm = core::localTime(tt);

    char filename[128];
    std::snprintf(filename, sizeof(filename),
                  "WoWee_%04d%02d%02d_%02d%02d%02d.%s",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, extension);
    return dir + "/" + filename;
}

}  // namespace

void GameScreen::startRecording() {
    auto* renderer = services_.renderer;
    if (!renderer || !services_.gameHandler || renderer->isRecording()) return;
    const std::string path = capturePath("recordings", "mp4");
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::string error;
    if (renderer->startRecording(path, error)) {
        recordingPath_ = path;
        services_.gameHandler->addSystemChatMessage(
            "Recording to " + path + ". Type /record again to stop.");
    } else {
        services_.gameHandler->addSystemChatMessage("Could not start recording: " + error + ".");
    }
}

void GameScreen::stopRecording() {
    auto* renderer = services_.renderer;
    if (!renderer || !services_.gameHandler || !renderer->isRecording()) return;
    const auto stats = renderer->stopRecording();
    const int seconds = static_cast<int>(stats.seconds + 0.5);
    char length[32];
    std::snprintf(length, sizeof(length), "%d:%02d", seconds / 60, seconds % 60);
    std::string message = "Recording saved: " + recordingPath_ + " (" + length;
    if (!stats.hasAudio) message += ", no sound";
    if (stats.framesDropped > 0) {
        message += ", " + std::to_string(stats.framesDropped) + " frames dropped";
    }
    services_.gameHandler->addSystemChatMessage(message + ").");
}

void GameScreen::toggleRecording() {
    auto* renderer = services_.renderer;
    if (!renderer) return;
    if (renderer->isRecording()) {
        stopRecording();
    } else {
        startRecording();
    }
}

void GameScreen::reportRecordingFailure() {
    auto* renderer = services_.renderer;
    if (!renderer || !services_.gameHandler) return;
    const std::string failure = renderer->takeRecordingFailure();
    if (failure.empty()) return;
    services_.gameHandler->addSystemChatMessage(
        "Recording stopped: " + failure + ". What was recorded is saved in " + recordingPath_ + ".");
}

void GameScreen::takeScreenshot() {
    auto* renderer = services_.renderer;
    if (!renderer) return;

    const std::string path = capturePath("screenshots", "png");

    if (renderer->captureScreenshot(path)) {
        game::MessageChatData sysMsg;
        sysMsg.type = game::ChatType::SYSTEM;
        sysMsg.language = game::ChatLanguage::UNIVERSAL;
        sysMsg.message = "Screenshot saved: " + path;
        services_.gameHandler->addLocalChatMessage(sysMsg);
    }
}

}} // namespace wowee::ui
