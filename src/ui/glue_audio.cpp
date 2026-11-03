#include "ui/glue_audio.hpp"

#include "audio/ambient_sound_manager.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sample_load.hpp"
#include "audio/sound_data.hpp"
#include "audio/stock_sound_rules.hpp"
#include "audio/ui_sound_manager.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "game/glue_rules.hpp"
#include "pipeline/asset_manager.hpp"

#include <vector>

namespace wowee {
namespace ui {
namespace glue_audio {

namespace {

struct Sample {
    std::string path;
    std::vector<uint8_t> data;
    bool loaded = false;
};

struct State {
    std::string ambienceName;
    uint32_t ambienceId = 0;
    bool failed = false;  ///< ambienceName could not be played; not retried
};

State& state() {
    static State s;
    return s;
}

/// The coordinator with SoundEntries read and the interface sounds ready.
audio::AudioCoordinator* ready() {
    auto& app = core::Application::getInstance();
    auto* ac = app.getAudioCoordinator();
    auto* assets = app.getAssetManager();
    if (!ac || !assets || !assets->isInitialized()) return nullptr;
    audio::AudioEngine::instance().setAssetManager(assets);
    ac->loadSoundData(assets);  // once; the world asks again and gets the same
    if (auto* ui = ac->getUiSoundManager(); ui && !ui->isInitialized()) {
        ui->initialize(assets);
        if (const auto* data = ac->getSoundData()) ui->setSoundData(data);
    }
    return ac;
}

} // namespace

void play(std::string_view soundName) {
    auto* ac = ready();
    if (!ac) return;
    if (auto* ui = ac->getUiSoundManager()) {
        if (!ui->playByName(std::string(soundName)))
            LOG_DEBUG("glue_audio: no sound ", soundName);
    }
}

void playAmbience(std::string_view soundName, float fadeInSeconds) {
    auto& st = state();
    auto& engine = audio::AudioEngine::instance();
    if (st.ambienceName == soundName &&
        (st.failed || (st.ambienceId != 0 && engine.isSoundPlaying(st.ambienceId)))) {
        return;
    }
    auto* ac = ready();
    if (!ac) return;
    // The one playing goes over 3 s (0x9ebbc4), even when nothing replaces it.
    if (st.ambienceId != 0) {
        engine.fadeOutSound(st.ambienceId, 3000.0f);
        st.ambienceId = 0;
    }
    st.ambienceName = std::string(soundName);
    st.failed = true;
    const auto* data = ac->getSoundData();
    const audio::SoundEntry* entry = data ? data->soundByName(st.ambienceName) : nullptr;
    if (!entry || entry->files.empty()) return;
    const std::string* file = data->nextFile(*entry);
    if (!file) return;
    static Sample sample;
    if (sample.path != *file || !sample.loaded) {
        audio::loadSampleFile(*file, sample, core::Application::getInstance().getAssetManager(), "glue_audio");
    }
    if (!sample.loaded) return;
    float scale = 1.0f;
    if (auto* ambient = ac->getAmbientSoundManager()) scale = ambient->getVolumeScale();
    // An ambience loop: props type 2 (AMBIENCE), looping, faded in.
    audio::SoundOptions opts;
    opts.category = audio::stock::Category::Ambience;
    opts.priority = audio::stock::kPriorityZone;
    opts.kitId = entry->id;
    st.ambienceId = engine.playSound2DStoppable(sample.data, entry->volume * scale, /*loop=*/true,
                                                fadeInSeconds * 1000.0f, audio::SoundBus::Interface, opts);
    st.failed = (st.ambienceId == 0);
    LOG_INFO("glue_audio: ambience ", st.ambienceName, " (", *file, ")");
}

void playSceneAmbience(std::string_view sceneName) {
    const std::string_view track = game::glue::ambienceForScene(game::glue::sceneKey(sceneName));
    if (!track.empty()) playAmbience(track, game::glue::kAmbienceFadeInSeconds);
}

void stopAmbience(float fadeSeconds) {
    auto& st = state();
    if (st.ambienceId != 0) audio::AudioEngine::instance().fadeOutSound(st.ambienceId, fadeSeconds * 1000.0f);
    st.ambienceId = 0;
    st.ambienceName.clear();
    st.failed = false;
}

} // namespace glue_audio
} // namespace ui
} // namespace wowee
