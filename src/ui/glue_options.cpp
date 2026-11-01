#include "ui/glue_options.hpp"

#include "core/application.hpp"
#include "ui/display_modes.hpp"
#include "ui/game_screen.hpp"
#include "ui/glue_ui.hpp"
#include "ui/graphics_presets.hpp"
#include "ui/settings_panel.hpp"
#include "ui/settings_schema.hpp"
#include "ui/ui_manager.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <set>

namespace wowee::ui {

namespace {

GameScreen* gameScreen() {
    auto* uim = core::Application::getInstance().getUIManager();
    return uim ? &uim->getGameScreen() : nullptr;
}

const SettingDesc* findDesc(const std::string& key) {
    std::size_t count = 0;
    const SettingDesc* schema = clientSettingsSchema(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (key == schema[i].key) return &schema[i];
    }
    return nullptr;
}

std::string glueString(const char* key, const char* fallback) {
    return glue::plain(glue::str(key, fallback));
}

std::vector<std::string> splitChoices(const char* s) {
    std::vector<std::string> out;
    if (!s || !*s) return out;
    std::string cur;
    for (const char* p = s; *p; ++p) {
        if (*p == '|') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(*p);
        }
    }
    out.push_back(cur);
    return out;
}

/// One control on a page: a schema row, or one of the two the stock
/// Resolution page has that the schema keeps differently.
struct Control {
    std::string label;
    std::string tooltip;
    std::string restartNote;
    SettingKind kind = SettingKind::Bool;
    float lo = 0.0f;
    float hi = 1.0f;
    float step = 0.0f;
    std::vector<std::string> choices;
    const char* enabledWhen = "";
    std::string defaultValue;
};

/// The resolution, by position in display_modes.hpp. Not a setting: the
/// panel keeps a width and height, and GameScreen picks the row.
constexpr const char* kResolutionKey = "@resolution";
/// Windowed Mode, the stock checkbox, which is "fullscreen" turned over.
constexpr const char* kWindowedKey = "@windowed";

bool controlFor(const std::string& key, Control& c) {
    if (key == kResolutionKey) {
        c.label = glueString("RESOLUTION", "Resolution");
        c.kind = SettingKind::Enum;
        for (int i = 0; i < kNumDisplayResolutions; ++i) c.choices.push_back(displayResolutionLabel(i));
        c.tooltip = "The size of the window, or of the screen mode when full screen.";
        return true;
    }
    if (key == kWindowedKey) {
        c.label = glueString("WINDOWED_MODE", "Windowed Mode");
        c.kind = SettingKind::Bool;
        c.tooltip = "Play in a window rather than full screen.";
        c.defaultValue = "1";
        return true;
    }
    const SettingDesc* d = findDesc(key);
    if (!d) return false;
    c.label = d->label;
    c.tooltip = d->tooltip ? d->tooltip : "";
    c.restartNote = d->restartNote ? d->restartNote : "";
    c.kind = d->kind;
    c.lo = d->minValue;
    c.hi = d->maxValue;
    c.step = d->step;
    c.choices = splitChoices(d->choices);
    c.enabledWhen = d->enabledWhen ? d->enabledWhen : "";
    c.defaultValue = (d->kind == SettingKind::Bool) ? (d->defaultValue != 0.0f ? "1" : "0")
                                                    : settingNumberText(d->defaultValue);
    return true;
}

/// The settings a quality preset sets, so choosing one shows its values in
/// the controls under it before anything is applied.
constexpr const char* kPresetKeys[] = {
    "viewdistance", "shadows",      "shadowdistance", "antialiasing", "fxaa",
    "normalmapping", "normalmapstrength", "parallax", "parallaxquality", "groundclutter",
    "grassenabled", "grassdensity", "grassheight",    "grassdistance",
};

bool isPresetKey(const std::string& key) {
    for (const char* k : kPresetKeys) {
        if (key == k) return true;
    }
    return false;
}

/// A slider's value as its label shows it.
std::string shownNumber(float v, float step) {
    if (step >= 1.0f) return std::to_string(static_cast<long long>(std::lround(v)));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
    std::string s(buf);
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

void GlueOptionsFrame::open() {
    open_ = true;
    restartNotice_ = false;
    pending_.clear();
    // Built again each time: the schema does not change, but which of its
    // rows the panel answers is asked of the panel.
    built_ = false;
    page_ = 1;
    glue::closeDropdown();
}

void GlueOptionsFrame::close() {
    open_ = false;
    pending_.clear();
    glue::closeDropdown();
}

std::string GlueOptionsFrame::value(const std::string& key) const {
    if (key == kWindowedKey) return settingIsOn(value("fullscreen")) ? "0" : "1";
    if (auto it = pending_.find(key); it != pending_.end()) return it->second;
    GameScreen* gs = gameScreen();
    if (!gs) return {};
    if (key == kResolutionKey) return std::to_string(gs->getResolutionIndex());
    return gs->getSettingsPanel().settingValue(key);
}

void GlueOptionsFrame::set(const std::string& key, const std::string& v) {
    if (key == kWindowedKey) {
        set("fullscreen", settingIsOn(v) ? "0" : "1");
        return;
    }
    pending_[key] = v;
    if (key == "graphicspreset") {
        // A preset is a set of values: show them now, as the in-game panel
        // does the moment one is picked. Custom sets nothing.
        const int index = std::atoi(v.c_str()) - 1;
        if (index < 0 || index >= kGraphicsPresetCount) return;
        const GraphicsPresetValues& p = kGraphicsPresets[index];
        const auto on = [](bool b) { return std::string(b ? "1" : "0"); };
        pending_["viewdistance"] = settingNumberText(p.viewDistance);
        pending_["shadows"] = on(p.shadows);
        pending_["shadowdistance"] = settingNumberText(p.shadowDistance);
        pending_["antialiasing"] = settingNumberText(p.antiAliasing);
        pending_["fxaa"] = on(p.fxaa);
        pending_["normalmapping"] = on(p.normalMapping);
        pending_["normalmapstrength"] = settingNumberText(p.normalMapStrength);
        pending_["parallax"] = on(p.parallax);
        pending_["parallaxquality"] = settingNumberText(p.parallaxQuality);
        pending_["groundclutter"] = settingNumberText(p.groundClutter);
        pending_["grassenabled"] = on(p.grass);
        pending_["grassdensity"] = settingNumberText(p.grassDensity);
        pending_["grassheight"] = settingNumberText(p.grassHeight);
        pending_["grassdistance"] = settingNumberText(p.grassDistance);
    } else if (isPresetKey(key)) {
        // Changed by hand, these are no longer the preset; Apply works out
        // whether they happen to be another one.
        pending_["graphicspreset"] = "0";
    }
}

bool GlueOptionsFrame::conditionHolds(const char* cond) const {
    if (!cond || !*cond) return true;
    const std::string c(cond);
    if (const auto ne = c.find("!="); ne != std::string::npos) {
        return std::atof(value(c.substr(0, ne)).c_str()) != std::atof(c.substr(ne + 2).c_str());
    }
    if (const auto eq = c.find('='); eq != std::string::npos) {
        return std::atof(value(c.substr(0, eq)).c_str()) == std::atof(c.substr(eq + 1).c_str());
    }
    return settingIsOn(value(c));
}

bool GlueOptionsFrame::anyPending() const {
    GameScreen* gs = gameScreen();
    if (!gs) return false;
    const SettingsPanel& sp = gs->getSettingsPanel();
    for (const auto& [key, v] : pending_) {
        if (key == kResolutionKey) {
            if (std::atoi(v.c_str()) != gs->getResolutionIndex()) return true;
        } else if (sp.settingValue(key) != v) {
            return true;
        }
    }
    return false;
}

void GlueOptionsFrame::apply() {
    GameScreen* gs = gameScreen();
    if (!gs) return;
    SettingsPanel& sp = gs->getSettingsPanel();
    bool changed = false;
    const auto note = [&](const std::string& key) {
        if (const SettingDesc* d = findDesc(key); d && d->restartNote && *d->restartNote)
            restartNotice_ = true;
    };

    // The resolution through GameScreen, which picks the row, resizes the
    // window and saves.
    if (auto it = pending_.find(kResolutionKey); it != pending_.end()) {
        gs->setResolutionIndex(std::atoi(it->second.c_str()));
    }
    // A preset first, so the settings it covers land on it and only what was
    // changed by hand afterwards is written over it.
    if (auto it = pending_.find("graphicspreset"); it != pending_.end()) {
        if (it->second != "0" && sp.settingValue("graphicspreset") != it->second) {
            sp.setSettingValue("graphicspreset", it->second);
            changed = true;
        }
    }
    for (const auto& [key, v] : pending_) {
        if (key == kResolutionKey || key == "graphicspreset") continue;
        if (sp.settingValue(key) == v) continue;
        // The same call the interface's options and the Lua bridge make: the
        // value is clamped to its row and applied where it takes effect.
        if (sp.setSettingValue(key, v)) {
            changed = true;
            note(key);
        }
    }
    if (changed) gs->saveSettings();
    pending_.clear();
}

void GlueOptionsFrame::defaults() {
    if (page_ < 0 || page_ >= static_cast<int>(pages_.size())) return;
    for (const std::string& key : pages_[static_cast<size_t>(page_)].keys) {
        if (key.empty() || key[0] == '#' || key == kResolutionKey) continue;
        Control c;
        if (!controlFor(key, c)) continue;
        set(key, c.defaultValue);
    }
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

void GlueOptionsFrame::buildPages(SettingsPanel& sp) {
    pages_.clear();
    std::set<std::string> used;
    const auto keep = [&](std::initializer_list<const char*> keys) {
        std::vector<std::string> out;
        for (const char* k : keys) {
            const std::string key(k);
            if (key[0] != '@') {
                const SettingDesc* d = findDesc(key);
                // Only what this client keeps itself: the rows on the
                // interface's stores want the interface running.
                if (!d || (d->store && *d->store) || sp.settingValue(key).empty()) continue;
            }
            out.push_back(key);
            used.insert(key);
        }
        return out;
    };

    // The stock categories: VideoOptionsFrame's Resolution and Effects under
    // Graphics, and AudioOptionsFrame's Sound.
    pages_.push_back({"Graphics", "", {}, 0, true});
    pages_.push_back({glueString("RESOLUTION_LABEL", "Resolution"),
                      glueString("RESOLUTION_SUBTEXT",
                                 "These options allow you to change the size and detail in which "
                                 "your video hardware renders the game."),
                      keep({kResolutionKey, "antialiasing", "framecap", kWindowedKey, "vsync",
                            "brightness"}),
                      1});
    pages_.push_back({glueString("EFFECTS_LABEL", "Effects"),
                      glueString("EFFECTS_SUBTEXT",
                                 "These controls allow you to modify specific detail levels for "
                                 "many game elements and effects."),
                      keep({"graphicspreset", "viewdistance", "shadows", "shadowdistance",
                            "shadowstrength", "groundclutter", "groundclutterdistance", "particledensity",
                            "environmentdetail", "weatherdetail", "texturefiltering"}),
                      1});
    pages_.push_back({"Advanced",
                      "Anti-aliasing, upscaling, surface detail, fog, light and the sky.",
                      keep({"fxaa", "normalmapping", "normalmapstrength", "parallax",
                            "parallaxquality", "upscaling", "fsrquality", "fsrsharpness",
                            "framegen", "lightshafts", "mistdensity", "fogstrength",
                            "fogskyblend", "lensflare", "sharpstars", "sunshafts",
                            "foliagewind"}),
                      0});
    pages_.push_back({"Sound & Voice",
                      glueString("SOUND_SUBTEXT",
                                 "These options control sound hardware and the types and volumes "
                                 "of sounds within the game."),
                      keep({"mutesound", "soundinbackground", "enablesoundeffects", "errorspeech",
                            "characterspeech", "enablemusic", "loopmusic", "enableambience",
                            "mastervolume", "effectsvolume", "musicvolume", "ambientvolume"}),
                      0});

    // Everything else this client keeps, one page per schema category, in
    // the schema's own order and with its section headings.
    std::vector<std::string> order;
    std::unordered_map<std::string, std::vector<std::string>> byCategory;
    std::unordered_map<std::string, std::string> section, emitted;
    std::size_t count = 0;
    const SettingDesc* schema = clientSettingsSchema(count);
    for (std::size_t i = 0; i < count; ++i) {
        const SettingDesc& d = schema[i];
        const std::string category = d.category ? d.category : "";
        if (d.section && *d.section) section[category] = d.section;
        if (d.store && *d.store) continue;
        if (used.count(d.key) || sp.settingValue(d.key).empty()) continue;
        if (!byCategory.count(category)) order.push_back(category);
        auto& keys = byCategory[category];
        if (!section[category].empty() && emitted[category] != section[category]) {
            keys.push_back("#" + section[category]);
            emitted[category] = section[category];
        }
        keys.push_back(d.key);
    }
    if (!order.empty()) {
        pages_.push_back({"Wowee", "", {}, 0, true});
        for (const std::string& category : order) {
            pages_.push_back({category,
                              "Settings WoWee adds beyond the original game's own options. The "
                              "rest are in the in-game Interface options.",
                              byCategory[category], 1});
        }
    }
    page_ = std::clamp(page_, 0, static_cast<int>(pages_.size()) - 1);
    if (pages_[static_cast<size_t>(page_)].header) ++page_;
}

void GlueOptionsFrame::drawPage(PaperUI& ui, ImDrawList* dl, ImVec2 a, ImVec2 b, bool listOpen) {
    if (page_ < 0 || page_ >= static_cast<int>(pages_.size())) return;
    const Page& page = pages_[static_cast<size_t>(page_)];
    const float u = glue::unit();

    // Title (OptionsFontLarge) at TOPLEFT(16,-16), the subtext under it
    // (OptionsFontHighlightSmall), three lines at most.
    glue::text(dl, ImVec2(a.x + 16.0f * u, a.y + 16.0f * u), page.title.c_str(), 18.0f * u,
               glue::kNormal);
    {
        const float size = 10.0f * u;
        const auto lines = glue::wrap(page.subtext, (b.x - a.x) - 48.0f * u, size);
        float y = a.y + 16.0f * u + 18.0f * u + 8.0f * u;
        for (size_t i = 0; i < lines.size() && i < 3; ++i) {
            glue::text(dl, ImVec2(a.x + 16.0f * u, y), lines[i].c_str(), size, glue::kHighlight);
            y += size * 1.25f;
        }
    }

    // The controls: two columns flowing down, the page scrolling when they
    // do not fit.
    struct Item {
        std::string key;
        Control c;
        bool heading = false;
        float h = 0.0f;
    };
    std::vector<Item> items;
    float total = 0.0f;
    bool anyRestart = false;
    for (const std::string& key : page.keys) {
        Item it;
        it.key = key;
        if (!key.empty() && key[0] == '#') {
            it.heading = true;
            it.c.label = key.substr(1);
            it.h = 24.0f * u;
        } else {
            if (!controlFor(key, it.c)) continue;
            switch (it.c.kind) {
                case SettingKind::Bool: it.h = 26.0f * u; break;
                case SettingKind::Enum: it.h = 52.0f * u; break;
                default: it.h = 54.0f * u; break;
            }
            if (!it.c.restartNote.empty()) anyRestart = true;
        }
        total += it.h;
        items.push_back(std::move(it));
    }

    // Split where the two columns come out most even, at a heading when the
    // page has any, so a section is not broken across the two.
    const bool hasHeadings =
        std::any_of(items.begin(), items.end(), [](const Item& i) { return i.heading; });
    size_t split = items.size();
    {
        float best = total;
        float cum = 0.0f;
        for (size_t i = 0; i <= items.size(); ++i) {
            const bool allowed = (i == items.size()) || !hasHeadings || items[i].heading;
            if (allowed) {
                const float worst = std::max(cum, total - cum);
                if (worst < best) {
                    best = worst;
                    split = i;
                }
            }
            if (i < items.size()) cum += items[i].h;
        }
    }

    const ImVec2 bodyA(a.x + 4.0f * u, a.y + 76.0f * u);
    const ImVec2 bodyB(b.x - 4.0f * u, b.y - 6.0f * u);
    const std::string scrollId = "gopt.scroll." + page.title;
    const float top = ui.beginScroll(scrollId.c_str(), bodyA, bodyB);
    const float colGap = 20.0f * u;
    const float colW = ((bodyB.x - bodyA.x) - 24.0f * u - colGap) * 0.5f;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool mouseInBody = mouse.x >= bodyA.x && mouse.x < bodyB.x && mouse.y >= bodyA.y &&
                             mouse.y < bodyB.y;
    float bottom = top;

    for (int column = 0; column < 2; ++column) {
        const size_t first = column == 0 ? 0 : split;
        const size_t last = column == 0 ? split : items.size();
        const float x = bodyA.x + 12.0f * u + (colW + colGap) * static_cast<float>(column);
        float y = top + 14.0f * u;
        for (size_t i = first; i < last; ++i) {
            Item& it = items[i];
            const std::string id = "gopt." + it.key;
            const bool enabled = it.heading || conditionHolds(it.c.enabledWhen);
            std::string label = it.c.label;
            if (!it.c.restartNote.empty()) label += " *";

            if (it.heading) {
                glue::text(dl, ImVec2(x, y + 4.0f * u), label.c_str(), 12.0f * u, glue::kHighlight);
                dl->AddLine(ImVec2(x, y + 19.0f * u), ImVec2(x + colW, y + 19.0f * u),
                            IM_COL32(150, 150, 150, 130), std::max(1.0f, u));
            } else if (it.c.kind == SettingKind::Bool) {
                // OptionsCheckButtonTemplate: the box, its label right of it.
                const float box = 26.0f * u;
                const bool on = settingIsOn(value(it.key));
                bool flip = glue::checkbox(ui, dl, id.c_str(), ImVec2(x, y), box, on, enabled);
                const float size = 12.0f * u;
                const ImVec2 la(x + box, y);
                const ImVec2 lb(x + colW, y + box);
                const std::string labelId = id + ".label";
                flip |= ui.press(labelId.c_str(), la, lb, enabled).clicked;
                glue::text(dl, ImVec2(la.x + 2.0f * u, y + (box - size) * 0.5f), label.c_str(), size,
                           enabled ? glue::kNormal : glue::kDisable);
                if (flip) set(it.key, on ? "0" : "1");
            } else if (it.c.kind == SettingKind::Enum) {
                int index = std::atoi(value(it.key).c_str());
                if (glue::dropdown(ui, dl, id.c_str(), ImVec2(x - 12.0f * u, y + 12.0f * u),
                                   colW / u - 44.0f, label.c_str(), it.c.choices, &index, enabled)) {
                    set(it.key, std::to_string(index));
                }
            } else {
                float v = static_cast<float>(std::atof(value(it.key).c_str()));
                const std::string shown = label + ": " + shownNumber(v, it.c.step);
                const std::string low = shownNumber(it.c.lo, it.c.step);
                const std::string high = shownNumber(it.c.hi, it.c.step);
                const float step = it.c.kind == SettingKind::Int ? std::max(1.0f, it.c.step)
                                                                 : it.c.step;
                if (glue::slider(ui, dl, id.c_str(), ImVec2(x + 4.0f * u, y + 14.0f * u),
                                 colW - 12.0f * u, &v, it.c.lo, it.c.hi, step, shown.c_str(),
                                 low.c_str(), high.c_str(), enabled)) {
                    set(it.key, it.c.kind == SettingKind::Int
                                    ? std::to_string(static_cast<long long>(std::lround(v)))
                                    : settingNumberText(v));
                }
            }

            // The tooltip for whatever row the pointer is on.
            if (!it.heading && !listOpen && mouseInBody && mouse.x >= x && mouse.x < x + colW &&
                mouse.y >= y && mouse.y < y + it.h) {
                tooltipText_ = it.c.label;
                if (!it.c.tooltip.empty()) tooltipText_ += "\n" + it.c.tooltip;
                if (!it.c.restartNote.empty()) tooltipText_ += "\n" + it.c.restartNote;
            }
            y += it.h;
        }
        bottom = std::max(bottom, y);
    }
    ui.endScroll(bottom + 8.0f * u);

    if (anyRestart) {
        const float size = 10.0f * u;
        glue::textRight(dl, b.x - 16.0f * u, a.y + 16.0f * u + 4.0f * u,
                        "* takes effect after a restart", size, glue::kNormal);
    }
}

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

void GlueOptionsFrame::render(PaperUI& ui) {
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float u = glue::unit();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ui.setLayer(PaperLayer::Overlay);

    if (!open_) {
        // What the in-game frame asks on closing when a change wants a
        // restart: here it can only be said.
        if (restartNotice_) {
            if (glue::dialog(ui, "gopt.restart",
                             "Some of the changes you made take effect the next time WoWee "
                             "starts.",
                             glueString("OKAY", "Okay").c_str()) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
                restartNotice_ = false;
            }
        }
        ui.setLayer(PaperLayer::Page);
        return;
    }

    GameScreen* gs = gameScreen();
    if (!gs) {
        close();
        ui.setLayer(PaperLayer::Page);
        return;
    }
    if (!built_) {
        buildPages(gs->getSettingsPanel());
        built_ = true;
    }

    // Asked before anything is drawn: while a list is down, everything but
    // the list is deaf, and a key that closes the list closes only it.
    const bool listOpen = glue::dropdownOpen();

    // OptionsFrameTemplate's backdrop frame: the whole screen at .75 black.
    dl->AddRectFilled(ImVec2(0, 0), screen, IM_COL32(0, 0, 0, 191));
    ui.claimMouse(ImVec2(0, 0), screen);

    const float w = 648.0f * u;
    const float h = 520.0f * u;
    const ImVec2 a(std::floor((screen.x - w) * 0.5f), std::floor((screen.y - h) * 0.5f));
    const ImVec2 b(a.x + w, a.y + h);
    glue::backdrop(dl, a, b, "Interface\\DialogFrame\\UI-DialogBox-Background",
                   "Interface\\DialogFrame\\UI-DialogBox-Border", 32.0f * u, 32.0f * u, 11.0f * u,
                   11.0f * u, 12.0f * u, 10.0f * u);
    {
        // UI-DialogBox-Header, 300x68 at TOP(0,12), OPTIONS 14 units down it.
        const float cx = (a.x + b.x) * 0.5f;
        const float top = a.y - 12.0f * u;
        glue::image(dl, "Interface\\DialogFrame\\UI-DialogBox-Header",
                    ImVec2(cx - 150.0f * u, top), ImVec2(cx + 150.0f * u, top + 68.0f * u));
        glue::textCentered(dl, cx, top + 14.0f * u, glueString("OPTIONS", "Options").c_str(),
                           glue::kFontNormal * u, glue::kNormal);
    }

    if (listOpen) ui.pushInert();

    // The category list: 175x429 at TOPLEFT(22,-40), 18-unit rows.
    const ImVec2 ca(a.x + 22.0f * u, a.y + 40.0f * u);
    const ImVec2 cb(ca.x + 175.0f * u, ca.y + 429.0f * u);
    glue::backdrop(dl, ca, cb, "", "Interface\\Tooltips\\UI-Tooltip-Border", 16.0f * u, 16.0f * u,
                   5.0f * u, 5.0f * u, 5.0f * u, 5.0f * u);
    {
        const float rows = static_cast<float>(pages_.size());
        const float rowH = std::min(18.0f * u, ((cb.y - ca.y) - 16.0f * u) / std::max(rows, 1.0f));
        float y = ca.y + 8.0f * u;
        for (int i = 0; i < static_cast<int>(pages_.size()); ++i) {
            const Page& p = pages_[static_cast<size_t>(i)];
            const ImVec2 ra(ca.x + 6.0f * u, y);
            const ImVec2 rb(cb.x - 6.0f * u, y + rowH);
            const std::string id = "gopt.cat." + std::to_string(i);
            const PaperUI::PressState st = ui.press(id.c_str(), ra, rb);
            const bool selected = (i == page_);
            if (selected || st.hovered) {
                glue::image(dl, "Interface\\QuestFrame\\UI-QuestLogTitleHighlight", ra, rb,
                            ImVec2(0, 0), ImVec2(1, 1),
                            selected ? IM_COL32(110, 160, 255, 255) : IM_COL32_WHITE, true);
            }
            const float size = (p.depth == 0 ? glue::kFontNormal : glue::kFontSmall) * u;
            const float indent = (p.depth == 0 ? 8.0f : 22.0f) * u;
            glue::text(dl, ImVec2(ra.x + indent, (ra.y + rb.y) * 0.5f - size * 0.5f - 1.0f * u),
                       p.title.c_str(), std::min(size, rowH),
                       (selected || st.hovered) ? glue::kHighlight : glue::kNormal);
            if (st.clicked) {
                // A heading row opens the first page under it.
                page_ = (p.header && i + 1 < static_cast<int>(pages_.size())) ? i + 1 : i;
                glue::closeDropdown();
            }
            y += rowH;
        }
    }

    // The page: the panel container, TOPLEFT 16 right of the list to 22 in
    // from the frame's right, its border at .6 grey.
    const ImVec2 pa(cb.x + 16.0f * u, ca.y);
    const ImVec2 pb(b.x - 22.0f * u, cb.y + 1.0f * u);
    glue::backdrop(dl, pa, pb, "", "Interface\\Tooltips\\UI-Tooltip-Border", 16.0f * u, 16.0f * u,
                   5.0f * u, 5.0f * u, 5.0f * u, 5.0f * u, IM_COL32_WHITE,
                   IM_COL32(153, 153, 153, 255));
    tooltipText_.clear();
    drawPage(ui, dl, pa, pb, listOpen);

    // Defaults at BOTTOMLEFT(16,8); Apply at BOTTOMRIGHT(-16,8) with Cancel
    // and Okay to its left. GlueButtonSmallTemplateBlue, 110x38.
    bool okay = false;
    {
        const float bw = 110.0f * u, bh = 38.0f * u;
        const float y1 = b.y - 8.0f * u;
        const float x1 = b.x - 16.0f * u;
        if (glue::button(ui, dl, "gopt.defaults", ImVec2(a.x + 16.0f * u, y1 - bh),
                         ImVec2(a.x + 16.0f * u + bw, y1), glueString("DEFAULTS", "Defaults").c_str(),
                         glue::ButtonStyle::Blue, true, glue::kFontSmall)) {
            defaults();
        }
        if (glue::button(ui, dl, "gopt.apply", ImVec2(x1 - bw, y1 - bh), ImVec2(x1, y1),
                         glueString("APPLY", "Apply").c_str(), glue::ButtonStyle::Blue,
                         anyPending(), glue::kFontSmall)) {
            apply();
        }
        if (glue::button(ui, dl, "gopt.cancel", ImVec2(x1 - bw * 2.0f, y1 - bh),
                         ImVec2(x1 - bw, y1), glueString("CANCEL", "Cancel").c_str(),
                         glue::ButtonStyle::Blue, true, glue::kFontSmall)) {
            close();
        }
        if (glue::button(ui, dl, "gopt.okay", ImVec2(x1 - bw * 3.0f, y1 - bh),
                         ImVec2(x1 - bw * 2.0f, y1), glueString("OKAY", "Okay").c_str(),
                         glue::ButtonStyle::Blue, true, glue::kFontSmall)) {
            okay = true;
        }
    }

    if (listOpen) ui.popInert();
    glue::dropdownList(ui, dl);

    if (!tooltipText_.empty() && !glue::dropdownOpen()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        glue::tooltip(dl, ImVec2(m.x + 18.0f * u, m.y + 18.0f * u), tooltipText_);
    }

    // OptionsFrame_OnKeyDown: Escape is Cancel and Enter is Okay.
    if (!listOpen && open_) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            close();
        } else if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                   ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
            okay = true;
        }
    }
    if (okay && open_) {
        apply();
        close();
    }
    ui.setLayer(PaperLayer::Page);
}

}  // namespace wowee::ui
