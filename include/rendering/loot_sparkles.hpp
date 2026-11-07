#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class M2Renderer;

/// The glitter over a corpse that has loot for the player, as the original
/// client draws it: Particles\LootFX.m2 set on the body for as long as the
/// server marks it lootable (UNIT_DYNFLAG_LOOTABLE, which it only sends to a
/// player allowed to loot it) and taken off once it is emptied or gone.
class LootSparkles {
public:
    /// A corpse with loot, or a game object a quest needs (GAMEOBJECT_DYNAMIC
    /// 0x8), where it lies in render coordinates, and the sparkle's scale.
    struct Corpse {
        Corpse(uint64_t g, const glm::vec3& p, float s = 1.0f) : first(g), second(p), scale(s) {}
        uint64_t first = 0;
        glm::vec3 second{0.0f};
        float scale = 1.0f;
    };

    /// Once a frame with every lootable corpse there is now. Starts the
    /// sparkle on new ones, keeps each on its body, and stops it on the rest.
    void update(M2Renderer* m2, pipeline::AssetManager* assets,
                const std::vector<Corpse>& corpses);

private:
    /// Clear of the ranges the level-up effect (999900) and spell visuals
    /// (below 999800) take.
    static constexpr uint32_t MODEL_ID = 999901;

    bool ensureModel(M2Renderer* m2, pipeline::AssetManager* assets);

    M2Renderer* m2_ = nullptr;
    bool failed_ = false;
    std::unordered_map<uint64_t, uint32_t> instances_;  // corpse guid -> sparkle instance
};

} // namespace rendering
} // namespace wowee
