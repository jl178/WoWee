#include "core/coordinates.hpp"
#include "rendering/water_surface_grid.hpp"
#include "rendering/water_mask.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/water_caustics.hpp"
#include "pipeline/stock_wmo_liquid.hpp"
#include "rendering/water_mask.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/stock_liquid.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <array>
#include <unordered_map>

namespace wowee {
namespace rendering {

// Matches set 1 binding 0 in water.frag.glsl
// Per surface (set 1), matching WaterMaterial in water.frag.glsl.
struct WaterMaterialUBO {
    glm::vec4 vertexColor;   // attrib4: white, or an interior WMO group's MOMT colour
    float interior;          // 1: the fixed interior light and the WMO ramp's white columns
    float pad0 = 0.0f;
    float pad1 = 0.0f;
    float pad2 = 0.0f;
};

// Push constants matching water.vert.glsl
struct WaterPushConstants {
    glm::mat4 model;
    // The liquid texture's transform (stock texture matrix, 0x8a5590 and
    // 0x8a6090): its scale, and the magma scroll's offset this frame. These
    // were Gerstner wave parameters, always fed an amplitude of 0 - the stock
    // surface is flat.
    float texScale = 1.0f;
    float texOffsetU = 0.0f;
    float texOffsetV = 0.0f;
    float liquidBasicType; // 0=water, 1=ocean, 2=magma, 3=slime
    glm::vec2 screenSize;  // target size, for screen-space UVs
    glm::vec2 depthRange;  // camera near, far - the shader linearises SceneDepth with these
    float sceneValid = 0.0f; // 1 once captureSceneHistory has run; the shader used to guess this per pixel
    float liquidTexValid = 0.0f; // 1 when set 3 holds a frame of this liquid's texture
    float shoreFoam = 0.0f;      // 1 draws the shoreline foam band, 0 the stock water
    float depthRamp = 0.0f;      // LiquidInfo::depthRamp: 0 river, 1 ocean, 2 WMO water
    float texRotCos = 1.0f;      // the stage 1 rotation (LiquidInfo::texRotCos/Sin)
    float texRotSin = 0.0f;
    // Rain ripples (not stock, the "Rain ripples" setting): how hard it rains
    // for the rings, 0..1; 0 with the setting off or in dry weather, and
    // water.frag then skips them. Only water.frag declares it.
    float rainRipple = 0.0f;
    // Sunlight under water (not stock, the "Sunlight under water" setting):
    // its strength by the sun's height; only water_caustics.frag reads it.
    float caustics = 0.0f;
};
static_assert(sizeof(WaterPushConstants) == 128,
              "water.frag and water_caustics.frag lay this range out for 128 bytes, the most every device takes");



WaterRenderer::WaterRenderer() = default;

WaterRenderer::~WaterRenderer() {
    shutdown();
}

bool WaterRenderer::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    vkCtx = ctx;
    if (!vkCtx) return false;

    LOG_INFO("Initializing water renderer (Vulkan)");
    VkDevice device = vkCtx->getDevice();

    // --- Material descriptor set layout (set 1) ---
    // binding 0: WaterMaterial UBO
    VkDescriptorSetLayoutBinding matBinding{};
    matBinding.binding = 0;
    matBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    matBinding.descriptorCount = 1;
    matBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    materialSetLayout = createDescriptorSetLayout(device, { matBinding });
    if (!materialSetLayout) {
        LOG_ERROR("WaterRenderer: failed to create material set layout");
        return false;
    }

    // --- Descriptor pool ---
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = MAX_WATER_SETS;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = MAX_WATER_SETS;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &materialDescPool) != VK_SUCCESS) {
        LOG_ERROR("WaterRenderer: failed to create descriptor pool");
        return false;
    }

    // --- Scene history + reflection descriptor set layout (set 2) ---
    VkDescriptorSetLayoutBinding sceneColorBinding{};
    sceneColorBinding.binding = 0;
    sceneColorBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sceneColorBinding.descriptorCount = 1;
    sceneColorBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding sceneDepthBinding{};
    sceneDepthBinding.binding = 1;
    sceneDepthBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sceneDepthBinding.descriptorCount = 1;
    sceneDepthBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding reflColorBinding{};
    reflColorBinding.binding = 2;
    reflColorBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    reflColorBinding.descriptorCount = 1;
    reflColorBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding reflUBOBinding{};
    reflUBOBinding.binding = 3;
    reflUBOBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    reflUBOBinding.descriptorCount = 1;
    reflUBOBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    sceneSetLayout = createDescriptorSetLayout(device,
        {sceneColorBinding, sceneDepthBinding, reflColorBinding, reflUBOBinding});
    if (!sceneSetLayout) {
        LOG_ERROR("WaterRenderer: failed to create scene set layout");
        return false;
    }

    // Pool needs 3 combined image samplers + 1 uniform buffer per frame
    std::array<VkDescriptorPoolSize, 2> scenePoolSizes{};
    scenePoolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    scenePoolSizes[0].descriptorCount = 3 * SCENE_HISTORY_FRAMES;
    scenePoolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    scenePoolSizes[1].descriptorCount = SCENE_HISTORY_FRAMES;
    VkDescriptorPoolCreateInfo scenePoolInfo{};
    scenePoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    scenePoolInfo.maxSets = SCENE_HISTORY_FRAMES;
    scenePoolInfo.poolSizeCount = static_cast<uint32_t>(scenePoolSizes.size());
    scenePoolInfo.pPoolSizes = scenePoolSizes.data();
    if (vkCreateDescriptorPool(device, &scenePoolInfo, nullptr, &sceneDescPool) != VK_SUCCESS) {
        LOG_ERROR("WaterRenderer: failed to create scene descriptor pool");
        return false;
    }

    // --- Liquid texture set layout (set 3) ---
    if (!createLiquidTextureResources()) {
        LOG_ERROR("WaterRenderer: failed to create liquid texture resources");
        return false;
    }

    // --- Pipeline layout ---
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(WaterPushConstants);

    std::vector<VkDescriptorSetLayout> setLayouts = { perFrameLayout, materialSetLayout, sceneSetLayout,
                                                      liquidTexSetLayout_ };
    pipelineLayout = createPipelineLayout(device, setLayouts, { pushRange });
    if (!pipelineLayout) {
        LOG_ERROR("WaterRenderer: failed to create pipeline layout");
        return false;
    }

    // Create reflection resources FIRST so reflectionUBO exists when
    // createSceneHistoryResources writes descriptor binding 3
    createReflectionResources();

    createSceneHistoryResources(refractionCaptureExtent(),
                                vkCtx->getSceneColorFormat(),
                                vkCtx->getDepthFormat());

    // --- Shaders ---
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/water.vert.spv")) {
        LOG_ERROR("WaterRenderer: failed to load vertex shader");
        return false;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/water.frag.spv")) {
        LOG_ERROR("WaterRenderer: failed to load fragment shader");
        return false;
    }

    // --- Vertex input (interleaved: pos3 + normal3 + uv2 = 8 floats = 32 bytes) ---
    VkVertexInputBindingDescription vertBinding{};
    vertBinding.binding = 0;
    vertBinding.stride = 8 * sizeof(float);
    vertBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // aPos at 0, the liquid uv at 1, and the stock ramp coordinate (carried in
    // the normal slot - the stock normal is always up) at 2.
    std::vector<VkVertexInputAttributeDescription> vertAttribs = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0 },                     // aPos
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 6 * sizeof(float) },        // aTexCoord
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = 3 * sizeof(float) },           // ramp coordinate
    };

    VkRenderPass mainPass = vkCtx->getImGuiRenderPass();

    waterPipeline = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({ vertBinding }, vertAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)  // depth test yes, write no
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx->getMsaaSamples())
        .setLayout(pipelineLayout)
        .setRenderPass(mainPass)
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx->getPipelineCache());

    vertShader.destroy();
    fragShader.destroy();

    if (!waterPipeline) {
        LOG_ERROR("WaterRenderer: failed to create pipeline");
        return false;
    }

    LOG_INFO("Water renderer initialized (Vulkan)");
    return true;
}

void WaterRenderer::recreatePipelines() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();

    destroyReflectionResources();
    createReflectionResources();
    createSceneHistoryResources(refractionCaptureExtent(),
                                vkCtx->getSceneColorFormat(),
                                vkCtx->getDepthFormat());

    // Destroy old pipeline (keep layout)
    destroy(device, waterPipeline);
    // Rebuilt against the new sample count the next time they draw.
    destroyCausticsPipelines(false);
    causticsBuildFailed_ = false;
    destroyHdPipelines(false);
    hdBuildFailed_ = false;

    // Load shaders
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/water.vert.spv")) {
        LOG_ERROR("WaterRenderer::recreatePipelines: failed to load vertex shader");
        return;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/water.frag.spv")) {
        LOG_ERROR("WaterRenderer::recreatePipelines: failed to load fragment shader");
        vertShader.destroy();
        return;
    }

    // Vertex input (same as initialize)
    VkVertexInputBindingDescription vertBinding{};
    vertBinding.binding = 0;
    vertBinding.stride = 8 * sizeof(float);
    vertBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> vertAttribs = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0 },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 6 * sizeof(float) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = 3 * sizeof(float) },        // ramp coordinate
    };

    VkRenderPass mainPass = vkCtx->getImGuiRenderPass();

    waterPipeline = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({ vertBinding }, vertAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx->getMsaaSamples())
        .setLayout(pipelineLayout)
        .setRenderPass(mainPass)
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx->getPipelineCache());

    vertShader.destroy();
    fragShader.destroy();

    if (!waterPipeline) {
        LOG_ERROR("WaterRenderer::recreatePipelines: failed to create pipeline");
    }
}

// Sunlight under water: water.vert with water_caustics.frag, the water's
// layout and vertex format, depth tested and not written, blended
// dst * (1 + src) in colour and the destination's alpha kept.
VkPipeline WaterRenderer::buildCausticsPipeline(VkRenderPass pass, VkSampleCountFlagBits samples) {
    if (!vkCtx || !pipelineLayout || pass == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    VkDevice device = vkCtx->getDevice();
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/water.vert.spv") ||
        !fragShader.loadFromFile(device, "assets/shaders/water_caustics.frag.spv")) {
        LOG_WARNING("WaterRenderer: water_caustics.frag.spv missing - no sunlight under water");
        vertShader.destroy();
        fragShader.destroy();
        return VK_NULL_HANDLE;
    }
    VkVertexInputBindingDescription vertBinding{};
    vertBinding.binding = 0;
    vertBinding.stride = 8 * sizeof(float);
    vertBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::vector<VkVertexInputAttributeDescription> vertAttribs = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0 },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 6 * sizeof(float) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = 3 * sizeof(float) },
    };
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipeline p = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({ vertBinding }, vertAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(blend)
        .setMultisample(samples)
        .setLayout(pipelineLayout)
        .setRenderPass(pass)
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx->getPipelineCache());
    vertShader.destroy();
    fragShader.destroy();
    if (!p) LOG_WARNING("WaterRenderer: failed to create the caustics pipeline");
    return p;
}

void WaterRenderer::destroyCausticsPipelines(bool deferred) {
    destroyPipelinePair(causticsPipeline_, caustics1xPipeline_, deferred);
}

// A pipeline made on first use for the scene pass and its 1x sibling: both
// let go, now or once no frame in flight can still be drawing with them.
void WaterRenderer::destroyPipelinePair(VkPipeline& scenePipeline, VkPipeline& pipeline1x, bool deferred) {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    for (VkPipeline* pp : {&scenePipeline, &pipeline1x}) {
        VkPipeline p = *pp;
        *pp = VK_NULL_HANDLE;
        if (!p) continue;
        if (deferred) {
            vkCtx->deferAfterAllFrameFences([device, p]() { vkDestroyPipeline(device, p, nullptr); });
        } else {
            vkDestroyPipeline(device, p, nullptr);
        }
    }
}

void WaterRenderer::setCausticsEnabled(bool on) {
    if (on == causticsEnabled_) return;
    causticsEnabled_ = on;
    causticsBuildFailed_ = false;
    if (!on) {
        // Off: nothing drawn from the next frame, and the pipelines go once
        // no frame in flight can still be using them.
        causticsStrength_ = 0.0f;
        updateSceneCopy();
        destroyCausticsPipelines(true);
    }
}

// HD water: water.vert with water_hd.frag, everything else the water's own -
// layout, vertex format, depth tested and not written, alpha blended. Where
// the colour copy holds the scene the shader composes the bed itself and
// writes alpha 1; elsewhere it blends as the stock water does. "Water (WoW
// Enhanced)" is the same source built with ENHANCED_WATER (water_fw.frag.spv),
// so each pipeline carries only its own path.
VkPipeline WaterRenderer::buildHdPipeline(VkRenderPass pass, VkSampleCountFlagBits samples) {
    if (!vkCtx || !pipelineLayout || pass == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    VkDevice device = vkCtx->getDevice();
    VkShaderModule vertShader, fragShader;
    const char* fragPath = enhancedWater_ ? "assets/shaders/water_fw.frag.spv" : "assets/shaders/water_hd.frag.spv";
    if (!vertShader.loadFromFile(device, "assets/shaders/water.vert.spv") ||
        !fragShader.loadFromFile(device, fragPath)) {
        LOG_WARNING("WaterRenderer: ", fragPath, " missing - the stock water instead");
        vertShader.destroy();
        fragShader.destroy();
        return VK_NULL_HANDLE;
    }
    VkVertexInputBindingDescription vertBinding{};
    vertBinding.binding = 0;
    vertBinding.stride = 8 * sizeof(float);
    vertBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::vector<VkVertexInputAttributeDescription> vertAttribs = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0 },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 6 * sizeof(float) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = 3 * sizeof(float) },
    };
    VkPipeline p = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({ vertBinding }, vertAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(samples)
        .setLayout(pipelineLayout)
        .setRenderPass(pass)
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx->getPipelineCache());
    vertShader.destroy();
    fragShader.destroy();
    if (!p) LOG_WARNING("WaterRenderer: failed to create the HD water pipeline");
    return p;
}

