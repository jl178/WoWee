#pragma once

#include "core/coordinates.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/terrain_mesh.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/blp_loader.hpp"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <optional>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <queue>
#include <list>
#include <vector>
#include <condition_variable>
#include <deque>
#include <glm/glm.hpp>
#include <array>

namespace wowee {

namespace pipeline { class AssetManager; }
namespace audio { class AmbientSoundManager; class WorldSoundEmitters; }
namespace rendering { class TerrainRenderer; class Camera; class WaterRenderer; class M2Renderer; class WMORenderer; }

namespace rendering {

/**
 * Terrain tile coordinates
 */
struct TileCoord {
    int x;
    int y;

    bool operator==(const TileCoord& other) const {
        return x == other.x && y == other.y;
    }

    struct Hash {
        size_t operator()(const TileCoord& coord) const {
            return std::hash<int>()(coord.x) ^ (std::hash<int>()(coord.y) << 1);
        }
    };
};

/**
 * Loaded terrain tile data
 */
/// One doodad to place: a model the tile loaded, and where.
struct M2Placement {
    uint32_t modelId;
    uint32_t uniqueId;
    glm::vec3 position;
    glm::vec3 rotation;
    float scale;
};

/// One piece of ground clutter. Smaller than an M2Placement - a grassy tile
/// holds tens of thousands - and carrying the terrain's normal under the
/// piece, so it can be lit as the ground it stands on is.
struct ClutterPlacement {
    uint32_t modelId;
    glm::vec3 position;
    float yaw;
    float scale;
    glm::vec3 groundNormal;
};

struct TerrainTile {
    TileCoord coord;
    pipeline::ADTTerrain terrain;
    pipeline::TerrainMesh mesh;
    bool loaded = false;

    // Tile bounds in world coordinates
    float minX, minY, maxX, maxY;

    // Instance IDs for cleanup on unload
    std::vector<uint32_t> wmoInstanceIds;
    std::vector<uint32_t> wmoUniqueIds;  // For WMO dedup cleanup on unload
    std::vector<uint32_t> m2InstanceIds;
    std::vector<uint32_t> doodadUniqueIds;  // For dedup cleanup on unload
    // WorldSoundEmitters ids of the doodads' own sounds ($DSL / $DSO events).
    std::vector<uint32_t> soundEmitterIds;
    // Ground clutter is kept as placements and only becomes renderer
    // instances while its map chunk is within clutter range of the camera.
    // See TerrainManager::streamGroundClutter.
    //
    // Per chunk rather than per tile: a tile is 533 yards across and the
    // clutter is drawn to 140 at most, so instancing whole tiles made several
    // times more pieces than could ever be seen - most of a tile's, for each
    // of up to four tiles round a corner.
    std::vector<ClutterPlacement> clutterPlacements;  ///< in chunk order (cy * 16 + cx)
    /// Chunk c's placements are [clutterChunkStart[c], clutterChunkStart[c + 1]).
    std::array<uint32_t, 257> clutterChunkStart{};
    /// Chunk c's placements' xy extent: min x, min y, max x, max y.
    std::array<glm::vec4, 256> clutterChunkBounds{};
    /// The instances made from chunk c, and the next of its placements to make.
    std::array<std::vector<uint32_t>, 256> clutterChunkIds;
    std::array<uint32_t, 256> clutterChunkNext{};
    /// Set while a chunk is out of range and waiting for a batched removal.
    std::array<uint8_t, 256> clutterChunkRetired{};
    /// Instances alive across all chunks, so a tile with none is skipped whole.
    size_t clutterLive = 0;
    /// Hand every clutter instance id to `out` and forget them, leaving the
    /// placements to be instanced again.
    void takeClutterInstances(std::vector<uint32_t>& out) {
        for (size_t c = 0; c < clutterChunkIds.size(); ++c) {
            out.insert(out.end(), clutterChunkIds[c].begin(), clutterChunkIds[c].end());
            clutterChunkIds[c].clear();
            clutterChunkNext[c] = 0;
            clutterChunkRetired[c] = 0;
        }
        clutterLive = 0;
    }
    /// Which ground clutter density the placements were made at. A tile whose
    /// number is behind TerrainManager's is placed again, so the setting
    /// reaches the ground already loaded rather than only the next tile.
    uint32_t clutterGeneration = 0;
};

/**
 * Pre-processed tile data ready for GPU upload (produced by background thread)
 */
struct PendingTile {
    TileCoord coord;
    pipeline::ADTTerrain terrain;
    pipeline::TerrainMesh mesh;

