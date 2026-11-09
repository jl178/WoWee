#pragma once

#include "core/geoset_rules.hpp"
#include "pipeline/hd_characters.hpp"
#include "game/character.hpp"
#include "game/inventory.hpp"
#include "rendering/animation/weapon_type.hpp"
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <memory>
#include <unordered_set>
#include <cstdint>

namespace wowee {

namespace rendering { class Renderer; }
namespace pipeline { class AssetManager; class DBCLayout; struct M2Model; }
namespace game { class GameHandler; class Inventory; }

namespace core {

class EntitySpawner;

/// The M2 attachment a weapon-slot item goes on: held (hand, or the shield
/// mount for a shield) or put away (back, hip or shield sheath).
/// UINT32_MAX when the item has nowhere to go (hidden). Shared by the local
/// character, other players and creatures.
uint32_t weaponAttachment(bool sheathed, game::EquipSlot slot, uint8_t inventoryType);
/// The model's sheath point for a put-away item, from its Item.dbc
/// SheatheType (UINT32_MAX when there is no row: by inventory type):
/// two-handers 26/27, staves and polearms 30/31, shields 28, one-handers
/// 32/33. These points carry the sheathed orientation themselves, so the
/// item goes on with an identity transform. UINT32_MAX = not shown.
uint32_t sheathedAttachment(uint32_t sheatheType, game::EquipSlot slot, uint8_t inventoryType);
/// Where a put-away bow, gun or crossbow goes.
uint32_t rangedSheathAttachment();
/// A back sheath point some models (creatures) lack: callers retry on the
/// plain Back point (12) with weaponLocalTransform's back layout.
bool isLegacyBackFallbackPoint(uint32_t attachmentId);
/// The offset that lays a weapon along the plain Back point (12), for a
/// model without the sheath points only.
glm::mat4 weaponLocalTransform(bool sheathed, game::EquipSlot slot, uint8_t inventoryType);
/// Where a put-away item sits, as the sheath reach needs it (hip or back).
rendering::SheathSpot sheathSpotFor(uint32_t sheathedAttachmentId);

/// Resolved texture paths from CharSections.dbc for player character compositing.
struct PlayerTextureInfo {
    std::string bodySkinPath;
    std::string faceLowerPath;
    std::string faceUpperPath;
    /// CharSections section 2: the facial hair painted onto the face regions.
    std::string facialLowerPath;
    std::string facialUpperPath;
    std::string hairTexturePath;
    /// CharSections' second texture on the skin row - the "Extra" art an HD
    /// character model asks for as texture type 8. Empty on the stock models,
    /// which have no type-8 texture and never look for one.
    std::string skinExtraPath;
    std::vector<std::string> underwearPaths;
};

/// Handles player character visual appearance: skin compositing, geoset selection,
/// texture path lookups, and equipment weapon rendering.
class AppearanceComposer {
public:
    AppearanceComposer(rendering::Renderer* renderer,
                       pipeline::AssetManager* assetManager,
                       game::GameHandler* gameHandler,
                       EntitySpawner* entitySpawner);

    // Player model path resolution

    // Resolve texture paths from CharSections.dbc and fill model texture slots.
    // Call BEFORE charRenderer->loadModel().
    /// `useFemaleModel` is the body a nonbinary character chose. The skin
    /// textures are picked per sex, so a female body reading male skins is
    /// the same mismatch the model path had.
    PlayerTextureInfo resolvePlayerTextures(pipeline::M2Model& model,
                                            game::Race race, game::Gender gender,
                                            uint32_t appearanceBytes,
                                            bool useFemaleModel = false,
                                            uint8_t facialHairId = 0);

    // Apply composited textures to loaded model instance.
    // Call AFTER charRenderer->loadModel(). Saves skin state for re-compositing.
    void compositePlayerSkin(uint32_t modelSlotId, const PlayerTextureInfo& texInfo);

    // Build default active geosets for player character
    /// On an HD player model (as last given to resolvePlayerTextures) the
    /// hair, facial hair and extra groups are the model's own
    /// (pipeline/hd_characters.hpp).
    std::unordered_set<uint16_t> buildDefaultPlayerGeosets(uint8_t raceId, uint8_t sexId,
                                                           uint8_t hairStyleId, uint8_t facialId);


    /// The HD player model's face shape (its .bone adjustments), from the last
    /// resolvePlayerTextures; empty for a 3.3.5 model.
    const std::vector<std::pair<uint16_t, glm::mat4>>& playerHdBones() const { return playerHdBones_; }

    // Equipment weapon loading (reads inventory, attaches weapon M2 models)
    void loadEquippedWeapons();

