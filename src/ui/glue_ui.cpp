#include "ui/glue_ui.hpp"

#include "ui/paper_ui.hpp"
#include "ui/interface_fonts.hpp"
#include "core/application.hpp"
#include "core/window.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "rendering/vk_context.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>

namespace wowee::ui::glue {

namespace {

std::string normalisedKey(const std::string& path, bool additive) {
    std::string key = path;
    for (char& c : key) {
        c = (c == '/') ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (additive) key += "|add";
    return key;
}

}  // namespace

float unit() {
    const float h = ImGui::GetIO().DisplaySize.y;
    return h > 1.0f ? h / 768.0f : 1.0f;
}

ImTextureID texture(const std::string& pathIn, bool additive) {
    // Kept for the life of the program: VkContext owns UI textures until it
    // goes, and the glue screens draw the same few dozen every frame.
    static std::unordered_map<std::string, VkDescriptorSet> cache;
    std::string path = pathIn;
    {
        std::string lower = path;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".blp") != 0) path += ".blp";
    }
    const std::string key = normalisedKey(path, additive);
    if (auto it = cache.find(key); it != cache.end()) {
        return reinterpret_cast<ImTextureID>(it->second);
    }

    auto& app = core::Application::getInstance();
    auto* assets = app.getAssetManager();
    auto* window = app.getWindow();
    auto* vk = window ? window->getVkContext() : nullptr;
    // Not cached while the assets or the device are still coming up: asked
    // again next frame rather than remembered as missing for good.
    if (!assets || !assets->isInitialized() || !vk) return ImTextureID{};

    VkDescriptorSet ds = VK_NULL_HANDLE;
    auto bytes = assets->readFile(path);
    if (!bytes.empty()) {
        auto image = pipeline::BLPLoader::load(bytes);
        if (image.isValid() && !image.data.empty()) {
            if (additive) {
                // ADD blending adds the colour and ignores alpha, so black is
                // "nothing". ImGui blends by alpha only: the brightest channel
                // becomes the alpha and the colour is divided back out of it,
                // which over a dark backdrop comes to nearly the same thing.
                for (size_t i = 0; i + 3 < image.data.size(); i += 4) {
                    uint8_t* p = &image.data[i];
                    const int m = std::max({static_cast<int>(p[0]), static_cast<int>(p[1]),
                                            static_cast<int>(p[2])});
                    const int a = m * p[3] / 255;
                    if (m == 0 || a == 0) { p[3] = 0; continue; }
                    p[0] = static_cast<uint8_t>(p[0] * 255 / m);
                    p[1] = static_cast<uint8_t>(p[1] * 255 / m);
                    p[2] = static_cast<uint8_t>(p[2] * 255 / m);
                    p[3] = static_cast<uint8_t>(a);
                }
            }
            ds = vk->uploadImGuiTexture(image.data.data(), image.width, image.height);
        }
    }
    cache.emplace(key, ds);
    return reinterpret_cast<ImTextureID>(ds);
}

void image(ImDrawList* dl, const std::string& path, ImVec2 a, ImVec2 b, ImVec2 uv0, ImVec2 uv1,
           ImU32 tint, bool additive) {
    if (!dl) return;
    if (ImTextureID tex = texture(path, additive)) dl->AddImage(tex, a, b, uv0, uv1, tint);
}

ImFont* face() {
    if (ImFont* f = interfaceFace("frizqt__.ttf")) return f;
    return ImGui::GetFont();
}

float textWidth(const char* s, float sizePx) {
    if (!s || !*s) return 0.0f;
    return face()->CalcTextSizeA(sizePx, FLT_MAX, 0.0f, s).x;
}

