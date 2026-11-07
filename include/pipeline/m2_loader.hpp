#pragma once

#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace wowee {
namespace pipeline {

/**
 * M2 Model Format (WoW Character/Creature Models)
 *
 * M2 files contain:
 * - Skeletal animated meshes
 * - Multiple texture units and materials
 * - Animation sequences
 * - Bone hierarchy
 * - Particle emitters, ribbon emitters, etc.
 *
 * Reference: https://wowdev.wiki/M2
 */

// Animation sequence data
struct M2Sequence {
    uint32_t id;                    // Animation ID
    uint32_t variationIndex;        // Sub-animation index
    uint32_t duration;              // Length in milliseconds
    float movingSpeed;              // Speed during animation
    uint32_t flags;                 // Animation flags
    int16_t frequency;              // Probability weight
    uint32_t replayMin;             // Minimum replay delay
    uint32_t replayMax;             // Maximum replay delay
    uint32_t blendTime;             // Blend time in ms
    glm::vec3 boundMin;             // Bounding box
    glm::vec3 boundMax;
    float boundRadius;              // Bounding sphere radius
    int16_t nextAnimation;          // Next animation in chain
    uint16_t aliasNext;             // Alias for next animation
};

// Animation track with per-sequence keyframe data
struct M2AnimationTrack {
    uint16_t interpolationType = 0; // 0=none, 1=linear, 2=hermite, 3=bezier
    int16_t globalSequence = -1;    // -1 if not a global sequence

    struct SequenceKeys {
        std::vector<uint32_t> timestamps;   // Milliseconds
        std::vector<glm::vec3> vec3Values;  // For translation/scale tracks
        std::vector<glm::quat> quatValues;  // For rotation tracks
        std::vector<float> floatValues;     // For float tracks (particle emitters)
    };
    std::vector<SequenceKeys> sequences;    // One per animation sequence

    [[nodiscard]] bool hasData() const { return !sequences.empty(); }
};

// Bone data for skeletal animation
struct M2Bone {
    int32_t keyBoneId;              // Bone ID (-1 = not key bone)
    uint32_t flags;                 // Bone flags
    int16_t parentBone;             // Parent bone index (-1 = root)
    uint16_t submeshId;             // Submesh ID
    glm::vec3 pivot;                // Pivot point

    M2AnimationTrack translation;   // Position keyframes per sequence
    M2AnimationTrack rotation;      // Rotation keyframes per sequence
    M2AnimationTrack scale;         // Scale keyframes per sequence
};

// Vertex with skinning data
struct M2Vertex {
    glm::vec3 position;
    uint8_t boneWeights[4];         // Bone weights (0-255)
    uint8_t boneIndices[4];         // Bone indices
    glm::vec3 normal;
    glm::vec2 texCoords[2];         // Two UV sets
};

// Texture unit
struct M2Texture {
    uint32_t type;                  // Texture type
    uint32_t flags;                 // Texture flags
    std::string filename;           // Texture filename (from FileData or embedded)
};

// Render batch (submesh)
struct M2Batch {
    uint8_t flags;
    int8_t priorityPlane;
    uint16_t shader;                // Shader ID
    uint16_t skinSectionIndex;      // Submesh index
    uint16_t colorIndex;            // Color animation index
    uint16_t materialIndex;         // Material index
    uint16_t materialLayer;         // Material layer
    uint16_t textureCount;          // Number of textures
    uint16_t textureIndex;          // First texture lookup index
    uint16_t textureUnit;           // Texture unit
    uint16_t transparencyIndex;     // Transparency animation index
    uint16_t textureAnimIndex;      // Texture animation index

    // Render data
    uint32_t indexStart;            // First index
    uint32_t indexCount;            // Number of indices
    uint32_t vertexStart;           // First vertex
    uint32_t vertexCount;           // Number of vertices

    // Geoset info (from submesh)
    uint16_t submeshId = 0;         // Submesh/geoset ID (determines body part group)
    uint16_t submeshLevel = 0;      // Submesh level (0=base, 1+=LOD/alternate mesh)

