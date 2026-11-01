#pragma once

namespace wowee {
namespace audio {

/// The day half of the clock as the sound tables see it. ZoneMusic and
/// SoundAmbience each carry a day and a night entry and nothing in between,
/// so one boundary serves music, ambience and the hand-written libraries.
inline bool isDaytimeHours(float hours) {
    return hours >= 6.0f && hours < 20.0f;
}

} // namespace audio
} // namespace wowee
