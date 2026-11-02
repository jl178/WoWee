// A positional sound as AudioEngine sets it up in miniaudio
// (audio/positional_sound.hpp): panned by its direction from the listener,
// as FMOD pans every sound the stock client gives a position, with no
// distance gain but the stock rolloff the engine applies through the volume.
// Mixed offline: an engine with no device, read by hand.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DEVICE_IO
#define MA_NO_ENCODING
#include "audio/positional_sound.hpp"

#include <catch_amalgamated.hpp>

#include <cmath>
#include <vector>

namespace {

constexpr ma_uint32 kRate = 48000;
constexpr ma_uint32 kFrames = 9600;  // 200 ms: past the spatializer's gain smoothing

struct Levels {
    float left = 0.0f;
    float right = 0.0f;
};

/// The listener as AudioEngine::setListenerOrientation leaves it in render
/// space (Z up), looking along +X; 200 ms of a steady mono tone placed
/// at `position`. Returns the RMS of each output channel over the last
/// quarter of the mix.
Levels mixAt(const glm::vec3& position) {
    ma_engine_config config = ma_engine_config_init();
    config.noDevice = MA_TRUE;
    config.channels = 2;
    config.sampleRate = kRate;
    ma_engine engine;
    REQUIRE(ma_engine_init(&config, &engine) == MA_SUCCESS);
    ma_engine_listener_set_position(&engine, 0, 0.0f, 0.0f, 0.0f);
    ma_engine_listener_set_direction(&engine, 0, 1.0f, 0.0f, 0.0f);
    ma_engine_listener_set_world_up(&engine, 0, 0.0f, 0.0f, 1.0f);

    std::vector<float> tone(kFrames);
    for (ma_uint32 i = 0; i < kFrames; ++i) tone[i] = 0.5f * std::sin(static_cast<float>(i) * 0.05f);
    ma_audio_buffer_config bufferConfig = ma_audio_buffer_config_init(ma_format_f32, 1, kFrames, tone.data(), nullptr);
    bufferConfig.sampleRate = kRate;
    ma_audio_buffer buffer;
    REQUIRE(ma_audio_buffer_init(&bufferConfig, &buffer) == MA_SUCCESS);
    ma_sound sound;
    REQUIRE(ma_sound_init_from_data_source(&engine, &buffer, MA_SOUND_FLAG_NO_PITCH, nullptr, &sound) == MA_SUCCESS);
    wowee::audio::placeForPanning(&sound, position);
    REQUIRE(ma_sound_start(&sound) == MA_SUCCESS);

    std::vector<float> out(static_cast<size_t>(kFrames) * 2);
    ma_uint64 read = 0;
    REQUIRE(ma_engine_read_pcm_frames(&engine, out.data(), kFrames, &read) == MA_SUCCESS);
    REQUIRE(read == kFrames);

    Levels l;
    const ma_uint32 from = kFrames * 3 / 4;
    for (ma_uint32 i = from; i < kFrames; ++i) {
        l.left += out[i * 2] * out[i * 2];
        l.right += out[i * 2 + 1] * out[i * 2 + 1];
    }
    l.left = std::sqrt(l.left / static_cast<float>(kFrames - from));
    l.right = std::sqrt(l.right / static_cast<float>(kFrames - from));

    ma_sound_uninit(&sound);
    ma_audio_buffer_uninit(&buffer);
    ma_engine_uninit(&engine);
    return l;
}

}  // namespace

TEST_CASE("A positional sound is panned towards its side of the listener", "[positional_sound]") {
    // Looking along +X with Z up, the right hand is -Y.
    const Levels right = mixAt({0.0f, -20.0f, 0.0f});
    const Levels left = mixAt({0.0f, 20.0f, 0.0f});
    REQUIRE(right.right > 2.0f * right.left);
    REQUIRE(left.left > 2.0f * left.right);
    // Straight ahead: both ears alike.
    const Levels ahead = mixAt({20.0f, 0.0f, 0.0f});
    REQUIRE(ahead.left == Catch::Approx(ahead.right).epsilon(0.05));
}

TEST_CASE("miniaudio adds no distance gain of its own", "[positional_sound]") {
    // The stock rolloff, set through the volume, is the only distance curve:
    // the same direction near and far mixes at the same level.
    const Levels near = mixAt({0.0f, -2.0f, 0.0f});
    const Levels far = mixAt({0.0f, -40.0f, 0.0f});
    REQUIRE(far.right == Catch::Approx(near.right).epsilon(0.02));
    REQUIRE(far.left == Catch::Approx(near.left).margin(0.01));
    REQUIRE(far.right > 0.2f);
}
