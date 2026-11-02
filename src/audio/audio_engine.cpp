#define MINIAUDIO_IMPLEMENTATION
#include "audio/audio_engine.hpp"
#include "audio/positional_sound.hpp"
#include "audio/world_fx.hpp"
#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"


#include "../../extern/miniaudio.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

namespace wowee {
namespace audio {

namespace {

/// Owns malloc'd storage for a miniaudio object and unwinds it correctly on
/// any early return.
///
/// miniaudio initialises into caller-provided storage, so teardown is two
/// steps -- uninit the object, then free the storage -- and the uninit must
/// not run on storage that was never initialised. That is the distinction the
/// hand-written cleanup here used to encode by having a different free()
/// sequence at each of five early returns per function, duplicated across two
/// functions. Every one of them had to be right, and nothing checked that.
///
/// Call markInitialised() once the matching ma_*_init has succeeded, and
/// release() to hand the pointer to activeSounds_, which owns it from then on.
template <typename T, void (*Uninit)(T*)>
class MaStorage {
public:
    MaStorage() : ptr_(static_cast<T*>(std::malloc(sizeof(T)))) {}
    ~MaStorage() {
        if (ptr_ != nullptr) {
            if (initialised_) {
                Uninit(ptr_);
            }
            std::free(ptr_);
        }
    }

    MaStorage(const MaStorage&) = delete;
    MaStorage& operator=(const MaStorage&) = delete;

    explicit operator bool() const { return ptr_ != nullptr; }
    T* get() const { return ptr_; }
    void markInitialised() { initialised_ = true; }

    /// Give up ownership. The caller frees it from here on.
    T* release() {
        T* p = ptr_;
        ptr_ = nullptr;
        initialised_ = false;
        return p;
    }

private:
    T* ptr_ = nullptr;
    bool initialised_ = false;
};

using AudioBufferStorage = MaStorage<ma_audio_buffer, ma_audio_buffer_uninit>;
using SoundStorage = MaStorage<ma_sound, ma_sound_uninit>;

struct DecodedWavCacheEntry {
    ma_format format = ma_format_unknown;
    ma_uint32 channels = 0;
    ma_uint32 sampleRate = 0;
    ma_uint64 frames = 0;
    std::shared_ptr<std::vector<uint8_t>> pcmData;
};

static std::unordered_map<uint64_t, DecodedWavCacheEntry> gDecodedWavCache;
// Protects gDecodedWavCache - shared_lock for reads, unique_lock for writes.
// Required because playSound2D() can be called from multiple threads
// (main thread, async loaders, animation callbacks).
static std::shared_mutex gDecodedWavCacheMutex;

static uint64_t makeWavCacheKey(const std::vector<uint8_t>& wavData) {
    // FNV-1a over the first 256 bytes + last 256 bytes + total size.
    // Full-content hash would be correct but slow for large files; sampling the
    // edges catches virtually all distinct files while keeping cost O(1).
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ull;
    constexpr uint64_t FNV_PRIME  = 1099511628211ull;
    uint64_t h = FNV_OFFSET;
    auto mix = [&](uint8_t b) { h ^= b; h *= FNV_PRIME; };

    const size_t sz = wavData.size();
    const size_t head = std::min(sz, size_t(256));
    for (size_t i = 0; i < head; ++i) mix(wavData[i]);
    if (sz > 256) {
        const size_t tail_start = sz > 512 ? sz - 256 : 256;
        for (size_t i = tail_start; i < sz; ++i) mix(wavData[i]);
    }
    // Mix in the total size so files with identical head/tail but different
    // lengths still produce different keys.
    for (int s = 0; s < 8; ++s) mix(static_cast<uint8_t>(sz >> (s * 8)));
    return h;
}

static bool decodeWavCached(const std::vector<uint8_t>& wavData, DecodedWavCacheEntry& out) {
    if (wavData.empty()) return false;

    const uint64_t key = makeWavCacheKey(wavData);

    // Fast path: shared (read) lock for cache hits - allows concurrent lookups.
    {
        std::shared_lock<std::shared_mutex> readLock(gDecodedWavCacheMutex);
        if (auto it = gDecodedWavCache.find(key); it != gDecodedWavCache.end()) {
            out = it->second;
            return true;
        }
    }

    ma_decoder decoder;
    ma_decoder_config decoderConfig = ma_decoder_config_init_default();
    ma_result result = ma_decoder_init_memory(
        wavData.data(),
        wavData.size(),
        &decoderConfig,
        &decoder
    );
    if (result != MA_SUCCESS) {
        LOG_ERROR("AudioEngine: Failed to decode WAV data (", wavData.size(), " bytes): error ", result);
        return false;
    }

    ma_uint64 totalFrames = 0;
    result = ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);
    if (result != MA_SUCCESS) totalFrames = 0;

    ma_format format = decoder.outputFormat;
    ma_uint32 channels = decoder.outputChannels;
    ma_uint32 sampleRate = decoder.outputSampleRate;
    ma_uint64 maxFrames = sampleRate * 60;
    if (totalFrames == 0 || totalFrames > maxFrames) totalFrames = maxFrames;

