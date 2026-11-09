#include "rendering/terrain_horizon.hpp"

#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_texture.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace wowee {
namespace rendering {

namespace st = stock_terrain;
namespace ff = far_field;

/// The far field's atlas on the GPU, with the one descriptor set that binds
/// it; released whole once no frame can use it.
struct TerrainHorizon::AtlasGpu {
    VkTexture texture;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    ~AtlasGpu() {
        if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
        texture.destroy(device, allocator);
    }
};

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

VkPipeline TerrainHorizon::buildOne(const char* vertPath, const char* fragPath, bool lit,
                                    VkPipelineLayout layout) {
    VkDevice device = vkCtx_->getDevice();
    VkShaderModule vert, frag;
    if (!vert.loadFromFile(device, vertPath) || !frag.loadFromFile(device, fragPath)) {
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
    // The lit variant (E11) reads a packed normal from a second stream.
    VkVertexInputAttributeDescription nrm{};
    nrm.location = 1;
    nrm.binding = 1;
    nrm.format = VK_FORMAT_R8G8B8A8_SNORM;
    nrm.offset = 0;
    std::vector<VkVertexInputBindingDescription> bindings{perVertexBinding(sizeof(glm::vec3))};
    std::vector<VkVertexInputAttributeDescription> attributes{pos};
    if (lit) {
        bindings.push_back(perVertexBinding(sizeof(uint32_t), 1));
        attributes.push_back(nrm);
    }
    VkPipeline built = PipelineBuilder()
        .setShaders(vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput(bindings, attributes)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(layout ? layout : layout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());
    vert.destroy();
    frag.destroy();
    return built;
}

bool TerrainHorizon::buildPipeline() {
    pipeline_ = buildOne("assets/shaders/terrain_horizon.vert.spv",
                         "assets/shaders/terrain_horizon.frag.spv", false);
    farPipeline_ = buildOne("assets/shaders/terrain_horizon.vert.spv",
                            "assets/shaders/terrain_horizon_far.frag.spv", false);
    return pipeline_ != VK_NULL_HANDLE && farPipeline_ != VK_NULL_HANDLE;
}

void TerrainHorizon::recreatePipeline() {
    if (!vkCtx_ || !layout_) return;
    const bool hadLit = litPipeline_ != VK_NULL_HANDLE;
    const bool hadColor = colorPipeline_ != VK_NULL_HANDLE;
    destroy(vkCtx_->getDevice(), colorPipeline_);
    destroy(vkCtx_->getDevice(), colorFarPipeline_);
    destroy(vkCtx_->getDevice(), pipeline_);
    destroy(vkCtx_->getDevice(), farPipeline_);
    destroy(vkCtx_->getDevice(), litPipeline_);
    destroy(vkCtx_->getDevice(), litFarPipeline_);
    buildPipeline();
    if (hadLit || lit_ || farField_) {
        litFailed_ = false;
        ensureLit();
    }
    if (hadColor || farField_) {
        colorFailed_ = false;
        ensureColor();
    }
}

void TerrainHorizon::setLit(bool lit) {
    if (lit == lit_) return;
    lit_ = lit;
    if (lit) {
        litFailed_ = false;
        ensureLit();
    } else if (!farField_) {
        releaseLit();
    }
}

void TerrainHorizon::setFarField(bool on) {
    if (on == farField_) return;
    farField_ = on;
    if (on) {
        litFailed_ = false;
        colorFailed_ = false;
        ensureLit();
        ensureColor();
    } else if (!lit_) {
        releaseLit();
    }
}

bool TerrainHorizon::ensureColor() {
    if (colorFailed_ || !vkCtx_) return false;
    VkDevice device = vkCtx_->getDevice();
    if (!atlasSetLayout_) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = 1;
        ci.pBindings = &b;
        if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &atlasSetLayout_) != VK_SUCCESS) {
            atlasSetLayout_ = VK_NULL_HANDLE;
            colorFailed_ = true;
            return false;
        }
    }
    if (!colorLayout_) {
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        push.offset = 0;
        push.size = 2 * sizeof(glm::vec4);
        colorLayout_ = createPipelineLayout(device, {perFrameLayout_, atlasSetLayout_}, {push});
        if (!colorLayout_) {
            colorFailed_ = true;
            return false;
        }
    }
    if (!colorPipeline_ || !colorFarPipeline_) {
        colorPipeline_ = buildOne("assets/shaders/terrain_horizon_color.vert.spv",
                                  "assets/shaders/terrain_horizon_color.frag.spv", true, colorLayout_);
        colorFarPipeline_ = buildOne("assets/shaders/terrain_horizon_color.vert.spv",
                                     "assets/shaders/terrain_horizon_far_color.frag.spv", true, colorLayout_);
        if (!colorPipeline_ || !colorFarPipeline_) {
            LOG_WARNING("TerrainHorizon: far field shaders missing - the horizon stays as it was");
            colorFailed_ = true;
            return false;
        }
    }
    // WOWEE_FARFIELD_GAIN: the minimap colour's scale, for matching the
    // near ground by eye.
    if (const char* g = std::getenv("WOWEE_FARFIELD_GAIN"); g && *g) {
        colorGain_ = std::clamp(std::strtof(g, nullptr), 0.1f, 4.0f);
    }
    return true;
}

