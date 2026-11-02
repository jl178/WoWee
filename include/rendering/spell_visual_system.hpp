#pragma once

#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <glm/glm.hpp>

struct lua_State;

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class M2Renderer;
class Renderer;
class CharacterRenderer;

class SpellVisualSystem {
public:
    SpellVisualSystem() = default;
    ~SpellVisualSystem();  // closes the missile-motion Lua state
    SpellVisualSystem(const SpellVisualSystem&) = delete;
    SpellVisualSystem& operator=(const SpellVisualSystem&) = delete;

    // Initialize with references to the M2 renderer and parent renderer
    void initialize(M2Renderer* m2Renderer, Renderer* renderer);
    void shutdown();

    // Play a SpellVisual's CastKit (useImpactKit=false) or ImpactKit (true) on
    // a unit. Every effect slot the kit fills (head, chest, base, both hands,
    // breath, weapons, specials) spawns its own model at that unit's matching
    // attachment point. attachInstanceId is the unit's CharacterRenderer
    // instance (0 = not drawn: the effects stand at worldPosition, the unit's
    // feet, raised to the slot's usual height).
    void playSpellVisual(uint32_t visualId, const glm::vec3& worldPosition,
                         bool useImpactKit = false, uint32_t attachInstanceId = 0);

    // Play one SpellVisualKit by its own id - what SMSG_PLAY_SPELL_VISUAL and
    // SMSG_PLAY_SPELL_IMPACT carry (SpellVisualKit.dbc, not SpellVisual.dbc).
    void playSpellVisualKit(uint32_t kitId, const glm::vec3& worldPosition,
                            uint32_t attachInstanceId = 0);

    // Play one effect model on a unit's base, as the stock client plays its
    // hardcoded SpellVisualEffectName rows - 21 "Unit Level Up"
    // (Spells\LevelUp\LevelUp.m2) on anyone who levels, 4392 "Achievement
    // Base" (Spells\Achievement_OnRoot.m2) on anyone who earns one. Its $SND
    // events play at it as they come due, like any kit model's. False when
    // the model cannot be loaded.
    bool playEffectModel(const std::string& modelPath, const glm::vec3& feetPos,
                         uint32_t attachInstanceId = 0);

    // The caster's build-up while the cast bar fills (PrecastKit), or while a
    // channel runs (channel=true: ChannelKit). It stays on the caster until
    // cancelPrecastVisuals(casterGuid) - SPELL_GO, failure, interrupt, channel
    // end - or, as a safety net, durationMs plus a grace second.
    void playSpellVisualPrecast(uint32_t visualId, const glm::vec3& worldPosition,
                                uint32_t durationMs = 0, uint32_t attachInstanceId = 0,
                                uint64_t casterGuid = 0, bool channel = false);

    // Whether the kit this visual plays at one moment of the cast names a
    // sound: the build-up (PrecastKit or ChannelKit), the release (CastKit,
    // or the missile's own MissileSound) or the landing (ImpactKit,
    // TargetImpactKit or ImpactAreaKit); ANY: any of those or the area and
    // state kits. The school-sound fallback plays only for a spell with no
    // kit sound at all (ANY false), as the stock client plays only kits.
    enum class KitSoundMoment { PRECAST, CAST, IMPACT, ANY };
    bool visualHasKitSound(uint32_t visualId, KitSoundMoment moment);

    // Whether this visual flies a missile: SpellVisual.HasMissile set and a
    // MissileModel to fly. (Divine Spirit's row names a model with
    // HasMissile 0; the stock client shows nothing.)
    bool hasSpellMissile(uint32_t visualId);

    // Fly the visual's missile model from the caster to the target at speed
    // yards a second (Spell.dbc Speed). It leaves from the caster attachment
    // named by SpellVisual.MissileAttachment (default: between the hands) and
    // homes on the target attachment named by MissileDestinationAttachment
    // (usually the chest), following a moving target. start/end are the units'
    // feet, used when a unit is not drawn. With impactAtEnd the impact kit
    // plays on the target when it lands. False when nothing launched.
    //
    // The path follows SpellVisual.MissileMotion's script (SpellMissileMotion
    // .dbc: arcs, spins, spirals, several missiles), a plain parabola for
    // MissilePathType 2 without one, and the ground for MissileFollowGround.
    // MissileSound plays on it, looping while it flies when its SoundEntries
    // row loops. groundTarget: end is a point on the ground (a destination
    // location), aimed at exactly, and the landing plays ImpactAreaKit there.
    bool playSpellMissile(uint32_t visualId, const glm::vec3& start, const glm::vec3& end,
                          float speed, bool impactAtEnd,
                          uint32_t casterInstanceId = 0, uint32_t targetInstanceId = 0,
                          bool groundTarget = false);

