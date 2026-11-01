#include "audio/zone_music_controller.hpp"

namespace wowee {
namespace audio {

namespace {
// How long a started track may take to report itself playing before the
// controller decides it never will and moves on.
constexpr float kStartGraceSeconds = 3.0f;
}

ZoneMusicController::ZoneMusicController(const SoundData& data, uint32_t seed)
    : data_(data), rng_(seed) {}

void ZoneMusicController::reset() {
    lastAreaId_ = 0;
    currentSetId_ = 0;
    trackSeen_ = false;
    startGrace_ = 0.0f;
    silenceLeft_ = 0.0f;
}

ZoneMusicAction ZoneMusicController::update(const ZoneMusicInput& in) {
    elapsed_ += in.deltaTime;
    const bool running = in.trackPlaying || in.trackLoading;

    if (in.areaId != 0 && in.areaId != lastAreaId_) {
        lastAreaId_ = in.areaId;
        const ZoneMusicSet* set = data_.areaMusic(in.areaId);
        const uint32_t setId = set ? set->id : 0;
        if (setId != currentSetId_) {
            currentSetId_ = setId;
            silenceLeft_ = 0.0f;
            trackSeen_ = false;
            if (!set) {
                return {running ? ZoneMusicAction::Kind::Stop : ZoneMusicAction::Kind::None, {}, 1.0f};
            }
            ZoneMusicAction intro = playIntro(in.areaId);
            if (intro.kind == ZoneMusicAction::Kind::Play) return intro;
            return start(*set, in);
        }
    }

    const ZoneMusicSet* set = data_.zoneMusic(currentSetId_);
    if (!set) return {};

    if (running) {
        trackSeen_ = true;
        startGrace_ = 0.0f;
        return {};
    }
    if (startGrace_ > 0.0f) {
        startGrace_ -= in.deltaTime;
        return {};
    }
    if (trackSeen_) {
        // The track just ended. Rest for the set's silence interval.
        trackSeen_ = false;
        const int d = in.isDay ? 0 : 1;
        const uint32_t lo = set->silenceMinMs[d];
        const uint32_t hi = set->silenceMaxMs[d] > lo ? set->silenceMaxMs[d] : lo;
        silenceLeft_ = std::uniform_int_distribution<uint32_t>(lo, hi)(rng_) / 1000.0f;
        return {};
    }
    if (silenceLeft_ > 0.0f) {
        silenceLeft_ -= in.deltaTime;
        return {};
    }
    return start(*set, in);
}

ZoneMusicAction ZoneMusicController::playIntro(uint32_t areaId) {
    const uint32_t introId = data_.areaIntro(areaId);
    const ZoneIntro* intro = introId ? data_.intro(introId) : nullptr;
    if (!intro) return {};
    const SoundEntry* entry = data_.sound(intro->soundId);
    if (!entry || entry->files.empty()) return {};
    auto played = introPlayedAt_.find(introId);
    if (played != introPlayedAt_.end() &&
        elapsed_ - played->second < static_cast<float>(intro->minDelayMinutes) * 60.0f) {
        return {};
    }
    introPlayedAt_[introId] = elapsed_;
    startGrace_ = kStartGraceSeconds;
    lastTrack_ = entry->files.front();
    return {ZoneMusicAction::Kind::Play, lastTrack_, entry->volume};
}

std::vector<std::string> ZoneMusicController::pool(const ZoneMusicSet& set, bool isDay) const {
    // A set with one side empty plays the other side all day.
    const int first = isDay ? 0 : 1;
    for (int d : {first, 1 - first}) {
        const SoundEntry* entry = data_.sound(set.soundId[d]);
        if (entry && !entry->files.empty()) return entry->files;
    }
    return {};
}

ZoneMusicAction ZoneMusicController::start(const ZoneMusicSet& set, const ZoneMusicInput& in) {
    std::vector<std::string> tracks = pool(set, in.isDay);
    float volume = 1.0f;
    if (const SoundEntry* entry = data_.sound(set.soundId[in.isDay ? 0 : 1])) volume = entry->volume;
    tracks.insert(tracks.end(), in.extraTracks.begin(), in.extraTracks.end());
    if (tracks.empty()) return {};

    const std::string* pick = &tracks.front();
    if (tracks.size() > 1) {
        std::uniform_int_distribution<size_t> dist(0, tracks.size() - 1);
        for (int attempt = 0; attempt < 5; ++attempt) {
            pick = &tracks[dist(rng_)];
            if (*pick != lastTrack_) break;
        }
    }
    lastTrack_ = *pick;
    startGrace_ = kStartGraceSeconds;
    return {ZoneMusicAction::Kind::Play, lastTrack_, volume};
}

} // namespace audio
} // namespace wowee
