#include "rendering/minimap.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_render_target.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/camera.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <sstream>
#include <cmath>

namespace wowee {
namespace rendering {

// Push constant for tile composite vertex shader: where the image's four
// corners land in the composite, in its clip space. uv00/uv10 are image
// (0,0) and (1,0), uv01/uv11 are (0,1) and (1,1).
//
// A terrain tile is an axis-aligned ninth of the composite, turned a quarter
// (the composite runs south along x and east along y, the image east along u
// and south along v). A WMO tile is wherever the building's placement puts
// it, at any yaw. Corners carry both.
struct MinimapTilePush {
    glm::vec2 uv00, uv10;
    glm::vec2 uv01, uv11;
};  // 32 bytes

namespace {

/// Composite 0..1 space to the clip space the tile shader writes.
glm::vec2 compositeToClip(const glm::vec2& c) { return c * 2.0f - 1.0f; }

}  // namespace

// Push constant for display vertex + fragment shaders
struct MinimapDisplayPush {
    glm::vec4 rect;         // x, y, w, h in 0..1 screen space
    glm::vec2 playerUV;
    float rotation;
    float arrowRotation;
    float zoomRadius;
    int32_t squareShape;
    float opacity;
};  // 44 bytes

Minimap::Minimap() = default;

Minimap::~Minimap() {
    shutdown();
}

/// The pipeline that draws the minimap's assembled texture to the screen.
///
/// initialize() builds it beside the tile pipeline that composes that texture;
/// recreatePipelines() rebuilds only this one, because the tile pipeline draws
/// into an offscreen pass a settings change does not touch. Both described it
/// identically, vertex layout included.
void Minimap::buildDisplayPipeline(VkDevice device,
                                   const VkPipelineShaderStageCreateInfo& vertStage,
                                   const VkPipelineShaderStageCreateInfo& fragStage) {
    // Two vec2s per vertex: position then texture coordinate.
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = 4 * sizeof(float);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attrs(2);
    attrs[0] = { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 };
    attrs[1] = { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 2 * sizeof(float) };

    displayPipeline = PipelineBuilder()
        .setShaders(vertStage, fragStage)
        .setVertexInput({ binding }, attrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(targetPass_ != VK_NULL_HANDLE ? targetSamples_
                                                      : vkCtx->getMsaaSamples())
        .setLayout(displayPipelineLayout)
        .setRenderPass(targetPass_ != VK_NULL_HANDLE ? targetPass_
                                                     : vkCtx->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx->getPipelineCache());
}

bool Minimap::initialize(VkContext* ctx, VkDescriptorSetLayout /*perFrameLayout*/, int size) {
    vkCtx = ctx;
    mapSize = size;
    VkDevice device = vkCtx->getDevice();

    // --- Composite render target (768x768) ---
    compositeTarget = std::make_unique<VkRenderTarget>();
    if (!compositeTarget->create(*vkCtx, COMPOSITE_PX, COMPOSITE_PX)) {
        LOG_ERROR("Minimap: failed to create composite render target");
        return false;
    }

    // --- No-data fallback texture (dark blue-gray, 1x1) ---
    noDataTexture = std::make_unique<VkTexture>();
    uint8_t darkPixel[4] = { 12, 20, 30, 255 };
    noDataTexture->upload(*vkCtx, darkPixel, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
    noDataTexture->createSampler(device, VK_FILTER_NEAREST, VK_FILTER_NEAREST,
                                 VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f);

    // --- Shared quad vertex buffer (unit quad: pos2 + uv2) ---
    float quadVerts[] = {
        // pos (x,y), uv (u,v)
        0.0f, 0.0f,  0.0f, 0.0f,
        1.0f, 0.0f,  1.0f, 0.0f,
        1.0f, 1.0f,  1.0f, 1.0f,
        0.0f, 0.0f,  0.0f, 0.0f,
        1.0f, 1.0f,  1.0f, 1.0f,
        0.0f, 1.0f,  0.0f, 1.0f,
    };
    auto quadBuf = uploadBuffer(*vkCtx, quadVerts, sizeof(quadVerts),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    quadVB = quadBuf.buffer;
    quadVBAlloc = quadBuf.allocation;

    // --- Descriptor set layout: 1 combined image sampler at binding 0 (fragment) ---
    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = 0;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    samplerSetLayout = createDescriptorSetLayout(device, { samplerBinding });

    // --- Descriptor pool ---
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = MAX_DESC_SETS;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = MAX_DESC_SETS;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool);

    // --- Allocate all descriptor sets ---
    // 18 tile sets (2 frames × 9 tiles) + 1 display set = 19 total
    std::vector<VkDescriptorSetLayout> layouts(19, samplerSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descPool;
    allocInfo.descriptorSetCount = 19;
    allocInfo.pSetLayouts = layouts.data();

    // Checked, not assumed. On failure allSets holds nothing this code put
    // there, and every one of the nineteen is then written and bound - which
    // is an invalid handle in a live descriptor rather than a minimap that
    // does not draw.
    VkDescriptorSet allSets[19];
    if (vkAllocateDescriptorSets(device, &allocInfo, allSets) != VK_SUCCESS) {
        LOG_ERROR("Minimap: failed to allocate descriptor sets");
        return false;
    }

    for (int f = 0; f < 2; f++)
        for (int t = 0; t < 9; t++)
            tileDescSets[f][t] = allSets[f * 9 + t];
    displayDescSet = allSets[18];

    // The WMO mode's tile sets, one per tile a composite can draw per frame.
    {
        std::vector<VkDescriptorSetLayout> wmoLayouts(2 * kMaxWmoTiles, samplerSetLayout);
        VkDescriptorSetAllocateInfo wmoAlloc = allocInfo;
        wmoAlloc.descriptorSetCount = static_cast<uint32_t>(wmoLayouts.size());
        wmoAlloc.pSetLayouts = wmoLayouts.data();
        if (vkAllocateDescriptorSets(device, &wmoAlloc, &wmoTileDescSets_[0][0]) != VK_SUCCESS) {
            LOG_ERROR("Minimap: failed to allocate WMO tile descriptor sets");
            return false;
        }
    }

    // --- Write display descriptor set → composite render target ---
    VkDescriptorImageInfo compositeImgInfo = compositeTarget->descriptorInfo();
    VkWriteDescriptorSet displayWrite{};
    displayWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    displayWrite.dstSet = displayDescSet;
    displayWrite.dstBinding = 0;
    displayWrite.descriptorCount = 1;
    displayWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    displayWrite.pImageInfo = &compositeImgInfo;
    vkUpdateDescriptorSets(device, 1, &displayWrite, 0, nullptr);

    // --- Tile pipeline layout: samplerSetLayout + 32-byte push constant (vertex) ---
    VkPushConstantRange tilePush{};
    tilePush.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    tilePush.offset = 0;
    tilePush.size = sizeof(MinimapTilePush);
    tilePipelineLayout = createPipelineLayout(device, { samplerSetLayout }, { tilePush });

    // --- Display pipeline layout: samplerSetLayout + 40-byte push constant (vert+frag) ---
    VkPushConstantRange displayPush{};
    displayPush.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    displayPush.offset = 0;
    displayPush.size = sizeof(MinimapDisplayPush);
    displayPipelineLayout = createPipelineLayout(device, { samplerSetLayout }, { displayPush });

    // --- Vertex input: pos2 (loc 0) + uv2 (loc 1), stride 16 ---
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = 4 * sizeof(float);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attrs(2);
    attrs[0] = { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 };                    // aPos
    attrs[1] = { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 2 * sizeof(float) };    // aUV

    // --- Load tile shaders ---
    {
        VkShaderModule vs, fs;
        if (!vs.loadFromFile(device, "assets/shaders/minimap_tile.vert.spv") ||
            !fs.loadFromFile(device, "assets/shaders/minimap_tile.frag.spv")) {
            LOG_ERROR("Minimap: failed to load tile shaders");
            return false;
        }

        tilePipeline = PipelineBuilder()
            .setShaders(vs.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                        fs.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({ binding }, attrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setNoDepthTest()
            .setColorBlendAttachment(PipelineBuilder::blendDisabled())
            .setLayout(tilePipelineLayout)
            .setRenderPass(compositeTarget->getRenderPass())
            .setDynamicStates(viewportAndScissorDynamic())
            .build(device, vkCtx->getPipelineCache());

        wmoTilePipeline = PipelineBuilder()
            .setShaders(vs.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                        fs.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({ binding }, attrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setNoDepthTest()
            .setColorBlendAttachment(PipelineBuilder::blendAlpha())
            .setLayout(tilePipelineLayout)
            .setRenderPass(compositeTarget->getRenderPass())
            .setDynamicStates(viewportAndScissorDynamic())
            .build(device, vkCtx->getPipelineCache());

        vs.destroy();
        fs.destroy();
    }

    // --- Load display shaders ---
    {
        VkShaderModule vs, fs;
        if (!vs.loadFromFile(device, "assets/shaders/minimap_display.vert.spv") ||
            !fs.loadFromFile(device, "assets/shaders/minimap_display.frag.spv")) {
            LOG_ERROR("Minimap: failed to load display shaders");
            return false;
        }

        buildDisplayPipeline(device, vs.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                             fs.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT));

        vs.destroy();
        fs.destroy();
    }

    if (!tilePipeline || !wmoTilePipeline || !displayPipeline) {
        LOG_ERROR("Minimap: failed to create pipelines");
        return false;
    }

    LOG_INFO("Minimap initialized (", mapSize, "x", mapSize, " screen, ",
             COMPOSITE_PX, "x", COMPOSITE_PX, " composite)");
    return true;
}

void Minimap::shutdown() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    VmaAllocator alloc = vkCtx->getAllocator();

    vkDeviceWaitIdle(device);

    destroy(device, tilePipeline);
    destroy(device, wmoTilePipeline);
    destroy(device, displayPipeline);
    destroy(device, tilePipelineLayout);
    destroy(device, displayPipelineLayout);
    destroy(device, descPool);
    destroy(device, samplerSetLayout);

    destroy(alloc, quadVB, quadVBAlloc);

    for (auto& [hash, tex] : tileTextureCache) {
        if (tex) tex->destroy(device, alloc);
    }
    tileTextureCache.clear();
    tileInsertionOrder.clear();

    if (noDataTexture) { noDataTexture->destroy(device, alloc); noDataTexture.reset(); }
    if (compositeTarget) { compositeTarget->destroy(device, alloc); compositeTarget.reset(); }

    vkCtx = nullptr;
}

void Minimap::recreatePipelines() {
    if (!vkCtx || !displayPipelineLayout) return;
    VkDevice device = vkCtx->getDevice();

    destroy(device, displayPipeline);

    VkShaderModule vs, fs;
    if (!vs.loadFromFile(device, "assets/shaders/minimap_display.vert.spv") ||
        !fs.loadFromFile(device, "assets/shaders/minimap_display.frag.spv")) {
        LOG_ERROR("Minimap: failed to reload display shaders for pipeline recreation");
        return;
    }

    buildDisplayPipeline(device, vs.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                         fs.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT));

    vs.destroy();
    fs.destroy();

    LOG_INFO("Minimap: display pipeline recreated with MSAA ", static_cast<int>(vkCtx->getMsaaSamples()), "x");
}

void Minimap::setMapName(const std::string& name) {
    if (mapName != name) {
        mapName = name;
        hasCachedFrame = false;
        lastCenterTileX = -1;
        lastCenterTileY = -1;
    }
}

// --------------------------------------------------------
// TRS parsing
// --------------------------------------------------------

void Minimap::parseTRS() {
    if (trsParsed || !assetManager) return;
    trsParsed = true;

    auto data = assetManager->readFile("Textures\\Minimap\\md5translate.trs");
    if (data.empty()) {
        LOG_WARNING("Failed to load md5translate.trs");
        return;
    }

    std::string content(reinterpret_cast<const char*>(data.data()), data.size());
    std::istringstream stream(content);
    std::string line;
    int count = 0;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.substr(0, 4) == "dir:") continue;

        auto tabPos = line.find('\t');
        if (tabPos == std::string::npos) continue;

        std::string key = line.substr(0, tabPos);
        std::string hashFile = line.substr(tabPos + 1);

        if (key.size() > 4 && key.substr(key.size() - 4) == ".blp")
            key = key.substr(0, key.size() - 4);
        if (hashFile.size() > 4 && hashFile.substr(hashFile.size() - 4) == ".blp")
            hashFile = hashFile.substr(0, hashFile.size() - 4);

        trsLookup[key] = hashFile;
        count++;

        // A WMO's tiles: "WMO\...\ironforge_044_00_01". Kept again in lower
        // case, and the root (everything before "_044_00_01") noted.
        if (key.size() > 14 && (key[0] == 'W' || key[0] == 'w') &&
            (key[1] == 'M' || key[1] == 'm') && (key[2] == 'O' || key[2] == 'o') && key[3] == '\\') {
            std::string lower = key;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            wmoTileRoots_.insert(lower.substr(0, lower.size() - 10));
            wmoTrsLookup_[std::move(lower)] = hashFile;
        }
    }

    LOG_INFO("Parsed md5translate.trs: ", count, " entries");
}

// --------------------------------------------------------
// Tile texture loading
// --------------------------------------------------------

VkTexture* Minimap::getOrLoadTileTexture(int tileX, int tileY) {
    if (!trsParsed) parseTRS();

    std::string key = mapName + "\\map" + std::to_string(tileX) + "_" + std::to_string(tileY);

    auto trsIt = trsLookup.find(key);
    if (trsIt == trsLookup.end())
        return noDataTexture.get();

    VkTexture* tex = loadTextureByHash(trsIt->second);
    return tex ? tex : noDataTexture.get();
}

VkTexture* Minimap::loadTextureByHash(const std::string& hash) {
    auto cacheIt = tileTextureCache.find(hash);
    if (cacheIt != tileTextureCache.end())
        return cacheIt->second.get();

    // Load from MPQ
    std::string blpPath = "Textures\\Minimap\\" + hash + ".blp";
    auto blpImage = assetManager->loadTexture(blpPath);
    if (!blpImage.isValid()) {
        tileTextureCache[hash] = nullptr;  // Mark as failed
        return nullptr;
    }

    auto tex = std::make_unique<VkTexture>();
    tex->upload(*vkCtx, blpImage.data.data(), blpImage.width, blpImage.height,
                VK_FORMAT_R8G8B8A8_UNORM, false);
    tex->createSampler(vkCtx->getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                       VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f);

    VkTexture* ptr = tex.get();
    tileTextureCache[hash] = std::move(tex);
    tileInsertionOrder.push_back(hash);

    // Evict oldest tiles when cache grows too large to bound GPU memory usage.
    // Freed once no frame in flight can still be sampling it: a composite
    // from the last frame may have it bound.
    while (tileInsertionOrder.size() > MAX_TILE_CACHE) {
        auto oldIt = tileTextureCache.find(tileInsertionOrder.front());
        if (oldIt != tileTextureCache.end()) {
            std::shared_ptr<VkTexture> dying(std::move(oldIt->second));
            if (dying) vkCtx->deferAfterAllFrameFences([dying]() {});
            tileTextureCache.erase(oldIt);
        }
        tileInsertionOrder.pop_front();
    }

    return ptr;
}

// --------------------------------------------------------
// Update tile descriptor sets for composite pass
// --------------------------------------------------------

void Minimap::updateTileDescriptors(uint32_t frameIdx, int centerTileX, int centerTileY) {
    constexpr int kTileCount = 9; // 3x3 grid
    VkDevice device = vkCtx->getDevice();
    std::array<VkDescriptorImageInfo, kTileCount> imgInfos{};
    std::array<VkWriteDescriptorSet, kTileCount> writes{};
    int slot = 0;

    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            int tx = centerTileX + dr;
            int ty = centerTileY + dc;

            VkTexture* tileTex = getOrLoadTileTexture(tx, ty);
            if (!tileTex || !tileTex->isValid())
                tileTex = noDataTexture.get();

            imgInfos[slot] = tileTex->descriptorInfo();

            writes[slot] = {};
            writes[slot].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[slot].dstSet = tileDescSets[frameIdx][slot];
            writes[slot].dstBinding = 0;
            writes[slot].descriptorCount = 1;
            writes[slot].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[slot].pImageInfo = &imgInfos[slot];
            slot++;
        }
    }

    vkUpdateDescriptorSets(device, kTileCount, writes.data(), 0, nullptr);
}

// --------------------------------------------------------
// Off-screen composite pass (call BEFORE main render pass)
// --------------------------------------------------------

void Minimap::compositePass(VkCommandBuffer cmd, const glm::vec3& centerWorldPos) {
    if (!enabled || !assetManager || !compositeTarget || !compositeTarget->isValid()) return;

    if (!trsParsed) parseTRS();

    if (indoors_) {
        compositeWmoPass(cmd, centerWorldPos);
        return;
    }

    // Check if composite needs refresh
    const auto now = std::chrono::steady_clock::now();
    bool needsRefresh = !hasCachedFrame || compositeIsWmo_;
    if (!needsRefresh) {
        float mdx = centerWorldPos.x - lastUpdatePos.x;
        float mdy = centerWorldPos.y - lastUpdatePos.y;
        float movedSq = mdx * mdx + mdy * mdy;
        float elapsed = std::chrono::duration<float>(now - lastUpdateTime).count();
        needsRefresh = (movedSq >= updateDistance * updateDistance) || (elapsed >= updateIntervalSec);
    }

    // Also refresh if player crossed a tile boundary
    auto [curTileX, curTileY] = core::coords::worldToTile(centerWorldPos.x, centerWorldPos.y);
    if (curTileX != lastCenterTileX || curTileY != lastCenterTileY)
        needsRefresh = true;

    if (!needsRefresh) return;

    uint32_t frameIdx = vkCtx->getCurrentFrame();

    // Update tile descriptor sets
    updateTileDescriptors(frameIdx, curTileX, curTileY);

    // Begin off-screen render pass
    VkClearColorValue clearColor = {{ 0.05f, 0.08f, 0.12f, 1.0f }};
    compositeTarget->beginPass(cmd, clearColor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tilePipeline);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &quadVB, &offset);

