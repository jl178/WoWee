#include "core/local_time.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/enhanced_palette.hpp"
#include "rendering/enhanced_lights.hpp"
#include "rendering/light_coords.hpp"
#include "rendering/light_band_block.hpp"
#include "rendering/stock_day_night.hpp"
#include "rendering/stock_light_math.hpp"
#include "rendering/stock_sky.hpp"
#include <glm/gtc/constants.hpp>
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <type_traits>

namespace wowee {
namespace rendering {


// WoW's Light.dbc stores time-of-day as half-minutes (0..2879).
// 24 hours × 60 minutes × 2 = 2880 half-minute ticks per day cycle.
constexpr uint16_t kHalfMinutesPerDay = 2880;


namespace {

/// Every field of LightingParams that is blended between lights, between a
/// clear and a storm set, and smoothed over time - all of them but the sun
/// direction (normalised separately) and dnc.db's day and night (the same
/// everywhere). One list, so a field added to the struct is added once.
template <typename Params, typename F>
void forEachBlended(Params& a, const LightingParams& b, F&& f) {
    f(a.ambientColor, b.ambientColor);
    f(a.diffuseColor, b.diffuseColor);
    f(a.fogColor, b.fogColor);
    f(a.fogStart, b.fogStart);
    f(a.fogEnd, b.fogEnd);
    f(a.fogDensity, b.fogDensity);
    f(a.fogScaler, b.fogScaler);
    f(a.fogExponent, b.fogExponent);
    f(a.skyTopColor, b.skyTopColor);
    f(a.skyMiddleColor, b.skyMiddleColor);
    f(a.skyBand1Color, b.skyBand1Color);
    f(a.skyBand2Color, b.skyBand2Color);
    f(a.cloudDensity, b.cloudDensity);
    f(a.horizonGlow, b.horizonGlow);
    f(a.skySmogColor, b.skySmogColor);
    f(a.skyFogColor, b.skyFogColor);
    f(a.sunColor, b.sunColor);
    f(a.cloudSunColor, b.cloudSunColor);
    f(a.cloudEmissiveColor, b.cloudEmissiveColor);
    f(a.cloudLayer1AmbientColor, b.cloudLayer1AmbientColor);
    f(a.cloudLayer2AmbientColor, b.cloudLayer2AmbientColor);
    f(a.celestialGlowThrough, b.celestialGlowThrough);
    f(a.oceanCloseColor, b.oceanCloseColor);
    f(a.oceanFarColor, b.oceanFarColor);
    f(a.riverCloseColor, b.riverCloseColor);
    f(a.riverFarColor, b.riverFarColor);
    f(a.waterShallowAlpha, b.waterShallowAlpha);
    f(a.waterDeepAlpha, b.waterDeepAlpha);
    f(a.oceanShallowAlpha, b.oceanShallowAlpha);
    f(a.oceanDeepAlpha, b.oceanDeepAlpha);
    f(a.shadowOpacity, b.shadowOpacity);
    f(a.glow, b.glow);
    f(a.airDensity, b.airDensity);
}

}  // namespace

LightingManager::LightingManager() {
    // Set fallback lighting (Elwynn Forest-ish outdoor daytime)
    fallbackParams_.ambientColor = glm::vec3(0.5f, 0.5f, 0.6f);
    fallbackParams_.diffuseColor = glm::vec3(1.0f, 0.95f, 0.85f);
    fallbackParams_.directionalDir = glm::normalize(glm::vec3(0.3f, -0.7f, 0.6f));
    fallbackParams_.fogColor = glm::vec3(0.6f, 0.7f, 0.85f);
    fallbackParams_.fogStart = 300.0f;
    fallbackParams_.fogEnd = 1500.0f;
    fallbackParams_.skyTopColor = glm::vec3(0.4f, 0.6f, 0.9f);
    fallbackParams_.skyMiddleColor = glm::vec3(0.6f, 0.75f, 0.95f);

    currentParams_ = fallbackParams_;
}

LightingManager::~LightingManager() {
}

bool LightingManager::initialize(pipeline::AssetManager* assetManager) {
    if (!assetManager) {
        LOG_ERROR("LightingManager::initialize: null AssetManager");
        return false;
    }


    // Load DBCs (non-fatal if missing, will use fallback lighting)
    loadLightDbc(assetManager);
    loadLightParamsDbc(assetManager);
    loadLightSkyboxDbc(assetManager);
    loadLiquidTypeDbc(assetManager);
    loadLightBandDbcs(assetManager);
    loadDayNightCycle(assetManager);
    loadEnhancedProfiles(assetManager);

    initialized_ = true;
    LOG_INFO("LightingManager initialized: ", lightVolumesByMap_.size(), " maps with lighting");
    return true;
}

bool LightingManager::loadLightDbc(pipeline::AssetManager* assetManager) {
    auto dbcData = assetManager->readFile("DBFilesClient\\Light.dbc");
    if (dbcData.empty()) {
        LOG_WARNING("Light.dbc not found, using fallback lighting");
        return false;
    }

    auto dbc = std::make_unique<pipeline::DBCFile>();
    if (!dbc->load(dbcData)) {
        LOG_ERROR("Failed to load Light.dbc");
        return false;
    }

    uint32_t recordCount = dbc->getRecordCount();
    LOG_INFO("Loading Light.dbc: ", recordCount, " light volumes");

    // Parse light volumes
    // Light.dbc structure (WotLK 3.3.5a):
    // 0: uint32 ID
    // 1: uint32 MapID
    // 2-4: float X, Z, Y (note: z and y swapped!)
    // 5: float FalloffStart (inner radius)
    // 6: float FalloffEnd (outer radius)
    // 7-11: uint32 LightParamsID - clear, clear underwater, storm,
    //       storm underwater, death
    // 12-14: further slots, all but empty in 3.3.5

    const auto* activeLayout = pipeline::getActiveDBCLayout();
    const auto* lL = activeLayout ? activeLayout->getLayout("Light") : nullptr;

    for (uint32_t i = 0; i < recordCount; ++i) {
        LightVolume volume;
        volume.lightId = dbc->getUInt32(i, lL ? (*lL)["ID"] : 0);
        volume.mapId = dbc->getUInt32(i, lL ? (*lL)["MapID"] : 1);

        // Into world space: thirty-sixths of a yard on the tile grid's axes,
        // which are mirrored and swapped against the world's. Checked against
        // six zones whose world coordinates are known - Tirisfal, Undercity,
        // Stormwind, Ironforge, Westfall and Booty Bay - each of which lands
        // inside a volume only under this mapping.
        const float dbcX = dbc->getFloat(i, lL ? (*lL)["X"] : 2);
        const float dbcZ = dbc->getFloat(i, lL ? (*lL)["Z"] : 3);
        const float dbcY = dbc->getFloat(i, lL ? (*lL)["Y"] : 4);
        volume.position = lightPositionToWorld(dbcX, dbcY, dbcZ);
        // The map's default light sits at the file's 0,0,0, before the move
        // to world space (0x7eaf60 leaves such a row unconverted and 0x7ecb30
        // takes it as the map's default).
        volume.isMapDefault = dbcX == 0.0f && dbcY == 0.0f && dbcZ == 0.0f;

        volume.innerRadius =
            dbc->getFloat(i, lL ? (*lL)["InnerRadius"] : 5) / LIGHT_COORD_UNITS_PER_YARD;
        volume.outerRadius =
            dbc->getFloat(i, lL ? (*lL)["OuterRadius"] : 6) / LIGHT_COORD_UNITS_PER_YARD;

        // The five LightParams slots, consecutive from the first: clear,
        // clear underwater, storm, storm underwater, death (see LightVolume).
        // Addressed from the layout's first slot rather than by the names it
        // gives columns 8 and 9 ("Rain", "Underwater"), which were swapped
        // against the file: underwater got the storm set and rain the
        // underwater one.
        const uint32_t firstSlot = lL ? (*lL)["LightParamsID"] : 7;
        for (uint32_t s = 0; s < LightVolume::SLOT_COUNT; ++s) {
            if (firstSlot + s >= dbc->getFieldCount()) break;
            volume.lightParamsIds[s] = dbc->getUInt32(i, firstSlot + s);
        }

        // Add to map-specific list
        lightVolumesByMap_[volume.mapId].push_back(volume);
    }
    // Light 1, the default for a map with no light of its own (0x7ecb30).
    globalDefaultLight_ = nullptr;
    lightsById_.clear();
    for (const auto& [map, list] : lightVolumesByMap_) {
        for (const auto& v : list) {
            lightsById_[v.lightId] = &v;
            if (v.lightId == 1) globalDefaultLight_ = &v;
        }
    }

    LOG_INFO("Loaded ", lightVolumesByMap_.size(), " maps with lighting volumes");
    return true;
}

bool LightingManager::loadLightParamsDbc(pipeline::AssetManager* assetManager) {
    auto dbcData = assetManager->readFile("DBFilesClient\\LightParams.dbc");
    if (dbcData.empty()) {
        LOG_WARNING("LightParams.dbc not found");
        return false;
    }

    auto dbc = std::make_unique<pipeline::DBCFile>();
    if (!dbc->load(dbcData)) {
        LOG_ERROR("Failed to load LightParams.dbc");
        return false;
    }

    uint32_t recordCount = dbc->getRecordCount();
    LOG_INFO("Loaded LightParams.dbc: ", recordCount, " profiles");

    // Create profile entries (will be populated by band loading)
    const auto* lpL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("LightParams") : nullptr;
    for (uint32_t i = 0; i < recordCount; ++i) {
        uint32_t paramId = dbc->getUInt32(i, lpL ? (*lpL)["LightParamsID"] : 0);
        LightParamsProfile profile;
        profile.lightParamsId = paramId;
        if (dbc->getFieldCount() > 2) {
            profile.lightSkyboxId = dbc->getUInt32(i, 2);
            // Field 1, HighlightSky: an int the client turns into a float
            // (0x7ebff0, DNLight [0x15]).
            profile.highlightSky = static_cast<float>(dbc->getUInt32(i, 1));
        }
        // 3.3.5: ID, HighlightSky, LightSkyboxID, CloudTypeID, Glow,
        // WaterShallowAlpha, WaterDeepAlpha, OceanShallowAlpha, OceanDeepAlpha.
        if (dbc->getFieldCount() >= 9) {
            profile.waterShallowAlpha = dbc->getFloat(i, 5);
            profile.waterDeepAlpha = dbc->getFloat(i, 6);
            profile.oceanShallowAlpha = dbc->getFloat(i, 7);
            profile.oceanDeepAlpha = dbc->getFloat(i, 8);
            profile.glow = dbc->getFloat(i, 4);
        }
        lightParamsProfiles_[paramId] = profile;
    }

    return true;
}

void LightingManager::loadEnhancedProfiles(pipeline::AssetManager* assetManager) {
    namespace fl = enhanced_lights;
    enhancedProfiles_.clear();
    fl::Table table;
    const std::vector<uint8_t> data = assetManager->readFileOptional(fl::kFileName);
    if (data.empty()) {
        LOG_INFO("Enhanced light keyframes: no <data>/override/wowee/enhanced_lights.bin "
                 "(tools/enhanced_light_gen.py writes it); the Enhanced daylight uses "
                 "the averaged trend everywhere");
        return;
    }
    if (!fl::parse(data, table)) {
        LOG_WARNING("Enhanced light keyframes: wowee/enhanced_lights.bin is not a valid "
                    "table; the Enhanced daylight uses the averaged trend everywhere");
        return;
    }
    for (const fl::Link& link : table.links) {
        auto vit = lightsById_.find(link.lightId);
        if (vit == lightsById_.end() || link.slot >= LightVolume::SLOT_COUNT) continue;
        // Over the slot's own profile: its skybox, water alphas and every
        // band Enhanced does not author stay 3.3.5's.
        LightParamsProfile profile;
        const uint32_t stockId =
            paramsIdForSlot(vit->second, static_cast<LightVolume::ParamsSlot>(link.slot));
        if (auto pit = lightParamsProfiles_.find(stockId); pit != lightParamsProfiles_.end()) {
            profile = pit->second;
        }
        // Less of the stock full-screen glow: Enhanced's own is 0 on almost
        // every profile (enhanced_palette::kGlowScale).
        profile.glow *= enhanced_palette::glowScale();
        const fl::Profile& src = table.profiles[link.profile];
        const fl::Key* keys = &table.keys[src.firstKey];
        const uint8_t n = static_cast<uint8_t>(std::min(src.keyCount, fl::kMaxKeys));
        for (uint32_t c = 0; c < fl::kColorChannels; ++c) {
            if (!(src.colorMask & (1u << c))) continue;
            ColorBand& band = profile.colorBands[c];
            band.numKeyframes = n;
            for (uint8_t k = 0; k < n; ++k) {
                // The sky's colours without Enhanced's exposure
                // (fl::kSkyExposure): 3.3.5's 8-bit sky has no tone map to
                // take it back.
                const uint32_t col = fl::isSkyChannel(c) ? fl::withoutSkyExposure(keys[k].color[c])
                                                         : keys[k].color[c];
                band.times[k] = static_cast<uint16_t>(keys[k].time);
                band.packed[k] = col;
                band.colors[k] = dbcColorToVec3(col);
            }
        }
        for (uint32_t f = 0; f < fl::kFloatChannels; ++f) {
            if (!(src.floatMask & (1u << f))) continue;
            FloatBand& band = profile.floatBands[f];
            band.numKeyframes = 0;
            for (uint8_t k = 0; k < n; ++k) {
                // A key without a fog end (0) is left out of that band.
                if (f == LightParamsProfile::FOG_END && keys[k].value[f] == 0.0f) continue;
                band.times[band.numKeyframes] = static_cast<uint16_t>(keys[k].time);
                band.values[band.numKeyframes] = keys[k].value[f];
                ++band.numKeyframes;
            }
        }
        // The air (enhanced_lights.hpp), Enhanced's global fog layer.
        profile.airDensity = src.airDensity;
        enhancedProfiles_[link.lightId * 8u + link.slot] = profile;
    }
    LOG_INFO("Enhanced light keyframes: ", enhancedProfiles_.size(), " light slots from wowee/enhanced_lights.bin");
}

const LightParamsProfile* LightingManager::profileForSlot(const LightVolume* volume,
                                                          LightVolume::ParamsSlot slot,
                                                          bool* fromEnhanced) const {
    if (fromEnhanced) *fromEnhanced = false;
    if (!volume) return nullptr;
    if (enhancedActive_) {
        auto fit = enhancedProfiles_.find(volume->lightId * 8u + slot);
        if (fit != enhancedProfiles_.end()) {
            if (fromEnhanced) *fromEnhanced = true;
            return &fit->second;
        }
        // A storm slot that falls back to the clear set falls back to
        // Enhanced's clear set, so rain does not bring 3.3.5's light back.
        if (slot == LightVolume::SLOT_STORM &&
            paramsIdForSlot(volume, slot) == paramsIdForSlot(volume, LightVolume::SLOT_CLEAR)) {
            return profileForSlot(volume, LightVolume::SLOT_CLEAR, fromEnhanced);
        }
    }
    auto it = lightParamsProfiles_.find(paramsIdForSlot(volume, slot));
    return it == lightParamsProfiles_.end() ? nullptr : &it->second;
}

void LightingManager::setWmoSkybox(std::string path) {
    if (path != wmoSkybox_) {
        LOG_INFO("WMO skybox (MOSB): ", path.empty() ? std::string("none") : path);
    }
    wmoSkybox_ = std::move(path);
}

bool LightingManager::loadLightSkyboxDbc(pipeline::AssetManager* assetManager) {
    auto dbcData = assetManager->readFile("DBFilesClient\\LightSkybox.dbc");
    if (dbcData.empty()) {
        LOG_WARNING("LightSkybox.dbc not found");
        return false;
    }

    pipeline::DBCFile dbc;
    if (!dbc.load(dbcData) || dbc.getFieldCount() < 2) {
        LOG_ERROR("Failed to load LightSkybox.dbc");
        return false;
    }

    lightSkyboxPaths_.clear();
    for (uint32_t i = 0; i < dbc.getRecordCount(); ++i) {
        const uint32_t id = dbc.getUInt32(i, 0);
        std::string path = dbc.getString(i, 1);
        // Field 2, the flags: bit 2 lets a model share the sky with the ones
        // listed before it even at full weight (0x7f3230).
        if (id != 0 && dbc.getFieldCount() > 2) lightSkyboxFlags_[id] = dbc.getUInt32(i, 2);
        if (id != 0 && !path.empty()) lightSkyboxPaths_[id] = std::move(path);
    }
    LOG_INFO("Loaded LightSkybox.dbc: ", lightSkyboxPaths_.size(), " model paths");
    return !lightSkyboxPaths_.empty();
}

bool LightingManager::loadLiquidTypeDbc(pipeline::AssetManager* assetManager) {
    // LiquidType.dbc (3.3.5, 45 fields): 6 MaxDarkenDepth, 7 FogDarkenIntensity,
    // 8 AmbDarkenIntensity, 9 DirDarkenIntensity, 10 LightID - the record's
    // +0x18..+0x28 that 0x7f3230 reads for the liquid around the camera.
    auto data = assetManager->readFile("DBFilesClient\\LiquidType.dbc");
    if (data.empty()) return false;
    pipeline::DBCFile dbc;
    if (!dbc.load(data) || dbc.getFieldCount() < 11) return false;
    liquidLights_.clear();
    for (uint32_t i = 0; i < dbc.getRecordCount(); ++i) {
        LiquidLight l;
        l.maxDarkenDepth = dbc.getFloat(i, 6);
        l.fogDarken = dbc.getFloat(i, 7);
        l.ambientDarken = dbc.getFloat(i, 8);
        l.diffuseDarken = dbc.getFloat(i, 9);
        l.lightId = dbc.getUInt32(i, 10);
        liquidLights_[dbc.getUInt32(i, 0)] = l;
    }
    return true;
}

bool LightingManager::loadLightBandDbcs(pipeline::AssetManager* assetManager) {
    // Load LightIntBand.dbc for RGB color curves (18 channels per LightParams)
    auto intBandData = assetManager->readFile("DBFilesClient\\LightIntBand.dbc");
    if (!intBandData.empty()) {
        auto dbc = std::make_unique<pipeline::DBCFile>();
        if (dbc->load(intBandData)) {
            LOG_INFO("Loaded LightIntBand.dbc: ", dbc->getRecordCount(), " color bands");

            // Parse int bands
            // Structure: ID, Entry (block index), NumValues, Time[16], Color[16]
            // Block index = LightParamsID * 18 + channel
            const auto* libL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("LightIntBand") : nullptr;
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                const uint32_t blockIndex =
                    dbc->getUInt32(i, libL ? (*libL)["BlockIndex"] : 0);
                const LightBandSlot slot =
                    lightBandSlot(blockIndex, LIGHT_INT_CHANNELS);
                if (!slot.valid) continue;

                auto it = lightParamsProfiles_.find(slot.lightParamsId);
                if (it == lightParamsProfiles_.end()) continue;

                const uint32_t channelIndex = slot.channel;
                if (channelIndex >= LightParamsProfile::COLOR_CHANNEL_COUNT) continue;

                ColorBand& band = it->second.colorBands[channelIndex];
                band.numKeyframes = dbc->getUInt32(i, libL ? (*libL)["NumKeyframes"] : 1);
                if (band.numKeyframes > 16) band.numKeyframes = 16;

                // Read time keys (field 3-18) - stored as uint16 half-minutes
                uint32_t timeKeyBase = libL ? (*libL)["TimeKey0"] : 2;
                for (uint8_t k = 0; k < band.numKeyframes && k < 16; ++k) {
                    uint32_t timeValue = dbc->getUInt32(i, timeKeyBase + k);
                    band.times[k] = static_cast<uint16_t>(timeValue % kHalfMinutesPerDay);  // Clamp to valid range
                }

                // Read color values (field 19-34) - stored as BGRA packed uint32
                uint32_t valueBase = libL ? (*libL)["Value0"] : 18;
                for (uint8_t k = 0; k < band.numKeyframes && k < 16; ++k) {
                    uint32_t colorBGRA = dbc->getUInt32(i, valueBase + k);
                    band.colors[k] = dbcColorToVec3(colorBGRA);
                    band.packed[k] = colorBGRA & 0x00FFFFFFu;
                }
            }
        }
    }

