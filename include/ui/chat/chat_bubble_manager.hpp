#pragma once

#include "ui/ui_services.hpp"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <vector>

namespace wowee {
namespace game { class GameHandler; }
namespace ui {

/**
 * The stock chat bubbles (CGChatBubbleFrame, ChatBubbleFrame.cpp): one per
 * speaker, hung over the speaker's name attachment, faded in and out, sized
 * and timed as 3.3.5a has them. docs/re/world-ui.md has the rules.
 */
class ChatBubbleManager {
public:
    /** Add or replace the bubble for the given speaker. */
    void addBubble(uint64_t senderGuid, const std::string& message, uint8_t chatType,
                   bool ownMessage = false);

    /** Render and tick all active bubbles (projects to screen via camera). */
    void render(game::GameHandler& gameHandler, const UIServices& services);

    /** Register the chat-bubble callback on GameHandler (call once per session). */
    void setupCallback(game::GameHandler& gameHandler);

    [[nodiscard]] bool empty() const { return bubbles_.empty(); }

    /// The chatBubbles CVar. The interface options put a checkbox on it and
    /// nothing here read it, so turning bubbles off left them on the screen.
    /// Turning it off drops what is queued too, rather than leaving the last
    /// few to fade.
    [[nodiscard]] bool bubblesShown() const { return show_; }
    void setBubblesShown(bool shown) { show_ = shown; }

private:
    struct ChatBubble {
        uint64_t senderGuid = 0;
        std::string message;
        uint8_t chatType = 1;   ///< game::ChatType, for the stock colour
        float ageMs = 0.0f;     ///< since the line arrived
        int lifetimeMs = 0;     ///< full strength until this, then a 250 ms fade
        float alpha = 0.0f;     ///< 0..1, eased toward its goal 1/250 ms
        uint32_t order = 0;     ///< creation order: newer draws on top
    };
    // Interface\Tooltips\ChatBubble-* textures, loaded once.
    VkDescriptorSet bg_ = VK_NULL_HANDLE, edge_ = VK_NULL_HANDLE, tail_ = VK_NULL_HANDLE;
    bool texGiveUp_ = false;
    std::vector<ChatBubble> bubbles_;
    uint32_t nextOrder_ = 0;
    bool callbackSet_ = false;
    bool show_ = true;
};

} // namespace ui
} // namespace wowee
