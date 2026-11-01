#include "rendering/celestial.hpp"
#include "rendering/sky_system.hpp"
#include "rendering/stock_day_night.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_utils.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/logger.hpp"
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <vector>

namespace wowee {
namespace rendering {

namespace {

// The client's sky sprites, by the paths 3.3.5 ships them under.
constexpr const char* kSpritePaths[] = {
    "Textures\\SunCenter.blp",
    "Textures\\SunGlare.blp",
    "Textures\\Moon.blp",
    "Textures\\MoonGlare.blp",
};

// Angular half sizes, in radians. The sun's core is about seven degrees
// across with its glare reaching some twenty degrees out; the moon is a
// little smaller than the sun's core.
constexpr float kSunCenterHalfSize = 0.060f;
constexpr float kSunGlareHalfSize  = 0.340f;
constexpr float kMoonHalfSize      = 0.050f;
constexpr float kMoonGlareHalfSize = 0.110f;

/// Premultiplied blending, so one pipeline draws both the additive sprites
/// (alpha written as 0) and the alpha-blended moon.
VkPipelineColorBlendAttachmentState blendPremultiplied() {
    VkPipelineColorBlendAttachmentState state{};
    state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    state.blendEnable = VK_TRUE;
    state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    state.colorBlendOp = VK_BLEND_OP_ADD;
    state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    state.alphaBlendOp = VK_BLEND_OP_ADD;
    return state;
}

} // namespace

Celestial::Celestial() = default;

Celestial::~Celestial() {
    shutdown();
}

VkPipeline Celestial::buildPipeline(VkDevice device,
                                    const VkPipelineShaderStageCreateInfo& vertStage,
                                    const VkPipelineShaderStageCreateInfo& fragStage) {
    // No vertex buffer: the quad's corners come from gl_VertexIndex.
    return PipelineBuilder()
        .setShaders(vertStage, fragStage)
        .setVertexInput({}, {})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        // On the far plane and tested, never written: the ground is drawn before
        // the sky, and this is what keeps the sun behind a mountain.
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(blendPremultiplied())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());
}

bool Celestial::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    LOG_INFO("Initializing celestial renderer (Vulkan)");

    vkCtx_ = ctx;
    VkDevice device = vkCtx_->getDevice();

    if (!createSamplerSets(device, SPRITE_COUNT, textureSetLayout_, descriptorPool_,
                           textureSets_.data())) {
        LOG_ERROR("Failed to create celestial descriptors");
        return false;
    }

    auto shaders = loadShaderPair(device, "assets/shaders/celestial.vert.spv",
                                  "assets/shaders/celestial.frag.spv", "celestial");
    if (!shaders) return false;

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(CelestialPush);

    pipelineLayout_ = createPipelineLayout(device, {perFrameLayout, textureSetLayout_}, {pushRange});
    if (pipelineLayout_ == VK_NULL_HANDLE) {
        LOG_ERROR("Failed to create celestial pipeline layout");
        return false;
    }

    pipeline_ = buildPipeline(device, shaders.vertStage, shaders.fragStage);
    if (pipeline_ == VK_NULL_HANDLE) {
        LOG_ERROR("Failed to create celestial pipeline");
        return false;
    }

    LOG_INFO("Celestial renderer initialized");
    return true;
}

void Celestial::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();

    destroy(device, pipeline_);

    auto shaders = loadShaderPair(device, "assets/shaders/celestial.vert.spv",
                                  "assets/shaders/celestial.frag.spv", "celestial");
    if (!shaders) return;

    pipeline_ = buildPipeline(device, shaders.vertStage, shaders.fragStage);
    if (pipeline_ == VK_NULL_HANDLE) {
        LOG_ERROR("Celestial::recreatePipelines: failed to create pipeline");
    }
}

void Celestial::shutdown() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator allocator = vkCtx_->getAllocator();
    for (auto& texture : textures_) texture.destroy(device, allocator);
    textureReady_.fill(false);
    destroyPipeline(device, pipeline_, pipelineLayout_);
    destroy(device, descriptorPool_);
    destroy(device, textureSetLayout_);
    textureSets_.fill(VK_NULL_HANDLE);
    texturesAttempted_ = false;
    vkCtx_ = nullptr;
}

