// InputModifierAdapter - concrete IModifierState wrapping core::Input.
#include "ui/chat/input_modifier_adapter.hpp"
#include "core/input.hpp"
#include <SDL3/SDL_scancode.h>

namespace wowee { namespace ui {

bool InputModifierAdapter::isKeyDown(Key key) const {
    auto& input = core::Input::getInstance();
    switch (key) {
        case Key::LeftShift:  return input.isKeyPressed(SDL_SCANCODE_LSHIFT);
        case Key::RightShift: return input.isKeyPressed(SDL_SCANCODE_RSHIFT);
        case Key::LeftCtrl:   return input.isKeyPressed(SDL_SCANCODE_LCTRL);
        case Key::RightCtrl:  return input.isKeyPressed(SDL_SCANCODE_RCTRL);
        case Key::LeftAlt:    return input.isKeyPressed(SDL_SCANCODE_LALT);
        case Key::RightAlt:   return input.isKeyPressed(SDL_SCANCODE_RALT);
    }
    return false;
}

} // namespace ui
} // namespace wowee
