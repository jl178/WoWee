// Which images createImage keeps out of the driver's compression
// (rendering/image_create_policy.hpp): small single-level R8 textures that are
// only sampled - the terrain's alpha and shadow maps - and nothing else: not
// another format, a mipped texture, a render target, a storage image or a
// multisampled image, and nothing at all unless the policy is on.

#include <catch_amalgamated.hpp>
#include "rendering/image_create_policy.hpp"

using namespace wowee::rendering;

namespace {
constexpr VkImageUsageFlags kTexture = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr VkFormat kR8 = VK_FORMAT_R8_UNORM;
}

TEST_CASE("the terrain's maps skip compression when the policy is on", "[image_policy]") {
    CHECK(sampledImageCreateFlags(true, kR8, kTexture, VK_SAMPLE_COUNT_1_BIT, 64, 64, 1) == VK_IMAGE_CREATE_ALIAS_BIT);
    CHECK(sampledImageCreateFlags(true, kR8, kTexture, VK_SAMPLE_COUNT_1_BIT, 1, 1, 1) == VK_IMAGE_CREATE_ALIAS_BIT);
    CHECK(sampledImageCreateFlags(true, kR8, kTexture | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  VK_SAMPLE_COUNT_1_BIT, 512, 512, 1) == VK_IMAGE_CREATE_ALIAS_BIT);
}

TEST_CASE("nothing changes with the policy off", "[image_policy]") {
    CHECK(sampledImageCreateFlags(false, kR8, kTexture, VK_SAMPLE_COUNT_1_BIT, 64, 64, 1) == 0);
}

TEST_CASE("other formats keep their compression", "[image_policy]") {
    // Uncompressing the small RGBA8 textures changed drawn pixels by a level.
    CHECK(sampledImageCreateFlags(true, VK_FORMAT_R8G8B8A8_UNORM, kTexture, VK_SAMPLE_COUNT_1_BIT, 1, 1, 1) == 0);
    CHECK(sampledImageCreateFlags(true, VK_FORMAT_R8G8B8A8_UNORM, kTexture, VK_SAMPLE_COUNT_1_BIT, 256, 256, 1) == 0);
    CHECK(sampledImageCreateFlags(true, VK_FORMAT_R16G16B16A16_SFLOAT, kTexture, VK_SAMPLE_COUNT_1_BIT, 1, 1, 1) == 0);
    CHECK(sampledImageCreateFlags(true, VK_FORMAT_R8G8_UNORM, kTexture, VK_SAMPLE_COUNT_1_BIT, 64, 64, 1) == 0);
}

TEST_CASE("textures with a mip chain keep their compression", "[image_policy]") {
    CHECK(sampledImageCreateFlags(true, kR8, kTexture | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  VK_SAMPLE_COUNT_1_BIT, 64, 64, 7) == 0);
}

TEST_CASE("large textures keep their compression", "[image_policy]") {
    CHECK(sampledImageCreateFlags(true, kR8, kTexture, VK_SAMPLE_COUNT_1_BIT, 513, 64, 1) == 0);
    CHECK(sampledImageCreateFlags(true, kR8, kTexture, VK_SAMPLE_COUNT_1_BIT, 64, 1024, 1) == 0);
}

TEST_CASE("render targets, storage and multisampled images are untouched", "[image_policy]") {
    const VkImageUsageFlags rendered[] = {
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
        VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT};
    for (VkImageUsageFlags extra : rendered) {
        CHECK(sampledImageCreateFlags(true, kR8, kTexture | extra, VK_SAMPLE_COUNT_1_BIT, 64, 64, 1) == 0);
    }
    CHECK(sampledImageCreateFlags(true, kR8, kTexture, VK_SAMPLE_COUNT_4_BIT, 64, 64, 1) == 0);
    // Not sampled at all: a transfer-only image is none of this policy's business.
    CHECK(sampledImageCreateFlags(true, kR8, VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT, 64, 64, 1) == 0);
}
