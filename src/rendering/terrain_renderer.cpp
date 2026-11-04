#include "rendering/terrain_vertex.hpp"
#include "rendering/shadow_params.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/cliff_projection.hpp"
#include "core/hitch_legacy.hpp"
#include "rendering/stream_fade.hpp"
#include "rendering/terrain_mega_slots.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/frustum.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/logger.hpp"
#include "core/env_flag.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <unordered_set>
#include <limits>
#include <cstring>

namespace wowee {
namespace rendering {

// Matches set 1 binding 7 in terrain.frag.glsl
struct TerrainParamsUBO {
    int32_t layerCount;
    int32_t hasLayer1;
    int32_t hasLayer2;
    int32_t hasLayer3;
    int32_t layerFlags[4];  // MCLY flags, | kSpecularMaskBit for a `_s` texture
    int32_t chunkMode[4];   // big alpha, any unlit layer, any scrolling layer
};

/// terrain.vert's push block: the model matrix and the highlight's colour
/// (rgb) and power (w, 0 when off).
struct TerrainPush {
    glm::mat4 model;
    glm::vec4 specular;
};
/// The push range: TerrainPush for the vertex stage and, after it, the
/// vec4 terrain_fade.frag reads (x: how much of the chunk is still hidden).
/// The stock fragment shader reads none of it.
constexpr uint32_t kTerrainStreamFadeOffset = sizeof(TerrainPush);
constexpr uint32_t kTerrainPushRangeSize = sizeof(TerrainPush) + sizeof(glm::vec4);
constexpr VkShaderStageFlags kTerrainPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

/// Beside the MCLY flags in TerrainParams: the layer's texture is the `_s`
/// tileset, whose alpha is the specular mask.
constexpr int32_t kSpecularMaskBit = 0x10000;

namespace {
/// WOWEE_TERRAIN_MEGA_SLOTS=0 goes back to one bump allocation of the mega
/// buffers that unloading never returns; see kMegaTileSlotVerts.
const bool kTerrainMegaSlots = core::envFlagEnabled("WOWEE_TERRAIN_MEGA_SLOTS", true);
/// WOWEE_TERRAIN_SHADOW_MERGE=0 stores the mega indices relative to each
/// chunk and draws the shadow pass one chunk at a time; see uploadChunk.
const bool kTerrainShadowMerge = core::envFlagEnabled("WOWEE_TERRAIN_SHADOW_MERGE", true);
/// WOWEE_TERRAIN_SHADOW_CACHE=0 culls the shadow pass's chunks every frame;
/// see renderShadow.
const bool kTerrainShadowCache = core::envFlagEnabled("WOWEE_TERRAIN_SHADOW_CACHE", true);
/// WOWEE_TERRAIN_TILE_APPEND=0 regroups every chunk by tile whenever one is
/// added; see updateTileGrouping.
const bool kTerrainTileAppend = core::envFlagEnabled("WOWEE_TERRAIN_TILE_APPEND", true);
/// WOWEE_TERRAIN_SLOT_MIGRATE=0 leaves each tile where it landed when it
/// loaded, in a slot or not, and keeps the 42-slot buffers; see
/// updateMegaSlotPlacement.
const bool kTerrainSlotMigrate = core::envFlagEnabled("WOWEE_TERRAIN_SLOT_MIGRATE", true);

uint64_t terrainTileKey(int tileX, int tileY) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(tileX)) << 32) | static_cast<uint32_t>(tileY);
}
}  // namespace

TerrainRenderer::TerrainRenderer() = default;

TerrainRenderer::~TerrainRenderer() {
    shutdown();
}

/// Builds the fill pipeline and its wireframe derivative from a loaded shader
/// pair.
///
/// initialize() and recreatePipelines() both need exactly these two in exactly
/// these states, and each described both for itself. Answers whether the fill
/// pipeline built; the wireframe is a debug view, so its absence is a warning
/// rather than a failure. Destroying the shader modules is left to the caller,
/// which loaded them.
bool TerrainRenderer::buildMainPassPipelines(VkDevice device,
                                             wowee::rendering::VkShaderModule& vertShader,
                                             wowee::rendering::VkShaderModule& fragShader) {
    VkVertexInputBindingDescription vertexBinding{};
    vertexBinding = perVertexBinding(sizeof(pipeline::TerrainVertex));
    const std::vector<VkVertexInputAttributeDescription> vertexAttribs =
        toVkAttributes(kTerrainVertexAttributes);

    // --- Build fill pipeline (base for derivatives - shared state optimization) ---
    VkRenderPass mainPass = vkCtx->getImGuiRenderPass();

    pipeline = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({ vertexBinding }, vertexAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(vkCtx->getMsaaSamples())
        .setLayout(pipelineLayout)
        .setRenderPass(mainPass)
        .setDynamicStates(viewportAndScissorDynamic())
        .setFlags(VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT)
        .build(device, vkCtx->getPipelineCache());

    if (!pipeline) {
        LOG_ERROR("TerrainRenderer: failed to create fill pipeline");
        return false;
    }

    // --- Build wireframe pipeline (derivative of fill) ---
    // VK_POLYGON_MODE_LINE needs fillModeNonSolid, which plenty of mobile GPUs
    // do not have. Asking anyway is a validation error and a null pipeline;
    // drawing falls back to the filled one either way.
    if (!vkCtx->isWireframeSupported()) {
        LOG_WARNING("TerrainRenderer: wireframe views need fillModeNonSolid, "
                    "which this device does not support");
        return true;
    }

    wireframePipeline = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({ vertexBinding }, vertexAttribs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_LINE, VK_CULL_MODE_NONE)
        .setDepthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(vkCtx->getMsaaSamples())
        .setLayout(pipelineLayout)
        .setRenderPass(mainPass)
        .setDynamicStates(viewportAndScissorDynamic())
        .setFlags(VK_PIPELINE_CREATE_DERIVATIVE_BIT)
        .setBasePipeline(pipeline)
        .build(device, vkCtx->getPipelineCache());

    if (!wireframePipeline) {
        LOG_WARNING("TerrainRenderer: wireframe pipeline not available");
    }

    return true;
}

bool TerrainRenderer::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                                  pipeline::AssetManager* assets) {
    vkCtx = ctx;
    assetManager = assets;

    if (!vkCtx || !assetManager) {
        LOG_ERROR("TerrainRenderer: null context or asset manager");
        return false;
    }

    LOG_INFO("Initializing terrain renderer (Vulkan)");
    VkDevice device = vkCtx->getDevice();

    // --- Create material descriptor set layout (set 1) ---
    // bindings 0-6: combined image samplers (base + 3 layer + 3 alpha)
    // binding 7: uniform buffer (TerrainParams)
    // binding 8: combined image sampler (baked MCSH shadow, as light)
    std::vector<VkDescriptorSetLayoutBinding> materialBindings(9);
    for (uint32_t i = 0; i < 7; i++) {
        materialBindings[i] = {};
        materialBindings[i].binding = i;
        materialBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        materialBindings[i].descriptorCount = 1;
        materialBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    materialBindings[7] = {};
    materialBindings[7].binding = 7;
    materialBindings[7].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    materialBindings[7].descriptorCount = 1;
    materialBindings[7].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    materialBindings[8] = {};
    materialBindings[8].binding = 8;
    materialBindings[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    materialBindings[8].descriptorCount = 1;
    materialBindings[8].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    materialSetLayout = createDescriptorSetLayout(device, materialBindings);
    if (!materialSetLayout) {
        LOG_ERROR("TerrainRenderer: failed to create material set layout");
        return false;
    }

    // --- Create descriptor pool ---
    VkDescriptorPoolSize poolSizes[] = {
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = MAX_MATERIAL_SETS * 8 },
        { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = MAX_MATERIAL_SETS },
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = MAX_MATERIAL_SETS;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &materialDescPool) != VK_SUCCESS) {
        LOG_ERROR("TerrainRenderer: failed to create descriptor pool");
        return false;
    }

    // --- Create pipeline layout ---
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = kTerrainPushStages;
    pushRange.offset = 0;
    pushRange.size = kTerrainPushRangeSize;

    std::vector<VkDescriptorSetLayout> setLayouts = { perFrameLayout, materialSetLayout };
    pipelineLayout = createPipelineLayout(device, setLayouts, { pushRange });
    if (!pipelineLayout) {
        LOG_ERROR("TerrainRenderer: failed to create pipeline layout");
        return false;
    }

    // --- Load shaders ---
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/terrain.vert.spv")) {
        LOG_ERROR("TerrainRenderer: failed to load vertex shader");
        return false;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/terrain.frag.spv")) {
        LOG_ERROR("TerrainRenderer: failed to load fragment shader");
        return false;
    }

    // --- Vertex input ---
    if (!buildMainPassPipelines(device, vertShader, fragShader)) {
        vertShader.destroy();
        fragShader.destroy();
        return false;
    }

    vertShader.destroy();
    fragShader.destroy();

    // --- Create fallback textures ---
    whiteTexture = std::make_unique<VkTexture>();
    uint8_t whitePixel[4] = {255, 255, 255, 255};
    whiteTexture->upload(*vkCtx, whitePixel, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
    whiteTexture->createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                 VK_SAMPLER_ADDRESS_MODE_REPEAT);

    opaqueAlphaTexture = std::make_unique<VkTexture>();
    uint8_t opaqueAlpha = 255;
    opaqueAlphaTexture->upload(*vkCtx, &opaqueAlpha, 1, 1, VK_FORMAT_R8_UNORM, false);
    opaqueAlphaTexture->createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                       VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    textureCacheBudgetBytes_ =
        envSizeMBOrDefault("WOWEE_TERRAIN_TEX_CACHE_MB", 4096) * 1024ull * 1024ull;

    // The distant mountains. Without them the far clip is a hard edge of sky.
    // WOWEE_TERRAIN_HORIZON=0 leaves them out, as the stock showLowDetail off.
    if (core::envFlagEnabled("WOWEE_TERRAIN_HORIZON", true)) {
        horizon_ = std::make_unique<TerrainHorizon>();
        if (!horizon_->initialize(vkCtx, perFrameLayout)) {
            LOG_WARNING("TerrainRenderer: no horizon pipeline");
            horizon_->shutdown();
            horizon_.reset();
        }
    }
    LOG_INFO("Terrain texture cache budget: ", textureCacheBudgetBytes_ / (1024 * 1024), " MB");

    // Allocate mega vertex/index buffers and indirect draw buffer.
    // All terrain chunks share these buffers, eliminating per-chunk VB/IB rebinds.
    {
        VmaAllocator allocator = vkCtx->getAllocator();

        // How many tile slots. With placement on, enough for every tile the
        // view can reach at its longest (2400 yards: ~85 tiles) with room
        // for the shadow reach, so the nearest tiles fit whatever the
        // setting; placement then keeps the slots on them. Off, the buffers
        // stay the size they were.
        if (kTerrainMegaSlots && (kTerrainSlotMigrate || std::getenv("WOWEE_TERRAIN_MEGA_TILE_SLOTS"))) {
            const auto slots = static_cast<uint32_t>(
                core::envIntClamped("WOWEE_TERRAIN_MEGA_TILE_SLOTS", 128, 8, 512));
            megaVBMaxVerts_ = slots * kMegaTileSlotVerts;
            megaIBMaxIndices_ = slots * kMegaTileSlotIndices;
        } else {
            megaVBMaxVerts_ = MEGA_VB_MAX_VERTS;
            megaIBMaxIndices_ = MEGA_IB_MAX_INDICES;
        }

        // Mega vertex buffer (host-visible for direct write during chunk upload)
        VkBufferCreateInfo vbCI{};
        vbCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        vbCI.size = static_cast<VkDeviceSize>(megaVBMaxVerts_) * sizeof(pipeline::TerrainVertex);
        vbCI.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo vbAllocCI{};
        vbAllocCI.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        vbAllocCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo vbInfo{};
        if (vmaCreateBuffer(allocator, &vbCI, &vbAllocCI,
                &megaVB_, &megaVBAlloc_, &vbInfo) == VK_SUCCESS) {
            megaVBMapped_ = vbInfo.pMappedData;
        } else {
            LOG_WARNING("TerrainRenderer: mega VB allocation failed, per-chunk fallback");
        }

        // Mega index buffer
        VkBufferCreateInfo ibCI{};
        ibCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ibCI.size = static_cast<VkDeviceSize>(megaIBMaxIndices_) * sizeof(uint32_t);
        ibCI.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        VmaAllocationCreateInfo ibAllocCI{};
        ibAllocCI.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        ibAllocCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo ibInfo{};
        if (vmaCreateBuffer(allocator, &ibCI, &ibAllocCI,
                &megaIB_, &megaIBAlloc_, &ibInfo) == VK_SUCCESS) {
            megaIBMapped_ = ibInfo.pMappedData;
        } else {
            LOG_WARNING("TerrainRenderer: mega IB allocation failed, per-chunk fallback");
        }

        // Indirect draw command buffer
        VkBufferCreateInfo indCI{};
        indCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        indCI.size = MAX_INDIRECT_DRAWS * sizeof(VkDrawIndexedIndirectCommand);
        indCI.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        VmaAllocationCreateInfo indAllocCI{};
        indAllocCI.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        indAllocCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo indInfo{};
        if (vmaCreateBuffer(allocator, &indCI, &indAllocCI,
                &indirectBuffer_, &indirectAlloc_, &indInfo) == VK_SUCCESS) {
        } else {
            LOG_WARNING("TerrainRenderer: indirect buffer allocation failed");
        }

        megaSlotPool_ = std::make_shared<MegaSlotPool>();
        megaSlotPool_->slots = LowestFreeSlots(std::min(megaVBMaxVerts_ / kMegaTileSlotVerts,
                                                        megaIBMaxIndices_ / kMegaTileSlotIndices));
        // Where the buffers landed matters as much as their size: without
        // resizable BAR the host-visible VRAM is 256 MB, and a buffer that
        // does not fit there goes to system memory, which the GPU reads
        // across the bus every frame.
        const auto deviceLocal = [allocator](VmaAllocation alloc) {
            if (!alloc) return "none";
            VkMemoryPropertyFlags flags = 0;
            vmaGetAllocationMemoryProperties(allocator, alloc, &flags);
            return (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "VRAM" : "system memory";
        };
        LOG_WARNING("Terrain mega buffers: VB=", vbCI.size / (1024*1024), "MB (", deviceLocal(megaVBAlloc_),
                    ") IB=", ibCI.size / (1024*1024), "MB (", deviceLocal(megaIBAlloc_), ") indirect=",
                    indCI.size / 1024, "KB, ", megaSlotPool_->slots.capacity(), " tile slots, placement ",
                    (kTerrainSlotMigrate && kTerrainMegaSlots) ? "on" : "off");
    }

    LOG_INFO("Terrain renderer initialized (Vulkan)");
    return true;
}

