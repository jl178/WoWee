#include "audio/audio_coordinator.hpp"
#include "addons/lua_api_registrations.hpp"
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
#include <random>

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

    // The lookup points into soundData_, which goes with this coordinator.
    AudioEngine::instance().setAdvancedLookup(nullptr);
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
        // Every kit the engine starts finds its SoundEntriesAdvanced row
        // here: the stock instance behaviour applies to whoever plays it.
        AudioEngine::instance().setAdvancedLookup(
            [this](uint32_t kitId) { return soundData_.advancedKit(kitId); });
        AudioEngine::instance().setKitNameLookup([this](uint32_t kitId) -> std::string_view {
            const SoundEntry* e = soundData_.sound(kitId);
            return e ? std::string_view(e->name) : std::string_view();
        });
        if (npcVoiceManager_) npcVoiceManager_->setSoundData(&soundData_);
        if (playerVoiceManager_) playerVoiceManager_->setSoundData(&soundData_);
        if (combatSoundManager_) combatSoundManager_->setSoundData(&soundData_);
        if (worldSoundEmitters_) worldSoundEmitters_->setSoundData(&soundData_);
        if (uiSoundManager_) uiSoundManager_->setSoundData(&soundData_);
        // The character's own steps now come from its footfall events and
        // FootstepTerrainLookup, as every unit's do.
        if (footstepManager_) footstepManager_->setOwnStepsFromTables(true);
        if (movementSoundManager_) movementSoundManager_->setWaterStepsFromTables(true);
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
    static std::mt19937 rng{std::random_device{}()};
    const float scale = uiSoundManager_ ? uiSoundManager_->getVolumeScale() : 1.0f;
    const std::string* file = soundData_.nextFile(*entry);
    if (!file) return false;
    SoundOptions opts;
    opts.kitId = entry->id;
    opts.noDuplicate = (entry->flags & stock::kFlagNoDuplicate) != 0;
    return AudioEngine::instance().playSound2D(*file, stock::kitVolume(entry->volume, entry->flags, scale, rng),
                                               stock::kitPitch(entry->flags, rng), SoundBus::Interface, opts);
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
        // With the tables the underwater loop is an ambience slot of its own
        // (updateTableAmbience); the separate loop is for data without them.
        if (!soundData_.loaded()) {
            ambientSoundManager_->setUnderwaterLoop(underwaterLoopPath_, underwaterLoopVolume_);
            ambientSoundManager_->setSubmerged(under);
        }
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
            id = soundData_.wmoReverb(ctx.wmoRootId, ctx.wmoNameSet, ctx.wmoGroupId, under);
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
    // Dry unless Sound_EnableReverb is on, which stock leaves off by default
    // (the Sound options' Enable Reverb box).
    if (addons::storedCVarValue("Sound_EnableReverb", "0") == "0") id = 0;
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

void AudioCoordinator::setScreenEffectSounds(uint32_t soundAmbienceId, uint32_t zoneMusicId) {
    if (soundAmbienceId == screenEffectAmbience_ && zoneMusicId == screenEffectMusic_) return;
    screenEffectAmbience_ = soundAmbienceId;
    screenEffectMusic_ = zoneMusicId;
    ambienceKey_ = 0;  // choose the ambience again on the next update
    LOG_INFO("Screen effect sounds: ambience ", soundAmbienceId, ", music ", zoneMusicId);
}

