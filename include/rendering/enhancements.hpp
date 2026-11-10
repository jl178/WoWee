#pragma once

/// Every way this client's picture, sound or timing departs from the stock
/// 3.3.5a client (build 12340), as one value.
///
/// A default-constructed Enhancements is the stock client: every switch off,
/// every multiplier at the value that leaves the stock result alone. That is
/// what the Classic preset is (kGraphicsPresets), and test_enhancements_stock
/// holds the two together. What each member does and costs, its label, its
/// stock and recommended values and the key it is saved under are in one table,
/// kEnhancementRows (ui/enhancement_rows.hpp), from which the settings rows,
/// the options pages, the saved file and the presets are all read.
///
/// The rule for every member (docs/enhancements-plan.md section 1): with it at
/// its stock value the client draws, sounds and times exactly as stock, and the
/// feature costs nothing - no pass runs, no resource is made, no per-frame work
/// happens for it. The Renderer is handed the whole value at once
/// (Renderer::setEnhancements) and passes each part to what it controls.
///
/// No includes: the presets table, the settings panel, the renderer and a test
/// all read it.

#include <cstdint>

namespace wowee {
namespace rendering {

struct Enhancements {
    // ---- Light and shadow ------------------------------------------------
    /// The sun's shadow map on terrain, buildings, doodads and units. Off is
    /// the stock default (extShadowQuality 0): the baked terrain shadows and
    /// the round ShadowBlob.blp under each unit (characters.md section 6).
    bool  shadows = false;
    /// A multiplier on Light.dbc's shadow darkness. 1 is stock.
    float shadowStrength = 1.0f;
    /// The shadow map's edge filter: 0 the stock 5-tap PCF, 1 Wowee's 9-tap
    /// rotated disk.
    int   shadowFilter = 0;
    /// Point lights from torches, braziers, lamps and lava on the ground,
    /// buildings and units (up to 64 around the camera). Off is stock: no point
    /// light on terrain or WMOs, and each unit and doodad lit by the four
    /// nearest M2 lights (characters.md section 1).
    bool  localLights = false;
    /// Those lights, and the lamp halos, gutter like flame. Needs localLights
    /// or glowSprites to show.
    bool  lightFlicker = false;
    /// A short ray toward the sun from each pixel near the camera finds the
    /// small things the shadow map is too coarse for - a boot on the road, a
    /// crate's foot, a mug on a table - and takes the sun's share of the
    /// light away where it is blocked (contact_shadows.hpp, E14). Only with
    /// `shadows`; off is the shadow map alone.
    bool  contactShadows = false;
    /// Corners, creases, the ground under a tree's roots and the foot of a
    /// wall lose some of the sky's light, as the reference client's ASSAO darkens them
    /// (ambient_occlusion.hpp, docs/re/enhanced-lighting.md section 4). Only
    /// the ambient share of each pixel's light is taken; sunlit faces keep
    /// the sun. Off is stock: no occlusion.
    bool  ambientOcclusion = false;
    /// Shade filled with light from the sky above and the sunlit ground
    /// below, a cheap stand-in for the reference client's probe GI (the ambient pass,
    /// ambient_occlusion.frag). Off is stock: the shade is Light.dbc's flat
    /// ambient colour.
    bool  fillLight = false;
    /// Units and doodads inside a building take the light of the room around
    /// them, read from a grid of probes baked from the group's MOCV and MODD
    /// colours at load: warm in a tavern, cold in a crypt, darker in a corner
    /// (indoor_probes.hpp, E12). Off is the stock light: every unit lit by the
    /// building's one colour, every doodad by its MODD colour.
    bool  indoorLight = false;
    /// The sun's highlight on `_s` tilesets (stock CVar `specular`, default 0).
    bool  terrainSpecular = false;
    /// Generated normal maps on buildings and the ground.
    bool  normalMapping = false;
    float normalMapStrength = 0.8f;
    /// Generated parallax depth from the same maps.
    bool  parallax = false;
    int   parallaxQuality = 1;
    /// Steep slopes take their layer textures from the side as well as from
    /// above, so a cliff shows the rock painted rather than the same texture
    /// pulled into streaks down the slope (terrain_cliff.frag, E16). Ground
    /// flatter than 45 degrees is drawn as stock either way. Off is the
    /// stock top-down mapping everywhere.
    bool  cliffProjection = false;
    /// The nearest characters (six on screen, within 22 yards) have their
    /// outline rounded by Phong tessellation of the skinned mesh, from the
    /// normals the art already has (character.tesc/.tese,
    /// CharacterRenderer::setCharacterFidelity). The same models, textures
    /// and light; off is the stock mesh.
    bool  characterFidelity = false;

