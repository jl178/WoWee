#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <cstdint>
#include <vector>
#include <future>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "rendering/animation/body_facing.hpp"
#include "rendering/pass_ablation.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/shadow_cache.hpp"
#include "rendering/enhancements.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/sky_system.hpp"
#include "core/screen_recorder.hpp"
#include "pipeline/custom_zone_discovery.hpp"

#include "pipeline/grass_biomes.hpp"
#include "pipeline/grass_clearing.hpp"
#include "pipeline/grass_population.hpp"
#include "pipeline/grass_profile.hpp"

namespace wowee {
namespace core { class Window; }
namespace rendering { class VkContext; }
namespace game { class World; class ZoneManager; class GameHandler; }
namespace audio { class AudioCoordinator; }
namespace pipeline { class AssetManager; }

namespace rendering {

class Camera;
class CameraController;
class TerrainRenderer;
class TerrainManager;
class PerformanceHUD;
class WaterRenderer;
class Skybox;
class Celestial;
class StarField;
class Clouds;
class LensFlare;
class Weather;
class Lightning;
class LightingManager;
class SwimEffects;
class FootfallDust;
class LevelUpEffect;
class CharacterRenderer;
class WMORenderer;
class M2Renderer;
class Minimap;
namespace world_map { class WorldMapFacade; }
using WorldMap = world_map::WorldMapFacade;
class QuestMarkerRenderer;
class FootprintRenderer;
class CharacterPreview;
class AmdFsr3Runtime;
class SpellVisualSystem;
class PostProcessPipeline;
class AnimationController;
class LevelUpEffect;
class SwimEffects;
class RenderGraph;
class OverlaySystem;
class HiZSystem;
class GrassRenderer;
class VolumetricFog;
class RtScene;
class RtLighting;
class LootSparkles;
class SunShafts;
class SoftParticlePass;
class ScreenCapture;

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool initialize(core::Window* window);
    void shutdown();

    void beginFrame();
    /// Where beginFrame reports how long each of its parts took, when the
    /// frame profiler is on. Unset, nothing is measured.
    void setStageSink(std::function<void(const char*, float)> sink);
    void endFrame();

    /// Recorded after the interface is drawn and before the frame is submitted,
    /// into the frame's own command buffer. A second window draws here - see
    /// AuxSwapchain - so the frame's one fence covers it. Empty clears it.
    void setAfterInterfaceRecorder(std::function<void(VkCommandBuffer)> recorder) {
        afterInterface_ = std::move(recorder);
    }

    void renderWorld(game::World* world, game::GameHandler* gameHandler = nullptr);

    /**
     * Update renderer (camera, etc.)
     */
    void update(float deltaTime);

    /**
     * Load test terrain for debugging
     * @param assetManager Asset manager to load terrain data
     * @param adtPath Path to ADT file (e.g., "World\\Maps\\Azeroth\\Azeroth_32_49.adt")
     */
    bool loadTestTerrain(pipeline::AssetManager* assetManager, const std::string& adtPath);

    /**
     * Initialize all sub-renderers (WMO, M2, Character, terrain, water, minimap, etc.)
     * without loading any ADT tile.  Used by WMO-only maps (dungeons/raids/BGs).
     */
    bool initializeRenderers(pipeline::AssetManager* assetManager, const std::string& mapName);
    /// The map every sub-renderer is looking at.
    ///
    /// initializeRenderers set this on all four when it built them, and it
    /// builds them once - so on every later map change the two the loader
    /// remembered by hand moved and the two it did not stayed on the map the
    /// session started in. The minimap was one of those: in a dungeon it went
    /// on asking for the tiles of wherever the player had logged in, and got
    /// none, which is a minimap with no dungeon on it.
    void setActiveMapName(const std::string& name);

    /**
     * Enable/disable terrain rendering
     */
    void setTerrainEnabled(bool enabled) { terrainEnabled = enabled; }

    /**
     * Enable/disable wireframe mode
     */
    void setWireframeMode(bool enabled);



    /**
     * Render performance HUD
     */
    void renderHUD();

    Camera* getCamera() { return camera.get(); }
    CameraController* getCameraController() { return cameraController.get(); }
    TerrainRenderer* getTerrainRenderer() const { return terrainRenderer.get(); }
    TerrainManager* getTerrainManager() const { return terrainManager.get(); }
    PerformanceHUD* getPerformanceHUD() { return performanceHUD.get(); }
    WaterRenderer* getWaterRenderer() const { return waterRenderer.get(); }
    Skybox* getSkybox() const { return skySystem ? skySystem->getSkybox() : nullptr; }
    Celestial* getCelestial() const { return skySystem ? skySystem->getCelestial() : nullptr; }
    StarField* getStarField() const { return skySystem ? skySystem->getStarField() : nullptr; }
    Clouds* getClouds() const { return skySystem ? skySystem->getClouds() : nullptr; }
    LensFlare* getLensFlare() const { return skySystem ? skySystem->getLensFlare() : nullptr; }
    Weather* getWeather() const { return weather.get(); }
    Lightning* getLightning() const { return lightning.get(); }
    CharacterRenderer* getCharacterRenderer() const { return characterRenderer.get(); }
    /// M2 sound event hook (instanceId, animationId, eventTag, soundEntryId,
    /// render-space position) - see CharacterRenderer::AnimSoundEventCallback.
    /// Kept here because the character renderer is created with the world,
    /// after the core callbacks are installed; it is applied to the current
    /// one and to any created later.
    using CharacterAnimSoundEventCallback =
        std::function<void(uint32_t, uint32_t, uint32_t, uint32_t, const glm::vec3&)>;
    void setCharacterAnimSoundEventCallback(CharacterAnimSoundEventCallback cb);
    WMORenderer* getWMORenderer() const { return wmoRenderer.get(); }
    M2Renderer* getM2Renderer() const { return m2Renderer.get(); }
    Minimap* getMinimap() const { return minimap.get(); }
    WorldMap* getWorldMap() const { return worldMap.get(); }
    QuestMarkerRenderer* getQuestMarkerRenderer() const { return questMarkerRenderer.get(); }
    FootprintRenderer* getFootprintRenderer() const { return footprintRenderer.get(); }
    /// A footfall of the local character or its mount, from the footstep
    /// driver (which times them for the sounds, with a fallback where the
    /// model has no $FSD): the "Dust kicked up" row's feed for those two.
    /// Everyone else's come from their models' events. Nothing with the row off.
    void noteLocalFootfall(uint32_t instanceId, const glm::vec3& position) {
        if (enh_.footfallDust && pendingFootfalls_.size() < 512)
            pendingFootfalls_.push_back({instanceId, position});
    }
    SkySystem* getSkySystem() const { return skySystem.get(); }
    const std::string& getCurrentZoneName() const;
    uint32_t getCurrentZoneId() const;
    /// AreaTable id of the area the player is in, 0 if unknown: the WMO
    /// group's WMOAreaTable area while one decides it (Ironforge, the
    /// Undercity), else the terrain chunk's. Resolved once a frame.
    uint32_t getCurrentAreaId() const;
    /// The WMOAreaTable name of the room the player is in ("The Great Forge",
    /// "Trade Quarter"), the stock subzone text there. Empty outdoors and in
    /// rooms the table leaves unnamed.
    const std::string& getCurrentWmoAreaName() const { return liveWmoAreaName_; }
    /// The global id (MOGP group id) of the WMO group the player stands in,
    /// 0 outdoors. The world map picks a dungeon floor from it, through
    /// DungeonMapChunk, the way the client does.
    uint32_t getPlayerWmoGroupId() const;
    /// The area under the player, asked of AreaTable rather than resolved to a
    /// zone first - the world PvP flag is on the subzone. See ZoneManager.
    bool isOnOutdoorPvpObjective() const;
    bool isPlayerIndoors() const { return playerIndoors_; }
    VkContext* getVkContext() const { return vkCtx; }
    VkDescriptorSetLayout getPerFrameSetLayout() const { return perFrameSetLayout; }
    /// What a per-frame set allocated elsewhere binds at binding 2: a fog
    /// volume of clear air, for a set whose block leaves the fog off.
    VkImageView getNeutralFogVolumeView() const;
    /// What a per-frame set allocated elsewhere binds at 3 and 4.
    VkImageView getNeutralRtLightingView() const;
    /// The geometry the ray traced lighting sees; renderers register with it.
    RtScene* getRtScene() const { return rtScene_.get(); }
    VkRenderPass getShadowRenderPass() const { return shadowRenderPass; }