void TerrainRenderer::recreatePipelines() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();

    // Destroy old pipelines (keep layouts)
    destroy(device, pipeline);
    destroy(device, wireframePipeline);
    const bool hadFade = fadePipeline_ != VK_NULL_HANDLE;
    destroy(device, fadePipeline_);
    destroy(device, cliffPipeline_);
    destroy(device, cliffFadePipeline_);

    // Load shaders
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/terrain.vert.spv")) {
        LOG_ERROR("TerrainRenderer::recreatePipelines: failed to load vertex shader");
        return;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/terrain.frag.spv")) {
        LOG_ERROR("TerrainRenderer::recreatePipelines: failed to load fragment shader");
        vertShader.destroy();
        return;
    }

    buildMainPassPipelines(device, vertShader, fragShader);

    vertShader.destroy();
    fragShader.destroy();
    if (hadFade) (void)buildFadePipeline();
    if (cliffOn_) buildCliffPipelines();
    if (horizon_) horizon_->recreatePipeline();
}

VkPipeline TerrainRenderer::buildFillVariant(const char* fragPath) {
    if (!vkCtx || !pipelineLayout) return VK_NULL_HANDLE;
    VkDevice device = vkCtx->getDevice();
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/terrain.vert.spv") ||
        !fragShader.loadFromFile(device, fragPath)) {
        LOG_WARNING("TerrainRenderer: ", fragPath, " missing - drawn by the stock pipeline");
        vertShader.destroy();
        fragShader.destroy();
        return VK_NULL_HANDLE;
    }
    // The fill pipeline's state exactly, with another build of the fragment
    // shader (buildMainPassPipelines).
    VkPipeline p = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({ perVertexBinding(sizeof(pipeline::TerrainVertex)) },
                        toVkAttributes(kTerrainVertexAttributes))
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(vkCtx->getMsaaSamples())
        .setLayout(pipelineLayout)
        .setRenderPass(vkCtx->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx->getPipelineCache());
    vertShader.destroy();
    fragShader.destroy();
    return p;
}

bool TerrainRenderer::buildFadePipeline() {
    if (!vkCtx || !pipelineLayout) return false;
    if (!fadePipeline_) fadePipeline_ = buildFillVariant("assets/shaders/terrain_fade.frag.spv");
    if (cliffOn_ && !cliffFadePipeline_) {
        cliffFadePipeline_ = buildFillVariant("assets/shaders/terrain_cliff_fade.frag.spv");
    }
    return fadePipeline_ != VK_NULL_HANDLE;
}

void TerrainRenderer::buildCliffPipelines() {
    if (!cliffPipeline_) cliffPipeline_ = buildFillVariant("assets/shaders/terrain_cliff.frag.spv");
    if (streamFadeOn_ && !cliffFadePipeline_) {
        cliffFadePipeline_ = buildFillVariant("assets/shaders/terrain_cliff_fade.frag.spv");
    }
}

void TerrainRenderer::setCliffProjection(bool on) {
    if (on == cliffOn_) return;
    cliffOn_ = on;
    if (on) {
        buildCliffPipelines();
        return;
    }
    // Off: the stock pipelines from the next frame, and these go once no
    // frame in flight can still be drawing with them.
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    for (VkPipeline* pp : {&cliffPipeline_, &cliffFadePipeline_}) {
        VkPipeline p = *pp;
        *pp = VK_NULL_HANDLE;
        if (p) vkCtx->deferAfterAllFrameFences([device, p]() { vkDestroyPipeline(device, p, nullptr); });
    }
}

void TerrainRenderer::setStreamFadeIn(bool on) {
    if (on == streamFadeOn_) return;
    streamFadeOn_ = on;
    if (on) {
        (void)buildFadePipeline();
        return;
    }
    // Off: nothing stays stamped, and the pipeline goes once no frame in
    // flight can still be drawing with it.
    for (auto& c : chunks) c.streamFadeStart = -1.0f;
    if (vkCtx) {
        VkDevice device = vkCtx->getDevice();
        for (VkPipeline* pp : {&fadePipeline_, &cliffFadePipeline_}) {
            VkPipeline p = *pp;
            *pp = VK_NULL_HANDLE;
            if (p) vkCtx->deferAfterAllFrameFences([device, p]() { vkDestroyPipeline(device, p, nullptr); });
        }
    }
}

void TerrainRenderer::setHorizonTiles(const std::vector<stock_terrain::WdlTile>& tiles) {
    if (horizon_) horizon_->setTiles(tiles);
}

void TerrainRenderer::setHorizonWmos(horizon_detail::FarMesh mesh) {
    if (horizon_) horizon_->setFarMesh(std::move(mesh));
}

void TerrainRenderer::setHorizonLit(bool lit) {
    if (horizon_) horizon_->setLit(lit);
}

void TerrainRenderer::shutdown() {
    LOG_INFO("Shutting down terrain renderer");

    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    VmaAllocator allocator = vkCtx->getAllocator();

    vkDeviceWaitIdle(device);

    if (horizon_) {
        horizon_->shutdown();
        horizon_.reset();
    }
    clear();
    // clear() defers chunk destruction, and no further frames will run to drain
    // the queue - so run it now, while the descriptor pools those lambdas free
    // sets from are still alive. Without this every resident chunk's vertex and
    // index buffers outlived the device: twenty thousand of each.
    vkCtx->flushDeferredCleanup();

    for (auto& [path, entry] : textureCache) {
        if (entry.texture) entry.texture->destroy(device, allocator);
    }
    textureCache.clear();
    alphaTextureKeys_.clear();
    textureCacheBytes_ = 0;
    textureCacheCounter_ = 0;
    failedTextureCache_.clear();
    loggedTextureLoadFails_.clear();

    if (whiteTexture) { whiteTexture->destroy(device, allocator); whiteTexture.reset(); }
    if (opaqueAlphaTexture) { opaqueAlphaTexture->destroy(device, allocator); opaqueAlphaTexture.reset(); }

    destroy(device, pipeline);
    destroy(device, wireframePipeline);
    destroy(device, fadePipeline_);
    destroy(device, cliffPipeline_);
    destroy(device, cliffFadePipeline_);
    destroy(device, pipelineLayout);
    destroy(device, materialDescPool);
    destroy(device, materialSetLayout);

    // Shadow pipeline cleanup
    destroy(device, shadowPipeline_);
    destroy(device, shadowPipelineLayout_);
    destroyShadowParamsSet(device, allocator, shadowParams_);

    // Destroy mega buffers and indirect draw buffer
    if (megaVB_) { vmaDestroyBuffer(allocator, megaVB_, megaVBAlloc_); megaVB_ = VK_NULL_HANDLE; megaVBAlloc_ = VK_NULL_HANDLE; megaVBMapped_ = nullptr; }
    if (megaIB_) { vmaDestroyBuffer(allocator, megaIB_, megaIBAlloc_); megaIB_ = VK_NULL_HANDLE; megaIBAlloc_ = VK_NULL_HANDLE; megaIBMapped_ = nullptr; }
    if (indirectBuffer_) { vmaDestroyBuffer(allocator, indirectBuffer_, indirectAlloc_); indirectBuffer_ = VK_NULL_HANDLE; indirectAlloc_ = VK_NULL_HANDLE; }
    megaVBUsed_ = 0;
    megaIBUsed_ = 0;
    // The deferred releases ran in the flush above; nothing is held now.
    megaTileSlots_.clear();
    if (megaSlotPool_) {
        std::lock_guard<std::mutex> lock(megaSlotPool_->mutex);
        megaSlotPool_->slots.releaseAll();
        megaSlotPool_->releasing = 0;
    }
    slotTiles_.reset();
    slotTilesFallback_.clear();
    slotRefused_.clear();

    vkCtx = nullptr;
}

bool TerrainRenderer::loadTerrain(const pipeline::TerrainMesh& mesh,
                                   const std::vector<std::string>& texturePaths,
                                   int tileX, int tileY) {
    if (mesh.validChunkCount == 0) {
        LOG_WARNING("loadTerrain[", tileX, ",", tileY, "]: mesh has 0 valid chunks (", texturePaths.size(), " textures)");
        return false;
    }
    LOG_DEBUG("Loading terrain mesh: ", mesh.validChunkCount, " chunks");

    evictProtectFrom_ = textureCacheCounter_ + 1;
    vkCtx->beginUploadBatch();

    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            const auto& chunk = mesh.getChunk(x, y);
            if (!chunk.isValid()) continue;

            TerrainChunkGPU gpuChunk = uploadChunk(chunk, tileX, tileY);
            if (!gpuChunk.isValid()) {
                LOG_WARNING("Failed to upload chunk [", x, ",", y, "]");
                continue;
            }

            calculateBoundingSphere(gpuChunk, chunk);

            // Load textures for this chunk
            bindChunkTextures(gpuChunk, chunk, mesh, texturePaths, tileX, tileY, x, y);

            gpuChunk.tileX = tileX;
            gpuChunk.tileY = tileY;
            gpuChunk.meshChunk = static_cast<int16_t>(y * 16 + x);

            // Create per-chunk params UBO
            // A failed allocation here is pressure rather than corruption, but
            // this path has no way to come back for the chunk, so it skips it.
            if (!createChunkParamsUBO(gpuChunk)) {
                LOG_WARNING("Terrain chunk UBO allocation failed - skipping chunk");
                destroyChunkGPU(gpuChunk);
                continue;
            }

            gpuChunk.materialSet = allocateMaterialSet();
            if (!gpuChunk.materialSet) {
                destroyChunkGPU(gpuChunk);
                continue;
            }
            if (!writeMaterialDescriptors(gpuChunk.materialSet, gpuChunk)) {
                destroyChunkGPU(gpuChunk);
                continue;
            }

            registerRtChunk(gpuChunk, chunk);
            if (streamFadeOn_) gpuChunk.streamFadeStart = streamFadeClock_;
            chunks.push_back(std::move(gpuChunk));
            ++chunksGeneration_;
        }
    }

    vkCtx->endUploadBatch();

    LOG_DEBUG("Loaded ", chunks.size(), " terrain chunks to GPU");
    return !chunks.empty();
}

