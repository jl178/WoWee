#pragma once

#include "rendering/mist_wind.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <cstdint>

namespace wowee {
namespace rendering {

class VkContext;

/**
 * Fog that light moves through: lit by the sun past the shadow map, so trees
 * and buildings cast shafts through it, by the zone's own fog colour where
 * the sun does not reach, and by the torches, braziers and lava the renderer
 * already gathers for the surfaces around them.
 *
 * It is a volume over the camera's frustum - a grid of cells, 'froxels', each
 * a pixel tile deep by a slice of distance, the slices growing exponentially
 * so the near air is fine and the far air coarse. Two compute passes build it
 * every frame after the shadow map:
 *
 *   inject     fills each cell with how much its air scatters and absorbs
 *              and how much light arrives there, sampled at a jittered point
 *              and blended with last frame's cell, reprojected, so the jitter
 *              averages out instead of crawling;
 *   integrate  walks each column away from the camera and stores, at every
 *              depth, the light scattered toward the camera so far and how
 *              much of what lies behind still shows through.
 *
 * Every world shader then reads that volume at its own position where it used
 * to add fog alone. The zone's distance fog is kept and applied first: it is
 * the far haze the sky is painted to meet, and the volume is the air in front
 * of it - which is also what lets the sky take the volume and keep matching
 * the terrain at the horizon.
 *
 * The volumes are double-buffered per frame in flight, as the shadow maps are:
 * a frame writes its own and reads the other one's cells as history.
 */
class VolumetricFog {
public:
    /// Off, then three volume sizes. The order is persisted in the settings
    /// file as an index, so add at the end.
    enum class Quality : int { Off = 0, Low = 1, Medium = 2, High = 3 };

    /// How deep the volume reaches, in yards along the view. The air thins
    /// to nothing over its back two thirds, so the last slice holds all of it
    /// and stands for everything further, which is what the sky reads; the
    /// zone's distance fog is the haze beyond.
    ///
    /// The near air only, on purpose. A shaft is a few yards of sunlit air,
    /// and it shows only where those yards are a real share of all the air
    /// the eye looks through. Air thick enough for that all the way out
    /// whites the horizon long before a shaft reads; air that thins with
    /// distance can be thick where the trees and doorways are.
    static constexpr float kNear = 0.5f;
    static constexpr float kFar = 400.0f;
    /// The enhanced haze's reach (sunHaze): the same cells over twice the
    /// depth, so canopy shafts run on past 400 yards. Torch and lamp light
    /// is gathered only in the near kLocalLightReach of either (the inject
    /// shader), which is what kept the longer volume cheap.
    static constexpr float kFarLong = 800.0f;
    void setLongReach(bool on) { far_ = on ? kFarLong : kFar; }

