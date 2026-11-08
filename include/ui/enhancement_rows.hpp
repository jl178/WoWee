#pragma once

/// The one table of enhancements: every way the client departs from the stock
/// 3.3.5a picture, sound or timing, with what each is called, what it costs,
/// what stock does instead and the key it is saved under.
///
/// Adding a boolean used to touch nine places - a schema row, a pending field,
/// two key lists, the preset assignment and its match, the binding table, the
/// side-effect switch, three lists on the login screen's options, the save and
/// the load, and a renderer setter - and the Classic preset lost four keys that
/// way. Now it is a member of rendering::Enhancements and a row here. From the
/// row come the settings schema rows (clientSettingsSchema), the values the
/// options pages read and write (SettingsPanel::settingValue), the config file
/// lines (GameScreen::saveSettings / loadSettings), and what a preset sets;
/// Renderer::setEnhancements is the one place the value reaches the renderer.
///
/// Rules (test_enhancements_stock enforces them):
///   - stockValue is the member's default in Enhancements{}; Enhancements{}
///     is the Classic preset.
///   - every member has exactly one row (a sizeof guard catches a new member
///     with none).
///   - saveKey is never renamed or reused: a saved file keeps its meaning.

#include "rendering/enhancements.hpp"
#include "ui/settings_schema.hpp"

#include <cstddef>
#include <string>

