#include "ui/glue_ui.hpp"
#include "ui/glue_audio.hpp"

#include "ui/paper_ui.hpp"
#include "ui/interface_fonts.hpp"
#include "core/application.hpp"
#include "core/window.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "rendering/vk_context.hpp"
#include "game/glue_rules.hpp"

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

GlueParentScope::GlueParentScope() {
    if (ImGui::GetCurrentContext() == nullptr) return;
    ImGuiIO& io = ImGui::GetIO();
    screen_ = io.DisplaySize;
    const game::glue::Rect parent = game::glue::glueParentRect(screen_.x, screen_.y);
    // Half a pixel either side is a rounding of exactly 16:9, not a bar.
    if (parent.x0 < 0.5f) return;
    active_ = true;
    offsetX_ = parent.x0;
    width_ = parent.width();
    lists_[0] = ImGui::GetBackgroundDrawList();
    lists_[1] = ImGui::GetForegroundDrawList();
    for (int i = 0; i < 2; ++i) {
        ImDrawList* dl = lists_[i];
        // A command of its own, so what was drawn before this is not moved
        // with it when the two would otherwise have merged.
        if (!dl->CmdBuffer.empty() && dl->CmdBuffer.back().ElemCount != 0) dl->AddDrawCmd();
        vtxStart_[i] = dl->VtxBuffer.Size;
        cmdStart_[i] = dl->CmdBuffer.Size - 1;
    }
    io.DisplaySize.x = width_;
    if (ImGui::IsMousePosValid(&io.MousePos)) io.MousePos.x -= offsetX_;
    for (ImVec2& clicked : io.MouseClickedPos) clicked.x -= offsetX_;
}

GlueParentScope::~GlueParentScope() {
    if (!active_) return;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = screen_;
    if (ImGui::IsMousePosValid(&io.MousePos)) io.MousePos.x += offsetX_;
    for (ImVec2& clicked : io.MouseClickedPos) clicked.x += offsetX_;
    const float x0 = offsetX_;
    const float x1 = offsetX_ + width_;
    for (int i = 0; i < 2; ++i) {
        ImDrawList* dl = lists_[i];
        for (int v = vtxStart_[i]; v < dl->VtxBuffer.Size; ++v) dl->VtxBuffer[v].pos.x += offsetX_;
        for (int c = std::max(cmdStart_[i], 0); c < dl->CmdBuffer.Size; ++c) {
            ImVec4& clip = dl->CmdBuffer[c].ClipRect;
            clip.x = std::max(clip.x + offsetX_, x0);
            clip.z = std::min(clip.z + offsetX_, x1);
        }
        // Back to the whole window for whatever is drawn after this.
        dl->AddDrawCmd();
    }
    // Outside GlueParent nothing is drawn: black, as the stock window is.
    ImDrawList* bg = lists_[0];
    bg->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(x0, screen_.y), IM_COL32(0, 0, 0, 255));
    bg->AddRectFilled(ImVec2(x1, 0.0f), ImVec2(screen_.x, screen_.y), IM_COL32(0, 0, 0, 255));
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
    drawButton(dl, a, b, label, style, enabled, st.hovered, st.held, fontUnits * unit());
    return st.clicked;
}

void drawButton(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* label, ButtonStyle style,
                bool enabled, bool hovered, bool held, float labelPx) {
    if (!dl) return;
    struct {
        bool hovered, held;
    } st{hovered, held};

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
        const float size = labelPx;
        const float push = st.held ? 2.0f * u : 0.0f;
        // ButtonText is anchored CENTER with a three-unit lift.
        const float cy = (a.y + b.y) * 0.5f - 3.0f * u + push;
        const ImU32 col = !enabled ? kDisable : (st.hovered ? kHighlight : kNormal);
        textCentered(dl, (a.x + b.x) * 0.5f + push, cy - size * 0.5f, label, size, col);
    }
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
    // GlueDialog_OnClick: every button of a glue dialog says gsTitleOptionOK.
    const bool clicked = button(ui, fg, id, ba, ImVec2(ba.x + bw, ba.y + bh), buttonLabel,
                                ButtonStyle::Red, true, kFontNormal);
    if (clicked) glue_audio::play("gsTitleOptionOK");
    return clicked;
}

// ---------------------------------------------------------------------------
// Slider and dropdown (OptionsSliderTemplate, GlueDropDownMenuTemplate)
// ---------------------------------------------------------------------------

