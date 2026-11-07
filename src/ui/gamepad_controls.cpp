#include "ui/gamepad_controls.hpp"

#include "core/gamepad.hpp"
#include "core/input.hpp"
#include "ui/keybinding_manager.hpp"
#include "addons/lua_api_registrations.hpp"
#include "core/logger.hpp"
#include "rendering/camera_controller.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace wowee {
namespace ui {

namespace {

/// Whether a direction is pushed far enough to count, given whether it was
/// already. The release threshold is lower than the press one, so a thumb
/// resting on the edge does not flicker between walking and standing.
bool pushed(float axis, float threshold, bool wasOn) {
    const float release = std::max(threshold - 0.10f, 0.05f);
    return axis > (wasOn ? release : threshold);
}

}  // namespace

GamepadControls& gamepadControls() {
    static GamepadControls instance;
    return instance;
}

void GamepadControls::setInWorld(bool inWorld) {
    if (inWorld_ == inWorld) return;
    inWorld_ = inWorld;
    if (!inWorld_) reset();
}

void GamepadControls::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (!enabled_) reset();
}

void GamepadControls::setLookDegreesPerSecond(float degrees) {
    lookDegreesPerSecond_ = std::clamp(degrees, 30.0f, 720.0f);
}

void GamepadControls::releaseKeys() {
    stickMovement_ = {};
    auto& bindings = KeybindingManager::getInstance();
    for (const char* command : {"MOVEFORWARD", "MOVEBACKWARD", "STRAFELEFT", "STRAFERIGHT"})
        bindings.setVirtualCommand(command, "pad-stick", false);
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
        const auto& route = routes_[static_cast<size_t>(b)];
        bindings.setVirtualCommand(route.command, "pad-button" + std::to_string(b), false);
        if (buttonWasDown_[static_cast<size_t>(b)] && keyRouter_ && !route.key.empty())
            keyRouter_(route.key.c_str(), false);
    }
    // Forgotten as well as released, so a button still held when the pad is
    // live again is looked up afresh rather than answered from before.
    routes_.fill(ButtonRoute{});
    buttonWasDown_.fill(false);
    steering_ = false;
    zoomRemainder_ = 0.0f;
}

void GamepadControls::reset() {
    releaseKeys();
    for (std::size_t i = 0; i < heldMouseButtons_.size(); ++i) {
        if (!heldMouseButtons_[i]) continue;
        heldMouseButtons_[i] = false;
        const int button = static_cast<int>(i);
        core::Input::getInstance().setVirtualMouseButton(button, false);
        ImGui::GetIO().AddMouseButtonEvent(button == SDL_BUTTON_RIGHT ? 1 : 0, false);
    }
    touchTrail_.reset();
    touchClickButton_ = 0;
}

void GamepadControls::applyMovement(float x, float y) {
    // SDL's Y is positive downwards, and pushing the stick up means forward.
    const bool forward = pushed(-y, kWalkThreshold, stickMovement_.forward);
    const bool back = pushed(y, kWalkThreshold, stickMovement_.back);
    // Sideways means strafe; the right stick turns the view.
    const bool left = pushed(-x, kStrafeThreshold, stickMovement_.left);
    const bool right = pushed(x, kStrafeThreshold, stickMovement_.right);
    stickMovement_ = {forward, back, left, right};
    auto& bindings = KeybindingManager::getInstance();
    bindings.setVirtualCommand("MOVEFORWARD", "pad-stick", forward);
    bindings.setVirtualCommand("MOVEBACKWARD", "pad-stick", back);
    bindings.setVirtualCommand("STRAFELEFT", "pad-stick", left);
    bindings.setVirtualCommand("STRAFERIGHT", "pad-stick", right);

    // Facing follows the camera while the stick is pushed, which is what the
    // right mouse button does on a desktop. Without it the character walks
    // sideways across the screen while still facing wherever they last were.
    steering_ = forward || back || left || right;
}

void GamepadControls::applyLook(float x, float y, float deltaTime) {
    if (!camera_) return;
    if (x == 0.0f && y == 0.0f) return;
    // In degrees, not mouse pixels: the pad's turn rate is degrees a second,
    // so the mouse's speeds (which differ by axis) do not reach it.
    const float degrees = lookDegreesPerSecond_ * deltaTime;
    const float up = invertLook_ ? -y : y;
    camera_->applyLookDegrees(-x * degrees, -up * degrees);
}

