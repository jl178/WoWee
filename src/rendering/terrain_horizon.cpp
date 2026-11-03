#include "rendering/terrain_horizon.hpp"

#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <limits>

namespace wowee {
namespace rendering {

namespace st = stock_terrain;

namespace {
/// Tiles wholly past this many far clips draw the coarse level.
constexpr float kCoarseFrom = 2.0f;
}  // namespace

bool TerrainHorizon::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    vkCtx_ = ctx;
    perFrameLayout_ = perFrameLayout;
    if (!vkCtx_) return false;
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = sizeof(glm::vec4);
    layout_ = createPipelineLayout(vkCtx_->getDevice(), {perFrameLayout_}, {push});
    if (!layout_) return false;
    return buildPipeline();
}

VkPipeline TerrainHorizon::buildOne(const char* fragPath) {
    VkDevice device = vkCtx_->getDevice();
    VkShaderModule vert, frag;
    if (!vert.loadFromFile(device, "assets/shaders/terrain_horizon.vert.spv") ||
        !frag.loadFromFile(device, fragPath)) {
        LOG_WARNING("TerrainHorizon: shaders missing - no distant mountains");
        vert.destroy();
        frag.destroy();
        return VK_NULL_HANDLE;
    }
    VkVertexInputAttributeDescription pos{};
    pos.location = 0;
    pos.binding = 0;
    pos.format = VK_FORMAT_R32G32B32_SFLOAT;
    pos.offset = 0;
    VkPipeline built = PipelineBuilder()
        .setShaders(vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({perVertexBinding(sizeof(glm::vec3))}, {pos})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(layout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());
    vert.destroy();
    frag.destroy();
    return built;
}

bool TerrainHorizon::buildPipeline() {
    pipeline_ = buildOne("assets/shaders/terrain_horizon.frag.spv");
    farPipeline_ = buildOne("assets/shaders/terrain_horizon_far.frag.spv");
    return pipeline_ != VK_NULL_HANDLE && farPipeline_ != VK_NULL_HANDLE;
}

void TerrainHorizon::recreatePipeline() {
    if (!vkCtx_ || !layout_) return;
    destroy(vkCtx_->getDevice(), pipeline_);
    destroy(vkCtx_->getDevice(), farPipeline_);
    buildPipeline();
}

void TerrainHorizon::releaseBuffers() {
    if (!vkCtx_) return;
    AllocatedBuffer v = vertices_, i = indices_;
    vertices_ = {};
    indices_ = {};
    if (!v.buffer && !i.buffer) return;
    // Frames in flight may still draw the last map's horizon.
    VmaAllocator allocator = vkCtx_->getAllocator();
    vkCtx_->deferAfterAllFrameFences([allocator, v, i]() mutable {
        if (v.buffer) destroyBuffer(allocator, v);
        if (i.buffer) destroyBuffer(allocator, i);
    });
}

void TerrainHorizon::shutdown() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    if (vertices_.buffer) destroyBuffer(vkCtx_->getAllocator(), vertices_);
    if (indices_.buffer) destroyBuffer(vkCtx_->getAllocator(), indices_);
    destroy(device, pipeline_);
    destroy(device, farPipeline_);
    destroy(device, layout_);
    tiles_.clear();
    visible_.clear();
    vkCtx_ = nullptr;
}

void TerrainHorizon::setTiles(const std::vector<st::WdlTile>& wdl) {
    releaseBuffers();
    tiles_.clear();
    visible_.clear();
    visibleFar_.clear();
    if (!vkCtx_ || wdl.empty()) return;

    std::vector<glm::vec3> verts;
    std::vector<uint16_t> idx;
    verts.reserve(wdl.size() * 545);
    idx.reserve(wdl.size() * (16 * 16 * 12 + 8 * 8 * 6));
    tiles_.reserve(wdl.size());
    for (const auto& t : wdl) {
        Tile tile;
        tile.vertexOffset = static_cast<int32_t>(verts.size());
        tile.firstIndex = static_cast<uint32_t>(idx.size());
        st::wdlTileVertices(t, core::coords::TILE_SIZE, verts);
        st::wdlTileIndices(t, idx);
        tile.indexCount = static_cast<uint32_t>(idx.size()) - tile.firstIndex;
        if (tile.indexCount == 0) {
            verts.resize(static_cast<size_t>(tile.vertexOffset));
            continue;
        }
        tile.coarseFirst = static_cast<uint32_t>(idx.size());
        st::wdlTileIndicesCoarse(t, idx);
        tile.coarseCount = static_cast<uint32_t>(idx.size()) - tile.coarseFirst;
        glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
        for (size_t v = static_cast<size_t>(tile.vertexOffset); v < verts.size(); ++v) {
            lo = glm::min(lo, verts[v]);
            hi = glm::max(hi, verts[v]);
        }
        tile.center = (lo + hi) * 0.5f;
        tile.radius = glm::length(hi - lo) * 0.5f;
        tiles_.push_back(tile);
    }
    if (tiles_.empty()) return;
    vertices_ = uploadBuffer(*vkCtx_, verts.data(), verts.size() * sizeof(glm::vec3),
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    indices_ = uploadBuffer(*vkCtx_, idx.data(), idx.size() * sizeof(uint16_t),
                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!vertices_.buffer || !indices_.buffer) {
        LOG_WARNING("TerrainHorizon: upload failed - no distant mountains");
        releaseBuffers();
        tiles_.clear();
        return;
    }
    LOG_INFO("TerrainHorizon: ", tiles_.size(), " WDL tiles, ", verts.size(), " vertices, ",
             idx.size() / 3, " triangles");
}

void TerrainHorizon::cull(const Camera& camera, float farClip) {
    visible_.clear();
    visibleFar_.clear();
    if (tiles_.empty() || !pipeline_ || !farPipeline_) return;
    coarse_.resize(tiles_.size());
    depthRange_ = st::horizonDepthRange(farClip);
    const glm::mat4 view = camera.getViewMatrix();
    Frustum frustum;
    frustum.extractFromMatrix(camera.getProjectionMatrix() * view);
    const glm::vec3 cam = camera.getPosition();
    for (uint32_t i = 0; i < tiles_.size(); ++i) {
        const Tile& t = tiles_[i];
        const float d = glm::length(glm::vec2(t.center) - glm::vec2(cam));
        // Wholly inside the near limit, where the real ground is drawn, or
        // wholly past the far one (0x7cc0b0 picks by the same reach).
        if (d + t.radius < depthRange_.x || d - t.radius > depthRange_.y) continue;
        if (!frustum.intersectsSphere(t.center, t.radius)) continue;
        // Its nearest view depth: past the near limit, it needs no discard.
        const float nearDepth = -(view * glm::vec4(t.center, 1.0f)).z - t.radius;
        (nearDepth >= depthRange_.x ? visibleFar_ : visible_).push_back(i);
        // Past two far clips a tile is a few dozen pixels across, and the
        // full mesh's thousand triangles were most of what the horizon cost.
        coarse_[i] = d - t.radius > kCoarseFrom * farClip && t.coarseCount > 0;
    }
    // Nearest first, so the depth test turns away the ridges behind before
    // their fragments are shaded.
    const auto nearer = [&](uint32_t a, uint32_t b) {
        const glm::vec2 da = glm::vec2(tiles_[a].center) - glm::vec2(cam);
        const glm::vec2 db = glm::vec2(tiles_[b].center) - glm::vec2(cam);
        return glm::dot(da, da) < glm::dot(db, db);
    };
    std::sort(visible_.begin(), visible_.end(), nearer);
    std::sort(visibleFar_.begin(), visibleFar_.end(), nearer);
}

void TerrainHorizon::record(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) const {
    if ((visible_.empty() && visibleFar_.empty()) || !vertices_.buffer || !indices_.buffer) return;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &perFrameSet, 0,
                            nullptr);
    const glm::vec4 push(depthRange_, 0.0f, 0.0f);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices_.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, indices_.buffer, 0, VK_INDEX_TYPE_UINT16);
    // The ones reaching nearer than the limit first: they are the nearer.
    const auto draw = [&](VkPipeline p, const std::vector<uint32_t>& list) {
        if (list.empty()) return;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        for (uint32_t i : list) {
            const Tile& t = tiles_[i];
            if (coarse_[i]) {
                vkCmdDrawIndexed(cmd, t.coarseCount, 1, t.coarseFirst, t.vertexOffset, 0);
            } else {
                vkCmdDrawIndexed(cmd, t.indexCount, 1, t.firstIndex, t.vertexOffset, 0);
            }
        }
    };
    draw(pipeline_, visible_);
    draw(farPipeline_, visibleFar_);
}

}  // namespace rendering
}  // namespace wowee
