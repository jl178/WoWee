#pragma once

/// The late particle pass behind the "Soft particles" enhancement
/// (docs/enhancements-plan.md E4).
///
/// A blended particle is a flat quad, and where it passes through the ground
/// or a wall the depth test cuts it along a hard line. To fade it out over the
/// last stretch in front of what is behind it, its shader has to read the
/// scene's depth - which it cannot do in the scene pass, where that depth is
/// the attachment being written. So with the row on, the scene pass draws the
/// particles that write depth (opaque, alpha key) as stock does, and the
/// blended ones and the glow halos wait for this pass: single sample, colour
/// only, recorded on the scene's resolved colour once the scene pass has
/// closed, with the scene's one-sample depth (its own without MSAA, its resolve
/// with it) moved to a read-only layout and bound as a texture. The shader
/// does the depth test itself against that texture (soft_particle.glsli).
///
/// Nothing here exists until the row is first turned on, and with it off the
/// renderer never calls in: the particles are drawn in the scene pass by the
/// stock pipelines exactly as before. A frame whose depth has no one-sample
/// copy (MSAA on a device without depth resolve) keeps them there too.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>

namespace wowee {
namespace rendering {

class VkContext;

class SoftParticlePass {
public:
    ~SoftParticlePass() { shutdown(); }

    /// The descriptor set layout, pool, sets and sampler. Idempotent; false
    /// when they could not be made (and nothing is left half made).
    bool initialize(VkContext* ctx);
    void shutdown();
    [[nodiscard]] bool isInitialized() const { return layout_ != VK_NULL_HANDLE; }

    /// Set 3 of the particle pipelines (set 2 of the halos'): the scene depth,
    /// one combined image sampler for the fragment stage.
    [[nodiscard]] VkDescriptorSetLayout depthLayout() const { return layout_; }

    /// Where the pass draws and what it reads.
    struct Target {
        VkRenderPass pass = VK_NULL_HANDLE;      ///< colour only, LOAD, PRESENT_SRC in and out
        VkFramebuffer framebuffer = VK_NULL_HANDLE;  ///< or null: one is made for colorView
        VkImageView colorView = VK_NULL_HANDLE;  ///< the resolved scene colour, when no framebuffer
        VkExtent2D extent{};                     ///< the scene's render extent
        VkImage depth = VK_NULL_HANDLE;          ///< one-sample, in DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    };

    /// Record the pass: the depth to a read-only layout, the pass begun with
    /// the viewport and scissor at `extent`, `draw` called with the frame's
    /// depth set, the pass ended and the depth put back. Outside any render
    /// pass. False (and nothing recorded) when the target is incomplete.
    bool record(VkCommandBuffer cmd, const Target& target,
                const std::function<void(VkCommandBuffer, VkDescriptorSet)>& draw);

private:
    VkContext* ctx_ = nullptr;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;  // owned by VkContext's sampler cache
    static constexpr uint32_t kSlots = 2;  // MAX_FRAMES_IN_FLIGHT
    VkDescriptorSet sets_[kSlots] = {};
};

}  // namespace rendering
}  // namespace wowee
