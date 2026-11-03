#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <array>
#include <memory>
#include <random>
#include <vector>

#include "rendering/stock_particulates.hpp"
#include "rendering/vk_texture.hpp"

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class Camera;
class CameraController;
class WaterRenderer;
class M2Renderer;
class VkContext;

class SwimEffects {
public:
    SwimEffects();
    ~SwimEffects();

    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    void shutdown();
    void recreatePipelines();
    /// The three pipelines both initialize() and recreatePipelines() need.
    void buildRipplePipeline(
        VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
        const VkPipelineShaderStageCreateInfo& fragStage,
        const VkVertexInputBindingDescription& binding,
        const std::vector<VkVertexInputAttributeDescription>& attrs);
    void buildBubblePipeline(
        VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
        const VkPipelineShaderStageCreateInfo& fragStage,
        const VkVertexInputBindingDescription& binding,
        const std::vector<VkVertexInputAttributeDescription>& attrs);
    void buildInsectPipeline(
        VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
        const VkPipelineShaderStageCreateInfo& fragStage,
        const VkVertexInputBindingDescription& binding,
        const std::vector<VkVertexInputAttributeDescription>& attrs);

    /// Select the render pass these particles will be recorded into. Water now
    /// draws after them in a pass of its own, so the spray has to move into that
    /// pass too or the water sheet is painted straight over it. Call before
    /// initialize() or recreatePipelines().
    void setTargetPass(VkRenderPass pass, VkSampleCountFlagBits samples) {
        targetPass_ = pass;
        targetSamples_ = samples;
    }
    void update(const Camera& camera, const CameraController& cc,
                const WaterRenderer& water, float deltaTime);
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);
    void spawnFootSplash(const glm::vec3& footPos, float waterH);
    void setM2Renderer(M2Renderer* renderer) { m2Renderer = renderer; }

    /// The "Splashes, wakes and bubbles" setting: the spray, rings, bubbles
    /// and reed midges this class throws. None of them is in the stock client
    /// (its ripple decals are off by default, docs/re/water.md), so they are
    /// off unless asked for. The stock motes below do not depend on it.
    void setSplashesEnabled(bool enabled);
    /// The "Insects over water" setting: midges over reeds and still water.
    /// Split from the splashes (docs/enhancements-plan.md 2.1 #13): they add
    /// creatures the zone data does not place. Off by default.
    void setInsectsEnabled(bool enabled);

    /// The stock underwater particulates (stock_particulates.hpp): which
    /// liquid the camera is in this frame and what its LiquidType row says.
    /// `show` false (not in liquid, or a liquid without Flags 0x8) hides them.
    struct MoteLiquid {
        bool show = false;
        uint32_t liquidType = 0;
        float particleScale = 1.0f;  // LiquidType ParticleScale
        int movement = 0;            // ParticleMovement
        int texSlots = 0;            // ParticleTexSlots
        bool fogged = false;         // Flags 0x10
    };
    void setCameraLiquid(const MoteLiquid& liquid) { moteLiquid_ = liquid; }
    /// Textures\WaterPoop02.blp, once.
    void loadMoteTexture(pipeline::AssetManager* assetManager);