bool TerrainRenderer::loadTerrainIncremental(const pipeline::TerrainMesh& mesh,
                                              const std::vector<std::string>& texturePaths,
                                              int tileX, int tileY,
                                              int& chunkIndex, int maxChunksPerCall) {
    // Batch all GPU uploads (VBs, IBs, textures) into a single command buffer
    // submission with one fence wait, instead of one per buffer/texture.
    // Nothing this call is handed can be evicted under it: the chunk being
    // built is not in `chunks` yet, so the in-use scan cannot see its textures.
    evictProtectFrom_ = textureCacheCounter_ + 1;

    vkCtx->beginUploadBatch();

    // Where a slow call's time went, said when it gets worse (see
    // TerrainManager's "Terrain finalize" lines).
    using Clock = std::chrono::steady_clock;
    double msUpload = 0.0, msTextures = 0.0, msRest = 0.0;
    const auto callStart = Clock::now();
    const auto since = [](Clock::time_point t) {
        return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    };

    int uploaded = 0;
    while (chunkIndex < 256 && uploaded < maxChunksPerCall) {
        int cy = chunkIndex / 16;
        int cx = chunkIndex % 16;
        chunkIndex++;

        const auto& chunk = mesh.getChunk(cx, cy);
        if (!chunk.isValid()) continue;

        auto t0 = Clock::now();
        TerrainChunkGPU gpuChunk = uploadChunk(chunk, tileX, tileY);
        msUpload += since(t0);
        if (!gpuChunk.isValid()) continue;

        calculateBoundingSphere(gpuChunk, chunk);

        t0 = Clock::now();
        bindChunkTextures(gpuChunk, chunk, mesh, texturePaths, tileX, tileY, cx, cy);
        msTextures += since(t0);
        t0 = Clock::now();

        gpuChunk.tileX = tileX;
        gpuChunk.tileY = tileY;
        gpuChunk.meshChunk = static_cast<int16_t>(cy * 16 + cx);

        if (!createChunkParamsUBO(gpuChunk)) {
            LOG_WARNING("Terrain[", tileX, ",", tileY, "] chunk UBO allocation failed"
                        " - retrying next frame");
            destroyChunkGPU(gpuChunk);
            chunkIndex--;
            break;
        }

        gpuChunk.materialSet = allocateMaterialSet();
        if (!gpuChunk.materialSet) {
            destroyChunkGPU(gpuChunk);
            // Give the chunk back rather than dropping it. Both failures above
            // are pressure, not corruption: the descriptor pool is shared by
            // every resident tile and its sets come back only through
            // deferAfterAllFrameFences, so a tile arriving while an unloaded
            // one is still in flight can find the pool momentarily full. A
            // dropped chunk was never retried - the tile counted as loaded
            // with a hole in it, and the hole stayed for the rest of the
            // session. Stepping the index back and leaving means the caller
            // sees "not finished" and comes back once the frees have landed.
            chunkIndex--;
            break;
        }
        if (!writeMaterialDescriptors(gpuChunk.materialSet, gpuChunk)) {
            // Not the retry above: that one is descriptor-pool pressure, which
            // passes. This is the fallback textures being unsampleable, which
            // does not, so the chunk is dropped rather than asked for again.
            destroyChunkGPU(gpuChunk);
            continue;
        }

        registerRtChunk(gpuChunk, chunk);
        if (streamFadeOn_) gpuChunk.streamFadeStart = streamFadeClock_;
        chunks.push_back(std::move(gpuChunk));
        ++chunksGeneration_;
        uploaded++;
        msRest += since(t0);
    }

    vkCtx->endUploadBatch();
    if (const double total = since(callStart); total > 6.0) {
        static double worst = 0.0;
        if (total > worst) {
            worst = total;
            LOG_WARNING("Terrain chunks [", tileX, ",", tileY, "] ", uploaded, " in ", total,
                        "ms: buffers ", msUpload, ", textures ", msTextures,
                        ", UBO+descriptors+rt ", msRest, " - said again only if it gets worse");
        }
    }

    return chunkIndex >= 256;
}

void TerrainRenderer::bindChunkTextures(TerrainChunkGPU& gpuChunk,
                                        const pipeline::ChunkMesh& chunk,
                                        const pipeline::TerrainMesh& mesh,
                                        const std::vector<std::string>& texturePaths,
                                        int tileX, int tileY, int chunkX, int chunkY) {
    gpuChunk.bigAlpha = mesh.bigAlpha;
    // The `_s` tileset where the manager found one (stock_terrain): the same
    // colours, and the highlight mask in its alpha.
    const auto layerTexture = [&](uint32_t texId, int32_t& flags) -> VkTexture* {
        if (texId < mesh.specularTextures.size() && !mesh.specularTextures[texId].empty()) {
            VkTexture* t = loadTexture(mesh.specularTextures[texId]);
            if (t != whiteTexture.get()) {
                flags |= kSpecularMaskBit;
                return t;
            }
        }
        return loadTexture(texturePaths[texId]);
    };
    // The baked shadow rides the alpha-map path: the same 64x64 R8 texture,
    // the same cache and eviction. No map = the opaque fallback = lit.
    gpuChunk.shadowTexture = chunk.shadowLight.empty() ? nullptr
                                                       : createAlphaTexture(chunk.shadowLight);
    if (chunk.layers.empty()) {
        gpuChunk.baseTexture = whiteTexture.get();
        return;
    }

    uint32_t baseTexId = chunk.layers[0].textureId;
    gpuChunk.layerFlags[0] = static_cast<int32_t>(chunk.layers[0].flags & 0xffff);
    if (baseTexId < texturePaths.size()) {
        gpuChunk.baseTexture = layerTexture(baseTexId, gpuChunk.layerFlags[0]);
    } else {
        LOG_WARNING("Terrain[", tileX, ",", tileY, "] chunk[", chunkX, ",", chunkY,
                    "] base textureId ", baseTexId, " >= texturePaths size ",
                    texturePaths.size(), " - white fallback");
        gpuChunk.baseTexture = whiteTexture.get();
    }

    // Layer 0 is the base, so the three blended layers are 1..3.
    for (size_t i = 1; i < chunk.layers.size() && i < 4; i++) {
        const auto& layer = chunk.layers[i];
        int li = static_cast<int>(i) - 1;

        VkTexture* layerTex = whiteTexture.get();
        gpuChunk.layerFlags[i] = static_cast<int32_t>(layer.flags & 0xffff);
        if (layer.textureId < texturePaths.size()) {
            layerTex = layerTexture(layer.textureId, gpuChunk.layerFlags[i]);
        } else {
            LOG_WARNING("Terrain[", tileX, ",", tileY, "] chunk[", chunkX, ",", chunkY,
                        "] layer[", i, "] textureId ", layer.textureId,
                        " >= texturePaths size ", texturePaths.size(),
                        " - white fallback");
        }
        gpuChunk.layerTextures[li] = layerTex;

        // A layer with no alpha map covers everything under it.
        VkTexture* alphaTex = opaqueAlphaTexture.get();
        if (!layer.alphaData.empty()) {
            alphaTex = createAlphaTexture(layer.alphaData);
        }
        gpuChunk.alphaTextures[li] = alphaTex;
        gpuChunk.layerCount = static_cast<int>(i);
    }
}

bool TerrainRenderer::createChunkParamsUBO(TerrainChunkGPU& gpuChunk) {
    TerrainParamsUBO params{};
    params.layerCount = gpuChunk.layerCount;
    params.hasLayer1 = gpuChunk.layerCount >= 1 ? 1 : 0;
    params.hasLayer2 = gpuChunk.layerCount >= 2 ? 1 : 0;
    params.hasLayer3 = gpuChunk.layerCount >= 3 ? 1 : 0;
    int32_t anyUnlit = 0, anyScroll = 0;
    for (int i = 0; i < 4; ++i) {
        params.layerFlags[i] = gpuChunk.layerFlags[i];
        if (i > gpuChunk.layerCount) continue;
        if (gpuChunk.layerFlags[i] & static_cast<int32_t>(stock_terrain::kLayerUnlit)) anyUnlit = 1;
        if (gpuChunk.layerFlags[i] & static_cast<int32_t>(stock_terrain::kLayerAnimate)) anyScroll = 1;
    }
    params.chunkMode[0] = gpuChunk.bigAlpha ? 1 : 0;
    params.chunkMode[1] = anyUnlit;
    params.chunkMode[2] = anyScroll;
    params.chunkMode[3] = 0;

    VkBufferCreateInfo bufCI{};
    bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCI.size = sizeof(TerrainParamsUBO);
    bufCI.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    allocCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo mapInfo{};
    // Check the return value - a null UBO handle would leave the GPU reading
    // from an invalid descriptor, crashing the driver under memory pressure
    // rather than losing one chunk.
    if (vmaCreateBuffer(vkCtx->getAllocator(), &bufCI, &allocCI,
                        &gpuChunk.paramsUBO, &gpuChunk.paramsAlloc, &mapInfo) != VK_SUCCESS) {
        return false;
    }
    if (mapInfo.pMappedData) {
        std::memcpy(mapInfo.pMappedData, &params, sizeof(params));
    }
    return true;
}

