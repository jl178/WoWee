#include "ui/character_screen.hpp"
#include "game/equipment_hash.hpp"
#include "ui/ui_colors.hpp"
#include "ui/glue_ui.hpp"
#include "rendering/character_preview.hpp"
#include "rendering/renderer.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/application.hpp"
#include "core/config_paths.hpp"
#include "core/logger.hpp"
#include "addons/addon_manager.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace wowee { namespace ui {

namespace {

/// AddonList.lua: MAX_ADDONS_DISPLAYED rows of ADDON_BUTTON_HEIGHT, each 4
/// under the last.
constexpr int kAddonRowsShown = 19;
constexpr float kAddonRowStep = 16.0f + 4.0f;

/// Enter is claimed for the rest of the press when it takes a character into
/// the world, so the game screen does not read the same keystroke as "open
/// chat" the moment it appears. Any distinct id will do; this one is not a
/// window's, because this screen no longer has one.
constexpr ImGuiID kCharListOwner = 0xC4A21151u;

/// CharacterSelect.xml's numbers, in glue units.
constexpr float kFrameW = 260.0f;      ///< CharacterSelectCharacterFrame
constexpr float kFrameH = 642.0f;
constexpr float kRowW = 256.0f;        ///< CharSelectCharacterButtonTemplate
constexpr float kRowH = 70.0f;
constexpr float kRowStep = 70.0f - 13.0f;  ///< each anchored 13 up into the last
constexpr int kRowsShown = 10;
constexpr uint32_t kCharacterFlagGhost = 0x2000;

/// The scene GetSelectBackgroundModel answers for a class: a death knight
/// stands in Acherus whatever its race.
const char* sceneOverrideFor(const game::Character& c) {
    return c.characterClass == game::Class::DEATH_KNIGHT ? "UI_DeathKnight" : "";
}

} // namespace

CharacterScreen::CharacterScreen() {
}

