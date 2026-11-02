#pragma once

#include "game/character.hpp"
#include "game/game_services.hpp"
#include "pipeline/blp_loader.hpp"
#include "rendering/animation/weapon_type.hpp"
#include "rendering/animation/body_facing.hpp"
#include "rendering/animation/unit_locomotion.hpp"
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <chrono>
#include <optional>
#include <future>
#include <mutex>
#include <glm/glm.hpp>

namespace wowee {

// Forward declarations
namespace rendering { class Renderer; }
namespace pipeline { class AssetManager; struct M2Model; struct WMOModel; }
namespace audio { enum class VoiceType; }
namespace game { class GameHandler; class Unit; }

namespace core {

class Application;

class EntitySpawner {
public:
    EntitySpawner(rendering::Renderer* renderer,
                  pipeline::AssetManager* assetManager,
                  game::GameHandler* gameHandler,
                  game::GameServices* gameServices);
    ~EntitySpawner();

    EntitySpawner(const EntitySpawner&) = delete;
    EntitySpawner& operator=(const EntitySpawner&) = delete;

    // Lifecycle
    void initialize();   // Build DBC lookups
    void update();       // Process all spawn/despawn queues (called from Application::update)
    void shutdown();     // Clear all instances and queues

    // Queue-based spawn API (called from GameHandler callbacks)
    void queueCreatureSpawn(uint64_t guid, uint32_t displayId,
                            float x, float y, float z, float orientation, float scale = 1.0f);
    void queuePlayerSpawn(uint64_t guid, uint8_t raceId, uint8_t genderId,
                          uint32_t appearanceBytes, uint8_t facialFeatures,
                          float x, float y, float z, float orientation);
    void queueGameObjectSpawn(uint64_t guid, uint32_t entry, uint32_t displayId,
                              float x, float y, float z, float orientation, float scale = 1.0f);
    void queuePlayerEquipment(uint64_t guid,
                              const std::array<uint32_t, 19>& displayInfoIds,
                              const std::array<uint8_t, 19>& inventoryTypes);

    // Immediate despawn
    void despawnCreature(uint64_t guid);
    void despawnPlayer(uint64_t guid);
    void despawnGameObject(uint64_t guid);

    // Re-evaluate the animation policy for spawned game objects of this entry.
    // Called when a GAMEOBJECT_QUERY_RESPONSE finally supplies the type that the
    // policy depends on; harmless if nothing of that entry is waiting.
    void onGameObjectInfoReceived(uint32_t entry);

private:
    // Freeze or animate a spawned game object based on its type, deferring the
    // decision when the type has not been queried yet.
    void applyGameObjectAnimationPolicy(uint64_t guid, uint32_t entry, uint32_t instanceId);

public:

    // Clear all queues and instances (logout, reconnect)
    void clearAllQueues();
    void despawnAllCreatures();
    void despawnAllPlayers();
    void despawnAllGameObjects();

    // Full reset - despawns entities, waits for async loads, clears all state.
    // Used by logoutToLogin() and loadOnlineWorldTerrain() for clean slate.
    void resetAllState();

    // Rebuild lookup tables (for reloadExpansionData after expansion change)
    void rebuildLookups();

    // Check if any spawn/async work is still pending
    bool hasWorkPending() const;

    // Status queries
    bool isCreatureSpawned(uint64_t guid) const { return creatureInstances_.count(guid) > 0; }
    bool isCreaturePending(uint64_t guid) const { return pendingCreatureSpawnGuids_.count(guid) > 0; }
    bool isPlayerSpawned(uint64_t guid) const { return playerInstances_.count(guid) > 0; }
    bool isPlayerPending(uint64_t guid) const { return pendingPlayerSpawnGuids_.count(guid) > 0; }
    bool isGameObjectSpawned(uint64_t guid) const { return gameObjectInstances_.count(guid) > 0; }

    /// What the server says a game object is doing: open, closed, destroyed.
    ///
    /// A door is a pose. The state arrives twice - once in the create block for
    /// an object that is already open when it comes into view, and again
    /// whenever it changes - and the first of those used to arrive before the
    /// model existed and was dropped, so every door in the world was drawn in
    /// its closed pose however the server had it.
    void applyGameObjectState(uint64_t guid, uint8_t goState);

    // Quick instance ID lookups (returns 0 if not found)
    uint32_t getCreatureInstanceId(uint64_t guid) const {
        auto it = creatureInstances_.find(guid); return (it != creatureInstances_.end()) ? it->second : 0;
    }
    uint32_t getPlayerInstanceId(uint64_t guid) const {
        auto it = playerInstances_.find(guid); return (it != playerInstances_.end()) ? it->second : 0;
    }

    // Render bounds/position queries (used by click targeting, etc.)
    bool getRenderBoundsForGuid(uint64_t guid, glm::vec3& outCenter, float& outRadius) const;
    uint32_t characterInstanceIdForGuid(uint64_t guid) const;
    bool getRenderFootZForGuid(uint64_t guid, float& outFootZ) const;
    bool getRenderPositionForGuid(uint64_t guid, glm::vec3& outPos) const;

    // Display data lookups
    bool areCreatureLookupsBuilt() const { return creatureLookupsBuilt_; }
    bool areGameObjectLookupsBuilt() const { return gameObjectLookupsBuilt_; }
    std::string getModelPathForDisplayId(uint32_t displayId) const;
    /// The skin textures a creature display wears, by M2 texture type - 11,
    /// 12 and 13 are skin1, skin2 and skin3 in CreatureDisplayInfo.dbc.
    ///
    /// A creature's M2 does not name these: the model declares the slots and
    /// the display row fills them, which is how one model serves a dozen
    /// recolours. Anything loading a creature model outside the spawner needs
    /// them too, or it draws an untextured shape - which is what the target
    /// frame's portrait did until this was shared.
    ///
    /// Paths are resolved against the model's own directory, and only ones the
    /// install actually has are returned.
    std::vector<std::pair<uint32_t, std::string>>
    getCreatureSkinPaths(uint32_t displayId, const std::string& modelPath) const;

    /// A humanoid creature's appearance, if this display is one.
    ///
    /// Most NPCs worth looking at are: a guard, a questgiver, an innkeeper.
    /// CreatureDisplayInfoExtra gives them a race, a sex and the same skin,
    /// face, hair and facial-hair choices a player character has - so the way
    /// to draw one is the way a character is drawn, not the way a creature is.
    /// A creature's skin fields are empty for these, which is why loading one
    /// as a creature gives an untextured shape.
    ///
    /// The appearance bytes are packed as a character's are: skin, face, hair
    /// style and hair colour, one byte each from the bottom up.
    /// The pre-composited skin for a humanoid display, or empty.
    ///
    /// This is the whole appearance already baked - skin, face, hair and the
    /// armour the NPC wears - which is how the game draws them and how this
    /// client can, without compositing anything.
    std::string getHumanoidBakePath(uint32_t displayId) const;

