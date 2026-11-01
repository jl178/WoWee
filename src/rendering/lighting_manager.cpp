#include "core/local_time.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/light_coords.hpp"
#include "rendering/light_band_block.hpp"
#include "rendering/light_headroom.hpp"
#include "rendering/light_volume_order.hpp"
#include "rendering/stock_day_night.hpp"
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

// Maximum volumes to blend (top 2-4)
constexpr size_t MAX_BLEND_VOLUMES = 2;

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
    loadLightBandDbcs(assetManager);
    loadDayNightCycle(assetManager);

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
        }
        // 3.3.5: ID, HighlightSky, LightSkyboxID, CloudTypeID, Glow,
        // WaterShallowAlpha, WaterDeepAlpha, OceanShallowAlpha, OceanDeepAlpha.
        if (dbc->getFieldCount() >= 9) {
            profile.waterShallowAlpha = dbc->getFloat(i, 5);
            profile.waterDeepAlpha = dbc->getFloat(i, 6);
            profile.oceanShallowAlpha = dbc->getFloat(i, 7);
            profile.oceanDeepAlpha = dbc->getFloat(i, 8);
        }
        lightParamsProfiles_[paramId] = profile;
    }

    return true;
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
        if (id != 0 && !path.empty()) lightSkyboxPaths_[id] = std::move(path);
    }
    LOG_INFO("Loaded LightSkybox.dbc: ", lightSkyboxPaths_.size(), " model paths");
    return !lightSkyboxPaths_.empty();
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

    // Convert visual time to half-minutes (WoW DBC format: 0-2879).
    const float visualDayFraction = visualTimeOfDayHours_ / 24.0f;
    uint16_t timeHalfMinutes = static_cast<uint16_t>(visualDayFraction * static_cast<float>(kHalfMinutesPerDay)) % kHalfMinutesPerDay;
    // Unrounded, for the sun and light directions: the stock client moves
    // them continuously rather than a half-minute at a time.
    const float timeHalfMinutesExact = visualDayFraction * static_cast<float>(kHalfMinutesPerDay);

    // Update player position and map
    currentPlayerPos_ = playerPos;

    // Find light volumes for blending
    activeVolumes_ = findLightVolumes(playerPos, mapId);

    // Smoothing by real time. This assumed sixty frames a second, so on a
    // display running at a hundred and twenty every blend below ran twice as
    // fast, and at thirty half as fast.
    const auto now = std::chrono::steady_clock::now();
    float deltaTime = 0.016f;
    if (lastUpdate_.time_since_epoch().count() != 0) {
        deltaTime = std::clamp(std::chrono::duration<float>(now - lastUpdate_).count(),
                               0.0f, 0.25f);
    }
    lastUpdate_ = now;
    const float blendFactor = 1.0f - std::exp(-deltaTime * 5.0f);

    // dnc.db's day and night, the same everywhere: they say when the sun,
    // and the moon and stars, are up.
    float dayIntensity = 1.0f, nightIntensity = 0.0f;
    sampleDayNight(timeHalfMinutes, dayIntensity, nightIntensity);

    // Which sky models are overhead, and how much of each.
    //
    // One model was chosen and held until the player left its zone's
    // falloff, then swapped for the next in a single frame - while the sky
    // colours had been blending across that same falloff all along. So at a
    // border the old zone's sky sat over colours already most of the way to
    // the new one: Hellfire's nebula stops at a line of longitude, and past it
    // the sky behind was already Terokkar's pale blue, a hard edge down the
    // middle of the screen. Then it popped.
    //
    // Now each model is up by the weight of the lights that name it, over the
    // same volumes and with the same smoothing the colours use, so the two
    // cross the border together. A light with no sky model contributes to no
    // layer, and the models fade toward the plain sky there.
    {
        // The night stars the stock client draws wherever no sky model does:
        // Stars.mdx, the LightSkybox 4 model, which no Azeroth or Kalimdor
        // LightParams row names (all 209 of their lights have no sky model).
        static const std::string kStarsModel = "Environments\\Stars\\Stars.mdx";
        std::vector<SkyboxLayer> target;
        if (!isIndoors_) {
            const size_t blendCount = std::min(activeVolumes_.size(), MAX_BLEND_VOLUMES);
            float zoneSkyWeight = 0.0f;
            for (size_t i = 0; i < blendCount; ++i) {
                const auto& wv = activeVolumes_[i];
                const uint32_t paramsId = dominantParamsId(wv.volume, stormIntensity, isUnderwater);
                auto profileIt = lightParamsProfiles_.find(paramsId);
                if (profileIt == lightParamsProfiles_.end() ||
                    profileIt->second.lightSkyboxId == 0) continue;
                auto skyIt = lightSkyboxPaths_.find(profileIt->second.lightSkyboxId);
                if (skyIt == lightSkyboxPaths_.end() || skyIt->second.empty()) continue;
                zoneSkyWeight += wv.weight;
                auto t = std::find_if(target.begin(), target.end(),
                                      [&](const SkyboxLayer& e) { return e.path == skyIt->second; });
                if (t == target.end()) target.push_back({.path = skyIt->second, .weight = wv.weight});
                else t->weight += wv.weight;
            }
            const float starsWeight =
                nightIntensity * (1.0f - glm::clamp(zoneSkyWeight, 0.0f, 1.0f));
            if (starsWeight > 0.0f && !isUnderwater) {
                target.push_back({.path = kStarsModel, .weight = starsWeight, .stars = true});
            }
        }
        auto targetFor = [&](const std::string& path) {
            for (const auto& t : target) if (t.path == path) return t.weight;
            return 0.0f;
        };
        for (auto& layer : skyboxLayers_) {
            layer.weight += (targetFor(layer.path) - layer.weight) * blendFactor;
        }
        for (const auto& t : target) {
            const bool present = std::any_of(skyboxLayers_.begin(), skyboxLayers_.end(),
                                             [&](const SkyboxLayer& l) { return l.path == t.path; });
            if (!present) {
                skyboxLayers_.push_back({.path = t.path, .weight = t.weight * blendFactor,
                                         .stars = t.stars});
            }
        }
        // Gone once faded out and not wanted - never while it is still wanted,
        // so a model is not dropped and reloaded on its way in.
        std::erase_if(skyboxLayers_, [&](const SkyboxLayer& l) {
            return l.weight < 0.005f && targetFor(l.path) <= 0.0f;
        });
        std::sort(skyboxLayers_.begin(), skyboxLayers_.end(),
                  [](const SkyboxLayer& a, const SkyboxLayer& b) { return a.weight > b.weight; });
    }

    // Sample and blend lighting
    LightingParams newParams;

    if (isIndoors_) {
        // Indoor lighting: static ambient-heavy
        newParams.ambientColor = glm::vec3(0.6f, 0.6f, 0.65f);
        newParams.diffuseColor = glm::vec3(0.3f, 0.3f, 0.35f);
        newParams.fogColor = glm::vec3(0.3f, 0.3f, 0.4f);
        newParams.fogStart = 50.0f;
        newParams.fogEnd = 300.0f;
    } else if (!activeVolumes_.empty()) {
        // Outdoor with DBC lighting - blend multiple volumes
        // `= {}` keeps the default member initialisers, so every field the
        // weighted sum below adds onto is zeroed by hand. Only the liquid
        // fields used to be: ambient, sun, sky, fog and clouds were the
        // defaults *plus* the blend, which put about +0.4 on every outdoor
        // ambient and over-bright sun, washing the world out against stock.
        newParams = {};
        forEachBlended(newParams, newParams, [](auto& x, const auto&) {
            x = std::remove_reference_t<decltype(x)>(0.0f);  // glm's {} need not be zero
        });
        float profileWeight = 0.0f;
        glm::vec3 blendedDir(0.0f);  // Accumulate weighted directions

        // The first MAX_BLEND_VOLUMES only: the list is the whole
        // neighbourhood now, and those are the ones whose weights were
        // normalized to sum to one.
        const size_t blendCount = std::min(activeVolumes_.size(), MAX_BLEND_VOLUMES);
        for (size_t i = 0; i < blendCount; ++i) {
            const auto& wv = activeVolumes_[i];
            LightingParams params;
            if (!sampleVolume(wv.volume, timeHalfMinutes, stormIntensity, isUnderwater, params)) {
                continue;
            }
            const float w = wv.weight;
            forEachBlended(newParams, params, [w](auto& x, const auto& y) { x += y * w; });
            profileWeight += w;
            // Blend direction weighted (normalize at end)
            blendedDir += params.directionalDir * w;
        }

        // A weighted average over the volumes that had a profile, so a volume
        // naming a missing LightParams row does not fade anything out.
        if (profileWeight > 1e-4f) {
            const float inv = 1.0f / profileWeight;
            forEachBlended(newParams, newParams, [inv](auto& x, const auto&) { x *= inv; });
        } else {
            const glm::vec3 keepDir = newParams.directionalDir;
            newParams = LightingParams{};
            newParams.directionalDir = keepDir;
        }

        // Normalize blended direction
        float blendedDirLenSq = glm::dot(blendedDir, blendedDir);
        if (blendedDirLenSq > 1e-6f) {
            newParams.directionalDir = blendedDir * glm::inversesqrt(blendedDirLenSq);
        } else {
            // No volume had a profile. The light's way does not depend on
            // the volume anyway (see stockLightDirection).
            newParams.directionalDir = stockLightDirection(timeHalfMinutesExact);
        }
    } else {
        // No light volume, use fallback with time-based animation
        newParams = fallbackParams_;

        // The stock light by the hour, as with Light.dbc.
        newParams.directionalDir = stockLightDirection(timeHalfMinutesExact);

        // Time-of-day color adjustments
        if (timeOfDay_ < 0.25f || timeOfDay_ > 0.75f) {
            // Night: darker, bluer
            float nightness = (timeOfDay_ < 0.25f) ? (0.25f - timeOfDay_) * 4.0f
                                                    : (timeOfDay_ - 0.75f) * 4.0f;
            newParams.ambientColor *= (0.3f + 0.7f * (1.0f - nightness));
            newParams.diffuseColor *= (0.2f + 0.8f * (1.0f - nightness));
            newParams.ambientColor.b += nightness * 0.1f;
        }
    }

    // The sky dome's fog band is the authored colour, before the haze blend.
    newParams.skyFogColor = newParams.fogColor;

    // Fog toward the colour of the sky it is seen against.
    //
    // The fog colour comes from LightParams alone, so it has no relation to
    // what is behind it. In a zone whose sky is dark - Hellfire's purple, the
    // Plaguelands, anywhere at night - terrain faded to a pale grey that the
    // sky never reaches, and the horizon read as a bright band laid over a
    // dark background rather than distance.
    //
    // The horizon colour is skyMiddleColor: that is the band the far terrain
    // actually sits in front of, and mixing toward it makes distant ground
    // approach what is behind it from any angle. The DBC's own colour is kept
    // as the other end, because it carries the zone's authored tint and a fog
    // that is purely sky has no colour of its own at all.
    //
    // The amount is a setting rather than a constant, because how much haze a
    // zone should have is taste and the right value is not the same at noon in
    // Elwynn as at night in Hellfire. Zero is the old behaviour exactly.
    //
    // Neither this nor the strength below applies under water: there the fog
    // is the water itself, Light.dbc's underwater set exactly (about 170
    // yards, teal), and the sky it would be pulled toward is not in view.
    const bool authoredFogOnly = isUnderwater && !isIndoors_;
    if (!authoredFogOnly) {
        newParams.fogColor = glm::mix(newParams.fogColor, newParams.skyMiddleColor,
                                      glm::clamp(fogSkyBlend_, 0.0f, 1.0f));
    }

    // How much fog, as a multiplier on the distances the zone asks for.
    //
    // Fog is drawn between fogStart and fogEnd, so bringing both in thickens
    // it and pushing both out clears it. Scaling by 1/strength keeps the
    // shape of the zone's own curve and only moves where it sits: 1.0 is the
    // DBC exactly, 2.0 puts the same gradient at half the distance, and 0.5
    // doubles it.
    //
    // Zero is off rather than infinitely thick. A strength of zero scaling
    // distances by 1/0 is where a divide would land, and what the player means
    // by nothing is no fog at all.
    const float strength = authoredFogOnly ? 1.0f : glm::clamp(fogStrength_, 0.0f, 2.0f);
    if (strength <= 0.001f) {
        newParams.fogStart = 1.0e6f;
        newParams.fogEnd = 1.0e6f + 1.0f;
    } else if (std::abs(strength - 1.0f) > 0.001f) {
        const float scale = 1.0f / strength;
        newParams.fogStart *= scale;
        newParams.fogEnd *= scale;
    }

    // And as far as the view distance reaches.
    //
    // The fog's distances knew nothing of the view distance, so wherever the
    // fog ended beyond it - most zones at the default 0.4 strength, which
    // pushes Light.dbc's 300 to 1000 yards out two and a half times - the
    // world stopped at a hard line half-hazed against the sky. The fog now
    // always finishes by the view distance, the start keeping its place on
    // the curve. Past the reference distance (the setting's default) it also
    // moves out in proportion, so a longer view sees further into the same
    // haze rather than the same haze with more drawn behind it. At or below
    // the reference nothing moves except that last clamp: at the default the
    // fog is Light.dbc's, times the strength, as before. Off (strength 0)
    // stays off, and under water the water's own fog is left alone.
    if (strength > 0.001f && newParams.fogEnd > 1.0f) {
        const float reach = std::max(viewDistance_, 1.0f);
        float scale = authoredFogOnly ? 1.0f : std::max(1.0f, reach / kFogReferenceViewDistance);
        if (newParams.fogEnd * scale > reach) scale = reach / newParams.fogEnd;
        newParams.fogStart *= scale;
        newParams.fogEnd *= scale;
    }

    // Every lit shader adds these as `ambient + diffuse * N-dot-L`, and on
    // ground facing the sun that is nearly their plain sum. 82% of the
    // client's 844 LightParams rows exceed 1.0 in some channel at noon, the
    // worst reaching 2.00, and clipping happens per channel - the ones that
    // overflow stop while the rest keep their value, and the hue slides toward
    // whatever saturated. Scaling both together keeps the hue and only ever
    // darkens.
    applyLightHeadroom(newParams.ambientColor, newParams.diffuseColor);

    // What the sky is being told, whenever it changes.
    //
    // Three separate boundary faults have been fixed behind a report of the
    // sky changing brightness as the player walks, and it is still reported.
    // Guessing at a fourth is worth less than one line saying which input
    // moved: the visual hour, the zone, the sky model, the volumes being
    // blended, or none of them - which would put it past this file entirely.
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
        if (!activeVolumes_.empty()) firstVolume = activeVolumes_[0].volume->lightId;
        if (activeVolumes_.size() > 1) secondVolume = activeVolumes_[1].volume->lightId;
        const bool changed =
            zoneId != diagZoneId_ || topSkybox != diagSkyboxPath_ ||
            firstVolume != diagFirstVolume_ || secondVolume != diagSecondVolume_ ||
            std::abs(visualTimeOfDayHours_ - diagVisualHours_) > 0.02f ||
            std::abs(skyLuma - diagSkyLuma_) > 0.01f;
        // Rate-limited as well as change-gated, because a change is not rare:
        // walking through the falloff between two volumes moves the target
        // luminance every frame, and a formatted write per frame is what this
        // whole investigation turned out to be about. Half a second is fast
        // enough to see what is oscillating and slow enough to cost nothing.
        ++diagCallsSinceLog_;
        if (changed && diagCallsSinceLog_ >= 30) {
            diagCallsSinceLog_ = 0;
            LOG_INFO("sky: zone=", zoneId, " hour=", visualTimeOfDayHours_,
                     " skyLuma=", skyLuma, " volumes=", firstVolume, "/", secondVolume,
                     " inRange=", activeVolumes_.size(),
                     " skybox=", topSkybox.empty() ? "-" : topSkybox);
            diagZoneId_ = zoneId;
            diagSkyboxPath_ = topSkybox;
            diagFirstVolume_ = firstVolume;
            diagSecondVolume_ = secondVolume;
            diagVisualHours_ = visualTimeOfDayHours_;
            diagSkyLuma_ = skyLuma;
        }
    }

    // Smooth temporal blending to avoid snapping (5.0 = blend rate); see
    // blendFactor above.

    forEachBlended(currentParams_, newParams,
                   [blendFactor](auto& x, const auto& y) { x = glm::mix(x, y, blendFactor); });
    // Day and night follow the clock alone, so they are not smoothed.
    currentParams_.dayIntensity = dayIntensity;
    currentParams_.nightIntensity = nightIntensity;
    // So does the sun sprite.
    currentParams_.sunDir = stockSunDirection(timeHalfMinutesExact);
    currentParams_.directionalDir = glm::normalize(glm::mix(currentParams_.directionalDir, newParams.directionalDir, blendFactor));
    // Settled exactly once within a millionth of the target. The blend comes
    // to rest an ulp or two short of it, and for most directions it then
    // flips between neighbouring floats from frame to frame for as long as
    // the hour holds: invisible, but a new shadow projection every frame, and
    // the M2 shadow pass keeps its static casters only while the projection
    // is unchanged (M2Renderer::renderShadow). WOWEE_SHADOW_CACHE=0 leaves
    // the blend as it was.
    static const bool settleSunDir = [] {
        const char* v = std::getenv("WOWEE_SHADOW_CACHE");
        return !(v && v[0] == '0');
    }();
    if (settleSunDir) {
        const glm::vec3 d = currentParams_.directionalDir - newParams.directionalDir;
        if (glm::dot(d, d) < 1e-12f) currentParams_.directionalDir = newParams.directionalDir;
    }
}

