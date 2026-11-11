// Renderer's part of three enhancements: contact shadows (E14), ambient
// occlusion and the room light indoors (E12). Kept out of renderer.cpp; see contact_shadows.hpp and
// indoor_probes.hpp for what each does.
#include "rendering/renderer.hpp"

#include "rendering/bounce_grid_gpu.hpp"
#include "rendering/sky_height_field.hpp"
#include "rendering/camera.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/contact_shadow_pass.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/weather.hpp"
#include "rendering/wmo_renderer.hpp"
#include "core/logger.hpp"
#include "core/env_flag.hpp"

#include <glm/gtc/matrix_inverse.hpp>

#include <chrono>
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
                                    int blend, VkDescriptorSetLayout extraLayout,
                                    VkDescriptorSetLayout extraLayout2) {
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
                                                         static_cast<ContactShadowPass::Blend>(blend), extraLayout,
                                                         extraLayout2);
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
    // "Sky shade (the reference client)" rides on the bounce light: its build of the
    // pass (SKY_FIELD) also reads the sky height field, as set 3. Off, the
    // pass is the bounce build as it was, and nothing draws the field.
    bool sky = bounce && enh_.skyShade && shadowRenderPass != VK_NULL_HANDLE;
    if (sky) {
        if (!skyField_) skyField_ = std::make_unique<SkyHeightField>();
        sky = skyField_->ensure(vkCtx, shadowRenderPass);
        skyOffFrames_ = 0;
    } else if (skyField_ && ++skyOffFrames_ > 8) {
        std::shared_ptr<SkyHeightField> prev(std::move(skyField_));
        vkCtx->deferAfterAllFrameFences([prev]() mutable { prev.reset(); });
        skyOffFrames_ = 0;
    }
    bounceFrame_ = decideDepthLightPass(enh_.ambientOcclusion || enh_.fillLight || bounce, ambientOcclusion_,
                                        // Without the occlusion, the build without its
                                        // spiral (FILL_ONLY): the same picture, a smaller
                                        // shader.
                                        sky ? (enh_.ambientOcclusion ? "assets/shaders/ambient_sky.frag.spv"
                                                                     : "assets/shaders/ambient_sky_fill.frag.spv")
                                        : bounce ? (enh_.ambientOcclusion ? "assets/shaders/ambient_bounce.frag.spv"
                                                                          : "assets/shaders/ambient_fill.frag.spv")
                                                 : "assets/shaders/ambient_occlusion.frag.spv",
                                        "Ambient occlusion",
                                        static_cast<int>(ContactShadowPass::Blend::Multiply2x),
                                        bounce ? bounceGrid_->setLayout() : VK_NULL_HANDLE,
                                        sky ? skyField_->setLayout() : VK_NULL_HANDLE) &&
                   bounce;
    skyFrame_ = bounceFrame_ && sky;
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
        glm::vec4 sky(0.0f);
        VkDescriptorSet gridSet = VK_NULL_HANDLE;
        VkDescriptorSet skySet = VK_NULL_HANDLE;
        // The sky build binds the field, which is cleared before first use
        // (readySkyField); until it is, the pass waits a frame.
        const bool skyReady = !skyFrame_ || (skyField_ && skyField_->readable());
        if (bounceFrame_ && bounceGrid_ && skyReady) {
            // The bounce light: flag 8; the grid's corner and whether it
            // holds a bake yet (until it does, the plain fill).
            bounceGrid_->recordUpload(cmd, vkCtx->getCurrentFrame());
            // WOWEE_BOUNCE_DEBUG=1 draws the grid over the picture (flag 16).
            static const bool kBounceDebug = std::getenv("WOWEE_BOUNCE_DEBUG") != nullptr;
            flags |= 8 | (kBounceDebug ? 16 : 0);
            extra = glm::vec4(bounceGrid_->origin(), bounceGrid_->ready() ? 1.0f : 0.0f, 0.0f);
            gridSet = bounceGrid_->set();
            if (skyFrame_) {
                // The sky height field: its corner, the height its depth 0
                // stands for, and whether it holds a drawing yet.
                sky = skyField_->params();
                skySet = skyField_->set();
                // WOWEE_SKY_DEBUG=1 darkens each pixel by what of the sky it
                // sees, under Enhanced's daylight (flag 32).
                static const bool kSkyDebug = std::getenv("WOWEE_SKY_DEBUG") != nullptr;
                flags |= kSkyDebug ? 32 : 0;
            }
        }
        // Without the occlusion the pass is the fill and the bounce light
        // alone, and every term of both is outdoor light, scaled by 1 -
        // the indoor factor (ambient_occlusion.frag): wholly indoors each
        // pixel comes out within the shader's own discard band of 1. The
        // pass stays made for the way out.
        const bool fillOnlyIndoors = !enh_.ambientOcclusion && !(flags & 16) && indoorFactor_ >= 0.999f;
        if (!fillOnlyIndoors && ambientOcclusion_.pass->record(cmd, target, perFrame, invViewProj, 1.0f - 0.7f * wx, flags, extra,
                                           gridSet, sky, skySet)) {
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
        // Enhanced's air (its global fog layer, docs/re/enhanced-lighting.md
        // 5b): its density per yard, 0 outside Enhanced's daylight.
        // Its r: how much of the sun's light scatters toward the sun, 0 under
        // a sky with no sun disc (LightingParams::sunDisc, Outland).
        glm::vec4 air(1.0f, 0.0f, 0.0f, 0.0f);
        if (lightingManager) {
            air.w = lightingManager->getLightingParams().airDensity;
            air.r = lightingManager->getLightingParams().sunDisc;
        }
        if (sunHaze_.pass->record(cmd, target, perFrame, invViewProj, 1.0f - 0.8f * wx, hazeFlags, air)) {
            vkCtx->gpuMark(cmd, "sun haze");
        }
    }
}

