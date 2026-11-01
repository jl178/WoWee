#pragma once

/// The stock 3.3.5a glue screens' look, drawn natively.
///
/// The login, character select and their dialogs are drawn by this client's
/// own code rather than by running GlueXML, but they are meant to look like
/// GlueXML: the same textures, the same fonts, at the same places. Those
/// places are written in the glue's own units - a screen 768 units tall and
/// as wide as the window's aspect makes it, anchored to its edges - so the
/// numbers below can be read straight out of AccountLogin.xml and
/// CharacterSelect.xml.

#include "ui/paper_ui.hpp"

#include <imgui.h>
#include <string>
#include <vector>

namespace wowee::ui {

namespace glue {

/// One glue unit in pixels: the window's height over 768.
float unit();

/// A glue texture by its archive path, uploaded once and kept. `additive`
/// is for the ADD-blended art (highlights, glows), which has no alpha of its
/// own: its brightness is turned into alpha so it can be drawn with ImGui's
/// ordinary blend. Null when the file is not there.
ImTextureID texture(const std::string& path, bool additive = false);

/// The texture at `path` over a..b. Nothing is drawn when it is missing.
void image(ImDrawList* dl, const std::string& path, ImVec2 a, ImVec2 b,
           ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1),
           ImU32 tint = IM_COL32_WHITE, bool additive = false);

/// GlueXML's colours.
constexpr ImU32 kNormal = IM_COL32(255, 199, 0, 255);      ///< GlueFontNormal (1, .78, 0)
constexpr ImU32 kHighlight = IM_COL32(255, 255, 255, 255); ///< GlueFontHighlight
constexpr ImU32 kDisable = IM_COL32(128, 128, 128, 255);   ///< GlueFontDisable
constexpr ImU32 kRed = IM_COL32(255, 26, 26, 255);         ///< GlueFontRed
constexpr ImU32 kGreen = IM_COL32(26, 255, 26, 255);       ///< GlueFontGreen

/// GlueXML's font heights, in glue units.
constexpr float kFontSmall = 12.0f;  ///< GlueFontNormalSmall
constexpr float kFontNormal = 15.0f; ///< GlueFontNormal
constexpr float kFontLarge = 18.0f;  ///< GlueFontNormalLarge
constexpr float kFontHuge = 22.0f;   ///< GlueFontNormalHuge

/// FRIZQT__, the face every glue font names, or ImGui's current one.
ImFont* face();

/// Text the way the glue fonts draw it: outlined and with a drop shadow.
/// `sizePx` is in pixels.
void text(ImDrawList* dl, ImVec2 at, const char* s, float sizePx, ImU32 col,
          bool outline = true);
void textCentered(ImDrawList* dl, float cx, float y, const char* s, float sizePx, ImU32 col,
                  bool outline = true);
void textRight(ImDrawList* dl, float rx, float y, const char* s, float sizePx, ImU32 col,
               bool outline = true);
float textWidth(const char* s, float sizePx);

/// MORPHEUS, the face FactionName_Shadow_Huge names, or face() without it.
ImFont* titleFace();
/// The same text in a face of the caller's choosing.
void text(ImDrawList* dl, ImFont* font, ImVec2 at, const char* s, float sizePx, ImU32 col,
          bool outline = true);
float textWidth(ImFont* font, const char* s, float sizePx);

/// A GlueStrings.lua global ("CHARACTER_CREATE_ACCEPT"), read once from
/// Interface\GlueXML\GlueStrings.lua, or `fallback` when the file or the
/// name is not there. The value is as the file has it, markup and all; see
/// plain().
std::string str(const char* key, const char* fallback = "");
/// Whether GlueStrings has `key` at all.
bool hasStr(const std::string& key);
/// A glue string with its markup taken out: |n is a line break, || a bar,
/// and |cAARRGGBB..|r colour is dropped.
std::string plain(const std::string& s);
/// `s` (already plain) broken into lines no wider than `widthPx`, at
/// `sizePx` in `font` (face() when null). Line breaks in `s` are kept, and
/// an empty paragraph is an empty line.
std::vector<std::string> wrap(const std::string& s, float widthPx, float sizePx,
                              ImFont* font = nullptr);

/// A FrameXML <Backdrop>: a tiled background inset into a border cut from an
/// edge file's eight 1:1 segments (left, right, top, bottom, then the four
/// corners). Sizes in pixels. `bgFile` or `edgeFile` may be empty.
/// `bgTint` and `edgeTint` are SetBackdropColor and SetBackdropBorderColor.
void backdrop(ImDrawList* dl, ImVec2 a, ImVec2 b, const std::string& bgFile,
              const std::string& edgeFile, float edgePx, float tilePx, float insetL,
              float insetR, float insetT, float insetB, ImU32 bgTint = IM_COL32_WHITE,
              ImU32 edgeTint = IM_COL32_WHITE);

/// The backdrop every glue edit box and the character list wear:
/// Glue-Tooltip-Background inside Glue-Tooltip-Border, 16-unit edges.
void tooltipBackdrop(ImDrawList* dl, ImVec2 a, ImVec2 b);

/// UI-DialogBox-Background inside UI-DialogBox-Border, as glue dialogs use.
void dialogBackdrop(ImDrawList* dl, ImVec2 a, ImVec2 b);

enum class ButtonStyle {
    Red,   ///< GlueButtonTemplate: Glue-Panel-Button-*
    Blue,  ///< GlueButtonTemplateBlue: Glue-Panel-Button-*-Blue
};

/// A glue panel button over a..b (pixels), labelled in `fontUnits` glue
/// units. Answers true on the frame it is clicked. Hover and press come from
/// `ui`, so a button under an open sheet does not hear the pointer.
bool button(PaperUI& ui, ImDrawList* dl, const char* id, ImVec2 a, ImVec2 b,
            const char* label, ButtonStyle style = ButtonStyle::Red, bool enabled = true,
            float fontUnits = kFontNormal);

/// A button that is nothing but its art: NormalTexture (`up`), PushedTexture
/// (`down`, while held) and an ADD HighlightTexture over it while pointed at.
/// `uv0`/`uv1` are the TexCoords all three share. `highlight` may be null.
PaperUI::PressState textureButton(PaperUI& ui, ImDrawList* dl, const char* id, ImVec2 a,
                                  ImVec2 b, const char* up, const char* down,
                                  const char* highlight, bool enabled = true,
                                  ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1));

