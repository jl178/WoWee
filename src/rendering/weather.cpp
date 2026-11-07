#include "rendering/weather.hpp"
#include "rendering/weather_wind.hpp"
#include "rendering/weather_fx.hpp"
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
    // Loaded only for the mist sheets and the fireflies: the stock rain
    // and snow mist's sheet, a soft white puff (MapWeather 0x78c420).
    "Textures\\Weather\\SnowMist01.blp",        // 256x256 puff
};
// The sprites the stock weather draws, loaded with it; the rest on first use.
constexpr uint32_t kStockSpriteCount = 3;

// The column of air the weather fills around the camera, in yards.
struct Volume {
    float radius;   // half the box's width
    float above;    // how far above the camera drops start
    float below;    // how far below the camera a drop that found no ground ends
};
constexpr Volume kRainVolume{32.0f, 36.0f, 40.0f};
constexpr Volume kSnowVolume{26.0f, 26.0f, 30.0f};
// A sandstorm's grains blow level through a low box around the eye (the
// stock sand box is 20 x 12.5 yd, world-lighting.md 8).
constexpr Volume kSandVolume{20.0f, 3.0f, 8.0f};

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
    if (type == Weather::Type::SAND) return kSandVolume;
    return type == Weather::Type::SNOW ? kSnowVolume : kRainVolume;
}

// Varied rain: how far out splashes are made (the stock 22 yd otherwise).
constexpr float kVariedSplashRadius = 30.0f;
// Mist sheets: at most this many (the stock mist's cap is 128, its sheets
// living a few seconds; these live longer and are larger).
constexpr int kMaxSheets = 40;
// Fireflies: at most this many, within this radius of the camera.
constexpr int kMaxFireflies = 40;
constexpr float kFireflyRadius = 16.0f;

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

    // Fireflies glow: what is behind them plus the sprite times its alpha.
    addPipeline_ = PipelineBuilder()
        .setShaders(vertStage, fragStage)
        .setVertexInput({binding}, attrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS)
        .setColorBlendAttachment(PipelineBuilder::blendAdditive())
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
    destroy(device, addPipeline_);

    auto shaders = loadShaderPair(device, "assets/shaders/weather.vert.spv",
                                  "assets/shaders/weather.frag.spv", "weather");
    if (!shaders) return;

    buildPipelines(device, shaders.vertStage, shaders.fragStage);
    if (pipeline == VK_NULL_HANDLE) {
        LOG_ERROR("Weather::recreatePipelines: failed to create pipeline");
    }
}

