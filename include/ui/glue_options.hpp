#pragma once

/// The login screen's Options, as the stock glue draws them.
///
/// 3.3.5a's glue Options opens VideoOptionsFrame and AudioOptionsFrame:
/// OptionsFrameTemplate's 648x520 UI-DialogBox panel with the category list
/// down its left, the page in a bordered box beside it, and Defaults, Okay,
/// Cancel and Apply along the bottom. This is that frame, drawn natively with
/// glue_ui in the glue's own units, over this client's settings rather than
/// the stock CVars: every control reads and writes a key of the settings
/// schema through SettingsPanel, so a change made here is the same change the
/// in-game panels make - applied by applySettingSideEffects and saved by
/// GameScreen::saveSettings.
///
/// The stock categories are kept (Graphics with Resolution and Effects under
/// it, Advanced, Sound & Voice), and everything else the schema offers that
/// this client keeps itself is on the pages under "Wowee", so nothing the old
/// login sheet or the in-game panel offered is missing here. Rows the schema
/// keeps in the interface's own stores (CVars, Lua) are left to the in-game
/// options, which have the interface running to answer them.
///
/// Changes are pending until Okay or Apply, as the stock frame's are; Cancel
/// and Escape drop them.

#include "ui/paper_ui.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::ui {

struct SettingDesc;
class SettingsPanel;
class GameScreen;

class GlueOptionsFrame {
public:
    /// Show it, on its first page, with nothing pending.
    void open();
    /// Hide it, dropping anything pending.
    void close();
    /// Whether it, or the restart notice it leaves after applying a change
    /// that needs one, is still up.
    [[nodiscard]] bool isOpen() const { return open_ || restartNotice_; }

    /// Draw it over the whole screen, modal, on `ui`'s overlay layer. `ui`
    /// must be between begin() and end(), and the screen behind it inert.
    void render(PaperUI& ui);

private:
    struct Page {
        std::string title;
        std::string subtext;
        std::vector<std::string> keys;
        int depth = 0;        ///< 1 for a page listed under the one before it
        bool header = false;  ///< a list row that selects the page after it
    };

    void buildPages(SettingsPanel& sp);
    void drawPage(PaperUI& ui, ImDrawList* dl, ImVec2 a, ImVec2 b, bool listOpen);

    [[nodiscard]] std::string value(const std::string& key) const;
    void set(const std::string& key, const std::string& v);
    [[nodiscard]] bool conditionHolds(const char* cond) const;
    [[nodiscard]] bool anyPending() const;
    void apply();
    void defaults();

    bool open_ = false;
    bool restartNotice_ = false;
    bool built_ = false;
    int page_ = 0;
    std::vector<Page> pages_;
    std::unordered_map<std::string, std::string> pending_;
    std::string tooltipText_;
};

}  // namespace wowee::ui