void text(ImDrawList* dl, ImVec2 at, const char* s, float sizePx, ImU32 col, bool outline) {
    if (!dl || !s || !*s) return;
    ImFont* f = face();
    at.x = std::floor(at.x);
    at.y = std::floor(at.y);
    const ImU32 black = IM_COL32(0, 0, 0, (col >> IM_COL32_A_SHIFT) & 0xFF);
    // The shadow, one unit down and right, as the glue fonts' <Shadow> sets it.
    const float sh = std::max(1.0f, std::round(unit()));
    dl->AddText(f, sizePx, ImVec2(at.x + sh, at.y + sh), black, s);
    if (outline) {
        // outline="NORMAL": a one-pixel black ring.
        static const ImVec2 kRing[] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (const ImVec2& o : kRing) dl->AddText(f, sizePx, ImVec2(at.x + o.x, at.y + o.y), black, s);
    }
    dl->AddText(f, sizePx, at, col, s);
}

void textCentered(ImDrawList* dl, float cx, float y, const char* s, float sizePx, ImU32 col,
                  bool outline) {
    text(dl, ImVec2(cx - textWidth(s, sizePx) * 0.5f, y), s, sizePx, col, outline);
}

void textRight(ImDrawList* dl, float rx, float y, const char* s, float sizePx, ImU32 col,
               bool outline) {
    text(dl, ImVec2(rx - textWidth(s, sizePx), y), s, sizePx, col, outline);
}

void backdrop(ImDrawList* dl, ImVec2 a, ImVec2 b, const std::string& bgFile,
              const std::string& edgeFile, float edgePx, float tilePx, float insetL,
              float insetR, float insetT, float insetB, ImU32 bgTint) {
    if (!dl) return;
    // The background, tiled. The UI sampler clamps, so each tile is its own
    // quad, the last ones cut short with their texture coordinates.
    if (!bgFile.empty()) {
        if (ImTextureID bg = texture(bgFile)) {
            const ImVec2 b0(a.x + insetL, a.y + insetT);
            const ImVec2 b1(b.x - insetR, b.y - insetB);
            const float tile = std::max(tilePx, 4.0f);
            for (float y = b0.y; y < b1.y; y += tile) {
                const float y1 = std::min(y + tile, b1.y);
                for (float x = b0.x; x < b1.x; x += tile) {
                    const float x1 = std::min(x + tile, b1.x);
                    dl->AddImage(bg, ImVec2(x, y), ImVec2(x1, y1), ImVec2(0, 0),
                                 ImVec2((x1 - x) / tile, (y1 - y) / tile), bgTint);
                }
            }
        }
    }
    if (edgeFile.empty()) return;
    ImTextureID edge = texture(edgeFile);
    if (!edge) return;
    const float e = edgePx;
    const auto seg = [](int i) { return std::make_pair(i / 8.0f, (i + 1) / 8.0f); };

    // Left and right edges, tiled down their length.
    const auto vertical = [&](int i, float x0) {
        const auto [u0, u1] = seg(i);
        for (float y = a.y + e; y < b.y - e; y += e) {
            const float y1 = std::min(y + e, b.y - e);
            dl->AddImage(edge, ImVec2(x0, y), ImVec2(x0 + e, y1), ImVec2(u0, 0),
                         ImVec2(u1, (y1 - y) / e));
        }
    };
    vertical(0, a.x);
    vertical(1, b.x - e);

    // Top and bottom are stored turned on their side: across the segment
    // (left to right) runs down the screen, and down the segment runs along
    // the edge. Both put the segment's left at the top.
    const auto horizontal = [&](int i, float y0) {
        const auto [u0, u1] = seg(i);
        for (float x = a.x + e; x < b.x - e; x += e) {
            const float x1 = std::min(x + e, b.x - e);
            const float vEnd = (x1 - x) / e;
            dl->AddImageQuad(edge, ImVec2(x, y0), ImVec2(x1, y0), ImVec2(x1, y0 + e),
                             ImVec2(x, y0 + e), ImVec2(u0, 1.0f), ImVec2(u0, 1.0f - vEnd),
                             ImVec2(u1, 1.0f - vEnd), ImVec2(u1, 1.0f));
        }
    };
    horizontal(2, a.y);
    horizontal(3, b.y - e);

    const auto corner = [&](int i, ImVec2 p) {
        const auto [u0, u1] = seg(i);
        dl->AddImage(edge, p, ImVec2(p.x + e, p.y + e), ImVec2(u0, 0), ImVec2(u1, 1));
    };
    corner(4, ImVec2(a.x, a.y));
    corner(5, ImVec2(b.x - e, a.y));
    corner(6, ImVec2(a.x, b.y - e));
    corner(7, ImVec2(b.x - e, b.y - e));
}