    bool getHumanoidAppearance(uint32_t displayId, uint8_t& race, uint8_t& sex,
                               uint32_t& appearanceBytes, uint8_t& facialHair) const;

    /// What that humanoid is wearing, as (ItemDisplayInfo id, inventory type)
    /// pairs - the shape applyEquipment reads. CreatureDisplayInfoExtra holds
    /// eleven slots in its own order, and this is that order translated into
    /// the inventory types the rest of the client speaks.
    std::vector<std::pair<uint32_t, uint8_t>>
    getHumanoidEquipment(uint32_t displayId) const;
    std::string getGameObjectModelPathForDisplayId(uint32_t displayId) const;

    /// The hull a transport is drawn with, or "" to use the display lookup.
    ///
    /// A preloaded transport can be spawned before its displayId is known, so
    /// the vehicle has to be recognisable from its GameObject entry too.
    ///
    /// One function because there were two copies of this table, in
    /// entity_spawner_player.cpp and entity_spawner_processing.cpp, and the
    /// queued path in the second is the one most spawns actually take -
    /// so fixing the first left the bug on screen.
    static std::string transportModelPath(uint32_t entry, uint32_t displayId);
    audio::VoiceType detectVoiceTypeFromDisplayId(uint32_t displayId) const;

    // Attempts one deferred attachment and owns retry bookkeeping. Returns true
    // only when this call consumed the caller's per-frame attachment budget.
    bool retryCreatureVirtualWeapons(uint64_t guid, uint32_t instanceId,
                                     uint8_t maxAttempts);

    /// One Item.dbc row, the client's own table of every item's class,
    /// subclass, inventory type, sheathe type and display.
    struct ItemDbcRow {
        uint32_t itemClass = 0;
        uint32_t subclass = 0;
        uint32_t inventoryType = 0;
        uint32_t sheatheType = UINT32_MAX;  ///< UINT32_MAX: no row / unknown
        uint32_t displayId = 0;
        uint32_t material = 0;              ///< 1 metal, 2 wood... (SheatheSoundLookups)
    };
    /// Play the sheathe (or draw) sound for an item at a render position,
    /// from SheatheSoundLookups.dbc (class, subclass and, where the row asks,
    /// material: SheathMetal / SheathWood / SheathShield and the Unsheath ones).
    void playSheathSound(const ItemDbcRow& item, bool drawing, const glm::vec3& renderPos);
    /// The row for an item entry; nullptr when Item.dbc has none (or the
    /// expansion ships no Item.dbc).
    const ItemDbcRow* lookupItemDbc(uint32_t entry);
    /// A unit's main-hand, off-hand and ranged items: other players from
    /// their visible item fields, creatures from UNIT_VIRTUAL_ITEM_SLOT_ID.
    /// Not for the local character, whose inventory is authoritative.
    std::array<ItemDbcRow, 3> unitWeaponItems(uint64_t guid);
    /// The weapon loadout the combat animations are picked from.
    rendering::WeaponLoadout unitWeaponLoadout(uint64_t guid);
    /// UNIT_FIELD_BYTES_2 byte 0: 0 nothing drawn, 1 melee, 2 ranged.
    uint8_t unitSheathState(uint64_t guid) const;
    /// The sheath state the unit's weapons are shown in: the byte above as
    /// the stock client changes it for the animation playing and for an
    /// attack target (rendering::anim::presentedSheath).
    uint8_t presentedSheathState(uint64_t guid, uint32_t instanceId) const;
    /// Re-attach a unit's weapons when its sheath state changed since the
    /// last look. Cheap when nothing changed; called every frame.
    void syncUnitSheath(uint64_t guid, uint32_t instanceId);
    /// The loop another unit idles in: its cast hold while it casts or
    /// channels, else its weapon's Ready stance while alive and in combat
    /// (UNIT_FLAG_IN_COMBAT), Stand otherwise - and Stand for a model with no
    /// ready stance at all. StealthStand while stealthed.
    uint32_t unitIdleAnim(uint64_t guid, uint32_t instanceId);
    /// The cast or channel loop another unit holds from its SMSG_SPELL_START
    /// or MSG_CHANNEL_START until the cast resolves (0 to end it). Everything
    /// that returns a unit to its idle - a flinch, a stop, the Stand/Ready
    /// switch - returns it to this, so the stance never replaces a cast.
    void setUnitCastHold(uint64_t guid, uint32_t animId) {
        if (animId) unitCastHolds_[guid] = animId; else unitCastHolds_.erase(guid);
    }
    uint32_t unitCastHold(uint64_t guid) const {
        auto it = unitCastHolds_.find(guid);
        return it != unitCastHolds_.end() ? it->second : 0u;
    }
    /// The stealth variant of a locomotion sequence (Walk -> StealthWalk,
    /// Run -> StealthRun or StealthWalk) for a unit sent with the CREEP
    /// visibility flag, where its model has one; the sequence itself
    /// otherwise.
    uint32_t unitMoveAnim(uint64_t guid, uint32_t instanceId, uint32_t moveAnim);

    // Mount
    void setMountDisplayId(uint32_t displayId) { pendingMountDisplayId_ = displayId; }
    uint32_t getMountInstanceId() const { return mountInstanceId_; }
    uint32_t getMountModelId() const { return mountModelId_; }

    struct RemotePlayerMount {
        uint32_t displayId = 0;
        uint32_t modelId = 0;
        uint32_t instanceId = 0;
        float riderHeight = 0.0f;
    };
    void setRemotePlayerMountDisplayId(uint64_t guid, uint32_t displayId);
    const std::unordered_map<uint64_t, RemotePlayerMount>& getRemotePlayerMounts() const {
        return remotePlayerMounts_;
    }
    const RemotePlayerMount* getRemotePlayerMount(uint64_t guid) const {
        auto it = remotePlayerMounts_.find(guid);
        return it != remotePlayerMounts_.end() ? &it->second : nullptr;
    }

