#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <vector>

struct SDL_Window;
struct ImDrawList;

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class VkContext;

class LoadingScreen {
public:
    LoadingScreen();
    ~LoadingScreen();

    bool initialize();
    void shutdown();

    void selectRandomImage();

    // Render the loading screen with progress bar and status text (pure ImGui)
    void render();

    // Draw loading screen as ImGui overlay (call within an existing ImGui frame).
    // Used during warmup to overlay loading screen on top of the rendered world.
    void renderOverlay();
    /// The fullscreen window both draws open, and its background blit.
    void beginBackdrop(const char* windowName, float screenW, float screenH);

    void setProgress(float progress) { loadProgress = progress; }
    void setStatus(const std::string& status) { statusText = status; }
    void setZoneName(const std::string& name) { zoneName = name; }

    /// The stock screen: the map's art from LoadingScreens.dbc (the *Wide
    /// variant on a widescreen window when the row says it has one) and the
    /// glue loading bar. Both before initialize(); without them the screen
    /// falls back to the bundled picture.
    void setAssetManager(pipeline::AssetManager* am) { assetManager = am; }
    void setMapId(uint32_t id) { mapId = id; }

    // Must be set before initialize() for Vulkan texture upload
    void setVkContext(VkContext* ctx) { vkCtx = ctx; }
    void setSDLWindow(SDL_Window* win) { sdlWindow = win; }

private:
    bool loadImage(const std::string& path);
    /// The map's LoadingScreens.dbc art and the bar textures. False when
    /// there is no art for the map, which leaves the bundled picture.
    bool loadStockImage();
    /// Background and bar, into whatever list the caller is drawing.
    void drawContents(ImDrawList* dl, float screenW, float screenH);

    pipeline::AssetManager* assetManager = nullptr;
    uint32_t mapId = 0xFFFFFFFFu;
    bool stockImage = false;   ///< bgDescriptorSet is LoadingScreens art (4:3 unless wide)
    bool wideImage = false;
    VkDescriptorSet barBorder = VK_NULL_HANDLE;
    VkDescriptorSet barFill = VK_NULL_HANDLE;
    VkDescriptorSet barGlow = VK_NULL_HANDLE;
    VkDescriptorSet barGlass = VK_NULL_HANDLE;

    VkContext* vkCtx = nullptr;
    SDL_Window* sdlWindow = nullptr;

    // Vulkan texture for background image
    VkImage bgImage = VK_NULL_HANDLE;
    VkDeviceMemory bgMemory = VK_NULL_HANDLE;
    VkImageView bgImageView = VK_NULL_HANDLE;
    VkSampler bgSampler = VK_NULL_HANDLE;
    VkDescriptorSet bgDescriptorSet = VK_NULL_HANDLE; // ImGui texture handle

    std::vector<std::string> imagePaths;
    int currentImageIndex = 0;

    float loadProgress = 0.0f;
    std::string statusText = "Loading...";
    std::string zoneName;

    int imageWidth = 0;
    int imageHeight = 0;
};

} // namespace rendering
} // namespace wowee
