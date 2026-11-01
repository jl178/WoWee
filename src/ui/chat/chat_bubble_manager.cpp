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
#include "ui/sight_cache.hpp"
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
// 3.3.5 ChatTypeInfo defaults for the chat types that get bubbles.
ImVec4 bubbleColor(uint8_t t) {
    switch (t) {
        case 0x06: case 0x0E: return {1.0f, 0.251f, 0.251f, 1.0f};  // YELL, MONSTER_YELL
        case 0x02: case 0x0D: return {0.667f, 0.667f, 1.0f, 1.0f};  // PARTY, MONSTER_PARTY
        case 0x03:            return {1.0f, 0.498f, 0.0f, 1.0f};    // RAID
        case 0x0C:            return {1.0f, 1.0f, 0.624f, 1.0f};    // MONSTER_SAY
        default:              return {1.0f, 1.0f, 1.0f, 1.0f};      // SAY
    }
}
} // namespace

void ChatBubbleManager::addBubble(uint64_t senderGuid, const std::string& message, uint8_t chatType) {
    const bool isYell = chatType == 0x06 || chatType == 0x0E;
    float duration = 8.0f + static_cast<float>(message.size()) * 0.06f;
    if (isYell) duration += 2.0f;
    if (duration > 15.0f) duration = 15.0f;

    // Replace existing bubble for same sender
    for (auto& b : bubbles_) {
        if (b.senderGuid == senderGuid) {
            b.message = message;
            b.timeRemaining = duration;
            b.totalDuration = duration;
            b.chatType = chatType;
            return;
        }
    }
    // Evict oldest if too many
    if (bubbles_.size() >= kMaxBubbles) {
        bubbles_.erase(bubbles_.begin());
    }
    bubbles_.push_back({.senderGuid = senderGuid, .message = message, .timeRemaining = duration, .totalDuration = duration, .chatType = chatType});
}

