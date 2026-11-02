#pragma once

#include "audio/stock_sound_rules.hpp"

#include "audio/footstep_manager.hpp"
#include "platform/process.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace wowee { namespace audio { struct SoundEntry; class SoundData; } }

namespace wowee {
namespace pipeline { class AssetManager; }
namespace audio {

class ActivitySoundManager {
public:
    ActivitySoundManager();
    ~ActivitySoundManager();

    bool initialize(pipeline::AssetManager* assets);
    void shutdown();
    void update(float deltaTime);
    [[nodiscard]] bool isInitialized() const { return initialized; }

    void playLanding(FootstepSurface surface, bool hardLanding);
    void setSwimmingState(bool swimming, bool moving);
    void setCharacterVoiceProfile(const std::string& modelName);
    void setCharacterVoiceProfile(const std::string& raceFolder, const std::string& raceBase, bool male);
    /// The voice the client's tables give the race: exertion, wound and death
    /// from CreatureSoundData, jump and land from SoundEntries. Anything the
    /// tables leave out keeps the guessed paths of the overload above.
    void setCharacterVoiceProfile(const std::string& raceFolder, const std::string& raceBase, bool male,
                                  uint32_t raceId, const SoundData* data);
    void playWaterEnter();
    void playWaterExit();
    /// The character's own exertion on a swing that lands: 35 in 100, or
    /// always on a crit, one exertion at a time (Wow.exe 0x7631a0, category 9).
    void playAttackGrunt(bool isCrit = false);
    /// The character's own injury cry: 30 in 100, always on a crit, one at a
    /// time (category 12). No timer beyond that, as in the stock client.
    void playWound(bool isCrit = false);
    void setVolumeScale(float scale) { volumeScale = scale; }
    [[nodiscard]] float getVolumeScale() const { return volumeScale; }

private:
    struct Sample {
        std::string path;
        std::vector<uint8_t> data;
        float volume = 0.7f;  ///< Gain under the activity volume; the data's VolumeFloat when it names the clip.
    };

    struct SurfaceLandingSet {
        std::vector<Sample> clips;
    };

    bool initialized = false;
    pipeline::AssetManager* assetManager = nullptr;

    std::vector<Sample> jumpClips;
    std::vector<Sample> splashEnterClips;
    std::vector<Sample> splashExitClips;
    std::vector<Sample> swimLoopClips;
    std::vector<Sample> hardLandClips;
    std::vector<Sample> landVoiceClips;
    std::vector<Sample> attackGruntClips;
    std::vector<Sample> woundClips;
    std::vector<Sample> woundCritClips;
    std::vector<Sample> deathClips;
    std::array<SurfaceLandingSet, 7> landingSets;

    bool swimmingActive = false;
    bool swimMoving = false;
    ProcessHandle swimLoopPid = INVALID_PROCESS;
    std::string loopTempPath = platform::getTempFilePath("wowee_swim_loop.wav");
    std::mt19937 rng;

    std::chrono::steady_clock::time_point lastLandAt{};
    std::chrono::steady_clock::time_point lastSplashAt{};
    std::chrono::steady_clock::time_point lastSwimStrokeAt{};
    std::string voiceProfileKey;
    float volumeScale = 1.0f;

    void preloadCandidates(std::vector<Sample>& out, const std::vector<std::string>& candidates);
    /// Replace out with the files a SoundEntries row names, when any of them load.
    void replaceFromEntry(std::vector<Sample>& out, const SoundEntry* entry);
    /// The character's own voice: 2D at 0.65, priority 110 (SoundOptions::ownCharacter).
    bool playVoice(const std::vector<Sample>& clips, stock::Category category = stock::Category::Sfx);
    /// The character's own non-vocal sounds - landings, splashes, strokes.
    bool playOwn(const std::vector<uint8_t>& data, float volume, float pitch);
    void preloadLandingSet(FootstepSurface surface, const std::string& material);
    void rebuildJumpClipsForProfile(const std::string& raceFolder, const std::string& raceBase, bool male);
    void rebuildSwimLoopClipsForProfile(const std::string& raceFolder, const std::string& raceBase, bool male);
    void rebuildHardLandClipsForProfile(const std::string& raceFolder, const std::string& raceBase, bool male);
    void rebuildCombatVocalClipsForProfile(const std::string& raceFolder, const std::string& raceBase, bool male);
    bool playSplash(const std::vector<Sample>& clips);
    void startSwimLoop();
    void stopSwimLoop();
    void stopOneShot();
    void reapProcesses();
};

} // namespace audio
} // namespace wowee
