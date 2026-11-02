#pragma once

#include "audio/stock_sound_rules.hpp"

#include <glm/glm.hpp>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

// Forward declare miniaudio types to avoid exposing implementation in header
struct ma_engine;
struct ma_sound;
struct ma_device;

namespace wowee {
namespace pipeline { class AssetManager; }
namespace audio {

struct ReverbSettings;

/// Where a sound is mixed. World sounds pass through the room's reverb and
/// the underwater muffle (WorldFx); interface sounds and music do not - a
/// button click is not in the cave with the player.
enum class SoundBus { World, Interface };

/// How a sound competes for a voice and fades with distance, the stock way
/// (stock_sound_rules.hpp). The defaults are a bare file at FMOD's default
/// priority with SoundEntries' commonest MinDistance.
struct SoundOptions {
    /// SoundEntries.MinDistance: full volume inside it. 0 or less: 8 yd.
    float minDistance = -1.0f;
    /// SoundEntries id, for the three-voices-per-kit limit. 0 for none.
    uint32_t kitId = 0;
    int priority = stock::kPriorityDefault;
    stock::Category category = stock::Category::Sfx;
    /// SoundEntries flag 0x20: refused while the same kit is playing.
    bool noDuplicate = false;
    /// A loop that is not music starts at a random point of itself
    /// (Wow.exe 0x879760).
    bool randomStart = false;
};

/**
 * AudioEngine: Singleton managing miniaudio device and playback.
 * Replaces process-spawning audio system with proper non-blocking library.
 */
class AudioEngine {
public:
    static AudioEngine& instance();

    ~AudioEngine();

    // Initialization
    [[nodiscard]] bool initialize();
    void shutdown();
    [[nodiscard]] bool isInitialized() const { return initialized_; }

    /// Sound_NumChannels: how many sounds may play at once, music included.
    /// Clamped to the stock 12..128; 32 by default.
    void setNumChannels(int channels);
    [[nodiscard]] int getNumChannels() const { return numChannels_; }

    /// The playback device miniaudio actually opened, by name.
    ///
    /// For the Sound panel's output dropdown, which lists the drivers the
    /// client can use. This client opens whichever device the system offers
    /// and does not switch between them, so the list is this one name - the
    /// truth rather than a guess, and the same shape the video panel's
    /// resolution list already takes. Empty if the engine is not up.
    [[nodiscard]] std::string getOutputDeviceName() const;

    // Master volume (0.0 = silent, 1.0 = full)
    void setMasterVolume(float volume);
    [[nodiscard]] float getMasterVolume() const { return masterVolume_; }

    /// Silence the output without forgetting the volume the player chose.
    ///
    /// For the window losing focus, where the client should go quiet and come
    /// back at the same level. Winding masterVolume_ down to zero instead
    /// would work once and then answer the volume slider wrongly, and would
    /// trip the masterVolume_ <= 0 early-outs that stop a sound being started
    /// at all - so a suspended client would come back to silence where a
    /// looping track used to be.
    void setSuspended(bool suspended);
    [[nodiscard]] bool isSuspended() const { return suspended_; }

    // Asset manager (enables sound loading by MPQ path)
    void setAssetManager(pipeline::AssetManager* am) { assetManager_ = am; }

    // 3D listener position (for positional audio)
    void setListenerPosition(const glm::vec3& position);
    void setListenerOrientation(const glm::vec3& forward, const glm::vec3& up);
    [[nodiscard]] const glm::vec3& getListenerPosition() const { return listenerPosition_; }

    // Simple 2D sound playback (non-blocking)
    bool playSound2D(const std::vector<uint8_t>& wavData, float volume = 1.0f, float pitch = 1.0f,
                     SoundBus bus = SoundBus::World, const SoundOptions& opts = {});
    bool playSound2D(const std::string& mpqPath, float volume = 1.0f, float pitch = 1.0f,
                     SoundBus bus = SoundBus::World, const SoundOptions& opts = {});

    // Stoppable 2D sound - returns a non-zero handle, or 0 on failure
    uint32_t playSound2DStoppable(const std::vector<uint8_t>& wavData, float volume = 1.0f,
                                  bool loop = false, float fadeInMs = 0.0f,
                                  SoundBus bus = SoundBus::World, const SoundOptions& opts = {});