    // A spell cast at a point on the ground (SPELL_GO's destination location):
    // InstantAreaKit there now, ImpactAreaKit when it lands (now, unless a
    // missile is carrying it - impactNow=false), and PersistentAreaKit kept
    // there for lifetimeSec. channelOwnerGuid != 0 marks a channel's area
    // (Blizzard, Rain of Fire): it also ends with cancelChannelAreaVisuals.
    void playSpellAreaVisual(uint32_t visualId, const glm::vec3& groundPos, float lifetimeSec,
                             uint64_t channelOwnerGuid, bool impactNow);
    // End a caster's channelled ground areas (the channel ended or broke).
    void cancelChannelAreaVisuals(uint64_t casterGuid);

    // Aura state visuals. The unit's current aura visuals (Spell.SpellVisualID
    // of every aura it has) - each one's StateKit loops on the unit while it
    // is in the list; one that leaves plays its StateDoneKit. The resolvers
    // find a unit's CharacterRenderer instance and feet as it comes and goes.
    using UnitInstanceResolver = std::function<uint32_t(uint64_t guid)>;
    using UnitPositionResolver = std::function<bool(uint64_t guid, glm::vec3& feet)>;
    void setUnitResolvers(UnitInstanceResolver instanceOf, UnitPositionResolver feetOf);
    [[nodiscard]] bool hasUnitResolvers() const { return static_cast<bool>(unitInstanceOf_); }
    void syncUnitStateVisuals(uint64_t guid, const std::vector<uint32_t>& visualIds);
    // Forget every unit's aura states (logout, aura caches dropped).
    void clearAllUnitStateVisuals();

    // Caster animations named by the visual's kits (SpellVisualKit.AnimID),
    // 0 where the kit names none: e.g. Fireball 51/53 (ReadySpellDirected,
    // SpellCastDirected), Flash Heal 52/54 (the Omni pair), Arcane Missiles'
    // channel 124 (ChannelCastDirected).
    struct KitAnimations { uint32_t precast = 0; uint32_t cast = 0; uint32_t channel = 0; };
    KitAnimations getKitAnimations(uint32_t visualId);

    // Launch a physical weapon projectile (arrow, bullet, or thrown item)
    // without invoking the spell visual pipeline.
    void playPhysicalProjectile(const std::string& modelPath,
                                const std::string& texturePath,
                                const glm::vec3& start,
                                const glm::vec3& end,
                                float duration,
                                bool spin);

    // Advance lifetime timers and remove expired instances.
    void update(float deltaTime);

    // End the precast/channel build-up of one caster (its cast went off,
    // failed or was interrupted, or its channel ended). It winds down - Decay,
    // no new particles, the live ones finish (at most ~1.5 s) - and its
    // looping sound fades.
    void cancelPrecastVisuals(uint64_t casterGuid);

    // Remove all active spell visual instances and reset caches.
    // Called on map change / combat reset.
    void reset();

    // SpellVisualKit effect slots, in the kit's column order. SpellVisual's
    // MissileAttachment / MissileDestinationAttachment use the same numbering
    // (Fireball: -1 = default, destination 1 = chest; dragon breath bolts 5).
    enum KitSlot : uint8_t {
        SLOT_HEAD = 0, SLOT_CHEST, SLOT_BASE, SLOT_LEFT_HAND, SLOT_RIGHT_HAND,
        SLOT_BREATH, SLOT_LEFT_WEAPON, SLOT_RIGHT_WEAPON,
        SLOT_SPECIAL0, SLOT_SPECIAL1, SLOT_SPECIAL2, SLOT_COUNT
    };

private:
    // Spell visual effects - transient M2 instances, and the looping sounds
    // of kits (instanceId 0, soundHandle set).
    struct SpellVisualInstance {
        uint32_t instanceId = 0;
        float elapsed = 0.0f;
        float duration = 0.0f;  // lifetime in seconds (M2 anim, default, or the caller's)
        bool isPrecast = false; // precast/channel build-up (ended on cancel/interrupt)
        uint8_t slot = SLOT_BASE;   // KitSlot it plays in (decides the attachment point)
        uint32_t attachInstanceId = 0;  // CharacterRenderer instance it rides (0 = static)
        uint64_t ownerGuid = 0;  // caster, for per-caster precast cancel (0 = none)
        uint32_t soundHandle = 0;   // AudioEngine id of a looping kit sound
        glm::vec3 soundPos{0.0f};   // where an unattached sound stands
        uint64_t stateGuid = 0;     // aura state visual: the unit...
        uint32_t stateVisual = 0;   // ...and the aura's SpellVisual id
        uint64_t areaOwner = 0;     // a channel's ground area: the caster
        // Birth (sequence 0), then Hold (158) looping for a kit that outlives
        // its birth, then Decay (159) as it ends - the stock spell models'
        // three sequences. Without them it just stops emitting.
        bool hasHold = false, hasDecay = false, holding = false;
        float birthDuration = 0.0f;
        // Ending: no new particles; kept until the live ones have run out.
        bool retiring = false;
        float linger = 0.0f, lingerCap = 0.0f, decayDuration = 0.0f;
        bool emissionStopped = false;
        // The model's $SND events (modelEffectSounds_), the next one due, and
        // where the model stands for them.
        uint32_t modelId = 0;
        uint32_t nextEffectSound = 0;
        glm::vec3 effectPos{0.0f};
    };
    // A model's $SND events: SoundEntries id at ms into its birth sequence.
    struct EffectSound { uint32_t soundEntryId = 0; uint32_t timeMs = 0; };

