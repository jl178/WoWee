#include "rendering/terrain_manager.hpp"

#include <vector>

#include "pipeline/adt_alpha.hpp"
#include "pipeline/grass_profile.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/m2_model_classifier.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/camera.hpp"
#include "audio/ambient_sound_manager.hpp"
#include "core/coordinates.hpp"
#include "pipeline/wowee_terrain_loader.hpp"
#include "pipeline/wowee_model.hpp"
#include "pipeline/wowee_building.hpp"
#include "pipeline/wowee_collision.hpp"
#include "core/memory_monitor.hpp"
#include "core/profiler.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/wmo_group_path.hpp"
#include "pipeline/terrain_mesh.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <limits>
#include <unordered_set>

#ifdef __linux__
#include <sched.h>
#include <pthread.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <array>
#include <cstring>

#endif

namespace wowee {
namespace rendering {

namespace {
/// The euler triple a placement's three degrees become, in render axes.
///
/// MDDF and MODF store the rotation identically and this was written out twice,
/// once for each; it is one function. Both are composed Z, Y, X, in
/// placement_transform.hpp.
glm::vec3 placementEuler(const float rotation[3]) {
    // Solved against Blizzard's own numbers rather than judged by eye. MODF
    // records, beside every building placement, the world-space bounding box
    // its tools computed for it - so a candidate convention can be scored by
    // whether it reproduces that box. Over the 1469 placements in 3.3.5's ADTs
    // carrying more than three degrees of pitch or roll, this triple composed
    // Z, Y, X reproduces every one of them to zero error, and the next best
    // candidate is out by a median of 4.3 yards.
    //
    // The note that stood here said the mapping was not where the error was,
    // and that a heightmap the bridges were judged against was the thing left
    // to suspect. It was the mapping. What hid it is that the signs and the
    // order are wrong together and right together: composed Z, Y, X these
    // components are positive, composed X, Y, Z the first two had to be
    // negated to keep flat ground looking right. Turning one dial at a time -
    // six orders, then the sign of each component - never reaches a pair that
    // only works as a pair, and every single-dial result looks worse than what
    // it replaced.
    //
    // That is also why a factor of four once looked like the closest thing to
    // an answer: with the order wrong, no constant can be right, and fitting
    // one to a single bridge produces a number that is wrong differently for
    // every other roll.
    constexpr float kDeg = core::coords::PI / 180.0f;
    return glm::vec3(rotation[2] * kDeg,
                     rotation[0] * kDeg,
                     (rotation[1] + 180.0f) * kDeg);
}


// Alpha map format constants live with the loader now - see
// pipeline/adt_alpha.hpp - because two copies of the decoder had grown and the
// grass terrain adapter would have made a third.
using pipeline::ALPHA_MAP_SIZE;

// Random float normalization: mask to 16-bit then divide by max value to get [0..1]
constexpr float kRand16Max = 65535.0f;

// Placement transform constants
constexpr float kInv1024  = 1.0f / 1024.0f;

int computeTerrainWorkerCount() {
    const char* raw = std::getenv("WOWEE_TERRAIN_WORKERS");
    if (raw && *raw) {
        char* end = nullptr;
        unsigned long long forced = std::strtoull(raw, &end, 10);
        if (end != raw && forced > 0) {
            return static_cast<int>(forced);
        }
    }

    unsigned hc = std::thread::hardware_concurrency();
    if (hc > 0) {
        // Keep terrain workers conservative by default. Over-subscribing loader
        // threads can starve main-thread networking/render updates on large-core CPUs.
        const unsigned reserved = (hc >= 16u) ? 4u : ((hc >= 8u) ? 2u : 1u);
        const unsigned maxDefaultWorkers = 8u;
        const unsigned targetWorkers = std::max(4u, std::min(maxDefaultWorkers, hc - reserved));
        return static_cast<int>(targetWorkers);
    }
    return 4;  // Fallback
}

using pipeline::decodeLayerAlpha;

std::string toLowerCopy(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}

} // namespace

TerrainManager::TerrainManager() {
}

TerrainManager::~TerrainManager() {
    stopWorkers();
}

bool TerrainManager::initialize(pipeline::AssetManager* assets, TerrainRenderer* renderer) {
    assetManager = assets;
    // On this thread, before any tile worker and before the first footstep
    // asks what the ground is made of.
    ensureGroundEffectTablesLoaded();
    terrainRenderer = renderer;

    if (!assetManager) {
        LOG_ERROR("Asset manager is null");
        return false;
    }

    if (!terrainRenderer) {
        LOG_ERROR("Terrain renderer is null");
        return false;
    }

    // Set dynamic tile cache budget.
    // Keep this lower so decompressed MPQ file cache can stay very aggressive.
    auto& memMonitor = core::MemoryMonitor::getInstance();
    tileCacheBudgetBytes_ = memMonitor.getRecommendedCacheBudget() / 4;
    LOG_INFO("Terrain tile cache budget: ", tileCacheBudgetBytes_ / (1024 * 1024), " MB (dynamic)");

    // Start background worker pool (dynamic: scales with available cores)
    // Keep defaults moderate; env override can increase if streaming is bottlenecked.
    workerRunning.store(true);
    workerCount = computeTerrainWorkerCount();
    workerThreads.reserve(workerCount);
    for (int i = 0; i < workerCount; i++) {
        workerThreads.emplace_back(&TerrainManager::workerLoop, this);
    }

    LOG_INFO("Terrain manager initialized (async loading enabled)");
    LOG_INFO("  Map: ", mapName);
    LOG_INFO("  Load radius: ", loadRadius, " tiles");
    LOG_INFO("  Unload radius: ", unloadRadius, " tiles");
    LOG_INFO("  Workers: ", workerCount);

    return true;
}

void TerrainManager::update(const Camera& camera, float deltaTime) {
    ZoneScopedN("TerrainManager::update");
    lastPhases_ = UpdatePhases{};
    if (!streamingEnabled || !assetManager || !terrainRenderer) {
        return;
    }

    // Phase timing: this call stalls for ~180ms when tiles arrive, and the
    // caller only sees one number. Both processReadyTiles and
    // processPendingUnloads claim to be time-budgeted internally, so knowing
    // which phase actually runs long says whether a budget is being exceeded or
    // whether the cost is in streamTiles enumerating the world.
    using clock = std::chrono::steady_clock;
    const auto tStart = clock::now();
    auto elapsedMs = [](clock::time_point a, clock::time_point b) {
        return std::chrono::duration<float, std::milli>(b - a).count();
    };
    float reconcileMs = 0.0f, readyMs = 0.0f, unloadMs = 0.0f, streamMs = 0.0f;
    // Reports the breakdown on the way out of any long call, whichever return
    // path is taken.
    struct PhaseReport {
        clock::time_point start;
        const float *reconcile, *ready, *unload, *stream;
        ~PhaseReport() {
            const float total = std::chrono::duration<float, std::milli>(
                clock::now() - start).count();
            if (total > 50.0f) {
                LOG_WARNING("SLOW terrain update ", total, "ms: reconcile=", *reconcile,
                            " readyTiles=", *ready, " unloads=", *unload,
                            " streamTiles=", *stream);
            }
        }
    } report{.start = tStart, .reconcile = &reconcileMs, .ready = &readyMs, .unload = &unloadMs, .stream = &streamMs};

    // Reconcile the "already uploaded" cache against models the renderer reaped
    // for being instanceless. Without this, a model freed after leaving an area
    // stays marked uploaded, so the next tile prep skips its load and pushes an
    // empty placeholder - doodads (e.g. Stormwind tunnel torches) then fail to
    // spawn on revisit with "M2 model has no renderable content".
    if (m2Renderer) {
        std::vector<uint32_t> reaped = m2Renderer->drainReapedModelIds();
        if (!reaped.empty()) {
            std::lock_guard<std::mutex> lock(uploadedM2IdsMutex_);
            for (uint32_t id : reaped) uploadedM2Ids_.erase(id);
        }
    }

    // Always process ready tiles each frame (GPU uploads from background thread)
    // Time-budgeted internally to prevent frame spikes.
    const auto tReconcile = clock::now();
    reconcileMs = elapsedMs(tStart, tReconcile);
    processReadyTiles();
    readyMs = elapsedMs(tReconcile, clock::now());

    // Always drain a bounded batch of pending unloads each frame - same
    // frame-spike rationale as processReadyTiles() above.
    const auto tUnloadStart = clock::now();
    processPendingUnloads();
    unloadMs = elapsedMs(tUnloadStart, clock::now());
    const auto tClutter = clock::now();
    streamGroundClutter(camera.getPosition());
    lastPhases_.readyMs = readyMs;
    lastPhases_.unloadMs = unloadMs;
    lastPhases_.clutterMs = elapsedMs(tClutter, clock::now());

    timeSinceLastUpdate += deltaTime;

    // Only update streaming periodically (not every frame)
    if (timeSinceLastUpdate < updateInterval) {
        return;
    }

    timeSinceLastUpdate = 0.0f;

    // Get current tile from camera position.
    glm::vec3 camPos = camera.getPosition();
    TileCoord newTile = worldToTile(camPos.x, camPos.y);

    // Check if we've moved to a different tile
    if (newTile.x != currentTile.x || newTile.y != currentTile.y) {
        LOG_DEBUG("Camera moved to tile [", newTile.x, ",", newTile.y, "]");
        currentTile = newTile;
    }

    // Stream tiles when player crosses a tile boundary
    if (newTile.x != lastStreamTile.x || newTile.y != lastStreamTile.y) {
        LOG_DEBUG("Streaming: cam=(", camPos.x, ",", camPos.y, ",", camPos.z,
                 ") tile=[", newTile.x, ",", newTile.y,
                 "] loaded=", loadedTiles.size());
        const auto tStream = clock::now();
        streamTiles();
        streamMs = elapsedMs(tStream, clock::now());
        lastStreamTile = newTile;
    } else {
        // Proactive loading: when workers are idle, periodically re-check for
        // unloaded tiles within range. Throttled to avoid hitching right after
        // world load when many tiles finalize simultaneously.
        proactiveStreamTimer_ += deltaTime;
        if (proactiveStreamTimer_ >= 2.0f) {
            proactiveStreamTimer_ = 0.0f;
            bool workersIdle;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                workersIdle = loadQueue.empty();
            }
            if (workersIdle) {
                const auto tStream = clock::now();
                streamTiles();
                streamMs = elapsedMs(tStream, clock::now());
            }
        }
    }
}

bool TerrainManager::enqueueTile(int x, int y) {
    TileCoord coord = {.x = x, .y = y};
    if (loadedTiles.find(coord) != loadedTiles.end()) {
        return true;
    }
    if (pendingTiles.find(coord) != pendingTiles.end()) {
        return true;
    }
    if (failedTiles.find(coord) != failedTiles.end()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        loadQueue.push_back(coord);
        pendingTiles[coord] = true;
    }
    queueCV.notify_all();
    return true;
}

std::shared_ptr<PendingTile> TerrainManager::prepareTile(int x, int y) {
    TileCoord coord = {.x = x, .y = y};
    if (auto cached = getCachedTile(coord)) {
        LOG_DEBUG("Using cached tile [", x, ",", y, "]");
        return cached;
    }

    LOG_DEBUG("Preparing tile [", x, ",", y, "] (CPU work)");

    // Early-exit check - worker should bail fast during shutdown
    if (!workerRunning.load()) return nullptr;

    // Try Wowee Open Terrain format first (custom zones)
    std::string wotBase = "custom_zones/" + mapName + "/" + mapName + "_" +
                          std::to_string(coord.x) + "_" + std::to_string(coord.y);
    auto terrainPtr = std::make_unique<pipeline::ADTTerrain>();
    bool loadedFromWot = false;

    if (pipeline::WoweeTerrainLoader::exists(wotBase)) {
        if (pipeline::WoweeTerrainLoader::load(wotBase, *terrainPtr)) {
            loadedFromWot = true;
            LOG_INFO("Loaded custom zone terrain: ", wotBase);
            // Load collision mesh if available
            if (pipeline::WoweeCollisionBuilder::exists(wotBase)) {
                auto woc = pipeline::WoweeCollisionBuilder::load(wotBase + ".woc");
                if (woc.isValid()) {
                    CollisionData cd;
                    cd.triangles.reserve(woc.triangles.size());
                    for (const auto& t : woc.triangles)
                        cd.triangles.push_back({.v0 = t.v0, .v1 = t.v1, .v2 = t.v2, .flags = t.flags});
                    cd.boundsMin = woc.bounds.min;
                    cd.boundsMax = woc.bounds.max;
                    cd.loaded = true;
                    collisionTiles_[tileKey(coord.x, coord.y)] = std::move(cd);
                    LOG_INFO("Loaded WOC collision: ", woc.triangles.size(), " triangles");
                }
            }
        }
    }

    // Also check output directory (editor exports here)
    if (!loadedFromWot) {
        std::string outputBase = "output/" + mapName + "/" + mapName + "_" +
                                 std::to_string(coord.x) + "_" + std::to_string(coord.y);
        if (pipeline::WoweeTerrainLoader::exists(outputBase)) {
            if (pipeline::WoweeTerrainLoader::load(outputBase, *terrainPtr)) {
                loadedFromWot = true;
                LOG_INFO("Loaded editor output terrain: ", outputBase);
                if (pipeline::WoweeCollisionBuilder::exists(outputBase)) {
                    auto woc = pipeline::WoweeCollisionBuilder::load(outputBase + ".woc");
                    if (woc.isValid()) {
                        CollisionData cd;
                        cd.triangles.reserve(woc.triangles.size());
                        for (const auto& t : woc.triangles)
                            cd.triangles.push_back({.v0 = t.v0, .v1 = t.v1, .v2 = t.v2, .flags = t.flags});
                        cd.boundsMin = woc.bounds.min;
                        cd.boundsMax = woc.bounds.max;
                        cd.loaded = true;
                        collisionTiles_[tileKey(coord.x, coord.y)] = std::move(cd);
                        LOG_INFO("Loaded WOC collision: ", woc.triangles.size(), " triangles");
                    }
                }
            }
        }
    }

    // Try WHM/WOT sidecar from the asset tree (asset_extract --emit-terrain
    // writes one alongside the ADT). This lets the runtime use the open
    // format without copying anything into custom_zones/.
    if (!loadedFromWot) {
        std::string adtPath = getADTPath(coord);
        std::string adtFsPath = assetManager->resolveFile(adtPath);
        if (!adtFsPath.empty() && adtFsPath.size() >= 4) {
            std::string sidecarBase = adtFsPath.substr(0, adtFsPath.size() - 4);
            if (pipeline::WoweeTerrainLoader::exists(sidecarBase) &&
                pipeline::WoweeTerrainLoader::load(sidecarBase, *terrainPtr)) {
                loadedFromWot = true;
                LOG_INFO("Loaded asset-tree WHM/WOT sidecar: ", sidecarBase);
                if (pipeline::WoweeCollisionBuilder::exists(sidecarBase)) {
                    auto woc = pipeline::WoweeCollisionBuilder::load(sidecarBase + ".woc");
                    if (woc.isValid()) {
                        CollisionData cd;
                        cd.triangles.reserve(woc.triangles.size());
                        for (const auto& t : woc.triangles)
                            cd.triangles.push_back({.v0 = t.v0, .v1 = t.v1, .v2 = t.v2, .flags = t.flags});
                        cd.boundsMin = woc.bounds.min;
                        cd.boundsMax = woc.bounds.max;
                        cd.loaded = true;
                        collisionTiles_[tileKey(coord.x, coord.y)] = std::move(cd);
                        LOG_INFO("Loaded sidecar WOC collision: ",
                                 woc.triangles.size(), " triangles");
                    }
                }
            }
        }
    }

    // Fall back to ADT format
    if (!loadedFromWot) {
        std::string adtPath = getADTPath(coord);
        auto adtData = assetManager->readFile(adtPath);

        if (adtData.empty()) {
            logMissingAdtOnce(adtPath);
            return nullptr;
        }

        *terrainPtr = pipeline::ADTLoader::load(adtData);
        if (!terrainPtr->isLoaded()) {
            LOG_ERROR("Failed to parse ADT terrain: ", adtPath);
            return nullptr;
        }
    }

    if (!workerRunning.load()) return nullptr;

    // WotLK split ADTs can store placements in *_obj0.adt.
    // Only needed for ADT-loaded tiles, not for WOT custom zones.
    if (!loadedFromWot) {
    std::string objPath = "World\\Maps\\" + mapName + "\\" + mapName + "_" +
                          std::to_string(coord.x) + "_" + std::to_string(coord.y) + "_obj0.adt";
    auto objData = assetManager->readFile(objPath);
    if (!objData.empty()) {
        auto objTerrain = std::make_unique<pipeline::ADTTerrain>(pipeline::ADTLoader::load(objData));
        if (objTerrain->isLoaded()) {
            const uint32_t doodadNameBase = static_cast<uint32_t>(terrainPtr->doodadNames.size());
            const uint32_t wmoNameBase = static_cast<uint32_t>(terrainPtr->wmoNames.size());

            terrainPtr->doodadNames.insert(terrainPtr->doodadNames.end(),
                                       objTerrain->doodadNames.begin(), objTerrain->doodadNames.end());
            terrainPtr->wmoNames.insert(terrainPtr->wmoNames.end(),
                                    objTerrain->wmoNames.begin(), objTerrain->wmoNames.end());

            std::unordered_set<uint32_t> existingDoodadUniqueIds;
            existingDoodadUniqueIds.reserve(terrainPtr->doodadPlacements.size());
            for (const auto& p : terrainPtr->doodadPlacements) {
                if (p.uniqueId != 0) existingDoodadUniqueIds.insert(p.uniqueId);
            }

            size_t mergedDoodads = 0;
            for (auto placement : objTerrain->doodadPlacements) {
                if (placement.nameId >= objTerrain->doodadNames.size()) continue;
                placement.nameId += doodadNameBase;
                if (placement.uniqueId != 0 && !existingDoodadUniqueIds.insert(placement.uniqueId).second) {
                    continue;
                }
                terrainPtr->doodadPlacements.push_back(placement);
                mergedDoodads++;
            }

            std::unordered_set<uint32_t> existingWmoUniqueIds;
            existingWmoUniqueIds.reserve(terrainPtr->wmoPlacements.size());
            for (const auto& p : terrainPtr->wmoPlacements) {
                if (p.uniqueId != 0) existingWmoUniqueIds.insert(p.uniqueId);
            }

            size_t mergedWmos = 0;
            for (auto placement : objTerrain->wmoPlacements) {
                if (placement.nameId >= objTerrain->wmoNames.size()) continue;
                placement.nameId += wmoNameBase;
                if (placement.uniqueId != 0 && !existingWmoUniqueIds.insert(placement.uniqueId).second) {
                    continue;
                }
                terrainPtr->wmoPlacements.push_back(placement);
                mergedWmos++;
            }

            if (mergedDoodads > 0 || mergedWmos > 0) {
                LOG_DEBUG("Merged obj0 tile [", x, ",", y, "]: +", mergedDoodads,
                          " doodads, +", mergedWmos, " WMOs");
            }
        }
    }
    } // end if (!loadedFromWot) obj0 merge

    // Set tile coordinates so mesh knows where to position this tile in world
    terrainPtr->coord.x = x;
    terrainPtr->coord.y = y;

    // The stock Azeroth ADT leaves two terrain-hole cells exposed along the
    // exterior edge of the Westfall lumbermill. The original client hides the
    // mask beneath the building footprint, but our WMO floor ends just short
    // of it, producing a small textureless strip between the inn/prison camp
    // and lumber yard. Repair only this confirmed exterior seam; other ADT
    // holes remain intact for caves and below-ground WMO entrances.
    if (toLowerCopy(mapName) == "azeroth" && x == 29 && y == 51) {
        auto& lumbermillChunk = terrainPtr->chunks[15 * 16 + 14];
        if (lumbermillChunk.indexX == 14 && lumbermillChunk.indexY == 15 &&
            lumbermillChunk.holes == 0x0044) {
            lumbermillChunk.holes = 0;
            LOG_INFO("Repaired Westfall lumbermill terrain seam in tile [29,51]");
        }
    }

    // Generate mesh
    pipeline::TerrainMesh mesh = pipeline::TerrainMeshGenerator::generate(*terrainPtr);
    if (mesh.validChunkCount == 0) {
        LOG_ERROR("Failed to generate terrain mesh for tile [", x, ",", y, "]");
        return nullptr;
    }

    if (!workerRunning.load()) return nullptr;

    auto pending = std::make_shared<PendingTile>();
    pending->coord = coord;
    pending->terrain = std::move(*terrainPtr);
    pending->mesh = std::move(mesh);

    std::unordered_set<uint32_t> preparedModelIds;
    auto ensureModelPrepared = [&](const std::string& m2Path,
                                   uint32_t modelId,
                                   int& skippedFileNotFound,
                                   int& skippedInvalid,
                                   int& skippedSkinNotFound) -> bool {
        if (preparedModelIds.find(modelId) != preparedModelIds.end()) return true;

        // Skip file I/O + parsing for models already uploaded to GPU from previous tiles
        {
            std::lock_guard<std::mutex> lock(uploadedM2IdsMutex_);
            if (uploadedM2Ids_.count(modelId)) {
                preparedModelIds.insert(modelId);
                return true;
            }
        }

        // Check for WOM open format first (custom zone models)
        // Try open WOM format first via shared helper. Per-zone prefixes are
        // checked before the global fallback so a zone export overrides a
        // generic custom asset of the same name.
        {
            std::vector<std::string> extraPrefixes = {
                "output/" + mapName + "/models/",
                "custom_zones/" + mapName + "/models/",
            };
            // Asset extractor's --emit-wom writes WOM sidecars next to the
            // M2 in the asset tree (e.g. <data>/world/maps/foo/foo.wom).
            // Add the data path as a prefix so the runtime picks them up
            // without needing to copy them into custom_zones/.
            if (assetManager && !assetManager->getDataPath().empty()) {
                extraPrefixes.push_back(assetManager->getDataPath() + "/");
            }
            auto wom = pipeline::WoweeModelLoader::tryLoadByGamePath(m2Path, extraPrefixes);
            if (wom.isValid()) {
                auto m2Model = pipeline::WoweeModelLoader::toM2(wom);
                m2Model.name = m2Path;
                pending->m2Models.push_back({.modelId = modelId, .model = std::move(m2Model), .path = {}});
                preparedModelIds.insert(modelId);
                LOG_INFO("Loaded WOM model: ", m2Path, " (v", wom.version,
                         ", ", wom.batches.size(), " batches)");
                return true;
            }
        }

        std::vector<uint8_t> m2Data = assetManager->readFile(m2Path);
        if (m2Data.empty()) {
            skippedFileNotFound++;
            return false;
        }

        pipeline::M2Model m2Model = pipeline::M2Loader::load(m2Data);
        // The asset path carries foliage semantics; embedded names frequently
        // do not. Always classify ADT doodads using the path.
        m2Model.name = m2Path;
        std::string skinPath = pipeline::skinPathForM2(m2Path);
        std::vector<uint8_t> skinData = assetManager->readFileOptional(skinPath);
        if (!skinData.empty() && m2Model.version >= 264) {
            pipeline::M2Loader::loadSkin(skinData, m2Model);
        } else if (skinData.empty() && m2Model.version >= 264) {
            skippedSkinNotFound++;
        }

        if (!m2Model.isValid()) {
            skippedInvalid++;
            LOG_DEBUG("M2 model invalid (no verts/indices): ", m2Path);
            return false;
        }

        // Pre-decode M2 model textures on background thread
        for (const auto& tex : m2Model.textures) {
            if (tex.filename.empty()) continue;
            std::string texKey = tex.filename;
            std::replace(texKey.begin(), texKey.end(), '/', '\\');
            std::transform(texKey.begin(), texKey.end(), texKey.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (pending->preloadedM2Textures.find(texKey) != pending->preloadedM2Textures.end()) continue;
            auto blp = assetManager->loadTexture(texKey, true);
            if (blp.isValid()) {
                pending->preloadedM2Textures[texKey] = std::move(blp);
            }
        }

        PendingTile::M2Ready ready;
        ready.modelId = modelId;
        ready.model = std::move(m2Model);
        ready.path = m2Path;
        pending->m2Models.push_back(std::move(ready));
        preparedModelIds.insert(modelId);
        return true;
    };

    // Pre-load M2 doodads (CPU: read files, parse models)
    int skippedNameId = 0, skippedFileNotFound = 0, skippedInvalid = 0, skippedSkinNotFound = 0;
    for (const auto& placement : pending->terrain.doodadPlacements) {
        if (!workerRunning.load()) return nullptr;
        if (placement.nameId >= pending->terrain.doodadNames.size()) {
            skippedNameId++;
            continue;
        }

        std::string m2Path = pending->terrain.doodadNames[placement.nameId];
        // .mdx and .mdl both mean the .m2 that shipped. This site knew only
        // .mdx, so an ADT doodad named with .mdl was looked up as ".mdl", found
        // nothing, and did not appear.
        m2Path = pipeline::modelPathToM2(m2Path);

        uint32_t modelId = static_cast<uint32_t>(std::hash<std::string>{}(m2Path));
        if (!ensureModelPrepared(m2Path, modelId, skippedFileNotFound, skippedInvalid, skippedSkinNotFound)) {
            continue;
        }

        float wowX = placement.position[0];
        float wowY = placement.position[1];
        float wowZ = placement.position[2];
        glm::vec3 glPos = core::coords::adtToWorld(wowX, wowY, wowZ);

        PendingTile::M2Placement p;
        p.modelId = modelId;
        p.uniqueId = placement.uniqueId;
        p.position = glPos;
        p.rotation = placementEuler(placement.rotation);
        p.scale = placement.scale * kInv1024;
        pending->m2Placements.push_back(p);
    }

    if (skippedNameId > 0 || skippedFileNotFound > 0 || skippedInvalid > 0 || skippedSkinNotFound > 0) {
        LOG_DEBUG("Tile [", x, ",", y, "] doodad issues: ",
                  skippedNameId, " bad nameId, ",
                  skippedFileNotFound, " file not found, ",
                  skippedInvalid, " invalid model, ",
                  skippedSkinNotFound, " skin not found");
    }

    // Procedural ground clutter from terrain layer effectId -> GroundEffectTexture/Doodad DBCs.
    ensureGroundEffectTablesLoaded();
    // Stamped before placing: a density change that lands while this runs
    // leaves the tile one generation behind, and it is placed again once
    // loaded rather than keeping the old density.
    pending->clutterGeneration = clutterDensityGeneration_.load();
    generateGroundClutterPlacements(pending, preparedModelIds);

    if (!workerRunning.load()) return nullptr;

    // Pre-load WMOs (CPU: read files, parse models and groups)
    if (!pending->terrain.wmoPlacements.empty()) {
        for (const auto& placement : pending->terrain.wmoPlacements) {
            if (!workerRunning.load()) return nullptr;
            if (placement.nameId >= pending->terrain.wmoNames.size()) continue;

            const std::string& wmoPath = pending->terrain.wmoNames[placement.nameId];

            // Check for WOB open format first (custom zone buildings)
            bool wobLoaded = false;
            pipeline::WMOModel wmoModel;
            {
                // Per-zone overrides win over global custom_zones/ overrides.
                std::vector<std::string> extraPrefixes = {
                    "output/" + mapName + "/buildings/",
                    "custom_zones/" + mapName + "/buildings/",
                };
                // asset_extract --emit-wob writes WOB next to the WMO in
                // the asset tree; add the data path so the runtime picks
                // them up there too.
                if (assetManager && !assetManager->getDataPath().empty()) {
                    extraPrefixes.push_back(assetManager->getDataPath() + "/");
                }
                auto wob = pipeline::WoweeBuildingLoader::tryLoadByGamePath(
                    wmoPath, extraPrefixes);
                if (wob.isValid() &&
                    pipeline::WoweeBuildingLoader::toWMOModel(wob, wmoModel)) {
                    LOG_INFO("Loaded WOB building: ", wmoPath);
                    wobLoaded = true;
                }
            }

            if (!wobLoaded) {
                std::vector<uint8_t> wmoData = assetManager->readFile(wmoPath);
                if (wmoData.empty()) continue;

                wmoModel = pipeline::WMOLoader::load(wmoData);
                if (wmoModel.nGroups > 0) {
                    for (uint32_t gi = 0; gi < wmoModel.nGroups; gi++) {
                        for (const std::string& groupPath :
                             pipeline::wmoGroupCandidates(wmoPath, gi)) {
                            std::vector<uint8_t> groupData =
                                assetManager->readFile(groupPath);
                            if (groupData.empty()) continue;
                            pipeline::WMOLoader::loadGroup(groupData, wmoModel, gi);
                            break;
                        }
                    }
                }
            }

            if (!wmoModel.groups.empty()) {
                wmoModel.sourcePath = wmoPath;
                glm::vec3 pos = core::coords::adtToWorld(placement.position[0],
                                                       placement.position[1],
                                                       placement.position[2]);

                glm::vec3 rot = placementEuler(placement.rotation);

                // Pre-load WMO doodads (M2 models inside WMO)
                if (!workerRunning.load()) return nullptr;

                // Skip WMO doodads if this placement was already prepared by another tile's worker.
                // This prevents 15+ copies of Stormwind's ~6000 doodads from being parsed
                // simultaneously, which was the primary cause of OOM during world load.
                bool wmoAlreadyPrepared = false;
                if (placement.uniqueId != 0) {
                    std::lock_guard<std::mutex> lock(preparedWmoUniqueIdsMutex_);
                    wmoAlreadyPrepared = !preparedWmoUniqueIds_.insert(placement.uniqueId).second;
                }

                if (!wmoAlreadyPrepared && !wmoModel.doodadSets.empty() && !wmoModel.doodads.empty()) {
                    glm::mat4 wmoMatrix(1.0f);
                    wmoMatrix = glm::translate(wmoMatrix, pos);
                    wmoMatrix = glm::rotate(wmoMatrix, rot.z, glm::vec3(0, 0, 1));
                    wmoMatrix = glm::rotate(wmoMatrix, rot.y, glm::vec3(0, 1, 0));
                    wmoMatrix = glm::rotate(wmoMatrix, rot.x, glm::vec3(1, 0, 0));

                    // Load doodads from set 0 (global) + placement-specific set
                    std::vector<uint32_t> setsToLoad = {0};
                    if (placement.doodadSet > 0 && placement.doodadSet < wmoModel.doodadSets.size()) {
                        setsToLoad.push_back(placement.doodadSet);
                    }
                    std::unordered_set<uint32_t> loadedDoodadIndices;
                    std::unordered_set<uint32_t> wmoPreparedModelIds;  // within-WMO model dedup
                    for (uint32_t setIdx : setsToLoad) {
                        const auto& doodadSet = wmoModel.doodadSets[setIdx];
                    for (uint32_t di = 0; di < doodadSet.count; di++) {
                        uint32_t doodadIdx = doodadSet.startIndex + di;
                        if (doodadIdx >= wmoModel.doodads.size()) break;
                        if (!loadedDoodadIndices.insert(doodadIdx).second) continue;

                        const auto& doodad = wmoModel.doodads[doodadIdx];
                        auto nameIt = wmoModel.doodadNames.find(doodad.nameIndex);
                        if (nameIt == wmoModel.doodadNames.end()) continue;

                        std::string m2Path = nameIt->second;
                        if (m2Path.empty()) continue;

                        m2Path = pipeline::modelPathToM2(m2Path);

                        uint32_t doodadModelId = static_cast<uint32_t>(std::hash<std::string>{}(m2Path));

                        // Skip file I/O if model already uploaded or already prepared within this WMO
                        bool modelAlreadyUploaded = false;
                        {
                            std::lock_guard<std::mutex> lock(uploadedM2IdsMutex_);
                            modelAlreadyUploaded = uploadedM2Ids_.count(doodadModelId) > 0;
                        }
                        // Membership check only - the id is claimed below, after a
                        // successful prep. Claiming up front meant a model whose first
                        // occurrence failed (missing/invalid file) poisoned every later
                        // occurrence into an empty placeholder push, which finalize then
                        // rejected with "no renderable content".
                        bool modelAlreadyPreparedInWmo = wmoPreparedModelIds.count(doodadModelId) > 0;

                        pipeline::M2Model m2Model;
                        if (!modelAlreadyUploaded && !modelAlreadyPreparedInWmo) {
                            std::vector<uint8_t> m2Data = assetManager->readFile(m2Path);
                            if (m2Data.empty()) continue;

                            m2Model = pipeline::M2Loader::load(m2Data);
                            m2Model.name = m2Path;
                            std::string skinPath = pipeline::skinPathForM2(m2Path);
                            std::vector<uint8_t> skinData = assetManager->readFile(skinPath);
                            if (!skinData.empty() && m2Model.version >= 264) {
                                pipeline::M2Loader::loadSkin(skinData, m2Model);
                            }
                            if (!m2Model.isValid()) continue;
                            wmoPreparedModelIds.insert(doodadModelId);

                            // Pre-decode doodad M2 textures on background thread
                            for (const auto& tex : m2Model.textures) {
                                if (tex.filename.empty()) continue;
                                std::string texKey = tex.filename;
                                std::replace(texKey.begin(), texKey.end(), '/', '\\');
                                std::transform(texKey.begin(), texKey.end(), texKey.begin(),
                                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                                if (pending->preloadedM2Textures.find(texKey) != pending->preloadedM2Textures.end()) continue;
                                auto blp = assetManager->loadTexture(texKey, true);
                                if (blp.isValid()) {
                                    pending->preloadedM2Textures[texKey] = std::move(blp);
                                }
                            }
                        }

                        // Build doodad's local transform (WoW coordinates)
                        // WMO doodads use quaternion rotation
                        glm::quat fixedRotation(doodad.rotation.w, doodad.rotation.x, doodad.rotation.y, doodad.rotation.z);

                        glm::mat4 doodadLocal(1.0f);
                        doodadLocal = glm::translate(doodadLocal, doodad.position);
                        doodadLocal *= glm::mat4_cast(fixedRotation);
                        doodadLocal = glm::scale(doodadLocal, glm::vec3(doodad.scale));

                        // Full world transform = WMO world transform * doodad local transform
                        glm::mat4 worldMatrix = wmoMatrix * doodadLocal;

                        // Extract world position for frustum culling
                        glm::vec3 worldPos = glm::vec3(worldMatrix[3]);

                        // Detect ambient sound emitters from doodad model path
                        std::string m2PathLower = m2Path;
                        std::transform(m2PathLower.begin(), m2PathLower.end(), m2PathLower.begin(), ::tolower);

                        // Debug: Log all doodad paths to help identify fire-related models
                        static int doodadLogCount = 0;
                        if (doodadLogCount < 50) {  // Limit logging to first 50 doodads
                            LOG_DEBUG("WMO doodad: ", m2Path);
                            doodadLogCount++;
                        }

                        auto emitterType = rendering::classifyAmbientEmitter(m2PathLower);
                        if (emitterType != rendering::AmbientEmitterType::None) {
                            PendingTile::AmbientEmitter emitter;
                            emitter.position = worldPos;
                            // Map classifier enum to AmbientSoundManager type codes
                            switch (emitterType) {
                                case rendering::AmbientEmitterType::FireplaceSmall: emitter.type = 0; break;
                                case rendering::AmbientEmitterType::FireplaceLarge: emitter.type = 1; break;
                                case rendering::AmbientEmitterType::Torch:          emitter.type = 2; break;
                                case rendering::AmbientEmitterType::Fountain:       emitter.type = 3; break;
                                case rendering::AmbientEmitterType::Waterfall:      emitter.type = 6; break;
                                case rendering::AmbientEmitterType::Forge:          emitter.type = 1; break; // Forge → large fire
                                default: emitter.type = 0; break;
                            }
                            pending->ambientEmitters.push_back(emitter);
                        }

                        PendingTile::WMODoodadReady doodadReady;
                        doodadReady.modelId = doodadModelId;
                        doodadReady.model = std::move(m2Model);
                        doodadReady.worldPosition = worldPos;
                        doodadReady.modelMatrix = worldMatrix;
                        pending->wmoDoodads.push_back(std::move(doodadReady));
                    }
                    }
                }

                // Pre-decode WMO textures on background thread
                for (const auto& texPath : wmoModel.textures) {
                    if (texPath.empty()) continue;
                    std::string texKey = texPath;
                    // Truncate at NUL (WMO paths can have stray bytes)
                    size_t nul = texKey.find('\0');
                    if (nul != std::string::npos) texKey.resize(nul);
                    std::replace(texKey.begin(), texKey.end(), '/', '\\');
                    std::transform(texKey.begin(), texKey.end(), texKey.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (texKey.empty()) continue;
                    if (pending->preloadedWMOTextures.find(texKey) != pending->preloadedWMOTextures.end()) continue;
                    // Try .blp variant
                    std::string blpKey = texKey;
                    if (blpKey.size() >= 4) {
                        std::string ext = blpKey.substr(blpKey.size() - 4);
                        if (ext == ".tga" || ext == ".dds") {
                            blpKey = blpKey.substr(0, blpKey.size() - 4) + ".blp";
                        }
                    }
                    // Blocks for the upload; the normal map decodes a copy
                    // here on the worker thread and lets it go, which is where
                    // that cost already was.
                    auto blp = assetManager->loadTexture(blpKey, true);
                    if (blp.isValid()) {
                        float variance = 0.0f;
                        const std::vector<uint8_t> decoded =
                            blp.isBlockCompressed()
                                ? pipeline::BLPLoader::decodeBaseLevel(blp)
                                : blp.data;
                        auto normalPixels = decoded.empty()
                            ? pipeline::BLPImage{}
                            : WMORenderer::generateNormalHeightMapPixels(
                                  decoded.data(), static_cast<uint32_t>(blp.width),
                                  static_cast<uint32_t>(blp.height), variance);
                        if (normalPixels.isValid()) {
                            pending->preloadedWMONormalMaps[blpKey] = std::move(normalPixels);
                            pending->preloadedWMONormalMapVariances[blpKey] = variance;
                        }
                        pending->preloadedWMOTextures[blpKey] = std::move(blp);
                    }
                }

                PendingTile::WMOReady ready;
                // Cache WMO model uploads by path; placement dedup uses uniqueId separately.
                ready.modelId = static_cast<uint32_t>(std::hash<std::string>{}(wmoPath));
                if (ready.modelId == 0) ready.modelId = 1;
                ready.uniqueId = placement.uniqueId;
                ready.model = std::move(wmoModel);
                ready.position = pos;
                ready.rotation = rot;
                ready.scale = placement.scale > 0
                    ? static_cast<float>(placement.scale) / 1024.0f : 1.0f;
                ready.nameSet = placement.nameSet;
                pending->wmoModels.push_back(std::move(ready));
            }
        }
    }

    if (!workerRunning.load()) return nullptr;

    // Pre-load terrain texture BLP data on background thread so finalizeTile
    // doesn't block the main thread with file I/O.
    for (const auto& texPath : pending->terrain.textures) {
        if (pending->preloadedTextures.find(texPath) != pending->preloadedTextures.end()) continue;
        // Terrain tilesets and M2 skins stay in their blocks - both are only
        // sampled, and M2's transparency now comes from the blocks too. The
        // normal-map source below still decodes: it reads the pixels.
        pending->preloadedTextures[texPath] = assetManager->loadTexture(texPath, true);
    }

    LOG_DEBUG("Prepared tile [", x, ",", y, "]: ",
             pending->m2Models.size(), " M2 models, ",
             pending->m2Placements.size(), " M2 placements, ",
             pending->wmoModels.size(), " WMOs, ",
             pending->wmoDoodads.size(), " WMO doodads, ",
             pending->preloadedTextures.size(), " textures");

    return pending;
}

void TerrainManager::logMissingAdtOnce(const std::string& adtPath) {
    std::string normalized = adtPath;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::lock_guard<std::mutex> lock(missingAdtWarningsMutex_);
    if (missingAdtWarnings_.insert(normalized).second) {
        LOG_WARNING("Failed to load ADT file: ", adtPath);
    }
}

bool TerrainManager::advanceFinalization(FinalizingTile& ft) {
    auto& pending = ft.pending;
    int x = pending->coord.x;
    int y = pending->coord.y;
    TileCoord coord = pending->coord;

    switch (ft.phase) {

    case FinalizationPhase::TERRAIN: {
        // Check if tile was already loaded or failed
        if (loadedTiles.find(coord) != loadedTiles.end() || failedTiles.find(coord) != failedTiles.end()) {
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                pendingTiles.erase(coord);
            }
            ft.phase = FinalizationPhase::DONE;
            return true;
        }

        // Upload pre-loaded textures (once)
        if (!ft.terrainPreloaded) {
            LOG_DEBUG("Finalizing tile [", x, ",", y, "] (incremental)");
            if (!pending->preloadedTextures.empty()) {
                terrainRenderer->uploadPreloadedTextures(pending->preloadedTextures);
            }
            ft.terrainPreloaded = true;
            // Yield after preload to give time budget a chance to interrupt
            return false;
        }

        // Upload terrain chunks incrementally (16 per call to spread across frames)
        if (!ft.terrainMeshDone) {
            if (pending->mesh.validChunkCount == 0) {
                LOG_ERROR("Failed to upload terrain to GPU for tile [", x, ",", y, "]");
                failedTiles[coord] = true;
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    pendingTiles.erase(coord);
                }
                ft.phase = FinalizationPhase::DONE;
                return true;
            }
            bool allDone = terrainRenderer->loadTerrainIncremental(
                pending->mesh, pending->terrain.textures, x, y,
                ft.terrainChunkNext, 16);
            if (!allDone) {
                return false; // More chunks remain - yield to time budget
            }
            ft.terrainMeshDone = true;
        }

        // Load water after all terrain chunks are uploaded
        if (waterRenderer) {
            size_t beforeSurfaces = waterRenderer->getSurfaceCount();
            waterRenderer->loadFromTerrain(pending->terrain, true, x, y);
            size_t afterSurfaces = waterRenderer->getSurfaceCount();
            if (afterSurfaces > beforeSurfaces) {
                LOG_INFO("Water: tile [", x, ",", y, "] added ", afterSurfaces - beforeSurfaces,
                         " surfaces (total: ", afterSurfaces, ")");
            }
        } else {
            LOG_WARNING("Water: waterRenderer is null during tile [", x, ",", y, "] finalization!");
        }

        // Ensure M2 renderer has asset manager
        if (m2Renderer && assetManager) {
            if (!m2Renderer->initialize(nullptr, VK_NULL_HANDLE, assetManager))
                LOG_WARNING("M2Renderer terrain re-init failed");
        }

        ft.phase = FinalizationPhase::M2_MODELS;
        return false;
    }

    case FinalizationPhase::M2_MODELS: {
        // Upload multiple M2 models per call (batched GPU uploads).
        // When no more tiles are queued for background parsing, increase the
        // per-frame budget so idle workers don't waste time waiting for the
        // main thread to trickle-upload models.
        if (m2Renderer && ft.m2ModelIndex < pending->m2Models.size()) {
            // Set pre-decoded BLP cache so loadTexture() skips main-thread BLP decode
            m2Renderer->setPredecodedBLPCache(&pending->preloadedM2Textures);
            bool workersIdle;
            {
                std::lock_guard<std::mutex> lk(queueMutex);
                workersIdle = loadQueue.empty() && readyQueue.empty();
            }
            const size_t kModelsPerStep = workersIdle ? 6 : 4;
            size_t uploaded = 0;
            while (ft.m2ModelIndex < pending->m2Models.size() && uploaded < kModelsPerStep) {
                auto& m2Ready = pending->m2Models[ft.m2ModelIndex];
                if (m2Renderer->loadModel(m2Ready.model, m2Ready.modelId)) {
                    ft.uploadedM2ModelIds.insert(m2Ready.modelId);
                    // Track uploaded model IDs so background threads can skip re-reading
                    std::lock_guard<std::mutex> lock(uploadedM2IdsMutex_);
                    uploadedM2Ids_.insert(m2Ready.modelId);
                }
                ft.m2ModelIndex++;
                uploaded++;
            }
            m2Renderer->setPredecodedBLPCache(nullptr);
            // Stay in this phase until all models uploaded
            if (ft.m2ModelIndex < pending->m2Models.size()) {
                return false;
            }
        }
        if (!ft.uploadedM2ModelIds.empty()) {
            LOG_DEBUG("  Uploaded ", ft.uploadedM2ModelIds.size(), " M2 models for tile [", x, ",", y, "]");
        }
        ft.phase = FinalizationPhase::M2_INSTANCES;
        return false;
    }

    case FinalizationPhase::M2_INSTANCES: {
        // Create M2 instances incrementally to avoid main-thread stalls.
        // createInstance includes an O(n) bone-sibling scan that becomes expensive
        // on dense tiles with many placements and a large existing instance list.
        if (m2Renderer && ft.m2InstanceIndex < pending->m2Placements.size()) {
            // A fixed count was overshooting: 32 instances measured at 19ms
            // against this phase's 8ms budget, because createInstance carries an
            // O(n) bone-sibling scan whose cost grows with the instances already
            // present. Bound by time instead, which holds regardless of how
            // heavy each one turns out to be.
            constexpr size_t kInstancesPerStep = 32;
            constexpr float kInstanceBudgetMs = 4.0f;
            const auto instanceStart = std::chrono::steady_clock::now();
            size_t created = 0;
            while (ft.m2InstanceIndex < pending->m2Placements.size() && created < kInstancesPerStep) {
                if (created > 0 && std::chrono::duration<float, std::milli>(
                        std::chrono::steady_clock::now() - instanceStart).count() >= kInstanceBudgetMs) {
                    break;
                }
                const auto& p = pending->m2Placements[ft.m2InstanceIndex++];
                if (p.uniqueId != 0 && placedDoodadIds.count(p.uniqueId)) {
                    continue;
                }
                if (!m2Renderer->hasModel(p.modelId)) {
                    continue;
                }
                uint32_t instId = m2Renderer->createInstance(p.modelId, p.position, p.rotation, p.scale);
                if (instId) {
                    ft.m2InstanceIds.push_back(instId);
                    if (p.uniqueId != 0) {
                        placedDoodadIds.insert(p.uniqueId);
                        ft.tileUniqueIds.push_back(p.uniqueId);
                    }
                    created++;
                }
            }
            if (ft.m2InstanceIndex < pending->m2Placements.size()) {
                return false; // More instances to create - yield
            }
            LOG_DEBUG("  Loaded doodads for tile [", x, ",", y, "]: ",
                     ft.m2InstanceIds.size(), " instances (", ft.uploadedM2ModelIds.size(), " new models)");
        }
        ft.phase = FinalizationPhase::WMO_MODELS;
        return false;
    }

    case FinalizationPhase::WMO_MODELS: {
        // Upload multiple WMO models per call (batched GPU uploads)
        if (wmoRenderer && assetManager) {
            if (!wmoRenderer->initialize(nullptr, VK_NULL_HANDLE, assetManager))
                LOG_WARNING("WMORenderer terrain re-init failed");
            // Diffuse decode and normal/height generation were completed by the
            // terrain worker. The main thread only uploads those prepared pixels.
            wmoRenderer->setPredecodedBLPCache(&pending->preloadedWMOTextures);
            wmoRenderer->setPredecodedNormalMapCache(
                &pending->preloadedWMONormalMaps,
                &pending->preloadedWMONormalMapVariances);
            wmoRenderer->setDeferNormalMaps(true);

            // One model per step, and a large model spread across several
            // steps: a 286-group WMO took 131ms to upload in one go, against
            // this phase's 8ms budget. Uploading two per step when the workers
            // were idle only doubled that worst case.
            constexpr float kWmoGroupBudgetMs = 6.0f;
            while (ft.wmoModelIndex < pending->wmoModels.size()) {
                auto& wmoReady = pending->wmoModels[ft.wmoModelIndex];
                if (wmoReady.uniqueId != 0 && placedWmoIds.count(wmoReady.uniqueId)) {
                    ft.wmoModelIndex++;
                    continue;
                }
                const auto result = wmoRenderer->loadModelIncremental(
                    wmoReady.model, wmoReady.modelId, kWmoGroupBudgetMs);
                if (result == WMORenderer::ModelLoadResult::InProgress) {
                    break;  // same model resumes on the next call
                }
                ft.wmoModelIndex++;  // Complete or Failed - either way, move on
                break;               // one model per step
            }
            wmoRenderer->setDeferNormalMaps(false);
            wmoRenderer->setPredecodedBLPCache(nullptr);
            wmoRenderer->setPredecodedNormalMapCache(nullptr, nullptr);
            if (ft.wmoModelIndex < pending->wmoModels.size()) return false;
        }
        ft.phase = FinalizationPhase::WMO_INSTANCES;
        return false;
    }

    case FinalizationPhase::WMO_INSTANCES: {
        // Create WMO instances incrementally to avoid stalls on tiles with many WMOs.
        // Liquid group loading is also budgeted (max 4 per call) to prevent stalls
        // on WMOs with many liquid groups (e.g. Stormwind canals).
        if (wmoRenderer && ft.wmoInstanceIndex < pending->wmoModels.size()) {
            constexpr size_t kWmoInstancesPerStep = 4;
            constexpr size_t kLiquidGroupsPerStep = 4;
            size_t created = 0;
            size_t liquidGroupsLoaded = 0;
            while (ft.wmoInstanceIndex < pending->wmoModels.size() && created < kWmoInstancesPerStep) {
                auto& wmoReady = pending->wmoModels[ft.wmoInstanceIndex];
                // Skip duplicates and unloaded models
                if (wmoReady.uniqueId != 0 && placedWmoIds.count(wmoReady.uniqueId)) {
                    ft.wmoInstanceIndex++;
                    ft.wmoLiquidGroupIndex = 0;
                    continue;
                }
                if (!wmoRenderer->isModelLoaded(wmoReady.modelId)) {
                    ft.wmoInstanceIndex++;
                    ft.wmoLiquidGroupIndex = 0;
                    continue;
                }
                // Create the instance on first visit (liquidGroupIndex == 0)
                if (ft.wmoLiquidGroupIndex == 0) {
                    uint32_t wmoInstId = wmoRenderer->createInstance(
                        wmoReady.modelId, wmoReady.position, wmoReady.rotation, wmoReady.scale);
                    if (!wmoInstId) {
                        ft.wmoInstanceIndex++;
                        continue;
                    }
                    wmoRenderer->setInstanceNameSet(wmoInstId, wmoReady.nameSet);
                    ft.wmoInstanceIds.push_back(wmoInstId);
                    if (wmoReady.uniqueId != 0) {
                        placedWmoIds.insert(wmoReady.uniqueId);
                        ft.tileWmoUniqueIds.push_back(wmoReady.uniqueId);
                    }
                }
                // Load WMO liquids incrementally (canals, pools, etc.)
                if (waterRenderer) {
                    uint32_t wmoInstId = ft.wmoInstanceIds.back();
                    glm::mat4 modelMatrix = glm::mat4(1.0f);
                    modelMatrix = glm::translate(modelMatrix, wmoReady.position);
                    modelMatrix = glm::rotate(modelMatrix, wmoReady.rotation.z, glm::vec3(0.0f, 0.0f, 1.0f));
                    modelMatrix = glm::rotate(modelMatrix, wmoReady.rotation.y, glm::vec3(0.0f, 1.0f, 0.0f));
                    modelMatrix = glm::rotate(modelMatrix, wmoReady.rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));
                    const auto& groups = wmoReady.model.groups;
                    while (ft.wmoLiquidGroupIndex < groups.size() && liquidGroupsLoaded < kLiquidGroupsPerStep) {
                        const auto& group = groups[ft.wmoLiquidGroupIndex];
                        ft.wmoLiquidGroupIndex++;
                        if (!group.liquid.hasLiquid()) continue;
                        if (group.flags & 0x2000) {
                            uint16_t lt = group.liquid.materialId;
                            uint8_t basicType = (lt == 0) ? 0 : ((lt - 1) % 4);
                            if (basicType < 2) continue;
                        }
                        waterRenderer->loadFromWMO(group.liquid, modelMatrix, wmoInstId);
                        liquidGroupsLoaded++;
                    }
                    // More liquid groups remain on this WMO - yield
                    if (ft.wmoLiquidGroupIndex < groups.size()) {
                        return false;
                    }
                }
                ft.wmoInstanceIndex++;
                ft.wmoLiquidGroupIndex = 0;
                created++;
            }
            if (ft.wmoInstanceIndex < pending->wmoModels.size()) {
                return false; // More WMO instances to create - yield
            }
            LOG_DEBUG("  Loaded WMOs for tile [", x, ",", y, "]: ", ft.wmoInstanceIds.size(), " instances");
        }
        ft.phase = FinalizationPhase::WMO_DOODADS;
        return false;
    }

    case FinalizationPhase::WMO_DOODADS: {
        // Upload multiple WMO doodad M2s per call (batched GPU uploads)
        if (m2Renderer && ft.wmoDoodadIndex < pending->wmoDoodads.size()) {
            // Set pre-decoded BLP cache for doodad M2 textures
            m2Renderer->setPredecodedBLPCache(&pending->preloadedM2Textures);
            constexpr size_t kDoodadsPerStep = 4;
            size_t uploaded = 0;
            while (ft.wmoDoodadIndex < pending->wmoDoodads.size() && uploaded < kDoodadsPerStep) {
                auto& doodad = pending->wmoDoodads[ft.wmoDoodadIndex];
                if (!m2Renderer->loadModel(doodad.model, doodad.modelId)) {
                    ft.wmoDoodadIndex++;
                    uploaded++;
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lock(uploadedM2IdsMutex_);
                    uploadedM2Ids_.insert(doodad.modelId);
                }
                uint32_t wmoDoodadInstId = m2Renderer->createInstanceWithMatrix(
                    doodad.modelId, doodad.modelMatrix, doodad.worldPosition);
                if (wmoDoodadInstId) {
                    // WMO doodads should not add duplicate/over-aggressive wall
                    // blocking, but structural doodads such as Exodarplatform01
                    // carry the only authored floor for their walkable ramps.
                    m2Renderer->setSkipWallCollision(wmoDoodadInstId, true);
                    ft.m2InstanceIds.push_back(wmoDoodadInstId);
                }
                ft.wmoDoodadIndex++;
                uploaded++;
            }
            m2Renderer->setPredecodedBLPCache(nullptr);
            if (ft.wmoDoodadIndex < pending->wmoDoodads.size()) return false;
        }
        ft.phase = FinalizationPhase::WATER;
        return false;
    }

    case FinalizationPhase::WATER: {
        // Terrain water was already loaded in TERRAIN phase.
        // Generate water ambient emitters here.
        if (ambientSoundManager) {
            for (size_t chunkIdx = 0; chunkIdx < pending->terrain.waterData.size(); chunkIdx++) {
                const auto& chunkWater = pending->terrain.waterData[chunkIdx];
                if (!chunkWater.hasWater()) continue;

                int chunkX = chunkIdx % 16;
                int chunkY = chunkIdx / 16;
                float tileOriginX = (32.0f - x) * core::coords::TILE_SIZE;
                float tileOriginY = (32.0f - y) * core::coords::TILE_SIZE;
                float chunkCenterX = tileOriginX + (chunkX + 0.5f) * 33.333333f;
                float chunkCenterY = tileOriginY + (chunkY + 0.5f) * 33.333333f;

                if (!chunkWater.layers.empty()) {
                    const auto& layer = chunkWater.layers[0];
                    float waterHeight = layer.minHeight;
                    if (layer.liquidType == 0 && chunkIdx % 32 == 0) {
                        PendingTile::AmbientEmitter emitter;
                        emitter.position = glm::vec3(chunkCenterX, chunkCenterY, waterHeight);
                        emitter.type = 4;
                        pending->ambientEmitters.push_back(emitter);
                    } else if (layer.liquidType == 1 && chunkIdx % 64 == 0) {
                        PendingTile::AmbientEmitter emitter;
                        emitter.position = glm::vec3(chunkCenterX, chunkCenterY, waterHeight);
                        emitter.type = 4;
                        pending->ambientEmitters.push_back(emitter);
                    }
                }
            }
        }

        ft.phase = FinalizationPhase::AMBIENT;
        return false;
    }

    case FinalizationPhase::AMBIENT: {
        // Register ambient sound emitters
        if (ambientSoundManager && !pending->ambientEmitters.empty()) {
            for (const auto& emitter : pending->ambientEmitters) {
                auto type = static_cast<audio::AmbientSoundManager::AmbientType>(emitter.type);
                ambientSoundManager->addEmitter(emitter.position, type);
            }
        }

        // Commit tile to loadedTiles
        auto tile = std::make_unique<TerrainTile>();
        tile->coord = coord;
        tile->terrain = std::move(pending->terrain);
        tile->mesh = std::move(pending->mesh);
        tile->loaded = true;
        tile->m2InstanceIds = std::move(ft.m2InstanceIds);
        tile->wmoInstanceIds = std::move(ft.wmoInstanceIds);
        tile->wmoUniqueIds = std::move(ft.tileWmoUniqueIds);
        tile->doodadUniqueIds = std::move(ft.tileUniqueIds);
        tile->clutterPlacements = std::move(pending->clutterPlacements);
        tile->clutterChunkStart = pending->clutterChunkStart;
        tile->clutterChunkBounds = pending->clutterChunkBounds;
        tile->clutterGeneration = pending->clutterGeneration;
        getTileBounds(coord, tile->minX, tile->minY, tile->maxX, tile->maxY);
        loadedTiles[coord] = std::move(tile);
        // NOTE: Don't cache pending here - std::move above empties terrain/mesh,
        // so the cached tile would have 0 valid chunks on reuse.  Tiles are
        // re-parsed from ADT files (file-cache hit) when they re-enter range.

        // Now safe to remove from pendingTiles (tile is in loadedTiles)
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            pendingTiles.erase(coord);
        }

        LOG_DEBUG("  Finalized tile [", x, ",", y, "]");

        ft.phase = FinalizationPhase::DONE;
        return true;
    }

    case FinalizationPhase::DONE:
        return true;
    }
    return true;
}