    // Pre-loaded M2 data
    struct M2Ready {
        uint32_t modelId;
        pipeline::M2Model model;
        std::string path;
    };
    std::vector<M2Ready> m2Models;

    // M2 instance placement data (references modelId from m2Models)
    using M2Placement = rendering::M2Placement;
    std::vector<M2Placement> m2Placements;
    // Ground clutter placements, instantiated by distance rather than at load.
    std::vector<ClutterPlacement> clutterPlacements;
    /// Per-chunk ranges and extents of clutterPlacements; see TerrainTile.
    std::array<uint32_t, 257> clutterChunkStart{};
    std::array<glm::vec4, 256> clutterChunkBounds{};
    /// TerrainManager's clutter density generation when these were placed.
    uint32_t clutterGeneration = 0;

    // Pre-loaded WMO data
    struct WMOReady {
        uint32_t modelId;
        uint32_t uniqueId;
        pipeline::WMOModel model;
        glm::vec3 position;
        glm::vec3 rotation;
        float scale = 1.0f;
        uint16_t nameSet = 0;  ///< MODF nameSet, part of the WMOAreaTable key
    };
    std::vector<WMOReady> wmoModels;

    // WMO doodad M2 models (M2s placed inside WMOs)
    struct WMODoodadReady {
        uint32_t modelId;
        pipeline::M2Model model;
        glm::vec3 worldPosition;   // For frustum culling
        glm::mat4 modelMatrix;     // Pre-computed world transform
    };
    std::vector<WMODoodadReady> wmoDoodads;

    // Ambient sound emitters (detected from doodads)
    struct AmbientEmitter {
        glm::vec3 position;
        uint32_t type;  // Maps to AmbientSoundManager::AmbientType
    };
    std::vector<AmbientEmitter> ambientEmitters;

    // Pre-loaded terrain texture BLP data (loaded on background thread to avoid
    // blocking file I/O on the main thread during finalizeTile)
    std::unordered_map<std::string, pipeline::BLPImage> preloadedTextures;

    // Pre-decoded M2 model textures (decoded on background thread)
    std::unordered_map<std::string, pipeline::BLPImage> preloadedM2Textures;

    // Pre-decoded WMO textures (decoded on background thread)
    std::unordered_map<std::string, pipeline::BLPImage> preloadedWMOTextures;
    // CPU-generated normal/height pixels for those WMO textures. Keeping this
    // work on terrain workers avoids multi-second main-thread backfill stalls.
    std::unordered_map<std::string, pipeline::BLPImage> preloadedWMONormalMaps;
    std::unordered_map<std::string, float> preloadedWMONormalMapVariances;
};

/**
 * Phases for incremental tile finalization (one bounded unit of work per call)
 */
enum class FinalizationPhase {
    TERRAIN,        // Upload terrain mesh + textures + water
    M2_MODELS,      // Upload ONE M2 model per call
    M2_INSTANCES,   // Create all M2 instances (lightweight struct allocation)
    WMO_MODELS,     // Upload ONE WMO model per call
    WMO_INSTANCES,  // Create all WMO instances + load WMO liquids
    WMO_DOODADS,    // Upload ONE WMO doodad M2 per call
    WATER,          // Generate water ambient emitters
    AMBIENT,        // Register ambient emitters + commit tile
    DONE            // Fully finalized
};

/**
 * In-progress tile finalization state - tracks progress across frames
 */
struct FinalizingTile {
    std::shared_ptr<PendingTile> pending;
    FinalizationPhase phase = FinalizationPhase::TERRAIN;

    // Progress indices within current phase
    size_t m2ModelIndex     = 0;   // Next M2 model to upload
    size_t m2InstanceIndex  = 0;   // Next M2 placement to instantiate
    size_t wmoModelIndex    = 0;   // Next WMO model to upload
    size_t wmoInstanceIndex = 0;   // Next WMO placement to instantiate
    size_t wmoDoodadIndex   = 0;   // Next WMO doodad to upload
    size_t wmoLiquidGroupIndex = 0; // Next liquid group within current WMO instance