    size_t bufferSize = totalFrames * channels * ma_get_bytes_per_sample(format);
    auto pcmData = std::make_shared<std::vector<uint8_t>>(bufferSize);
    ma_uint64 framesRead = 0;
    result = ma_decoder_read_pcm_frames(&decoder, pcmData->data(), totalFrames, &framesRead);
    ma_decoder_uninit(&decoder);
    if (result != MA_SUCCESS || framesRead == 0) {
        LOG_ERROR("AudioEngine: Failed to read frames from WAV: error ", result, ", framesRead=", framesRead);
        return false;
    }

    pcmData->resize(framesRead * channels * ma_get_bytes_per_sample(format));

    DecodedWavCacheEntry entry;
    entry.format = format;
    entry.channels = channels;
    entry.sampleRate = sampleRate;
    entry.frames = framesRead;
    entry.pcmData = pcmData;
    // Evict oldest half when cache grows too large. 256 entries ≈ 50-100 MB of decoded
    // PCM data depending on file lengths; halving keeps memory bounded while retaining
    // recently-heard sounds (footsteps, UI clicks, combat hits) for instant replay.
    // Exclusive (write) lock - only one thread can evict + insert.
    {
        std::lock_guard<std::shared_mutex> writeLock(gDecodedWavCacheMutex);
        // Re-check in case another thread inserted while we were decoding.
        if (auto it = gDecodedWavCache.find(key); it != gDecodedWavCache.end()) {
            out = it->second;
            return true;
        }
        constexpr size_t kMaxCachedSounds = 256;
        if (gDecodedWavCache.size() >= kMaxCachedSounds) {
            auto it = gDecodedWavCache.begin();
            std::advance(it, gDecodedWavCache.size() / 2);
            gDecodedWavCache.erase(gDecodedWavCache.begin(), it);
        }
        gDecodedWavCache.emplace(key, entry);
    }
    out = entry;
    return true;
}

/// A miniaudio node around WorldFx: one input bus, one output bus, the
/// engine's channel count on both. ma_node_base must come first - miniaudio
/// treats a pointer to this as a pointer to it.
struct WorldFxNode {
    ma_node_base base;
    WorldFx fx;
};

void worldFxProcess(ma_node* node, const float** framesIn, ma_uint32* frameCountIn,
                    float** framesOut, ma_uint32* frameCountOut) {
    (void)frameCountIn;
    auto* self = static_cast<WorldFxNode*>(node);
    self->fx.process(framesIn[0], framesOut[0], *frameCountOut);
}

// Continuous: the reverb's tail keeps sounding after the last sound in the
// group has finished, so the node is pulled even when nothing feeds it.
ma_node_vtable gWorldFxVtable = {worldFxProcess, nullptr, 1, 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING};

} // namespace

AudioEngine& AudioEngine::instance() {
    static AudioEngine instance;
    return instance;
}

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::initialize() {
    if (initialized_) {
        LOG_WARNING("AudioEngine already initialized");
        return true;
    }

    // Allocate miniaudio engine
    engine_ = new ma_engine();

    // The default config, but with the device's callback our own: it mixes the
    // same way miniaudio's does and also feeds the screen recorder's tap.
    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.dataCallback = &AudioEngine::deviceDataCallback;
    ma_result result = ma_engine_init(&engineConfig, engine_);
    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to initialize miniaudio engine: ", result);
        delete engine_;
        engine_ = nullptr;
        return false;
    }

    // Set default master volume
    ma_engine_set_volume(engine_, masterVolume_);

    // Log audio backend info
    ma_backend backend = ma_engine_get_device(engine_)->pContext->backend;
    const char* backendName = "unknown";
    switch (backend) {
        case ma_backend_wasapi: backendName = "WASAPI"; break;
        case ma_backend_dsound: backendName = "DirectSound"; break;
        case ma_backend_winmm: backendName = "WinMM"; break;
        case ma_backend_coreaudio: backendName = "CoreAudio"; break;
        case ma_backend_sndio: backendName = "sndio"; break;
        case ma_backend_audio4: backendName = "audio(4)"; break;
        case ma_backend_oss: backendName = "OSS"; break;
        case ma_backend_pulseaudio: backendName = "PulseAudio"; break;
        case ma_backend_alsa: backendName = "ALSA"; break;
        case ma_backend_jack: backendName = "JACK"; break;
        case ma_backend_aaudio: backendName = "AAudio"; break;
        case ma_backend_opensl: backendName = "OpenSL|ES"; break;
        case ma_backend_webaudio: backendName = "WebAudio"; break;
        case ma_backend_custom: backendName = "Custom"; break;
        case ma_backend_null: backendName = "Null (no output)"; break;
        default: break;
    }

    initWorldBus();

    initialized_ = true;
    LOG_INFO("AudioEngine initialized (miniaudio, backend: ", backendName, ")");
    return true;
}