void TerrainManager::workerLoop() {
    // Leave placement to the OS scheduler. Artificially reserving CPU 0 made
    // this worker policy depend on the old main-thread pin and reduced the
    // scheduler's ability to balance streaming with render workers.
    LOG_INFO("Terrain worker thread started");

    while (workerRunning.load()) {
        TileCoord coord;
        bool hasWork = false;

        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCV.wait(lock, [this]() {
                return !loadQueue.empty() || !workerRunning.load();
            });

            if (!workerRunning.load()) {
                break;
            }

            // --- Memory-aware throttling ---
            // Back-pressure: if the ready queue is deep (finalization can't
            // keep up), or the system is running low on RAM, sleep instead
            // of pulling more tiles.  Each prepared tile can hold hundreds
            // of MB of decoded textures; limiting concurrency here prevents
            // WoWee from consuming all system memory during world load.
            //
            // Low memory narrows loading to one tile at a time; it never stops
            // it. Waiting only helps while a tile is in flight whose
            // finalization will free something. With none, nothing this client
            // does will raise the number, and a phone - where Android keeps
            // little memory free by design - sat at under 15% with every
            // worker asleep and no terrain at all.
            const auto& memMon = core::MemoryMonitor::getInstance();
            const bool severe = memMon.isSevereMemoryPressure();
            const bool pressure = severe || memMon.isMemoryPressure();
            const bool inFlight = preparingTiles_ > 0 || !readyQueue.empty();
            if (pressure && inFlight) {
                if (!memoryWaitReported_) {
                    memoryWaitReported_ = true;
                    LOG_WARNING("Terrain streaming slowed to one tile at a time: ",
                                memMon.getAvailableRAM() / (1024 * 1024), " MB of ",
                                memMon.getTotalRAM() / (1024 * 1024), " MB available");
                }
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(severe ? 200 : 50));
                continue;
            }
            if (!pressure) memoryWaitReported_ = false;
            if (readyQueue.size() >= maxReadyQueueSize_) {
                // Finalization is behind - sleep briefly to let the main
                // thread catch up.
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            if (!loadQueue.empty()) {
                coord = loadQueue.front();
                loadQueue.pop_front();
                hasWork = true;
                ++preparingTiles_;
            }
        }

        if (hasWork) {
            auto pending = prepareTile(coord.x, coord.y);

            std::lock_guard<std::mutex> lock(queueMutex);
            --preparingTiles_;
            if (pending) {
                readyQueue.push(pending);
            } else {
                // Mark as failed so we don't re-enqueue
                // We'll set failedTiles on the main thread in processReadyTiles
                // For now, just remove from pending tracking
                pendingTiles.erase(coord);
            }
        }
    }

    LOG_INFO("Terrain worker thread stopped");
}