    /// Draw a player - the local one included - as a creature display, or as
    /// itself again given 0.
    ///
    /// This is what UNIT_FIELD_DISPLAYID differing from
    /// UNIT_FIELD_NATIVEDISPLAYID means: a druid form, ghost wolf, a
    /// polymorph, a costume. The display goes through the creature path
    /// (CreatureDisplayInfo model, skins, scale, a humanoid's composited
    /// look), and the resulting instance takes the player's place in
    /// playerInstances_ - or the renderer's character slot - so movement,
    /// melee, casts and deaths animate the form with its own sequences. The
    /// character's own instance is kept hidden underneath, wearing its
    /// equipment, so turning back is instant.
    void setPlayerMorphDisplayId(uint64_t guid, uint32_t displayId);
    [[nodiscard]] bool isPlayerMorphed(uint64_t guid) const { return playerMorphs_.count(guid) != 0; }

    /// A player's corpse as stock draws it: the bones model for its race and
    /// sex (World\Generic\PassiveDoodads\DeathSkeletons\<Race><Sex>DeathSkeleton,
    /// the path wow.exe formats) once CORPSE_FLAG_BONES is set.
    void spawnCorpseBones(uint64_t guid, uint8_t raceId, uint8_t genderId,
                          float x, float y, float z, float orientation);

    // Local player GUID exclusion (EntitySpawner skips spawning the local player)
    void setLocalPlayerGuid(uint64_t guid) { spawnedPlayerGuid_ = guid; }

    // Weapon model IDs (shared counter for creature weapons + equipment helm/weapon models)
    uint32_t allocateWeaponModelId() { return nextWeaponModelId_++; }

    // Maximum weapon attachment operations per tick (used by Application update loop)
    static constexpr int MAX_WEAPON_ATTACHES_PER_TICK = 2;

    // Dead creature tracking (spawn in corpse/death pose)
    void markCreatureDead(uint64_t guid) { deadCreatureGuids_.insert(guid); }
    void unmarkCreatureDead(uint64_t guid) { deadCreatureGuids_.erase(guid); }
    void clearDeadCreatureGuids() { deadCreatureGuids_.clear(); }

    // Mount state management
    void clearMountState();

    // Transport registration API (used by setupUICallbacks transport spawn callback)
    void queueTransportRegistration(uint64_t guid, uint32_t entry, uint32_t displayId,
                                     float x, float y, float z, float orientation);
    void setTransportPendingMove(uint64_t guid, float x, float y, float z, float orientation);
    bool hasTransportRegistrationPending(uint64_t guid) const;
    void updateTransportRegistration(uint64_t guid, uint32_t displayId,
                                      float x, float y, float z, float orientation);

    // Test transport setup flag
    bool isTestTransportSetup() const { return testTransportSetup_; }
    void setTestTransportSetup(bool v) { testTransportSetup_ = v; }

    // Creature animation state accessors (for movement sync in Application::update)
    std::unordered_map<uint64_t, uint32_t>& getCreatureInstances() { return creatureInstances_; }
    const std::unordered_map<uint64_t, uint32_t>& getCreatureInstances() const { return creatureInstances_; }
    std::unordered_map<uint64_t, uint32_t>& getCreatureModelIds() { return creatureModelIds_; }
    const std::unordered_map<uint64_t, uint32_t>& getCreatureDisplayIds() const { return creatureDisplayIds_; }
    std::unordered_map<uint64_t, glm::vec3>& getCreatureRenderPosCache() { return creatureRenderPosCache_; }
    std::unordered_map<uint64_t, bool>& getCreatureWasMoving() { return creatureWasMoving_; }
    std::unordered_map<uint64_t, bool>& getCreatureWasSwimming() { return creatureWasSwimming_; }
    std::unordered_map<uint64_t, bool>& getCreatureWasFlying() { return creatureWasFlying_; }
    std::unordered_map<uint64_t, bool>& getCreatureWasWalking() { return creatureWasWalking_; }
    std::unordered_map<uint64_t, bool>& getCreatureSwimmingState() { return creatureSwimmingState_; }
    std::unordered_map<uint64_t, bool>& getCreatureWalkingState() { return creatureWalkingState_; }
    std::unordered_map<uint64_t, bool>& getCreatureFlyingState() { return creatureFlyingState_; }
    /// Last movement-direction flags per unit (FORWARD 0x1, BACKWARD 0x2,
    /// STRAFE_LEFT 0x4, STRAFE_RIGHT 0x8, TURN_LEFT 0x10, TURN_RIGHT 0x20,
    /// FALLING 0x1000, FALLINGFAR 0x2000) for the stock body yaw and the
    /// locomotion animation, and that body yaw's state per remote player.
    std::unordered_map<uint64_t, uint32_t>& getUnitMoveDirFlags() { return unitMoveDirFlags_; }
    /// Movement flags that keep a unit off the floor, per unit: LEVITATING
    /// (gravity disabled), CAN_FLY, FLYING and HOVER. A unit with none of them
    /// falls to the floor under its server position in the stock client.
    std::unordered_map<uint64_t, uint32_t>& getUnitGravityFreeFlags() { return unitGravityFreeFlags_; }
    /// Where a standing creature's feet land: the floor found under its server
    /// position, the position it was found for, and when to look again (a WMO
    /// or doodad streaming in after the creature can change the answer).
    struct RestingFloor {
        glm::vec3 serverRenderPos{0.0f};
        float floorZ = 0.0f;
        float recheckIn = 0.0f;
    };
    std::unordered_map<uint64_t, RestingFloor>& getCreatureRestingFloor() { return creatureRestingFloor_; }
    std::unordered_map<uint64_t, rendering::anim::BodyFacingState>& getUnitBodyFacing() { return unitBodyFacing_; }
    /// Per remote player and NPC: the LocomotionFSM driving its movement
    /// animation (animation/unit_locomotion.hpp).
    std::unordered_map<uint64_t, rendering::anim::UnitLocomotion>& getUnitLocomotion() { return unitLocomotion_; }
    /// The same, for the mount models of mounted players (keyed by the rider).
    std::unordered_map<uint64_t, rendering::anim::UnitLocomotion>& getMountLocomotion() { return mountLocomotion_; }
    std::unordered_map<uint64_t, uint32_t>& getCreatureActiveEmotes() { return creatureActiveEmotes_; }
    std::unordered_map<uint32_t, bool>& getModelIdIsWolfLike() { return modelIdIsWolfLike_; }

    // Player instance accessors (for movement sync in Application::update)
    std::unordered_map<uint64_t, uint32_t>& getPlayerInstances() { return playerInstances_; }
    const std::unordered_map<uint64_t, uint32_t>& getPlayerInstances() const { return playerInstances_; }