    // Incremental terrain upload state (splits TERRAIN phase across frames)
    bool terrainPreloaded = false;  // True after preloaded textures uploaded
    int terrainChunkNext = 0;       // Next chunk index to upload (0-255, row-major)
    bool terrainMeshDone = false;   // True when all chunks uploaded

    // Accumulated results (built up across phases)
    std::vector<uint32_t> m2InstanceIds;
    std::vector<uint32_t> wmoInstanceIds;
    std::vector<uint32_t> tileUniqueIds;
    std::vector<uint32_t> tileWmoUniqueIds;
    std::unordered_set<uint32_t> uploadedM2ModelIds;
    std::vector<uint32_t> soundEmitterIds;
};

/**
 * Terrain manager for multi-tile terrain streaming
 *
 * Handles loading and unloading terrain tiles based on camera position
 */
class TerrainManager {
public:
    TerrainManager();
    ~TerrainManager();

    /**
     * Initialize terrain manager
     * @param assetManager Asset manager for loading files
     * @param terrainRenderer Terrain renderer for GPU upload
     */
    bool initialize(pipeline::AssetManager* assetManager, TerrainRenderer* terrainRenderer);

    /**
     * Update terrain streaming based on camera position
     * @param camera Current camera
     * @param deltaTime Time since last update
     */
    void update(const Camera& camera, float deltaTime);

    /// How long the last update spent in each of its per-frame phases, for
    /// the WOWEE_FRAME_PROFILE breakdown. Zero for a phase that did not run.
    struct UpdatePhases { float readyMs = 0.0f, unloadMs = 0.0f, clutterMs = 0.0f; };
    [[nodiscard]] const UpdatePhases& lastUpdatePhases() const { return lastPhases_; }

    /**
     * Set map name
     * @param mapName Map name (e.g., "Azeroth", "Kalimdor")
     */
    void setMapName(const std::string& mapName) { this->mapName = mapName; }
    [[nodiscard]] bool isCustomZone() const { return isCustomZone_; }
    void setCustomZone(bool custom) { isCustomZone_ = custom; }


    /**
     * Enqueue a tile for async loading (returns false if previously failed).
     */
    bool enqueueTile(int x, int y);

    /**
     * Unload a tile
     * @param x Tile X coordinate
     * @param y Tile Y coordinate
     */
    void unloadTile(int x, int y);
    /// Create clutter instances for tiles near the camera, drop them for
    /// tiles that have moved out of range. A frame's worth of work at a time.
    void streamGroundClutter(const glm::vec3& cameraPos);

    void stopWorkers();  // Stop worker threads without restarting (for shutdown)
    void softReset();  // Clear tile data without stopping worker threads (non-blocking)

    /**
     * Precache a set of tiles (for taxi routes, etc.)
     * @param tiles Vector of (x, y) tile coordinates to preload
     */
    void precacheTiles(const std::vector<std::pair<int, int>>& tiles);

    /**
     * Set streaming parameters
     */
    void setLoadRadius(int radius) { loadRadius = radius; }
    void setUnloadRadius(int radius) { unloadRadius = radius; }
    void setStreamingEnabled(bool enabled) { streamingEnabled = enabled; }
    void setUpdateInterval(float seconds) { updateInterval = seconds; }
    void setTaxiStreamingMode(bool enabled) { taxiStreamingMode_ = enabled; }
    /// Also marks every loaded tile's clutter stale when the value changes:
    /// placements are made once per tile, so without that the setting only
    /// reached tiles loaded after it, and Apply looked like it did nothing
    /// until the player travelled. streamGroundClutter re-places them, one
    /// tile a frame, once the value has stopped moving (a slider drag sends
    /// every step).
    void setGroundClutterDensityScale(float scale);
    [[nodiscard]] float getGroundClutterDensityScale() const { return groundClutterDensityScale_; }

    /** Density for a ground-effect id, or 0 if it grows nothing or is unknown.
     *
     * The one question grass asks of this table. Exposed rather than handing
     * out the table, so callers cannot start depending on its shape - and so
     * the grass generator can take a plain callback and stay testable without
     * a TerrainManager at all.
     */
    [[nodiscard]] uint32_t getGroundEffectDensity(uint32_t effectId) const;