void TerrainManager::processReadyTiles() {
    ZoneScopedN("TerrainManager::processReadyTiles");
    // Move newly ready tiles into the finalizing deque.
    // Keep them in pendingTiles so streamTiles() won't re-enqueue them.
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        while (!readyQueue.empty()) {
            auto pending = readyQueue.front();
            readyQueue.pop();
            if (pending) {
                FinalizingTile ft;
                ft.pending = std::move(pending);
                finalizingTiles_.push_back(std::move(ft));
            }
        }
    }

    VkContext* vkCtx = terrainRenderer ? terrainRenderer->getVkContext() : nullptr;

    // Reclaim completed async uploads from previous frames (non-blocking)
    if (vkCtx) vkCtx->pollUploadBatches();

    // Nothing to finalize - done.
    if (finalizingTiles_.empty()) return;

    // Async upload batch: record GPU copies into a command buffer, submit with
    // a fence, but DON'T wait.  The fence is polled on subsequent frames.
    // This eliminates the main-thread stall from vkWaitForFences entirely.
    //
    // Time-budgeted: yield after 8ms to prevent main-loop stalls. Each
    // advanceFinalization step is designed to be small, but texture uploads
    // and M2 model loads can occasionally spike. The budget ensures we
    // spread heavy tiles across multiple frames instead of blocking.
    const auto budgetStart = std::chrono::steady_clock::now();
    const float budgetMs = taxiStreamingMode_ ? 16.0f : 8.0f;

    if (vkCtx) vkCtx->beginUploadBatch();

    // The budget is checked between steps, so it bounds how many run - not how
    // long one takes. Most phases handle a single model per call, but TERRAIN
    // uploads a whole tile's chunks and textures, and the INSTANCES phases build
    // every instance at once, so one step can overrun the whole budget on its
    // own. Name the phase when it does, rather than leaving one opaque number.
    auto phaseName = [](FinalizationPhase p) {
        switch (p) {
            case FinalizationPhase::TERRAIN:       return "TERRAIN";
            case FinalizationPhase::M2_MODELS:     return "M2_MODELS";
            case FinalizationPhase::M2_INSTANCES:  return "M2_INSTANCES";
            case FinalizationPhase::WMO_MODELS:    return "WMO_MODELS";
            case FinalizationPhase::WMO_INSTANCES: return "WMO_INSTANCES";
            case FinalizationPhase::WMO_DOODADS:   return "WMO_DOODADS";
            case FinalizationPhase::WATER:         return "WATER";
            case FinalizationPhase::AMBIENT:       return "AMBIENT";
            case FinalizationPhase::DONE:          return "DONE";
        }
        return "?";
    };

    while (!finalizingTiles_.empty()) {
        auto& ft = finalizingTiles_.front();
        const auto phaseBefore = ft.phase;
        const auto stepStart = std::chrono::steady_clock::now();
        bool done = advanceFinalization(ft);
        const float stepMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - stepStart).count();
        // Reported the first time a phase overruns, and after that only when
        // it overruns worse than it ever has. Every tile streamed in runs
        // every phase, so a phase that is over budget is over budget for as
        // long as the player keeps walking - seventeen lines in a minute here,
        // saying the same thing about the same phase. What is worth a line is
        // a new worst case.
        if (stepMs > budgetMs) {
            static std::unordered_map<int, float> worstByPhase;
            const int phaseKey = static_cast<int>(phaseBefore);
            auto it = worstByPhase.find(phaseKey);
            if (it == worstByPhase.end() || stepMs > it->second) {
                worstByPhase[phaseKey] = stepMs;
                LOG_WARNING("Terrain finalize step overran: ", phaseName(phaseBefore),
                            " took ", stepMs, "ms (budget ", budgetMs, "ms) tile=[",
                            ft.pending ? ft.pending->coord.x : -1, ",",
                            ft.pending ? ft.pending->coord.y : -1,
                            "] - said again only if it gets worse");
            }
        }
        if (done) {
            finalizingTiles_.pop_front();
        }
        float elapsed = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - budgetStart).count();
        if (elapsed >= budgetMs) break;
    }

    if (vkCtx) vkCtx->endUploadBatch();  // Async - submits but doesn't wait
}

