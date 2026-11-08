#pragma once

#include <vector>
#include <memory>
#include <optional>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include "rendering/vk_texture.hpp"

namespace wowee {
namespace pipeline {
    class AssetManager;
    struct ADTTerrain;
    struct LiquidData;
    struct WMOLiquid;
}

namespace rendering {

class Camera;
class VkContext;
struct LightingParams;

/**
 * Water surface for a single map chunk
 */
struct WaterSurface {
    glm::vec3 position;
    glm::vec3 origin;
    glm::vec3 stepX;
    glm::vec3 stepY;
    float minHeight;
    float maxHeight;
    uint16_t liquidType;

    int tileX = -1, tileY = -1;
    uint32_t wmoId = 0;

    uint8_t xOffset = 0;
    uint8_t yOffset = 0;
    uint8_t width = 8;
    uint8_t height = 8;

    std::vector<float> heights;
    std::vector<uint8_t> mask;
    /// Per vertex, the stock depth byte (docs/re/water.md): the depth ramp's
    /// row is byte / 42 for river types and byte / 255 for ocean and WMO
    /// ones. Empty for magma and slime.
    std::vector<uint8_t> depths;
    /// Per vertex, the liquid texture's uv when the data carries one (magma
    /// and slime) or the WMO cell grid gives it; two floats a vertex. Empty:
    /// the shader takes world x,y x 0.06 (0x7ce390).
    std::vector<float> uvs;
    /// The vertex colour (attrib4): white, or for a WMO group that is not
    /// exterior the liquid's MOMT colour (0x7a6d70); and that group's fixed
    /// interior light (0x7d4f40: every colour 1).
    glm::vec4 vertexColor{1.0f};
    bool interior = false;

    // Vulkan render data
    ::VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc = VK_NULL_HANDLE;
    ::VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAlloc = VK_NULL_HANDLE;
    int indexCount = 0;

    // Per-surface material UBO
    ::VkBuffer materialUBO = VK_NULL_HANDLE;
    VmaAllocation materialAlloc = VK_NULL_HANDLE;

    // Material descriptor set (set 1)
    VkDescriptorSet materialSet = VK_NULL_HANDLE;

    [[nodiscard]] bool hasHeightData() const { return !heights.empty(); }
};

/**
 * Water renderer (Vulkan) with planar reflections, Gerstner waves,
 * GGX specular, shoreline foam, and subsurface scattering.
 */
// Matches set 2 binding 3 in water.frag.glsl. Keep kMaxWakePoints in step with
// the MAX_WAKE_POINTS constant declared there.
constexpr int kMaxWakePoints = 32;

struct WaterFrameUBOData {
    glm::mat4 reflViewProj{1.0f};
    // The view-projection the scene depth/colour copy (set 2 bindings 0-1)
    // was rendered with. With MSAA or an upscaler the copy is last frame's,
    // so a lookup at this frame's screen position read the scene from where
    // the camera was: the shoreline band and caustics trailed a moving
    // camera. The shaders project the surface with this to find its pixel
    // in the copy. Equal to this frame's matrix when the copy is fresh.
    glm::mat4 historyViewProj{1.0f};
    glm::vec4 wakeBounds{0.0f};              // xy = centre, z = cull radius, w = count
    glm::vec4 wakePoints[kMaxWakePoints]{};  // xy = pos, z = age 0..1, w = strength
    // Light.dbc liquid colours: rgb, a = LightParams' shallow alpha on the
    // close colour and deep alpha on the far one. LightParams 1 at noon until
    // the lighting first arrives.
    glm::vec4 riverClose{30.0f / 255.0f, 48.0f / 255.0f, 62.0f / 255.0f, 0.5f};
    glm::vec4 riverFar{0.0f, 19.0f / 255.0f, 23.0f / 255.0f, 1.0f};
    glm::vec4 oceanClose{30.0f / 255.0f, 48.0f / 255.0f, 62.0f / 255.0f, 0.75f};
    glm::vec4 oceanFar{0.0f, 19.0f / 255.0f, 23.0f / 255.0f, 1.0f};
    // HD water (water_hd.frag; water.frag declares the block only this far,
    // so nothing below changes the stock surface). x: 1 while the row is on;
    // y: 1 when this frame's colour copy holds the scene without the water
    // (its refraction reads it); zw: the share of
    // the colour copy that copy filled, for the screen uv.
    glm::vec4 hdParams{0.0f};
    // Light.dbc's sky (the skybox's rings, LightIntBand 2-7): the reflected
    // sky where nothing on screen is mirrored.
    glm::vec4 skyTop{0.5f, 0.7f, 1.0f, 0.0f};
    glm::vec4 skyMiddle{0.7f, 0.85f, 1.0f, 0.0f};
    glm::vec4 skyBand1{0.9f, 0.95f, 1.0f, 0.0f};
    glm::vec4 skyBand2{1.0f, 0.98f, 0.9f, 0.0f};
    glm::vec4 skySmog{0.7f, 0.7f, 0.7f, 0.0f};
    glm::vec4 skyFog{0.5f, 0.6f, 0.7f, 0.0f};
};

class WaterRenderer {
public:
    WaterRenderer();
    ~WaterRenderer();

    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    void shutdown();