    // Third-person character follow
    void setCharacterFollow(uint32_t instanceId);
    glm::vec3& getCharacterPosition() { return characterPosition; }
    /// The instance the local player is drawn and animated with right now:
    /// the character itself, or the creature model of a shapeshift, polymorph
    /// or costume while UNIT_FIELD_DISPLAYID differs from the native display.
    uint32_t getCharacterInstanceId() const { return characterInstanceId; }
    /// The character's own model, morphed or not. Equipment, geosets and
    /// skin belong to this one; while a morph is drawn it is kept, hidden,
    /// so turning back is instant and wears what it wore.
    uint32_t getNativeCharacterInstanceId() const {
        return nativeCharacterInstanceId_ != 0 ? nativeCharacterInstanceId_ : characterInstanceId;
    }
    /// Draw the local player with another instance (a creature display), or
    /// with its own model again given 0. The camera and the animation
    /// controller follow the drawn instance, so the pivot sits on the form's
    /// head and the form plays its own Stand/Run/Attack.
    void setCharacterMorphInstance(uint32_t morphInstanceId);
    float getCharacterYaw() const { return characterYaw; }
    void setCharacterYaw(float yawDeg) { characterYaw = yawDeg; }
    /// The yaw the player model is DRAWN at (degrees): the stock body yaw,
    /// which springs toward the direction of travel while strafing and lags a
    /// turn in place (animation/body_facing.hpp). getCharacterYaw stays the
    /// logical facing (server orientation, melee arcs).
    float getCharacterVisualYaw() const {
        return bodyFacing_.initialized ? characterVisualYawDeg_ : characterYaw;
    }
    /// Radians the drawn body turned this frame by the clamp and catch-up
    /// (BodyFacingResult::bodyStep, CCW positive). Standing, a step past
    /// 1e-5 either way is what plays ShuffleLeft/ShuffleRight in the stock
    /// client (CGUnit_C 0x0073E2C0).
    float getBodyFacingStep() const { return bodyFacingStep_; }

    // Screenshot capture - copies swapchain image to PNG file
    bool captureScreenshot(const std::string& outputPath);

    /// Start writing the screen, interface included, and what the client
    /// plays to a video file. False, with the reason in error, when it cannot.
    bool startRecording(const std::string& path, std::string& error);
    /// Finish the file and close it.
    core::ScreenRecorder::Stats stopRecording();
    [[nodiscard]] bool isRecording() const;
    /// Why a recording stopped by itself - the disk filled, the encoder gave
    /// up - once, for the interface to say; empty otherwise. The file is
    /// finished properly all the same.
    std::string takeRecordingFailure();

    // Spell visual effects (SMSG_PLAY_SPELL_VISUAL / SMSG_PLAY_SPELL_IMPACT)
    // Delegates to SpellVisualSystem (owned by Renderer)
    SpellVisualSystem* getSpellVisualSystem() const { return spellVisualSystem_.get(); }

    // Combat visual state (compound: resets AnimationController + SpellVisualSystem)
    void resetCombatVisualState();

    // Sub-system accessors (§4.2)
    AnimationController* getAnimationController() const { return animationController_.get(); }
    LevelUpEffect* getLevelUpEffect() const { return levelUpEffect.get(); }
    LootSparkles* getLootSparkles() const { return lootSparkles_.get(); }
    SwimEffects* getSwimEffects() const { return swimEffects.get(); }

    // Selection circle for targeted entity
    /// See OverlaySystem::setSelectionCircle.
    void setSelectionCircle(const glm::vec3& pos, float radius, const glm::vec3& color,
                            float alpha = 1.0f, float attackPulse = -1.0f);
    void clearSelectionCircle();
    /// See OverlaySystem::setSpellTargetDecal.
    void setSpellTargetDecal(const glm::vec3& center, float halfWidth, bool acceptable);
    void clearSpellTargetDecal();