    // GameObject instance accessors
    auto& getGameObjectInstances() { return gameObjectInstances_; }
    const auto& getGameObjectInstances() const { return gameObjectInstances_; }

    // Display data accessors (needed by Application for gryphon/wyvern display IDs)
    const auto& getDisplayDataMap() const { return displayDataMap_; }

    /// Everything needed to draw a creature somewhere other than the world:
    /// the model it uses and the skins that go on it.
    ///
    /// Both halves already live here - CreatureDisplayInfo.dbc gives the model
    /// id and the skin names, CreatureModelData.dbc turns the model id into a
    /// path - and they were reachable only from inside the spawner. A preview
    /// frame wants the same answer and reading the two files again would put a
    /// second copy of the layout somewhere else, which is how a display id ends
    /// up meaning two different creatures.
    struct CreatureModel {
        std::string m2Path;
        std::string skin1, skin2, skin3;
        uint32_t    modelId = 0;
        /// A model with no skin draws as a white shape, which is worse than an
        /// empty frame - so a caller with nowhere to get textures should treat
        /// this as nothing to draw.
        [[nodiscard]] bool valid() const { return !m2Path.empty() && !skin1.empty(); }
    };
    uint32_t getGryphonDisplayId() const { return gryphonDisplayId_; }
    uint32_t getWyvernDisplayId() const { return wyvernDisplayId_; }

    // Character section lookups (needed for player character skin compositing)
    /// The key for charSectionsCache_, in one place.
    ///
    /// It was written out twice - once where the cache is filled and once where
    /// it is read - and the two had to agree bit for bit or every lookup missed
    /// silently. Two copies of a packing rule is the same hazard as two copies
    /// of any other rule, and cheaper to remove than to remember.
    static constexpr uint64_t charSectionKey(uint8_t race, uint8_t sex, uint8_t section,
                                             uint8_t variation, uint8_t color, int texIndex) {
        return (static_cast<uint64_t>(race) << 26) |
               (static_cast<uint64_t>(sex & 0xF) << 22) |
               (static_cast<uint64_t>(section & 0xF) << 18) |
               (static_cast<uint64_t>(variation & 0xFF) << 10) |
               (static_cast<uint64_t>(color & 0xFF) << 2) |
               static_cast<uint64_t>(texIndex);
    }

    std::string lookupCharSection(uint8_t race, uint8_t sex, uint8_t section,
                                  uint8_t variation, uint8_t color, int texIndex = 0) const;
    const std::unordered_map<uint32_t, uint16_t>& getHairGeosetMap() const { return hairGeosetMap_; }

    struct FacialHairGeosets { uint16_t geoset100 = 0; uint16_t geoset300 = 0; uint16_t geoset200 = 0;
                               uint16_t geoset1700 = 0; };
    const std::unordered_map<uint32_t, FacialHairGeosets>& getFacialHairGeosetMap() const { return facialHairGeosetMap_; }

    // Creature M2 sync loader (used by spawnPlayerCharacter in Application)

private:
    // Dependencies (non-owning)
    rendering::Renderer* renderer_;
    pipeline::AssetManager* assetManager_;
    game::GameHandler* gameHandler_;
    game::GameServices* gameServices_;

    // --- Creature display data (from DBC files) ---
    struct CreatureDisplayData {
        uint32_t modelId = 0;
        std::string skin1, skin2, skin3;  // Texture names from CreatureDisplayInfo.dbc
        uint32_t extraDisplayId = 0;      // Link to CreatureDisplayInfoExtra.dbc
        /// CreatureDisplayInfo.CreatureModelScale, which is how one model is
        /// reused at different sizes: the boar model is 0.6 for a piglet and
        /// 1.5 for a giant, over ten displays. Nothing read it, so every
        /// creature sharing a model drew at that model's own size.
        float displayScale = 1.0f;
        /// CreatureDisplayInfo.CreatureModelAlpha / 255: the display's base
        /// opacity (stock 0x715B50). 1,264 displays are below 1 - ghosts,
        /// spirits, wisps.
        float modelAlpha = 1.0f;
        /// CreatureDisplayInfo.CreatureGeosetData: geoset variant nibbles for
        /// groups 1-8 (stock 0x4E7790).
        uint32_t creatureGeosetData = 0;
        /// CreatureDisplayInfo.ParticleColorID (column 13): the
        /// ParticleColor.dbc row for the model's emitters on colour index
        /// 11-13 (stock 0x4F20C0 -> 0x4EA9E0). 560 displays: etherials,
        /// elementals, wisps, imps.
        uint32_t particleColorId = 0;
    };

    /// CreatureModelData.ModelScale for a model id, the second half of the
    /// same product. Kept beside the display data rather than in it because it
    /// belongs to the model and several displays share one.
    /// CreatureModelData.ModelScale for the model a display names.
    ///
    /// Keyed by the display, not by a model id: the renderer's model handles
    /// are a counter of its own and share no numbering with the DBC, so
    /// asking this with one silently answered 1.0 for every creature there
    /// has ever been. 306 displays want 1.25 and two want 2.0.
    float creatureModelScale(uint32_t displayId) const;

    /// CreatureDisplayInfo.CreatureModelScale for a display id, or 1.0 where
    /// the display is unknown - an unknown display is already drawn at the
    /// model's own size, and guessing smaller would hide it.
    float creatureDisplayScale(uint32_t displayId) const;
    /// CreatureDisplayInfo.ParticleColorID for a display, 0 if none or unknown.
    uint32_t creatureDisplayParticleColor(uint32_t displayId) const;

    /// Apply the textures a creature display names: its own skin variations,
    /// and for a humanoid the composited body, face, hair and equipment. Once
    /// per display, since the model is shared by every creature using it.
    /// Start a creature in the pose the server already has it in - dead,
    /// mid-emote, or newly arrived, and fade it in.
    void playCreatureSpawnPose(uint64_t guid, uint32_t instanceId);

    /// Choose one mesh per clothing group for a character-style NPC, leaving
    /// every other batch of the model untouched. An NPC has no inventory to
    /// build a geoset set from, so its equipment comes from
    /// CreatureDisplayInfoExtra.
    void normalizeHumanoidClothingGeosets(uint32_t instanceId, uint32_t modelId,
                                          uint32_t displayId);

    /// Per-instance colouring of a humanoid NPC: hair, skin, and the
    /// head-detail sheet. Per instance rather than per model, so two NPCs
    /// sharing a model still get their own hair colour.
    void applyHumanoidInstanceOverrides(uint32_t instanceId, uint32_t modelId,
                                        uint32_t displayId);

