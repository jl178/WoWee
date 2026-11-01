#pragma once

#include <memory>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace wowee {
namespace rendering {

class Camera;
class VkContext;
class Skybox;
class Celestial;
class StarField;
class Clouds;
class LensFlare;
class LightingManager;

/**
 * Sky rendering parameters (extracted from LightingManager)
 */
struct SkyParams {
    // Sun/moon positioning
    glm::vec3 directionalDir{0.0f, -1.0f, 0.3f};
    glm::vec3 sunColor{1.0f, 1.0f, 0.9f};

    // Sky colors (for skybox tinting/blending)
    glm::vec3 skyTopColor{0.5f, 0.7f, 1.0f};
    glm::vec3 skyMiddleColor{0.7f, 0.85f, 1.0f};
    glm::vec3 skyBand1Color{0.9f, 0.95f, 1.0f};
    glm::vec3 skyBand2Color{1.0f, 0.98f, 0.9f};
    // The horizon (LightIntBand 6) and the authored fog below it (7): the
    // stock dome's last two stops.
    glm::vec3 skySmogColor{0.8f, 0.85f, 0.9f};
    glm::vec3 skyFogColor{0.5f, 0.6f, 0.7f};

    // Cloud light, LightIntBand 10-13.
    glm::vec3 cloudSunColor{1.0f, 0.78f, 0.54f};
    glm::vec3 cloudEmissiveColor{0.17f, 0.41f, 0.52f};
    glm::vec3 cloudLayer1AmbientColor{0.0f};
    glm::vec3 cloudLayer2AmbientColor{0.38f, 0.51f, 0.72f};
    /// LightFloatBand 2: how much the sun and moon show through cloud.
    float celestialGlowThrough = 1.0f;
    /// dnc.db: how far the sun, and the moon, are up (0..1).
    float dayIntensity = 1.0f;
    float nightIntensity = 0.0f;

    // Atmospheric effects
    float cloudDensity = 0.0f;      // 0-1
    float fogDensity = 0.0f;        // 0-1
    float horizonGlow = 0.3f;       // 0-1
    float weatherIntensity = 0.0f;  // 0-1 (rain/snow intensity, attenuates lens flare)
    // How much of the line from the eye to the sun is blocked, 0 clear to 1
    // solid. The lens flare had no such input: it asked only whether the sun
    // was in front of the camera and on screen, so a hillside, a building or a
    // ceiling between the two changed nothing and the flare hung over the
    // terrain that was covering it.
    float sunOcclusion = 0.0f;

    // Time
    float timeOfDay = 12.0f;    // 0-24 hours
    float gameTime = -1.0f;     // Server game time, hours since midnight (-1 = use fallback)

    // Skybox selection (future: from LightSkybox.dbc)
    uint32_t skyboxModelId = 0;
    bool skyboxHasStars = false;  // Does loaded skybox include baked stars?
    bool useOriginalSkybox = false; // Original camera-centered client M2 is active
};

/**
 * Unified sky rendering system
 *
 * Coordinates skybox (authoritative), celestial bodies (sun + 2 moons),
 * and fallback procedural stars. Driven by lighting system data.
 *
 * Architecture:
 * - Skybox is PRIMARY (includes baked stars from M2 models)
 * - Celestial renders sun + White Lady + Blue Child
 * - StarField is DEBUG/FALLBACK only (disabled when skybox has stars)
 */
class SkySystem {
public:
    SkySystem();
    ~SkySystem();

    /**
     * Initialize sky system components.
     * @param ctx            Vulkan context (required for Vulkan renderers)
     * @param perFrameLayout Descriptor set layout for set 0 (camera UBO)
     */
    bool initialize(VkContext* ctx = nullptr, VkDescriptorSetLayout perFrameLayout = VK_NULL_HANDLE);
    void shutdown();

    /**
     * Update sky system (time, moon phases, etc.)
     */
    void update(float deltaTime);

    /**
     * Render complete sky.
     * @param cmd         Active Vulkan command buffer
     * @param perFrameSet Per-frame descriptor set (set 0, camera UBO)
     * @param camera      Camera for legacy sub-renderers (lens flare, etc.)
     * @param params      Sky parameters from lighting system
     */
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                const Camera& camera, const SkyParams& params);

    /// The sky in two halves, for a caller that draws the client's sky models
    /// (LightSkybox, and the night stars) between them: the dome first, the
    /// models over it, then the sun, moon and clouds over those - so cloud
    /// covers the stars as it does in the stock client. render() is both.
    void renderDome(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const SkyParams& params);
    void renderOverlays(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                        const Camera& camera, const SkyParams& params);

    /**
     * Enable/disable procedural stars (DEBUG/FALLBACK)
     * Default: OFF (stars come from skybox)
     */
    void setProceduralStarsEnabled(bool enabled) { proceduralStarsEnabled_ = enabled; }
    [[nodiscard]] bool isProceduralStarsEnabled() const { return proceduralStarsEnabled_; }

    /**
     * Enable/disable debug sky mode (forces procedural stars even with skybox)
     */
    void setDebugSkyMode(bool enabled) { debugSkyMode_ = enabled; }
    [[nodiscard]] bool isDebugSkyMode() const { return debugSkyMode_; }

    /**
     * Enable/disable moon phase cycling
     */
    void setMoonPhaseCycling(bool enabled);

    void setBlueChildPhase(float phase);

    [[nodiscard]] float getBlueChildPhase() const;

    // Component accessors (for direct control if needed)
    [[nodiscard]] Skybox*    getSkybox()    const { return skybox_.get(); }
    [[nodiscard]] Celestial* getCelestial() const { return celestial_.get(); }
    [[nodiscard]] StarField* getStarField() const { return starField_.get(); }
    [[nodiscard]] Clouds*    getClouds()    const { return clouds_.get(); }
    [[nodiscard]] LensFlare* getLensFlare() const { return lensFlare_.get(); }

private:
    std::unique_ptr<Skybox>    skybox_;      // Authoritative sky
    std::unique_ptr<Celestial> celestial_;   // Sun + 2 moons
    std::unique_ptr<StarField> starField_;   // Fallback procedural stars
    std::unique_ptr<Clouds>    clouds_;      // Cloud layer
    std::unique_ptr<LensFlare> lensFlare_;   // Sun lens flare

    bool proceduralStarsEnabled_ = false;
    bool debugSkyMode_ = false;
    bool initialized_ = false;
};

} // namespace rendering
} // namespace wowee
