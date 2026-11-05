#pragma once

#include "audio/sound_data.hpp"
#include "audio/zone_music_controller.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <glm/vec3.hpp>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace game { class ZoneManager; }
namespace audio {

class MusicManager;
class FootstepManager;
class ActivitySoundManager;
class MountSoundManager;
class NpcVoiceManager;
class PlayerVoiceManager;
class AmbientSoundManager;
class UiSoundManager;
class CombatSoundManager;
class SpellSoundManager;
class MovementSoundManager;
class WorldSoundEmitters;

/// Flat context passed from Renderer into updateZoneAudio() each frame.
/// All values are pre-queried so AudioCoordinator needs no rendering pointers.
struct ZoneAudioContext {
    float deltaTime = 0.0f;
    glm::vec3 cameraPosition{0.0f};
    bool isSwimming = false;
    bool insideWmo = false;
    uint32_t insideWmoId = 0;
    // Visual weather state for ambient audio sync
    int weatherType = 0;      // 0=none, 1=rain, 2=snow, 3=storm
    float weatherIntensity = 0.0f;
    // The server's weather state (SMSG_WEATHER), a Weather.dbc row: its
    // AmbienceID is the rain or snow loop (sound-engine.md section 6).
    uint32_t weatherState = 0;
    // Visible sky clock after zone ambience overrides (hours in [0, 24)).
    float gameTimeHours = 12.0f;
    // Terrain tile for offline zone lookup
    int tileX = 0, tileY = 0;
    bool hasTile = false;
    // Server-authoritative zone (from SMSG_INIT_WORLD_STATES); 0 = offline
    uint32_t serverZoneId = 0;
    // AreaTable id of the terrain chunk under the player; 0 = unknown. The
    // sound tables are keyed by area, not zone: Goldshire and Elwynn share
    // music, Stormwind's districts share the city's.
    uint32_t areaId = 0;
    // The building and room the camera is in, for WMOAreaTable: a tavern's
    // music and a crypt's ambience live there, not in AreaTable.
    // The group is named by its MOGP id, the WMOAreaTable key, not its index
    // in the root file: keyed on the index, no room row ever matched and
    // every room took its building's -1 row.
    uint32_t wmoRootId = 0;
    int32_t wmoGroupId = -1;
    uint16_t wmoNameSet = 0;
    // That group has the indoor flag: a room or a cave, whose reverb is its
    // own. An outdoor group - a city street - sounds like the area around it.
    bool wmoInterior = false;
    // The group lacks the exterior flag (MOGP 0x8): what the stock sound
    // code calls indoors (0x7a1480). The area's music and ambience give way
    // to the building's rows there, and the weather's loop is not heard.
    bool wmoSoundIndoors = false;
    // The camera is below the water's surface (not merely swimming at it):
    // the world goes muffled and the underwater loop plays.
    bool cameraUnderwater = false;
    // The map the world is on, for SoundEmitters.dbc; 0xFFFFFFFF = unknown.
    uint32_t mapId = 0xFFFFFFFFu;
    // Zone manager pointer (for zone info and music queries)
    game::ZoneManager* zoneManager = nullptr;
};

/// Coordinates all audio subsystems.
/// Extracted from Renderer to separate audio lifecycle from rendering.
/// Owned by Application; Renderer and UI components access through Application.
class AudioCoordinator {
public:
    /// Loop Music, from the audio options: the stock Sound_ZoneMusicNoDelay.
    /// With it on, the next zone track follows the last with no silence.
    void setZoneMusicLooping(bool loop);

    AudioCoordinator();
    ~AudioCoordinator();

    /// Initialize the audio engine and all managers.
    /// @return true if audio is available (engine initialized successfully)
    [[nodiscard]] bool initialize();


    /// Shutdown all audio managers and engine.
    void shutdown();

    /// Per-frame zone detection, music transitions, and ambient weather sync.
    /// Called from Renderer::update() with a pre-filled context.
    void updateZoneAudio(const ZoneAudioContext& ctx);

    /// Read the sound tables. Until this succeeds, music and ambience come
    /// from the hand-written tables in ZoneManager and AmbientSoundManager.
    void loadSoundData(pipeline::AssetManager* assets);
    [[nodiscard]] const SoundData* getSoundData() const { return soundData_.loaded() ? &soundData_ : nullptr; }

    /// The screen effect's sounds (ScreenEffect.dbc SoundAmbienceID and
    /// ZoneMusicID, 0 for none): the top music and ambience slot (0x4c8fa0),
    /// which wins over the area's. The ghost world's GhostState loop and
    /// GhostMusic. 0, 0 hands both back to the area.
    void setScreenEffectSounds(uint32_t soundAmbienceId, uint32_t zoneMusicId);