void ChatBubbleManager::render(game::GameHandler& gameHandler, const UIServices& services) {
    // Off means off: the queue is dropped as well, so turning the setting off
    // clears what is on screen rather than leaving it to fade for five seconds.
    if (!show_) { bubbles_.clear(); return; }
    if (bubbles_.empty()) return;

    auto* renderer = services.renderer;
    auto* camera = renderer ? renderer->getCamera() : nullptr;
    if (!camera) return;

    auto* window0 = services.window;
    auto load = [&](VkDescriptorSet& ds, const char* p) {
        if (ds || texGiveUp_ || !services.assetManager || !window0) return;
        UiTextureLoad why{};
        ds = uploadUiTextureFromBlp(services.assetManager, p, window0, &why);
        if (why == UiTextureLoad::NotFound || why == UiTextureLoad::DecodeFailed) texGiveUp_ = true;
    };
    load(bg_,   "Interface\\Tooltips\\ChatBubble-Background.blp");
    load(edge_, "Interface\\Tooltips\\ChatBubble-Backdrop.blp");
    load(tail_, "Interface\\Tooltips\\ChatBubble-Tail.blp");

    auto* window = services.window;
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;

    // Get delta time from ImGui
    float dt = ImGui::GetIO().DeltaTime;

    glm::mat4 viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();

    // Update and render bubbles
    for (int i = static_cast<int>(bubbles_.size()) - 1; i >= 0; --i) {
        auto& bubble = bubbles_[i];
        bubble.timeRemaining -= dt;
        if (bubble.timeRemaining <= 0.0f) {
            bubbles_.erase(bubbles_.begin() + i);
            continue;
        }

        // Get entity position
        auto entity = gameHandler.getEntityManager().getEntity(bubble.senderGuid);
        if (!entity) continue;

        // Convert canonical → render coordinates, offset up by 2.5 units for bubble above head
        glm::vec3 canonical(entity->getX(), entity->getY(), entity->getZ() + 2.5f);
        glm::vec3 renderPos = core::coords::canonicalToRender(canonical);

        // Not through a wall. A bubble carries a name as well as a line, so
        // one drawn over the building someone is standing behind says they are
        // in front of it - the same fault the nameplates had, answered by the
        // same query.
        static SightCache sight;
        if (sight.blocked(renderer->getWMORenderer(), bubble.senderGuid,
                          camera->getPosition(), renderPos)) {
            continue;
        }

        // Project to screen
        glm::vec4 clipPos = viewProj * glm::vec4(renderPos, 1.0f);
        if (clipPos.w <= 0.0f) continue;  // Behind camera

        glm::vec2 ndc(clipPos.x / clipPos.w, clipPos.y / clipPos.w);
        float screenX = (ndc.x * 0.5f + 0.5f) * screenW;
        // Camera bakes the Vulkan Y-flip into the projection matrix:
        // NDC y=-1 is top, y=1 is bottom - same convention as nameplate/minimap projection.
        float screenY = (ndc.y * 0.5f + 0.5f) * screenH;

        // Skip if off-screen
        if (screenX < -200.0f || screenX > screenW + 200.0f ||
            screenY < -100.0f || screenY > screenH + 100.0f) continue;

        // Fade alpha over last 2 seconds
        float alpha = 1.0f;
        if (bubble.timeRemaining < 2.0f) {
            alpha = bubble.timeRemaining / 2.0f;
        }

        // Into the background draw list, which is where the interface is
        // drawn - and drawn into before it, so the interface covers this.
        //
        // These were ImGui windows, and an ImGui window is above every draw
        // list the interface uses, whatever the interface is doing: a bubble
        // sat on top of the auction house, the bags, the map, anything open.
        // Nothing about the position was wrong; a window was simply the one
        // thing that cannot go underneath.
        // Say range, fading out towards its edge as the stock bubbles do.
        const float s = screenH / 768.0f;  // interface units to pixels
        // Measured from the nearer of the camera and the player's character.
        // From the camera alone, a zoomed-out view (22 yards and more behind
        // the player) put someone at the edge of /say range - 25 yards from
        // the player, on the far side - past the fade, and their bubble
        // faded or vanished while their line was in chat.
        float dist = glm::length(camera->getPosition() - renderPos);
        if (auto self = gameHandler.getEntityManager().getEntity(gameHandler.getPlayerGuid())) {
            const glm::vec3 me(self->getX(), self->getY(), self->getZ() + 2.5f);
            dist = std::min(dist, glm::length(me - canonical));
        }
        alpha *= std::clamp((60.0f - dist) / 20.0f, 0.0f, 1.0f);
        if (alpha <= 0.0f) continue;

        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        ImFont* font = interfaceFace("frizqt__");
        if (!font) font = ImGui::GetFont();
        const float fs = 13.0f * s, lineH = fs * 1.2f;
        const float e = 16.0f * s, pad = 12.0f * s, maxW = 300.0f * s;
        auto measure = [&](const std::string& t) {
            return font->CalcTextSizeA(fs, FLT_MAX, 0.0f, t.c_str()).x;
        };
        const auto lines = wrapText(parseMarkup(bubble.message), maxW, true, measure);
        std::vector<float> lw;
        float textW = 0.0f;
        for (const auto& ln : lines) {
            float w = 0.0f;
            for (const auto& run : ln) w += measure(run.text);
            lw.push_back(w);
            textW = std::max(textW, w);
        }
        const float bw = std::max(textW + pad * 2.0f, e * 2.0f);
        const float bh = std::max(lineH * static_cast<float>(lines.size()) + pad * 2.0f, e * 2.0f);
        const float tailH = 16.0f * s;
        const float x0 = screenX - bw * 0.5f, x1 = x0 + bw;
        const float y1 = screenY - tailH + 4.0f * s, y0 = y1 - bh;
        const ImU32 white = IM_COL32(255, 255, 255, static_cast<int>(alpha * 255.0f));
        auto tex = [](VkDescriptorSet d) { return reinterpret_cast<ImTextureID>(d); };
        if (bg_) {
            dl->AddImage(tex(bg_), {x0 + e, y0 + e}, {x1 - e, y1 - e}, {0, 0},
                         {(bw - 2 * e) / e, (bh - 2 * e) / e}, white);
        } else {
            dl->AddRectFilled({x0, y0}, {x1, y1}, IM_COL32(0, 0, 0, static_cast<int>(alpha * 180.0f)), 8.0f * s);
        }
        if (edge_) {  // 8 cells: left, right, top, bottom, then the four corners
            auto cell = [&](int i, ImVec2 a, ImVec2 b) {
                dl->AddImage(tex(edge_), a, b, {i / 8.0f, 0.0f}, {(i + 1) / 8.0f, 1.0f}, white);
            };
            auto hrun = [&](int i, float px0, float py0, float px1, float py1) {
                const float u0 = i / 8.0f, u1 = (i + 1) / 8.0f, f = (px1 - px0) / e;
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
            dl->AddImage(tex(tail_), {screenX - 8.0f * s, y1 - 4.0f * s},
                         {screenX + 8.0f * s, y1 - 4.0f * s + tailH}, {0, 0}, {1, 1}, white);
        }
        const ImVec4 base = bubbleColor(bubble.chatType);
        float ty = y0 + (bh - lineH * static_cast<float>(lines.size())) * 0.5f;
        for (size_t li = 0; li < lines.size(); ++li, ty += lineH) {
            float tx = screenX - lw[li] * 0.5f;  // centred, like stock
            for (const auto& run : lines[li]) {
                ImVec4 c = run.hasColor ? ImVec4(run.rgba[0], run.rgba[1], run.rgba[2], 1.0f) : base;
                c.w = alpha;
                dl->AddText(font, fs, {tx, ty}, ImGui::GetColorU32(c), run.text.c_str());
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
                addBubble(guid, chat_utils::replaceGenderPlaceholders(msg, gameHandler), type);
            });
        callbackSet_ = true;
    }
}

} // namespace ui
} // namespace wowee
