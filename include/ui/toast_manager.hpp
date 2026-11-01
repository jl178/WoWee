#pragma once

#include "ui/ui_services.hpp"
#include <string>
#include <cstdint>

namespace wowee {
namespace ui {

/**
 * What is left of the client's own notifications.
 *
 * The level-up banner, the resurrection banner and the duel countdown are
 * frames of the interface's own (lua_engine.cpp); reputation, honour, loot and
 * discovery are chat lines, and the zone banner is ZoneTextFrame. This keeps
 * the sound and the emote that go with them.
 */
class ToastManager {
public:
    ToastManager() = default;

    /// The character cheers on a level-up.
    void triggerDing(uint32_t newLevel);

    // UIServices injection (Phase B singleton breaking)
    void setServices(const UIServices& services) { services_ = services; }

private:
    UIServices services_;
};

} // namespace ui
} // namespace wowee
