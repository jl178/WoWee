#pragma once

#include "rendering/collision_geometry.hpp"
#include "rendering/frustum.hpp"
#include "rendering/spatial_grid.hpp"
#include "rendering/shadow_params.hpp"

#include "pipeline/m2_loader.hpp"
#include "pipeline/blp_loader.hpp"
#include "pipeline/grass_clearing.hpp"
#include "rendering/m2_model_classifier.hpp"
#include "rendering/m2_particle_step.hpp"
#include "rendering/m2_particle_look.hpp"
#include "rendering/stream_fade.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <array>
#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cmath>
#include <deque>
#include <string>
#include <functional>
#include <optional>
#include <random>
#include <chrono>
#include <future>
#include <algorithm>

namespace wowee {

namespace pipeline {
    class AssetManager;
}

namespace rendering {

class Camera;
class VkContext;
class VkTexture;
class HiZSystem;
class RtScene;

// Ceiling on the bone matrices computed and uploaded for one M2 instance.
// WoW models run well past a hundred bones - the largest shipped model has
// 315 - and a vertex weighted to a bone above this ceiling would read outside
// its instance's range in the mega bone buffer, which is what threw stray
// spikes out of water elementals and other high-bone-count creatures.
inline constexpr uint32_t kMaxBonesPerInstance = 512;

/**
 * GPU representation of an M2 model
 */
struct M2ModelGPU {
    struct BatchGPU {
        VkTexture* texture = nullptr;  // from cache, NOT owned
        VkDescriptorSet materialSet = VK_NULL_HANDLE;  // set 1
        ::VkBuffer materialUBO = VK_NULL_HANDLE;
        VmaAllocation materialUBOAlloc = VK_NULL_HANDLE;
        void* materialUBOMapped = nullptr;  // cached mapped pointer (avoids per-frame vmaGetAllocationInfo)
        uint32_t indexStart = 0;   // offset in indices (not bytes)
        uint32_t indexCount = 0;
        bool hasAlpha = false;
        /// The alpha is a real silhouette, not an atlas leftover.
        bool alphaIsSilhouette = false;
        bool colorKeyBlack = false;
        glm::vec3 tint{1.0f};  ///< the batch's authored colour
        uint16_t textureAnimIndex = 0xFFFF; // 0xFFFF = no texture animation
        uint16_t blendMode = 0;   // 0=Opaque, 1=AlphaKey, 2=Alpha, 3=Add, etc.
        uint16_t materialFlags = 0; // M2 material flags (0x01=Unlit, 0x04=TwoSided, 0x10=NoDepthWrite)
        uint16_t submeshLevel = 0; // LOD level: 0=base, 1=LOD1, 2=LOD2, 3=LOD3
        // Each texture stage's coordinate source, as the vertex shader's
        // texCoordSet push constant takes it: bits 0-1 the first stage,
        // 2-3 the second (m2fx::UvSource: 0 UV0, 1 UV1, 2 sphere env map).
        uint8_t textureUnit = 0;
        // The stock pixel shader and its stage count (m2_effect_select.hpp).
        uint8_t combiner = 1;
        uint8_t texStages = 1;
        VkTexture* texture2 = nullptr;  // the second stage's texture, NOT owned
        /// The second stage's texture transform (index into
        /// textureTransforms), -1 for none.
        int16_t stage2Transform = -1;
        uint8_t tex2Flags = 0;  // the second texture's M2Texture.flags (wrap S/T)
        uint8_t texFlags = 0;     // M2Texture.flags (bit0=WrapS, bit1=WrapT)
        bool lanternGlowHint = false; // Texture/model hints this batch is a glow-card billboard
        bool glowCardLike = false; // Batch likely is a flat emissive card that should be sprite-replaced
        bool preserveGlowMesh = false; // Keep emissive glass/fixture mesh below its glow sprite
        // Forge fire card: the flame/coals/glow batches of a forge, as opposed
        // to the masonry and ironwork the rest of the model is made of.
        bool forgeFireCard = false;
        // A sky model's star-point layer. Suppressed when the client draws its
        // own stars instead: the authored layer is a 256x256 compressed texture
        // magnified across the whole dome, which is soft at any resolution and
        // softer at a high one.
        bool starLayer = false;
        /// A cone of light drawn as geometry, picked out by its texture.
        ///
        /// Per batch rather than per model, because the model that carries the
        /// zeppelin's searchlight also carries its fins, its engine, a bike
        /// and a goblin pedlar - flagging the whole of HordeZepAnimation
        /// would have softened all of them and cut haze holes in the crew.
        bool volumetricBeam = false;
        uint8_t glowTint = 0; // 0=warm, 1=cool, 2=red
        float batchOpacity = 1.0f; // Resolved texture weight opacity (0=transparent, skip batch)
        // The batch's alpha tracks that can change (its M2Color's alpha, and
        // its texture weight through the lookup), as indices into the model's
        // copies below; -1 for none, or for a constant one, which is baked
        // into batchOpacity instead. Every model plays them per frame, as the
        // stock client does: the Judgement hammer is invisible as it falls
        // and shows from 267 to 534 ms, Frost Armor's shell fades in over
        // 134 ms and out from 2.2 s. bakedTrackAlpha is the share of
        // batchOpacity taken from them, now always 1 (kept for the diagnostics).
        int16_t colorAlphaTrack = -1;
        int16_t textureWeightTrack = -1;
        float bakedTrackAlpha = 1.0f;
        bool alphaAnimates = false;  // colorAlphaTrack or textureWeightTrack set
        glm::vec3 center = glm::vec3(0.0f); // Center of batch geometry (model space)
        float glowSize = 1.0f;              // Approx radius of batch geometry

        struct LightBoneAnchor {
            uint16_t bone = 0;
            // Sum of (vertex position * skin weight, skin weight), divided by
            // sampled vertex count. Transforming and summing these reproduces
            // the animated center of the skinned glow geometry.
            glm::vec4 weightedPoint{0.0f};
        };
        std::vector<LightBoneAnchor> lightBoneAnchors;
        uint16_t lightSuspensionBone = UINT16_MAX;
        glm::vec3 lightSuspensionPoint{0.0f};
    };

    ::VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc = VK_NULL_HANDLE;
    ::VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAlloc = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    std::vector<BatchGPU> batches;

    glm::vec3 boundMin;
    glm::vec3 boundMax;
    float boundRadius = 0.0f;
    bool collisionSteppedFountain = false;
    bool collisionSteppedLowPlatform = false;
    bool collisionPlanter = false;
    bool collisionBridge = false;
    bool collisionSmallSolidProp = false;
    bool collisionNarrowVerticalProp = false;
    bool collisionTreeTrunk = false;
    bool collisionNoBlock = false;
    bool collisionStatue = false;
    bool isSmallFoliage = false;    // Small foliage (bushes, grass, plants) - skip during taxi
    bool isInvisibleTrap = false;   // Invisible trap objects (don't render, no collision)
    bool isGroundDetail = false;    // Ground clutter/detail doodads (special fallback render path)
    bool isWaterVegetation = false; // Cattails, reeds, kelp etc. near water (insect spawning)
    bool isFireflyEffect = false;   // Firefly/fireflies M2 (exempt from particle dampeners)
    bool isWaterfall = false;       // Waterfall model (ambient sound + splash particles)
    bool isBrazierOrFire = false;   // Brazier / campfire / bonfire model
    bool isGroundFire = false;      // Ground fire whose halo follows its lowest flame emitter
    bool isTorch = false;           // Wall-mounted or standing torch
    bool isForge = false;           // Smithy forge (contained fire, lights its surroundings)
    AmbientEmitterType ambientEmitterType = AmbientEmitterType::None;

    // Collision mesh with spatial grid (from M2 bounding geometry)
    struct CollisionMesh {
        std::vector<glm::vec3> vertices;
        std::vector<uint16_t> indices;
        uint32_t triCount = 0;

        struct TriBounds { float minZ, maxZ; };
        std::vector<TriBounds> triBounds;

        static constexpr float CELL_SIZE = 4.0f;
        glm::vec2 gridOrigin{0.0f};
        int gridCellsX = 0, gridCellsY = 0;
        std::vector<std::vector<uint32_t>> cellFloorTris;
        std::vector<std::vector<uint32_t>> cellWallTris;

        void build();
        /// The triangles of one of the two cell arrays that a query box
        /// reaches. The two queries below differ only in which they pass.
        void gatherTrisInRange(const std::vector<std::vector<uint32_t>>& cells,
                               float minX, float minY, float maxX, float maxY,
                               std::vector<uint32_t>& out) const;

        void getFloorTrisInRange(float minX, float minY, float maxX, float maxY,
                                 std::vector<uint32_t>& out) const;
        void getWallTrisInRange(float minX, float minY, float maxX, float maxY,
                                std::vector<uint32_t>& out) const;
        [[nodiscard]] bool valid() const { return triCount > 0; }
    };
    CollisionMesh collision;

    std::string name;

    // Skeletal animation data (kept from M2Model for bone computation)
    std::vector<pipeline::M2Bone> bones;
    std::vector<pipeline::M2Sequence> sequences;
    std::vector<uint32_t> globalSequenceDurations;  // Loop durations for global sequence tracks
    bool hasAnimation = false;  // True if any bone has keyframes
    /// Every bone track holds at most one key per sequence (global ones
    /// included) and no bone billboards, so the pose depends on which
    /// sequence is playing and on nothing else - not the clock, not the
    /// camera. Its bones are computed once per sequence, not once a frame.
    bool staticPose = false;
    uint32_t rtMesh = ~0u;      // RtScene mesh, or ~0u when it does not cast
    bool isSmoke = false;       // True for smoke models (UV scroll animation)
    bool isSpellEffect = false;  // True for spell effect models (skip particle dampeners)
    bool isInstancePortal = false; // Instance portal model (spin + glow)
    bool disableAnimation = false; // Keep foliage/tree doodads visually stable
    bool shadowWindFoliage = false; // Apply wind sway in shadow pass for foliage/tree cards
    /// Foliage whose own bones are keyed (durotarpalm01): it plays its authored
    /// sway and takes no shader wind. Still foliage for the shadow pass's alpha
    /// test and for the long draw distance foliage gets.
    bool foliageAnimatesItself = false;
    /// Awning, tent or tarp: flutters along its normal by a per-vertex weight
    /// (the 19th float of each vertex, location 6) that is zero at its frame.
    bool isCanvas = false;
    /// The model's largest extent, which the canvas flutter is a fraction of.
    float canvasExtent = 0.0f;
    bool isHangingCloth = false;    // Banner/flag/tapestry: sways from the end it is held by
    bool isStandingCloth = false;   // ...and that end is the foot of a planted pole
    bool isFoliageLike = false;     // Model name matches foliage/tree/bush/grass etc (precomputed)
    bool isElvenLike = false;       // Model name matches elf/elven/quel (precomputed)
    bool isLanternLike = false;     // Model name matches lantern/lamp/light (precomputed)
    bool isKoboldFlame = false;     // Model name matches kobold+(candle/torch/mine) (precomputed)
    bool isLavaModel = false;       // Model name contains lava/molten/magma (UV scroll fallback)
    /// The model's own point lights (M2Light type 1), what the stock client
    /// lights nearby units, doodads, WMO groups and terrain with
    /// (docs/re/characters.md section 1). Colour is diffuseColor x
    /// diffuseIntensity at the first key; lights whose visibility starts off
    /// are left out.
    struct StockPointLight {
        int16_t bone = -1;
        glm::vec3 position{0.0f};
        glm::vec3 color{0.0f};
    };
    std::vector<StockPointLight> stockPointLights;
    bool isSkyBird = false;         // Flying bird/bat doodad - hide until animation range
    bool isLightBeam = false;       // Lighthouse/light-ray beam - distant bones must keep updating
    bool isVolumetricBeam = false;  // Light drawn as geometry; softened at its silhouette
    /// Play the sequence forward, then backward, rather than looping it.
    ///
    /// A searchlight sweeps one way and then the other. Wrapping its timeline
    /// at the end of the sequence throws the beam back to where it started in
    /// a single frame, which reads as a jump - the light is at one side of the
    /// arc and then, without crossing the middle, at the other. Reversing
    /// instead is what the sweep actually is.
    bool pingPongAnim = false;
    /// Which bones the reversing clock applies to, one byte per bone.
    ///
    /// The searchlight shares HordeZepAnimation with the zeppelin's fins and
    /// propeller, and reversing the whole timeline turned the propeller
    /// backwards for half of every cycle. Only the bones the beam is actually
    /// skinned to run backward; everything else keeps looping forward.
    std::vector<uint8_t> pingPongBones;
    bool isTransportDoodad = false; // Animated ship sail/paddle child
    bool isBoat = false;            // Small boat; bobs if it turns out to be afloat
    bool hasTextureAnimation = false; // True if any batch has UV animation
    bool hasTransparentBatches = false; // True if any batch uses alpha-blend or additive (blendMode >= 2)
    uint8_t availableLODs = 0;  // Bitmask: bit N set if any batch has submeshLevel==N

