#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <optional>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>

#include "rendering/vk_texture.hpp"

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class Camera;
class VkContext;

/**
 * @brief Rain and snow, drawn the way the stock 3.3.5 client draws them
 *
 * - Rain: textured streaks (Textures\Weather\RainDrop01.blp) stretched along
 *   the direction they fall, around the camera, with splashes
 *   (RainDropSplash01.blp, a 4x4 sheet: four frames across, four view
 *   angles down)
 *   where they land near the camera.
 * - Snow: camera-facing flakes (SnowFlake01.blp) drifting down.
 * - How many falls by the server's weather intensity, times the Weather
 *   Detail setting, past the stock quarter-grade threshold. Rain multiplies
 *   the scene (Mod2x); snow is white and unlit, as stock draws them.
 * - The stock mist (MapWeather 0x78c420, 0x786e10): 12 yd sheets in the fog
 *   colour blowing back past the player, past half the level for rain and
 *   snow, at any level for sand.
 * - Sand: the stock grains (0x785ea0, sand.bls), fog-coloured points that
 *   blow at the player from 34-40 yd ahead.
 *
 * Particles live in a box around the camera that wraps as the camera moves,
 * so the fall stays even while the player runs. World space is Z-up.
 */
class Weather {
public:
    enum class Type {
        NONE,
        RAIN,
        SNOW,
        STORM,
        /// A sandstorm (Weather.dbc EffectType 3): the stock sand grains and
        /// the grainy mist (stock_weather.hpp); the "Sandstorms" row adds
        /// its haze and wind on top.
        SAND
    };

    /// The weather enhancements this draws (docs/enhancements-plan.md,
    /// "Enhancements: Weather"; weather_fx.hpp). A default Style is the stock
    /// rain and snow: the same counts, sizes, splash rule and nothing else.
    struct Style {
        float intensity = 1.0f;   ///< "Weather intensity": times the stock count
        bool variedRain = false;  ///< drop sizes and streaks vary; splashes on the move, further out
        bool softSnow = false;    ///< flake sizes vary, flakes turn and eddy
        bool mistSheets = false;  ///< taller distant sheets of rain, snow or dust beyond the stock mist
        bool operator==(const Style&) const = default;
    };

    /// Ground height under a point: (x, y, z to look down from) -> height.
    using GroundQuery = std::function<std::optional<float>(float x, float y, float fromZ)>;

    Weather();
    ~Weather();

