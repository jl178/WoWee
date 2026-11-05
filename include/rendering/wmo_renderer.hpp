#pragma once

#include "rendering/collision_geometry.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/spatial_grid.hpp"
#include "rendering/shadow_params.hpp"
#include "rendering/shadow_cache.hpp"
#include "rendering/wmo_minimap.hpp"
#include "rendering/stock_wmo_portal.hpp"
#include "rendering/indoor_probes.hpp"

#include "pipeline/blp_loader.hpp"
#include "pipeline/grass_clearing.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <atomic>
#include <cstddef>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <optional>
#include <limits>
#include <future>
#include <algorithm>

namespace wowee {
namespace pipeline {
    struct WMOModel;
    struct WMOGroup;
    class AssetManager;
}

namespace rendering {

class Camera;
class Frustum;
class M2Renderer;
class RtScene;
class VkContext;
class VkTexture;

/**
 * WMO (World Model Object) Renderer (Vulkan)
 *
 * Renders buildings, dungeons, and large structures from WMO files.
 * Features:
 * - Multi-material rendering
 * - Batched rendering per group
 * - Frustum culling
 * - Portal visibility (future)
 */
class WMORenderer {
public:
    WMORenderer();
    ~WMORenderer();

    /**
     * Initialize renderer (Vulkan)
     * @param ctx Vulkan context
     * @param perFrameLayout Descriptor set layout for set 0 (per-frame UBO)
     * @param assetManager Asset manager for loading textures (optional)
     */
    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                    pipeline::AssetManager* assetManager = nullptr);

    /**
     * Cleanup GPU resources
     */
    void shutdown();

    /**
     * Set M2 renderer for hierarchical transform updates (doodads follow parent WMO)
     */
    void setM2Renderer(M2Renderer* renderer);

    /// Where loaded models register their geometry for the ray traced lighting.
    void setRtScene(RtScene* scene) { rtScene_ = scene; }
    /// Bring the scene's instances in line with this renderer's: added,
    /// moved, hidden and removed. Once a frame, and only while the lighting
    /// is on; one pass over the instances is cheaper than hooking every one
    /// of the half-dozen ways they change.
    void syncRtScene();

    /**
     * Load WMO model and create GPU resources
     * @param model WMO model with geometry data
     * @param id Unique identifier for this WMO instance
     * @return True if successful
     */
    bool loadModel(const pipeline::WMOModel& model, uint32_t id);

    enum class ModelLoadResult { Complete, InProgress, Failed };

    /// Upload a model a few groups at a time. InProgress means call again with
    /// the same arguments; a single large model can otherwise take over a
    /// hundred milliseconds, which lands as a visible hitch when terrain
    /// streaming finalises a tile.
    ModelLoadResult loadModelIncremental(const pipeline::WMOModel& model, uint32_t id, float budgetMs);

    /**
     * Check if a WMO model is currently resident in the renderer
     * @param id WMO model identifier
     */
    bool isModelLoaded(uint32_t id) const;
    /// Whether this instance's model has finished uploading its groups, and so
    /// whether a failed floor query means "nothing under you" rather than "not
    /// loaded yet". The two need telling apart: a rider held in place waiting
    /// for collision that is already there waits forever.
    bool instanceHasCollisionGeometry(uint32_t instanceId) const;

    /**
     * Check if a WMO instance is still live in the renderer. Owners that cache
     * instance IDs use this to detect an instance dropped underneath them
     * (e.g. by a renderer-wide clear) instead of addressing a dead handle.
     * @param instanceId Instance identifier returned by createInstance
     */
    bool hasInstance(uint32_t instanceId) const;

    /**
     * Unload WMO model and free GPU resources
     * @param id WMO model identifier
     */
    void unloadModel(uint32_t id);

    /**
     * Create a WMO instance in the world
     * @param modelId WMO model to instantiate
     * @param position World position
     * @param rotation Rotation (euler angles in radians)
     * @param scale Uniform scale
     * @return Instance ID
     */
    uint32_t createInstance(uint32_t modelId, const glm::vec3& position,
                           const glm::vec3& rotation = glm::vec3(0.0f),
                           float scale = 1.0f);

    /**
     * Update the world position of an existing instance (e.g., for transports)
     * @param instanceId Instance to update
     * @param position New world position
     */
    void setInstancePosition(uint32_t instanceId, const glm::vec3& position);

    /**
     * Update the full transform of an existing instance (for moving transports)
     * @param instanceId Instance to update
     * @param transform World transform matrix
     */
    void setInstanceTransform(uint32_t instanceId, const glm::mat4& transform);

    /// Mark an instance as a moving transport. Its collision still answers the
    /// static-world floor query (you walk onto a hull, you stand on a lift), but
    /// only when the deck is genuinely underfoot - see getFloorHeight. Idempotent;
    /// safe on every register.
    void setInstanceIsTransport(uint32_t instanceId, bool isTransport);

    /// Take an instance out of the world without unloading it.
    ///
    /// A cross-continent transport is one object on a route that spans two
    /// maps: for half its cycle the Undercity zeppelin is over Howling Fjord
    /// and is not here at all. Its hull holds still on this map's slice while
    /// it is away, so without this it would sit invisible-but-solid over the
    /// tower, and a player could stand on a zeppelin that is on another
    /// continent. Hidden instances are skipped by drawing and by every spatial
    /// query, which is where collision comes from.
    void setInstanceHidden(uint32_t instanceId, bool hidden);

    /**
     * Add doodad (child M2) to WMO instance
     * @param instanceId WMO instance to add doodad to
     * @param m2InstanceId M2 instance ID of the doodad
     * @param localTransform Local transform relative to WMO origin
     */
    void addDoodadToInstance(uint32_t instanceId, uint32_t m2InstanceId, const glm::mat4& localTransform);

    /// The MODF uniqueId an instance was placed with, so that doodads prepared
    /// by another tile's worker (attachDoodadByUniqueId) can find it.
    void setInstanceUniqueId(uint32_t instanceId, uint32_t uniqueId);
    /// A placed M2 is MODD doodad `moddIndex` of a WMO instance. It is drawn
    /// only while its group is (0x7ac060 draws a group's doodads with the
    /// group) and lit as 0x7c1150 lights it: an interior group's doodad by its
    /// MODD colour, an exterior one by the day/night light.
    void attachDoodad(uint32_t instanceId, uint32_t moddIndex, uint32_t m2InstanceId);
    /// The same by the placement's uniqueId; held until that instance exists.
    void attachDoodadByUniqueId(uint32_t uniqueId, uint32_t moddIndex, uint32_t m2InstanceId);

    /// The portal walk for this frame (docs/re/wmo-rendering.md §5, §10):
    /// which groups of which instances are drawn, through which part of the
    /// screen, and so which WMO doodads. Runs on the main thread before the
    /// WMO and M2 culls. Returns true when the camera stands in an interior
    /// group and no portal opens the outdoors (0x79a870 then draws no sky, no
    /// terrain and no other map object).
    /// `groundBelow` is the terrain height under the camera, when there is
    /// terrain: 0x795d40 casts its ray against the world first and only
    /// takes a map object floor that is nearer than the ground.
    bool computeVisibility(const Camera& camera, std::optional<float> groundBelow = std::nullopt);
    /// What computeVisibility found about the outdoors.
    bool outdoorSealed() const { return visSealed_; }

    // Forward declare DoodadTemplate for public API
    struct DoodadTemplate {
        std::string m2Path;
        glm::mat4 localTransform;
    };

