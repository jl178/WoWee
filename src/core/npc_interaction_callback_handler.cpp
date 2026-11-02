#include "core/npc_interaction_callback_handler.hpp"
#include "core/entity_spawner.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "game/game_handler.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sound_data.hpp"
#include "game/update_field_table.hpp"
#include "game/inventory.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/water_renderer.hpp"
#include "pipeline/m2_loader.hpp"
#include <chrono>
#include <cmath>
#include <optional>
#include <random>

namespace wowee { namespace core {

NPCInteractionCallbackHandler::NPCInteractionCallbackHandler(
    EntitySpawner& entitySpawner,
    rendering::Renderer* renderer,
    game::GameHandler& gameHandler,
    audio::AudioCoordinator* audioCoordinator)
    : entitySpawner_(entitySpawner)
    , renderer_(renderer)
    , gameHandler_(gameHandler)
    , audioCoordinator_(audioCoordinator)
{
}

audio::VoiceType NPCInteractionCallbackHandler::resolveNpcVoiceType(uint64_t guid) const {
    audio::VoiceType voiceType = audio::VoiceType::GENERIC;
    auto entity = gameHandler_.getEntityManager().getEntity(guid);
    if (entity && entity->getType() == game::ObjectType::UNIT) {
        auto unit = std::static_pointer_cast<game::Unit>(entity);
        uint32_t displayId = unit->getDisplayId();
        voiceType = entitySpawner_.detectVoiceTypeFromDisplayId(displayId);
    }
    return voiceType;
}

uint32_t NPCInteractionCallbackHandler::resolveNpcDisplayId(uint64_t guid) const {
    auto entity = gameHandler_.getEntityManager().getEntity(guid);
    if (entity && entity->getType() == game::ObjectType::UNIT) {
        return std::static_pointer_cast<game::Unit>(entity)->getDisplayId();
    }
    return 0;
}

namespace {
double steadyNowMs() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
audio::CombatItem combatItem(const EntitySpawner::ItemDbcRow* row) {
    audio::CombatItem item;
    if (row) {
        item.itemClass = row->itemClass;
        item.subclass = row->subclass;
        item.material = row->material;
        item.inventoryType = row->inventoryType;
    }
    return item;
}
// A unit's emote voice, at the unit: the row's own MinDistance and
// DistanceCutoff, as every positional kit plays (Wow.exe 0x4c6a40 ->
// set3DMinMaxDistance), rather than a fixed 60 yd cutoff that was still
// heard where the row says the stock client has gone silent.
// The local character's own: in the head at 0.65 (0x746d60, 0x747310).
void playEntryAtUnit(const audio::SoundEntry& entry, const std::string& file, const glm::vec3& position,
                     bool own = false) {
    audio::SoundOptions opts;
    opts.minDistance = entry.minDistance;
    opts.kitId = entry.id;
    opts.ownCharacter = own;
    const float cutoff = entry.distanceCutoff > 0.0f ? entry.distanceCutoff : audio::stock::kDefaultCutoff;
    audio::AudioEngine::instance().playSound3D(file, position, entry.volume, 1.0f, cutoff, opts);
}
} // namespace

uint64_t NPCInteractionCallbackHandler::guidForInstance(uint32_t instanceId) {
    if (instanceId == 0) return 0;
    if (renderer_ && instanceId == renderer_->getCharacterInstanceId()) return gameHandler_.getPlayerGuid();
    auto hit = instanceGuids_.find(instanceId);
    if (hit != instanceGuids_.end() && entitySpawner_.characterInstanceIdForGuid(hit->second) == instanceId)
        return hit->second;
    // Rebuilt from the spawner's maps on a miss, at most twice a second:
    // mounts and other instances that are no unit miss every time.
    const double now = steadyNowMs();
    if (now - instanceGuidsBuiltMs_ < 500.0) return 0;
    instanceGuidsBuiltMs_ = now;
    instanceGuids_.clear();
    for (const auto& [guid, id] : entitySpawner_.getCreatureInstances()) instanceGuids_[id] = guid;
    for (const auto& [guid, id] : entitySpawner_.getPlayerInstances()) instanceGuids_[id] = guid;
    hit = instanceGuids_.find(instanceId);
    return hit != instanceGuids_.end() ? hit->second : 0;
}

uint32_t NPCInteractionCallbackHandler::mountDisplayForInstance(uint32_t instanceId) const {
    if (instanceId == 0) return 0;
    if (renderer_) {
        if (auto* ac = renderer_->getAnimationController();
            ac && ac->isMounted() && ac->getMountInstanceId() == instanceId)
            return ac->getMountDisplayId();
    }
    for (const auto& [guid, m] : entitySpawner_.getRemotePlayerMounts())
        if (m.instanceId == instanceId) return m.displayId;
    return 0;
}

bool NPCInteractionCallbackHandler::resolveCombatUnit(uint64_t guid, audio::CombatUnitInfo& out) {
    if (guid == 0) return false;
    const bool isLocal = guid == gameHandler_.getPlayerGuid();
    auto entity = gameHandler_.getEntityManager().getEntity(guid);
    if (entity && !entity->isUnit()) return false;
    if (!entity && !isLocal) return false;
    out = {};
    out.isLocalPlayer = isLocal;
    out.isPlayer = isLocal || entity->getType() == game::ObjectType::PLAYER;
    if (entity) out.displayId = std::static_pointer_cast<game::Unit>(entity)->getDisplayId();

    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    out.instanceId = entitySpawner_.characterInstanceIdForGuid(guid);
    if (!(cr && out.instanceId != 0 && cr->getInstancePosition(out.instanceId, out.position))) {
        if (isLocal && renderer_) out.position = renderer_->getCharacterPosition();
        else if (entity) out.position = core::coords::canonicalToRender(
            glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
        else return false;
    }

    if (isLocal) {
        // The local character's own inventory is the authority on what it holds.
        const auto& inv = gameHandler_.getInventory();
        auto worn = [&](game::EquipSlot slot) -> const EntitySpawner::ItemDbcRow* {
            const auto& s = inv.getEquipSlot(slot);
            return s.empty() ? nullptr : entitySpawner_.lookupItemDbc(s.item.itemId);
        };
        out.mainHand = combatItem(worn(game::EquipSlot::MAIN_HAND));
        out.offHand = combatItem(worn(game::EquipSlot::OFF_HAND));
        if (const auto* chest = worn(game::EquipSlot::CHEST); chest && chest->itemClass == 4)
            out.chestArmorSubclass = chest->subclass;
    } else {
        // Other players from their visible items, creatures from their
        // virtual item slots.
        const auto items = entitySpawner_.unitWeaponItems(guid);
        out.mainHand = combatItem(&items[0]);
        out.offHand = combatItem(&items[1]);
        if (out.isPlayer) {
            auto& table = gameHandler_.otherPlayerVisibleItemEntriesRef();
            if (auto it = table.find(guid); it != table.end()) {
                const auto* chest = entitySpawner_.lookupItemDbc(it->second[static_cast<int>(game::EquipSlot::CHEST)]);
                if (chest && chest->itemClass == 4) out.chestArmorSubclass = chest->subclass;
            }
        }
    }

    if (cr && out.instanceId != 0) {
        // The swing may be on the upper body over a run; its events fire
        // from there too.
        auto hasEvent = [&](uint32_t tag) {
            return cr->currentAnimHasEvent(out.instanceId, tag) || cr->upperBodyAnimHasEvent(out.instanceId, tag);
        };
        out.animHasSwingEvent = hasEvent(pipeline::m2EventTag("$CSS"));
        out.animHasHitEvent = hasEvent(pipeline::m2EventTag("$CAH"));
    }
    return true;
}

void NPCInteractionCallbackHandler::footstepGround(const glm::vec3& p, uint32_t& terrainSoundId,
                                                   bool& inWater) const {
    terrainSoundId = 0;
    inWater = false;
    if (!renderer_) return;
    if (auto* water = renderer_->getWaterRenderer()) {
        if (auto h = water->getWaterHeightAt(p.x, p.y); h && p.z < *h + 0.25f) {
            inWater = true;
            terrainSoundId = 10;  // TerrainType Water
            return;
        }
    }
    auto* terrain = renderer_->getTerrainManager();
    if (auto* wmo = renderer_->getWMORenderer()) {
        const auto floor = wmo->getFloorHeight(p.x, p.y, p.z + 1.5f);
        const auto ground = terrain ? terrain->getHeightAt(p.x, p.y) : std::nullopt;
        if (floor && std::abs(*floor - p.z) <= 2.5f && (!ground || *floor >= *ground - 0.1f)) {
            // A building's floor. Its material's ground type is not read
            // here, so the floor is stone, as the local character's steps
            // have it.
            terrainSoundId = 3;
            return;
        }
    }
    if (terrain) {
        if (auto sound = terrain->getTerrainSoundTypeAt(p.x, p.y)) terrainSoundId = *sound;
        else terrainSoundId = 1;  // a layer with no ground effect: dirt
    }
}

void NPCInteractionCallbackHandler::handleUnitAnimEvent(uint32_t instanceId, uint32_t eventTag,
                                                        const glm::vec3& position) {
    using pipeline::m2EventTag;
    if (meleeEventHook_ && (eventTag == m2EventTag("$CAH") || eventTag == m2EventTag("$CPP")))
        meleeEventHook_(instanceId, eventTag);
    if (eventTag == m2EventTag("$CPP")) return;  // silent
    auto* csm = audioCoordinator_ ? audioCoordinator_->getCombatSoundManager() : nullptr;
    if (!csm) return;
    if (eventTag == m2EventTag("$CSS") || eventTag == m2EventTag("$CAH")) {
        csm->onMeleeAnimEvent(instanceId, eventTag);
        return;
    }
    // Everything below is heard only nearby; the instance lookup and the
    // ground probe are skipped for the rest.
    if (glm::distance(audio::AudioEngine::instance().getListenerPosition(), position) > 60.0f) return;
    // A mount's own model: its events sound with the mount display's
    // CreatureSoundData. The stock client points a mounted unit's creature
    // sounds at the mount (0x73D5D0 stores 0x7195D0's answer at CGUnit
    // +0x978, which 0x71A3F0 prefers): hooves on the ground, wing flaps in
    // the air. The local player's mount included - its footstep driver only
    // leaves the footprints.
    if (const uint32_t mountDisplay = mountDisplayForInstance(instanceId)) {
        using BS2 = audio::CombatSoundManager::BodySound;
        // The local player's mount is the local player's unit: its sounds
        // take the own-unit rule, in the head at 0.65.
        auto* anim = renderer_ ? renderer_->getAnimationController() : nullptr;
        const bool ownMount = anim && anim->isMounted() && anim->getMountInstanceId() == instanceId;
        if (eventTag == m2EventTag("$FSD")) {
            uint32_t ground = 0;
            bool inWater = false;
            footstepGround(position, ground, inWater);
            auto* fsm = audioCoordinator_ ? audioCoordinator_->getFootstepManager() : nullptr;
            csm->playFootstep(mountDisplay, ground, inWater, position, ownMount,
                              fsm ? fsm->getVolumeScale() : 1.0f);
        } else if (eventTag == m2EventTag("$WNG")) {
            csm->playBodySound(mountDisplay, BS2::WING_FLAP, 0, position, ownMount);
        }
        return;
    }
    const uint64_t guid = guidForInstance(instanceId);
    if (guid == 0) return;
    auto entity = gameHandler_.getEntityManager().getEntity(guid);
    if (!entity || !entity->isUnit()) return;
    // The display the unit wears now: a druid in cat form steps and dies as a cat.
    const uint32_t displayId = std::static_pointer_cast<game::Unit>(entity)->getDisplayId();
    if (displayId == 0) return;
    using BS = audio::CombatSoundManager::BodySound;
    const uint32_t stem = eventTag & 0x00FFFFFFu;
    const uint32_t last = eventTag >> 24;
    // The local character's own sounds play by the own-unit rule: in the
    // listener's head at 0.65 (Wow.exe 0x4cf170, 0x747310).
    const bool own = guid == gameHandler_.getPlayerGuid();
    if (eventTag == m2EventTag("$FSD")) {
        // The local character's steps come from here too once the footstep
        // manager has handed them over to the tables; until then its footstep
        // driver plays them from its own bank.
        auto* fsm = audioCoordinator_ ? audioCoordinator_->getFootstepManager() : nullptr;
        if (own && !(fsm && fsm->ownStepsFromTables())) return;
        uint32_t ground = 0;
        bool inWater = false;
        footstepGround(position, ground, inWater);
        csm->playFootstep(displayId, ground, inWater, position, own, fsm ? fsm->getVolumeScale() : 1.0f);
    } else if (eventTag == rendering::CharacterRenderer::kAnimEventDeathStart) {
        csm->playBodySound(displayId, BS::DEATH, 0, position, own);
    } else if (eventTag == m2EventTag("$WNG")) {
        csm->playBodySound(displayId, BS::WING_FLAP, 0, position, own);
    } else if (stem == (m2EventTag("$FD1") & 0x00FFFFFFu) && last >= '1' && last <= '5') {
        csm->playBodySound(displayId, BS::FIDGET, last - '1', position, own);
    } else if (stem == (m2EventTag("$AH0") & 0x00FFFFFFu) && last >= '0' && last <= '3') {
        csm->playBodySound(displayId, BS::CUSTOM_ATTACK, last - '0', position, own);
    }
}

void NPCInteractionCallbackHandler::setupCallbacks() {
    gameHandler_.setTextEmoteSoundCallback([this](uint64_t guid, uint32_t textEmoteId) {
        const auto* data = audioCoordinator_ ? audioCoordinator_->getSoundData() : nullptr;
        if (!data) return;
        auto entity = gameHandler_.getEntityManager().getEntity(guid);
        if (!entity || !entity->isUnit()) return;
        auto unit = std::static_pointer_cast<game::Unit>(entity);
        // UNIT_FIELD_BYTES_0: race, class, gender, power type.
        const uint32_t bytes0 = unit->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
        const uint32_t race = bytes0 & 0xFF;
        const uint32_t sex = (bytes0 >> 16) & 0xFF;
        const audio::SoundEntry* entry = data->emoteSound(textEmoteId, race, sex == 1 ? 1u : 0u);
        if (!entry || entry->files.empty()) return;
        static std::mt19937 rng{std::random_device{}()};
        const auto& file = entry->files[std::uniform_int_distribution<size_t>(0, entry->files.size() - 1)(rng)];
        const glm::vec3 pos = core::coords::canonicalToRender(
            glm::vec3(unit->getX(), unit->getY(), unit->getZ()));
        playEntryAtUnit(*entry, file, pos, guid == gameHandler_.getPlayerGuid());
    });

    // M2 $CSD animation sound events: the emote voices EmotesTextSound has no
    // rows for (/laugh, /cry, /kiss, /chicken, /applaud) come from the model,
    // keyed to the animation's frames, as in the stock client. Animations an
    // EmotesTextSound row already voices are skipped so nothing plays twice.
    //
    // The model's other sound events come the same way, for every unit in
    // view: $FSD footfalls, $CSS / $CAH (the melee swoosh and blow), $FDn
    // fidgets, $WNG wing flaps, $AHn custom attacks, and the death cry as
    // the Death animation starts.
    if (renderer_) {
        renderer_->setCharacterAnimSoundEventCallback(
            [this](uint32_t instanceId, uint32_t animationId, uint32_t eventTag, uint32_t soundEntryId,
                   const glm::vec3& position) {
                if (eventTag != pipeline::m2EventTag("$CSD")) {
                    handleUnitAnimEvent(instanceId, eventTag, position);
                    return;
                }
                const auto* data = audioCoordinator_ ? audioCoordinator_->getSoundData() : nullptr;
                static int logBudget = 30;
                if (logBudget > 0) {
                    --logBudget;
                    LOG_INFO("Anim sound event: anim ", animationId, " sound ", soundEntryId,
                             " skipped=", (data && data->animHasEmoteTextSound(animationId)),
                             " entry=", (data && data->sound(soundEntryId)) ? "yes" : "no");
                }
                if (!data || data->animHasEmoteTextSound(animationId)) return;
                // Missing ids are normal: EmoteTrain's $CSD points at rows
                // the shipped SoundEntries.dbc no longer has.
                const audio::SoundEntry* entry = data->sound(soundEntryId);
                if (!entry || entry->files.empty()) return;
                static std::mt19937 rng{std::random_device{}()};
                const auto& file =
                    entry->files[std::uniform_int_distribution<size_t>(0, entry->files.size() - 1)(rng)];
                playEntryAtUnit(*entry, file, position,
                                renderer_ && instanceId == renderer_->getCharacterInstanceId());
            });
    }

    // The melee sounds ask here what each fighter holds and wears.
    if (auto* csm = audioCoordinator_ ? audioCoordinator_->getCombatSoundManager() : nullptr) {
        csm->setUnitResolver([this](uint64_t guid, audio::CombatUnitInfo& out) {
            return resolveCombatUnit(guid, out);
        });
    }

    // NPC greeting callback - play voice line
    // Said when the NPC's window opens (GameHandler::updateInteractTarget). The
    // stock client plays the NPCSounds greeting and nothing else: no welcome
    // animation (0x51FCE0 only re-picks the NPC's idle, 0x720860 plays the
    // sound). A talk animation, when there is one, is the emote the server
    // names in the quest giver packets.
    gameHandler_.setNpcGreetingCallback([this](uint64_t guid, const glm::vec3& position) {
        if (audioCoordinator_ && audioCoordinator_->getNpcVoiceManager()) {
            // Convert canonical to render coords for 3D audio
            glm::vec3 renderPos = core::coords::canonicalToRender(position);
            audio::VoiceType voiceType = resolveNpcVoiceType(guid);
            audioCoordinator_->getNpcVoiceManager()->playGreeting(guid, resolveNpcDisplayId(guid), voiceType, renderPos);
        }
    });

    // NPC farewell callback - play farewell voice line
    gameHandler_.setNpcFarewellCallback([this](uint64_t guid, const glm::vec3& position) {
        if (audioCoordinator_ && audioCoordinator_->getNpcVoiceManager()) {
            glm::vec3 renderPos = core::coords::canonicalToRender(position);
            audio::VoiceType voiceType = resolveNpcVoiceType(guid);
            audioCoordinator_->getNpcVoiceManager()->playFarewell(guid, resolveNpcDisplayId(guid), voiceType, renderPos);
        }
    });

    // NPC vendor callback - play vendor voice line
    gameHandler_.setNpcVendorCallback([this](uint64_t guid, const glm::vec3& position) {
        if (audioCoordinator_ && audioCoordinator_->getNpcVoiceManager()) {
            glm::vec3 renderPos = core::coords::canonicalToRender(position);
            audio::VoiceType voiceType = resolveNpcVoiceType(guid);
            audioCoordinator_->getNpcVoiceManager()->playVendor(guid, resolveNpcDisplayId(guid), voiceType, renderPos);
        }
    });

    // NPC aggro callback - play combat start voice line
    gameHandler_.setNpcAggroCallback([this](uint64_t guid, const glm::vec3& position) {
        if (audioCoordinator_ && audioCoordinator_->getNpcVoiceManager()) {
            glm::vec3 renderPos = core::coords::canonicalToRender(position);
            audio::VoiceType voiceType = resolveNpcVoiceType(guid);
            uint32_t displayId = 0;
            auto entity = gameHandler_.getEntityManager().getEntity(guid);
            if (entity && entity->getType() == game::ObjectType::UNIT) {
                displayId = std::static_pointer_cast<game::Unit>(entity)->getDisplayId();
            }
            audioCoordinator_->getNpcVoiceManager()->playAggro(
                guid, displayId, voiceType, renderPos);
        }
    });
}

}} // namespace wowee::core