std::vector<LightingManager::WeightedVolume> LightingManager::findLightVolumes(const glm::vec3& playerPos, uint32_t mapId) const {
    auto it = lightVolumesByMap_.find(mapId);
    if (it == lightVolumesByMap_.end()) {
        return {};
    }

    const std::vector<LightVolume>& volumes = it->second;
    if (volumes.empty()) {
        return {};
    }

    // Collect all volumes with weight > 0
    std::vector<WeightedVolume> weighted;
    weighted.reserve(volumes.size());

    for (const auto& volume : volumes) {
        glm::vec3 toPlayer = playerPos - volume.position;
        float distSq = glm::dot(toPlayer, toPlayer);

        float weight = 0.0f;
        if (distSq <= volume.innerRadius * volume.innerRadius) {
            // Inside inner radius: full weight
            weight = 1.0f;
        } else if (distSq < volume.outerRadius * volume.outerRadius) {
            // Between inner and outer: fade out with smoothstep (sqrt needed for interpolation)
            float dist = std::sqrt(distSq);
            float t = (dist - volume.innerRadius) / (volume.outerRadius - volume.innerRadius);
            t = glm::clamp(t, 0.0f, 1.0f);
            weight = 1.0f - (t * t * (3.0f - 2.0f * t));  // Smoothstep
        }

        if (weight > 0.0f) {
            weighted.push_back({.volume = &volume, .weight = weight});
        }
    }

    if (weighted.empty()) {
        return {};
    }

    // Keep the top N by weight, ordered by something that cannot change under
    // the player's feet - see light_volume_order.hpp for why weight alone is
    // not an order here and what the flicker looked like.
    const auto moreSpecific = [](const WeightedVolume& a, const WeightedVolume& b) {
        return lightVolumeOrderedBefore(a.weight, a.volume->outerRadius, a.volume->lightId,
                                        b.weight, b.volume->outerRadius, b.volume->lightId);
    };
    // Sorted in full rather than truncated here. Only the first
    // MAX_BLEND_VOLUMES are blended, but the skybox is chosen over the whole
    // list: a volume that names a sky and sits third on weight still says
    // which sky is overhead, and cutting the list here meant it dropped out
    // and back in as the player walked.
    std::sort(weighted.begin(), weighted.end(), moreSpecific);

    // Normalize the ones that will be blended, so their weights sum to 1.0.
    // The rest keep the weight they were given; nothing reads it but the
    // skybox walk, which only asks whether they are in range at all.
    const size_t blended = std::min(weighted.size(), MAX_BLEND_VOLUMES);
    float totalWeight = 0.0f;
    for (size_t i = 0; i < blended; ++i) {
        totalWeight += weighted[i].weight;
    }
    if (totalWeight > 0.0f) {
        for (size_t i = 0; i < blended; ++i) {
            weighted[i].weight /= totalWeight;
        }
    }

    // Which volumes this map turned out to have around the player, once.
    //
    // This was three lines inside the loop above, meant to fire on one frame
    // and guarded by a counter that only decremented when the third volume was
    // pushed. Almost nowhere has three: Hellfire Peninsula has one. So the
    // counter never reached zero and the line was written every frame, for the
    // whole session - 534 of one log's 3384 lines, still going four minutes in.
    //
    // It reads as movement-triggered, which is what makes it worth explaining
    // rather than just deleting. The logger collapses a message identical to
    // the one before it, so standing still folds into "previous message
    // repeated" and walking changes distSq every frame and folds into nothing.
    // Whatever a per-frame formatted write costs, it was being paid only while
    // the player moved, and only with WOWEE_LOG_LEVEL=info.
    if (mapId != diagLoggedMapId_) {
        diagLoggedMapId_ = mapId;
        std::string named;
        for (size_t i = 0; i < weighted.size() && i < 3; ++i) {
            const LightVolume& v = *weighted[i].volume;
            named += " [" + std::to_string(v.lightId) + " inner=" +
                     std::to_string(static_cast<int>(v.innerRadius)) + " outer=" +
                     std::to_string(static_cast<int>(v.outerRadius)) + "]";
        }
        LOG_INFO("Light volumes on map ", mapId, ": ", weighted.size(),
                 " in range,", named.empty() ? " none" : named);
    }

    return weighted;
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

uint32_t LightingManager::dominantParamsId(const LightVolume* volume, float stormIntensity,
                                           bool isUnderwater) const {
    if (playerDead_) return paramsIdForSlot(volume, LightVolume::SLOT_DEATH);
    const bool storm = stormIntensity >= 0.5f;
    if (isUnderwater) {
        return paramsIdForSlot(volume, storm ? LightVolume::SLOT_STORM_UNDERWATER
                                             : LightVolume::SLOT_CLEAR_UNDERWATER);
    }
    return paramsIdForSlot(volume, storm ? LightVolume::SLOT_STORM : LightVolume::SLOT_CLEAR);
}

bool LightingManager::sampleVolume(const LightVolume* volume, uint16_t timeHalfMinutes,
                                   float stormIntensity, bool isUnderwater,
                                   LightingParams& out) const {
    if (!volume) return false;
    auto sample = [&](uint32_t id, LightingParams& p) {
        auto it = lightParamsProfiles_.find(id);
        if (it == lightParamsProfiles_.end()) return false;
        p = sampleLightParams(&it->second, timeHalfMinutes);
        return true;
    };

    if (playerDead_) {
        return sample(paramsIdForSlot(volume, LightVolume::SLOT_DEATH), out);
    }

    const uint32_t clearId = paramsIdForSlot(
        volume, isUnderwater ? LightVolume::SLOT_CLEAR_UNDERWATER : LightVolume::SLOT_CLEAR);
    if (!sample(clearId, out)) return false;

    // Toward the storm set by the weather's intensity: light rain greys the
    // light a little, a downpour all the way.
    const uint32_t stormId = paramsIdForSlot(
        volume, isUnderwater ? LightVolume::SLOT_STORM_UNDERWATER : LightVolume::SLOT_STORM);
    if (stormIntensity > 0.001f && stormId != clearId) {
        LightingParams storm;
        if (sample(stormId, storm)) {
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

    // Sample color bands
    params.ambientColor = sampleColorBand(profile->colorBands[LightParamsProfile::AMBIENT_COLOR], timeHalfMinutes);
    params.diffuseColor = sampleColorBand(profile->colorBands[LightParamsProfile::DIFFUSE_COLOR], timeHalfMinutes);
    params.fogColor = sampleColorBand(profile->colorBands[LightParamsProfile::FOG_COLOR], timeHalfMinutes);
    params.skyTopColor = sampleColorBand(profile->colorBands[LightParamsProfile::SKY_TOP_COLOR], timeHalfMinutes);
    params.skyMiddleColor = sampleColorBand(profile->colorBands[LightParamsProfile::SKY_MIDDLE_COLOR], timeHalfMinutes);
    params.skyBand1Color = sampleColorBand(profile->colorBands[LightParamsProfile::SKY_BAND1_COLOR], timeHalfMinutes);
    params.skyBand2Color = sampleColorBand(profile->colorBands[LightParamsProfile::SKY_BAND2_COLOR], timeHalfMinutes);

    // The rest of the sky. A profile with no band for one (the underwater
    // sets carry no sky bands at all) keeps the nearest sky colour, not grey.
    auto bandOr = [&](LightParamsProfile::ColorChannel ch, const glm::vec3& fallback) {
        const ColorBand& band = profile->colorBands[ch];
        return band.numKeyframes > 0 ? sampleColorBand(band, timeHalfMinutes) : fallback;
    };
    params.skySmogColor = bandOr(LightParamsProfile::SKY_SMOG_COLOR, params.skyBand2Color);
    params.sunColor = bandOr(LightParamsProfile::SUN_COLOR, params.diffuseColor);
    params.cloudSunColor = bandOr(LightParamsProfile::CLOUD_SUN_COLOR, params.sunColor);
    params.cloudEmissiveColor = bandOr(LightParamsProfile::CLOUD_EMISSIVE_COLOR, params.ambientColor);
    params.cloudLayer1AmbientColor =
        bandOr(LightParamsProfile::CLOUD_LAYER1_AMBIENT_COLOR, glm::vec3(0.0f));
    params.cloudLayer2AmbientColor =
        bandOr(LightParamsProfile::CLOUD_LAYER2_AMBIENT_COLOR, params.ambientColor);

    // Shadow darkness from channel 8, the grey later clients split out as
    // ShadowOpacity: (77,77,77) at Stormwind noon. The darker the grey, the
    // deeper the shadow - so a storm set or an overcast hour that lightens it
    // softens the shadows with it, and the sky decides rather than the shadow
    // distance slider, which is what used to set this. k puts that noon grey
    // (luminance 0.30) at the 0.8 every shadow had before; the clamp keeps a
    // set with no shadow band, or a black one, from erasing or inking them.
    {
        const ColorBand& band = profile->colorBands[LightParamsProfile::SHADOW_COLOR];
        if (band.numKeyframes > 0) {
            const glm::vec3 c = sampleColorBand(band, timeHalfMinutes);
            const float luminance = glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f));
            constexpr float kShadowOpacityScale = 1.15f;
            params.shadowOpacity = glm::clamp(kShadowOpacityScale * (1.0f - luminance), 0.4f, 0.9f);
        }
    }

    // Liquid colours. A profile with no band for a channel keeps the default
    // rather than sampleColorBand's grey, and a missing far colour follows the
    // close one.
    auto liquidColor = [&](LightParamsProfile::ColorChannel ch, const glm::vec3& fallback) {
        const ColorBand& band = profile->colorBands[ch];
        return band.numKeyframes > 0 ? sampleColorBand(band, timeHalfMinutes) : fallback;
    };
    params.oceanCloseColor = liquidColor(LightParamsProfile::OCEAN_CLOSE_COLOR, params.oceanCloseColor);
    params.oceanFarColor = liquidColor(LightParamsProfile::OCEAN_FAR_COLOR, params.oceanCloseColor * 0.4f);
    params.riverCloseColor = liquidColor(LightParamsProfile::RIVER_CLOSE_COLOR, params.riverCloseColor);
    params.riverFarColor = liquidColor(LightParamsProfile::RIVER_FAR_COLOR, params.riverCloseColor * 0.4f);
    params.waterShallowAlpha = profile->waterShallowAlpha;
    params.waterDeepAlpha = profile->waterDeepAlpha;
    params.oceanShallowAlpha = profile->oceanShallowAlpha;
    params.oceanDeepAlpha = profile->oceanDeepAlpha;

    // Sample float bands. The fog distance is stored in the same
    // thirty-sixths of a yard as the light positions, and was being used raw:
    // Tirisfal's 12000 became 12000 yards, so the fog ended six times further
    // out than the far clip and nothing was ever hazed. It is 333 yards.
    params.fogEnd = sampleFloatBand(profile->floatBands[LightParamsProfile::FOG_END],
                                    timeHalfMinutes) / LIGHT_COORD_UNITS_PER_YARD;
    // The start is a fraction of the end rather than a distance, so it needs
    // no conversion of its own.
    const float fogStartScalar = sampleFloatBand(
        profile->floatBands[LightParamsProfile::FOG_START_SCALAR], timeHalfMinutes);
    params.fogStart = params.fogEnd * fogStartScalar;
    params.fogDensity = sampleFloatBand(profile->floatBands[LightParamsProfile::FOG_DENSITY], timeHalfMinutes);
    params.cloudDensity = sampleFloatBand(profile->floatBands[LightParamsProfile::CLOUD_DENSITY], timeHalfMinutes);
    params.celestialGlowThrough = sampleFloatBand(
        profile->floatBands[LightParamsProfile::CELESTIAL_GLOW_THROUGH], timeHalfMinutes);

    // Debug logging for fog params (first few samples only)
    static int debugCount = 0;
    if (debugCount < 3) {
        LOG_INFO("Fog params: start=", params.fogStart, " end=", params.fogEnd,
                 " color=(", params.fogColor.r, ",", params.fogColor.g, ",", params.fogColor.b, ")");
        debugCount++;
    }

    // Light.dbc carries no direction: the stock client's DayNight sets it from
    // the hour alone, the same in every light and on every map.
    params.directionalDir = stockLightDirection(static_cast<float>(timeHalfMinutes));

    return params;
}

glm::vec3 LightingManager::sampleColorBand(const ColorBand& band, uint16_t timeHalfMinutes) const {
    if (band.numKeyframes == 0) {
        return glm::vec3(0.5f);  // Fallback gray
    }

    if (band.numKeyframes == 1) {
        return band.colors[0];  // Single keyframe
    }

    // Safer initialization: default to wrapping last→first
    uint8_t idx1 = band.numKeyframes - 1;
    uint8_t idx2 = 0;

    // Find surrounding keyframes
    for (uint8_t i = 0; i < band.numKeyframes; ++i) {
        if (timeHalfMinutes < band.times[i]) {
            idx2 = i;
            idx1 = (i > 0) ? (i - 1) : (band.numKeyframes - 1);  // Wrap to last
            break;
        }
    }

    // Calculate interpolation factor
    uint16_t t1 = band.times[idx1];
    uint16_t t2 = band.times[idx2];

    // Handle midnight wrap
    uint16_t timeSpan = (t2 > t1) ? (t2 - t1) : (kHalfMinutesPerDay - t1 + t2);
    uint16_t elapsed = (timeHalfMinutes >= t1) ? (timeHalfMinutes - t1) : (kHalfMinutesPerDay - t1 + timeHalfMinutes);

    float t = (timeSpan > 0) ? (static_cast<float>(elapsed) / static_cast<float>(timeSpan)) : 0.0f;
    t = glm::clamp(t, 0.0f, 1.0f);

    // Linear interpolation
    return glm::mix(band.colors[idx1], band.colors[idx2], t);
}

float LightingManager::sampleFloatBand(const FloatBand& band, uint16_t timeHalfMinutes) const {
    if (band.numKeyframes == 0) {
        return 1.0f;  // Fallback
    }

    if (band.numKeyframes == 1) {
        return band.values[0];
    }

    // Safer initialization: default to wrapping last→first
    uint8_t idx1 = band.numKeyframes - 1;
    uint8_t idx2 = 0;

    // Find surrounding keyframes
    for (uint8_t i = 0; i < band.numKeyframes; ++i) {
        if (timeHalfMinutes < band.times[i]) {
            idx2 = i;
            idx1 = (i > 0) ? (i - 1) : (band.numKeyframes - 1);
            break;
        }
    }

    uint16_t t1 = band.times[idx1];
    uint16_t t2 = band.times[idx2];

    uint16_t timeSpan = (t2 > t1) ? (t2 - t1) : (kHalfMinutesPerDay - t1 + t2);
    uint16_t elapsed = (timeHalfMinutes >= t1) ? (timeHalfMinutes - t1) : (kHalfMinutesPerDay - t1 + timeHalfMinutes);

    float t = (timeSpan > 0) ? (static_cast<float>(elapsed) / static_cast<float>(timeSpan)) : 0.0f;
    t = glm::clamp(t, 0.0f, 1.0f);

    return glm::mix(band.values[idx1], band.values[idx2], t);
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