    void applyCreatureDisplayTextures(uint32_t displayId, uint32_t modelId,
                                      const CreatureDisplayData& dispData);

    /// Bind the creature skin slots (M2 types 11-13) of an uploaded model the
    /// way stock does (core/creature_skin_rules.hpp): the display's own
    /// TextureVariation from the model's directory, the default white for a
    /// file that is not there, and nothing - black - for an empty one. The one
    /// place a creature, a mount or another player's mount gets its skins.
    /// Returns how many slots were bound.
    int bindCreatureSkins(uint32_t modelId, uint32_t displayId, const std::string& m2Path,
                          const CreatureDisplayData& dispData);

    struct HumanoidDisplayExtra {
        uint8_t raceId = 0;
        uint8_t sexId = 0;
        uint8_t skinId = 0;
        uint8_t faceId = 0;
        uint8_t hairStyleId = 0;
        uint8_t hairColorId = 0;
        uint8_t facialHairId = 0;
        std::string bakeName;  // Pre-baked texture path if available
        // Equipment display IDs (from columns 8-18)
        // 0=helm, 1=shoulder, 2=shirt, 3=chest, 4=belt, 5=legs, 6=feet, 7=wrist, 8=hands, 9=tabard, 10=cape
        uint32_t equipDisplayId[11] = {0};
    };
    std::unordered_map<uint32_t, CreatureDisplayData> displayDataMap_;  // displayId → display data
    std::unordered_map<uint32_t, HumanoidDisplayExtra> humanoidExtraMap_;  // extraDisplayId → humanoid data
    std::unordered_map<uint32_t, std::string> modelIdToPath_;   // modelId → M2 path (from CreatureModelData.dbc)
    std::unordered_map<uint32_t, float> modelIdToScale_;        // modelId → CreatureModelData.ModelScale
    // CharHairGeosets.dbc: key = (raceId<<16)|(sexId<<8)|variationId → geosetId (skinSectionId)
    std::unordered_map<uint32_t, uint16_t> hairGeosetMap_;
    // CharFacialHairStyles.dbc: key = (raceId<<16)|(sexId<<8)|variationId → {geoset100, geoset300, geoset200}
    std::unordered_map<uint32_t, FacialHairGeosets> facialHairGeosetMap_;
    bool creatureLookupsBuilt_ = false;
    bool tryAttachCreatureVirtualWeapons(uint64_t guid, uint32_t instanceId);

    // Item.dbc, loaded on first use.
    std::unordered_map<uint32_t, ItemDbcRow> itemDbcCache_;
    bool itemDbcLoaded_ = false;
    // SheatheSoundLookups.dbc rows: class, subclass, material, checkMaterial,
    // sheathe sound, draw sound. Loaded on first use.
    struct SheathSoundRow { uint32_t itemClass, subclass, material, checkMaterial, sheathe, unsheathe; };
    std::vector<SheathSoundRow> sheathSoundRows_;
    bool sheathSoundsLoaded_ = false;
    /// A unit's sheath animation in progress: weapons move to where the new
    /// sheath state puts them at swapAtMs (the model's $SHL/$SHR hand-off),
    /// with that moment's sound; nothing more happens until endAtMs.
    struct SheathTransition {
        double swapAtMs = 0.0;
        double endAtMs = 0.0;
        bool swapped = false;
        uint8_t fromState = 0;
    };
    std::unordered_map<uint64_t, SheathTransition> unitSheathTransitions_;
    /// Other players' weapon-slot displays (main, off, ranged) and inventory
    /// types as last sent, so a sheath change can re-attach without them.
    struct UnitWeaponDisplays {
        std::array<uint32_t, 3> displayIds{};
        std::array<uint8_t, 3> inventoryTypes{};
    };
    std::unordered_map<uint64_t, UnitWeaponDisplays> onlinePlayerWeaponSlots_;
    /// Creatures whose weapons came from UNIT_VIRTUAL_ITEM_SLOT_ID and follow
    /// their sheath state.
    std::unordered_set<uint64_t> creatureVirtualItemUnits_;
    /// The sheath state each unit's weapons were last attached for.
    std::unordered_map<uint64_t, uint8_t> unitSheathAttached_;
    /// Each casting unit's cast hold (see setUnitCastHold).
    std::unordered_map<uint64_t, uint32_t> unitCastHolds_;
    /// Attach a unit's three weapon-slot items where its sheath state puts
    /// them: in the hands when drawn, on the back or hips by their sheathe
    /// type when not, the ranged weapon in hand when it is the one drawn
    /// and on the back otherwise. Returns whether anything was attached.
    bool attachUnitWeapons(uint64_t guid, uint32_t instanceId,
                           const std::array<uint32_t, 3>& displayIds,
                           const std::array<ItemDbcRow, 3>& items);

    // CharSections.dbc lookup cache
    std::unordered_map<uint64_t, std::string> charSectionsCache_;
    bool charSectionsCacheBuilt_ = false;
    void buildCharSectionsCache();

    // Creature display lookup building
    void buildCreatureDisplayLookups();

    // Weapon M2 loading helper
    bool loadWeaponM2(const std::string& m2Path, pipeline::M2Model& outModel);

    // GameObject display lookups
    std::unordered_map<uint32_t, std::string> gameObjectDisplayIdToPath_;
    bool gameObjectLookupsBuilt_ = false;
    void buildGameObjectDisplayLookups();

    // --- Creature instances and tracking ---
    // Drop the pending-spawn marker for guid unless a queue entry still references it.
    void erasePendingGuidIfUnqueued(uint64_t guid);

