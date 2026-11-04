#include "rendering/weather.hpp"
#include "rendering/weather_wind.hpp"
#include "rendering/camera.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/stock_screen_fx.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <random>
#include <cmath>
#include <cstring>

namespace wowee {
namespace rendering {

namespace {

// Seeded RNG for weather particle positions and cycle durations.
// Replaces bare rand() which defaults to seed 1 without srand(),
// producing identical weather patterns on every launch.
std::mt19937& weatherRng() {
    static std::mt19937 gen(std::random_device{}());
    return gen;
}
float weatherRandFloat() {
    return std::uniform_real_distribution<float>(0.0f, 1.0f)(weatherRng());
}
float weatherRandRange(float lo, float hi) {
    return lo + (hi - lo) * weatherRandFloat();
}

// The client's weather sprites (textures\weather in the 3.3.5 data).
constexpr const char* kSpritePaths[] = {
    "Textures\\Weather\\RainDrop01.blp",        // 16x128 streak
    "Textures\\Weather\\RainDropSplash01.blp",  // 256x256, 4x4 sheet
    "Textures\\Weather\\SnowFlake01.blp",       // 64x64 flake
};

// The column of air the weather fills around the camera, in yards.
struct Volume {
    float radius;   // half the box's width
    float above;    // how far above the camera drops start
    float below;    // how far below the camera a drop that found no ground ends
};
constexpr Volume kRainVolume{32.0f, 36.0f, 40.0f};
constexpr Volume kSnowVolume{26.0f, 26.0f, 30.0f};

// Rain: a streak about a yard and a half long, as wide as the texture's 1:8
// aspect makes it, falling at some twenty-six yards a second.
constexpr float kRainHalfLength = 0.70f;
constexpr float kRainHalfWidth  = 0.0875f;
constexpr float kRainFallSpeed  = 26.0f;
constexpr float kStormFallSpeed = 32.0f;
// Snow: flakes about a quarter yard across, drifting down at a walk.
constexpr float kSnowHalfSize   = 0.11f;
constexpr float kSnowFallSpeed  = 2.4f;
// Splashes: only where a drop lands this close. The rest is the stock rain's
// (MapWeather.cpp: splash spawn 0x785640, draw 0x7891b0 / shader 0x78a030):
// a camera-facing triangle 1/12 yd either side of the impact and 1/6 yd tall
// (0xa37f38, 0xa33b7c), living a quarter second (0x9e8ce4) through the
// sheet's four columns at age * 4 * 3.99 (0x9e8d2c, 0xa3ebd0), its row picked
// by how steeply the eye looks down on it, and no new ones while the camera's
// averaged speed squared is over 2 (0xa4040c).
constexpr float kSplashRadius     = 22.0f;
constexpr float kSplashLife       = 0.25f;
constexpr float kSplashHalfWidth  = 1.0f / 12.0f;
constexpr float kSplashHeight     = 1.0f / 6.0f;
constexpr float kSplashApexV      = 0.04296875f;   // 0xa3ebcc: apex v in the cell's 0.25
constexpr float kSplashMaxSpeedSq = 2.0f;

const Volume& volumeFor(Weather::Type type) {
    return type == Weather::Type::SNOW ? kSnowVolume : kRainVolume;
}

} // namespace

Weather::Weather() {
}

Weather::~Weather() {
    shutdown();
}

/// Builds the one pipeline this effect draws with, vertex layout and all.
void Weather::buildPipelines(VkDevice device,
                             const VkPipelineShaderStageCreateInfo& vertStage,
                             const VkPipelineShaderStageCreateInfo& fragStage) {
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attrs = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
         .offset = static_cast<uint32_t>(offsetof(Vertex, pos))},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
         .offset = static_cast<uint32_t>(offsetof(Vertex, uv))},
        {.location = 2, .binding = 0, .format = VK_FORMAT_R32_SFLOAT,
         .offset = static_cast<uint32_t>(offsetof(Vertex, alpha))},
    };

    const VkRenderPass pass = targetPass_ ? targetPass_ : vkCtx->getImGuiRenderPass();
    const VkSampleCountFlagBits samples = targetPass_ ? targetSamples_ : vkCtx->getMsaaSamples();

    pipeline = PipelineBuilder()
        .setShaders(vertStage, fragStage)
        .setVertexInput({binding}, attrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS)  // tested, not written
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(samples)
        .setLayout(pipelineLayout)
        .setRenderPass(pass)
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx->getPipelineCache());

    // Rain and splashes multiply what is behind them by twice the texture.
    VkPipelineColorBlendAttachmentState mod2x{};
    mod2x.blendEnable = VK_TRUE;
    mod2x.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    mod2x.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
    mod2x.colorBlendOp = VK_BLEND_OP_ADD;
    mod2x.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    mod2x.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    mod2x.alphaBlendOp = VK_BLEND_OP_ADD;
    mod2x.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    mod2xPipeline_ = PipelineBuilder()
        .setShaders(vertStage, fragStage)
        .setVertexInput({binding}, attrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS)
        .setColorBlendAttachment(mod2x)
        .setMultisample(samples)
        .setLayout(pipelineLayout)
        .setRenderPass(pass)
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx->getPipelineCache());
}