namespace {

constexpr const char* kSliderBg = "Interface\\Buttons\\UI-SliderBar-Background";
constexpr const char* kSliderBorder = "Interface\\Buttons\\UI-SliderBar-Border";
constexpr const char* kSliderThumb = "Interface\\Buttons\\UI-SliderBar-Button-Horizontal";

/// The bar and its thumb over a..b (the bar's own 17-unit height is centred
/// in it), the thumb's centre `t` of the way between `x0` and `x1`.
void drawSliderBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float x0, float x1, float t, bool enabled) {
    const float u = unit();
    const float cy = (a.y + b.y) * 0.5f;
    const float barH = 17.0f * u;
    const ImU32 tint = enabled ? IM_COL32_WHITE : IM_COL32(128, 128, 128, 255);
    backdrop(dl, ImVec2(a.x, cy - barH * 0.5f), ImVec2(b.x, cy + barH * 0.5f), kSliderBg,
             kSliderBorder, 8.0f * u, 8.0f * u, 3.0f * u, 3.0f * u, 6.0f * u, 6.0f * u, tint, tint);
    const float cx = x0 + (x1 - x0) * std::clamp(t, 0.0f, 1.0f);
    const float r = 16.0f * u;
    image(dl, kSliderThumb, ImVec2(cx - r, cy - r), ImVec2(cx + r, cy + r), ImVec2(0, 0),
          ImVec2(1, 1), tint);
}

uint32_t idHash(const char* s) {
    uint32_t h = 2166136261u;
    for (const char* p = s; p && *p; ++p) {
        h ^= static_cast<unsigned char>(*p);
        h *= 16777619u;
    }
    return h ? h : 1u;
}

/// The one glue dropdown list that can be down, and a choice made in it
/// waiting for its box to collect it.
struct DropdownState {
    uint32_t open = 0;
    int frame = -1;  ///< the frame its box last drew on
    ImVec2 a, b;
    float rowH = 0.0f;
    std::vector<std::string> items;
    int selected = -1;
    uint32_t pickedId = 0;
    int pickedIndex = -1;
};

DropdownState& dropState() {
    static DropdownState s;
    return s;
}

}  // namespace

bool slider(PaperUI& ui, ImDrawList* dl, const char* id, ImVec2 at, float widthPx, float* value,
            float lo, float hi, float step, const char* label, const char* low, const char* high,
            bool enabled) {
    if (!value || hi <= lo) return false;
    const float u = unit();
    const ImVec2 a = at;
    const ImVec2 b(at.x + widthPx, at.y + 17.0f * u);
    // HitRectInsets: ten units above and below the bar hear the pointer too.
    const PaperUI::PressState st =
        ui.press(id, ImVec2(a.x, a.y - 10.0f * u), ImVec2(b.x, b.y + 10.0f * u), enabled);
    // The thumb is 32 units of art round a narrower knob, and stops with its
    // art against the bar's ends.
    const float x0 = a.x + 16.0f * u;
    const float x1 = std::max(x0 + 1.0f, b.x - 16.0f * u);

    bool changed = false;
    if (st.held) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - x0) / (x1 - x0), 0.0f, 1.0f);
        float next = lo + (hi - lo) * t;
        if (step > 0.0f) next = std::clamp(lo + std::round((next - lo) / step) * step, lo, hi);
        if (next != *value) {
            *value = next;
            changed = true;
        }
    }
    if (!dl) return changed;

    drawSliderBar(dl, a, b, x0, x1, (*value - lo) / (hi - lo), enabled);
    if (label && *label) {
        // OptionsFontHighlight, its bottom on the bar's top.
        const float size = 12.0f * u;
        textCentered(dl, (a.x + b.x) * 0.5f, a.y - size - 2.0f * u, label, size,
                     enabled ? kHighlight : kDisable);
    }
    const float small = 10.0f * u;  // OptionsFontSmall, at the ends under it
    if (low && *low) text(dl, ImVec2(a.x - 4.0f * u, b.y - 1.0f * u), low, small, enabled ? kNormal : kDisable);
    if (high && *high) textRight(dl, b.x + 4.0f * u, b.y - 1.0f * u, high, small, enabled ? kNormal : kDisable);
    return changed;
}

