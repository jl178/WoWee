#pragma once

#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class M2Renderer;

/// The ! and ? over a quest giver's head, as the stock client draws them.
///
/// They are not sprites. 3.3.5a ships them as small 3D models in
/// Interface\Buttons - TalkToMe.m2 is the gold !, TalkToMeQuestionMark.m2 the
/// gold ?, and Grey / Blue / LTBlue variants of each - whose one bone is a
/// Z-locked cylindrical billboard (bone flags 0x240) with a 1.5 s bob in its
/// Stand sequence. This used to stretch the 16-pixel gossip-list icons from
/// Interface\GossipFrame over a camera-facing quad, which is where the blurry,
/// flat look came from.
///
/// Each marker is an instance of the stock model in the shared M2 renderer, so
/// it gets the model's own unlit gradient material, its own animation, fog and
/// depth like anything else in the world. The M2 renderer does not evaluate
/// billboard bones, so the cylindrical billboard is applied here: the instance
/// is turned about Z every frame to face the camera.
class QuestMarkerRenderer {
public:
    /// Which of the stock models to show.
    enum class Kind : uint8_t {
        Available,            ///< gold !   TalkToMe
        AvailableLow,         ///< grey !   TalkToMeGrey (out-levelled)
        AvailableRepeatable,  ///< blue !   TalkToMeBlue (daily / repeatable)
        Reward,               ///< gold ?   TalkToMeQuestionMark
        RewardRepeatable,     ///< blue ?   TalkToMeQuestion_LTBlue
        Incomplete,           ///< grey ?   TalkToMeQuestion_Grey
        Count
    };

    struct Marker {
        uint64_t guid = 0;
        glm::vec3 position{0.0f};  ///< unit origin (its feet), render coordinates
        float headTop = 2.0f;      ///< height of the top of the unit above position.z
        Kind kind = Kind::Available;
    };

    /// Once a frame with every marker there should be now. Places new ones,
    /// keeps the rest on their units facing the camera, and removes the others.
    void update(M2Renderer* m2, pipeline::AssetManager* assets,
                const glm::vec3& cameraPos, const std::vector<Marker>& markers);

    /// Take every marker down (map change, logout).
    void clear();

private:
    /// Clear of the level-up effect (999900), the loot sparkle (999901) and
    /// the spell visual range (below 999800).
    static constexpr uint32_t MODEL_ID_BASE = 999910;
    static constexpr size_t KIND_COUNT = static_cast<size_t>(Kind::Count);

    bool ensureModel(M2Renderer* m2, pipeline::AssetManager* assets, Kind kind);

    struct Placed {
        uint32_t instanceId = 0;
        Kind kind = Kind::Available;
        glm::vec2 facing{1.0f, 0.0f};  ///< last horizontal direction to the camera
    };

    M2Renderer* m2_ = nullptr;
    std::array<bool, KIND_COUNT> failed_{};
    std::unordered_map<uint64_t, Placed> instances_;  // unit guid -> marker instance
};

} // namespace rendering
} // namespace wowee
