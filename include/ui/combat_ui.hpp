#pragma once

#include "ui/ui_services.hpp"
#include <imgui.h>
#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <glm/glm.hpp>

namespace wowee {
namespace game { class GameHandler; struct CombatTextEntry; }
namespace rendering { class Camera; }
namespace pipeline { class AssetManager; }
namespace ui {

class SettingsPanel;
class SpellbookScreen;
class InventoryScreen;

/**
 * Combat UI overlay manager (extracted from GameScreen)
 *
 * Owns all combat-related rendering:
 *   cast bar, cooldown tracker, raid warning overlay, floating combat text,
 *   DPS/HPS meter, buff bar, battleground score HUD, combat log,
 *   threat window, BG scoreboard.
 */
class CombatUI {
public:
    CombatUI() = default;

    // ---- Callback type for spell icon lookup (stays in GameScreen) ----
    using SpellIconFn = std::function<VkDescriptorSet(uint32_t spellId, pipeline::AssetManager*)>;

    // ---- Toggle booleans (written by slash commands / escape handler / settings) ----
    size_t raidWarnChatSeenCount_ = 0;

    // ---- Public render methods ----
    /// The whisper sound, taken from a walk of new chat lines. What is left
    /// of the raid warning overlay, whose banner is FrameXML's.
    void playSoundsForNewChat(game::GameHandler& gameHandler);
    /// World text over targets always; what happened to the player only when
    /// `drawIncoming` - false once Blizzard_CombatText is drawing that half.
    void renderCombatText(game::GameHandler& gameHandler, bool drawIncoming);
    /// The combat text size setting as a factor (combat_text_size.hpp).
    void setCombatTextScale(float scale) { combatTextScale_ = scale; }

    // UIServices injection (Phase B singleton breaking)
    void setServices(const UIServices& services) { services_ = services; }

private:
    float combatTextScale_ = 1.0f;
    /// One number over a unit, by the stock world-text rules (0x7e5c30).
    /// Screen rectangles of the world texts drawn over each unit this frame
    /// (x0, y0, x1, y1), so one landing with another is stepped clear of it.
    std::unordered_map<uint64_t, std::vector<ImVec4>> placedWorldText_;
    void drawWorldText(game::GameHandler& gameHandler, const game::CombatTextEntry& entry,
                       std::unordered_map<uint64_t, int>& onUnit, const glm::mat4& viewProj,
                       const rendering::Camera* camera, float screenW, float screenH,
                       ImFont* font);

    UIServices services_;
};

} // namespace ui
} // namespace wowee
