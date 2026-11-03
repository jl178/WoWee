#include "ui/realm_screen.hpp"
#include "ui/ui_colors.hpp"
#include "ui/glue_ui.hpp"
#include "ui/glue_audio.hpp"
#include "core/application.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"

#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <map>
#include <numeric>
#include <vector>

namespace wowee { namespace ui {

namespace {

// RealmList.xml's numbers, in glue units (see glue_ui.hpp).
constexpr int kRowsShown = 18;        ///< MAX_REALMS_DISPLAYED
constexpr float kRowStep = 16.0f + 4.0f;  ///< REALM_BUTTON_HEIGHT, each 4 under the last

/// Realm flags as the logon server sends them.
constexpr uint8_t kRealmInvalid = 0x01;
constexpr uint8_t kRealmOffline = 0x02;
constexpr uint8_t kRealmRecommended = 0x20;
constexpr uint8_t kRealmNew = 0x40;
constexpr uint8_t kRealmFull = 0x80;

constexpr ImU32 kBlue = IM_COL32(26, 128, 255, 255);  // BLUE_FONT_COLOR (.1, .5, 1)

/// A Cfg_Categories name for a realm's category, the tab it sits under.
std::string categoryName(int id) {
    static std::map<int, std::string> names;
    static bool loaded = false;
    if (!loaded) {
        auto* assets = core::Application::getInstance().getAssetManager();
        if (assets && assets->isInitialized()) {
            loaded = true;
            if (auto dbc = assets->loadDBC("Cfg_Categories.dbc")) {
                // ID, LocaleMask, CharsetMask, Flags, Name_lang...
                for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
                    names[static_cast<int>(dbc->getUInt32(r, 0))] = dbc->getString(r, 4);
                }
            }
        }
    }
    if (auto it = names.find(id); it != names.end() && !it->second.empty()) return it->second;
    char fallback[32];
    std::snprintf(fallback, sizeof(fallback), "Realms %d", id);
    return fallback;
}

} // namespace

RealmScreen::RealmScreen() {
}

void RealmScreen::render(auth::AuthHandler& authHandler) {
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float scale = std::clamp(std::min(screen.x / 1280.0f, screen.y / 760.0f), 0.62f, 2.6f);
    ui_.begin(ImGui::GetIO().DeltaTime, scale);
    ui_.setLayer(PaperLayer::Page);

    const auto& realms = authHandler.getRealms();

    // Picking a realm connects to it there and then and leaves this screen;
    // still here a frame later with one picked, the connection failed.
    const bool connectFailed = realmSelected;

    // Auto-select: prefer realm with characters, then single realm, then first available
    if (!realms.empty() && !autoSelectAttempted && !realmSelected) {
        autoSelectAttempted = true;

        int bestRealm = -1;
        for (size_t i = 0; i < realms.size(); ++i) {
            if (!realms[i].lock && realms[i].characters > 0) {
                bestRealm = static_cast<int>(i);
                break;
            }
        }

        if (realms.size() == 1 && !realms[0].lock) {
            selectedRealmIndex = 0;
            realmSelected = true;
            selectedRealmName = realms[0].name;
            selectedRealmAddress = realms[0].address;
            setStatus("Auto-selecting realm: " + realms[0].name);
            if (onRealmSelected) {
                onRealmSelected(selectedRealmName, selectedRealmAddress);
            }
        } else if (bestRealm >= 0) {
            // Pre-highlight realm with characters (don't auto-connect, let user confirm)
            selectedRealmIndex = bestRealm;
        }
    }

    const float u = glue::unit();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImU32 white = IM_COL32_WHITE;

    const auto enterRealm = [&](const auth::Realm& realm) {
        glue_audio::play("gsLoginChangeRealmOK");  // RealmListOkButton
        realmSelected = true;
        selectedRealmName = realm.name;
        selectedRealmAddress = realm.address;
        setStatus("Connecting to realm: " + realm.name);
        if (onRealmSelected) onRealmSelected(selectedRealmName, selectedRealmAddress);
    };
    const auto cancel = [&]() {
        glue_audio::play("gsLoginChangeRealmCancel");  // RealmListCancelButton
        if (onBack) onBack();
    };

    const bool fetching = realms.empty();
    const bool modalUp = fetching || connectFailed;
    if (modalUp) ui_.pushInert();

    // RealmList: the login screen behind at a quarter, and the 640x512
    // RealmListBackground at CENTER(24,0).
    dl->AddRectFilled(ImVec2(0, 0), screen, IM_COL32(0, 0, 0, 191));
    ui_.claimMouse(ImVec2(0, 0), screen);
    const ImVec2 a(screen.x * 0.5f + (24.0f - 320.0f) * u, screen.y * 0.5f - 256.0f * u);
    const ImVec2 b(a.x + 640.0f * u, a.y + 512.0f * u);
    glue::helpFrame(dl, a, glue::str("SERVER_SELECTION", "Realm Selection").c_str(), -12.0f);

    // ---- categories ---------------------------------------------------------
    // GetRealmCategories: one tab per category the list has realms in, none
    // when there is only the one.
    std::vector<int> categories;
    for (const auto& realm : realms) {
        const int c = realm.timezone;
        if (std::find(categories.begin(), categories.end(), c) == categories.end())
            categories.push_back(c);
    }
    std::sort(categories.begin(), categories.end());
    if (std::find(categories.begin(), categories.end(), category_) == categories.end()) {
        category_ = categories.empty() ? -1 : categories.front();
        if (selectedRealmIndex >= 0 && selectedRealmIndex < static_cast<int>(realms.size()))
            category_ = realms[static_cast<size_t>(selectedRealmIndex)].timezone;
    }
    const bool tabbed = categories.size() > 1;

    // The realms under the tab, in SortRealms order.
    std::vector<int> rows;
    for (int i = 0; i < static_cast<int>(realms.size()); ++i) {
        if (!tabbed || realms[static_cast<size_t>(i)].timezone == category_) rows.push_back(i);
    }
    if (sortColumn_ >= 0) {
        std::stable_sort(rows.begin(), rows.end(), [&](int l, int r) {
            const auto& x = realms[static_cast<size_t>(sortDescending_ ? r : l)];
            const auto& y = realms[static_cast<size_t>(sortDescending_ ? l : r)];
            switch (sortColumn_) {
                case 0: return x.name < y.name;
                case 1: return x.icon < y.icon;
                case 2: return x.characters > y.characters;
                default: return x.population < y.population;
            }
        });
    }

    // ---- the column headers: RealmNameSort and the rest ---------------------
    {
        struct SortColumn { const char* key; const char* fallback; float w; };
        const SortColumn columns[4] = {{"REALM_NAME", "Realm Name", 223.0f},
                                   {"REALM_TYPE", "Type", 80.0f},
                                   {"REALM_CHARACTERS", "Characters", 100.0f},
                                   {"REALM_LOAD", "Population", 138.0f}};
        const char* tabs = "Interface\\FriendsFrame\\WhoFrame-ColumnTabs";
        const float small = glue::kFontSmall * u;
        float x = a.x + 21.0f * u;
        const float y0 = a.y + 31.0f * u, y1 = a.y + 50.0f * u;
        for (int k = 0; k < 4; ++k) {
            const float x1 = x + columns[k].w * u;
            glue::image(dl, tabs, ImVec2(x, y0), ImVec2(x + 5.0f * u, y1), ImVec2(0, 0),
                        ImVec2(0.078125f, 0.59375f));
            glue::image(dl, tabs, ImVec2(x + 5.0f * u, y0), ImVec2(x1 - 4.0f * u, y1),
                        ImVec2(0.078125f, 0), ImVec2(0.90625f, 0.59375f));
            glue::image(dl, tabs, ImVec2(x1 - 4.0f * u, y0), ImVec2(x1, y1),
                        ImVec2(0.90625f, 0), ImVec2(0.96875f, 0.59375f));
            char id[16];
            std::snprintf(id, sizeof(id), "realm.sort%d", k);
            const auto st = ui_.press(id, ImVec2(x, y0), ImVec2(x1, y1));
            if (st.hovered) {
                glue::image(dl, "Interface\\PaperDollInfoFrame\\UI-Character-Tab-Highlight",
                            ImVec2(x, y0 - 2.5f * u), ImVec2(x1 + 4.0f * u, y1 + 2.5f * u),
                            ImVec2(0, 0), ImVec2(1, 1), white, true);
            }
            const std::string label = glue::str(columns[k].key, columns[k].fallback);
            const float ty = (y0 + y1) * 0.5f - small * 0.5f;
            glue::text(dl, ImVec2(x + 8.0f * u, ty), label.c_str(), small, white);
            if (sortColumn_ == k) {
                const float ax = x + 8.0f * u + glue::textWidth(label.c_str(), small) + 3.0f * u;
                const float ay = (y0 + y1) * 0.5f - 2.0f * u;
                glue::image(dl, "Interface\\Buttons\\UI-SortArrow", ImVec2(ax, ay),
                            ImVec2(ax + 9.0f * u, ay + 8.0f * u),
                            ImVec2(0, sortDescending_ ? 1.0f : 0.0f),
                            ImVec2(0.5625f, sortDescending_ ? 0.0f : 1.0f));
            }
            if (st.clicked) {
                if (sortColumn_ == k) {
                    sortDescending_ = !sortDescending_;
                } else {
                    sortColumn_ = k;
                    sortDescending_ = false;
                }
            }
            x = x1;
        }
    }

    // ---- the realms: RealmListRealmButton1..18 ------------------------------
    const int count = static_cast<int>(rows.size());
    listScroll_ = std::clamp(listScroll_, 0, std::max(0, count - kRowsShown));
    {
        const ImVec2 la(a.x + 22.0f * u, a.y + 56.0f * u);
        const ImVec2 lb(la.x + 557.0f * u, la.y + kRowStep * kRowsShown * u);
        if (count > kRowsShown && !modalUp && ui_.hover(la, lb)) {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel > 0.0f) listScroll_ = std::max(0, listScroll_ - 1);
            if (wheel < 0.0f) listScroll_ = std::min(count - kRowsShown, listScroll_ + 1);
        }
    }
    const float normal = glue::kFontNormal * u;
    const float small = glue::kFontSmall * u;
    std::string tip;
    ImVec2 tipAt(0, 0);
    for (int row = 0; row < kRowsShown; ++row) {
        const int at = listScroll_ + row;
        if (at >= count) break;
        const int index = rows[static_cast<size_t>(at)];
        const auto& realm = realms[static_cast<size_t>(index)];
        const ImVec2 ra(a.x + 22.0f * u, a.y + (56.0f + kRowStep * row) * u);
        const ImVec2 rb(ra.x + 512.0f * u, ra.y + 16.0f * u);
        const bool down = (realm.flags & kRealmOffline) != 0;
        const bool invalid = (realm.flags & kRealmInvalid) != 0;
        char id[24];
        std::snprintf(id, sizeof(id), "realm.row%d", index);
        const auto st = ui_.press(id, ra, rb, !down);
        const bool selected = (index == selectedRealmIndex);

        // RealmListHighlight on the chosen one, green where there are
        // characters, red when it will not take this client, gold otherwise.
        if (selected && !down) {
            const ImU32 tint = invalid ? IM_COL32(255, 26, 26, 255)
                             : realm.characters > 0 ? IM_COL32(26, 255, 26, 255)
                                                    : IM_COL32(255, 199, 0, 255);
            glue::image(dl, "Interface\\QuestFrame\\UI-QuestLogTitleHighlight", ra,
                        ImVec2(ra.x + 557.0f * u, rb.y), ImVec2(0, 0), ImVec2(1, 1), tint, true);
        }

        ImU32 nameCol;
        if (down) {
            nameCol = st.hovered ? IM_COL32(204, 204, 204, 255) : glue::kDisable;
        } else if (st.hovered || selected) {
            nameCol = invalid && st.hovered ? IM_COL32(255, 128, 128, 255) : glue::kHighlight;
        } else if (invalid) {
            nameCol = glue::kRed;
        } else {
            nameCol = realm.characters > 0 ? glue::kGreen : glue::kNormal;
        }
        std::string name = realm.name;
        if (realm.hasVersionInfo() && (realm.majorVersion || realm.minorVersion || realm.patchVersion)) {
            char version[32];
            std::snprintf(version, sizeof(version), " (%d.%d.%d)", realm.majorVersion,
                          realm.minorVersion, realm.patchVersion);
            name += version;
        }
        glue::text(dl, ImVec2(ra.x + 5.0f * u, ra.y + (16.0f * u - normal) * 0.5f), name.c_str(),
                   normal, nameCol);

        // $parentPVP: the realm's rules.
        std::string type;
        ImU32 typeCol = glue::kNormal;
        switch (realm.icon) {
            case 1: type = glue::str("PVP_PARENTHESES", "(PVP)"); typeCol = glue::kRed; break;
            case 6: type = glue::str("RP_PARENTHESES", "(RP)"); typeCol = glue::kGreen; break;
            case 8: type = glue::str("RPPVP_PARENTHESES", "(RPPVP)"); break;
            default: type = glue::str("GAMETYPE_NORMAL", "Normal"); break;
        }
        // $parentLoad: how full it is, or why it cannot be had.
        std::string load;
        ImU32 loadCol = glue::kNormal;
        if (down) {
            load = glue::str("REALM_DOWN", "Offline");
            loadCol = glue::kDisable;
        } else if (realm.lock) {
            load = glue::str("REALM_LOCKED", "Locked");
            loadCol = glue::kRed;
        } else if (realm.flags & kRealmRecommended) {
            load = glue::str("LOAD_RECOMMENDED", "New Players");
            loadCol = kBlue;
        } else if (realm.flags & kRealmNew) {
            load = glue::str("LOAD_NEW", "New");
            loadCol = glue::kGreen;
        } else if ((realm.flags & kRealmFull) || realm.population >= 2.5f) {
            load = glue::str("LOAD_FULL", "Full");
            loadCol = glue::kRed;
        } else if (realm.population >= 1.5f) {
            load = glue::str("LOAD_HIGH", "High");
            loadCol = glue::kRed;
        } else if (realm.population < 0.5f) {
            load = glue::str("LOAD_LOW", "Low");
            loadCol = glue::kGreen;
        } else {
            load = glue::str("LOAD_MEDIUM", "Medium");
        }
        if (selected && !down) typeCol = loadCol = glue::kHighlight;
        const float sy = ra.y + 2.0f * u;
        glue::text(dl, ImVec2(ra.x + 235.0f * u, sy), type.c_str(), small, typeCol);
        if (realm.characters > 0) {
            char players[16];
            std::snprintf(players, sizeof(players), "(%d)", realm.characters);
            glue::text(dl, ImVec2(ra.x + 335.0f * u, sy), players, small, glue::kHighlight);
        }
        glue::text(dl, ImVec2(ra.x + 412.0f * u, sy), load.c_str(), small, loadCol);

        if (st.hovered && realm.hasVersionInfo() && realm.build) {
            char line[96];
            std::snprintf(line, sizeof(line), "%d.%d.%d  %d", realm.majorVersion,
                          realm.minorVersion, realm.patchVersion, realm.build);
            tip = line;
            tipAt = ImVec2(ra.x - 50.0f * u, rb.y);
        }
        if (st.clicked) selectedRealmIndex = index;
        if (st.hovered && !down && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            !realm.lock) {
            selectedRealmIndex = index;
            enterRealm(realm);
        }
    }

