#pragma once

/// The pass behind the "Contact shadows" enhancement (E14,
/// contact_shadows.hpp, contact_shadow.frag). A full-screen triangle over the
/// scene's resolved colour, once the scene pass has closed and before the
/// late particles, reading the scene's one-sample depth through its own
/// SoftParticlePass (the same "depth as a texture, colour-only pass" frame the
/// late particles use) and the per-frame block and sun maps through set 0.
/// Blended dst * src: it only ever darkens, and only where a short ray to the
/// sun is blocked. Nothing exists until the row is first on; the renderer
/// lets it go a few frames after the row goes off.

#include "rendering/soft_particle_pass.hpp"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace wowee {
namespace rendering {

class VkContext;

class ContactShadowPass {
public:
    ~ContactShadowPass() { shutdown(); }

    /// The depth set, the layout and the pipeline for `pass` (the overlay
    /// pass). Idempotent; false when any could not be made.
    bool ensure(VkContext* ctx, VkDescriptorSetLayout perFrameLayout, VkRenderPass pass);
    void shutdown();

    /// Record the pass on `target`: false (nothing recorded) when it is
    /// incomplete or nothing is made. `darkness` is how much of the sun a
    /// full shadow takes away (Light.dbc's shadow band x the player's setting,
    /// times how far outdoors the player is).
    bool record(VkCommandBuffer cmd, const SoftParticlePass::Target& target, VkDescriptorSet perFrameSet,
                const glm::mat4& invViewProj, float darkness);

private:
    VkContext* ctx_ = nullptr;
    SoftParticlePass depth_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkRenderPass builtFor_ = VK_NULL_HANDLE;
    bool failed_ = false;
};

}  // namespace rendering
}  // namespace wowee