    /// What one frame's volume is built from.
    struct FrameInputs {
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};
        glm::vec3 cameraPos{0.0f};
        float time = 0.0f;           ///< seconds, for the drift of the mist
        /// Extinction per yard at and below the layer's base.
        float density = 0.0f;
        /// World height the densest air sits on.
        float layerBase = 0.0f;
        /// Yards over which the density falls by a factor of e above that.
        float layerHeight = 30.0f;
        /// Share of the density still left high above the layer, so a shaft
        /// seen from a flying mount does not vanish for want of air. Kept low:
        /// the zenith looks through the whole depth of it.
        float layerFloor = 0.1f;
        /// 0 is even mist; 1 lets it thin to nothing and pile to double.
        float noiseAmount = 0.6f;
        /// Brightness of the sunlit air. Most of it goes forward, so this is
        /// mostly what the air toward the sun does.
        float sunScatter = 0.8f;
        /// Brightness of the air the sun does not reach, lit by the zone's fog
        /// colour. Half, so a shaft stands against the shade beside it rather
        /// than against a haze nearly as bright.
        float ambientScatter = 0.5f;
        /// Brightness of the glow around local lights. Well above one because
        /// a torch's reach is a dozen yards, and that little air scatters
        /// little of it.
        float localLightScatter = 8.0f;
        /// How much of the sunlit air's light goes forward, toward the sun
        /// (LightingParams::sunDisc): 0 under a sky with no sun disc, where
        /// it scatters evenly instead of glowing at a sun that is not there.
        float sunLobe = 1.0f;
        /// The reference client's canopy shafts (docs/re/enhanced-lighting.md 5c): how
        /// much more a sunlit cell scatters for the shade round it, 0 for
        /// none (the stock mist). Only under the enhanced light, outdoors.
        float shaftStrength = 0.0f;
        /// How far it is night: the shafts' light is then the moonlight the
        /// shadow map is cast from, not the sun.
        float shaftNight = 0.0f;
        /// Yards round a cell, level, that it looks for shade beside it.
        float shaftProbe = 2.5f;
        /// Yards from the camera over which the shafts are full, fading out
        /// over as many again.
        float shaftReach = 60.0f;
        /// The sky height field for the dawn rays and the trees' height
        /// round a shaft (SkyHeightField::params: its corner x, y, the height
        /// its depth 0 stands for, 1 when it holds a drawing), 0s for none.
        glm::vec4 skyField{0.0f};
        /// Toward the sun's sprite, and how far the far air takes its sun
        /// from the sprite's own direction (0 by day and with no field).
        glm::vec3 toSun{0.0f, 0.0f, 1.0f};
        float dawnRays = 0.0f;
        /// The zone's wind the mist drifts on (mist_wind::windOf), and how
        /// far weather stirs it (0-1: rain, snow or sand at its intensity).
        mist_wind::Wind wind{};
        float storm = 0.0f;
        /// The shafts as lines (enhanced_shafts::Lines): the share of the
        /// air's sky light kept by night, how far a beam is held to a thin
        /// one by night and by day, and the gain the beams left take as far
        /// as they are held. The identity leaves the mist as it was.
        glm::vec4 shaftLines{1.0f, 0.0f, 0.0f, 1.0f};
    };

    VolumetricFog() = default;
    ~VolumetricFog();
    VolumetricFog(const VolumetricFog&) = delete;
    VolumetricFog& operator=(const VolumetricFog&) = delete;

    /// The parts the per-frame descriptor layout needs before it exists: the
    /// sampler it bakes in and the neutral volume every set can bind.
    [[nodiscard]] bool initialize(VkContext* ctx);

    /// The compute passes. Set 0 is the renderer's per-frame layout, whose
    /// UBO they read; the shadow views are the per-frame-slot shadow maps.
    [[nodiscard]] bool createPipelines(VkDescriptorSetLayout perFrameLayout,
                                       const VkImageView shadowViews[2]);

    void shutdown();

    /// Takes effect at the next applyPendingQuality().
    void setQuality(Quality quality) { pendingQuality_ = quality; }
    [[nodiscard]] float farDistance() const { return far_; }

    /// Builds or frees the volumes for a quality change. Called between
    /// frames; waits for the device when there is something to change.
    /// Returns true when getVolumeView() now answers differently, so the
    /// per-frame descriptor sets must be written again.
    [[nodiscard]] bool applyPendingQuality();

    /// Volumes exist and a frame can build one.
    [[nodiscard]] bool isOn() const { return volumesReady_; }

    /// GPUPerFrameData::volumetricParams for a frame that builds its volume.
    [[nodiscard]] glm::vec4 frameParams() const;

    [[nodiscard]] VkSampler getSampler() const { return sampler_; }
    [[nodiscard]] VkImageView getNeutralView() const { return neutral_.view; }
    /// What set 0 binding 2 should hold for this frame slot: its integrated
    /// volume, or the neutral one while the fog is off.
    [[nodiscard]] VkImageView getVolumeView(uint32_t frame) const;

    /// Build this frame slot's volume. Outside any render pass, after the
    /// shadow map is drawn and left readable, and before anything samples
    /// binding 2.
    void record(VkCommandBuffer cmd, uint32_t frame, VkDescriptorSet perFrameSet,
                const FrameInputs& in);

