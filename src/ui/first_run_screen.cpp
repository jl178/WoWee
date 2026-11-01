#include "ui/first_run_screen.hpp"

#ifdef WOWEE_HAVE_ASSET_PANEL

#include <algorithm>

#include <imgui.h>
#include "core/application.hpp"
#include "ui/glue_ui.hpp"
#include "ui/paper_ui.hpp"

namespace wowee::ui {

namespace {

/// Wide enough for a path and the Browse button beside it without the field
/// collapsing, and short enough to leave the backdrop visible around it.
/// Both are in the same units the login card is laid out in, and scaled by
/// the same figure below.
constexpr float kCardWidth = 760.0f;
constexpr float kCardMargin = 34.0f;

}  // namespace

FirstRunScreen::FirstRunScreen() { assets::initPanelDefaults(app_); }

bool FirstRunScreen::render() {
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    // The figure the login card sizes itself by, so this screen is drawn at
    // the same size as the one it stands in front of. A share of the window
    // rather than a count of pixels, for the reason AuthScreen gives.
    const float scale = std::clamp(std::min(screen.x / 1280.0f, screen.y / 760.0f), 0.62f, 2.6f);
    ui_.begin(ImGui::GetIO().DeltaTime, scale);
    ui_.setLayer(PaperLayer::Page);

    // Two ways in, and they need different sentences. A first run arrives
    // here because there is nothing to play with; somebody who came from the
    // login screen already has a game and wants another, and telling them
    // they have no assets would be wrong.
    const bool alreadyHaveAssets =
        core::Application::getInstance().getAssetInventory().anyUsable();
    // Reached from the glue login's "Game Assets" button, the page wears the
    // glue's look like the screen it came from: a glue dialog, glue buttons
    // and edit boxes, the same controls doing the same things. A first run
    // has no archives to draw that with, and stays on paper.
    const bool glueLook = alreadyHaveAssets && glue::paperSkinAvailable();
    ui_.setSkin(glueLook ? glue::paperSkin() : nullptr);

    const float margin = ui_.px(kCardMargin);
    const float width = std::min(ui_.px(kCardWidth), screen.x - margin * 2.0f);
    const ImVec2 a((screen.x - width) * 0.5f, margin);
    const ImVec2 b(a.x + width, screen.y - margin);
    ui_.sheet(a, b, true);
    ui_.claimMouse(a, b);

    const PaperTheme& theme = ui_.theme();
    const float inset = ui_.px(34);
    const float x0 = a.x + inset;
    const float x1 = b.x - inset;
    float y = a.y + ui_.px(28);

    const float titleSize = ui_.px(30);
    if (glueLook) {
        // UI-DialogBox-Header over the dialog's top, as glue dialogs title
        // themselves, rather than a heading inside it.
        const float u = glue::unit();
        const float cx = (a.x + b.x) * 0.5f;
        const float top = a.y - 12.0f * u;
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        glue::image(dl, "Interface\\DialogFrame\\UI-DialogBox-Header", ImVec2(cx - 150.0f * u, top),
                    ImVec2(cx + 150.0f * u, top + 68.0f * u));
        glue::textCentered(dl, cx, top + 14.0f * u, "Game Assets", glue::kFontNormal * u,
                           glue::kNormal);
    } else {
        ui_.text(ImVec2(x0, y), alreadyHaveAssets ? "Game assets" : "Before you can play",
                 titleSize, theme.ink, true);
    }
    if (alreadyHaveAssets) {
        // And a way back, which a first run neither needs nor should have:
        // there is nowhere for it to go.
        const float row = ui_.px(30);
        const float w = ui_.textWidth("Back to login", row * 0.44f) + ui_.px(40);
        if (ui_.button("firstrun.back", ImVec2(x1 - w, y), ImVec2(x1, y + row),
                       "Back to login")) {
            core::Application::getInstance().setState(core::AppState::AUTHENTICATION);
        }
    }
    y += ui_.inkHeight(titleSize, true) + ui_.px(8);
    y += ui_.wrapped(ImVec2(x0, y), x1 - x0,
                     alreadyHaveAssets
                         ? "Build another game into the same folder, or rebuild what is "
                           "already there. The client offers everything it finds at the "
                           "login screen. Nothing is written into the installation you "
                           "build from."
                         : "WoWee has no game assets yet, so there is nothing to log in to. "
                           "Point it at a World of Warcraft installation you own and it "
                           "will build what it needs. Nothing is written into that "
                           "installation.",
                     ui_.px(15), theme.inkSoft);

    const bool done = assets::finishedSuccessfully(app_);
    // Restart rather than carrying on into the game from here. The asset
    // manager, the DBC tables, the model and terrain loaders and the addon
    // environment are all built once during startup from a data path that
    // was empty at the time, and the interface's glyph atlas cannot be
    // rebuilt mid-session either. Reopening is one line to say and nothing to
    // go wrong; re-running all of that in place is not.
    const char* restart = "Assets built. Close WoWee and open it again to use them.";
    const float restartSize = ui_.px(16);
    const float bottom = b.y - ui_.px(26) -
                         (done ? ui_.lineHeight(restartSize) + ui_.px(10) : 0.0f);
    assets::drawPanel(app_, ui_, ImVec2(x0, y + ui_.px(4)), ImVec2(x1, bottom));
    if (done) {
        ui_.text(ImVec2(x0, bottom + ui_.px(10)), restart, restartSize, theme.crayonGreen);
    }

    ui_.end();
    return done;
}

}  // namespace wowee::ui

#endif  // WOWEE_HAVE_ASSET_PANEL