void WaterRenderer::destroyHdPipelines(bool deferred) {
    destroyPipelinePair(hdPipeline_, hd1xPipeline_, deferred);
}

void WaterRenderer::setEnhancedWater(bool on) {
    if (on == enhancedWater_) return;
    enhancedWater_ = on;
    // The other build of water_hd.frag from the next frame; the old
    // pipelines go once no frame in flight can still be drawing with them.
    hdBuildFailed_ = false;
    destroyHdPipelines(true);
}

void WaterRenderer::setHdWater(bool on) {
    if (on == hdWater_) return;
    hdWater_ = on;
    hdBuildFailed_ = false;
    frameUBO_.hdParams = glm::vec4(0.0f);
    colorCopiedThisFrame_ = false;
    updateSceneCopy();
    // Off: water.frag from the next frame, and the pipelines go once no
    // frame in flight can still be drawing with them.
    if (!on) destroyHdPipelines(true);
}

void WaterRenderer::setRefractionEnabled(bool enabled) {
    if (refractionEnabled == enabled) return;
    refractionEnabled = enabled;

    // When turning off, clear scene history images to black so the shader
    // detects "no data" and uses the non-refraction path.
    if (!enabled && vkCtx) {
        vkCtx->immediateSubmit([&](VkCommandBuffer cmd) {
            for (auto& sh : sceneHistory) {
                if (!sh.colorImage) continue;

                VkImageMemoryBarrier2 toTransfer{};
                toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                toTransfer.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                toTransfer.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
                toTransfer.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toTransfer.image = sh.colorImage;
                toTransfer.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
                toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                VkDependencyInfo toTransferDep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                toTransferDep.imageMemoryBarrierCount = 1;
                toTransferDep.pImageMemoryBarriers = &toTransfer;
                cmdPipelineBarrier2(cmd, toTransferDep);

                VkClearColorValue clearColor = {{0.0f, 0.0f, 0.0f, 0.0f}};
                VkImageSubresourceRange range = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
                vkCmdClearColorImage(cmd, sh.colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);

                VkImageMemoryBarrier2 toRead = toTransfer;
                toRead.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
                toRead.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                VkDependencyInfo toReadDep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                toReadDep.imageMemoryBarrierCount = 1;
                toReadDep.pImageMemoryBarriers = &toRead;
                cmdPipelineBarrier2(cmd, toReadDep);
            }
        });
    }
}

void WaterRenderer::shutdown() {
    clear();

    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    vkDeviceWaitIdle(device);

    // clear() defers surface destruction, and those lambdas free descriptor sets
    // from the pools destroyed just below. Drain them here, while the pools are
    // still valid - otherwise they linger until some later subsystem's flush
    // runs them against a dead pool.
    vkCtx->flushDeferredCleanup();

    destroyWater1xResources();
    destroyReflectionResources();
    destroySceneHistoryResources();
    destroyCausticsPipelines(false);
    destroyHdPipelines(false);
    destroy(device, waterPipeline);
    destroy(device, pipelineLayout);
    destroyLiquidTextureResources();
    destroy(device, sceneDescPool);
    destroy(device, sceneSetLayout);
    destroy(device, materialDescPool);
    destroy(device, materialSetLayout);

    vkCtx = nullptr;
}

// ==============================================================
// Liquid textures (LiquidType.dbc animated sequences)
// ==============================================================

bool WaterRenderer::createLiquidTextureResources() {
    VkDevice device = vkCtx->getDevice();

    VkDescriptorSetLayoutBinding texBinding{};
    texBinding.binding = 0;
    texBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    texBinding.descriptorCount = 1;
    texBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    liquidTexSetLayout_ = createDescriptorSetLayout(device, { texBinding });
    if (!liquidTexSetLayout_) return false;

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = MAX_LIQUID_TEXTURE_SETS;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = MAX_LIQUID_TEXTURE_SETS;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &liquidTexPool_) != VK_SUCCESS) return false;

    // A transparent black texel: added over the water colour it changes
    // nothing, so a liquid whose texture failed to load still draws its colour.
    const uint8_t blank[4] = {0, 0, 0, 0};
    blankLiquidTexture_ = std::make_unique<VkTexture>();
    if (!blankLiquidTexture_->upload(*vkCtx, blank, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false)) return false;
    blankLiquidTexture_->createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                       VK_SAMPLER_ADDRESS_MODE_REPEAT);
    if (!blankLiquidTexture_->isValid()) return false;

    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = liquidTexPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &liquidTexSetLayout_;
    if (vkAllocateDescriptorSets(device, &ai, &blankLiquidSet_) != VK_SUCCESS) return false;

    VkDescriptorImageInfo imageInfo = blankLiquidTexture_->descriptorInfo();
    VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = blankLiquidSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    return true;
}

void WaterRenderer::destroyLiquidTextureResources() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    VmaAllocator allocator = vkCtx->getAllocator();
    for (auto& set : liquidTextureSets_) {
        for (auto& tex : set.frames) {
            if (tex) tex->destroy(device, allocator);
        }
    }
    liquidTextureSets_.clear();
    liquidTextureSetByPattern_.clear();
    liquidInfoById_.clear();
    for (int& d : defaultTextureSetByBasic_) d = -1;
    liquidTexturesLoaded_ = false;
    if (blankLiquidTexture_) {
        blankLiquidTexture_->destroy(device, allocator);
        blankLiquidTexture_.reset();
    }
    blankLiquidSet_ = VK_NULL_HANDLE;  // freed with the pool
    destroy(device, liquidTexPool_);
    destroy(device, liquidTexSetLayout_);
}

int WaterRenderer::loadLiquidTextureSequence(pipeline::AssetManager* assetManager,
                                             const std::string& pattern) {
    std::string key = pattern;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (auto it = liquidTextureSetByPattern_.find(key); it != liquidTextureSetByPattern_.end()) {
        return it->second;
    }
    liquidTextureSetByPattern_[key] = -1;  // a failed sequence is not retried

    const size_t slot = pattern.find("%d");
    if (slot == std::string::npos) return -1;

    VkDevice device = vkCtx->getDevice();
    LiquidTextureSet set;
    // Frames are numbered from 1. Most sequences have 30; fast_a has 16, so
    // the count is whatever is there.
    constexpr int kMaxFrames = 30;
    for (int frame = 1; frame <= kMaxFrames; ++frame) {
        std::string path = pattern;
        path.replace(slot, 2, std::to_string(frame));
        if (!assetManager->fileExists(path)) break;

        auto tex = std::make_unique<VkTexture>();
        bool ok = false;
        pipeline::BLPImage compressed = assetManager->loadTexture(path, true);
        if (compressed.isValid()) ok = tex->uploadBLP(*vkCtx, compressed);
        if (!ok) {
            pipeline::BLPImage image = assetManager->loadTexture(path);
            ok = image.isValid() &&
                 tex->upload(*vkCtx, image.data.data(), image.width, image.height,
                             VK_FORMAT_R8G8B8A8_UNORM, true);
        }
        if (!ok) break;
        tex->createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                           VK_SAMPLER_ADDRESS_MODE_REPEAT);
        if (!tex->isValid()) break;
        set.frames.push_back(std::move(tex));
    }
    if (set.frames.empty()) {
        LOG_WARNING("Water: no frames found for liquid texture ", pattern);
        return -1;
    }

    for (const auto& tex : set.frames) {
        VkDescriptorSet ds = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = liquidTexPool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &liquidTexSetLayout_;
        if (vkAllocateDescriptorSets(device, &ai, &ds) != VK_SUCCESS) {
            LOG_WARNING("Water: liquid texture descriptor pool exhausted at ", pattern);
            break;
        }
        VkDescriptorImageInfo imageInfo = tex->descriptorInfo();
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = ds;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        set.sets.push_back(ds);
    }
    if (set.sets.empty()) return -1;
    set.frames.resize(set.sets.size());

    LOG_INFO("Water: loaded ", set.sets.size(), " frames of ", pattern);
    const int index = static_cast<int>(liquidTextureSets_.size());
    liquidTextureSets_.push_back(std::move(set));
    liquidTextureSetByPattern_[key] = index;
    return index;
}

void WaterRenderer::loadLiquidTextures(pipeline::AssetManager* assetManager) {
    if (liquidTexturesLoaded_ || !assetManager || !vkCtx || !liquidTexPool_) return;
    liquidTexturesLoaded_ = true;

    // Each basic type's stock sequence, for a liquid the DBC does not list.
    static const char* const kDefaultPatterns[4] = {
        "XTextures\\river\\lake_a.%d.blp",
        "XTextures\\ocean\\ocean_h.%d.blp",
        "XTextures\\lava\\lava.%d.blp",
        "XTextures\\slime\\slime.%d.blp",
    };
    for (int b = 0; b < 4; ++b) {
        defaultTextureSetByBasic_[b] = loadLiquidTextureSequence(assetManager, kDefaultPatterns[b]);
    }

    // LiquidType.dbc (3.3.5, 45 fields): 0 ID, 1 Name, 3 Type (0 water,
    // 1 ocean, 2 magma, 3 slime), 15-20 Texture[6]. The first texture slot
    // holding a frame sequence is the surface; the procedural types list
    // their height maps there too, which are not the surface.
    auto dbc = assetManager->loadDBCOptional("LiquidType.dbc");
    if (!dbc || !dbc->isLoaded() || dbc->getFieldCount() < 21) {
        LOG_WARNING("Water: LiquidType.dbc unavailable, using the stock texture per basic type");
        return;
    }
    for (uint32_t row = 0; row < dbc->getRecordCount(); ++row) {
        const uint32_t id = dbc->getUInt32(row, 0);
        if (id > 0xFFFF) continue;
        LiquidInfo info;
        const uint32_t type = dbc->getUInt32(row, 3);
        info.basicType = static_cast<uint8_t>(type < 4 ? type : (id == 0 ? 0 : (id - 1) % 4));
        for (uint32_t f = 15; f <= 20; ++f) {
            std::string path = dbc->getString(row, f);
            if (path.find("%d") == std::string::npos) continue;
            std::string lower = path;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.find("height") != std::string::npos) continue;
            info.textureSet = loadLiquidTextureSequence(assetManager, path);
            break;
        }
        // How the sequence moves (see LiquidInfo): 14 MaterialID (2 is the
        // magma material, which magma and slime use), 23-24 Float[0..1],
        // 42 Int[1].
        {
            std::string ramp = dbc->getString(row, 16);
            std::transform(ramp.begin(), ramp.end(), ramp.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ramp.find("wmowater") != std::string::npos) info.depthRamp = 2;
            else if (ramp.find("ocean") != std::string::npos) info.depthRamp = 1;
            else if (ramp.empty() && info.basicType == 1) info.depthRamp = 1;
        }
        info.flags = dbc->getUInt32(row, 2);
        info.particleScale = dbc->getFloat(row, 11);
        info.particleMovement = static_cast<int>(dbc->getUInt32(row, 12));
        info.particleTexSlots = static_cast<int>(dbc->getUInt32(row, 13));
        if (dbc->getFieldCount() >= 43) {
            const bool magmaMaterial = dbc->getUInt32(row, 14) == 2 || info.basicType >= 2;
            info.framePeriodMs = stock_liquid::framePeriodMs(dbc->getUInt32(row, 42), magmaMaterial);
            const float f0 = dbc->getFloat(row, 23);
            const float f1 = dbc->getFloat(row, 24);
            if (magmaMaterial) {
                info.scrollU = f0;
                info.scrollV = f1;
            } else {
                if (f0 > 0.0f) info.texScale = f0;
                const float angle = stock_liquid::textureRotationRadians(f1);
                info.texRotCos = std::cos(angle);
                info.texRotSin = std::sin(angle);
                const float f2 = dbc->getFloat(row, 25);
                if (f2 > 0.0f) info.rampScale = f2;
            }
            info.depthDivisor = stock_liquid::depthDivisor(dbc->getUInt32(row, 41));
        }
        liquidInfoById_[static_cast<uint16_t>(id)] = info;
    }
    LOG_INFO("Water: ", liquidInfoById_.size(), " liquid types, ",
             liquidTextureSets_.size(), " texture sequences");
}

WaterRenderer::LiquidInfo WaterRenderer::resolveLiquid(uint16_t liquidType) const {
    LiquidInfo info;
    if (auto it = liquidInfoById_.find(liquidType); it != liquidInfoById_.end()) {
        info = it->second;
    } else {
        info.basicType = static_cast<uint8_t>((liquidType == 0) ? 0 : ((liquidType - 1) % 4));
        info.depthRamp = info.basicType == 1 ? 1 : 0;
    }
    if (info.textureSet < 0) info.textureSet = defaultTextureSetByBasic_[info.basicType & 3];
    return info;
}

void WaterRenderer::setLiquidLighting(const LightingParams& lighting) {
    frameUBO_.riverClose = glm::vec4(lighting.riverCloseColor, lighting.waterShallowAlpha);
    frameUBO_.riverFar = glm::vec4(lighting.riverFarColor, lighting.waterDeepAlpha);
    frameUBO_.oceanClose = glm::vec4(lighting.oceanCloseColor, lighting.oceanShallowAlpha);
    frameUBO_.oceanFar = glm::vec4(lighting.oceanFarColor, lighting.oceanDeepAlpha);
    // HD water's reflected sky; water.frag never reads these.
    // The .w's carry the sun for the Enhanced water's highlight and sun gate:
    // dnc.db's day and night intensity and the direction to the sun sprite
    // (the stock light stays up all night; LightingParams::sunDir).
    frameUBO_.skyTop = glm::vec4(lighting.skyTopColor, lighting.dayIntensity);
    frameUBO_.skyMiddle = glm::vec4(lighting.skyMiddleColor, lighting.nightIntensity);
    frameUBO_.skyBand1 = glm::vec4(lighting.skyBand1Color, lighting.sunDir.x);
    frameUBO_.skyBand2 = glm::vec4(lighting.skyBand2Color, lighting.sunDir.y);
    frameUBO_.skySmog = glm::vec4(lighting.skySmogColor, lighting.sunDir.z);
    frameUBO_.skyFog = glm::vec4(lighting.skyFogColor, 0.0f);
}