    struct KitEffect { uint8_t slot; std::string path; };
    struct KitRow {
        std::vector<KitEffect> effects;
        std::string worldEffect;  // WorldEffect: stands in the world at the kit's point
        uint32_t animId = 0;
        uint32_t soundId = 0;     // SoundEntries.dbc
    };
    struct VisualRow {
        uint32_t precastKit = 0, castKit = 0, impactKit = 0, channelKit = 0;
        uint32_t stateKit = 0, stateDoneKit = 0;
        uint32_t targetImpactKit = 0;  // plays on the target with the impact (Blizzard's ticks)
        uint32_t instantAreaKit = 0, impactAreaKit = 0, persistentAreaKit = 0;
        int32_t missileAttachment = -1;      // KitSlot, -1 = default (the hands)
        int32_t missileDestination = SLOT_CHEST;
        std::string missilePath;
        bool hasMissile = false;             // HasMissile
        uint32_t missileSound = 0;           // SoundEntries.dbc
        int32_t pathType = 0;                // MissilePathType: 2 = lobbed
        uint32_t motionId = 0;               // SpellMissileMotion.dbc
        uint32_t followGroundFlags = 0;      // bit 0: hug the ground
        float followGroundHeight = 0.0f;     // yards above it
        float followGroundDropSpeed = 0.0f;  // yards a second down to it
        float followGroundApproach = 0.0f;   // yards out it rises to the target
    };
    // SpellMissileMotion.dbc: a Lua script run each frame for each missile.
    struct MotionRow {
        std::string name, script;
        uint32_t missileCount = 1;
        int ref = -2;  // Lua registry ref; -2 not compiled yet, -1 failed
    };
    struct MotionOut {
        float transAngle = 0.0f, transMag = 0.0f;
        float transUp = 0.0f, transRight = 0.0f, transFront = 0.0f;
        float modelPitch = 0.0f, modelYaw = 0.0f, modelRoll = 0.0f;
        float scale = 1.0f, speedScalar = 1.0f, speedAbs = 0.0f;
    };

    struct PhysicalProjectile {
        uint32_t instanceId = 0;
        glm::vec3 start{0.0f};
        glm::vec3 end{0.0f};
        glm::vec3 rotation{0.0f};
        float elapsed = 0.0f;
        float duration = 0.0f;
        bool spin = false;
    };

    struct SpellMissile {
        uint32_t instanceId = 0;
        uint32_t visualId = 0;
        glm::vec3 pos{0.0f};        // the homing point: straight line to the target
        glm::vec3 shown{0.0f};      // pos plus the motion script's offsets
        glm::vec3 launch{0.0f};     // where it left from
        glm::vec3 end{0.0f};        // where it is going (re-aimed each frame)
        glm::vec3 endFeet{0.0f};    // target's feet when it launched (fallback)
        uint32_t targetInstanceId = 0;
        int32_t destSlot = SLOT_CHEST;
        float speed = 0.0f;
        float elapsed = 0.0f;
        float startDistance = 0.0f;
        float travelled = 0.0f;
        float speedFactor = 1.0f;   // the script's speedScalar / speedAbs, last frame
        uint32_t motionId = 0;      // 0 = none
        bool arc = false;           // MissilePathType 2 without a script: a parabola
        uint32_t missileIndex = 0, missileCount = 1;
        float rand[3] = {0.0f, 0.0f, 0.0f};
        uint32_t soundHandle = 0;   // AudioEngine id of its MissileSound
        bool soundLoops = false;
        bool impactAtEnd = false;
        bool groundTarget = false;
    };

    // A kit to spawn: where, on whom, for how long, and what it belongs to.
    struct KitSpawn {
        glm::vec3 feetPos{0.0f};
        uint32_t attachInstanceId = 0;
        bool isPrecast = false;
        uint64_t ownerGuid = 0;
        float lifetime = 0.0f;  // <= 0: the model's own length; infinity: until ended
        uint64_t stateGuid = 0;
        uint32_t stateVisual = 0;
        uint64_t areaOwner = 0;
    };

