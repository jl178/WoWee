// Renderer's part of three enhancements: contact shadows (E14), ambient
// occlusion and the room light indoors (E12). Kept out of renderer.cpp; see contact_shadows.hpp and
// indoor_probes.hpp for what each does.
#include "rendering/renderer.hpp"

#include "rendering/bounce_grid_gpu.hpp"
#include "rendering/camera.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/contact_shadow_pass.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/weather.hpp"
#include "rendering/wmo_renderer.hpp"
#include "core/logger.hpp"

#include <glm/gtc/matrix_inverse.hpp>

#include <cstdlib>
#include <cstring>

namespace wowee {
namespace rendering {

// Contact shadows and ambient occlusion are the same kind of pass: a
// full-screen triangle over the scene's resolved colour once the scene pass
// has closed, reading its one-sample depth (as the soft particles do),
// blended dst * src. Both need the overlay pass and that depth; each is let
// go eight frames after it stops being wanted.
bool Renderer::depthLightPassPossible() const {
    if (!vkCtx || vkCtx->getOverlayRenderPass() == VK_NULL_HANDLE) return false;
    const bool offscreen = postProcessPipeline_ &&
                           postProcessPipeline_->getSceneFramebuffer() != VK_NULL_HANDLE;
    if (offscreen) {
        return postProcessPipeline_->getSceneColorImageView() != VK_NULL_HANDLE &&
               postProcessPipeline_->getSceneDepthImage() != VK_NULL_HANDLE &&
               !postProcessPipeline_->sceneDepthIsMsaa();
    }
    return !vkCtx->getOverlayFramebuffers().empty() && !vkCtx->isDepthCopySourceMsaa();
}

namespace {
SoftParticlePass::Target depthLightPassTarget(VkContext* vkCtx, PostProcessPipeline* postProcessPipeline_,
                                              uint32_t currentImageIndex) {
    SoftParticlePass::Target t;
    t.pass = vkCtx->getSceneOverlayRenderPass();
    if (postProcessPipeline_ && postProcessPipeline_->getSceneFramebuffer() != VK_NULL_HANDLE) {
        t.colorView = postProcessPipeline_->getSceneColorImageView();
        t.extent = postProcessPipeline_->getSceneRenderExtent();
        t.depth = postProcessPipeline_->getSceneDepthImage();
    } else {
        const auto& fbs = vkCtx->getOverlayFramebuffers();
        if (currentImageIndex < fbs.size()) t.framebuffer = fbs[currentImageIndex];
        t.extent = vkCtx->getSwapchainExtent();
        t.depth = vkCtx->getDepthCopySourceImage();
    }
    return t;
}
}  // namespace

bool Renderer::decideDepthLightPass(bool want, DepthLightPassState& s, const char* fragSpv, const char* name,
                                    int blend, VkDescriptorSetLayout extraLayout) {
    s.frame = false;
    s.done = false;
    want = want && depthLightPassPossible();
    if (want) {
        if (s.pass && std::strcmp(s.pass->fragSpv(), fragSpv) != 0) {
            // Another shader (the ambient pass with or without the bounce
            // grid): the frames in flight may still draw with the one made.
            std::shared_ptr<ContactShadowPass> prev(std::move(s.pass));
            vkCtx->deferAfterAllFrameFences([prev]() mutable { prev.reset(); });
        }
        if (!s.pass) {
            s.pass = std::make_unique<ContactShadowPass>(fragSpv, name,
                                                         static_cast<ContactShadowPass::Blend>(blend), extraLayout);
        }
        want = s.pass->ensure(vkCtx, perFrameSetLayout, vkCtx->getSceneOverlayRenderPass());
    }
    s.frame = want;
    if (want) {
        s.offFrames = 0;
    } else if (s.pass && ++s.offFrames > 8) {
        s.pass.reset();
        s.offFrames = 0;
    }
    return want;
}

// Contact shadows also need the sun's shadows on and the player not wholly
// indoors, where the sun lights nothing.
void Renderer::decideContactShadows() {
    decideDepthLightPass(enh_.contactShadows && shadowsEnabled && indoorFactor_ < 0.99f, contactShadows_,
                         "assets/shaders/contact_shadow.frag.spv", "Contact shadows",
                         static_cast<int>(ContactShadowPass::Blend::Multiply));
    // Ambient occlusion, the fill light and the bounce light share a pass:
    // all change only the ambient share of the light, indoors and out. With
    // the bounce light it is the BOUNCE_GRID build, reading the grid.
    bool bounce = enh_.bounceLight && depthLightPassPossible();
    if (bounce) {
        if (!bounceGrid_) bounceGrid_ = std::make_unique<BounceGrid>();
        bounce = bounceGrid_->ensure(vkCtx);
        bounceOffFrames_ = 0;
    } else if (bounceGrid_ && ++bounceOffFrames_ > 8) {
        // Eight frames on, no frame in flight reads it; the pass that did
        // was remade without it when the row went off.
        std::shared_ptr<BounceGrid> prev(std::move(bounceGrid_));
        vkCtx->deferAfterAllFrameFences([prev]() mutable { prev.reset(); });
        bounceOffFrames_ = 0;
    }
    bounceFrame_ = decideDepthLightPass(enh_.ambientOcclusion || enh_.fillLight || bounce, ambientOcclusion_,
                                        bounce ? "assets/shaders/ambient_bounce.frag.spv"
                                               : "assets/shaders/ambient_occlusion.frag.spv",
                                        "Ambient occlusion",
                                        static_cast<int>(ContactShadowPass::Blend::Multiply2x),
                                        bounce ? bounceGrid_->setLayout() : VK_NULL_HANDLE) &&
                   bounce;
    // The Enhanced haze lays light over everything the opaque pass drew.
    decideDepthLightPass(enh_.sunHaze && indoorFactor_ < 0.99f, sunHaze_,
                         "assets/shaders/enhanced_haze.frag.spv", "Sun haze",
                         static_cast<int>(ContactShadowPass::Blend::OverTransmittance));
}

void Renderer::recordContactShadowPass(VkCommandBuffer cmd) {
    if (!worldDrawnThisFrame_ || !vkCtx || cmd == VK_NULL_HANDLE) return;
    const glm::mat4 invViewProj = glm::inverse(currentFrameData.projection * currentFrameData.view);
    const VkDescriptorSet perFrame = perFrameDescSets[vkCtx->getCurrentFrame()];
    const SoftParticlePass::Target target = depthLightPassTarget(vkCtx, postProcessPipeline_.get(), currentImageIndex);
    // Occlusion first: it scales the sky's light, which the contact shadow's
    // ratio of lit to unlit then reads as it stands.
    if (ambientOcclusion_.frame && !ambientOcclusion_.done && ambientOcclusion_.pass) {
        ambientOcclusion_.done = true;
        int flags = (enh_.ambientOcclusion ? 1 : 0) | (enh_.fillLight ? 2 : 0);
        const float wx = weather ? glm::clamp(weather->getIntensity(), 0.0f, 1.0f) : 0.0f;
        glm::vec4 extra(0.0f);
        VkDescriptorSet gridSet = VK_NULL_HANDLE;
        if (bounceFrame_ && bounceGrid_) {
            // The bounce light: flag 8; the grid's corner and whether it
            // holds a bake yet (until it does, the plain fill).
            bounceGrid_->recordUpload(cmd, vkCtx->getCurrentFrame());
            // WOWEE_BOUNCE_DEBUG=1 draws the grid over the picture (flag 16).
            static const bool kBounceDebug = std::getenv("WOWEE_BOUNCE_DEBUG") != nullptr;
            flags |= 8 | (kBounceDebug ? 16 : 0);
            extra = glm::vec4(bounceGrid_->origin(), bounceGrid_->ready() ? 1.0f : 0.0f, 0.0f);
            gridSet = bounceGrid_->set();
        }
        if (ambientOcclusion_.pass->record(cmd, target, perFrame, invViewProj, 1.0f - 0.7f * wx, flags, extra,
                                           gridSet)) {
            vkCtx->gpuMark(cmd, "ambient occlusion");
        }
    }
    if (contactShadows_.frame && !contactShadows_.done && contactShadows_.pass) {
        contactShadows_.done = true;
        // Light.dbc's shadow band x the player's setting, as the shadow map's
        // (shadowParams.y), and none of it indoors.
        const float darkness = currentFrameData.shadowParams.y * (1.0f - glm::clamp(indoorFactor_, 0.0f, 1.0f));
        if (contactShadows_.pass->record(cmd, target, perFrame, invViewProj, darkness)) {
            vkCtx->gpuMark(cmd, "contact shadows");
        }
    }
    if (sunHaze_.frame && !sunHaze_.done && sunHaze_.pass) {
        sunHaze_.done = true;
        // Rain and snow already close the distance in with their own fog
        // and sheets: the golden air is a clear day's, so it thins with the
        // weather rather than adding a grey layer to it.
        const float wx = weather ? glm::clamp(weather->getIntensity(), 0.0f, 1.0f) : 0.0f;
        // Flag 8: the HDR scene, where the haze's light may pass 1.3.
        const int hazeFlags = vkCtx->isHdrScene() ? 8 : 0;
        if (sunHaze_.pass->record(cmd, target, perFrame, invViewProj, 1.0f - 0.8f * wx, hazeFlags)) {
            vkCtx->gpuMark(cmd, "sun haze");
        }
    }
}

void Renderer::updateBounceGrid(float dt) {
    if (!bounceGrid_ || !enh_.bounceLight || !camera) return;
    bounceGrid_->update(camera->getPosition(), dt, m2Renderer.get(), wmoRenderer.get(), cachedAssetManager,
                        terrainManager ? terrainManager->getMapName() : std::string());
}

void Renderer::updateUnitIndoorLight(float fxDt) {
    if (!characterRenderer) return;
    if (!enh_.indoorLight || !wmoRenderer) {
        characterRenderer->updateIndoorProbes(fxDt, nullptr, glm::vec3(0.0f));
        return;
    }
    const CharacterRenderer::IndoorProbeFn probe = [this](const glm::vec3& pos, glm::vec3& colour,
                                                          glm::vec2& lean) {
        return wmoRenderer->sampleIndoorProbe(pos, colour, lean);
    };
    // The eases run on the frame's delta even with the clock frozen: a
    // unit walking into the inn under WOWEE_FREEZE_CLOCK should still take
    // its light.
    characterRenderer->updateIndoorProbes(fxDt > 0.0f ? fxDt : 1.0f / 60.0f, &probe,
                                          camera ? camera->getPosition() : glm::vec3(0.0f));
}

}  // namespace rendering
}  // namespace wowee
