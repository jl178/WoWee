#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <glm/glm.hpp>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class M2Renderer;
class Renderer;
class CharacterRenderer;

class SpellVisualSystem {
public:
    SpellVisualSystem() = default;
    ~SpellVisualSystem() = default;

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

    // The caster's build-up while the cast bar fills (PrecastKit), or while a
    // channel runs (channel=true: ChannelKit). It stays on the caster until
    // cancelPrecastVisuals(casterGuid) - SPELL_GO, failure, interrupt, channel
    // end - or, as a safety net, durationMs plus a grace second.
    void playSpellVisualPrecast(uint32_t visualId, const glm::vec3& worldPosition,
                                uint32_t durationMs = 0, uint32_t attachInstanceId = 0,
                                uint64_t casterGuid = 0, bool channel = false);

    // Whether this visual has a missile model (SpellVisual.dbc MissileModel).
    bool hasSpellMissile(uint32_t visualId);

    // Fly the visual's missile model from the caster to the target at speed
    // yards a second (Spell.dbc Speed). It leaves from the caster attachment
    // named by SpellVisual.MissileAttachment (default: between the hands) and
    // homes on the target attachment named by MissileDestinationAttachment
    // (usually the chest), following a moving target. start/end are the units'
    // feet, used when a unit is not drawn. With impactAtEnd the impact kit
    // plays on the target when it lands. False when nothing launched.
    bool playSpellMissile(uint32_t visualId, const glm::vec3& start, const glm::vec3& end,
                          float speed, bool impactAtEnd,
                          uint32_t casterInstanceId = 0, uint32_t targetInstanceId = 0);

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

    // Remove the precast/channel build-up of one caster (its cast went off,
    // failed or was interrupted, or its channel ended).
    void cancelPrecastVisuals(uint64_t casterGuid);

    // Remove every caster's precast/channel build-up.
    void cancelAllPrecastVisuals();

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
    // Spell visual effects - transient M2 instances
    struct SpellVisualInstance {
        uint32_t instanceId;
        float elapsed;
        float duration;  // per-instance lifetime in seconds (from M2 anim or default)
        bool isPrecast;  // true for precast/channel effects (removed on cancel/interrupt)
        uint8_t slot;    // KitSlot it plays in (decides the attachment point)
        uint32_t attachInstanceId;  // CharacterRenderer instance it rides (0 = static)
        uint64_t ownerGuid;  // caster, for per-caster precast cancel (0 = none)
    };

    struct KitEffect { uint8_t slot; std::string path; };
    struct KitRow {
        std::vector<KitEffect> effects;
        uint32_t animId = 0;
    };
    struct VisualRow {
        uint32_t precastKit = 0, castKit = 0, impactKit = 0, channelKit = 0;
        int32_t missileAttachment = -1;      // KitSlot, -1 = default (the hands)
        int32_t missileDestination = SLOT_CHEST;
        std::string missilePath;
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
        glm::vec3 pos{0.0f};        // where it is now
        glm::vec3 end{0.0f};        // where it is going (re-aimed each frame)
        glm::vec3 endFeet{0.0f};    // target's feet when it launched (fallback)
        uint32_t targetInstanceId = 0;
        int32_t destSlot = SLOT_CHEST;
        float speed = 0.0f;
        float elapsed = 0.0f;
        bool impactAtEnd = false;
    };

    void loadSpellVisualDbc();
    // Model ID for a cast, impact or missile model, loading it on first use.
    // 0 when it cannot be loaded.
    uint32_t loadSpellModel(const std::string& modelPath);
    // Spawn every effect of one kit on a unit. isPrecast/ownerGuid/lifetime
    // mark build-up effects; lifetime <= 0 means the model's own length.
    void spawnKit(uint32_t kitId, const glm::vec3& feetPos, uint32_t attachInstanceId,
                  bool isPrecast, uint64_t ownerGuid, float lifetime);
    // World transform of a kit slot on a unit: its attachment point when the
    // unit is drawn and has one, else the feet raised to the slot's height.
    // False when only the fallback was available.
    bool slotTransform(uint8_t slot, uint32_t attachInstanceId, const glm::vec3& feetPos,
                       glm::mat4& out) const;
    void placeSpellMissile(const SpellMissile& missile, const glm::vec3& dir);

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
    uint32_t nextSpellVisualModelId_ = 999000; // Reserved range 999000-999799
    uint32_t nextProjectileModelId_ = 998000;  // Reserved range 998000-998999
    std::unordered_map<std::string, uint32_t> projectileModelIds_;
    bool spellVisualDbcLoaded_ = false;
    static constexpr float SPELL_VISUAL_MAX_DURATION = 5.0f;
    static constexpr float SPELL_VISUAL_DEFAULT_DURATION = 2.0f;
    static constexpr float SPELL_MISSILE_MAX_DURATION = 6.0f;
    static constexpr float SPELL_PRECAST_MAX_DURATION = 60.0f;
};

} // namespace rendering
} // namespace wowee
