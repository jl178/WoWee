#pragma once

/// The bounce light's grid on the GPU (bounce_grid.hpp for what it holds):
/// gathered from the loaded world on the main thread, baked on a worker,
/// copied into a 256x256x3 RGBA16F array image outside any render pass, and
/// read by the ambient pass through its own descriptor set. Nothing here
/// exists until the "Bounce light (enhanced)" row is first on.

#include "rendering/bounce_grid.hpp"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace pipeline {
class AssetManager;
}
namespace rendering {

class VkContext;
class M2Renderer;
class WMORenderer;

class BounceGrid {
public:
    ~BounceGrid() { shutdown(); }

    /// The image, its view, sampler and set, and the staging buffers.
    /// Idempotent; false when any could not be made.
    bool ensure(VkContext* ctx);
    void shutdown();

    /// Main thread, once a frame: lands a finished bake, and starts a new one
    /// when the camera has left the grid's middle, the map changed, or the
    /// loaded world changed (checked every two seconds).
    void update(const glm::vec3& camera, float dt, const M2Renderer* m2, const WMORenderer* wmo,
                pipeline::AssetManager* assets, const std::string& mapName);

    /// Outside any render pass: the first use's layout change, and a landed
    /// bake's copy into the image.
    void recordUpload(VkCommandBuffer cmd, uint32_t frameSlot);

    [[nodiscard]] VkDescriptorSetLayout setLayout() const { return setLayout_; }
    [[nodiscard]] VkDescriptorSet set() const { return set_; }
    /// The grid's corner in world x, y, and whether the image holds a bake
    /// (the shader falls back to the plain fill until it does).
    [[nodiscard]] glm::vec2 origin() const { return origin_; }
    [[nodiscard]] bool ready() const { return ready_; }

private:
    VkContext* ctx_ = nullptr;
    bool failed_ = false;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation imageAlloc_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;  // VkContext's sampler cache owns it
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    static constexpr uint32_t kSlots = 2;  // MAX_FRAMES_IN_FLIGHT
    VkBuffer staging_[kSlots] = {};
    VmaAllocation stagingAlloc_[kSlots] = {};
    void* stagingMapped_[kSlots] = {};
    bool layoutReady_ = false;

    glm::vec2 origin_{0.0f};
    bool ready_ = false;
    // The bake waiting for its copy.
    std::vector<uint16_t> pendingHalves_;
    glm::vec2 pendingOrigin_{0.0f};
    bool pendingUpload_ = false;

    // The bake in flight, and what it was started for.
    std::future<bounce_grid::Grid> job_;
    glm::vec2 jobOrigin_{0.0f};
    bool haveGrid_ = false;          // a bake landed or is in flight
    glm::vec2 lastOrigin_{0.0f};     // the last bake started
    std::string lastMap_;
    uint32_t lastM2Count_ = 0;
    uint32_t lastWmoCount_ = 0;
    float sinceCheck_ = 0.0f;
    bool worldChanged_ = false;

    /// The minimap: md5translate's names for the current map, and the tiles
    /// decoded so far, as 128x128 RGB floats. Touched only by the bake job
    /// (one at a time) once it has started.
    struct Minimap {
        std::string map;
        std::unordered_map<std::string, std::string> trs;
        std::unordered_map<int, std::shared_ptr<std::vector<float>>> tiles;  // key x * 64 + y
    };
    std::shared_ptr<Minimap> minimap_;
};

}  // namespace rendering
}  // namespace wowee
