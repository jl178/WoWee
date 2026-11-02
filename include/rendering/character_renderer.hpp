#pragma once

#include "rendering/vk_shader.hpp"
#include "rendering/shadow_params.hpp"

#include "pipeline/m2_loader.hpp"
#include "pipeline/blp_loader.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <utility>
#include <future>
#include <functional>
#include <deque>
#include <condition_variable>
#include <mutex>
#include <atomic>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

// Forward declarations
class Camera;
class VkContext;
class VkTexture;

// Enchant visual (glint, glow) attached to a weapon at one of the weapon model's
// own item-visual attachment points.
struct WeaponEffectAttachment {
    uint32_t effectModelId;
    uint32_t effectInstanceId;
    glm::vec3 offset;          // attachment position on the weapon model
};

// Weapon attached to a character instance at a bone attachment point
struct WeaponAttachment {
    uint32_t weaponModelId;
    uint32_t weaponInstanceId;
    uint32_t attachmentId;     // 1=RightHand, 2=LeftHand
    uint16_t boneIndex;
    glm::vec3 offset;
    glm::mat4 localTransform{1.0f}; // sheath/hand orientation after attachment point
    float sheathPush = 0.0f;   // smoothed outward push when the arm swings into the blade
    std::vector<WeaponEffectAttachment> effects;
};

/**
 * Character renderer for M2 models with skeletal animation
 *
 * Features:
 * - Skeletal animation with bone transformations
 * - Keyframe interpolation (linear position/scale, slerp rotation)
 * - Vertex skinning (GPU-accelerated via bone SSBO)
 * - Texture loading from BLP via AssetManager
 */