void CharacterScreen::render(game::GameHandler& gameHandler) {
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float scale = std::clamp(std::min(screen.x / 1280.0f, screen.y / 760.0f), 0.62f, 2.6f);
    ui_.begin(ImGui::GetIO().DeltaTime, scale);
    ui_.setLayer(PaperLayer::Page);
    sceneShown_ = false;

    const float u = glue::unit();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    // Ensure we can render a preview even if the state transition hook didn't
    // inject the AssetManager.
    if (!assetManager_) {
        assetManager_ = core::Application::getInstance().getAssetManager();
    }

    const auto& characters = gameHandler.getCharacters();
    const auto state = gameHandler.getState();
    if (state == game::WorldState::READY) {
        gameHandler.requestCharacterList();
    }

    // If the list refreshed, keep selection stable by GUID.
    if (selectedCharacterGuid != 0 && !characters.empty()) {
        const bool needReselect =
            (selectedCharacterIndex < 0) ||
            (selectedCharacterIndex >= static_cast<int>(characters.size())) ||
            (characters[static_cast<size_t>(selectedCharacterIndex)].guid != selectedCharacterGuid);
        if (needReselect) {
            for (size_t i = 0; i < characters.size(); ++i) {
                if (characters[i].guid == selectedCharacterGuid) {
                    selectedCharacterIndex = static_cast<int>(i);
                    break;
                }
            }
        }
    }

    // Restore last-selected character (once per screen visit)
    if (!restoredLastCharacter && !characters.empty()) {
        // Priority 1: Select newly created character if set
        if (!newlyCreatedCharacterName.empty()) {
            for (size_t i = 0; i < characters.size(); ++i) {
                if (characters[i].name == newlyCreatedCharacterName) {
                    selectedCharacterIndex = static_cast<int>(i);
                    selectedCharacterGuid = characters[i].guid;
                    saveLastCharacter(characters[i].guid);
                    newlyCreatedCharacterName.clear();
                    break;
                }
            }
        }
        // Priority 2: Restore last selected character
        if (selectedCharacterIndex < 0) {
            uint64_t lastGuid = loadLastCharacter();
            if (lastGuid != 0) {
                for (size_t i = 0; i < characters.size(); ++i) {
                    if (characters[i].guid == lastGuid) {
                        selectedCharacterIndex = static_cast<int>(i);
                        selectedCharacterGuid = lastGuid;
                        break;
                    }
                }
            }
        }
        // Fall back to first character if nothing matched
        if (selectedCharacterIndex < 0) {
            selectedCharacterIndex = 0;
            selectedCharacterGuid = characters[0].guid;
        }
        // Keep the selection on the visible page of the list.
        listScroll_ = std::clamp(selectedCharacterIndex - (kRowsShown - 1), 0,
                                 std::max(0, static_cast<int>(characters.size()) - kRowsShown));
        restoredLastCharacter = true;
    }

    const bool haveSelection = selectedCharacterIndex >= 0 &&
                               selectedCharacterIndex < static_cast<int>(characters.size());

    const auto enterWorld = [&](const game::Character& character) {
        characterSelected = true;
        saveLastCharacter(character.guid);
        setStatus("Entering world with " + character.name + "...");
        gameHandler.selectCharacter(character.guid);
        if (onCharacterSelected) onCharacterSelected(character.guid);
    };
    if (!autoEnterTried_ && !characterSelected && !characters.empty()) {
        autoEnterTried_ = true;
        if (const char* env = std::getenv("WOWEE_AUTO_CHARACTER"); env && *env) {
            const std::string wanted(env);
            bool found = false;
            for (const auto& character : characters) {
                if (character.name == wanted) {
                    LOG_WARNING("Auto entering world as ", wanted);
                    enterWorld(character);
                    found = true;
                    break;
                }
            }
            if (!found) LOG_WARNING("WOWEE_AUTO_CHARACTER: no character named ", wanted);
        }
    }

    // ---- the scene ----------------------------------------------------------
    // The selected character in its race's backdrop, over the whole window.
    // With nobody to show, the login's Northrend scene (drawn by the auth
    // screen behind this one) stays, as the stock default does.
    if (haveSelection) {
        drawScene(characters[static_cast<size_t>(selectedCharacterIndex)], screen.x, screen.y);
    }

    // CharacterSelectLogo: the WotLK logo, 256x128 at TOPLEFT(3,-7). The
    // auth screen draws it with the login scene; drawn here when this
    // screen's own scene has covered that one.
    if (sceneShown_) {
        glue::image(dl, "Interface\\Glues\\Common\\Glues-WoW-WotLKLogo", ImVec2(3.0f * u, 7.0f * u),
                    ImVec2((3.0f + 256.0f) * u, (7.0f + 128.0f) * u));
    }

    const bool loading = (state == game::WorldState::READY ||
                          state == game::WorldState::CHAR_LIST_REQUESTED) && characters.empty();
    const bool disconnected = (state == game::WorldState::DISCONNECTED ||
                               state == game::WorldState::FAILED);
    const bool errorUp = statusIsError && !statusMessage.empty() &&
                         statusMessage != dismissedStatus_;
    const bool modalUp = (deleteConfirmStage != 0) || showAddonsWindow_ || loading ||
                         (disconnected && characters.empty()) || errorUp;
    if (modalUp) ui_.pushInert();

    // ---- the list -------------------------------------------------------------
    renderCharacterList(gameHandler, screen.x, screen.y);

    // ---- name, Enter World, rotation -------------------------------------------
    const float cx = screen.x * 0.5f;
    if (haveSelection) {
        const auto& character = characters[static_cast<size_t>(selectedCharacterIndex)];
        // CharSelectCharacterName: GlueFontNormalHuge at BOTTOM(0,100).
        const float huge = glue::kFontHuge * u;
        glue::textCentered(dl, cx, screen.y - 100.0f * u - huge, character.name.c_str(), huge,
                           glue::kNormal);

        // CharSelectEnterWorldButton: GlueButtonTemplate, 200x60 at BOTTOM(0,30).
        const ImVec2 ea(cx - 100.0f * u, screen.y - 30.0f * u - 60.0f * u);
        const ImVec2 eb(cx + 100.0f * u, screen.y - 30.0f * u);
        if (glue::button(ui_, dl, "cs.enter", ea, eb, "Enter World", glue::ButtonStyle::Red,
                         !disconnected, glue::kFontNormal)) {
            enterWorld(character);
        }

        // CharacterSelectRotateLeft/Right: 50x50 under it, turning while held.
        if (preview_) {
            const float bs = 50.0f * u;
            const ImVec2 la(cx - 15.0f * u - bs * 0.5f, eb.y - 19.0f * u);
            const ImVec2 ra(la.x + bs - 19.0f * u, la.y);
            const auto rotateButton = [&](const char* id, ImVec2 a, bool flip, bool& held) {
                const ImVec2 b(a.x + bs, a.y + bs);
                const auto st = ui_.press(id, a, b);
                held = st.held;
                const char* tex = st.held
                    ? "Interface\\Glues\\CharacterCreate\\UI-RotationRight-Big-Down"
                    : "Interface\\Glues\\CharacterCreate\\UI-RotationRight-Big-Up";
                glue::image(dl, tex, a, b, ImVec2(flip ? 1.0f : 0.0f, 0.0f),
                            ImVec2(flip ? 0.0f : 1.0f, 1.0f));
                if (st.hovered) {
                    const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                    glue::image(dl, "Interface\\Buttons\\UI-Common-MouseHilight",
                                ImVec2(c.x - 15.0f * u, c.y - 15.0f * u),
                                ImVec2(c.x + 15.0f * u, c.y + 15.0f * u), ImVec2(0, 0),
                                ImVec2(1, 1), IM_COL32_WHITE, true);
                }
            };
            rotateButton("cs.rotl", la, true, rotatingLeft_);
            rotateButton("cs.rotr", ra, false, rotatingRight_);
            const float dt = ImGui::GetIO().DeltaTime;
            if (rotatingLeft_) preview_->rotate(-120.0f * dt);
            if (rotatingRight_) preview_->rotate(120.0f * dt);
        }

        // Dragging on the character turns it, as it does in the stock client.
        if (preview_ && !modalUp && ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
            !rotatingLeft_ && !rotatingRight_) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            const float listX = screen.x - (5.0f + kFrameW) * u;
            if (m.x < listX && m.y < screen.y - 110.0f * u) {
                preview_->rotate(ImGui::GetIO().MouseDelta.x * 0.4f);
            }
        }
    }

    // CharacterSelectAddonsButton: 150x35 at BOTTOMLEFT(30,25).
    {
        const ImVec2 a(30.0f * u, screen.y - 25.0f * u - 35.0f * u);
        const ImVec2 b(a.x + 150.0f * u, screen.y - 25.0f * u);
        if (glue::button(ui_, dl, "cs.addons", a, b, "AddOns", glue::ButtonStyle::Red, true,
                         glue::kFontSmall)) {
            openAddons();
        }
    }
    // CharacterSelectBackButton: 100x35 at BOTTOMRIGHT(-30,25), and
    // CharacterSelectDeleteButton 165x35 to its left.
    {
        const ImVec2 bb(screen.x - 30.0f * u, screen.y - 25.0f * u);
        const ImVec2 ba(bb.x - 100.0f * u, bb.y - 35.0f * u);
        if (glue::button(ui_, dl, "cs.back", ba, bb, "Back", glue::ButtonStyle::Red, true,
                         glue::kFontSmall)) {
            if (onBack) onBack();
        }
        const ImVec2 db(ba.x, bb.y);
        const ImVec2 da(db.x - 165.0f * u, ba.y);
        if (glue::button(ui_, dl, "cs.delete", da, db, "Delete Character",
                         glue::ButtonStyle::Red, haveSelection, glue::kFontSmall)) {
            deleteConfirmStage = 1;
            deleteConfirm_.clear();
            ui_.focus("cs.deleteconfirm");
        }
    }

    // A status that is not an error ("Entering world...") is said quietly
    // over the name; an error gets the dialog below.
    if (!statusMessage.empty() && !statusIsError) {
        const float small = glue::kFontSmall * u;
        glue::textCentered(dl, cx, screen.y - 130.0f * u - small, statusMessage.c_str(), small,
                           glue::kHighlight);
    }

    // Keyboard: up and down move the selection, Enter takes it into the world.
    if (!modalUp && !characters.empty()) {
        bool moved = false;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) && selectedCharacterIndex > 0) {
            selectedCharacterIndex--;
            moved = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) &&
            selectedCharacterIndex < static_cast<int>(characters.size()) - 1) {
            selectedCharacterIndex++;
            moved = true;
        }
        if (moved) {
            selectedCharacterGuid = characters[static_cast<size_t>(selectedCharacterIndex)].guid;
            saveLastCharacter(selectedCharacterGuid);
            if (selectedCharacterIndex < listScroll_) listScroll_ = selectedCharacterIndex;
            if (selectedCharacterIndex >= listScroll_ + kRowsShown)
                listScroll_ = selectedCharacterIndex - kRowsShown + 1;
        }
        if (haveSelection && !disconnected &&
            (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
             ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
            // Claim Enter so the game screen doesn't activate chat on the same press
            ImGui::SetKeyOwner(ImGuiKey_Enter, kCharListOwner, ImGuiInputFlags_LockUntilRelease);
            ImGui::SetKeyOwner(ImGuiKey_KeypadEnter, kCharListOwner,
                               ImGuiInputFlags_LockUntilRelease);
            enterWorld(characters[static_cast<size_t>(selectedCharacterIndex)]);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (onBack) onBack();
        }
    }

    if (modalUp) ui_.popInert();

    // ---- dialogs ------------------------------------------------------------
    if (deleteConfirmStage != 0 && haveSelection) {
        renderDeleteConfirm(characters[static_cast<size_t>(selectedCharacterIndex)], screen.x,
                            screen.y);
    } else if (deleteConfirmStage != 0) {
        deleteConfirmStage = 0;  // the character it asked about is gone
    } else if (showAddonsWindow_) {
        renderAddonsSheet(screen.x, screen.y);
    } else if (disconnected && characters.empty()) {
        if (glueDialog("cs.disconnected",
                       "The server closed the connection before it sent the character list.",
                       "Okay", screen.x, screen.y)) {
            if (onBack) onBack();
        }
    } else if (loading) {
        glueDialog("cs.loading", "Retrieving character list", nullptr, screen.x, screen.y);
    } else if (errorUp) {
        if (glueDialog("cs.error", statusMessage, "Okay", screen.x, screen.y) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
            dismissedStatus_ = statusMessage;
        }
    }

    ui_.end();
}

