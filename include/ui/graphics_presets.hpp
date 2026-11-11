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
/// Classic, Enhanced. Classic and Enhanced come last because they were added
/// last, not because of what they ask: both are off the Low-to-Ultra ladder
/// (see kClassicPresetIndex).
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
/// Normal mapping and parallax are off on every row, as in the recommended
/// set: the 3.3.5 art ships no such maps - they are generated from the colour
/// texture - and Jered chose the painted look (docs/enhancements-plan.md
/// section 6, Nov 2026). Either is still a switch on the Enhancements page;
/// the presets keep a bump strength and step count for when one is turned on.
/// Characters never use them, because generated maps embossed the composited
/// skins. Low also drops its mist to the light volume, and the shoreline foam
/// and wet ground that cost a little on every water and ground pixel. The
/// grass columns are the generator's own experiment and off on every row;
/// their sliders stay at the values the presets always gave them.
constexpr rendering::Enhancements presetEnhancements(int level /* 0 Low .. 3 Ultra */) {
    rendering::Enhancements e = recommendedEnhancements();
    // The sun's shadow map is on every step of the ladder: picking a preset
    // is asking for its quality columns, shadow distance included. The
    // recommended set has it on as well (the shadows row in kEnhancementRows).
    e.shadows = true;
    e.grassDensity = 70;
    e.grassHeight = 50;
    e.grassDistance = 215;
    constexpr float kBump[] = {0.6f, 0.6f, 0.8f, 1.2f};
    e.normalMapStrength = kBump[level];
    e.parallaxQuality = level <= 1 ? 0 : level - 1;
    if (level == 0) {
        e.lightShafts = 1;
        e.shorelineFoam = false;
        e.wetGround = false;
    }
    return e;
}

