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

inline constexpr EnhancementRow kEnhancementRows[] = {
    // ------------------------------------------------------- Light and shadow
    {.key = "shadows", .label = "Sun shadows", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPage, .section = "Light and shadow",
     .tooltip = "Shadows from the sun and moon on the ground, buildings and\n"
                "everyone in the world. Off is the original game's default: a\n"
                "soft round shadow under each character and the ground's own\n"
                "painted shadows.",
     // Off in the recommended set by its cost, not its look: at the Ultra
     // shadow distance on an Iris Xe the map costs 2.8-4.3 ms of GPU and
     // 0.7-2.2 ms of main-thread recording (most of it the M2 and unit
     // casters) over the stock blob - alone more than the whole set's budget
     // of 3 ms GPU and 0.5 ms CPU (docs/enhancements-plan.md, "Cost"). The
     // quality presets keep it (presetEnhancements).
     .choices = "", .stockValue = 0, .recommendedValue = 0, .saveKey = "shadows",
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
                "five-sample filter; Soft takes nine samples on a turning disc.",
     .choices = "Original|Soft", .stockValue = 0, .recommendedValue = 1,
     .saveKey = "shadow_filter",
     .controls = "sampleShadowPCF: 9-tap Vogel disk vs the stock 5-tap PCF",
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
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "normal_mapping",
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
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "pom",
     .controls = "the parallax march in wmo .frag",
     .restartNote = "Turning this on reaches buildings loaded from now on;\nrestart to see it everywhere.",
     .asBool = &Enhancements::parallax},
    {.key = "parallaxquality", .label = "Surface depth quality", .kind = SettingKind::Enum,
     .minValue = 0, .maxValue = 2, .step = 1, .category = kEnhPage, .section = "",
     .tooltip = "How finely each surface is traced for that depth: 16, 32 or 64\n"
                "steps. Higher looks steadier up close and costs more on big walls.",
     .choices = "Low|Medium|High", .stockValue = 1, .recommendedValue = 1, .saveKey = "pom_quality",
     .controls = "parallax step count", .enabledWhen = "parallax",
     .asInt = &Enhancements::parallaxQuality},

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

    // ------------------------------------------------------ Water and weather
    {.key = "shorelinefoam", .label = "Shoreline foam", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWater, .section = "Water and weather",
     .tooltip = "A soft band of foam where water laps at the shore, rocks and\n"
                "posts. Off is the original water's edge.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "shoreline_foam",
     .controls = "the scene depth copy and the foam term in water.frag",
     .asBool = &Enhancements::shorelineFoam},
    {.key = "wetground", .label = "Wet ground in rain", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWater, .section = "",
     .tooltip = "Ground and outside walls darken and catch a faint sheen while\n"
                "it rains, and dry off slowly after. Never indoors.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "wet_ground",
     .controls = "the wet factor (rtParams.w) and wet_surface.glsli",
     .asBool = &Enhancements::wetGround},
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
    {.key = "mountdust", .label = "Dust under mounts", .kind = SettingKind::Bool,
     .minValue = 0, .maxValue = 0, .step = 0, .category = kEnhPageWater, .section = "",
     .tooltip = "A running mount kicks up dust at its feet.",
     .choices = "", .stockValue = 0, .recommendedValue = 1, .saveKey = "mount_dust",
     .controls = "MountDust spawning and its draw",
     .asBool = &Enhancements::mountDust},
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
                "ground and walls.",
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
