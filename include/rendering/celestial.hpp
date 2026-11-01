#pragma once

#include <array>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "rendering/vk_texture.hpp"

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class VkContext;
struct SkyParams;

/**
 * The stock sun and moon (Vulkan).
 *
 * Four camera-facing sprites from the client's own textures, as 3.3.5 draws
 * them over the sky dome: Textures\SunCenter.blp with Textures\SunGlare.blp
 * around it, and Textures\Moon.blp with Textures\MoonGlare.blp. The sun is
 * tinted by LightIntBand 9 (the sun colour: a flat grey in the storm sets,
 * black in the death set, which hides it) and is up by dnc.db's day
 * intensity; the moon by its night intensity. One moon, as stock shows - the
 * procedural sun disc, the cratered phase-shaded moon and the second moon
 * this replaced were not the client's.
 *
 * Pipeline layout:
 *   set 0 = perFrameLayout  (camera UBO)
 *   set 1 = the sprite's texture
 *   push  = CelestialPush   (direction + size, colour, mode)
 */
class Celestial {
public:
    Celestial();
    ~Celestial();

    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    void shutdown();
    void recreatePipelines();

    /// Loads the four sprites, once; until then render() draws nothing.
    /// Safe to call every frame.
    void loadTextures(pipeline::AssetManager* assetManager);

    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const SkyParams& params);

    /**
     * Update celestial bodies (moon phase cycling).
     */
    void update(float deltaTime);

    // --- Enable / disable ---
    void setEnabled(bool enabled) { renderingEnabled_ = enabled; }
    [[nodiscard]] bool isEnabled() const { return renderingEnabled_; }

    // --- Moon phase (reported only: the stock moon sprite has no phases) ---
    [[nodiscard]] float getMoonPhase() const { return whiteLadyPhase_; }
    void setBlueChildPhase(float phase);
    [[nodiscard]] float getBlueChildPhase() const { return blueChildPhase_; }
    void setMoonPhaseCycling(bool enabled) { moonPhaseCycling_ = enabled; }
    [[nodiscard]] bool isMoonPhaseCycling() const { return moonPhaseCycling_; }

    /// Where the moon is: where the sun will be in twelve hours, by the same
    /// path LightingManager moves the sun along, so the two stay opposite.
    [[nodiscard]] static glm::vec3 moonDirection(float timeOfDayHours);

private:
    struct CelestialPush {
        glm::vec4 dirAndSize;  // xyz = unit direction to the sprite, w = half size (radians)
        glm::vec4 color;       // rgb = tint, a = opacity
        glm::vec4 mode;        // x = 1 alpha-blended, 0 additive
    };
    static_assert(sizeof(CelestialPush) == 48, "CelestialPush size mismatch");

    enum Sprite : uint32_t { SUN_CENTER = 0, SUN_GLARE, MOON, MOON_GLARE, SPRITE_COUNT };

    VkPipeline buildPipeline(VkDevice device,
                             const VkPipelineShaderStageCreateInfo& vertStage,
                             const VkPipelineShaderStageCreateInfo& fragStage);
    void drawSprite(VkCommandBuffer cmd, Sprite sprite, const glm::vec3& dir, float halfSize,
                    const glm::vec4& color, bool alphaBlended);

    VkContext*            vkCtx_          = nullptr;
    VkPipeline            pipeline_       = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout textureSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      descriptorPool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, SPRITE_COUNT> textureSets_{};
    std::array<VkTexture, SPRITE_COUNT> textures_;
    std::array<bool, SPRITE_COUNT> textureReady_{};
    bool texturesAttempted_ = false;

    bool renderingEnabled_ = true;

    // Moon phase bookkeeping, for the performance HUD.
    float whiteLadyPhase_ = 0.5f;
    float blueChildPhase_ = 0.25f;
    bool  moonPhaseCycling_ = true;
    float moonPhaseTimer_   = 0.0f;
    static constexpr float MOON_CYCLE_DURATION = 240.0f;
};

} // namespace rendering
} // namespace wowee