void TerrainManager::processPendingUnloads() {
    ZoneScopedN("TerrainManager::processPendingUnloads");
    if (pendingUnloadQueue_.empty()) return;

    // Time-budgeted rather than count-capped (see pendingUnloadQueue_'s comment) so
    // throughput scales with whatever time is actually available this frame instead
    // of a fixed tile count that can't keep pace when frame rate drops or the queue
    // is unusually large (e.g. after a long, fast taxi flight). Taxi mode gets a
    // larger budget, matching processReadyTiles()'s existing taxi-aware budget.
    const auto budgetStart = std::chrono::steady_clock::now();
    const float budgetMs = taxiStreamingMode_ ? 16.0f : 8.0f;

    size_t unloaded = 0;
    while (!pendingUnloadQueue_.empty()) {
        TileCoord coord = pendingUnloadQueue_.front();
        pendingUnloadQueue_.pop_front();

        // Skip stale entries: the player may have reversed course since this
        // tile was queued, bringing it back within range. Unloading it now
        // would pop visible terrain out from under them.
        int dx = coord.x - currentTile.x;
        int dy = coord.y - currentTile.y;
        if (dx*dx + dy*dy <= unloadRadius*unloadRadius) continue;

        unloadTile(coord.x, coord.y);
        unloaded++;

        const float elapsed = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - budgetStart).count();
        if (elapsed >= budgetMs) break;
    }

    if (unloaded > 0) {
        LOG_DEBUG("Unloaded ", unloaded, " distant tiles (", pendingUnloadQueue_.size(),
                 " queued), ", loadedTiles.size(), " remain (models kept in VRAM)");
    }
}