    // Weapon sheathe state
    /// Also sends CMSG_SET_SHEATHED when the state changes, so other clients
    /// see the character draw and put away its weapons.
    /// Immediate, with no reach (consumables, swimming, transports); it also
    /// cancels a reach in progress and anything queued behind it.
    /// `cause` names the trigger for the (rate-limited) sheath log.
    void setWeaponsSheathed(bool sheathed, const char* cause = "direct");
    [[nodiscard]] bool isWeaponsSheathed() const { return weaponsSheathed_; }
    /// Draw or put away the way Z does in the stock client: the Sheath /
    /// HipSheath reach on the upper body, the weapons changing hands at the
    /// model's $SHL/$SHR hand-off with the item's sheathe or draw sound.
    /// A request during a reach waits for it to finish; only the last one
    /// counts, so spamming the key never flips the weapons twice in a reach.
    void requestWeaponsSheathed(bool sheathed, const char* cause = "request");
    /// Z: toggles the state the weapons are heading for.
    void toggleWeaponsSheathed() { requestWeaponsSheathed(!targetWeaponsSheathed(), "toggle (Z)"); }
    /// The state the weapons will end in: queued, in flight, or current.
    [[nodiscard]] bool targetWeaponsSheathed() const {
        return sheathQueued_ >= 0 ? sheathQueued_ == 1
             : sheathReachActive_ ? sheathReachTarget_ : weaponsSheathed_;
    }
    /// Advance a reach in progress (the hand-off, the end, the queued
    /// request). Call every frame.
    void updateSheathReach();
    /// Where the item in a hand is put away; NONE when the hand is empty.
    [[nodiscard]] rendering::SheathSpot sheathSpot(game::EquipSlot slot) const;

    // Ranged weapon swap: temporarily show ranged weapon in right hand
    void showRangedWeapon(bool show);
    [[nodiscard]] bool isShowingRanged() const { return showingRanged_; }

    // Mining casts temporarily replace the held main-hand model with a pickaxe.
    void showMiningPick(bool show);

    // Fishing casts temporarily replace the held main-hand model with a pole
    // found in the player's equipped slots or bags.
    void showFishingPole(bool show);

    // Saved skin state accessors (used by game_screen.cpp for equipment re-compositing)
    [[nodiscard]] const std::string& getBodySkinPath() const { return bodySkinPath_; }
    [[nodiscard]] const std::vector<std::string>& getUnderwearPaths() const { return underwearPaths_; }
    [[nodiscard]] uint32_t getSkinTextureSlotIndex() const { return skinTextureSlotIndex_; }
    [[nodiscard]] uint32_t getCloakTextureSlotIndex() const { return cloakTextureSlotIndex_; }

private:
    bool loadWeaponM2(const std::string& m2Path, pipeline::M2Model& outModel);

    /// Attach the equipped head item's model. Other players resolve this through
    /// EntitySpawner; the local character had no equivalent at all.
    void loadEquippedHelm(game::Inventory& inventory);

    // Attach the enchant visual (sharpening-stone glint, weapon glow) of the item in
    // the given equipment slot to the weapon already attached at attachmentId.
    void applyEnchantVisuals(uint32_t charInstanceId, int equipSlotIndex, uint32_t attachmentId,
                             uint32_t displayInfoId);

    rendering::Renderer* renderer_;
    pipeline::AssetManager* assetManager_;
    game::GameHandler* gameHandler_;
    EntitySpawner* entitySpawner_;

    // Saved at spawn for skin re-compositing on equipment changes
    std::string bodySkinPath_;
    std::vector<std::string> underwearPaths_;
    uint32_t skinTextureSlotIndex_ = 0;
    // The player model's HD appearance table; null for a 3.3.5 model.
    std::shared_ptr<const pipeline::HdCharacterLook> playerHdLook_;
    std::vector<std::pair<uint16_t, glm::mat4>> playerHdBones_;
    uint32_t cloakTextureSlotIndex_ = 0;

    bool weaponsSheathed_ = false;
    // The Z reach in progress.
    bool sheathReachActive_ = false;
    bool sheathReachSwapped_ = false;
    bool sheathReachTarget_ = false;
    int sheathQueued_ = -1;           // -1 none, 0 draw, 1 put away
    double sheathSwapAtMs_ = 0.0;
    double sheathEndAtMs_ = 0.0;
    const char* sheathReachCause_ = "";
    const char* sheathQueuedCause_ = "";
    void startSheathReach(bool sheathed, const char* cause);
    /// Move the weapons to the new state now: the server is told, the models
    /// move, and the item's sheathe or draw sound plays if `withSound`.
    void applyWeaponsSheathed(bool sheathed, bool withSound, const char* cause);
    bool showingRanged_ = false;
    bool showingMiningPick_ = false;
    uint32_t miningPickInstanceId_ = 0;
    bool showingFishingPole_ = false;
};

} // namespace core
} // namespace wowee
