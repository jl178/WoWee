#include "rendering/character_preview.hpp"
#include "rendering/imgui_texture.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/glue_scene_particles.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/vk_render_target.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/camera.hpp"
#include "rendering/renderer.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/char_sections.hpp"
#include "pipeline/dbc_layout.hpp"
#include "core/appearance_composer.hpp"
#include "core/geoset_rules.hpp"
#include "core/helm_visual.hpp"
#include "pipeline/item_textures.hpp"
#include "pipeline/m2_asset_loader.hpp"
#include "core/logger.hpp"
#include <cstdlib>
#include "core/application.hpp"
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <cctype>

namespace wowee {
namespace rendering {

namespace {

/// WoW's own key bone numbering: 4 is the lower spine, 6 the head, 7 the jaw.
constexpr int32_t kKeyBoneHead = 6;

bool isFiniteVec3(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

void frameCameraForModelBounds(Camera& camera, const glm::vec3& boundMin, const glm::vec3& boundMax) {
    if (!isFiniteVec3(boundMin) || !isFiniteVec3(boundMax)) {
        return;
    }

    const glm::vec3 extent = boundMax - boundMin;
    if (extent.z <= 0.0f) {
        return;
    }

    const float fovY = glm::radians(camera.getFovDegrees());
    const float tanHalfY = std::tan(fovY * 0.5f);
    if (!std::isfinite(tanHalfY) || tanHalfY <= 0.0f) {
        return;
    }

    const float tanHalfX = tanHalfY * std::max(camera.getAspectRatio(), 0.1f);
    const float height = std::max(extent.z, 0.1f);
    const float width = std::max(std::max(extent.x, extent.y), 0.1f);
    const float margin = 1.50f;
    const float distanceForHeight = (height * margin) / (2.0f * tanHalfY);
    const float distanceForWidth = (width * margin) / (2.0f * tanHalfX);
    const float distance = std::max({4.5f, distanceForHeight, distanceForWidth});
    const float centerZ = (boundMin.z + boundMax.z) * 0.5f;

    camera.setPosition(glm::vec3(0.0f, distance, centerZ));
    camera.setRotation(270.0f, 0.0f);
}

} // namespace

CharacterPreview::CharacterPreview() = default;

CharacterPreview::~CharacterPreview() {
    shutdown();
}

void CharacterPreview::ensureAppearanceGeosetsLoaded() {
    if (appearanceGeosetsLoaded_ || !assetManager_) {
        return;
    }

    appearanceGeosetsLoaded_ = true;
    hairGeosetMap_.clear();
    facialHairGeosetMap_.clear();

    // CharHairGeosets.dbc maps (race, sex, hairStyleId) to the group-0
    // scalp/hair submesh. Activating every group-0 submesh draws all hair
    // variants at once, which shows up as flickering magenta patches.
    if (auto chg = assetManager_->loadDBC("CharHairGeosets.dbc"); chg && chg->isLoaded()) {
        const auto* chgL = pipeline::getActiveDBCLayout()
            ? pipeline::getActiveDBCLayout()->getLayout("CharHairGeosets") : nullptr;
        for (uint32_t i = 0; i < chg->getRecordCount(); i++) {
            uint32_t raceId = chg->getUInt32(i, chgL ? (*chgL)["RaceID"] : 1);
            uint32_t sexId = chg->getUInt32(i, chgL ? (*chgL)["SexID"] : 2);
            uint32_t variation = chg->getUInt32(i, chgL ? (*chgL)["Variation"] : 3);
            uint32_t geosetId = chg->getUInt32(i, chgL ? (*chgL)["GeosetID"] : 4);
            const bool useDefaultScalp = chg->getFieldCount() > 5 && chg->getUInt32(i, 5) != 0;
            const uint32_t key = core::appearanceKey(static_cast<uint8_t>(raceId),
                                                    static_cast<uint8_t>(sexId),
                                                    static_cast<uint8_t>(variation));
            hairGeosetMap_[key] = static_cast<uint16_t>(useDefaultScalp ? 1 : geosetId);
        }
        LOG_INFO("CharacterPreview: loaded ", hairGeosetMap_.size(), " hair geoset mappings");
    }

    if (auto cfh = assetManager_->loadDBC("CharacterFacialHairStyles.dbc"); cfh && cfh->isLoaded()) {
        const auto* cfhL = pipeline::getActiveDBCLayout()
            ? pipeline::getActiveDBCLayout()->getLayout("CharacterFacialHairStyles") : nullptr;
        const auto fhF = pipeline::detectFacialHairFields(cfh.get(), cfhL);
        for (uint32_t i = 0; i < cfh->getRecordCount(); i++) {
            uint32_t raceId = cfh->getUInt32(i, cfhL ? (*cfhL)["RaceID"] : 0);
            uint32_t sexId = cfh->getUInt32(i, cfhL ? (*cfhL)["SexID"] : 1);
            uint32_t variation = cfh->getUInt32(i, cfhL ? (*cfhL)["Variation"] : 2);
            const uint32_t key = core::appearanceKey(static_cast<uint8_t>(raceId),
                                                    static_cast<uint8_t>(sexId),
                                                    static_cast<uint8_t>(variation));

            FacialHairGeosets geosets;
            // Whichever columns this copy of the DBC keeps them in - see
            // detectFacialHairFields, and the same read in EntitySpawner.
            geosets.geoset100 = static_cast<uint16_t>(cfh->getUInt32(i, fhF.geoset100));
            geosets.geoset300 = static_cast<uint16_t>(cfh->getUInt32(i, fhF.geoset300));
            geosets.geoset200 = static_cast<uint16_t>(cfh->getUInt32(i, fhF.geoset200));
            if (fhF.geoset1700 != pipeline::FacialHairFields::kNoFacialField &&
                fhF.geoset1700 < cfh->getFieldCount()) {
                geosets.geoset1700 = pipeline::facialGeosetVariant(cfh->getUInt32(i, fhF.geoset1700));
            }
            facialHairGeosetMap_[key] = geosets;
        }
        LOG_INFO("CharacterPreview: loaded ", facialHairGeosetMap_.size(), " facial hair geoset mappings");
    }
}

uint16_t CharacterPreview::selectedHairScalpGeoset() const {
    const uint8_t raceId = static_cast<uint8_t>(race_);
    const uint8_t sexId = (gender_ == game::Gender::FEMALE ||
                           (gender_ == game::Gender::NONBINARY && useFemaleModel_)) ? 1u : 0u;
    const uint32_t key = core::appearanceKey(raceId, sexId, static_cast<uint8_t>(hairStyle_));

    auto it = hairGeosetMap_.find(key);
    if (it != hairGeosetMap_.end() && it->second > 0) {
        return it->second;
    }

    // Last-resort heuristic for incomplete data sets. The DBC path above is
    // expected for real clients and is much more reliable than this fallback.
    return static_cast<uint16_t>(std::max<uint8_t>(hairStyle_ + 1, 1));
}

std::unordered_set<uint16_t> CharacterPreview::buildBaseGeosets() {
    ensureAppearanceGeosetsLoaded();

    const uint8_t raceId = static_cast<uint8_t>(race_);
    const uint8_t sexId = (gender_ == game::Gender::FEMALE ||
                           (gender_ == game::Gender::NONBINARY && useFemaleModel_)) ? 1u : 0u;
    const uint16_t selectedHairScalp = selectedHairScalpGeoset();

    uint16_t facial100 = 1, facial200 = 1, facial300 = 1, facial1700 = 0;
    auto itFacial = facialHairGeosetMap_.find(
        core::appearanceKey(raceId, sexId, static_cast<uint8_t>(facialHair_)));
    if (itFacial != facialHairGeosetMap_.end()) {
        facial100 = itFacial->second.geoset100;
        facial200 = itFacial->second.geoset200;
        facial300 = itFacial->second.geoset300;
        facial1700 = itFacial->second.geoset1700;
    }

    // The same bare set the player gets. This used to be its own list and had
    // drifted: it named one of the two feet variants, so an HD model spelling
    // its feet the other way stood in the portrait without them, and it named
    // the no-cloak panel, which the HD models do not carry.
    std::unordered_set<uint16_t> activeGeosets =
        core::bareCharacterGeosets(selectedHairScalp, facial100, facial200, facial300, raceId,
                                   facial1700);

    return activeGeosets;
}

bool CharacterPreview::initialize(pipeline::AssetManager* am, int width, int height) {
    assetManager_ = am;
    // If already initialized with valid resources, reuse them.
    // This avoids destroying GPU resources that may still be referenced by
    // an in-flight command buffer (compositePass recorded earlier this frame).
    if (renderTarget_ && renderTarget_->isValid() && charRenderer_ && camera_) {
        // Mark model as not loaded - loadCharacter() will handle instance cleanup
        modelLoaded_ = false;
        return true;
    }

    // Only where the view is actually being built. The render target, the
    // camera's aspect ratio and the composite are all sized from these, so
    // taking a new size while keeping a target built at the old one would put
    // every one of the three out of step with the image they are drawing into.
    if (width > 0) fboWidth_ = width;
    if (height > 0) fboHeight_ = height;

    auto* appRenderer = core::Application::getInstance().getRenderer();
    vkCtx_ = appRenderer ? appRenderer->getVkContext() : nullptr;
    VkDescriptorSetLayout perFrameLayout = appRenderer ? appRenderer->getPerFrameSetLayout() : VK_NULL_HANDLE;

    if (!vkCtx_ || perFrameLayout == VK_NULL_HANDLE) {
        LOG_ERROR("CharacterPreview: no VkContext or perFrameLayout available");
        return false;
    }

    // Create off-screen render target first (need its render pass for pipeline creation)
    createFBO();
    if (!renderTarget_ || !renderTarget_->isValid()) {
        LOG_ERROR("CharacterPreview: failed to create off-screen render target");
        return false;
    }

    // Initialize CharacterRenderer with our off-screen render pass
    charRenderer_ = std::make_unique<CharacterRenderer>();
    if (!charRenderer_->initialize(vkCtx_, perFrameLayout, am, renderTarget_->getRenderPass(),
                                   renderTarget_->getSampleCount())) {
        LOG_ERROR("CharacterPreview: failed to initialize CharacterRenderer");
        return false;
    }

    // Configure lighting for character preview
    // Use distant fog to avoid clipping, enable shadows for visual depth
    charRenderer_->setFog(glm::vec3(0.05f, 0.05f, 0.1f), 9999.0f, 10000.0f);

    camera_ = std::make_unique<Camera>();
    // Portrait-style camera: WoW Z-up coordinate system
    // Model at origin, camera positioned along +Y looking toward -Y
    camera_->setFov(30.0f);
    camera_->setAspectRatio(static_cast<float>(fboWidth_) / static_cast<float>(fboHeight_));
    // Pull camera back far enough to see full body + head with margin
    camera_->setPosition(glm::vec3(0.0f, 4.5f, 0.9f));
    camera_->setRotation(270.0f, 0.0f);

    LOG_INFO("CharacterPreview initialized (", fboWidth_, "x", fboHeight_, ")");
    return true;
}

void CharacterPreview::shutdown() {
    // Unregister from renderer before destroying resources
    auto* appRenderer = core::Application::getInstance().getRenderer();
    if (appRenderer) appRenderer->unregisterPreview(this);

    // Before the CharacterRenderer: its texture cache holds the particle textures.
    if (sceneParticles_) {
        sceneParticles_->shutdown();
        sceneParticles_.reset();
    }
    if (charRenderer_) {
        charRenderer_->shutdown();
        charRenderer_.reset();
    }
    camera_.reset();
    destroyFBO();
    modelLoaded_ = false;
    compositeRendered_ = false;
    instanceId_ = 0;
}

void CharacterPreview::createFBO() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator allocator = vkCtx_->getAllocator();

