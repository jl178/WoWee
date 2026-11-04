#pragma once

#include "rendering/horizon_detail.hpp"
#include "rendering/stock_terrain.hpp"
#include "rendering/vk_utils.hpp"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace wowee {
namespace rendering {

class VkContext;
class Camera;

/// The stock horizon: the map's WDL heights, drawn past the far clip in the
/// flat fog colour so the distant mountains stand against the sky.
///
/// 0x795f80 draws them after the terrain, between the far clip less 50 yards
/// and four far clips (horizonFarclipScale), with fog start 0 and end 1 - every
/// pixel the fog colour - at depth 0.998-0.999, so only the sky is behind
/// them. Here they are drawn at their true depth, and the near limit is a
/// discard on the view depth; the result is the same silhouette wherever no
/// nearer ground was drawn. docs/re/terrain.md section 8.
///
/// The WDL's low-detail WMOs (setFarMesh) are drawn in the same pass by the
/// same rule. With setLit (E11, horizon_detail.hpp) both are shaded by the sun
/// under the fog colour, through pipelines and a normal buffer made on first
/// use; off, those are never made and the pass is the stock one.
///
/// Owned by TerrainRenderer and recorded on its worker, after the chunks.
/// setTiles, setFarMesh and setLit are main-thread only; cull and record only
/// read.
class TerrainHorizon {
public:
    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    void shutdown();
    /// After an MSAA change, as TerrainRenderer::recreatePipelines.
    void recreatePipeline();

    /// The map's tiles, replacing the last map's; empty clears the horizon.
    void setTiles(const std::vector<stock_terrain::WdlTile>& tiles);
    /// The WDL's low-detail buildings, replacing the last map's.
    void setFarMesh(horizon_detail::FarMesh mesh);
    /// E11 on or off: on makes the lit pipelines and the normals (main
    /// thread, at once); off releases them once no frame can use them.
    void setLit(bool lit);

    /// The tiles in reach of a far clip and in the view.
    void cull(const Camera& camera, float farClip);
    void record(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) const;

private:
    bool buildPipeline();
    VkPipeline buildOne(const char* vertPath, const char* fragPath, bool lit);
    void releaseBuffers();
    void releaseFarBuffers();
    /// The lit pipelines and the normals, made if missing. False if they
    /// cannot be. Main thread.
    bool ensureLit();
    void releaseLit();

    struct Tile {
        glm::vec3 center{0.0f};
        float radius = 0.0f;
        int32_t vertexOffset = 0;
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        uint32_t coarseFirst = 0;   // wdlTileIndicesCoarse, for far tiles
        uint32_t coarseCount = 0;
    };

    VkContext* vkCtx_ = nullptr;
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;     // with the near-limit discard
    VkPipeline farPipeline_ = VK_NULL_HANDLE;  // wholly past it: no discard
    AllocatedBuffer vertices_{};
    AllocatedBuffer indices_{};
    std::vector<Tile> tiles_;
    std::vector<uint32_t> visible_;     // reaching nearer than the limit
    std::vector<uint32_t> visibleFar_;  // wholly past it
    std::vector<uint8_t> coarse_;       // per tile: draw the coarse level
    glm::vec2 depthRange_{0.0f};
    float farClip_ = 0.0f;

    // E11: the shaded horizon. The tiles' heights are kept so their normals
    // can be made when it is first turned on.
    bool lit_ = false;
    bool litFailed_ = false;
    bool drawLit_ = false;                     // this frame's cull chose it
    std::vector<stock_terrain::WdlTile> source_;
    VkPipeline litPipeline_ = VK_NULL_HANDLE;
    VkPipeline litFarPipeline_ = VK_NULL_HANDLE;
    AllocatedBuffer normals_{};

    // The WDL's low-detail WMOs: positions and indices always (stock draws
    // them), normals uploaded with the lit pipelines.
    horizon_detail::FarMesh far_;
    AllocatedBuffer farPositions_{};
    AllocatedBuffer farNormals_{};
    AllocatedBuffer farIndices_{};
    std::vector<uint32_t> farVisible_;
};

}  // namespace rendering
}  // namespace wowee
