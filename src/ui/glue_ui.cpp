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
#include <cfloat>
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

/// GlueStrings.lua's NAME = "value"; lines, read once. Only those lines: the
/// file is nothing else but comments and the odd blank, and running it as Lua
/// would mean a Lua state on the login screen for a table of strings.
const std::unordered_map<std::string, std::string>& glueStrings() {
    static std::unordered_map<std::string, std::string> map;
    static bool loaded = false;
    if (loaded) return map;
    auto* assets = core::Application::getInstance().getAssetManager();
    // Asked again next frame while the assets are still coming up.
    if (!assets || !assets->isInitialized()) return map;
    loaded = true;
    const auto bytes = assets->readFileOptional("Interface\\GlueXML\\GlueStrings.lua");
    const std::string src(bytes.begin(), bytes.end());
    size_t i = 0;
    const size_t n = src.size();
    while (i < n) {
        const size_t eol = std::min(src.find('\n', i), n);
        size_t p = i;
        while (p < eol && (src[p] == ' ' || src[p] == '\t')) ++p;
        const size_t nameStart = p;
        while (p < eol && (std::isalnum(static_cast<unsigned char>(src[p])) || src[p] == '_')) ++p;
        const size_t nameEnd = p;
        while (p < eol && (src[p] == ' ' || src[p] == '\t')) ++p;
        if (nameEnd > nameStart && p < eol && src[p] == '=') {
            ++p;
            while (p < eol && (src[p] == ' ' || src[p] == '\t')) ++p;
            if (p < eol && src[p] == '"') {
                ++p;
                std::string value;
                bool closed = false;
                for (; p < eol; ++p) {
                    const char c = src[p];
                    if (c == '\\' && p + 1 < eol) {
                        const char e = src[++p];
                        switch (e) {
                            case 'n': value.push_back('\n'); break;
                            case 't': value.push_back('\t'); break;
                            default: value.push_back(e); break;
                        }
                    } else if (c == '"') {
                        closed = true;
                        break;
                    } else if (c != '\r') {
                        value.push_back(c);
                    }
                }
                if (closed) map[src.substr(nameStart, nameEnd - nameStart)] = std::move(value);
            }
        }
        i = eol + 1;
    }
    return map;
}

}  // namespace

std::string str(const char* key, const char* fallback) {
    const auto& map = glueStrings();
    if (key) {
        if (auto it = map.find(key); it != map.end()) return it->second;
    }
    return fallback ? fallback : "";
}

bool hasStr(const std::string& key) {
    return glueStrings().count(key) != 0;
}

