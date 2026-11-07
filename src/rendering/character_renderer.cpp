/**
 * CharacterRenderer - GPU rendering of M2 character models with skeletal animation (Vulkan)
 *
 * Handles:
 *  - Uploading M2 vertex/index data to Vulkan buffers via VMA
 *  - Per-frame bone matrix computation (hierarchical, with keyframe interpolation)
 *  - GPU vertex skinning via a bone-matrix SSBO in the vertex shader
 *  - Per-batch texture binding through the M2 texture-lookup indirection
 *  - Geoset filtering (activeGeosets) to show/hide body part groups
 *  - CPU texture compositing for character skins (base skin + underwear overlays)
 *
 * The character texture compositing uses the WoW CharComponentTextureSections
 * layout, placing region overlays (pelvis, torso, etc.) at their correct pixel
 * positions on the 512x512 body skin atlas. Region coordinates sourced from
 * the original WoW Model Viewer (charcontrol.h, REGION_FAC=2).
 */
#include <atomic>
#include "rendering/character_renderer.hpp"
#include "rendering/indoor_probes.hpp"
#include "rendering/pom_quality.hpp"
#include "rendering/shadow_params.hpp"
#include "rendering/normal_map.hpp"
#include "rendering/m2_track_sampler.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/sequence_variation.hpp"
#include "rendering/animation/anim_selection.hpp"
#include "rendering/corpse_settle.hpp"
#include "core/thread_pool.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/bone_slots.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "rendering/shadow_cache.hpp"
#include "rendering/anim_cull.hpp"
#include "rendering/shadow_draw_list.hpp"
#include "rendering/m2_model_classifier.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/m2_asset_loader.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/unit_light_rules.hpp"
#include "rendering/character_composite_rules.hpp"
#include "core/env_flag.hpp"
#include "core/geoset_rules.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <climits>
#include <cmath>
#include <filesystem>
#include <future>
#include <numeric>
#include <thread>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <cstring>

namespace wowee {
namespace rendering {

namespace {
size_t approxTextureBytesWithMips(int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    size_t base = static_cast<size_t>(w) * static_cast<size_t>(h) * 4ull;
    return base + (base / 3);  // ~4/3 for mip chain
}

std::string normalizeTexturePathKey(std::string key) {
    std::replace(key.begin(), key.end(), '/', '\\');
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return key;
}

bool isMagentaKeyCandidate(const uint8_t* rgba) {
    const int r = rgba[0];
    const int g = rgba[1];
    const int b = rgba[2];
    const int rbDelta = (r > b) ? (r - b) : (b - r);
    return r >= 170 && b >= 170 && g <= 120 &&
           r >= g + 70 && b >= g + 70 && rbDelta <= 96;
}

bool shouldApplyMagentaKey(const std::string& normalizedPath) {
    return normalizedPath.find("character\\") == 0 ||
           normalizedPath.find("item\\texturecomponents\\") == 0 ||
           normalizedPath.find("item\\objectcomponents\\") == 0 ||
           normalizedPath.find("\\hair") != std::string::npos ||
           normalizedPath.find("hair") == 0 ||
           normalizedPath.find("\\cape\\") != std::string::npos ||
           normalizedPath.find("cape\\") == 0;
}

size_t bleedAndStripMagentaKey(std::vector<uint8_t>& rgba, int width, int height) {
    if (width <= 0 || height <= 0 || rgba.size() < static_cast<size_t>(width) * height * 4) {
        return 0;
    }

    const size_t pixelCount = static_cast<size_t>(width) * height;
    std::vector<uint8_t> source = rgba;
    std::vector<uint8_t> mask(pixelCount, 0);
    size_t stripped = 0;

    for (size_t p = 0; p < pixelCount; ++p) {
        size_t i = p * 4;
        if (!isMagentaKeyCandidate(&source[i])) continue;
        mask[p] = 1;
        ++stripped;
    }

    if (stripped == 0) return 0;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            size_t p = static_cast<size_t>(y) * width + x;
            if (!mask[p]) continue;

            uint32_t rSum = 0;
            uint32_t gSum = 0;
            uint32_t bSum = 0;
            uint32_t samples = 0;

            for (int radius = 1; radius <= 8 && samples == 0; ++radius) {
                for (int dy = -radius; dy <= radius; ++dy) {
                    int ny = y + dy;
                    if (ny < 0 || ny >= height) continue;
                    for (int dx = -radius; dx <= radius; ++dx) {
                        if (std::abs(dx) != radius && std::abs(dy) != radius) continue;
                        int nx = x + dx;
                        if (nx < 0 || nx >= width) continue;

                        size_t np = static_cast<size_t>(ny) * width + nx;
                        if (mask[np]) continue;
                        size_t ni = np * 4;
                        if (source[ni + 3] == 0) continue;
                        rSum += source[ni + 0];
                        gSum += source[ni + 1];
                        bSum += source[ni + 2];
                        ++samples;
                    }
                }
            }

            size_t i = p * 4;
            if (samples > 0) {
                rgba[i + 0] = static_cast<uint8_t>(rSum / samples);
                rgba[i + 1] = static_cast<uint8_t>(gSum / samples);
                rgba[i + 2] = static_cast<uint8_t>(bSum / samples);
            } else {
                rgba[i + 0] = 0;
                rgba[i + 1] = 0;
                rgba[i + 2] = 0;
            }
            rgba[i + 3] = 0;
        }
    }
    return stripped;
}

bool hasNonOpaqueAlpha(const std::vector<uint8_t>& rgba) {
    for (size_t i = 3; i < rgba.size(); i += 4) {
        if (rgba[i] != 255) {
            return true;
        }
    }
    return false;
}

void applyMagentaKeyIfNeeded(pipeline::BLPImage& image, const std::string& path) {
    if (!image.isValid()) return;
    std::string key = normalizeTexturePathKey(path);
    if (!shouldApplyMagentaKey(key)) return;
    bleedAndStripMagentaKey(image.data, image.width, image.height);
}
} // namespace

// Descriptor pool sizing
static constexpr uint32_t MAX_MATERIAL_SETS = 4096;
static constexpr uint32_t MAX_BONE_SETS = 8192;

// Texture compositing sizes (NPC skin upscale)
static constexpr int kBaseTexSize    = 256;  // NPC baked texture default
static constexpr int kUpscaleTexSize = 512;  // Target size for region compositing
static constexpr int32_t kPreviewSimpleTextureMode = -31336;

// WOWEE_SCENE_DIAG=1 - dump what each glue-scene backdrop batch is handed at draw
// time. The scene renders through the character path, so when it comes out wrong
// the question is always which texture, blend mode and shader path it actually got.
static bool sceneDiagEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("WOWEE_SCENE_DIAG");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

// Evaluate a batch's M2 color-alpha track at the given animation sequence/time.
// Returns 1.0 when the batch has no color slot or the track has no usable data.
// Global-sequence-timed tracks are not culled (they run on a different clock and
// typically drive pulsing glows, not visibility gating).
static float evalBatchColorAlpha(const pipeline::M2Model& model,
                                 const pipeline::M2Batch& batch,
                                 int sequenceIndex, float animationTimeMs,
                                 float globalTimeMs) {
    if (batch.colorIndex == 0xFFFF ||
        batch.colorIndex >= model.colorAlphaTracks.size()) {
        return 1.0f;
    }
    return m2_track::sampleFloat(model.colorAlphaTracks[batch.colorIndex],
                                 sequenceIndex, animationTimeMs, globalTimeMs,
                                 model.globalSequenceDurations, 1.0f);
}

// Evaluate the material transparency track selected through the skin batch's
// lookup table. Enchant cards depend on this track for their authored duty
// cycle and strength; treating the first key as a constant makes every pulse
// both brighter and several times more frequent than intended.
static float evalBatchTextureWeight(const pipeline::M2Model& model,
                                    const pipeline::M2Batch& batch,
                                    int sequenceIndex, float animationTimeMs,
                                    float globalTimeMs) {
    if (batch.transparencyIndex == 0xFFFF ||
        batch.transparencyIndex >= model.textureWeightLookup.size()) {
        return 1.0f;
    }
    const uint16_t trackIndex = model.textureWeightLookup[batch.transparencyIndex];
    if (trackIndex == 0xFFFF || trackIndex >= model.textureWeightTracks.size()) {
        return 1.0f;
    }
    return m2_track::sampleFloat(model.textureWeightTracks[trackIndex],
                                 sequenceIndex, animationTimeMs, globalTimeMs,
                                 model.globalSequenceDurations, 1.0f);
}

// CharMaterial UBO layout (matches character.frag.glsl set=1 binding=1)
struct CharMaterialUBO {
    float opacity;
    int32_t alphaTest;
    int32_t colorKeyBlack;
    int32_t unlit;
    float emissiveBoost;
    float emissiveTintR, emissiveTintG, emissiveTintB;
    float specularIntensity;
    int32_t enableNormalMap;
    int32_t enablePOM;
    float pomScale;
    int32_t pomMaxSamples;
    float heightMapVariance;
    float normalMapStrength;
    int32_t hairMaterial;
    float ambientMul;   // unit-only ambient boost (0x783429)
    float highlight;    // 1 for the mouseover / target (0x743C70)
    // The stock pixel shader (m2fx::Combiner, m2_combiner.glsli) and how many
    // texture stages it reads.
    int32_t combiner;
    int32_t texStages;
    // The M2 blend mode: Mod and Mod2x take no light and fog toward white
    // and grey, additive toward black (0xA45390).
    int32_t blendMode;
    // The model's colour (CM2Model +0x180, Wow.exe 0x82E140): it scales the
    // sun's diffuse light on the model, not the ambient. A spell's
    // SpellVisualKit CharProc 1 or 13 sets it on a unit (0x720DB0) - the
    // blue of a Frostbolt's chill, Immolate's orange.
    float diffuseTintR;
    float diffuseTintG;
    float diffuseTintB;
    // Room light indoors (E12, updateIndoorProbes): probeW 0 is stock (the
    // player's building colour and indoor factor from the per-frame block);
    // 1 + indoor otherwise, with the unit's own colour and lean.
    float probeR;
    float probeG;
    float probeB;
    float probeW;
    float probeLeanX;
    float probeLeanY;
    float probePad0;
    float probePad1;
};

// GPU vertex struct with tangent (expanded from M2Vertex for normal mapping)
struct CharVertexGPU {
    glm::vec3 position;      // 12 bytes, offset 0
    uint8_t boneWeights[4];  // 4 bytes,  offset 12
    uint8_t boneIndices[4];  // 4 bytes,  offset 16
    glm::vec3 normal;        // 12 bytes, offset 20
    glm::vec2 texCoords;     // 8 bytes,  offset 32
    glm::vec4 tangent;       // 16 bytes, offset 40 (xyz=dir, w=handedness)
    glm::vec2 texCoords2;    // 8 bytes,  offset 56: the second UV set
};  // 64 bytes total

CharacterRenderer::CharacterRenderer() {
}

CharacterRenderer::~CharacterRenderer() {
    shutdown();
}

/// Builds the five main-pass pipelines from an already-loaded shader pair.
///
/// initialize() and recreatePipelines() both need exactly these five, blend
/// state apart, and each described the vertex layout and the builder for
/// itself. The layout is the part worth having once: the bone weights and
/// indices are packed bytes, so a description that drifts from the struct
/// feeds the skinning garbage rather than failing to build.
void CharacterRenderer::buildMainPassPipelines(VkDevice device, VkRenderPass mainPass,
                                               VkSampleCountFlagBits samples,
                                               wowee::rendering::VkShaderModule& charVert,
                                               wowee::rendering::VkShaderModule& charFrag) {
    // --- Vertex input ---
    // CharVertexGPU: vec3 pos(12) + uint8[4] boneWeights(4) + uint8[4] boneIndices(4) +
    //               vec3 normal(12) + vec2 texCoords(8) + vec4 tangent(16) + vec2 texCoords2(8) = 64 bytes
    VkVertexInputBindingDescription charBinding{};
    charBinding.binding = 0;
    charBinding.stride = sizeof(CharVertexGPU);
    charBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> charAttrs = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, position))},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UNORM,   .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, boneWeights))},
        {.location = 2, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UINT,     .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, boneIndices))},
        {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,  .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, normal))},
        {.location = 4, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,     .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, texCoords))},
        {.location = 5, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, tangent))},
        {.location = 6, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,     .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, texCoords2))},
    };

    // --- Build pipelines ---
    auto buildCharPipeline = [&](VkPipelineColorBlendAttachmentState blendState,
                                  bool depthWrite, bool alphaToCoverage = false) -> VkPipeline {
        auto builder = PipelineBuilder()
            .setShaders(charVert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                        charFrag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({charBinding}, charAttrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            // Counter-clockwise front: M2 triangles wind counter-clockwise
            // about their outward normals (8483 of 8638 in UI_Human, 6289 of
            // 6356 in HumanMale), and the camera's Y-flipped projection keeps
            // that winding on screen. With the builder's clockwise default
            // every face turned to the camera reported !gl_FrontFacing, and
            // character.frag turned its normal inward: lit from behind.
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .setDepthTest(true, depthWrite, VK_COMPARE_OP_LESS)
            .setDepthBias(0.0f, 0.0f)
            .setColorBlendAttachment(blendState)
            .setMultisample(samples);
        if (alphaToCoverage)
            builder.setAlphaToCoverage(true);
        return builder
            .setLayout(pipelineLayout_)
            .setRenderPass(mainPass)
            .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS})
            .build(device, vkCtx_->getPipelineCache());
    };

    opaquePipeline_ = buildCharPipeline(PipelineBuilder::blendDisabled(), true);
    alphaTestPipeline_ = buildCharPipeline(PipelineBuilder::blendDisabled(), true, true);
    alphaPipeline_ = buildCharPipeline(PipelineBuilder::blendAlpha(), false);
    additivePipeline_ = buildCharPipeline(PipelineBuilder::blendAdditive(), false);
    translucentPipeline_ = buildCharPipeline(PipelineBuilder::blendAlpha(), true);
    // The rest of the stock blend table, the framebuffer's alpha left alone.
    const auto colourOnly = [](VkBlendFactor src, VkBlendFactor dst) {
        VkPipelineColorBlendAttachmentState st = PipelineBuilder::blendAlpha();
        st.srcColorBlendFactor = src;
        st.dstColorBlendFactor = dst;
        st.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        st.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        return st;
    };
    addOnePipeline_ = buildCharPipeline(colourOnly(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE), false);
    modPipeline_ = buildCharPipeline(colourOnly(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ZERO), false);
    mod2xPipeline_ = buildCharPipeline(colourOnly(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_SRC_COLOR), false);

}

bool CharacterRenderer::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                                    pipeline::AssetManager* am,
                                    VkRenderPass renderPassOverride,
                                    VkSampleCountFlagBits msaaSamples) {
    core::Logger::getInstance().info("Initializing character renderer (Vulkan)...");

    vkCtx_ = ctx;
    assetManager = am;
    perFrameLayout_ = perFrameLayout;
    renderPassOverride_ = renderPassOverride;
    msaaSamplesOverride_ = msaaSamples;
    const unsigned hc = std::thread::hardware_concurrency();
    const size_t availableCores = (hc > 1u) ? static_cast<size_t>(hc - 1u) : 1ull;
    // Character updates run alongside M2/WMO work; default to a smaller share.
    const size_t defaultAnimThreads = std::max<size_t>(1, availableCores / 4);
    numAnimThreads_ = static_cast<uint32_t>(std::max<size_t>(
        1, envSizeOrDefault("WOWEE_CHAR_ANIM_THREADS", defaultAnimThreads)));
    core::Logger::getInstance().info("Character anim threads: ", numAnimThreads_);

    VkDevice device = vkCtx_->getDevice();

    // --- Descriptor set layouts ---

    // Material set layout (set 1): binding 0 = sampler2D, binding 1 = CharMaterial UBO, binding 2 = normal/height map,
    // binding 3 = the second texture stage
    {
        VkDescriptorSetLayoutBinding bindings[4] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[3].binding = 3;
        bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[3].descriptorCount = 1;
        bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 4;
        ci.pBindings = bindings;
        vkCreateDescriptorSetLayout(device, &ci, nullptr, &materialSetLayout_);
    }

    // Bone set layout (set 2): binding 0 = STORAGE_BUFFER (bone matrices)
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1;
        ci.pBindings = &binding;
        vkCreateDescriptorSetLayout(device, &ci, nullptr, &boneSetLayout_);
    }

    // --- Descriptor pools ---
    // Material descriptors are transient and allocated every draw; keep per-frame
    // pools so we can reset safely each frame slot without exhausting descriptors.
    for (auto& materialDescPool : materialDescPools_) {
        VkDescriptorPoolSize sizes[] = {
            {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = MAX_MATERIAL_SETS * 3},  // diffuse + normal/height + stage 2
            {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .descriptorCount = MAX_MATERIAL_SETS},
        };
        VkDescriptorPoolCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = MAX_MATERIAL_SETS;
        ci.poolSizeCount = 2;
        ci.pPoolSizes = sizes;
        ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        vkCreateDescriptorPool(device, &ci, nullptr, &materialDescPool);
    }
    {
        VkDescriptorPoolSize sizes[] = {
            {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = MAX_BONE_SETS},
        };
        VkDescriptorPoolCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = MAX_BONE_SETS;
        ci.poolSizeCount = 1;
        ci.pPoolSizes = sizes;
        ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        vkCreateDescriptorPool(device, &ci, nullptr, &boneDescPool_);
    }

    // --- Material UBO ring buffers (one per frame slot) ---
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(ctx->getPhysicalDevice(), &props);
        materialUboAlignment_ = static_cast<uint32_t>(props.limits.minUniformBufferOffsetAlignment);
        if (materialUboAlignment_ < 1) materialUboAlignment_ = 1;
        // Round up UBO size to alignment
        uint32_t alignedUboSize = (sizeof(CharMaterialUBO) + materialUboAlignment_ - 1) & ~(materialUboAlignment_ - 1);
        uint32_t ringSize = alignedUboSize * MATERIAL_RING_CAPACITY;
        for (int i = 0; i < 2; i++) {
            VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = ringSize;
            bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo allocInfo{};
            vmaCreateBuffer(ctx->getAllocator(), &bci, &aci,
                            &materialRingBuffer_[i], &materialRingAlloc_[i], &allocInfo);
            materialRingMapped_[i] = allocInfo.pMappedData;
        }
    }

    // --- Pipeline layout ---
    // set 0 = perFrame, set 1 = material, set 2 = bones
    // Push constant: mat4 model (64 bytes) + vec4 uvOffset (16 bytes, the
    // batch's texture-animation scroll per stage; see character.vert.glsl) +
    // ivec4 stage sources (16 bytes)
    {
        VkDescriptorSetLayout setLayouts[] = {perFrameLayout, materialSetLayout_, boneSetLayout_};
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset = 0;
        pushRange.size = 96; // mat4 + vec4 + ivec4

        VkPipelineLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 3;
        ci.pSetLayouts = setLayouts;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pushRange;
        vkCreatePipelineLayout(device, &ci, nullptr, &pipelineLayout_);
    }

    // --- Load shaders ---
    rendering::VkShaderModule charVert, charFrag;
    if (!charVert.loadFromFile(device, "assets/shaders/character.vert.spv") ||
        !charFrag.loadFromFile(device, "assets/shaders/character.frag.spv")) {
        LOG_ERROR("Character: Missing required shaders, cannot initialize");
        return false;
    }

    VkRenderPass mainPass = renderPassOverride_ ? renderPassOverride_ : vkCtx_->getImGuiRenderPass();
    VkSampleCountFlagBits samples = renderPassOverride_ ? msaaSamplesOverride_ : vkCtx_->getMsaaSamples();

    buildMainPassPipelines(device, mainPass, samples, charVert, charFrag);

    // Clean up shader modules
    charVert.destroy();
    charFrag.destroy();

    createFallbackTextures(device);

    // Diagnostics-only: cache lifetime is currently tied to renderer lifetime.
    textureCacheBudgetBytes_ = envSizeMBOrDefault("WOWEE_CHARACTER_TEX_CACHE_MB", 4096) * 1024ull * 1024ull;
    LOG_INFO("Character texture cache budget: ", textureCacheBudgetBytes_ / (1024 * 1024), " MB");

    core::Logger::getInstance().info("Character renderer initialized (Vulkan)");
    return true;
}

void CharacterRenderer::shutdown() {
    if (!vkCtx_) return;

    LOG_INFO("CharacterRenderer::shutdown instances=", instances.size(),
             " models=", models.size(), " override=", (void*)renderPassOverride_);

    // Wait for any in-flight background normal map generation threads
    {
        std::unique_lock<std::mutex> lock(normalMapResultsMutex_);
        normalMapDoneCV_.wait(lock, [this] {
            return pendingNormalMapCount_.load(std::memory_order_acquire) == 0;
        });
    }

    vkDeviceWaitIdle(vkCtx_->getDevice());
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator alloc = vkCtx_->getAllocator();

    // Clean up GPU resources for models
    for (auto& pair : models) {
        destroyModelGPU(pair.second);
    }

    // Clean up instance bone buffers
    for (auto& pair : instances) {
        destroyInstanceBones(pair.second);
    }

    // Model and bone destruction above is deferred; drain it now while the
    // descriptor pools are still alive, since no further frames will run.
    vkCtx_->flushDeferredCleanup();

    // ~VkTexture is empty by design -- it has no device or allocator to free
    // with -- so clearing the map drops the unique_ptrs without destroying the
    // image, view or allocation behind each one. Both textures in an entry are
    // owned here.
    for (auto& [path, entry] : textureCache) {
        if (entry.texture) entry.texture->destroy(device, alloc);
        if (entry.normalHeightMap) entry.normalHeightMap->destroy(device, alloc);
    }
    textureCache.clear();
    // The singletons the cache never held. Same reason as above.
    if (whiteTexture_)       { whiteTexture_->destroy(device, alloc);       whiteTexture_.reset(); }
    if (transparentTexture_) { transparentTexture_->destroy(device, alloc); transparentTexture_.reset(); }
    if (unboundTexture_)     { unboundTexture_->destroy(device, alloc);     unboundTexture_.reset(); }
    if (defaultTexture_)     { defaultTexture_->destroy(device, alloc);     defaultTexture_.reset(); }
    if (flatNormalTexture_)  { flatNormalTexture_->destroy(device, alloc);  flatNormalTexture_.reset(); }
    texturePropsByPtr_.clear();
    normalMapByTexPtr_.clear();
    textureCacheBytes_ = 0;
    textureCacheCounter_ = 0;

    // Clean up composite cache
    compositeCache_.clear();
    failedTextureCache_.clear();
    failedTextureRetryAt_.clear();
    textureLookupSerial_ = 0;

    whiteTexture_.reset();
    transparentTexture_.reset();
    unboundTexture_.reset();
    defaultTexture_.reset();
    flatNormalTexture_.reset();

    models.clear();
    instances.clear();
    ++instancesGeneration_;

    // Destroy pipelines
    auto destroyPipeline = [&](VkPipeline& p) {
        destroy(device, p);
    };
    destroyPipeline(opaquePipeline_);
    destroyPipeline(alphaTestPipeline_);
    destroyPipeline(alphaPipeline_);
    destroyPipeline(additivePipeline_);
    destroyPipeline(translucentPipeline_);
    destroyPipeline(addOnePipeline_);
    destroyPipeline(modPipeline_);
    destroyPipeline(mod2xPipeline_);

    destroy(device, pipelineLayout_);

    // Destroy material ring buffers
    for (int i = 0; i < 2; i++) {
        if (materialRingBuffer_[i]) {
            vmaDestroyBuffer(alloc, materialRingBuffer_[i], materialRingAlloc_[i]);
            materialRingBuffer_[i] = VK_NULL_HANDLE;
            materialRingAlloc_[i] = VK_NULL_HANDLE;
            materialRingMapped_[i] = nullptr;
        }
        materialRingOffset_[i] = 0;
    }

    // Destroy descriptor pools and layouts
    for (auto& materialDescPool : materialDescPools_) {
        destroy(device, materialDescPool);
    }
    if (boneDescPool_) {
        if (boneDescPoolGeneration_) boneDescPoolGeneration_->fetch_add(1, std::memory_order_relaxed);
        vkDestroyDescriptorPool(device, boneDescPool_, nullptr);
        boneDescPool_ = VK_NULL_HANDLE;
    }
    destroy(device, materialSetLayout_);
    destroy(device, boneSetLayout_);

    // Shadow resources
    destroy(device, shadowPipeline_);
    destroy(device, shadowPipelineLayout_);
    destroyShadowParamsSet(device, alloc, shadowParams_);
    shadowTexSetCache_.clear();
    for (auto& pool : shadowTexPool_) {
        if (pool) { vkDestroyDescriptorPool(device, pool, nullptr); pool = VK_NULL_HANDLE; }
    }

    vkCtx_ = nullptr;
}

void CharacterRenderer::clear() {
    if (!vkCtx_) return;

    LOG_INFO("CharacterRenderer::clear instances=", instances.size(),
             " models=", models.size());

    // Wait for any in-flight background normal map generation threads
    {
        std::unique_lock<std::mutex> lock(normalMapResultsMutex_);
        normalMapDoneCV_.wait(lock, [this] {
            return pendingNormalMapCount_.load(std::memory_order_acquire) == 0;
        });
    }
    // Discard any completed results that haven't been uploaded
    {
        std::lock_guard<std::mutex> lock(normalMapResultsMutex_);
        completedNormalMaps_.clear();
    }

    vkDeviceWaitIdle(vkCtx_->getDevice());
    VkDevice device = vkCtx_->getDevice();

    // Destroy GPU resources for all models
    for (auto& pair : models) {
        destroyModelGPU(pair.second);
    }

    // Destroy bone buffers for all instances
    for (auto& pair : instances) {
        destroyInstanceBones(pair.second);
        // Weapon glows and emitter twins hosted by M2Renderer are not ours
        // to clear with the map.
        for (const auto& wa : pair.second.weaponAttachments)
            for (const auto& fx : wa.effects)
                if (fx.m2InstanceId != 0 && effectRenderer_) effectRenderer_->removeInstance(fx.m2InstanceId);
        if (pair.second.emitterInstanceId != 0 && effectRenderer_)
            effectRenderer_->removeInstance(pair.second.emitterInstanceId);
    }

    // See CharacterRenderer::shutdown: ~VkTexture frees nothing on its own.
    if (vkCtx_) {
        VkDevice dev = vkCtx_->getDevice();
        VmaAllocator vma = vkCtx_->getAllocator();
        for (auto& [path, entry] : textureCache) {
            if (entry.texture) entry.texture->destroy(dev, vma);
            if (entry.normalHeightMap) entry.normalHeightMap->destroy(dev, vma);
        }
    }
    textureCache.clear();
    texturePropsByPtr_.clear();
    normalMapByTexPtr_.clear();
    textureCacheBytes_ = 0;
    textureCacheCounter_ = 0;
    loggedTextureLoadFails_.clear();
    failedTextureRetryAt_.clear();
    textureLookupSerial_ = 0;

    // Clear composite and failed caches
    compositeCache_.clear();
    failedTextureCache_.clear();

    // The fallback textures (white, transparent, unbound, default, flat
    // normal) are kept. They are 1x1 constants made at initialize, and the
    // unit shadow's fallback set (shadowParams_.set, set 0 binding 0) was
    // written with the white one's view then and never again: recreating them
    // here on every map change left that set naming a destroyed view, which
    // every unit's shadow draw bound after the teleport - the validation
    // layer's "combined image sampler descriptor [Set 0, Binding 0] is using
    // imageView 0x0" (enhancements-plan.md 3.12).

    models.clear();
    instances.clear();
    ++instancesGeneration_;

    // Reset material ring buffer offsets (buffers persist, just reset write position)
    for (uint32_t& offset : materialRingOffset_) {
        offset = 0;
    }

    // Reset descriptor pools (don't destroy - reuse for new allocations)
    for (auto& materialDescPool : materialDescPools_) {
        if (materialDescPool) {
            vkResetDescriptorPool(device, materialDescPool, 0);
        }
    }
    if (boneDescPool_) {
        if (boneDescPoolGeneration_) boneDescPoolGeneration_->fetch_add(1, std::memory_order_relaxed);
        vkResetDescriptorPool(device, boneDescPool_, 0);
    }
}

