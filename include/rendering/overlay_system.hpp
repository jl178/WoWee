#pragma once

#include "rendering/vk_texture.hpp"
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace rendering {

class VkContext;

/// Manages selection circle and fullscreen overlay Vulkan pipelines.
/// Extracted from Renderer to isolate overlay rendering resources.
class OverlaySystem {
public:
    /// Height query callable: returns floor height at (x, y) or (x, y, probeZ).
    using HeightQuery2D = std::function<std::optional<float>(float x, float y)>;
    using HeightQuery3D = std::function<std::optional<float>(float x, float y, float probeZ)>;

    explicit OverlaySystem(VkContext* ctx);
    ~OverlaySystem();

    OverlaySystem(const OverlaySystem&) = delete;
    OverlaySystem& operator=(const OverlaySystem&) = delete;

    // Selection circle - the stock client's: Textures\UnitSelectTexture.blp
    // projected straight down onto whatever floor lies within `radius` of the
    // unit's feet (terrain, WMO, M2), additive, faded by height through the
    // box. `pos` is the unit's feet; `radius` is the half-width of the
    // projected square (CGUnit_C +0xB0C, see GameScreen), not the ring's.
    // `alpha` scales the texture's (the stock colour's alpha byte).
    // attackPulse in [0, 1) is how far through its half second the ring
    // that closes in on a unit the player has just been told to attack is
    // (Wow.exe 0x725980); below 0 there is none.
    void setSelectionCircle(const glm::vec3& pos, float radius, const glm::vec3& color,
                            float alpha = 1.0f, float attackPulse = -1.0f);
    void clearSelectionCircle();
    /// Draws the selection circle and the spell targeting circle, whichever
    /// are set.
    void renderSelectionCircle(const glm::mat4& view, const glm::mat4& projection,
                               VkCommandBuffer cmd,
                               const HeightQuery2D& terrainHeight,
                               const HeightQuery3D& wmoHeight,
                               const HeightQuery3D& m2Height);

    // Spell targeting circle - the stock one (WorldFrame 0x4F8A40, set each
    // frame by 0x4F66C0): Interface\SpellShadow\Spell-Shadow-Acceptable.blp,
    // or Spell-Shadow-Unacceptable.blp out of range, alpha blended in white
    // through the same projector as the selection circle, over the box
    // centre +/- halfWidth in x and y and +/- 2 in z, unrotated.
    void setSpellTargetDecal(const glm::vec3& center, float halfWidth, bool acceptable);
    void clearSpellTargetDecal();

    /// One unit's stock blob shadow (docs/re/characters.md section 6, "Unit
    /// blob shadow: full rule"): Textures\ShadowBlob.blp under the model,
    /// a rectangle +/- halfX along its facing and +/- halfY across it,
    /// multiplied into the floor (GxBlend Mod) through the ShadowMod height
    /// ramp over feet.z - 1.6667 h .. feet.z + h, faded by model alpha.
    struct UnitBlob {
        glm::vec3 feet{0.0f};   ///< the model's origin
        glm::vec2 forward{1.0f, 0.0f};  ///< unit facing direction in xy
        float halfX = 0.5f;     ///< along the facing, yards, <= 5
        float halfY = 0.5f;     ///< across it
        float halfZ = 1.0f;     ///< h: half the scaled box height
        float alpha = 1.0f;     ///< model alpha
    };
    /// The units that get a blob this frame (empty with the sun shadows
    /// enhancement on, which is the stock extShadowQuality >= 1 case).
    void setUnitBlobs(std::vector<UnitBlob> blobs) { unitBlobs_ = std::move(blobs); }
    /// Load the texture and build the pipeline. Outside command recording
    /// (a one-off upload submits to the queue); called from the update while
    /// blobs are wanted, and a no-op once done.
    void prepareUnitBlobs();

    // Fullscreen color overlay (underwater tint, etc.)
    void renderOverlay(const glm::vec4& color, VkCommandBuffer cmd);

    /// Underwater tint bounded by a waterline that sweeps across the view as the
    /// camera crosses the surface. lineNdc runs from +1 (line off the bottom, so
    /// nothing is tinted) to -1 (off the top, so everything is), softness spans
    /// the meniscus and rippleAmp bends the edge.
    void renderWaterline(const glm::vec4& color, const glm::mat4& invViewProj,
                         float waterZ, float softness, float rippleAmp,
                         float time, bool hasSeam, VkCommandBuffer cmd);

    // Fullscreen multiplicative brightness scale (scene.rgb *= scale). Uses a
    // dst-color blend so brightness > 1 truly scales luminance instead of
    // lerping toward white (which washed everything out). scale > 1 only.
    void renderBrightnessScale(float scale, VkCommandBuffer cmd);

    /// Destroy all Vulkan resources (called before VkContext teardown).
    void cleanup();

    /// Recreate pipelines after swapchain resize / MSAA change.
    void recreatePipelines();

    /// Cells per side of the floor lattice the selection decal is draped over.
    static constexpr int kSelGridCells = 12;
    static constexpr int kSelGridVerts = kSelGridCells + 1;
    /// Vertex-buffer slices, one per frame in flight (static_assert'd in the .cpp).
    static constexpr uint32_t kSelFrameSlices = 3;

private:
    // A patch of floor samples a decal is draped over, and the floor heights
    // found for it. The decal is draped over a world-aligned lattice so a
    // lattice point keeps its world position while the decal moves, and only
    // the new edge of the patch has to be raycast. Each point keeps every
    // floor the three queries found, and the highest inside the box is picked
    // per frame. The cache drops when the spacing changes, when the box moves
    // far enough vertically for the WMO/M2 probes to answer differently, and
    // periodically in case geometry streams in.
    struct FloorSample {
        std::optional<float> terrain;
        std::optional<float> wmo;
        std::optional<float> m2;
    };
    struct FloorLattice {
        std::unordered_map<uint64_t, FloorSample> cache;
        float spacing = 0.0f;
        float probeZ = 0.0f;
        int age = 0;
        std::vector<float> verts;
    };
    /// Fill lattice.verts for a box centre +/- r in x, y and +/- zHalf in z.
    void buildFloorLattice(FloorLattice& lattice, const glm::vec3& center, float r, float zHalf,
                           const HeightQuery2D& terrainHeight, const HeightQuery3D& wmoHeight,
                           const HeightQuery3D& m2Height);
    void renderSelectionCircleOnly(const glm::mat4& view, const glm::mat4& projection,
                                   VkCommandBuffer cmd, const HeightQuery2D& terrainHeight,
                                   const HeightQuery3D& wmoHeight, const HeightQuery3D& m2Height);
    void renderSpellTargetDecal(const glm::mat4& view, const glm::mat4& projection,
                                VkCommandBuffer cmd, const HeightQuery2D& terrainHeight,
                                const HeightQuery3D& wmoHeight, const HeightQuery3D& m2Height);
    void initSpellTargetDecal();
    void loadSpellTargetTextures();
    void initSelectionCircle();
    bool initSelectionDescriptors();
    void loadSelectionTexture();
    void initOverlayPipeline();
    void renderUnitBlobs(const glm::mat4& viewProj, VkCommandBuffer cmd,
                         const HeightQuery2D& terrainHeight, const HeightQuery3D& wmoHeight);
    void initBrightnessPipeline();

    VkContext* vkCtx_ = nullptr;

    // Selection circle resources
    VkPipeline selCirclePipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout selCirclePipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout selCircleSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool selCirclePool_ = VK_NULL_HANDLE;
    VkDescriptorSet selCircleSet_ = VK_NULL_HANDLE;
    VkTexture selCircleTexture_;
    bool selCircleTextureTried_ = false;
    // Host-visible, persistently mapped lattice vertices (xyz floats), one
    // slice per frame in flight so a frame never rewrites what the GPU reads.
    ::VkBuffer selCircleVertBuf_ = VK_NULL_HANDLE;
    VmaAllocation selCircleVertAlloc_ = VK_NULL_HANDLE;
    void* selCircleVertMapped_ = nullptr;
    ::VkBuffer selCircleIdxBuf_ = VK_NULL_HANDLE;
    VmaAllocation selCircleIdxAlloc_ = VK_NULL_HANDLE;
    int selCircleVertCount_ = 0;  // index count
    glm::vec3 selCirclePos_{0.0f};
    glm::vec3 selCircleColor_{1.0f, 0.0f, 0.0f};
    float selCircleRadius_ = 1.2f;
    float selCircleAlpha_ = 1.0f;
    float selPulseT_ = -1.0f;
    bool selCircleVisible_ = false;

    FloorLattice selLattice_;

    // Spell targeting circle resources. Shares the selection circle's shaders,
    // set layout, pipeline layout and lattice indices; blended by alpha
    // (0x4F8A40 sets blend mode 2), with its own textures and vertices.
    VkPipeline spellDecalPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool spellDecalPool_ = VK_NULL_HANDLE;
    VkDescriptorSet spellDecalSet_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};  // acceptable, unacceptable
    VkTexture spellDecalTexture_[2];
    bool spellDecalTexturesTried_ = false;
    ::VkBuffer spellDecalVertBuf_ = VK_NULL_HANDLE;
    VmaAllocation spellDecalVertAlloc_ = VK_NULL_HANDLE;
    void* spellDecalVertMapped_ = nullptr;
    glm::vec3 spellDecalCenter_{0.0f};
    float spellDecalHalfWidth_ = 1.0f;
    bool spellDecalAcceptable_ = true;
    bool spellDecalVisible_ = false;
    FloorLattice spellDecalLattice_;

    // Unit blob shadows. Made on first use, so the shadow-map path never
    // allocates them.
    std::vector<UnitBlob> unitBlobs_;
    VkPipeline blobPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout blobPipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool blobPool_ = VK_NULL_HANDLE;
    VkDescriptorSet blobSet_ = VK_NULL_HANDLE;
    VkTexture blobTexture_;
    bool blobTextureTried_ = false;
    ::VkBuffer blobVertBuf_ = VK_NULL_HANDLE;
    VmaAllocation blobVertAlloc_ = VK_NULL_HANDLE;
    void* blobVertMapped_ = nullptr;
    /// Floor heights per lattice point (terrain, WMO), shared by every blob.
    std::unordered_map<uint64_t, FloorSample> blobFloorCache_;
    int blobFloorAge_ = 0;
    std::vector<float> blobVerts_;
    static constexpr uint32_t kBlobMaxVerts = 49152;  // per frame slice

    // Fullscreen overlay resources
    VkPipeline overlayPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout overlayPipelineLayout_ = VK_NULL_HANDLE;
    // Multiplicative brightness pipeline (shares overlayPipelineLayout_).
    VkPipeline brightnessPipeline_ = VK_NULL_HANDLE;
};

} // namespace rendering
} // namespace wowee