    // Load LightFloatBand.dbc for fog/intensity curves (6 channels per LightParams)
    auto floatBandData = assetManager->readFile("DBFilesClient\\LightFloatBand.dbc");
    if (!floatBandData.empty()) {
        auto dbc = std::make_unique<pipeline::DBCFile>();
        if (dbc->load(floatBandData)) {
            LOG_INFO("Loaded LightFloatBand.dbc: ", dbc->getRecordCount(), " float bands");

            // Parse float bands
            // Structure: ID, Entry (block index), NumValues, Time[16], Value[16]
            // Block index = LightParamsID * 6 + channel
            const auto* lfbL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("LightFloatBand") : nullptr;
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                const uint32_t blockIndex =
                    dbc->getUInt32(i, lfbL ? (*lfbL)["BlockIndex"] : 0);
                const LightBandSlot slot =
                    lightBandSlot(blockIndex, LIGHT_FLOAT_CHANNELS);
                if (!slot.valid) continue;

                auto it = lightParamsProfiles_.find(slot.lightParamsId);
                if (it == lightParamsProfiles_.end()) continue;

                const uint32_t channelIndex = slot.channel;
                if (channelIndex >= LightParamsProfile::FLOAT_CHANNEL_COUNT) continue;

                FloatBand& band = it->second.floatBands[channelIndex];
                band.numKeyframes = dbc->getUInt32(i, lfbL ? (*lfbL)["NumKeyframes"] : 1);
                if (band.numKeyframes > 16) band.numKeyframes = 16;

                // Read time keys (field 3-18)
                uint32_t timeKeyBase = lfbL ? (*lfbL)["TimeKey0"] : 2;
                for (uint8_t k = 0; k < band.numKeyframes && k < 16; ++k) {
                    uint32_t timeValue = dbc->getUInt32(i, timeKeyBase + k);
                    band.times[k] = static_cast<uint16_t>(timeValue % kHalfMinutesPerDay);  // Clamp to valid range
                }

                // Read float values (field 19-34)
                uint32_t valueBase = lfbL ? (*lfbL)["Value0"] : 18;
                for (uint8_t k = 0; k < band.numKeyframes && k < 16; ++k) {
                    band.values[k] = dbc->getFloat(i, valueBase + k);
                }
            }
        }
    }