    // The scroll bar's art, where there is more than one page.
    if (count > kRowsShown) {
        const char* bar = "Interface\\PaperDollInfoFrame\\UI-Character-ScrollBar";
        const float x = a.x + 557.0f * u;
        const float top = a.y + 48.0f * u, bottom = a.y + 465.0f * u;
        glue::image(dl, bar, ImVec2(x, top), ImVec2(x + 31.0f * u, top + 256.0f * u), ImVec2(0, 0),
                    ImVec2(0.484375f, 1.0f));
        glue::image(dl, bar, ImVec2(x, top + 256.0f * u), ImVec2(x + 31.0f * u, bottom - 106.0f * u),
                    ImVec2(0, 0.75f), ImVec2(0.484375f, 1.0f));
        glue::image(dl, bar, ImVec2(x, bottom - 106.0f * u), ImVec2(x + 31.0f * u, bottom),
                    ImVec2(0.515625f, 0), ImVec2(1.0f, 0.4140625f));
        const float trackTop = top + 24.0f * u, trackBottom = bottom - 24.0f * u;
        const float knobH = 24.0f * u;
        const float t = static_cast<float>(listScroll_) / static_cast<float>(count - kRowsShown);
        const float ky = trackTop + (trackBottom - trackTop - knobH) * t;
        glue::image(dl, "Interface\\Buttons\\UI-ScrollBar-Knob", ImVec2(x + 2.0f * u, ky),
                    ImVec2(x + 29.0f * u, ky + knobH));
    }

