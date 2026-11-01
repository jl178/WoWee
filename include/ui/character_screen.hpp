#pragma once

#include "ui/paper_ui.hpp"
#include "ui/text_edit.hpp"
#include "ui/ui_services.hpp"
#include "game/game_handler.hpp"
#include <imgui.h>
#include <string>
#include <functional>
#include <memory>
#include <utility>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering { class CharacterPreview; }
namespace ui {

/**
 * Character selection screen UI
 *
 * Displays character list and allows user to select one to play
 */
class CharacterScreen {
public:
    CharacterScreen();

    /**
     * Render the UI
     * @param gameHandler Reference to game handler
     */
    void render(game::GameHandler& gameHandler);

    void setAssetManager(pipeline::AssetManager* am) {
        assetManager_ = am;
        previewInitialized_ = false;
        previewGuid_ = 0;
        previewAppearanceBytes_ = 0;
        previewFacialFeatures_ = 0;
        previewUseFemaleModel_ = false;
        previewEquipHash_ = 0;
    }

    /**
     * Set callback for character selection
     * @param callback Function to call when character is selected (receives character GUID)
     */
    void setOnCharacterSelected(std::function<void(uint64_t)> callback) {
        onCharacterSelected = std::move(callback);
    }

    void setOnCreateCharacter(std::function<void()> cb) { onCreateCharacter = std::move(cb); }
    void setOnBack(std::function<void()> cb) { onBack = std::move(cb); }
    void setOnDeleteCharacter(std::function<void(uint64_t)> cb) { onDeleteCharacter = std::move(cb); }

    /// Set services (dependency injection)
    void setServices(const UIServices& services) { services_ = services; }

    /// The realm the list is from, shown at the top of the character list as
    /// CharSelectRealmName is.
    void setRealmName(const std::string& name) { realmName_ = name; }

    /// Whether this screen drew its own full-window glue scene last frame -
    /// the selected character in its race's backdrop - so the login scene
    /// behind it need not be drawn as well.
    [[nodiscard]] bool drawsOwnScene() const { return sceneShown_; }

    /**
     * Reset selection state (e.g., when switching servers)
     */
    void reset() {
        selectedCharacterIndex = -1;
        characterSelected = false;
        selectedCharacterGuid = 0;
        restoredLastCharacter = false;
        newlyCreatedCharacterName.clear();
        statusMessage.clear();
        statusIsError = false;
        deleteConfirmStage = 0;
        deleteConfirm_.clear();
        dismissedStatus_.clear();
        listScroll_ = 0;
        sceneShown_ = false;
        previewInitialized_ = false;
        previewGuid_ = 0;
        previewAppearanceBytes_ = 0;
        previewFacialFeatures_ = 0;
        previewUseFemaleModel_ = false;
        previewEquipHash_ = 0;
    }

    /**
     * Check if a character has been selected
     */
    [[nodiscard]] bool hasSelection() const { return characterSelected; }

    /**
     * Get selected character GUID
     */
    [[nodiscard]] uint64_t getSelectedGuid() const { return selectedCharacterGuid; }

    /**
     * Update status message
     */
    void setStatus(const std::string& message, bool isError = false);

    /**
     * Select character by name (used after character creation)
     */
    void selectCharacterByName(const std::string& name);

private:
    UIServices services_;  // Injected service references

    // UI state
    int selectedCharacterIndex = -1;
    bool characterSelected = false;
    uint64_t selectedCharacterGuid = 0;
    bool restoredLastCharacter = false;
    bool autoEnterTried_ = false;  ///< WOWEE_AUTO_CHARACTER=name enters the world once.
    std::string newlyCreatedCharacterName;  // Auto-select this character if set

    // Status
    std::string statusMessage;
    bool statusIsError = false;

    // Callbacks
    std::function<void(uint64_t)> onCharacterSelected;
    std::function<void()> onCreateCharacter;
    std::function<void()> onBack;
    std::function<void(uint64_t)> onDeleteCharacter;
    int deleteConfirmStage = 0;  // 0=none, 1=first warning, 2=final warning

    /**
     * Get faction color based on race
     */
    [[nodiscard]] ImVec4 getFactionColor(game::Race race) const;

    /// The controls. See paper_ui.hpp - this screen is drawn rather than
    /// asked for, the same as the login and realm screens before it.
    PaperUI ui_;

    /// The selected character, standing in its race's glue scene, drawn over
    /// the whole window as the stock CharacterSelect ModelFFX is.
    void drawScene(const game::Character& character, float screenW, float screenH);
    /// The right-hand list: realm name, Change Realm, the character buttons
    /// and Create New Character, in CharacterSelectCharacterFrame's place.
    void renderCharacterList(game::GameHandler& gameHandler, float screenW, float screenH);
    /// CharacterDeleteDialog: the name, and DELETE typed to confirm.
    void renderDeleteConfirm(const game::Character& character, float screenW,
                             float screenH);
    /// GlueDialog: a message and one button. Answers true when it is pressed.
    bool glueDialog(const char* id, const std::string& message, const char* buttonLabel,
                    float screenW, float screenH);

    std::string realmName_;
    bool sceneShown_ = false;
    int listScroll_ = 0;  ///< first row shown, for more than ten characters
    TextEdit deleteConfirm_{32};
    std::string dismissedStatus_;
    bool rotatingLeft_ = false;
    bool rotatingRight_ = false;

    /// The stock AddonList dialog (checkboxes, Enable All / Disable All, Okay
    /// and Cancel), opened from the AddOns button.
    bool showAddonsWindow_ = false;
    void renderAddonsSheet(float screenW, float screenH);
    /// Opens it, remembering what was enabled so Cancel can put it back
    /// (ResetAddOns).
    void openAddons();
    std::vector<std::pair<std::string, bool>> addonsOnOpen_;
    int addonScroll_ = 0;

    /**
     * Persist / restore last selected character GUID
     */
    static std::string getConfigDir();
    void saveLastCharacter(uint64_t guid);
    uint64_t loadLastCharacter();

    // Preview (3D character portrait)
    pipeline::AssetManager* assetManager_ = nullptr;
    std::unique_ptr<rendering::CharacterPreview> preview_;
    bool previewInitialized_ = false;
    /// How long the window has been another size than the preview was built
    /// at; the preview is rebuilt at the new size once that settles.
    float previewResizeTimer_ = 0.0f;
    uint64_t previewGuid_ = 0;
    uint32_t previewAppearanceBytes_ = 0;
    uint8_t previewFacialFeatures_ = 0;
    bool previewUseFemaleModel_ = false;
    uint64_t previewEquipHash_ = 0;
};

} // namespace ui
} // namespace wowee
