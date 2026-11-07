// The "Enhancements: Weather" rows on the renderer's side (docs/enhancements-
// plan.md E23-E31; the rules are rendering/weather_fx.hpp): the weather's
// style, the client-only preview, snow settling, a sandstorm's haze,
// lightning and its thunder and the fireflies; and the stock cold breath,
// which the "Breath in the cold" row extends to falling snow.
//
// With every row at its stock value nothing here allocates, spawns, plays or
// changes a number: updateWeatherFx hands the weather a default Style and a
// zero firefly count, and applyWeatherFxLight leaves the frame alone.

#include "rendering/renderer.hpp"
#include "rendering/weather.hpp"
#include "rendering/weather_fx.hpp"
#include "rendering/stock_weather.hpp"
#include "rendering/spell_visual_system.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "game/stock_death_rules.hpp"
#include "rendering/terrain_manager.hpp"
#include "game/zone_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "game/wmo_area.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/camera.hpp"
#include "rendering/stock_screen_fx.hpp"
#include "audio/audio_engine.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "game/game_handler.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <random>

namespace wowee {
namespace rendering {

namespace {

std::mt19937& fxRng() {
    static std::mt19937 gen(std::random_device{}());
    return gen;
}
float fxRand() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(fxRng()); }

// Thunder: the stock Blasted Lands strike samples (their doodads' own), a
// sharp crack for a near strike and three low rumbles for the rest.
constexpr const char* kThunderCrack = "Sound\\Doodad\\BlastedLandsLightningBolt01Stand-Bolt.wav";
constexpr const char* kThunderRumbles[] = {
    "Sound\\Doodad\\BlastedLandsLightningBolt01Stand-Bolt1.wav",
    "Sound\\Doodad\\BlastedLandsLightningBolt01Stand-Bolt2.wav",
    "Sound\\Doodad\\BlastedLandsLightningBolt01Stand-Bolt3.wav",
};

}  // namespace

weather_fx::Forced Renderer::weatherPreview() const {
    return weather_fx::previewWeather(enh_.weatherPreview);
}

void Renderer::effectiveWeather(uint32_t& state, float& grade) const {
    const weather_fx::Forced f = weatherPreview();
    if (f.active) {
        state = f.state;
        grade = f.grade;
        return;
    }
    const auto* gh = core::Application::getInstance().getGameHandler();
    state = gh ? gh->getWeatherType() : 0;
    grade = gh ? gh->getWeatherIntensity() : 0.0f;
}

const stock_weather::WeatherRow* Renderer::weatherRowFor(uint32_t state) {
    if (!weatherRowsLoaded_) {
        weatherRowsLoaded_ = true;
        auto* am = core::Application::getInstance().getAssetManager();
        auto dbc = am ? am->loadDBCOptional("Weather.dbc") : nullptr;
        // 3.3.5 (and 2.4.3): ID, AmbienceID, EffectType, the light scale,
        // Color[3], EffectTexture - 8 fields. Another width is skipped and
        // the state's number decides.
        if (dbc && dbc->isLoaded() && dbc->getFieldCount() == 8) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                stock_weather::WeatherRow row;
                row.known = true;
                row.effect = dbc->getUInt32(i, 2);
                row.lightScale = dbc->getFloat(i, 3);
                for (int c = 0; c < 3; ++c) row.colour[c] = dbc->getFloat(i, 4 + static_cast<uint32_t>(c));
                row.texture = dbc->getString(i, 7);
                weatherRows_[dbc->getUInt32(i, 0)] = std::move(row);
            }
            LOG_INFO("Weather.dbc: ", weatherRows_.size(), " rows");
        } else if (dbc && dbc->isLoaded()) {
            LOG_WARNING("Weather.dbc has ", dbc->getFieldCount(), " fields, not 8; the weather kind "
                        "follows the state number");
        }
    }
    auto it = weatherRows_.find(state);
    return it == weatherRows_.end() ? nullptr : &it->second;
}