void CharacterRenderer::createFallbackTextures(VkDevice device) {
    // White: default diffuse when no texture is assigned
    {
        uint8_t white[] = {255, 255, 255, 255};
        whiteTexture_ = std::make_unique<VkTexture>();
        whiteTexture_->upload(*vkCtx_, white, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
        whiteTexture_->createSampler(device, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    }
    // Transparent: placeholder for optional overlay layers (e.g. hair highlights)
    {
        uint8_t transparent[] = {0, 0, 0, 0};
        transparentTexture_ = std::make_unique<VkTexture>();
        transparentTexture_->upload(*vkCtx_, transparent, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
        transparentTexture_->createSampler(device, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    }
    // What the stock client binds to a creature skin slot (type 11-13) whose
    // display names no skin: nothing. A sampler with no texture reads
    // (0, 0, 0, 1) - opaque black, and no light at all when added. Kept
    // apart from the white texture, which the draw path treats as "not set
    // yet" and replaces with a body texture (core/creature_skin_rules.hpp).
    {
        uint8_t black[] = {0, 0, 0, 255};
        unboundTexture_ = std::make_unique<VkTexture>();
        unboundTexture_->upload(*vkCtx_, black, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
        unboundTexture_->createSampler(device, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    }
    // The stock default texture (0x4B5280): solid 0xFFFFFFFF, what a named
    // file that is not there loads as. White like whiteTexture_, but a slot
    // holding it is decided, not empty, so nothing substitutes for it.
    {
        uint8_t white[] = {255, 255, 255, 255};
        defaultTexture_ = std::make_unique<VkTexture>();
        defaultTexture_->upload(*vkCtx_, white, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
        defaultTexture_->createSampler(device, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    }
    // Flat normal: neutral normal map (128,128,255) + 0.5 height in alpha channel
    {
        uint8_t flatNormal[] = {128, 128, 255, 128};
        flatNormalTexture_ = std::make_unique<VkTexture>();
        flatNormalTexture_->upload(*vkCtx_, flatNormal, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
        flatNormalTexture_->createSampler(device, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    }
}

void CharacterRenderer::destroyModelGPU(M2ModelGPU& gpuModel, bool defer) {
    if (!vkCtx_) return;
    VmaAllocator alloc = vkCtx_->getAllocator();

    // Snapshot raw handles and null the model fields immediately
    ::VkBuffer vb = gpuModel.vertexBuffer;
    VmaAllocation vbAlloc = gpuModel.vertexAlloc;
    ::VkBuffer ib = gpuModel.indexBuffer;
    VmaAllocation ibAlloc = gpuModel.indexAlloc;
    gpuModel.vertexBuffer = VK_NULL_HANDLE;
    gpuModel.vertexAlloc = VK_NULL_HANDLE;
    gpuModel.indexBuffer = VK_NULL_HANDLE;
    gpuModel.indexAlloc = VK_NULL_HANDLE;

    if (!defer) {
        // Safe after vkDeviceWaitIdle (shutdown / clear paths)
        if (vb) vmaDestroyBuffer(alloc, vb, vbAlloc);
        if (ib) vmaDestroyBuffer(alloc, ib, ibAlloc);
    } else if (vb || ib) {
        // Streaming path: in-flight command buffers may still reference these
        vkCtx_->deferAfterAllFrameFences([alloc, vb, vbAlloc, ib, ibAlloc]() {
            if (vb) vmaDestroyBuffer(alloc, vb, vbAlloc);
            if (ib) vmaDestroyBuffer(alloc, ib, ibAlloc);
        });
    }
}

void CharacterRenderer::destroyInstanceBones(CharacterInstance& inst, bool defer) {
    if (!vkCtx_) return;
    releaseInstanceBones(*vkCtx_, boneDescPool_, boneDescPoolGeneration_, inst, defer);
}

std::unique_ptr<VkTexture> CharacterRenderer::generateNormalHeightMap(
        const uint8_t* pixels, uint32_t width, uint32_t height, float& outVariance) {
    if (!vkCtx_ || width == 0 || height == 0) return nullptr;

    // Use the CPU-only static method, then upload to GPU
    std::vector<uint8_t> dummy(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    std::memcpy(dummy.data(), pixels, dummy.size());
    auto result = generateNormalHeightMapCPU("", std::move(dummy), width, height);
    outVariance = result.variance;

    auto tex = std::make_unique<VkTexture>();
    if (!tex->upload(*vkCtx_, result.pixels.data(), width, height, VK_FORMAT_R8G8B8A8_UNORM, true)) {
        return nullptr;
    }
    tex->createSampler(vkCtx_->getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                        VK_SAMPLER_ADDRESS_MODE_REPEAT);
    return tex;
}

// Static, thread-safe CPU-only normal map generation (no GPU access)
bool CharacterRenderer::queueNormalMapGeneration(const std::string& cacheKey,
                                                std::vector<uint8_t> pixels,
                                                uint32_t width, uint32_t height) {
    // Every surface this renderer draws derives its normal map from its own
    // diffuse art, and this is the one place that starts that work. It used to
    // be spelled out inside the file-loading path alone - so a texture that
    // never came from a file never got one, and a character's body is exactly
    // that: composited in memory from a skin, a face and whatever armour is
    // worn. The largest lit surface on screen was the one surface with no
    // normal map, which reads as the lighting working everywhere except on
    // people.
    if (width < 32 || height < 32) return false;
    // Use acq_rel so the increment is visible to shutdown()'s acquire load
    // before the thread body begins (relaxed could delay visibility and cause
    // shutdown() to see 0 and proceed while a thread is still running).
    pendingNormalMapCount_.fetch_add(1, std::memory_order_acq_rel);
    auto* self = this;
    std::thread([self, ck = cacheKey, px = std::move(pixels), width, height]() mutable {
        // try-catch guarantees the counter is decremented even if the compute
        // throws (e.g., bad_alloc). Without this, shutdown() would deadlock
        // waiting for a count that never reaches zero.
        try {
            auto result = generateNormalHeightMapCPU(std::move(ck), std::move(px),
                                                     width, height);
            {
                std::lock_guard<std::mutex> lock(self->normalMapResultsMutex_);
                self->completedNormalMaps_.push_back(std::move(result));
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Normal map generation failed: ", e.what());
        }
        if (self->pendingNormalMapCount_.fetch_sub(1, std::memory_order_release) == 1) {
            self->normalMapDoneCV_.notify_one();
        }
    }).detach();
    return true;
}

CharacterRenderer::NormalMapResult CharacterRenderer::generateNormalHeightMapCPU(
        std::string cacheKey, std::vector<uint8_t> srcPixels, uint32_t width, uint32_t height) {
    NormalMapResult result;
    result.cacheKey = std::move(cacheKey);
    result.width = width;
    result.height = height;
    result.variance = 0.0f;
    // Five, where the WMO renderer asks for two: this is skin and cloth seen
    // close up, and it wants the gradient exaggerated.
    result.pixels = rendering::generateNormalHeightMap(
        srcPixels.data(), width, height, /*strength=*/5.0f, result.variance);
    return result;
}

VkTexture* CharacterRenderer::loadTexture(const std::string& path) {
    constexpr uint64_t kFailedTextureRetryLookups = 512;
    // Skip empty or whitespace-only paths (type-0 textures have no filename)
    if (path.empty()) return whiteTexture_.get();
    bool allWhitespace = true;
    for (char c : path) {
        if (c != ' ' && c != '\t' && c != '\0' && c != '\n') { allWhitespace = false; break; }
    }
    if (allWhitespace) return whiteTexture_.get();

    std::string key = normalizeTexturePathKey(path);
    const uint64_t lookupSerial = ++textureLookupSerial_;
    // The same question the M2 renderer asks, through the same answer. This
    // carried four of its eleven tokens and searched the whole path, so which
    // textures were colour-keyed depended on which renderer had loaded them.
    const bool colorKeyBlackHint = assetNameLooksLikeFlame(key);

    // Check cache
    auto it = textureCache.find(key);
    if (it != textureCache.end()) {
        it->second.lastUse = ++textureCacheCounter_;
        return it->second.texture.get();
    }
    auto failIt = failedTextureRetryAt_.find(key);
    if (failIt != failedTextureRetryAt_.end() && lookupSerial < failIt->second) {
        return whiteTexture_.get();
    }

    if (!assetManager || !assetManager->isInitialized()) {
        return whiteTexture_.get();
    }

    // Check pre-decoded BLP cache first (populated by background threads)
    pipeline::BLPImage blpImage;
    if (predecodedBLPCache_) {
        auto pit = predecodedBLPCache_->find(key);
        if (pit != predecodedBLPCache_->end()) {
            blpImage = std::move(pit->second);
            predecodedBLPCache_->erase(pit);
        }
    }
    if (!blpImage.isValid()) {
        blpImage = assetManager->loadTexture(key);
    }
    if (!blpImage.isValid()) {
        // Cache misses briefly to avoid repeated expensive MPQ/disk probes.
        failedTextureCache_.insert(key);
        failedTextureRetryAt_[key] = lookupSerial + kFailedTextureRetryLookups;
        if (loggedTextureLoadFails_.insert(key).second) {
            core::Logger::getInstance().warning("Failed to load texture: ", path);
        }
        return whiteTexture_.get();
    }

    applyMagentaKeyIfNeeded(blpImage, key);

    size_t approxBytes = approxTextureBytesWithMips(blpImage.width, blpImage.height);
    if (textureCacheBytes_ + approxBytes > textureCacheBudgetBytes_) {
        static constexpr size_t kMaxFailedTextureCache = 200000;
        if (failedTextureCache_.size() < kMaxFailedTextureCache) {
            // Budget is saturated; avoid repeatedly decoding/uploading this texture.
            failedTextureCache_.insert(key);
            failedTextureRetryAt_[key] = lookupSerial + kFailedTextureRetryLookups;
        }
        if (textureBudgetRejectWarnings_ < 3) {
            core::Logger::getInstance().warning(
                "Character texture cache full (",
                textureCacheBytes_ / (1024 * 1024), " MB / ",
                textureCacheBudgetBytes_ / (1024 * 1024), " MB), rejecting texture: ",
                path);
        }
        ++textureBudgetRejectWarnings_;
        return whiteTexture_.get();
    }

    bool hasAlpha = false;
    for (size_t i = 3; i < blpImage.data.size(); i += 4) {
        if (blpImage.data[i] != 255) {
            hasAlpha = true;
            break;
        }
    }

    auto tex = std::make_unique<VkTexture>();
    tex->upload(*vkCtx_, blpImage.data.data(), blpImage.width, blpImage.height,
                VK_FORMAT_R8G8B8A8_UNORM, true);
    tex->createSampler(vkCtx_->getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                       VK_SAMPLER_ADDRESS_MODE_REPEAT);

    VkTexture* texPtr = tex.get();

    TextureCacheEntry e;
    e.texture = std::move(tex);
    e.approxBytes = approxBytes;
    e.lastUse = ++textureCacheCounter_;
    e.hasAlpha = hasAlpha;
    e.colorKeyBlack = colorKeyBlackHint;

    // Launch normal map generation on background thread - CPU work is pure compute,
    // only the GPU upload (in processPendingNormalMaps) needs the main thread (~1-2ms).
    e.normalMapPending = queueNormalMapGeneration(
        key, std::vector<uint8_t>(blpImage.data.begin(), blpImage.data.end()),
        blpImage.width, blpImage.height);

    textureCacheBytes_ += e.approxBytes;
    texturePropsByPtr_[texPtr] = {.hasAlpha = hasAlpha, .colorKeyBlack = colorKeyBlackHint};
    textureCache[key] = std::move(e);
    failedTextureCache_.erase(key);
    failedTextureRetryAt_.erase(key);

    core::Logger::getInstance().debug("Loaded character texture: ", path, " (", blpImage.width, "x", blpImage.height, ")");
    return texPtr;
}

void CharacterRenderer::processPendingNormalMaps(int budget) {
    if (!vkCtx_) return;

    // Collect completed results from background threads
    std::deque<NormalMapResult> ready;
    {
        std::lock_guard<std::mutex> lock(normalMapResultsMutex_);
        if (completedNormalMaps_.empty()) return;
        int count = std::min(budget, static_cast<int>(completedNormalMaps_.size()));
        for (int i = 0; i < count; i++) {
            ready.push_back(std::move(completedNormalMaps_.front()));
            completedNormalMaps_.pop_front();
        }
    }

    // GPU upload only (~1-2ms each) - CPU work already done on background thread
    for (auto& result : ready) {
        auto it = textureCache.find(result.cacheKey);
        if (it == textureCache.end()) continue;  // texture was evicted

        vkCtx_->beginUploadBatch();
        auto tex = std::make_unique<VkTexture>();
        bool ok = tex->upload(*vkCtx_, result.pixels.data(), result.width, result.height,
                              VK_FORMAT_R8G8B8A8_UNORM, true);
        if (ok) {
            tex->createSampler(vkCtx_->getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                               VK_SAMPLER_ADDRESS_MODE_REPEAT);
            it->second.heightMapVariance = result.variance;
            it->second.approxBytes += approxTextureBytesWithMips(result.width, result.height);
            textureCacheBytes_ += approxTextureBytesWithMips(result.width, result.height);
            it->second.normalHeightMap = std::move(tex);
            if (it->second.texture) {
                normalMapByTexPtr_[it->second.texture.get()] = {
                    .normalMap = it->second.normalHeightMap.get(), .heightMapVariance = it->second.heightMapVariance};
            }
        }
        vkCtx_->endUploadBatch();
        it->second.normalMapPending = false;
    }
}

// Alpha-blend overlay onto composite at (dstX, dstY)
static void blitOverlay(std::vector<uint8_t>& composite, int compW, int compH,
                         const pipeline::BLPImage& overlay, int dstX, int dstY) {
    for (int sy = 0; sy < overlay.height; sy++) {
        int dy = dstY + sy;
        if (dy < 0 || dy >= compH) continue;
        for (int sx = 0; sx < overlay.width; sx++) {
            int dx = dstX + sx;
            if (dx < 0 || dx >= compW) continue;

            size_t srcIdx = (static_cast<size_t>(sy) * overlay.width + sx) * 4;
            size_t dstIdx = (static_cast<size_t>(dy) * compW + dx) * 4;

            uint8_t srcA = overlay.data[srcIdx + 3];
            if (srcA == 0) continue;

            if (srcA == 255) {
                composite[dstIdx + 0] = overlay.data[srcIdx + 0];
                composite[dstIdx + 1] = overlay.data[srcIdx + 1];
                composite[dstIdx + 2] = overlay.data[srcIdx + 2];
                composite[dstIdx + 3] = 255;
            } else {
                float alpha = srcA / 255.0f;
                float invAlpha = 1.0f - alpha;
                composite[dstIdx + 0] = static_cast<uint8_t>(overlay.data[srcIdx + 0] * alpha + composite[dstIdx + 0] * invAlpha);
                composite[dstIdx + 1] = static_cast<uint8_t>(overlay.data[srcIdx + 1] * alpha + composite[dstIdx + 1] * invAlpha);
                composite[dstIdx + 2] = static_cast<uint8_t>(overlay.data[srcIdx + 2] * alpha + composite[dstIdx + 2] * invAlpha);
                composite[dstIdx + 3] = std::max(composite[dstIdx + 3], srcA);
            }
        }
    }
}

// Nearest-neighbor NxN scale blit of overlay onto composite at (dstX, dstY)
// Blit an overlay resampled to an explicit destination size. The integer-scale
// version below only grows an overlay, and only by a whole factor, so an overlay
// that arrives larger than the region it belongs in was pasted at its own size
// and covered several regions of the atlas. Nearest-neighbour is enough here:
// these are small atlas patches, and the alternative was no resize at all.
static void blitOverlayScaledN(std::vector<uint8_t>& composite, int compW, int compH,
                                const pipeline::BLPImage& overlay, int dstX, int dstY, int scale);
static void blitOverlayResampled(std::vector<uint8_t>& composite, int compW, int compH,
                                 const pipeline::BLPImage& overlay,
                                 int dstX, int dstY, int dstW, int dstH) {
    if (dstW <= 0 || dstH <= 0 || overlay.width <= 0 || overlay.height <= 0) return;
    if (dstW == overlay.width * 2 && dstH == overlay.height * 2) {
        blitOverlayScaledN(composite, compW, compH, overlay, dstX, dstY, 2);
        return;
    }
    for (int y = 0; y < dstH; ++y) {
        const int dy = dstY + y;
        if (dy < 0 || dy >= compH) continue;
        const int sy = std::min(overlay.height - 1, y * overlay.height / dstH);
        for (int x = 0; x < dstW; ++x) {
            const int dx = dstX + x;
            if (dx < 0 || dx >= compW) continue;
            const int sx = std::min(overlay.width - 1, x * overlay.width / dstW);

            const size_t srcIdx = (static_cast<size_t>(sy) * overlay.width + sx) * 4;
            const uint8_t srcA = overlay.data[srcIdx + 3];
            if (srcA == 0) continue;

            const size_t dstIdx = (static_cast<size_t>(dy) * compW + dx) * 4;
            if (srcA == 255) {
                composite[dstIdx + 0] = overlay.data[srcIdx + 0];
                composite[dstIdx + 1] = overlay.data[srcIdx + 1];
                composite[dstIdx + 2] = overlay.data[srcIdx + 2];
                composite[dstIdx + 3] = 255;
            } else {
                const float alpha = srcA / 255.0f;
                const float inv = 1.0f - alpha;
                for (int c = 0; c < 3; ++c) {
                    composite[dstIdx + c] = static_cast<uint8_t>(
                        overlay.data[srcIdx + c] * alpha + composite[dstIdx + c] * inv);
                }
                composite[dstIdx + 3] = std::max(composite[dstIdx + 3], srcA);
            }
        }
    }
}

static void blitOverlayScaledN(std::vector<uint8_t>& composite, int compW, int compH,
                                const pipeline::BLPImage& overlay, int dstX, int dstY, int scale) {
    if (scale < 1) scale = 1;
    // Stock grows a 256-composite texture onto a 512 one with neighbour
    // averaging (0x4E89F0 and its siblings), not by repeating texels.
    if (scale == 2 && !overlay.data.empty()) {
        pipeline::BLPImage grown;
        grown.width = overlay.width * 2;
        grown.height = overlay.height * 2;
        grown.data = composite::stockUpscale2x(overlay.data.data(), overlay.width, overlay.height);
        blitOverlay(composite, compW, compH, grown, dstX, dstY);
        return;
    }
    for (int sy = 0; sy < overlay.height; sy++) {
        for (int sx = 0; sx < overlay.width; sx++) {
            size_t srcIdx = (static_cast<size_t>(sy) * overlay.width + sx) * 4;
            uint8_t srcA = overlay.data[srcIdx + 3];
            if (srcA == 0) continue;

            // Write to scale x scale block of destination pixels
            for (int dy2 = 0; dy2 < scale; dy2++) {
                int dy = dstY + sy * scale + dy2;
                if (dy < 0 || dy >= compH) continue;
                for (int dx2 = 0; dx2 < scale; dx2++) {
                    int dx = dstX + sx * scale + dx2;
                    if (dx < 0 || dx >= compW) continue;

                    size_t dstIdx = (static_cast<size_t>(dy) * compW + dx) * 4;
                    if (srcA == 255) {
                        composite[dstIdx + 0] = overlay.data[srcIdx + 0];
                        composite[dstIdx + 1] = overlay.data[srcIdx + 1];
                        composite[dstIdx + 2] = overlay.data[srcIdx + 2];
                        composite[dstIdx + 3] = 255;
                    } else {
                        float alpha = srcA / 255.0f;
                        float invAlpha = 1.0f - alpha;
                        composite[dstIdx + 0] = static_cast<uint8_t>(overlay.data[srcIdx + 0] * alpha + composite[dstIdx + 0] * invAlpha);
                        composite[dstIdx + 1] = static_cast<uint8_t>(overlay.data[srcIdx + 1] * alpha + composite[dstIdx + 1] * invAlpha);
                        composite[dstIdx + 2] = static_cast<uint8_t>(overlay.data[srcIdx + 2] * alpha + composite[dstIdx + 2] * invAlpha);
                        composite[dstIdx + 3] = std::max(composite[dstIdx + 3], srcA);
                    }
                }
            }
        }
    }
}

// Legacy 2x wrapper
static void blitOverlayScaled2x(std::vector<uint8_t>& composite, int compW, int compH,
                                 const pipeline::BLPImage& overlay, int dstX, int dstY) {
    blitOverlayScaledN(composite, compW, compH, overlay, dstX, dstY, 2);
}

// Nearest-neighbor downscale blit: sample every Nth pixel from overlay
static void blitOverlayDownscaleN(std::vector<uint8_t>& composite, int compW, int compH,
                                   const pipeline::BLPImage& overlay, int dstX, int dstY, int scale) {
    if (scale < 2) { blitOverlay(composite, compW, compH, overlay, dstX, dstY); return; }
    int outW = overlay.width / scale;
    int outH = overlay.height / scale;
    for (int oy = 0; oy < outH; oy++) {
        int dy = dstY + oy;
        if (dy < 0 || dy >= compH) continue;
        for (int ox = 0; ox < outW; ox++) {
            int dx = dstX + ox;
            if (dx < 0 || dx >= compW) continue;

            int sx = ox * scale;
            int sy = oy * scale;
            size_t srcIdx = (static_cast<size_t>(sy) * overlay.width + sx) * 4;
            size_t dstIdx = (static_cast<size_t>(dy) * compW + dx) * 4;

            uint8_t srcA = overlay.data[srcIdx + 3];
            if (srcA == 0) continue;

            if (srcA == 255) {
                composite[dstIdx + 0] = overlay.data[srcIdx + 0];
                composite[dstIdx + 1] = overlay.data[srcIdx + 1];
                composite[dstIdx + 2] = overlay.data[srcIdx + 2];
                composite[dstIdx + 3] = 255;
            } else {
                float alpha = srcA / 255.0f;
                float invAlpha = 1.0f - alpha;
                composite[dstIdx + 0] = static_cast<uint8_t>(overlay.data[srcIdx + 0] * alpha + composite[dstIdx + 0] * invAlpha);
                composite[dstIdx + 1] = static_cast<uint8_t>(overlay.data[srcIdx + 1] * alpha + composite[dstIdx + 1] * invAlpha);
                composite[dstIdx + 2] = static_cast<uint8_t>(overlay.data[srcIdx + 2] * alpha + composite[dstIdx + 2] * invAlpha);
                composite[dstIdx + 3] = std::max(composite[dstIdx + 3], srcA);
            }
        }
    }
}

namespace {

/// Where a character-texture overlay belongs on the body atlas, in the
/// coordinates of the 256x256 reference atlas everything is expressed in.
///
/// One table, consulted twice: once to work out how big the canvas has to be
/// for the art being laid on it, and once to place each layer. It was written
/// out inline at the placement site alone, and then the size question could
/// only be answered by guessing.
struct AtlasRegion256 { int x, y, w, h; bool known; };

AtlasRegion256 regionFor(const std::string& pathLower) {
    // FacialUpperHair/FacialLowerHair (CharSections section 2) paint the same
    // two face regions as the face itself, on top of it.
    // Scalps (CharSections hair tex 2/3, ScalpUpperHairNN_CC) paint the same
    // two head regions, after the facial hair (stock 0x4F0AD0 / 0x4F0B70).
    if (pathLower.find("faceupper") != std::string::npos ||
        pathLower.find("scalpupper") != std::string::npos ||
        pathLower.find("facialupper") != std::string::npos) return {  .x = 0, .y = 160, .w = 128, .h = 32, .known = true};
    if (pathLower.find("facelower") != std::string::npos ||
        pathLower.find("scalplower") != std::string::npos ||
        pathLower.find("faciallower") != std::string::npos) return {  .x = 0, .y = 192, .w = 128, .h = 64, .known = true};
    if (pathLower.find("pelvis")    != std::string::npos) return {.x = 128,  .y = 96, .w = 128, .h = 64, .known = true};
    if (pathLower.find("torso")     != std::string::npos) return {.x = 128,   .y = 0, .w = 128, .h = 64, .known = true};
    if (pathLower.find("armupper")  != std::string::npos) return {  .x = 0,   .y = 0, .w = 128, .h = 64, .known = true};
    if (pathLower.find("armlower")  != std::string::npos) return {  .x = 0,  .y = 64, .w = 128, .h = 64, .known = true};
    if (pathLower.find("hand")      != std::string::npos) return {  .x = 0, .y = 128, .w = 128, .h = 32, .known = true};
    if (pathLower.find("foot")      != std::string::npos ||
        pathLower.find("feet")      != std::string::npos) return {.x = 128, .y = 224, .w = 128, .h = 32, .known = true};
    if (pathLower.find("legupper")  != std::string::npos ||
        pathLower.find("leg")       != std::string::npos) return {.x = 128, .y = 160, .w = 128, .h = 64, .known = true};
    return {.x = 0, .y = 0, .w = 0, .h = 0, .known = false};
}

std::string lowerPath(const std::string& s) {
    std::string out = s;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/// The atlas scale a layer of this size implies for its region. A face authored
/// at 512x256 belongs in a 128x64 region, so it is asking for a 1024 atlas.
int impliedScale(const AtlasRegion256& region, int overlayWidth) {
    if (!region.known || region.w <= 0 || overlayWidth <= 0) return 1;
    int scale = overlayWidth / region.w;
    return scale < 1 ? 1 : scale;
}

}  // namespace

VkTexture* CharacterRenderer::compositeTextures(const std::vector<std::string>& layerPaths) {
    if (layerPaths.empty() || !assetManager || !assetManager->isInitialized()) {
        return whiteTexture_.get();
    }

    // Composite key is deterministic from layer set; if we've already built it,
    // reuse the existing GPU texture to keep live instance pointers valid.
    std::string cacheKey = "__composite__";
    for (const auto& lp : layerPaths) { cacheKey += '|'; cacheKey += lp; }
    auto cachedComposite = textureCache.find(cacheKey);
    if (cachedComposite != textureCache.end()) {
        cachedComposite->second.lastUse = ++textureCacheCounter_;
        return cachedComposite->second.texture.get();
    }

    // Load base layer
    pipeline::BLPImage base;
    if (predecodedBLPCache_) {
        std::string key = layerPaths[0];
        std::replace(key.begin(), key.end(), '/', '\\');
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto pit = predecodedBLPCache_->find(key);
        if (pit != predecodedBLPCache_->end()) {
            base = std::move(pit->second);
            predecodedBLPCache_->erase(pit);
        }
    }
    if (!base.isValid()) base = assetManager->loadTexture(layerPaths[0]);
    if (!base.isValid()) {
        core::Logger::getInstance().warning("Composite: failed to load base layer: ", layerPaths[0]);
        return whiteTexture_.get();
    }
    applyMagentaKeyIfNeeded(base, layerPaths[0]);

    // Copy base pixel data as our working buffer
    std::vector<uint8_t> composite = base.data;
    int width = base.width;
    int height = base.height;

    core::Logger::getInstance().info("Composite: base layer ", width, "x", height, " from ", layerPaths[0]);

    // WoW character texture atlas regions (from WoW Model Viewer / CharComponentTextureSections)
    // Coordinates at 256x256 base resolution:
    // Region          X    Y    W    H
    // Base            0    0    256  256
    // Arm Upper       0    0    128  64
    // Arm Lower       0    64   128  64
    // Hand            0    128  128  32
    // Face Upper      0    160  128  32
    // Face Lower      0    192  128  64
    // Torso Upper     128  0    128  64
    // Torso Lower     128  64   128  32
    // Pelvis Upper    128  96   128  64
    // Pelvis Lower    128  160  128  64
    // Foot            128  224  128  32

    // Scale factor: base texture may be larger than the 256x256 reference atlas
    int coordScale = width / 256;
    if (coordScale < 1) coordScale = 1;

    // Load every overlay before deciding how big the canvas is.
    //
    // The body decided the atlas size on its own, and each overlay was then
    // squeezed into whatever region that gave it. An HD art set ships a body at
    // the size the stock one uses and a face at twice it - so a 512x256 face was
    // resampled down to 256x128 to fit a 512 body, which throws away exactly the
    // detail the art exists for and lands it soft next to a crisp body.
    //
    // The canvas is sized to the most demanding layer instead. Nothing changes
    // for art that agrees with its body, which is every set that shipped with
    // the game; a set that asks for more gets a bigger atlas and is placed at
    // its own resolution.
    struct LoadedOverlay { std::string path; pipeline::BLPImage image; };
    std::vector<LoadedOverlay> overlays;
    overlays.reserve(layerPaths.size());
    int requiredScale = coordScale;
    int largestRegionScale = 1;
    bool sawRegionLayer = false;
    for (size_t layer = 1; layer < layerPaths.size(); layer++) {
        if (layerPaths[layer].empty()) continue;
        pipeline::BLPImage overlay;
        if (predecodedBLPCache_) {
            std::string key = layerPaths[layer];
            std::replace(key.begin(), key.end(), '/', '\\');
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            auto pit = predecodedBLPCache_->find(key);
            if (pit != predecodedBLPCache_->end()) {
                overlay = std::move(pit->second);
                predecodedBLPCache_->erase(pit);
            }
        }
        if (!overlay.isValid()) overlay = assetManager->loadTexture(layerPaths[layer]);
        if (!overlay.isValid()) {
            core::Logger::getInstance().warning("Composite: FAILED to load overlay: ",
                                                layerPaths[layer]);
            continue;
        }
        applyMagentaKeyIfNeeded(overlay, layerPaths[layer]);
        // A full-atlas layer speaks for itself and is not a region at all.
        if (overlay.width != width || overlay.height != height) {
            const AtlasRegion256 region = regionFor(lowerPath(layerPaths[layer]));
            if (region.known) {
                const int want = impliedScale(region, overlay.width);
                if (want > largestRegionScale) largestRegionScale = want;
                sawRegionLayer = true;
            }
        }
        overlays.push_back({.path = layerPaths[layer], .image = std::move(overlay)});
    }

    // Grow to the most demanding layer.
    //
    // A layer that does not fit its region is not merely higher resolution. A
    // draenei's HD faceLower is a front-facing head where the stock one is a
    // side profile, and a tauren's is a muzzle seen head on where the stock one
    // is the side of a head - different pictures, not larger ones, and shrinking
    // them into a region sized for the stock art puts the wrong thing on the
    // face. At 512x256 they are exactly their region on a 1024 atlas, which is
    // the size their scale is asking for.
    //
    // The cost is the layers that do fit: a body, a pelvis and a torso authored
    // for a 512 atlas get upscaled to sit beside them. That is softer, and
    // softer in the right place beats sharp in the wrong one. It applies only
    // to the four race and sex pairs that ship such a layer; every other set
    // agrees with its body and is untouched.
    if (sawRegionLayer && largestRegionScale > coordScale) {
        requiredScale = largestRegionScale;
    }

    if (requiredScale > coordScale) {
        const int newSize = 256 * requiredScale;
        std::vector<uint8_t> grown(static_cast<size_t>(newSize) * newSize * 4);
        const int factor = newSize / width;
        for (int y = 0; y < newSize; y++) {
            const int srcY = y / factor;
            for (int x = 0; x < newSize; x++) {
                const int srcIdx = (srcY * width + x / factor) * 4;
                const int dstIdx = (y * newSize + x) * 4;
                grown[dstIdx + 0] = composite[srcIdx + 0];
                grown[dstIdx + 1] = composite[srcIdx + 1];
                grown[dstIdx + 2] = composite[srcIdx + 2];
                grown[dstIdx + 3] = composite[srcIdx + 3];
            }
        }
        core::Logger::getInstance().info("Composite: body is ", width, "x", height,
                                         " but its art asks for ", newSize, "x", newSize,
                                         " - growing the atlas to keep the detail");
        composite = std::move(grown);
        width = height = newSize;
        coordScale = requiredScale;
    }

    // Alpha-blend each overlay onto the composite
    for (auto& loaded : overlays) {
        const pipeline::BLPImage& overlay = loaded.image;

        core::Logger::getInstance().info("Composite: overlay ", loaded.path,
            " (", overlay.width, "x", overlay.height, ")");

        if (overlay.width == width && overlay.height == height) {
            // Same size: full alpha-blend
            blitOverlay(composite, width, height, overlay, 0, 0);
        } else {
            // Where this layer belongs, from the one table that also sized the
            // canvas above.
            const AtlasRegion256 region = regionFor(lowerPath(loaded.path));
            if (!region.known) {
                // Unknown -- center placement as fallback
                const int cx = (width - overlay.width) / 2;
                const int cy = (height - overlay.height) / 2;
                core::Logger::getInstance().info("Composite: UNKNOWN region for '",
                    loaded.path, "', centering at (", cx, ",", cy, ")");
                blitOverlay(composite, width, height, overlay, cx, cy);
                continue;
            }
            int dstX = region.x, dstY = region.y;
            const int expectedW256 = region.w, expectedH256 = region.h;

            // Scale coordinates from 256-base to actual canvas
            dstX *= coordScale;
            dstY *= coordScale;

            // The region is a fixed fraction of the atlas, so the overlay has to
            // land at exactly this size whatever resolution it was authored at.
            // Growing it by a whole factor was not enough, because it only ever
            // grew: an overlay arriving larger than its region was pasted at its
            // own size, spilling across neighbouring regions and dragging every
            // feature on the head to the wrong scale. These assets ship at two
            // resolutions - a 256-wide face belongs in a 128-wide slot on a
            // 256-wide atlas - so the two can meet whenever a lookup resolves
            // the body and the face from different sets.
            const int expectedW = expectedW256 * coordScale;
            const int expectedH = expectedH256 * coordScale;
            const bool needsResample =
                (overlay.width != expectedW || overlay.height != expectedH);

            if (needsResample) {
                // Resampling here means this overlay was authored for a different
                // atlas size than the body it is going onto - the two came from
                // different art sets. It will be placed correctly, but a quarter
                // resolution face stretched over an HD head is soft and muddy
                // next to a crisp body, and that reads as the face not fitting.
                core::Logger::getInstance().warning(
                    "Composite: '", loaded.path, "' is ", overlay.width, "x",
                    overlay.height, " but its region on this ", width, "x", height,
                    " body is ", expectedW, "x", expectedH,
                    " - mismatched art sets; resampling to fit");
            } else {
                core::Logger::getInstance().info("Composite: placing '", loaded.path,
                    "' (", overlay.width, "x", overlay.height,
                    ") at (", dstX, ",", dstY, ") on ", width, "x", height);
            }

            if (needsResample) {
                blitOverlayResampled(composite, width, height, overlay,
                                     dstX, dstY, expectedW, expectedH);
            } else {
                blitOverlay(composite, width, height, overlay, dstX, dstY);
            }
        }
    }

    bleedAndStripMagentaKey(composite, width, height);
    const bool hasAlpha = hasNonOpaqueAlpha(composite);

    // Upload composite to GPU via VkTexture
    auto tex = std::make_unique<VkTexture>();
    tex->upload(*vkCtx_, composite.data(), width, height, VK_FORMAT_R8G8B8A8_UNORM, true);
    tex->createSampler(vkCtx_->getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                       VK_SAMPLER_ADDRESS_MODE_REPEAT);

    VkTexture* texPtr = tex.get();

    // Store in texture cache with deterministic key.
    // Keep the first allocation for a key to avoid invalidating raw pointers
    // held by active render instances.
    TextureCacheEntry e;
    e.texture = std::move(tex);
    e.approxBytes = approxTextureBytesWithMips(width, height);
    e.lastUse = ++textureCacheCounter_;
    e.hasAlpha = hasAlpha;
    e.colorKeyBlack = false;
    texturePropsByPtr_[texPtr] = {.hasAlpha = hasAlpha, .colorKeyBlack = false};
    // No derived normal map for a composited body, and this is why: the
    // derivation reads luminance as height, which holds for stone and bark and
    // does not hold for skin. Every freckle, every painted shadow under a
    // collarbone, becomes a ridge - and on a character that reads as stretch
    // marks. Art authored as a surface gets one; art authored as a person does
    // not.
    // Checked before emplacing, not after. emplace builds its node from the
    // arguments and then destroys that node if the key is already present --
    // and ~VkTexture frees nothing, so the texture goes in, comes back out
    // destroyed-but-not-freed, and there is no longer a pointer to free it by.
    if (auto existing = textureCache.find(cacheKey); existing != textureCache.end()) {
        e.texture->destroy(vkCtx_->getDevice(), vkCtx_->getAllocator());
        existing->second.lastUse = ++textureCacheCounter_;
        return existing->second.texture.get();
    }
    textureCache.emplace(cacheKey, std::move(e));

    core::Logger::getInstance().info("Composite texture created: ", width, "x", height, " from ", layerPaths.size(), " layers");
    return texPtr;
}

void CharacterRenderer::clearCompositeCache() {
    // Just clear the lookup map so next compositeWithRegions() creates fresh textures.
    // Don't delete GPU textures -- they may still be referenced by models or instances.
    // Orphaned textures will be cleaned up when their model/instance is destroyed.
    compositeCache_.clear();
}

VkTexture* CharacterRenderer::compositeWithRegions(const std::string& basePath,
                                                const std::vector<std::string>& baseLayers,
                                                const std::vector<std::pair<int, std::string>>& regionLayers) {
    // Build cache key from all inputs to avoid redundant compositing
    std::string cacheKey = basePath;
    for (const auto& bl : baseLayers) { cacheKey += '|'; cacheKey += bl; }
    cacheKey += '#';
    for (const auto& rl : regionLayers) {
        cacheKey += std::to_string(rl.first);
        cacheKey += ':';
        cacheKey += rl.second;
        cacheKey += ',';
    }
    auto cacheIt = compositeCache_.find(cacheKey);
    if (cacheIt != compositeCache_.end() && cacheIt->second != nullptr) {
        return cacheIt->second;
    }

    // If the lookup map was cleared, recover from the texture cache without
    // regenerating/replacing the underlying GPU texture.
    std::string storageKey = "__compositeRegions__" + cacheKey;
    auto cachedComposite = textureCache.find(storageKey);
    if (cachedComposite != textureCache.end()) {
        cachedComposite->second.lastUse = ++textureCacheCounter_;
        VkTexture* texPtr = cachedComposite->second.texture.get();
        compositeCache_[cacheKey] = texPtr;
        return texPtr;
    }

    // Region index -> pixel coordinates on the 256x256 base atlas
    // These are scaled up by (width/256, height/256) for larger textures (512x512, 1024x1024)
    static constexpr int regionCoords256[][2] = {
        {   0,   0 },  // 0 = ArmUpper
        {   0,  64 },  // 1 = ArmLower
        {   0, 128 },  // 2 = Hand
        { 128,   0 },  // 3 = TorsoUpper
        { 128,  64 },  // 4 = TorsoLower
        { 128,  96 },  // 5 = LegUpper
        { 128, 160 },  // 6 = LegLower
        { 128, 224 },  // 7 = Foot
    };

    // First, build base skin + underwear using existing compositeTextures
    std::vector<std::string> layers;
    layers.push_back(basePath);
    for (const auto& ul : baseLayers) {
        layers.push_back(ul);
    }
    // Load base composite into CPU buffer
    if (!assetManager || !assetManager->isInitialized()) {
        return whiteTexture_.get();
    }

    pipeline::BLPImage base;
    if (predecodedBLPCache_) {
        std::string key = basePath;
        std::replace(key.begin(), key.end(), '/', '\\');
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto pit = predecodedBLPCache_->find(key);
        if (pit != predecodedBLPCache_->end()) {
            base = std::move(pit->second);
            predecodedBLPCache_->erase(pit);
        }
    }
    if (!base.isValid()) base = assetManager->loadTexture(basePath);
    if (!base.isValid()) {
        return whiteTexture_.get();
    }
    applyMagentaKeyIfNeeded(base, basePath);

    std::vector<uint8_t> composite;
    int width = base.width;
    int height = base.height;

    // No atlas growth on this path, and the reason is the equipment.
    //
    // Growing it here scales the region coordinates but not the art placed into
    // them: the equipment layers are authored for a 512 atlas, so on a 1024 one
    // they land at quarter size in the right corner of the right region, and a
    // fully armoured NPC comes out naked with fragments of armour scattered
    // over it. That is what growing this path did, and it is worse than what it
    // was meant to fix.
    //
    // The face it was meant to fix is a dwarf's, and a dwarf's HD faceLower is
    // a faithful 2x of the stock one - same parts in the same places - so
    // resampling it down gives back the stock picture. Softer, and right.
    // compositeTextures still grows, because it has no equipment to misplace.

    // If base texture is 256x256 (e.g., baked NPC texture), upscale to 512x512
    // so equipment regions can be composited at correct coordinates
    if (width == kBaseTexSize && height == kBaseTexSize && !regionLayers.empty()) {
        width = kUpscaleTexSize;
        height = kUpscaleTexSize;
        composite.resize(width * height * 4);
        // Simple 2x nearest-neighbor upscale
        for (int y = 0; y < kUpscaleTexSize; y++) {
            for (int x = 0; x < kUpscaleTexSize; x++) {
                int srcX = x / 2;
                int srcY = y / 2;
                int srcIdx = (srcY * kBaseTexSize + srcX) * 4;
                int dstIdx = (y * kUpscaleTexSize + x) * 4;
                composite[dstIdx + 0] = base.data[srcIdx + 0];
                composite[dstIdx + 1] = base.data[srcIdx + 1];
                composite[dstIdx + 2] = base.data[srcIdx + 2];
                composite[dstIdx + 3] = base.data[srcIdx + 3];
            }
        }
        core::Logger::getInstance().debug("compositeWithRegions: upscaled 256x256 to 512x512");
    } else {
        composite = base.data;
    }

    // Blend face, facial hair and underwear overlays, each into its region at
    // the region's size on this canvas.
    const int coordScale = std::max(1, width / 256);
    for (const auto& ul : baseLayers) {
        if (ul.empty()) continue;
        pipeline::BLPImage overlay;
        if (predecodedBLPCache_) {
            std::string key = ul;
            std::replace(key.begin(), key.end(), '/', '\\');
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            auto pit = predecodedBLPCache_->find(key);
            if (pit != predecodedBLPCache_->end()) {
                overlay = std::move(pit->second);
                predecodedBLPCache_->erase(pit);
            }
        }
        if (!overlay.isValid()) overlay = assetManager->loadTexture(ul);
        if (!overlay.isValid()) continue;
        applyMagentaKeyIfNeeded(overlay, ul);

        if (overlay.width == width && overlay.height == height) {
            blitOverlay(composite, width, height, overlay, 0, 0);
        } else {
            // The same region table compositeTextures uses. This path kept its
            // own copy, which knew no facial hair: FacialUpperHair (a human's
            // eyebrows) matched nothing and was pasted in the middle of the
            // atlas, so the brows vanished as soon as anything was worn.
            //
            // And the layer lands at its region's size on this canvas, whatever
            // it was authored at. Pasting at its own size put 128x64 legacy art
            // (every facial-hair texture, the death knight faces) into a
            // quarter of its region on a 512 skin.
            const AtlasRegion256 region = regionFor(lowerPath(ul));
            if (!region.known) {
                blitOverlay(composite, width, height, overlay,
                            (width - overlay.width) / 2, (height - overlay.height) / 2);
                continue;
            }
            const int dstX = region.x * coordScale;
            const int dstY = region.y * coordScale;
            const int dstW = region.w * coordScale;
            const int dstH = region.h * coordScale;
            if (overlay.width == dstW && overlay.height == dstH) {
                blitOverlay(composite, width, height, overlay, dstX, dstY);
            } else {
                blitOverlayResampled(composite, width, height, overlay, dstX, dstY, dstW, dstH);
            }
        }
    }

    // Expected region sizes on the 256x256 base atlas (scaled like coords)
    static constexpr int regionSizes256[][2] = {
        { 128,  64 },  // 0 = ArmUpper
        { 128,  64 },  // 1 = ArmLower
        { 128,  32 },  // 2 = Hand
        { 128,  64 },  // 3 = TorsoUpper
        { 128,  32 },  // 4 = TorsoLower
        { 128,  64 },  // 5 = LegUpper
        { 128,  64 },  // 6 = LegLower
        { 128,  32 },  // 7 = Foot
    };

    // Scale factor from 256-base to actual texture size
    int scaleX = width / 256;
    int scaleY = height / 256;
    if (scaleX < 1) scaleX = 1;
    if (scaleY < 1) scaleY = 1;

    // Now blit equipment region textures at explicit coordinates
    for (const auto& rl : regionLayers) {
        int regionIdx = rl.first;
        if (regionIdx < 0 || regionIdx >= 8) continue;

        pipeline::BLPImage overlay;
        if (predecodedBLPCache_) {
            std::string key = rl.second;
            std::replace(key.begin(), key.end(), '/', '\\');
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            auto pit = predecodedBLPCache_->find(key);
            if (pit != predecodedBLPCache_->end()) {
                overlay = std::move(pit->second);
                predecodedBLPCache_->erase(pit);
            }
        }
        if (!overlay.isValid()) overlay = assetManager->loadTexture(rl.second);
        if (!overlay.isValid()) {
            core::Logger::getInstance().warning("compositeWithRegions: failed to load ", rl.second);
            continue;
        }
        applyMagentaKeyIfNeeded(overlay, rl.second);

        int dstX = regionCoords256[regionIdx][0] * scaleX;
        int dstY = regionCoords256[regionIdx][1] * scaleY;

        // Expected full-resolution size for this region at current atlas scale
        int expectedW = regionSizes256[regionIdx][0] * scaleX;
        int expectedH = regionSizes256[regionIdx][1] * scaleY;
        if (overlay.width == expectedW && overlay.height == expectedH) {
            // Exact match - blit 1:1
            blitOverlay(composite, width, height, overlay, dstX, dstY);
        } else if (overlay.width * 2 == expectedW && overlay.height * 2 == expectedH) {
            // Overlay is half size - upscale 2x
            blitOverlayScaled2x(composite, width, height, overlay, dstX, dstY);
        } else if (overlay.width > expectedW && overlay.height > expectedH &&
                   expectedW > 0 && expectedH > 0) {
            // Overlay is larger than region (e.g. HD textures for 1024 atlas on 512 canvas)
            // Downscale to fit
            int dsX = overlay.width / expectedW;
            int dsY = overlay.height / expectedH;
            int ds = std::min(dsX, dsY);
            if (ds >= 2) {
                blitOverlayDownscaleN(composite, width, height, overlay, dstX, dstY, ds);
            } else {
                blitOverlay(composite, width, height, overlay, dstX, dstY);
            }
        } else {
            // Size mismatch - blit at natural size (may clip or leave gap)
            core::Logger::getInstance().warning("compositeWithRegions: region ", regionIdx,
                " at (", dstX, ",", dstY, ") overlay=", overlay.width, "x", overlay.height,
                " expected=", expectedW, "x", expectedH, " from ", rl.second);
            blitOverlay(composite, width, height, overlay, dstX, dstY);
        }
    }

    bleedAndStripMagentaKey(composite, width, height);
    const bool hasAlpha = hasNonOpaqueAlpha(composite);

    // Upload to GPU via VkTexture
    auto tex = std::make_unique<VkTexture>();
    tex->upload(*vkCtx_, composite.data(), width, height, VK_FORMAT_R8G8B8A8_UNORM, true);
    tex->createSampler(vkCtx_->getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                       VK_SAMPLER_ADDRESS_MODE_REPEAT);

    VkTexture* texPtr = tex.get();

    // Store in texture cache.
    // Use emplace to avoid replacing an existing texture for this key; replacing
    // would invalidate pointers currently bound to active instances.
    TextureCacheEntry entry;
    entry.texture = std::move(tex);
    entry.approxBytes = approxTextureBytesWithMips(width, height);
    entry.lastUse = ++textureCacheCounter_;
    entry.hasAlpha = hasAlpha;
    entry.colorKeyBlack = false;
    texturePropsByPtr_[texPtr] = {.hasAlpha = hasAlpha, .colorKeyBlack = false};
    // Skin again, with armour composited onto it. Same reason as above.
    // Checked before emplacing: see the note in compositeTexture. Testing
    // ins.second afterwards is too late -- by then the texture has been moved
    // into a node that emplace already destroyed, leaking it with no handle
    // left to free.
    if (auto existing = textureCache.find(storageKey); existing != textureCache.end()) {
        entry.texture->destroy(vkCtx_->getDevice(), vkCtx_->getAllocator());
        existing->second.lastUse = ++textureCacheCounter_;
        compositeCache_[cacheKey] = existing->second.texture.get();
        return existing->second.texture.get();
    }
    textureCache.emplace(storageKey, std::move(entry));

    core::Logger::getInstance().debug("compositeWithRegions: created ", width, "x", height,
        " texture with ", regionLayers.size(), " equipment regions");
    compositeCache_[cacheKey] = texPtr;
    return texPtr;
}

void CharacterRenderer::setModelTexture(uint32_t modelId, uint32_t textureSlot, VkTexture* texture) {
    auto it = models.find(modelId);
    if (it == models.end()) {
        core::Logger::getInstance().warning("setModelTexture: model ", modelId, " not found");
        return;
    }

    auto& gpuModel = it->second;
    if (textureSlot >= gpuModel.textureIds.size()) {
        core::Logger::getInstance().warning("setModelTexture: slot ", textureSlot, " out of range (", gpuModel.textureIds.size(), " textures)");
        return;
    }

    gpuModel.textureIds[textureSlot] = texture;
    core::Logger::getInstance().debug("Replaced model ", modelId, " texture slot ", textureSlot, " with composited texture");
}
bool CharacterRenderer::loadModel(const pipeline::M2Model& model, uint32_t id) {
    if (!model.isValid()) {
        core::Logger::getInstance().error("Cannot load invalid M2 model");
        return false;
    }

    auto existingIt = models.find(id);
    if (existingIt != models.end()) {
        core::Logger::getInstance().warning("Model ID ", id, " already loaded, replacing");
        destroyModelGPU(existingIt->second, /*defer=*/true);
        models.erase(existingIt);
    }

    M2ModelGPU gpuModel;
    gpuModel.data = model;
    if (!model.vertices.empty()) {
        gpuModel.visualBoundMin = glm::vec3(std::numeric_limits<float>::max());
        gpuModel.visualBoundMax = glm::vec3(-std::numeric_limits<float>::max());
        for (const auto& vertex : model.vertices) {
            gpuModel.visualBoundMin = glm::min(gpuModel.visualBoundMin, vertex.position);
            gpuModel.visualBoundMax = glm::max(gpuModel.visualBoundMax, vertex.position);
        }
        gpuModel.visualBoundRadius =
            glm::length(gpuModel.visualBoundMax - gpuModel.visualBoundMin) * 0.5f;
    }
    const auto classification = classifyM2Model(
        model.name, model.boundMin, model.boundMax,
        model.vertices.size(), model.particleEmitters.size());
    gpuModel.isSkyBird = classification.isSkyBird;
    gpuModel.hasEmitters = !model.particleEmitters.empty() || !model.ribbonEmitters.empty();
    {
        const auto keyed = [](const pipeline::M2AnimationTrack& t) {
            for (const auto& seq : t.sequences)
                if (seq.timestamps.size() > 1) return true;
            return false;
        };
        for (const auto& bone : model.bones) {
            if (keyed(bone.rotation) || keyed(bone.translation) || keyed(bone.scale)) {
                gpuModel.bonesMove = true;
                break;
            }
        }
    }

    // Every model this renderer takes on, named once.
    //
    // The same line M2Renderer::loadModel carries, and it has to be in both:
    // creatures and players are drawn here, not there, so a diagnostic that
    // only named M2Renderer's models reported four hundred doodads and not one
    // creature. Three rounds of the Elemental Slave's white sheets were spent
    // reading M2Renderer's particle, glow and bone code before it was noticed
    // that the creature never goes through any of it.
    //
    // The emitter count is worth having here for its own reason: this renderer
    // reads it to classify and then draws none of them, so a creature whose
    // effects are missing entirely says so on this line.
    // Behind the same switch as M2Renderer's: an inventory rather than a
    // fault, and 41 more lines on top of that one's 197.
    static const bool kLoadDiag = core::envFlagEnabled("WOWEE_M2_LOAD_DIAG", false);
    if (kLoadDiag) {
        static core::LogBudget characterLoadBudget(300, "character models named at load");
        if (characterLoadBudget.take()) {
            LOG_WARNING("Character M2 load: '",
                        model.name.empty() ? "<unnamed>" : model.name,
                        "' id=", id,
                        " verts=", model.vertices.size(),
                        " emitters=", model.particleEmitters.size(),
                        " (not drawn)",
                        " ribbons=", model.ribbonEmitters.size());
        }
    }

    // Batch all GPU uploads (VB, IB, textures) into a single command buffer
    // submission with one fence wait, instead of one fence wait per upload.
    vkCtx_->beginUploadBatch();

    // Setup GPU buffers
    setupModelBuffers(gpuModel);

    // Calculate bind pose
    calculateBindPose(gpuModel);

    // Load textures from model.
    //
    // Only type 0 names its own file. Every other type is a slot the client
    // fills: the body and hair from CharSections, the monster skins from
    // CreatureDisplayInfo, the tabard from the guild. Whatever name a model
    // carries on one of those is leftover from whoever built it, and loading
    // it is loading the wrong thing.
    //
    // It reads as nothing on the shipped data, where those names are empty -
    // of 946 creature models and 38 character models, not one names a
    // replaceable slot, and the only three among 8205 item models are
    // Blizzard's own build machine ("Z:\World of Warcraft Proj Server\...")
    // and could never have resolved. It is imported models that carry them:
    // 103 of 784 from a Legion installation, naming Legion paths this
    // installation does not have. The boar's mane asked for "maehne" and drew
    // untextured when nothing answered, because CreatureDisplayInfo has no
    // second skin for it to be overwritten with.
    for (const auto& tex : model.textures) {
        VkTexture* texPtr = tex.type == 0 ? loadTexture(tex.filename) : nullptr;
        gpuModel.textureIds.push_back(texPtr);
    }

    vkCtx_->endUploadBatch();

    // Precompute batch render order (priorityPlane, materialLayer). The result
    // depends only on the model, so caching it here removes the per-frame
    // per-instance allocate + sort from render().
    gpuModel.batchEffects = m2fx::resolveBatchEffects(gpuModel.data);
    gpuModel.sortedBatchIndices.resize(gpuModel.data.batches.size());
    std::iota(gpuModel.sortedBatchIndices.begin(), gpuModel.sortedBatchIndices.end(), 0);
    std::stable_sort(gpuModel.sortedBatchIndices.begin(), gpuModel.sortedBatchIndices.end(),
        [&batches = gpuModel.data.batches](size_t a, size_t b) {
            const auto& ba = batches[a];
            const auto& bb = batches[b];
            if (ba.priorityPlane != bb.priorityPlane)
                return ba.priorityPlane < bb.priorityPlane;
            return ba.materialLayer < bb.materialLayer;
        });

    {
        std::string lowerName = gpuModel.data.name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        gpuModel.isKoboldFlame =
            (lowerName.find("kobold") != std::string::npos) &&
            ((lowerName.find("candle") != std::string::npos) ||
             (lowerName.find("torch") != std::string::npos) ||
             (lowerName.find("mine") != std::string::npos));
    }

    for (const auto& tex : gpuModel.data.textures) {
        if (tex.type == 1 || tex.type == 2 || tex.type == 6 || tex.type == 8) {
            gpuModel.hasCompositeSlots = true;
            break;
        }
    }

    models[id] = std::move(gpuModel);
    if (springLevel_ > 0) analyzeSpringChains(models[id]);

    core::Logger::getInstance().debug("Loaded M2 model ", id, " (", model.vertices.size(),
                       " verts, ", model.bones.size(), " bones, ", model.sequences.size(),
                       " anims, ", model.textures.size(), " textures)");

    return true;
}

void CharacterRenderer::setupModelBuffers(M2ModelGPU& gpuModel) {
    auto& model = gpuModel.data;

    if (model.vertices.empty() || model.indices.empty()) return;

    const size_t vertCount = model.vertices.size();
    const size_t idxCount = model.indices.size();

    // Build expanded GPU vertex buffer with tangents (Lengyel's method)
    std::vector<CharVertexGPU> gpuVerts(vertCount);
    std::vector<glm::vec3> tanAccum(vertCount, glm::vec3(0.0f));
    std::vector<glm::vec3> bitanAccum(vertCount, glm::vec3(0.0f));

    // Copy base vertex data
    size_t numBones = model.bones.size();
    int outOfRangeCount = 0, nonzeroWeightOOR = 0;
    for (size_t i = 0; i < vertCount; i++) {
        const auto& src = model.vertices[i];
        auto& dst = gpuVerts[i];
        dst.position = src.position;
        std::memcpy(dst.boneWeights, src.boneWeights, 4);
        std::memcpy(dst.boneIndices, src.boneIndices, 4);
        dst.normal = src.normal;
        dst.texCoords = src.texCoords[0];
        dst.texCoords2 = src.texCoords[1];
        dst.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f); // default

        // Diagnostic: check bone indices
        for (int j = 0; j < 4; j++) {
            uint8_t bi = src.boneIndices[j];
            uint8_t bw = src.boneWeights[j];
            if (bi >= numBones) {
                outOfRangeCount++;
                if (bw > 0) nonzeroWeightOOR++;
            }
        }
    }
    // A bone index past the end of the bone list, which is a broken model or a
    // misread one.
    //
    // An index at or above 128 is not that: it is what any model with more than
    // 128 bones has, and the character models have 219. Warning on it fired on
    // every character in the world and reported outOfRange=0 every time - a
    // line per model saying nothing was wrong, in a log whose whole value is
    // that what is in it is.
    if (outOfRangeCount > 0) {
        LOG_WARNING("Model has bone indices past its bone list: bones=", numBones,
                    " verts=", vertCount, " outOfRange=", outOfRangeCount,
                    " (nonzeroWeight=", nonzeroWeightOOR, ")",
                    " - those vertices skin to nothing and collapse to the origin");
    }

    // Accumulate tangent/bitangent per triangle
    for (size_t i = 0; i + 2 < idxCount; i += 3) {
        uint16_t i0 = model.indices[i], i1 = model.indices[i+1], i2 = model.indices[i+2];
        if (i0 >= vertCount || i1 >= vertCount || i2 >= vertCount) continue;

        const glm::vec3& p0 = gpuVerts[i0].position;
        const glm::vec3& p1 = gpuVerts[i1].position;
        const glm::vec3& p2 = gpuVerts[i2].position;
        const glm::vec2& uv0 = gpuVerts[i0].texCoords;
        const glm::vec2& uv1 = gpuVerts[i1].texCoords;
        const glm::vec2& uv2 = gpuVerts[i2].texCoords;

        glm::vec3 edge1 = p1 - p0;
        glm::vec3 edge2 = p2 - p0;
        glm::vec2 duv1 = uv1 - uv0;
        glm::vec2 duv2 = uv2 - uv0;

        float det = duv1.x * duv2.y - duv2.x * duv1.y;
        if (std::abs(det) < 1e-8f) continue;
        float invDet = 1.0f / det;

        glm::vec3 t = (edge1 * duv2.y - edge2 * duv1.y) * invDet;
        glm::vec3 b = (edge2 * duv1.x - edge1 * duv2.x) * invDet;

        tanAccum[i0] += t; tanAccum[i1] += t; tanAccum[i2] += t;
        bitanAccum[i0] += b; bitanAccum[i1] += b; bitanAccum[i2] += b;
    }

    // Orthogonalize and compute handedness
    for (size_t i = 0; i < vertCount; i++) {
        const glm::vec3& n = gpuVerts[i].normal;
        const glm::vec3& t = tanAccum[i];
        if (glm::dot(t, t) < 1e-8f) {
            gpuVerts[i].tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            continue;
        }
        // Gram-Schmidt orthogonalize
        glm::vec3 tOrtho = glm::normalize(t - n * glm::dot(n, t));
        float w = (glm::dot(glm::cross(n, t), bitanAccum[i]) < 0.0f) ? -1.0f : 1.0f;
        gpuVerts[i].tangent = glm::vec4(tOrtho, w);
    }

    // Upload vertex buffer (CharVertexGPU, 56 bytes per vertex)
    auto vb = uploadBuffer(*vkCtx_,
        gpuVerts.data(),
        gpuVerts.size() * sizeof(CharVertexGPU),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    gpuModel.vertexBuffer = vb.buffer;
    gpuModel.vertexAlloc = vb.allocation;
    gpuModel.vertexCount = static_cast<uint32_t>(vertCount);

    // Upload index buffer
    auto ib = uploadBuffer(*vkCtx_,
        model.indices.data(),
        idxCount * sizeof(uint16_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    gpuModel.indexBuffer = ib.buffer;
    gpuModel.indexAlloc = ib.allocation;
    gpuModel.indexCount = static_cast<uint32_t>(idxCount);
}

void CharacterRenderer::calculateBindPose(M2ModelGPU& gpuModel) {
    auto& bones = gpuModel.data.bones;
    size_t numBones = bones.size();
    gpuModel.bindPose.resize(numBones);

    // Compute full hierarchical rest pose, then invert.
    // Each bone's rest position is T(pivot), composed with its parent chain.
    std::vector<glm::mat4> restPose(numBones);
    for (size_t i = 0; i < numBones; i++) {
        glm::mat4 local = glm::translate(glm::mat4(1.0f), bones[i].pivot);
        if (bones[i].parentBone >= 0 && static_cast<size_t>(bones[i].parentBone) < numBones) {
            restPose[i] = restPose[bones[i].parentBone] * local;
        } else {
            restPose[i] = local;
        }
        gpuModel.bindPose[i] = glm::inverse(restPose[i]);
    }
}

uint32_t CharacterRenderer::createInstance(uint32_t modelId, const glm::vec3& position,
                                           const glm::vec3& rotation, float scale) {
    auto modelIt = models.find(modelId);
    if (modelIt == models.end()) {
        core::Logger::getInstance().error("Cannot create instance: model ", modelId, " not loaded");
        return 0;
    }

    CharacterInstance instance;
    instance.id = nextInstanceId++;
    instance.modelId = modelId;
    instance.position = position;
    instance.rotation = rotation;
    instance.scale = scale;

    // Initialize bone matrices to identity
    auto& gpuRef = modelIt->second;
    instance.boneMatrices.resize(std::max(static_cast<size_t>(1), gpuRef.data.bones.size()), glm::mat4(1.0f));
    instance.cachedModel = &gpuRef;

    uint32_t id = instance.id;
    instances[id] = std::move(instance);
    ++instancesGeneration_;
    return id;
}

void CharacterRenderer::playAnimation(uint32_t instanceId, uint32_t animationId, bool loop,
                                      uint32_t oneShotReturnAnim) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) {
        core::Logger::getInstance().warning("Cannot play animation: instance ", instanceId, " not found");
        return;
    }

    auto& instance = it->second;
    // A dead unit stays down. Idle, Ready, swings, flinches, emote clears,
    // cast ends and dismounts all reach here from their own paths while the
    // unit lies dead; one gate here instead of a Death check in each of them.
    if (instance.deathLocked && animationId != anim::DEATH && animationId != anim::DEAD &&
        animationId != anim::DECAY) {
        return;
    }
    auto& model = models[instance.modelId].data;
    instance.oneShotReturnAnim = loop ? 0 : oneShotReturnAnim;

    // Track death state for preventing movement while dead
    if (animationId == 1) {
        instance.isDead = true;
    } else if (instance.isDead && animationId == 0) {
        instance.isDead = false;  // Respawned
    }

    // What was playing, for the phase carry and the cross-fade below.
    const int oldSeq = instance.currentSequenceIndex;
    const float oldTime = instance.animationTime;
    const float oldSpeed = instance.animationFrozen ? 0.0f : instance.animationSpeed;
    const bool oldLoop = instance.animationLoop;
    const uint8_t oldMode = instance.playMode;
    const bool oldValid = oldSeq >= 0 && oldSeq < static_cast<int>(model.sequences.size());

    // The model's sequence for the request (CM2Model 0x826350): the id, else
    // AnimationData's Fallback chain - with the play mode its Flags give
    // (0x10 backwards, 0x20 held) - else Stand, Closed, the first sequence.
    // The instance keeps reporting the id that was asked for, as the stock
    // client does (bs+0x90): a fallback is how the model plays it.
    auto has = [&](uint32_t id) {
        for (const auto& q : model.sequences)
            if (q.id == id) return true;
        return false;
    };
    const anim::FallbackResult fr = anim::resolveFallback(
        has, animationId, model.sequences.empty() ? 0u : model.sequences[0].id);
    if (!fr.found && !model.sequences.empty()) {
        // Only log missing animation once per model (reduce spam)
        static std::unordered_map<uint32_t, std::unordered_set<uint32_t>> loggedMissingAnims;
        if (loggedMissingAnims[instance.modelId].insert(animationId).second) {
            LOG_WARNING("Animation ", animationId, " not found in model ", instance.modelId,
                        ", playing ", fr.id);
        }
    }
    instance.currentAnimationId = fr.found ? animationId : fr.id;
    instance.playMode = static_cast<uint8_t>(fr.mode);
    instance.animationTime = 0.0f;
    instance.animationLoop = loop;
    instance.animationSpeed = 1.0f;

    // Every play rolls a variation by frequency, one-shots as well as loops
    // (CGUnit_C::PlayAnimation 0x7385C0 passes variation -1; HumanMale's
    // Attack1H has two swings, Attack2HL three). Stand and the Ready stances
    // hold their first variation while the unit attacks or casts (0x737EF0).
    instance.currentSequenceIndex = pickLoopVariation(instanceId, model, fr.id);
    if (instance.currentSequenceIndex < 0 && !model.sequences.empty()) instance.currentSequenceIndex = 0;
    // An alias (flag 0x40) has no keyframes; it plays the sequence it names.
    instance.currentSequenceIndex = anim::resolveSequenceAlias(model.sequences, instance.currentSequenceIndex);

    // A looping play rolls again at every lap (update()). A one-shot whose
    // sequence stops at its end (flag 0x01) plays its replay range of laps
    // first (0x826B00); any other one-shot ends after its first lap, where
    // the stock client's end-of-lap callback re-picks.
    instance.variationLapsLeft = 0;
    const bool newValid = instance.currentSequenceIndex >= 0 &&
                          instance.currentSequenceIndex < static_cast<int>(model.sequences.size());
    if (newValid && !loop) {
        const auto& q = model.sequences[instance.currentSequenceIndex];
        if (q.flags & anim::kSeqFlagNonLooping)
            instance.variationLapsLeft = anim::stockReplayLaps(q, anim::stockRand()) - 1;
    }

    // A moving animation that replaces another moving one starts at the same
    // fraction of its cycle (0x7385C0), so the feet do not jump when a walk
    // becomes a run or a run a sprint.
    if (newValid && oldValid && loop && oldSeq != instance.currentSequenceIndex &&
        instance.playMode == 0 && anim::isMovingAnim(fr.id)) {
        const auto& o = model.sequences[oldSeq];
        const auto& n = model.sequences[instance.currentSequenceIndex];
        if (o.movingSpeed != 0.0f && n.movingSpeed != 0.0f) {
            instance.animationTime = anim::carriedPhaseMs(oldTime, static_cast<float>(o.duration),
                                                          static_cast<float>(n.duration));
        }
    }

    // Cross-fade out of whatever the bones were doing, over the incoming
    // sequence's blendTime (150 ms on nearly every 3.3.5 character and
    // creature sequence, 500 for HumanMale's Stand), uncapped; 0 cuts. The
    // outgoing sequence keeps playing while it fades. Needs a pose for this
    // skeleton: a fresh instance has nothing to fade from, and nor does one
    // the view cull has left unposed (poseStale) - nobody sees that switch.
    if (!instance.poseStale && newValid && !model.bones.empty() &&
        instance.localPose.size() == model.bones.size()) {
        // The outgoing sequence is the one that was playing, in the state it
        // had before this play reset the clock.
        beginSequenceBlend(instance, model, instance.currentSequenceIndex,
                           oldValid ? oldSeq : -1, oldTime, oldSpeed, oldLoop, oldMode);
    } else {
        instance.blendDurationMs = 0.0f;
        instance.blendFromSeq = -1;
    }
}

void CharacterRenderer::beginSequenceBlend(CharacterInstance& inst, const pipeline::M2Model& model,
                                           int newSeq, int fromSeq, float fromTime, float fromSpeed,
                                           bool fromLoop, uint8_t fromMode) {
    if (newSeq < 0 || newSeq >= static_cast<int>(model.sequences.size())) return;
    uint32_t blendMs = model.sequences[newSeq].blendTime;
    // E10: another unit's switch between two locomotion sequences fades over
    // at least 150 ms, where the sequence would cut or nearly cut.
    if (remoteSmoothing_ && blendMs < remote_smoothing::kLocomotionBlendFloorMs &&
        fromSeq >= 0 && fromSeq < static_cast<int>(model.sequences.size()) && isRemoteUnit(inst)) {
        const uint32_t fromId = model.sequences[fromSeq].id;
        const uint32_t toId = model.sequences[newSeq].id;
        blendMs = remote_smoothing::blendMs(
            blendMs, true, remote_smoothing::smoothableLocomotion(fromId, anim::isMovingAnim(fromId)),
            remote_smoothing::smoothableLocomotion(toId, anim::isMovingAnim(toId)));
    }
    // A fade still more than half on the outgoing side keeps its outgoing
    // sequence and its clock; only the incoming one changes (0x826C40).
    if (inst.blendDurationMs > 0.0f && inst.blendElapsedMs < inst.blendDurationMs) {
        const float incoming = m2_track::sequenceBlendWeight(inst.blendElapsedMs / inst.blendDurationMs);
        if (1.0f - incoming > 0.5f) return;
    }
    if (blendMs == 0) {
        inst.blendDurationMs = 0.0f;
        inst.blendFromSeq = -1;
        return;
    }
    if (fromSeq >= 0 && fromSeq < static_cast<int>(model.sequences.size())) {
        inst.blendFromSeq = fromSeq;
        inst.blendFromTime = fromTime;
        inst.blendFromSpeed = fromSpeed;
        inst.blendFromLoop = fromLoop;
        inst.blendFromMode = fromMode;
    } else {
        // Nothing playing that can be sampled: fade from the pose itself.
        inst.blendFromSeq = -1;
        inst.blendFromPose = inst.localPose;
    }
    inst.blendElapsedMs = 0.0f;
    inst.blendDurationMs = static_cast<float>(blendMs);
}

void CharacterRenderer::playDeath(uint32_t instanceId, bool alreadyDead) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    auto& inst = it->second;
    const bool wasDown = inst.deathLocked &&
        (inst.currentAnimationId == anim::DEATH || inst.currentAnimationId == anim::DEAD ||
         inst.currentAnimationId == anim::DECAY);
    inst.deathLocked = true;
    inst.isDead = true;
    // Already lying there: a second report of the same death (health and the
    // dynamic flags both say so, or the unit is created again in view) must
    // not stand it up to fall again.
    if (wasDown) return;

    if (alreadyDead && hasAnimation(instanceId, anim::DEATH) && hasAnimation(instanceId, anim::DECAY)) {
        // A corpse with Decay coming into view: already decayed, Decay's
        // last frame (see the death hold in update).
        playAnimation(instanceId, anim::DECAY, /*loop=*/false);
        const auto mIt = models.find(inst.modelId);
        if (mIt != models.end() && inst.currentSequenceIndex >= 0 &&
            inst.currentSequenceIndex < static_cast<int>(mIt->second.data.sequences.size())) {
            inst.animationTime = static_cast<float>(
                mIt->second.data.sequences[inst.currentSequenceIndex].duration);
        }
        inst.blendDurationMs = 0.0f;
    } else if (hasAnimation(instanceId, anim::DEATH)) {
        playAnimation(instanceId, anim::DEATH, /*loop=*/false);
        if (alreadyDead) {
            // A corpse coming into view is already down: the last frame of
            // Death, with no fall and no fade from whatever pose it had.
            const auto mIt = models.find(inst.modelId);
            if (mIt != models.end() && inst.currentSequenceIndex >= 0 &&
                inst.currentSequenceIndex < static_cast<int>(mIt->second.data.sequences.size())) {
                inst.animationTime = static_cast<float>(
                    mIt->second.data.sequences[inst.currentSequenceIndex].duration);
            }
            inst.blendDurationMs = 0.0f;
        }
    } else if (hasAnimation(instanceId, anim::DEAD)) {
        playAnimation(instanceId, anim::DEAD, /*loop=*/true);
        if (alreadyDead) inst.blendDurationMs = 0.0f;
    } else {
        // Neither: whatever playAnimation's fallback makes of Death, held.
        playAnimation(instanceId, anim::DEATH, /*loop=*/false);
    }
    // A death seen happening (not a corpse coming into view): the unit's
    // death cry, which the stock client plays as the Death animation starts.
    if (!alreadyDead && animSoundEventCallback_)
        animSoundEventCallback_(instanceId, anim::DEATH, kAnimEventDeathStart, 0, inst.position);
}

uint32_t CharacterRenderer::upperBodyAnimId(uint32_t instanceId) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return 0;
    const auto& inst = it->second;
    if (inst.upperTime < 0.0f || !inst.cachedModel) return 0;
    const int seq = inst.upperSequenceIndex[2];
    const auto& seqs = inst.cachedModel->data.sequences;
    if (seq < 0 || seq >= static_cast<int>(seqs.size())) return 0;
    return seqs[seq].id;
}

bool CharacterRenderer::upperBodyAnimHasEvent(uint32_t instanceId, uint32_t eventTag) const {
    auto it = instances.find(instanceId);
    if (it == instances.end() || !it->second.cachedModel) return false;
    const auto& inst = it->second;
    const int upSeq = inst.upperSequenceIndex[2];
    const auto& events = inst.cachedModel->data.soundEvents;
    if (inst.upperTime < 0.0f || upSeq < 0 || upSeq >= static_cast<int>(events.size())) return false;
    for (const auto& ev : events[upSeq])
        if (ev.tag == eventTag) return true;
    return false;
}

void CharacterRenderer::setAnimationSpeed(uint32_t instanceId, float speed) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    it->second.animationSpeed = std::clamp(speed, 0.05f, 4.0f);
}

int CharacterRenderer::pickLoopVariation(uint32_t instanceId, const pipeline::M2Model& model,
                                         uint32_t animationId) const {
    // Stand and the Ready stances (BehaviorID 0 and 25-29) are the ones the
    // stock client pins to their first variation (0x737EF0).
    const bool stance = animationId == anim::STAND ||
                        (animationId >= anim::READY_UNARMED && animationId <= anim::READY_BOW);
    if (stance && firstVariationPredicate_ && anim::hasSequenceVariations(model.sequences, animationId) &&
        firstVariationPredicate_(instanceId)) {
        for (size_t i = 0; i < model.sequences.size(); i++) {
            const int idx = static_cast<int>(i);
            if (model.sequences[i].id == animationId && model.sequences[i].variationIndex == 0 &&
                anim::sequencePlayable(model.sequences, model.bones, idx))
                return idx;
        }
    }
    return anim::rollVariationStock(model.sequences, model.bones, animationId, anim::stockRand());
}

bool CharacterRenderer::currentAnimHasEvent(uint32_t instanceId, uint32_t eventTag) const {
    auto it = instances.find(instanceId);
    if (it == instances.end() || !it->second.cachedModel) return false;
    const auto& inst = it->second;
    const auto& events = inst.cachedModel->data.soundEvents;
    if (inst.currentSequenceIndex < 0 || inst.currentSequenceIndex >= static_cast<int>(events.size()))
        return false;
    for (const auto& ev : events[inst.currentSequenceIndex])
        if (ev.tag == eventTag) return true;
    return false;
}

void CharacterRenderer::clearDeath(uint32_t instanceId) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    it->second.deathLocked = false;
    it->second.isDead = false;
}

bool CharacterRenderer::isDeathLocked(uint32_t instanceId) const {
    auto it = instances.find(instanceId);
    return it != instances.end() && it->second.deathLocked;
}

bool CharacterRenderer::isDeathAnimationDone(uint32_t instanceId) const {
    auto it = instances.find(instanceId);
    if (it == instances.end() || !it->second.deathLocked) return false;
    const auto& inst = it->second;
    if (inst.currentAnimationId != anim::DEATH) return true;
    const auto mIt = models.find(inst.modelId);
    if (mIt == models.end() || inst.currentSequenceIndex < 0 ||
        inst.currentSequenceIndex >= static_cast<int>(mIt->second.data.sequences.size())) {
        return true;
    }
    const float duration =
        static_cast<float>(mIt->second.data.sequences[inst.currentSequenceIndex].duration);
    return inst.animationTime >= duration;
}

void CharacterRenderer::setAnimCullView(const glm::mat4& viewProjection,
                                        const glm::vec3& shadowRayDir) {
    animCullViewProj_ = viewProjection;
    animCullShadowDir_ = shadowRayDir;
    animCullValid_ = true;
}

void CharacterRenderer::update(float deltaTime, const glm::vec3& cameraPos) {
    lastCameraPos_ = cameraPos;
    // The springs' clock (E7): each pose steps by the time since its last.
    if (springLevel_ > 0) springClock_ += std::max(deltaTime, 0.0f);
    // Stock eases the unit ambient multiplier as cur += (target - cur) * dt,
    // snapping within 0.01 (0x783429..0x7834DF).
    {
        const float d = unitAmbientTarget_ - unitAmbientMul_;
        unitAmbientMul_ = (std::abs(d) < 0.01f) ? unitAmbientTarget_
                                                : unitAmbientMul_ + d * std::min(1.0f, deltaTime);
    }
    // Distance culling for animation updates in dense areas. The default is the
    // render radius (WOWEE_CHAR_RENDER_RADIUS, 130): at 120 a unit drawn 120-130
    // yards out stood frozen mid-stride, its clock stopped with its bones.
    const float animUpdateRadius = static_cast<float>(envSizeOrDefault("WOWEE_CHAR_ANIM_RADIUS", 130));
    const float animUpdateRadiusSq = animUpdateRadius * animUpdateRadius;
    // Creature birds are rendered by this path rather than M2Renderer.  Their
    // rapid wing motion remains conspicuous at distance, and the render radius
    // can be larger than the generic animation radius.  Use the same boundary
    // as rendering so a visible bird never holds its last bone pose.
    const float birdUpdateRadius = static_cast<float>(envSizeOrDefault("WOWEE_CHAR_RENDER_RADIUS", 130));
    const float birdUpdateRadiusSq = birdUpdateRadius * birdUpdateRadius;

    // View cull for skeletons (setAnimCullView). In a city most of the units
    // in range stand behind or beside the camera, and each skeleton is a
    // hundred-odd bones sampled three tracks apiece. Tested with the main
    // pass's own culling sphere, grown to take in the unit's shadow and a
    // margin for the camera turning before the frame is drawn.
    static const bool animCullEnabled = core::envFlagEnabled("WOWEE_CHAR_ANIM_CULL", true);
    static const float animCullMargin = static_cast<float>(
        envSizeOrDefault("WOWEE_CHAR_ANIM_CULL_MARGIN", 2));
    const bool viewCull = animCullEnabled && animCullValid_;
    Frustum animFrustum;
    if (viewCull) animFrustum.extractFromMatrix(animCullViewProj_);
    animCullValid_ = false;  // one frame's view; a caller that stops setting it stops culling
    // Same default as render(): bounds-less models and anything smaller.
    constexpr float kDefaultCharacterCullRadius = 4.0f;
    constexpr float kMaxShadowRun = 40.0f;

    // Single pass: fade-in, movement, and animation bone collection
    toUpdate_.clear();

    for (auto& pair : instances) {
        auto& inst = pair.second;

        // Update fade-in opacity
        if (inst.fadeInDuration > 0.0f && inst.opacity < 1.0f) {
            inst.fadeInTime += deltaTime;
            inst.opacity = std::min(1.0f, inst.fadeInTime / inst.fadeInDuration);
            if (inst.opacity >= 1.0f) {
                inst.fadeInDuration = 0.0f;
            }
        }

        // Interpolate creature movement
        if (inst.isMoving) {
            inst.moveElapsed += deltaTime;
            float t = inst.moveElapsed / inst.moveDuration;
            if (t >= 1.0f) {
                inst.position = inst.moveEnd;
                inst.isMoving = false;
            } else {
                inst.position = glm::mix(inst.moveStart, inst.moveEnd, t);
            }
        }

        // E10: another unit's position steps and facing snaps become offsets
        // that decay (remote_smoothing.hpp); getModelMatrix draws them.
        if (remoteSmoothing_) {
            if (isRemoteUnit(inst) && !inst.hasOverrideModelMatrix && inst.mountedOn == 0) {
                if (inst.smoothReported)
                    remote_smoothing::stepReported(inst.smooth, inst.position, inst.rotation.z,
                                                   deltaTime, inst.pendingCorrection);
                else
                    remote_smoothing::step(inst.smooth, inst.position, inst.rotation.z, deltaTime);
                inst.smoothActive = true;
            } else if (inst.smoothActive) {
                inst.smooth = remote_smoothing::State{};
                inst.smoothActive = false;
            }
        }
        inst.pendingCorrection = glm::vec3(0.0f);

        // Skip weapon instances for animation - their transforms are set by parent
        // bones. Enchant visuals are the exception: they are pure animated FX.
        // So is an item model that moves or emits (an epic weapon's glow, a
        // helm's flames): stock animates every attached CM2Model on its own
        // clock, and its emitters follow its bones and tracks.
        const bool animatesItself = inst.isAttachedItem && inst.cachedModel &&
                                    (inst.cachedModel->hasEmitters || inst.cachedModel->bonesMove);
        // A rider or a passenger also has its matrix set by its seat, and
        // plays its own animation all the same (Mount, a ride loop).
        if (inst.hasOverrideModelMatrix && !inst.isEffectModel && !animatesItself &&
            inst.mountedOn == 0) continue;

        float distSq = glm::distance2(inst.position, cameraPos);
        const bool isSkyBird = inst.cachedModel && inst.cachedModel->isSkyBird;
        const float updateRadiusSq = isSkyBird ? birdUpdateRadiusSq : animUpdateRadiusSq;
        if (distSq > updateRadiusSq && !inst.isSceneModel) continue;

        // Advance global sequence timer (accumulates independently of animation wrapping)
        inst.globalSequenceTime += deltaTime * 1000.0f;

        // Upper-body overlay clock; it ends by itself. The torso part's
        // events fire as the whole body's do below: a swing or a flinch on
        // the upper body over a run reaches its $CPP and $CAH, and its blow
        // is heard, instead of waiting out the fallback timers.
        if (inst.upperTime >= 0.0f) {
            const float prevUpper = inst.upperTime;
            inst.upperTime += deltaTime * 1000.0f;
            const int upSeq = inst.upperSequenceIndex[2];
            if (animSoundEventCallback_ && inst.cachedModel && upSeq >= 0 &&
                upSeq < static_cast<int>(inst.cachedModel->data.soundEvents.size())) {
                const auto& seqs = inst.cachedModel->data.sequences;
                const uint32_t upAnim = seqs[upSeq].id;
                const float upEnd = static_cast<float>(seqs[upSeq].duration);
                for (const auto& ev : inst.cachedModel->data.soundEvents[upSeq]) {
                    const float t = static_cast<float>(ev.timeMs);
                    // A one-shot: [prev, now), and its last frame once.
                    if (t >= prevUpper && (t < inst.upperTime || (inst.upperTime >= upEnd && t <= upEnd)))
                        animSoundEventCallback_(pair.first, upAnim, ev.tag, ev.soundEntryId, inst.position);
                }
            }
            if (inst.upperTime >= inst.upperDurationMs) inst.upperTime = -1.0f;
        }

        // Sequence cross-fade clock. Before the sequence clock below, which may
        // end a one-shot and start a fresh fade from zero. The outgoing
        // sequence runs on with it: looping, or held at its end.
        if (inst.blendDurationMs > 0.0f) {
            inst.blendElapsedMs += deltaTime * 1000.0f;
            if (inst.blendElapsedMs >= inst.blendDurationMs) {
                inst.blendDurationMs = 0.0f;
                inst.blendFromSeq = -1;
            } else if (inst.blendFromSeq >= 0 && inst.cachedModel &&
                       inst.blendFromSeq < static_cast<int>(inst.cachedModel->data.sequences.size())) {
                const float fromDur =
                    static_cast<float>(inst.cachedModel->data.sequences[inst.blendFromSeq].duration);
                inst.blendFromTime += deltaTime * 1000.0f * inst.blendFromSpeed;
                if (fromDur > 0.0f && inst.blendFromTime >= fromDur) {
                    inst.blendFromTime = inst.blendFromLoop ? std::fmod(inst.blendFromTime, fromDur) : fromDur;
                }
            }
        }

        // Always advance animation time (cheap)
        if (inst.cachedModel && !inst.cachedModel->data.sequences.empty()) {
            if (inst.currentSequenceIndex < 0) {
                inst.currentSequenceIndex = 0;
                inst.currentAnimationId = inst.cachedModel->data.sequences[0].id;
            }
            const auto& seq = inst.cachedModel->data.sequences[inst.currentSequenceIndex];
            const float prevAnimTime = inst.animationTime;
            inst.animationTime += inst.animationFrozen ? 0.0f : deltaTime * 1000.0f * inst.animationSpeed;

            // Sound events ($CSD, $CSS, $CAH, $AHn, $FDn, $WNG) and $FSD
            // footfalls whose timestamps the sequence just passed:
            // [prev, now), split at the loop point, and a one-shot's events
            // up to and including its last frame. Fired before the wrap below,
            // which may switch the instance to its resume animation. Every
            // unit's, so every unit in view steps, fidgets and swings aloud.
            const auto& soundEvents = inst.cachedModel->data.soundEvents;
            const auto& footfalls = inst.cachedModel->data.footstepEventTimes;
            const int seqIdx = inst.currentSequenceIndex;
            const bool haveSoundEvents = seqIdx < static_cast<int>(soundEvents.size()) &&
                                         !soundEvents[seqIdx].empty();
            const bool haveFootfalls = seqIdx < static_cast<int>(footfalls.size()) &&
                                       !footfalls[seqIdx].empty();
            // Only a sequence played forward fires its events: a fallback
            // played backwards or held (anim::PlayMode) is a pose, not a take.
            if (animSoundEventCallback_ && !inst.isAttachedItem && inst.playMode == 0 &&
                (haveSoundEvents || haveFootfalls)) {
                const float dur = static_cast<float>(seq.duration);
                const float now = inst.animationTime;
                static constexpr uint32_t kFootfallTag = pipeline::m2EventTag("$FSD");
                auto fireRange = [&](float from, float to, bool inclusiveEnd) {
                    auto passed = [&](uint32_t timeMs) {
                        const float t = static_cast<float>(timeMs);
                        return t >= from && (t < to || (inclusiveEnd && t <= to));
                    };
                    if (haveSoundEvents) {
                        for (const auto& ev : soundEvents[seqIdx]) {
                            if (passed(ev.timeMs))
                                animSoundEventCallback_(pair.first, inst.currentAnimationId, ev.tag,
                                                        ev.soundEntryId, inst.position);
                        }
                    }
                    if (haveFootfalls) {
                        for (uint32_t timeMs : footfalls[seqIdx]) {
                            if (passed(timeMs))
                                animSoundEventCallback_(pair.first, inst.currentAnimationId, kFootfallTag,
                                                        0, inst.position);
                        }
                    }
                };
                if (seq.duration == 0 || now < dur) {
                    fireRange(prevAnimTime, now, false);
                } else if (inst.animationLoop) {
                    fireRange(prevAnimTime, dur, false);
                    fireRange(0.0f, std::min(now - dur, dur), false);
                } else if (prevAnimTime < dur) {
                    fireRange(prevAnimTime, dur, true);
                }
            }

            if (seq.duration > 0 && inst.animationTime >= static_cast<float>(seq.duration)) {
                if (inst.animationLoop) {
                    // Subtract duration instead of fmod to preserve float precision
                    // fmod() loses precision with large animationTime values
                    inst.animationTime -= static_cast<float>(seq.duration);
                    // Clamp to [0, duration) to handle multiple loops in one frame
                    while (inst.animationTime >= static_cast<float>(seq.duration)) {
                        inst.animationTime -= static_cast<float>(seq.duration);
                    }
                    // Next lap: a new variation rolled by frequency (the Stand
                    // fidgets), faded in over its blendTime like any other
                    // switch. Every lap of a looping sequence rolls (0x831FC0:
                    // its end fires each lap); the replay range counts only
                    // for a sequence that stops at its end (flag 0x01).
                    {
                        const auto& seqs = inst.cachedModel->data.sequences;
                        const auto& bones = inst.cachedModel->data.bones;
                        const uint32_t playingId = seq.id;
                        if (anim::hasSequenceVariations(seqs, playingId)) {
                            int next = pickLoopVariation(pair.first, inst.cachedModel->data, playingId);
                            if (next >= 0) next = anim::resolveSequenceAlias(seqs, next);
                            if (next >= 0 && next < static_cast<int>(seqs.size()) &&
                                next != inst.currentSequenceIndex) {
                                // Out of view the fade is skipped, as in playAnimation.
                                if (!inst.poseStale && !bones.empty() &&
                                    inst.localPose.size() == bones.size()) {
                                    beginSequenceBlend(inst, inst.cachedModel->data, next,
                                                       inst.currentSequenceIndex, inst.animationTime,
                                                       inst.animationFrozen ? 0.0f : inst.animationSpeed,
                                                       true, inst.playMode);
                                }
                                inst.currentSequenceIndex = next;
                                const float nextDur = static_cast<float>(seqs[next].duration);
                                if (nextDur > 0.0f) {
                                    while (inst.animationTime >= nextDur) inst.animationTime -= nextDur;
                                } else {
                                    inst.animationTime = 0.0f;
                                }
                            }
                        }
                    }
                } else if (inst.variationLapsLeft > 0) {
                    // A flag-0x01 one-shot with laps of its replay range to go.
                    --inst.variationLapsLeft;
                    while (inst.animationTime >= static_cast<float>(seq.duration))
                        inst.animationTime -= static_cast<float>(seq.duration);
                } else {
                    // One-shot animation finished: return to the caller-specified
                    // resume anim (NPC state emote) or Stand, unless dead
                    if (inst.currentAnimationId != anim::DEATH && !inst.deathLocked) {
                        const uint32_t resumeAnim =
                            inst.oneShotReturnAnim != 0 ? inst.oneShotReturnAnim : anim::STAND;
                        playAnimation(pair.first, resumeAnim, true);
                    } else if (inst.deathLocked && inst.currentAnimationId == anim::DEATH &&
                               hasAnimation(pair.first, anim::DECAY)) {
                        // Down: a corpse model with Decay plays it once and
                        // holds its last frame (Wow.exe 0x744870, the Death
                        // animation's end callback; the sand vortex and the
                        // vapor are the creatures that have one).
                        playAnimation(pair.first, anim::DECAY, /*loop=*/false);
                    } else {
                        // Stay on last frame of death (or of Decay)
                        inst.animationTime = static_cast<float>(seq.duration);
                    }
                }
            }
        }

        // Keep combat-range creatures at the render frame rate. Aggressive
        // throttling used to begin at 10 yards and reached every eighth frame
        // at 40 yards, making ordinary monster locomotion look like a slideshow.
        // Reserve frame skipping for models far enough away that bone detail is
        // much less noticeable, consistent with the generic M2 renderer.
        //
        // Every frame out to 80 yards, every other frame beyond. The old 45/90
        // yard steps (every 2nd, then every 4th frame) gave units across a
        // ordinary pull or town square 15 and 7.5 poses a second at 30 fps,
        // which read as choppy; the stock client poses everything in view
        // every frame.
        uint32_t boneInterval = 1;
        if (!isSkyBird && distSq > 80.0f * 80.0f) boneInterval = 2;

        // Out of view, shadow and all: the clocks above ran, the skeleton
        // waits. A unit with no pose yet, a scene backdrop and an attached
        // enchant effect (drawn wherever its weapon is) are always posed.
        if (viewCull && !inst.isSceneModel && !inst.hasOverrideModelMatrix &&
            !inst.boneMatrices.empty() && inst.cachedModel) {
            const float modelR = inst.cachedModel->data.boundRadius;
            const float cullRadius = modelR > 0.01f
                ? std::max(kDefaultCharacterCullRadius, modelR * std::max(0.001f, inst.scale))
                : kDefaultCharacterCullRadius;
            const float height = 2.0f * std::max(0.0f, modelR * inst.scale);
            const AnimCullSphere s = animCullSphere(inst.position, cullRadius, height,
                                                    animCullShadowDir_, kMaxShadowRun);
            if (!animFrustum.intersectsSphere(s.center, s.radius + animCullMargin)) {
                inst.poseStale = true;
                continue;
            }
        }

        inst.boneUpdateCounter++;
        // A unit coming back into view is posed this frame, whatever its interval.
        bool needsBones = (inst.boneUpdateCounter >= boneInterval) || inst.boneMatrices.empty() ||
                          inst.poseStale;
        if (needsBones) {
            inst.boneUpdateCounter = 0;
            toUpdate_.push_back(std::ref(inst));
        }
    }

    const size_t updatedCount = toUpdate_.size();

    // Thread bone matrix computation in chunks
    if (updatedCount >= 8 && numAnimThreads_ > 1) {
        static const size_t minAnimWorkPerThread = std::max<size_t>(
            8, envSizeOrDefault("WOWEE_CHAR_ANIM_WORK_PER_THREAD", 16));
        const size_t maxUsefulThreads = std::max<size_t>(
            1, (updatedCount + minAnimWorkPerThread - 1) / minAnimWorkPerThread);
        const size_t numThreads = std::min(static_cast<size_t>(numAnimThreads_), maxUsefulThreads);

        if (numThreads <= 1) {
            for (auto& instRef : toUpdate_) {
                calculateBoneMatrices(instRef.get());
            }
        } else {
            const size_t chunkSize = updatedCount / numThreads;
            const size_t remainder = updatedCount % numThreads;

            auto processRange = [this](size_t begin, size_t end) {
                for (size_t i = begin; i < end; i++) {
                    calculateBoneMatrices(toUpdate_[i].get());
                }
            };

            animFutures_.clear();
            if (animFutures_.capacity() < numThreads) {
                animFutures_.reserve(numThreads);
            }

            // Dispatch all but the last chunk to the shared pool; process the
            // last chunk on this thread so the pool is never a hard dependency
            // for finishing this frame's bone work.
            size_t start = 0;
            for (size_t t = 0; t + 1 < numThreads; t++) {
                size_t end = start + chunkSize + (t < remainder ? 1 : 0);
                animFutures_.push_back(core::ThreadPool::frameWorkers().submit(
                    [processRange, start, end]() { processRange(start, end); }));
                start = end;
            }
            processRange(start, updatedCount);

            for (auto& f : animFutures_) {
                f.get();
            }
        }
    } else {
        for (auto& instRef : toUpdate_) {
            calculateBoneMatrices(instRef.get());
        }
    }

    // Riders on their mount's attachment 0 (stock 0x73D5D0), before the
    // weapons, which hang off the rider's matrix: mount world x seat bone x
    // the point's position x 1/mount scale, so the rider keeps its own size.
    for (auto& pair : instances) {
        auto& rider = pair.second;
        if (rider.mountedOn == 0) continue;
        auto mIt = instances.find(rider.mountedOn);
        uint16_t seatBone = 0;
        glm::vec3 seatPos(0.0f);
        const bool onBase = rider.seatAttachment < 0;
        if (mIt == instances.end() ||
            (!onBase && !findAttachmentStrict(mIt->second.modelId,
                                              static_cast<uint32_t>(rider.seatAttachment),
                                              seatBone, seatPos))) {
            if (mIt == instances.end()) rider.mountedOn = 0;
            rider.hasOverrideModelMatrix = false;
            continue;
        }
        const auto& mount = mIt->second;
        const glm::mat4 mountMat = mount.hasOverrideModelMatrix ? mount.overrideModelMatrix
                                                                : getModelMatrix(mount);
        const glm::mat4 seat = (!onBase && seatBone < mount.boneMatrices.size())
                                   ? mount.boneMatrices[seatBone] : glm::mat4(1.0f);
        const float k = rider.scale / std::max(0.001f, mount.scale);
        glm::mat4 m = mountMat * seat * glm::translate(glm::mat4(1.0f), seatPos + rider.seatOffset);
        if (rider.seatYaw != 0.0f)
            m = m * glm::rotate(glm::mat4(1.0f), -rider.seatYaw, glm::vec3(0.0f, 0.0f, 1.0f));
        m = m * glm::scale(glm::mat4(1.0f), glm::vec3(k));
        if (rider.seatPassengerAttachment >= 0) {
            uint16_t pb = 0;
            glm::vec3 pp(0.0f);
            if (findAttachmentStrict(rider.modelId, static_cast<uint32_t>(rider.seatPassengerAttachment), pb, pp))
                m = m * glm::translate(glm::mat4(1.0f), -pp);
        }
        rider.seatPoint = glm::vec3(m[3]);
        rider.seatPointValid = true;
        if (rider.seatArcBlend < 1.0f || rider.seatArcLift != 0.0f) {
            const glm::vec3 at = glm::mix(rider.seatArcFrom, rider.seatPoint, rider.seatArcBlend) +
                                 glm::vec3(0.0f, 0.0f, rider.seatArcLift);
            m[3] = glm::vec4(at, 1.0f);
        }
        rider.overrideModelMatrix = m;
        rider.hasOverrideModelMatrix = true;
    }

    // Update weapon attachment transforms (after all bone matrices are computed)
    for (auto& pair : instances) {
        auto& instance = pair.second;
        if (instance.weaponAttachments.empty()) continue;
        if (glm::distance2(instance.position, cameraPos) > animUpdateRadiusSq) continue;

        glm::mat4 charModelMat = instance.hasOverrideModelMatrix
            ? instance.overrideModelMatrix
            : getModelMatrix(instance);

        for (auto& wa : instance.weaponAttachments) {
            auto weapIt = instances.find(wa.weaponInstanceId);
            if (weapIt == instances.end()) continue;

            // Get the bone matrix for the attachment bone
            glm::mat4 boneMat(1.0f);
            if (wa.boneIndex < instance.boneMatrices.size()) {
                boneMat = instance.boneMatrices[wa.boneIndex];
            }

            // Weapon model matrix = character model * bone transform * attachment
            // offset * item/sheath orientation.
            glm::mat4 weaponMat =
                charModelMat * boneMat * glm::translate(glm::mat4(1.0f), wa.offset) *
                wa.localTransform;

            // Back-sheathed weapons: the swinging left arm passes through the
            // canted blade while running. Sample the whole arm - shoulder,
            // elbow, hand attachment points plus segment midpoints - against
            // the weapon model's AABB and ease the blade outward, away from
            // the spine, so the arm pushes it instead of clipping. A single
            // sphere at the elbow joint missed forearm/upper-arm contact.
            constexpr uint32_t kAttachmentBack = 12;
            // Not for a wielder the view cull left unposed: the arm test would
            // pose it (getAttachmentTransform), and nobody sees the blade.
            if (wa.attachmentId == kAttachmentBack && weapIt->second.cachedModel &&
                !instance.poseStale) {
                constexpr uint32_t kAttachmentHandLeft = 2;
                constexpr uint32_t kAttachmentElbowLeft = 4;
                constexpr uint32_t kAttachmentShoulderLeft = 6;
                constexpr float kArmRadius = 0.16f;      // arm mesh thickness around the joint chain
                constexpr float kPushClearance = 0.03f;  // keep the blade slightly off the sleeve
                constexpr float kMaxPush = 0.35f;

                const glm::mat4 weaponInv = glm::inverse(weaponMat);
                const auto& wm = weapIt->second.cachedModel->data;

                glm::vec3 joints[3];
                int jointCount = 0;
                for (uint32_t attId : {kAttachmentShoulderLeft, kAttachmentElbowLeft, kAttachmentHandLeft}) {
                    glm::mat4 m;
                    if (getAttachmentTransform(pair.first, attId, m)) {
                        joints[jointCount++] = glm::vec3(m[3]);
                    }
                }

                float targetPush = 0.0f;
                auto testPoint = [&](const glm::vec3& worldPt) {
                    const glm::vec3 local = glm::vec3(weaponInv * glm::vec4(worldPt, 1.0f));
                    const glm::vec3 closest = glm::clamp(local, wm.boundMin, wm.boundMax);
                    const float pen = kArmRadius - glm::distance(local, closest);
                    if (pen > 0.0f) targetPush = std::max(targetPush, pen + kPushClearance);
                };
                for (int j = 0; j < jointCount; ++j) {
                    testPoint(joints[j]);
                    if (j + 1 < jointCount) testPoint((joints[j] + joints[j + 1]) * 0.5f);
                }
                targetPush = std::min(targetPush, kMaxPush);
                if (targetPush > 0.0f && wa.sheathPush < 0.01f) {
                    core::Logger::getInstance().debug("Sheath push engaged: pen=", targetPush,
                                                      " joints=", jointCount);
                }

                wa.sheathPush += (targetPush - wa.sheathPush) * std::min(1.0f, deltaTime * 14.0f);
                if (wa.sheathPush > 0.001f) {
                    // Horizontal direction from the spine out through the weapon.
                    glm::vec3 outward = glm::vec3(weaponMat[3]) - glm::vec3(charModelMat[3]);
                    outward.z = 0.0f;
                    const float len = glm::length(outward);
                    if (len > 0.001f) {
                        weaponMat = glm::translate(glm::mat4(1.0f), (outward / len) * wa.sheathPush) * weaponMat;
                    }
                }
            }
            weapIt->second.overrideModelMatrix = weaponMat;
            weapIt->second.hasOverrideModelMatrix = true;
            // Where the item is, for the animation range test of an item
            // that animates itself (see the loop above).
            weapIt->second.position = glm::vec3(weaponMat[3]);

            // Item visuals ride their attachment on the weapon: the weapon's
            // matrix, then the attachment bone's pose, then the point's
            // position in that bone (stock makes them children of the bone).
            for (auto& fx : wa.effects) {
                glm::mat4 fxBone(1.0f);
                if (fx.boneIndex < weapIt->second.boneMatrices.size())
                    fxBone = weapIt->second.boneMatrices[fx.boneIndex];
                const glm::mat4 fxMat = weaponMat * fxBone * glm::translate(glm::mat4(1.0f), fx.offset);
                if (fx.m2InstanceId != 0) {
                    // Handed over in poseModelEmitters: M2Renderer's update
                    // runs on another thread while this one does, and moving
                    // its instance here raced it (the matrix, the grid).
                    fx.worldMatrix = fxMat;
                    fx.fade = instance.visible ? instance.opacity : 0.0f;
                    fx.posePending = true;
                    continue;
                }
                auto fxIt = instances.find(fx.effectInstanceId);
                if (fxIt == instances.end()) continue;
                fxIt->second.overrideModelMatrix = fxMat;
                fxIt->second.hasOverrideModelMatrix = true;
                // Keep the effect near the character so animation distance culling
                // (which works off instance position) treats it like its wielder.
                fxIt->second.position = instance.position;
            }
        }
    }
}
// --- Bone transform calculation ---

constexpr int32_t kKeyBoneShoulderL = 2;
constexpr int32_t kKeyBoneShoulderR = 3;
constexpr int32_t kKeyBoneSpineLow = 4;
constexpr int32_t kKeyBoneHead = 6;

bool CharacterRenderer::playUpperBodyAnimation(uint32_t instanceId, uint32_t torsoAnim,
                                               uint32_t leftArmAnim, uint32_t rightArmAnim) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    auto& instance = it->second;
    const auto& model = models[instance.modelId].data;
    // A variation rolled by frequency, as for any play (0x7385C0 passes -1
    // to every track); the same one for every part playing that id.
    uint32_t rolledId[3] = {0, 0, 0};
    int rolledSeq[3] = {-1, -1, -1};
    int rolledCount = 0;
    auto find = [&](uint32_t id) -> int {
        if (id == 0 || id == anim::STAND) return -1;
        for (int k = 0; k < rolledCount; ++k)
            if (rolledId[k] == id) return rolledSeq[k];
        int idx = anim::rollVariationStock(model.sequences, model.bones, id, anim::stockRand());
        idx = anim::resolveSequenceAlias(model.sequences, idx);
        if (rolledCount < 3) {
            rolledId[rolledCount] = id;
            rolledSeq[rolledCount] = idx;
            ++rolledCount;
        }
        return idx;
    };
    const uint32_t anims[3] = {leftArmAnim, rightArmAnim, torsoAnim};
    bool any = false;
    float duration = 0.0f;
    for (int part = 0; part < 3; part++) {
        instance.upperSequenceIndex[part] = find(anims[part]);
        if (instance.upperSequenceIndex[part] >= 0) {
            any = true;
            duration = std::max(duration, static_cast<float>(
                model.sequences[instance.upperSequenceIndex[part]].duration));
        }
    }
    if (!any || duration <= 0.0f) {
        instance.upperTime = -1.0f;
        return false;
    }
    instance.upperTime = 0.0f;
    instance.upperDurationMs = duration;

    // Which part each bone is in: the lower spine and everything under it is
    // the upper body, a shoulder and everything under it an arm. Parents come
    // first in M2 bone order, so one pass settles it.
    instance.boneUpper.assign(model.bones.size(), -1);
    for (size_t i = 0; i < model.bones.size(); i++) {
        const auto& bone = model.bones[i];
        const int8_t parent = bone.parentBone >= 0 && static_cast<size_t>(bone.parentBone) < i
            ? instance.boneUpper[bone.parentBone] : -1;
        if (bone.keyBoneId == kKeyBoneShoulderL)      instance.boneUpper[i] = 0;
        else if (bone.keyBoneId == kKeyBoneShoulderR) instance.boneUpper[i] = 1;
        else if (bone.keyBoneId == kKeyBoneSpineLow)  instance.boneUpper[i] = 2;
        else                                          instance.boneUpper[i] = parent;
    }
    return true;
}

uint32_t CharacterRenderer::getSheathEventTime(uint32_t instanceId, uint32_t animationId) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return UINT32_MAX;
    auto mIt = models.find(it->second.modelId);
    if (mIt == models.end()) return UINT32_MAX;
    const auto& model = mIt->second.data;
    for (size_t i = 0; i < model.sequences.size() && i < model.sheathEventTimes.size(); i++) {
        if (model.sequences[i].id == animationId && model.sequences[i].variationIndex == 0)
            return model.sheathEventTimes[i];
    }
    return UINT32_MAX;
}

bool CharacterRenderer::getCastReleaseEvent(uint32_t instanceId, uint32_t animationId,
                                            uint32_t& timeMs, glm::vec3* worldPos) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    auto mIt = models.find(it->second.modelId);
    if (mIt == models.end()) return false;
    const auto& model = mIt->second.data;
    for (size_t i = 0; i < model.sequences.size() && i < model.castReleaseEvents.size(); i++) {
        if (model.sequences[i].id != animationId || model.sequences[i].variationIndex != 0) continue;
        const auto& ev = model.castReleaseEvents[i];
        if (ev.timeMs == UINT32_MAX) return false;
        timeMs = ev.timeMs;
        if (worldPos) {
            // The pose of now, as getAttachmentTransform gives it.
            refreshStalePose(it->second);
            const auto& instance = it->second;
            glm::mat4 boneMat(1.0f);
            if (ev.bone < instance.boneMatrices.size()) boneMat = instance.boneMatrices[ev.bone];
            const glm::mat4 modelMat = instance.hasOverrideModelMatrix
                ? instance.overrideModelMatrix : getModelMatrix(instance);
            *worldPos = glm::vec3(modelMat * boneMat * glm::vec4(ev.position, 1.0f));
        }
        return true;
    }
    return false;
}

size_t CharacterRenderer::getWeaponAttachmentCount(uint32_t instanceId) const {
    auto it = instances.find(instanceId);
    return it != instances.end() ? it->second.weaponAttachments.size() : 0;
}

bool CharacterRenderer::getWeaponTrailPoints(uint32_t instanceId, size_t index, glm::vec3& bottom,
                                             glm::vec3& top) {
    auto it = instances.find(instanceId);
    if (it == instances.end() || index >= it->second.weaponAttachments.size()) return false;
    // Only a weapon in the hand swings (0x715BA0 asks for the drawn state).
    const uint32_t att = it->second.weaponAttachments[index].attachmentId;
    if (att != 1 && att != 2) return false;
    auto wIt = instances.find(it->second.weaponAttachments[index].weaponInstanceId);
    if (wIt == instances.end()) return false;
    auto mIt = models.find(wIt->second.modelId);
    if (mIt == models.end()) return false;
    const auto& model = mIt->second.data;
    if (model.weaponTrailBottom.bone == UINT32_MAX || model.weaponTrailTop.bone == UINT32_MAX)
        return false;
    // The pose of now, as getAttachmentTransform gives it, for the unit and
    // the weapon riding its hand.
    refreshStalePose(it->second);
    refreshStalePose(wIt->second);
    const auto& w = wIt->second;
    const glm::mat4 modelMat = w.hasOverrideModelMatrix ? w.overrideModelMatrix : getModelMatrix(w);
    auto place = [&](const pipeline::M2Model::WeaponTrailPoint& p) {
        glm::mat4 bone(1.0f);
        if (p.bone < w.boneMatrices.size()) bone = w.boneMatrices[p.bone];
        return glm::vec3(modelMat * bone * glm::vec4(p.position, 1.0f));
    };
    bottom = place(model.weaponTrailBottom);
    top = place(model.weaponTrailTop);
    return true;
}

void CharacterRenderer::setInstanceAnimationFrozen(uint32_t instanceId, bool frozen, float atMs) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    auto& inst = it->second;
    if (frozen && atMs >= 0.0f && inst.cachedModel && inst.currentSequenceIndex >= 0 &&
        inst.currentSequenceIndex < static_cast<int>(inst.cachedModel->data.sequences.size())) {
        const float dur = static_cast<float>(
            inst.cachedModel->data.sequences[static_cast<size_t>(inst.currentSequenceIndex)].duration);
        inst.animationTime = dur > 0.0f ? std::min(atMs, dur) : atMs;
    }
    inst.animationFrozen = frozen;
}

uint32_t CharacterRenderer::getAnimationDurationMs(uint32_t instanceId, uint32_t animationId) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return 0;
    auto mIt = models.find(it->second.modelId);
    if (mIt == models.end()) return 0;
    for (const auto& seq : mIt->second.data.sequences)
        if (seq.id == animationId && seq.variationIndex == 0) return seq.duration;
    return 0;
}

void CharacterRenderer::calculateBoneMatrices(CharacterInstance& instance) {
    if (!instance.cachedModel) return;
    auto& model = instance.cachedModel->data;

    if (model.bones.empty()) {
        return;
    }

    size_t numBones = model.bones.size();
    instance.boneMatrices.resize(numBones);
    instance.poseStale = false;
    // A new pose for the bone buffers to take (uploadBones). 0 is "none".
    if (++instance.poseSerial == 0) instance.poseSerial = 1;

    const auto& gsd = model.globalSequenceDurations;

    // One-time diagnostic: check bone ordering (parents must precede children)
    static bool checkedBoneOrder = false;
    if (!checkedBoneOrder) {
        checkedBoneOrder = true;
        for (size_t i = 0; i < numBones; i++) {
            const auto& bone = model.bones[i];
            if (bone.parentBone >= 0 && static_cast<size_t>(bone.parentBone) >= i) {
                LOG_WARNING("Bone ", i, " references parent ", bone.parentBone,
                            " which comes AFTER it - will use stale matrix!");
            }
        }
    }

    // How many more calls the extreme-translation probe below runs for.
    // Declared here rather than inside the loop so it can be advanced once per
    // call, after it, instead of on a bone the loop happens to reach.
    static int diagFrames = 0;

    // Cross-fade weight of the new sequence: 0 at the switch, 1 when done,
    // eased as the stock client eases it (m2_track::sequenceBlendWeight).
    // The outgoing side is the old sequence still playing on its own clock
    // (blendFromSeq), or a snapshot of the pose where there was none.
    const bool fromSequence = instance.blendFromSeq >= 0 &&
                              instance.blendFromSeq < static_cast<int>(model.sequences.size());
    const bool blending = instance.blendDurationMs > 0.0f &&
                          instance.blendElapsedMs < instance.blendDurationMs &&
                          (fromSequence || instance.blendFromPose.size() == numBones);
    const float fromSampleTime = fromSequence
        ? anim::playModeSampleTime(static_cast<anim::PlayMode>(instance.blendFromMode), instance.blendFromTime,
                                   static_cast<float>(model.sequences[instance.blendFromSeq].duration))
        : 0.0f;
    // A fallback played backwards or held samples its own time (anim::PlayMode).
    const float bodySampleTime =
        (instance.playMode != 0 && instance.currentSequenceIndex >= 0 &&
         instance.currentSequenceIndex < static_cast<int>(model.sequences.size()))
            ? anim::playModeSampleTime(static_cast<anim::PlayMode>(instance.playMode), instance.animationTime,
                                       static_cast<float>(model.sequences[instance.currentSequenceIndex].duration))
            : instance.animationTime;
    const float blendWeight = blending
        ? m2_track::sequenceBlendWeight(instance.blendElapsedMs / instance.blendDurationMs)
        : 1.0f;
    instance.localPose.resize(numBones);

    // E7: the spring chains, stepped for this pose. Off (springLevel_ 0) this
    // is one compare and the loop below never looks at a spring slot.
    const bool springs = springLevel_ > 0 && stepSprings(instance);
    const M2ModelGPU& gm = *instance.cachedModel;

    // Key hints, six per bone: the body's three tracks, then the upper-body
    // overlay's, which plays another sequence and would only push the body's
    // hints off their keys. Each thread owns the instances it poses, so the
    // hints need no lock. The samples are the same either way (see
    // m2_track::lowerKeyIndexFrom); WOWEE_CHAR_KEY_CURSOR=0 searches from
    // scratch every time.
    static const bool keyCursorEnabled = core::envFlagEnabled("WOWEE_CHAR_KEY_CURSOR", true);
    uint32_t* cursors = nullptr;
    if (keyCursorEnabled) {
        if (instance.keyCursor.size() != numBones * 6) instance.keyCursor.assign(numBones * 6, 0u);
        cursors = instance.keyCursor.data();
    }

    for (size_t i = 0; i < numBones; i++) {
        const auto& bone = model.bones[i];

        int sequence = instance.currentSequenceIndex;
        float time = bodySampleTime;

        // Local transform includes pivot bracket: T(pivot)*T*R*S*T(-pivot)
        // At rest this is identity, so no separate bind pose is needed
        BoneLocalPose pose = sampleBonePose(bone, time, instance.globalSequenceTime,
                                            sequence, gsd, cursors ? cursors + i * 6 : nullptr);
        if (blending) {
            // Tracks on a global sequence run on their own clock whatever the
            // body plays, so they have nothing to fade from.
            const BoneLocalPose from = fromSequence
                ? sampleBonePose(bone, fromSampleTime, instance.globalSequenceTime, instance.blendFromSeq, gsd)
                : instance.blendFromPose[i];
            if (bone.translation.globalSequence < 0)
                pose.translation = glm::mix(from.translation, pose.translation, blendWeight);
            if (bone.rotation.globalSequence < 0)
                pose.rotation = glm::normalize(glm::slerp(from.rotation, pose.rotation, blendWeight));
            if (bone.scale.globalSequence < 0)
                pose.scale = glm::mix(from.scale, pose.scale, blendWeight);
        }
        // The upper-body overlay over whatever the body plays, faded in and
        // out over 150 ms so the reach neither pops on nor snaps off the run.
        if (instance.upperTime >= 0.0f && instance.boneUpper.size() == numBones &&
            instance.boneUpper[i] >= 0) {
            const int upSeq = instance.upperSequenceIndex[instance.boneUpper[i]];
            if (upSeq >= 0 && upSeq < static_cast<int>(model.sequences.size())) {
                const float upDur = static_cast<float>(model.sequences[upSeq].duration);
                const float upT = std::min(instance.upperTime, upDur);
                const float edge = std::min(instance.upperTime,
                                            instance.upperDurationMs - instance.upperTime);
                const float w = glm::clamp(edge / 150.0f, 0.0f, 1.0f);
                const BoneLocalPose up = sampleBonePose(bone, upT, instance.globalSequenceTime,
                                                        upSeq, gsd,
                                                        cursors ? cursors + i * 6 + 3 : nullptr);
                pose.translation = glm::mix(pose.translation, up.translation, w);
                pose.rotation = glm::normalize(glm::slerp(pose.rotation, up.rotation, w));
                pose.scale = glm::mix(pose.scale, up.scale, w);
            }
        }
        instance.localPose[i] = pose;
        glm::mat4 localTransform = composeBoneTransform(bone, pose);

        // Look twist (stock CGUnit_C 0x0073E1A5/0x0073E237): a yaw about the
        // SpineLow and Head key bones, so the chest and face turn back to the
        // facing while the body runs toward a strafe.
        const float twistYaw = bone.keyBoneId == kKeyBoneSpineLow ? instance.torsoYawOverrideRad
                             : bone.keyBoneId == kKeyBoneHead     ? instance.headYawOverrideRad
                             :                                      0.0f;
        if (twistYaw != 0.0f) {
            glm::mat4 extraYaw = glm::translate(glm::mat4(1.0f), bone.pivot)
                                * glm::rotate(glm::mat4(1.0f), twistYaw, glm::vec3(0.0f, 0.0f, 1.0f))
                                * glm::translate(glm::mat4(1.0f), -bone.pivot);
            localTransform = extraYaw * localTransform;
        }

        // E7: the spring's swing on top of the sampled pose, about the bone's
        // pivot, its model-space axis taken into the parent's frame. A fan's
        // bones share a parent, so each also takes the swings of the fan
        // bones nearer the body, about their pivots, as a chain would pass on.
        if (springs && i < gm.springSlot.size() && gm.springSlot[i] >= 0) {
            namespace sm = secondary_motion;
            const int slot = gm.springSlot[i];
            const sm::Chain& chain = gm.springChains[gm.springSlotChain[slot]];
            const glm::mat3 toParent = (bone.parentBone >= 0 && static_cast<size_t>(bone.parentBone) < numBones)
                ? glm::inverse(glm::mat3(instance.boneMatrices[bone.parentBone]))
                : glm::mat3(1.0f);
            auto swingAbout = [&](int s, const glm::vec3& centre) -> std::optional<glm::mat4> {
                const glm::vec3 rv = sm::rotationVector(chain, instance.springBones[s], instance.springWeight);
                if (glm::dot(rv, rv) < 1e-12f) return std::nullopt;
                return glm::translate(glm::mat4(1.0f), centre) *
                       glm::mat4_cast(sm::rotationInFrame(rv, toParent)) *
                       glm::translate(glm::mat4(1.0f), -centre);
            };
            if (!chain.fan) {
                if (auto m = swingAbout(slot, bone.pivot + pose.translation)) localTransform = *m * localTransform;
            } else {
                const int first = slot - gm.springSlotIndex[slot];
                glm::mat4 acc(1.0f);
                bool any = false;
                for (int k = first; k <= slot; ++k) {
                    if (auto m = swingAbout(k, model.bones[chain.bones[k - first]].pivot)) { acc = acc * *m; any = true; }
                }
                if (any) localTransform = acc * localTransform;
            }
        }

        // Compose with parent
        if (bone.parentBone >= 0 && static_cast<size_t>(bone.parentBone) < numBones) {
            instance.boneMatrices[i] = instance.boneMatrices[bone.parentBone] * localTransform;
        } else {
            instance.boneMatrices[i] = localTransform;
        }

        // Diagnostic: detect bones with extreme translation. Gated so the abs()
        // probes and the post-loop counter bump only run for the first few frames.
        if (diagFrames < 3) {
            float tx = std::abs(instance.boneMatrices[i][3][0]);
            float ty = std::abs(instance.boneMatrices[i][3][1]);
            float tz = std::abs(instance.boneMatrices[i][3][2]);
            if (tx > 50.0f || ty > 50.0f || tz > 50.0f) {
                LOG_DEBUG("BONE DIAG: bone[", i, "] keyBone=", bone.keyBoneId,
                            " flags=0x", std::hex, bone.flags, std::dec,
                            " parent=", bone.parentBone,
                            " pivot=(", bone.pivot.x, ",", bone.pivot.y, ",", bone.pivot.z, ")",
                            " mat_t=(", instance.boneMatrices[i][3][0], ",",
                            instance.boneMatrices[i][3][1], ",", instance.boneMatrices[i][3][2], ")",
                            " local_t=(", localTransform[3][0], ",", localTransform[3][1], ",",
                            localTransform[3][2], ")",
                            " animTime=", instance.animationTime,
                            " gsTime=", instance.globalSequenceTime,
                            " seqIdx=", instance.currentSequenceIndex);
            }
        }
    }
    // E9: the feet put on the floor under them (foot_ik.hpp), with the
    // targets updateFootPlacement left; the ends as the animation had them
    // are kept for its next floor reads. Off, or a model without two legs,
    // this is one compare.
    if (footIkOn_ && gm.footRig.valid) {
        foot_ik::apply(instance.boneMatrices, gm.footRig, instance.footIk.targets, instance.footIk.ends);
        instance.footIk.endsValid = true;
    }
    // E7: cloth turned back out of the legs and hips, on the final pose.
    if (springs && !gm.springColliders.empty()) applySpringCollisions(instance, gm);
    // E7: the robe's hem, from the legs as posed now. A pose without it
    // clears the flag, so the robe batches draw stock.
    if (springLevel_ > 0 && !gm.robeGeosets.empty()) stepRobeHem(instance, gm);
    else instance.hemActive = false;
    // E7: the cloak bent as one from the shoulders by its chain's swing.
    if (springs && gm.capeChain >= 0) updateCapeBend(instance, gm);
    else instance.capeActive = false;

    // Once per call, not once per last bone. The bump used to sit inside the
    // loop under `i == numBones - 1`, which this loop does reach - but a
    // counter that only advances on a condition inside the block it guards is
    // the shape that had a light-volume diagnostic logging every frame of every
    // session (a5080cec), and one `continue` added above would make this the
    // same fault. tools/bounded_log_check.py reports it.
    if (diagFrames < 3) diagFrames++;
}

glm::mat4 CharacterRenderer::getBoneTransform(const pipeline::M2Bone& bone, float animTime, float globalSeqTime,
                                               int sequenceIndex, const std::vector<uint32_t>& globalSeqDurations) {
    return composeBoneTransform(bone, sampleBonePose(bone, animTime, globalSeqTime,
                                                     sequenceIndex, globalSeqDurations));
}

CharacterRenderer::BoneLocalPose CharacterRenderer::sampleBonePose(
        const pipeline::M2Bone& bone, float animTime, float globalSeqTime,
        int sequenceIndex, const std::vector<uint32_t>& globalSeqDurations,
        uint32_t* cursors) {
    // Resolve global sequences: bones with globalSequence >= 0 use sequence 0
    // with time wrapped at the global sequence duration, independent of the
    // character's current animation.
    BoneLocalPose pose;
    pose.translation = m2_track::sampleVec3(
        bone.translation, sequenceIndex, animTime, globalSeqTime,
        globalSeqDurations, glm::vec3(0.0f), cursors ? cursors + 0 : nullptr);
    pose.rotation = m2_track::sampleQuat(
        bone.rotation, sequenceIndex, animTime, globalSeqTime,
        globalSeqDurations, cursors ? cursors + 1 : nullptr);
    pose.scale = m2_track::sampleVec3(
        bone.scale, sequenceIndex, animTime, globalSeqTime,
        globalSeqDurations, glm::vec3(1.0f), cursors ? cursors + 2 : nullptr);
    return pose;
}

glm::mat4 CharacterRenderer::composeBoneTransform(const pipeline::M2Bone& bone,
                                                  const BoneLocalPose& pose) {
    const glm::vec3& translation = pose.translation;
    const glm::quat& rotation = pose.rotation;
    const glm::vec3& scale = pose.scale;

    // M2 bone transform: T(pivot) * T(trans) * R(rot) * S(scale) * T(-pivot).
    // Build directly instead of chaining glm::translate/rotate/scale (each of
    // those is a full mat4 multiply). The composed matrix has:
    //   linear part      = R * diag(scale)     (3 column scales of R)
    //   translation part = pivot + trans - RS * pivot
    glm::mat3 R = glm::mat3_cast(rotation);
    glm::vec3 c0 = R[0] * scale.x;
    glm::vec3 c1 = R[1] * scale.y;
    glm::vec3 c2 = R[2] * scale.z;
    glm::vec3 t  = (bone.pivot + translation)
                   - (c0 * bone.pivot.x + c1 * bone.pivot.y + c2 * bone.pivot.z);
    glm::mat4 transform;
    transform[0] = glm::vec4(c0, 0.0f);
    transform[1] = glm::vec4(c1, 0.0f);
    transform[2] = glm::vec4(c2, 0.0f);
    transform[3] = glm::vec4(t,  1.0f);
    return transform;
}

// --- Rendering ---

void CharacterRenderer::prepareRender(uint32_t frameIndex) {
    if (instances.empty() || !opaquePipeline_) return;
    if (frameIndex >= 2 || boneDescPool_ == VK_NULL_HANDLE || boneSetLayout_ == VK_NULL_HANDLE) return;

    // Pre-allocate bone SSBOs + descriptor sets on main thread (pool ops not thread-safe)
    for (auto& [id, instance] : instances) {
        int numBones = std::min(static_cast<int>(instance.boneMatrices.size()), MAX_BONES);
        if (numBones <= 0) continue;

        if (!instance.boneBuffer[frameIndex]) {
            VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = MAX_BONES * sizeof(glm::mat4);
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo allocInfo{};
            if (vmaCreateBuffer(vkCtx_->getAllocator(), &bci, &aci,
                            &instance.boneBuffer[frameIndex], &instance.boneAlloc[frameIndex], &allocInfo) != VK_SUCCESS) {
                instance.boneBuffer[frameIndex] = VK_NULL_HANDLE;
                instance.boneAlloc[frameIndex] = VK_NULL_HANDLE;
                instance.boneMapped[frameIndex] = nullptr;
                continue;
            }
            instance.boneMapped[frameIndex] = allocInfo.pMappedData;
            instance.uploadedPoseSerial[frameIndex] = 0;  // holds no pose yet

            // Initialize all bone slots to identity so out-of-range indices
            // produce correct (neutral) transforms instead of GPU garbage
            if (instance.boneMapped[frameIndex]) {
                auto* dst = static_cast<glm::mat4*>(instance.boneMapped[frameIndex]);
                for (int j = 0; j < MAX_BONES; j++) dst[j] = glm::mat4(1.0f);
            }

            VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = boneDescPool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &boneSetLayout_;
            VkResult dsRes = vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &instance.boneSet[frameIndex]);
            if (dsRes != VK_SUCCESS) {
                LOG_ERROR("CharacterRenderer::prepareRender: bone descriptor alloc failed (instance=",
                          id, ", frame=", frameIndex, ", vk=", static_cast<int>(dsRes), ")");
                if (instance.boneBuffer[frameIndex]) {
                    vmaDestroyBuffer(vkCtx_->getAllocator(),
                                     instance.boneBuffer[frameIndex], instance.boneAlloc[frameIndex]);
                    instance.boneBuffer[frameIndex] = VK_NULL_HANDLE;
                    instance.boneAlloc[frameIndex] = VK_NULL_HANDLE;
                    instance.boneMapped[frameIndex] = nullptr;
                }
                continue;
            }

            if (instance.boneSet[frameIndex]) {
                VkDescriptorBufferInfo bufInfo{};
                bufInfo.buffer = instance.boneBuffer[frameIndex];
                bufInfo.offset = 0;
                bufInfo.range = bci.size;
                VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = instance.boneSet[frameIndex];
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &bufInfo;
                vkUpdateDescriptorSets(vkCtx_->getDevice(), 1, &write, 0, nullptr);
            }
        }
    }
}

void CharacterRenderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera) {
    if (instances.empty() || !opaquePipeline_) {
        return;
    }
    const float renderRadius = static_cast<float>(envSizeOrDefault("WOWEE_CHAR_RENDER_RADIUS", 130));
    const float renderRadiusSq = renderRadius * renderRadius;
    // Default frustum-cull radius when model bounds aren't available.
    // 4.0 covers Tauren, mounted characters, and most creature models.
    constexpr float kDefaultCharacterCullRadius = 4.0f;
    const glm::vec3 camPos = camera.getPosition();
    const float frameTimeSeconds = std::chrono::duration<float>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // Extract frustum planes for per-instance visibility testing
    Frustum frustum;
    frustum.extractFromMatrix(camera.getViewProjectionMatrix());

    uint32_t frameIndex = vkCtx_->getCurrentFrame();
    uint32_t frameSlot = frameIndex % 2u;

    // Reset material ring buffer and descriptor pool once per frame slot.
    if (lastMaterialPoolResetFrame_ != frameIndex) {
        materialRingOffset_[frameSlot] = 0;
        if (materialDescPools_[frameSlot]) {
            vkResetDescriptorPool(vkCtx_->getDevice(), materialDescPools_[frameSlot], 0);
        }
        materialDescriptorCache_[frameSlot].clear();
        lastMaterialPoolResetFrame_ = frameIndex;
    }

    // Pre-compute aligned UBO stride for ring buffer sub-allocation
    const uint32_t uboStride = (sizeof(CharMaterialUBO) + materialUboAlignment_ - 1) & ~(materialUboAlignment_ - 1);
    const uint32_t ringCapacityBytes = uboStride * MATERIAL_RING_CAPACITY;
    auto getMaterialDescriptorSet = [&](VkTexture* diffuse, VkTexture* normal,
                                        VkTexture* stage2) -> VkDescriptorSet {
        // Valid, not merely non-null. descriptorInfo() hands back whatever the
        // texture holds - VK_NULL_HANDLE for a view and a sampler that were
        // never created - and declares SHADER_READ_ONLY_OPTIMAL either way. A
        // draw that samples that is undefined behaviour, and on NVIDIA it
        // surfaces as a graphics engine exception and a lost device rather
        // than anything this client can catch.
        //
        // Both call sites check the diffuse and neither checks the normal, and
        // the normal is the one that can arrive from an asynchronous generation
        // pass or from a flat fallback whose own upload can fail under the same
        // memory pressure that makes the texture cache start rejecting.
        if (!diffuse || !diffuse->isValid()) diffuse = whiteTexture_.get();
        if (!normal || !normal->isValid()) normal = flatNormalTexture_.get();
        if (!stage2 || !stage2->isValid()) stage2 = whiteTexture_.get();
        if (!diffuse || !diffuse->isValid() || !normal || !normal->isValid() ||
            !stage2 || !stage2->isValid()) {
            // Even the fallbacks are gone. Skipping the draw loses a model;
            // binding a null view loses the device.
            return VK_NULL_HANDLE;
        }
        const VkDescriptorImageInfo diffuseInfo = diffuse->descriptorInfo();
        const VkDescriptorImageInfo normalInfo = normal->descriptorInfo();
        const VkDescriptorImageInfo stage2Info = stage2->descriptorInfo();
        const MaterialDescriptorKey key{.diffuse = diffuseInfo.imageView, .normal = normalInfo.imageView,
                                        .diffuseSampler = diffuseInfo.sampler, .normalSampler = normalInfo.sampler,
                                        .stage2 = stage2Info.imageView, .stage2Sampler = stage2Info.sampler};
        auto& cache = materialDescriptorCache_[frameSlot];
        if (auto it = cache.find(key); it != cache.end()) return it->second;

        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = materialDescPools_[frameSlot];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &materialSetLayout_;
        if (vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &set) != VK_SUCCESS)
            return VK_NULL_HANDLE;

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = materialRingBuffer_[frameSlot];
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(CharMaterialUBO);
        VkWriteDescriptorSet writes[4] = {};
        writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = nullptr, .dstSet = set, .dstBinding = 0, .dstArrayElement = 0, .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &diffuseInfo, .pBufferInfo = nullptr, .pTexelBufferView = nullptr};
        writes[1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = nullptr, .dstSet = set, .dstBinding = 1, .dstArrayElement = 0, .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pImageInfo = nullptr, .pBufferInfo = &bufferInfo, .pTexelBufferView = nullptr};
        writes[2] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = nullptr, .dstSet = set, .dstBinding = 2, .dstArrayElement = 0, .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &normalInfo, .pBufferInfo = nullptr, .pTexelBufferView = nullptr};
        writes[3] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = nullptr, .dstSet = set, .dstBinding = 3, .dstArrayElement = 0, .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &stage2Info, .pBufferInfo = nullptr, .pTexelBufferView = nullptr};
        vkUpdateDescriptorSets(vkCtx_->getDevice(), 4, writes, 0, nullptr);
        cache.emplace(key, set);
        return set;
    };

    // Bind per-frame descriptor set (set 0) -- shared across all draws
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);

    // Start with opaque pipeline
    VkPipeline currentPipeline = opaquePipeline_;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, currentPipeline);

    for (auto& pair : instances) {
        auto& instance = pair.second;

        // Skip invisible instances (e.g., player in first-person mode)
        if (!instance.visible) continue;

        // Character instance culling: test both distance and frustum visibility
        if (!instance.hasOverrideModelMatrix && !instance.isSceneModel) {
            glm::vec3 toInst = instance.position - camPos;
            float distSq = glm::dot(toInst, toInst);

            // Distance cull: skip if beyond render radius
            if (distSq > renderRadiusSq) continue;

            // Compute per-instance bounding radius from model data when available.
            float cullRadius = kDefaultCharacterCullRadius;
            auto mIt = models.find(instance.modelId);
            if (mIt != models.end()) {
                float modelR = mIt->second.data.boundRadius;
                if (modelR > 0.01f)
                    cullRadius = std::max(kDefaultCharacterCullRadius, modelR * std::max(0.001f, instance.scale));
            }

            // Frustum cull: skip if outside view frustum
            if (!frustum.intersectsSphere(instance.position, cullRadius)) continue;
        }

        if (!instance.cachedModel) continue;
        const auto& gpuModel = *instance.cachedModel;

        // Skip models without GPU buffers
        if (!gpuModel.vertexBuffer) continue;

        // Skip fully transparent instances
        if (instance.opacity <= 0.0f) continue;

        // Set model matrix (use override for weapon instances)
        glm::mat4 modelMat = instance.hasOverrideModelMatrix
            ? instance.overrideModelMatrix
            : getModelMatrix(instance);

        // Push model matrix, and a zero UV scroll; batches with a texture
        // animation push their own below.
        struct { glm::mat4 model; glm::vec4 uvOffset; glm::ivec4 stages; } charPush{
            modelMat, glm::vec4(0.0f), glm::ivec4(0)};
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(charPush), &charPush);
        glm::vec4 pushedUvOffset(0.0f);
        int32_t pushedStages = 0;

        // Upload bone matrices to SSBO
        int numBones = std::min(static_cast<int>(instance.boneMatrices.size()), MAX_BONES);
        if (numBones > 0) {
            // GPU allocation is performed by prepareRender() before command
            // recording. Never allocate buffers/descriptors from the draw loop.
            if (!instance.boneBuffer[frameIndex] || !instance.boneSet[frameIndex]) continue;

            // Upload bone matrices. The shadow pass, recorded earlier in the
            // frame, has normally put this pose in the buffer already.
            uploadBones(instance, frameIndex, numBones);

            // Bind bone descriptor set (set 2)
            if (instance.boneSet[frameIndex]) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipelineLayout_, 2, 1, &instance.boneSet[frameIndex], 0, nullptr);
            }
        }

        // Bind vertex and index buffers
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &gpuModel.vertexBuffer, &offset);
        vkCmdBindIndexBuffer(cmd, gpuModel.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

        if (!gpuModel.data.batches.empty()) {
            bool applyGeosetFilter = !instance.activeGeosets.empty();
            if (applyGeosetFilter) {
                bool hasRenderableGeoset = false;
                for (const auto& batch : gpuModel.data.batches) {
                    if (instance.activeGeosets.find(batch.submeshId) != instance.activeGeosets.end()) {
                        hasRenderableGeoset = true;
                        break;
                    }
                }
                if (!hasRenderableGeoset) {
                    static std::unordered_set<uint32_t> loggedGeosetFallback;
                    if (loggedGeosetFallback.insert(instance.id).second) {
                        LOG_WARNING("Geoset filter matched no batches for instance ",
                                    instance.id, " (model ", instance.modelId,
                                    "); rendering all batches as fallback");
                    }
                    applyGeosetFilter = false;
                }
            }


            auto batchUsesTextureType = [](const M2ModelGPU& gm, const pipeline::M2Batch& b, uint32_t wantedType) {
                if (b.textureIndex == 0xFFFF || gm.data.textureLookup.empty()) return false;

                uint32_t comboCount = b.textureCount ? static_cast<uint32_t>(b.textureCount) : 1u;
                comboCount = std::min<uint32_t>(comboCount, 8u);
                for (uint32_t i = 0; i < comboCount; ++i) {
                    uint32_t lookupPos = static_cast<uint32_t>(b.textureIndex) + i;
                    if (lookupPos >= gm.data.textureLookup.size()) break;
                    uint16_t texSlot = gm.data.textureLookup[lookupPos];
                    if (texSlot < gm.data.textures.size() && gm.data.textures[texSlot].type == wantedType)
                        return true;
                }
                return false;
            };

            // A lit preview (a glue scene) takes the lit path for everything.
            const bool previewMainModel = renderPassOverride_ != VK_NULL_HANDLE &&
                                          !previewLit_ &&
                                          !instance.hasOverrideModelMatrix;

            // Draw batches in two passes: opaque (blendMode 0) first, then
            // alpha-key/blend after.  This ensures capes and body parts write
            // depth before hair overlay, preventing hair→cape z-fight.
            auto getBatchBlendMode = [&](const pipeline::M2Batch& b) -> uint16_t {
                if (b.materialIndex < gpuModel.data.materials.size())
                    return gpuModel.data.materials[b.materialIndex].blendMode;
                return 0;
            };

            // Use precomputed batch render order (cached on gpuModel at load time;
            // depends only on static batch metadata, so per-frame re-sorting was waste).
            const auto& sortedBatchIndices = gpuModel.sortedBatchIndices;

            for (int pass = 0; pass < 2; pass++) {
            for (size_t bi : sortedBatchIndices) {
                const auto& batch = gpuModel.data.batches[bi];
                // The stock shader for this batch. A layer an earlier batch
                // absorbed as its second texture (shader 0x8000) is not drawn.
                const m2fx::BatchEffect* effect =
                    bi < gpuModel.batchEffects.size() ? &gpuModel.batchEffects[bi] : nullptr;
                if (effect && effect->hidden) continue;
                uint16_t bm = getBatchBlendMode(batch);
                if (pass == 0 && bm != 0) continue;  // pass 0: opaque only
                if (pass == 1 && bm == 0) continue;   // pass 1: non-opaque only
                if (applyGeosetFilter) {
                    if (instance.activeGeosets.find(batch.submeshId) == instance.activeGeosets.end()) {
                        continue;
                    }
                } else {
                    // Even without a geoset filter, skip eye glow (group 17)
                    // and group 18 unless explicitly opted in. These geosets are
                    // only for DK/NE eye glow and should be off by default.
                    //
                    // Group 15 joins them, for the same reason and a visible
                    // one: it is the cloak, and its variants above 1501 are
                    // cloaks that exist. A model drawn with no filter drew one
                    // whether or not the character wore anything - and with no
                    // cloak texture to bind, a white sheet. An NPC that owns a
                    // cape says so by naming the geoset; one that says nothing
                    // has none.
                    //
                    // Only on a character model. A creature model's sections
                    // are all on in stock (0x832EA0), and its cloak is skin
                    // art: skipping group 15 there took the capes off the
                    // Four Horsemen, Rexxar, fel orcs and eredar.
                    uint16_t grp = batch.submeshId / 100;
                    if (gpuModel.hasCompositeSlots && (grp == 17 || grp == 18 || grp == 15)) continue;
                }
                // One line per head batch, for the first few instances: which
                // texture slot it resolved to and what type that slot is. The
                // player and an NPC use the same model, the same art and the
                // same pairing, and one of them draws a face - so the answer is
                // in what each batch actually got, which nothing has yet shown.
                // Only instances that are actually a character in the world: one
                // with per-instance texture overrides is an NPC, and the player
                // is the one that draws its head detail. The first run of this
                // spent all its lines on the character-select models before a
                // single NPC had spawned.
                const bool worldCharacter =
                    !instance.textureSlotOverrides.empty() || instance.drawSkinExtra;
                if (batch.submeshId == 0 && worldCharacter && headBatchCanaryCount_ < 24) {
                    ++headBatchCanaryCount_;
                    uint32_t chosenType = 0;
                    if (batch.textureIndex != 0xFFFF && !gpuModel.data.textureLookup.empty()) {
                        uint16_t sl = gpuModel.data.textureLookup[batch.textureIndex];
                        if (sl < gpuModel.data.textures.size())
                            chosenType = gpuModel.data.textures[sl].type;
                    }
                    VkTexture* rt = resolveBatchTexture(instance, gpuModel, batch);
                    // At debug: this canary was for heads resolving to the
                    // white texture, and they resolve to a texture. Twenty-four
                    // of them a session is noise in a log read for faults.
                    core::Logger::getInstance().debug(
                        "Head batch: instance=", pair.first, " model=", instance.modelId,
                        " geoset=", batch.submeshId, " firstSlotType=", chosenType,
                        " resolved=", (rt == whiteTexture_.get() ? "WHITE"
                                       : (rt == nullptr ? "null" : "texture")),
                        " ptr=", static_cast<const void*>(rt),
                        " overrides=", instance.textureSlotOverrides.size(),
                        " drawSkinExtra=", (instance.drawSkinExtra ? 1 : 0),
                        " verts=", gpuModel.data.vertices.size());
                }
                // Note on the Skin Extra batch, since it has been misread twice:
                // it is NOT a layer over the head. On an HD character model it
                // is its own section - 703 vertices and 634 triangles of the
                // human female's head, separate from the 403 and 877 of the
                // sections beside it - carrying the eyes, the mouth, the ears
                // and the eyelashes. Skipping it does not remove a detail pass,
                // it removes a face's features. Whatever is wrong with an NPC
                // face, it is not this batch existing.
                // M2 color-alpha animation gates prop submeshes per animation -
                // e.g. the peasant lumberjack carry model has two wood-bundle
                // submeshes and only one is alpha-1 in any given animation.
                // Opaque batches can't express alpha in the shader, so cull
                // batches whose track evaluates to ~0 for the current sequence.
                const float batchColorAlpha = glm::clamp(
                    evalBatchColorAlpha(gpuModel.data, batch,
                                        instance.currentSequenceIndex,
                                        instance.animationTime,
                                        instance.globalSequenceTime) *
                    evalBatchTextureWeight(gpuModel.data, batch,
                                           instance.currentSequenceIndex,
                                           instance.animationTime,
                                           instance.globalSequenceTime),
                    0.0f, 1.0f);
                if (batchColorAlpha <= 0.01f) {
                    continue;
                }
                // Resolve texture for this batch (prefer hair textures for hair geosets).
                // A two-stage batch binds its combo's first texture on stage 0
                // and its second on stage 1, as stock does (0x81F450).
                VkTexture* texPtr = nullptr;
                VkTexture* stage2Tex = whiteTexture_.get();
                const bool twoStages = effect && effect->stages > 1;
                if (twoStages) {
                    pipeline::M2Batch firstStage = batch;
                    firstStage.textureCount = 1;
                    texPtr = resolveBatchTexture(instance, gpuModel, firstStage);
                    const int32_t slot = effect->texture[1];
                    if (slot >= 0 && static_cast<size_t>(slot) < gpuModel.textureIds.size()) {
                        VkTexture* t = gpuModel.textureIds[static_cast<size_t>(slot)];
                        auto itO = instance.textureSlotOverrides.find(static_cast<uint16_t>(slot));
                        if (itO != instance.textureSlotOverrides.end() && itO->second != nullptr)
                            t = itO->second;
                        if (t && t->isValid()) stage2Tex = t;
                    }
                } else {
                    texPtr = resolveBatchTexture(instance, gpuModel, batch);
                }
                const uint16_t batchGroup = static_cast<uint16_t>(batch.submeshId / 100);
                auto groupTexIt = instance.groupTextureOverrides.find(batchGroup);
                if (groupTexIt != instance.groupTextureOverrides.end() && groupTexIt->second != nullptr) {
                    texPtr = groupTexIt->second;
                }

                // Respect M2 material blend mode for creature/character submeshes.
                uint16_t blendMode = 0;
                uint16_t materialFlags = 0;
                if (batch.materialIndex < gpuModel.data.materials.size()) {
                    blendMode = gpuModel.data.materials[batch.materialIndex].blendMode;
                    materialFlags = gpuModel.data.materials[batch.materialIndex].flags;
                }

                const uint16_t submeshGroup = static_cast<uint16_t>(batch.submeshId / 100);
                const bool hairTexture = batchUsesTextureType(gpuModel, batch, 6);
                const bool hairGeoset = (submeshGroup >= 1 && submeshGroup <= 3) ||
                                        (submeshGroup == 0 && batch.submeshId > 0 && batch.submeshId <= 99);
                // Scene models have no hair, and their submesh ids are all 0, which
                // would otherwise satisfy the hair-geoset guess for every batch.
                const bool hairMaterial = !instance.isSceneModel &&
                                          (hairTexture ||
                                           (hairGeoset && (blendMode != 0 || batch.textureCount > 1)));

                // An attached weapon's additive and modulate batches - the
                // glow cards and runes on a sword - are drawn, as the stock
                // client draws every batch of an attachment (m2-runtime #31).
                // They were dropped as "detached flat quads" before the blend
                // table, the second stage and layer folding were stock.
                // WOWEE_CHAR_DROP_WEAPON_FX=1 drops them again (enchant
                // visuals, being effect models, always draw).
                static const bool kDropWeaponFx = core::envFlagEnabled("WOWEE_CHAR_DROP_WEAPON_FX", false);
                if (kDropWeaponFx && instance.hasOverrideModelMatrix && !instance.isEffectModel &&
                    blendMode >= 3) {
                    continue;
                }

                // For body/equipment parts with white/fallback texture, use skin (type 1) texture.
                if (texPtr == whiteTexture_.get() && !instance.isSceneModel) {
                    uint16_t group = batchGroup;
                    // 1 and 2 (beard, moustache, chin) and 7 (ears) sample the
                    // body atlas as much as 3 does.
                    bool isSkinGroup = (group <= 5 || group == 7 ||
                                        group == 8 || group == 9 || group == 13);
                    if (isSkinGroup) {
                        uint32_t texType = 0;
                        if (batch.textureIndex < gpuModel.data.textureLookup.size()) {
                            uint16_t lk = gpuModel.data.textureLookup[batch.textureIndex];
                            if (lk < gpuModel.data.textures.size()) {
                                texType = gpuModel.data.textures[lk].type;
                            }
                        }
                        // Do NOT apply skin composite to hair (type 6) batches
                        if (texType != 6) {
                            for (size_t ti = 0; ti < gpuModel.textureIds.size(); ti++) {
                                VkTexture* candidate = gpuModel.textureIds[ti];
                                auto itO = instance.textureSlotOverrides.find(static_cast<uint16_t>(ti));
                                if (itO != instance.textureSlotOverrides.end() && itO->second != nullptr) {
                                    candidate = itO->second;
                                }
                                if (candidate != whiteTexture_.get() && candidate != nullptr) {
                                    if (ti < gpuModel.data.textures.size() &&
                                        (gpuModel.data.textures[ti].type == 1 || gpuModel.data.textures[ti].type == 11)) {
                                        texPtr = candidate;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }

                // Determine material properties
                bool alphaCutout = false;
                bool colorKeyBlack = false;
                if (texPtr != nullptr && texPtr != whiteTexture_.get()) {
                    auto pit = texturePropsByPtr_.find(texPtr);
                    if (pit != texturePropsByPtr_.end()) {
                        alphaCutout = pit->second.hasAlpha;
                        colorKeyBlack = pit->second.colorKeyBlack;
                    }
                }
                // A scene means what its materials say. Stormwind's walls are DXT5 with
                // an unused alpha channel - every texel below the 0.5 cutoff - so
                // inferring a cutout from "the texture has alpha" discards the whole
                // building and leaves the sky showing through it. Only an alpha-key
                // material (blendMode 1) cuts out here.
                const bool blendNeedsCutout = instance.isSceneModel
                    ? (blendMode == 1)
                    : ((blendMode == 1) ||
                       (blendMode == 0 && alphaCutout) ||
                       (blendMode >= 2 && !alphaCutout) ||
                       hairMaterial);
                // Stock: only render flag 0x01 is unlit (0x8201A9 moves the
                // diffuse into the emissive). Additive batches are lit like any
                // other. Effect models (glows hosted here for the preview)
                // emit their own light.
                const bool unlit = ((materialFlags & 0x01) != 0) || instance.isEffectModel;

                // Hair textures are authored as alpha-cut cards. If they use the
                // translucent pipeline they form a soft shell around the head.
                // M2Blend: 0 Opaque, 1 AlphaKey, 2 Alpha, 3 NoAlphaAdd, 4 Add,
                // 5 Mod, 6 Mod2x, 7 BlendAdd. (6/Mod2x is kept on additive as it
                // has been; the doodad path in M2Renderer already sends
                // everything above 2 there.)
                const bool additiveBlend = (blendMode == 3 || blendMode == 4 || blendMode == 7);
                // Mod and Mod2x multiply the framebuffer (DstColor blends,
                // 0xA453B0); like additive, a partial alpha does not move them
                // to alpha blending (rows 1 and 2 of the table keep them).
                const bool modulateBlend = (blendMode == 5 || blendMode == 6);

                VkPipeline desiredPipeline;
                if (instance.isEffectModel) {
                    // Enchant visuals are glow cards drawn on black. Their materials
                    // declare Mod/alpha blending, which would composite that black
                    // background as an opaque quad - force additive so only the light adds.
                    desiredPipeline = additivePipeline_;
                } else if (additiveBlend) {
                    // Decided before the fade branch below, not after it. An
                    // additive card fades by adding less light - matData.opacity
                    // already scales what it contributes - so a partial alpha is
                    // not a reason to divert it to the translucent pipeline, and
                    // diverting it composites the card's black backing as black.
                    //
                    // The Darnassus wisps are the case: their glow alpha is an
                    // animated pulse, so the diversion tripped on almost every
                    // frame the pulse was not at full, and the cards showed as
                    // black roughly half the time.
                    desiredPipeline = blendMode == 3 && addOnePipeline_ ? addOnePipeline_ : additivePipeline_;
                } else if (modulateBlend) {
                    desiredPipeline = blendMode == 6 ? mod2xPipeline_ : modPipeline_;
                    if (!desiredPipeline) desiredPipeline = alphaPipeline_;
                } else if (instance.opacity * batchColorAlpha < 0.999f) {
                    // Whole-instance fade (ghost form, spawn fade-in): the opaque and
                    // alpha-test pipelines have blending disabled, so the shader's
                    // texColor.a * opacity output is discarded and only hair (via
                    // alpha-to-coverage) ever looked translucent. Route every batch
                    // through the blend pipeline; the per-batch alphaTest UBO flag
                    // still handles cutout materials in the shader.
                    //
                    // Except that a batch which must not write depth still must
                    // not, fading or otherwise. translucentPipeline_ writes
                    // depth so a fading character's own solid parts keep sorting
                    // against each other; alphaPipeline_ is the same blend with
                    // the depth write off. Sending a translucent card down the
                    // writing one puts an invisible occluder in front of
                    // whatever it covers, which is how the glue screens' cloud
                    // and haze cards were punching a rectangle out of the
                    // character standing behind them - the card's own materials
                    // ask for no depth write (0x10) and the scenes animate their
                    // alpha, so almost every frame took this branch.
                    const bool noDepthWrite = (blendMode >= 2) || ((materialFlags & 0x10) != 0);
                    desiredPipeline = noDepthWrite ? alphaPipeline_ : translucentPipeline_;
                } else if (hairMaterial) {
                    desiredPipeline = alphaTestPipeline_;
                } else {
                    // additiveBlend (3/4/6/7) is handled above; only the
                    // non-additive modes reach here. 4/Add is the one that used
                    // to fall through to the alpha pipeline, which composited a
                    // glow card's black backing as an actual black quad - every
                    // material on Wisp.m2 declares it.
                    switch (blendMode) {
                        case 0: desiredPipeline = opaquePipeline_; break;
                        case 1: desiredPipeline = alphaTestPipeline_; break;
                        case 2: desiredPipeline = alphaPipeline_; break;
                        default: desiredPipeline = alphaPipeline_; break;
                    }
                }
                if (desiredPipeline != currentPipeline) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
                    currentPipeline = desiredPipeline;
                }

                // Unlit is the texture at its own brightness (stock: texture x
                // M2Color), not doubled: the shader draws tex * (1 + tint * boost).
                float emissiveBoost = 0.0f;
                glm::vec3 emissiveTint(1.0f, 1.0f, 1.0f);
                const bool koboldCandleFlame = colorKeyBlack && gpuModel.isKoboldFlame;
                if (unlit && koboldCandleFlame) {
                    float phase = static_cast<float>(batch.submeshId) * 0.31f;
                    float f1 = std::sin(frameTimeSeconds * 7.9f + phase);
                    float f2 = std::sin(frameTimeSeconds * 12.7f + phase * 1.73f);
                    float f3 = std::sin(frameTimeSeconds * 4.3f + phase * 2.11f);
                    float flicker = 0.90f + 0.10f * f1 + 0.06f * f2 + 0.04f * f3;
                    flicker = std::clamp(flicker, 0.72f, 1.12f);
                    emissiveBoost = (blendMode >= 3) ? (2.4f * flicker) : (1.5f * flicker);
                    emissiveTint = glm::vec3(1.28f, 1.04f, 0.82f);
                }
                if (instance.isSceneModel && unlit) {
                    // The shader draws an unlit batch at tex * (1 + tint *
                    // boost), which is twice the texture by default. A glue
                    // scene's unlit sky, clouds and glows are the texture at
                    // its own brightness, times the batch's M2Color: so the
                    // tint is that colour less one. The login scene's and the
                    // human scene's skies were drawn doubled.
                    glm::vec3 color(1.0f);
                    if (batch.colorIndex != 0xFFFF && batch.colorIndex < gpuModel.data.colorRGB.size())
                        color = glm::clamp(gpuModel.data.colorRGB[batch.colorIndex], glm::vec3(0.0f), glm::vec3(1.0f));
                    emissiveBoost = 1.0f;
                    emissiveTint = color - glm::vec3(1.0f);
                }

                // Resolve normal/height map for this texture
                VkTexture* normalMap = flatNormalTexture_.get();
                float batchHeightVariance = 0.0f;
                if (texPtr && texPtr != whiteTexture_.get()) {
                    auto nmIt = normalMapByTexPtr_.find(texPtr);
                    if (nmIt != normalMapByTexPtr_.end()) {
                        normalMap = nmIt->second.normalMap;
                        batchHeightVariance = nmIt->second.heightMapVariance;
                    }
                }

                // POM quality → sample count
                const int pomSamples = pomSamplesFor(pomQuality_);
                const bool useAdvancedMaterials = !previewMainModel;
                const bool usePreviewSimpleShader = previewMainModel;

                // Create per-batch material UBO
                CharMaterialUBO matData{};
                matData.opacity = instance.opacity * batchColorAlpha;
                matData.alphaTest = blendNeedsCutout ? 1 : 0;
                matData.colorKeyBlack = colorKeyBlack ? 1 : 0;
                matData.unlit = unlit ? 1 : 0;
                // Bit 1: M2 render flag 0x2, unfogged. Only a glue scene has
                // fog worth skipping (its sky dome and cloud cards carry it),
                // so only a scene sets it.
                if (instance.isSceneModel && (materialFlags & 0x02) != 0) matData.unlit |= 2;
                matData.emissiveBoost = emissiveBoost;
                matData.emissiveTintR = emissiveTint.r;
                matData.emissiveTintG = emissiveTint.g;
                matData.emissiveTintB = emissiveTint.b;
                // Stock 3.3.5 lights characters with diffuse plus ambient only:
                // no specular and no normal map. The generated maps came from
                // the composited skin itself, so sunlight embossed every seam
                // and baked NPC texture (worst on legs and sleeves).
                matData.specularIntensity = 0.0f;
                matData.enableNormalMap = 0;
                (void)useAdvancedMaterials;
                // Character textures are layered/mirrored atlases, not coherent
                // height fields. Parallax offsets cross face seams and reveal
                // underlying armor through tabards, so keep POM out of the
                // character path even when it is enabled for the world.
                matData.enablePOM = 0;
                matData.pomScale = 0.06f;
                matData.pomMaxSamples = pomSamples;
                matData.heightMapVariance = useAdvancedMaterials ? batchHeightVariance : 0.0f;
                matData.normalMapStrength = normalMapStrength_;
                matData.hairMaterial = hairMaterial ? 1 : 0;
                matData.ambientMul = instance.isSceneModel ? 1.0f : unitAmbientMul_;
                matData.highlight = (pair.first != 0 &&
                                     (pair.first == highlightA_ || pair.first == highlightB_)) ? 1.0f : 0.0f;
                matData.combiner = effect ? static_cast<int32_t>(effect->combiner) : 1;
                matData.texStages = effect ? effect->stages : 1;
                matData.blendMode = blendMode;
                matData.diffuseTintR = instance.diffuseTint.r;
                matData.diffuseTintG = instance.diffuseTint.g;
                matData.diffuseTintB = instance.diffuseTint.b;
                if (indoorProbesOn_ && instance.probeLive && !instance.isSceneModel) {
                    matData.probeR = instance.probeColour.r;
                    matData.probeG = instance.probeColour.g;
                    matData.probeB = instance.probeColour.b;
                    matData.probeW = 1.0f + std::clamp(instance.probeIndoor, 0.0f, 1.0f);
                    matData.probeLeanX = instance.probeLean.x;
                    matData.probeLeanY = instance.probeLean.y;
                }

                // The base humanoid mesh samples a mirrored character atlas,
                // with the face sitting directly beside a UV seam. Parallax
                // displacement moves each mirrored half in opposite screen
                // directions, producing crossed eyes and a center-squashed
                // mouth at oblique angles. Keep normal lighting, but never
                // displace UVs on group 0 body/head surfaces - nor on the
                // no-cloak panel, which is a patch of the same torso.
                if (batchGroup == 0 || batch.submeshId == core::kGeosetNoCape) {
                    normalMap = flatNormalTexture_.get();
                    matData.specularIntensity = 0.0f;
                    matData.enableNormalMap = 0;
                    matData.enablePOM = 0;
                    matData.heightMapVariance = 0.0f;
                }

                // Tabards are a thin cloth layer over the torso. Reusing the
                // body composite's generated height/normal response makes the
                // armor underneath appear embossed and reflective through the
                // cloth as the camera moves. Keep the diffuse tabard artwork,
                // but give group 12 a flat, low-specular cloth material.
                if (batchGroup == 12) {
                    normalMap = flatNormalTexture_.get();
                    matData.specularIntensity = 0.0f;
                    matData.enableNormalMap = 0;
                    matData.enablePOM = 0;
                    matData.heightMapVariance = 0.0f;
                }
                if (usePreviewSimpleShader) {
                    matData.enableNormalMap = 0;
                    matData.enablePOM = kPreviewSimpleTextureMode;
                    matData.heightMapVariance = 0.0f;
                }

                // WOWEE_SCENE_DIAG=1 dumps what each backdrop batch is actually told to
                // draw - texture, blend mode, shader path - once per scene model.
                static int sceneDiagLines = 0;
                if (instance.isSceneModel && sceneDiagEnabled() && sceneDiagLines++ < 40) {
                    std::string texName = "<white>";
                    if (batch.textureIndex < gpuModel.data.textureLookup.size()) {
                        uint16_t lk = gpuModel.data.textureLookup[batch.textureIndex];
                        if (lk < gpuModel.data.textures.size())
                            texName = gpuModel.data.textures[lk].filename;
                    }
                    // Warning level: the diagnostic is opt-in already, and the file log
                    // filters info out by default.
                    core::Logger::getInstance().warning(
                        "SCENE DIAG batch submesh=", batch.submeshId,
                        " blend=", blendMode, " matFlags=0x", std::hex, materialFlags, std::dec,
                        " alphaTest=", matData.alphaTest,
                        " unlit=", matData.unlit,
                        " simplePath=", (matData.enablePOM == kPreviewSimpleTextureMode ? 1 : 0),
                        " whiteFallback=", (texPtr == whiteTexture_.get() ? 1 : 0),
                        " tex=", texName);
                }

                // Sub-allocate material UBO from ring buffer
                uint32_t matOffset = materialRingOffset_[frameSlot];
                if (matOffset + uboStride > ringCapacityBytes) continue; // ring exhausted
                memcpy(static_cast<char*>(materialRingMapped_[frameSlot]) + matOffset, &matData, sizeof(CharMaterialUBO));
                materialRingOffset_[frameSlot] = matOffset + uboStride;

                VkTexture* bindTex = (texPtr && texPtr->isValid()) ? texPtr : whiteTexture_.get();
                VkDescriptorSet materialSet = getMaterialDescriptorSet(bindTex, normalMap, stage2Tex);
                if (!materialSet) continue;

                // Bind material descriptor set (set 1)
                const uint32_t dynamicOffset = matOffset;
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipelineLayout_, 1, 1, &materialSet, 1, &dynamicOffset);

                // Per-batch depth bias from materialLayer to separate coplanar
                // armor pieces (chest/legs/gloves) that share identical depth.
                vkCmdSetDepthBias(cmd, static_cast<float>(batch.materialLayer) * 0.5f, 0.0f, 0.0f);

                // M2 texture animation: the batch's UV translation track. The
                // glue scenes scroll their fog, clouds, water and the death
                // knight's floor glow this way, and creatures (elementals,
                // waterfalls on models) use it too; this renderer used to
                // ignore it, so all of those stood still.
                // Each texture stage has its own transform and coordinate
                // source (m2_effect_select.hpp): xy is the first stage's
                // scroll, zw the second's, and the ivec4 says where each
                // stage's coordinates come from (UV0, UV1 or the env map).
                {
                    glm::vec4 uvOffset(0.0f);
                    const auto& md = gpuModel.data;
                    auto scrollOf = [&](int32_t ti) -> glm::vec2 {
                        if (ti < 0 || static_cast<size_t>(ti) >= md.textureTransforms.size())
                            return glm::vec2(0.0f);
                        const glm::vec3 t = m2_track::sampleVec3(
                            md.textureTransforms[static_cast<size_t>(ti)].translation,
                            instance.currentSequenceIndex, instance.animationTime,
                            instance.globalSequenceTime, md.globalSequenceDurations,
                            glm::vec3(0.0f));
                        return glm::vec2(t.x, t.y);
                    };
                    if (batch.textureAnimIndex != 0xFFFF &&
                        batch.textureAnimIndex < md.textureTransformLookup.size()) {
                        const glm::vec2 s0 = scrollOf(md.textureTransformLookup[batch.textureAnimIndex]);
                        uvOffset.x = s0.x;
                        uvOffset.y = s0.y;
                    }
                    int32_t stages = 0;
                    if (effect) {
                        stages = static_cast<int32_t>(effect->uv[0]) |
                                 (static_cast<int32_t>(effect->uv[1]) << 2);
                        if (effect->stages > 1) {
                            const glm::vec2 s1 = scrollOf(effect->transform[1]);
                            uvOffset.z = s1.x;
                            uvOffset.w = s1.y;
                        }
                    }
                    // The robe sway (E7) reads its offsets on the robe's own
                    // batches only; everything else is skinned as stock.
                    if (instance.hemActive && batch.submeshId >= 1302 && batch.submeshId < 1400)
                        stages |= kStagesRobeHem;
                    if (instance.capeActive && batch.submeshId > 1501 && batch.submeshId < 1600)
                        stages |= kStagesCape;
                    if (uvOffset != pushedUvOffset || stages != pushedStages) {
                        const int32_t sway = ((stages & kStagesRobeHem) ? 1 : 0) | ((stages & kStagesCape) ? 2 : 0);
                        struct { glm::vec4 uv; glm::ivec4 stages; } push{
                            uvOffset, glm::ivec4(stages & ~(kStagesRobeHem | kStagesCape), sway, 0, 0)};
                        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                                           sizeof(glm::mat4), sizeof(push), &push);
                        pushedUvOffset = uvOffset;
                        pushedStages = stages;
                    }
                }

                vkCmdDrawIndexed(cmd, batch.indexCount, 1, batch.indexStart, 0, 0);
            }
            } // end pass loop
        } else {
            // Draw entire model with first texture
            VkTexture* texPtr = !gpuModel.textureIds.empty() ? gpuModel.textureIds[0] : whiteTexture_.get();
            if (!texPtr || !texPtr->isValid()) texPtr = whiteTexture_.get();

            // POM quality → sample count
            const int pomSamples2 = pomSamplesFor(pomQuality_);

            // Whole-model fallback inherits whatever pipeline was bound last;
            // pick it explicitly so instance fades blend here too.
            VkPipeline fallbackPipeline = (instance.opacity < 0.999f)
                ? translucentPipeline_ : opaquePipeline_;
            if (fallbackPipeline != currentPipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fallbackPipeline);
                currentPipeline = fallbackPipeline;
            }

            CharMaterialUBO matData{};
            matData.combiner = 1;  // Combiners_Mod: the texture as it is
            matData.texStages = 1;
            matData.opacity = instance.opacity;
            matData.alphaTest = 0;
            matData.colorKeyBlack = 0;
            matData.unlit = 0;
            matData.emissiveBoost = 1.0f;
            matData.emissiveTintR = 1.0f;
            matData.emissiveTintG = 1.0f;
            matData.emissiveTintB = 1.0f;
            matData.specularIntensity = 0.5f;
            const bool previewMainModel = renderPassOverride_ != VK_NULL_HANDLE &&
                                          !previewLit_ &&
                                          !instance.hasOverrideModelMatrix;
            const bool useAdvancedMaterials = !previewMainModel;
            const bool usePreviewSimpleShader = previewMainModel;
            matData.enableNormalMap = (useAdvancedMaterials && normalMappingEnabled_) ? 1 : 0;
            matData.enablePOM = (useAdvancedMaterials && pomEnabled_) ? 1 : 0;
            matData.pomScale = 0.06f;
            matData.pomMaxSamples = pomSamples2;
            matData.heightMapVariance = 0.0f;
            matData.normalMapStrength = normalMapStrength_;
            matData.hairMaterial = 0;
            matData.ambientMul = unitAmbientMul_;
            matData.highlight = 0.0f;
            matData.diffuseTintR = matData.diffuseTintG = matData.diffuseTintB = 1.0f;
            if (usePreviewSimpleShader) {
                matData.enableNormalMap = 0;
                matData.enablePOM = kPreviewSimpleTextureMode;
            }

            // Sub-allocate material UBO from ring buffer
            uint32_t matOffset2 = materialRingOffset_[frameSlot];
            if (matOffset2 + uboStride > ringCapacityBytes) continue; // ring exhausted
            memcpy(static_cast<char*>(materialRingMapped_[frameSlot]) + matOffset2, &matData, sizeof(CharMaterialUBO));
            materialRingOffset_[frameSlot] = matOffset2 + uboStride;

            VkDescriptorSet materialSet =
                getMaterialDescriptorSet(texPtr, flatNormalTexture_.get(), whiteTexture_.get());
            if (!materialSet) continue;

            const uint32_t dynamicOffset = matOffset2;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLayout_, 1, 1, &materialSet, 1, &dynamicOffset);

            vkCmdDrawIndexed(cmd, gpuModel.indexCount, 1, 0, 0, 0);
        }
    }
}