void TerrainManager::processOneReadyTile() {
    // Move ready tiles into finalizing deque
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        while (!readyQueue.empty()) {
            auto pending = readyQueue.front();
            readyQueue.pop();
            if (pending) {
                FinalizingTile ft;
                ft.pending = std::move(pending);
                finalizingTiles_.push_back(std::move(ft));
            }
        }
    }
    // Finalize ONE tile completely, then return so caller can update the screen
    if (!finalizingTiles_.empty()) {
        VkContext* vkCtx = terrainRenderer ? terrainRenderer->getVkContext() : nullptr;
        if (vkCtx) vkCtx->beginUploadBatch();

        auto& ft = finalizingTiles_.front();
        while (!advanceFinalization(ft)) {}
        finalizingTiles_.pop_front();

        if (vkCtx) vkCtx->endUploadBatchSync();  // Sync - load screen needs data ready
    }
}

std::shared_ptr<PendingTile> TerrainManager::getCachedTile(const TileCoord& coord) {
    std::lock_guard<std::mutex> lock(tileCacheMutex_);
    auto it = tileCache_.find(coord);
    if (it == tileCache_.end()) return nullptr;
    tileCacheLru_.erase(it->second.lruIt);
    tileCacheLru_.push_front(coord);
    it->second.lruIt = tileCacheLru_.begin();
    return it->second.tile;
}
void TerrainManager::streamGroundClutter(const glm::vec3& cameraPos) {
    if (!m2Renderer) return;
    // Clutter draws out to the clutter distance setting and no further, so
    // a chunk further away than that holds instances nothing can see. Chunks
    // that come inside the range get their instances, close ones first; chunks
    // that leave it, with slack so a player pacing a boundary does not churn,
    // are retired and given back in batches.
    //
    // Per chunk, not per tile. A tile is 533 yards across, so instancing every
    // tile that came within range instanced up to four tiles' worth of grass
    // round a corner for a disc 280 yards wide - several times what any frame
    // could draw, and every one of them walked by the cull upload, the
    // animation pass and the shadow caster scan each frame.
    //
    // Retired chunks are batched because removing instances rebuilds the
    // renderer's whole spatial index - every doodad in the world, not just the
    // grass - and doing that for each chunk crossing the edge was a hitch every
    // few yards. Out of range they cost a distance test and nothing else.
    const float clutterDistance = m2Renderer->groundDetailDistance();
    const float drawDistance = clutterDistance > 0.0f ? clutterDistance : 140.0f;
    const float nearEdge = drawDistance + 8.0f;
    const float farEdge = drawDistance + 48.0f;
    constexpr size_t kInstancesPerFrame = 256;
    constexpr size_t kRetiredBatch = 3072;
    size_t created = 0;
    auto tileDistance = [&cameraPos](const TerrainTile& t) {
        const float dx = std::max({t.minX - cameraPos.x, 0.0f, cameraPos.x - t.maxX});
        const float dy = std::max({t.minY - cameraPos.y, 0.0f, cameraPos.y - t.maxY});
        return std::sqrt(dx * dx + dy * dy);
    };

    // The density changed since some tiles were placed. Once it has held
    // still for a moment, re-place the nearest stale tile in clutter range -
    // one a frame, as placing costs a few milliseconds. Stale tiles further
    // out hold no instances and are re-placed when they come into range.
    const uint32_t generation = clutterDensityGeneration_.load();
    const auto now = std::chrono::steady_clock::now();

    // Settled: the last sweep made nothing and re-placed nothing, and since
    // then the camera has stayed within a couple of yards, no tile has come
    // or gone, and the density has not changed. Every answer below would be
    // the same as last time, so it is not asked - two passes over every
    // loaded tile and 256 chunks of each in range, every frame, for nothing.
    // Two yards against the eight of slack past the draw distance that
    // creation already allows, so nothing appears later than it would have
    // anywhere it can be seen. A full sweep still runs twice a second.
    {
        const glm::vec2 moved(cameraPos.x - clutterSettledAt_.x, cameraPos.y - clutterSettledAt_.y);
        if (clutterSettled_ && generation == clutterSettledGeneration_ &&
            loadedTiles.size() == clutterSettledTiles_ &&
            glm::dot(moved, moved) < 4.0f &&
            now - clutterSettledSince_ < std::chrono::milliseconds(500)) {
            return;
        }
    }
    bool regenerated = false;

    const bool densitySettled = now - clutterDensityChangedAt_ > std::chrono::milliseconds(300);
    if (densitySettled && !taxiStreamingMode_) {
        TerrainTile* stalest = nullptr;
        float nearest = farEdge;
        for (auto& [coord, tile] : loadedTiles) {
            (void)coord;
            if (!tile || tile->clutterGeneration == generation) continue;
            const float distance = tileDistance(*tile);
            if (distance <= nearest) {
                nearest = distance;
                stalest = tile.get();
            }
        }
        if (stalest) {
            regenerateTileClutter(*stalest);
            regenerated = true;
        }
    }

    auto chunkDistance = [&cameraPos](const glm::vec4& b) {
        const float dx = std::max({b.x - cameraPos.x, 0.0f, cameraPos.x - b.z});
        const float dy = std::max({b.y - cameraPos.y, 0.0f, cameraPos.y - b.w});
        return std::sqrt(dx * dx + dy * dy);
    };

    // Two sweeps. The first only makes what is close, so arriving somewhere -
    // a login, a teleport, a flight path's end - fills in round the player
    // before the far edge; the second makes the rest and settles retirement.
    size_t retired = 0;
    for (int sweep = 0; sweep < 2; ++sweep) {
        const bool last = (sweep == 1);
        const float createEdge = last ? nearEdge : std::min(nearEdge, 40.0f);
        for (auto& [coord, tile] : loadedTiles) {
            (void)coord;
            if (!tile || tile->clutterPlacements.empty()) continue;
            const float tileDist = tileDistance(*tile);
            if (tileDist > farEdge && tile->clutterLive == 0) continue;
            if (!last && tileDist > createEdge) continue;
            for (size_t c = 0; c < 256; ++c) {
                const uint32_t begin = tile->clutterChunkStart[c];
                const uint32_t end = std::min<uint32_t>(
                    tile->clutterChunkStart[c + 1],
                    static_cast<uint32_t>(tile->clutterPlacements.size()));
                if (begin >= end) continue;
                auto& ids = tile->clutterChunkIds[c];
                uint32_t& next = tile->clutterChunkNext[c];
                const float distance = chunkDistance(tile->clutterChunkBounds[c]);
                if (last) {
                    if (distance > farEdge) {
                        if (!ids.empty() || next > 0) {
                            tile->clutterChunkRetired[c] = 1;
                            retired += ids.size();
                        }
                        continue;
                    }
                    // Back in range before its batch went: keep what it has.
                    tile->clutterChunkRetired[c] = 0;
                }
                if (distance > createEdge || created >= kInstancesPerFrame) continue;
                if (next < begin) next = begin;
                while (next < end && created < kInstancesPerFrame) {
                    const auto& p = tile->clutterPlacements[next++];
                    if (!m2Renderer->hasModel(p.modelId)) continue;
                    if (const uint32_t id = m2Renderer->createInstance(
                            p.modelId, p.position, glm::vec3(0.0f, 0.0f, p.yaw), p.scale)) {
                        m2Renderer->setInstanceGroundNormal(id, p.groundNormal);
                        ids.push_back(id);
                        ++tile->clutterLive;
                        ++created;
                    }
                }
            }
        }
    }

    if (retired >= kRetiredBatch) {
        std::vector<uint32_t> toRemove;
        toRemove.reserve(retired);
        for (auto& [coord, tile] : loadedTiles) {
            (void)coord;
            if (!tile || tile->clutterLive == 0) continue;
            for (size_t c = 0; c < 256; ++c) {
                if (!tile->clutterChunkRetired[c]) continue;
                auto& ids = tile->clutterChunkIds[c];
                toRemove.insert(toRemove.end(), ids.begin(), ids.end());
                tile->clutterLive -= std::min(tile->clutterLive, ids.size());
                ids.clear();
                tile->clutterChunkNext[c] = 0;
                tile->clutterChunkRetired[c] = 0;
            }
        }
        if (!toRemove.empty()) m2Renderer->removeInstances(toRemove);
    }

    // Nothing made and nothing re-placed: settled until something changes.
    // A sweep that removed a batch is not settled - removal ran this frame,
    // and the next sweep is what confirms nothing else is due.
    clutterSettled_ = created == 0 && !regenerated && densitySettled &&
                      retired < kRetiredBatch;
    clutterSettledAt_ = cameraPos;
    clutterSettledTiles_ = loadedTiles.size();
    clutterSettledGeneration_ = generation;
    clutterSettledSince_ = now;
}

