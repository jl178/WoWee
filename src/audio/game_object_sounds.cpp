#include "audio/game_object_sounds.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sound_data.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/logger.hpp"

#include <glm/gtc/matrix_inverse.hpp>

namespace wowee {
namespace audio {

void GameObjectSounds::loadDisplaySounds(pipeline::AssetManager* assets) {
    if (!assets) return;
    auto dbc = assets->loadDBC("GameObjectDisplayInfo.dbc");
    if (!dbc || !dbc->isLoaded()) return;
    const auto* layout = pipeline::getActiveDBCLayout()
                             ? pipeline::getActiveDBCLayout()->getLayout("GameObjectDisplayInfo")
                             : nullptr;
    const uint32_t idField = layout ? (*layout)["ID"] : 0;
    // ID, ModelName, Sound[10]: the same columns in every expansion's file.
    constexpr uint32_t kFirstSound = 2;
    size_t withSound = 0;
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        std::array<uint32_t, 10> s{};
        bool any = false;
        for (uint32_t n = 0; n < 10; ++n) {
            s[n] = dbc->getUInt32(i, kFirstSound + n);
            any = any || s[n] != 0;
        }
        if (!any) continue;
        displaySounds_[dbc->getUInt32(i, idField)] = s;
        ++withSound;
    }
    LOG_INFO("GameObjectSounds: ", withSound, " displays with sounds");
}

uint32_t GameObjectSounds::displaySound(uint32_t displayId, int slot) const {
    if (slot < 0 || slot >= 10) return 0;
    auto it = displaySounds_.find(displayId);
    return it == displaySounds_.end() ? 0 : it->second[static_cast<size_t>(slot)];
}

void GameObjectSounds::addObject(uint32_t instanceId, uint32_t displayId) {
    auto& e = emitters_[instanceId];
    stop(e, 0.0f);
    e = Emitter{};
    e.displayId = displayId;
}

void GameObjectSounds::addDoodad(uint32_t instanceId) {
    auto& e = emitters_[instanceId];
    stop(e, 0.0f);
    e = Emitter{};
    e.doodad = true;
}

void GameObjectSounds::remove(uint32_t instanceId) {
    auto it = emitters_.find(instanceId);
    if (it == emitters_.end()) return;
    stop(it->second, 0.0f);
    emitters_.erase(it);
}

void GameObjectSounds::stopLoop(uint32_t instanceId) {
    auto it = emitters_.find(instanceId);
    if (it != emitters_.end()) stop(it->second, it->second.doodad ? 0.0f : kObjectDslFadeMs);
}

void GameObjectSounds::clear() {
    for (auto& [id, e] : emitters_) stop(e, 0.0f);
    emitters_.clear();
}

bool GameObjectSounds::playing(uint32_t handle) const {
    if (handle == 0) return false;
    if (playingQuery_) return playingQuery_(handle);
    return AudioEngine::instance().isSoundPlaying(handle);
}

bool GameObjectSounds::sameLoopNearby(uint32_t soundId, const glm::vec3& position) const {
    constexpr float r2 = kObjectLoopDedupeYards * kObjectLoopDedupeYards;
    for (const auto& [id, e] : emitters_) {
        if (e.loopSoundId != soundId || !playing(e.loopHandle)) continue;
        const glm::vec3 d = e.loopPosition - position;
        if (glm::dot(d, d) < r2) return true;
    }
    return false;
}

void GameObjectSounds::stop(Emitter& e, float fadeOutMs) {
    if (e.loopHandle != 0) {
        if (!recorder_) {
            auto& engine = AudioEngine::instance();
            if (fadeOutMs > 0.0f) engine.fadeOutSound(e.loopHandle, fadeOutMs);
            else engine.stopSound(e.loopHandle);
        }
    }
    e.loopHandle = 0;
    e.loopSoundId = 0;
    e.loopLocalSet = false;
}

void GameObjectSounds::playLoop(uint32_t instanceId, Emitter& e, uint32_t soundId,
                                const glm::vec3& position, float fadeInMs) {
    // One handle: while it sounds, the next lap's event plays nothing.
    if (playing(e.loopHandle)) return;
    if (sameLoopNearby(soundId, position)) return;
    uint32_t handle = 0;
    if (recorder_) {
        handle = recorder_({instanceId, soundId, true, fadeInMs, position});
    } else {
        const SoundEntry* entry = soundData_ ? soundData_->sound(soundId) : nullptr;
        if (!entry) return;
        KitPlay play;
        play.position = &position;
        play.keepLoop = true;
        play.opts.randomStart = true;
        play.opts.fadeInMs = fadeInMs;
        handle = AudioEngine::instance().playKit(*soundData_, *entry, play);
    }
    if (handle == 0) return;
    e.loopHandle = handle;
    e.loopSoundId = soundId;
    e.loopPosition = position;
    e.loopLocalSet = false;
}

void GameObjectSounds::playOnce(uint32_t instanceId, uint32_t soundId, const glm::vec3& position) {
    if (recorder_) {
        recorder_({instanceId, soundId, false, 0.0f, position});
        return;
    }
    const SoundEntry* entry = soundData_ ? soundData_->sound(soundId) : nullptr;
    if (!entry) return;
    KitPlay play;
    play.position = &position;
    AudioEngine::instance().playKit(*soundData_, *entry, play);
}

void GameObjectSounds::onEvent(uint32_t instanceId, uint32_t tag, uint32_t data,
                               const glm::vec3& position) {
    auto it = emitters_.find(instanceId);
    if (it == emitters_.end()) return;
    Emitter& e = it->second;
    using pipeline::m2EventTag;
    const bool isLoop = tag == m2EventTag("$DSL");
    const bool isOnce = tag == m2EventTag("$DSO") || (!e.doodad && tag == m2EventTag("$SND"));
    if (e.doodad) {
        // 0x7bd5a0: $DSL, $DSE, $DSO.
        if (isLoop) playLoop(instanceId, e, data, position, kDoodadDslFadeInMs);
        else if (tag == m2EventTag("$DSE")) stop(e, 0.0f);
        else if (isOnce) playOnce(instanceId, data, position);
        return;
    }
    // 0x70c050.
    if (isLoop) {
        playLoop(instanceId, e, data, position, kObjectDslFadeMs);
        return;
    }
    if (isOnce) {
        playOnce(instanceId, data, position);
        return;
    }
    const int slot = pipeline::gameObjectSoundSlot(tag);
    if (slot < 0) return;
    const uint32_t soundId = displaySound(e.displayId, slot);
    if (soundId == 0) return;
    // 0x70bf70: a looping row on the handle, anything else once.
    bool loops = false;
    if (loopQuery_) {
        loops = loopQuery_(soundId);
    } else {
        const SoundEntry* entry = soundData_ ? soundData_->sound(soundId) : nullptr;
        if (!entry) return;
        loops = (entry->flags & stock::kFlagLoop) != 0;
    }
    if (loops) playLoop(instanceId, e, soundId, position, 0.0f);
    else playOnce(instanceId, soundId, position);
}

void GameObjectSounds::update(const std::function<bool(uint32_t, glm::mat4&)>& matrixOf) {
    for (auto& [id, e] : emitters_) {
        if (e.loopHandle == 0) continue;
        if (!playing(e.loopHandle)) {
            e.loopHandle = 0;
            e.loopSoundId = 0;
            continue;
        }
        if (!e.doodad) continue;  // a game object's model stands where it is
        glm::mat4 m(1.0f);
        if (!matrixOf || !matrixOf(id, m)) continue;
        if (!e.loopLocalSet) {
            e.loopLocal = glm::vec3(glm::inverse(m) * glm::vec4(e.loopPosition, 1.0f));
            e.loopLocalSet = true;
            continue;
        }
        const glm::vec3 now = glm::vec3(m * glm::vec4(e.loopLocal, 1.0f));
        if (now != e.loopPosition) {
            e.loopPosition = now;
            if (!recorder_) AudioEngine::instance().setSoundPosition(e.loopHandle, now);
        }
    }
}

} // namespace audio
} // namespace wowee