    // The skin section's sort point (M2SkinSection +0x12, +0x20, +0x2C): the
    // stock client sorts a blended batch by sortCenter through centerBone,
    // pushed by sortRadius when batch flags 0x1 or 0x2 ask (0x821A20).
    uint16_t centerBoneIndex = 0;
    glm::vec3 sortCenter{0.0f};
    float sortRadius = 0.0f;
};

// Material / render flags (per-batch blend mode)
struct M2Material {
    uint16_t flags;       // Render flags (unlit, unfogged, two-sided, etc.)
    uint16_t blendMode;   // 0=Opaque, 1=AlphaKey, 2=Alpha, 3=Add, 4=Mod, 5=Mod2x, 6=BlendAdd, 7=Screen
};

// Texture transform (UV animation) data
struct M2TextureTransform {
    M2AnimationTrack translation;   // UV translation keyframes
    M2AnimationTrack rotation;      // UV rotation keyframes (quat)
    M2AnimationTrack scale;         // UV scale keyframes
};

// Attachment point (bone-anchored position for weapons, effects, etc.)
struct M2Attachment {
    uint32_t id;        // 0=Head, 1=RightHand, 2=LeftHand, etc.
    uint16_t bone;      // Bone index
    glm::vec3 position; // Offset from bone pivot
};

// Camera baked into the model. Scene models (the character-select glue screens,
// cinematics) carry the framing the artist authored them for; without it, a scene
// whose geometry sits hundreds of units from its origin cannot be placed sensibly.
struct M2Camera {
    /// 0 is the portrait camera, 1 the character-sheet one, -1 anything else.
    /// Kept because the portrait framing is chosen by it: a model that carries
    /// its own portrait camera knows where its face is far better than a
    /// fraction of the bind-pose height can guess.
    int32_t type = -1;
    float fov = 0.0f;      // radians
    float farClip = 0.0f;
    float nearClip = 0.0f;
    glm::vec3 positionBase{0.0f};
    glm::vec3 targetBase{0.0f};  // the point the camera looks at
};

// FBlock: particle lifetime curve (color/alpha/scale over particle life)
// A light the model carries (M2Light, 156 bytes in 3.3.5). The glue scenes
// are lit by these and nothing else: the login screen's single light is an
// ambient of (0.72, 0.83, 0.93) x 1.3 with no diffuse, the human select scene
// a 0.27 ambient plus a warm and a cool directional light.
//
// A directional light (type 0) shines along -Z of its bone: the human scene's
// two are carried by bones rotated so that -Z points down onto the street from
// the camera's side, each from the side its bone stands on. A point light
// (type 1) sits at its bone's pivot plus position and fades out between the
// attenuation start and end.
struct M2Light {
    uint16_t type = 0;          // 0 directional, 1 point
    int16_t bone = -1;
    glm::vec3 position{0.0f};   // relative to the bone
    M2AnimationTrack ambientColor;      // vec3
    M2AnimationTrack ambientIntensity;  // float
    M2AnimationTrack diffuseColor;      // vec3
    M2AnimationTrack diffuseIntensity;  // float
    M2AnimationTrack attenuationStart;  // float
    M2AnimationTrack attenuationEnd;    // float
    M2AnimationTrack visibility;        // byte, read as 0/1 floats
};

/// A particle's lifetime curve ("fake animation block"): keys over the
/// particle's life ratio, with no interpolation type or global sequence.
/// The stock client reads the timestamps as fixed16 fractions, value / 32767
/// (CParticleEmitter2 0x9793B0), not normalised by the last key; they are
/// stored here already divided. Which key pair is used and how the fraction is
/// worked out is m2_particle::fblockSegment.
struct M2FBlock {
    std::vector<float> timestamps;      // life ratio, s16 / 32767
    std::vector<float> floatValues;     // alpha (s16 / 32767), scale x, cell index
    std::vector<glm::vec3> vec3Values;  // colour, 0..1 (on disk 0..255)
    std::vector<glm::vec2> vec2Values;  // scale x, y
};

// Particle emitter definition parsed from M2 (WotLK M2Particle, 0x1DC bytes).
// Offsets are into the on-disk record; docs/re/particles.md says what the
// stock runtime does with each field.
struct M2ParticleEmitter {
    int32_t particleId;
    uint32_t flags;
    glm::vec3 position;
    uint16_t bone;
    uint16_t texture;
    uint8_t blendingType;   // 0x28: 0 opaque, 1 alpha key, 2 alpha, 3 add (one, one), 4 add, 5 mod, 6 mod2x
    uint8_t emitterType;    // 0x29: 1 plane, 2 sphere, 3 spline
    uint16_t particleColorIndex = 0;  // 0x2A: ParticleColor.dbc override slot
    uint8_t particleType = 0;         // 0x2C: not read by the 3.3.5 runtime
    uint8_t headOrTail = 0;           // 0x2D: not read either; flags 0x20000/0x40000 decide
    int16_t textureTileRotation = 0;
    uint16_t textureRows = 1;
    uint16_t textureCols = 1;
    M2AnimationTrack emissionSpeed;
    M2AnimationTrack speedVariation;
    M2AnimationTrack verticalRange;
    M2AnimationTrack horizontalRange;
    M2AnimationTrack gravity;
    M2AnimationTrack lifespan;
    float lifespanVary = 0.0f;      // 0xAC
    M2AnimationTrack emissionRate;
    float emissionRateVary = 0.0f;  // 0xC4
    M2AnimationTrack emissionAreaLength;
    M2AnimationTrack emissionAreaWidth;
    M2AnimationTrack deceleration;  // on-disk zSource (0xF0); not a deceleration
    M2FBlock particleColor;   // 0x104, vec3
    M2FBlock particleAlpha;   // 0x114, fixed16
    M2FBlock particleScale;   // 0x124, vec2 (floatValues holds x)
    glm::vec2 scaleVary{0.0f};  // 0x134
    M2FBlock headCell;        // 0x13C, uint16 cell index over life
    M2FBlock tailCell;        // 0x14C
    float tailLength = 0.0f;      // 0x15C, seconds of travel the tail covers
    float twinkleSpeed = 0.0f;    // 0x160
    float twinklePercent = 1.0f;  // 0x164
    float twinkleScaleMin = 1.0f; // 0x168
    float twinkleScaleMax = 1.0f; // 0x16C
    float burstMultiplier = 1.0f; // 0x170
    float drag = 0.0f;            // 0x174
    float baseSpin = 0.0f;        // 0x178
    float baseSpinVary = 0.0f;    // 0x17C
    float spin = 0.0f;            // 0x180
    float spinVary = 0.0f;        // 0x184
    glm::vec3 tumbleMin{0.0f};    // 0x188 (model particles only)
    glm::vec3 tumbleMax{0.0f};    // 0x194
    glm::vec3 windVector{0.0f};   // 0x1A0
    float windTime = 0.0f;        // 0x1AC
    float followSpeed1 = 0.0f, followScale1 = 0.0f;  // 0x1B0
    float followSpeed2 = 0.0f, followScale2 = 0.0f;  // 0x1B8
    std::vector<glm::vec3> splinePoints;  // 0x1C0
    M2AnimationTrack enabledTrack;  // 0x1C8, uint8
    bool enabled = true;
};

// Ribbon emitter definition parsed from M2 (WotLK format)
struct M2RibbonEmitter {
    int32_t  ribbonId   = 0;
    uint32_t bone       = 0;        // Bone that drives the ribbon spine
    glm::vec3 position{0.0f};       // Offset from bone pivot