    std::unordered_map<uint64_t, uint32_t> creatureInstances_;  // guid → render instanceId
    std::unordered_map<uint64_t, uint32_t> creatureModelIds_;   // guid → loaded modelId
    std::unordered_map<uint64_t, uint32_t> creatureDisplayIds_; // guid → active displayId
    // Latest server-requested display, including creatures whose async model load
    // has not completed yet. This prevents an older queued display from winning.
    std::unordered_map<uint64_t, uint32_t> requestedCreatureDisplayIds_;
    std::unordered_map<uint64_t, glm::vec3> creatureRenderPosCache_; // guid → last synced render position
    std::unordered_map<uint64_t, bool> creatureWasMoving_;
    std::unordered_map<uint64_t, bool> creatureWasSwimming_;
    std::unordered_map<uint64_t, bool> creatureWasFlying_;
    std::unordered_map<uint64_t, bool> creatureWasWalking_;
    std::unordered_map<uint64_t, bool> creatureSwimmingState_;
    std::unordered_map<uint64_t, bool> creatureWalkingState_;
    std::unordered_map<uint64_t, bool> creatureFlyingState_;
    std::unordered_map<uint64_t, uint32_t> unitMoveDirFlags_;
    std::unordered_map<uint64_t, uint32_t> unitGravityFreeFlags_;
    std::unordered_map<uint64_t, RestingFloor> creatureRestingFloor_;
    std::unordered_map<uint64_t, rendering::anim::BodyFacingState> unitBodyFacing_;
    std::unordered_map<uint64_t, rendering::anim::UnitLocomotion> unitLocomotion_;
    std::unordered_map<uint64_t, rendering::anim::UnitLocomotion> mountLocomotion_;
    // Server waypoint scripts hold work/chop emotes for many seconds. Locomotion
    // temporarily overrides them, then the idle transition restores this loop.
    std::unordered_map<uint64_t, uint32_t> creatureActiveEmotes_;
    /// The translucency last given to each unit's drawn instance, by guid:
    /// which instance, and the alpha. Keyed with the instance so a morph
    /// swap (stealth in cat form) is re-applied to the new model.
    struct PresentedAlpha { uint32_t instanceId = 0; float alpha = 1.0f; };
    std::unordered_map<uint64_t, PresentedAlpha> unitPresentedAlpha_;
    uint32_t stealthSyncFrameCounter_ = 0;  // throttles syncUnitPresentation()
    std::unordered_set<uint64_t> creatureWeaponsAttached_;
    std::unordered_map<uint64_t, uint8_t> creatureWeaponAttachAttempts_;
    std::unordered_map<uint32_t, bool> modelIdIsWolfLike_;

    /// Ghosts and stealthed units drawn see-through, every unit alike: a
    /// player ghost (PLAYER_FLAGS_GHOST), a unit sent with the CREEP
    /// visibility flag, or one carrying a stealth or invisibility aura
    /// (SPELL_AURA_MOD_STEALTH 16, SPELL_AURA_MOD_INVISIBILITY 18).
    void syncUnitPresentation();
    float unitPresentationAlpha(uint64_t guid, const game::Unit& unit, bool isLocal);
    void presentUnitAlpha(uint64_t guid, uint32_t instanceId, float alpha);
    /// spellId -> whether one of its effects applies stealth or invisibility.
    std::unordered_map<uint32_t, bool> stealthAuraSpells_;
    bool isStealthAuraSpell(uint32_t spellId);
    /// The transparency an aura's spell visual asks for: its StateKit's
    /// SpellVisualKit CharProc 14 parameter (stock: Stealth 0.3, Invisibility
    /// 0.5 or 0.35, Ghost 0.5). Negative when it asks for none.
    float auraKitTransparency(uint32_t spellId);
    std::unordered_map<uint32_t, float> auraKitTransparency_;
public:
    /// CreatureDisplayInfo.CreatureModelAlpha / 255 for a display (1 if unknown).
    float creatureDisplayAlpha(uint32_t displayId) const;
    /// The size a playable race draws at: its ChrRaces display's
    /// CreatureModelScale x CreatureModelData.ModelScale (stock 0x71C110) -
    /// tauren males 1.35, tauren females 1.25, gnomes 1.15. Players were all
    /// drawn at 1.0.
    float playerRaceDisplayScale(uint8_t raceId, uint8_t genderId);
private:

    // --- Creature async loads ---
    struct PreparedCreatureModel {
        uint64_t guid;
        uint32_t displayId;
        uint32_t modelId;
        float x, y, z, orientation;
        float scale = 1.0f;
        std::shared_ptr<pipeline::M2Model> model;
        std::unordered_map<std::string, pipeline::BLPImage> predecodedTextures;
        bool valid = false;
        bool permanent_failure = false;
    };
    struct AsyncCreatureLoad {
        std::future<PreparedCreatureModel> future;
    };
    std::vector<AsyncCreatureLoad> asyncCreatureLoads_;
    std::unordered_set<uint32_t> asyncCreatureDisplayLoads_;
    void processAsyncCreatureResults(bool unlimited = false);
    static constexpr int MAX_ASYNC_CREATURE_LOADS = 4;
    std::unordered_set<uint64_t> deadCreatureGuids_;

    // --- NPC equipment attachment models (helm, shoulders) ---
    // Equipment is shared across NPCs, so a crowd wearing the same gear would
    // otherwise re-read, re-parse and re-upload the same M2 once per spawn. Cache
    // it by resolved path. A modelId of 0 negative-caches a path that has no model,
    // so a missing file is not retried against the disk on every spawn.
    struct CachedAttachmentModel {
        uint32_t modelId = 0;                      // 0 = no usable model
        std::shared_ptr<pipeline::M2Model> model;  // shared_ptr: M2Model is incomplete here
    };
    // Parsed geometry, keyed by path. A null value negative-caches a missing file so
    // it is not probed against the disk on every spawn.
    std::unordered_map<std::string, std::shared_ptr<pipeline::M2Model>> attachmentModelData_;
    // Renderer model id, keyed by "path|texture". attachWeapon() binds the texture to
    // the model id, so two recolours of one mesh must not share an id or the last
    // NPC spawned would retexture every other NPC wearing that mesh.
    std::unordered_map<std::string, uint32_t> attachmentModelIds_;
    /// Resolve the first candidate path with a usable model, reading and parsing it
    /// at most once. Returns modelId == 0 when no candidate yields a valid model.
    CachedAttachmentModel getOrLoadAttachmentModel(
        const std::vector<std::string>& candidatePaths, const std::string& texturePath);

    std::unordered_map<uint32_t, uint32_t> displayIdModelCache_;
    std::unordered_set<uint32_t> displayIdTexturesApplied_;
    std::unordered_map<uint32_t, std::unordered_map<std::string, pipeline::BLPImage>> displayIdPredecodedTextures_;
    mutable std::unordered_set<uint32_t> warnedMissingDisplayDataIds_;
    mutable std::unordered_set<uint32_t> warnedMissingModelPathIds_;
    /// Says which model the first few humanoid displays resolved to, so an
    /// asset overlay that re-points them can be told apart from one that never
    /// reached the client. Five is enough to see and few enough to ignore.
    mutable int humanoidDisplayCanaryCount_ = 0;
    /// Says what the first few NPCs resolved for their head-detail texture.
    int npcHeadDetailCanaryCount_ = 0;
    uint32_t nextCreatureModelId_ = 5000;
    uint32_t gryphonDisplayId_ = 0;
    uint32_t wyvernDisplayId_ = 0;

