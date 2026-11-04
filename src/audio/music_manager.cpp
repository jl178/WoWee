#include "audio/music_manager.hpp"

#include <chrono>
#include "audio/audio_engine.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/thread_pool.hpp"
#include "core/hitch_legacy.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace wowee {
namespace audio {

MusicManager::MusicManager() = default;

MusicManager::~MusicManager() {
    shutdown();
}

bool MusicManager::initialize(pipeline::AssetManager* assets) {
    assetManager = assets;
    LOG_INFO("Music manager initialized");
    return true;
}

float MusicManager::effectiveMusicVolume() const {
    float vol = volumePercent / 100.0f * trackGain_;
    if (underwaterMode) {
        vol *= 0.3f;
    }
    return vol;
}

void MusicManager::shutdown() {
    cancelPendingFileLoad();
    for (auto& f : retiredArchiveReads_) {
        if (f.valid()) f.wait();
    }
    retiredArchiveReads_.clear();
    AudioEngine::instance().stopMusic();
    playing = false;
    fadingIn = false;
    fadingOut = false;
    fadeInTimer = 0.0f;
    fadeInDuration = 0.0f;
    fadeInTargetVolume = 0.0f;
    currentTrack.clear();
    currentTrackIsFile = false;
    musicDataCache_.clear();
    assetManager = nullptr;
}

void MusicManager::preloadMusic(const std::string& mpqPath) {
    if (!assetManager || mpqPath.empty()) return;
    if (musicDataCache_.find(mpqPath) != musicDataCache_.end()) return;

    auto data = assetManager->readFile(mpqPath);
    if (!data.empty()) {
        musicDataCache_[mpqPath] =
            std::make_shared<const std::vector<uint8_t>>(std::move(data));
    }
}

void MusicManager::playMusic(const std::string& mpqPath, bool loop, float fadeInMs) {
    if (!assetManager) return;
    if (mpqPath == currentTrack && playing) return;

    // Check if AudioEngine is ready
    if (!AudioEngine::instance().isInitialized()) {
        LOG_WARNING("Music: AudioEngine not initialized");
        return;
    }

    // A newer request always wins, regardless of whether it comes from an archive
    // or the filesystem. Pool-backed futures can be discarded without joining.
    cancelPendingFileLoad();

    // Read music file from cache or MPQ
    auto cacheIt = musicDataCache_.find(mpqPath);
    if (cacheIt == musicDataCache_.end() && core::hitchLegacy()) {
        preloadMusic(mpqPath);
        cacheIt = musicDataCache_.find(mpqPath);
    }
    if (cacheIt == musicDataCache_.end()) {
        // On a worker, as playFilePath reads a file: a zone's track is a few
        // megabytes inflated out of the archive, and reading it here was the
        // frame's stall on every zone line crossed. pollPendingFileLoad caches
        // it and comes back here when it lands; the track fades in then, as a
        // file track always has.
        if (pendingFileLoad_ && pendingFileLoad_->fromArchive && pendingFileLoad_->path == mpqPath) {
            pendingFileLoad_->loop = loop;
            pendingFileLoad_->fadeInMs = fadeInMs;
            return;
        }
        auto* am = assetManager;
        pendingFileLoad_ = PendingFileLoad{
            core::ThreadPool::ioWorkers().submit([am, mpqPath]()
                    -> std::shared_ptr<const std::vector<uint8_t>> {
                auto data = am->readFile(mpqPath);
                if (data.empty()) return nullptr;
                return std::make_shared<const std::vector<uint8_t>>(std::move(data));
            }),
            mpqPath, loop, fadeInMs, /*fromArchive=*/true
        };
        return;
    }
    if (cacheIt == musicDataCache_.end() || !cacheIt->second || cacheIt->second->empty()) {
        LOG_WARNING("Music: Could not read: ", mpqPath);
        return;
    }

    // Play with AudioEngine (non-blocking, streams from memory)
    float targetVolume = effectiveMusicVolume();
    float startVolume = (fadeInMs > 0.0f) ? 0.0f : targetVolume;
    if (AudioEngine::instance().playMusic(cacheIt->second, startVolume, loop)) {
        playing = true;
        fadingIn = false;
        if (fadeInMs > 0.0f) {
            fadingIn = true;
            fadeInTimer = 0.0f;
            fadeInDuration = std::max(0.05f, fadeInMs / 1000.0f);
            fadeInTargetVolume = targetVolume;
            AudioEngine::instance().setMusicVolume(0.0f);
        }
        currentTrack = mpqPath;
        currentTrackIsFile = false;
        LOG_INFO("Music: Playing ", mpqPath);
    } else {
        LOG_ERROR("Music: Failed to play music via AudioEngine");
    }
}

void MusicManager::playFilePath(const std::string& filePath, bool loop, float fadeInMs) {
    if (filePath.empty()) return;
    if (filePath == currentTrack && playing) return;
    if (!std::filesystem::exists(filePath)) {
        LOG_WARNING("Music: file not found: ", filePath);
        return;
    }

    // Check if AudioEngine is ready
    if (!AudioEngine::instance().isInitialized()) {
        LOG_WARNING("Music: AudioEngine not initialized");
        return;
    }

    // Reuse an in-flight read of the same file while honoring the newest playback
    // options. A different request replaces it without waiting for the old read.
    if (pendingFileLoad_ && pendingFileLoad_->path == filePath) {
        pendingFileLoad_->loop = loop;
        pendingFileLoad_->fadeInMs = fadeInMs;
        return;
    }

    // Read the track on a worker. A login track is ~6MB, and reading it here stalled
    // the render frame that asked for it. pollPendingFileLoad() picks it up from
    // update(), so every AudioEngine call still happens on the main thread.
    pendingFileLoad_ = PendingFileLoad{
        core::ThreadPool::ioWorkers().submit([filePath]()
                -> std::shared_ptr<const std::vector<uint8_t>> {
            std::ifstream file(filePath, std::ios::binary | std::ios::ate);
            if (!file) return nullptr;
            std::streamsize size = file.tellg();
            if (size <= 0) return nullptr;
            file.seekg(0, std::ios::beg);
            std::vector<uint8_t> bytes(static_cast<size_t>(size));
            if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) return nullptr;
            return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
        }),
        filePath, loop, fadeInMs
    };
}