    // Particle emitter data (kept from M2Model)
    std::vector<pipeline::M2ParticleEmitter> particleEmitters;
    /// How far, in model units, a particle can get from its emitter: travel
    /// over its lifespan plus its size. Sets how far the particles are worth
    /// stepping (m2ParticleMaxDistSq) for vents whose smoke dwarfs the model.
    float particleReach = 0.0f;
    std::vector<VkTexture*> particleTextures;    // Resolved Vulkan textures per emitter
    std::vector<VkDescriptorSet> particleTexSets; // Pre-allocated descriptor sets per emitter (stable, avoids per-frame alloc)
    /// Each emitter's lifetime curves packed as the particle shader reads
    /// them (m2_particle::packCurve, kCurveVec4s vec4s an emitter). Built by
    /// the update on first emission (M2Renderer::ensureParticleCurves) -
    /// before any particle of the model exists for the draw pass to read.
    mutable std::vector<glm::vec4> particleCurves;
    /// Each emitter's float tracks, classified once per model alongside the
    /// curves, so the per-frame setup skips the key search for the constant
    /// ones (see m2_particle::TrackConst).
    struct ParticleTracks {
        m2_particle::TrackConst rate, life, speed, speedVar, vRange, hRange,
                                gravity, areaL, areaW, zSource;
    };
    mutable std::vector<ParticleTracks> particleTracks;

    // Ribbon emitter data (kept from M2Model)
    std::vector<pipeline::M2RibbonEmitter> ribbonEmitters;
    std::vector<VkTexture*> ribbonTextures;       // Resolved texture per ribbon emitter
    std::vector<VkDescriptorSet> ribbonTexSets;   // Descriptor sets per ribbon emitter

    // The alpha tracks the batches name (colorAlphaTrack, textureWeightTrack).
    std::vector<pipeline::M2AnimationTrack> colorAlphaTracks;
    std::vector<pipeline::M2AnimationTrack> textureWeightTracks;

    // Texture transform data for UV animation
    std::vector<pipeline::M2TextureTransform> textureTransforms;
    std::vector<uint16_t> textureTransformLookup;
    std::vector<int> idleVariationIndices;  // Sequence indices for idle variations (animId 0)

    [[nodiscard]] bool isValid() const { return vertexBuffer != VK_NULL_HANDLE && indexCount > 0; }
};

/// A single M2 particle emitted from a particle emitter.
///
/// Position and velocity are in the particle's space: world space, or for an
/// emitter with flag 0x10 the emitter's own frame, placed through its current
/// matrix when drawn - so the particles turn and travel with it, as the stock
/// CParticleEmitter2 keeps them (runtime flag 0x200). Which it is, is the
/// emitter's flag. What the stock draws per particle from its random seed
/// (spin, size variation, random cell, twinkle phase) is drawn once at birth.
struct M2Particle {
    glm::vec3 position;
    float life;                 // current age in seconds
    glm::vec3 velocity;
    float maxLife;              // total lifespan
    glm::vec2 sizeVary{1.0f};   // scaleVary factors (0x979E90)
    float spinBase = 0.0f;      // radians (0x97A130)
    float spinSpeed = 0.0f;     // radians a second, its sign the spin direction
    uint16_t emitterIndex = 0;  // which emitter spawned this
    uint16_t tileIndex = 0;     // random head cell (flag 0x10000)
    uint16_t phase = 0;         // twinkle phase into the random table
    uint16_t pad = 0;
};
static_assert(sizeof(M2Particle) == 56);

/**
 * Instance of an M2 model in the world
 *
 * Aligned to a cache line so the first 64 bytes, which hold everything the
 * per-frame walks over every instance read and write (the cull upload, the
 * verdict scatter and the visibility classification in M2Renderer::render),
 * are always one line. They were 66 bytes at an arbitrary offset, plus
 * hizPrevCulledFrames 500 bytes further on: two to three lines per instance
 * per walk, over 88k instances, three walks a frame. The layout is checked
 * by the static_asserts after the struct.
 */
struct alignas(64) M2Instance {
    uint32_t id = 0;     // Unique instance ID
    uint32_t modelId;
    glm::vec3 position;
    float fade = 1.0f;
    bool cachedHasAnimation = false;
    bool cachedDisableAnimation = false;
    bool cachedIsSmoke = false;
    bool cachedIsGroundDetail = false;
    bool cachedIsInvisibleTrap = false;
    bool cachedIsSkyBird = false;
    bool cachedIsValid = false;
    // Server game object (mailbox, chest, node, door): exempt from the adaptive
    // ambient-doodad distance, which collapses to ~200 units in a city and hid
    // interactable objects long before the server stopped sending them.
    // Set by the spawner; see M2_GAME_OBJECT_MIN_RENDER_DISTANCE.
    bool isGameObject = false;
    glm::vec3 cachedCullCenter{0.0f};              // transformed visual-bounds center
    float cachedEffectiveMaxDistSqFactor = 1.0f;  // multiplied by maxRenderDistanceSq each frame
    float cachedPaddedRadius = 0.0f;              // sphere radius used by the cull compute
    // Result of the most recent GPU cull dispatch that actually covered this
    // instance, matched back by instance ID (never by array index - see
    // cullSubmittedIds_).  Defaults to visible so an instance created after the
    // in-flight dispatch draws immediately instead of waiting ~2 frames for its
    // first cull result.
    uint8_t lastCullVisible = 1;
    // Consecutive frames this instance has been culled (capped at 3), used for
    // the HiZ `previouslyVisible` hysteresis.  Defaults to 2 ("not recently
    // visible") so a freshly spawned instance skips the HiZ test rather than
    // being judged against depth data that never contained it.
    uint8_t hizPrevCulledFrames = 2;
    const M2ModelGPU* cachedModel = nullptr;  // Avoid per-frame hash lookups
    // End of the first cache line.
    //
    // The second and third lines hold what the main pass reads to write a
    // drawn instance's GPU entry (M2Renderer::recordPart): the matrix, then
    // the bones, their slot, the highlight and the flag bits. These were on
    // six lines spread over 600 bytes (matrix at 80, bones at 336, highlight
    // 400, flags 533-543, slot 648), read for every drawn instance.
    glm::mat4 modelMatrix;
    std::vector<glm::mat4> boneMatrices;
    // Mega bone SSBO offset - base bone index for this instance (set per-frame in prepareRender)
    uint32_t megaBoneOffset = 0;
    /// Lit while the player is pressing on this instance: 0 none, 1 full.
    ///
    /// A game object has no selection circle - it is used rather than selected
    /// - so the press is the only thing that can say the click landed on this
    /// one and not the scenery beside it.
    float highlight = 0.0f;
    /// Ground clutter: the terrain normal under it, packed into the GPU
    /// entry's flags (bit 1 set, x and y as bytes in bits 8-15 and 16-23) so
    /// m2.vert lights the piece as the ground it stands on. 0 for the rest.
    int32_t groundNormalFlags = 0;
    // Whether the boat sits on water: -1 not known yet, 0 no, 1 afloat.
    int8_t afloat = -1;
    /// A WMO doodad: its slot in WMORenderer's doodad visibility (drawn only
    /// while its group is, 0x7ac060), ~0u for every other model.
    uint32_t portalSlot = ~0u;
    /// A doodad of an interior WMO group, lit by its MODD colour (0x7c1150):
    /// these two packed RGB colours replace the day/night ambient and
    /// diffuse, and the light comes along stock_wmo_portal's fixed direction.
    bool wmoInteriorLight = false;
    uint32_t wmoAmbientPacked = 0;
    uint32_t wmoDiffusePacked = 0;
    glm::vec3 rotation;  // Euler angles in degrees
    float scale;
    glm::mat4 invModelMatrix;
    // Its RtScene instance and the matrix last given to it; see syncRtScene.
    uint32_t rtInstance = ~0u;
    glm::mat4 rtMatrix{0.0f};
    glm::vec3 worldBoundsMin;
    glm::vec3 worldBoundsMax;

    // Animation state
    float animTime = 0.0f;       // Current animation time (ms)
    float globalSequenceTime = 0.0f; // Independent clock for global animation tracks
    // Scene time the two clocks above were last brought up to. They advance
    // when something reads them - an animated or particle instance each
    // frame, a static one when it is drawn - not for all 70k instances at once.
    double clockStampMs = 0.0;
    float animSpeed = 1.0f;      // Animation playback speed
    int currentSequenceIndex = 0;// Index into sequences array
    float animDuration = 0.0f;   // Duration of current animation (ms)
    // boneMatrices sits after modelMatrix, with the other fields the draw reads.

    // Sequence cross-fade (see beginM2SequenceBlend): the old sequence keeps
    // playing on its own clock and is faded out over the incoming sequence's
    // blendTime. blendDurationMs 0 = not blending.
    int blendFromSequence = -1;
    float blendFromTime = 0.0f;
    float blendElapsedMs = 0.0f;
    float blendDurationMs = 0.0f;

    // Idle variation state
    int idleSequenceIndex = 0;   // Default idle sequence index
    float variationTimer = 0.0f; // Time until next variation attempt (ms)
    bool playingVariation = false;// Currently playing a one-shot variation
    /// Laps the looping variation still owes (its replay range) before the
    /// next one is rolled by frequency (animation/sequence_variation.hpp).
    uint32_t variationLapsLeft = 0;

    /// Stop at the end of the sequence and stay there.
    ///
    /// A door is a pose, not a performance: the server says open or closed and
    /// the model holds that. Without this a one-shot ran to its end and went
    /// back to the idle sequence, which for a door is the closed pose - so an
    /// open door swung open and shut itself, over and over.
    bool holdAtEnd = false;
    /// With holdAtEnd: the sequence to loop once the held one-shot ends, -1 none.
    ///
    /// The game's own state pairs: Open runs once and Opened loops, Close then
    /// Closed, Destroy then Destroyed. A chest's Opened is where its lid
    /// glows or its coins glint; holding Open's last frame loses that.
    int thenLoopSequence = -1;

    // highlight sits after modelMatrix, with the other fields the draw reads.
    /// An alpha its owner sets on the whole model - the sky's crossfade
    /// between zones. Below one the instance is drawn in the blended pass
    /// alone, every layer of it, since an opaque layer cannot fade otherwise.

    // Particle emitter state
    /// Per emitter, what CParticleEmitter2 keeps between frames.
    struct EmitterState {
        glm::mat4 xf{1.0f};          // model * bone * T(position) * Rz90 this frame
        glm::vec3 pos{0.0f};         // xf's translation (+0x1B4)
        glm::vec3 prevPos{0.0f};     // last frame's (+0x1D0)
        bool havePos = false;
        float acc = 0.0f;            // births owed (+0x08)
        float inheritTimer = 0.0f;   // +0x1DC
        glm::vec3 inheritVel{0.0f};  // +0x1E0, flag 0x40
        glm::vec3 followMove{0.0f};  // +0x1F4, flag 0x4000
        uint32_t poolCap = 0;        // +0x30: room for this many
        uint32_t live = 0;           // alive now
        bool wasEmitting = false;    // +0x86, flag 0x8000's edge
        // Tracks this frame (0x82D2F0, fed in by 0x8309C0).
        bool enabled = false;
        float rate = 0.0f, life = 0.0f, speed = 0.0f, speedVar = 0.0f, vRange = 0.0f,
              hRange = 0.0f, gravity = 0.0f, areaL = 0.0f, areaW = 0.0f, zSource = 0.0f;
    };
    std::vector<EmitterState> emitterStates;
    std::vector<M2Particle> particles;
    /// Seconds of particle time skipped while off screen, paid on the next
    /// step; see the particle loop in M2Renderer::update.
    float particleDtDebt = 0.0f;
    /// Off screen or out of range: its particles were dropped and it neither
    /// emits nor steps. particleDtDebt then counts how long it has lain so,
    /// and the step that wakes it warms the emitters up over that time.
    bool particlesDormant = false;
    /// The shadow candidate generation in which this instance moved; see
    /// M2Renderer::shadowMovers_.
    uint32_t shadowMoverStamp = 0;
    /// The static shadow list build that holds this instance as a caster;
    /// see M2Renderer::shadowStaticMark_.
    uint32_t shadowStaticMark = 0;
    /// No new particles or ribbon edges; the live ones run out their lives.
    /// A spell effect at the end of its life is kept this way until they do.
    bool emissionStopped = false;
    /// Spawned by the spell system (a kit or a missile), not placed in the
    /// world. Its particles run at the scene's full range, because the kit's
    /// end waits on them; a placed doodad that merely looks like an effect -
    /// a glowing mushroom, a wisp - is ranged by its size like the rest.
    bool spellVisual = false;
    /// The emitters of a model another renderer draws: a creature, a player,
    /// or an item on one (CharacterRenderer). It has no geometry here; its
    /// owner hands it the model matrix, bones and clocks each frame
    /// (setPosedEmitterPose) and M2Renderer only steps and draws its
    /// particles and ribbons (updatePosedEmitters), so the stock emitter code
    /// serves every model as CM2Model does. Kept out of every list but the
    /// particle one: no animation, collision, shadow, light or grid entry.
    bool posedExternally = false;
    /// Its owner is drawn: when not, its particles and trails are dropped.
    bool posedVisible = false;
    /// The curves with a ParticleColor.dbc override applied, shared by every
    /// instance of the model with the same colours; null for the model's own
    /// (see setInstanceParticleColors).
    std::shared_ptr<const std::vector<glm::vec4>> particleCurveOverride;