    /**
     * Get doodad templates for a WMO model
     * @param modelId WMO model ID
     * @return Vector of doodad templates (empty if no doodads or model not found)
     */
    /// Set an animation on every child doodad of an instance. Returns how many
    /// doodads were addressed, so a caller streaming them in over several frames
    /// can tell when the set has grown and re-apply. A doodad lacking the
    /// sequence is left as it is.
    size_t setInstanceDoodadAnimation(uint32_t instanceId, uint32_t animationId, bool loop);
    const std::vector<DoodadTemplate>* getDoodadTemplates(uint32_t modelId) const;

    /**
     * Remove WMO instance
     * @param instanceId Instance to remove
     */
    void removeInstance(uint32_t instanceId);
    /**
     * Remove multiple WMO instances with a single spatial-index rebuild.
     */
    void removeInstances(const std::vector<uint32_t>& instanceIds);

    /**
     * Remove all instances
     */
    void clearInstances();

    /**
     * Clear all instances, loaded models, and texture cache (for map transitions)
     */
    void clearAll();

    /**
     * Render all WMO instances (Vulkan)
     * @param cmd Command buffer to record into
     * @param perFrameSet Per-frame descriptor set (set 0)
     * @param camera Camera for frustum culling
     */
    /** Pre-update mutable state (frame ID, material UBOs) on main thread before parallel render. */
    void prepareRender();
    /// viewerPos is the character; portal culling seeds from it as well as from
    /// the camera, because at a doorway the two stand in different groups and
    /// neither alone is reliably the right place to start.
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera,
                const glm::vec3* viewerPos = nullptr);

    /// render() in two steps, so the recording can be shared between threads:
    /// cullForRecording decides what is drawn, then recordPart(cmd, set, i, n)
    /// records the i-th of n consecutive slices of it into its own command
    /// buffer. Executed in order, the n buffers draw exactly what render()
    /// draws, in the same order; each starts by binding the state the first
    /// draw needs, as a secondary buffer inherits none. recordPart may run
    /// on several threads at once after cullForRecording has returned.
    void cullForRecording(const Camera& camera, const glm::vec3* viewerPos = nullptr);
    void recordPart(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                    uint32_t part, uint32_t partCount);
    /// How many slices this frame's recording is worth splitting into: 1 or 2.
    /// 1 when the frame is small enough that a second buffer would cost more
    /// than it saves, or when WOWEE_WMO_ONE_WORKER / WOWEE_WMO_LEGACY_RECORD
    /// is set. Valid after cullForRecording.
    uint32_t recordPartsWanted() const;

    /**
     * Initialize shadow pipeline (Phase 7)
     */
    [[nodiscard]] bool initializeShadow(VkRenderPass shadowRenderPass);

