#pragma once

#include <vector>
#include <memory>
#include <string>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <glm/vec3.hpp>

namespace wowee {
namespace pipeline {
class AssetManager;
}

namespace audio {

class SoundData;

/// One equipped item as the combat sounds need it (Item.dbc columns).
struct CombatItem {
    uint32_t itemClass = 0;      ///< 2 weapon, 4 armour
    uint32_t subclass = 0;
    uint32_t material = 0;       ///< 1 metal, 2 wood, 5 chain, 6 plate, 7 cloth, 8 leather
    uint32_t inventoryType = 0;
    [[nodiscard]] bool isWeapon() const { return itemClass == 2; }
    [[nodiscard]] bool isShield() const { return inventoryType == 14 || (itemClass == 4 && subclass == 6); }
};

/// What the melee sounds need to know about one unit; filled by the core
/// layer, which knows the entities, their items and their render instances.
struct CombatUnitInfo {
    uint32_t instanceId = 0;     ///< render instance, 0 when the unit is not drawn
    glm::vec3 position{0.0f};    ///< render space
    uint32_t displayId = 0;      ///< CreatureDisplayInfo (players: their model's)
    bool isPlayer = false;
    bool isLocalPlayer = false;
    CombatItem mainHand, offHand;
    uint32_t chestArmorSubclass = 0;  ///< players' worn chest: 1 cloth 2 leather 3 mail 4 plate
    /// The animation the unit is playing now carries $CSS (swoosh) / $CAH
    /// (the blow lands) events. Asked right after the swing animation starts.
    bool animHasSwingEvent = false;
    bool animHasHitEvent = false;
};

class CombatSoundManager {
public:
    CombatSoundManager() = default;
    ~CombatSoundManager() = default;

    // Initialization
    bool initialize(pipeline::AssetManager* assets);
    void shutdown();

    // Volume control
    void setVolumeScale(float scale);
    [[nodiscard]] float getVolumeScale() const { return volumeScale_; }

    // ---- Table-driven melee and body sounds (the stock client's rules) ----
    //
    //  - The swoosh is WeaponSwingSounds2's, by the swinging weapon's swing
    //    type (light / medium / heavy) and whether it is a crit. It plays at
    //    the attack animation's $CSS event.
    //  - The blow is WeaponImpactSounds' row for the weapon subclass (13 Fist
    //    for an empty hand or a creature's claws) and the weapon's material,
    //    in the column for what it struck: the victim's body (flesh, a
    //    player's mail or plate, CreatureImpactType stone), the victim's
    //    shield on a block, the victim's weapon on a parry. A dodge and a miss
    //    are the swoosh alone. It plays at the attack animation's $CAH event.
    //  - The victim's injury voice (CreatureSoundData) plays with the blow:
    //    the crit one on a crit, the normal one on some hits.
    // Animations without those events play the sounds when the server
    // reports the swing.
    enum class MeleeOutcome { HIT, CRIT, MISS, DODGE, PARRY, BLOCK, NONE };
    using UnitResolver = std::function<bool(uint64_t guid, CombatUnitInfo& out)>;
    void setSoundData(const SoundData* data) { soundData_ = data; }
    void setUnitResolver(UnitResolver resolver) { unitResolver_ = std::move(resolver); }
    [[nodiscard]] bool hasUnitResolver() const { return static_cast<bool>(unitResolver_); }
    /// One SMSG_ATTACKERSTATEUPDATE (or a melee ability's hit), for any fight
    /// in view. offHand: HITINFO_OFFHAND. animated: a swing animation was
    /// started for it. injureVictim: false when the victim's voice is played
    /// elsewhere (the local character's, by the activity sounds). ourCombat:
    /// the local character is in it - the only fights the fixed fallback bank
    /// (no tables, no unit resolver) is played for.
    /// withSwoosh false: the swing's own swoosh is left out (a melee ability
    /// whose cast kit plays its special swing, SwingWeaponSpecialWarrior).
    void onMelee(uint64_t attackerGuid, uint64_t victimGuid, MeleeOutcome outcome, bool offHand,
                 bool animated, bool injureVictim, bool ourCombat, bool withSwoosh = true);
    /// A unit other than the local character took spell damage: its injury
    /// voice, by the same chance rules as a melee blow.
    void onWounded(uint64_t victimGuid, bool crit);
    /// A render instance passed an M2 event; the tags are the four bytes of
    /// the event name read as a little-endian uint32 ('$CSS', '$CAH').
    void onMeleeAnimEvent(uint32_t instanceId, uint32_t tag);
    /// Plays what is still waiting on an animation event that never came.
    void update();