    LOG_INFO("Loaded bands for ", lightParamsProfiles_.size(), " LightParams profiles");
    return true;
}

void LightingManager::update(const glm::vec3& playerPos, uint32_t mapId, uint32_t zoneId,
                              float gameTime,
                              float stormIntensity, bool isUnderwater) {
    if (!initialized_) return;
    stormIntensity = glm::clamp(stormIntensity, 0.0f, 1.0f);

    // Update time
    if (!manualTime_) {
        if (gameTime >= 0.0f) {
            // Server-sent game time, in hours since midnight. It was being
            // divided by 86400 as though it were seconds, against a field that
            // held a raw packed bitfield - so the sky's clock was a number with
            // no relation to the hour, and it looked plausible because any
            // value lands somewhere in the day.
            timeOfDay_ = std::fmod(gameTime / 24.0f, 1.0f);  // 0.0-1.0
        } else {
            // Fallback: use real time for day/night cycle
            std::time_t now = std::time(nullptr);
            const std::tm localTime = core::localTime(now);
            float secondsSinceMidnight = localTime.tm_hour * 3600.0f +
                                          localTime.tm_min * 60.0f +
                                          localTime.tm_sec;
            timeOfDay_ = secondsSinceMidnight / 86400.0f;  // 0.0-1.0
        }
    }
    // else: manualTime_ is set, use timeOfDay_ as-is

    // The world clock, in every zone. Duskwood, Tirisfal and Silverpine are
    // dark because their Light.dbc rows are; the stock client neither pins
    // Duskwood's hour nor paints over those zones' sky colours, so the
    // zone_ambience overrides (resolveZoneVisualTimeHours,
    // applyZoneAmbienceOverride) are no longer applied here.
    visualTimeOfDayHours_ = timeOfDay_ * 24.0f;

    // The band key, as 0x7f3230 takes it: round(dayFraction * 2880 - 0.5).
    const float visualDayFraction = visualTimeOfDayHours_ / 24.0f;
    const uint16_t timeHalfMinutes =
        static_cast<uint16_t>(stock_light::bandTime(visualDayFraction));
    // Unrounded, for the sun and light directions: the stock client moves
    // them continuously rather than a half-minute at a time.
    const float timeHalfMinutesExact = visualDayFraction * static_cast<float>(kHalfMinutesPerDay);

    currentPlayerPos_ = playerPos;
    farClipFog_ = stock_light::usesFarClipFog(mapId);

    // The weather's pull toward the storm sets. The stock weather caps the
    // grade at a quarter and eases toward it at a quarter per ten seconds
    // (0x7846a0, 0x784850: (|change| * 4 + 0.001) * 10 s), and DayNight takes
    // four times that (0x7f3920: DayNight+0x88 = +0x4c * 4): a quarter-grade
    // rain is already the whole storm light, reached ten seconds after a
    // clear sky.
    const auto now = std::chrono::steady_clock::now();
    float deltaTime = 0.016f;
    if (lastUpdate_.time_since_epoch().count() != 0) {
        deltaTime = std::clamp(std::chrono::duration<float>(now - lastUpdate_).count(),
                               0.0f, 0.25f);
    }
    lastUpdate_ = now;
    {
        const float target = std::min(stormIntensity, 0.25f);
        const float step = 0.025f * deltaTime;
        if (std::abs(target - stormEased_) <= step) stormEased_ = target;
        else stormEased_ += target > stormEased_ ? step : -step;
    }
    const float storm = glm::clamp(stormEased_ * 4.0f, 0.0f, 1.0f);

    // The lights around the player, in paint order.
    activeVolumes_ = findLightVolumes(playerPos, mapId);

    // dnc.db's day and night, the same everywhere: they say when the sun,
    // and the moon and stars, are up.
    float dayIntensity = 1.0f, nightIntensity = 0.0f;
    sampleDayNight(timeHalfMinutes, dayIntensity, nightIntensity);

    // The sky models overhead, as 0x7ed4c0 collects them while it paints the
    // lights: each light adds its LightSkybox at the weight it was painted
    // with (summed, capped at one, if the model is already listed), up to
    // three; the default light's at full weight. Then 0x7f3230 walks the
    // three: a model at full weight (above 0.99) whose LightSkybox flags lack
    // bit 2 starts the list again, so it replaces the ones before it rather
    // than sitting over them.
    struct SkyEntry { uint32_t id; float weight; };
    std::vector<SkyEntry> skyEntries;
    auto addSky = [&](uint32_t skyboxId, float w) {
        if (skyboxId == 0 || w <= 0.0f) return;
        for (auto& e : skyEntries) {
            if (e.id == skyboxId) { e.weight = std::min(e.weight + w, 1.0f); return; }
        }
        if (skyEntries.size() < 3) skyEntries.push_back({skyboxId, w});
    };
    auto skyboxOf = [&](uint32_t paramsId) -> uint32_t {
        auto it = lightParamsProfiles_.find(paramsId);
        return it == lightParamsProfiles_.end() ? 0u : it->second.lightSkyboxId;
    };

    // The liquid around the camera (0x7f3230): magma and slime name a light
    // of their own, which is all that shows while the camera is in them.
    const LiquidLight* liquid = nullptr;
    if (isUnderwater && cameraLiquidType_ != 0) {
        auto lit = liquidLights_.find(cameraLiquidType_);
        if (lit != liquidLights_.end()) liquid = &lit->second;
    }

    // Enhanced daylight (not stock): on the two continents, not under water,
    // each light Enhanced re-authored is sampled from Enhanced's own keyframes
    // (enhanced_lights.hpp, profileForSlot); every other light takes the
    // averaged trend (enhanced_palette.hpp) by day, before the lights are
    // blended. The sky's fog band takes the fog colour's trend; the fog
    // colour keeps Light.dbc's.
    enhancedActive_ = enhancedDaylight_ && !isUnderwater && enhanced_palette::appliesToMap(mapId);
    size_t enhancedVolumes = 0;
    auto enhancedTrend = [&](LightingParams& p) {
        if (!enhancedActive_) return;
        namespace fp = enhanced_palette;
        const float w = glm::clamp(dayIntensity, 0.0f, 1.0f);
        p.diffuseColor = fp::apply(p.diffuseColor, fp::kDirect, w);
        p.ambientColor = fp::apply(p.ambientColor, fp::kAmbient, w);
        p.skyTopColor = fp::apply(p.skyTopColor, fp::kSkyTop, w);
        p.skyMiddleColor = fp::apply(p.skyMiddleColor, fp::kSkyMiddle, w);
        p.skyFogColor = fp::apply(p.skyFogColor, fp::kSkyFog, w);
        p.sunColor = fp::apply(p.sunColor, fp::kSun, w);
    };

    LightingParams newParams;
    bool sampled = false;
    if (liquid && liquid->lightId != 0 && !playerDead_) {
        auto vit = lightsById_.find(liquid->lightId);
        if (vit != lightsById_.end()) {
            const uint32_t id = paramsIdForSlot(vit->second, LightVolume::SLOT_CLEAR);
            auto pit = lightParamsProfiles_.find(id);
            if (pit != lightParamsProfiles_.end()) {
                newParams = sampleLightParams(&pit->second, timeHalfMinutes);
                addSky(pit->second.lightSkyboxId, 1.0f);
                sampled = true;
            }
        }
    }
    if (!sampled && !isIndoors_ && !activeVolumes_.empty()) {
        // While the screen effect forces the death slot (a ghost) the stock
        // client shows the map default light's death set and nothing else
        // (0x7f3230, slot override 0xd38b58): no local lights, no storm.
        if (playerDead_) {
            const LightVolume* def = activeVolumes_.front().volume;
            const uint32_t id = paramsIdForSlot(def, LightVolume::SLOT_DEATH);
            auto it = lightParamsProfiles_.find(id);
            if (it != lightParamsProfiles_.end()) {
                newParams = sampleLightParams(&it->second, timeHalfMinutes);
                enhancedTrend(newParams);
                addSky(it->second.lightSkyboxId, 1.0f);
                sampled = true;
            }
        }
        if (!sampled) {
            for (size_t i = 0; i < activeVolumes_.size(); ++i) {
                const auto& wv = activeVolumes_[i];
                LightingParams params;
                bool fromEnhanced = false;
                if (!sampleVolume(wv.volume, timeHalfMinutes, storm, isUnderwater, params,
                                  &fromEnhanced)) {
                    continue;
                }
                if (fromEnhanced) {
                    ++enhancedVolumes;
                } else {
                    enhancedTrend(params);
                }
                const uint32_t clearId = paramsIdForSlot(
                    wv.volume, isUnderwater ? LightVolume::SLOT_CLEAR_UNDERWATER
                                            : LightVolume::SLOT_CLEAR);
                if (!sampled) {
                    // The default light, the canvas everything else is
                    // painted on.
                    newParams = params;
                    addSky(skyboxOf(clearId), 1.0f);
                    sampled = true;
                    continue;
                }
                // 0x7ed4c0: everything moves toward this light by its weight.
                const float w = wv.weight;
                forEachBlended(newParams, params,
                               [w](auto& x, const auto& y) { x = glm::mix(x, y, w); });
                addSky(skyboxOf(clearId), w);
            }
        }
    }
    if (!sampled) {
        // No light at all (no Light.dbc, or a map it does not cover and no
        // Light 1): the fixed lighting, by the hour.
        newParams = fallbackParams_;
        if (timeOfDay_ < 0.25f || timeOfDay_ > 0.75f) {
            float nightness = (timeOfDay_ < 0.25f) ? (0.25f - timeOfDay_) * 4.0f
                                                    : (timeOfDay_ - 0.75f) * 4.0f;
            newParams.ambientColor *= (0.3f + 0.7f * (1.0f - nightness));
            newParams.diffuseColor *= (0.2f + 0.8f * (1.0f - nightness));
            newParams.ambientColor.b += nightness * 0.1f;
        }
        newParams.fogScaler = newParams.fogEnd > 0.0f ? newParams.fogStart / newParams.fogEnd : 0.0f;
        newParams.fogExponent = 1.0f;
        newParams.skyFogColor = newParams.fogColor;
        enhancedTrend(newParams);
    }

    // Enhanced's colours are authored for a linear pipeline: the sun is the
    // one that, added to the ambient as light, gives what Enhanced's lit face
    // shows, instead of clipping to white on the 8-bit sum
    // (enhanced_palette::linearLightDirect, docs/re/enhanced-lighting.md 2c).
    if (enhancedActive_) {
        newParams.diffuseColor =
            enhanced_palette::linearLightDirect(newParams.ambientColor, newParams.diffuseColor, 1.0f,
                                               hdrScene_ ? glm::mix(1.0f, enhanced_palette::hdrSunIntensity(),
                                                                    glm::clamp(dayIntensity, 0.0f, 1.0f))
                                                         : 1.0f,
                                               hdrScene_);
    }

    // Light.dbc carries no direction: DayNight sets it from the hour alone,
    // the same in every light and on every map (0x7eea90).
    newParams.directionalDir = stockLightDirection(timeHalfMinutesExact);

    // Deeper is darker (0x7f3230, LiquidType fields 6-9): the ambient and
    // sun colours lose value by depth / MaxDarkenDepth times their intensity
    // - ocean at thirty yards and below keeps half its ambient and three
    // quarters of its sun. Scaling HSV value is scaling the three channels
    // alike. 0x7f3230 darkens the fog colour too (field 7), but 0x7f16f0
    // rebuilds the fog from the light's channel 7 straight after, so the fog
    // never shows it.
    if (liquid && liquid->maxDarkenDepth > 0.0f) {
        const float t = glm::clamp(cameraLiquidDepth_ / liquid->maxDarkenDepth, 0.0f, 1.0f);
        newParams.ambientColor *= std::max(0.0f, 1.0f - t * liquid->ambientDarken);
        newParams.diffuseColor *= std::max(0.0f, 1.0f - t * liquid->diffuseDarken);
    }

    // The frame's fog (0x7f16f0): it ends by the far clip - the view
    // distance - and starts at the blended fraction of that end.
    // The far clip the fog is held to, the long view's reach included (the
    // strength below keeps to it too).
    float fogClip = std::max(viewDistance_, 1.0f);
    {
        float& farClip = fogClip;
        // Long view (not stock; the Enhanced preset): Enhanced draws to a
        // 7000-yard far clip and its outdoor fog ends at 18000, so what is
        // drawn is never swallowed by a fog wall. Here the fog may end at
        // kLongViewFogReach times the view distance instead of at it, the
        // WDL horizon carries the land past the clip, and the Enhanced haze
        // gives the distance its air. Only on the maps whose fog is the
        // band's own (below 530), never under water.
        if (longView_ && !farClipFog_ && !isUnderwater)
            farClip = std::max(farClip, longViewReach_) * kLongViewFogReach;
        stock_light::LightFog blended;
        blended.end = newParams.fogEnd;
        blended.scaler = newParams.fogScaler;
        blended.exponent = newParams.fogExponent;
        const stock_light::Fog fog =
            stock_light::frameFog(blended, farClip, farClipFog_, isUnderwater);
        newParams.fogStart = fog.start;
        newParams.fogEnd = fog.end;
        newParams.fogExponent = fog.exponent;
    }

    // Two Wowee options over the stock fog, both off by default (blend 0,
    // strength 1) and neither applied under water, where the fog is the
    // water itself:
    //  - the sky blend pulls the fog colour toward the sky's horizon colour,
    //    for those who find a pale fog over a dark sky reads as a band;
    //  - the strength divides the fog distances: 2 is the same curve at half
    //    the distance, 0 is no fog at all.
    const bool authoredFogOnly = isUnderwater;
    if (!authoredFogOnly && fogSkyBlend_ > 0.0f) {
        newParams.fogColor = glm::mix(newParams.fogColor, newParams.skyMiddleColor,
                                      glm::clamp(fogSkyBlend_, 0.0f, 1.0f));
    }
    const float strength = authoredFogOnly ? 1.0f : glm::clamp(fogStrength_, 0.0f, 2.0f);
    if (strength <= 0.001f) {
        newParams.fogStart = 1.0e6f;
        newParams.fogEnd = 1.0e6f + 1.0f;
    } else if (std::abs(strength - 1.0f) > 0.001f) {
        const float scale = 1.0f / strength;
        newParams.fogStart *= scale;
        newParams.fogEnd = std::min(newParams.fogEnd * scale, fogClip);
    }


    // No headroom scaling of ambient + diffuse: the stock shaders clamp
    // ambient + diffuse * N.L at one per channel (terrain.bls and the M2 and
    // WMO vertex shaders), hue shift and all, and the lit shaders here do the
    // same (stock_light::terrainLight).

    // What the sky is being told, whenever it changes.
    //
    // Only on a change, so standing still is silent and a walk prints one line
    // per event rather than one per frame. INFO, so it costs nothing until
    // somebody asks for it with WOWEE_LOG_LEVEL=info.
    {
        const float skyLuma = 0.2126f * newParams.skyTopColor.r +
                              0.7152f * newParams.skyTopColor.g +
                              0.0722f * newParams.skyTopColor.b;
        const std::string topSkybox = skyboxLayers_.empty() ? std::string() : skyboxLayers_.front().path;
        uint32_t firstVolume = 0, secondVolume = 0;
        if (!activeVolumes_.empty()) firstVolume = activeVolumes_.back().volume->lightId;
        if (activeVolumes_.size() > 1) secondVolume = activeVolumes_[activeVolumes_.size() - 2].volume->lightId;
        const bool changed =
            zoneId != diagZoneId_ || topSkybox != diagSkyboxPath_ ||
            firstVolume != diagFirstVolume_ || secondVolume != diagSecondVolume_ ||
            std::abs(visualTimeOfDayHours_ - diagVisualHours_) > 0.02f ||
            std::abs(skyLuma - diagSkyLuma_) > 0.01f;
        ++diagCallsSinceLog_;
        if (changed && diagCallsSinceLog_ >= 30) {
            diagCallsSinceLog_ = 0;
            LOG_INFO("sky: zone=", zoneId, " hour=", visualTimeOfDayHours_,
                     " skyLuma=", skyLuma, " volumes=", firstVolume, "/", secondVolume,
                     " inRange=", activeVolumes_.size(), " enhanced=", enhancedVolumes,
                     " skybox=", topSkybox.empty() ? "-" : topSkybox);
            diagZoneId_ = zoneId;
            diagSkyboxPath_ = topSkybox;
            diagFirstVolume_ = firstVolume;
            diagSecondVolume_ = secondVolume;
            diagVisualHours_ = visualTimeOfDayHours_;
            diagSkyLuma_ = skyLuma;
        }
    }

    // No easing over time: the stock light is wherever the clock and the
    // player's position put it this frame. What moves smoothly does so
    // because the bands and the volume weights are continuous.
    currentParams_ = newParams;
    currentParams_.dayIntensity = dayIntensity;
    currentParams_.nightIntensity = nightIntensity;
    // Enhanced's sun climbs to 65 degrees, not 85, and rises and sets
    // earlier; its bearing and the light's direction are stock
    // (stock_day_night::kEnhancedSunPhi).
    currentParams_.sunDir = enhancedActive_ ? enhancedSunDirection(timeHalfMinutesExact)
                                           : stockSunDirection(timeHalfMinutesExact);
    // And Enhanced's sun lights the world from where it stands: its shadows
    // are cast from the sun (the road through Goldshire lies in the sun at
    // 9:22, under 3.3.5's lower north-west light the trees shade all of it).
    // By day only, eased in over the sun's first 9 degrees, so the night
    // keeps 3.3.5's moonlight direction (enhanced_palette::sunLightDirection).
    currentParams_.sunScale = 1.0f;
    if (enhancedActive_) {
        const glm::vec3 stockTravel = currentParams_.directionalDir;
        currentParams_.directionalDir =
            enhanced_palette::sunLightDirection(stockTravel, currentParams_.sunDir);
        currentParams_.sunScale =
            enhanced_palette::sunIrradianceScale(stockTravel, currentParams_.directionalDir);
    }

    // The sky model list (0x7f3230's last loop), then the night stars the
    // stock client draws wherever no sky model does: Stars.mdx, the
    // LightSkybox 4 model, which no Azeroth or Kalimdor LightParams row names.
    {
        static const std::string kStarsModel = "Environments\\Stars\\Stars.mdx";
        std::vector<SkyboxLayer> layers;
        if (!isIndoors_) {
            bool replaced = false;
            for (const auto& e : skyEntries) {
                auto skyIt = lightSkyboxPaths_.find(e.id);
                if (skyIt == lightSkyboxPaths_.end() || skyIt->second.empty()) continue;
                const auto flagsIt = lightSkyboxFlags_.find(e.id);
                const uint32_t flags = flagsIt == lightSkyboxFlags_.end() ? 0u : flagsIt->second;
                const bool full = e.weight > 0.99f && (flags & 2u) == 0;
                if (full && !layers.empty()) layers.clear();
                layers.push_back({.path = skyIt->second, .weight = e.weight, .replacesSky = full});
                replaced = replaced || full;
            }
            // The stars, by stars.mdl's own clock, unless a model has taken
            // the whole sky.
            const float starsWeight = replaced ? 0.0f : stock_sky::starsAlpha(visualDayFraction);
            if (starsWeight > 0.0f && !isUnderwater) {
                layers.push_back({.path = kStarsModel, .weight = starsWeight, .stars = true});
            }
        }
        std::sort(layers.begin(), layers.end(),
                  [](const SkyboxLayer& a, const SkyboxLayer& b) { return a.weight > b.weight; });
        // A WMO skybox (MOSB) takes slot 0 and empties slot 1; the stars and
        // any third model stay.
        if (!wmoSkybox_.empty()) {
            std::vector<SkyboxLayer> withWmo;
            withWmo.push_back({.path = wmoSkybox_, .weight = 1.0f});
            size_t models = 0;
            for (auto& l : layers) {
                if (l.stars) { withWmo.push_back(std::move(l)); continue; }
                if (++models >= 3) withWmo.push_back(std::move(l));
            }
            layers = std::move(withWmo);
        }
        skyboxLayers_ = std::move(layers);
    }
}