    /** The detail doodads a ground effect plants, and their weights.
     *
     * Their model names are what says whether a patch of ground is meadow,
     * scrub or scree, so grass derives its whole appearance from them rather
     * than from a table of zones. Doodad ids that resolved to no model are
     * left out, along with their weights.
     */
    void getGroundEffectDoodads(uint32_t effectId, std::vector<std::string>& outModels,
                                std::vector<uint32_t>& outWeights) const;
    void setWaterRenderer(WaterRenderer* renderer) { waterRenderer = renderer; }
    void setM2Renderer(M2Renderer* renderer) { m2Renderer = renderer; }
    void setWMORenderer(WMORenderer* renderer) { wmoRenderer = renderer; }
    void setAmbientSoundManager(audio::AmbientSoundManager* manager) { ambientSoundManager = manager; }
    /// Where placed doodads' own sound events ($DSL loops, $DSO one-shots) go.
    void setWorldSoundEmitters(audio::WorldSoundEmitters* emitters) { worldSoundEmitters = emitters; }

    /**
     * Get terrain height at GL coordinates
     * @param glX GL X position
     * @param glY GL Y position
     * @return Height (GL Z) if terrain loaded at that position, empty otherwise
     */
    [[nodiscard]] std::optional<float> getHeightAt(float glX, float glY) const;

    /**
     * True when the MCNK chunk containing this position is cut by terrain holes.
     * getHeightAt interpolates straight across a hole and reports a surface that
     * is not there, so anything reasoning about "below the terrain" has to know
     * the heightfield is fiction here - hole-cut chunks are how cave mouths and
     * below-ground entrances are opened up. Answered per chunk rather than per
     * quad: this only ever gates a safety net, and being coarse in the safe
     * direction beats depending on the hole bit's axis order.
     */
    [[nodiscard]] bool chunkHasHoles(float glX, float glY) const;

    /**
     * True when the single terrain quad under this position is cut away.
     * chunkHasHoles answers for a whole 33x33-yard chunk, which is far too
     * coarse to decide whether the player is standing on ground: a chunk
     * containing one cave mouth would report every yard of it as empty. This
     * is the per-quad question, matched to the quads the mesh builder skips.
     */
    [[nodiscard]] bool isHoleAt(float glX, float glY) const;

    /**
     * Whether the tile covering a world position has finished streaming in.
     *
     * getHeightAt answers nothing both for a hole, which the character should
     * fall through, and for a tile that has not arrived yet, which it should
     * not. Physics cannot tell those apart from the height alone, and on a
     * slow device the second one is what happens at every login: the character
     * enters the world before the ground does and falls until the server kills
     * it.
     */
    [[nodiscard]] bool isTileLoadedAt(float glX, float glY) const;

    /**
     * The chunk under a world position, plus the offsets within it that
     * getHeightAt samples and isHoleAt reads a quad from. Shared so those two
     * cannot drift: the first copy of this search that was written by hand
     * omitted the full-scan fallback and answered "no hole" for chunks the
     * index guess missed.
     */
    const pipeline::MapChunk* findChunkAt(float glX, float glY,
                                          float& fracX, float& fracY,
                                          const TerrainTile** outTile = nullptr) const;

    /** The tones a terrain texture is painted in, cached by path.
     *
     * These textures already depict their region's own grass and foliage -
     * Elwynn's greens, Westfall's dry golds - so the grass growing out of one
     * takes its colours from it rather than from a palette of ours. Shadow
     * and highlight are luminance percentiles of the decoded base level, not
     * a mean: a mean of a grass texture is the average of its blades and the
     * earth between them, and grass wants the blades.
     *
     * Wrong answers here are a tint, not a hole, so failures return greys.
     */
    struct TerrainTextureTones {
        glm::vec3 shadow{0.35f};
        glm::vec3 highlight{0.65f};
    };
    [[nodiscard]] TerrainTextureTones getTerrainTextureTones(const std::string& texturePath);

    /** Get the precise MCNK AreaTable ID at a world position. */
    [[nodiscard]] std::optional<uint32_t> getAreaIdAt(float glX, float glY) const;


    /**
     * Get dominant terrain texture name at a GL position.
     * Returns empty if terrain is not loaded at that position.
     */
    [[nodiscard]] std::optional<std::string> getDominantTextureAt(float glX, float glY) const;
    /// TerrainType.SoundID for the ground under a point, as the stock client
    /// derives it: the dominant layer's GroundEffectTexture row names a
    /// TerrainType, whose SoundID keys FootstepTerrainLookup. 1 dirt, 2 metal,
    /// 3 stone, 4 snow, 5 wood, 6 grass, 7 leaves, 8 sand, 9 soggy, 10 water.
    /// Empty when the layer carries no effect or the tables are absent.
    [[nodiscard]] std::optional<uint32_t> getTerrainSoundTypeAt(float glX, float glY) const;

