#include "rendering/soft_particle_pass.hpp"

#include "rendering/vk_context.hpp"
#include "rendering/vk_utils.hpp"
#include "core/logger.hpp"

namespace wowee {
namespace rendering {

bool SoftParticlePass::initialize(VkContext* ctx) {
    if (isInitialized()) return true;
    if (!ctx) return false;
    ctx_ = ctx;
    VkDevice device = ctx->getDevice();

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 1;
    lci.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device, &lci, nullptr, &layout_) != VK_SUCCESS) {
        layout_ = VK_NULL_HANDLE;
        return false;
    }

    VkDescriptorPoolSize size{.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = kSlots};
    VkDescriptorPoolCreateInfo pci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = kSlots;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &size;
    if (vkCreateDescriptorPool(device, &pci, nullptr, &pool_) != VK_SUCCESS) {
        pool_ = VK_NULL_HANDLE;
        shutdown();
        return false;
    }
    VkDescriptorSetLayout layouts[kSlots] = {layout_, layout_};
    VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = kSlots;
    ai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(device, &ai, sets_) != VK_SUCCESS) {
        shutdown();
        return false;
    }

    // Depth is read texel for texel (texelFetch); the sampler only has to
    // exist, and must not filter or compare.
    VkSamplerCreateInfo sci{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ = ctx->getOrCreateSampler(sci);
    if (sampler_ == VK_NULL_HANDLE) {
        shutdown();
        return false;
    }
    LOG_INFO("Soft particles: late pass resources made");
    return true;
}

void SoftParticlePass::shutdown() {
    if (!ctx_) return;
    VkDevice device = ctx_->getDevice();
    destroy(device, pool_);  // frees the sets
    for (auto& s : sets_) s = VK_NULL_HANDLE;
    destroy(device, layout_);
    sampler_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

namespace {

void depthToRead(VkCommandBuffer cmd, VkImage image, bool toRead) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    if (toRead) {
        // The scene pass wrote it as an attachment, or resolved into it at
        // its end (a depth resolve counts as colour attachment output).
        b.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        b.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    } else {
        b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.srcAccessMask = 0;
        b.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT |
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_2_TRANSFER_READ_BIT |
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    cmdPipelineBarrier2(cmd, dep);
}

}  // namespace

bool SoftParticlePass::record(VkCommandBuffer cmd, const Target& t,
                              const std::function<void(VkCommandBuffer, VkDescriptorSet)>& draw) {
    if (!isInitialized() || !cmd || t.pass == VK_NULL_HANDLE || t.depth == VK_NULL_HANDLE ||
        t.extent.width == 0 || t.extent.height == 0 ||
        (t.framebuffer == VK_NULL_HANDLE && t.colorView == VK_NULL_HANDLE)) {
        return false;
    }
    VkDevice device = ctx_->getDevice();

    // The view and, for an off-screen scene, the framebuffer are made for this
    // frame and destroyed once its fence has passed: the images behind them
    // are rebuilt on every resize, sample count and upscaler change, and a
    // handle cached across those could name an image that has gone.
    VkImageViewCreateInfo vi{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.depth;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ctx_->getDepthFormat();
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkImageView depthView = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &vi, nullptr, &depthView) != VK_SUCCESS) return false;
    ctx_->deferAfterFrameFence([device, depthView]() { vkDestroyImageView(device, depthView, nullptr); });

    VkFramebuffer fb = t.framebuffer;
    if (fb == VK_NULL_HANDLE) {
        VkFramebufferCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = t.pass;
        fci.attachmentCount = 1;
        fci.pAttachments = &t.colorView;
        fci.width = t.extent.width;
        fci.height = t.extent.height;
        fci.layers = 1;
        if (vkCreateFramebuffer(device, &fci, nullptr, &fb) != VK_SUCCESS) return false;
        ctx_->deferAfterFrameFence([device, fb]() { vkDestroyFramebuffer(device, fb, nullptr); });
    }

    const VkDescriptorSet set = sets_[ctx_->getCurrentFrame() % kSlots];
    VkDescriptorImageInfo ii{.sampler = sampler_, .imageView = depthView,
                             .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);

    depthToRead(cmd, t.depth, true);

    VkRenderPassBeginInfo rp{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = t.pass;
    rp.framebuffer = fb;
    rp.renderArea.extent = t.extent;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{};
    vp.width = static_cast<float>(t.extent.width);
    vp.height = static_cast<float>(t.extent.height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{};
    sc.extent = t.extent;
    vkCmdSetScissor(cmd, 0, 1, &sc);

    draw(cmd, set);

    vkCmdEndRenderPass(cmd);
    depthToRead(cmd, t.depth, false);
    return true;
}

}  // namespace rendering
}  // namespace wowee