    // ---- Sky and air -------------------------------------------------------
    /// Volumetric mist and light shafts: 0 off, 1-3 the volume's size.
    int   lightShafts = 0;
    float mistDensity = 1.0f;
    bool  sunShafts = false;
    float lensFlare = 0.0f;
    bool  sharpStars = false;
    /// Fog thickness and fog sky blend. 1 and 0 are Light.dbc's own fog.
    float fogStrength = 1.0f;
    float fogSkyBlend = 0.0f;
    /// The stock full-screen glow can push a bright surface past white, where
    /// stock clips it flat; this rolls those channels off on a soft shoulder
    /// above 0.8 instead, scaled to the glow's own reach (highlight_rolloff.hpp,
    /// E18). Nothing below 0.8 changes, and with no glow nothing changes.
    bool  highlightRollOff = false;
    /// the reference client's daytime colour grade on the two continents: its noon
    /// LUT's per-channel curve, weighted by the hour (enhanced_grade.hpp).
    /// Off is the stock picture, ungraded.
    bool  colorGrade = false;
    /// The day's light shifted the way the reference client re-authored 1.12's on the
    /// two continents: a whiter sun, brighter fill light, a brighter and
    /// bluer sky and haze (enhanced_palette.hpp). Off is Light.dbc's colours.
    bool  enhancedDaylight = false;
    /// the reference client's global fog layer over the whole view: air thickest
    /// near the ground, lit by the sky and, mostly forward, by the sun, so
    /// the distance pales and a low sun glows gold (enhanced_haze.frag).
    /// Off is stock: Light.dbc's distance fog alone.
    bool  sunHaze = false;
    /// Bloom over the highlights, 0 off (fxaa.frag bloomAt). Enhanced ships
    /// its Bloom Intensity at the maximum; stock has only the glow.
    float bloom = 0.0f;
    /// The distance fog ends well past the view distance, as the reference client's
    /// does (18000 yards over a 7000-yard far clip), so the land is not
    /// swallowed by a fog wall at the edge of what is drawn; the WDL horizon
    /// and the sun haze carry the distance. Off is stock: the fog ends at
    /// the far clip.
    bool  longView = false;
    /// Past the view distance the land goes on in its own colours to
    /// Enhanced's 7000-yard horizonClip (far_field.hpp): the WDL heights
    /// coloured by the minimap, lit and fogged as the ground, and the big
    /// outdoor parts of the loaded buildings out to 3000 yards. With the
    /// long view, the fog ends at 18/7 of that reach, as Enhanced's. Off is
    /// stock: the fog-coloured WDL silhouette.
    bool  farField = false;
    /// The scene drawn in 16-bit float (RGBA16F) instead of the screen's 8
    /// bits, as the reference client's linear lighting is, so light brighter than
    /// white is kept to the final composite instead of clipping there
    /// (Renderer::decideHdrScene, docs/re/enhanced-lighting.md section 9b).
    /// Needs the post-process pass and no upscaler. Off is the stock 8-bit
    /// scene.
    bool  hdrScene = false;
    /// Sunlight through the leaves: a tree's leaf cards glow in their own
    /// colour where the sun stands behind them, strongest looking into it,
    /// where the sun's shadow map says light reaches the leaf, and only by
    /// day (m2.frag; the reference client's material3_mesh_ps back light,
    /// docs/re/enhanced-lighting.md section 9c). Off is stock: a leaf's back
    /// is lit by the ambient alone.
    bool  leafLight = false;
    /// The shade takes the colour of what is around it, as the reference client's
    /// probe GI gives it: green and dimmer under a canopy, warm beside
    /// sunlit ground and walls, from a grid baked over the ground around the
    /// camera (bounce_grid.hpp, the ambient pass's BOUNCE_GRID build;
    /// docs/re/enhanced-lighting.md section 6b). It colours the fill light's
    /// sky and ground terms by place and runs the ambient pass even with the
    /// fill row off. Off is stock: no grid, the fill (if on) the same colour
    /// everywhere.
    bool  bounceLight = false;

