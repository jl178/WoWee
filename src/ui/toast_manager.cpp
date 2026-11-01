#include "ui/toast_manager.hpp"
#include "rendering/renderer.hpp"
#include "rendering/animation_controller.hpp"

namespace wowee { namespace ui {

// The "Level N!" banner is a frame of the interface's own, raised from
// PLAYER_LEVEL_UP (lua_engine.cpp), and the level-up sound is played where the
// level is read. What is left here is the character's cheer.
void ToastManager::triggerDing(uint32_t /*newLevel*/) {
    if (auto* renderer = services_.renderer) {
        if (auto* ac = renderer->getAnimationController()) ac->playEmote("cheer");
    }
}

// No achievement sound here. alertframes.lua raises the badge, and the sound
// (AchievementSound) is played once where SMSG_ACHIEVEMENT_EARNED is read -
// it was played from both places, on the same packet.

} } // namespace wowee::ui