    // ---- RealmListTab1..: under the frame's bottom left ---------------------
    if (tabbed) {
        float x = a.x + 11.0f * u;
        const float top = b.y - 17.0f * u;
        for (int c : categories) {
            const std::string label = categoryName(c);
            const float mid = std::max(glue::textWidth(label.c_str(), small) + 20.0f * u, 40.0f * u);
            const float w = 40.0f * u + mid;
            const bool chosen = (c == category_);
            const char* file = chosen ? "Interface\\PaperDollInfoFrame\\UI-Character-ActiveTab"
                                      : "Interface\\PaperDollInfoFrame\\UI-Character-InActiveTab";
            const float y = top - (chosen ? 3.0f * u : 0.0f);
            glue::image(dl, file, ImVec2(x, y), ImVec2(x + 20.0f * u, y + 32.0f * u), ImVec2(0, 0),
                        ImVec2(0.15625f, 1));
            glue::image(dl, file, ImVec2(x + 20.0f * u, y), ImVec2(x + 20.0f * u + mid, y + 32.0f * u),
                        ImVec2(0.15625f, 0), ImVec2(0.84375f, 1));
            glue::image(dl, file, ImVec2(x + 20.0f * u + mid, y), ImVec2(x + w, y + 32.0f * u),
                        ImVec2(0.84375f, 0), ImVec2(1, 1));
            char id[24];
            std::snprintf(id, sizeof(id), "realm.tab%d", c);
            const auto st = ui_.press(id, ImVec2(x, top), ImVec2(x + w, top + 32.0f * u));
            if (st.hovered && !chosen) {
                glue::image(dl, "Interface\\PaperDollInfoFrame\\UI-Character-Tab-Highlight",
                            ImVec2(x + 10.0f * u, top + 2.0f * u),
                            ImVec2(x + w - 10.0f * u, top + 34.0f * u), ImVec2(0, 0), ImVec2(1, 1),
                            white, true);
            }
            glue::textCentered(dl, x + w * 0.5f, top + 16.0f * u - 2.0f * u - small * 0.5f,
                               label.c_str(), small,
                               chosen || st.hovered ? glue::kHighlight : glue::kNormal);
            if (st.clicked && !chosen) {
                category_ = c;
                listScroll_ = 0;
            }
            x += w - 15.0f * u;
        }
    }