bool CharacterRenderer::initializeShadow(VkRenderPass shadowRenderPass) {
    if (!vkCtx_ || shadowRenderPass == VK_NULL_HANDLE) return false;
    VkDevice device = vkCtx_->getDevice();

    // ShadowCharParams UBO (matches character_shadow.frag.glsl set=1 binding=1)
    struct ShadowCharParams {
        int32_t alphaTest = 0;
        int32_t colorKeyBlack = 0;
    };

    // The same set the other three shadow passes bind - a sampler and a small
    // uniform buffer - with this pass's own params behind binding 1.
    if (!createShadowParamsSet(device, vkCtx_->getAllocator(), sizeof(ShadowCharParams),
                               whiteTexture_->getImageView(), whiteTexture_->getSampler(),
                               "CharacterRenderer", shadowParams_)) {
        return false;
    }

    // Alpha testing on, once, because it never varies here.
    //
    // The buffer is zero-filled at creation and was then never written, so the
    // flag read 0 and every caster stamped its bounding rectangle into the
    // shadow map - hair, capes and cloaks cast solid slabs instead of their own
    // outline. With it on, an opaque batch is bound to the white fallback whose
    // alpha is 1 and passes the cutoff untouched, while an alpha-keyed batch is
    // bound to its own texture below and cuts properly.
    {
        VmaAllocationInfo paramsInfo{};
        vmaGetAllocationInfo(vkCtx_->getAllocator(), shadowParams_.alloc, &paramsInfo);
        if (paramsInfo.pMappedData) {
            ShadowCharParams p{};
            p.alphaTest = 1;
            p.colorKeyBlack = 0;
            std::memcpy(paramsInfo.pMappedData, &p, sizeof(p));
        }
    }

    // One texture-set pool per frame in flight, reset at the top of each
    // frame's shadow pass.
    {
        VkDescriptorPoolSize texPoolSizes[2]{};
        texPoolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texPoolSizes[0].descriptorCount = 256;
        texPoolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        texPoolSizes[1].descriptorCount = 256;
        VkDescriptorPoolCreateInfo texPoolCI{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        texPoolCI.maxSets = 256;
        texPoolCI.poolSizeCount = 2;
        texPoolCI.pPoolSizes = texPoolSizes;
        for (uint32_t f = 0; f < kShadowTexPoolFrames; ++f) {
            if (vkCreateDescriptorPool(device, &texPoolCI, nullptr, &shadowTexPool_[f]) != VK_SUCCESS) {
                LOG_ERROR("CharacterRenderer: failed to create shadow texture pool ", f);
                return false;
            }
        }
    }

    // Pipeline layout: set 0 = shadowParams_.layout, set 1 = boneSetLayout_
    //
    // There used to be a third, perFrameLayout_, sitting at set 0 as a dummy
    // that nothing ever bound, which pushed the params to set 1 and the bones to
    // set 2. That made this the odd one out of the four passes sharing the
    // shadow render pass: terrain, WMO and M2 all bind their params at set 0
    // under a layout of their own. Binding set 1 while set 0 still carried
    // another pass's incompatible layout left the params disturbed rather than
    // bound, and the fragment shader read its alpha-test flags from nothing -
    // thousands of times a session, which GPU-assisted validation reports as
    // indexing a descriptor array of length zero. A set that is never bound has
    // no business being in the layout.
    // Push constant: 128 bytes (lightSpaceMatrix + model), VERTEX stage
    VkDescriptorSetLayout setLayouts[] = {shadowParams_.layout, boneSetLayout_};
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pc.offset = 0;
    pc.size = sizeof(ShadowPush);  // one combined matrix, plus the sway slot
    VkPipelineLayoutCreateInfo plCI{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plCI.setLayoutCount = 2;
    plCI.pSetLayouts = setLayouts;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(device, &plCI, nullptr, &shadowPipelineLayout_) != VK_SUCCESS) {
        LOG_ERROR("CharacterRenderer: failed to create shadow pipeline layout");
        return false;
    }

    // Load character shadow shaders
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/character_shadow.vert.spv")) {
        LOG_ERROR("CharacterRenderer: failed to load character_shadow.vert.spv");
        return false;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/character_shadow.frag.spv")) {
        LOG_ERROR("CharacterRenderer: failed to load character_shadow.frag.spv");
        vertShader.destroy();
        return false;
    }

    // Character vertex format (CharVertexGPU): stride = 64 bytes
    // loc 0: vec3 aPos          (R32G32B32_SFLOAT, offset 0)
    // loc 1: vec4 aBoneWeights  (R8G8B8A8_UNORM,   offset 12)
    // loc 2: uvec4 aBoneIndices (R8G8B8A8_UINT,    offset 16)
    // loc 3: vec2 aTexCoord     (R32G32_SFLOAT,    offset 32)
    VkVertexInputBindingDescription vertBind{};
    vertBind.binding = 0;
    vertBind.stride = static_cast<uint32_t>(sizeof(CharVertexGPU));
    vertBind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::vector<VkVertexInputAttributeDescription> vertAttrs = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, position))},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UNORM,   .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, boneWeights))},
        {.location = 2, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UINT,    .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, boneIndices))},
        {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,    .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, texCoords))},
    };

    shadowPipeline_ = buildShadowPipeline(
        device, vkCtx_->getPipelineCache(),
        vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
        fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT),
        vertBind, vertAttrs, shadowPipelineLayout_, shadowRenderPass,
        vkCtx_->useDynamicRendering());

    vertShader.destroy();
    fragShader.destroy();

    if (!shadowPipeline_) {
        LOG_ERROR("CharacterRenderer: failed to create shadow pipeline");
        return false;
    }
    LOG_INFO("CharacterRenderer shadow pipeline initialized");
    return true;
}

