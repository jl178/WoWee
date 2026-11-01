#include "ui/auth_screen.hpp"
#include <future>
#include <chrono>
#include "rendering/pom_quality.hpp"
#include "ui/graphics_presets.hpp"
#include "ui/settings_panel.hpp"
#include "auth/crypto.hpp"
#include "core/application.hpp"
#include "pipeline/asset_inventory.hpp"
#include "core/config_paths.hpp"
#include "core/logger.hpp"
#include "core/open_url.hpp"
#include "core/version.hpp"
#include "core/window.hpp"
#include "rendering/renderer.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/character_preview.hpp"
#include "ui/ui_texture_load.hpp"
#include "ui/glue_ui.hpp"
#include "pipeline/asset_manager.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/music_manager.hpp"
#include "game/expansion_profile.hpp"
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include "stb_image.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <iomanip>
#include <unordered_map>

namespace wowee { namespace ui {

static std::string trimAscii(std::string s) {
    auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t b = 0;
    while (b < s.size() && isSpace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && isSpace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

static std::string hexEncode(const std::vector<uint8_t>& data) {
    std::ostringstream ss;
    for (uint8_t b : data)
        ss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(b);
    return ss.str();
}

static std::vector<uint8_t> hexDecode(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        try {
            uint8_t b = static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16));
            bytes.push_back(b);
        } catch (...) {
            return {};
        }
    }
    return bytes;
}

AuthScreen::AuthScreen() {
}

AuthScreen::~AuthScreen() {
    // The background image was uploaded with plain vkCreateImage and
    // vkAllocateMemory and nothing released it, so it and its view outlived
    // the device on every run. bgSampler is the context's, cached and shared,
    // and bgDescriptorSet belongs to the ImGui backend, which frees its own
    // pool -- neither is this class's to destroy.
    if (!bgVkCtx) return;
    VkDevice device = bgVkCtx->getDevice();
    if (device == VK_NULL_HANDLE) return;
    if (bgImageView) { vkDestroyImageView(device, bgImageView, nullptr); bgImageView = VK_NULL_HANDLE; }
    if (bgImage)     { vkDestroyImage(device, bgImage, nullptr);         bgImage = VK_NULL_HANDLE; }
    if (bgMemory)    { vkFreeMemory(device, bgMemory, nullptr);          bgMemory = VK_NULL_HANDLE; }
}

std::string AuthScreen::makeServerKey(const std::string& host, int port) {
    std::ostringstream ss;
    ss << host << ":" << port;
    return ss.str();
}

std::string AuthScreen::serverRowLabel(const ServerProfile& s) {
    std::string row = s.label.empty() ? makeServerKey(s.hostname, s.port) : s.label;
    if (!s.username.empty()) row += "   " + s.username;
    return row;
}

std::string AuthScreen::currentExpansionId() const {
    auto* reg = core::Application::getInstance().getExpansionRegistry();
    if (reg && reg->getActive()) {
        return reg->getActive()->id;
    }
    return "wotlk";
}

void AuthScreen::seedKnownServers() {
    // The servers this client is developed and tested against, offered by
    // name. A first run used to open on an empty list with the address
    // defaulted to localhost - which is a server nobody new has - so the only
    // way to reach anything was to already know a realmlist, and to find the
    // box for it behind "more options". Nothing here is a recommendation; it
    // is the address somebody would otherwise be looking up.
    struct Known {
        const char* label;
        const char* hostname;
        int port;
        const char* expansionId;
        /// Content pack index, or empty. A known realm that ships one would
        /// still want a first-time confirmation before anything downloads;
        /// none does yet, so none is listed.
        const char* packUrl;
    };
    static constexpr Known kKnown[] = {
        {"ChromieCraft", "logon.chromiecraft.com", 3724, "wotlk", ""},
    };

    for (const Known& k : kKnown) {
        const std::string key = makeServerKey(k.hostname, k.port);
        bool already = false;
        for (ServerProfile& s : servers_) {
            if (makeServerKey(s.hostname, s.port) != key) continue;
            // Somebody has logged into it before, so their own entry stands -
            // it carries their account. It only gains the name.
            if (s.label.empty()) s.label = k.label;
            already = true;
            break;
        }
        if (already) continue;

        ServerProfile s;
        s.hostname = k.hostname;
        s.port = k.port;
        s.expansionId = k.expansionId;
        s.label = k.label;
        s.packUrl = k.packUrl;
        servers_.push_back(std::move(s));
    }
}

void AuthScreen::selectServerProfile(int index) {
    if (index < 0 || index >= static_cast<int>(servers_.size())) {
        selectedServerIndex_ = -1;
        return;
    }

    selectedServerIndex_ = index;
    const auto& s = servers_[index];

    hostname_.setText(s.hostname);
    setPort(s.port);
    username_.setText(s.username);

    savedPasswordHash = s.passwordHash;
    usingStoredHash = !savedPasswordHash.empty();
    password_.setText(usingStoredHash ? PASSWORD_PLACEHOLDER : "");

    auto& app = core::Application::getInstance();
    if (!s.expansionId.empty()) {
        auto* expReg = app.getExpansionRegistry();
        if (expReg && expReg->setActive(s.expansionId)) {
            auto& profiles = expReg->getAllProfiles();
            for (int i = 0; i < static_cast<int>(profiles.size()); ++i) {
                if (profiles[i].id == s.expansionId) { expansionIndex = i; break; }
            }
        }
    }
    assetProfileId_ = s.assetProfileId;
    if (!app.setAssetExpansionOverride(assetProfileId_)) {
        assetProfileId_.clear();
        app.setAssetExpansionOverride({});
    }
    app.reloadExpansionData();

    packUrl_.setText(s.packUrl);
    packSkipped_ = false;
    startPackSync();
}

void AuthScreen::startPackSync() {
    auto& app = core::Application::getInstance();
    auto* assets = app.getAssetManager();
    if (assets == nullptr) return;
    std::string url = trimAscii(packUrl_.text());
    if (!core::readSettingsFlag("content_packs", true)) url.clear();
    app.getWowpackSync().start(url, assets->getOverridePath(), currentExpansionId());
}

void AuthScreen::upsertCurrentServerProfile(bool includePasswordHash) {
    const std::string hostStr = hostname_.text();
    if (hostStr.empty() || port <= 0) {
        return;
    }

    const std::string key = makeServerKey(hostStr, port);
    int foundIndex = -1;
    for (int i = 0; i < static_cast<int>(servers_.size()); ++i) {
        if (makeServerKey(servers_[i].hostname, servers_[i].port) == key) {
            foundIndex = i;
            break;
        }
    }

    ServerProfile s;
    s.hostname = hostStr;
    s.port = port;
    s.username = username_.text();
    s.expansionId = currentExpansionId();
    s.assetProfileId = assetProfileId_;
    s.packUrl = trimAscii(packUrl_.text());
    if (foundIndex >= 0) s.label = servers_[foundIndex].label;
    if (includePasswordHash && !savedPasswordHash.empty()) {
        s.passwordHash = savedPasswordHash;
    } else if (foundIndex >= 0) {
        // Preserve existing stored hash if we aren't updating it.
        s.passwordHash = servers_[foundIndex].passwordHash;
    }

    if (foundIndex >= 0) {
        servers_[foundIndex] = std::move(s);
        selectedServerIndex_ = foundIndex;
    } else {
        servers_.push_back(std::move(s));
        selectedServerIndex_ = static_cast<int>(servers_.size()) - 1;
    }

    // Keep deterministic ordering (and stable combo ordering) across runs.
    std::sort(servers_.begin(), servers_.end(),
              [](const ServerProfile& a, const ServerProfile& b) {
                  if (a.hostname != b.hostname) return a.hostname < b.hostname;
                  return a.port < b.port;
              });

    // Fix up index after sort.
    for (int i = 0; i < static_cast<int>(servers_.size()); ++i) {
        if (makeServerKey(servers_[i].hostname, servers_[i].port) == key) {
            selectedServerIndex_ = i;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// The card
// ---------------------------------------------------------------------------

namespace {

// The card's layout, written in units and drawn at a scale taken from the
// window, so what follows is its proportions rather than its pixels.
constexpr float kCardWidth     = 432.0f;
constexpr float kCardPadX      = 34.0f;
constexpr float kCardPadTop    = 26.0f;
constexpr float kCardPadBottom = 20.0f;
constexpr float kTitleSize     = 44.0f;
constexpr float kLabelSize     = 14.0f;
constexpr float kBodySize      = 15.0f;
constexpr float kSmallSize     = 13.0f;
constexpr float kFieldHeight   = 38.0f;
constexpr float kButtonHeight  = 46.0f;
constexpr float kRowGap        = 14.0f;
constexpr float kLabelGap      = 3.0f;

/// A code that looks like one someone finished typing.
///
/// PIN grids run four to ten digits and an authenticator is six, which is why
/// six is named twice: it is in range either way and the check reads clearer
/// saying so than leaving it implied.
bool looksLikeSecurityCode(const std::string& code) {
    if (code.empty()) return false;
    for (char c : code)
        if (c < '0' || c > '9') return false;
    return (code.size() >= 4 && code.size() <= 10) || code.size() == 6;
}

} // namespace

void AuthScreen::setPort(int value) {
    port = std::clamp(value, 1, 65535);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d", port);
    portText_.setText(buf);
}

void AuthScreen::render(auth::AuthHandler& authHandler) {
    // Load saved login info on first render
    if (!loginInfoLoaded) {
        loadLoginInfo();
        // After the load, so it covers every way out of it - a config that
        // was read, one that was migrated, and the first run where there is
        // no file at all and the list would otherwise be empty.
        seedKnownServers();
        if (selectedServerIndex_ < 0 && !servers_.empty()) selectServerProfile(0);
        loginInfoLoaded = true;
        if (portText_.empty()) setPort(port);
        // Only when the list left nothing to point at. This used to run
        // unconditionally, so a first run opened aimed at a server the person
        // running it does not have.
        if (hostname_.empty()) hostname_.setText("localhost");
        auto* registry = core::Application::getInstance().getExpansionRegistry();
        if (registry && registry->getActive()) {
            const auto& profiles = registry->getAllProfiles();
            for (int i = 0; i < static_cast<int>(profiles.size()); ++i) {
                if (profiles[i].id == registry->getActive()->id) {
                    expansionIndex = i;
                    break;
                }
            }
        }
    }

    // Once the saved profile is in the fields and its content pack, if any,
    // has finished syncing. attemptAuth refuses while a sync runs.
    if (!autoLoginTried_) {
        const char* env = std::getenv("WOWEE_AUTO_LOGIN");
        if (!env || !*env) {
            autoLoginTried_ = true;
        } else if (!core::Application::getInstance().getWowpackSync().running()) {
            autoLoginTried_ = true;
            // account:password, or any other value to use the saved profile.
            const std::string spec(env);
            const auto colon = spec.find(':');
            if (colon != std::string::npos && colon > 0) {
                username_.setText(spec.substr(0, colon));
                password_.setText(spec.substr(colon + 1));
                usingStoredHash = false;
            }
            LOG_WARNING("Auto login as ", username_.text(), " at ", hostname_.text(), ":", port);
            attemptAuth(authHandler);
        }
    }

    drawBackdrop();
    updateMusic();

    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    // A share of the window rather than a count of pixels, so the card is the
    // same size on a phone, on a laptop and on a 4K monitor. The floor keeps
    // it legible in a small window; the ceiling keeps it from swallowing a
    // large one.
    const float scale = std::clamp(std::min(screen.x / 1280.0f, screen.y / 760.0f), 0.62f, 2.6f);
    ui_.begin(ImGui::GetIO().DeltaTime, scale);
    ui_.setLayer(PaperLayer::Page);

    // Everything about authenticating that is not drawing. It runs before the
    // card so the card can simply read the state it leaves behind.
    const auto authState = authHandler.getState();
    const bool waitingForSecurityCode = (authState == auth::AuthState::PIN_REQUIRED ||
                                         authState == auth::AuthState::AUTHENTICATOR_REQUIRED);

    if (authenticating) {
        if (!waitingForSecurityCode) {
            pinAutoSubmitted_ = false;
            securityPromptFocused_ = false;
            authTimer += ImGui::GetIO().DeltaTime;
        } else if (!pinAutoSubmitted_ && looksLikeSecurityCode(pinCode_.text())) {
            // The player typed a code before pressing the button; send it now
            // rather than making them press a second one.
            authHandler.submitSecurityCode(pinCode_.text());
            pinCode_.clear();
            pinAutoSubmitted_ = true;
        }

        if (authState == auth::AuthState::AUTHENTICATED) {
            setStatus("Authentication successful!", false);
            authenticating = false;

            // Compute and save password hash if user typed a fresh password
            if (!usingStoredHash) {
                std::string upperUser = username_.text();
                std::string upperPass = password_.text();
                auto toUp = [](unsigned char c) { return static_cast<char>(std::toupper(c)); };
                std::transform(upperUser.begin(), upperUser.end(), upperUser.begin(), toUp);
                std::transform(upperPass.begin(), upperPass.end(), upperPass.begin(), toUp);
                std::string combined = upperUser + ":" + upperPass;
                auto hash = auth::Crypto::sha1(combined);
                savedPasswordHash = hexEncode(hash);
            }
            saveLoginInfo(true);

            if (onSuccess) {
                onSuccess();
            }
        } else if (authState == auth::AuthState::FAILED) {
            // Protocol fallback: a 1.12 realm that speaks the other vanilla auth
            // protocol byte rejects or drops our handshake rather than replying
            // usefully. Retry once on the next candidate before giving up - but
            // only for protocol-shaped failures, never for a rejected password
            // (retrying those can trip server-side lockouts).
            const bool haveFallback = (authProtocolAttempt_ + 1 < authProtocols_.size());
            if (haveFallback && authHandler.lastFailureWasProtocol()) {
                LOG_INFO("Auth failed on protocol ",
                         static_cast<int>(authProtocols_[authProtocolAttempt_]),
                         " (", failureReason, ") - retrying with protocol ",
                         static_cast<int>(authProtocols_[authProtocolAttempt_ + 1]));
                ++authProtocolAttempt_;
                authHandler.disconnect();
                beginAuthAttempt(authHandler);
            } else {
                setStatus(failureReason.empty() ? "Authentication failed" : failureReason, true);
                authenticating = false;
            }
        } else if (!waitingForSecurityCode && authTimer >= AUTH_TIMEOUT) {
            setStatus("Connection timed out - server did not respond", true);
            authenticating = false;
            authHandler.disconnect();
        }
    }

    // The glue scene came up in drawBackdrop above: draw the stock login
    // over it. Without it - no assets yet - the paper card is the screen.
    const bool glueMode = glueScene_ != nullptr;
    glueModalUp_ = false;
    if (!glueMode) renderProminentStatus(screen.x, screen.y);

    // Whether a dropdown was down when the frame began. Escape closes one
    // from inside the control, so asking afterwards would find it already
    // shut and go on to close the settings sheet behind it - two things
    // dismissed by one press.
    const bool popupWasOpen = ui_.popupOpen();

    // The card hears nothing while the settings sheet is over it, but it is
    // still drawn - a modal that blanks what it covers loses the player's
    // place.
    //
    // Asked once. The gear is on the card, so rendering it can turn the sheet
    // on halfway through, and asking again afterwards popped an inertness
    // that had never been pushed.
    const bool sheetWasOpen = settingsOpen_;
    if (sheetWasOpen) ui_.pushInert();
    if (glueMode) {
        renderGlueLogin(authHandler, screen.x, screen.y);
    } else {
        renderCard(authHandler, screen.x, screen.y);
    }
    if (sheetWasOpen) ui_.popInert();

    if (settingsOpen_) renderLoginSettingsSheet(screen.x, screen.y);

    // Escape backs out of one thing per press: the open dropdown, then the
    // settings sheet, then the keyboard.
    if (!popupWasOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (settingsOpen_) {
            settingsOpen_ = false;
        } else if (serverSheetOpen_) {
            serverSheetOpen_ = false;
        } else {
            ui_.clearFocus();
        }
    }

    // Enter with the keyboard in none of the boxes still means log in. The
    // screen opens with nothing focused - deliberately, so a phone does not
    // meet it with the on-screen keyboard already up over the card - and
    // pressing Enter there should do the obvious thing rather than nothing.
    if (!settingsOpen_ && !serverSheetOpen_ && !glueModalUp_ && !authenticating &&
        !ui_.wantsTextInput() &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
         ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
        attemptAuth(authHandler);
    }

    // Build version, bottom-left over the login art, as the retail client
    // does. On the overlay because the art behind it is a photograph and it
    // needs its own shadow to stay readable over any part of one. The glue
    // login draws its own, in AccountLoginVersion's place.
    if (!glueMode) {
        ui_.setLayer(PaperLayer::Overlay);
        const float size = ui_.px(kSmallSize);
        const ImVec2 at(ui_.px(14), screen.y - ui_.lineHeight(size) - ui_.px(8));
        ui_.text(ImVec2(at.x + 1.0f, at.y + 1.0f), core::kVersionString, size,
                 IM_COL32(0, 0, 0, 150));
        ui_.text(at, core::kVersionString, size, IM_COL32(0xEC, 0xE2, 0xCC, 0xC8));

        // And beside it, if GitHub has a newer one. Next to the version
        // rather than in the card: it is about the program, not about
        // logging in, and the card is the busiest thing on the screen
        // already. Nothing is downloaded - clicking it opens the release
        // page and the player decides from there.
        const core::UpdateCheck& updates = core::Application::getInstance().getUpdateCheck();
        if (const std::string newer = updates.newerVersion(); !newer.empty()) {
            const std::string note = "  -  " + newer + " is available";
            const ImVec2 beside(at.x + ui_.textWidth(core::kVersionString, size), at.y);
            ui_.text(ImVec2(beside.x + 1.0f, beside.y + 1.0f), note.c_str(), size,
                     IM_COL32(0, 0, 0, 150));
            if (ui_.link("update", beside, note.c_str(), size,
                         IM_COL32(0xF6, 0xD9, 0x6B, 0xE0))) {
                // openExternalUrl refuses anything that is not a plain
                // http(s) URL, which is the standard every caller is held to
                // - this one comes off the network like a chat link does.
                core::openExternalUrl(updates.releaseUrl());
            }
        }
        ui_.setLayer(PaperLayer::Page);
    }

    ui_.end();
}

void AuthScreen::renderProminentStatus(float screenW, float screenH) {
    // Across the top of the screen, on its own strip of paper.
    //
    // A disconnect is not a form being wrong - the player did not ask to be
    // here and may not have been looking - so it is said where it cannot be
    // missed rather than as another red line inside the card. It is cleared
    // like any other status: by typing, or by connecting again.
    if (!statusProminent || statusMessage.empty()) return;

    ui_.setLayer(PaperLayer::Overlay);
    const float size = ui_.px(30);
    const float pad = ui_.px(26);
    const float width = std::min(screenW - ui_.px(80),
                                 ui_.textWidth(statusMessage.c_str(), size) + pad * 2.0f);
    const float textW = width - pad * 2.0f;
    const float textH = ui_.wrappedHeight(textW, statusMessage.c_str(), size);
    const float x0 = (screenW - width) * 0.5f;
    const float y0 = screenH * 0.10f;
    const ImVec2 a(x0, y0);
    const ImVec2 b(x0 + width, y0 + textH + pad);

    ui_.sheet(a, b, /*taped=*/false);
    ui_.wrapped(ImVec2(a.x + pad, a.y + pad * 0.5f), textW, statusMessage.c_str(), size,
                ui_.theme().crayonRed);
    ui_.setLayer(PaperLayer::Page);
}

namespace {

/// One asset set that can be played with: a label to show and the id to set.
struct AssetChoice {
    std::string label;
    std::string id;     ///< empty means "whatever the protocol expansion is"
};

/// Every asset set actually installed, in the order they should be offered.
///
/// Several games can be built into one Data folder, each under its own name,
/// and which one to draw with is a choice worth making before logging in - a
/// Wrath server played with Cataclysm's art is the whole point of building
/// more than one. Only sets with a manifest are listed: an expansion the
/// registry knows about but which was never extracted is not something anyone
/// can choose.
std::vector<AssetChoice> assetChoices(const game::ExpansionRegistry* registry) {
    std::vector<AssetChoice> out;
    if (registry == nullptr) return out;

    const game::ExpansionProfile* active = registry->getActive();
    out.push_back({active ? "Match protocol  (" + active->shortName + ")"
                          : std::string("Match protocol"),
                   std::string()});

    for (const auto& candidate : registry->getAllProfiles()) {
        if (!std::filesystem::exists(candidate.dataPath + "/manifest.json")) continue;
        out.push_back({candidate.shortName + " assets", candidate.id});
    }

    const char* dataPathEnv = std::getenv("WOW_DATA_PATH");
    const std::filesystem::path baseData = dataPathEnv ? dataPathEnv : "./Data";
    if (std::filesystem::exists(baseData / "manifest.json")) {
        out.push_back({"Legacy root Data", "legacy"});
    }
    return out;
}

/// One line saying what the assets about to be used actually are.
///
/// Which set that is depends on the override: with none, it is whatever the
/// protocol expansion's is, which is the answer somebody gets by doing nothing.
std::string assetLine(const pipeline::AssetInventory& inventory,
                      const game::ExpansionRegistry* registry,
                      const std::string& overrideId) {
    std::string id = overrideId;
    if (id == "legacy") return {};
    if (id.empty() && registry != nullptr) {
        if (const game::ExpansionProfile* active = registry->getActive()) id = active->id;
    }
    const pipeline::AssetSet* set = inventory.find(id);
    if (set == nullptr || !set->usable()) return {};
    return set->summary();
}

/// One line about the selected server's content pack, or empty when the
/// server has none.
std::string packStatusLine(const core::WowpackSync& sync) {
    using State = core::WowpackSync::State;
    switch (sync.state()) {
        case State::Idle:
            return {};
        case State::Running: {
            const std::uint64_t total = sync.filesTotal();
            if (total == 0) return "Content pack: checking...";
            char buf[160];
            std::snprintf(buf, sizeof(buf), "Content pack: %llu of %llu files, %.1f of %.1f MB",
                          static_cast<unsigned long long>(sync.filesDone()),
                          static_cast<unsigned long long>(total),
                          static_cast<double>(sync.bytesDone()) / (1024.0 * 1024.0),
                          static_cast<double>(sync.bytesTotal()) / (1024.0 * 1024.0));
            return buf;
        }
        case State::Done: {
            const std::string name = sync.packName();
            if (name.empty()) return "Content pack removed";
            return "Content pack " + name + " " + sync.packVersion() + " is up to date";
        }
        case State::Failed:
            return "Content pack: " + sync.error();
    }
    return {};
}

}  // namespace

void AuthScreen::renderCard(auth::AuthHandler& authHandler, float screenW, float screenH) {
    const PaperTheme& theme = ui_.theme();
    const auto px = [this](float units) { return ui_.px(units); };

    const auto authState = authHandler.getState();
    const bool waitingForSecurityCode = (authState == auth::AuthState::PIN_REQUIRED ||
                                         authState == auth::AuthState::AUTHENTICATOR_REQUIRED);

    const float labelSize = px(kLabelSize);
    const float bodySize = px(kBodySize);
    const float smallSize = px(kSmallSize);
    const float labelRow = ui_.lineHeight(labelSize) + px(kLabelGap);
    const float fieldRow = labelRow + px(kFieldHeight);
    const float smallRow = ui_.lineHeight(smallSize);
    const float contentW = px(kCardWidth) - px(kCardPadX) * 2.0f;

    // A security prompt outranks the disclosure: when the server is waiting
    // for a code, the box for it belongs in the middle of the card and not
    // three rows down behind a link.
    const bool codeInMain = waitingForSecurityCode;
    const bool codeInAdvanced = !codeInMain;

    const float statusH = statusMessage.empty()
        ? 0.0f
        : ui_.wrappedHeight(contentW, statusMessage.c_str(), bodySize);

    // The card is sized before it is drawn, so every row that might not
    // appear has to be asked about here on the same terms the drawing asks -
    // otherwise the sheet comes out taller than what is on it.
    auto* registry = core::Application::getInstance().getExpansionRegistry();
    const bool haveExpansions = registry && !registry->getAllProfiles().empty();

    float advancedH = 0.0f;
    if (advancedOpen_) {
        advancedH = px(kRowGap) + px(2);                     // the rule above it
        advancedH += fieldRow + px(kRowGap);                 // address and port
        if (haveExpansions) {
            advancedH += fieldRow + px(kRowGap);             // expansion
        } else {
            advancedH += smallRow + px(kRowGap);             // the one line instead
        }
        if (codeInAdvanced) advancedH += fieldRow + px(kRowGap);
        advancedH += fieldRow + px(kRowGap);                 // content pack url
#ifdef WOWEE_HAVE_ASSET_PANEL
        advancedH += smallRow + px(kRowGap);             // the assets link
#endif
    }

    // The title, its rule and the gap under it, measured once and laid out from
    // the same number below. Measuring it as the em while drawing it as the
    // ink left the card short by a third of a title's height, and everything
    // that sits at the bottom of it - the graphics button, the close button -
    // was drawn over the sheet's own bottom edge.
    const float titleBlockH = ui_.inkHeight(px(kTitleSize), true) * 1.02f + px(18);

    float contentH = titleBlockH;                            // title and its underline
    contentH += fieldRow + px(kRowGap);                      // server
    contentH += fieldRow + px(kRowGap);                      // account
    contentH += fieldRow + px(kRowGap);                      // password

    // The assets row sits in the card itself rather than behind the disclosure,
    // and only when there is more than one set installed - with a single set
    // there is nothing to choose and the row is a question with one answer.
    const std::vector<AssetChoice> assets = assetChoices(registry);
    const bool chooseAssets = assets.size() > 2;
    if (chooseAssets) {
        contentH += fieldRow + px(kRowGap);
        if (!assetProfileId_.empty()) contentH += smallRow + px(4);
    }

    // What is installed, said on the screen somebody is looking at when they
    // wonder. Either a line describing the set about to be played with, or -
    // when there is nothing to play with - what to do about it, which is the
    // one thing the client never used to say.
    const pipeline::AssetInventory& inventory =
        core::Application::getInstance().getAssetInventory();
    const std::string trouble = inventory.troubleText();
    const std::string haveLine = assetLine(inventory, registry, assetProfileId_);

    float troubleH = 0.0f;
    float haveH = 0.0f;
    if (!trouble.empty()) {
        troubleH = ui_.wrappedHeight(contentW, trouble.c_str(), smallSize) + px(6);
        contentH += troubleH;
    } else if (!haveLine.empty()) {
        haveH = ui_.wrappedHeight(contentW, haveLine.c_str(), smallSize) + px(6);
        contentH += haveH;
    }

    // The content pack of the selected server: syncing, synced, or failed.
    // One line, the same size as the "what is installed" line above it.
    auto& packSync = core::Application::getInstance().getWowpackSync();
    const std::string packLine = packStatusLine(packSync);
    float packH = 0.0f;
    if (!packLine.empty()) {
        packH = ui_.wrappedHeight(contentW, packLine.c_str(), smallSize) + px(6);
        if (packSync.state() == core::WowpackSync::State::Failed) packH += smallRow;
        contentH += packH;
    }

    if (codeInMain) contentH += fieldRow + px(kRowGap);
    if (statusH > 0.0f) contentH += statusH + px(10);
    contentH += px(kButtonHeight) + px(kRowGap);
    contentH += smallRow + px(kRowGap);                      // the disclosure link
    contentH += advancedH;
    contentH += px(10) + smallRow;                           // the footer rule and row

    const float cardW = px(kCardWidth);
    const float cardH = px(kCardPadTop) + contentH + px(kCardPadBottom);
    const ImVec2 cardA((screenW - cardW) * 0.5f,
                       std::max(px(12), (screenH - cardH) * 0.48f));
    const ImVec2 cardB(cardA.x + cardW, cardA.y + cardH);

    ui_.sheet(cardA, cardB);
    ui_.claimMouse(cardA, cardB);

    Column col{cardA.x + px(kCardPadX), cardB.x - px(kCardPadX), cardA.y + px(kCardPadTop)};
    const float centreX = (cardA.x + cardB.x) * 0.5f;

    // ---- title ----------------------------------------------------------
    {
        const float titleSize = px(kTitleSize);
        const float top = col.y;
        ui_.textCentered(centreX, top, "WoWee", titleSize, theme.ink, /*titleFace=*/true);
        const float titleW = ui_.textWidth("WoWee", titleSize, true);
        // Below the title's own descenders, which is further down than the
        // size names: a size is an em and the face draws taller than one.
        const float underlineY = top + ui_.inkHeight(titleSize, true) * 1.02f;
        ui_.squiggle(ImVec2(centreX - titleW * 0.62f, underlineY),
                     ImVec2(centreX + titleW * 0.62f, underlineY),
                     theme.crayonRed, px(2.0f), 0x51A1u);
        // The height the card was measured with, so the two cannot drift.
        col.y = top + titleBlockH;
    }

    // ---- credentials ----------------------------------------------------
    const auto labelledField = [&](const char* label, const char* id, TextEdit& edit,
                                   const PaperUI::FieldOpts& opts) {
        ui_.text(col.at(), label, labelSize, theme.inkSoft);
        col.gap(labelRow);
        const auto [a, b] = col.row(px(kFieldHeight));
        const PaperUI::FieldResult r = ui_.field(id, a, b, edit, opts);
        col.gap(px(kRowGap));
        return r;
    };

    bool submit = false;

    // ---- which server ----------------------------------------------------
    //
    // First, and in the card rather than behind the disclosure: which server
    // you are logging into is the question that comes before who you are, and
    // somebody who has never done this cannot answer the two below it without
    // it. It used to sit three rows down behind "more options", so the screen
    // asked for an account on a server it never named.
    {
        std::vector<std::string> rows;
        rows.reserve(servers_.size() + 1);
        for (const ServerProfile& s : servers_) rows.push_back(serverRowLabel(s));
        rows.emplace_back("Somewhere else...");

        const bool known = selectedServerIndex_ >= 0 &&
                           selectedServerIndex_ < static_cast<int>(servers_.size());
        const std::string preview =
            known ? serverRowLabel(servers_[static_cast<size_t>(selectedServerIndex_)])
                  : makeServerKey(hostname_.text(), port);

        int choice = known ? selectedServerIndex_ : static_cast<int>(servers_.size());
        ui_.text(col.at(), "Server", labelSize, theme.inkSoft);
        col.gap(labelRow);
        const auto [a, b] = col.row(px(kFieldHeight));
        if (ui_.dropdown("servers", a, b, preview, rows, &choice)) {
            if (choice >= static_cast<int>(servers_.size())) {
                // "Somewhere else" is a request for the address box, which
                // lives behind the disclosure - so open it, or the choice
                // does nothing visible and there is nowhere to type.
                selectedServerIndex_ = -1;
                advancedOpen_ = true;
            } else {
                selectServerProfile(choice);
            }
        }
        col.gap(px(kRowGap));
    }

    {
        PaperUI::FieldOpts opts;
        opts.placeholder = "account name";
        const PaperUI::FieldResult r = labelledField("Account", "account", username_, opts);
        if (r.changed) statusProminent = false;
        if (r.submitted) ui_.focus("password");
    }

    {
        // The eye sits on the label row, right-aligned, so the box itself is
        // the same shape as the one above it.
        ui_.text(col.at(), "Password", labelSize, theme.inkSoft);
        const float eyeR = labelSize * 0.72f;
        if (ui_.glyphButton("reveal", ImVec2(col.x1 - eyeR, col.y + labelSize * 0.5f), eyeR,
                            showPassword ? PaperUI::Glyph::Eye : PaperUI::Glyph::EyeClosed)) {
            showPassword = !showPassword;
        }
        col.gap(labelRow);

        const bool hadPlaceholder = usingStoredHash && password_.text() == PASSWORD_PLACEHOLDER;
        const auto [a, b] = col.row(px(kFieldHeight));
        PaperUI::FieldOpts opts;
        opts.password = !showPassword;
        opts.placeholder = "password";
        const PaperUI::FieldResult r = ui_.field("password", a, b, password_, opts);
        col.gap(px(kRowGap));

        // A remembered password is a stored hash behind eight placeholder
        // bytes; there is nothing in the box worth editing. Taking the
        // keyboard therefore offers the whole of it up for replacement, so
        // the first keystroke starts a new password rather than appending to
        // a value that is not one.
        if (hadPlaceholder && r.focused && !passwordFocused_) password_.selectAll();
        passwordFocused_ = r.focused;

        if (r.changed) statusProminent = false;
        if (r.submitted) submit = true;
    }

    if (codeInMain) {
        ui_.text(col.at(), "Security code", labelSize, theme.inkSoft);
        ui_.textRight(col.x1, col.y + (labelSize - smallSize) * 0.5f,
                      authState == auth::AuthState::AUTHENTICATOR_REQUIRED ? "authenticator"
                                                                           : "PIN",
                      smallSize, theme.pencil);
        col.gap(labelRow);
        const auto [a, b] = col.row(px(kFieldHeight));
        PaperUI::FieldOpts opts;
        opts.password = true;
        opts.digitsOnly = true;
        opts.placeholder = "code";
        const PaperUI::FieldResult r = ui_.field("pin", a, b, pinCode_, opts);
        col.gap(px(kRowGap));
        if (r.submitted) submit = true;

        if (!securityPromptFocused_) {
            ui_.focus("pin");
            securityPromptFocused_ = true;
        }
    }

    // ---- which assets ----------------------------------------------------
    if (chooseAssets) {
        int choice = 0;
        for (int i = 0; i < static_cast<int>(assets.size()); ++i) {
            if (assets[static_cast<size_t>(i)].id == assetProfileId_) { choice = i; break; }
        }
        std::vector<std::string> rows;
        rows.reserve(assets.size());
        for (const AssetChoice& one : assets) rows.push_back(one.label);

        ui_.text(col.at(), "Assets", labelSize, theme.inkSoft);
        col.gap(labelRow);
        const auto [a, b] = col.row(px(kFieldHeight));
        if (ui_.dropdown("assets", a, b, rows[static_cast<size_t>(choice)], rows, &choice)) {
            assetProfileId_ = assets[static_cast<size_t>(choice)].id;
            core::Application::getInstance().setAssetExpansionOverride(assetProfileId_);
            core::Application::getInstance().reloadExpansionData();
        }
        col.gap(px(kRowGap));
        if (!assetProfileId_.empty()) {
            ui_.text(col.at(), "Cross-expansion DBC and model formats may differ.", smallSize,
                     theme.pencil);
            col.gap(smallRow + px(4));
        }
    }

    // ---- what is installed ------------------------------------------------
    if (troubleH > 0.0f) {
        ui_.wrapped(col.at(), contentW, trouble.c_str(), smallSize, theme.crayonRed);
        col.gap(troubleH);
    } else if (haveH > 0.0f) {
        ui_.wrapped(col.at(), contentW, haveLine.c_str(), smallSize, theme.pencil);
        col.gap(haveH);
    }

    // ---- content pack -----------------------------------------------------
    if (packH > 0.0f) {
        const bool failed = packSync.state() == core::WowpackSync::State::Failed;
        ui_.wrapped(col.at(), contentW, packLine.c_str(), smallSize,
                    failed ? theme.crayonRed : theme.pencil);
        col.gap(packH - (failed ? smallRow : 0.0f));
        if (failed) {
            const float retryW = ui_.textWidth("retry", smallSize);
            const float anywayW = ui_.textWidth("log in anyway", smallSize);
            const float gap = px(16);
            const float x = centreX - (retryW + gap + anywayW) * 0.5f;
            if (ui_.link("packretry", ImVec2(x, col.y), "retry", smallSize)) {
                packSkipped_ = false;
                startPackSync();
            }
            if (ui_.link("packskip", ImVec2(x + retryW + gap, col.y), "log in anyway", smallSize)) {
                packSkipped_ = true;
            }
            col.gap(smallRow);
        }
    }
    // A finished sync that changed files: what the asset manager cached
    // before is stale. Done here, on the thread that draws, never on the
    // worker.
    if (packSync.finished() && packSync.takeChanged()) {
        auto& app = core::Application::getInstance();
        app.reloadExpansionData();
        app.rescanAddons();
    }

    // ---- status ---------------------------------------------------------
    if (statusH > 0.0f) {
        ui_.wrapped(col.at(), contentW, statusMessage.c_str(), bodySize,
                    statusIsError ? theme.crayonRed : theme.crayonGreen);
        col.gap(statusH + px(10));
    }

    // ---- the one button --------------------------------------------------
    {
        const float w = px(200);
        const auto [a, b] = col.cell((contentW - w) * 0.5f, w, px(kButtonHeight));
        col.gap(px(kRowGap));

        char label[64];
        bool enabled = true;
        if (codeInMain) {
            std::snprintf(label, sizeof(label), "Submit Code");
        } else if (authenticating) {
            std::snprintf(label, sizeof(label), "Connecting  %.0fs", authTimer);
            enabled = false;
        } else if (packSync.running() && !packSkipped_) {
            std::snprintf(label, sizeof(label), "Syncing...");
            enabled = false;
        } else {
            std::snprintf(label, sizeof(label), "Log In");
        }
        if (ui_.button("login", a, b, label, PaperUI::ButtonKind::Primary, enabled)) submit = true;
    }

    // ---- the disclosure --------------------------------------------------
    {
        const char* label = advancedOpen_ ? "fewer options" : "more options";
        const float w = ui_.textWidth(label, smallSize);
        if (ui_.link("advanced", ImVec2(centreX - w * 0.5f, col.y), label, smallSize)) {
            advancedOpen_ = !advancedOpen_;
        }
        col.gap(smallRow + px(kRowGap));
    }

    // ---- server, expansion, assets ---------------------------------------
    if (advancedOpen_) {
        ui_.rule(ImVec2(col.x0, col.y), ImVec2(col.x1, col.y), paperFade(theme.pencil, 0.6f),
                 px(1.0f), 0x2C71u);
        col.gap(px(kRowGap) + px(2));

        auto& app = core::Application::getInstance();

        // The server list itself is in the card now, above the account it
        // belongs with. What stays here is the address behind it, for the
        // server that is not on the list.

        // Address and port, on one row, because they are one address.
        {
            const float portW = px(84);
            const float gap = px(12);
            const float hostW = contentW - portW - gap;
            ui_.text(col.at(), "Address", labelSize, theme.inkSoft);
            ui_.text(ImVec2(col.x0 + hostW + gap, col.y), "Port", labelSize, theme.inkSoft);
            col.gap(labelRow);

            const float y = col.y;
            PaperUI::FieldOpts hostOpts;
            hostOpts.placeholder = "logon.example.com";
            const PaperUI::FieldResult h = ui_.field(
                "hostname", ImVec2(col.x0, y), ImVec2(col.x0 + hostW, y + px(kFieldHeight)),
                hostname_, hostOpts);

            PaperUI::FieldOpts portOpts;
            portOpts.digitsOnly = true;
            portOpts.placeholder = "3724";
            const PaperUI::FieldResult p = ui_.field(
                "port", ImVec2(col.x1 - portW, y), ImVec2(col.x1, y + px(kFieldHeight)),
                portText_, portOpts);
            col.gap(px(kFieldHeight) + px(kRowGap));

            if (h.changed || p.changed) selectedServerIndex_ = -1;
            if (h.submitted || p.submitted) submit = true;
            // An empty box is the default port rather than an error: it is a
            // field halfway through being retyped, and there is only one port
            // anyone means by leaving it blank.
            port = portText_.empty()
                ? 3724
                : std::clamp(std::atoi(portText_.c_str()), 1, 65535);
        }

        // The content pack this server publishes. Empty for most servers.
        {
            ui_.text(col.at(), "Content pack URL", labelSize, theme.inkSoft);
            ui_.textRight(col.x1, col.y + (labelSize - smallSize) * 0.5f, "if your realm has one",
                          smallSize, theme.pencil);
            col.gap(labelRow);
            const auto [a, b] = col.row(px(kFieldHeight));
            PaperUI::FieldOpts opts;
            opts.placeholder = "https://realm.example/pack/wowpack.json";
            const PaperUI::FieldResult r = ui_.field("packurl", a, b, packUrl_, opts);
            col.gap(px(kRowGap));
            if (r.submitted) {
                packSkipped_ = false;
                startPackSync();
            }
        }

        // Expansion.
        if (haveExpansions) {
            const auto& profiles = registry->getAllProfiles();

            ui_.text(col.at(), "Expansion", labelSize, theme.inkSoft);
            col.gap(labelRow);
            std::vector<std::string> rows;
            rows.reserve(profiles.size());
            for (const auto& p : profiles)
                rows.push_back(p.shortName + "  (" + p.versionString() + ")");
            const std::string preview =
                (expansionIndex >= 0 && expansionIndex < static_cast<int>(profiles.size()))
                    ? rows[static_cast<size_t>(expansionIndex)]
                    : std::string("choose one");
            {
                const auto [a, b] = col.row(px(kFieldHeight));
                if (ui_.dropdown("expansion", a, b, preview, rows, &expansionIndex)) {
                    registry->setActive(profiles[static_cast<size_t>(expansionIndex)].id);
                    app.reloadExpansionData();
                }
                col.gap(px(kRowGap));
            }

        } else {
            ui_.text(col.at(), "Expansion: WotLK 3.3.5a (default)", smallSize, theme.pencil);
            col.gap(smallRow + px(kRowGap));
        }

        if (codeInAdvanced) {
            ui_.text(col.at(), "Security code", labelSize, theme.inkSoft);
            ui_.textRight(col.x1, col.y + (labelSize - smallSize) * 0.5f, "if your realm asks",
                          smallSize, theme.pencil);
            col.gap(labelRow);
            const auto [a, b] = col.row(px(kFieldHeight));
            PaperUI::FieldOpts opts;
            opts.password = true;
            opts.digitsOnly = true;
            opts.placeholder = "PIN or authenticator";
            if (ui_.field("pin", a, b, pinCode_, opts).submitted) submit = true;
            col.gap(px(kRowGap));
        }

#ifdef WOWEE_HAVE_ASSET_PANEL
        // The way back to the asset builder once there is already something
        // installed. It is the screen a first run opens on, and without a
        // door to it from here the only way to add a second game - or to
        // rebuild the one that is there - was to go and find the separate
        // program that draws the same panel.
        {
            // A link rather than a button, and nothing explaining it. With
            // this section open the card is already the tallest thing on the
            // screen - a button's worth of height pushed the footer off the
            // bottom edge - and the panel it opens says all of this at the
            // top of itself. Same shape as the disclosure above it.
            const char* label = "add or rebuild assets...";
            const float w = ui_.textWidth(label, smallSize);
            if (ui_.link("assets", ImVec2(centreX - w * 0.5f, col.y), label, smallSize)) {
                core::Application::getInstance().setState(core::AppState::FIRST_RUN);
            }
            col.gap(smallRow + px(kRowGap));
        }
#endif
    }

    // ---- footer ----------------------------------------------------------
    {
        col.gap(px(10));
        ui_.rule(ImVec2(col.x0, col.y - px(6)), ImVec2(col.x1, col.y - px(6)),
                 paperFade(theme.pencil, 0.5f), px(1.0f), 0x77A3u);

        std::string where = makeServerKey(hostname_.text(), port);
        if (auto* registry = core::Application::getInstance().getExpansionRegistry())
            if (const auto* active = registry->getActive())
                where += "  \xc2\xb7  " + active->shortName;
        ui_.text(col.at(), where.c_str(), smallSize, theme.pencil);

        const float r = smallSize * 0.86f;
        const float cy = col.y + smallRow * 0.5f - px(1);
        if (ui_.glyphButton("quit", ImVec2(col.x1 - r, cy), r, PaperUI::Glyph::Cross)) {
            if (auto* window = core::Application::getInstance().getWindow())
                window->setShouldClose(true);
        }
        if (ui_.glyphButton("settings", ImVec2(col.x1 - r * 3.4f, cy), r,
                            PaperUI::Glyph::Gear)) {
            settingsOpen_ = true;
            loginGfxLoaded_ = false;  // reload from disk each time it opens
            // The sheet lands under the pointer, and one of its controls is
            // wherever this gear is. Without this the press carries into it:
            // at 1280x760 the gear sits on the ground-clutter slider, which
            // takes the value it was dropped on and drags while held.
            ui_.swallowPress();
        }
    }

    if (submit && !authenticating) {
        attemptAuth(authHandler);
    } else if (submit && waitingForSecurityCode) {
        const std::string code = trimAscii(pinCode_.text());
        if (!code.empty()) {
            authHandler.submitSecurityCode(code);
            pinCode_.clear();
            pinAutoSubmitted_ = true;
        }
    }
}

// ---------------------------------------------------------------------------
// The stock login, over the glue scene
// ---------------------------------------------------------------------------

void AuthScreen::renderGlueLogin(auth::AuthHandler& authHandler, float screenW, float screenH) {
    const float u = glue::unit();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    // Glue positions are written from the bottom edge, as AccountLogin.xml's
    // anchors are: `fromBottom` is the frame's bottom edge in glue units.
    const auto bottomRect = [&](float cxOffset, float fromBottom, float w, float h) {
        const float x0 = screenW * 0.5f + (cxOffset - w * 0.5f) * u;
        const float y1 = screenH - fromBottom * u;
        return std::make_pair(ImVec2(x0, y1 - h * u), ImVec2(x0 + w * u, y1));
    };

    const auto authState = authHandler.getState();
    const bool waitingForSecurityCode = (authState == auth::AuthState::PIN_REQUIRED ||
                                         authState == auth::AuthState::AUTHENTICATOR_REQUIRED);
    auto& packSync = core::Application::getInstance().getWowpackSync();
    if (packSync.finished() && packSync.takeChanged()) {
        auto& app = core::Application::getInstance();
        app.reloadExpansionData();
        app.rescanAddons();
    }

    // Glues-BlizzardLogo, 100x100 at BOTTOM(0,8).
    {
        const auto [a, b] = bottomRect(0.0f, 8.0f, 100.0f, 100.0f);
        glue::image(dl, "Interface\\Glues\\Mainmenu\\Glues-BlizzardLogo", a, b);
    }
    // AccountLoginVersion: GlueFontNormalSmall at BOTTOMLEFT(0,10), and the
    // update note beside it when there is one.
    {
        const float size = glue::kFontSmall * u;
        const ImVec2 at(4.0f * u, screenH - 10.0f * u - size);
        glue::text(dl, at, core::kVersionString, size, glue::kNormal);
        const core::UpdateCheck& updates = core::Application::getInstance().getUpdateCheck();
        if (const std::string newer = updates.newerVersion(); !newer.empty()) {
            const std::string note = "  -  " + newer + " is available";
            const ImVec2 beside(at.x + glue::textWidth(core::kVersionString, size), at.y);
            const ImVec2 end(beside.x + glue::textWidth(note.c_str(), size), beside.y + size);
            const auto st = ui_.press("glue.update", beside, end);
            glue::text(dl, beside, note.c_str(), size,
                       st.hovered ? glue::kHighlight : IM_COL32(246, 217, 107, 255));
            if (st.clicked) core::openExternalUrl(updates.releaseUrl());
        }
    }

    // The server sheet: the paper card, over a scrim, with its own way back.
    if (serverSheetOpen_) {
        ui_.setLayer(PaperLayer::Overlay);
        ui_.scrim(0.55f);
        renderCard(authHandler, screenW, screenH);
        ui_.setLayer(PaperLayer::Page);
        const auto [a, b] = bottomRect(0.0f, 20.0f, 150.0f, 38.0f);
        if (glue::button(ui_, ImGui::GetForegroundDrawList(), "glue.serverdone", a, b, "Done",
                         glue::ButtonStyle::Blue, true, glue::kFontSmall)) {
            serverSheetOpen_ = false;
        }
        return;
    }

    bool submit = false;
    const bool busy = authenticating && !waitingForSecurityCode;
    // While a dialog is up the form behind it is drawn but deaf, as GlueDialog
    // is modal over AccountLogin.
    const bool errorUp = !busy && (statusIsError || statusProminent) &&
                         !statusMessage.empty() && statusMessage != glueDismissedStatus_;
    const bool modal = busy || errorUp;
    glueModalUp_ = modal;
    if (modal) ui_.pushInert();

    const auto editBox = [&](const char* id, const char* label, float fromBottom, TextEdit& edit,
                             const PaperUI::FieldOpts& opts) {
        // 200x37 with the tooltip backdrop; the text sits inside TextInsets
        // left 12, right 5, bottom 5. The label is GlueFontNormal, centred
        // nine units above the box.
        const auto [a, b] = bottomRect(0.0f, fromBottom, 200.0f, 37.0f);
        glue::tooltipBackdrop(dl, a, b);
        const float labelSize = glue::kFontNormal * u;
        glue::textCentered(dl, (a.x + b.x) * 0.5f, a.y - 9.0f * u - labelSize * 0.5f, label,
                           labelSize, glue::kNormal);
        const ImVec2 in0(a.x + 12.0f * u, a.y + 2.0f * u);
        const ImVec2 in1(b.x - 5.0f * u, b.y - 5.0f * u);
        return ui_.field(id, in0, in1, edit, opts);
    };

    {
        PaperUI::FieldOpts opts;
        opts.glue = true;
        const PaperUI::FieldResult r = editBox("account", "Account Name", 345.0f, username_, opts);
        if (r.changed) { statusProminent = false; glueDismissedStatus_.clear(); }
        if (r.submitted) ui_.focus("password");
    }
    {
        const bool hadPlaceholder = usingStoredHash && password_.text() == PASSWORD_PLACEHOLDER;
        PaperUI::FieldOpts opts;
        opts.glue = true;
        opts.password = true;
        const PaperUI::FieldResult r = editBox("password", "Password", 275.0f, password_, opts);
        if (hadPlaceholder && r.focused && !passwordFocused_) password_.selectAll();
        passwordFocused_ = r.focused;
        if (r.changed) { statusProminent = false; glueDismissedStatus_.clear(); }
        if (r.submitted) submit = true;
    }
    // AccountLoginTokenEdit: shown when the realm asks for a code, under the
    // password box.
    if (waitingForSecurityCode) {
        PaperUI::FieldOpts opts;
        opts.glue = true;
        opts.password = true;
        opts.digitsOnly = true;
        const char* label = authState == auth::AuthState::AUTHENTICATOR_REQUIRED
                                ? "Authenticator Code" : "PIN";
        const PaperUI::FieldResult r = editBox("pin", label, 275.0f - 37.0f - 15.0f - 37.0f,
                                               pinCode_, opts);
        if (r.submitted) submit = true;
        if (!securityPromptFocused_) {
            ui_.focus("pin");
            securityPromptFocused_ = true;
        }
    }

    // AccountLoginLoginButton: GlueButtonTemplateBlue, 220x45 at BOTTOM(0,170).
    {
        const auto [a, b] = bottomRect(0.0f, waitingForSecurityCode ? 120.0f : 170.0f, 220.0f, 45.0f);
        const bool syncing = packSync.running() && !packSkipped_;
        const char* label = waitingForSecurityCode ? "Submit Code" : (syncing ? "Syncing..." : "Login");
        if (glue::button(ui_, dl, "glue.login", a, b, label, glue::ButtonStyle::Blue, !syncing)) {
            submit = true;
        }

        // Anything the player has to know before logging in goes under it,
        // where SAVE_ACCOUNT_NAME sits in the stock layout.
        float y = b.y + 6.0f * u;
        const float small = glue::kFontSmall * u;
        const float cx = (a.x + b.x) * 0.5f;
        const pipeline::AssetInventory& inventory =
            core::Application::getInstance().getAssetInventory();
        if (const std::string trouble = inventory.troubleText(); !trouble.empty()) {
            glue::textCentered(dl, cx, y, trouble.c_str(), small, glue::kRed);
            y += small * 1.4f;
        }
        if (const std::string packLine = packStatusLine(packSync); !packLine.empty()) {
            const bool failed = packSync.state() == core::WowpackSync::State::Failed;
            glue::textCentered(dl, cx, y, packLine.c_str(), small,
                               failed ? glue::kRed : glue::kNormal);
            y += small * 1.4f;
            if (failed) {
                const float bw = 120.0f * u, bh = 32.0f * u, gap = 6.0f * u;
                if (glue::button(ui_, dl, "glue.packretry", ImVec2(cx - bw - gap * 0.5f, y),
                                 ImVec2(cx - gap * 0.5f, y + bh), "Retry",
                                 glue::ButtonStyle::Blue, true, glue::kFontSmall)) {
                    packSkipped_ = false;
                    startPackSync();
                }
                if (glue::button(ui_, dl, "glue.packskip", ImVec2(cx + gap * 0.5f, y),
                                 ImVec2(cx + bw + gap * 0.5f, y + bh), "Log In Anyway",
                                 glue::ButtonStyle::Blue, true, glue::kFontSmall)) {
                    packSkipped_ = true;
                }
            }
        }
    }

    // The right-hand column, GlueButtonSmallTemplateBlue (150x38): Quit at
    // BOTTOMRIGHT(-5,29), the realm list's name under it, and Options where
    // the stock column starts above the gap.
    {
        const float w = 150.0f * u, h = 38.0f * u;
        const float x1 = screenW - 5.0f * u;
        const float quitY1 = screenH - 29.0f * u;
        if (glue::button(ui_, dl, "glue.quit", ImVec2(x1 - w, quitY1 - h), ImVec2(x1, quitY1),
                         "Quit", glue::ButtonStyle::Blue, true, glue::kFontSmall)) {
            if (auto* window = core::Application::getInstance().getWindow())
                window->setShouldClose(true);
        }
        // AccountLoginRealmName: GlueFontDisableSmall, right-justified under Quit.
        std::string where = makeServerKey(hostname_.text(), port);
        if (auto* registry = core::Application::getInstance().getExpansionRegistry())
            if (const auto* active = registry->getActive()) where += "  " + active->shortName;
        glue::textRight(dl, x1 - 8.0f * u, quitY1 + 4.0f * u, where.c_str(),
                        glue::kFontSmall * u, glue::kDisable);

        const float optY1 = quitY1 - h - 80.0f * u;
        if (glue::button(ui_, dl, "glue.options", ImVec2(x1 - w, optY1 - h), ImVec2(x1, optY1),
                         "Options", glue::ButtonStyle::Blue, true, glue::kFontSmall)) {
            settingsOpen_ = true;
            loginGfxLoaded_ = false;
            ui_.swallowPress();
        }
    }

    // The left-hand column, where Manage Account and Community Site sit at
    // BOTTOMLEFT(10,135): this client's server settings instead.
    {
        const float w = 150.0f * u, h = 38.0f * u;
        const float x0 = 10.0f * u;
        float y1 = screenH - 135.0f * u;
#ifdef WOWEE_HAVE_ASSET_PANEL
        if (glue::button(ui_, dl, "glue.assets", ImVec2(x0, y1 - h), ImVec2(x0 + w, y1),
                         "Game Assets", glue::ButtonStyle::Blue, true, glue::kFontSmall)) {
            core::Application::getInstance().setState(core::AppState::FIRST_RUN);
        }
        y1 -= h;
#endif
        if (glue::button(ui_, dl, "glue.server", ImVec2(x0, y1 - h), ImVec2(x0 + w, y1),
                         "Change Server", glue::ButtonStyle::Blue, true, glue::kFontSmall)) {
            serverSheetOpen_ = true;
            ui_.swallowPress();
        }
    }

    if (modal) ui_.popInert();

    // GlueDialog: "Connecting..." with Cancel while the attempt runs, the
    // failure with Okay once it has failed.
    if (modal) {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const float w = 512.0f * u * 0.75f;
        const float body = glue::kFontNormal * u;
        const std::string message =
            busy ? (statusMessage.empty() ? std::string("Connecting") : statusMessage)
                 : statusMessage;
        // Wrapped by hand to the dialog's width, so a long reason stays inside.
        std::vector<std::string> lines;
        {
            std::string line, word;
            const float maxW = w - 60.0f * u;
            const auto flushWord = [&]() {
                if (word.empty()) return;
                const std::string trial = line.empty() ? word : line + " " + word;
                if (!line.empty() && glue::textWidth(trial.c_str(), body) > maxW) {
                    lines.push_back(line);
                    line = word;
                } else {
                    line = trial;
                }
                word.clear();
            };
            for (char c : message) {
                if (c == ' ') flushWord(); else word.push_back(c);
            }
            flushWord();
            if (!line.empty()) lines.push_back(line);
        }
        const float lineH = body * 1.3f;
        const float h = 26.0f * u + lineH * static_cast<float>(lines.size()) + 20.0f * u +
                        40.0f * u + 16.0f * u;
        const ImVec2 a((screenW - w) * 0.5f, (screenH - h) * 0.5f);
        const ImVec2 b(a.x + w, a.y + h);
        fg->AddRectFilled(ImVec2(0, 0), ImVec2(screenW, screenH), IM_COL32(0, 0, 0, 90));
        glue::dialogBackdrop(fg, a, b);
        ui_.claimMouse(ImVec2(0, 0), ImVec2(screenW, screenH));
        float y = a.y + 26.0f * u;
        for (const std::string& line : lines) {
            glue::textCentered(fg, (a.x + b.x) * 0.5f, y, line.c_str(), body, glue::kHighlight);
            y += lineH;
        }
        // GlueDialogButtonTemplate: 220x40, sixteen units off the bottom.
        const float bw = 220.0f * u * 0.75f, bh = 40.0f * u;
        const ImVec2 ba((a.x + b.x - bw) * 0.5f, b.y - 16.0f * u - bh);
        const ImVec2 bb(ba.x + bw, ba.y + bh);
        if (glue::button(ui_, fg, "glue.dialog", ba, bb, busy ? "Cancel" : "Okay",
                         glue::ButtonStyle::Red, true, glue::kFontNormal) ||
            (!busy && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                       ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                       ImGui::IsKeyPressed(ImGuiKey_Escape, false)))) {
            if (busy) {
                authHandler.disconnect();
                authenticating = false;
                setStatus("", false);
            } else {
                glueDismissedStatus_ = statusMessage;
                statusProminent = false;
            }
        }
        // The key that closed the dialog is not also a login.
        submit = false;
    }

    if (submit && !authenticating) {
        glueDismissedStatus_.clear();
        attemptAuth(authHandler);
    } else if (submit && waitingForSecurityCode) {
        const std::string code = trimAscii(pinCode_.text());
        if (!code.empty()) {
            authHandler.submitSecurityCode(code);
            pinCode_.clear();
            pinAutoSubmitted_ = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Background art and login music
// ---------------------------------------------------------------------------

// The art is drawn straight into ImGui's background list rather than through
// PaperUI: it is behind everything by definition, and the realm and character
// screens ask for it before they have begun a frame of their own.

bool AuthScreen::drawGlueScene() {
    auto& app = core::Application::getInstance();
    auto* assets = app.getAssetManager();
    auto* renderer = app.getRenderer();
    if (!assets || !assets->isInitialized() || !renderer) return false;
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    if (screen.x < 2.0f || screen.y < 2.0f) return false;
    if (!glueSceneTried_) {
        glueSceneTried_ = true;
        auto scene = std::make_unique<rendering::CharacterPreview>();
        if (scene->initialize(assets, static_cast<int>(screen.x), static_cast<int>(screen.y))) {
            renderer->registerPreview(scene.get());
            if (scene->loadGlueScene("Interface\\Glues\\Models\\UI_MainMenu_Northrend\\UI_MainMenu_Northrend.m2")) {
                glueScene_ = std::move(scene);
            }
        }
        if (!glueScene_) LOG_WARNING("AuthScreen: no login glue scene; using the flat backdrop");
    }
    if (!glueScene_) return false;
    glueScene_->update(ImGui::GetIO().DeltaTime);
    glueScene_->render();
    glueScene_->requestComposite();
    auto* bg = ImGui::GetBackgroundDrawList();
    if (VkDescriptorSet tex = glueScene_->getTextureId()) {
        bg->AddImage(reinterpret_cast<ImTextureID>(tex), ImVec2(0, 0), screen);
    } else {
        bg->AddRectFilled(ImVec2(0, 0), screen, IM_COL32(0, 0, 0, 255));
    }
    // The logo, 256x128 at (3,-7) from the top left in 768-tall glue units.
    if (!glueLogoTried_) {
        glueLogoTried_ = true;
        glueLogo_ = uploadUiTextureFromBlp(assets, "Interface\\Glues\\Common\\Glues-WoW-WotLKLogo.blp", app.getWindow());
    }
    if (glueLogo_) {
        const float u = screen.y / 768.0f;
        bg->AddImage(reinterpret_cast<ImTextureID>(glueLogo_), ImVec2(3.0f * u, 7.0f * u),
                     ImVec2((3.0f + 256.0f) * u, (7.0f + 128.0f) * u));
    }
    return true;
}

void AuthScreen::releaseGlueScene() {
    glueScene_.reset();
    glueSceneTried_ = false;
}

void AuthScreen::drawBackdrop() {
    if (drawGlueScene()) return;
    // Kick the decode off once, then upload on whichever frame it lands.
    if (!bgDecodeStarted) {
        bgDecodeStarted = true;
        std::string imgPath = "assets/krayonsignin.png";
        if (!std::filesystem::exists(imgPath))
            imgPath = (std::filesystem::current_path() / imgPath).string();
        bgDecodeFuture = std::async(std::launch::async, [imgPath]() {
            DecodedBackground out;
            int channels = 0;
            stbi_set_flip_vertically_on_load(false);
            unsigned char* data = stbi_load(imgPath.c_str(), &out.width, &out.height, &channels, 4);
            if (data) {
                out.pixels.assign(data, data + static_cast<size_t>(out.width) * out.height * 4);
                stbi_image_free(data);
            }
            return out;
        });
    }
    if (!bgInitAttempted && bgDecodeFuture.valid() &&
        bgDecodeFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        bgInitAttempted = true;
        DecodedBackground decoded = bgDecodeFuture.get();
        if (!decoded.pixels.empty()) {
            bgWidth = decoded.width;
            bgHeight = decoded.height;
            uploadBackgroundImage(decoded.pixels.data());
        } else {
            LOG_WARNING("Auth screen: failed to decode background image");
        }
    }
    if (!bgDescriptorSet) return;

    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float imgW = static_cast<float>(bgWidth);
    const float imgH = static_cast<float>(bgHeight);
    if (imgW <= 0.0f || imgH <= 0.0f) return;

    const float screenAspect = screen.x / screen.y;
    const float imgAspect = imgW / imgH;
    ImVec2 uv0(0.0f, 0.0f);
    ImVec2 uv1(1.0f, 1.0f);
    if (imgAspect > screenAspect) {
        const float crop = (1.0f - screenAspect / imgAspect) * 0.5f;
        uv0.x = crop;
        uv1.x = 1.0f - crop;
    } else if (imgAspect < screenAspect) {
        const float crop = (1.0f - imgAspect / screenAspect) * 0.5f;
        uv0.y = crop;
        uv1.y = 1.0f - crop;
    }
    ImGui::GetBackgroundDrawList()->AddImage(reinterpret_cast<ImTextureID>(bgDescriptorSet),
                                             ImVec2(0, 0), ImVec2(screen.x, screen.y), uv0, uv1);
}

void AuthScreen::updateMusic() {
    auto& app = core::Application::getInstance();
    auto* ac = app.getAudioCoordinator();
    if (!musicInitAttempted) {
        musicInitAttempted = true;
        auto* assets = app.getAssetManager();
        if (ac) {
            auto* music = ac->getMusicManager();
            if (music && assets && assets->isInitialized() && !music->isInitialized()) {
                music->initialize(assets);
            }
        }
    }
    if (!ac) return;
    auto* music = ac->getMusicManager();
    if (!music) return;

    if (!loginMusicVolumeAdjusted_) {
        savedMusicVolume_ = music->getVolume();
        // Reduce music to 80% during login so UI button clicks and error sounds
        // remain audible over the background track
        int loginVolume = (savedMusicVolume_ * 80) / 100;
        loginVolume = std::clamp(loginVolume, 0, 100);
        music->setVolume(loginVolume);
        loginMusicVolumeAdjusted_ = true;
    }
    music->update(ImGui::GetIO().DeltaTime);
    if (music->isPlaying() || music->isLoading()) return;

    if (!introTracksScanned_) {
        introTracksScanned_ = true;
        // The client's own login track, only when the WoWee soundtrack is on
        // and the file shipped (release archives leave it out). Otherwise the
        // Wrath glue theme, which is what a 3.3.5 login screen plays.
        if (core::readSettingsFlag("use_original_soundtrack", false)) {
            const std::filesystem::path relative =
                std::filesystem::path("assets") / "Original Music" / "TavernAllianceREMIX.mp3";
            if (std::filesystem::exists(relative)) {
                loginTrackPath_ = relative.string();
            } else {
                const auto absolute = std::filesystem::current_path() / relative;
                if (std::filesystem::exists(absolute)) {
                    loginTrackPath_ = absolute.string();
                }
            }
        }
    }
    if (!loginTrackPath_.empty()) {
        music->playFilePath(loginTrackPath_, true, 1800.0f);
        musicPlaying = music->isPlaying() || music->isLoading();
        if (musicPlaying) {
            LOG_INFO("AuthScreen: Playing login intro track: ", loginTrackPath_);
        } else {
            loginTrackPath_.clear();
        }
    } else {
        static const char* const kGlueThemes[] = {
            "Sound\\Music\\GlueScreenMusic\\wotlk_main_title.mp3",
            "Sound\\Music\\GlueScreenMusic\\wow_main_theme.mp3",
        };
        auto* assets = app.getAssetManager();
        for (const char* theme : kGlueThemes) {
            if (assets && !assets->fileExists(theme)) continue;
            music->setTrackGain(1.0f);
            music->playMusic(theme, true, 1800.0f);
            musicPlaying = music->isPlaying() || music->isLoading();
            if (musicPlaying) break;
        }
        if (!musicPlaying && !missingIntroTracksLogged_) {
            LOG_WARNING("AuthScreen: no login music: neither the WoWee track nor the glue theme is available");
            missingIntroTracksLogged_ = true;
        }
    }
}

void AuthScreen::stopLoginMusic() {
    auto& app = core::Application::getInstance();
    auto* ac = app.getAudioCoordinator();
    if (!ac) return;
    auto* music = ac->getMusicManager();
    if (!music) return;
    if (musicPlaying) {
        music->stopMusic(500.0f);
        musicPlaying = false;
    }
    if (loginMusicVolumeAdjusted_) {
        music->setVolume(savedMusicVolume_);
        loginMusicVolumeAdjusted_ = false;
    }
}

void AuthScreen::attemptAuth(auth::AuthHandler& authHandler) {
    // A new attempt: whatever it says, the glue dialog shows it.
    glueDismissedStatus_.clear();
    {
        auto& packSync = core::Application::getInstance().getWowpackSync();
        if (packSync.running() && !packSkipped_) {
            setStatus("The content pack is still syncing", true);
            return;
        }
    }
    // Validate inputs
    if (username_.empty()) {
        setStatus("Enter an account name", true);
        return;
    }

    // Check if using stored hash (password field contains placeholder)
    const bool useHash = usingStoredHash && password_.text() == PASSWORD_PLACEHOLDER;

    if (!useHash && password_.empty()) {
        setStatus("Enter a password", true);
        return;
    }

    if (hostname_.empty()) {
        setStatus("Enter an address to connect to", true);
        return;
    }

    // Build the auth-protocol candidate chain for this attempt. The profile's
    // own value is always tried first; vanilla-family profiles get the other
    // vanilla protocol byte appended as a fallback, because vanilla-family
    // servers disagree on it (Turtle/vmangos-derived: 8, older mangos/cmangos:
    // 3) and the profile can only name one. TBC/WotLK have no such ambiguity.
    authProtocols_.clear();
    authProtocolAttempt_ = 0;
    {
        uint8_t primary = 8;
        bool vanillaFamily = false;
        auto* reg = core::Application::getInstance().getExpansionRegistry();
        if (reg) {
            if (auto* profile = reg->getActive()) {
                primary = profile->protocolVersion;
                vanillaFamily = (profile->id == "classic" || profile->id == "turtle");
            }
        }
        authProtocols_.push_back(primary);
        if (vanillaFamily) {
            const uint8_t alternate = (primary == 3) ? uint8_t{8} : uint8_t{3};
            if (alternate != primary) authProtocols_.push_back(alternate);
        }
    }

    beginAuthAttempt(authHandler);
}

void AuthScreen::beginAuthAttempt(auth::AuthHandler& authHandler) {
    const bool useHash = usingStoredHash && password_.text() == PASSWORD_PLACEHOLDER;
    const uint8_t protocolVersion = (authProtocolAttempt_ < authProtocols_.size())
        ? authProtocols_[authProtocolAttempt_] : uint8_t{8};
    const bool isRetry = (authProtocolAttempt_ > 0);

    std::stringstream ss;
    if (isRetry) {
        ss << "Retrying " << hostname_.text() << ":" << port
           << " with auth protocol " << static_cast<int>(protocolVersion) << "...";
    } else {
        ss << "Connecting to " << hostname_.text() << ":" << port << "...";
    }
    setStatus(ss.str(), false);

    // Wire up failure callback to capture specific error reason
    failureReason.clear();
    authHandler.setOnFailure([this](const std::string& reason) {
        failureReason = reason;
    });

    // Configure client version from active expansion profile
    auto* reg = core::Application::getInstance().getExpansionRegistry();
    if (reg) {
        auto* profile = reg->getActive();
        if (profile) {
            auth::ClientInfo info;
            info.majorVersion = profile->majorVersion;
            info.minorVersion = profile->minorVersion;
            info.patchVersion = profile->patchVersion;
            info.build = profile->build;
            info.protocolVersion = protocolVersion;
            info.game = profile->game;
            info.platform = profile->platform;
            info.os = profile->os;
            info.locale = profile->locale;
            info.timezone = profile->timezone;
            // Vanilla-family servers send the legacy realm-list layout even
            // when the auth handshake itself uses protocol v8 (vmangos), so key
            // this on the expansion rather than on the protocol byte in play.
            info.legacyVanillaRealmList = (profile->id == "classic" ||
                                           profile->id == "turtle" ||
                                           profile->protocolVersion <= 3);
            authHandler.setClientInfo(info);
        }
    }

    if (authHandler.connect(hostname_.text(), static_cast<uint16_t>(port))) {
        authenticating = true;
        authTimer = 0.0f;
        setStatus(isRetry ? "Reconnected, authenticating..." : "Connected, authenticating...", false);
        pinAutoSubmitted_ = false;
        securityPromptFocused_ = false;

        // Save login info for next session
        saveLoginInfo(false);

        const std::string pinStr = trimAscii(pinCode_.text());

        // Send authentication credentials
        if (useHash) {
            auto hashBytes = hexDecode(savedPasswordHash);
            authHandler.authenticateWithHash(username_.text(), hashBytes, pinStr);
        } else {
            usingStoredHash = false;
            authHandler.authenticate(username_.text(), password_.text(), pinStr);
        }

        // Don't keep the code around longer than needed.
        pinCode_.clear();
    } else {
        std::stringstream errSs;
        errSs << "Failed to connect to " << hostname_.text() << ":" << port
              << " - check that the server is online and the address is correct";
        setStatus(errSs.str(), true);
        authenticating = false;
    }
}

void AuthScreen::setStatus(const std::string& message, bool isError, bool prominent) {
    statusMessage = message;
    statusIsError = isError;
    statusProminent = prominent;
}

std::string AuthScreen::getConfigPath() {
    return core::getConfigRoot() + "/login.cfg";
}

void AuthScreen::saveLoginInfo(bool includePasswordHash) {
    upsertCurrentServerProfile(includePasswordHash);

    std::string path = getConfigPath();
    std::filesystem::path dir = std::filesystem::path(path).parent_path();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::ofstream out(path);
    if (!out.is_open()) {
        LOG_WARNING("Could not save login info to ", path);
        return;
    }

    out << "version=3\n";
    out << "active=" << makeServerKey(hostname_.text(), port) << "\n";

    for (const auto& s : servers_) {
        out << "\n[server " << makeServerKey(s.hostname, s.port) << "]\n";
        out << "username=" << s.username << "\n";
        if (!s.passwordHash.empty()) {
            out << "password_hash=" << s.passwordHash << "\n";
        }
        if (!s.expansionId.empty()) {
            out << "expansion=" << s.expansionId << "\n";
        }
        if (!s.assetProfileId.empty()) {
            out << "assets=" << s.assetProfileId << "\n";
        }
        if (!s.packUrl.empty()) {
            out << "pack_url=" << s.packUrl << "\n";
        }
    }

    LOG_INFO("Login info saved to ", path);
}

void AuthScreen::loadLoginInfo() {
    std::string path = getConfigPath();
    std::ifstream in(path);
    if (!in.is_open()) return;

    std::string file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // If this looks like the old flat format, migrate it into a single server entry.
    if (file.find("[server ") == std::string::npos) {
        std::unordered_map<std::string, std::string> kv;
        std::istringstream ss(file);
        std::string line;
        while (std::getline(ss, line)) {
            line = trimAscii(line);
            if (line.empty() || line[0] == '#') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            kv[trimAscii(line.substr(0, eq))] = trimAscii(line.substr(eq + 1));
        }

        std::string host = kv["hostname"];
        int p = 3724;
        try { if (!kv["port"].empty()) p = std::stoi(kv["port"]); } catch (...) {}
        if (!host.empty()) {
            ServerProfile s;
            s.hostname = host;
            s.port = p;
            s.username = kv["username"];
            s.passwordHash = kv["password_hash"];
            s.expansionId = kv["expansion"];
            s.assetProfileId = kv["assets"];
            servers_.push_back(std::move(s));
            selectServerProfile(0);
        }

        LOG_INFO("Login info loaded from ", path, " (migrated legacy profile -> v3)");
        return;
    }

    servers_.clear();
    selectedServerIndex_ = -1;

    std::string activeKey;
    ServerProfile current;
    bool inServer = false;

    auto flushServer = [&]() {
        if (!inServer) return;
        if (!current.hostname.empty() && current.port > 0) {
            servers_.push_back(current);
        }
        current = ServerProfile{};
        inServer = false;
    };

    std::istringstream ss(file);
    std::string line;
    while (std::getline(ss, line)) {
        line = trimAscii(line);
        if (line.empty() || line[0] == '#') continue;

        if (line.front() == '[' && line.back() == ']') {
            flushServer();
            std::string inside = line.substr(1, line.size() - 2);
            inside = trimAscii(inside);
            const std::string prefix = "server ";
            if (inside.rfind(prefix, 0) == 0) {
                std::string key = trimAscii(inside.substr(prefix.size()));
                // Parse host:port (split on last ':', allow [ipv6]:port).
                std::string hostPart = key;
                int portPart = 3724;
                if (!key.empty() && key.front() == '[') {
                    auto rb = key.find(']');
                    if (rb != std::string::npos) {
                        hostPart = key.substr(1, rb - 1);
                        auto colon = key.find(':', rb);
                        if (colon != std::string::npos) {
                            try { portPart = std::stoi(key.substr(colon + 1)); } catch (...) {}
                        }
                    }
                } else {
                    auto colon = key.rfind(':');
                    if (colon != std::string::npos) {
                        hostPart = key.substr(0, colon);
                        try { portPart = std::stoi(key.substr(colon + 1)); } catch (...) {}
                    }
                }

                current.hostname = hostPart;
                current.port = portPart;
                inServer = true;
            }
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trimAscii(line.substr(0, eq));
        std::string val = trimAscii(line.substr(eq + 1));

        if (!inServer) {
            if (key == "active") activeKey = val;
            continue;
        }

        if (key == "username") current.username = val;
        else if (key == "password_hash") current.passwordHash = val;
        else if (key == "expansion") current.expansionId = val;
        else if (key == "assets") current.assetProfileId = val;
        else if (key == "pack_url") current.packUrl = val;
    }
    flushServer();

    if (!servers_.empty()) {
        std::sort(servers_.begin(), servers_.end(),
                  [](const ServerProfile& a, const ServerProfile& b) {
                      if (a.hostname != b.hostname) return a.hostname < b.hostname;
                      return a.port < b.port;
                  });

        if (!activeKey.empty()) {
            for (int i = 0; i < static_cast<int>(servers_.size()); ++i) {
                if (makeServerKey(servers_[i].hostname, servers_[i].port) == activeKey) {
                    selectServerProfile(i);
                    break;
                }
            }
        }

        if (selectedServerIndex_ < 0) {
            selectServerProfile(0);
        }
    }

    LOG_INFO("Login info loaded from ", path);
}

static uint32_t findMemType(VkPhysicalDevice pd, uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((filter & (1 << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    LOG_ERROR("AuthScreen: no suitable memory type found");
    return UINT32_MAX;
}

// Takes pixels already decoded on a worker thread and does the GPU-side work,
// which has to happen on the main thread.
bool AuthScreen::uploadBackgroundImage(const unsigned char* data) {
    auto& app = core::Application::getInstance();
    auto* renderer = app.getRenderer();
    if (!renderer) return false;
    bgVkCtx = renderer->getVkContext();
    if (!bgVkCtx) return false;
    if (!data) return false;

    VkDevice device = bgVkCtx->getDevice();
    VkPhysicalDevice physDevice = bgVkCtx->getPhysicalDevice();
    VkDeviceSize imageSize = static_cast<VkDeviceSize>(bgWidth) * bgHeight * 4;

    // Staging buffer
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
        allocInfo.memoryTypeIndex = findMemType(physDevice, memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory);
        vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

        void* mapped;
        vkMapMemory(device, stagingMemory, 0, imageSize, 0, &mapped);
        memcpy(mapped, data, imageSize);
        vkUnmapMemory(device, stagingMemory);
    }
    // The pixels belong to the caller's decoded buffer, not to stb_image.

    // Create VkImage
    {
        VkImageCreateInfo imgInfo{};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imgInfo.extent = {.width = static_cast<uint32_t>(bgWidth), .height = static_cast<uint32_t>(bgHeight), .depth = 1};
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
        allocInfo.memoryTypeIndex = findMemType(physDevice, memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(device, &allocInfo, nullptr, &bgMemory);
        vkBindImageMemory(device, bgImage, bgMemory, 0);
    }

    // Transfer
    bgVkCtx->immediateSubmit([&](VkCommandBuffer cmd) {
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
        rendering::cmdPipelineBarrier2(cmd, barrierDep);

        VkBufferImageCopy region{};
        region.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
        region.imageExtent = {.width = static_cast<uint32_t>(bgWidth), .height = static_cast<uint32_t>(bgHeight), .depth = 1};
        vkCmdCopyBufferToImage(cmd, stagingBuffer, bgImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkDependencyInfo toReadDep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        toReadDep.imageMemoryBarrierCount = 1;
        toReadDep.pImageMemoryBarriers = &barrier;
        rendering::cmdPipelineBarrier2(cmd, toReadDep);
    });

    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);

    // Image view
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = bgImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        vkCreateImageView(device, &viewInfo, nullptr, &bgImageView);
    }

    // Sampler
    {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        bgSampler = bgVkCtx->getOrCreateSampler(samplerInfo);
    }

    bgDescriptorSet = ImGui_ImplVulkan_AddTexture(bgSampler, bgImageView,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    LOG_INFO("Auth screen background loaded: ", bgWidth, "x", bgHeight);
    return true;
}
// ---------------------------------------------------------------------------
// Login-screen graphics settings popup
// ---------------------------------------------------------------------------

void AuthScreen::applyPresetToState(LoginGraphicsState& s, int preset) {
    // The numbers come from the one table both screens read. They were written
    // out here as well until they disagreed with it in four of ten columns, so
    // that choosing High here and High in game gave two different pictures.
    //
    // Custom is preset 0 and is not a set of values, so it is left alone.
    const int index = preset - 1;
    if (index < 0 || index >= kGraphicsPresetCount) return;
    const GraphicsPresetValues& p = kGraphicsPresets[index];

    s.viewDistance   = p.viewDistance;
    s.shadows        = p.shadows;
    s.shadowDistance = p.shadowDistance;
    s.antiAliasing   = p.antiAliasing;
    s.fxaa           = p.fxaa;
    s.normalMapping  = p.normalMapping;
    s.pom            = p.parallax;
    s.pomQuality     = p.parallaxQuality;
    s.groundClutter  = p.groundClutter;
    s.grass          = p.grass;
    s.grassDensity   = p.grassDensity;
    s.grassHeight    = p.grassHeight;
    s.grassDistance  = p.grassDistance;

    // Not in the table because the in-game preset has no opinion about them
    // either: upscaling and water refraction are the player's, and brightness,
    // vsync and fullscreen are not quality settings. A preset that reset them
    // would undo a display choice every time one was picked.
}

void AuthScreen::loadLoginGraphicsState() {
    std::ifstream file(SettingsPanel::getSettingsPath());
    if (!file.is_open()) {
        // File doesn't exist yet - keep struct defaults (Medium equivalent)
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        // Clamped to the same ranges GameScreen::loadSettings uses.
        //
        // Four of these had no clamp at all until 2026-08-15 - the preset
        // index, the parallax quality, the upscaling mode and the brightness -
        // so a value the file held outside its range was kept here, shown
        // against a control that could not represent it, and written back on
        // Apply. The shadow distance had one, but it started at 50 where the
        // schema and the game both start at 40, so a saved 40 came back as 50.
        //
        // Without this a value the file holds outside the range is kept, shown
        // against a slider that cannot represent it, and written straight back
        // on Apply - while the game clamps the same value on the way in. A
        // config carrying view_distance=0 showed the slider pinned at the far
        // left reading 0 and stayed there for good, with the world drawing at
        // 400 regardless. Reported as settings "always saved as low".
        const auto clampF = [](float v, float lo, float hi) {
            return v < lo ? lo : (v > hi ? hi : v);
        };
        const auto clampI = [](int v, int lo, int hi) {
            return v < lo ? lo : (v > hi ? hi : v);
        };
        if (key == "graphics_preset")       loginGfx_.preset        = clampI(std::stoi(val), 0, kGraphicsPresetCount);
        else if (key == "shadows")          loginGfx_.shadows        = (val == "1");
        else if (key == "shadow_distance")  loginGfx_.shadowDistance = clampF(std::stof(val), 40.0f, 500.0f);
        else if (key == "view_distance")    loginGfx_.viewDistance   = clampF(std::stof(val), 400.0f, 2400.0f);
        else if (key == "fog_sky_blend")    loginGfx_.fogSkyBlend    = clampF(std::stof(val), 0.0f, 1.0f);
        else if (key == "fog_strength")     loginGfx_.fogStrength    = clampF(std::stof(val), 0.0f, 2.0f);
        else if (key == "sharp_stars")      loginGfx_.sharpStars     = (val == "1");
        else if (key == "antialiasing")     loginGfx_.antiAliasing   = clampI(std::stoi(val), 0, 3);
        else if (key == "fxaa")             loginGfx_.fxaa           = (val == "1");
        else if (key == "normal_mapping")   loginGfx_.normalMapping  = (val == "1");
        else if (key == "pom")              loginGfx_.pom            = (val == "1");
        else if (key == "pom_quality")      loginGfx_.pomQuality     = clampI(std::stoi(val), 0, rendering::kPomQualityCount - 1);
        else if (key == "upscaling_mode")   loginGfx_.upscalingMode  = clampI(std::stoi(val), 0, 2);
        else if (key == "water_refraction") loginGfx_.waterRefraction = (val == "1");
        else if (key == "ground_clutter_density") loginGfx_.groundClutter = clampI(std::stoi(val), 0, 150);
        else if (key == "grass_enabled")    loginGfx_.grass          = (val == "1");
        else if (key == "grass_density")    loginGfx_.grassDensity   = clampI(std::stoi(val), 0, 300);
        else if (key == "grass_height")     loginGfx_.grassHeight    = clampI(std::stoi(val), 50, 300);
        else if (key == "grass_distance")   loginGfx_.grassDistance  = clampI(std::stoi(val), 30, 800);
        else if (key == "brightness")       loginGfx_.brightness     = clampI(std::stoi(val), 0, 100);
        else if (key == "vsync")            loginGfx_.vsync          = (val == "1");
        else if (key == "fullscreen")       loginGfx_.fullscreen     = (val == "1");
    }
}

void AuthScreen::saveLoginGraphicsState() {
    // Read the full settings file into a map to preserve non-graphics keys.
    std::map<std::string, std::string> cfg;
    std::ifstream in(SettingsPanel::getSettingsPath());
    if (in.is_open()) {
        std::string line;
        while (std::getline(in, line)) {
            auto eq = line.find('=');
            if (eq != std::string::npos)
                cfg[line.substr(0, eq)] = line.substr(eq + 1);
        }
        in.close();
    }

    // Overwrite graphics keys.
    cfg["graphics_preset"]       = std::to_string(loginGfx_.preset);
    cfg["shadows"]               = loginGfx_.shadows        ? "1" : "0";
    cfg["shadow_distance"]       = std::to_string(static_cast<int>(loginGfx_.shadowDistance));
    cfg["view_distance"]         = std::to_string(static_cast<int>(loginGfx_.viewDistance));
    cfg["fog_sky_blend"]         = std::to_string(loginGfx_.fogSkyBlend);
    cfg["fog_strength"]          = std::to_string(loginGfx_.fogStrength);
    cfg["sharp_stars"]           = loginGfx_.sharpStars      ? "1" : "0";
    cfg["antialiasing"]          = std::to_string(loginGfx_.antiAliasing);
    cfg["fxaa"]                  = loginGfx_.fxaa           ? "1" : "0";
    cfg["normal_mapping"]        = loginGfx_.normalMapping  ? "1" : "0";
    cfg["pom"]                   = loginGfx_.pom            ? "1" : "0";
    cfg["pom_quality"]           = std::to_string(loginGfx_.pomQuality);
    cfg["upscaling_mode"]        = std::to_string(loginGfx_.upscalingMode);
    cfg["water_refraction"]      = loginGfx_.waterRefraction ? "1" : "0";
    cfg["ground_clutter_density"]= std::to_string(loginGfx_.groundClutter);
    cfg["grass_enabled"]         = loginGfx_.grass ? "1" : "0";
    cfg["grass_density"]         = std::to_string(loginGfx_.grassDensity);
    cfg["grass_height"]          = std::to_string(loginGfx_.grassHeight);
    cfg["grass_distance"]        = std::to_string(loginGfx_.grassDistance);
    cfg["brightness"]            = std::to_string(loginGfx_.brightness);
    cfg["vsync"]                 = loginGfx_.vsync           ? "1" : "0";
    cfg["fullscreen"]            = loginGfx_.fullscreen      ? "1" : "0";

    // Write everything back.
    std::ofstream out(SettingsPanel::getSettingsPath());
    if (!out.is_open()) return;
    for (const auto& [k, v] : cfg)
        out << k << "=" << v << "\n";
}

void AuthScreen::renderLoginSettingsSheet(float screenW, float screenH) {
    if (!loginGfxLoaded_) {
        loadLoginGraphicsState();
        loginGfxLoaded_ = true;
    }

    const PaperTheme& theme = ui_.theme();
    const auto px = [this](float units) { return ui_.px(units); };

    constexpr float kPad = 30.0f;
    constexpr float kTitle = 30.0f;
    constexpr float kLabel = 13.5f;
    constexpr float kSmall = 12.5f;
    constexpr float kControl = 34.0f;
    constexpr float kCheckBox = 19.0f;
    constexpr float kCheckStep = 30.0f;
    constexpr float kButton = 42.0f;

    const float labelRow = ui_.lineHeight(px(kLabel)) + px(3);
    const float controlRow = labelRow + px(kControl);
    const float smallRow = ui_.lineHeight(px(kSmall));

    // Seven switches on the left, six things with a range on the right. The
    // taller of the two columns is what the sheet has to be tall enough for.
    const float leftH = 7.0f * px(kCheckStep);
    const float rightH = 6.0f * (controlRow + px(8));
    const float contentH = px(kTitle) * 1.05f + smallRow + px(16)   // title block
                         + controlRow + px(20)                       // preset
                         + std::max(leftH, rightH) + px(22)
                         + px(kButton);
    const float w = px(560);
    const float h = px(kPad) * 2.0f + contentH;

    ui_.setLayer(PaperLayer::Overlay);
    ui_.scrim(0.45f);

    const ImVec2 a((screenW - w) * 0.5f, std::max(px(10), (screenH - h) * 0.5f));
    const ImVec2 b(a.x + w, a.y + h);
    ui_.sheet(a, b, /*taped=*/false);
    // The sheet eats every click that lands on it, including the ones that
    // land on none of its controls.
    ui_.claimMouse(a, b);

    Column col{a.x + px(kPad), b.x - px(kPad), a.y + px(kPad)};

    ui_.text(col.at(), "Graphics", px(kTitle), theme.ink, /*titleFace=*/true);
    {
        const float r = px(kSmall) * 0.95f;
        if (ui_.glyphButton("gfx.close", ImVec2(col.x1 - r, col.y + r), r,
                            PaperUI::Glyph::Cross)) {
            settingsOpen_ = false;
        }
    }
    col.gap(px(kTitle) * 1.05f);
    ui_.text(col.at(), "These take effect the next time you log in.", px(kSmall), theme.pencil);
    col.gap(smallRow + px(8));
    ui_.rule(ImVec2(col.x0, col.y), ImVec2(col.x1, col.y), paperFade(theme.pencil, 0.6f),
             px(1.0f), 0x9A17u);
    col.gap(px(8));

    // ---- preset -----------------------------------------------------------
    {
        ui_.text(col.at(), "Preset", px(kLabel), theme.inkSoft);
        col.gap(labelRow);
        const std::vector<std::string> names = {"Custom", "Low", "Medium", "High", "Ultra"};
        int preset = std::clamp(loginGfx_.preset, 0, static_cast<int>(names.size()) - 1);
        const auto [pa, pb] = col.cell(0.0f, px(220), px(kControl));
        if (ui_.dropdown("gfx.preset", pa, pb, names[static_cast<size_t>(preset)], names,
                         &preset)) {
            loginGfx_.preset = preset;
            // Custom is not a set of values, it is the absence of one; picking
            // it leaves everything where the player put it.
            if (preset != 0) applyPresetToState(loginGfx_, preset);
        }
        col.gap(px(20));
    }

    const float columnGap = px(30);
    const float columnW = (col.width() - columnGap) * 0.5f;
    const float columnTop = col.y;

    // ---- the switches -----------------------------------------------------
    {
        Column left{col.x0, col.x0 + columnW, columnTop};
        const auto toggle = [&](const char* id, const char* label, bool* value) {
            ui_.checkbox(id, ImVec2(left.x0, left.y), px(kCheckBox), label, value);
            left.gap(px(kCheckStep));
        };
        toggle("gfx.shadows", "Shadows", &loginGfx_.shadows);
        toggle("gfx.fxaa", "FXAA", &loginGfx_.fxaa);
        toggle("gfx.normals", "Normal mapping", &loginGfx_.normalMapping);
        toggle("gfx.pom", "Parallax occlusion", &loginGfx_.pom);
        toggle("gfx.water", "Water refraction", &loginGfx_.waterRefraction);
        toggle("gfx.vsync", "V-Sync", &loginGfx_.vsync);
        toggle("gfx.fullscreen", "Fullscreen", &loginGfx_.fullscreen);
    }

    // ---- the ranges -------------------------------------------------------
    {
        Column right{col.x1 - columnW, col.x1, columnTop};
        const auto labelled = [&](const char* label) {
            ui_.text(right.at(), label, px(kLabel), theme.inkSoft);
            right.gap(labelRow);
            return right.row(px(kControl));
        };

        {
            const auto [ra, rb] = labelled("Anti-aliasing");
            // The same four the settings schema offers. This list once had
            // three, so 8x could be set in the game, never shown here, and
            // silently rewritten by anything picked here instead.
            const std::vector<std::string> names = {"Off", "2x MSAA", "4x MSAA", "8x MSAA"};
            int aa = std::clamp(loginGfx_.antiAliasing, 0, 3);
            if (ui_.dropdown("gfx.aa", ra, rb, names[static_cast<size_t>(aa)], names, &aa))
                loginGfx_.antiAliasing = aa;
            right.gap(px(8));
        }
        {
            const auto [ra, rb] = labelled("Parallax quality");
            // The names and the count both come from the one scale, so this
            // cannot go back to offering two entries of a three-entry setting
            // and writing the wrong index for the ones it did offer.
            std::vector<std::string> names;
            for (int i = 0; i < rendering::kPomQualityCount; ++i)
                names.emplace_back(rendering::kPomQualityLabels[i]);
            int quality = std::clamp(loginGfx_.pomQuality, 0, rendering::kPomQualityCount - 1);
            if (ui_.dropdown("gfx.pomq", ra, rb, names[static_cast<size_t>(quality)], names,
                             &quality))
                loginGfx_.pomQuality = quality;
            right.gap(px(8));
        }
        {
            const auto [ra, rb] = labelled("Shadow distance");
            ui_.sliderFloat("gfx.shadowdist", ra, rb, &loginGfx_.shadowDistance, 50.0f, 600.0f);
            right.gap(px(8));
        }
        {
            const auto [ra, rb] = labelled("View distance");
            ui_.sliderFloat("gfx.viewdist", ra, rb, &loginGfx_.viewDistance, 400.0f, 2400.0f);
            right.gap(px(8));
        }
        {
            // 150, not 200: GameScreen::loadSettings clamps there, so the last
            // fifty units of this slider were silently discarded at login.
            const auto [ra, rb] = labelled("Ground clutter");
            ui_.sliderInt("gfx.clutter", ra, rb, &loginGfx_.groundClutter, 0, 150);
            right.gap(px(8));
        }
        {
            const auto [ra, rb] = labelled("Brightness");
            ui_.sliderInt("gfx.brightness", ra, rb, &loginGfx_.brightness, 0, 100);
            right.gap(px(8));
        }
    }

    col.y = columnTop + std::max(leftH, rightH) + px(22);

    // ---- the way out ------------------------------------------------------
    {
        const float y = col.y;
        const float bh = px(kButton);
        if (ui_.button("gfx.reset", ImVec2(col.x0, y), ImVec2(col.x0 + px(170), y + bh),
                       "Reset to Medium")) {
            applyPresetToState(loginGfx_, 2);
            loginGfx_.preset = 2;
        }
        const float applyW = px(130);
        const float cancelW = px(110);
        if (ui_.button("gfx.cancel", ImVec2(col.x1 - applyW - px(12) - cancelW, y),
                       ImVec2(col.x1 - applyW - px(12), y + bh), "Cancel")) {
            settingsOpen_ = false;
        }
        if (ui_.button("gfx.apply", ImVec2(col.x1 - applyW, y), ImVec2(col.x1, y + bh), "Apply",
                       PaperUI::ButtonKind::Primary)) {
            saveLoginGraphicsState();
            if (services_.window && services_.window->isVsyncEnabled() != loginGfx_.vsync) {
                services_.window->setVsync(loginGfx_.vsync);
            }
            settingsOpen_ = false;
        }
    }

    ui_.setLayer(PaperLayer::Page);
}

}} // namespace wowee::ui
