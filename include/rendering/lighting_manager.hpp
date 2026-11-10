#pragma once

#include <chrono>
#include <vector>
#include <map>
#include <memory>
#include <string>
#include <glm/glm.hpp>

namespace wowee {
namespace pipeline { class DBCFile; class AssetManager; }

namespace rendering {

/**
 * Time-of-day lighting parameters sampled from DBC curves
 */
struct LightingParams {
    glm::vec3 ambientColor{0.4f, 0.4f, 0.5f};      // Fill lighting
    glm::vec3 diffuseColor{1.0f, 0.95f, 0.8f};     // Directional sun color
    glm::vec3 directionalDir{0.0f, -1.0f, 0.5f};   // Way the light travels (normalized)
    /// Unit direction from the eye to the sun sprite. Not -directionalDir: the
    /// stock light stays 20-37 degrees up all day and night while the sun
    /// climbs to the zenith and sets (see stock_day_night.hpp). Follows the
    /// clock alone, so it is neither blended nor smoothed. Defaults to noon.
    glm::vec3 sunDir{0.057f, 0.057f, 0.997f};

    glm::vec3 fogColor{0.5f, 0.6f, 0.7f};          // Fog color
    float fogStart = 100.0f;                        // Fog start distance
    float fogEnd = 1000.0f;                         // Fog end distance
    float fogDensity = 0.001f;                      // Fog density
    /// FloatBand 1, the fog start as a fraction of the end (0x7ebff0 clamps
    /// it to [-1, 1]). Blended between lights; fogStart is end * this.
    float fogScaler = 0.25f;
    /// The fog curve's power (stock_light::Fog): 1 on Azeroth and Kalimdor,
    /// 1.5 to 7 on maps from 530 on, where the fog runs to the far clip.
    /// The shaders read it from localLightMeta.y (stock_fog.glsli): what
    /// shows of a surface at view depth z is
    /// clamp(pow(max((end - z) / (end - start), 0), this)) (0x873210).
    float fogExponent = 1.0f;

    glm::vec3 skyTopColor{0.5f, 0.7f, 1.0f};       // Sky zenith color
    glm::vec3 skyMiddleColor{0.7f, 0.85f, 1.0f};   // Sky horizon color
    glm::vec3 skyBand1Color{0.9f, 0.95f, 1.0f};    // Sky band 1
    glm::vec3 skyBand2Color{1.0f, 0.98f, 0.9f};    // Sky band 2

    float cloudDensity = 0.3f;                      // Cloud density/opacity
    float horizonGlow = 0.3f;                       // Horizon glow intensity

    // The rest of the stock sky, LightIntBand channels 6-13 (see
    // LightParamsProfile::ColorChannel). The dome runs top, middle, band 1,
    // band 2, smog at the horizon and the raw fog colour below it - skyFogColor
    // is channel 7 before the fog-to-sky blend fogColor gets.
    glm::vec3 skySmogColor{0.7f, 0.7f, 0.7f};
    glm::vec3 skyFogColor{0.5f, 0.6f, 0.7f};
    glm::vec3 sunColor{1.0f, 0.97f, 0.87f};         // The sun sprite (ch 9)
    glm::vec3 cloudSunColor{1.0f, 0.78f, 0.54f};    // Sunlit side of clouds (ch 10)
    glm::vec3 cloudEmissiveColor{0.17f, 0.41f, 0.52f};   // ch 11
    glm::vec3 cloudLayer1AmbientColor{0.0f};        // ch 12
    glm::vec3 cloudLayer2AmbientColor{0.38f, 0.51f, 0.72f};  // ch 13
    /// LightFloatBand channel 2: how much the sun and moon show through cloud.
    float celestialGlowThrough = 1.0f;
    /// LightParams.dbc field 4 (Glow): the stock ffxGlow strength, blended
    /// between lights and toward the storm set like the floats (0x7ed4c0,
    /// 0x7ec220). Read by Renderer for PostProcessPipeline::setGlow.
    float glow = 0.5f;
    /// How dark the sun's shadow is, 0.4..0.9, from LightIntBand channel 8
    /// (SHADOW_COLOR): the darker that grey, the deeper the shadow. 0.8 is
    /// what every shadow was before, and roughly what noon in Elwynn reads.
    float shadowOpacity = 0.8f;
    /// world/dnc.db: how much of the day's sun and the night's moon and stars
    /// are up, 0..1. Not blended per light - the same everywhere.
    float dayIntensity = 1.0f;
    float nightIntensity = 0.0f;