    /**
     * Get statistics
     */
    [[nodiscard]] int getLoadedTileCount() const { return static_cast<int>(loadedTiles.size()); }
    [[nodiscard]] int getPendingTileCount() const { return static_cast<int>(pendingTiles.size()); }
    [[nodiscard]] int getReadyQueueCount() const { return static_cast<int>(readyQueue.size()); }
    /** Total unfinished tiles (worker threads + ready queue + finalizing) */
    [[nodiscard]] int getRemainingTileCount() const { return static_cast<int>(pendingTiles.size() + readyQueue.size() + finalizingTiles_.size()); }
    [[nodiscard]] TileCoord getCurrentTile() const { return currentTile; }

    /** Process one ready tile (for loading screens with per-tile progress updates) */
    void processOneReadyTile();

    /** Process a bounded batch of ready tiles with async GPU upload (no sync wait) */
    void processReadyTiles();

    /** Unload a bounded batch of distant tiles queued by streamTiles() (no frame-spike) */
    void processPendingUnloads();

private:
    /**
     * Get tile coordinates from GL world position
     */
    [[nodiscard]] TileCoord worldToTile(float worldX, float worldY) const;

    /**
     * Get world bounds for a tile
     */
    void getTileBounds(const TileCoord& coord, float& minX, float& minY,
                       float& maxX, float& maxY) const;

    /**
     * Build ADT file path
     */
    [[nodiscard]] std::string getADTPath(const TileCoord& coord) const;

    /**
     * Load tiles in radius around current tile
     */
    void streamTiles();

    /**
     * Background thread: prepare tile data (CPU work only, no OpenGL)
     */
    std::shared_ptr<PendingTile> prepareTile(int x, int y);

    /**
     * Advance incremental finalization of a tile (one bounded unit of work).
     * Returns true when the tile is fully finalized (phase == DONE).
     */
    bool advanceFinalization(FinalizingTile& ft);

    /**
     * Background worker thread loop
     */
    void workerLoop();

    void ensureGroundEffectTablesLoaded();
    void generateGroundClutterPlacements(std::shared_ptr<PendingTile>& pending,
                                         std::unordered_set<uint32_t>& preparedModelIds);
    /// Place a loaded tile's clutter again at the current density, through
    /// the same generateGroundClutterPlacements the workers use, and drop the
    /// instances made from the old placements. Main thread.
    void regenerateTileClutter(TerrainTile& tile);

    pipeline::AssetManager* assetManager = nullptr;
    TerrainRenderer* terrainRenderer = nullptr;
    WaterRenderer* waterRenderer = nullptr;
    M2Renderer* m2Renderer = nullptr;
    WMORenderer* wmoRenderer = nullptr;
    audio::AmbientSoundManager* ambientSoundManager = nullptr;
    audio::WorldSoundEmitters* worldSoundEmitters = nullptr;

    // Each doodad model's $DSL / $DSO events, recorded when a worker parses
    // it: a model already uploaded is not parsed again, and its placements
    // in later tiles still need its sounds. An empty list = parsed, silent.
    std::unordered_map<uint32_t, std::vector<pipeline::M2Model::DoodadSound>> doodadSoundsByModel_;
    mutable std::mutex doodadSoundsMutex_;
    void recordDoodadSounds(uint32_t modelId, const pipeline::M2Model& model);
    /// Whether a parsed model has sound events of its own (false if unknown).
    bool modelHasDoodadSounds(uint32_t modelId) const;
    /// Register a placed doodad's sound events at its world transform.
    void addDoodadSounds(uint32_t modelId, const glm::mat4& modelMatrix,
                         std::vector<uint32_t>& outIds);
    void removeSoundEmitters(std::vector<uint32_t>& ids);

    std::string mapName = "Azeroth";

    // Loaded tiles (keyed by coordinate)
    std::unordered_map<TileCoord, std::unique_ptr<TerrainTile>, TileCoord::Hash> loadedTiles;

    // Tiles that failed to load (don't retry)
    std::unordered_map<TileCoord, bool, TileCoord::Hash> failedTiles;