bool dropdown(PaperUI& ui, ImDrawList* dl, const char* id, ImVec2 at, float widthUnits,
              const char* label, const std::vector<std::string>& items, int* index, bool enabled) {
    const float u = unit();
    DropdownState& s = dropState();
    const uint32_t key = idHash(id);

    bool changed = false;
    if (s.pickedId == key) {
        if (index && s.pickedIndex >= 0 && *index != s.pickedIndex) {
            *index = s.pickedIndex;
            changed = true;
        }
        s.pickedId = 0;
    }

    // The three pieces of CharacterCreate-LabelFrame, 64 units tall from 17
    // above the frame's top.
    const float mid = widthUnits * u;
    const float artTop = at.y - 17.0f * u;
    const float artBottom = artTop + 64.0f * u;
    const float right = at.x + 50.0f * u + mid;
    const ImVec2 hitA(at.x + 16.0f * u, at.y);
    const ImVec2 hitB(right - 14.0f * u, at.y + 28.0f * u);
    const PaperUI::PressState st = ui.press(id, hitA, hitB, enabled && !items.empty());
    if (st.clicked) s.open = (s.open == key) ? 0u : key;

    if (dl) {
        const char* frame = "Interface\\Glues\\CharacterCreate\\CharacterCreate-LabelFrame";
        const ImU32 tint = enabled ? IM_COL32_WHITE : IM_COL32(150, 150, 150, 255);
        image(dl, frame, ImVec2(at.x, artTop), ImVec2(at.x + 25.0f * u, artBottom), ImVec2(0, 0),
              ImVec2(0.1953125f, 1), tint);
        image(dl, frame, ImVec2(at.x + 25.0f * u, artTop), ImVec2(at.x + 25.0f * u + mid, artBottom),
              ImVec2(0.1953125f, 0), ImVec2(0.8046875f, 1), tint);
        image(dl, frame, ImVec2(right - 25.0f * u, artTop), ImVec2(right, artBottom),
              ImVec2(0.8046875f, 0), ImVec2(1, 1), tint);

        // The scroll-down button, 24 units at the right piece's TOPRIGHT(-16,-18).
        const ImVec2 bA(right - 40.0f * u, artTop + 18.0f * u);
        const ImVec2 bB(bA.x + 24.0f * u, bA.y + 24.0f * u);
        const char* arrow = !enabled ? "Interface\\ChatFrame\\UI-ChatIcon-ScrollDown-Disabled"
                            : st.held ? "Interface\\ChatFrame\\UI-ChatIcon-ScrollDown-Down"
                                      : "Interface\\ChatFrame\\UI-ChatIcon-ScrollDown-Up";
        image(dl, arrow, bA, bB);
        if (st.hovered) {
            image(dl, "Interface\\Buttons\\UI-Common-MouseHilight", bA, bB, ImVec2(0, 0),
                  ImVec2(1, 1), IM_COL32_WHITE, true);
        }

        // The choice, GlueFontHighlightSmall, its right end 43 units in from
        // the frame's right and kept inside the box.
        const float size = kFontSmall * u;
        const int cur = index ? *index : -1;
        const char* shown = (cur >= 0 && cur < static_cast<int>(items.size()))
                                ? items[static_cast<size_t>(cur)].c_str() : "";
        const float cy = at.y + 14.0f * u;
        dl->PushClipRect(ImVec2(at.x + 20.0f * u, artTop), ImVec2(right - 40.0f * u, artBottom), true);
        textRight(dl, right - 43.0f * u, cy - size * 0.5f, shown, size,
                  enabled ? kHighlight : kDisable);
        dl->PopClipRect();

        if (label && *label) {
            // OptionsFontSmall, its bottom on the frame's top, 21 units in.
            const float ls = 10.0f * u;
            text(dl, ImVec2(at.x + 21.0f * u, at.y - ls - 1.0f * u), label, ls,
                 enabled ? kNormal : kDisable);
        }
    }

    if (s.open == key) {
        if (!enabled) {
            s.open = 0;
        } else {
            // UIDropDownList: rows sixteen units tall inside a fifteen-unit
            // border, wide enough for the longest entry with its check.
            const float rowH = 16.0f * u;
            const float textSize = kFontSmall * u;
            float widest = 0.0f;
            for (const auto& item : items) widest = std::max(widest, textWidth(item.c_str(), textSize));
            const float w = std::max(mid + 20.0f * u, widest + 24.0f * u + 40.0f * u);
            const float h = rowH * static_cast<float>(items.size()) + 30.0f * u;
            const ImVec2 screen = ImGui::GetIO().DisplaySize;
            float top = at.y + 26.0f * u;
            if (top + h > screen.y) top = std::max(0.0f, at.y - h);
            float left = at.x + 9.0f * u;
            if (left + w > screen.x) left = std::max(0.0f, screen.x - w);
            s.a = ImVec2(left, top);
            s.b = ImVec2(left + w, top + h);
            s.rowH = rowH;
            s.items = items;
            s.selected = index ? *index : -1;
            s.frame = ImGui::GetFrameCount();
        }
    }
    return changed;
}

bool dropdownOpen() {
    return dropState().open != 0;
}

void closeDropdown() {
    dropState().open = 0;
}