void Weather::loadTextures(pipeline::AssetManager* assetManager) {
    if (!assetManager || !vkCtx || !descriptorPool_) return;
    // The stock sprites at once; the mist sheet only once a style asks for it.
    uint32_t first = 0, last = 0;
    if (!texturesAttempted_) {
        texturesAttempted_ = true;
        last = kStockSpriteCount;
    } else if (!extraTexturesAttempted_ && (style_.mistSheets || fireflyAmount_ > 0.0f)) {
        extraTexturesAttempted_ = true;
        first = kStockSpriteCount;
        last = SPRITE_COUNT;
    } else {
        return;
    }

    VkDevice device = vkCtx->getDevice();
    for (uint32_t i = first; i < last; ++i) {
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
    const float radius = style_.variedRain ? kVariedSplashRadius : kSplashRadius;
    if (glm::dot(d, d) > radius * radius) return fallback;
    if (auto ground = groundQuery_(at.x, at.y, cameraPos.z + vol.above)) return *ground;
    return fallback;
}

void Weather::spawnParticle(Particle& p, const glm::vec3& cameraPos, bool anywhereInColumn) {
    const Volume& vol = volumeFor(particlesType_);
    // A sandstorm's grains blow level and fill their low box anywhere.
    if (particlesType_ == Type::SAND) anywhereInColumn = true;
    p.position.x = cameraPos.x + weatherRandRange(-vol.radius, vol.radius);
    p.position.y = cameraPos.y + weatherRandRange(-vol.radius, vol.radius);
    p.position.z = anywhereInColumn
        ? cameraPos.z + weatherRandRange(-vol.below * 0.5f, vol.above)
        : cameraPos.z + vol.above + weatherRandRange(0.0f, 4.0f);
    p.phase = weatherRandRange(0.0f, 6.2831853f);
    // The style's own; the stock values unless a style below sets them.
    p.length = 1.0f;
    p.alpha = 1.0f;
    p.spin = 0.0f;

    switch (particlesType_) {
        case Type::SNOW:
            p.velocity = glm::vec3(0.6f, 0.3f, -kSnowFallSpeed * weatherRandRange(0.8f, 1.2f));
            p.size = weatherRandRange(0.7f, 1.3f);
            break;
        case Type::STORM:
            p.velocity = glm::vec3(7.0f, 3.5f, -kStormFallSpeed * weatherRandRange(0.9f, 1.1f));
            p.size = weatherRandRange(0.85f, 1.2f);
            break;
        case Type::SAND: {
            // Blown along the prevailing wind (weather_wind's direction), a
            // little up or down.
            const glm::vec2 dir = weather_wind::direction();
            const float speed = weatherRandRange(7.0f, 13.0f);
            p.velocity = glm::vec3(dir * speed, weatherRandRange(-0.6f, 0.6f));
            p.size = weatherRandRange(0.6f, 1.4f);
            p.alpha = weatherRandRange(0.35f, 1.0f);
            break;
        }
        default:
            p.velocity = glm::vec3(1.5f, 0.8f, -kRainFallSpeed * weatherRandRange(0.9f, 1.1f));
            p.size = weatherRandRange(0.8f, 1.15f);
            break;
    }

    // Style: drops of every size, most of them fine, a few heavy and falling
    // faster on longer streaks; flakes from a speck to a clump, the large ones
    // drifting slower and turning.
    const bool rain = particlesType_ == Type::RAIN || particlesType_ == Type::STORM;
    if (rain && style_.variedRain) {
        const float r = weatherRandFloat();
        p.size = 0.55f + 1.0f * r * r * r;
        p.velocity.z *= 0.85f + 0.2f * p.size;
        p.length = p.size * weatherRandRange(0.8f, 1.4f);
        p.alpha = 0.5f + 0.5f * std::min(1.0f, p.size);
    } else if (particlesType_ == Type::SNOW && style_.softSnow) {
        const float r = weatherRandFloat();
        p.size = 0.45f + 1.45f * std::pow(r, 2.2f);
        p.velocity.z *= 1.1f - 0.3f * r;
        p.alpha = 0.55f + 0.45f * (1.0f - r * 0.5f) * weatherRandFloat();
        p.alpha = std::max(p.alpha, 0.45f);
        p.spin = weatherRandRange(-0.9f, 0.9f);
    }

    if (particlesType_ == Type::SAND) {
        p.groundZ = cameraPos.z - vol.below;
        p.lands = false;
        return;
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

void Weather::spawnSheet(MistSheet& s, const glm::vec3& cameraPos) {
    const bool sand = particlesType_ == Type::SAND;
    const float bearing = weatherRandRange(0.0f, 6.2831853f);
    const float dist = sand ? weatherRandRange(8.0f, 45.0f) : weatherRandRange(16.0f, 70.0f);
    const glm::vec2 at = glm::vec2(cameraPos) + glm::vec2(std::cos(bearing), std::sin(bearing)) * dist;
    float ground = cameraPos.z - 6.0f;
    if (groundQuery_) {
        if (auto g = groundQuery_(at.x, at.y, cameraPos.z + 60.0f)) ground = *g;
    }
    s.base = glm::vec3(at, ground - 1.5f);
    s.age = 0.0f;
    s.life = weatherRandRange(6.0f, 11.0f);
    s.halfWidth = weatherRandRange(6.0f, 9.0f);
    s.flipU = weatherRandFloat() < 0.5f;
    // The sheet drifts as the weather does: the type's stock drift, and the
    // foliage's wind where that is on (E19), at about half the drops' speed.
    glm::vec2 drift;
    switch (particlesType_) {
        case Type::SNOW:  drift = glm::vec2(0.6f, 0.3f); break;
        case Type::STORM: drift = glm::vec2(7.0f, 3.5f); break;
        case Type::SAND:  drift = weather_wind::direction() * 8.0f; break;
        default:          drift = glm::vec2(1.5f, 0.8f); break;
    }
    if (windStrength_ > 0.0f) {
        drift += weather_wind::push(windStrength_, at, windClock_, particlesType_ == Type::SNOW, bearing);
    }
    s.velocity = glm::vec3(drift * 0.5f, 0.0f);
    switch (particlesType_) {
        case Type::SNOW:  s.height = weatherRandRange(20.0f, 30.0f); s.peak = 0.55f; break;
        case Type::STORM: s.height = weatherRandRange(24.0f, 34.0f); s.peak = 0.70f; break;
        case Type::SAND:  s.height = weatherRandRange(9.0f, 16.0f);  s.peak = 0.85f; break;
        default:          s.height = weatherRandRange(22.0f, 32.0f); s.peak = 0.50f; break;
    }
}

void Weather::updateSheets(const glm::vec3& cameraPos, float level, float deltaTime) {
    if (!style_.mistSheets || level <= 0.0f) {
        sheets_.clear();
        return;
    }
    const int target = std::min(kMaxSheets,
        weather_fx::mistSheetTarget(particlesType_ == Type::SAND, level, style_.intensity, kMaxSheets));
    for (auto& s : sheets_) {
        s.age += deltaTime;
        s.base += s.velocity * deltaTime;
    }
    std::erase_if(sheets_, [&](const MistSheet& s) {
        const glm::vec2 d = glm::vec2(s.base) - glm::vec2(cameraPos);
        return s.age >= s.life || glm::dot(d, d) > 95.0f * 95.0f;
    });
    // A few a second at most, so a change of weather gathers rather than
    // appears; the first fill is staggered in age so they do not all fade
    // together.
    const bool filling = sheets_.empty();
    int spawns = filling ? target : std::max(1, static_cast<int>(target * deltaTime / 4.0f + weatherRandFloat()));
    while (static_cast<int>(sheets_.size()) < target && spawns-- > 0) {
        MistSheet s;
        spawnSheet(s, cameraPos);
        if (filling) s.age = weatherRandRange(0.0f, s.life * 0.8f);
        sheets_.push_back(s);
    }
    if (static_cast<int>(sheets_.size()) > target) sheets_.resize(std::max(target, 0));
}

void Weather::updateFireflies(const glm::vec3& cameraPos, float deltaTime) {
    if (fireflyAmount_ <= 0.0f) {
        fireflies_.clear();
        return;
    }
    const int target = static_cast<int>(kMaxFireflies * fireflyAmount_);
    auto place = [&](Firefly& f) {
        const float a = weatherRandRange(0.0f, 6.2831853f);
        const float r = kFireflyRadius * std::sqrt(weatherRandFloat());
        const glm::vec2 at = glm::vec2(cameraPos) + glm::vec2(std::cos(a), std::sin(a)) * r;
        float ground = cameraPos.z - 1.6f;
        if (groundQuery_) {
            if (auto g = groundQuery_(at.x, at.y, cameraPos.z + 8.0f)) ground = *g;
        }
        f.home = glm::vec3(at, ground + weatherRandRange(0.4f, 2.4f));
        f.position = f.home;
        f.phase = weatherRandRange(0.0f, 6.2831853f);
        f.rate = weatherRandRange(0.6f, 1.4f);
        f.age = 0.0f;
    };
    while (static_cast<int>(fireflies_.size()) < target) {
        Firefly f;
        place(f);
        fireflies_.push_back(f);
    }
    if (static_cast<int>(fireflies_.size()) > target) fireflies_.resize(std::max(target, 0));
    for (auto& f : fireflies_) {
        f.age += deltaTime;
        // A slow, looping wander about its spot.
        const float t = time_ * 0.35f * f.rate + f.phase;
        f.position = f.home + glm::vec3(std::sin(t) * 1.1f + std::sin(t * 2.3f) * 0.3f,
                                        std::cos(t * 0.8f) * 1.1f,
                                        std::sin(t * 1.7f) * 0.35f);
        const glm::vec2 d = glm::vec2(f.home) - glm::vec2(cameraPos);
        if (glm::dot(d, d) > kFireflyRadius * kFireflyRadius * 1.3f) place(f);
    }
}

void Weather::update(const Camera& camera, float deltaTime) {
    loadTextures(core::Application::getInstance().getAssetManager());

    vertices_.clear();
    sheetVertexCount_ = 0;
    particleVertexCount_ = 0;
    splashVertexCount_ = 0;
    fireflyVertexCount_ = 0;
    time_ += deltaTime;

    const glm::vec3 cameraPos = camera.getPosition();
    updateFireflies(cameraPos, deltaTime);
    if (!enabled || weatherType == Type::NONE) {
        particles.clear();
        splashes_.clear();
        sheets_.clear();
        particlesType_ = Type::NONE;
        if (!fireflies_.empty()) buildVertices(camera);
        return;
    }

    // A different kind of weather starts over, filling the column at once.
    if (particlesType_ != weatherType) {
        particles.clear();
        splashes_.clear();
        sheets_.clear();
        particlesType_ = weatherType;
    }
    const Volume& vol = volumeFor(particlesType_);

    // How many: the server's grade as the stock emitters take it - nothing
    // below a quarter, all of it at one (stock_screen_fx::particleLevel,
    // 0x784850) - times the Weather Detail setting, times the "Weather
    // intensity" slider (1, stock, adds nothing).
    const float typeScale = particlesType_ == Type::SNOW ? 0.7f
                          : particlesType_ == Type::SAND ? 0.8f : 1.0f;
    const float level = stock_screen_fx::particleLevel(intensity);
    const int target = weather_fx::particleTarget(MAX_PARTICLES, typeScale, level, densityScale_,
                                                  style_.intensity);
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
        sheets_.clear();
    } else if (haveLastCamera_ && deltaTime > 0.0f) {
        // The camera's velocity, averaged over a fraction of a second as the
        // client averages its recent moves: splashes only when nearly still.
        const glm::vec3 v = (cameraPos - lastCameraPos_) / deltaTime;
        const float k = std::min(1.0f, deltaTime / 0.25f);
        cameraVelocity_ += (v - cameraVelocity_) * k;
    }
    lastCameraPos_ = cameraPos;
    haveLastCamera_ = true;
    // Varied rain splashes whether or not the eye is moving.
    const bool splashesAllowed = style_.variedRain ||
        glm::dot(cameraVelocity_, cameraVelocity_) <= kSplashMaxSpeedSq;
    const int maxSplashes = static_cast<int>(MAX_SPLASHES * weather_fx::countScale(style_.intensity));

    // Storm gusts.
    const float gust = particlesType_ == Type::STORM ? 1.0f + 0.35f * std::sin(time_ * 0.7f) : 1.0f;
    const bool softSnow = particlesType_ == Type::SNOW && style_.softSnow;
    const bool sand = particlesType_ == Type::SAND;

    for (auto& p : particles) {
        glm::vec3 v = p.velocity;
        if (particlesType_ == Type::SNOW) {
            v.x += std::sin(time_ * 1.3f + p.phase) * 0.5f;
            v.y += std::cos(time_ * 1.1f + p.phase * 1.7f) * 0.5f;
            if (softSnow) {
                // Slow eddies the flakes ride through together: a field over
                // the box, turning with time, and a lift and drop on it.
                v.x += std::sin(p.position.y * 0.21f + time_ * 0.5f) * 0.7f;
                v.y += std::cos(p.position.x * 0.17f - time_ * 0.43f) * 0.7f;
                v.z += std::sin(time_ * 0.9f + p.phase * 3.0f) * 0.3f;
            }
        } else if (sand) {
            // Grains skip and swirl in the gusts.
            const float g = 0.75f + 0.45f * std::sin(time_ * 1.1f + p.position.x * 0.05f);
            v.x *= g;
            v.y *= g;
            v.z += std::sin(time_ * 2.0f + p.phase) * 0.8f;
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
        if (sand) {
            // Level grains: the box's top and bottom wrap too.
            const float dz = p.position.z - cameraPos.z;
            if (dz > vol.above) p.position.z -= vol.above + vol.below;
            if (dz < -vol.below) p.position.z += vol.above + vol.below;
            continue;
        }
        if (wrapped) {
            const float fallback = cameraPos.z - vol.below;
            p.groundZ = resolveGround(p.position, cameraPos);
            p.lands = p.groundZ != fallback && particlesType_ != Type::SNOW;
        }

        if (p.position.z <= p.groundZ || p.position.z < cameraPos.z - vol.below) {
            if (p.lands && splashesAllowed && p.position.z <= p.groundZ &&
                static_cast<int>(splashes_.size()) < maxSplashes) {
                splashes_.push_back({.position = glm::vec3(p.position.x, p.position.y, p.groundZ),
                                     .age = 0.0f,
                                     .size = style_.variedRain ? std::clamp(p.size, 0.7f, 1.4f) : 1.0f});
            }
            spawnParticle(p, cameraPos, false);
        }
    }

    for (auto& s : splashes_) s.age += deltaTime;
    std::erase_if(splashes_, [](const Splash& s) { return s.age >= kSplashLife; });

    updateSheets(cameraPos, level, deltaTime);

    buildVertices(camera);
}

void Weather::buildVertices(const Camera& camera) {
    const glm::vec3 cameraPos = camera.getPosition();
    const glm::vec3 camRight = camera.getRight();
    const glm::vec3 camUp = camera.getUp();
    const Volume& vol = volumeFor(particlesType_);
    const bool snow = particlesType_ == Type::SNOW;
    const bool sand = particlesType_ == Type::SAND;

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

    // Distant sheets first: they are behind the drops.
    if (!sheets_.empty() && textureReady_[MIST_SHEET]) {
        const float side = weather_fx::sideScale(style_.intensity);
        for (const auto& s : sheets_) {
            const glm::vec2 toCam2 = glm::vec2(cameraPos) - glm::vec2(s.base);
            const float dist = glm::length(toCam2);
            if (dist < 1e-3f) continue;
            const float life = std::sin(3.14159265f * std::clamp(s.age / s.life, 0.0f, 1.0f));
            const float alpha = std::min(1.0f, s.peak * side) * life * weather_fx::mistDistanceFade(dist);
            if (alpha <= 0.01f) continue;
            // Turned about the vertical to face the eye.
            const glm::vec2 n = toCam2 / dist;
            const glm::vec3 r = glm::vec3(-n.y, n.x, 0.0f) * s.halfWidth;
            const glm::vec3 up(0.0f, 0.0f, s.height);
            const float u0 = s.flipU ? 1.0f : 0.0f;
            quad(s.base - r + up, s.base + r + up, s.base + r, s.base - r,
                 {u0, 0.0f}, {1.0f - u0, 1.0f}, alpha);
        }
    }
    sheetVertexCount_ = static_cast<uint32_t>(vertices_.size());

    // Streaks or flakes.
    const bool haveParticleSprite = textureReady_[snow ? SNOW_FLAKE : RAIN_STREAK];
    if (haveParticleSprite) {
        for (const auto& p : particles) {
            const glm::vec3 toCam = cameraPos - p.position;
            const float dist = glm::length(toCam);
            // Not in the lens, and not past the box's edge where it wraps.
            const float horiz = glm::length(glm::vec2(toCam));
            // Sand grains this close would streak across the lens.
            float alpha = (sand ? glm::smoothstep(2.0f, 6.0f, dist) : glm::smoothstep(0.6f, 2.5f, dist)) *
                          (1.0f - glm::smoothstep(vol.radius * 0.7f, vol.radius, horiz));
            if (alpha <= 0.01f) continue;
            alpha *= p.alpha;  // 1 unless a style varied it

            if (snow) {
                glm::vec3 r = camRight * (kSnowHalfSize * p.size);
                glm::vec3 u = camUp * (kSnowHalfSize * p.size);
                if (p.spin != 0.0f) {
                    // Soft snow: each flake turns on its own.
                    const float a = p.phase + time_ * p.spin;
                    const float c = std::cos(a), sn = std::sin(a);
                    const glm::vec3 r2 = r * c + u * sn;
                    u = u * c - r * sn;
                    r = r2;
                }
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
                if (sand) {
                    // Fine grains in short dashes.
                    side *= (0.03f * p.size) / sideLen;
                    const glm::vec3 half = axis * (0.35f * p.size);
                    quad(p.position - half - side, p.position - half + side,
                         p.position + half + side, p.position + half - side,
                         {0.0f, 0.0f}, {1.0f, 1.0f}, alpha);
                    continue;
                }
                const float widthScale = p.length != 1.0f ? 0.8f + 0.4f * std::min(p.size, 1.3f) : 1.0f;
                side *= (kRainHalfWidth * p.size * widthScale) / sideLen;
                const glm::vec3 half = axis * (kRainHalfLength * p.size * p.length);
                const glm::vec3 top = p.position - half;
                const glm::vec3 bottom = p.position + half;
                quad(top - side, top + side, bottom + side, bottom - side,
                     {0.0f, 0.0f}, {1.0f, 1.0f}, alpha);
            }
        }
    }
    particleVertexCount_ = static_cast<uint32_t>(vertices_.size()) - sheetVertexCount_;

    // Splashes, as the client draws them: one triangle facing the camera,
    // base on the impact and apex above it. Looking down on one, it slides
    // down the screen by half its height and up off the ground by a twelfth
    // of a yard, so it stays centred on the impact. The sheet's columns are
    // the four frames; its rows are the view, from straight down (row 0) to
    // level (row 3). No fade: a splash shows whole until it ends.
    if (!snow && !sand && textureReady_[RAIN_SPLASH]) {
        for (const auto& s : splashes_) {
            const glm::vec3 right = camRight * (kSplashHalfWidth * s.size);
            const glm::vec3 up = camUp * (kSplashHeight * s.size);
            const glm::vec3 fromCam = s.position - cameraPos;
            const float len = glm::length(fromCam);
            if (len < 1e-3f) continue;
            const float down = std::max(0.0f, -fromCam.z / len);
            const glm::vec3 base = s.position - up * (0.5f * down) +
                                   glm::vec3(0.0f, 0.0f, down * kSplashHalfWidth * s.size + 0.02f);
            const int frame = std::min(3, static_cast<int>(s.age * 4.0f * 3.99f));
            const int row = std::min(3, static_cast<int>((1.0f - down) * 3.99f));
            const float u0 = frame * 0.25f;
            const float v0 = row * 0.25f;
            vertices_.push_back({base - right, {u0, v0 + 0.25f}, 1.0f});
            vertices_.push_back({base + up, {u0 + 0.125f, v0 + kSplashApexV}, 1.0f});
            vertices_.push_back({base + right, {u0 + 0.25f, v0 + 0.25f}, 1.0f});
        }
    }
    splashVertexCount_ = static_cast<uint32_t>(vertices_.size()) - sheetVertexCount_ - particleVertexCount_;

    // Fireflies: small glows that pulse slowly, fading out toward the edge
    // of their ring.
    if (!fireflies_.empty() && textureReady_[MIST_SHEET]) {
        for (const auto& f : fireflies_) {
            const glm::vec3 toCam = cameraPos - f.position;
            const float dist = glm::length(toCam);
            const float blink = std::pow(std::max(0.0f, std::sin(time_ * 0.9f * f.rate + f.phase)), 3.0f);
            const float alpha = blink * std::min(1.0f, f.age / 1.5f) * glm::smoothstep(0.8f, 2.0f, dist) *
                                (1.0f - glm::smoothstep(kFireflyRadius * 0.75f, kFireflyRadius, dist));
            if (alpha <= 0.02f) continue;
            // The soft round mist puff, small: a point of light with a glow.
            const glm::vec3 r = camRight * 0.07f;
            const glm::vec3 u = camUp * 0.07f;
            quad(f.position - r + u, f.position + r + u, f.position + r - u, f.position - r - u,
                 {0.0f, 0.0f}, {1.0f, 1.0f}, alpha);
        }
    }
    fireflyVertexCount_ = static_cast<uint32_t>(vertices_.size()) - sheetVertexCount_ -
                          particleVertexCount_ - splashVertexCount_;
    builtType_ = particlesType_;
}

bool Weather::ensureVertexCapacity(size_t vertexCount) {
    const VkDeviceSize need = static_cast<VkDeviceSize>(vertexCount) * sizeof(Vertex);
    if (need <= frameBufferSize_) return true;
    if (!vkCtx) return false;
    // Retire the old buffers once every frame in flight is done with them.
    std::vector<FrameBuffer> old = std::move(frameBuffers_);
    frameBuffers_.clear();
    VmaAllocator allocator = vkCtx->getAllocator();
    vkCtx->deferAfterAllFrameFences([allocator, old]() mutable {
        for (auto& fb : old) destroy(allocator, fb.buffer, fb.alloc);
    });
    frameBufferSize_ = need + need / 4;
    frameBuffers_.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& fb : frameBuffers_) {
        AllocatedBuffer buf = createBuffer(allocator, frameBufferSize_,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        fb.buffer = buf.buffer;
        fb.alloc = buf.allocation;
        fb.info = buf.info;
        if (fb.buffer == VK_NULL_HANDLE || !fb.info.pMappedData) {
            LOG_ERROR("Weather: failed to grow the vertex buffer");
            for (auto& b : frameBuffers_) destroy(allocator, b.buffer, b.alloc);
            frameBuffers_.clear();
            frameBufferSize_ = 0;
            return false;
        }
    }
    return true;
}

void Weather::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    // Nothing built this frame (no weather, indoors, no fireflies) draws nothing.
    if (vertices_.empty() || pipeline == VK_NULL_HANDLE) return;
    if (!ensureVertexCapacity(vertices_.size()) || frameBuffers_.empty()) return;

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
    auto draw = [&](Sprite sprite, uint32_t first, uint32_t count, const glm::vec4& tint,
                    bool additive = false) {
        if (count == 0 || !textureReady_[sprite] || first >= vertexLimit) return;
        count = std::min(count, vertexLimit - first);
        // tint.a < 0 asks for the stock Mod2x (see weather.frag).
        VkPipeline want = (tint.a < 0.0f && mod2xPipeline_) ? mod2xPipeline_ : pipeline;
        if (additive) want = addPipeline_;
        if (want == VK_NULL_HANDLE) return;
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

    // The mist sheets in the fog colour, alpha-blended, as the stock mist.
    const uint32_t particleFirst = sheetVertexCount_;
    const uint32_t splashFirst = particleFirst + particleVertexCount_;
    const uint32_t fireflyFirst = splashFirst + splashVertexCount_;
    draw(MIST_SHEET, 0, sheetVertexCount_, glm::vec4(fogColour_, 1.0f));

    // As the stock client draws them (MapWeather.cpp): snow alpha-blended in
    // the Weather.dbc colour (white on every row), neither lit nor fogged
    // (snowpoint.bls writes no fog); rain and splashes in Mod2x, the drop's
    // grey-128 texture times the Weather.dbc colour, fogged toward neutral
    // grey (0x80808080) rather than the fog colour. Neither is lit by the
    // scene: the rain darkens and brightens what is behind it instead.
    if (builtType_ == Type::SNOW) {
        draw(SNOW_FLAKE, particleFirst, particleVertexCount_, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
    } else if (builtType_ == Type::SAND) {
        // Sand in the fog colour, as the stock sand.bls (the drop texture is
        // grey 128, so twice the colour).
        // A little lighter than the haze, as sunlit dust: against the sky a
        // grain in the fog colour itself read as a dark scratch.
        const glm::vec3 grain = glm::mix(fogColour_, glm::vec3(1.0f), 0.3f);
        draw(RAIN_STREAK, particleFirst, particleVertexCount_,
             glm::vec4(glm::min(grain * 2.0f, glm::vec3(2.0f)), 0.4f));
    } else if (builtType_ != Type::NONE) {
        draw(RAIN_STREAK, particleFirst, particleVertexCount_, glm::vec4(1.0f, 1.0f, 1.0f, -1.0f));
        draw(RAIN_SPLASH, splashFirst, splashVertexCount_,
             glm::vec4(1.0f, 1.0f, 1.0f, -1.0f));
    }
    // Fireflies: a warm yellow-green, added (the puff's alpha peaks near
    // half, hence the strength).
    draw(MIST_SHEET, fireflyFirst, fireflyVertexCount_, glm::vec4(1.3f, 1.7f, 0.6f, 1.0f), true);
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
        destroy(device, addPipeline_);
        destroyPipeline(device, pipeline, pipelineLayout);
        destroy(device, descriptorPool_);
        destroy(device, textureSetLayout_);
        textureSets_.fill(VK_NULL_HANDLE);
        texturesAttempted_ = false;
        extraTexturesAttempted_ = false;
    }

    vkCtx = nullptr;
    particles.clear();
    splashes_.clear();
    sheets_.clear();
    fireflies_.clear();
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
