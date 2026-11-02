#pragma once

#include "audio/stock_sound_rules.hpp"

namespace wowee {
namespace audio {

/// The day half of the clock as the sound tables see it. ZoneMusic and
/// SoundAmbience each carry a day and a night entry and nothing in between,
/// so one boundary serves music, ambience and the hand-written libraries:
/// the stock client's 05:30 to 21:00 (Wow.exe 0x4c9850).
inline bool isDaytimeHours(float hours) {
    return stock::isDaytime(hours);
}

} // namespace audio
} // namespace wowee