    // Ribbon emitter state (CRibbonEmitter, 0x97F940 / 0x980090).
    struct RibbonEdge {
        glm::vec3 bottom{0.0f};  // spine - Y * below: texture v = 0
        glm::vec3 top{0.0f};     // spine + Y * above: v = 1
        glm::vec3 color{1.0f};   // the emitter's colour and alpha when laid
        float alpha = 1.0f;
        float age = 0.0f;        // seconds
    };
    /// One deque of laid edges per ribbon emitter, oldest first.
    std::vector<std::deque<RibbonEdge>> ribbonEdges;
    /// The emitter frames a ribbon keeps: the previous and the current
    /// position and Y and Z axes (scaled, as the matrix has them). Edges laid
    /// in a step run from the one to the other.
    struct RibbonTrack {
        glm::vec3 prevPos{0.0f}, prevY{0.0f}, prevZ{0.0f};
        glm::vec3 curPos{0.0f}, curY{0.0f}, curZ{0.0f};
        bool hasPrev = false;   // +0x160 bit 0: cleared while hidden
        bool stepped = false;   // +0x160 bit 0x10: the first step lays one edge
        float frac = 0.0f;      // +0x1C: part of an edge owed
        RibbonEdge head;        // the live end, at the emitter (not laid)
        bool hasHead = false;
        uint32_t texSlot = 0;   // +0x170: texture cell
    };
    std::vector<RibbonTrack> ribbonTracks;

    // Cached model flags (set at creation to avoid per-frame hash lookups)
    bool cachedHasParticleEmitters = false;
    bool cachedIsInstancePortal = false;
    bool cachedIsLightBeam = false;
    bool cachedIsTransportDoodad = false;
    bool cachedIsBoat = false;
    // afloat and groundNormalFlags sit after modelMatrix, with the other
    // fields the draw reads.
    uint8_t afloatTries = 0;
    float afloatRecheck = 0.0f;
    bool skipCollision = false;    // Fully non-collidable visual/effect instance
    bool skipWallCollision = false; // No invented (bounding-box) wall blocking; an authored collision mesh still blocks
    /// Off for a marker floating over a unit - the quest giver's ! and ? - which
    /// the stock client never lets fall on the ground beneath it.
    bool castsShadow = true;
    float cachedBoundRadius = 0.0f;
    float cachedVisualRadius = 0.0f;               // transformed visual-bounds half diagonal
    // Pre-computed per-instance cull factors (depend only on static flags + scale +
    // bound radius), populated by recomputeCachedCullFactors(). The per-frame SSBO
    // upload just multiplies by the smoothed render distance and packs the rest.
    /// +1 playing forward, -1 playing back. See M2ModelGPU::pingPongAnim.
    float animDir = 1.0f;
    /// The reversing clock, for the bones named in pingPongBones. The ordinary
    /// animTime beside it keeps looping, so the same model can sweep a beam
    /// back and forth while its propeller turns one way.
    float animTimeAlt = 0.0f;
    float portalSpinAngle = 0.0f;  // Accumulated spin angle for portal rotation
    /// "Fade in new scenery": the stream-fade clock when this doodad came in
    /// with its tile (M2Renderer::markStreamedIn), or negative - always, with
    /// the row off - for one that is simply there.
    float streamFadeStart = -1.0f;

    void recomputeCachedCullFactors();

    // Frame-skip optimization (update distant animations less frequently)
    uint8_t frameSkipCounter = 0;
    /// For a staticPose model: the sequence boneMatrices were last computed
    /// for by the update, or -1. See M2ModelGPU::staticPose.
    int16_t posedSequence = -1;
    bool bonesDirty[2] = {false, false};  // Per-frame-index: set when bones recomputed, cleared after upload
    // Mega-bone slot this instance's bones were last uploaded to, per frame
    // index (0 = never uploaded; valid slots start at 1). Lets prepareRender
    // skip the memcpy when the bones are unchanged and still at the same slot.
    uint32_t megaBoneUploadedSlot[2] = {0, 0};

    // Per-instance bone SSBO (double-buffered) - legacy; see mega bone SSBO in M2Renderer
    ::VkBuffer boneBuffer[2] = {};
    VmaAllocation boneAlloc[2] = {};
    void* boneMapped[2] = {};
    VkDescriptorSet boneSet[2] = {};

    // megaBoneOffset sits after modelMatrix, with the other fields the draw reads.

    void updateModelMatrix();
};

// The per-frame walks' fields stay inside the first cache line; see M2Instance.
// offsetof on a type with vector members is conditionally supported, and GCC
// and Clang give the plain layout answer, which is the one wanted here.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
static_assert(alignof(M2Instance) == 64);
static_assert(offsetof(M2Instance, isGameObject) < 64);
static_assert(offsetof(M2Instance, lastCullVisible) < 64);
static_assert(offsetof(M2Instance, hizPrevCulledFrames) < 64);
static_assert(offsetof(M2Instance, cachedModel) + sizeof(void*) <= 64);
// ...and the main pass's per-drawn-instance reads inside the next two.
static_assert(offsetof(M2Instance, modelMatrix) == 64);
static_assert(offsetof(M2Instance, boneMatrices) >= 128);
static_assert(offsetof(M2Instance, afloat) < 192);
static_assert(offsetof(M2Instance, groundNormalFlags) + sizeof(int32_t) <= 192);
#pragma GCC diagnostic pop

/// Whether an instance is filed in M2Renderer's spatial grid, which only the
/// collision queries (floor, walls, camera rays) read. Ground clutter is not:
/// it is collisionNoBlock and the stock client never collides with ground
/// effects, and since clutter went per chunk a 64-yard cell held hundreds of
/// tufts, every one a hash lookup and a cache miss in every query. A model
/// under a detail path that ships its own collision mesh stays filed: the
/// floor query honours authored collision whatever the classifier says.
inline bool m2GridHolds(const M2Instance& inst) {
    if (inst.posedExternally) return false;  // emitters only: nothing to collide with
    return !inst.cachedIsGroundDetail ||
           (inst.cachedModel != nullptr && inst.cachedModel->collision.valid());
}

/**
 * A single smoke particle emitted from a chimney or similar M2 model
 */
struct SmokeParticle {
    glm::vec3 position;
    glm::vec3 velocity;
    float life = 0.0f;
    float maxLife = 3.0f;
    float size = 1.0f;
    float isSpark = 0.0f;  // 0 = smoke, 1 = ember/spark
    uint32_t instanceId = 0;
};

// M2 material UBO - matches M2Material in m2.frag.glsl (set 1, binding 2)
struct M2MaterialUBO {
    int32_t hasTexture;
    int32_t alphaTest;
    int32_t colorKeyBlack;
    float colorKeyThreshold;
    int32_t unlit;
    int32_t blendMode;
    float fadeAlpha;
    float interiorDarken;
    float specularIntensity;
    float emissiveBoost;
    // The batch's authored colour, from the M2's colour track, with any
    // flicker already folded in. Three scalars rather than a vec3 so the
    // std140 rules stay trivial.
    float tintR;
    float tintG;
    float tintB;
    /// Light drawn as geometry - a searchlight cone, a god ray, a shaft
    /// through a window. The fragment shader fades it out where the surface
    /// turns away from the eye, so the mesh's silhouette stops being an edge.
    int32_t volumetricBeam;
    /// A flame card. Fades out over the top of its own model, so the card's
    /// upper edge is not where the fire stops.
    int32_t fireCard;
    /// The stock pixel shader (m2fx::Combiner) and how many texture stages
    /// it reads.
    int32_t combiner;
    int32_t texStages;
};

// M2 params UBO - matches M2Params in m2.vert.glsl (set 1, binding 1)
struct M2ParamsUBO {
    float uvOffsetX;
    float uvOffsetY;
    int32_t texCoordSet;
    int32_t useBones;
};

/**
 * M2 Model Renderer (Vulkan)
 *
 * Handles rendering of M2 models (doodads like trees, rocks, bushes)
 */
class M2Renderer {
public:
    M2Renderer();
    ~M2Renderer();

    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                    pipeline::AssetManager* assets);
    /** Configure this renderer for camera-centered sky M2s before initialize(). */
    void setSkyMode(bool enabled) { skyMode_ = enabled; }
    void shutdown();

    [[nodiscard]] bool hasModel(uint32_t modelId) const;
    bool loadModel(const pipeline::M2Model& model, uint32_t modelId);
    /** Force-remove a model and all its GPU resources. Caller must ensure no instances reference it. */
    void unloadModel(uint32_t modelId);
    /** Mark a loaded model as a spell effect (full-brightness particles, no collision). */
    void markModelAsSpellEffect(uint32_t modelId);

    /// Instances of the same model at the same spot are treated as one placement,
    /// which is right for the static world and wrong for anything whose position
    /// arrives later from a parent. Pass allowPositionDedup=false for a child
    /// instance created at a placeholder position - see the note in
    /// setInstanceTransform on why the placeholder key outlives the placement.
    uint32_t createInstance(uint32_t modelId, const glm::vec3& position,
                            const glm::vec3& rotation = glm::vec3(0.0f),
                            float scale = 1.0f,
                            bool allowPositionDedup = true);
    uint32_t createInstanceWithMatrix(uint32_t modelId, const glm::mat4& modelMatrix,
                                       const glm::vec3& position);

    void update(float deltaTime, const glm::vec3& cameraPos, const glm::mat4& viewProjection);

    /**
     * Render all visible instances (Vulkan)
     */
    /** Pre-allocate GPU resources (bone SSBOs, descriptors) on main thread before parallel render. */
    void prepareRender(uint32_t frameIndex, const Camera& camera);
    /** Dispatch GPU frustum culling compute shader on primary cmd before render pass. */
    void dispatchCullCompute(VkCommandBuffer cmd, uint32_t frameIndex, const Camera& camera);
    /** The main pass on one command buffer: cullForRecording, recordPart(0, 1), recordGlowSprites. */
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera);

    /// The main pass in steps, so it can be recorded into two command buffers
    /// on two threads (Renderer::renderWorld). First the cull and the sorts;
    /// false means there is nothing to record this frame.
    bool cullForRecording(const Camera& camera);
    /// 1, or 2 when the opaque groups are worth cutting in two. Call after
    /// cullForRecording; WOWEE_M2_ONE_WORKER=1 always answers 1.
    uint32_t recordPartsWanted();
    /// Record one part. Part 0 is the opaque groups before the split, the
    /// last part the rest plus the blended pass, so part 0's buffer is
    /// executed first. The parts of one frame may be recorded concurrently.
    void recordPart(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, uint32_t part, uint32_t parts);
    /// After every part has finished: the glow sprites both generated, into
    /// the last part's buffer, and the frame's counts and profile line.
    void recordGlowSprites(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

    /** Gather the nearest authored glow cards as inexpensive scene point lights. */
    uint32_t gatherLocalLights(const glm::vec3& cameraPos,
                               glm::vec4* outPosRadius,
                               glm::vec4* outColorIntensity,
                               uint32_t maxLights) const;

    /// The stock point lights (Torch and lamp light off): the M2 type-1
    /// lights of the doodads around the camera, nearest first, with their
    /// authored colour. Each slot's w radius is a nominal 20 yards for the
    /// shaders that still cull by it (grass, fog); the stock shaders ignore it.
    uint32_t gatherStockPointLights(const glm::vec3& cameraPos,
                                    glm::vec4* outPosRadius,
                                    glm::vec4* outColorIntensity,
                                    uint32_t maxLights) const;

    /// "Fade in new scenery" (Renderer::setEnhancements): doodads that
    /// arrive with a streamed tile dissolve in (stream_fade.glsli) instead of
    /// appearing whole. Off stamps nothing, and every instance draws as stock.
    void setStreamFadeIn(bool on) { streamFadeOn_ = on; }
    /// The renderer's clock for it, set each frame while it is on.
    void setStreamFadeClock(float seconds) { streamFadeClock_ = seconds; }
    /// A doodad the terrain streamer just placed: it fades in from now.
    void markStreamedIn(uint32_t instanceId);

    /// Enhancement switches (Renderer::setEnhancements). Off is stock.
    void setLightFlicker(bool on) { lightFlicker_ = on; }
    void setGlowSprites(bool on) { glowSpritesOn_ = on; }
    void setExtraSmoke(bool on) { extraSmoke_ = on; }

    /// Soft particles (the "Soft particles" row; SoftParticlePass). While
    /// deferred for a frame, the scene pass draws only the opaque and
    /// alpha-key particles - the ones that write depth - and the glow halos'
    /// upload without their draw; renderSoftParticles then draws the blended
    /// ones and the halos in the late pass, reading the scene's depth. Set
    /// before the frame is recorded and left alone until it is.
    void setSoftParticlesDeferred(bool on) { softDeferred_ = on; }
    [[nodiscard]] bool softParticlesDeferred() const { return softDeferred_; }
    /// Build the late pass's pipelines against `latePass` (single sample,
    /// colour only), with the scene depth in `depthLayout`. Once; false while
    /// they cannot be built, and then nothing is deferred.
    bool ensureSoftParticlePipelines(VkRenderPass latePass, VkDescriptorSetLayout depthLayout);
    /// Free them (the row went off, or the pass they were built for went).
    /// Between frames only.
    void releaseSoftParticlePipelines();
    [[nodiscard]] bool hasSoftParticlePipelines() const { return particleSoftPipelines_[2] != VK_NULL_HANDLE; }
    /// The deferred draws, inside the late pass, after the frame's scene
    /// recording has finished.
    void renderSoftParticles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, VkDescriptorSet depthSet);