bool Weather::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    LOG_INFO("Initializing weather system");

    vkCtx = ctx;
    VkDevice device = vkCtx->getDevice();

    if (!createSamplerSets(device, SPRITE_COUNT, textureSetLayout_, descriptorPool_,
                           textureSets_.data())) {
        LOG_ERROR("Failed to create weather descriptors");
        return false;
    }

    auto shaders = loadShaderPair(device, "assets/shaders/weather.vert.spv",
                                  "assets/shaders/weather.frag.spv", "weather");
    if (!shaders) return false;

    // Push constant: vec4 tint
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(glm::vec4);

    pipelineLayout = createPipelineLayout(device, {perFrameLayout, textureSetLayout_}, {pushRange});
    if (pipelineLayout == VK_NULL_HANDLE) {
        LOG_ERROR("Failed to create weather pipeline layout");
        return false;
    }

    buildPipelines(device, shaders.vertStage, shaders.fragStage);
    if (pipeline == VK_NULL_HANDLE) {
        LOG_ERROR("Failed to create weather pipeline");
        return false;
    }

    // Six vertices a quad: every particle, and every splash.
    frameBufferSize_ = static_cast<VkDeviceSize>(MAX_PARTICLES + MAX_SPLASHES) * 6 * sizeof(Vertex);
    frameBuffers_.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& fb : frameBuffers_) {
        AllocatedBuffer buf = createBuffer(vkCtx->getAllocator(), frameBufferSize_,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        fb.buffer = buf.buffer;
        fb.alloc = buf.allocation;
        fb.info = buf.info;
        if (fb.buffer == VK_NULL_HANDLE || !fb.info.pMappedData) {
            LOG_ERROR("Failed to create weather vertex buffer");
            return false;
        }
    }

    particles.reserve(MAX_PARTICLES);
    splashes_.reserve(MAX_SPLASHES);
    vertices_.reserve(static_cast<size_t>(MAX_PARTICLES + MAX_SPLASHES) * 6);

    LOG_INFO("Weather system initialized");
    return true;
}

void Weather::recreatePipelines() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();

    destroy(device, pipeline);
    destroy(device, mod2xPipeline_);

    auto shaders = loadShaderPair(device, "assets/shaders/weather.vert.spv",
                                  "assets/shaders/weather.frag.spv", "weather");
    if (!shaders) return;

    buildPipelines(device, shaders.vertStage, shaders.fragStage);
    if (pipeline == VK_NULL_HANDLE) {
        LOG_ERROR("Weather::recreatePipelines: failed to create pipeline");
    }
}