std::vector<LightingManager::WeightedVolume> LightingManager::findLightVolumes(const glm::vec3& playerPos, uint32_t mapId) const {
    std::vector<WeightedVolume> painted;
    auto it = lightVolumesByMap_.find(mapId);

    // The canvas: the map's own default light, or Light 1 (0x7ecb30).
    const LightVolume* def = nullptr;
    if (it != lightVolumesByMap_.end()) {
        for (const auto& v : it->second) {
            if (v.isMapDefault) { def = &v; break; }
        }
    }
    if (!def) def = globalDefaultLight_;
    if (!def) return painted;
    painted.push_back({.volume = def, .weight = 1.0f});
    if (it == lightVolumesByMap_.end()) return painted;

    // Every other light within its outer radius, if that radius is at least
    // three yards (0x7f1360), with 0x7ee5d0's linear weight.
    struct Candidate {
        WeightedVolume wv;
        stock_light::VolumeKey key;
    };
    std::vector<Candidate> candidates;
    for (const auto& volume : it->second) {
        if (&volume == def || volume.isMapDefault) continue;
        if (volume.outerRadius < stock_light::kMinVolumeOuterRadius) continue;
        const glm::vec3 toPlayer = playerPos - volume.position;
        const float distSq = glm::dot(toPlayer, toPlayer);
        if (distSq >= volume.outerRadius * volume.outerRadius) continue;
        const float w = stock_light::volumeWeight(std::sqrt(distSq), volume.innerRadius,
                                                  volume.outerRadius);
        candidates.push_back({.wv = {.volume = &volume, .weight = w},
                              .key = {.distanceSq = distSq,
                                      .innerRadius = volume.innerRadius,
                                      .centre = volume.position}});
    }
    // Farthest first, so the nearest is painted last and ends on top. The
    // light id breaks exact ties, so the order cannot flicker between frames.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (stock_light::paintedBefore(a.key, b.key)) return true;
        if (stock_light::paintedBefore(b.key, a.key)) return false;
        return a.wv.volume->lightId < b.wv.volume->lightId;
    });
    for (const auto& c : candidates) painted.push_back(c.wv);

    // Which volumes this map turned out to have around the player, once per
    // map (the logger collapses repeats only when they are identical, and a
    // walk changes the numbers every frame).
    if (mapId != diagLoggedMapId_) {
        diagLoggedMapId_ = mapId;
        std::string named;
        for (size_t i = 0; i < painted.size() && i < 4; ++i) {
            const LightVolume& v = *painted[i].volume;
            named += " [" + std::to_string(v.lightId) + " inner=" +
                     std::to_string(static_cast<int>(v.innerRadius)) + " outer=" +
                     std::to_string(static_cast<int>(v.outerRadius)) + "]";
        }
        LOG_INFO("Light volumes on map ", mapId, ": ", painted.size(),
                 " painted,", named.empty() ? " none" : named);
    }
    return painted;
}

