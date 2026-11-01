#pragma once

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
    bool playSound2D(const std::vector<uint8_t>& wavData, float volume = 1.0f, float pitch = 1.0f);
    bool playSound2D(const std::string& mpqPath, float volume = 1.0f, float pitch = 1.0f);

    // Stoppable 2D sound - returns a non-zero handle, or 0 on failure
    uint32_t playSound2DStoppable(const std::vector<uint8_t>& wavData, float volume = 1.0f,
                                  bool loop = false, float fadeInMs = 0.0f);
    /// Change a stoppable sound's volume while it plays.
    void setSoundVolume(uint32_t id, float volume);
    /// Fade a stoppable sound to silence over fadeMs and stop it. The id is
    /// dead on return; the update loop reclaims the sound once it stops.
    void fadeOutSound(uint32_t id, float fadeMs);
    // Stop a sound started with playSound2DStoppable (no-op if already finished)
    void stopSound(uint32_t id);

    // 3D positional sound playback
    bool playSound3D(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                     float volume = 1.0f, float pitch = 1.0f, float maxDistance = 100.0f);
    bool playSound3D(const std::string& mpqPath, const glm::vec3& position,
                     float volume = 1.0f, float pitch = 1.0f, float maxDistance = 100.0f);
    /// A positional sound its owner keeps hold of - a missile's flight loop.
    /// Returns a non-zero id for stopSound / fadeOutSound / setSoundPosition,
    /// or 0 on failure. A looping one plays until it is stopped.
    /// minDistance, when given, is how far out it stays at full volume (a
    /// SoundEntries row's MinDistance); otherwise the engine's default.
    uint32_t playSound3DStoppable(const std::string& mpqPath, const glm::vec3& position,
                                  float volume = 1.0f, float maxDistance = 100.0f,
                                  bool loop = false, float minDistance = -1.0f);
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
                          bool stoppable, float minDistance = -1.0f);

    // Track active one-shot sounds for cleanup
    struct ActiveSound {
        ma_sound* sound;
        void* buffer;  // ma_audio_buffer* - Keep audio buffer alive
        std::shared_ptr<const std::vector<uint8_t>> pcmDataRef;  // Keep decoded PCM alive
        uint32_t id = 0;  // 0 = anonymous (not stoppable)
    };
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