    // CPU timing stats (milliseconds, last frame).
    double getLastUpdateMs() const { return lastUpdateMs; }
    double getLastRenderMs() const { return lastRenderMs; }
    double getLastCameraUpdateMs() const { return lastCameraUpdateMs; }
    double getLastTerrainRenderMs() const { return lastTerrainRenderMs; }
    double getLastWMORenderMs() const { return lastWMORenderMs; }
    double getLastM2RenderMs() const { return lastM2RenderMs; }
    // Audio coordinator - owned by Application, set via setAudioCoordinator().
    void setAudioCoordinator(audio::AudioCoordinator* ac) { audioCoordinator_ = ac; }
    audio::AudioCoordinator* getAudioCoordinator() { return audioCoordinator_; }
    game::ZoneManager* getZoneManager() { return zoneManager.get(); }
    LightingManager* getLightingManager() { return lightingManager.get(); }

    const std::vector<pipeline::CustomZoneInfo>& getCustomZones() const { return customZones_; }

private:
    std::function<void(VkCommandBuffer)> afterInterface_;

    // True when water is drawn in the scene continuation pass rather than in
    // the scene pass itself (see renderWorld).
    bool waterDrawsInContinuePass() const;

    /// Ghost tint, brightness and the minimap, in that order, at the end of
    /// the scene pass. The threaded and single-threaded paths both finish this
    /// way and differ only in which command buffer they are recording into.
    /// The underwater tint and its waterline. One implementation, called
    /// from both the parallel and the fallback recording paths, which had
    /// carried different ones.
    void renderPostSceneOverlays(VkCommandBuffer cmd, game::GameHandler* gameHandler);
    void renderMinimapOverlay(VkCommandBuffer cmd, game::GameHandler* gameHandler);

    /// Point the swim spray at whichever pass the water ends up drawing in, so
    /// it can be recorded after the water rather than under it. Must run before
    /// the spray's pipelines are built, and again whenever they are rebuilt.
    void syncSwimEffectsTargetPass();
    /// syncSwimEffectsTargetPass, then rebuild the spray or the minimap if the
    /// answer moved either of them. Runs from beginFrame every frame, once the
    /// post-process target for the frame is settled; free when nothing moved.
    void refreshSwimEffectsPass();

    /// True when the spray's pipelines were built for the water continuation
    /// pass. Draw sites test this rather than waterDrawsInContinuePass() so a
    /// mid-run mode change cannot record a pipeline into an incompatible pass.
    bool swimEffectsDrawWithWater_ = false;
    /// Whether the minimap follows water out of the scene pass, for the same
    /// reason the spray does - see syncSwimEffectsTargetPass.
    bool minimapDrawsWithWater_ = false;
    /// Whether the minimap is drawn at the end of the late particle pass
    /// instead (soft particles on, water still in the scene pass): that pass
    /// draws after everything the scene pass holds, the minimap included,
    /// and would put the particles over it.
    bool minimapDrawsLate_ = false;
    /// Whether the weather follows the water into its continuation pass:
    /// only with soft particles on, so the blended particles - drawn in the
    /// late pass between the scene and the water - stay under the rain and
    /// snow as they stay under the water. Off, the weather is where it was.
    bool weatherDrawsWithWater_ = false;
    /// The spray, minimap and weather objects the last refresh built for.
    /// One made since (the minimap comes with the world, after the first
    /// decision) built its pipelines for the scene pass whatever the flags
    /// say, so it is rebuilt for where it now draws.
    const void* passBuiltFor_[3] = {};
    /// The framebuffer for the water continuation over an off-screen MSAA
    /// scene (soft particles on): its resolved colour and depth, made for the
    /// frame and released after its fence.
    VkFramebuffer sceneWater1xFramebuffer(VkExtent2D& extent);
    /// Its view and framebuffer per frame slot, destroyed when the slot next
    /// makes them (its fence has passed) or at shutdown - not through
    /// VkContext's deferred queue, which a preview renderer's shutdown during
    /// the UI flushes while this frame still records.
    struct SceneWaterRetired {
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
    };
    SceneWaterRetired sceneWaterRetired_[2] = {};
    void destroySceneWaterRetired(uint32_t slot);

    // Soft particles (the "Soft particles" row, SoftParticlePass). Decided
    // once a frame in beginFrame, before anything is recorded: the M2
    // renderer leaves its blended particles for the late pass only on a
    // frame that will have one.
    std::unique_ptr<SoftParticlePass> softParticlePass_;
    bool softParticlesFrame_ = false;
    bool softParticlesLateDone_ = false;
    uint32_t softParticlesOffFrames_ = 0;
    game::GameHandler* softParticlesGameHandler_ = nullptr;
    void decideSoftParticles();
    /// The late pass, at whichever point of the frame the scene pass closes
    /// first; once a frame, and only on a frame decideSoftParticles allowed.
    void recordSoftParticlePass(VkCommandBuffer cmd);

    void runDeferredWorldInitStep(float deltaTime);

    core::Window* window = nullptr;
    std::unique_ptr<Camera> camera;
    std::unique_ptr<CameraController> cameraController;
    std::unique_ptr<TerrainRenderer> terrainRenderer;
    /// How much of the line from the eye to the sun is blocked, smoothed.
    ///
    /// Sampled once a frame in update() and eased, because the raw answer is a
    /// yes or a no and a hill edge crossing it would snap the flare on and off.
    float sunOcclusion_ = 0.0f;

    /// That line, asked of the terrain, the buildings and where the camera is.
    [[nodiscard]] float sampleSunOcclusion() const;