    // Draw 3x3 tile grid
    int slot = 0;
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    tilePipelineLayout, 0, 1,
                                    &tileDescSets[frameIdx][slot], 0, nullptr);

            // The ninth at column dc + 1 and row dr + 1. The image's u runs
            // down the composite and its v across, which is the quarter turn
            // the shader used to make by sampling (v, u).
            const glm::vec2 cell(static_cast<float>(dc + 1), static_cast<float>(dr + 1));
            MinimapTilePush push{};
            push.uv00 = compositeToClip(cell / 3.0f);
            push.uv10 = compositeToClip((cell + glm::vec2(0.0f, 1.0f)) / 3.0f);
            push.uv01 = compositeToClip((cell + glm::vec2(1.0f, 0.0f)) / 3.0f);
            push.uv11 = compositeToClip((cell + glm::vec2(1.0f, 1.0f)) / 3.0f);
            vkCmdPushConstants(cmd, tilePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(push), &push);

            vkCmdDraw(cmd, 6, 1, 0, 0);
            slot++;
        }
    }

    compositeTarget->endPass(cmd);

    // Update tracking
    lastCenterTileX = curTileX;
    lastCenterTileY = curTileY;
    lastUpdateTime = now;
    lastUpdatePos = centerWorldPos;
    hasCachedFrame = true;
    compositeIsWmo_ = false;
}

