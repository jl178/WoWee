#pragma once

/// The sky over each point under Enhanced's daylight (docs/re/enhanced-lighting.md
/// section 6e): a height field drawn from straight above round the camera,
/// the way a shadow map is drawn from the sun - the same casters, pipelines
/// and depth format - and read by the bounce pass, which marches a few
/// directions out from each pixel for what the trees and buildings hide of
/// the sky.
///
/// Three maps: the tops of what stands on the ground (buildings and doodads)
/// seen from above, their undersides seen from below - so a crown is a slab
/// with the sky under its edge, not a column down to the ground - and the
/// terrain alone, which says where the ground under a point is: how high a
/// leaf is off it, and where the sunlit ground round it lies.
///
/// Two sets of them: the pass reads the front while the back is drawn, when
/// the camera has walked kRecentre yards from the front's middle or every
/// kRefreshSeconds (the world streams in, the sun moves), in two parts on
/// two frames - the ground and the tops, then the undersides - each on a
/// frame the sun's world map is not being redrawn (Renderer::renderSkyField
/// says why), and then the two swap.

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include "rendering/sky_field.hpp"

#include <cstdint>

namespace wowee {
namespace rendering {

class VkContext;

class SkyHeightField {
public:
    ~SkyHeightField() { shutdown(); }

    /// The depth images in the shadow maps' format, their framebuffers for
    /// `shadowPass` (the render-pass path), a point sampler and a set for
    /// each side. Idempotent; false when any could not be made.
    bool ensure(VkContext* ctx, VkRenderPass shadowPass);
    void shutdown();

    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    static constexpr uint32_t kMaps = 3;
    enum Map : uint32_t { kTops = 0, kGround = 1, kFeet = 2 };

    /// The part of a redraw due this frame: 0 none, 1 the ground and the
    /// tops, 2 the undersides. A redraw once begun runs to its end.
    [[nodiscard]] int partDue(const glm::vec3& camera, double now);
    /// Where the redraw under way stands (sky_field::snappedCentre).
    [[nodiscard]] glm::vec3 redrawCentre() const { return backCentre_; }
    /// The back's map to draw this part into.
    Target& back(Map m) { return targets_[1 - front_][m]; }
    /// A part drawn; after the last the back becomes the front.
    void partDrawn(double now);

    /// Every map of both sides readable (cleared or drawn at least once).
    [[nodiscard]] bool readable() const;
    /// Visits every map of both sides (the first frame's clears).
    template <typename F> void forEach(F&& f) {
        for (auto& side : targets_)
            for (Target& t : side) f(t);
    }

    [[nodiscard]] VkDescriptorSetLayout setLayout() const { return setLayout_; }
    /// The front's set.
    [[nodiscard]] VkDescriptorSet set() const { return sets_[front_]; }
    /// For the shader: the front's corner in world x, y, the height depth 0
    /// stands for, and 1 once it holds a drawing (0 before: no sky term).
    [[nodiscard]] glm::vec4 params() const;

private:
    VkContext* ctx_ = nullptr;
    bool failed_ = false;
    Target targets_[2][kMaps];
    VkSampler sampler_ = VK_NULL_HANDLE;  // VkContext's sampler cache owns it
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[2] = {};
    uint32_t front_ = 0;
    bool frontDrawn_ = false;
    glm::vec3 frontCentre_{0.0f};
    double drawnAt_ = 0.0;
    int nextPart_ = 0;  // 0 idle, else the part a redraw under way does next
    glm::vec3 backCentre_{0.0f};
};

}  // namespace rendering
}  // namespace wowee