bool CharacterRenderer::uploadBones(CharacterInstance& inst, uint32_t frameIndex, int numBones) {
    static const bool reuse = core::envFlagEnabled("WOWEE_CHAR_BONE_REUSE", true);
    if (frameIndex >= 2 || numBones <= 0 || !inst.boneMapped[frameIndex]) return false;
    // Each slot's buffer is written only here (and set to identity when it is
    // made), so a slot that took this pose last still holds it.
    if (reuse && inst.uploadedPoseSerial[frameIndex] == inst.poseSerial) return false;
    memcpy(inst.boneMapped[frameIndex], inst.boneMatrices.data(), numBones * sizeof(glm::mat4));
    if (inst.hemActive && numBones <= kCapeSlot)
        static_cast<glm::mat4*>(inst.boneMapped[frameIndex])[kHemSlot] = inst.hemData;
    if (inst.capeActive && numBones <= kCapeSlot)
        static_cast<glm::mat4*>(inst.boneMapped[frameIndex])[kCapeSlot] = inst.capeData;
    inst.uploadedPoseSerial[frameIndex] = inst.poseSerial;
    return true;
}

void CharacterRenderer::renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                                     const glm::vec3& shadowCenter, float shadowRadius,
                                     std::vector<glm::vec4>* casterSpheres,
                                     const ShadowReceiverView* seen) {
    if (!shadowPipeline_ || !shadowParams_.set) return;
    if (instances.empty() || models.empty()) return;
    if (boneDescPool_ == VK_NULL_HANDLE || boneSetLayout_ == VK_NULL_HANDLE) return;

    uint32_t frameIndex = vkCtx_->getCurrentFrame();
    if (frameIndex >= 2) return;
    VkDevice device = vkCtx_->getDevice();

    // WOWEE_CHAR_SHADOW_LIST=0 walks the map and filters every batch every
    // frame; WOWEE_CHAR_SHADOW_CULL=0 draws every unit in range wherever the
    // light's box is.
    static const bool listOn = core::envFlagEnabled("WOWEE_CHAR_SHADOW_LIST", true);
    static const bool cullOn = core::envFlagEnabled("WOWEE_CHAR_SHADOW_CULL", true);
    static const bool profileOn = core::envFlagEnabled("WOWEE_FRAME_PROFILE", false);

    // This frame slot's fence was waited on in beginFrame, so last time's sets
    // are finished with and the pool can be handed back whole.
    if (frameIndex < kShadowTexPoolFrames && shadowTexPool_[frameIndex]) {
        vkResetDescriptorPool(device, shadowTexPool_[frameIndex], 0);
    }
    shadowTexSetCache_.clear();

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);

    if (listOn && shadowInstancesGeneration_ != instancesGeneration_) {
        shadowInstances_.clear();
        shadowInstances_.reserve(instances.size());
        for (auto& pair : instances) shadowInstances_.push_back(&pair.second);
        shadowInstancesGeneration_ = instancesGeneration_;
    }

    // Which of a model's batches cast, and how: -1 not at all, 0 solid, 1 in
    // the shape of its texture. Transparent batches cast nothing; with no
    // geosets chosen, groups 17 and 18 are left out.
    const auto shadowBatchKind = [](const CharacterInstance& inst, const M2ModelGPU& gpuModel,
                                    const pipeline::M2Batch& batch) -> int {
        uint16_t blendMode = 0;
        if (batch.materialIndex < gpuModel.data.materials.size()) {
            blendMode = gpuModel.data.materials[batch.materialIndex].blendMode;
        }
        if (blendMode >= 2) return -1; // skip transparent
        const bool applyGeosetFilter = !inst.activeGeosets.empty();
        if (applyGeosetFilter &&
            inst.activeGeosets.find(batch.submeshId) == inst.activeGeosets.end()) return -1;
        if (!applyGeosetFilter && gpuModel.hasCompositeSlots) {
            uint16_t grp = batch.submeshId / 100;
            if (grp == 17 || grp == 18) return -1;
        }
        return blendMode == 1 ? 1 : 0;
    };

    const float shadowRadiusSq = shadowRadius * shadowRadius;
    // Which set is bound at 0 right now, so a run of opaque batches does not
    // rebind the same fallback for each one.
    VkDescriptorSet currentTexSet = VK_NULL_HANDLE;
    uint64_t walked = 0, inRange = 0, lightCulled = 0, viewCulled = 0, drawn = 0, boneCopies = 0, boneReuses = 0;
    const auto castShadow = [&](CharacterInstance& inst) {
        ++walked;
        if (!inst.visible) return;

        // Distance cull against shadow frustum
        glm::vec3 diff = inst.position - shadowCenter;
        if (glm::dot(diff, diff) > shadowRadiusSq) return;
        ++inRange;

        if (!inst.cachedModel) return;
        const M2ModelGPU& gpuModel = *inst.cachedModel;
        if (!gpuModel.vertexBuffer) return;

        glm::mat4 modelMat = inst.hasOverrideModelMatrix
            ? inst.overrideModelMatrix
            : getModelMatrix(inst);

        // Against the light's own box. The distance test above is a sphere
        // round the player; the map is a box across the sun, so a unit in
        // range off its side or past either depth plane draws nothing, every
        // triangle clipped. The sphere is the main pass's culling sphere
        // (bounds radius, at least 4 yards) grown by the model's height
        // twice over - the origin is at the feet, and a swing reaches past
        // the bind-pose bounds - at whatever scale the matrix carries.
        // Scene backdrops (origin far from their geometry) and models with
        // no bounds are never culled here.
        // A unit with no bound radius in its header still has a box, mostly;
        // with neither it is taken to be about a person's size.
        float maskRadius = 3.0f;
        glm::vec3 maskCentre = glm::vec3(modelMat * glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
        if (casterSpheres && (!cullOn || inst.isSceneModel || gpuModel.data.boundRadius <= 0.01f)) {
            const glm::vec3 lo = gpuModel.data.boundMin, hi = gpuModel.data.boundMax;
            if (inst.isSceneModel) {
                maskRadius = 1e4f;  // a backdrop: every cell
            } else if (glm::all(glm::lessThan(lo, hi))) {
                const float s = std::max({glm::length(glm::vec3(modelMat[0])),
                                          glm::length(glm::vec3(modelMat[1])),
                                          glm::length(glm::vec3(modelMat[2]))});
                maskCentre = glm::vec3(modelMat * glm::vec4((lo + hi) * 0.5f, 1.0f));
                maskRadius = std::max(1.0f, 0.5f * glm::length(hi - lo) * s * 1.25f);
            }
        }
        if (cullOn && !inst.isSceneModel && gpuModel.data.boundRadius > 0.01f) {
            const float matScale = std::max({glm::length(glm::vec3(modelMat[0])),
                                             glm::length(glm::vec3(modelMat[1])),
                                             glm::length(glm::vec3(modelMat[2]))});
            const float bodyR = gpuModel.data.boundRadius * matScale;
            const float reach = std::max(4.0f, bodyR) + 2.0f * bodyR;
            // The unit map's mask wants the outline, not the cull's slack: in
            // an orthographic light what a body shades lies under its own
            // bounds - the box's sphere, round the box's centre, with a
            // quarter more for a swing past the bind pose.
            const glm::vec3 lo = gpuModel.data.boundMin, hi = gpuModel.data.boundMax;
            maskRadius = std::max(1.0f, bodyR * 1.25f);
            if (glm::all(glm::lessThan(lo, hi))) {
                maskCentre = glm::vec3(modelMat * glm::vec4((lo + hi) * 0.5f, 1.0f));
                maskRadius = std::min(maskRadius, std::max(1.0f, 0.5f * glm::length(hi - lo) * matScale * 1.25f));
            }
            if (sphereOutsideOrthoClip(lightSpaceMatrix, glm::vec3(modelMat[3]), reach)) {
                ++lightCulled;
                return;
            }
        }
        // A unit whose shadow cannot reach anything the camera sees (behind
        // it, mostly) is left out: no bones copied, no draws, no mask cells.
        // It counts as cast, so the blob does not stand in for it either -
        // the blob lies at its feet, inside the same capsule. A yard more for
        // the filter's reach and the receivers' offsets.
        if (seen && !inst.isSceneModel && maskRadius < 1e3f &&
            !shadowCasterSeen(*seen, maskCentre, maskRadius + 1.0f)) {
            ++viewCulled;
            inst.sunShadowStamp = sunShadowStamp_;
            return;
        }

        // Ensure bone SSBO is allocated and upload bone matrices
        int numBones = std::min(static_cast<int>(inst.boneMatrices.size()), MAX_BONES);
        if (numBones > 0) {
            if (!inst.boneBuffer[frameIndex]) {
                VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bci.size = MAX_BONES * sizeof(glm::mat4);
                bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                VmaAllocationCreateInfo aci{};
                aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
                aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo ai{};
                if (vmaCreateBuffer(vkCtx_->getAllocator(), &bci, &aci,
                    &inst.boneBuffer[frameIndex], &inst.boneAlloc[frameIndex], &ai) != VK_SUCCESS) {
                    inst.boneBuffer[frameIndex] = VK_NULL_HANDLE;
                    inst.boneAlloc[frameIndex] = VK_NULL_HANDLE;
                    inst.boneMapped[frameIndex] = nullptr;
                    return;
                }
                inst.boneMapped[frameIndex] = ai.pMappedData;
                inst.uploadedPoseSerial[frameIndex] = 0;  // holds no pose yet

                // Initialize all bone slots to identity so out-of-range indices
                // produce correct (neutral) transforms instead of GPU garbage
                if (inst.boneMapped[frameIndex]) {
                    auto* dst = static_cast<glm::mat4*>(inst.boneMapped[frameIndex]);
                    for (int j = 0; j < MAX_BONES; j++) dst[j] = glm::mat4(1.0f);
                }

                VkDescriptorSetAllocateInfo dsAI{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                dsAI.descriptorPool = boneDescPool_;
                dsAI.descriptorSetCount = 1;
                dsAI.pSetLayouts = &boneSetLayout_;
                VkResult dsRes = vkAllocateDescriptorSets(device, &dsAI, &inst.boneSet[frameIndex]);
                if (dsRes != VK_SUCCESS) {
                    LOG_ERROR("CharacterRenderer[shadow]: bone descriptor allocation failed (instance=",
                              inst.id, ", frame=", frameIndex, ", vk=", static_cast<int>(dsRes), ")");
                    if (inst.boneBuffer[frameIndex]) {
                        vmaDestroyBuffer(vkCtx_->getAllocator(),
                                         inst.boneBuffer[frameIndex], inst.boneAlloc[frameIndex]);
                        inst.boneBuffer[frameIndex] = VK_NULL_HANDLE;
                        inst.boneAlloc[frameIndex] = VK_NULL_HANDLE;
                        inst.boneMapped[frameIndex] = nullptr;
                    }
                }

                if (inst.boneSet[frameIndex]) {
                    VkDescriptorBufferInfo bInfo{};
                    bInfo.buffer = inst.boneBuffer[frameIndex];
                    bInfo.offset = 0;
                    bInfo.range = bci.size;
                    VkWriteDescriptorSet w{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    w.dstSet = inst.boneSet[frameIndex];
                    w.dstBinding = 0;
                    w.descriptorCount = 1;
                    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    w.pBufferInfo = &bInfo;
                    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
                }
            }
            if (inst.boneMapped[frameIndex]) {
                if (uploadBones(inst, frameIndex, numBones)) ++boneCopies;
                else ++boneReuses;
            }
        }

        if (!inst.boneSet[frameIndex]) return;
        ++drawn;
        inst.sunShadowStamp = sunShadowStamp_;
        if (casterSpheres) casterSpheres->emplace_back(maskCentre, maskRadius);

        // Params at set 1 and bones at set 2, together, per instance.
        //
        // Binding set 1 once before the loop was not enough. This pass runs
        // after the M2 shadow pass, which binds its own set at set 0 under a
        // different pipeline layout, and a descriptor set bound while a lower
        // set carries an incompatible layout is disturbed rather than kept. The
        // fragment shader then read set 1 binding 1 - its alpha-test flags -
        // from a set the GPU no longer considered bound, which GPU-assisted
        // validation reports as indexing a descriptor array of length zero,
        // thousands of times a session.
        VkDescriptorSet sets[2] = {shadowParams_.set, inst.boneSet[frameIndex]};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_,
            0, 2, sets, 0, nullptr);
        currentTexSet = shadowParams_.set;

        ShadowPush push{.lightSpaceModel = lightSpaceMatrix * modelMat};
        vkCmdPushConstants(cmd, shadowPipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(ShadowPush), &push);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &gpuModel.vertexBuffer, &offset);
        vkCmdBindIndexBuffer(cmd, gpuModel.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

        const auto drawBatch = [&](const pipeline::M2Batch& batch, bool alphaKeyed,
                                   uint32_t indexStart, uint32_t indexCount) {
            // An alpha-keyed batch casts the shape of its texture; everything
            // else keeps the white fallback and casts solid. Only the set at
            // binding 0 changes, so the bones stay bound from above.
            VkDescriptorSet texSet = shadowParams_.set;
            if (alphaKeyed) {
                VkTexture* tex = resolveBatchTexture(inst, gpuModel, batch);
                if (tex && tex != whiteTexture_.get()) {
                    texSet = shadowTexDescSet(tex, frameIndex);
                }
            }
            if (texSet != currentTexSet) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_,
                                        0, 1, &texSet, 0, nullptr);
                currentTexSet = texSet;
            }

            vkCmdDrawIndexed(cmd, indexCount, 1, indexStart, 0, 0);
        };

        const auto& batches = gpuModel.data.batches;
        if (!listOn) {
            for (const auto& batch : batches) {
                const int kind = shadowBatchKind(inst, gpuModel, batch);
                if (kind >= 0) drawBatch(batch, kind == 1, batch.indexStart, batch.indexCount);
            }
            return;
        }
        if (!inst.shadowBatchesValid) {
            inst.shadowBatches.clear();
            for (uint32_t b = 0; b < batches.size(); ++b) {
                const int kind = shadowBatchKind(inst, gpuModel, batches[b]);
                if (kind < 0) continue;
                const auto& batch = batches[b];
                auto& list = inst.shadowBatches;
                if (kind == 0 && !list.empty() && !list.back().alphaKeyed) {
                    auto& last = list.back();
                    // A second texture layer over the same triangles casts
                    // nothing the first did not.
                    if (batch.indexStart >= last.indexStart &&
                        batch.indexStart + batch.indexCount <= last.indexStart + last.indexCount) continue;
                    if (last.indexStart + last.indexCount == batch.indexStart) {
                        last.indexCount += batch.indexCount;  // runs on from the last solid one
                        continue;
                    }
                }
                list.push_back({.batch = b, .alphaKeyed = kind == 1,
                                .indexStart = batch.indexStart, .indexCount = batch.indexCount});
            }
            inst.shadowBatchesValid = true;
        }
        for (const auto& ref : inst.shadowBatches) {
            if (ref.batch < batches.size()) drawBatch(batches[ref.batch], ref.alphaKeyed, ref.indexStart, ref.indexCount);
        }
    };
    // The unit map (casterSpheres given) draws at most the nearest
    // kUnitShadowCap units to its centre, the player: a city crowd put five
    // hundred into it, a millisecond of main-thread recording, for shadows a
    // few pixels across at the back of the crowd; 128 were still 0.4 ms.
    // WOWEE_UNIT_SHADOW_CAP=<n> changes it, 0 lifts it.
    static const size_t kUnitShadowCap = [] {
        const char* v = std::getenv("WOWEE_UNIT_SHADOW_CAP");
        return v && *v ? static_cast<size_t>(std::max(0, std::atoi(v))) : size_t{64};
    }();
    if (casterSpheres && kUnitShadowCap > 0) {
        shadowNearest_.clear();
        const auto consider = [&](CharacterInstance& inst) {
            if (!inst.visible) return;
            const glm::vec3 d = inst.position - shadowCenter;
            const float distSq = glm::dot(d, d);
            if (distSq <= shadowRadiusSq) shadowNearest_.emplace_back(distSq, &inst);
        };
        if (listOn) {
            for (CharacterInstance* inst : shadowInstances_) consider(*inst);
        } else {
            for (auto& pair : instances) consider(pair.second);
        }
        if (shadowNearest_.size() > kUnitShadowCap) {
            std::nth_element(shadowNearest_.begin(), shadowNearest_.begin() + static_cast<std::ptrdiff_t>(kUnitShadowCap),
                             shadowNearest_.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            shadowNearest_.resize(kUnitShadowCap);
        }
        for (const auto& [distSq, inst] : shadowNearest_) castShadow(*inst);
    } else if (listOn) {
        for (CharacterInstance* inst : shadowInstances_) castShadow(*inst);
    } else {
        for (auto& pair : instances) castShadow(pair.second);
    }

    if (profileOn) {
        auto& p = shadowProf_;
        p.walked += walked;
        p.inRange += inRange;
        p.lightCulled += lightCulled;
        p.viewCulled += viewCulled;
        p.drawn += drawn;
        p.boneCopies += boneCopies;
        p.boneReuses += boneReuses;
        if (++p.frames >= 600) {
            LOG_WARNING("  character shadow: ", p.walked / p.frames, " walked, ",
                        p.inRange / p.frames, " in range, ", p.lightCulled / p.frames,
                        " outside the light's box, ", p.viewCulled / p.frames,
                        " cast out of view, ", p.drawn / p.frames, " drawn per frame; bones copied ",
                        p.boneCopies / p.frames, ", already in place ", p.boneReuses / p.frames);
            p = ShadowProfile{};
        }
    }
}