    void loadFromTerrain(const pipeline::ADTTerrain& terrain, bool append = false,
                         int tileX = -1, int tileY = -1);

    /// `momtColor` is the liquid's MOMT colour (material materialId, +0x1c),
    /// the vertex colour of a group that is not exterior.
    void loadFromWMO(const pipeline::WMOLiquid& liquid, const glm::mat4& modelMatrix, uint32_t wmoId,
                     uint32_t momtColor = 0xFFFFFFFFu);

    /// Load the animated texture sequences LiquidType.dbc names for each
    /// liquid (XTextures\river\lake_a.%d.blp and the rest). Once; later
    /// calls do nothing.
    void loadLiquidTextures(pipeline::AssetManager* assetManager);

    /// The water's colours and alphas for this frame, from Light.dbc.
    void setLiquidLighting(const LightingParams& lighting);
    void removeWMO(uint32_t wmoId);
    void removeTile(int tileX, int tileY);
    void clear();

    void recreatePipelines();

    // Separate 1x pass for MSAA mode - water rendered after MSAA resolve
    bool createWater1xPass(VkFormat colorFormat, VkFormat depthFormat);
    void createWater1xFramebuffers(const std::vector<VkImageView>& swapViews,
                                    VkImageView depthView, VkExtent2D extent);
    void destroyWater1xResources();
    bool hasWater1xPass() const { return water1xRenderPass != VK_NULL_HANDLE; }
    VkRenderPass getWater1xRenderPass() const { return water1xRenderPass; }
    /// The same pass (compatible with every pipeline made for it) that stores
    /// the depth it loads instead of discarding it: for an off-screen scene's
    /// resolved depth, which the post-processing reads after the water. Made
    /// with the 1x pass; the soft particles' water continuation uses it.
    VkRenderPass getWater1xKeepDepthRenderPass() const { return water1xKeepDepthRenderPass_; }
    VkFramebuffer getWater1xFramebuffer(uint32_t index) const {
        return index < water1xFramebuffers.size() ? water1xFramebuffers[index] : VK_NULL_HANDLE;
    }

    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera, float time, bool use1x = false, uint32_t frameIndex = 0);
    void captureSceneHistory(VkCommandBuffer cmd,
                             VkImage srcColorImage,
                             VkImage srcDepthImage,
                             VkExtent2D srcExtent,
                             bool srcDepthIsMsaa,
                             uint32_t frameIndex = 0);

    // --- Planar reflection pass ---
    // Call sequence: beginReflectionPass → [render scene] → endReflectionPass
    bool beginReflectionPass(VkCommandBuffer cmd);
    void endReflectionPass(VkCommandBuffer cmd);

