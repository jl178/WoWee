#pragma once

/// The one piece of the sound trace (audio_engine.hpp, WOWEE_SOUND_TRACE)
/// the sample loaders need: a place to say which file a buffer of bytes was
/// read from. Header-only so a loader can be built without the engine; the
/// engine installs the hook only while the trace is on.

#include <cstdint>
#include <string_view>
#include <vector>

namespace wowee::audio {

using SoundTraceRememberFn = void (*)(std::string_view path, const std::vector<uint8_t>& data);
inline SoundTraceRememberFn gSoundTraceRemember = nullptr;

/// Note that `data` was read from `path`. Free when the trace is off.
inline void noteSoundFile(std::string_view path, const std::vector<uint8_t>& data) {
    if (gSoundTraceRemember) gSoundTraceRemember(path, data);
}

}  // namespace wowee::audio