    std::unique_ptr<TerrainManager> terrainManager;
    std::unique_ptr<PerformanceHUD> performanceHUD;
    std::unique_ptr<WaterRenderer> waterRenderer;
    std::unique_ptr<Skybox> skybox;
    std::unique_ptr<Celestial> celestial;
    std::unique_ptr<StarField> starField;
    std::unique_ptr<Clouds> clouds;
    std::unique_ptr<LensFlare> lensFlare;
    std::unique_ptr<Weather> weather;
    std::unique_ptr<Lightning> lightning;
    std::unique_ptr<LightingManager> lightingManager;
    std::unique_ptr<SkySystem> skySystem;  // Coordinator for sky rendering
    std::unique_ptr<SwimEffects> swimEffects;
    std::unique_ptr<FootfallDust> footfallDust_;
    /// Frames since the dust went off; it is released past the frames in flight.
    uint32_t footfallDustIdleFrames_ = 0;
    bool footfallDustFailed_ = false;
    /// Footfalls ($FSD) reported by the character update this frame, turned
    /// into dust after the M2 update has joined (updateFootfallDust).
    struct PendingFootfall {
        uint32_t instanceId;
        glm::vec3 position;
    };
    std::vector<PendingFootfall> pendingFootfalls_;
    /// Each walker's last footfall, for its speed, and the ground there.
    struct LastFootfall {
        glm::vec3 position;
        float time;
        glm::vec3 groundAt;
        uint8_t ground;  // footfall_dust::Ground
    };
    std::unordered_map<uint32_t, LastFootfall> lastFootfall_;
    float footfallClock_ = 0.0f;
    /// The character renderer's event callback: the footfalls for the dust (row
    /// on) ahead of the sounds' callback. Installed only when one of the two
    /// wants events, so with neither the character update fires none.
    void installCharacterAnimEventCallback();
    bool animEventCallbackHasDust_ = false;
    std::unique_ptr<LevelUpEffect> levelUpEffect;
    std::unique_ptr<LootSparkles> lootSparkles_;
    std::unique_ptr<CharacterRenderer> characterRenderer;
    CharacterAnimSoundEventCallback characterAnimSoundEventCallback_;
    std::unique_ptr<WMORenderer> wmoRenderer;
    std::unique_ptr<M2Renderer> m2Renderer;
    std::unique_ptr<M2Renderer> skyboxModelRenderer_;
    /// The last zone a terrain chunk actually named. Many chunks carry an area
    /// id of zero, and a tile being loaded carries none at all, so the lookup
    /// answers "do not know" often - and answering from somewhere else instead
    /// made the zone flip at chunk boundaries while walking. Mutable because
    /// getCurrentZoneId() is const and this is a cache of what it last learnt.
    mutable uint32_t lastResolvedZoneId_ = 0;
    /// The map that answer belongs to, so it is dropped on a
    /// continent change rather than held across one.
    mutable uint32_t lastResolvedZoneMapId_ = 0xFFFFFFFFu;

    /// The original client's sky models that are up, one instance each, faded
    /// by the weight LightingManager gives them. See updateSkyboxLayers.
    struct SkyLayerInstance {
        std::string path;
        uint32_t instanceId = 0;
    };
    std::vector<SkyLayerInstance> skyLayers_;
    /// Sky models already uploaded, by path, so one that fades out and back in
    /// is not read off disk again.
    std::unordered_map<std::string, uint32_t> loadedSkyModels_;
    /// Sky paths that did not resolve to a usable model, so a failing path is
    /// not read off disk again on every frame it is wanted.
    std::unordered_set<std::string> failedSkyboxPaths_;
    std::unique_ptr<Minimap> minimap;
    std::unique_ptr<WorldMap> worldMap;
    std::unique_ptr<QuestMarkerRenderer> questMarkerRenderer;
    std::unique_ptr<FootprintRenderer> footprintRenderer;
    audio::AudioCoordinator* audioCoordinator_ = nullptr;  // Owned by Application
    std::unique_ptr<AnimationController> animationController_;  // §4.2
    std::unique_ptr<game::ZoneManager> zoneManager;
    // Shadow mapping (Vulkan)
    /// The shadow map is square and this is its side, chosen before the
    /// per-frame resources are built and not changed after.
    ///
    /// Deliberately not live. Those resources are created once at start-up and
    /// destroyed at shutdown - they are not part of the swapchain-resize path,
    /// so rebuilding them mid-session would be a new and unexercised one, and
    /// this renderer has lost a device to exactly that before. The setting is
    /// marked as needing a restart in the panel instead, which is what the
    /// original client does for the settings it cannot change live.
    uint32_t SHADOW_MAP_SIZE = 4096;
    void setShadowMapSize(uint32_t side) {
        // Powers of two between 512 and 4096: the quality slider has five
        // steps and these are they.
        SHADOW_MAP_SIZE = std::clamp(side, 512u, 4096u);
    }
    // Per-frame shadow resources: each in-flight frame has its own depth image and
    // framebuffer so that frame N's shadow read and frame N+1's shadow write don't
    // race on the same image across concurrent GPU submissions.
    // Array size must match MAX_FRAMES (= 2, defined in the private section below).
    VkImage shadowDepthImage[2] = {};
    VmaAllocation shadowDepthAlloc[2] = {};
    VkImageView shadowDepthView[2] = {};
    VkSampler shadowSampler = VK_NULL_HANDLE;
    VkRenderPass shadowRenderPass = VK_NULL_HANDLE;
    VkFramebuffer shadowFramebuffer[2] = {};
    VkImageLayout shadowDepthLayout_[2] = {};
    /// With shadows off, whether this slot's map has been cleared and left
    /// readable since they went off (renderShadowPass then records nothing).
    bool shadowClearedWhileOff_[2] = {};