    /// The room the world's sounds are heard in; WorldFx eases to it over
    /// about a second. A wet of 0 is a dry mix.
    void setWorldReverb(const ReverbSettings& reverb);
    /// Muffle the world's sounds as heard from under water, eased in and out
    /// over a third of a second. Interface sounds and music stay clear.
    void setWorldUnderwater(bool underwater);
    /// Change a stoppable sound's volume while it plays.
    void setSoundVolume(uint32_t id, float volume);
    /// Fade a stoppable sound to silence over fadeMs and stop it. The id is
    /// dead on return; the update loop reclaims the sound once it stops.
    void fadeOutSound(uint32_t id, float fadeMs);
    // Stop a sound started with playSound2DStoppable (no-op if already finished)
    void stopSound(uint32_t id);
    /// True while a stoppable sound is still sounding: for "never talk over
    /// your own last line", the stock client's handle check (0x4c5be0).
    [[nodiscard]] bool isSoundPlaying(uint32_t id) const;

    // 3D positional sound playback
    ///
    /// Heard through the stock rolloff (stock::rolloffGain): full volume
    /// inside opts.minDistance, inverse falloff at scale 4 beyond it, a
    /// linear fade over the last tenth of maxDistance (SoundEntries
    /// DistanceCutoff) and silence past it. Panned by direction, no Doppler.
    bool playSound3D(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                     float volume = 1.0f, float pitch = 1.0f, float maxDistance = stock::kDefaultCutoff,
                     const SoundOptions& opts = {});
    bool playSound3D(const std::string& mpqPath, const glm::vec3& position,
                     float volume = 1.0f, float pitch = 1.0f, float maxDistance = stock::kDefaultCutoff,
                     const SoundOptions& opts = {});
    /// A positional sound its owner keeps hold of - a missile's flight loop.
    /// Returns a non-zero id for stopSound / fadeOutSound / setSoundPosition,
    /// or 0 on failure. A looping one plays until it is stopped.
    /// minDistance, when given, is how far out it stays at full volume (a
    /// SoundEntries row's MinDistance); otherwise the engine's default.
    uint32_t playSound3DStoppable(const std::string& mpqPath, const glm::vec3& position,
                                  float volume = 1.0f, float maxDistance = stock::kDefaultCutoff,
                                  bool loop = false, float minDistance = -1.0f,
                                  const SoundOptions& opts = {});
    /// Move a stoppable 3D sound (no-op once it has finished).
    void setSoundPosition(uint32_t id, const glm::vec3& position);

    // Music streaming (for background music)
    // Retains shared ownership: the decoder streams directly from encoded tracks that
    // run to several MB, so cached music must not be copied for every playback.
    bool playMusic(std::shared_ptr<const std::vector<uint8_t>> musicData,
                   float volume = 1.0f, bool loop = true);
    void stopMusic();
    /// Hand the playing track to a second voice that fades it to silence over
    /// fadeMs and then stops, leaving the main voice free for the next track
    /// to fade in over it - a crossfade. The fade runs on the audio thread, so
    /// it finishes even on a frame that never calls update(); update() frees
    /// the voice once it has stopped. A track already fading is cut short.
    void fadeOutMusicVoice(float fadeMs);
    [[nodiscard]] bool isMusicPlaying() const;
    void setMusicVolume(float volume);

    // Update (call once per frame for cleanup/position sync)
    void update(float deltaTime);

    /// Keep a copy of what the speakers play, for the screen recorder.
    ///
    /// Taken in the device's own callback after the mix and the master volume,
    /// so a recording sounds as the session did - muted or suspended included.
    /// Anything captured before this call is discarded. False when there is no
    /// device to listen to, in which case a recording goes without sound.
    bool beginOutputCapture();
    void endOutputCapture();
    /// Up to maxFrames interleaved float frames captured since the last read,
    /// oldest first. One reader at a time; any thread.
    uint32_t readCapturedOutput(float* dst, uint32_t maxFrames);
    /// Frames captured and not yet read.
    [[nodiscard]] uint32_t capturedOutputAvailable() const;
    [[nodiscard]] uint32_t getOutputChannels() const;
    [[nodiscard]] uint32_t getOutputSampleRate() const;

private:
    AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    /// Start a positional sound. Returns its id when stoppable, else 1 on
    /// success; 0 on failure.
    uint32_t startSound3D(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                          float volume, float pitch, float maxDistance, bool loop,
                          bool stoppable, const SoundOptions& opts);