TerrainChunkGPU TerrainRenderer::uploadChunk(const pipeline::ChunkMesh& chunk, int tileX, int tileY) {
    TerrainChunkGPU gpuChunk;

    gpuChunk.worldX = chunk.worldX;
    gpuChunk.worldY = chunk.worldY;
    gpuChunk.worldZ = chunk.worldZ;
    gpuChunk.indexCount = static_cast<uint32_t>(chunk.indices.size());
    gpuChunk.vertexCount = static_cast<uint32_t>(chunk.vertices.size());
    // Named now, not only by the caller after the upload, so a chunk dropped
    // on the way finds its mega slot (destroyChunkGPU).
    gpuChunk.tileX = tileX;
    gpuChunk.tileY = tileY;

    VkDeviceSize vbSize = chunk.vertices.size() * sizeof(pipeline::TerrainVertex);
    AllocatedBuffer vb = uploadBuffer(*vkCtx, chunk.vertices.data(), vbSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    gpuChunk.vertexBuffer = vb.buffer;
    gpuChunk.vertexAlloc = vb.allocation;

    VkDeviceSize ibSize = chunk.indices.size() * sizeof(pipeline::TerrainIndex);
    AllocatedBuffer ib = uploadBuffer(*vkCtx, chunk.indices.data(), ibSize,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    gpuChunk.indexBuffer = ib.buffer;
    gpuChunk.indexAlloc = ib.allocation;

    // Also copy into mega buffers for indirect drawing
    uint32_t vertCount = static_cast<uint32_t>(chunk.vertices.size());
    uint32_t idxCount = static_cast<uint32_t>(chunk.indices.size());
    // Where in the mega buffers the chunk goes, if anywhere. With slots, the
    // tile's own slot, chunks packed in arrival order, so a tile's chunks
    // sit end to end whatever other tiles load in between; without, the end
    // of everything ever loaded.
    bool haveMegaPlace = false;
    uint32_t megaVertex = 0;
    uint32_t megaIndex = 0;
    if (!megaVBMapped_ || !megaIBMapped_) {
        // Per-chunk buffers only.
    } else if (kTerrainMegaSlots && megaSlotPool_) {
        const uint64_t key = terrainTileKey(tileX, tileY);
        auto it = megaTileSlots_.find(key);
        if (it == megaTileSlots_.end()) {
            int64_t slot = -1;
            {
                // Not the slot placement is keeping for a nearer tile: freed
                // by an eviction, it would otherwise go to whichever tile
                // uploads first, which on a trip is one at the far edge.
                std::lock_guard<std::mutex> lock(megaSlotPool_->mutex);
                if (megaSlotPool_->slots.freeCount() > megaSlotsReserved_)
                    slot = megaSlotPool_->slots.acquire();
            }
            if (slot >= 0) {
                it = megaTileSlots_.emplace(key, MegaTileSlot{.slot = static_cast<uint32_t>(slot)}).first;
            }
        }
        if (it != megaTileSlots_.end() &&
            it->second.vertsUsed + vertCount <= kMegaTileSlotVerts &&
            it->second.indicesUsed + idxCount <= kMegaTileSlotIndices) {
            megaVertex = it->second.slot * kMegaTileSlotVerts + it->second.vertsUsed;
            megaIndex = it->second.slot * kMegaTileSlotIndices + it->second.indicesUsed;
            it->second.vertsUsed += vertCount;
            it->second.indicesUsed += idxCount;
            haveMegaPlace = true;
        }
    } else if (megaVBUsed_ + vertCount <= megaVBMaxVerts_ &&
               megaIBUsed_ + idxCount <= megaIBMaxIndices_) {
        megaVertex = megaVBUsed_;
        megaIndex = megaIBUsed_;
        megaVBUsed_ += vertCount;
        megaIBUsed_ += idxCount;
        haveMegaPlace = true;
    }
    if (haveMegaPlace) {
        writeMegaChunk(chunk, megaVertex, megaIndex);
        gpuChunk.megaBaseVertex = static_cast<int32_t>(megaVertex);
        gpuChunk.megaFirstIndex = megaIndex;
    }

    return gpuChunk;
}

void TerrainRenderer::writeMegaChunk(const pipeline::ChunkMesh& chunk, uint32_t megaVertex,
                                     uint32_t megaIndex) {
    const auto vertCount = static_cast<uint32_t>(chunk.vertices.size());
    const auto idxCount = static_cast<uint32_t>(chunk.indices.size());
    // Copy vertices
    auto* vbDst = static_cast<pipeline::TerrainVertex*>(megaVBMapped_) + megaVertex;
    std::memcpy(vbDst, chunk.vertices.data(), vertCount * sizeof(pipeline::TerrainVertex));
    // Copy indices. Offset to the chunk's place in the vertex buffer when
    // the shadow pass merges draws: every mega chunk is then drawn with a
    // vertexOffset of 0, and chunks whose indices follow one another are
    // one draw. The vertex each index names is the same either way.
    auto* ibDst = static_cast<uint32_t*>(megaIBMapped_) + megaIndex;
    if (kTerrainShadowMerge) {
        for (uint32_t i = 0; i < idxCount; ++i) ibDst[i] = chunk.indices[i] + megaVertex;
    } else {
        std::memcpy(ibDst, chunk.indices.data(), idxCount * sizeof(uint32_t));
    }
}

int32_t TerrainRenderer::megaVertexOffset(const TerrainChunkGPU& chunk) const {
    return kTerrainShadowMerge ? 0 : chunk.megaBaseVertex;
}

void TerrainRenderer::releaseMegaTileSlot(uint64_t key) {
    auto it = megaTileSlots_.find(key);
    if (it == megaTileSlots_.end()) return;
    const uint32_t slot = it->second.slot;
    megaTileSlots_.erase(it);
    if (!megaSlotPool_ || !vkCtx) return;
    // Frames in flight may still draw from the slot; it is written again
    // only once the last of them has finished.
    std::shared_ptr<MegaSlotPool> pool = megaSlotPool_;
    {
        std::lock_guard<std::mutex> lock(pool->mutex);
        ++pool->releasing;
    }
    vkCtx->deferAfterAllFrameFences([pool, slot]() {
        std::lock_guard<std::mutex> lock(pool->mutex);
        pool->slots.release(slot);
        if (pool->releasing > 0) --pool->releasing;
    });
}

uint32_t TerrainRenderer::countFallbackChunks(const TileChunkGroup& tile) const {
    uint32_t n = 0;
    for (uint32_t ci : tile.chunkIndices) {
        if (ci < chunks.size() && chunks[ci].megaBaseVertex < 0) ++n;
    }
    return n;
}

bool TerrainRenderer::promoteTileToMegaSlot(uint32_t groupSlot, const pipeline::TerrainMesh& mesh) {
    if (groupSlot >= slotTiles_.tiles.size() || !megaSlotPool_) return false;
    const TileChunkGroup& tile = slotTiles_.tiles[groupSlot];
    if (tile.chunkIndices.empty()) return false;
    const TerrainChunkGPU& first = chunks[tile.chunkIndices.front()];
    const uint64_t key = terrainTileKey(first.tileX, first.tileY);

    // Everything is checked before a slot is taken: each chunk must be the
    // one the mesh holds, the same size as what went into its own buffers,
    // and the tile must fit. A tile that fails is not offered again.
    uint32_t verts = 0;
    uint32_t indices = 0;
    for (uint32_t ci : tile.chunkIndices) {
        const TerrainChunkGPU& c = chunks[ci];
        if (c.meshChunk < 0 || c.meshChunk >= static_cast<int>(mesh.chunks.size()) ||
            mesh.chunks[c.meshChunk].vertices.size() != c.vertexCount ||
            mesh.chunks[c.meshChunk].indices.size() != c.indexCount) {
            slotRefused_.insert(key);
            return false;
        }
        verts += c.vertexCount;
        indices += c.indexCount;
    }
    if (verts > kMegaTileSlotVerts || indices > kMegaTileSlotIndices) {
        slotRefused_.insert(key);
        return false;
    }

    int64_t slot = -1;
    {
        std::lock_guard<std::mutex> lock(megaSlotPool_->mutex);
        slot = megaSlotPool_->slots.acquire();
    }
    if (slot < 0) return false;

    // A slot the tile held already (some chunks in it, some not) goes back
    // through the fences, since frames in flight may draw from it; the
    // tile is written whole into the new one. That one came back through
    // the fences itself, so nothing in flight reads it. The chunks are
    // packed in `chunks` order, as uploadChunk packs them, so the shadow
    // pass merges them the same way.
    releaseMegaTileSlot(key);
    MegaTileSlot placed{.slot = static_cast<uint32_t>(slot)};
    for (uint32_t ci : tile.chunkIndices) {
        TerrainChunkGPU& c = chunks[ci];
        const uint32_t megaVertex = placed.slot * kMegaTileSlotVerts + placed.vertsUsed;
        const uint32_t megaIndex = placed.slot * kMegaTileSlotIndices + placed.indicesUsed;
        writeMegaChunk(mesh.chunks[c.meshChunk], megaVertex, megaIndex);
        c.megaBaseVertex = static_cast<int32_t>(megaVertex);
        c.megaFirstIndex = megaIndex;
        placed.vertsUsed += c.vertexCount;
        placed.indicesUsed += c.indexCount;
    }
    megaTileSlots_.emplace(key, placed);
    slotTilesFallback_[groupSlot] = 0;
    shadowDrawsStale_ = true;
    return true;
}

void TerrainRenderer::evictTileFromMegaSlot(uint32_t groupSlot) {
    if (groupSlot >= slotTiles_.tiles.size()) return;
    const TileChunkGroup& tile = slotTiles_.tiles[groupSlot];
    if (tile.chunkIndices.empty()) return;
    // Every chunk kept its own buffers, so it draws from them from the next
    // recording on; the slot is written again only after this frame and
    // those before it have finished with it.
    for (uint32_t ci : tile.chunkIndices) chunks[ci].megaBaseVertex = -1;
    const TerrainChunkGPU& first = chunks[tile.chunkIndices.front()];
    releaseMegaTileSlot(terrainTileKey(first.tileX, first.tileY));
    slotTilesFallback_[groupSlot] = static_cast<uint32_t>(tile.chunkIndices.size());
    shadowDrawsStale_ = true;
}

void TerrainRenderer::updateMegaSlotPlacement(const glm::vec3& cameraPos, const TileMeshLookup& meshOf) {
    if (!kTerrainMegaSlots || !megaSlotPool_ || !megaVBMapped_ || !megaIBMapped_) return;

    // The grouping, and with it the count of chunks on their own buffers
    // per tile, for the tiles that gained chunks (all of them after a
    // removal). The moves below keep the counts of the tiles they move.
    updateTileGrouping(slotTiles_, &slotTouched_);
    slotTilesFallback_.resize(slotTiles_.tiles.size(), 0);
    for (uint32_t s : slotTouched_) slotTilesFallback_[s] = countFallbackChunks(slotTiles_.tiles[s]);

    // How far the slots should reach: the main pass's view distance, and
    // twice the shadow cull radius - the map is a box reaching toward the
    // sun, so casters lie further out than the radius round the player.
    // The margin is half a tile.
    const float shadowReach = (slotShadowRadius_ > 0.0f && slotShadowRadius_ < 1.0e5f)
                                  ? 2.0f * slotShadowRadius_ : 0.0f;
    const float need = std::max(maxViewDistance_, shadowReach);
    constexpr float kEvictMargin = 533.333f * 0.5f;

    std::vector<TerrainSlotTile> states(slotTiles_.tiles.size());
    uint32_t nearTiles = 0, nearSlotted = 0, nearFallback = 0;
    bool wanted = false;
    for (size_t s = 0; s < slotTiles_.tiles.size(); ++s) {
        const TileChunkGroup& tile = slotTiles_.tiles[s];
        if (tile.chunkIndices.empty()) {
            states[s].distance = std::numeric_limits<float>::infinity();
            continue;
        }
        const TerrainChunkGPU& first = chunks[tile.chunkIndices.front()];
        const uint64_t key = terrainTileKey(first.tileX, first.tileY);
        const float dx = tile.center.x - cameraPos.x;
        const float dy = tile.center.y - cameraPos.y;
        TerrainSlotTile& st = states[s];
        st.distance = std::max(0.0f, std::sqrt(dx * dx + dy * dy) - tile.radius);
        st.hasSlot = megaTileSlots_.count(key) != 0;
        st.hasFallback = slotTilesFallback_[s] > 0;
        if (st.distance <= need) {
            ++nearTiles;
            nearFallback += slotTilesFallback_[s];
            nearSlotted += static_cast<uint32_t>(tile.chunkIndices.size()) - slotTilesFallback_[s];
            // Only asked of the tiles that could be chosen.
            st.promotable = st.hasFallback && !slotRefused_.count(key) && meshOf &&
                            meshOf(first.tileX, first.tileY) != nullptr;
            wanted = wanted || st.promotable;
        }
    }
    // While a near tile waits, one free slot is kept for it (uploadChunk).
    megaSlotsReserved_ = (kTerrainSlotMigrate && wanted) ? 1 : 0;

    uint32_t freeSlots = 0, releasing = 0, capacity = 0;
    {
        std::lock_guard<std::mutex> lock(megaSlotPool_->mutex);
        freeSlots = megaSlotPool_->slots.freeCount();
        releasing = megaSlotPool_->releasing;
        capacity = megaSlotPool_->slots.capacity();
    }

    if (kTerrainSlotMigrate) {
        const TerrainSlotMove move = chooseTerrainSlotMove(states, freeSlots, releasing > 0, need, kEvictMargin);
        if (move.kind != TerrainSlotMove::Kind::None) {
            const auto t0 = std::chrono::steady_clock::now();
            const TerrainChunkGPU& first = chunks[slotTiles_.tiles[move.tile].chunkIndices.front()];
            if (move.kind == TerrainSlotMove::Kind::Promote) {
                const pipeline::TerrainMesh* mesh = meshOf(first.tileX, first.tileY);
                if (mesh && promoteTileToMegaSlot(static_cast<uint32_t>(move.tile), *mesh)) {
                    ++slotMovesIn_;
                    megaSlotsReserved_ = 0;  // asked again next frame
                }
            } else {
                evictTileFromMegaSlot(static_cast<uint32_t>(move.tile));
                ++slotMovesOut_;
            }
            slotLastMoveMs_ = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
        }
    }

    // Said when the near counts change, at most every two seconds, and at
    // once when chunks near the player go onto or off their own buffers.
    const double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const bool fallbackFlipped = (nearFallback == 0) != (slotLoggedNearFallback_ == 0);
    if ((nearTiles != slotLoggedNearTiles_ || nearFallback != slotLoggedNearFallback_) &&
        (fallbackFlipped || now - slotLoggedAt_ >= 2.0)) {
        LOG_WARNING("Terrain mega slots: ", nearTiles, " tiles within ", static_cast<int>(need),
                    " yd, ", nearSlotted, " chunks in slots, ", nearFallback,
                    " on their own buffers; slots ", megaTileSlots_.size(), " of ", capacity,
                    " held, ", freeSlots, " free, ", releasing, " coming back; moved in ", slotMovesIn_,
                    ", out ", slotMovesOut_, " (last move ", slotLastMoveMs_, " ms), placement ",
                    kTerrainSlotMigrate ? "on" : "off");
        slotLoggedNearTiles_ = nearTiles;
        slotLoggedNearFallback_ = nearFallback;
        slotLoggedAt_ = now;
    }
}

namespace {
/// Chunks removed so far (unloads, clears): what can make a texture
/// evictable again (see evictTexturesFor).
uint64_t& chunkRemovals() {
    static uint64_t n = 0;
    return n;
}
}  // namespace

bool TerrainRenderer::evictTexturesFor(size_t needBytes) {
    if (textureCacheBytes_ + needBytes <= textureCacheBudgetBytes_) return true;
    if (!vkCtx) return false;
    // A pass that found nothing to give back finds nothing again until a
    // chunk has gone: only an unload frees a texture from every live chunk.
    static uint64_t fruitlessAtRemovals = ~uint64_t{0};
    if (fruitlessAtRemovals == chunkRemovals() && !core::hitchLegacy()) return false;
    fruitlessAtRemovals = ~uint64_t{0};

    // What the live chunks are holding. These are raw pointers into the cache
    // and each chunk's material descriptor set names the image directly, so
    // freeing one out from under a chunk is a use-after-free on the GPU.
    std::unordered_set<const VkTexture*> inUse;
    inUse.reserve(chunks.size() * 4);
    for (const auto& c : chunks) {
        if (c.baseTexture) inUse.insert(c.baseTexture);
        for (VkTexture* t : c.layerTextures) if (t) inUse.insert(t);
        for (VkTexture* t : c.alphaTextures) if (t) inUse.insert(t);
        if (c.shadowTexture) inUse.insert(c.shadowTexture);
    }

    std::vector<std::pair<uint64_t, std::string>> candidates;
    candidates.reserve(textureCache.size());
    for (const auto& [key, entry] : textureCache) {
        if (entry.lastUse >= evictProtectFrom_) continue;
        if (inUse.count(entry.texture.get()) != 0) continue;
        candidates.emplace_back(entry.lastUse, key);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Down to a sixteenth under the budget, not just under it. Every pass
    // above walks every live chunk and every cached texture (~100k of each in
    // a city) and sorts them, and createAlphaTexture asks for room once per
    // alpha map: freeing only the bytes asked for meant a full cache paid that
    // pass for every alpha map of every chunk streamed in - 35-70 ms for one
    // step of sixteen chunks. With the slack it is paid once per few thousand.
    // Only textures no live chunk holds are given back, oldest first, as before.
    const size_t slack = core::hitchLegacy() ? 0 : textureCacheBudgetBytes_ / 16;
    const size_t target = textureCacheBudgetBytes_ > slack + needBytes
                              ? textureCacheBudgetBytes_ - slack - needBytes : 0;
    std::vector<VkTexture*> retired;
    size_t freedBytes = 0;
    for (const auto& [lastUse, key] : candidates) {
        if (textureCacheBytes_ <= target) break;
        auto it = textureCache.find(key);
        if (it == textureCache.end()) continue;
        textureCacheBytes_ -= it->second.approxBytes;
        freedBytes += it->second.approxBytes;
        alphaTextureKeys_.erase(it->second.texture.get());
        retired.push_back(it->second.texture.release());
        textureCache.erase(it);
    }

    if (retired.empty()) fruitlessAtRemovals = chunkRemovals();
    if (!retired.empty()) {
        VkDevice device = vkCtx->getDevice();
        VmaAllocator allocator = vkCtx->getAllocator();
        vkCtx->deferAfterAllFrameFences([device, allocator, retired]() {
            for (VkTexture* tex : retired) {
                tex->destroy(device, allocator);
                delete tex;
            }
        });
        // The first one at warning, because until now this never happened: the
        // cache only grew, and the ground going white on a distant continent
        // was the first anyone heard of it. After that it is routine.
        static bool saidFirstEviction = false;
        if (!saidFirstEviction) {
            saidFirstEviction = true;
            LOG_WARNING("Terrain texture cache reached its ",
                        textureCacheBudgetBytes_ / (1024 * 1024),
                        " MB budget and is now evicting - freed ", retired.size(),
                        " textures, ", freedBytes / (1024 * 1024), " MB");
        }
        LOG_DEBUG("Terrain texture cache: evicted ", retired.size(), " textures (",
                  freedBytes / (1024 * 1024), " MB), now ",
                  textureCacheBytes_ / (1024 * 1024), " MB of ",
                  textureCacheBudgetBytes_ / (1024 * 1024), " MB");
    }

    return textureCacheBytes_ + needBytes <= textureCacheBudgetBytes_;
}

VkTexture* TerrainRenderer::loadTexture(const std::string& path) {
    auto normalizeKey = [](std::string key) {
        std::replace(key.begin(), key.end(), '/', '\\');
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return key;
    };
    std::string key = normalizeKey(path);

    auto it = textureCache.find(key);
    if (it != textureCache.end()) {
        it->second.lastUse = ++textureCacheCounter_;
        return it->second.texture.get();
    }
    // Terrain tileset textures are sampled and never read back, so they go
    // up as blocks with the file's own mip levels.
    pipeline::BLPImage blp = assetManager->loadTexture(key, true);
    if (!blp.isValid()) {
        // Return white fallback but don't cache the failure - allow retry
        // on next tile load in case the asset becomes available.
        if (loggedTextureLoadFails_.insert(key).second) {
            LOG_WARNING("Failed to load texture: ", path);
        }
        return whiteTexture.get();
    }

    const size_t approxBytes = blp.approxUploadBytes();
    if (!evictTexturesFor(approxBytes)) {
        // Everything in the cache is under a live chunk, so there is nothing
        // to give back. White ground is what this looks like, and it used to
        // be permanent: the cache only ever grew, so the first tile to fill it
        // condemned every tile after it, and the complaint stopped after three
        // lines. Said once per texture now, because it is no longer a state
        // the client cannot leave.
        if (loggedTextureLoadFails_.insert("budget:" + key).second) {
            LOG_WARNING("Terrain texture cache full (", textureCacheBytes_ / (1024 * 1024),
                        " MB / ", textureCacheBudgetBytes_ / (1024 * 1024),
                        " MB) and nothing in it is evictable - white ground for: ", path);
        }
        return whiteTexture.get();
    }

    auto tex = std::make_unique<VkTexture>();
    if (!tex->uploadBLP(*vkCtx, blp)) {
        LOG_WARNING("Failed to upload texture to GPU: ", path);
        return whiteTexture.get();
    }
    tex->createSampler(vkCtx->getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                        VK_SAMPLER_ADDRESS_MODE_REPEAT);

    VkTexture* raw = tex.get();
    TextureCacheEntry e;
    e.texture = std::move(tex);
    e.approxBytes = approxBytes;
    e.lastUse = ++textureCacheCounter_;
    textureCacheBytes_ += e.approxBytes;
    textureCache[key] = std::move(e);

    return raw;
}

void TerrainRenderer::uploadPreloadedTextures(
    const std::unordered_map<std::string, pipeline::BLPImage>& textures) {
    auto normalizeKey = [](std::string key) {
        std::replace(key.begin(), key.end(), '/', '\\');
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return key;
    };
    // Batch all texture uploads into a single command buffer submission
    evictProtectFrom_ = textureCacheCounter_ + 1;
    vkCtx->beginUploadBatch();

    for (const auto& [path, blp] : textures) {
        std::string key = normalizeKey(path);
        if (textureCache.find(key) != textureCache.end()) continue;
        if (!blp.isValid()) continue;

        // This path added to the cache without consulting the budget at all,
        // so the preload for one tile could carry it past the limit and the
        // next loadTexture would find no room.
        if (!evictTexturesFor(blp.approxUploadBytes())) continue;

        auto tex = std::make_unique<VkTexture>();
        if (!tex->uploadBLP(*vkCtx, blp)) continue;
        tex->createSampler(vkCtx->getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                            VK_SAMPLER_ADDRESS_MODE_REPEAT);

        TextureCacheEntry e;
        e.texture = std::move(tex);
        e.approxBytes = blp.approxUploadBytes();
        e.lastUse = ++textureCacheCounter_;
        textureCacheBytes_ += e.approxBytes;
        textureCache[key] = std::move(e);
    }

    vkCtx->endUploadBatch();
}

VkTexture* TerrainRenderer::createAlphaTexture(const std::vector<uint8_t>& alphaData) {
    if (alphaData.empty()) return opaqueAlphaTexture.get();

    std::vector<uint8_t> expanded;
    const uint8_t* src = alphaData.data();
    if (alphaData.size() < 4096) {
        expanded.assign(4096, 255);
        std::copy(alphaData.begin(), alphaData.end(), expanded.begin());
        src = expanded.data();
    }

    auto tex = std::make_unique<VkTexture>();
    if (!tex->upload(*vkCtx, src, 64, 64, VK_FORMAT_R8_UNORM, false)) {
        return opaqueAlphaTexture.get();
    }
    tex->createSampler(vkCtx->getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    VkTexture* raw = tex.get();
    // One per chunk layer, under a key nothing ever looks up again, so these
    // are dead the moment their chunk is unloaded. They are counted against
    // the budget and given back with their chunk (releaseChunkAlphaTextures).
    // Left for the eviction pass instead, they filled the 4 GB cache with about
    // a million dead 64x64 images over a quarter hour of travel - each its own
    // device allocation - and every pass that then made room walked and sorted
    // all of them: 420 ms frames on arrival (regression tour, 2026-11-04).
    //
    // Not refused when the cache is full, unlike a tileset texture: 4KB is
    // never what filled it, and a chunk without its alpha map draws its top
    // layer over everything below.
    evictTexturesFor(64 * 64);
    static uint64_t alphaCounter = 0;
    std::string key = "__alpha_" + std::to_string(++alphaCounter);
    TextureCacheEntry e;
    e.texture = std::move(tex);
    e.approxBytes = 64 * 64;
    e.lastUse = ++textureCacheCounter_;
    textureCacheBytes_ += e.approxBytes;
    alphaTextureKeys_[raw] = key;
    textureCache[key] = std::move(e);

    return raw;
}

VkDescriptorSet TerrainRenderer::allocateMaterialSet() {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = materialDescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &materialSetLayout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(vkCtx->getDevice(), &allocInfo, &set) != VK_SUCCESS) {
        static uint64_t failCount = 0;
        ++failCount;
        if (failCount <= 8 || (failCount % 256) == 0) {
            LOG_WARNING("TerrainRenderer: failed to allocate material descriptor set (count=", failCount, ")");
        }
        return VK_NULL_HANDLE;
    }
    return set;
}

bool TerrainRenderer::writeMaterialDescriptors(VkDescriptorSet set, const TerrainChunkGPU& chunk) {
    VkTexture* white = whiteTexture.get();
    VkTexture* opaque = opaqueAlphaTexture.get();

    // Valid, not merely non-null. descriptorInfo() returns the texture's
    // handles as they are, so one whose upload or view creation failed writes
    // VK_NULL_HANDLE into a live descriptor and declares
    // SHADER_READ_ONLY_OPTIMAL over it. Sampling that is undefined behaviour
    // and reaches an NVIDIA driver as a graphics engine exception and a lost
    // device rather than as anything this client can catch. See #123.
    //
    // A chunk texture failing is ordinary - it is what the cache does under
    // memory pressure - and the 1x1 fallbacks are what that case is for.
    const auto sampleable = [](VkTexture* t) { return t && t->isValid(); };
    const auto pick = [&](VkTexture* wanted, VkTexture* fallback) {
        return sampleable(wanted) ? wanted : fallback;
    };
    if (!sampleable(white) || !sampleable(opaque)) {
        // The fallbacks themselves are gone, so there is nothing safe to write.
        // The caller drops the chunk: a hole in the ground costs a view of one
        // tile, and a null image view costs the device.
        static bool told = false;
        if (!told) {
            told = true;
            LOG_ERROR("TerrainRenderer: the 1x1 fallback textures are not "
                      "sampleable, so no chunk can be drawn");
        }
        return false;
    }

    VkDescriptorImageInfo imageInfos[8];
    imageInfos[0] = pick(chunk.baseTexture, white)->descriptorInfo();
    for (int i = 0; i < 3; i++) {
        imageInfos[1 + i] = pick(chunk.layerTextures[i], white)->descriptorInfo();
        imageInfos[4 + i] = pick(chunk.alphaTextures[i], opaque)->descriptorInfo();
    }
    // Binding 8, the baked shadow: the opaque 255 is "lit".
    imageInfos[7] = pick(chunk.shadowTexture, opaque)->descriptorInfo();

    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = chunk.paramsUBO;
    bufInfo.offset = 0;
    bufInfo.range = sizeof(TerrainParamsUBO);

    VkWriteDescriptorSet writes[9] = {};
    writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[8].dstSet = set;
    writes[8].dstBinding = 8;
    writes[8].descriptorCount = 1;
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[8].pImageInfo = &imageInfos[7];
    for (int i = 0; i < 7; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = static_cast<uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imageInfos[i];
    }
    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = set;
    writes[7].dstBinding = 7;
    writes[7].descriptorCount = 1;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[7].pBufferInfo = &bufInfo;

    vkUpdateDescriptorSets(vkCtx->getDevice(), 9, writes, 0, nullptr);
    return true;
}

namespace {
/// WOWEE_TERRAIN_TILE_CULL=0 tests every loaded chunk again rather than
/// tiles first; see TerrainRenderer::cullForRecording.
const bool kTerrainTileCull = core::envFlagEnabled("WOWEE_TERRAIN_TILE_CULL", true);
/// WOWEE_TERRAIN_ONE_WORKER=1 keeps the main pass in one command buffer on
/// one thread; see TerrainRenderer::recordPartsWanted.
const bool kTerrainOneWorker = core::envFlagEnabled("WOWEE_TERRAIN_ONE_WORKER", false);
/// Below this many chunks the pass is recorded on one thread: the second
/// buffer and the hand-off cost tens of microseconds, which a few hundred
/// draws do not repay.
constexpr size_t kTerrainSplitMinChunks = 512;
}  // namespace

bool TerrainRenderer::updateTileGrouping(TileChunkGrouping& grouping,
                                         std::vector<uint32_t>* touched) const {
    // Regrouped from nothing after a removal, and on every change with the
    // append path off. Otherwise only the chunks added since: a tile load
    // adds 16 chunks a call, and regrouping all 28k loaded ones for each of
    // those calls ran every frame while tiles streamed in, in both passes.
    bool restarted = false;
    if (grouping.removalGeneration != chunksRemovedGeneration_ || grouping.grouped > chunks.size() ||
        (!kTerrainTileAppend && grouping.grouped != chunks.size())) {
        grouping.reset();
        grouping.removalGeneration = chunksRemovedGeneration_;
        restarted = true;
    }
    grouping.append(
        chunks.size(),
        [this](size_t ci) { return terrainTileKey(chunks[ci].tileX, chunks[ci].tileY); },
        [this](uint32_t ci, glm::vec3& centre, float& radius) {
            centre = chunks[ci].boundingSphereCenter;
            radius = chunks[ci].boundingSphereRadius;
        },
        touched);
    return restarted;
}

void TerrainRenderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera) {
    if (!cullForRecording(camera)) return;
    recordPart(cmd, perFrameSet, 0, 1);
}

bool TerrainRenderer::cullForRecording(const Camera& camera) {
    visibleChunks_.clear();
    if (chunks.empty() || !pipeline) {
        static int emptyLog = 0;
        if (++emptyLog <= 3)
            LOG_WARNING("TerrainRenderer::render: chunks=", chunks.size(), " pipeline=", (pipeline != VK_NULL_HANDLE));
        return false;
    }

    // One-time diagnostic: log chunk nearest to camera
    static bool loggedDiag = false;
    if (!loggedDiag && !chunks.empty()) {
        loggedDiag = true;
        glm::vec3 cam = camera.getPosition();
        // Find chunk nearest to camera
        const TerrainChunkGPU* nearest = nullptr;
        float nearestDist = std::numeric_limits<float>::max();
        for (const auto& ch : chunks) {
            float dx = ch.boundingSphereCenter.x - cam.x;
            float dy = ch.boundingSphereCenter.y - cam.y;
            float dz = ch.boundingSphereCenter.z - cam.z;
            float d = dx*dx + dy*dy + dz*dz;
            if (d < nearestDist) { nearestDist = d; nearest = &ch; }
        }
        if (nearest) {
            float d2d = std::sqrt((nearest->boundingSphereCenter.x-cam.x)*(nearest->boundingSphereCenter.x-cam.x) +
                                  (nearest->boundingSphereCenter.y-cam.y)*(nearest->boundingSphereCenter.y-cam.y));
            LOG_INFO("Terrain diag: chunks=", chunks.size(),
                     " cam=(", cam.x, ",", cam.y, ",", cam.z, ")",
                     " nearest_center=(", nearest->boundingSphereCenter.x, ",", nearest->boundingSphereCenter.y, ",", nearest->boundingSphereCenter.z, ")",
                     " dist2d=", d2d, " dist3d=", std::sqrt(nearestDist),
                     " radius=", nearest->boundingSphereRadius,
                     " matSet=", (nearest->materialSet != VK_NULL_HANDLE ? "ok" : "NULL"));
        }
    }

    // WOWEE_TERRAIN_AB_PERIOD=<seconds>: the highlight and the horizon go
    // off and on again every period, said in the log, so one run measures
    // both from the same view.
    static const int kAbPeriod = core::envIntClamped("WOWEE_TERRAIN_AB_PERIOD", 0, 0, 3600);
    stockExtrasOff_ = false;
    if (kAbPeriod > 0) {
        static const auto t0 = std::chrono::steady_clock::now();
        const auto s = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - t0).count();
        stockExtrasOff_ = ((s / kAbPeriod) % 2) == 1;
        static bool lastOff = false;
        if (stockExtrasOff_ != lastOff) {
            lastOff = stockExtrasOff_;
            LOG_WARNING("TERRAIN_AB ", stockExtrasOff_ ? "off" : "on");
        }
    }
    // The horizon's tiles, for the far clip the chunks are cut at.
    if (horizon_) horizon_->cull(camera, stockExtrasOff_ ? 0.0f : maxViewDistance_);

    Frustum frustum;
    if (frustumCullingEnabled) {
        glm::mat4 viewProj = camera.getProjectionMatrix() * camera.getViewMatrix();
        frustum.extractFromMatrix(viewProj);
    }

    glm::vec3 camPos = camera.getPosition();
    const float maxTerrainDistSq = maxViewDistance_ * maxViewDistance_;

    renderedChunks = 0;
    culledChunks = 0;
    furthestDrawnSq_ = 0.0f;

    // The test every chunk is put to, unchanged; it decides what is drawn.
    const auto testChunk = [&](uint32_t ci) {
        const auto& chunk = chunks[ci];
        if (!chunk.isValid() || !chunk.materialSet) return;

        float dx = chunk.boundingSphereCenter.x - camPos.x;
        float dy = chunk.boundingSphereCenter.y - camPos.y;
        float distSq = dx * dx + dy * dy;
        if (distSq > maxTerrainDistSq) {
            culledChunks++;
            return;
        }

        if (frustumCullingEnabled && !isChunkVisible(chunk, frustum)) {
            culledChunks++;
            return;
        }
        visibleChunks_.push_back(ci);
        renderedChunks++;
        if (distSq > furthestDrawnSq_) furthestDrawnSq_ = distSq;
    };

    if (!kTerrainTileCull) {
        for (uint32_t ci = 0; ci < chunks.size(); ++ci) testChunk(ci);
        return true;
    }

    // Tiles first, as the shadow pass does. Every loaded chunk was tested
    // here each frame - 15k to 28k of them, each two cache lines of a
    // ~190-byte struct - to keep a few thousand, and the walk alone was a
    // large share of the terrain worker. A tile's sphere holds all of its
    // chunks' spheres, so a tile beyond the distance (plus its radius, in
    // the plane, which the chunk test also measures in) or wholly behind a
    // frustum plane cannot hold a chunk that passes; the tiles that pass
    // put their chunks through the same test as before. Padded a yard so
    // rounding in the tile's sphere can never drop a chunk on the edge.
    // Culled tiles count all their chunks as culled, unloaded ones included.
    updateTileGrouping(viewTiles_, nullptr);
    constexpr float kTilePad = 1.0f;
    for (const auto& tile : viewTiles_.tiles) {
        const float reach = tile.radius + kTilePad;
        const float dx = tile.center.x - camPos.x;
        const float dy = tile.center.y - camPos.y;
        const float limit = maxViewDistance_ + reach;
        if (dx * dx + dy * dy > limit * limit ||
            (frustumCullingEnabled && !frustum.intersectsSphere(tile.center, reach))) {
            culledChunks += static_cast<int>(tile.chunkIndices.size());
            continue;
        }
        for (uint32_t ci : tile.chunkIndices) {
            if (ci < chunks.size()) testChunk(ci);
        }
    }
    // Drawn in the order of `chunks`, as before. The tiles hold their chunks
    // in that order and normally follow it themselves; when they do not,
    // the few thousand survivors are put back in order.
    if (!std::is_sorted(visibleChunks_.begin(), visibleChunks_.end()))
        std::sort(visibleChunks_.begin(), visibleChunks_.end());
    return true;
}

uint32_t TerrainRenderer::recordPartsWanted() const {
    // Each surviving chunk is a descriptor set bind and a draw, which this
    // driver encodes as they are recorded. The list is cut in two: the first
    // half recorded into a buffer executed ahead of the second, so the draws
    // reach the GPU in the order one buffer would give them.
    if (kTerrainOneWorker || visibleChunks_.size() < kTerrainSplitMinChunks) return 1;
    return 2;
}

void TerrainRenderer::recordPart(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                                 uint32_t part, uint32_t parts) {
    if (!pipeline || part >= parts) return;
    const size_t total = visibleChunks_.size();
    const size_t begin = total * part / parts;
    const size_t end = total * (part + 1) / parts;

    // Unstretched cliffs (E16) on: its build of the fill pipeline for every
    // chunk, the fade twin for those dissolving in.
    // Only for the chunks with a steep slope in them (TerrainChunkGPU::steep);
    // the rest take the stock pipelines, which is what they would draw.
    const bool cliff = cliffOn_ && cliffPipeline_ != VK_NULL_HANDLE && !(wireframe && wireframePipeline);
    VkPipeline fillPipeline = pipeline;
    VkPipeline activePipeline = (wireframe && wireframePipeline) ? wireframePipeline : fillPipeline;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                             0, 1, &perFrameSet, 0, nullptr);

    TerrainPush push{};
    push.model = glm::mat4(1.0f);
    // The stock Terrain Highlights option (CVar `specular`, default 0): the
    // Ground highlights enhancement row. WOWEE_TERRAIN_SPECULAR=0 still
    // forces it off for an A/B.
    static const bool kSpecularAllowed = core::envFlagEnabled("WOWEE_TERRAIN_SPECULAR", true);
    push.specular = glm::vec4(sunSpecular_, kSpecularAllowed && specularEnabled_ && !stockExtrasOff_
                                                ? stock_terrain::kSpecularPower : 0.0f);
    vkCmdPushConstants(cmd, pipelineLayout, kTerrainPushStages,
                        0, sizeof(TerrainPush), &push);

    // Use mega VB + IB when available.
    // Bind mega buffers once, then use direct draws with base vertex/index offsets.
    const bool useMegaBuffers = (megaVB_ && megaIB_);
    bool megaBuffersBound = false;
    if (useMegaBuffers) {
        VkDeviceSize megaOffset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &megaVB_, &megaOffset);
        vkCmdBindIndexBuffer(cmd, megaIB_, 0, VK_INDEX_TYPE_UINT32);
        megaBuffersBound = true;
    }

    // Fade in new scenery: a chunk still dissolving in is drawn with the fade
    // pipeline and its hidden fraction; the rest with the stock one. Neither
    // the wireframe view nor the row off ever binds it.
    const bool fadeChunks = streamFadeOn_ && fadePipeline_ != VK_NULL_HANDLE &&
                            activePipeline == fillPipeline;
    VkPipeline boundPipeline = activePipeline;

    // In the usual order, switching pipeline where a steep chunk follows a
    // flat one: drawing the steep ones in a pass of their own was tried and
    // moved the terrain pass by -1.6 to +1.3 ms with the view (the change of
    // order changes what the depth test rejects), more than the switches.
    for (size_t vi = begin; vi < end; ++vi) {
        const auto& chunk = chunks[visibleChunks_[vi]];

        if (fadeChunks || cliff) {
            const float hide = fadeChunks && chunk.streamFadeStart >= 0.0f
                ? stream_fade::hidden(chunk.streamFadeStart, streamFadeClock_) : 0.0f;
            const bool steep = cliff && chunk.steep;
            VkPipeline want = activePipeline;
            if (hide > 0.0f) {
                want = steep && cliffFadePipeline_ ? cliffFadePipeline_ : fadePipeline_;
            } else if (steep) {
                want = cliffPipeline_;
            }
            if (want != boundPipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
                boundPipeline = want;
            }
            if (hide > 0.0f) {
                const glm::vec4 fade(hide, 0.0f, 0.0f, 0.0f);
                vkCmdPushConstants(cmd, pipelineLayout, kTerrainPushStages,
                                   kTerrainStreamFadeOffset, sizeof(glm::vec4), &fade);
            }
        }

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                 1, 1, &chunk.materialSet, 0, nullptr);

        if (useMegaBuffers && chunk.megaBaseVertex >= 0) {
            // Rebound if a fallback chunk bound its own buffers since. The mega
            // buffers are bound once before the loop, and a chunk without a
            // place in them binds its own - which stays bound for whatever comes
            // next. A chunk after that one then drew its mega offsets against a
            // single chunk's buffer: firstIndex 6,290,784 into 3,072 bytes,
            // which is what took the GPU down.
            if (!megaBuffersBound) {
                VkDeviceSize megaOffset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &megaVB_, &megaOffset);
                vkCmdBindIndexBuffer(cmd, megaIB_, 0, VK_INDEX_TYPE_UINT32);
                megaBuffersBound = true;
            }
            vkCmdDrawIndexed(cmd, chunk.indexCount, 1,
                             chunk.megaFirstIndex, megaVertexOffset(chunk), 0);
        } else {
            // Fallback: per-chunk VB/IB bind + direct draw
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &chunk.vertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, chunk.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, chunk.indexCount, 1, 0, 0, 0);
            megaBuffersBound = false;
        }
    }
    // The horizon after the ground, as 0x795f80 follows the chunks: it fills
    // only what nothing nearer covered. The last part draws it, which is the
    // one executed last.
    if (horizon_ && part + 1 == parts && !stockExtrasOff_) horizon_->record(cmd, perFrameSet);
}