private:
    struct Particle {
        glm::vec3 position;
        glm::vec3 velocity;
        float lifetime;
        float maxLifetime;
        float size;
        float alpha;
    };

    struct InsectParticle {
        glm::vec3 position;
        glm::vec3 orbitCenter;  // vegetation position to orbit around
        float lifetime;
        float maxLifetime;
        float size;
        float alpha;
        float phase;       // random phase offset for erratic motion
        float orbitRadius;
        float orbitSpeed;
        float heightOffset; // height above plant
    };

    static constexpr int MAX_RIPPLE_PARTICLES = 200;
    static constexpr int MAX_BUBBLE_PARTICLES = 150;
    static constexpr int MAX_INSECT_PARTICLES = 50;

    std::vector<Particle> ripples;
    std::vector<Particle> bubbles;
    std::vector<InsectParticle> insects;

    // Vulkan objects
    VkContext* vkCtx = nullptr;
    M2Renderer* m2Renderer = nullptr;

    // Ripple pipeline + dynamic buffer
    VkPipeline ripplePipeline = VK_NULL_HANDLE;
    VkPipelineLayout ripplePipelineLayout = VK_NULL_HANDLE;
    ::VkBuffer rippleDynamicVB = VK_NULL_HANDLE;
    VmaAllocation rippleDynamicVBAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo rippleDynamicVBAllocInfo{};
    VkDeviceSize rippleDynamicVBSize = 0;

    // Bubble pipeline + dynamic buffer
    VkPipeline bubblePipeline = VK_NULL_HANDLE;
    VkPipelineLayout bubblePipelineLayout = VK_NULL_HANDLE;
    ::VkBuffer bubbleDynamicVB = VK_NULL_HANDLE;
    VmaAllocation bubbleDynamicVBAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo bubbleDynamicVBAllocInfo{};
    VkDeviceSize bubbleDynamicVBSize = 0;

    // Insect pipeline + dynamic buffer
    VkPipeline insectPipeline = VK_NULL_HANDLE;
    VkPipelineLayout insectPipelineLayout = VK_NULL_HANDLE;
    ::VkBuffer insectDynamicVB = VK_NULL_HANDLE;
    VmaAllocation insectDynamicVBAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo insectDynamicVBAllocInfo{};
    VkDeviceSize insectDynamicVBSize = 0;

    std::vector<float> rippleVertexData;
    std::vector<float> bubbleVertexData;
    std::vector<float> insectVertexData;

    VkRenderPass targetPass_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits targetSamples_ = VK_SAMPLE_COUNT_1_BIT;

    float wadeSpawnAccum = 0.0f;
    float rippleSpawnAccum = 0.0f;
    float bubbleSpawnAccum = 0.0f;
    float insectSpawnAccum = 0.0f;

    // --- Stock underwater particulates ---
    void buildMotePipeline(VkDevice device);
    void updateMotes(const Camera& camera, float deltaTime);
    void clearSplashes();

    bool splashesEnabled_ = false;
    bool insectsEnabled_ = false;
    /// The enhancement parts' pipelines and buffers, made on first use and
    /// released after a few frames off (update).
    bool ensureSplashResources();
    bool ensureInsectResources();
    void releaseSplashResources();
    void releaseInsectResources();
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;
    uint32_t splashIdleFrames_ = 0;
    uint32_t insectIdleFrames_ = 0;
    bool splashResourcesFailed_ = false;
    bool insectResourcesFailed_ = false;
    MoteLiquid moteLiquid_{};
    uint32_t moteLiquidShown_ = 0;           // the liquid the set was made for
    std::vector<stock_particulates::Mote> motes_;
    stock_particulates::Current moteCurrent_{};
    glm::vec3 moteLastCamera_{0.0f};
    bool motesValid_ = false;
    std::mt19937 moteRng_{0x5eedu};
    float moteRand() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(moteRng_); }
    std::vector<float> moteVertexData;      // pos3 + uv2 per vertex, 6 a mote
    VkPipeline motePipeline = VK_NULL_HANDLE;
    VkPipelineLayout motePipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout moteSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool motePool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 1> moteSet_{};
    VkTexture moteTexture_;
    bool moteTextureReady_ = false;
    bool moteTextureAttempted_ = false;
    ::VkBuffer moteDynamicVB = VK_NULL_HANDLE;
    VmaAllocation moteDynamicVBAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo moteDynamicVBAllocInfo{};

    void spawnRipple(const glm::vec3& pos, const glm::vec3& moveDir, float waterH);
    void spawnBubble(const glm::vec3& pos, float waterH);
    void spawnInsect(const glm::vec3& vegPos);
};

} // namespace rendering
} // namespace wowee
