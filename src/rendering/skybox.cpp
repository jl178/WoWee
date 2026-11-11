#include "rendering/skybox.hpp"
#include "rendering/sky_system.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_frame_data.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <algorithm>
#include "rendering/stock_sky.hpp"

namespace wowee {
namespace rendering {

namespace {
// Enhanced's sun glow (dnsky), fitted against Jered's Barrens frame at 17:45:
// the strength above and on the horizon, the cone's start (cosine to the
// sun), the share left once the sun is 30 degrees up, and the glow colour's
// brightness in the HDR scene.
constexpr float kSunGlowAbove = 0.8f;
constexpr float kSunGlowStart = 0.0f;
constexpr float kSunGlowHorizon = 0.8f;
constexpr float kSunGlowHigh = 0.0f;
constexpr float kSunGlowGainHdr = 1.6f;
}

// Push constant struct - must match skybox.frag.glsl layout
// The stock sky dome is LightIntBand 2-7 laid over fixed elevations - top at
// the zenith, middle at 18 degrees, band 1 at 10, band 2 at 3, smog on the
// horizon, fog from 30 degrees below it down - interpolated between them and
// nothing else: no scattering, no sun glow (that is the sun's own glare
// sprite). The same angles every model viewer that draws the 3.3.5 sky uses.
struct SkyPushConstants {
    glm::vec4 topColor;       // LightIntBand 2
    glm::vec4 middleColor;    // 3
    glm::vec4 band1Color;     // 4
    glm::vec4 band2Color;     // 5
    glm::vec4 smogColor;      // 6
    glm::vec4 fogColor;       // 7
    glm::vec4 params;         // x = dawn/dusk highlight (stock_sky::highlight),
                              // y = Enhanced's sun glow above the horizon,
                              // z = its start (cosine from the sun), w = on the horizon
    glm::vec4 sunGlowColor;   // rgb: the colour the dome is drawn toward round the sun
};
static_assert(sizeof(SkyPushConstants) == 128, "SkyPushConstants size mismatch");

Skybox::Skybox() = default;

Skybox::~Skybox() {
    shutdown();
}

bool Skybox::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    LOG_INFO("Initializing skybox");

    vkCtx = ctx;

    VkDevice device = vkCtx->getDevice();

    // Load SPIR-V shaders
    auto shaders = loadShaderPair(device, "assets/shaders/skybox.vert.spv", "assets/shaders/skybox.frag.spv", "skybox");
    if (!shaders) return false;
    const auto& vertStage = shaders.vertStage;
    const auto& fragStage = shaders.fragStage;

    // Push constant range: 8 x vec4 = 128 bytes, the guaranteed minimum
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(SkyPushConstants);

    // Create pipeline layout with perFrameLayout (set 0) + push constants
    pipelineLayout = createPipelineLayout(device, {perFrameLayout}, {pushRange});
    if (pipelineLayout == VK_NULL_HANDLE) {
        LOG_ERROR("Failed to create skybox pipeline layout");
        return false;
    }

    // Fullscreen triangle - no vertex buffer, no vertex input.
    // Dynamic viewport and scissor
    std::vector<VkDynamicState> dynamicStates = viewportAndScissorDynamic();

    pipeline = PipelineBuilder()
        .setShaders(vertStage, fragStage)
        .setVertexInput({}, {})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)  // depth test on, write off, LEQUAL for far plane
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(vkCtx->getMsaaSamples())
        .setLayout(pipelineLayout)
        .setRenderPass(vkCtx->getImGuiRenderPass())
        .setDynamicStates(dynamicStates)
        .build(device, vkCtx->getPipelineCache());

    // Shader modules can be freed after pipeline creation

    if (pipeline == VK_NULL_HANDLE) {
        LOG_ERROR("Failed to create skybox pipeline");
        return false;
    }

    LOG_INFO("Skybox initialized");
    return true;
}