    // The sun's shadow in two maps (rendering/shadow_cache.hpp). The two
    // images above are the world map's: shadowDepthImage[shadowFront_] is the
    // one the frame reads, the other the one a redraw goes into. They are no
    // longer one per frame slot; binding 1 of each slot's set is pointed at
    // the front one when that slot next begins (bindWorldShadowMap).
    uint32_t shadowFront_ = 0;
    /// What binding 1 of each slot's per-frame and reflection set holds.
    VkImageView shadowBoundView_[2] = {};
    /// What the front map was drawn for, and its matrix.
    WorldShadowCacheKey shadowFrontKey_;
    glm::mat4 shadowFrontMatrix_{0.0f};
    glm::vec3 shadowFrontCenter_{0.0f};
    /// A redraw of the world map in progress: a part a frame into `target`,
    /// or all of it at once (`now`). Its key and projection are taken when it
    /// starts and kept to the end, so its parts line up.
    struct WorldShadowRedrawState {
        bool active = false;
        bool now = false;
        uint8_t nextPart = 0;
        uint32_t target = 1;
        WorldShadowCacheKey key;
        ShadowProjection proj;
    } shadowRedraw_;
    /// This frame's parts (bit per WorldShadowPart), and whether the redraw
    /// ends with them; decided in planShadowFrame, drawn in renderShadowPass.
    uint8_t shadowPartsThisFrame_ = 0;
    bool shadowRedrawEndsThisFrame_ = false;
    /// Redraws started, by reason: drawn at once, drift, sun, world change.
    uint32_t shadowRedrawCounts_[4] = {};
    /// The unit map, per frame slot, a quarter of the world map's area.
    uint32_t SHADOW_NEAR_SIZE = 2048;
    VkImage shadowNearImage[2] = {};
    VmaAllocation shadowNearAlloc[2] = {};
    VkImageView shadowNearView[2] = {};
    VkFramebuffer shadowNearFramebuffer[2] = {};
    VkImageLayout shadowNearLayout_[2] = {};
    bool shadowNearClearedWhileOff_[2] = {};
    /// The shadow render pass with LOAD rather than CLEAR, for the second and
    /// third parts of a spread redraw (the dynamic-rendering path sets the
    /// load op per begin instead).
    VkRenderPass shadowRenderPassLoad = VK_NULL_HANDLE;
    bool nearShadowOn_ = false;
    ShadowProjection nearShadowProj_;
    NearShadowMask nearShadowMask_;
    std::vector<glm::vec4> nearCasterSpheres_;
    uint32_t nearSphereSplit_[2] = {};  ///< where the units' and doodads' spheres end
    /// WOWEE_SHADOW_SPLIT=0: one map, every caster, every frame, as before
    /// the split - for measuring what it saves.
    bool shadowSplit_ = true;
    glm::mat4 lightSpaceMatrix = glm::mat4(1.0f);
    glm::vec3 shadowCenter = glm::vec3(0.0f);
    bool shadowCenterInitialized = false;
    bool shadowsEnabled = true;
    float shadowDistance_ = 300.0f;  // Shadow frustum half-extent (default: 300 units)
    float viewDistance_ = 1200.0f;
    bool sharpStars_ = true;
    float diagTerrainFurthest_ = -1.0f;
    float diagM2Furthest_ = -1.0f;


public:
    // Character preview registration (for off-screen composite pass)
    void registerPreview(CharacterPreview* preview);
    void unregisterPreview(CharacterPreview* preview);

    /// Off stops the scene sampling the shadow map; the shadow pass itself
    /// still runs whole, casters and all. See renderShadowPass.
    void setShadowsEnabled(bool enabled) { shadowsEnabled = enabled; }
    bool areShadowsEnabled() const { return shadowsEnabled; }
    void setShadowDistance(float dist) { shadowDistance_ = glm::clamp(dist, 40.0f, 500.0f); }
    float getShadowDistance() const { return shadowDistance_; }
    void setViewDistance(float distance);
    float getViewDistance() const { return viewDistance_; }
    /// Draw the client's own point stars in place of the sky model's baked
    /// star layer, which is a 256x256 compressed texture stretched across the
    /// whole dome. See Renderer::setSharpStars.
    /// WOWEE_VIEW_DIAG=1: one line naming how far terrain and doodads each
    /// actually drew, so the two can be compared rather than reasoned about.
    void logViewDistanceDiag();
    void setSharpStars(bool enabled);
    bool areSharpStars() const { return sharpStars_; }
    /// Fog lit by the sun through the shadow map and by nearby torches: 0 is
    /// off, 1-3 the volume's resolution. Applied at the start of the next
    /// frame. See VolumetricFog.
    void setVolumetricFogQuality(int quality);
    /// Ray traced lighting: 0 off, 1 sun shadows, 2 + ambient occlusion,
    /// 3 + one-bounce diffuse. See RtLighting.
    void setRtLightingMode(int mode);
    /// A multiplier on how thick that air is; 1 is the default mist.
    void setVolumetricFogDensity(float density) { volumetricFogDensity_ = glm::clamp(density, 0.0f, 3.0f); }
    /// Rays streaming from the sun across the finished picture. See SunShafts.
    void setSunShaftsEnabled(bool enabled) { sunShaftsEnabled_ = enabled; }
    /// How hard the wind moves foliage, grass and cloth; 1 is the usual
    /// breeze, 0 holds everything still. Weather and indoors scale it further,
    /// eased; see updatePerFrameUBO.
    void setFoliageWind(float strength) { foliageWindSetting_ = glm::clamp(strength, 0.0f, 2.0f); }
    /// A multiplier on the shadow darkness Light.dbc asks for; 1 is as set.
    void setShadowStrength(float strength) { shadowStrengthSetting_ = glm::clamp(strength, 0.5f, 1.5f); }
    /// A soft foam band where water meets the shore, from the water's depth
    /// over the scene under it. Handed to the water every frame, since the
    /// water renderer is built with the world, after settings are applied.
    void setShorelineFoam(bool enabled) { shorelineFoam_ = enabled; }
    void setWaterReflections(bool enabled) { waterReflections_ = enabled; }
    void setWaterSplashes(bool enabled) { waterSplashes_ = enabled; }
    /// Darker, slightly glossy outdoor ground and walls while it rains, eased
    /// in and out (WetGround). Off snaps the ground dry at once.
    void setWetGround(bool enabled) { wetGroundEnabled_ = enabled; }
    /// The ground's relief: the terrain shader's bump from its own texture,
    /// which follows the Surface bumps setting and its strength as the
    /// buildings' normal maps do. 0 is the flat stock ground; 1 the default
    /// strength (0.8). Carried to terrain.frag in viewPos.w.
    void setTerrainRelief(bool enabled, float strength) {
        terrainRelief_ = enabled ? glm::clamp(strength / 0.8f, 0.0f, 2.5f) : 0.0f;
    }
    int getTerrainLoadRadius() const;
    int getTerrainUnloadRadius() const { return getTerrainLoadRadius() + 3; }
    void setMsaaSamples(VkSampleCountFlagBits samples);

    // Post-process pipeline API - delegates to PostProcessPipeline (§4.3)
    PostProcessPipeline* getPostProcessPipeline() const;
    void setFSREnabled(bool enabled);
    void setFSR2Enabled(bool enabled);