    // The liquid colours, LightIntBand channels 14-17: the stock water is this
    // colour, close (shallow) blending to far (deep) with the water's depth,
    // at LightParams' shallow/deep alpha. Defaults are LightParams 1 at noon.
    glm::vec3 oceanCloseColor{30.0f / 255.0f, 48.0f / 255.0f, 62.0f / 255.0f};
    glm::vec3 oceanFarColor{0.0f, 19.0f / 255.0f, 23.0f / 255.0f};
    glm::vec3 riverCloseColor{30.0f / 255.0f, 48.0f / 255.0f, 62.0f / 255.0f};
    glm::vec3 riverFarColor{0.0f, 19.0f / 255.0f, 23.0f / 255.0f};
    float waterShallowAlpha = 0.5f;
    float waterDeepAlpha = 1.0f;
    float oceanShallowAlpha = 0.75f;
    float oceanDeepAlpha = 1.0f;
};

/** Apply any authored ambience that must remain stable regardless of world time. */
void applyZoneAmbienceOverride(uint32_t zoneId, LightingParams& params);

/** Resolve the sky clock shown for a zone without changing the world clock. */
float resolveZoneVisualTimeHours(uint32_t zoneId, bool isIndoors, float worldTimeHours);

/**
 * Light set keyframe for time-of-day interpolation
 */
struct LightKeyframe {
    uint32_t time;  // Time in minutes since midnight (0-1439)

    // Colors stored as RGB int tuples (0-255) in DBC
    glm::vec3 ambientColor;
    glm::vec3 diffuseColor;
    glm::vec3 fogColor;
    glm::vec3 skyTopColor;
    glm::vec3 skyMiddleColor;
    glm::vec3 skyBand1Color;
    glm::vec3 skyBand2Color;

    float fogStart;
    float fogEnd;
    float fogDensity;
    float cloudDensity;
    float horizonGlow;
};

/**
 * Light volume from Light.dbc (spatial lighting)
 */
struct LightVolume {
    uint32_t lightId = 0;
    uint32_t mapId = 0;
    glm::vec3 position{0.0f};  // World position (note: DBC stores as x,z,y!)
    float innerRadius = 0.0f;   // Full weight radius
    float outerRadius = 0.0f;   // Fade-out radius

    // Light.dbc's LightParams slots, columns 7-11 in every client from 1.12
    // to 3.3.5. Read off the file (715 rows): column 8 equals column 10 in
    // 409 of them and column 9 equals column 7 in 337 - each underwater set
    // pairs with its weather set, and many zones have no storm set of their
    // own. Light 1 (the map default) names 12 clear, 13 underwater, 10 storm,
    // 13 storm underwater and 4 death; 4 is the ghost's blue world with
    // LightSkybox 90 (DeathSkybox) and is the death slot of nearly every row.
    // Columns 12-14 are almost all zero.
    enum ParamsSlot : uint8_t {
        SLOT_CLEAR = 0,
        SLOT_CLEAR_UNDERWATER = 1,
        SLOT_STORM = 2,
        SLOT_STORM_UNDERWATER = 3,
        SLOT_DEATH = 4,
        SLOT_COUNT = 5
    };
    uint32_t lightParamsIds[SLOT_COUNT] = {};
    /// The map's default light: the row Light.dbc places at 0,0,0 (radius
    /// 0). The stock client lays it under every other light at full weight
    /// (0x7ecb30, 0x7f3230); a map without one takes Light 1.
    bool isMapDefault = false;
};

/**
 * Color band with time-of-day keyframes
 */
struct ColorBand {
    uint8_t numKeyframes = 0;
    uint16_t times[16];        // Time keyframes (half-minutes since midnight)
    glm::vec3 colors[16];      // Color values (RGB 0-1)
    uint32_t packed[16];       // The same as the file has them, 0x00RRGGBB
};

/**
 * Float band with time-of-day keyframes
 */
struct FloatBand {
    uint8_t numKeyframes = 0;
    uint16_t times[16];        // Time keyframes (half-minutes since midnight)
    float values[16];          // Float values
};

/**
 * LightParams profile with 18 color bands + 6 float bands
 */
struct LightParamsProfile {
    uint32_t lightParamsId = 0;
    uint32_t lightSkyboxId = 0;