std::string plain(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c != '|' || i + 1 >= s.size()) {
            out.push_back(c);
            continue;
        }
        const char e = s[i + 1];
        if (e == 'n') {
            out.push_back('\n');
            ++i;
        } else if (e == '|') {
            out.push_back('|');
            ++i;
        } else if ((e == 'c' || e == 'C') && i + 9 < s.size()) {
            i += 9;  // |cAARRGGBB
        } else if (e == 'r' || e == 'R') {
            ++i;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::vector<std::string> wrap(const std::string& s, float widthPx, float sizePx, ImFont* font) {
    std::vector<std::string> lines;
    if (!font) font = face();
    const auto width = [&](const std::string& t) {
        return font->CalcTextSizeA(sizePx, FLT_MAX, 0.0f, t.c_str()).x;
    };
    size_t start = 0;
    while (start <= s.size()) {
        const size_t end = std::min(s.find('\n', start), s.size());
        const std::string para = s.substr(start, end - start);
        std::string line, word;
        const auto flushWord = [&]() {
            if (word.empty()) return;
            const std::string trial = line.empty() ? word : line + " " + word;
            if (!line.empty() && width(trial) > widthPx) {
                lines.push_back(line);
                line = word;
            } else {
                line = trial;
            }
            word.clear();
        };
        for (char c : para) {
            if (c == ' ' || c == '\t') flushWord(); else word.push_back(c);
        }
        flushWord();
        lines.push_back(line);
        if (end >= s.size()) break;
        start = end + 1;
    }
    return lines;
}

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

ImFont* titleFace() {
    if (ImFont* f = interfaceFace("morpheus.ttf")) return f;
    return face();
}

float textWidth(ImFont* font, const char* s, float sizePx) {
    if (!s || !*s) return 0.0f;
    return (font ? font : face())->CalcTextSizeA(sizePx, FLT_MAX, 0.0f, s).x;
}

void text(ImDrawList* dl, ImVec2 at, const char* s, float sizePx, ImU32 col, bool outline) {
    text(dl, face(), at, s, sizePx, col, outline);
}

void text(ImDrawList* dl, ImFont* font, ImVec2 at, const char* s, float sizePx, ImU32 col,
          bool outline) {
    if (!dl || !s || !*s) return;
    ImFont* f = font ? font : face();
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
              float insetR, float insetT, float insetB, ImU32 bgTint, ImU32 edgeTint) {
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
                         ImVec2(u1, (y1 - y) / e), edgeTint);
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
                             ImVec2(u1, 1.0f - vEnd), ImVec2(u1, 1.0f), edgeTint);
        }
    };
    horizontal(2, a.y);
    horizontal(3, b.y - e);

    const auto corner = [&](int i, ImVec2 p) {
        const auto [u0, u1] = seg(i);
        dl->AddImage(edge, p, ImVec2(p.x + e, p.y + e), ImVec2(u0, 0), ImVec2(u1, 1), edgeTint);
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

PaperUI::PressState textureButton(PaperUI& ui, ImDrawList* dl, const char* id, ImVec2 a,
                                  ImVec2 b, const char* up, const char* down,
                                  const char* highlight, bool enabled, ImVec2 uv0, ImVec2 uv1) {
    const PaperUI::PressState st = ui.press(id, a, b, enabled);
    if (!dl) return st;
    const char* art = (st.held && down) ? down : up;
    const ImU32 tint = enabled ? IM_COL32_WHITE : IM_COL32(128, 128, 128, 255);
    if (art) image(dl, art, a, b, uv0, uv1, tint);
    if (enabled && st.hovered && highlight) image(dl, highlight, a, b, uv0, uv1, IM_COL32_WHITE, true);
    return st;
}

bool checkbox(PaperUI& ui, ImDrawList* dl, const char* id, ImVec2 at, float sizePx, bool checked,
              bool enabled) {
    const ImVec2 b(at.x + sizePx, at.y + sizePx);
    const PaperUI::PressState st =
        textureButton(ui, dl, id, at, b, "Interface\\Buttons\\UI-CheckBox-Up",
                      "Interface\\Buttons\\UI-CheckBox-Down",
                      "Interface\\Buttons\\UI-CheckBox-Highlight", enabled);
    if (checked) {
        image(dl, enabled ? "Interface\\Buttons\\UI-CheckBox-Check"
                          : "Interface\\Buttons\\UI-CheckBox-Check-Disabled",
              at, b);
    }
    return st.clicked;
}

bool closeButton(PaperUI& ui, ImDrawList* dl, const char* id, ImVec2 a) {
    const float s = 32.0f * unit();
    return textureButton(ui, dl, id, a, ImVec2(a.x + s, a.y + s),
                         "Interface\\Buttons\\UI-Panel-MinimizeButton-Up",
                         "Interface\\Buttons\\UI-Panel-MinimizeButton-Down",
                         "Interface\\Buttons\\UI-Panel-MinimizeButton-Highlight")
        .clicked;
}

void helpFrame(ImDrawList* dl, ImVec2 a, const char* title, float headerOffset) {
    const float u = unit();
    const auto piece = [&](const char* file, float x, float y, float w, float h) {
        image(dl, file, ImVec2(a.x + x * u, a.y + y * u), ImVec2(a.x + (x + w) * u, a.y + (y + h) * u));
    };
    piece("Interface\\HelpFrame\\HelpFrame-TopLeft", 0, 0, 256, 256);
    piece("Interface\\HelpFrame\\HelpFrame-Top", 256, 0, 256, 256);
    piece("Interface\\HelpFrame\\HelpFrame-TopRight", 512, 0, 128, 256);
    piece("Interface\\HelpFrame\\HelpFrame-BotLeft", 0, 256, 256, 256);
    piece("Interface\\HelpFrame\\HelpFrame-Bottom", 256, 256, 256, 256);
    piece("Interface\\HelpFrame\\HelpFrame-BotRight", 512, 256, 128, 256);
    // UI-DialogBox-Header: 256x64 at TOP(offset, 12), the title 14 down it.
    const float cx = a.x + (320.0f + headerOffset) * u;
    const float top = a.y - 12.0f * u;
    image(dl, "Interface\\DialogFrame\\UI-DialogBox-Header", ImVec2(cx - 128.0f * u, top),
          ImVec2(cx + 128.0f * u, top + 64.0f * u));
    if (title && *title) {
        textCentered(dl, cx, top + 14.0f * u, title, kFontSmall * u, kNormal);
    }
}

void tooltip(ImDrawList* dl, ImVec2 at, const std::string& textIn) {
    if (!dl || textIn.empty()) return;
    const float u = unit();
    const float size = kFontNormal * u;  // CharacterCreateTooltipFont
    const std::vector<std::string> lines = wrap(plain(textIn), 300.0f * u, size);
    float w = 0.0f;
    for (const auto& l : lines) w = std::max(w, textWidth(l.c_str(), size));
    const float lineH = size + 2.0f * u;
    const ImVec2 sz(w + 20.0f * u, lineH * static_cast<float>(lines.size()) + 18.0f * u);
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    at.x = std::clamp(at.x, 0.0f, std::max(0.0f, screen.x - sz.x));
    at.y = std::clamp(at.y, 0.0f, std::max(0.0f, screen.y - sz.y));
    backdrop(dl, at, ImVec2(at.x + sz.x, at.y + sz.y), "Interface\\Tooltips\\UI-Tooltip-Background",
             "Interface\\Tooltips\\UI-Tooltip-Border", 16.0f * u, 16.0f * u, 3.0f * u, 3.0f * u,
             3.0f * u, 3.0f * u, IM_COL32(23, 23, 48, 255));
    float y = at.y + 10.0f * u;
    for (size_t i = 0; i < lines.size(); ++i) {
        // The first line is the tooltip's title, in white; the rest gold.
        text(dl, ImVec2(at.x + 10.0f * u, y), lines[i].c_str(), size, i == 0 ? kHighlight : kNormal);
        y += lineH;
    }
}

bool dialog(PaperUI& ui, const char* id, const std::string& message, const char* buttonLabel) {
    const float u = unit();
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const float body = kFontNormal * u;
    const float w = 384.0f * u;
    const std::vector<std::string> lines = wrap(plain(message), w - 60.0f * u, body);
    const float lineH = body * 1.3f;
    const float buttonH = buttonLabel ? 40.0f * u + 16.0f * u : 0.0f;
    const float h = 26.0f * u + lineH * static_cast<float>(lines.size()) + 20.0f * u + buttonH;
    const ImVec2 a((screen.x - w) * 0.5f, (screen.y - h) * 0.5f);
    const ImVec2 b(a.x + w, a.y + h);
    fg->AddRectFilled(ImVec2(0, 0), screen, IM_COL32(0, 0, 0, 90));
    dialogBackdrop(fg, a, b);
    ui.claimMouse(ImVec2(0, 0), screen);
    float y = a.y + 26.0f * u;
    for (const std::string& line : lines) {
        textCentered(fg, (a.x + b.x) * 0.5f, y, line.c_str(), body, kHighlight);
        y += lineH;
    }
    if (!buttonLabel) return false;
    // GlueDialogButtonTemplate, sixteen units off the bottom.
    const float bw = 165.0f * u, bh = 40.0f * u;
    const ImVec2 ba((a.x + b.x - bw) * 0.5f, b.y - 16.0f * u - bh);
    return button(ui, fg, id, ba, ImVec2(ba.x + bw, ba.y + bh), buttonLabel, ButtonStyle::Red, true,
                  kFontNormal);
}

}  // namespace wowee::ui::glue