    /** Set the HiZ system for occlusion culling (Phase 6.3). nullptr disables HiZ. */
    void setHiZSystem(HiZSystem* hiz) { hizSystem_ = hiz; }

    /// Where loaded models register their geometry for the ray traced
    /// lighting. Static doodads only - animated models would cast their
    /// bind pose. Not given to the sky's renderer.
    void setRtScene(RtScene* scene) { rtScene_ = scene; }
    /// Water surface height near a point, for telling a moored boat from a beached one.
    void setWaterHeightQuery(std::function<std::optional<float>(float, float, float)> q) { waterHeightAt_ = std::move(q); }
    /// Bring the scene's instances in line with this renderer's, once a frame
    /// while the lighting is on.
    void syncRtScene();
    void setForceNoCull(bool v) { forceNoCull_ = v; }

    /** Ensure GPU→CPU cull output is visible to the host after a fence wait.
     *  Call after the early compute submission finishes (endSingleTimeCommands). */
    void invalidateCullOutput(uint32_t frameIndex);

    /**
     * Initialize shadow pipeline (Phase 7)
     */
    [[nodiscard]] bool initializeShadow(VkRenderPass shadowRenderPass);
    [[nodiscard]] bool hasShadowPipeline() const { return shadowPipeline_ != VK_NULL_HANDLE; }