void MusicManager::cancelPendingFileLoad() {
    // Futures returned by ThreadPool own only a shared packaged task state; unlike
    // std::async futures, destroying one never joins the worker.
    retiredArchiveReads_.erase(
        std::remove_if(retiredArchiveReads_.begin(), retiredArchiveReads_.end(),
                       [](const auto& f) {
                           return !f.valid() ||
                                  f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                       }),
        retiredArchiveReads_.end());
    if (pendingFileLoad_ && pendingFileLoad_->fromArchive && pendingFileLoad_->future.valid()) {
        retiredArchiveReads_.push_back(std::move(pendingFileLoad_->future));
    }
    pendingFileLoad_.reset();
}

void MusicManager::pollPendingFileLoad() {
    if (!pendingFileLoad_) return;
    auto& pending = *pendingFileLoad_;
    if (!pending.future.valid()) { pendingFileLoad_.reset(); return; }
    if (pending.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;

    auto data = pending.future.get();
    const std::string filePath = pending.path;
    const bool loop = pending.loop;
    const float fadeInMs = pending.fadeInMs;
    const bool fromArchive = pending.fromArchive;
    pendingFileLoad_.reset();

    if (fromArchive) {
        if (!data || data->empty()) {
            LOG_WARNING("Music: Could not read: ", filePath);
            return;
        }
        musicDataCache_[filePath] = std::move(data);
        playMusic(filePath, loop, fadeInMs);
        return;
    }

    if (!data || data->empty()) {
        LOG_ERROR("Music: Could not read file: ", filePath);
        return;
    }

    // Play with AudioEngine
    float targetVolume = effectiveMusicVolume();
    float startVolume = (fadeInMs > 0.0f) ? 0.0f : targetVolume;
    if (AudioEngine::instance().playMusic(std::move(data), startVolume, loop)) {
        playing = true;
        fadingIn = false;
        if (fadeInMs > 0.0f) {
            fadingIn = true;
            fadeInTimer = 0.0f;
            fadeInDuration = std::max(0.05f, fadeInMs / 1000.0f);
            fadeInTargetVolume = targetVolume;
            AudioEngine::instance().setMusicVolume(0.0f);
        }
        currentTrack = filePath;
        currentTrackIsFile = true;
        LOG_INFO("Music: Playing file ", filePath);
    } else {
        LOG_ERROR("Music: Failed to play music via AudioEngine");
    }
}

void MusicManager::stopMusic(float fadeMs) {
    cancelPendingFileLoad();
    fadingIn = false;
    if (!playing) return;

    if (fadeMs > 0.0f) {
        // Begin fade-out; actual stop happens once volume reaches zero in update()
        fadingOut = true;
        fadeOutTimer = 0.0f;
        fadeOutDuration = fadeMs / 1000.0f;
        fadeOutStartVolume = effectiveMusicVolume();
    } else {
        AudioEngine::instance().stopMusic();
        playing = false;
        fadingOut = false;
        currentTrack.clear();
        currentTrackIsFile = false;
    }
}

void MusicManager::setTrackGain(float gain) {
    gain = std::clamp(gain, 0.0f, 1.0f);
    if (trackGain_ == gain) return;
    trackGain_ = gain;
    AudioEngine::instance().setMusicVolume(effectiveMusicVolume());
}

void MusicManager::setVolume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (volumePercent == volume) return;
    volumePercent = volume;

    // Update AudioEngine music volume directly (no restart needed!)
    float vol = effectiveMusicVolume();
    if (fadingIn) {
        fadeInTargetVolume = vol;
        float t = std::clamp(fadeInTimer / std::max(fadeInDuration, 0.001f), 0.0f, 1.0f);
        AudioEngine::instance().setMusicVolume(fadeInTargetVolume * t);
    } else {
        AudioEngine::instance().setMusicVolume(vol);
    }
}

