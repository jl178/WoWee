#include "rendering/overlay_system.hpp"
#include "core/coordinates.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_utils.hpp"
#include "core/logger.hpp"
#include "core/application.hpp"
#include "pipeline/asset_manager.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace wowee {
namespace rendering {

static_assert(OverlaySystem::kSelFrameSlices >= MAX_FRAMES_IN_FLIGHT,
              "one selection-lattice slice per frame in flight");

namespace {

// The stock selection circle, as Wow.exe 3.3.5a (12340) draws it.
//
//   Object_C 0x7460C0   loads Textures\UnitSelectTexture.blp into 0xCA12C4 and
//                       registers the ObjectSelectionCircle cvar (default "1").
//   WorldFrame 0x4F6F90 calls the target's vtable +0x6C (CGUnit_C 0x725980)
//                       once a frame; only the locked target (0xBD07B0) gets
//                       there, never the mouseover, never the active player.
//   0x725980            box = feet +/- R in x, y and z, R = unit +0xB0C;
//                       colour = vtable +0x78 (the UnitSelectionColor rules);
//                       texture matrix = Z rotation by -atan2(unit - camera)
//                       (0x744150), so the texture turns with the camera.
//   0x744EB0            texture stage 0 = UnitSelectTexture, blend mode 3
//                       (GxBlend_Add: src*srcAlpha + dst).
//   0x7E4370/0x7E3E80   the shadow-blob projector: re-draws the terrain chunks
//                       and the up-facing WMO triangles inside the box with
//                       planar texgen (u,v over the box's x,y after a further
//                       -90 degree turn, 0x7E2D60), a depth bias of 0.4, and a
//                       second stage indexed by height through the box - the
//                       "ShadowAdd" 64x8 ramp (0x7E36E0): alpha = 6t over the
//                       first sixth, 1, then falling to 0 over the last sixth.
//
// Vulkan cannot re-draw the world's own triangles here, so the decal is
// draped over a lattice of floor samples instead; the texture, its rotation,
// its blend, its height ramp and its size are the stock ones.
constexpr float kSelMinSpacing = 0.3f;    // yards between lattice points, at least
constexpr float kSelSurfaceLift = 0.04f;  // under a lattice cell the floor can bulge
constexpr int kSelCacheMaxAge = 240;      // frames before the floor is asked again
constexpr size_t kSelCacheMaxEntries = 4096;

struct SelCirclePush {
    glm::mat4 viewProj;
    glm::vec4 color;
    glm::vec4 center;  // feet x, y, z; box half-width
    glm::vec4 facing;  // camera->unit direction xy; surface lift; unused
};
static_assert(sizeof(SelCirclePush) == 112, "must match selection_circle.*.glsl");

uint64_t latticeKey(int32_t ix, int32_t iy) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(ix)) << 32) |
           static_cast<uint64_t>(static_cast<uint32_t>(iy));
}

}  // namespace

OverlaySystem::OverlaySystem(VkContext* ctx)
    : vkCtx_(ctx) {}

OverlaySystem::~OverlaySystem() {
    cleanup();
}

void OverlaySystem::cleanup() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    destroy(device, selCirclePipeline_);
    destroy(device, selCirclePipelineLayout_);
    destroy(vkCtx_->getAllocator(), selCircleVertBuf_, selCircleVertAlloc_);
    selCircleVertMapped_ = nullptr;
    destroy(vkCtx_->getAllocator(), selCircleIdxBuf_, selCircleIdxAlloc_);
    selCircleTexture_.destroy(device, vkCtx_->getAllocator());
    destroy(device, selCirclePool_);  // frees selCircleSet_ with it
    selCircleSet_ = VK_NULL_HANDLE;
    destroy(device, spellDecalPipeline_);
    destroy(vkCtx_->getAllocator(), spellDecalVertBuf_, spellDecalVertAlloc_);
    spellDecalVertMapped_ = nullptr;
    for (auto& t : spellDecalTexture_) t.destroy(device, vkCtx_->getAllocator());
    destroy(device, spellDecalPool_);  // frees spellDecalSet_ with it
    spellDecalSet_[0] = spellDecalSet_[1] = VK_NULL_HANDLE;
    spellDecalTexturesTried_ = false;
    destroy(device, selCircleSetLayout_);
    selCircleTextureTried_ = false;
    destroy(device, overlayPipeline_);
    destroy(device, brightnessPipeline_);
    destroy(device, overlayPipelineLayout_);
    destroy(device, blobPipeline_);
    destroy(device, blobPipelineLayout_);
    destroy(vkCtx_->getAllocator(), blobVertBuf_, blobVertAlloc_);
    blobVertMapped_ = nullptr;
    blobTexture_.destroy(device, vkCtx_->getAllocator());
    destroy(device, blobPool_);
    blobSet_ = VK_NULL_HANDLE;
    blobTextureTried_ = false;
}

void OverlaySystem::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    // Destroy only pipelines (keep geometry buffers)
    destroy(device, selCirclePipeline_);
    destroy(device, spellDecalPipeline_);
    destroy(device, overlayPipeline_);
    destroy(device, brightnessPipeline_);
    destroy(device, blobPipeline_);
}

void OverlaySystem::setSelectionCircle(const glm::vec3& pos, float radius, const glm::vec3& color,
                                       float alpha, float attackPulse) {
    selCirclePos_ = pos;
    selCircleRadius_ = std::max(radius, 0.05f);
    selCircleColor_ = color;
    selCircleAlpha_ = std::clamp(alpha, 0.0f, 1.0f);
    selPulseT_ = attackPulse < 1.0f ? attackPulse : -1.0f;
    selCircleVisible_ = true;
    // Here rather than at draw time: this runs from the UI update, outside
    // command recording, so the one-off upload's queue submit cannot meet a
    // secondary buffer being recorded on another thread.
    if (!selCircleTextureTried_) loadSelectionTexture();
}