void AudioEngine::initWorldBus() {
    if (!engine_) return;
    const ma_uint32 channels = ma_engine_get_channels(engine_);
    auto* node = new WorldFxNode();
    node->fx.prepare(ma_engine_get_sample_rate(engine_), channels);
    ma_node_config config = ma_node_config_init();
    config.vtable = &gWorldFxVtable;
    config.pInputChannels = &channels;
    config.pOutputChannels = &channels;
    if (ma_node_init(ma_engine_get_node_graph(engine_), &config, nullptr, &node->base) != MA_SUCCESS) {
        LOG_WARNING("AudioEngine: world effects node failed; world sounds play dry");
        delete node;
        return;
    }
    if (ma_node_attach_output_bus(&node->base, 0, ma_engine_get_endpoint(engine_), 0) != MA_SUCCESS) {
        LOG_WARNING("AudioEngine: world effects node would not attach; world sounds play dry");
        ma_node_uninit(&node->base, nullptr);
        delete node;
        return;
    }
    auto* group = static_cast<ma_sound*>(std::malloc(sizeof(ma_sound)));
    if (!group || ma_sound_group_init(engine_, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, group) != MA_SUCCESS) {
        LOG_WARNING("AudioEngine: world sound group failed; world sounds play dry");
        std::free(group);
        ma_node_uninit(&node->base, nullptr);
        delete node;
        return;
    }
    // The group starts on the endpoint; moved onto the effects node instead.
    ma_node_attach_output_bus(group, 0, &node->base, 0);
    worldGroup_ = group;
    worldFxNode_ = node;
    LOG_INFO("AudioEngine: world bus up (reverb and underwater muffle), ", channels, " channels at ",
             ma_engine_get_sample_rate(engine_), " Hz");
}

void AudioEngine::releaseWorldBus() {
    // After every sound in the group is gone, before the engine.
    if (worldGroup_) {
        ma_sound_group_uninit(worldGroup_);
        std::free(worldGroup_);
        worldGroup_ = nullptr;
    }
    if (worldFxNode_) {
        auto* node = static_cast<WorldFxNode*>(worldFxNode_);
        ma_node_uninit(&node->base, nullptr);
        delete node;
        worldFxNode_ = nullptr;
    }
}

void AudioEngine::setWorldReverb(const ReverbSettings& reverb) {
    if (auto* node = static_cast<WorldFxNode*>(worldFxNode_)) node->fx.setReverb(reverb);
}

void AudioEngine::setWorldUnderwater(bool underwater) {
    if (auto* node = static_cast<WorldFxNode*>(worldFxNode_)) node->fx.setMuffle(underwater ? 1.0f : 0.0f);
}

std::string AudioEngine::getOutputDeviceName() const {
    if (!initialized_ || !engine_) return {};
    const ma_device* device = ma_engine_get_device(const_cast<ma_engine*>(engine_));
    if (!device || device->playback.name[0] == '\0') return {};
    return device->playback.name;
}

void AudioEngine::shutdown() {
    if (!initialized_) {
        return;
    }

    // Stop music, the outgoing track of a crossfade included
    stopMusic();
    releaseRetiringMusic();

    // Clean up all active sounds
    for (auto& activeSound : activeSounds_) {
        ma_sound_uninit(activeSound.sound);
        std::free(activeSound.sound);
        ma_audio_buffer* buffer = static_cast<ma_audio_buffer*>(activeSound.buffer);
        ma_audio_buffer_uninit(buffer);
        std::free(buffer);
    }
    activeSounds_.clear();
    releaseWorldBus();

    capturing_.store(false, std::memory_order_release);
    if (engine_) {
        ma_engine_uninit(engine_);
        delete engine_;
        engine_ = nullptr;
    }
    // The device is gone, so nothing writes to the ring any more.
    if (captureRing_) {
        auto* ring = static_cast<ma_pcm_rb*>(captureRing_);
        ma_pcm_rb_uninit(ring);
        delete ring;
        captureRing_ = nullptr;
    }

    initialized_ = false;
    LOG_INFO("AudioEngine shutdown");
}

void AudioEngine::setMasterVolume(float volume) {
    masterVolume_ = glm::clamp(volume, 0.0f, 1.0f);
    if (engine_) {
        if (!suspended_) ma_engine_set_volume(engine_, masterVolume_);
    }
}
void AudioEngine::setSuspended(bool suspended) {
    if (suspended_ == suspended) return;
    suspended_ = suspended;
    // The engine's own level is the one thing that changes; masterVolume_ is
    // left alone so the slider and the resume both still read it.
    if (engine_) ma_engine_set_volume(engine_, suspended_ ? 0.0f : masterVolume_);
}


void AudioEngine::setListenerPosition(const glm::vec3& position) {
    listenerPosition_ = position;
    if (engine_) {
        ma_engine_listener_set_position(engine_, 0, position.x, position.y, position.z);
    }
}

void AudioEngine::setListenerOrientation(const glm::vec3& forward, const glm::vec3& up) {
    listenerForward_ = forward;
    listenerUp_ = up;
    if (engine_) {
        ma_engine_listener_set_direction(engine_, 0, forward.x, forward.y, forward.z);
        ma_engine_listener_set_world_up(engine_, 0, up.x, up.y, up.z);
    }
}

