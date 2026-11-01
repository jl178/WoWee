#include "rendering/sky_system.hpp"
#include "rendering/sun_direction.hpp"
#include "rendering/skybox.hpp"
#include "rendering/celestial.hpp"
#include "rendering/starfield.hpp"
#include "rendering/clouds.hpp"
#include "rendering/lens_flare.hpp"
#include "rendering/camera.hpp"
#include "rendering/vk_context.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"

namespace wowee {
namespace rendering {

SkySystem::SkySystem() = default;

SkySystem::~SkySystem() {
    shutdown();
}

bool SkySystem::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    if (initialized_) {
        LOG_WARNING("SkySystem already initialized");
        return true;
    }

    LOG_INFO("Initializing sky system");

    // Skybox (Vulkan)
    skybox_ = std::make_unique<Skybox>();
    if (!skybox_->initialize(ctx, perFrameLayout)) {
        LOG_ERROR("Failed to initialize skybox");
        return false;
    }

    // Celestial bodies - sun + 2 moons (Vulkan)
    celestial_ = std::make_unique<Celestial>();
    if (!celestial_->initialize(ctx, perFrameLayout)) {
        LOG_ERROR("Failed to initialize celestial bodies");
        return false;
    }

    // Procedural stars - fallback / debug (Vulkan)
    starField_ = std::make_unique<StarField>();
    if (!starField_->initialize(ctx, perFrameLayout)) {
        LOG_ERROR("Failed to initialize star field");
        return false;
    }
    starField_->setEnabled(false); // Off by default; skybox is authoritative

    // Clouds (Vulkan)
    clouds_ = std::make_unique<Clouds>();
    if (!clouds_->initialize(ctx, perFrameLayout)) {
        LOG_ERROR("Failed to initialize clouds");
        return false;
    }

    // Lens flare (Vulkan)
    lensFlare_ = std::make_unique<LensFlare>();
    if (!lensFlare_->initialize(ctx, perFrameLayout)) {
        LOG_ERROR("Failed to initialize lens flare");
        return false;
    }

    initialized_ = true;
    LOG_INFO("Sky system initialized successfully");
    return true;
}

void SkySystem::shutdown() {
    if (!initialized_) {
        return;
    }

    LOG_INFO("Shutting down sky system");

    if (lensFlare_)  lensFlare_->shutdown();
    if (clouds_)     clouds_->shutdown();
    if (starField_)  starField_->shutdown();
    if (celestial_)  celestial_->shutdown();
    if (skybox_)     skybox_->shutdown();

    lensFlare_.reset();
    clouds_.reset();
    starField_.reset();
    celestial_.reset();
    skybox_.reset();

    initialized_ = false;
}

void SkySystem::update(float deltaTime) {
    if (!initialized_) {
        return;
    }

    if (skybox_)    skybox_->update(deltaTime);
    if (celestial_) {
        // The stock sprites, loaded the first time there is an asset manager
        // - here rather than while the sky is being recorded.
        celestial_->loadTextures(core::Application::getInstance().getAssetManager());
        celestial_->update(deltaTime);
    }
    if (starField_) starField_->update(deltaTime);
    if (clouds_)    clouds_->update(deltaTime);
}

void SkySystem::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                        const Camera& camera, const SkyParams& params) {
    renderDome(cmd, perFrameSet, params);
    renderOverlays(cmd, perFrameSet, camera, params);
}

void SkySystem::renderDome(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                           const SkyParams& params) {
    if (!initialized_) return;

    // --- The dome: Light.dbc's six sky bands ---
    if (skybox_) {
        skybox_->render(cmd, perFrameSet, params);
    }
}

void SkySystem::renderOverlays(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                               const Camera& /*camera*/, const SkyParams& params) {
    if (!initialized_) return;

    // --- Procedural stars: only when asked for ---
    // The stock night sky is Stars.mdx, which LightingManager puts up as a sky
    // model layer at dnc.db's night intensity. These are the "sharp stars"
    // option (and the debug view), drawn where M2Renderer drops a sky model's
    // star batches - not a fallback under every sky without a model.
    const bool renderProceduralStars = debugSkyMode_ || proceduralStarsEnabled_;
    if (starField_) {
        starField_->setEnabled(renderProceduralStars);
        if (renderProceduralStars) {
            starField_->render(cmd, perFrameSet, params.timeOfDay,
                               params.cloudDensity, params.fogDensity);
        }
    }

    // Original client sky M2s (LightSkybox) carry their own sun, moons and
    // clouds; the caller has drawn that model over the dome already.
    if (params.useOriginalSkybox) return;

    // --- Sun and moon: the client's sprites ---
    if (celestial_) {
        celestial_->render(cmd, perFrameSet, params);
    }

    // --- Clouds: Light.dbc's density and colours. A storm's overcast comes
    // from its storm set, which LightingManager blends in by the weather's
    // intensity, so nothing is added for the weather here. ---
    if (clouds_) {
        clouds_->setDensity(params.cloudDensity);
        clouds_->render(cmd, perFrameSet, params);
    }

    // No lens flare: the stock sun has its glare sprite and nothing more.
}

glm::vec3 SkySystem::getSunPosition(const SkyParams& params) const {
    // Below the horizon it stays below - see sun_direction.hpp for what the
    // mirror that used to be here cost.
    return sunDirectionFromLightDir(params.directionalDir) * 800.0f;
}

void SkySystem::setMoonPhaseCycling(bool enabled) {
    if (celestial_) celestial_->setMoonPhaseCycling(enabled);
}
void SkySystem::setBlueChildPhase(float phase) {
    if (celestial_) celestial_->setBlueChildPhase(phase);
}
float SkySystem::getBlueChildPhase() const {
    return celestial_ ? celestial_->getBlueChildPhase() : 0.25f;
}

} // namespace rendering
} // namespace wowee
