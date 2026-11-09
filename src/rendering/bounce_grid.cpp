// The bounce light's grid (bounce_grid.hpp, bounce_grid_gpu.hpp): what it
// gathers from the loaded world, the bake job, and the copy to the GPU.
#include "rendering/bounce_grid_gpu.hpp"

#include "rendering/m2_renderer.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/wmo_renderer.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

namespace wowee {
namespace rendering {

namespace bg = bounce_grid;

// ---------------------------------------------------------------- gathering

void M2Renderer::collectBounceCanopies(const glm::vec2& lo, const glm::vec2& hi,
                                       std::vector<bounce_grid::Canopy>& out) const {
    // Per model: the leaves' colour and how much of the sky its crown takes.
    struct Leaf {
        glm::vec3 colour{0.3f, 0.4f, 0.15f};
        float density = 0.0f;
    };
    std::unordered_map<const M2ModelGPU*, Leaf> leafOf;
    const auto leafFor = [&](const M2ModelGPU& m) -> const Leaf& {
        auto it = leafOf.find(&m);
        if (it != leafOf.end()) return it->second;
        Leaf l;
        glm::vec3 sum(0.0f);
        float w = 0.0f;
        for (const auto& b : m.batches) {
            // The leaf cards: a texture with real holes in it (the cutout
            // batches m2.frag keys), not the bark or a glow.
            if (!b.texture || b.blendMode > 3 || b.submeshLevel != 0) continue;
            const float cov = b.texture->alphaCoverage();
            if (cov >= 0.98f || cov <= 0.02f) continue;
            const float weight = static_cast<float>(b.indexCount);
            sum += b.texture->averageColor() * weight;
            w += weight;
        }
        if (w > 0.0f) {
            l.colour = sum / w;
            // A full crown takes most of the sky at its middle; the disc's
            // falloff thins it toward the rim.
            l.density = 0.85f;
        } else {
            // No leaf cards: a bare or dead tree, a palm drawn opaque.
            l.density = 0.3f;
        }
        return leafOf.emplace(&m, l).first->second;
    };
    for (const M2Instance& inst : instances) {
        const M2ModelGPU* model = inst.cachedModel;
        if (!model || !inst.cachedIsValid) continue;
        if (!model->isFoliageLike || model->isGroundDetail || model->isSmallFoliage || model->isSpellEffect) continue;
        if (inst.position.x < lo.x - 20.0f || inst.position.y < lo.y - 20.0f || inst.position.x > hi.x + 20.0f ||
            inst.position.y > hi.y + 20.0f)
            continue;
        const glm::vec3 bmin = model->boundMin, bmax = model->boundMax;
        glm::vec3 wmin(1e30f), wmax(-1e30f);
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 p((c & 1) ? bmax.x : bmin.x, (c & 2) ? bmax.y : bmin.y, (c & 4) ? bmax.z : bmin.z);
            const glm::vec3 w = glm::vec3(inst.modelMatrix * glm::vec4(p, 1.0f));
            wmin = glm::min(wmin, w);
            wmax = glm::max(wmax, w);
        }
        const float h = wmax.z - wmin.z;
        // Trees: tall enough to stand over a person. Bushes and ferns are
        // isSmallFoliage or under this.
        if (h < 5.0f) continue;
        const float s = std::abs(inst.scale) > 0.0f ? std::abs(inst.scale) : 1.0f;
        // The crown's reach is the model's widest extent; its leaves are
        // thick over the inner two thirds of that (the outer branches hold
        // a few cards). Seen against the grid drawn over Goldshire, the full
        // extent covered the open square between the oaks.
        const float r = 0.5f * std::max(bmax.x - bmin.x, bmax.y - bmin.y) * s * 0.65f;
        if (r < 1.0f) continue;
        const Leaf& leaf = leafFor(*model);
        const glm::vec3 c = glm::vec3(inst.modelMatrix * glm::vec4((bmin + bmax) * 0.5f, 1.0f));
        bg::Canopy cp;
        cp.centre = glm::vec2(c.x, c.y);
        cp.radius = std::min(r, 24.0f);
        // Most of the 3.3.5 trees' crowns start a third of the way up.
        cp.bottom = wmin.z + 0.35f * h;
        cp.top = wmax.z;
        cp.leaf = leaf.colour;
        cp.density = leaf.density;
        out.push_back(cp);
    }
}

void WMORenderer::collectBounceBuildings(const glm::vec2& lo, const glm::vec2& hi,
                                         std::vector<bounce_grid::Building>& out) const {
    for (const WMOInstance& inst : instances) {
        if (inst.worldBoundsMax.x < lo.x || inst.worldBoundsMax.y < lo.y || inst.worldBoundsMin.x > hi.x ||
            inst.worldBoundsMin.y > hi.y)
            continue;
        auto mit = loadedModels.find(inst.modelId);
        if (mit == loadedModels.end()) continue;
        const ModelData& model = mit->second;
        const size_t n = std::min(model.groups.size(), inst.worldGroupBounds.size());
        for (size_t gi = 0; gi < n; ++gi) {
            // Exterior groups (MOGP 0x8) only: an interior's walls are inside.
            if ((model.groups[gi].groupFlags & 0x8u) == 0) continue;
            const auto& [gMin, gMax] = inst.worldGroupBounds[gi];
            const glm::vec3 size = gMax - gMin;
            // A house, a tower, a wall: not a whole city block or a street's
            // ground (those are groups too, and their boxes are mostly air).
            if (size.z < 3.0f || size.x > 48.0f || size.y > 48.0f || size.x < 1.0f || size.y < 1.0f) continue;
            bg::Building b;
            b.lo = glm::vec2(gMin.x, gMin.y);
            b.hi = glm::vec2(gMax.x, gMax.y);
            b.bottom = gMin.z;
            b.top = gMax.z;
            out.push_back(b);
        }
    }
}

// --------------------------------------------------------------------- GPU

bool BounceGrid::ensure(VkContext* ctx) {
    if (failed_ || !ctx) return false;
    if (set_ != VK_NULL_HANDLE) return true;
    ctx_ = ctx;
    VkDevice device = ctx->getDevice();
    VmaAllocator alloc = ctx->getAllocator();

    VkImageCreateInfo ici{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ici.extent = {static_cast<uint32_t>(bg::kCells), static_cast<uint32_t>(bg::kCells), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = bg::kLayers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(alloc, &ici, &aci, &image_, &imageAlloc_, nullptr) != VK_SUCCESS) {
        image_ = VK_NULL_HANDLE;
        failed_ = true;
        shutdown();
        return false;
    }
    VkImageViewCreateInfo vci{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vci.format = ici.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, static_cast<uint32_t>(bg::kLayers)};
    if (vkCreateImageView(device, &vci, nullptr, &view_) != VK_SUCCESS) {
        view_ = VK_NULL_HANDLE;
        failed_ = true;
        shutdown();
        return false;
    }
    VkSamplerCreateInfo sci{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    sampler_ = ctx->getOrCreateSampler(sci);

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 1;
    lci.pBindings = &binding;
    VkDescriptorPoolSize ps{.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1};
    VkDescriptorPoolCreateInfo pci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (sampler_ == VK_NULL_HANDLE || vkCreateDescriptorSetLayout(device, &lci, nullptr, &setLayout_) != VK_SUCCESS ||
        vkCreateDescriptorPool(device, &pci, nullptr, &pool_) != VK_SUCCESS) {
        failed_ = true;
        shutdown();
        return false;
    }
    VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &setLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device, &ai, &set) != VK_SUCCESS) {
        failed_ = true;
        shutdown();
        return false;
    }
    VkDescriptorImageInfo ii{sampler_, view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(bg::kLayers) * bg::kCells * bg::kCells * 4 * 2;
    for (uint32_t i = 0; i < kSlots; ++i) {
        AllocatedBuffer b = createBuffer(alloc, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
        staging_[i] = b.buffer;
        stagingAlloc_[i] = b.allocation;
        stagingMapped_[i] = b.info.pMappedData;
        if (!staging_[i] || !stagingMapped_[i]) {
            failed_ = true;
            set_ = set;
            shutdown();
            return false;
        }
    }
    set_ = set;
    LOG_INFO("Bounce light: grid made (", bg::kCells, "x", bg::kCells, " x", bg::kLayers, " RGBA16F, ",
             bg::kCellYards, " yd a cell)");
    return true;
}

void BounceGrid::shutdown() {
    if (job_.valid()) job_.wait();
    if (!ctx_) return;
    VkDevice device = ctx_->getDevice();
    VmaAllocator alloc = ctx_->getAllocator();
    for (uint32_t i = 0; i < kSlots; ++i) {
        destroy(alloc, staging_[i], stagingAlloc_[i]);
        stagingMapped_[i] = nullptr;
    }
    destroy(device, pool_);
    set_ = VK_NULL_HANDLE;
    destroy(device, setLayout_);
    if (view_) vkDestroyImageView(device, view_, nullptr);
    view_ = VK_NULL_HANDLE;
    if (image_) vmaDestroyImage(alloc, image_, imageAlloc_);
    image_ = VK_NULL_HANDLE;
    imageAlloc_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    layoutReady_ = false;
    ready_ = false;
    ctx_ = nullptr;
}

namespace {

std::string lower(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}

void stripBlp(std::string& v) {
    if (v.size() > 4 && lower(v.substr(v.size() - 4)) == ".blp") v.resize(v.size() - 4);
}

// md5translate.trs: "<map>\map<x>_<y>.blp<TAB><hash>.blp" lines (as
// TerrainManager::buildFarFieldAtlas reads it), for one map.
std::unordered_map<std::string, std::string> readTrs(pipeline::AssetManager* assets, const std::string& map) {
    std::unordered_map<std::string, std::string> trs;
    const std::vector<uint8_t> data = assets->readFile("Textures\\Minimap\\md5translate.trs");
    const std::string prefix = lower(map) + "\\";
    std::string line;
    size_t i = 0;
    while (i < data.size()) {
        size_t j = i;
        while (j < data.size() && data[j] != '\n') ++j;
        line.assign(reinterpret_cast<const char*>(data.data()) + i, j - i);
        i = j + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string key = lower(line.substr(0, tab));
        if (key.compare(0, prefix.size(), prefix) != 0) continue;
        stripBlp(key);
        std::string hash = line.substr(tab + 1);
        stripBlp(hash);
        trs[key.substr(prefix.size())] = std::move(hash);
    }
    return trs;
}

constexpr int kMinimapTexels = 128;  // a tile's picture, ~4.2 yards a texel

}  // namespace

void BounceGrid::update(const glm::vec3& camera, float dt, const M2Renderer* m2, const WMORenderer* wmo,
                        pipeline::AssetManager* assets, const std::string& mapName) {
    if (job_.valid()) {
        if (job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        bg::Grid g = job_.get();
        if (!g.empty()) {
            bg::pack(g, pendingHalves_);
            pendingOrigin_ = g.origin;
            pendingUpload_ = true;
        }
    }
    if (!assets) return;
    sinceCheck_ += dt;
    if (sinceCheck_ >= 2.0f) {
        sinceCheck_ = 0.0f;
        const uint32_t m2n = m2 ? m2->getInstanceCount() : 0;
        const uint32_t wmon = wmo ? wmo->getInstanceCount() : 0;
        // Units' and spells' models come and go all the time; a tile's
        // trees and buildings arrive by the hundred.
        const auto moved = [](uint32_t a, uint32_t b) {
            const uint32_t d = a > b ? a - b : b - a;
            return d > std::max<uint32_t>(32u, std::max(a, b) / 50u);
        };
        if (moved(m2n, lastM2Count_) || wmon != lastWmoCount_) worldChanged_ = true;
        lastM2Count_ = m2n;
        lastWmoCount_ = wmon;
    }
    const bool mapChanged = mapName != lastMap_;
    if (haveGrid_ && !mapChanged && !worldChanged_ && !bg::needsRecentre(lastOrigin_, camera.x, camera.y)) return;

    bg::Inputs in;
    in.origin = bg::originFor(camera.x, camera.y);
    const glm::vec2 lo = in.origin, hi = in.origin + glm::vec2(bg::kSpan);
    const auto t0 = std::chrono::steady_clock::now();
    if (m2) m2->collectBounceCanopies(lo, hi, in.canopies);
    if (wmo) wmo->collectBounceBuildings(lo, hi, in.buildings);
    const double gatherMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!minimap_ || minimap_->map != mapName) {
        minimap_ = std::make_shared<Minimap>();
        minimap_->map = mapName;
        minimap_->trs.clear();
    }
    lastOrigin_ = in.origin;
    lastMap_ = mapName;
    worldChanged_ = false;
    haveGrid_ = true;
    jobOrigin_ = in.origin;
    std::shared_ptr<Minimap> mm = minimap_;
    const size_t canopies = in.canopies.size(), buildings = in.buildings.size();
    job_ = std::async(std::launch::async, [in = std::move(in), mm, assets, gatherMs, canopies,
                                           buildings]() mutable {
        const auto t1 = std::chrono::steady_clock::now();
        if (mm->trs.empty()) mm->trs = readTrs(assets, mm->map);
        // The tiles the grid covers (at most 2x2 for 512 yards), decoded and
        // boxed down once each.
        const float T = core::coords::TILE_SIZE;
        const auto tileFor = [&](int tx, int ty) -> std::shared_ptr<std::vector<float>> {
            const int key = tx * 64 + ty;
            auto it = mm->tiles.find(key);
            if (it != mm->tiles.end()) return it->second;
            std::shared_ptr<std::vector<float>> img;
            auto t = mm->trs.find("map" + std::to_string(tx) + "_" + std::to_string(ty));
            if (t != mm->trs.end()) {
                const pipeline::BLPImage blp = assets->loadTexture("Textures\\Minimap\\" + t->second + ".blp");
                if (blp.isValid() && !blp.data.empty() && blp.width % kMinimapTexels == 0 &&
                    blp.height % kMinimapTexels == 0) {
                    img = std::make_shared<std::vector<float>>(kMinimapTexels * kMinimapTexels * 3);
                    const int fx = blp.width / kMinimapTexels, fy = blp.height / kMinimapTexels;
                    for (int y = 0; y < kMinimapTexels; ++y) {
                        for (int x = 0; x < kMinimapTexels; ++x) {
                            float s[3] = {0, 0, 0};
                            for (int j = 0; j < fy; ++j)
                                for (int i = 0; i < fx; ++i) {
                                    const uint8_t* p =
                                        &blp.data[(static_cast<size_t>(y * fy + j) * blp.width + x * fx + i) * 4];
                                    s[0] += p[0];
                                    s[1] += p[1];
                                    s[2] += p[2];
                                }
                            const float n = 255.0f * static_cast<float>(fx * fy);
                            float* o = &(*img)[(static_cast<size_t>(y) * kMinimapTexels + x) * 3];
                            o[0] = s[0] / n;
                            o[1] = s[1] / n;
                            o[2] = s[2] / n;
                        }
                    }
                }
            }
            mm->tiles[key] = img;
            if (mm->tiles.size() > 16) {
                // Keep the cache small: drop any tile other than this one.
                for (auto d = mm->tiles.begin(); d != mm->tiles.end(); ++d) {
                    if (d->first != key) {
                        mm->tiles.erase(d);
                        break;
                    }
                }
            }
            return img;
        };
        // As the far field reads the minimap (terrain_horizon.glsli): tile
        // x from world y, tile y from world x, both counted down from 32.
        in.groundAt = [&](float wx, float wy, glm::vec3& rgb) {
            const float u = 32.0f - wy / T, v = 32.0f - wx / T;
            const int tx = static_cast<int>(std::floor(u)), ty = static_cast<int>(std::floor(v));
            if (tx < 0 || ty < 0 || tx >= 64 || ty >= 64) return false;
            const auto img = tileFor(tx, ty);
            if (!img) return false;
            const int px = std::clamp(static_cast<int>((u - tx) * kMinimapTexels), 0, kMinimapTexels - 1);
            const int py = std::clamp(static_cast<int>((v - ty) * kMinimapTexels), 0, kMinimapTexels - 1);
            const float* c = &(*img)[(static_cast<size_t>(py) * kMinimapTexels + px) * 3];
            rgb = glm::vec3(c[0], c[1], c[2]);
            return true;
        };
        bg::Grid g = bg::bake(in);
        const double bakeMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        LOG_DEBUG("Bounce light: grid at (", in.origin.x, ", ", in.origin.y, "): ", canopies, " crowns, ",
                 buildings, " walls; gathered in ", gatherMs, " ms (main), baked in ", bakeMs, " ms (worker)");
        return g;
    });
}

void BounceGrid::recordUpload(VkCommandBuffer cmd, uint32_t frameSlot) {
    if (set_ == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE) return;
    VkImageMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image_;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, static_cast<uint32_t>(bg::kLayers)};
    if (!pendingUpload_) {
        if (layoutReady_) return;
        // Readable (and never read: the shader is told the grid is not
        // ready) until the first bake lands.
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
        layoutReady_ = true;
        return;
    }
    const uint32_t slot = frameSlot % kSlots;
    std::memcpy(stagingMapped_[slot], pendingHalves_.data(), pendingHalves_.size() * sizeof(uint16_t));
    vmaFlushAllocation(ctx_->getAllocator(), stagingAlloc_[slot], 0, VK_WHOLE_SIZE);
    // Whatever read it last frame is done before the copy writes it.
    b.oldLayout = layoutReady_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask = layoutReady_ ? VK_ACCESS_SHADER_READ_BIT : 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, static_cast<uint32_t>(bg::kLayers)};
    region.imageExtent = {static_cast<uint32_t>(bg::kCells), static_cast<uint32_t>(bg::kCells), 1};
    vkCmdCopyBufferToImage(cmd, staging_[slot], image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
    layoutReady_ = true;
    origin_ = pendingOrigin_;
    ready_ = true;
    pendingUpload_ = false;
}

}  // namespace rendering
}  // namespace wowee
