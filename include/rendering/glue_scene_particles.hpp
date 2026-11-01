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
 * CharacterRenderer animates, and draws point sprites with the M2 particle
 * shaders through the preview's own render pass.
 */
class GlueSceneParticles {
public:
    GlueSceneParticles() = default;
    ~GlueSceneParticles();
    GlueSceneParticles(const GlueSceneParticles&) = delete;
    GlueSceneParticles& operator=(const GlueSceneParticles&) = delete;

    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                    VkRenderPass renderPass, VkSampleCountFlagBits samples);
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

    /// Record the draw, inside the preview's render pass.
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, uint32_t frameIndex);

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

    static constexpr uint32_t kFrames = 2;
    static constexpr uint32_t kMaxParticles = 8192;

    VkContext* vkCtx_ = nullptr;
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
