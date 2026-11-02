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
    /// A ZoneMusic id from WMOAreaTable: the room's row, else its building's
    /// -1 row. Wins over the area's.
    uint32_t zoneMusicOverride = 0;
    /// A ZoneIntroMusicTable id from WMOAreaTable, the same way.
    uint32_t introOverride = 0;
    /// The player is in an interior WMO group (MOGP lacks the exterior
    /// flag). Indoors the stock client asks only WMOAreaTable (0x78e9a0): a
    /// room whose rows name no music has none, whatever the area outside.
    bool indoors = false;
    bool isDay = true;
    bool trackPlaying = false;  ///< The music channel has a track running.
    bool trackLoading = false;  ///< A track is being read and will start.
    float deltaTime = 0.0f;
    /// Sound_ZoneMusicNoDelay, the Sound panel's "Loop Music": the next
    /// track follows the last one with no silence between them.
    bool noDelay = false;
    /// Tracks to mix into the area's pool, for the client's own soundtrack.
    std::vector<std::string> extraTracks;
};

/// What the caller should do to the music channel this frame.
struct ZoneMusicAction {
    enum class Kind { None, Play, Stop };
    Kind kind = Kind::None;
    std::string path;     ///< Archive path for Play.
    float volume = 1.0f;  ///< SoundEntries volume for Play.
    /// Play: how long the new track takes to come in. The track it replaces
    /// fades out over fadeOutSeconds meanwhile - a crossfade.
    float fadeInSeconds = 0.0f;
    /// Play and Stop: how long the outgoing track takes to fade out.
    float fadeOutSeconds = 0.0f;
};

/// Zone music the way the stock client schedules it
/// (SoundInterface2ZoneSounds.cpp, Wow.exe 0x4c9910 each frame).
///
/// What should play is decided afresh every frame, by priority:
///  - a zone intro (ZoneIntroMusicTable) whose cooldown has run out; an
///    intro already playing is never interrupted;
///  - else the area's ZoneMusic set, its day or its night sound, unless the
///    silence after the last track is still running.
/// When the choice differs from what is playing, the new piece crossfades
/// in over 4 s (0.01 s from silence). When a zone track ends by itself the
/// channel rests a random time inside the set's silence interval if the same
/// sound is still the choice, and not at all if the choice has moved on. An
/// intro's cooldown (MinDelayMinutes) counts from the end of the intro. An
/// area with no music fades the track out and forgets the silence. Tracks
/// are drawn from the SoundEntries row the stock way (SoundData::nextFile).
///
/// Pure: the caller owns the music channel and applies the returned action.
class ZoneMusicController {
public:
    explicit ZoneMusicController(const SoundData& data, uint32_t seed = std::random_device{}());

    ZoneMusicAction update(const ZoneMusicInput& in);
    /// Forget what is playing and the silence timer. For map changes.
    void reset();

    /// The ZoneMusic set the place resolves to; 0 for none.
    [[nodiscard]] uint32_t currentSetId() const { return currentSetId_; }
    /// The SoundEntries row last started; 0 when none is ours.
    [[nodiscard]] uint32_t currentSoundId() const { return currentSound_; }
    /// Seconds of silence left before the next zone track may start.
    [[nodiscard]] float silenceRemaining() const;

private:
    struct Choice {
        enum class Kind { None, Wait, Sound } kind = Kind::None;
        uint32_t soundId = 0;
        bool intro = false;
    };
    Choice choose(const ZoneMusicInput& in, bool running) const;
    void onTrackEnded(const ZoneMusicInput& in);
    ZoneMusicAction start(const Choice& c, const ZoneMusicInput& in, bool running);
    [[nodiscard]] const ZoneMusicSet* resolveSet(const ZoneMusicInput& in) const;
    [[nodiscard]] uint32_t resolveIntro(const ZoneMusicInput& in) const;

    const SoundData& data_;
    std::mt19937 rng_;
    double nowMs_ = 0.0;
    uint32_t currentSetId_ = 0;
    uint32_t currentSound_ = 0;     ///< the SoundEntries row we started
    uint32_t currentIntroId_ = 0;   ///< its ZoneIntroMusicTable row when it is an intro
    bool trackSeen_ = false;        ///< it has been reported playing
    float startGrace_ = 0.0f;       ///< seconds to wait for it to report
    double nextAllowedMs_ = 0.0;    ///< 0xb4aaa0: no zone track before this
    std::unordered_map<uint32_t, double> introReadyMs_;  ///< by SoundEntries id (0xb4acd8)
};

} // namespace audio
} // namespace wowee
