#pragma once

#include "audio/sound_data.hpp"

#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace audio {

/// What the controller needs to know each frame.
struct ZoneMusicInput {
    uint32_t areaId = 0;        ///< AreaTable id under the player, 0 = unknown.
    bool isDay = true;
    bool trackPlaying = false;  ///< The music channel has a track running.
    bool trackLoading = false;  ///< A track is being read and will start.
    float deltaTime = 0.0f;
    /// Tracks to mix into the area's pool, for the client's own soundtrack.
    std::vector<std::string> extraTracks;
};

/// What the caller should do to the music channel this frame.
struct ZoneMusicAction {
    enum class Kind { None, Play, Stop };
    Kind kind = Kind::None;
    std::string path;     ///< Archive path for Play.
    float volume = 1.0f;  ///< SoundEntries volume for Play.
};

/// Zone music the way the stock client runs it.
///
/// Each area names a ZoneMusic set, inheriting its parent's when it has
/// none. The set has a day pool and a night pool and a silence interval. On
/// entering an area whose set differs from the current one a track starts
/// at once; a city with an intro piece plays that first, once per its
/// MinDelayMinutes. When a track ends the channel stays quiet for a random
/// time inside the set's silence interval, then the next track from the pool
/// for the current time of day starts. Entering an area with no set stops
/// the music.
///
/// Pure: the caller owns the music channel and applies the returned action.
class ZoneMusicController {
public:
    explicit ZoneMusicController(const SoundData& data, uint32_t seed = std::random_device{}());

    ZoneMusicAction update(const ZoneMusicInput& in);
    /// Forget the current set and the silence timer. For map changes.
    void reset();

    [[nodiscard]] uint32_t currentSetId() const { return currentSetId_; }
    [[nodiscard]] float silenceRemaining() const { return silenceLeft_; }

private:
    ZoneMusicAction start(const ZoneMusicSet& set, const ZoneMusicInput& in);
    ZoneMusicAction playIntro(uint32_t areaId);
    std::vector<std::string> pool(const ZoneMusicSet& set, bool isDay) const;

    const SoundData& data_;
    std::mt19937 rng_;
    uint32_t lastAreaId_ = 0;
    uint32_t currentSetId_ = 0;
    bool trackSeen_ = false;    ///< A track we started has been heard playing.
    float startGrace_ = 0.0f;   ///< Seconds to wait for a started track to report playing.
    float silenceLeft_ = 0.0f;
    float elapsed_ = 0.0f;
    std::string lastTrack_;
    std::unordered_map<uint32_t, float> introPlayedAt_;
};

} // namespace audio
} // namespace wowee
