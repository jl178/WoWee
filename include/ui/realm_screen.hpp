#pragma once

#include "auth/auth_handler.hpp"
#include "ui/paper_ui.hpp"
#include <imgui.h>
#include <string>
#include <functional>
#include <utility>

namespace wowee { namespace ui {

/**
 * Realm selection screen UI
 *
 * Displays available realms and allows user to select one
 */
class RealmScreen {
public:
    RealmScreen();

    /**
     * Render the UI
     * @param authHandler Reference to auth handler
     */
    void render(auth::AuthHandler& authHandler);

    /**
     * Set callback for realm selection
     * @param callback Function to call when realm is selected (receives realm name and address)
     */
    void setOnRealmSelected(std::function<void(const std::string&, const std::string&)> callback) {
        onRealmSelected = std::move(callback);
    }

    void setOnBack(std::function<void()> cb) { onBack = std::move(cb); }

    /**
     * Reset selection state (e.g., when switching servers)
     */
    void reset() {
        selectedRealmIndex = -1;
        realmSelected = false;
        autoSelectAttempted = false;
        selectedRealmName.clear();
        selectedRealmAddress.clear();
        statusMessage.clear();
        listScroll_ = 0;
        category_ = -1;
        sortColumn_ = -1;
    }

    /**
     * Reset for back-navigation from character screen.
     * Preserves autoSelectAttempted so single-realm auto-connect doesn't re-fire.
     */
    void resetForBack() {
        selectedRealmIndex = -1;
        realmSelected = false;
        selectedRealmName.clear();
        selectedRealmAddress.clear();
        statusMessage.clear();
    }

    /**
     * Check if a realm has been selected
     */
    [[nodiscard]] bool hasSelection() const { return realmSelected; }

    /**
     * Get selected realm info
     */
    [[nodiscard]] const std::string& getSelectedName() const { return selectedRealmName; }
    [[nodiscard]] const std::string& getSelectedAddress() const { return selectedRealmAddress; }

private:
    // UI state
    int selectedRealmIndex = -1;
    bool realmSelected = false;
    bool autoSelectAttempted = false;
    std::string selectedRealmName;
    std::string selectedRealmAddress;

    int listScroll_ = 0;     ///< the first of the eighteen rows shown
    int category_ = -1;      ///< the realm category (Cfg_Categories) whose tab is up; -1 not yet
    int sortColumn_ = -1;    ///< SortRealms: 0 name, 1 type, 2 characters, 3 population; -1 as sent
    bool sortDescending_ = false;

    // Status
    std::string statusMessage;

    /// The controls. See paper_ui.hpp - this screen is drawn rather than
    /// asked for, the same as the login screen it follows.
    PaperUI ui_;

    // Callbacks
    std::function<void(const std::string&, const std::string&)> onRealmSelected;
    std::function<void()> onBack;

    /**
     * Update status message
     */
    void setStatus(const std::string& message);
};

}} // namespace wowee::ui