    // ---- Water and weather -------------------------------------------------
    bool  shorelineFoam = false;
    bool  wetGround = false;
    /// Spray, wake froth and bubbles where the player swims or wades.
    bool  waterSplashes = false;
    /// Midges over reeds and still water.
    bool  waterInsects = false;
    /// The sky mirrored in water. Stock water reflects nothing.
    bool  waterReflections = false;
    /// Rings on lakes, rivers and puddled ground while it rains. Stock rain
    /// leaves the water as it was.
    bool  rainRipples = false;
    /// Rain and snow slant and drift with the wind that moves the trees - its
    /// direction, its eased strength and its rolling gusts (Weather::setWind).
    /// Stock rain and snow fall on their own fixed drift.
    bool  weatherWind = false;
    /// Soft moving light from the sun on the ground and anything else under
    /// shallow water, fading out with depth and at night (water_caustics.frag,
    /// E15). Stock water lights nothing beneath it.
    bool  caustics = false;
    /// HD water (water_hd.frag, E34): the stock animated texture and
    /// Light.dbc's close and far colours as the base, with moving surface
    /// normals derived from the stock frames, a Fresnel reflection of the
    /// stock sky, a sun glint, colour and clarity from the real
    /// depth under the surface, a soft shoreline and a gentle refraction.
    /// Off draws water.frag, the stock surface, and takes no colour copy.
    bool  hdWater = false;
    /// the reference client's PBR water (docs/re/enhanced-water.md): per liquid, its
    /// absorption and scattering colours from LiquidType 1.60.1, its
    /// reflection and sun highlight, foam where the bed meets the surface
    /// and down flowing rivers. Uses the HD water pipelines. Off is the rows
    /// above.
    bool  enhancedWater = false;

    // ---- Weather (the "Enhancements: Weather" page; weather_fx.hpp) -------
    /// How much rain, snow and sand to draw, as a multiple of the stock count:
    /// 1 is stock, 2.5 the most. The mist sheets and splashes follow on the
    /// square root.
    float weatherIntensity = 1.0f;
    /// Raindrops of every size, most fine, a few heavy on longer streaks;
    /// splashes on the move and further out (Weather::Style::variedRain).
    bool  variedRain = false;
    /// Flakes from a speck to a clump that turn as they fall and ride slow
    /// eddies (Weather::Style::softSnow).
    bool  softSnow = false;
    /// Distant sheets of rain, snow or dust in the fog colour, as the stock
    /// mist (SnowMist01, past half the weather's level) which Wowee did not
    /// draw (Weather::Style::mistSheets).
    bool  weatherSheets = false;
    /// Snow settles on the upward faces of the ground and outside walls over
    /// a few minutes of snowfall, and melts slowly after (wet_surface.glsli).
    bool  snowCover = false;
    /// Lightning lights the land in a thunderstorm and its thunder follows
    /// at the speed of sound.
    bool  stormLightning = false;
    /// The server's sandstorms (Weather.dbc EffectType 3) blow grains and
    /// dust and close the fog in. Stock Wowee draws nothing for them.
    bool  sandstorms = false;
    /// A breath of mist from players and creatures near the camera in the
    /// snowfields, on cold nights, and in snow.
    bool  coldBreath = false;
    /// Fireflies drift and glow in woods and meadows at night.
    bool  fireflies = false;
    /// The rain is heard faintly under a roof instead of not at all (the
    /// stock ambience drops the weather's loop indoors).
    bool  rainIndoors = false;
    /// A client-only weather to look at the rows with, in place of the
    /// server's (weather_fx::previewWeather): 0 off. Never sent.
    int   weatherPreview = 0;
    /// A client-only hour for the sky and the world's light, in place of
    /// the server's clock (kFixedTimeOfDayHours): 0 off. Never sent.
    int   fixedTimeOfDay = 0;