bool AudioEngine::playSound2D(const std::vector<uint8_t>& wavData, float volume, float pitch,
                              SoundBus bus, const SoundOptions& opts) {
    // Size and volume, because this overload is handed decoded bytes and has
    // no name to report. The sample managers all cache their clips and call
    // this one, so the path-named log above never fires for them - and a sound
    // reported as playing loudly on every world entry produced no sfx: line at
    // all. A byte count identifies the file well enough to find it on disk.
    LOG_INFO("sfx2d: bytes=", wavData.size(), " vol=", volume);
    if (!initialized_ || !engine_ || wavData.empty()) return false;
    if (masterVolume_ <= 0.0f) return false;

    ActiveSound voice{};
    voice.baseVolume = volume;
    voice.priority = opts.priority;
    voice.kitId = opts.kitId;
    voice.category = opts.category;
    if (!admitVoice(voice, opts.noDuplicate)) return false;

    DecodedWavCacheEntry decoded;
    if (!decodeWavCached(wavData, decoded) || !decoded.pcmData || decoded.frames == 0) {
        return false;
    }

    // Create audio buffer from decoded PCM data (heap allocated to keep alive)
    ma_audio_buffer_config bufferConfig = ma_audio_buffer_config_init(
        decoded.format,
        decoded.channels,
        decoded.frames,
        decoded.pcmData->data(),
        nullptr  // No custom allocator
    );
    // Must set explicitly - miniaudio defaults to device sample rate, which causes
    // pitch distortion if it differs from the file's native rate (e.g. 22050 vs 44100 Hz).
    bufferConfig.sampleRate = decoded.sampleRate;

    AudioBufferStorage audioBuffer;
    if (!audioBuffer) return false;
    ma_result result = ma_audio_buffer_init(&bufferConfig, audioBuffer.get());
    if (result != MA_SUCCESS) {
        LOG_WARNING("Failed to create audio buffer: ", result);
        return false;
    }
    audioBuffer.markInitialised();

    // Create sound from audio buffer
    SoundStorage sound;
    if (!sound) return false;
    // Pitched only for a SoundEntries kit, whose random-pitch rows (0x400)
    // play their 2D copies retuned as well as their 3D ones. The hand-made
    // banks' pitch was never applied in 2D and stays that way.
    if (opts.kitId == 0) pitch = 1.0f;
    const ma_uint32 pitchFlag = pitch != 1.0f ? 0u : static_cast<ma_uint32>(MA_SOUND_FLAG_NO_PITCH);
    result = ma_sound_init_from_data_source(
        engine_,
        audioBuffer.get(),
        MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC | pitchFlag | MA_SOUND_FLAG_NO_SPATIALIZATION,
        groupFor(bus),
        sound.get()
    );

    if (result != MA_SUCCESS) {
        LOG_WARNING("Failed to create sound: ", result);
        return false;
    }
    sound.markInitialised();

    ma_sound_set_volume(sound.get(), volume);
    if (pitch != 1.0f) ma_sound_set_pitch(sound.get(), pitch);

    // Start playback
    result = ma_sound_start(sound.get());
    if (result != MA_SUCCESS) {
        LOG_WARNING("Failed to start sound: ", result);
        return false;
    }

    // Track this sound for cleanup (decoded PCM shared across plays)
    voice.sound = sound.release();
    voice.buffer = audioBuffer.release();
    voice.pcmDataRef = decoded.pcmData;
    activeSounds_.push_back(std::move(voice));

    return true;
}

uint32_t AudioEngine::playSound2DStoppable(const std::vector<uint8_t>& wavData, float volume,
                                           bool loop, float fadeInMs, SoundBus bus,
                                           const SoundOptions& opts) {
    if (!initialized_ || !engine_ || wavData.empty()) return 0;
    if (masterVolume_ <= 0.0f) return 0;

    ActiveSound voice{};
    voice.baseVolume = volume;
    voice.loop = loop;
    voice.priority = opts.priority;
    voice.kitId = opts.kitId;
    voice.category = opts.category;
    if (!admitVoice(voice, opts.noDuplicate)) return 0;

    DecodedWavCacheEntry decoded;
    if (!decodeWavCached(wavData, decoded) || !decoded.pcmData || decoded.frames == 0) return 0;

    ma_audio_buffer_config bufferConfig = ma_audio_buffer_config_init(
        decoded.format, decoded.channels, decoded.frames, decoded.pcmData->data(), nullptr);
    bufferConfig.sampleRate = decoded.sampleRate;

    AudioBufferStorage audioBuffer;
    if (!audioBuffer) return 0;
    if (ma_audio_buffer_init(&bufferConfig, audioBuffer.get()) != MA_SUCCESS) {
        return 0;
    }
    audioBuffer.markInitialised();

    SoundStorage sound;
    if (!sound) return 0;
    ma_result result = ma_sound_init_from_data_source(
        engine_, audioBuffer.get(),
        MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC | MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION,
        groupFor(bus), sound.get());
    if (result != MA_SUCCESS) {
        return 0;
    }
    sound.markInitialised();

    ma_sound_set_volume(sound.get(), volume);
    if (loop) ma_sound_set_looping(sound.get(), MA_TRUE);
    if (loop && opts.randomStart && decoded.frames > 1) {
        ma_sound_seek_to_pcm_frame(sound.get(), static_cast<ma_uint64>(std::rand()) % decoded.frames);
    }
    if (fadeInMs > 0.0f) {
        ma_sound_set_fade_in_milliseconds(sound.get(), 0.0f, volume, static_cast<ma_uint64>(fadeInMs));
    }
    if (ma_sound_start(sound.get()) != MA_SUCCESS) {
        return 0;
    }

    uint32_t id = nextSoundId_++;
    if (nextSoundId_ == 0) nextSoundId_ = 1;  // Skip 0 (sentinel)
    voice.sound = sound.release();
    voice.buffer = audioBuffer.release();
    voice.pcmDataRef = decoded.pcmData;
    voice.id = id;
    activeSounds_.push_back(std::move(voice));
    return id;
}