    // Get the dominant water height near a position (for reflection plane)
    std::optional<float> getDominantWaterHeight(const glm::vec3& cameraPos) const;

    // Compute reflected view matrix for a given water height
    static glm::mat4 computeReflectedView(const Camera& camera, float waterHeight);
    // Compute oblique clip projection to clip below-water geometry in reflection
    static glm::mat4 computeObliqueProjection(const glm::mat4& proj, const glm::mat4& view, float waterHeight);

    // Update the reflection UBO with reflected viewProj matrix
    void updateReflectionUBO(const glm::mat4& reflViewProj);
    /// No reflection this frame: the matrix goes to zero, which the shader
    /// reads as "nothing drawn" (w <= 0), so a pass that did not run - the
    /// setting off, the camera under the surface - leaves no stale sky.
    /// This frame's view-projection, as the scene is rendered with it; the
    /// scene copy taken this frame is tagged with it (historyViewProj).
    void setFrameViewProj(const glm::mat4& viewProj) { frameViewProj_ = viewProj; }
    void clearReflection() { frameUBO_.reflViewProj = glm::mat4(0.0f); }

    /// Feed the surface disturbance left by something moving through the water.
    /// `intensity` is 0 when nothing is disturbing the surface. `wading` picks
    /// churned-up froth underfoot; swimming instead lays a V wake off the
    /// shoulders. Call once per frame.
    void updateWake(float deltaTime, const glm::vec2& pos, const glm::vec2& travelDir,
                    float intensity, bool wading);

    VkRenderPass getReflectionRenderPass() const { return reflectionRenderPass; }
    VkExtent2D getReflectionExtent() const { return {.width = REFLECTION_WIDTH, .height = REFLECTION_HEIGHT}; }
    bool hasReflectionPass() const { return reflectionRenderPass != VK_NULL_HANDLE; }
    bool hasSurfaces() const { return !surfaces.empty(); }

    void setEnabled(bool enabled) { renderingEnabled = enabled; }
    bool isEnabled() const { return renderingEnabled; }

    void setRefractionEnabled(bool enabled);
    bool isRefractionEnabled() const { return refractionEnabled; }
    /// The foam band along the shore (water.frag, push.shoreFoam). Not stock;
    /// the "Shoreline foam" setting.
    /// The foam is the only reader of the scene's depth copy, so the copy is
    /// taken only while it is on.
    void setShorelineFoam(bool enabled) {
        shorelineFoam_ = enabled;
        updateSceneCopy();
    }
    /// HD water (E34, water_hd.frag): the row. On, the water and ocean are
    /// drawn with the HD pipelines (made on first use, let go when it goes
    /// off) and the scene's depth copy is taken for them; where the water
    /// draws after the scene pass the copy takes the colour as well. Off is
    /// water.frag and no colour copy.
    void setHdWater(bool on);
    [[nodiscard]] bool hdWater() const { return hdWater_; }
    /// Sunlight under water (E15, water_caustics.hpp): the row, which keeps
    /// the caustics pipelines (made on first use, let go when it goes off),
    /// and each frame its strength by the sun's height, 0 at night. While the
    /// strength is above 0 the scene's depth copy is taken for it as well.
    void setCausticsEnabled(bool on);
    void setCaustics(float strength) {
        causticsStrength_ = causticsEnabled_ ? strength : 0.0f;
        updateSceneCopy();
    }
    /// Rain ripples (water.frag, push.rainRipple): how hard it is raining for
    /// the rings, 0..1. 0 - the "Rain ripples" setting off, or dry - draws
    /// the stock surface. Set each frame by the renderer.
    void setRainRipple(float strength) { rainRipple_ = strength; }
    /// The underwater motes a liquid asks for (LiquidType Flags 0x8 and the
    /// particle columns); `show` false when the row has none.
    struct ParticulateInfo { bool show = false; float scale = 1.0f; int movement = 0; int texSlots = 0; bool fogged = false; };
    [[nodiscard]] ParticulateInfo particulatesFor(uint16_t liquidType) const {
        const LiquidInfo info = resolveLiquid(liquidType);
        return {(info.flags & 0x8u) != 0, info.particleScale, info.particleMovement,
                info.particleTexSlots, (info.flags & 0x10u) != 0};
    }
    /// The wake froth (not stock) off: no points, nothing drawn.
    void clearWake();
    // Display brightness (1.0 = neutral). The scene-history capture used for
    // refraction bakes this in, so the shader divides it back out.
    // Size of the target the water is drawn into. Screen-space lookups derive
    // their UVs from this rather than from the refraction texture's own size,
    // which is deliberately smaller than the frame.
    void setRenderExtent(VkExtent2D e) { renderExtent_ = e; }