    // Current tile (where camera is)
    TileCoord currentTile = {.x = -1, .y = -1};
    TileCoord lastStreamTile = {.x = -1, .y = -1};

    // Streaming parameters
    bool streamingEnabled = true;
    int loadRadius = 6;      // Load tiles within this radius (13x13 grid = 169 tiles)
    int unloadRadius = 9;    // Unload tiles beyond this radius
    float updateInterval = 0.033f;  // Check streaming every 33ms (~30 fps)
    float timeSinceLastUpdate = 0.0f;
    float proactiveStreamTimer_ = 0.0f;
    bool taxiStreamingMode_ = false;
    bool isCustomZone_ = false;
    UpdatePhases lastPhases_;
    // Ground clutter streaming settles: once a sweep made nothing and had
    // nothing stale to re-place, the next one only runs after the camera has
    // moved, the loaded tile set has changed, or a short timer has run out.
    // See streamGroundClutter.
    bool clutterSettled_ = false;
    glm::vec3 clutterSettledAt_{0.0f};
    size_t clutterSettledTiles_ = 0;
    uint32_t clutterSettledGeneration_ = 0;
    std::chrono::steady_clock::time_point clutterSettledSince_{};

    // Collision data for custom zones (loaded from WOC files)
    struct CollisionData {
        struct Triangle { glm::vec3 v0, v1, v2; uint8_t flags; };
        std::vector<Triangle> triangles;
        glm::vec3 boundsMin{1e30f}, boundsMax{-1e30f};
        bool loaded = false;
    };
    std::unordered_map<uint64_t, CollisionData> collisionTiles_;
    [[nodiscard]] uint64_t tileKey(int x, int y) const { return (static_cast<uint64_t>(x) << 32) | static_cast<uint32_t>(y); }

    // Tile size constants (WoW ADT specifications)
    // A tile (ADT) = 16x16 chunks = 533.33 units across
    // A chunk = 8x8 vertex quads = 33.33 units across
    static constexpr float TILE_SIZE = core::coords::TILE_SIZE;
    static constexpr float CHUNK_SIZE = 33.33333f;          // One chunk = 33.33 units

    // Background loading worker pool
    std::vector<std::thread> workerThreads;
    int workerCount = 0;
    // THREAD-SAFE: guards loadQueue, readyQueue, and pendingTiles.
    // Workers wait on queueCV; main thread signals when new tiles are enqueued
    // or when readyQueue drains below maxReadyQueueSize_.
    std::mutex queueMutex;
    std::condition_variable queueCV;
    std::deque<TileCoord> loadQueue;              // THREAD-SAFE: protected by queueMutex
    std::queue<std::shared_ptr<PendingTile>> readyQueue; // THREAD-SAFE: protected by queueMutex
    // Maximum number of prepared-but-not-finalized tiles in readyQueue.
    // Each prepared tile can hold 100–500 MB of decoded textures in RAM.
    // Workers sleep when this limit is reached, letting the main thread
    // finalize (GPU-upload + free) before more tiles are prepared.
    static constexpr size_t maxReadyQueueSize_ = 3;

    // In-RAM tile cache (LRU) to avoid re-reading from disk
    struct CachedTile {
        std::shared_ptr<PendingTile> tile;
        size_t bytes = 0;
        std::list<TileCoord>::iterator lruIt;
    };
    // THREAD-SAFE: protected by tileCacheMutex_.
    std::unordered_map<TileCoord, CachedTile, TileCoord::Hash> tileCache_;
    std::list<TileCoord> tileCacheLru_;
    size_t tileCacheBudgetBytes_ = 8ull * 1024 * 1024 * 1024; // Dynamic, set at init based on RAM
    std::mutex tileCacheMutex_;

    std::shared_ptr<PendingTile> getCachedTile(const TileCoord& coord);
    void logMissingAdtOnce(const std::string& adtPath);
    std::atomic<bool> workerRunning{false};

    // Track tiles currently queued or being processed to avoid duplicates
    std::unordered_map<TileCoord, bool, TileCoord::Hash> pendingTiles; // THREAD-SAFE: protected by queueMutex
    // Tiles a worker has taken and not yet handed to readyQueue. Protected by queueMutex.
    int preparingTiles_ = 0;
    // Whether the workers are waiting on memory, so the wait is reported once
    // per episode rather than once per retry. Protected by queueMutex.
    bool memoryWaitReported_ = false;
    std::unordered_set<std::string> missingAdtWarnings_; // THREAD-SAFE: protected by missingAdtWarningsMutex_
    std::mutex missingAdtWarningsMutex_;