bool TerrainRenderer::initializeShadow(VkRenderPass shadowRenderPass) {
    if (!vkCtx || shadowRenderPass == VK_NULL_HANDLE) return false;
    if (shadowPipeline_ != VK_NULL_HANDLE) return true;  // already initialised
    VkDevice device = vkCtx->getDevice();
    VmaAllocator allocator = vkCtx->getAllocator();

    if (!createShadowParamsSet(device, allocator, sizeof(ShadowParamsUBO),
                               whiteTexture->getImageView(),
                               whiteTexture->getSampler(), "TerrainRenderer",
                               shadowParams_)) {
        return false;
    }

    // Pipeline layout: set 0 = shadowParams_.layout, push 128 bytes (lightSpaceMatrix + model)
    VkPushConstantRange pc{};
    // The fragment stage reads the alpha-test flags out of the same block.
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = sizeof(ShadowPush);  // one combined matrix, plus the sway slot
    shadowPipelineLayout_ = createPipelineLayout(device, {shadowParams_.layout}, {pc});
    if (!shadowPipelineLayout_) {
        LOG_ERROR("TerrainRenderer: failed to create shadow pipeline layout");
        return false;
    }

    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/shadow.vert.spv")) {
        LOG_ERROR("TerrainRenderer: failed to load shadow vertex shader");
        return false;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/shadow.frag.spv")) {
        LOG_ERROR("TerrainRenderer: failed to load shadow fragment shader");
        vertShader.destroy();
        return false;
    }

    // The shadow shader is shared with the skinned renderers, so it declares
    // bone inputs terrain has none of; kTerrainShadowVertexAttributes says
    // where they point and why.
    const VkVertexInputBindingDescription vertBind =
        perVertexBinding(sizeof(pipeline::TerrainVertex));
    const std::vector<VkVertexInputAttributeDescription> vertAttrs =
        toVkAttributes(kTerrainShadowVertexAttributes);

    shadowPipeline_ = buildShadowPipeline(
        device, vkCtx->getPipelineCache(),
        vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
        fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT),
        vertBind, vertAttrs, shadowPipelineLayout_, shadowRenderPass,
        vkCtx->useDynamicRendering());

    vertShader.destroy();
    fragShader.destroy();

    if (!shadowPipeline_) {
        LOG_ERROR("TerrainRenderer: failed to create shadow pipeline");
        return false;
    }
    LOG_INFO("TerrainRenderer shadow pipeline initialized");
    return true;
}

