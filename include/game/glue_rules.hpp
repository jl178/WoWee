#pragma once

// What the stock 3.3.5a glue screens (login, character select, character
// create, the loading screen) decide, as plain functions over plain data.
// Each rule cites where it was read: GlueXML (GlueParent.lua and friends) or
// Wow.exe build 12340. docs/re/glue-screens.md has the whole picture.

#include <glm/glm.hpp>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace wowee {
namespace game {
namespace glue {

// ---- scenes ----------------------------------------------------------------

/// GlueParent.lua's key for a glue scene: SetBackgroundModel's argument
/// upper-cased, which is the M2's name without "UI_" ("UI_NightElf" ->
/// "NIGHTELF"). The key indexes CharModelFogInfo, RaceLights and
/// GlueAmbienceTracks.
std::string sceneKey(std::string_view sceneName);

/// One RaceLights row, the arguments of ModelFFX:AddLight after `enabled`:
/// a directional light and an ambient, either of which may be black.
struct SceneLight {
    bool omni = false;
    glm::vec3 dir{0.0f, 0.0f, -1.0f};  ///< the way the light travels
    float ambIntensity = 0.0f;
    glm::vec3 ambColor{0.0f};
    float dirIntensity = 0.0f;
    glm::vec3 dirColor{0.0f};
};

/// GlueParent.lua's RaceLights[key], which SetLighting adds to the
/// background, character and pet light sets. Adding any light to a set
/// replaces the model's own directional lights for that set; the model's
/// point lights stay ("pulls the default point lights from the models").
/// Empty for a key with no row (the login scene), which keeps the model's.
const std::vector<SceneLight>& raceLights(std::string_view key);

/// GlueAmbienceTracks[key]: the SoundEntries name SetBackgroundModel plays
/// with PlayGlueAmbience(name, 4.0). Empty when the table has none.
std::string_view ambienceForScene(std::string_view key);

/// PlayGlueAmbience's fade-in as GlueXML calls it, and StopGlueAmbience's
/// fade-out (Wow.exe 0x4dc130 passes 1.0).
constexpr float kAmbienceFadeInSeconds = 4.0f;
constexpr float kAmbienceFadeOutSeconds = 1.0f;
/// PlayGlueMusic (0x9861c0): GS_LichKing, faded out over 3 s when replaced.
constexpr const char* kGlueMusic = "GS_LichKing";
constexpr const char* kLoginAmbience = "GlueScreenIntro";  ///< "DARKPORTAL"

/// GlueParent_OnLoad: a window wider than 16:9 is pillarboxed to 16:9, so
/// no glue ModelFFX is ever wider than that.
constexpr float kMaxGlueAspect = 16.0f / 9.0f;

/// The vertical field of view a ModelFFX looks through, in radians, from the
/// M2 camera's diagonal one. 0x6bfe00: vertical = diagonal / sqrt(1 + a^2),
/// a = the frame's width / height - which GlueParent caps at 16:9.
float verticalFovFromDiagonal(float diagonalRadians, float aspect);

/// SetCharacterCreateFacing(-15) on show and on every race click
/// (CharacterCreate.lua); the select screen's is 0 on every selection
/// (0x4e3cd0). Degrees from the scene's attachment 0, which the character
/// is attached to.
constexpr float kCreateFacingDegrees = -15.0f;
constexpr float kSelectFacingDegrees = 0.0f;
/// CHARACTER_ROTATION_CONSTANT (degrees per pixel of drag) and
/// CHARACTER_FACING_INCREMENT (degrees per frame a rotate button is held).
constexpr float kRotationDegreesPerPixel = 0.6f;
constexpr float kFacingIncrementDegrees = 2.0f;

/// A ghost at character select: SpellVisualKit 989, which 0x4e2ce0 applies
/// to the model - colour 0x8CBAFD on the model (CM2Model +0x180) and alpha
/// 0.5 (+0x178) - with Spells\Ghost_state.mdx on the Base attachment.
constexpr glm::vec3 kGhostTint{0x8C / 255.0f, 0xBA / 255.0f, 0xFD / 255.0f};
constexpr float kGhostAlpha = 0.5f;
constexpr uint32_t kCharacterFlagGhost = 0x2000;

// ---- loading screen (LoadingScreen.cpp) -------------------------------------

/// 0x409ed0: LoadingScreens.HasWideScreen art is used when the window's
/// aspect exceeds 4:3 by more than 0.001.
bool useWideLoadingArt(float aspect);

/// A rectangle in window pixels, y down.
struct Rect {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    [[nodiscard]] float width() const { return x1 - x0; }
    [[nodiscard]] float height() const { return y1 - y0; }
};

/// 0x40a270: the art keeps its own aspect (4:3, or 1.6 for the wide art)
/// and is fitted inside the window, centred, with black around it.
Rect loadingImageRect(float screenW, float screenH, bool wideArt);

/// The bar, in the image's rectangle (0x9e2dfc): the border 0.6 x 0.05 of
/// the image, centred 0.075 above its bottom edge; the fill 0.525 x 0.025 at
/// the same centre, its right edge at left + width x progress, with the
/// whole fill texture stretched over what has filled.
struct LoadingBar {
    Rect border;
    Rect fill;  ///< zero width at no progress
};
LoadingBar loadingBarRects(const Rect& image, float progress);

/// 0x4079d0 and its setters (0x40af40, 0x40af90): the bar never moves back.
/// Each report is ignored unless it is larger than the one before.
inline float advanceLoadingProgress(float current, float reported) {
    return reported > current ? (reported > 1.0f ? 1.0f : reported) : current;
}

// ---- the random name (0x4dfca0, 0x4e6560, 0x4e63a0) -------------------------

/// GetRandomName: a chain over the race and sex's NameGen.dbc names. Each
/// name, lower-cased with "_" in front, is cut into overlapping three-letter
/// pieces (the last only two) counted by how often they occur. A name starts
/// on a "_" piece and grows by a piece whose first two letters are the name's
/// last two, drawn by count, until a two-letter (final) piece or 13 letters.
class NameChain {
public:
    void build(const std::vector<std::string>& names);
    [[nodiscard]] bool empty() const { return pieces_.empty(); }
    /// One raw draw, capitalised, possibly empty.
    [[nodiscard]] std::string draw(std::mt19937& rng) const;
    /// GetRandomName's loop: draws until a name of 4 to 10 letters that
    /// `valid` accepts (the client checks it as it would a typed name).
    template <class Valid>
    [[nodiscard]] std::string generate(std::mt19937& rng, Valid valid, int maxTries = 200) const {
        for (int i = 0; i < maxTries; ++i) {
            std::string n = draw(rng);
            if (n.empty()) return n;
            if (n.size() >= 4 && n.size() <= 10 && valid(n)) return n;
        }
        return {};
    }

private:
    struct Piece {
        char key[4] = {0, 0, 0, 0};
        uint32_t count = 0;
    };
    std::vector<Piece> pieces_;  ///< sorted by key, as the client's qsort leaves them
};

/// The client's own name rules the generator's names have to pass
/// (CHAR_NAME_SUCCESS): letters only, no letter three times running.
bool plausibleCharacterName(std::string_view name);

} // namespace glue
} // namespace game
} // namespace wowee
