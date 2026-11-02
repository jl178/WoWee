#pragma once

#include <cstdint>
#include <string>
#include <optional>
#include <glm/vec3.hpp>

namespace wowee {

namespace rendering { class Renderer; }
namespace game { class GameHandler; }
namespace audio { class AudioCoordinator; }
namespace pipeline { class AssetManager; }
namespace ui { class UIManager; }

namespace core {

/// Handles audio-related callbacks: music, sound effects, level-up, achievement, LFG.
class AudioCallbackHandler {
public:
    AudioCallbackHandler(pipeline::AssetManager& assetManager,
                         audio::AudioCoordinator* audioCoordinator,
                         rendering::Renderer* renderer,
                         ui::UIManager* uiManager,
                         game::GameHandler& gameHandler);

    void setupCallbacks();

private:
    /// Resolve SoundEntries.dbc → the row's first file path, for SMSG_PLAY_MUSIC
    [[nodiscard]] std::optional<std::string> resolveSoundEntryPath(uint32_t soundId) const;
    /// LevelUp.m2, with its sound, on a unit. False when only the silent
    /// stand-in could be shown (or nothing), so the caller can still be heard.
    bool showLevelUpEffect(uint64_t guid, const glm::vec3& feet);

    pipeline::AssetManager& assetManager_;
    audio::AudioCoordinator* audioCoordinator_;
    rendering::Renderer* renderer_;
    ui::UIManager* uiManager_;
    game::GameHandler& gameHandler_;
};

} // namespace core
} // namespace wowee
