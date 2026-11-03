#include "rendering/loading_screen.hpp"

#include <SDL3/SDL_vulkan.h>
#include "rendering/vk_context.hpp"
#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "pipeline/dbc_loader.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_sdl3.h>
#include <SDL3/SDL.h>
#include <random>
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <unordered_map>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace wowee {
namespace rendering {

LoadingScreen::LoadingScreen() {
    imagePaths.emplace_back("assets/krayonload.png");
}

LoadingScreen::~LoadingScreen() {
    shutdown();
}

bool LoadingScreen::initialize() {
    LOG_INFO("Initializing loading screen (Vulkan/ImGui)");
    if (!loadStockImage()) selectRandomImage();
    LOG_INFO("Loading screen initialized");
    return true;
}

namespace {

/// Every BLP the loading screen has shown, uploaded once for the life of the
/// program. VkContext keeps UI textures until it is destroyed and offers no
/// way to free one, so uploading the same 1024x1024 art on every map change
/// would grow without bound; the art a player meets is a handful of images.
VkDescriptorSet loadingBlpTexture(pipeline::AssetManager* am, VkContext* ctx,
                                  const std::string& path, bool additive = false,
                                  int* outW = nullptr, int* outH = nullptr) {
    struct Entry { VkDescriptorSet ds = VK_NULL_HANDLE; int w = 0; int h = 0; };
    static std::unordered_map<std::string, Entry> cache;
    std::string key = path;
    for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (additive) key += "|add";
    auto it = cache.find(key);
    if (it == cache.end()) {
        Entry e;
        if (am && ctx) {
            auto bytes = am->readFile(path);
            if (!bytes.empty()) {
                auto image = pipeline::BLPLoader::load(bytes);
                if (image.isValid() && !image.data.empty()) {
                    if (additive) {
                        // An ADD-blended texture has no alpha of its own:
                        // black is "nothing added". ImGui only blends by
                        // alpha, so the brightness becomes the alpha and the
                        // colour is un-premultiplied by it.
                        for (size_t i = 0; i + 3 < image.data.size(); i += 4) {
                            uint8_t* p = &image.data[i];
                            const int m = std::max({p[0], p[1], p[2]});
                            if (m == 0) { p[3] = 0; continue; }
                            p[0] = static_cast<uint8_t>(p[0] * 255 / m);
                            p[1] = static_cast<uint8_t>(p[1] * 255 / m);
                            p[2] = static_cast<uint8_t>(p[2] * 255 / m);
                            p[3] = static_cast<uint8_t>(m);
                        }
                    }
                    e.ds = ctx->uploadImGuiTexture(image.data.data(), image.width, image.height);
                    e.w = image.width;
                    e.h = image.height;
                }
            }
        }
        it = cache.emplace(key, e).first;
    }
    if (outW) *outW = it->second.w;
    if (outH) *outH = it->second.h;
    return it->second.ds;
}

} // namespace

bool LoadingScreen::loadStockImage() {
    if (!assetManager || !vkCtx) return false;

    // The bar is drawn whenever its art is there, even over the bundled
    // picture, so a map without a LoadingScreens row still gets the stock bar.
    barBorder = loadingBlpTexture(assetManager, vkCtx, "Interface\\Glues\\LoadingBar\\Loading-BarBorder.blp");
    barFill = loadingBlpTexture(assetManager, vkCtx, "Interface\\Glues\\LoadingBar\\Loading-BarFill.blp");
    // The stock bar is these two and nothing else (0x9e2dfc); the glow and
    // glass art in the same folder is not drawn by the loading screen.

    // 0x409ed0: with no art of the map's own, "Interface\Glues\loading".
    const auto genericArt = [&]() {
        int w = 0, h = 0;
        VkDescriptorSet ds = loadingBlpTexture(assetManager, vkCtx, "Interface\\Glues\\loading.blp", false, &w, &h);
        if (!ds) return false;
        bgDescriptorSet = ds;
        imageWidth = w;
        imageHeight = h;
        stockImage = true;
        wideImage = false;
        LOG_INFO("Loading screen: map ", mapId, " -> Interface\\Glues\\loading");
        return true;
    };
    if (mapId == 0xFFFFFFFFu) return genericArt();
    auto mapDbc = assetManager->loadDBC("Map.dbc");
    auto screens = assetManager->loadDBC("LoadingScreens.dbc");
    if (!mapDbc || !mapDbc->isLoaded() || !screens || !screens->isLoaded()) return genericArt();
    const int32_t mapRow = mapDbc->findRecordById(mapId);
    if (mapRow < 0) return genericArt();

    // Map.LoadingScreenID: field 57 in 3.3.5a's 66-field Map.dbc. Other
    // builds put it elsewhere, so it is found there by the two rows whose
    // answer is known - Azeroth shows screen 4, Kalimdor screen 3.
    uint32_t screenField = 0;
    const uint32_t mapFields = mapDbc->getFieldCount();
    if (mapFields == 66) {
        screenField = 57;
    } else {
        const int32_t azeroth = mapDbc->findRecordById(0);
        const int32_t kalimdor = mapDbc->findRecordById(1);
        if (azeroth >= 0 && kalimdor >= 0) {
            for (uint32_t f = 4; f < mapFields; ++f) {
                if (mapDbc->getUInt32(static_cast<uint32_t>(azeroth), f) == 4 &&
                    mapDbc->getUInt32(static_cast<uint32_t>(kalimdor), f) == 3) {
                    screenField = f;
                    break;
                }
            }
        }
    }
    if (screenField == 0) return genericArt();
    const uint32_t screenId = mapDbc->getUInt32(static_cast<uint32_t>(mapRow), screenField);
    if (screenId == 0) return genericArt();
    const int32_t screenRow = screens->findRecordById(screenId);
    if (screenRow < 0) return genericArt();

    // LoadingScreens: ID, Name, FileName, HasWideScreen.
    std::string file = screens->getString(static_cast<uint32_t>(screenRow), 2);
    if (file.empty()) return genericArt();
    const bool hasWide = screens->getFieldCount() > 3 &&
                         screens->getUInt32(static_cast<uint32_t>(screenRow), 3) != 0;

    float aspect = 16.0f / 9.0f;
    if (ImGui::GetCurrentContext()) {
        const ImVec2 d = ImGui::GetIO().DisplaySize;
        if (d.x > 1.0f && d.y > 1.0f) aspect = d.x / d.y;
    }

    VkDescriptorSet ds = VK_NULL_HANDLE;
    int w = 0, h = 0;
    // The client swaps in the *Wide art on a window wider than 4:3 (by more
    // than 0.001, 0x409ed0), inserting "Wide" before the extension.
    if (hasWide && game::glue::useWideLoadingArt(aspect)) {
        std::string wide = file;
        std::string lower = wide;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const size_t dot = lower.rfind(".blp");
        if (dot != std::string::npos) wide.insert(dot, "Wide");
        else wide += "Wide";
        ds = loadingBlpTexture(assetManager, vkCtx, wide, false, &w, &h);
        if (ds) wideImage = true;
    }
    if (!ds) ds = loadingBlpTexture(assetManager, vkCtx, file, false, &w, &h);
    if (!ds) {
        LOG_WARNING("Loading screen: no art at ", file, " for map ", mapId);
        return genericArt();
    }
    bgDescriptorSet = ds;
    imageWidth = w;
    imageHeight = h;
    stockImage = true;
    LOG_INFO("Loading screen: map ", mapId, " -> ", file, wideImage ? " (wide)" : "");
    return true;
}

/// What the stock client draws while a world loads: the art, full screen, and
/// the bar near the bottom. No zone name, no percentage, no status line.
void LoadingScreen::drawContents(ImDrawList* dl, float screenW, float screenH) {
    if (!dl || screenW < 1.0f || screenH < 1.0f) return;
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(screenW, screenH), IM_COL32(0, 0, 0, 255));

    // 0x40a270: the art keeps its own aspect (4:3, or 1.6 for the wide
    // art), fitted inside the window with black round it, and the bar is
    // laid out in the art's rectangle, not the window's.
    game::glue::Rect image{0.0f, 0.0f, screenW, screenH};
    if (stockImage) image = game::glue::loadingImageRect(screenW, screenH, wideImage);
    if (bgDescriptorSet) {
        dl->AddImage(reinterpret_cast<ImTextureID>(bgDescriptorSet), ImVec2(image.x0, image.y0),
                     ImVec2(image.x1, image.y1));
    }

    const float progress = std::clamp(loadProgress, 0.0f, 1.0f);
    const game::glue::LoadingBar bar = game::glue::loadingBarRects(image, progress);
    if (!barBorder) {
        // No glue art at all: a plain bar where the stock one sits.
        dl->AddRectFilled(ImVec2(bar.border.x0, bar.border.y0), ImVec2(bar.border.x1, bar.border.y1),
                          IM_COL32(0, 0, 0, 200));
        if (bar.fill.width() > 0.5f)
            dl->AddRectFilled(ImVec2(bar.fill.x0, bar.fill.y0), ImVec2(bar.fill.x1, bar.fill.y1),
                              IM_COL32(60, 110, 200, 255));
        return;
    }
    // The fill first, the whole texture squeezed over what has loaded (its
    // texture coordinates are fixed at 0..1, 0xab6400), then the border.
    if (barFill && bar.fill.width() > 0.5f) {
        dl->AddImage(reinterpret_cast<ImTextureID>(barFill), ImVec2(bar.fill.x0, bar.fill.y0),
                     ImVec2(bar.fill.x1, bar.fill.y1));
    }
    dl->AddImage(reinterpret_cast<ImTextureID>(barBorder), ImVec2(bar.border.x0, bar.border.y0),
                 ImVec2(bar.border.x1, bar.border.y1));
}

void LoadingScreen::shutdown() {
    if (vkCtx && bgImage) {
        VkDevice device = vkCtx->getDevice();
        vkDeviceWaitIdle(device);

        if (bgDescriptorSet) {
            // ImGui manages descriptor set lifetime
            bgDescriptorSet = VK_NULL_HANDLE;
        }
        bgSampler = VK_NULL_HANDLE; // Owned by VkContext sampler cache
        if (bgImageView) {
            vkDestroyImageView(device, bgImageView, nullptr);
            bgImageView = VK_NULL_HANDLE;
        }
        if (bgImage) {
            vkDestroyImage(device, bgImage, nullptr);
            bgImage = VK_NULL_HANDLE;
        }
        if (bgMemory) {
            vkFreeMemory(device, bgMemory, nullptr);
            bgMemory = VK_NULL_HANDLE;
        }
    }
}

void LoadingScreen::selectRandomImage() {
    if (imagePaths.empty()) return;

    unsigned seed = static_cast<unsigned>(
        std::chrono::system_clock::now().time_since_epoch().count());
    std::default_random_engine generator(seed);
    std::uniform_int_distribution<int> distribution(0, imagePaths.size() - 1);

    currentImageIndex = distribution(generator);
    LOG_INFO("Selected loading screen: ", imagePaths[currentImageIndex]);

    loadImage(imagePaths[currentImageIndex]);
}

static uint32_t findMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    LOG_ERROR("LoadingScreen: no suitable memory type found");
    return UINT32_MAX;
}