    /// The shadow map this slot's inject reads: the renderer's front world
    /// map (rendering/shadow_cache.hpp), which changes when a redraw ends.
    /// Called when the slot's fence has been waited on and before record.
    void setShadowView(uint32_t frame, VkImageView view);

    /// The sky height field's front maps this slot's inject reads for the
    /// dawn rays and how tall the trees round a shaft stand (SkyHeightField:
    /// the tops of what stands, and the ground), or null for none
    /// (FrameInputs::skyField.w is then 0 and they are not read; a shadow
    /// map stands in so the binding is valid). Called as setShadowView is.
    void setSkyViews(uint32_t frame, VkImageView tops, VkImageView ground);

    /// The sky height field is being retired: its views are forgotten for
    /// both slots, so nothing writes them again once they are destroyed -
    /// not a quality change's rewrite of every binding, and not while this
    /// volume is off and setSkyViews is not being called. Nothing is written
    /// here (the other slot's set may be in flight); each slot's set is
    /// rewritten with the stand-in at its next setSkyViews, before its record.
    void forgetSkyViews();

    /// Forget the previous frames, after a teleport or a loading screen:
    /// reprojecting air from somewhere else would smear it across the view.
    void resetHistory() { historyValid_ = false; }

private:
    float far_ = kFar;
    static constexpr uint32_t MAX_FRAMES = 2;

    struct Volume {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    bool createVolume(Volume& v, glm::uvec3 size, VkImageUsageFlags usage,
                      VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT);
    void destroyVolume(Volume& v);
    bool createVolumes(glm::uvec3 size);
    void destroyVolumes();
    void writeComputeSets();

    VkContext* ctx_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;       // trilinear, clamped; owned by the context cache
    VkSampler depthSampler_ = VK_NULL_HANDLE;  // nearest, for reading the shadow map's depth
    Volume neutral_;                           // 1x1x1: no light scattered, everything through

    // Per frame slot: the lit cells and their running sum.
    Volume scatter_[MAX_FRAMES];
    Volume integrated_[MAX_FRAMES];
    /// Whether the sun reached each cell (R32F), for the canopy shafts: the
    /// next frame reads the other slot's as the shade round a cell, a ring
    /// of taps in a small volume instead of in the shadow map, whose scattered
    /// taps cost the Iris Xe 4 to 20 ms (docs/re/enhanced-lighting.md 5c).
    Volume visibility_[MAX_FRAMES];
    glm::uvec3 size_{0};
    bool volumesReady_ = false;
    bool formatSupported_ = true;
    bool pipelinesReady_ = false;

    Quality pendingQuality_ = Quality::Off;
    Quality builtQuality_ = Quality::Off;

    VkImageView shadowViews_[MAX_FRAMES] = {};
    VkImageView skyViews_[MAX_FRAMES][2] = {};
    /// The slot's set still names views forgetSkyViews dropped.
    bool skyViewsStale_[MAX_FRAMES] = {};
    void writeSkyViews(uint32_t frame);
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline injectPipeline_ = VK_NULL_HANDLE;
    VkPipeline integratePipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet computeSets_[MAX_FRAMES] = {};

    VkBuffer paramsUBO_[MAX_FRAMES] = {};
    VmaAllocation paramsAlloc_[MAX_FRAMES] = {};
    void* paramsMapped_[MAX_FRAMES] = {};

    // Reprojection state.
    glm::mat4 prevViewProj_{1.0f};
    glm::vec3 prevCameraPos_{0.0f};
    bool historyValid_ = false;
    uint32_t frameCounter_ = 0;
    // The mist's wind, eased, and the noise offsets it has carried.
    mist_wind::Wind wind_{};
    mist_wind::Offsets windOffsets_{};
    float windTime_ = 0.0f;
    bool windTimeValid_ = false;
    int lastRecordedFrame_ = -1;
};

} // namespace rendering
} // namespace wowee
