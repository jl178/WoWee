// InputModifierAdapter - wraps core::Input to implement IModifierState.
#pragma once

#include "ui/chat/i_modifier_state.hpp"

namespace wowee {
namespace ui {

/// The six modifier keys as SDL sees them. No modified-click actions: those
/// are the interface's (IsModifiedClick) and LuaModifierState answers them.
class InputModifierAdapter : public IModifierState {
public:
    [[nodiscard]] bool isKeyDown(Key key) const override;
};

} // namespace ui
} // namespace wowee