bool LoadingScreen::loadImage(const std::string& path) {
    if (!vkCtx) {
        LOG_WARNING("No VkContext for loading screen image");
        return false;
    }

    // Clean up old image
    if (bgImage) {
        VkDevice device = vkCtx->getDevice();
        vkDeviceWaitIdle(device);
        bgSampler = VK_NULL_HANDLE; // Owned by VkContext sampler cache
        if (bgImageView) { vkDestroyImageView(device, bgImageView, nullptr); bgImageView = VK_NULL_HANDLE; }
        if (bgImage) { vkDestroyImage(device, bgImage, nullptr); bgImage = VK_NULL_HANDLE; }
        if (bgMemory) { vkFreeMemory(device, bgMemory, nullptr); bgMemory = VK_NULL_HANDLE; }
        bgDescriptorSet = VK_NULL_HANDLE;
    }

    int channels;
    stbi_set_flip_vertically_on_load(false); // ImGui expects top-down
    unsigned char* data = stbi_load(path.c_str(), &imageWidth, &imageHeight, &channels, 4);

    if (!data) {
        LOG_ERROR("Failed to load loading screen image: ", path);
        return false;
    }

    LOG_INFO("Loaded loading screen image: ", imageWidth, "x", imageHeight);

    VkDevice device = vkCtx->getDevice();
    VkPhysicalDevice physDevice = vkCtx->getPhysicalDevice();
    VkDeviceSize imageSize = static_cast<VkDeviceSize>(imageWidth) * imageHeight * 4;

    // Create staging buffer
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = imageSize;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device, &bufInfo, nullptr, &stagingBuffer);

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(device, stagingBuffer, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(physDevice, memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory);
        vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

        void* mapped;
        vkMapMemory(device, stagingMemory, 0, imageSize, 0, &mapped);
        memcpy(mapped, data, imageSize);
        vkUnmapMemory(device, stagingMemory);
    }

    stbi_image_free(data);

    // Create image
    {
        VkImageCreateInfo imgInfo{};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imgInfo.extent = {.width = static_cast<uint32_t>(imageWidth), .height = static_cast<uint32_t>(imageHeight), .depth = 1};
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(device, &imgInfo, nullptr, &bgImage);

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, bgImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(physDevice, memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(device, &allocInfo, nullptr, &bgMemory);
        vkBindImageMemory(device, bgImage, bgMemory, 0);
    }

    // Transfer: transition, copy, transition
    vkCtx->immediateSubmit([&](VkCommandBuffer cmd) {
        // Transition to transfer dst
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = bgImage;
        barrier.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        VkDependencyInfo barrierDep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        barrierDep.dependencyFlags = 0;
        barrierDep.imageMemoryBarrierCount = 1;
        barrierDep.pImageMemoryBarriers = &barrier;
        cmdPipelineBarrier2(cmd, barrierDep);

        // Copy buffer to image
        VkBufferImageCopy region{};
        region.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
        region.imageExtent = {.width = static_cast<uint32_t>(imageWidth), .height = static_cast<uint32_t>(imageHeight), .depth = 1};
        vkCmdCopyBufferToImage(cmd, stagingBuffer, bgImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        // Transition to shader read
        barrier.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkDependencyInfo toReadDep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        toReadDep.imageMemoryBarrierCount = 1;
        toReadDep.pImageMemoryBarriers = &barrier;
        cmdPipelineBarrier2(cmd, toReadDep);
    });

    // Cleanup staging
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);

    // Create image view
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = bgImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        vkCreateImageView(device, &viewInfo, nullptr, &bgImageView);
    }

    // Create sampler
    {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        bgSampler = vkCtx->getOrCreateSampler(samplerInfo);
    }

    // Register with ImGui as a texture
    bgDescriptorSet = ImGui_ImplVulkan_AddTexture(bgSampler, bgImageView,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    return true;
}