void AudioEngine::setSoundVolume(uint32_t id, float volume) {
    if (id == 0) return;
    for (auto& active : activeSounds_) {
        if (active.id == id) {
            active.baseVolume = volume;
            ma_sound_set_volume(active.sound, voiceGain(active));
            return;
        }
    }
}

void AudioEngine::fadeOutSound(uint32_t id, float fadeMs) {
    if (id == 0 || !engine_) return;
    for (auto& active : activeSounds_) {
        if (active.id != id) continue;
        const auto frames = static_cast<ma_uint64>(fadeMs / 1000.0f * ma_engine_get_sample_rate(engine_));
        ma_sound_set_fade_in_pcm_frames(active.sound, -1.0f, 0.0f, frames);
        ma_sound_set_stop_time_in_pcm_frames(active.sound, ma_engine_get_time_in_pcm_frames(engine_) + frames);
        active.id = 0;  // anonymous from here: nothing may stop or retune it twice
        return;
    }
}

bool AudioEngine::isSoundPlaying(uint32_t id) const {
    if (id == 0) return false;
    for (const auto& active : activeSounds_) {
        if (active.id == id) return ma_sound_is_playing(active.sound) == MA_TRUE;
    }
    return false;
}

void AudioEngine::stopSound(uint32_t id) {
    if (id == 0) return;
    for (auto it = activeSounds_.begin(); it != activeSounds_.end(); ++it) {
        if (it->id == id) {
            ma_sound_stop(it->sound);
            ma_sound_uninit(it->sound);
            std::free(it->sound);
            ma_audio_buffer* buffer = static_cast<ma_audio_buffer*>(it->buffer);
            ma_audio_buffer_uninit(buffer);
            std::free(buffer);
            activeSounds_.erase(it);
            return;
        }
    }
}

bool AudioEngine::playSound2D(const std::string& mpqPath, float volume, float pitch, SoundBus bus,
                              const SoundOptions& opts) {
    // Which file, and how loud, for the one-shots that name a path.
    //
    // A sound reported as playing loudly on every world entry cannot be found
    // by reading: a dozen managers reach this and none of them is obviously the
    // one. This says so in a line, and the timestamp beside the world-entry
    // lines in the same log is what pins it.
    LOG_INFO("sfx: ", mpqPath, " vol=", volume);
    if (!assetManager_) {
        LOG_WARNING("AudioEngine::playSound2D(path): no AssetManager set");
        return false;
    }
    auto data = assetManager_->readFile(mpqPath);
    if (data.empty()) {
        LOG_WARNING("AudioEngine::playSound2D: failed to load '", mpqPath, "'");
        return false;
    }
    return playSound2D(data, volume, pitch, bus, opts);
}

bool AudioEngine::playSound3D(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                              float volume, float pitch, float maxDistance, const SoundOptions& opts) {
    return startSound3D(wavData, position, volume, pitch, maxDistance, false, false, opts) != 0;
}

uint32_t AudioEngine::playSound3DStoppable(const std::string& mpqPath, const glm::vec3& position,
                                           float volume, float maxDistance, bool loop,
                                           float minDistance, const SoundOptions& opts) {
    if (!assetManager_) return 0;
    auto data = assetManager_->readFile(mpqPath);
    if (data.empty()) {
        LOG_WARNING("AudioEngine::playSound3DStoppable: failed to load '", mpqPath, "'");
        return 0;
    }
    SoundOptions o = opts;
    if (o.minDistance <= 0.0f) o.minDistance = minDistance;
    return startSound3D(data, position, volume, 1.0f, maxDistance, loop, true, o);
}

void AudioEngine::setSoundPosition(uint32_t id, const glm::vec3& position) {
    if (id == 0) return;
    for (auto& active : activeSounds_) {
        if (active.id == id) {
            active.position = position;
            ma_sound_set_position(active.sound, position.x, position.y, position.z);
            ma_sound_set_volume(active.sound, voiceGain(active));
            return;
        }
    }
}

