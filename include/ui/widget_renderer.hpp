#pragma once

// Draws a WidgetTree.
//
// Kept apart from the tree itself so the layout rules stay testable without a
// Vulkan device. This half is the part that needs one.
//
// Textures come from the game's own Interface\ art through the existing asset
// path - read the BLP, upload it, hand ImGui the descriptor set - which is the
// same route the action bar already takes for its backpack button. Nothing new
// is shipped; it is the player's own install being drawn.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "ui/stock_font_metrics.hpp"
#include "ui/text_measure_cache.hpp"

struct ImDrawList;   // global, as ImGui declares it
struct ImFont;
struct ImVec2;

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering { class VkContext; }

namespace ui {

/// What a font string adds to a markup draw beyond its face and size: the
/// outline it was measured with, which moves its glyphs, and the distance from
/// one line to the next in pixels - zero for 1.2 times the size, which is what
/// tooltips and message frames lay their rows out by.
struct MarkupTextStyle {
    FontOutline outline = FontOutline::None;
    float lineHeight = 0.0f;
};

class WidgetTree;
struct Widget;

class WidgetRenderer {
public:
    void initialize(pipeline::AssetManager* assets, rendering::VkContext* vkCtx);

    /// Lay the tree out for this screen and draw it. Safe to call with no
    /// device or assets - it simply does nothing, which is what the headless
    /// tests want.
    void render(WidgetTree& tree, float screenW, float screenH);

    /// The two halves of render(), for callers that need something to happen
    /// between them.
    ///
    /// Hit testing reads the rects layout() produces, so it has to run early -
    /// before the frame's clicks are resolved, or a frame that moved this frame
    /// is clicked where it used to be. Drawing has the opposite requirement:
    /// the interface sits over the world, and the world's overlays - the
    /// nameplates and the minimap's blips - go into the same ImGui background
    /// list this does, where whatever is added last is on top. Drawn from here
    /// the panels went down first and every nameplate in the world showed
    /// through the bags.
    void layout(WidgetTree& tree, float screenW, float screenH);
    void draw(WidgetTree& tree, float screenW, float screenH);

    /// What is on screen, and what should be but is not - the instrumentation
    /// the FrameXML transition is being carried out with. Draws nothing.
    void reportWidgetDiagnostics(WidgetTree& tree, const std::vector<const Widget*>& order,
                                 float s, float screenW, float screenH);

    /// The sharp interface (enhancement N14). Text is drawn from glyphs baked
    /// at the size they are drawn, one sample per pixel and on whole pixels -
    /// above 32 pixels as well, where stock stretches its 32 pixel bitmaps -
    /// and art from a mip chain, so a texture drawn smaller than its file is
    /// filtered rather than skipped through. Off is the stock path exactly:
    /// the same bakes, the same single-level images, nothing extra made.
    /// Measurement and layout are the stock client's either way.
    void setCrispInterface(bool on) { crisp_ = on; }
    [[nodiscard]] bool crispInterface() const { return crisp_; }

    /// Number of distinct textures resident. Cheap diagnostic; the cache never
    /// evicts, because Interface\ art is small, bounded and reused constantly.
    [[nodiscard]] size_t textureCount() const { return textures_.size(); }

    /// Whether the art at this path can be read and decoded at all, and how big
    /// it is. Public because it answers a question worth asking from outside:
    /// FrameXML naming art the assets do not carry is a blank where an icon
    /// should be, and nothing else reports it - the draw silently substitutes
    /// nothing and carries on.
    bool artResolves(const std::string& path, float& w, float& h) {
        return textureSize(path, w, h);
    }

