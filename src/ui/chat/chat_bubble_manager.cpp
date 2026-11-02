// ChatBubbleManager - 3D-projected chat bubbles above entities.
// Moved from ChatPanel::renderBubbles / setupCallbacks (Phase 1.4).
#include "ui/chat/chat_bubble_manager.hpp"
#include "game/game_handler.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera.hpp"
#include "core/coordinates.hpp"
#include "core/window.hpp"
#include <imgui.h>
#include "ui/chat/chat_utils.hpp"
#include "ui/nameplate_registry.hpp"
#include "ui/world_ui_rules.hpp"
#include "core/application.hpp"
#include "ui/ui_texture_load.hpp"
#include "ui/interface_fonts.hpp"
#include "ui/text_markup.hpp"
#include "ui/text_wrap.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>

#include <glm/glm.hpp>

namespace wowee { namespace ui {

namespace {
// 3.3.5 ChatTypeInfo defaults for the chat types that get bubbles (the stock
// table at 0xb74728 is these until the player recolours a type).
ImVec4 bubbleColor(uint8_t t) {
    switch (t) {
        case 0x06: case 0x0E: return {1.0f, 0.251f, 0.251f, 1.0f};  // YELL, MONSTER_YELL
        case 0x02:            return {0.667f, 0.667f, 1.0f, 1.0f};  // PARTY
        case 0x33:            return {0.463f, 0.784f, 1.0f, 1.0f};  // PARTY_LEADER
        case 0x0C:            return {1.0f, 1.0f, 0.624f, 1.0f};    // MONSTER_SAY
        default:              return {1.0f, 1.0f, 1.0f, 1.0f};      // SAY
    }
}

float distanceToPlayer(game::GameHandler& gh, const game::Entity& e) {
    auto self = gh.getEntityManager().getEntity(gh.getPlayerGuid());
    if (!self) return 0.0f;
    const glm::vec3 d(e.getX() - self->getX(), e.getY() - self->getY(), e.getZ() - self->getZ());
    return glm::length(d);
}
} // namespace

void ChatBubbleManager::addBubble(uint64_t senderGuid, const std::string& message, uint8_t chatType,
                                  bool ownMessage) {
    // A unit with a nameplate gets no bubble (0x720010): the plate is where
    // its name is.
    if (platedUnits().count(senderGuid)) return;
    const int lifetime = stockui::chatBubbleLifetimeMs(message, ownMessage);
    // One bubble per speaker: a new line replaces the old one, fading in again.
    for (auto& b : bubbles_) {
        if (b.senderGuid == senderGuid) {
            b = ChatBubble{.senderGuid = senderGuid, .message = message, .chatType = chatType,
                           .ageMs = 0.0f, .lifetimeMs = lifetime, .alpha = 0.0f,
                           .order = nextOrder_++};
            return;
        }
    }
    bubbles_.push_back({.senderGuid = senderGuid, .message = message, .chatType = chatType,
                        .ageMs = 0.0f, .lifetimeMs = lifetime, .alpha = 0.0f,
                        .order = nextOrder_++});
}

void ChatBubbleManager::render(game::GameHandler& gameHandler, const UIServices& services) {
    // Off means off: the queue is dropped as well, so turning the setting off
    // clears what is on screen rather than leaving it to fade.
    if (!show_) { bubbles_.clear(); return; }
    if (bubbles_.empty()) return;

    auto* renderer = services.renderer;
    auto* camera = renderer ? renderer->getCamera() : nullptr;
    if (!camera) return;

    auto* window = services.window;
    auto load = [&](VkDescriptorSet& ds, const char* p) {
        if (ds || texGiveUp_ || !services.assetManager || !window) return;
        UiTextureLoad why{};
        ds = uploadUiTextureFromBlp(services.assetManager, p, window, &why);
        if (why == UiTextureLoad::NotFound || why == UiTextureLoad::DecodeFailed) texGiveUp_ = true;
    };
    load(bg_,   "Interface\\Tooltips\\ChatBubble-Background.blp");
    load(edge_, "Interface\\Tooltips\\ChatBubble-Backdrop.blp");
    load(tail_, "Interface\\Tooltips\\ChatBubble-Tail.blp");

    const float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    const float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;
    const float dtMs = ImGui::GetIO().DeltaTime * 1000.0f;
    const glm::mat4 viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();

    // Newest on top (0x56d050 gives them rising frame levels by age).
    std::sort(bubbles_.begin(), bubbles_.end(),
              [](const ChatBubble& a, const ChatBubble& b) { return a.order < b.order; });

    // Sizes: the text and its padding are in the engine's DDC, the backdrop
    // and tail in plain interface units.
    const float fs = stockui::ddcPixels(stockui::kChatBubbleTextDdc, screenW, screenH);
    const float pad = stockui::ddcPixels(stockui::kChatBubblePadDdc, screenW, screenH);
    const float maxTextW = stockui::ddcPixels(stockui::kChatBubbleMaxTextDdc, screenW, screenH);
    const float minTextW = stockui::uiUnitPixels(stockui::kChatBubbleMinTextUnits, screenH);
    const float e = stockui::uiUnitPixels(stockui::kChatBubbleEdgeUnits, screenH);
    const float tailSz = stockui::uiUnitPixels(stockui::kChatBubbleTailUnits, screenH);
    ImFont* font = interfaceFace("frizqt__");
    if (!font) font = ImGui::GetFont();
    const float lineH = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, "Ay").y;
    auto measure = [&](const std::string& t) {
        return font->CalcTextSizeA(fs, FLT_MAX, 0.0f, t.c_str()).x;
    };
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    auto tex = [](VkDescriptorSet d) { return reinterpret_cast<ImTextureID>(d); };