void CharacterScreen::drawScene(const game::Character& character, float screenW, float screenH) {
    if (!assetManager_ || !assetManager_->isInitialized()) return;
    if (!preview_) {
        preview_ = std::make_unique<rendering::CharacterPreview>();
    }
    if (!previewInitialized_) {
        // The whole window: the stock character select is a ModelFFX the size
        // of the screen, looking through the scene's own camera.
        const int w = std::max(64, static_cast<int>(screenW));
        const int h = std::max(64, static_cast<int>(screenH));
        previewInitialized_ = preview_->initialize(assetManager_, w, h);
        if (!previewInitialized_) {
            LOG_WARNING("CharacterScreen: failed to init CharacterPreview");
            preview_.reset();
            return;
        }
        preview_->setSceneCameraFraming(true);
        if (auto* renderer = core::Application::getInstance().getRenderer())
            renderer->registerPreview(preview_.get());
    }

    const uint64_t equipHash = game::hashEquipmentAppearance(character.equipment);
    const bool changed =
        (previewGuid_ != character.guid) ||
        (previewAppearanceBytes_ != character.appearanceBytes) ||
        (previewFacialFeatures_ != character.facialFeatures) ||
        (previewUseFemaleModel_ != character.useFemaleModel) ||
        (previewEquipHash_ != equipHash);
    if (changed) {
        const uint8_t skin = character.appearanceBytes & 0xFF;
        const uint8_t face = (character.appearanceBytes >> 8) & 0xFF;
        const uint8_t hairStyle = (character.appearanceBytes >> 16) & 0xFF;
        const uint8_t hairColor = (character.appearanceBytes >> 24) & 0xFF;
        preview_->setGlueSceneOverride(sceneOverrideFor(character));
        if (preview_->loadCharacter(character.race, character.gender, skin, face, hairStyle,
                                    hairColor, character.facialFeatures,
                                    character.useFemaleModel)) {
            preview_->applyEquipment(character.equipment);
        }
        previewGuid_ = character.guid;
        previewAppearanceBytes_ = character.appearanceBytes;
        previewFacialFeatures_ = character.facialFeatures;
        previewUseFemaleModel_ = character.useFemaleModel;
        previewEquipHash_ = equipHash;
    }

    preview_->update(ImGui::GetIO().DeltaTime);
    preview_->render();
    preview_->requestComposite();
    if (VkDescriptorSet tex = preview_->getTextureId()) {
        ImGui::GetBackgroundDrawList()->AddImage(reinterpret_cast<ImTextureID>(tex),
                                                 ImVec2(0, 0), ImVec2(screenW, screenH));
        sceneShown_ = true;
    }
}

