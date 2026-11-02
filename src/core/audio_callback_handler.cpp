#include "core/audio_callback_handler.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/spell_visual_system.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "game/game_handler.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/music_manager.hpp"
#include "audio/sound_data.hpp"
#include "audio/ui_sound_manager.hpp"
#include "audio/audio_engine.hpp"
#include "ui/ui_manager.hpp"

namespace wowee { namespace core {

AudioCallbackHandler::AudioCallbackHandler(
    pipeline::AssetManager& assetManager,
    audio::AudioCoordinator* audioCoordinator,
    rendering::Renderer* renderer,
    ui::UIManager* uiManager,
    game::GameHandler& gameHandler)
    : assetManager_(assetManager)
    , audioCoordinator_(audioCoordinator)
    , renderer_(renderer)
    , uiManager_(uiManager)
    , gameHandler_(gameHandler)
{
}

std::optional<std::string> AudioCallbackHandler::resolveSoundEntryPath(uint32_t soundId) const {
    auto dbc = assetManager_.loadDBC("SoundEntries.dbc");
    if (!dbc || !dbc->isLoaded()) return std::nullopt;

    int32_t idx = dbc->findRecordById(soundId);
    if (idx < 0) return std::nullopt;

    // SoundEntries.dbc (WotLK): field 2 = Name (label), fields 3-12 = File[0..9], field 23 = DirectoryBase
    const uint32_t row = static_cast<uint32_t>(idx);
    std::string dir = dbc->getString(row, 23);
    for (uint32_t f = 3; f <= 12; ++f) {
        std::string name = dbc->getString(row, f);
        if (name.empty()) continue;
        return dir.empty() ? name : dir + "\\" + name;
    }
    return std::nullopt;
}

bool AudioCallbackHandler::showLevelUpEffect(uint64_t guid, const glm::vec3& feet) {
    if (!renderer_) return false;
    // Spells\LevelUp\LevelUp.m2 on whoever levelled - the stock client's
    // hardcoded SpellVisualEffectName 21, "Unit Level Up" - and the level-up
    // sound is that model's $SND event (LEVELUP, 888, 33 ms in), so it plays
    // at them for anyone in sight, not only for the player. The spell visual
    // system plays model events; LevelUpEffect draws the same model without
    // them and stands in only when the first cannot load it.
    if (auto* svs = renderer_->getSpellVisualSystem()) {
        if (svs->playEffectModel("Spells\\LevelUp\\LevelUp.m2", feet,
                                 gameHandler_.resolveUnitRenderInstance(guid)))
            return true;
    }
    if (auto* ac = renderer_->getAnimationController()) ac->triggerLevelUpEffect(feet);
    return false;
}

void AudioCallbackHandler::setupCallbacks() {
    // Level-up callback - UI ding overlay, and the level-up effect with its sound
    gameHandler_.setLevelUpCallback([this](uint32_t newLevel) {
        if (uiManager_) {
            uiManager_->getGameScreen().toastManager().triggerDing(newLevel);
        }
        const glm::vec3 feet = renderer_ ? renderer_->getCharacterPosition() : glm::vec3(0.0f);
        if (showLevelUpEffect(gameHandler_.getPlayerGuid(), feet)) return;
        // No model to carry it: the sound alone, as the interface would hear it.
        if (auto* sfx = audioCoordinator_ ? audioCoordinator_->getUiSoundManager() : nullptr)
            sfx->playLevelUp();
    });

    // No achievement callback: the badge is alertframes.lua's and the sound is
    // played where SMSG_ACHIEVEMENT_EARNED is read (SpellHandler).

    // Server-triggered music callback (SMSG_PLAY_MUSIC)
    // Resolves soundId → SoundEntries.dbc → MPQ path → MusicManager.
    gameHandler_.setPlayMusicCallback([this](uint32_t soundId) {
        if (!renderer_) return;
        auto* music = audioCoordinator_ ? audioCoordinator_->getMusicManager() : nullptr;
        if (!music) return;

        auto path = resolveSoundEntryPath(soundId);
        if (path) {
            music->playMusic(*path, /*loop=*/false);
        }
    });

    // SMSG_PLAY_SOUND: the SoundEntries row, played as every other row is -
    // one of its files picked at random, at the row's volume. This used to
    // take the row's first file at full volume, so a row of several variants
    // always played the same one, and louder than the stock client does.
    gameHandler_.setPlaySoundCallback([this](uint32_t soundId) {
        auto* sfx = audioCoordinator_ ? audioCoordinator_->getUiSoundManager() : nullptr;
        const audio::SoundData* data = audioCoordinator_ ? audioCoordinator_->getSoundData() : nullptr;
        if (sfx && data) sfx->playEntry(data->sound(soundId));
    });

    // SMSG_PLAY_OBJECT_SOUND / SMSG_PLAY_SPELL_IMPACT: the same, at the object.
    gameHandler_.setPlayPositionalSoundCallback([this](uint32_t soundId, uint64_t sourceGuid) {
        auto* sfx = audioCoordinator_ ? audioCoordinator_->getUiSoundManager() : nullptr;
        const audio::SoundData* data = audioCoordinator_ ? audioCoordinator_->getSoundData() : nullptr;
        if (!sfx || !data) return;
        const audio::SoundEntry* entry = data->sound(soundId);
        if (!entry) return;
        // Entity stores canonical coords; listener uses render coords (camera).
        // An object this client does not know is heard without a position.
        auto entity = gameHandler_.getEntityManager().getEntity(sourceGuid);
        if (entity) {
            glm::vec3 canonical{entity->getLatestX(), entity->getLatestY(), entity->getLatestZ()};
            sfx->playEntryAt(entry, core::coords::canonicalToRender(canonical));
        } else {
            sfx->playEntry(entry);
        }
    });

    // Other player level-up callback - trigger 3D effect + chat notification
    gameHandler_.setOtherPlayerLevelUpCallback([this](uint64_t guid, uint32_t newLevel) {
        if (!renderer_) return;

        // The level-up effect on the other player, with its sound at them.
        auto entity = gameHandler_.getEntityManager().getEntity(guid);
        if (entity) {
            glm::vec3 canonical(entity->getX(), entity->getY(), entity->getZ());
            showLevelUpEffect(guid, core::coords::canonicalToRender(canonical));
        }

        // Show chat message if in group
        if (gameHandler_.isInGroup()) {
            std::string name = gameHandler_.getCachedPlayerName(guid);
            if (name.empty()) name = "A party member";
            game::MessageChatData msg;
            msg.type = game::ChatType::SYSTEM;
            msg.language = game::ChatLanguage::UNIVERSAL;
            msg.message = name + " has reached level " + std::to_string(newLevel) + "!";
            gameHandler_.addLocalChatMessage(msg);
        }
    });

    // Open dungeon finder callback - server sends SMSG_OPEN_LFG_DUNGEON_FINDER
    gameHandler_.setOpenLfgCallback([this]() {
        if (uiManager_) uiManager_->getGameScreen().openDungeonFinder(gameHandler_);
    });
}

}} // namespace wowee::core