void Skybox::recreatePipelines() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();

    destroy(device, pipeline);

    auto shaders = loadShaderPair(device, "assets/shaders/skybox.vert.spv", "assets/shaders/skybox.frag.spv", "skybox");
    if (!shaders) return;
    const auto& vertStage = shaders.vertStage;
    const auto& fragStage = shaders.fragStage;

    std::vector<VkDynamicState> dynamicStates = viewportAndScissorDynamic();

    pipeline = PipelineBuilder()
        .setShaders(vertStage, fragStage)
        .setVertexInput({}, {})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(vkCtx->getMsaaSamples())
        .setLayout(pipelineLayout)
        .setRenderPass(vkCtx->getImGuiRenderPass())
        .setDynamicStates(dynamicStates)
        .build(device, vkCtx->getPipelineCache());


    if (pipeline == VK_NULL_HANDLE) {
        LOG_ERROR("Skybox::recreatePipelines: failed to create pipeline");
    }
}

void Skybox::shutdown() {
    if (vkCtx) destroyPipeline(vkCtx->getDevice(), pipeline, pipelineLayout);

    vkCtx = nullptr;
}

void Skybox::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const SkyParams& params) {
    if (pipeline == VK_NULL_HANDLE || !renderingEnabled) {
        return;
    }

    // Where the sun sprite is, which is not where the light comes from.
    SkyPushConstants push{};
    push.topColor      = glm::vec4(params.skyTopColor, 1.0f);
    push.middleColor   = glm::vec4(params.skyMiddleColor, 1.0f);
    push.band1Color    = glm::vec4(params.skyBand1Color, 1.0f);
    push.band2Color    = glm::vec4(params.skyBand2Color, 1.0f);
    push.smogColor     = glm::vec4(params.skySmogColor, 1.0f);
    push.fogColor      = glm::vec4(params.skyFogColor, 1.0f);
    // 0x7f0530: the bands pulled toward the middle colour at dawn and dusk,
    // for LightParams with HighlightSky set.
    const float dayFraction = glm::fract(params.timeOfDay / 24.0f);
    push.params = glm::vec4(stock_sky::highlight(dayFraction, params.highlightSky), 0.0f, 0.0f, 0.0f);
    // the reference client's dome (dnsky.bls, pixel shader 2990767): within the cone
    // where the cosine to the sun passes a start c, the dome's colour is
    // drawn toward a glow colour by ((cos - c) / (1 - c))^3 times a strength,
    // one strength on the horizon and another from a little above it (its
    // vertex height times 50, saturated). The numbers come from the CPU and
    // were not read; they are set against Jered's Barrens frame toward a
    // 17:45 sun, up with the day. Enhanced preset only (enhancedSunGlow).
    if (params.enhancedSunGlow > 0.0f) {
        const float day = glm::clamp(params.dayIntensity, 0.0f, 1.0f) * params.enhancedSunGlow;
        push.params.y = kSunGlowAbove * day;
        push.params.z = kSunGlowStart;
        push.params.w = kSunGlowHorizon * day;
        // The horizon's own gold (LightIntBand 6, the smog) at full
        // brightness: the frame's sky by the sun is pale gold, never white.
        const glm::vec3 smog = glm::max(params.skySmogColor, glm::vec3(0.0f));
        const float top = std::max(std::max(smog.r, smog.g), std::max(smog.b, 1e-3f));
        // Brighter than 1 only in the HDR scene, where the tone map rolls it
        // off; in the 8-bit scene it stays at the smog's own brightness.
        const float gain = params.sunGain > 1.0f ? kSunGlowGainHdr : 1.0f;
        push.sunGlowColor = glm::vec4(smog / top * gain, kSunGlowHigh);
    }

    // Bind pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Bind per-frame descriptor set (set 0 - camera UBO)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
        0, 1, &perFrameSet, 0, nullptr);

    // Push constants
    vkCmdPushConstants(cmd, pipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(push), &push);

    // Draw fullscreen triangle - no vertex buffer needed
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void Skybox::update(float deltaTime) {
    if (timeProgressionEnabled) {
        timeOfDay += deltaTime * timeSpeed;

        // Wrap around 24 hours
        if (timeOfDay >= 24.0f) {
            timeOfDay -= 24.0f;
        }
    }
}

void Skybox::setTimeOfDay(float time) {
    // Wrap to [0, 24) range using fmod instead of iterative subtraction
    time = std::fmod(time, 24.0f);
    if (time < 0.0f) time += 24.0f;
    timeOfDay = time;
}

} // namespace rendering
} // namespace wowee