    // ---- Motion ------------------------------------------------------------
    /// How hard the wind moves trees and cloth. 0 is stock: nothing sways.
    float foliageWind = 0.0f;
    /// Grass, ferns and bushes part around the player and spring back.
    bool  foliagePush = false;
    /// Moored boats rise, fall and rock with the water.
    bool  boatBob = false;
    /// Small puffs of dust at the footfalls of players, creatures and mounts
    /// on dry dirt and sand, more the faster they go (FootfallDust). Stock
    /// throws up nothing. Saved under the old mount dust key it replaced.
    bool  footfallDust = false;
    /// A dead unit's body lies along the slope or floor under it instead of
    /// level with half of it in the air (CharacterRenderer::updateCorpseSettle).
    /// Stock draws the corpse level.
    bool  corpseSettle = false;
    /// Capes, robe and tabard flaps, tails and long hair swing a little with
    /// the unit's running, turning, jumping and the wind, on top of the
    /// animation (secondary_motion.hpp, CharacterRenderer). 0 is stock: every
    /// bone is the animation's; 1 capes and robes; 2 also tails and hair.
    int   secondaryMotion = 0;
    /// How far those swing, 0-2 (secondary_motion::strength): 1 the tuned
    /// swing, 0 none at all - stock, as secondaryMotion 0 is.
    float secondaryMotionStrength = 0.0f;
    /// Other units' position corrections and turns glide instead of stepping,
    /// and their switches between moving animations always fade
    /// (remote_smoothing.hpp). Stock puts them where each packet says and cuts
    /// where a sequence's blend time is 0.
    bool  remoteSmoothing = false;
    /// Standing and walking units plant their feet on slopes and steps: the
    /// knees bend, the body lowers a little, the feet tilt (foot_ik.hpp).
    /// Stock poses the legs on a flat plane through the unit's position.
    bool  footPlacement = false;
    /// The mouse wheel's zoom eases to its new distance instead of travelling
    /// at the stock constant 8.33 yd/s (camera_zoom.hpp).
    bool  smoothZoom = false;
    /// Extra smoke puffs over models named for smoke.
    bool  extraSmoke = false;
    /// Wowee's own drunk view: a wavy, blurred picture and a swaying camera.
    /// Off is the stock effect, the full-screen glow blended by the drunk level.
    bool  drunkWobble = false;

    // ---- Edges -------------------------------------------------------------
    /// Leaf and grass cutouts smoothed by alpha-to-coverage under MSAA. Off is
    /// the stock alpha test at 224/255.
    bool  smoothLeaves = false;
    /// Lamp and torch glow cards replaced by soft billboard halos. Off draws
    /// the cards the models carry, as stock does.
    bool  glowSprites = false;
    /// Blended particles (and the soft lamp halos) fade where they meet the
    /// ground or a wall instead of cutting a hard line. Off is the stock
    /// depth test in the scene pass.
    bool  softParticles = false;

    // ---- Distance ----------------------------------------------------------
    /// Terrain, buildings and doodads that finish streaming in dissolve in
    /// over a moment instead of appearing in one frame, as stock does.
    bool  streamFadeIn = false;
    /// The far horizon (the WDL's mountains and its low-detail buildings) takes
    /// the sun's light under the haze instead of being one flat fog colour
    /// (TerrainHorizon). Stock draws it in the fog colour alone.
    bool  horizonDetail = false;

    // ---- Experimental ------------------------------------------------------
    bool  grass = false;
    int   grassDensity = 100;
    int   grassHeight = 100;
    int   grassDistance = 150;
    /// Ray traced lighting: 0 off, 1 sun, 2 + occlusion, 3 + bounce.
    int   rtLighting = 0;