    /// LOOTWINDOWCOINSOUND, the stock client's only coin sound. Plays
    /// LootCoinSmall.wav directly when SoundEntries.dbc is missing.
    void playCoinSound();
    /// An item display's ItemGroupSounds pickup or put-down sound. False when
    /// the tables are missing or the display has no group, so the caller can
    /// choose a fallback.
    bool playItemFoley(uint32_t itemDisplayId, bool pickup);

    [[nodiscard]] const std::string& getCurrentZoneName() const { return currentZoneName_; }
    [[nodiscard]] uint32_t getCurrentZoneId() const { return currentZoneId_; }

    /// Called when the "Enable WoWee Music" setting is turned off. If one of
    /// the WoWee (file-based) tracks is currently playing, crossfade to a
    /// stock track so the disable takes effect immediately instead of waiting
    /// for the track to end.
    void onOriginalSoundtrackDisabled(game::ZoneManager* zm);

    // Accessors for all audio managers (same interface as Renderer had)
    MusicManager* getMusicManager() { return musicManager_.get(); }
    FootstepManager* getFootstepManager() { return footstepManager_.get(); }
    ActivitySoundManager* getActivitySoundManager() { return activitySoundManager_.get(); }
    MountSoundManager* getMountSoundManager() { return mountSoundManager_.get(); }
    NpcVoiceManager* getNpcVoiceManager() { return npcVoiceManager_.get(); }
    PlayerVoiceManager* getPlayerVoiceManager() { return playerVoiceManager_.get(); }
    AmbientSoundManager* getAmbientSoundManager() { return ambientSoundManager_.get(); }
    UiSoundManager* getUiSoundManager() { return uiSoundManager_.get(); }
    CombatSoundManager* getCombatSoundManager() { return combatSoundManager_.get(); }
    SpellSoundManager* getSpellSoundManager() { return spellSoundManager_.get(); }
    MovementSoundManager* getMovementSoundManager() { return movementSoundManager_.get(); }
    /// SoundEmitters.dbc loops and M2 doodad sound events, by distance.
    WorldSoundEmitters* getWorldSoundEmitters() { return worldSoundEmitters_.get(); }

private:
    void playZoneMusic(const std::string& music, float fadeMs = stock::kMusicFadeSeconds * 1000.0f);
    void updateTableMusic(const ZoneAudioContext& ctx, uint32_t zoneId, bool suppressed);
    void updateTableAmbience(const ZoneAudioContext& ctx);
    void updateReverb(const ZoneAudioContext& ctx);
    /// One of the row's files on the interface bus, at the row's volume times
    /// the interface volume.
    bool playInterfaceEntry(const SoundEntry* entry);

    SoundData soundData_;
    std::unique_ptr<ZoneMusicController> zoneMusic_;
    uint64_t ambienceKey_ = 0;
    bool ambienceUnderwater_ = false;  ///< slot 9 held the underwater loop last update
    int32_t reverbPrefId_ = -1;   ///< the SoundProviderPreferences row in force; 0 dry, -1 none yet
    bool reverbUnderwater_ = false;
    std::string underwaterLoopPath_;  ///< from SoundEntries, resolved once the tables load
    float underwaterLoopVolume_ = 0.69f;
    bool loopMusic_ = false;  ///< Loop Music: Sound_ZoneMusicNoDelay, no rest between tracks
    uint32_t screenEffectAmbience_ = 0;  ///< SoundAmbience row from the screen effect (slot 10)
    uint32_t screenEffectMusic_ = 0;     ///< ZoneMusic row from the screen effect (slot 10)

    std::unique_ptr<MusicManager> musicManager_;
    std::unique_ptr<FootstepManager> footstepManager_;
    std::unique_ptr<ActivitySoundManager> activitySoundManager_;
    std::unique_ptr<MountSoundManager> mountSoundManager_;
    std::unique_ptr<NpcVoiceManager> npcVoiceManager_;
    std::unique_ptr<PlayerVoiceManager> playerVoiceManager_;
    std::unique_ptr<AmbientSoundManager> ambientSoundManager_;
    std::unique_ptr<UiSoundManager> uiSoundManager_;
    std::unique_ptr<CombatSoundManager> combatSoundManager_;
    std::unique_ptr<SpellSoundManager> spellSoundManager_;
    std::unique_ptr<MovementSoundManager> movementSoundManager_;
    std::unique_ptr<WorldSoundEmitters> worldSoundEmitters_;

    bool audioAvailable_ = false;

    // Zone/music state - moved from Renderer
    uint32_t currentZoneId_ = 0;
    std::string currentZoneName_;
    bool inTavern_ = false;
    bool inBlacksmith_ = false;
    float musicSwitchCooldown_ = 0.0f;
};

} // namespace audio
} // namespace wowee