VkDescriptorSet WaterRenderer::allocateMaterialSet() {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = materialDescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &materialSetLayout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(vkCtx->getDevice(), &allocInfo, &set) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return set;
}

void WaterRenderer::destroySceneHistoryResources() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    for (auto& sh : sceneHistory) {
        if (sh.colorView) { vkDestroyImageView(device, sh.colorView, nullptr); sh.colorView = VK_NULL_HANDLE; }
        if (sh.depthView) { vkDestroyImageView(device, sh.depthView, nullptr); sh.depthView = VK_NULL_HANDLE; }
        if (sh.colorImage) { vmaDestroyImage(vkCtx->getAllocator(), sh.colorImage, sh.colorAlloc); sh.colorImage = VK_NULL_HANDLE; sh.colorAlloc = VK_NULL_HANDLE; }
        if (sh.depthImage) { vmaDestroyImage(vkCtx->getAllocator(), sh.depthImage, sh.depthAlloc); sh.depthImage = VK_NULL_HANDLE; sh.depthAlloc = VK_NULL_HANDLE; }
        sh.sceneSet = VK_NULL_HANDLE;
    }
    sceneColorSampler = VK_NULL_HANDLE; // Owned by VkContext sampler cache
    sceneDepthSampler = VK_NULL_HANDLE; // Owned by VkContext sampler cache
    sceneHistoryExtent = {.width = 0, .height = 0};
    sceneHistoryReady = false;
}

// Full resolution, deliberately.
//
// This was half, on the grounds that refraction is low-frequency - sampled
// through a rippling normal that displaces it by a few pixels anyway - so the
// difference would not show through moving water. That holds for a lake bed.
// It does not hold for the player's own character, which is what is under the
// surface most of the time this is being looked at, and which arrived through
// the refraction visibly aliased however much anti-aliasing was asked for: the
// copy it was sampled from had already thrown half the resolution away, and
// nothing downstream can put that back. It is the thing in the middle of the
// screen, so it gets the pixels.
VkExtent2D WaterRenderer::refractionCaptureExtent() const {
    VkExtent2D full = vkCtx ? vkCtx->getSwapchainExtent() : VkExtent2D{.width = 0, .height = 0};
    return { .width = std::max(1u, full.width), .height = std::max(1u, full.height) };
}

void WaterRenderer::createSceneHistoryResources(VkExtent2D extent, VkFormat colorFormat, VkFormat depthFormat) {
    if (!vkCtx || extent.width == 0 || extent.height == 0 || !sceneSetLayout || !sceneDescPool) return;
    VkDevice device = vkCtx->getDevice();

    destroySceneHistoryResources();
    vkResetDescriptorPool(device, sceneDescPool, 0);
    sceneHistoryExtent = extent;

    // Create shared samplers
    VkSamplerCreateInfo sampCI{};
    sampCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampCI.magFilter = VK_FILTER_LINEAR;
    sampCI.minFilter = VK_FILTER_LINEAR;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sceneColorSampler = vkCtx->getOrCreateSampler(sampCI);
    if (sceneColorSampler == VK_NULL_HANDLE) {
        LOG_ERROR("WaterRenderer: failed to create scene color sampler");
        return;
    }
    sampCI.magFilter = VK_FILTER_NEAREST;
    sampCI.minFilter = VK_FILTER_NEAREST;
    sceneDepthSampler = vkCtx->getOrCreateSampler(sampCI);
    if (sceneDepthSampler == VK_NULL_HANDLE) {
        LOG_ERROR("WaterRenderer: failed to create scene depth sampler");
        return;
    }

    VkImageCreateInfo colorImgInfo{};
    colorImgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    colorImgInfo.imageType = VK_IMAGE_TYPE_2D;
    colorImgInfo.format = colorFormat;
    colorImgInfo.extent = {.width = extent.width, .height = extent.height, .depth = 1};
    colorImgInfo.mipLevels = 1;
    colorImgInfo.arrayLayers = 1;
    colorImgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    colorImgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    colorImgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    colorImgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageCreateInfo depthImgInfo = colorImgInfo;
    depthImgInfo.format = depthFormat;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    // Create per-frame images, views, and descriptor sets
    for (uint32_t f = 0; f < SCENE_HISTORY_FRAMES; f++) {
        auto& sh = sceneHistory[f];

        if (vmaCreateImage(vkCtx->getAllocator(), &colorImgInfo, &allocCI, &sh.colorImage, &sh.colorAlloc, nullptr) != VK_SUCCESS) {
            LOG_ERROR("WaterRenderer: failed to create scene color history image [", f, "]");
            return;
        }
        if (vmaCreateImage(vkCtx->getAllocator(), &depthImgInfo, &allocCI, &sh.depthImage, &sh.depthAlloc, nullptr) != VK_SUCCESS) {
            LOG_ERROR("WaterRenderer: failed to create scene depth history image [", f, "]");
            return;
        }

        VkImageViewCreateInfo colorViewInfo{};
        colorViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        colorViewInfo.image = sh.colorImage;
        colorViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        colorViewInfo.format = colorFormat;
        colorViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        colorViewInfo.subresourceRange.levelCount = 1;
        colorViewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &colorViewInfo, nullptr, &sh.colorView) != VK_SUCCESS) {
            LOG_ERROR("WaterRenderer: failed to create scene color history view [", f, "]");
            return;
        }

        VkImageViewCreateInfo depthViewInfo = colorViewInfo;
        depthViewInfo.image = sh.depthImage;
        depthViewInfo.format = depthFormat;
        depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (vkCreateImageView(device, &depthViewInfo, nullptr, &sh.depthView) != VK_SUCCESS) {
            LOG_ERROR("WaterRenderer: failed to create scene depth history view [", f, "]");
            return;
        }

        // Allocate descriptor set for this frame
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = sceneDescPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &sceneSetLayout;
        if (vkAllocateDescriptorSets(device, &ai, &sh.sceneSet) != VK_SUCCESS) {
            LOG_ERROR("WaterRenderer: failed to allocate scene descriptor set [", f, "]");
            sh.sceneSet = VK_NULL_HANDLE;
            return;
        }

        VkDescriptorImageInfo colorInfo{};
        colorInfo.sampler = sceneColorSampler;
        colorInfo.imageView = sh.colorView;
        colorInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo depthInfo{};
        depthInfo.sampler = sceneDepthSampler;
        depthInfo.imageView = sh.depthView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo reflColorInfo{};
        reflColorInfo.sampler = sceneColorSampler;
        reflColorInfo.imageView = reflectionColorView ? reflectionColorView : sh.colorView;
        reflColorInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo reflUBOInfo{};
        reflUBOInfo.buffer = reflectionUBO;
        // Each frame in flight reads its own slot; see uploadFrameUBO.
        reflUBOInfo.offset = static_cast<VkDeviceSize>(f) * kFrameUBOStride;
        reflUBOInfo.range = sizeof(WaterFrameUBOData);

        std::vector<VkWriteDescriptorSet> writes;

        VkWriteDescriptorSet w0{};
        w0.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w0.dstSet = sh.sceneSet;
        w0.dstBinding = 0;
        w0.descriptorCount = 1;
        w0.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w0.pImageInfo = &colorInfo;
        writes.push_back(w0);

        VkWriteDescriptorSet w1{};
        w1.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w1.dstSet = sh.sceneSet;
        w1.dstBinding = 1;
        w1.descriptorCount = 1;
        w1.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w1.pImageInfo = &depthInfo;
        writes.push_back(w1);

        VkWriteDescriptorSet w2{};
        w2.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w2.dstSet = sh.sceneSet;
        w2.dstBinding = 2;
        w2.descriptorCount = 1;
        w2.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w2.pImageInfo = &reflColorInfo;
        writes.push_back(w2);

        if (reflectionUBO) {
            VkWriteDescriptorSet w3{};
            w3.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w3.dstSet = sh.sceneSet;
            w3.dstBinding = 3;
            w3.descriptorCount = 1;
            w3.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w3.pBufferInfo = &reflUBOInfo;
            writes.push_back(w3);
        }

        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    // Initialize all per-frame history images to shader-read layout
    vkCtx->immediateSubmit([&](VkCommandBuffer cmd) {
        std::vector<VkImageMemoryBarrier2> barriers;
        for (auto& sh : sceneHistory) {
            VkImageMemoryBarrier2 b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = sh.colorImage;
            b.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barriers.push_back(b);

            b.image = sh.depthImage;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            barriers.push_back(b);
        }
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dep.pImageMemoryBarriers = barriers.data();
        cmdPipelineBarrier2(cmd, dep);
    });
}

void WaterRenderer::updateMaterialUBO(WaterSurface& surface) {
    WaterMaterialUBO mat{};
    mat.vertexColor = surface.vertexColor;
    mat.interior = surface.interior ? 1.0f : 0.0f;

    // Create UBO
    VkBufferCreateInfo bufCI{};
    bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCI.size = sizeof(WaterMaterialUBO);
    bufCI.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    allocCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo mapInfo{};
    vmaCreateBuffer(vkCtx->getAllocator(), &bufCI, &allocCI,
                    &surface.materialUBO, &surface.materialAlloc, &mapInfo);
    if (mapInfo.pMappedData) {
        std::memcpy(mapInfo.pMappedData, &mat, sizeof(mat));
    }

    // Allocate and write descriptor set
    surface.materialSet = allocateMaterialSet();
    if (surface.materialSet) {
        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = surface.materialUBO;
        bufInfo.offset = 0;
        bufInfo.range = sizeof(WaterMaterialUBO);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = surface.materialSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufInfo;

        vkUpdateDescriptorSets(vkCtx->getDevice(), 1, &write, 0, nullptr);
    } else {
        LOG_WARNING("Water: failed to allocate material descriptor set (pool exhaustion?)");
    }
}

// ==============================================================
// Data loading (preserved from GL version - no GL calls)
// ==============================================================

