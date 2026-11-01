#pragma once

#include "pipeline/m2_loader.hpp"
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace rendering {

class VkContext;
class VkTexture;
class CharacterRenderer;

/**
 * The particle emitters of a glue scene (character select/create backdrops,
 * the login screen), drawn into the off-screen preview pass.
 *
 * The scenes are drawn by the preview's CharacterRenderer, which has no
 * particle system: the death knight scene's mist and brazier glow on its
 * platform, the night elf wisps, the orc dust, the undead torches, the
 * draenei sparks and the blood elf leaves were all missing. M2Renderer's
 * system is tied to the world pass and its tens of thousands of instances,
 * so this is a small one for a single scene: it reads the emitters off the
 * scene model, takes the bone palette and clocks from the scene instance the
 * CharacterRenderer animates, and draws point sprites through the preview's
 * own render pass with its own shaders (glue_particle.vert/frag), which take
 * everything from push constants and bind nothing but the particle texture.
 *
 * The first version reused the M2 particle shaders and bound the preview's
 * per-frame set; the first Night Elf scene frame lost the device. That
 * version is gone along with every input it shared with the world path, and
 * every vertex is checked finite and clamped before it is written.
 */
class GlueSceneParticles {
public:
    GlueSceneParticles() = default;
    ~GlueSceneParticles();
    GlueSceneParticles(const GlueSceneParticles&) = delete;
    GlueSceneParticles& operator=(const GlueSceneParticles&) = delete;

    bool initialize(VkContext* ctx, VkRenderPass renderPass, VkSampleCountFlagBits samples);
    void shutdown();

    /// Take the scene's emitters (none clears). Textures are loaded through
    /// the CharacterRenderer's cache, so it must outlive this object's use.
    void setScene(const pipeline::M2Model& model, CharacterRenderer& textures);
    void clear();
    [[nodiscard]] bool empty() const { return emitters_.empty(); }

    /// Emit and age particles. bones/modelMatrix/clocks come from the scene
    /// instance (CharacterRenderer::getInstancePose).
    void update(float dt, const std::vector<glm::mat4>& bones, const glm::mat4& modelMatrix,
                int sequenceIndex, float animTimeMs, float globalSeqTimeMs);

    /// Record the draw, inside the preview's render pass. pixelsPerUnit is
    /// the point size in pixels of a one-unit particle one unit away
    /// (projection[1][1] * target height / 2).
    void render(VkCommandBuffer cmd, const glm::mat4& viewProj, const glm::vec3& cameraPos,
                float pixelsPerUnit, uint32_t frameIndex);

private:
    struct Particle {
        glm::vec3 position;
        glm::vec3 velocity;
        float life = 0.0f;
        float maxLife = 1.0f;
        uint16_t emitter = 0;
        uint16_t tile = 0;
    };
    struct Emitter {
        const pipeline::M2ParticleEmitter* data = nullptr;  // into model_ copy
        VkTexture* texture = nullptr;
        VkDescriptorSet texSet = VK_NULL_HANDLE;
        float accumulator = 0.0f;
    };

    VkDescriptorSet textureSet(VkTexture* tex);

    static constexpr uint32_t kFrames = 2;  // MAX_FRAMES_IN_FLIGHT
    static constexpr uint32_t kMaxParticles = 8192;

    VkContext* vkCtx_ = nullptr;
    float maxPointSize_ = 64.0f;  // the device's point size limit, capped
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline alphaPipeline_ = VK_NULL_HANDLE;
    VkPipeline additivePipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout texLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool texPool_ = VK_NULL_HANDLE;
    std::unordered_map<VkTexture*, VkDescriptorSet> texSets_;
    ::VkBuffer vb_[kFrames] = {};  // the Vulkan handle, not rendering::VkBuffer
    VmaAllocation vbAlloc_[kFrames] = {};
    void* vbMapped_[kFrames] = {};

    std::vector<pipeline::M2ParticleEmitter> emitterData_;
    std::vector<uint32_t> globalSeqDurations_;
    std::vector<Emitter> emitters_;
    std::vector<Particle> particles_;
    std::mt19937 rng_{0x61756e63u};
};

} // namespace rendering
} // namespace wowee
