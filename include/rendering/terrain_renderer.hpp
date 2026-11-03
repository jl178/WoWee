#pragma once

#include <cmath>

#include "rendering/vk_shader.hpp"
#include "rendering/shadow_params.hpp"
#include "rendering/shadow_draw_list.hpp"
#include "rendering/stock_terrain.hpp"
#include "rendering/terrain_horizon.hpp"

#include "pipeline/terrain_mesh.hpp"
#include "pipeline/blp_loader.hpp"
#include "rendering/camera.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <algorithm>

namespace wowee {

// Forward declarations
namespace pipeline { class AssetManager; }

namespace rendering {

class VkContext;
class VkTexture;
class Frustum;
class RtScene;

/**
 * GPU-side terrain chunk data (Vulkan)
 */
struct TerrainChunkGPU {
    ::VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc = VK_NULL_HANDLE;
    ::VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAlloc = VK_NULL_HANDLE;
    uint32_t indexCount = 0;

    // Material descriptor set (set 1: 7 samplers + params UBO)
    VkDescriptorSet materialSet = VK_NULL_HANDLE;

    // Per-chunk params UBO (hasLayer1/2/3)
    ::VkBuffer paramsUBO = VK_NULL_HANDLE;
    VmaAllocation paramsAlloc = VK_NULL_HANDLE;

    // Texture handles (owned by cache, NOT destroyed per-chunk)
    VkTexture* baseTexture = nullptr;
    VkTexture* layerTextures[3] = {nullptr, nullptr, nullptr};
    VkTexture* alphaTextures[3] = {nullptr, nullptr, nullptr};
    // Baked shadow (MCSH) as light, binding 8; null = the opaque fallback, lit.
    VkTexture* shadowTexture = nullptr;
    int layerCount = 0;
    /// Per layer, the MCLY flags and bit 16 for a `_s` texture; and the
    /// map's big-alpha flag. What TerrainParams hands the shader.
    int32_t layerFlags[4] = {0, 0, 0, 0};
    bool bigAlpha = false;

    // World position for culling
    float worldX = 0.0f;
    float worldY = 0.0f;
    float worldZ = 0.0f;

    // Owning tile coordinates (for per-tile removal)
    int tileX = -1, tileY = -1;

    // Bounding sphere for frustum culling
    float boundingSphereRadius = 0.0f;
    glm::vec3 boundingSphereCenter = glm::vec3(0.0f);

    // Which chunk of the tile's TerrainMesh this is (cy * 16 + cx), so it can
    // be written into a mega slot again later; -1 when not known.
    int16_t meshChunk = -1;

    // Offsets into mega buffers for indirect drawing (-1 = not in mega buffer)
    int32_t megaBaseVertex = -1;
    uint32_t megaFirstIndex = 0;
    uint32_t vertexCount = 0;

    // The chunk as the ray traced lighting sees it (RtScene ids, or ~0u).
    uint32_t rtMesh = ~0u;
    uint32_t rtInstance = ~0u;

    [[nodiscard]] bool isValid() const { return vertexBuffer != VK_NULL_HANDLE && indexBuffer != VK_NULL_HANDLE; }
};

/**
 * Terrain renderer (Vulkan)
 */
class TerrainRenderer {
public:
    TerrainRenderer();
    ~TerrainRenderer();

    /**
     * Initialize terrain renderer
     * @param ctx Vulkan context
     * @param perFrameLayout Descriptor set layout for set 0 (per-frame UBO)
     * @param assetManager Asset manager for loading textures
     */
    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                    pipeline::AssetManager* assetManager);

    void shutdown();

    bool loadTerrain(const pipeline::TerrainMesh& mesh,
                     const std::vector<std::string>& texturePaths,
                     int tileX = -1, int tileY = -1);


