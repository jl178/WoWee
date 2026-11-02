#include "rendering/glue_scene_particles.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/m2_track_sampler.hpp"
#include "rendering/m2_particle_stock.hpp"
#include "rendering/m2_particle_step.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_utils.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace wowee {
namespace rendering {

namespace {

namespace mp = m2_particle;

bool finite3(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

} // namespace

GlueSceneParticles::~GlueSceneParticles() {
    shutdown();
}

bool GlueSceneParticles::initialize(VkContext* ctx, VkRenderPass renderPass,
                                    VkSampleCountFlagBits samples) {
    if (vkCtx_) return true;
    if (!ctx || renderPass == VK_NULL_HANDLE) return false;
    vkCtx_ = ctx;
    VkDevice device = ctx->getDevice();

    // set 0: the particle texture, the only descriptor this draw binds.
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
    // glue_particle.vert/frag: set 0 the texture; push constants
    // {mat4 viewProj; vec4 unused[2]} for the vertex stage at 0..96 and
    // {vec2 unused; int alphaKey} for the fragment stage at 96.
    {
        VkPushConstantRange pc[2]{};
        pc[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pc[0].offset = 0;
        pc[0].size = 96;
        pc[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pc[1].offset = 96;
        pc[1].size = 12;
        VkPipelineLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 1;
        ci.pSetLayouts = &texLayout_;
        ci.pushConstantRangeCount = 2;
        ci.pPushConstantRanges = pc;
        vkCreatePipelineLayout(device, &ci, nullptr, &pipelineLayout_);
    }
    if (!texLayout_ || !texPool_ || !pipelineLayout_) {
        shutdown();
        return false;
    }

    rendering::VkShaderModule vert, frag;
    if (!vert.loadFromFile(device, "assets/shaders/glue_particle.vert.spv") ||
        !frag.loadFromFile(device, "assets/shaders/glue_particle.frag.spv")) {
        LOG_WARNING("GlueSceneParticles: particle shaders missing; glue scenes draw without particles");
        shutdown();
        return false;
    }
    VkVertexInputBindingDescription bind{};
    bind.binding = 0;
    bind.stride = 9 * sizeof(float);  // pos3 + color4 + uv2
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::vector<VkVertexInputAttributeDescription> attrs = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 3 * sizeof(float)},
        {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 7 * sizeof(float)},
    };
    auto build = [&](VkPipelineColorBlendAttachmentState blend) {
        return PipelineBuilder()
            .setShaders(vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                        frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({bind}, attrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
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
        bci.size = static_cast<VkDeviceSize>(kMaxParticles) * kVertsPerQuad * 2 * 9 * sizeof(float);
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
    if (!(dt > 0.0f)) return;
    auto track = [&](const pipeline::M2AnimationTrack& t) {
        return m2_track::sampleFloat(t, seq, animTime, gsTime, globalSeqDurations_, 0.0f);
    };
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    std::uniform_real_distribution<float> uN(-1.0f, 1.0f);
    const glm::mat4 rz90 = mp::emitterRotation();

    // The same emission and step as the world's emitters
    // (M2Renderer::stepParticles), for one scene and without the pool or
    // distance limits a glue scene never reaches.
    for (size_t ei = 0; ei < emitters_.size(); ++ei) {
        Emitter& e = emitters_[ei];
        const auto& em = *e.data;
        const glm::mat4 bone = em.bone < bones.size() ? bones[em.bone] : glm::mat4(1.0f);
        e.xf = modelMatrix * bone * glm::translate(glm::mat4(1.0f), em.position) * rz90;
        if (!em.enabled || e.texSet == VK_NULL_HANDLE) continue;
        const bool on = m2_track::sampleFloat(em.enabledTrack, seq, animTime, gsTime,
                                              globalSeqDurations_, 1.0f) > 0.5f;
        const float rate = on ? std::max(track(em.emissionRate), 0.0f) : 0.0f;
        const float life = track(em.lifespan);
        int count = 0;
        const float r = rate + em.emissionRateVary * uN(rng_);
        if (em.flags & mp::kFlagBurstOnEnable) {
            const bool now = on && rate > 0.0f;
            if (now && !e.wasOn) count = static_cast<int>(r);
            e.wasOn = now;
        } else if (on) {
            e.accumulator += r * dt;
            count = static_cast<int>(e.accumulator + 0.5f);
            e.accumulator -= static_cast<float>(count);
        }
        if (count <= 0 || life <= 0.0f) continue;

        const glm::mat3 rot(e.xf);
        const bool modelSpace = (em.flags & mp::kFlagModelSpace) != 0;
        const float speed = track(em.emissionSpeed);
        const float speedVar = track(em.speedVariation);
        const float vRange = track(em.verticalRange);
        const float hRange = track(em.horizontalRange);
        const float areaL = track(em.emissionAreaLength);
        const float areaW = track(em.emissionAreaWidth);
        float zSource = track(em.deceleration);
        if (std::abs(zSource) < 0.001f) zSource = 0.0f;
        const uint32_t tiles = std::max<uint32_t>(1u, uint32_t(em.textureRows) * em.textureCols);
        const bool randomTile = (em.flags & mp::kFlagRandomTile) && tiles > 1 && em.headCell.floatValues.empty();

        for (int k = 0; k < count && particles_.size() < kMaxParticles; ++k) {
            glm::vec3 lp(0.0f), lv(0.0f);
            const float s = (uN(rng_) * speedVar + 1.0f) * speed;
            if (em.emitterType == mp::kEmitterSphere) {
                const float radius = u01(rng_) * (areaW - areaL) + areaL;
                const glm::vec3 d = mp::sphereDirection(vRange, hRange, uN(rng_), uN(rng_));
                lp = d * radius;
                glm::vec3 dir = d;
                if (zSource != 0.0f) {
                    dir = lp - glm::vec3(0.0f, 0.0f, zSource);
                    const float l2 = glm::dot(dir, dir);
                    if (l2 > 2.4e-7f) dir *= 1.0f / std::sqrt(l2);
                } else if (em.flags & mp::kFlagSphereVelocityUp) {
                    dir = glm::vec3(0.0f, 0.0f, 1.0f);
                }
                lv = dir * s;
            } else {
                lp = glm::vec3(uN(rng_) * areaL * 0.5f, uN(rng_) * areaW * 0.5f, 0.0f);
                if (zSource != 0.0f) {
                    lv = glm::normalize(lp - glm::vec3(0.0f, 0.0f, zSource)) * s;
                } else {
                    lv = mp::planeDirection(vRange, hRange, uN(rng_), uN(rng_)) * s;
                }
            }
            Particle p;
            p.position = modelSpace ? lp : glm::vec3(e.xf * glm::vec4(lp, 1.0f));
            p.velocity = modelSpace ? lv : rot * lv;
            if (!finite3(p.position) || !finite3(p.velocity)) continue;
            p.life = u01(rng_) * dt;
            p.maxLife = std::clamp(life + uN(rng_) * em.lifespanVary, 0.001f, 60.0f);
            p.sizeVary = mp::scaleVariation(em, uN(rng_), uN(rng_));
            p.spinBase = em.baseSpin + uN(rng_) * em.baseSpinVary;
            p.spinSpeed = em.spin + uN(rng_) * em.spinVary;
            if ((em.flags & mp::kFlagRandomSpinDirection) && (rng_() & 1u)) {
                p.spinBase = -p.spinBase;
                p.spinSpeed = -p.spinSpeed;
            }
            p.emitter = static_cast<uint16_t>(ei);
            p.tile = randomTile ? static_cast<uint16_t>(rng_() % tiles) : 0;
            p.phase = static_cast<uint16_t>(rng_() & 127u);
            particles_.push_back(p);
        }
    }

    // Age and move what is alive (m2_particle::stepParticle).
    for (size_t i = 0; i < particles_.size();) {
        Particle& p = particles_[i];
        const auto& em = *emitters_[p.emitter].data;
        mp::StepEmitter se;
        se.gravity = track(em.gravity);
        se.drag = em.drag;
        se.windTime = em.windTime;
        se.wind = em.windVector;
        se.center = (em.flags & mp::kFlagModelSpace) ? glm::vec3(0.0f) : glm::vec3(emitters_[p.emitter].xf[3]);
        se.killPastCenter = em.emitterType == mp::kEmitterSphere && (em.flags & mp::kFlagSphereKillPastCenter);
        if (!mp::stepParticle(p, se, dt) || !finite3(p.position) || !finite3(p.velocity)) {
            p = particles_.back();
            particles_.pop_back();
            continue;
        }
        ++i;
    }
}

void GlueSceneParticles::render(VkCommandBuffer cmd, const glm::mat4& view,
                                const glm::mat4& projection, uint32_t frameIndex) {
    if (!vkCtx_ || particles_.empty() || emitters_.empty()) return;
    if (!alphaPipeline_ || !additivePipeline_ || !pipelineLayout_) return;
    const uint32_t fi = frameIndex % kFrames;
    float* vb = static_cast<float*>(vbMapped_[fi]);
    if (!vb || vb_[fi] == VK_NULL_HANDLE) return;
    const glm::mat4 viewProj = projection * view;
    const glm::vec3 right(view[0][0], view[1][0], view[2][0]);
    const glm::vec3 up(view[0][1], view[1][1], view[2][1]);

    // Grouped by emitter, so one texture bind and draw per emitter.
    struct Run { uint32_t first, count; uint16_t emitter; };
    std::vector<Run> runs;
    runs.reserve(emitters_.size());
    std::vector<uint32_t> order(particles_.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return particles_[a].emitter < particles_[b].emitter;
    });
    const uint32_t maxVerts = kMaxParticles * kVertsPerQuad * 2;
    uint32_t written = 0;
    const auto quad = [&](const glm::vec3 (&c)[4], const glm::vec2 (&uv)[4], const glm::vec4& col) {
        static constexpr int kTri[6] = {0, 1, 2, 1, 3, 2};
        for (int k : kTri) {
            float* v = vb + static_cast<size_t>(written) * 9;
            v[0] = c[k].x; v[1] = c[k].y; v[2] = c[k].z;
            v[3] = col.r; v[4] = col.g; v[5] = col.b; v[6] = col.a;
            v[7] = uv[k].x; v[8] = uv[k].y;
            ++written;
        }
        ++runs.back().count;
    };
    for (uint32_t idx : order) {
        if (written + 2 * kVertsPerQuad > maxVerts) break;
        const Particle& p = particles_[idx];
        if (p.emitter >= emitters_.size()) continue;
        const Emitter& pe = emitters_[p.emitter];
        if (pe.texSet == VK_NULL_HANDLE) continue;
        const auto& em = *pe.data;
        const bool modelSpace = (em.flags & mp::kFlagModelSpace) != 0;
        const glm::vec3 pos = modelSpace ? glm::vec3(pe.xf * glm::vec4(p.position, 1.0f)) : p.position;
        const glm::vec3 vel = modelSpace ? glm::mat3(pe.xf) * p.velocity : p.velocity;
        if (!finite3(pos)) continue;
        const mp::Twinkle tw = mp::twinkle(em, p.phase, p.life);
        if (!tw.visible) continue;
        const float t = glm::clamp(p.life / std::max(p.maxLife, 0.001f), 0.0f, 1.0f);
        const glm::vec3 color = glm::clamp(mp::particleColor(em, t), glm::vec3(0.0f), glm::vec3(1.0f));
        const float alpha = glm::clamp(mp::particleAlpha(em, t), 0.0f, 1.0f);
        glm::vec2 size = mp::particleScale(em, t) * p.sizeVary * tw.scale;
        if (em.flags & mp::kFlagInheritScale) size *= glm::length(glm::vec3(pe.xf[0]));
        if (!(alpha > 0.0f) || !finite3(color) || !std::isfinite(size.x) || !std::isfinite(size.y)) continue;
        const glm::vec4 col(color, alpha);
        const uint32_t rows = std::max<uint16_t>(em.textureRows, 1), cols = std::max<uint16_t>(em.textureCols, 1);
        if (runs.empty() || runs.back().emitter != p.emitter) runs.push_back({written, 0, p.emitter});

        glm::vec3 c[4];
        glm::vec2 uv[4];
        if (em.flags & mp::kFlagHead) {
            int32_t cell = mp::particleCell(em.headCell, t);
            if (cell < 0) cell = p.tile;
            const bool spin = mp::emitterSpins(em);
            const float angle = spin ? p.life * p.spinSpeed + p.spinBase : 0.0f;
            const bool velQuad = (em.flags & mp::kFlagVelocityQuad) && glm::dot(p.velocity, p.velocity) > 2.4e-7f;
            for (int i = 0; i < 4; ++i) {
                glm::vec2 o;
                if (velQuad) {
                    glm::vec3 v3 = -vel;
                    o = mp::velocityCorner(i, size, glm::vec3(glm::dot(v3, right), glm::dot(v3, up),
                                                              glm::dot(v3, glm::cross(right, up))));
                } else {
                    o = mp::billboardCorner(i, size, spin, angle);
                }
                if (!velQuad && (em.flags & mp::kFlagXYQuad)) {
                    glm::vec3 ax(1, 0, 0), ay(0, 1, 0);
                    if (modelSpace) {
                        ax = glm::normalize(glm::vec3(pe.xf[0]));
                        ay = glm::normalize(glm::vec3(pe.xf[1]));
                    }
                    c[i] = pos + ax * o.x + ay * o.y;
                } else {
                    c[i] = pos + right * o.x + up * o.y;
                }
                uv[i] = mp::cellUV(static_cast<uint32_t>(std::max(cell, 0)), rows, cols, mp::kCornerUV[i]);
            }
            quad(c, uv, col);
        }
        if (em.flags & mp::kFlagTail) {
            float len = em.tailLength;
            if ((em.flags & mp::kFlagClampTailToAge) && p.life < len) len = p.life;
            const glm::vec3 tw3 = -vel * len;
            const glm::vec3 tv(glm::dot(tw3, right), glm::dot(tw3, up), 0.0f);
            const uint32_t cell = static_cast<uint32_t>(std::max(mp::particleCell(em.tailCell, t), 0));
            for (int i = 0; i < 4; ++i) {
                glm::vec3 o;
                if (mp::tailCorner(i, size, tv, o)) {
                    // o is in view space with the tail's own view offset;
                    // the tail is added in world space instead.
                    const glm::vec3 along = i >= 2 ? tw3 : glm::vec3(0.0f);
                    const glm::vec3 pv = i >= 2 ? o - tv : o;
                    const glm::vec3 across = right * pv.x + up * pv.y;
                    c[i] = pos + along + across;
                } else {
                    const glm::vec2 b = mp::billboardCorner(i, size, false, 0.0f);
                    c[i] = pos + right * b.x + up * b.y;
                }
                uv[i] = mp::cellUV(cell, rows, cols, mp::kCornerUV[i]);
            }
            quad(c, uv, col);
        }
    }
    if (written == 0) return;
    // Visible to the device whether or not the memory type is coherent.
    vmaFlushAllocation(vkCtx_->getAllocator(), vbAlloc_[fi], 0,
                       static_cast<VkDeviceSize>(written) * 9 * sizeof(float));

    struct VertPush {
        glm::mat4 viewProj;
        glm::vec4 unused0;
        glm::vec4 unused1;
    } vp{viewProj, glm::vec4(0.0f), glm::vec4(0.0f)};
    static_assert(sizeof(VertPush) == 96, "matches glue_particle.vert.glsl");

    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb_[fi], &off);
    VkPipeline bound = VK_NULL_HANDLE;
    for (const Run& r : runs) {
        if (r.count == 0) continue;
        const Emitter& e = emitters_[r.emitter];
        const auto& em = *e.data;
        // 3 and 4 are the additive blends.
        VkPipeline want = (em.blendingType == 3 || em.blendingType == 4) ? additivePipeline_
                                                                         : alphaPipeline_;
        if (want != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
            vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(vp), &vp);
            bound = want;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &e.texSet, 0, nullptr);
        struct { float tx, ty; int32_t alphaKey; } pc{1.0f, 1.0f, em.blendingType <= 1 ? 1 : 0};
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 96, sizeof(pc), &pc);
        vkCmdDraw(cmd, r.count * kVertsPerQuad, 1, r.first, 0);
    }
}

} // namespace rendering
} // namespace wowee