float Renderer::easeWeatherLightScale(float target, bool snap, float dt) {
    if (snap) {
        lightScaleFrom_ = lightScaleTo_ = lightScale_ = target;
        lightScaleElapsed_ = lightScaleSeconds_ = 0.0f;
        return lightScale_;
    }
    if (target != lightScaleTo_) {
        lightScaleFrom_ = lightScale_;
        lightScaleTo_ = target;
        lightScaleElapsed_ = 0.0f;
        lightScaleSeconds_ = stock_weather::lightScaleSeconds(lightScaleFrom_, lightScaleTo_);
    }
    lightScaleElapsed_ += std::max(dt, 0.0f);
    const float t = lightScaleSeconds_ > 0.0f ? std::min(lightScaleElapsed_ / lightScaleSeconds_, 1.0f) : 1.0f;
    lightScale_ = lightScaleFrom_ + (lightScaleTo_ - lightScaleFrom_) * t;
    return lightScale_;
}

void Renderer::applyScreenEffectFog() {
    const auto* gh = core::Application::getInstance().getGameHandler();
    const bool nether = gh && gh->getScreenEffect().id != 0 &&
        gh->getScreenEffect().effect == static_cast<uint32_t>(game::stock_death::ScreenEffectKind::Nether);
    // The post pass's fade: up by the frame's time to 0.75 (0xa4179c), and
    // gone with the effect.
    netherFade_ = nether ? std::min(netherFade_ + lastDeltaTime_, 0.75f) : 0.0f;
    if (postProcessPipeline_) postProcessPipeline_->setNether(netherFade_);
    if (!nether) return;
    // 0x7ed870(0.7, 150, 0xff4c4c63, 0): the fog colour and its end 150 yd,
    // its start 0.7 of that.
    currentFrameData.fogColor = glm::vec4(glm::vec3(0x4c, 0x4c, 0x63) / 255.0f, currentFrameData.fogColor.w);
    currentFrameData.fogParams.x = 0.7f * 150.0f;
    currentFrameData.fogParams.y = 150.0f;
}