uint32_t LightingManager::paramsIdForSlot(const LightVolume* volume,
                                          LightVolume::ParamsSlot slot) const {
    if (!volume) return 0;
    const uint32_t id = volume->lightParamsIds[slot];
    if (id != 0 && lightParamsProfiles_.count(id)) return id;
    // A storm underwater set falls back to the plain underwater one before
    // the clear set, so a dive in the rain stays under water.
    if (slot == LightVolume::SLOT_STORM_UNDERWATER) {
        return paramsIdForSlot(volume, LightVolume::SLOT_CLEAR_UNDERWATER);
    }
    return volume->lightParamsIds[LightVolume::SLOT_CLEAR];
}

bool LightingManager::sampleVolume(const LightVolume* volume, uint16_t timeHalfMinutes,
                                   float stormIntensity, bool isUnderwater,
                                   LightingParams& out, bool* fromEnhanced) const {
    if (fromEnhanced) *fromEnhanced = false;
    if (!volume) return false;
    auto sample = [&](const LightParamsProfile* profile, LightingParams& p) {
        if (!profile) return false;
        p = sampleLightParams(profile, timeHalfMinutes);
        return true;
    };

    if (playerDead_) {
        return sample(profileForSlot(volume, LightVolume::SLOT_DEATH), out);
    }

    const LightParamsProfile* clear = profileForSlot(
        volume, isUnderwater ? LightVolume::SLOT_CLEAR_UNDERWATER : LightVolume::SLOT_CLEAR,
        fromEnhanced);
    if (!sample(clear, out)) return false;

    // Toward the storm set by the weather's intensity: light rain greys the
    // light a little, a downpour all the way.
    const LightParamsProfile* stormProfile = profileForSlot(
        volume, isUnderwater ? LightVolume::SLOT_STORM_UNDERWATER : LightVolume::SLOT_STORM);
    if (stormIntensity > 0.001f && stormProfile != clear) {
        LightingParams storm;
        if (sample(stormProfile, storm)) {
            forEachBlended(out, storm, [stormIntensity](auto& x, const auto& y) {
                x = glm::mix(x, y, stormIntensity);
            });
        }
    }
    return true;
}