    /**
     * Render depth-only for shadow casting
     */
    /// `casters`: Static leaves transports out (the kept world map), Moving
    /// draws only them (the unit map), each one's bounding sphere then added
    /// to `moverSpheres` when given (rendering/shadow_cache.hpp).
    void renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                      const glm::vec3& shadowCenter = glm::vec3(0), float shadowRadius = 1e9f,
                      ShadowCasterSet casters = ShadowCasterSet::All,
                      std::vector<glm::vec4>* moverSpheres = nullptr);

    /**
     * Get number of loaded models
     */
    void recreatePipelines();
    /// The four main-pass pipelines, which initialize() and
    /// recreatePipelines() both need and each used to describe.
    bool buildMainPassPipelines(VkDevice device,
                                wowee::rendering::VkShaderModule& vertShader,
                                wowee::rendering::VkShaderModule& fragShader);
    bool isInitialized() const { return initialized_; }
    uint32_t getModelCount() const { return loadedModels.size(); }

    /**
     * Get number of active instances
     */
    uint32_t getInstanceCount() const { return instances.size(); }
    size_t getLoadedModelCount() const { return loadedModels.size(); }

    /**
     * Remove models that have no instances referencing them
     * Call periodically to free GPU memory
     */
    void cleanupUnusedModels();

    /**
     * Get total triangle count (all instances)
     */
    uint32_t getTotalTriangleCount() const;

    /**
     * Get total draw call count (last frame)
     */
    uint32_t getDrawCallCount() const { return lastDrawCalls; }

    /**
     * Normal mapping / Parallax Occlusion Mapping settings
     */
    void setNormalMappingEnabled(bool enabled) { normalMappingEnabled_ = enabled; materialSettingsDirty_ = true; }
    void setNormalMapStrength(float s) { normalMapStrength_ = s; materialSettingsDirty_ = true; }
    void setPOMEnabled(bool enabled) { pomEnabled_ = enabled; materialSettingsDirty_ = true; }
    void setPOMQuality(int q) { pomQuality_ = q; materialSettingsDirty_ = true; }
    /// How much of the F_SIDN night glow is on (stock_wmo::sidnNightFactor of
    /// the time of day, 0x7a8520). Negative (the default) until the renderer
    /// sets it: wmo.frag then estimates it from how dark the light is.
    void setSidnNightFactor(float f) { sidnNightFactor_ = f; }
    /// "Fade in new scenery" (Renderer::setEnhancements): a building that
    /// arrives with a streamed tile dissolves in (push.frame.y, wmo.frag).
    /// Off stamps nothing, and every instance draws as stock.
    void setStreamFadeIn(bool on) { streamFadeOn_ = on; }
    void setStreamFadeClock(float seconds) { streamFadeClock_ = seconds; }
    /// An instance the terrain streamer just made: it fades in from now.
    void markStreamedIn(uint32_t instanceId);
    // Nothing reads the WMO-only flag since the glass path went; the call
    // still marks materials for a refresh.
    void setWMOOnlyMap(bool) { materialSettingsDirty_ = true; }
    bool isNormalMappingEnabled() const { return normalMappingEnabled_; }
    float getNormalMapStrength() const { return normalMapStrength_; }
    bool isPOMEnabled() const { return pomEnabled_; }
    int getPOMQuality() const { return pomQuality_; }

    /**
     * Enable/disable wireframe rendering
     */
    void setWireframeMode(bool enabled) { wireframeMode = enabled; }

    /**
     * Enable/disable frustum culling
     */
    void setFrustumCulling(bool enabled) { frustumCulling = enabled; }

    /**
     * Enable/disable portal-based visibility culling
     */
    void setPortalCulling(bool enabled) { portalCulling = enabled; }
    bool isPortalCullingEnabled() const { return portalCulling; }

    /**
     * Enable/disable distance-based group culling
     */
    void setDistanceCulling(bool enabled, float maxDistance = 500.0f) {
        distanceCulling = enabled;
        maxGroupDistance = maxDistance;
        maxGroupDistanceSq = maxDistance * maxDistance;
    }
    bool isDistanceCullingEnabled() const { return distanceCulling; }
    float getMaxGroupDistance() const { return maxGroupDistance; }
    void setViewDistance(float distance) { viewDistance_ = std::clamp(distance, 400.0f, 2400.0f); }

    /**
     * Get number of groups culled by portals last frame
     */
    uint32_t getPortalCulledGroups() const { return lastPortalCulledGroups; }

    /**
     * Get number of groups culled by distance last frame
     */
    uint32_t getDistanceCulledGroups() const { return lastDistanceCulledGroups; }

    /**
     * Enable/disable GPU occlusion query culling (stubbed in Vulkan)
     */
    void setOcclusionCulling(bool /*enabled*/) { /* stubbed */ }
    bool isOcclusionCullingEnabled() const { return false; }

    /**
     * Get number of groups culled by occlusion queries last frame
     */
    uint32_t getOcclusionCulledGroups() const { return 0; }

    // Lighting/fog/shadow are now in the per-frame UBO; these are no-ops for API compat
    void setFog(const glm::vec3& /*color*/, float /*start*/, float /*end*/) {}
    void setLighting(const float /*lightDir*/[3], const float /*lightColor*/[3],
                     const float /*ambientColor*/[3]) {}
    void setShadowMap(uint32_t /*depthTex*/, const glm::mat4& /*lightSpace*/) {}
    void clearShadowMap() {}

    /**
     * Get floor height at a GL position via ray-triangle intersection.
     * @param outNormalZ If not null, receives the Z component of the floor surface normal
     *                   (1.0 = flat, 0.0 = vertical). Useful for slope walkability checks.
     */
    /// The floor under (glX, glY), searching at or below glZ. By default it
    /// returns the highest such floor, which is the step-up behaviour. Pass
    /// `referenceZ` (the querier's actual feet height) to instead get the floor
    /// closest to the feet - which is what keeps a player standing on the lower
    /// of two stacked floors under an overhang, rather than being snapped up to
    /// the level above them.
    std::optional<float> getFloorHeight(float glX, float glY, float glZ,
                                        float* outNormalZ = nullptr,
                                        float referenceZ =
                                            std::numeric_limits<float>::quiet_NaN()) const;

    /** Query floor collision from one moving WMO instance. */
    std::optional<float> getInstanceFloorHeight(uint32_t instanceId,
                                                float glX, float glY, float glZ,
                                                float* outNormalZ = nullptr) const;

    /** Dump diagnostic info about WMO groups overlapping a position */
    void debugDumpGroupsAtPosition(float glX, float glY, float glZ) const;

    /**
     * Append the 2D footprints of instances touching the given window, for
     * the grass generator's clearings - grass grows short and sparse beside
     * a building the way it does beside a road. Per-group boxes rather than
     * the instance's outer bounds: a keep's outer box swallows its own
     * courtyards, and a courtyard is exactly where grass belongs.
     */
    void collectGrassClearings(float minX, float minY, float maxX, float maxY,
                               std::vector<pipeline::GrassClearingSource>& out) const;

    /**
     * Check wall collision and adjust position
     * @param from Starting position
     * @param to Desired position
     * @param adjustedPos Output adjusted position (pushed away from walls)
     * @param insideWMO If true, use tighter collision for indoor precision
     * @param body The mover's collision box (half-width, height, step height),
     *        or nothing for the old fixed cylinder (0.45 / 0.5 radius, 2 tall,
     *        1 yd step).
     * @return true if collision occurred
     */
    struct Body {
        float radius;
        float height;
        float stepHeight;
    };
    bool checkWallCollision(const glm::vec3& from, const glm::vec3& to, glm::vec3& adjustedPos,
                            bool insideWMO = false, const Body* body = nullptr) const;

    /// Whether solid WMO geometry stands between two points - a sight line
    /// rather than a step, so no player radius and no step height. See the
    /// definition for why checkWallCollision cannot answer this.
    bool segmentBlocked(const glm::vec3& from, const glm::vec3& to) const;

    /// The nearest *visible, solid* WMO triangle along a ray: rendered
    /// (MOPY 0x20) and not detail (0x04). Collision-only hulls, invisible
    /// blockers and detail such as railings and gears do not count - this is
    /// what the cursor pick's occlusion asks, where only what the eye sees can
    /// stand in front of a unit. Ignores the collision focus. Returns
    /// maxDistance on no hit; `hitDesc` receives "model <id> group <n>".
    float raycastVisibleSolid(const glm::vec3& origin, const glm::vec3& direction,
                              float maxDistance, std::string* hitDesc = nullptr) const;

    /**
     * Check if a position is inside any WMO
     * @param outModelId If not null, receives the model ID of the WMO
     * @return true if inside a WMO
     */
    bool isInsideWMO(float glX, float glY, float glZ, uint32_t* outModelId = nullptr) const;

    /// Which building and which room a point is in, for the sound tables.
    struct AreaHit {
        uint32_t modelId = 0;
        uint32_t wmoId = 0;       ///< MOHD wmoID
        int32_t groupIndex = -1;  ///< index of the containing group
        uint32_t wmoGroupId = 0;  ///< the tightest containing group's MOGP id
        uint16_t nameSet = 0;
        bool interior = false;    ///< the group has the indoor flag
        /// The tightest group lacks the exterior flag (MOGP 0x8): indoors as
        /// the stock sound code counts it (0x7a1480 -> 0x4c8600, 0x78e9a0).
        bool soundIndoors = false;
        /// The group is lit as an interior: INDOOR and not EXTERIOR_LIT (0x40).
        bool interiorLit = false;
        uint32_t instanceId = 0;
        /// The tightest containing group, as an index into the loaded model's
        /// groups (not the file index), -1 when none was found.
        int32_t tightGroupIndex = -1;
    };
    bool insideWmoArea(float glX, float glY, float glZ, AreaHit* out) const;

    /// What the indoor minimap needs of an instance: its model matrix, the
    /// tile key prefix of its root (empty when the root's path is unknown),
    /// every loaded group with its file index, header bounds and flags, and
    /// where `worldPos` lies in the model's space. `groups` is in loaded-group
    /// order, so AreaHit::tightGroupIndex indexes it. False when the instance
    /// or its model is not loaded.
    bool getMinimapSource(uint32_t instanceId, const glm::vec3& worldPos,
                          glm::mat4& modelMatrix, std::string& tileRoot,
                          std::vector<wmo_minimap::Group>& groups,
                          glm::vec3& localPos) const;
    /// MOHD ambient of a loaded model (interior fill light); 0.5 grey if unknown.
    glm::vec3 getModelAmbient(uint32_t modelId) const {
        auto it = loadedModels.find(modelId);
        return it != loadedModels.end() ? it->second.wmoAmbientColor : glm::vec3(0.5f);
    }
    void setInstanceNameSet(uint32_t instanceId, uint16_t nameSet);

    /// Indoor light for units and doodads (E12, indoor_probes.hpp). On: each
    /// interior group loaded from now on bakes its probe grid, and every
    /// placed MODD doodad of an interior group takes part of the room's light
    /// into its two colours (re-applied to the doodads already placed when
    /// this changes). Off (stock): no grid is baked, the doodads keep the
    /// stock MODD light, and sampleIndoorProbe is not asked.
    void setIndoorProbes(bool on);
    [[nodiscard]] bool indoorProbes() const { return indoorProbes_; }
    /// The light a unit at `worldPos` takes indoors with the row on: false
    /// when it is not inside a group lit as an interior. `colour` is the
    /// stock colour (the building's, as character.frag takes it) moved toward
    /// the room's (indoor_probe::unitColour); `leanWorld` which way the room
    /// is brighter, world xy, |lean| <= kMaxLean.
    bool sampleIndoorProbe(const glm::vec3& worldPos, glm::vec3& colour, glm::vec2& leanWorld) const;

    /**
     * Check if a position is inside an interior WMO group (flag 0x2000).
     * Used to dim M2 lighting for doodads placed indoors.
     */
    bool isInsideInteriorWMO(float glX, float glY, float glZ) const;

    /** Gather local orange point lights derived from visible lava materials. */
    uint32_t gatherLavaLights(const glm::vec3& cameraPos,
                              glm::vec4* outPosRadius,
                              glm::vec4* outColorIntensity,
                              uint32_t maxLights) const;

    /**
     * Raycast against WMO bounding boxes for camera collision
     * @param origin Ray origin (e.g., character head position)
     * @param direction Ray direction (normalized)
     * @param maxDistance Maximum ray distance to check
     * @return Distance to first intersection, or maxDistance if no hit
     */
    float raycastBoundingBoxes(const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const;

    /**
     * Limit expensive collision/raycast queries to objects near a focus point.
     */
    void setCollisionFocus(const glm::vec3& worldPos, float radius);

    void resetQueryStats();
    double getQueryTimeMs() const { return queryTimeMs; }
    uint32_t getQueryCallCount() const { return queryCallCount; }

    /**
     * Update the tracked active WMO group based on player position.
     * Called at low frequency (every ~10 frames or on significant movement).
     */
    void updateActiveGroup(float glX, float glY, float glZ);

    // Floor cache persistence (zone-specific files)
    void setMapName(const std::string& name) { mapName_ = name; }
    const std::string& getMapName() const { return mapName_; }
    bool saveFloorCache() const;  // Saves to cache/wmo_floor_<mapName>.bin
    bool loadFloorCache();        // Loads from cache/wmo_floor_<mapName>.bin
    size_t getFloorCacheSize() const { return precomputedFloorGrid.size(); }

    // Pre-compute floor cache for all loaded WMO instances
    void precomputeFloorCache();

    // Pre-decoded BLP cache: set before calling loadModel() to skip main-thread BLP decode
    void setPredecodedBLPCache(std::unordered_map<std::string, pipeline::BLPImage>* cache) { predecodedBLPCache_ = cache; }

    // Normal/height pixels may be generated by terrain workers alongside the
    // decoded diffuse textures. loadTexture() then only performs the bounded GPU
    // upload while building the WMO model on the main thread.
    void setPredecodedNormalMapCache(
        std::unordered_map<std::string, pipeline::BLPImage>* cache,
        std::unordered_map<std::string, float>* variances) {
        predecodedNormalMapCache_ = cache;
        predecodedNormalMapVariances_ = variances;
    }
    static pipeline::BLPImage generateNormalHeightMapPixels(
        const uint8_t* pixels, uint32_t width, uint32_t height, float& outVariance);

    // Defer normal/height map generation during streaming to avoid CPU stalls
    void setDeferNormalMaps(bool defer) { deferNormalMaps_ = defer; }

private:
    // WMO material UBO - matches WMOMaterial in wmo.frag.glsl
    struct WMOMaterialUBO {
        int32_t hasTexture;        // 0
        /// The alpha reference of the batch's blend (0x873ee0): 224/255 for
        /// AlphaKey, 1/255 for Alpha..ModAdd, 0 for no test. Tested against
        /// the alpha the pixel program writes.
        float alphaRef;            // 4
        int32_t unlit;             // 8
        /// The stock light mode (0x7a8b10) of the batch, or of its outdoor
        /// pass when it is a trans batch: 0 none, 1 the outdoor light, 2 the
        /// F_WINDOW light, 3 the MOHD ambient. See stock_wmo_light.hpp.
        int32_t lightModeA;        // 12
        float specularIntensity;   // 16
        /// A trans batch's room pass (0 or 3), blended with the outdoor pass
        /// by the vertex alpha; -1 for every other batch.
        int32_t lightModeB;        // 20
        int32_t enableNormalMap;   // 24
        int32_t enablePOM;         // 28
        float pomScale;            // 32 (height scale)
        int32_t pomMaxSamples;     // 36 (max ray-march steps)
        float heightMapVariance;   // 40 (low variance = skip POM)
        float normalMapStrength;   // 44 (0=flat, 1=full, 2=exaggerated)
        int32_t isLava;            // 48 (1=lava/magma UV scroll)
        float wmoAmbientR;         // 52 (interior ambient color R)
        float wmoAmbientG;         // 56 (interior ambient color G)
        float wmoAmbientB;         // 60 (interior ambient color B)
        int32_t emissive;           // 64 (0 none, 1 lamp glass, 2 firelit)
        int32_t hasVertexColors;    // 68: the group carries MOCV (MOGP flag 0x4)
        uint32_t sidnColor;         // 72: F_SIDN night glow, MOMT sidnColor 0x00RRGGBB (0 = none)
        /// kWmoMat* bits: unified (MapObjU) formula, F_UNFOGGED, which
        /// alpha the shader writes.
        int32_t stockFlags;         // 76
        /// stock_wmo::Effect: which pixel program (0x7afee0 tables).
        int32_t effect;             // 80
        int32_t pad0, pad1, pad2;   // 84..95
    };  // 96 bytes total
    static constexpr int32_t kWmoMatUnified = 0x1;      ///< MOHD 0x2: MapObjU programs
    static constexpr int32_t kWmoMatUnfogged = 0x2;     ///< MOMT F_UNFOGGED (0x2)
    static constexpr int32_t kWmoMatTexAlpha = 0x4;     ///< MOMT shader 0/6: alpha = vertex a * texture a
    static constexpr int32_t kWmoMatExtras = 0x8;       ///< WOWEE_WMO_EXTRAS: lamp glass, clock fire, lava scroll
    static constexpr int32_t kWmoMatShadowed = 0x10;    ///< the outdoor pass takes the shadow map (0x8745d0)

    /**
     * WMO group GPU resources
     */
    struct GroupResources {
        ::VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VmaAllocation vertexAlloc = VK_NULL_HANDLE;
        ::VkBuffer indexBuffer = VK_NULL_HANDLE;
        VmaAllocation indexAlloc = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
        uint32_t vertexCount = 0;
        glm::vec3 boundingBoxMin;
        glm::vec3 boundingBoxMax;
        // The MOGP header's bounds, which the box above replaces with the
        // vertices' own. The minimap tiles are laid out from these.
        glm::vec3 headerBoxMin{0.0f};
        glm::vec3 headerBoxMax{0.0f};
        // Index in the root file. Empty groups are not loaded, so this and the
        // group's place in ModelData::groups can differ; tile keys use this.
        uint32_t fileIndex = 0;

        uint32_t groupFlags = 0;
        uint32_t wmoGroupId = 0;  ///< MOGP group id (global), see WMOGroup
        /// The room light's probes (E12), model space; empty unless the row
        /// was on when the group loaded and the group is a lit interior.
        indoor_probe::Grid probe;
        /// MOGP trans / interior batch counts: MOBA runs A (trans), B
        /// (interior), C (exterior), and the stock client lights each kind
        /// differently (0x7ac9f0).
        uint16_t transBatchCount = 0;
        uint16_t intBatchCount = 0;
        bool allUntextured = false;  // True if ALL batches use fallback white texture (collision/placeholder group)
        // A merged batch is cloth (clothDrop > 0): wmo.vert moves its vertices,
        // so the group's bounds do not hold what it draws and the frustum
        // test in render() leaves it alone.
        bool hasCloth = false;

        // Material batches (start index, count, material ID)
        struct Batch {
            uint32_t startIndex;   // First index in EBO
            uint32_t indexCount;   // Number of indices to draw
            uint8_t materialId;    // Material/texture reference
        };
        std::vector<Batch> batches;

        // Pre-merged batches for efficient rendering (computed at load time)
        //
        // Aligned to a cache line, with everything the draw loop reads in the
        // first 64 bytes: the set, the ranges, the cloth and the blend flag.
        // At 120 bytes from wherever the vector put it, a batch spanned two or
        // three lines and those fields were spread over two of them; the first
        // touch of each batch was the largest cost left in render().
        struct alignas(64) MergedBatch {
            VkDescriptorSet materialSet = VK_NULL_HANDLE;  // set 1
            // For multi-draw: store index ranges
            struct DrawRange { uint32_t firstIndex; uint32_t indexCount; };
            // draws[0], copied here when the batch is built. Most batches are
            // a single range once adjacent ranges are joined, and the draw
            // loop reads this one beside the material set instead of
            // following draws to its own heap block, which was a cache miss
            // per batch drawn and half of WMORenderer::render's time.
            DrawRange firstDraw{0, 0};
            std::vector<DrawRange> draws;
            /// Cloth hung in the building's own mesh: where the bar is, how
            /// far the cloth drops below it, and where it hangs.
            ///
            /// Stormwind's gate banners are geometry in the city WMO rather
            /// than doodads - the material is STORMWINDBANNER01 - so the sway
            /// a banner model gets has to be applied to the batch that draws
            /// one. Zero drop means the batch is not cloth.
            float clothTop = 0.0f;
            float clothDrop = 0.0f;
            float clothCentreX = 0.0f;
            float clothCentreY = 0.0f;
            bool isTransparent = false;     // blendMode >= 2
            /// blendPipelines_ slot: gxBlend * 2 + cullBack.
            uint8_t pipelineSlot = 0;
            // End of what the draw loop reads.
            bool hasTexture = false;
            bool alphaTest = false;
            bool unlit = false;
            bool isLava = false;            // lava/magma texture (UV scroll)
            // 0 = not luminous, 1 = authored lamp glass (bright), 2 = firelit
            // from behind with a flicker (clock faces). See wmo.frag's emissive
            // branch for how each level is shaded.
            uint8_t emissiveLevel = 0;
            uint32_t sidnColor = 0;         // F_SIDN night glow colour, 0x00RRGGBB (0 = none)
            int8_t lightModeA = 1;          // see WMOMaterialUBO::lightModeA
            int8_t lightModeB = -1;         // see WMOMaterialUBO::lightModeB
            uint8_t stockFlags = 0;         // WMOMaterialUBO::kWmoMat* bits
            uint8_t gxBlend = 0;            // EGxBlend this batch is drawn with
            bool cullBack = false;          // !F_UNCULLED
            uint8_t effect = 0;             // stock_wmo::Effect
            bool clampS = false;            // F_CLAMP_S
            bool clampT = false;            // F_CLAMP_T
            float alphaRef = 0.0f;          // see WMOMaterialUBO::alphaRef
            /// Each draw range's box in model space, parallel to draws: the
            /// batch culling of 0x7a7630 against the group's portal frustum.
            std::vector<std::pair<glm::vec3, glm::vec3>> drawBoxes;
            VkTexture* texture2 = nullptr;  // MOMT texture2 (env / layer 2), NOT owned
            float heightMapVariance = 0.0f; // variance of height map (low = flat texture)
            VkTexture* texture = nullptr;   // from cache, NOT owned
            VkTexture* normalHeightMap = nullptr;  // generated from diffuse, NOT owned
            ::VkBuffer materialUBO = VK_NULL_HANDLE;
            VmaAllocation materialUBOAlloc = VK_NULL_HANDLE;
        };
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
        static_assert(offsetof(MergedBatch, isTransparent) < 64);
        static_assert(offsetof(MergedBatch, pipelineSlot) < 64);
#pragma GCC diagnostic pop
        std::vector<MergedBatch> mergedBatches;
        // The ranges of every merged batch, sorted and joined, for the shadow
        // pass. It binds nothing per batch and writes depth alone, which comes
        // out the same whatever order or grouping the triangles arrive in, so
        // a group is usually one draw there instead of one per material.
        std::vector<MergedBatch::DrawRange> shadowDraws;
        // Local-space center/radius for each authored lava draw range.
        std::vector<glm::vec4> lavaLights;

        // Collision geometry (positions only, for floor raycasting)
        std::vector<glm::vec3> collisionVertices;
        std::vector<uint16_t> collisionIndices;

        // 2D spatial grid for fast triangle lookup (built at load time).
        // Bins triangles by their XY bounding box into grid cells.
        static constexpr float COLLISION_CELL_SIZE = 4.0f;
        int gridCellsX = 0;
        int gridCellsY = 0;
        glm::vec2 gridOrigin;  // XY of bounding box min
        // cellTriangles[cellY * gridCellsX + cellX] = list of triangle start indices
        std::vector<std::vector<uint32_t>> cellTriangles;

        // Pre-classified triangle lists per cell (built at load time)
        std::vector<std::vector<uint32_t>> cellFloorTriangles;  // abs(normal.z) >= 0.35
        std::vector<std::vector<uint32_t>> cellWallTriangles;   // abs(normal.z) < 0.35

        // Pre-computed per-triangle Z bounds for fast vertical reject
        struct TriBounds { float minZ; float maxZ; };
        std::vector<TriBounds> triBounds;  // indexed by triStart/3

        // Pre-computed per-triangle normals (unit length, indexed by triStart/3)
        std::vector<glm::vec3> triNormals;

        // Per-collision-triangle MOPY flags (indexed by collision tri index, i.e. triStart/3)
        std::vector<uint8_t> triMopyFlags;
        /// True when no triangle in this group blocks: no collision hull, and
        /// nothing rendered that is not detail. Detail never blocks, so such a
        /// group is walk-through in its entirety - which is a thing to be
        /// walked through only if it was meant to be, and Darkshore's bridges
        /// are 428 triangles of it.
        bool noBlockingTriangles = false;

        // Scratch bitset for deduplicating triangle queries (sized to numTriangles)
        mutable std::vector<uint8_t> triVisited;

        // Build the spatial grid from collision geometry
        void buildCollisionGrid();


        // Get triangle indices for a local-space XY range (for wall collision)
        /// The triangles of one of the three cell arrays that a query box
        /// reaches, deduplicated. The three queries below differ only in
        /// which array they pass.
        void gatherCellTriangles(const std::vector<std::vector<uint32_t>>& cells,
                                 float minX, float minY, float maxX, float maxY,
                                 std::vector<uint32_t>& out) const;

        void getTrianglesInRange(float minX, float minY, float maxX, float maxY,
                                 std::vector<uint32_t>& out) const;

        /// The triangles within `margin` of the local-space segment a-b in
        /// XY: the cells along it rather than the rectangle it spans
        /// (segmentBandColumns), deduplicated, in the rectangle's row-major
        /// order. For rays that cross a group at a slant.
        void getTrianglesAlongSegment(const glm::vec2& a, const glm::vec2& b, float margin,
                                      std::vector<uint32_t>& out) const;

        // Get pre-classified floor/wall triangles in range
        void getFloorTrianglesInRange(float minX, float minY, float maxX, float maxY,
                                      std::vector<uint32_t>& out) const;
        void getWallTrianglesInRange(float minX, float minY, float maxX, float maxY,
                                     std::vector<uint32_t>& out) const;
    };

    /**
     * Portal data for visibility culling
     */
    struct PortalData {
        uint16_t startVertex;
        uint16_t vertexCount;
        glm::vec3 normal;
        float distance;
    };

    struct PortalRef {
        uint16_t portalIndex;
        uint16_t groupIndex;
        int16_t side;
    };

    /**
     * Loaded WMO model data
     */
    struct ModelData {
        uint32_t id;
        std::vector<GroupResources> groups;
        glm::vec3 boundingBoxMin;
        glm::vec3 boundingBoxMax;
        glm::vec3 wmoAmbientColor{0.5f, 0.5f, 0.5f};  // From MOHD, used for interior lighting
        uint32_t wmoId = 0;  // MOHD wmoID, the WMOAreaTable key
        // The md5translate key prefix of the root's minimap tiles, from its
        // path (wmo_minimap::tileRoot). Empty when the loader did not say
        // where the model came from.
        std::string minimapTileRoot;
        bool isLowPlatform = false;

        // Doodad templates (M2 models placed in WMO, stored for instancing)
        // Uses the public DoodadTemplate struct defined above
        std::vector<DoodadTemplate> doodadTemplates;

        // Texture handles for this model (indexed by texture path order)
        std::vector<VkTexture*> textures;  // non-owning, from cache
        std::vector<std::string> textureNames;  // lowercase texture paths (parallel to textures)

        // Material texture indices (materialId -> texture index)
        std::vector<uint32_t> materialTextureIndices;

        // Material blend modes (materialId -> blendMode; 1 = alpha-test cutout)
        std::vector<uint32_t> materialBlendModes;

        // Material flags (materialId -> flags; 0x01 = unlit)
        std::vector<uint32_t> materialFlags;
        // MOMT sidnColor per material (BGRA as stored), the F_SIDN night glow
        std::vector<uint32_t> materialSidnColors;
        // MOMT shader per material (0 Diffuse .. 6 Composite)
        std::vector<uint32_t> materialShaders;
        /// MOMT texture2, resolved to an index into textures: the env map of
        /// MapObjEnv / EnvMetal, the second layer of MapObjUComposite.
        std::vector<uint32_t> materialTexture2Indices;
        // MOHD flags (0x2 unified render path, 0x8 colours not fixed)
        uint16_t mohdFlags = 0;
        /// MOSB, empty when the root names none.
        std::string skyboxName;

        // Portal visibility data
        std::vector<PortalData> portals;
        std::vector<glm::vec3> portalVertices;
        std::vector<PortalRef> portalRefs;
        // For each group: which portal refs belong to it (start index, count)
        std::vector<std::pair<uint16_t, uint16_t>> groupPortalRefs;
        /// The portal graph by file group index (MOGP flags, MOPR ranges,
        /// MOPT planes), what stock_wmo_portal walks.
        stock_wmo_portal::Graph portalGraph;
        /// MOGP header boxes by file group, the box 0x78fb20 tests.
        std::vector<std::pair<glm::vec3, glm::vec3>> fileGroupBoxes;
        /// File group index -> index into groups, -1 for an empty group.
        std::vector<int32_t> fileToLoaded;
        /// MODD index -> the file group whose MODR lists it (0xffff: none).
        std::vector<uint16_t> doodadGroup;
        /// MODD colour per doodad (BGRA as read).
        std::vector<uint32_t> doodadColor;
        /// MODD position per doodad, model space; only kept with the indoor
        /// light row on at load (E12), for the doodads' room light.
        std::vector<glm::vec3> doodadPos;

        // Set once the textures and materials below have been populated, so a
        // resumed load skips straight to the groups it has left.
        bool setupDone = false;
        // Next group to upload. A large model's groups are spread across several
        // calls under a time budget rather than uploaded in one stall.
        // Next texture to upload. Uploading them all at once cost 40ms on a
        // transport - the images are large, and the expense is the GPU upload
        // rather than the decode, which the worker already did.
        size_t nextTextureIndex = 0;
        size_t nextGroupIndex = 0;
        uint32_t loadedGroups = 0;
        // textureCacheGeneration_ when this load started. `textures` (and the
        // merged batches built from them) are raw pointers into textureCache;
        // a load that outlives a cache clear must start again, not resume.
        uint64_t textureGeneration = 0;

        [[nodiscard]] uint32_t getTotalTriangles() const {
            uint32_t total = 0;
            for (const auto& group : groups) {
                total += group.indexCount / 3;
            }
            return total;
        }
    };

    /**
     * WMO instance in the world
     */
    struct WMOInstance {
        uint32_t id;
        uint32_t modelId;
        /// The stream-fade clock when it came in with its tile
        /// (markStreamedIn), or negative.
        float streamFadeStart = -1.0f;
        glm::vec3 position;
        glm::vec3 rotation;  // Euler angles (radians)
        float scale;
        glm::mat4 modelMatrix;
        glm::mat4 invModelMatrix;  // Cached inverse for collision
        glm::vec3 worldBoundsMin;
        glm::vec3 worldBoundsMax;
        std::vector<std::pair<glm::vec3, glm::vec3>> worldGroupBounds;
        // The union of worldGroupBounds, which come from the vertices rather
        // than the header and so hold everything the instance draws. Not
        // valid when a group is cloth (see GroupResources::hasCloth) or the
        // model had no groups when the bounds were made.
        glm::vec3 drawBoundsMin{0.0f};
        glm::vec3 drawBoundsMax{0.0f};
        bool drawBoundsValid = false;

        // Doodad tracking: M2 instances that are children of this WMO
        struct DoodadInfo {
            uint32_t m2InstanceId;       // ID of the M2 instance
            glm::mat4 localTransform;    // Local transform relative to WMO origin
        };
        std::vector<DoodadInfo> doodads;

        /// MODF uniqueId (0 when unknown).
        uint32_t uniqueId = 0;
        /// Placed MODD doodads and the visibility slot each is drawn by.
        struct PortalDoodad {
            uint32_t slot;
            uint32_t m2InstanceId;
            uint16_t fileGroup;
            uint32_t moddIndex = 0xffffffffu;  ///< for re-lighting (setIndoorProbes)
            float radius = -1.0f;        ///< world radius, -1 until asked
            glm::vec3 localCenter{0.0f};
        };
        std::vector<PortalDoodad> portalDoodads;

        // A moving transport (ship hull, elevator). It keeps ordinary collision -
        // a rider gets exact deck height from getInstanceFloorHeight, but everyone
        // else needs the hull solid to walk aboard in the first place. The flag
        // only restricts how far from the feet the static floor query will accept
        // this deck, so a lift cycling past a bystander cannot become their ground
        // (the Undercity elevator yo-yo).
        bool isTransport = false;

        /// Loaded, positioned, and not in the world right now. See
        /// setInstanceHidden.
        bool hidden = false;
        /// MODF nameSet of the placement: with the model's wmoId and a group,
        /// the WMOAreaTable key.
        uint16_t nameSet = 0;

        void updateModelMatrix();
    };

    /// Recomputes one instance's world and per-group bounds after its
    /// model matrix has changed.
    void refreshInstanceBounds(WMOInstance& inst);

    /// Whether a collision focus is set and this instance is outside it.
    bool outsideCollisionFocus(const WMOInstance& instance) const;

    /// Whether a point is inside any of a WMO's groups, or only its
    /// interior ones. The two public queries above differ by that flag.
    bool isInsideWMOGroups(float glX, float glY, float glZ,
                           bool interiorOnly, uint32_t* outModelId,
                           AreaHit* outHit = nullptr) const;

    /// Whether a point is inside an instance's world bounds, with the Z
    /// window widened by the caller's margins.
    bool withinWorldBounds(const WMOInstance& instance,
                           float glX, float glY, float glZ,
                           float zMarginDown = 0.0f, float zMarginUp = 0.0f) const;

    /**
     * Create GPU resources for a WMO group
     *
     * mohdFlags are the root's MOHD flags and model the root, whose portals
     * the trans vertices are attenuated by: the vertex colours are fixed the
     * way the client does on load and first draw (stockVertexColors in the
     * .cpp, stock_wmo_light.hpp).
     */
    bool createGroupResources(const pipeline::WMOGroup& group, GroupResources& resources, uint32_t groupFlags = 0,
                              const glm::vec3& mohdAmbient = glm::vec3(0.0f), uint16_t mohdFlags = 0,
                              const pipeline::WMOModel* model = nullptr);

    /**
     * Check if group is visible in frustum
     */
    bool isGroupVisible(const GroupResources& group, const glm::mat4& modelMatrix,
                       const Camera& camera) const;

    /**
     * Find which group index contains a position (model space)
     * @return Group index or -1 if outside all groups
     */
    int findContainingGroup(const ModelData& model, const glm::vec3& localPos) const;
public:
    /// The MOSB sky model to draw for a camera at `worldPos`: the root's name
    /// when the camera stands in a group with MOGP flag 0x40000 (0x7ac060
    /// sets 0xcd861c while the camera is inside), else empty.
    std::string skyboxAt(const glm::vec3& worldPos) const;
private:

    /**
     * Get visible groups via portal traversal
     */
    /// Seeds from both the camera's group and the character's: at a doorway the
    /// two are in different rooms, and walking from only one while judging every
    /// door against the camera's frustum empties the interior.
    void getVisibleGroupsViaPortals(const ModelData& model,
                                     const glm::vec3& cameraLocalPos,
                                     const glm::vec3& viewerLocalPos,
                                     const Frustum& frustum,
                                     const glm::mat4& modelMatrix,
                                     std::unordered_set<uint32_t>& outVisibleGroups) const;

    /**
     * Test if a portal polygon is visible from a position through a frustum
     */
    bool isPortalVisible(const ModelData& model, uint16_t portalIndex,
                         const glm::vec3& cameraLocalPos,
                         const Frustum& frustum,
                         const glm::mat4& modelMatrix) const;

    /**
     * Load a texture from path
     */
    VkTexture* loadTexture(const std::string& path);
    std::unordered_map<std::string, pipeline::BLPImage>* predecodedBLPCache_ = nullptr;
    std::unordered_map<std::string, pipeline::BLPImage>* predecodedNormalMapCache_ = nullptr;
    std::unordered_map<std::string, float>* predecodedNormalMapVariances_ = nullptr;

    /**
     * Generate normal+height map from diffuse RGBA8 pixels
     * @param pixels RGBA8 pixel data
     * @param width Texture width
     * @param height Texture height
     * @param outVariance Receives height map variance (for POM threshold)
     * @return Generated VkTexture (RGBA8: RGB=normal, A=height)
     */
    std::unique_ptr<VkTexture> generateNormalHeightMap(const uint8_t* pixels, uint32_t width, uint32_t height, float& outVariance);

    /**
     * Allocate a material descriptor set from the pool
     */
    VkDescriptorSet allocateMaterialSet();

    /**
     * Destroy GPU resources for a single group.
     * When defer=true, destruction is scheduled via deferAfterFrameFence
     * so in-flight command buffers are not invalidated.
     */
    void destroyGroupGPU(GroupResources& group, bool defer = false);


    void rebuildSpatialIndex();
    void gatherCandidates(const glm::vec3& queryMin, const glm::vec3& queryMax, std::vector<size_t>& outIndices) const;

    // Vulkan context
    VkContext* vkCtx_ = nullptr;

    // Asset manager for loading textures
    pipeline::AssetManager* assetManager = nullptr;

    // M2 renderer for hierarchical transforms (doodads following WMO parent)
    M2Renderer* m2Renderer_ = nullptr;

    RtScene* rtScene_ = nullptr;
    std::unordered_map<uint32_t, uint32_t> rtModelMeshes_;  // modelId -> RtScene mesh
    struct RtInstanceRecord {
        uint32_t rtId;
        uint32_t modelId;
        glm::mat4 matrix;
        uint64_t seen;
    };
    std::unordered_map<uint32_t, RtInstanceRecord> rtInstances_;  // WMO instance id ->
    uint64_t rtSyncGeneration_ = 0;
    void registerRtModel(uint32_t modelId, const ModelData& model);
    void releaseRtModel(uint32_t modelId);

    // Current map name for zone-specific floor cache
    std::string mapName_;

    // Vulkan pipelines
    VkPipeline opaquePipeline_ = VK_NULL_HANDLE;  // blend 0, two-sided; base of the rest
    /// One pipeline per EGxBlend and culling, [blend][1 = back faces culled],
    /// as the client sets render states 6 and 0x11 per batch (0x7ac9f0).
    /// AlphaKey shares Opaque's (the key is the shader's alpha test);
    /// [0][0] is opaquePipeline_. Depth is written by all of them: the WMO
    /// draw loops never turn it off.
    static constexpr uint32_t kBlendPipelineCount = 12;
    VkPipeline blendPipelines_[kBlendPipelineCount][2] = {};
    VkPipeline wireframePipeline_ = VK_NULL_HANDLE;
    void destroyMainPassPipelines(VkDevice device);
    /// The clamp samplers F_CLAMP_S / F_CLAMP_T (MOMT 0x40 / 0x80) ask for,
    /// [clampS + 2 * clampT]; [0] is unused (the texture's own repeat).
    VkSampler wrapSamplers_[4] = {};
    VkSampler wrapSampler(bool clampS, bool clampT);
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;

    // Shadow rendering (Phase 7)
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadowPipelineLayout_ = VK_NULL_HANDLE;
    /// The set the shadow pass binds. Five separate members before,
    /// built and torn down here and in three other renderers.
    ShadowParamsSet shadowParams_;

    // Descriptor set layouts
    VkDescriptorSetLayout materialSetLayout_ = VK_NULL_HANDLE;

    // Descriptor pool for material sets
    VkDescriptorPool materialDescPool_ = VK_NULL_HANDLE;
    static constexpr uint32_t MAX_MATERIAL_SETS = 32768;

    // Texture cache (path -> VkTexture)
    struct TextureCacheEntry {
        std::unique_ptr<VkTexture> texture;
        std::unique_ptr<VkTexture> normalHeightMap;  // generated normal+height from diffuse
        float heightMapVariance = 0.0f;  // variance of generated height map
        size_t approxBytes = 0;
        uint64_t lastUse = 0;
    };
    std::unordered_map<std::string, TextureCacheEntry> textureCache;
    size_t textureCacheBytes_ = 0;
    uint64_t textureCacheCounter_ = 0;
    // Bumped whenever textureCache is emptied (clearAll, shutdown): every
    // VkTexture* handed out before then is gone. See ModelData::textureGeneration.
    uint64_t textureCacheGeneration_ = 0;
    // Destroys the GPU resources of the models part-way through a load and
    // forgets them. Their texture pointers die with the cache they came from.
    void dropLoadingModels(bool defer);
    size_t textureCacheBudgetBytes_ = 8192ull * 1024 * 1024;  // 8 GB default, overridden at init
    std::unordered_set<std::string> failedTextureCache_;
    std::unordered_map<std::string, uint64_t> failedTextureRetryAt_;
    std::unordered_set<std::string> loggedTextureLoadFails_;
    uint64_t textureLookupSerial_ = 0;
    uint32_t textureBudgetRejectWarnings_ = 0;

    // Default white texture
    std::unique_ptr<VkTexture> whiteTexture_;

    // Flat normal placeholder (128,128,255,128) = up-pointing normal, mid-height
    std::unique_ptr<VkTexture> flatNormalTexture_;

    // Loaded models (modelId -> ModelData)
    std::unordered_map<uint32_t, ModelData> loadedModels;
    // Models part-way through an incremental load; moved into loadedModels once
    // every group is uploaded, so a half-built model is never rendered.
    std::unordered_map<uint32_t, ModelData> loadingModels_;
    size_t modelCacheLimit_ = 4000;
    uint32_t modelLimitRejectWarnings_ = 0;

    // Active instances
    std::vector<WMOInstance> instances;
    uint32_t nextInstanceId = 1;

    bool initialized_ = false;

    // Normal mapping / POM settings
    bool normalMappingEnabled_ = true;   // on by default
    bool deferNormalMaps_ = false;       // skip normal map gen during streaming
    float normalMapStrength_ = 0.8f;     // 0.0 = flat, 1.0 = full, 2.0 = exaggerated
    bool pomEnabled_ = true;             // on by default
    float sidnNightFactor_ = -1.0f;      // see setSidnNightFactor
    bool streamFadeOn_ = false;          // see setStreamFadeIn
    bool indoorProbes_ = false;          // see setIndoorProbes
    /// A placed doodad's two colours: stock MODD light, or with the row on and
    /// a grid for its group, moved toward the room's at its position.
    void applyDoodadLight(const WMOInstance& inst, const ModelData& model, uint32_t moddIndex,
                          uint16_t fileGroup, uint32_t m2InstanceId, uint32_t slot);
    float streamFadeClock_ = 0.0f;
    int pomQuality_ = 1;                 // 0=Low(16), 1=Medium(32), 2=High(64)
    bool materialSettingsDirty_ = false; // rebuild UBOs when settings change

    // Rendering state
    bool wireframeMode = false;
    bool frustumCulling = true;  // dead: every instance is included, the group cull does the work
    bool portalCulling = true;   // AABB transform bug fixed; conservative frustum test (no plane-side check) is visually safe
    bool distanceCulling = false;  // Disabled - causes ground to disappear

    /// Master switch, and it is off: no WMO group is dropped for any reason.
    ///
    /// Buildings kept disappearing from angles that had no business hiding
    /// them. Distance culling was turned off years ago for the same complaint
    /// ("causes ground to disappear") and it did not settle the matter, because
    /// the distance test below runs whether that flag is set or not - the flag
    /// only chooses which of two distances to use, so everything past
    /// viewDistance_ vanished regardless.
    ///
    /// The set of instances is already bounded by which tiles are loaded, so
    /// what this costs is the groups of loaded buildings that are behind the
    /// camera or too far to matter, not an unbounded scene.
    ///
    /// WOWEE_WMO_CULL=1 puts portal and distance culling back, for measuring
    /// what it costs or for chasing this again.
    bool cullingEnabled_ = false;
    float maxGroupDistance = 500.0f;
    float maxGroupDistanceSq = 250000.0f;  // maxGroupDistance^2
    float viewDistance_ = 1200.0f;
    // Atomic because recordPart adds to it from more than one thread.
    std::atomic<uint32_t> lastDrawCalls{0};
    mutable uint32_t lastPortalCulledGroups = 0;
    mutable uint32_t lastDistanceCulledGroups = 0;

    // Optional query-space culling for collision/raycast hot paths.
    CollisionFocus collisionFocus;

    // Uniform grid for fast local collision queries.
    SpatialGrid spatialGrid;
    std::unordered_map<uint32_t, size_t> instanceIndexById;
    // Collision scratch buffers are thread_local (see wmo_renderer.cpp) for thread-safety.

    // Parallel visibility culling
    uint32_t numCullThreads_ = 1;

    struct InstanceDrawList {
        size_t instanceIndex;
        const ModelData* model = nullptr;     // cached pointer; saves a hashmap find per instance per frame
        std::vector<uint32_t> visibleGroups;  // group indices that passed culling
        /// Per visible group, its portal frustum in model space; the draw
        /// ranges are culled to it (0x7a7630). Empty: no batch culling.
        std::vector<stock_wmo_portal::Planes> groupPlanes;
        uint32_t portalCulled = 0;
        uint32_t distanceCulled = 0;
        // Running total of recordCost over this list and every list before
        // it, which is what recordPart cuts the frame at.
        uint32_t costEnd = 0;
    };
    std::vector<size_t> visibleInstances_;      // reused per frame
    std::vector<InstanceDrawList> drawLists_;    // reused per frame
    // Commands this frame's recording will issue, roughly: a bind and a draw
    // per batch, two binds per group. Set by cullForRecording.
    uint32_t recordCost_ = 0;
    std::unordered_set<uint32_t> portalVisibleGroupSet_; // reused per frame (portal culling scratch)

    // The stock portal walk (computeVisibility). Per instance, by index into
    // instances, valid for the frame it was computed in.
    struct InstanceVis {
        bool walked = false;   ///< false: draw as before (no graph, walk off)
        stock_wmo_portal::WalkResult res;
    };
    std::vector<InstanceVis> instanceVis_;
    bool visActive_ = false;           ///< the walk ran this frame
    bool visSealed_ = false;
    std::string visSkybox_;            ///< MOSB from the walk, camera inside
    /// Doodad visibility by slot, read by M2Renderer's cull.
    std::vector<uint8_t> doodadVisible_;
    std::vector<uint32_t> freeDoodadSlots_;
    std::unordered_map<uint32_t, uint32_t> instanceByUniqueId_;
    struct PendingDoodad { uint32_t moddIndex; uint32_t m2InstanceId; };
    std::unordered_map<uint32_t, std::vector<PendingDoodad>> pendingDoodads_;
    void releasePortalDoodads(WMOInstance& inst);
    /// The group the camera stands over (0x795d40): the nearest floor under it
    /// within 1760 yards. Returns the instance index and its file group.
    bool findCameraGroup(const glm::vec3& camPos, std::optional<float> groundBelow,
                         size_t& outInstance, uint32_t& outFileGroup) const;

    // Collision query profiling - atomic because getFloorHeight is dispatched
    // on async threads from camera_controller while the main thread reads these.
    mutable std::atomic<double> queryTimeMs{0.0};
    mutable std::atomic<uint32_t> queryCallCount{0};

    // Floor height cache - persistent precomputed grid
    static constexpr float FLOOR_GRID_CELL_SIZE = 2.0f;  // 2 unit grid cells
    mutable std::unordered_map<uint64_t, float> precomputedFloorGrid;  // key -> floor height
    mutable uint32_t currentFrameId = 0;

    uint64_t floorGridKey(float x, float y) const {
        int32_t ix = static_cast<int32_t>(std::floor(x / FLOOR_GRID_CELL_SIZE));
        int32_t iy = static_cast<int32_t>(std::floor(y / FLOOR_GRID_CELL_SIZE));
        return (static_cast<uint64_t>(static_cast<uint32_t>(ix)) << 32) |
               static_cast<uint64_t>(static_cast<uint32_t>(iy));
    }


    // Active WMO group tracking - reduces per-query group iteration
    struct ActiveGroupInfo {
        uint32_t instanceIdx = UINT32_MAX;
        uint32_t modelId = 0;
        int32_t groupIdx = -1;
        std::vector<uint32_t> neighborGroups;  // portal-connected groups
        [[nodiscard]] bool isValid() const { return instanceIdx != UINT32_MAX && groupIdx >= 0; }
        void invalidate() { instanceIdx = UINT32_MAX; groupIdx = -1; neighborGroups.clear(); }
    };
    mutable ActiveGroupInfo activeGroup_;

    // Per-frame floor height dedup cache (same XY queried 3-5x per frame)
    struct FrameFloorCache {
        static constexpr size_t CAPACITY = 16;
        struct Entry { uint64_t key; float resultZ; float normalZ; uint32_t frameId; };
        Entry entries[CAPACITY] = {};

        [[nodiscard]] uint64_t makeKey(float x, float y) const {
            // 0.5-unit quantized grid
            int32_t ix = static_cast<int32_t>(std::floor(x * 2.0f));
            int32_t iy = static_cast<int32_t>(std::floor(y * 2.0f));
            return (static_cast<uint64_t>(static_cast<uint32_t>(ix)) << 32) |
                   static_cast<uint64_t>(static_cast<uint32_t>(iy));
        }

        std::optional<float> get(float x, float y, uint32_t frame, float* outNormalZ = nullptr) const {
            uint64_t k = makeKey(x, y);
            size_t slot = k % CAPACITY;
            const auto& e = entries[slot];
            if (e.frameId == frame && e.key == k) {
                if (outNormalZ) *outNormalZ = e.normalZ;
                return e.resultZ;
            }
            return std::nullopt;
        }

        void put(float x, float y, float result, float normalZ, uint32_t frame) {
            uint64_t k = makeKey(x, y);
            size_t slot = k % CAPACITY;
            entries[slot] = { .key = k, .resultZ = result, .normalZ = normalZ, .frameId = frame };
        }
    };
};

} // namespace rendering
} // namespace wowee