/// A UI-CheckBox CheckButton, `sizePx` square at `at`. Answers true on the
/// frame it is clicked; the caller flips its own value.
bool checkbox(PaperUI& ui, ImDrawList* dl, const char* id, ImVec2 at, float sizePx,
              bool checked, bool enabled = true);

/// GlueCloseButton: UI-Panel-MinimizeButton, 32 units square at `a`.
bool closeButton(PaperUI& ui, ImDrawList* dl, const char* id, ImVec2 a);

/// The 640x512 HelpFrame panel the realm list and the addon list are drawn
/// on, with its top left at `a` (pixels), and the UI-DialogBox-Header over
/// its top carrying `title`, `headerOffset` units off centre.
void helpFrame(ImDrawList* dl, ImVec2 a, const char* title, float headerOffset);

/// GlueTooltip: UI-Tooltip-Border round the glue's dark blue, the text in
/// CharacterCreateTooltipFont, its top left at `at` and kept on screen.
void tooltip(ImDrawList* dl, ImVec2 at, const std::string& text);

/// GlueDialog: `message` in the dialog backdrop at the middle of the screen
/// over a dimmed one, with one button labelled `buttonLabel` (none when null,
/// for a "please wait"). Takes the whole screen's pointer while it is up.
/// Answers true on the frame the button is clicked.
bool dialog(PaperUI& ui, const char* id, const std::string& message, const char* buttonLabel);

/// The glue panel button's art and label with no behaviour of its own, for a
/// caller that has already decided hover and press (the paper skin).
void drawButton(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* label, ButtonStyle style,
                bool enabled, bool hovered, bool held, float labelPx);

/// OptionsSliderTemplate: UI-SliderBar-Background inside UI-SliderBar-Border,
/// UI-SliderBar-Button-Horizontal for the thumb, `widthPx` wide and 17 units
/// tall with its top left at `at`. `label` is drawn white above it, `low` and
/// `high` small and gold under its ends; any may be null. The value moves in
/// `step`s from `lo` (0 for none). Answers true while dragging changes it.
bool slider(PaperUI& ui, ImDrawList* dl, const char* id, ImVec2 at, float widthPx,
            float* value, float lo, float hi, float step, const char* label,
            const char* low, const char* high, bool enabled = true);

/// GlueDropDownMenuTemplate: the CharacterCreate-LabelFrame box with the
/// chat frame's scroll-down button, `widthUnits` across its middle, the
/// frame's top left at `at` (the art reaches 17 units above that, as the
/// template's does). `label` is drawn small and gold over it.
///
/// The list it opens is drawn by dropdownList(), which a screen calls once
/// after everything else, so the list lies over every control drawn after
/// the box. A choice made in the list is answered here on the next frame:
/// `*index` is set and this returns true.
bool dropdown(PaperUI& ui, ImDrawList* dl, const char* id, ImVec2 at, float widthUnits,
              const char* label, const std::vector<std::string>& items, int* index,
              bool enabled = true);
/// Whether a glue dropdown's list is open. A screen asks at the top of its
/// frame and holds its other controls inert while it is.
bool dropdownOpen();
void closeDropdown();
/// The open list, if any: drawn into `dl` (the screen's top list) and its
/// clicks taken. A click outside it closes it and is not heard by anything
/// else, as the screen was inert for the frame.
void dropdownList(PaperUI& ui, ImDrawList* dl);

/// The skin that draws PaperUI's controls as the glue's: the dialog
/// backdrop for a sheet, the panel buttons, the edit box backdrop, the
/// UI-CheckBox and radio art, the slider bar, FRIZQT with its outline.
/// Owned here; never null.
PaperSkin* paperSkin();
/// Whether the art it draws with is there to draw - false before the
/// archives are open, where a screen should stay on paper.
bool paperSkinAvailable();

}  // namespace glue
}  // namespace wowee::ui