void Celestial::loadTextures(pipeline::AssetManager* assetManager) {
    if (texturesAttempted_ || !assetManager || !vkCtx_ || !descriptorPool_) return;
    texturesAttempted_ = true;

    VkDevice device = vkCtx_->getDevice();
    for (uint32_t i = 0; i < SPRITE_COUNT; ++i) {
        pipeline::BLPImage image = assetManager->loadTexture(kSpritePaths[i]);
        if (!image.isValid() ||
            !textures_[i].upload(*vkCtx_, image.data.data(), image.width, image.height,
                                 VK_FORMAT_R8G8B8A8_UNORM, true)) {
            LOG_WARNING("Celestial: failed to load ", kSpritePaths[i]);
            continue;
        }
        textures_[i].createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                   VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        if (!textures_[i].isValid()) {
            LOG_WARNING("Celestial: ", kSpritePaths[i], " is not sampleable");
            continue;
        }
        VkDescriptorImageInfo imageInfo = textures_[i].descriptorInfo();
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = textureSets_[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        textureReady_[i] = true;
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

glm::vec3 Celestial::moonDirection(float timeOfDayHours) {
    // The White Lady's own table in the stock client, 120 half-minutes an hour.
    return stockMoonDirection(timeOfDayHours * 120.0f);
}

void Celestial::drawSprite(VkCommandBuffer cmd, Sprite sprite, const glm::vec3& dir,
                           float halfSize, const glm::vec4& color, bool alphaBlended) {
    if (!textureReady_[sprite] || color.a <= 0.002f) return;
    CelestialPush push{};
    push.dirAndSize = glm::vec4(glm::normalize(dir), halfSize);
    push.color = color;
    push.mode = glm::vec4(alphaBlended ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                            1, 1, &textureSets_[sprite], 0, nullptr);
    vkCmdPushConstants(cmd, pipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

void Celestial::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const SkyParams& params) {
    if (!renderingEnabled_ || pipeline_ == VK_NULL_HANDLE) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                            0, 1, &perFrameSet, 0, nullptr);

    // Through cloud only as much as LightFloatBand 2 lets it: 1 in almost
    // every set, 0 in the death set and the storm sets' heaviest hours.
    const float throughCloud =
        glm::mix(1.0f, glm::clamp(params.celestialGlowThrough, 0.0f, 1.0f),
                 glm::clamp(params.cloudDensity, 0.0f, 1.0f));

    // --- Moon: behind the sun's glare, so drawn first ---
    const float night = glm::clamp(params.nightIntensity, 0.0f, 1.0f) * throughCloud;
    if (night > 0.002f) {
        const glm::vec3 moonDir = moonDirection(params.timeOfDay);
        if (moonDir.z > -0.1f) {
            drawSprite(cmd, MOON_GLARE, moonDir, kMoonGlareHalfSize,
                       glm::vec4(1.0f, 1.0f, 1.0f, night), false);
            drawSprite(cmd, MOON, moonDir, kMoonHalfSize,
                       glm::vec4(1.0f, 1.0f, 1.0f, night), true);
        }
    }

    // --- Sun ---
    const float day = glm::clamp(params.dayIntensity, 0.0f, 1.0f) * throughCloud;
    const glm::vec3 sunDir = glm::normalize(params.sunDir);
    if (day > 0.002f && sunDir.z > -0.1f) {
        const glm::vec3 sunColor = glm::max(params.sunColor, glm::vec3(0.0f));
        drawSprite(cmd, SUN_GLARE, sunDir, kSunGlareHalfSize, glm::vec4(sunColor, day), false);
        drawSprite(cmd, SUN_CENTER, sunDir, kSunCenterHalfSize, glm::vec4(sunColor, day), false);
    }
}

// ---------------------------------------------------------------------------
// Moon phase bookkeeping
// ---------------------------------------------------------------------------

void Celestial::update(float deltaTime) {
    if (!moonPhaseCycling_) return;
    moonPhaseTimer_ += deltaTime;
    whiteLadyPhase_ = std::fmod(moonPhaseTimer_ / MOON_CYCLE_DURATION, 1.0f);
    blueChildPhase_ = std::fmod(moonPhaseTimer_ / 210.0f, 1.0f);
}

void Celestial::setBlueChildPhase(float phase) {
    blueChildPhase_ = glm::clamp(phase, 0.0f, 1.0f);
}

} // namespace rendering
} // namespace wowee