    // 18 color channels (IntBand)
    //
    // Read off the file rather than from the order they are usually listed in.
    // The sky gradient anchors the rest: for Stormwind at noon, channels 2 to 5
    // are (0,31,73), (58,162,207), (153,220,245), (175,218,224) - a deep blue
    // overhead paling to the horizon, which can only be the sky and fixes every
    // index around it.
    //
    // Channel 0 is not the ambient. Across zones it is the warm, bright one -
    // (199,168,134) over Dun Morogh's snow, (255,224,169) in Teldrassil,
    // (255,136,0) in Stormwind - while channel 1 is the dark cool one that
    // carries the zone's cast: (31,82,125) in Dun Morogh, violet (125,72,130)
    // in Teldrassil and Winterspring. A scene ambient multiplies every surface,
    // so reading channel 0 into it put Stormwind's orange over the whole city
    // and left it looking like Durotar, whose channel 0 is (255,204,148).
    //
    // Channel 7 is the fog. Channel 6 is the sky's smog layer and is a neutral
    // grey - (180,180,180) at Stormwind noon - which is the pale grey the fog
    // blend further down was written to work around.
    enum ColorChannel {
        DIFFUSE_COLOR = 0,
        AMBIENT_COLOR = 1,
        SKY_TOP_COLOR = 2,
        SKY_MIDDLE_COLOR = 3,
        SKY_BAND1_COLOR = 4,
        SKY_BAND2_COLOR = 5,
        SKY_SMOG_COLOR = 6,
        FOG_COLOR = 7,
        // Channel 8 is a neutral grey (77,77,77 at Stormwind noon): the shadow
        // strength later clients split out as ShadowOpacity. 9-13 follow the
        // order those clients name - sun, cloud sun, cloud emissive, cloud
        // layer 1 and 2 ambient - and fit the file: the storm set's sun (9)
        // is a flat grey and the death set's 9-13 are black.
        SHADOW_COLOR = 8,
        SUN_COLOR = 9,
        CLOUD_SUN_COLOR = 10,
        CLOUD_EMISSIVE_COLOR = 11,
        CLOUD_LAYER1_AMBIENT_COLOR = 12,
        CLOUD_LAYER2_AMBIENT_COLOR = 13,
        // Liquid colours: close is the shallow end, far the deep end.
        OCEAN_CLOSE_COLOR = 14,
        OCEAN_FAR_COLOR = 15,
        RIVER_CLOSE_COLOR = 16,
        RIVER_FAR_COLOR = 17,
        COLOR_CHANNEL_COUNT = 18
    };

    // LightParams.dbc fields 5-8: the water's opacity at the shallow and the
    // deep end of the colour ramp above.
    float waterShallowAlpha = 0.5f;
    float waterDeepAlpha = 1.0f;
    float oceanShallowAlpha = 0.75f;
    float oceanDeepAlpha = 1.0f;
    float glow = 0.5f;  ///< field 4

    /// LightParams.dbc field 1 (HighlightSky): how strongly the dome's
    /// dawn and dusk highlight shows (stock_sky::highlight; 0x7f0530 reads it
    /// from DNLight [0x15]).
    float highlightSky = 0.0f;

