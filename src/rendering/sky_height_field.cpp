// The height field from above (sky_height_field.hpp): its images and set.
// What draws into it is Renderer::renderSkyField, with the shadow casters.
#include "rendering/sky_height_field.hpp"

#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_utils.hpp"
#include "core/logger.hpp"

#include <cmath>

namespace wowee {
namespace rendering {

bool SkyHeightField::ensure(VkContext* ctx, VkRenderPass shadowPass) {
    if (failed_ || !ctx) return false;
    if (sets_[0] != VK_NULL_HANDLE) return true;
    ctx_ = ctx;
    VkDevice device = ctx->getDevice();
    VmaAllocator alloc = ctx->getAllocator();
    const auto fail = [&]() {
        failed_ = true;
        shutdown();
        return false;
    };

    for (auto& side : targets_) for (Target& t : side) {
        VkImageCreateInfo ici{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = shadowDepthFormat();
        ici.extent = {sky_field::kTexels, sky_field::kTexels, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (vmaCreateImage(alloc, &ici, &aci, &t.image, &t.alloc, nullptr) != VK_SUCCESS) {
            t.image = VK_NULL_HANDLE;
            return fail();
        }
        t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageViewCreateInfo vci{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = t.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = ici.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &vci, nullptr, &t.view) != VK_SUCCESS) {
            t.view = VK_NULL_HANDLE;
            return fail();
        }
        // The render-pass path (no dynamic rendering) draws through a
        // framebuffer of the shadow pass, which these images match.
        if (shadowPass != VK_NULL_HANDLE) {
            VkFramebufferCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fci.renderPass = shadowPass;
            fci.attachmentCount = 1;
            fci.pAttachments = &t.view;
            fci.width = sky_field::kTexels;
            fci.height = sky_field::kTexels;
            fci.layers = 1;
            if (vkCreateFramebuffer(device, &fci, nullptr, &t.framebuffer) != VK_SUCCESS) {
                t.framebuffer = VK_NULL_HANDLE;
                return fail();
            }
        } else if (!ctx->useDynamicRendering()) {
            return fail();
        }
    }

    // Point sampled: the shader gathers four texels and keeps the highest.
    VkSamplerCreateInfo sci{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    sampler_ = ctx->getOrCreateSampler(sci);

    VkDescriptorSetLayoutBinding bindings[kMaps]{};
    for (uint32_t b = 0; b < kMaps; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = kMaps;
    lci.pBindings = bindings;
    VkDescriptorPoolSize ps{.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 2 * kMaps};
    VkDescriptorPoolCreateInfo pci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (sampler_ == VK_NULL_HANDLE || vkCreateDescriptorSetLayout(device, &lci, nullptr, &setLayout_) != VK_SUCCESS ||
        vkCreateDescriptorPool(device, &pci, nullptr, &pool_) != VK_SUCCESS) {
        return fail();
    }
    // One set a side, written once: the front is picked by which is bound.
    VkDescriptorSetLayout layouts[2] = {setLayout_, setLayout_};
    VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    VkDescriptorSet sets[2] = {};
    if (vkAllocateDescriptorSets(device, &ai, sets) != VK_SUCCESS) return fail();
    VkDescriptorImageInfo ii[2][kMaps]{};
    VkWriteDescriptorSet w[2 * kMaps]{};
    for (uint32_t side = 0; side < 2; ++side) {
        for (uint32_t b = 0; b < kMaps; ++b) {
            ii[side][b] = {sampler_, targets_[side][b].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet& wr = w[side * kMaps + b];
            wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr.dstSet = sets[side];
            wr.dstBinding = b;
            wr.descriptorCount = 1;
            wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr.pImageInfo = &ii[side][b];
        }
    }
    vkUpdateDescriptorSets(device, 2 * kMaps, w, 0, nullptr);
    sets_[0] = sets[0];
    sets_[1] = sets[1];
    LOG_INFO("Sky visibility: height field made (", sky_field::kTexels, "x", sky_field::kTexels, " over ",
             sky_field::kSpan, " yd, three maps, two sides)");
    return true;
}

void SkyHeightField::shutdown() {
    if (!ctx_) return;
    VkDevice device = ctx_->getDevice();
    VmaAllocator alloc = ctx_->getAllocator();
    destroy(device, pool_);
    sets_[0] = sets_[1] = VK_NULL_HANDLE;
    destroy(device, setLayout_);
    for (auto& side : targets_) for (Target& t : side) {
        if (t.framebuffer) vkDestroyFramebuffer(device, t.framebuffer, nullptr);
        if (t.view) vkDestroyImageView(device, t.view, nullptr);
        if (t.image) vmaDestroyImage(alloc, t.image, t.alloc);
        t = Target{};
    }
    sampler_ = VK_NULL_HANDLE;
    front_ = 0;
    frontDrawn_ = false;
    nextPart_ = 0;
    ctx_ = nullptr;
}

int SkyHeightField::partDue(const glm::vec3& camera, double now) {
    if (sets_[0] == VK_NULL_HANDLE) return 0;
    if (nextPart_ != 0) return nextPart_;
    const glm::vec2 d = glm::vec2(camera) - glm::vec2(frontCentre_);
    const bool due = !frontDrawn_ || glm::dot(d, d) > sky_field::kRecentre * sky_field::kRecentre ||
                     std::abs(camera.z - frontCentre_.z) > 0.5f * sky_field::kAbove ||
                     now - drawnAt_ > sky_field::kRefreshSeconds;
    if (!due) return 0;
    backCentre_ = sky_field::snappedCentre(camera);
    nextPart_ = 1;
    return nextPart_;
}

void SkyHeightField::partDrawn(double now) {
    if (nextPart_ == 1) {
        nextPart_ = 2;
        return;
    }
    nextPart_ = 0;
    front_ = 1 - front_;
    frontCentre_ = backCentre_;
    frontDrawn_ = true;
    drawnAt_ = now;
}

bool SkyHeightField::readable() const {
    if (sets_[0] == VK_NULL_HANDLE) return false;
    for (const auto& side : targets_)
        for (const Target& t : side)
            if (t.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return false;
    return true;
}

glm::vec4 SkyHeightField::params() const {
    return glm::vec4(frontCentre_.x - 0.5f * sky_field::kSpan, frontCentre_.y - 0.5f * sky_field::kSpan,
                     frontCentre_.z + sky_field::kAbove, frontDrawn_ ? 1.0f : 0.0f);
}

}  // namespace rendering
}  // namespace wowee