bool LightingManager::loadDayNightCycle(pipeline::AssetManager* assetManager) {
    // world/dnc.db, the client's day/night table: a header of (tag, offset)
    // pairs, then one record per hour of 25 tagged 4-byte values - hour,
    // minute, day intensity, day colour and direction, night intensity,
    // night colour and direction, ambient, fog. The shipped file has 25
    // records, 00:00 to 23:00; the day intensity is 0.8 from 06:00 to 19:00
    // and the night intensity 1.0 from 18:00 to 05:00.
    dayNightKeys_.clear();
    auto data = assetManager->readFileOptional("World\\dnc.db");
    if (data.size() < 16) {
        LOG_WARNING("World\\dnc.db not found; using a fixed day and night");
    } else {
        auto u32 = [&](size_t off) {
            uint32_t v = 0;
            if (off + 4 <= data.size()) std::memcpy(&v, data.data() + off, 4);
            return v;
        };
        auto f32 = [&](size_t off) {
            float v = 0.0f;
            if (off + 4 <= data.size()) std::memcpy(&v, data.data() + off, 4);
            return v;
        };
        const uint32_t count = u32(4);
        const uint32_t end = std::min<uint32_t>(u32(12), static_cast<uint32_t>(data.size()));
        constexpr size_t kValues = 25;
        size_t pos = 8 + static_cast<size_t>(count) * 8;
        while (count < 1000 && pos + kValues * 8 <= end) {
            // Each value is a 4-byte tag then the value.
            auto value = [&](size_t index) { return f32(pos + index * 8 + 4); };
            const float hour = value(0), minute = value(1);
            DayNightKey key;
            key.time = static_cast<uint16_t>(
                (static_cast<int>(hour) * 60 + static_cast<int>(minute)) * 2 % kHalfMinutesPerDay);
            key.day = value(2);
            key.night = value(9);
            dayNightKeys_.push_back(key);
            pos += kValues * 8;
        }
    }
    if (dayNightKeys_.empty()) {
        for (int h = 0; h < 24; ++h) {
            DayNightKey key;
            key.time = static_cast<uint16_t>(h * 120);
            key.day = (h >= 6 && h <= 19) ? 0.8f : 0.0f;
            key.night = (h >= 18 || h <= 5) ? 1.0f : 0.0f;
            dayNightKeys_.push_back(key);
        }
    }
    std::sort(dayNightKeys_.begin(), dayNightKeys_.end(),
              [](const DayNightKey& a, const DayNightKey& b) { return a.time < b.time; });
    LOG_INFO("Day/night cycle: ", dayNightKeys_.size(), " keys");
    return true;
}