    ColorBand colorBands[COLOR_CHANNEL_COUNT];

    // 6 float channels (FloatBand)
    enum FloatChannel {
        FOG_END = 0,
        FOG_START_SCALAR = 1,  // Multiplier for fog start
        // Channel 2 is not a density. Measured over every band in the file,
        // 98% of its 2509 samples are exactly 1.0 - it is the switch for how
        // much the sun and moon show through cloud, and reading it as cloud
        // density left almost every zone under solid overcast at all hours,
        // which is what hid the skybox.
        CELESTIAL_GLOW_THROUGH = 2,
        // Channel 3 is the curve that actually varies: 0 to 5, mean 0.50.
        CLOUD_DENSITY = 3,
        FOG_DENSITY = 3,
        // ... more channels
        FLOAT_CHANNEL_COUNT = 6
    };

    FloatBand floatBands[FLOAT_CHANNEL_COUNT];
};

/**
 * WoW DBC-driven lighting manager
 *
 * Implements WotLK's time-of-day lighting system:
 * - Loads Light.dbc, LightParams.dbc, LightIntBand.dbc, LightFloatBand.dbc
 * - Samples lighting curves based on time-of-day
 * - Interpolates between keyframes
 * - Provides lighting parameters for rendering
 */
class LightingManager {
public:
    LightingManager();
    ~LightingManager();

    /**
     * Initialize lighting system and load DBCs
     */
    bool initialize(pipeline::AssetManager* assetManager);

    /**
     * Update lighting for current time and player position
     * @param playerPos Player world position
     * @param mapId Current map ID
     * @param gameTime Optional game time in hours (use -1 for real time)
     * @param stormIntensity 0..1: how far each light leans from its clear
     *        LightParams toward its storm set - the server's weather intensity
     * @param isUnderwater Whether the camera is under water (the underwater
     *        slots of Light.dbc)
     *
     * Note: WoW uses server-sent game time, not local PC time.
     * Pass gameTime from SMSG_LOGIN_SETTIMESPEED or similar.
     */
    void update(const glm::vec3& playerPos, uint32_t mapId, uint32_t zoneId,
                float gameTime = -1.0f,
                float stormIntensity = 0.0f, bool isUnderwater = false);

    /**
     * Get current lighting parameters
     */
    [[nodiscard]] const LightingParams& getLightingParams() const { return currentParams_; }

    /// While the player is dead or a ghost every light uses its death set,
    /// Light.dbc column 11 (LightParams 4 nearly everywhere: the blue ghost
    /// world under DeathSkybox). Cheap to call every frame.
    void setPlayerDead(bool dead) { playerDead_ = dead; }

    /// The liquid the camera is in (LiquidType.dbc id, 0 for none) and how
    /// far below its surface, in yards. Set before update(). Magma and slime
    /// name a light of their own (LiquidType field 10: Light 7 and 6), which
    /// replaces Light.dbc's while the camera is in them (0x7f3230); ocean
    /// darkens the fog, ambient and sun with depth, down to field 6's 30
    /// yards (fields 7-9: fog and ambient to half, sun to three quarters).
    void setCameraLiquid(uint32_t liquidTypeId, float depthBelowSurface) {
        cameraLiquidType_ = liquidTypeId;
        cameraLiquidDepth_ = depthBelowSurface;
    }
    [[nodiscard]] bool isPlayerDead() const { return playerDead_; }