void Renderer::updateWeatherFx(float fxDt) {
    if (!weather) return;

    Weather::Style style;
    style.intensity = enh_.weatherIntensity;
    style.variedRain = enh_.variedRain;
    style.softSnow = enh_.softSnow;
    style.mistSheets = enh_.weatherSheets;
    weather->setStyle(style);
    if (lightingManager) weather->setFogColour(lightingManager->getLightingParams().fogColor);

    const Weather::Type type = weather->getWeatherType();
    const float grade = glm::clamp(weather->getIntensity(), 0.0f, 1.0f);
    const bool falling = type != Weather::Type::NONE && grade > 0.05f;
    const float hours = lightingManager ? lightingManager->getVisualTimeOfDayHours() : 12.0f;
    const uint8_t climate = enh_.fireflies ? weather_fx::climateForZone(getCurrentZoneId())
                                          : static_cast<uint8_t>(weather_fx::kTemperate);

    // Fireflies: out of doors, in their country, at night, dry.
    weather->setFireflies(enh_.fireflies
        ? weather_fx::fireflyAmount(climate, hours, falling, playerIndoors_ || cameraUnderwater_)
        : 0.0f);

    // Snow settling: from the weather, outdoors or in (the roofs outside
    // keep it while the player is in the inn).
    snowCover_ = enh_.snowCover
        ? weather_fx::easeSnowCover(snowCover_, weather_fx::snowCoverTarget(type == Weather::Type::SNOW, grade), fxDt)
        : 0.0f;

    // A sandstorm's haze, eased over a few seconds.
    {
        const float target = (enh_.sandstorms && type == Weather::Type::SAND)
            ? stock_screen_fx::particleLevel(grade) : 0.0f;
        const float k = 1.0f - std::exp(-std::max(fxDt, 0.0f) / 3.0f);
        sandHaze_ += (target - sandHaze_) * k;
        if (target <= 0.0f && sandHaze_ < 0.002f) sandHaze_ = 0.0f;
    }

    // Lightning: strikes on a timer while a thunderstorm falls; each flash
    // lights the frame for under a second and queues its thunder.
    if (enh_.stormLightning && weather_fx::stormHasLightning(type == Weather::Type::STORM, grade)) {
        if (nextStrikeIn_ <= 0.0f) nextStrikeIn_ = weather_fx::strikeInterval(grade, fxRand(), enh_.weatherIntensity) * 0.5f;
        nextStrikeIn_ -= fxDt;
        if (nextStrikeIn_ <= 0.0f) {
            const float dist = weather_fx::strikeDistance(fxRand());
            strikeStrength_ = weather_fx::flashStrength(dist);
            strikeSeed_ = fxRand();
            strikeClock_ = 0.0f;
            pendingThunder_.push_back({weather_fx::thunderDelay(dist), weather_fx::thunderVolume(dist),
                                       dist < weather_fx::kCrackDistance});
            nextStrikeIn_ = weather_fx::strikeInterval(grade, fxRand(), enh_.weatherIntensity);
            LOG_INFO("Lightning: strike ", static_cast<int>(dist), " yd off, flash ", strikeStrength_,
                     ", thunder in ", weather_fx::thunderDelay(dist), " s, next in ", nextStrikeIn_, " s");
        }
    } else {
        nextStrikeIn_ = 0.0f;
    }
    if (strikeClock_ >= 0.0f) {
        strikeClock_ += fxDt;
        // Indoors the flash is what comes through the windows.
        const float indoor = 1.0f - 0.85f * glm::clamp(indoorFactor_, 0.0f, 1.0f);
        lightningFlash_ = strikeStrength_ * indoor * weather_fx::flashEnvelope(strikeClock_, strikeSeed_);
        if (strikeClock_ > 1.0f) {
            strikeClock_ = -1.0f;
            lightningFlash_ = 0.0f;
        }
    } else {
        lightningFlash_ = 0.0f;
    }
    if (!pendingThunder_.empty()) {
        for (auto& t : pendingThunder_) t.delay -= fxDt;
        for (const auto& t : pendingThunder_) {
            if (t.delay > 0.0f) continue;
            if (!enh_.stormLightning) continue;
            audio::SoundOptions opts;
            opts.category = audio::stock::Category::Ambience;
            opts.priority = audio::stock::kPriorityZone;
            const char* path = t.crack ? kThunderCrack
                                       : kThunderRumbles[static_cast<int>(fxRand() * 2.999f)];
            const float indoor = 1.0f - 0.4f * glm::clamp(indoorFactor_, 0.0f, 1.0f);
            audio::AudioEngine::instance().playSound2D(path, t.volume * indoor, 0.85f + 0.2f * fxRand(),
                                                       audio::SoundBus::World, opts);
        }
        std::erase_if(pendingThunder_, [](const PendingThunder& t) { return t.delay <= 0.0f; });
    }

    breathClock_ += fxDt;
    // Instances come and go; the areas are cheap to ask again.
    if (breathAreas_.size() > 512) breathAreas_.clear();

    // WOWEE_WEATHER_LOG=1: the weather's state every two seconds, for
    // checking the rows in a scripted run.
    static const bool logWeather = [] {
        const char* v = std::getenv("WOWEE_WEATHER_LOG");
        return v && *v && *v != '0';
    }();
    if (logWeather) {
        static float logClock = 0.0f;
        logClock += fxDt;
        if (logClock >= 2.0f) {
            logClock = 0.0f;
            LOG_INFO("Weather fx: type ", static_cast<int>(type), " grade ", grade, " enabled ",
                     weather->isEnabled(), " particles ", weather->getParticleCount(), " sheets ",
                     weather->mistSheetCount(), " fireflies ", weather->fireflyCount(), " snowCover ",
                     snowCover_, " sandHaze ", sandHaze_, " zone ", getCurrentZoneId(), " mist ",
                     weather->stockMistCount(), " grains ", weather->sandGrainCount(), " breath areas ",
                     breathAreas_.size());
        }
    }
}