bool OverlaySystem::initSelectionDescriptors() {
    if (selCircleSet_ != VK_NULL_HANDLE) return true;
    if (!vkCtx_) return false;
    VkDevice device = vkCtx_->getDevice();
    if (selCircleSetLayout_ == VK_NULL_HANDLE) {
        VkDescriptorSetLayoutBinding sampler{};
        sampler.binding = 0;
        sampler.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampler.descriptorCount = 1;
        sampler.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        selCircleSetLayout_ = createDescriptorSetLayout(device, {sampler});
        if (selCircleSetLayout_ == VK_NULL_HANDLE) return false;
    }
    if (selCirclePool_ == VK_NULL_HANDLE) {
        VkDescriptorPoolSize size{};
        size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        size.descriptorCount = 1;
        VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        if (vkCreateDescriptorPool(device, &pool, nullptr, &selCirclePool_) != VK_SUCCESS) {
            selCirclePool_ = VK_NULL_HANDLE;
            return false;
        }
    }
    VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = selCirclePool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &selCircleSetLayout_;
    if (vkAllocateDescriptorSets(device, &alloc, &selCircleSet_) != VK_SUCCESS) {
        selCircleSet_ = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void OverlaySystem::loadSelectionTexture() {
    selCircleTextureTried_ = true;
    if (!vkCtx_ || !initSelectionDescriptors()) return;
    VkDevice device = vkCtx_->getDevice();

    // The stock file: 256x256, white, the ring in alpha at 0.88 of the
    // half-width, a soft fill inward and a brighter lip along the bottom edge,
    // which the camera-relative rotation keeps nearest the viewer.
    bool uploaded = false;
    if (auto* assets = core::Application::getInstance().getAssetManager()) {
        pipeline::BLPImage image = assets->loadTexture("Textures\\UnitSelectTexture.blp");
        if (image.isValid() && !image.data.empty()) {
            uploaded = selCircleTexture_.upload(*vkCtx_, image.data.data(),
                                                static_cast<uint32_t>(image.width),
                                                static_cast<uint32_t>(image.height),
                                                VK_FORMAT_R8G8B8A8_UNORM, true);
        }
    }
    if (!uploaded) {
        // No client data: a stand-in with the same radial profile, so the
        // circle still sits where the stock one would.
        LOG_WARNING("OverlaySystem: Textures\\UnitSelectTexture.blp unavailable, using a generated ring");
        constexpr int N = 128;
        std::vector<uint8_t> pixels(N * N * 4, 255);
        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                const float dx = (x + 0.5f) / N * 2.0f - 1.0f;
                const float dy = (y + 0.5f) / N * 2.0f - 1.0f;
                const float r = std::sqrt(dx * dx + dy * dy);
                const float ring = std::exp(-std::pow((r - 0.88f) / 0.025f, 2.0f));
                const float fill = r < 0.88f ? std::clamp((r - 0.3f) / 0.58f, 0.0f, 1.0f) * 0.17f : 0.0f;
                const float lip = 0.6f + 0.4f * std::clamp(dy, 0.0f, 1.0f);
                const float a = r > 0.95f ? 0.0f : std::min(1.0f, (ring + fill) * lip);
                pixels[(y * N + x) * 4 + 3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
            }
        }
        uploaded = selCircleTexture_.upload(*vkCtx_, pixels.data(), N, N,
                                            VK_FORMAT_R8G8B8A8_UNORM, true);
    }
    if (!uploaded) return;
    selCircleTexture_.createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    if (!selCircleTexture_.isValid()) return;  // never write an unsampleable texture (#123)

    VkDescriptorImageInfo imageInfo = selCircleTexture_.descriptorInfo();
    VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = selCircleSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void OverlaySystem::clearSelectionCircle() {
    selCircleVisible_ = false;
}

void OverlaySystem::initSelectionCircle() {
    if (selCirclePipeline_ != VK_NULL_HANDLE) return;
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();

    // Load shaders
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/selection_circle.vert.spv")) {
        LOG_ERROR("OverlaySystem: failed to load selection circle vertex shader");
        return;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/selection_circle.frag.spv")) {
        LOG_ERROR("OverlaySystem: failed to load selection circle fragment shader");
        vertShader.destroy();
        return;
    }

    if (!initSelectionDescriptors()) {
        LOG_ERROR("OverlaySystem: failed to create selection circle descriptors");
        vertShader.destroy();
        fragShader.destroy();
        return;
    }

    // Set 0: the selection texture. Push block: SelCirclePush, both stages.
    if (selCirclePipelineLayout_ == VK_NULL_HANDLE) {
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcRange.offset = 0;
        pcRange.size = sizeof(SelCirclePush);
        selCirclePipelineLayout_ = createPipelineLayout(device, {selCircleSetLayout_}, {pcRange});
    }

    // Vertex input: binding 0, stride 12, world-space vec3 at location 0
    VkVertexInputBindingDescription vertBind{.binding = 0, .stride = 12, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription vertAttr{.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0};

    if (selCircleVertBuf_ == VK_NULL_HANDLE) {
        // The lattice's triangles never change; only its heights do.
        std::vector<uint16_t> indices;
        indices.reserve(kSelGridCells * kSelGridCells * 6);
        for (int y = 0; y < kSelGridCells; ++y) {
            for (int x = 0; x < kSelGridCells; ++x) {
                const auto i0 = static_cast<uint16_t>(y * kSelGridVerts + x);
                const auto i1 = static_cast<uint16_t>(i0 + 1);
                const auto i2 = static_cast<uint16_t>(i0 + kSelGridVerts);
                const auto i3 = static_cast<uint16_t>(i2 + 1);
                indices.insert(indices.end(), {i0, i1, i3, i0, i3, i2});
            }
        }
        selCircleVertCount_ = static_cast<int>(indices.size());
        AllocatedBuffer ibuf = uploadBuffer(*vkCtx_, indices.data(),
            indices.size() * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        selCircleIdxBuf_ = ibuf.buffer;
        selCircleIdxAlloc_ = ibuf.allocation;

        const VkDeviceSize sliceBytes = sizeof(float) * 3 * kSelGridVerts * kSelGridVerts;
        AllocatedBuffer vbuf = createBuffer(vkCtx_->getAllocator(), sliceBytes * kSelFrameSlices,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        selCircleVertBuf_ = vbuf.buffer;
        selCircleVertAlloc_ = vbuf.allocation;
        selCircleVertMapped_ = vbuf.info.pMappedData;
    }

    // Build pipeline. Tested against the scene's depth without writing it and
    // pulled toward the camera, as the stock projector biases its re-drawn
    // floor (0.4); additive, as GxBlend_Add.
    selCirclePipeline_ = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({vertBind}, {vertAttr})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setDepthBias(-1.0f, -1.0f)
        .setColorBlendAttachment(PipelineBuilder::blendAdditive())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(selCirclePipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());

    vertShader.destroy();
    fragShader.destroy();

    if (!selCirclePipeline_) {
        LOG_ERROR("OverlaySystem: failed to build selection circle pipeline");
    }
}

void OverlaySystem::buildFloorLattice(FloorLattice& lattice, const glm::vec3& center, float r,
                                      float zHalf, const HeightQuery2D& terrainHeight,
                                      const HeightQuery3D& wmoHeight,
                                      const HeightQuery3D& m2Height) {
    // Lattice spacing: the box (2R) plus a cell of slack fits in the grid
    // wherever the lattice happens to fall, never finer than kSelMinSpacing.
    const float spacing = std::max(kSelMinSpacing,
                                   2.0f * r / static_cast<float>(kSelGridCells - 2));
    // WMO/M2 floors are found by a downward probe; start it at the top of the
    // box so a step or a deck up to the box's top is still a floor.
    const float probeZ = center.z + zHalf + 0.25f;
    if (spacing != lattice.spacing || std::abs(probeZ - lattice.probeZ) > 0.35f ||
        ++lattice.age > kSelCacheMaxAge || lattice.cache.size() > kSelCacheMaxEntries) {
        lattice.cache.clear();
        lattice.spacing = spacing;
        lattice.probeZ = probeZ;
        lattice.age = 0;
    }

    const auto originX = static_cast<int32_t>(std::floor((center.x - r) / spacing));
    const auto originY = static_cast<int32_t>(std::floor((center.y - r) / spacing));
    // Below the box: the shader's height ramp is zero there, so a lattice point
    // with no floor inside the box draws nothing.
    const float noFloorZ = center.z - 2.0f * zHalf - 1.0f;

    lattice.verts.resize(static_cast<size_t>(kSelGridVerts) * kSelGridVerts * 3);
    size_t out = 0;
    for (int gy = 0; gy < kSelGridVerts; ++gy) {
        for (int gx = 0; gx < kSelGridVerts; ++gx) {
            const int32_t ix = originX + gx;
            const int32_t iy = originY + gy;
            const float wx = static_cast<float>(ix) * spacing;
            const float wy = static_cast<float>(iy) * spacing;
            auto [it, inserted] = lattice.cache.try_emplace(latticeKey(ix, iy));
            if (inserted) {
                FloorSample& fs = it->second;
                if (terrainHeight) fs.terrain = terrainHeight(wx, wy);
                if (wmoHeight) fs.wmo = wmoHeight(wx, wy, lattice.probeZ);
                if (m2Height) fs.m2 = m2Height(wx, wy, lattice.probeZ);
            }
            // The highest floor inside the box: from above, as the stock
            // projector sees it, the top surface is the one left showing.
            float z = noFloorZ;
            bool found = false;
            for (const std::optional<float>& h : {it->second.terrain, it->second.wmo, it->second.m2}) {
                if (!h || *h < center.z - zHalf || *h > center.z + zHalf) continue;
                if (!found || *h > z) z = *h;
                found = true;
            }
            lattice.verts[out++] = wx;
            lattice.verts[out++] = wy;
            lattice.verts[out++] = z;
        }
    }
}

namespace {

struct BlobPush {
    glm::mat4 viewProj;
    glm::vec4 feet;    // xyz the model origin, w = h
    glm::vec4 axes;    // xy the facing, z half-width along it, w across it
    glm::vec4 extra;   // x model alpha, y surface lift
};
static_assert(sizeof(BlobPush) == 112, "must match unit_blob.*.glsl");

constexpr float kBlobSpacing = 0.5f;   // yards between floor samples, at least
constexpr int kBlobMaxCells = 18;      // per side, per blob
constexpr int kBlobCacheMaxAge = 240;  // frames before the floor is asked again
constexpr size_t kBlobCacheMaxEntries = 16384;

uint64_t blobKey(int32_t ix, int32_t iy, int32_t probe, int32_t spacingCm) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(ix) & 0xFFFFFu) << 44) |
           (static_cast<uint64_t>(static_cast<uint32_t>(iy) & 0xFFFFFu) << 24) |
           (static_cast<uint64_t>(static_cast<uint32_t>(probe) & 0xFFFu) << 12) |
           static_cast<uint64_t>(static_cast<uint32_t>(spacingCm) & 0xFFFu);
}

}  // namespace

void OverlaySystem::prepareUnitBlobs() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    if (!blobTextureTried_) {
        blobTextureTried_ = true;
        // 32x32, grey 160/255 inside six texels of the centre, rising to white
        // at twelve (docs/re/characters.md section 6). Only its colour is read.
        bool uploaded = false;
        if (auto* assets = core::Application::getInstance().getAssetManager()) {
            pipeline::BLPImage image = assets->loadTexture("Textures\\ShadowBlob.blp");
            if (image.isValid() && !image.data.empty()) {
                uploaded = blobTexture_.upload(*vkCtx_, image.data.data(),
                                               static_cast<uint32_t>(image.width),
                                               static_cast<uint32_t>(image.height),
                                               VK_FORMAT_R8G8B8A8_UNORM, true);
            }
        }
        if (!uploaded) {
            LOG_WARNING("OverlaySystem: Textures\\ShadowBlob.blp unavailable, using a generated blob");
            constexpr int N = 32;
            std::vector<uint8_t> px(N * N * 4, 255);
            for (int y = 0; y < N; ++y) {
                for (int x = 0; x < N; ++x) {
                    const float r = std::hypot(x + 0.5f - 16.0f, y + 0.5f - 16.0f);
                    const float g = std::clamp(160.0f + (r - 6.0f) * 15.9f, 160.0f, 255.0f);
                    for (int c = 0; c < 3; ++c) px[(y * N + x) * 4 + c] = static_cast<uint8_t>(g);
                }
            }
            uploaded = blobTexture_.upload(*vkCtx_, px.data(), N, N, VK_FORMAT_R8G8B8A8_UNORM, true);
        }
        if (uploaded) {
            blobTexture_.createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                       VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        }
    }
    if (!blobTexture_.isValid() || !initSelectionDescriptors()) return;
    if (blobSet_ == VK_NULL_HANDLE) {
        VkDescriptorPoolSize size{};
        size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        size.descriptorCount = 1;
        VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        if (vkCreateDescriptorPool(device, &pool, nullptr, &blobPool_) != VK_SUCCESS) {
            blobPool_ = VK_NULL_HANDLE;
            return;
        }
        VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = blobPool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &selCircleSetLayout_;
        if (vkAllocateDescriptorSets(device, &alloc, &blobSet_) != VK_SUCCESS) {
            blobSet_ = VK_NULL_HANDLE;
            return;
        }
        VkDescriptorImageInfo imageInfo = blobTexture_.descriptorInfo();
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = blobSet_;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
    if (blobVertBuf_ == VK_NULL_HANDLE) {
        const VkDeviceSize sliceBytes = sizeof(float) * 3 * kBlobMaxVerts;
        AllocatedBuffer vbuf = createBuffer(vkCtx_->getAllocator(), sliceBytes * kSelFrameSlices,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        blobVertBuf_ = vbuf.buffer;
        blobVertAlloc_ = vbuf.allocation;
        blobVertMapped_ = vbuf.info.pMappedData;
    }
    if (blobPipelineLayout_ == VK_NULL_HANDLE) {
        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pc.size = sizeof(BlobPush);
        blobPipelineLayout_ = createPipelineLayout(device, {selCircleSetLayout_}, {pc});
    }
    if (blobPipeline_ == VK_NULL_HANDLE && blobPipelineLayout_ != VK_NULL_HANDLE) {
        VkShaderModule vs, fs;
        if (!vs.loadFromFile(device, "assets/shaders/unit_blob.vert.spv")) return;
        if (!fs.loadFromFile(device, "assets/shaders/unit_blob.frag.spv")) { vs.destroy(); return; }
        VkVertexInputBindingDescription bind{.binding = 0, .stride = 12, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attr{.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0};
        // GxBlend_Mod (dst *= src), the framebuffer's alpha left alone; depth
        // tested and not written, biased toward the camera like the stock
        // projector's re-drawn floor.
        VkPipelineColorBlendAttachmentState mod{};
        mod.blendEnable = VK_TRUE;
        mod.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        mod.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        mod.colorBlendOp = VK_BLEND_OP_ADD;
        mod.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        mod.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        mod.alphaBlendOp = VK_BLEND_OP_ADD;
        mod.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blobPipeline_ = PipelineBuilder()
            .setShaders(vs.stageInfo(VK_SHADER_STAGE_VERTEX_BIT), fs.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({bind}, {attr})
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
            .setDepthBias(-1.0f, -1.0f)
            .setColorBlendAttachment(mod)
            .setMultisample(vkCtx_->getMsaaSamples())
            .setLayout(blobPipelineLayout_)
            .setRenderPass(vkCtx_->getImGuiRenderPass())
            .setDynamicStates(viewportAndScissorDynamic())
            .build(device, vkCtx_->getPipelineCache());
        vs.destroy();
        fs.destroy();
        if (!blobPipeline_) LOG_ERROR("OverlaySystem: failed to build the unit blob pipeline");
    }
}

void OverlaySystem::renderUnitBlobs(const glm::mat4& viewProj, VkCommandBuffer cmd,
                                    const HeightQuery2D& terrainHeight,
                                    const HeightQuery3D& wmoHeight) {
    if (unitBlobs_.empty() || cmd == VK_NULL_HANDLE) return;
    if (blobPipeline_ == VK_NULL_HANDLE || blobSet_ == VK_NULL_HANDLE || !blobVertMapped_) return;

    if (++blobFloorAge_ > kBlobCacheMaxAge || blobFloorCache_.size() > kBlobCacheMaxEntries) {
        blobFloorCache_.clear();
        blobFloorAge_ = 0;
    }
    // Each blob is draped over a world-aligned lattice of floor samples
    // (terrain and WMO floors: the stock receivers; doodads are not), as the
    // selection circle is. Vulkan cannot re-draw the world's own triangles.
    struct Range { uint32_t first, count; size_t blob; };
    std::vector<Range> ranges;
    blobVerts_.clear();
    for (size_t bi = 0; bi < unitBlobs_.size(); ++bi) {
        const UnitBlob& b = unitBlobs_[bi];
        if (b.alpha <= 0.0f) continue;
        const float r = std::sqrt(b.halfX * b.halfX + b.halfY * b.halfY);
        const float spacing = std::max(kBlobSpacing, 2.0f * r / static_cast<float>(kBlobMaxCells - 1));
        const int cells = std::clamp(static_cast<int>(std::ceil(2.0f * r / spacing)) + 1, 2, kBlobMaxCells);
        const uint32_t need = static_cast<uint32_t>(cells * cells * 6);
        if (blobVerts_.size() / 3 + need > kBlobMaxVerts) break;
        const float zMin = b.feet.z - 1.6667f * b.halfZ;
        const float zMax = b.feet.z + b.halfZ;
        const float probeZ = zMax + 0.25f;
        const auto probeBucket = static_cast<int32_t>(std::floor(probeZ / 2.0f));
        const float probeAt = (static_cast<float>(probeBucket) + 1.0f) * 2.0f;
        const auto spacingCm = static_cast<int32_t>(std::lround(spacing * 100.0f));
        const auto ox = static_cast<int32_t>(std::floor((b.feet.x - r) / spacing));
        const auto oy = static_cast<int32_t>(std::floor((b.feet.y - r) / spacing));
        const float noFloor = zMin - 1.0f;
        std::vector<float> zs(static_cast<size_t>((cells + 1) * (cells + 1)));
        for (int gy = 0; gy <= cells; ++gy) {
            for (int gx = 0; gx <= cells; ++gx) {
                const int32_t ix = ox + gx, iy = oy + gy;
                const float wx = static_cast<float>(ix) * spacing;
                const float wy = static_cast<float>(iy) * spacing;
                auto [it, inserted] = blobFloorCache_.try_emplace(blobKey(ix, iy, probeBucket, spacingCm));
                if (inserted) {
                    if (terrainHeight) it->second.terrain = terrainHeight(wx, wy);
                    if (wmoHeight) it->second.wmo = wmoHeight(wx, wy, probeAt);
                }
                float z = noFloor;
                bool found = false;
                for (const std::optional<float>& h : {it->second.terrain, it->second.wmo}) {
                    if (!h || *h < zMin || *h > zMax) continue;
                    if (!found || *h > z) z = *h;
                    found = true;
                }
                zs[static_cast<size_t>(gy * (cells + 1) + gx)] = z;
            }
        }
        const uint32_t first = static_cast<uint32_t>(blobVerts_.size() / 3);
        const auto vtx = [&](int gx, int gy) {
            blobVerts_.push_back(static_cast<float>(ox + gx) * spacing);
            blobVerts_.push_back(static_cast<float>(oy + gy) * spacing);
            blobVerts_.push_back(zs[static_cast<size_t>(gy * (cells + 1) + gx)]);
        };
        for (int gy = 0; gy < cells; ++gy) {
            for (int gx = 0; gx < cells; ++gx) {
                vtx(gx, gy); vtx(gx + 1, gy); vtx(gx + 1, gy + 1);
                vtx(gx, gy); vtx(gx + 1, gy + 1); vtx(gx, gy + 1);
            }
        }
        ranges.push_back({first, need, bi});
    }
    if (ranges.empty()) return;

    const uint32_t slice = vkCtx_->getCurrentFrame() % kSelFrameSlices;
    const VkDeviceSize sliceBytes = sizeof(float) * 3 * kBlobMaxVerts;
    const VkDeviceSize bytes = sizeof(float) * blobVerts_.size();
    std::memcpy(static_cast<uint8_t*>(blobVertMapped_) + slice * sliceBytes, blobVerts_.data(), bytes);
    vmaFlushAllocation(vkCtx_->getAllocator(), blobVertAlloc_, slice * sliceBytes, bytes);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blobPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blobPipelineLayout_,
                            0, 1, &blobSet_, 0, nullptr);
    const VkDeviceSize offset = slice * sliceBytes;
    vkCmdBindVertexBuffers(cmd, 0, 1, &blobVertBuf_, &offset);
    for (const Range& rg : ranges) {
        const UnitBlob& b = unitBlobs_[rg.blob];
        BlobPush push{};
        push.viewProj = viewProj;
        push.feet = glm::vec4(b.feet, b.halfZ);
        push.axes = glm::vec4(b.forward, b.halfX, b.halfY);
        push.extra = glm::vec4(std::clamp(b.alpha, 0.0f, 1.0f), kSelSurfaceLift, 0.0f, 0.0f);
        vkCmdPushConstants(cmd, blobPipelineLayout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, rg.count, 1, rg.first, 0);
    }
}

void OverlaySystem::renderSelectionCircle(const glm::mat4& view, const glm::mat4& projection,
                                           VkCommandBuffer cmd,
                                           const HeightQuery2D& terrainHeight,
                                           const HeightQuery3D& wmoHeight,
                                           const HeightQuery3D& m2Height) {
    // The blobs first: stock draws them with the world, before any model.
    renderUnitBlobs(projection * view, cmd, terrainHeight, wmoHeight);
    renderSelectionCircleOnly(view, projection, cmd, terrainHeight, wmoHeight, m2Height);
    renderSpellTargetDecal(view, projection, cmd, terrainHeight, wmoHeight, m2Height);
}

void OverlaySystem::renderSelectionCircleOnly(const glm::mat4& view, const glm::mat4& projection,
                                              VkCommandBuffer cmd,
                                              const HeightQuery2D& terrainHeight,
                                              const HeightQuery3D& wmoHeight,
                                              const HeightQuery3D& m2Height) {
    if (!selCircleVisible_) return;
    initSelectionCircle();
    if (selCirclePipeline_ == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE ||
        selCircleVertMapped_ == nullptr || selCircleSet_ == VK_NULL_HANDLE ||
        !selCircleTexture_.isValid()) {
        return;
    }

    const glm::vec3 feet = selCirclePos_;
    // The attack pulse (0x725980, the second draw): over 500 ms its box
    // closes from 1.6R + 2.3 to R along (1 - t)^3, and its alpha - the
    // circle's, times 1 - (1 - t)^3 - fades in as fast and, past halfway,
    // back out by 1 - (2t - 1)^2.
    const float rMain = selCircleRadius_;
    const float rPulseMax = 1.6f * rMain + 2.3f;
    float rPulse = 0.0f, pulseAlpha = 0.0f;
    if (selPulseT_ >= 0.0f) {
        const float t = selPulseT_;
        const float k = (1.0f - t) * (1.0f - t) * (1.0f - t);
        rPulse = rMain + (rPulseMax - rMain) * k;
        pulseAlpha = 1.0f - k;
        if (t > 0.5f) pulseAlpha *= 1.0f - (2.0f * t - 1.0f) * (2.0f * t - 1.0f);
    }
    // The lattice covers the pulse's largest box for as long as it runs, so
    // its spacing (and the floor cache) holds still while the ring closes;
    // both decals are draped over the same floor samples.
    const float r = selPulseT_ >= 0.0f ? rPulseMax : rMain;
    buildFloorLattice(selLattice_, feet, r, r, terrainHeight, wmoHeight, m2Height);

    const uint32_t slice = vkCtx_->getCurrentFrame() % kSelFrameSlices;
    const VkDeviceSize sliceBytes = sizeof(float) * selLattice_.verts.size();
    std::memcpy(static_cast<uint8_t*>(selCircleVertMapped_) + slice * sliceBytes,
                selLattice_.verts.data(), sliceBytes);
    vmaFlushAllocation(vkCtx_->getAllocator(), selCircleVertAlloc_, slice * sliceBytes, sliceBytes);

    // The texture turns with the camera (0x744150): its bright lower lip is
    // kept on the side of the ring nearest the viewer.
    const glm::vec3 camPos = glm::vec3(glm::inverse(view)[3]);
    glm::vec2 facing(feet.x - camPos.x, feet.y - camPos.y);
    const float facingLen = glm::length(facing);
    facing = facingLen > 1e-4f ? facing / facingLen : glm::vec2(1.0f, 0.0f);

    SelCirclePush push{};
    push.viewProj = projection * view;
    push.color = glm::vec4(selCircleColor_, selCircleAlpha_);
    push.center = glm::vec4(feet, rMain);
    push.facing = glm::vec4(facing, kSelSurfaceLift, 0.0f);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, selCirclePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, selCirclePipelineLayout_,
                            0, 1, &selCircleSet_, 0, nullptr);
    const VkDeviceSize offset = slice * sliceBytes;
    vkCmdBindVertexBuffers(cmd, 0, 1, &selCircleVertBuf_, &offset);
    vkCmdBindIndexBuffer(cmd, selCircleIdxBuf_, 0, VK_INDEX_TYPE_UINT16);
    vkCmdPushConstants(cmd, selCirclePipelineLayout_,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(push), &push);
    vkCmdDrawIndexed(cmd, static_cast<uint32_t>(selCircleVertCount_), 1, 0, 0, 0);
    if (pulseAlpha > 0.0f) {
        push.color.a = selCircleAlpha_ * pulseAlpha;
        push.center.w = rPulse;
        vkCmdPushConstants(cmd, selCirclePipelineLayout_,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(push), &push);
        vkCmdDrawIndexed(cmd, static_cast<uint32_t>(selCircleVertCount_), 1, 0, 0, 0);
    }
}

void OverlaySystem::setSpellTargetDecal(const glm::vec3& center, float halfWidth, bool acceptable) {
    spellDecalCenter_ = center;
    spellDecalHalfWidth_ = std::max(halfWidth, 0.05f);
    spellDecalAcceptable_ = acceptable;
    spellDecalVisible_ = true;
    // Outside command recording, for the reason setSelectionCircle gives.
    if (!spellDecalTexturesTried_) loadSpellTargetTextures();
}

void OverlaySystem::clearSpellTargetDecal() {
    spellDecalVisible_ = false;
}

void OverlaySystem::loadSpellTargetTextures() {
    spellDecalTexturesTried_ = true;
    if (!vkCtx_ || !initSelectionDescriptors()) return;
    VkDevice device = vkCtx_->getDevice();
    if (spellDecalPool_ == VK_NULL_HANDLE) {
        VkDescriptorPoolSize size{};
        size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        size.descriptorCount = 2;
        VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 2;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        if (vkCreateDescriptorPool(device, &pool, nullptr, &spellDecalPool_) != VK_SUCCESS) {
            spellDecalPool_ = VK_NULL_HANDLE;
            return;
        }
    }
    // The pair 0xAC799C names, loaded by WorldFrame's constructor (0x4FABD0)
    // into 0xB74350/0xB74354: index 0 in range, 1 out of it.
    static const char* const kPaths[2] = {
        "Interface\\SpellShadow\\Spell-Shadow-Acceptable.blp",
        "Interface\\SpellShadow\\Spell-Shadow-Unacceptable.blp",
    };
    auto* assets = core::Application::getInstance().getAssetManager();
    for (int i = 0; i < 2; ++i) {
        bool uploaded = false;
        if (assets) {
            pipeline::BLPImage image = assets->loadTexture(kPaths[i]);
            if (image.isValid() && !image.data.empty()) {
                uploaded = spellDecalTexture_[i].upload(*vkCtx_, image.data.data(),
                                                        static_cast<uint32_t>(image.width),
                                                        static_cast<uint32_t>(image.height),
                                                        VK_FORMAT_R8G8B8A8_UNORM, true);
            }
        }
        if (!uploaded) {
            LOG_WARNING("OverlaySystem: ", kPaths[i], " unavailable - no spell targeting circle");
            continue;
        }
        spellDecalTexture_[i].createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        if (!spellDecalTexture_[i].isValid()) continue;
        VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = spellDecalPool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &selCircleSetLayout_;
        if (vkAllocateDescriptorSets(device, &alloc, &spellDecalSet_[i]) != VK_SUCCESS) {
            spellDecalSet_[i] = VK_NULL_HANDLE;
            continue;
        }
        VkDescriptorImageInfo imageInfo = spellDecalTexture_[i].descriptorInfo();
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = spellDecalSet_[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
}

void OverlaySystem::initSpellTargetDecal() {
    if (spellDecalPipeline_ != VK_NULL_HANDLE) return;
    // The selection circle's layout, indices and shaders.
    initSelectionCircle();
    if (!vkCtx_ || selCirclePipelineLayout_ == VK_NULL_HANDLE) return;
    VkDevice device = vkCtx_->getDevice();

    if (spellDecalVertBuf_ == VK_NULL_HANDLE) {
        const VkDeviceSize sliceBytes = sizeof(float) * 3 * kSelGridVerts * kSelGridVerts;
        AllocatedBuffer vbuf = createBuffer(vkCtx_->getAllocator(), sliceBytes * kSelFrameSlices,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        spellDecalVertBuf_ = vbuf.buffer;
        spellDecalVertAlloc_ = vbuf.allocation;
        spellDecalVertMapped_ = vbuf.info.pMappedData;
    }

    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/selection_circle.vert.spv")) return;
    if (!fragShader.loadFromFile(device, "assets/shaders/selection_circle.frag.spv")) {
        vertShader.destroy();
        return;
    }
    VkVertexInputBindingDescription vertBind{.binding = 0, .stride = 12, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription vertAttr{.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0};
    // As the selection circle's, but blended by alpha: 0x4F8A40 sets render
    // state 6 (the blend mode) to 2, GxBlend_Alpha, with the 0.4 depth bias.
    spellDecalPipeline_ = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({vertBind}, {vertAttr})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setDepthBias(-1.0f, -1.0f)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(selCirclePipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());
    vertShader.destroy();
    fragShader.destroy();
    if (!spellDecalPipeline_) LOG_ERROR("OverlaySystem: failed to build spell targeting pipeline");
}

void OverlaySystem::renderSpellTargetDecal(const glm::mat4& view, const glm::mat4& projection,
                                           VkCommandBuffer cmd,
                                           const HeightQuery2D& terrainHeight,
                                           const HeightQuery3D& wmoHeight,
                                           const HeightQuery3D& m2Height) {
    if (!spellDecalVisible_) return;
    initSpellTargetDecal();
    const VkDescriptorSet set = spellDecalSet_[spellDecalAcceptable_ ? 0 : 1];
    if (spellDecalPipeline_ == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE ||
        spellDecalVertMapped_ == nullptr || set == VK_NULL_HANDLE || selCircleIdxBuf_ == VK_NULL_HANDLE) {
        return;
    }
    const float r = spellDecalHalfWidth_;
    constexpr float kHalfHeight = 2.0f;  // 0xA4040C
    buildFloorLattice(spellDecalLattice_, spellDecalCenter_, r, kHalfHeight,
                      terrainHeight, wmoHeight, m2Height);

    const uint32_t slice = vkCtx_->getCurrentFrame() % kSelFrameSlices;
    const VkDeviceSize sliceBytes = sizeof(float) * spellDecalLattice_.verts.size();
    std::memcpy(static_cast<uint8_t*>(spellDecalVertMapped_) + slice * sliceBytes,
                spellDecalLattice_.verts.data(), sliceBytes);
    vmaFlushAllocation(vkCtx_->getAllocator(), spellDecalVertAlloc_, slice * sliceBytes, sliceBytes);

    SelCirclePush push{};
    push.viewProj = projection * view;
    push.color = glm::vec4(1.0f);  // 0xFFFFFFFF
    push.center = glm::vec4(spellDecalCenter_, r);
    // No texture matrix (0x7E4370's third argument is 0): the texture lies
    // along the world's axes whatever the camera does.
    push.facing = glm::vec4(0.0f, 1.0f, kSelSurfaceLift, kHalfHeight);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, spellDecalPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, selCirclePipelineLayout_,
                            0, 1, &set, 0, nullptr);
    const VkDeviceSize offset = slice * sliceBytes;
    vkCmdBindVertexBuffers(cmd, 0, 1, &spellDecalVertBuf_, &offset);
    vkCmdBindIndexBuffer(cmd, selCircleIdxBuf_, 0, VK_INDEX_TYPE_UINT16);
    vkCmdPushConstants(cmd, selCirclePipelineLayout_,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(push), &push);
    vkCmdDrawIndexed(cmd, static_cast<uint32_t>(selCircleVertCount_), 1, 0, 0, 0);
}

void OverlaySystem::initOverlayPipeline() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = 112;  // mat4 invViewProj + vec4 colour + vec4 plane + vec4 params

    VkPipelineLayoutCreateInfo plCI{};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pc;
    vkCreatePipelineLayout(device, &plCI, nullptr, &overlayPipelineLayout_);

    VkShaderModule vertMod, fragMod;
    if (!vertMod.loadFromFile(device, "assets/shaders/postprocess.vert.spv") ||
        !fragMod.loadFromFile(device, "assets/shaders/overlay.frag.spv")) {
        LOG_ERROR("OverlaySystem: failed to load overlay shaders");
        vertMod.destroy(); fragMod.destroy();
        return;
    }

    overlayPipeline_ = PipelineBuilder()
        .setShaders(vertMod.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragMod.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({}, {})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(overlayPipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());

    vertMod.destroy(); fragMod.destroy();

    if (overlayPipeline_) LOG_INFO("OverlaySystem: overlay pipeline initialized");
}

namespace {

/// overlay.frag's push block. One definition, because there are two entry
/// points into that shader and only one of them was keeping up with it.
struct OverlayPush {
    glm::mat4 invViewProj;
    glm::vec4 color;
    glm::vec4 plane;
    glm::vec4 params;
};
static_assert(sizeof(OverlayPush) == 112, "must match overlay.frag's push block");

OverlayPush makeOverlayPush(const glm::vec4& color, const glm::mat4& invViewProj,
                            float waterZ, float softness, float rippleAmp,
                            float time, bool hasSeam) {
    OverlayPush p{};
    p.invViewProj = invViewProj;
    p.color = color;
    p.plane = glm::vec4(0.0f, 0.0f, 0.0f, waterZ);
    p.params = glm::vec4(softness, time, rippleAmp, hasSeam ? 1.0f : 0.0f);
    return p;
}

}  // namespace

void OverlaySystem::renderOverlay(const glm::vec4& color, VkCommandBuffer cmd) {
    // Whole screen: no seam, so the shader skips the surface test entirely and
    // the matrix and height below are never read.
    renderWaterline(color, glm::mat4(1.0f), 0.0f, 0.0f, 0.0f, 0.0f, false, cmd);
}

void OverlaySystem::renderWaterline(const glm::vec4& color, const glm::mat4& invViewProj,
                                    float waterZ, float softness, float rippleAmp,
                                    float time, bool hasSeam, VkCommandBuffer cmd) {
    if (!overlayPipeline_) initOverlayPipeline();
    if (!overlayPipeline_ || cmd == VK_NULL_HANDLE) return;
    OverlayPush push = makeOverlayPush(color, invViewProj, waterZ, softness, rippleAmp,
                                       time, hasSeam);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipeline_);
    vkCmdPushConstants(cmd, overlayPipelineLayout_,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void OverlaySystem::initBrightnessPipeline() {
    if (brightnessPipeline_ != VK_NULL_HANDLE) return;
    if (!vkCtx_) return;
    // Reuse the overlay pipeline's layout (same fragment push constant); only
    // the blend state differs.
    if (!overlayPipeline_) initOverlayPipeline();
    if (!overlayPipelineLayout_) return;
    VkDevice device = vkCtx_->getDevice();

    VkShaderModule vertMod, fragMod;
    if (!vertMod.loadFromFile(device, "assets/shaders/postprocess.vert.spv") ||
        !fragMod.loadFromFile(device, "assets/shaders/overlay.frag.spv")) {
        LOG_ERROR("OverlaySystem: failed to load brightness shaders");
        vertMod.destroy(); fragMod.destroy();
        return;
    }

    // result.rgb = src.rgb * dst.rgb + dst.rgb * 1 = dst.rgb * (1 + src.rgb).
    // Pushing src.rgb = (scale - 1) yields dst.rgb * scale - a true luminance
    // multiply. Alpha is left untouched (dst passes through).
    VkPipelineColorBlendAttachmentState mul{};
    mul.blendEnable = VK_TRUE;
    mul.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    mul.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    mul.colorBlendOp = VK_BLEND_OP_ADD;
    mul.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    mul.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    mul.alphaBlendOp = VK_BLEND_OP_ADD;
    mul.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    brightnessPipeline_ = PipelineBuilder()
        .setShaders(vertMod.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragMod.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({}, {})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(mul)
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(overlayPipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());

    vertMod.destroy(); fragMod.destroy();
    if (brightnessPipeline_) LOG_INFO("OverlaySystem: brightness pipeline initialized");
}

void OverlaySystem::renderBrightnessScale(float scale, VkCommandBuffer cmd) {
    if (scale <= 1.0f) return; // darkening handled by the black overlay path
    if (!brightnessPipeline_) initBrightnessPipeline();
    if (!brightnessPipeline_ || cmd == VK_NULL_HANDLE) return;
    // overlay.frag outputs the pushed color; the blend multiplies it by dst.
    //
    // The whole block, not just the colour. This pushed sixteen bytes back when
    // the block started with one, and kept doing it after a mat4 was put in
    // front: the colour landed in the matrix's first row and everything the
    // shader actually reads was whatever the last draw had left there. It runs
    // whenever brightness is above neutral, so the screen flashed green or red
    // by the frame.
    const glm::vec4 tint(scale - 1.0f, scale - 1.0f, scale - 1.0f, 1.0f);
    OverlayPush push = makeOverlayPush(tint, glm::mat4(1.0f), 0.0f, 0.0f, 0.0f, 0.0f, false);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, brightnessPipeline_);
    vkCmdPushConstants(cmd, overlayPipelineLayout_,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace rendering
} // namespace wowee