VkDescriptorSet CharacterRenderer::shadowTexDescSet(VkTexture* tex, uint32_t frameIndex) {
    // Valid, not merely non-null, for the reason in getMaterialDescriptorSet:
    // a texture whose upload or view creation failed writes a null view and
    // sampler into a live descriptor. The alpha-tested shadow pass samples it,
    // which is undefined behaviour and reaches NVIDIA as a lost device.
    //
    // Also worth the check for the cache below, which is keyed on the view: a
    // null one would key every failed texture to the same set.
    if (!tex || !tex->isValid()) return shadowParams_.set;
    if (frameIndex >= kShadowTexPoolFrames) return shadowParams_.set;
    VkDescriptorPool pool = shadowTexPool_[frameIndex];
    if (!pool) return shadowParams_.set;

    VkImageView view = tex->getImageView();
    if (auto it = shadowTexSetCache_.find(view); it != shadowTexSetCache_.end()) return it->second;

    VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &shadowParams_.layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &set) != VK_SUCCESS) {
        // Out of sets for this frame: the white fallback casts a full shadow,
        // which is the old behaviour rather than a hole.
        return shadowParams_.set;
    }

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfo.imageView = view;
    imgInfo.sampler = tex->getSampler();
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = shadowParams_.ubo;
    bufInfo.offset = 0;
    bufInfo.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &imgInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &bufInfo;
    vkUpdateDescriptorSets(vkCtx_->getDevice(), 2, writes, 0, nullptr);

    shadowTexSetCache_[view] = set;
    return set;
}

