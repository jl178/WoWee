#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/music_manager.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/player_voice_manager.hpp"
#include "audio/ambient_sound_manager.hpp"
#include "audio/ui_sound_manager.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/spell_sound_manager.hpp"
#include "audio/movement_sound_manager.hpp"
#include "audio/world_sound_emitters.hpp"
#include "audio/world_fx.hpp"
#include "pipeline/asset_manager.hpp"
#include "audio/day_night.hpp"
#include "game/zone_manager.hpp"
#include "core/logger.hpp"

#include <cstdlib>

namespace wowee {
namespace audio {

AudioCoordinator::AudioCoordinator() = default;

AudioCoordinator::~AudioCoordinator() {
    shutdown();
}

bool AudioCoordinator::initialize() {
    // Initialize AudioEngine (singleton)
    if (!AudioEngine::instance().initialize()) {
        LOG_WARNING("Failed to initialize AudioEngine - audio will be disabled");
        audioAvailable_ = false;
        return false;
    }
    audioAvailable_ = true;

    // Create all audio managers (initialized later with asset manager)
    musicManager_ = std::make_unique<MusicManager>();
    footstepManager_ = std::make_unique<FootstepManager>();
    activitySoundManager_ = std::make_unique<ActivitySoundManager>();
    mountSoundManager_ = std::make_unique<MountSoundManager>();
    npcVoiceManager_ = std::make_unique<NpcVoiceManager>();
    playerVoiceManager_ = std::make_unique<PlayerVoiceManager>();
    ambientSoundManager_ = std::make_unique<AmbientSoundManager>();
    uiSoundManager_ = std::make_unique<UiSoundManager>();
    combatSoundManager_ = std::make_unique<CombatSoundManager>();
    spellSoundManager_ = std::make_unique<SpellSoundManager>();
    movementSoundManager_ = std::make_unique<MovementSoundManager>();
    worldSoundEmitters_ = std::make_unique<WorldSoundEmitters>();
    // The terrain can hand it doodads before the tables are read; it resolves
    // them once soundData_ has loaded.
    worldSoundEmitters_->setSoundData(&soundData_);

    LOG_INFO("AudioCoordinator initialized with ", 12, " audio managers");
    return true;
}
void AudioCoordinator::shutdown() {
    // Reset all managers first (they may reference AudioEngine)
    worldSoundEmitters_.reset();
    movementSoundManager_.reset();
    spellSoundManager_.reset();
    combatSoundManager_.reset();
    uiSoundManager_.reset();
    ambientSoundManager_.reset();
    playerVoiceManager_.reset();
    npcVoiceManager_.reset();
    mountSoundManager_.reset();
    activitySoundManager_.reset();
    footstepManager_.reset();
    musicManager_.reset();

    // Shutdown audio engine last
    if (audioAvailable_) {
        AudioEngine::instance().shutdown();
        audioAvailable_ = false;
    }

    LOG_INFO("AudioCoordinator shutdown complete");
}

void AudioCoordinator::loadSoundData(pipeline::AssetManager* assets) {
    if (soundData_.loaded()) return;
    if (!assets) return;
    if (soundData_.load([assets](const std::string& name) { return assets->loadDBCOptional(name); })) {
        zoneMusic_ = std::make_unique<ZoneMusicController>(soundData_);
        ambienceKey_ = 0;
        if (npcVoiceManager_) npcVoiceManager_->setSoundData(&soundData_);
        if (playerVoiceManager_) playerVoiceManager_->setSoundData(&soundData_);
        if (combatSoundManager_) combatSoundManager_->setSoundData(&soundData_);
        if (worldSoundEmitters_) worldSoundEmitters_->setSoundData(&soundData_);
        // The client looks this one up by name, hence the warning in it.
        const SoundEntry* loop = soundData_.soundByName("Underwater (DONOTRENAME)");
        if (!loop || loop->files.empty()) loop = soundData_.soundByName("UnderWaterLoop");
        if (loop && !loop->files.empty()) {
            underwaterLoopPath_ = loop->files.front();
            underwaterLoopVolume_ = loop->volume;
        }
        reverbPrefId_ = -1;
    }
}

bool AudioCoordinator::playInterfaceEntry(const SoundEntry* entry) {
    if (!audioAvailable_ || !entry || entry->files.empty()) return false;
    float volume = entry->volume;
    if (uiSoundManager_) volume *= uiSoundManager_->getVolumeScale();
    const auto& file = entry->files[static_cast<size_t>(std::rand()) % entry->files.size()];
    return AudioEngine::instance().playSound2D(file, volume, 1.0f, SoundBus::Interface);
}

void AudioCoordinator::playCoinSound() {
    if (playInterfaceEntry(soundData_.loaded() ? soundData_.coinSound() : nullptr)) return;
    // No SoundEntries row: the file that row names.
    if (uiSoundManager_) uiSoundManager_->playLootCoinSmall();
}

bool AudioCoordinator::playItemFoley(uint32_t itemDisplayId, bool pickup) {
    if (!soundData_.loaded()) return false;
    return playInterfaceEntry(soundData_.itemFoleyEntry(itemDisplayId, pickup));
}

void AudioCoordinator::updateReverb(const ZoneAudioContext& ctx) {
    const bool under = ctx.cameraUnderwater;
    AudioEngine::instance().setWorldUnderwater(under);
    if (ambientSoundManager_) {
        ambientSoundManager_->setUnderwaterLoop(underwaterLoopPath_, underwaterLoopVolume_);
        ambientSoundManager_->setSubmerged(under);
    }

    // Which SoundProviderPreferences row. A room or cave - an indoor group -
    // has its WMOAreaTable row's; one with no row of its own gets a small
    // plain room rather than the open air outside. Anywhere else, the area's,
    // which for nearly all of the outdoors is none: the stock data leaves
    // fields and forests dry. Under water the rows' underwater column, and
    // the stock Underwater row where a city's area names none.
    int32_t id = 0;
    if (soundData_.loaded()) {
        if (ctx.wmoRootId != 0 && ctx.wmoInterior) {
            id = soundData_.wmoReverb(ctx.wmoRootId, ctx.wmoNameSet, ctx.wmoGroupIndex, under);
            if (id == 0 && !under) {
                if (const SoundProviderPref* room = soundData_.providerPrefByName("PRESET_ROOM")) {
                    id = static_cast<int32_t>(room->id);
                }
            }
        }
        if (id == 0) id = soundData_.areaReverb(ctx.areaId, under);
        if (id == 0 && under) {
            if (const SoundProviderPref* water = soundData_.providerPrefByName("Underwater")) {
                id = static_cast<int32_t>(water->id);
            }
        }
    }
    if (id == reverbPrefId_ && under == reverbUnderwater_) return;
    reverbPrefId_ = id;
    reverbUnderwater_ = under;

    ReverbSettings reverb;  // dry
    const SoundProviderPref* pref = soundData_.providerPref(id);
    if (pref) reverb = reverbFromProvider(*pref);
    AudioEngine::instance().setWorldReverb(reverb);
    LOG_INFO("Reverb: ", pref ? pref->name : std::string("none"), " (", id, ")",
             under ? " under water" : "", " wet=", reverb.wet, " decay=", reverb.decayTime, "s");
}

void AudioCoordinator::updateTableAmbience(const ZoneAudioContext& ctx) {
    if (!ambientSoundManager_ || ctx.areaId == 0) return;
    // A room with its own WMOAreaTable ambience sounds like itself; any other
    // place, indoors or out, sounds like its area.
    const WmoAreaSounds* room = ctx.wmoRootId
        ? soundData_.wmoArea(ctx.wmoRootId, ctx.wmoNameSet, ctx.wmoGroupIndex) : nullptr;
    const uint32_t roomAmbience = room ? room->ambienceId : 0;
    const uint64_t key = (static_cast<uint64_t>(roomAmbience) << 32) | ctx.areaId;
    if (key == ambienceKey_) return;
    ambienceKey_ = key;
    uint32_t day = 0, night = 0;
    std::string dayPath, nightPath;
    float volume = 1.0f;
    const bool found = roomAmbience ? soundData_.ambience(roomAmbience, day, night)
                                    : soundData_.areaAmbience(ctx.areaId, day, night);
    if (found) {
        if (const SoundEntry* e = soundData_.sound(day); e && !e->files.empty()) {
            dayPath = e->files.front();
            volume = e->volume;
        }
        if (const SoundEntry* e = soundData_.sound(night); e && !e->files.empty()) {
            nightPath = e->files.front();
            if (dayPath.empty()) volume = e->volume;
        }
        // Half the areas name a day loop only; it then runs all night.
        if (nightPath.empty()) nightPath = dayPath;
        if (dayPath.empty()) dayPath = nightPath;
    }
    ambientSoundManager_->setAreaAmbience(dayPath, nightPath, volume);
}

void AudioCoordinator::updateTableMusic(const ZoneAudioContext& ctx, uint32_t zoneId, bool suppressed) {
    if (suppressed) return;
    ZoneMusicInput in;
    in.areaId = ctx.areaId;
    if (ctx.wmoRootId) {
        if (const WmoAreaSounds* room = soundData_.wmoArea(ctx.wmoRootId, ctx.wmoNameSet, ctx.wmoGroupIndex)) {
            in.zoneMusicOverride = room->zoneMusicId;
        }
    }
    in.isDay = isDaytimeHours(ctx.gameTimeHours);
    in.trackPlaying = musicManager_->isPlaying();
    in.trackLoading = musicManager_->isLoading();
    in.deltaTime = ctx.deltaTime;
    // The client's own soundtrack joins the pool only when asked for.
    if (ctx.zoneManager && ctx.zoneManager->getUseOriginalSoundtrack()) {
        if (const auto* info = ctx.zoneManager->getZoneInfo(zoneId)) {
            for (const auto& path : info->musicPaths) {
                if (path.rfind("file:", 0) == 0) in.extraTracks.push_back(path);
            }
        }
    }
    const ZoneMusicAction action = zoneMusic_->update(in);
    switch (action.kind) {
    case ZoneMusicAction::Kind::Play:
        musicManager_->setTrackGain(action.path.rfind("file:", 0) == 0 ? 1.0f : action.volume);
        musicManager_->setLooping(loopMusic_);  // the player's Loop Music setting
        playZoneMusic(action.path);
        LOG_INFO("Zone music: ", action.path, " (set ", zoneMusic_->currentSetId(), ")");
        break;
    case ZoneMusicAction::Kind::Stop:
        musicManager_->stopMusic();
        break;
    case ZoneMusicAction::Kind::None:
        break;
    }
}

void AudioCoordinator::playZoneMusic(const std::string& music) {
    if (music.empty() || !musicManager_) return;
    if (music.rfind("file:", 0) == 0) {
        musicManager_->crossfadeToFile(music.substr(5));
    } else {
        musicManager_->crossfadeTo(music);
    }
}

void AudioCoordinator::setZoneMusicLooping(bool loop) {
    loopMusic_ = loop;
    if (musicManager_) musicManager_->setLooping(loop);
}

void AudioCoordinator::onOriginalSoundtrackDisabled(game::ZoneManager* zm) {
    if (!zm || !musicManager_) return;
    if (!musicManager_->isCurrentTrackFile()) return;
    if (!musicManager_->isPlaying() && !musicManager_->isLoading()) return;
    // Only act in-world with a known zone; the login screen intentionally
    // plays a file track and is not part of the zone rotation this setting
    // controls.
    if (currentZoneId_ == 0) return;

    std::string music = zm->getRandomMusic(currentZoneId_);
    if (!music.empty() && music.rfind("file:", 0) != 0) {
        playZoneMusic(music);
        musicSwitchCooldown_ = 6.0f;
    } else {
        musicManager_->stopMusic();
    }
}

void AudioCoordinator::updateZoneAudio(const ZoneAudioContext& ctx) {
    float deltaTime = ctx.deltaTime;
    // Blows still waiting on an attack animation's $CSS / $CAH event.
    if (combatSoundManager_) combatSoundManager_->update();
    if (musicSwitchCooldown_ > 0.0f) {
        musicSwitchCooldown_ = std::max(0.0f, musicSwitchCooldown_ - deltaTime);
    }

    // Resolve the spatial zone before updating ambience. Zone ambience used to
    // run first, leaving it one zone behind and permanently on its noon default.
    auto* zm = ctx.zoneManager;
    const uint32_t tileZoneId = (zm && ctx.hasTile)
        ? zm->getZoneId(ctx.tileX, ctx.tileY)
        : 0;
    const uint32_t serverZoneId = (zm && ctx.serverZoneId != 0)
        ? zm->resolveAreaZoneId(ctx.serverZoneId)
        : ctx.serverZoneId;
    uint32_t zoneId = serverZoneId != 0 ? serverZoneId : tileZoneId;

    // ── Ambient weather audio sync ──
    if (ambientSoundManager_) {
        bool isBlacksmith = (ctx.insideWmoId == 96048);

        if (zoneId != 0) {
            ambientSoundManager_->setZoneId(zoneId);
        }
        ambientSoundManager_->setGameTime(ctx.gameTimeHours);

        // Map visual weather type to ambient sound weather type
        AmbientSoundManager::WeatherType audioWeatherType = AmbientSoundManager::WeatherType::NONE;
        if (ctx.weatherType == 1) { // RAIN
            if (ctx.weatherIntensity < 0.33f)      audioWeatherType = AmbientSoundManager::WeatherType::RAIN_LIGHT;
            else if (ctx.weatherIntensity < 0.66f)  audioWeatherType = AmbientSoundManager::WeatherType::RAIN_MEDIUM;
            else                                     audioWeatherType = AmbientSoundManager::WeatherType::RAIN_HEAVY;
        } else if (ctx.weatherType == 2) { // SNOW
            if (ctx.weatherIntensity < 0.33f)      audioWeatherType = AmbientSoundManager::WeatherType::SNOW_LIGHT;
            else if (ctx.weatherIntensity < 0.66f)  audioWeatherType = AmbientSoundManager::WeatherType::SNOW_MEDIUM;
            else                                     audioWeatherType = AmbientSoundManager::WeatherType::SNOW_HEAVY;
        }
        ambientSoundManager_->setWeather(audioWeatherType);
        if (soundData_.loaded()) updateTableAmbience(ctx);
        ambientSoundManager_->update(deltaTime, ctx.cameraPosition, ctx.insideWmo, ctx.isSwimming, isBlacksmith);
    }

    // ── The room the world is heard in, and the water over it ──
    updateReverb(ctx);

    // ── Placed emitters and doodad sound events ──
    // Heard from the listener, which stands at the character, the way the
    // stock client measures SoundEntries cutoffs.
    if (worldSoundEmitters_ && soundData_.loaded()) {
        if (ctx.mapId != 0xFFFFFFFFu) worldSoundEmitters_->setMap(ctx.mapId);
        const float ambientScale = ambientSoundManager_ ? ambientSoundManager_->getVolumeScale() : 1.0f;
        worldSoundEmitters_->update(deltaTime, AudioEngine::instance().getListenerPosition(), ambientScale);
    }

    // ── Zone detection and music transitions ──
    if (!zm || !musicManager_ || !ctx.hasTile) return;

    bool insideTavern = false;
    bool insideBlacksmith = false;
    std::string tavernMusic;

    // WMO-based location overrides (taverns, blacksmiths, city zones)
    if (ctx.insideWmo) {
        uint32_t wmoModelId = ctx.insideWmoId;

        // Stormwind WMO → force Stormwind City zone
        if (wmoModelId == 10047) zoneId = 1519;

        // Log WMO transitions
        static uint32_t lastLoggedWmoId = 0;
        if (wmoModelId != lastLoggedWmoId) {
            LOG_INFO("Inside WMO model ID: ", wmoModelId);
            lastLoggedWmoId = wmoModelId;
        }

        // Blacksmith detection (ambient forge sounds)
        if (wmoModelId == 96048) {
            insideBlacksmith = true;
            LOG_INFO("Detected blacksmith WMO ", wmoModelId);
        }

        // Tavern / inn detection, for data without WMOAreaTable rows. With the
        // tables loaded a tavern names its own music there.
        const bool tableKnowsRoom = soundData_.loaded() && ctx.wmoRootId != 0 &&
            soundData_.wmoArea(ctx.wmoRootId, ctx.wmoNameSet, ctx.wmoGroupIndex) != nullptr;
        if (!tableKnowsRoom &&
            (wmoModelId == 191 || wmoModelId == 71414 || wmoModelId == 190 ||
             wmoModelId == 220 || wmoModelId == 221 ||
             wmoModelId == 5392 || wmoModelId == 5393)) {
            insideTavern = true;
            // The first slot is our own tavern track. It falls back to the
            // Blizzard one it replaces when the file is missing, and the
            // original-soundtrack setting drops it the same way zone music does.
            static const std::string tavernRemix =
                game::ZoneManager::resolveOriginalMusicFile("TavernAllianceREMIX.mp3");
            const bool useRemix = !tavernRemix.empty() &&
                (!ctx.zoneManager || ctx.zoneManager->getUseOriginalSoundtrack());
            const std::vector<std::string> tavernTracks = {
                useRemix ? tavernRemix
                         : std::string("Sound\\Music\\ZoneMusic\\TavernAlliance\\TavernAlliance01.mp3"),
                "Sound\\Music\\ZoneMusic\\TavernAlliance\\TavernAlliance02.mp3",
                "Sound\\Music\\ZoneMusic\\TavernHuman\\RA_HumanTavern1A.mp3",
                "Sound\\Music\\ZoneMusic\\TavernHuman\\RA_HumanTavern2A.mp3",
            };
            static int tavernTrackIndex = 0;
            tavernMusic = tavernTracks[tavernTrackIndex++ % tavernTracks.size()];
            LOG_INFO("Detected tavern WMO ", wmoModelId, ", playing: ", tavernMusic);
        }
    }

    // Tavern music transitions
    if (insideTavern) {
        if (!inTavern_ && !tavernMusic.empty()) {
            inTavern_ = true;
            LOG_INFO("Entered tavern");
            musicManager_->playMusic(tavernMusic, true);
            musicSwitchCooldown_ = 6.0f;
        }
    } else if (inTavern_) {
        inTavern_ = false;
        LOG_INFO("Exited tavern");
        auto* info = zm->getZoneInfo(currentZoneId_);
        if (info) {
            std::string music = zm->getRandomMusic(currentZoneId_);
            if (!music.empty()) {
                playZoneMusic(music);
                musicSwitchCooldown_ = 6.0f;
            }
        }
    }

    // Blacksmith transitions (stop music, let ambience play)
    if (insideBlacksmith) {
        if (!inBlacksmith_) {
            inBlacksmith_ = true;
            LOG_INFO("Entered blacksmith - stopping music");
            musicManager_->stopMusic();
        }
    } else if (inBlacksmith_) {
        inBlacksmith_ = false;
        LOG_INFO("Exited blacksmith - restoring music");
        auto* info = zm->getZoneInfo(currentZoneId_);
        if (info) {
            std::string music = zm->getRandomMusic(currentZoneId_);
            if (!music.empty()) {
                playZoneMusic(music);
                musicSwitchCooldown_ = 6.0f;
            }
        }
    }

    // Zone music from the client's tables. The tavern and blacksmith
    // overrides above still win while the player is inside one.
    if (zoneMusic_) {
        if (zoneId != currentZoneId_ && zoneId != 0) {
            currentZoneId_ = zoneId;
            if (const auto* area = soundData_.area(zoneId)) currentZoneName_ = area->name;
            else if (const auto* info = zm->getZoneInfo(zoneId)) currentZoneName_ = info->name;
            LOG_INFO("Entered zone: ", currentZoneName_);
        }
        updateTableMusic(ctx, zoneId, insideTavern || insideBlacksmith || inTavern_ || inBlacksmith_);
        musicManager_->update(deltaTime);
        return;
    }

    // Normal zone transitions
    if (!insideTavern && !insideBlacksmith && zoneId != currentZoneId_ && zoneId != 0) {
        currentZoneId_ = zoneId;
        auto* info = zm->getZoneInfo(zoneId);
        if (info) {
            currentZoneName_ = info->name;
            LOG_INFO("Entered zone: ", info->name);
            if (musicSwitchCooldown_ <= 0.0f) {
                std::string music = zm->getRandomMusic(zoneId);
                if (!music.empty()) {
                    playZoneMusic(music);
                    musicSwitchCooldown_ = 6.0f;
                }
            }
        }
    }

    musicManager_->update(deltaTime);

    // When a track finishes, pick a new random track from the current zone
    if (!musicManager_->isPlaying() && !inTavern_ && !inBlacksmith_ &&
        currentZoneId_ != 0 && musicSwitchCooldown_ <= 0.0f) {
        std::string music = zm->getRandomMusic(currentZoneId_);
        if (!music.empty()) {
            playZoneMusic(music);
            musicSwitchCooldown_ = 2.0f;
        }
    }
}

} // namespace audio
} // namespace wowee