    /**
     * @brief Initialize weather system
     * @param ctx Vulkan context
     * @param perFrameLayout Descriptor set layout for the per-frame UBO (set 0)
     * @return true if initialization succeeded
     */
    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);

    void recreatePipelines();

    /// The pass the next recreatePipelines() builds for: the scene pass at
    /// its sample count (null, the default), or the single-sampled water pass
    /// the soft particles move the water into, where the weather follows it so
    /// that the blended particles stay under both (Renderer::
    /// syncSwimEffectsTargetPass).
    void setTargetPass(VkRenderPass pass, VkSampleCountFlagBits samples) {
        targetPass_ = pass;
        targetSamples_ = samples;
    }

    /// The pipeline state both initialize() and recreatePipelines() need.
    void buildPipelines(VkDevice device,
                        const VkPipelineShaderStageCreateInfo& vertStage,
                        const VkPipelineShaderStageCreateInfo& fragStage);

    /**
     * @brief Update weather particles and build this frame's quads
     * @param camera Camera for particle positioning and facing
     * @param deltaTime Time since last frame
     */
    void update(const Camera& camera, float deltaTime);

    /**
     * @brief Render weather particles
     * @param cmd Command buffer to record into
     * @param perFrameSet Per-frame descriptor set (set 0, contains camera UBO)
     */
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

    [[nodiscard]] Type getWeatherType() const { return weatherType; }
    [[nodiscard]] float getIntensity() const { return intensity; }

    /// Follow the server's weather (SMSG_WEATHER) as the stock weather object
    /// does (0x7846a0, 0x784850): the grade moves linearly to each new one
    /// over (|change| + 0.001) * 10 s rather than snapping, and a change of
    /// kind fades the old weather out before the new one starts (0x78d170).
    /// Fine weather is NONE, which fades the current kind out. Call every
    /// frame.
    void followServerWeather(Type type, float grade, float deltaTime);

    /// How much of the weather to draw, as a fraction of what the current
    /// weather would use. The game's Weather Detail setting, which is a
    /// quality knob rather than a forecast: it says how heavily to draw the
    /// rain, not how hard it is raining.
    void setDensityScale(float scale) {
        densityScale_ = std::clamp(scale, 0.0f, 1.0f);
    }
    [[nodiscard]] float densityScale() const { return densityScale_; }

    /// Where the ground is, for the splashes. Without one, rain falls through
    /// and nothing splashes.
    void setGroundQuery(GroundQuery query) { groundQuery_ = std::move(query); }
    [[nodiscard]] bool hasGroundQuery() const { return static_cast<bool>(groundQuery_); }

    /// E19, "Rain and snow in the wind" (weather_wind.hpp): the foliage's
    /// eased wind strength (0: the stock fall, nothing added) and the scene
    /// clock its gust band runs on. Call before update().
    void setWind(float strength, float clock) {
        windStrength_ = strength;
        windClock_ = clock;
    }

    /// The enhancements' style (Style{} is stock). Call before update().
    void setStyle(const Style& style) { style_ = style; }
    [[nodiscard]] const Style& style() const { return style_; }
    /// Where the player is and which way it faces (radians, 0 = +x): the
    /// stock mist and sand blow from its heading (stock_weather::heading).
    /// Call before update().
    void setPlayer(const glm::vec3& position, float facing) {
        playerPos_ = position;
        playerFacing_ = facing;
        havePlayer_ = true;
    }
    /// The fog colour (Light.dbc), which the mist sheets and the sand take,
    /// as the stock mist and sand are drawn in it (world-lighting.md 8).
    void setFogColour(const glm::vec3& colour) { fogColour_ = colour; }
    /// Fireflies around the camera ("Fireflies at night"): 0 none - nothing
    /// is kept or drawn - to 1 the most. Independent of the weather.
    void setFireflies(float amount) { fireflyAmount_ = std::clamp(amount, 0.0f, 1.0f); }
    [[nodiscard]] size_t fireflyCount() const { return fireflies_.size(); }
    [[nodiscard]] size_t mistSheetCount() const { return sheets_.size(); }
    /// The stock mist's sheets and sand grains alive now.
    [[nodiscard]] size_t stockMistCount() const { return mist_.size(); }
    [[nodiscard]] size_t sandGrainCount() const { return grains_.size(); }

    /// Whether the weather is drawn (the camera is not sealed indoors). Off,
    /// it is still simulated, as stock updates its emitters and only skips
    /// the draw (0x78ae20 and the others return after the update).
    void setEnabled(bool enabled) { this->enabled = enabled; }
    [[nodiscard]] bool isEnabled() const { return enabled; }

    [[nodiscard]] int getParticleCount() const;

    /**
     * @brief Zone weather configuration
     * Provides default weather per zone for single-player mode.
     * When connected to a server, SMSG_WEATHER overrides these.
     */
    struct ZoneWeather {
        Type type = Type::NONE;
        float minIntensity = 0.0f;
        float maxIntensity = 0.0f;
        float probability = 0.0f;
    };

    void setZoneWeather(uint32_t zoneId, Type type, float minIntensity, float maxIntensity, float probability);
    void updateZoneWeather(uint32_t zoneId, float deltaTime);
    void initializeZoneWeatherDefaults();

    void shutdown();