uint32_t AudioEngine::startSound3D(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                                   float volume, float pitch, float maxDistance, bool loop,
                                   bool stoppable, const SoundOptions& opts) {
    if (!initialized_ || !engine_ || wavData.empty()) return 0;
    if (masterVolume_ <= 0.0f) return 0;

    ActiveSound voice{};
    voice.baseVolume = volume;
    voice.spatial = true;
    voice.loop = loop;
    voice.position = position;
    voice.maxDistance = maxDistance > 0.0f ? maxDistance : stock::kDefaultCutoff;
    voice.minDistance = opts.minDistance > 0.0f ? opts.minDistance : stock::kDefaultMinDistance;
    voice.priority = opts.priority;
    voice.kitId = opts.kitId;
    voice.category = opts.category;
    // Past its cutoff a sound is silent from the first sample: not started.
    if (voiceGain(voice) <= 0.0f && !loop) return 0;
    if (!admitVoice(voice, opts.noDuplicate)) return 0;

    DecodedWavCacheEntry decoded;
    if (!decodeWavCached(wavData, decoded) || !decoded.pcmData || decoded.frames == 0) {
        return 0;
    }

    LOG_DEBUG("playSound3D: cached WAV - format:", decoded.format,
              " channels:", decoded.channels, " sampleRate:", decoded.sampleRate,
              " pitch:", pitch);

    // Create audio buffer with correct sample rate
    ma_audio_buffer_config bufferConfig = ma_audio_buffer_config_init(
        decoded.format,
        decoded.channels,
        decoded.frames,
        decoded.pcmData->data(),
        nullptr
    );
    // Must set explicitly - miniaudio defaults to device sample rate, which causes
    // pitch distortion if it differs from the file's native rate (e.g. 22050 vs 44100 Hz).
    bufferConfig.sampleRate = decoded.sampleRate;

    AudioBufferStorage audioBuffer;
    if (!audioBuffer) return false;
    ma_result result = ma_audio_buffer_init(&bufferConfig, audioBuffer.get());
    if (result != MA_SUCCESS) {
        return false;
    }
    audioBuffer.markInitialised();

    // Create 3D sound (spatialization enabled, pitch enabled)
    SoundStorage sound;
    if (!sound) return false;
    result = ma_sound_init_from_data_source(
        engine_,
        audioBuffer.get(),
        MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC,  // Removed NO_PITCH flag
        groupFor(SoundBus::World),
        sound.get()
    );

    if (result != MA_SUCCESS) {
        LOG_WARNING("playSound3D: Failed to create sound, error: ", result);
        return false;
    }
    sound.markInitialised();

    // Panned by direction; the distance gain is the stock rolloff callback's
    // (stock::rolloffGain), applied through the volume here and in update()
    // as the listener moves: miniaudio's own models have no fade to zero at
    // the cutoff.
    placeForPanning(sound.get(), position);
    ma_sound_set_volume(sound.get(), voiceGain(voice));
    ma_sound_set_pitch(sound.get(), pitch);
    if (loop) ma_sound_set_looping(sound.get(), MA_TRUE);
    if (loop && opts.randomStart && decoded.frames > 1) {
        ma_sound_seek_to_pcm_frame(sound.get(), static_cast<ma_uint64>(std::rand()) % decoded.frames);
    }

    result = ma_sound_start(sound.get());
    if (result != MA_SUCCESS) {
        return 0;
    }

    // Track for cleanup; a stoppable sound gets an id its owner can stop,
    // move or fade it by, an anonymous one plays out and is reclaimed.
    uint32_t id = 0;
    if (stoppable) {
        id = nextSoundId_++;
        if (nextSoundId_ == 0) nextSoundId_ = 1;  // Skip 0 (sentinel)
    }
    voice.sound = sound.release();
    voice.buffer = audioBuffer.release();
    voice.pcmDataRef = decoded.pcmData;
    voice.id = id;
    activeSounds_.push_back(std::move(voice));

    return stoppable ? id : 1u;
}

bool AudioEngine::playSound3D(const std::string& mpqPath, const glm::vec3& position,
                              float volume, float pitch, float maxDistance, const SoundOptions& opts) {
    if (!assetManager_) {
        LOG_WARNING("AudioEngine::playSound3D(path): no AssetManager set");
        return false;
    }
    auto data = assetManager_->readFile(mpqPath);
    if (data.empty()) {
        LOG_WARNING("AudioEngine::playSound3D: failed to load '", mpqPath, "'");
        return false;
    }
    return playSound3D(data, position, volume, pitch, maxDistance, opts);
}

bool AudioEngine::playMusic(std::shared_ptr<const std::vector<uint8_t>> musicData,
                            float volume, bool loop) {
    if (!initialized_ || !engine_ || !musicData || musicData->empty()) {
        return false;
    }

    LOG_INFO("AudioEngine::playMusic - data size: ", musicData->size(), " bytes, volume: ", volume);

    // Stop any currently playing music
    stopMusic();

    // Keep the encoded bytes alive for as long as miniaudio's decoder streams them.
    musicData_ = std::move(musicData);
    musicVolume_ = volume;

    // Create decoder from memory (for streaming MP3/OGG)
    ma_decoder* decoder = new ma_decoder();
    ma_decoder_config decoderConfig = ma_decoder_config_init_default();
    ma_result result = ma_decoder_init_memory(
        musicData_->data(),
        musicData_->size(),
        &decoderConfig,
        decoder
    );

    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to create music decoder: ", result);
        delete decoder;
        musicData_.reset();
        return false;
    }

    LOG_INFO("Decoder created - format: ", decoder->outputFormat,
             ", channels: ", decoder->outputChannels,
             ", sampleRate: ", decoder->outputSampleRate);

    musicDecoder_ = decoder;

    // Create streaming sound from decoder
    musicSound_ = static_cast<ma_sound*>(std::malloc(sizeof(ma_sound)));
    if (!musicSound_) {
        ma_decoder_uninit(decoder);
        delete decoder;
        musicDecoder_ = nullptr;
        musicData_.reset();
        return false;
    }
    result = ma_sound_init_from_data_source(
        engine_,
        decoder,
        MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION,
        nullptr,
        musicSound_
    );

    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to create music sound: ", result);
        ma_decoder_uninit(decoder);
        delete decoder;
        musicDecoder_ = nullptr;
        std::free(musicSound_);
        musicSound_ = nullptr;
        musicData_.reset();
        return false;
    }

    // Set volume and looping
    ma_sound_set_volume(musicSound_, volume);
    ma_sound_set_looping(musicSound_, loop ? MA_TRUE : MA_FALSE);

    // Start playback
    result = ma_sound_start(musicSound_);
    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to start music playback: ", result);
        ma_sound_uninit(musicSound_);
        std::free(musicSound_);
        musicSound_ = nullptr;
        ma_decoder_uninit(decoder);
        delete decoder;
        musicDecoder_ = nullptr;
        // The two failure paths above release this and this one did not, so a
        // failed start pinned the whole encoded file until the next playMusic
        // reassigned the member.
        musicData_.reset();
        return false;
    }

    LOG_INFO("Music playback started successfully - volume: ", volume,
             ", loop: ", loop,
             ", is_playing: ", ma_sound_is_playing(musicSound_));

    return true;
}