VkTexture* CharacterRenderer::resolveBatchTexture(const CharacterInstance& inst,
                                                 const M2ModelGPU& gm,
                                                 const pipeline::M2Batch& b) const {
            // A skin batch can reference multiple textures (b.textureCount) starting at b.textureIndex.
            // We currently bind only a single texture, so pick the most appropriate one.
            if (b.textureIndex == 0xFFFF) return whiteTexture_.get();
            if (gm.data.textureLookup.empty() || gm.textureIds.empty()) return whiteTexture_.get();

            uint32_t comboCount = b.textureCount ? static_cast<uint32_t>(b.textureCount) : 1u;
            comboCount = std::min<uint32_t>(comboCount, 8u);

            struct Candidate { VkTexture* tex; uint32_t type; };
            Candidate first{.tex = whiteTexture_.get(), .type = 0};
            bool hasFirst = false;
            Candidate firstNonWhite{.tex = whiteTexture_.get(), .type = 0};
            bool hasFirstNonWhite = false;

            for (uint32_t i = 0; i < comboCount; i++) {
                uint32_t lookupPos = static_cast<uint32_t>(b.textureIndex) + i;
                if (lookupPos >= gm.data.textureLookup.size()) break;
                uint16_t texSlot = gm.data.textureLookup[lookupPos];
                if (texSlot >= gm.textureIds.size()) continue;

                VkTexture* texPtr = gm.textureIds[texSlot];
                uint32_t texType = (texSlot < gm.data.textures.size()) ? gm.data.textures[texSlot].type : 0;
                // Apply texture slot overrides, whatever geoset group the batch
                // is in. A batch that names the type-1 slot samples the body
                // atlas by definition - cloaks bind type 2 and hair type 6, so
                // they never reach the skin override. This used to be limited
                // to a list of "skin groups" that left out 1 and 2 (beard,
                // moustache, bare chin), 7 (ears), 10, 11, 12 and 18. On every
                // instance textured by override - other players and NPCs, whose
                // shared model's own type-1 slot is empty - those batches drew
                // the white texture: a white beard on a human male with
                // facial style 3 (102/202), where the local player, textured
                // through the model slot instead, looked right.
                {
                    auto itO = inst.textureSlotOverrides.find(texSlot);
                    if (itO != inst.textureSlotOverrides.end() && itO->second != nullptr) {
                        texPtr = itO->second;
                    }
                }

                if (!hasFirst) {
                    first = {.tex = texPtr, .type = texType};
                    hasFirst = true;
                }

                if (texPtr == nullptr || texPtr == whiteTexture_.get()) continue;

                // Prefer the hair texture slot (type 6) whenever present in the combo.
                if (texType == 6) {
                    return texPtr;
                }

                if (!hasFirstNonWhite) {
                    firstNonWhite = {.tex = texPtr, .type = texType};
                    hasFirstNonWhite = true;
                }
            }

            if (hasFirstNonWhite) return firstNonWhite.tex;
            if (hasFirst && first.tex != nullptr) return first.tex;
            return whiteTexture_.get();
}

glm::mat4 CharacterRenderer::getModelMatrix(const CharacterInstance& instance) const {
    glm::mat4 model = glm::mat4(1.0f);

    // Apply transformations: T * R * S
    // E10: another unit drawn a decaying step behind where it was put.
    const bool smoothed = instance.smoothActive;
    model = glm::translate(model, smoothed ? instance.position + instance.smooth.offset : instance.position);

    // A corpse tilted onto the slope it lies on (updateCorpseSettle), about
    // its position on the ground. Never set with the row off.
    if (instance.corpseSettle > 0.0f) {
        model *= glm::mat4_cast(corpse_settle::tilt(instance.corpseNormal, instance.corpseSettle));
    }

    // Apply rotation (euler angles, Z-up)
    // Convention: yaw around Z, pitch around X, roll around Y.
    model = glm::rotate(model, smoothed ? instance.rotation.z + instance.smooth.yawOffset : instance.rotation.z,
                        glm::vec3(0.0f, 0.0f, 1.0f));  // Yaw
    model = glm::rotate(model, instance.rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));  // Pitch
    model = glm::rotate(model, instance.rotation.y, glm::vec3(0.0f, 1.0f, 0.0f));  // Roll

    model = glm::scale(model, glm::vec3(instance.scale));

    return model;
}

void CharacterRenderer::updateCorpseSettle(float deltaTime, const FloorHeightFn* floor) {
    if (!floor && !corpseSettleActive_) return;
    namespace cs = corpse_settle;
    bool active = false;
    for (auto& [id, inst] : instances) {
        (void)id;
        if (!floor) {
            inst.corpseSettle = 0.0f;
            inst.corpseSampled = false;
            continue;
        }
        const bool body = inst.isDead && inst.deathLocked && !inst.hasOverrideModelMatrix &&
                          inst.mountedOn == 0 && !inst.isAttachedItem && !inst.isEffectModel &&
                          !inst.isSceneModel && inst.cachedModel;
        if (!body) {
            // Risen (or never a body): back up over the same moment.
            if (inst.corpseSettle > 0.0f) {
                inst.corpseSettle = cs::step(inst.corpseSettle, false, deltaTime);
                active = true;
            } else {
                inst.corpseSampled = false;
            }
            continue;
        }
        // Time left of the fall; negative once it is down (or for a unit
        // whose model has only the looping Dead pose).
        float msLeft = -1.0f;
        if (inst.currentAnimationId == anim::DEATH && inst.currentSequenceIndex >= 0 &&
            inst.currentSequenceIndex < static_cast<int>(inst.cachedModel->data.sequences.size())) {
            const float dur = static_cast<float>(
                inst.cachedModel->data.sequences[inst.currentSequenceIndex].duration);
            msLeft = dur - inst.animationTime;
            if (msLeft <= 0.0f) msLeft = -1.0f;
        }
        const bool settle = cs::settling(true, msLeft);
        if (settle && (!inst.corpseSampled ||
                       glm::distance(inst.position, inst.corpseSampledAt) > cs::kResampleDistance)) {
            // A corpse that is already down the first time it is measured came
            // into view lying there: it is drawn settled at once.
            const bool alreadyDown = !inst.corpseSampled && msLeft < 0.0f;
            float top = inst.cachedModel->visualBoundMax.z;
            if (top <= 0.001f) top = inst.cachedModel->data.boundMax.z;
            const float half = cs::footprintHalfExtent(std::max(0.1f, top) * std::max(0.001f, inst.scale));
            const glm::vec3& p = inst.position;
            const float yaw = inst.rotation.z;
            const glm::vec2 ax(std::cos(yaw), std::sin(yaw));
            const glm::vec2 ay(-std::sin(yaw), std::cos(yaw));
            auto at = [&](const glm::vec2& off) { return (*floor)(p.x + off.x, p.y + off.y, p.z); };
            cs::FootprintHeights h;
            h.centre = at(glm::vec2(0.0f));
            h.front = at(ax * half);
            h.back = at(-ax * half);
            h.left = at(ay * half);
            h.right = at(-ay * half);
            inst.corpseNormal = cs::groundNormal(h, yaw, half);
            inst.corpseSampledAt = p;
            inst.corpseSampled = true;
            if (alreadyDown) inst.corpseSettle = 1.0f;
        }
        inst.corpseSettle = cs::step(inst.corpseSettle, settle, deltaTime);
        active = true;
    }
    corpseSettleActive_ = active;
}