void dropdownList(PaperUI& ui, ImDrawList* dl) {
    DropdownState& s = dropState();
    if (s.open == 0) return;
    // Its box was not drawn this frame - the page it is on was left - so the
    // list goes with it.
    if (s.frame != ImGui::GetFrameCount() || !dl) {
        s.open = 0;
        return;
    }
    const float u = unit();
    dialogBackdrop(dl, s.a, s.b);
    ui.hover(s.a, s.b);

    const ImVec2 m = ImGui::GetIO().MousePos;
    const auto inside = [&](ImVec2 a, ImVec2 b) {
        return m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y;
    };
    const float textSize = kFontSmall * u;
    int hoveredRow = -1;
    for (int i = 0; i < static_cast<int>(s.items.size()); ++i) {
        const float y0 = s.a.y + 15.0f * u + s.rowH * static_cast<float>(i);
        const ImVec2 ra(s.a.x + 12.0f * u, y0);
        const ImVec2 rb(s.b.x - 12.0f * u, y0 + s.rowH);
        const bool over = inside(ra, rb);
        if (over) {
            hoveredRow = i;
            image(dl, "Interface\\QuestFrame\\UI-QuestTitleHighlight", ra, rb, ImVec2(0, 0),
                  ImVec2(1, 1), IM_COL32_WHITE, true);
        }
        if (i == s.selected) {
            const float c = s.rowH * 1.25f;
            const float cy = (ra.y + rb.y) * 0.5f;
            image(dl, "Interface\\Buttons\\UI-CheckBox-Check", ImVec2(ra.x - 2.0f * u, cy - c * 0.5f),
                  ImVec2(ra.x - 2.0f * u + c, cy + c * 0.5f));
        }
        text(dl, ImVec2(ra.x + 20.0f * u, (ra.y + rb.y) * 0.5f - textSize * 0.5f - 1.0f * u),
             s.items[static_cast<size_t>(i)].c_str(), textSize, kHighlight);
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hoveredRow >= 0) {
            s.pickedId = s.open;
            s.pickedIndex = hoveredRow;
            s.open = 0;
        } else if (!inside(s.a, s.b)) {
            s.open = 0;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) s.open = 0;
}

// ---------------------------------------------------------------------------
// The paper skin
// ---------------------------------------------------------------------------

namespace {

class GlueSkin final : public PaperSkin {
public:
    PaperTheme theme() const override {
        PaperTheme t;
        t.paperTop = IM_COL32(8, 8, 12, 255);
        t.paperBottom = IM_COL32(8, 8, 12, 255);
        t.paperEdge = IM_COL32(110, 110, 110, 255);
        t.shadow = IM_COL32(0, 0, 0, 160);
        t.ink = kNormal;
        t.inkSoft = kNormal;
        t.pencil = IM_COL32(160, 160, 160, 255);
        t.fieldFill = IM_COL32(0, 0, 0, 0);
        t.crayonRed = kRed;
        t.crayonRedLit = IM_COL32(255, 90, 90, 255);
        t.crayonRedDim = IM_COL32(150, 10, 10, 255);
        t.crayonBlue = IM_COL32(110, 170, 255, 255);
        t.crayonGreen = kGreen;
        t.highlighter = IM_COL32(60, 120, 230, 120);
        t.onCrayon = kHighlight;
        t.photoMat = IM_COL32(0, 0, 0, 255);
        t.photoFade = 0.0f;
        t.photoTint = IM_COL32_WHITE;
        return t;
    }

    void text(ImDrawList* dl, ImFont* font, ImVec2 at, const char* s, float size,
              ImU32 col) override {
        glue::text(dl, font, at, s, size, col, true);
    }

    void sheet(ImDrawList* dl, ImVec2 a, ImVec2 b) override { dialogBackdrop(dl, a, b); }

    void scrim(ImDrawList* dl, ImVec2 screen, float alpha) override {
        const int a = static_cast<int>(std::clamp(alpha * 1.3f, 0.0f, 0.75f) * 255.0f);
        dl->AddRectFilled(ImVec2(0, 0), screen, IM_COL32(0, 0, 0, a));
    }

    void button(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* label, bool primary, bool enabled,
                bool hovered, bool held) override {
        const float u = unit();
        const float size = std::min((primary ? kFontNormal : kFontSmall) * u, (b.y - a.y) * 0.42f);
        drawButton(dl, a, b, label, ButtonStyle::Blue, enabled, hovered, held, size);
    }

    void box(ImDrawList* dl, ImVec2 a, ImVec2 b, bool) override { tooltipBackdrop(dl, a, b); }