void TerrainManager::setGroundClutterDensityScale(float scale) {
    const float clamped = glm::clamp(scale, 0.0f, 1.5f);
    if (std::abs(clamped - groundClutterDensityScale_) < 1e-4f) return;
    groundClutterDensityScale_ = clamped;
    clutterDensityGeneration_.fetch_add(1);
    clutterDensityChangedAt_ = std::chrono::steady_clock::now();
}

void TerrainManager::regenerateTileClutter(TerrainTile& tile) {
    const uint32_t generation = clutterDensityGeneration_.load();
    ensureGroundEffectTablesLoaded();

    // The placement code reads the tile's terrain through a PendingTile, as
    // the workers hand it one. Lent and returned rather than copied: a tile's
    // chunks and alpha maps are megabytes, and nothing else touches the
    // loaded tile while this runs on the main thread.
    auto pending = std::make_shared<PendingTile>();
    pending->coord = tile.coord;
    pending->terrain = std::move(tile.terrain);
    // The models already on the GPU need not be read again; only one the new
    // density reaches for the first time is loaded below.
    std::unordered_set<uint32_t> preparedModelIds;
    {
        std::lock_guard<std::mutex> lock(uploadedM2IdsMutex_);
        preparedModelIds = uploadedM2Ids_;
    }
    generateGroundClutterPlacements(pending, preparedModelIds);
    tile.terrain = std::move(pending->terrain);

    if (m2Renderer) {
        for (auto& ready : pending->m2Models) {
            if (m2Renderer->hasModel(ready.modelId)) continue;
            if (m2Renderer->loadModel(ready.model, ready.modelId)) {
                std::lock_guard<std::mutex> lock(uploadedM2IdsMutex_);
                uploadedM2Ids_.insert(ready.modelId);
            }
        }
    }
    std::vector<uint32_t> oldIds;
    tile.takeClutterInstances(oldIds);
    if (m2Renderer && !oldIds.empty()) m2Renderer->removeInstances(oldIds);
    tile.clutterPlacements = std::move(pending->clutterPlacements);
    tile.clutterChunkStart = pending->clutterChunkStart;
    tile.clutterChunkBounds = pending->clutterChunkBounds;
    tile.clutterGeneration = generation;
}

void TerrainManager::unloadTile(int x, int y) {
    TileCoord coord = {.x = x, .y = y};

    // Also remove from pending if it was queued but not yet loaded
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        pendingTiles.erase(coord);
    }

    // Remove from finalizingTiles_ if it's being incrementally finalized.
    // Water may have already been loaded in TERRAIN phase, so clean it up.
    for (auto fit = finalizingTiles_.begin(); fit != finalizingTiles_.end(); ++fit) {
        if (fit->pending && fit->pending->coord == coord) {
            // If terrain chunks were already uploaded, free their descriptor sets
            if (fit->terrainMeshDone && terrainRenderer) {
                terrainRenderer->removeTile(x, y);
            }
            // If past TERRAIN phase, water was already loaded - remove it
            if (fit->phase != FinalizationPhase::TERRAIN && waterRenderer) {
                waterRenderer->removeTile(x, y);
            }
            // Clean up any M2/WMO instances that were already created
            if (m2Renderer && !fit->m2InstanceIds.empty()) {
                m2Renderer->removeInstances(fit->m2InstanceIds);
            }
            if (wmoRenderer && !fit->wmoInstanceIds.empty()) {
                for (uint32_t id : fit->wmoInstanceIds) {
                    if (waterRenderer) waterRenderer->removeWMO(id);
                }
                wmoRenderer->removeInstances(fit->wmoInstanceIds);
            }
            for (uint32_t uid : fit->tileUniqueIds) placedDoodadIds.erase(uid);
            for (uint32_t uid : fit->tileWmoUniqueIds) {
                placedWmoIds.erase(uid);
                std::lock_guard<std::mutex> lock(preparedWmoUniqueIdsMutex_);
                preparedWmoUniqueIds_.erase(uid);
            }
            finalizingTiles_.erase(fit);
            return;
        }
    }

    auto it = loadedTiles.find(coord);
    if (it == loadedTiles.end()) {
        return;
    }

    LOG_INFO("Unloading terrain tile [", x, ",", y, "]");

    const auto& tile = it->second;

    // Remove doodad unique IDs from dedup set
    for (uint32_t uid : tile->doodadUniqueIds) {
        placedDoodadIds.erase(uid);
    }
    for (uint32_t uid : tile->wmoUniqueIds) {
        placedWmoIds.erase(uid);
        std::lock_guard<std::mutex> lock(preparedWmoUniqueIdsMutex_);
        preparedWmoUniqueIds_.erase(uid);
    }

    // Remove M2 doodad instances
    if (m2Renderer) {
        // One removal for the doodads and the clutter together: each call
        // rebuilds the renderer's spatial index over every instance.
        std::vector<uint32_t> ids = tile->m2InstanceIds;
        tile->takeClutterInstances(ids);
        m2Renderer->removeInstances(ids);
        LOG_DEBUG("  Removed ", tile->m2InstanceIds.size(), " M2 instances");
    }

    // Remove WMO instances and their liquids
    if (wmoRenderer) {
        for (uint32_t id : tile->wmoInstanceIds) {
            // Remove WMO liquids associated with this instance
            if (waterRenderer) {
                waterRenderer->removeWMO(id);
            }
        }
        wmoRenderer->removeInstances(tile->wmoInstanceIds);
        LOG_DEBUG("  Removed ", tile->wmoInstanceIds.size(), " WMO instances");
    }

    // Remove terrain chunks for this tile
    if (terrainRenderer) {
        terrainRenderer->removeTile(x, y);
    }

    // Remove water surfaces for this tile
    if (waterRenderer) {
        waterRenderer->removeTile(x, y);
    }

    loadedTiles.erase(it);
}

void TerrainManager::stopWorkers() {
    if (!workerRunning.load()) {
        LOG_DEBUG("stopWorkers: already stopped");
        return;
    }
    LOG_DEBUG("stopWorkers: signaling ", workerThreads.size(), " workers to stop...");
    workerRunning.store(false);
    queueCV.notify_all();

    // Workers check workerRunning at each I/O point in prepareTile() and bail
    // out quickly.  Use plain join() which is safe with std::thread - no
    // pthread_timedjoin_np (which silently joins the pthread but leaves the
    // std::thread object thinking it's still joinable → std::terminate on dtor).
    for (size_t i = 0; i < workerThreads.size(); i++) {
        if (workerThreads[i].joinable()) {
            LOG_DEBUG("stopWorkers: joining worker ", i, "...");
            workerThreads[i].join();
        }
    }
    workerThreads.clear();
    LOG_DEBUG("stopWorkers: done");
}

void TerrainManager::softReset() {
    // Clear queues (workers may still be running - they'll find empty queues)
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        loadQueue.clear();
        while (!readyQueue.empty()) readyQueue.pop();
    }
    pendingTiles.clear();
    finalizingTiles_.clear();
    placedDoodadIds.clear();
    placedWmoIds.clear();
    {
        std::lock_guard<std::mutex> lock(uploadedM2IdsMutex_);
        uploadedM2Ids_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(preparedWmoUniqueIdsMutex_);
        preparedWmoUniqueIds_.clear();
    }

    // Clear tile cache - keys are (x,y) without map name, so stale entries from
    // a different map with overlapping coordinates would produce wrong geometry.
    {
        std::lock_guard<std::mutex> lock(tileCacheMutex_);
        tileCache_.clear();
        tileCacheLru_.clear();
    }

    LOG_INFO("Soft-resetting terrain (clearing tiles + water + cache, workers stay alive)");
    loadedTiles.clear();
    failedTiles.clear();

    currentTile = {.x = -1, .y = -1};
    lastStreamTile = {.x = -1, .y = -1};

    if (terrainRenderer) {
        terrainRenderer->clear();
    }
    if (waterRenderer) {
        waterRenderer->clear();
    }
}

TileCoord TerrainManager::worldToTile(float glX, float glY) const {
    auto [tileX, tileY] = core::coords::worldToTile(glX, glY);
    return {.x = tileX, .y = tileY};
}

void TerrainManager::getTileBounds(const TileCoord& coord, float& minX, float& minY,
                                    float& maxX, float& maxY) const {
    // The tile's extent in render space, which is not the axis its own index
    // counts along: worldToTile takes the tile's X index from renderY and its
    // Y index from renderX - see core/coordinates.hpp, where the pair is
    // written the same way round. So the render-X edge comes from coord.y.
    //
    // These were named for one axis and computed from the other. Everything
    // that reads them guesses a chunk index and then checks the guess against
    // the chunk's own position, so the fault showed as the guess missing and
    // the search widening - never as a wrong answer, until getAreaIdAt asked
    // the same question without checking.
    const float maxRenderX = (32 - coord.y) * TILE_SIZE;
    const float maxRenderY = (32 - coord.x) * TILE_SIZE;

    minX = maxRenderX - TILE_SIZE;
    minY = maxRenderY - TILE_SIZE;
    maxX = maxRenderX;
    maxY = maxRenderY;
}

std::string TerrainManager::getADTPath(const TileCoord& coord) const {
    // Format: World\Maps\{MapName}\{MapName}_{X}_{Y}.adt
    return "World\\Maps\\" + mapName + "\\" + mapName + "_" +
           std::to_string(coord.x) + "_" + std::to_string(coord.y) + ".adt";
}

void TerrainManager::ensureGroundEffectTablesLoaded() {
    if (groundEffectsLoaded_ || !assetManager) return;
    groundEffectsLoaded_ = true;

    auto groundEffectTex = assetManager->loadDBC("GroundEffectTexture.dbc");
    auto groundEffectDoodad = assetManager->loadDBC("GroundEffectDoodad.dbc");
    if (!groundEffectTex || !groundEffectDoodad) {
        LOG_WARNING("Ground clutter DBCs missing; skipping procedural ground effects");
        return;
    }

    // GroundEffectTexture: id + 4 doodad IDs + 4 weights + density + sound
    for (uint32_t i = 0; i < groundEffectTex->getRecordCount(); ++i) {
        uint32_t effectId = groundEffectTex->getUInt32(i, 0);
        if (effectId == 0) continue;

        GroundEffectEntry e;
        e.doodadIds[0] = groundEffectTex->getUInt32(i, 1);
        e.doodadIds[1] = groundEffectTex->getUInt32(i, 2);
        e.doodadIds[2] = groundEffectTex->getUInt32(i, 3);
        e.doodadIds[3] = groundEffectTex->getUInt32(i, 4);
        e.weights[0] = groundEffectTex->getUInt32(i, 5);
        e.weights[1] = groundEffectTex->getUInt32(i, 6);
        e.weights[2] = groundEffectTex->getUInt32(i, 7);
        e.weights[3] = groundEffectTex->getUInt32(i, 8);
        e.density = groundEffectTex->getUInt32(i, 9);
        if (groundEffectTex->getFieldCount() > 10) e.terrainType = groundEffectTex->getUInt32(i, 10);
        groundEffectById_[effectId] = e;
    }
    // TerrainType: ID, description, two spray effects, SoundID, flags.
    if (auto terrainType = assetManager->loadDBCOptional("TerrainType.dbc");
        terrainType && terrainType->isLoaded() && terrainType->getFieldCount() >= 5) {
        for (uint32_t i = 0; i < terrainType->getRecordCount(); ++i) {
            terrainTypeSoundId_[terrainType->getUInt32(i, 0)] = terrainType->getUInt32(i, 4);
        }
    }

    // GroundEffectDoodad: id + modelName(offset) + flags
    for (uint32_t i = 0; i < groundEffectDoodad->getRecordCount(); ++i) {
        uint32_t doodadId = groundEffectDoodad->getUInt32(i, 0);
        std::string modelName = groundEffectDoodad->getString(i, 1);
        if (doodadId == 0 || modelName.empty()) continue;

        std::string lower = toLowerCopy(modelName);
        if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".mdl") {
            lower = lower.substr(0, lower.size() - 4) + ".m2";
        }
        if (lower.find('\\') != std::string::npos || lower.find('/') != std::string::npos) {
            groundDoodadModelById_[doodadId] = lower;
        } else {
            groundDoodadModelById_[doodadId] = "World\\NoDXT\\Detail\\" + lower;
        }
    }

    LOG_INFO("Ground clutter tables loaded: ", groundEffectById_.size(),
             " effects, ", groundDoodadModelById_.size(), " doodad models");
}