    /// The shaded quest areas of a QuestPOIFrame (WorldMapBlobFrame), as
    /// polygons in fractions of the frame, (0,0) at its top-left. The data is
    /// the interface's - which quest is selected, which objectives are open -
    /// so it is asked for rather than known here.
    using QuestBlobSource = std::function<std::vector<std::vector<std::pair<float, float>>>()>;
    void setQuestBlobSource(QuestBlobSource source) { questBlobSource_ = std::move(source); }

private:
    QuestBlobSource questBlobSource_;
    /// Draws the source's polygons in the frame's rect with the art and alphas
    /// the frame was given (SetFillTexture, SetBorderTexture, SetFillAlpha,
    /// SetBorderAlpha, SetBorderScalar) - WorldMapBlobFrame_OnLoad asks for
    /// UI-QuestBlob-Inside at 128, edged with UI-QuestBlob-Outside at 192.
    void drawQuestBlobs(ImDrawList* dl, const Widget& w, float x0, float y0, float x1, float y1);
    /// What a quest-blob texture looks like, read off its pixels once: its
    /// colour (alpha-weighted mean), its alpha (the mean for a fill, the
    /// brightest row for an edge), and for an edge how far into the texture
    /// the edge reaches before it has faded out, as a fraction of its height.
    /// Falls back to the stock art's figures when the file cannot be read.
    struct BlobArt {
        float rgb[3] = {1.0f, 1.0f, 1.0f};
        float alpha = 1.0f;
        float reach = 1.0f;
    };
    const BlobArt& blobArt(const std::string& path, bool border);
    std::unordered_map<std::string, BlobArt> blobArt_;
    /// A Model frame playing the bag buttons' item animation: the icon
    /// ReplaceIconTexture gave it, posed by ui::itemPushAnimPose. Pixel origin
    /// (ox, oy) is the frame's bottom-left on screen; scale is pixels per
    /// interface unit for this frame.
    void drawItemPushModel(ImDrawList* dl, const Widget& w, float ox, float oy, float scale);
    /// Descriptor set for an Interface\ path, loading it on first use. Returns
    /// VK_NULL_HANDLE for anything missing, and remembers the failure so a
    /// mistyped path is not re-read every frame.
    VkDescriptorSet texture(const std::string& path, bool add = false);
    /// Already-uploaded texture for a path, without triggering an upload.
    [[nodiscard]] VkDescriptorSet resident(const std::string& path, bool add = false) const;

    /// scale is pixels per interface unit. The rect arrives in pixels, but a
    /// backdrop's insets and edge size are authored in units like everything
    /// else, so they have to make the same trip or a border comes out the
    /// wrong thickness on any display that is not 768 pixels tall.
    /// Sizes every tooltip to the lines it holds, before layout runs. A
    /// tooltip has no size until it has something to say.
    /// Give every unsized label the size of the text in it.
    ///
    /// A FontString with no <Size> takes the size of its string, as it does in
    /// WoW. Leaving it at zero lays it out to nothing and draws nothing, so the
    /// text is set, correct, and invisible - the player frame's level number
    /// read text="14" in a rect of 0x0.
    /// Gives a texture the dimensions of its own image on any axis nothing else
    /// decides. WoW's rule, and FrameXML depends on it - the friends list's
    /// status icon declares one anchor and no size whatsoever.
    /// Labels and textures, in one pass. See sizeArtAndText. Only those
    /// changed since the last pass unless everyWidget, which is the old full
    /// walk; useMeasureCache false measures every string afresh. Answers how
    /// many widgets it visited.
    size_t sizeArtAndText(WidgetTree& tree, bool everyWidget, bool useMeasureCache);
    void sizeTextureWidget(Widget* w);
    void sizeFontStringWidget(Widget* w, ImFont* font, bool useMeasureCache);
    /// WOWEE_LAYOUT_VERIFY: lay `scratch` - a copy of `tree` taken before this
    /// frame's sizing - out the old way, every widget, and compare the two.
    void verifyAgainstFullLayout(WidgetTree& tree, WidgetTree& scratch,
                                 float screenW, float screenH);
    /// Text measurements by face, size, text and wrap. See text_measure_cache.hpp.
    TextMeasureCache measureCache_;
    /// Screen pixels per interface unit for the sizing pass in flight: the
    /// tree's interface scale. A label's own effective scale multiplies it.
    float sizingPixelScale_ = 1.0f;
    void sizeTooltipWidget(Widget* w, ImFont* font, WidgetTree& tree);
    /// How big the picture is, without uploading it. No Vulkan context needed:
    /// asking a file its dimensions does not require a GPU, and requiring one
    /// would put this beyond the reach of the headless harness.
    bool textureSize(const std::string& path, float& w, float& h);
    /// The bytes of a texture, with the extension and folder fallbacks applied.
    /// Shared by the upload and the size query so the two cannot look in
    /// different places.
    std::vector<uint8_t> readTextureFile(const std::string& path,
                                         std::string& resolvedOut);