    for (size_t i = 0; i < bubbles_.size();) {
        auto& bubble = bubbles_[i];
        bubble.ageMs += dtMs;
        auto entity = gameHandler.getEntityManager().getEntity(bubble.senderGuid);
        const float endMs = static_cast<float>(bubble.lifetimeMs) + stockui::kChatBubbleFadeMs;
        if (!entity || (bubble.ageMs > endMs && bubble.alpha <= 0.0f)) {
            bubbles_.erase(bubbles_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;

        // Anchor: the name attachment's height over the unit, 0.7 yd higher
        // (0x56c500); the bubble's BOTTOM sits on it.
        glm::vec3 headPos;
        if (!core::Application::getInstance().getNamePositionForGuid(bubble.senderGuid, headPos)) {
            headPos = core::coords::canonicalToRender(
                glm::vec3(entity->getX(), entity->getY(), entity->getZ() + 2.0f));
        }
        headPos.z += stockui::kChatBubbleLiftYards;
        const glm::vec4 clip = viewProj * glm::vec4(headPos, 1.0f);
        const bool inFront = clip.w > 0.01f;

        // Full strength while it lasts, in range (25 yd from the player) and in
        // view; a bubble out of range or view fades out but is kept, and comes
        // back when its speaker does.
        const bool wanted = bubble.ageMs <= endMs && inFront &&
                            distanceToPlayer(gameHandler, *entity) <= stockui::kChatBubbleRange;
        const float step = dtMs / stockui::kChatBubbleFadeMs;
        bubble.alpha = std::clamp(bubble.alpha + (wanted ? step : -step), 0.0f, 1.0f);
        if (bubble.alpha <= 0.0f || !inFront) continue;

        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        const float ax = (ndc.x * 0.5f + 0.5f) * screenW;
        const float ay = (ndc.y * 0.5f + 0.5f) * screenH;

        // The text: wrapped at 0.2 DDC, never narrower than two edges.
        const auto lines = wrapText(parseMarkup(bubble.message), maxTextW, true, measure);
        std::vector<float> lw;
        float natural = 0.0f;
        for (const auto& ln : lines) {
            float w = 0.0f;
            for (const auto& run : ln) w += measure(run.text);
            lw.push_back(w);
            natural = std::max(natural, w);
        }
        const float textW = std::clamp(natural, minTextW, maxTextW);
        const float textH = lineH * static_cast<float>(std::max<size_t>(lines.size(), 1));
        const float bw = textW + pad * 2.0f;
        const float bh = std::max(textH + pad * 2.0f, e * 2.0f);
        const float x0 = std::round(ax - bw * 0.5f), x1 = x0 + bw;
        const float y1 = std::round(ay), y0 = y1 - bh;
        const int a8 = static_cast<int>(bubble.alpha * 255.0f);
        const ImU32 white = IM_COL32(255, 255, 255, a8);

        if (bg_) {
            dl->AddImage(tex(bg_), {x0 + e, y0 + e}, {x1 - e, y1 - e}, {0, 0},
                         {std::max(0.0f, bw - 2 * e) / e, std::max(0.0f, bh - 2 * e) / e}, white);
        } else {
            dl->AddRectFilled({x0, y0}, {x1, y1}, IM_COL32(0, 0, 0, a8 * 3 / 4), e * 0.5f);
        }
        if (edge_) {  // 8 cells: left, right, top, bottom, then the four corners
            auto cell = [&](int c, ImVec2 a, ImVec2 b) {
                dl->AddImage(tex(edge_), a, b, {c / 8.0f, 0.0f}, {(c + 1) / 8.0f, 1.0f}, white);
            };
            auto hrun = [&](int c, float px0, float py0, float px1, float py1) {
                const float u0 = c / 8.0f, u1 = (c + 1) / 8.0f, f = (px1 - px0) / e;
                dl->AddImageQuad(tex(edge_), {px0, py0}, {px1, py0}, {px1, py1}, {px0, py1},
                                 {u0, 0.0f}, {u0, f}, {u1, f}, {u1, 0.0f}, white);
            };
            cell(0, {x0, y0 + e}, {x0 + e, y1 - e});
            cell(1, {x1 - e, y0 + e}, {x1, y1 - e});
            hrun(2, x0 + e, y0, x1 - e, y0 + e);
            hrun(3, x0 + e, y1 - e, x1 - e, y1);
            cell(4, {x0, y0}, {x0 + e, y0 + e});
            cell(5, {x1 - e, y0}, {x1, y0 + e});
            cell(6, {x0, y1 - e}, {x0 + e, y1});
            cell(7, {x1 - e, y1 - e}, {x1, y1});
        }
        if (tail_) {
            // TOPRIGHT at the bubble's BOTTOM, 4 units up into it.
            const float cx = std::round(ax);
            dl->AddImage(tex(tail_), {cx - tailSz, y1 - stockui::uiUnitPixels(4.0f, screenH)},
                         {cx, y1 - stockui::uiUnitPixels(4.0f, screenH) + tailSz}, {0, 0}, {1, 1},
                         white);
        }
        const ImVec4 base = bubbleColor(bubble.chatType);
        float ty = y0 + (bh - textH) * 0.5f;
        for (size_t li = 0; li < lines.size(); ++li, ty += lineH) {
            float tx = std::round(ax - lw[li] * 0.5f);
            for (const auto& run : lines[li]) {
                ImVec4 c = run.hasColor ? ImVec4(run.rgba[0], run.rgba[1], run.rgba[2], 1.0f) : base;
                c.w = bubble.alpha;
                dl->AddText(font, fs, {tx, std::round(ty)}, ImGui::GetColorU32(c), run.text.c_str());
                tx += measure(run.text);
            }
        }
    }
}

void ChatBubbleManager::setupCallback(game::GameHandler& gameHandler) {
    if (!callbackSet_) {
        // Resolve $-tokens here rather than at draw time: a bubble is built once
        // and drawn every frame. The chat panel already does this for the same
        // text, which is why an NPC line read correctly in the log and showed
        // its raw $r/$c/$g markers in the bubble over their head.
        gameHandler.setChatBubbleCallback(
            [this, &gameHandler](uint64_t guid, const std::string& msg, uint8_t type) {
                addBubble(guid, chat_utils::replaceGenderPlaceholders(msg, gameHandler), type,
                          guid == gameHandler.getPlayerGuid());
            });
        callbackSet_ = true;
    }
}

} // namespace ui
} // namespace wowee
