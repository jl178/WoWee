#include "rendering/footfall_dust.hpp"

#include "rendering/camera.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdlib>

namespace wowee {
namespace rendering {

FootfallDust::~FootfallDust() { shutdown(); }

void FootfallDust::buildPipeline() {
    VkDevice device = vkCtx_->getDevice();
    auto shaders = loadShaderPair(device, "assets/shaders/footfall_dust.vert.spv",
                                  "assets/shaders/footfall_dust.frag.spv", "footfall_dust");
    if (!shaders) return;

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::vector<VkVertexInputAttributeDescription> attrs = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
         .offset = static_cast<uint32_t>(offsetof(Vertex, pos))},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
         .offset = static_cast<uint32_t>(offsetof(Vertex, uv))},
        {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
         .offset = static_cast<uint32_t>(offsetof(Vertex, colourAlpha))},
        {.location = 3, .binding = 0, .format = VK_FORMAT_R32_SFLOAT,
         .offset = static_cast<uint32_t>(offsetof(Vertex, seed))},
    };

    // In the scene pass with the other ground effects (the footprints, the
    // weather), tested against the scene's depth and not writing it.
    pipeline_ = PipelineBuilder()
        .setShaders(shaders.vertStage, shaders.fragStage)
        .setVertexInput({binding}, attrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());
}

bool FootfallDust::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    if (!ctx) return false;
    vkCtx_ = ctx;
    VkDevice device = ctx->getDevice();

    pipelineLayout_ = createPipelineLayout(device, {perFrameLayout}, {});
    if (pipelineLayout_ == VK_NULL_HANDLE) return false;
    buildPipeline();
    if (pipeline_ == VK_NULL_HANDLE) {
        shutdown();
        return false;
    }

    frameBufferSize_ = static_cast<VkDeviceSize>(kMaxSprites) * 6 * sizeof(Vertex);
    frameBuffers_.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto& fb : frameBuffers_) {
        AllocatedBuffer buf = createBuffer(ctx->getAllocator(), frameBufferSize_,
                                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        fb.buffer = buf.buffer;
        fb.alloc = buf.allocation;
        fb.mapped = buf.info.pMappedData;
        if (fb.buffer == VK_NULL_HANDLE || !fb.mapped) {
            shutdown();
            return false;
        }
    }
    sprites_.reserve(kMaxSprites);
    vertices_.reserve(static_cast<size_t>(kMaxSprites) * 6);
    LOG_INFO("Footfall dust: made");
    return true;
}

void FootfallDust::shutdown() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    for (auto& fb : frameBuffers_) {
        if (fb.buffer) vmaDestroyBuffer(vkCtx_->getAllocator(), fb.buffer, fb.alloc);
        fb = FrameBuffer{};
    }
    frameBuffers_.clear();
    destroyPipeline(device, pipeline_, pipelineLayout_);
    sprites_.clear();
    vertices_.clear();
    vkCtx_ = nullptr;
}

void FootfallDust::recreatePipelines() {
    if (!vkCtx_) return;
    destroy(vkCtx_->getDevice(), pipeline_);
    buildPipeline();
}