    // ---- Okay and Cancel, and the close button --------------------------------
    const bool haveSelection = selectedRealmIndex >= 0 &&
                               selectedRealmIndex < static_cast<int>(realms.size());
    const auth::Realm* chosen = haveSelection ? &realms[static_cast<size_t>(selectedRealmIndex)]
                                              : nullptr;
    const bool canEnter = chosen && !chosen->lock && !(chosen->flags & kRealmOffline) &&
                          !realmSelected;
    {
        const ImVec2 cb(b.x - 46.0f * u, b.y - 13.0f * u);
        const ImVec2 ca(cb.x - 125.0f * u, cb.y - 35.0f * u);
        const std::string cancelLabel = glue::str("CANCEL", "Cancel");
        if (glue::button(ui_, dl, "realm.cancel", ca, cb, cancelLabel.c_str(),
                         glue::ButtonStyle::Red, true, glue::kFontNormal)) {
            cancel();
        }
        const ImVec2 ob(ca.x + 8.0f * u, cb.y);
        const ImVec2 oa(ob.x - 125.0f * u, ca.y);
        const std::string okayLabel = glue::str("OKAY", "Okay");
        if (glue::button(ui_, dl, "realm.okay", oa, ob, okayLabel.c_str(), glue::ButtonStyle::Red,
                         canEnter, glue::kFontNormal) &&
            canEnter) {
            enterRealm(*chosen);
        }
        if (glue::closeButton(ui_, dl, "realm.close",
                              ImVec2(b.x - 42.0f * u - 32.0f * u, a.y + 3.0f * u))) {
            cancel();
        }
    }