    /// Where a point sits on a surface, if that surface has water there:
    /// the projection and the render mask, which every query asks first.
    std::optional<glm::vec2> wateredGridPosition(const WaterSurface& surface,
                                                 float glX, float glY) const;

    std::optional<float> getWaterHeightAt(float glX, float glY) const;
    /// Like getWaterHeightAt but only returns water surfaces whose height is
    /// close to the query Z (within maxAbove units above). Avoids false
    /// underwater detection from elevated WMO water far above the camera.
    std::optional<float> getNearestWaterHeightAt(float glX, float glY, float queryZ, float maxAbove = 15.0f) const;
    std::optional<uint16_t> getWaterTypeAt(float glX, float glY) const;

    int getSurfaceCount() const { return static_cast<int>(surfaces.size()); }

private:
    void createWaterMesh(WaterSurface& surface);
    void destroyWaterMesh(WaterSurface& surface);

    // What LiquidType.dbc says about a liquid: its basic type (0 water,
    // 1 ocean, 2 magma, 3 slime) and which texture sequence it draws.
    //
    // The rest is how the stock client animates that texture (0x8a27c0 copies
    // these out of the row; 0x8a5590 and 0x8a6090 use them): the sequence's
    // period in ms (Int[1], field 42 - 1250 for every water and ocean; magma
    // and slime always 1250), the texture's scale (Float[0], field 23; 1, or
    // 0.25 for WMO ocean) on the water material, and on magma and slime its
    // scroll in repeats per second (Float[0] and Float[1]).
    struct LiquidInfo {
        uint8_t basicType = 0;
        int textureSet = -1;
        uint32_t framePeriodMs = 1250;
        float texScale = 1.0f;
        float scrollU = 0.0f;
        float scrollV = 0.0f;
        /// Which depth ramp texture 2 (field 16) names: 0 proceduralRiverDepthTex
        /// (river close -> far), 1 proceduralOceanDepthTex (ocean close ->
        /// far), 2 proceduralWmoWaterTex (river far alone, 0x8a2ac0). Only the
        /// alpha runs shallow -> deep on the WMO ramp.
        uint8_t depthRamp = 0;
        /// LiquidType Flags (field 2) and the particulate columns: 0x8 shows
        /// the underwater motes, 0x10 fogs them; ParticleScale (11),
        /// ParticleMovement (12) and ParticleTexSlots (13).
        uint32_t flags = 0;
        /// The depth byte's divisor for the ramp row: 42 for LiquidType Int[0]
        /// 0 (the river table 0xcdf7d0), 255 for 1 (ocean and WMO, 0xcdfbd0).
        /// Applied per vertex when the mesh is built.
        float depthDivisor = 255.0f;
        /// Float[2]: the ramp coordinate's scale (stage 0 matrix); 1 bar
        /// Orange Slime's 8.5.
        float rampScale = 1.0f;
        /// The stage 1 rotation, as stock computes it: Float[1] x 57.29578
        /// taken as radians (0x9ed910 - a degrees/radians slip). Only WMO
        /// Ocean has one: 180 -> about 145.14 degrees.
        float texRotCos = 1.0f;
        float texRotSin = 0.0f;
        float particleScale = 1.0f;
        int particleMovement = 0;
        int particleTexSlots = 0;
    };
    LiquidInfo resolveLiquid(uint16_t liquidType) const;
    int loadLiquidTextureSequence(pipeline::AssetManager* assetManager, const std::string& pattern);
    bool createLiquidTextureResources();
    void destroyLiquidTextureResources();

