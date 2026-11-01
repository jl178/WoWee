#pragma once

#include <unordered_map>
#include <vector>
#include <memory>
#include <string>
#include <cstdint>

namespace wowee {
namespace pipeline {
class AssetManager;
}

namespace audio {

class SoundData;
struct SoundEntry;

class UiSoundManager {
public:
    UiSoundManager() = default;
    ~UiSoundManager() = default;

    // Initialization
    bool initialize(pipeline::AssetManager* assets);
    void shutdown();

    /// The client's sound tables, once they are read. Every named sound below
    /// resolves through SoundEntries.dbc first; the files loaded at start-up
    /// are only what an install without the table falls back to.
    void setSoundData(const SoundData* data) { soundData_ = data; }

    /// Play a sound by the name FrameXML asks for - PlaySound("igQuestFailed")
    /// and its siblings - or the C++ client asks for by name.
    ///
    /// Resolved through SoundEntries.dbc, which is where those names come
    /// from: the interface is naming a row, and the row names the files and
    /// the volume. That is the sound the stock client plays, at the level it
    /// plays it, rather than the nearest one this client had already loaded.
    ///
    /// False when the name is not in the table or none of its files will
    /// load, so the caller can fall back rather than going silent.
    bool playByName(const std::string& soundName);

    /// Play one SoundEntries row: one of its files, picked each time as the
    /// stock client does, at the row's volume times the interface volume.
    bool playEntry(const SoundEntry* entry);

    /// Play a sound by path, which is what PlaySoundFile takes. Cached the
    /// same way and by the same store as the rows' files.
    bool playFile(const std::string& path, float volume = 0.7f);

    // Volume control
    void setVolumeScale(float scale);
    [[nodiscard]] float getVolumeScale() const { return volumeScale_; }

    // The sounds the C++ client plays itself. Each names the SoundEntries row
    // the stock client plays for that moment; the file after it in the .cpp
    // is the no-table fallback and is the file that row names.

    /// QUESTADDED: the stock client's ERR_QUEST_ACCEPTED_S message carries it.
    void playQuestActivate();
    /// igQuestListComplete: SMSG_QUESTGIVER_QUEST_COMPLETE (Wow.exe 0x6d0e10).
    void playQuestComplete();
    /// igQuestFailed: every ERR_QUEST_FAILED_* / ERR_QUEST_LOG_FULL message.
    void playQuestFailed();
    /// "Fishing Hooked": the bobber display's custom-animation sound
    /// (GameObjectDisplayInfo 668, Sound[6]).
    void playFishingBite();

    // Loot sounds
    /// LOOTWINDOWCOINSOUND, for AudioCoordinator::playCoinSound without tables.
    void playLootCoinSmall();
    /// Stand-in for the ItemGroupSounds put-down foley without tables.
    void playLootItem();

    /// "Food" and "Drinking", the food and drink spells' visual kit sounds.
    /// Only for a spell whose visual names none.
    void playEating();
    void playDrinking();

    /// LEVELUP (888), the $SND event of Spells\LevelUp\LevelUp.m2.
    void playLevelUp();

    /// AchievementSound (12891), the $SND event of achievement_onroot.m2.
    void playAchievementAlert();

    /// TellMessage, which chatframe.lua plays for a whisper. Only for a chat
    /// frame that is not FrameXML's.
    void playWhisperReceived();

    /// MapPing, which minimap.lua plays on MINIMAP_PING. Only for a minimap
    /// that is not FrameXML's.
    void playMinimapPing();

private:
    struct UISample {
        std::string path;
        std::vector<uint8_t> data;
        bool loaded;
    };

    // No-table fallbacks, one per method above
    std::vector<UISample> questActivateSounds_;
    std::vector<UISample> questCompleteSounds_;
    std::vector<UISample> questFailedSounds_;
    std::vector<UISample> fishingBiteSounds_;

    std::vector<UISample> lootCoinSmallSounds_;
    std::vector<UISample> lootItemSounds_;

    std::vector<UISample> eatingSounds_;
    std::vector<UISample> drinkingSounds_;

    std::vector<UISample> levelUpSounds_;
    std::vector<UISample> achievementSounds_;

    std::vector<UISample> whisperSounds_;
    std::vector<UISample> minimapPingSounds_;

    // State tracking
    float volumeScale_ = 1.0f;
    bool initialized_ = false;
    const SoundData* soundData_ = nullptr;

    // Helper methods
    bool loadSound(const std::string& path, UISample& sample, pipeline::AssetManager* assets);
    /// A file read once and kept, by path. Null when it will not load, which
    /// is remembered too, so a missing file is looked for once rather than on
    /// every click.
    const UISample* cachedSample(const std::string& path);
    /// Samples loaded on demand, by path. Kept because a UI sound is played
    /// again and again, and reading it from the archive each time is the one
    /// cost this has that the preloaded fallbacks do not.
    std::unordered_map<std::string, UISample> namedSamples_;
    pipeline::AssetManager* assets_ = nullptr;
    /// The row by name, or the fallback when there is no table or no row.
    void playStock(const char* soundName, const std::vector<UISample>& fallback);
    void playSound(const std::vector<UISample>& library);
};

} // namespace audio
} // namespace wowee
