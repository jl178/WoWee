#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include "rendering/light_tiles.hpp"
#include "rendering/shadow_cache.hpp"
#include <atomic>
#include <chrono>

namespace wowee {
namespace rendering {

static constexpr uint32_t MAX_LOCAL_LIGHTS = 64;

// Must match the PerFrame UBO layout in all shaders (std140 alignment)
struct GPUPerFrameData {
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 lightSpaceMatrix;
    glm::vec4 lightDir;       // xyz = direction, w = unused
    glm::vec4 lightColor;     // xyz = color, w = unused
    glm::vec4 ambientColor;   // xyz = color, w = unused
    glm::vec4 viewPos;        // xyz = camera pos, w = terrain relief (Renderer::setTerrainRelief)
    // w = the cloth wind strength (banners, awnings, WMO cloth): playerWake.w
    // without the indoor cut, because a hall's banners always breathed.
    glm::vec4 fogColor;       // xyz = color, w = cloth wind strength
    glm::vec4 fogParams;      // x = fogStart, y = fogEnd, z = time, w = water ripple strength
    glm::vec4 shadowParams;   // x = enabled(0/1), y = strength, z = one shadow-map texel, w = unused
    // The player, for effects that react to where they are standing: water
    // ripples and the foliage the player brushes past. playerWake trails the
    // player by a fixed time constant, so clutter the player has already walked
    // through springs back over that interval instead of snapping upright.
    glm::vec4 playerPos;      // xyz = player world position, w = horizontal speed (yd/s)
    // w = the shared wind strength every swaying thing scales by: the
    // "Foliage and cloth wind" setting times the weather, eased over a few
    // seconds, zero indoors. 1.0 is the motion as it was before the setting.
    glm::vec4 playerWake;     // xyz = trailing player position, w = wind strength
    glm::vec4 localLightPosRadius[MAX_LOCAL_LIGHTS];       // xyz = position, w = radius
    glm::vec4 localLightColorIntensity[MAX_LOCAL_LIGHTS];  // rgb = color, w = intensity
    glm::ivec4 localLightMeta;                             // x = active light count
    // The fog volume at set 0 binding 2, and how to find a depth in it:
    // x = on (0/1), y = near edge of the first slice in yards,
    // z = 1 / ln(far / near), w = slice count. Off in the reflection pass and
    // the character preview, which bind a neutral volume there.
    glm::vec4 volumetricParams;
    // Last frame's ray traced lighting at set 0 bindings 3 and 4, and how to
    // find a surface in it (rt_lighting.glsli): the view-projection and camera
    // position it was traced with, and params.x = mode, 0 when there is no
    // result to read. Zero in the reflection pass and the character preview.
    // rtParams.w is not the tracer's: it is how wet the ground is in rain,
    // 0..1 (Renderer::updatePerFrameUBO, wet_surface.glsli).
    glm::mat4 rtViewProj;
    glm::vec4 rtCameraPos;
    glm::vec4 rtParams;
    // The local lights culled to the screen (local_light_tiles.glsli):
    // x, y the grid, z 1 when the masks hold for this pass's camera and
    // target - 0 in the reflection pass, the character preview and with the
    // point lights off, and then a pixel walks every light as before.
    // localLightTileScale.xy is tiles per pixel of the pass's target.
    glm::ivec4 localLightTileInfo;
    glm::vec4 localLightTileScale;
    glm::uvec4 localLightTiles[LIGHT_TILE_WORDS];
    // The unit map at set 0 binding 5 (rendering/shadow_cache.hpp,
    // shadow_filter.glsli): its projection; params x = on (0/1), y = one
    // texel in uv, z = 1 / its depth range in yards, w = yards per texel; and
    // the 64x64 mask of the cells a caster's footprint can reach. Read by the
    // terrain, WMO, M2 and character fragment shaders only, which declare the
    // block this far; everything else declares less of it.
    glm::mat4 nearLightSpaceMatrix{0.0f};
    glm::vec4 nearShadowParams{0.0f};
    glm::uvec4 nearShadowMask[kNearShadowMaskWords / 4] = {};
    // The detail map at set 0 binding 6 (Enhanced's sun shadows,
    // shadow_cache.hpp): its projection; params x = on (0/1), y = one texel
    // in uv, z = 1 / its depth range in yards, w = yards per texel. The same
    // four shaders read it, and declare the block this far.
    glm::mat4 detailLightSpaceMatrix{0.0f};
    glm::vec4 detailShadowParams{0.0f};
};

// Push constants for the model matrix (most common case)
struct GPUPushConstants {
    glm::mat4 model;
};

/// The WMO's own, which carries where a batch of cloth hangs beside the model
/// matrix: (top z, drop, centre x, centre y) in the model's local space.
struct WMOPushConstants {
    glm::mat4 model;
    glm::vec4 cloth{0.0f};
};

// Push constants for shadow rendering passes.
//
// Two matrices filled 128 bytes exactly, which is all Vulkan guarantees for
// push constants - and left no room for the sway a foliage caster needs to
// match the tree it belongs to. The light-space and model matrices are
// multiplied on the CPU instead, since nothing in the shadow shaders wanted
// them apart: the fragment shader's world position was never read.
struct ShadowPush {
    glm::mat4 lightSpaceModel;
    /// xy: the instance's world origin, which gives the wind its per-tree
    /// phase. z: the height the bend is normalised against. w: its amplitude.
    /// All zero for anything that does not sway.
    glm::vec4 sway{0.0f};
    /// x: sample the texture. y: alpha-test it. z: bend in the wind.
    ///
    /// These were in the uniform buffer, and the M2 pass writes that buffer
    /// twice a frame - once for its solid casters, once for its foliage. A
    /// uniform buffer is read when the draw executes rather than when it is
    /// recorded, so the second write decided what the first pass's draws saw,
    /// and one persistently-mapped copy shared between frames in flight let a
    /// CPU write land in the middle of the previous frame's reads. Losing the
    /// alpha test that way turns a leaf cutout into the solid outline of the
    /// canopy. Push constants are recorded into the command buffer with the
    /// draw, so there is nothing left to race.
    glm::ivec4 flags{0};
    /// x: the wind clock the sway is sampled at. y: the shared wind strength
    /// (M2 casters; the main pass has it as playerWake.w).
    glm::vec4 wind{0.0f};
};

// Uniform buffer for shadow rendering parameters (matches shader std140 layout)
struct ShadowParamsUBO {
    int32_t useBones;
    int32_t useTexture;
    int32_t alphaTest;
    int32_t foliageSway;
    float windTime;
    float foliageMotionDamp;
};

// Timer utility for performance profiling queries.
// Uses atomics because floor-height queries are dispatched on async threads
// from CameraController while the main thread may read the counters.
struct QueryTimer {
    std::atomic<double>* totalMs = nullptr;
    std::atomic<uint32_t>* callCount = nullptr;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    QueryTimer(std::atomic<double>* total, std::atomic<uint32_t>* calls)
        : totalMs(total), callCount(calls) {}
    ~QueryTimer() {
        if (callCount) {
            callCount->fetch_add(1, std::memory_order_relaxed);
        }
        if (totalMs) {
            auto end = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - start).count();
            // Relaxed is fine for diagnostics - exact ordering doesn't matter.
            double old = totalMs->load(std::memory_order_relaxed);
            while (!totalMs->compare_exchange_weak(old, old + ms, std::memory_order_relaxed)) {}
        }
    }
};

} // namespace rendering
} // namespace wowee