    struct LiquidTextureSet {
        std::vector<std::unique_ptr<VkTexture>> frames;
        std::vector<VkDescriptorSet> sets;  // one per frame, set 3 binding 0
    };
    std::vector<LiquidTextureSet> liquidTextureSets_;
    std::unordered_map<std::string, int> liquidTextureSetByPattern_;
    std::unordered_map<uint16_t, LiquidInfo> liquidInfoById_;
    int defaultTextureSetByBasic_[4] = {-1, -1, -1, -1};
    VkDescriptorSetLayout liquidTexSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool liquidTexPool_ = VK_NULL_HANDLE;
    static constexpr uint32_t MAX_LIQUID_TEXTURE_SETS = 512;
    // Bound for a liquid with no texture, so set 3 is never left empty.
    std::unique_ptr<VkTexture> blankLiquidTexture_;
    VkDescriptorSet blankLiquidSet_ = VK_NULL_HANDLE;
    bool liquidTexturesLoaded_ = false;


    void updateMaterialUBO(WaterSurface& surface);
    VkDescriptorSet allocateMaterialSet();
    VkExtent2D refractionCaptureExtent() const;
    void createSceneHistoryResources(VkExtent2D extent, VkFormat colorFormat, VkFormat depthFormat);
    void destroySceneHistoryResources();

    // Reflection pass resources
    void createReflectionResources();
    void destroyReflectionResources();

    VkContext* vkCtx = nullptr;

    // Pipeline
    VkPipeline waterPipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool materialDescPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout sceneSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool sceneDescPool = VK_NULL_HANDLE;
    static constexpr uint32_t MAX_WATER_SETS = 16384;

    VkSampler sceneColorSampler = VK_NULL_HANDLE;
    VkSampler sceneDepthSampler = VK_NULL_HANDLE;
    // Per-frame scene history to avoid race between frames in flight
    static constexpr uint32_t SCENE_HISTORY_FRAMES = 2;
    // One reflection/wake UBO slot per frame in flight, at an offset every
    // device accepts for a uniform buffer (Vulkan caps the alignment at 256).
    static constexpr VkDeviceSize kFrameUBOStride =
        (sizeof(WaterFrameUBOData) + 255) / 256 * 256;
    struct PerFrameSceneHistory {
        VkImage colorImage = VK_NULL_HANDLE;
        VmaAllocation colorAlloc = VK_NULL_HANDLE;
        VkImageView colorView = VK_NULL_HANDLE;
        VkImage depthImage = VK_NULL_HANDLE;
        VmaAllocation depthAlloc = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
        VkDescriptorSet sceneSet = VK_NULL_HANDLE;
    };
    PerFrameSceneHistory sceneHistory[SCENE_HISTORY_FRAMES];
    VkExtent2D sceneHistoryExtent = {.width = 0, .height = 0};
    bool sceneHistoryReady = false;
    mutable uint32_t renderDiagCounter_ = 0;