// --------------------------------------------------------
// WMO mode: a building's own minimap tiles
// --------------------------------------------------------

bool Minimap::setWmoView(const WmoView& view) {
    if (!trsParsed) parseTRS();
    if (view.tileRoot.empty() || !wmoTileRoots_.count(view.tileRoot) ||
        view.currentGroup >= view.groups.size()) {
        clearWmoView();
        return false;
    }

    const std::vector<size_t> order =
        wmo_minimap::groupsToDraw(view.groups, view.currentGroup, view.playerLocal.z);

    // A different building, or a different set of rooms (a stair up to the
    // next floor), wants a new composite now rather than at the next tick.
    bool changed = !indoors_ || view.tileRoot != wmoRoot_ || order.size() != wmoDrawGroups_.size() ||
                   view.modelMatrix != wmoModelMatrix_;
    for (size_t i = 0; !changed && i < order.size(); ++i) {
        changed = view.groups[order[i]].fileIndex != wmoDrawGroups_[i].fileIndex;
    }
    wmoPlayerLocalZ_ = view.playerLocal.z;
    if (!changed) return true;

    wmoRoot_ = view.tileRoot;
    wmoModelMatrix_ = view.modelMatrix;
    wmoDrawGroups_.clear();
    for (size_t i : order) wmoDrawGroups_.push_back(view.groups[i]);
    hasCachedFrame = false;
    if (!indoors_) {
        indoors_ = true;
        viewRadius = kInsideZoomRadius[insideZoomLevel_];
    }
    return true;
}