    /**
     * Set whether player is indoors (disables outdoor lighting)
     */
    void setIndoors(bool indoors) { isIndoors_ = indoors; }
    /// A WMO's MOSB sky model while the camera is in one of its 0x40000
    /// groups, empty otherwise. The client puts it in sky slot 0 and empties
    /// slot 1 (0x79a870 -> 0x7f31c0); the dome is drawn as usual.
    void setWmoSkybox(std::string path);
    /// How far the distance fog is pulled toward the sky's horizon colour.
    /// 0 keeps LightParams' own fog colour, as stock does, and is the
    /// default; 1 is the sky itself. A Wowee option. See
    /// LightingManager::update.
    void setFogSkyBlend(float blend) { fogSkyBlend_ = blend; }
    /// the reference client's daylight on maps 0 and 1: its own keyframes for every
    /// light enhanced_lights.hpp matches, the averaged trend
    /// (enhanced_palette.hpp) for the rest.
    void setEnhancedDaylight(bool on) { enhancedDaylight_ = on; }
    /// Whether Enhanced's daylight is lighting the frame: the row on, on a map
    /// Enhanced re-authored (0 or 1), not under water.
    [[nodiscard]] bool isEnhancedActive() const { return enhancedActive_; }
    /// Whether the scene is HDR (RGBA16F, tone mapped): the Enhanced sun is
    /// then added to the ambient as light without holding the sum at one
    /// (enhanced_palette::linearLightDirect).
    void setHdrScene(bool on) { hdrScene_ = on; }
    /// How many of 3.3.5's lights have Enhanced keyframes (0 before initialize).
    [[nodiscard]] size_t enhancedLightCount() const { return enhancedProfiles_.size(); }
    /// Long view: the fog may end past the far clip (see update()).
    void setLongView(bool on) { longView_ = on; }
    /// The far field's reach (far_field::kReach), or 0: with the long view,
    /// the fog ends at kLongViewFogReach times the farther of it and the
    /// view distance - Enhanced's 18000 over its 7000-yard horizonClip.
    void setLongViewReach(float yards) { longViewReach_ = yards; }
    /// Enhanced's fog end over its far clip: 18000 / 7000.
    static constexpr float kLongViewFogReach = 18000.0f / 7000.0f;
    [[nodiscard]] float getFogSkyBlend() const { return fogSkyBlend_; }
    /// How much distance fog, as a multiplier on the zone's own fog distances.
    /// 1 is the DBC unchanged (stock, and the default), above 1 is thicker,
    /// 0 is none. A Wowee option. See LightingManager::update.
    void setFogStrength(float strength) { fogStrength_ = strength; }
    [[nodiscard]] float getFogStrength() const { return fogStrength_; }
    /// The view distance setting, in yards: the stock client's far clip. The
    /// fog always ends by it (0x7f16f0), and on maps from 530 on it is where
    /// the fog ends (stock_light::lightFog). See LightingManager::update.
    void setViewDistance(float yards) { viewDistance_ = yards; }
    /// The view distance setting's default (ui::kDefaultViewDistance, checked
    /// against it in settings_panel.cpp).
    static constexpr float kFogReferenceViewDistance = 1900.0f;

    /**
     * Get current time of day (0.0-1.0)
     */
    [[nodiscard]] float getTimeOfDay() const { return timeOfDay_; }

    /** Time used by the visible sky, including persistent zone ambience. */
    [[nodiscard]] float getVisualTimeOfDayHours() const { return visualTimeOfDayHours_; }

    /// One of the original client's sky models, and how much of it is up.
    struct SkyboxLayer {
        std::string path;
        float weight = 0.0f;   ///< 0..1: the share of the lights here that name it
        /// The night stars (Environments\Stars\Stars.mdx, LightSkybox 4)
        /// the stock client draws under its sky dome, at stars.mdl's own
        /// table (stock_sky::starsAlpha: full to 03:00, gone by 04:30, back
        /// from 22:30). Not a zone sky: it does not stand in for the sun,
        /// moon and clouds the way a LightSkybox model does.
        bool stars = false;
        /// A sky model at full weight (above 0.99) whose LightSkybox flags
        /// lack bit 2: it takes the place of the whole procedural sky - dome,
        /// sun, moons, clouds and stars (0x7f09b0).
        bool replacesSky = false;
    };
    /// Every sky model the lights around the player name, heaviest first, at
    /// the weights those lights have - the same falloff the sky colours blend
    /// by, smoothed the same way. A zone's sky fades out across its edge as
    /// the next one's fades in, rather than one swapping for the other.
    [[nodiscard]] const std::vector<SkyboxLayer>& getSkyboxLayers() const { return skyboxLayers_; }