    /// Upload a batch of terrain chunks incrementally. Returns true when all chunks done.
    /// chunkIndex is updated to the next chunk to process (0-255 row-major).
    bool loadTerrainIncremental(const pipeline::TerrainMesh& mesh,
                                const std::vector<std::string>& texturePaths,
                                int tileX, int tileY,
                                int& chunkIndex, int maxChunksPerCall = 16);

    void removeTile(int tileX, int tileY);

    /// The CPU mesh tile (x, y) was uploaded from, or null when it is not
    /// at hand.
    using TileMeshLookup = std::function<const pipeline::TerrainMesh*(int tileX, int tileY)>;

    /// Keep the tiles nearest `cameraPos` in the mega buffers' slots.
    ///
    /// A tile takes a slot when it loads, if one is free; on a long trip the
    /// tiles that end up next to the player can be ones that loaded while
    /// every slot was held, and those draw each chunk from its own buffers
    /// (a bind pair and a draw each, in both passes). Each call makes at most
    /// one move: writes the nearest such tile within the view and shadow
    /// reach into a free slot from its CPU mesh, or, with none free, sends
    /// the farthest slotted tile beyond that reach back to its own buffers
    /// (which every chunk keeps) so its slot comes free after the frames in
    /// flight. Draws are the same either way. Main thread, where tiles are
    /// loaded and removed. WOWEE_TERRAIN_SLOT_MIGRATE=0 moves nothing; the
    /// summary line is logged either way.
    void updateMegaSlotPlacement(const glm::vec3& cameraPos, const TileMeshLookup& meshOf);

    void uploadPreloadedTextures(const std::unordered_map<std::string, pipeline::BLPImage>& textures);

    /**
     * Render terrain
     * @param cmd Command buffer to record into
     * @param perFrameSet Per-frame descriptor set (set 0)
     * @param camera Camera for frustum culling
     */
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera);

    /// render() in steps, so the pass can be recorded into two command
    /// buffers on two threads (Renderer::renderWorld). The cull first; false
    /// means there is nothing to record at all.
    bool cullForRecording(const Camera& camera);
    /// 1, or 2 when the surviving chunks are worth cutting in two.
    /// WOWEE_TERRAIN_ONE_WORKER=1 always answers 1.
    [[nodiscard]] uint32_t recordPartsWanted() const;
    /// Record one part of the culled chunks, in order: part 0's buffer is
    /// executed first. The parts of one frame may be recorded concurrently.
    void recordPart(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, uint32_t part, uint32_t parts);

    /**
     * Initialize terrain shadow pipeline (must be called after initialize()).
     * @param shadowRenderPass  Depth-only render pass used for the shadow map.
     */
    bool initializeShadow(VkRenderPass shadowRenderPass);