void Minimap::clearWmoView() {
    if (!indoors_) return;
    indoors_ = false;
    wmoRoot_.clear();
    wmoDrawGroups_.clear();
    viewRadius = kZoomRadius[zoomLevel_];
    hasCachedFrame = false;
}

void Minimap::compositeWmoPass(VkCommandBuffer cmd, const glm::vec3& centerWorldPos) {
    const auto now = std::chrono::steady_clock::now();
    const float span = wmoCompositeSpan();
    bool needsRefresh = !hasCachedFrame || !compositeIsWmo_ || span != wmoCompositeSpanUsed_;
    if (!needsRefresh) {
        float mdx = centerWorldPos.x - lastUpdatePos.x;
        float mdy = centerWorldPos.y - lastUpdatePos.y;
        float elapsed = std::chrono::duration<float>(now - lastUpdateTime).count();
        needsRefresh = (mdx * mdx + mdy * mdy >= updateDistance * updateDistance) ||
                       (elapsed >= updateIntervalSec);
    }
    if (!needsRefresh) return;

    const uint32_t frameIdx = vkCtx->getCurrentFrame();
    const glm::vec2 center(centerWorldPos.x, centerWorldPos.y);

    // Every tile of every drawn group that lands in the composite. Tiles
    // md5translate does not name are not there in the data either: the
    // stock client leaves them black, as the clear below does.
    std::vector<VkDescriptorImageInfo> imgInfos;
    std::vector<MinimapTilePush> pushes;
    imgInfos.reserve(kMaxWmoTiles);
    pushes.reserve(kMaxWmoTiles);
    for (const wmo_minimap::Group& g : wmoDrawGroups_) {
        const int nx = wmo_minimap::tileCount(g.boxMax.x - g.boxMin.x);
        const int ny = wmo_minimap::tileCount(g.boxMax.y - g.boxMin.y);
        for (int x = 0; x < nx; ++x) {
            for (int y = 0; y < ny; ++y) {
                if (pushes.size() >= kMaxWmoTiles) break;
                auto it = wmoTrsLookup_.find(wmo_minimap::tileKey(wmoRoot_, g.fileIndex, x, y));
                if (it == wmoTrsLookup_.end()) continue;

                // Placed before loading, so a tile out of view costs nothing.
                // The image size is not known yet: the full 128 yards bounds it.
                const wmo_minimap::TileCorners full = wmo_minimap::tileCorners(
                    glm::vec2(g.boxMin), x, y,
                    static_cast<uint32_t>(wmo_minimap::kTileYards * wmo_minimap::kPixelsPerYard),
                    static_cast<uint32_t>(wmo_minimap::kTileYards * wmo_minimap::kPixelsPerYard));
                auto toClip = [&](const glm::vec2& local) {
                    const glm::vec4 r = wmoModelMatrix_ * glm::vec4(local, wmoPlayerLocalZ_, 1.0f);
                    return compositeToClip(wmo_minimap::renderToComposite(glm::vec2(r), center, span));
                };
                const glm::vec2 f[4] = {toClip(full.uv00), toClip(full.uv10),
                                        toClip(full.uv01), toClip(full.uv11)};
                glm::vec2 lo = f[0], hi = f[0];
                for (const glm::vec2& p : f) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
                if (hi.x < -1.0f || hi.y < -1.0f || lo.x > 1.0f || lo.y > 1.0f) continue;

                VkTexture* tex = loadTextureByHash(it->second);
                if (!tex || !tex->isValid()) continue;
                const wmo_minimap::TileCorners c = wmo_minimap::tileCorners(
                    glm::vec2(g.boxMin), x, y, tex->getWidth(), tex->getHeight());
                MinimapTilePush push{};
                push.uv00 = toClip(c.uv00);
                push.uv10 = toClip(c.uv10);
                push.uv01 = toClip(c.uv01);
                push.uv11 = toClip(c.uv11);
                pushes.push_back(push);
                imgInfos.push_back(tex->descriptorInfo());
            }
        }
    }

    std::vector<VkWriteDescriptorSet> writes(pushes.size());
    for (size_t i = 0; i < pushes.size(); ++i) {
        writes[i] = {};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = wmoTileDescSets_[frameIdx][i];
        writes[i].dstBinding = 0;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imgInfos[i];
    }
    if (!writes.empty()) {
        vkUpdateDescriptorSets(vkCtx->getDevice(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    VkClearColorValue clearColor = {{ 0.0f, 0.0f, 0.0f, 1.0f }};
    compositeTarget->beginPass(cmd, clearColor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, wmoTilePipeline);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &quadVB, &offset);
    for (size_t i = 0; i < pushes.size(); ++i) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                tilePipelineLayout, 0, 1,
                                &wmoTileDescSets_[frameIdx][i], 0, nullptr);
        vkCmdPushConstants(cmd, tilePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(MinimapTilePush), &pushes[i]);
        vkCmdDraw(cmd, 6, 1, 0, 0);
    }
    compositeTarget->endPass(cmd);

    lastUpdateTime = now;
    lastUpdatePos = centerWorldPos;
    hasCachedFrame = true;
    compositeIsWmo_ = true;
    wmoCompositeCenter_ = center;
    wmoCompositeSpanUsed_ = span;
    // The terrain pass checks its tile against these to skip a refresh; make
    // sure the way back out draws.
    lastCenterTileX = -1;
    lastCenterTileY = -1;
}