class CharacterRenderer {
public:
    CharacterRenderer();
    ~CharacterRenderer();

    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout, pipeline::AssetManager* am,
                    VkRenderPass renderPassOverride = VK_NULL_HANDLE,
                    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT);
    void shutdown();
    void clear();  // Remove all models/instances/textures but keep pipelines/pools

    void setAssetManager(pipeline::AssetManager* am) { assetManager = am; }

    bool loadModel(const pipeline::M2Model& model, uint32_t id);

    uint32_t createInstance(uint32_t modelId, const glm::vec3& position,
                           const glm::vec3& rotation = glm::vec3(0.0f),
                           float scale = 1.0f);

    // oneShotReturnAnim: animation to resume (looping) when a non-looping
    // animation finishes; 0 = Stand. Lets NPC one-shot emotes return to their
    // persistent work/state loop instead of idling.
    void playAnimation(uint32_t instanceId, uint32_t animationId, bool loop = true,
                       uint32_t oneShotReturnAnim = 0);
    /// The unit is dead. Locks the instance in its death pose: every
    /// playAnimation but Death/Dead is ignored until clearDeath. A unit that
    /// dies in view (alreadyDead false) plays Death once and holds its last
    /// frame - not restarted when it is already in Death. A unit that is
    /// already dead when it comes into view (alreadyDead true) starts on that
    /// last frame, lying down. Dead (6) stands in for a model with no Death.
    void playDeath(uint32_t instanceId, bool alreadyDead);
    /// Alive again (resurrection, respawn, ghost): lifts the death lock. The
    /// caller then plays Stand or Rise.
    void clearDeath(uint32_t instanceId);
    [[nodiscard]] bool isDeathLocked(uint32_t instanceId) const;
    /// Play a one-shot on the upper body only - the lower spine and all above
    /// it - over whatever the legs are playing (a run, a walk, a stand), the
    /// way the stock client sheathes while moving. Each arm plays its own
    /// anim; 0 or STAND for an arm leaves it following the body underneath.
    /// It outlives playAnimation, fades in and out, and ends by itself.
    /// False when the model has none of the animations.
    bool playUpperBodyAnimation(uint32_t instanceId, uint32_t torsoAnim,
                                uint32_t leftArmAnim, uint32_t rightArmAnim);
    /// The model's weapon hand-off time ($SHL/$SHR) in an animation, in ms;
    /// UINT32_MAX when the model or the sequence has none.
    [[nodiscard]] uint32_t getSheathEventTime(uint32_t instanceId, uint32_t animationId) const;
    /// The model's spell release ($CSL/$CSR/$CST) in an animation: its time
    /// in ms and, when worldPos is given, where that event's point is now.
    /// False when the model or the sequence has none.
    bool getCastReleaseEvent(uint32_t instanceId, uint32_t animationId, uint32_t& timeMs,
                             glm::vec3* worldPos = nullptr);
    /// Length of an animation on this instance's model, in ms (0 if absent).
    [[nodiscard]] uint32_t getAnimationDurationMs(uint32_t instanceId, uint32_t animationId) const;

    void update(float deltaTime, const glm::vec3& cameraPos = glm::vec3(0.0f));
    /// The view the coming frame draws from, and the way shadow rays travel
    /// (zero when shadows are off). update() then skips the skeleton of a unit
    /// that neither the view nor the unit's shadow reaches; its clocks, sound
    /// events and sequence changes still run, and anything that asks for its
    /// bones (attachments, a cross-fade) has them computed on the spot. Never
    /// called, nothing is skipped - the preview renderers rely on that.
    /// WOWEE_CHAR_ANIM_CULL=0 turns the skipping off.
    void setAnimCullView(const glm::mat4& viewProjection, const glm::vec3& shadowRayDir);

    /** Pre-allocate GPU resources (bone SSBOs, descriptors) on main thread before parallel render. */
    void prepareRender(uint32_t frameIndex);
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera);
    void recreatePipelines();
    /// The five main-pass pipelines, which initialize() and
    /// recreatePipelines() both need and each used to describe.
    void buildMainPassPipelines(VkDevice device, VkRenderPass mainPass,
                                VkSampleCountFlagBits samples,
                                wowee::rendering::VkShaderModule& charVert,
                                wowee::rendering::VkShaderModule& charFrag);
    [[nodiscard]] bool initializeShadow(VkRenderPass shadowRenderPass);
    void renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                      const glm::vec3& shadowCenter = glm::vec3(0), float shadowRadius = 1e9f);

    void setInstancePosition(uint32_t instanceId, const glm::vec3& position);
    void setInstanceRotation(uint32_t instanceId, const glm::vec3& rotation);
    /// Look twist: yaw on the SpineLow key bone and, on top of it, the Head
    /// key bone (radians, CCW positive). See animation/body_facing.hpp.
    void setInstanceTorsoYaw(uint32_t instanceId, float deltaYawRad, float headYawRad = 0.0f);
    void moveInstanceTo(uint32_t instanceId, const glm::vec3& destination, float durationSeconds);
    void startFadeIn(uint32_t instanceId, float durationSeconds);
    void setInstanceOpacity(uint32_t instanceId, float opacity);
    [[nodiscard]] const pipeline::M2Model* getModelData(uint32_t modelId) const;
    [[nodiscard]] const pipeline::M2Model* getInstanceModelData(uint32_t instanceId) const;
    void setActiveGeosets(uint32_t instanceId, const std::unordered_set<uint16_t>& geosets);
    /// Opt an instance into the Skin Extra head-detail batch. Only a character
    /// whose type 8 slot has been filled from CharSections should ask for it.
    void setDrawSkinExtra(uint32_t instanceId, bool enabled);

    /// Counts the head-batch diagnostic lines so it stops after a few.
    int headBatchCanaryCount_ = 0;
    void setGroupTextureOverride(uint32_t instanceId, uint16_t geosetGroup, VkTexture* texture);
    void setTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot, VkTexture* texture);
    void clearTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot);
    void setInstanceVisible(uint32_t instanceId, bool visible);
    void removeInstance(uint32_t instanceId);
    bool getAnimationState(uint32_t instanceId, uint32_t& animationId, float& animationTimeMs, float& animationDurationMs) const;
    /// Footfall ($FSD) event times in ms for the sequence the instance is
    /// currently playing; nullptr if the model has none for that sequence.
    [[nodiscard]] const std::vector<uint32_t>* getFootstepEventTimes(uint32_t instanceId) const;
    /// Called from update() on the main thread when a playing sequence passes
    /// one of its sound-making events. position is the instance's
    /// render-space position. eventTag is the event name as
    /// pipeline::m2EventTag makes it: $CSD (soundEntryId is the event's
    /// SoundEntries id), $FSD footfalls, $CSS / $CAH / $AHn / $FDn / $WNG
    /// (soundEntryId 0) - and kAnimEventDeathStart, not an M2 event, when a
    /// Death animation starts (the stock client's SoundDeathID moment).
    using AnimSoundEventCallback = std::function<void(uint32_t instanceId, uint32_t animationId,
                                                      uint32_t eventTag, uint32_t soundEntryId,
                                                      const glm::vec3& position)>;
    static constexpr uint32_t kAnimEventDeathStart = 0x48544544u;  // "DETH"
    void setAnimSoundEventCallback(AnimSoundEventCallback cb) { animSoundEventCallback_ = std::move(cb); }
    /// Whether the sequence the instance is playing now carries an event
    /// with this tag ($CSS, $CAH ...).
    [[nodiscard]] bool currentAnimHasEvent(uint32_t instanceId, uint32_t eventTag) const;
    /// Whether the animation playing on the upper body (playUpperBodyAnimation)
    /// carries an event with this tag; false when none plays there.
    [[nodiscard]] bool upperBodyAnimHasEvent(uint32_t instanceId, uint32_t eventTag) const;
    /// Plays the rest of the whole-body animation at `speed` (1 = as authored);
    /// the next playAnimation starts at 1 again. The stock client slows a
    /// swing that missed to half speed from its blow (Wow.exe 0x735CC0).
    void setAnimationSpeed(uint32_t instanceId, float speed);
    /// Asked whenever a Stand or Ready stance with more than one variation
    /// starts or laps on an instance: true holds it on its first variation,
    /// as the stock client does for a unit that is attacking or casting
    /// (Wow.exe 0x737EF0), so it does not fidget.
    using FirstVariationPredicate = std::function<bool(uint32_t instanceId)>;
    void setFirstVariationPredicate(FirstVariationPredicate p) { firstVariationPredicate_ = std::move(p); }
    [[nodiscard]] bool hasAnimation(uint32_t instanceId, uint32_t animationId) const;
    bool getAnimationSequences(uint32_t instanceId, std::vector<pipeline::M2Sequence>& out) const;
    bool getInstanceModelName(uint32_t instanceId, std::string& modelName) const;
    bool getInstanceBounds(uint32_t instanceId, glm::vec3& outCenter, float& outRadius) const;

    /// How tall this character stands, from its feet to the top of its head,
    /// with its scale applied. The model origin is at the feet.
    bool getInstanceHeight(uint32_t instanceId, float& outHeight) const;

    /// Where one of the skeleton's named bones sits, in model Z with the
    /// instance's scale applied. The key bone ids are WoW's own: 4 is the
    /// lower spine, 6 the head, 7 the jaw.
    ///
    /// A bone is where a thing actually is. A fraction of the bounding box is
    /// where it usually is, which is not the same for a race whose hair is
    /// half the distance from its chin to the top of the box.
    bool getInstanceKeyBonePivotZ(uint32_t instanceId, int32_t keyBoneId, float& outZ) const;
    bool getInstanceFootZ(uint32_t instanceId, float& outFootZ) const;
    bool getInstancePosition(uint32_t instanceId, glm::vec3& outPos) const;

    /// The pose an instance was last updated to: its bone palette (model
    /// space), model matrix and animation clocks. For effects that hang off
    /// the skeleton outside this renderer - a glue scene's particle emitters.
    /// The bone pointer is valid until the next update() or removeInstance().
    bool getInstancePose(uint32_t instanceId, const std::vector<glm::mat4>*& outBones,
                         glm::mat4& outModel, int& outSequenceIndex,
                         float& outAnimTimeMs, float& outGlobalSeqTimeMs) const;

    /** Debug: Log all available animations for an instance */
    void dumpAnimations(uint32_t instanceId) const;

    /** Attach a weapon model to a character instance at the given attachment point. */
    bool attachWeapon(uint32_t charInstanceId, uint32_t attachmentId,
                      const pipeline::M2Model& weaponModel, uint32_t weaponModelId,
                      const std::string& texturePath,
                      const glm::mat4& localTransform = glm::mat4(1.0f));

    /** Detach a weapon from the given attachment point (drops its enchant effects too). */
    void detachWeapon(uint32_t charInstanceId, uint32_t attachmentId);

    // Free a model's GPU buffers and map entry once no instance references it.
    // For per-instance model ids (weapons/effects/player composites, which get
    // a fresh id per attach or spawn) - without this every reload or despawn
    // leaked the model. Do NOT call for displayId-keyed NPC models; those are
    // cached across despawn/respawn on purpose. Buffers are destroyed via the
    // frame-fence deferral path; shared textures stay in the cache.
    void unloadModelIfUnused(uint32_t modelId);

    // Model id an instance renders with (0 if the instance doesn't exist).
    [[nodiscard]] uint32_t getInstanceModelId(uint32_t instanceId) const {
        auto it = instances.find(instanceId);
        return it != instances.end() ? it->second.modelId : 0;
    }

    // The scale an instance draws at (1 if the instance doesn't exist). Read by
    // the selection circle, whose size the stock client scales by the same.
    [[nodiscard]] float getInstanceScale(uint32_t instanceId) const {
        auto it = instances.find(instanceId);
        return it != instances.end() ? it->second.scale : 1.0f;
    }

    /**
     * Attach an enchant visual to the weapon at the given attachment point.
     * visualSlot is the ItemVisuals.dbc slot (0-4), which selects the attachment
     * point on the weapon model the effect hangs from.
     */
    bool attachWeaponEffect(uint32_t charInstanceId, uint32_t attachmentId, uint32_t visualSlot,
                            const pipeline::M2Model& effectModel, uint32_t effectModelId);

    /** Remove all enchant visuals from the weapon at the given attachment point. */
    void detachWeaponEffects(uint32_t charInstanceId, uint32_t attachmentId);

    /** Mark an instance as a scene backdrop: no culling, no character material heuristics. */
    void setInstanceSceneModel(uint32_t instanceId, bool isScene);

    /** Off-screen previews draw their models as flat texture unless this is
     *  set; a glue scene sets it, so the scene and the character standing in
     *  it are lit by the per-frame block (the scene M2's own lights). */
    void setPreviewLit(bool lit) { previewLit_ = lit; }


    /** Get the world-space transform of an attachment point on an instance. */
    bool getAttachmentTransform(uint32_t instanceId, uint32_t attachmentId, glm::mat4& outTransform);

    [[nodiscard]] size_t getInstanceCount() const { return instances.size(); }

    // Normal mapping / POM settings
    void setNormalMappingEnabled(bool enabled) { normalMappingEnabled_ = enabled; }
    void setNormalMapStrength(float strength) { normalMapStrength_ = strength; }
    void setPOMEnabled(bool enabled) { pomEnabled_ = enabled; }
    void setPOMQuality(int quality) { pomQuality_ = quality; }

    // Fog/lighting/shadow are now in per-frame UBO - keep stubs for callers that haven't been updated
    void setFog(const glm::vec3&, float, float) {}
    void setLighting(const float[3], const float[3], const float[3]) {}
    void setShadowMap(VkTexture*, const glm::mat4&) {}
    void clearShadowMap() {}

    // Pre-decoded BLP cache: set before calling loadModel() to skip main-thread BLP decode
    void setPredecodedBLPCache(std::unordered_map<std::string, pipeline::BLPImage>* cache) { predecodedBLPCache_ = cache; }