void AudioCoordinator::updateTableAmbience(const ZoneAudioContext& ctx) {
    if (!ambientSoundManager_) return;
    // "Rain heard indoors": under a roof, where slot 8 is passed over, the
    // weather's own loop at a quarter of its level beside the room's. Not
    // under water or in the ghost world, which replace every other loop.
    {
        std::string roofPath;
        float roofVolume = 0.0f;
        const bool roofed = ctx.rainIndoors && ctx.wmoRootId != 0 && ctx.wmoSoundIndoors &&
                            !ctx.cameraUnderwater && screenEffectAmbience_ == 0;
        if (roofed) {
            if (const uint32_t weather = soundData_.weatherSound(ctx.weatherState)) {
                if (const SoundEntry* e = soundData_.sound(weather); e && !e->files.empty()) {
                    roofPath = e->files.front();
                    roofVolume = e->volume * 0.25f;
                }
            }
        }
        ambientSoundManager_->setRoofRain(roofPath, roofVolume);
    }
    // The screen effect's loop is the top slot (0x4c86f0 walks 10 down to
    // 0): the ghost world sounds like GhostState wherever it is.
    if (screenEffectAmbience_ != 0) {
        const uint64_t key = (1ull << 62) | screenEffectAmbience_;
        if (key == ambienceKey_) return;
        uint32_t day = 0, night = 0;
        std::string dayPath, nightPath;
        float volume = 1.0f;
        if (soundData_.ambience(screenEffectAmbience_, day, night)) {
            if (const SoundEntry* e = soundData_.sound(day); e && !e->files.empty()) {
                dayPath = e->files.front();
                volume = e->volume;
            }
            if (const SoundEntry* e = soundData_.sound(night); e && !e->files.empty()) {
                nightPath = e->files.front();
            }
            if (nightPath.empty()) nightPath = dayPath;
            if (dayPath.empty()) dayPath = nightPath;
        }
        if (dayPath.empty()) return;  // keep the area's until the row resolves
        ambienceKey_ = key;
        ambientSoundManager_->setAreaAmbience(dayPath, nightPath, volume);
        return;
    }
    // Under water the camera hears SoundEntries 4209 "Underwater (DONOTRENAME)"
    // alone: slot 9 (0x5fe7b0 -> 0x4c8630 sets slot 9's ambience to 4209
    // while the camera is in liquid), above the weather and the area, and the
    // change cuts over at once instead of crossfading (0x4c9910 passes no
    // fade while 0xb4aa9c, "the underwater state changed", is set).
    const bool under = ctx.cameraUnderwater && !underwaterLoopPath_.empty();
    const bool underChanged = under != ambienceUnderwater_;
    ambienceUnderwater_ = under;
    if (under) {
        const uint64_t key = (1ull << 60);
        if (key == ambienceKey_) return;
        ambienceKey_ = key;
        ambientSoundManager_->setAreaAmbience(underwaterLoopPath_, underwaterLoopPath_, underwaterLoopVolume_,
                                              /*instant=*/true);
        return;
    }
    // The weather's loop, Weather.dbc AmbienceID by SMSG_WEATHER's state, is
    // slot 8 (SMSG_WEATHER 0x526530 -> 0x4c8610 writes slot 8's day and night
    // ambience): above the area's, so rain replaces the forest with the same
    // 5 s crossfade, and passed over indoors (0x4c86f0 skips slot 8 while
    // 0x4c8600's flag, set from the area resolution's indoor test, is up).
    const bool interior = ctx.wmoRootId != 0 && ctx.wmoSoundIndoors;
    if (const uint32_t weather = interior ? 0 : soundData_.weatherSound(ctx.weatherState)) {
        const uint64_t key = (1ull << 61) | weather;
        if (key == ambienceKey_) return;
        const SoundEntry* e = soundData_.sound(weather);
        if (e && !e->files.empty()) {
            ambienceKey_ = key;
            ambientSoundManager_->setAreaAmbience(e->files.front(), e->files.front(), e->volume, underChanged);
            return;
        }
    }
    if (ctx.areaId == 0) return;
    // A room with its own WMOAreaTable ambience sounds like itself, a room in
    // a building whose -1 row names one like the building. Outdoors, and in
    // an outdoor group such as a city street, the area's. Indoors the stock
    // client asks WMOAreaTable alone (Wow.exe 0x78e9a0): a room whose rows
    // name no ambience has none, and the area's loop fades out at the door.
    const uint32_t roomAmbience = ctx.wmoRootId
        ? soundData_.wmoAmbience(ctx.wmoRootId, ctx.wmoNameSet, ctx.wmoGroupId) : 0;
    // "Indoors" is the stock sound code's: the group lacks the exterior flag
    // (0x7a1480), which is not the same as having the indoor one.
    const bool indoorsBare = roomAmbience == 0 && ctx.wmoRootId != 0 && ctx.wmoSoundIndoors;
    const uint64_t key = (static_cast<uint64_t>(roomAmbience) << 32) | ctx.areaId |
                         (indoorsBare ? (1ull << 63) : 0ull);
    if (key == ambienceKey_) return;
    ambienceKey_ = key;
    uint32_t day = 0, night = 0;
    std::string dayPath, nightPath;
    float volume = 1.0f;
    const bool found = roomAmbience ? soundData_.ambience(roomAmbience, day, night)
                     : indoorsBare  ? false
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
    ambientSoundManager_->setAreaAmbience(dayPath, nightPath, volume, underChanged);
}

void AudioCoordinator::updateTableMusic(const ZoneAudioContext& ctx, uint32_t zoneId, bool suppressed) {
    if (suppressed) return;
    ZoneMusicInput in;
    in.areaId = ctx.areaId;
    if (ctx.wmoRootId) {
        in.zoneMusicOverride = soundData_.wmoZoneMusic(ctx.wmoRootId, ctx.wmoNameSet, ctx.wmoGroupId);
        in.introOverride = soundData_.wmoIntro(ctx.wmoRootId, ctx.wmoNameSet, ctx.wmoGroupId);
        in.indoors = ctx.wmoSoundIndoors;
    }
    in.screenEffectMusic = screenEffectMusic_;
    in.noDelay = loopMusic_;
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
        // A zone track plays once; Loop Music means no silence before the
        // next one (Sound_ZoneMusicNoDelay), not the same track again.
        musicManager_->setLooping(false);
        playZoneMusic(action.path, action.fadeOutSeconds * 1000.0f);
        LOG_INFO("Zone music: ", action.path, " (set ", zoneMusic_->currentSetId(), ")");
        break;
    case ZoneMusicAction::Kind::Stop:
        musicManager_->stopMusic(action.fadeOutSeconds * 1000.0f);
        break;
    case ZoneMusicAction::Kind::None:
        break;
    }
}

void AudioCoordinator::playZoneMusic(const std::string& music, float fadeMs) {
    if (music.empty() || !musicManager_) return;
    if (music.rfind("file:", 0) == 0) {
        musicManager_->crossfadeToFile(music.substr(5), fadeMs);
    } else {
        musicManager_->crossfadeTo(music, fadeMs);
    }
}

void AudioCoordinator::setZoneMusicLooping(bool loop) {
    // Sound_ZoneMusicNoDelay: read by the schedule on the next track end.
    loopMusic_ = loop;
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
    // The clock the advanced rows' time windows read (0x4ca7b0's game time).
    AudioEngine::instance().setTimeOfDayMs(static_cast<int32_t>(ctx.gameTimeHours * 3600000.0f));
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
        // The hand-made forge loop is for data without the tables: with them
        // a smithy sounds like its area and its forge and anvil doodads.
        bool isBlacksmith = !soundData_.loaded() && (ctx.insideWmoId == 96048);

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
            soundData_.wmoArea(ctx.wmoRootId, ctx.wmoNameSet, ctx.wmoGroupId) != nullptr;
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
