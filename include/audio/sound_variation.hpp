#pragma once

#include <cmath>
#include <cstddef>
#include <random>

namespace wowee::audio {

/// A small random turn of pitch and level for a sound that repeats - a step,
/// a blow - so a run of them reads as a run of different events rather than
/// one sample on a loop. Kept small on purpose: the clips are the stock ones
/// and should still sound like themselves.
struct SoundJitter {
    float pitch = 1.0f;  ///< playback rate multiplier
    float gain = 1.0f;   ///< linear volume multiplier
};

/// Pitch within +/-pitchSpread (0.04 = 4%) and level within +/-gainSpreadDb.
template <typename Rng>
SoundJitter soundJitter(Rng& gen, float pitchSpread = 0.04f, float gainSpreadDb = 1.5f) {
    std::uniform_real_distribution<float> pitchDist(1.0f - pitchSpread, 1.0f + pitchSpread);
    std::uniform_real_distribution<float> dbDist(-gainSpreadDb, gainSpreadDb);
    SoundJitter j;
    j.pitch = pitchDist(gen);
    j.gain = std::pow(10.0f, dbDist(gen) / 20.0f);
    return j;
}

/// An index in [0, count) that is not `last`, when there is any choice. With
/// one clip there is none; with `last` out of range (nothing played yet) every
/// index is open.
template <typename Rng>
std::size_t pickAvoidingRepeat(std::size_t count, std::size_t last, Rng& gen) {
    if (count <= 1) return 0;
    if (last >= count) {
        return std::uniform_int_distribution<std::size_t>(0, count - 1)(gen);
    }
    std::size_t i = std::uniform_int_distribution<std::size_t>(0, count - 2)(gen);
    if (i >= last) ++i;
    return i;
}

}  // namespace wowee::audio