void Weather::loadTextures(pipeline::AssetManager* assetManager) {
    if (texturesAttempted_ || !assetManager || !vkCtx || !descriptorPool_) return;
    texturesAttempted_ = true;

    VkDevice device = vkCtx->getDevice();
    for (uint32_t i = 0; i < SPRITE_COUNT; ++i) {
        pipeline::BLPImage image = assetManager->loadTexture(kSpritePaths[i]);
        if (!image.isValid() ||
            !textures_[i].upload(*vkCtx, image.data.data(), image.width, image.height,
                                 VK_FORMAT_R8G8B8A8_UNORM, true)) {
            LOG_WARNING("Weather: failed to load ", kSpritePaths[i]);
            continue;
        }
        textures_[i].createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                   VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        if (!textures_[i].isValid()) {
            LOG_WARNING("Weather: ", kSpritePaths[i], " is not sampleable");
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
// Simulation
// ---------------------------------------------------------------------------

float Weather::resolveGround(const glm::vec3& at, const glm::vec3& cameraPos) const {
    const Volume& vol = volumeFor(particlesType_);
    const float fallback = cameraPos.z - vol.below;
    if (!groundQuery_) return fallback;
    const glm::vec2 d(at.x - cameraPos.x, at.y - cameraPos.y);
    if (glm::dot(d, d) > kSplashRadius * kSplashRadius) return fallback;
    if (auto ground = groundQuery_(at.x, at.y, cameraPos.z + vol.above)) return *ground;
    return fallback;
}

void Weather::spawnParticle(Particle& p, const glm::vec3& cameraPos, bool anywhereInColumn) {
    const Volume& vol = volumeFor(particlesType_);
    p.position.x = cameraPos.x + weatherRandRange(-vol.radius, vol.radius);
    p.position.y = cameraPos.y + weatherRandRange(-vol.radius, vol.radius);
    p.position.z = anywhereInColumn
        ? cameraPos.z + weatherRandRange(-vol.below * 0.5f, vol.above)
        : cameraPos.z + vol.above + weatherRandRange(0.0f, 4.0f);
    p.phase = weatherRandRange(0.0f, 6.2831853f);

    switch (particlesType_) {
        case Type::SNOW:
            p.velocity = glm::vec3(0.6f, 0.3f, -kSnowFallSpeed * weatherRandRange(0.8f, 1.2f));
            p.size = weatherRandRange(0.7f, 1.3f);
            break;
        case Type::STORM:
            p.velocity = glm::vec3(7.0f, 3.5f, -kStormFallSpeed * weatherRandRange(0.9f, 1.1f));
            p.size = weatherRandRange(0.85f, 1.2f);
            break;
        default:
            p.velocity = glm::vec3(1.5f, 0.8f, -kRainFallSpeed * weatherRandRange(0.9f, 1.1f));
            p.size = weatherRandRange(0.8f, 1.15f);
            break;
    }

    // Where it will come down: below the spot it reaches the camera's height,
    // carried that far by the wind.
    const float fallTime = std::max(0.0f, (p.position.z - cameraPos.z) / -p.velocity.z);
    glm::vec3 landing = p.position + glm::vec3(p.velocity.x, p.velocity.y, 0.0f) * fallTime;
    if (windStrength_ > 0.0f) {
        // E19: and by the foliage's wind, as it blows where the drop starts.
        const glm::vec2 w = weather_wind::push(windStrength_, glm::vec2(p.position), windClock_,
                                               particlesType_ == Type::SNOW, p.phase);
        landing += glm::vec3(w, 0.0f) * fallTime;
    }
    const float fallback = cameraPos.z - vol.below;
    p.groundZ = resolveGround(landing, cameraPos);
    p.lands = p.groundZ != fallback && particlesType_ != Type::SNOW;
    // Started under the ground (the column reaches below the camera): move it up.
    if (p.position.z < p.groundZ) p.position.z = cameraPos.z + vol.above;
}

void Weather::update(const Camera& camera, float deltaTime) {
    loadTextures(core::Application::getInstance().getAssetManager());

    vertices_.clear();
    particleVertexCount_ = 0;
    splashVertexCount_ = 0;
    time_ += deltaTime;

    const glm::vec3 cameraPos = camera.getPosition();
    if (!enabled || weatherType == Type::NONE) {
        particles.clear();
        splashes_.clear();
        particlesType_ = Type::NONE;
        return;
    }

    // A different kind of weather starts over, filling the column at once.
    if (particlesType_ != weatherType) {
        particles.clear();
        splashes_.clear();
        particlesType_ = weatherType;
    }
    const Volume& vol = volumeFor(particlesType_);

    // How many: the server's grade as the stock emitters take it - nothing
    // below a quarter, all of it at one (stock_screen_fx::particleLevel,
    // 0x784850) - times the Weather Detail setting.
    const float typeScale = particlesType_ == Type::SNOW ? 0.7f : 1.0f;
    const float level = stock_screen_fx::particleLevel(intensity);
    const int target = static_cast<int>(MAX_PARTICLES * typeScale * level * densityScale_);
    const bool filling = particles.empty();
    while (static_cast<int>(particles.size()) < target) {
        Particle p;
        spawnParticle(p, cameraPos, filling || weatherRandFloat() < 0.5f);
        particles.push_back(p);
    }
    if (static_cast<int>(particles.size()) > target) particles.resize(std::max(target, 0));

    // A teleport or a long fall: start the column again around the new spot.
    if (haveLastCamera_ && glm::length(cameraPos - lastCameraPos_) > vol.radius * 2.0f) {
        for (auto& p : particles) spawnParticle(p, cameraPos, true);
        splashes_.clear();
    } else if (haveLastCamera_ && deltaTime > 0.0f) {
        // The camera's velocity, averaged over a fraction of a second as the
        // client averages its recent moves: splashes only when nearly still.
        const glm::vec3 v = (cameraPos - lastCameraPos_) / deltaTime;
        const float k = std::min(1.0f, deltaTime / 0.25f);
        cameraVelocity_ += (v - cameraVelocity_) * k;
    }
    lastCameraPos_ = cameraPos;
    haveLastCamera_ = true;
    const bool splashesAllowed = glm::dot(cameraVelocity_, cameraVelocity_) <= kSplashMaxSpeedSq;

    // Storm gusts.
    const float gust = particlesType_ == Type::STORM ? 1.0f + 0.35f * std::sin(time_ * 0.7f) : 1.0f;

    for (auto& p : particles) {
        glm::vec3 v = p.velocity;
        if (particlesType_ == Type::SNOW) {
            v.x += std::sin(time_ * 1.3f + p.phase) * 0.5f;
            v.y += std::cos(time_ * 1.1f + p.phase * 1.7f) * 0.5f;
        } else {
            v.x *= gust;
            v.y *= gust;
        }
        if (windStrength_ > 0.0f) {
            // E19: carried by the trees' wind and its gust band as well.
            v += glm::vec3(weather_wind::push(windStrength_, glm::vec2(p.position), windClock_,
                                              particlesType_ == Type::SNOW, p.phase), 0.0f);
        }
        p.position += v * deltaTime;

        // Keep the column around the camera: what drifts out of the box on
        // one side comes back in on the other.
        bool wrapped = false;
        for (int axis = 0; axis < 2; ++axis) {
            const float d = p.position[axis] - cameraPos[axis];
            if (d > vol.radius)  { p.position[axis] -= 2.0f * vol.radius; wrapped = true; }
            if (d < -vol.radius) { p.position[axis] += 2.0f * vol.radius; wrapped = true; }
        }
        if (wrapped) {
            const float fallback = cameraPos.z - vol.below;
            p.groundZ = resolveGround(p.position, cameraPos);
            p.lands = p.groundZ != fallback && particlesType_ != Type::SNOW;
        }

        if (p.position.z <= p.groundZ || p.position.z < cameraPos.z - vol.below) {
            if (p.lands && splashesAllowed && p.position.z <= p.groundZ &&
                static_cast<int>(splashes_.size()) < MAX_SPLASHES) {
                splashes_.push_back({.position = glm::vec3(p.position.x, p.position.y, p.groundZ),
                                     .age = 0.0f});
            }
            spawnParticle(p, cameraPos, false);
        }
    }

    for (auto& s : splashes_) s.age += deltaTime;
    std::erase_if(splashes_, [](const Splash& s) { return s.age >= kSplashLife; });

    buildVertices(camera);
}

void Weather::buildVertices(const Camera& camera) {
    const glm::vec3 cameraPos = camera.getPosition();
    const glm::vec3 camRight = camera.getRight();
    const glm::vec3 camUp = camera.getUp();
    const Volume& vol = volumeFor(particlesType_);
    const bool snow = particlesType_ == Type::SNOW;

    auto quad = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d,
                    const glm::vec2& uv0, const glm::vec2& uv1, float alpha) {
        // a b
        // d c   with uv0 at a and uv1 at c
        vertices_.push_back({a, uv0, alpha});
        vertices_.push_back({b, {uv1.x, uv0.y}, alpha});
        vertices_.push_back({c, uv1, alpha});
        vertices_.push_back({a, uv0, alpha});
        vertices_.push_back({c, uv1, alpha});
        vertices_.push_back({d, {uv0.x, uv1.y}, alpha});
    };

    // Streaks or flakes.
    const bool haveParticleSprite = textureReady_[snow ? SNOW_FLAKE : RAIN_STREAK];
    if (haveParticleSprite) {
        for (const auto& p : particles) {
            const glm::vec3 toCam = cameraPos - p.position;
            const float dist = glm::length(toCam);
            // Not in the lens, and not past the box's edge where it wraps.
            const float horiz = glm::length(glm::vec2(toCam));
            float alpha = glm::smoothstep(0.6f, 2.5f, dist) *
                          (1.0f - glm::smoothstep(vol.radius * 0.7f, vol.radius, horiz));
            if (alpha <= 0.01f) continue;

            if (snow) {
                const glm::vec3 r = camRight * (kSnowHalfSize * p.size);
                const glm::vec3 u = camUp * (kSnowHalfSize * p.size);
                quad(p.position - r + u, p.position + r + u, p.position + r - u, p.position - r - u,
                     {0.0f, 0.0f}, {1.0f, 1.0f}, alpha);
            } else {
                // Along the way it falls, turned about that line to face the eye.
                glm::vec3 fall = p.velocity;
                if (windStrength_ > 0.0f) {
                    // E19: the streak slants along the way the wind carries it.
                    fall += glm::vec3(weather_wind::push(windStrength_, glm::vec2(p.position),
                                                         windClock_, false, p.phase), 0.0f);
                }
                const glm::vec3 axis = glm::normalize(fall);
                glm::vec3 side = glm::cross(axis, toCam);
                const float sideLen = glm::length(side);
                if (sideLen < 1e-4f) continue;
                side *= (kRainHalfWidth * p.size) / sideLen;
                const glm::vec3 half = axis * (kRainHalfLength * p.size);
                const glm::vec3 top = p.position - half;
                const glm::vec3 bottom = p.position + half;
                quad(top - side, top + side, bottom + side, bottom - side,
                     {0.0f, 0.0f}, {1.0f, 1.0f}, alpha);
            }
        }
    }
    particleVertexCount_ = static_cast<uint32_t>(vertices_.size());

    // Splashes, as the client draws them: one triangle facing the camera,
    // base on the impact and apex above it. Looking down on one, it slides
    // down the screen by half its height and up off the ground by a twelfth
    // of a yard, so it stays centred on the impact. The sheet's columns are
    // the four frames; its rows are the view, from straight down (row 0) to
    // level (row 3). No fade: a splash shows whole until it ends.
    if (!snow && textureReady_[RAIN_SPLASH]) {
        const glm::vec3 right = camRight * kSplashHalfWidth;
        const glm::vec3 up = camUp * kSplashHeight;
        for (const auto& s : splashes_) {
            const glm::vec3 fromCam = s.position - cameraPos;
            const float len = glm::length(fromCam);
            if (len < 1e-3f) continue;
            const float down = std::max(0.0f, -fromCam.z / len);
            const glm::vec3 base = s.position - up * (0.5f * down) +
                                   glm::vec3(0.0f, 0.0f, down * kSplashHalfWidth + 0.02f);
            const int frame = std::min(3, static_cast<int>(s.age * 4.0f * 3.99f));
            const int row = std::min(3, static_cast<int>((1.0f - down) * 3.99f));
            const float u0 = frame * 0.25f;
            const float v0 = row * 0.25f;
            vertices_.push_back({base - right, {u0, v0 + 0.25f}, 1.0f});
            vertices_.push_back({base + up, {u0 + 0.125f, v0 + kSplashApexV}, 1.0f});
            vertices_.push_back({base + right, {u0 + 0.25f, v0 + 0.25f}, 1.0f});
        }
    }
    splashVertexCount_ = static_cast<uint32_t>(vertices_.size()) - particleVertexCount_;
    builtType_ = particlesType_;
}

void Weather::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (!enabled || weatherType == Type::NONE || vertices_.empty() ||
        pipeline == VK_NULL_HANDLE || frameBuffers_.empty()) {
        return;
    }

    const FrameBuffer& fb = frameBuffers_[vkCtx->getCurrentFrame() % frameBuffers_.size()];
    const VkDeviceSize bytes = std::min<VkDeviceSize>(vertices_.size() * sizeof(Vertex),
                                                      frameBufferSize_);
    std::memcpy(fb.info.pMappedData, vertices_.data(), static_cast<size_t>(bytes));
    const uint32_t vertexLimit = static_cast<uint32_t>(bytes / sizeof(Vertex));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                            0, 1, &perFrameSet, 0, nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &fb.buffer, &offset);

    VkPipeline bound = pipeline;
    auto draw = [&](Sprite sprite, uint32_t first, uint32_t count, const glm::vec4& tint) {
        if (count == 0 || !textureReady_[sprite] || first >= vertexLimit) return;
        count = std::min(count, vertexLimit - first);
        // tint.a < 0 asks for the stock Mod2x (see weather.frag).
        VkPipeline want = (tint.a < 0.0f && mod2xPipeline_) ? mod2xPipeline_ : pipeline;
        if (want != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
            bound = want;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                1, 1, &textureSets_[sprite], 0, nullptr);
        vkCmdPushConstants(cmd, pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(tint), &tint);
        vkCmdDraw(cmd, count, 1, first, 0);
    };

    // As the stock client draws them (MapWeather.cpp): snow alpha-blended in
    // the Weather.dbc colour (white on every row), neither lit nor fogged
    // (snowpoint.bls writes no fog); rain and splashes in Mod2x, the drop's
    // grey-128 texture times the Weather.dbc colour, fogged toward neutral
    // grey (0x80808080) rather than the fog colour. Neither is lit by the
    // scene: the rain darkens and brightens what is behind it instead.
    if (builtType_ == Type::SNOW) {
        draw(SNOW_FLAKE, 0, particleVertexCount_, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
    } else {
        draw(RAIN_STREAK, 0, particleVertexCount_, glm::vec4(1.0f, 1.0f, 1.0f, -1.0f));
        draw(RAIN_SPLASH, particleVertexCount_, splashVertexCount_,
             glm::vec4(1.0f, 1.0f, 1.0f, -1.0f));
    }
}

void Weather::easeGrade(float target, float deltaTime) {
    target = glm::clamp(target, 0.0f, 1.0f);
    if (target != gradeTo_) {
        gradeFrom_ = intensity;
        gradeTo_ = target;
        gradeElapsed_ = 0.0f;
        gradeDuration_ = stock_screen_fx::particleTransitionSeconds(gradeFrom_, gradeTo_);
    }
    gradeElapsed_ += deltaTime;
    const float t = gradeDuration_ > 0.0f ? std::min(gradeElapsed_ / gradeDuration_, 1.0f) : 1.0f;
    intensity = glm::mix(gradeFrom_, gradeTo_, t);
}

void Weather::followServerWeather(Type type, float grade, float deltaTime) {
    // Nothing falling yet: the new kind starts from a clear sky, not from
    // whatever grade the last one was left at.
    if (weatherType == Type::NONE) {
        intensity = 0.0f;
        gradeTo_ = 0.0f;
        weatherType = type;
    }
    if (type != weatherType) {
        // The old kind eases out first; only a clear sky changes it.
        easeGrade(0.0f, deltaTime);
        if (intensity <= 0.0f) weatherType = type;
        return;
    }
    easeGrade(type == Type::NONE ? 0.0f : grade, deltaTime);
}

int Weather::getParticleCount() const {
    return static_cast<int>(particles.size());
}

void Weather::shutdown() {
    if (vkCtx) {
        VkDevice device = vkCtx->getDevice();
        VmaAllocator allocator = vkCtx->getAllocator();
        for (auto& texture : textures_) texture.destroy(device, allocator);
        textureReady_.fill(false);
        for (auto& fb : frameBuffers_) destroy(allocator, fb.buffer, fb.alloc);
        frameBuffers_.clear();
        destroy(device, mod2xPipeline_);
        destroyPipeline(device, pipeline, pipelineLayout);
        destroy(device, descriptorPool_);
        destroy(device, textureSetLayout_);
        textureSets_.fill(VK_NULL_HANDLE);
        texturesAttempted_ = false;
    }

    vkCtx = nullptr;
    particles.clear();
    splashes_.clear();
    vertices_.clear();
}

// ---------------------------------------------------------------------------
// Zone-based weather configuration (single-player, no server weather)
// ---------------------------------------------------------------------------

void Weather::setZoneWeather(uint32_t zoneId, Type type, float minIntensity, float maxIntensity, float probability) {
    zoneWeatherTable_[zoneId] = {.type = type, .minIntensity = minIntensity, .maxIntensity = maxIntensity, .probability = probability};
}

void Weather::initializeZoneWeatherDefaults() {
    if (zoneWeatherInitialized_) return;
    zoneWeatherInitialized_ = true;

    // Eastern Kingdoms zones
    setZoneWeather(11,   Type::RAIN, 0.1f, 0.4f, 0.15f);  // Wetlands - moderate rain
    setZoneWeather(8,    Type::RAIN, 0.1f, 0.5f, 0.2f);   // Swamp of Sorrows
    setZoneWeather(33,   Type::RAIN, 0.2f, 0.7f, 0.25f);  // Stranglethorn Vale
    setZoneWeather(44,   Type::RAIN, 0.1f, 0.3f, 0.1f);   // Redridge Mountains - light rain
    setZoneWeather(36,   Type::RAIN, 0.1f, 0.4f, 0.15f);  // Alterac Mountains
    setZoneWeather(45,   Type::RAIN, 0.1f, 0.3f, 0.1f);   // Arathi Highlands
    setZoneWeather(267,  Type::RAIN, 0.2f, 0.5f, 0.2f);   // Hillsbrad Foothills
    setZoneWeather(28,   Type::RAIN, 0.1f, 0.3f, 0.1f);   // Western Plaguelands - occasional rain
    setZoneWeather(139,  Type::RAIN, 0.1f, 0.3f, 0.1f);   // Eastern Plaguelands

    // Snowy zones
    setZoneWeather(1,    Type::SNOW, 0.2f, 0.6f, 0.3f);   // Dun Morogh
    setZoneWeather(51,   Type::SNOW, 0.1f, 0.5f, 0.2f);   // Searing Gorge (occasional)
    setZoneWeather(41,   Type::SNOW, 0.1f, 0.4f, 0.15f);  // Deadwind Pass
    setZoneWeather(2817, Type::SNOW, 0.3f, 0.7f, 0.4f);   // Crystalsong Forest
    setZoneWeather(67,   Type::SNOW, 0.2f, 0.6f, 0.35f);  // Storm Peaks
    setZoneWeather(65,   Type::SNOW, 0.2f, 0.5f, 0.3f);   // Dragonblight
    setZoneWeather(394,  Type::SNOW, 0.1f, 0.4f, 0.2f);   // Grizzly Hills
    setZoneWeather(495,  Type::SNOW, 0.3f, 0.8f, 0.5f);   // Howling Fjord
    setZoneWeather(210,  Type::SNOW, 0.2f, 0.5f, 0.25f);  // Icecrown
    setZoneWeather(3537, Type::SNOW, 0.2f, 0.6f, 0.3f);   // Borean Tundra
    setZoneWeather(4742, Type::SNOW, 0.2f, 0.5f, 0.3f);   // Hrothgar's Landing

    // Kalimdor zones
    setZoneWeather(15,   Type::RAIN, 0.1f, 0.4f, 0.15f);  // Dustwallow Marsh
    setZoneWeather(16,   Type::RAIN, 0.1f, 0.3f, 0.1f);   // Azshara
    setZoneWeather(148,  Type::RAIN, 0.1f, 0.4f, 0.15f);  // Darkshore
    setZoneWeather(331,  Type::RAIN, 0.1f, 0.3f, 0.1f);   // Ashenvale
    setZoneWeather(405,  Type::RAIN, 0.1f, 0.3f, 0.1f);   // Desolace
    setZoneWeather(490,  Type::RAIN, 0.1f, 0.4f, 0.15f);  // Un'Goro Crater
    setZoneWeather(493,  Type::RAIN, 0.1f, 0.3f, 0.1f);   // Moonglade

    // Winterspring is snowy
    setZoneWeather(618,  Type::SNOW, 0.2f, 0.6f, 0.3f);   // Winterspring

    // Outland
    setZoneWeather(3483, Type::RAIN, 0.1f, 0.3f, 0.1f);   // Hellfire Peninsula (occasional)
    setZoneWeather(3521, Type::RAIN, 0.1f, 0.4f, 0.15f);  // Zangarmarsh
    setZoneWeather(3519, Type::RAIN, 0.1f, 0.3f, 0.1f);   // Terokkar Forest
}

void Weather::updateZoneWeather(uint32_t zoneId, float deltaTime) {
    if (!zoneWeatherInitialized_) {
        initializeZoneWeatherDefaults();
    }

    // Zone changed - reset weather cycle
    if (zoneId != currentWeatherZone_) {
        currentWeatherZone_ = zoneId;
        zoneWeatherTimer_ = 0.0f;

        auto it = zoneWeatherTable_.find(zoneId);
        if (it == zoneWeatherTable_.end()) {
            targetIntensity_ = 0.0f;
        } else {
            float roll = weatherRandFloat();
            zoneWeatherActive_ = (roll < it->second.probability);

            if (zoneWeatherActive_) {
                weatherType = it->second.type;
                float t = weatherRandFloat();
                targetIntensity_ = glm::mix(it->second.minIntensity, it->second.maxIntensity, t);
                zoneWeatherCycleDuration_ = 180.0f + weatherRandFloat() * 300.0f;
            } else {
                targetIntensity_ = 0.0f;
                zoneWeatherCycleDuration_ = 120.0f + weatherRandFloat() * 180.0f;
            }
        }
    }

    // At the stock pace, the same as the server's weather.
    easeGrade(targetIntensity_, deltaTime);

    if (intensity <= 0.01f && targetIntensity_ <= 0.01f) {
        if (weatherType != Type::NONE) {
            weatherType = Type::NONE;
            particles.clear();
        }
    }

    // Weather cycling - periodically re-roll weather
    zoneWeatherTimer_ += deltaTime;
    if (zoneWeatherTimer_ >= zoneWeatherCycleDuration_ && zoneWeatherCycleDuration_ > 0.0f) {
        zoneWeatherTimer_ = 0.0f;

        auto it = zoneWeatherTable_.find(zoneId);
        if (it != zoneWeatherTable_.end()) {
            float roll = weatherRandFloat();
            zoneWeatherActive_ = (roll < it->second.probability);

            if (zoneWeatherActive_) {
                weatherType = it->second.type;
                float t = weatherRandFloat();
                targetIntensity_ = glm::mix(it->second.minIntensity, it->second.maxIntensity, t);
            } else {
                targetIntensity_ = 0.0f;
            }

            zoneWeatherCycleDuration_ = 180.0f + weatherRandFloat() * 300.0f;
        }
    }
}

} // namespace rendering
} // namespace wowee