    uint16_t textureIndex  = 0;     // First texture lookup index
    uint16_t materialIndex = 0;     // First material lookup index (blend mode)
    /// The material's blend mode and render flags, from the model's material
    /// table (M2Material). The ribbon's material index is into that table,
    /// not into the skin's batches (stock 0x833900).
    uint16_t blendMode = 4;
    uint16_t materialFlags = 0;

    // Animated tracks
    M2AnimationTrack colorTrack;       // RGB 0..1
    M2AnimationTrack alphaTrack;       // float 0..1 (stored as fixed16 on disk)
    M2AnimationTrack heightAboveTrack; // Half-width above bone
    M2AnimationTrack heightBelowTrack; // Half-width below bone
    M2AnimationTrack visibilityTrack;  // 0=hidden, 1=visible
    M2AnimationTrack texSlotTrack;     // 0x84 uint16: which texture cell

    float edgesPerSecond = 15.0f;   // How many edge points are generated per second
    float edgeLifetime   = 0.5f;    // Seconds before edges expire
    float gravity        = 0.0f;    // Downward pull on edges per s²
    uint16_t textureRows = 1;
    uint16_t textureCols = 1;
};

/// An M2 event name ("$CSS") as the uint32 its four bytes make, little-endian.
constexpr uint32_t m2EventTag(const char (&name)[5]) {
    return static_cast<uint32_t>(static_cast<uint8_t>(name[0])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(name[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(name[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(name[3])) << 24);
}

/// The M2 events kept in M2Model::soundEvents besides $CSD (see there): the
/// ones that make a sound, and $CPP, the moment in an attack animation the
/// victim's dodge, parry or block plays (Wow.exe 0x756240), which is silent
/// but rides the same list to reach the same callback.
constexpr bool isM2SoundEventTag(uint32_t tag) {
    // $BTH, silent too: the unit breathes out (0x732650 plays the cold,
    // underwater or drunk breath effect at attachment 17).
    if (tag == m2EventTag("$CSS") || tag == m2EventTag("$CAH") || tag == m2EventTag("$WNG") ||
        tag == m2EventTag("$CPP") || tag == m2EventTag("$BTH"))
        return true;
    const uint32_t stem = tag & 0x00FFFFFFu;
    const uint32_t last = tag >> 24;
    if (stem == (m2EventTag("$AH0") & 0x00FFFFFFu)) return last >= '0' && last <= '3';
    if (stem == (m2EventTag("$FD1") & 0x00FFFFFFu)) return last >= '1' && last <= '5';
    return false;
}

/// The events a game object's model or a map doodad acts on, as the stock
/// client's object callbacks read them: 0x70c050 (every game object type with
/// the default handler, through 0x70cac0) takes $GO0-$GO5, $GC0-$GC3, $DSL,
/// $DSO, $SND and $SHK; 0x7bd5a0 (doodads of the map and of map objects)
/// takes $DSL, $DSE and $DSO.
constexpr bool isM2ObjectEventTag(uint32_t tag) {
    const uint32_t stem = tag & 0x00FFFFFFu;
    const uint32_t last = tag >> 24;
    if (stem == (m2EventTag("$GO0") & 0x00FFFFFFu)) return last >= '0' && last <= '5';
    if (stem == (m2EventTag("$GC0") & 0x00FFFFFFu)) return last >= '0' && last <= '3';
    return tag == m2EventTag("$DSL") || tag == m2EventTag("$DSO") || tag == m2EventTag("$DSE") ||
           tag == m2EventTag("$SND") || tag == m2EventTag("$SHK");
}

/// The GameObjectDisplayInfo Sound[] slot a game object event plays
/// (0x70c050 -> 0x70bf70): $GO0..$GO5 slots 0-5, $GC0..$GC3 slots 6-9; -1
/// for any other tag.
constexpr int gameObjectSoundSlot(uint32_t tag) {
    const uint32_t stem = tag & 0x00FFFFFFu;
    const int last = static_cast<int>(tag >> 24);
    if (stem == (m2EventTag("$GO0") & 0x00FFFFFFu) && last >= '0' && last <= '5') return last - '0';
    if (stem == (m2EventTag("$GC0") & 0x00FFFFFFu) && last >= '0' && last <= '3') return 6 + last - '0';
    return -1;
}

// Complete M2 model structure
struct M2Model {
    // Model metadata
    std::string name;
    uint32_t version;
    glm::vec3 boundMin;             // Model bounding box
    glm::vec3 boundMax;
    float boundRadius;              // Bounding sphere

    // Geometry data
    std::vector<M2Vertex> vertices;
    std::vector<uint16_t> indices;

    // Skeletal animation
    std::vector<M2Bone> bones;
    std::vector<M2Sequence> sequences;
    std::vector<uint32_t> globalSequenceDurations;  // Per-global-sequence loop durations (ms)

    // Footfall ($FSD) animation event times per sequence index, in ms from the
    // start of the sequence, sorted ascending. These are the authored keyframes
    // the game client uses to sync footstep sounds to feet hitting the ground.
    // Empty inner list = no footfall events for that sequence.
    std::vector<std::vector<uint32_t>> footstepEventTimes;

    // Sound ($CSD) animation events per sequence index, sorted by time. The
    // event's data field is a SoundEntries id: character models carry one per
    // vocal emote (EmoteLaugh -> HumanMaleEmoteLaugh, EmoteCry, EmoteKiss,
    // EmoteChicken, EmoteApplaud -> ClapSounds). Empty inner list = none.
    //
    // The same list carries the other events that make a sound, with
    // soundEntryId 0 and the event's name in tag (its four bytes read as a
    // little-endian uint32, see m2EventTag): $CSS weapon swoosh, $CAH the
    // blow landing, $AH0-$AH3 CreatureSoundData custom attacks, $FD1-$FD5
    // fidgets, $WNG wing flaps; and the silent $CPP, the victim's defence.
    // $FSD footfalls stay in footstepEventTimes.
    struct SoundEvent {
        uint32_t timeMs;        // from the start of the sequence
        uint32_t soundEntryId;  // SoundEntries.dbc id ($CSD), 0 for the others
        uint32_t tag = 0x44534324u;  // '$CSD'
    };
    std::vector<std::vector<SoundEvent>> soundEvents;

    // The object events (isM2ObjectEventTag) per sequence index, sorted by
    // time, for the models that fire them at run time: game objects (door
    // swings, chest lids, the bobber's bite) and doodads that move with a
    // transport (a zeppelin's engines). The data field is a SoundEntries id
    // for $DSL/$DSO/$SND, a SpellEffectCameraShakes id for $SHK, unused for
    // $GOn/$GCn (the slot is in the tag). Position is on the bone, in its
    // rest frame. Embedded sequences only.
    struct ObjectEvent {
        uint32_t timeMs = 0;
        uint32_t tag = 0;
        uint32_t data = 0;
        uint16_t bone = 0;
        glm::vec3 position{0.0f};
    };
    std::vector<std::vector<ObjectEvent>> objectEvents;

    // A doodad's own sounds: $DSL events (loop) and $DSO events (one-shot at
    // timeMs into the idle sequence, every periodMs - that sequence's length).
    // The data field is a SoundEntries id; position is in model space.
    struct DoodadSound {
        uint32_t soundEntryId = 0;
        glm::vec3 position{0.0f};
        bool loop = true;
        uint32_t periodMs = 0;
        uint32_t timeMs = 0;
    };
    std::vector<DoodadSound> doodadSounds;

    // A spell effect model's own sounds: $SND events, at timeMs into its
    // first sequence (the birth), sorted by time. The data field is a
    // SoundEntries id. Twenty 3.3.5 models carry one, all under Spells\:
    // LevelUp.m2 plays LEVELUP (888) 33 ms in, Achievement_OnRoot.m2
    // AchievementSound (12891), ReputationLevelUp.m2 8473, the meteor and
    // infernal impacts 3181.
    struct EffectSound {
        uint32_t soundEntryId = 0;
        uint32_t timeMs = 0;
    };
    std::vector<EffectSound> effectSounds;
    // A spell effect model's camera shakes: $SHK events, the data field a
    // SpellEffectCameraShakes id (Wow.exe 0x704C30), at timeMs into the
    // birth. Eight 3.3.5 models: the meteor, infernal and abyssal impacts.
    std::vector<EffectSound> effectShakes;

    // Weapon hand-off ($SHL / $SHR) per sequence index, in ms: the frame of
    // Sheath (89) / HipSheath (90) where the hand reaches the sheath and the
    // weapon moves between hand and back or hip (HumanMale: 500 of 1000 on
    // Sheath, 567 on HipSheath). UINT32_MAX = the sequence has none.
    std::vector<uint32_t> sheathEventTimes;

    // The spell's release ($CSL / $CSR / $CST) per sequence index: the frame
    // of a cast animation where the spell leaves the hand, and the bone and
    // point it leaves from. The earliest of the three per sequence, as the
    // stock client launches a waiting missile on whichever fires first
    // (Wow.exe 0x732650 -> 0x715A50). HumanMale: SpellCastDirected $CSL at
    // 200 of 1000 ms, SpellCastOmni $CST at 300. timeMs UINT32_MAX = none.
    struct CastReleaseEvent {
        uint32_t timeMs = UINT32_MAX;
        uint32_t bone = 0;
        glm::vec3 position{0.0f};
    };
    std::vector<CastReleaseEvent> castReleaseEvents;

    // A weapon's swing trail runs between two points: $WTB (bottom) and
    // $WTT (top), each a bone and a point (Wow.exe 0x7E4F50). 1106 of the
    // 1379 3.3.5 weapon models carry both. bone UINT32_MAX = none.
    struct WeaponTrailPoint {
        uint32_t bone = UINT32_MAX;
        glm::vec3 position{0.0f};
    };
    WeaponTrailPoint weaponTrailBottom, weaponTrailTop;

    // Bone lookup table (vertex bone indices reference this to get global bone index)
    std::vector<uint16_t> boneLookupTable;

    // Rendering
    std::vector<M2Batch> batches;
    std::vector<M2Texture> textures;
    std::vector<uint16_t> textureLookup;  // Batch texture index lookup
    /// Texture coordinate combos (header 0x88): per texture stage, 0 = UV set
    /// 0, 1 = UV set 1, -1 (above 2 read unsigned) = sphere environment map.
    /// A batch's textureUnit indexes the first of its stages here.
    std::vector<int16_t> textureCoordCombos;
    /// Texture combiner combos (header 0x130, only with globalFlags 0x08):
    /// per texture stage, the stock combiner op a batch's shader id indexes.
    std::vector<uint16_t> textureCombinerCombos;
    std::vector<M2Material> materials;    // Render flags / blend modes

    // Texture transforms (UV animation)
    std::vector<M2TextureTransform> textureTransforms;
    std::vector<uint16_t> textureTransformLookup;

    // Texture weights (per-batch opacity, from M2Track<fixed16>)
    // Each entry is the "at-rest" opacity value (0=transparent, 1=opaque).
    // batch.transparencyIndex → textureWeightLookup[idx] → textureWeights[trackIdx]
    std::vector<float> textureWeights;
    std::vector<M2AnimationTrack> textureWeightTracks;
    std::vector<uint16_t> textureWeightLookup;

    // Color animation alpha values (from M2Color.alpha M2Track<fixed16>)
    // One entry per color animation slot; batch.colorIndex indexes directly into this.
    // Value 0=transparent, 1=opaque. Independent from textureWeights.
    std::vector<float> colorAlphas;
    /// At-rest RGB of each colour track, parallel to colorAlphas.
    ///
    /// An M2Color is a vec3 colour track followed by an alpha track, and only
    /// the alpha was read. The colour is what tints a batch: Orgrimmar's
    /// bonfire glow is authored white and carries (1.0, 0.329, 0.0) here, so
    /// without it the fire renders as a white blob.
    std::vector<glm::vec3> colorRGB;

    // Full per-sequence alpha keyframes for the same color slots. Evaluated at
    // render time to hide batches whose alpha animates to 0 in the current
    // animation (e.g. the lumberjack carry model's alternate wood bundle).
    std::vector<M2AnimationTrack> colorAlphaTracks;
    /// The RGB half of each M2Color, the whole track (colorRGB is its first key).
    std::vector<M2AnimationTrack> colorRgbTracks;

    // Attachment points (for weapon/effect anchoring)
    std::vector<M2Attachment> attachments;
    std::vector<M2Camera> cameras;
    std::vector<M2Light> lights;            // 3.3.5 models only
    std::vector<uint16_t> attachmentLookup; // attachment ID → index

    // Particle emitters
    std::vector<M2ParticleEmitter> particleEmitters;

    // Ribbon emitters
    std::vector<M2RibbonEmitter> ribbonEmitters;

    // Collision mesh (simplified geometry for physics)
    std::vector<glm::vec3> collisionVertices;
    std::vector<uint16_t> collisionIndices;      // 3 per triangle
    std::vector<glm::vec4> collisionNormals;     // xyz=normal, w=distance; one per triangle

    // Flags
    uint32_t globalFlags;

    [[nodiscard]] bool isValid() const {
        return !vertices.empty() && !indices.empty();
    }
};

class M2Loader {
public:
    /**
     * Load M2 model from raw file data
     *
     * @param m2Data Raw M2 file bytes
     * @return Parsed M2 model
     */
    static M2Model load(const std::vector<uint8_t>& m2Data);

    /**
     * Load M2 skin file (contains submesh/batch data)
     *
     * @param skinData Raw M2 skin file bytes
     * @param model Model to populate with skin data
     * @return True if successful
     */
    static bool loadSkin(const std::vector<uint8_t>& skinData, M2Model& model);

    /**
     * Load external .anim file data into model bone tracks
     *
     * @param m2Data Original M2 file bytes (contains track headers)
     * @param animData Raw .anim file bytes
     * @param sequenceIndex Which sequence index this .anim file provides data for
     * @param model Model to patch with animation data
     */
    static void loadAnimFile(const std::vector<uint8_t>& m2Data,
                             const std::vector<uint8_t>& animData,
                             uint32_t sequenceIndex,
                             M2Model& model);
};

std::string skinPathForM2(const std::string& m2Path);

/// The .m2 a model reference means, whatever it was written as.
///
/// WMO doodad lists, ADT doodad lists and GameObjectDisplayInfo all name models
/// with the extension the art was authored with - .mdx, and occasionally .mdl -
/// while what ships is .m2. Every reader has to rewrite it, and nine of them
/// did, in their own words:
///
///   * four rewrote .mdx and .mdl
///   * three rewrote only .mdx, so a .mdl reference reached the asset manager
///     as ".mdl", found nothing, and the doodad silently did not appear
///
/// Both are handled here. A path with any other extension, or none, comes back
/// unchanged - this rewrites a known alias and does not guess.
std::string modelPathToM2(const std::string& modelPath);

} // namespace pipeline
} // namespace wowee