private:
    struct Particle {
        glm::vec3 position;
        glm::vec3 velocity;
        float groundZ = -1.0e9f;   ///< Where it lands; below this it respawns
        float phase = 0.0f;        ///< Snow sway
        float size = 1.0f;         ///< Scale on the type's sprite size
        bool lands = false;        ///< groundZ is real ground: splash there
        // Style only; stock leaves these at 1, 1 and 0.
        float length = 1.0f;       ///< streak length scale (varied rain)
        float alpha = 1.0f;        ///< opacity (varied rain, soft snow)
        float spin = 0.0f;         ///< turn rate, rad/s (soft snow)
    };
    struct Splash {
        glm::vec3 position;
        float age = 0.0f;
        float size = 1.0f;         ///< the drop's size (varied rain), else 1
    };
    /// A distant sheet of rain, snow or dust (Style::mistSheets).
    struct MistSheet {
        glm::vec3 base;            ///< bottom centre
        glm::vec3 velocity;
        float age = 0.0f;
        float life = 8.0f;
        float halfWidth = 6.0f;
        float height = 24.0f;
        float peak = 0.5f;
        bool flipU = false;
    };
    /// One sheet of the stock mist (stock_weather.hpp).
    struct StockMist {
        glm::vec3 position;        ///< centre
        glm::vec3 velocity;
        float accel = 0.0f;        ///< on every axis, yd/s^2 (0x128)
        float age = 0.0f;
        float life = 2.7f;
        glm::vec3 groundAt[3];     ///< the ground under its path: start, middle, end (x, y, ground z)
    };
    /// One stock sand grain: where it was born, its velocity, and when its
    /// path ends (it dies where it meets the ground).
    struct Grain {
        glm::vec3 origin;
        glm::vec3 velocity;
        float born = 0.0f;
        float life = 0.0f;
    };
    struct Firefly {
        glm::vec3 position;
        glm::vec3 home;            ///< where it wanders about
        float phase = 0.0f;
        float rate = 1.0f;         ///< blink speed
        float age = 0.0f;
    };
    struct Vertex {
        glm::vec3 pos;
        glm::vec2 uv;
        float alpha;
    };
    static_assert(sizeof(Vertex) == 24, "Weather vertex size");

    enum Sprite : uint32_t { RAIN_STREAK = 0, RAIN_SPLASH, SNOW_FLAKE, MIST_SHEET, SAND_MIST, WHITE, SPRITE_COUNT };

    void loadTextures(pipeline::AssetManager* assetManager);
    void spawnParticle(Particle& p, const glm::vec3& cameraPos, bool anywhereInColumn);
    float resolveGround(const glm::vec3& at, const glm::vec3& cameraPos) const;
    void buildVertices(const Camera& camera);
    void spawnSheet(MistSheet& s, const glm::vec3& cameraPos);
    void updateSheets(const glm::vec3& cameraPos, float level, float deltaTime);
    void updateFireflies(const glm::vec3& cameraPos, float deltaTime);
    void updatePlayerMotion(float deltaTime);
    void updateStockMist(const glm::vec3& cameraPos, float level, float deltaTime);
    void spawnStockMist(StockMist& m, const glm::vec3& cameraPos);
    void updateSand(const glm::vec3& cameraPos, float level, float deltaTime);
    void spawnGrain(Grain& g, const glm::vec3& cameraPos);
    /// Grow the per-frame vertex buffers to hold `vertexCount` (the
    /// intensity slider and the extra sprites can need more than the stock
    /// cap); the old ones are freed once no frame in flight reads them.
    bool ensureVertexCapacity(size_t vertexCount);

    // Vulkan objects
    VkContext* vkCtx = nullptr;
    VkRenderPass targetPass_ = VK_NULL_HANDLE;  // null: the scene pass
    VkSampleCountFlagBits targetSamples_ = VK_SAMPLE_COUNT_1_BIT;
    VkPipeline pipeline = VK_NULL_HANDLE;
    /// Rain and splashes: the stock GxBlend_Mod2x (dst = 2 * src * dst),
    /// RainDrop01/RainDropSplash01 being grey 128 at rest (MapWeather 0x7891b0,
    /// 0x78a030: blend 5, fog colour 0x80808080).
    VkPipeline mod2xPipeline_ = VK_NULL_HANDLE;
    /// Fireflies: additive, so they glow over the night.
    VkPipeline addPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout textureSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, SPRITE_COUNT> textureSets_{};
    std::array<VkTexture, SPRITE_COUNT> textures_;
    std::array<bool, SPRITE_COUNT> textureReady_{};
    bool texturesAttempted_ = false;

    // One mapped vertex buffer per frame in flight: the GPU may still be
    // reading last frame's while this one is written.
    struct FrameBuffer {
        ::VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VmaAllocationInfo info{};
    };
    std::vector<FrameBuffer> frameBuffers_;   ///< MAX_FRAMES_IN_FLIGHT of them
    VkDeviceSize frameBufferSize_ = 0;

    // Particles and this frame's geometry
    std::vector<Particle> particles;
    std::vector<Splash> splashes_;
    std::vector<Vertex> vertices_;
    uint32_t stockMistVertexCount_ = 0;  ///< The stock mist first, then
    uint32_t sheetVertexCount_ = 0;      ///< the row's sheets, then
    uint32_t particleVertexCount_ = 0;   ///< streaks or flakes, then splashes,
    uint32_t splashVertexCount_ = 0;
    uint32_t grainVertexCount_ = 0;      ///< then sand grains
    uint32_t fireflyVertexCount_ = 0;    ///< then fireflies
    std::vector<MistSheet> sheets_;
    std::vector<StockMist> mist_;
    std::vector<Grain> grains_;
    float mistAccum_ = 0.0f;   ///< sheets owed by the spawn rate
    float grainAccum_ = 0.0f;
    glm::vec3 playerPos_{0.0f};
    glm::vec3 lastPlayerPos_{0.0f};
    glm::vec3 playerVelocity_{0.0f};
    float playerFacing_ = 0.0f;
    bool havePlayer_ = false;
    bool haveLastPlayer_ = false;
    std::vector<Firefly> fireflies_;
    Type builtType_ = Type::NONE;
    glm::vec3 lastCameraPos_{0.0f};
    bool haveLastCamera_ = false;
    glm::vec3 cameraVelocity_{0.0f};   ///< Averaged, yards a second: gates splashes
    GroundQuery groundQuery_;

    // Weather parameters
    bool enabled = false;
    Type weatherType = Type::NONE;
    Type particlesType_ = Type::NONE;
    float intensity = 0.5f;
    float densityScale_ = 1.0f;
    float time_ = 0.0f;
    float windStrength_ = 0.0f;  ///< setWind; 0 is stock
    float windClock_ = 0.0f;
    Style style_{};
    glm::vec3 fogColour_{0.5f};
    float fireflyAmount_ = 0.0f;

    static constexpr int MAX_PARTICLES = 3000;
    /// Sand grains alive at most. Stock keeps about 67000 at weatherDensity 2
    /// (32000 a second times 0.66 for 3.2 s) on the GPU; these are built on
    /// the CPU, so fewer, each as large as stock's.
    static constexpr int MAX_GRAINS = 9000;
    static constexpr int MAX_SPLASHES = 400;

    // Zone-based weather
    std::unordered_map<uint32_t, ZoneWeather> zoneWeatherTable_;
    uint32_t currentWeatherZone_ = 0;
    float zoneWeatherTimer_ = 0.0f;
    float zoneWeatherCycleDuration_ = 0.0f;
    bool zoneWeatherActive_ = false;
    float targetIntensity_ = 0.0f;
    bool zoneWeatherInitialized_ = false;

    /// The grade's current transition (stock 0x784850), restarted whenever
    /// the target changes: from where it stood, over particleTransitionSeconds.
    void easeGrade(float target, float deltaTime);
    float gradeFrom_ = 0.0f;
    float gradeTo_ = 0.0f;
    float gradeElapsed_ = 0.0f;
    float gradeDuration_ = 0.0f;
};

} // namespace rendering
} // namespace wowee