void tooltipBackdrop(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const float u = unit();
    backdrop(dl, a, b, "Interface\\Glues\\Common\\Glue-Tooltip-Background",
             "Interface\\Glues\\Common\\Glue-Tooltip-Border", 16.0f * u, 16.0f * u, 10.0f * u,
             5.0f * u, 4.0f * u, 9.0f * u);
}

void dialogBackdrop(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const float u = unit();
    backdrop(dl, a, b, "Interface\\DialogFrame\\UI-DialogBox-Background",
             "Interface\\DialogFrame\\UI-DialogBox-Border", 32.0f * u, 32.0f * u, 11.0f * u,
             12.0f * u, 12.0f * u, 11.0f * u);
}

bool button(PaperUI& ui, ImDrawList* dl, const char* id, ImVec2 a, ImVec2 b, const char* label,
            ButtonStyle style, bool enabled, float fontUnits) {
    const PaperUI::PressState st = ui.press(id, a, b, enabled);
    if (!dl) return st.clicked;

    const bool blue = (style == ButtonStyle::Blue);
    const char* up = blue ? "Interface\\Glues\\Common\\Glue-Panel-Button-Up-Blue"
                          : "Interface\\Glues\\Common\\Glue-Panel-Button-Up";
    const char* down = blue ? "Interface\\Glues\\Common\\Glue-Panel-Button-Down-Blue"
                            : "Interface\\Glues\\Common\\Glue-Panel-Button-Down";
    const char* hl = blue ? "Interface\\Glues\\Common\\Glue-Panel-Button-Highlight-Blue"
                          : "Interface\\Glues\\Common\\Glue-Panel-Button-Highlight";
    const char* face = !enabled ? "Interface\\Glues\\Common\\Glue-Panel-Button-Disabled"
                                : (st.held ? down : up);

    // GluePanelButton*Texture: the art fills 0..0.578 x 0..0.75 of the file.
    const ImVec2 uv1(0.578125f, 0.75f);
    if (ImTextureID tex = texture(face)) {
        dl->AddImage(tex, a, b, ImVec2(0, 0), uv1);
    } else {
        // No art: a plain dark button, so the screen still works.
        dl->AddRectFilled(a, b, blue ? IM_COL32(20, 40, 90, 230) : IM_COL32(90, 10, 10, 230), 4.0f);
        dl->AddRect(a, b, IM_COL32(160, 160, 160, 255), 4.0f);
    }
    if (enabled && st.hovered) {
        // Highlight art fills 0..0.625 x 0..0.6875 and is ADD-blended.
        image(dl, hl, a, b, ImVec2(0, 0), ImVec2(0.625f, 0.6875f), IM_COL32_WHITE, true);
    }

    if (label && *label) {
        const float u = unit();
        const float size = fontUnits * u;
        const float push = st.held ? 2.0f * u : 0.0f;
        // ButtonText is anchored CENTER with a three-unit lift.
        const float cy = (a.y + b.y) * 0.5f - 3.0f * u + push;
        const ImU32 col = !enabled ? kDisable : (st.hovered ? kHighlight : kNormal);
        textCentered(dl, (a.x + b.x) * 0.5f + push, cy - size * 0.5f, label, size, col);
    }
    return st.clicked;
}

}  // namespace wowee::ui::glue
