#include "rendering/glue_scene_particles.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/m2_track_sampler.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_utils.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace wowee {
namespace rendering {

namespace {

// M2Renderer's meaning for this bit (m2_renderer_internal.h).
constexpr uint32_t kFlagRandomTile = 0x40;

float lifeCurve(const pipeline::M2FBlock& fb, float t) {
    if (fb.floatValues.empty()) return 1.0f;
    if (fb.floatValues.size() == 1 || fb.timestamps.size() < 2) return fb.floatValues[0];
    t = glm::clamp(t, 0.0f, 1.0f);
    for (size_t i = 0; i + 1 < fb.timestamps.size(); ++i) {
        if (t <= fb.timestamps[i + 1]) {
            const float t0 = fb.timestamps[i], t1 = fb.timestamps[i + 1];
            const float f = t1 > t0 ? (t - t0) / (t1 - t0) : 0.0f;
            const size_t a = std::min(i, fb.floatValues.size() - 1);
            const size_t b = std::min(i + 1, fb.floatValues.size() - 1);
            return glm::mix(fb.floatValues[a], fb.floatValues[b], f);
        }
    }
    return fb.floatValues.back();
}

glm::vec3 lifeCurve3(const pipeline::M2FBlock& fb, float t) {
    if (fb.vec3Values.empty()) return glm::vec3(1.0f);
    if (fb.vec3Values.size() == 1 || fb.timestamps.size() < 2) return fb.vec3Values[0];
    t = glm::clamp(t, 0.0f, 1.0f);
    for (size_t i = 0; i + 1 < fb.timestamps.size(); ++i) {
        if (t <= fb.timestamps[i + 1]) {
            const float t0 = fb.timestamps[i], t1 = fb.timestamps[i + 1];
            const float f = t1 > t0 ? (t - t0) / (t1 - t0) : 0.0f;
            const size_t a = std::min(i, fb.vec3Values.size() - 1);
            const size_t b = std::min(i + 1, fb.vec3Values.size() - 1);
            return glm::mix(fb.vec3Values[a], fb.vec3Values[b], f);
        }
    }
    return fb.vec3Values.back();
}

} // namespace

GlueSceneParticles::~GlueSceneParticles() {
    shutdown();
}

bool GlueSceneParticles::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                                    VkRenderPass renderPass, VkSampleCountFlagBits samples) {
    if (vkCtx_) return true;
    if (!ctx || perFrameLayout == VK_NULL_HANDLE || renderPass == VK_NULL_HANDLE) return false;
    vkCtx_ = ctx;
    VkDevice device = ctx->getDevice();

    // set 1: the particle texture.
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1;
        ci.pBindings = &b;
        vkCreateDescriptorSetLayout(device, &ci, nullptr, &texLayout_);
    }
    // Sets are cached per texture for the life of this object and never
    // freed while frames may still use them; a few dozen textures cover
    // every glue scene.
    {
        VkDescriptorPoolSize size{.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 256};
        VkDescriptorPoolCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = 256;
        ci.poolSizeCount = 1;
        ci.pPoolSizes = &size;
        vkCreateDescriptorPool(device, &ci, nullptr, &texPool_);
    }
    // The M2 particle shaders' layout: set 0 per-frame, set 1 texture, and
    // the fragment push block {vec2 tileCount; int alphaKey}.
    {
        VkDescriptorSetLayout sets[] = {perFrameLayout, texLayout_};
        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pc.offset = 0;
        pc.size = 12;
        VkPipelineLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 2;
        ci.pSetLayouts = sets;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pc;
        vkCreatePipelineLayout(device, &ci, nullptr, &pipelineLayout_);
    }

    rendering::VkShaderModule vert, frag;
    if (!vert.loadFromFile(device, "assets/shaders/m2_particle.vert.spv") ||
        !frag.loadFromFile(device, "assets/shaders/m2_particle.frag.spv")) {
        LOG_WARNING("GlueSceneParticles: particle shaders missing; glue scenes draw without particles");
        shutdown();
        return false;
    }
    VkVertexInputBindingDescription bind{};
    bind.binding = 0;
    bind.stride = 9 * sizeof(float);  // pos3 + color4 + size1 + tile1
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::vector<VkVertexInputAttributeDescription> attrs = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 3 * sizeof(float)},
        {.location = 2, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = 7 * sizeof(float)},
        {.location = 3, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = 8 * sizeof(float)},
    };
    auto build = [&](VkPipelineColorBlendAttachmentState blend) {
        return PipelineBuilder()
            .setShaders(vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                        frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({bind}, attrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
            .setColorBlendAttachment(blend)
            .setMultisample(samples)
            .setLayout(pipelineLayout_)
            .setRenderPass(renderPass)
            .setDynamicStates(viewportAndScissorDynamic())
            .build(device, ctx->getPipelineCache());
    };
    alphaPipeline_ = build(PipelineBuilder::blendAlpha());
    additivePipeline_ = build(PipelineBuilder::blendAdditive());
    vert.destroy();
    frag.destroy();
    if (!alphaPipeline_ || !additivePipeline_) {
        LOG_WARNING("GlueSceneParticles: pipeline creation failed");
        shutdown();
        return false;
    }

    for (uint32_t i = 0; i < kFrames; ++i) {
        VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = static_cast<VkDeviceSize>(kMaxParticles) * 9 * sizeof(float);
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(ctx->getAllocator(), &bci, &aci, &vb_[i], &vbAlloc_[i], &info) != VK_SUCCESS) {
            shutdown();
            return false;
        }
        vbMapped_[i] = info.pMappedData;
    }
    return true;
}