void CharacterRenderer::updateIndoorProbes(float deltaTime, const IndoorProbeFn* probe,
                                           const glm::vec3& cameraPos) {
    if (!probe) {
        if (!indoorProbesOn_) return;
        indoorProbesOn_ = false;
        for (auto& [id, inst] : instances) {
            (void)id;
            inst.probeLive = false;
        }
        return;
    }
    indoorProbesOn_ = true;
    namespace ip = indoor_probe;
    const float k = ip::easeFactor(deltaTime);
    constexpr float kReach = 120.0f;
    for (auto& [id, inst] : instances) {
        (void)id;
        if (inst.isSceneModel || !inst.visible) continue;
        // A weapon or a rider is placed by its matrix; read it where that is.
        const glm::vec3 at = inst.hasOverrideModelMatrix ? glm::vec3(inst.overrideModelMatrix[3])
                                                         : inst.position;
        const glm::vec3 d = at - cameraPos;
        if (glm::dot(d, d) > kReach * kReach) continue;
        inst.probeAge += deltaTime;
        if (!inst.probeLive || ip::needsResample(at, inst.probeReadAt, inst.probeAge)) {
            glm::vec3 colour(0.35f);
            glm::vec2 lean(0.0f);
            // Chest height, as the stock ray starts a yard over the feet.
            const bool in = (*probe)(at + glm::vec3(0.0f, 0.0f, 1.0f), colour, lean);
            inst.probeTargetIndoor = in ? 1.0f : 0.0f;
            if (in) {
                inst.probeTargetColour = colour;
                inst.probeTargetLean = lean;
            }
            inst.probeReadAt = at;
            inst.probeAge = 0.0f;
            if (!inst.probeLive) {
                // First read: start where it is, as a unit that spawns indoors
                // does not walk in from the sky's light.
                inst.probeLive = true;
                inst.probeColour = inst.probeTargetColour;
                inst.probeIndoor = inst.probeTargetIndoor;
                inst.probeLean = inst.probeTargetLean;
                continue;
            }
        }
        inst.probeColour += (inst.probeTargetColour - inst.probeColour) * k;
        inst.probeIndoor += (inst.probeTargetIndoor - inst.probeIndoor) * k;
        inst.probeLean += (inst.probeTargetLean - inst.probeLean) * k;
    }
}

void CharacterRenderer::ensureFootRig(M2ModelGPU& gm) {
    if (gm.footRigAnalyzed) return;
    gm.footRigAnalyzed = true;
    const auto& data = gm.data;
    std::vector<foot_ik::BoneInfo> info(data.bones.size());
    for (size_t i = 0; i < data.bones.size(); ++i) {
        info[i].parent = data.bones[i].parentBone;
        info[i].keyBoneId = data.bones[i].keyBoneId;
        info[i].pivot = data.bones[i].pivot;
    }
    for (const auto& v : data.vertices) {
        for (int j = 0; j < 4; ++j) {
            if (v.boneWeights[j] > 0.0f && v.boneIndices[j] < info.size()) {
                info[v.boneIndices[j]].weight += v.boneWeights[j];
            }
        }
    }
    gm.footRig = foot_ik::findLegs(info);
}

void CharacterRenderer::updateFootPlacement(float deltaTime, const FloorHeightFn* floor,
                                            const glm::vec3& cameraPos) {
    footIkOn_ = floor != nullptr;
    if (!floor && !footIkActive_) return;
    namespace fk = foot_ik;
    bool active = false;
    for (auto& [id, inst] : instances) {
        (void)id;
        fk::State& f = inst.footIk;
        if (!floor) {
            f = fk::State{};
            continue;
        }
        const bool unit = inst.cachedModel && !inst.isAttachedItem && !inst.isEffectModel &&
                          !inst.isSceneModel && !inst.hasOverrideModelMatrix && inst.mountedOn == 0;
        if (!unit) {
            if (f.targets.weight > 0.0f || f.havePosition) f = fk::State{};
            continue;
        }
        // The legs, found once per model (the rule in foot_ik::findLegs).
        if (!inst.cachedModel->footRigAnalyzed) {
            auto mit = models.find(inst.modelId);
            if (mit == models.end()) continue;
            ensureFootRig(mit->second);
        }
        const fk::Rig& rig = inst.cachedModel->footRig;
        if (!rig.valid) continue;

        // The unit's speed over the ground, eased; a jump of more than a few
        // yards in a frame is a teleport, not speed.
        if (f.havePosition && deltaTime > 0.0f) {
            float v = glm::length(glm::vec2(inst.position - f.lastPosition)) / deltaTime;
            if (v > 60.0f) v = 0.0f;
            f.speed = fk::ease(f.speed, v, deltaTime, fk::kSpeedSeconds);
        }
        f.lastPosition = inst.position;
        f.havePosition = true;

        const bool inRange = glm::distance(inst.position, cameraPos) < fk::kMaxDistance;
        const bool stands = inst.visible && !inst.isDead && !inst.deathLocked &&
                            fk::standsOnFeet(inst.currentAnimationId);
        const float want = (inRange && stands) ? fk::speedWeight(f.speed) : 0.0f;
        f.targets.weight = fk::ease(f.targets.weight, want, deltaTime, fk::kWeightSeconds);
        if (want <= 0.0f && f.targets.weight < 0.01f) {
            // Faded out: back to the stock pose, and nothing read until it
            // comes back.
            f.targets = fk::Targets{};
            f.read = false;
            continue;
        }
        active = true;
        if (!f.endsValid) continue;

        // The floor under the unit, ahead of it and under each foot, read
        // again only once the unit or that foot has moved.
        const float scale = std::max(inst.scale, 1e-3f);
        const float legWorld = rig.legLength * scale;
        const glm::mat4 model = getModelMatrix(inst);
        const glm::vec3 p = inst.position;
        const float yaw = inst.rotation.z;
        const glm::vec2 fwd(std::cos(yaw), std::sin(yaw));
        const glm::vec2 left(-fwd.y, fwd.x);
        const float frontDist = std::max(0.3f, 0.5f * legWorld);
        if (!f.read || glm::distance(glm::vec2(p), f.unitReadAt) > fk::kResampleDistance) {
            f.unitFloor = (*floor)(p.x, p.y, p.z);
            const glm::vec2 a = glm::vec2(p) + fwd * frontDist;
            f.frontFloor = (*floor)(a.x, a.y, p.z);
            f.unitReadAt = glm::vec2(p);
        }
        glm::vec2 footXY[2];
        for (int l = 0; l < 2; ++l) {
            footXY[l] = glm::vec2(model * glm::vec4(f.ends[l], 1.0f));
            if (!f.read || glm::distance(footXY[l], f.footReadAt[l]) > fk::kResampleDistance) {
                f.footFloor[l] = (*floor)(footXY[l].x, footXY[l].y, p.z);
                f.footReadAt[l] = footXY[l];
            }
        }
        f.read = true;
        const float leftSpan = glm::dot(footXY[0] - footXY[1], left);
        const fk::Targets aim = fk::aim(f.unitFloor, f.footFloor[0], f.footFloor[1], f.frontFloor,
                                        frontDist, leftSpan, legWorld, scale);
        fk::stepTargets(f.targets, aim, deltaTime);
    }
    footIkActive_ = active;
}

void CharacterRenderer::setMotionEnhancements(int secondaryMotion, float secondaryStrength,
                                              const glm::vec3& windWorld, bool remoteSmoothing,
                                              uint32_t localA, uint32_t localB, uint32_t localC) {
    localInstances_[0] = localA;
    localInstances_[1] = localB;
    localInstances_[2] = localC;
    springWind_ = windWorld;
    // The strength row at 0 is off as level 0 is: no spring runs and every
    // bone is its tracks.
    springStrength_ = secondary_motion::clampStrength(secondaryStrength);
    const int level = springStrength_ > 0.0f ? std::clamp(secondaryMotion, 0, 2) : 0;
    if (level != springLevel_) {
        springLevel_ = level;
        if (level > 0) {
            for (auto& [id, m] : models) {
                (void)id;
                if (!m.springAnalyzed) analyzeSpringChains(m);
            }
        }
        // Every chain starts again from rest: off leaves nothing behind, and a
        // kind turned off or on does not carry an old swing.
        for (auto& [id, inst] : instances) {
            (void)id;
            inst.springBones.clear();
            inst.springChainStates.clear();
            inst.springStamp = -1.0f;
            inst.springWeight = 0.0f;
            inst.hem[0] = inst.hem[1] = secondary_motion::HemState{};
            inst.hemStamp = -1.0f;
            inst.hemActive = false;
            inst.capeActive = false;
        }
    }
    if (remoteSmoothing != remoteSmoothing_) {
        remoteSmoothing_ = remoteSmoothing;
        if (!remoteSmoothing) {
            for (auto& [id, inst] : instances) {
                (void)id;
                inst.smooth = remote_smoothing::State{};
                inst.smoothActive = false;
            }
        }
    }
}

void CharacterRenderer::addMotionCorrection(uint32_t instanceId, const glm::vec3& renderDelta) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    it->second.smoothReported = true;
    it->second.pendingCorrection += renderDelta;
}

bool CharacterRenderer::isRemoteUnit(const CharacterInstance& inst) const {
    if (inst.isAttachedItem || inst.isEffectModel || inst.isSceneModel) return false;
    for (uint32_t local : localInstances_) {
        if (local != 0 && inst.id == local) return false;
    }
    return true;
}

namespace {
/// The points of a cloth bone the body collider tests: its tip and middle,
/// each on the midline and toward both edges of the cloth.
void collisionPoints(const glm::vec3& pivot, const glm::vec3& tip, const glm::vec3& lateral, float halfWidth,
                     glm::vec3 (&out)[6]) {
    const glm::vec3 mid = 0.5f * (pivot + tip);
    const glm::vec3 w = lateral * (0.75f * halfWidth);
    out[0] = tip;
    out[1] = tip + w;
    out[2] = tip - w;
    out[3] = mid;
    out[4] = mid + w;
    out[5] = mid - w;
}
}  // namespace

void CharacterRenderer::analyzeSpringChains(M2ModelGPU& gm) {
    namespace sm = secondary_motion;
    gm.springAnalyzed = true;
    gm.springChains.clear();
    gm.springSlot.clear();
    gm.springSlotChain.clear();
    gm.springSlotIndex.clear();
    const auto& data = gm.data;
    const size_t n = data.bones.size();
    if (n < 2 || data.vertices.empty()) return;
    // Each bone's vertex weight, by the kind of geoset the vertex is drawn in
    // (a vertex shared by two sections counts in both, as it is drawn in both).
    std::vector<sm::BoneInfo> info(n);
    for (size_t i = 0; i < n; ++i) {
        info[i].keyBoneId = data.bones[i].keyBoneId;
        info[i].parent = data.bones[i].parentBone;
        info[i].pivot = data.bones[i].pivot;
    }
    const bool character = gm.hasCompositeSlots;
    std::vector<uint8_t> seen(data.vertices.size(), 0);
    std::unordered_set<uint64_t> sectionsDone;
    for (const auto& batch : data.batches) {
        // Several batches (texture layers) draw one section: count it once.
        const uint64_t sectionKey = (static_cast<uint64_t>(batch.indexStart) << 32) | batch.indexCount;
        if (!sectionsDone.insert(sectionKey).second) continue;
        const sm::GeosetClass cls = sm::classifyGeoset(batch.submeshId, character);
        const uint32_t end = std::min<uint32_t>(batch.indexStart + batch.indexCount,
                                                static_cast<uint32_t>(data.indices.size()));
        std::fill(seen.begin(), seen.end(), 0);
        for (uint32_t k = batch.indexStart; k < end; ++k) {
            const uint16_t vi = data.indices[k];
            if (vi >= data.vertices.size() || seen[vi]) continue;
            seen[vi] = 1;
            const auto& v = data.vertices[vi];
            for (int j = 0; j < 4; ++j) {
                const float w = v.boneWeights[j];
                const uint8_t b = v.boneIndices[j];
                if (w <= 0.0f || b >= n) continue;
                auto& bi = info[b];
                bi.total += w;
                if (cls == sm::GeosetClass::Body) bi.body += w;
                else if (cls == sm::GeosetClass::Hair) bi.hair += w;
                else if (cls == sm::GeosetClass::Cloth) bi.cloth += w;
            }
        }
    }
    gm.springChains = sm::findChains(info, character);
    gm.springSlot.assign(n, -1);
    for (size_t c = 0; c < gm.springChains.size(); ++c) {
        const auto& chain = gm.springChains[c];
        for (size_t k = 0; k < chain.bones.size(); ++k) {
            gm.springSlot[chain.bones[k]] = static_cast<int16_t>(gm.springSlotChain.size());
            gm.springSlotChain.push_back(static_cast<uint8_t>(c));
            gm.springSlotIndex.push_back(static_cast<uint8_t>(k));
        }
    }
    // The body collider: each cloth bone's far end at rest and the bones
    // below it, and capsules on the legs fitted to the rest pose.
    const size_t slots = gm.springSlotChain.size();
    gm.springTip.assign(slots, glm::vec3(0.0f));
    gm.springHalfWidth.assign(slots, 0.0f);
    gm.springBelow.assign(slots, {});
    gm.springColliders.clear();
    std::vector<float> reach(slots, -1.0f);
    for (size_t slot = 0; slot < slots; ++slot) {
        const auto& chain = gm.springChains[gm.springSlotChain[slot]];
        const size_t k = gm.springSlotIndex[slot];
        const uint16_t b = chain.bones[k];
        gm.springTip[slot] = !chain.fan && k + 1 < chain.bones.size()
            ? data.bones[chain.bones[k + 1]].pivot
            : data.bones[b].pivot + chain.restDir * (0.25f * chain.length);
        for (size_t j = b + 1u; j < n; ++j) {
            for (int up = data.bones[j].parentBone; up >= 0 && static_cast<size_t>(up) < n;
                 up = data.bones[up].parentBone) {
                if (up == b) { gm.springBelow[slot].push_back(static_cast<uint16_t>(j)); break; }
                if (up < b) break;
            }
        }
    }
    // The last bone of a chain reaches as far as the farthest vertex it
    // carries most of (a cloak's hem hangs well below its last bone).
    for (const auto& v : data.vertices) {
        int best = 0;
        for (int j = 1; j < 4; ++j) if (v.boneWeights[j] > v.boneWeights[best]) best = j;
        const uint8_t b = v.boneIndices[best];
        if (v.boneWeights[best] <= 0.0f || b >= n || gm.springSlot[b] < 0) continue;
        const size_t slot = static_cast<size_t>(gm.springSlot[b]);
        const auto& chain = gm.springChains[gm.springSlotChain[slot]];
        gm.springHalfWidth[slot] = std::max(gm.springHalfWidth[slot],
                                            std::abs(glm::dot(v.position - data.bones[b].pivot, chain.lateral)));
        if (chain.fan || gm.springSlotIndex[slot] + 1u != chain.bones.size()) continue;
        const float d = glm::dot(v.position - data.bones[b].pivot, chain.restDir);
        if (d > reach[slot]) { reach[slot] = d; gm.springTip[slot] = data.bones[b].pivot + chain.restDir * d; }
    }
    ensureFootRig(gm);
    // The cloak: the back cloth chain, and how high the cloak geosets reach
    // (it bends from a little below its top, where it lies on the shoulders).
    gm.capeChain = -1;
    if (character) {
        for (size_t c = 0; c < gm.springChains.size(); ++c) {
            const auto& chain = gm.springChains[c];
            if (chain.kind == sm::ChainKind::Cloth && !chain.fan && chain.outward.x < -0.5f) {
                gm.capeChain = static_cast<int>(c);
                break;
            }
        }
        float top = -1e30f, hem = 1e30f;
        for (const auto& batch : data.batches) {
            if (batch.submeshId <= 1501 || batch.submeshId >= 1600) continue;
            const uint32_t end = std::min<uint32_t>(batch.indexStart + batch.indexCount,
                                                    static_cast<uint32_t>(data.indices.size()));
            for (uint32_t k = batch.indexStart; k < end; ++k) {
                const uint16_t vi = data.indices[k];
                if (vi >= data.vertices.size()) continue;
                top = std::max(top, data.vertices[vi].position.z);
                hem = std::min(hem, data.vertices[vi].position.z);
            }
        }
        if (gm.capeChain >= 0 && top > hem + 0.1f) {
            gm.capePivotZ = top - 0.15f * (top - hem);
            gm.capeHemZ = hem;
        } else {
            gm.capeChain = -1;
        }
    }
    // Long robes: their geosets and how far they reach, top to hem.
    gm.robeGeosets.clear();
    if (character && gm.footRig.valid) {
        float top = -1e30f, hem = 1e30f;
        std::unordered_set<uint64_t> robeSections;
        for (const auto& batch : data.batches) {
            if (batch.submeshId < 1302 || batch.submeshId >= 1400) continue;
            if (std::find(gm.robeGeosets.begin(), gm.robeGeosets.end(), batch.submeshId) == gm.robeGeosets.end())
                gm.robeGeosets.push_back(batch.submeshId);
            const uint64_t key = (static_cast<uint64_t>(batch.indexStart) << 32) | batch.indexCount;
            if (!robeSections.insert(key).second) continue;
            const uint32_t end = std::min<uint32_t>(batch.indexStart + batch.indexCount,
                                                    static_cast<uint32_t>(data.indices.size()));
            for (uint32_t k = batch.indexStart; k < end; ++k) {
                const uint16_t vi = data.indices[k];
                if (vi >= data.vertices.size()) continue;
                top = std::max(top, data.vertices[vi].position.z);
                hem = std::min(hem, data.vertices[vi].position.z);
            }
        }
        if (top > hem + 0.05f) {
            gm.robeTopZ = top;
            gm.robeHemZ = hem;
        } else {
            gm.robeGeosets.clear();
        }
    }
    if (gm.footRig.valid) {
        std::vector<glm::vec3> rest;
        for (size_t slot = 0; slot < slots; ++slot) {
            const auto& chain = gm.springChains[gm.springSlotChain[slot]];
            if (chain.kind != sm::ChainKind::Cloth || chain.fan) continue;
            const glm::vec3 p = data.bones[chain.bones[gm.springSlotIndex[slot]]].pivot;
            glm::vec3 pts[6];
            collisionPoints(p, gm.springTip[slot], chain.lateral, gm.springHalfWidth[slot], pts);
            rest.insert(rest.end(), std::begin(pts), std::end(pts));
        }
        const float leg = gm.footRig.legLength;
        auto add = [&](int a, const glm::vec3& pa, int b, const glm::vec3& pb, float nominal) {
            M2ModelGPU::SpringCollider c;
            c.a = static_cast<int16_t>(a);
            c.b = static_cast<int16_t>(b);
            c.pivotA = pa;
            c.pivotB = pb;
            c.r = sm::fitRadius(nominal * leg, pa, pb, rest);
            if (c.r > 0.01f * leg) gm.springColliders.push_back(c);
        };
        for (const auto& l : gm.footRig.legs) {
            add(l.hip, l.hipPivot, l.knee, l.kneePivot, 0.11f);
            add(l.knee, l.kneePivot, l.end, l.endPivot, 0.08f);
        }
        const auto& legL = gm.footRig.legs[0];
        const auto& legR = gm.footRig.legs[1];
        add(legL.hip, legL.hipPivot, legR.hip, legR.hipPivot, 0.13f);
    }
    static const bool logChains = core::envFlagEnabled("WOWEE_SPRING_LOG", false);
    if (logChains) {
        for (const auto& c : gm.springColliders)
            LOG_INFO("Spring collider on ", data.name, ": bones ", c.a, "-", c.b, " r=", c.r);
        for (const auto& chain : gm.springChains) {
            std::string bonesText;
            for (uint16_t b : chain.bones) bonesText += std::to_string(b) + " ";
            LOG_INFO("Spring chain on ", data.name, ": ",
                     chain.kind == sm::ChainKind::Cloth ? "cloth" : chain.kind == sm::ChainKind::Hair ? "hair" : "tail",
                     chain.fan ? " (fan)" : "", " bones ", bonesText, "length ", chain.length);
        }
    }
}

void CharacterRenderer::stepRobeHem(CharacterInstance& inst, const M2ModelGPU& gm) {
    namespace sm = secondary_motion;
    const foot_ik::Rig& rig = gm.footRig;
    inst.hemActive = false;
    if (!rig.valid || inst.isAttachedItem || inst.isEffectModel || inst.isSceneModel) return;
    bool worn = false;
    for (uint16_t g : gm.robeGeosets) {
        if (inst.activeGeosets.count(g)) { worn = true; break; }
    }
    const float weight = sm::distanceWeight(glm::distance(inst.position, lastCameraPos_));
    if (!worn || weight <= 0.0f || springStrength_ <= 0.0f) {
        inst.hem[0] = inst.hem[1] = sm::HemState{};
        return;
    }
    const size_t numBones = inst.boneMatrices.size();
    if (numBones > static_cast<size_t>(kCapeSlot)) return;  // no free slot for the data
    const glm::mat4 world = inst.hasOverrideModelMatrix ? inst.overrideModelMatrix : getModelMatrix(inst);
    const glm::mat3 lin(world);
    const float scale = std::max(glm::length(lin[0]), 1e-4f);
    const glm::mat3 toModel = glm::transpose(lin / scale) / scale;
    // Each step reads the time since this unit's last pose, as the chains do.
    const float dt = inst.hemStamp < 0.0f ? -1.0f : std::max(springClock_ - inst.hemStamp, 0.0f);
    inst.hemStamp = springClock_;
    const float maxOff = sm::kHemMaxOffset * rig.legLength * sm::limitScale(springStrength_);
    glm::vec3 off[2];
    for (int l = 0; l < 2; ++l) {
        const foot_ik::Leg& leg = rig.legs[l];
        if (leg.knee < 0 || leg.end < 0 || static_cast<size_t>(std::max(leg.knee, leg.end)) >= numBones) return;
        const glm::vec3 knee(inst.boneMatrices[leg.knee] * glm::vec4(leg.kneePivot, 1.0f));
        const glm::vec3 ankle(inst.boneMatrices[leg.end] * glm::vec4(leg.endPivot, 1.0f));
        const glm::vec3 shin = 0.5f * (knee + ankle);
        const glm::vec3 target(world * glm::vec4(shin, 1.0f));
        const glm::vec3 o = sm::stepHem(inst.hem[l], sm::HemParams{}, target, springWind_ * 0.5f,
                                        dt < 0.0f ? sm::kResetDt * 2.0f : dt);
        off[l] = sm::softClamp(toModel * o * springStrength_, maxOff) * weight;
        off[l].z *= 0.3f;  // the hem swings; it barely lifts
    }
    // Columns: the left and right offsets; the robe's top and hem heights and
    // the left leg's line; the right leg's line and the band (from the leg's
    // line) over which the cloth goes from hugging the leg to swinging free.
    const float r0 = 0.09f * rig.legLength, r1 = 0.22f * rig.legLength;
    inst.hemData = glm::mat4(glm::vec4(off[0], 1.0f), glm::vec4(off[1], 0.0f),
                             glm::vec4(gm.robeTopZ, gm.robeHemZ, rig.legs[0].kneePivot.x, rig.legs[0].kneePivot.y),
                             glm::vec4(rig.legs[1].kneePivot.x, rig.legs[1].kneePivot.y, r0, r1));
    inst.hemActive = true;
}

void CharacterRenderer::updateCapeBend(CharacterInstance& inst, const M2ModelGPU& gm) {
    namespace sm = secondary_motion;
    inst.capeActive = false;
    if (inst.boneMatrices.size() > static_cast<size_t>(kCapeSlot)) return;
    const auto& chain = gm.springChains[static_cast<size_t>(gm.capeChain)];
    size_t first = 0;
    for (int c = 0; c < gm.capeChain; ++c) first += gm.springChains[static_cast<size_t>(c)].bones.size();
    if (first + chain.bones.size() > inst.springBones.size()) return;
    // The chain's whole swing, taken by the cloak as one plate turning
    // about a line across the shoulders: per yard below that line, the
    // vertex moves out by the out-swing and aside by the side-swing (small
    // angles), and rises by half the square, so the cloth keeps its length.
    glm::vec2 swing(0.0f);
    for (size_t k = 0; k < chain.bones.size(); ++k) swing += inst.springBones[first + k].swing;
    swing *= sm::kCapeBendShare * inst.springWeight;
    swing.x = std::max(swing.x, -0.05f);  // never bent into the back
    const int parent = gm.data.bones[chain.bones.front()].parentBone;
    if (parent < 0 || static_cast<size_t>(parent) >= inst.boneMatrices.size()) return;
    const glm::mat3 frame(inst.boneMatrices[static_cast<size_t>(parent)]);
    const glm::vec3 perDepth = frame * (chain.outward * swing.x + chain.lateral * swing.y);
    const glm::vec3 lift = frame * glm::vec3(0.0f, 0.0f, 0.5f * glm::dot(swing, swing));
    inst.capeData = glm::mat4(glm::vec4(perDepth, 0.0f), glm::vec4(lift, 0.0f),
                              glm::vec4(gm.capePivotZ, gm.capeHemZ, 0.0f, 0.0f), glm::vec4(0.0f));
    inst.capeActive = glm::dot(swing, swing) > 1e-8f;
}

void CharacterRenderer::applySpringCollisions(CharacterInstance& inst, const M2ModelGPU& gm) {
    namespace sm = secondary_motion;
    const auto& bones = gm.data.bones;
    const size_t numBones = std::min(bones.size(), inst.boneMatrices.size());
    if (inst.springWeight <= 0.0f) return;
    auto posed = [&](int b, const glm::vec3& p) {
        return glm::vec3(inst.boneMatrices[static_cast<size_t>(b)] * glm::vec4(p, 1.0f));
    };
    // The capsules as posed now.
    sm::Capsule caps[8];
    size_t capCount = 0;
    for (const auto& c : gm.springColliders) {
        if (capCount == std::size(caps)) break;
        if (c.a < 0 || c.b < 0 || static_cast<size_t>(c.a) >= numBones || static_cast<size_t>(c.b) >= numBones)
            continue;
        caps[capCount++] = sm::Capsule{posed(c.a, c.pivotA), posed(c.b, c.pivotB), c.r};
    }
    if (capCount == 0) return;
    size_t first = 0;
    for (const auto& chain : gm.springChains) {
        const size_t count = chain.bones.size();
        const size_t base = first;
        first += count;
        if (chain.kind != sm::ChainKind::Cloth || chain.fan || !sm::kindEnabled(chain.kind, springLevel_))
            continue;
        const int parent = bones[chain.bones.front()].parentBone;
        if (parent < 0 || static_cast<size_t>(parent) >= numBones) continue;
        const glm::mat3 frame(inst.boneMatrices[static_cast<size_t>(parent)]);
        const glm::vec3 n = glm::normalize(frame * chain.outward);
        const glm::vec3 side = glm::normalize(frame * chain.lateral);
        if (!std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z) || !std::isfinite(side.x)) continue;
        for (size_t k = 0; k < count; ++k) {
            const size_t slot = base + k;
            const uint16_t b = chain.bones[k];
            if (b >= numBones || slot >= gm.springTip.size()) break;
            const glm::vec3 p = posed(b, bones[b].pivot);
            const glm::vec3 tip = posed(b, gm.springTip[slot]);
            glm::vec3 pts[6];
            collisionPoints(p, tip, side, gm.springHalfWidth[slot], pts);
            const glm::vec3 turn = sm::clearanceTurn(p, pts, std::size(pts), n, caps, capCount) * inst.springWeight;
            const float angle = glm::length(turn);
            if (angle < 1e-5f) continue;
            const glm::mat4 fix = glm::translate(glm::mat4(1.0f), p) *
                                  glm::mat4_cast(glm::angleAxis(angle, turn / angle)) *
                                  glm::translate(glm::mat4(1.0f), -p);
            inst.boneMatrices[b] = fix * inst.boneMatrices[b];
            for (uint16_t j : gm.springBelow[slot])
                if (j < numBones) inst.boneMatrices[j] = fix * inst.boneMatrices[j];
        }
    }
}

bool CharacterRenderer::stepSprings(CharacterInstance& inst) {
    namespace sm = secondary_motion;
    const M2ModelGPU* gm = inst.cachedModel;
    if (!gm || gm->springChains.empty() || inst.isAttachedItem || inst.isEffectModel || inst.isSceneModel)
        return false;
    const float weight = sm::distanceWeight(glm::distance(inst.position, lastCameraPos_));
    const float dt = inst.springStamp < 0.0f ? -1.0f : springClock_ - inst.springStamp;
    inst.springStamp = springClock_;
    if (weight <= 0.0f) {
        // Out of range: rest, and start over when it comes back.
        if (!inst.springChainStates.empty()) {
            inst.springChainStates.clear();
            inst.springBones.clear();
        }
        inst.springWeight = 0.0f;
        return false;
    }
    const size_t slots = gm->springSlotChain.size();
    if (inst.springBones.size() != slots) inst.springBones.assign(slots, sm::BoneState{});
    if (inst.springChainStates.size() != gm->springChains.size())
        inst.springChainStates.assign(gm->springChains.size(), sm::ChainState{});

    const glm::mat4 world = inst.hasOverrideModelMatrix ? inst.overrideModelMatrix : getModelMatrix(inst);
    const glm::mat3 lin(world);
    const float scale = std::max(glm::length(lin[0]), 1e-4f);
    const glm::mat3 toModel = glm::transpose(lin / scale);
    const glm::vec2 windDir = glm::dot(glm::vec2(springWind_), glm::vec2(springWind_)) > 1e-8f
        ? glm::normalize(glm::vec2(springWind_)) : glm::vec2(0.8f, 0.6f);
    const glm::vec3 wind = springWind_ *
        sm::gust(glm::vec2(inst.position), windDir, springClock_, static_cast<float>(inst.id % 97u) * 0.37f);

    bool any = false;
    size_t first = 0;
    for (size_t c = 0; c < gm->springChains.size(); ++c) {
        const sm::Chain& chain = gm->springChains[c];
        sm::BoneState* bones = inst.springBones.data() + first;
        first += chain.bones.size();
        sm::ChainState& cs = inst.springChainStates[c];
        if (!sm::kindEnabled(chain.kind, springLevel_)) {
            if (cs.primed) sm::resetChain(cs, bones, chain.bones.size(), glm::vec3(0.0f));
            cs.primed = false;
            continue;
        }
        sm::Input in;
        // The probe rides half on the root's parent bone as it was last
        // posed, so the body's own animated motion - the bob of a stride,
        // the crouch and spring of a jump, a mount's gallop under a rider -
        // pushes the chain as well as the model's travel does.
        glm::vec3 probeModel = chain.probe;
        const int parent = gm->data.bones[chain.bones.front()].parentBone;
        // (Not before the first pose: the matrices are zero until then.)
        if (inst.poseSerial > 2 && parent >= 0 && static_cast<size_t>(parent) < inst.boneMatrices.size())
            probeModel = glm::mix(probeModel,
                                  glm::vec3(inst.boneMatrices[parent] * glm::vec4(chain.probe, 1.0f)),
                                  sm::kGaitShare);
        in.probeWorld = glm::vec3(world * glm::vec4(probeModel, 1.0f));
        in.strength = springStrength_;
        in.worldToModel = toModel;
        in.windWorld = wind;
        in.dt = dt < 0.0f ? sm::kResetDt * 2.0f : dt;
        in.freqScale = sm::lengthFrequencyScale(chain.length * scale);
        sm::stepChain(chain, sm::paramsFor(chain.kind), cs, bones, in);
        any = true;
    }
    inst.springWeight = weight;
    // WOWEE_SPRING_LOG=1: the player's chains' swing a few times a second.
    static const bool logSwing = core::envFlagEnabled("WOWEE_SPRING_LOG", false);
    if (logSwing && inst.id == localInstances_[0] && std::fmod(springClock_, 0.25f) < dt) {
        std::string text;
        size_t at = 0;
        for (size_t c = 0; c < gm->springChains.size(); ++c) {
            float out = 0.0f, side = 0.0f;
            for (size_t k = 0; k < gm->springChains[c].bones.size(); ++k, ++at) {
                out += inst.springBones[at].swing.x;
                side += inst.springBones[at].swing.y;
            }
            text += " chain" + std::to_string(c) + " out=" + std::to_string(glm::degrees(out)) +
                    " side=" + std::to_string(glm::degrees(side));
        }
        LOG_INFO("Spring swing t=", springClock_, text);
    }
    return any;
}

void CharacterRenderer::setInstancePosition(uint32_t instanceId, const glm::vec3& position) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) {
        it->second.position = position;
        it->second.moveStart = position;
        it->second.moveEnd = position;
        it->second.moveElapsed = 0.0f;
        it->second.moveDuration = 0.0f;
        it->second.isMoving = false;
    }
}

void CharacterRenderer::setInstanceRotation(uint32_t instanceId, const glm::vec3& rotation) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) {
        it->second.rotation = rotation;
    }
}

void CharacterRenderer::setInstanceTorsoYaw(uint32_t instanceId, float deltaYawRad, float headYawRad) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) {
        it->second.torsoYawOverrideRad = deltaYawRad;
        it->second.headYawOverrideRad = headYawRad;
    }
}

void CharacterRenderer::moveInstanceTo(uint32_t instanceId, const glm::vec3& destination, float durationSeconds) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;

    auto& inst = it->second;

    // Don't move dead instances (corpses shouldn't slide around)
    if (inst.isDead) return;

    float pdx = destination.x - inst.position.x;
    float pdy = destination.y - inst.position.y;
    float planarDistSq = pdx * pdx + pdy * pdy;
    if (durationSeconds <= 0.0f) {
        if (planarDistSq < 1e-4f) {
            // Stop at current location.
            inst.position = destination;
            inst.isMoving = false;
            if (inst.currentAnimationId == anim::WALK || inst.currentAnimationId == anim::RUN) {
                playAnimation(instanceId, anim::STAND, true);
            }
            return;
        }
        // Some cores send movement-only deltas without spline duration.
        // Synthesize a tiny duration so movement anim/rotation still updates.
        durationSeconds = std::clamp(std::sqrt(planarDistSq) / 7.0f, 0.05f, 0.20f);
    }

    inst.moveStart = inst.position;
    inst.moveEnd = destination;
    inst.moveDuration = durationSeconds;
    inst.moveElapsed = 0.0f;
    inst.isMoving = true;

    // Face toward destination (yaw around Z axis since Z is up)
    glm::vec3 dir = destination - inst.position;
    if (dir.x * dir.x + dir.y * dir.y > 1e-6f) {
        float angle = std::atan2(dir.y, dir.x);
        inst.rotation.z = angle;
    }

    // No movement animation is chosen here. The callers - the per-frame remote
    // player sync in Application and the creature move callback - pick
    // Walk/Run/Swim/Fly/Mount from the movement flags on each state change.
    // The guess that used to be made here came from one frame's displacement
    // over a duration clamped to at least 50 ms, so at 30-40 fps a running
    // player's estimate straddled its 4.5 yd/s line and flipped Walk<->Run
    // from frame to frame, restarting the cycle each time; it put a mounted
    // rider into Run every frame for the sync loop to put back into Mount; and
    // it started Run again during the dead-reckoning tail after the sync loop
    // had already chosen Stand, leaving the unit running in place.
}

const pipeline::M2Model* CharacterRenderer::getModelData(uint32_t modelId) const {
    auto it = models.find(modelId);
    if (it == models.end()) return nullptr;
    return &it->second.data;
}

void CharacterRenderer::startFadeIn(uint32_t instanceId, float durationSeconds) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    it->second.opacity = 0.0f;
    it->second.fadeInTime = 0.0f;
    it->second.fadeInDuration = durationSeconds;
}

void CharacterRenderer::setInstanceDiffuseTint(uint32_t instanceId, const glm::vec3& tint) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) it->second.diffuseTint = tint;
}

void CharacterRenderer::setInstanceOpacity(uint32_t instanceId, float opacity) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) {
        const float clampedOpacity = std::clamp(opacity, 0.0f, 1.0f);
        it->second.opacity = clampedOpacity;
        // Cancel any fade-in in progress to avoid overwriting the new opacity
        it->second.fadeInDuration = 0.0f;

        // Equipment is rendered as independent character instances. Keep the
        // whole visual together instead of leaving opaque weapons floating on
        // a translucent stealthed creature.
        for (const auto& attachment : it->second.weaponAttachments) {
            auto weaponIt = instances.find(attachment.weaponInstanceId);
            if (weaponIt != instances.end()) {
                weaponIt->second.opacity = clampedOpacity;
                weaponIt->second.fadeInDuration = 0.0f;
            }
            for (const auto& effect : attachment.effects) {
                auto effectIt = instances.find(effect.effectInstanceId);
                if (effectIt != instances.end()) {
                    effectIt->second.opacity = clampedOpacity;
                    effectIt->second.fadeInDuration = 0.0f;
                }
            }
        }
    }
}

void CharacterRenderer::setUnitAmbientArea(uint32_t areaId) {
    if (areaId == 0 || areaId == unitAmbientAreaId_ || !assetManager) return;
    unitAmbientAreaId_ = areaId;
    auto areas = assetManager->loadDBC("AreaTable.dbc");
    // AmbientMultiplier is column 34 of the 36-column WotLK record.
    if (!areas || !areas->isLoaded() || areas->getFieldCount() < 36) return;
    int32_t row = areas->findRecordById(areaId);
    if (row < 0) return;
    const uint32_t parent = areas->getUInt32(static_cast<uint32_t>(row), 2);
    const uint32_t flags = areas->getUInt32(static_cast<uint32_t>(row), 4);
    if (unit_light::subareaUsesParent(parent, flags)) {
        const int32_t parentRow = areas->findRecordById(parent);
        if (parentRow >= 0) row = parentRow;
    }
    const float m = areas->getFloat(static_cast<uint32_t>(row), 34);
    unitAmbientTarget_ = unit_light::ambientMultiplier(std::clamp(m, 0.0f, 4.0f));
}

void CharacterRenderer::setInstanceMountedOn(uint32_t riderInstanceId, uint32_t mountInstanceId) {
    setInstanceSeatedOn(riderInstanceId, mountInstanceId, 0, glm::vec3(0.0f), 0.0f, -1);
}

void CharacterRenderer::setInstanceSeatedOn(uint32_t passengerInstanceId, uint32_t vehicleInstanceId,
                                            int attachmentId, const glm::vec3& offset, float yaw,
                                            int passengerAttachmentId) {
    auto it = instances.find(passengerInstanceId);
    if (it == instances.end()) return;
    if (it->second.mountedOn != 0 && vehicleInstanceId == 0) it->second.hasOverrideModelMatrix = false;
    if (vehicleInstanceId == 0 || it->second.mountedOn != vehicleInstanceId) {
        it->second.seatArcBlend = 1.0f;
        it->second.seatArcLift = 0.0f;
        it->second.seatPointValid = false;
    }
    it->second.mountedOn = vehicleInstanceId;
    it->second.seatAttachment = attachmentId;
    it->second.seatOffset = offset;
    it->second.seatYaw = yaw;
    it->second.seatPassengerAttachment = passengerAttachmentId;
}