    // 1. Create off-screen render target with depth
    // WOWEE_PREVIEW_MSAA=1|2|4|8 overrides the preview's own sample count. A
    // diagnostic for the NVIDIA device loss at character select (#154), which
    // needs scene MSAA on and a draw into this 4x pass: it separates "the
    // preview's multisampling" from "the scene's".
    static const VkSampleCountFlagBits previewSamples = [] {
        const char* v = std::getenv("WOWEE_PREVIEW_MSAA");
        if (!v || !*v) return VK_SAMPLE_COUNT_4_BIT;
        switch (std::atoi(v)) {
            case 1: return VK_SAMPLE_COUNT_1_BIT;
            case 2: return VK_SAMPLE_COUNT_2_BIT;
            case 8: return VK_SAMPLE_COUNT_8_BIT;
            default: return VK_SAMPLE_COUNT_4_BIT;
        }
    }();
    if (previewSamples != VK_SAMPLE_COUNT_4_BIT) {
        LOG_WARNING("CharacterPreview: WOWEE_PREVIEW_MSAA - ", static_cast<int>(previewSamples),
                    "x instead of 4x");
    }
    renderTarget_ = std::make_unique<VkRenderTarget>();
    if (!renderTarget_->create(*vkCtx_, fboWidth_, fboHeight_, VK_FORMAT_R8G8B8A8_UNORM, true,
                               previewSamples)) {
        LOG_ERROR("CharacterPreview: failed to create render target");
        renderTarget_.reset();
        return;
    }

    // 1b. Transition the color image from UNDEFINED to SHADER_READ_ONLY_OPTIMAL
    // so that ImGui::Image doesn't sample an image in UNDEFINED layout before
    // the first compositePass runs.
    {
        VkCommandBuffer cmd = vkCtx_->beginSingleTimeCommands();
        VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = renderTarget_->getColorImage();
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkDependencyInfo barrierDep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        barrierDep.dependencyFlags = 0;
        barrierDep.imageMemoryBarrierCount = 1;
        barrierDep.pImageMemoryBarriers = &barrier;
        cmdPipelineBarrier2(cmd, barrierDep);
        vkCtx_->endSingleTimeCommands(cmd);
    }