    /**
     * Render depth-only pass for shadow casting
     */
    void renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix, float globalTime = 0.0f,
                      const glm::vec3& shadowCenter = glm::vec3(0), float shadowRadius = 1e9f);

    /// The shared wind: `foliage` is setting x weather, eased, zero indoors;
    /// `cloth` the same without the indoor cut. The main pass reads them from
    /// the per-frame playerWake.w and fogColor.w; the shadow pass has no
    /// per-frame buffer of its own and takes them from here.
    void setWindStrength(float foliage, float cloth) {
        windStrength_ = foliage;
        clothWindStrength_ = cloth;
    }

    /**
     * Render M2 particle emitters (point sprites)
     */
    void renderM2Particles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

    /**
     * Render smoke particles from chimneys etc.
     */
    void renderSmokeParticles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

    /**
     * Render M2 ribbon emitters (spell trails / wing effects)
     */
    void renderM2Ribbons(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

    /// A triangle strip drawn with the ribbons, built on the CPU by someone
    /// else: a spell chain effect's bolt (SpellChainEffects, Wow.exe
    /// 0x9AA210) or a weapon's swing trail (0x7E4CE0). verts holds 9 floats
    /// a vertex, the ribbon layout: position, rgb, alpha, uv. gxBlend is the
    /// stock EGxBlend (2 alpha, 3 add, ...).
    struct EffectStrip {
        VkTexture* texture = nullptr;  // null: white
        uint8_t gxBlend = 2;
        std::vector<float> verts;
    };
    /// The strips to draw from now on, until the next call replaces them.
    void setEffectStrips(std::vector<EffectStrip> strips) { effectStrips_ = std::move(strips); }
    /// A texture for an effect strip, wrapping in u (the bolts scroll it).
    VkTexture* loadEffectTexture(const std::string& path) { return loadTexture(path, 0x3); }

    void setInstancePosition(uint32_t instanceId, const glm::vec3& position);
    /// WOWEE_M2_CENSUS: what a model is actually drawn at, said once each.
    void censusInstance(const M2Instance& instance);

    void setInstanceTransform(uint32_t instanceId, const glm::mat4& transform);
    void setInstanceAnimationFrozen(uint32_t instanceId, bool frozen);
    /// Play from the first frame, with bones to match.
    ///
    /// New instances start at a random point so placed doodads don't animate
    /// in lockstep, and take their first bones from a sibling that is already
    /// mid-animation. A spell cast has to start at its beginning instead.
    void restartInstanceAnimation(uint32_t instanceId);
    /// Let the sequence the instance is playing run once and stay on its last
    /// frame instead of starting over (a one-shot spell kit model).
    void holdInstanceAnimationAtEnd(uint32_t instanceId);
    /// Play an animation once and stay on its last frame.
    ///
    /// skipToEnd puts the model there immediately, which is what a door that
    /// was already open when it came into view wants - the swing belongs to
    /// the moment it opens, not to the moment it is first seen.
    ///
    /// thenLoopAnimationId, when the model has it, is looped after the one-shot
    /// instead of holding its last frame (Open -> Opened); with skipToEnd it is
    /// played straight away. Without it, or when the model lacks it, the
    /// one-shot is held as before.
    void setInstanceAnimationHeld(uint32_t instanceId, uint32_t animationId,
                                  bool skipToEnd,
                                  uint32_t thenLoopAnimationId = UINT32_MAX);

    /// Light an instance while it is being pressed on. 0 clears it.
    void setInstanceHighlight(uint32_t instanceId, float amount);
    /// See M2Instance::fade.
    void setInstanceFade(uint32_t instanceId, float alpha);
    /// See M2Instance::emissionStopped.
    void stopInstanceEmission(uint32_t instanceId);
    /// See M2Instance::spellVisual.
    void markInstanceSpellVisual(uint32_t instanceId);
    /// Whether any particle or ribbon edge of the instance is still alive.
    [[nodiscard]] bool instanceHasLiveParticles(uint32_t instanceId) const;

    /// What the owner of a posed emitter instance (M2Instance::posedExternally)
    /// says about it each frame.
    struct EmitterPose {
        glm::mat4 model{1.0f};                       // the owner's model matrix
        const std::vector<glm::mat4>* bones = nullptr;  // its bone palette
        int sequenceIndex = 0;                       // the sequence it plays
        float animTimeMs = 0.0f;
        float globalSeqTimeMs = 0.0f;
        float alpha = 1.0f;                          // model alpha (particles x this)
        bool visible = true;                         // drawn at all
        glm::vec3 cullCenter{0.0f};                  // its bounds, for range and view
        float cullRadius = 0.0f;
    };
    /// An instance that carries only the emitters of a model loaded here
    /// (geometry stripped) and is posed by its owner; 0 when the model has no
    /// emitters or ribbons, or is not loaded.
    uint32_t createPosedEmitterInstance(uint32_t modelId);
    /// False when the instance is gone (a renderer-wide clear): the owner
    /// makes a new one.
    bool setPosedEmitterPose(uint32_t instanceId, const EmitterPose& pose);
    /// ParticleColor.dbc colours for the instance's emitters on colour index
    /// 11-13 (stock CM2Model::SetParticleColor, 0x825410); null restores the
    /// model's own.
    void setInstanceParticleColors(uint32_t instanceId,
                                   const m2_particle::ParticleColorOverride* colors);
    /// Steps the posed emitter instances, after their owners posed them for
    /// this frame and after update(): on the calling thread, never alongside
    /// update().
    void updatePosedEmitters(float deltaTime);
    /// Take the light off whatever has it, whichever instance that was.
    void clearInstanceHighlights();
    /// Set the animation sequence by animation ID (e.g. anim::OPEN, anim::CLOSE).
    /// Finds the first sequence with matching ID. Unfreezes the instance and resets time.
    void setInstanceAnimation(uint32_t instanceId, uint32_t animationId, bool loop = true);
    /// Check if a model instance has a specific animation ID in its sequence table.
    [[nodiscard]] bool hasAnimation(uint32_t instanceId, uint32_t animationId) const;
    [[nodiscard]] float getInstanceAnimDuration(uint32_t instanceId) const;
    /// World-space visual bounding sphere of an instance (center + radius),
    /// used for cursor picking and selection circles. Returns false for an
    /// unknown instance or a degenerate (zero-radius) model.
    bool getInstanceBounds(uint32_t instanceId, glm::vec3& outCenter, float& outRadius) const;

    /// The world-space box of an instance's collision geometry.
    ///
    /// What a lift's deck actually covers, as opposed to how far its origin is
    /// from you. Boarding a generic M2 transport is decided on a radius around
    /// that origin, which is why standing beside an Undercity lift shaft
    /// attaches you to the car.
    bool getInstanceWorldBounds(uint32_t instanceId, glm::vec3& outMin, glm::vec3& outMax) const;

    /// The floor one named instance offers under a point, ignoring every other.
    ///
    /// Asked of a lift car, this is "is its deck under your feet" - which a box
    /// around the car cannot answer, because an axis-aligned box covers the
    /// doorway and the ground just outside it.
    std::optional<float> getInstanceFloorHeight(uint32_t instanceId,
                                                float glX, float glY, float glZ) const;

    /// The id of an instance's only animation, where it has exactly one.
    ///
    /// A door with no OPEN or CLOSE sequence of its own carries one animation
    /// and that animation is the opening. Undercity's lift doors are the case:
    /// one bone, one sequence, id 0, 3333ms.
    [[nodiscard]] std::optional<uint32_t> soleSequenceId(uint32_t instanceId) const;
    /// True while the instance is still live in the renderer. Owners that cache
    /// instance IDs (game objects, transports) use this to notice an instance
    /// that was dropped underneath them - e.g. by a renderer-wide clear - and
    /// respawn it instead of silently addressing a dead handle forever.
    [[nodiscard]] bool hasInstance(uint32_t instanceId) const {
        return instanceIndexById.find(instanceId) != instanceIndexById.end();
    }
    void removeInstance(uint32_t instanceId);
    void removeInstances(const std::vector<uint32_t>& instanceIds);
    /// Mark an instance as a server game object so the adaptive doodad render
    /// distance can't cull it while the server still considers it visible.
    void setInstanceIsGameObject(uint32_t instanceId, bool isGameObject);
    void setSkipCollision(uint32_t instanceId, bool skip);
    void setSkipWallCollision(uint32_t instanceId, bool skip);
    /// Mark an instance as a WMO doodad (WMORenderer::attachDoodad): its
    /// visibility slot and, when its group is an interior one, its light.
    void setInstanceWmoDoodad(uint32_t instanceId, uint32_t slot, bool interiorLight,
                              const glm::vec3& ambient, const glm::vec3& diffuse);
    /// WMORenderer's doodad visibility, by slot; read by the cull. Null: none.
    void setPortalVisibility(const std::vector<uint8_t>* vis) { portalVisibility_ = vis; }
    void setInstanceCastsShadow(uint32_t instanceId, bool casts);
    void clear();
    /** Drop all instances but keep models in GPU memory. Cheap path for the
     *  editor's rebuild loop where the same model is re-instanced repeatedly. */
    void clearInstances();
    void cleanupUnusedModels();
    /// Keep a model resident even while it has no instances. Game object models
    /// are a small, bounded set (one per display ID actually encountered) that
    /// goes instance-free every time the player leaves an area and comes back -
    /// exactly the reap/reload churn seen in the field. Pinning them keeps that
    /// cycle out of the picture entirely; ambient doodads still get reaped.
    void setModelPinned(uint32_t modelId, bool pinned);
    /** Return and clear the model IDs evicted by cleanupUnusedModels() since the
     *  last call. Lets callers that track uploaded models drop stale entries. */
    std::vector<uint32_t> drainReapedModelIds();

    bool checkCollision(const glm::vec3& from, const glm::vec3& to,
                        glm::vec3& adjustedPos, float playerRadius = 0.5f) const;
    std::optional<float> getFloorHeight(float glX, float glY, float glZ, float* outNormalZ = nullptr) const;

    /// Every doodad the floor query would consider here, and what each one
    /// contributes. The WMO side has had debugDumpGroupsAtPosition for this
    /// since the Undercity pull was first chased; the M2 side had nothing, so
    /// a floor that should have come from a lift deck and did not left no
    /// trace at all beyond "m2=-99999".
    void debugDumpFloorCandidatesAt(float glX, float glY, float glZ) const;
    [[nodiscard]] float raycastBoundingBoxes(const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const;
    /// The nearest doodad *collision mesh* triangle along a ray, for the
    /// cursor pick's occlusion. Only models that ship authored collision
    /// count - not ground clutter, not soft or bounding-box-only doodads, not
    /// spell effects - and the test is per triangle, so a tree hides what is
    /// behind its trunk and not what is under its crown. Returns maxDistance
    /// on no hit; `hitName` receives the model's name when there is one.
    [[nodiscard]] float raycastCollisionMeshes(const glm::vec3& origin, const glm::vec3& direction,
                                               float maxDistance, std::string* hitName = nullptr) const;
    void setCollisionFocus(const glm::vec3& worldPos, float radius);

    void resetQueryStats();
    [[nodiscard]] double getQueryTimeMs() const { return queryTimeMs; }
    [[nodiscard]] uint32_t getQueryCallCount() const { return queryCallCount; }

    void recreatePipelines();

    /// Build the nine main-pass pipelines. Called by initialize() and again by
    /// recreatePipelines() after a device loss, which is the reason it exists:
    /// the two used to be separate copies of the same 190 lines.
    bool buildMainPassPipelines(VkDescriptorSetLayout perFrameLayout);

    // Stats
    [[nodiscard]] bool isInitialized() const { return initialized_; }
    [[nodiscard]] uint32_t getModelCount() const { return static_cast<uint32_t>(models.size()); }
    [[nodiscard]] uint32_t getInstanceCount() const { return static_cast<uint32_t>(instances.size()); }
    [[nodiscard]] uint32_t getTotalTriangleCount() const;
    [[nodiscard]] uint32_t getDrawCallCount() const { return lastDrawCallCount; }

    /**
     * Append the 2D footprints of placed props touching the given window,
     * for the grass generator's clearings. Ground detail is skipped - it is
     * the clutter itself - and the footprint radius is capped so a tree
     * clears the ground at its trunk, not under its whole crown.
     */
    void collectGrassClearings(float minX, float minY, float maxX, float maxY,
                               std::vector<pipeline::GrassClearingSource>& out) const;

    // Lighting/fog/shadow are now in per-frame UBO; these are no-ops for API compat
    void setFog(const glm::vec3& /*color*/, float /*start*/, float /*end*/) {}
    void setLighting(const float /*lightDirIn*/[3], const float /*lightColorIn*/[3],
                     const float /*ambientColorIn*/[3]) {}
    void setShadowMap(uint32_t /*depthTex*/, const glm::mat4& /*lightSpace*/) {}
    void clearShadowMap() {}

    /// How far the furthest doodad drawn this frame was. See
    /// Renderer::logViewDistanceDiag.
    [[nodiscard]] float getFurthestDrawnDistance() const { return std::sqrt(furthestDrawnSq_); }
    /// How many particles to emit, as a fraction of what a model asks for -
    /// the game's Particle Density.
    ///
    /// Applied to the authored rate before the floor that holds flames
    /// together, so turning it down thins smoke, dust and spell effects while
    /// a candle keeps enough particles alive to still read as a flame. See the
    /// note in m2_renderer_particles.cpp: below that floor a fire does not get
    /// smaller, it disappears.
    void setParticleDensity(float density) {
        particleDensity_ = std::clamp(density, 0.05f, 1.0f);
    }
    [[nodiscard]] float particleDensity() const { return particleDensity_; }

    void setSuppressBakedStars(bool suppress) { suppressBakedStars_ = suppress; }
    void setInsideInterior(bool inside) { insideInterior = inside; }

    /// Drop the ground clutter from this pass, for the ablation's clutter
    /// phase. Clutter is drawn by the thousand and shares this renderer with
    /// the world's doodads, so switching the whole pass off says what the two
    /// cost together and nothing about which of them it was.
    void setSkipGroundDetail(bool skip) { skipGroundDetail_ = skip; }

    /// Hold the doodads to this many yards, on top of whatever the density
    /// rule and the instance's own size allow. Zero lifts it.
    ///
    /// The ablation's far-doodad phase: these models have no LOD skins, so a
    /// tree a thousand yards off is drawn with every triangle it has to cover
    /// a few pixels. Whether that is where the time goes is a question about
    /// distance alone, and switching the whole pass off cannot answer it.
    void setDoodadDistanceCap(float yards) { doodadDistanceCapYards_ = yards; }

    /// The doodad view distance with that cap applied.
    float cappedViewDistance() const {
        return doodadDistanceCapYards_ > 0.0f
                   ? std::min(viewDistanceAbsolute_, doodadDistanceCapYards_)
                   : viewDistanceAbsolute_;
    }
    void setViewDistance(float distance) {
        viewDistanceRaw_ = std::clamp(distance, 400.0f, 2400.0f);
        // And the distance itself, as a ceiling. The scale multiplies a
        // constant tuned for scene density - 2800 yards where models are
        // sparse - so at 2400 it put doodads 5600 yards out while the terrain
        // and the WMOs both stop at the 2400 the player asked for. Distant
        // trees and buildings then stood on nothing.
        viewDistanceAbsolute_ = viewDistanceRaw_;
        recomputeViewDistanceScale();
    }

    /// How far the world's clutter is drawn, against how far the world is -
    /// the game's Environment Detail.
    ///
    /// View distance is how far there is anything to see; this is how much of
    /// the scenery inside that is worth drawing, and it only ever takes away.
    /// Above 1 it changes nothing: the ceiling above is the terrain's own, and
    /// a doodad past that is a tree standing on nothing, which is the fault
    /// that ceiling was added for.
    /// How far the ground cover is drawn - the game's Ground Clutter Radius.
    ///
    /// Its own distance rather than a share of the doodad one: grass stops
    /// between 70 and 140 yards in the original client while doodads run to
    /// the horizon, and the setting names those yards outright.
    ///
    /// Held to the stock groundEffectDist range. A saved 300 drew - and the
    /// streamer instanced - more than four times the ground of the stock
    /// maximum, all of it walked by the per-instance passes every frame, for
    /// grass a few pixels tall. Zero, which used to mean no cap and so grass
    /// out to the view distance, is the stock maximum too.
    void setGroundDetailDistance(float yards) {
        groundDetailMaxDistance_ = yards > 0.0f ? std::clamp(yards, 40.0f, kMaxGroundDetailDistance)
                                                : kMaxGroundDetailDistance;
    }
    static constexpr float kMaxGroundDetailDistance = 140.0f;
    /// Light a ground clutter instance with the terrain normal under it
    /// rather than its own cards'. See M2Instance::groundNormalFlags.
    void setInstanceGroundNormal(uint32_t instanceId, const glm::vec3& normal);
    [[nodiscard]] float groundDetailDistance() const { return groundDetailMaxDistance_; }

    void setEnvironmentDetail(float detail) {
        environmentDetail_ = std::clamp(detail, 0.25f, 1.5f);
        recomputeViewDistanceScale();
    }
    [[nodiscard]] float environmentDetail() const { return environmentDetail_; }

    [[nodiscard]] std::vector<glm::vec3> getWaterVegetationPositions(const glm::vec3& camPos, float maxDist) const;

    // Pre-decoded BLP cache: set by terrain manager before calling loadModel()
    // so loadTexture() can skip the expensive assetManager->loadTexture() call.
    void setPredecodedBLPCache(std::unordered_map<std::string, pipeline::BLPImage>* cache) { predecodedBLPCache_ = cache; }

private:
    bool initialized_ = false;
    bool insideInterior = false;
    pipeline::AssetManager* assetManager = nullptr;

    // Vulkan context
    VkContext* vkCtx_ = nullptr;

    // Vulkan pipelines (one per blend mode)
    VkPipeline opaquePipeline_ = VK_NULL_HANDLE;       // blend mode 0
    /// Cutout: leaves, ground clutter, anything the fragment shader alpha-tests.
    /// Blend disabled with alpha-to-coverage on, which is what turns the
    /// shader's sharpened alpha into per-sample coverage.
    VkPipeline cutoutPipeline_ = VK_NULL_HANDLE;
    /// The ablation's clutter phase; see setSkipGroundDetail.
    bool skipGroundDetail_ = false;
    /// The ablation's far-doodad phase; see setDoodadDistanceCap.
    float doodadDistanceCapYards_ = 0.0f;
    /// Said once: a foliage batch reached the shadow pass with no texture.
    bool warnedShadowNoTexture_ = false;
    /// See setWindStrength.
    float windStrength_ = 1.0f;
    float clothWindStrength_ = 1.0f;
    /// Foliage casters drawn into the shadow map last frame, for spotting a
    /// caster count that swings while nothing is moving.
    uint32_t lastFoliageCasters_ = 0;
    VkPipeline alphaTestPipeline_ = VK_NULL_HANDLE;     // blend mode 1
    VkPipeline alphaPipeline_ = VK_NULL_HANDLE;         // blend mode 2
    VkPipeline additivePipeline_ = VK_NULL_HANDLE;      // blend mode 4 (SrcAlpha, One)
    // The rest of the stock blend table (0xA453B0): 3 is One + One, 5 is
    // Mod (DstColor, Zero), 6 is Mod2x (DstColor, SrcColor).
    VkPipeline addOnePipeline_ = VK_NULL_HANDLE;
    VkPipeline modPipeline_ = VK_NULL_HANDLE;
    VkPipeline mod2xPipeline_ = VK_NULL_HANDLE;
    /// The main-pass pipeline for a stock M2 blend mode.
    [[nodiscard]] VkPipeline pipelineForBlend(uint16_t blendMode) const;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;

    // Shadow rendering (Phase 7)
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadowPipelineLayout_ = VK_NULL_HANDLE;
    /// The set the shadow pass binds. Five separate members before,
    /// built and torn down here and in three other renderers.
    ShadowParamsSet shadowParams_;
    // Per-frame pools for foliage shadow texture descriptor sets (one per frame-in-flight)
    static constexpr uint32_t kShadowTexPoolFrames = 2;
    VkDescriptorPool shadowTexPool_[kShadowTexPoolFrames] = {};

    // Particle pipelines
    VkPipeline particlePipeline_ = VK_NULL_HANDLE;       // M2 emitter particles
    VkPipeline particleAdditivePipeline_ = VK_NULL_HANDLE; // Additive particle blend
    VkPipelineLayout particlePipelineLayout_ = VK_NULL_HANDLE;
    // The same particles from compact records (see m2_particle_look.hpp):
    // set 0 = perFrame, 1 = particleTex, 2 = the frame's curve buffer.
    // One quad pipeline per emitter blend type (0..6), the stock state for
    // each (blend table 0xA453B0 row 0; depth write for 0 and 1 only).
    VkPipeline particleGpuPipelines_[7] = {};
    VkPipelineLayout particleGpuPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline smokePipeline_ = VK_NULL_HANDLE;           // Smoke particles
    VkPipelineLayout smokePipelineLayout_ = VK_NULL_HANDLE;

    // Ribbon pipelines (additive + alpha-blend)
    VkPipeline ribbonPipeline_ = VK_NULL_HANDLE;          // Alpha-blend ribbons
    VkPipeline ribbonAdditivePipeline_ = VK_NULL_HANDLE;  // Additive ribbons
    VkPipelineLayout ribbonPipelineLayout_ = VK_NULL_HANDLE;
    /// The per-frame set layout initialize() was given. recreatePipelines()
    /// runs long after that call and needs the same one.
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;

    // Descriptor set layouts
    VkDescriptorSetLayout materialSetLayout_ = VK_NULL_HANDLE;  // set 1
    VkDescriptorSetLayout boneSetLayout_ = VK_NULL_HANDLE;      // set 2
    VkDescriptorSetLayout particleTexLayout_ = VK_NULL_HANDLE;  // particle set 1 (texture only)

    // Descriptor pools
    VkDescriptorPool materialDescPool_ = VK_NULL_HANDLE;
    VkDescriptorPool boneDescPool_ = VK_NULL_HANDLE;
    std::shared_ptr<std::atomic<uint64_t>> boneDescPoolGeneration_ =
        std::make_shared<std::atomic<uint64_t>>(0);
    static constexpr uint32_t MAX_MATERIAL_SETS = 16384;
    static constexpr uint32_t MAX_BONE_SETS = 16384;

    // Dummy identity bone buffer + descriptor set for non-animated models.
    // The pipeline layout declares set 2 (bones) and some drivers (Intel ANV)
    // require all declared sets to be bound even when the shader doesn't access them.
    ::VkBuffer dummyBoneBuffer_ = VK_NULL_HANDLE;
    VmaAllocation dummyBoneAlloc_ = VK_NULL_HANDLE;
    VkDescriptorSet dummyBoneSet_ = VK_NULL_HANDLE;

    // Mega bone SSBO - consolidates all per-instance bone matrices into a single buffer per frame.
    // Replaces per-instance bone SSBOs for fewer descriptor binds and enables GPU instancing.
    static constexpr uint32_t MEGA_BONE_MAX_INSTANCES = 4096;
    // Per-instance bone ranges are packed at their model's true bone count
    // rather than at a fixed stride, so the buffer is sized in matrices. The
    // budget matches the old 4096 x 128 slots; models under the old stride
    // now leave room for the ones above it.
    static constexpr uint32_t MEGA_BONE_MATRIX_CAPACITY = MEGA_BONE_MAX_INSTANCES * 128;
    ::VkBuffer megaBoneBuffer_[2] = {};
    VmaAllocation megaBoneAlloc_[2] = {};
    void* megaBoneMapped_[2] = {};
    VkDescriptorSet megaBoneSet_[2] = {};

    // GPU instance data SSBO - per-instance transforms, fade, bones for instanced draws.
    // Shader reads instanceData[push.instanceDataOffset + gl_InstanceIndex].
    struct M2InstanceGPU {
        glm::mat4 model;           // 64 bytes @ offset 0
        glm::vec2 uvOffset;        //  8 bytes @ offset 64
        float fadeAlpha;           //  4 bytes @ offset 72
        // The second texture stage's UV translation as two halves
        // (glm::packHalf2x16). Whether to skin is flags bit 2 now.
        uint32_t uvOffset2 = 0;    //  4 bytes @ offset 76
        int32_t boneBase;          //  4 bytes @ offset 80
        int32_t boneCount;         //  4 bytes @ offset 84 - clamps skinning reads
        float highlight = 0.0f;    //  4 bytes @ offset 88 - pressed-on lift
        int32_t flags = {};        //  4 bytes @ offset 92 - bit 0: afloat (bobs on the water), bit 2: skinned,
                                   //                         bit 3: WMO interior doodad light
        uint32_t wmoAmbient = 0;   //  4 bytes @ offset 96 - packed RGB (packUnorm4x8)
        uint32_t wmoDiffuse = 0;   //  4 bytes @ offset 100
        uint32_t pad0 = 0;         //  4 bytes @ offset 104
        uint32_t pad1 = 0;         //  4 bytes @ offset 108
    };
    // How many instances one frame may hand the GPU, not how many exist. Ground
    // clutter is what fills it: it is drawn by the thousand and every tuft
    // takes a slot of its own, so at 16384 a dwarf standing in Dun Morogh
    // grass was already losing whole models for a frame at a time - which reads
    // as clutter blinking rather than as anything being over budget. Two
    // buffers of 96 bytes a slot, so this costs 6 MB rather than 3.
    static constexpr uint32_t MAX_INSTANCE_DATA = 32768;
    const std::vector<uint8_t>* portalVisibility_ = nullptr;
    VkDescriptorSetLayout instanceSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool instanceDescPool_ = VK_NULL_HANDLE;
    ::VkBuffer instanceBuffer_[2] = {};
    VmaAllocation instanceAlloc_[2] = {};
    void* instanceMapped_[2] = {};
    VkDescriptorSet instanceSet_[2] = {};
    // The shadow pass's casters, one model matrix each, sorted by model so a
    // model's casters are one instanced draw. Same set layout as the
    // instance data above (one storage buffer, vertex stage).
    static constexpr uint32_t MAX_SHADOW_CASTERS = 32768;
    ::VkBuffer casterBuffer_[2] = {};
    VmaAllocation casterAlloc_[2] = {};
    void* casterMapped_[2] = {};
    VkDescriptorSet casterSet_[2] = {};
    VkDescriptorPool casterDescPool_ = VK_NULL_HANDLE;
    // Write cursor into this frame's instance buffer, reset in
    // cullForRecording. Atomic because two parts claim ranges of it at once.
    std::atomic<uint32_t> instanceDataCount_{0};
    /// Claim `count` contiguous instance slots, or as many as fit (none with
    /// allOrNothing when not all do). Returns how many; `base` the first.
    uint32_t claimInstanceSlots(uint32_t count, bool allOrNothing, uint32_t& base);

    // GPU Frustum Culling via Compute Shader
    // Compute shader tests each M2 instance against frustum planes + distance, writes visibility[].
    // CPU reads back visibility to build sortedVisible_ without per-instance frustum/distance tests.
    struct CullInstanceGPU {        // matches CullInstance in m2_cull.comp.glsl (32 bytes, std430)
        glm::vec4 sphere;           // xyz = world position, w = padded radius
        float effectiveMaxDistSq;   // adaptive distance cull threshold
        uint32_t flags;             // bit 0 = valid, bit 1 = smoke, bit 2 = invisibleTrap
        float _pad[2] = {};
    };
    struct CullUniformsGPU {        // matches CullUniforms in m2_cull_hiz.comp.glsl (std140)
        glm::vec4 frustumPlanes[6]; // xyz = normal, w = distance         (96 bytes)
        glm::vec4 cameraPos;        // xyz = camera position, w = maxPossibleDistSq (16 bytes)
        uint32_t instanceCount;     //                                    (4 bytes)
        uint32_t hizEnabled;        // 1 = HiZ occlusion active           (4 bytes)
        uint32_t hizMipLevels;      // mip levels in HiZ pyramid          (4 bytes)
        uint32_t _pad2 = {};        //                                    (4 bytes)
        glm::vec4 hizParams;        // x=pyramidW, y=pyramidH, z=nearPlane, w=unused (16 bytes)
        glm::mat4 viewProj;         // current frame view-projection                 (64 bytes)
        glm::mat4 prevViewProj;     // previous frame VP for HiZ reprojection        (64 bytes)
    };                              // Total: 272 bytes
    static constexpr uint32_t MAX_CULL_INSTANCES = 24576;
    VkPipeline cullPipeline_ = VK_NULL_HANDLE;           // frustum-only (fallback)
    VkPipeline cullHiZPipeline_ = VK_NULL_HANDLE;        // frustum + HiZ occlusion
    VkPipelineLayout cullPipelineLayout_ = VK_NULL_HANDLE;  // frustum-only layout (set 0)
    VkPipelineLayout cullHiZPipelineLayout_ = VK_NULL_HANDLE; // HiZ layout (set 0 + set 1)
    VkDescriptorSetLayout cullSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool cullDescPool_ = VK_NULL_HANDLE;
    VkDescriptorSet cullSet_[2] = {};               // double-buffered
    ::VkBuffer cullUniformBuffer_[2] = {};           // frustum planes + camera + HiZ params (UBO)
    VmaAllocation cullUniformAlloc_[2] = {};
    void* cullUniformMapped_[2] = {};
    ::VkBuffer cullInputBuffer_[2] = {};             // per-instance bounding sphere + flags (SSBO)
    VmaAllocation cullInputAlloc_[2] = {};
    void* cullInputMapped_[2] = {};
    ::VkBuffer cullOutputBuffer_[2] = {};            // uint visibility[] (SSBO, host-readable)
    VmaAllocation cullOutputAlloc_[2] = {};
    void* cullOutputMapped_[2] = {};

    // HiZ occlusion culling (Phase 6.3) - optional, driven by Renderer
    HiZSystem* hizSystem_ = nullptr;

    RtScene* rtScene_ = nullptr;
    // RtScene instances this renderer owns, and per RtScene instance id the
    // sync generation that last saw it and the mesh it places.
    std::vector<uint32_t> rtOwned_;
    std::vector<uint64_t> rtSeen_;
    std::vector<uint32_t> rtMeshOf_;
    uint64_t rtSyncGeneration_ = 0;
    void registerRtModel(M2ModelGPU& gpuModel, const pipeline::M2Model& model);
    void releaseRtModel(M2ModelGPU& gpuModel);

    // Previous frame's view-projection for temporal reprojection in HiZ culling.
    // Stored each frame so the cull shader can project into the same screen space
    // as the depth buffer the HiZ pyramid was built from.
    glm::mat4 prevVP_{1.0f};

    // Instance IDs, in dispatch order, for the cull results a frame slot holds.
    //
    // The visibility SSBO is read back one full slot cycle after it is written:
    // dispatchCullCompute() records a dispatch into this frame's command buffer,
    // and render() reads the mapped output of the dispatch recorded on the SAME
    // slot ~2 frames earlier.  Array indices are not stable across that gap -
    // createInstance() appends and removeInstance() swap-removes, so slot i can
    // easily describe a different object by the time it is read.  Recording the
    // IDs lets render() match each result back to the instance it was computed
    // for.  cullSubmittedIds_ is the dispatch just recorded (results not ready);
    // cullReadableIds_ is the previous one, matching the mapped data now.
    std::vector<uint32_t> cullSubmittedIds_[2];
    std::vector<uint32_t> cullReadableIds_[2];

    // Dynamic ribbon vertex buffer (CPU-written triangle strip)
    static constexpr size_t MAX_RIBBON_VERTS = 8192;  // 9 floats each
    ::VkBuffer ribbonVB_ = VK_NULL_HANDLE;
    VmaAllocation ribbonVBAlloc_ = VK_NULL_HANDLE;
    void* ribbonVBMapped_ = nullptr;

    // Dynamic particle buffers
    ::VkBuffer smokeVB_ = VK_NULL_HANDLE;
    VmaAllocation smokeVBAlloc_ = VK_NULL_HANDLE;
    void* smokeVBMapped_ = nullptr;
    // The GPU path's buffers, one of each per frame in flight: the particle
    // records (vertex buffer) and the curves they index (storage buffer,
    // bound as set 2 through instanceSetLayout_'s one-SSBO layout).
    ::VkBuffer particleRecordBuf_[2] = {};
    VmaAllocation particleRecordAlloc_[2] = {};
    void* particleRecordMapped_[2] = {};
    ::VkBuffer particleCurveBuf_[2] = {};
    VmaAllocation particleCurveAlloc_[2] = {};
    void* particleCurveMapped_[2] = {};
    VkDescriptorPool particleCurveDescPool_ = VK_NULL_HANDLE;
    VkDescriptorSet particleCurveSet_[2] = {};
    // Dedicated glow sprite vertex buffer (separate from particle VB to avoid data race)
    static constexpr size_t MAX_GLOW_SPRITES = 8000;
    ::VkBuffer glowVB_ = VK_NULL_HANDLE;
    VmaAllocation glowVBAlloc_ = VK_NULL_HANDLE;
    void* glowVBMapped_ = nullptr;

    std::unordered_map<uint32_t, M2ModelGPU> models;
    // Grace period for model cleanup: track when a model first became instanceless.
    // Models are only evicted after 60 seconds with no instances.
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> modelUnusedSince_;
    std::unordered_set<uint32_t> pinnedModelIds_;
    // Model IDs evicted by cleanupUnusedModels() since the last drain. Owners that
    // cache "already uploaded" model IDs (e.g. TerrainManager) reconcile against
    // this so a reaped model is re-loaded instead of skipped as a stale hit.
    std::vector<uint32_t> reapedModelIds_;
    std::vector<M2Instance> instances;

    // O(1) dedup: key = (modelId, quantized x, quantized y, quantized z) → instanceId
    struct DedupKey {
        uint32_t modelId;
        int32_t qx, qy, qz; // position quantized to 0.1 units
        bool operator==(const DedupKey& o) const {
            return modelId == o.modelId && qx == o.qx && qy == o.qy && qz == o.qz;
        }
    };
    struct DedupHash {
        size_t operator()(const DedupKey& k) const {
            size_t h = std::hash<uint32_t>()(k.modelId);
            h ^= std::hash<int32_t>()(k.qx) * 2654435761u;
            h ^= std::hash<int32_t>()(k.qy) * 40503u;
            h ^= std::hash<int32_t>()(k.qz) * 12289u;
            return h;
        }
    };
    std::unordered_map<DedupKey, uint32_t, DedupHash> instanceDedupMap_;

    uint32_t nextInstanceId = 1;
    uint32_t lastDrawCallCount = 0;
    size_t modelCacheLimit_ = 6000;
    uint32_t modelLimitRejectWarnings_ = 0;

    VkTexture* loadTexture(const std::string& path, uint32_t texFlags = 0);
    std::unordered_map<std::string, pipeline::BLPImage>* predecodedBLPCache_ = nullptr;

    struct TextureCacheEntry {
        std::unique_ptr<VkTexture> texture;
        size_t approxBytes = 0;
        uint64_t lastUse = 0;
    };
    std::unordered_map<std::string, TextureCacheEntry> textureCache;
    struct TextureProperties {
        bool hasAlpha = false;
        bool alphaIsSilhouette = false;
        bool colorKeyBlack = false;
    };
    std::unordered_map<VkTexture*, TextureProperties> texturePropsByPtr_;
    size_t textureCacheBytes_ = 0;
    uint64_t textureCacheCounter_ = 0;
    size_t textureCacheBudgetBytes_ = 2048ull * 1024 * 1024;
    /// Make room for `bytesNeeded` by dropping textures nothing is looking at,
    /// oldest first. Returns what it actually freed.
    ///
    /// The cache hands out raw VkTexture* and a model's batches, particle
    /// emitters and ribbons all keep one, so age alone cannot decide: a
    /// referenced texture would leave those pointers dangling whatever
    /// lastUse says about it. What model unloading leaves behind is exactly
    /// what this collects.
    size_t evictUnreferencedTextures(size_t bytesNeeded);

    std::unordered_set<std::string> failedTextureCache_;
    /// Keys refused for want of budget rather than because the file is bad.
    /// Cleared when eviction frees something, so they are retried at once
    /// instead of waiting out a timer set when the cache was full.
    std::unordered_set<std::string> budgetRejected_;
    std::unordered_map<std::string, uint64_t> failedTextureRetryAt_;
    std::unordered_set<std::string> loggedTextureLoadFails_;
    uint64_t textureLookupSerial_ = 0;
    uint32_t textureBudgetRejectWarnings_ = 0;
    std::unique_ptr<VkTexture> whiteTexture_;
    std::unique_ptr<VkTexture> glowTexture_;
    VkDescriptorSet glowTexDescSet_ = VK_NULL_HANDLE;  // cached glow texture descriptor (allocated once)

    // Optional query-space culling for collision/raycast hot paths.
    CollisionFocus collisionFocus;

    void rebuildSpatialIndex();
    void gatherCandidates(const glm::vec3& queryMin, const glm::vec3& queryMax, std::vector<size_t>& outIndices) const;

    SpatialGrid spatialGrid;
    std::unordered_map<uint32_t, size_t> instanceIndexById;
    /// The generation gatherCandidates' per-thread cache keys on. Replaced
    /// (from nextGridLayoutGen) whenever its answer could change: an id filed
    /// in or out of a cell, or an id given a different index. A move that
    /// stays inside its cells changes none of those and keeps it. An empty
    /// answer is cached like any other; only the opt-in scan on a miss
    /// (WOWEE_COLLISION_SCAN_ON_MISS) is not.
    std::atomic<uint64_t> candidateLayoutGen_{nextGridLayoutGen()};
    void noteCandidateLayoutChanged() {
        candidateLayoutGen_.store(nextGridLayoutGen(), std::memory_order_relaxed);
    }
    // Instance to copy bone matrices from when spawning another of the same
    // model. Finding one by scanning every instance is O(n) per spawn, which
    // became 18ms for a single createInstance once a zone held tens of
    // thousands of them.
    std::unordered_map<uint32_t, uint32_t> boneSeedInstanceByModel_;
    // Collision scratch buffers are thread_local (see m2_renderer.cpp) for thread-safety.

    // Collision query profiling - atomic because getFloorHeight is dispatched
    // on async threads from camera_controller while the main thread reads these.
    mutable std::atomic<double> queryTimeMs{0.0};
    mutable std::atomic<uint32_t> queryCallCount{0};

    // Persistent render buffers (avoid per-frame allocation/deallocation)
    struct VisibleEntry {
        uint32_t index;
        uint32_t modelId;
        float distSq;
        float effectiveMaxDistSq;
    };
    std::vector<VisibleEntry> sortedVisible_;  // Reused each frame
    std::vector<VisibleEntry> transparentVisible_; // Only models needing pass 2
    struct GlowSprite {
        glm::vec3 worldPos;
        glm::vec4 color;
        float size;
    };
    std::vector<GlowSprite> glowSprites_;  // Reused each frame; part 0's, then all of them
    std::vector<GlowSprite> glowSpritesB_; // The second part's, until recordGlowSprites

    // The main pass between cullForRecording and recordGlowSprites.
    struct alignas(64) RecordCounts {  // a line each: the parts count concurrently
        uint32_t draws = 0;
        uint32_t skyOpaque = 0;
        uint32_t skyTransparent = 0;
    };
    RecordCounts recordCounts_[2];          // per part, summed by recordGlowSprites
    size_t opaqueSplit_ = 0;                // first sortedVisible_ entry of the second part
    std::vector<std::pair<size_t, uint32_t>> splitGroups_;  // scratch for recordPartsWanted
    uint32_t recordFrameIndex_ = 0;
    glm::vec3 recordCamPos_{0.0f};
    float recordLavaSeconds_ = 0.0f;
    std::chrono::steady_clock::time_point recordT0_, recordT1_, recordT2_;  // profile

    // Shadow-pass texture descriptor cache (reused each frame, cleared via pool reset)
    std::unordered_map<VkImageView, VkDescriptorSet> shadowTexSetCache_;
    /// The casters this frame, grouped by model: [0] solid, [1] foliage.
    /// Only the movers' now; the rest are kept in shadowStaticCasters_.
    ///
    /// The shadow pass used to walk every instance in the world twice - once
    /// per pass - and draw them in whatever order they happened to sit in,
    /// rebinding the vertex and index buffers whenever consecutive instances
    /// came from different models, and rebinding a batch's texture once per
    /// instance rather than once per batch. Culling once into these and
    /// sorting them by model turns both into once per model.
    ///
    /// Kept as members so a frame's worth of casters costs no allocation.
    std::vector<std::pair<uint32_t, uint32_t>> shadowCasters_[2];

    /// The instances that could cast into the shadow disc, as (index, id),
    /// gathered with a margin and refreshed a few times a second rather than
    /// walking all 85k instances every frame. The exact caster tests still
    /// run on every candidate every frame against its current position; this
    /// only decides who is asked. See renderShadow.
    std::vector<std::pair<uint32_t, uint32_t>> shadowCandidates_;
    glm::vec3 shadowCandCenter_{0.0f};
    float shadowCandRadius_ = -1.0f;
    uint32_t shadowCandLayoutGen_ = ~0u;
    std::chrono::steady_clock::time_point shadowCandBuiltAt_{};
    /// Bumped by whatever reorders `instances` wholesale (rebuildSpatialIndex),
    /// which the candidate list cannot follow. Single removals patch the list
    /// in place, creations append to it (noteShadowCandidate) and moves are
    /// tracked in shadowMovers_, so none of those rebuild it any more - they
    /// were most of the ~8 rebuilds a second, each a walk of 85k instances.
    uint32_t casterLayoutGen_ = 0;
    /// Which list build is current; an instance whose shadowMoverStamp
    /// matches has moved since and is asked through shadowMovers_ instead.
    uint32_t shadowCandStamp_ = 1;
    /// Ids of instances moved since the list was built. A mover is tested
    /// every frame wherever it now is, so no margin has to cover its travel.
    std::vector<uint32_t> shadowMovers_;
    /// shadowMovers_ resolved to (index, id) for this frame; reused storage.
    std::vector<std::pair<uint32_t, uint32_t>> shadowMoverPairs_;

    /// The candidates' share of the casters - every candidate that has not
    /// moved - culled and sorted by model as (modelId, index), [0] solid and
    /// [1] foliage, and kept from frame to frame while nothing the cull reads
    /// has changed. Only the movers are culled every frame.
    ///
    /// The cull of an unmoved candidate reads its position, flags, model and
    /// scale, which change only through the paths that bump shadowStaticGen_,
    /// and the light-space matrix, centre and radius, which are compared
    /// exactly. So a kept list is the list the cull would build again, and
    /// the matrices it put at the front of a frame slot's caster buffer are
    /// still the instances' matrices. WOWEE_SHADOW_CACHE=0 rebuilds it every
    /// frame. See renderShadow.
    std::vector<std::pair<uint32_t, uint32_t>> shadowStaticCasters_[2];
    /// A model's run in shadowStaticCasters_[b], [begin, end), and where its
    /// matrices start in the caster buffer.
    struct ShadowCasterGroup { uint32_t begin, end, base; };
    std::vector<ShadowCasterGroup> shadowStaticGroups_[2];
    /// Matrices the static groups take at the front of the caster buffer.
    uint32_t shadowStaticCount_ = 0;
    glm::mat4 shadowStaticLightSpace_{0.0f};
    glm::vec3 shadowStaticCenter_{0.0f};
    float shadowStaticRadius_ = -1.0f;
    /// Bumped by everything that can change the static casters: a list
    /// build, a static caster turned mover, a removal that takes one of
    /// them, castsShadow, a model marked as a spell effect or unloaded.
    uint32_t shadowStaticGen_ = 0;
    uint32_t shadowStaticBuiltGen_ = ~0u;
    enum ShadowGenSource : uint8_t {
        kShadowGenRebuild, kShadowGenMover, kShadowGenRemoval,
        kShadowGenCastsShadow, kShadowGenModel, kShadowGenLayout, kShadowGenCount
    };
    void bumpShadowStatic(ShadowGenSource source) {
        ++shadowStaticGen_;
        ++updateProf_.shadowGenBumps[source];
    }
    /// Numbers the static list builds; a caster on the current list carries
    /// it in M2Instance::shadowStaticMark, so a first move of anything else
    /// leaves the list alone.
    uint32_t shadowStaticMark_ = 1;
    /// The candidates whose verdict depends on the centre or the matrix,
    /// with how far each is from turning (see renderShadow's CullSlack) and
    /// the verdict (-1 none, 0 solid, 1 foliage). A frame whose centre and
    /// matrix moved by less than every probe's slack keeps the list; probes
    /// within it are re-tested, and the list is kept if none turned.
    struct ShadowStaticProbe {
        uint32_t index, id;
        float slackDist, slackClip;
        int8_t bucket;
    };
    std::vector<ShadowStaticProbe> shadowStaticProbes_;
    std::vector<std::pair<uint32_t, uint32_t>> shadowRecheck_[2];
    /// Which build of the static list is current, and which one each frame
    /// slot's caster buffer holds at its front (0 = none).
    uint64_t shadowStaticEpoch_ = 0;
    uint64_t casterSlotEpoch_[2] = {0, 0};
    /// A removal of `idx` with `lastIdx` moved into its place: a static
    /// caster removed means a rebuild; otherwise the moved one's index is
    /// patched (on the list and the probes), and its matrix in the caster
    /// buffers is still right.
    void patchShadowStaticRemoval(size_t idx, size_t lastIdx);
    /// The margin the current list was gathered with.
    float shadowCandMargin_ = 0.0f;
    /// A new instance at `idx`: if it could reach the disc the list was
    /// built for, ask it every frame as a mover until the next build.
    void noteShadowCandidate(size_t idx);
    /// An instance moved: track it as a mover until the next build.
    void noteShadowMover(M2Instance& inst) {
        if (inst.shadowMoverStamp == shadowCandStamp_) return;
        // Missiles and kits move every frame and never cast; see renderShadow.
        if (inst.cachedModel && inst.cachedModel->isSpellEffect) return;
        inst.shadowMoverStamp = shadowCandStamp_;
        shadowMovers_.push_back(inst.id);
        // Only a caster on the static list changes it by leaving; any other
        // candidate was culled out of it already.
        if (inst.shadowStaticMark == shadowStaticMark_) bumpShadowStatic(kShadowGenMover);
    }

    // Ribbon draw-call list (reused each frame)
    struct RibbonDrawCall {
        VkDescriptorSet texSet;
        VkPipeline      pipeline;
        uint32_t        firstVertex;
        uint32_t        vertexCount;
        /// m2_ribbon.frag's push block: the material's M2 blend and whether
        /// it is unfogged (flag 0x2), which pick the stock fog colour.
        uint32_t        blendMode;
        uint32_t        unfogged;
    };
    std::vector<RibbonDrawCall> ribbonDraws_;
    std::vector<EffectStrip> effectStrips_;
    /// A descriptor set per texture an effect strip has used.
    std::unordered_map<VkTexture*, VkDescriptorSet> effectTexSets_;
    VkDescriptorSet effectTextureSet(VkTexture* tex);

    // Particle group structures (reused each frame)
    struct ParticleGroupKey {
        VkTexture* texture;
        uint8_t blendType;
        uint16_t tilesX;
        uint16_t tilesY;
        bool operator==(const ParticleGroupKey& other) const {
            return texture == other.texture &&
                   blendType == other.blendType &&
                   tilesX == other.tilesX &&
                   tilesY == other.tilesY;
        }
    };
    struct ParticleGroupKeyHash {
        size_t operator()(const ParticleGroupKey& key) const {
            size_t h1 = std::hash<uintptr_t>{}(reinterpret_cast<uintptr_t>(key.texture));
            size_t h2 = std::hash<uint32_t>{}((static_cast<uint32_t>(key.tilesX) << 16) | key.tilesY);
            size_t h3 = std::hash<uint8_t>{}(key.blendType);
            return h1 ^ (h2 * 0x9e3779b9u) ^ (h3 * 0x85ebca6bu);
        }
    };
    struct ParticleGroup {
        VkTexture* texture;
        uint8_t blendType;
        uint16_t tilesX;
        uint16_t tilesY;
        VkDescriptorSet preAllocSet = VK_NULL_HANDLE;
    };
    std::unordered_map<ParticleGroupKey, ParticleGroup, ParticleGroupKeyHash> particleGroups_;

    /// One run of particles written to the vertex buffer that share a group.
    ///
    /// The vertices used to be accumulated into a std::vector<float> per
    /// group - nine push_backs a particle - and then memcpy'd into the mapped
    /// buffer. They are written straight into it now, in the order they are
    /// walked, and a run is closed whenever the group changes. Particles from
    /// one emitter cluster together, so a run is roughly an emitter.
    ///
    /// That also fixes the upload: every group used to memcpy to offset zero
    /// and draw from vertex zero, so with more than one group on screen they
    /// all drew whichever group was copied last.
    struct ParticleRun {
        ParticleGroup* group = nullptr;
        uint32_t first = 0;
        uint32_t count = 0;
    };
    std::vector<ParticleRun> particleRuns_;
    /// Quads the particle pass draws per frame, one 48-byte record each (a
    /// particle with a head and a tail is two). Dalaran has ~39k live in
    /// view. WOWEE_PARTICLE_BUDGET overrides it, read at init.
    static constexpr uint32_t kDefaultParticleRecordBudget = 65536;
    uint32_t particleRecordBudget_ = kDefaultParticleRecordBudget;
    /// Distinct emitter curves the GPU path can upload per frame; an emitter
    /// whose curve does not fit is not drawn that frame.
    static constexpr uint32_t kMaxParticleCurvesPerFrame = 2048;
    /// Where each emitter curve went in this frame's curve buffer, in vec4s.
    std::unordered_map<const void*, uint32_t> particleCurveSlots_;
    /// renderM2Particles' fill: a ParticleRecord per quad into
    /// particleRecordBuf_[frame] and each emitter's curves into
    /// particleCurveBuf_[frame]. Fills particleRuns_ and returns how many
    /// records it wrote.
    size_t writeParticleRecordsGpu(uint32_t frame);

    // Animation update buffers (avoid per-frame allocation)
    std::vector<size_t> boneWorkIndices_;        // Reused each frame
    /// What update() spent and on how many instances, summed over a window
    /// and reported under WOWEE_FRAME_PROFILE; see the end of update().
    struct UpdateProfile {
        double preMs = 0.0, animLoopMs = 0.0, bonesMs = 0.0, particlesMs = 0.0;
        uint64_t animatedListed = 0, boneWork = 0, staticPoseSkips = 0, particleListed = 0;
        uint64_t shadowCandidates = 0, shadowCasters = 0, shadowRebuilds = 0, shadowMovers = 0;
        uint64_t shadowStaticReuses = 0;
        /// Why the static shadow list was not kept, per frame: generation
        /// bumped, radius changed, or a candidate's verdict turned (or came
        /// within rounding of turning) after the centre or matrix moved.
        uint64_t shadowMissGen = 0, shadowMissRadius = 0, shadowMissFlip = 0;
        /// Frames whose centre or light matrix differed from the build's,
        /// kept or not; kept within the slack; candidates re-tested.
        uint64_t shadowCentreDiffers = 0, shadowLightDiffers = 0;
        uint64_t shadowKeptInSlack = 0, shadowRechecks = 0;
        /// Generation bumps by source (ShadowGenSource).
        uint64_t shadowGenBumps[6] = {};
        uint64_t particleFull = 0, particleDormant = 0, particleLive = 0;
        /// The particle loop's first pass (clocks, range, view), and how
        /// many threads its second pass ran on, summed over frames.
        double particleClassifyMs = 0.0;
        uint64_t particleThreads = 0;
        double queryMs = 0.0;
        uint64_t queryCalls = 0;
        uint32_t frames = 0;
        std::chrono::steady_clock::time_point since{};
    } updateProf_;
    /// Frame counter staggering the off-screen particle steps.
    uint32_t particleFrame_ = 0;
    /// Particle vertices the last particle pass wrote; written by the render
    /// worker, read by the update's profile line.
    std::atomic<uint32_t> particleVertsDrawn_{0};
    /// Draw runs (texture group changes) the particle pass made last frame.
    std::atomic<uint32_t> particleRunsDrawn_{0};
    std::vector<std::future<void>> animFutures_; // Reused each frame
    bool spatialIndexDirty_ = false;

    // Fast-path instance index lists (rebuilt in rebuildSpatialIndex / on create)
    std::vector<size_t> animatedInstanceIndices_;   // hasAnimation && !disableAnimation
    std::vector<size_t> boatInstanceIndices_;       // cachedIsBoat: the afloat check
    std::vector<size_t> lightInstanceIndices_;      // lanterns, torches, braziers, forges, lava: gatherLocalLights
    bool lightFlicker_ = false;   // lamps gutter (enhancement)
    bool glowSpritesOn_ = false;  // glow cards drawn as halos (enhancement)
    bool extraSmoke_ = false;     // synthesised smoke over smoke models (enhancement)
    // Fade in new scenery (setStreamFadeIn).
    bool streamFadeOn_ = false;
    float streamFadeClock_ = 0.0f;
    uint32_t lastNewInstanceId_ = 0;  // the last createInstance* that made one
    /// The draw's flags bits 24-31 for an instance (stream_fade::hideByte).
    [[nodiscard]] int32_t streamFadeFlags(const M2Instance& inst) const;
    // Soft particles (setSoftParticlesDeferred). Made on first use only.
    bool softDeferred_ = false;
    VkPipelineLayout particleSoftLayout_ = VK_NULL_HANDLE;  // sets: perFrame, tex, curves, depth
    VkPipelineLayout glowSoftLayout_ = VK_NULL_HANDLE;      // sets: perFrame, tex, depth
    VkPipeline particleSoftPipelines_[7] = {};         // blend types 2..6 used
    VkPipeline glowSoftPipeline_ = VK_NULL_HANDLE;
    uint32_t glowSoftCount_ = 0;                       // halos uploaded for the late pass
    /// A particle group's texture set for this frame (the model's, else one
    /// made now and kept on the group until the frame's next write).
    VkDescriptorSet particleTexSet(ParticleGroup& group);
    static bool modelGivesLight(const M2ModelGPU& m) {
        return m.isLanternLike || m.isTorch || m.isBrazierOrFire || m.isForge || m.isLavaModel ||
               !m.stockPointLights.empty();
    }
    double sceneTimeMs_ = 0.0;                      // sum of update() deltas
    Frustum cachedFrustum_;                         // this frame's view, for the particle pass
    /// Bring an instance's animation clocks up to the scene time.
    void advanceClock(M2Instance& inst) const {
        const float dt = static_cast<float>(sceneTimeMs_ - inst.clockStampMs);
        if (dt <= 0.0f) return;
        inst.clockStampMs = sceneTimeMs_;
        inst.animTime += dt;
        inst.globalSequenceTime += dt;
        const bool stepped = inst.cachedHasAnimation && !inst.cachedDisableAnimation;
        if (!stepped && inst.animDuration > 0.0f && inst.animTime >= inst.animDuration) {
            inst.animTime = std::fmod(inst.animTime, inst.animDuration);
        }
    }
    std::vector<size_t> particleOnlyInstanceIndices_; // !hasAnimation && hasParticleEmitters
    std::vector<size_t> particleInstanceIndices_;    // ALL instances with particle emitters
    /// How far this instance's particles are stepped and drawn, squared: the
    /// scene's range, cut down to what is big enough on screen to see.
    float particleRangeSq(const M2Instance& inst, const M2ModelGPU& model) const;
    std::vector<size_t> particleDrawOrder_;          // this frame's drawable emitters, near ones first
    /// What an emitter instance does this frame: step, wake (warm up), step
    /// its ribbons only, or nothing (out of range). Moves it in and out of
    /// dormancy; shared by update() and updatePosedEmitters().
    enum ParticleWorkMode : uint8_t { kWorkStep, kWorkWake, kWorkRibbonsOnly, kWorkSkip };
    ParticleWorkMode classifyParticleWork(M2Instance& inst, const M2ModelGPU& model,
                                          const Frustum& frustum, float deltaTime);
    /// Override curves by model and colours (setInstanceParticleColors).
    struct ParticleColorKey {
        uint32_t modelId = 0;
        std::array<float, 27> rgb{};
        bool operator<(const ParticleColorKey& o) const {
            return modelId != o.modelId ? modelId < o.modelId : rgb < o.rgb;
        }
    };
    std::map<ParticleColorKey, std::shared_ptr<const std::vector<glm::vec4>>> particleColorCurves_;

    // Smoke particle system
    std::vector<SmokeParticle> smokeParticles;
    std::vector<size_t> smokeInstanceIndices_;  // Indices into instances[] for smoke emitters
    std::vector<size_t> portalInstanceIndices_; // Indices into instances[] for spinning portals
    static constexpr int MAX_SMOKE_PARTICLES = 1000;
    float smokeEmitAccum = 0.0f;
    std::mt19937 smokeRng{42};

    // M2 particle emitter system
    static constexpr size_t MAX_M2_PARTICLES = 4000;
    std::mt19937 particleRng_{123};
    bool skyMode_ = false;
    std::function<std::optional<float>(float, float, float)> waterHeightAt_;
    // What the sky-model clock diagnostic last reported, so it prints on a
    // restart or once a second rather than every frame. See M2Renderer::update.
    uint32_t skyDiagInstanceId_ = 0;
    float skyDiagAnimTime_ = 0.0f;
    /// Whether the sky model was drawn last frame, so the cull
    /// diagnostic prints on a change rather than every frame.
    bool skyDiagWasDrawn_ = false;
    /// Sky batches that reached a draw call this frame, and the last counts
    /// reported, so the line prints on a change rather than every frame.
    uint32_t skyDiagDrawsOpaque_ = 0;
    uint32_t skyDiagDrawsTransparent_ = 0;
    uint32_t skyDiagLastOpaque_ = 0xFFFFFFFFu;
    uint32_t skyDiagLastTransparent_ = 0xFFFFFFFFu;

    // Cached camera state from update() for frustum-culling bones
    glm::vec3 cachedCamPos_ = glm::vec3(0.0f);
    float cachedMaxRenderDistSq_ = 0.0f;
    float smoothedRenderDist_ = 1000.0f;  // Smoothed render distance to prevent flickering
    /// The distance as asked for, kept so the scale can be rebuilt when the
    /// detail setting changes without the caller having to say it again.
    float viewDistanceRaw_ = 1200.0f;
    float environmentDetail_ = 1.0f;
    float groundDetailMaxDistance_ = kMaxGroundDetailDistance;  // see setGroundDetailDistance
    void recomputeViewDistanceScale() {
        viewDistanceScale_ = (viewDistanceRaw_ / 1200.0f) * environmentDetail_;
    }
    float viewDistanceScale_ = 1.0f;
    float viewDistanceAbsolute_ = 1200.0f;
    /// Drop the sky model's baked star layer, because something else is
    /// drawing stars. See BatchGPU::starLayer.
    bool suppressBakedStars_ = false;
    float particleDensity_ = 1.0f;
    float furthestDrawnSq_ = 0.0f;
    bool forceNoCull_ = false;

    // Thread count for parallel bone animation
    uint32_t numAnimThreads_ = 1;

    float interpFloat(const pipeline::M2AnimationTrack& track, float animTime,
                      float globalTime, int seqIdx,
                      const std::vector<uint32_t>& globalSeqDurations);
    /// What one thread of the particle step owns: its own random stream,
    /// emitter scratch and birth list. Instances are independent, so the
    /// emitter list is split across threads and each runs its share with
    /// one of these; stream t is seeded 123 + t (stream 0 is the seed the
    /// single-threaded step has always used).
    struct ParticleWorker {
        std::mt19937 rng;
        std::vector<m2_particle::StepEmitter> ems;
        std::vector<M2Particle> births;
        explicit ParticleWorker(uint32_t seed) : rng(seed) {}
    };
    std::vector<ParticleWorker> particleWorkers_;
    /// The particle loop's per-frame lists; see M2Renderer::update.
    struct ParticleWork {
        uint32_t idx;
        uint8_t mode;  // ParticleWorkMode
    };
    std::vector<ParticleWork> particleWork_;
    std::vector<uint32_t> particleWorkWeight_;
    std::vector<size_t> particleWorkBounds_;
    std::vector<std::future<void>> particleFutures_;
    /// Runs the instance's particle emitters for dt the way the stock
    /// CParticleEmitter2::Update does (0x97EB10): emitter frames and tracks
    /// once, then sub-steps of at most a tenth of a second, each spawning
    /// and then moving. A long dt is a warm-up: an emitter coming back into
    /// view after lying dormant gets its standing population in one call,
    /// since the sub-steps never cover more than a lifespan (see
    /// m2_particle::substeps and M2Renderer::update).
    ///
    /// Births come from the worker's random stream and go to its list, then
    /// are merged in after the last particle of their own emitter.
    void stepParticles(M2Instance& inst, const M2ModelGPU& gpu, float dt, ParticleWorker& worker);
    /// Builds gpu.particleCurves and gpu.particleTracks once per model. Not thread-safe: the update calls it for
    /// every emitter it will step before the step is split across threads.
    void ensureParticleCurves(const M2ModelGPU& gpu);
    void updateRibbons(M2Instance& inst, const M2ModelGPU& gpu, float dt);
    /// Drops an instance's ribbon edges and its trails' continuity: the next
    /// step starts each trail afresh where its emitter then is.
    static void resetRibbons(M2Instance& inst);

    // Helper to allocate descriptor sets
    VkDescriptorSet allocateMaterialSet();
    VkDescriptorSet allocateBoneSet();

    // Helper to destroy model GPU resources
    void destroyModelGPU(M2ModelGPU& model);
    // Helper to destroy instance bone buffers.
    // When defer=true, destruction is scheduled via deferAfterFrameFence so
    // in-flight command buffers are not invalidated (use for streaming unload).
    /// Starts a new instance's animation and seeds its bones from a
    /// sibling of the same model, so it draws on the frame it spawns.
    /// Both spawn paths need it and each used to have its own copy.
    void seedInstanceAnimation(const M2ModelGPU& model, uint32_t modelId,
                               M2Instance& instance);
    void seedUnanimatedSequence(const M2ModelGPU& model, M2Instance& instance);

    void destroyInstanceBones(M2Instance& inst, bool defer = false);
};

} // namespace rendering
} // namespace wowee