void WaterRenderer::loadFromTerrain(const pipeline::ADTTerrain& terrain, bool append,
                                     int tileX, int tileY) {
    constexpr float TILE_SIZE = 33.33333f / 8.0f;

    if (!append) {
        clear();
    }

    // ── Pass 1: collect layers into merge groups keyed by {liquidType, roundedHeight} ──
    struct ChunkLayerInfo {
        int chunkX, chunkY;
        const pipeline::ADTTerrain::WaterLayer* layer;
    };

    struct MergeKey {
        uint16_t liquidType;
        int32_t roundedHeight;  // minHeight * 2, rounded to int
        bool operator==(const MergeKey& o) const {
            return liquidType == o.liquidType && roundedHeight == o.roundedHeight;
        }
    };

    struct MergeKeyHash {
        size_t operator()(const MergeKey& k) const {
            return std::hash<uint64_t>()((uint64_t(k.liquidType) << 32) | uint32_t(k.roundedHeight));
        }
    };

    std::unordered_map<MergeKey, std::vector<ChunkLayerInfo>, MergeKeyHash> mergeGroups;

    for (int chunkIdx = 0; chunkIdx < 256; chunkIdx++) {
        const auto& chunkWater = terrain.waterData[chunkIdx];
        if (!chunkWater.hasWater()) continue;

        int chunkX = chunkIdx % 16;
        int chunkY = chunkIdx / 16;

        for (const auto& layer : chunkWater.layers) {
            MergeKey key;
            key.liquidType = layer.liquidType;
            key.roundedHeight = static_cast<int32_t>(std::round(layer.minHeight * 2.0f));
            mergeGroups[key].push_back({.chunkX = chunkX, .chunkY = chunkY, .layer = &layer});
        }
    }

    // Tile origin = NW corner = chunk(0,0) position
    const auto& chunk00 = terrain.getChunk(0, 0);

    // Stormwind water lowering check
    bool isStormwindArea = (tileX >= 28 && tileX <= 50 && tileY >= 28 && tileY <= 52);
    float tileWorldX = 0, tileWorldY = 0;
    glm::vec2 moonwellPos2D(0.0f);
    if (isStormwindArea) {
        tileWorldX = (32.0f - tileX) * core::coords::TILE_SIZE;
        tileWorldY = (32.0f - tileY) * core::coords::TILE_SIZE;
        moonwellPos2D = glm::vec2(-8755.9f, 1108.9f);
    }

    int totalSurfaces = 0;

    // Merge threshold: groups with more than this many chunks get merged into
    // one tile-wide surface.  Small groups (shore, lakes) stay per-chunk so
    // their original mask / height data is preserved exactly.
    constexpr size_t MERGE_THRESHOLD = 4;

    // ── Pass 2: create surfaces ──
    for (auto& [key, chunkLayers] : mergeGroups) {

        // ── Small group → per-chunk surfaces (original code path) ──
        if (chunkLayers.size() <= MERGE_THRESHOLD) {
            for (const auto& info : chunkLayers) {
                const auto& layer = *info.layer;
                const auto& terrainChunk = terrain.getChunk(info.chunkX, info.chunkY);

                WaterSurface surface;
                surface.position = glm::vec3(
                    terrainChunk.position[0],
                    terrainChunk.position[1],
                    layer.minHeight
                );
                surface.origin = glm::vec3(
                    // Same axis convention as terrain_mesh.cpp vertices:
                    // MH2O rows (y) advance along render X, columns (x)
                    // along render Y.
                    surface.position.x - (static_cast<float>(layer.y) * TILE_SIZE),
                    surface.position.y - (static_cast<float>(layer.x) * TILE_SIZE),
                    layer.minHeight
                );
                // stepX walks the mesh grid's x (MH2O column → render Y),
                // stepY walks the grid's y (MH2O row → render X).
                surface.stepX = glm::vec3(0.0f, -TILE_SIZE, 0.0f);
                surface.stepY = glm::vec3(-TILE_SIZE, 0.0f, 0.0f);

                surface.minHeight = layer.minHeight;
                surface.maxHeight = layer.maxHeight;
                surface.liquidType = layer.liquidType;
                // Keep mask coordinates in the MH2O chunk's native x/y order;
                // only the world-space origin needs the terrain axis conversion.
                surface.xOffset = layer.x;
                surface.yOffset = layer.y;
                surface.width = layer.width;
                surface.height = layer.height;

                size_t numVertices = (layer.width + 1) * (layer.height + 1);
                bool useFlat = true;
                if (layer.heights.size() == numVertices) {
                    bool sane = true;
                    for (float h : layer.heights) {
                        if (!std::isfinite(h) || std::abs(h) > 50000.0f) { sane = false; break; }
                        if (h < layer.minHeight - 8.0f || h > layer.maxHeight + 8.0f) { sane = false; break; }
                    }
                    if (sane) { useFlat = false; surface.heights = layer.heights; }
                }
                if (useFlat) surface.heights.resize(numVertices, layer.minHeight);

                if (isStormwindArea && layer.minHeight > 94.0f) {
                    float distToMoonwell = glm::distance(glm::vec2(tileWorldX, tileWorldY), moonwellPos2D);
                    if (distToMoonwell > 300.0f) {
                        for (float& h : surface.heights) h -= 1.0f;
                        surface.minHeight -= 1.0f;
                        surface.maxHeight -= 1.0f;
                    }
                }

                surface.mask = layer.mask;
                surface.tileX = tileX;
                surface.tileY = tileY;
                if (layer.depths.size() == numVertices) surface.depths = layer.depths;
                if (layer.uvs.size() == numVertices * 2) surface.uvs = layer.uvs;

                createWaterMesh(surface);
                if (surface.indexCount > 0 && vkCtx) {
                    updateMaterialUBO(surface);
                }
                surfaces.push_back(std::move(surface));
                totalSurfaces++;
            }
            continue;
        }

        // ── Large group → merged tile-wide surface ──
        WaterSurface surface;

        float groupHeight = key.roundedHeight / 2.0f;

        surface.width = 128;
        surface.height = 128;
        surface.xOffset = 0;
        surface.yOffset = 0;
        surface.liquidType = key.liquidType;
        surface.tileX = tileX;
        surface.tileY = tileY;

        // Origin = chunk(0,0) position (NW corner of tile)
        surface.origin = glm::vec3(chunk00.position[0], chunk00.position[1], groupHeight);
        surface.position = surface.origin;
        surface.stepX = glm::vec3(-TILE_SIZE, 0.0f, 0.0f);
        surface.stepY = glm::vec3(0.0f, -TILE_SIZE, 0.0f);

        surface.minHeight = groupHeight;
        surface.maxHeight = groupHeight;

        // Initialize height grid (129×129) with group height
        constexpr int MERGED_W = 128;
        const int gridW = MERGED_W + 1;  // 129
        const int gridH = MERGED_W + 1;
        surface.heights.resize(gridW * gridH, groupHeight);
        // Depth bytes and uvs on the same grid, filled where a layer has
        // them; a vertex no layer covers is the deep end.
        bool groupHasDepths = false;
        bool groupHasUVs = false;
        for (const auto& info : chunkLayers) {
            groupHasDepths = groupHasDepths || !info.layer->depths.empty();
            groupHasUVs = groupHasUVs || !info.layer->uvs.empty();
        }
        if (groupHasDepths) surface.depths.assign(static_cast<size_t>(gridW) * gridH, 0xFF);
        if (groupHasUVs) surface.uvs.assign(static_cast<size_t>(gridW) * gridH * 2, 0.0f);

        // Initialize mask (128×128 sub-tiles)
        // Mask uses LSB bit order: tileIndex = row * 128 + col
        const int maskBytes = (MERGED_W * MERGED_W + 7) / 8;
        // Ocean water (basicType 1) at sea level fills each chunk that declares
        // it, whatever its own bitmap says - an ocean layer's sub-rect is often
        // sparse and the gaps read as holes in open sea.
        //
        // Each chunk that declares it, not the whole tile. This used to seed
        // the entire 128×128 mask, on the reading that depth testing against
        // the terrain would hide the water wherever there was land. That holds
        // only for land *above* sea level. Somewhere the ground drops below it
        // the water is drawn over the top with nothing to occlude it, and a
        // chunk that declares no liquid at all got a surface anyway: the
        // Caverns of Time, a hole in a coastal tile in Tanaris, was roofed with
        // ocean. Any cave below sea level on an ocean tile had the same.
        uint8_t basicType = (key.liquidType == 0) ? 0 : ((key.liquidType - 1) % 4);
        bool isOcean = (basicType == 1) && (std::abs(groupHeight) < 1.0f);
        surface.mask.resize(maskBytes, 0x00);

        // ── Fill from each contributing chunk ──
        for (const auto& info : chunkLayers) {
            const auto& layer = *info.layer;

            // Merged grid offset for this chunk.
            // Grid x is the row axis (render X): chunk row (idx/16) and MH2O y.
            // Grid y is the column axis (render Y): chunk col (idx%16) and MH2O x.
            int baseGx = info.chunkY * 8;
            int baseGy = info.chunkX * 8;

            // Copy heights
            int layerGridW = layer.width + 1;
            size_t numVertices = static_cast<size_t>(layerGridW) * (layer.height + 1);
            bool useFlat = true;
            if (layer.heights.size() == numVertices) {
                bool sane = true;
                for (float h : layer.heights) {
                    if (!std::isfinite(h) || std::abs(h) > 50000.0f) { sane = false; break; }
                    if (h < layer.minHeight - 8.0f || h > layer.maxHeight + 8.0f) { sane = false; break; }
                }
                if (sane) useFlat = false;
            }

            for (int ly = 0; ly <= layer.height; ly++) {
                for (int lx = 0; lx <= layer.width; lx++) {
                    int mgx = baseGx + layer.y + ly;
                    int mgy = baseGy + layer.x + lx;
                    if (mgx >= gridW || mgy >= gridH) continue;

                    float h;
                    if (!useFlat) {
                        int layerIdx = ly * layerGridW + lx;
                        h = layer.heights[layerIdx];
                    } else {
                        h = layer.minHeight;
                    }

                    surface.heights[mgy * gridW + mgx] = h;
                    const size_t layerIdx = static_cast<size_t>(ly) * layerGridW + lx;
                    if (!surface.depths.empty() && layer.depths.size() == numVertices) {
                        surface.depths[mgy * gridW + mgx] = layer.depths[layerIdx];
                    }
                    if (!surface.uvs.empty() && layer.uvs.size() == numVertices * 2) {
                        surface.uvs[(mgy * gridW + mgx) * 2] = layer.uvs[layerIdx * 2];
                        surface.uvs[(mgy * gridW + mgx) * 2 + 1] = layer.uvs[layerIdx * 2 + 1];
                    }
                    if (h < surface.minHeight) surface.minHeight = h;
                    if (h > surface.maxHeight) surface.maxHeight = h;
                }
            }

            // Which of this chunk's sixty-four sub-tiles are drawn. The rule
            // lives in water_mask.hpp so it can be checked without a device;
            // this places the answer, in the chunk-row-to-render-X order the
            // merged grid uses.
            const uint64_t chunkMask = chunkWaterMask(
                layer.mask, layer.x, layer.y, layer.width, layer.height, isOcean);
            for (int row = 0; row < 8; row++) {
                for (int col = 0; col < 8; col++) {
                    if (!(chunkMask & (uint64_t{1} << (row * 8 + col)))) continue;
                    const int mx = baseGx + row;
                    const int my = baseGy + col;
                    if (mx >= MERGED_W || my >= MERGED_W) continue;
                    const int mergedTileIdx = my * MERGED_W + mx;
                    surface.mask[mergedTileIdx / 8] |=
                        static_cast<uint8_t>(1 << (mergedTileIdx % 8));
                }
            }
        }

        // Stormwind water lowering
        if (isStormwindArea && surface.minHeight > 94.0f) {
            float distToMoonwell = glm::distance(glm::vec2(tileWorldX, tileWorldY), moonwellPos2D);
            if (distToMoonwell > 300.0f) {
                for (float& h : surface.heights) h -= 1.0f;
                surface.minHeight -= 1.0f;
                surface.maxHeight -= 1.0f;
            }
        }

        createWaterMesh(surface);
        if (surface.indexCount > 0 && vkCtx) {
            updateMaterialUBO(surface);
        }
        surfaces.push_back(std::move(surface));
        totalSurfaces++;
    }

    if (totalSurfaces > 0) {
        LOG_INFO("Water: Loaded ", totalSurfaces, " surfaces from tile [", tileX, ",", tileY,
                 "] (", mergeGroups.size(), " groups), total surfaces: ", surfaces.size());
    }
}

void WaterRenderer::removeTile(int tileX, int tileY) {
    int removed = 0;
    auto it = surfaces.begin();
    while (it != surfaces.end()) {
        if (it->tileX == tileX && it->tileY == tileY) {
            destroyWaterMesh(*it);
            it = surfaces.erase(it);
            removed++;
        } else {
            ++it;
        }
    }
    if (removed > 0) {
        LOG_DEBUG("Water: Removed ", removed, " surfaces for tile [", tileX, ",", tileY, "], remaining: ", surfaces.size());
    }
}