    /// Every enhancement over the stock picture at once (rendering::Enhancements,
    /// ui/enhancement_rows.hpp). Kept here and handed to each subsystem when it
    /// changes and whenever a subsystem is new (the world's renderers are made
    /// per world load), so a caller can set it every frame for the price of a
    /// comparison. Enhancements{} is the stock client.
    void setEnhancements(const Enhancements& e);
    const Enhancements& getEnhancements() const { return enh_; }
    /// The player's drunk level, PLAYER_BYTES_3 byte 1 capped at 100, / 100
    /// (Wow.exe 0x4F7290). The stock drunk look and steer read it.
    void setDrunkLevel(float level);

private:
    /// Push enh_ to every subsystem that exists, when it or they changed.
    void applyEnhancementsToSubsystems();
    /// The units that get the stock blob shadow this frame (shadows off).
    void updateUnitBlobs();
    /// WOWEE_FREEZE_CLOCK, read once.
    void readFrozenClock();
    /// The "Dust kicked up" enhancement (FootfallDust): made on first use,
    /// fed this frame's dry-ground footfalls, stepped, released a few frames
    /// after it goes off.
    void updateFootfallDust(float fxDt);
    /// The "Corpses settle to the slope" and "Feet on the ground" enhancements
    /// (CharacterRenderer::updateCorpseSettle, updateFootPlacement), which
    /// read the same floors.
    void updateCorpseSettle(float fxDt);
    /// The local lights culled to a screen grid for this frame's camera
    /// (GPUPerFrameData::localLightTiles). Only with the point lights on.
    void buildLocalLightTiles(uint32_t count);
    Enhancements enh_{};
    float drunkLevel_ = 0.0f;
    bool enhDirty_ = true;
    const void* enhTargets_[8] = {};
    void applyMsaaChange();
    bool updateSkyboxLayers();
    uint32_t loadSkyboxModel(const std::string& path);
    VkSampleCountFlagBits pendingMsaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    bool msaaChangePending_ = false;
    void renderShadowPass();
    /// Decides this frame's shadow work (rendering/shadow_cache.hpp): whether
    /// the world map is redrawn and which part, the unit map's projection,
    /// and the matrix the frame's block carries. After the slot's fence.
    void planShadowFrame();
    /// Points this slot's binding 1 (per-frame, reflection and fog sets) at
    /// the front world map, if it is not already.
    void bindWorldShadowMap(uint32_t slot);
    /// What the world map is drawn from, folded into one number: terrain
    /// chunks, buildings, the doodads' content generation.
    [[nodiscard]] uint64_t shadowContentKey() const;
    /// Clears a shadow image and leaves it readable, if it is not yet, for
    /// a frame that draws no casters into it.
    void clearShadowImageOnce(VkImage image, VkImageView view, VkFramebuffer fb, uint32_t size,
                              VkImageLayout& layout, bool& cleared);
    void beginShadowTarget(VkImage image, VkImageView view, VkFramebuffer fb, uint32_t size,
                           VkImageLayout& layout, bool clear);
    void endShadowTarget(VkImage image, VkImageLayout& layout);
    /// The way shadow rays travel (unit, Z clamped downward), as the shadow
    /// projection uses it. Also tells character animation culling which way
    /// an off-screen unit's shadow can reach into view.
    [[nodiscard]] glm::vec3 shadowRayDirection() const;

    std::vector<pipeline::CustomZoneInfo> customZones_;
    pipeline::AssetManager* cachedAssetManager = nullptr;

    // Spell visual effects - owned SpellVisualSystem (extracted from Renderer §4.4)
    std::unique_ptr<SpellVisualSystem> spellVisualSystem_;

    // Post-process pipeline - owns all FSR/FXAA/FSR2 state (extracted §4.3)
    std::unique_ptr<PostProcessPipeline> postProcessPipeline_;

    float indoorFactor_ = 0.0f;           // 0 outdoors .. 1 inside an interior group, eased
    float foliageWindSetting_ = 1.0f;     // the "Foliage and cloth wind" setting
    float windStrength_ = 1.0f;           // setting x weather x outdoors, eased over ~4 s
    float clothWindStrength_ = 1.0f;      // setting x weather, eased: cloth breathes indoors too
    float shadowStrengthSetting_ = 1.0f;  // the "Shadow darkness" setting
    bool shorelineFoam_ = true;           // the "Shoreline foam" setting
    bool waterReflections_ = false;       // "Water reflections": the sky in the water, not stock
    bool waterSplashes_ = false;          // "Splashes, wakes and bubbles": not stock
    uint32_t cameraLiquidType_ = 0;       // LiquidType the camera is in, 0 when not
    bool wetGroundEnabled_ = true;        // the "Wet ground in rain" setting
    float wetness_ = 0.0f;                // 0 dry .. 1 soaked, eased (WetGround); rtParams.w
    float rainRipple_ = 0.0f;             // rain ripples: how hard it rains, eased (rain_ripples.hpp); rtParams.z
    /// The clock "Fade in new scenery" stamps and reads (stream_fade.hpp):
    /// drawn time, each frame's step held to a tenth of a second. A tile
    /// that loads during a hitch - a teleport's reload draws nothing for
    /// seconds - is stamped with the clock as it stood, and then fades in
    /// over the frames that follow rather than having faded unseen.
    float streamFadeClock_ = 0.0f;
    float terrainRelief_ = 1.0f;          // see setTerrainRelief
    bool cameraUnderwater_ = false;       // camera below the water's surface, for the audio
    glm::vec3 interiorAmbient_{0.5f};     // MOHD ambient of the WMO the camera is in
    bool playerIndoors_ = false;  // Cached WMO inside state for macro conditionals
    // The area the player is in, resolved once a frame by updateLiveArea:
    // WMOAreaTable's while the player stands in a WMO group that decides it,
    // the terrain's otherwise.
    bool liveAreaValid_ = false;
    uint32_t liveAreaId_ = 0;
    bool liveAreaFromWmo_ = false;
    std::string liveWmoAreaName_;
    // What the zone events last announced, so a crossing fires once.
    uint32_t zoneEventMapId_ = 0xFFFFFFFFu;
    uint32_t zoneEventZoneId_ = 0;
    uint32_t zoneEventAreaId_ = 0;
    std::string zoneEventWmoName_;
    /// wmoId 0: the player is in no WMO group. Otherwise the root's MOHD
    /// wmoID, the placement's name set and the group's MOGP id.
    void updateLiveArea(uint32_t wmoId, uint16_t nameSet, uint32_t wmoGroupId);
    void fireZoneEvents();
    /// Hand the minimap the WMO the player is in while it is an indoor group
    /// they stand on, so it draws the building's own tiles (wmo_minimap.hpp);
    /// instanceId 0 or WOWEE_WMO_MINIMAP=0 puts the terrain back.
    /// tightGroupIndex is WMORenderer::AreaHit's.
    void updateMinimapWmo(uint32_t instanceId, int32_t tightGroupIndex);
    bool deferredWorldInitEnabled_ = true;
    bool deferredWorldInitPending_ = false;
    uint8_t deferredWorldInitStage_ = 0;
    float deferredWorldInitCooldown_ = 0.0f;

