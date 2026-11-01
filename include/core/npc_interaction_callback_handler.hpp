#pragma once

#include <cstdint>
#include <unordered_map>
#include <glm/glm.hpp>
#include "audio/npc_voice_manager.hpp"
#include "audio/combat_sound_manager.hpp"

namespace wowee {

namespace rendering { class Renderer; }
namespace game { class GameHandler; }
namespace audio { class AudioCoordinator; }
namespace core { class EntitySpawner; }

namespace core {

/// Handles NPC interaction callbacks: greeting, farewell, vendor, aggro voice lines.
class NPCInteractionCallbackHandler {
public:
    NPCInteractionCallbackHandler(EntitySpawner& entitySpawner,
                                  rendering::Renderer* renderer,
                                  game::GameHandler& gameHandler,
                                  audio::AudioCoordinator* audioCoordinator);

    void setupCallbacks();

private:
    /// Resolve NPC voice type from GUID (eliminates 4x copy-paste of display-ID lookup)
    [[nodiscard]] audio::VoiceType resolveNpcVoiceType(uint64_t guid) const;
    [[nodiscard]] uint32_t resolveNpcDisplayId(uint64_t guid) const;

    /// The unit a character render instance draws: the local character, an
    /// other player or a creature. 0 for anything else (mounts, previews).
    uint64_t guidForInstance(uint32_t instanceId);
    /// What the combat sounds need to know about a unit (its weapons, worn
    /// chest, display, render instance and position).
    bool resolveCombatUnit(uint64_t guid, audio::CombatUnitInfo& out);
    /// The ground under a point for a footstep: TerrainType.SoundID (stone on
    /// a building's floor) and whether the feet are in water.
    void footstepGround(const glm::vec3& renderPos, uint32_t& terrainSoundId, bool& inWater) const;
    /// The M2 sound events that are not $CSD: footfalls, swooshes, blows,
    /// fidgets, wing flaps, custom attacks and the death cry.
    void handleUnitAnimEvent(uint32_t instanceId, uint32_t eventTag, const glm::vec3& position);

    std::unordered_map<uint32_t, uint64_t> instanceGuids_;
    double instanceGuidsBuiltMs_ = -1.0e9;

    EntitySpawner& entitySpawner_;
    rendering::Renderer* renderer_;
    game::GameHandler& gameHandler_;
    audio::AudioCoordinator* audioCoordinator_;
};

} // namespace core
} // namespace wowee