void WaterRenderer::loadFromWMO(const pipeline::WMOLiquid& liquid, const glm::mat4& modelMatrix,
                                 uint32_t wmoId, uint32_t momtColor) {
    if (!liquid.hasLiquid() || liquid.xTiles == 0 || liquid.yTiles == 0) return;
    if (liquid.xVerts < 2 || liquid.yVerts < 2) return;
    if (liquid.xTiles != liquid.xVerts - 1 || liquid.yTiles != liquid.yVerts - 1) return;
    if (liquid.xTiles > 64 || liquid.yTiles > 64) return;

    WaterSurface surface;
    surface.tileX = -1;
    surface.tileY = -1;
    surface.wmoId = wmoId;
    // The LiquidType the client draws (0x793d20): the group's, resolved at
    // load (stock_wmo_liquid::groupLiquidType), with water in a group that is
    // not exterior turned into 17, "WMO Water - Interior". None of the water
    // types carries the LiquidType flag 0x200 that would keep it, so the DBC
    // is not needed here. MLIQ's own field is a MOMT material, not a type.
    surface.liquidType = static_cast<uint16_t>(pipeline::stock_wmo_liquid::drawnLiquidType(
        liquid.liquidTypeId != 0 ? liquid.liquidTypeId : 13u, liquid.groupFlags, 0u));
    surface.xOffset = 0;
    surface.yOffset = 0;
    surface.width = static_cast<uint8_t>(std::min<uint32_t>(255, liquid.xTiles));
    surface.height = static_cast<uint8_t>(std::min<uint32_t>(255, liquid.yTiles));

    constexpr float WMO_LIQUID_TILE_SIZE = pipeline::stock_wmo_liquid::kLiquidTileSize;
    const glm::vec3 localBase(liquid.basePosition.x, liquid.basePosition.y, liquid.basePosition.z);
    const glm::vec3 localStepX(WMO_LIQUID_TILE_SIZE, 0.0f, 0.0f);
    const glm::vec3 localStepY(0.0f, WMO_LIQUID_TILE_SIZE, 0.0f);

    surface.origin = glm::vec3(modelMatrix * glm::vec4(localBase, 1.0f));
    surface.stepX = glm::vec3(modelMatrix * glm::vec4(localStepX, 0.0f));
    surface.stepY = glm::vec3(modelMatrix * glm::vec4(localStepY, 0.0f));
    surface.position = surface.origin;

    float stepXLen = glm::length(surface.stepX);
    float stepYLen = glm::length(surface.stepY);
    glm::vec3 planeN = glm::cross(surface.stepX, surface.stepY);
    float planeNLenSq = glm::dot(planeN, planeN);
    float nz = (planeNLenSq > 1e-8f) ? std::abs(planeN.z * glm::inversesqrt(planeNLenSq)) : 0.0f;
    float spanX = stepXLen * static_cast<float>(surface.width);
    float spanY = stepYLen * static_cast<float>(surface.height);
    if (stepXLen < 0.2f || stepXLen > 12.0f ||
        stepYLen < 0.2f || stepYLen > 12.0f ||
        nz < 0.60f || spanX > 450.0f || spanY > 450.0f) return;

    const int gridWidth = static_cast<int>(surface.width) + 1;
    const int gridHeight = static_cast<int>(surface.height) + 1;
    const int vertexCount = gridWidth * gridHeight;

    // Each vertex at its own height (0x7c8c60): MLIQ's 8-byte vertices end in
    // a float that is the surface's z in the model, and the client builds the
    // sheet from those - x and y from the corner in 25/6-yard steps, z from
    // the vertex. This used to be a flat sheet a yard below the corner's z.
    surface.heights.resize(vertexCount);
    surface.minHeight = std::numeric_limits<float>::max();
    surface.maxHeight = std::numeric_limits<float>::lowest();
    for (int y = 0; y < gridHeight; ++y) {
        for (int x = 0; x < gridWidth; ++x) {
            const size_t i = static_cast<size_t>(y) * gridWidth + x;
            const float h = i < liquid.heights.size() ? liquid.heights[i] : liquid.basePosition.z;
            const glm::vec3 local = localBase + localStepX * static_cast<float>(x) +
                                    localStepY * static_cast<float>(y);
            const float z = (modelMatrix * glm::vec4(local.x, local.y, h, 1.0f)).z;
            surface.heights[i] = z;
            surface.minHeight = std::min(surface.minHeight, z);
            surface.maxHeight = std::max(surface.maxHeight, z);
        }
    }

    if (surface.origin.z > 2000.0f || surface.origin.z < -500.0f) return;

    // The stock vertex (0x7a7b00): water carries its depth byte (byte 0) and
    // a uv of (vertex - MLIQ corner) x 0.24 a yard, one repeat a cell - so
    // the cell grid itself; magma and slime carry s,t in 1/256ths.
    {
        const LiquidInfo info = resolveLiquid(surface.liquidType);
        const bool magmaFormat = info.basicType >= 2;
        const bool haveWords = liquid.vertexWords.size() >= static_cast<size_t>(vertexCount);
        surface.uvs.resize(static_cast<size_t>(vertexCount) * 2);
        if (!magmaFormat) surface.depths.assign(static_cast<size_t>(vertexCount), 0xFF);
        for (int y = 0; y < gridHeight; ++y) {
            for (int x = 0; x < gridWidth; ++x) {
                const size_t i = static_cast<size_t>(y) * gridWidth + x;
                const uint32_t w = haveWords ? liquid.vertexWords[i] : 0u;
                if (magmaFormat) {
                    surface.uvs[i * 2] = static_cast<float>(static_cast<int16_t>(w & 0xFFFFu)) / 256.0f;
                    surface.uvs[i * 2 + 1] = static_cast<float>(static_cast<int16_t>(w >> 16)) / 256.0f;
                } else {
                    surface.uvs[i * 2] = static_cast<float>(x);
                    surface.uvs[i * 2 + 1] = static_cast<float>(y);
                    if (haveWords) surface.depths[i] = static_cast<uint8_t>(w & 0xFFu);
                }
            }
        }
        // A group that is not exterior (MOGP & 0x48 == 0) - unless the type
        // has flag 0x200, as WMO Ocean does - is lit by the fixed interior
        // light (every colour 1) and coloured by its MOMT colour; outside,
        // the vertex colour is white (0x7a7b00, 0x7d4f40).
        surface.interior = (liquid.groupFlags & 0x48u) == 0 && (info.flags & 0x200u) == 0;
        if (surface.interior) {
            // MOMT colours are BGRA bytes: 0xAARRGGBB.
            surface.vertexColor = glm::vec4(static_cast<float>((momtColor >> 16) & 0xFFu),
                                            static_cast<float>((momtColor >> 8) & 0xFFu),
                                            static_cast<float>(momtColor & 0xFFu),
                                            static_cast<float>((momtColor >> 24) & 0xFFu)) / 255.0f;
        }
    }

    // Build tile mask from MLIQ flags
    size_t tileCount = static_cast<size_t>(surface.width) * static_cast<size_t>(surface.height);
    size_t maskBytes = (tileCount + 7) / 8;
    surface.mask.assign(maskBytes, 0x00);
    for (size_t t = 0; t < tileCount; t++) {
        bool hasLiquid = true;
        int tx = static_cast<int>(t) % surface.width;
        int ty = static_cast<int>(t) / surface.width;

        // Standard WoW check: low nibble 0x0F = "don't render"
        if (t < liquid.flags.size()) {
            if ((liquid.flags[t] & 0x0F) == 0x0F) {
                hasLiquid = false;
            }
        }
        // Suppress water tiles that extend into enclosed WMO areas
        // (e.g. Stormwind barracks stairway where canal water pokes through)
        // Render coords: x=wowY(west), y=wowX(north)
        if (hasLiquid) {
            glm::vec3 tileWorld = surface.origin +
                surface.stepX * (static_cast<float>(tx) + 0.5f) +
                surface.stepY * (static_cast<float>(ty) + 0.5f);
            // Stormwind Barracks / Stockade stairway:
            // Stockade entrance at approximately render (-8768, 848)
            if (tileWorld.x > -8790.0f && tileWorld.x < -8735.0f &&
                tileWorld.y > 828.0f && tileWorld.y < 878.0f) {
                hasLiquid = false;
            }
        }
        if (hasLiquid) {
            size_t byteIdx = t / 8;
            size_t bitIdx = t % 8;
            surface.mask[byteIdx] |= (1 << bitIdx);
        }
    }

    createWaterMesh(surface);

    // Count how many tiles passed the flag check and compute bounds
    size_t activeTiles = 0;
    float minWX = 1e9f, maxWX = -1e9f, minWY = 1e9f, maxWY = -1e9f;
    for (size_t t = 0; t < tileCount; t++) {
        size_t byteIdx = t / 8;
        size_t bitIdx = t % 8;
        if (surface.mask[byteIdx] & (1 << bitIdx)) {
            activeTiles++;
            int atx = static_cast<int>(t) % surface.width;
            int aty = static_cast<int>(t) / surface.width;
            glm::vec3 tw = surface.origin +
                surface.stepX * (static_cast<float>(atx) + 0.5f) +
                surface.stepY * (static_cast<float>(aty) + 0.5f);
            if (tw.x < minWX) minWX = tw.x;
            if (tw.x > maxWX) maxWX = tw.x;
            if (tw.y < minWY) minWY = tw.y;
            if (tw.y > maxWY) maxWY = tw.y;
        }
    }
    LOG_DEBUG("WMO water: origin=(", surface.origin.x, ",", surface.origin.y, ",", surface.origin.z,
             ") tiles=", static_cast<int>(surface.width), "x", static_cast<int>(surface.height),
             " active=", activeTiles, "/", tileCount,
             " wmoId=", wmoId, " indexCount=", surface.indexCount,
             " bounds x=[", minWX, "..", maxWX, "] y=[", minWY, "..", maxWY, "]");

    if (surface.indexCount > 0) {
        if (vkCtx) updateMaterialUBO(surface);
        surfaces.push_back(std::move(surface));
    }
}

void WaterRenderer::removeWMO(uint32_t wmoId) {
    if (wmoId == 0) return;
    auto it = surfaces.begin();
    while (it != surfaces.end()) {
        if (it->wmoId == wmoId) {
            destroyWaterMesh(*it);
            it = surfaces.erase(it);
        } else {
            ++it;
        }
    }
}

void WaterRenderer::clear() {
    for (auto& surface : surfaces) {
        destroyWaterMesh(surface);
    }
    surfaces.clear();
}

// ==============================================================
// Rendering
// ==============================================================

void WaterRenderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                            const Camera& camera, float time, bool use1x, uint32_t frameIndex) {
    // HD water (not stock): its pipeline in place of the water's, and whether
    // this frame's colour copy, taken just before the water, can be read.
    VkPipeline hdPipeline = VK_NULL_HANDLE;
    if (hdWater_ && !hdBuildFailed_) {
        VkPipeline& hp = use1x ? hd1xPipeline_ : hdPipeline_;
        if (!hp) {
            hp = use1x ? buildHdPipeline(water1xRenderPass, VK_SAMPLE_COUNT_1_BIT)
                       : buildHdPipeline(vkCtx->getImGuiRenderPass(), vkCtx->getMsaaSamples());
            if (!hp) hdBuildFailed_ = true;
        }
        hdPipeline = hp;
    }
    if (hdPipeline) {
        frameUBO_.hdParams = glm::vec4(enhancedWater_ ? 2.0f : 1.0f, colorCopiedThisFrame_ ? 1.0f : 0.0f,
                                       colorCopyUvScale_.x, colorCopyUvScale_.y);
    } else {
        frameUBO_.hdParams = glm::vec4(0.0f);
    }
    colorCopiedThisFrame_ = false;

    // The wake as it stands, into this frame's slot - safe now, the frame has
    // waited for the GPU to finish with it.
    uploadFrameUBO();
    VkPipeline pipeline = (use1x && water1xPipeline) ? water1xPipeline : waterPipeline;
    if (hdPipeline && pipeline) pipeline = hdPipeline;
    if (!renderingEnabled || surfaces.empty() || !pipeline) {
        if (renderDiagCounter_++ % 300 == 0 && !surfaces.empty()) {
            LOG_WARNING("Water: render skipped - enabled=", renderingEnabled,
                        " surfaces=", surfaces.size(),
                        " pipeline=", (pipeline ? "ok" : "null"),
                        " use1x=", use1x);
        }
        return;
    }
    uint32_t fi = frameIndex % SCENE_HISTORY_FRAMES;
    VkDescriptorSet activeSceneSet = sceneHistory[fi].sceneSet;
    if (!activeSceneSet) {
        if (renderDiagCounter_++ % 300 == 0) {
            LOG_WARNING("Water: render skipped - sceneSet is null, surfaces=", surfaces.size());
        }
        return;
    }

    // Frustum culling setup
    Frustum frustum;
    frustum.extractFromMatrix(camera.getViewProjectionMatrix());

    // Frustum cull: the AABB from the surface's origin and step vectors.
    auto surfaceInView = [&frustum](const WaterSurface& surface) {
        const glm::vec3 extentX = surface.stepX * static_cast<float>(surface.width);
        const glm::vec3 extentY = surface.stepY * static_cast<float>(surface.height);
        const glm::vec3 c0 = surface.origin;
        const glm::vec3 c1 = surface.origin + extentX;
        const glm::vec3 c2 = surface.origin + extentY;
        const glm::vec3 c3 = surface.origin + extentX + extentY;
        const glm::vec3 aabbMin(
            std::min({c0.x, c1.x, c2.x, c3.x}),
            std::min({c0.y, c1.y, c2.y, c3.y}),
            surface.minHeight
        );
        const glm::vec3 aabbMax(
            std::max({c0.x, c1.x, c2.x, c3.x}),
            std::max({c0.y, c1.y, c2.y, c3.y}),
            surface.maxHeight
        );
        return frustum.intersectsAABB(aabbMin, aabbMax);
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                             0, 1, &perFrameSet, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                             2, 1, &activeSceneSet, 0, nullptr);

    for (const auto& surface : surfaces) {
        if (surface.vertexBuffer == VK_NULL_HANDLE || surface.indexCount == 0) continue;
        if (!surface.materialSet) continue;
        if (!surfaceInView(surface)) continue;

        // Basic type and texture sequence from LiquidType.dbc, not from the
        // ID's position in the table: Naxxramas slime is ID 21 and a magma.
        // The surface is the flat sheet the ADT and WMO give it: the stock
        // liquid has no wave displacement (vsLiquidWater moves no vertex).
        const LiquidInfo liquid = resolveLiquid(surface.liquidType);
        uint8_t basicType = liquid.basicType;
        const auto timeMs = static_cast<uint64_t>(std::max(0.0f, time) * 1000.0f);

        // The sequence's frame for now (0x8a1d60): the whole sequence once
        // every LiquidType Int[1] ms - 1250, so lake_a's 30 frames run at 24
        // a second and fast_a's 16 at 12.8 - not a fixed 30 a second.
        VkDescriptorSet liquidSet = blankLiquidSet_;
        bool liquidTexValid = false;
        if (liquid.textureSet >= 0 &&
            static_cast<size_t>(liquid.textureSet) < liquidTextureSets_.size()) {
            const auto& sets = liquidTextureSets_[liquid.textureSet].sets;
            if (!sets.empty()) {
                const size_t frame = stock_liquid::frameIndex(timeMs, liquid.framePeriodMs, sets.size());
                liquidSet = sets[frame];
                liquidTexValid = true;
            }
        }
        if (!liquidSet) continue;

        WaterPushConstants push{};
        push.model = glm::mat4(1.0f);
        push.texScale = liquid.texScale;
        push.texOffsetU = stock_liquid::scrollOffset(timeMs, liquid.scrollU);
        push.texOffsetV = stock_liquid::scrollOffset(timeMs, liquid.scrollV);
        push.liquidBasicType = static_cast<float>(basicType);
        push.screenSize = glm::vec2(static_cast<float>(renderExtent_.width),
                                    static_cast<float>(renderExtent_.height));
        // Asked of the camera rather than written out in the shader, which is
        // how the two came to disagree: the shader linearised the depth buffer
        // against a near plane of 0.05 while the camera's is 0.5.
        push.depthRange = glm::vec2(camera.getNearPlane(), camera.getFarPlane());
        push.sceneValid = sceneHistoryReady ? 1.0f : 0.0f;
        push.liquidTexValid = liquidTexValid ? 1.0f : 0.0f;
        // Magma and slime return before the foam in the shader; only water
        // and ocean take it.
        push.shoreFoam = shorelineFoam_ ? 1.0f : 0.0f;
        // Water inside a WMO is LiquidType 13 or 17 in the stock client
        // (0x793d20 swaps an interior group's water for 17), and both draw
        // proceduralWmoWaterTex. The type Wowee has for a WMO liquid is the
        // MLIQ material, not always a LiquidType ID, so a WMO's plain water
        // takes the WMO ramp whatever ID it carries.
        uint8_t depthRamp = liquid.depthRamp;
        if (surface.wmoId != 0 && basicType == 0) depthRamp = 2;
        push.depthRamp = static_cast<float>(depthRamp);
        push.texRotCos = liquid.texRotCos;
        push.texRotSin = liquid.texRotSin;
        push.rainRipple = rainRipple_;

        vkCmdPushConstants(cmd, pipelineLayout,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(WaterPushConstants), &push);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                 1, 1, &surface.materialSet, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                 3, 1, &liquidSet, 0, nullptr);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &surface.vertexBuffer, &offset);
        vkCmdBindIndexBuffer(cmd, surface.indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        vkCmdDrawIndexed(cmd, static_cast<uint32_t>(surface.indexCount), 1, 0, 0, 0);
    }

    // Sunlight under water (not stock): every water and ocean surface in view
    // once more, after the water, with the caustics blend - it brightens the
    // shallows as seen through the surface where the net of light crosses the
    // bed. (Drawn under the water instead it was all but hidden: the stock
    // water's alpha leaves a tenth or two of the bed showing.) Needs the
    // scene's depth copy (sceneHistoryReady) and the sun up (strength > 0).
    if (causticsStrength_ > 0.0f && sceneHistoryReady && causticsEnabled_ && !causticsBuildFailed_) {
        VkPipeline& cp = use1x ? caustics1xPipeline_ : causticsPipeline_;
        if (!cp) {
            cp = use1x ? buildCausticsPipeline(water1xRenderPass, VK_SAMPLE_COUNT_1_BIT)
                       : buildCausticsPipeline(vkCtx->getImGuiRenderPass(), vkCtx->getMsaaSamples());
            if (!cp) causticsBuildFailed_ = true;
        }
        if (cp) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cp);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                     0, 1, &perFrameSet, 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                     2, 1, &activeSceneSet, 0, nullptr);
            WaterPushConstants push{};
            push.model = glm::mat4(1.0f);
            push.screenSize = glm::vec2(static_cast<float>(renderExtent_.width),
                                        static_cast<float>(renderExtent_.height));
            push.depthRange = glm::vec2(camera.getNearPlane(), camera.getFarPlane());
            push.sceneValid = 1.0f;
            push.caustics = causticsStrength_;
            vkCmdPushConstants(cmd, pipelineLayout,
                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                0, sizeof(WaterPushConstants), &push);
            const glm::vec3 eye = camera.getPosition();
            for (const auto& surface : surfaces) {
                if (surface.vertexBuffer == VK_NULL_HANDLE || surface.indexCount == 0) continue;
                const LiquidInfo liquid = resolveLiquid(surface.liquidType);
                if (!water_caustics::liquidTakesCaustics(liquid.basicType, surface.interior)) continue;
                if (!surfaceInView(surface)) continue;
                // Nothing is drawn past the net's fade-out distance: a surface
                // wholly beyond it (most of an ocean) is not drawn again at all.
                if (water_caustics::beyondReach(surface.origin, surface.stepX * static_cast<float>(surface.width),
                                                surface.stepY * static_cast<float>(surface.height),
                                                surface.minHeight, surface.maxHeight, eye)) continue;
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &surface.vertexBuffer, &offset);
                vkCmdBindIndexBuffer(cmd, surface.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, static_cast<uint32_t>(surface.indexCount), 1, 0, 0, 0);
            }
        }
    }

}

