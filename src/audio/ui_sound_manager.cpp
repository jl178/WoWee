#include "audio/ui_sound_manager.hpp"
#include "audio/sample_load.hpp"
#include "audio/sound_data.hpp"
#include "audio/audio_engine.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <random>

namespace wowee {
namespace audio {

namespace {
std::mt19937& uiRng() {
    static std::mt19937 rng{std::random_device{}()};
    return rng;
}
}  // namespace

bool UiSoundManager::initialize(pipeline::AssetManager* assets) {
    if (!assets) {
        LOG_ERROR("UISoundManager: AssetManager is null");
        return false;
    }

    assets_ = assets;
    LOG_INFO("UISoundManager: Initializing...");

    // The no-table fallbacks. Each is the file the SoundEntries row named in
    // the header points at, so an install without SoundEntries.dbc hears the
    // same file - only the volume is this client's (0.7) rather than the row's.
    //
    // There is no Error.wav here any more, nor a bag, button, target, mail or
    // purchase sound: the stock client plays none of those from its own code
    // (Wow.exe's PlaySoundByName callers and its error table list every one
    // it does), and FrameXML plays its own panel sounds through PlaySound.
    questActivateSounds_.resize(1);
    bool questActivateLoaded = loadSound("Sound\\Interface\\iQuestActivate.wav", questActivateSounds_[0], assets);

    questCompleteSounds_.resize(1);
    bool questCompleteLoaded = loadSound("Sound\\Interface\\iQuestComplete.wav", questCompleteSounds_[0], assets);

    questFailedSounds_.resize(1);
    bool questFailedLoaded = loadSound("Sound\\Interface\\igQuestFailed.wav", questFailedSounds_[0], assets);

    lootCoinSmallSounds_.resize(1);
    bool lootCoinSmallLoaded = loadSound("Sound\\Interface\\LootCoinSmall.wav", lootCoinSmallSounds_[0], assets);

    // No SoundEntries row names this file; it stands in for the item's own
    // ItemGroupSounds put-down sound, which needs the tables to find.
    lootItemSounds_.resize(1);
    bool lootItemLoaded = loadSound("Sound\\Interface\\igLootCreature.wav", lootItemSounds_[0], assets);

    eatingSounds_.resize(1);
    bool eatingLoaded = loadSound("Sound\\Interface\\iEating1.wav", eatingSounds_[0], assets);

    drinkingSounds_.resize(1);
    bool drinkingLoaded = loadSound("Sound\\Interface\\iDrinking1.wav", drinkingSounds_[0], assets);

    // Sound\Spells, where LEVELUP and AchievementSound point. The achievement
    // file was asked for as Sound\Interface\AchievementSound.wav, which does
    // not exist, so every achievement played the level-up sound instead.
    levelUpSounds_.resize(1);
    bool levelUpLoaded = loadSound("Sound\\Spells\\LevelUp.wav", levelUpSounds_[0], assets);

    achievementSounds_.resize(1);
    bool achievementLoaded = loadSound("Sound\\Spells\\AchievmentSound1.wav", achievementSounds_[0], assets);

    // TellMessage's file. Whisper_TellMale.wav does not exist either, and its
    // fallback was the target-select click.
    whisperSounds_.resize(1);
    loadSound("Sound\\Interface\\iTellMessage.wav", whisperSounds_[0], assets);

    minimapPingSounds_.resize(1);
    loadSound("Sound\\Interface\\MapPing.wav", minimapPingSounds_[0], assets);

    LOG_INFO("UISoundManager: Quest sounds - Activate: ", questActivateLoaded ? "YES" : "NO",
             ", Complete: ", questCompleteLoaded ? "YES" : "NO",
             ", Failed: ", questFailedLoaded ? "YES" : "NO");
    LOG_INFO("UISoundManager: Loot sounds - Coins: ", lootCoinSmallLoaded ? "YES" : "NO",
             ", Items: ", lootItemLoaded ? "YES" : "NO");
    LOG_INFO("UISoundManager: Misc sounds - Eating: ", eatingLoaded ? "YES" : "NO",
             ", Drinking: ", drinkingLoaded ? "YES" : "NO",
             ", LevelUp: ", levelUpLoaded ? "YES" : "NO",
             ", Achievement: ", achievementLoaded ? "YES" : "NO");

    initialized_ = true;
    LOG_INFO("UISoundManager: Initialization complete");
    return true;
}

void UiSoundManager::shutdown() {
    initialized_ = false;
}

bool UiSoundManager::loadSound(const std::string& path, UISample& sample, pipeline::AssetManager* assets) {
    return loadSampleFile(path, sample, assets, "UISoundManager");
}

const UiSoundManager::UISample* UiSoundManager::cachedSample(const std::string& path) {
    auto cached = namedSamples_.find(path);
    if (cached == namedSamples_.end()) {
        UISample sample;
        sample.loaded = false;
        loadSound(path, sample, assets_);
        cached = namedSamples_.emplace(path, std::move(sample)).first;
    }
    return cached->second.loaded ? &cached->second : nullptr;
}

bool UiSoundManager::playEntry(const SoundEntry* entry) {
    if (!initialized_ || !entry || entry->files.empty()) return false;
    // A random file of the row each time, and the next one along when that
    // one will not load or decode: playSound2D takes WAV only, and
    // SoundEntries lists mp3s among its wavs. Claiming success for one of
    // those would skip the caller's fallback and play nothing at all.
    const size_t count = entry->files.size();
    const size_t first = firstFile(*entry);
    SoundOptions opts;
    opts.kitId = entry->id;
    opts.noDuplicate = (entry->flags & stock::kFlagNoDuplicate) != 0;
    for (size_t i = 0; i < count; ++i) {
        const UISample* sample = cachedSample(entry->files[(first + i) % count]);
        if (!sample) continue;
        if (AudioEngine::instance().playSound2D(sample->data,
                                                stock::kitVolume(entry->volume, entry->flags, volumeScale_, uiRng()),
                                                stock::kitPitch(entry->flags, uiRng()), SoundBus::Interface, opts)) {
            return true;
        }
    }
    return false;
}

size_t UiSoundManager::firstFile(const SoundEntry& entry) const {
    // The row's own bag when the tables are read (each file once before any
    // repeats, never the same twice running); a plain draw otherwise.
    if (soundData_) {
        if (const std::string* f = soundData_->nextFile(entry)) {
            return static_cast<size_t>(f - entry.files.data());
        }
    }
    return std::uniform_int_distribution<size_t>(0, entry.files.size() - 1)(uiRng());
}

bool UiSoundManager::playEntryAt(const SoundEntry* entry, const glm::vec3& position) {
    if (!initialized_ || !entry || entry->files.empty()) return false;
    auto& engine = AudioEngine::instance();
    const float maxDistance = entry->distanceCutoff > 0.0f ? entry->distanceCutoff : stock::kDefaultCutoff;
    // As playEntry: the row's next file, and the next one along when it will
    // not load or decode.
    const size_t count = entry->files.size();
    const size_t first = firstFile(*entry);
    SoundOptions opts;
    opts.minDistance = entry->minDistance;
    opts.kitId = entry->id;
    opts.noDuplicate = (entry->flags & stock::kFlagNoDuplicate) != 0;
    for (size_t i = 0; i < count; ++i) {
        const UISample* sample = cachedSample(entry->files[(first + i) % count]);
        if (!sample) continue;
        if (engine.playSound3D(sample->data, position, stock::kitVolume(entry->volume, entry->flags, 1.0f, uiRng()),
                               stock::kitPitch(entry->flags, uiRng()), maxDistance, opts)) {
            return true;
        }
    }
    return false;
}

bool UiSoundManager::playUiMessage(std::string_view messageKey) {
    const char* soundName = uiMessageSoundName(messageKey);
    if (!soundName) return false;
    if (playByName(soundName)) return true;
    // No table: the preloaded file of the rows that have one.
    const std::string_view name(soundName);
    if (name == "igQuestFailed") playSound(questFailedSounds_);
    else if (name == "QUESTADDED") playSound(questActivateSounds_);
    else if (name == "LEVELUP") playSound(levelUpSounds_);
    else return false;
    return true;
}

bool UiSoundManager::playByName(const std::string& soundName) {
    if (!initialized_ || soundName.empty() || !soundData_) return false;
    return playEntry(soundData_->soundByName(soundName));
}

bool UiSoundManager::playFile(const std::string& path, float volume) {
    if (!initialized_ || path.empty() || !assets_) return false;
    const UISample* sample = cachedSample(path);
    if (!sample) return false;
    return AudioEngine::instance().playSound2D(sample->data, volume * volumeScale_,
                                               1.0f, SoundBus::Interface);
}

void UiSoundManager::playSound(const std::vector<UISample>& library) {
    if (!initialized_) return;
    const UISample* sample = pickLoadedSample(library, uiRng());
    if (!sample) return;
    AudioEngine::instance().playSound2D(sample->data, 0.7f * volumeScale_, 1.0f, SoundBus::Interface);
}

void UiSoundManager::playStock(const char* soundName, const std::vector<UISample>& fallback) {
    if (playByName(soundName)) return;
    playSound(fallback);
}

void UiSoundManager::setVolumeScale(float scale) {
    volumeScale_ = std::max(0.0f, std::min(1.0f, scale));
}

// Quest sounds
void UiSoundManager::playQuestComplete() { playStock("igQuestListComplete", questCompleteSounds_); }
void UiSoundManager::playQuestFailed() { playStock("igQuestFailed", questFailedSounds_); }

// Loot sounds
void UiSoundManager::playLootCoinSmall() { playStock("LOOTWINDOWCOINSOUND", lootCoinSmallSounds_); }
void UiSoundManager::playLootItem() { playSound(lootItemSounds_); }

// Eating/drinking
void UiSoundManager::playEating() { playStock("Food", eatingSounds_); }
void UiSoundManager::playDrinking() { playStock("Drinking", drinkingSounds_); }

// Level up
void UiSoundManager::playLevelUp() { playStock("LEVELUP", levelUpSounds_); }

// Achievement
void UiSoundManager::playAchievementAlert() { playStock("AchievementSound", achievementSounds_); }

// Chat notifications
void UiSoundManager::playWhisperReceived() { playStock("TellMessage", whisperSounds_); }

// Minimap ping
void UiSoundManager::playMinimapPing() { playStock("MapPing", minimapPingSounds_); }

} // namespace audio
} // namespace wowee