// --------------------------------------------------------
// Display quad (call INSIDE main render pass)
// --------------------------------------------------------

void Minimap::render(VkCommandBuffer cmd, const Camera& playerCamera,
                     const glm::vec3& centerWorldPos,
                     int screenWidth, int screenHeight,
                     float playerOrientation, bool hasPlayerOrientation) {
    if (!enabled || !hasCachedFrame || !displayPipeline) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, displayPipeline);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            displayPipelineLayout, 0, 1,
                            &displayDescSet, 0, nullptr);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &quadVB, &offset);

    // Top-right corner, unless something asked for a particular rect - which
    // is what happens when FrameXML owns the minimap and the map has to sit
    // inside the frame it drew.
    float margin = 10.0f;
    float pixelW, pixelH, x, y;
    if (haveRect_ && rectW_ > 0.0f && rectH_ > 0.0f) {
        pixelW = rectW_ / screenWidth;
        pixelH = rectH_ / screenHeight;
        x = rectX_ / screenWidth;
        y = rectY_ / screenHeight;   // y=0 is the top edge in Vulkan
    } else {
        pixelW = static_cast<float>(mapSize) / screenWidth;
        pixelH = static_cast<float>(mapSize) / screenHeight;
        x = 1.0f - pixelW - margin / screenWidth;
        y = margin / screenHeight;
    }

    // Compute player's UV in the composite texture
    constexpr float TILE_SIZE = core::coords::TILE_SIZE;
    auto [tileX, tileY] = core::coords::worldToTile(centerWorldPos.x, centerWorldPos.y);

    float fracNS = 32.0f - static_cast<float>(tileX) - centerWorldPos.y / TILE_SIZE;
    float fracEW = 32.0f - static_cast<float>(tileY) - centerWorldPos.x / TILE_SIZE;

    float playerU = (1.0f + fracEW) / 3.0f;
    float playerV = (1.0f + fracNS) / 3.0f;

    float zoomRadius = viewRadius / (TILE_SIZE * 3.0f);

    // A WMO composite is a square of its own around where the player was when
    // it was drawn, in the same orientation, so only the scale and the origin
    // differ. Whatever the last composite was decides it, not the mode: they
    // part for the one frame a doorway is crossed in.
    if (compositeIsWmo_) {
        const glm::vec2 uv = wmo_minimap::renderToComposite(
            glm::vec2(centerWorldPos.x, centerWorldPos.y), wmoCompositeCenter_, wmoCompositeSpanUsed_);
        playerU = uv.x;
        playerV = uv.y;
        zoomRadius = viewRadius / wmoCompositeSpanUsed_;
    }

    // Rotating with the camera is off everywhere: the saved setting is read and
    // dropped in loadSettings, since "Stabilize transports and correct minimap
    // orientation". This is why, as far as it can be worked out without the
    // client on screen.
    //
    // The two modes disagree by half a turn. Take the heading where
    // atan2(-fwd.x, fwd.y) is 0. North-up draws the arrow at pi - that same
    // expression, which is pi: pointing down, so that heading renders as south.
    // Rotating mode turns the map by the expression itself, which is 0 - no
    // rotation at all - while pinning the arrow up. So the map says the player
    // faces north and the arrow agrees, and both are half a turn from where
    // north-up puts them.
    //
    // North-up is the mode that has been looked at, so its arrow is the one to
    // trust: the map wants turning by the negative of where that arrow points,
    // not by the expression the arrow is built from.
    //
    // The direction is a second question and not settled here. What the shader
    // does with this angle is a mirror as well as a turn - the matrix it builds
    // has determinant -1 - so the map may also rotate the wrong way once the
    // half turn is accounted for. That mirror is right for north-up, where the
    // angle is zero, which is what makes this hard to reason about and easy to
    // check by turning it on and walking north.
    float rotation = 0.0f;
    if (rotateWithCamera) {
        glm::vec3 fwd = playerCamera.getForward();
        rotation = std::atan2(-fwd.x, fwd.y);
    }

    float arrowRotation = 0.0f;
    if (!rotateWithCamera) {
        if (hasPlayerOrientation) {
            arrowRotation = playerOrientation;
        } else {
            glm::vec3 fwd = playerCamera.getForward();
            arrowRotation = glm::pi<float>() - std::atan2(-fwd.x, fwd.y);
        }
    } else if (hasPlayerOrientation) {
        // Show character facing relative to the rotated map
        arrowRotation = playerOrientation + rotation;
    }

    MinimapDisplayPush push{};
    push.rect = glm::vec4(x, y, pixelW, pixelH);
    push.playerUV = glm::vec2(playerU, playerV);
    push.rotation = rotation;
    push.arrowRotation = arrowRotation;
    push.zoomRadius = zoomRadius;
    push.squareShape = squareShape ? 1 : 0;
    push.opacity = opacity_;

    vkCmdPushConstants(cmd, displayPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    vkCmdDraw(cmd, 6, 1, 0, 0);
}

} // namespace rendering
} // namespace wowee