    // ---- Interface and sound ----------------------------------------------
    /// The pulsing red vignette under 20% health. Stock has only its
    /// screenEdgeFlash.
    bool  lowHealthVignette = false;
    /// Wowee's own soundtrack in the zone music rotation.
    bool  woweeMusic = false;
    /// The interface's text from glyphs rasterised at the size they are drawn,
    /// one sample per pixel on whole pixels (no 32 pixel cap), and its art
    /// sampled through a mip chain (N14, WidgetRenderer::setCrispInterface).
    /// Off is the stock bake: 2x horizontal samples, at most 32 pixels and
    /// stretched above, single-level images.
    bool  crispInterface = false;
    /// On a screen wider than 16:9 the stock interface's frames pinned to the
    /// screen's left and right edges are pinned to a centred 16:9 area's
    /// instead (N15, WidgetTree::setHudAspect); addons' frames and the world
    /// keep the whole screen. Off is the stock layout across the whole width.
    bool  ultrawideHud = false;

    // ---- Combat feel (the reference client, docs/re/enhanced-combat.md) --------------
    /// A new animation that starts while a cross-fade is still running fades
    /// from the pose as drawn (a snapshot) over its blendTime, so a swing cut
    /// short by a flinch, a run or another swing never pops. Off is the stock
    /// 0x826C40: the fade keeps its outgoing sequence and swaps the incoming
    /// one at once, or drops the outgoing one.
    bool  poseBlend = false;
    /// The spell queue window in ms (Enhanced's SpellQueueWindow, 400): a press
    /// that only the GCD, a cooldown or the cast's end holds within that time
    /// is sent the moment it can be (SpellHandler). 0 is stock 3.3.5a: no
    /// queue, the press refused (or the CVar's own window).
    int   spellQueueWindow = 0;
    /// Enhanced's swing timer (Blizzard_SwingTimer, Camelot only): a bar per
    /// weapon hand over the action bars, filled from each of the player's
    /// own swings to the next and dimmed out of melee range. Off: none.
    bool  swingTimer = false;

    bool operator==(const Enhancements&) const = default;
};

/// The bits of Enhancements the world shaders read, packed into
/// GPUPerFrameData::localLightMeta.z. Only switches a shader branches on are
/// here; the rest are applied on the CPU.
namespace enh_bits {
inline constexpr uint32_t kFoliagePush    = 1u << 0;  ///< m2.vert player brush
inline constexpr uint32_t kBoatBob        = 1u << 1;  ///< m2.vert boat motion
inline constexpr uint32_t kLocalLights    = 1u << 2;  ///< point lights on terrain/WMO; all lights on units
inline constexpr uint32_t kSoftShadows    = 1u << 3;  ///< 9-tap disk instead of the stock 5-tap PCF
inline constexpr uint32_t kSmoothLeaves   = 1u << 4;  ///< alpha-to-coverage ramp instead of the 224/255 key
inline constexpr uint32_t kHdrLight      = 1u << 5;  ///< HDR scene: lit shaders' clamp at 1 lifted (set by the renderer)
inline constexpr uint32_t kLeafLight     = 1u << 6;  ///< m2.frag sunlight through foliage cards
/// Enhanced's daylight in use (LightingManager::isEnhancedActive, set by the
/// renderer): M2s, WMOs and units take Enhanced's ambient, turned toward the
/// sun (enhanced_palette::kAmbientAway..kAmbientToward by half-Lambert), and
/// the terrain's shadow floor follows shadowParams.y below stock's 0.7.
inline constexpr uint32_t kEnhancedAmbient = 1u << 7;
}  // namespace enh_bits

inline uint32_t shaderBits(const Enhancements& e) {
    uint32_t b = 0;
    if (e.foliagePush) b |= enh_bits::kFoliagePush;
    if (e.boatBob) b |= enh_bits::kBoatBob;
    if (e.localLights) b |= enh_bits::kLocalLights;
    if (e.shadowFilter != 0) b |= enh_bits::kSoftShadows;
    if (e.smoothLeaves) b |= enh_bits::kSmoothLeaves;
    if (e.leafLight) b |= enh_bits::kLeafLight;
    return b;
}

}  // namespace rendering
}  // namespace wowee
