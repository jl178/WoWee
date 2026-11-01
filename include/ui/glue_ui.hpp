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

#include <imgui.h>
#include <string>

namespace wowee::ui {

class PaperUI;

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

/// A FrameXML <Backdrop>: a tiled background inset into a border cut from an
/// edge file's eight 1:1 segments (left, right, top, bottom, then the four
/// corners). Sizes in pixels. `bgFile` or `edgeFile` may be empty.
void backdrop(ImDrawList* dl, ImVec2 a, ImVec2 b, const std::string& bgFile,
              const std::string& edgeFile, float edgePx, float tilePx, float insetL,
              float insetR, float insetT, float insetB, ImU32 bgTint = IM_COL32_WHITE);

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

}  // namespace glue
}  // namespace wowee::ui