    // Third-person character state
    glm::vec3 characterPosition = glm::vec3(0.0f);
    uint32_t characterInstanceId = 0;
    /// The character's own instance while a morph is drawn in its place; 0
    /// when the character is drawn as itself.
    uint32_t nativeCharacterInstanceId_ = 0;
    float characterYaw = 0.0f;
    // Stock body yaw and look twist of the player model; see
    // getCharacterVisualYaw. The clock and stamp stand in for unit+0xABC
    // (the last input-control change while not turning).
    anim::BodyFacingState bodyFacing_;
    float characterVisualYawDeg_ = 0.0f;
    float bodyFacingClock_ = 0.0f;
    float bodyFacingStamp_ = 0.0f;
    uint32_t bodyFacingControls_ = 0;
    float bodyFacingStep_ = 0.0f;  // radians the body turned this frame (CCW +)

    // Where the player was, for shaders that react to them moving through the
    // world. playerWakePos_ chases characterPosition with a fixed time
    // constant; the lag is what gives brushed-past foliage its springback.
    glm::vec3 playerWakePos_ = glm::vec3(0.0f);
    glm::vec3 prevPlayerPos_ = glm::vec3(0.0f);
    float playerSpeed_ = 0.0f;
    bool playerMotionTracked_ = false;



    // Selection circle + overlay rendering (owned by OverlaySystem)
    std::unique_ptr<OverlaySystem> overlaySystem_;



    // Vulkan frame state
    VkContext* vkCtx = nullptr;
    VkCommandBuffer currentCmd = VK_NULL_HANDLE;
    uint32_t currentImageIndex = 0;

    // Per-frame UBO + descriptors (set 0)
    static constexpr uint32_t MAX_FRAMES = 2;
    VkDescriptorSetLayout perFrameSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool sceneDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet perFrameDescSets[MAX_FRAMES] = {};
    VkBuffer perFrameUBOs[MAX_FRAMES] = {};
    VmaAllocation perFrameUBOAllocs[MAX_FRAMES] = {};
    void* perFrameUBOMapped[MAX_FRAMES] = {};
    GPUPerFrameData currentFrameData{};
    float globalTime = 0.0f;
    /// WOWEE_FREEZE_CLOCK=<seconds>: the scene clock held at that value and
    /// every animation, particle, sky and weather step given a zero delta, so
    /// two screenshots of the same view can be compared pixel for pixel (the
    /// enhancement stock test, docs/enhancements-plan.md 3.3). The camera and
    /// streaming keep the real delta. Negative when not frozen.
    float frozenClock_ = -1.0f;
    bool frozenClockRead_ = false;

    // Per-frame reflection UBO (mirrors camera for planar reflections)
    VkBuffer reflPerFrameUBO = VK_NULL_HANDLE;
    VmaAllocation reflPerFrameUBOAlloc = VK_NULL_HANDLE;
    void* reflPerFrameUBOMapped = nullptr;
    VkDescriptorSet reflPerFrameDescSet[MAX_FRAMES] = {};

    bool createPerFrameResources();
    void destroyPerFrameResources();
    void updatePerFrameUBO();
    void setupWater1xPass();
    void renderReflectionPass();

    // ── Multithreaded secondary command buffer recording ──
    // Indices into secondaryCmds_ arrays
    static constexpr uint32_t SEC_SKY       = 0;  // sky (main thread)
    static constexpr uint32_t SEC_TERRAIN   = 1;  // terrain (worker 0)
    static constexpr uint32_t SEC_WMO       = 2;  // WMO (worker 1)
    static constexpr uint32_t SEC_SELECTION = 3;  // selection circle (main thread)
    static constexpr uint32_t SEC_CHARS     = 4;  // characters (worker 2)
    static constexpr uint32_t SEC_M2        = 5;  // M2 + particles + glow (worker 3)
    static constexpr uint32_t SEC_POST      = 6;  // water + weather + effects (worker 4)
    static constexpr uint32_t SEC_IMGUI     = 7;  // ImGui (main thread, non-FSR only)
    static constexpr uint32_t SEC_WMO_B     = 8;  // second half of WMO (worker 5), see renderWorld
    static constexpr uint32_t SEC_M2_A      = 9;  // first share of M2 opaque (worker 6), before SEC_M2
    static constexpr uint32_t SEC_TERRAIN_A = 10; // first half of terrain (worker 7), before SEC_TERRAIN
    static constexpr uint32_t NUM_SECONDARIES = 11;
    static constexpr uint32_t NUM_WORKERS = 8;

    // Per-worker command pools (thread-safe: one pool per thread)
    VkCommandPool workerCmdPools_[NUM_WORKERS] = {};
    // Main-thread command pool for its secondary buffers
    VkCommandPool mainSecondaryCmdPool_ = VK_NULL_HANDLE;
    // Pre-allocated secondary command buffers [secondaryIndex][frameInFlight]
    VkCommandBuffer secondaryCmds_[NUM_SECONDARIES][MAX_FRAMES] = {};

    bool parallelRecordingEnabled_ = false;  // set true after pools/buffers created
    // WOWEE_PASS_ABLATION: switches one world pass off at a time and reports
    // what the frame did without it. See pass_ablation.hpp for why the GPU's
    // own timestamps cannot answer that on this platform.
    std::unique_ptr<PassAblation> passAblation_;
    bool passAblationReported_ = false;
    bool worldDrawnLastFrame_ = false;
    std::function<void(const char*, float)> stageSink_;
    std::chrono::steady_clock::time_point lastFrameStart_{};
    float lastDeltaTime_ = 0.0f;           // cached for post-process pipeline
    bool createSecondaryCommandResources();
    void destroySecondaryCommandResources();
    VkCommandBuffer beginSecondary(uint32_t secondaryIndex);
    void setSecondaryViewportScissor(VkCommandBuffer cmd);

