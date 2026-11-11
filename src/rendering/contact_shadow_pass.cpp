#include "rendering/contact_shadow_pass.hpp"

#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "core/logger.hpp"
#include "core/env_flag.hpp"

namespace wowee {
namespace rendering {

namespace {
struct ContactPush {
    glm::mat4 invViewProj;
    glm::vec4 target;  // xy extent, z darkness
    glm::vec4 extra;   // the shader's own (ambient_occlusion.frag: the bounce grid's corner)
    glm::vec4 extra2;  // and one more (ambient_occlusion.frag: the sky height field's)
};
static_assert(sizeof(ContactPush) == 112, "the passes' push block is 112 bytes (80 read by most)");
}  // namespace

bool ContactShadowPass::ensure(VkContext* ctx, VkDescriptorSetLayout perFrameLayout, VkRenderPass pass) {
    if (!ctx || perFrameLayout == VK_NULL_HANDLE || pass == VK_NULL_HANDLE) return false;
    if (failed_) return false;
    if (pipeline_ != VK_NULL_HANDLE && builtFor_ == pass) return true;
    ctx_ = ctx;
    VkDevice device = ctx->getDevice();
    if (!depth_.initialize(ctx)) {
        failed_ = true;
        return false;
    }
    if (layout_ == VK_NULL_HANDLE) {
        VkDescriptorSetLayout sets[4] = {perFrameLayout, depth_.depthLayout(), extraLayout_, extraLayout2_};
        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pc.offset = 0;
        pc.size = sizeof(ContactPush);
        VkPipelineLayoutCreateInfo lci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.setLayoutCount = setCount();
        lci.pSetLayouts = sets;
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges = &pc;
        if (vkCreatePipelineLayout(device, &lci, nullptr, &layout_) != VK_SUCCESS) {
            layout_ = VK_NULL_HANDLE;
            failed_ = true;
            return false;
        }
    }
    if (pipeline_ != VK_NULL_HANDLE) {
        // The overlay pass was rebuilt (a resize or a swapchain change).
        VkPipeline old = pipeline_;
        ctx->deferAfterAllFrameFences([device, old]() { vkDestroyPipeline(device, old, nullptr); });
        pipeline_ = VK_NULL_HANDLE;
    }
    VkShaderModule vert, frag;
    if (!vert.loadFromFile(device, "assets/shaders/postprocess.vert.spv") ||
        !frag.loadFromFile(device, fragSpv_)) {
        LOG_WARNING(name_, ": ", fragSpv_, " missing - pass off");
        vert.destroy();
        frag.destroy();
        failed_ = true;
        return false;
    }
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    blend.blendEnable = VK_TRUE;
    switch (blendMode_) {
    case Blend::Multiply:
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        break;
    case Blend::Multiply2x:
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
        break;
    case Blend::OverTransmittance:
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        break;
    }
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    pipeline_ = PipelineBuilder()
        .setShaders(vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT), frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({}, {})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(blend)
        .setMultisample(VK_SAMPLE_COUNT_1_BIT)
        .setLayout(layout_)
        .setRenderPass(pass)
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, ctx->getPipelineCache());
    vert.destroy();
    frag.destroy();
    if (pipeline_ == VK_NULL_HANDLE) {
        LOG_WARNING(name_, ": the pipeline could not be made");
        failed_ = true;
        return false;
    }
    builtFor_ = pass;
    LOG_INFO(name_, ": pass made");
    return true;
}

void ContactShadowPass::shutdown() {
    if (!ctx_) return;
    VkDevice device = ctx_->getDevice();
    if (pipeline_) vkDestroyPipeline(device, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device, layout_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    builtFor_ = VK_NULL_HANDLE;
    depth_.shutdown();
    ctx_ = nullptr;
}

// WOWEE_CONTACT_SHADOW_DEBUG=1 tints red whatever the ray finds blocked,
// shaded by the map or not, for checking where it reaches.
static const bool kDebug = core::envFlagEnabled("WOWEE_CONTACT_SHADOW_DEBUG", false);

bool ContactShadowPass::record(VkCommandBuffer cmd, const SoftParticlePass::Target& target,
                               VkDescriptorSet perFrameSet, const glm::mat4& invViewProj, float darkness,
                               int flags, const glm::vec4& extra, VkDescriptorSet extraSet,
                               const glm::vec4& extra2, VkDescriptorSet extraSet2) {
    if (pipeline_ == VK_NULL_HANDLE || perFrameSet == VK_NULL_HANDLE || target.pass != builtFor_) return false;
    if (extraLayout_ != VK_NULL_HANDLE && extraSet == VK_NULL_HANDLE) return false;
    if (setCount() == 4 && extraSet2 == VK_NULL_HANDLE) return false;
    return depth_.record(cmd, target, [&](VkCommandBuffer c, VkDescriptorSet depthSet) {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        VkDescriptorSet sets[4] = {perFrameSet, depthSet, extraSet, extraSet2};
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, setCount(), sets, 0, nullptr);
        ContactPush push{};
        push.invViewProj = invViewProj;
        push.target = glm::vec4(static_cast<float>(target.extent.width), static_cast<float>(target.extent.height),
                                darkness, static_cast<float>(flags + (kDebug ? 4 : 0)));
        push.extra = extra;
        push.extra2 = extra2;
        vkCmdPushConstants(c, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(c, 3, 1, 0, 0);
    });
}

}  // namespace rendering
}  // namespace wowee