    /**
     * Manually set time of day for testing
     */
    void setTimeOfDay(float tod) { timeOfDay_ = tod; manualTime_ = true; }

    /**
     * Use real time for day/night cycle
     */
    void useRealTime(bool use) { manualTime_ = !use; }

private:
    /**
     * Load Light.dbc
     */
    bool loadLightDbc(pipeline::AssetManager* assetManager);

    /**
     * Load LightParams.dbc for zone→light mapping
     */
    bool loadLightParamsDbc(pipeline::AssetManager* assetManager);

    bool loadLightSkyboxDbc(pipeline::AssetManager* assetManager);
    bool loadLiquidTypeDbc(pipeline::AssetManager* assetManager);

    /**
     * Load LightIntBand.dbc and LightFloatBand.dbc for time curves
     */
    bool loadLightBandDbcs(pipeline::AssetManager* assetManager);

    /**
     * Weighted light volume for blending
     */
    struct WeightedVolume {
        const LightVolume* volume = nullptr;
        float weight = 0.0f;
    };

    /**
     * The lights around the player in the order the stock client paints them
     * (0x7f1360): the map's default light first at weight 1, then every
     * other light in range, farthest first, each with 0x7ee5d0's linear
     * weight. Each is painted over the result so far, so the nearest ends on
     * top.
     */
    [[nodiscard]] std::vector<WeightedVolume> findLightVolumes(const glm::vec3& playerPos, uint32_t mapId) const;

    /**
     * The LightParams row a volume uses for one of its slots, falling back to
     * the clear set where a slot is empty or names a missing row.
     */
    uint32_t paramsIdForSlot(const LightVolume* volume, LightVolume::ParamsSlot slot) const;

    /**
     * One volume's lighting: the clear (or underwater) set blended toward its
     * storm set by the weather intensity, or the death set while dead.
     * False if the volume names no profile that exists.
     */
    bool sampleVolume(const LightVolume* volume, uint16_t timeHalfMinutes,
                      float stormIntensity, bool isUnderwater, LightingParams& out,
                      bool* fromEnhanced = nullptr) const;

    /// The profile a volume's slot is sampled from: Enhanced's keyframes for
    /// it while the Enhanced daylight is on over map 0 or 1 and one exists,
    /// else the slot's own LightParams (paramsIdForSlot). Null if neither.
    const LightParamsProfile* profileForSlot(const LightVolume* volume,
                                             LightVolume::ParamsSlot slot,
                                             bool* fromEnhanced = nullptr) const;

    /// enhanced_lights.hpp's table, read from the client data's override
    /// folder, as profiles built over the stock ones (channels Enhanced leaves
    /// empty keep 3.3.5's bands). No file: none, and the trend everywhere.
    void loadEnhancedProfiles(pipeline::AssetManager* assetManager);

    /** world/dnc.db: the day and night intensity keys, by time of day. */
    bool loadDayNightCycle(pipeline::AssetManager* assetManager);
    void sampleDayNight(uint16_t timeHalfMinutes, float& day, float& night) const;

    /**
     * Sample lighting from LightParams profile
     */
    LightingParams sampleLightParams(const LightParamsProfile* profile, uint16_t timeHalfMinutes) const;

    /**
     * Sample color from band
     */
    [[nodiscard]] glm::vec3 sampleColorBand(const ColorBand& band, uint16_t timeHalfMinutes) const;

    /**
     * Sample float from band
     */
    [[nodiscard]] float sampleFloatBand(const FloatBand& band, uint16_t timeHalfMinutes) const;

    /**
     * Convert DBC BGR color to RGB vec3
     */
    [[nodiscard]] glm::vec3 dbcColorToVec3(uint32_t dbcColor) const;


