#pragma once

/// The dust a footfall on dry ground throws up (docs/enhancements-plan.md E21,
/// the "Dust kicked up" row): a few soft, lit, fogged puffs per step, for every
/// player, creature and mount near the camera. What a step throws up, and on
/// which ground, is footfall_dust_rules.hpp; Renderer::updateFootfallDust
/// feeds this from the models' own $FSD footfall events. It replaced the
/// mount-only MountDust, which sprayed fixed-size points from the player's
/// mount on a timer.
///
/// Made on first use and released a few frames after the row goes off; with
/// it off nothing here exists (Renderer::updateFootfallDust).

#include "rendering/footfall_dust_rules.hpp"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <random>
#include <vector>

namespace wowee {
namespace rendering {

class Camera;
class VkContext;

class FootfallDust {
public:
    FootfallDust() = default;
    ~FootfallDust();

    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    void shutdown();
    void recreatePipelines();

    /// One footfall's puff at `foot` (the walker's position on the ground),
    /// `moveDir` its horizontal direction of travel (zero if unknown).
    void spawn(const glm::vec3& foot, const glm::vec2& moveDir,
               const footfall_dust::Puff& puff, const glm::vec3& colour);

    /// Age, drift and fade the puffs, and build this frame's camera-facing
    /// quads.
    void update(float deltaTime, const Camera& camera);
    void clear() { sprites_.clear(); vertices_.clear(); }
    [[nodiscard]] size_t spriteCount() const { return sprites_.size(); }
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

private:
    void buildPipeline();

    struct Sprite {
        glm::vec3 pos;
        glm::vec3 vel;
        glm::vec3 colour;
        float age;
        float life;
        float size;
        float alpha;
        float seed;
    };
    struct Vertex {
        glm::vec3 pos;
        glm::vec2 uv;
        glm::vec4 colourAlpha;  // rgb colour, a opacity
        float seed;
    };
    static constexpr uint32_t kMaxSprites = 512;

    std::vector<Sprite> sprites_;
    std::vector<Vertex> vertices_;
    std::mt19937 rng_{0x5eedu};

    VkContext* vkCtx_ = nullptr;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    struct FrameBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        void* mapped = nullptr;
    };
    std::vector<FrameBuffer> frameBuffers_;
    VkDeviceSize frameBufferSize_ = 0;
};

}  // namespace rendering
}  // namespace wowee