    // An aura state visual a unit should have, and the instance it is on.
    struct UnitStateRecord {
        uint32_t visualId = 0;
        uint32_t attachInstanceId = 0;
        bool spawned = false;
        float unresolvedTime = 0.0f;
    };

    void loadSpellVisualDbc();
    bool ensureLoaded();
    // Model ID for a cast, impact or missile model, loading it on first use.
    // 0 when it cannot be loaded.
    uint32_t loadSpellModel(const std::string& modelPath);
    // Spawn every effect of one kit, and play its sound.
    void spawnKit(uint32_t kitId, const KitSpawn& spawn);
    // One effect model of a kit; false when it could not be made.
    bool spawnKitModel(uint32_t kitId, const std::string& path, uint8_t slot, bool world,
                       const KitSpawn& spawn);
    // Play a SoundEntries row at pos. A looping row (flag 0x200) is kept and
    // its AudioEngine id returned when keepLoop; anything else plays once and
    // 0 is returned. *loops says which the row was.
    uint32_t playSoundEntry(uint32_t soundEntryId, const glm::vec3& pos, bool keepLoop,
                            bool* loops = nullptr);
    // World transform of a kit slot on a unit: its attachment point when the
    // unit is drawn and has one, else the feet raised to the slot's height.
    // False when only the fallback was available.
    bool slotTransform(uint8_t slot, uint32_t attachInstanceId, const glm::vec3& feetPos,
                       glm::mat4& out) const;
    void placeSpellMissile(const SpellMissile& missile, const glm::vec3& dir,
                           const MotionOut* motion = nullptr);
    // Start ending one visual: Decay or stop emitting, fade the mesh, keep it
    // until its particles are gone (capped). Sounds fade out.
    void retireVisual(SpellVisualInstance& sv);
    void removeVisualNow(SpellVisualInstance& sv);
    // A landed missile, kept where it stopped until its particles finish.
    void retireMissileModel(uint32_t instanceId);
    // Run a missile's motion script; false when there is none or it failed.
    bool evalMotion(SpellMissile& m, float progress, MotionOut& out);
    bool compileMotion(MotionRow& row, uint32_t motionId);
    float groundHeightAt(const glm::vec3& pos) const;
    void spawnUnitState(uint64_t guid, UnitStateRecord& rec);
    void retireUnitState(uint64_t guid, uint32_t visualId);
    void refreshUnitStates(float deltaTime);

    M2Renderer* m2Renderer_ = nullptr;
    Renderer* renderer_ = nullptr;
    pipeline::AssetManager* cachedAssetManager_ = nullptr;

    std::vector<SpellVisualInstance> activeSpellVisuals_;
    std::vector<PhysicalProjectile> physicalProjectiles_;
    std::vector<SpellMissile> spellMissiles_;
    std::unordered_map<uint32_t, VisualRow> visuals_;  // SpellVisual.dbc id → kits + missile
    std::unordered_map<uint32_t, KitRow> kits_;        // SpellVisualKit.dbc id → effects
    std::unordered_map<std::string, uint32_t> spellVisualModelIds_;   // M2 path → M2Renderer modelId
    std::unordered_set<uint32_t> spellVisualFailedModels_;           // modelIds that failed to load (negative cache)
    std::unordered_map<uint32_t, std::vector<EffectSound>> modelEffectSounds_;  // modelId → $SND events
    uint32_t nextSpellVisualModelId_ = 999000; // Reserved range 999000-999799
    uint32_t nextProjectileModelId_ = 998000;  // Reserved range 998000-998999
    std::unordered_map<std::string, uint32_t> projectileModelIds_;
    std::unordered_map<uint32_t, MotionRow> motions_;  // SpellMissileMotion.dbc id → script
    lua_State* motionLua_ = nullptr;
    bool motionLuaFailed_ = false;
    std::unordered_map<uint64_t, std::vector<UnitStateRecord>> unitStates_;
    UnitInstanceResolver unitInstanceOf_;
    UnitPositionResolver unitFeetOf_;
    float stateRecheckTimer_ = 0.0f;
    std::mt19937 rng_{std::random_device{}()};
    bool spellVisualDbcLoaded_ = false;
    static constexpr float SPELL_VISUAL_MAX_DURATION = 5.0f;
    static constexpr float SPELL_VISUAL_DEFAULT_DURATION = 2.0f;
    static constexpr float SPELL_MISSILE_MAX_DURATION = 6.0f;
    static constexpr float SPELL_PRECAST_MAX_DURATION = 60.0f;
    static constexpr float SPELL_AREA_MAX_DURATION = 120.0f;
    // How long an ended effect may keep its particles alive.
    static constexpr float SPELL_LINGER_CAP = 1.5f;
    static constexpr float SPELL_MESH_FADE = 0.25f;
};

} // namespace rendering
} // namespace wowee