    // Cached render pass state for secondary buffer inheritance
    VkRenderPass activeRenderPass_ = VK_NULL_HANDLE;
    VkFramebuffer activeFramebuffer_ = VK_NULL_HANDLE;
    VkExtent2D activeRenderExtent_ = {.width = 0, .height = 0};

    // Active character previews for off-screen rendering
    std::vector<CharacterPreview*> activePreviews_;

    bool terrainEnabled = true;
    bool terrainLoaded = false;

    bool ghostMode_ = false;  // set each frame from gameHandler->isPlayerGhost()

    // Render Graph - declarative pass ordering with automatic barriers
    std::unique_ptr<RenderGraph> renderGraph_;
    void buildFrameGraph(game::GameHandler* gameHandler);

    // HiZ occlusion culling - builds depth pyramid each frame
    std::unique_ptr<HiZSystem> hizSystem_;

    // Volumetric fog: a froxel volume built after the shadow pass and read by
    // every world shader through set 0 binding 2.
    std::unique_ptr<VolumetricFog> volumetricFog_;
    // Ray traced lighting. Created with the per-frame resources and destroyed
    // after every renderer that registers geometry with the scene.
    std::unique_ptr<RtScene> rtScene_;
    std::unique_ptr<RtLighting> rtLighting_;
    void writeRtLightingBindings();
    VkExtent2D sceneRenderExtent() const;
    void recordRtLighting(VkImage sceneDepth, VkExtent2D sceneExtent, bool depthIsMsaa);
    bool rtRecordedThisFrame_ = false;
    /// GPU mark for the pass that just closed over the world: the scene pass,
    /// or the water continuation drawn after the refraction copy. Called
    /// right after vkCmdEndRenderPass, which is the only place a mark can go:
    /// the scene pass is begun for secondary buffers.
    void markWorldPassClosed();
    bool waterPassOpen_ = false;
    float volumetricFogDensity_ = 1.0f;
    /// Whether this frame builds the volume, decided where the per-frame block
    /// is written so the block's switch and the dispatch cannot disagree.
    bool volumetricThisFrame_ = false;
    /// The ground the mist lies on, chased toward the ground under the player.
    float fogLayerBase_ = 0.0f;
    bool fogLayerBaseValid_ = false;
    /// The extinction the volume is built with, chased toward what the zone,
    /// the weather and the hour ask for so walking indoors does not switch it.
    float fogExtinction_ = -1.0f;
    void renderVolumetricFog();
    void writeFogVolumeBindings();
    float volumetricFogExtinction() const;

    // Screen recording: the GPU side reads frames back, the recorder encodes.
    std::unique_ptr<ScreenCapture> screenCapture_;
    std::unique_ptr<core::ScreenRecorder> recorder_;
    std::string recordingFailure_;
    void collectRecordedFrame();
    void recordScreenCapture();

    // Screen-space sun shafts, built from the finished frame at the end of
    // endFrame and added in the overlay pass ahead of the interface.
    std::unique_ptr<SunShafts> sunShafts_;
    bool sunShaftsEnabled_ = true;
    /// renderWorld ran this frame. The shafts are built from the world's
    /// picture, and a login screen or a loading screen is not one.
    bool worldDrawnThisFrame_ = false;
    void recordSunShafts();

    // GPU-driven grass: compute cull with atomic compaction feeding an
    // indirect draw, over a population generated from terrain suitability.
    std::unique_ptr<GrassRenderer> grassRenderer_;
    // Where the live population was generated for. Rebuilt when the player
    // leaves it; the generator's lattice is world-anchored, so a rebuild
    // reproduces every blade that is still in range rather than reshuffling.
    glm::vec3 grassWindowCenter_{0.0f};
    bool grassWindowValid_ = false;
    // The window being generated, a bounded number of lattice cells per
    // frame. Small windows still finish inside one; the ones the distance
    // slider allows take as many frames as they take, and upload when done.
    pipeline::GrassPopulationBuilder grassBuilder_;
    glm::vec3 grassBuildCenter_{0.0f};
    // Clearings around placed WMOs and props for the window being built,
    // gathered once per rebuild from the renderers that own the instances.
    pipeline::GrassClearingField grassClearing_;
    void updateGrassPopulation();
public:
    /** Grass density and height, as fractions of the generator's defaults.
     * Set from the settings panel; either changing rebuilds the field. */
    void setGrassScales(float density, float height);
    /** How far out grass draws, in yards. Density thins with distance past
     * the near field, so range costs blades logarithmically, not by area. */
    void setGrassDistance(float yards);
    /** Turn grass on or off. Off is the default: it is new, it costs
     * generation time on the main thread, and turning it off has to release
     * what it was holding rather than merely stop drawing. */
    void setGrassEnabled(bool enabled);
private:
    bool grassEnabled_ = false;
    float grassDensityScale_ = 1.0f;
    float grassHeightScale_ = 1.0f;
    float grassDistance_ = 150.0f;
    // (biome, effectId) -> index into the profile table, built as effects are
    // met. Grass profiles are a blend of five categories crossed with the
    // biome overrides, so distinct ones stay far fewer than the hundreds of
    // ground effects that map onto them.
    std::unordered_map<uint64_t, uint32_t> grassProfileIndex_;
    std::vector<pipeline::GrassProfile> grassProfiles_;
    uint32_t grassProfileFor(uint32_t effectId, uint32_t areaId);
    // The per-zone look table from assets/grass_biomes.json, loaded on first
    // use, and a memo of which biome each area resolved to - resolution walks
    // AreaTable parentage and runs per blade sample without it.
    pipeline::GrassBiomeSet grassBiomes_;
    bool grassBiomesLoaded_ = false;
    std::unordered_map<uint32_t, uint32_t> grassBiomeForArea_;
    // Session totals, reported once at shutdown. The log is bounded and the
    // per-rebuild lines rotate out of it whenever the player stands still for
    // a minute, so the tail is the only place a "check the log" can rely on.
    uint32_t grassRebuilds_ = 0;
    size_t grassLastCount_ = 0;
    double grassWorstGenerateMs_ = 0.0;

    // CPU timing stats (last frame/update).
    double lastUpdateMs = 0.0;
    double lastRenderMs = 0.0;
    double lastCameraUpdateMs = 0.0;
    double lastTerrainRenderMs = 0.0;
    double lastWMORenderMs = 0.0;
    double lastM2RenderMs = 0.0;
};

} // namespace rendering
} // namespace wowee