void TerrainRenderer::renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                                    const glm::vec3& shadowCenter, float shadowRadius) {
    if (!shadowPipeline_ || !shadowParams_.set) return;
    if (chunks.empty()) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_,
        0, 1, &shadowParams_.set, 0, nullptr);

    // Identity model matrix - terrain vertices are already in world space
    // Terrain is already in world space, so the model matrix it used to push
    // was the identity: the combined matrix is the light-space one.
    ShadowPush push{.lightSpaceModel = lightSpaceMatrix};
    // Both stages, as the layout's range declares them (the fragment stage
    // reads the alpha-test flags); the vertex stage alone is a validation error.
    vkCmdPushConstants(cmd, shadowPipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(ShadowPush), &push);

    // Tiles first. Every loaded chunk was tested here each frame - 15k to 28k
    // of them, each a light-space transform on a ~200-byte struct - to keep
    // about 2000. Grouped apart from the main pass's tiles, which the terrain
    // worker reads; the two are kept by the same function.
    const bool regrouped = updateTileGrouping(shadowTiles_, &shadowTouched_);

    // The draws are kept between frames. What the cull reads is the light
    // matrix, the centre, the radius and the chunks; with the first three the
    // same to the bit (the sun settles exactly on its target, and the centre
    // follows the player, so standing still they are) and no chunk removed,
    // every tile that gained no chunk would be culled to the list it already
    // has. Those that did gain are culled again, whole, since their sphere
    // has changed. Anything else culls everything, as every frame used to.
    // Either way the list is the one this frame's cull gives, so the map is
    // the same. WOWEE_TERRAIN_SHADOW_CACHE=0 culls everything every frame.
    const bool lightSame = shadowListValid_ && kTerrainShadowCache && !regrouped &&
        std::memcmp(&shadowListLightSpace_, &lightSpaceMatrix, sizeof(glm::mat4)) == 0 &&
        shadowListCenter_ == shadowCenter && shadowListRadius_ == shadowRadius;
    if (!lightSame) {
        shadowTileKept_.resize(shadowTiles_.tiles.size());  // each one cleared by its cull
        for (uint32_t slot = 0; slot < shadowTiles_.tiles.size(); ++slot)
            cullShadowTile(slot, lightSpaceMatrix, shadowCenter, shadowRadius);
        buildShadowDraws();
        shadowListLightSpace_ = lightSpaceMatrix;
        shadowListCenter_ = shadowCenter;
        shadowListRadius_ = shadowRadius;
        shadowListValid_ = true;
        ++shadowListFull_;
    } else if (!shadowTouched_.empty() || shadowDrawsStale_) {
        // Also when tiles moved into or out of the mega buffers: the kept
        // chunks are the same, but where each is drawn from is not.
        shadowTileKept_.resize(shadowTiles_.tiles.size());
        for (uint32_t slot : shadowTouched_)
            cullShadowTile(slot, lightSpaceMatrix, shadowCenter, shadowRadius);
        buildShadowDraws();
        ++shadowListPatched_;
    } else {
        ++shadowListKept_;
    }
    shadowDrawsStale_ = false;
    slotShadowRadius_ = shadowRadius;

    // Bind mega buffers once for shadow pass (same as opaque)
    const bool useMegaShadow = (megaVB_ && megaIB_);
    bool megaShadowBound = false;
    if (useMegaShadow) {
        VkDeviceSize megaOffset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &megaVB_, &megaOffset);
        vkCmdBindIndexBuffer(cmd, megaIB_, 0, VK_INDEX_TYPE_UINT32);
        megaShadowBound = true;
    }
    for (const TerrainShadowDraw& draw : shadowDraws_) {
        if (!draw.vertexBuffer) {
            // Rebound after a fallback chunk, for the reason given in the main
            // pass above: the mega offsets are meaningless against the buffer a
            // single chunk left bound.
            if (!megaShadowBound) {
                VkDeviceSize megaOffset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &megaVB_, &megaOffset);
                vkCmdBindIndexBuffer(cmd, megaIB_, 0, VK_INDEX_TYPE_UINT32);
                megaShadowBound = true;
            }
        } else {
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &draw.vertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, draw.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            megaShadowBound = false;
        }
        vkCmdDrawIndexed(cmd, draw.range.indexCount, 1, draw.range.firstIndex, draw.range.vertexOffset, 0);
    }

    // How many chunks cast, said when it moves by a quarter: the count is the
    // cost of this pass, and the cull that decides it is not obvious.
    const uint32_t shadowChunksDrawn = shadowDrawChunks_;
    static uint32_t lastReported = 0;
    if (shadowChunksDrawn > lastReported + lastReported / 4 + 16 ||
        shadowChunksDrawn + lastReported / 4 + 16 < lastReported) {
        LOG_WARNING("Terrain shadow pass: ", shadowChunksDrawn, " of ", chunks.size(),
                    " chunks cast in ", shadowDraws_.size(), " draws; list kept in ",
                    shadowListKept_, " frames, culled whole in ", shadowListFull_,
                    ", patched for new chunks in ", shadowListPatched_);
        lastReported = shadowChunksDrawn;
    }
}

