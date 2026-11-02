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
/// It carries no includes, so the login screen, the settings panel and a test
/// can all read the same rows.

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
    bool  shadows;
    float shadowDistance;
    int   antiAliasing;      ///< index into the four the panel offers
    bool  fxaa;
    bool  normalMapping;
    float normalMapStrength;
    bool  parallax;
    int   parallaxQuality;
    int   groundClutter;     ///< percent
    /// Ground cover from the terrain's own ground-effect data. Stated on every
    /// row rather than only the one that wants it: a preset that leaves a field
    /// alone is what left FXAA running after a drop from Ultra to Low.
    bool  grass;
    int   grassDensity;      ///< percent of what the terrain asks for
    int   grassHeight;       ///< percent
    int   grassDistance;     ///< yards
    // The enhancements over the stock picture, stated on every row for the
    // same reason as the grass: picking Ultra after Classic has to bring them
    // back, and a row that left them alone would not.
    float foliageWind;       ///< foliagewind: 1 the usual breeze, 0 still
    float shadowStrength;    ///< shadowstrength: 1 is Light.dbc's own darkness
    int   lightShafts;       ///< lightshafts: index of Off, Low, Medium, High
    float fogStrength;       ///< fogstrength: 1 is Light.dbc's fog exactly
    float fogSkyBlend;       ///< fogskyblend: 0 is the zone's own fog colour
    float lensFlare;         ///< lensflare: 0 is none
    bool  sharpStars;        ///< off is the sky model's own star texture
    bool  sunShafts;
    bool  shorelineFoam;     ///< shorelinefoam: off is the stock water edge
    bool  wetGround;         ///< wetground: off is the stock dry ground in rain
};

// Normal mapping and parallax are on from Medium up, by choice: Jered
// prefers the lit relief on the ground and walls. The 3.3.5 art ships no
// such maps (they are generated from the colour texture), so this is a
// deliberate departure; characters never use them (character_renderer),
// because generated maps embossed the composited skins.
// Note the shadows column: every preset leaves them on. Turning them off
// saves no time - the shadow pass runs either way - so Low leans on its short
// shadow distance instead, which is where the cost actually is.
// And the FXAA column: off on every row. Ultra used to turn it on over 8x
// MSAA, where there is nothing left for it to find and its sub-pixel filter
// only softened the resolve. It stays in the panel for anyone who wants it,
// which mostly means anyone running without MSAA.
// And the grass column: off on every row. The GPU grass blades are not in the
// stock client - its ground cover is the detail doodads the terrain's
// GroundEffectTexture rows place, which the clutter column already draws - and
// on Ultra they were a second field of a hundred thousand blades over the
// first, rebuilt every ten to fifteen seconds of travel with a hitch of 30 ms
// and up each time (the frame profile's beginFrame/grass+misc). It stays in
// the panel for anyone who wants it.
// The enhancement columns on Low to Ultra are each setting's own default, bar
// Low's lighter mist and its plain shorelines and dry rain: those two cost a
// little on every water and ground pixel, which is what Low is for saving.
// The fog columns are the stock fog on every row (strength 1, no sky blend):
// with the stock curve (LightingManager, stock_light_math.hpp) the fog is
// Light.dbc's own, ending by the view distance, and thinning or recolouring
// it changes the picture rather than adding to it. The sliders remain.
// Classic sets every one of them to the stock picture: still trees and cloth,
// Light.dbc's shadow darkness, fog and fog colour exactly, no mist, flare or
// sun shafts, the sky's own stars, no shoreline foam or wet ground, and no
// normal maps or parallax (the ground's relief follows normal mapping, so it
// is flat too). Shadows stay on, as on every row; the rest of its quality columns are
// High's, at the default view distance, where the fog is Light.dbc's own
// (LightingManager::kFogReferenceViewDistance).
constexpr GraphicsPresetValues kGraphicsPresets[] = {
    /* Low    */ { .viewDistance = 600.0f, .shadows = true,  .shadowDistance = 100.0f, .antiAliasing = 0, .fxaa = false, .normalMapping = false, .normalMapStrength = 0.6f, .parallax = false, .parallaxQuality = 0,  .groundClutter = 25,  .grass = false, .grassDensity = 70, .grassHeight = 50, .grassDistance = 215,
                   .foliageWind = 1.0f, .shadowStrength = 1.0f, .lightShafts = 1, .fogStrength = 1.0f, .fogSkyBlend = 0.0f, .lensFlare = 1.0f, .sharpStars = true, .sunShafts = true, .shorelineFoam = false, .wetGround = false},
    /* Medium */ {.viewDistance = 1000.0f, .shadows = true,  .shadowDistance = 200.0f, .antiAliasing = 1, .fxaa = false, .normalMapping = true, .normalMapStrength = 0.6f, .parallax = true, .parallaxQuality = 0,  .groundClutter = 60,  .grass = false, .grassDensity = 70, .grassHeight = 50, .grassDistance = 215,
                   .foliageWind = 1.0f, .shadowStrength = 1.0f, .lightShafts = 2, .fogStrength = 1.0f, .fogSkyBlend = 0.0f, .lensFlare = 1.0f, .sharpStars = true, .sunShafts = true, .shorelineFoam = true, .wetGround = true},
    /* High   */ {.viewDistance = 1600.0f, .shadows = true,  .shadowDistance = 350.0f, .antiAliasing = 2, .fxaa = false, .normalMapping = true, .normalMapStrength = 0.8f, .parallax = true, .parallaxQuality = 1, .groundClutter = 100, .grass = false, .grassDensity = 70, .grassHeight = 50, .grassDistance = 215,
                   .foliageWind = 1.0f, .shadowStrength = 1.0f, .lightShafts = 2, .fogStrength = 1.0f, .fogSkyBlend = 0.0f, .lensFlare = 1.0f, .sharpStars = true, .sunShafts = true, .shorelineFoam = true, .wetGround = true},
    /* Ultra  */ {.viewDistance = 2400.0f, .shadows = true,  .shadowDistance = 500.0f, .antiAliasing = 3, .fxaa = false, .normalMapping = true, .normalMapStrength = 1.2f, .parallax = true, .parallaxQuality = 2, .groundClutter = 150, .grass = false, .grassDensity = 70, .grassHeight = 50, .grassDistance = 215,
                   .foliageWind = 1.0f, .shadowStrength = 1.0f, .lightShafts = 2, .fogStrength = 1.0f, .fogSkyBlend = 0.0f, .lensFlare = 1.0f, .sharpStars = true, .sunShafts = true, .shorelineFoam = true, .wetGround = true},
    /* Classic */ {.viewDistance = kDefaultViewDistance, .shadows = true, .shadowDistance = 350.0f, .antiAliasing = 2, .fxaa = false, .normalMapping = false, .normalMapStrength = 0.8f, .parallax = false, .parallaxQuality = 1, .groundClutter = 100, .grass = false, .grassDensity = 70, .grassHeight = 50, .grassDistance = 215,
                   .foliageWind = 0.0f, .shadowStrength = 1.0f, .lightShafts = 0, .fogStrength = 1.0f, .fogSkyBlend = 0.0f, .lensFlare = 0.0f, .sharpStars = false, .sunShafts = false, .shorelineFoam = false, .wetGround = false},
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
