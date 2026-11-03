#pragma once

#include "rendering/wmo_minimap.hpp"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <vector>
#include <algorithm>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class Camera;
class VkContext;
class VkTexture;
class VkRenderTarget;

class Minimap {
public:
    Minimap();
    ~Minimap();

    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout, int size = 200);
    void shutdown();
    void recreatePipelines();
    /// The display pipeline, which initialize() and recreatePipelines()
    /// both build.
    void buildDisplayPipeline(VkDevice device,
                              const VkPipelineShaderStageCreateInfo& vertStage,
                              const VkPipelineShaderStageCreateInfo& fragStage);

    /// Select the render pass the display quad is recorded into.
    ///
    /// The minimap draws over the finished scene, and water is the one thing
    /// that draws after it: where water leaves the scene pass for a
    /// continuation of its own, the minimap has to follow or the water sheet is
    /// painted straight over it. Same reason SwimEffects has this.
    ///
    /// A null pass means the scene pass, which is where it goes when water has
    /// not moved. Call before initialize() or recreatePipelines().
    void setTargetPass(VkRenderPass pass, VkSampleCountFlagBits samples) {
        targetPass_ = pass;
        targetSamples_ = samples;
    }

    void setAssetManager(pipeline::AssetManager* am) { assetManager = am; }
    void setMapName(const std::string& name);

    /// Off-screen composite pass - call BEFORE the main render pass begins.
    void compositePass(VkCommandBuffer cmd, const glm::vec3& centerWorldPos);

    /// Display quad - call INSIDE the main render pass.
    void render(VkCommandBuffer cmd, const Camera& playerCamera,
                const glm::vec3& centerWorldPos, int screenWidth, int screenHeight,
                float playerOrientation = 0.0f, bool hasPlayerOrientation = false);

    void setEnabled(bool enabled) { this->enabled = enabled; }
    [[nodiscard]] bool isEnabled() const { return enabled; }
    void toggle() { enabled = !enabled; }

    void setViewRadius(float radius) { viewRadius = radius; }
    /// rotateMinimap: the map turns so the player's facing is up. The name is
    /// historical - it is the player's facing, not the camera's, that the
    /// stock client turns it by (0x0057C6A0 reads the active unit's facing).
    void setRotateWithCamera(bool rotate) { rotateWithCamera = rotate; }
    /// The player's facing in the client's own terms (0 north, a quarter turn
    /// counter-clockwise west), handed in every frame by the marker pass,
    /// which has the game state. Used only while rotating.
    void setBearing(float facing) { bearing_ = facing; }
    [[nodiscard]] float bearing() const { return rotateWithCamera ? bearing_ : 0.0f; }
    [[nodiscard]] bool isRotateWithCamera() const { return rotateWithCamera; }

    void setSquareShape(bool square) { squareShape = square; }
    [[nodiscard]] bool isSquareShape() const { return squareShape; }
    [[nodiscard]] float getViewRadius() const { return viewRadius; }

    /// The stock zoom levels: Minimap:GetZoomLevels() is 6, Minimap:SetZoom
    /// takes 0 (furthest out) to 5. The outdoor map across, in yards, is
    /// 466 2/3, 400, 333 1/3, 266 2/3, 200 and 133 1/3 - the figures the
    /// coordinate libraries (Astrolabe, HereBeDragons) measured from the real
    /// client. viewRadius is half of that.
    ///
    /// Indoors - while the map shows a WMO's own tiles, see setWmoView - the
    /// stock table is 300, 240, 180, 120, 80 and 50 yards, and the level is
    /// a second one (the client's minimapInsideZoom beside minimapZoom), so
    /// zooming in inside Ironforge leaves the outdoor zoom where it was.
    static constexpr int kZoomLevels = 6;
    static constexpr float kZoomRadius[kZoomLevels] = {
        233.0f + 1.0f / 3.0f, 200.0f, 166.0f + 2.0f / 3.0f,
        133.0f + 1.0f / 3.0f, 100.0f, 66.0f + 2.0f / 3.0f};
    static constexpr float kInsideZoomRadius[kZoomLevels] = {
        150.0f, 120.0f, 90.0f, 60.0f, 40.0f, 25.0f};
    /// Sets the level of whichever table is in use, indoor or outdoor.
    void setZoomLevel(int level) {
        int& active = indoors_ ? insideZoomLevel_ : zoomLevel_;
        // 0x007F3AE0 compares unsigned: anything above 4, negatives included,
        // is 5.
        active = (static_cast<unsigned>(level) > 4u) ? kZoomLevels - 1 : level;
        viewRadius = indoors_ ? kInsideZoomRadius[active] : kZoomRadius[active];
    }
    [[nodiscard]] int getZoomLevel() const { return indoors_ ? insideZoomLevel_ : zoomLevel_; }
    void zoomIn() { if (getZoomLevel() < kZoomLevels - 1) setZoomLevel(getZoomLevel() + 1); }
    void zoomOut() { if (getZoomLevel() > 0) setZoomLevel(getZoomLevel() - 1); }

    /// The WMO the player is in, for the indoor map. See wmo_minimap.hpp.
    struct WmoView {
        std::string tileRoot;            ///< wmo_minimap::tileRoot of the root
        glm::mat4 modelMatrix{1.0f};     ///< the instance's, local to render space
        std::vector<wmo_minimap::Group> groups;  ///< every loaded group
        size_t currentGroup = 0;         ///< the group the player is in, into groups
        glm::vec3 playerLocal{0.0f};     ///< a yard above the feet, local space
    };
    /// Draw the WMO's own tiles instead of the terrain, as the stock client
    /// does in an indoor group. Called every frame the player is in one. A
    /// root md5translate names no tiles for leaves the terrain map up, and
    /// so does WOWEE_WMO_MINIMAP=0 (decided by the caller). True when the
    /// map is in its WMO mode afterwards.
    bool setWmoView(const WmoView& view);
    /// Back to the terrain map and the outdoor zoom.
    void clearWmoView();
    /// The map is showing a WMO's tiles, and the indoor zoom table is in use.
    [[nodiscard]] bool isIndoors() const { return indoors_; }

    void setOpacity(float opacity) { opacity_ = opacity; }

    /// The player arrow's size in interface units, as Minimap:SetPlayerTexture-
    /// Width/Height set it on the FrameXML Minimap. Held here because the
    /// arrow is drawn by the UI marker pass (game_screen_minimap.cpp), which
    /// reads the map rather than the widget. 40x40 until told otherwise -
    /// what MinimapPing_OnLoad asks for.
    void setPlayerArrowSize(float w, float h) {
        playerArrowW_ = w > 0.0f ? w : kDefaultPlayerArrowSize;
        playerArrowH_ = h > 0.0f ? h : kDefaultPlayerArrowSize;
    }
    [[nodiscard]] float playerArrowWidth() const { return playerArrowW_; }
    [[nodiscard]] float playerArrowHeight() const { return playerArrowH_; }

    /// Where the map is drawn, in pixels from the top-left of the window.
    ///
    /// Unset, it goes in the top-right corner at its own size, which is where
    /// this client's own interface puts it. FrameXML puts it inside a frame it
    /// owns, so when the original interface is drawing the minimap the rect of
    /// that frame is handed here instead - this is a Vulkan pass of its own
    /// rather than an image the widget renderer could draw, so the map moves to
    /// the frame rather than the frame receiving the map.
    void setScreenRect(float x, float y, float w, float h) {
        rectX_ = x; rectY_ = y; rectW_ = w; rectH_ = h; haveRect_ = true;
    }
    void clearScreenRect() { haveRect_ = false; }
    /// Where the map is actually being drawn, when something placed it.
    ///
    /// The marker pass needs this: it used to assume the corner this client
    /// puts its own minimap in, and when FrameXML draws the ring the map moves
    /// to whatever rect the Minimap widget occupies. Blips computed against
    /// the old corner land beside the map rather than on it.
    [[nodiscard]] bool hasScreenRect() const { return haveRect_; }
    [[nodiscard]] float screenRectX() const { return rectX_; }
    [[nodiscard]] float screenRectY() const { return rectY_; }
    [[nodiscard]] float screenRectW() const { return rectW_; }
    [[nodiscard]] float screenRectH() const { return rectH_; }

    // Public accessors for WorldMap. Terrain tiles only.
    VkTexture* getOrLoadTileTexture(int tileX, int tileY);
    void ensureTRSParsed() { if (!trsParsed) parseTRS(); }
    [[nodiscard]] const std::string& getMapName() const { return mapName; }

