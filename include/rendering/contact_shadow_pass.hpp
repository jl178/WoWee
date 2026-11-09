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

/// The same frame serves the "Ambient occlusion" row (ambient_occlusion.frag):
/// another fragment shader with the same inputs and the same dst * src blend,
/// named at construction.
class ContactShadowPass {
public:
    /// How the shader's output meets the scene: Multiply is dst * src (only
    /// darkens), Multiply2x is 2 * dst * src (the shader writes half the
    /// factor, so it can brighten up to twice), OverTransmittance is
    /// src.rgb + dst * src.a (light added in front of what it dims).
    enum class Blend { Multiply, Multiply2x, OverTransmittance };
    /// `extraLayout`, when given, is set 2 of the pipeline: one more set the
    /// shader reads (the bounce light's grid), bound at record.
    explicit ContactShadowPass(const char* fragSpv = "assets/shaders/contact_shadow.frag.spv",
                               const char* name = "Contact shadows", Blend blend = Blend::Multiply,
                               VkDescriptorSetLayout extraLayout = VK_NULL_HANDLE)
        : fragSpv_(fragSpv), name_(name), blendMode_(blend), extraLayout_(extraLayout) {}
    [[nodiscard]] const char* fragSpv() const { return fragSpv_; }
    ~ContactShadowPass() { shutdown(); }

    /// The depth set, the layout and the pipeline for `pass` (the overlay
    /// pass). Idempotent; false when any could not be made.
    bool ensure(VkContext* ctx, VkDescriptorSetLayout perFrameLayout, VkRenderPass pass);
    void shutdown();

    /// Record the pass on `target`: false (nothing recorded) when it is
    /// incomplete or nothing is made. `darkness` is how much of the sun a
    /// full shadow takes away (Light.dbc's shadow band x the player's setting,
    /// times how far outdoors the player is). `flags` goes to the shader's
    /// target.w with 4 added under WOWEE_CONTACT_SHADOW_DEBUG.
    /// `extra` goes to the shader's second push vec4; `extraSet` is bound as
    /// set 2 when the pass was made with an extra layout (and must be then).
    bool record(VkCommandBuffer cmd, const SoftParticlePass::Target& target, VkDescriptorSet perFrameSet,
                const glm::mat4& invViewProj, float darkness, int flags = 0,
                const glm::vec4& extra = glm::vec4(0.0f), VkDescriptorSet extraSet = VK_NULL_HANDLE);

private:
    const char* fragSpv_;
    const char* name_;
    Blend blendMode_;
    VkDescriptorSetLayout extraLayout_ = VK_NULL_HANDLE;
    VkContext* ctx_ = nullptr;
    SoftParticlePass depth_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkRenderPass builtFor_ = VK_NULL_HANDLE;
    bool failed_ = false;
};

}  // namespace rendering
}  // namespace wowee
