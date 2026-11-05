#include "audio/footstep_manager.hpp"
#include "audio/sound_trace_hook.hpp"
#include "audio/footstep_paths.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sound_variation.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cstdint>
#include <string>

namespace wowee {
namespace audio {


FootstepManager::FootstepManager() : rng(std::random_device{}()) {}

FootstepManager::~FootstepManager() {
    shutdown();
}

bool FootstepManager::initialize(pipeline::AssetManager* assets) {
    assetManager = assets;
    sampleCount = 0;
    for (auto& surface : surfaces) {
        surface.clips.clear();
    }
    for (auto& surface : horseSurfaces) {
        surface.clips.clear();
    }
    for (auto& surface : hugeSurfaces) {
        surface.clips.clear();
    }

    if (!assetManager) {
        return false;
    }

    preloadSurface(surfaces, FootstepSurface::STONE, classicFootstepPaths("Stone"), "character");
    preloadSurface(surfaces, FootstepSurface::DIRT, classicFootstepPaths("Dirt"), "character");
    preloadSurface(surfaces, FootstepSurface::GRASS, classicFootstepPaths("Grass"), "character");
    preloadSurface(surfaces, FootstepSurface::WOOD, classicFootstepPaths("Wood"), "character");
    preloadSurface(surfaces, FootstepSurface::SNOW, classicFootstepPaths("Snow"), "character");
    preloadSurface(surfaces, FootstepSurface::WATER, waterFootstepPaths(), "character");

    // Alternate naming seen in some builds (especially metals).
    preloadSurface(surfaces, FootstepSurface::METAL,
                   altFootstepPaths("MediumLargeMetalFootsteps", "MediumLargeFootstepMetal"), "character");
    if (surfaces[static_cast<size_t>(FootstepSurface::METAL)].clips.empty()) {
        preloadSurface(surfaces, FootstepSurface::METAL, classicFootstepPaths("Metal"), "character");
    }

    preloadSurface(horseSurfaces, FootstepSurface::STONE, horseFootstepPaths("Stone"), "horse");
    preloadSurface(horseSurfaces, FootstepSurface::DIRT, horseFootstepPaths("Dirt"), "horse");
    preloadSurface(horseSurfaces, FootstepSurface::GRASS, horseFootstepPaths("Grass"), "horse");
    preloadSurface(horseSurfaces, FootstepSurface::WOOD, horseFootstepPaths("Wood"), "horse");
    preloadSurface(horseSurfaces, FootstepSurface::SNOW, horseFootstepPaths("Snow"), "horse");

    preloadSurface(hugeSurfaces, FootstepSurface::STONE, hugeFootstepPaths("Stone"), "huge");
    preloadSurface(hugeSurfaces, FootstepSurface::DIRT, hugeFootstepPaths("Dirt"), "huge");
    preloadSurface(hugeSurfaces, FootstepSurface::GRASS, hugeFootstepPaths("Grass"), "huge");
    preloadSurface(hugeSurfaces, FootstepSurface::WOOD, hugeFootstepPaths("Wood"), "huge");
    preloadSurface(hugeSurfaces, FootstepSurface::SNOW, hugeFootstepPaths("Snow"), "huge");
    preloadSurface(hugeSurfaces, FootstepSurface::WATER, hugeWaterFootstepPaths(), "huge");

    LOG_INFO("Footstep manager initialized (", sampleCount, " clips)");
    return sampleCount > 0;
}

void FootstepManager::shutdown() {
    for (auto& surface : surfaces) {
        surface.clips.clear();
    }
    for (auto& surface : horseSurfaces) {
        surface.clips.clear();
    }
    for (auto& surface : hugeSurfaces) {
        surface.clips.clear();
    }
    sampleCount = 0;
    assetManager = nullptr;
}

void FootstepManager::update(float) {
    // No longer needed - AudioEngine handles cleanup internally
}

void FootstepManager::playFootstep(FootstepSurface surface, bool sprinting) {
    if (!assetManager || sampleCount == 0 || !AudioEngine::instance().isInitialized()) {
        return;
    }
    // The character's own steps come from its model's $FSD events and the
    // FootstepTerrainLookup kit once the tables are read, as every other
    // unit's do (Wow.exe 0x4cf170 plays both).
    if (ownStepsFromTables_) return;
    playRandomStep(surface, FootstepBank::CHARACTER,
                   sprinting ? 0.09f : 0.14f,
                   sprinting ? 1.0f : 0.88f);
}

void FootstepManager::preloadSurface(SurfaceSamples* bank, FootstepSurface surface,
                                     const std::vector<std::string>& candidates,
                                     const char* bankName) {
    if (!assetManager) {
        return;
    }

    auto& list = bank[static_cast<size_t>(surface)].clips;
    for (const std::string& path : candidates) {
        if (!assetManager->fileExists(path)) {
            continue;
        }
        auto data = assetManager->readFile(path);
        if (data.empty()) {
            continue;
        }
        noteSoundFile(path, data);
        list.push_back({path, std::move(data)});
        sampleCount++;
    }

    if (!list.empty()) {
        LOG_INFO("Footsteps ", bankName, "/", surfaceName(surface), ": loaded ", list.size(), " clips");
    }
}

bool FootstepManager::playRandomStep(FootstepSurface surface, FootstepBank bank,
                                     float minInterval, float volumeMul) {
    auto now = std::chrono::steady_clock::now();
    if (lastPlayTime.time_since_epoch().count() != 0) {
        float elapsed = std::chrono::duration<float>(now - lastPlayTime).count();
        if (elapsed < minInterval) {
            return false;
        }
    }

    const auto surfaceIndex = static_cast<size_t>(surface);
    const std::vector<Sample>* list = nullptr;
    switch (bank) {
        case FootstepBank::HORSE: list = &horseSurfaces[surfaceIndex].clips; break;
        case FootstepBank::HEAVY: list = &hugeSurfaces[surfaceIndex].clips; break;
        default:                  list = &surfaces[surfaceIndex].clips; break;
    }

    if (list->empty() && bank != FootstepBank::CHARACTER) {
        // Mount archives do not author metal or water variants everywhere.
        // Hooves on metal are closest to the stone bank; water keeps the
        // normal splash bank.
        if (surface == FootstepSurface::WATER) {
            list = &surfaces[surfaceIndex].clips;
        } else if (bank == FootstepBank::HORSE) {
            list = &horseSurfaces[static_cast<size_t>(FootstepSurface::STONE)].clips;
        } else {
            list = &hugeSurfaces[static_cast<size_t>(FootstepSurface::STONE)].clips;
        }
    }
    if (list->empty()) {
        list = &surfaces[static_cast<size_t>(FootstepSurface::STONE)].clips;
        if (list->empty()) return false;
    }

    // A different clip from the last one, whenever the bank has a choice: two
    // identical steps in a row is the loop a run of footsteps gives itself
    // away by. The last clip is remembered by bank, so a mount and its rider
    // do not count against each other.
    const size_t last = (list == lastStepList_) ? lastStepIndex_ : SIZE_MAX;
    const size_t index = pickAvoidingRepeat(list->size(), last, rng);
    const Sample& sample = (*list)[index];
    lastStepList_ = list;
    lastStepIndex_ = index;

    // Pitch within 4% and level within 1.5 dB, so the cadence varies the way
    // feet on ground do without any clip stopping sounding like itself.
    const SoundJitter jitter = soundJitter(rng);
    float pitch = jitter.pitch;
    float volume = jitter.gain * volumeMul * volumeScale;
    if (volume > 1.0f) volume = 1.0f;
    if (volume < 0.1f) volume = 0.1f;

    // The player's own steps: in the listener's head at 0.65, at priority
    // 115, four at most at once (Wow.exe 0x4cf170, category 17).
    SoundOptions opts;
    opts.priority = stock::kPriorityOwnFootstep;
    opts.category = stock::Category::OwnFootstep;
    opts.ownCharacter = true;
    bool success = AudioEngine::instance().playSound2D(sample.data, volume, pitch, SoundBus::World, opts);

    if (success) {
        lastPlayTime = now;
        return true;
    }

    return false;
}

const char* FootstepManager::surfaceName(FootstepSurface surface) {
    switch (surface) {
        case FootstepSurface::STONE: return "stone";
        case FootstepSurface::DIRT: return "dirt";
        case FootstepSurface::GRASS: return "grass";
        case FootstepSurface::WOOD: return "wood";
        case FootstepSurface::METAL: return "metal";
        case FootstepSurface::WATER: return "water";
        case FootstepSurface::SNOW: return "snow";
        default: return "unknown";
    }
}

} // namespace audio
} // namespace wowee
