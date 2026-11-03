#pragma once

// Create flags for sampled textures, by format, usage and size.
//
// On Intel's driver over the i915 kernel interface, every colour image that
// gets lossless (CCS) compression also gets a private buffer for its clear
// colour, and every vkQueueSubmit walks the list of all such images to hand
// their buffers to the kernel (Mesa anv i915 setup_execbuf_for_cmd_buffers,
// image_private_objects). A city keeps ~100k of the terrain's 64x64 alpha and
// shadow maps, and that walk was 55-70% of the main thread's time, all of it
// inside the frame's submit and the upload batches' submits.
//
// VK_IMAGE_CREATE_ALIAS_BIT keeps the driver from compressing the image, so it
// never joins the list. Only single-level, single-sample R8 images up to
// 512x512 that are sampled and never rendered into or stored to - which is
// what the terrain's maps are. Other formats keep their compression: with
// every small RGBA8 texture uncompressed as well (the 1x1 white and flat
// normal textures, the minimap tiles), a Trade District shot moved 30% of
// its pixels by one level, which the R8 maps alone do not. Larger textures,
// mipped textures and every render target keep it too; block-compressed (BC)
// textures never had any.

#include <vulkan/vulkan.h>

#include <cstdint>

namespace wowee::rendering {

inline VkImageCreateFlags sampledImageCreateFlags(bool avoidPrivateBindings, VkFormat format,
                                                  VkImageUsageFlags usage,
                                                  VkSampleCountFlagBits samples,
                                                  uint32_t width, uint32_t height,
                                                  uint32_t mipLevels) {
    if (!avoidPrivateBindings || format != VK_FORMAT_R8_UNORM) return 0;
    if (samples != VK_SAMPLE_COUNT_1_BIT || mipLevels != 1) return 0;
    constexpr VkImageUsageFlags kRendered =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    if (!(usage & VK_IMAGE_USAGE_SAMPLED_BIT) || (usage & kRendered)) return 0;
    if (width > 512 || height > 512) return 0;
    return VK_IMAGE_CREATE_ALIAS_BIT;
}

}  // namespace wowee::rendering