void GamepadControls::applyZoom(float in, float out, float deltaTime) {
    if (!camera_) return;
    const float pull = in - out;
    if (pull == 0.0f) {
        zoomRemainder_ = 0.0f;
        return;
    }
    // Carried between frames. A trigger held a third of the way is worth two
    // notches a second, which is less than one in any single frame - rounded
    // away each time, it would be a trigger that does nothing.
    zoomRemainder_ += pull * kZoomNotchesPerSecond * deltaTime;
    const float whole = std::trunc(zoomRemainder_);
    if (whole == 0.0f) return;
    zoomRemainder_ -= whole;
    camera_->processMouseWheel(whole);
}

void GamepadControls::holdMouseButton(int button, bool held) {
    if (button < 0 || button >= static_cast<int>(heldMouseButtons_.size())) return;
    const auto i = static_cast<std::size_t>(button);
    // A button this is not holding is left alone, so a real
    // mouse and the pad cannot switch each other off.
    if (!held && !heldMouseButtons_[i]) return;
    if (held == heldMouseButtons_[i]) return;
    heldMouseButtons_[i] = held;
    // Both channels, because both are read. The world's own targeting polls
    // core::Input, and every panel asks ImGui.
    core::Input::getInstance().setVirtualMouseButton(button, held);
    ImGui::GetIO().AddMouseButtonEvent(button == SDL_BUTTON_RIGHT ? 1 : 0, held);
}

void GamepadControls::setPointerMode(bool on) {
    if (pointerMode_ == on) return;
    pointerMode_ = on;
    // Nothing is released here. Every path that turns the pointer off either
    // resets all held buttons or goes on to applyClicks, which stops counting
    // A and X - and a release here would drop a click the touchpad still holds.
    if (!on) return;
    // Starts where the pointer already is rather than at the middle of the
    // screen, so turning it on twice does not throw away where it was left.
    float fx = 0.0f;
    float fy = 0.0f;
    SDL_GetMouseState(&fx, &fy);
    const int x = static_cast<int>(fx);
    const int y = static_cast<int>(fy);
    pointerX_ = fx;
    pointerY_ = fy;
    if (window_ && (x == 0 && y == 0)) {
        int w = 0;
        int h = 0;
        SDL_GetWindowSize(window_, &w, &h);
        pointerX_ = static_cast<float>(w) * 0.5f;
        pointerY_ = static_cast<float>(h) * 0.5f;
    }
}

void GamepadControls::applyPointer(float deltaTime) {
    if (!window_) return;
    int w = 0;
    int h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    if (w <= 0 || h <= 0) return;

    const auto& pad = core::gamepad();
    const glm::vec2 step = pointerStep(pad.rightStick().x, pad.rightStick().y, deltaTime);
    if (step.x != 0.0f || step.y != 0.0f) {
        pointerX_ = std::clamp(pointerX_ + step.x, 0.0f, static_cast<float>(w - 1));
        pointerY_ = std::clamp(pointerY_ + step.y, 0.0f, static_cast<float>(h - 1));
        SDL_WarpMouseInWindow(window_, static_cast<int>(pointerX_), static_cast<int>(pointerY_));
    }
}

void GamepadControls::applyTouchpad() {
    const auto& finger = core::gamepad().touch(0);
    // Followed every frame, before anything can return, so a finger that
    // was down while there was no window is not read as one long slide.
    const glm::vec2 slide = touchTrail_.follow(finger.down, finger.position);
    if (!window_ || (slide.x == 0.0f && slide.y == 0.0f)) return;

    int w = 0;
    int h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    if (w <= 0 || h <= 0) return;

    // Starts from where the cursor really is. The touchpad works outside
    // pointer mode too, so the stored position can be stale - left from the
    // last time the stick moved it, with a real mouse used since.
    float fx = 0.0f;
    float fy = 0.0f;
    SDL_GetMouseState(&fx, &fy);
    if (std::abs(fx - pointerX_) > 1.0f || std::abs(fy - pointerY_) > 1.0f) {
        pointerX_ = fx;
        pointerY_ = fy;
    }
    const glm::vec2 step = touchStep(slide, static_cast<float>(w));
    pointerX_ = std::clamp(pointerX_ + step.x, 0.0f, static_cast<float>(w - 1));
    pointerY_ = std::clamp(pointerY_ + step.y, 0.0f, static_cast<float>(h - 1));
    SDL_WarpMouseInWindow(window_, static_cast<int>(pointerX_), static_cast<int>(pointerY_));
}