    /// Labels whose glyphs are wider than the rect they were given.
    void reportOverflowingText(WidgetTree& tree);
    /// Labels holding a coin amount with a letter on the end of it.
    void reportLetteredAmounts(WidgetTree& tree);

    using TextStyle = MarkupTextStyle;
    /// Draw a string that may carry WoW's inline colour markup, as runs.
    /// wrapWidth of zero draws one line, which is what an auto-sized label
    /// and a tooltip row want; a positive one breaks the text to fit.
    /// Glyphs are placed by the stock client's pen (stock_font_metrics.hpp)
    /// where the face has stock metrics, so they land where the measurement
    /// put them. Answers the height it drew, in pixels.
    float drawMarkupText(ImDrawList* dl, ImFont* font, float size, ImVec2 at,
                        uint32_t fallback, float alpha, const std::string& text,
                        float wrapWidth = 0.0f, bool nonSpaceWrap = false,
                        const char* justifyH = nullptr, bool forceColor = false,
                        WidgetTree* linkSink = nullptr, uint32_t linkOwner = 0,
                        const TextStyle& style = TextStyle{});
    /// A SimpleHTML page holding a document rather than a string: its blocks
    /// stacked down the frame, text justified per block and each picture on a
    /// line of its own. See simple_html.hpp.
    void drawSimpleHtml(ImDrawList* dl, WidgetTree& tree, const Widget& w,
                        float ws, float x0, float y0, float x1);
    /// Screen height and interface scale of the pass in flight, so a link rect
    /// can be filed in the coordinates the click will arrive in.
    float linkScreenH_ = 0.0f;
    float linkScale_ = 1.0f;
    void drawBackdrop(ImDrawList* dl, const Widget& w, float scale,
                      float x0, float y0, float x1, float y1);
    void drawStatusBar(ImDrawList* dl, const Widget& w,
                       float x0, float y0, float x1, float y1);
    void drawSlider(ImDrawList* dl, const Widget& w,
                    float x0, float y0, float x1, float y1);
    /// One of a colour picker's four regions: the hue-saturation wheel, the
    /// brightness bar, or either thumb. None of them has art on disk - the
    /// wheel and the bar are generated from the colour `picker` holds, and the
    /// thumbs are placed by it rather than anchored, since where they belong is
    /// the answer rather than the question.
    void drawColorPicker(ImDrawList* dl, const WidgetTree& tree, const Widget& w,
                         const Widget& picker, float screenH,
                         float x0, float y0, float x1, float y1);
    void drawThumb(ImDrawList* dl, const Widget& w,
                   float x0, float y0, float x1, float y1);
    void drawCooldown(ImDrawList* dl, const Widget& w,
                      float x0, float y0, float x1, float y1);

    pipeline::AssetManager* assets_ = nullptr;
    rendering::VkContext* vkCtx_ = nullptr;
    bool crisp_ = false;
    std::unordered_map<std::string, VkDescriptorSet> textures_;
    /// The same art with a mip chain, for the sharp interface. Its own map so
    /// the stock path neither finds nor makes these.
    std::unordered_map<std::string, VkDescriptorSet> texturesMipped_;
    [[nodiscard]] const std::unordered_map<std::string, VkDescriptorSet>& textureCache() const {
        return crisp_ ? texturesMipped_ : textures_;
    }
    std::unordered_map<std::string, VkDescriptorSet>& textureCache() {
        return crisp_ ? texturesMipped_ : textures_;
    }
    /// The cached set for a path, or null when nothing is cached for it -
    /// which is different from a cached kMissing, and both callers care.
    ///
    /// Here rather than through cacheKey because cacheKey returns a string by
    /// value, so the ordinary case copied the path onto the heap purely to
    /// look it up. The draw pass does that twice for every texture on screen,
    /// every frame.
    [[nodiscard]] const VkDescriptorSet* cachedTexture(const std::string& path,
                                                       bool add) const;
    /// Image dimensions by path, including the ones that could not be read -
    /// stored as zero so a missing file is looked for once and not once a frame.
    std::unordered_map<std::string, std::pair<float, float>> textureSizes_;
    /// Which incarnation of ImGui's backend the cache above belongs to.
    uint32_t uiTextureGenerationSeen_ = 0;
};

} // namespace ui
} // namespace wowee