void CharacterScreen::renderCharacterList(game::GameHandler& gameHandler, float screenW,
                                          float /*screenH*/) {
    const float u = glue::unit();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const auto& characters = gameHandler.getCharacters();

    // CharacterSelectCharacterFrame: 260x642 at TOPRIGHT(-5,-15), in the
    // tooltip backdrop.
    const ImVec2 fa(screenW - (5.0f + kFrameW) * u, 15.0f * u);
    const ImVec2 fb(fa.x + kFrameW * u, fa.y + kFrameH * u);
    glue::tooltipBackdrop(dl, fa, fb);
    ui_.claimMouse(fa, fb);

    // CharSelectRealmName: GlueFontDisableLarge at the top, and
    // CharSelectChangeRealmButton (135x33) under it.
    const float large = glue::kFontLarge * u;
    const float cx = (fa.x + fb.x) * 0.5f;
    if (!realmName_.empty()) {
        glue::textCentered(dl, cx, fa.y + 10.0f * u, realmName_.c_str(), large, glue::kDisable);
    }
    {
        const float top = fa.y + 10.0f * u + 13.0f * u + 4.0f * u;
        const ImVec2 a(cx - 67.5f * u, top);
        const ImVec2 b(cx + 67.5f * u, top + 33.0f * u);
        if (glue::button(ui_, dl, "cs.realm", a, b, "Change Realm", glue::ButtonStyle::Red, true,
                         glue::kFontSmall)) {
            if (onBack) onBack();
        }
    }

    // The character buttons: 256x70 from TOPLEFT(24,-65), each anchored 13
    // units up into the one above. Name in GlueFontNormal, "Level N Class"
    // in GlueFontHighlightSmall, the zone in GlueFontDisableSmall, and the
    // highlight art locked on the selected one.
    const int count = static_cast<int>(characters.size());
    listScroll_ = std::clamp(listScroll_, 0, std::max(0, count - kRowsShown));
    if (count > kRowsShown && ui_.hover(fa, fb)) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel > 0.0f) listScroll_ = std::max(0, listScroll_ - 1);
        if (wheel < 0.0f) listScroll_ = std::min(count - kRowsShown, listScroll_ + 1);
    }
    const float normal = glue::kFontNormal * u;
    const float small = glue::kFontSmall * u;
    for (int row = 0; row < kRowsShown; ++row) {
        const int index = listScroll_ + row;
        if (index >= count) break;
        const auto& character = characters[static_cast<size_t>(index)];
        const ImVec2 a(fa.x + 24.0f * u, fa.y + (65.0f + kRowStep * row) * u);
        const ImVec2 b(a.x + kRowW * u, a.y + kRowH * u);
        // The clickable part stops where the next button starts.
        const ImVec2 hitB(b.x, a.y + kRowStep * u);

        char id[32];
        std::snprintf(id, sizeof(id), "cs.row%d", index);
        const auto st = ui_.press(id, a, hitB);
        const bool selected = (index == selectedCharacterIndex);
        if (selected || st.hovered) {
            // Glue-CharacterSelect-Highlight: 256x74 at TOPLEFT(-20,8), ADD.
            glue::image(dl, "Interface\\Glues\\CharacterSelect\\Glue-CharacterSelect-Highlight",
                        ImVec2(a.x - 20.0f * u, a.y - 8.0f * u),
                        ImVec2(a.x - 20.0f * u + 256.0f * u, a.y - 8.0f * u + 74.0f * u),
                        ImVec2(0, 0), ImVec2(1, 1),
                        selected ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 150), true);
        }

        float y = a.y + 5.0f * u;
        glue::text(dl, ImVec2(a.x, y), character.name.c_str(), normal,
                   selected || st.hovered ? glue::kHighlight : glue::kNormal);
        y += normal + 1.0f * u;
        char info[96];
        std::snprintf(info, sizeof(info),
                      (character.flags & kCharacterFlagGhost) ? "Level %d %s (Ghost)"
                                                              : "Level %d %s",
                      character.level, game::getClassName(character.characterClass));
        glue::text(dl, ImVec2(a.x, y), info, small, glue::kHighlight);
        y += small + 1.0f * u;
        std::string zone = gameHandler.getWhoAreaName(character.zoneId);
        glue::text(dl, ImVec2(a.x, y), zone.c_str(), small, glue::kDisable);

        if (st.clicked) {
            selectedCharacterIndex = index;
            selectedCharacterGuid = character.guid;
            saveLastCharacter(selectedCharacterGuid);
        }
        // A double click goes straight in, as it does in the stock list.
        if (st.hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            selectedCharacterIndex = index;
            selectedCharacterGuid = character.guid;
            characterSelected = true;
            saveLastCharacter(character.guid);
            setStatus("Entering world with " + character.name + "...");
            gameHandler.selectCharacter(character.guid);
            if (onCharacterSelected) onCharacterSelected(character.guid);
        }
    }

    // CharSelectCreateCharacterButton: 200x45 at the frame's BOTTOM(0,15).
    {
        const ImVec2 a(cx - 100.0f * u, fb.y - 15.0f * u - 45.0f * u);
        const ImVec2 b(cx + 100.0f * u, fb.y - 15.0f * u);
        if (glue::button(ui_, dl, "cs.create", a, b, "Create New Character",
                         glue::ButtonStyle::Red, true, glue::kFontSmall)) {
            if (onCreateCharacter) onCreateCharacter();
        }
    }
}