bool Renderer::unitInColdArea(uint32_t instanceId, const glm::vec3& position) {
    auto& slot = breathAreas_[instanceId];
    if (slot.recheckAt > breathClock_) return slot.cold;
    slot.recheckAt = breathClock_ + stock_weather::kColdRecheckSeconds;
    slot.cold = false;
    slot.underwater = false;
    // 0x71fa90 asks the liquid first: deep under its surface, the bubbles
    // (bit 0x20) and not the cold. Taken here as the surface over the
    // unit's breath point.
    if (waterRenderer) {
        glm::vec3 breathAt = position + glm::vec3(0.0f, 0.0f, 1.7f);
        if (characterRenderer) {
            glm::mat4 m;
            if (characterRenderer->getAttachmentTransform(instanceId, 17, m)) breathAt = glm::vec3(m[3]);
        }
        if (auto surface = waterRenderer->getNearestWaterHeightAt(breathAt.x, breathAt.y, breathAt.z, 30.0f)) {
            if (*surface > breathAt.z) {
                slot.underwater = true;
                return false;
            }
        }
    }
    if (!zoneManager) return false;
    // The unit's area as the player's is found (updateLiveArea): the map
    // object group's WMOAreaTable row when it stands on that group's floor,
    // else the terrain's (0x78F1F0: 0x77F1B0, else 0x782560).
    uint32_t area = 0;
    if (instanceId == characterInstanceId && liveAreaValid_) {
        area = liveAreaId_;
    } else {
        if (terrainManager) {
            if (auto a = terrainManager->getAreaIdAt(position.x, position.y)) area = *a;
        }
        WMORenderer::AreaHit hit;
        if (wmoRenderer && wmoRenderer->insideWmoArea(position.x, position.y, position.z + 1.0f, &hit) &&
            hit.wmoId != 0 && hit.wmoGroupId != 0) {
            const game::WmoAreaInfo info =
                zoneManager->resolveWmoArea(hit.wmoId, hit.nameSet, static_cast<int32_t>(hit.wmoGroupId));
            if (info.areaId != 0) {
                const auto floor = wmoRenderer->getFloorHeight(position.x, position.y, position.z + 2.0f,
                                                               nullptr, position.z);
                std::optional<float> ground;
                if (terrainManager && !terrainManager->isHoleAt(position.x, position.y))
                    ground = terrainManager->getHeightAt(position.x, position.y);
                if (game::wmoDecidesArea(position.z, floor, ground)) area = info.areaId;
            }
        }
    }
    if (area == 0) return false;
    const uint32_t zone = zoneManager->resolveAreaZoneId(area);
    const bool hasParent = zone != 0 && zone != area;
    slot.cold = stock_weather::areaIsCold(zoneManager->getAreaFlags(area), hasParent,
                                          hasParent ? zoneManager->getAreaFlags(zone) : 0u);
    return slot.cold;
}

void Renderer::onUnitBreathEvent(uint32_t instanceId, const glm::vec3& position) {
    if (!spellVisualSystem_ || !camera) return;
    // Units far off are not worth the model; stock plays it on every unit
    // whose model animates.
    if (glm::distance(position, camera->getPosition()) > 100.0f) return;
    bool cold = unitInColdArea(instanceId, position);
    const bool underwater = breathAreas_[instanceId].underwater;
    // "Breath in the cold": the stock breath wherever snow is falling too.
    if (!cold && enh_.coldBreath && weather && weather->getWeatherType() == Weather::Type::SNOW &&
        stock_screen_fx::particleLevel(weather->getIntensity()) > 0.0f) {
        cold = true;
    }
    // The drunk bubbles: the local player's drunk level (other players'
    // PLAYER_BYTES_3 are not read here).
    const bool isPlayer = instanceId == characterInstanceId;
    const char* model = nullptr;
    switch (stock_weather::breathFor(isPlayer, isPlayer ? drunkLevel_ : 0.0f, underwater, cold)) {
        case stock_weather::Breath::DRUNK:      model = stock_weather::kDrunkBreathModel; break;
        case stock_weather::Breath::UNDERWATER: model = stock_weather::kUnderwaterBreathModel; break;
        case stock_weather::Breath::COLD:       model = stock_weather::kColdBreathModel; break;
        case stock_weather::Breath::NONE:       return;
    }
    spellVisualSystem_->playEffectModel(model, position, instanceId, SpellVisualSystem::SLOT_BREATH);
}

void Renderer::applyWeatherFxLight() {
    if (lightningFlash_ > 0.001f) {
        // A cold white-blue over the land and the haze for a moment.
        const glm::vec3 flash = glm::vec3(0.78f, 0.84f, 1.0f) * lightningFlash_;
        currentFrameData.lightColor += glm::vec4(flash * 0.9f, 0.0f);
        currentFrameData.ambientColor += glm::vec4(flash * 0.5f, 0.0f);
        currentFrameData.fogColor += glm::vec4(flash * 0.35f, 0.0f);
    }
    if (sandHaze_ > 0.0f) {
        const float k = weather_fx::sandFogScale(sandHaze_, enh_.weatherIntensity);
        currentFrameData.fogParams.x *= k;
        currentFrameData.fogParams.y *= k;
    }
    // A nether screen effect takes the fog over (stock, not a row).
    applyScreenEffectFog();
}

}  // namespace rendering
}  // namespace wowee