void TerrainRenderer::cullShadowTile(uint32_t slot, const glm::mat4& lightSpaceMatrix,
                                     const glm::vec3& shadowCenter, float shadowRadius) {
    if (slot >= shadowTiles_.tiles.size() || slot >= shadowTileKept_.size()) return;
    const TileChunkGroup& tile = shadowTiles_.tiles[slot];
    std::vector<uint32_t>& kept = shadowTileKept_[slot];
    kept.clear();

    // Culled against the map's own footprint, not a sphere round the player.
    // The map is a box reaching hundreds of yards toward the sun, and a ridge
    // that far off in that direction is exactly what shades the ground and the
    // air round the player - the sphere left it out, so the sun lit the mist
    // straight through it. A chunk is kept if its centre lands on the map
    // across the light, at any depth along it; the orthographic scale of the
    // matrix is what turns the chunk's radius into the same units.
    //
    // Bounded in depth as well. Beyond the player, on the side away from the
    // sun, terrain shades nothing the camera is near - and left in, a low sun
    // turned the footprint into a strip the length of the loaded world, and
    // the shadow pass drew thousands of chunks a frame.
    const glm::vec4 centreLs = lightSpaceMatrix * glm::vec4(shadowCenter, 1.0f);
    const float scaleZ = glm::length(glm::vec3(lightSpaceMatrix[0][2], lightSpaceMatrix[1][2],
                                               lightSpaceMatrix[2][2]));
    const float scaleX = glm::length(glm::vec3(lightSpaceMatrix[0][0], lightSpaceMatrix[1][0],
                                               lightSpaceMatrix[2][0]));
    const float scaleY = glm::length(glm::vec3(lightSpaceMatrix[0][1], lightSpaceMatrix[1][1],
                                               lightSpaceMatrix[2][1]));

    // A tile's sphere holds all of its chunks' spheres, so a tile that fails
    // the same test cannot hold a chunk that passes it.
    {
        const glm::vec4 tls = lightSpaceMatrix * glm::vec4(tile.center, 1.0f);
        if (std::abs(tls.x) > 1.0f + tile.radius * scaleX ||
            std::abs(tls.y) > 1.0f + tile.radius * scaleY) {
            return;
        }
        if (tls.z > centreLs.z + (shadowRadius + tile.radius) * scaleZ) return;
    }
    for (uint32_t ci : tile.chunkIndices) {
        if (ci >= chunks.size()) continue;
        const auto& chunk = chunks[ci];
        if (!chunk.isValid()) continue;

        const glm::vec4 ls = lightSpaceMatrix * glm::vec4(chunk.boundingSphereCenter, 1.0f);
        if (std::abs(ls.x) > 1.0f + chunk.boundingSphereRadius * scaleX ||
            std::abs(ls.y) > 1.0f + chunk.boundingSphereRadius * scaleY) {
            continue;
        }
        // Depth grows away from the light in either depth convention.
        if (ls.z > centreLs.z + (shadowRadius + chunk.boundingSphereRadius) * scaleZ) continue;
        kept.push_back(ci);
    }
}