    // Thread-safe set of M2 model IDs already uploaded to GPU
    // (checked by workers to skip redundant file I/O + parsing)
    std::unordered_set<uint32_t> uploadedM2Ids_;
    std::mutex uploadedM2IdsMutex_;

    // Cross-tile dedup for WMO doodad preparation on background workers
    // (prevents re-parsing thousands of doodads when same WMO spans multiple tiles)
    std::unordered_set<uint32_t> preparedWmoUniqueIds_;
    std::mutex preparedWmoUniqueIdsMutex_;

    // MAIN-THREAD-ONLY: tiles beyond unloadRadius, queued by streamTiles() and drained a
    // time-budgeted batch at a time by processPendingUnloads() each frame. Unloading them
    // all synchronously in one call (e.g. ~100 tiles right after a taxi landing snaps the
    // radius down) caused multi-second main-thread stalls - live-confirmed via "SLOW
    // terrainManager->update: 1943.71ms" immediately after "Unloaded 103 distant tiles" in
    // a real flight-landing log.
    //
    // This was originally a fixed per-frame *count* cap (8 tiles/frame) rather than a time
    // budget. That prevented the single-frame catastrophic stall but doesn't scale with
    // actual frame cost: streamTiles() discovers newly-out-of-range tiles at a roughly
    // constant real-world rate during a long, fast (taxi) flight, but a count-based drain
    // processes fewer tiles per *second* whenever frame rate drops for any reason - and once
    // the backlog (and therefore loadedTiles_/streamTiles()'s own per-call scan cost) starts
    // growing, frame rate drops further, which throttles the count-based drain further still.
    // Live-reproduced: a long cross-continent taxi flight (the first sustained-large-radius
    // tile churn any test had done) snowballed from ~50ms terrain-update frames early on to
    // 600ms+ later in the same flight. A time budget (matching processReadyTiles()/
    // advanceFinalization()'s existing pattern) scales unload throughput with whatever time
    // is actually available each frame instead of a fixed count, so it can't fall behind
    // real-world churn the way the count cap could.
    std::deque<TileCoord> pendingUnloadQueue_;

    // MAIN-THREAD-ONLY: checked and modified in processReadyTiles() and unloadDistantTiles(),
    // both of which run exclusively on the main thread.
    std::unordered_set<uint32_t> placedDoodadIds;

    // MAIN-THREAD-ONLY: same contract as placedDoodadIds.
    std::unordered_set<uint32_t> placedWmoIds;

    // Tiles currently being incrementally finalized across frames
    std::deque<FinalizingTile> finalizingTiles_;

    struct GroundEffectEntry {
        std::array<uint32_t, 4> doodadIds{{0, 0, 0, 0}};
        std::array<uint32_t, 4> weights{{0, 0, 0, 0}};
        uint32_t density = 0;
        uint32_t terrainType = 0;  ///< TerrainType.dbc id (GroundEffectTexture.Sound)
    };
    std::unordered_map<uint32_t, uint32_t> terrainTypeSoundId_;  // TerrainType id -> SoundID
    /// The layer with the most weight at a point, or null.
    const pipeline::TextureLayer* dominantLayerAt(float glX, float glY, const TerrainTile** outTile) const;
    bool groundEffectsLoaded_ = false;
    std::unordered_map<uint32_t, GroundEffectEntry> groundEffectById_; // effectId -> config
    std::unordered_map<uint32_t, std::string> groundDoodadModelById_;  // doodadId -> model path
    float groundClutterDensityScale_ = 1.0f;
    /// Bumped each time the density changes. Workers stamp it on the tiles
    /// they place; a loaded tile stamped with an older one is re-placed.
    std::atomic<uint32_t> clutterDensityGeneration_{0};
    /// When the density last changed, so re-placing waits for a slider drag
    /// to finish rather than restarting on every step of it.
    std::chrono::steady_clock::time_point clutterDensityChangedAt_{};
    std::unordered_map<std::string, TerrainTextureTones> terrainTextureTones_;
};

} // namespace rendering
} // namespace wowee
