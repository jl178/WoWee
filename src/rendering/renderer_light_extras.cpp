// Renderer's part of two enhancements: contact shadows (E14) and the room
// light indoors (E12). Kept out of renderer.cpp; see contact_shadows.hpp and
// indoor_probes.hpp for what each does.
#include "rendering/renderer.hpp"

#include "rendering/camera.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/contact_shadow_pass.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/wmo_renderer.hpp"
#include "core/logger.hpp"

#include <glm/gtc/matrix_inverse.hpp>

namespace wowee {
namespace rendering {

// Contact shadows need the sun's shadows on, the overlay pass, a one-sample
// depth to read (as the soft particles do) and the player not wholly indoors,
// where the sun lights nothing. Everything is let go eight frames after they
// stop being wanted.
void Renderer::decideContactShadows() {
    contactShadowsFrame_ = false;
    contactShadowsDone_ = false;
    bool want = enh_.contactShadows && shadowsEnabled && vkCtx &&
                vkCtx->getOverlayRenderPass() != VK_NULL_HANDLE && indoorFactor_ < 0.99f;
    if (want) {
        const bool offscreen = postProcessPipeline_ &&
                               postProcessPipeline_->getSceneFramebuffer() != VK_NULL_HANDLE;
        if (offscreen) {
            want = postProcessPipeline_->getSceneColorImageView() != VK_NULL_HANDLE &&
                   postProcessPipeline_->getSceneDepthImage() != VK_NULL_HANDLE &&
                   !postProcessPipeline_->sceneDepthIsMsaa();
        } else {
            want = !vkCtx->getOverlayFramebuffers().empty() && !vkCtx->isDepthCopySourceMsaa();
        }
    }
    if (want) {
        if (!contactShadowPass_) contactShadowPass_ = std::make_unique<ContactShadowPass>();
        want = contactShadowPass_->ensure(vkCtx, perFrameSetLayout, vkCtx->getOverlayRenderPass());
    }
    contactShadowsFrame_ = want;
    if (want) {
        contactShadowsOffFrames_ = 0;
    } else if (contactShadowPass_ && ++contactShadowsOffFrames_ > 8) {
        contactShadowPass_.reset();
        contactShadowsOffFrames_ = 0;
    }
}

void Renderer::recordContactShadowPass(VkCommandBuffer cmd) {
    if (!contactShadowsFrame_ || contactShadowsDone_ || !worldDrawnThisFrame_ || !contactShadowPass_ ||
        !vkCtx || cmd == VK_NULL_HANDLE) {
        return;
    }
    contactShadowsDone_ = true;
    SoftParticlePass::Target t;
    t.pass = vkCtx->getOverlayRenderPass();
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
    const glm::mat4 invViewProj = glm::inverse(currentFrameData.projection * currentFrameData.view);
    // Light.dbc's shadow band x the player's setting, as the shadow map's
    // (shadowParams.y), and none of it indoors.
    const float darkness = currentFrameData.shadowParams.y * (1.0f - glm::clamp(indoorFactor_, 0.0f, 1.0f));
    if (contactShadowPass_->record(cmd, t, perFrameDescSets[vkCtx->getCurrentFrame()], invViewProj, darkness)) {
        vkCtx->gpuMark(cmd, "contact shadows");
    }
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