void LightingManager::sampleDayNight(uint16_t timeHalfMinutes, float& day, float& night) const {
    day = 1.0f;
    night = 0.0f;
    if (dayNightKeys_.empty()) return;
    // Between the key at or before this time and the next, wrapping at midnight.
    size_t next = 0;
    while (next < dayNightKeys_.size() && dayNightKeys_[next].time <= timeHalfMinutes) ++next;
    const DayNightKey& b = dayNightKeys_[next % dayNightKeys_.size()];
    const DayNightKey& a = dayNightKeys_[(next + dayNightKeys_.size() - 1) % dayNightKeys_.size()];
    const int span = (static_cast<int>(b.time) - a.time + kHalfMinutesPerDay) % kHalfMinutesPerDay;
    const int into = (static_cast<int>(timeHalfMinutes) - a.time + kHalfMinutesPerDay) % kHalfMinutesPerDay;
    const float t = span > 0 ? glm::clamp(static_cast<float>(into) / span, 0.0f, 1.0f) : 0.0f;
    // The file's day intensity tops out at 0.8; as a visibility it is "up".
    day = glm::clamp(glm::mix(a.day, b.day, t) / 0.8f, 0.0f, 1.0f);
    night = glm::clamp(glm::mix(a.night, b.night, t), 0.0f, 1.0f);
}

