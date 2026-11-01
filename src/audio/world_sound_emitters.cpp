#include "audio/world_sound_emitters.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sound_data.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace wowee {
namespace audio {

namespace {

std::mt19937& rng() {
    static std::mt19937 gen{std::random_device{}()};
    return gen;
}

const std::string* pickFile(const SoundEntry& entry) {
    if (entry.files.empty()) return nullptr;
    std::uniform_int_distribution<size_t> d(0, entry.files.size() - 1);
    return &entry.files[d(rng())];
}

// A loop already playing keeps going a little past its cutoff, so standing on
// the edge does not start and stop it every few steps.
constexpr float kStopSlack = 1.1f;
// How often the loops are re-chosen; periodic one-shots tick every frame.
constexpr float kScanIntervalS = 0.25f;
// A loop that failed to start (engine muted, file missing) tries again after.
constexpr float kRetryS = 2.0f;

} // namespace

WorldSoundEmitters::~WorldSoundEmitters() {
    clear();
}

bool WorldSoundEmitters::resolve(Emitter& e) const {
    if (e.resolved) return true;
    if (!soundData_ || !soundData_->loaded()) return false;   // ask again later
    const SoundEntry* entry = soundData_->sound(e.soundId);
    e.resolved = true;
    if (!entry || entry->files.empty()) {
        e.dead = true;
        return true;
    }
    e.volume = entry->volume > 0.0f ? entry->volume : 1.0f;
    // A cutoff of 0 is "unset" in a few rows; the engine's own default then.
    e.cutoff = entry->distanceCutoff > 0.0f ? entry->distanceCutoff : 30.0f;
    e.minDistance = entry->minDistance > 0.0f ? entry->minDistance : -1.0f;
    return true;
}

uint32_t WorldSoundEmitters::add(Emitter e) {
    if (e.soundId == 0) return 0;
    // Doodads can arrive before the sound tables are read; those are
    // resolved on the first update that has them.
    resolve(e);
    if (e.dead) return 0;
    uint32_t id = nextId_++;
    if (nextId_ == 0) nextId_ = 1;
    emitters_[id] = e;
    return id;
}

uint32_t WorldSoundEmitters::addLoop(const glm::vec3& renderPos, uint32_t soundEntryId) {
    Emitter e;
    e.position = renderPos;
    e.soundId = soundEntryId;
    return add(e);
}

uint32_t WorldSoundEmitters::addPeriodic(const glm::vec3& renderPos, uint32_t soundEntryId,
                                         uint32_t periodMs, uint32_t phaseMs) {
    if (periodMs == 0) return 0;
    Emitter e;
    e.position = renderPos;
    e.soundId = soundEntryId;
    e.periodic = true;
    e.periodS = static_cast<float>(periodMs) / 1000.0f;
    // Placed doodads start their animation at a random point, so the first
    // play is anywhere in the period; phase keeps two events of one model apart.
    std::uniform_real_distribution<float> d(0.0f, e.periodS);
    e.timerS = std::fmod(d(rng()) + static_cast<float>(phaseMs) / 1000.0f, e.periodS);
    return add(e);
}

void WorldSoundEmitters::stop(Emitter& e) {
    if (e.handle != 0) {
        AudioEngine::instance().fadeOutSound(e.handle, 250.0f);
        e.handle = 0;
    }
}

void WorldSoundEmitters::remove(uint32_t id) {
    auto it = emitters_.find(id);
    if (it == emitters_.end()) return;
    stop(it->second);
    emitters_.erase(it);
}

void WorldSoundEmitters::clear() {
    for (auto& [id, e] : emitters_) {
        if (e.handle != 0) AudioEngine::instance().stopSound(e.handle);
        e.handle = 0;
    }
    emitters_.clear();
    mapId_ = 0xFFFFFFFFu;
}

void WorldSoundEmitters::setMap(uint32_t mapId) {
    if (mapId == mapId_) return;
    mapId_ = mapId;
    for (auto it = emitters_.begin(); it != emitters_.end();) {
        if (it->second.placed) {
            stop(it->second);
            it = emitters_.erase(it);
        } else {
            ++it;
        }
    }
    if (!soundData_) return;
    const auto* rows = soundData_->mapEmitters(mapId);
    size_t added = 0;
    if (rows) {
        for (const auto& row : *rows) {
            Emitter e;
            // The table is in server/wire coordinates, like AreaTrigger.dbc.
            e.position = core::coords::canonicalToRender(core::coords::serverToCanonical(
                glm::vec3(row.position[0], row.position[1], row.position[2])));
            e.soundId = row.soundId;
            e.placed = true;
            if (add(e) != 0) ++added;
        }
    }
    LOG_INFO("WorldSoundEmitters: map ", mapId, ": ", added, " placed emitters from SoundEmitters.dbc");
}

bool WorldSoundEmitters::start(Emitter& e, float volumeScale) {
    const SoundEntry* entry = soundData_ ? soundData_->sound(e.soundId) : nullptr;
    const std::string* file = entry ? pickFile(*entry) : nullptr;
    if (!file) return false;
    e.handle = AudioEngine::instance().playSound3DStoppable(*file, e.position, e.volume * volumeScale,
                                                            e.cutoff, /*loop=*/true, e.minDistance);
    return e.handle != 0;
}

void WorldSoundEmitters::update(float deltaTime, const glm::vec3& listenerPos, float volumeScale) {
    if (emitters_.empty()) return;
    auto& engine = AudioEngine::instance();

    // The volume setting moved: re-level what is already playing.
    if (volumeScale != lastVolumeScale_) {
        lastVolumeScale_ = volumeScale;
        for (auto& [id, e] : emitters_) {
            if (e.handle != 0) engine.setSoundVolume(e.handle, e.volume * volumeScale);
        }
    }

    // One-shots on their animation clock, heard only inside their cutoff.
    for (auto& [id, e] : emitters_) {
        if (!e.periodic) continue;
        if (!resolve(e) || e.dead) continue;
        e.timerS -= deltaTime;
        if (e.timerS > 0.0f) continue;
        e.timerS += e.periodS;
        if (e.timerS <= 0.0f) e.timerS = e.periodS;   // a long frame: no burst
        const glm::vec3 d = e.position - listenerPos;
        if (glm::dot(d, d) > e.cutoff * e.cutoff || volumeScale <= 0.0f) continue;
        const SoundEntry* entry = soundData_ ? soundData_->sound(e.soundId) : nullptr;
        if (const std::string* file = entry ? pickFile(*entry) : nullptr) {
            engine.playSound3D(*file, e.position, e.volume * volumeScale, 1.0f, e.cutoff);
        }
    }

    scanTimerS_ -= deltaTime;
    if (scanTimerS_ > 0.0f) return;
    scanTimerS_ = kScanIntervalS;

    // Loops: everything inside its cutoff, nearest first, up to kMaxLoops.
    candidates_.clear();
    for (auto& [id, e] : emitters_) {
        if (e.periodic) continue;
        if (!resolve(e) || e.dead) continue;
        if (e.retryS > 0.0f) e.retryS -= kScanIntervalS;
        const glm::vec3 d = e.position - listenerPos;
        const float distSq = glm::dot(d, d);
        const float reach = e.handle != 0 ? e.cutoff * kStopSlack : e.cutoff;
        if (distSq <= reach * reach) {
            candidates_.emplace_back(distSq, id);
        } else if (e.handle != 0) {
            stop(e);
        }
    }
    std::sort(candidates_.begin(), candidates_.end());
    for (size_t i = 0; i < candidates_.size(); ++i) {
        Emitter& e = emitters_[candidates_[i].second];
        if (static_cast<int>(i) >= kMaxLoops || volumeScale <= 0.0f) {
            stop(e);
            continue;
        }
        if (e.handle != 0 || e.retryS > 0.0f) continue;
        if (!start(e, volumeScale)) e.retryS = kRetryS;
    }
}

} // namespace audio
} // namespace wowee