    // RealmList_OnKeyDown: Enter is Okay, Escape is Cancel; the arrows move
    // through the list.
    if (!modalUp) {
        if (canEnter && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                         ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
            enterRealm(*chosen);
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            cancel();
        } else if (count > 0 && (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) ||
                                 ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))) {
            const auto it = std::find(rows.begin(), rows.end(), selectedRealmIndex);
            int at = it == rows.end() ? -1 : static_cast<int>(it - rows.begin());
            at += ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) ? -1 : 1;
            at = std::clamp(at, 0, count - 1);
            selectedRealmIndex = rows[static_cast<size_t>(at)];
            if (at < listScroll_) listScroll_ = at;
            if (at >= listScroll_ + kRowsShown) listScroll_ = at - kRowsShown + 1;
        }
    }

    if (modalUp) ui_.popInert();
    if (!tip.empty() && !modalUp) glue::tooltip(ImGui::GetForegroundDrawList(), tipAt, tip);

    // ---- dialogs --------------------------------------------------------------
    if (connectFailed) {
        if (glue::dialog(ui_, "realm.failed",
                         "Unable to connect to " + selectedRealmName + ".",
                         glue::str("OKAY", "Okay").c_str()) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
            realmSelected = false;
            statusMessage.clear();
        }
    } else if (fetching) {
        // REALM_LIST_IN_PROGRESS, with Cancel back to the login.
        authHandler.requestRealmList();
        if (glue::dialog(ui_, "realm.fetching",
                         glue::str("REALM_LIST_IN_PROGRESS", "Retrieving realm list"),
                         glue::str("CANCEL", "Cancel").c_str())) {
            cancel();
        }
    }

    ui_.end();
}

void RealmScreen::setStatus(const std::string& message) {
    statusMessage = message;
}

}} // namespace wowee::ui