void GlueSceneParticles::shutdown() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    vkDeviceWaitIdle(device);
    for (uint32_t i = 0; i < kFrames; ++i) {
        destroy(vkCtx_->getAllocator(), vb_[i], vbAlloc_[i]);
        vbMapped_[i] = nullptr;
    }
    destroy(device, alphaPipeline_);
    destroy(device, additivePipeline_);
    destroy(device, pipelineLayout_);
    destroy(device, texPool_);  // frees the cached sets with it
    destroy(device, texLayout_);
    texSets_.clear();
    clear();
    vkCtx_ = nullptr;
}

VkDescriptorSet GlueSceneParticles::textureSet(VkTexture* tex) {
    if (!vkCtx_ || !tex || !tex->isValid()) return VK_NULL_HANDLE;
    auto it = texSets_.find(tex);
    if (it != texSets_.end()) return it->second;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = texPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &texLayout_;
    if (vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorImageInfo img = tex->descriptorInfo();
    VkWriteDescriptorSet w{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &img;
    vkUpdateDescriptorSets(vkCtx_->getDevice(), 1, &w, 0, nullptr);
    texSets_.emplace(tex, set);
    return set;
}

void GlueSceneParticles::clear() {
    emitters_.clear();
    emitterData_.clear();
    globalSeqDurations_.clear();
    particles_.clear();
}

void GlueSceneParticles::setScene(const pipeline::M2Model& model, CharacterRenderer& textures) {
    clear();
    if (!vkCtx_ || model.particleEmitters.empty()) return;
    emitterData_ = model.particleEmitters;
    globalSeqDurations_ = model.globalSequenceDurations;
    emitters_.resize(emitterData_.size());
    size_t drawable = 0;
    for (size_t i = 0; i < emitterData_.size(); ++i) {
        Emitter& e = emitters_[i];
        e.data = &emitterData_[i];
        const uint16_t ti = emitterData_[i].texture;
        if (ti < model.textures.size() && !model.textures[ti].filename.empty()) {
            e.texture = textures.loadTexture(model.textures[ti].filename);
        }
        e.texSet = textureSet(e.texture);
        if (e.texSet != VK_NULL_HANDLE) ++drawable;
    }
    LOG_INFO("GlueSceneParticles: ", emitterData_.size(), " emitters (", drawable, " textured)");
}

void GlueSceneParticles::update(float dt, const std::vector<glm::mat4>& bones,
                                const glm::mat4& modelMatrix, int seq, float animTime,
                                float gsTime) {
    if (emitters_.empty()) return;
    dt = std::clamp(dt, 0.0f, 0.1f);
    auto track = [&](const pipeline::M2AnimationTrack& t) {
        return m2_track::sampleFloat(t, seq, animTime, gsTime, globalSeqDurations_, 0.0f);
    };
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    std::uniform_real_distribution<float> uN(-1.0f, 1.0f);

    // Age what is alive.
    for (size_t i = 0; i < particles_.size();) {
        Particle& p = particles_[i];
        p.life += dt;
        if (p.life >= p.maxLife) {
            p = particles_.back();
            particles_.pop_back();
            continue;
        }
        const auto& em = *emitters_[p.emitter].data;
        p.velocity.z -= track(em.gravity) * dt;
        if (em.drag > 0.0f) p.velocity *= std::exp(-em.drag * dt);
        p.position += p.velocity * dt;
        ++i;
    }

    // Emit.
    for (size_t ei = 0; ei < emitters_.size(); ++ei) {
        Emitter& e = emitters_[ei];
        const auto& em = *e.data;
        if (!em.enabled || e.texSet == VK_NULL_HANDLE) continue;
        const float rate = track(em.emissionRate);
        const float life = track(em.lifespan);
        if (rate <= 0.0f || life <= 0.0f) continue;
        e.accumulator = std::min(e.accumulator + rate * dt, 4.0f);
        if (e.accumulator < 1.0f) continue;

        const glm::mat4 bone = em.bone < bones.size() ? bones[em.bone] : glm::mat4(1.0f);
        const glm::mat4 xf = modelMatrix * bone;
        const glm::mat3 rot(xf);
        const float speed = track(em.emissionSpeed);
        const float speedVar = track(em.speedVariation);
        const float vRange = track(em.verticalRange);
        const float hRange = track(em.horizontalRange);
        const float areaL = track(em.emissionAreaLength);
        const float areaW = track(em.emissionAreaWidth);
        const uint32_t tiles = std::max<uint32_t>(1u, uint32_t(em.textureRows) * em.textureCols);

        while (e.accumulator >= 1.0f && particles_.size() < kMaxParticles) {
            e.accumulator -= 1.0f;
            glm::vec3 local = em.position;
            glm::vec3 dir;
            if (em.emitterType == 2) {
                // Sphere: out from the centre, at a radius between the two
                // area values, within the polar (vertical) and azimuth
                // (horizontal) ranges.
                const float maxPolar = glm::clamp(vRange, 0.0f, 3.14159265f);
                const float cosPolar = 1.0f - u01(rng_) * (1.0f - std::cos(maxPolar));
                const float polar = std::acos(glm::clamp(cosPolar, -1.0f, 1.0f));
                const float azim = (hRange > 0.0f ? uN(rng_) * hRange : u01(rng_) * 6.2831853f);
                dir = glm::vec3(std::sin(polar) * std::cos(azim), std::sin(polar) * std::sin(azim),
                                std::cos(polar));
                const float radius = glm::mix(areaL, areaW, u01(rng_));
                local += dir * radius;
            } else {
                // Plane (and spline, approximated as one): anywhere on the
                // emitter's L x W rectangle, heading up its Z tilted by up to
                // the vertical range at a random azimuth.
                local += glm::vec3(uN(rng_) * areaL * 0.5f, uN(rng_) * areaW * 0.5f, 0.0f);
                const float theta = uN(rng_) * vRange;
                const float phi = u01(rng_) * 6.2831853f;
                dir = glm::vec3(std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi),
                                std::cos(theta));
            }
            Particle p;
            p.position = glm::vec3(xf * glm::vec4(local, 1.0f));
            const float s = speed * (1.0f + uN(rng_) * speedVar);
            p.velocity = glm::normalize(rot * dir) * s;
            if (!std::isfinite(p.velocity.x)) p.velocity = glm::vec3(0.0f);
            p.maxLife = life;
            p.emitter = static_cast<uint16_t>(ei);
            p.tile = (tiles > 1 && (em.flags & kFlagRandomTile))
                         ? static_cast<uint16_t>(rng_() % tiles) : 0;
            particles_.push_back(p);
        }
    }
}

void GlueSceneParticles::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                                uint32_t frameIndex) {
    if (!vkCtx_ || particles_.empty() || emitters_.empty()) return;
    const uint32_t fi = frameIndex % kFrames;
    float* vb = static_cast<float*>(vbMapped_[fi]);
    if (!vb) return;

    // Grouped by emitter, so one texture bind and draw per emitter.
    struct Run { uint32_t first, count; uint16_t emitter; };
    std::vector<Run> runs;
    runs.reserve(emitters_.size());
    std::vector<uint32_t> order(particles_.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return particles_[a].emitter < particles_[b].emitter;
    });
    uint32_t written = 0;
    for (uint32_t idx : order) {
        const Particle& p = particles_[idx];
        const auto& em = *emitters_[p.emitter].data;
        const float t = p.life / std::max(p.maxLife, 0.001f);
        const glm::vec3 color = lifeCurve3(em.particleColor, t);
        const float alpha = std::min(lifeCurve(em.particleAlpha, t), 1.0f);
        const float scale = lifeCurve(em.particleScale, t);
        const uint32_t tiles = std::max<uint32_t>(1u, uint32_t(em.textureRows) * em.textureCols);
        float tile = 0.0f;
        if (tiles > 1) {
            // Step through the cells over the particle's life.
            tile = static_cast<float>((p.tile + static_cast<uint32_t>(t * tiles)) % tiles);
        }
        if (runs.empty() || runs.back().emitter != p.emitter) runs.push_back({written, 0, p.emitter});
        float* v = vb + static_cast<size_t>(written) * 9;
        v[0] = p.position.x; v[1] = p.position.y; v[2] = p.position.z;
        v[3] = color.r; v[4] = color.g; v[5] = color.b; v[6] = alpha;
        v[7] = scale;
        v[8] = tile;
        ++written;
        ++runs.back().count;
    }
    if (written == 0) return;

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                            &perFrameSet, 0, nullptr);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb_[fi], &off);
    VkPipeline bound = VK_NULL_HANDLE;
    for (const Run& r : runs) {
        const Emitter& e = emitters_[r.emitter];
        const auto& em = *e.data;
        // 3 and 4 are the additive blends, as M2Renderer draws them.
        VkPipeline want = (em.blendingType == 3 || em.blendingType == 4) ? additivePipeline_
                                                                         : alphaPipeline_;
        if (want != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
            bound = want;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1,
                                &e.texSet, 0, nullptr);
        struct { float tx, ty; int32_t alphaKey; } pc{
            static_cast<float>(std::max<uint16_t>(em.textureCols, 1)),
            static_cast<float>(std::max<uint16_t>(em.textureRows, 1)),
            em.blendingType == 1 ? 1 : 0};
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        vkCmdDraw(cmd, r.count, 1, r.first, 0);
    }
}

} // namespace rendering
} // namespace wowee