void WaterRenderer::captureSceneHistory(VkCommandBuffer cmd,
                                        VkImage srcColorImage,
                                        VkImage srcDepthImage,
                                        VkExtent2D srcExtent,
                                        bool srcDepthIsMsaa,
                                        uint32_t frameIndex) {
    uint32_t fi = frameIndex % SCENE_HISTORY_FRAMES;
    auto& sh = sceneHistory[fi];
    if (!vkCtx || !cmd || !sh.colorImage || !sh.depthImage || srcExtent.width == 0 || srcExtent.height == 0) {
        return;
    }

    // The scene is not always rendered at the history's resolution - FSR renders
    // smaller and upscales later. Copying the smaller image into the corner of a
    // larger history would leave the refraction sampling a fraction of the frame
    // stretched over all of it, so scale with a blit whenever they differ and
    // keep the cheap straight copy when they match.
    const bool needsScaling = (srcExtent.width != sceneHistoryExtent.width ||
                               srcExtent.height != sceneHistoryExtent.height);
    VkExtent2D copyExtent{
        .width = std::min(srcExtent.width, sceneHistoryExtent.width),
        .height = std::min(srcExtent.height, sceneHistoryExtent.height)
    };
    if (copyExtent.width == 0 || copyExtent.height == 0) return;

    auto barrier2 = [&](VkImage image,
                        VkImageAspectFlags aspect,
                        VkImageLayout oldLayout,
                        VkImageLayout newLayout,
                        VkAccessFlags srcAccess,
                        VkAccessFlags dstAccess,
                        VkPipelineStageFlags srcStage,
                        VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = srcStage;
        b.dstStageMask = dstStage;
        b.oldLayout = oldLayout;
        b.newLayout = newLayout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange.aspectMask = aspect;
        b.subresourceRange.baseMipLevel = 0;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount = 1;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        cmdPipelineBarrier2(cmd, dep);
    };

    // No colour for the stock water. It does not refract (water.frag:
    // SceneColor is bound but not read), and the reflection binding only
    // names this image as a fallback view while reflections are off. Copying
    // the whole frame into it was most of the 0.27 ms "water refraction copy"
    // the shoreline foam cost on the Iris Xe (enhancements-plan.md 3.12); the
    // foam reads the depth.
    //
    // HD water reads it, for its refraction,
    // and only from a caller that hands over the scene as it stands just
    // before the water (the continuation after the scene pass): a copy taken
    // after the water would feed the water its own picture. Copied 1:1 into
    // the corner of the history (the scene is drawn at the upscaler's size,
    // smaller than the history), which the shader scales its uv to. Every
    // such scene image leaves its pass in PRESENT_SRC (the scene pass and its
    // continuation both end there).
    if (hdWater_ && srcColorImage != VK_NULL_HANDLE && sh.colorImage &&
        srcExtent.width <= sceneHistoryExtent.width && srcExtent.height <= sceneHistoryExtent.height) {
        barrier2(srcColorImage, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier2(sh.colorImage, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy colorCopy{};
        colorCopy.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
        colorCopy.dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
        colorCopy.extent = {.width = srcExtent.width, .height = srcExtent.height, .depth = 1};
        vkCmdCopyImage(cmd, srcColorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       sh.colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &colorCopy);
        barrier2(sh.colorImage, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        barrier2(srcColorImage, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        colorCopiedThisFrame_ = true;
        colorCopyUvScale_ = glm::vec2(static_cast<float>(srcExtent.width) / static_cast<float>(sceneHistoryExtent.width),
                                      static_cast<float>(srcExtent.height) / static_cast<float>(sceneHistoryExtent.height));
    }

    // Depth source: only copy when source is single-sampled.
    if (!srcDepthIsMsaa && srcDepthImage != VK_NULL_HANDLE) {
        barrier2(srcDepthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier2(sh.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        if (needsScaling) {
            // Depth must not be filtered - an interpolated depth is a surface
            // that exists nowhere.
            VkImageBlit depthBlit{};
            depthBlit.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
            depthBlit.dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
            depthBlit.srcOffsets[1] = {.x = static_cast<int32_t>(srcExtent.width),
                                       .y = static_cast<int32_t>(srcExtent.height), .z = 1};
            depthBlit.dstOffsets[1] = {.x = static_cast<int32_t>(sceneHistoryExtent.width),
                                       .y = static_cast<int32_t>(sceneHistoryExtent.height), .z = 1};
            vkCmdBlitImage(cmd, srcDepthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           sh.depthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &depthBlit, VK_FILTER_NEAREST);
        } else {
            VkImageCopy depthCopy{};
            depthCopy.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
            depthCopy.dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
            depthCopy.extent = {.width = copyExtent.width, .height = copyExtent.height, .depth = 1};
            vkCmdCopyImage(cmd, srcDepthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           sh.depthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &depthCopy);
        }

        barrier2(sh.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        barrier2(srcDepthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
    }

    sceneHistoryReady = true;
    // The camera this slot's copy was rendered with, for the shaders to find
    // the surface's pixel in it (WaterFrameUBOData::historyViewProj).
    historyViewProj_[fi] = frameViewProj_;
    uploadFrameUBO();
}

// ==============================================================
// Mesh creation (Vulkan upload instead of GL)
// ==============================================================

void WaterRenderer::createWaterMesh(WaterSurface& surface) {
    const int gridWidth = surface.width + 1;
    const int gridHeight = surface.height + 1;
    constexpr float VISUAL_WATER_Z_BIAS = 0.02f;

    std::vector<float> vertices;
    std::vector<uint32_t> indices;
    const LiquidInfo liquid = resolveLiquid(surface.liquidType);

    for (int y = 0; y < gridHeight; y++) {
        for (int x = 0; x < gridWidth; x++) {
            int index = y * gridWidth + x;
            float height = (index < static_cast<int>(surface.heights.size()))
                ? surface.heights[index] : surface.minHeight;

            glm::vec3 pos = surface.origin +
                            surface.stepX * static_cast<float>(x) +
                            surface.stepY * static_cast<float>(y);
            pos.z = height + VISUAL_WATER_Z_BIAS;

            // pos (3 floats)
            vertices.push_back(pos.x);
            vertices.push_back(pos.y);
            vertices.push_back(pos.z);
            // The normal slot carries the stock vertex's ramp coordinate
            // (attrib 7's v, through the stage 0 matrix): its depth byte
            // over 42 or 255 by LiquidType Int[0], clamped at 1 per vertex as
            // the client's tables do, times Float[2]. 255, the deep end, when
            // the data has none. The normal itself is up (0x7ce390), so the
            // shader does not read one.
            const size_t vi = static_cast<size_t>(index);
            const uint8_t depthByte = vi < surface.depths.size() ? surface.depths[vi] : uint8_t{0xFF};
            vertices.push_back(stock_liquid::rampCoordinate(depthByte, liquid.depthDivisor) * liquid.rampScale);
            vertices.push_back(0.0f);
            vertices.push_back(1.0f);
            // The liquid texture's uv before its texture matrix (attrib 6):
            // the data's own (magma, WMO cells), else world x,y x 0.06 - one
            // repeat every 16.67 yards (0x7ce390, 0xa40410).
            if (vi * 2 + 1 < surface.uvs.size()) {
                vertices.push_back(surface.uvs[vi * 2]);
                vertices.push_back(surface.uvs[vi * 2 + 1]);
            } else {
                vertices.push_back(pos.x * 0.06f);
                vertices.push_back(pos.y * 0.06f);
            }
        }
    }

    // Generate indices respecting render mask (same logic as GL version)
    for (int y = 0; y < gridHeight - 1; y++) {
        for (int x = 0; x < gridWidth - 1; x++) {
            const bool renderTile =
                waterCellRendered(surface.mask, surface.wmoId, surface.width,
                                  surface.xOffset, surface.yOffset, x, y);

            if (!renderTile) continue;

            int topLeft = y * gridWidth + x;
            int topRight = topLeft + 1;
            int bottomLeft = (y + 1) * gridWidth + x;
            int bottomRight = bottomLeft + 1;

            indices.push_back(topLeft);
            indices.push_back(bottomLeft);
            indices.push_back(topRight);
            indices.push_back(topRight);
            indices.push_back(bottomLeft);
            indices.push_back(bottomRight);
        }
    }

    if (indices.empty()) return;
    surface.indexCount = static_cast<int>(indices.size());

    if (!vkCtx) return;

    // Upload vertex buffer
    VkDeviceSize vbSize = vertices.size() * sizeof(float);
    AllocatedBuffer vb = uploadBuffer(*vkCtx, vertices.data(), vbSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    surface.vertexBuffer = vb.buffer;
    surface.vertexAlloc = vb.allocation;

    // Upload index buffer
    VkDeviceSize ibSize = indices.size() * sizeof(uint32_t);
    AllocatedBuffer ib = uploadBuffer(*vkCtx, indices.data(), ibSize,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    surface.indexBuffer = ib.buffer;
    surface.indexAlloc = ib.allocation;
}

void WaterRenderer::destroyWaterMesh(WaterSurface& surface) {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    VmaAllocator allocator = vkCtx->getAllocator();

    ::VkBuffer vertexBuffer = surface.vertexBuffer;
    VmaAllocation vertexAlloc = surface.vertexAlloc;
    ::VkBuffer indexBuffer = surface.indexBuffer;
    VmaAllocation indexAlloc = surface.indexAlloc;
    ::VkBuffer materialUBO = surface.materialUBO;
    VmaAllocation materialAlloc = surface.materialAlloc;
    VkDescriptorPool pool = materialDescPool;
    VkDescriptorSet materialSet = surface.materialSet;

    surface.vertexBuffer = VK_NULL_HANDLE;
    surface.vertexAlloc = VK_NULL_HANDLE;
    surface.indexBuffer = VK_NULL_HANDLE;
    surface.indexAlloc = VK_NULL_HANDLE;
    surface.materialUBO = VK_NULL_HANDLE;
    surface.materialAlloc = VK_NULL_HANDLE;
    surface.materialSet = VK_NULL_HANDLE;

    vkCtx->deferAfterAllFrameFences([device, allocator, vertexBuffer, vertexAlloc, indexBuffer, indexAlloc,
                                     materialUBO, materialAlloc, pool, materialSet]() {
        if (vertexBuffer) {
            AllocatedBuffer ab{}; ab.buffer = vertexBuffer; ab.allocation = vertexAlloc;
            destroyBuffer(allocator, ab);
        }
        if (indexBuffer) {
            AllocatedBuffer ab{}; ab.buffer = indexBuffer; ab.allocation = indexAlloc;
            destroyBuffer(allocator, ab);
        }
        if (materialUBO) {
            AllocatedBuffer ab{}; ab.buffer = materialUBO; ab.allocation = materialAlloc;
            destroyBuffer(allocator, ab);
        }
        if (materialSet && pool) {
            VkDescriptorSet set = materialSet;
            vkFreeDescriptorSets(device, pool, 1, &set);
        }
    });
}

// ==============================================================
// Query functions (data-only, no GL)
// ==============================================================

/// Where a point sits on a surface, if that surface has water there at all.
///
/// Every water query starts with the same pair of questions: does the point
/// fall on this surface, and does its render mask say the cell it lands in
/// holds water. Three of them asked it separately - the height, the nearest
/// height and the liquid type - and the cell arithmetic between the two halves
/// was written out each time.
///
/// The cell is clamped at the far edge because a grid of `width` cells has
/// `width + 1` corners: a point exactly on the last corner belongs to the last
/// cell, not to one past it.
std::optional<glm::vec2> WaterRenderer::wateredGridPosition(const WaterSurface& surface,
                                                            float glX, float glY) const {
    const auto grid = surfaceGridPosition(surface.origin, surface.stepX, surface.stepY,
                                          surface.width, surface.height, glX, glY);
    if (!grid) return std::nullopt;

    const int ix = std::min(static_cast<int>(grid->x), static_cast<int>(surface.width) - 1);
    const int iy = std::min(static_cast<int>(grid->y), static_cast<int>(surface.height) - 1);
    if (!waterCellRendered(surface.mask, surface.wmoId, surface.width,
                           surface.xOffset, surface.yOffset, ix, iy)) {
        return std::nullopt;
    }
    return grid;
}

std::optional<float> WaterRenderer::getWaterHeightAt(float glX, float glY) const {
    std::optional<float> best;

    for (const auto& surface : surfaces) {
        const auto grid = wateredGridPosition(surface, glX, glY);
        if (!grid) continue;

        const auto sampled = sampleGridHeight(surface.heights, surface.width,
                                              surface.height, grid->x, grid->y);
        if (!sampled) continue;
        const float h = *sampled;

        if (!best || h > *best) best = h;
    }

    return best;
}

std::optional<float> WaterRenderer::getNearestWaterHeightAt(float glX, float glY, float queryZ, float maxAbove) const {
    std::optional<float> best;
    float bestDist = 1e9f;

    for (const auto& surface : surfaces) {
        const auto grid = wateredGridPosition(surface, glX, glY);
        if (!grid) continue;

        const auto sampled = sampleGridHeight(surface.heights, surface.width,
                                              surface.height, grid->x, grid->y);
        if (!sampled) continue;
        const float h = *sampled;

        // Only consider water that's above queryZ but not too far above
        if (h < queryZ - 2.0f) continue;  // water below camera, skip
        if (h > queryZ + maxAbove) continue;  // water way above camera, skip

        float dist = std::abs(h - queryZ);
        if (!best || dist < bestDist) {
            best = h;
            bestDist = dist;
        }
    }

    return best;
}

std::optional<uint16_t> WaterRenderer::getWaterTypeAt(float glX, float glY) const {
    std::optional<float> bestHeight;
    std::optional<uint16_t> bestType;

    for (const auto& surface : surfaces) {
        if (!wateredGridPosition(surface, glX, glY)) continue;

        float h = surface.minHeight;
        if (!bestHeight || h > *bestHeight) {
            bestHeight = h;
            bestType = surface.liquidType;
        }
    }

    return bestType;
}

// ==============================================================
// Planar reflection resources
// ==============================================================

void WaterRenderer::createReflectionResources() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    VmaAllocator allocator = vkCtx->getAllocator();

    // --- Reflection color image ---
    VkImageCreateInfo colorImgCI{};
    colorImgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    colorImgCI.imageType = VK_IMAGE_TYPE_2D;
    colorImgCI.format = vkCtx->getSceneColorFormat();
    colorImgCI.extent = {.width = REFLECTION_WIDTH, .height = REFLECTION_HEIGHT, .depth = 1};
    colorImgCI.mipLevels = 1;
    colorImgCI.arrayLayers = 1;
    colorImgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    colorImgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    colorImgCI.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    colorImgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(allocator, &colorImgCI, &allocCI,
                       &reflectionColorImage, &reflectionColorAlloc, nullptr) != VK_SUCCESS) {
        LOG_ERROR("WaterRenderer: failed to create reflection color image");
        return;
    }

    VkImageViewCreateInfo colorViewCI{};
    colorViewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    colorViewCI.image = reflectionColorImage;
    colorViewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    colorViewCI.format = vkCtx->getSceneColorFormat();
    colorViewCI.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
    if (vkCreateImageView(device, &colorViewCI, nullptr, &reflectionColorView) != VK_SUCCESS) {
        LOG_ERROR("WaterRenderer: failed to create reflection color view");
        return;
    }

    // --- Reflection depth image ---
    VkImageCreateInfo depthImgCI{};
    depthImgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depthImgCI.imageType = VK_IMAGE_TYPE_2D;
    depthImgCI.format = vkCtx->getDepthFormat();
    depthImgCI.extent = {.width = REFLECTION_WIDTH, .height = REFLECTION_HEIGHT, .depth = 1};
    depthImgCI.mipLevels = 1;
    depthImgCI.arrayLayers = 1;
    depthImgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    depthImgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthImgCI.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthImgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vmaCreateImage(allocator, &depthImgCI, &allocCI,
                       &reflectionDepthImage, &reflectionDepthAlloc, nullptr) != VK_SUCCESS) {
        LOG_ERROR("WaterRenderer: failed to create reflection depth image");
        return;
    }

    VkImageViewCreateInfo depthViewCI{};
    depthViewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthViewCI.image = reflectionDepthImage;
    depthViewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthViewCI.format = vkCtx->getDepthFormat();
    depthViewCI.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
    if (vkCreateImageView(device, &depthViewCI, nullptr, &reflectionDepthView) != VK_SUCCESS) {
        LOG_ERROR("WaterRenderer: failed to create reflection depth view");
        return;
    }

    // --- Reflection sampler ---
    VkSamplerCreateInfo sampCI{};
    sampCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampCI.magFilter = VK_FILTER_LINEAR;
    sampCI.minFilter = VK_FILTER_LINEAR;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    reflectionSampler = vkCtx->getOrCreateSampler(sampCI);
    if (reflectionSampler == VK_NULL_HANDLE) {
        LOG_ERROR("WaterRenderer: failed to create reflection sampler");
        return;
    }

    // --- Reflection render pass ---
    VkAttachmentDescription colorAttach{};
    colorAttach.format = vkCtx->getSceneColorFormat();
    colorAttach.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttach.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription depthAttach{};
    depthAttach.format = vkCtx->getDepthFormat();
    depthAttach.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttach.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttach.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{.attachment = 1, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    // The scene's pipelines draw into this pass, so it carries the scene
    // pass's dependency to stay compatible with them.
    const VkSubpassDependency dep = VkContext::sceneEntryDependency();

    std::array<VkAttachmentDescription, 2> attachments = {colorAttach, depthAttach};
    VkRenderPassCreateInfo rpCI{};
    rpCI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpCI.attachmentCount = static_cast<uint32_t>(attachments.size());
    rpCI.pAttachments = attachments.data();
    rpCI.subpassCount = 1;
    rpCI.pSubpasses = &subpass;
    rpCI.dependencyCount = 1;
    rpCI.pDependencies = &dep;

    if (vkCreateRenderPass(device, &rpCI, nullptr, &reflectionRenderPass) != VK_SUCCESS) {
        LOG_ERROR("WaterRenderer: failed to create reflection render pass");
        return;
    }

    // --- Reflection framebuffer ---
    std::array<VkImageView, 2> fbAttach = {reflectionColorView, reflectionDepthView};
    VkFramebufferCreateInfo fbCI{};
    fbCI.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbCI.renderPass = reflectionRenderPass;
    fbCI.attachmentCount = static_cast<uint32_t>(fbAttach.size());
    fbCI.pAttachments = fbAttach.data();
    fbCI.width = REFLECTION_WIDTH;
    fbCI.height = REFLECTION_HEIGHT;
    fbCI.layers = 1;

    if (vkCreateFramebuffer(device, &fbCI, nullptr, &reflectionFramebuffer) != VK_SUCCESS) {
        LOG_ERROR("WaterRenderer: failed to create reflection framebuffer");
        return;
    }

    // --- Reflection UBO ---
    VkBufferCreateInfo bufCI{};
    bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCI.size = kFrameUBOStride * SCENE_HISTORY_FRAMES;
    bufCI.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

    VmaAllocationCreateInfo uboAllocCI{};
    uboAllocCI.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    uboAllocCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo mapInfo{};
    if (vmaCreateBuffer(allocator, &bufCI, &uboAllocCI,
                        &reflectionUBO, &reflectionUBOAlloc, &mapInfo) != VK_SUCCESS) {
        LOG_ERROR("WaterRenderer: failed to create reflection UBO");
        return;
    }
    reflectionUBOMapped = mapInfo.pMappedData;

    // Initialize with identity
    WaterFrameUBOData initData{};
    initData.reflViewProj = glm::mat4(1.0f);
    if (reflectionUBOMapped) {
        for (uint32_t f = 0; f < SCENE_HISTORY_FRAMES; ++f) {
            std::memcpy(static_cast<char*>(reflectionUBOMapped) + f * kFrameUBOStride,
                        &initData, sizeof(initData));
        }
    }

    // Transition reflection color image to shader-read so first frame doesn't read undefined
    vkCtx->immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = reflectionColorImage;
        barrier.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        cmdPipelineBarrier2(cmd, dep);
    });
    reflectionColorLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    LOG_INFO("Water reflection resources created (", REFLECTION_WIDTH, "x", REFLECTION_HEIGHT, ")");
}

void WaterRenderer::destroyReflectionResources() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    VmaAllocator allocator = vkCtx->getAllocator();

    if (reflectionFramebuffer) { vkDestroyFramebuffer(device, reflectionFramebuffer, nullptr); reflectionFramebuffer = VK_NULL_HANDLE; }
    if (reflectionRenderPass) { vkDestroyRenderPass(device, reflectionRenderPass, nullptr); reflectionRenderPass = VK_NULL_HANDLE; }
    if (reflectionColorView) { vkDestroyImageView(device, reflectionColorView, nullptr); reflectionColorView = VK_NULL_HANDLE; }
    if (reflectionDepthView) { vkDestroyImageView(device, reflectionDepthView, nullptr); reflectionDepthView = VK_NULL_HANDLE; }
    if (reflectionColorImage) { vmaDestroyImage(allocator, reflectionColorImage, reflectionColorAlloc); reflectionColorImage = VK_NULL_HANDLE; }
    if (reflectionDepthImage) { vmaDestroyImage(allocator, reflectionDepthImage, reflectionDepthAlloc); reflectionDepthImage = VK_NULL_HANDLE; }
    reflectionSampler = VK_NULL_HANDLE; // Owned by VkContext sampler cache
    if (reflectionUBO) {
        AllocatedBuffer ab{}; ab.buffer = reflectionUBO; ab.allocation = reflectionUBOAlloc;
        destroyBuffer(allocator, ab);
        reflectionUBO = VK_NULL_HANDLE;
        reflectionUBOMapped = nullptr;
    }
    reflectionColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

// ==============================================================
// Reflection pass begin/end
// ==============================================================

bool WaterRenderer::beginReflectionPass(VkCommandBuffer cmd) {
    if (!reflectionRenderPass || !reflectionFramebuffer || !cmd) return false;

    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass = reflectionRenderPass;
    rpInfo.framebuffer = reflectionFramebuffer;
    rpInfo.renderArea = {.offset = {.x = 0, .y = 0}, .extent = {.width = REFLECTION_WIDTH, .height = REFLECTION_HEIGHT}};

    VkClearValue clears[2]{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clears[1].depthStencil = {.depth = 1.0f, .stencil = 0};
    rpInfo.clearValueCount = 2;
    rpInfo.pClearValues = clears;

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{.x = 0, .y = 0, .width = static_cast<float>(REFLECTION_WIDTH), .height = static_cast<float>(REFLECTION_HEIGHT), .minDepth = 0.0f, .maxDepth = 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{.offset = {.x = 0, .y = 0}, .extent = {.width = REFLECTION_WIDTH, .height = REFLECTION_HEIGHT}};
    vkCmdSetScissor(cmd, 0, 1, &sc);

    return true;
}

void WaterRenderer::endReflectionPass(VkCommandBuffer cmd) {
    if (!cmd) return;
    vkCmdEndRenderPass(cmd);
    reflectionColorLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Update only the current frame's scene descriptor set with the reflection texture.
    // Updating all frames would race with in-flight command buffers that have the
    // other frame's descriptor set bound.
    if (reflectionColorView && reflectionSampler && vkCtx) {
        uint32_t fi = vkCtx->getCurrentFrame() % SCENE_HISTORY_FRAMES;
        if (sceneHistory[fi].sceneSet) {
            VkDescriptorImageInfo reflInfo{};
            reflInfo.sampler = reflectionSampler;
            reflInfo.imageView = reflectionColorView;
            reflInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = sceneHistory[fi].sceneSet;
            write.dstBinding = 2;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &reflInfo;
            vkUpdateDescriptorSets(vkCtx->getDevice(), 1, &write, 0, nullptr);
        }
    }
}

void WaterRenderer::clearWake() {
    wakePoints_.clear();
    hasWakeEmitPos_ = false;
    frameUBO_.wakeBounds = glm::vec4(0.0f);
}

void WaterRenderer::clearRings() {
    rings_.clear();
    ringTimers_.clear();
    frameUBO_.ringBounds = glm::vec4(0.0f);
}

// Ripple rings (Enhanced water). A ring lives kRingLife seconds; the shader
// draws it as a few wavelets on a front that runs out at a fixed speed,
// lower as it spreads and ages. Each source keeps its own beat, so a bobber
// bobs out slow rings and someone wading throws them close together.
void WaterRenderer::updateRings(float deltaTime, const std::vector<RingEmitter>& emitters) {
    constexpr float kRingLife = 4.5f;  // water_hd.frag FW_RING_LIFE
    for (auto& r : rings_) r.age += deltaTime;
    rings_.erase(std::remove_if(rings_.begin(), rings_.end(),
                                [](const WaterRing& r) { return r.age >= kRingLife; }),
                 rings_.end());

    std::unordered_map<uint64_t, float> timers;
    for (const auto& e : emitters) {
        auto it = ringTimers_.find(e.key);
        // A new source rings at once; a known one when its beat comes round.
        float t = it == ringTimers_.end() ? e.period : it->second + deltaTime;
        if (t >= e.period && e.strength > 0.0f) {
            if (static_cast<int>(rings_.size()) >= kMaxWaterRings) rings_.erase(rings_.begin());
            rings_.push_back({e.pos, 0.0f, e.strength});
            t = 0.0f;
        }
        timers[e.key] = t;
    }
    ringTimers_ = std::move(timers);

    const int count = std::min(static_cast<int>(rings_.size()), kMaxWaterRings);
    glm::vec2 centre(0.0f);
    for (int i = 0; i < count; ++i) centre += rings_[i].pos;
    if (count > 0) centre /= static_cast<float>(count);
    float radius = 0.0f;
    // The shader's front speed (FW_RING_SPEED) times the life, plus the
    // wavelets' reach behind and ahead of the front.
    constexpr float kRingReach = 0.85f * kRingLife + 1.0f;
    for (int i = 0; i < count; ++i) {
        const WaterRing& r = rings_[i];
        radius = std::max(radius, glm::length(r.pos - centre) + kRingReach);
        frameUBO_.rings[i] = glm::vec4(r.pos.x, r.pos.y, r.age, r.strength);
    }
    frameUBO_.ringBounds = glm::vec4(centre.x, centre.y, radius, static_cast<float>(count));
}

void WaterRenderer::updateReflectionUBO(const glm::mat4& reflViewProj) {
    frameUBO_.reflViewProj = reflViewProj;
    uploadFrameUBO();
}

// Into the slot of the frame being recorded. There was one slot for every frame
// in flight, so the reflection matrix was rewritten for the next frame while the
// GPU was still drawing water with it for the last one - the reflection was
// sampled through a camera a frame out of step with the one that rendered it,
// and shook whenever the view moved.
void WaterRenderer::uploadFrameUBO() {
    if (!reflectionUBOMapped || !vkCtx) return;
    const uint32_t slot = vkCtx->getCurrentFrame() % SCENE_HISTORY_FRAMES;
    frameUBO_.historyViewProj = historyViewProj_[slot];
    std::memcpy(static_cast<char*>(reflectionUBOMapped) + slot * kFrameUBOStride,
                &frameUBO_, sizeof(frameUBO_));
}

// Disturbance trail. Points are dropped along the path rather than parented to
// the character, so the froth stays where the water was churned and the
// character runs out of it - froth pinned under the feet reads as a decal.
void WaterRenderer::updateWake(float deltaTime, const glm::vec2& pos,
                               const glm::vec2& travelDir, float intensity, bool wading) {
    // Age the existing trail. Drift is what spreads a swimmer's two emission
    // lines apart with distance behind, which is the V.
    for (auto& p : wakePoints_) {
        p.age += deltaTime;
        p.pos += p.drift * deltaTime;
        // Barely any drag. A wake's arms are straight because the separation
        // keeps growing with distance behind; damping the drift collapsed the
        // two arms back onto each other and the V read as a single line.
        p.drift *= std::max(0.0f, 1.0f - 0.25f * deltaTime);
    }
    wakePoints_.erase(std::remove_if(wakePoints_.begin(), wakePoints_.end(),
                                     [](const WakePoint& p) { return p.age >= p.life; }),
                      wakePoints_.end());

    if (intensity > 0.0f) {
        // Emit by distance travelled, not by time: standing still in the
        // shallows should settle rather than pile froth up in one spot.
        // Close enough together that the patches overlap into a continuous
        // trail. Spaced out, each one reads as its own blob.
        const float spacing = wading ? 0.40f : 0.60f;
        bool emit = !hasWakeEmitPos_;
        if (!emit) {
            const glm::vec2 delta = pos - lastWakeEmitPos_;
            emit = glm::dot(delta, delta) >= spacing * spacing;
        }

        if (emit) {
            hasWakeEmitPos_ = true;
            lastWakeEmitPos_ = pos;

            glm::vec2 fwd = travelDir;
            const float fwdLenSq = glm::dot(fwd, fwd);
            fwd = (fwdLenSq > 1e-6f) ? fwd * glm::inversesqrt(fwdLenSq) : glm::vec2(0.0f, 1.0f);
            const glm::vec2 side(-fwd.y, fwd.x);

            auto push = [&](const glm::vec2& at, const glm::vec2& drift,
                            float life, float strength) {
                if (static_cast<int>(wakePoints_.size()) >= kMaxWakePoints) {
                    wakePoints_.erase(wakePoints_.begin());  // oldest goes first
                }
                WakePoint wp;
                wp.pos = at;
                wp.drift = drift;
                wp.life = life;
                wp.strength = strength;
                wakePoints_.push_back(wp);
            };

            if (wading) {
                // Legs punching through the surface: one churned patch per
                // stride, thrown slightly forward of where the foot lands.
                push(pos + fwd * 0.25f, side * 0.30f, 1.5f, intensity);
            } else {
                // Swimming: a pair off the shoulders, each drifting outward and
                // back. The arms open with age because the emissions keep
                // separating after the swimmer has moved on, so the trail is a V
                // even though every point is laid down on the same line.
                const float spread = 0.50f;
                const float lateral = 1.15f;
                const float life = 1.9f;
                push(pos + side * spread - fwd * 0.2f,
                     side * lateral - fwd * 0.30f, life, intensity * 0.8f);
                push(pos - side * spread - fwd * 0.2f,
                     -side * lateral - fwd * 0.30f, life, intensity * 0.8f);
            }
        }
    } else {
        hasWakeEmitPos_ = false;
    }

    // Pack for the shader, and build the bounding circle the fragment shader
    // uses to reject the rest of the sheet in one test.
    const int count = std::min(static_cast<int>(wakePoints_.size()), kMaxWakePoints);
    glm::vec2 centre(0.0f);
    for (int i = 0; i < count; ++i) centre += wakePoints_[i].pos;
    if (count > 0) centre /= static_cast<float>(count);

    float cullRadius = 0.0f;
    for (int i = 0; i < count; ++i) {
        const WakePoint& p = wakePoints_[i];
        const float age01 = std::min(1.0f, p.age / p.life);
        // Must cover the point's own spread, or froth is clipped mid-patch.
        constexpr float kMaxPatchRadius = 1.45f;
        cullRadius = std::max(cullRadius, glm::length(p.pos - centre) + kMaxPatchRadius);
        frameUBO_.wakePoints[i] = glm::vec4(p.pos.x, p.pos.y, age01, p.strength);
    }
    frameUBO_.wakeBounds = glm::vec4(centre.x, centre.y, cullRadius, static_cast<float>(count));
    // Uploaded when the frame is recorded, not here: this runs before the frame
    // has waited for its slot, and the slot it would name is still being read
    // by the frame the GPU is drawing.
    
}

// ==============================================================
// Mirror camera computations
// ==============================================================

std::optional<float> WaterRenderer::getDominantWaterHeight(const glm::vec3& cameraPos) const {
    if (surfaces.empty()) return std::nullopt;

    // Find the water surface closest to the camera (XY distance)
    float bestDist = std::numeric_limits<float>::max();
    float bestHeight = 0.0f;
    bool found = false;

    for (const auto& surface : surfaces) {
        // Skip magma/slime - only reflect water/ocean
        uint8_t basicType = (surface.liquidType == 0) ? 0 : ((surface.liquidType - 1) % 4);
        if (basicType >= 2) continue;

        // Compute center of surface in world space
        glm::vec3 center = surface.origin +
            surface.stepX * (static_cast<float>(surface.width) * 0.5f) +
            surface.stepY * (static_cast<float>(surface.height) * 0.5f);

        float dx = cameraPos.x - center.x;
        float dy = cameraPos.y - center.y;
        float dist = dx * dx + dy * dy;
        if (dist < bestDist) {
            bestDist = dist;
            bestHeight = surface.minHeight;
            found = true;
        }
    }

    if (!found) return std::nullopt;
    return bestHeight;
}

glm::mat4 WaterRenderer::computeReflectedView(const Camera& camera, float waterHeight) {
    // In this engine, Z is up. Water height is stored in the Z component.
    // Mirror camera position across Z = waterHeight plane.

    glm::vec3 camPos = camera.getPosition();
    glm::vec3 reflPos = camPos;
    reflPos.z = 2.0f * waterHeight - camPos.z;

    // Get camera forward and reflect the Z component
    glm::vec3 forward = camera.getForward();
    forward.z = -forward.z;
    glm::vec3 reflTarget = reflPos + forward;

    glm::vec3 up(0.0f, 0.0f, 1.0f);
    return glm::lookAt(reflPos, reflTarget, up);
}

glm::mat4 WaterRenderer::computeObliqueProjection(const glm::mat4& proj, const glm::mat4& view,
                                                    float waterHeight) {
    // Clip plane: everything below waterHeight in world space
    // Z is up, so the clip plane normal is (0, 0, 1)
    glm::vec4 clipPlaneWorld(0.0f, 0.0f, 1.0f, -waterHeight);
    glm::vec4 clipPlaneView = glm::transpose(glm::inverse(view)) * clipPlaneWorld;

    // Lengyel's oblique near-plane projection matrix modification
    glm::mat4 result = proj;
    glm::vec4 q;
    q.x = (glm::sign(clipPlaneView.x) + result[2][0]) / result[0][0];
    q.y = (glm::sign(clipPlaneView.y) + result[2][1]) / result[1][1];
    q.z = -1.0f;
    q.w = (1.0f + result[2][2]) / result[3][2];

    glm::vec4 c = clipPlaneView * (2.0f / glm::dot(clipPlaneView, q));
    result[0][2] = c.x;
    result[1][2] = c.y;
    result[2][2] = c.z + 1.0f;
    result[3][2] = c.w;

    return result;
}

// ==============================================================
// Separate 1x water pass (used when MSAA is active)
// ==============================================================

bool WaterRenderer::createWater1xPass(VkFormat colorFormat, VkFormat depthFormat) {
    if (!vkCtx) return false;
    VkDevice device = vkCtx->getDevice();

    VkAttachmentDescription attachments[2]{};
    // Color: load existing resolved content, store after water draw
    attachments[0].format = colorFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // Depth: load resolved depth for depth testing
    attachments[1].format = depthFormat;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{.attachment = 1, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

    VkRenderPassCreateInfo rpCI{};
    rpCI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpCI.attachmentCount = 2;
    rpCI.pAttachments = attachments;
    rpCI.subpassCount = 1;
    rpCI.pSubpasses = &subpass;
    rpCI.dependencyCount = 1;
    rpCI.pDependencies = &dep;

    if (vkCreateRenderPass(device, &rpCI, nullptr, &water1xRenderPass) != VK_SUCCESS) {
        LOG_ERROR("WaterRenderer: failed to create 1x water render pass");
        return false;
    }
    // The sibling that keeps the depth: identical but for the store op, so
    // the pipelines below draw in either.
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (vkCreateRenderPass(device, &rpCI, nullptr, &water1xKeepDepthRenderPass_) != VK_SUCCESS) {
        water1xKeepDepthRenderPass_ = VK_NULL_HANDLE;
        LOG_WARNING("WaterRenderer: failed to create the depth-keeping 1x water pass");
    }
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

    // Build 1x water pipeline against this render pass
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/water.vert.spv") ||
        !fragShader.loadFromFile(device, "assets/shaders/water.frag.spv")) {
        LOG_ERROR("WaterRenderer: failed to load shaders for 1x pipeline");
        return false;
    }

    VkVertexInputBindingDescription vertBinding{};
    vertBinding.binding = 0;
    vertBinding.stride = 8 * sizeof(float);
    vertBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> vertAttribs = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0 },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 6 * sizeof(float) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = 3 * sizeof(float) },        // ramp coordinate
    };

    water1xPipeline = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({ vertBinding }, vertAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(VK_SAMPLE_COUNT_1_BIT)
        .setLayout(pipelineLayout)
        .setRenderPass(water1xRenderPass)
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx->getPipelineCache());

    vertShader.destroy();
    fragShader.destroy();

    if (!water1xPipeline) {
        LOG_ERROR("WaterRenderer: failed to create 1x water pipeline");
        return false;
    }

    LOG_INFO("WaterRenderer: created 1x water pass and pipeline");
    return true;
}

void WaterRenderer::createWater1xFramebuffers(const std::vector<VkImageView>& swapViews,
                                               VkImageView depthView, VkExtent2D extent) {
    if (!vkCtx || !water1xRenderPass || !depthView) return;
    VkDevice device = vkCtx->getDevice();

    // Destroy old framebuffers
    for (auto fb : water1xFramebuffers) {
        if (fb) vkDestroyFramebuffer(device, fb, nullptr);
    }
    water1xFramebuffers.clear();

    water1xFramebuffers.resize(swapViews.size());
    for (size_t i = 0; i < swapViews.size(); i++) {
        VkImageView views[2] = { swapViews[i], depthView };
        VkFramebufferCreateInfo fbCI{};
        fbCI.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbCI.renderPass = water1xRenderPass;
        fbCI.attachmentCount = 2;
        fbCI.pAttachments = views;
        fbCI.width = extent.width;
        fbCI.height = extent.height;
        fbCI.layers = 1;
        if (vkCreateFramebuffer(device, &fbCI, nullptr, &water1xFramebuffers[i]) != VK_SUCCESS) {
            LOG_ERROR("WaterRenderer: failed to create 1x framebuffer ", i);
        }
    }
}

void WaterRenderer::destroyWater1xResources() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    for (auto fb : water1xFramebuffers) {
        if (fb) vkDestroyFramebuffer(device, fb, nullptr);
    }
    water1xFramebuffers.clear();
    destroy(device, water1xPipeline);
    // The 1x caustics pipeline names the pass going away; both are made
    // again on demand.
    destroyCausticsPipelines(false);
    destroyHdPipelines(false);
    if (water1xRenderPass) { vkDestroyRenderPass(device, water1xRenderPass, nullptr); water1xRenderPass = VK_NULL_HANDLE; }
    if (water1xKeepDepthRenderPass_) {
        vkDestroyRenderPass(device, water1xKeepDepthRenderPass_, nullptr);
        water1xKeepDepthRenderPass_ = VK_NULL_HANDLE;
    }
}
} // namespace rendering
} // namespace wowee