    // --- Creature spawn queue ---
    struct PendingCreatureSpawn {
        uint64_t guid;
        uint32_t displayId;
        float x, y, z, orientation;
        float scale = 1.0f;
    };
    std::deque<PendingCreatureSpawn> pendingCreatureSpawns_;
    static constexpr int MAX_SPAWNS_PER_FRAME = 3;
    static constexpr auto CREATURE_SPAWN_RETRY_WINDOW = std::chrono::seconds(5);
    std::unordered_set<uint64_t> pendingCreatureSpawnGuids_;
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point>
        creatureSpawnRetryDeadlines_;
    /// How many retry windows a creature has already used up.
    ///
    /// Running out used to abandon the spawn for good, and nothing ever asked
    /// again: the server does not re-send an object that is already in range,
    /// so the creature stayed missing until something made it re-send - which
    /// is what walking out of the zone and back does, and why they "appear
    /// when I zone again". A window that expires now buys another one, up to
    /// this many, so a spawn that keeps failing is delayed rather than lost.
    std::unordered_map<uint64_t, int> creatureSpawnRetryWindowsUsed_;
    static constexpr int MAX_CREATURE_SPAWN_RETRY_WINDOWS = 6;
    std::unordered_set<uint32_t> nonRenderableCreatureDisplayIds_;
    std::unordered_set<uint64_t> creaturePermanentFailureGuids_;
    void processCreatureSpawnQueue(bool unlimited = false);

    // --- NPC composite loads ---
    struct DeferredNpcComposite {
        uint32_t modelId;
        uint32_t displayId;
        std::string basePath;
        std::vector<std::string> overlayPaths;
        std::vector<std::pair<int, std::string>> regionLayers;
        std::vector<uint32_t> skinTextureSlots;
        bool hasComposite = false;
        bool hasSimpleSkin = false;
        std::string bakedSkinPath;
        bool hasBakedSkin = false;
        std::vector<uint32_t> hairTextureSlots;
        std::string hairTexturePath;
        bool useBakedForHair = false;
    };
    struct PreparedNpcComposite {
        DeferredNpcComposite info;
        std::unordered_map<std::string, pipeline::BLPImage> predecodedTextures;
    };
    struct AsyncNpcCompositeLoad {
        std::future<PreparedNpcComposite> future;
    };
    std::vector<AsyncNpcCompositeLoad> asyncNpcCompositeLoads_;
    void processAsyncNpcCompositeResults(bool unlimited = false);

    // --- Player instances ---
    std::unordered_map<uint64_t, uint32_t> playerInstances_;  // guid → render instanceId
    struct OnlinePlayerAppearanceState {
        uint32_t instanceId = 0;
        uint32_t modelId = 0;
        uint8_t raceId = 0;
        uint8_t genderId = 0;
        uint32_t appearanceBytes = 0;
        uint8_t facialFeatures = 0;
        std::string bodySkinPath;
        std::vector<std::string> underwearPaths;
    };
    std::unordered_map<uint64_t, OnlinePlayerAppearanceState> onlinePlayerAppearance_;
    std::unordered_map<uint64_t, std::pair<std::array<uint32_t, 19>, std::array<uint8_t, 19>>> pendingOnlinePlayerEquipment_;
    std::deque<std::pair<uint64_t, std::pair<std::array<uint32_t, 19>, std::array<uint8_t, 19>>>> deferredEquipmentQueue_;
    void processDeferredEquipmentQueue();
    struct PreparedEquipmentUpdate {
        uint64_t guid;
        std::array<uint32_t, 19> displayInfoIds;
        std::array<uint8_t, 19> inventoryTypes;
        std::unordered_map<std::string, pipeline::BLPImage> predecodedTextures;
    };
    struct AsyncEquipmentLoad {
        std::future<PreparedEquipmentUpdate> future;
    };
    std::vector<AsyncEquipmentLoad> asyncEquipmentLoads_;
    void processAsyncEquipmentResults();
    std::vector<std::string> resolveEquipmentTexturePaths(uint64_t guid,
        const std::array<uint32_t, 19>& displayInfoIds,
        const std::array<uint8_t, 19>& inventoryTypes) const;
    std::unordered_map<uint32_t, uint32_t> playerModelCache_;
    /// Which runtime texture slot of a player model takes which art. Type 8 is
    /// Skin Extra - the head detail sheet, not the underwear, though on the
    /// models the game shipped the underwear art is what ends up in it.
    struct PlayerTextureSlots { int skin = -1; int hair = -1; int skinExtra = -1; };
    std::unordered_map<uint32_t, PlayerTextureSlots> playerTextureSlotsByModelId_;
    uint32_t nextPlayerModelId_ = 60000;

    // --- Player spawn queue ---
    struct PendingPlayerSpawn {
        uint64_t guid;
        uint8_t raceId;
        uint8_t genderId;
        uint32_t appearanceBytes;
        uint8_t facialFeatures;
        float x, y, z, orientation;
    };
    std::deque<PendingPlayerSpawn> pendingPlayerSpawns_;
    std::unordered_set<uint64_t> pendingPlayerSpawnGuids_;
    void processPlayerSpawnQueue();

    // --- GameObject instances ---
    struct GameObjectInstanceInfo {
        uint32_t modelId = 0;
        uint32_t instanceId = 0;
        bool isWmo = false;
        /// Last GOState applied, 0xFF none yet. GAMEOBJECT_BYTES_1 also carries
        /// the art kit and anim progress, so the same state arrives again with
        /// every change to those, and must not replay the Open swing.
        uint8_t goState = 0xFF;
    };
    // Game objects spawned before their type was known, keyed by entry. Each is
    // frozen on the conservative assumption that it is state-driven until the
    // query response says otherwise.
    std::unordered_map<uint32_t, std::vector<uint32_t>> gameObjectPendingAnimPolicy_;

