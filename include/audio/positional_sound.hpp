#pragma once

// miniaudio's half of a positional sound. Kept apart from AudioEngine so the
// rule can be tested against miniaudio on its own (test_positional_sound).

#include "../../extern/miniaudio.h"

#include <glm/vec3.hpp>

namespace wowee {
namespace audio {

/// Place a world sound at `position`, heard the stock way: panned by its
/// direction from the listener - FMOD's full 3D pan, which every sound the
/// stock client gives a position gets (PlaySoundKit 0x4c6a40 ->
/// set3DAttributes 0x8d0c70, pan level 1) - with no distance gain of
/// miniaudio's own. The distance gain is the stock rolloff
/// (stock::rolloffGain), which AudioEngine sets through the volume.
///
/// Not ma_attenuation_model_none: that model turns miniaudio's spatializer
/// off altogether, panning and all, and a blow forty yards to the left then
/// sounds in the middle of the head - beside the listener, only quieter. The
/// inverse model at rolloff 0 is a gain of exactly 1 at every distance with
/// the panning kept. No Doppler: the stock client never gives FMOD a velocity.
inline void placeForPanning(ma_sound* sound, const glm::vec3& position) {
    ma_sound_set_position(sound, position.x, position.y, position.z);
    ma_sound_set_attenuation_model(sound, ma_attenuation_model_inverse);
    ma_sound_set_rolloff(sound, 0.0f);
    ma_sound_set_doppler_factor(sound, 0.0f);
}

}  // namespace audio
}  // namespace wowee