void TerrainHorizon::releaseAtlas() {
    if (!atlas_) return;
    std::shared_ptr<AtlasGpu> old = std::move(atlas_);
    atlas_.reset();
    atlasLayout_ = {};
    if (vkCtx_) vkCtx_->deferAfterAllFrameFences([old]() mutable { old.reset(); });
}

void TerrainHorizon::setAtlas(ff::Atlas atlas) {
    releaseAtlas();
    if (!vkCtx_ || atlas.empty()) return;
    if (!ensureColor()) return;
    auto gpu = std::make_shared<AtlasGpu>();
    gpu->device = vkCtx_->getDevice();
    gpu->allocator = vkCtx_->getAllocator();
    std::vector<const uint8_t*> data;
    std::vector<uint32_t> sizes;
    for (const auto& level : atlas.levels) {
        data.push_back(level.data());
        sizes.push_back(static_cast<uint32_t>(level.size()));
    }
    const auto w = static_cast<uint32_t>(atlas.layout.width());
    const auto h = static_cast<uint32_t>(atlas.layout.height());
    if (!gpu->texture.uploadMips(*vkCtx_, data.data(), sizes.data(), static_cast<uint32_t>(data.size()), w, h,
                                 VK_FORMAT_BC1_RGB_UNORM_BLOCK) ||
        !gpu->texture.createSampler(gpu->device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 4.0f)) {
        LOG_WARNING("TerrainHorizon: far field atlas upload failed");
        return;
    }
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(gpu->device, &pci, nullptr, &gpu->pool) != VK_SUCCESS) {
        gpu->pool = VK_NULL_HANDLE;
        return;
    }
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = gpu->pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &atlasSetLayout_;
    if (vkAllocateDescriptorSets(gpu->device, &ai, &gpu->set) != VK_SUCCESS) return;
    const VkDescriptorImageInfo info = gpu->texture.descriptorInfo();
    VkWriteDescriptorSet wr{};
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = gpu->set;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &info;
    vkUpdateDescriptorSets(gpu->device, 1, &wr, 0, nullptr);
    atlas_ = std::move(gpu);
    atlasLayout_ = atlas.layout;
    size_t bytes = 0;
    for (uint32_t sz : sizes) bytes += sz;
    LOG_INFO("TerrainHorizon: far field atlas ", w, "x", h, ", ", bytes / 1024, " KB");
}