void TerrainRenderer::buildShadowDraws() {
    // In tile order and, within a tile, chunk order, as the draws always
    // went. A tile's chunks sit end to end in its mega slot, so with the
    // indices already offset to their vertices (uploadChunk) a run of kept
    // neighbours is one draw: the triangles of all of them, in the same
    // order; the cast-count line says how many draws are left. A chunk
    // outside the mega buffers is still a draw of its own on its own buffers.
    shadowDraws_.clear();
    shadowDrawChunks_ = 0;
    const bool useMega = (megaVB_ && megaIB_);
    for (const auto& kept : shadowTileKept_) {
        for (uint32_t ci : kept) {
            const TerrainChunkGPU& chunk = chunks[ci];
            ++shadowDrawChunks_;
            if (useMega && chunk.megaBaseVertex >= 0) {
                const IndexRange range{.firstIndex = chunk.megaFirstIndex, .indexCount = chunk.indexCount,
                                       .vertexOffset = megaVertexOffset(chunk)};
                if (kTerrainShadowMerge && !shadowDraws_.empty() && !shadowDraws_.back().vertexBuffer &&
                    tryExtendIndexRange(shadowDraws_.back().range, range)) {
                    continue;
                }
                shadowDraws_.push_back({.range = range});
            } else {
                shadowDraws_.push_back({.vertexBuffer = chunk.vertexBuffer, .indexBuffer = chunk.indexBuffer,
                                        .range = {.firstIndex = 0, .indexCount = chunk.indexCount,
                                                  .vertexOffset = 0}});
            }
        }
    }
}

void TerrainRenderer::removeTile(int tileX, int tileY) {
    // One pass: destroy the tile's chunks in order, then close the gaps once.
    // Erasing each chunk where it stood moved every chunk after it, 256 times
    // per tile - with a city's 11k chunks loaded that was half a gigabyte of
    // copying and a 30-95 ms frame on every tile unload (regression tour,
    // 2026-11-04).
    const auto onTile = [&](const TerrainChunkGPU& c) {
        return c.tileX == tileX && c.tileY == tileY;
    };
    int removed = 0;
    for (auto& c : chunks) {
        if (!onTile(c)) continue;
        destroyChunkGPU(c);
        ++chunkRemovals();
        ++chunksGeneration_;
        removed++;
    }
    if (removed > 0) {
        chunks.erase(std::remove_if(chunks.begin(), chunks.end(), onTile), chunks.end());
        ++chunksRemovedGeneration_;
        LOG_DEBUG("Removed ", removed, " terrain chunks for tile [", tileX, ",", tileY, "]");
    }
    releaseMegaTileSlot(terrainTileKey(tileX, tileY));
    slotRefused_.erase(terrainTileKey(tileX, tileY));
}

void TerrainRenderer::clear() {
    if (!vkCtx) return;

    for (auto& chunk : chunks) {
        destroyChunkGPU(chunk);
    }
    chunks.clear();
    ++chunkRemovals();
    ++chunksGeneration_;
    ++chunksRemovedGeneration_;
    while (!megaTileSlots_.empty()) releaseMegaTileSlot(megaTileSlots_.begin()->first);
    slotRefused_.clear();
    megaSlotsReserved_ = 0;
    renderedChunks = 0;
    // No chunk is holding anything now, so nothing needs protecting from the
    // next upload's point of view either.
    evictProtectFrom_ = 0;
}

void TerrainRenderer::registerRtChunk(TerrainChunkGPU& gpuChunk, const pipeline::ChunkMesh& chunk) {
    if (!rtScene_) return;
    RtScene::MeshSource src;
    src.positions.reserve(chunk.vertices.size());
    for (const auto& v : chunk.vertices) {
        src.positions.emplace_back(v.position[0], v.position[1], v.position[2]);
    }
    src.indices.assign(chunk.indices.begin(), chunk.indices.end());

    // One colour for the chunk: the base layer's, with each layer over it in
    // proportion to how much of the chunk its alpha map covers.
    glm::vec3 albedo = gpuChunk.baseTexture ? gpuChunk.baseTexture->averageColor() : glm::vec3(0.5f);
    for (int i = 0; i < 3 && i + 1 < static_cast<int>(chunk.layers.size()); ++i) {
        const VkTexture* tex = gpuChunk.layerTextures[i];
        const auto& alpha = chunk.layers[i + 1].alphaData;
        if (!tex || alpha.empty()) continue;
        uint64_t sum = 0;
        for (uint8_t a : alpha) sum += a;
        const float cover = static_cast<float>(sum) / (255.0f * static_cast<float>(alpha.size()));
        albedo = glm::mix(albedo, tex->averageColor(), cover);
    }
    src.surfaces.push_back(packRtSurface(albedo, 1.0f));

    gpuChunk.rtMesh = rtScene_->addMesh(std::move(src));
    // Terrain vertices are already in world space.
    gpuChunk.rtInstance = rtScene_->addInstance(gpuChunk.rtMesh, glm::mat4(1.0f));
}

void TerrainRenderer::releaseChunkAlphaTextures(TerrainChunkGPU& chunk) {
    if (!vkCtx || alphaTextureKeys_.empty()) return;
    std::vector<VkTexture*> retired;
    const auto release = [&](VkTexture* tex) {
        if (!tex) return;
        auto keyIt = alphaTextureKeys_.find(tex);
        if (keyIt == alphaTextureKeys_.end()) return;  // shared (opaque) or already gone
        auto it = textureCache.find(keyIt->second);
        alphaTextureKeys_.erase(keyIt);
        if (it == textureCache.end()) return;
        textureCacheBytes_ -= std::min(textureCacheBytes_, it->second.approxBytes);
        retired.push_back(it->second.texture.release());
        textureCache.erase(it);
    };
    for (VkTexture* t : chunk.alphaTextures) release(t);
    release(chunk.shadowTexture);
    if (retired.empty()) return;
    VkDevice device = vkCtx->getDevice();
    VmaAllocator allocator = vkCtx->getAllocator();
    vkCtx->deferAfterAllFrameFences([device, allocator, retired]() {
        for (VkTexture* tex : retired) {
            tex->destroy(device, allocator);
            delete tex;
        }
    });
}

void TerrainRenderer::destroyChunkGPU(TerrainChunkGPU& chunk) {
    if (!vkCtx) return;

    if (rtScene_ && chunk.rtMesh != RtScene::kInvalid) {
        rtScene_->removeInstance(chunk.rtInstance);
        rtScene_->removeMesh(chunk.rtMesh);
        chunk.rtInstance = chunk.rtMesh = RtScene::kInvalid;
    }

    // A chunk dropped or handed back for a retry right after its upload was
    // the last thing put in its tile's mega slot; the space goes back so the
    // retry lands where it would have and the tile stays end to end. Never
    // drawn, so nothing in flight reads it. (The last chunk of a tile being
    // unloaded passes the same test, on a slot about to be released whole.)
    if (chunk.megaBaseVertex >= 0) {
        auto it = megaTileSlots_.find(terrainTileKey(chunk.tileX, chunk.tileY));
        if (it != megaTileSlots_.end()) {
            MegaTileSlot& s = it->second;
            const uint32_t vertEnd = s.slot * kMegaTileSlotVerts + s.vertsUsed;
            const uint32_t indexEnd = s.slot * kMegaTileSlotIndices + s.indicesUsed;
            if (static_cast<uint32_t>(chunk.megaBaseVertex) + chunk.vertexCount == vertEnd &&
                chunk.megaFirstIndex + chunk.indexCount == indexEnd &&
                s.vertsUsed >= chunk.vertexCount && s.indicesUsed >= chunk.indexCount) {
                s.vertsUsed -= chunk.vertexCount;
                s.indicesUsed -= chunk.indexCount;
            }
        }
        chunk.megaBaseVertex = -1;
    }

    VkDevice device = vkCtx->getDevice();
    VmaAllocator allocator = vkCtx->getAllocator();

    // These resources may still be referenced by in-flight command buffers from
    // previous frames. Defer actual destruction until this frame slot is safe.
    ::VkBuffer vertexBuffer = chunk.vertexBuffer;
    VmaAllocation vertexAlloc = chunk.vertexAlloc;
    ::VkBuffer indexBuffer = chunk.indexBuffer;
    VmaAllocation indexAlloc = chunk.indexAlloc;
    ::VkBuffer paramsUBO = chunk.paramsUBO;
    VmaAllocation paramsAlloc = chunk.paramsAlloc;
    VkDescriptorPool pool = materialDescPool;
    VkDescriptorSet materialSet = chunk.materialSet;

    // The chunk's own alpha maps go with it. They sit in the texture cache
    // (createAlphaTexture puts them there, so they count against its budget)
    // but no other chunk can ever use them.
    releaseChunkAlphaTextures(chunk);

    chunk.vertexBuffer = VK_NULL_HANDLE;
    chunk.vertexAlloc = VK_NULL_HANDLE;
    chunk.indexBuffer = VK_NULL_HANDLE;
    chunk.indexAlloc = VK_NULL_HANDLE;
    chunk.paramsUBO = VK_NULL_HANDLE;
    chunk.paramsAlloc = VK_NULL_HANDLE;
    chunk.materialSet = VK_NULL_HANDLE;
    chunk.baseTexture = nullptr;
    for (VkTexture*& t : chunk.layerTextures) t = nullptr;
    for (VkTexture*& t : chunk.alphaTextures) t = nullptr;
    chunk.shadowTexture = nullptr;

    vkCtx->deferAfterAllFrameFences([device, allocator, vertexBuffer, vertexAlloc, indexBuffer, indexAlloc,
                                     paramsUBO, paramsAlloc, pool, materialSet]() {
        if (vertexBuffer) {
            AllocatedBuffer ab{}; ab.buffer = vertexBuffer; ab.allocation = vertexAlloc;
            destroyBuffer(allocator, ab);
        }
        if (indexBuffer) {
            AllocatedBuffer ab{}; ab.buffer = indexBuffer; ab.allocation = indexAlloc;
            destroyBuffer(allocator, ab);
        }
        if (paramsUBO) {
            AllocatedBuffer ab{}; ab.buffer = paramsUBO; ab.allocation = paramsAlloc;
            destroyBuffer(allocator, ab);
        }
        if (materialSet && pool) {
            VkDescriptorSet set = materialSet;
            vkFreeDescriptorSets(device, pool, 1, &set);
        }
    });
}

int TerrainRenderer::getTriangleCount() const {
    int total = 0;
    for (const auto& chunk : chunks) {
        total += chunk.indexCount / 3;
    }
    return total;
}

bool TerrainRenderer::isChunkVisible(const TerrainChunkGPU& chunk, const Frustum& frustum) {
    return frustum.intersectsSphere(chunk.boundingSphereCenter, chunk.boundingSphereRadius);
}

void TerrainRenderer::calculateBoundingSphere(TerrainChunkGPU& gpuChunk,
                                                const pipeline::ChunkMesh& meshChunk) {
    if (meshChunk.vertices.empty()) {
        gpuChunk.boundingSphereRadius = 0.0f;
        gpuChunk.boundingSphereCenter = glm::vec3(0.0f);
        return;
    }

    glm::vec3 min(std::numeric_limits<float>::max());
    glm::vec3 max(std::numeric_limits<float>::lowest());

    for (const auto& vertex : meshChunk.vertices) {
        glm::vec3 pos(vertex.position[0], vertex.position[1], vertex.position[2]);
        min = glm::min(min, pos);
        max = glm::max(max, pos);
    }

    gpuChunk.boundingSphereCenter = (min + max) * 0.5f;

    // Steep enough anywhere for the cliff pipelines to draw anything but the
    // stock read (cliff_projection.hpp); a hair of margin for the shader's
    // own normalising.
    gpuChunk.steep = false;
    for (const auto& vertex : meshChunk.vertices) {
        const glm::vec3 n(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
        const float len = glm::length(n);
        if (len > 1e-4f && std::fabs(n.z) / len < cliff_projection::kFlatNormalZ + 0.01f) {
            gpuChunk.steep = true;
            break;
        }
    }

    float maxDistSq = 0.0f;
    for (const auto& vertex : meshChunk.vertices) {
        glm::vec3 pos(vertex.position[0], vertex.position[1], vertex.position[2]);
        glm::vec3 diff = pos - gpuChunk.boundingSphereCenter;
        float distSq = glm::dot(diff, diff);
        maxDistSq = std::max(maxDistSq, distSq);
    }

    gpuChunk.boundingSphereRadius = std::sqrt(maxDistSq);
}

} // namespace rendering
} // namespace wowee