/// Opens the fullscreen window the loading screen draws into and blits its
/// background.
///
/// Both draws need it: the overlay that sits over a live frame and the standalone
/// one that runs its own ImGui frame. Every flag matters. NoInputs is what keeps
/// the screen from swallowing clicks meant for what is behind it, and
/// NoBringToFrontOnFocus is what stops it climbing over the windows it is meant
/// to sit under.
void LoadingScreen::beginBackdrop(const char* windowName, float screenW, float screenH) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(screenW, screenH));
    ImGui::Begin(windowName, nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoBringToFrontOnFocus);
    // The picture itself is drawn by drawContents, which knows whether it is
    // 4:3 art to letterbox or wide art to fill the window with.
}

void LoadingScreen::renderOverlay() {
    // Draw loading screen content as ImGui overlay within an existing ImGui frame.
    // Caller is responsible for ImGui NewFrame/Render and Vulkan frame management.
    ImGuiIO& io = ImGui::GetIO();
    const float screenW = io.DisplaySize.x;
    const float screenH = io.DisplaySize.y;
    beginBackdrop("##LoadingScreenOverlay", screenW, screenH);
    drawContents(ImGui::GetWindowDrawList(), screenW, screenH);
    ImGui::End();
}

void LoadingScreen::render() {
    // If a frame is already in progress (e.g. called from a UI callback),
    // end it before starting our own
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx && ctx->FrameCount >= 0 && ctx->WithinFrameScope) {
        ImGui::EndFrame();
    }

    ImGuiIO& io = ImGui::GetIO();
    float screenW = io.DisplaySize.x;
    float screenH = io.DisplaySize.y;

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // Invisible fullscreen window
    beginBackdrop("##LoadingScreen", screenW, screenH);
    drawContents(ImGui::GetWindowDrawList(), screenW, screenH);

    ImGui::End();
    ImGui::Render();

    // Submit the frame to Vulkan (loading screen runs outside the main render loop)
    if (vkCtx) {
        // Handle window resize: recreate swapchain before acquiring an image
        if (vkCtx->isSwapchainDirty() && sdlWindow) {
            // The surface, in pixels. SDL_GetWindowSize answers points, and
            // on a high density display rebuilding at those halves the
            // swapchain under a loading screen that is drawn full width.
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(sdlWindow, &w, &h);
            if (w > 0 && h > 0) {
                (void)vkCtx->recreateSwapchain(w, h);
            }
        }

        uint32_t imageIndex = 0;
        VkCommandBuffer cmd = vkCtx->beginFrame(imageIndex);
        if (cmd != VK_NULL_HANDLE) {
            // Begin render pass
            // The UI draws in the overlay pass, which is what ImGui's pipelines
            // are built for: single-sampled and colour only. Drawing it in the
            // scene pass instead put a 1x pipeline inside an 8x pass, which the
            // validation layers reject and the driver renders as it pleases.
            // This variant clears, since there is no scene underneath here.
            VkRenderPassBeginInfo rpInfo{};
            rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpInfo.renderPass = vkCtx->getOverlayClearRenderPass();
            rpInfo.framebuffer = vkCtx->getOverlayFramebuffers()[imageIndex];
            rpInfo.renderArea.offset = {.x = 0, .y = 0};
            rpInfo.renderArea.extent = vkCtx->getSwapchainExtent();

            VkClearValue clearValues[1]{};
            clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
            rpInfo.clearValueCount = 1;
            rpInfo.pClearValues = clearValues;

            const bool overlayReady =
                rpInfo.renderPass != VK_NULL_HANDLE &&
                imageIndex < vkCtx->getOverlayFramebuffers().size();
            if (overlayReady) {
                vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
                vkCmdEndRenderPass(cmd);
            }

            vkCtx->endFrame(cmd, imageIndex);
        }
    }
}

} // namespace rendering
} // namespace wowee