bool CharacterScreen::glueDialog(const char* id, const std::string& message,
                                 const char* buttonLabel, float screenW, float screenH) {
    const float u = glue::unit();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const float body = glue::kFontNormal * u;
    const float w = 384.0f * u;

    std::vector<std::string> lines;
    {
        std::string line, word;
        const float maxW = w - 60.0f * u;
        const auto flushWord = [&]() {
            if (word.empty()) return;
            const std::string trial = line.empty() ? word : line + " " + word;
            if (!line.empty() && glue::textWidth(trial.c_str(), body) > maxW) {
                lines.push_back(line);
                line = word;
            } else {
                line = trial;
            }
            word.clear();
        };
        for (char c : message) {
            if (c == ' ') flushWord(); else word.push_back(c);
        }
        flushWord();
        if (!line.empty()) lines.push_back(line);
    }
    const float lineH = body * 1.3f;
    const float buttonH = buttonLabel ? 40.0f * u + 16.0f * u : 0.0f;
    const float h = 26.0f * u + lineH * static_cast<float>(lines.size()) + 20.0f * u + buttonH;
    const ImVec2 a((screenW - w) * 0.5f, (screenH - h) * 0.5f);
    const ImVec2 b(a.x + w, a.y + h);
    fg->AddRectFilled(ImVec2(0, 0), ImVec2(screenW, screenH), IM_COL32(0, 0, 0, 90));
    glue::dialogBackdrop(fg, a, b);
    ui_.claimMouse(ImVec2(0, 0), ImVec2(screenW, screenH));
    float y = a.y + 26.0f * u;
    for (const std::string& line : lines) {
        glue::textCentered(fg, (a.x + b.x) * 0.5f, y, line.c_str(), body, glue::kHighlight);
        y += lineH;
    }
    if (!buttonLabel) return false;
    const float bw = 165.0f * u, bh = 40.0f * u;
    const ImVec2 ba((a.x + b.x - bw) * 0.5f, b.y - 16.0f * u - bh);
    return glue::button(ui_, fg, id, ba, ImVec2(ba.x + bw, ba.y + bh), buttonLabel,
                        glue::ButtonStyle::Red, true, glue::kFontNormal);
}