    float boxPad() const override { return 12.0f * unit(); }

    void checkbox(ImDrawList* dl, ImVec2 a, ImVec2 b, bool checked, bool enabled, bool hovered,
                  bool held) override {
        const ImU32 tint = enabled ? IM_COL32_WHITE : IM_COL32(128, 128, 128, 255);
        image(dl, held ? "Interface\\Buttons\\UI-CheckBox-Down" : "Interface\\Buttons\\UI-CheckBox-Up",
              a, b, ImVec2(0, 0), ImVec2(1, 1), tint);
        if (hovered && enabled) {
            image(dl, "Interface\\Buttons\\UI-CheckBox-Highlight", a, b, ImVec2(0, 0), ImVec2(1, 1),
                  IM_COL32_WHITE, true);
        }
        if (checked) {
            image(dl, enabled ? "Interface\\Buttons\\UI-CheckBox-Check"
                              : "Interface\\Buttons\\UI-CheckBox-Check-Disabled",
                  a, b);
        }
    }

    void radio(ImDrawList* dl, ImVec2 a, ImVec2 b, bool selected, bool enabled,
               bool hovered) override {
        // UI-RadioButton: four 16x16 cells - normal, checked, highlight,
        // disabled check.
        const char* file = "Interface\\Buttons\\UI-RadioButton";
        const auto cell = [&](int i, ImU32 tint, bool add) {
            image(dl, file, a, b, ImVec2(i * 0.25f, 0), ImVec2((i + 1) * 0.25f, 1), tint, add);
        };
        cell(0, enabled ? IM_COL32_WHITE : IM_COL32(128, 128, 128, 255), false);
        if (selected) cell(enabled ? 1 : 3, IM_COL32_WHITE, false);
        if (hovered && enabled) cell(2, IM_COL32_WHITE, true);
    }

    void slider(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, bool) override {
        // Where PaperUI puts its knob's ends, so the thumb is under the pointer.
        const float knob = (b.y - a.y) * 0.28f;
        drawSliderBar(dl, a, b, a.x + knob, b.x - knob, t, true);
    }

    void progress(ImDrawList* dl, ImVec2 a, ImVec2 b, float t) override {
        dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 180), 2.0f);
        if (t > 0.0f) {
            const float x = a.x + 2.0f + (b.x - a.x - 4.0f) * t;
            dl->AddRectFilledMultiColor(ImVec2(a.x + 2.0f, a.y + 2.0f), ImVec2(x, b.y - 2.0f),
                                        IM_COL32(40, 110, 230, 255), IM_COL32(70, 150, 255, 255),
                                        IM_COL32(20, 70, 170, 255), IM_COL32(20, 60, 150, 255));
        }
        dl->AddRect(a, b, IM_COL32(150, 150, 150, 255), 2.0f, 0, 1.0f);
    }

    void popup(ImDrawList* dl, ImVec2 a, ImVec2 b) override { dialogBackdrop(dl, a, b); }

    void rowMark(ImDrawList* dl, ImVec2 a, ImVec2 b, bool selected, bool hovered) override {
        const char* hl = "Interface\\QuestFrame\\UI-QuestTitleHighlight";
        if (selected) image(dl, hl, a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(90, 150, 255, 255), true);
        if (hovered) image(dl, hl, a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, true);
    }

    void dropdownArrow(ImDrawList* dl, ImVec2 a, ImVec2 b, bool open, bool hovered) override {
        const float s = std::min(b.y - a.y, 24.0f * unit() * 1.2f) * 0.75f;
        const float cy = (a.y + b.y) * 0.5f - (b.y - a.y) * 0.05f;
        const ImVec2 bA(b.x - boxPad() * 0.5f - s, cy - s * 0.5f);
        const ImVec2 bB(bA.x + s, bA.y + s);
        image(dl, open ? "Interface\\ChatFrame\\UI-ChatIcon-ScrollDown-Down"
                       : "Interface\\ChatFrame\\UI-ChatIcon-ScrollDown-Up",
              bA, bB);
        if (hovered) {
            image(dl, "Interface\\Buttons\\UI-Common-MouseHilight", bA, bB, ImVec2(0, 0),
                  ImVec2(1, 1), IM_COL32_WHITE, true);
        }
    }
};

}  // namespace

PaperSkin* paperSkin() {
    static GlueSkin skin;
    return &skin;
}

bool paperSkinAvailable() {
    return texture("Interface\\DialogFrame\\UI-DialogBox-Background") &&
           texture("Interface\\Glues\\Common\\Glue-Panel-Button-Up-Blue");
}

}  // namespace wowee::ui::glue