    // 2. Create 1x1 dummy depth texture (shadow map placeholder, depth=1.0 = no shadow).
    //    Must be a depth format for sampler2DShadow compatibility.
    {
        VkImageCreateInfo imgCI{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imgCI.imageType = VK_IMAGE_TYPE_2D;
        imgCI.format = VK_FORMAT_D16_UNORM;
        imgCI.extent = {.width = 1, .height = 1, .depth = 1};
        imgCI.mipLevels = 1;
        imgCI.arrayLayers = 1;
        imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
        imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgCI.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocCI{};
        allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (vmaCreateImage(vkCtx_->getAllocator(), &imgCI, &allocCI,
                &dummyShadowImage_, &dummyShadowAlloc_, nullptr) != VK_SUCCESS) {
            LOG_ERROR("CharacterPreview: failed to create dummy shadow image");
            return;
        }
        VkImageViewCreateInfo viewCI{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewCI.image = dummyShadowImage_;
        viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewCI.format = VK_FORMAT_D16_UNORM;
        viewCI.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        if (vkCreateImageView(device, &viewCI, nullptr, &dummyShadowView_) != VK_SUCCESS) {
            LOG_ERROR("CharacterPreview: failed to create dummy shadow image view");
            return;
        }
        // Clear to depth 1.0 and transition to shader-read layout
        vkCtx_->immediateSubmit([&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier2 toTransfer{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            toTransfer.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            toTransfer.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
            toTransfer.image = dummyShadowImage_;
            toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
            toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            VkDependencyInfo toTransferDep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            toTransferDep.dependencyFlags = 0;
            toTransferDep.imageMemoryBarrierCount = 1;
            toTransferDep.pImageMemoryBarriers = &toTransfer;
            cmdPipelineBarrier2(cmd, toTransferDep);
            VkClearDepthStencilValue clearVal{.depth = 1.0f, .stencil = 0};
            VkImageSubresourceRange range{.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
            vkCmdClearDepthStencilImage(cmd, dummyShadowImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearVal, 1, &range);
            VkImageMemoryBarrier2 toRead{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            toRead.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
            toRead.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            toRead.image = dummyShadowImage_;
            toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toRead.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
            toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            VkDependencyInfo toReadDep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            toReadDep.dependencyFlags = 0;
            toReadDep.imageMemoryBarrierCount = 1;
            toReadDep.pImageMemoryBarriers = &toRead;
            cmdPipelineBarrier2(cmd, toReadDep);
        });
    }

    // 3. Create descriptor pool for per-frame sets (2 UBO + 2 shadow + 2 fog volume)
    {
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[0].descriptorCount = MAX_FRAMES;
        sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[1].descriptorCount = MAX_FRAMES * 4;

        VkDescriptorPoolCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = MAX_FRAMES;
        ci.poolSizeCount = 2;
        ci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device, &ci, nullptr, &previewDescPool_) != VK_SUCCESS) {
            LOG_ERROR("CharacterPreview: failed to create descriptor pool");
            return;
        }
    }

    // 4. Create per-frame UBOs and descriptor sets
    auto* appRenderer = core::Application::getInstance().getRenderer();
    VkDescriptorSetLayout perFrameLayout = appRenderer->getPerFrameSetLayout();

    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        // Create mapped UBO
        VkBufferCreateInfo bufInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufInfo.size = sizeof(GPUPerFrameData);
        bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo mapInfo{};
        if (vmaCreateBuffer(allocator, &bufInfo, &allocInfo,
                &previewUBO_[i], &previewUBOAlloc_[i], &mapInfo) != VK_SUCCESS) {
            LOG_ERROR("CharacterPreview: failed to create UBO ", i);
            return;
        }
        previewUBOMapped_[i] = mapInfo.pMappedData;

        // Allocate descriptor set
        VkDescriptorSetAllocateInfo setAlloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setAlloc.descriptorPool = previewDescPool_;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &perFrameLayout;
        if (vkAllocateDescriptorSets(device, &setAlloc, &previewPerFrameSet_[i]) != VK_SUCCESS) {
            LOG_ERROR("CharacterPreview: failed to allocate descriptor set ", i);
            return;
        }

        // Write UBO binding (0) and shadow sampler binding (1) using dummy white texture
        VkDescriptorBufferInfo descBuf{};
        descBuf.buffer = previewUBO_[i];
        descBuf.offset = 0;
        descBuf.range = sizeof(GPUPerFrameData);

        VkDescriptorImageInfo shadowImg{};
        // sampler is ignored: this set comes from the renderer's per-frame
        // layout, where binding 1 declares an immutable comparison sampler.
        shadowImg.imageView = dummyShadowView_;
        shadowImg.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Binding 2 is the fog volume, which a portrait has no air for: the
        // renderer's neutral one, with the block's switch left at zero. Its
        // sampler is immutable in the layout, like binding 1's.
        VkDescriptorImageInfo fogImg{};
        fogImg.imageView = appRenderer->getNeutralFogVolumeView();
        fogImg.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        // Bindings 3 and 4 are the world's ray traced lighting; the neutral
        // image, with the block's switch left at zero.
        VkDescriptorImageInfo rtImg{};
        rtImg.imageView = appRenderer->getNeutralRtLightingView();
        rtImg.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[5]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = previewPerFrameSet_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &descBuf;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = previewPerFrameSet_[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &shadowImg;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = previewPerFrameSet_[i];
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &fogImg;
        for (uint32_t b = 3; b <= 4; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = previewPerFrameSet_[i];
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[b].pImageInfo = &rtImg;
        }

        vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);
    }

    // 5. Register the color attachment as an ImGui texture
    imguiTextureId_ = ImGui_ImplVulkan_AddTexture(
        renderTarget_->getSampler(),
        renderTarget_->getColorImageView(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    LOG_INFO("CharacterPreview: off-screen FBO created (", fboWidth_, "x", fboHeight_, ")");
}

void CharacterPreview::destroyFBO() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator allocator = vkCtx_->getAllocator();

    if (imguiTextureId_) {
        removeImGuiTexture(imguiTextureId_);
        imguiTextureId_ = VK_NULL_HANDLE;
    }

    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        destroy(allocator, previewUBO_[i], previewUBOAlloc_[i]);
    }

    destroy(device, previewDescPool_);

    if (dummyShadowView_) { vkDestroyImageView(device, dummyShadowView_, nullptr); dummyShadowView_ = VK_NULL_HANDLE; }
    if (dummyShadowImage_) { vmaDestroyImage(allocator, dummyShadowImage_, dummyShadowAlloc_); dummyShadowImage_ = VK_NULL_HANDLE; dummyShadowAlloc_ = VK_NULL_HANDLE; }

    if (renderTarget_) {
        renderTarget_->destroy(device, allocator);
        renderTarget_.reset();
    }
}

bool CharacterPreview::loadCharacter(game::Race race, game::Gender gender,
                                      uint8_t skin, uint8_t face,
                                      uint8_t hairStyle, uint8_t hairColor,
                                      uint8_t facialHair, bool useFemaleModel) {
    if (!charRenderer_ || !assetManager_ || !assetManager_->isInitialized()) {
        return false;
    }

    // Remove existing instance.
    // Must wait for GPU to finish - compositePass() may have recorded draw commands
    // referencing this instance's bone buffers earlier in the current frame.
    if (instanceId_ > 0) {
        if (vkCtx_) vkDeviceWaitIdle(vkCtx_->getDevice());
        charRenderer_->removeInstance(instanceId_);
        instanceId_ = 0;
        modelLoaded_ = false;
    }

    std::string m2Path = game::getPlayerModelPath(race, gender, useFemaleModel);

    auto m2Data = assetManager_->readFile(m2Path);
    if (m2Data.empty()) {
        LOG_WARNING("CharacterPreview: failed to read M2: ", m2Path);
        return false;
    }

    auto model = pipeline::M2Loader::load(m2Data);
    if (model.name.empty()) model.name = m2Path;

    // M2 version 264+ (WotLK) stores submesh/bone data in external .skin files.
    // Earlier versions (Classic ≤256, TBC ≤263) have skin data embedded in the M2.
    std::string skinPath = pipeline::skinPathForM2(m2Path);
    auto skinData = assetManager_->readFile(skinPath);
    if (!skinData.empty() && model.version >= 264) {
        pipeline::M2Loader::loadSkin(skinData, model);
    }

    if (!model.isValid()) {
        LOG_WARNING("CharacterPreview: invalid model: ", m2Path);
        return false;
    }

    if (camera_) {
        glm::vec3 frameMin = model.boundMin;
        glm::vec3 frameMax = model.boundMax;
        if (!model.vertices.empty()) {
            glm::vec3 tightMin(std::numeric_limits<float>::max());
            glm::vec3 tightMax(-std::numeric_limits<float>::max());
            for (const auto& v : model.vertices) {
                if (!isFiniteVec3(v.position)) continue;
                tightMin = glm::min(tightMin, v.position);
                tightMax = glm::max(tightMax, v.position);
            }
            if (tightMin.x <= tightMax.x && tightMin.y <= tightMax.y && tightMin.z <= tightMax.z) {
                frameMin = tightMin;
                frameMax = tightMax;
            }
        }
        frameCameraForModelBounds(*camera_, frameMin, frameMax);
        modelBoundMinZ_ = frameMin.z;
        modelBoundMaxZ_ = frameMax.z;
        modelHeadZ_ = 0.0f;
        if (charRenderer_ && instanceId_ > 0) {
            float headZ = 0.0f;
            if (charRenderer_->getInstanceKeyBonePivotZ(instanceId_, kKeyBoneHead, headZ)) {
                modelHeadZ_ = headZ;
            }
        }
        fullBodyDistance_ = camera_->getPosition().y;
        // A player model is framed by its bounds; no creature portrait camera
        // is in play, and a stale one from the shared preview must not be.
        portraitCameraValid_ = false;
    }

    // Look up CharSections.dbc for all appearance textures
    uint32_t targetRaceId = static_cast<uint32_t>(race);
    uint32_t targetSexId = (gender == game::Gender::FEMALE ||
                            (gender == game::Gender::NONBINARY && useFemaleModel)) ? 1u : 0u;

    std::string faceLowerPath;
    std::string faceUpperPath;
    std::string facialLowerPath;
    std::string facialUpperPath;
    std::string hairScalpPath;
    std::vector<std::string> underwearPaths;
    bodySkinPath_.clear();
    baseLayers_.clear();

    auto charSectionsDbc = assetManager_->loadDBC("CharSections.dbc");
    if (charSectionsDbc) {
        const auto* csL = pipeline::getActiveDBCLayout()
            ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
        const auto csF = pipeline::detectCharSectionsFields(charSectionsDbc.get(), csL);

        // The one reader, in pipeline/char_sections.hpp. This copy had no
        // fallback for a face the table does not carry - a character created
        // with a face number that has no row simply had no face - and it
        // matched the skin and underwear rows on variation 0, which the other
        // readers do not, so a table that numbers those rows any other way
        // found nothing here and everything there.
        pipeline::CharacterAppearance who;
        who.raceId = targetRaceId;
        who.sexId = targetSexId;
        who.skinId = static_cast<uint8_t>(skin);
        who.faceId = static_cast<uint8_t>(face);
        who.hairStyleId = static_cast<uint8_t>(hairStyle);
        who.hairColorId = static_cast<uint8_t>(hairColor);
        who.facialHairId = facialHair;

        const auto sections = pipeline::resolveCharacterSections(
            charSectionsDbc.get(), csF, who,
            [](const std::string& path, void* ctx) {
                return static_cast<pipeline::AssetManager*>(ctx)->fileExists(path);
            },
            assetManager_);

        bodySkinPath_   = sections.bodySkin;
        skinExtraPath_  = sections.skinExtra;
        faceLowerPath   = sections.faceLower;
        faceUpperPath   = sections.faceUpper;
        facialLowerPath = sections.facialLower;
        facialUpperPath = sections.facialUpper;
        hairScalpPath   = sections.hair;
        underwearPaths  = sections.underwear;

        LOG_INFO("CharSections lookup: skin=",
                 bodySkinPath_.empty() ? "(not found)" : bodySkinPath_,
                 " face=", sections.haveFace
                     ? (sections.exactFace ? faceLowerPath : faceLowerPath + " (nearest)")
                     : "(not found)",
                 " hair=", sections.haveHair ? hairScalpPath : "(not found)",
                 " underwear=", underwearPaths.size(), " textures");
    } else {
        LOG_WARNING("CharSections.dbc not loaded - no character textures");
    }

    // Assign texture filenames on model before GPU upload
    // pipeline/char_sections.hpp fills the runtime slots. This copy still
    // guarded types 1 and 6 with "only if the slot is empty", which is the trap
    // the skin-extra slot was already fixed for: a name in a runtime slot is not
    // a filename, and 'Ohren' is what a model in the wild puts there.
    {
        pipeline::CharacterSectionTextures resolved;
        resolved.bodySkin  = bodySkinPath_;
        resolved.skinExtra = skinExtraPath_;
        resolved.hair      = hairScalpPath;
        resolved.underwear = underwearPaths;
        // The race folder is the second component of the model path:
        // Character\Human\Female\HumanFemale.m2 -> Human. Taken from the path
        // rather than restated, so the two cannot disagree.
        std::string raceFolder;
        {
            const size_t first = m2Path.find('\\');
            const size_t second = (first == std::string::npos)
                ? std::string::npos : m2Path.find('\\', first + 1);
            if (first != std::string::npos && second != std::string::npos) {
                raceFolder = m2Path.substr(first + 1, second - first - 1);
            }
        }
        pipeline::applyCharacterTextures(model, resolved, raceFolder);
    }

    // Load external .anim files for sequences that store keyframes outside the M2.
    // Flag 0x20 = embedded data; when clear, animation lives in {ModelName}{SeqID}-{Var}.anim
    pipeline::loadExternalAnimations(*assetManager_, m2Path, m2Data, model);

    if (!charRenderer_->loadModel(model, PREVIEW_MODEL_ID)) {
        LOG_WARNING("CharacterPreview: failed to load model to GPU");
        return false;
    }
    // Composite body skin + face + underwear overlays
    if (!bodySkinPath_.empty()) {
        std::vector<std::string> layers;
        layers.push_back(bodySkinPath_);
        // Face lower texture composited onto body at the face region
        if (!faceLowerPath.empty()) {
            layers.push_back(faceLowerPath);
        }
        if (!faceUpperPath.empty()) {
            layers.push_back(faceUpperPath);
        }
        // Facial hair over the face (CharSections section 2).
        if (!facialLowerPath.empty()) layers.push_back(facialLowerPath);
        if (!facialUpperPath.empty()) layers.push_back(facialUpperPath);
        for (const auto& up : underwearPaths) {
            layers.push_back(up);
        }

        // Cache for later equipment compositing.
        // Keep baseLayers_ without the base skin (compositeWithRegions takes basePath separately).
        if (!faceLowerPath.empty()) baseLayers_.push_back(faceLowerPath);
        if (!faceUpperPath.empty()) baseLayers_.push_back(faceUpperPath);
        if (!facialLowerPath.empty()) baseLayers_.push_back(facialLowerPath);
        if (!facialUpperPath.empty()) baseLayers_.push_back(facialUpperPath);
        for (const auto& up : underwearPaths) baseLayers_.push_back(up);

        if (layers.size() > 1) {
            VkTexture* compositeTex = charRenderer_->compositeTextures(layers);
            if (compositeTex != nullptr) {
                for (size_t ti = 0; ti < model.textures.size(); ti++) {
                    if (model.textures[ti].type == 1) {
                        charRenderer_->setModelTexture(PREVIEW_MODEL_ID, static_cast<uint32_t>(ti), compositeTex);
                        break;
                    }
                }
            }
        } else {
            // Single layer (body skin only, no face/underwear overlays) - load directly
            VkTexture* skinTex = charRenderer_->loadTexture(bodySkinPath_);
            if (skinTex != nullptr) {
                for (size_t ti = 0; ti < model.textures.size(); ti++) {
                    if (model.textures[ti].type == 1) {
                        charRenderer_->setModelTexture(PREVIEW_MODEL_ID, static_cast<uint32_t>(ti), skinTex);
                        break;
                    }
                }
            }
        }
    }

    // If hair scalp texture was found, ensure it's loaded for type-6 slot
    if (!hairScalpPath.empty()) {
        VkTexture* hairTex = charRenderer_->loadTexture(hairScalpPath);
        if (hairTex != nullptr) {
            for (size_t ti = 0; ti < model.textures.size(); ti++) {
                if (model.textures[ti].type == 6) {
                    charRenderer_->setModelTexture(PREVIEW_MODEL_ID, static_cast<uint32_t>(ti), hairTex);
                    break;
                }
            }
        }
    }

    // Create instance at origin with current yaw (degrees here; the renderer
    // takes radians)
    instanceId_ = charRenderer_->createInstance(PREVIEW_MODEL_ID,
        glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, glm::radians(modelYaw_)),
        1.0f);

    if (instanceId_ == 0) {
        LOG_WARNING("CharacterPreview: failed to create instance");
        return false;
    }

    // Cache core appearance before geoset selection; the DBC resolver uses it.
    race_ = race;
    gender_ = gender;
    useFemaleModel_ = useFemaleModel;
    hairStyle_ = hairStyle;
    facialHair_ = facialHair;

    std::unordered_set<uint16_t> activeGeosets = buildBaseGeosets();
    charRenderer_->setActiveGeosets(instanceId_, activeGeosets);

    // Play idle animation (Stand = animation ID 0)
    charRenderer_->playAnimation(instanceId_, rendering::anim::STAND, true);

    // Cache the type-1 texture slot index so applyEquipment can update it.
    skinTextureSlotIndex_ = 0;
    for (size_t ti = 0; ti < model.textures.size(); ti++) {
        if (model.textures[ti].type == 1) {
            skinTextureSlotIndex_ = static_cast<uint32_t>(ti);
            break;
        }
    }

    modelLoaded_ = true;
    loadRacialBackdrop(race);
    LOG_INFO("CharacterPreview: loaded ", m2Path,
             " skin=", static_cast<int>(skin), " face=", static_cast<int>(face),
             " hair=", static_cast<int>(hairStyle), " hairColor=", static_cast<int>(hairColor),
             " facial=", static_cast<int>(facialHair));
    return true;
}

bool CharacterPreview::applyEquipment(const std::vector<game::EquipmentItem>& equipment) {
    if (!modelLoaded_ || instanceId_ == 0 || !charRenderer_ || !assetManager_ || !assetManager_->isInitialized()) {
        return false;
    }

    // Weapons first, and unconditionally: they depend on nothing below, while the
    // geoset/skin work that follows bails out early on characters whose body skin
    // could not be composited. Attaching last meant those characters showed no
    // weapon at all - and kept the previously selected character's weapon and
    // enchant, since detaching happens here too.
    attachWeapons(equipment);

    charRenderer_->clearTextureSlotOverride(instanceId_, static_cast<uint16_t>(skinTextureSlotIndex_));
    charRenderer_->setGroupTextureOverride(instanceId_, 15, nullptr);

    auto displayInfoDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc || !displayInfoDbc->isLoaded()) {
        LOG_WARNING("applyEquipment: ItemDisplayInfo.dbc not loaded");
        return false;
    }