void CharacterScreen::renderDeleteConfirm(const game::Character& character, float screenW,
                                          float screenH) {
    const float u = glue::unit();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    ui_.setLayer(PaperLayer::Overlay);

    // CharacterDeleteBackground: 512x256 in the dialog backdrop, centred.
    fg->AddRectFilled(ImVec2(0, 0), ImVec2(screenW, screenH), IM_COL32(0, 0, 0, 120));
    const ImVec2 a((screenW - 512.0f * u) * 0.5f, (screenH - 256.0f * u) * 0.5f);
    const ImVec2 b(a.x + 512.0f * u, a.y + 256.0f * u);
    glue::dialogBackdrop(fg, a, b);
    ui_.claimMouse(ImVec2(0, 0), ImVec2(screenW, screenH));

    // UI-Dialog-Icon-AlertNew, 48x48 at LEFT(24,10).
    {
        const float cy = (a.y + b.y) * 0.5f - 10.0f * u;
        glue::image(fg, "Interface\\DialogFrame\\UI-Dialog-Icon-AlertNew",
                    ImVec2(a.x + 24.0f * u, cy - 24.0f * u), ImVec2(a.x + 72.0f * u, cy + 24.0f * u));
    }

    // CONFIRM_CHAR_DELETE in GlueFontNormalLarge, its second line white.
    const float cx = (a.x + b.x) * 0.5f;
    const float large = glue::kFontLarge * u;
    float y = a.y + 16.0f * u;
    glue::textCentered(fg, cx, y, "Do you want to delete", large, glue::kNormal);
    y += large * 1.25f;
    char line[160];
    std::snprintf(line, sizeof(line), "%s   Level %d   %s?", character.name.c_str(),
                  character.level, game::getClassName(character.characterClass));
    glue::textCentered(fg, cx, y, line, large, glue::kHighlight);
    y += large * 1.25f + 20.0f * u;
    const float small = glue::kFontSmall * u;
    glue::textCentered(fg, cx, y, "Type \"DELETE\" into the field to confirm.", small,
                       glue::kNormal);
    y += small + 5.0f * u;

    // CharacterDeleteEditBox: 130x32 under the instructions.
    bool submitted = false;
    {
        const ImVec2 ea(cx - 65.0f * u, y);
        const ImVec2 eb(cx + 65.0f * u, y + 32.0f * u);
        glue::tooltipBackdrop(fg, ImVec2(ea.x - 10.0f * u, ea.y), ImVec2(eb.x + 10.0f * u, eb.y));
        PaperUI::FieldOpts opts;
        opts.glue = true;
        const PaperUI::FieldResult r =
            ui_.field("cs.deleteconfirm", ImVec2(ea.x, ea.y + 3.0f * u),
                      ImVec2(eb.x, eb.y - 5.0f * u), deleteConfirm_, opts);
        submitted = r.submitted;
    }

    // CharacterDeleteButton1/2: Okay right of centre's left, Cancel beside it.
    std::string typed = deleteConfirm_.text();
    for (char& c : typed) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const bool confirmed = (typed == "DELETE");
    const float bw = 165.0f * u, bh = 40.0f * u;
    const float by = b.y - 16.0f * u - bh;
    const bool okay = glue::button(ui_, fg, "cs.delokay", ImVec2(cx - 6.0f * u - bw, by),
                                   ImVec2(cx - 6.0f * u, by + bh), "Okay",
                                   glue::ButtonStyle::Red, confirmed, glue::kFontNormal);
    const bool cancel = glue::button(ui_, fg, "cs.delcancel", ImVec2(cx + 7.0f * u, by),
                                     ImVec2(cx + 7.0f * u + bw, by + bh), "Cancel",
                                     glue::ButtonStyle::Red, true, glue::kFontNormal);
    if (okay || (submitted && confirmed)) {
        if (onDeleteCharacter) onDeleteCharacter(character.guid);
        deleteConfirmStage = 0;
        deleteConfirm_.clear();
        selectedCharacterIndex = -1;
        selectedCharacterGuid = 0;
        restoredLastCharacter = false;
    } else if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        deleteConfirmStage = 0;
        deleteConfirm_.clear();
    }

    ui_.setLayer(PaperLayer::Page);
}

void CharacterScreen::setStatus(const std::string& message, bool isError) {
    dismissedStatus_.clear();
    statusMessage = message;
    statusIsError = isError;
}

void CharacterScreen::openAddons() {
    addonsOnOpen_.clear();
    addonScroll_ = 0;
    if (auto* am = services_.addonManager) {
        for (const auto& addon : am->getAddons()) {
            addonsOnOpen_.emplace_back(addon.addonName, am->isAddonEnabled(addon.addonName));
        }
    }
    showAddonsWindow_ = true;
}