    /// The creature-sound columns the model events and deaths name.
    enum class BodySound { DEATH, WING_FLAP, FIDGET, CUSTOM_ATTACK, INJURY, INJURY_CRIT };
    void playBodySound(uint32_t displayId, BodySound which, uint32_t index, const glm::vec3& position);
    /// A $FSD footfall: FootstepTerrainLookup for the display's footstep set
    /// on this ground (TerrainType.SoundID), the splash column in water.
    void playFootstep(uint32_t displayId, uint32_t terrainSoundId, bool inWater, const glm::vec3& position);
    /// Play a SoundEntries row at a point. False when it has no files.
    /// `vary` turns pitch by up to 4% and level by up to 1.5 dB and never
    /// picks the file this row played last - for weapon impacts, which come
    /// in runs where one sample on repeat is easy to hear.
    bool playEntry3D(uint32_t soundId, const glm::vec3& position, float volumeMul = 1.0f,
                     bool vary = false);

    // Weapon swing sounds (whoosh sounds before impact)
    enum class WeaponSize {
        SMALL,   // 1H weapons (daggers, swords, maces)
        MEDIUM,  // 2H weapons (2H swords, axes)
        LARGE    // 2H heavy weapons (polearms, staves)
    };

    void playWeaponSwing(WeaponSize size, bool isCrit = false);
    void playWeaponMiss(bool twoHanded = false);

    // Impact sounds (when weapon hits target)
    enum class ImpactType {
        FLESH,        // Hit unarmored/cloth
        CHAIN,        // Hit chain armor
        PLATE,        // Hit plate armor
        SHIELD,       // Hit shield
        METAL_WEAPON, // Parry/weapon clash
        WOOD,         // Hit wooden objects
        STONE         // Hit stone/rock
    };

    void playImpact(WeaponSize weaponSize, ImpactType impactType, bool isCrit = false);


    // Player character vocals
    enum class PlayerRace {
        BLOOD_ELF_MALE,
        BLOOD_ELF_FEMALE,
        DRAENEI_MALE,
        DRAENEI_FEMALE
    };


private:
    struct CombatSample {
        std::string path;
        std::vector<uint8_t> data;
        bool loaded;
    };

    // Weapon swing sound libraries
    std::vector<CombatSample> swingSmallSounds_;
    std::vector<CombatSample> swingMediumSounds_;
    std::vector<CombatSample> swingLargeSounds_;
    std::vector<CombatSample> swingSmallCritSounds_;
    std::vector<CombatSample> swingMediumCritSounds_;
    std::vector<CombatSample> swingLargeCritSounds_;
    std::vector<CombatSample> missWhoosh1HSounds_;
    std::vector<CombatSample> missWhoosh2HSounds_;

    // Impact sound libraries (using 1H axe as base - similar across weapon types)
    std::vector<CombatSample> hitFleshSounds_;
    std::vector<CombatSample> hitChainSounds_;
    std::vector<CombatSample> hitPlateSounds_;
    std::vector<CombatSample> hitShieldSounds_;
    std::vector<CombatSample> hitMetalWeaponSounds_;
    std::vector<CombatSample> hitWoodSounds_;
    std::vector<CombatSample> hitStoneSounds_;
    std::vector<CombatSample> hitFleshCritSounds_;
    std::vector<CombatSample> hitChainCritSounds_;
    std::vector<CombatSample> hitPlateCritSounds_;
    std::vector<CombatSample> hitShieldCritSounds_;

    // Emote sounds

    // Player character vocal libraries




    // Table-driven state.
    struct PendingBlow {
        uint32_t swingSound = 0;     ///< WeaponSwingSounds2 row's sound, 0 none
        uint32_t impactSound = 0;    ///< WeaponImpactSounds row's sound, 0 none
        uint32_t injurySound = 0;    ///< victim's injury voice, 0 none
        glm::vec3 attackerPos{0.0f};
        glm::vec3 victimPos{0.0f};
        bool swingPlayed = false;
        bool impactPlayed = false;
        bool impactWithSwing = false; ///< the animation has $CSS but no $CAH
        double deadlineMs = 0.0;     ///< play whatever is left by then
    };
    void playBlowSwing(PendingBlow& blow);
    void playBlowImpact(PendingBlow& blow);
    uint32_t pickInjury(uint64_t victimGuid, uint32_t displayId, bool crit);
    const SoundData* soundData_ = nullptr;
    UnitResolver unitResolver_;
    std::unordered_map<uint32_t, PendingBlow> pendingBlows_;  // by attacker instance
    std::unordered_map<uint64_t, double> lastInjuryMs_;       // by victim
    std::unordered_map<uint32_t, size_t> lastVariedFile_;     // by SoundEntries row; see playEntry3D
    const void* lastImpactLibrary_ = nullptr;                 // the fallback bank, see playImpact
    size_t lastImpactIndex_ = SIZE_MAX;

    // State tracking
    float volumeScale_ = 1.0f;
    bool initialized_ = false;

    // Helper methods
    bool loadSound(const std::string& path, CombatSample& sample, pipeline::AssetManager* assets);
    void playSound(const std::vector<CombatSample>& library, float volumeMultiplier = 1.0f);
    void playRandomSound(const std::vector<CombatSample>& library, float volumeMultiplier = 1.0f);
    /// playRandomSound with the impact jitter: 4% pitch, 1.5 dB, no repeats.
    void playVariedImpact(const std::vector<CombatSample>& library, float volumeMultiplier = 1.0f);
};

} // namespace audio
} // namespace wowee