void Renderer::updateBounceGrid(float dt) {
    if (!bounceGrid_ || !enh_.bounceLight || !camera) return;
    bounceGrid_->update(camera->getPosition(), dt, m2Renderer.get(), wmoRenderer.get(), cachedAssetManager,
                        terrainManager ? terrainManager->getMapName() : std::string());
}

// The sky height field (sky_height_field.hpp), inside the shadow pass on a
// frame no part of the world map is drawn: the doodads' static caster list
// and the front of this slot's caster buffer are the world map's, and this
// draw rebuilds both for its own matrix (the world map's next part rebuilds
// them again, as it would for a moved sun), so the two must not share a
// frame. The candidates are asked with the world map's centre and radius,
// which keeps their list. Under Enhanced's daylight only: nothing else reads it.
void Renderer::renderSkyField() {
    if (!skyField_ || !skyFrame_ || !camera || currentCmd == VK_NULL_HANDLE || worldShadowCullRadius_ <= 0.0f) return;
    if (!lightingManager || !lightingManager->isEnhancedActive()) return;
    // WOWEE_SKY_FIELD=0: never drawn, so the pass leaves the sky term out
    // (for side-by-side work and for measuring its cost).
    static const bool kSkyFieldOn = core::envFlagEnabled("WOWEE_SKY_FIELD", true);
    if (!kSkyFieldOn) return;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const int part = skyField_->partDue(camera->getPosition(), now);
    if (part == 0) return;
    const glm::vec3 centre = skyField_->redrawCentre();
    const float reach = 0.75f * sky_field::kSpan;  // past the square's corners
    // Part 1: the ground alone, then the buildings and doodads from above.
    // Part 2, the next frame: the same from below. Each part asks the
    // doodads once, so this frame slot's caster buffer holds one list.
    const glm::mat4 m = sky_field::projection(centre, part == 2);
    if (part == 1) {
        SkyHeightField::Target& g = skyField_->back(SkyHeightField::kGround);
        beginShadowTarget(g.image, g.view, g.framebuffer, sky_field::kTexels, g.layout, true);
        if (terrainRenderer) terrainRenderer->renderShadow(currentCmd, m, centre, reach);
        endShadowTarget(g.image, g.layout);
    }
    SkyHeightField::Target& t = skyField_->back(part == 1 ? SkyHeightField::kTops : SkyHeightField::kFeet);
    beginShadowTarget(t.image, t.view, t.framebuffer, sky_field::kTexels, t.layout, true);
    if (wmoRenderer) wmoRenderer->renderShadow(currentCmd, m, centre, reach, ShadowCasterSet::Static);
    if (m2Renderer) {
        m2Renderer->setShadowCountQuiet(true);
        m2Renderer->renderShadow(currentCmd, m, globalTime, worldShadowCullCentre_, worldShadowCullRadius_,
                                 ShadowCasterSet::Static);
        m2Renderer->setShadowCountQuiet(false);
    }
    endShadowTarget(t.image, t.layout);
    skyField_->partDrawn(now);
    vkCtx->gpuMark(currentCmd, "sky field");
}

// Never bound unwritten: every map cleared (to nothing standing anywhere)
// the first frame it exists, whether or not the shadow pass draws it.
void Renderer::readySkyField() {
    if (!skyField_ || skyField_->readable() || currentCmd == VK_NULL_HANDLE) return;
    skyField_->forEach([this](SkyHeightField::Target& t) {
        if (t.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || t.image == VK_NULL_HANDLE) return;
        beginShadowTarget(t.image, t.view, t.framebuffer, sky_field::kTexels, t.layout, true);
        endShadowTarget(t.image, t.layout);
    });
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