void CharacterScreen::renderAddonsSheet(float screenW, float screenH) {
    const float u = glue::unit();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    ui_.setLayer(PaperLayer::Overlay);

    auto* am = services_.addonManager;
    // AddonList_OnOk keeps what was ticked (SaveAddOns); AddonList_OnCancel
    // puts back what was enabled when the list was opened (ResetAddOns).
    const auto close = [&](bool keep) {
        if (!keep && am) {
            for (const auto& [name, enabled] : addonsOnOpen_) {
                if (am->isAddonEnabled(name) != enabled) am->setAddonEnabled(name, enabled);
            }
        }
        addonsOnOpen_.clear();
        showAddonsWindow_ = false;
    };

    // AddonList: the screen behind at a quarter, and the 640x512 frame at
    // CENTER(24,0) with the strip under its header the dropdown sits in.
    fg->AddRectFilled(ImVec2(0, 0), ImVec2(screenW, screenH), IM_COL32(0, 0, 0, 191));
    ui_.claimMouse(ImVec2(0, 0), ImVec2(screenW, screenH));
    const ImVec2 a(screenW * 0.5f + (24.0f - 320.0f) * u, screenH * 0.5f - 256.0f * u);
    const ImVec2 b(a.x + 640.0f * u, a.y + 512.0f * u);
    glue::helpFrame(fg, a, glue::str("ADDON_LIST", "AddOn List").c_str(), -22.0f);
    {
        const ImVec2 uv0(0, 0.12109375f), uv1(1, 0.234375f);
        const float y0 = a.y + 50.0f * u, y1 = y0 + 29.0f * u;
        glue::image(fg, "Interface\\HelpFrame\\HelpFrame-TopLeft", ImVec2(a.x, y0),
                    ImVec2(a.x + 256.0f * u, y1), uv0, uv1);
        glue::image(fg, "Interface\\HelpFrame\\HelpFrame-Top", ImVec2(a.x + 256.0f * u, y0),
                    ImVec2(a.x + 512.0f * u, y1), uv0, uv1);
        glue::image(fg, "Interface\\HelpFrame\\HelpFrame-TopRight", ImVec2(a.x + 512.0f * u, y0),
                    ImVec2(b.x, y1), uv0, uv1);
    }

    const float normal = glue::kFontNormal * u;
    const float small = glue::kFontSmall * u;
    std::string tip;
    ImVec2 tipAt(0, 0);

    const auto* addons = am ? &am->getAddons() : nullptr;
    const int count = addons ? static_cast<int>(addons->size()) : 0;
    if (count == 0) {
        const char* none = am ? "No AddOns are installed." : "The addon system is not running.";
        glue::textCentered(fg, (a.x + b.x) * 0.5f - 22.0f * u, a.y + 100.0f * u, none, normal,
                           glue::kNormal);
    }

    // AddonListEntry1..19 from TOPLEFT(37,-80): the checkbox, the title in
    // gold (grey when off), and the status beside it.
    addonScroll_ = std::clamp(addonScroll_, 0, std::max(0, count - kAddonRowsShown));
    {
        const ImVec2 la(a.x + 37.0f * u, a.y + 72.0f * u);
        const ImVec2 lb(la.x + 520.0f * u, la.y + kAddonRowStep * kAddonRowsShown * u);
        if (count > kAddonRowsShown && ui_.hover(la, lb)) {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel > 0.0f) addonScroll_ = std::max(0, addonScroll_ - 1);
            if (wheel < 0.0f) addonScroll_ = std::min(count - kAddonRowsShown, addonScroll_ + 1);
        }
    }
    for (int row = 0; row < kAddonRowsShown && addons; ++row) {
        const int index = addonScroll_ + row;
        if (index >= count) break;
        const auto& addon = (*addons)[static_cast<size_t>(index)];
        const bool enabled = am->isAddonEnabled(addon.addonName);
        const ImVec2 ea(a.x + 37.0f * u, a.y + (80.0f + kAddonRowStep * row) * u);
        const ImVec2 eb(ea.x + 520.0f * u, ea.y + 16.0f * u);

        char id[40];
        std::snprintf(id, sizeof(id), "addon.check%d", index);
        if (glue::checkbox(ui_, fg, id, ImVec2(ea.x + 5.0f * u, ea.y - 8.0f * u), 32.0f * u,
                           enabled)) {
            am->setAddonEnabled(addon.addonName, !enabled);
        }

        const std::string title = glue::plain(addon.getTitle());
        glue::text(fg, ImVec2(ea.x + 42.0f * u, ea.y + (16.0f * u - normal) * 0.5f),
                   title.c_str(), normal, enabled ? glue::kNormal : glue::kDisable);
        if (!enabled) {
            const std::string status = glue::str("ADDON_DISABLED", "Disabled");
            glue::text(fg, ImVec2(ea.x + 292.0f * u, ea.y + 2.0f * u), status.c_str(), small,
                       glue::kNormal);
        }

        // AddonTooltip: the title, the notes in white, and what it needs.
        if (ui_.hover(ImVec2(ea.x + 40.0f * u, ea.y), eb)) {
            tip = title;
            if (auto it = addon.directives.find("Notes");
                it != addon.directives.end() && !it->second.empty()) {
                tip += "\n" + glue::plain(it->second);
            }
            for (const char* key : {"Dependencies", "RequiredDeps", "Dep"}) {
                if (auto it = addon.directives.find(key);
                    it != addon.directives.end() && !it->second.empty()) {
                    tip += "\n" + glue::str("ADDON_DEPENDENCIES", "Dependencies: ") + it->second;
                    break;
                }
            }
            tipAt = ImVec2(a.x - 226.0f * u, ea.y);
        }
    }

    // The scroll bar's art, where there is more than one page.
    if (count > kAddonRowsShown) {
        const char* bar = "Interface\\PaperDollInfoFrame\\UI-Character-ScrollBar";
        const float x = a.x + 557.0f * u;
        const float top = a.y + 68.0f * u, bottom = a.y + 465.0f * u;
        glue::image(fg, bar, ImVec2(x, top), ImVec2(x + 31.0f * u, top + 256.0f * u), ImVec2(0, 0),
                    ImVec2(0.484375f, 1.0f));
        glue::image(fg, bar, ImVec2(x, top + 256.0f * u), ImVec2(x + 31.0f * u, bottom - 106.0f * u),
                    ImVec2(0, 0.75f), ImVec2(0.484375f, 1.0f));
        glue::image(fg, bar, ImVec2(x, bottom - 106.0f * u), ImVec2(x + 31.0f * u, bottom),
                    ImVec2(0.515625f, 0), ImVec2(1.0f, 0.4140625f));
        const float trackTop = top + 24.0f * u, trackBottom = bottom - 24.0f * u;
        const float knobH = 24.0f * u;
        const float t = static_cast<float>(addonScroll_) / static_cast<float>(count - kAddonRowsShown);
        const float ky = trackTop + (trackBottom - trackTop - knobH) * t;
        glue::image(fg, "Interface\\Buttons\\UI-ScrollBar-Knob", ImVec2(x + 2.0f * u, ky),
                    ImVec2(x + 29.0f * u, ky + knobH));
    }

    // AddonListDisableAllButton / EnableAllButton at BOTTOMLEFT(16,13), and
    // Cancel at BOTTOMRIGHT(-46,13) with Okay beside it.
    {
        const float y1 = b.y - 13.0f * u, y0 = y1 - 35.0f * u;
        const float x0 = a.x + 16.0f * u;
        const std::string disableAll = glue::str("DISABLE_ALL_ADDONS", "Disable All");
        const std::string enableAll = glue::str("ENABLE_ALL_ADDONS", "Enable All");
        if (glue::button(ui_, fg, "addon.disableall", ImVec2(x0, y0), ImVec2(x0 + 160.0f * u, y1),
                         disableAll.c_str(), glue::ButtonStyle::Red, count > 0, glue::kFontNormal) &&
            addons) {
            for (const auto& addon : *addons) am->setAddonEnabled(addon.addonName, false);
        }
        if (glue::button(ui_, fg, "addon.enableall", ImVec2(x0 + 160.0f * u, y0),
                         ImVec2(x0 + 320.0f * u, y1), enableAll.c_str(), glue::ButtonStyle::Red,
                         count > 0, glue::kFontNormal) &&
            addons) {
            for (const auto& addon : *addons) am->setAddonEnabled(addon.addonName, true);
        }

        const ImVec2 cb(b.x - 46.0f * u, y1);
        const ImVec2 ca(cb.x - 125.0f * u, y0);
        const std::string cancelLabel = glue::str("CANCEL", "Cancel");
        const std::string okayLabel = glue::str("OKAY", "Okay");
        if (glue::button(ui_, fg, "addon.cancel", ca, cb, cancelLabel.c_str(),
                         glue::ButtonStyle::Red, true, glue::kFontNormal)) {
            close(false);
        } else if (glue::button(ui_, fg, "addon.okay", ImVec2(ca.x + 8.0f * u - 125.0f * u, y0),
                                ImVec2(ca.x + 8.0f * u, y1), okayLabel.c_str(),
                                glue::ButtonStyle::Red, true, glue::kFontNormal)) {
            close(true);
        } else if (glue::closeButton(ui_, fg, "addon.close",
                                     ImVec2(b.x - 42.0f * u - 32.0f * u, a.y + 3.0f * u))) {
            close(false);
        }
    }

    // AddonList_OnKeyDown: Escape is Cancel, Enter is Okay.
    if (showAddonsWindow_) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            close(false);
        } else if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                   ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
            close(true);
        }
    }

    if (!tip.empty()) glue::tooltip(fg, tipAt, tip);
    ui_.setLayer(PaperLayer::Page);
}