void TerrainManager::generateGroundClutterPlacements(std::shared_ptr<PendingTile>& pending,
                                                     std::unordered_set<uint32_t>& preparedModelIds) {
    if (taxiStreamingMode_) return;  // Skip clutter while on taxi flights.
    if (!pending || groundEffectById_.empty() || groundDoodadModelById_.empty()) return;

    // No stand-in model and no minimum per tile. Both put Elwynn's grass
    // wherever the data asked for something that would not load, or for
    // nothing at all - which is how ElwGra01 came to grow in Hellfire
    // Peninsula. Ground the data leaves bare stays bare, as it does in the
    // stock client.
    size_t modelMissing = 0;
    size_t modelInvalid = 0;
    auto ensureModelPrepared = [&](const std::string& m2Path, uint32_t modelId) -> bool {
        if (preparedModelIds.count(modelId)) return true;

        std::vector<uint8_t> m2Data = assetManager->readFile(m2Path);
        if (m2Data.empty()) {
            modelMissing++;
            return false;
        }

        pipeline::M2Model m2Model = pipeline::M2Loader::load(m2Data);
        m2Model.name = m2Path;
        std::string skinPath = pipeline::skinPathForM2(m2Path);
        std::vector<uint8_t> skinData = assetManager->readFileOptional(skinPath);
        if (!skinData.empty() && m2Model.version >= 264) {
            pipeline::M2Loader::loadSkin(skinData, m2Model);
        }
        if (!m2Model.isValid()) {
            modelInvalid++;
            return false;
        }

        PendingTile::M2Ready ready;
        ready.modelId = modelId;
        ready.model = std::move(m2Model);
        ready.path = m2Path;
        pending->m2Models.push_back(std::move(ready));
        preparedModelIds.insert(modelId);
        return true;
    };

    constexpr float unitSize = CHUNK_SIZE / 8.0f;
    constexpr float pi = core::coords::PI;

    // The stock placement, as the 3.3.5 client does it: each of a chunk's 8x8
    // quads that the map does not mask off (MCNK noEffectDoodad) grows the
    // ground effect of the one layer the map assigns it (MCNK doodadMapping,
    // the low-resolution texture map), and as many pieces as that effect's
    // GroundEffectTexture density times the groundEffectDensity setting says.
    //
    // This used to scatter random points across the whole chunk per layer,
    // test each against the layer's alpha and a list of road texture names,
    // and stop at eight a chunk - an eighth of what Elwynn asks for at the
    // stock default, which read as stubs in the dirt, while the masks that
    // say where the data wants nothing were not read at all.
    //
    // groundEffectDensity: the settings slider is 0-150 percent with 70 the
    // default, the stock setting 16-256 with 64 the default. 70 is mapped to
    // 64 and 150 to 256, linearly either side. At 64, density 8 - most of
    // Azeroth's grass - is one piece a quad, 64 to a fully grown chunk.
    const float densityPercent = glm::clamp(groundClutterDensityScale_, 0.0f, 1.5f) * 100.0f;
    const float groundEffectDensity = densityPercent <= 70.0f
        ? densityPercent / 70.0f * 64.0f
        : 64.0f + (densityPercent - 70.0f) / 80.0f * 192.0f;
    // A ceiling per quad for the few textures whose density is far above the
    // rest (50 to 123 against a usual 4 to 16): at 256 they would be sixty
    // pieces in four yards square.
    constexpr uint32_t kMaxPerQuad = 8;
    std::vector<uint8_t> alphaScratchTex;
    size_t added = 0;
    size_t quadsGrowing = 0;
    size_t noEffectMatch = 0;
    size_t noDoodadModel = 0;

    auto layerWeightAt = [&](const pipeline::MapChunk& chunk, size_t layerIdx, int alphaIndex) -> int {
        if (layerIdx >= chunk.layers.size()) return 0;
        if (layerIdx == 0) {
            int accum = 0;
            size_t numLayers = std::min(chunk.layers.size(), static_cast<size_t>(4));
            for (size_t i = 1; i < numLayers; ++i) {
                int a = 0;
                if (decodeLayerAlpha(chunk, i, alphaScratchTex) &&
                    alphaIndex >= 0 &&
                    alphaIndex < static_cast<int>(alphaScratchTex.size())) {
                    a = alphaScratchTex[alphaIndex];
                }
                accum += a;
            }
            return glm::clamp(255 - accum, 0, 255);
        }
        if (decodeLayerAlpha(chunk, layerIdx, alphaScratchTex) &&
            alphaIndex >= 0 &&
            alphaIndex < static_cast<int>(alphaScratchTex.size())) {
            return alphaScratchTex[alphaIndex];
        }
        return 0;
    };

    // The terrain's own normal at a point, from MCNR - the normals the terrain
    // shader lights the ground with - so a piece is lit as the ground under it
    // is rather than by its own card's normal. The quad's four outer vertices
    // bilinearly, half and half with its centre vertex, which is near enough
    // the four-wedge fan the mesh draws for a value only used for shading.
    auto groundNormalAt = [](const pipeline::MapChunk& chunk, float fracX, float fracY) -> glm::vec3 {
        auto vertexNormal = [&chunk](int index) -> glm::vec3 {
            const size_t i = static_cast<size_t>(index) * 3;
            if (index < 0 || i + 2 >= chunk.normals.size()) return glm::vec3(0.0f, 0.0f, 1.0f);
            return glm::vec3(chunk.normals[i], chunk.normals[i + 1], chunk.normals[i + 2]) / 127.0f;
        };
        const int qx = glm::clamp(static_cast<int>(std::floor(fracX)), 0, 7);
        const int qy = glm::clamp(static_cast<int>(std::floor(fracY)), 0, 7);
        const float u = glm::clamp(fracX - static_cast<float>(qx), 0.0f, 1.0f);
        const float v = glm::clamp(fracY - static_cast<float>(qy), 0.0f, 1.0f);
        const glm::vec3 corners =
            vertexNormal(qy * 17 + qx)           * (1.0f - u) * (1.0f - v) +
            vertexNormal(qy * 17 + qx + 1)       * u * (1.0f - v) +
            vertexNormal((qy + 1) * 17 + qx)     * (1.0f - u) * v +
            vertexNormal((qy + 1) * 17 + qx + 1) * u * v;
        glm::vec3 n = 0.5f * corners + 0.5f * vertexNormal(9 + qy * 17 + qx);
        const float len = glm::length(n);
        if (len < 1e-4f || !std::isfinite(len)) return glm::vec3(0.0f, 0.0f, 1.0f);
        n /= len;
        return n.z < 0.0f ? -n : n;
    };

    // The model each GroundEffectDoodad id resolves to, loaded once per tile;
    // 0 for an id with no row or a model that will not load.
    std::unordered_map<uint32_t, uint32_t> modelIdForDoodad;
    auto modelForDoodad = [&](uint32_t doodadId) -> uint32_t {
        if (doodadId == 0) return 0;
        auto cached = modelIdForDoodad.find(doodadId);
        if (cached != modelIdForDoodad.end()) return cached->second;
        uint32_t modelId = 0;
        auto doodadIt = groundDoodadModelById_.find(doodadId);
        if (doodadIt == groundDoodadModelById_.end()) {
            noDoodadModel++;
        } else {
            const uint32_t id = static_cast<uint32_t>(std::hash<std::string>{}(doodadIt->second));
            if (ensureModelPrepared(doodadIt->second, id)) modelId = id;
        }
        modelIdForDoodad.emplace(doodadId, modelId);
        return modelId;
    };

    for (int cy = 0; cy < 16; ++cy) {
        for (int cx = 0; cx < 16; ++cx) {
            const size_t chunkIndex = static_cast<size_t>(cy * 16 + cx);
            const size_t chunkBegin = pending->clutterPlacements.size();
            pending->clutterChunkStart[chunkIndex] = static_cast<uint32_t>(chunkBegin);
            const auto& chunk = pending->terrain.getChunk(cx, cy);
            if (!chunk.hasHeightMap() || chunk.layers.empty()) continue;

            // A chunk whose mapping is all zero either is all its first layer
            // or comes from data that never filled the mapping in. The quad's
            // heaviest layer answers both the same way.
            bool mappingEmpty = true;
            for (uint8_t b : chunk.doodadMapping) {
                if (b != 0) { mappingEmpty = false; break; }
            }
            const size_t layerCount = std::min<size_t>(chunk.layers.size(), 4);

            uint32_t seed = static_cast<uint32_t>(
                ((pending->coord.x & 0xFF) << 24) ^
                ((pending->coord.y & 0xFF) << 16) ^
                ((cx & 0x1F) << 8) ^
                ((cy & 0x1F) << 3)) * 2654435761u + 0x9E3779B9u;
            auto nextRand = [&seed]() -> uint32_t {
                seed = seed * 1664525u + 1013904223u;
                return seed;
            };
            auto rand01 = [&nextRand]() -> float {
                return static_cast<float>((nextRand() >> 8) & 0xFFFFu) / kRand16Max;
            };

            for (int qy = 0; qy < 8; ++qy) {
                for (int qx = 0; qx < 8; ++qx) {
                    if (chunk.isHole(qy, qx) || chunk.isEffectDisabled(qy, qx)) continue;

                    size_t layerIdx = chunk.effectLayerFor(qy, qx);
                    if (mappingEmpty && layerCount > 1) {
                        const int alphaIndex = static_cast<int>(pipeline::alphaTexelIndex(
                            (static_cast<float>(qx) + 0.5f) / 8.0f, (static_cast<float>(qy) + 0.5f) / 8.0f));
                        int best = -1;
                        for (size_t li = 0; li < layerCount; ++li) {
                            const int w = layerWeightAt(chunk, li, alphaIndex);
                            if (w > best) { best = w; layerIdx = li; }
                        }
                    }
                    if (layerIdx >= layerCount) continue;
                    const uint32_t effectId = chunk.layers[layerIdx].effectId;
                    if (effectId == 0) continue;
                    auto geIt = groundEffectById_.find(effectId);
                    if (geIt == groundEffectById_.end()) {
                        noEffectMatch++;
                        continue;
                    }
                    const GroundEffectEntry& ge = geIt->second;

                    const float expected =
                        static_cast<float>(ge.density) * groundEffectDensity / 512.0f;
                    uint32_t count = static_cast<uint32_t>(expected);
                    if (rand01() < expected - static_cast<float>(count)) ++count;
                    count = std::min(count, kMaxPerQuad);
                    if (count == 0) continue;

                    // The four doodads and their weights, over those that load.
                    std::array<uint32_t, 4> models{};
                    std::array<uint32_t, 4> weights{};
                    uint32_t totalWeight = 0;
                    uint32_t usable = 0;
                    for (int i = 0; i < 4; ++i) {
                        models[i] = modelForDoodad(ge.doodadIds[i]);
                        if (models[i] == 0) continue;
                        weights[i] = ge.weights[i];
                        totalWeight += weights[i];
                        ++usable;
                    }
                    if (usable == 0) continue;
                    if (totalWeight == 0) {
                        for (int i = 0; i < 4; ++i) weights[i] = models[i] ? 1u : 0u;
                        totalWeight = usable;
                    }
                    ++quadsGrowing;

                    for (uint32_t n = 0; n < count; ++n) {
                        const float fracX = static_cast<float>(qx) + rand01();
                        const float fracY = static_cast<float>(qy) + rand01();
                        const uint32_t roll = nextRand() % totalWeight;
                        int pick = 0;
                        uint32_t acc = 0;
                        for (int i = 0; i < 4; ++i) {
                            acc += weights[i];
                            if (roll < acc) { pick = i; break; }
                        }
                        if (models[pick] == 0) continue;

                        // On the drawn surface, eased down where the ground
                        // falls away: the lowest of four points a foot out
                        // takes half the drop, so a piece on a slope keeps its
                        // downhill side on the ground instead of standing on
                        // its uphill edge with the rest in the air.
                        const glm::vec3 surfacePoint = pipeline::TerrainMeshGenerator::chunkSurfacePoint(
                            chunk.position, chunk.heightMap, fracX, fracY, unitSize);
                        constexpr float kFootprint = 0.3f / unitSize;
                        float lowest = surfacePoint.z;
                        const float offsets[4][2] = {{kFootprint, 0.0f}, {-kFootprint, 0.0f},
                                                     {0.0f, kFootprint}, {0.0f, -kFootprint}};
                        for (const auto& o : offsets) {
                            const float sx = glm::clamp(fracX + o[0], 0.0f, 8.0f);
                            const float sy = glm::clamp(fracY + o[1], 0.0f, 8.0f);
                            lowest = std::min(lowest, pipeline::TerrainMeshGenerator::chunkSurfacePoint(
                                chunk.position, chunk.heightMap, sx, sy, unitSize).z);
                        }

                        ClutterPlacement p;
                        p.modelId = models[pick];
                        // MCNK chunk.position is already in terrain/render world space.
                        // Do not convert via ADT placement mapping (that is for MDDF/MODF records).
                        p.yaw = rand01() * (2.0f * pi);
                        p.scale = 0.80f + rand01() * 0.35f;
                        p.position = glm::vec3(surfacePoint.x, surfacePoint.y,
                                               surfacePoint.z - 0.5f * (surfacePoint.z - lowest));
                        p.groundNormal = groundNormalAt(chunk, fracX, fracY);
                        pending->clutterPlacements.push_back(p);
                        added++;
                    }
                }
            }

            // The chunk's extent, which is what the streamer measures to.
            const size_t chunkEnd = pending->clutterPlacements.size();
            if (chunkEnd > chunkBegin) {
                glm::vec4 bounds(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                                 std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());
                for (size_t i = chunkBegin; i < chunkEnd; ++i) {
                    const glm::vec3& pos = pending->clutterPlacements[i].position;
                    bounds.x = std::min(bounds.x, pos.x);
                    bounds.y = std::min(bounds.y, pos.y);
                    bounds.z = std::max(bounds.z, pos.x);
                    bounds.w = std::max(bounds.w, pos.y);
                }
                pending->clutterChunkBounds[chunkIndex] = bounds;
            }
        }
    }
    pending->clutterChunkStart[256] = static_cast<uint32_t>(pending->clutterPlacements.size());

    // At debug either way: a tile with no clutter is ordinary (deserts,
    // snowfields, cities), and the counts say why when it is not.
    static int clutterLogCount = 0;
    if (clutterLogCount < 12) {
        LOG_DEBUG("Ground clutter tile [", pending->coord.x, ",", pending->coord.y,
                  "] added=", added, " quadsGrowing=", quadsGrowing,
                  " groundEffectDensity=", groundEffectDensity,
                  " noEffect=", noEffectMatch,
                  " noDoodadModel=", noDoodadModel,
                  " modelMissing=", modelMissing,
                  " modelInvalid=", modelInvalid);
        clutterLogCount++;
    }
}

TerrainManager::TerrainTextureTones
TerrainManager::getTerrainTextureTones(const std::string& texturePath) {
    const auto it = terrainTextureTones_.find(texturePath);
    if (it != terrainTextureTones_.end()) return it->second;

    TerrainTextureTones tones;  // greys, which tint nothing much either way
    if (assetManager) {
        const pipeline::BLPImage blp = assetManager->loadTexture(texturePath, false);
        if (blp.isValid() && !blp.data.empty()) {
            // Every sixteenth pixel, sorted by luminance. Percentiles rather
            // than a mean because a grass texture is blades over earth, and
            // the mean is the two averaged into neither.
            struct Sample { float luma; uint8_t r, g, b; };
            std::vector<Sample> samples;
            samples.reserve(blp.data.size() / 64 + 1);
            for (size_t i = 0; i + 3 < blp.data.size(); i += 64) {
                const uint8_t r = blp.data[i];
                const uint8_t g = blp.data[i + 1];
                const uint8_t b = blp.data[i + 2];
                samples.push_back({0.299f * static_cast<float>(r) +
                                       0.587f * static_cast<float>(g) +
                                       0.114f * static_cast<float>(b),
                                   r, g, b});
            }
            if (samples.size() >= 8) {
                std::sort(samples.begin(), samples.end(),
                          [](const Sample& a, const Sample& b) { return a.luma < b.luma; });
                auto at = [&](float pct) {
                    const auto idx = static_cast<size_t>(
                        static_cast<float>(samples.size() - 1) * pct);
                    const Sample& sm = samples[idx];
                    return glm::vec3(static_cast<float>(sm.r), static_cast<float>(sm.g),
                                     static_cast<float>(sm.b)) / 255.0f;
                };
                tones.shadow = at(0.25f);
                tones.highlight = at(0.85f);
            }
        }
    }
    terrainTextureTones_[texturePath] = tones;
    return tones;
}

