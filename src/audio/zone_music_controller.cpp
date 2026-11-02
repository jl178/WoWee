#include "audio/zone_music_controller.hpp"
#include "audio/stock_sound_rules.hpp"

#include <algorithm>

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
    currentSetId_ = 0;
    currentSound_ = 0;
    currentIntroId_ = 0;
    trackSeen_ = false;
    startGrace_ = 0.0f;
    nextAllowedMs_ = 0.0;
}

float ZoneMusicController::silenceRemaining() const {
    return static_cast<float>(std::max(0.0, nextAllowedMs_ - nowMs_) / 1000.0);
}

const ZoneMusicSet* ZoneMusicController::resolveSet(const ZoneMusicInput& in) const {
    if (in.zoneMusicOverride) return data_.zoneMusic(in.zoneMusicOverride);
    if (in.indoors || in.areaId == 0) return nullptr;
    return data_.areaMusic(in.areaId);
}

uint32_t ZoneMusicController::resolveIntro(const ZoneMusicInput& in) const {
    if (in.introOverride) return in.introOverride;
    if (in.indoors || in.areaId == 0) return 0;
    return data_.areaIntro(in.areaId);
}

ZoneMusicController::Choice ZoneMusicController::choose(const ZoneMusicInput& in, bool running) const {
    Choice c;
    // An intro under way plays to its end (0x4c88b0, slots 5..7).
    if (running && currentIntroId_ != 0) {
        c.kind = Choice::Kind::Sound;
        c.soundId = currentSound_;
        c.intro = true;
        return c;
    }
    if (const ZoneIntro* intro = data_.intro(resolveIntro(in))) {
        const SoundEntry* entry = data_.sound(intro->soundId);
        auto ready = introReadyMs_.find(intro->soundId);
        if (entry && !entry->files.empty() &&
            (ready == introReadyMs_.end() || ready->second <= nowMs_)) {
            c.kind = Choice::Kind::Sound;
            c.soundId = intro->soundId;
            c.intro = true;
            return c;
        }
    }
    const ZoneMusicSet* set = resolveSet(in);
    if (!set) return c;
    const uint32_t sound = set->soundId[in.isDay ? 0 : 1];
    // A set with no sound for this half of the day is silent then (the two
    // one-sided sets, Gnomeregan and an Ulduar walk, are night-silent).
    if (sound == 0) return c;
    if (!in.noDelay && nowMs_ < nextAllowedMs_) {
        c.kind = Choice::Kind::Wait;
        return c;
    }
    c.kind = Choice::Kind::Sound;
    c.soundId = sound;
    return c;
}

void ZoneMusicController::onTrackEnded(const ZoneMusicInput& in) {
    if (currentIntroId_ != 0) {
        // 0x4c9080: the intro may come again MinDelayMinutes after it ended.
        if (const ZoneIntro* intro = data_.intro(currentIntroId_)) {
            introReadyMs_[currentSound_] = nowMs_ + static_cast<double>(intro->minDelayMinutes) * 60000.0;
        }
    } else if (trackSeen_) {
        // 0x4c8ab0: still the choice, rest; the choice moved on, no rest.
        const ZoneMusicSet* set = resolveSet(in);
        const int half = in.isDay ? 0 : 1;
        if (set && set->soundId[half] == currentSound_) {
            const uint32_t lo = set->silenceMinMs[half];
            const uint32_t hi = std::max(lo, set->silenceMaxMs[half]);
            nextAllowedMs_ = nowMs_ + std::uniform_int_distribution<uint32_t>(lo, hi)(rng_);
        } else {
            nextAllowedMs_ = 0.0;
        }
    }
    currentSound_ = 0;
    currentIntroId_ = 0;
    trackSeen_ = false;
    startGrace_ = 0.0f;
}

ZoneMusicAction ZoneMusicController::start(const Choice& c, const ZoneMusicInput& in, bool running) {
    const SoundEntry* entry = data_.sound(c.soundId);
    if (!entry || entry->files.empty()) return {};
    std::string path;
    float volume = entry->volume;
    // The client's own soundtrack shares the pool when asked for: as many
    // draws as it has tracks against the row's.
    if (!c.intro && !in.extraTracks.empty()) {
        const size_t total = entry->files.size() + in.extraTracks.size();
        const size_t r = std::uniform_int_distribution<size_t>(0, total - 1)(rng_);
        if (r >= entry->files.size()) {
            path = in.extraTracks[r - entry->files.size()];
            volume = 1.0f;
        }
    }
    if (path.empty()) {
        const std::string* file = data_.nextFile(*entry);
        if (!file) return {};
        path = *file;
    }
    currentSound_ = c.soundId;
    currentIntroId_ = c.intro ? resolveIntro(in) : 0;
    trackSeen_ = false;
    startGrace_ = kStartGraceSeconds;

    ZoneMusicAction a;
    a.kind = ZoneMusicAction::Kind::Play;
    a.path = std::move(path);
    a.volume = volume;
    a.fadeInSeconds = running ? stock::kMusicFadeSeconds : stock::kMusicFirstFadeInSeconds;
    a.fadeOutSeconds = stock::kMusicFadeSeconds;
    return a;
}

ZoneMusicAction ZoneMusicController::update(const ZoneMusicInput& in) {
    nowMs_ += static_cast<double>(in.deltaTime) * 1000.0;
    const bool running = in.trackPlaying || in.trackLoading;

    // Nothing is decided while the place is unknown (its chunk not resident).
    const bool known = in.areaId != 0 || in.zoneMusicOverride != 0 || in.introOverride != 0;
    if (known) {
        const ZoneMusicSet* set = resolveSet(in);
        currentSetId_ = set ? set->id : 0;
    }

    if (currentSound_ != 0) {
        if (running) {
            trackSeen_ = true;
            startGrace_ = 0.0f;
        } else if (!trackSeen_ && startGrace_ > 0.0f) {
            // Started but not yet heard: wait for it rather than start another.
            startGrace_ -= in.deltaTime;
            return {};
        } else {
            onTrackEnded(in);
        }
    }
    if (!known) return {};

    const Choice c = choose(in, running);
    switch (c.kind) {
    case Choice::Kind::Wait:
        return {};
    case Choice::Kind::None: {
        // 0x4c8d80: a place with no music forgets the silence, and whatever
        // plays fades out.
        nextAllowedMs_ = 0.0;
        const bool ours = currentSound_ != 0;
        currentSound_ = 0;
        currentIntroId_ = 0;
        trackSeen_ = false;
        if (!running || !ours) return {};
        ZoneMusicAction stop;
        stop.kind = ZoneMusicAction::Kind::Stop;
        stop.fadeOutSeconds = stock::kMusicFadeSeconds;
        return stop;
    }
    case Choice::Kind::Sound:
        if (c.soundId == currentSound_ && running) return {};
        return start(c, in, running);
    }
    return {};
}

} // namespace audio
} // namespace wowee