void CharacterScreen::selectCharacterByName(const std::string& name) {
    newlyCreatedCharacterName = name;
    restoredLastCharacter = false;  // Allow re-selection in render()
    selectedCharacterIndex = -1;
}

ImVec4 CharacterScreen::getFactionColor(game::Race race) const {
    // Alliance races: blue
    if (race == game::Race::HUMAN ||
        race == game::Race::DWARF ||
        race == game::Race::NIGHT_ELF ||
        race == game::Race::GNOME ||
        race == game::Race::DRAENEI) {
        return ImVec4(0.3f, 0.5f, 1.0f, 1.0f);
    }

    // Horde races: red
    if (race == game::Race::ORC ||
        race == game::Race::UNDEAD ||
        race == game::Race::TAUREN ||
        race == game::Race::TROLL ||
        race == game::Race::BLOOD_ELF) {
        return ui::colors::kRed;
    }

    return ui::colors::kWhite;
}

std::string CharacterScreen::getConfigDir() {
    return core::getConfigRoot();
}

void CharacterScreen::saveLastCharacter(uint64_t guid) {
    std::string dir = getConfigDir();
    std::filesystem::create_directories(dir);
    std::ofstream f(dir + "/last_character.cfg");
    if (f) f << guid;
}

uint64_t CharacterScreen::loadLastCharacter() {
    std::string path = getConfigDir() + "/last_character.cfg";
    std::ifstream f(path);
    uint64_t guid = 0;
    if (f) f >> guid;
    return guid;
}

}} // namespace wowee::ui