    // Track active one-shot sounds for cleanup
    struct ActiveSound {
        ma_sound* sound;
        void* buffer;  // ma_audio_buffer* - Keep audio buffer alive
        std::shared_ptr<const std::vector<uint8_t>> pcmDataRef;  // Keep decoded PCM alive
        uint32_t id = 0;  // 0 = anonymous (not stoppable)
        // What the voice limiter and the rolloff need.
        float baseVolume = 1.0f;      ///< the caller's volume, before distance
        bool spatial = false;
        bool loop = false;
        glm::vec3 position{0.0f};
        float minDistance = stock::kDefaultMinDistance;
        float maxDistance = stock::kDefaultCutoff;
        int priority = stock::kPriorityDefault;
        uint32_t kitId = 0;
        stock::Category category = stock::Category::Sfx;
    };
    /// The volume a voice is mixed at: its own, times the rolloff for a
    /// positional one at its distance from the listener.
    [[nodiscard]] float voiceGain(const ActiveSound& s) const;
    /// Make room for a sound by the stock rules, stopping what it displaces.
    /// False when the sound may not start.
    bool admitVoice(const ActiveSound& incoming, bool noDuplicate);
    void releaseActive(size_t index);
    int numChannels_ = stock::kDefaultNumChannels;
    std::vector<ActiveSound> activeSounds_;
    uint32_t nextSoundId_ = 1;

    // Music track state
    ma_sound* musicSound_ = nullptr;
    void* musicDecoder_ = nullptr;  // ma_decoder* - Keep decoder alive for streaming
    std::shared_ptr<const std::vector<uint8_t>> musicData_;  // Keep encoded music data alive
    float musicVolume_ = 1.0f;
    // The outgoing track of a crossfade; see fadeOutMusicVoice.
    ma_sound* retiringMusicSound_ = nullptr;
    void* retiringMusicDecoder_ = nullptr;
    std::shared_ptr<const std::vector<uint8_t>> retiringMusicData_;
    void releaseRetiringMusic();

    bool initialized_ = false;
    float masterVolume_ = 1.0f;
    bool suspended_ = false;
    glm::vec3 listenerPosition_{0.0f, 0.0f, 0.0f};
    glm::vec3 listenerForward_{0.0f, 0.0f, -1.0f};
    glm::vec3 listenerUp_{0.0f, 1.0f, 0.0f};

    pipeline::AssetManager* assetManager_ = nullptr;

    // miniaudio engine (opaque pointer)
    ma_engine* engine_ = nullptr;

    // The world bus: a sound group (an ma_sound_group is an ma_sound) feeding
    // the WorldFx node, which feeds the endpoint. Null when either failed to
    // come up, and world sounds then go straight to the endpoint.
    ma_sound* worldGroup_ = nullptr;
    void* worldFxNode_ = nullptr;  // WorldFxNode*, see audio_engine.cpp
    void initWorldBus();
    void releaseWorldBus();
    /// The group a sound on this bus is started in; null is the endpoint.
    [[nodiscard]] ma_sound* groupFor(SoundBus bus) const {
        return bus == SoundBus::World ? worldGroup_ : nullptr;
    }

    /// Replaces miniaudio's own device callback: it mixes exactly as that one
    /// does, then hands the result to the capture ring when one is open.
    static void deviceDataCallback(ma_device* device, void* out, const void* in, uint32_t frames);
    // ma_pcm_rb, which miniaudio declares anonymously and so cannot be named
    // here. Made on the first capture and kept until shutdown: freeing it while
    // the audio thread might still be writing is the one race not worth having.
    void* captureRing_ = nullptr;
    std::atomic<bool> capturing_{false};
};

} // namespace audio
} // namespace wowee