    auto hasInvType = [&](std::initializer_list<uint8_t> types) -> bool {
        for (const auto& it : equipment) {
            if (it.displayModel == 0) continue;
            for (uint8_t t : types) {
                if (it.inventoryType == t) return true;
            }
        }
        return false;
    };

    auto findDisplayId = [&](std::initializer_list<uint8_t> types) -> uint32_t {
        for (const auto& it : equipment) {
            if (it.displayModel == 0) continue;
            for (uint8_t t : types) {
                if (it.inventoryType == t) return it.displayModel;
            }
        }
        return 0;
    };

    const auto* idiL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    const uint32_t geosetGroup1Field = idiL ? (*idiL)["GeosetGroup1"] : 7u;
    const uint32_t geosetGroup3Field = idiL ? (*idiL)["GeosetGroup3"] : 9u;

    auto getGeosetGroup = [&](uint32_t displayInfoId, uint32_t fieldIdx) -> uint32_t {
        if (displayInfoId == 0) return 0;
        int32_t recIdx = displayInfoDbc->findRecordById(displayInfoId);
        if (recIdx < 0) return 0;
        return displayInfoDbc->getUInt32(static_cast<uint32_t>(recIdx), fieldIdx);
    };

    // --- Geosets ---
    // M2 geoset IDs encode body part group × 100 + variant (e.g., 801 = group 8
    // (sleeves) variant 1, 1301 = group 13 (pants) variant 1). ItemDisplayInfo.dbc
    // provides the variant offset per equipped item; base IDs are per-group constants.
    std::unordered_set<uint16_t> geosets = buildBaseGeosets();

    auto eraseGroup = [&](uint16_t group) {
        for (auto it = geosets.begin(); it != geosets.end();) {
            if ((*it / 100) == group) {
                it = geosets.erase(it);
            } else {
                ++it;
            }
        }
    };

    // CharGeosets: group 4=gloves(forearm), 5=boots(shin), 8=sleeves, 13=pants
    std::unordered_set<uint16_t> modelGeosets;
    if (const auto* modelData = charRenderer_->getModelData(PREVIEW_MODEL_ID)) {
        for (const auto& batch : modelData->batches) {
            modelGeosets.insert(batch.submeshId);
        }
    }

    // core/geoset_rules.hpp, the same rule the world paths use.
    //
    // This copy took a second id as its fallback rather than looking inside the
    // group, so where the two ids given were the same - which every call below
    // does - it could only answer "the model has it" or "nothing", and a model
    // spelling that part with a different variant lost it entirely.
    auto pickGeoset = [&](uint16_t preferred, uint16_t fallback) -> uint16_t {
        const uint16_t chosen = core::resolveGeoset(preferred, modelGeosets);
        if (chosen != 0) return chosen;
        return (fallback != 0 && modelGeosets.count(fallback) > 0) ? fallback : 0;
    };

    auto lowestInGroup = [&](uint16_t group) -> uint16_t {
        return core::resolveGeoset(static_cast<uint16_t>(group * 100 + 2), modelGeosets);
    };

    uint16_t geosetGloves = pickGeoset(core::kGeosetBareForearms, core::kGeosetBareForearms);
    uint16_t geosetBoots = pickGeoset(core::kGeosetBareShins, lowestInGroup(5));
    uint16_t geosetSleeves = pickGeoset(core::kGeosetBareSleeves, core::kGeosetBareSleeves);
    uint16_t geosetPants = pickGeoset(core::kGeosetBarePants, core::kGeosetBarePants);

    bool robeSkirt = false;
    // Chest/Shirt/Robe → group 8 (sleeves)
    {
        // Chest or robe over the shirt beneath it.
        uint32_t did = findDisplayId({5, 20});
        if (did == 0) did = findDisplayId({4});
        uint32_t gg = getGeosetGroup(did, geosetGroup1Field);
        if (gg > 0) geosetSleeves = pickGeoset(core::equippedGeoset(core::equipment::kChestBare, gg), core::kGeosetBareSleeves);
        // Robe kilt legs
        uint32_t gg3 = getGeosetGroup(did, geosetGroup3Field);
        if (gg3 > 0) { geosetPants = pickGeoset(core::equippedGeoset(core::equipment::kRobeKiltBare, gg3), core::kGeosetBarePants); robeSkirt = true; }
    }
    // Legs: GeosetGroup[2] is a dress or kilt (group 13). [0] is group 11
    // pant flaps, never the skirt. A robe's skirt wins.
    {
        uint32_t did = findDisplayId({7});
        uint32_t gg3 = getGeosetGroup(did, geosetGroup3Field);
        if (gg3 > 0 && !robeSkirt) { geosetPants = pickGeoset(core::equippedGeoset(core::equipment::kRobeKiltBare, gg3), core::kGeosetBarePants); robeSkirt = true; }
    }
    // Boots → group 5 (shins); a skirt hides the boot tops
    {
        uint32_t did = findDisplayId({8});
        uint32_t gg = getGeosetGroup(did, geosetGroup1Field);
        if (gg > 0 && !robeSkirt) geosetBoots = pickGeoset(core::equippedGeoset(core::equipment::kBootsBare, gg), lowestInGroup(5));
    }
    // Gloves → group 4 (forearms)
    {
        uint32_t did = findDisplayId({10});
        uint32_t gg = getGeosetGroup(did, geosetGroup1Field);
        if (gg > 0) geosetGloves = pickGeoset(core::equippedGeoset(core::equipment::kGlovesBare, gg), core::kGeosetBareForearms);
    }
    // Wrists/Bracers → group 8 (sleeves, only if chest/shirt didn't set it)
    {
        uint32_t did = findDisplayId({9});
        if (did != 0 && geosetSleeves == pickGeoset(core::kGeosetBareSleeves, core::kGeosetBareSleeves)) {
            uint32_t gg = getGeosetGroup(did, geosetGroup1Field);
            if (gg > 0) geosetSleeves = pickGeoset(core::equippedGeoset(core::equipment::kChestBare, gg), core::kGeosetBareSleeves);
        }
    }
    // Belt → group 18 (buckle), falling back to the base variant so the waist
    // is not left empty - see the spawner's copy for what that costs.
    uint16_t geosetBelt = 0;
    {
        uint32_t did = findDisplayId({6});
        uint32_t gg = getGeosetGroup(did, geosetGroup1Field);
        geosetBelt = pickGeoset(
            gg > 0 ? core::equippedGeoset(core::equipment::kBeltBase, gg) : 0,
            core::equipment::kBeltBase);
    }

    eraseGroup(4);
    eraseGroup(5);
    eraseGroup(8);
    eraseGroup(13);
    eraseGroup(15);
    eraseGroup(18);
    if (geosetGloves != 0) geosets.insert(geosetGloves);
    if (geosetBoots != 0) geosets.insert(geosetBoots);
    if (geosetSleeves != 0) geosets.insert(geosetSleeves);
    if (geosetPants != 0) geosets.insert(geosetPants);
    if (geosetBelt != 0) geosets.insert(geosetBelt);
    uint32_t capeGG = getGeosetGroup(findDisplayId({16}), geosetGroup1Field);
    uint16_t geosetCape = pickGeoset(
        hasInvType({16}) ? core::cloakGeoset(capeGG) : core::kGeosetNoCape,
        core::kGeosetNoCape);
    if (geosetCape != 0) geosets.insert(geosetCape); // Cloak mesh toggle (visual may still be limited)
    if (hasInvType({19})) {
        uint16_t geosetTabard = pickGeoset(core::kGeosetDefaultTabard, 0);
        if (geosetTabard != 0) geosets.insert(geosetTabard);
    }

    // Keep hair visible in the preview. The in-world renderer can hide hair
    // because it attaches helmet models, but this preview path does not yet
    // render head-slot attachments; hiding hair here leaves bald characters.

    charRenderer_->setActiveGeosets(instanceId_, geosets);

    // --- Textures (equipment overlays onto body skin) ---
    if (bodySkinPath_.empty()) return true; // geosets applied, but can't composite


    // Texture component region fields - use DBC layout when available, fall back to binary offsets.
    uint32_t texRegionFields[8];
    pipeline::getItemDisplayInfoTextureFields(*displayInfoDbc, idiL, texRegionFields);

    std::vector<std::pair<int, std::string>> regionLayers;
    regionLayers.reserve(32);
    std::vector<std::pair<int, std::string>> robeLegs;  // drawn last

    // Paint in the stock layering order, not the order the caller listed the
    // items in. CharStartOutfit lists a caster's robe before the shirt, so the
    // shirt's Chest_TU/TL went on top of the robe and the mage, priest and
    // warlock wore a brown tunic over it. Shirt first, then trousers, boots,
    // chest/robe, bracers, gloves, belt, tabard; a robe's legs after all of it.
    auto layerRank = [](uint8_t invType) -> int {
        switch (invType) {
            case 4:  return 0;   // shirt
            case 7:  return 1;   // legs
            case 8:  return 2;   // feet
            case 5:
            case 20: return 3;   // chest / robe
            case 9:  return 4;   // wrists
            case 10: return 5;   // hands
            case 6:  return 6;   // waist
            case 19: return 7;   // tabard
            default: return 8;
        }
    };
    std::vector<const game::EquipmentItem*> layered;
    layered.reserve(equipment.size());
    for (const auto& it : equipment) layered.push_back(&it);
    std::stable_sort(layered.begin(), layered.end(),
        [&](const game::EquipmentItem* a, const game::EquipmentItem* b) {
            return layerRank(a->inventoryType) < layerRank(b->inventoryType);
        });

    for (const game::EquipmentItem* itp : layered) {
        const auto& it = *itp;
        if (it.displayModel == 0) continue;
        int32_t recIdx = displayInfoDbc->findRecordById(it.displayModel);
        if (recIdx < 0) continue;
        const bool robeRec = pipeline::isRobeDisplay(*displayInfoDbc, idiL, recIdx);

        for (int region = 0; region < 8; region++) {
            std::string texName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), texRegionFields[region]);
            if (texName.empty()) continue;