    // Light volumes by map
    std::map<uint32_t, std::vector<LightVolume>> lightVolumesByMap_;

    // LightParams profiles by ID
    std::map<uint32_t, LightParamsProfile> lightParamsProfiles_;
    std::map<uint32_t, std::string> lightSkyboxPaths_;
    std::map<uint32_t, uint32_t> lightSkyboxFlags_;
    /// Light 1, under any map without a default light of its own.
    const LightVolume* globalDefaultLight_ = nullptr;
    /// Every light by its Light.dbc id, for LiquidType's lights.
    std::map<uint32_t, const LightVolume*> lightsById_;
    /// LiquidType.dbc's underwater fields (0x7f3230 reads +0x18..+0x28).
    struct LiquidLight {
        float maxDarkenDepth = 0.0f;
        float fogDarken = 0.0f;
        float ambientDarken = 0.0f;
        float diffuseDarken = 0.0f;
        uint32_t lightId = 0;
    };
    std::map<uint32_t, LiquidLight> liquidLights_;
    uint32_t cameraLiquidType_ = 0;
    float cameraLiquidDepth_ = 0.0f;

    // Current state
    LightingParams currentParams_;
    std::vector<WeightedVolume> activeVolumes_;
    glm::vec3 currentPlayerPos_{0.0f};
    float timeOfDay_ = 0.5f;  // Start at noon
    float visualTimeOfDayHours_ = 12.0f;
    std::vector<SkyboxLayer> skyboxLayers_;
    /// When update last ran, for smoothing by real time rather than by frame.
    std::chrono::steady_clock::time_point lastUpdate_{};
    bool isIndoors_ = false;
    std::string wmoSkybox_;
    bool playerDead_ = false;
    float fogSkyBlend_ = 0.0f;
    bool enhancedDaylight_ = false;
    bool hdrScene_ = false;
    /// enhancedDaylight_ and the map being drawn is one Enhanced re-authored.
    bool enhancedActive_ = false;
    /// Enhanced's keyframes by 3.3.5 light id * 8 + slot (loadEnhancedProfiles).
    std::map<uint32_t, LightParamsProfile> enhancedProfiles_;
    bool longView_ = false;
    float longViewReach_ = 0.0f;
    float fogStrength_ = 1.0f;
    /// stock_light::usesFarClipFog for the map being drawn; read by
    /// sampleLightParams, which shapes each light's fog the way 0x7ecd80
    /// does before the lights are blended.
    bool farClipFog_ = false;
    /// The weather grade, capped at a quarter and eased at a quarter per ten
    /// seconds, as the stock weather hands it to DayNight+0x4c (0x7849f4).
    float stormEased_ = 0.0f;
    float viewDistance_ = kFogReferenceViewDistance;

    /// dnc.db's keys: half-minutes since midnight, day and night intensity.
    struct DayNightKey {
        uint16_t time = 0;
        float day = 0.0f;
        float night = 0.0f;
    };
    std::vector<DayNightKey> dayNightKeys_;

    // Last values the sky diagnostic reported, so it prints on a change
    // rather than every frame. See LightingManager::update.
    /// The map the volume list was last named for, so it is named once per map
    /// rather than once per frame. findLightVolumes is const.
    mutable uint32_t diagLoggedMapId_ = 0xFFFFFFFFu;
    uint32_t diagZoneId_ = 0xFFFFFFFFu;
    uint32_t diagCallsSinceLog_ = 0;
    uint32_t diagFirstVolume_ = 0xFFFFFFFFu;
    uint32_t diagSecondVolume_ = 0xFFFFFFFFu;
    float diagVisualHours_ = -1.0f;
    float diagSkyLuma_ = -1.0f;
    std::string diagSkyboxPath_ = "\x01";
    bool manualTime_ = false;
    bool initialized_ = false;

    // Fallback lighting
    LightingParams fallbackParams_;
};

} // namespace rendering
} // namespace wowee
