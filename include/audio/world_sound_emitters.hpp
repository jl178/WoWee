#pragma once

#include "audio/stock_sound_advanced.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>
#include <glm/vec3.hpp>

namespace wowee {
namespace audio {

class SoundData;

/// Sounds that belong to a place, played by distance from the listener.
///
/// Two sources feed it, both the stock client's:
///  - SoundEmitters.dbc, the loops placed by hand on a map (waterfalls, river
///    bends, forges). Set per map with setMap().
///  - M2 doodad sound events: a model's $DSL event is a SoundEntries loop that
///    plays where the event sits (a torch's crackle, a fountain), its $DSO
///    event a one-shot at a time in the model's animation (a banner's flap).
///    The terrain adds these per placed doodad and removes them per tile.
///
/// A loop starts once the listener is inside its SoundEntries distance cutoff
/// and stops when they leave it again; the nearest kMaxLoops play when more
/// are in range. Positions are render coordinates, as the listener's are.
///
/// A sound whose kit has a SoundEntriesAdvanced row plays as that row's
/// instance does in the stock client (stock::advancedStep): usage 0 loops
/// with 3 s fades and moves to its next file every TimeIntervalMin..Max,
/// usage 1 is a one-shot replayed after every interval - the Dalaran pet
/// shop's frogs, the ice cracking in Icecrown - rather than a loop of it.
class WorldSoundEmitters {
public:
    WorldSoundEmitters() = default;
    ~WorldSoundEmitters();
    WorldSoundEmitters(const WorldSoundEmitters&) = delete;
    WorldSoundEmitters& operator=(const WorldSoundEmitters&) = delete;

    void setSoundData(const SoundData* data) { soundData_ = data; }

    /// Replace the placed emitters with SoundEmitters.dbc's rows for a map.
    /// Doodad emitters are left alone: the terrain removes them with its tiles.
    void setMap(uint32_t mapId);

    /// A looping SoundEntries sound at a point. Returns its id (never 0), or
    /// 0 when the sound id has no files.
    uint32_t addLoop(const glm::vec3& renderPos, uint32_t soundEntryId);
    /// A one-shot played every periodMs, phaseMs into the period - an M2
    /// event's time in a looping sequence of that length.
    uint32_t addPeriodic(const glm::vec3& renderPos, uint32_t soundEntryId,
                         uint32_t periodMs, uint32_t phaseMs);
    void remove(uint32_t id);
    /// Stop and forget everything, placed and doodad alike.
    void clear();

    /// volumeScale is the ambience volume setting, 0..1.
    void update(float deltaTime, const glm::vec3& listenerPos, float volumeScale);

    [[nodiscard]] size_t emitterCount() const { return emitters_.size(); }

private:
    static constexpr int kMaxLoops = 24;

    struct Emitter {
        glm::vec3 position{0.0f};
        uint32_t soundId = 0;
        float volume = 1.0f;
        float cutoff = 30.0f;
        float minDistance = -1.0f; // full volume inside this (SoundEntries MinDistance)
        bool placed = false;      // SoundEmitters.dbc row, cleared by setMap
        bool periodic = false;
        float periodS = 0.0f;
        float timerS = 0.0f;      // periodic: time left to the next play
        uint32_t handle = 0;      // playing loop, 0 none
        float retryS = 0.0f;      // a loop that failed to start waits this long
        bool resolved = false;    // volume and cutoff read from SoundEntries
        bool dead = false;        // the sound id has no files: never plays
        // A kit with a usage 0 or 1 SoundEntriesAdvanced row: its instance,
        // alive while the emitter is in range.
        const stock::AdvancedKit* advanced = nullptr;
        bool instanceAlive = false;
        stock::AdvancedSchedule schedule;
        int32_t offsetMs = 0;
        uint32_t order = 0;       // position in emitters_' iteration order (refreshOrder)
    };

    /// The per-frame loops walk these instead of the whole map: the periodic
    /// emitters, and the advanced instances alive (at most kMaxLoops), each
    /// in emitters_' own iteration order so the shared random stream is drawn
    /// in the same order the full walk drew it. Rebuilt when the map changes.
    void refreshOrder();
    void dropAlive(const Emitter* e);

    bool resolve(Emitter& e) const;
    uint32_t add(Emitter e);
    void stop(Emitter& e);
    bool start(Emitter& e, float volumeScale);
    /// One frame of an advanced instance (0x4ca7b0).
    void stepAdvanced(Emitter& e, float deltaTime, float volumeScale);
    /// Begin a fresh instance as the emitter comes into range (0x4cb580).
    void beginInstance(Emitter& e);

    const SoundData* soundData_ = nullptr;
    std::unordered_map<uint32_t, Emitter> emitters_;
    uint32_t nextId_ = 1;
    uint32_t mapId_ = 0xFFFFFFFFu;
    float lastVolumeScale_ = -1.0f;
    float scanTimerS_ = 0.0f;
    std::vector<std::pair<float, uint32_t>> candidates_;  // scratch: (distSq, id)
    std::vector<Emitter*> periodic_;
    std::vector<Emitter*> alive_;
    bool orderDirty_ = true;
};

} // namespace audio
} // namespace wowee