    /**
     * Render terrain into the shadow depth map.
     * @param cmd               Command buffer (inside shadow render pass).
     * @param lightSpaceMatrix  Orthographic light-space transform.
     * @param shadowCenter      World-space centre of shadow coverage.
     * @param shadowRadius      Cull radius around shadowCenter.
     */
    void renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                      const glm::vec3& shadowCenter, float shadowRadius);

    [[nodiscard]] bool hasShadowPipeline() const { return shadowPipeline_ != VK_NULL_HANDLE; }

    void clear();

    void recreatePipelines();
    /// The fill pipeline and its wireframe derivative, which
    /// initialize() and recreatePipelines() both need.
    bool buildMainPassPipelines(VkDevice device,
                                wowee::rendering::VkShaderModule& vertShader,
                                wowee::rendering::VkShaderModule& fragShader);

    void setWireframe(bool enabled) { wireframe = enabled; }
    void setFrustumCulling(bool enabled) { frustumCullingEnabled = enabled; }
    void setFogEnabled(bool enabled) { fogEnabled = enabled; }
    [[nodiscard]] bool isFogEnabled() const { return fogEnabled; }
    /// Terrain Highlights (stock CVar `specular`): the sun's glint on `_s`
    /// tilesets. Off, the push constant's power is 0 and terrain.vert skips it.
    void setSpecularEnabled(bool on) { specularEnabled_ = on; }
    void setViewDistance(float distance) { maxViewDistance_ = std::clamp(distance, 400.0f, 2400.0f); }

    /// The highlight's colour, the sun's (DayNight channel 9, the stock c27),
    /// set each frame; drawn only where a `_s` tileset was loaded.
    void setSunSpecular(const glm::vec3& colour) { sunSpecular_ = colour; }
    /// The map's WDL, for the distant mountains (TerrainHorizon). Main thread.
    void setHorizonTiles(const std::vector<stock_terrain::WdlTile>& tiles);

    void setShadowMap(VkDescriptorImageInfo /*depthInfo*/, const glm::mat4& /*lightSpaceMat*/) {}
    void clearShadowMap() {}

    [[nodiscard]] int getChunkCount() const { return static_cast<int>(chunks.size()); }
    [[nodiscard]] int getRenderedChunkCount() const { return renderedChunks; }
    /// How far the furthest chunk drawn this frame was, for the diagnostic that
    /// compares it against the doodads. See Renderer::logViewDistanceDiag.
    [[nodiscard]] float getFurthestDrawnDistance() const { return std::sqrt(furthestDrawnSq_); }
    [[nodiscard]] int getCulledChunkCount() const { return culledChunks; }
    [[nodiscard]] int getTriangleCount() const;
    [[nodiscard]] VkContext* getVkContext() const { return vkCtx; }

    /// Where chunks register their geometry for the ray traced lighting.
    void setRtScene(RtScene* scene) { rtScene_ = scene; }

