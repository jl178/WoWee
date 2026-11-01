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
    void setSelectionCircle(const glm::vec3& pos, float radius, const glm::vec3& color);
    void clearSelectionCircle();
    void renderSelectionCircle(const glm::mat4& view, const glm::mat4& projection,
                               VkCommandBuffer cmd,
                               const HeightQuery2D& terrainHeight,
                               const HeightQuery3D& wmoHeight,
                               const HeightQuery3D& m2Height);

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
    void initSelectionCircle();
    bool initSelectionDescriptors();
    void loadSelectionTexture();
    void initOverlayPipeline();
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
    bool selCircleVisible_ = false;

    // Floor lattice cache. The decal is draped over a world-aligned lattice so
    // a lattice point keeps its world position while the target walks, and only
    // the new edge of the patch has to be raycast. Each point keeps every
    // floor the three queries found, and the one nearest the feet is picked
    // per frame. The cache drops when the spacing changes, when the feet move
    // far enough vertically for the WMO/M2 probes to answer differently, and
    // periodically in case geometry streams in.
    struct SelFloorSample {
        std::optional<float> terrain;
        std::optional<float> wmo;
        std::optional<float> m2;
    };
    std::unordered_map<uint64_t, SelFloorSample> selFloorCache_;
    float selFloorCacheSpacing_ = 0.0f;
    float selFloorCacheProbeZ_ = 0.0f;
    int selFloorCacheAge_ = 0;
    std::vector<float> selLatticeVerts_;

    // Fullscreen overlay resources
    VkPipeline overlayPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout overlayPipelineLayout_ = VK_NULL_HANDLE;
    // Multiplicative brightness pipeline (shares overlayPipelineLayout_).
    VkPipeline brightnessPipeline_ = VK_NULL_HANDLE;
};

} // namespace rendering
} // namespace wowee