void GamepadControls::applyClicks() {
    const auto& pad = core::gamepad();
    if (!pad.held(SDL_GAMEPAD_BUTTON_TOUCHPAD)) {
        touchClickButton_ = 0;
    } else if (touchClickButton_ == 0) {
        touchClickButton_ = pad.touch(1).down ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
    }

    // A is the click, as it is on every console, and X is the right click -
    // which in this game opens a corpse, uses a door and brings up a unit's
    // menu, so a pad without one cannot loot. Those two only count while the
    // pointer is up. Both channels are set once, from every source together,
    // so one source letting go cannot release a button another still holds.
    // held rather than schemeHeld: while the pointer is up these two are the
    // pointer's, whatever they are bound to. The default scheme's own entries
    // for them live in the binding table now, so asking whether they are
    // still unbound would answer no and leave the pointer with no click.
    const bool left = (pointerMode_ && pad.held(SDL_GAMEPAD_BUTTON_SOUTH)) ||
                      touchClickButton_ == SDL_BUTTON_LEFT;
    const bool right = (pointerMode_ && pad.held(SDL_GAMEPAD_BUTTON_WEST)) ||
                       touchClickButton_ == SDL_BUTTON_RIGHT;
    holdMouseButton(SDL_BUTTON_LEFT, left);
    holdMouseButton(SDL_BUTTON_RIGHT, right);
}

bool GamepadControls::schemeHeld(SDL_GamepadButton button) const {
    if (button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT) return false;
    return core::gamepad().held(button) &&
           routes_[static_cast<std::size_t>(button)].kind == PadKeyAnswer::Kind::Unbound;
}

void GamepadControls::routeButtons() {
    const auto& pad = core::gamepad();
    auto& bindings = KeybindingManager::getInstance();
    const auto route = [&](SDL_GamepadButton button) {
        const int b = static_cast<int>(button);
        const size_t i = static_cast<size_t>(b);
        const bool down = pad.held(button);
        const bool wasDown = buttonWasDown_[i];
        buttonWasDown_[i] = down;
        const char* name = padKeyName(button);
        if (!down) {
            bindings.setVirtualCommand(routes_[i].command, "pad-button" + std::to_string(b), false);
            if (wasDown && keyRouter_ && !routes_[i].key.empty()) keyRouter_(routes_[i].key.c_str(), false);
            routes_[i] = {};
            return;
        }
        if (wasDown || !*name) return;
        if (pointerMode_ && (button == SDL_GAMEPAD_BUTTON_SOUTH || button == SDL_GAMEPAD_BUTTON_WEST)) {
            routes_[i].kind = PadKeyAnswer::Kind::Taken;
            return;
        }
        routes_[i].key = button != SDL_GAMEPAD_BUTTON_LEFT_SHOULDER &&
                         schemeHeld(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)
            ? "SHIFT-" + std::string(name) : std::string(name);
        PadKeyAnswer answer;
        if (keyRouter_) answer = keyRouter_(routes_[i].key.c_str(), true);
        if (answer.kind == PadKeyAnswer::Kind::Unbound) {
            answer.command = bindings.resolveCommand(routes_[i].key);
            if (!answer.command.empty())
                answer.kind = addons::clientActsOnBinding(answer.command)
                    ? PadKeyAnswer::Kind::Command : PadKeyAnswer::Kind::Taken;
        }
        routes_[i].kind = answer.kind;
        routes_[i].command = answer.command;
    };
    // Resolve the modifier and pointer mode before A/X, even on a simultaneous
    // press: pointer clicks must never run a gameplay binding or Lua script.
    route(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    route(SDL_GAMEPAD_BUTTON_BACK);
    const bool toggle = schemeHeld(SDL_GAMEPAD_BUTTON_BACK);
    if (toggle && !pointerToggleWasDown_) {
        setPointerMode(!pointerMode_);
        LOG_WARNING("Gamepad: the right stick now ",
                    pointerMode_ ? "moves the pointer" : "turns the view");
    }
    pointerToggleWasDown_ = toggle;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
        const auto button = static_cast<SDL_GamepadButton>(b);
        if (button != SDL_GAMEPAD_BUTTON_LEFT_SHOULDER && button != SDL_GAMEPAD_BUTTON_BACK)
            route(button);
    }
}

void GamepadControls::applyButtons() {
    auto& bindings = KeybindingManager::getInstance();
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
        const auto& route = routes_[static_cast<size_t>(b)];
        const auto button = static_cast<SDL_GamepadButton>(b);
        const bool pointerClick = pointerMode_ && (button == SDL_GAMEPAD_BUTTON_SOUTH || button == SDL_GAMEPAD_BUTTON_WEST);
        bindings.setVirtualCommand(route.command, "pad-button" + std::to_string(b),
            route.kind == PadKeyAnswer::Kind::Command && core::gamepad().held(button) && !pointerClick);
    }
}

