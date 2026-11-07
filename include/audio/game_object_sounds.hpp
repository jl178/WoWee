#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace audio {

class SoundData;
struct SoundEntry;

/// The sounds game objects and their doodads make from their own model
/// events, the way the stock 3.3.5a client plays them.
///
/// A game object's model (every type with the default handler, Wow.exe
/// 0x70cac0 -> handler +0x40 -> 0x70c050):
///  - $GO0..$GO5 and $GC0..$GC3 play GameObjectDisplayInfo Sound[0..5] and
///    Sound[6..9] at the event (0x70bf70). A one-shot row just plays; a
///    looping row (flag 0x200) plays on the object's one handle (handler
///    +0x1c), and not while that handle still sounds or while the same row
///    plays within 6 yards (0x4cfe00, 0x9e8cf8); keep-alive, no fades.
///    The doors' swings and slams, the chests' lids, the bobber's bite.
///  - $DSL: its data's loop on the same handle, 1 s fades in and out.
///  - $DSO, $SND: its data's row once at the event.
///  - The handle is stopped when the object changes state (0x70d690) and
///    when it goes.
///  - Transports (types 11 and 15) fire nothing (their +0x40 is 0x6b1b80,
///    an empty function).
/// A doodad of a map object (0x7bd5a0): $DSL a loop on the doodad's own
/// handle (3 s fade in, the same 6-yard rule), $DSE stops it, $DSO once.
/// Here: the doodads of a transport - the zeppelin's engines
/// (ZepAnimation.m2's 5457, 5458), the icebreaker's paddlewheel (11840) -
/// whose loops are moved with the ship.
class GameObjectSounds {
public:
    void setSoundData(const SoundData* data) { soundData_ = data; }
    /// GameObjectDisplayInfo.dbc's Sound[10] per display.
    void loadDisplaySounds(pipeline::AssetManager* assets);
    void setDisplaySounds(uint32_t displayId, const std::array<uint32_t, 10>& sounds) {
        displaySounds_[displayId] = sounds;
    }
    [[nodiscard]] uint32_t displaySound(uint32_t displayId, int slot) const;

    /// A game object's model instance.
    void addObject(uint32_t instanceId, uint32_t displayId);
    /// A doodad that moves with a transport.
    void addDoodad(uint32_t instanceId);
    /// The instance is gone: its loop stops.
    void remove(uint32_t instanceId);
    /// The object changed state (0x70d690): its loop stops.
    void stopLoop(uint32_t instanceId);

    /// One fired model event (M2Renderer::FiredObjectEvent), render coords.
    void onEvent(uint32_t instanceId, uint32_t tag, uint32_t data, const glm::vec3& position);

    /// Keep the loops on their instances as these move (a transport's).
    /// `matrixOf` gives an instance's model matrix, false when it is gone.
    void update(const std::function<bool(uint32_t, glm::mat4&)>& matrixOf);

    void clear();

    /// For tests: what onEvent asked for, instead of the engine.
    struct Request {
        uint32_t instanceId = 0;
        uint32_t soundId = 0;
        bool loop = false;
        float fadeInMs = 0.0f;
        glm::vec3 position{0.0f};
    };
    void setRecorder(std::function<uint32_t(const Request&)> recorder) { recorder_ = std::move(recorder); }
    void setPlayingQuery(std::function<bool(uint32_t)> q) { playingQuery_ = std::move(q); }
    /// For tests: whether a SoundEntries row loops (flag 0x200).
    void setLoopQuery(std::function<bool(uint32_t)> q) { loopQuery_ = std::move(q); }

private:
    struct Emitter {
        bool doodad = false;
        uint32_t displayId = 0;
        uint32_t loopHandle = 0;
        uint32_t loopSoundId = 0;
        glm::vec3 loopPosition{0.0f};
        /// The loop's position on the instance, for moving it along.
        glm::vec3 loopLocal{0.0f};
        bool loopLocalSet = false;
    };

    void playLoop(uint32_t instanceId, Emitter& e, uint32_t soundId, const glm::vec3& position,
                  float fadeInMs);
    void playOnce(uint32_t instanceId, uint32_t soundId, const glm::vec3& position);
    void stop(Emitter& e, float fadeOutMs);
    [[nodiscard]] bool playing(uint32_t handle) const;
    [[nodiscard]] bool sameLoopNearby(uint32_t soundId, const glm::vec3& position) const;

    const SoundData* soundData_ = nullptr;
    std::unordered_map<uint32_t, std::array<uint32_t, 10>> displaySounds_;
    std::unordered_map<uint32_t, Emitter> emitters_;
    std::function<uint32_t(const Request&)> recorder_;
    std::function<bool(uint32_t)> playingQuery_;
    std::function<bool(uint32_t)> loopQuery_;
};

/// The stock dedupe radius for a looping object sound: the same row already
/// playing within this distance (0x4cfe00, 0x9e8cf8 = 36, squared).
constexpr float kObjectLoopDedupeYards = 6.0f;
/// $DSL fades on a game object (0x70c050: props 1.0 s in and out) and on a
/// map doodad (0x7bd5a0: 3.0 s in, 0x9ebbc4).
constexpr float kObjectDslFadeMs = 1000.0f;
constexpr float kDoodadDslFadeInMs = 3000.0f;

} // namespace audio
} // namespace wowee