            const std::string fullPath = pipeline::resolveItemRegionTexture(
                *assetManager_, region, texName, gender_ == game::Gender::FEMALE);
            if (fullPath.empty()) continue;
            if (robeRec && pipeline::isRobeLegRegion(region)) robeLegs.emplace_back(region, fullPath);
            else regionLayers.emplace_back(region, fullPath);
        }
    }
    regionLayers.insert(regionLayers.end(), robeLegs.begin(), robeLegs.end());

    if (!regionLayers.empty()) {
        VkTexture* newTex = charRenderer_->compositeWithRegions(bodySkinPath_, baseLayers_, regionLayers);
        if (newTex != nullptr) {
            charRenderer_->setTextureSlotOverride(instanceId_, static_cast<uint16_t>(skinTextureSlotIndex_), newTex);
        }
    }

    // Cloak texture (group 15) is separate from body compositing.
    if (hasInvType({16})) {
        uint32_t capeDisplayId = findDisplayId({16});
        if (capeDisplayId != 0) {
            int32_t capeRecIdx = displayInfoDbc->findRecordById(capeDisplayId);
            if (capeRecIdx >= 0) {
                std::vector<std::string> capeNames;
                auto addName = [&](const std::string& n) {
                    if (!n.empty() && std::find(capeNames.begin(), capeNames.end(), n) == capeNames.end()) {
                        capeNames.push_back(n);
                    }
                };
                std::string leftName = displayInfoDbc->getString(static_cast<uint32_t>(capeRecIdx), 3);
                std::string rightName = displayInfoDbc->getString(static_cast<uint32_t>(capeRecIdx), 4);
                if (gender_ == game::Gender::FEMALE) {
                    addName(rightName);
                    addName(leftName);
                } else {
                    addName(leftName);
                    addName(rightName);
                }

                // pipeline/item_textures.hpp knows where a cape's art is and
                // in what order to try for it.
                std::vector<std::string> candidates;
                for (const auto& nameRaw : capeNames) {
                    for (auto& c : pipeline::capeTextureCandidates(
                             nameRaw, gender_ == game::Gender::FEMALE)) {
                        if (std::find(candidates.begin(), candidates.end(), c) == candidates.end()) {
                            candidates.push_back(std::move(c));
                        }
                    }
                }
                VkTexture* whiteTex = charRenderer_->loadTexture("");
                for (const auto& c : candidates) {
                    VkTexture* capeTex = charRenderer_->loadTexture(c);
                    if (capeTex != nullptr && capeTex != whiteTex) {
                        charRenderer_->setGroupTextureOverride(instanceId_, 15, capeTex);
                        if (const auto* md = charRenderer_->getModelData(PREVIEW_MODEL_ID)) {
                            for (size_t ti = 0; ti < md->textures.size(); ti++) {
                                if (md->textures[ti].type == 2) {
                                    charRenderer_->setTextureSlotOverride(instanceId_, static_cast<uint16_t>(ti), capeTex);
                                }
                            }
                        }
                        break;
                    }
                }
            }
        }
    } else {
        if (const auto* md = charRenderer_->getModelData(PREVIEW_MODEL_ID)) {
            for (size_t ti = 0; ti < md->textures.size(); ti++) {
                if (md->textures[ti].type == 2) {
                    charRenderer_->clearTextureSlotOverride(instanceId_, static_cast<uint16_t>(ti));
                }
            }
        }
    }

    return true;
}

/// Any model, by path, with none of the appearance work.
///
/// loadCharacter builds a player: a race model, a composited skin, geosets
/// chosen from hair and facial hair, underwear. A creature is none of that -
/// its M2 names its own textures and has no geoset choices to make - so this
/// is the same three steps with the middle two thirds left out: read the M2,
/// frame the camera on its bounds, hand it to the renderer and stand it up.
///
/// The renderer is the one that already draws every unit in the world, so a
/// creature model needs nothing here that the world does not already do.
bool CharacterPreview::setBakedSkin(const std::string& bakePath) {
    if (!charRenderer_ || instanceId_ == 0 || bakePath.empty()) return false;
    // A bake is only right for the model it was baked for. Where an asset pack
    // has put an HD character model under a display, the bake knows nothing
    // about its face and the table is the better answer - the same rule, and
    // the same vertex ceiling, the world spawner applies.
    constexpr size_t kShippedCharacterVertexCeiling = 12000;
    if (const auto* md = charRenderer_->getModelData(PREVIEW_MODEL_ID)) {
        if (md->vertices.size() > kShippedCharacterVertexCeiling) return false;
    }
    VkTexture* tex = charRenderer_->loadTexture(bakePath);
    if (!tex) return false;
    charRenderer_->setTextureSlotOverride(
        instanceId_, static_cast<uint16_t>(skinTextureSlotIndex_), tex);
    return true;
}

bool CharacterPreview::loadCreature(
        const std::string& m2Path,
        const std::vector<std::pair<uint32_t, std::string>>& skins) {
    if (!charRenderer_ || !assetManager_ || !assetManager_->isInitialized()) {
        return false;
    }
    if (m2Path.empty()) return false;

    if (instanceId_ > 0) {
        charRenderer_->removeInstance(instanceId_);
        instanceId_ = 0;
    }

    pipeline::M2Model model;
    if (!loadPreviewM2(m2Path, model)) {
        LOG_WARNING("CharacterPreview: could not read creature model: ", m2Path);
        return false;
    }

    if (camera_) {
        // The declared bounds where there are any, and the vertices where
        // there are not - the same choice loadCharacter makes, and for the
        // same reason: a model whose header bounds are wrong frames wrong.
        glm::vec3 frameMin = model.boundMin;
        glm::vec3 frameMax = model.boundMax;
        if (!model.vertices.empty()) {
            glm::vec3 tightMin(std::numeric_limits<float>::max());
            glm::vec3 tightMax(-std::numeric_limits<float>::max());
            for (const auto& v : model.vertices) {
                if (!isFiniteVec3(v.position)) continue;
                tightMin = glm::min(tightMin, v.position);
                tightMax = glm::max(tightMax, v.position);
            }
            if (tightMin.x <= tightMax.x && tightMin.y <= tightMax.y &&
                tightMin.z <= tightMax.z) {
                frameMin = tightMin;
                frameMax = tightMax;
            }
        }
        frameCameraForModelBounds(*camera_, frameMin, frameMax);
        modelBoundMinZ_ = frameMin.z;
        modelBoundMaxZ_ = frameMax.z;
        modelHeadZ_ = 0.0f;
        if (charRenderer_ && instanceId_ > 0) {
            float headZ = 0.0f;
            if (charRenderer_->getInstanceKeyBonePivotZ(instanceId_, kKeyBoneHead, headZ)) {
                modelHeadZ_ = headZ;
            }
        }
        fullBodyDistance_ = camera_->getPosition().y;
    }

    // The artist's own portrait framing, where the model carries one.
    portraitCameraValid_ = false;
    for (const auto& cam : model.cameras) {
        if (cam.type != 0) continue;
        if (!isFiniteVec3(cam.positionBase) || !isFiniteVec3(cam.targetBase)) break;
        portraitCameraPos_ = cam.positionBase;
        portraitCameraTarget_ = cam.targetBase;
        portraitCameraFovDegrees_ =
            (cam.fov > 0.0f && cam.fov < 3.14159f) ? glm::degrees(cam.fov) : 0.0f;
        portraitCameraValid_ = true;
        break;
    }

    if (!charRenderer_->loadModel(model, PREVIEW_MODEL_ID)) {
        LOG_WARNING("CharacterPreview: failed to upload creature model");
        return false;
    }

    instanceId_ = charRenderer_->createInstance(PREVIEW_MODEL_ID,
        glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, glm::radians(modelYaw_)),
        1.0f);
    if (instanceId_ == 0) {
        LOG_WARNING("CharacterPreview: failed to create creature instance");
        return false;
    }

    // The display's skins, into the slots the model declared for them. Without
    // this the geometry draws with no texture at all, which is what the target
    // frame's portrait showed: a white shape in the right silhouette.
    if (const auto* data = charRenderer_->getModelData(PREVIEW_MODEL_ID)) {
        for (size_t ti = 0; ti < data->textures.size(); ++ti) {
            for (const auto& [texType, path] : skins) {
                if (data->textures[ti].type != texType) continue;
                if (VkTexture* tex = charRenderer_->loadTexture(path)) {
                    charRenderer_->setModelTexture(PREVIEW_MODEL_ID,
                                                   static_cast<uint32_t>(ti), tex);
                }
                break;
            }
        }
    }

    // No geoset selection: a creature shows every submesh its skin declares,
    // which is what leaving the active set alone means.
    charRenderer_->playAnimation(instanceId_, rendering::anim::STAND, true);
    modelLoaded_ = true;
    return true;
}

bool CharacterPreview::loadPreviewM2(const std::string& m2Path, pipeline::M2Model& outModel) {
    if (!assetManager_) return false;
    return pipeline::loadM2WithSkin(*assetManager_, m2Path, outModel);  // m2_asset_loader.hpp
}

void CharacterPreview::attachWeapons(const std::vector<game::EquipmentItem>& equipment) {
    if (!charRenderer_ || !assetManager_ || instanceId_ == 0) return;

    // Attachment 1 = right hand, 2 = left hand.
    charRenderer_->detachWeapon(instanceId_, 1);
    charRenderer_->detachWeapon(instanceId_, 2);

    // Shoulders (INVTYPE_SHOULDER = 3) through the same helper the world uses.
    // The helm is still left off here: hair is kept visible in the preview
    // (see setEquipment), and a helm over it would clip.
    {
        uint32_t shoulderDisplayId = 0;
        for (const auto& item : equipment) {
            if (item.displayModel != 0 && item.inventoryType == 3) {
                shoulderDisplayId = item.displayModel;
                break;
            }
        }
        core::attachShoulders(*charRenderer_, *assetManager_, instanceId_, shoulderDisplayId,
                              [this](const std::string& m2Path) { return previewModelIdFor(m2Path); });
    }

    auto displayInfoDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc || !displayInfoDbc->isLoaded()) return;


    struct WeaponSlot {
        std::initializer_list<uint8_t> invTypes;
        uint32_t attachmentId;
    };
    // Main hand also covers two-handers and ranged; off hand covers shields and held items.
    const WeaponSlot slots[] = {
        { .invTypes = {13, 17, 21, 15, 25, 26}, .attachmentId = 1 },
        { .invTypes = {14, 22, 23},             .attachmentId = 2 },
    };

    for (const auto& ws : slots) {
        uint32_t displayId = 0;
        uint32_t itemVisualId = 0;
        for (const auto& item : equipment) {
            if (item.displayModel == 0) continue;
            for (uint8_t t : ws.invTypes) {
                if (item.inventoryType == t) {
                    displayId = item.displayModel;
                    // SMSG_CHAR_ENUM already reports the enchant as its ItemVisual id.
                    itemVisualId = item.enchantment;
                    break;
                }
            }
            if (displayId != 0) break;
        }
        if (displayId == 0) continue;

        int32_t recIdx = displayInfoDbc->findRecordById(displayId);
        if (recIdx < 0) continue;

        const auto art = pipeline::readItemDisplayArt(*displayInfoDbc,
                                                      static_cast<uint32_t>(recIdx));
        if (art.modelFile.empty()) continue;
        const std::string& modelFile = art.modelFile;
        const std::string& textureName = art.textureName;

        pipeline::M2Model weaponModel;
        std::string m2Path = "Item\\ObjectComponents\\Weapon\\" + modelFile;
        if (!loadPreviewM2(m2Path, weaponModel)) {
            m2Path = "Item\\ObjectComponents\\Shield\\" + modelFile;
            if (!loadPreviewM2(m2Path, weaponModel)) {
                LOG_WARNING("CharacterPreview: failed to load weapon model ", modelFile);
                continue;
            }
        }

        std::string texturePath;
        if (!textureName.empty()) {
            texturePath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
            if (!assetManager_->fileExists(texturePath)) {
                texturePath = "Item\\ObjectComponents\\Shield\\" + textureName + ".blp";
            }
        }

        const uint32_t weaponModelId = previewModelIdFor(m2Path);
        if (!charRenderer_->attachWeapon(instanceId_, ws.attachmentId, weaponModel,
                                         weaponModelId, texturePath)) {
            continue;
        }
        attachWeaponEnchantVisual(ws.attachmentId, itemVisualId);
    }
}