/// The Enhanced look: Ultra plus what the reference client (1.60.1) draws that the
/// ladder leaves out, at Enhanced's own values where they are known
/// (docs/re/enhanced-lighting.md section 8). Its ambient occlusion (ASSAO,
/// SSAOSettings 116), its noon colour grade and its daylight are rows of
/// their own; its "Compute Effects 3" volumetric fog is the light volume
/// at the stock mist; its
/// "Depth Based Opacity" is the soft particles; its PBR water and screen-space
/// reflections are the HD water and the sky in it.
constexpr rendering::Enhancements enhancedEnhancements() {
    rendering::Enhancements e = presetEnhancements(3);
    // Ambient occlusion stays off: the screen-space version darkens rims
    // around everything (Jered, 2026-11-09); on again only once it is subtle
    // and clean against Enhanced's captures.
    e.colorGrade = true;
    e.enhancedDaylight = true;
    e.fillLight = true;
    e.sunHaze = true;
    e.bloom = 0.6f;
    e.longView = true;
    // Enhanced's distance: full detail to its 1900-yard horizonStart (the
    // view distance below), the far field to its 7000-yard horizonClip.
    e.farField = true;
    // Enhanced's linear HDR light (its `linear` CVar, RGBA16F): the scene in
    // 16-bit float, tone mapped, the sun 1.25x by day and the lit shaders'
    // clamp at 1 lifted, so sunlit faces keep their hue and highlights glow
    // instead of clipping (enhanced_tonemap.hpp; ~+1 ms GPU at 1280x540).
    e.hdrScene = true;
    // Sunlight through the leaves toward a low sun (m2.frag; Enhanced's mesh
    // shader back light with a forward lobe), in the shadow already read.
    e.leafLight = true;
    // Enhanced's probe GI, by place (bounce_grid.hpp): the shade under the
    // trees green and dimmer, beside sunlit ground and walls warm.
    e.bounceLight = true;
    // The shade by the sky each point sees (sky_height_field.hpp): Enhanced's
    // terrain light, the ambient scaled by the open sky and a fill in open
    // shade (docs/re/enhanced-lighting.md 6e).
    e.skyShade = true;
    // Enhanced's cascaded sun shadows (shadowNumCascades 3, 2048): crisp
    // near the player, leaves cut out, clutter shaded (and casting near).
    e.enhancedShadows = true;
    // Enhanced's ground is covered where 3.3.5's shows bare texture between
    // tufts. Its clutter data is 1.12's; the cover comes from its settings
    // (groundEffectDist 200) and models. Wowee's grass field stands in for
    // it, out to the same 200 yards, at the density and height the terrain
    // asks for. Its slices are timed, the blades glow against a low sun
    // (grass.frag), and walking Goldshire, Elwynn and the Barrens stays
    // under 20 ms a frame (docs/re/enhanced-lighting.md section 7).
    e.grass = true;
    e.grassDensity = 100;
    e.grassHeight = 100;
    e.grassDistance = 200;
    // Medium's light volume at the stock mist: twice the mist, chosen side
    // by side in an afternoon Goldshire, was a grey wall in Jered's evening
    // under cloud; the sun haze carries the distance. High's volume costs
    // 2.5 ms more GPU (1280x540, Jered's laptop) for little visible change.
    e.lightShafts = 2;
    e.mistDensity = 1.0f;
    e.softParticles = true;
    e.hdWater = true;
    e.enhancedWater = true;
    e.waterReflections = true;
    // And how it plays (docs/re/enhanced-combat.md, kEnhancedCombatValues):
    // fades from the drawn pose, smoothed other units, the 400 ms spell
    // queue and the swing timer.
    e.poseBlend = true;
    e.remoteSmoothing = true;
    e.spellQueueWindow = 400;
    e.swingTimer = true;
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
// that default rather than the stock clamp of 791.7 yards below map 530
// (Jered, Nov 2026, enhancements-plan.md section 6); the View distance slider
// still moves freely from 400 to 2400 under it, and the fog follows the far
// clip down as stock's does (stock_light::frameFog).
constexpr GraphicsPresetValues kGraphicsPresets[] = {
    /* Low    */ {.viewDistance = 600.0f,  .shadowDistance = 100.0f, .antiAliasing = 0, .fxaa = false, .groundClutter = 25,  .enh = presetEnhancements(0)},
    /* Medium */ {.viewDistance = 1000.0f, .shadowDistance = 200.0f, .antiAliasing = 1, .fxaa = false, .groundClutter = 60,  .enh = presetEnhancements(1)},
    /* High   */ {.viewDistance = 1600.0f, .shadowDistance = 350.0f, .antiAliasing = 2, .fxaa = false, .groundClutter = 100, .enh = presetEnhancements(2)},
    /* Ultra  */ {.viewDistance = 2400.0f, .shadowDistance = 500.0f, .antiAliasing = 3, .fxaa = false, .groundClutter = 150, .enh = presetEnhancements(3)},
    /* Classic */ {.viewDistance = kDefaultViewDistance, .shadowDistance = 350.0f, .antiAliasing = 2, .fxaa = false, .groundClutter = 100, .enh = rendering::Enhancements{}},
    // Enhanced: Ultra's shadows and clutter, Enhanced's 1900-yard detailed
    // range (horizonStart; the far field carries on), and FXAA in place of
    // multisampling, as Enhanced's own edge filter (CMAA2) is a post pass -
    // which also leaves a one-sample depth for the occlusion pass to read.
    /* Enhanced */ {.viewDistance = 1900.0f, .shadowDistance = 500.0f, .antiAliasing = 0, .fxaa = true, .groundClutter = 150, .enh = enhancedEnhancements()},
};

/// The number of presets, not counting Custom - which is not a set of values
/// but the name for "these are whatever you made them".
inline constexpr int kGraphicsPresetCount =
    static_cast<int>(sizeof(kGraphicsPresets) / sizeof(kGraphicsPresets[0]));

/// Classic's row. The ones before it are the Low-to-Ultra ladder, each asking
/// for more than the last; Classic stands beside it, and Enhanced after it.
inline constexpr int kClassicPresetIndex = 4;
/// The Enhanced look's row: the whole set of Enhanced-matched enhancements.
inline constexpr int kEnhancedPresetIndex = 5;
static_assert(kEnhancedPresetIndex == kGraphicsPresetCount - 1, "Enhanced is the last row");

}  // namespace ui
}  // namespace wowee