void GamepadControls::update(float deltaTime) {
    auto& pad = core::gamepad();
    ImGuiIO& io = ImGui::GetIO();
    // A pad is only read while the window has focus. SDL does not update a
    // controller for an unfocused window unless it is told to, so whatever
    // the stick last said stays said - and alt-tabbing mid-stride would leave
    // the character walking north for as long as the player was away.
    const bool focused =
        !window_ || (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0;
    // Two tiers, because a pad is not only a way to play: it is the only
    // input device some players have in front of them.
    //
    // The world's controls - the sticks, the action buttons, every key -
    // belong to the world. The touchpad does not: a login screen is a screen
    // with buttons on it, and a trackpad that goes dead until the player is
    // already in the world is dead exactly where someone holding a pad needs
    // it, with no way to press Login at all.
    const bool usable = enabled_ && pad.isConnected() && focused;
    const bool live = usable && inWorld_;

    // ImGui navigates its own windows with a pad, which is what the login and
    // character screens want and the opposite of what the world wants: in the
    // world the D-pad casts spells, and a panel left open would take those
    // presses as "move the highlight" as well. So the nav flag follows who is
    // driving.
    if (live) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    }

    if (!pad.isConnected()) announced_ = false;
    if (!usable) {
        // Losing the pad or switching it off ends pointer mode; losing focus
        // for a moment does not. Coming back from another window to find the
        // pointer gone would be a small betrayal of the mode the player left
        // it in.
        if (!enabled_ || !pad.isConnected()) setPointerMode(false);
        reset();
        return;
    }
    // Pointer mode is the right stick's, and the right stick is the world's.
    // The touchpad below needs no mode.
    if (!inWorld_) setPointerMode(false);

    if (!announced_) {
        announced_ = true;
        // Once per connection, at warning, because a player who has plugged a
        // pad in and is wondering whether the client saw it has exactly one
        // place to look, and that log is warnings only.
        const auto kind = pad.kind();
        const auto name = [kind](SDL_GamepadButton button) {
            return padButtonLabel(button, kind);
        };
        LOG_WARNING("Gamepad: ", pad.describe(),
                    " - left stick moves, right stick looks, triggers zoom, ",
                    name(SDL_GAMEPAD_BUTTON_SOUTH), " jumps, ",
                    name(SDL_GAMEPAD_BUTTON_EAST), " closes, ",
                    name(SDL_GAMEPAD_BUTTON_WEST), "/", name(SDL_GAMEPAD_BUTTON_NORTH),
                    " and the D-pad are actions 1-6, hold ",
                    name(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), " for 7-12, ",
                    name(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), " targets, ",
                    name(SDL_GAMEPAD_BUTTON_LEFT_STICK), " autoruns, ",
                    name(SDL_GAMEPAD_BUTTON_BACK), " gives you a pointer",
                    pad.hasButton(SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1)
                        ? ", the back buttons are actions 7-10"
                        : "",
                    pad.hasTouchpad() ? ", and the touchpad is a trackpad - click it, or click "
                                        "with two fingers to right-click"
                                      : "");
    }

    // Typing takes the keys, not the pointer. A stick nudged while typing
    // should not walk the character out of town - but a finger sliding on the
    // touchpad should still move the cursor, which is what it does on the
    // laptop this is being typed on, and at the login screen the box being
    // typed into is the very thing a pointer is needed to leave.
    const bool typing = io.WantTextInput || interfaceTakingTypedInput();

    if (!live || typing) {
        releaseKeys();
        applyTouchpad();
        applyClicks();
        return;
    }

    // Bindings first. A button the player has bound is no longer the default
    // scheme's, including Back's pointer and the bumper's Shift modifier.
    routeButtons();

    // Walking and zooming work in both modes. A pointer that stopped the
    // player from stepping back out of a fire would be worse than none.
    applyMovement(pad.leftStick().x, pad.leftStick().y);
    applyZoom(pad.rightTrigger(), pad.leftTrigger(), deltaTime);
    if (pointerMode_) {
        applyPointer(deltaTime);
    } else {
        applyLook(pad.rightStick().x, pad.rightStick().y, deltaTime);
    }
    applyTouchpad();
    applyClicks();
    applyButtons();
}

}  // namespace ui
}  // namespace wowee
