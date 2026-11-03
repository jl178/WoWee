#pragma once

/// What the quality presets mean, for both places that offer them.
///
/// The settings window and the login screen each had their own copy of these
/// numbers, and they had drifted: the same preset named the same thing meant a
/// different picture depending on which one you chose it from. High was 350
/// yards of shadow with 4x MSAA and no FXAA in game, and 250 yards with 2x and
/// FXAA on at the login screen; Medium differed in clutter and multisampling;
/// Low still turned shadows off there after the client stopped honouring that.
///
/// It carries no renderer includes, so the login screen, the settings panel and
/// a test can all read the same rows.

#include "rendering/enhancements.hpp"
#include "ui/enhancement_rows.hpp"
#include "ui/graphics_defaults.hpp"

namespace wowee {
namespace ui {

/// What each quality preset means, in the order Low, Medium, High, Ultra,
/// Classic. Classic is last because it was added last, not because it is the
/// most demanding: it is off the Low-to-Ultra ladder (see kClassicPresetIndex).
///
/// That order is what the config file holds. graphics_preset is written as an
/// index - 0 for Custom, then one per row here - so a preset added at the front
/// or in the middle changes what every saved file means, and a player who chose
/// Ultra comes back on High. Add at the end, or migrate the stored value.
///
/// The same is true of the antiAliasing and parallaxQuality columns below: each
/// is an index into a dropdown whose order is also persisted, so a row here can
/// be wrong in two directions at once if either list is reordered. See the note
/// on SettingDesc::choices.
///
/// One row per preset, where there used to be a block per preset - the same
/// ten settings assigned and then pushed at the renderer four times over. The
/// blocks had drifted apart, as four copies of one fact do:
///
///   * Low set a shadow distance of 100 and never told the renderer, so the
///     field said 100 and the shadows stayed at whatever the last preset left.
///   * Only Ultra had an opinion about FXAA. Going from Ultra to Low turned
///     everything else down and left FXAA running.
///   * Low left the normal map strength and parallax quality at whatever they
///     were, which does not matter while both are off and does the moment one
///     is switched back on by hand.
///
/// Reading them as a table is also what lets a preset be recognised again
/// afterwards without writing the numbers out a second time.
struct GraphicsPresetValues {
    float viewDistance;
    float shadowDistance;
    int   antiAliasing;      ///< index into the four the panel offers
    bool  fxaa;
    int   groundClutter;     ///< percent
    /// Every enhancement over the stock picture, stated on every row: picking
    /// Ultra after Classic has to bring them back, and a row that left any of
    /// them alone would not. Classic's is Enhancements{}, the stock client, by
    /// construction (test_enhancements_stock). The others start from the
    /// table's recommended values (kEnhancementRows) and differ only where a
    /// preset trades a feature for frames.
    rendering::Enhancements enh;
};

/// A preset's enhancements: the recommended set, with what the lower presets
/// give up for speed.
///
/// Normal mapping and parallax are on from Medium up, by choice: Jered
/// prefers the lit relief on the ground and walls (docs/enhancements-plan.md
/// section 6 leaves the question open). The 3.3.5 art ships no such maps -
/// they are generated from the colour texture - so this is a deliberate
/// departure; characters never use them, because generated maps embossed the
/// composited skins. Low drops them, its mist to the light volume, and the
/// shoreline foam and wet ground that cost a little on every water and ground
/// pixel. The grass columns are the generator's own experiment and off on
/// every row; their sliders stay at the values the presets always gave them.
constexpr rendering::Enhancements presetEnhancements(int level /* 0 Low .. 3 Ultra */) {
    rendering::Enhancements e = recommendedEnhancements();
    // The sun's shadow map is on every step of the ladder: picking a preset
    // is asking for its quality columns, shadow distance included. The
    // recommended set (a fresh install) leaves it off for its cost; see the
    // shadows row in kEnhancementRows.
    e.shadows = true;
    e.grassDensity = 70;
    e.grassHeight = 50;
    e.grassDistance = 215;
    constexpr float kBump[] = {0.6f, 0.6f, 0.8f, 1.2f};
    e.normalMapStrength = kBump[level];
    e.parallaxQuality = level <= 1 ? 0 : level - 1;
    if (level == 0) {
        e.normalMapping = false;
        e.parallax = false;
        e.lightShafts = 1;
        e.shorelineFoam = false;
        e.wetGround = false;
    }
    return e;
}

// The quality columns. Shadows are on from Low up (an enhancement row, so
// Classic has the stock blob instead); Low leans on its short shadow distance,
// which is where the cost is. FXAA is off on every row: Ultra used to turn it
// on over 8x MSAA, where there is nothing left for it to find and its sub-pixel
// filter only softened the resolve.
// Classic is the stock picture: every enhancement off (the unit blob rather
// than the shadow map, still trees and cloth, Light.dbc's fog and shadow
// darkness exactly, no mist, flare, sun shafts, foam, wet ground, generated
// relief, point lights on the ground, glow halos or flicker), with High's
// quality columns at the default view distance, where the fog is Light.dbc's
// own (LightingManager::kFogReferenceViewDistance). Its view distance stays at
// that default rather than the stock clamp of 791.7 yards below map 530 - a
// question left open for Jered (enhancements-plan.md section 6), since the fog
// reference is tied to it.
constexpr GraphicsPresetValues kGraphicsPresets[] = {
    /* Low    */ {.viewDistance = 600.0f,  .shadowDistance = 100.0f, .antiAliasing = 0, .fxaa = false, .groundClutter = 25,  .enh = presetEnhancements(0)},
    /* Medium */ {.viewDistance = 1000.0f, .shadowDistance = 200.0f, .antiAliasing = 1, .fxaa = false, .groundClutter = 60,  .enh = presetEnhancements(1)},
    /* High   */ {.viewDistance = 1600.0f, .shadowDistance = 350.0f, .antiAliasing = 2, .fxaa = false, .groundClutter = 100, .enh = presetEnhancements(2)},
    /* Ultra  */ {.viewDistance = 2400.0f, .shadowDistance = 500.0f, .antiAliasing = 3, .fxaa = false, .groundClutter = 150, .enh = presetEnhancements(3)},
    /* Classic */ {.viewDistance = kDefaultViewDistance, .shadowDistance = 350.0f, .antiAliasing = 2, .fxaa = false, .groundClutter = 100, .enh = rendering::Enhancements{}},
};

/// The number of presets, not counting Custom - which is not a set of values
/// but the name for "these are whatever you made them".
inline constexpr int kGraphicsPresetCount =
    static_cast<int>(sizeof(kGraphicsPresets) / sizeof(kGraphicsPresets[0]));

/// Classic's row. The ones before it are the Low-to-Ultra ladder, each asking
/// for more than the last; Classic stands beside it.
inline constexpr int kClassicPresetIndex = 4;
static_assert(kClassicPresetIndex == kGraphicsPresetCount - 1, "Classic is the last row");

}  // namespace ui
}  // namespace wowee