private:
    std::unordered_map<std::string, pipeline::BLPImage>* predecodedBLPCache_ = nullptr;
    // GPU representation of M2 model
    struct M2ModelGPU {
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VmaAllocation vertexAlloc = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VmaAllocation indexAlloc = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
        uint32_t vertexCount = 0;

        pipeline::M2Model data;  // Original model data
        std::vector<glm::mat4> bindPose;  // Inverse bind pose matrices

        // Tight bind-pose bounds from rendered vertices. M2 header
        // boundingBox/boundingRadius describe collision geometry on many
        // creatures and can cover little more than their feet.
        glm::vec3 visualBoundMin{0.0f};
        glm::vec3 visualBoundMax{0.0f};
        float visualBoundRadius = 0.0f;

        // Textures loaded from BLP (indexed by texture array position)
        std::vector<VkTexture*> textureIds;

        // Cached batch render order sorted by (priorityPlane, materialLayer).
        // Built once at load time - the sort only depends on the model's static
        // batch metadata, so doing it per-instance per-frame in render() was
        // pure overhead.
        std::vector<size_t> sortedBatchIndices;

        // Pre-classified at load time to avoid per-batch string ops in render loop
        bool isKoboldFlame = false;
        bool isSkyBird = false;
    };

    // One bone's local animated pose, before the pivot bracket.
    struct BoneLocalPose {
        glm::vec3 translation{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};
    };

    // Character instance
    struct CharacterInstance {
        uint32_t id;
        uint32_t modelId;

        glm::vec3 position;
        glm::vec3 rotation;
        float scale;
        bool visible = true;  // For first-person camera hiding
        float torsoYawOverrideRad = 0.0f;  // SpineLow (key bone 4) look twist
        float headYawOverrideRad = 0.0f;   // Head (key bone 6) look twist

        // Animation state
        uint32_t currentAnimationId = 0;
        int currentSequenceIndex = -1;  // Index into M2Model::sequences
        // Upper-body overlay (playUpperBodyAnimation): a one-shot on the spine
        // and everything above it, over whatever the legs play, surviving
        // playAnimation. Index 0/1 the arms, 2 the torso; -1 = follows the body.
        int upperSequenceIndex[3] = {-1, -1, -1};
        float upperTime = -1.0f;      // ms into the overlay; < 0 = none
        float animationSpeed = 1.0f;  // setAnimationSpeed; reset by playAnimation
        float upperDurationMs = 0.0f;
        std::vector<int8_t> boneUpper;  // Per bone: 0 left arm, 1 right arm, 2 torso/head, -1 lower body
        float animationTime = 0.0f;
        float globalSequenceTime = 0.0f; // Separate timer for global sequences (accumulates without wrapping at sequence duration)
        bool animationLoop = true;
        uint32_t oneShotReturnAnim = 0; // Anim to resume when a one-shot ends (0 = Stand)
        // Laps the playing variation still owes (its replay range) before the
        // next variation is rolled. See animation/sequence_variation.hpp.
        uint32_t variationLapsLeft = 0;
        bool isDead = false;  // Prevents movement while in death state
        bool deathLocked = false;  // playDeath: nothing but Death/Dead plays until clearDeath
        std::vector<glm::mat4> boneMatrices;  // Current bone transforms

        // Sequence cross-fade, as the stock client does on every switch: the
        // pose the old sequence left the bones in is faded into the new one
        // over the incoming sequence's blendTime. localPose is the pose last
        // computed (after any blend, so an interrupted fade starts from where
        // the bones actually were); blendFromPose is the snapshot being faded out.
        std::vector<BoneLocalPose> localPose;
        std::vector<BoneLocalPose> blendFromPose;
        float blendElapsedMs = 0.0f;
        float blendDurationMs = 0.0f;  // 0 = not blending

        // Geoset visibility - which submesh IDs to render
        // Empty = render all (for non-character models)
        std::unordered_set<uint16_t> activeGeosets;
        /// Draw the Skin Extra (texture type 8) head-detail batch. True only
        /// where the instance has been set up to composite it - the player.
        bool drawSkinExtra = false;

        // Per-geoset-group texture overrides (group → VkTexture*)
        std::unordered_map<uint16_t, VkTexture*> groupTextureOverrides;

        // Per-texture-slot overrides (slot → VkTexture*)
        std::unordered_map<uint16_t, VkTexture*> textureSlotOverrides;

        // Weapon attachments (weapons parented to this instance's bones)
        std::vector<WeaponAttachment> weaponAttachments;

        // Opacity (for fade-in)
        float opacity = 1.0f;
        float fadeInTime = 0.0f;     // elapsed fade time (seconds)
        float fadeInDuration = 0.0f; // total fade duration (0 = no fade)

        // Movement interpolation
        bool isMoving = false;
        glm::vec3 moveStart{0.0f};
        glm::vec3 moveEnd{0.0f};
        float moveDuration = 0.0f;   // seconds
        float moveElapsed = 0.0f;

        // Override model matrix (used for weapon instances positioned by parent bone)
        bool hasOverrideModelMatrix = false;
        glm::mat4 overrideModelMatrix{1.0f};

        // Enchant visual attached to a weapon. Such a model is nothing but the
        // additive FX batches that attached weapons otherwise drop, and it still
        // needs its animation advanced even though its transform comes from the parent.
        bool isEffectModel = false;

        // A scene rather than a character: the glue-screen backdrops. Two things
        // follow. Their origin can sit hundreds of units from their geometry, so
        // culling on it would drop them. And the material heuristics below exist to
        // rescue character textures - applied to a scene they erase it, because
        // Stormwind's walls are DXT5 with an unused alpha channel that the opaque
        // batches must ignore, exactly as the blend mode says.
        bool isSceneModel = false;


        // Bone update throttling for characters outside normal gameplay range.
        uint32_t boneUpdateCounter = 0;
        // Skeleton skipped by the view cull since it was last computed:
        // boneMatrices and localPose describe an earlier time. refreshStalePose
        // brings them up to date for whoever needs them before the next frame.
        bool poseStale = false;
        // Per bone and track (translation, rotation, scale), the key the last
        // sample landed on; a hint for the next search, see m2_track::KeyCursor.
        std::vector<uint32_t> keyCursor;
        const M2ModelGPU* cachedModel = nullptr;  // Avoid per-frame hash lookups

        // Per-instance bone SSBO (double-buffered per frame)
        VkBuffer boneBuffer[2] = {};
        VmaAllocation boneAlloc[2] = {};
        void* boneMapped[2] = {};
        VkDescriptorSet boneSet[2] = {};
        /// Bumped each time boneMatrices is computed; and per frame slot, the
        /// pose that slot's bone buffer holds (0: none yet). See uploadBones.
        uint32_t poseSerial = 1;
        uint32_t uploadedPoseSerial[2] = {};

        /// The batches the shadow pass draws, by the same filter it always
        /// applied (opaque and alpha-keyed only, geosets), worked out once
        /// rather than with a hash lookup per batch per frame. Cleared by
        /// setActiveGeosets, the one input that changes after creation.
        struct ShadowBatchRef {
            uint32_t batch = 0;
            bool alphaKeyed = false;  // blend mode 1: casts its texture's shape
        };
        std::vector<ShadowBatchRef> shadowBatches;
        bool shadowBatchesValid = false;
    };

    /// Copy the instance's bones into this frame slot's bone buffer, unless
    /// the slot already holds this pose. The shadow pass and the main pass
    /// read the same buffer in the same frame; each used to copy the bones
    /// in, and the shadow pass copied them for every unit in range whether or
    /// not its skeleton had moved (off screen it is not posed at all). Off
    /// with WOWEE_CHAR_BONE_REUSE=0: every call copies, as both passes did.
    /// True when it copied.
    bool uploadBones(CharacterInstance& inst, uint32_t frameIndex, int numBones);

    void setupModelBuffers(M2ModelGPU& gpuModel);
    void calculateBindPose(M2ModelGPU& gpuModel);
    void calculateBoneMatrices(CharacterInstance& instance);
    /// Computes the skeleton now if the view cull skipped it.
    void refreshStalePose(CharacterInstance& instance) {
        if (instance.poseStale) calculateBoneMatrices(instance);
    }
    glm::mat4 getBoneTransform(const pipeline::M2Bone& bone, float animTime, float globalSeqTime,
                               int sequenceIndex, const std::vector<uint32_t>& globalSeqDurations);
    /// `cursors`, when given, is three key hints (translation, rotation,
    /// scale) the searches start from and leave updated; the pose is the same.
    static BoneLocalPose sampleBonePose(const pipeline::M2Bone& bone, float animTime, float globalSeqTime,
                                        int sequenceIndex, const std::vector<uint32_t>& globalSeqDurations,
                                        uint32_t* cursors = nullptr);
    static glm::mat4 composeBoneTransform(const pipeline::M2Bone& bone, const BoneLocalPose& pose);
    [[nodiscard]] glm::mat4 getModelMatrix(const CharacterInstance& instance) const;
    void destroyModelGPU(M2ModelGPU& gpuModel, bool defer = false);
    void destroyInstanceBones(CharacterInstance& inst, bool defer = false);

    // Attachment point lookup helper - shared by attachWeapon() and getAttachmentTransform()
    bool findAttachmentBone(uint32_t modelId, uint32_t attachmentId,
                           uint16_t& outBoneIndex, glm::vec3& outOffset) const;