    // Planar reflection resources
    static constexpr uint32_t REFLECTION_WIDTH = 512;
    static constexpr uint32_t REFLECTION_HEIGHT = 512;
    VkRenderPass reflectionRenderPass = VK_NULL_HANDLE;
    VkFramebuffer reflectionFramebuffer = VK_NULL_HANDLE;
    VkImage reflectionColorImage = VK_NULL_HANDLE;
    VmaAllocation reflectionColorAlloc = VK_NULL_HANDLE;
    VkImageView reflectionColorView = VK_NULL_HANDLE;
    VkImage reflectionDepthImage = VK_NULL_HANDLE;
    VmaAllocation reflectionDepthAlloc = VK_NULL_HANDLE;
    VkImageView reflectionDepthView = VK_NULL_HANDLE;
    VkSampler reflectionSampler = VK_NULL_HANDLE;
    VkImageLayout reflectionColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Reflection UBO (mat4 reflViewProj)
    // Surface disturbance trail. Points age out, spreading as they go; the
    // per-point drift is what turns a swimmer's pair of emissions into a V.
    struct WakePoint {
        glm::vec2 pos{0.0f};
        glm::vec2 drift{0.0f};
        float age = 0.0f;        // seconds
        float life = 1.0f;       // seconds
        float strength = 0.0f;
    };
    std::vector<WakePoint> wakePoints_;
    WaterFrameUBOData frameUBO_{};
    glm::mat4 frameViewProj_{1.0f};
    glm::mat4 historyViewProj_[SCENE_HISTORY_FRAMES] = {glm::mat4(1.0f), glm::mat4(1.0f)};
    glm::vec2 lastWakeEmitPos_{0.0f};
    bool hasWakeEmitPos_ = false;

    void uploadFrameUBO();

    ::VkBuffer reflectionUBO = VK_NULL_HANDLE;
    VmaAllocation reflectionUBOAlloc = VK_NULL_HANDLE;
    void* reflectionUBOMapped = nullptr;

    // Separate 1x water pass (used when MSAA is active)
    VkRenderPass water1xRenderPass = VK_NULL_HANDLE;
    VkRenderPass water1xKeepDepthRenderPass_ = VK_NULL_HANDLE;
    VkPipeline water1xPipeline = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> water1xFramebuffers;

    // Sunlight under water (water_caustics.frag): the same surfaces again,
    // before the water, in the scene pass and in the 1x pass. Made on first
    // use, gone while the row is off.
    VkPipeline causticsPipeline_ = VK_NULL_HANDLE;
    VkPipeline caustics1xPipeline_ = VK_NULL_HANDLE;
    bool causticsEnabled_ = false;
    bool causticsBuildFailed_ = false;
    float causticsStrength_ = 0.0f;
    VkPipeline buildCausticsPipeline(VkRenderPass pass, VkSampleCountFlagBits samples);
    void destroyCausticsPipelines(bool deferred);

    // HD water (water_hd.frag): the water's pipeline and its 1x sibling,
    // same layout, vertex format and blend. Made on first use, gone while
    // the row is off.
    VkPipeline hdPipeline_ = VK_NULL_HANDLE;
    VkPipeline hd1xPipeline_ = VK_NULL_HANDLE;
    bool hdWater_ = false;
    bool hdBuildFailed_ = false;
    // Set by captureSceneHistory when it copied the scene's colour (taken
    // just before the water, in the same frame), cleared once render() has
    // told the shader.
    bool colorCopiedThisFrame_ = false;
    glm::vec2 colorCopyUvScale_{1.0f};
    VkPipeline buildHdPipeline(VkRenderPass pass, VkSampleCountFlagBits samples);
    void destroyHdPipelines(bool deferred);
    void destroyPipelinePair(VkPipeline& scenePipeline, VkPipeline& pipeline1x, bool deferred);
    void updateSceneCopy() {
        setRefractionEnabled(shorelineFoam_ || causticsStrength_ > 0.0f || hdWater_);
    }

    std::vector<WaterSurface> surfaces;
    bool renderingEnabled = true;
    bool refractionEnabled = false;
    bool shorelineFoam_ = true;
    float rainRipple_ = 0.0f;
    VkExtent2D renderExtent_{.width = 0, .height = 0};
};

} // namespace rendering
} // namespace wowee