private:
    void parseTRS();
    void updateTileDescriptors(uint32_t frameIdx, int centerTileX, int centerTileY);
    /// A minimap BLP by its md5translate hash, cached. Null when it is missing.
    VkTexture* loadTextureByHash(const std::string& hash);
    /// The composite pass in WMO mode: the drawn groups' tiles around the
    /// player, on black.
    void compositeWmoPass(VkCommandBuffer cmd, const glm::vec3& centerWorldPos);
    /// How many yards across the WMO composite is: the view's radius with room
    /// for the corners of a square or rotated map, and for the player moving
    /// between refreshes.
    [[nodiscard]] float wmoCompositeSpan() const { return 2.0f * (viewRadius * 1.5f + 16.0f); }

    VkContext* vkCtx = nullptr;
    /// Null until something says otherwise - see setTargetPass.
    VkRenderPass targetPass_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits targetSamples_ = VK_SAMPLE_COUNT_1_BIT;
    pipeline::AssetManager* assetManager = nullptr;
    std::string mapName = "Azeroth";

    // TRS lookup: "Azeroth\map32_49" → "e7f0dea73ee6baca78231aaf4b7e772a"
    std::unordered_map<std::string, std::string> trsLookup;
    // The WMO tiles again, keyed in lower case ("wmo\khazmodan\...\ironforge_
    // 044_00_01"), since a WMO's path and md5translate disagree on case; and
    // the roots that have any, so a building without tiles keeps the terrain.
    std::unordered_map<std::string, std::string> wmoTrsLookup_;
    std::unordered_set<std::string> wmoTileRoots_;
    bool trsParsed = false;

    // Tile texture cache: hash → VkTexture
    // Evicted (FIFO) when the count of successfully-loaded tiles exceeds
    // MAX_TILE_CACHE. Room for a full WMO composite (kMaxWmoTiles) beside the
    // terrain's nine, so one composite does not evict its own tiles.
    static constexpr size_t MAX_TILE_CACHE = 256;
    std::unordered_map<std::string, std::unique_ptr<VkTexture>> tileTextureCache;
    std::deque<std::string> tileInsertionOrder;  // hashes of successfully loaded tiles, oldest first
    std::unique_ptr<VkTexture> noDataTexture;

    // Composite render target (3x3 tiles = 768x768)
    std::unique_ptr<VkRenderTarget> compositeTarget;
    static constexpr int TILE_PX = 256;
    static constexpr int COMPOSITE_PX = TILE_PX * 3;  // 768

    // Shared quad vertex buffer (6 verts, pos2 + uv2 = 16 bytes/vert)
    ::VkBuffer quadVB = VK_NULL_HANDLE;
    VmaAllocation quadVBAlloc = VK_NULL_HANDLE;

    // Descriptor resources (shared layout: 1 combined image sampler at binding 0)
    VkDescriptorSetLayout samplerSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    // The most WMO tiles one composite draws. Ironforge's level around the
    // Great Forge is a few dozen.
    static constexpr uint32_t kMaxWmoTiles = 128;
    static constexpr uint32_t MAX_DESC_SETS = 19 + 2 * kMaxWmoTiles;

    // Tile composite pipeline (renders into VkRenderTarget)
    VkPipeline tilePipeline = VK_NULL_HANDLE;
    VkPipelineLayout tilePipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSet tileDescSets[2][9] = {};  // [frameInFlight][tileSlot]
    // The same shaders and layout, blending: a WMO tile is transparent where
    // the group has nothing, and groups overlap.
    VkPipeline wmoTilePipeline = VK_NULL_HANDLE;
    VkDescriptorSet wmoTileDescSets_[2][kMaxWmoTiles] = {};  // [frameInFlight][tile]

    // Display pipeline (renders into main render pass)
    VkPipeline displayPipeline = VK_NULL_HANDLE;
    VkPipelineLayout displayPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSet displayDescSet = VK_NULL_HANDLE;

    int mapSize = 200;
    float viewRadius = kZoomRadius[0];
    int zoomLevel_ = 0;
    int insideZoomLevel_ = 0;

    // WMO mode (setWmoView). indoors_ is what the next composite draws;
    // compositeIsWmo_ is what the last one drew, which render() has to match.
    bool indoors_ = false;
    bool compositeIsWmo_ = false;
    std::string wmoRoot_;
    glm::mat4 wmoModelMatrix_{1.0f};
    float wmoPlayerLocalZ_ = 0.0f;
    std::vector<wmo_minimap::Group> wmoDrawGroups_;  // in draw order
    glm::vec2 wmoCompositeCenter_{0.0f};  // render x/y the last WMO composite is centred on
    float wmoCompositeSpanUsed_ = 1.0f;
    bool enabled = true;
    bool rotateWithCamera = false;
    float bearing_ = 0.0f;
    bool squareShape = false;
    float opacity_ = 1.0f;
    bool  haveRect_ = false;
    float rectX_ = 0.0f, rectY_ = 0.0f, rectW_ = 0.0f, rectH_ = 0.0f;

    // Throttling
    float updateIntervalSec = 0.25f;
    float updateDistance = 6.0f;
    std::chrono::steady_clock::time_point lastUpdateTime{};
    glm::vec3 lastUpdatePos{0.0f};
    bool hasCachedFrame = false;

    // Tile tracking
    int lastCenterTileX = -1;
    int lastCenterTileY = -1;

    // No arrow texture here: the player arrow is Interface\Minimap\MinimapArrow
    // drawn by the UI marker pass (game_screen_minimap.cpp) at the size
    // Minimap.lua asks for with SetPlayerTextureWidth/Height, kept below. The
    // display shader used to draw a triangle of its own as well and no longer
    // does.
    static constexpr float kDefaultPlayerArrowSize = 40.0f;
    float playerArrowW_ = kDefaultPlayerArrowSize;
    float playerArrowH_ = kDefaultPlayerArrowSize;
};

} // namespace rendering
} // namespace wowee
