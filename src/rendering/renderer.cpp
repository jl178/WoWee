#include "rendering/renderer.hpp"
#include "rendering/bounce_grid_gpu.hpp"
#include "rendering/sun_direction.hpp"
#include "rendering/enhanced_grade.hpp"
#include "rendering/enhanced_palette.hpp"
#include "rendering/enhanced_tonemap.hpp"
#include "rendering/leaf_light.hpp"

#include <cstddef>
#include <cstring>
#include <bit>
#include <fstream>
#include <iterator>
#include "addons/lua_api_registrations.hpp"
#include "core/env_flag.hpp"
#include "rendering/sky_params_from_lighting.hpp"
#include "core/coordinates.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "pipeline/custom_zone_discovery.hpp"
#include "rendering/performance_hud.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/water_caustics.hpp"
#include "rendering/skybox.hpp"
#include "rendering/celestial.hpp"
#include "rendering/starfield.hpp"
#include "rendering/clouds.hpp"
#include "rendering/lens_flare.hpp"
#include "rendering/weather.hpp"
#include "rendering/wet_ground.hpp"
#include "rendering/lightning.hpp"
#include "rendering/lighting_manager.hpp"
#include "core/profiler.hpp"
#include "core/thread_pool.hpp"
#include "rendering/sky_system.hpp"
#include "rendering/swim_effects.hpp"
#include "rendering/footfall_dust.hpp"
#include "rendering/footfall_dust_rules.hpp"
#include "rendering/water_pass_order.hpp"
#include "rendering/levelup_effect.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/character_preview.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "pipeline/grass_profile.hpp"
#include "pipeline/grass_population.hpp"
#include "pipeline/grass_terrain.hpp"
#include "rendering/grass_renderer.hpp"
#include "rendering/hiz_system.hpp"
#include "rendering/volumetric_fog.hpp"
#include "rendering/sun_shafts.hpp"
#include "rendering/soft_particle_pass.hpp"
#include "rendering/contact_shadow_pass.hpp"
#include "rendering/rain_ripples.hpp"
#include "rendering/stream_fade.hpp"
#include "rendering/rt_lighting.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/screen_capture.hpp"
#include "rendering/loot_sparkles.hpp"
#include "rendering/minimap.hpp"
#include "rendering/world_map.hpp"
#include "rendering/quest_marker_renderer.hpp"
#include "rendering/footprint_renderer.hpp"
#include "game/game_handler.hpp"
#include "pipeline/m2_loader.hpp"
#include <algorithm>
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/terrain_mesh.hpp"
#include "core/application.hpp"
#include "core/window.hpp"
#include "core/logger.hpp"
#include "game/world.hpp"
#include "game/zone_manager.hpp"
#include "rendering/stock_weather.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/music_manager.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/player_voice_manager.hpp"
#include "audio/ambient_sound_manager.hpp"
#include "audio/ui_sound_manager.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/spell_sound_manager.hpp"
#include "audio/movement_sound_manager.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/amd_fsr3_runtime.hpp"
#include "rendering/spell_visual_system.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/stock_screen_fx.hpp"
#include "rendering/stock_wmo_light.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/render_graph.hpp"
#include "rendering/overlay_system.hpp"
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cctype>
#include <cmath>
#include <chrono>
#include <filesystem>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <cstdlib>
#include <optional>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <future>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace wowee {
namespace rendering {



Renderer::Renderer() = default;
Renderer::~Renderer() = default;

bool Renderer::createPerFrameResources() {
    VkDevice device = vkCtx->getDevice();

    // --- Create per-frame shadow depth images (one per in-flight frame) ---
    // Each frame slot has its own depth image so that frame N's shadow read and
    // frame N+1's shadow write cannot race on the same image.
    VkImageCreateInfo imgCI{};
    imgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType = VK_IMAGE_TYPE_2D;
    imgCI.format = shadowDepthFormat();
    imgCI.extent = {.width = SHADOW_MAP_SIZE, .height = SHADOW_MAP_SIZE, .depth = 1};
    imgCI.mipLevels = 1;
    imgCI.arrayLayers = 1;
    imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo imgAllocCI{};
    imgAllocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (vmaCreateImage(vkCtx->getAllocator(), &imgCI, &imgAllocCI,
                &shadowDepthImage[i], &shadowDepthAlloc[i], nullptr) != VK_SUCCESS) {
            LOG_ERROR("Failed to create shadow depth image [", i, "]");
            return false;
        }
        shadowDepthLayout_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    // --- Create per-frame shadow depth image views ---
    VkImageViewCreateInfo viewCI{};
    viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format = shadowDepthFormat();
    viewCI.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        viewCI.image = shadowDepthImage[i];
        if (vkCreateImageView(device, &viewCI, nullptr, &shadowDepthView[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create shadow depth image view [", i, "]");
            return false;
        }
    }

    // --- The unit map (rendering/shadow_cache.hpp): one per frame slot, drawn
    //     every frame, half the quality level's side (a quarter of the world map's) ---
    SHADOW_NEAR_SIZE = std::max(512u, shadowLevelSide_ / 2);
    imgCI.extent = {.width = SHADOW_NEAR_SIZE, .height = SHADOW_NEAR_SIZE, .depth = 1};
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (vmaCreateImage(vkCtx->getAllocator(), &imgCI, &imgAllocCI,
                &shadowNearImage[i], &shadowNearAlloc[i], nullptr) != VK_SUCCESS) {
            LOG_ERROR("Failed to create unit shadow image [", i, "]");
            return false;
        }
        shadowNearLayout_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        viewCI.image = shadowNearImage[i];
        if (vkCreateImageView(device, &viewCI, nullptr, &shadowNearView[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create unit shadow image view [", i, "]");
            return false;
        }
    }

    // --- Create shadow sampler (shared - read-only, no per-frame needed) ---
    VkSamplerCreateInfo sampCI{};
    sampCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampCI.magFilter = VK_FILTER_LINEAR;
    sampCI.minFilter = VK_FILTER_LINEAR;
    sampCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampCI.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sampCI.compareEnable = VK_TRUE;
    sampCI.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    shadowSampler = vkCtx->getOrCreateSampler(sampCI);
    if (shadowSampler == VK_NULL_HANDLE) {
        LOG_ERROR("Failed to create shadow sampler");
        return false;
    }

    // --- Create shadow render pass (depth-only) ---
    VkAttachmentDescription depthAtt{};
    depthAtt.format = shadowDepthFormat();
    depthAtt.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 0;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dep.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpCI{};
    rpCI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpCI.attachmentCount = 1;
    rpCI.pAttachments = &depthAtt;
    rpCI.subpassCount = 1;
    rpCI.pSubpasses = &subpass;
    rpCI.dependencyCount = 1;
    rpCI.pDependencies = &dep;
    if (vkCreateRenderPass(device, &rpCI, nullptr, &shadowRenderPass) != VK_SUCCESS) {
        LOG_ERROR("Failed to create shadow render pass");
        return false;
    }
    // The same pass keeping what is there: the second and third parts of a
    // world map redraw go on top of the first. Compatible with the one
    // above, so the casters' pipelines and the framebuffers serve both.
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    if (vkCreateRenderPass(device, &rpCI, nullptr, &shadowRenderPassLoad) != VK_SUCCESS) {
        LOG_ERROR("Failed to create shadow render pass (load)");
        return false;
    }

    // --- Create per-frame shadow framebuffers ---
    VkFramebufferCreateInfo fbCI{};
    fbCI.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbCI.renderPass = shadowRenderPass;
    fbCI.attachmentCount = 1;
    fbCI.width = SHADOW_MAP_SIZE;
    fbCI.height = SHADOW_MAP_SIZE;
    fbCI.layers = 1;
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        fbCI.pAttachments = &shadowDepthView[i];
        if (vkCreateFramebuffer(device, &fbCI, nullptr, &shadowFramebuffer[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create shadow framebuffer [", i, "]");
            return false;
        }
    }
    fbCI.width = SHADOW_NEAR_SIZE;
    fbCI.height = SHADOW_NEAR_SIZE;
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        fbCI.pAttachments = &shadowNearView[i];
        if (vkCreateFramebuffer(device, &fbCI, nullptr, &shadowNearFramebuffer[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create unit shadow framebuffer [", i, "]");
            return false;
        }
    }

    // The fog's sampler and neutral volume come first: the layout below bakes
    // the one in, and every set written below binds the other until the fog
    // has volumes of its own.
    volumetricFog_ = std::make_unique<VolumetricFog>();
    if (!volumetricFog_->initialize(vkCtx)) {
        LOG_ERROR("Failed to create the volumetric fog's sampler and neutral volume");
        return false;
    }

    // The ray traced lighting's scene and pass. They outlive the per-frame
    // sets: the renderers register geometry with the scene as they load, and
    // bindings 3 and 4 below take the pass's sampler as immutable.
    if (!rtScene_) {
        rtScene_ = std::make_unique<RtScene>();
        if (!rtScene_->initialize(vkCtx)) {
            LOG_ERROR("Failed to create the ray tracing scene");
            return false;
        }
        rtLighting_ = std::make_unique<RtLighting>();
        if (!rtLighting_->initialize(vkCtx, rtScene_.get())) {
            LOG_ERROR("Failed to create the ray traced lighting");
            return false;
        }
    }

    // --- Create descriptor set layout for set 0 (per-frame UBO + shadow sampler + fog volume
    //     + ray traced lighting) ---
    VkDescriptorSetLayoutBinding bindings[6]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    // Compute as well: the fog volume is lit from this same block.
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                             VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // Immutable, because this one compares. Portability implementations may
    // report VkPhysicalDevicePortabilitySubsetFeaturesKHR::mutableComparisonSamplers
    // as false -- MoltenVK does -- and then a sampler with compareEnable set is
    // only legal here, baked into the layout, rather than written into the
    // descriptor per frame. shadowSampler is created above this point.
    bindings[1].pImmutableSamplers = &shadowSampler;
    // The fog volume, read per fragment by surfaces and per vertex by
    // particles and ribbons. Immutable like binding 1, so a set allocated
    // anywhere else - the character preview's - only has to name a view.
    const VkSampler fogSampler = volumetricFog_->getSampler();
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].pImmutableSamplers = &fogSampler;
    // Last frame's ray traced lighting (RtLighting), read by the terrain,
    // building, doodad and character surfaces. Immutable samplers again, so
    // the preview's sets only name the neutral view.
    const VkSampler rtSampler = rtLighting_->sampler();
    for (uint32_t b = 3; b <= 4; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[b].pImmutableSamplers = &rtSampler;
    }

    // The unit shadow map (shadow_filter.glsli), compared like binding 1.
    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[5].pImmutableSamplers = &shadowSampler;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 6;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &perFrameSetLayout) != VK_SUCCESS) {
        LOG_ERROR("Failed to create per-frame descriptor set layout");
        return false;
    }

    // --- Create descriptor pool for UBO + image sampler (normal frames + reflection) ---
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = MAX_FRAMES * 2; // normal frames + reflection frames
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = MAX_FRAMES * 2 * 5;  // two shadow maps, fog, two RT results, per set

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = MAX_FRAMES * 2; // normal frames + reflection frames
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &sceneDescriptorPool) != VK_SUCCESS) {
        LOG_ERROR("Failed to create scene descriptor pool");
        return false;
    }

    // --- Create per-frame UBOs and descriptor sets ---
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        // Create mapped UBO
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = sizeof(GPUPerFrameData);
        bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo mapInfo{};
        if (vmaCreateBuffer(vkCtx->getAllocator(), &bufInfo, &allocInfo,
                &perFrameUBOs[i], &perFrameUBOAllocs[i], &mapInfo) != VK_SUCCESS) {
            LOG_ERROR("Failed to create per-frame UBO ", i);
            return false;
        }
        perFrameUBOMapped[i] = mapInfo.pMappedData;

        // Allocate descriptor set
        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool = sceneDescriptorPool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &perFrameSetLayout;

        if (vkAllocateDescriptorSets(device, &setAlloc, &perFrameDescSets[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate per-frame descriptor set ", i);
            return false;
        }

        // Write binding 0 (UBO) and binding 1 (shadow sampler)
        VkDescriptorBufferInfo descBuf{};
        descBuf.buffer = perFrameUBOs[i];
        descBuf.offset = 0;
        descBuf.range = sizeof(GPUPerFrameData);

        VkDescriptorImageInfo shadowImgInfo{};
        // sampler is ignored: binding 1 declares it immutable in the layout.
        // The front world map, which is image 0 until a redraw swaps them
        // (bindWorldShadowMap).
        shadowImgInfo.imageView = shadowDepthView[shadowFront_];
        shadowImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        shadowBoundView_[i] = shadowDepthView[shadowFront_];
        VkDescriptorImageInfo nearImgInfo{};
        nearImgInfo.imageView = shadowNearView[i];
        nearImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Neutral until the fog is switched on; writeFogVolumeBindings swaps
        // in this slot's own volume then.
        VkDescriptorImageInfo fogImgInfo{};
        fogImgInfo.imageView = volumetricFog_->getVolumeView(i);
        fogImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        // Neutral until the ray traced lighting is on; writeRtLightingBindings
        // swaps in the other slot's results then.
        VkDescriptorImageInfo rtImgInfo[2]{};
        rtImgInfo[0].imageView = rtLighting_->lightViewForSlot(i);
        rtImgInfo[1].imageView = rtLighting_->giViewForSlot(i);
        rtImgInfo[0].imageLayout = rtImgInfo[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[6]{};
        writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[5].dstSet = perFrameDescSets[i];
        writes[5].dstBinding = 5;
        writes[5].descriptorCount = 1;
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[5].pImageInfo = &nearImgInfo;
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = perFrameDescSets[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &descBuf;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = perFrameDescSets[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &shadowImgInfo;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = perFrameDescSets[i];
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &fogImgInfo;
        for (uint32_t b = 0; b < 2; ++b) {
            writes[3 + b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[3 + b].dstSet = perFrameDescSets[i];
            writes[3 + b].dstBinding = 3 + b;
            writes[3 + b].descriptorCount = 1;
            writes[3 + b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[3 + b].pImageInfo = &rtImgInfo[b];
        }

        vkUpdateDescriptorSets(device, 6, writes, 0, nullptr);
    }

    // --- Create reflection per-frame UBO and descriptor set ---
    {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = sizeof(GPUPerFrameData);
        bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo mapInfo{};
        if (vmaCreateBuffer(vkCtx->getAllocator(), &bufInfo, &allocInfo,
                &reflPerFrameUBO, &reflPerFrameUBOAlloc, &mapInfo) != VK_SUCCESS) {
            LOG_ERROR("Failed to create reflection per-frame UBO");
            return false;
        }
        reflPerFrameUBOMapped = mapInfo.pMappedData;

        VkDescriptorSetLayout layouts[MAX_FRAMES];
        for (auto& layout : layouts) layout = perFrameSetLayout;

        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool = sceneDescriptorPool;
        setAlloc.descriptorSetCount = MAX_FRAMES;
        setAlloc.pSetLayouts = layouts;

        if (vkAllocateDescriptorSets(device, &setAlloc, reflPerFrameDescSet) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate reflection per-frame descriptor sets");
            return false;
        }

        // Bind each reflection descriptor to the same UBO but its own frame's shadow view
        for (uint32_t i = 0; i < MAX_FRAMES; i++) {
            VkDescriptorBufferInfo descBuf{};
            descBuf.buffer = reflPerFrameUBO;
            descBuf.offset = 0;
            descBuf.range = sizeof(GPUPerFrameData);

            VkDescriptorImageInfo shadowImgInfo{};
            // sampler is ignored: binding 1 declares it immutable in the layout.
            shadowImgInfo.imageView = shadowDepthView[shadowFront_];
            shadowImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDescriptorImageInfo nearImgInfo{};
            nearImgInfo.imageView = shadowNearView[i];
            nearImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            // Always neutral: the volume is built for the camera, and the
            // mirrored one would read it at the wrong place. The reflection's
            // block switches the fog off.
            VkDescriptorImageInfo fogImgInfo{};
            fogImgInfo.imageView = volumetricFog_->getNeutralView();
            fogImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            // Neutral as well: the result is the camera's, and the block's
            // switch is off in the reflection's copy.
            VkDescriptorImageInfo rtImgInfo{};
            rtImgInfo.imageView = rtLighting_->neutralView();
            rtImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet writes[6]{};
            writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[5].dstSet = reflPerFrameDescSet[i];
            writes[5].dstBinding = 5;
            writes[5].descriptorCount = 1;
            writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[5].pImageInfo = &nearImgInfo;
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = reflPerFrameDescSet[i];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo = &descBuf;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = reflPerFrameDescSet[i];
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo = &shadowImgInfo;
            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = reflPerFrameDescSet[i];
            writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2].pImageInfo = &fogImgInfo;
            for (uint32_t b = 3; b <= 4; ++b) {
                writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[b].dstSet = reflPerFrameDescSet[i];
                writes[b].dstBinding = b;
                writes[b].descriptorCount = 1;
                writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[b].pImageInfo = &rtImgInfo;
            }

            vkUpdateDescriptorSets(device, 6, writes, 0, nullptr);
        }
    }

    // The compute side. Not fatal: without it the fog simply stays off, and
    // applyPendingQuality says so when it is asked for.
    const VkImageView fogShadowViews[MAX_FRAMES] = {shadowDepthView[shadowFront_], shadowDepthView[shadowFront_]};
    if (!volumetricFog_->createPipelines(perFrameSetLayout, fogShadowViews)) {
        LOG_WARNING("Volumetric fog pipelines failed to build - volumetric fog unavailable");
    }

    LOG_INFO("Per-frame Vulkan resources created (shadow map ", SHADOW_MAP_SIZE, "x", SHADOW_MAP_SIZE, ")");
    return true;
}

void Renderer::destroyPerFrameResources() {
    if (!vkCtx) return;
    vkDeviceWaitIdle(vkCtx->getDevice());
    VkDevice device = vkCtx->getDevice();

    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        destroy(vkCtx->getAllocator(), perFrameUBOs[i], perFrameUBOAllocs[i]);
    }
    if (reflPerFrameUBO) {
        vmaDestroyBuffer(vkCtx->getAllocator(), reflPerFrameUBO, reflPerFrameUBOAlloc);
        reflPerFrameUBO = VK_NULL_HANDLE;
        reflPerFrameUBOMapped = nullptr;
    }
    // Before the layout: its compute pipeline layout was built from it.
    if (volumetricFog_) {
        volumetricFog_->shutdown();
        volumetricFog_.reset();
    }
    destroy(device, sceneDescriptorPool);
    destroy(device, perFrameSetLayout);
    // After every renderer has let go of its geometry, which is why this is
    // here rather than beside the fog: shutdown() calls this last.
    if (rtLighting_) {
        rtLighting_->shutdown();
        rtLighting_.reset();
    }
    if (rtScene_) {
        rtScene_->shutdown();
        rtScene_.reset();
    }

    // Destroy per-frame shadow resources
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (shadowFramebuffer[i]) { vkDestroyFramebuffer(device, shadowFramebuffer[i], nullptr); shadowFramebuffer[i] = VK_NULL_HANDLE; }
        if (shadowDepthView[i]) { vkDestroyImageView(device, shadowDepthView[i], nullptr); shadowDepthView[i] = VK_NULL_HANDLE; }
        if (shadowDepthImage[i]) { vmaDestroyImage(vkCtx->getAllocator(), shadowDepthImage[i], shadowDepthAlloc[i]); shadowDepthImage[i] = VK_NULL_HANDLE; shadowDepthAlloc[i] = VK_NULL_HANDLE; }
        shadowDepthLayout_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (shadowNearFramebuffer[i]) { vkDestroyFramebuffer(device, shadowNearFramebuffer[i], nullptr); shadowNearFramebuffer[i] = VK_NULL_HANDLE; }
        if (shadowNearView[i]) { vkDestroyImageView(device, shadowNearView[i], nullptr); shadowNearView[i] = VK_NULL_HANDLE; }
        if (shadowNearImage[i]) { vmaDestroyImage(vkCtx->getAllocator(), shadowNearImage[i], shadowNearAlloc[i]); shadowNearImage[i] = VK_NULL_HANDLE; shadowNearAlloc[i] = VK_NULL_HANDLE; }
        shadowNearLayout_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        shadowBoundView_[i] = VK_NULL_HANDLE;
    }
    shadowFront_ = 0;
    shadowFrontKey_ = {};
    shadowRedraw_ = {};
    if (shadowRenderPass) { vkDestroyRenderPass(device, shadowRenderPass, nullptr); shadowRenderPass = VK_NULL_HANDLE; }
    if (shadowRenderPassLoad) { vkDestroyRenderPass(device, shadowRenderPassLoad, nullptr); shadowRenderPassLoad = VK_NULL_HANDLE; }
    shadowSampler = VK_NULL_HANDLE; // Owned by VkContext sampler cache
}

void Renderer::updatePerFrameUBO() {
    if (!camera) return;

    currentFrameData.view = camera->getViewMatrix();
    currentFrameData.projection = camera->getProjectionMatrix();
    if (waterRenderer) waterRenderer->setFrameViewProj(currentFrameData.projection * currentFrameData.view);
    // w, which no shader read, is the ground's relief; see setTerrainRelief.
    currentFrameData.viewPos = glm::vec4(camera->getPosition(), terrainRelief_);
    currentFrameData.fogParams.z = globalTime;

    // Lighting from LightingManager
    if (lightingManager) {
        const auto& lp = lightingManager->getLightingParams();
        // The world's ambient is the area's, wherever the camera is. It used
        // to be eased toward the building's MOHD ambient whenever the camera
        // entered an interior group, and every shader reads it - so stepping
        // into a Stormwind canal tunnel relit the whole Trade District seen
        // through its mouth.
        //
        // Units indoors still take the building's ambient (MOHD, stored half
        // intensity like MOCV). character.frag alone reads it, from the .w
        // slots no shader otherwise uses: lightDir.w, lightColor.w and
        // ambientColor.w carry its r, g and b, and shadowParams.w how far
        // indoors the player is.
        const glm::vec3 unitIndoorAmbient = glm::max(interiorAmbient_ * 2.0f, glm::vec3(0.35f));
        currentFrameData.lightDir = glm::vec4(lp.directionalDir, unitIndoorAmbient.r);
        currentFrameData.lightColor = glm::vec4(lp.diffuseColor * lp.sunScale, unitIndoorAmbient.g);
        currentFrameData.ambientColor = glm::vec4(lp.ambientColor, unitIndoorAmbient.b);
        // The HDR scene's sun is brighter by day (enhanced_tonemap::kSunLight,
        // Enhanced's F052), on everything the sun lights; the tone map takes
        // what passes 1. Nothing at night, nothing without the HDR scene.
        // The stock light itself stays 20-37 degrees up all night, so the
        // sun sprite's height (sunDir, toward the sun) says whether it is day.
        if (vkCtx && vkCtx->isHdrScene()) {
            const float g = enhanced_tonemap::sunLightScale(-lp.sunDir.z,
                                                           enhanced_tonemap::tuning().sunLight);
            currentFrameData.lightColor = glm::vec4(glm::vec3(currentFrameData.lightColor) * g,
                                                    currentFrameData.lightColor.w);
        }
        currentFrameData.fogColor = glm::vec4(lp.fogColor, 1.0f);
        currentFrameData.fogParams.x = lp.fogStart;
        currentFrameData.fogParams.y = lp.fogEnd;
        // The terrain highlight is the sun's colour (stock c27, channel 9).
        if (terrainRenderer) terrainRenderer->setSunSpecular(lp.sunColor);
        // Under water the fog is Light.dbc's underwater set (LightingManager
        // switches to it with the camera below the surface), so no fixed blue
        // is mixed in here any more.
        // A lightning flash, a sandstorm's haze (weather rows; nothing while
        // neither happens).
        applyWeatherFxLight();
    }

    currentFrameData.lightSpaceMatrix = lightSpaceMatrix;
    // The unit map's projection; its mask is filled in by the shadow pass,
    // which is where the casters are found (renderShadowPass).
    if (nearShadowOn_ && shadowsEnabled) {
        currentFrameData.nearLightSpaceMatrix = nearShadowProj_.matrix;
        currentFrameData.nearShadowParams = glm::vec4(
            1.0f, 1.0f / static_cast<float>(SHADOW_NEAR_SIZE),
            nearShadowProj_.depthRange > 0.0f ? 1.0f / nearShadowProj_.depthRange : 0.0f,
            nearShadowProj_.texelWorld);
    } else {
        currentFrameData.nearShadowParams = glm::vec4(0.0f);
    }
    // y is how dark the shadow is, and the sky decides it: Light.dbc's shadow
    // band through LightingManager, times the player's Shadow darkness. It
    // was named a bias and derived from the shadow distance - 0.8 at 300
    // yards, a tenth of that at 40 - so shortening the distance for sharper
    // shadows also all but erased them. The depth bias is the shaders' own,
    // from the surface's angle to the light and one texel of the map (z).
    // Under Enhanced's daylight its own shadow opacity
    // (enhanced_palette::shadowOpacity: held at 3.3.5's 0.3 for now, Enhanced's
    // measured 0.46 darkened the evening and forest views;
    // docs/re/enhanced-lighting.md 2e).
    const float skyShadowOpacity =
        lightingManager && lightingManager->isEnhancedActive() ? enhanced_palette::shadowOpacity()
        : lightingManager ? lightingManager->getLightingParams().shadowOpacity : 0.8f;
    const float shadowStrength = glm::clamp(skyShadowOpacity * shadowStrengthSetting_, 0.0f, 1.0f);
    // z carries one texel of the shadow map. The shaders used to hold that as
    // a constant for 4096, and the map is 512 to 4096 by the quality level.
    currentFrameData.shadowParams = glm::vec4(shadowsEnabled ? 1.0f : 0.0f, shadowStrength,
                                              1.0f / static_cast<float>(SHADOW_MAP_SIZE), indoorFactor_);

    // Whether this frame builds the fog volume. Decided here, beside the
    // switch in the block the shaders read, and the frame graph dispatches by
    // the same flag - so no shader reads a volume its frame did not build.
    volumetricThisFrame_ = volumetricFog_ && volumetricFog_->isOn() &&
                           volumetricFogDensity_ > 0.0f &&
                           !(passAblation_ && passAblation_->skip(AblationPass::VolumetricFog));
    currentFrameData.volumetricParams = volumetricThisFrame_ ? volumetricFog_->frameParams()
                                                             : glm::vec4(0.0f);

    for (uint32_t i = 0; i < MAX_LOCAL_LIGHTS; ++i) {
        currentFrameData.localLightPosRadius[i] = glm::vec4(0.0f);
        currentFrameData.localLightColorIntensity[i] = glm::vec4(0.0f);
    }
    // With the point-light enhancement off, only the M2 lights are gathered -
    // for the units, which stock lights by the four nearest of them
    // (character.frag) - and the lava lights, which stock has nowhere, are not.
    uint32_t localLightCount = (wmoRenderer && enh_.localLights)
        ? wmoRenderer->gatherLavaLights(camera->getPosition(),
              currentFrameData.localLightPosRadius,
              currentFrameData.localLightColorIntensity,
              MAX_LOCAL_LIGHTS)
        : 0;
    if (m2Renderer && !enh_.localLights) {
        localLightCount = m2Renderer->gatherStockPointLights(camera->getPosition(),
            currentFrameData.localLightPosRadius, currentFrameData.localLightColorIntensity,
            MAX_LOCAL_LIGHTS);
    } else if (m2Renderer && localLightCount < MAX_LOCAL_LIGHTS) {
        localLightCount += m2Renderer->gatherLocalLights(camera->getPosition(),
            currentFrameData.localLightPosRadius + localLightCount,
            currentFrameData.localLightColorIntensity + localLightCount,
            MAX_LOCAL_LIGHTS - localLightCount);
    }
    // y carries the stock fog curve's exponent as float bits
    // (stock_fog.glsli): 1 on Azeroth and Kalimdor, 1.5-7 on maps from 530
    // on, where the stock fog runs to the far clip. The free slot is here
    // because every .w of the float vectors already carries something.
    const float fogExponent =
        lightingManager ? lightingManager->getLightingParams().fogExponent : 1.0f;
    // z: the enhancement switches the world shaders branch on (enh_bits).
    // The HDR scene's lifted light clamp goes with the target actually being
    // in RGBA16F, not with the row alone (it stands down under FSR).
    const uint32_t hdrLightBit = vkCtx && vkCtx->isHdrScene() ? enh_bits::kHdrLight : 0u;
    // Enhanced's ambient goes with its daylight actually lighting the frame
    // (maps 0 and 1, above water), not with the row alone.
    const uint32_t enhancedAmbientBit =
        lightingManager && lightingManager->isEnhancedActive() && enhanced_palette::ambientRampOn()
            ? enh_bits::kEnhancedAmbient
            : 0u;
    currentFrameData.localLightMeta = glm::ivec4(
        static_cast<int32_t>(localLightCount), std::bit_cast<int32_t>(fogExponent),
        static_cast<int32_t>(shaderBits(enh_) | hdrLightBit | enhancedAmbientBit), 0);
    // w: the sun sprite's direction and the day weight, for the light through
    // leaves and grass (leaf_light.hpp); the light direction is not the sun.
    if (lightingManager)
        currentFrameData.localLightMeta.w = std::bit_cast<int32_t>(
            leaf_light::packSun(lightingManager->getLightingParams().sunDir));
    buildLocalLightTiles(localLightCount);

    if (rtLighting_) {
        const RtLighting::ConsumerData rt = rtLighting_->consumerData();
        currentFrameData.rtViewProj = rt.viewProj;
        currentFrameData.rtCameraPos = rt.cameraPos;
        currentFrameData.rtParams = rt.params;
    }

    // What the local lights are actually doing, throttled to a line every few
    // seconds. These are gathered around the camera rather than the player, so
    // which ones are in the set changes as the view is orbited - and they are
    // the warm ones: braziers, torches, forges, lava. A warm cast that moves
    // with the camera and nothing else would look exactly like this, so it is
    // worth being able to read the count rather than reason about it.
    {
        static double lastLightLog = 0.0;
        if (localLightCount > 0 && (globalTime - lastLightLog) > 3.0) {
            lastLightLog = globalTime;
            const glm::vec4& c0 = currentFrameData.localLightColorIntensity[0];
            const glm::vec4& p0 = currentFrameData.localLightPosRadius[0];
            LOG_INFO("localLights: count=", localLightCount, " of ", MAX_LOCAL_LIGHTS,
                     " first rgb=(", c0.r, ",", c0.g, ",", c0.b, ") intensity=", c0.w,
                     " radius=", p0.w);
        }
    }

    // Player motion, consumed by water ripples and by the foliage brush in
    // m2.vert. Horizontal speed only: a fall shouldn't read as running.
    {
        const float dt = std::max(lastDeltaTime_, 0.0f);
        if (!playerMotionTracked_) {
            prevPlayerPos_ = characterPosition;
            playerWakePos_ = characterPosition;
            playerMotionTracked_ = true;
        }
        if (dt > 0.0f) {
            const glm::vec2 step(characterPosition.x - prevPlayerPos_.x,
                                 characterPosition.y - prevPlayerPos_.y);
            // A teleport is not a sprint. Anything past a gallop is discarded
            // rather than smoothed, or one map change flattens a whole field.
            constexpr float MAX_TRACKED_SPEED = 30.0f;
            const float rawSpeed = glm::length(step) / dt;
            playerSpeed_ = (rawSpeed > MAX_TRACKED_SPEED) ? 0.0f
                                                          : glm::mix(playerSpeed_, rawSpeed, 0.25f);

            // Exponential chase, framerate independent.
            constexpr float WAKE_TIME_CONSTANT = 0.30f;  // seconds of springback
            const float k = 1.0f - std::exp(-dt / WAKE_TIME_CONSTANT);
            playerWakePos_ += (characterPosition - playerWakePos_) * k;
            if (rawSpeed > MAX_TRACKED_SPEED) playerWakePos_ = characterPosition;
        }
        prevPlayerPos_ = characterPosition;
    }
    currentFrameData.playerPos = glm::vec4(characterPosition, playerSpeed_);

    // One wind for everything that sways: the trees, the grass, the banners
    // and the awnings each had their own idea of it, so a storm left the
    // canopy as calm as a clear noon. The setting scales it, the weather
    // stirs it - rain a little, a storm by how hard it is blowing, snow
    // barely - and indoors the foliage outside the window stands still.
    // Eased over about four seconds, so a squall arriving or a step through
    // a door is a lull or a gathering rather than a switch.
    //
    // Cloth keeps the weather but not the indoor cut: a banner in a hall
    // always breathed, and freezing it whenever the player walked in would be
    // a different hall. playerWake.w carries the foliage number and
    // fogColor.w, which no shader read, the cloth one.
    {
        float weatherWind = 1.0f;
        if (weather && weather->isEnabled()) {
            const float wi = glm::clamp(weather->getIntensity(), 0.0f, 1.0f);
            switch (weather->getWeatherType()) {
                case Weather::Type::RAIN:  weatherWind = 1.3f; break;
                case Weather::Type::SNOW:  weatherWind = 1.1f; break;
                case Weather::Type::STORM: weatherWind = std::max(1.3f, 1.0f + 0.8f * wi); break;
                // A sandstorm blows hard with its enhancement row.
                case Weather::Type::SAND:  if (enh_.sandstorms) weatherWind = 1.2f + 0.6f * wi; break;
                default: break;
            }
        }
        const float clothTarget = foliageWindSetting_ * weatherWind;
        const float foliageTarget = clothTarget * (1.0f - glm::clamp(indoorFactor_, 0.0f, 1.0f));
        constexpr float WIND_TIME_CONSTANT = 4.0f;  // seconds
        const float k = 1.0f - std::exp(-std::max(lastDeltaTime_, 0.0f) / WIND_TIME_CONSTANT);
        windStrength_ += (foliageTarget - windStrength_) * k;
        clothWindStrength_ += (clothTarget - clothWindStrength_) * k;
        // The M2 shadow pass has no per-frame buffer and is told directly;
        // WMO banners read fogColor.w from the per-frame buffer like m2.vert.
        if (m2Renderer) m2Renderer->setWindStrength(windStrength_, clothWindStrength_);
    }
    currentFrameData.playerWake = glm::vec4(playerWakePos_, windStrength_);
    currentFrameData.fogColor.w = clothWindStrength_;

    // Wet ground in rain, eased the same way as the wind but slower: about
    // 12 s to soak and 18 s to dry (WetGround). Read from the weather's type
    // and intensity rather than isEnabled, which goes false whenever the
    // player steps into a building: the street outside stays wet while they
    // are in the inn. Interiors themselves never take it - wmo.frag applies
    // it to exterior groups only. Carried in rtParams.w, which the ray traced
    // lighting leaves at 0; the reflection pass and the character preview
    // zero the whole vector, so neither shows it.
    {
        float target = 0.0f;
        if (weather) {
            const Weather::Type type = weather->getWeatherType();
            target = wetGroundTarget(type == Weather::Type::RAIN, type == Weather::Type::STORM,
                                     weather->getIntensity());
        }
        wetness_ = wetGroundEnabled_ ? easeWetGround(wetness_, target, lastDeltaTime_) : 0.0f;
    }
    currentFrameData.rtParams.w = wetness_;

    // Rain ripples (the "Rain ripples" setting): how hard the drops fall for
    // the rings, from the same weather as the wet ground but eased over a
    // second or two - rings start with the rain, not with the soaking, and
    // stop soon after it. rtParams.z, which the ray traced lighting leaves
    // at 0; and the water's push. 0 with the setting off.
    {
        float target = 0.0f;
        if (weather) {
            const Weather::Type type = weather->getWeatherType();
            target = rainRippleTarget(type == Weather::Type::RAIN, type == Weather::Type::STORM,
                                      weather->getIntensity());
        }
        rainRipple_ = enh_.rainRipples ? easeRainRipple(rainRipple_, target, lastDeltaTime_) : 0.0f;
    }
    currentFrameData.rtParams.z = rainRipple_;
    if (waterRenderer) waterRenderer->setRainRipple(rainRipple_);

    // Fade in new scenery: the clock the streamed things are stamped with.
    if (enh_.streamFadeIn) {
        // WOWEE_STREAM_FADE_SECONDS=<s> stretches the dissolve for a look at
        // it (a screenshot of a 0.4 s fade is mostly luck).
        static const float kClockScale = [] {
            const char* v = std::getenv("WOWEE_STREAM_FADE_SECONDS");
            const float s = v ? static_cast<float>(std::atof(v)) : 0.0f;
            return s > 0.0f ? stream_fade::kSeconds / s : 1.0f;
        }();
        streamFadeClock_ += kClockScale * std::clamp(lastDeltaTime_, 0.0f, 0.1f);
        const float now = streamFadeClock_;
        if (m2Renderer) m2Renderer->setStreamFadeClock(now);
        if (wmoRenderer) wmoRenderer->setStreamFadeClock(now);
        if (terrainRenderer) terrainRenderer->setStreamFadeClock(now);
    }
    // The water renderer is built with the world, after the settings are
    // applied, so it is told every frame rather than once.
    if (waterRenderer) waterRenderer->setShorelineFoam(shorelineFoam_);
    if (waterRenderer) {
        waterRenderer->setHdWater(enh_.hdWater || enh_.enhancedWater);  // E34
        waterRenderer->setEnhancedWater(enh_.enhancedWater);
    }
    // Sunlight under water (E15): by day only, from the sun's height on the
    // stock clock (the light's own direction stays up all night).
    if (waterRenderer) {
        waterRenderer->setCausticsEnabled(enh_.caustics);
        float caustics = 0.0f;
        if (enh_.caustics && lightingManager) {
            caustics = water_caustics::dayStrength(lightingManager->getLightingParams().sunDir.z);
            static const bool kCausticsDebug = core::envFlagEnabled("WOWEE_CAUSTICS_DEBUG", false);
            if (kCausticsDebug) {
                static int n = 0;
                if (n++ % 300 == 0) LOG_INFO("caustics debug: sunDir.z ", lightingManager->getLightingParams().sunDir.z,
                                             " strength ", caustics, " tod ", lightingManager->getTimeOfDay());
                caustics = 2.0f;
            }
        }
        waterRenderer->setCaustics(caustics);
    }

    // Water ripple gate: swimming and actually moving.
    if (cameraController) {
        bool inWater = cameraController->isSwimming();
        bool moving = cameraController->isMoving();
        currentFrameData.fogParams.w = (inWater && moving) ? 1.0f : 0.0f;
    } else {
        currentFrameData.fogParams.w = 0.0f;
    }

    // Copy to current frame's mapped UBO
    uint32_t frame = vkCtx->getCurrentFrame();
    std::memcpy(perFrameUBOMapped[frame], &currentFrameData, sizeof(GPUPerFrameData));
}

bool Renderer::initialize(core::Window* win) {
    window = win;
    vkCtx = win->getVkContext();
    deferredWorldInitEnabled_ = core::envFlagEnabled("WOWEE_DEFER_WORLD_SYSTEMS", true);
    LOG_INFO("Initializing renderer (Vulkan)");

    // Create camera (in front of Stormwind gate, looking north)
    camera = std::make_unique<Camera>();
    camera->setPosition(glm::vec3(-8900.0f, -170.0f, 150.0f));
    camera->setRotation(0.0f, -5.0f);
    camera->setAspectRatio(window->getAspectRatio());
    camera->setFov(60.0f);

    // Create camera controller
    cameraController = std::make_unique<CameraController>(camera.get());
    cameraController->setUseWoWSpeed(true);  // Use realistic WoW movement speed

    // Create performance HUD
    performanceHUD = std::make_unique<PerformanceHUD>();
    performanceHUD->setPosition(PerformanceHUD::Position::TOP_LEFT);

    // What the player last chose for shadow quality, before the resources that
    // bake it in are built. Read from the CVar file rather than waited for:
    // the interface that would normally hand it over does not load until well
    // after this, and by then the shadow map exists at whatever size it was
    // given here. See Renderer::SHADOW_MAP_SIZE.
    {
        // The top two levels are the same size on purpose, and it is worth
        // saying why before someone raises the last one to 8192 as an easy win
        // for modern hardware. Each level doubles the side, so it quadruples
        // the image: at 4096 a depth map is 64 MB and there are two of them,
        // one per frame in flight, which is 128 MB. 8192 would be 512 MB of
        // VRAM for shadows alone - not a step a machine that can run this is
        // certain to have spare, and the allocation failing at start-up is not
        // a path this renderer handles gently. setShadowMapSize clamps to 4096
        // for the same reason.
        constexpr uint32_t kShadowSideForLevel[] = {512, 1024, 2048, 4096, 4096};
        // 4096 was the default, and the arithmetic above says what that costs:
        // 64 MB a map, two of them in flight, 128 MB of depth before anything
        // is drawn, refilled every frame. That is a lot to ask of a machine
        // nobody checked, and a good part of the reports of this client running
        // badly are hardware that was never going to carry it. 2048 is a
        // quarter of the fill and 32 MB for the pair, and the slider still
        // reaches the top for anyone who wants to spend it.
        //
        // A phone starts lower again: its GPU memory is the system's memory.
#ifdef __ANDROID__
        constexpr const char* kDefaultShadowLevel = "1";   // 1024, 8 MB the pair
#else
        constexpr const char* kDefaultShadowLevel = "2";   // 2048, 32 MB the pair
#endif
        const int level = std::clamp(
            std::atoi(addons::storedCVarValue("extShadowQuality", kDefaultShadowLevel).c_str()),
            0, 4);
        setShadowMapSize(kShadowSideForLevel[level]);
        // The world map is kept and drawn again only on a redraw (walked 5%
        // of the distance, the sun turned, the world changed), a part a
        // frame, so its side is twice the level's, up to 4096: at the 2048
        // level over the 500 yd distance its texel was half a yard, and every
        // building and tree shadow was a smear of them. The one-map path
        // (WOWEE_SHADOW_SPLIT=0) draws it every frame and keeps the level's.
        // WOWEE_SHADOW_WORLD_SIDE_LEVEL=1 keeps the level's side for the split
        // too (the A/B for this, 3.15).
        if (core::envFlagEnabled("WOWEE_SHADOW_SPLIT", true) &&
            !core::envFlagEnabled("WOWEE_SHADOW_WORLD_SIDE_LEVEL", false)) {
            SHADOW_MAP_SIZE = std::min(4096u, shadowLevelSide_ * 2u);
        }
        LOG_INFO("Shadow maps: world ", SHADOW_MAP_SIZE, ", unit ", std::max(512u, shadowLevelSide_ / 2u),
                 " (quality level ", level, ")");
    }

    // Create per-frame UBO and descriptor sets
    if (!createPerFrameResources()) {
        LOG_ERROR("Failed to create per-frame Vulkan resources");
        return false;
    }

    // Initialize Vulkan sub-renderers (Phase 3)

    // Sky system (owns skybox, starfield, celestial, clouds, lens flare)
    skySystem = std::make_unique<SkySystem>();
    if (!skySystem->initialize(vkCtx, perFrameSetLayout)) {
        LOG_ERROR("Failed to initialize sky system");
        return false;
    }
    // Expose sub-components via renderer accessors
    skybox = nullptr;  // Owned by skySystem; access via skySystem->getSkybox()
    celestial = nullptr;
    starField = nullptr;
    clouds = nullptr;
    lensFlare = nullptr;

    weather = std::make_unique<Weather>();
    if (!weather->initialize(vkCtx, perFrameSetLayout))
        LOG_WARNING("Weather effect initialization failed (non-fatal)");

    lightning = std::make_unique<Lightning>();
    if (!lightning->initialize(vkCtx, perFrameSetLayout))
        LOG_WARNING("Lightning effect initialization failed (non-fatal)");

    swimEffects = std::make_unique<SwimEffects>();
    syncSwimEffectsTargetPass();
    if (!swimEffects->initialize(vkCtx, perFrameSetLayout))
        LOG_WARNING("Swim effect initialization failed (non-fatal)");

    // Mount dust is an enhancement: made the first time it is switched on
    // (update), so a stock session never creates its pipeline or buffer.


    levelUpEffect = std::make_unique<LevelUpEffect>();
    lootSparkles_ = std::make_unique<LootSparkles>();

    // Non-fatal like the effects above: a device that cannot build the compute
    // pipeline still gets everything else, and isReady() gates both call sites.
    grassRenderer_ = std::make_unique<GrassRenderer>();
    if (!grassRenderer_->initialize(vkCtx, perFrameSetLayout))
        LOG_WARNING("Grass renderer initialization failed (non-fatal)");
    // The distance setting may have been applied before this existed.
    grassRenderer_->setCullDistance(grassDistance_);

    questMarkerRenderer = std::make_unique<QuestMarkerRenderer>();
    footprintRenderer = std::make_unique<FootprintRenderer>();

    LOG_INFO("Vulkan sub-renderers initialized (Phase 3)");

    // LightingManager doesn't use GL - initialize for data-only use
    lightingManager = std::make_unique<LightingManager>();
    auto* assetManager = core::Application::getInstance().getAssetManager();

    // Create zone manager; enrich music paths from DBC if available
    zoneManager = std::make_unique<game::ZoneManager>();
    zoneManager->initialize();
    if (assetManager) {
        zoneManager->enrichFromDBC(assetManager);
    }

    // Audio is now owned by AudioCoordinator (created by Application).
    // Renderer receives AudioCoordinator* via setAudioCoordinator().

    // Create secondary command buffer resources for multithreaded rendering.
    //
    // WOWEE_SINGLE_THREAD_RECORD takes the fallback path instead. That path
    // records the world inline on one thread, which costs CPU time but marks
    // every pass separately - grass included, where the parallel path records
    // grass into the terrain secondary and so reports the two as one number.
    // Running a frame profile both ways says how much of "terrain" is grass,
    // and whether serialising the recording moves the frame time at all.
    if (std::getenv("WOWEE_PASS_ABLATION") != nullptr) {
        passAblation_ = std::make_unique<PassAblation>();
        LOG_WARNING("Pass ablation enabled - ", PassAblation::phaseCount(),
                    " phases over about ",
                    static_cast<int>(passAblation_->expectedRunMs() / 1000.0),
                    "s of being in the world, the first 10s of it settling. Stand "
                    "still outdoors, do not move the camera, and do not quit before "
                    "the table is logged.");
    }

    static const bool forceSingleThread = std::getenv("WOWEE_SINGLE_THREAD_RECORD") != nullptr;
    if (forceSingleThread) {
        LOG_INFO("WOWEE_SINGLE_THREAD_RECORD set - inline recording, one pass per GPU mark");
    } else if (!createSecondaryCommandResources()) {
        LOG_WARNING("Failed to create secondary command buffers - falling back to single-threaded rendering");
    }

    // Create PostProcessPipeline (§4.3 - owns FSR/FXAA/FSR2/FSR3/brightness)
    postProcessPipeline_ = std::make_unique<PostProcessPipeline>();
    postProcessPipeline_->initialize(vkCtx);
    // The ray traced lighting normally records where the water leaves the
    // scene pass. A multisampled scene in an off-screen target never leaves
    // it early, so the pass records here instead, as the upscaler takes over.
    postProcessPipeline_->setSceneClosedHook([this](VkCommandBuffer cmd) {
        markWorldPassClosed();
        // The water drew inside this off-screen scene (MSAA, no late
        // particle pass): its depth copy for the next frame comes from this
        // scene's own depth, here where it is finished. endFrame's copy is of
        // the swapchain path's depth, which an off-screen scene never wrote -
        // the shoreline foam and the caustics read a stale image from it.
        if (postProcessPipeline_ && !waterDrawsInContinuePass() && waterRenderer &&
            waterRenderer->isRefractionEnabled() && waterRenderer->hasSurfaces() &&
            postProcessPipeline_->getSceneDepthImage() != VK_NULL_HANDLE) {
            waterRenderer->captureSceneHistory(cmd, VK_NULL_HANDLE,
                                               postProcessPipeline_->getSceneDepthImage(),
                                               postProcessPipeline_->getSceneRenderExtent(),
                                               postProcessPipeline_->sceneDepthIsMsaa(),
                                               vkCtx->getCurrentFrame());
            vkCtx->gpuMark(cmd, "water refraction copy");
        }
        recordContactShadowPass(cmd);
        recordSoftParticlePass(cmd);
        if (rtRecordedThisFrame_ || !postProcessPipeline_) return;
        recordRtLighting(postProcessPipeline_->getSceneDepthImage(),
                         postProcessPipeline_->getSceneRenderExtent(),
                         postProcessPipeline_->sceneDepthIsMsaa());
    });

    // Not fatal: without them the picture is only missing its rays.
    sunShafts_ = std::make_unique<SunShafts>();
    if (!sunShafts_->initialize(vkCtx)) {
        LOG_WARNING("Sun shafts failed to initialise - sun shafts unavailable");
        sunShafts_->shutdown();
        sunShafts_.reset();
    }

    // Create render graph and register virtual resources
    renderGraph_ = std::make_unique<RenderGraph>();
    if (stageSink_) renderGraph_->setStageSink(stageSink_);

    // Create overlay system (selection circle + fullscreen overlay)
    overlaySystem_ = std::make_unique<OverlaySystem>(vkCtx);
    renderGraph_->registerResource("shadow_depth");
    renderGraph_->registerResource("volumetric_fog");
    renderGraph_->registerResource("reflection_texture");
    renderGraph_->registerResource("scene_color");
    renderGraph_->registerResource("scene_depth");
    renderGraph_->registerResource("final_image");

    LOG_INFO("Renderer initialized");
    return true;
}

void Renderer::shutdown() {
    // A recording in progress is finished, not abandoned: the file is only
    // playable once its index is written.
    if (recorder_) stopRecording();

    destroySecondaryCommandResources();

    LOG_DEBUG("Renderer::shutdown - terrainManager stopWorkers...");
    if (terrainManager) {
        terrainManager->stopWorkers();
        LOG_DEBUG("Renderer::shutdown - terrainManager reset...");
        terrainManager.reset();
    }

    LOG_DEBUG("Renderer::shutdown - terrainRenderer...");
    if (terrainRenderer) {
        terrainRenderer->shutdown();
        terrainRenderer.reset();
    }

    LOG_DEBUG("Renderer::shutdown - waterRenderer...");
    if (waterRenderer) {
        waterRenderer->shutdown();
        waterRenderer.reset();
    }

    LOG_DEBUG("Renderer::shutdown - minimap...");
    if (minimap) {
        minimap->shutdown();
        minimap.reset();
    }

    LOG_DEBUG("Renderer::shutdown - worldMap...");
    if (worldMap) {
        worldMap->shutdown();
        worldMap.reset();
    }

    if (sunShafts_) {
        sunShafts_->shutdown();
        sunShafts_.reset();
    }
    softParticlePass_.reset();
    // The ambient pass reads the bounce grid's set: the pass goes first.
    ambientOcclusion_.pass.reset();
    bounceGrid_.reset();
    // The water continuation's views and framebuffers (the device is idle:
    // the water renderer's shutdown above waited for it).
    destroySceneWaterRetired(0);
    destroySceneWaterRetired(1);

    LOG_DEBUG("Renderer::shutdown - skySystem...");
    if (skySystem) {
        skySystem->shutdown();
        skySystem.reset();
    }

    // Individual sky components are owned by skySystem; just null the aliases
    skybox = nullptr;
    celestial = nullptr;
    starField = nullptr;
    clouds = nullptr;
    lensFlare = nullptr;

    if (weather) {
        weather.reset();
    }

    if (lightning) {
        lightning->shutdown();
        lightning.reset();
    }

    if (swimEffects) {
        swimEffects->shutdown();
        swimEffects.reset();
    }

    if (footprintRenderer) {
        footprintRenderer->shutdown();
        footprintRenderer.reset();
    }

    LOG_DEBUG("Renderer::shutdown - characterRenderer...");
    if (characterRenderer) {
        characterRenderer->shutdown();
        characterRenderer.reset();
    }

    // Shutdown AnimationController before renderers it references (§4.2)
    animationController_.reset();

    LOG_DEBUG("Renderer::shutdown - wmoRenderer...");
    if (wmoRenderer) {
        wmoRenderer->shutdown();
        wmoRenderer.reset();
    }

    // Shutdown SpellVisualSystem before M2Renderer (it holds M2Renderer pointer) (§4.4)
    if (spellVisualSystem_) {
        spellVisualSystem_->shutdown();
        spellVisualSystem_.reset();
    }

    if (grassRenderer_) {
        // The whole session in one line, at the end where the bounded log
        // cannot rotate it away before anyone reads it.
        if (grassRebuilds_ > 0) {
            LOG_INFO("Grass session: ", grassRebuilds_, " rebuilds, last population ",
                     grassLastCount_, " blades, ", grassProfiles_.size(),
                     " profiles derived, worst generate ",
                     static_cast<int>(grassWorstGenerateMs_), "ms");
        }
        grassRenderer_->shutdown();
        grassRenderer_.reset();
    }

    LOG_DEBUG("Renderer::shutdown - m2Renderer...");
    if (hizSystem_) {
        hizSystem_->shutdown();
        hizSystem_.reset();
    }
    if (m2Renderer) {
        m2Renderer->shutdown();
        m2Renderer.reset();
    }
    if (skyboxModelRenderer_) {
        skyboxModelRenderer_->shutdown();
        skyboxModelRenderer_.reset();
        skyLayers_.clear();
        loadedSkyModels_.clear();
    }
    if (skyStarsRenderer_) {
        skyStarsRenderer_->shutdown();
        skyStarsRenderer_.reset();
    }

    // Audio shutdown is handled by AudioCoordinator (owned by Application).
    audioCoordinator_ = nullptr;

    // Cleanup selection circle + overlay resources
    if (overlaySystem_) {
        overlaySystem_->cleanup();
        overlaySystem_.reset();
    }

    // Shutdown post-process pipeline (FSR/FXAA/FSR2 resources) (§4.3)
    if (postProcessPipeline_) {
        postProcessPipeline_->shutdown();
        postProcessPipeline_.reset();
    }

    // Destroy render graph
    renderGraph_.reset();

    destroyPerFrameResources();

    zoneManager.reset();

    performanceHUD.reset();
    cameraController.reset();
    camera.reset();

    LOG_INFO("Renderer shutdown");
}

void Renderer::registerPreview(CharacterPreview* preview) {
    if (!preview) return;
    auto it = std::find(activePreviews_.begin(), activePreviews_.end(), preview);
    if (it == activePreviews_.end()) {
        activePreviews_.push_back(preview);
    }
}

void Renderer::unregisterPreview(CharacterPreview* preview) {
    auto it = std::find(activePreviews_.begin(), activePreviews_.end(), preview);
    if (it != activePreviews_.end()) {
        activePreviews_.erase(it);
    }
}

void Renderer::buildLocalLightTiles(uint32_t count) {
    // Which of the gathered lights can reach each tile of the screen; see
    // rendering/light_tiles.hpp. Only for the enhanced lights: the stock rule
    // (the four nearest, no range) has to see them all.
    auto& fd = currentFrameData;
    fd.localLightTileInfo = glm::ivec4(0);
    // z: the angle one screen pixel spans, which the sun's shadow filter reads
    // for a pixel's footprint on the map (shadow_filter.glsli) whether or not
    // the tiles are built.
    const VkExtent2D pixExt = sceneRenderExtent();
    const float pixelAngle = pixExt.height > 0 && std::abs(fd.projection[1][1]) > 1e-6f
        ? 2.0f / (std::abs(fd.projection[1][1]) * static_cast<float>(pixExt.height)) : 0.0f;
    // w: the "Snow settles" cover (renderer_weather_fx.cpp), 0 without it.
    fd.localLightTileScale = glm::vec4(0.0f, 0.0f, pixelAngle, snowCover_);
    // WOWEE_LIGHT_TILES=0 walks every light per pixel, as before the grid -
    // for comparing pictures and timings against it.
    static const bool tilesOn = core::envFlagEnabled("WOWEE_LIGHT_TILES", true);
    if (!tilesOn || !enh_.localLights || count == 0 || !camera) return;
    const VkExtent2D ext = sceneRenderExtent();
    if (ext.width == 0 || ext.height == 0) return;
    // WOWEE_LIGHT_TILE_MARGIN_TILE=1 grows each light by a whole tile and
    // bounds it by its box, as before enhancements-plan.md 3.12.
    static const bool oldMargin = core::envFlagEnabled("WOWEE_LIGHT_TILE_MARGIN_TILE", false);
    if (oldMargin) {
        cullLightsToTiles(fd.projection * fd.view, fd.localLightPosRadius, count, fd.localLightTiles);
    } else {
        cullLightsToTiles(fd.view, fd.projection, fd.localLightPosRadius, count, fd.localLightTiles,
                          lightTileMarginPixels(2.0f, static_cast<float>(ext.width),
                                                static_cast<float>(ext.height)));
    }
    fd.localLightTileInfo = glm::ivec4(static_cast<int>(LIGHT_TILES_X), static_cast<int>(LIGHT_TILES_Y), 1, 0);
    fd.localLightTileScale = glm::vec4(static_cast<float>(LIGHT_TILES_X) / static_cast<float>(ext.width),
                                       static_cast<float>(LIGHT_TILES_Y) / static_cast<float>(ext.height),
                                       pixelAngle, snowCover_);
}

void Renderer::updateFootfallDust(float fxDt) {
    // Not stock: nothing is made, queued or spawned with the row off, and what
    // was in the air is dropped. Made on first use and released a few frames
    // after it goes off, once no frame in flight can still draw from it.
    if (!enh_.footfallDust) {
        pendingFootfalls_.clear();
        if (!lastFootfall_.empty()) lastFootfall_.clear();
        if (footfallDust_) {
            footfallDust_->clear();
            if (++footfallDustIdleFrames_ > MAX_FRAMES + 1) footfallDust_.reset();
        }
        return;
    }
    footfallDustIdleFrames_ = 0;
    if (!footfallDust_ && !footfallDustFailed_ && vkCtx) {
        footfallDust_ = std::make_unique<FootfallDust>();
        if (!footfallDust_->initialize(vkCtx, perFrameSetLayout)) {
            LOG_WARNING("Footfall dust initialization failed (non-fatal)");
            footfallDust_.reset();
            footfallDustFailed_ = true;
        }
    }
    footfallClock_ += fxDt;
    if (!footfallDust_ || !camera) {
        pendingFootfalls_.clear();
        return;
    }

    // Rain and snow lay the dust: none while either falls.
    bool wet = false;
    if (weather) {
        const Weather::Type wt = weather->getWeatherType();
        wet = wt != Weather::Type::NONE && wt != Weather::Type::SAND && weather->getIntensity() > 0.05f;
    }
    namespace fd = footfall_dust;
    const glm::vec3 camPos = camera->getPosition();
    for (const PendingFootfall& f : pendingFootfalls_) {
        auto [it, fresh] = lastFootfall_.try_emplace(f.instanceId);
        LastFootfall& last = it->second;
        const float speed = fresh ? 0.0f
            : fd::stepSpeed(last.position, last.time, f.position, footfallClock_);
        const glm::vec3 prevPos = fresh ? f.position : last.position;
        last.position = f.position;
        last.time = footfallClock_;
        if (wet || speed <= 0.0f || glm::distance(f.position, camPos) > fd::kMaxDistance) continue;

        // The ground as the footstep sound reads it, asked again only once the
        // walker has gone a couple of yards from where it last was.
        if (fresh || glm::distance(last.groundAt, f.position) > 2.0f) {
            const glm::vec3& p = f.position;
            bool inWater = false, onFloor = false;
            if (waterRenderer) {
                if (auto h = waterRenderer->getWaterHeightAt(p.x, p.y); h && p.z < *h + 0.25f) inWater = true;
            }
            const auto ground = terrainManager ? terrainManager->getHeightAt(p.x, p.y) : std::nullopt;
            if (!inWater && wmoRenderer) {
                const auto floor = wmoRenderer->getFloorHeight(p.x, p.y, p.z + 1.5f);
                onFloor = floor && std::abs(*floor - p.z) <= 2.5f && (!ground || *floor >= *ground - 0.1f);
            }
            std::optional<uint32_t> soundType;
            std::string texture;
            if (!inWater && !onFloor && terrainManager) {
                soundType = terrainManager->getTerrainSoundTypeAt(p.x, p.y);
                if (!soundType) {
                    if (auto t = terrainManager->getDominantTextureAt(p.x, p.y)) texture = *t;
                }
            }
            last.ground = static_cast<uint8_t>(fd::groundFor(soundType, texture, inWater, onFloor));
            last.groundAt = f.position;
        }
        const auto ground = static_cast<fd::Ground>(last.ground);
        // WOWEE_FOOTFALL_DUST_LOG=1: each footfall near the camera, its speed
        // and ground, and how many puffs are in the air - for checking which
        // grounds raise dust and how much.
        static const bool logDust = [] {
            const char* v = std::getenv("WOWEE_FOOTFALL_DUST_LOG");
            return v && *v && *v != '0';
        }();
        if (logDust) {
            LOG_WARNING("Footfall dust: instance ", f.instanceId, " at (", f.position.x, ", ", f.position.y,
                        ") speed ", speed, " ground ", static_cast<int>(last.ground), " sprites ",
                        footfallDust_->spriteCount());
        }
        if (ground == fd::Ground::None) continue;

        float radius = 1.0f;
        glm::vec3 centre;
        if (characterRenderer) (void)characterRenderer->getInstanceBounds(f.instanceId, centre, radius);
        std::uniform_real_distribution<float> roll(0.0f, 1.0f);
        static std::mt19937 rng{0xd057u};
        const fd::Puff puff = fd::puffFor(speed, radius, roll(rng));
        if (puff.sprites == 0) continue;
        glm::vec2 dir(f.position.x - prevPos.x, f.position.y - prevPos.y);
        const float len = glm::length(dir);
        dir = len > 1e-3f ? dir / len : glm::vec2(0.0f);
        footfallDust_->spawn(f.position, dir, puff, fd::colourFor(ground));
    }
    pendingFootfalls_.clear();
    // Walkers long gone (out of range, despawned) are forgotten.
    if (lastFootfall_.size() > 256) {
        for (auto it = lastFootfall_.begin(); it != lastFootfall_.end();) {
            it = (footfallClock_ - it->second.time > 3.0f) ? lastFootfall_.erase(it) : std::next(it);
        }
    }
    footfallDust_->update(fxDt, *camera);
}

void Renderer::installCharacterAnimEventCallback() {
    if (!characterRenderer) return;
    const bool dust = enh_.footfallDust;
    if (!dust) {
        // The sounds' callback as it was, or none: with the row off the
        // character update fires exactly the events it fired before.
        characterRenderer->setAnimSoundEventCallback(characterAnimSoundEventCallback_);
        animEventCallbackHasDust_ = false;
        return;
    }
    characterRenderer->setAnimSoundEventCallback(
        [this](uint32_t instanceId, uint32_t animationId, uint32_t eventTag, uint32_t soundEntryId,
               const glm::vec3& position) {
            static constexpr uint32_t kFootfall = pipeline::m2EventTag("$FSD");
            // The local character and its mount come from the footstep
            // driver instead (noteLocalFootfall), which times their steps
            // even on a model without $FSD.
            const bool local = instanceId == characterInstanceId ||
                (animationController_ && instanceId == animationController_->getMountInstanceId());
            if (eventTag == kFootfall && enh_.footfallDust && !local && pendingFootfalls_.size() < 512) {
                pendingFootfalls_.push_back({instanceId, position});
            }
            if (characterAnimSoundEventCallback_) {
                characterAnimSoundEventCallback_(instanceId, animationId, eventTag, soundEntryId, position);
            }
        });
    animEventCallbackHasDust_ = true;
}

void Renderer::updateCorpseSettle(float fxDt) {
    if (!characterRenderer) return;
    if (!enh_.corpseSettle && !enh_.footPlacement) {
        // Free once nothing is tilted or corrected.
        characterRenderer->updateCorpseSettle(fxDt, nullptr);
        characterRenderer->updateFootPlacement(fxDt, nullptr, glm::vec3(0.0f));
        return;
    }
    // The floor under a point as the selection circle reads it: the highest
    // of terrain, building and doodad floors at most 1.5 yd above the body
    // and no more than 4 below it.
    const CharacterRenderer::FloorHeightFn floor = [this](float x, float y, float z) -> std::optional<float> {
        const float from = z + 1.5f;
        std::optional<float> best;
        auto consider = [&](std::optional<float> h) {
            if (h && *h <= from && *h >= z - 4.0f && (!best || *h > *best)) best = h;
        };
        if (terrainManager) consider(terrainManager->getHeightAt(x, y));
        if (wmoRenderer) consider(wmoRenderer->getFloorHeight(x, y, from));
        if (m2Renderer) consider(m2Renderer->getFloorHeight(x, y, from));
        return best;
    };
    characterRenderer->updateCorpseSettle(fxDt, enh_.corpseSettle ? &floor : nullptr);
    // E9 reads the same floors under the feet of standing and walking units.
    characterRenderer->updateFootPlacement(fxDt, enh_.footPlacement ? &floor : nullptr,
                                           camera ? camera->getPosition() : glm::vec3(0.0f));
}

void Renderer::readFrozenClock() {
    if (frozenClockRead_) return;
    frozenClockRead_ = true;
    if (const char* f = std::getenv("WOWEE_FREEZE_CLOCK"); f && *f) {
        frozenClock_ = std::max(0.0f, static_cast<float>(std::atof(f)));
        LOG_INFO("WOWEE_FREEZE_CLOCK: scene clock held at ", frozenClock_, " s");
    }
}

void Renderer::updateUnitBlobs() {
    // The stock blob under each unit (stock extShadowQuality 0). With the sun
    // shadow map on, the units the last shadow pass drew into a sun map have
    // their shadow there; every other unit - past the unit map's 64 nearest,
    // outside its range, or with the unit map off - keeps the blob, so no
    // unit is left without a shadow (docs/re/characters.md L10).
    if (!overlaySystem_) return;
    {
        std::vector<OverlaySystem::UnitBlob> blobs;
        if (characterRenderer && camera) {
            overlaySystem_->prepareUnitBlobs();
            std::vector<CharacterRenderer::BlobSource> src;
            // Stock has no distance cut-off; past 150 yards a blob is a few
            // pixels under the fog, and 128 is more units than a city holds
            // on screen. Only the blobs in view are taken: one outside it
            // draws nothing, and its floor samples were most of this pass's
            // main-thread time.
            Frustum view;
            view.extractFromMatrix(camera->getViewProjectionMatrix());
            characterRenderer->collectUnitBlobs(camera->getPosition(), 150.0f, 128, src,
                                                /*skipSunShadowCasters=*/shadowsEnabled, &view);
            blobs.reserve(src.size());
            for (const auto& b : src) {
                blobs.push_back({.feet = b.feet, .forward = b.forward, .halfX = b.halfX,
                                 .halfY = b.halfY, .halfZ = b.halfZ, .alpha = b.alpha});
            }
        }
        overlaySystem_->setUnitBlobs(std::move(blobs));
    }
}

void Renderer::setDrunkLevel(float level) {
    drunkLevel_ = glm::clamp(level, 0.0f, 1.0f);
    if (cameraController) cameraController->setDrunkLevel(drunkLevel_);
}

void Renderer::setEnhancements(const Enhancements& e) {
    if (e == enh_) return;
    enh_ = e;
    enhDirty_ = true;
}

void Renderer::applyEnhancementsToSubsystems() {
    // The subsystems come and go with the world, so the set is handed over
    // again whenever one of them is new as well as when the set changed.
    const void* targets[std::extent_v<decltype(enhTargets_)>] = {
        m2Renderer.get(), wmoRenderer.get(), characterRenderer.get(),
        terrainRenderer.get(), lensFlare.get(), lightingManager.get(),
        postProcessPipeline_.get(), cameraController.get(),
    };
    bool changed = enhDirty_;
    for (size_t i = 0; i < std::size(enhTargets_); ++i) {
        if (targets[i] != enhTargets_[i]) { changed = true; enhTargets_[i] = targets[i]; }
    }
    if (!changed) return;
    enhDirty_ = false;
    const Enhancements& e = enh_;

    // The footfalls for the dust ride the character events (see
    // installCharacterAnimEventCallback); re-installed as the row flips.
    if (characterRenderer && animEventCallbackHasDust_ != e.footfallDust) installCharacterAnimEventCallback();

    // Light and shadow. Off is the stock blob under each unit
    // (OverlaySystem::renderUnitBlobShadows) and the baked MCSH shadows.
    setShadowsEnabled(e.shadows);
    setShadowStrength(e.shadowStrength);
    setTerrainRelief(e.normalMapping, e.normalMapStrength);
    if (wmoRenderer) {
        wmoRenderer->setNormalMappingEnabled(e.normalMapping);
        wmoRenderer->setNormalMapStrength(e.normalMapStrength);
        wmoRenderer->setPOMEnabled(e.parallax);
        wmoRenderer->setPOMQuality(e.parallaxQuality);
    }
    if (characterRenderer) {
        characterRenderer->setNormalMappingEnabled(e.normalMapping);
        characterRenderer->setNormalMapStrength(e.normalMapStrength);
        characterRenderer->setPOMEnabled(e.parallax);
        characterRenderer->setPOMQuality(e.parallaxQuality);
        characterRenderer->setCharacterFidelity(e.characterFidelity);
    }
    if (terrainRenderer) terrainRenderer->setSpecularEnabled(e.terrainSpecular);
    // Unstretched cliffs (E16): the terrain's side-projection pipelines.
    if (terrainRenderer) terrainRenderer->setCliffProjection(e.cliffProjection);
    // Room light indoors (E12): the WMO probes and the doodads' light.
    if (wmoRenderer) wmoRenderer->setIndoorProbes(e.indoorLight);
    if (m2Renderer) {
        m2Renderer->setLightFlicker(e.lightFlicker);
        m2Renderer->setGlowSprites(e.glowSprites);
        m2Renderer->setExtraSmoke(e.extraSmoke);
    }

    // Distance. Soft particles are decided each frame (decideSoftParticles)
    // and the rain ripples eased each frame (updatePerFrameUBO), from enh_.
    if (m2Renderer) m2Renderer->setStreamFadeIn(e.streamFadeIn);
    if (wmoRenderer) wmoRenderer->setStreamFadeIn(e.streamFadeIn);
    if (terrainRenderer) terrainRenderer->setStreamFadeIn(e.streamFadeIn);

    // Sky and air.
    setVolumetricFogQuality(e.lightShafts);
    setVolumetricFogDensity(e.mistDensity);
    if (volumetricFog_) volumetricFog_->setLongReach(e.sunHaze);
    setSunShaftsEnabled(e.sunShafts);
    if (lensFlare) lensFlare->setIntensity(e.lensFlare);
    setSharpStars(e.sharpStars);
    // Soft highlights (E18): the shoulder on the glow composite.
    if (postProcessPipeline_) {
        postProcessPipeline_->setHighlightRollOff(e.highlightRollOff);
        postProcessPipeline_->setBloom(e.bloom);
    }
    if (lightingManager) {
        lightingManager->setFogStrength(e.fogStrength);
        lightingManager->setFogSkyBlend(e.fogSkyBlend);
        lightingManager->setEnhancedDaylight(e.enhancedDaylight);
        lightingManager->setLongView(e.longView);
        lightingManager->setLongViewReach(e.farField ? far_field::kReach : 0.0f);
    }

    // Water, weather and motion. The water and swim switches are handed on
    // each frame where those are used (update), as before.
    setShorelineFoam(e.shorelineFoam);
    setWetGround(e.wetGround);
    setWaterSplashes(e.waterSplashes);
    setWaterReflections(e.waterReflections);
    setFoliageWind(e.foliageWind);
    if (postProcessPipeline_) postProcessPipeline_->setDrunkWobble(e.drunkWobble);
    if (cameraController) cameraController->setDrunkWobble(e.drunkWobble);
    if (cameraController) cameraController->setSmoothZoom(e.smoothZoom);
    // Feet on the ground (E9) and the rain in the wind (E19) are read from
    // enh_ each frame (updateCorpseSettle, the weather update); the lit
    // horizon (E11) goes to the terrain's horizon.
    if (terrainRenderer) terrainRenderer->setHorizonLit(e.horizonDetail);
    // The far field (far_field.hpp): the coloured horizon, the buildings'
    // silhouettes past the view distance, and the tiles loaded for them.
    if (terrainRenderer) terrainRenderer->setHorizonFarField(e.farField);
    // WOWEE_FARFIELD_WMO_REACH=<yards> overrides the reach, for measuring.
    static const float kWmoReach = static_cast<float>(
        core::envIntClamped("WOWEE_FARFIELD_WMO_REACH", static_cast<int>(kFarFieldWmoReach), 0, 6000));
    if (wmoRenderer) wmoRenderer->setFarReach(e.farField ? kWmoReach : 0.0f);
    if (farField_ != e.farField) {
        farField_ = e.farField;
        setViewDistance(viewDistance_);
    }

    // Experimental.
    setGrassEnabled(e.grass);
    setGrassScales(static_cast<float>(e.grassDensity) / 100.0f,
                   static_cast<float>(e.grassHeight) / 100.0f);
    setGrassDistance(static_cast<float>(e.grassDistance));
    setRtLightingMode(e.rtLighting);
}
void Renderer::setMsaaSamples(VkSampleCountFlagBits samples) {
    if (!vkCtx) return;

    // Only a device that cannot resolve depth has to choose between the
    // temporal upscaler and multisampling; everywhere else both run. The
    // saved choice is kept either way and re-asserted when the upscaler goes
    // off, by the settings panel.
    if (postProcessPipeline_ && postProcessPipeline_->isFsr2BlockingMsaa() && samples > VK_SAMPLE_COUNT_1_BIT) {
        LOG_WARNING("Multisampling left off: the temporal upscaler is on and this device "
                    "cannot resolve depth");
        return;
    }

    // Clamp to device maximum
    VkSampleCountFlagBits maxSamples = vkCtx->getMaxUsableSampleCount();
    if (samples > maxSamples) samples = maxSamples;

    if (samples == vkCtx->getMsaaSamples()) return;

    // Defer to between frames - cannot destroy render pass/framebuffers mid-frame
    pendingMsaaSamples_ = samples;
    msaaChangePending_ = true;
}

// The "HDR lighting (the reference client)" row: the scene in RGBA16F. Only with the
// post-process pass as the way to the screen and neither upscaler (their
// scene targets are made in the swapchain's format), and given up for the
// session if its target could not be made. A change is queued on the MSAA
// rebuild, which already remakes every pass and pipeline the format is in.
void Renderer::decideHdrScene() {
    if (!vkCtx) return;
    bool want = enh_.hdrScene && !hdrSceneFailed_ && postProcessPipeline_ &&
                !postProcessPipeline_->isFSREnabled() && !postProcessPipeline_->isFSR2Enabled();
    if (want && !hdrSceneSupportChecked_) {
        hdrSceneSupportChecked_ = true;
        if (!vkCtx->isHdrSceneSupported()) {
            LOG_WARNING("HDR scene: RGBA16F cannot be drawn and blitted here - HDR lighting off");
            hdrSceneFailed_ = true;
        }
    }
    want = want && !hdrSceneFailed_;
    hdrScenePending_ = want;
    if (want != vkCtx->isHdrScene() && !msaaChangePending_) {
        pendingMsaaSamples_ = vkCtx->getMsaaSamples();
        msaaChangePending_ = true;
    }
}

void Renderer::applyMsaaChange() {
    VkSampleCountFlagBits samples = pendingMsaaSamples_;
    msaaChangePending_ = false;

    // Only where the upscaler cannot share a multisampled scene - a device
    // with no depth resolve - does a queued change have to fall back to 1x.
    if (samples > VK_SAMPLE_COUNT_1_BIT &&
        postProcessPipeline_ && postProcessPipeline_->isFsr2BlockingMsaa()) {
        samples = VK_SAMPLE_COUNT_1_BIT;
    }

    VkSampleCountFlagBits current = vkCtx->getMsaaSamples();
    // The HDR scene's format change takes the same rebuild (decideHdrScene).
    const bool hdrChange = hdrScenePending_ != vkCtx->isHdrScene();
    if (samples == current && !hdrChange) return;

    // Single GPU wait - all subsequent operations are CPU-side object creation
    vkDeviceWaitIdle(vkCtx->getDevice());

    // Set new MSAA and recreate swapchain (render pass, depth, MSAA image, framebuffers)
    vkCtx->setMsaaSamples(samples);
    vkCtx->setHdrScene(hdrScenePending_);
    if (!vkCtx->recreateSwapchain(window->getDrawableWidth(), window->getDrawableHeight())) {
        LOG_ERROR("MSAA change failed - reverting to 1x");
        vkCtx->setMsaaSamples(VK_SAMPLE_COUNT_1_BIT);
        vkCtx->setHdrScene(false);
        hdrScenePending_ = false;
        hdrSceneFailed_ = true;
        (void)vkCtx->recreateSwapchain(window->getDrawableWidth(), window->getDrawableHeight());
    }
    if (hdrChange) {
        LOG_INFO("HDR scene: ", vkCtx->isHdrScene() ? "on (RGBA16F)" : "off");
        // Built once against the late pass, whose format has just changed.
        if (m2Renderer) m2Renderer->releaseSoftParticlePipelines();
    }

    // Recreate all sub-renderer pipelines (they embed sample count from render pass)
    if (terrainRenderer) terrainRenderer->recreatePipelines();
    if (grassRenderer_) grassRenderer_->recreatePipelines();
    if (waterRenderer) {
        waterRenderer->recreatePipelines();
        // Under MSAA the water draws single-sampled in its own pass, after the
        // scene has resolved - it is a large alpha-blended surface whose edges
        // MSAA does nothing for, and drawing it there also keeps it out of its
        // own refraction copy.
        waterRenderer->destroyWater1xResources();
        setupWater1xPass();
    }
    if (wmoRenderer) wmoRenderer->recreatePipelines();
    if (m2Renderer) m2Renderer->recreatePipelines();
    if (skyboxModelRenderer_) skyboxModelRenderer_->recreatePipelines();
    if (skyStarsRenderer_) skyStarsRenderer_->recreatePipelines();
    if (characterRenderer) characterRenderer->recreatePipelines();
    if (footprintRenderer) footprintRenderer->recreatePipelines();
    if (weather) weather->recreatePipelines();
    if (lightning) lightning->recreatePipelines();
    if (swimEffects) {
        syncSwimEffectsTargetPass();
        swimEffects->recreatePipelines();
    }
    if (footfallDust_) footfallDust_->recreatePipelines();

    // Sky system sub-renderers
    if (skySystem) {
        if (auto* sb = skySystem->getSkybox()) sb->recreatePipelines();
        if (auto* sf = skySystem->getStarField()) sf->recreatePipelines();
        if (auto* ce = skySystem->getCelestial()) ce->recreatePipelines();
        if (auto* cl = skySystem->getClouds()) cl->recreatePipelines();
        if (auto* lf = skySystem->getLensFlare()) lf->recreatePipelines();
    }

    if (minimap) {
        // After syncSwimEffectsTargetPass above, which is what decides the pass
        // this is built against.
        minimap->recreatePipelines();
    }

    // Resize HiZ pyramid (depth format/MSAA may have changed)
    if (hizSystem_) {
        auto ext = vkCtx->getSwapchainExtent();
        if (!hizSystem_->resize(ext.width, ext.height)) {
            LOG_WARNING("HiZ resize failed after MSAA change");
            if (m2Renderer) m2Renderer->setHiZSystem(nullptr);
            hizSystem_->shutdown();
            hizSystem_.reset();
        }
    }

    // Selection circle + overlay + FSR use lazy init, just destroy them
    if (overlaySystem_) overlaySystem_->recreatePipelines();
    if (postProcessPipeline_) postProcessPipeline_->destroyAllResources(); // Will be lazily recreated in beginFrame()

    // ImGui is deliberately not restarted here.
    //
    // It always initialises at one sample into the overlay pass, which is
    // itself always single-sampled and depends only on the swapchain format -
    // so a change of scene anti-aliasing does not change anything ImGui built.
    // Recreating the swapchain produces a new overlay pass handle, but a
    // structurally identical one, and Vulkan requires a pipeline's render pass
    // to be compatible rather than the same object.
    //
    // Tearing the backend down destroyed its descriptor pool, and every UI
    // texture in the client - item and spell icons, raid icons, the talent
    // background, the world map layers, the widget renderer - holds a
    // descriptor set allocated from it. Nothing was told, so the next frame
    // drew with freed descriptors and the GPU was reset: the log shows the
    // swapchain and pipelines rebuilt, then the fence wait failing with
    // VK_ERROR_DEVICE_LOST a fraction of a second later. Applying a saved
    // anti-aliasing setting at startup made that look like a crash on launch.

}

void Renderer::setStageSink(std::function<void(const char*, float)> sink) {
    stageSink_ = std::move(sink);
    if (renderGraph_) renderGraph_->setStageSink(stageSink_);
}

void Renderer::beginFrame() {
    ZoneScopedN("Renderer::beginFrame");
    if (!vkCtx) return;
    if (vkCtx->isDeviceLost()) return;
    // Section timing for the frame profiler; see GameHandler::update.
    using clock = std::chrono::steady_clock;
    clock::time_point sectionStart = stageSink_ ? clock::now() : clock::time_point{};
    auto section = [&](const char* name) {
        if (!stageSink_) return;
        const auto now = clock::now();
        stageSink_(name, std::chrono::duration<float, std::milli>(now - sectionStart).count());
        sectionStart = now;
    };

    // The ablation's clock. Top of one frame to the top of the next is the
    // frame's whole wall time, which is the number a pass has to move to be
    // worth anything - a pass that gets cheaper while the frame does not has
    // not been paid for.
    if (passAblation_) {
        const auto now = std::chrono::steady_clock::now();
        // Only frames that drew the world. A frame of character select costs
        // five milliseconds and a frame with a zone streaming into it costs a
        // hundred and twenty, and the first run of this walked its baseline
        // through the former and its terrain phase through the latter - it
        // reported that terrain was worth minus 110 milliseconds.
        if (worldDrawnLastFrame_ && lastFrameStart_.time_since_epoch().count() != 0) {
            const bool wasSettling = passAblation_->settling();
            const AblationPass before = passAblation_->current();
            passAblation_->frame(std::chrono::duration<double, std::milli>(
                now - lastFrameStart_).count());
            // Say where it has got to. A run is the better part of a minute of
            // standing still and the first one silently went eight phases deep
            // before it was quit, with no way to tell it had started.
            if (passAblation_->running() &&
                (passAblation_->current() != before || (wasSettling && !passAblation_->settling()))) {
                LOG_WARNING("Pass ablation ", passAblation_->phaseNumber(), "/",
                            PassAblation::phaseCount(), ": ",
                            ablationPassName(passAblation_->current()),
                            passAblation_->current() == AblationPass::None ? "" : " off");
            }
        }
        lastFrameStart_ = now;
        worldDrawnLastFrame_ = false;
        if (!passAblation_->running() && !passAblationReported_) {
            passAblationReported_ = true;
            LOG_WARNING(passAblation_->report());
        }
    }

    worldDrawnThisFrame_ = false;

    // The HDR scene follows its row, and rides the MSAA rebuild below.
    decideHdrScene();

    // Apply deferred MSAA change between frames (before any rendering state is used)
    if (msaaChangePending_) {
        applyMsaaChange();
        // The rebuild destroys and remakes the swapchain, every render pass and
        // every pipeline. The frame slots are left mid-cycle by it, and the
        // next frame would reset a fence and re-record a command buffer the
        // GPU has not finished with - which is what validation reports and the
        // driver answers by losing the device.
        if (vkCtx) vkCtx->resetFrameSyncState();
    }

    // A fog quality change builds or frees its volumes, which the per-frame
    // sets bind - so between frames, before this one's set is used.
    if (volumetricFog_ && volumetricFog_->applyPendingQuality()) writeFogVolumeBindings();

    // Retire finished upload batches every frame.
    //
    // This was polled only from the terrain manager, so batches submitted by
    // anything else retired only while terrain happened to be streaming. A
    // rebuild reported 1423 submitted against 1241 retired - 182 outstanding,
    // each holding a fence, a command buffer and its staging buffers. With
    // FrameXML uploading hundreds of textures the backlog is much larger than
    // it was, and nothing bounded it.
    if (vkCtx) vkCtx->pollUploadBatches();

    // Post-process resource management (§4.3 - delegates to PostProcessPipeline)
    if (postProcessPipeline_) postProcessPipeline_->manageResources();
    // The HDR scene draws only into the post-process target; without one it
    // goes back to the swapchain's format before anything is recorded.
    if (vkCtx->isHdrScene() &&
        (!postProcessPipeline_ || postProcessPipeline_->getSceneFramebuffer() == VK_NULL_HANDLE)) {
        LOG_ERROR("HDR scene: no off-screen scene target - HDR lighting off");
        hdrSceneFailed_ = true;
        decideHdrScene();
        pendingMsaaSamples_ = vkCtx->getMsaaSamples();
        applyMsaaChange();
        vkCtx->resetFrameSyncState();
        if (postProcessPipeline_) postProcessPipeline_->manageResources();
    }

    // Whether this frame's blended particles wait for the late pass, which
    // also decides where the minimap goes (syncSwimEffectsTargetPass).
    decideSoftParticles();
    decideContactShadows();

    // The target that call may have just created or destroyed decides where
    // the water draws, and with it where the spray and the minimap go. Asked
    // again here, after the answer can have changed, rather than only at the
    // anti-aliasing rebuild that runs before the target exists.
    refreshSwimEffectsPass();

    // Handle swapchain recreation if needed
    if (vkCtx->isSwapchainDirty()) {
        // Skip recreation while window is minimized (0×0 extent is a Vulkan spec violation)
        // Pixels, like the extent below: a window with a surface behind it is
        // never rebuilt from the size the desktop places it at.
        if (window->getDrawableWidth() == 0 || window->getDrawableHeight() == 0) return;
        (void)vkCtx->recreateSwapchain(window->getDrawableWidth(), window->getDrawableHeight());
        // Rebuild water resources that reference swapchain extent/views
        if (waterRenderer) {
            waterRenderer->recreatePipelines();
            waterRenderer->destroyWater1xResources();
            setupWater1xPass();
        }
        // Recreate post-process resources for new swapchain dimensions
        if (postProcessPipeline_) postProcessPipeline_->handleSwapchainResize();
        // Resize HiZ depth pyramid for new swapchain dimensions
        if (hizSystem_) {
            auto ext = vkCtx->getSwapchainExtent();
            if (!hizSystem_->resize(ext.width, ext.height)) {
                LOG_WARNING("HiZ resize failed - disabling occlusion culling");
                if (m2Renderer) m2Renderer->setHiZSystem(nullptr);
                hizSystem_->shutdown();
                hizSystem_.reset();
            }
        }
    }

    // Between frames, so a resize of the ray traced lighting's images can wait
    // for the device and rewrite both slots' sets.
    if (rtLighting_ && rtLighting_->prepare(sceneRenderExtent())) writeRtLightingBindings();

    rtRecordedThisFrame_ = false;
    waterPassOpen_ = false;
    section("beginFrame/uploads+swapchain");

    // Acquire swapchain image and begin command buffer
    currentCmd = vkCtx->beginFrame(currentImageIndex);
    if (currentCmd == VK_NULL_HANDLE) {
        // Swapchain out of date, will retry next frame
        return;
    }
    section("beginFrame/acquire");

    // This slot's fence has just been waited on, so a frame it copied for the
    // recording two frames ago is complete.
    collectRecordedFrame();

    // FSR2 jitter pattern (§4.3 - delegates to PostProcessPipeline)
    if (postProcessPipeline_ && camera) postProcessPipeline_->applyJitter(camera.get());

    // Compute fresh shadow matrix BEFORE UBO update so shaders get current-frame data.
    planShadowFrame();
    // The unit map's units record on a worker from here to the shadow pass,
    // beside the per-frame block, the culls and the composites.
    startUnitShadowRecording();

    // Update per-frame UBO with current camera/lighting state
    updatePerFrameUBO();
    section("beginFrame/record+ubo");

    // ── Early compute: M2 frustum culling ──
    // beginFrame() has already waited for this frame slot's previous fence, so
    // its mapped visibility output is complete and safe for the CPU to reuse.
    // Read/invalidate that completed output, then record the next cull dispatch
    // directly into the normal frame command buffer. The old path submitted a
    // separate command buffer and synchronously waited on a fence every frame,
    // serializing CPU and GPU work solely to obtain same-frame cull results.
    if (m2Renderer && camera && vkCtx) {
        uint32_t frame = vkCtx->getCurrentFrame();
        m2Renderer->invalidateCullOutput(frame);
        m2Renderer->dispatchCullCompute(currentCmd, frame, *camera);
    }
    section("beginFrame/m2 cull upload");

    // Grass culls here too, for the same reason: a dispatch has to be recorded
    // outside a render pass. Unlike the M2 path nothing reads the result back -
    // the count it produces is consumed by the indirect draw on the GPU.
    if (grassRenderer_ && camera && vkCtx) {
        updateGrassPopulation();
        grassRenderer_->reportCullResult();
        grassRenderer_->dispatchCull(currentCmd, vkCtx->getCurrentFrame(), *camera,
                                     characterPosition);
    }

    // --- Off-screen pre-passes ---
    // Build frame graph: registers pre-passes as graph nodes with dependencies.
    // compile() topologically sorts; execute() runs them with auto barriers.
    section("beginFrame/grass+misc");
    buildFrameGraph(nullptr);
    if (renderGraph_) {
        renderGraph_->execute(currentCmd);
    }
    // The shadow pass joins the units' job; one it did not reach (a graph
    // that left the pass out) is waited for here, and its buffer unused,
    // before the frame's recording moves on.
    if (unitShadowJobActive_) {
        unitShadowFuture_.wait();
        unitShadowJobActive_ = false;
    }
    section("beginFrame/graph (shadows, hiz, fog)");

    // --- Begin render pass ---
    // Select framebuffer: PP off-screen target or swapchain (§4.3 - PostProcessPipeline)
    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass = vkCtx->getImGuiRenderPass();

    VkExtent2D renderExtent;
    VkFramebuffer ppFB = postProcessPipeline_ ? postProcessPipeline_->getSceneFramebuffer() : VK_NULL_HANDLE;
    if (ppFB != VK_NULL_HANDLE) {
        rpInfo.framebuffer = ppFB;
        renderExtent = postProcessPipeline_->getSceneRenderExtent();
    } else {
        // The HDR scene never gets here: beginFrame turns it off when the
        // off-screen target it needs could not be made (applyMsaaChange).
        rpInfo.framebuffer = vkCtx->getSwapchainFramebuffers()[currentImageIndex];
        renderExtent = vkCtx->getSwapchainExtent();
    }

    rpInfo.renderArea.offset = {.x = 0, .y = 0};
    rpInfo.renderArea.extent = renderExtent;

    // Clear values must match attachment count: 2 (no MSAA), 3 (MSAA), or 4 (MSAA+depth resolve)
    VkClearValue clearValues[4]{};
    // Black, or under liquid the fog colour - the underwater light's, which is
    // what the stock client clears to there (0x79a870: DayNight+0x8c).
    const glm::vec3 clearRgb = cameraUnderwater_ ? glm::vec3(currentFrameData.fogColor) : glm::vec3(0.0f);
    clearValues[0].color = {{clearRgb.r, clearRgb.g, clearRgb.b, 1.0f}};
    clearValues[1].depthStencil = {.depth = 1.0f, .stencil = 0};
    clearValues[2].color = {{clearRgb.r, clearRgb.g, clearRgb.b, 1.0f}};
    clearValues[3].depthStencil = {.depth = 1.0f, .stencil = 0};
    bool msaaOn = (vkCtx->getMsaaSamples() > VK_SAMPLE_COUNT_1_BIT);
    if (msaaOn) {
        bool depthRes = (vkCtx->getDepthResolveImageView() != VK_NULL_HANDLE);
        rpInfo.clearValueCount = depthRes ? 4 : 3;
    } else {
        rpInfo.clearValueCount = 2;
    }
    rpInfo.pClearValues = clearValues;

    // Cache render pass state for secondary command buffer inheritance
    activeRenderPass_ = rpInfo.renderPass;
    activeFramebuffer_ = rpInfo.framebuffer;
    activeRenderExtent_ = renderExtent;

    VkSubpassContents subpassMode = parallelRecordingEnabled_
        ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS
        : VK_SUBPASS_CONTENTS_INLINE;
    vkCmdBeginRenderPass(currentCmd, &rpInfo, subpassMode);

    if (!parallelRecordingEnabled_) {
        // Fallback: set dynamic viewport and scissor on primary (inline mode)
        VkViewport viewport{};
        viewport.width = static_cast<float>(renderExtent.width);
        viewport.height = static_cast<float>(renderExtent.height);
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(currentCmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.extent = renderExtent;
        vkCmdSetScissor(currentCmd, 0, 1, &scissor);
    }
}

void Renderer::endFrame() {
    ZoneScopedN("Renderer::endFrame");
    if (!vkCtx || currentCmd == VK_NULL_HANDLE) return;

    logViewDistanceDiag();

    // Post-process execution (§4.3 - delegates to PostProcessPipeline). Whether
    // it swapped the scene pass for an INLINE one no longer matters to the
    // caller: the UI is drawn in the overlay pass, which this function opens
    // itself once whichever pass is current has been closed.
    bool postProcessed = false;
    if (postProcessPipeline_) {
        postProcessed = postProcessPipeline_->executePostProcessing(
            currentCmd, currentImageIndex, camera.get(), lastDeltaTime_);
    }

    // The scene is complete: close its pass so the water refraction copy can run
    // (a copy is illegal inside a render pass), then draw the UI in the overlay
    // pass. Capturing after the UI instead is what refracted the interface into
    // the water. The overlay pass is single-sampled and colour-only, which is
    // also why the UI costs the same here whatever MSAA the scene uses.
    vkCmdEndRenderPass(currentCmd);
    // After the end, not before: when nothing post-processed, the pass still
    // open is the scene pass, begun for secondary buffers, and a checkpoint or
    // timestamp recorded inline into it is invalid.
    //
    // And named for what that pass was. With no post-processing on, it is the
    // scene or the water after it, and labelling it "post-process" put the
    // whole world under a pass that had drawn nothing.
    if (postProcessed) {
        vkCtx->gpuMark(currentCmd, postProcessPipeline_->outputPassLabel());
    } else {
        markWorldPassClosed();
        // The scene went straight to the swapchain: this is where it closed.
        recordContactShadowPass(currentCmd);
        recordSoftParticlePass(currentCmd);
    }

    // Only when water could not be moved out of the scene pass (MSAA). Otherwise
    // renderWorld already took the copy at the one point in the frame where the
    // scene is finished but the water is not yet over it; copying again here
    // would replace that with an image containing the water.
    // An off-screen scene took its copy as it closed (setSceneClosedHook).
    if (!waterDrawsInContinuePass()
        && !(postProcessPipeline_ && postProcessPipeline_->getSceneFramebuffer() != VK_NULL_HANDLE)
        && waterRenderer && waterRenderer->isRefractionEnabled() && waterRenderer->hasSurfaces()
        && currentImageIndex < vkCtx->getSwapchainImages().size()) {
        // No colour: the swapchain already holds this frame's water.
        waterRenderer->captureSceneHistory(
            currentCmd,
            VK_NULL_HANDLE,
            vkCtx->getDepthCopySourceImage(),
            vkCtx->getSwapchainExtent(),
            vkCtx->isDepthCopySourceMsaa(),
            vkCtx->getCurrentFrame());
        vkCtx->gpuMark(currentCmd, "water refraction copy");
    }

    // The picture is finished and out of every pass that drew it: the one
    // point where the shafts can copy it down, before the overlay pass opens.
    recordSunShafts();

    const auto& overlayFbs = vkCtx->getOverlayFramebuffers();
    if (vkCtx->getOverlayRenderPass() != VK_NULL_HANDLE && currentImageIndex < overlayFbs.size()) {
        VkRenderPassBeginInfo overlayRp{};
        overlayRp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        overlayRp.renderPass = vkCtx->getOverlayRenderPass();
        overlayRp.framebuffer = overlayFbs[currentImageIndex];
        overlayRp.renderArea.extent = vkCtx->getSwapchainExtent();
        vkCmdBeginRenderPass(currentCmd, &overlayRp, VK_SUBPASS_CONTENTS_INLINE);

        VkExtent2D ext = vkCtx->getSwapchainExtent();
        VkViewport vp{};
        vp.width = static_cast<float>(ext.width);
        vp.height = static_cast<float>(ext.height);
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(currentCmd, 0, 1, &vp);
        VkRect2D sc{};
        sc.extent = ext;
        vkCmdSetScissor(currentCmd, 0, 1, &sc);

        // Under the interface, over everything else.
        if (sunShafts_) sunShafts_->composite(currentCmd, vkCtx->getCurrentFrame());

        // ImGui's pipelines are built against the overlay pass, so it always
        // records inline here rather than into a scene-pass secondary buffer.
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), currentCmd);
        vkCmdEndRenderPass(currentCmd);
        if (vkCtx) vkCtx->gpuMark(currentCmd, "interface");
    } else {
        LOG_ERROR("Overlay render pass missing - UI not drawn this frame");
    }

    // Water now renders in the main pass (renderWorld), no separate 1x pass needed.

    if (afterInterface_) afterInterface_(currentCmd);

    // Last, so the recording holds everything the player sees - and then the
    // recording dot, which it does not.
    recordScreenCapture();

    // Submit and present
    vkCtx->endFrame(currentCmd, currentImageIndex);
    currentCmd = VK_NULL_HANDLE;
}

void Renderer::setCharacterFollow(uint32_t instanceId) {
    characterInstanceId = instanceId;
    // A new character instance is the character itself; whatever morph was
    // drawn for the old one went with it, and the spawner re-applies it.
    nativeCharacterInstanceId_ = 0;
    if (cameraController && instanceId > 0) {
        cameraController->setFollowTarget(&characterPosition);
    }
    if (animationController_) animationController_->onCharacterFollow(instanceId);
}

void Renderer::setCharacterMorphInstance(uint32_t morphInstanceId) {
    const uint32_t native = getNativeCharacterInstanceId();
    if (native == 0 || !characterRenderer) return;
    const uint32_t drawn = morphInstanceId != 0 ? morphInstanceId : native;
    if (drawn == characterInstanceId) return;
    nativeCharacterInstanceId_ = morphInstanceId != 0 ? native : 0;
    characterRenderer->setInstanceVisible(native, morphInstanceId == 0);
    characterInstanceId = drawn;
    // The camera measures the pivot off the drawn model (a bear's head is not
    // a tauren's), and hides it in first person; the animation controller
    // re-probes which sequences the new model has.
    if (cameraController) cameraController->setCharacterRenderer(characterRenderer.get(), drawn);
    if (animationController_) animationController_->onCharacterFollow(drawn);
}

bool Renderer::startRecording(const std::string& path, std::string& error) {
    if (recorder_ && recorder_->isRecording()) {
        error = "already recording";
        return false;
    }
    if (!vkCtx) {
        error = "there is no renderer to record from";
        return false;
    }
    if (!core::ScreenRecorder::compiledIn()) {
        error = "this build was made without FFmpeg 5.1 or later, which recording needs";
        return false;
    }
    const VkExtent2D extent = vkCtx->getSwapchainExtent();
    const core::RecordingSize size = core::recordingFrameSize(extent.width, extent.height);
    auto capture = std::make_unique<ScreenCapture>();
    if (!capture->initialize(vkCtx, size.width, size.height)) {
        capture->shutdown();
        error = "could not set up reading frames back from the GPU";
        return false;
    }
    auto recorder = std::make_unique<core::ScreenRecorder>();
    if (!recorder->start(path, size, error)) {
        capture->shutdown();
        return false;
    }
    screenCapture_ = std::move(capture);
    recorder_ = std::move(recorder);
    recordingFailure_.clear();
    return true;
}

core::ScreenRecorder::Stats Renderer::stopRecording() {
    core::ScreenRecorder::Stats stats;
    if (!recorder_) return stats;
    // The frames still on the GPU are finished and handed over, oldest first,
    // so the file ends where the recording did rather than two frames short.
    if (vkCtx && screenCapture_) {
        vkDeviceWaitIdle(vkCtx->getDevice());
        ScreenCapture::Ready ready[2] = {screenCapture_->collect(0), screenCapture_->collect(1)};
        if (ready[0].valid && ready[1].valid && ready[1].pts < ready[0].pts) std::swap(ready[0], ready[1]);
        ScreenCapture* capture = screenCapture_.get();
        for (const auto& r : ready) {
            if (!r.valid) continue;
            const uint32_t buffer = r.buffer;
            recorder_->submit({.bgra = r.bgra, .stride = r.stride, .pts = r.pts,
                               .release = [capture, buffer] { capture->release(buffer); }});
        }
    }
    // The recorder first: its thread holds readback buffers until it is done.
    stats = recorder_->stop();
    recorder_.reset();
    if (screenCapture_) {
        screenCapture_->shutdown();
        screenCapture_.reset();
    }
    return stats;
}

bool Renderer::isRecording() const {
    return recorder_ && recorder_->isRecording();
}

std::string Renderer::takeRecordingFailure() {
    std::string failure;
    failure.swap(recordingFailure_);
    return failure;
}

void Renderer::collectRecordedFrame() {
    if (!recorder_ || !screenCapture_ || !vkCtx) return;
    const ScreenCapture::Ready ready = screenCapture_->collect(vkCtx->getCurrentFrame());
    if (ready.valid) {
        ScreenCapture* capture = screenCapture_.get();
        const uint32_t buffer = ready.buffer;
        recorder_->submit({.bgra = ready.bgra, .stride = ready.stride, .pts = ready.pts,
                           .release = [capture, buffer] { capture->release(buffer); }});
    }
    // Given up on its own: finish what it has and say why, once.
    if (recorder_->failed()) {
        recordingFailure_ = recorder_->failure();
        stopRecording();
    }
}

void Renderer::recordScreenCapture() {
    if (!recorder_ || !screenCapture_ || !recorder_->isRecording() || currentCmd == VK_NULL_HANDLE) return;
    const auto& images = vkCtx->getSwapchainImages();
    const VkExtent2D extent = vkCtx->getSwapchainExtent();
    int64_t pts = 0;
    if (currentImageIndex < images.size() && recorder_->frameDue(&pts)) {
        if (!screenCapture_->record(currentCmd, vkCtx->getCurrentFrame(), images[currentImageIndex],
                                    extent, pts)) {
            recorder_->frameDropped();
        }
        recorder_->frameTaken(pts);
    }
    const auto& overlayFbs = vkCtx->getOverlayFramebuffers();
    if (vkCtx->getOverlayRenderPass() != VK_NULL_HANDLE && currentImageIndex < overlayFbs.size()) {
        screenCapture_->drawIndicator(currentCmd, overlayFbs[currentImageIndex], extent,
                                      static_cast<float>(recorder_->elapsedSeconds()));
    }
}

bool Renderer::captureScreenshot(const std::string& outputPath, int jpegQuality) {
    if (!vkCtx) return false;

    VkDevice device     = vkCtx->getDevice();
    VmaAllocator alloc  = vkCtx->getAllocator();
    VkExtent2D extent   = vkCtx->getSwapchainExtent();
    const auto& images  = vkCtx->getSwapchainImages();

    if (images.empty() || currentImageIndex >= images.size()) return false;

    VkImage srcImage = images[currentImageIndex];
    uint32_t w = extent.width;
    uint32_t h = extent.height;
    VkDeviceSize bufSize = static_cast<VkDeviceSize>(w) * h * 4;

    // Stall GPU so the swapchain image is idle
    vkDeviceWaitIdle(device);

    // Create staging buffer
    VkBufferCreateInfo bufInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size  = bufSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_CPU_ONLY;

    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    if (vmaCreateBuffer(alloc, &bufInfo, &allocCI, &stagingBuf, &stagingAlloc, nullptr) != VK_SUCCESS) {
        LOG_WARNING("Screenshot: failed to create staging buffer");
        return false;
    }

    // Record copy commands
    VkCommandBuffer cmd = vkCtx->beginSingleTimeCommands();

    // Transition swapchain image: PRESENT_SRC → TRANSFER_SRC
    VkImageMemoryBarrier2 toTransfer{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toTransfer.srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    toTransfer.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    toTransfer.srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT;
    toTransfer.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toTransfer.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.image               = srcImage;
    toTransfer.subresourceRange    = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
    VkDependencyInfo toTransferDep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    toTransferDep.dependencyFlags = 0;
    toTransferDep.imageMemoryBarrierCount = 1;
    toTransferDep.pImageMemoryBarriers = &toTransfer;
    cmdPipelineBarrier2(cmd, toTransferDep);

    // Copy image to buffer
    VkBufferImageCopy region{};
    region.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
    region.imageExtent      = {.width = w, .height = h, .depth = 1};
    vkCmdCopyImageToBuffer(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    // Transition back: TRANSFER_SRC → PRESENT_SRC
    VkImageMemoryBarrier2 toPresent = toTransfer;
    toPresent.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    toPresent.dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toPresent.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    toPresent.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkDependencyInfo toPresentDep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    toPresentDep.imageMemoryBarrierCount = 1;
    toPresentDep.pImageMemoryBarriers = &toPresent;
    cmdPipelineBarrier2(cmd, toPresentDep);

    vkCtx->endSingleTimeCommands(cmd);

    // Map and convert BGRA → RGBA
    void* mapped = nullptr;
    vmaMapMemory(alloc, stagingAlloc, &mapped);
    auto* pixels = static_cast<uint8_t*>(mapped);
    for (uint32_t i = 0; i < w * h; ++i) {
        std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]); // B ↔ R
    }

    // Ensure output directory exists
    std::filesystem::path outPath(outputPath);
    if (outPath.has_parent_path())
        std::filesystem::create_directories(outPath.parent_path());

    const std::string ext = outPath.extension().string();
    int ok = 0;
    if (ext == ".jpg" || ext == ".jpeg")
        ok = stbi_write_jpg(outputPath.c_str(), static_cast<int>(w), static_cast<int>(h),
                            4, pixels, jpegQuality);
    else if (ext == ".tga")
        ok = stbi_write_tga(outputPath.c_str(), static_cast<int>(w), static_cast<int>(h), 4, pixels);
    else
        ok = stbi_write_png(outputPath.c_str(), static_cast<int>(w), static_cast<int>(h),
                            4, pixels, static_cast<int>(w * 4));

    vmaUnmapMemory(alloc, stagingAlloc);
    vmaDestroyBuffer(alloc, stagingBuf, stagingAlloc);

    if (ok) {
        LOG_INFO("Screenshot saved: ", outputPath);
    } else {
        LOG_WARNING("Screenshot: could not write for ", outputPath);
    }
    return ok != 0;
}

void Renderer::resetCombatVisualState() {
    if (animationController_) animationController_->resetCombatVisualState();
    if (spellVisualSystem_) spellVisualSystem_->reset();
}

const std::string& Renderer::getCurrentZoneName() const {
    static const std::string empty;
    return audioCoordinator_ ? audioCoordinator_->getCurrentZoneName() : empty;
}

bool Renderer::updateSkyboxLayers() {
    // Which skybox models a place uses is Light.dbc's answer, not a map id:
    // LightParams names a LightSkybox row and LightSkybox names the model.
    // LightingManager walks that for the lights around the player and says
    // how much of each is up; this keeps one instance per model and fades it
    // by that weight, so crossing from one zone's sky to another's is a blend
    // and not a swap. WOWEE_NO_SKY_M2=1 draws the procedural sky alone.
    static const bool noSkyM2 = std::getenv("WOWEE_NO_SKY_M2") != nullptr;
    if (noSkyM2) return false;
    if (!skyboxModelRenderer_ || !lightingManager || !cachedAssetManager || !camera) {
        return false;
    }

    auto normalized = [](std::string p) {
        std::replace(p.begin(), p.end(), '/', '\\');
        return p;
    };
    const auto& layers = lightingManager->getSkyboxLayers();

    // The night stars ride their own renderer: the stock client draws them
    // first, under the dome (0x7f09b0), and the LightSkybox models last,
    // after the sun, moons and clouds.
    auto rendererFor = [&](bool stars) -> M2Renderer* {
        return stars ? skyStarsRenderer_.get() : skyboxModelRenderer_.get();
    };

    // Faded out and no longer wanted.
    std::erase_if(skyLayers_, [&](const SkyLayerInstance& sky) {
        const bool wanted = std::any_of(layers.begin(), layers.end(), [&](const auto& l) {
            return normalized(l.path) == sky.path && l.stars == sky.stars;
        });
        if (!wanted) {
            if (M2Renderer* r = rendererFor(sky.stars)) r->removeInstance(sky.instanceId);
        }
        return !wanted;
    });

    for (const auto& layer : layers) {
        M2Renderer* r = rendererFor(layer.stars);
        if (!r) continue;
        const std::string path = normalized(layer.path);
        auto it = std::find_if(skyLayers_.begin(), skyLayers_.end(), [&](const SkyLayerInstance& s) {
            return s.path == path && s.stars == layer.stars;
        });
        if (it == skyLayers_.end()) {
            const uint32_t modelId = loadSkyboxModel(path, layer.stars);
            if (modelId == 0) continue;
            const uint32_t instanceId = r->createInstance(modelId, camera->getPosition(), glm::vec3(0.0f), 1.0f);
            if (instanceId == 0) continue;
            r->setSkipCollision(instanceId, true);
            skyLayers_.push_back({.path = path, .instanceId = instanceId, .stars = layer.stars});
            it = std::prev(skyLayers_.end());
        }
        r->setInstanceFade(it->instanceId, layer.weight);
        r->setInstancePosition(it->instanceId, camera->getPosition());
    }
    return !skyLayers_.empty();
}

uint32_t Renderer::loadSkyboxModel(const std::string& path, bool stars) {
    M2Renderer* target = stars ? skyStarsRenderer_.get() : skyboxModelRenderer_.get();
    if (!target) return 0;
    const std::string cacheKey = stars ? "*" + path : path;
    if (auto it = loadedSkyModels_.find(cacheKey); it != loadedSkyModels_.end()) return it->second;
    if (failedSkyboxPaths_.count(path)) return 0;

    std::vector<std::string> candidates{path};
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        candidates.push_back(path + ".m2");
    } else {
        std::string ext = path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".mdx" || ext == ".mdl") candidates.push_back(path.substr(0, dot) + ".m2");
    }

    std::vector<uint8_t> modelData;
    std::string resolvedPath;
    for (const auto& candidate : candidates) {
        modelData = cachedAssetManager->readFileOptional(candidate);
        if (!modelData.empty()) {
            resolvedPath = candidate;
            break;
        }
    }
    if (modelData.empty()) {
        LOG_WARNING("Skybox model unavailable: ", path);
        failedSkyboxPaths_.insert(path);
        return 0;
    }

    pipeline::M2Model model = pipeline::M2Loader::load(modelData);
    model.name = resolvedPath + "#original-sky";
    const std::string skinPath = pipeline::skinPathForM2(resolvedPath);
    auto skinData = cachedAssetManager->readFileOptional(skinPath);
    if (!skinData.empty() && model.version >= 264) {
        pipeline::M2Loader::loadSkin(skinData, model);
    }
    if (!model.isValid()) {
        LOG_WARNING("Skybox model is invalid: ", resolvedPath);
        failedSkyboxPaths_.insert(path);
        return 0;
    }

    const uint32_t modelId = static_cast<uint32_t>(std::hash<std::string>{}(model.name));
    if (!target->loadModel(model, modelId)) {
        LOG_WARNING("Failed to upload skybox model: ", resolvedPath);
        failedSkyboxPaths_.insert(path);
        return 0;
    }
    loadedSkyModels_[cacheKey] = modelId;
    LOG_INFO("Skybox model loaded: ", resolvedPath);
    return modelId;
}

bool Renderer::isOnOutdoorPvpObjective() const {
    if (!zoneManager) return false;
    const uint32_t areaId = getCurrentAreaId();
    return areaId != 0 && zoneManager->isOutdoorPvpArea(areaId);
}

uint32_t Renderer::getCurrentAreaId() const {
    if (liveAreaValid_) return liveAreaId_;
    if (!terrainManager) return 0;
    const auto areaId = terrainManager->getAreaIdAt(characterPosition.x, characterPosition.y);
    return areaId ? *areaId : 0u;
}

void Renderer::updateLiveArea(uint32_t wmoId, uint16_t nameSet, uint32_t wmoGroupId) {
    const float x = characterPosition.x, y = characterPosition.y, z = characterPosition.z;
    uint32_t area = 0;
    if (terrainManager) {
        if (const auto terrainArea = terrainManager->getAreaIdAt(x, y)) area = *terrainArea;
    }
    liveAreaFromWmo_ = false;
    liveWmoAreaName_.clear();

    // Inside a WMO group the stock client asks WMOAreaTable first: that is
    // what names Ironforge and the Undercity, whose terrain chunks are Dun
    // Morogh's and Tirisfal's. See game/wmo_area.hpp.
    if (wmoId != 0 && wmoGroupId != 0 && zoneManager && wmoRenderer) {
        const game::WmoAreaInfo info =
            zoneManager->resolveWmoArea(wmoId, nameSet, static_cast<int32_t>(wmoGroupId));
        if (info.areaId != 0 || !info.name.empty()) {
            // The group is a bounding-box answer, and Ironforge's boxes reach up
            // through the mountain it is cut into. Only a WMO floor the player
            // is standing on, and that is not under the open ground, counts.
            const auto floor = wmoRenderer->getFloorHeight(x, y, z + 2.0f, nullptr, z);
            std::optional<float> ground;
            if (terrainManager && !terrainManager->isHoleAt(x, y)) ground = terrainManager->getHeightAt(x, y);
            if (game::wmoDecidesArea(z, floor, ground)) {
                if (info.areaId != 0) area = info.areaId;
                liveWmoAreaName_ = info.name;
                liveAreaFromWmo_ = true;
            }
        }
    }
    liveAreaId_ = area;
    liveAreaValid_ = true;
}

void Renderer::updateMinimapWmo(uint32_t instanceId, int32_t tightGroupIndex) {
    if (!minimap) return;
    // WOWEE_WMO_MINIMAP=0 keeps the terrain map everywhere, as before.
    static const bool enabled = [] {
        const char* v = std::getenv("WOWEE_WMO_MINIMAP");
        return !(v && v[0] == '0');
    }();

    bool indoors = false;
    if (enabled && instanceId != 0 && tightGroupIndex >= 0 && wmoRenderer) {
        Minimap::WmoView view;
        const glm::vec3 probe = characterPosition + glm::vec3(0.0f, 0.0f, 1.0f);
        if (wmoRenderer->getMinimapSource(instanceId, probe, view.modelMatrix, view.tileRoot,
                                          view.groups, view.playerLocal)) {
            view.currentGroup = static_cast<size_t>(tightGroupIndex);
            // Only an indoor group switches the map, as in stock: a city
            // street (Stormwind's, Silvermoon's) is outdoors and keeps the
            // terrain. And only on its floor, by the same test that lets the
            // WMO name the area: Ironforge's boxes reach up through the
            // mountain, and standing on top of it is Dun Morogh.
            if (view.currentGroup < view.groups.size() &&
                wmo_minimap::groupIsIndoorForMinimap(view.groups[view.currentGroup].flags)) {
                const float x = characterPosition.x, y = characterPosition.y, z = characterPosition.z;
                const auto floor = wmoRenderer->getFloorHeight(x, y, z + 2.0f, nullptr, z);
                std::optional<float> ground;
                if (terrainManager && !terrainManager->isHoleAt(x, y)) ground = terrainManager->getHeightAt(x, y);
                if (game::wmoDecidesArea(z, floor, ground)) indoors = minimap->setWmoView(view);
            }
        }
    }
    if (!indoors) minimap->clearWmoView();
}

void Renderer::fireZoneEvents() {
    auto* gh = core::Application::getInstance().getGameHandler();
    if (!gh) return;
    const uint32_t mapId = gh->getCurrentMapId();
    const uint32_t areaId = getCurrentAreaId();
    const uint32_t zoneId = (areaId != 0 && zoneManager) ? zoneManager->resolveAreaZoneId(areaId) : areaId;
    if (zoneId == 0) return;  // nothing known here yet; keep the last answer

    // A new map, or the first area known on it, is announced by world entry
    // (PLAYER_ENTERING_WORLD and ZONE_CHANGED_NEW_AREA), not here.
    const bool seed = mapId != zoneEventMapId_ || zoneEventZoneId_ == 0;
    const bool zoneChanged = zoneId != zoneEventZoneId_;
    const bool subzoneChanged = areaId != zoneEventAreaId_ || liveWmoAreaName_ != zoneEventWmoName_;
    zoneEventMapId_ = mapId;
    zoneEventZoneId_ = zoneId;
    zoneEventAreaId_ = areaId;
    zoneEventWmoName_ = liveWmoAreaName_;
    if (seed || (!zoneChanged && !subzoneChanged)) return;

    // The stock client works these out from its own lookup, as this does:
    // ZONE_CHANGED_NEW_AREA for a new zone, ZONE_CHANGED_INDOORS for a new
    // room or area inside a WMO, ZONE_CHANGED for a new subzone outdoors and
    // for the way back out. zonetext.lua shows the zone banner on any of them
    // once GetZoneText changes, so walking through Ironforge's gates names
    // the city even before the server's SMSG_INIT_WORLD_STATES arrives.
    //
    // After a teleport on the same map the server's SMSG_INIT_WORLD_STATES
    // names the new zone before this lookup has the ground under the player,
    // and its ZONE_CHANGED_NEW_AREA went out while GetZoneText and the zone
    // SetMapToCurrentZone reads were still the old one: WatchFrame set the
    // map to the zone just left, GetPlayerMapPosition answered 0,0 there and
    // the tracker's POIs were the old zone's until the next zone change (a
    // hearthstone within a continent does exactly this). The stock client
    // fires the event from its own zone, so when this lookup arrives at a
    // zone already announced for it, the interface hears it again now that
    // the answers match.
    if (zoneChanged && !gh->announceZoneChange(zoneId)) {
        gh->fireAddonEvent("ZONE_CHANGED_NEW_AREA", {});
    }
    gh->fireAddonEvent(liveAreaFromWmo_ ? "ZONE_CHANGED_INDOORS" : "ZONE_CHANGED", {});
}

uint32_t Renderer::getPlayerWmoGroupId() const {
    if (!wmoRenderer) return 0;
    WMORenderer::AreaHit hit;
    // A little above the feet, so the floor stood on is inside its group.
    if (!wmoRenderer->insideWmoArea(characterPosition.x, characterPosition.y,
                                    characterPosition.z + 1.0f, &hit)) {
        return 0;
    }
    return hit.wmoGroupId;
}

uint32_t Renderer::getCurrentZoneId() const {
    // A zone remembered from the last map is worse than none: the sticky
    // answer below would hold Duskwood's pinned midnight over a continent
    // away until the first chunk of the new one loaded.
    if (const auto* gh = core::Application::getInstance().getGameHandler()) {
        const uint32_t mapId = gh->getCurrentMapId();
        if (mapId != lastResolvedZoneMapId_) {
            lastResolvedZoneMapId_ = mapId;
            lastResolvedZoneId_ = 0;
        }
    }

    uint32_t tileZoneId = 0;
    // The area the player is in, WMOAreaTable's inside a city like Ironforge.
    if (zoneManager) {
        if (const uint32_t areaId = getCurrentAreaId()) {
            lastResolvedZoneId_ = zoneManager->resolveAreaZoneId(areaId);
            return lastResolvedZoneId_;
        }
    }
    if (zoneManager && terrainManager) {
        const auto tile = terrainManager->getCurrentTile();
        tileZoneId = zoneManager->getZoneId(tile.x, tile.y);
    }

    // The chunk under the player did not say which area it is - either its
    // ADT is not resident yet, or its area id is zero, which a great many
    // chunks carry. That is "this chunk does not know", not "the zone
    // changed", and answering from a different source instead made the zone
    // id flip back and forth as the player walked from a chunk that knew to
    // one that did not.
    //
    // Nothing about that is quiet. The zone id picks the dark-zone ambience
    // override, which replaces the four sky colours outright, and in Duskwood
    // it also pins the visual hour to the small hours - so the sky changed
    // brightness every time a chunk boundary was crossed and held still the
    // moment the player did. Keep the last chunk that did know.
    if (lastResolvedZoneId_ != 0) return lastResolvedZoneId_;

    const auto* gh = core::Application::getInstance().getGameHandler();
    if (gh && gh->getWorldStateZoneId() != 0) {
        const uint32_t areaId = gh->getWorldStateZoneId();
        return zoneManager ? zoneManager->resolveAreaZoneId(areaId) : areaId;
    }
    if (audioCoordinator_ && audioCoordinator_->getCurrentZoneId() != 0)
        return audioCoordinator_->getCurrentZoneId();
    return tileZoneId;
}

float Renderer::sampleSunOcclusion() const {
    if (!camera) return 1.0f;
    const glm::vec3 eye = camera->getPosition();

    // The sun Celestial draws, which is not where the light comes from: the
    // stock light stays up all night (see stock_day_night.hpp).
    const glm::vec3 sunDir = lightingManager
        ? lightingManager->getLightingParams().sunDir
        : glm::vec3(0.0f, 0.0f, -1.0f);
    // Below the horizon there is nothing to be occluded by, and nothing to
    // flare either - the time-of-day gate in LensFlare covers the same ground.
    if (sunDir.z <= 0.0f) return 1.0f;

    // Indoors the sun is behind a roof by definition, and a roof is the one
    // occluder the terrain march below cannot see.
    if (wmoRenderer && wmoRenderer->isInsideWMO(eye.x, eye.y, eye.z)) return 1.0f;

    // How far to look. Far enough to clear the hill the camera is standing
    // under, and no further: past a few hundred yards a ridge on the horizon
    // is the sky's business, not the flare's.
    constexpr float kReach = 600.0f;

    if (wmoRenderer && wmoRenderer->raycastBoundingBoxes(eye, sunDir, kReach) < kReach) {
        return 1.0f;
    }

    // The terrain, marched rather than intersected: the heightmap is what
    // terrain collision is here, so asking it how high the ground is under each
    // sample answers the same question an intersection would.
    //
    // Geometric steps, because the sample that decides this is almost always
    // near the eye - the lip of the slope being stood under, or the hillside
    // the camera has been pushed into - while a sample five hundred yards out
    // only has to be finer than a mountain. Nineteen of them cover the range.
    if (terrainManager) {
        for (float t = 1.0f; t < kReach; t *= 1.4f) {
            const glm::vec3 p = eye + sunDir * t;
            const std::optional<float> ground = terrainManager->getHeightAt(p.x, p.y);
            if (ground && *ground > p.z) return 1.0f;
        }
    }
    return 0.0f;
}

void Renderer::update(float deltaTime) {
    ZoneScopedN("Renderer::update");
    using clock = std::chrono::steady_clock;
    clock::time_point sectionStart = stageSink_ ? clock::now() : clock::time_point{};
    auto section = [&](const char* name) {
        if (!stageSink_) return;
        const auto now = clock::now();
        stageSink_(name, std::chrono::duration<float, std::milli>(now - sectionStart).count());
        sectionStart = now;
    };
    globalTime += deltaTime;
    readFrozenClock();
    // The delta every animated thing below steps by. Zero with the clock
    // frozen, so models, particles, sky and weather hold still while the
    // camera and streaming (which take deltaTime) carry on.
    const float fxDt = frozenClock_ >= 0.0f ? 0.0f : deltaTime;
    if (frozenClock_ >= 0.0f) globalTime = frozenClock_;
    applyEnhancementsToSubsystems();
    runDeferredWorldInitStep(deltaTime);

    // Ease toward it rather than taking it. The sample is a yes or a no, and
    // walking a hill edge across the line to the sun would otherwise snap the
    // flare on and off; a quarter second of travel reads as the sun going
    // behind something.
    {
        const float target = sampleSunOcclusion();
        const float rate = 1.0f - std::exp(-deltaTime / 0.25f);
        sunOcclusion_ += (target - sunOcclusion_) * glm::clamp(rate, 0.0f, 1.0f);
    }
    // "update/before m2" was one 4.4 ms bucket holding everything from here
    // to the M2 launch. These split it; the remainder keeps the old name.
    section("update/pre/sun occlusion");

    auto updateStart = std::chrono::steady_clock::now();
    lastDeltaTime_ = deltaTime;

    if (wmoRenderer) wmoRenderer->resetQueryStats();
    if (m2Renderer) m2Renderer->resetQueryStats();

    if (cameraController) {
        auto cameraStart = std::chrono::steady_clock::now();
        cameraController->update(deltaTime);
        auto cameraEnd = std::chrono::steady_clock::now();
        lastCameraUpdateMs = std::chrono::duration<double, std::milli>(cameraEnd - cameraStart).count();
        if (lastCameraUpdateMs > 50.0) {
            LOG_WARNING("SLOW cameraController->update: ", lastCameraUpdateMs, "ms");
        }

        // Update 3D audio listener position/orientation to match camera.
        // getUp() internally calls getRight() which calls getForward() again,
        // and we ask for getForward() once more on the same line - that's 3
        // independent trig sequences. Cache the basis vectors once.
        if (camera) {
            const glm::vec3 fwd = camera->getForward();
            const glm::vec3 worldUp(0.0f, 0.0f, 1.0f);
            glm::vec3 right = glm::cross(fwd, worldUp);
            float rLen = glm::length(right);
            right = (rLen < 1e-6f) ? glm::vec3(1.0f, 0.0f, 0.0f) : right / rLen;
            glm::vec3 up = glm::cross(right, fwd);
            float uLen = glm::length(up);
            up = (uLen < 1e-6f) ? glm::vec3(0.0f, 0.0f, 1.0f) : up / uLen;
            // At the character, facing where the camera looks: stock 3.3.5's
            // Sound_ListenerAtCharacter (on by default), 2 yd back along the
            // view and 4 yd up (Wow.exe 0x4fa5f0; audio::stock::listenerPosition).
            // With the CVar off, the camera (0x4fa5f0 takes the camera's
            // position when Sound_ListenerAtCharacter is 0).
            const glm::vec3 listenerPos =
                (characterInstanceId > 0 && audio::AudioEngine::instance().isListenerAtCharacter())
                ? audio::stock::listenerPosition(characterPosition, fwd)
                : camera->getPosition();
            audio::AudioEngine::instance().setListenerPosition(listenerPos);
            audio::AudioEngine::instance().setListenerOrientation(fwd, up);
        }
    } else {
        lastCameraUpdateMs = 0.0;
    }

    // Visibility hardening: ensure player instance cannot stay hidden after
    // taxi/camera transitions, but preserve first-person self-hide.
    if (characterRenderer && characterInstanceId > 0 && cameraController) {
        if ((cameraController->isThirdPerson() && !cameraController->isFirstPersonView()) || (animationController_ && animationController_->isTaxiFlight())) {
            characterRenderer->setInstanceVisible(characterInstanceId, true);
        }
    }
    section("update/pre/camera");

    // Resolve WMO containment before weather and ambience consume it. Server
    // weather remains authoritative outdoors, but particles must not follow the
    // camera through a roof into Ironforge or other enclosed WMOs.
    const bool canQueryWmo = (camera && wmoRenderer);
    const glm::vec3 camPos = camera ? camera->getPosition() : glm::vec3(0.0f);
    uint32_t insideWmoId = 0;
    WMORenderer::AreaHit wmoHit;
    const bool insideWmo = canQueryWmo &&
        wmoRenderer->insideWmoArea(camPos.x, camPos.y, camPos.z, &wmoHit);
    if (insideWmo) insideWmoId = wmoHit.modelId;
    // The zone events are not fired from this camera test. They follow the
    // area the player is in (fireZoneEvents, below), which is what the zone
    // text reads: the camera swinging into a wall's group is not a new
    // subzone, and walking into Ironforge is one whatever the camera does.
    //
    // Nor is the weather: the stock client draws it whenever the sky is drawn
    // (docs/re/weather-sky.md 1) - the camera's group is an exterior one, or a
    // portal of its interior opens the outdoors - not whenever the camera is
    // in some group's box. Gadgetzan's and K3's streets are such boxes.
    const bool weatherShown = stock_weather::weatherDrawsFromCamera(
        wmoRenderer && wmoRenderer->portalWalkActive(),
        wmoRenderer && wmoRenderer->outdoorSealed(),
        canQueryWmo && wmoRenderer->isInsideInteriorWMO(camPos.x, camPos.y, camPos.z));
    {
        // Ease characters between sun and interior light over about a second,
        // as the stock client does, rather than flipping on the doorway frame.
        //
        // Decided by where the player stands, not the camera: the camera
        // backing into a wall's group took the player's own light indoors
        // with it. And only a group lit as an interior counts: an INTERIOR
        // group flagged EXTERIOR_LIT (0x40) - Stormwind's Trade District and
        // Silvermoon's streets are hundreds of them - is under the sky.
        WMORenderer::AreaHit unitHit;
        const bool unitInside = wmoRenderer &&
            wmoRenderer->insideWmoArea(characterPosition.x, characterPosition.y,
                                       characterPosition.z + 1.0f, &unitHit);
        const bool interior = unitInside && unitHit.interiorLit;
        if (interior) interiorAmbient_ = wmoRenderer->getModelAmbient(unitHit.modelId);
        const float target = interior ? 1.0f : 0.0f;
        indoorFactor_ += (target - indoorFactor_) * (1.0f - std::exp(-deltaTime / 0.35f));

        // The same group decides the area the player is in, before the
        // lighting, audio and zone text below read it.
        if (unitInside) {
            updateLiveArea(unitHit.wmoId, unitHit.nameSet, unitHit.wmoGroupId);
        } else {
            updateLiveArea(0, 0, 0);
        }
        // IsIndoors / [indoors] (0x71B7F0 -> 0x77FBF0): the player's group
        // lacks the exterior flag 0x8 unless its WMOAreaTable row forces it
        // (0x4 outdoors, 0x2 indoors), else the area's AreaTable flags
        // (0x4000000 outdoors, 0x2000000 indoors). Not the camera's box.
        {
            uint32_t areaFlags = 0, wmoAreaFlags = 0;
            const bool inGroup = liveAreaFromWmo_ && unitInside;
            if (zoneManager && liveAreaValid_) areaFlags = zoneManager->getAreaFlags(liveAreaId_);
            if (zoneManager && inGroup) {
                wmoAreaFlags = zoneManager->resolveWmoArea(unitHit.wmoId, unitHit.nameSet,
                                                           static_cast<int32_t>(unitHit.wmoGroupId)).flags;
            }
            playerIndoors_ = !stock_weather::playerOutdoors(inGroup, unitHit.soundIndoors, areaFlags,
                                                         wmoAreaFlags);
        }
        fireZoneEvents();
        // And whether the minimap shows the building's own map.
        updateMinimapWmo(unitInside ? unitHit.instanceId : 0, unitHit.tightGroupIndex);
    }

    // Update lighting system
    if (lightingManager) {
        const auto* gh = core::Application::getInstance().getGameHandler();
        uint32_t mapId    = gh ? gh->getCurrentMapId() : 0;
        float gameTime    = gh ? gh->getGameTime() : -1.0f;
        // How far the lights lean toward their storm LightParams: the
        // server's weather intensity for any weather it sends, as stock blends
        // the storm set in by intensity rather than switching on rain.
        // The weather preview (a client-only enhancement row) stands in for
        // the server's weather while it is on.
        uint32_t weatherState = 0;
        float weatherGrade = 0.0f;
        effectiveWeather(weatherState, weatherGrade);
        const bool weatherPreviewOn = weatherPreview().active;
        // Times the Weather.dbc row's storm-light scale (0x784850: weather
        // +0x14 x +0x28): 0 on the coloured sandstorms 103 and 105-107.
        const stock_weather::WeatherRow* weatherRow = weatherRowFor(weatherState);
        float stormIntensity =
            ((gh || weatherPreviewOn) && game::weatherKind(weatherState) != game::WeatherKind::NONE)
                ? glm::clamp(weatherGrade, 0.0f, 1.0f) : 0.0f;
        stormIntensity *= easeWeatherLightScale(
            weatherRow && weatherRow->known ? glm::clamp(weatherRow->lightScale, 0.0f, 1.0f) : 1.0f,
            stormIntensity <= 0.0f, deltaTime);
        // Light.dbc's underwater slots while the camera is under the surface -
        // not while the player swims at it, which kept the underwater set up
        // with the camera in open air.
        bool isUnderwater = false;
        uint32_t cameraLiquid = 0;
        float cameraLiquidDepth = 0.0f;
        if (waterRenderer && camera) {
            const glm::vec3 eye = camera->getPosition();
            const auto surface = waterRenderer->getNearestWaterHeightAt(eye.x, eye.y, eye.z, 200.0f);
            // 0x7a0b00: in the liquid while the eye is under its surface plus
            // 0.01 yards (0x9f1968); the eye's own depth is surface - z.
            isUnderwater = surface && eye.z < *surface + 0.01f;
            if (isUnderwater) {
                // Which liquid and how deep, for its own light (magma, slime)
                // and its darkening with depth (ocean).
                if (auto lt = waterRenderer->getWaterTypeAt(eye.x, eye.y)) cameraLiquid = *lt;
                cameraLiquidDepth = std::max(*surface - eye.z, 0.0f);
            }
        }
        lightingManager->setCameraLiquid(cameraLiquid, cameraLiquidDepth);
        // The sound goes under with the picture; see updateZoneAudio.
        cameraUnderwater_ = isUnderwater;
        cameraLiquidType_ = isUnderwater ? cameraLiquid : 0;
        const uint32_t resolvedZoneId = getCurrentZoneId();

        // Light.dbc's death slot while the screen effect asks for it - the
        // ghost's does; a corpse that has not released keeps the world's.
        lightingManager->setPlayerDead(gh && gh->getScreenEffectLightSlot() == game::stock_death::kLightSlotDeath);
        // "Time of day (this client only)": the row's hour in place of the
        // server's clock, for the light only.
        static constexpr float kFixedTimeOfDayHours[] = {
            -1.0f, 6.0f, 9.0f, 12.0f, 15.0f, 19.0f, 21.0f, 23.0f, 0.0f};
        const int fixedHour = enh_.fixedTimeOfDay;
        const float lightTime =
            (fixedHour > 0 && fixedHour < static_cast<int>(std::size(kFixedTimeOfDayHours)))
                ? kFixedTimeOfDayHours[fixedHour]
                : gameTime;
        lightingManager->setHdrScene(vkCtx && vkCtx->isHdrScene());
        lightingManager->update(characterPosition, mapId, resolvedZoneId,
                                lightTime, stormIntensity, isUnderwater);
        lightMapId_ = mapId;
        // The water's Light.dbc colours and alphas follow the same lights.
        if (waterRenderer) waterRenderer->setLiquidLighting(lightingManager->getLightingParams());
        // Window glass (MOMT F_SIDN) glows by the stock night table
        // (0x7a8520), from the same clock.
        // A WMO's own sky model (MOSB) for the camera's group, next frame's.
        lightingManager->setWmoSkybox(wmoRenderer && camera
                                          ? wmoRenderer->skyboxAt(camera->getPosition())
                                          : std::string());
        if (wmoRenderer) {
            wmoRenderer->setSidnNightFactor(
                stock_wmo::sidnNightFactor(lightingManager->getTimeOfDay()));
        }

        // Sync weather visual renderer with game state
        if (weather && (gh || weatherPreviewOn)) {
            const uint32_t wType = weatherState;
            const float wInt = weatherGrade;
            // The server's weather, and only that, in every zone (Duskwood
            // included: stock draws whatever rain the server sends there): it
            // sends fine weather on entering a zone that has none, and this
            // drew a random zone weather over it. Fog and sandstorms have no
            // particles here, so they draw nothing rather than rain.
            // The emitter is the Weather.dbc row's EffectType (0x78d170), not
            // the state's number: 106 is a sand row. 86 stays STORM for the
            // rows that read it; it is drawn as the stock rain.
            Weather::Type wanted = Weather::Type::NONE;
            switch (stock_weather::effectFor(weatherRow, wType)) {
                case stock_weather::WeatherEffect::RAIN:
                    wanted = wType == 86 ? Weather::Type::STORM : Weather::Type::RAIN;
                    break;
                case stock_weather::WeatherEffect::SNOW: wanted = Weather::Type::SNOW; break;
                // The stock sand and its grainy mist (0x784580, 0x78cf20);
                // the "Sandstorms" row adds its haze and wind on top.
                case stock_weather::WeatherEffect::SAND: wanted = Weather::Type::SAND; break;
                default: break;
            }
            if (weatherRow && weatherRow->known) {
                weather->setRowLook(glm::vec3(weatherRow->colour[0], weatherRow->colour[1],
                                              weatherRow->colour[2]),
                                    weatherRow->texture);
            } else {
                weather->setRowLook(glm::vec3(1.0f), std::string());
            }
            weather->followServerWeather(wanted, wInt, deltaTime);
            weather->setEnabled(weatherShown);

            // Lightning flash disabled
            if (lightning) {
                lightning->setEnabled(false);
            }
        } else if (weather) {
            // No game handler (single-player without network) - zone weather only
            weather->updateZoneWeather(getCurrentZoneId(), deltaTime);
            weather->setEnabled(weatherShown);
        }
    }
    section("update/pre/wmo area+lighting");

    // Sync character model position/rotation and animation with follow target
    if (characterInstanceId > 0 && characterRenderer && cameraController) {
        // Drawn where the eased step puts it; characterPosition stays the
        // physics position that movement packets and collision use.
        characterRenderer->setInstancePosition(
            characterInstanceId,
            characterPosition + glm::vec3(0.0f, 0.0f, cameraController->getRenderZOffset()));

        // Movement-facing comes from camera controller and is decoupled from LMB orbit.
        bool taxiFlight = animationController_ && animationController_->isTaxiFlight();
        if (taxiFlight) {
            characterYaw = cameraController->getFacingYaw();
        } else if (cameraController->isMoving() || cameraController->isRightMouseHeld() ||
                   cameraController->isTurningLeft() || cameraController->isTurningRight()) {
            characterYaw = cameraController->getFacingYaw();
        }
        // Nothing else turns the character: not a target, not a fight. The
        // stock client never faces the player toward what it attacks or what
        // hits it; a swing at something behind is refused until the player
        // turns. This used to spin the idle character toward its target
        // whenever it was in combat, which also changed the facing sent to
        // the server.
        // Drawn body yaw and look twist, as the stock 3.3.5a client does it
        // (CGUnit_C update 0x0073DAB0; see animation/body_facing.hpp). The
        // logical facing (characterYaw: what the server and melee arcs use)
        // never changes for strafing. The body springs 45 degrees toward a
        // diagonal and 90 toward a pure strafe (a back diagonal turns the
        // other way so the back leads), and SpineLow and Head twist back up to
        // 45 degrees each, so the chest and face stay on the facing - on the
        // enemy - while the legs run sideways. Standing and turning, the body
        // lags and the twist leads; it catches up once the turning stops.
        {
            const bool mounted = animationController_ && animationController_->isMounted();
            const bool turning = cameraController->isTurningLeft() || cameraController->isTurningRight() ||
                                 cameraController->isRightMouseHeld();
            const uint32_t controls =
                (cameraController->isMovingForward()  ? 0x01u : 0u) |
                (cameraController->isMovingBackward() ? 0x02u : 0u) |
                (cameraController->isStrafingLeft()   ? 0x04u : 0u) |
                (cameraController->isStrafingRight()  ? 0x08u : 0u) |
                (cameraController->isTurningLeft()    ? 0x10u : 0u) |
                (cameraController->isTurningRight()   ? 0x20u : 0u) |
                (cameraController->isRightMouseHeld() ? 0x40u : 0u);
            bodyFacingClock_ += std::max(deltaTime, 0.0f);
            // unit+0xABC is stamped when the input controls change and the
            // player is not turning (CGUnit_C 0x00735FC2, reached from the
            // input-control handlers).
            if (controls != bodyFacingControls_ && !turning) bodyFacingStamp_ = bodyFacingClock_;
            bodyFacingControls_ = controls;

            anim::BodyFacingInput in;
            in.facing = glm::radians(characterYaw);
            in.forward = cameraController->isMovingForward();
            in.backward = cameraController->isMovingBackward();
            in.strafeLeft = cameraController->isStrafingLeft();
            in.strafeRight = cameraController->isStrafingRight();
            in.falling = !cameraController->isGrounded() && !cameraController->isSwimming();
            in.enabled = !taxiFlight && !cameraController->isSwimming() &&
                         !cameraController->isFlyingActive();
            in.turning = turning;
            in.activePlayer = true;
            in.dt = deltaTime;
            in.catchUpElapsed = bodyFacingClock_ - bodyFacingStamp_;
            const anim::BodyFacingResult bf = anim::updateBodyFacing(bodyFacing_, in);
            characterVisualYawDeg_ = glm::degrees(bf.body);
            bodyFacingStep_ = bf.bodyStep;  // the turn-in-place shuffle reads it
            float yawRad = bf.body;
            characterRenderer->setInstanceRotation(characterInstanceId, glm::vec3(0.0f, 0.0f, yawRad));
            // Mounted, the twist belongs to the mount's model (the unit's
            // model in the stock client); the rider sits on it untwisted.
            if (mounted) {
                characterRenderer->setInstanceTorsoYaw(characterInstanceId, 0.0f, 0.0f);
                characterRenderer->setInstanceTorsoYaw(animationController_->getMountInstanceId(),
                                                       bf.spineYaw, bf.headYaw);
            } else {
                characterRenderer->setInstanceTorsoYaw(characterInstanceId, bf.spineYaw, bf.headYaw);
            }
        }

        // Update animation based on movement state (delegated to AnimationController §4.2)
        if (animationController_) {
            animationController_->updateMeleeTimers(deltaTime);
            animationController_->setDeltaTime(deltaTime);
            animationController_->updateCharacterAnimation();
        }
    }
    section("update/pre/player sync");

    // Update terrain streaming
    if (terrainManager && camera) {
        auto terrStart = std::chrono::steady_clock::now();
        terrainManager->update(*camera, deltaTime);
        float terrMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - terrStart).count();
        if (terrMs > 50.0f) {
            LOG_WARNING("SLOW terrainManager->update: ", terrMs, "ms");
        }
        if (stageSink_) {
            // Nested under update/pre/terrain, which already counts them.
            const auto& tp = terrainManager->lastUpdatePhases();
            stageSink_("update/pre/terrain/ready tiles", tp.readyMs);
            stageSink_("update/pre/terrain/unloads", tp.unloadMs);
            stageSink_("update/pre/terrain/clutter", tp.clutterMs);
        }
    }
    section("update/pre/terrain");

    // Update sky system (skybox time, star twinkle, clouds, celestial moon phases)
    if (skySystem) {
        skySystem->update(fxDt);
    }
    if (updateSkyboxLayers() && camera) {
        const glm::mat4 viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();
        if (skyboxModelRenderer_) skyboxModelRenderer_->update(fxDt, camera->getPosition(), viewProj);
        if (skyStarsRenderer_) skyStarsRenderer_->update(fxDt, camera->getPosition(), viewProj);
    }

    // Update weather particles
    if (weather && camera) {
        // Where rain lands, for its splashes: the highest of the terrain and
        // any building floor under the drop (city streets are WMO floors).
        if (!weather->hasGroundQuery()) {
            weather->setGroundQuery([this](float x, float y, float fromZ) -> std::optional<float> {
                std::optional<float> ground;
                if (terrainManager) ground = terrainManager->getHeightAt(x, y);
                if (wmoRenderer) {
                    if (auto floor = wmoRenderer->getFloorHeight(x, y, fromZ)) {
                        ground = ground ? std::max(*ground, *floor) : *floor;
                    }
                }
                return ground;
            });
        }
        // E19: the rain and snow take the cloth's wind (the foliage's,
        // without the indoor cut: the weather is off indoors anyway).
        weather->setWind(enh_.weatherWind ? clothWindStrength_ : 0.0f, globalTime);
        // The weather rows: style, fireflies, snow cover, lightning, breath.
        updateWeatherFx(fxDt);
        // The stock mist and sand blow from the player's heading (0x78d170).
        weather->setPlayer(characterPosition, glm::radians(characterYaw));
        weather->update(*camera, fxDt);
    }

    // Update lightning (storm / heavy rain)
    if (lightning && camera && lightning->isEnabled()) {
        lightning->update(fxDt, *camera);
    }

    // Update swim effects: the stock underwater motes for the liquid the
    // camera is in (LiquidType Flags 0x8 and its particle columns), and the
    // non-stock splashes only when that setting is on.
    if (swimEffects && camera && cameraController && waterRenderer) {
        SwimEffects::MoteLiquid motes;
        if (cameraUnderwater_ && cameraLiquidType_ != 0) {
            const auto p = waterRenderer->particulatesFor(static_cast<uint16_t>(cameraLiquidType_));
            motes = {.show = p.show, .liquidType = cameraLiquidType_, .particleScale = p.scale,
                     .movement = p.movement, .texSlots = p.texSlots, .fogged = p.fogged};
        }
        swimEffects->setCameraLiquid(motes);
        swimEffects->setSplashesEnabled(waterSplashes_);
        swimEffects->setInsectsEnabled(enh_.waterInsects);
        swimEffects->update(*camera, *cameraController, *waterRenderer, fxDt);
    }

    // Surface disturbance the character leaves in the water. The droplet spray
    // is thrown by SwimEffects above; this is the froth on the surface itself,
    // which has to come from the water shader to move and light with the water.
    if (waterRenderer && !waterSplashes_) waterRenderer->clearWake();
    if (waterRenderer && camera && cameraController && waterSplashes_) {
        glm::vec3 charPos = camera->getPosition();
        const glm::vec3* followTarget = cameraController->getFollowTarget();
        if (cameraController->isThirdPerson() && followTarget) {
            charPos = *followTarget;
        }

        const bool swimming = cameraController->isSwimming();
        float intensity = 0.0f;
        bool wading = false;

        if (cameraController->isMoving()) {
            if (auto waterH = waterRenderer->getWaterHeightAt(charPos.x, charPos.y)) {
                if (swimming) {
                    // A wake only exists where the swimmer meets the surface -
                    // diving deep leaves the surface undisturbed.
                    const float below = *waterH - charPos.z;
                    intensity = glm::clamp(1.0f - (below - 0.6f) / 1.4f, 0.0f, 1.0f);
                } else {
                    // Ankle-deep barely marks the water; thigh-deep throws the
                    // most, past which the character starts swimming anyway.
                    const float depth = *waterH - charPos.z;
                    if (depth > 0.03f && depth < 1.8f) {
                        wading = true;
                        intensity = glm::clamp(depth / 0.6f, 0.3f, 1.0f);
                    }
                }
            }
        }

        const float yawRad = glm::radians(cameraController->getYaw());
        const glm::vec2 travelDir(std::sin(yawRad), -std::cos(yawRad));
        waterRenderer->updateWake(fxDt, glm::vec2(charPos.x, charPos.y),
                                  travelDir, intensity, wading);
    }

    // Update level-up effect
    if (levelUpEffect) {
        levelUpEffect->update(fxDt);
    }
    section("update/pre/sky+weather+water+fx");
    // Update transient spell visual instances (delegated to SpellVisualSystem §4.4)
    if (spellVisualSystem_) spellVisualSystem_->update(fxDt);
    section("update/pre/spell visuals");


    // Launch M2 doodad animation on background thread (overlaps with character animation + audio)
    std::future<void> m2AnimFuture;
    bool m2AnimLaunched = false;
    if (m2Renderer && camera) {
        float m2DeltaTime = fxDt;
        glm::vec3 m2CamPos = camera->getPosition();
        glm::mat4 m2ViewProj = camera->getProjectionMatrix() * camera->getViewMatrix();
        m2AnimFuture = core::ThreadPool::frameWorkers().submit(
            [this, m2DeltaTime, m2CamPos, m2ViewProj]() {
                m2Renderer->update(m2DeltaTime, m2CamPos, m2ViewProj);
            });
        m2AnimLaunched = true;
    }

    section("update/before m2");
    // Update character animations (runs in parallel with M2 animation above)
    if (characterRenderer && camera) {
        // What this frame will draw from, so units nobody can see - nor see
        // the shadow of - keep their clocks but skip their skeletons.
        characterRenderer->setAnimCullView(camera->getViewProjectionMatrix(),
                                           shadowsEnabled ? shadowRayDirection() : glm::vec3(0.0f));
        characterRenderer->setUnitAmbientArea(getCurrentAreaId());
        // Capes, tails and hair (E7) take the foliage's wind: its direction
        // (m2.vert's kWindDir) and its eased, outdoor strength, about 5 yd/s
        // of air at the usual breeze. Other units' smoothing (E10) leaves the
        // player's own character, native form and mount alone.
        {
            constexpr float kClothWindSpeed = 5.0f;
            const glm::vec2 windDir = glm::normalize(glm::vec2(0.80f, 0.60f));
            const glm::vec3 wind(windDir * (windStrength_ * kClothWindSpeed), 0.0f);
            characterRenderer->setMotionEnhancements(
                enh_.secondaryMotion, enh_.secondaryMotionStrength, wind, enh_.remoteSmoothing, characterInstanceId,
                nativeCharacterInstanceId_,
                animationController_ ? animationController_->getMountInstanceId() : 0u);
            characterRenderer->setPoseBlend(enh_.poseBlend);
        }
        characterRenderer->update(fxDt, camera->getPosition());
    }
    updateUnitBlobs();
    section("update/characters");

    // Update AudioEngine (cleanup finished sounds, etc.)
    audio::AudioEngine::instance().update(deltaTime);
    section("update/audio engine");

    // M2Renderer::update may rebuild the instance spatial index when streaming
    // marked it dirty. Footprint spawning below performs an M2 floor query and
    // traverses that same index. Joining only after footsteps allowed the main
    // thread to walk unordered_map nodes while the animation worker cleared and
    // rebuilt them, producing a SIGSEGV in M2Renderer::gatherCandidates. Keep
    // the useful overlap with character animation and audio above, but finish
    // structural M2 work before any main-thread collision query.
    if (m2AnimLaunched) {
        try { m2AnimFuture.get(); }
        catch (const std::exception& e) { LOG_ERROR("M2 animation worker: ", e.what()); }
        m2AnimLaunched = false;
    }

    section("update/m2 join");
    // The emitters of the models CharacterRenderer draws (creatures, players,
    // their weapons and armour) and the weapon glows: posed from this frame's
    // skeletons, now that M2Renderer's update is done with its instances,
    // then stepped (stock steps every CM2Model's emitters alike).
    if (characterRenderer && m2Renderer && camera) {
        characterRenderer->poseModelEmitters();
        m2Renderer->updatePosedEmitters(fxDt);
    }
    section("update/model emitters");
    // Footsteps: age visual prints, then let authored footfall events add new ones.
    if (footprintRenderer) footprintRenderer->update(fxDt);
    if (animationController_) animationController_->updateFootsteps(deltaTime);
    // The dust from this frame's footfalls, and the corpses' slopes: both ask
    // the floors, so both wait for the M2 update above to have joined.
    updateFootfallDust(fxDt);
    updateCorpseSettle(fxDt);
    updateUnitIndoorLight(fxDt);
    updateBounceGrid(fxDt > 0.0f ? fxDt : deltaTime);

    // Activity SFX + mount ambient sounds: delegated to AnimationController (§4.2)
    if (animationController_) animationController_->updateSfxState(deltaTime);
    section("update/footfall+sfx");

    // Ambient environmental sounds + zone/music transitions (delegated to AudioCoordinator)
    if (audioCoordinator_) {
        audio::ZoneAudioContext zctx;
        zctx.deltaTime = deltaTime;
        zctx.cameraPosition = camPos;
        zctx.isSwimming = cameraController ? cameraController->isSwimming() : false;
        zctx.insideWmo = insideWmo;
        zctx.insideWmoId = insideWmoId;
        if (insideWmo) {
            zctx.wmoRootId = wmoHit.wmoId;
            zctx.wmoGroupId = static_cast<int32_t>(wmoHit.wmoGroupId);
            zctx.wmoNameSet = wmoHit.nameSet;
            zctx.wmoInterior = wmoHit.interior;
            zctx.wmoSoundIndoors = wmoHit.soundIndoors;
        }
        zctx.cameraUnderwater = cameraUnderwater_;
        if (weather) {
            auto wt = weather->getWeatherType();
            if (wt == Weather::Type::RAIN)       zctx.weatherType = 1;
            else if (wt == Weather::Type::SNOW)  zctx.weatherType = 2;
            else if (wt == Weather::Type::STORM) zctx.weatherType = 3;
            zctx.weatherIntensity = weather->getIntensity();
        }
        if (lightingManager) {
            zctx.gameTimeHours = lightingManager->getVisualTimeOfDayHours();
        }
        if (terrainManager) {
            auto tile = terrainManager->getCurrentTile();
            zctx.tileX = tile.x;
            zctx.tileY = tile.y;
            zctx.hasTile = true;
        }
        // Use the precise MCNK area classification when available; this avoids
        // stale server world-state zones and whole-ADT ambiguity at river banks.
        zctx.serverZoneId = getCurrentZoneId();
        // The area the zone text names: inside Ironforge, Ironforge's music and
        // ambience rather than the Dun Morogh chunk's under it.
        zctx.areaId = getCurrentAreaId();
        zctx.zoneManager = zoneManager.get();
        // For SoundEmitters.dbc, which places its loops per map.
        if (terrainManager) {
            if (const auto* gh = core::Application::getInstance().getGameHandler()) {
                zctx.mapId = gh->getCurrentMapId();
            }
        }
        {
            // The server's weather, or the preview's (a client-only row).
            float grade = 0.0f;
            effectiveWeather(zctx.weatherState, grade);
        }
        zctx.rainIndoors = enh_.rainIndoors;
        audioCoordinator_->updateZoneAudio(zctx);
    }
    section("update/zone audio");

    // Update performance HUD
    if (performanceHUD) {
        performanceHUD->update(deltaTime);
    }

    // Periodic cache hygiene: drop model GPU data no longer referenced by active instances.
    static float modelCleanupTimer = 0.0f;
    modelCleanupTimer += deltaTime;
    if (modelCleanupTimer >= 5.0f) {
        if (wmoRenderer) {
            wmoRenderer->cleanupUnusedModels();
        }
        if (m2Renderer) {
            m2Renderer->cleanupUnusedModels();
        }
        modelCleanupTimer = 0.0f;
    }
    section("update/model cleanup");

    auto updateEnd = std::chrono::steady_clock::now();
    lastUpdateMs = std::chrono::duration<double, std::milli>(updateEnd - updateStart).count();
}

void Renderer::runDeferredWorldInitStep(float deltaTime) {
    if (!deferredWorldInitEnabled_ || !deferredWorldInitPending_ || !cachedAssetManager) return;
    if (deferredWorldInitCooldown_ > 0.0f) {
        deferredWorldInitCooldown_ = std::max(0.0f, deferredWorldInitCooldown_ - deltaTime);
        if (deferredWorldInitCooldown_ > 0.0f) return;
    }

    switch (deferredWorldInitStage_) {
        case 0:
            if (audioCoordinator_->getAmbientSoundManager()) {
                audioCoordinator_->getAmbientSoundManager()->initialize(cachedAssetManager);
            }
            if (terrainManager && audioCoordinator_->getAmbientSoundManager()) {
                terrainManager->setAmbientSoundManager(audioCoordinator_->getAmbientSoundManager());
            }
            break;
        case 1:
            if (audioCoordinator_->getUiSoundManager()) audioCoordinator_->getUiSoundManager()->initialize(cachedAssetManager);
            break;
        case 2:
            if (audioCoordinator_->getCombatSoundManager()) audioCoordinator_->getCombatSoundManager()->initialize(cachedAssetManager);
            break;
        case 3:
            if (audioCoordinator_->getSpellSoundManager()) audioCoordinator_->getSpellSoundManager()->initialize(cachedAssetManager);
            break;
        case 4:
            if (audioCoordinator_->getMovementSoundManager()) audioCoordinator_->getMovementSoundManager()->initialize(cachedAssetManager);
            break;
        case 5:
            if (footprintRenderer && !footprintRenderer->initialize(this, vkCtx, perFrameSetLayout, cachedAssetManager))
                LOG_WARNING("Footprint renderer re-init failed (non-fatal)");
            break;
        default:
            deferredWorldInitPending_ = false;
            return;
    }

    deferredWorldInitStage_++;
    deferredWorldInitCooldown_ = 0.12f;
}

void Renderer::setSelectionCircle(const glm::vec3& pos, float radius, const glm::vec3& color,
                                  float alpha, float attackPulse) {
    if (overlaySystem_) overlaySystem_->setSelectionCircle(pos, radius, color, alpha, attackPulse);
}

void Renderer::clearSelectionCircle() {
    if (overlaySystem_) overlaySystem_->clearSelectionCircle();
}

void Renderer::setSpellTargetDecal(const glm::vec3& center, float halfWidth, bool acceptable) {
    if (overlaySystem_) overlaySystem_->setSpellTargetDecal(center, halfWidth, acceptable);
}

void Renderer::clearSpellTargetDecal() {
    if (overlaySystem_) overlaySystem_->clearSpellTargetDecal();
}

// ========================= PostProcessPipeline delegation stubs (§4.3) =========================

PostProcessPipeline* Renderer::getPostProcessPipeline() const {
    return postProcessPipeline_.get();
}

void Renderer::setFSREnabled(bool enabled) {
    if (!postProcessPipeline_) return;
    auto req = postProcessPipeline_->setFSREnabled(enabled);
    if (req.requested) {
        pendingMsaaSamples_ = req.samples;
        msaaChangePending_ = true;
    }
}
void Renderer::setFSR2Enabled(bool enabled) {
    if (!postProcessPipeline_) return;
    // The FSR2 compute shaders need shaderStorageImageWriteWithoutFormat and
    // shaderInt16. A device without them used to be refused at startup; now it
    // starts, so the setting has to refuse instead.
    if (enabled) {
        VkContext* ctx = VkContext::globalInstance();
        if (ctx && !ctx->areFsr2ComputeFeaturesSupported()) {
            LOG_WARNING("FSR2 needs shaderStorageImageWriteWithoutFormat and shaderInt16, "
                        "which this device does not support - leaving it off");
            return;
        }
    }
    auto req = postProcessPipeline_->setFSR2Enabled(enabled, camera.get());
    if (req.requested) {
        pendingMsaaSamples_ = req.samples;
        msaaChangePending_ = true;
    }
    // A pending multisampling change loaded before the upscaler only has to
    // yield on a device that cannot resolve depth.
    if (enabled && msaaChangePending_ && pendingMsaaSamples_ > VK_SAMPLE_COUNT_1_BIT &&
        postProcessPipeline_->isFsr2BlockingMsaa()) {
        pendingMsaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    }
}
void Renderer::setGrassEnabled(bool enabled) {
    if (enabled == grassEnabled_) return;
    grassEnabled_ = enabled;
    if (!enabled && grassRenderer_) {
        // Empty the population rather than just skipping the draw: a blade
        // count of zero is what stops the cull dispatching too, and the buffer
        // it was holding is the point of turning it off.
        grassRenderer_->setPopulation(nullptr, 0);
    }
    grassBuilder_ = pipeline::GrassPopulationBuilder{};
    grassWindowValid_ = false;
}

void Renderer::setGrassScales(float density, float height) {
    // Up to triple. Density past 1 costs generation time and can meet the
    // blade cap, which thins the whole window rather than cutting a side off
    // it; height past 1 costs nothing.
    const float d = glm::clamp(density, 0.0f, 3.0f);
    const float h = glm::clamp(height, 0.5f, 3.0f);
    if (d == grassDensityScale_ && h == grassHeightScale_) return;
    grassDensityScale_ = d;
    grassHeightScale_ = h;
    // The live population was generated with the old numbers, so it has to go
    // rather than wait for the player to walk far enough to be rebuilt - and
    // so does a build in progress, which froze them when it began.
    grassBuilder_ = pipeline::GrassPopulationBuilder{};
    grassWindowValid_ = false;
}

void Renderer::setGrassDistance(float yards) {
    // The slider's own range. The ceiling is generosity rather than promise:
    // a 0.4 yard blade is under a pixel tall past a couple of hundred yards,
    // so the far end of a long range is carried by the taller seeded stems
    // and by density, not by every blade surviving.
    const float d = glm::clamp(yards, 30.0f, 2000.0f);
    if (d == grassDistance_) return;
    grassDistance_ = d;
    if (grassRenderer_) grassRenderer_->setCullDistance(d);
    // Regenerate: the window radius and the falloff are both functions of the
    // distance, so neither the live population nor a build in progress
    // matches it any more.
    grassBuilder_ = pipeline::GrassPopulationBuilder{};
    grassWindowValid_ = false;
}

uint32_t Renderer::grassProfileFor(uint32_t effectId, uint32_t areaId) {
    // The biome table, read once. Its absence is fine - grass then looks the
    // way the effect data alone says - but a file that exists and does not
    // parse is an authoring error worth a line.
    if (!grassBiomesLoaded_) {
        grassBiomesLoaded_ = true;
        std::ifstream f("assets/grass_biomes.json");
        if (f) {
            const std::string text((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
            std::string parseError;
            grassBiomes_ = pipeline::loadGrassBiomes(text, parseError);
            if (!parseError.empty()) {
                LOG_WARNING("Grass biomes: ", parseError);
            } else {
                LOG_INFO("Grass biomes: ", grassBiomes_.size(), " regions");
            }
        }
    }

    // Which biome this ground belongs to, memoised per area: the zone walk
    // costs a few map lookups and this is called for every sampled blade.
    uint32_t biomeIdx = 0;
    const auto cachedBiome = grassBiomeForArea_.find(areaId);
    if (cachedBiome != grassBiomeForArea_.end()) {
        biomeIdx = cachedBiome->second;
    } else {
        const uint32_t zoneId =
            zoneManager ? zoneManager->resolveAreaZoneId(areaId) : areaId;
        biomeIdx = grassBiomes_.findFor(areaId, zoneId);
        grassBiomeForArea_[areaId] = biomeIdx;
    }

    // Derived once per (biome, effect) and kept. The table is uploaded whole
    // whenever it grows, which happens a handful of times as the player
    // crosses into ground they have not stood on before and then stops.
    const uint64_t key = (static_cast<uint64_t>(biomeIdx) << 32) | effectId;
    const auto known = grassProfileIndex_.find(key);
    if (known != grassProfileIndex_.end()) return known->second;

    std::vector<std::string> models;
    std::vector<uint32_t> weights;
    if (terrainManager) terrainManager->getGroundEffectDoodads(effectId, models, weights);

    pipeline::GrassProfile profile = pipeline::deriveProfile(models, weights);
    if (const auto* biome = grassBiomes_.biome(biomeIdx)) {
        biome->override_.apply(profile);
    }

    uint32_t index = 0;
    if (grassProfiles_.size() < GrassRenderer::kMaxProfiles) {
        index = static_cast<uint32_t>(grassProfiles_.size());
        grassProfiles_.push_back(profile);
    }
    grassProfileIndex_[key] = index;
    return index;
}

void Renderer::updateGrassPopulation() {
    if (grassRenderer_) grassRenderer_->pollPopulation();
    if (!grassEnabled_) return;
    if (!grassRenderer_ || !grassRenderer_->isReady() || !terrainManager) return;

    // How far out blades are generated, and how far the player may walk before
    // the window is rebuilt. Both follow the grass distance setting. The
    // margin between them is the invariant: at its stalest the window centre
    // lags the player by a full step, so the grass ahead reaches
    // (radius - step) - and that must clear the cull distance, or the field
    // ends at a visible edge on one side of the player and pops forward on
    // every rebuild. The first numbers this had put the window *inside* the
    // cull distance, which is exactly how it looked; building the radius as
    // distance + step keeps the margin by construction at every distance the
    // slider allows.
    // Constant, not scaled by the distance setting: the octave walk made
    // rebuild cost grow with the log of the window rather than its area, so
    // frequent small steps beat rare big ones - and the slack every ring
    // must carry to cover a stale centre is the step, which at three
    // hundred yards would have meant full-density generation far past the
    // near field.
    const float rebuildStep = 24.0f;
    const float windowRadius = grassDistance_ + rebuildStep;
    const float rebuildStepSq = rebuildStep * rebuildStep;

    const glm::vec3 center = characterPosition;
    if (glm::dot(center, center) <= 0.0f) return;  // no character yet

    // Whether a new build has to start. While one is running the comparison
    // is against its centre rather than the live window's, so walking far
    // during a long build restarts it around where the player now is instead
    // of finishing a window they have already left.
    bool needBegin;
    if (grassBuilder_.active()) {
        const glm::vec2 moved(center.x - grassBuildCenter_.x, center.y - grassBuildCenter_.y);
        needBegin = glm::dot(moved, moved) >= rebuildStepSq;
    } else if (grassWindowValid_) {
        const glm::vec2 moved(center.x - grassWindowCenter_.x, center.y - grassWindowCenter_.y);
        needBegin = glm::dot(moved, moved) >= rebuildStepSq;
    } else {
        needBegin = true;
    }
    if (!needBegin && !grassBuilder_.active()) return;

    // One chunk decoded at a time. populateArea walks cells in world order, so
    // consecutive samples land in the same chunk and this memo almost always
    // hits - without it every candidate would decode the chunk's alpha maps
    // again, which is four kilobytes a layer for each of tens of thousands.
    const pipeline::MapChunk* cached = nullptr;
    const TerrainTile* cachedTile = nullptr;
    pipeline::ChunkGrassContext context;
    auto densityFor = [this](uint32_t effectId) {
        return terrainManager->getGroundEffectDensity(effectId);
    };
    // One-shot diagnostic: which link in the chain is empty. A population of
    // zero can mean no chunk under the sample, no effect id on its layers, or
    // an effect the table grows nothing for, and the three are indistinguish-
    // able from the outside.
    static bool reportedChain = false;
    size_t noChunk = 0;
    size_t sampled = 0;

    // A chunk is 33 yards across and candidates are a third of a yard apart, so
    // a hundred consecutive samples land in the chunk the last one did. Testing
    // that chunk before searching for another is what takes this off the frame:
    // findChunkAt is a tile lookup and a 3x3 probe, and running it per sample
    // was most of the cost.
    constexpr float kUnitSize = core::coords::TILE_SIZE / 16.0f / 8.0f;

    auto sampler = [&](float wx, float wy) -> pipeline::GrassSuitability {
        float fracX = 0.0f;
        float fracY = 0.0f;
        ++sampled;
        const pipeline::MapChunk* chunk = nullptr;
        if (cached && pipeline::TerrainMeshGenerator::chunkFractionsAt(
                          cached->position, wx, wy, kUnitSize, fracX, fracY)) {
            chunk = cached;
        } else {
            chunk = terrainManager->findChunkAt(wx, wy, fracX, fracY, &cachedTile);
        }
        if (!chunk) { ++noChunk; return {}; }
        if (chunk != cached) {
            // The texture names live on the tile, which pipeline code never
            // sees, so they arrive as a lookup. build() uses them to keep
            // grass off made surfaces; nothing else can, because a road's
            // ground effect is as real as a meadow's.
            const TerrainTile* tile = cachedTile;
            auto textureNameFor = [tile](uint32_t texId) -> std::string {
                if (!tile || texId >= tile->terrain.textures.size()) return {};
                return tile->terrain.textures[texId];
            };
            context = pipeline::ChunkGrassContext{};
            context.build(*chunk, densityFor, textureNameFor);

            // Reported for the first few chunks that carry a made surface, at
            // a level the default log actually keeps. Grass on cobblestone has
            // survived two fixes that each looked right, so this is the code
            // saying what it sees rather than me saying what it should - and
            // aimed at the chunks in question rather than at whichever chunk
            // happened to be sampled first.
            static int reportedRoadChunks = 0;
            bool anyRoad = false;
            for (size_t i = 0; i < std::min<size_t>(chunk->layers.size(), 4); ++i) {
                anyRoad = anyRoad ||
                          pipeline::isRoadLikeTexture(textureNameFor(chunk->layers[i].textureId));
            }
            if (anyRoad && reportedRoadChunks < 4) {
                ++reportedRoadChunks;
                std::string detail;
                for (size_t i = 0; i < std::min<size_t>(chunk->layers.size(), 4); ++i) {
                    const std::string name = textureNameFor(chunk->layers[i].textureId);
                    detail += "\n    [" + std::to_string(i) + "] tex=" +
                              std::to_string(chunk->layers[i].textureId) + " '" +
                              (name.empty() ? std::string("<no name>") : name) + "' effect=" +
                              std::to_string(chunk->layers[i].effectId) +
                              (pipeline::isRoadLikeTexture(name) ? " ROAD" : "") +
                              (context.grows[i] ? " grows" : " bare");
                }
                LOG_WARNING("Grass road chunk: ", chunk->layers.size(), " layers", detail);
            }

            // Any water over this chunk. Sampled once here rather than per
            // blade: a pond's surface is flat, and three hundred thousand
            // height queries a rebuild is not.
            if (waterRenderer) {
                const float cx = chunk->position[0] - 4.0f * kUnitSize;
                const float cy = chunk->position[1] - 4.0f * kUnitSize;
                if (const auto level = waterRenderer->getWaterHeightAt(cx, cy)) {
                    context.waterHeight = *level;
                    context.hasWater = true;
                }
            }

            // The tones the ground is painted in, for colouring the blades.
            const size_t n = std::min<size_t>(chunk->layers.size(), 4);
            for (size_t i = 0; i < n; ++i) {
                const std::string name = textureNameFor(chunk->layers[i].textureId);
                if (name.empty()) continue;
                const auto tones = terrainManager->getTerrainTextureTones(name);
                context.layerShadow[i] = tones.shadow;
                context.layerHighlight[i] = tones.highlight;
                context.hasLayerColors = true;
            }
            cached = chunk;
            if (!reportedChain) {
                reportedChain = true;
                std::string layers;
                for (size_t i = 0; i < chunk->layers.size(); ++i) {
                    layers += " [" + std::to_string(i) +
                              "] effect=" + std::to_string(chunk->layers[i].effectId) +
                              " density=" +
                              std::to_string(terrainManager->getGroundEffectDensity(
                                  chunk->layers[i].effectId));
                }
                const auto fit = pipeline::evaluateGrass(context, *chunk, fracX, fracY);
                LOG_INFO("Grass chain: chunk at (", wx, ",", wy, ") frac=(", fracX, ",", fracY,
                         ") layers=", chunk->layers.size(), layers,
                         " -> suitability=", fit.suitability, " slope=", fit.slope);
            }
        }
        auto fit = pipeline::evaluateGrass(context, *chunk, fracX, fracY);
        // The terrain's verge eased toward roads; the placed world eases the
        // same number toward walls and wagon wheels.
        if (fit.suitability > 0.0f) {
            fit.wildness = std::min(fit.wildness, grassClearing_.wildness(wx, wy));
        }
        return fit;
    };

    pipeline::GrassPopulationParams params;
    params.densityScale = grassDensityScale_;
    params.baseHeight *= grassHeightScale_;
    // Full density out to the base range; past it the lattice coarsens by
    // octaves, so the long ranges the slider allows cost blades - and build
    // time - by the log of the radius rather than by its area. The slack
    // keeps blades in the buffer a rebuild step before they fade in, so a
    // ride toward them never outruns the window.
    params.fullDensityRadius = GrassRenderer::kCullDistance;
    params.ringSlack = rebuildStep;

    // Zero means the player turned grass off. Said out loud once: a density of
    // zero produces an empty population through a chain that is otherwise
    // working perfectly, and nothing about that looks like a setting rather
    // than a fault.
    if (params.densityScale <= 0.0f) {
        static bool warnedDisabled = false;
        if (!warnedDisabled) {
            warnedDisabled = true;
            LOG_WARNING("Grass: density is 0, so no grass will grow. "
                        "Raise Grass Density in Settings > Graphics to see any.");
        }
        grassBuilder_ = pipeline::GrassPopulationBuilder{};
        grassRenderer_->setPopulation(nullptr, 0);
        grassWindowCenter_ = center;
        grassWindowValid_ = true;
        return;
    }

    if (needBegin) {
        // Clearings around the built and the placed, gathered once per
        // rebuild: the terrain's own verges handle roads and paths, but a
        // building is a placement, and only its renderer knows where it is.
        std::vector<pipeline::GrassClearingSource> clearings;
        const float winMinX = center.x - windowRadius;
        const float winMaxX = center.x + windowRadius;
        const float winMinY = center.y - windowRadius;
        const float winMaxY = center.y + windowRadius;
        if (wmoRenderer) {
            wmoRenderer->collectGrassClearings(winMinX, winMinY, winMaxX, winMaxY, clearings);
        }
        if (m2Renderer) {
            m2Renderer->collectGrassClearings(winMinX, winMinY, winMaxX, winMaxY, clearings);
        }
        grassClearing_.build(std::move(clearings), winMinX, winMinY, winMaxX, winMaxY);

        grassBuilder_.begin(center.x, center.y, windowRadius, params,
                            GrassRenderer::kMaxBlades);
        grassBuildCenter_ = center;
    }

    // WOWEE_GRASS_DEBUG=1: everything about the ground the player is standing
    // on. Screenshots cannot say whether a stretch of cobble resolved to a
    // road layer that was suppressed, a dirt layer that was not, or a grass
    // layer underneath showing through - and those want three different fixes.
    // Once per build, not per slice.
    if (needBegin && rendering::envFlagEnabled("WOWEE_GRASS_DEBUG")) {
        float fx = 0.0f;
        float fy = 0.0f;
        const TerrainTile* tile = nullptr;
        if (const pipeline::MapChunk* here =
                terrainManager->findChunkAt(center.x, center.y, fx, fy, &tile)) {
            auto nameFor = [tile](uint32_t texId) -> std::string {
                if (!tile || texId >= tile->terrain.textures.size()) return {};
                return tile->terrain.textures[texId];
            };
            pipeline::ChunkGrassContext ctx;
            ctx.build(*here, densityFor, nameFor);
            std::string report;
            for (size_t i = 0; i < std::min<size_t>(here->layers.size(), 4); ++i) {
                const uint32_t texId = here->layers[i].textureId;
                const std::string name = nameFor(texId).empty() ? std::string("<none>")
                                                                : nameFor(texId);
                const bool road = pipeline::isRoadLikeTexture(name);
                report += "\n    [" + std::to_string(i) + "] effect=" +
                          std::to_string(here->layers[i].effectId) + " density=" +
                          std::to_string(terrainManager->getGroundEffectDensity(
                              here->layers[i].effectId)) +
                          (road ? " ROAD-SUPPRESSED " : " ") + name;
            }
            const auto fit = pipeline::evaluateGrass(ctx, *here, fx, fy);
            // The no-effect mask in full, plus the player's own quad. Stand on
            // a farm row or an abbey floor and this is the line that says
            // whether the data flags it and whether the bit order reads it
            // the right way round.
            const int qx = std::clamp(static_cast<int>(fx), 0, 7);
            const int qy = std::clamp(static_cast<int>(fy), 0, 7);
            LOG_INFO("Grass under player: frac=(", fx, ",", fy, ") suitability=",
                     fit.suitability, " growsAnything=", ctx.growsAnything ? 1 : 0,
                     " noEffectDoodad=0x", std::hex, here->noEffectDoodad, std::dec,
                     " thisQuad=", here->isEffectDisabled(qy, qx) ? "no-grow" : "grows",
                     " mappedLayer=", here->effectLayerFor(qy, qx),
                     report);
        }
    }

    const size_t profilesBefore = grassProfiles_.size();
    auto profileFor = [this](uint32_t effectId, uint32_t areaId) {
        pipeline::GrassProfileRef ref;
        ref.index = grassProfileFor(effectId, areaId);
        const auto& p = grassProfiles_[ref.index];
        ref.heightScale = p.heightScale;
        ref.widthScale = p.widthScale;
        ref.densityScale = p.densityScale;
        return ref;
    };

    // A bounded number of lattice blocks per frame, so a rebuild costs the
    // same per frame at any window size - a large one just takes more frames
    // and the field grows in when it lands. Since the octave walk, nearly
    // every block surveyed becomes a terrain sample, so the budget is set by
    // the cost of sampling rather than of walking: small enough to stay off
    // the frame, large enough that a default window lands in a few slices.
    //
    // A fixed 64000 cells cost 17-32 ms a frame on Jered's laptop in
    // Goldshire - a second of hitching each time the field rebuilt. The slice
    // is now sized by time instead: after each one the cell count is set to
    // what kGrassSliceMs would have bought at the rate just measured, so the
    // field takes more frames to land and no frame pays more than about that.
    constexpr double kGrassSliceMs = 4.0;
    constexpr size_t kMinCellsPerSlice = 1000;
    constexpr size_t kMaxCellsPerSlice = 64000;
    const size_t cells = grassCellsPerSlice_;
    const auto started = std::chrono::steady_clock::now();
    const bool done = grassBuilder_.step(sampler, profileFor, cells, kGrassSliceMs);
    const auto stepped = std::chrono::steady_clock::now();

    // Only when it grew, and after every slice rather than at the end: the
    // shaders index this by blade, so it has to reach the device before the
    // population that refers to it does.
    if (grassProfiles_.size() != profilesBefore) {
        grassRenderer_->setProfiles(grassProfiles_);
    }
    const double generateMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (generateMs > 8.0) {
        LOG_INFO("Grass slow slice: ", generateMs, " ms for ", cells, " cells (step ",
                 std::chrono::duration<double, std::milli>(stepped - started).count(), " ms, profiles ",
                 grassProfiles_.size() - profilesBefore, " new)");
    }
    grassWorstGenerateMs_ = std::max(grassWorstGenerateMs_, generateMs);
    if (generateMs > 0.05) {
        // Down at once, up by at most a quarter a frame: the cost of a cell
        // varies several times over across a window (bare road against
        // dense meadow), and a rate measured on cheap cells overshot on the
        // next dense ones.
        const double rate = static_cast<double>(cells) / generateMs;
        const size_t want = static_cast<size_t>(rate * kGrassSliceMs);
        grassCellsPerSlice_ = std::clamp(std::min(want, cells + cells / 4), kMinCellsPerSlice,
                                         kMaxCellsPerSlice);
    }
    if (!done) return;

    std::vector<pipeline::GrassBladeSample>& blades = grassBuilder_.blades();
    const bool complete = grassBuilder_.complete();
    if (!complete) {
        // The window met the blade cap. The generator thins uniformly when it
        // can see this coming, so meeting it anyway means the density slider
        // and the terrain conspired past the estimate - worth a line, because
        // the visible symptom is a field that ends early on its north side.
        LOG_WARNING("Grass population truncated at ", blades.size(),
                    " blades; lower Grass Density or Grass Distance");
    }

    // WOWEE_GRASS_DUMP=1: write the first generated population to a CSV so
    // the field's actual shape around the player can be analysed offline. The
    // cull reports a stable 2-3x more grass kept looking one way than the
    // other, which is a claim about where blades are, and this answers it.
    static const bool dumpPopulation = rendering::envFlagEnabled("WOWEE_GRASS_DUMP");
    if (dumpPopulation) {
        static bool dumped = false;
        if (!dumped && !blades.empty()) {
            dumped = true;
            std::ofstream out("grass_population.csv");
            out << "# player," << center.x << "," << center.y << "," << center.z << "\n";
            for (const auto& b : blades) {
                out << b.x << "," << b.y << "," << b.z << "," << b.height << "\n";
            }
            LOG_INFO("Grass population dumped: ", blades.size(), " blades");
        }
    }

    // Packed and staged on a worker; it lands in a later frame
    // (pollPopulation at the top of this function).
    const size_t populationCount = blades.size();
    grassRenderer_->setPopulationAsync(std::move(blades));
    // The build's centre, not where the player stands now: they may have
    // walked most of a rebuild step while the window generated, and measuring
    // future movement from here rather than from them is what keeps the
    // stale-window margin honest.
    grassWindowCenter_ = grassBuildCenter_;
    grassWindowValid_ = true;
    ++grassRebuilds_;
    grassLastCount_ = populationCount;

    const double totalMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (!blades.empty()) {
        float minZ = blades[0].z, maxZ = blades[0].z;
        float minH = blades[0].height, maxH = blades[0].height;
        for (const auto& b : blades) {
            minZ = std::min(minZ, b.z); maxZ = std::max(maxZ, b.z);
            minH = std::min(minH, b.height); maxH = std::max(maxH, b.height);
        }
        LOG_INFO("Grass extent: player=(", center.x, ",", center.y, ",", center.z,
                 ") bladeZ=[", minZ, ",", maxZ, "] height=[", minH, ",", maxH,
                 "] first=(", blades[0].x, ",", blades[0].y, ",", blades[0].z, ")");
    }
    if (blades.empty()) {
        LOG_INFO("Grass population empty: ", sampled, " samples, ", noChunk,
                 " with no chunk under them");
    }
    LOG_INFO("Grass population rebuilt: ", blades.size(), " blades, final slice ",
             static_cast<int>(totalMs), "ms (generate ", static_cast<int>(generateMs),
             "ms, density ", params.densityScale, ", distance ", grassDistance_, ")",
             complete ? "" : " - hit the blade cap");
}

void Renderer::renderWorld(game::World* world, game::GameHandler* gameHandler) {
    ZoneScopedN("Renderer::renderWorld");
    (void)world;

    // Guard against null command buffer (e.g. after VK_ERROR_DEVICE_LOST)
    if (currentCmd == VK_NULL_HANDLE) return;

    // GPU crash diagnostic: skip ALL world rendering to isolate crash source
    static const bool skipAll = (std::getenv("WOWEE_SKIP_ALL_RENDER") != nullptr);
    if (skipAll) return;

    worldDrawnLastFrame_ = true;
    worldDrawnThisFrame_ = true;
    softParticlesGameHandler_ = gameHandler;

    auto renderStart = std::chrono::steady_clock::now();
    lastTerrainRenderMs = 0.0;
    lastWMORenderMs = 0.0;
    lastM2RenderMs = 0.0;

    // Cache ghost state for use in overlay and FXAA passes this frame.
    ghostMode_ = (gameHandler && gameHandler->screenEffectIsDeathFx());
    // The spirit world's colour is a desaturation of the finished scene, in
    // the post pass; the tint quad below only covers frames where that pass
    // is not there yet (or could not be made).
    if (postProcessPipeline_) postProcessPipeline_->setGhostDesaturation(ghostMode_ ? 1.0f : 0.0f);
    // The stock full-screen glow: LightParams.Glow of the light around the
    // camera, and a third of the picture swapped for its blur under water
    // (stock_screen_fx, 0x4f8770).
    if (postProcessPipeline_ && lightingManager) {
        postProcessPipeline_->setGlow(
            stock_screen_fx::glowAmount(lightingManager->getLightingParams().glow),
            // The drunk level blends the glow in too: that is the whole of
            // the stock drunk look (0x4F8770). Under the Drunken wobble
            // enhancement Wowee's own blur stands in for it, as before.
            stock_screen_fx::glowBlend(cameraUnderwater_, enh_.drunkWobble ? 0.0f : drunkLevel_));
        postProcessPipeline_->setUnderwaterWave(cameraUnderwater_);
        // the reference client's colour grade: the noon keyframe's curve for this
        // continent, weighted by the hour, outdoors and above water only.
        const float* curve = enhanced_grade::curveForMap(static_cast<int>(lightMapId_));
        const float gradeWeight = (enh_.colorGrade && curve && !cameraUnderwater_)
            ? enhanced_grade::kMaxWeight * enhanced_grade::timeWeight(lightingManager->getTimeOfDay()) *
                  (1.0f - glm::clamp(indoorFactor_, 0.0f, 1.0f))
            : 0.0f;
        postProcessPipeline_->setColorGrade(enh_.colorGrade, gradeWeight, curve == enhanced_grade::kKalimdor);
        // Enhanced's exposure in the HDR scene's tone map, where its daylight
        // is (enhanced_palette::kHdrExposure); outdoors and above water only.
        const bool enhancedTone = enh_.enhancedDaylight && !cameraUnderwater_ &&
                                 enhanced_palette::appliesToMap(lightMapId_);
        const float outdoor = 1.0f - glm::clamp(indoorFactor_, 0.0f, 1.0f);
        // And the shade's chroma and the grey it keeps, by day.
        const auto& tonelp = lightingManager->getLightingParams();
        const float shadeOn = enhancedTone ? outdoor * enhanced_palette::dayShare(tonelp.sunDir.z) : 0.0f;
        postProcessPipeline_->setEnhancedTone(
            enhancedTone ? glm::mix(1.0f, enhanced_palette::hdrExposure(), outdoor) : 0.0f,
            enhancedTone ? glm::mix(1.0f, enhanced_palette::hdrSaturation(), outdoor) : 0.0f,
            glm::mix(1.0f, enhanced_palette::hdrShadeChroma(), shadeOn),
            enhanced_palette::shadeTint(tonelp.ambientColor, shadeOn),
            // The softer shoulder goes with the stronger sun: outdoors, by
            // day, under Enhanced's daylight (enhanced_tonemap::kEnhancedKnee).
            enhancedTone ? glm::mix(enhanced_tonemap::tuning().knee, enhanced_tonemap::enhancedKnee(),
                                   outdoor * enhanced_palette::dayShare(tonelp.sunDir.z))
                        : 0.0f);
    }

    uint32_t frameIdx = vkCtx->getCurrentFrame();
    VkDescriptorSet perFrameSet = perFrameDescSets[frameIdx];
    const glm::mat4& view = camera ? camera->getViewMatrix() : glm::mat4(1.0f);
    const glm::mat4& projection = camera ? camera->getProjectionMatrix() : glm::mat4(1.0f);

    // GPU crash diagnostic: skip individual renderers to isolate which one faults
    static const bool envSkipWMO = (std::getenv("WOWEE_SKIP_WMO") != nullptr);
    static const bool envSkipChars = (std::getenv("WOWEE_SKIP_CHARS") != nullptr);
    static const bool envSkipM2 = (std::getenv("WOWEE_SKIP_M2") != nullptr);
    static const bool envSkipTerrain = (std::getenv("WOWEE_SKIP_TERRAIN") != nullptr);
    static const bool envSkipSky = (std::getenv("WOWEE_SKIP_SKY") != nullptr);
    // ...and the ablation switches the same passes off, one phase at a time.
    const auto ablated = [&](AblationPass p) {
        return passAblation_ && passAblation_->skip(p);
    };
    const bool skipWMO = envSkipWMO || ablated(AblationPass::WMO);
    const bool skipChars = envSkipChars || ablated(AblationPass::Characters);
    const bool skipM2 = envSkipM2 || ablated(AblationPass::M2);
    // The WMO portal walk, before any cull reads it: which groups (and so
    // which WMO doodads) are drawn, and whether the camera stands in an
    // interior with no portal open to the outdoors - in which case the client
    // draws no sky, no terrain and nothing outside (0x79a870, 0x7ad1f0).
    std::optional<float> groundUnderCamera;
    if (terrainManager && camera) {
        groundUnderCamera = terrainManager->getHeightAt(camera->getPosition().x, camera->getPosition().y);
    }
    const bool sealedIndoors = wmoRenderer && camera && !skipWMO &&
                               wmoRenderer->computeVisibility(*camera, groundUnderCamera);
    // The opening the outdoors shows through from an interior, in pixels:
    // terrain, grass, the sky and every map object outside are drawn only
    // inside it, as stock 0x79a790 narrows the outdoor pass.
    if (wmoRenderer) wmoRenderer->setOutdoorScissor(activeRenderExtent_.width, activeRenderExtent_.height);
    const auto clipToOutdoors = [&](VkCommandBuffer c) {
        if (wmoRenderer && wmoRenderer->outdoorClipped()) {
            const VkRect2D sc = wmoRenderer->outdoorScissor();
            vkCmdSetScissor(c, 0, 1, &sc);
        }
    };
    const bool skipTerrain = envSkipTerrain || ablated(AblationPass::Terrain) || sealedIndoors;
    // Under liquid the stock client draws no sky (0x79a870 calls the sky only
    // while 0xcd8794, the camera's liquid, is 0) and clears to the fog colour.
    const bool skipSky = envSkipSky || ablated(AblationPass::Sky) || sealedIndoors ||
                         cameraUnderwater_;
    const bool skipGrass = ablated(AblationPass::Grass) || sealedIndoors;
    if (m2Renderer) {
        m2Renderer->setSkipGroundDetail(ablated(AblationPass::Clutter));
        // 250 yards: past the shadow distance and well past where a tree with
        // no LOD is still worth every triangle it has.
        m2Renderer->setDoodadDistanceCap(ablated(AblationPass::FarDoodads) ? 250.0f : 0.0f);
    }

    // Get time of day for sky-related rendering
    auto* skybox = skySystem ? skySystem->getSkybox() : nullptr;
    float timeOfDay = lightingManager
        ? lightingManager->getVisualTimeOfDayHours()
        : (skybox ? skybox->getTimeOfDay() : 12.0f);
    // Two questions, apart while the sky crossfades. The sky models are drawn
    // whenever any is up at all; the procedural sun, moons and clouds they
    // stand in for hand over halfway, where both are at half and a switch is
    // least visible - rather than staying away until the last model has faded.
    const bool drawSkyModels = skyboxModelRenderer_ &&
        std::any_of(skyLayers_.begin(), skyLayers_.end(), [](const SkyLayerInstance& l) { return !l.stars; });
    const bool drawSkyStars = skyStarsRenderer_ &&
        std::any_of(skyLayers_.begin(), skyLayers_.end(), [](const SkyLayerInstance& l) { return l.stars; });
    // The procedural sky goes only when a sky model takes the whole of it:
    // one above 0.99 weight whose LightSkybox flags lack bit 2 (0x7f09b0).
    // Below that the dome, sun, moons and clouds stay, with the models over
    // them at their weights.
    bool skyModelReplacesSky = false;
    if (lightingManager) {
        for (const auto& layer : lightingManager->getSkyboxLayers()) {
            if (layer.replacesSky) skyModelReplacesSky = true;
        }
    }
    const bool useOriginalSkybox = drawSkyModels && skyModelReplacesSky;

    // ── Multithreaded secondary command buffer recording ──
    // Terrain, WMO, and M2 record on worker threads while main thread handles
    // sky, characters, water, and effects.  prepareRender() on main thread first
    // to handle thread-unsafe GPU allocations (descriptor pools, bone SSBOs).
    if (parallelRecordingEnabled_) {
        // --- Pre-compute state + GPU allocations on main thread (not thread-safe) ---
        if (m2Renderer && cameraController) {
            // Use isInsideInteriorWMO (flag 0x2000) - not isInsideWMO which includes
            // outdoor WMO groups like archways/bridges that should receive shadows.
            m2Renderer->setInsideInterior(cameraController->isInsideInteriorWMO());
        }
        auto prepStart = std::chrono::steady_clock::now();
        if (wmoRenderer) wmoRenderer->prepareRender();
        auto prepWmoEnd = std::chrono::steady_clock::now();
        if (m2Renderer && camera) m2Renderer->prepareRender(frameIdx, *camera);
        if (drawSkyModels && camera)
            skyboxModelRenderer_->prepareRender(frameIdx, *camera);
        if (drawSkyStars && camera)
            skyStarsRenderer_->prepareRender(frameIdx, *camera);
        auto prepM2End = std::chrono::steady_clock::now();
        if (characterRenderer) characterRenderer->prepareRender(frameIdx);
        auto prepEnd = std::chrono::steady_clock::now();
        const double prepWmoMs  = std::chrono::duration<double, std::milli>(prepWmoEnd - prepStart).count();
        const double prepM2Ms   = std::chrono::duration<double, std::milli>(prepM2End - prepWmoEnd).count();
        const double prepCharMs = std::chrono::duration<double, std::milli>(prepEnd - prepM2End).count();

        // --- Dispatch worker threads (terrain + WMO + M2) ---
        std::future<double> terrainFuture, wmoFuture, charFuture, m2Future, postFuture;

        // Grass rides in the terrain secondary: it sits on the ground, and
        // that buffer is executed after the sky and before WMO, which is
        // exactly the order grass wants. Recording it there touches only the
        // one worker's command buffer.
        //
        // So the buffer is recorded when EITHER wants it, not when terrain
        // does. Hanging it off terrain alone meant that switching terrain off
        // took grass with it - which for a crash bisect is harmless and for an
        // ablation is the whole measurement, since the terrain phase would
        // then be reporting what terrain and grass cost together.
        const bool drawTerrain = terrainRenderer && camera && terrainEnabled && !skipTerrain;
        const bool drawGrass = grassRenderer_ && !skipGrass;
        // With the WMO pass split, terrain is the next worker in line behind
        // M2, and its cost is a bind and a draw per surviving chunk. Cut the
        // same way as WMO and M2 below: the first half of the culled chunks
        // is recorded into SEC_TERRAIN_A on another worker, executed just
        // before SEC_TERRAIN, which takes the second half and then the grass.
        // Set by the worker, read after it is joined.
        bool terrainFirstHalf = false;
        if (drawTerrain || drawGrass) {
            terrainFuture = core::ThreadPool::frameWorkers().submit([&]() -> double {
                auto t0 = std::chrono::steady_clock::now();
                const bool terrainCulled = drawTerrain && terrainRenderer->cullForRecording(*camera);
                const uint32_t parts = terrainCulled ? terrainRenderer->recordPartsWanted() : 1;
                // Caller-runs-a-chunk, as on the WMO worker.
                std::future<void> first;
                if (parts > 1) {
                    terrainFirstHalf = true;
                    first = core::ThreadPool::frameWorkers().submit([&]() {
                        VkCommandBuffer cmdA = beginSecondary(SEC_TERRAIN_A);
                        setSecondaryViewportScissor(cmdA);
                        clipToOutdoors(cmdA);
                        terrainRenderer->recordPart(cmdA, perFrameSet, 0, parts);
                        vkEndCommandBuffer(cmdA);
                    });
                }
                try {
                    VkCommandBuffer cmd = beginSecondary(SEC_TERRAIN);
                    setSecondaryViewportScissor(cmd);
                    clipToOutdoors(cmd);
                    if (terrainCulled) terrainRenderer->recordPart(cmd, perFrameSet, parts - 1, parts);
                    if (drawGrass) grassRenderer_->render(cmd, frameIdx, perFrameSet);
                    vkEndCommandBuffer(cmd);
                } catch (...) {
                    // The other half still holds references into this frame.
                    if (first.valid()) first.wait();
                    throw;
                }
                if (first.valid()) first.get();
                return std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
            });
        }

        // The WMO worker is the critical path of renderWorld, and most of what
        // it spends is the driver encoding its commands, which on this driver
        // happens as they are recorded and at vkEndCommandBuffer. So a large
        // frame is cut in two after the cull: the second half is recorded
        // into SEC_WMO_B on another worker while this one records the first,
        // and SEC_WMO_B is executed straight after SEC_WMO, so the draws reach
        // the GPU in the order one buffer would have given them. Set by the
        // worker, read after it is joined.
        bool wmoSecondHalf = false;
        if (wmoRenderer && camera && !skipWMO) {
            wmoFuture = core::ThreadPool::frameWorkers().submit([&]() -> double {
                auto t0 = std::chrono::steady_clock::now();
                wmoRenderer->cullForRecording(*camera, &characterPosition);
                const uint32_t parts = wmoRenderer->recordPartsWanted();
                // Caller-runs-a-chunk, as ThreadPool allows: this thread
                // records its half before it waits on the other.
                std::future<void> second;
                if (parts > 1) {
                    wmoSecondHalf = true;
                    second = core::ThreadPool::frameWorkers().submit([&]() {
                        VkCommandBuffer cmdB = beginSecondary(SEC_WMO_B);
                        setSecondaryViewportScissor(cmdB);
                        wmoRenderer->recordPart(cmdB, perFrameSet, 1, parts);
                        vkEndCommandBuffer(cmdB);
                    });
                }
                try {
                    VkCommandBuffer cmd = beginSecondary(SEC_WMO);
                    setSecondaryViewportScissor(cmd);
                    wmoRenderer->recordPart(cmd, perFrameSet, 0, parts);
                    vkEndCommandBuffer(cmd);
                } catch (...) {
                    // The other half still holds references into this frame.
                    if (second.valid()) second.wait();
                    throw;
                }
                if (second.valid()) second.get();
                return std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
            });
        }

        // The M2 worker is the critical path once WMO is split. Its opaque
        // groups are cut in two after the cull (M2Renderer::recordPartsWanted):
        // the first share is recorded into SEC_M2_A on another worker and
        // executed just before SEC_M2, which takes the rest, the blended pass,
        // the glow sprites and the particles, all of which must follow every
        // opaque draw. Set by the worker, read after it is joined.
        bool m2FirstShare = false;
        if (m2Renderer && camera && !skipM2) {
            m2Future = core::ThreadPool::frameWorkers().submit([&]() -> double {
                auto t0 = std::chrono::steady_clock::now();
                VkCommandBuffer cmd = beginSecondary(SEC_M2);
                setSecondaryViewportScissor(cmd);
                const auto tBegin = std::chrono::steady_clock::now();
                if (m2Renderer->cullForRecording(*camera)) {
                    const uint32_t parts = perFrameSet ? m2Renderer->recordPartsWanted() : 1;
                    // Caller-runs-a-chunk, as on the WMO worker.
                    std::future<void> first;
                    if (parts > 1) {
                        m2FirstShare = true;
                        first = core::ThreadPool::frameWorkers().submit([&]() {
                            VkCommandBuffer cmdA = beginSecondary(SEC_M2_A);
                            setSecondaryViewportScissor(cmdA);
                            m2Renderer->recordPart(cmdA, perFrameSet, 0, parts);
                            vkEndCommandBuffer(cmdA);
                        });
                    }
                    try {
                        m2Renderer->recordPart(cmd, perFrameSet, parts - 1, parts);
                    } catch (...) {
                        // The other share still holds references into this frame.
                        if (first.valid()) first.wait();
                        throw;
                    }
                    if (first.valid()) first.get();
                    // Needs both shares' sprites, so after the join.
                    m2Renderer->recordGlowSprites(cmd, perFrameSet);
                }
                const auto tModels = std::chrono::steady_clock::now();
                m2Renderer->renderSmokeParticles(cmd, perFrameSet);
                const auto tSmoke = std::chrono::steady_clock::now();
                m2Renderer->renderM2Particles(cmd, perFrameSet);
                const auto tParts = std::chrono::steady_clock::now();
                m2Renderer->renderM2Ribbons(cmd, perFrameSet);
                const auto tParticles = std::chrono::steady_clock::now();
                vkEndCommandBuffer(cmd);

                // This worker is the critical path of renderWorld, and the
                // model pass inside it accounts for barely a fifth of what it
                // takes: the cull, the sort and the draw recording together
                // measure half a millisecond against the three this returns.
                // So the rest is timed too - the particles and ribbons that
                // share the buffer, and ending the buffer itself, which on
                // this driver is where a secondary is actually encoded.
                static const bool prof = core::envFlagEnabled("WOWEE_FRAME_PROFILE", false);
                if (prof) {
                    static auto lastSaid = std::chrono::steady_clock::now();
                    const auto tEnd = std::chrono::steady_clock::now();
                    if (tEnd - lastSaid > std::chrono::seconds(10)) {
                        lastSaid = tEnd;
                        const auto ms = [](auto a, auto b) {
                            return std::chrono::duration<double, std::milli>(b - a).count();
                        };
                        LOG_WARNING("  m2 worker: begin ", ms(t0, tBegin),
                                    "ms, models ", ms(tBegin, tModels),
                                    "ms, smoke ", ms(tModels, tSmoke),
                                    "ms, particles ", ms(tSmoke, tParts),
                                    "ms, ribbons ", ms(tParts, tParticles),
                                    "ms, endCommandBuffer ", ms(tParticles, tEnd), "ms");
                    }
                }
                return std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
            });
        }

        // --- Main thread: record sky (SEC_SKY) ---
        {
            VkCommandBuffer cmd = beginSecondary(SEC_SKY);
            setSecondaryViewportScissor(cmd);
            clipToOutdoors(cmd);
            if (skySystem && camera && !skipSky) {
                rendering::SkyParams skyParams = rendering::skyParamsFromLighting(
                    timeOfDay,
                    gameHandler ? gameHandler->getGameTime() : -1.0f,
                    effectiveWeatherGrade(),
                    lightingManager ? &lightingManager->getLightingParams() : nullptr,
                    useOriginalSkybox);
                skyParams.sunOcclusion = sunOcclusion_;
                skyParams.sunGain = vkCtx->isHdrScene() ? enhanced_tonemap::tuning().sunGain : 1.0f;
                // The stock order (0x7f09b0): stars, sun and moons, the
                // additive dome and the clouds, then the LightSkybox models.
                // The dome is opaque here and drawn first; the stars and the
                // sprites add to it, which comes to the same.
                skySystem->renderDome(cmd, perFrameSet, skyParams);
                if (drawSkyStars) skyStarsRenderer_->render(cmd, perFrameSet, *camera);
                skySystem->renderOverlays(cmd, perFrameSet, *camera, skyParams);
                if (drawSkyModels) {
                    skyboxModelRenderer_->render(cmd, perFrameSet, *camera);
                }
            }
            vkEndCommandBuffer(cmd);
        }

        // --- Main thread: record selection circle before overlay state is used by post ---
        {
            VkCommandBuffer cmd = beginSecondary(SEC_SELECTION);
            setSecondaryViewportScissor(cmd);
            if (overlaySystem_) {
                overlaySystem_->renderSelectionCircle(view, projection, cmd,
                    terrainManager ? OverlaySystem::HeightQuery2D([&](float x, float y) { return terrainManager->getHeightAt(x, y); }) : OverlaySystem::HeightQuery2D{},
                    wmoRenderer ? OverlaySystem::HeightQuery3D([&](float x, float y, float z) { return wmoRenderer->getFloorHeight(x, y, z); }) : OverlaySystem::HeightQuery3D{},
                    m2Renderer ? OverlaySystem::HeightQuery3D([&](float x, float y, float z) { return m2Renderer->getFloorHeight(x, y, z); }) : OverlaySystem::HeightQuery3D{});
            }
            vkEndCommandBuffer(cmd);
        }

        // Character recording is independent after prepareRender() and no
        // longer shares the selection-circle overlay command buffer.
        charFuture = core::ThreadPool::frameWorkers().submit([&]() -> double {
            auto t0 = std::chrono::steady_clock::now();
            VkCommandBuffer cmd = beginSecondary(SEC_CHARS);
            setSecondaryViewportScissor(cmd);
            if (characterRenderer && camera && !skipChars) {
                characterRenderer->render(cmd, perFrameSet, *camera);
            }
            vkEndCommandBuffer(cmd);
            return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
        });

        // Post-world systems are disjoint from terrain/WMO/M2/characters. Start
        // this after selection recording so OverlaySystem is never used from
        // two threads at once.
        postFuture = core::ThreadPool::frameWorkers().submit([&]() -> double {
            auto t0 = std::chrono::steady_clock::now();
            VkCommandBuffer cmd = beginSecondary(SEC_POST);
            setSecondaryViewportScissor(cmd);
            if (waterRenderer && camera && !waterDrawsInContinuePass()) {
                waterRenderer->setRenderExtent(activeRenderExtent_);
                waterRenderer->render(cmd, perFrameSet, *camera, globalTime, false, frameIdx);
            }
            if (weather && camera && !weatherDrawsWithWater_) weather->render(cmd, perFrameSet);
            if (lightning && camera && lightning->isEnabled()) lightning->render(cmd, perFrameSet);
            if (swimEffects && camera && !swimEffectsDrawWithWater_) {
                swimEffects->render(cmd, perFrameSet);
            }
            if (footfallDust_ && camera) footfallDust_->render(cmd, perFrameSet);
            if (footprintRenderer && camera) footprintRenderer->render(cmd, perFrameSet, *camera);

            renderPostSceneOverlays(cmd, gameHandler);
            vkEndCommandBuffer(cmd);
            return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
        });

        // --- Wait for workers ---
        // Guard with try-catch: future::get() re-throws any exception from the
        // async task. Without this, a single bad_alloc in a render worker would
        // propagate as an unhandled exception and terminate the process.
        try { if (terrainFuture.valid()) lastTerrainRenderMs = terrainFuture.get(); }
        catch (const std::exception& e) { LOG_ERROR("Terrain render worker: ", e.what()); }
        try { if (wmoFuture.valid()) lastWMORenderMs = wmoFuture.get(); }
        catch (const std::exception& e) { LOG_ERROR("WMO render worker: ", e.what()); }
        try { if (m2Future.valid()) lastM2RenderMs = m2Future.get(); }
        catch (const std::exception& e) { LOG_ERROR("M2 render worker: ", e.what()); }
        try { if (charFuture.valid()) (void)charFuture.get(); }
        catch (const std::exception& e) { LOG_ERROR("Character render worker: ", e.what()); }
        try { if (postFuture.valid()) (void)postFuture.get(); }
        catch (const std::exception& e) { LOG_ERROR("Post render worker: ", e.what()); }

        // prepareRender() does the GPU allocations that are not thread-safe, so it runs
        // on the main thread and is not covered by the worker timings. Name the culprit
        // when a frame runs long instead of leaving renderWorld as one opaque number.
        const double prepTotalMs = prepWmoMs + prepM2Ms + prepCharMs;
        const double worstWorkerMs = std::max({lastTerrainRenderMs, lastWMORenderMs, lastM2RenderMs});

        // The same breakdown on a timer, not only when a frame runs long.
        //
        // renderWorld is 3.3ms of a 15.8ms frame and the CPU and the GPU are
        // within a millisecond of each other now, so what the main thread
        // spends here decides the frame as much as any pass does - and the
        // 40ms threshold below only ever speaks when something has already
        // gone wrong. This says where the steady state goes.
        static const bool frameProfile = core::envFlagEnabled("WOWEE_FRAME_PROFILE", false);
        if (frameProfile) {
            static auto lastSaid = std::chrono::steady_clock::now();
            const auto sayNow = std::chrono::steady_clock::now();
            if (sayNow - lastSaid > std::chrono::seconds(10)) {
                lastSaid = sayNow;
                LOG_WARNING("  renderWorld: prepare ", prepTotalMs,
                            "ms (wmo ", prepWmoMs, " m2 ", prepM2Ms,
                            " char ", prepCharMs, "), workers terrain ",
                            lastTerrainRenderMs, " wmo ", lastWMORenderMs,
                            " m2 ", lastM2RenderMs, " (worst ", worstWorkerMs,
                            "), terrain chunks drawn ",
                            terrainRenderer ? terrainRenderer->getRenderedChunkCount() : 0,
                            " culled ",
                            terrainRenderer ? terrainRenderer->getCulledChunkCount() : 0);
            }
        }

        if (prepTotalMs + worstWorkerMs > 40.0) {
            LOG_WARNING("SLOW renderWorld breakdown: prepare=", prepTotalMs,
                        "ms (wmo=", prepWmoMs, " m2=", prepM2Ms, " char=", prepCharMs,
                        ") workers: terrain=", lastTerrainRenderMs,
                        " wmo=", lastWMORenderMs, " m2=", lastM2RenderMs,
                        // Terrain is usually the critical path here, and its cost
                        // is one descriptor bind plus one draw per surviving
                        // chunk - so the counts say whether a slow frame is draw
                        // volume or something else entirely.
                        " | terrain chunks drawn=",
                        terrainRenderer ? terrainRenderer->getRenderedChunkCount() : 0,
                        " culled=",
                        terrainRenderer ? terrainRenderer->getCulledChunkCount() : 0,
                        " resident=",
                        terrainRenderer ? terrainRenderer->getChunkCount() : 0);
        }

        // --- Execute all secondary buffers in correct draw order ---
        VkCommandBuffer validCmds[NUM_SECONDARIES];
        uint32_t numCmds = 0;
        // Terrain first, then the sky. Every sky layer sits on the far plane
        // and depth-tests against what is already there, so drawing it after
        // the ground skips the clouds' noise on every pixel a hill covers,
        // which outdoors is most of them. Only the terrain goes ahead of it:
        // it is the one pass that is opaque throughout. Buildings carry
        // blended windows and doodads carry leaves and particles, and blended
        // pixels leave no depth behind, so a sky drawn after them would paint
        // over whichever of them stood against it.
        const auto queue = [&](VkCommandBuffer buffer) { validCmds[numCmds++] = buffer; };
        if (drawTerrain || drawGrass) {
            if (terrainFirstHalf) queue(secondaryCmds_[SEC_TERRAIN_A][frameIdx]);
            queue(secondaryCmds_[SEC_TERRAIN][frameIdx]);
        }
        queue(secondaryCmds_[SEC_SKY][frameIdx]);
        if (wmoRenderer && camera && !skipWMO) {
            queue(secondaryCmds_[SEC_WMO][frameIdx]);
            if (wmoSecondHalf) queue(secondaryCmds_[SEC_WMO_B][frameIdx]);
        }
        queue(secondaryCmds_[SEC_SELECTION][frameIdx]);
        queue(secondaryCmds_[SEC_CHARS][frameIdx]);
        if (m2Renderer && camera && !skipM2) {
            if (m2FirstShare) queue(secondaryCmds_[SEC_M2_A][frameIdx]);
            queue(secondaryCmds_[SEC_M2][frameIdx]);
        }
        queue(secondaryCmds_[SEC_POST][frameIdx]);

        // No marks in here. This pass was begun for secondary buffers, and a
        // primary may record nothing but vkCmdExecuteCommands inside it: the
        // timestamps and checkpoints the marks wrote between the passes were
        // invalid, and validation said so on NVIDIA. The world shows in the
        // timeline as the gap before the next mark outside the pass;
        // WOWEE_SINGLE_THREAD_RECORD gives the per-pass breakdown.
        if (numCmds > 0) vkCmdExecuteCommands(currentCmd, numCmds, validCmds);

    } else {
        // ── Fallback: single-threaded inline recording (original path) ──
        clipToOutdoors(currentCmd);

        if (terrainRenderer && camera && terrainEnabled && !skipTerrain) {
            auto terrainStart = std::chrono::steady_clock::now();
            terrainRenderer->render(currentCmd, perFrameSet, *camera);
            if (vkCtx) vkCtx->gpuMark(currentCmd, "terrain");
            lastTerrainRenderMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - terrainStart).count();
        }

        // After terrain, before the world's models: grass sits on the ground and
        // is occluded by everything standing on it.
        if (grassRenderer_ && vkCtx && !skipGrass) {
            grassRenderer_->render(currentCmd, vkCtx->getCurrentFrame(), perFrameSet);
            if (vkCtx) vkCtx->gpuMark(currentCmd, "grass");
        }

        // Sky after the ground, for the reason given on the parallel path.
        if (skySystem && camera && !skipSky) {
            rendering::SkyParams skyParams = rendering::skyParamsFromLighting(
                timeOfDay,
                gameHandler ? gameHandler->getGameTime() : -1.0f,
                effectiveWeatherGrade(),
                lightingManager ? &lightingManager->getLightingParams() : nullptr,
                useOriginalSkybox);
            skyParams.sunOcclusion = sunOcclusion_;
            skyParams.sunGain = vkCtx->isHdrScene() ? enhanced_tonemap::tuning().sunGain : 1.0f;
            // The stock order (0x7f09b0): stars, sun and moons, the additive
            // dome and the clouds, then the LightSkybox models (see the
            // parallel path).
            skySystem->renderDome(currentCmd, perFrameSet, skyParams);
            if (drawSkyStars) {
                skyStarsRenderer_->prepareRender(frameIdx, *camera);
                skyStarsRenderer_->render(currentCmd, perFrameSet, *camera);
            }
            skySystem->renderOverlays(currentCmd, perFrameSet, *camera, skyParams);
            if (drawSkyModels) {
                skyboxModelRenderer_->prepareRender(frameIdx, *camera);
                skyboxModelRenderer_->render(currentCmd, perFrameSet, *camera);
            }
            if (vkCtx) vkCtx->gpuMark(currentCmd, "sky");
        }

        if (wmoRenderer && wmoRenderer->outdoorClipped()) {
            const VkRect2D full = wmoRenderer->fullScissor();
            vkCmdSetScissor(currentCmd, 0, 1, &full);
        }
        if (wmoRenderer && camera && !skipWMO) {
            wmoRenderer->prepareRender();
            auto wmoStart = std::chrono::steady_clock::now();
            wmoRenderer->render(currentCmd, perFrameSet, *camera, &characterPosition);
            if (vkCtx) vkCtx->gpuMark(currentCmd, "wmo");
            lastWMORenderMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - wmoStart).count();
        }

        if (overlaySystem_) {
            overlaySystem_->renderSelectionCircle(view, projection, currentCmd,
                terrainManager ? OverlaySystem::HeightQuery2D([&](float x, float y) { return terrainManager->getHeightAt(x, y); }) : OverlaySystem::HeightQuery2D{},
                wmoRenderer ? OverlaySystem::HeightQuery3D([&](float x, float y, float z) { return wmoRenderer->getFloorHeight(x, y, z); }) : OverlaySystem::HeightQuery3D{},
                m2Renderer ? OverlaySystem::HeightQuery3D([&](float x, float y, float z) { return m2Renderer->getFloorHeight(x, y, z); }) : OverlaySystem::HeightQuery3D{});
        }

        if (characterRenderer && camera && !skipChars) {
            characterRenderer->prepareRender(frameIdx);
            characterRenderer->render(currentCmd, perFrameSet, *camera);
            if (vkCtx) vkCtx->gpuMark(currentCmd, "characters");
        }

        if (m2Renderer && camera && !skipM2) {
            if (cameraController) {
                // Use isInsideInteriorWMO (flag 0x2000) for correct indoor detection
                m2Renderer->setInsideInterior(cameraController->isInsideInteriorWMO());
            }
            m2Renderer->prepareRender(frameIdx, *camera);
            auto m2Start = std::chrono::steady_clock::now();
            m2Renderer->render(currentCmd, perFrameSet, *camera);
            m2Renderer->renderSmokeParticles(currentCmd, perFrameSet);
            m2Renderer->renderM2Particles(currentCmd, perFrameSet);
            m2Renderer->renderM2Ribbons(currentCmd, perFrameSet);
            if (vkCtx) vkCtx->gpuMark(currentCmd, "m2");
            lastM2RenderMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - m2Start).count();
        }

        if (waterRenderer && camera && !waterDrawsInContinuePass()) {
            waterRenderer->setRenderExtent(activeRenderExtent_);
            waterRenderer->render(currentCmd, perFrameSet, *camera, globalTime, false, frameIdx);
            if (vkCtx) vkCtx->gpuMark(currentCmd, "water");
        }
        if (weather && camera && !weatherDrawsWithWater_) weather->render(currentCmd, perFrameSet);
        if (lightning && camera && lightning->isEnabled()) lightning->render(currentCmd, perFrameSet);
        if (swimEffects && camera && !swimEffectsDrawWithWater_) {
            swimEffects->render(currentCmd, perFrameSet);
        }
        if (footfallDust_ && camera) footfallDust_->render(currentCmd, perFrameSet);
        if (footprintRenderer && camera) footprintRenderer->render(currentCmd, perFrameSet, *camera);
        // The same one-line total the parallel path reports, so a profile taken
        // either way can be compared against the other.
        if (vkCtx) vkCtx->gpuMark(currentCmd, "world total");
    }

    // Post-scene overlays and minimap - in the fallback path these run inline;
    // in the parallel path they were already recorded into SEC_POST above.
    // (There is no underwater overlay: the stock client tints nothing under
    // liquid - the fog and the clear colour do it; see docs/re/water.md.)
    if (!parallelRecordingEnabled_) {
        renderPostSceneOverlays(currentCmd, gameHandler);
    }

    // Water is drawn last, in a continuation of the scene pass, so that the
    // refraction copy taken just before it holds the scene WITHOUT water. Taking
    // that copy from the finished frame instead fed the water its own output:
    // a moving object left one sharp copy per frame (a train of ghosts) and the
    // brightness applied to the water compounded through the loop and pumped.
    // Over an off-screen MSAA scene (soft particles on) the water pass needs a
    // framebuffer made for the frame; asked before the scene pass is closed,
    // so that if it cannot be made (out of memory) the pass stays open, the
    // frame goes on without its water, and the post-processing closes it.
    VkFramebuffer sceneWaterFb = VK_NULL_HANDLE;
    VkExtent2D sceneWaterExtent{};
    const bool sceneWaterPass = waterDrawsInContinuePass() && camera &&
        vkCtx->getMsaaSamples() > VK_SAMPLE_COUNT_1_BIT &&
        postProcessPipeline_ && postProcessPipeline_->getSceneFramebuffer() != VK_NULL_HANDLE;
    if (sceneWaterPass) {
        sceneWaterFb = sceneWater1xFramebuffer(sceneWaterExtent);
        if (sceneWaterFb == VK_NULL_HANDLE) {
            static bool said = false;
            if (!said) { said = true; LOG_ERROR("Water continuation: no framebuffer for the scene"); }
        }
    }
    if (waterDrawsInContinuePass() && camera && (!sceneWaterPass || sceneWaterFb != VK_NULL_HANDLE)) {
        vkCmdEndRenderPass(currentCmd);
        markWorldPassClosed();

        VkImage sceneColor = VK_NULL_HANDLE;
        VkImage sceneDepth = VK_NULL_HANDLE;
        VkExtent2D sceneExtent = vkCtx->getSwapchainExtent();
        bool depthIsMsaa = vkCtx->isDepthCopySourceMsaa();
        if (postProcessPipeline_ && postProcessPipeline_->getSceneFramebuffer() != VK_NULL_HANDLE) {
            sceneColor = postProcessPipeline_->getSceneColorImage();
            sceneDepth = postProcessPipeline_->getSceneDepthImage();
            sceneExtent = postProcessPipeline_->getSceneRenderExtent();
            depthIsMsaa = postProcessPipeline_->sceneDepthIsMsaa();
        } else if (currentImageIndex < vkCtx->getSwapchainImages().size()) {
            sceneColor = vkCtx->getSwapchainImages()[currentImageIndex];
            sceneDepth = vkCtx->getDepthCopySourceImage();
        }

        // The opaque scene is finished and out of its pass: the ray traced
        // lighting reads its depth here, for the surfaces of the next frame.
        recordRtLighting(sceneDepth, sceneExtent, depthIsMsaa);

        // The blended particles, before the water as they were in the scene
        // pass, now that its depth can be read (soft particles only).
        recordContactShadowPass(currentCmd);
        recordSoftParticlePass(currentCmd);

        // Only with water loaded to refract it, as the MSAA path in endFrame
        // already asks: the copy is the whole colour and depth of the frame,
        // and with no surface to sample it nothing reads it.
        if (sceneColor != VK_NULL_HANDLE && waterRenderer->isRefractionEnabled() &&
            waterRenderer->hasSurfaces()) {
            waterRenderer->captureSceneHistory(currentCmd, sceneColor, sceneDepth,
                                               sceneExtent, depthIsMsaa,
                                               vkCtx->getCurrentFrame());
            vkCtx->gpuMark(currentCmd, "water refraction copy");
        }

        // Without MSAA the water continues into the scene's own framebuffer. With
        // MSAA it draws single-sampled into the resolved image instead, which is
        // both cheaper and what lets it leave the multisampled pass at all.
        const bool msaaOn = vkCtx->getMsaaSamples() > VK_SAMPLE_COUNT_1_BIT;
        VkRenderPassBeginInfo contRp{};
        contRp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        VkExtent2D waterExtent = activeRenderExtent_;
        const bool offscreenScene =
            postProcessPipeline_ && postProcessPipeline_->getSceneFramebuffer() != VK_NULL_HANDLE;
        if (msaaOn && offscreenScene) {
            // Soft particles over an off-screen MSAA scene: its resolved
            // colour and depth, the depth kept for the post-processing.
            contRp.renderPass = waterRenderer->getWater1xKeepDepthRenderPass();
            contRp.framebuffer = sceneWaterFb;
            waterExtent = sceneWaterExtent;
        } else if (msaaOn) {
            contRp.renderPass = waterRenderer->getWater1xRenderPass();
            contRp.framebuffer = waterRenderer->getWater1xFramebuffer(currentImageIndex);
            waterExtent = vkCtx->getSwapchainExtent();
        } else {
            contRp.renderPass = vkCtx->getSceneContinueRenderPass();
            contRp.framebuffer = activeFramebuffer_;
        }
        contRp.renderArea.extent = waterExtent;
        vkCmdBeginRenderPass(currentCmd, &contRp, VK_SUBPASS_CONTENTS_INLINE);
        waterPassOpen_ = true;

        VkViewport vp{};
        vp.width = static_cast<float>(waterExtent.width);
        vp.height = static_cast<float>(waterExtent.height);
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(currentCmd, 0, 1, &vp);
        VkRect2D sc{};
        sc.extent = waterExtent;
        vkCmdSetScissor(currentCmd, 0, 1, &sc);

        waterRenderer->setRenderExtent(waterExtent);
        waterRenderer->render(currentCmd, perFrameSet, *camera, globalTime, msaaOn, frameIdx);

        // Rain and snow over the water, as in the scene pass, when they came
        // out of it with the water (soft particles on).
        if (weather && weatherDrawsWithWater_ && weather.get() == passBuiltFor_[2]) weather->render(currentCmd, perFrameSet);

        // Spray belongs on top of the surface it is thrown off. Recorded in the
        // scene pass it went under the water instead, which the sheet then hid -
        // barely at the shore where alpha sits near its floor, completely in the
        // deeper water you swim in.
        if (swimEffects && camera && swimEffectsDrawWithWater_ && swimEffects.get() == passBuiltFor_[0]) {
            swimEffects->render(currentCmd, perFrameSet);
        }

        // And the minimap, last of all: it is the interface rather than the
        // world, and nothing in the world belongs over it.
        // Only once it has been built for this pass (refreshSwimEffectsPass):
        // one made this frame still has the scene pass's pipelines.
        if (minimapDrawsWithWater_ && minimap.get() == passBuiltFor_[1]) renderMinimapOverlay(currentCmd, gameHandler);
    }

    auto renderEnd = std::chrono::steady_clock::now();
    lastRenderMs = std::chrono::duration<double, std::milli>(renderEnd - renderStart).count();
}

VkFramebuffer Renderer::sceneWater1xFramebuffer(VkExtent2D& extent) {
    if (!vkCtx || !waterRenderer || !postProcessPipeline_) return VK_NULL_HANDLE;
    const VkRenderPass pass = waterRenderer->getWater1xKeepDepthRenderPass();
    const VkImageView color = postProcessPipeline_->getSceneColorImageView();
    const VkImage depth = postProcessPipeline_->getSceneDepthImage();
    if (pass == VK_NULL_HANDLE || color == VK_NULL_HANDLE || depth == VK_NULL_HANDLE ||
        postProcessPipeline_->sceneDepthIsMsaa()) {
        return VK_NULL_HANDLE;
    }
    extent = postProcessPipeline_->getSceneRenderExtent();
    // Made for this frame and destroyed when this slot next comes round, as
    // the late particle pass makes its own: the images behind them are
    // rebuilt on every resize, sample count and upscaler change.
    VkDevice device = vkCtx->getDevice();
    const uint32_t slot = vkCtx->getCurrentFrame() % 2;
    destroySceneWaterRetired(slot);
    VkImageViewCreateInfo vi{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = depth;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = vkCtx->getDepthFormat();
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkImageView depthView = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &vi, nullptr, &depthView) != VK_SUCCESS) return VK_NULL_HANDLE;
    sceneWaterRetired_[slot].view = depthView;
    const VkImageView views[2] = {color, depthView};
    VkFramebufferCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = pass;
    fci.attachmentCount = 2;
    fci.pAttachments = views;
    fci.width = extent.width;
    fci.height = extent.height;
    fci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(device, &fci, nullptr, &fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    sceneWaterRetired_[slot].framebuffer = fb;
    return fb;
}

void Renderer::destroySceneWaterRetired(uint32_t slot) {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    SceneWaterRetired& r = sceneWaterRetired_[slot % 2];
    if (r.framebuffer) vkDestroyFramebuffer(device, r.framebuffer, nullptr);
    if (r.view) vkDestroyImageView(device, r.view, nullptr);
    r = SceneWaterRetired{};
}

// Water can leave the scene pass only when there is a continuation pass to draw
// it in, which excludes MSAA. Everything else in the frame is unaffected.
void Renderer::syncSwimEffectsTargetPass() {
    if (!vkCtx) return;

    // Default: they stay in the scene pass, matching its MSAA sample count.
    VkRenderPass pass = vkCtx->getImGuiRenderPass();
    VkSampleCountFlagBits samples = vkCtx->getMsaaSamples();
    swimEffectsDrawWithWater_ = false;
    minimapDrawsWithWater_ = false;

    if (waterDrawsInContinuePass()) {
        // Both continuation passes are single-sampled: with MSAA the water draws
        // into the resolved image, and without it there is nothing to resolve.
        if (vkCtx->getMsaaSamples() > VK_SAMPLE_COUNT_1_BIT) {
            pass = waterRenderer->getWater1xRenderPass();
        } else {
            pass = vkCtx->getSceneContinueRenderPass();
        }
        if (pass != VK_NULL_HANDLE) {
            samples = VK_SAMPLE_COUNT_1_BIT;
            swimEffectsDrawWithWater_ = true;
            minimapDrawsWithWater_ = true;
        } else {
            pass = vkCtx->getImGuiRenderPass();
        }
    }

    if (swimEffects) swimEffects->setTargetPass(pass, samples);
    // The weather follows the water only for the late particle pass (see
    // weatherDrawsWithWater_): stock draws particles, water, then weather.
    weatherDrawsWithWater_ = softParticlesFrame_ && swimEffectsDrawWithWater_;
    if (weather) weather->setTargetPass(weatherDrawsWithWater_ ? pass : VK_NULL_HANDLE, samples);
    // The minimap for the same reason as the spray, and it is the more visible
    // of the two: it is a fixed disc in the corner of the screen, so any water
    // on screen behind it painted straight over the terrain it draws. The
    // spray at least only lost against water it was thrown off.
    //
    // Only when water has actually left the scene pass. Where it has not - MSAA
    // onto an off-screen scene, or no continuation pass at all - water is drawn
    // before this and the minimap is already on top of it.
    //
    // With soft particles on and water still in the scene pass, the late
    // particle pass comes after everything that pass holds - the minimap
    // included - so the minimap moves to the end of the late pass, which is
    // single-sampled and has the overlay pass's attachments.
    minimapDrawsLate_ = softParticlesFrame_ && !minimapDrawsWithWater_ &&
                        vkCtx->getOverlayRenderPass() != VK_NULL_HANDLE;
    if (minimap) {
        if (minimapDrawsLate_) {
            // Into the late pass over the scene image, in the scene's format.
            minimap->setTargetPass(vkCtx->getSceneOverlayRenderPass(), VK_SAMPLE_COUNT_1_BIT);
        } else {
            minimap->setTargetPass(minimapDrawsWithWater_ ? pass : VK_NULL_HANDLE,
                                   samples);
        }
    }
}

void Renderer::renderPostSceneOverlays(VkCommandBuffer cmd,
                                       game::GameHandler* gameHandler) {
    // Ghost world: the post pass desaturates the scene (fxaa.frag ghostWorld).
    // Only while that pass is missing - its first frame, or a device that
    // could not make it - does the old cold blue-grey quad stand in.
    const bool ghostInPostPass = postProcessPipeline_ &&
                                 postProcessPipeline_->isGhostDesaturationActive();
    if (ghostMode_ && !ghostInPostPass && overlaySystem_) {
        overlaySystem_->renderOverlay(glm::vec4(0.30f, 0.35f, 0.42f, 0.45f), cmd);
    }

    // Brightness overlay, applied before the minimap so it doesn't affect UI.
    if (overlaySystem_) {
        float br = postProcessPipeline_ ? postProcessPipeline_->getBrightness() : 1.0f;
        if (br < 0.99f) {
            // Black overlay at alpha (1-br) darkens as scene*br (a true multiply).
            overlaySystem_->renderOverlay(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f - br), cmd);
        } else if (br > 1.01f) {
            // Multiply scene by br instead of lerping to white (washout). The
            // water refraction shader divides br back out of its captured
            // scene sample so this doesn't compound through the history.
            overlaySystem_->renderBrightnessScale(br, cmd);
        }
    }

    // Unless it is following water into the pass after this one, where it is
    // drawn instead - see renderMinimapOverlay's caller below the water.
    if (!minimapDrawsWithWater_ && !minimapDrawsLate_) renderMinimapOverlay(cmd, gameHandler);
}

/// The minimap disc, over whatever has been drawn so far.
void Renderer::renderMinimapOverlay(VkCommandBuffer cmd,
                                    game::GameHandler* gameHandler) {
    if (minimap && minimap->isEnabled() && camera && window) {
        glm::vec3 minimapCenter = camera->getPosition();
        if (cameraController && cameraController->isThirdPerson())
            minimapCenter = characterPosition;
        float minimapPlayerOrientation = 0.0f;
        bool hasMinimapPlayerOrientation = false;
        if (cameraController) {
            // Render-space character yaw faces north at 180 degrees; the
            // minimap shader arrow faces north at 0. Match the mirrored
            // minimap texture by flipping the visual arrow vertically.
            minimapPlayerOrientation = glm::radians(characterYaw);
            hasMinimapPlayerOrientation = true;
        } else if (gameHandler) {
            // movementInfo.orientation is canonical yaw: north is 0, east is +pi/2.
            // Match the mirrored minimap texture by flipping the visual
            // arrow vertically.
            minimapPlayerOrientation = glm::pi<float>() - gameHandler->getMovementInfo().orientation;
            hasMinimapPlayerOrientation = true;
        }
        minimap->render(cmd, *camera, minimapCenter,
                        window->getWidth(), window->getHeight(),
                        minimapPlayerOrientation, hasMinimapPlayerOrientation);
    }
}

bool Renderer::waterDrawsInContinuePass() const {
    if (!waterRenderer || !vkCtx) return false;
    // The 1x water pass targets the swapchain directly, so it cannot serve a
    // frame whose scene went to an off-screen post-processing target - unless
    // soft particles are on, whose late pass must come before the water
    // (water_pass_order.hpp); then it draws over the resolved scene colour and
    // depth (sceneWater1xFramebuffer). With them off nothing here changed.
    water_pass_order::Frame f;
    f.msaa = vkCtx->getMsaaSamples() > VK_SAMPLE_COUNT_1_BIT;
    f.offscreenScene = postProcessPipeline_ && postProcessPipeline_->getSceneFramebuffer() != VK_NULL_HANDLE;
    f.softParticles = softParticlesFrame_;
    f.water1xPass = waterRenderer->hasWater1xPass();
    f.water1xKeepDepth = waterRenderer->getWater1xKeepDepthRenderPass() != VK_NULL_HANDLE;
    f.sceneContinuePass = vkCtx->getSceneContinueRenderPass() != VK_NULL_HANDLE;
    return water_pass_order::waterAfterScene(f);
}

// Soft particles: whether this frame has a late particle pass.
//
// It needs the pipelines (built the first time the row is on), the overlay
// pass to draw in, and a one-sample depth of the scene to read - the scene's
// own without MSAA, its resolve with it. A device that cannot resolve depth
// under MSAA has none, and its particles stay in the scene pass as stock.
// Everything is let go a few frames after the row goes off, once no frame in
// flight can still be drawing with it.
void Renderer::decideSoftParticles() {
    softParticlesFrame_ = false;
    softParticlesLateDone_ = false;
    bool want = enh_.softParticles && m2Renderer && vkCtx &&
                vkCtx->getOverlayRenderPass() != VK_NULL_HANDLE;
    if (want) {
        const bool offscreen = postProcessPipeline_ &&
                               postProcessPipeline_->getSceneFramebuffer() != VK_NULL_HANDLE;
        if (offscreen) {
            want = postProcessPipeline_->getSceneColorImageView() != VK_NULL_HANDLE &&
                   postProcessPipeline_->getSceneDepthImage() != VK_NULL_HANDLE &&
                   !postProcessPipeline_->sceneDepthIsMsaa();
        } else {
            want = !vkCtx->getOverlayFramebuffers().empty() && !vkCtx->isDepthCopySourceMsaa();
        }
    }
    if (want) {
        if (!softParticlePass_) softParticlePass_ = std::make_unique<SoftParticlePass>();
        want = softParticlePass_->initialize(vkCtx) &&
               m2Renderer->ensureSoftParticlePipelines(vkCtx->getSceneOverlayRenderPass(),
                                                       softParticlePass_->depthLayout());
    }
    softParticlesFrame_ = want;
    if (m2Renderer) m2Renderer->setSoftParticlesDeferred(want);
    // The water has to come after this frame's late pass (water_pass_order),
    // and under MSAA that needs the 1x water pass - which is otherwise made
    // only when the sample count or the swapchain changes, so a session that
    // started with MSAA has none. Made here, once it is wanted.
    if (want && waterRenderer && !waterRenderer->hasWater1xPass() &&
        vkCtx->getMsaaSamples() > VK_SAMPLE_COUNT_1_BIT &&
        vkCtx->getDepthResolveImageView() != VK_NULL_HANDLE) {
        setupWater1xPass();
    }
    if (want) {
        softParticlesOffFrames_ = 0;
    } else if (softParticlePass_ && ++softParticlesOffFrames_ > 8) {
        // Eight frames on, every command buffer that used them has finished.
        if (m2Renderer) m2Renderer->releaseSoftParticlePipelines();
        softParticlePass_.reset();
        softParticlesOffFrames_ = 0;
    }
}

void Renderer::recordSoftParticlePass(VkCommandBuffer cmd) {
    if (!softParticlesFrame_ || softParticlesLateDone_ || !worldDrawnThisFrame_ ||
        !softParticlePass_ || !m2Renderer || !vkCtx || cmd == VK_NULL_HANDLE) {
        return;
    }
    softParticlesLateDone_ = true;

    SoftParticlePass::Target t;
    t.pass = vkCtx->getSceneOverlayRenderPass();
    if (postProcessPipeline_ && postProcessPipeline_->getSceneFramebuffer() != VK_NULL_HANDLE) {
        t.colorView = postProcessPipeline_->getSceneColorImageView();
        t.extent = postProcessPipeline_->getSceneRenderExtent();
        t.depth = postProcessPipeline_->getSceneDepthImage();
    } else {
        const auto& fbs = vkCtx->getOverlayFramebuffers();
        if (currentImageIndex < fbs.size()) t.framebuffer = fbs[currentImageIndex];
        t.extent = vkCtx->getSwapchainExtent();
        t.depth = vkCtx->getDepthCopySourceImage();
    }
    const VkDescriptorSet perFrameSet = perFrameDescSets[vkCtx->getCurrentFrame()];
    const bool recorded = softParticlePass_->record(cmd, t, [&](VkCommandBuffer c, VkDescriptorSet depthSet) {
        m2Renderer->renderSoftParticles(c, perFrameSet, depthSet);
        // Over the particles, as it was over everything in the scene pass.
        if (minimapDrawsLate_ && minimap.get() == passBuiltFor_[1]) renderMinimapOverlay(c, softParticlesGameHandler_);
    });
    if (recorded) {
        vkCtx->gpuMark(cmd, "soft particles");
    } else {
        static bool said = false;
        if (!said) {
            said = true;
            LOG_WARNING("Soft particles: the late pass could not be recorded - "
                        "blended particles were not drawn this frame");
        }
    }
}

// initPostProcess(), resizePostProcess(), shutdownPostProcess() removed -
// post-process pipeline is now handled by Vulkan (Phase 6 cleanup).

void Renderer::setActiveMapName(const std::string& name) {
    if (terrainManager) terrainManager->setMapName(name);
    if (minimap) minimap->setMapName(name);
    if (worldMap) worldMap->setMapName(name);
    if (wmoRenderer) wmoRenderer->setMapName(name);
}

void Renderer::setCharacterAnimSoundEventCallback(CharacterAnimSoundEventCallback cb) {
    characterAnimSoundEventCallback_ = std::move(cb);
    installCharacterAnimEventCallback();
}

bool Renderer::initializeRenderers(pipeline::AssetManager* assetManager, const std::string& mapName) {
    if (!assetManager) {
        LOG_ERROR("Asset manager is null");
        return false;
    }

    LOG_INFO("Initializing renderers for map: ", mapName);

    // Scan for custom zones on first initialization
    if (customZones_.empty()) {
        customZones_ = pipeline::CustomZoneDiscovery::scan({"custom_zones", "output"});
        if (!customZones_.empty()) {
            LOG_INFO("=== Custom Zones Available ===");
            for (const auto& z : customZones_) {
                LOG_INFO("  ", z.name, " (", z.directory, ")",
                         z.hasCreatures ? " [NPCs]" : "",
                         z.hasQuests ? " [Quests]" : "");
            }
            LOG_INFO("==============================");
        }
    }

    // Create terrain renderer if not already created
    if (!terrainRenderer) {
        terrainRenderer = std::make_unique<TerrainRenderer>();
        if (!terrainRenderer->initialize(vkCtx, perFrameSetLayout, assetManager)) {
            LOG_ERROR("Failed to initialize terrain renderer");
            terrainRenderer.reset();
            return false;
        }
        terrainRenderer->setRtScene(rtScene_.get());
        if (shadowRenderPass != VK_NULL_HANDLE) {
            terrainRenderer->initializeShadow(shadowRenderPass);
        }
    } else if (!terrainRenderer->hasShadowPipeline() && shadowRenderPass != VK_NULL_HANDLE) {
        terrainRenderer->initializeShadow(shadowRenderPass);
    }

    // Create water renderer if not already created
    if (!waterRenderer) {
        waterRenderer = std::make_unique<WaterRenderer>();
        if (!waterRenderer->initialize(vkCtx, perFrameSetLayout)) {
            LOG_ERROR("Failed to initialize water renderer");
            waterRenderer.reset();
        }
    }
    // The LiquidType.dbc texture sequences the water is drawn with. Loads once.
    if (waterRenderer) waterRenderer->loadLiquidTextures(assetManager);
    // Textures\WaterPoop02.blp, the stock underwater motes. Loads once.
    if (swimEffects) swimEffects->loadMoteTexture(assetManager);

    // Create minimap if not already created
    if (!minimap) {
        minimap = std::make_unique<Minimap>();
        if (!minimap->initialize(vkCtx, perFrameSetLayout)) {
            LOG_ERROR("Failed to initialize minimap");
            minimap.reset();
        }
    }

    // Create world map if not already created
    if (!worldMap) {
        worldMap = std::make_unique<WorldMap>();
        if (!worldMap->initialize(vkCtx, assetManager)) {
            LOG_ERROR("Failed to initialize world map");
            worldMap.reset();
        }
    }

    // Create M2, WMO, and Character renderers
    if (!m2Renderer) {
        m2Renderer = std::make_unique<M2Renderer>();
        if (!m2Renderer->initialize(vkCtx, perFrameSetLayout, assetManager))
            LOG_ERROR("M2Renderer initialization failed");
        m2Renderer->setRtScene(rtScene_.get());
        m2Renderer->setWaterHeightQuery([this](float x, float y, float z) -> std::optional<float> {
            return waterRenderer ? waterRenderer->getNearestWaterHeightAt(x, y, z, 3.0f)
                                 : std::nullopt;
        });
        if (swimEffects) {
            swimEffects->setM2Renderer(m2Renderer.get());
        }
        // Initialize SpellVisualSystem once M2Renderer is available (§4.4)
        if (!spellVisualSystem_) {
            spellVisualSystem_ = std::make_unique<SpellVisualSystem>();
            spellVisualSystem_->initialize(m2Renderer.get(), this);
        }
    }

    // The original client's skies are camera-centered M2 models selected
    // through LightParams and LightSkybox, on every map that names one - not
    // Outland alone, which is where this was built and where it stayed. The
    // lookup was opened to all maps and this was not, so it went on doing
    // nothing anywhere else: without the renderer there is nothing to draw
    // into.
    //
    // It keeps its own no-depth renderer so the sky draws behind terrain and
    // never enters world collision.
    if (!skyboxModelRenderer_) {
        skyboxModelRenderer_ = std::make_unique<M2Renderer>();
        skyboxModelRenderer_->setSkyMode(true);
        if (!skyboxModelRenderer_->initialize(vkCtx, perFrameSetLayout, assetManager)) {
            LOG_WARNING("Sky M2 renderer initialization failed");
            skyboxModelRenderer_.reset();
        }
    }
    if (!skyStarsRenderer_) {
        skyStarsRenderer_ = std::make_unique<M2Renderer>();
        skyStarsRenderer_->setSkyMode(true);
        if (!skyStarsRenderer_->initialize(vkCtx, perFrameSetLayout, assetManager)) {
            LOG_WARNING("Sky stars renderer initialization failed");
            skyStarsRenderer_.reset();
        }
    }

    // HiZ occlusion culling disabled - the pyramid build + blocking fence was
    // the main frame-rate bottleneck.  GPU frustum culling alone provides good
    // draw-call reduction without the per-frame GPU stall.  HiZ can be re-
    // enabled once the pyramid build is moved to an async compute queue.
    if (!wmoRenderer) {
        wmoRenderer = std::make_unique<WMORenderer>();
        if (!wmoRenderer->initialize(vkCtx, perFrameSetLayout, assetManager))
            LOG_ERROR("WMORenderer initialization failed");
        // Before the first model loads: the room light bakes at load (E12).
        wmoRenderer->setIndoorProbes(enh_.indoorLight);
        wmoRenderer->setRtScene(rtScene_.get());
        if (shadowRenderPass != VK_NULL_HANDLE) {
            if (!wmoRenderer->initializeShadow(shadowRenderPass))
                LOG_WARNING("WMO shadow pipeline initialization failed");
        }
    }

    // Renderer components can be recreated during map transitions. Restore the
    // configured view distance instead of falling back to their defaults.
    setViewDistance(viewDistance_);
    setSharpStars(sharpStars_);

    // Initialize shadow pipelines for M2 if not yet done
    if (m2Renderer && shadowRenderPass != VK_NULL_HANDLE && !m2Renderer->hasShadowPipeline()) {
        if (!m2Renderer->initializeShadow(shadowRenderPass))
            LOG_WARNING("M2 shadow pipeline initialization failed");
    }
    if (!characterRenderer) {
        characterRenderer = std::make_unique<CharacterRenderer>();
        if (!characterRenderer->initialize(vkCtx, perFrameSetLayout, assetManager))
            LOG_ERROR("CharacterRenderer initialization failed");
        if (shadowRenderPass != VK_NULL_HANDLE) {
            if (!characterRenderer->initializeShadow(shadowRenderPass))
                LOG_WARNING("Character shadow pipeline initialization failed");
        }
        installCharacterAnimEventCallback();
    }
    // Weapon glows are particle emitters: M2Renderer draws them.
    if (characterRenderer) characterRenderer->setEffectRenderer(m2Renderer.get());

    // Initialize AnimationController (§4.2)
    if (!animationController_) {
        animationController_ = std::make_unique<AnimationController>();
        animationController_->initialize(this);
    }

    // Create and initialize terrain manager
    if (!terrainManager) {
        terrainManager = std::make_unique<TerrainManager>();
        if (!terrainManager->initialize(assetManager, terrainRenderer.get())) {
            LOG_ERROR("Failed to initialize terrain manager");
            terrainManager.reset();
            return false;
        }
        // Set water renderer for terrain streaming
        if (waterRenderer) {
            terrainManager->setWaterRenderer(waterRenderer.get());
        }
        // Set M2 renderer for doodad loading during streaming
        if (m2Renderer) {
            terrainManager->setM2Renderer(m2Renderer.get());
        }
        // Set WMO renderer for building loading during streaming
        if (wmoRenderer) {
            terrainManager->setWMORenderer(wmoRenderer.get());
        }
        // A WMO's child M2 doodads - a ship's sails, its paddlewheel - are moved
        // and destroyed through this pointer. It was never set, so every one of
        // those paths was behind a null check that never passed: the doodads
        // were created at the origin, never given their parent's transform, and
        // so drawn at the middle of the map rather than on the ship. Static
        // world doodads were unaffected, because terrain streaming places those
        // at their world position itself and never goes through the parent.
        if (wmoRenderer && m2Renderer) {
            wmoRenderer->setM2Renderer(m2Renderer.get());
        }
        // Set ambient sound manager for environmental audio emitters
        if (audioCoordinator_->getAmbientSoundManager()) {
            terrainManager->setAmbientSoundManager(audioCoordinator_->getAmbientSoundManager());
        }
        // Doodads' own $DSL / $DSO sound events
        terrainManager->setWorldSoundEmitters(audioCoordinator_->getWorldSoundEmitters());
        // Pass asset manager to character renderer for texture loading
        if (characterRenderer) {
            characterRenderer->setAssetManager(assetManager);
        }
        // Wire asset manager to minimap for tile texture loading
        if (minimap) {
            minimap->setAssetManager(assetManager);
        }
        // Wire terrain manager, WMO renderer, and water renderer to camera controller
        if (cameraController) {
            cameraController->setTerrainManager(terrainManager.get());
            if (wmoRenderer) {
                cameraController->setWMORenderer(wmoRenderer.get());
            }
            if (m2Renderer) {
                cameraController->setM2Renderer(m2Renderer.get());
            }
            if (waterRenderer) {
                cameraController->setWaterRenderer(waterRenderer.get());
            }
        }
    }

    // Set map name on sub-renderers
    setActiveMapName(mapName);

    // Initialize audio managers
    if (audioCoordinator_->getMusicManager() && assetManager && !cachedAssetManager) {
        audio::AudioEngine::instance().setAssetManager(assetManager);
        audioCoordinator_->loadSoundData(assetManager);
        audioCoordinator_->getMusicManager()->initialize(assetManager);
        if (audioCoordinator_->getFootstepManager()) {
            audioCoordinator_->getFootstepManager()->initialize(assetManager);
        }
        if (audioCoordinator_->getActivitySoundManager()) {
            audioCoordinator_->getActivitySoundManager()->initialize(assetManager);
        }
        if (audioCoordinator_->getMountSoundManager()) {
            audioCoordinator_->getMountSoundManager()->initialize(assetManager);
        }
        if (audioCoordinator_->getNpcVoiceManager()) {
            audioCoordinator_->getNpcVoiceManager()->initialize(assetManager);
        }
        if (audioCoordinator_->getPlayerVoiceManager()) {
            audioCoordinator_->getPlayerVoiceManager()->initialize(assetManager);
        }
        if (!deferredWorldInitEnabled_) {
            if (audioCoordinator_->getAmbientSoundManager()) {
                audioCoordinator_->getAmbientSoundManager()->initialize(assetManager);
            }
            if (audioCoordinator_->getUiSoundManager()) {
                audioCoordinator_->getUiSoundManager()->initialize(assetManager);
            }
            if (audioCoordinator_->getCombatSoundManager()) {
                audioCoordinator_->getCombatSoundManager()->initialize(assetManager);
            }
            if (audioCoordinator_->getSpellSoundManager()) {
                audioCoordinator_->getSpellSoundManager()->initialize(assetManager);
            }
            if (audioCoordinator_->getMovementSoundManager()) {
                audioCoordinator_->getMovementSoundManager()->initialize(assetManager);
            }
            if (footprintRenderer) {
                if (!footprintRenderer->initialize(this, vkCtx, perFrameSetLayout, assetManager))
                    LOG_WARNING("Footprint renderer initialization failed (non-fatal)");
            }

            if (core::envFlagEnabled("WOWEE_PREWARM_ZONE_MUSIC", false)) {
                if (zoneManager) {
                    for (const auto& musicPath : zoneManager->getAllMusicPaths()) {
                        audioCoordinator_->getMusicManager()->preloadMusic(musicPath);
                    }
                }
                const std::string tavernRemix =
                    game::ZoneManager::resolveOriginalMusicFile("TavernAllianceREMIX.mp3");
                const std::vector<std::string> tavernTracks = {
                    tavernRemix.empty()
                        ? std::string("Sound\\Music\\ZoneMusic\\TavernAlliance\\TavernAlliance01.mp3")
                        : tavernRemix,
                    "Sound\\Music\\ZoneMusic\\TavernAlliance\\TavernAlliance02.mp3",
                    "Sound\\Music\\ZoneMusic\\TavernHuman\\RA_HumanTavern1A.mp3",
                    "Sound\\Music\\ZoneMusic\\TavernHuman\\RA_HumanTavern2A.mp3",
                };
                for (const auto& musicPath : tavernTracks) {
                    audioCoordinator_->getMusicManager()->preloadMusic(musicPath);
                }
            }
        } else {
            deferredWorldInitPending_ = true;
            deferredWorldInitStage_ = 0;
            deferredWorldInitCooldown_ = 0.25f;
        }

        cachedAssetManager = assetManager;

        // Enrich zone music from DBC if not already done (e.g. asset manager was null at init).
        if (zoneManager && assetManager) {
            zoneManager->enrichFromDBC(assetManager);
        }
    }

    // Snap camera to ground
    if (cameraController) {
        cameraController->reset();
    }

    return true;
}

bool Renderer::loadTestTerrain(pipeline::AssetManager* assetManager, const std::string& adtPath) {
    if (!assetManager) {
        LOG_ERROR("Asset manager is null");
        return false;
    }

    LOG_INFO("Loading test terrain: ", adtPath);

    // Extract map name from ADT path for renderer initialization
    std::string mapName;
    {
        size_t lastSep = adtPath.find_last_of("\\/");
        if (lastSep != std::string::npos) {
            std::string filename = adtPath.substr(lastSep + 1);
            size_t firstUnderscore = filename.find('_');
            mapName = filename.substr(0, firstUnderscore != std::string::npos ? firstUnderscore : filename.size());
        }
    }

    // Initialize all sub-renderers
    if (!initializeRenderers(assetManager, mapName)) {
        return false;
    }

    // Parse tile coordinates from ADT path
    // Format: World\Maps\{MapName}\{MapName}_{X}_{Y}.adt
    int tileX = 32, tileY = 49;  // defaults
    {
        size_t lastSep = adtPath.find_last_of("\\/");
        if (lastSep != std::string::npos) {
            std::string filename = adtPath.substr(lastSep + 1);
            size_t firstUnderscore = filename.find('_');
            if (firstUnderscore != std::string::npos) {
                size_t secondUnderscore = filename.find('_', firstUnderscore + 1);
                if (secondUnderscore != std::string::npos) {
                    size_t dot = filename.find('.', secondUnderscore);
                    if (dot != std::string::npos) {
                        try {
                            tileX = std::stoi(filename.substr(firstUnderscore + 1, secondUnderscore - firstUnderscore - 1));
                            tileY = std::stoi(filename.substr(secondUnderscore + 1, dot - secondUnderscore - 1));
                        } catch (...) {
                            LOG_WARNING("Failed to parse tile coords from: ", filename);
                        }
                    }
                }
            }
        }
    }

    LOG_INFO("Enqueuing initial tile [", tileX, ",", tileY, "] via terrain manager");

    // Enqueue the initial tile for async loading (avoids long sync stalls)
    if (!terrainManager->enqueueTile(tileX, tileY)) {
        LOG_ERROR("Failed to enqueue initial tile [", tileX, ",", tileY, "]");
        return false;
    }

    terrainLoaded = true;

    LOG_INFO("Test terrain loaded successfully!");
    LOG_INFO("  Chunks: ", terrainRenderer->getChunkCount());
    LOG_INFO("  Triangles: ", terrainRenderer->getTriangleCount());

    return true;
}

void Renderer::setWireframeMode(bool enabled) {
    if (terrainRenderer) {
        terrainRenderer->setWireframe(enabled);
    }
}

// One line naming how far each of the three actually drew this frame.
//
// "Distant objects float with no terrain" is a disagreement between two
// distances, and every attempt to settle it by reading the code picked the
// wrong one of the three places that compute it. This prints the answer:
// if terrain stops short of the doodads the fault is in loading the tiles,
// and if the doodads run past the setting the fault is in the cull. It is at
// warning because the default log is warnings only, and it prints on a change
// of half a tile rather than every frame.
void Renderer::logViewDistanceDiag() {
    static const bool enabled = std::getenv("WOWEE_VIEW_DIAG") != nullptr;
    if (!enabled) return;

    const float terrainFurthest = terrainRenderer
        ? terrainRenderer->getFurthestDrawnDistance() : 0.0f;
    const float m2Furthest = m2Renderer
        ? m2Renderer->getFurthestDrawnDistance() : 0.0f;

    if (std::abs(terrainFurthest - diagTerrainFurthest_) < 266.0f &&
        std::abs(m2Furthest - diagM2Furthest_) < 266.0f) {
        return;
    }
    diagTerrainFurthest_ = terrainFurthest;
    diagM2Furthest_ = m2Furthest;

    LOG_WARNING("view distance ", static_cast<int>(viewDistance_),
                ": terrain drew to ", static_cast<int>(terrainFurthest),
                ", doodads to ", static_cast<int>(m2Furthest),
                ", tiles loaded to ", getTerrainLoadRadius(),
                " (", static_cast<int>(getTerrainLoadRadius() *
                                       core::coords::TILE_SIZE), ")");
}

void Renderer::setSharpStars(bool enabled) {
    sharpStars_ = enabled;
    // Two halves of one switch: the sky model stops drawing its star layer and
    // the client's own point stars take its place. Setting either alone gives a
    // sky with no stars or a sky with two sets of them.
    if (m2Renderer) m2Renderer->setSuppressBakedStars(sharpStars_);
    if (skySystem) skySystem->setProceduralStarsEnabled(sharpStars_);
}

void Renderer::setViewDistance(float distance) {
    viewDistance_ = glm::clamp(distance, 400.0f, 2400.0f);

    if (terrainRenderer) terrainRenderer->setViewDistance(viewDistance_);
    if (wmoRenderer) wmoRenderer->setViewDistance(viewDistance_);
    if (m2Renderer) m2Renderer->setViewDistance(viewDistance_);
    // The fog finishes where the drawing does; see LightingManager::update.
    if (lightingManager) lightingManager->setViewDistance(viewDistance_);
    if (terrainManager) {
        terrainManager->setLoadRadius(getTerrainLoadRadius());
        terrainManager->setUnloadRadius(getTerrainUnloadRadius());
    }
}

int Renderer::getTerrainLoadRadius() const {
    constexpr float kAdtTileSize = core::coords::TILE_SIZE;
    // The far field's building silhouettes stand on the loaded tiles: six
    // rings hold the 3000 yards they reach.
    const int floor = farField_ ? 6 : 2;
    return glm::clamp(static_cast<int>(std::ceil(viewDistance_ / kAdtTileSize)) + 1, floor, 6);
}
void Renderer::renderHUD() {
    if (currentCmd == VK_NULL_HANDLE) return;
    if (performanceHUD && camera) {
        performanceHUD->render(this, camera.get());
    }
}

// ──────────────────────────────────────────────────────
// Shadow mapping helpers
// ──────────────────────────────────────────────────────

// initShadowMap() and compileShadowShader() removed - shadow resources now created
// in createPerFrameResources() as part of the Vulkan shadow infrastructure.

glm::vec3 Renderer::shadowRayDirection() const {
    // Use active lighting direction so shadow projection matches main shading.
    // Fragment shaders derive lighting with `ldir = normalize(-lightDir.xyz)`,
    // therefore shadow rays must use -directionalDir to stay aligned.
    glm::vec3 sunDir = glm::normalize(glm::vec3(-0.3f, -0.7f, -0.6f));
    if (lightingManager) {
        const auto& lighting = lightingManager->getLightingParams();
        float ldirLenSq = glm::dot(lighting.directionalDir, lighting.directionalDir);
        if (ldirLenSq > 1e-6f) {
            sunDir = -lighting.directionalDir * glm::inversesqrt(ldirLenSq);
        }
    }
    // Shadow camera expects light rays pointing downward in render space (Z up).
    // Some profiles/opcode paths provide the opposite convention; normalize here.
    if (sunDir.z > 0.0f) {
        sunDir = -sunDir;
    }
    // Keep a minimum downward component so the frustum doesn't collapse at grazing angles.
    if (sunDir.z > -0.15f) {
        sunDir.z = -0.15f;
        sunDir = glm::normalize(sunDir);
    }
    return sunDir;
}

uint64_t Renderer::shadowContentKey() const {
    // Not a hash of the world, only of what tells it has changed: a tile's
    // chunks streaming in or out, a building placed or removed, a doodad
    // placed, removed, turned mover or given a model (M2Renderer's own count).
    uint64_t key = 0;
    if (terrainRenderer) key ^= static_cast<uint64_t>(terrainRenderer->getChunkCount());
    if (wmoRenderer) key ^= static_cast<uint64_t>(wmoRenderer->getInstanceCount()) << 20;
    if (m2Renderer) key ^= static_cast<uint64_t>(m2Renderer->shadowContentGeneration()) << 40;
    return key;
}

void Renderer::planShadowFrame() {
    shadowPartsThisFrame_ = 0;
    shadowRedrawEndsThisFrame_ = false;
    nearShadowOn_ = false;
    static const bool castersWhenOff = core::envFlagEnabled("WOWEE_SHADOW_CASTERS_WHEN_OFF", false);
    // WOWEE_SHADOW_SPLIT=0 draws one map with every caster every frame, as
    // before the split; the frame then reads the image it has just drawn.
    static const bool split = core::envFlagEnabled("WOWEE_SHADOW_SPLIT", true);
    shadowSplit_ = split;
    const uint32_t slot = vkCtx ? vkCtx->getCurrentFrame() : 0;
    if (!shadowsEnabled && !castersWhenOff) {
        // Off: nothing is drawn, and the kept map is not trusted again - the
        // world has gone on changing without it.
        shadowFrontKey_.valid = false;
        shadowRedraw_.active = false;
        if (shadowDepthImage[0] != VK_NULL_HANDLE) bindWorldShadowMap(slot);
        return;
    }

    // Zero matrix means character position isn't set yet - skip the casters
    // (but not the clear and the transition; see renderShadowPass).
    const glm::vec3 player = characterPosition;
    if (!shadowCenterInitialized) {
        if (glm::dot(player, player) < 1.0f) {
            lightSpaceMatrix = glm::mat4(0.0f);
            if (shadowDepthImage[0] != VK_NULL_HANDLE) bindWorldShadowMap(slot);
            return;
        }
        shadowCenterInitialized = true;
    }
    // LightingManager already smooths the directional light every frame, and
    // the projection follows it. Quantizing it into 0.5-degree steps once made
    // the entire 600-yard footprint rotate in a single frame - a wide
    // light-switch flicker. The kept map follows it in steps of a quarter of
    // a degree, a texel or two at the far end of a tall shadow.
    const glm::vec3 sunDir = shadowRayDirection();
    const double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const WorldShadowCachePolicy policy{};

    const auto start = [&](bool all, uint64_t content, float extentScale, float extentPad, uint32_t reason) {
        shadowRedraw_.active = true;
        shadowRedraw_.now = all;
        shadowRedraw_.nextPart = 0;
        shadowRedraw_.target = shadowDepthImage[1] != VK_NULL_HANDLE ? 1u - shadowFront_ : shadowFront_;
        shadowRedraw_.key = WorldShadowCacheKey{.valid = true, .center = player, .sunDir = sunDir,
                                                .shadowDistance = shadowDistance_, .content = content,
                                                .drawnAt = now};
        shadowRedraw_.proj = worldShadowProjection(player, sunDir, shadowDistance_, SHADOW_MAP_SIZE,
                                                   extentScale, extentPad);
        if (reason < 4) ++shadowRedrawCounts_[reason];
    };

    if (!split) {
        start(true, 0, 1.0f, 0.0f, 0);
    } else {
        const uint64_t content = shadowContentKey();
        const WorldShadowRedraw need = worldShadowRedrawNeeded(
            shadowFrontKey_, player, sunDir, shadowDistance_, content, now, policy);
        // A redraw under way goes on to its end, unless the map it replaces
        // is no use at all any more (a teleport, the distance changed).
        if (need == WorldShadowRedraw::Now) {
            start(true, content, policy.extentScale, policy.extentPad, 0);
        } else if (need == WorldShadowRedraw::Spread && !shadowRedraw_.active) {
            const float drift = glm::length(player - shadowFrontKey_.center);
            const uint32_t reason = drift > policy.driftFraction * shadowDistance_ ? 1u
                : (glm::dot(shadowFrontKey_.sunDir, sunDir) <
                   std::cos(glm::radians(policy.maxAngleDegrees)) ? 2u : 3u);
            start(false, content, policy.extentScale, policy.extentPad, reason);
        }

        // The unit map, centred on the player every frame.
        nearShadowProj_ = nearShadowProjection(player, sunDir, nearShadowHalfExtent(shadowDistance_),
                                               SHADOW_NEAR_SIZE);
        nearShadowOn_ = shadowNearImage[0] != VK_NULL_HANDLE;
    }

    if (shadowRedraw_.active) {
        if (shadowRedraw_.now) {
            shadowPartsThisFrame_ = (1u << kWorldShadowPartCount) - 1u;
            shadowRedrawEndsThisFrame_ = true;
        } else {
            shadowPartsThisFrame_ = static_cast<uint8_t>(1u << shadowRedraw_.nextPart);
            shadowRedrawEndsThisFrame_ = shadowRedraw_.nextPart + 1u >= kWorldShadowPartCount;
        }
    }
    if (shadowRedrawEndsThisFrame_) {
        // This frame's shadow pass finishes the map before anything reads
        // it, so this frame reads it already.
        shadowFront_ = shadowRedraw_.target;
        shadowFrontKey_ = shadowRedraw_.key;
        shadowFrontMatrix_ = shadowRedraw_.proj.matrix;
        shadowFrontCenter_ = shadowRedraw_.proj.center;
    }
    lightSpaceMatrix = shadowFrontKey_.valid || !split ? shadowFrontMatrix_ : glm::mat4(0.0f);
    shadowCenter = shadowFrontCenter_;
    bindWorldShadowMap(slot);

    // Said every half minute at info: how often the kept map is drawn again,
    // and why. A count climbing while standing still is a world change
    // asking for it, which the content key says.
    static double lastLog = 0.0;
    if (split && now - lastLog > 30.0) {
        lastLog = now;
        LOG_INFO("Shadow world map redraws: at once ", shadowRedrawCounts_[0], ", walked ",
                 shadowRedrawCounts_[1], ", sun ", shadowRedrawCounts_[2], ", world changed ",
                 shadowRedrawCounts_[3], "; unit map ", SHADOW_NEAR_SIZE, " over +-",
                 nearShadowProj_.halfExtent, " yd, ", nearCasterSpheres_.size(),
                 " casters last frame (units ", nearSphereSplit_[0], ", doodads ",
                 nearSphereSplit_[1] - nearSphereSplit_[0], ", mean radius ", [this] {
                     float sum = 0.0f;
                     for (const auto& sp : nearCasterSpheres_) sum += sp.w;
                     return nearCasterSpheres_.empty() ? 0.0f : sum / static_cast<float>(nearCasterSpheres_.size());
                 }(), ") in ", [this] {
                     int n = 0;
                     for (uint32_t w : nearShadowMask_.words) n += std::popcount(w);
                     return n;
                 }(), " of 4096 cells");
    }
}

void Renderer::bindWorldShadowMap(uint32_t slot) {
    if (slot >= MAX_FRAMES || !vkCtx) return;
    const VkImageView view = shadowDepthView[shadowFront_];
    if (view == VK_NULL_HANDLE || shadowBoundView_[slot] == view) return;
    // This slot's fence has been waited on, so no frame in flight holds its
    // sets: the per-frame one, the reflection one and the fog's.
    VkDescriptorImageInfo info{};
    info.imageView = view;
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet writes[2]{};
    const VkDescriptorSet sets[2] = {perFrameDescSets[slot], reflPerFrameDescSet[slot]};
    uint32_t n = 0;
    for (VkDescriptorSet set : sets) {
        if (set == VK_NULL_HANDLE) continue;
        writes[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[n].dstSet = set;
        writes[n].dstBinding = 1;
        writes[n].descriptorCount = 1;
        writes[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[n].pImageInfo = &info;
        ++n;
    }
    if (n) vkUpdateDescriptorSets(vkCtx->getDevice(), n, writes, 0, nullptr);
    if (volumetricFog_) volumetricFog_->setShadowView(slot, view);
    shadowBoundView_[slot] = view;
}

void Renderer::setupWater1xPass() {
    if (!waterRenderer || !vkCtx) return;
    if (vkCtx->getMsaaSamples() == VK_SAMPLE_COUNT_1_BIT) {
        refreshSwimEffectsPass();  // scene continuation pass covers the water here
        return;
    }
    VkImageView depthView = vkCtx->getDepthResolveImageView();
    if (!depthView) {
        // Without a resolved depth buffer the single-sampled water has nothing
        // to depth test against, so it has to stay in the multisampled pass.
        LOG_WARNING("No depth resolve image available - water stays in the MSAA scene pass");
        refreshSwimEffectsPass();
        return;
    }

    waterRenderer->createWater1xPass(vkCtx->getSceneColorFormat(), vkCtx->getDepthFormat());
    // The swapchain's own 1x framebuffers serve an on-screen MSAA scene, which
    // the HDR scene never is (it is always off-screen, in another format).
    static const std::vector<VkImageView> kNoViews;
    waterRenderer->createWater1xFramebuffers(
        vkCtx->isHdrScene() ? kNoViews : vkCtx->getSwapchainImageViews(), depthView,
        vkCtx->getSwapchainExtent());

    // The spray follows the water into its pass, and this is the first point at
    // which that pass exists - the swim effects were built long before it, back
    // when the only choice was the scene pass.
    refreshSwimEffectsPass();
}

// Re-decide where the spray and the minimap draw, and rebuild whichever one
// the answer moved.
//
// Called between frames only: recreatePipelines() destroys the old pipelines
// outright rather than deferring them, so the wait below is what keeps that
// off a frame in flight. Free when nothing changed, which is every frame but
// a handful, so beginFrame runs it too. The decision depends on whether the
// post-process target exists, and that target is created lazily on the frame
// after the anti-aliasing rebuild that first made the decision. With MSAA and
// FXAA together the rebuild chose the single-sample water pass, the FXAA
// target then moved the water back into the scene pass, and the minimap and
// the spray - still flagged for a pass that no longer ran - were drawn
// nowhere at all. The same happened, in either direction, whenever FXAA was
// toggled in the settings.
void Renderer::refreshSwimEffectsPass() {
    if (!vkCtx) return;
    const bool sprayWasWithWater = swimEffectsDrawWithWater_;
    const bool minimapWasWithWater = minimapDrawsWithWater_;
    const bool minimapWasLate = minimapDrawsLate_;
    const bool weatherWasWithWater = weatherDrawsWithWater_;
    syncSwimEffectsTargetPass();
    bool sprayMoved = swimEffects && swimEffectsDrawWithWater_ != sprayWasWithWater;
    bool minimapMoved = minimap && (minimapDrawsWithWater_ != minimapWasWithWater ||
                                    minimapDrawsLate_ != minimapWasLate);
    bool weatherMoved = weather && weatherDrawsWithWater_ != weatherWasWithWater;
    // Made since the last refresh: built for the scene pass, which is only
    // right if that is where it draws.
    const void* now[3] = {swimEffects.get(), minimap.get(), weather.get()};
    const bool sprayNew = now[0] != passBuiltFor_[0] && swimEffectsDrawWithWater_;
    const bool minimapNew = now[1] != passBuiltFor_[1] && (minimapDrawsWithWater_ || minimapDrawsLate_);
    const bool weatherNew = now[2] != passBuiltFor_[2] && weatherDrawsWithWater_;
    for (int i = 0; i < 3; ++i) passBuiltFor_[i] = now[i];
    sprayMoved = sprayMoved || (swimEffects && sprayNew);
    minimapMoved = minimapMoved || (minimap && minimapNew);
    weatherMoved = weatherMoved || (weather && weatherNew);
    if (!sprayMoved && !minimapMoved && !weatherMoved) return;
    vkDeviceWaitIdle(vkCtx->getDevice());
    if (sprayMoved) swimEffects->recreatePipelines();
    if (minimapMoved) minimap->recreatePipelines();
    if (weatherMoved) weather->recreatePipelines();
}

// ========================= Multithreaded Secondary Command Buffers =========================

bool Renderer::createSecondaryCommandResources() {
    if (!vkCtx) return false;
    VkDevice device = vkCtx->getDevice();
    uint32_t queueFamily = vkCtx->getGraphicsQueueFamily();

    VkCommandPoolCreateInfo poolCI{};
    poolCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCI.queueFamilyIndex = queueFamily;

    // Create worker command pools (one per worker thread)
    for (uint32_t w = 0; w < NUM_WORKERS; ++w) {
        if (vkCreateCommandPool(device, &poolCI, nullptr, &workerCmdPools_[w]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create worker command pool ", w);
            return false;
        }
    }

    // Create main-thread secondary command pool
    if (vkCreateCommandPool(device, &poolCI, nullptr, &mainSecondaryCmdPool_) != VK_SUCCESS) {
        LOG_ERROR("Failed to create main secondary command pool");
        return false;
    }

    // Allocate secondary command buffers
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    allocInfo.commandBufferCount = 1;

    // Each concurrently recorded worker secondary owns a dedicated command pool.
    const uint32_t workerSecondaries[] = { SEC_TERRAIN, SEC_WMO, SEC_CHARS, SEC_M2, SEC_POST,
                                           SEC_WMO_B, SEC_M2_A, SEC_TERRAIN_A };
    static_assert(sizeof(workerSecondaries) / sizeof(workerSecondaries[0]) == NUM_WORKERS);
    for (uint32_t w = 0; w < NUM_WORKERS; ++w) {
        allocInfo.commandPool = workerCmdPools_[w];
        for (uint32_t f = 0; f < MAX_FRAMES; ++f) {
            if (vkAllocateCommandBuffers(device, &allocInfo, &secondaryCmds_[workerSecondaries[w]][f]) != VK_SUCCESS) {
                LOG_ERROR("Failed to allocate worker secondary buffer w=", w, " f=", f);
                return false;
            }
        }
    }

    const uint32_t mainSecondaries[] = { SEC_SKY, SEC_SELECTION, SEC_IMGUI };
    for (uint32_t idx : mainSecondaries) {
        allocInfo.commandPool = mainSecondaryCmdPool_;
        for (uint32_t f = 0; f < MAX_FRAMES; ++f) {
            if (vkAllocateCommandBuffers(device, &allocInfo, &secondaryCmds_[idx][f]) != VK_SUCCESS) {
                LOG_ERROR("Failed to allocate main secondary buffer idx=", idx, " f=", f);
                return false;
            }
        }
    }

    // The unit map's units: one pool of their own, as the job runs while no
    // other recording does but on whichever worker takes it; the movers'
    // secondary is the main thread's.
    if (vkCreateCommandPool(device, &poolCI, nullptr, &unitShadowCmdPool_) != VK_SUCCESS) {
        LOG_ERROR("Failed to create the unit shadow command pool");
        return false;
    }
    for (uint32_t f = 0; f < MAX_FRAMES; ++f) {
        allocInfo.commandPool = unitShadowCmdPool_;
        if (vkAllocateCommandBuffers(device, &allocInfo, &unitShadowCharCmd_[f]) != VK_SUCCESS) return false;
        allocInfo.commandPool = mainSecondaryCmdPool_;
        if (vkAllocateCommandBuffers(device, &allocInfo, &unitShadowMoverCmd_[f]) != VK_SUCCESS) return false;
    }

    parallelRecordingEnabled_ = true;
    LOG_INFO("Multithreaded rendering: ", NUM_WORKERS, " worker threads, ",
             NUM_SECONDARIES, " secondary buffers [ENABLED]");
    return true;
}

void Renderer::destroySecondaryCommandResources() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    vkDeviceWaitIdle(device);

    // Secondary buffers are freed when their pool is destroyed
    for (auto& workerCmdPool : workerCmdPools_) {
        if (workerCmdPool) {
            vkDestroyCommandPool(device, workerCmdPool, nullptr);
            workerCmdPool = VK_NULL_HANDLE;
        }
    }
    if (mainSecondaryCmdPool_) {
        vkDestroyCommandPool(device, mainSecondaryCmdPool_, nullptr);
        mainSecondaryCmdPool_ = VK_NULL_HANDLE;
    }
    if (unitShadowFuture_.valid()) unitShadowFuture_.wait();
    unitShadowJobActive_ = false;
    if (unitShadowCmdPool_) {
        vkDestroyCommandPool(device, unitShadowCmdPool_, nullptr);
        unitShadowCmdPool_ = VK_NULL_HANDLE;
    }
    for (uint32_t f = 0; f < MAX_FRAMES; ++f) {
        unitShadowCharCmd_[f] = VK_NULL_HANDLE;
        unitShadowMoverCmd_[f] = VK_NULL_HANDLE;
    }

    for (auto& arr : secondaryCmds_)
        for (auto& cmd : arr)
            cmd = VK_NULL_HANDLE;

    parallelRecordingEnabled_ = false;
}

VkCommandBuffer Renderer::beginSecondary(uint32_t secondaryIndex) {
    uint32_t frame = vkCtx->getCurrentFrame();
    VkCommandBuffer cmd = secondaryCmds_[secondaryIndex][frame];

    VkCommandBufferInheritanceInfo inheritInfo{};
    inheritInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inheritInfo.renderPass = activeRenderPass_;
    inheritInfo.subpass = 0;
    inheritInfo.framebuffer = activeFramebuffer_;

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
                    | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    beginInfo.pInheritanceInfo = &inheritInfo;

    VkResult result = vkBeginCommandBuffer(cmd, &beginInfo);
    if (result != VK_SUCCESS) {
        LOG_ERROR("vkBeginCommandBuffer failed for secondary ", secondaryIndex,
                  " frame ", frame, " result=", static_cast<int>(result));
    }
    return cmd;
}

void Renderer::setSecondaryViewportScissor(VkCommandBuffer cmd) {
    VkViewport vp{};
    vp.width = static_cast<float>(activeRenderExtent_.width);
    vp.height = static_cast<float>(activeRenderExtent_.height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = activeRenderExtent_;
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

void Renderer::renderReflectionPass() {
    // Not stock: psLiquidWater has no reflection term (docs/re/water.md), so
    // the sky in the water is the "Water reflections" setting, off by default.
    if (waterRenderer) waterRenderer->clearReflection();
    if (!waterReflections_) return;
    if (!waterRenderer || !camera || !waterRenderer->hasReflectionPass() || !waterRenderer->hasSurfaces()) return;
    if (currentCmd == VK_NULL_HANDLE || !reflPerFrameUBOMapped) return;

    // Select the current frame's pre-bound reflection descriptor set
    // (each frame's set was bound to its own shadow depth view at init).
    uint32_t frame = vkCtx->getCurrentFrame();
    VkDescriptorSet reflDescSet = reflPerFrameDescSet[frame];

    // Reflection pass uses 1x MSAA. Scene pipelines must be render-pass-compatible,
    // which requires matching sample counts. Only render scene into reflection when MSAA is off.
    bool canRenderScene = (vkCtx->getMsaaSamples() == VK_SAMPLE_COUNT_1_BIT);

    // Find dominant water height near camera
    const glm::vec3 camPos = camera->getPosition();
    auto waterH = waterRenderer->getDominantWaterHeight(camPos);
    if (!waterH) return;

    float waterHeight = *waterH;

    // Skip reflection if camera is underwater (Z is up)
    if (camPos.z < waterHeight + 0.5f) return;

    // Compute reflected view and oblique projection
    glm::mat4 reflView = WaterRenderer::computeReflectedView(*camera, waterHeight);
    glm::mat4 reflProj = WaterRenderer::computeObliqueProjection(
        camera->getProjectionMatrix(), reflView, waterHeight);

    // Update water renderer's reflection UBO with the reflected viewProj
    waterRenderer->updateReflectionUBO(reflProj * reflView);

    // Fill the reflection per-frame UBO (same as normal but with reflected matrices)
    GPUPerFrameData reflData = currentFrameData;
    reflData.view = reflView;
    reflData.projection = reflProj;
    // Reflected camera position (Z is up)
    glm::vec3 reflPos = camPos;
    reflPos.z = 2.0f * waterHeight - reflPos.z;
    reflData.viewPos = glm::vec4(reflPos, terrainRelief_);
    // The fog volume is the camera's; its sets here bind the neutral one.
    reflData.volumetricParams = glm::vec4(0.0f);
    reflData.rtParams = glm::vec4(0.0f);
    reflData.localLightTileInfo = glm::ivec4(0);  // another camera: every light per pixel
    std::memcpy(reflPerFrameUBOMapped, &reflData, sizeof(GPUPerFrameData));

    // Begin reflection render pass (clears to black; scene rendered if pipeline-compatible)
    if (!waterRenderer->beginReflectionPass(currentCmd)) return;

    if (canRenderScene) {
        // Render scene into reflection texture (sky + terrain + WMO only for perf)
        if (skySystem) {
            auto* reflSkybox = skySystem->getSkybox();
            // The same parameters the main sky is drawn with, from the one
            // helper, so the reflection's dome has the same bands and sun.
            rendering::SkyParams skyParams = rendering::skyParamsFromLighting(
                lightingManager ? lightingManager->getVisualTimeOfDayHours()
                                : (reflSkybox ? reflSkybox->getTimeOfDay() : 12.0f),
                -1.0f, 0.0f,
                lightingManager ? &lightingManager->getLightingParams() : nullptr,
                false);
            // weatherIntensity left at default 0 for reflection pass (no game handler in scope)
            // A flare is an artefact of the lens, so it belongs to the camera
            // and not to what the water is showing it.
            skyParams.sunOcclusion = 1.0f;
            skySystem->render(currentCmd, reflDescSet, *camera, skyParams);
        }
        // Sky only, as the 3.3.5 water reflects. Terrain and buildings were
        // drawn a second time here, culled with the unmirrored camera, for
        // 3.5ms a frame near water and a look the stock client never had.
    }

    waterRenderer->endReflectionPass(currentCmd);
    vkCtx->gpuMark(currentCmd, "water reflection");
}

void Renderer::beginShadowTarget(VkImage image, VkImageView view, VkFramebuffer fb, uint32_t size,
                                 VkImageLayout& layout, bool clear, bool secondaryContents) {
    // Into writable depth. From SHADER_READ_ONLY the barrier waits on the
    // fragment shaders and the fog's compute pass that read the map.
    VkImageMemoryBarrier2 b1{};
    b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b1.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    // A part drawn on top of the last one keeps its contents: a layout of
    // UNDEFINED would let the driver drop them.
    b1.oldLayout = clear ? (layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? layout : VK_IMAGE_LAYOUT_UNDEFINED)
                         : layout;
    b1.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    const bool wasRead = layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b1.srcAccessMask = wasRead ? VK_ACCESS_SHADER_READ_BIT : 0;
    b1.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b1.srcStageMask = wasRead ? (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
                              : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    b1.image = image;
    b1.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
    VkDependencyInfo b1Dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    b1Dep.imageMemoryBarrierCount = 1;
    b1Dep.pImageMemoryBarriers = &b1;
    cmdPipelineBarrier2(currentCmd, b1Dep);
    layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // The first pass converted to dynamic rendering, and the one with least
    // to lose by it: one attachment, no colour, no resolve, a single
    // begin/end, and pipelines nothing else shares. Its render pass declared
    // an EXTERNAL->0 dependency covering the same fragment-read to
    // depth-write hazard that the barrier above covers explicitly.
    const bool dynamicRendering = vkCtx->useDynamicRendering();
    // Said once, at warning level, because a bug report arrives with a
    // warnings-only log and "are the shadows drawn the new way" is the first
    // question this change makes anyone ask.
    static bool saidWhichPath = false;
    if (!saidWhichPath) {
        saidWhichPath = true;
        LOG_WARNING("Shadow pass records with ",
                    dynamicRendering ? "vkCmdBeginRendering" : "a VkRenderPass");
    }
    if (dynamicRendering) {
        VkRenderingAttachmentInfo depthAttach{};
        depthAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttach.imageView = view;
        depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttach.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depthAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttach.clearValue.depthStencil = {.depth = 1.0f, .stencil = 0};
        VkRenderingInfo renderInfo{};
        renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderInfo.renderArea = {.offset = {.x = 0, .y = 0}, .extent = {.width = size, .height = size}};
        renderInfo.layerCount = 1;
        renderInfo.pDepthAttachment = &depthAttach;
        if (secondaryContents) renderInfo.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
        vkCmdBeginRendering(currentCmd, &renderInfo);
    } else {
        VkRenderPassBeginInfo rpInfo{};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpInfo.renderPass = clear ? shadowRenderPass : shadowRenderPassLoad;
        rpInfo.framebuffer = fb;
        rpInfo.renderArea = {.offset = {.x = 0, .y = 0}, .extent = {.width = size, .height = size}};
        VkClearValue clearValue{};
        clearValue.depthStencil = {.depth = 1.0f, .stencil = 0};
        rpInfo.clearValueCount = clear ? 1 : 0;
        rpInfo.pClearValues = clear ? &clearValue : nullptr;
        vkCmdBeginRenderPass(currentCmd, &rpInfo, secondaryContents
                                 ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS
                                 : VK_SUBPASS_CONTENTS_INLINE);
    }
    // Inside a pass of secondaries only vkCmdExecuteCommands may be
    // recorded; each secondary sets its own viewport and scissor.
    if (secondaryContents) return;
    VkViewport vp{.x = 0, .y = 0, .width = static_cast<float>(size), .height = static_cast<float>(size), .minDepth = 0.0f, .maxDepth = 1.0f};
    vkCmdSetViewport(currentCmd, 0, 1, &vp);
    VkRect2D sc{.offset = {.x = 0, .y = 0}, .extent = {.width = size, .height = size}};
    vkCmdSetScissor(currentCmd, 0, 1, &sc);
}

void Renderer::endShadowTarget(VkImage image, VkImageLayout& layout) {
    if (vkCtx->useDynamicRendering()) {
        vkCmdEndRendering(currentCmd);
    } else {
        vkCmdEndRenderPass(currentCmd);
    }
    // DEPTH_STENCIL_ATTACHMENT_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
    VkImageMemoryBarrier2 b2{};
    b2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b2.srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    // Compute too: the volumetric fog samples it right after this pass.
    b2.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    b2.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b2.image = image;
    b2.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
    VkDependencyInfo b2Dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    b2Dep.imageMemoryBarrierCount = 1;
    b2Dep.pImageMemoryBarriers = &b2;
    cmdPipelineBarrier2(currentCmd, b2Dep);
    layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void Renderer::clearShadowImageOnce(VkImage image, VkImageView view, VkFramebuffer fb, uint32_t size,
                                    VkImageLayout& layout, bool& cleared) {
    if (image == VK_NULL_HANDLE) return;
    if (cleared && layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return;
    beginShadowTarget(image, view, fb, size, layout, true);
    endShadowTarget(image, layout);
    cleared = true;
}

namespace {
bool shadowsSkippedByEnv() {
    static const bool skip = std::getenv("WOWEE_SKIP_SHADOWS") != nullptr;
    return skip;
}
// WOWEE_SHADOW_CASTERS_WHEN_OFF=1 draws the casters with shadows off, as the
// pass once did - for measuring what skipping them saves.
bool shadowCastersWhenOff() {
    static const bool on = core::envFlagEnabled("WOWEE_SHADOW_CASTERS_WHEN_OFF", false);
    return on;
}
// WOWEE_SHADOW_CASTERS=terrain,wmo,m2,characters draws only the named
// casters - for finding which of them the pass's time goes to.
bool shadowCasterAllowed(const char* what) {
    static const std::string filter = [] {
        const char* v = std::getenv("WOWEE_SHADOW_CASTERS");
        return std::string(v ? v : "");
    }();
    return filter.empty() || filter.find(what) != std::string::npos;
}
}  // namespace

bool Renderer::unitMapDrawsThisFrame() const {
    if (shadowsSkippedByEnv()) return false;
    if (passAblation_ && passAblation_->skip(AblationPass::Shadows)) return false;
    if (!vkCtx || shadowDepthImage[0] == VK_NULL_HANDLE || currentCmd == VK_NULL_HANDLE) return false;
    if (!shadowsEnabled && !shadowCastersWhenOff()) return false;
    if (lightSpaceMatrix == glm::mat4(0.0f) && shadowPartsThisFrame_ == 0) return false;
    return nearShadowOn_ && shadowNearImage[vkCtx->getCurrentFrame()] != VK_NULL_HANDLE;
}

bool Renderer::unitShadowReceiverView(ShadowReceiverView& seen) const {
    // Where the camera can see the units' shadows: a unit whose shadow
    // falls wholly outside the view is not drawn (shadowCasterSeen).
    // Receivers are taken to lie no more than 40 yards below the lowest
    // of the camera, the player and the unit, and no further along the
    // ray than the map's depth. Not with the water's reflection on, which
    // sees what the camera does not.
    static const bool viewCull = core::envFlagEnabled("WOWEE_UNIT_SHADOW_VIEW_CULL", true);
    if (!viewCull || !camera || enh_.waterReflections) return false;
    Frustum f;
    f.extractFromMatrix(camera->getViewProjectionMatrix());
    for (int i = 0; i < 6; ++i) {
        const Plane& pl = f.getPlane(static_cast<Frustum::Side>(i));
        seen.planes[i] = glm::vec4(pl.normal, pl.distance);
    }
    seen.sunRay = shadowRayDirection();
    seen.floorZ = std::min(camera->getPosition().z, characterPosition.z) - 40.0f;
    seen.maxReach = 2.0f * std::max(2.0f * nearShadowProj_.halfExtent, 300.0f);
    return true;
}

bool Renderer::beginUnitShadowSecondary(VkCommandBuffer cmd, uint32_t frame) {
    VkCommandBufferInheritanceRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO;
    rendering.depthAttachmentFormat = shadowDepthFormat();
    rendering.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkCommandBufferInheritanceInfo inherit{};
    inherit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    if (vkCtx->useDynamicRendering()) {
        inherit.pNext = &rendering;
    } else {
        // The clearing pass and the loading one are compatible; the unit map
        // is begun with the clearing one.
        inherit.renderPass = shadowRenderPass;
        inherit.subpass = 0;
        inherit.framebuffer = shadowNearFramebuffer[frame];
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    begin.pInheritanceInfo = &inherit;
    if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) return false;
    const float size = static_cast<float>(SHADOW_NEAR_SIZE);
    VkViewport vp{.x = 0, .y = 0, .width = size, .height = size, .minDepth = 0.0f, .maxDepth = 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{.offset = {.x = 0, .y = 0}, .extent = {.width = SHADOW_NEAR_SIZE, .height = SHADOW_NEAR_SIZE}};
    vkCmdSetScissor(cmd, 0, 1, &sc);
    return true;
}

void Renderer::startUnitShadowRecording() {
    unitShadowJobActive_ = false;
    // WOWEE_UNIT_SHADOW_WORKER=0 records the units inline in the shadow pass.
    static const bool onWorker = core::envFlagEnabled("WOWEE_UNIT_SHADOW_WORKER", true);
    if (!onWorker || !parallelRecordingEnabled_ || !unitShadowCmdPool_ || !characterRenderer) return;
    if (!shadowSplit_ || !shadowCasterAllowed("characters") || !unitMapDrawsThisFrame()) return;
    const uint32_t frame = vkCtx->getCurrentFrame();
    VkCommandBuffer cmd = unitShadowCharCmd_[frame];
    if (cmd == VK_NULL_HANDLE) return;

    // What the job reads of the renderer is taken now; nothing on the main
    // thread touches the character renderer again before the shadow pass
    // joins it (the per-frame block, the M2 and grass culls and the
    // composites ahead of the pass do not).
    characterRenderer->beginSunShadowFrame();
    unitShadowUseSeen_ = unitShadowReceiverView(unitShadowSeen_);
    const ShadowProjection np = nearShadowProj_;
    const float nearCull = np.halfExtent * 1.5f;
    unitShadowCharSpheres_.clear();
    unitShadowFuture_ = core::ThreadPool::frameWorkers().submit([this, cmd, frame, np, nearCull]() -> double {
        const auto t0 = std::chrono::steady_clock::now();
        if (!beginUnitShadowSecondary(cmd, frame)) return -1.0;
        characterRenderer->renderShadow(cmd, np.matrix, np.center, nearCull, &unitShadowCharSpheres_,
                                        unitShadowUseSeen_ ? &unitShadowSeen_ : nullptr);
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) return -1.0;
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    });
    unitShadowJobActive_ = true;
}

void Renderer::renderShadowPass() {
    ZoneScopedN("Renderer::renderShadowPass");
    // A new record of the units with a sun shadow; those left out of it take
    // the blob (updateUnitBlobs), so it starts before any early return. A
    // units' job started it already.
    if (characterRenderer && !unitShadowJobActive_) characterRenderer->beginSunShadowFrame();
    if (shadowsSkippedByEnv()) return;
    if (passAblation_ && passAblation_->skip(AblationPass::Shadows)) return;
    if (shadowDepthImage[0] == VK_NULL_HANDLE) return;
    if (currentCmd == VK_NULL_HANDLE) return;
    // Shadows off (the stock default) draws no caster. Nothing samples the
    // maps then - every world shader and the fog's inject test shadowParams.x
    // first - but they are still bound in the per-frame sets, so each is
    // cleared and left in the layout the sets declare once, the first frame
    // it is off, and after that this pass records nothing.
    //
    // Skipping the casters once faulted the GPU within a second, and the
    // casters were drawn with shadows off from then on, at the full cost of
    // shadows on. What that version left out is not on record; what the
    // readers need is what this keeps: each map cleared and moved to
    // SHADER_READ_ONLY before any frame binds it, the pass never disabled
    // whole (see the render graph), and nothing sampling it while
    // shadowParams.x is 0. Checked on an RTX 4060 with the device-fault
    // diagnostics on: shadows flipped every 1.5 s for 90 s in Stormwind, then
    // held off for two minutes there and in Elwynn, no device loss.
    const bool on = shadowsEnabled || shadowCastersWhenOff();
    if (!on) {
        for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
            clearShadowImageOnce(shadowDepthImage[i], shadowDepthView[i], shadowFramebuffer[i],
                                 SHADOW_MAP_SIZE, shadowDepthLayout_[i], shadowClearedWhileOff_[i]);
            clearShadowImageOnce(shadowNearImage[i], shadowNearView[i], shadowNearFramebuffer[i],
                                 SHADOW_NEAR_SIZE, shadowNearLayout_[i], shadowNearClearedWhileOff_[i]);
        }
        return;
    }
    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        shadowClearedWhileOff_[i] = false;
        shadowNearClearedWhileOff_[i] = false;
    }

    // Zero matrix means character position isn't set yet - no casters, and
    // the maps only cleared and left readable.
    const bool drawCasters = lightSpaceMatrix != glm::mat4(0.0f) || shadowPartsThisFrame_ != 0;
    if (!drawCasters) {
        bool cleared = false;
        for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
            if (shadowDepthLayout_[i] != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                clearShadowImageOnce(shadowDepthImage[i], shadowDepthView[i], shadowFramebuffer[i],
                                     SHADOW_MAP_SIZE, shadowDepthLayout_[i], cleared);
            cleared = false;
            if (shadowNearLayout_[i] != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                clearShadowImageOnce(shadowNearImage[i], shadowNearView[i], shadowNearFramebuffer[i],
                                     SHADOW_NEAR_SIZE, shadowNearLayout_[i], cleared);
            cleared = false;
        }
        return;
    }

    const auto casts = [](const char* what) { return shadowCasterAllowed(what); };
    const auto timed = [this](const char* stage, auto&& fn) {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        if (stageSink_) stageSink_(stage, std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count());
    };
    if (m2Renderer) m2Renderer->beginShadowFrame();
    const uint32_t frame = vkCtx->getCurrentFrame();

    // The world map: this frame's part of a redraw, or nothing. With the
    // split off this is every caster, every frame, into the image the frame
    // then reads.
    if (shadowPartsThisFrame_ != 0) {
        const uint32_t target = shadowRedraw_.target;
        const ShadowProjection& proj = shadowRedraw_.proj;
        const bool first = (shadowPartsThisFrame_ & (1u << kWorldShadowTerrain)) != 0;
        const ShadowCasterSet set = shadowSplit_ ? ShadowCasterSet::Static : ShadowCasterSet::All;
        // The light-space box decides who casts; the sphere only narrows the
        // walk (see M2Renderer::renderShadow).
        const float cullRadius = proj.halfExtent * 1.35f;
        beginShadowTarget(shadowDepthImage[target], shadowDepthView[target], shadowFramebuffer[target],
                          SHADOW_MAP_SIZE, shadowDepthLayout_[target], first);
        if ((shadowPartsThisFrame_ & (1u << kWorldShadowTerrain)) && terrainRenderer && casts("terrain")) {
            timed("shadow/terrain", [&] { terrainRenderer->renderShadow(currentCmd, proj.matrix, proj.center, cullRadius); });
        }
        if ((shadowPartsThisFrame_ & (1u << kWorldShadowBuildings)) && wmoRenderer && casts("wmo")) {
            timed("shadow/wmo", [&] { wmoRenderer->renderShadow(currentCmd, proj.matrix, proj.center, cullRadius, set); });
        }
        if ((shadowPartsThisFrame_ & (1u << kWorldShadowDoodads)) && m2Renderer && casts("m2")) {
            timed("shadow/m2", [&] { m2Renderer->renderShadow(currentCmd, proj.matrix, globalTime, proj.center, cullRadius, set); });
        }
        if (!shadowSplit_ && characterRenderer && casts("characters")) {
            timed("shadow/characters", [&] { characterRenderer->renderShadow(currentCmd, proj.matrix, proj.center, cullRadius); });
        }
        endShadowTarget(shadowDepthImage[target], shadowDepthLayout_[target]);
        if (shadowRedrawEndsThisFrame_) shadowRedraw_.active = false;
        else ++shadowRedraw_.nextPart;
        if (shadowSplit_) vkCtx->gpuMark(currentCmd, "shadow world map");
    }
    // The front map has been drawn (planShadowFrame starts a redraw at once
    // when there is none); the other image is cleared once so that neither
    // is ever bound undefined.
    {
        bool cleared = false;
        const uint32_t other = 1u - shadowFront_;
        if (shadowDepthLayout_[other] != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            clearShadowImageOnce(shadowDepthImage[other], shadowDepthView[other], shadowFramebuffer[other],
                                 SHADOW_MAP_SIZE, shadowDepthLayout_[other], cleared);
    }

    // The unit map: this slot's, every frame, cleared and drawn with what
    // moves, and the mask of where a pixel should read it.
    nearShadowMask_.clear();
    if (nearShadowOn_ && shadowNearImage[frame] != VK_NULL_HANDLE) {
        const ShadowProjection& np = nearShadowProj_;
        // A sphere a little wider than the box; the box test in each
        // renderer does the rest. A unit further out whose shadow would
        // reach in under a low sun is left to the world map's absence of it,
        // as the one-map pass left anything past its own radius.
        const float nearCull = np.halfExtent * 1.5f;
        nearCasterSpheres_.clear();
        // The units recorded on a worker (startUnitShadowRecording): the
        // doodads and buildings that moved go into a secondary of their own
        // here, the job is joined, and the pass executes the two.
        if (unitShadowJobActive_) {
            VkCommandBuffer movers = unitShadowMoverCmd_[frame];
            unitShadowMoverSpheres_.clear();
            uint32_t m2Movers = 0;
            const bool moversOk = movers != VK_NULL_HANDLE && beginUnitShadowSecondary(movers, frame);
            if (moversOk) {
                if (m2Renderer && casts("m2")) {
                    timed("shadow/m2 movers", [&] { m2Renderer->renderShadow(movers, np.matrix, globalTime, np.center, nearCull, ShadowCasterSet::Moving, &unitShadowMoverSpheres_); });
                }
                m2Movers = static_cast<uint32_t>(unitShadowMoverSpheres_.size());
                if (wmoRenderer && casts("wmo")) {
                    timed("shadow/wmo movers", [&] { wmoRenderer->renderShadow(movers, np.matrix, np.center, nearCull, ShadowCasterSet::Moving, &unitShadowMoverSpheres_); });
                }
                vkEndCommandBuffer(movers);
            }
            double workerMs = -1.0;
            timed("shadow/characters wait", [&] { workerMs = unitShadowFuture_.get(); });
            unitShadowJobActive_ = false;
            if (stageSink_ && workerMs >= 0.0) stageSink_("shadow/characters (worker)", static_cast<float>(workerMs));

            VkCommandBuffer secondaries[2];
            uint32_t count = 0;
            if (workerMs >= 0.0) secondaries[count++] = unitShadowCharCmd_[frame];
            if (moversOk) secondaries[count++] = movers;
            beginShadowTarget(shadowNearImage[frame], shadowNearView[frame], shadowNearFramebuffer[frame],
                              SHADOW_NEAR_SIZE, shadowNearLayout_[frame], true, true);
            if (count > 0) vkCmdExecuteCommands(currentCmd, count, secondaries);
            endShadowTarget(shadowNearImage[frame], shadowNearLayout_[frame]);

            if (workerMs >= 0.0) nearCasterSpheres_ = unitShadowCharSpheres_;
            nearSphereSplit_[0] = static_cast<uint32_t>(nearCasterSpheres_.size());
            nearSphereSplit_[1] = nearSphereSplit_[0] + m2Movers;
            nearCasterSpheres_.insert(nearCasterSpheres_.end(), unitShadowMoverSpheres_.begin(),
                                      unitShadowMoverSpheres_.end());
            for (const glm::vec4& sphere : nearCasterSpheres_)
                nearShadowMask_.addCaster(np, SHADOW_NEAR_SIZE, glm::vec3(sphere), sphere.w);
        } else {
            beginShadowTarget(shadowNearImage[frame], shadowNearView[frame], shadowNearFramebuffer[frame],
                              SHADOW_NEAR_SIZE, shadowNearLayout_[frame], true);
            ShadowReceiverView seen;
            const bool useSeen = unitShadowReceiverView(seen);
            if (characterRenderer && casts("characters")) {
                timed("shadow/characters", [&] {
                    characterRenderer->renderShadow(currentCmd, np.matrix, np.center, nearCull, &nearCasterSpheres_,
                                                    useSeen ? &seen : nullptr);
                });
            }
            nearSphereSplit_[0] = static_cast<uint32_t>(nearCasterSpheres_.size());
            if (m2Renderer && casts("m2")) {
                timed("shadow/m2 movers", [&] { m2Renderer->renderShadow(currentCmd, np.matrix, globalTime, np.center, nearCull, ShadowCasterSet::Moving, &nearCasterSpheres_); });
            }
            nearSphereSplit_[1] = static_cast<uint32_t>(nearCasterSpheres_.size());
            if (wmoRenderer && casts("wmo")) {
                timed("shadow/wmo movers", [&] { wmoRenderer->renderShadow(currentCmd, np.matrix, np.center, nearCull, ShadowCasterSet::Moving, &nearCasterSpheres_); });
            }
            endShadowTarget(shadowNearImage[frame], shadowNearLayout_[frame]);
            for (const glm::vec4& sphere : nearCasterSpheres_)
                nearShadowMask_.addCaster(np, SHADOW_NEAR_SIZE, glm::vec3(sphere), sphere.w);
        }
    } else {
        bool cleared = false;
        if (shadowNearLayout_[frame] != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            clearShadowImageOnce(shadowNearImage[frame], shadowNearView[frame], shadowNearFramebuffer[frame],
                                 SHADOW_NEAR_SIZE, shadowNearLayout_[frame], cleared);
    }
    // The mask goes into this frame's block, which is mapped and read when
    // the frame executes: written now, after updatePerFrameUBO copied the
    // rest, it is still in time.
    std::memcpy(currentFrameData.nearShadowMask, nearShadowMask_.words, sizeof(nearShadowMask_.words));
    if (perFrameUBOMapped[frame]) {
        std::memcpy(static_cast<char*>(perFrameUBOMapped[frame]) + offsetof(GPUPerFrameData, nearShadowMask),
                    nearShadowMask_.words, sizeof(nearShadowMask_.words));
    }
    vkCtx->gpuMark(currentCmd, "shadows");
}

VkImageView Renderer::getNeutralRtLightingView() const {
    return rtLighting_ ? rtLighting_->neutralView() : VK_NULL_HANDLE;
}

void Renderer::setRtLightingMode(int mode) {
    if (!rtLighting_) return;
    mode = std::clamp(mode, 0, 3);
    // Without ray tracing hardware the rays are walked in a compute shader,
    // and on an integrated GPU that is far over any frame budget (on the
    // Iris Xe at 2560x1080 the pass alone was several times the whole set of
    // enhancements, docs/enhancements-plan.md 3.16) for shadows no better
    // than the shadow map's. The option stays off there unless asked for.
    if (mode != 0 && vkCtx && !vkCtx->hardwareRayQueryEnabled()) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(vkCtx->getPhysicalDevice(), &props);
        const bool integrated = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ||
                                props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        if (integrated && !core::envFlagEnabled("WOWEE_RT_ALLOW_SOFTWARE", false)) {
            static bool told = false;
            if (!told) {
                told = true;
                LOG_WARNING("Ray traced lighting stays off: no ray tracing hardware on an integrated GPU "
                            "(WOWEE_RT_ALLOW_SOFTWARE=1 runs the compute tracer anyway)");
            }
            mode = 0;
        }
    }
    rtLighting_->setMode(static_cast<RtLighting::Mode>(mode));
}

void Renderer::writeRtLightingBindings() {
    if (!rtLighting_ || !vkCtx) return;
    // RtLighting::prepare has waited for the device whenever it reports a
    // change, so neither slot's set is in use.
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        VkDescriptorImageInfo info[2]{};
        info[0].imageView = rtLighting_->lightViewForSlot(i);
        info[1].imageView = rtLighting_->giViewForSlot(i);
        info[0].imageLayout = info[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t b = 0; b < 2; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = perFrameDescSets[i];
            writes[b].dstBinding = 3 + b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[b].pImageInfo = &info[b];
        }
        vkUpdateDescriptorSets(vkCtx->getDevice(), 2, writes, 0, nullptr);
    }
}

VkExtent2D Renderer::sceneRenderExtent() const {
    if (postProcessPipeline_ && postProcessPipeline_->getSceneFramebuffer() != VK_NULL_HANDLE) {
        return postProcessPipeline_->getSceneRenderExtent();
    }
    return vkCtx->getSwapchainExtent();
}

void Renderer::recordRtLighting(VkImage sceneDepth, VkExtent2D sceneExtent, bool depthIsMsaa) {
    if (!rtLighting_ || !rtLighting_->active() || !camera) return;
    rtRecordedThisFrame_ = true;
    RtLighting::FrameInputs in{};
    in.viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();
    in.cameraPos = camera->getPosition();
    in.sunDir = -glm::vec3(currentFrameData.lightDir);
    in.sunColor = glm::vec3(currentFrameData.lightColor);
    in.skyColor = glm::vec3(currentFrameData.ambientColor);
    in.sunUp = in.sunDir.z > -0.05f;
    // Timed apart for the profile: the instance sync walks every WMO and M2
    // placement, the record builds and uploads what changed.
    const auto t0 = std::chrono::steady_clock::now();
    if (wmoRenderer) wmoRenderer->syncRtScene();
    if (m2Renderer) m2Renderer->syncRtScene(in.cameraPos, RtLighting::focusRadius());
    const auto t1 = std::chrono::steady_clock::now();
    rtLighting_->record(currentCmd, sceneDepth, sceneExtent, depthIsMsaa, in);
    vkCtx->gpuMark(currentCmd, "rt_lighting");
    if (stageSink_) {
        const auto t2 = std::chrono::steady_clock::now();
        stageSink_("rt_lighting/sync", std::chrono::duration<float, std::milli>(t1 - t0).count());
        stageSink_("rt_lighting/record", std::chrono::duration<float, std::milli>(t2 - t1).count());
    }
}

VkImageView Renderer::getNeutralFogVolumeView() const {
    return volumetricFog_ ? volumetricFog_->getNeutralView() : VK_NULL_HANDLE;
}

void Renderer::setVolumetricFogQuality(int quality) {
    if (!volumetricFog_) return;
    volumetricFog_->setQuality(static_cast<VolumetricFog::Quality>(std::clamp(quality, 0, 3)));
}

void Renderer::writeFogVolumeBindings() {
    if (!volumetricFog_ || !vkCtx) return;
    // applyPendingQuality has already waited for the device, so neither
    // slot's set is in use by a frame still in flight.
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        VkDescriptorImageInfo fogImgInfo{};
        fogImgInfo.imageView = volumetricFog_->getVolumeView(i);
        fogImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = perFrameDescSets[i];
        write.dstBinding = 2;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &fogImgInfo;
        vkUpdateDescriptorSets(vkCtx->getDevice(), 1, &write, 0, nullptr);
    }
}

float Renderer::volumetricFogExtinction() const {
    // Per yard, at and below the layer, near the camera. At this a sixth of
    // the light is gone over the first hundred yards and two fifths across
    // the whole volume, which thins its air out by 400 - a haze, not a wall,
    // before the slider scales it - while a ten-yard shaft near the sun still
    // lifts what is behind it by a tenth or more.
    constexpr float kBaseExtinction = 0.002f;
    float extinction = kBaseExtinction * volumetricFogDensity_;

    if (lightingManager) {
        const auto& lp = lightingManager->getLightingParams();
        // The zone's own opinion, read off how close its authored fog comes
        // in: Duskwood's ends at 525 yards and gets most of the mist, an
        // open plain's ends far out and gets less. The player's fog slider
        // divides those distances, so it is multiplied back out to reach the
        // zone's number rather than the slider's.
        const float strength = lightingManager->getFogStrength();
        if (strength > 0.001f && lp.fogEnd > 1.0f) {
            const float authoredEnd = lp.fogEnd * strength;
            extinction *= glm::clamp(900.0f / authoredEnd, 0.6f, 2.0f);
        }
        // Morning mist: thickest around half past six, gone by nine.
        const float hours = lightingManager->getVisualTimeOfDayHours();
        const float dawn = 1.0f - glm::smoothstep(0.0f, 2.5f, std::abs(hours - 6.5f));
        extinction *= 1.0f + 0.8f * dawn;
    }

    if (weather) {
        const float w = glm::clamp(weather->getIntensity(), 0.0f, 1.0f);
        switch (weather->getWeatherType()) {
            case Weather::Type::RAIN:  extinction *= 1.0f + 1.2f * w; break;
            case Weather::Type::SNOW:  extinction *= 1.0f + 0.8f * w; break;
            case Weather::Type::STORM: extinction *= 1.0f + 2.0f * w; break;
            case Weather::Type::SAND:  if (enh_.sandstorms) extinction *= 1.0f + 1.5f * w; break;
            default: break;
        }
    }

    // Indoors the outdoor air mostly stays outside. Not all of it: a hall
    // with torches in it should still show their glow.
    if (cameraController && cameraController->isInsideInteriorWMO()) extinction *= 0.35f;
    return extinction;
}

void Renderer::renderVolumetricFog() {
    ZoneScopedN("Renderer::renderVolumetricFog");
    if (!volumetricThisFrame_ || !volumetricFog_ || !camera || currentCmd == VK_NULL_HANDLE) return;
    const uint32_t frame = vkCtx->getCurrentFrame();
    // The inject pass samples this slot's shadow map, so not before the shadow
    // pass has left it readable: at the login screen, before the player has a
    // position, it has never been drawn and is still UNDEFINED, which the
    // validation layer reports for any dispatch that binds it. Skipping leaves
    // this slot's volume as it was - clear air, until the first real frame -
    // and the shaders read that.
    if (shadowDepthLayout_[shadowFront_] != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return;
    const float dt = std::max(lastDeltaTime_, 0.0f);

    // The ground the mist lies on: the terrain under the player, or the
    // player's own feet where those are lower - a cave, a city under a
    // mountain - so the layer is never a ceiling over them. Chased over a
    // second or two so a step off a ledge does not lift the whole bank of
    // mist with it; a teleport snaps it, and drops last frame's air too.
    float ground = characterPosition.z;
    if (terrainManager) {
        if (auto h = terrainManager->getHeightAt(characterPosition.x, characterPosition.y)) {
            ground = std::min(ground, *h);
        }
    }
    if (!fogLayerBaseValid_ || std::abs(ground - fogLayerBase_) > 150.0f) {
        fogLayerBase_ = ground;
        fogLayerBaseValid_ = true;
        volumetricFog_->resetHistory();
    } else {
        fogLayerBase_ += (ground - fogLayerBase_) * (1.0f - std::exp(-dt / 1.5f));
    }

    const float target = volumetricFogExtinction();
    if (fogExtinction_ < 0.0f) fogExtinction_ = target;
    fogExtinction_ += (target - fogExtinction_) * (1.0f - std::exp(-dt / 1.0f));

    VolumetricFog::FrameInputs in;
    in.view = currentFrameData.view;
    in.projection = currentFrameData.projection;
    in.cameraPos = glm::vec3(currentFrameData.viewPos);
    in.time = globalTime;
    in.density = fogExtinction_;
    in.layerBase = fogLayerBase_;
    volumetricFog_->record(currentCmd, frame, perFrameDescSets[frame], in);
    if (vkCtx) vkCtx->gpuMark(currentCmd, "volumetric fog integrate");

    // What it was built from, every few seconds, for the report that says the
    // fog is too thick or missing: INFO, so it costs nothing unless asked for.
    static double lastFogLog = 0.0;
    if (globalTime - lastFogLog > 5.0) {
        lastFogLog = globalTime;
        LOG_INFO("volumetricFog: extinction=", fogExtinction_, "/yd (target ", target,
                 ") layerBase=", fogLayerBase_, " lights=", currentFrameData.localLightMeta.x);
    }
}

namespace {
// The most the sun shafts add per channel under the Enhanced haze
// (SunShafts::FrameInputs::cap).
constexpr float kEnhancedShaftCap = 0.3f;
}  // namespace

void Renderer::recordSunShafts() {
    if (!sunShafts_ || currentCmd == VK_NULL_HANDLE) return;
    SunShafts::FrameInputs in;
    const auto& images = vkCtx->getSwapchainImages();
    if (sunShaftsEnabled_ && worldDrawnThisFrame_ && camera && lightingManager &&
        currentImageIndex < images.size() &&
        !(passAblation_ && passAblation_->skip(AblationPass::SunShafts))) {
        const auto& lp = lightingManager->getLightingParams();
        // The sun Celestial draws and the occlusion test looks for:
        // LightingParams::sunDir, the stock sun table - not -directionalDir,
        // the light the terrain is lit by, which in the stock client never
        // follows the sun. Projected without the upscaler's sub-pixel jitter,
        // because the picture the rays are laid over is the resolved,
        // unjittered one.
        const glm::vec3 sunDir = lp.sunDir;
        const SunOnScreen sun = sunScreenPosition(camera->getViewMatrix(),
                                                  camera->getUnjitteredProjectionMatrix(), sunDir);
        if (sun.inFront) {
            // Screen strength: 1 would be the sky's own colour over anything a
            // fully lit walk crosses. Less, because the sky around the sun is
            // already the brightest thing on screen.
            float strength = 0.8f;
            // Up out of the horizon and gone again as it sets. A sun under the
            // ground lights nothing to stream from.
            strength *= glm::smoothstep(-0.02f, 0.1f, sunDir.z);
            // As visible as Celestial draws the disc: its day intensity, seen
            // through as much cloud as LightFloatBand 2 lets it. Without this
            // the rays streamed from an empty patch of sky in the storm and
            // death sets, which hide the sun.
            const float throughCloud = glm::mix(1.0f, glm::clamp(lp.celestialGlowThrough, 0.0f, 1.0f),
                                                glm::clamp(lp.cloudDensity, 0.0f, 1.0f));
            strength *= glm::clamp(lp.dayIntensity, 0.0f, 1.0f) * throughCloud;
            // In view, or streaming in from just past an edge, fading out as
            // it goes half a screen beyond one.
            const glm::vec2 past = glm::max(glm::abs(sun.uv - 0.5f) - 0.5f, glm::vec2(0.0f));
            strength *= 1.0f - glm::smoothstep(0.0f, 0.5f, std::max(past.x, past.y));
            // Rain and snow put a lid over it.
            if (weather) strength *= 1.0f - 0.8f * glm::clamp(weather->getIntensity(), 0.0f, 1.0f);

            // The sun's own colour, kept in hue and not in brightness, and
            // half white: the rays should warm at dusk, not turn orange.
            const glm::vec3 c = lp.diffuseColor;
            const float peak = std::max({c.r, c.g, c.b, 1e-3f});
            in.tint = glm::mix(glm::vec3(1.0f), c / peak, 0.5f);
            if (enh_.sunHaze) {
                // With the Enhanced haze the rays keep more of the sun's gold, as
                // Enhanced's do through the Goldshire canopy in the afternoon.
                // Not stronger: 1.6x, then 1.25x, blew a sunlit trunk beside
                // an off-screen sun toward white with the haze and the bloom
                // on top of it. The haze's longer shadowed mist (800 yd)
                // carries the extra light instead.
                in.tint = glm::mix(glm::vec3(1.0f), c / peak, 0.75f);
                // And held under a ceiling: by the sun the walk runs through
                // sky the haze has already lit gold, and screened over it at
                // full strength the rays took a quarter of the screen to
                // white (Jered's Goldshire road toward a high sun). Streaks
                // through the canopy are well under it and keep their shape.
                in.cap = kEnhancedShaftCap;
            }
            in.sunUV = sun.uv;
            in.strength = strength;
        }
    }
    // Called every frame, strength zero included, so the composite knows
    // there is nothing of this frame's to add.
    if (sunShafts_->record(currentCmd, vkCtx->getCurrentFrame(),
                           currentImageIndex < images.size() ? images[currentImageIndex] : VK_NULL_HANDLE,
                           vkCtx->getSwapchainExtent(), in)) {
        vkCtx->gpuMark(currentCmd, "sun shafts");
    }
}

void Renderer::markWorldPassClosed() {
    if (!vkCtx || currentCmd == VK_NULL_HANDLE) return;
    // The scene pass holds terrain, sky, buildings, characters and doodads
    // together; WOWEE_PASS_ABLATION splits it. The water pass after the
    // refraction copy holds the water, its spray and the minimap.
    vkCtx->gpuMark(currentCmd, waterPassOpen_ ? "water" : "scene pass");
    waterPassOpen_ = false;
}

// Build the per-frame render graph for off-screen pre-passes.
// Declares passes as graph nodes with input/output dependencies.
// compile() performs topological sort; execute() runs them with auto barriers.
void Renderer::buildFrameGraph(game::GameHandler* gameHandler) {
    (void)gameHandler;
    if (!renderGraph_) return;

    renderGraph_->reset();

    auto shadowDepth = renderGraph_->findResource("shadow_depth");
    auto reflTex = renderGraph_->findResource("reflection_texture");

    // Minimap composites (no dependencies - standalone off-screen render target)
    renderGraph_->addPass("minimap_composite", {}, {},
        [this](VkCommandBuffer cmd) {
            if (minimap && minimap->isEnabled() && camera) {
                glm::vec3 minimapCenter = camera->getPosition();
                if (cameraController && cameraController->isThirdPerson())
                    minimapCenter = characterPosition;
                minimap->compositePass(cmd, minimapCenter);
            }
        });

    // World map composite (standalone)
    renderGraph_->addPass("worldmap_composite", {}, {},
        [this](VkCommandBuffer cmd) {
            if (worldMap) worldMap->compositePass(cmd);
        });

    // Character preview composites (standalone)
    renderGraph_->addPass("preview_composite", {}, {},
        [this](VkCommandBuffer cmd) {
            uint32_t frame = vkCtx->getCurrentFrame();
            for (auto* preview : activePreviews_) {
                // hasContent, not isModelLoaded: the login screen's glue
                // scene has no character, and gating on one meant it was
                // never drawn - the auth screen showed its black fill.
                if (preview && preview->hasContent())
                    preview->compositePass(cmd, frame);
            }
        });

    // Shadow pre-pass → outputs shadow_depth
    renderGraph_->addPass("shadow_pass", {}, {shadowDepth},
        [this](VkCommandBuffer cmd) {
            // Closes the span from the frame's start: the M2 and grass cull
            // dispatches and the composites above, which run first.
            vkCtx->gpuMark(cmd, "culling + composites");
            // Not gated on shadowsEnabled: renderShadowPass is what clears the
            // map and leaves it in the layout its readers expect, and it
            // already skips the casters on its own when shadows are off.
            // Declining to call it here put the transition back where it was
            // before, which is the whole of the fault this was meant to end.
            if (shadowDepthImage[0] != VK_NULL_HANDLE)
                renderShadowPass();
        });
    // Left enabled even with shadows off, as long as the image exists.
    //
    // A disabled pass is skipped whole, and that includes the image barriers
    // declared on it - so turning shadows off stopped the shadow map ever
    // being transitioned, while the passes that read it kept it bound and
    // sampled it in whatever layout it was last left in. The lambda above
    // already declines to draw anything; what has to keep happening is the
    // transition.
    renderGraph_->setPassEnabled("shadow_pass", shadowDepthImage[0] != VK_NULL_HANDLE);

    // Volumetric fog → reads this frame's shadow map, outputs the fog volume
    // every world shader samples.
    auto fogVolume = renderGraph_->findResource("volumetric_fog");
    renderGraph_->addPass("volumetric_fog", {shadowDepth}, {fogVolume},
        [this](VkCommandBuffer) {
            renderVolumetricFog();
        });
    renderGraph_->setPassEnabled("volumetric_fog", volumetricThisFrame_);

    // Reflection pre-pass → outputs reflection_texture (reads scene, so after shadow)
    renderGraph_->addPass("reflection_pass", {shadowDepth}, {reflTex},
        [this](VkCommandBuffer) {
            renderReflectionPass();
        });

    renderGraph_->compile();
}

} // namespace rendering
} // namespace wowee