void CharacterRenderer::setInstanceSeatArc(uint32_t passengerInstanceId, const glm::vec3& from,
                                           float blend, float lift) {
    auto it = instances.find(passengerInstanceId);
    if (it == instances.end()) return;
    it->second.seatArcFrom = from;
    it->second.seatArcBlend = std::clamp(blend, 0.0f, 1.0f);
    it->second.seatArcLift = lift;
}

bool CharacterRenderer::getInstanceSeatPoint(uint32_t passengerInstanceId, glm::vec3& out) const {
    auto it = instances.find(passengerInstanceId);
    if (it == instances.end() || it->second.mountedOn == 0 || !it->second.seatPointValid) return false;
    out = it->second.seatPoint;
    return true;
}

void CharacterRenderer::setInstanceScale(uint32_t instanceId, float scale) {
    auto it = instances.find(instanceId);
    if (it == instances.end() || !(scale > 0.0f)) return;
    it->second.scale = scale;
}

void CharacterRenderer::setActiveGeosets(uint32_t instanceId, const std::unordered_set<uint16_t>& geosets) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) {
        it->second.activeGeosets = geosets;
        it->second.shadowBatchesValid = false;
    }
}

void CharacterRenderer::setDrawSkinExtra(uint32_t instanceId, bool enabled) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) {
        it->second.drawSkinExtra = enabled;
    }
}

void CharacterRenderer::setGroupTextureOverride(uint32_t instanceId, uint16_t geosetGroup, VkTexture* texture) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) {
        it->second.groupTextureOverrides[geosetGroup] = texture;
    }
}

void CharacterRenderer::setTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot, VkTexture* texture) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) {
        it->second.textureSlotOverrides[textureSlot] = texture;
    }
}

void CharacterRenderer::clearTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) {
        it->second.textureSlotOverrides.erase(textureSlot);
    }
}

void CharacterRenderer::setInstanceVisible(uint32_t instanceId, bool visible) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) {
        if (it->second.visible != visible) {
            LOG_INFO("CharacterRenderer::setInstanceVisible id=", instanceId, " visible=", visible);
        }
        it->second.visible = visible;

        // Also hide/show attached weapons (for first-person mode)
        for (const auto& wa : it->second.weaponAttachments) {
            auto weapIt = instances.find(wa.weaponInstanceId);
            if (weapIt != instances.end()) {
                weapIt->second.visible = visible;
            }
        }
    }
}

void CharacterRenderer::removeInstance(uint32_t instanceId) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;

    LOG_INFO("CharacterRenderer::removeInstance id=", instanceId,
             " pos=(", it->second.position.x, ",", it->second.position.y, ",", it->second.position.z, ")",
             " remaining=", instances.size() - 1,
             " override=", (void*)renderPassOverride_);

    // Remove child attachments first (helmets/weapons/enchant effects),
    // otherwise they leak as orphan render instances when the parent creature
    // despawns. Their models get a fresh id per attach, so unload those too
    // once the instances are gone.
    auto attachments = it->second.weaponAttachments;
    for (const auto& wa : attachments) {
        for (const auto& fx : wa.effects) removeWeaponEffect(fx);
        removeInstance(wa.weaponInstanceId);
    }
    for (const auto& wa : attachments) {
        unloadModelIfUnused(wa.weaponModelId);
    }

    // Its emitters' twin in M2Renderer goes with it.
    if (it->second.emitterInstanceId != 0 && effectRenderer_)
        effectRenderer_->removeInstance(it->second.emitterInstanceId);

    // Defer bone buffer destruction - in-flight command buffers may still
    // reference these descriptor sets.
    destroyInstanceBones(it->second, /*defer=*/true);

    instances.erase(it);
    ++instancesGeneration_;
}

bool CharacterRenderer::getPlayingSequence(uint32_t instanceId, pipeline::M2Sequence& out) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    auto modelIt = models.find(it->second.modelId);
    if (modelIt == models.end()) return false;
    const auto& seqs = modelIt->second.data.sequences;
    const int idx = it->second.currentSequenceIndex;
    if (idx < 0 || idx >= static_cast<int>(seqs.size())) return false;
    out = seqs[idx];
    return true;
}

bool CharacterRenderer::getAnimationState(uint32_t instanceId, uint32_t& animationId,
                                          float& animationTimeMs, float& animationDurationMs) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) {
        return false;
    }

    const CharacterInstance& instance = it->second;
    auto modelIt = models.find(instance.modelId);
    if (modelIt == models.end()) {
        return false;
    }

    const auto& sequences = modelIt->second.data.sequences;
    if (instance.currentSequenceIndex < 0 || instance.currentSequenceIndex >= static_cast<int>(sequences.size())) {
        return false;
    }

    animationId = instance.currentAnimationId;
    animationTimeMs = instance.animationTime;
    animationDurationMs = static_cast<float>(sequences[instance.currentSequenceIndex].duration);
    return true;
}

const std::vector<uint32_t>* CharacterRenderer::getFootstepEventTimes(uint32_t instanceId) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) {
        return nullptr;
    }

    const CharacterInstance& instance = it->second;
    auto modelIt = models.find(instance.modelId);
    if (modelIt == models.end()) {
        return nullptr;
    }

    const auto& eventTimes = modelIt->second.data.footstepEventTimes;
    if (instance.currentSequenceIndex < 0 ||
        instance.currentSequenceIndex >= static_cast<int>(eventTimes.size()) ||
        eventTimes[instance.currentSequenceIndex].empty()) {
        return nullptr;
    }
    return &eventTimes[instance.currentSequenceIndex];
}

bool CharacterRenderer::hasAnimation(uint32_t instanceId, uint32_t animationId) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) {
        return false;
    }

    auto modelIt = models.find(it->second.modelId);
    if (modelIt == models.end()) {
        return false;
    }

    const auto& sequences = modelIt->second.data.sequences;
    for (const auto& seq : sequences) {
        if (seq.id == animationId) {
            return true;
        }
    }
    return false;
}

bool CharacterRenderer::getAnimationSequences(uint32_t instanceId, std::vector<pipeline::M2Sequence>& out) const {
    out.clear();
    auto it = instances.find(instanceId);
    if (it == instances.end()) {
        return false;
    }

    auto modelIt = models.find(it->second.modelId);
    if (modelIt == models.end()) {
        return false;
    }

    out = modelIt->second.data.sequences;
    return !out.empty();
}

bool CharacterRenderer::getInstanceModelName(uint32_t instanceId, std::string& modelName) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) {
        return false;
    }
    auto modelIt = models.find(it->second.modelId);
    if (modelIt == models.end()) {
        return false;
    }
    modelName = modelIt->second.data.name;
    return !modelName.empty();
}

bool CharacterRenderer::findAttachmentBone(uint32_t modelId, uint32_t attachmentId,
                                          uint16_t& outBoneIndex, glm::vec3& outOffset) const {
    auto modelIt = models.find(modelId);
    if (modelIt == models.end()) return false;
    const auto& model = modelIt->second.data;

    outBoneIndex = 0;
    outOffset = glm::vec3(0.0f);
    bool found = false;

    // Try attachment lookup first
    if (attachmentId < model.attachmentLookup.size()) {
        uint16_t attIdx = model.attachmentLookup[attachmentId];
        if (attIdx < model.attachments.size()) {
            outBoneIndex = model.attachments[attIdx].bone;
            outOffset = model.attachments[attIdx].position;
            found = true;
        }
    }

    // Fallback: scan attachments by id
    if (!found) {
        for (const auto& att : model.attachments) {
            if (att.id == attachmentId) {
                outBoneIndex = att.bone;
                outOffset = att.position;
                found = true;
                break;
            }
        }
    }

    // Fallback: key-bone lookup for weapon hand attachment IDs (ID 1 = right hand, ID 2 = left hand)
    if (!found && (attachmentId == 1 || attachmentId == 2)) {
        int32_t targetKeyBone = (attachmentId == 1) ? 26 : 27;
        for (size_t i = 0; i < model.bones.size(); i++) {
            if (model.bones[i].keyBoneId == targetKeyBone) {
                outBoneIndex = static_cast<uint16_t>(i);
                outOffset = glm::vec3(0.0f);
                found = true;
                break;
            }
        }
    }

    // Fallback for head attachment (ID 11): use bone 0 if attachment not defined
    if (!found && attachmentId == 11 && !model.bones.empty()) {
        outBoneIndex = 0;
        found = true;
    }

    // Validate bone index
    if (found && outBoneIndex >= model.bones.size()) {
        found = false;
    }

    return found;
}

bool CharacterRenderer::attachWeapon(uint32_t charInstanceId, uint32_t attachmentId,
                                      const pipeline::M2Model& weaponModel, uint32_t weaponModelId,
                                      const std::string& texturePath,
                                      const glm::mat4& localTransform) {
    auto charIt = instances.find(charInstanceId);
    if (charIt == instances.end()) {
        core::Logger::getInstance().warning("attachWeapon: character instance ", charInstanceId, " not found");
        return false;
    }
    auto& charInstance = charIt->second;
    auto charModelIt = models.find(charInstance.modelId);
    if (charModelIt == models.end()) return false;

    // Find bone index for this attachment point
    uint16_t boneIndex = 0;
    glm::vec3 offset(0.0f);
    if (!findAttachmentBone(charInstance.modelId, attachmentId, boneIndex, offset)) {
        core::Logger::getInstance().warning("attachWeapon: no bone found for attachment ", attachmentId);
        return false;
    }

    // Remove existing weapon at this attachment point
    detachWeapon(charInstanceId, attachmentId);

    // Load weapon model into renderer
    if (models.find(weaponModelId) == models.end()) {
        if (!loadModel(weaponModel, weaponModelId)) {
            core::Logger::getInstance().warning("attachWeapon: failed to load weapon model ", weaponModelId);
            return false;
        }
    }

    // Apply weapon texture if provided
    if (!texturePath.empty()) {
        VkTexture* texPtr = loadTexture(texturePath);
        if (texPtr != whiteTexture_.get()) {
            // Item models can keep an authored hilt texture in slot 0 and expose
            // the DBC-selected skin through one or more replaceable slots:
            // 2 = object skin, 3 = weapon blade, 4 = weapon handle.  Applying the
            // skin to slot 0 alone leaves multi-material weapons with an untextured
            // blade (notably Melris Malagan's sword).
            bool appliedReplaceableSlot = false;
            auto modelIt = models.find(weaponModelId);
            if (modelIt != models.end()) {
                const auto& textures = modelIt->second.data.textures;
                for (uint32_t slot = 0; slot < textures.size(); ++slot) {
                    const uint32_t type = textures[slot].type;
                    if (type == 2 || type == 3 || type == 4) {
                        setModelTexture(weaponModelId, slot, texPtr);
                        appliedReplaceableSlot = true;
                    }
                }
            }
            // Older/simple item models commonly expose only one unnamed slot.
            if (!appliedReplaceableSlot) {
                setModelTexture(weaponModelId, 0, texPtr);
            }
        }
    }

    // Create weapon instance
    uint32_t weaponInstanceId = createInstance(weaponModelId, glm::vec3(0.0f));
    if (weaponInstanceId == 0) return false;

    // Mark weapon instance as override-positioned
    auto weapIt = instances.find(weaponInstanceId);
    if (weapIt != instances.end()) {
        weapIt->second.hasOverrideModelMatrix = true;
        weapIt->second.isAttachedItem = true;
        weapIt->second.opacity = charInstance.opacity;
        weapIt->second.visible = charInstance.visible;
        // An item model plays its default sequence like any loaded model
        // (stock 0x834540: Stand if it has one, else the first).
        const auto& seqs = weapIt->second.cachedModel->data.sequences;
        if (!seqs.empty()) {
            weapIt->second.currentSequenceIndex =
                anim::defaultSequenceIndex(seqs, weapIt->second.cachedModel->data.bones);
            weapIt->second.currentAnimationId = seqs[weapIt->second.currentSequenceIndex].id;
        }
    }

    // Store attachment on parent character instance
    WeaponAttachment wa;
    wa.weaponModelId = weaponModelId;
    wa.weaponInstanceId = weaponInstanceId;
    wa.attachmentId = attachmentId;
    wa.boneIndex = boneIndex;
    wa.offset = offset;
    wa.localTransform = localTransform;
    charInstance.weaponAttachments.push_back(wa);

    core::Logger::getInstance().debug("Attached weapon model ", weaponModelId,
        " to instance ", charInstanceId, " at attachment ", attachmentId,
        " (bone ", boneIndex, ", offset ", offset.x, ",", offset.y, ",", offset.z, ")");
    return true;
}

bool CharacterRenderer::getInstanceBounds(uint32_t instanceId, glm::vec3& outCenter, float& outRadius) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    auto mIt = models.find(it->second.modelId);
    if (mIt == models.end()) return false;

    const auto& inst = it->second;
    const auto& model = mIt->second.data;

    glm::vec3 boundsMin = mIt->second.visualBoundMin;
    glm::vec3 boundsMax = mIt->second.visualBoundMax;
    float radius = mIt->second.visualBoundRadius;
    if (radius <= 0.001f) {
        boundsMin = model.boundMin;
        boundsMax = model.boundMax;
        radius = model.boundRadius;
        if (radius <= 0.001f) {
            radius = glm::length(boundsMax - boundsMin) * 0.5f;
        }
    }

    float scale = std::max(0.001f, inst.scale);
    const glm::vec3 localCenter = (boundsMin + boundsMax) * 0.5f;
    glm::mat4 rotation(1.0f);
    rotation = glm::rotate(rotation, inst.rotation.z, glm::vec3(0.0f, 0.0f, 1.0f));
    rotation = glm::rotate(rotation, inst.rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));
    rotation = glm::rotate(rotation, inst.rotation.y, glm::vec3(0.0f, 1.0f, 0.0f));
    outCenter = inst.position + glm::vec3(rotation * glm::vec4(localCenter * scale, 0.0f));
    outRadius = std::max(0.5f, radius * scale);
    return true;
}

bool CharacterRenderer::getInstanceKeyBonePivotZ(uint32_t instanceId, int32_t keyBoneId,
                                                 float& outZ) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    auto mIt = models.find(it->second.modelId);
    if (mIt == models.end()) return false;

    for (const auto& bone : mIt->second.data.bones) {
        if (bone.keyBoneId != keyBoneId) continue;
        outZ = bone.pivot.z * std::max(0.001f, it->second.scale);
        return true;
    }
    return false;
}

bool CharacterRenderer::getInstanceHeight(uint32_t instanceId, float& outHeight) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    auto mIt = models.find(it->second.modelId);
    if (mIt == models.end()) return false;

    // The tight bind-pose bounds, which is where the top of the head is. The
    // M2 header's own box describes collision on a good many models and can
    // come out shorter than the thing it encloses, so it is the fallback.
    float top = mIt->second.visualBoundMax.z;
    if (top <= 0.001f) top = mIt->second.data.boundMax.z;
    if (top <= 0.001f) return false;

    outHeight = top * std::max(0.001f, it->second.scale);
    return true;
}

bool CharacterRenderer::getInstanceFootZ(uint32_t instanceId, float& outFootZ) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    auto mIt = models.find(it->second.modelId);
    if (mIt == models.end()) return false;

    const auto& inst = it->second;
    const auto& model = mIt->second.data;
    float scale = std::max(0.001f, inst.scale);
    outFootZ = inst.position.z + model.boundMin.z * scale;
    return true;
}

bool CharacterRenderer::getInstancePosition(uint32_t instanceId, glm::vec3& outPos) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    outPos = it->second.position;
    return true;
}

bool CharacterRenderer::getInstancePose(uint32_t instanceId, const std::vector<glm::mat4>*& outBones,
                                        glm::mat4& outModel, int& outSequenceIndex,
                                        float& outAnimTimeMs, float& outGlobalSeqTimeMs) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    const auto& inst = it->second;
    outBones = &inst.boneMatrices;
    outModel = inst.hasOverrideModelMatrix ? inst.overrideModelMatrix : getModelMatrix(inst);
    outSequenceIndex = inst.currentSequenceIndex;
    outAnimTimeMs = inst.animationTime;
    outGlobalSeqTimeMs = inst.globalSequenceTime;
    return true;
}

void CharacterRenderer::detachWeapon(uint32_t charInstanceId, uint32_t attachmentId) {
    auto charIt = instances.find(charInstanceId);
    if (charIt == instances.end()) return;
    auto& attachments = charIt->second.weaponAttachments;

    for (auto it = attachments.begin(); it != attachments.end(); ++it) {
        if (it->attachmentId == attachmentId) {
            // Collect model ids before erasing the attachment, then unload the
            // ones no instance still uses (each attach allocates a fresh id).
            std::vector<uint32_t> modelIds;
            modelIds.push_back(it->weaponModelId);
            for (const auto& fx : it->effects) removeWeaponEffect(fx);
            removeInstance(it->weaponInstanceId);
            attachments.erase(it);
            for (uint32_t mid : modelIds) unloadModelIfUnused(mid);
            core::Logger::getInstance().info("Detached weapon from instance ", charInstanceId,
                " attachment ", attachmentId);
            return;
        }
    }
}

void CharacterRenderer::unloadModelIfUnused(uint32_t modelId) {
    auto modelIt = models.find(modelId);
    if (modelIt == models.end()) return;
    for (const auto& p : instances) {
        if (p.second.modelId == modelId) return;
    }
    destroyModelGPU(modelIt->second, /*defer=*/true);
    models.erase(modelIt);
}

bool CharacterRenderer::attachWeaponEffect(uint32_t charInstanceId, uint32_t attachmentId,
                                           uint32_t visualSlot,
                                           const pipeline::M2Model& effectModel,
                                           uint32_t effectModelId) {
    auto charIt = instances.find(charInstanceId);
    if (charIt == instances.end()) return false;

    auto& attachments = charIt->second.weaponAttachments;
    auto waIt = std::find_if(attachments.begin(), attachments.end(),
                             [attachmentId](const WeaponAttachment& wa) {
                                 return wa.attachmentId == attachmentId;
                             });
    if (waIt == attachments.end()) return false;  // no weapon to hang the effect on

    if (models.find(effectModelId) == models.end()) {
        if (!loadModel(effectModel, effectModelId)) {
            core::Logger::getInstance().warning("attachWeaponEffect: failed to load effect model ",
                                                effectModelId);
            return false;
        }
    }

    // The ItemVisuals slot names the attachment point on the weapon model
    // itself. A weapon without it gets nothing for the slot: stock looks the
    // id up and gives up (0x831630). The origin fallback this replaces piled
    // up to five glows on the grip of 545 weapon models.
    uint16_t boneIndex = 0;
    glm::vec3 offset(0.0f);
    if (!findAttachmentStrict(waIt->weaponModelId, visualSlot, boneIndex, offset)) {
        unloadModelIfUnused(effectModelId);
        return false;
    }

    uint32_t effectInstanceId = createInstance(effectModelId, glm::vec3(0.0f));
    if (effectInstanceId == 0) return false;

    auto fxIt = instances.find(effectInstanceId);
    if (fxIt == instances.end()) return false;
    fxIt->second.hasOverrideModelMatrix = true;
    fxIt->second.isEffectModel = true;
    fxIt->second.animationLoop = true;
    fxIt->second.opacity = charIt->second.opacity;

    WeaponEffectAttachment fx;
    fx.effectModelId = effectModelId;
    fx.effectInstanceId = effectInstanceId;
    fx.boneIndex = boneIndex;
    fx.offset = offset;
    waIt->effects.push_back(fx);

    core::Logger::getInstance().debug("Attached enchant visual model ", effectModelId,
        " to weapon at attachment ", attachmentId, " (visual slot ", visualSlot, ")");
    return true;
}

void CharacterRenderer::setInstanceSceneModel(uint32_t instanceId, bool isScene) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) it->second.isSceneModel = isScene;
}

void CharacterRenderer::removeWeaponEffect(const WeaponEffectAttachment& fx) {
    if (fx.m2InstanceId != 0) {
        if (effectRenderer_) effectRenderer_->removeInstance(fx.m2InstanceId);
        return;
    }
    if (fx.effectInstanceId != 0) removeInstance(fx.effectInstanceId);
    if (fx.effectModelId != 0) unloadModelIfUnused(fx.effectModelId);
}

bool CharacterRenderer::findAttachmentStrict(uint32_t modelId, uint32_t attachmentId,
                                             uint16_t& outBoneIndex, glm::vec3& outOffset) const {
    auto modelIt = models.find(modelId);
    if (modelIt == models.end()) return false;
    const auto& model = modelIt->second.data;
    if (attachmentId >= model.attachmentLookup.size()) return false;
    const uint16_t idx = model.attachmentLookup[attachmentId];
    if (idx >= model.attachments.size()) return false;  // 0xFFFF: not on this model
    const auto& att = model.attachments[idx];
    if (att.bone >= model.bones.size()) return false;
    outBoneIndex = att.bone;
    outOffset = att.position;
    return true;
}

int CharacterRenderer::applyWeaponItemVisual(uint32_t charInstanceId, uint32_t attachmentId,
                                             uint32_t itemVisualId) {
    auto charIt = instances.find(charInstanceId);
    if (charIt == instances.end()) return 0;
    auto waIt = std::find_if(charIt->second.weaponAttachments.begin(),
                             charIt->second.weaponAttachments.end(),
                             [attachmentId](const WeaponAttachment& wa) {
                                 return wa.attachmentId == attachmentId;
                             });
    if (waIt == charIt->second.weaponAttachments.end()) return 0;
    // Same row already on: nothing to rebuild (a refresh after a re-equip of
    // the same item, or an enchant change that resolves to the same visual).
    if (waIt->itemVisualId == itemVisualId && (itemVisualId == 0 || !waIt->effects.empty()))
        return static_cast<int>(waIt->effects.size());

    // Stock 0x4EA8F0 drops every child of the item model first.
    for (const auto& fx : waIt->effects) removeWeaponEffect(fx);
    waIt->effects.clear();
    waIt->itemVisualId = itemVisualId;
    if (itemVisualId == 0 || !assetManager) return 0;

    auto visualsDbc = assetManager->loadDBC("ItemVisuals.dbc");
    auto effectsDbc = assetManager->loadDBC("ItemVisualEffects.dbc");
    if (!visualsDbc || !visualsDbc->isLoaded() || !effectsDbc || !effectsDbc->isLoaded()) return 0;
    const auto paths = pipeline::resolveItemVisualModels(itemVisualId, visualsDbc.get(), effectsDbc.get());

    const uint32_t weaponModelId = waIt->weaponModelId;
    int attached = 0;
    for (uint32_t slot = 0; slot < paths.size(); ++slot) {
        if (paths[slot].empty()) continue;
        uint16_t bone = 0;
        glm::vec3 offset(0.0f);
        if (!findAttachmentStrict(weaponModelId, slot, bone, offset)) continue;

        // DBC paths say .mdx; the shipped files are .m2. ItemVisualEffects 61
        // is a bare directory and fails here, as it does in stock.
        std::string m2Path = paths[slot];
        const size_t dot = m2Path.rfind('.');
        const size_t sep = m2Path.find_last_of("\\/");
        if (dot != std::string::npos && (sep == std::string::npos || dot > sep))
            m2Path.resize(dot);
        m2Path += ".m2";

        if (effectRenderer_) {
            const uint32_t m2ModelId =
                static_cast<uint32_t>(std::hash<std::string>{}(m2Path + "#item-visual"));
            if (!effectRenderer_->hasModel(m2ModelId)) {
                pipeline::M2Model model;
                if (!pipeline::loadM2WithSkin(*assetManager, m2Path, model)) continue;
                if (model.name.empty()) model.name = m2Path;
                if (!effectRenderer_->loadModel(model, m2ModelId)) continue;
                // Transient-style model: animated, no shadow, never deduplicated
                // by position (five copies of one glow sit on one blade).
                effectRenderer_->markModelAsSpellEffect(m2ModelId);
            }
            const uint32_t m2Inst = effectRenderer_->createInstance(
                m2ModelId, glm::vec3(charIt->second.position), glm::vec3(0.0f), 1.0f,
                /*allowPositionDedup=*/false);
            if (m2Inst == 0) continue;
            WeaponEffectAttachment fx;
            fx.m2InstanceId = m2Inst;
            fx.boneIndex = bone;
            fx.offset = offset;
            waIt->effects.push_back(fx);
            ++attached;
        } else {
            pipeline::M2Model model;
            if (!pipeline::loadM2WithSkin(*assetManager, m2Path, model)) continue;
            static uint32_t nextLocalEffectModelId = 0x70000000u;
            if (attachWeaponEffect(charInstanceId, attachmentId, slot, model, nextLocalEffectModelId++))
                ++attached;
        }
        // createInstance inserts into the instance map, and a rehash leaves
        // both iterators dangling: find them again.
        charIt = instances.find(charInstanceId);
        if (charIt == instances.end()) return attached;
        waIt = std::find_if(charIt->second.weaponAttachments.begin(),
                            charIt->second.weaponAttachments.end(),
                            [attachmentId](const WeaponAttachment& wa) {
                                return wa.attachmentId == attachmentId;
                            });
        if (waIt == charIt->second.weaponAttachments.end()) return attached;
    }
    return attached;
}

// ---------------------------------------------------------------------------
// Emitters of the models drawn here (stock: one CParticleEmitter2 system for
// every CM2Model, units and their items included; docs/re/particles.md §8)
// ---------------------------------------------------------------------------
uint32_t CharacterRenderer::ensureEmitterTwin(CharacterInstance& inst) {
    if (!effectRenderer_ || !inst.cachedModel || !inst.cachedModel->hasEmitters) return 0;
    if (inst.emitterInstanceId != 0) {
        if (effectRenderer_->hasInstance(inst.emitterInstanceId)) return inst.emitterInstanceId;
        inst.emitterInstanceId = 0;  // dropped by a renderer-wide clear: make another
    }
    auto modelIt = models.find(inst.modelId);
    if (modelIt == models.end()) return 0;
    M2ModelGPU& gpu = modelIt->second;
    if (gpu.emitterModelFailed) return 0;
    if (gpu.emitterModelId == 0 || !effectRenderer_->hasModel(gpu.emitterModelId)) {
        // Keyed by the file: every display of a creature model, and every
        // copy of an item model (they get a fresh id here per attach), share
        // one emitter model there.
        const std::string& name = gpu.data.name;
        const uint32_t key = static_cast<uint32_t>(
            std::hash<std::string>{}((name.empty() ? std::to_string(inst.modelId) : name) + "#unit-emitters"));
        if (!effectRenderer_->hasModel(key)) {
            // The emitters, their textures and the clocks they read; none of
            // the geometry, which this renderer draws.
            pipeline::M2Model twin = gpu.data;
            twin.vertices.clear();
            twin.indices.clear();
            twin.batches.clear();
            twin.collisionVertices.clear();
            twin.collisionIndices.clear();
            twin.collisionNormals.clear();
            twin.soundEvents.clear();
            twin.doodadSounds.clear();
            twin.effectSounds.clear();
            twin.lights.clear();
            twin.cameras.clear();
            if (twin.name.empty()) twin.name = "unit-emitters-" + std::to_string(inst.modelId);
            if (!effectRenderer_->loadModel(twin, key)) {
                gpu.emitterModelFailed = true;
                return 0;
            }
        }
        gpu.emitterModelId = key;
    }
    inst.emitterInstanceId = effectRenderer_->createPosedEmitterInstance(gpu.emitterModelId);
    if (inst.emitterInstanceId != 0) applyEmitterColors(inst);
    return inst.emitterInstanceId;
}

void CharacterRenderer::applyEmitterColors(const CharacterInstance& inst) {
    if (!effectRenderer_ || inst.emitterInstanceId == 0) return;
    if (inst.particleColorId == 0 || !assetManager) {
        effectRenderer_->setInstanceParticleColors(inst.emitterInstanceId, nullptr);
        return;
    }
    // 0x4EA9E0: the row's Start/Mid/End columns i go to colour index 11 + i;
    // an id with no row paints 0xFF00FF00.
    m2_particle::ParticleColorOverride colors = m2_particle::particleColorMissingRow();
    auto dbc = assetManager->loadDBC("ParticleColor.dbc");
    if (dbc && dbc->isLoaded() && dbc->getFieldCount() >= 10) {
        const int32_t row = dbc->findRecordById(inst.particleColorId);
        if (row >= 0) {
            const auto r = static_cast<uint32_t>(row);
            std::array<uint32_t, 3> start{}, mid{}, end{};
            for (uint32_t i = 0; i < 3; ++i) {
                start[i] = dbc->getUInt32(r, 1 + i);
                mid[i] = dbc->getUInt32(r, 4 + i);
                end[i] = dbc->getUInt32(r, 7 + i);
            }
            colors = m2_particle::particleColorFromRow(start, mid, end);
        }
    }
    effectRenderer_->setInstanceParticleColors(inst.emitterInstanceId, &colors);
}

void CharacterRenderer::setInstanceParticleColor(uint32_t instanceId, uint32_t particleColorId) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    if (it->second.particleColorId == particleColorId) return;
    it->second.particleColorId = particleColorId;
    applyEmitterColors(it->second);
}

void CharacterRenderer::setAttachmentParticleColor(uint32_t charInstanceId, uint32_t attachmentId,
                                                   uint32_t particleColorId) {
    auto it = instances.find(charInstanceId);
    if (it == instances.end()) return;
    for (const auto& wa : it->second.weaponAttachments) {
        if (wa.attachmentId == attachmentId) {
            setInstanceParticleColor(wa.weaponInstanceId, particleColorId);
            return;
        }
    }
}

void CharacterRenderer::poseModelEmitters() {
    if (!effectRenderer_) return;
    // Beyond it a unit is neither drawn nor posed (update's animation radius,
    // the same 130 yd as the draw's): its emitters stop with it.
    const float radius = static_cast<float>(envSizeOrDefault("WOWEE_CHAR_RENDER_RADIUS", 130));
    const float radiusSq = radius * radius;
    constexpr float kDefaultCullRadius = 4.0f;
    for (auto& pair : instances) {
        auto& inst = pair.second;
        // Weapon glows hosted there, placed where update() put them.
        for (auto& wa : inst.weaponAttachments) {
            for (auto& fx : wa.effects) {
                if (fx.m2InstanceId == 0 || !fx.posePending) continue;
                fx.posePending = false;
                effectRenderer_->setInstanceTransform(fx.m2InstanceId, fx.worldMatrix);
                effectRenderer_->setInstanceFade(fx.m2InstanceId, fx.fade);
            }
        }

        if (inst.isEffectModel || inst.isSceneModel) continue;
        if (!inst.cachedModel || !inst.cachedModel->hasEmitters) continue;
        // An item model before its owner first placed it has no matrix yet.
        if (inst.isAttachedItem && !inst.hasOverrideModelMatrix) continue;
        const uint32_t twin = ensureEmitterTwin(inst);
        if (twin == 0) continue;

        M2Renderer::EmitterPose pose;
        pose.model = inst.hasOverrideModelMatrix ? inst.overrideModelMatrix : getModelMatrix(inst);
        pose.bones = &inst.boneMatrices;
        pose.sequenceIndex = std::max(inst.currentSequenceIndex, 0);
        pose.animTimeMs = inst.animationTime;
        pose.globalSeqTimeMs = inst.globalSequenceTime;
        pose.alpha = inst.opacity;
        const glm::vec3 origin(pose.model[3]);
        pose.visible = inst.visible && glm::distance2(origin, lastCameraPos_) <= radiusSq;
        // The draw's bounds: the model's box through its matrix.
        const auto& gm = *inst.cachedModel;
        const float scale = glm::length(glm::vec3(pose.model[0]));
        if (gm.visualBoundRadius > 0.0f) {
            const glm::vec3 c = 0.5f * (gm.visualBoundMin + gm.visualBoundMax);
            pose.cullCenter = glm::vec3(pose.model * glm::vec4(c, 1.0f));
            pose.cullRadius = std::max(gm.visualBoundRadius * scale, 0.25f);
        } else {
            pose.cullCenter = origin;
            pose.cullRadius = kDefaultCullRadius;
        }
        if (!effectRenderer_->setPosedEmitterPose(twin, pose)) inst.emitterInstanceId = 0;
    }
}

bool CharacterRenderer::getAttachmentTransform(uint32_t instanceId, uint32_t attachmentId, glm::mat4& outTransform) {
    auto instIt = instances.find(instanceId);
    if (instIt == instances.end()) return false;
    // A mount seat, a spell's hand: whoever asks gets the pose of now, even for
    // a unit the view cull left unposed.
    refreshStalePose(instIt->second);
    const auto& instance = instIt->second;

    // Find attachment point using shared lookup logic
    uint16_t boneIndex = 0;
    glm::vec3 offset(0.0f);
    if (!findAttachmentBone(instance.modelId, attachmentId, boneIndex, offset)) {
        return false;
    }

    // Get bone matrix
    glm::mat4 boneMat(1.0f);
    if (boneIndex < instance.boneMatrices.size()) {
        boneMat = instance.boneMatrices[boneIndex];
    }

    // Compute world transform: modelMatrix * boneMatrix * offsetTranslation
    glm::mat4 modelMat = instance.hasOverrideModelMatrix
        ? instance.overrideModelMatrix
        : getModelMatrix(instance);

    outTransform = modelMat * boneMat * glm::translate(glm::mat4(1.0f), offset);
    return true;
}

void CharacterRenderer::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();

    // Destroy old main-pass pipelines (NOT shadow, NOT pipeline layout)
    destroy(device, opaquePipeline_);
    destroy(device, alphaTestPipeline_);
    destroy(device, alphaPipeline_);
    destroy(device, additivePipeline_);
    destroy(device, translucentPipeline_);
    destroy(device, addOnePipeline_);
    destroy(device, modPipeline_);
    destroy(device, mod2xPipeline_);

    // --- Load shaders ---
    rendering::VkShaderModule charVert, charFrag;
    if (!charVert.loadFromFile(device, "assets/shaders/character.vert.spv") ||
        !charFrag.loadFromFile(device, "assets/shaders/character.frag.spv")) {
        LOG_ERROR("CharacterRenderer::recreatePipelines: missing required shaders");
        return;
    }

    VkRenderPass mainPass = renderPassOverride_ ? renderPassOverride_ : vkCtx_->getImGuiRenderPass();
    VkSampleCountFlagBits samples = renderPassOverride_ ? msaaSamplesOverride_ : vkCtx_->getMsaaSamples();

    LOG_INFO("CharacterRenderer::recreatePipelines: renderPass=", (void*)mainPass,
             " samples=", static_cast<int>(samples),
             " pipelineLayout=", (void*)pipelineLayout_);

    buildMainPassPipelines(device, mainPass, samples, charVert, charFrag);

    charVert.destroy();
    charFrag.destroy();

    if (!opaquePipeline_ || !alphaTestPipeline_ || !alphaPipeline_ || !additivePipeline_ ||
        !translucentPipeline_) {
        LOG_ERROR("CharacterRenderer::recreatePipelines FAILED: opaque=", (void*)opaquePipeline_,
                  " alphaTest=", (void*)alphaTestPipeline_,
                  " alpha=", (void*)alphaPipeline_,
                  " additive=", (void*)additivePipeline_,
                  " renderPass=", (void*)mainPass, " samples=", static_cast<int>(samples));
    } else {
        LOG_INFO("CharacterRenderer: pipelines recreated successfully (samples=",
                 static_cast<int>(samples), ")");
    }
}

} // namespace rendering
} // namespace wowee

namespace wowee {
namespace rendering {

void CharacterRenderer::collectUnitBlobs(const glm::vec3& cameraPos, float maxDist, size_t maxCount,
                                         std::vector<BlobSource>& out, bool skipSunShadowCasters,
                                         const Frustum* view) const {
    out.clear();
    std::vector<std::pair<float, BlobSource>> found;
    const float maxSq = maxDist * maxDist;
    for (const auto& [id, inst] : instances) {
        if (!inst.visible || inst.isAttachedItem || inst.isEffectModel || inst.isSceneModel ||
            inst.hasOverrideModelMatrix || inst.mountedOn != 0) {
            continue;
        }
        if (skipSunShadowCasters && inst.sunShadowStamp == sunShadowStamp_) continue;
        const M2ModelGPU* model = inst.cachedModel;
        if (!model) {
            auto it = models.find(inst.modelId);
            if (it == models.end()) continue;
            model = &it->second;
        }
        const glm::vec3 d = inst.position - cameraPos;
        const float distSq = glm::dot(d, d);
        if (distSq > maxSq) continue;
        // The box half-extents about the origin, scaled, each corner held to
        // +/- 5 yards (0x7E4480). The visual bind-pose box stands in for
        // CreatureModelData's GeoBox.
        const glm::vec3 lo = model->visualBoundMin, hi = model->visualBoundMax;
        if (!(hi.x > lo.x) || !(hi.y > lo.y) || !(hi.z > lo.z)) continue;
        const float s = inst.scale > 0.0f ? inst.scale : 1.0f;
        const auto clamp5 = [](float v) { return std::clamp(v, -5.0f, 5.0f); };
        BlobSource b;
        b.feet = inst.position;
        b.forward = glm::vec2(std::cos(inst.rotation.z), std::sin(inst.rotation.z));
        b.halfX = std::min((hi.x - lo.x) * 0.5f * s, 5.0f);
        b.halfY = std::min((hi.y - lo.y) * 0.5f * s, 5.0f);
        b.halfZ = (clamp5(hi.z * s) - clamp5(lo.z * s)) * 0.5f;
        b.alpha = std::clamp(inst.opacity, 0.0f, 1.0f);
        if (b.halfX <= 0.01f || b.halfY <= 0.01f || b.halfZ <= 0.01f) continue;
        // The blob is draped over the floor within its box round the feet
        // (OverlaySystem::renderUnitBlobs): a lattice r across and from
        // 1.67 halfZ below the feet to halfZ above. A sphere round the feet
        // holds all of it, with a yard over for the lattice's rounding.
        if (view) {
            const float r = std::sqrt(b.halfX * b.halfX + b.halfY * b.halfY);
            const float down = 1.6667f * b.halfZ;
            if (!view->intersectsSphere(b.feet, std::sqrt(r * r + down * down) + 1.0f)) continue;
        }
        found.emplace_back(distSq, b);
    }
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    if (found.size() > maxCount) found.resize(maxCount);
    out.reserve(found.size());
    for (auto& f : found) out.push_back(f.second);
}

}  // namespace rendering
}  // namespace wowee