void AudioEngine::stopMusic() {
    if (musicSound_) {
        ma_sound_uninit(musicSound_);
        std::free(musicSound_);
        musicSound_ = nullptr;
    }
    if (musicDecoder_) {
        ma_decoder* decoder = static_cast<ma_decoder*>(musicDecoder_);
        ma_decoder_uninit(decoder);
        delete decoder;
        musicDecoder_ = nullptr;
    }
    musicData_.reset();
}

void AudioEngine::releaseRetiringMusic() {
    if (retiringMusicSound_) {
        ma_sound_uninit(retiringMusicSound_);
        std::free(retiringMusicSound_);
        retiringMusicSound_ = nullptr;
    }
    if (retiringMusicDecoder_) {
        ma_decoder* decoder = static_cast<ma_decoder*>(retiringMusicDecoder_);
        ma_decoder_uninit(decoder);
        delete decoder;
        retiringMusicDecoder_ = nullptr;
    }
    retiringMusicData_.reset();
}

void AudioEngine::fadeOutMusicVoice(float fadeMs) {
    if (!musicSound_) return;
    // Two at once is the most there is: a third track arriving mid-fade cuts
    // the oldest, which by then is mostly faded anyway.
    releaseRetiringMusic();
    retiringMusicSound_ = musicSound_;
    retiringMusicDecoder_ = musicDecoder_;
    retiringMusicData_ = std::move(musicData_);
    musicSound_ = nullptr;
    musicDecoder_ = nullptr;
    musicData_.reset();
    // The fader multiplies the voice's volume rather than replacing it, so the
    // fade starts from wherever the track was - mid fade-in included - and a
    // loop flag cannot carry it past its end.
    ma_sound_set_looping(retiringMusicSound_, MA_FALSE);
    const float ms = std::max(fadeMs, 1.0f);
    ma_sound_stop_with_fade_in_milliseconds(retiringMusicSound_, static_cast<ma_uint64>(ms));
}

bool AudioEngine::isMusicPlaying() const {
    if (!musicSound_) {
        return false;
    }
    return ma_sound_is_playing(musicSound_) == MA_TRUE;
}

void AudioEngine::setMusicVolume(float volume) {
    musicVolume_ = glm::clamp(volume, 0.0f, 1.0f);
    if (musicSound_) {
        ma_sound_set_volume(musicSound_, musicVolume_);
    }
}

void AudioEngine::update(float deltaTime) {
    (void)deltaTime;

    if (!initialized_ || !engine_) {
        return;
    }

    // The outgoing track of a crossfade, once its fade has stopped it.
    if (retiringMusicSound_ && !ma_sound_is_playing(retiringMusicSound_)) {
        releaseRetiringMusic();
    }

    // Clean up finished sounds - swap-and-pop avoids the O(N) shift that
    // vector::erase does for each removal (and the ref-count atomics in
    // ActiveSound's shared_ptr made that shift noticeably more expensive).
    for (size_t i = 0; i < activeSounds_.size(); ) {
        if (!ma_sound_is_playing(activeSounds_[i].sound)) {
            releaseActive(i);
            continue;
        }
        ActiveSound& s = activeSounds_[i];
        // The listener moves: positional sounds follow the rolloff.
        if (s.spatial) ma_sound_set_volume(s.sound, voiceGain(s));
        // A one-shot heard playing outranks a newcomer at the default
        // priority from then on (0x87a580), so a burst of new sounds takes
        // free voices rather than cutting the ones already under way.
        if (s.id == 0 && !s.loop && s.priority == stock::kPriorityDefault) s.priority = stock::kPriorityHeard;
        ++i;
    }
}

float AudioEngine::voiceGain(const ActiveSound& s) const {
    if (!s.spatial) return s.baseVolume;
    const float d = glm::distance(s.position, listenerPosition_);
    return s.baseVolume * stock::rolloffGain(d, s.minDistance, s.maxDistance);
}

void AudioEngine::releaseActive(size_t i) {
    ma_sound_uninit(activeSounds_[i].sound);
    std::free(activeSounds_[i].sound);
    ma_audio_buffer* buffer = static_cast<ma_audio_buffer*>(activeSounds_[i].buffer);
    ma_audio_buffer_uninit(buffer);
    std::free(buffer);
    activeSounds_[i] = std::move(activeSounds_.back());
    activeSounds_.pop_back();
}

void AudioEngine::setNumChannels(int channels) {
    numChannels_ = std::clamp(channels, stock::kMinNumChannels, stock::kMaxNumChannels);
}