    std::unordered_map<uint32_t, uint32_t> gameObjectDisplayIdModelCache_;
    std::unordered_set<uint32_t> gameObjectDisplayIdFailedCache_;
    std::unordered_map<uint32_t, uint32_t> gameObjectDisplayIdWmoCache_;
    std::unordered_map<uint64_t, GameObjectInstanceInfo> gameObjectInstances_;
    /// States that arrived before their model did, applied when it spawns.
    std::unordered_map<uint64_t, uint8_t> gameObjectPendingState_;
    struct PendingTransportMove {
        float x = 0.0f, y = 0.0f, z = 0.0f, orientation = 0.0f;
    };
    struct PendingTransportRegistration {
        uint64_t guid = 0;
        uint32_t entry = 0;
        uint32_t displayId = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f, orientation = 0.0f;
        int retryFrames = 0;  // waiting for the GO's render instance to spawn
    };
    std::unordered_map<uint64_t, PendingTransportMove> pendingTransportMoves_;
    std::deque<PendingTransportRegistration> pendingTransportRegistrations_;
    uint32_t nextGameObjectModelId_ = 20000;
    uint32_t nextGameObjectWmoModelId_ = 40000;
    bool testTransportSetup_ = false;

    // --- GameObject spawn queue ---
    struct PendingGameObjectSpawn {
        uint64_t guid;
        uint32_t entry;
        uint32_t displayId;
        float x, y, z, orientation;
        float scale = 1.0f;
    };
    std::deque<PendingGameObjectSpawn> pendingGameObjectSpawns_;
    void processGameObjectSpawnQueue();

    // --- Async WMO loading for game objects ---
    struct PreparedGameObjectWMO {
        uint64_t guid;
        uint32_t entry;
        uint32_t displayId;
        float x, y, z, orientation;
        float scale = 1.0f;
        std::shared_ptr<pipeline::WMOModel> wmoModel;
        std::unordered_map<std::string, pipeline::BLPImage> predecodedTextures;
        bool valid = false;
        bool isWmo = false;
        std::string modelPath;
    };
    struct AsyncGameObjectLoad {
        std::future<PreparedGameObjectWMO> future;
    };

    // A parsed WMO whose GPU upload is being spread across frames. Uploading a
    // transport in one go cost ~40ms - its textures are large, and that upload
    // has to happen on the main thread even though a worker decoded them. The
    // result is held here because it owns the model and the decoded pixels.
    struct PendingWmoUpload {
        PreparedGameObjectWMO result;
        uint32_t modelId = 0;
    };
    std::vector<PendingWmoUpload> pendingWmoUploads_;
    void processPendingWmoUploads();
    void finishWmoSpawn(const PreparedGameObjectWMO& result, uint32_t modelId);
    std::vector<AsyncGameObjectLoad> asyncGameObjectLoads_;
    void processAsyncGameObjectResults();
    struct PendingTransportDoodadBatch {
        uint64_t guid = 0;
        uint32_t modelId = 0;
        uint32_t instanceId = 0;
        size_t nextIndex = 0;
        size_t doodadBudget = 0;
        size_t spawnedDoodads = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f, orientation = 0.0f;
    };
    std::vector<PendingTransportDoodadBatch> pendingTransportDoodadBatches_;
    static constexpr size_t MAX_TRANSPORT_DOODADS_PER_FRAME = 4;
    void processPendingTransportRegistrations();
    void processPendingTransportDoodads();

    // --- Mount ---
    uint32_t mountInstanceId_ = 0;
    uint32_t mountModelId_ = 0;
    uint32_t pendingMountDisplayId_ = 0;
    void processPendingMount();
    std::unordered_map<uint64_t, RemotePlayerMount> remotePlayerMounts_;
    std::unordered_map<uint64_t, uint32_t> pendingRemotePlayerMounts_;
    void processPendingRemotePlayerMounts();
    bool loadRemoteMountModel(uint32_t displayId, uint32_t& modelId,
                              std::string& modelPath, float& riderHeight);
    void removeRemotePlayerMount(uint64_t guid);

    // --- Player morphs (shapeshift, polymorph, costume) ---
    struct PlayerMorph {
        uint32_t displayId = 0;
        uint32_t instanceId = 0;        // the creature display drawn in the player's place
        uint32_t nativeInstanceId = 0;  // the character's own instance, hidden meanwhile
    };
    std::unordered_map<uint64_t, PlayerMorph> playerMorphs_;
    /// Wanted display per player (0 = turn back), until it is drawn.
    std::unordered_map<uint64_t, uint32_t> pendingPlayerMorphs_;
    /// The morph display is built by the ordinary creature spawn path under
    /// this key - the player's guid with the top bit flipped, which no
    /// player, creature, pet or object guid this client meets carries - and
    /// taken out of the creature maps once it exists.
    static constexpr uint64_t morphSpawnKey(uint64_t guid) { return guid ^ 0x8000000000000000ull; }
    void processPendingPlayerMorphs();
    void applyPlayerMorph(uint64_t guid, uint32_t displayId, uint32_t morphInstanceId,
                          uint32_t nativeInstanceId, bool isLocal);
    void clearPlayerMorph(uint64_t guid);
    /// Forget a creature's bookkeeping and hand back its render instance
    /// without removing it (0 if there is none). despawnCreature is this
    /// plus removing the instance.
    uint32_t releaseCreatureInstance(uint64_t guid);
    bool isLocalPlayer(uint64_t guid) const;

    // --- Corpse bones (other players' and the local player's) ---
    std::unordered_map<uint64_t, uint32_t> corpseBonesInstances_;  // corpse guid -> instance
    std::unordered_map<std::string, uint32_t> corpseBonesModels_;   // path -> model id (0 = missing)

    // --- Local player GUID exclusion ---
    uint64_t spawnedPlayerGuid_ = 0;

    // --- Weapon model ID counter ---
    // Weapon model ids share CharacterRenderer's models map with NPC
    // composites, which are keyed by creature displayId (1..~35000). Starting
    // at 1000 collided with loaded NPC models once enough weapon reloads
    // advanced the counter - attachWeapon then instanced an NPC mesh as the
    // weapon. Reserve a range no displayId can reach.
    uint32_t nextWeaponModelId_ = 0x40000000u;

    // --- Spawn internal methods ---
    void spawnOnlineCreature(uint64_t guid, uint32_t displayId,
                             float x, float y, float z, float orientation, float scale = 1.0f);
    void spawnOnlinePlayer(uint64_t guid, uint8_t raceId, uint8_t genderId,
                           uint32_t appearanceBytes, uint8_t facialFeatures,
                           float x, float y, float z, float orientation);
    void setOnlinePlayerEquipment(uint64_t guid,
                                  const std::array<uint32_t, 19>& displayInfoIds,
                                  const std::array<uint8_t, 19>& inventoryTypes);
    void spawnOnlineGameObject(uint64_t guid, uint32_t entry, uint32_t displayId,
                               float x, float y, float z, float orientation, float scale = 1.0f);
};

} // namespace core
} // namespace wowee