void MusicManager::setUnderwaterMode(bool underwater) {
    if (underwaterMode == underwater) return;
    underwaterMode = underwater;

    // Apply volume change immediately
    float vol = effectiveMusicVolume();
    if (fadingIn) {
        fadeInTargetVolume = vol;
        float t = std::clamp(fadeInTimer / std::max(fadeInDuration, 0.001f), 0.0f, 1.0f);
        AudioEngine::instance().setMusicVolume(fadeInTargetVolume * t);
    } else {
        AudioEngine::instance().setMusicVolume(vol);
    }
}

// A real crossfade: the old track goes to the engine's second voice and fades
// out there over fadeMs while the new one fades in on the main voice over the
// same span. This used to cut the old track dead, wait 30% of the fade in
// silence and start the new one at full volume - a zone line was a hard stop
// and a jump.
void MusicManager::beginCrossfadeOut(float fadeMs) {
    // Whatever fade the outgoing track was in is carried by the engine's fader
    // from its current level; a pending stop must not land on the new track.
    fadingOut = false;
    fadingIn = false;
    AudioEngine::instance().fadeOutMusicVoice(fadeMs);
    playing = false;
    currentTrack.clear();
    currentTrackIsFile = false;
}

void MusicManager::crossfadeTo(const std::string& mpqPath, float fadeMs) {
    if (mpqPath == currentTrack && playing) return;

    if (fadeMs > 0 && playing) {
        beginCrossfadeOut(fadeMs);
        playMusic(mpqPath, loopTracks_, fadeMs);
    } else {
        playMusic(mpqPath, loopTracks_);
    }
}

void MusicManager::crossfadeToFile(const std::string& filePath, float fadeMs) {
    if (filePath == currentTrack && playing) return;

    if (fadeMs > 0 && playing) {
        // The file is read on a worker as ever; it fades in from silence
        // whenever the read lands (pollPendingFileLoad), over the same span.
        beginCrossfadeOut(fadeMs);
        playFilePath(filePath, loopTracks_, fadeMs);
    } else {
        playFilePath(filePath, loopTracks_);
    }
}

void MusicManager::update(float deltaTime) {
    // Hand off any background-loaded track before touching playback state.
    pollPendingFileLoad();

    // Check if music is still playing
    if (playing && !AudioEngine::instance().isMusicPlaying()) {
        playing = false;
    }

    if (fadingOut) {
        fadeOutTimer += deltaTime;
        float t = std::clamp(1.0f - fadeOutTimer / std::max(fadeOutDuration, 0.001f), 0.0f, 1.0f);
        AudioEngine::instance().setMusicVolume(fadeOutStartVolume * t);
        if (t <= 0.0f) {
            // Fade complete - stop playback and restore volume for next track
            fadingOut = false;
            AudioEngine::instance().stopMusic();
            AudioEngine::instance().setMusicVolume(effectiveMusicVolume());
            playing = false;
            currentTrack.clear();
            currentTrackIsFile = false;
        }
        return;  // Don't process other fade logic while fading out
    }

    if (fadingIn) {
        fadeInTimer += deltaTime;
        float t = std::clamp(fadeInTimer / std::max(fadeInDuration, 0.001f), 0.0f, 1.0f);
        AudioEngine::instance().setMusicVolume(fadeInTargetVolume * t);
        if (t >= 1.0f) {
            fadingIn = false;
        }
    }
}

} // namespace audio
} // namespace wowee