namespace wowee {
namespace ui {

struct EnhancementRow {
    const char* key;          ///< the setting key (schema, Lua, options pages)
    const char* label;
    SettingKind kind;
    float minValue;
    float maxValue;
    float step;
    const char* category;     ///< the options page
    const char* section;      ///< heading; "" continues the last
    const char* tooltip;
    const char* choices;      ///< Enum only
    float stockValue;         ///< what the stock client is; the member default
    float recommendedValue;   ///< a fresh install's value (the schema default)
    const char* saveKey;      ///< the settings.cfg key
    /// What turning it off removes: the pass, resource or per-frame work it
    /// controls. Documentation for the reader of this table and the report.
    const char* controls;
    const char* enabledWhen = "";
    /// Where the generated row goes in the schema: after the row with this
    /// key. "" keeps it in the block of enhancement pages.
    const char* after = "";
    const char* restartNote = "";
    bool  rendering::Enhancements::* asBool  = nullptr;
    int   rendering::Enhancements::* asInt   = nullptr;
    float rendering::Enhancements::* asFloat = nullptr;
};

using rendering::Enhancements;

// The pages these land on. Three, because a page is two columns of about 366
// units (test_settings_panel_layout) and the rows need about 1,700.
inline constexpr const char* kEnhPage      = "Enhancements";
inline constexpr const char* kEnhPageSky   = "Enhancements: Sky";
inline constexpr const char* kEnhPageWater = "Enhancements: Motion";
inline constexpr const char* kEnhPageWeather = "Enhancements: Weather";
inline constexpr const char* kEnhPageEnhanced = "Enhancements: Enhanced";

inline constexpr EnhancementRow kEnhancementRows[] = {
    // ------------------------------------------------------- Light and shadow
    {.key = "shadows", .label = "Sun shadows", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPage, .section = "Light and shadow",
     .tooltip = "Shadows from the sun and moon on the ground, buildings and\n"
                "everyone in the world. Off is the original game's default: a\n"
                "soft round shadow under each character and the ground's own\n"
                "painted shadows.",
     // On in the recommended set again since the map was split in two
     // (rendering/shadow_cache.hpp): the world's casters are drawn into a
     // kept map only when the player has walked a few dozen yards, the sun
     // has turned or the world has changed, and only units and what moves
     // are drawn every frame, into a small map read near them. It was off
     // for its cost - 2.8-4.3 ms of GPU and 0.7-1.5 ms of main-thread
     // recording at the Ultra distance on the Iris Xe, alone over the whole
     // set's budget of 3 ms and 0.5 ms (docs/enhancements-plan.md, 3.5 and
     // 3.9 for what it costs now).
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "shadows",
     .controls = "the shadow map's casters and sampling; off draws the stock unit blob",
     .asBool = &Enhancements::shadows},
    {.key = "shadowstrength", .label = "Shadow darkness", .kind = SettingKind::Float,
     .minValue = 0.5f, .maxValue = 1.5f, .step = 0.05f, .category = kEnhPage, .section = "",
     .tooltip = "How dark shadows are, against what the zone's light asks for.\n"
                "1 is as the sky sets it: deeper at noon, faint under cloud.",
     .choices = "", .stockValue = 1, .recommendedValue = 1, .saveKey = "shadow_strength",
     .controls = "shadowParams.y multiplier", .enabledWhen = "shadows",
     .asFloat = &Enhancements::shadowStrength},
    {.key = "shadowfilter", .label = "Shadow edges", .kind = SettingKind::Enum,
     .minValue = 0, .maxValue = 1, .step = 1, .category = kEnhPage, .section = "",
     .tooltip = "How shadow edges are smoothed. Original is the game's own\n"
                "five-sample filter; Soft takes five on a turning disc.",
     .choices = "Original|Soft", .stockValue = 0, .recommendedValue = 1,
     .saveKey = "shadow_filter",
     .controls = "shadowFilter (shadow_filter.glsli): 5-tap rotated Vogel disk vs the stock 5-tap PCF",
     .enabledWhen = "shadows", .asInt = &Enhancements::shadowFilter},
    {.key = "locallights", .label = "Torch and lamp light", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPage, .section = "",
     .tooltip = "Torches, braziers, lamps and lava throw pools of light on the\n"
                "ground and walls around them. Off is the original: their light\n"
                "falls only on characters and objects near them.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "local_lights",
     .controls = "the 64-light gather (M2 and WMO lava) and its use in the terrain, "
                 "WMO, grass and fog shaders; units then take every gathered light "
                 "instead of the stock four nearest M2 lights",
     .asBool = &Enhancements::localLights},
    {.key = "lightflicker", .label = "Flickering flames", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPage, .section = "",
     .tooltip = "That light, and the glow of flames, gutters and breathes.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "light_flicker",
     .controls = "lampFlicker on gathered lights, glow sprites and additive fire batches",
     .asBool = &Enhancements::lightFlicker},
    {.key = "contactshadows", .label = "Contact shadows", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPage, .section = "",
     .tooltip = "A crisp shadow where small things touch the ground in\n"
                "sunlight - feet, crates, a mug on a table - which the sun's\n"
                "shadows are too coarse to show. Near the camera only.",
     // Off in the recommended set (docs/enhancements-plan.md 3.15): at a
     // 2560x1080 window the pass is 0.8 ms of GPU even with its march shared
     // across each 2x2 quad, over its 0.5 ms, for a line of shade at feet
     // and crates that Jered did not find worth it. It stays as an option.
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "contact_shadows",
     .controls = "ContactShadowPass (built on first use): one full-screen pass after the "
                 "scene closes, a short ray per pixel within 24 yd, dst * src",
     .enabledWhen = "shadows", .asBool = &Enhancements::contactShadows},
    {.key = "ambientocclusion", .label = "Ambient occlusion", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPage, .section = "",
     .tooltip = "Corners, creases, the ground between roots and the foot of a\n"
                "wall lose some of the sky's light, so things sit in the world\n"
                "instead of floating on it. Sunlit faces keep the sun. Tuned to\n"
                "the reference client's settings (screen-space ambient occlusion).",
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "ambient_occlusion",
     .controls = "the ambient pass (ambient_occlusion.frag, built on first use): one "
                 "full-screen pass after the scene closes, six depth taps a pixel within "
                 "1 yd, 2 * dst * src",
     .asBool = &Enhancements::ambientOcclusion},
    {.key = "filllight", .label = "Fill light in shade", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPage, .section = "",
     .tooltip = "Shade takes light from the sky above and from the sunlit\n"
                "ground below, so a wall in shadow or the underside of a tree\n"
                "reads lit rather than painted, as in the reference client.",
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "fill_light",
     .controls = "the ambient pass (ambient_occlusion.frag, built on first use): sky and "
                 "ground bounce on the ambient share, 2 * dst * src",
     .asBool = &Enhancements::fillLight},
    {.key = "indoorlight", .label = "Room light indoors", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPage, .section = "",
     .tooltip = "Characters and objects inside buildings take the colour of\n"
                "the room around them: warm by an inn's lamps, cold in a\n"
                "crypt. Off, the original lights everyone in a building alike.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "indoor_light",
     .controls = "WMO probe grids baked at group load, a probe read per unit every half "
                 "second or third of a yard, the room colour in character.frag and the "
                 "doodads' two MODD colours",
     .restartNote = "Turning this on reaches buildings loaded from now on;\nrestart to see it everywhere.",
     .asBool = &Enhancements::indoorLight},
    {.key = "terrainspecular", .label = "Ground highlights", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPage, .section = "",
     .tooltip = "The sun glints off polished ground textures - flagstones,\n"
                "ice, wet rock. The original game's Terrain Highlights option,\n"
                "off by default there.",
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "terrain_specular",
     .controls = "the per-vertex sun highlight on `_s` tilesets (terrain.vert)",
     .asBool = &Enhancements::terrainSpecular},
    {.key = "normalmapping", .label = "Surface bumps (normal mapping)", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPage, .section = "Surfaces",
     .tooltip = "Light stone, wood and metal by their surface texture,\n"
                "so they catch the light like the real material, not flat paint.",
     // Off in the recommended set (Jered, Nov 2026, enhancements-plan.md
     // section 6): the 3.3.5 art ships no normal maps, these are generated
     // from the colour texture, and he prefers the painted look. Parallax,
     // drawn from the same generated maps, is off too (below).
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "normal_mapping",
     .controls = "normal map generation at texture load and the bump branch in "
                 "wmo/terrain .frag",
     .restartNote = "Turning this on reaches buildings loaded from now on;\nrestart to see it everywhere.",
     .asBool = &Enhancements::normalMapping},
    {.key = "normalmapstrength", .label = "Bump strength", .kind = SettingKind::Float,
     .minValue = 0, .maxValue = 2, .step = 0.1f, .category = kEnhPage, .section = "",
     .tooltip = "How pronounced those surface bumps look. 1 is as the textures\n"
                "were made; higher exaggerates them.",
     .choices = "", .stockValue = 0.8f, .recommendedValue = 0.8f, .saveKey = "normal_map_strength",
     .controls = "the bump strength uniform", .enabledWhen = "normalmapping",
     .asFloat = &Enhancements::normalMapStrength},
    {.key = "parallax", .label = "Surface depth (parallax)", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPage, .section = "",
     .tooltip = "Gives bricks, cobbles and planks real depth when seen at an\n"
                "angle, so mortar lines sink and stones stand out.",
     // Off in the recommended set (enhancements-plan.md 3.11): about a
     // millisecond of the scene pass in the Trade District and Goldshire even
     // eased out past 25-45 yards, the same at every step count, for relief a
     // few centimetres deep - the whole set's 3 ms budget had no room for it.
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "pom",
     .controls = "the parallax march in wmo.frag and character.frag, eased out between 25 and 45 yards",
     .restartNote = "Turning this on reaches buildings loaded from now on;\nrestart to see it everywhere.",
     .asBool = &Enhancements::parallax},
    {.key = "parallaxquality", .label = "Surface depth quality", .kind = SettingKind::Enum,
     .minValue = 0, .maxValue = 2, .step = 1, .category = kEnhPage, .section = "",
     .tooltip = "How finely each surface is traced for that depth: 16, 32 or 64\n"
                "steps. Higher looks steadier up close and costs more on big walls.",
     .choices = "Low|Medium|High", .stockValue = 1, .recommendedValue = 1, .saveKey = "pom_quality",
     .controls = "parallax step count", .enabledWhen = "parallax",
     .asInt = &Enhancements::parallaxQuality},
    {.key = "cliffprojection", .label = "Unstretched cliffs", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPage, .section = "",
     .tooltip = "Steep slopes and cliffs show their rock and earth textures at\n"
                "their painted size instead of pulled into long streaks down\n"
                "the slope. Gentler ground is drawn exactly as the original.",
     // E16, measured in docs/enhancements-plan.md (3.13). Off in the
     // recommended set: 0.1-0.2 ms on flat land and coasts, but 0.35-0.75 in
     // views full of hills and mountains (Goldshire's road, the Storm Peaks),
     // over the half millisecond each new row was allowed.
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "cliff_projection",
     .controls = "the terrain_cliff.frag pipelines (built on first use): on pixels "
                 "steeper than 45 degrees, in the chunks that have such slopes, each layer is also read from the "
                 "side and blended by the slope; off binds the stock terrain.frag",
     .asBool = &Enhancements::cliffProjection},
    {.key = "characterfidelity", .label = "Smoother characters", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPage, .section = "",
     .tooltip = "The characters nearest you have rounder outlines - shoulders,\n"
                "arms, legs - where the original shows flat facets. The same\n"
                "models, textures and light as the original.",
     // Off in the recommended set (docs/enhancements-plan.md 3.19): about
     // 0.9 ms of GPU in a crowd for a change that shows only on the outline
     // of the nearest few units.
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "character_fidelity",
     .controls = "the tessellated character pipelines and per-model weld-normal buffers "
                 "(made on first use), for the six nearest units on screen within 22 yd",
     .asBool = &Enhancements::characterFidelity},

    // ------------------------------------------------------------ Sky and air
    {.key = "lightshafts", .label = "Light shafts and mist", .kind = SettingKind::Enum,
     .minValue = 0, .maxValue = 3, .step = 1, .category = kEnhPageSky, .section = "Sky and air",
     .tooltip = "Mist that light moves through: the sun casts shafts past trees\n"
                "and buildings, and torches and lava glow in it. Higher settings\n"
                "are sharper and cost more. The original game has no such mist.",
     .choices = "Off|Low|Medium|High", .stockValue = 0, .recommendedValue = 2, .saveKey = "light_shafts",
     .controls = "the volumetric fog volume (images) and its compute passes",
     .asInt = &Enhancements::lightShafts},
    {.key = "mistdensity", .label = "Mist density", .kind = SettingKind::Float,
     .minValue = 0, .maxValue = 3, .step = 0.1f, .category = kEnhPageSky, .section = "",
     .tooltip = "How thick that mist is. 1 is a light haze; above it the air\n"
                "closes in. Rain, snow, dawn and foggy zones thicken it further.",
     .choices = "", .stockValue = 1, .recommendedValue = 1, .saveKey = "mist_density",
     .controls = "the fog volume's density", .enabledWhen = "lightshafts!=0",
     .asFloat = &Enhancements::mistDensity},
    {.key = "sunshafts", .label = "Sun shafts", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageSky, .section = "",
     .tooltip = "Rays of light streaming from the sun through gaps in trees and\n"
                "between buildings when you look toward it.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "sun_shafts",
     .controls = "the screen-space shaft compute pass and composite",
     .asBool = &Enhancements::sunShafts},
    {.key = "lensflare", .label = "Lens flare", .kind = SettingKind::Float,
     .minValue = 0, .maxValue = 2, .step = 0.1f, .category = kEnhPageSky, .section = "",
     .tooltip = "How strong the sun's flare is when it is in view. It warms to\n"
                "amber near the horizon. 0 removes it, as in the original.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "lens_flare",
     .controls = "the lens flare draw",
     .asFloat = &Enhancements::lensFlare},
    {.key = "sharpstars", .label = "Sharp stars", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageSky, .section = "",
     .tooltip = "Draw the night sky's stars as crisp points. Off, they come from\n"
                "the sky's own star texture, as in the original.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "sharp_stars",
     .controls = "the starfield point draw",
     .asBool = &Enhancements::sharpStars},
    {.key = "fogstrength", .label = "Fog thickness", .kind = SettingKind::Float,
     .minValue = 0, .maxValue = 2, .step = 0.05f, .category = kEnhPageSky, .section = "Fog",
     .tooltip = "How heavy the distance fog is, against the zone's own design.\n"
                "1 is as designed. Below 1 thins it and 0 removes it; above 1\n"
                "brings it closer.",
     .choices = "", .stockValue = 1, .recommendedValue = 1, .saveKey = "fog_strength",
     .controls = "LightingManager fog distance scale",
     .asFloat = &Enhancements::fogStrength},
    {.key = "fogskyblend", .label = "Fog takes the sky's colour", .kind = SettingKind::Float,
     .minValue = 0, .maxValue = 1, .step = 0.05f, .category = kEnhPageSky, .section = "",
     .tooltip = "How much distant fog is tinted toward the sky behind it. 0 uses\n"
                "the zone's fog colour alone, as the original does; 1 matches\n"
                "the sky.",
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "fog_sky_blend",
     .controls = "LightingManager fog colour blend",
     .asFloat = &Enhancements::fogSkyBlend},
    {.key = "highlightrolloff", .label = "Soft highlights", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageSky, .section = "",
     .tooltip = "Where the game's own glow pushes sunlit snow, pale stone or a\n"
                "bright spell past white, ease it into white instead of\n"
                "clipping it flat, so its shape stays. Darker colours are\n"
                "untouched, and zones without glow look exactly the same.",
     // E18, measured in docs/enhancements-plan.md (3.13).
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "highlight_rolloff",
     .controls = "the shoulder at the end of the glow composite (fxaa.frag, "
                 "highlight_rolloff.hpp): a push constant; off and in a zone with "
                 "no glow the composite is the stock one",
     .asBool = &Enhancements::highlightRollOff},

    // --------------------------------------------------------------- Distance
    {.key = "streamfadein", .label = "Fade in new scenery", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageSky, .section = "Distance",
     .tooltip = "Ground, buildings and trees that finish loading as you travel\n"
                "dissolve in over a moment instead of appearing all at once.\n"
                "Off is the original pop-in.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "stream_fade_in",
     .controls = "the load-time stamp on streamed terrain chunks, WMOs and doodads "
                 "and the dither in terrain_fade.frag, wmo.frag and m2.frag; off "
                 "stamps nothing and the terrain never binds its fade pipeline",
     .asBool = &Enhancements::streamFadeIn},
    {.key = "horizondetail", .label = "Lit far horizon", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageSky, .section = "",
     .tooltip = "The distant mountains past the view distance, and the far\n"
                "landmarks drawn with them, catch the sun under the haze, so\n"
                "their ridges and faces show. Off is the original: one flat\n"
                "fog-coloured silhouette.",
     // Measured in docs/enhancements-plan.md (E11).
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "horizon_detail",
     .controls = "TerrainHorizon's lit pipelines and the WDL normals they read (made "
                 "on first use); off draws the stock fog-coloured pass",
     .asBool = &Enhancements::horizonDetail},
    // On the Sky page, under a heading of its own: the Motion page's two
    // columns are full (test_settings_panel_layout).
    {.key = "caustics", .label = "Sunlight under water", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageSky, .section = "Under water",
     .tooltip = "Faint ripples of sunlight play over the ground, rocks and\n"
                "wrecks under shallow water, fading as the water deepens, in\n"
                "shade and after dark. The original lights nothing below the\n"
                "surface.",
     // E15, measured in docs/enhancements-plan.md (3.13).
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "caustics",
     .controls = "WaterRenderer's caustics pipelines (built on first use) and one "
                 "extra draw of each water and ocean surface in view after it, "
                 "reading the scene's depth copy; none at night; off draws nothing "
                 "and takes no depth copy for it",
     .asBool = &Enhancements::caustics},
    // E34, measured in docs/enhancements-plan.md (3.19): 1-2 ms at 2560x1080
    // where water fills the view, over its 1 ms, so recommended off.
    {.key = "hdwater", .label = "HD water", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageSky, .section = "",
     .tooltip = "Lakes, rivers, the sea and the canals keep their own moving\n"
                "texture and colours, with a rippled surface that mirrors the\n"
                "sky, a glint of the sun, clear shallows that deepen to the\n"
                "zone's own water colour, soft edges at the shore\n"
                "and a gentle bend to what lies below. Off is the original\n"
                "water. Lava and slime stay as they are.",
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "hd_water",
     .controls = "WaterRenderer's HD pipelines (water_hd.frag, built on first use, "
                 "let go when off) in place of water.frag, the scene's depth copy "
                 "and, where the water draws after the scene pass, a colour copy "
                 "for the refraction; off draws water.frag and "
                 "copies no colour",
     .asBool = &Enhancements::hdWater},

    // ------------------------------------------------------ Water and weather
    {.key = "shorelinefoam", .label = "Shoreline foam", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWater, .section = "Water and weather",
     .tooltip = "A soft band of foam where water laps at the shore, rocks and\n"
                "posts. Off is the original water's edge.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "shoreline_foam",
     .controls = "the scene depth copy and the foam term in water.frag",
     .asBool = &Enhancements::shorelineFoam},
    {.key = "watersplashes", .label = "Splashes and wakes", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWater, .section = "",
     .tooltip = "Spray and froth where you swim or wade, and bubbles under\n"
                "water. The original water has none; its own underwater motes\n"
                "stay either way.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "water_splashes",
     .controls = "SwimEffects spray/bubbles and the water renderer's wake",
     .asBool = &Enhancements::waterSplashes},
    {.key = "waterinsects", .label = "Insects over water", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWater, .section = "",
     .tooltip = "Midges dancing over reeds and still water. Not in the\n"
                "original game.",
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "water_insects",
     .controls = "SwimEffects insect swarms",
     .asBool = &Enhancements::waterInsects},
    {.key = "waterreflections", .label = "Water reflections", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWater, .section = "",
     .tooltip = "The sky mirrored faintly in lakes, rivers and the sea. The\n"
                "original water reflects nothing.",
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "water_reflections",
     .controls = "the reflection sky draw and its term in water.frag",
     .asBool = &Enhancements::waterReflections},

    // ----------------------------------------------------------------- Motion
    {.key = "foliagewind", .label = "Foliage and cloth wind", .kind = SettingKind::Float,
     .minValue = 0, .maxValue = 2, .step = 0.05f, .category = kEnhPageWater, .section = "Motion",
     .tooltip = "How hard the wind moves trees, banners and awnings. 1 is the\n"
                "usual breeze; rain and storms stir it further. 0 holds\n"
                "everything still, as in the original.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "foliage_wind",
     .controls = "the sway blocks in m2.vert, m2_shadow.vert and wmo.vert",
     .asFloat = &Enhancements::foliageWind},
    {.key = "foliagepush", .label = "Plants part as you pass", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWater, .section = "",
     .tooltip = "Grass, ferns and bushes bend away from you as you walk through\n"
                "them, and spring back after.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "foliage_push",
     .controls = "the player brush in m2.vert",
     .asBool = &Enhancements::foliagePush},
    {.key = "boatbob", .label = "Boats move on the water", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWater, .section = "",
     .tooltip = "Moored boats rise, fall and rock a little with the water.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "boat_bob",
     .controls = "the boat motion in m2.vert",
     .asBool = &Enhancements::boatBob},
    // One system for everyone's feet; it replaced the player's mount dust and
    // keeps that row's keys, so a saved "off" stays off.
    {.key = "mountdust", .label = "Dust kicked up", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWater, .section = "",
     .tooltip = "Small puffs of dust where players, creatures and mounts run\n"
                "on dry dirt and sand - more at a gallop, none on grass, snow,\n"
                "stone or in the rain. The original throws up nothing.",
     // Measured in docs/enhancements-plan.md (E21): nothing standing still,
     // a few hundredths of a millisecond with a crowd running by.
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "mount_dust",
     .controls = "FootfallDust (made on first use), the footfall queue and its "
                 "ground lookups; off installs no event hook and draws nothing",
     .asBool = &Enhancements::footfallDust},
    {.key = "corpsesettle", .label = "Bodies lie along the ground", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWater, .section = "",
     .tooltip = "A fallen body settles onto the slope or steps under it as it\n"
                "lands, instead of lying level with half of it in the air. The\n"
                "original draws every body level.",
     // Measured in docs/enhancements-plan.md (E8): five floor queries once
     // per death, a branch per unit each frame while any body is tilted.
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "corpse_settle",
     .controls = "CharacterRenderer::updateCorpseSettle: the floor queries and the "
                 "tilt in getModelMatrix; off leaves every model matrix stock",
     .asBool = &Enhancements::corpseSettle},
    {.key = "secondarymotion", .label = "Capes, tails and hair", .kind = SettingKind::Enum,
     .minValue = 0, .maxValue = 2, .step = 1, .category = kEnhPageWater, .section = "",
     .tooltip = "Cloaks, robe and tabard flaps swing a little as characters run,\n"
                "turn, jump and land, and stir in the wind; tails and long hair\n"
                "follow with a lag. The animation still moves them as before.\n"
                "Off is the original: they move only as animated.",
     // Measured in docs/enhancements-plan.md (E7): a spring per chosen bone,
     // only for units within 45 yd that are drawn.
     .choices = "Off|Capes and robes|Capes, robes, tails and hair",
     .stockValue = 0, .recommendedValue = 2, .saveKey = "secondary_motion",
     .controls = "CharacterRenderer's spring chains (secondary_motion.hpp): the chain "
                 "search per model and the per-bone springs in calculateBoneMatrices; "
                 "off searches nothing and poses every bone from its tracks",
     .asInt = &Enhancements::secondaryMotion},
    {.key = "secondarymotionstrength", .label = "How far capes swing", .kind = SettingKind::Float,
     .minValue = 0, .maxValue = 2, .step = 0.05f, .category = kEnhPageWater, .section = "",
     .tooltip = "How far cloaks, robes, flaps, tails and hair swing and trail.\n"
                "1 is the tuned amount; 0 holds them to the animation, as in\n"
                "the original.",
     // Measured in docs/enhancements-plan.md (E7): the same springs, scaled.
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "secondary_motion_strength",
     .controls = "CharacterRenderer::setMotionEnhancements: the springs' gain and "
                 "limits; 0 runs no spring, as Off does",
     .enabledWhen = "secondarymotion", .asFloat = &Enhancements::secondaryMotionStrength},
    {.key = "remotesmoothing", .label = "Smoother other players", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWater, .section = "",
     .tooltip = "Other players and creatures glide over small position\n"
                "corrections instead of jumping, and always ease between\n"
                "standing, walking and running. Off is the original: they are\n"
                "put where each update says.",
     // Measured in docs/enhancements-plan.md (E10): a few vector operations
     // per unit per frame.
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "remote_smoothing",
     .controls = "the drawn-position and facing offsets in CharacterRenderer::update and "
                 "getModelMatrix, and the locomotion blend floor in beginSequenceBlend; "
                 "off draws every unit where it is set and every blend as authored",
     .asBool = &Enhancements::remoteSmoothing},
    {.key = "footplacement", .label = "Feet on the ground", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWater, .section = "",
     .tooltip = "Characters standing or walking on a slope or a stair plant\n"
                "their feet on it: the uphill knee bends and the body lowers a\n"
                "little. It fades out at a run, mounted, swimming or flying.\n"
                "Off is the original: legs posed as if the ground were flat.",
     // Measured in docs/enhancements-plan.md (E9).
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "foot_placement",
     .controls = "CharacterRenderer::updateFootPlacement (the floor reads under the "
                 "feet of units within 30 yd) and the leg solve in "
                 "calculateBoneMatrices; off reads nothing and poses every bone stock",
     .asBool = &Enhancements::footPlacement},
    {.key = "poseblend", .label = "Smoother animation changes", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = "Combat", .section = "Feel (the reference client)",
     .tooltip = "When a character changes what it is doing in the middle of a\n"
                "change - a swing cut short by a flinch, a cast by a run - it\n"
                "eases from where its body is into the new move, as in WoW\n"
                "Enhanced. Off is the original, which can jump part of the way.",
     // docs/re/enhanced-combat.md: a copy of the pose per interrupted fade.
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "pose_blend",
     .controls = "the snapshot branch of CharacterRenderer::beginSequenceBlend (a fade "
                 "begun during a fade copies localPose); off keeps 0x826C40's swap",
     .after = "focuscastkey",
     .asBool = &Enhancements::poseBlend},
    {.key = "smoothzoom", .label = "Smooth camera zoom", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWater, .section = "",
     .tooltip = "The mouse wheel's zoom glides to its new distance and settles,\n"
                "instead of moving at one steady speed and stopping dead. Walls\n"
                "still pull the camera in at once.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "smooth_zoom",
     .controls = "the eased zoom step in CameraController (camera_zoom.hpp); off is "
                 "the stock 8.33 yd/s travel",
     .asBool = &Enhancements::smoothZoom},
    {.key = "extrasmoke", .label = "Extra chimney smoke", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWater, .section = "",
     .tooltip = "Additional smoke puffs over chimneys and smoke stacks, on top\n"
                "of the smoke the models carry themselves.",
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "extra_smoke",
     .controls = "the synthesised smoke particles and their draw",
     .asBool = &Enhancements::extraSmoke},
    {.key = "drunkwobble", .label = "Drunken wobble", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWater, .section = "",
     .tooltip = "When drunk, the view waves, smears and sways. Off is the\n"
                "original: the picture blurs with how drunk you are.",
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "drunk_wobble",
     .controls = "the wave and 9-tap blur in fxaa.frag and the camera sway",
     .asBool = &Enhancements::drunkWobble},

    // ------------------------------------------------------------------ Edges
    {.key = "smoothleaves", .label = "Smooth leaf edges", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWater, .section = "Edges",
     .tooltip = "Softens the cut edges of leaves, grass and hair when\n"
                "anti-aliasing is on, and keeps distant canopies full. Off is\n"
                "the original hard cut.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "smooth_leaves",
     .controls = "the coverage ramp, fringe repair and mip boost in m2/character .frag "
                 "(off: the stock 224/255 alpha key)",
     .asBool = &Enhancements::smoothLeaves},
    {.key = "glowsprites", .label = "Soft lamp halos", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWater, .section = "",
     .tooltip = "Lamp, torch and brazier glow cards drawn as soft halos that\n"
                "face you. Off draws the cards the models carry, as the\n"
                "original does.",
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "glow_sprites",
     .controls = "glow sprite generation and recordGlowSprites",
     .asBool = &Enhancements::glowSprites},
    {.key = "softparticles", .label = "Soft particles", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWater, .section = "",
     .tooltip = "Fire, smoke, spell clouds and spray fade out where they meet\n"
                "the ground or a wall, instead of cutting a hard line through\n"
                "it. Off is the original edge.",
     // Off in the recommended set (enhancements-plan.md 3.11): its late pass
     // is a few hundred draws recorded on the main thread after the scene,
     // 0.15-0.25 ms of main-thread time, which with the shadows' unit map and
     // the light gather put the set over its 0.5 ms; nothing else in the set
     // buys as little for its time.
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "soft_particles",
     .controls = "the late particle pass (SoftParticlePass: a single-sample pass "
                 "after the scene that reads its depth) and the soft particle "
                 "pipelines; off draws every particle in the scene pass as stock",
     .asBool = &Enhancements::softParticles},

    // ---------------------------------------------------------------- Weather
    // The "Enhancements: Weather" page (docs/enhancements-plan.md E23-E31,
    // rendering/weather_fx.hpp for the rules). The first three moved here
    // from the Motion page; their keys did not change.
    {.key = "weatherintensity", .label = "Weather intensity", .kind = SettingKind::Float,
     .minValue = 1.0f, .maxValue = 2.5f, .step = 0.25f, .category = kEnhPageWeather,
     .section = "Rain and snow",
     .tooltip = "How heavily rain, snow and sand fall, against the original's\n"
                "count. 1 is the original; higher fills the air more, and the\n"
                "distant sheets and splashes a little more with it. The server\n"
                "still decides when it rains and how hard.",
     .choices = "", .stockValue = 1, .recommendedValue = 1.5f, .saveKey = "weather_intensity",
     .controls = "Weather::Style::intensity: the drop/flake target times countScale (1-2.5) "
                 "and the splash cap; the vertex buffer grows on first need",
     .asFloat = &Enhancements::weatherIntensity},
    {.key = "variedrain", .label = "Varied raindrops", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWeather, .section = "",
     .tooltip = "Raindrops of every size: most fine, a few heavy ones falling\n"
                "faster on longer streaks, and splashes on the ground and roofs\n"
                "while you move and further out. The original's drops are all\n"
                "alike and splash only near you while you stand still.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "varied_rain",
     .controls = "Weather::Style::variedRain: per-drop size, streak length and opacity "
                 "at spawn; the splash gate and radius",
     .asBool = &Enhancements::variedRain},
    {.key = "softsnow", .label = "Softer snow", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWeather, .section = "",
     .tooltip = "Snowflakes from a speck to a clump that turn as they fall and\n"
                "ride slow eddies. The original's flakes are all one size and\n"
                "sway on their own.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "soft_snow",
     .controls = "Weather::Style::softSnow: per-flake size, opacity and spin at spawn; "
                 "a shared eddy field in the update",
     .asBool = &Enhancements::softSnow},
    {.key = "weathersheets", .label = "Distant rain and snow", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWeather, .section = "",
     .tooltip = "Tall sheets of rain, snow or blowing dust drift across the\n"
                "middle distance in a heavy fall, in the colour of the fog,\n"
                "beyond the original game's own closer mist.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "weather_sheets",
     .controls = "Weather::Style::mistSheets: up to 40 camera-facing SnowMist01 sheets "
                 "16-70 yd out past half the weather's level (sand: any level), "
                 "on top of the stock mist (stock_weather.hpp)",
     .asBool = &Enhancements::weatherSheets},
    {.key = "wetground", .label = "Wet ground in rain", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWeather, .section = "",
     .tooltip = "Ground and outside walls darken and catch a faint sheen while\n"
                "it rains, and dry off slowly after. Never indoors.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "wet_ground",
     .controls = "the wet factor (rtParams.w) and wet_surface.glsli",
     .asBool = &Enhancements::wetGround},
    {.key = "rainripples", .label = "Rain ripples", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWeather, .section = "",
     .tooltip = "While it rains, drops ring the surface of lakes and rivers and\n"
                "the puddled ground, harder in a downpour. The original rain\n"
                "leaves the water as it was.",
     // Measured in docs/enhancements-plan.md (E5): nothing while dry - the
     // shaders skip the block on a zero strength - and a few tenths of a
     // millisecond on water-heavy views in rain.
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "rain_ripples",
     .controls = "the rain strength (rtParams.z, the water push) and the ring "
                 "block in water.frag and wet_surface.glsli; zero while dry",
     .asBool = &Enhancements::rainRipples},
    {.key = "weatherwind", .label = "Rain and snow in the wind", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWeather, .section = "",
     .tooltip = "Rain slants and snow drifts with the same wind that moves the\n"
                "trees, harder in a storm and in the gusts that roll across the\n"
                "land. Off is the original's fixed drift. Needs foliage wind.",
     // Measured in docs/enhancements-plan.md (E19).
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "weather_wind",
     .controls = "Weather::setWind: the wind added to each drop and flake as it "
                 "moves; off adds nothing",
     .asBool = &Enhancements::weatherWind},
    {.key = "snowcover", .label = "Snow settles", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWeather, .section = "",
     .tooltip = "While it snows, a thin white settles on the ground and on\n"
                "roofs and ledges over a few minutes, and melts away slowly\n"
                "after. Already white ground stays as it is. Never indoors.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "snow_cover",
     .controls = "the snow cover (localLightTileScale.w), eased over minutes, and "
                 "snowCover* in wet_surface.glsli (terrain, exterior WMO groups); "
                 "zero without snow",
     .asBool = &Enhancements::snowCover},
    {.key = "stormlightning", .label = "Lightning and thunder", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWeather, .section = "Storms and the air",
     .tooltip = "In a thunderstorm, lightning lights up the land now and then,\n"
                "and its thunder rolls in a moment later - sooner the closer\n"
                "the strike. The original's storms are rain alone.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "storm_lightning",
     .controls = "a strike timer in thunderstorms only; the flash lifts the frame's "
                 "sun, ambient and fog colours for under a second; the thunder is "
                 "a stock Blasted Lands bolt sample, delayed by the distance",
     .asBool = &Enhancements::stormLightning},
    {.key = "sandstorms", .label = "Sandstorms", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWeather, .section = "",
     .tooltip = "In a sandstorm (Tanaris, Silithus and the like) the haze\n"
                "closes in and the wind blows harder. Off, the original\n"
                "game's sand and dust are still drawn.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "sandstorms",
     .controls = "on top of the stock sand (Weather::updateSand): the fog pulled in by "
                 "up to 40%, eased, the foliage and cloth wind and the volumetric mist",
     .asBool = &Enhancements::sandstorms},
    {.key = "coldbreath", .label = "Breath in the cold", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWeather, .section = "",
     .tooltip = "The original game's breath in the cold (Dun Morogh,\n"
                "Winterspring, most of Northrend) also wherever it snows.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "cold_breath",
     .controls = "Renderer::onUnitBreathEvent: the stock $BTH breath (ColdBreath.m2) "
                 "played while snow falls, outside the AreaTable-cold areas too",
     .asBool = &Enhancements::coldBreath},
    {.key = "fireflies", .label = "Fireflies at night", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWeather, .section = "",
     .tooltip = "Fireflies drift and glow among the trees and meadows of the\n"
                "green zones at night, out of doors and out of the rain. Not\n"
                "in the original game.",
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "fireflies",
     .controls = "up to 40 additive glows within 16 yd in the weather's draw, night "
                 "only, in the zones weather_fx::climateForZone marks",
     .asBool = &Enhancements::fireflies},
    {.key = "rainindoors", .label = "Rain heard indoors", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWeather, .section = "",
     .tooltip = "Under a roof you still hear the rain or the storm outside,\n"
                "faintly. The original falls silent on the weather the moment\n"
                "you step in.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "rain_indoors",
     .controls = "AmbientSoundManager's roof loop: the weather's own loop at a quarter "
                 "of its level while the room drops it",
     .asBool = &Enhancements::rainIndoors},
    {.key = "weatherpreview", .label = "Preview weather (this client only)", .kind = SettingKind::Enum,
     .minValue = 0, .maxValue = 9, .step = 1, .category = kEnhPageWeather, .section = "Preview",
     .tooltip = "Shows a weather here, on this client only, to see the options\n"
                "above without waiting for it. The server and everyone else\n"
                "keep the real weather. Set it back to Off when done.",
     .choices = "Off (the server's weather)|Light rain|Rain|Heavy rain|Thunderstorm|"
                "Light snow|Snow|Heavy snow|Light sandstorm|Sandstorm",
     .stockValue = 0, .recommendedValue = 0, .saveKey = "weather_preview",
     .controls = "Renderer::effectiveWeather: the preview's Weather.dbc row and grade "
                 "in place of SMSG_WEATHER's for the picture, the storm light and the "
                 "weather loop; nothing is sent",
     .asInt = &Enhancements::weatherPreview},
    {.key = "fixedtimeofday", .label = "Time of day (this client only)", .kind = SettingKind::Enum,
     .minValue = 0, .maxValue = 8, .step = 1, .category = kEnhPageWeather, .section = "",
     .tooltip = "Holds the sky and the world's light at one hour, on this\n"
                "client only. The server's clock, quests and everyone else are\n"
                "unchanged. Off follows the server's time.",
     .choices = "Off (the server's time)|Dawn (6:00)|Morning (9:00)|Noon (12:00)|"
                "Afternoon (15:00)|Dusk (19:00)|Evening (21:00)|Night (23:00)|"
                "Midnight (0:00)",
     .stockValue = 0, .recommendedValue = 0, .saveKey = "fixed_time_of_day",
     .controls = "Renderer::update: the hour handed to LightingManager::update in place "
                 "of GameHandler::getGameTime; nothing is sent",
     .asInt = &Enhancements::fixedTimeOfDay},
    {.key = "colorgrade", .label = "Colour grade (the reference client)", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageEnhanced, .section = "the reference client",
     .tooltip = "the reference client's daytime grade in Azeroth and Kalimdor: brighter\n"
                "mid-tones and richer colour around midday, easing off toward\n"
                "morning and evening. Indoors, at night and in dungeons the\n"
                "picture is unchanged.",
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "color_grade",
     .controls = "the curve at the end of the glow composite (fxaa.frag, "
                 "enhanced_grade.hpp): two push constants; keeps that pass running",
     .asBool = &Enhancements::colorGrade},
    {.key = "enhanceddaylight", .label = "Daylight (the reference client)", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageEnhanced, .section = "",
     .tooltip = "Daylight in Azeroth and Kalimdor shifted the way the reference client\n"
                "repainted it: a whiter sun, brighter fill light in the shade,\n"
                "a brighter sky and haze. Night, dungeons and Outland are\n"
                "unchanged.",
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "enhanced_daylight",
     .controls = "LightingManager: the day's light colours scaled by the measured "
                 "Enhanced trend (enhanced_palette.hpp) after the band blend",
     .asBool = &Enhancements::enhancedDaylight},
    {.key = "sunhaze", .label = "Sun haze (the reference client)", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageEnhanced, .section = "",
     .tooltip = "Air over the whole view, thickest near the ground: the distance\n"
                "pales into the sky and the air round a low sun glows gold, as\n"
                "the reference client's global fog does.",
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "sun_haze",
     .controls = "the haze pass (enhanced_haze.frag, built on first use): one full-screen "
                 "pass after the opaque scene, src + dst * transmittance",
     .asBool = &Enhancements::sunHaze},
    {.key = "bloom", .label = "Bloom", .kind = SettingKind::Float,
     .minValue = 0, .maxValue = 2, .step = 0.1f, .category = kEnhPageEnhanced, .section = "",
     .tooltip = "A soft halo round the brightest parts of the picture: sunlit\n"
                "leaves, the sky round the sun, light through mist. 0 is the\n"
                "original, which has only its glow.",
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "bloom",
     .controls = "bloomAt in the glow composite (fxaa.frag): 9 taps of the quarter-size "
                 "scene; keeps that pass running",
     .asFloat = &Enhancements::bloom},
    {.key = "longview", .label = "Long view (the reference client)", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageEnhanced, .section = "",
     .tooltip = "The distance fog ends well past the edge of what is drawn, as\n"
                "the reference client's does, so far hills stand clear in the haze\n"
                "instead of fading into a wall of fog. Azeroth and Kalimdor.",
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "long_view",
     .controls = "LightingManager: the frame fog's end at 18/7 of the view distance "
                 "on maps below 530",
     .asBool = &Enhancements::longView},

    // ------------------------------------------------------------ Experimental
    {.key = "grassenabled", .label = "Grass (experimental)", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = "Grass", .section = "Ground cover (experimental)",
     .tooltip = "Grow grass from the terrain's own ground-effect data, bending in\n"
                "the wind and parting as you walk through it. Experimental: it\n"
                "costs time on the main thread as you move and has known faults.",
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "grass_enabled",
     .controls = "GrassRenderer generation, cull and draw",
     .asBool = &Enhancements::grass},
    {.key = "grassdensity", .label = "Grass density", .kind = SettingKind::Int,
     .minValue = 0, .maxValue = 300, .step = 5, .category = "Grass", .section = "",
     .tooltip = "How much grass grows, as a percentage of what the terrain asks\n"
                "for. 100 is as designed.",
     .choices = "", .stockValue = 100, .recommendedValue = 100, .saveKey = "grass_density",
     .controls = "grass density scale", .enabledWhen = "grassenabled",
     .asInt = &Enhancements::grassDensity},
    {.key = "grassheight", .label = "Grass height", .kind = SettingKind::Int,
     .minValue = 50, .maxValue = 300, .step = 5, .category = "Grass", .section = "",
     .tooltip = "How tall the blades grow, as a percentage. Taller grass shows\n"
                "the wind crossing it more.",
     .choices = "", .stockValue = 100, .recommendedValue = 100, .saveKey = "grass_height",
     .controls = "grass height scale", .enabledWhen = "grassenabled",
     .asInt = &Enhancements::grassHeight},
    {.key = "grassdistance", .label = "Grass distance", .kind = SettingKind::Int,
     .minValue = 30, .maxValue = 2000, .step = 5, .category = "Grass", .section = "",
     .tooltip = "How far out grass is drawn, in yards. Beyond 45 the field thins\n"
                "with distance and each blade fades in as you ride toward it.\n"
                "Farther costs more each time you move.",
     .choices = "", .stockValue = 150, .recommendedValue = 150, .saveKey = "grass_distance",
     .controls = "grass draw distance", .enabledWhen = "grassenabled",
     .asInt = &Enhancements::grassDistance},
    {.key = "raytracedlighting", .label = "Ray traced lighting (highly experimental)",
     .kind = SettingKind::Enum, .minValue = 0, .maxValue = 3, .step = 1,
     .category = "Ray Tracing", .section = "Highly experimental",
     .tooltip = "HIGHLY EXPERIMENTAL: expect visual artefacts and a large\n"
                "frame rate cost, especially without ray tracing hardware.\n"
                "Traces rays against the world for the sun's shadows, the\n"
                "darkening in corners and crevices, and light bounced off the\n"
                "ground and walls, out to 100 yards; the shadow map does the\n"
                "rest. Integrated graphics without ray tracing hardware keep\n"
                "it off: there it costs more than every other enhancement\n"
                "together and looks no better than the shadow map.",
     .choices = "Off|Sun shadows|Shadows and occlusion|Shadows, occlusion and bounce light",
     .stockValue = 0, .recommendedValue = 0, .saveKey = "ray_traced_lighting",
     .controls = "RtLighting scene, BVH and its compute passes",
     .asInt = &Enhancements::rtLighting},

    // ---------------------------------------------------- Interface and sound
    {.key = "lowhealthvignette", .label = "Low health warning", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = "HUD", .section = "",
     .tooltip = "Pulse a red edge around the screen while your health is\n"
                "below a fifth. The original game has only the edge flash\n"
                "below.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "low_health_vignette",
     .controls = "the vignette quad in GameScreen", .after = "damageflash",
     .asBool = &Enhancements::lowHealthVignette},
    {.key = "woweemusic", .label = "WoWee soundtrack", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = "Sound", .section = "",
     .tooltip = "Play this client's own music in the rotation alongside the\n"
                "game's.",
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "use_original_soundtrack",
     .controls = "the Wowee tracks in ZoneManager's music rotation", .after = "musicvolume",
     .asBool = &Enhancements::woweeMusic},
    {.key = "spellqueue", .label = "Spell queue window (ms)", .kind = SettingKind::Int,
     .minValue = 0, .maxValue = 400, .step = 50, .category = "Combat", .section = "",
     .tooltip = "Press your next spell up to this long before the global\n"
                "cooldown, its own cooldown or your cast ends, and it goes out\n"
                "the moment it can, as in the reference client (400). 0 is the original\n"
                "3.3.5: a press before then is refused.",
     .choices = "", .stockValue = 0, .recommendedValue = 400, .saveKey = "spell_queue_window",
     .controls = "SpellHandler's queue: one kept press, sent from updateTimers when its "
                 "cooldown and the GCD are over, or at the cast's end; 0 reads the "
                 "SpellQueueWindow CVar (default 0, no queue)",
     .after = "focuscastkey",
     .asInt = &Enhancements::spellQueueWindow},
    {.key = "swingtimer", .label = "Swing timer", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = "Combat", .section = "",
     .tooltip = "A bar over the action bars that fills from each of your melee\n"
                "swings to the next, one per weapon hand, dimmed when your\n"
                "target is out of reach. the reference client's swing timer; the\n"
                "original game has none.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "swing_timer",
     .controls = "GameScreen::renderSwingTimer: two ImGui bars in combat, from "
                 "CombatHandler's swing times and UNIT_FIELD_BASEATTACKTIME",
     .after = "targetcastbar",
     .asBool = &Enhancements::swingTimer},
    {.key = "crispinterface", .label = "Sharp interface text", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = "HUD", .section = "Display",
     .tooltip = "Draw the interface's letters at the size they appear on\n"
                "screen, one to one on the screen's pixels, and its art from\n"
                "smaller copies where it is drawn small. Sharper on a large\n"
                "screen and at a UI scale below 1. Off is the original game's\n"
                "softer text and art. Text stays the same size and place.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "crisp_interface",
     .controls = "the one-sample font bakes and the mip-mapped interface textures in WidgetRenderer",
     .after = "castabledebuffs",
     .asBool = &Enhancements::crispInterface},
    {.key = "ultrawidehud", .label = "Ultrawide HUD", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = "HUD", .section = "",
     .tooltip = "On a screen wider than 16:9, keep the action bars, unit\n"
                "frames, minimap, chat, bags and windows inside a 16:9 area\n"
                "in the middle while the world fills the screen. Addons and\n"
                "frames you have moved keep the whole screen. Off is the\n"
                "original game's layout, out to the screen's edges.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "ultrawide_hud",
     .controls = "the centred anchor area in WidgetTree::layout; nothing below 16:9",
     .after = "castabledebuffs",
     .asBool = &Enhancements::ultrawideHud},
};

/// The combat, targeting and animation rows as the reference client has them
/// (docs/re/enhanced-combat.md): what a "Enhanced" set gives these keys.
struct EnhancementPresetValue {
    const char* key;
    float value;
};
inline constexpr EnhancementPresetValue kEnhancedCombatValues[] = {
    {"poseblend", 1.0f},
    {"remotesmoothing", 1.0f},
    {"spellqueue", 400.0f},
    {"swingtimer", 1.0f},
};

inline constexpr std::size_t kEnhancementRowCount =
    sizeof(kEnhancementRows) / sizeof(kEnhancementRows[0]);

/// The extra row at the top of the first page: one choice that sets them all.
/// Derived like graphicspreset: it reads Stock when every row is at its stock
/// value, Recommended when every row is at its recommended one, else Custom.
inline constexpr SettingDesc kEnhancementSetRow = {
    "enhancementset", "Enhancement set", SettingKind::Enum, 0, 2, 1, kEnhPage, "Set",
    "Every enhancement at once. Original turns them all off, which is\n"
    "the original game's picture and sound; Recommended is this client's\n"
    "own choice. Change any one and this reads Custom.",
    "Custom|Original (all off)|Recommended", 0};

inline const EnhancementRow* findEnhancementRow(const std::string& key) {
    for (const auto& r : kEnhancementRows) {
        if (key == r.key) return &r;
    }
    return nullptr;
}

inline const EnhancementRow* findEnhancementRowBySaveKey(const std::string& saveKey) {
    for (const auto& r : kEnhancementRows) {
        if (saveKey == r.saveKey) return &r;
    }
    return nullptr;
}

/// A row's value in `e`, as a number.
constexpr float enhancementValue(const Enhancements& e, const EnhancementRow& r) {
    if (r.asBool) return (e.*(r.asBool)) ? 1.0f : 0.0f;
    if (r.asInt) return static_cast<float>(e.*(r.asInt));
    return e.*(r.asFloat);
}

/// Set a row's value in `e`, held to the row's range.
constexpr void setEnhancementValue(Enhancements& e, const EnhancementRow& r, float v) {
    if (r.asBool) { e.*(r.asBool) = (v != 0.0f); return; }
    if (r.kind != SettingKind::Bool) {
        if (v < r.minValue) v = r.minValue;
        if (v > r.maxValue) v = r.maxValue;
    }
    if (r.asInt) { e.*(r.asInt) = static_cast<int>(v + (v >= 0 ? 0.5f : -0.5f)); return; }
    e.*(r.asFloat) = v;
}

/// Every row at its recommended value: a fresh install.
constexpr Enhancements recommendedEnhancements() {
    Enhancements e;
    for (const auto& r : kEnhancementRows) setEnhancementValue(e, r, r.recommendedValue);
    return e;
}

}  // namespace ui
}  // namespace wowee