void TerrainHorizon::releaseLit() {
    if (!vkCtx_) return;
    AllocatedBuffer nb = normals_, fnb = farNormals_;
    VkPipeline a = litPipeline_, b = litFarPipeline_;
    normals_ = {};
    farNormals_ = {};
    litPipeline_ = VK_NULL_HANDLE;
    litFarPipeline_ = VK_NULL_HANDLE;
    if (!nb.buffer && !fnb.buffer && !a && !b) return;
    VmaAllocator allocator = vkCtx_->getAllocator();
    VkDevice device = vkCtx_->getDevice();
    vkCtx_->deferAfterAllFrameFences([allocator, device, nb, fnb, a, b]() mutable {
        if (nb.buffer) destroyBuffer(allocator, nb);
        if (fnb.buffer) destroyBuffer(allocator, fnb);
        if (a) vkDestroyPipeline(device, a, nullptr);
        if (b) vkDestroyPipeline(device, b, nullptr);
    });
}

bool TerrainHorizon::ensureLit() {
    if (litFailed_ || !vkCtx_) return false;
    if (!litPipeline_ || !litFarPipeline_) {
        litPipeline_ = buildOne("assets/shaders/terrain_horizon_lit.vert.spv",
                                "assets/shaders/terrain_horizon_lit.frag.spv", true);
        litFarPipeline_ = buildOne("assets/shaders/terrain_horizon_lit.vert.spv",
                                   "assets/shaders/terrain_horizon_far_lit.frag.spv", true);
        if (!litPipeline_ || !litFarPipeline_) {
            LOG_WARNING("TerrainHorizon: lit shaders missing - the horizon stays flat");
            litFailed_ = true;
            return false;
        }
    }
    if (!normals_.buffer && !source_.empty()) {
        // One packed normal per vertex, in the order setTiles laid the
        // vertices out (tiles with no triangles were left out there too).
        std::vector<uint32_t> packed;
        packed.reserve(source_.size() * 545);
        std::vector<uint16_t> scratch;
        for (const auto& t : source_) {
            scratch.clear();
            st::wdlTileIndices(t, scratch);
            if (scratch.empty()) continue;
            horizon_detail::wdlTileNormals(t, core::coords::TILE_SIZE, packed);
        }
        normals_ = uploadBuffer(*vkCtx_, packed.data(), packed.size() * sizeof(uint32_t),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (!normals_.buffer) {
            litFailed_ = true;
            return false;
        }
    }
    if (!farNormals_.buffer && !far_.normals.empty()) {
        farNormals_ = uploadBuffer(*vkCtx_, far_.normals.data(), far_.normals.size() * sizeof(uint32_t),
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (!farNormals_.buffer) {
            litFailed_ = true;
            return false;
        }
    }
    return true;
}

void TerrainHorizon::releaseBuffers() {
    if (!vkCtx_) return;
    AllocatedBuffer v = vertices_, i = indices_, nb = normals_;
    vertices_ = {};
    indices_ = {};
    normals_ = {};
    if (!v.buffer && !i.buffer && !nb.buffer) return;
    // Frames in flight may still draw the last map's horizon.
    VmaAllocator allocator = vkCtx_->getAllocator();
    vkCtx_->deferAfterAllFrameFences([allocator, v, i, nb]() mutable {
        if (v.buffer) destroyBuffer(allocator, v);
        if (i.buffer) destroyBuffer(allocator, i);
        if (nb.buffer) destroyBuffer(allocator, nb);
    });
}

void TerrainHorizon::releaseFarBuffers() {
    if (!vkCtx_) return;
    AllocatedBuffer p = farPositions_, nb = farNormals_, i = farIndices_;
    farPositions_ = {};
    farNormals_ = {};
    farIndices_ = {};
    if (!p.buffer && !nb.buffer && !i.buffer) return;
    VmaAllocator allocator = vkCtx_->getAllocator();
    vkCtx_->deferAfterAllFrameFences([allocator, p, nb, i]() mutable {
        if (p.buffer) destroyBuffer(allocator, p);
        if (nb.buffer) destroyBuffer(allocator, nb);
        if (i.buffer) destroyBuffer(allocator, i);
    });
}

void TerrainHorizon::setFarMesh(horizon_detail::FarMesh mesh) {
    releaseFarBuffers();
    farVisible_.clear();
    far_ = std::move(mesh);
    if (!vkCtx_ || far_.objects.empty() || far_.positions.empty()) {
        far_ = {};
        return;
    }
    farPositions_ = uploadBuffer(*vkCtx_, far_.positions.data(), far_.positions.size() * sizeof(glm::vec3),
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    farIndices_ = uploadBuffer(*vkCtx_, far_.indices.data(), far_.indices.size() * sizeof(uint32_t),
                               VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!farPositions_.buffer || !farIndices_.buffer) {
        LOG_WARNING("TerrainHorizon: far WMO upload failed");
        releaseFarBuffers();
        far_ = {};
        return;
    }
    // The positions and indices live on the GPU now; the normals stay until
    // the lit horizon asks for them.
    far_.positions.clear();
    far_.positions.shrink_to_fit();
    far_.indices.clear();
    far_.indices.shrink_to_fit();
    LOG_INFO("TerrainHorizon: ", far_.objects.size(), " low-detail WMOs");
    if (lit_ || farField_) ensureLit();
}

void TerrainHorizon::shutdown() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    if (vertices_.buffer) destroyBuffer(vkCtx_->getAllocator(), vertices_);
    if (indices_.buffer) destroyBuffer(vkCtx_->getAllocator(), indices_);
    if (normals_.buffer) destroyBuffer(vkCtx_->getAllocator(), normals_);
    if (farPositions_.buffer) destroyBuffer(vkCtx_->getAllocator(), farPositions_);
    if (farNormals_.buffer) destroyBuffer(vkCtx_->getAllocator(), farNormals_);
    if (farIndices_.buffer) destroyBuffer(vkCtx_->getAllocator(), farIndices_);
    destroy(device, pipeline_);
    destroy(device, farPipeline_);
    destroy(device, litPipeline_);
    destroy(device, litFarPipeline_);
    destroy(device, colorPipeline_);
    destroy(device, colorFarPipeline_);
    destroy(device, colorLayout_);
    atlas_.reset();
    if (atlasSetLayout_) vkDestroyDescriptorSetLayout(device, atlasSetLayout_, nullptr);
    atlasSetLayout_ = VK_NULL_HANDLE;
    far_ = {};
    source_.clear();
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
    source_.clear();
    if (!vkCtx_ || wdl.empty()) return;
    // Kept for the lit horizon's normals (E11), made when it is first on.
    source_ = wdl;

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
    if (lit_ || farField_) ensureLit();
}

void TerrainHorizon::cull(const Camera& camera, float farClip) {
    visible_.clear();
    visibleFar_.clear();
    farVisible_.clear();
    // The lit pipelines and normals are made on the main thread (setLit,
    // setTiles, setFarMesh); this only looks.
    const bool normalsReady = litPipeline_ && litFarPipeline_ && (tiles_.empty() || normals_.buffer) &&
                              (far_.objects.empty() || farNormals_.buffer);
    drawLit_ = lit_ && farClip > 0.0f && normalsReady;
    // The far field: from where the chunks stop (by distance in the plane)
    // to its reach, once its atlas is here.
    drawColor_ = farField_ && farClip > 0.0f && normalsReady && colorPipeline_ && colorFarPipeline_ && atlas_;
    if (!pipeline_ || !farPipeline_) return;
    depthRange_ = drawColor_ ? glm::vec2(std::max(farClip - ff::kNearInset, 0.0f), std::max(ff::kReach, farClip))
                             : st::horizonDepthRange(farClip);
    farClip_ = farClip;
    // The low-detail WMOs, by the tiles' rule (reach and view).
    if (!far_.objects.empty() && farClip > 0.0f) {
        Frustum ff;
        ff.extractFromMatrix(camera.getProjectionMatrix() * camera.getViewMatrix());
        const glm::vec3 c = camera.getPosition();
        for (uint32_t i = 0; i < far_.objects.size(); ++i) {
            const auto& o = far_.objects[i];
            const float d = glm::length(glm::vec2(o.center) - glm::vec2(c));
            if (d + o.radius < depthRange_.x || d - o.radius > depthRange_.y) continue;
            if (!ff.intersectsSphere(o.center, o.radius)) continue;
            farVisible_.push_back(i);
        }
    }
    if (tiles_.empty()) return;
    coarse_.resize(tiles_.size());
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
        const float nearDepth = drawColor_ ? d - t.radius : -(view * glm::vec4(t.center, 1.0f)).z - t.radius;
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
    // WOWEE_FARFIELD_DEBUG=1: what the pass draws, every few seconds.
    static const bool kDebug = std::getenv("WOWEE_FARFIELD_DEBUG") != nullptr;
    static uint32_t debugFrame = 0;
    if (kDebug && (++debugFrame % 600) == 0) {
        LOG_INFO("TerrainHorizon: farField=", farField_, " color=", drawColor_, " lit=", drawLit_,
                 " atlas=", atlas_ != nullptr, " range=(", depthRange_.x, ", ", depthRange_.y, ") tiles ",
                 visible_.size(), " + ", visibleFar_.size(), ", wmos ", farVisible_.size());
    }
}

void TerrainHorizon::record(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) const {
    const bool tiles = (!visible_.empty() || !visibleFar_.empty()) && vertices_.buffer && indices_.buffer;
    const bool farWmos = !farVisible_.empty() && farPositions_.buffer && farIndices_.buffer;
    if (!tiles && !farWmos) return;
    // E11: the shaded pipelines, with the normals as a second stream.
    const bool color = drawColor_ && atlas_ && atlas_->set;
    const bool lit = color || (drawLit_ && litPipeline_ && litFarPipeline_);
    VkPipeline nearPipe = color ? colorPipeline_ : (lit ? litPipeline_ : pipeline_);
    VkPipeline farPipe = color ? colorFarPipeline_ : (lit ? litFarPipeline_ : farPipeline_);
    if (color) {
        const VkDescriptorSet sets[2] = {perFrameSet, atlas_->set};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, colorLayout_, 0, 2, sets, 0, nullptr);
        const glm::vec4 push[2] = {
            glm::vec4(depthRange_, farClip_, colorGain_),
            glm::vec4(static_cast<float>(atlasLayout_.minX), static_cast<float>(atlasLayout_.minY),
                      static_cast<float>(atlasLayout_.tilesW), static_cast<float>(atlasLayout_.tilesH))};
        vkCmdPushConstants(cmd, colorLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    } else {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &perFrameSet, 0,
                                nullptr);
        // z, which the stock shader does not read: the far clip, for the lit haze.
        const glm::vec4 push(depthRange_, farClip_, 0.0f);
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    }
    VkDeviceSize offset = 0;
    if (farWmos) {
        // The buildings first: they stand in front of the ground behind them.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nearPipe);
        vkCmdBindVertexBuffers(cmd, 0, 1, &farPositions_.buffer, &offset);
        if (lit) vkCmdBindVertexBuffers(cmd, 1, 1, &farNormals_.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, farIndices_.buffer, 0, VK_INDEX_TYPE_UINT32);
        for (uint32_t i : farVisible_) {
            const auto& o = far_.objects[i];
            vkCmdDrawIndexed(cmd, o.indexCount, 1, o.firstIndex, 0, 0);
        }
    }
    if (!tiles) return;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices_.buffer, &offset);
    if (lit) vkCmdBindVertexBuffers(cmd, 1, 1, &normals_.buffer, &offset);
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
    draw(nearPipe, visible_);
    draw(farPipe, visibleFar_);
}

}  // namespace rendering
}  // namespace wowee
