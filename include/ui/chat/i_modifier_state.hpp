// IModifierState - the keys [modifier] / [mod] read.
//
// Six keys, left and right apart, because the stock option takes LSHIFT and
// RSHIFT as well as SHIFT (0x55D860), and the names a modified click is bound
// to (SELFCAST, FOCUSCAST, ...) as IsModifiedClick answers them (0x55F940).
#pragma once

#include <string>

namespace wowee {
namespace ui {

class IModifierState {
public:
    enum class Key { LeftShift, RightShift, LeftCtrl, RightCtrl, LeftAlt, RightAlt };

    virtual ~IModifierState() = default;

    [[nodiscard]] virtual bool isKeyDown(Key key) const = 0;
    /// IsModifiedClick(action): whether the modifier bound to the named
    /// click action is held. Unknown actions answer false.
    [[nodiscard]] virtual bool isModifiedClick(const std::string& action) const {
        (void)action;
        return false;
    }

    [[nodiscard]] bool isShiftHeld() const { return isKeyDown(Key::LeftShift) || isKeyDown(Key::RightShift); }
    [[nodiscard]] bool isCtrlHeld() const  { return isKeyDown(Key::LeftCtrl) || isKeyDown(Key::RightCtrl); }
    [[nodiscard]] bool isAltHeld() const   { return isKeyDown(Key::LeftAlt) || isKeyDown(Key::RightAlt); }
};

} // namespace ui
} // namespace wowee