public:
    /**
     * Build a composited character skin texture by alpha-blending overlay
     * layers onto a base skin BLP. Returns the resulting VkTexture*.
     */
    VkTexture* compositeTextures(const std::vector<std::string>& layerPaths);

    /**
     * Build a composited character skin with explicit region-based equipment overlays.
     */
    VkTexture* compositeWithRegions(const std::string& basePath,
                                const std::vector<std::string>& baseLayers,
                                const std::vector<std::pair<int, std::string>>& regionLayers);

    /** Clear the composite texture cache (forces re-compositing on next call). */
    void clearCompositeCache();

    /** Load a BLP texture from MPQ and return VkTexture* (cached). */
    VkTexture* loadTexture(const std::string& path);
    [[nodiscard]] VkTexture* getTransparentTexture() const { return transparentTexture_.get(); }

    /** Replace a loaded model's texture at the given slot. */
    void setModelTexture(uint32_t modelId, uint32_t textureSlot, VkTexture* texture);



private:
    // Create 1×1 fallback textures used when real textures are missing or still loading.
    // Called during both init and clear to ensure valid descriptor bindings at all times.
    void createFallbackTextures(VkDevice device);

    VkContext* vkCtx_ = nullptr;
    VkRenderPass renderPassOverride_ = VK_NULL_HANDLE;
    bool previewLit_ = false;  // see setPreviewLit
    VkSampleCountFlagBits msaaSamplesOverride_ = VK_SAMPLE_COUNT_1_BIT;
    pipeline::AssetManager* assetManager = nullptr;

    // Vulkan pipelines (one per blend mode)
    VkPipeline opaquePipeline_ = VK_NULL_HANDLE;
    VkPipeline alphaTestPipeline_ = VK_NULL_HANDLE;
    VkPipeline alphaPipeline_ = VK_NULL_HANDLE;
    VkPipeline additivePipeline_ = VK_NULL_HANDLE;
    // Whole-instance fades (ghost form, spawn fade-in): alpha blend with depth
    // write kept on, so the faded model still self-occludes instead of showing
    // backfaces and under-armor skin through the body.
    VkPipeline translucentPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;

    // Descriptor set layouts
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;  // set 0 (owned by Renderer)
    VkDescriptorSetLayout materialSetLayout_ = VK_NULL_HANDLE;  // set 1
    VkDescriptorSetLayout boneSetLayout_ = VK_NULL_HANDLE;  // set 2

    // Descriptor pool
    VkDescriptorPool materialDescPools_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    struct MaterialDescriptorKey {
        VkImageView diffuse = VK_NULL_HANDLE;
        VkImageView normal = VK_NULL_HANDLE;
        VkSampler diffuseSampler = VK_NULL_HANDLE;
        VkSampler normalSampler = VK_NULL_HANDLE;
        bool operator==(const MaterialDescriptorKey&) const = default;
    };
    struct MaterialDescriptorKeyHash {
        size_t operator()(const MaterialDescriptorKey& key) const {
            const size_t a = std::hash<VkImageView>{}(key.diffuse);
            const size_t b = std::hash<VkImageView>{}(key.normal);
            const size_t c = std::hash<VkSampler>{}(key.diffuseSampler);
            const size_t d = std::hash<VkSampler>{}(key.normalSampler);
            return a ^ (b << 1) ^ (c << 2) ^ (d << 3);
        }
    };
    std::unordered_map<MaterialDescriptorKey, VkDescriptorSet, MaterialDescriptorKeyHash>
        materialDescriptorCache_[2];
    VkDescriptorPool boneDescPool_ = VK_NULL_HANDLE;
    std::shared_ptr<std::atomic<uint64_t>> boneDescPoolGeneration_ =
        std::make_shared<std::atomic<uint64_t>>(0);
    uint32_t lastMaterialPoolResetFrame_ = 0xFFFFFFFFu;

    // Material UBO ring buffer - pre-allocated per frame slot, sub-allocated each draw
    VkBuffer materialRingBuffer_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VmaAllocation materialRingAlloc_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    void* materialRingMapped_[2] = {nullptr, nullptr};
    uint32_t materialRingOffset_[2] = {0, 0};
    uint32_t materialUboAlignment_ = 256;  // minUniformBufferOffsetAlignment
    static constexpr uint32_t MATERIAL_RING_CAPACITY = 4096;

    // Texture cache
    struct TextureCacheEntry {
        std::unique_ptr<VkTexture> texture;
        std::unique_ptr<VkTexture> normalHeightMap;
        float heightMapVariance = 0.0f;
        size_t approxBytes = 0;
        uint64_t lastUse = 0;
        bool hasAlpha = false;
        bool colorKeyBlack = false;
        bool normalMapPending = false;  // deferred normal map generation
    };
    std::unordered_map<std::string, TextureCacheEntry> textureCache;
    struct NormalMapInfo {
        VkTexture* normalMap = nullptr;
        float heightMapVariance = 0.0f;
    };
    std::unordered_map<VkTexture*, NormalMapInfo> normalMapByTexPtr_;
    struct TextureProperties {
        bool hasAlpha = false;
        bool colorKeyBlack = false;
    };
    std::unordered_map<VkTexture*, TextureProperties> texturePropsByPtr_;
    std::unordered_map<std::string, VkTexture*> compositeCache_;  // key → texture for reuse
    std::unordered_set<std::string> failedTextureCache_;  // negative cache for budget exhaustion
    std::unordered_map<std::string, uint64_t> failedTextureRetryAt_;
    std::unordered_set<std::string> loggedTextureLoadFails_;  // dedup warning logs
    uint64_t textureLookupSerial_ = 0;
    size_t textureCacheBytes_ = 0;
    uint64_t textureCacheCounter_ = 0;
    size_t textureCacheBudgetBytes_ = 1024ull * 1024 * 1024;
    uint32_t textureBudgetRejectWarnings_ = 0;
    std::unique_ptr<VkTexture> whiteTexture_;
    std::unique_ptr<VkTexture> transparentTexture_;
    std::unique_ptr<VkTexture> flatNormalTexture_;

    std::unordered_map<uint32_t, M2ModelGPU> models;
    std::unordered_map<uint32_t, CharacterInstance> instances;
    /// Bumped whenever `instances` gains or loses an entry. An element of an
    /// unordered_map stays where it is until erased, so a list of pointers
    /// into it is good until this moves.
    uint64_t instancesGeneration_ = 0;
    /// The shadow pass's walk: every instance, in the map's order, as a dense
    /// list rather than a walk of the map's nodes. Rebuilt when
    /// instancesGeneration_ moves.
    std::vector<CharacterInstance*> shadowInstances_;
    uint64_t shadowInstancesGeneration_ = ~0ull;
    /// Shadow pass counts under WOWEE_FRAME_PROFILE: instances walked, in
    /// range, dropped by the light-space cull, drawn, bone copies made and
    /// skipped, frames.
    struct ShadowProfile {
        uint64_t walked = 0, inRange = 0, lightCulled = 0, drawn = 0;
        uint64_t boneCopies = 0, boneReuses = 0, frames = 0;
    } shadowProf_;

    uint32_t nextInstanceId = 1;

    // Normal map generation (same algorithm as WMO renderer)
    std::unique_ptr<VkTexture> generateNormalHeightMap(
        const uint8_t* pixels, uint32_t width, uint32_t height, float& outVariance);

    // Background normal map generation - CPU work on thread pool, GPU upload on main thread
    struct NormalMapResult {
        std::string cacheKey;
        std::vector<uint8_t> pixels;  // RGBA normal map output
        uint32_t width, height;
        float variance;
    };
    // Completed results ready for GPU upload (populated by background threads)
    std::mutex normalMapResultsMutex_;
    std::condition_variable normalMapDoneCV_;  // signaled when pendingNormalMapCount_ reaches 0
    std::deque<NormalMapResult> completedNormalMaps_;
    std::atomic<int> pendingNormalMapCount_{0};  // in-flight background tasks

    // Pure CPU normal map generation (thread-safe, no GPU access)
    /// Start deriving a normal/height map for a texture already in the cache.
    /// Called for every surface this renderer draws, whether it came from a
    /// file or was composited in memory.
    bool queueNormalMapGeneration(const std::string& cacheKey,
                                  std::vector<uint8_t> pixels,
                                  uint32_t width, uint32_t height);

    static NormalMapResult generateNormalHeightMapCPU(
        std::string cacheKey, std::vector<uint8_t> pixels, uint32_t width, uint32_t height);