void FootfallDust::spawn(const glm::vec3& foot, const glm::vec2& moveDir,
                         const footfall_dust::Puff& puff, const glm::vec3& colour) {
    if (puff.sprites <= 0 || !vkCtx_) return;
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const glm::vec2 side(-moveDir.y, moveDir.x);
    for (int i = 0; i < puff.sprites; ++i) {
        if (sprites_.size() >= kMaxSprites) sprites_.erase(sprites_.begin());  // the oldest goes
        Sprite s{};
        // Spread a little along and across the step; the centre a little above
        // the ground, so the soft rim, not the middle, meets it.
        const float along = (u(rng_) - 0.6f) * 0.5f * puff.size * 3.0f;
        const float across = (u(rng_) - 0.5f) * 0.6f * puff.size * 3.0f;
        const glm::vec2 off = moveDir * along + side * across;
        s.pos = foot + glm::vec3(off, puff.size * 0.7f);
        // Kicked back from the stride and outward, rising slowly; the drag in
        // update() lets it hang.
        const float kick = 0.35f + 0.35f * u(rng_);
        const glm::vec2 out = side * ((u(rng_) - 0.5f) * 0.6f);
        s.vel = glm::vec3(-moveDir * kick + out, 0.18f + 0.22f * u(rng_));
        s.colour = colour * (0.92f + 0.16f * u(rng_));
        s.age = 0.0f;
        s.life = puff.life * (0.85f + 0.3f * u(rng_));
        s.size = puff.size * (0.85f + 0.3f * u(rng_));
        // WOWEE_FOOTFALL_DUST_GAIN=<k>: the opacity scaled, to find the puffs
        // in a screenshot while tuning. 1 without it.
        static const float gain = [] {
            const char* v = std::getenv("WOWEE_FOOTFALL_DUST_GAIN");
            const float g = v ? static_cast<float>(std::atof(v)) : 1.0f;
            return g > 0.0f ? g : 1.0f;
        }();
        s.alpha = std::min(1.0f, puff.alpha * gain);
        s.seed = u(rng_) * 64.0f;
        sprites_.push_back(s);
    }
}

void FootfallDust::update(float deltaTime, const Camera& camera) {
    vertices_.clear();
    if (sprites_.empty()) return;
    const float drag = std::exp(-2.2f * deltaTime);
    size_t w = 0;
    for (size_t r = 0; r < sprites_.size(); ++r) {
        Sprite s = sprites_[r];
        s.age += deltaTime;
        if (s.age >= s.life) continue;
        s.pos += s.vel * deltaTime;
        s.vel.x *= drag;
        s.vel.y *= drag;
        s.vel.z *= std::exp(-1.2f * deltaTime);
        sprites_[w++] = s;
    }
    sprites_.resize(w);

    // Camera-facing quads, from the view matrix's rows.
    const glm::mat4& v = camera.getViewMatrix();
    const glm::vec3 right(v[0][0], v[1][0], v[2][0]);
    const glm::vec3 up(v[0][1], v[1][1], v[2][1]);
    for (const Sprite& s : sprites_) {
        const float t = s.age / s.life;
        const float a = s.alpha * footfall_dust::alphaOverLife(t);
        if (a <= 0.002f) continue;
        // Spreads fast at first, then slowly: about 3.5x its start.
        const float r = s.size * (1.0f + 2.5f * std::sqrt(t));
        const glm::vec3 rx = right * r, uy = up * r;
        const glm::vec4 ca(s.colour, a);
        const Vertex q[4] = {
            {s.pos - rx - uy, {0.0f, 0.0f}, ca, s.seed},
            {s.pos + rx - uy, {1.0f, 0.0f}, ca, s.seed},
            {s.pos + rx + uy, {1.0f, 1.0f}, ca, s.seed},
            {s.pos - rx + uy, {0.0f, 1.0f}, ca, s.seed},
        };
        vertices_.push_back(q[0]);
        vertices_.push_back(q[1]);
        vertices_.push_back(q[2]);
        vertices_.push_back(q[0]);
        vertices_.push_back(q[2]);
        vertices_.push_back(q[3]);
    }
}

void FootfallDust::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (vertices_.empty() || pipeline_ == VK_NULL_HANDLE || frameBuffers_.empty()) return;
    const FrameBuffer& fb = frameBuffers_[vkCtx_->getCurrentFrame() % frameBuffers_.size()];
    const VkDeviceSize bytes = std::min<VkDeviceSize>(vertices_.size() * sizeof(Vertex), frameBufferSize_);
    std::memcpy(fb.mapped, vertices_.data(), static_cast<size_t>(bytes));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                            &perFrameSet, 0, nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &fb.buffer, &offset);
    vkCmdDraw(cmd, static_cast<uint32_t>(bytes / sizeof(Vertex)), 1, 0, 0);
}

}  // namespace rendering
}  // namespace wowee