bool AudioEngine::admitVoice(const ActiveSound& incoming, bool noDuplicate) {
    // Music and the outgoing track of a crossfade hold voices at priority
    // 100, which no sound effect outranks: they come off the top.
    int reserved = 0;
    if (musicSound_ && ma_sound_is_playing(musicSound_)) ++reserved;
    if (retiringMusicSound_ && ma_sound_is_playing(retiringMusicSound_)) ++reserved;

    std::vector<stock::Voice> voices;
    std::vector<size_t> index;
    voices.reserve(activeSounds_.size());
    index.reserve(activeSounds_.size());
    for (size_t i = 0; i < activeSounds_.size(); ++i) {
        const ActiveSound& s = activeSounds_[i];
        if (!ma_sound_is_playing(s.sound)) continue;  // finished, reclaimed next update
        voices.push_back({s.priority, voiceGain(s), s.kitId, s.category});
        index.push_back(i);
    }
    const stock::Voice want{incoming.priority, voiceGain(incoming), incoming.kitId, incoming.category};
    stock::VoiceDecision d = stock::decideVoice(voices, want, numChannels_ - reserved, noDuplicate);
    if (!d.admit) {
        LOG_DEBUG("sound refused: kit ", incoming.kitId, " category ", static_cast<int>(incoming.category),
                  " reason ", static_cast<int>(d.reason));
        return false;
    }
    // Highest index first, so swap-and-pop never moves one still to go.
    std::vector<size_t> evict;
    for (size_t v : d.evict) evict.push_back(index[v]);
    std::sort(evict.rbegin(), evict.rend());
    for (size_t i : evict) {
        ma_sound_stop(activeSounds_[i].sound);
        releaseActive(i);
    }
    return true;
}

void AudioEngine::deviceDataCallback(ma_device* device, void* out, const void* in, uint32_t frames) {
    (void)in;
    // What miniaudio's own callback does, less an Emscripten-only job pump.
    auto* engine = static_cast<ma_engine*>(device->pUserData);
    ma_engine_read_pcm_frames(engine, out, frames, nullptr);

    AudioEngine& self = instance();
    if (!self.capturing_.load(std::memory_order_acquire)) return;
    auto* ring = static_cast<ma_pcm_rb*>(self.captureRing_);
    const ma_uint32 channels = ma_engine_get_channels(engine);
    const auto* src = static_cast<const float*>(out);
    // Whatever does not fit is dropped: the recorder has fallen a second
    // behind, and the audio thread is the one thing that must never wait.
    ma_uint32 remaining = frames;
    while (remaining > 0) {
        ma_uint32 chunk = remaining;
        void* dst = nullptr;
        if (ma_pcm_rb_acquire_write(ring, &chunk, &dst) != MA_SUCCESS || chunk == 0) break;
        std::memcpy(dst, src, static_cast<size_t>(chunk) * channels * sizeof(float));
        ma_pcm_rb_commit_write(ring, chunk);
        src += static_cast<size_t>(chunk) * channels;
        remaining -= chunk;
    }
}

bool AudioEngine::beginOutputCapture() {
    if (!initialized_ || !engine_) return false;
    if (!captureRing_) {
        // A second of the device's output: the recorder drains it every few
        // tens of milliseconds, so this is room for a long stall.
        auto* ring = new ma_pcm_rb();
        if (ma_pcm_rb_init(ma_format_f32, ma_engine_get_channels(engine_),
                           ma_engine_get_sample_rate(engine_), nullptr, nullptr, ring) != MA_SUCCESS) {
            delete ring;
            LOG_WARNING("AudioEngine: could not make the capture buffer - recording without sound");
            return false;
        }
        captureRing_ = ring;
    }
    // Drop what an earlier capture left unread. The reader's side, so it is
    // safe with the audio thread writing.
    auto* ring = static_cast<ma_pcm_rb*>(captureRing_);
    const ma_uint32 stale = ma_pcm_rb_available_read(ring);
    if (stale > 0) ma_pcm_rb_seek_read(ring, stale);
    capturing_.store(true, std::memory_order_release);
    return true;
}

void AudioEngine::endOutputCapture() {
    capturing_.store(false, std::memory_order_release);
}

uint32_t AudioEngine::readCapturedOutput(float* dst, uint32_t maxFrames) {
    if (!captureRing_ || !dst || maxFrames == 0) return 0;
    auto* ring = static_cast<ma_pcm_rb*>(captureRing_);
    const ma_uint32 channels = ma_pcm_rb_get_channels(ring);
    uint32_t read = 0;
    while (read < maxFrames) {
        ma_uint32 chunk = maxFrames - read;
        void* src = nullptr;
        if (ma_pcm_rb_acquire_read(ring, &chunk, &src) != MA_SUCCESS || chunk == 0) break;
        std::memcpy(dst + static_cast<size_t>(read) * channels, src,
                    static_cast<size_t>(chunk) * channels * sizeof(float));
        ma_pcm_rb_commit_read(ring, chunk);
        read += chunk;
    }
    return read;
}

uint32_t AudioEngine::capturedOutputAvailable() const {
    if (!captureRing_) return 0;
    return ma_pcm_rb_available_read(static_cast<ma_pcm_rb*>(captureRing_));
}

uint32_t AudioEngine::getOutputChannels() const {
    return engine_ ? ma_engine_get_channels(engine_) : 0;
}

uint32_t AudioEngine::getOutputSampleRate() const {
    return engine_ ? ma_engine_get_sample_rate(engine_) : 0;
}

} // namespace audio
} // namespace wowee