void TerrainManager::getGroundEffectDoodads(uint32_t effectId,
                                            std::vector<std::string>& outModels,
                                            std::vector<uint32_t>& outWeights) const {
    outModels.clear();
    outWeights.clear();
    const auto it = groundEffectById_.find(effectId);
    if (it == groundEffectById_.end()) return;

    for (size_t i = 0; i < it->second.doodadIds.size(); ++i) {
        const uint32_t doodadId = it->second.doodadIds[i];
        if (doodadId == 0) continue;
        const auto model = groundDoodadModelById_.find(doodadId);
        if (model == groundDoodadModelById_.end()) continue;
        outModels.push_back(model->second);
        outWeights.push_back(it->second.weights[i]);
    }
}

uint32_t TerrainManager::getGroundEffectDensity(uint32_t effectId) const {
    if (effectId == 0) return 0;
    const auto it = groundEffectById_.find(effectId);
    return (it == groundEffectById_.end()) ? 0 : it->second.density;
}

const pipeline::MapChunk* TerrainManager::findChunkAt(float glX, float glY,
                                                      float& fracX, float& fracY,
                                                      const TerrainTile** outTile) const {
    // Terrain mesh vertices use chunk.position directly (WoW coordinates) and
    // the terrain is rendered without a model transform, so the camera's own
    // coordinates index it unchanged. A chunk spans
    //   X: [position[0] - 8 * unitSize, position[0]]
    //   Y: [position[1] - 8 * unitSize, position[1]]
    // and the two fractions handed back are the offsets within it, in the same
    // order the mesh builder walks its grid: fracY down the 17-stride rows,
    // fracX across.
    //
    // One finder for everyone who needs "which chunk is under this point".
    // isHoleAt used to carry its own copy and the copy was missing the full
    // scan below, so a chunk the guess did not land on read as no-hole rather
    // than as look-harder - which is every hole whose tile is indexed a little
    // differently from the guess. That is the whole reason the Gadgetzan
    // stairwell still reported hole=0 with 0x1000 sitting in its MCNK header.
    const float unitSize = CHUNK_SIZE / 8.0f;

    auto inChunk = [&](const TerrainTile* tile, int cx, int cy) -> const pipeline::MapChunk* {
        if (!tile || cx < 0 || cx >= 16 || cy < 0 || cy >= 16) return nullptr;
        const auto& chunk = tile->terrain.getChunk(cx, cy);
        if (!chunk.hasHeightMap()) return nullptr;

        if (!pipeline::TerrainMeshGenerator::chunkFractionsAt(chunk.position, glX, glY,
                                                              unitSize, fracX, fracY)) {
            return nullptr;
        }
        return &chunk;
    };

    auto inTile = [&](const TerrainTile* tile) -> const pipeline::MapChunk* {
        if (!tile || !tile->loaded) return nullptr;
        // Recorded per attempt rather than per hit: whichever tile the found
        // chunk came from was necessarily the last one tried.
        if (outTile) *outTile = tile;

        // Fast path: infer the likely chunk index and probe a 3x3 neighbourhood.
        const int guessCy = glm::clamp(
            static_cast<int>(std::floor((tile->maxX - glX) / CHUNK_SIZE)), 0, 15);
        const int guessCx = glm::clamp(
            static_cast<int>(std::floor((tile->maxY - glY) / CHUNK_SIZE)), 0, 15);
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (auto* c = inChunk(tile, guessCx + dx, guessCy + dy)) return c;
            }
        }

        // Fallback full scan for robustness at seams/unusual coords.
        for (int cy = 0; cy < 16; cy++) {
            for (int cx = 0; cx < 16; cx++) {
                if (auto* c = inChunk(tile, cx, cy)) return c;
            }
        }
        return nullptr;
    };

    // Fast path: the expected containing tile first.
    const TileCoord tc = worldToTile(glX, glY);
    auto it = loadedTiles.find(tc);
    if (it != loadedTiles.end()) {
        if (auto* c = inTile(it->second.get())) return c;
    }

    // Fallback: every loaded tile (handles seam/edge coordinate ambiguity).
    for (const auto& [coord, tile] : loadedTiles) {
        if (coord == tc) continue;
        if (auto* c = inTile(tile.get())) return c;
    }
    if (outTile) *outTile = nullptr;
    return nullptr;
}

std::optional<float> TerrainManager::getHeightAt(float glX, float glY) const {
    float fracX = 0.0f, fracY = 0.0f;
    const pipeline::MapChunk* chunk = findChunkAt(glX, glY, fracX, fracY);
    if (!chunk) return std::nullopt;

    // The one sampler, shared with the mesh builder and the clutter scatterer.
    //
    // This was a second bilinear interpolation of the four outer corners, which
    // is not the surface that gets drawn: the mesh fans four triangles from each
    // quad's centre vertex, and MCVT puts that vertex wherever the artist needed
    // it. The two answers differed by whatever the centre was offset by, so the
    // floor came out below the visible ground and the player sank into a slope.
    const glm::vec3 surface = pipeline::TerrainMeshGenerator::chunkSurfacePoint(
        chunk->position, chunk->heightMap, fracX, fracY, CHUNK_SIZE / 8.0f);
    return surface.z;
}

bool TerrainManager::isTileLoadedAt(float glX, float glY) const {
    return loadedTiles.find(worldToTile(glX, glY)) != loadedTiles.end();
}

bool TerrainManager::isHoleAt(float glX, float glY) const {
    float fracX = 0.0f, fracY = 0.0f;
    const pipeline::MapChunk* chunk = findChunkAt(glX, glY, fracX, fracY);
    if (!chunk || chunk->holes == 0) return false;

    // The quad, in the order the mesh builder passes to isHole: it walks
    // `for y { for x { if (chunk.isHole(y, x)) continue; ... } }` over vertices
    // at `y * 17 + x`, which is the grid getHeightAt samples as
    // `heights[gy * 17 + gx]`. Reading the quad from the same two fractions is
    // what keeps the surface stood on and the surface drawn agreeing about
    // which quads are there.
    const int qy = glm::clamp(static_cast<int>(std::floor(fracY)), 0, 7);
    const int qx = glm::clamp(static_cast<int>(std::floor(fracX)), 0, 7);
    return chunk->isHole(qy, qx);
}

bool TerrainManager::chunkHasHoles(float glX, float glY) const {
    // Through the one finder, which was the third copy of "which chunk is
    // under this point" in this file. This one took its index from the tile's
    // corner and read it out of the array without checking the chunk it landed
    // on - the same shape as the area lookup, and wrong in the same way at a
    // tile's edge.
    float fracX = 0.0f, fracY = 0.0f;
    const pipeline::MapChunk* chunk = findChunkAt(glX, glY, fracX, fracY);
    return chunk != nullptr && chunk->holes != 0;
}

std::optional<uint32_t> TerrainManager::getAreaIdAt(float glX, float glY) const {
    // Through the one finder, as the height and the holes go.
    //
    // This had its own copy of "which chunk is under this point": an index
    // worked out from the tile's corner and read straight out of the array,
    // with nothing comparing the chunk it landed on against the point it was
    // asked about. The other two check, and a check is what turns a wrong
    // index into the right chunk - so the ground under the player was read
    // correctly while the area under the same feet came from a chunk some way
    // off, and near a tile's edge that chunk belongs to another zone.
    //
    // Which is what the zone flickering in Duskwood was: the border moved as
    // the player walked, the zone name announced itself, and the dark-zone
    // override - which pins Duskwood's hour - came and went with it.
    float fracX = 0.0f, fracY = 0.0f;
    const pipeline::MapChunk* chunk = findChunkAt(glX, glY, fracX, fracY);
    if (!chunk || chunk->areaId == 0) return std::nullopt;
    return chunk->areaId;
}

const pipeline::TextureLayer* TerrainManager::dominantLayerAt(float glX, float glY,
                                                                 const TerrainTile** outTile) const {
    // Through the one finder, which was the last copy of "which chunk is under
    // this point" in this file: a bounds test against the chunk's own
    // position, the two fractions, a guess and a widening search - findChunkAt
    // to the line, written again inside two lambdas.
    float fracX = 0.0f, fracY = 0.0f;
    const pipeline::MapChunk* chunk = findChunkAt(glX, glY, fracX, fracY, outTile);
    if (!chunk || !*outTile || chunk->layers.empty()) return nullptr;

    const int alphaIndex =
        static_cast<int>(pipeline::alphaTexelIndex(fracX / 8.0f, fracY / 8.0f));

    // What each layer is worth at that texel. The base is whatever the layers
    // above it leave, which is how the renderer's own blend reads them.
    std::vector<uint8_t> alphaScratch;
    int weights[4] = {0, 0, 0, 0};
    const size_t numLayers = std::min(chunk->layers.size(), static_cast<size_t>(4));
    int accum = 0;
    for (size_t layerIdx = 1; layerIdx < numLayers; ++layerIdx) {
        int alpha = 0;
        if (decodeLayerAlpha(*chunk, layerIdx, alphaScratch) &&
            alphaIndex < static_cast<int>(alphaScratch.size())) {
            alpha = alphaScratch[alphaIndex];
        }
        weights[layerIdx] = alpha;
        accum += alpha;
    }
    weights[0] = glm::clamp(255 - accum, 0, 255);

    size_t bestLayer = 0;
    int bestWeight = weights[0];
    for (size_t i = 1; i < numLayers; ++i) {
        if (weights[i] > bestWeight) {
            bestWeight = weights[i];
            bestLayer = i;
        }
    }

    return &chunk->layers[bestLayer];
}

std::optional<std::string> TerrainManager::getDominantTextureAt(float glX, float glY) const {
    const TerrainTile* tile = nullptr;
    const pipeline::TextureLayer* layer = dominantLayerAt(glX, glY, &tile);
    if (!layer) return std::nullopt;
    if (layer->textureId >= tile->terrain.textures.size()) return std::nullopt;
    return tile->terrain.textures[layer->textureId];
}

std::optional<uint32_t> TerrainManager::getTerrainSoundTypeAt(float glX, float glY) const {
    const TerrainTile* tile = nullptr;
    const pipeline::TextureLayer* layer = dominantLayerAt(glX, glY, &tile);
    if (!layer || layer->effectId == 0) return std::nullopt;
    auto effect = groundEffectById_.find(layer->effectId);
    if (effect == groundEffectById_.end()) return std::nullopt;
    auto sound = terrainTypeSoundId_.find(effect->second.terrainType);
    if (sound == terrainTypeSoundId_.end() || sound->second == 0) return std::nullopt;
    return sound->second;
}

void TerrainManager::streamTiles() {
    auto shouldSkipMissingAdt = [this](const TileCoord& coord) -> bool {
        if (!assetManager) return false;
        if (failedTiles.find(coord) != failedTiles.end()) return true;
        const std::string adtPath = getADTPath(coord);
        if (!assetManager->fileExists(adtPath)) {
            // Mark permanently failed so future stream/precache passes do not retry.
            failedTiles[coord] = true;
            return true;
        }
        return false;
    };

    // Enqueue tiles in radius around current tile for async loading.
    // Collect all newly-needed tiles, then sort by distance so the closest
    // (most visible) tiles get loaded first.  This is critical during taxi
    // flight where new tiles enter the radius faster than they can load.
    {
        std::lock_guard<std::mutex> lock(queueMutex);

        struct PendingEntry { TileCoord coord; int distSq; };
        std::vector<PendingEntry> newTiles;

        for (int dy = -loadRadius; dy <= loadRadius; dy++) {
            for (int dx = -loadRadius; dx <= loadRadius; dx++) {
                int tileX = currentTile.x + dx;
                int tileY = currentTile.y + dy;

                // Check valid range
                if (tileX < 0 || tileX > 63 || tileY < 0 || tileY > 63) {
                    continue;
                }

                // Circular pattern: skip corner tiles beyond radius (Euclidean distance)
                if (dx*dx + dy*dy > loadRadius*loadRadius) {
                    continue;
                }

                TileCoord coord = {.x = tileX, .y = tileY};

                // Skip if already loaded, pending, or failed
                if (loadedTiles.find(coord) != loadedTiles.end()) continue;
                if (pendingTiles.find(coord) != pendingTiles.end()) continue;
                if (failedTiles.find(coord) != failedTiles.end()) continue;
                if (shouldSkipMissingAdt(coord)) continue;

                newTiles.push_back({.coord = coord, .distSq = dx*dx + dy*dy});
                pendingTiles[coord] = true;
            }
        }

        // Sort nearest tiles first so workers service the most visible tiles
        std::sort(newTiles.begin(), newTiles.end(),
                  [](const PendingEntry& a, const PendingEntry& b) { return a.distSq < b.distSq; });

        // Insert at front so new close tiles preempt any distant tiles already queued
        for (auto it = newTiles.rbegin(); it != newTiles.rend(); ++it) {
            loadQueue.push_front(it->coord);
        }
    }

    // Notify workers that there's work
    queueCV.notify_all();

    // Unload tiles beyond unload radius (well past the camera far clip).
    // Queue them rather than unloading synchronously here - processPendingUnloads()
    // drains a time-budgeted batch per frame instead (see pendingUnloadQueue_'s comment).
    std::unordered_set<TileCoord, TileCoord::Hash> alreadyQueued(
        pendingUnloadQueue_.begin(), pendingUnloadQueue_.end());
    size_t queuedNow = 0;

    for (const auto& pair : loadedTiles) {
        const TileCoord& coord = pair.first;

        int dx = coord.x - currentTile.x;
        int dy = coord.y - currentTile.y;

        // Circular pattern: unload beyond radius (Euclidean distance)
        if (dx*dx + dy*dy > unloadRadius*unloadRadius && !alreadyQueued.count(coord)) {
            pendingUnloadQueue_.push_back(coord);
            queuedNow++;
        }
    }

    if (queuedNow > 0) {
        LOG_DEBUG("Queued ", queuedNow, " distant tiles for unload (",
                 pendingUnloadQueue_.size(), " total pending)");
    }
}

void TerrainManager::precacheTiles(const std::vector<std::pair<int, int>>& tiles) {
    std::lock_guard<std::mutex> lock(queueMutex);

    for (const auto& [x, y] : tiles) {
        if (x < 0 || x > 63 || y < 0 || y > 63) continue;

        TileCoord coord = {.x = x, .y = y};

        // Skip if already loaded, pending, or failed
        if (loadedTiles.find(coord) != loadedTiles.end()) continue;
        if (pendingTiles.find(coord) != pendingTiles.end()) continue;
        if (failedTiles.find(coord) != failedTiles.end()) continue;
        if (assetManager && !assetManager->fileExists(getADTPath(coord))) {
            failedTiles[coord] = true;
            continue;
        }

        // Precache work is prioritized so taxi-route tiles are prepared before
        // opportunistic radius streaming tiles.
        loadQueue.push_front(coord);
        pendingTiles[coord] = true;
    }

    // Notify workers to start loading
    queueCV.notify_all();
}
} // namespace rendering
} // namespace wowee