public:
    void processPendingNormalMaps(int budget = 4);
private:

    // Normal mapping / POM settings
    bool normalMappingEnabled_ = true;
    float normalMapStrength_ = 0.8f;
    bool pomEnabled_ = true;
    int pomQuality_ = 1;  // 0=Low(16), 1=Medium(32), 2=High(64)

    // Maximum bones supported
    static constexpr int MAX_BONES = 240;
    uint32_t numAnimThreads_ = 1;
    std::vector<std::future<void>> animFutures_;
    std::vector<std::reference_wrapper<CharacterInstance>> toUpdate_;  // reused across frames
    AnimSoundEventCallback animSoundEventCallback_;
    FirstVariationPredicate firstVariationPredicate_;
    /// The variation of `animationId` to start: its first while
    /// firstVariationPredicate_ holds the instance there, else rolled.
    int pickLoopVariation(uint32_t instanceId, const pipeline::M2Model& model, uint32_t animationId) const;
    // setAnimCullView: the frame's view-projection and shadow ray direction.
    bool animCullValid_ = false;
    glm::mat4 animCullViewProj_{1.0f};
    glm::vec3 animCullShadowDir_{0.0f};

    // Shadow pipeline resources
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadowPipelineLayout_ = VK_NULL_HANDLE;
    /// The set the shadow pass binds. Five separate members before,
    /// built and torn down here and in three other renderers.
    ShadowParamsSet shadowParams_;

    /// Which texture a batch draws with. Shared by the main pass and the shadow
    /// pass, which needs it to cut a silhouette rather than a rectangle.
    [[nodiscard]] VkTexture* resolveBatchTexture(const CharacterInstance& inst,
                                   const M2ModelGPU& gm,
                                   const pipeline::M2Batch& b) const;

    /// Per-batch texture sets for alpha-keyed shadow casters, one pool per
    /// frame in flight and reset at the top of each frame's shadow pass. Same
    /// arrangement M2Renderer uses for its foliage shadows.
    static constexpr uint32_t kShadowTexPoolFrames = 2;
    VkDescriptorPool shadowTexPool_[kShadowTexPoolFrames] = {};
    std::unordered_map<VkImageView, VkDescriptorSet> shadowTexSetCache_;
    VkDescriptorSet shadowTexDescSet(VkTexture* tex, uint32_t frameIndex);
};

} // namespace rendering
} // namespace wowee