uint32_t CharacterPreview::previewModelIdFor(const std::string& assetKey) {
    auto it = previewModelIds_.find(assetKey);
    if (it != previewModelIds_.end()) return it->second;
    uint32_t id = nextPreviewModelId_++;
    previewModelIds_.emplace(assetKey, id);
    return id;
}

void CharacterPreview::attachWeaponEnchantVisual(uint32_t attachmentId, uint32_t itemVisualId) {
    if (itemVisualId == 0 || !charRenderer_ || !assetManager_) return;

    auto visualsDbc = assetManager_->loadDBC("ItemVisuals.dbc");
    auto effectsDbc = assetManager_->loadDBC("ItemVisualEffects.dbc");
    if (!visualsDbc || !visualsDbc->isLoaded() || !effectsDbc || !effectsDbc->isLoaded()) return;

    auto effectModels = pipeline::resolveItemVisualModels(itemVisualId, visualsDbc.get(),
                                                          effectsDbc.get());

    for (uint32_t visualSlot = 0; visualSlot < effectModels.size(); ++visualSlot) {
        const std::string& modelName = effectModels[visualSlot];
        if (modelName.empty()) continue;

        std::string m2Path = modelName;
        size_t dot = m2Path.rfind('.');
        m2Path = (dot != std::string::npos ? m2Path.substr(0, dot) : m2Path) + ".m2";

        pipeline::M2Model effectModel;
        if (!loadPreviewM2(m2Path, effectModel)) {
            LOG_WARNING("CharacterPreview: failed to load enchant visual ", m2Path);
            continue;
        }
        charRenderer_->attachWeaponEffect(instanceId_, attachmentId, visualSlot,
                                          effectModel, previewModelIdFor(m2Path));
    }
}

void CharacterPreview::loadRacialBackdrop(game::Race race) {
    // Nothing to stand in front of when the background is meant to show
    // through. Skipped here rather than removed afterwards, so a portrait does
    // not read and build a scene on every rebuild only to discard it.
    if (transparentBackground_) return;

    if (!charRenderer_ || !assetManager_) return;
    // A different scene asked for by name (the death knight's) is a different
    // backdrop even for the same race.
    if (backdropRace_ == static_cast<int>(race) && backdropInstanceId_ != 0 &&
        backdropSceneLoaded_ == glueSceneOverride_) {
        // Appearance changes recreate the character instance but keep the racial
        // scene. Reapply its stand mark and camera rig to the new instance.
        applyPreviewView();
        return;
    }

    if (backdropInstanceId_ != 0) {
        charRenderer_->removeInstance(backdropInstanceId_);
        backdropInstanceId_ = 0;
    }
    setSceneParticles(nullptr);
    backdropRace_ = static_cast<int>(race);
    backdropSceneLoaded_ = glueSceneOverride_;
    sceneCameraValid_ = false;
    clearSceneLighting();
    previewStandPosition_ = glm::vec3(0.0f);
    previewViewDirection_ = glm::vec3(0.0f, 1.0f, 0.0f);
    modelYaw_ = 90.0f;
    applyPreviewView();

    // The glue screens each stand the character in their racial home - humans in
    // Stormwind, orcs in Durotar, and so on. Undead reuse the Scourge scene.
    const char* sceneName = nullptr;
    switch (race) {
        case game::Race::HUMAN:     sceneName = "UI_Human";    break;
        case game::Race::ORC:       sceneName = "UI_Orc";      break;
        case game::Race::DWARF:     sceneName = "UI_Dwarf";    break;
        case game::Race::NIGHT_ELF: sceneName = "UI_NightElf"; break;
        case game::Race::UNDEAD:    sceneName = "UI_Scourge";  break;
        case game::Race::TAUREN:    sceneName = "UI_Tauren";   break;
        // Blizzard does not ship separate Gnome or Troll glue scenes in the
        // Classic/TBC/WotLK asset sets. These races intentionally share their
        // faction partner's authored selection backdrop.
        case game::Race::GNOME:     sceneName = "UI_Dwarf";    break;
        case game::Race::TROLL:     sceneName = "UI_Orc";      break;
        case game::Race::BLOOD_ELF: sceneName = "UI_BloodElf"; break;
        case game::Race::DRAENEI:   sceneName = "UI_Draenei";  break;
        default: break;
    }
    // GetSelectBackgroundModel answers by class for a death knight.
    if (!glueSceneOverride_.empty()) sceneName = glueSceneOverride_.c_str();
    if (!sceneName) return;

    std::string scenePath = std::string("Interface\\Glues\\Models\\") + sceneName + "\\" +
                            sceneName + ".m2";
    pipeline::M2Model sceneModel;
    if (!loadPreviewM2(scenePath, sceneModel)) {
        LOG_WARNING("CharacterPreview: no racial backdrop at ", scenePath);
        return;
    }

    // These scenes are authored in their own space - the human one sits ~230 units
    // from its origin - and carry the camera and the spot the character stands on.
    // Without both there is no way to place the scene, so leave it out entirely
    // rather than drop geometry somewhere off-screen.
    if (sceneModel.cameras.empty()) {
        LOG_WARNING("CharacterPreview: racial backdrop ", scenePath, " has no camera; skipping");
        return;
    }
    const auto& sceneCam = sceneModel.cameras[0];

    // Attachment 0 is the character's mark on the scene's ground.
    glm::vec3 standPos = sceneCam.targetBase;
    for (const auto& att : sceneModel.attachments) {
        if (att.id == 0) { standPos = att.position; break; }
    }

    if (!charRenderer_->loadModel(sceneModel, PREVIEW_BACKDROP_MODEL_ID)) {
        LOG_WARNING("CharacterPreview: failed to load racial backdrop ", scenePath);
        return;
    }

    backdropInstanceId_ = charRenderer_->createInstance(PREVIEW_BACKDROP_MODEL_ID, glm::vec3(0.0f));
    if (backdropInstanceId_ == 0) return;
    charRenderer_->setInstanceSceneModel(backdropInstanceId_, true);
    setSceneParticles(&sceneModel);

    // Keep the character on the scene's authored stand mark and use the scene
    // camera only for viewing direction. Distance and focus come from our portrait
    // rig so scroll-wheel zoom can move naturally toward the face.
    // The scene's own camera, kept for the stock character select, which
    // looks through it unchanged.
    sceneCameraPos_ = sceneCam.positionBase;
    sceneCameraTarget_ = sceneCam.targetBase;
    sceneCameraFovDiagonal_ = (sceneCam.fov > 0.0f && sceneCam.fov < 3.14159f) ? sceneCam.fov : 0.0f;
    sceneCameraValid_ = true;
    charRenderer_->playAnimation(backdropInstanceId_, 0, true);
    setupSceneLighting(sceneModel, standPos);
    setSceneFog(sceneName);

    glm::vec3 toCamera = sceneCam.positionBase - sceneCam.targetBase;
    if (glm::length(toCamera) < 0.001f) toCamera = glm::vec3(0.0f, 1.0f, 0.0f);
    previewStandPosition_ = standPos;
    previewViewDirection_ = glm::normalize(toCamera);
    modelYaw_ = glm::degrees(std::atan2(previewViewDirection_.y, previewViewDirection_.x));
    applyPreviewView();

    LOG_INFO("CharacterPreview: racial backdrop ", scenePath,
             " stand=(", standPos.x, ",", standPos.y, ",", standPos.z, ")");
}

bool CharacterPreview::loadGlueScene(const std::string& scenePath) {
    if (!charRenderer_ || !assetManager_ || !camera_) return false;
    if (backdropInstanceId_ != 0) {
        charRenderer_->removeInstance(backdropInstanceId_);
        backdropInstanceId_ = 0;
    }
    setSceneParticles(nullptr);
    clearSceneLighting();
    pipeline::M2Model sceneModel;
    if (!loadPreviewM2(scenePath, sceneModel) || sceneModel.cameras.empty()) {
        LOG_WARNING("CharacterPreview: no usable glue scene at ", scenePath);
        return false;
    }
    if (!charRenderer_->loadModel(sceneModel, PREVIEW_BACKDROP_MODEL_ID)) return false;
    backdropInstanceId_ = charRenderer_->createInstance(PREVIEW_BACKDROP_MODEL_ID, glm::vec3(0.0f));
    if (backdropInstanceId_ == 0) return false;
    charRenderer_->setInstanceSceneModel(backdropInstanceId_, true);
    charRenderer_->playAnimation(backdropInstanceId_, 0, true);
    setSceneParticles(&sceneModel);
    // The scene's own camera, as ModelFFX shows it.
    const auto& cam = sceneModel.cameras[0];
    camera_->setPosition(cam.positionBase);
    const glm::vec3 fwd = glm::normalize(cam.targetBase - cam.positionBase);
    camera_->setRotation(glm::degrees(std::atan2(fwd.y, fwd.x)),
                         glm::degrees(std::asin(std::clamp(fwd.z, -1.0f, 1.0f))));
    if (cam.fov > 0.0f && cam.fov < 3.14159f) camera_->setFov(verticalFovFromDiagonal(cam.fov));
    setupSceneLighting(sceneModel, cam.targetBase);
    sceneOnly_ = true;
    LOG_INFO("CharacterPreview: glue scene ", scenePath);
    return true;
}

void CharacterPreview::clearSceneLighting() {
    sceneLit_ = false;
    sceneLocalLights_.clear();
    sceneFogFar_ = 0.0f;
    if (charRenderer_) charRenderer_->setPreviewLit(false);
}