LightingParams LightingManager::sampleLightParams(const LightParamsProfile* profile, uint16_t timeHalfMinutes) const {
    if (!profile) return fallbackParams_;

    LightingParams params;

    // The colour channels, as 0x7ebff0 lays them out: channel 0 is the
    // directional (diffuse) light, channel 1 the ambient (0x780cd0 hands
    // DayNight+0x1ac to the ambient and +0x1a8 to the directional light), 2-6
    // the sky from the zenith down, 7 the fog. A band with no keys is black,
    // as in the client (0x7eb070) - the underwater sets carry no sky bands,
    // and their sky is never seen.
    auto band = [&](LightParamsProfile::ColorChannel ch) {
        return sampleColorBand(profile->colorBands[ch], timeHalfMinutes);
    };
    params.ambientColor = band(LightParamsProfile::AMBIENT_COLOR);
    params.diffuseColor = band(LightParamsProfile::DIFFUSE_COLOR);
    params.fogColor = band(LightParamsProfile::FOG_COLOR);
    // The sky dome's fog band is the authored fog colour, before any haze
    // blend; blended between lights alongside it.
    params.skyFogColor = params.fogColor;
    params.skyTopColor = band(LightParamsProfile::SKY_TOP_COLOR);
    params.skyMiddleColor = band(LightParamsProfile::SKY_MIDDLE_COLOR);
    params.skyBand1Color = band(LightParamsProfile::SKY_BAND1_COLOR);
    params.skyBand2Color = band(LightParamsProfile::SKY_BAND2_COLOR);
    params.skySmogColor = band(LightParamsProfile::SKY_SMOG_COLOR);
    params.sunColor = band(LightParamsProfile::SUN_COLOR);
    params.cloudSunColor = band(LightParamsProfile::CLOUD_SUN_COLOR);
    params.cloudEmissiveColor = band(LightParamsProfile::CLOUD_EMISSIVE_COLOR);
    params.cloudLayer1AmbientColor = band(LightParamsProfile::CLOUD_LAYER1_AMBIENT_COLOR);
    params.cloudLayer2AmbientColor = band(LightParamsProfile::CLOUD_LAYER2_AMBIENT_COLOR);

    // Channel 8 is loaded into DayNight (0x7ebff0 puts it at +0xdc) but no
    // renderer reads it. The stock terrain shaders take the shadowed ground
    // to a fixed 70% (terrain1-3.bls: 0.7 + 0.3 * lit, for MCSH and the
    // shadow map alike), so the shadow's opacity is 0.3 at every hour.
    params.shadowOpacity = 1.0f - stock_light::kTerrainShadowFloor;

    // Liquid colours. A profile with no band for a channel keeps the default
    // rather than black, and a missing far colour follows the close one: 211
    // of the 850 rows have no ocean colours. (The water renderer's own; the
    // stock client would hand it black.)
    auto liquidColor = [&](LightParamsProfile::ColorChannel ch, const glm::vec3& fallback) {
        const ColorBand& b = profile->colorBands[ch];
        return b.numKeyframes > 0 ? sampleColorBand(b, timeHalfMinutes) : fallback;
    };
    params.oceanCloseColor = liquidColor(LightParamsProfile::OCEAN_CLOSE_COLOR, params.oceanCloseColor);
    params.oceanFarColor = liquidColor(LightParamsProfile::OCEAN_FAR_COLOR, params.oceanCloseColor * 0.4f);
    params.riverCloseColor = liquidColor(LightParamsProfile::RIVER_CLOSE_COLOR, params.riverCloseColor);
    params.riverFarColor = liquidColor(LightParamsProfile::RIVER_FAR_COLOR, params.riverCloseColor * 0.4f);
    params.waterShallowAlpha = profile->waterShallowAlpha;
    params.waterDeepAlpha = profile->waterDeepAlpha;
    params.oceanShallowAlpha = profile->oceanShallowAlpha;
    params.oceanDeepAlpha = profile->oceanDeepAlpha;
    params.glow = profile->glow;
    // Enhanced's air (0 on 3.3.5's own profiles).
    params.airDensity = profile->airDensity;
    // The dome's dawn and dusk highlight strength (horizonGlow is what the
    // sky reads it from; blended between lights like the other floats).
    params.horizonGlow = profile->highlightSky;

    // The fog of this one light, shaped the way 0x7ecd80 shapes it before the
    // lights are blended: FloatBand 0 is the end in thirty-sixths of a yard
    // (0x7eaef0 scales it), never closer than ten yards; FloatBand 1 the
    // start as a fraction of the end. On maps from 530 on the end moves out
    // to the far clip and the band's own span sets the curve's exponent.
    {
        const float bandEnd = sampleFloatBand(profile->floatBands[LightParamsProfile::FOG_END],
                                              timeHalfMinutes) / LIGHT_COORD_UNITS_PER_YARD;
        const float bandScaler = sampleFloatBand(
            profile->floatBands[LightParamsProfile::FOG_START_SCALAR], timeHalfMinutes);
        const stock_light::LightFog fog = stock_light::lightFog(
            bandEnd, bandScaler, std::max(viewDistance_, 1.0f), farClipFog_);
        params.fogEnd = fog.end;
        params.fogScaler = fog.scaler;
        params.fogExponent = fog.exponent;
        params.fogStart = fog.end * fog.scaler;
    }
    params.fogDensity = sampleFloatBand(profile->floatBands[LightParamsProfile::FOG_DENSITY], timeHalfMinutes);
    params.cloudDensity = sampleFloatBand(profile->floatBands[LightParamsProfile::CLOUD_DENSITY], timeHalfMinutes);
    params.celestialGlowThrough = sampleFloatBand(
        profile->floatBands[LightParamsProfile::CELESTIAL_GLOW_THROUGH], timeHalfMinutes);

    // Light.dbc carries no direction: the stock client's DayNight sets it from
    // the hour alone, the same in every light and on every map.
    params.directionalDir = stockLightDirection(static_cast<float>(timeHalfMinutes));

    return params;
}

glm::vec3 LightingManager::sampleColorBand(const ColorBand& band, uint16_t timeHalfMinutes) const {
    // 0x7eb070: byte by byte between the two keys around the time, rounded.
    const uint32_t c = stock_light::sampleColorBand(band.times, band.packed, band.numKeyframes,
                                                    timeHalfMinutes);
    return dbcColorToVec3(c);
}

float LightingManager::sampleFloatBand(const FloatBand& band, uint16_t timeHalfMinutes) const {
    // 0x7eaef0.
    return stock_light::sampleFloatBand(band.times, band.values, band.numKeyframes,
                                        timeHalfMinutes);
}

glm::vec3 LightingManager::dbcColorToVec3(uint32_t dbcColor) const {
    // Red is the high byte: the packed value is 0x00RRGGBB. Reading it the
    // other way round swaps red and blue, which turns Teldrassil's violet
    // canopies magenta and leaves every zone's light fighting its own sky.
    //
    // Checked against the file rather than the layout's name for it. Over the
    // 844 LightParams rows that carry a first channel, taking red from the
    // high byte makes 66% of them warm at noon and 64% blue at midnight;
    // taking it from the low byte gives 35% and 41%, which is worse than
    // chance in both directions - the mark of a colour read backwards.
    const uint8_t r = (dbcColor >> 16) & 0xFF;
    const uint8_t g = (dbcColor >> 8) & 0xFF;
    const uint8_t b = dbcColor & 0xFF;

    return glm::vec3(r / 255.0f, g / 255.0f, b / 255.0f);
}

} // namespace rendering
} // namespace wowee