private:
    TerrainChunkGPU uploadChunk(const pipeline::ChunkMesh& chunk, int tileX, int tileY);
    VkTexture* loadTexture(const std::string& path);
    VkTexture* createAlphaTexture(const std::vector<uint8_t>& alphaData);
    bool isChunkVisible(const TerrainChunkGPU& chunk, const Frustum& frustum);
    void calculateBoundingSphere(TerrainChunkGPU& chunk, const pipeline::ChunkMesh& meshChunk);
    VkDescriptorSet allocateMaterialSet();
    /// False when there is nothing sampleable to write, in which case the
    /// caller must drop the chunk: a descriptor set that was allocated and
    /// never written is as undefined to bind as one holding a null view.
    bool writeMaterialDescriptors(VkDescriptorSet set, const TerrainChunkGPU& chunk);
    void destroyChunkGPU(TerrainChunkGPU& chunk);

    /// Point a chunk's base, layer and alpha textures at the loaded ones.
    ///
    /// Both load paths do this identically; chunkX and chunkY only name the
    /// chunk in the warnings. A layer whose textureId is past the end of the
    /// tile's texture list falls back to white rather than going unbound.
    void bindChunkTextures(TerrainChunkGPU& gpuChunk,
                           const pipeline::ChunkMesh& chunk,
                           const pipeline::TerrainMesh& mesh,
                           const std::vector<std::string>& texturePaths,
                           int tileX, int tileY, int chunkX, int chunkY);

    /// Allocate and fill a chunk's params UBO. False means the allocation
    /// failed, and the two callers answer that differently: the one-shot load
    /// skips the chunk, the incremental one hands it back to be retried.
    bool createChunkParamsUBO(TerrainChunkGPU& gpuChunk);

    VkContext* vkCtx = nullptr;
    RtScene* rtScene_ = nullptr;
    glm::vec3 sunSpecular_{0.0f};
    /// WOWEE_TERRAIN_AB_PERIOD's off phase: no highlight, no horizon.
    bool stockExtrasOff_ = false;
    bool specularEnabled_ = false;  // the `specular` option; stock default off
    std::unique_ptr<TerrainHorizon> horizon_;
    void registerRtChunk(TerrainChunkGPU& gpuChunk, const pipeline::ChunkMesh& chunk);
    pipeline::AssetManager* assetManager = nullptr;

    // Main pipelines
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipeline wireframePipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;

    // Shadow pipeline
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadowPipelineLayout_ = VK_NULL_HANDLE;
    /// The set the shadow pass binds. Five separate members before,
    /// built and torn down here and in three other renderers.
    ShadowParamsSet shadowParams_;

    // Descriptor pool for material sets. One set per terrain chunk, 256 chunks
    // to a tile, so this is a tile budget: 65536 covered 256 tiles, and tiles
    // are held to the *unload* radius, not the load radius - 8 loading and 11
    // unloading is 23×23 = 529 resident. Every tile past the 256th arrived
    // with no descriptor set and, before the retry in loadTerrainIncremental,
    // was dropped for good: terrain that simply was not there, in whichever
    // direction the player had travelled furthest.
    VkDescriptorPool materialDescPool = VK_NULL_HANDLE;
    static constexpr uint32_t MAX_MATERIAL_SETS = 160 * 1024;  // 640 tiles

    // Loaded terrain chunks
    std::vector<TerrainChunkGPU> chunks;
    /// Bumped whenever `chunks` gains or loses an entry.
    uint64_t chunksGeneration_ = 0;
    /// Bumped whenever `chunks` loses entries (removeTile, clear). The chunks
    /// after a removed one shift down, so whatever holds chunk indices starts
    /// again; an append leaves every index where it was.
    uint64_t chunksRemovedGeneration_ = 0;
    /// The chunks grouped by tile with a sphere round each tile, for the
    /// shadow pass: a tile whose sphere misses the shadow map skips its 256
    /// chunk tests.
    TileChunkGrouping shadowTiles_;
    /// The same grouping for the main pass's cull, kept apart because that
    /// runs on the terrain worker.
    TileChunkGrouping viewTiles_;
    /// Bring `grouping` up to date with `chunks`: appended chunks join their
    /// tiles, and a removal since the last call starts it over. The slots
    /// that changed go to `touched` (when given). True when it started over,
    /// which changes every slot.
    bool updateTileGrouping(TileChunkGrouping& grouping, std::vector<uint32_t>* touched) const;
    /// The chunks cullForRecording kept, in `chunks` order.
    std::vector<uint32_t> visibleChunks_;

    /// The shadow pass's draws, kept between frames (renderShadow).
    ///
    /// Per tile slot of shadowTiles_, the chunks that passed the cull; and
    /// from those, the draws, with the runs of chunks that sit end to end in
    /// the mega buffers merged into one. Culled again whole when the light
    /// matrix, the centre, the radius or a removal changes, and only for the
    /// tiles that gained chunks when chunks were appended.
    struct TerrainShadowDraw {
        ::VkBuffer vertexBuffer = VK_NULL_HANDLE;  // null: the mega buffers
        ::VkBuffer indexBuffer = VK_NULL_HANDLE;
        IndexRange range;
    };
    std::vector<std::vector<uint32_t>> shadowTileKept_;
    std::vector<TerrainShadowDraw> shadowDraws_;
    uint32_t shadowDrawChunks_ = 0;
    bool shadowListValid_ = false;
    glm::mat4 shadowListLightSpace_{0.0f};
    glm::vec3 shadowListCenter_{0.0f};
    float shadowListRadius_ = 0.0f;
    std::vector<uint32_t> shadowTouched_;
    /// Frames the list was kept as it was, rebuilt from a full cull, and
    /// patched for appended chunks; reported with the cast count.
    uint32_t shadowListKept_ = 0, shadowListFull_ = 0, shadowListPatched_ = 0;
    /// Cull tile `slot` of shadowTiles_ into shadowTileKept_[slot].
    void cullShadowTile(uint32_t slot, const glm::mat4& lightSpaceMatrix,
                        const glm::vec3& shadowCenter, float shadowRadius);
    /// Rebuild shadowDraws_ from shadowTileKept_.
    void buildShadowDraws();

    /// The vertexOffset a mega chunk is drawn with: 0 when the mega index
    /// buffer holds indices already offset to the chunk's vertices (see
    /// uploadChunk), its base vertex otherwise.
    [[nodiscard]] int32_t megaVertexOffset(const TerrainChunkGPU& chunk) const;

    // Texture cache (path -> VkTexture)
    struct TextureCacheEntry {
        std::unique_ptr<VkTexture> texture;
        size_t approxBytes = 0;
        uint64_t lastUse = 0;
    };
    std::unordered_map<std::string, TextureCacheEntry> textureCache;
    size_t textureCacheBytes_ = 0;
    uint64_t textureCacheCounter_ = 0;
    size_t textureCacheBudgetBytes_ = 4096ull * 1024 * 1024;
    std::unordered_set<std::string> failedTextureCache_;
    std::unordered_set<std::string> loggedTextureLoadFails_;
    /// Cache entries handed out at or after this counter are never evicted.
    ///
    /// A chunk under construction is not in `chunks` yet, so the scan for what
    /// is in use cannot see the textures it has already been given. Raised to
    /// the counter at the start of every upload call, which puts everything
    /// this tile has taken out of reach until the chunk holding it lands.
    uint64_t evictProtectFrom_ = 0;

    /// Free least-recently-used cached textures until `needBytes` fits.
    ///
    /// Returns whether there is now room. Only textures no live chunk points
    /// at are freed, and they go through the frame fences rather than being
    /// destroyed on the spot: a chunk's material descriptor set names the
    /// image, so one still being drawn from must outlive the frame that
    /// referenced it.
    bool evictTexturesFor(size_t needBytes);

    // Fallback textures
    std::unique_ptr<VkTexture> whiteTexture;
    std::unique_ptr<VkTexture> opaqueAlphaTexture;

    // Rendering state
    bool wireframe = false;
    bool frustumCullingEnabled = true;
    bool fogEnabled = true;
    float maxViewDistance_ = 1200.0f;
    int renderedChunks = 0;
    float furthestDrawnSq_ = 0.0f;
    int culledChunks = 0;

    // Mega vertex/index buffers for indirect drawing
    // All terrain chunks share a single VB + IB, eliminating per-chunk rebinds.
    // Indirect draw commands are built CPU-side each frame for visible chunks.
    VkBuffer megaVB_ = VK_NULL_HANDLE;
    VmaAllocation megaVBAlloc_ = VK_NULL_HANDLE;
    void* megaVBMapped_ = nullptr;
    VkBuffer megaIB_ = VK_NULL_HANDLE;
    VmaAllocation megaIBAlloc_ = VK_NULL_HANDLE;
    void* megaIBMapped_ = nullptr;
    uint32_t megaVBUsed_ = 0;  // vertices used
    uint32_t megaIBUsed_ = 0;  // indices used
    static constexpr uint32_t MEGA_VB_MAX_VERTS   = 1536 * 1024; // ~1.5M verts × 56B ≈ 84MB
    // A tile's worth of the mega buffers: 256 chunks of at most 145 vertices
    // and 768 indices. Each tile takes one slot while it is loaded and gives
    // it back when unloaded (WOWEE_TERRAIN_MEGA_SLOTS=0: one bump allocation
    // that is never given back, so every chunk loaded after the first ~8k of
    // the session fell back to its own buffers).
    static constexpr uint32_t kMegaTileSlotVerts = 256 * 145;
    static constexpr uint32_t kMegaTileSlotIndices = 256 * 768;
    // As many tiles of indices as the vertex buffer holds tiles of vertices
    // (42; 6M indices held 32). 8.26M indices × 4B ≈ 31.5MB.
    static constexpr uint32_t MEGA_IB_MAX_INDICES =
        (MEGA_VB_MAX_VERTS / kMegaTileSlotVerts) * kMegaTileSlotIndices;
    // The sizes the buffers were made with (initialize). With slot placement
    // on (WOWEE_TERRAIN_SLOT_MIGRATE, the default) a whole number of slots,
    // 128 unless WOWEE_TERRAIN_MEGA_TILE_SLOTS says otherwise: 2.73 MiB a
    // slot (1.98 vertices, 0.75 indices), 350 MiB in all. Off, the sizes
    // above: 42 slots, 115 MiB.
    uint32_t megaVBMaxVerts_ = MEGA_VB_MAX_VERTS;
    uint32_t megaIBMaxIndices_ = MEGA_IB_MAX_INDICES;
    struct MegaTileSlot {
        uint32_t slot = 0;
        uint32_t vertsUsed = 0;
        uint32_t indicesUsed = 0;
    };
    std::unordered_map<uint64_t, MegaTileSlot> megaTileSlots_;
    /// Shared with the deferred releases, which can run after this renderer
    /// has cleared its own state.
    struct MegaSlotPool {
        std::mutex mutex;
        LowestFreeSlots slots;
        /// Slots released and not yet back (their frames still in flight).
        uint32_t releasing = 0;
    };
    std::shared_ptr<MegaSlotPool> megaSlotPool_;
    /// Give tile `key`'s mega slot back once no frame in flight can read it.
    void releaseMegaTileSlot(uint64_t key);
    /// Copy one chunk's vertices and indices to their place in the mapped
    /// mega buffers, the indices offset as megaVertexOffset expects.
    void writeMegaChunk(const pipeline::ChunkMesh& chunk, uint32_t megaVertex, uint32_t megaIndex);

    /// Slot placement (updateMegaSlotPlacement). The chunks grouped by tile
    /// once more, kept on the main thread with the other two; per tile slot,
    /// how many of its chunks draw from their own buffers.
    TileChunkGrouping slotTiles_;
    std::vector<uint32_t> slotTilesFallback_;
    std::vector<uint32_t> slotTouched_;
    /// Tiles whose CPU mesh did not match what was uploaded; never offered
    /// a slot again while loaded.
    std::unordered_set<uint64_t> slotRefused_;
    /// Free slots a loading tile may not take: 1 while a near tile waits
    /// for one, so a slot an eviction freed reaches it (uploadChunk).
    uint32_t megaSlotsReserved_ = 0;
    /// Set when chunks moved in or out of the mega buffers, so the shadow
    /// pass rebuilds its kept draws (renderShadow).
    bool shadowDrawsStale_ = false;
    /// The shadow pass's last cull radius, for the reach slots should cover.
    float slotShadowRadius_ = 0.0f;
    /// Tiles moved in and out so far, and what the last move cost.
    uint32_t slotMovesIn_ = 0, slotMovesOut_ = 0;
    float slotLastMoveMs_ = 0.0f;
    /// What the last summary line said, so it is only said again on change.
    uint32_t slotLoggedNearTiles_ = ~0u, slotLoggedNearFallback_ = ~0u;
    double slotLoggedAt_ = -1e9;
    /// Write tile slot `groupSlot` of slotTiles_ into a fresh mega slot from
    /// `mesh`, giving back the one it held. False when it was not done.
    bool promoteTileToMegaSlot(uint32_t groupSlot, const pipeline::TerrainMesh& mesh);
    /// Send tile slot `groupSlot`'s chunks to their own buffers and give its
    /// mega slot back.
    void evictTileFromMegaSlot(uint32_t groupSlot);
    [[nodiscard]] uint32_t countFallbackChunks(const TileChunkGroup& tile) const;

    VkBuffer indirectBuffer_ = VK_NULL_HANDLE;
    VmaAllocation indirectAlloc_ = VK_NULL_HANDLE;
    static constexpr uint32_t MAX_INDIRECT_DRAWS = 8192;
};

} // namespace rendering
} // namespace wowee