void CharacterPreview::setSceneFog(const std::string& sceneName) {
    // GlueParent.lua's SetLighting: SetFogColor(CharModelFogInfo[race]),
    // SetFogNear(0), SetFogFar(far), else ClearFog. The key is the scene's
    // name without "UI_" (SetBackgroundModel's argument), so the troll's and
    // gnome's borrowed scenes take their partner's fog, as they do in stock.
    //
    // This haze is most of the human scene's look: Stormwind's stone is lit
    // by a 0.27 ambient and a low warm sun, and without the pink-gold fog
    // from 0 to 222 yards it read as near-black blocks with hard edges
    // against the sky (which is unfogged, M2 flag 0x2).
    struct Fog { const char* key; float r, g, b, far; };
    static constexpr Fog kFog[] = {
        {"HUMAN",           0.8f,  0.65f, 0.73f, 222.0f},
        {"ORC",             0.5f,  0.5f,  0.5f,  270.0f},
        {"DWARF",           0.85f, 0.88f, 1.0f,  500.0f},
        {"NIGHTELF",        0.25f, 0.22f, 0.55f, 611.0f},
        {"TAUREN",          1.0f,  0.61f, 0.42f, 153.0f},
        {"SCOURGE",         0.0f,  0.22f, 0.22f, 26.0f},
        {"CHARACTERSELECT", 0.8f,  0.65f, 0.73f, 222.0f},
    };
    std::string key = sceneName;
    if (key.size() > 3 && (key[0] == 'U' || key[0] == 'u') && (key[1] == 'I' || key[1] == 'i') &&
        key[2] == '_') {
        key = key.substr(3);
    }
    for (auto& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    sceneFogFar_ = 0.0f;
    for (const auto& f : kFog) {
        if (key == f.key) {
            sceneFogColor_ = glm::vec3(f.r, f.g, f.b);
            sceneFogFar_ = f.far;
            break;
        }
    }
}

void CharacterPreview::setupSceneLighting(const pipeline::M2Model& scene, const glm::vec3& focus) {
    // The first key of a track, which is all these scenes animate bar the
    // undead scene's torch flicker; fallback when the track has none.
    auto firstVec3 = [](const pipeline::M2AnimationTrack& t, const glm::vec3& fallback) {
        for (const auto& s : t.sequences)
            if (!s.vec3Values.empty()) return s.vec3Values.front();
        return fallback;
    };
    auto firstFloat = [](const pipeline::M2AnimationTrack& t, float fallback) {
        for (const auto& s : t.sequences)
            if (!s.floatValues.empty()) return s.floatValues.front();
        return fallback;
    };
    // A bone's rest transform, parents included: the pivot, the translation
    // track's first key, and the rotation's.
    auto boneRest = [&](int boneIndex, glm::quat& outRot, glm::vec3& outPos, const glm::vec3& p) {
        outRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        outPos = p;
        int guard = 0;
        while (boneIndex >= 0 && boneIndex < static_cast<int>(scene.bones.size()) && guard++ < 64) {
            const auto& b = scene.bones[boneIndex];
            glm::quat r(1.0f, 0.0f, 0.0f, 0.0f);
            for (const auto& s : b.rotation.sequences)
                if (!s.quatValues.empty()) { r = s.quatValues.front(); break; }
            const glm::vec3 tr = firstVec3(b.translation, glm::vec3(0.0f));
            outPos = b.pivot + tr + r * (outPos - b.pivot);
            outRot = r * outRot;
            boneIndex = b.parentBone;
        }
    };

    sceneLocalLights_.clear();
    sceneAmbient_ = glm::vec3(0.0f);
    sceneLightColor_ = glm::vec3(0.0f);
    sceneLightDir_ = glm::vec3(0.0f, 0.0f, -1.0f);
    struct Directional { glm::vec3 dir; glm::vec3 color; };
    std::vector<Directional> directionals;
    int visibleLights = 0;

    for (const auto& light : scene.lights) {
        if (firstFloat(light.visibility, 1.0f) < 0.5f) continue;
        ++visibleLights;
        sceneAmbient_ += firstVec3(light.ambientColor, glm::vec3(1.0f)) *
                         firstFloat(light.ambientIntensity, 0.0f);
        const glm::vec3 diffuse = firstVec3(light.diffuseColor, glm::vec3(1.0f)) *
                                  firstFloat(light.diffuseIntensity, 0.0f);
        if (glm::dot(diffuse, glm::vec3(1.0f)) <= 1e-4f) continue;
        glm::quat rot;
        glm::vec3 pos;
        boneRest(light.bone, rot, pos, light.position);
        if (light.type == 0) {
            // Down the bone's -Z. Checked against every scene that rotates
            // one: it points from the light toward the scene's focus each
            // time (human, tauren, draenei, blood elf, dwarf).
            directionals.push_back({glm::normalize(rot * glm::vec3(0.0f, 0.0f, -1.0f)), diffuse});
        } else {
            const float attStart = firstFloat(light.attenuationStart, 0.0f);
            const float attEnd = firstFloat(light.attenuationEnd, 0.0f);
            SceneLocalLight l;
            l.position = pos;
            l.radius = std::max({attEnd, attStart, 1.0f});
            const float peak = std::max({diffuse.r, diffuse.g, diffuse.b});
            l.color = diffuse / peak;
            l.intensity = peak;
            sceneLocalLights_.push_back(l);
        }
    }

    if (visibleLights == 0) {
        // No lights of its own: the scene at its textures' own brightness.
        sceneAmbient_ = glm::vec3(1.0f);
    } else if (!directionals.empty()) {
        // The shader has one sun. The brightest directional light is it; the
        // rest are stood far off along their directions as wide local lights.
        auto lum = [](const glm::vec3& c) { return glm::dot(c, glm::vec3(0.299f, 0.587f, 0.114f)); };
        std::sort(directionals.begin(), directionals.end(),
                  [&](const Directional& a, const Directional& b) { return lum(a.color) > lum(b.color); });
        sceneLightDir_ = directionals.front().dir;
        sceneLightColor_ = directionals.front().color;
        for (size_t i = 1; i < directionals.size(); ++i) {
            const float peak = std::max({directionals[i].color.r, directionals[i].color.g,
                                         directionals[i].color.b});
            SceneLocalLight l;
            l.position = focus - directionals[i].dir * 2000.0f;
            l.radius = 200000.0f;
            l.color = directionals[i].color / peak;
            l.intensity = peak;
            sceneLocalLights_.push_back(l);
        }
    }

    sceneLit_ = true;
    if (charRenderer_) charRenderer_->setPreviewLit(true);
    LOG_INFO("CharacterPreview: scene lights ", visibleLights, " ambient=(", sceneAmbient_.r, ",",
             sceneAmbient_.g, ",", sceneAmbient_.b, ") sun=(", sceneLightColor_.r, ",",
             sceneLightColor_.g, ",", sceneLightColor_.b, ") local=", sceneLocalLights_.size());
}

void CharacterPreview::update(float deltaTime) {
    if (charRenderer_ && (modelLoaded_ || sceneOnly_)) {
        // Racial glue scenes place the character far from world origin (the
        // human scene is roughly 230 units away). Use this preview's camera for
        // animation distance checks; testing against the renderer's default
        // origin culled bone and weapon-attachment updates, leaving the paper
        // doll rigid and its equipped weapons invisible.
        const glm::vec3 cameraPos = camera_ ? camera_->getPosition()
                                            : previewStandPosition_;
        charRenderer_->update(deltaTime, cameraPos);

        // The scene's emitters ride its skeleton and clocks, as just posed.
        if (sceneParticles_ && !sceneParticles_->empty() && backdropInstanceId_ != 0) {
            const std::vector<glm::mat4>* bones = nullptr;
            glm::mat4 modelMat(1.0f);
            int seq = 0;
            float animMs = 0.0f, gsMs = 0.0f;
            if (charRenderer_->getInstancePose(backdropInstanceId_, bones, modelMat, seq, animMs, gsMs) &&
                bones) {
                sceneParticles_->update(deltaTime, *bones, modelMat, seq, animMs, gsMs);
            }
        }
    }
}

void CharacterPreview::setSceneParticles(const pipeline::M2Model* scene) {
    if (!scene || scene->particleEmitters.empty()) {
        if (sceneParticles_) sceneParticles_->clear();
        return;
    }
    if (!charRenderer_ || !renderTarget_ || !renderTarget_->isValid()) return;
    if (!sceneParticles_) {
        auto particles = std::make_unique<GlueSceneParticles>();
        if (!particles->initialize(vkCtx_, renderTarget_->getRenderPass(),
                                   renderTarget_->getSampleCount())) {
            return;
        }
        sceneParticles_ = std::move(particles);
    }
    sceneParticles_->setScene(*scene, *charRenderer_);
}

void CharacterPreview::render() {
    // No-op - actual rendering happens in compositePass() called from Renderer::beginFrame()
}

void CharacterPreview::compositePass(VkCommandBuffer cmd, uint32_t frameIndex) {
    // Only composite when a UI screen actually requested it this frame
    if (!compositeRequested_) return;
    compositeRequested_ = false;

    if (!charRenderer_ || !camera_ || !(modelLoaded_ || sceneOnly_) || !renderTarget_ || !renderTarget_->isValid()) {
        return;
    }

    uint32_t fi = frameIndex % MAX_FRAMES;

    // Update per-frame UBO with preview camera matrices and studio lighting
    GPUPerFrameData ubo{};
    ubo.view = camera_->getViewMatrix();
    ubo.projection = camera_->getProjectionMatrix();
    ubo.lightSpaceMatrix = glm::mat4(1.0f);
    // Studio lighting: key light from upper-right-front
    ubo.lightDir = glm::vec4(glm::normalize(glm::vec3(0.5f, -0.7f, 0.5f)), 0.0f);
    ubo.lightColor = glm::vec4(1.0f, 0.95f, 0.9f, 0.0f);
    ubo.ambientColor = glm::vec4(0.35f, 0.35f, 0.4f, 0.0f);
    ubo.viewPos = glm::vec4(camera_->getPosition(), 0.0f);
    // No fog in preview
    ubo.fogColor = glm::vec4(0.05f, 0.05f, 0.1f, 0.0f);
    ubo.fogParams = glm::vec4(9999.0f, 10000.0f, 0.0f, 0.0f);
    // Off-screen preview has no real shadow pass/light-space setup. Sampling
    // the global shadow binding here can produce unstable fragments on some
    // drivers, so keep the portrait on studio lighting only.
    ubo.shadowParams = glm::vec4(0.0f);

    // A glue scene is lit by its own lights, not the studio rig: the login
    // screen by its pale blue ambient alone, the human scene by a dim ambient
    // and two directional lights. The scenes are hundreds of yards across, so
    // the fog is pushed out of reach as well.
    if (sceneLit_) {
        ubo.lightDir = glm::vec4(sceneLightDir_, 0.0f);
        ubo.lightColor = glm::vec4(sceneLightColor_, 0.0f);
        ubo.ambientColor = glm::vec4(sceneAmbient_, 0.0f);
        if (sceneFogFar_ > 0.0f) {
            ubo.fogColor = glm::vec4(sceneFogColor_, 0.0f);
            ubo.fogParams = glm::vec4(0.0f, sceneFogFar_, 0.0f, 0.0f);
        } else {
            ubo.fogParams = glm::vec4(1.0e6f, 1.0e6f + 1.0f, 0.0f, 0.0f);
        }
        const size_t n = std::min<size_t>(sceneLocalLights_.size(), MAX_LOCAL_LIGHTS);
        for (size_t i = 0; i < n; ++i) {
            const auto& l = sceneLocalLights_[i];
            ubo.localLightPosRadius[i] = glm::vec4(l.position, l.radius);
            ubo.localLightColorIntensity[i] = glm::vec4(l.color, l.intensity);
        }
        ubo.localLightMeta = glm::ivec4(static_cast<int>(n), 0, 0, 0);
    }

    std::memcpy(previewUBOMapped_[fi], &ubo, sizeof(GPUPerFrameData));

    // Begin off-screen render pass
    // Nothing at all behind a portrait, so the frame art around it shows
    // through; the studio backdrop everywhere else.
    VkClearColorValue clearColor = transparentBackground_
        ? VkClearColorValue{{0.0f, 0.0f, 0.0f, 0.0f}}
        : VkClearColorValue{{0.05f, 0.05f, 0.1f, 1.0f}};
    renderTarget_->beginPass(cmd, clearColor);

    // Preview rendering bypasses Renderer::renderWorld(), so it must run the
    // same resource-preparation hook itself after server appearance data has
    // produced bone matrices. This preserves lazy data loading while keeping
    // GPU allocation outside CharacterRenderer::render().
    charRenderer_->prepareRender(fi);

    // Render the character model
    charRenderer_->render(cmd, previewPerFrameSet_[fi], *camera_);

    // The scene's particles last: they test depth against the scene and the
    // character but do not write it.
    // Their own shaders and push constants; the per-frame set is not bound.
    if (sceneParticles_ && !sceneParticles_->empty()) {
        const glm::mat4 proj = camera_->getProjectionMatrix();
        const float pixelsPerUnit = std::abs(proj[1][1]) * static_cast<float>(fboHeight_) * 0.5f;
        sceneParticles_->render(cmd, proj * camera_->getViewMatrix(), camera_->getPosition(),
                                pixelsPerUnit, fi);
    }

    renderTarget_->endPass(cmd);

    compositeRendered_ = true;
}

void CharacterPreview::rotate(float yawDelta) {
    modelYaw_ = std::fmod(modelYaw_ + yawDelta, 360.0f);
    if (instanceId_ > 0 && charRenderer_) {
        charRenderer_->setInstanceRotation(instanceId_, glm::vec3(0.0f, 0.0f, glm::radians(modelYaw_)));
    }
}

void CharacterPreview::zoom(float wheelDelta) {
    if (!std::isfinite(wheelDelta) || wheelDelta == 0.0f) return;
    zoomLevel_ = std::clamp(zoomLevel_ + wheelDelta * 0.12f, 0.0f, 1.0f);
    applyPreviewView();
}

void CharacterPreview::setTransparentBackground(bool transparent) {
    transparentBackground_ = transparent;
    // The scene model behind the character is as opaque as the clear colour,
    // so it goes as well.
    if (transparent && backdropInstanceId_ != 0 && charRenderer_) {
        charRenderer_->removeInstance(backdropInstanceId_);
        backdropInstanceId_ = 0;
        backdropRace_ = -1;
        setSceneParticles(nullptr);
    }
    if (transparent) clearSceneLighting();
}

void CharacterPreview::setPortraitFraming() {
    zoomLevel_ = 1.0f;
    // Straight at the character. The model is turned to face the camera in the
    // same breath, which is the part loadRacialBackdrop would otherwise be the
    // only place to do.
    previewViewDirection_ = glm::vec3(0.0f, 1.0f, 0.0f);
    modelYaw_ = glm::degrees(std::atan2(previewViewDirection_.y,
                                        previewViewDirection_.x));
    applyPreviewView();
}

void CharacterPreview::resetView() {
    zoomLevel_ = 0.0f;
    applyPreviewView();
}

float CharacterPreview::verticalFovFromDiagonal(float diagonalRadians) const {
    // A 3.3.5 M2 camera's field of view is diagonal; the vertical one the
    // projection wants is that over sqrt(1 + aspect^2) for this target.
    const float aspect = (fboHeight_ > 0) ? static_cast<float>(fboWidth_) /
                                                static_cast<float>(fboHeight_)
                                          : 1.0f;
    return glm::degrees(diagonalRadians / std::sqrt(1.0f + aspect * aspect));
}

void CharacterPreview::setSceneCameraFraming(bool on) {
    sceneCameraFraming_ = on;
    applyPreviewView();
}

void CharacterPreview::setGlueSceneOverride(const std::string& sceneName) {
    glueSceneOverride_ = sceneName;
}

void CharacterPreview::applyPreviewView() {
    if (!camera_) return;

    // The stock character select: the racial scene's own camera, untouched,
    // looking at the character on the scene's stand mark.
    if (sceneCameraFraming_ && sceneCameraValid_ && zoomLevel_ <= 0.0f) {
        camera_->setPosition(sceneCameraPos_);
        const glm::vec3 d = sceneCameraTarget_ - sceneCameraPos_;
        if (glm::length(d) > 0.0001f) {
            const glm::vec3 fwd = glm::normalize(d);
            camera_->setRotation(glm::degrees(std::atan2(fwd.y, fwd.x)),
                                 glm::degrees(std::asin(std::clamp(fwd.z, -1.0f, 1.0f))));
        }
        if (sceneCameraFovDiagonal_ > 0.0f) {
            if (defaultFovDegrees_ <= 0.0f) defaultFovDegrees_ = camera_->getFovDegrees();
            camera_->setFov(verticalFovFromDiagonal(sceneCameraFovDiagonal_));
        }
        if (instanceId_ > 0 && charRenderer_) {
            charRenderer_->setInstancePosition(instanceId_, previewStandPosition_);
            charRenderer_->setInstanceRotation(instanceId_, glm::vec3(0.0f, 0.0f, glm::radians(modelYaw_)));
        }
        return;
    }

    // A model that carries its own portrait camera is framed by it. The
    // fraction-of-height guess below assumes the bind pose is the drawn pose,
    // which holds for a player and not for a creature that hunches: a gnoll
    // stands 2.04 tall in bind pose, so 82% of it aimed at 1.67 while the
    // artist's own camera looks at 0.96, and the portrait showed the top of
    // its head. The camera is placed in model space, so it is offset by where
    // the model stands and the model is left in its own orientation.
    if (zoomLevel_ >= 1.0f && portraitCameraValid_) {
        // Room for the idle to move in.
        //
        // The artist's camera frames the pose Stand opens on, tightly. Left at
        // exactly that, a gnoll swings its head out of shot for most of the
        // loop. Backing off along the same axis keeps the framing the artist
        // chose - the angle, the target, the whole composition - and only
        // loosens the crop, which is the one thing an animated portrait needs
        // and a still one does not. One number, tune it by eye.
        constexpr float kPortraitHeadroom = 1.35f;
        const glm::vec3 focus = previewStandPosition_ + portraitCameraTarget_;
        const glm::vec3 camPos =
            focus + (portraitCameraPos_ - portraitCameraTarget_) * kPortraitHeadroom;
        camera_->setPosition(camPos);
        const glm::vec3 forward = glm::normalize(focus - camPos);
        camera_->setRotation(glm::degrees(std::atan2(forward.y, forward.x)),
                             glm::degrees(std::asin(std::clamp(forward.z, -1.0f, 1.0f))));
        if (portraitCameraFovDegrees_ > 0.0f) {
            if (defaultFovDegrees_ <= 0.0f) defaultFovDegrees_ = camera_->getFovDegrees();
            camera_->setFov(portraitCameraFovDegrees_);
        }
        if (instanceId_ > 0 && charRenderer_) {
            charRenderer_->setInstancePosition(instanceId_, previewStandPosition_);
            charRenderer_->setInstanceRotation(instanceId_, glm::vec3(0.0f));
        }
        return;
    }

    // Whatever a portrait camera last set, handed back.
    if (defaultFovDegrees_ > 0.0f) camera_->setFov(defaultFovDegrees_);

    const float modelHeight = std::max(modelBoundMaxZ_ - modelBoundMinZ_, 0.1f);
    const float bodyFocusZ = (modelBoundMinZ_ + modelBoundMaxZ_) * 0.5f;

    // Aim at the bone the head is on, not at a share of the bounding box.
    //
    // Measured across the player models, the head bone sits at 82% of height
    // on a tauren and a dwarf - which is why 82% was chosen and why it looked
    // right - but at 87% on a human, 89% on a night elf and 60% on a gnome. A
    // gnome's hair and oversized head are 40% of everything above its feet, so
    // the fraction aimed 26cm over the top of its skull and the face fell out
    // of the bottom of the frame. The skeleton has no such opinion: the head
    // bone is where the head is, on every race, by construction.
    //
    // This is what the creature path already does by another route, reading
    // the artist's own portrait camera - whose target for a gnoll is within a
    // few centimetres of that model's head bone.
    const float faceFocusZ = modelHeadZ_ > 0.001f
                                 ? modelHeadZ_
                                 : modelBoundMinZ_ + modelHeight * 0.82f;
    const float focusZ = bodyFocusZ + (faceFocusZ - bodyFocusZ) * zoomLevel_;

    // Stay outside the near plane while getting close enough to inspect facial
    // textures and hair. Large races retain proportionally more distance.
    const float faceDistance = std::max(1.15f, modelHeight * 0.70f);
    const float distance = fullBodyDistance_ +
                           (std::min(faceDistance, fullBodyDistance_) - fullBodyDistance_) * zoomLevel_;
    const glm::vec3 focus = previewStandPosition_ + glm::vec3(0.0f, 0.0f, focusZ);
    const glm::vec3 camPos = focus + previewViewDirection_ * distance;
    camera_->setPosition(camPos);

    const glm::vec3 forward = glm::normalize(focus - camPos);
    const float yaw = glm::degrees(std::atan2(forward.y, forward.x));
    const float pitch = glm::degrees(std::asin(std::clamp(forward.z, -1.0f, 1.0f)));
    camera_->setRotation(yaw, pitch);

    if (instanceId_ > 0 && charRenderer_) {
        charRenderer_->setInstancePosition(instanceId_, previewStandPosition_);
        charRenderer_->setInstanceRotation(instanceId_, glm::vec3(0.0f, 0.0f, glm::radians(modelYaw_)));
    }
}

} // namespace rendering
} // namespace wowee
