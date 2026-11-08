#pragma once

#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <glm/glm.hpp>

#include "rendering/spell_chain_effect.hpp"
#include "rendering/spell_missile_rules.hpp"

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

    // Play a SpellVisual's CastKit (useImpactKit=false) or its impact on a
    // unit (true). Every effect slot the kit fills (head, chest, base, both
    // hands, breath, weapons, specials) spawns its own model at that unit's
    // matching attachment point. attachInstanceId is the unit's
    // CharacterRenderer instance (0 = not drawn: the effects stand at
    // worldPosition, the unit's feet, raised to the slot's usual height).
    // The impact is the stock client's choice of kit (Wow.exe 0x800D00):
    // TargetImpactKit on a target, CasterImpactKit on a caster hit by its
    // own spell, ImpactKit where the visual names neither.
    //
    // unitGuid is the unit the kit plays on, for the kit's character
    // procedures (SpellVisualKit CharProc: a tint, a flash, a chain effect).
    // targets is the cast's hit list, which a cast kit's chain effect runs
    // through (Chain Lightning: caster, first hit, second hit...).
    void playSpellVisual(uint32_t visualId, const glm::vec3& worldPosition,
                         bool useImpactKit = false, uint32_t attachInstanceId = 0,
                         bool targetIsCaster = false, uint64_t unitGuid = 0,
                         const std::vector<uint64_t>* targets = nullptr);

    // Play one SpellVisualKit by its own id - what SMSG_PLAY_SPELL_VISUAL and
    // SMSG_PLAY_SPELL_IMPACT carry (SpellVisualKit.dbc, not SpellVisual.dbc).
    void playSpellVisualKit(uint32_t kitId, const glm::vec3& worldPosition,
                            uint32_t attachInstanceId = 0);

    // Play one effect model on a unit's base, as the stock client plays its
    // hardcoded SpellVisualEffectName rows - 21 "Unit Level Up"
    // (Spells\LevelUp\LevelUp.m2) on anyone who levels, 4392 "Achievement
    // Base" (Spells\Achievement_OnRoot.m2) on anyone who earns one. Its $SND
    // events play at it as they come due, like any kit model's. False when
    // the model cannot be loaded. `slot` is the KitSlot it rides (a uint8_t:
    // the enum is declared below), the base by default; the cold breath
    // (0x732650) rides the breath point.
    bool playEffectModel(const std::string& modelPath, const glm::vec3& feetPos,
                         uint32_t attachInstanceId = 0, uint8_t slot = 2);

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

    // Whether this visual flies a missile (Wow.exe 0x7FFFB0): SpellVisual
    // .HasMissile set and a MissileModel to fly, or ammo named by the
    // SPELL_GO (a bow's visual flies nothing itself; the arrow is the ammo).
    // (Divine Spirit's row names a model with HasMissile 0; the stock client
    // shows nothing.)
    bool hasSpellMissile(uint32_t visualId, uint32_t ammoDisplayId = 0);

    // The visual a spell plays from this caster (CGUnit_C vtable +0xBC,
    // Wow.exe 0x720F80): a spell that uses the ranged slot (Attributes 0x2)
    // fills every kit and missile column its own visual leaves empty from
    // the ranged weapon's ItemDisplayInfo.SpellVisualID - Auto Shot, Shoot
    // and Throw have no visual of their own, and a wand's is its school bolt
    // (fire, shadow, frost...), a gun's its report, a bow's its draw.
    // spellVisualId 0 is a spell with none; rangedDisplayId the caster's
    // ranged item's display. Returns the visual to use: spellVisualId when the
    // weapon adds nothing, else a made-up id for the merged row.
    uint32_t rangedItemVisual(uint32_t spellVisualId, uint32_t rangedDisplayId);

    // A unit's item in slot 0 (main hand), 1 (off hand) or 2 (ranged): its
    // ItemDisplayInfo id and InventoryType (CGUnit_C vtable +0x134 / +300).
    // For the missiles that are the caster's weapons (MissileModel -1..-3).
    using UnitItemResolver = std::function<bool(uint64_t guid, uint32_t slot, uint32_t& displayId,
                                                uint32_t& inventoryType)>;
    void setUnitItemResolver(UnitItemResolver r) { unitItemOf_ = std::move(r); }
    [[nodiscard]] bool hasUnitItemResolver() const { return static_cast<bool>(unitItemOf_); }
    // The display of a unit's item in a slot, 0 when none (or no resolver).
    uint32_t unitItemDisplay(uint64_t guid, uint32_t slot);

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
    //
    // launchDelay: the missile waits that long before it leaves - the time of
    // the release event ($CSL/$CSR/$CST) in the caster's cast animation
    // (releaseAnim), as the stock client holds a missile until the hand lets
    // it go (Wow.exe 0x734390). It then leaves from that event's point and
    // flies faster to land when it would have (0x7022D0: at most twice the
    // speed). targetGuid is for the impact callback.
    //
    // The model is the visual's MissileModel (scaled by its
    // SpellVisualEffectName.Scale), or an item (Wow.exe 0x732FF0): -1/-2/-3
    // the caster's (casterGuid) main hand, off hand or ranged item, -4/-5
    // the ammo SPELL_GO names (ammoDisplayId, ammoInventoryType), and with
    // no model at all the ammo's flight model - the arrow or bullet of a
    // bow or gun, a thrown weapon. They all fly the same way: the motion
    // script spins a thrown axe, the parabola lobs it.
    bool playSpellMissile(uint32_t visualId, const glm::vec3& start, const glm::vec3& end,
                          float speed, bool impactAtEnd,
                          uint32_t casterInstanceId = 0, uint32_t targetInstanceId = 0,
                          bool groundTarget = false, float launchDelay = 0.0f,
                          uint32_t releaseAnim = 0, uint64_t targetGuid = 0,
                          uint64_t casterGuid = 0, uint32_t ammoDisplayId = 0,
                          uint32_t ammoInventoryType = 0);

    // Called as a missile carrying its impact lands on a unit (targetGuid as
    // given to playSpellMissile): the stock client makes a living target
    // flinch then (Wow.exe 0x702DB0), not when the damage was announced.
    using MissileImpactCallback = std::function<void(uint64_t targetGuid, uint32_t visualId)>;
    void setMissileImpactCallback(MissileImpactCallback cb) { missileImpactCallback_ = std::move(cb); }
    [[nodiscard]] bool hasMissileImpactCallback() const { return static_cast<bool>(missileImpactCallback_); }

    // SpellVisual.Flags (0 when unknown). 0x40: the target does not flinch
    // when the missile hits; 0x200: the missile attachments are raw M2
    // attachment ids, not the stock table's indices.
    uint32_t visualFlags(uint32_t visualId);

    // A spell cast at a point on the ground (SPELL_GO's destination location):
    // InstantAreaKit there now, ImpactAreaKit when it lands (now, unless a
    // missile is carrying it - impactNow=false), and PersistentAreaKit kept
    // there for lifetimeSec. channelOwnerGuid != 0 marks a channel's area
    // (Blizzard, Rain of Fire): it also ends with cancelChannelAreaVisuals.
    // radius: the area's (the spell's effect radius), where its persistent
    // kit's shards fall (CharProc 9).
    void playSpellAreaVisual(uint32_t visualId, const glm::vec3& groundPos, float lifetimeSec,
                             uint64_t channelOwnerGuid, bool impactNow, float radius = 0.0f);
    // End a caster's channelled ground areas (the channel ended or broke).
    void cancelChannelAreaVisuals(uint64_t casterGuid);

    // Aura state visuals. The unit's current aura visuals (Spell.SpellVisualID
    // of every aura it has) - each one's StateKit loops on the unit while it
    // is in the list; one that leaves plays its StateDoneKit. The resolvers
    // find a unit's CharacterRenderer instance and feet as it comes and goes.
    using UnitInstanceResolver = std::function<uint32_t(uint64_t guid)>;
    using UnitPositionResolver = std::function<bool(uint64_t guid, glm::vec3& feet)>;
    void setUnitResolvers(UnitInstanceResolver instanceOf, UnitPositionResolver feetOf);
    /// Who a unit channels at (UNIT_FIELD_CHANNEL_OBJECT), 0 for nobody: a
    /// channel kit's chain effect runs to them (Drain Life, Mind Flay).
    using ChannelTargetResolver = std::function<uint64_t(uint64_t casterGuid)>;
    void setChannelTargetResolver(ChannelTargetResolver r) { channelTargetOf_ = std::move(r); }
    [[nodiscard]] bool hasUnitResolvers() const { return static_cast<bool>(unitInstanceOf_); }
    void syncUnitStateVisuals(uint64_t guid, const std::vector<uint32_t>& visualIds);
    /// The animation the unit's aura state kits ask it to hold (the newest
    /// one that names one), 0 when none does: Bladestorm's Whirlwind loop.
    /// Mount auras are left out; the rider pose has its own path.
    [[nodiscard]] uint32_t unitStateAnim(uint64_t guid) const;
    // Forget every unit's aura states (logout, aura caches dropped).
    void clearAllUnitStateVisuals();

    // Caster animations named by the visual's kits (SpellVisualKit.AnimID),
    // 0 where the kit names none: e.g. Fireball 51/53 (ReadySpellDirected,
    // SpellCastDirected), Flash Heal 52/54 (the Omni pair), Arcane Missiles'
    // channel 124 (ChannelCastDirected).
    // precastStart is the precast kit's StartAnimID: the one-shot before its
    // loop. Stock plays only that; a kit without one goes straight to the loop.
    struct KitAnimations {
        uint32_t precast = 0; uint32_t cast = 0; uint32_t channel = 0;
        uint32_t precastStart = 0; bool hasPrecastKit = false;
        uint32_t impact = 0;  // the impact kit's, on the target (CombatWound for Frostbolt)
        uint32_t state = 0;   // the state kit's: a mount aura's is the rider's pose (0x724820)
    };
    KitAnimations getKitAnimations(uint32_t visualId);

    // Advance lifetime timers and remove expired instances.
    void update(float deltaTime);

    // End the precast/channel build-up of one caster (its cast went off,
    // failed or was interrupted, or its channel ended). It winds down - Decay,
    // no new particles, the live ones finish (at most ~1.5 s) - and its
    // looping sound fades.
    void cancelPrecastVisuals(uint64_t casterGuid);

    // One SpellChainEffects row (null when there is none) - for tests and
    // the audit tools.
    const ChainEffectRow* chainEffectRow(uint32_t id);
    // How many chain effects (bolt sets) are alive, and how many bolts they
    // draw this frame.
    size_t activeChainEffectCount() const { return chains_.size(); }

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
        // The kit's character procedures (tints, chain effects, area shards)
        // live as long as this does; 0 = none.
        uint32_t procToken = 0;
        // A missile targeting kit: ends as its missile lands (0x6FF410).
        uint32_t missileTag = 0;
        // A SpellVisualKitModelAttach model: the M2 attachment it rides
        // (-1: by its kit slot) and its offset and turn from there.
        int32_t rawAttachment = -1;
        glm::mat4 attachLocal{1.0f};
        // Birth (sequence 0), then Hold (158) looping for a kit that outlives
        // its birth, then Decay (159) as it ends - the stock spell models'
        // three sequences. Without them it just stops emitting. A precast kit
        // never takes Hold: the stock client replays its birth (Wow.exe
        // 0x7435A0); state and channel kits do (0x7449C0).
        bool hasHold = false, hasDecay = false, holding = false, noHold = false;
        float birthDuration = 0.0f;
        // Ending: no new particles; kept until the live ones have run out.
        bool retiring = false;
        float linger = 0.0f, lingerCap = 0.0f, decayDuration = 0.0f;
        bool emissionStopped = false;
        // The model's $SND events (modelEffectSounds_), the next one due, and
        // where the model stands for them.
        uint32_t modelId = 0;
        uint32_t nextEffectSound = 0;
        uint32_t nextEffectShake = 0;  // the next $SHK camera shake due
        glm::vec3 effectPos{0.0f};
    };
    // A model's $SND events: SoundEntries id at ms into its birth sequence.
    struct EffectSound { uint32_t soundEntryId = 0; uint32_t timeMs = 0; };

    struct KitEffect { uint8_t slot; std::string path; };
    // SpellVisualKit CharProc[4] with its four parameters (Wow.exe 0x7265C0):
    // 0/12 a chain effect, 1 a tint, 8 a weapon swing trail, 9 an area's
    // falling shards, 11 a frozen pose, 13 a flash, 14 transparency, 15 scale.
    struct KitProc { int32_t type = -1; float p[4] = {0.0f, 0.0f, 0.0f, 0.0f}; };
    // SpellVisualKitModelAttach: one more model, at an attachment (-1 the
    // kit's point) with an offset and a turn (0x7FA9F0, 0x745230).
    struct KitModelAttach {
        std::string path;
        int32_t attachment = -1;
        glm::vec3 offset{0.0f};
        float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
    };
    struct KitRow {
        std::vector<KitEffect> effects;
        KitProc procs[4];
        bool hasProcs = false;
        uint32_t shakeId = 0;     // SpellEffectCameraShakes.dbc
        std::vector<KitModelAttach> attaches;
        std::string worldEffect;  // WorldEffect: stands in the world at the kit's point
        uint32_t animId = 0;
        uint32_t startAnimId = 0;
        uint32_t soundId = 0;     // SoundEntries.dbc
        uint32_t flags = 0;       // SpellVisualKit.Flags; 0x20: a state loops Stand, not Hold
    };
    struct VisualRow {
        uint32_t precastKit = 0, castKit = 0, impactKit = 0, channelKit = 0;
        uint32_t stateKit = 0, stateDoneKit = 0;
        uint32_t targetImpactKit = 0;  // the impact on a target, in place of ImpactKit
        uint32_t casterImpactKit = 0;  // the impact on the caster itself, in place of ImpactKit
        uint32_t instantAreaKit = 0, impactAreaKit = 0, persistentAreaKit = 0;
        uint32_t flags = 0;                  // SpellVisual.Flags
        // M2 attachment ids, resolved from the DBC's indices through the stock
        // table (see kMissileAttachmentIds). -1: the default - the release
        // event's point for the source, the chest for the destination.
        int32_t missileAttachment = -1;
        int32_t missileDestination = 34;
        std::string missilePath;
        float missileScale = 1.0f;           // its SpellVisualEffectName.Scale
        // MissileModel as the row has it: below 0 an item flies (Wow.exe
        // 0x732FF0): -1/-2/-3 the caster's main hand, off hand, ranged item
        // (Heroic Throw, Shattering Throw), -4/-5 the ammo.
        int32_t missileModelRaw = 0;
        uint32_t missileTargetingKit = 0;    // plays from launch to landing
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

    struct SpellMissile {
        uint32_t instanceId = 0;
        uint32_t visualId = 0;
        glm::vec3 pos{0.0f};        // the homing point: straight line to the target
        glm::vec3 shown{0.0f};      // pos plus the motion script's offsets
        glm::vec3 launch{0.0f};     // where it left from
        glm::vec3 end{0.0f};        // where it is going (re-aimed each frame)
        glm::vec3 endFeet{0.0f};    // target's feet when it launched (fallback)
        uint32_t targetInstanceId = 0;
        uint64_t targetGuid = 0;
        int32_t destAttachment = 34;  // M2 attachment id on the target
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
        // An item model (a weapon, an arrow): a CharacterRenderer instance,
        // which dresses it in the item's texture; else an M2Renderer one.
        bool itemModel = false;
        float baseScale = 1.0f;     // SpellVisualEffectName.Scale
        uint32_t targetingTag = 0;  // its missile targeting kit, ended on landing
    };

    // A missile waiting for its caster's hand to let it go.
    struct PendingMissile {
        uint32_t visualId = 0;
        glm::vec3 start{0.0f}, end{0.0f};
        float speed = 0.0f;
        bool impactAtEnd = false;
        uint32_t casterInstanceId = 0, targetInstanceId = 0;
        bool groundTarget = false;
        uint32_t releaseAnim = 0;
        uint64_t targetGuid = 0;
        float delay = 0.0f, waited = 0.0f;
        uint64_t casterGuid = 0;
        uint32_t ammoDisplayId = 0, ammoInventoryType = 0;
    };

    // A kit to spawn: where, on whom, for how long, and what it belongs to.
    struct KitSpawn {
        glm::vec3 feetPos{0.0f};
        uint32_t attachInstanceId = 0;
        // Played at a point on the ground (the area kits): the stock client
        // puts only the kit's base and world effects there (Wow.exe 0x745230,
        // flag 0x2000) - a head or hand effect has no unit to sit on.
        bool atPoint = false;
        bool isPrecast = false;
        bool loopBirth = false;   // a precast kit: replay the birth, never Hold
        uint64_t ownerGuid = 0;
        float lifetime = 0.0f;  // <= 0: the model's own length; infinity: until ended
        uint64_t stateGuid = 0;
        uint32_t stateVisual = 0;
        uint64_t areaOwner = 0;
        // The unit the kit plays on (for its character procedures), the
        // visual it came from, and the cast's targets for a chain effect.
        uint64_t unitGuid = 0;
        uint32_t visualId = 0;
        const std::vector<uint64_t>* targets = nullptr;
        bool channelKit = false;  // a channel kit: its chain runs to the channel target
        // Ground areas: the spell's radius, for the falling shards.
        float areaRadius = 0.0f;
        uint32_t missileTag = 0;  // a missile targeting kit (see SpellVisualInstance)
        // A state kit moved onto the unit's new model (a teleport, a display
        // change): the aura did not begin again, so its one-shot sound is
        // not heard again. A looping one restarts with the kit.
        bool reattach = false;
    };

    // An aura state visual a unit should have, and the instance it is on.
    struct UnitStateRecord {
        uint32_t visualId = 0;
        uint32_t attachInstanceId = 0;
        bool spawned = false;
        bool sounded = false;   ///< its kit has been shown (and heard) once
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
                       const KitSpawn& spawn, const KitModelAttach* attach = nullptr);
    // Play a SoundEntries row at pos. A looping row (flag 0x200) is kept and
    // its AudioEngine id returned when keepLoop; anything else plays once and
    // 0 is returned. *loops says which the row was. own: on the local
    // character (audio::SoundOptions::ownCharacter).
    uint32_t playSoundEntry(uint32_t soundEntryId, const glm::vec3& pos, bool keepLoop,
                            bool* loops = nullptr, bool own = false);
    // World transform of a kit slot on a unit: its attachment point when the
    // unit is drawn and has one, else the feet raised to the slot's height.
    // False when only the fallback was available.
    bool slotTransform(uint8_t slot, uint32_t attachInstanceId, const glm::vec3& feetPos,
                       glm::mat4& out) const;
    // The same for one M2 attachment id (a missile's source or destination):
    // the stock fallbacks 15 (ChestBloodFront) then 19 (Base) where the model
    // lacks it, else the feet raised to fallbackHeight.
    bool attachmentTransform(int32_t attachmentId, uint32_t attachInstanceId,
                             const glm::vec3& feetPos, float fallbackHeight,
                             glm::mat4& out) const;
    // Launch one missile now (p.waited: how long it was held for its release).
    bool launchSpellMissile(const PendingMissile& p);
    // The item model a missile is (MissileModel -1..-5, or ammo): a
    // CharacterRenderer model id dressed in the item's texture, 0 when the
    // display gives none (the stock flies an ErrorCube).
    uint32_t itemMissileModel(const VisualRow& v, const PendingMissile& p);
    uint32_t loadItemMissileModel(const missilerules::ItemMissilePaths& paths);
    // A missile that landed or was dropped: its model goes, its targeting
    // kit ends.
    void endMissile(const SpellMissile& m, bool retireModel);
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

    // --- Character procedures (SpellVisualKit CharProc) -------------------
    // Run a kit's procedures; the carrier visual (procToken) ends them.
    uint32_t applyKitProcs(uint32_t kitId, const KitRow& kit, const KitSpawn& spawn);
    void endProcs(uint32_t token);
    // A chain effect: one bolt set (a SpellChainEffects row and its combo
    // rows each make one) along the hops caster -> target -> target...
    struct ChainHop {
        uint16_t from = 0, to = 1;
        float startMs = 0.0f, endMs = 0.0f;
        std::unique_ptr<LightningBolt> bolt;
    };
    struct ChainEffect {
        const ChainEffectRow* row = nullptr;
        std::vector<uint64_t> points;  // [0] the source unit, then the targets
        uint64_t channelCaster = 0;    // != 0: points[1] follows its channel target
        uint32_t visualId = 0;
        uint32_t castAnim = 0;         // the caster's kit animation, for its $CSL
        bool persistent = false;       // lasts as long as its kit (CharParamTwo)
        float ageMs = 0.0f, endMs = 0.0f;
        uint32_t token = 0;
        std::vector<ChainHop> hops;
    };
    void startChainEffect(uint32_t rowId, const KitProc& proc, const KitSpawn& spawn,
                          uint32_t token, uint32_t castAnim);
    // A strip to draw this frame: the texture by path, the stock blend.
    // layer: the stock draws bolts by RenderLayer, 0 to 3 (0x9AB070); unfogged:
    // the bolts draw with fog off (0x9AB070 sets GxRs_Fog 0).
    struct StripOut {
        std::string texture; uint8_t gxBlend = 2; int32_t layer = 0; bool unfogged = false;
        std::vector<ChainStripVertex> verts;
    };
    void updateChainEffects(float deltaTime, std::vector<StripOut>& strips);
    void submitStrips(std::vector<StripOut>& strips);
    bool chainSourcePoint(const ChainEffect& c, uint64_t guid, glm::vec3& out);
    bool chainTargetPoint(const ChainEffect& c, uint64_t guid, glm::vec3& out);
    // 3/4 of a unit's height above its feet (Wow.exe 0x717AD0 x 0.75): where
    // a bolt ends on a unit without the attachment it asks for.
    glm::vec3 unitThreeQuarterPoint(uint32_t inst, const glm::vec3& feet) const;
    void loadChainEffects();
    // A tint on a unit's model (CharProc 1): the newest one shows.
    struct UnitTint { uint32_t instanceId = 0; uint32_t token = 0; uint32_t key = 0; glm::vec3 color{1.0f}; };
    // A flash (CharProc 13): the colour for hold ms, then back to white over fade ms.
    struct UnitFlash { uint32_t instanceId = 0; glm::vec3 color{1.0f}; float holdMs = 0.0f, fadeMs = 0.0f, ageMs = 0.0f; };
    void updateUnitTints(float deltaTime);
    // An area's falling shards (CharProc 9, the stock BlizzardObject,
    // Wow.exe 0x7FB5B0 and 0x7FC220): ParamOne a second, anywhere in the
    // radius, each one model played once.
    struct ShardEmitter {
        std::string model;
        glm::vec3 center{0.0f};
        float radius = 0.0f;
        float rate = 0.0f;
        float accum = 0.0f;
        uint32_t token = 0;
    };
    struct PendingShard { std::string model; glm::vec3 pos{0.0f}; float delay = 0.0f; };
    void updateShards(float deltaTime);
    // A weapon's swing trail (CharProc 8, Wow.exe 0x715BA0 -> 0x7E4FF0):
    // its $WTB/$WTT ends laid down each frame for DurationMs, fading as it
    // goes (0x7E4CE0).
    struct SwingTrail {
        uint32_t instanceId = 0;
        size_t weapon = 0;
        glm::vec3 color{1.0f};
        float alpha = 255.0f;     // the colour's alpha byte (CharParamThree)
        float durationMs = 0.0f, elapsedMs = 0.0f;
        std::vector<std::pair<glm::vec3, glm::vec3>> edges;  // oldest first
    };
    void updateSwingTrails(float deltaTime, std::vector<StripOut>& strips);
    std::vector<SwingTrail> swingTrails_;
    // A held pose (CharProc 11): the instance and the kit it belongs to.
    struct FrozenPose { uint32_t instanceId = 0; uint32_t token = 0; };
    std::vector<FrozenPose> frozenPoses_;
    void loadAreaModels();
    // Camera shakes: SpellEffectCameraShakes -> up to three CameraShakes.
    void playCameraShake(uint32_t spellShakeId, const glm::vec3& source);
    void loadCameraShakes();

    std::vector<ChainEffect> chains_;
    std::unordered_map<uint32_t, ChainEffectRow> chainRows_;
    bool chainRowsLoaded_ = false;
    std::vector<UnitTint> unitTints_;
    std::vector<UnitFlash> unitFlashes_;
    std::unordered_set<uint32_t> tintedInstances_;
    std::vector<ShardEmitter> shardEmitters_;
    std::vector<PendingShard> pendingShards_;
    std::vector<std::string> areaModels_;  // SpellVisualKitAreaModel, by row order
    bool areaModelsLoaded_ = false;
    struct CameraShakeRow {
        int32_t type = 0, direction = 0;
        float amplitude = 0.0f, frequency = 0.0f, duration = 0.0f, phase = 0.0f, coefficient = 0.0f;
    };
    std::unordered_map<uint32_t, std::vector<CameraShakeRow>> spellShakes_;
    bool cameraShakesLoaded_ = false;
    bool hadStrips_ = false;
    uint32_t nextProcToken_ = 1;
    ChannelTargetResolver channelTargetOf_;

    M2Renderer* m2Renderer_ = nullptr;
    Renderer* renderer_ = nullptr;
    pipeline::AssetManager* cachedAssetManager_ = nullptr;

    std::vector<SpellVisualInstance> activeSpellVisuals_;
    std::vector<SpellMissile> spellMissiles_;
    std::vector<PendingMissile> pendingMissiles_;
    MissileImpactCallback missileImpactCallback_;
    std::unordered_map<uint32_t, VisualRow> visuals_;  // SpellVisual.dbc id → kits + missile
    std::unordered_map<uint32_t, KitRow> kits_;        // SpellVisualKit.dbc id → effects
    std::unordered_map<std::string, uint32_t> spellVisualModelIds_;   // M2 path → M2Renderer modelId
    std::unordered_set<uint32_t> spellVisualFailedModels_;           // modelIds that failed to load (negative cache)
    std::unordered_map<uint32_t, std::vector<EffectSound>> modelEffectSounds_;  // modelId → $SND events
    // modelId → $SHK events (SpellEffectCameraShakes id in soundEntryId):
    // the meteor and infernal impacts shake the camera (Wow.exe 0x704C30).
    std::unordered_map<uint32_t, std::vector<EffectSound>> modelEffectShakes_;
    uint32_t nextSpellVisualModelId_ = 999000; // Reserved range 999000-999799
    uint32_t nextProjectileModelId_ = 998000;  // Reserved range 998000-998999
    std::unordered_map<std::string, uint32_t> projectileModelIds_;  // item missile models
    // SpellVisualEffectName: id -> model path and Scale (column 4).
    std::unordered_map<uint32_t, std::string> effectPaths_;
    std::unordered_map<uint32_t, float> effectScales_;
    // The raw 3.3.5a SpellVisual rows (32 columns), for the ranged merge, and
    // the merged rows made so far: (spell visual << 32 | item visual) -> id.
    std::unordered_map<uint32_t, missilerules::VisualColumns> rawVisuals_;
    std::unordered_map<uint64_t, uint32_t> mergedVisuals_;
    std::unordered_map<uint32_t, uint32_t> itemDisplayVisuals_;  // display -> SpellVisualID
    uint32_t nextMergedVisualId_ = 0x40000000u;
    VisualRow visualRowFromColumns(const missilerules::VisualColumns& c) const;
    bool itemDisplayNames(uint32_t displayId, missilerules::ItemDisplayNames& out);
    UnitItemResolver unitItemOf_;
    uint32_t nextMissileTag_ = 1;
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
    // The stock CEffect fades an ended model out over 0.15 s (Wow.exe
    // 0x6F87C0, the constant at 0x9F23D4).
    static constexpr float SPELL_MESH_FADE = 0.15f;
    // A missile waits at most this long for its release (Wow.exe 0x734390).
    static constexpr float SPELL_RELEASE_MAX_WAIT = 2.0f;
};

} // namespace rendering
} // namespace wowee
