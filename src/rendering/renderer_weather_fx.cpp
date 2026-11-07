// The "Enhancements: Weather" rows on the renderer's side (docs/enhancements-
// plan.md E23-E31; the rules are rendering/weather_fx.hpp): the weather's
// style, the client-only preview, snow settling, a sandstorm's haze,
// lightning and its thunder, breath in the cold and the fireflies.
//
// With every row at its stock value nothing here allocates, spawns, plays or
// changes a number: updateWeatherFx hands the weather a default Style and a
// zero firefly count, and applyWeatherFxLight leaves the frame alone.

#include "rendering/renderer.hpp"
#include "rendering/weather.hpp"
#include "rendering/weather_fx.hpp"
#include "rendering/footfall_dust.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/camera.hpp"
#include "rendering/stock_screen_fx.hpp"
#include "audio/audio_engine.hpp"
#include "core/application.hpp"
#include "core/entity_spawner.hpp"
#include "core/logger.hpp"
#include "game/game_handler.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <random>
#include <unordered_set>

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

// The M2 attachment a breath comes from (stock $BTH: attachment 17), and the
// head's as a fallback for models that carry no breath point.
constexpr uint32_t kAttachBreath = 17;
constexpr uint32_t kAttachHead = 11;
constexpr float kBreathRange = 32.0f;

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
    const bool anyClimateRow = enh_.fireflies || enh_.coldBreath;
    const uint8_t climate = anyClimateRow ? weather_fx::climateForZone(getCurrentZoneId())
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

    updateBreath(fxDt, climate, type == Weather::Type::SNOW && falling, hours);

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
                     snowCover_, " sandHaze ", sandHaze_, " zone ", getCurrentZoneId(), " breathing ",
                     nextBreath_.size());
        }
    }
}

void Renderer::updateBreath(float fxDt, uint8_t climate, bool snowing, float hours) {
    // Made on first use and released a few frames after the row goes off,
    // once no frame in flight can still draw from it - as the footfall dust.
    if (!enh_.coldBreath) {
        nextBreath_.clear();
        if (breathPuffs_) {
            breathPuffs_->clear();
            if (++breathIdleFrames_ > MAX_FRAMES + 1) breathPuffs_.reset();
        }
        return;
    }
    breathIdleFrames_ = 0;
    if (!camera) return;
    const bool shows = weather_fx::breathShows(climate, snowing, hours) && !cameraUnderwater_;
    if (!shows) {
        nextBreath_.clear();
        if (breathPuffs_) breathPuffs_->update(fxDt, *camera);  // let the last ones fade
        return;
    }
    if (!breathPuffs_ && !breathFailed_ && vkCtx) {
        breathPuffs_ = std::make_unique<FootfallDust>();
        if (!breathPuffs_->initialize(vkCtx, perFrameSetLayout)) {
            LOG_WARNING("Breath puffs initialization failed (non-fatal)");
            breathPuffs_.reset();
            breathFailed_ = true;
        }
    }
    if (!breathPuffs_) return;

    // Who is close by, a few times a second: the player and the units the
    // spawner drew within range of the camera.
    breathScanClock_ += fxDt;
    if (breathScanClock_ >= 0.25f && characterRenderer) {
        const float elapsed = breathScanClock_;
        breathScanClock_ = 0.0f;
        const glm::vec3 camPos = camera->getPosition();
        std::unordered_set<uint32_t> nearUnits;
        auto consider = [&](uint32_t inst) {
            if (inst == 0 || nearUnits.count(inst)) return;
            glm::vec3 p;
            if (!characterRenderer->getInstancePosition(inst, p)) return;
            if (glm::distance(p, camPos) > kBreathRange) return;
            nearUnits.insert(inst);
        };
        consider(characterInstanceId);
        auto& app = core::Application::getInstance();
        const auto* gh = app.getGameHandler();
        const core::EntitySpawner* spawner = app.getEntitySpawner();
        if (gh && spawner) {
            for (const auto& [guid, entity] : gh->getEntityManager().getEntities()) {
                if (!entity || !entity->isUnit()) continue;
                consider(spawner->characterInstanceIdForGuid(guid));
                if (nearUnits.size() >= 48) break;
            }
        }
        // Each unit breathes on its own clock; one met for the first time
        // starts part way through its breath.
        std::erase_if(nextBreath_, [&](const auto& kv) { return nearUnits.count(kv.first) == 0; });
        for (uint32_t inst : nearUnits) {
            auto [it, fresh] = nextBreath_.try_emplace(inst, weather_fx::breathInterval(0.0f, fxRand()) * fxRand());
            if (!fresh) it->second -= elapsed;
            if (it->second > 0.0f) continue;
            it->second = weather_fx::breathInterval(0.0f, fxRand());
            glm::mat4 m;
            glm::vec3 mouth, facing;
            if (characterRenderer->getAttachmentTransform(inst, kAttachBreath, m)) {
                mouth = glm::vec3(m[3]);
                facing = glm::vec3(m[0]);
            } else if (characterRenderer->getAttachmentTransform(inst, kAttachHead, m)) {
                facing = glm::vec3(m[0]);
                mouth = glm::vec3(m[3]) + glm::normalize(facing) * 0.15f - glm::vec3(0.0f, 0.0f, 0.1f);
            } else {
                continue;
            }
            glm::vec2 dir(facing);
            const float len = glm::length(dir);
            dir = len > 1e-4f ? dir / len : glm::vec2(1.0f, 0.0f);
            footfall_dust::Puff puff;
            puff.sprites = 1;
            puff.alpha = 0.16f + 0.06f * fxRand();
            puff.size = 0.05f;
            puff.life = 1.3f + 0.4f * fxRand();
            // The dust kicks a puff back from the stride; a breath goes out
            // ahead of the face, so it is handed the reverse direction.
            breathPuffs_->spawn(mouth - glm::vec3(0.0f, 0.0f, puff.size * 0.7f), -dir, puff,
                                glm::vec3(0.86f, 0.89f, 0.93f));
        }
    }
    breathPuffs_->update(fxDt, *camera);
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
}

}  // namespace rendering
}  // namespace wowee
