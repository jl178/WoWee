#include "input_test_support.hpp"
#include "game_replay.hpp"
#include "ui/game_screen.hpp"
#include "addons/lua_engine.hpp"
#include "addons/addon_lua_snippets.hpp"
#include "addons/lua_api_registrations.hpp"
#include "core/gamepad.hpp"
#include "core/input.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "ui/gamepad_controls.hpp"
#include "ui/framexml_emitter.hpp"
#include "ui/xml_parser.hpp"
#include <cmath>
#include <memory>
#include <set>

using namespace input_test;
namespace ui = wowee::ui;
namespace addons = wowee::addons;
namespace core = wowee::core;
namespace rendering = wowee::rendering;

namespace {
struct Integration : Fixture {
    addons::LuaEngine lua;
    Integration() {
        REQUIRE(lua.initialize());
        // Missing APIs must fail, not turn a broken test into a no-op success.
        run("setmetatable(_G, nil); __WoweeBindingScripts = {}; __WoweeBindingRunOnUp = {}");
        ui::setTypedInputProbe([this] { return lua.editBoxHasFocus(); });
    }
    ~Integration() {
        lua.releaseInput();
        ui::setTypedInputProbe({});
        core::Input::getInstance().clearVirtualKeys();
        core::Input::getInstance().update();
    }
    void run(const std::string& code) {
        const bool ok = lua.executeString(code);
        INFO(lua.lastError());
        REQUIRE(ok);
    }
    void truth(const std::string& expression) {
        INFO(expression);
        REQUIRE(lua.evaluateBoolean(expression));
    }
    void layout() { lua.widgets().layout(800, 600); }
};

// Feed SDL's real joystick state, not synthetic button events: Gamepad polls
// SDL_GetGamepadButton/Axis. The tests fail if virtual-device setup is broken.
struct VirtualPad {
    SDL_JoystickID id = 0;
    SDL_Joystick* joystick = nullptr;
    std::optional<std::string> oldBackground;
    float previousDeadzone = core::gamepad().stickDeadzone();
    bool initialized = false;
    VirtualPad() {
        if (const char* old = SDL_GetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS)) oldBackground = old;
    }
    // Initialize after construction so failed REQUIREs still unwind this
    // member and restore SDL resources/hints before Catch runs another case.
    void initialize() {
        REQUIRE(SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"));
        initialized = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
        REQUIRE(initialized);
        SDL_VirtualJoystickDesc desc{};
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.name = "WoWee integration virtual controller";
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
        desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
        id = SDL_AttachVirtualJoystick(&desc);
        INFO(SDL_GetError());
        REQUIRE(id != 0);
        REQUIRE(SDL_IsGamepad(id));
        joystick = SDL_OpenJoystick(id);
        REQUIRE(joystick != nullptr);
        // Select this id explicitly, even when a physical pad is plugged in.
        core::gamepad().shutdown();
        SDL_Event added{}; added.type = SDL_EVENT_GAMEPAD_ADDED; added.cdevice.which = id;
        core::gamepad().handleEvent(added);
        REQUIRE(core::gamepad().isConnected());
        REQUIRE(core::gamepad().name() == desc.name);
        core::gamepad().setStickDeadzone(0.0f);
        SDL_UpdateJoysticks();
        core::gamepad().update();
    }
    ~VirtualPad() {
        core::gamepad().shutdown();
        core::gamepad().setStickDeadzone(previousDeadzone);
        if (joystick) SDL_CloseJoystick(joystick);
        if (id) SDL_DetachVirtualJoystick(id);
        if (initialized) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        if (oldBackground) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, oldBackground->c_str());
        else SDL_ResetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
    }
    void button(SDL_GamepadButton button, bool down) {
        INFO(SDL_GetError());
        REQUIRE(SDL_SetJoystickVirtualButton(joystick, button, down));
    }
    void axis(SDL_GamepadAxis axis, float value) {
        REQUIRE(SDL_SetJoystickVirtualAxis(joystick, axis, static_cast<Sint16>(value * 32767)));
    }
    void sample() { SDL_UpdateJoysticks(); core::gamepad().update(); }
};
struct PadIntegration : Integration {
    VirtualPad pad;
    ui::GamepadControls controls;
    PadIntegration() {
        pad.initialize();
        controls.setInWorld(true);
        controls.setKeyRouter([this](const char* key, bool down) {
            const auto outcome = lua.dispatchPadKey(key, down);
            ui::PadKeyAnswer answer;
            answer.command = outcome.command;
            answer.kind = !outcome.command.empty() ? ui::PadKeyAnswer::Kind::Command :
                outcome.taken ? ui::PadKeyAnswer::Kind::Taken : ui::PadKeyAnswer::Kind::Unbound;
            return answer;
        });
    }
    ~PadIntegration() { controls.reset(); }
    void pump() { manager.beginInputFrame(); pad.sample(); controls.update(1.0f / 60); }
    void press(SDL_GamepadButton button, bool down = true) { pad.button(button, down); pump(); }
};
} // namespace

TEST_CASE_METHOD(Integration, "Real Lua binding scripts latch their command and release function", "[input][lua][O4]") {
    REQUIRE(manager.setBinding("BUTTON2", "TEST_RIGHT"));
    REQUIRE(manager.setBinding("BUTTON3", "TEST_MIDDLE"));
    run(R"lua(
        calls = {}
        __WoweeBindingScripts.TEST_RIGHT = function(down) table.insert(calls, down == 'down' and 'right-down' or 'right-up') end
        __WoweeBindingScripts.TEST_MIDDLE = function(down) table.insert(calls, down == 'down' and 'middle-down' or 'middle-up') end
        __WoweeBindingRunOnUp.TEST_RIGHT = true
        __WoweeBindingRunOnUp.TEST_MIDDLE = true
    )lua");
    REQUIRE(lua.dispatchBindingInput("BUTTON2", false, false, false, true));
    REQUIRE(lua.dispatchBindingInput("BUTTON2", false, false, false, true));
    REQUIRE(manager.setBinding("BUTTON2", "TEST_MIDDLE"));
    run("__WoweeBindingScripts.TEST_RIGHT = function() error('new function must not receive old release') end");
    REQUIRE(lua.dispatchBindingInput("BUTTON2", true, true, false, false));
    truth("#calls == 2 and calls[1] == 'right-down' and calls[2] == 'right-up'");
    REQUIRE(lua.dispatchBindingInput("BUTTON3", false, false, false, true));
    lua.releaseInput();
    truth("#calls == 4 and calls[3] == 'middle-down' and calls[4] == 'middle-up'");
    CHECK_FALSE(lua.dispatchBindingInput("BUTTON3", false, false, false, false));
}

namespace {
// The CVar store is the process's, so a test that sets the CVar puts the
// default back however it leaves.
struct KeyDownCVar {
    explicit KeyDownCVar(const char* value) { addons::setStoredCVar("ActionButtonUseKeyDown", value); }
    ~KeyDownCVar() {
        addons::setStoredCVar("ActionButtonUseKeyDown", addons::kActionButtonUseKeyDownDefault);
    }
};
} // namespace

TEST_CASE_METHOD(Integration, "Action bar keys run their Bindings.xml scripts: pushed on press, cast on release", "[input][lua][timing]") {
    // 3.3.5a's ACTIONBUTTONn is runOnUp: ActionButtonDown pushes the button,
    // ActionButtonUp casts. They run in the pump like any interface binding,
    // not from the client's late-frame poll (docs/re/input-timing.md). The
    // stock timing is ActionButtonUseKeyDown 0; this client's default is 1.
    CHECK(addons::LuaEngine::interfaceRunsFirst("ACTIONBUTTON1"));
    CHECK(addons::LuaEngine::interfaceRunsFirst("MULTIACTIONBAR1BUTTON12"));
    CHECK(addons::LuaEngine::interfaceRunsFirst("SHAPESHIFTBUTTON1"));
    CHECK_FALSE(addons::LuaEngine::interfaceRunsFirst("MOVEFORWARD"));
    const KeyDownCVar stock("0");
    REQUIRE(manager.setBinding("1", "ACTIONBUTTON1"));
    run(R"lua(
        calls = {}
        __WoweeBindingScripts.ACTIONBUTTON1 = function(state) table.insert(calls, state) end
        __WoweeBindingRunOnUp.ACTIONBUTTON1 = true
    )lua");
    REQUIRE(lua.dispatchBindingInput("1", false, false, false, true));
    truth("#calls == 1 and calls[1] == 'down'");
    REQUIRE(lua.dispatchBindingInput("1", false, false, false, false));
    truth("#calls == 2 and calls[2] == 'up'");
}

TEST_CASE_METHOD(Integration, "Action bar keys cast on the press by default", "[input][lua][timing]") {
    // ActionButtonUseKeyDown is on unless the player turns it off: the press
    // runs down and up back to back, and the release adds nothing. GetCVar
    // and GetCVarDefault say so, so the interface's own option shows it.
    truth("GetCVar('ActionButtonUseKeyDown') == '1'");
    truth("GetCVarDefault('ActionButtonUseKeyDown') == '1'");
    truth("GetCVarBool('ActionButtonUseKeyDown')");
    REQUIRE(manager.setBinding("1", "ACTIONBUTTON1"));
    run(R"lua(
        calls = {}
        __WoweeBindingScripts.ACTIONBUTTON1 = function(state) table.insert(calls, state) end
        __WoweeBindingRunOnUp.ACTIONBUTTON1 = true
    )lua");
    REQUIRE(lua.dispatchBindingInput("1", false, false, false, true));
    truth("#calls == 2 and calls[1] == 'down' and calls[2] == 'up'");
    REQUIRE(lua.dispatchBindingInput("1", false, false, false, false));
    truth("#calls == 2");

    // And 0 takes it back to the release, from SetCVar as a player would.
    const KeyDownCVar restore(addons::kActionButtonUseKeyDownDefault);
    run("SetCVar('ActionButtonUseKeyDown', '0')");
    truth("GetCVar('ActionButtonUseKeyDown') == '0'");
    REQUIRE(lua.dispatchBindingInput("1", false, false, false, true));
    truth("#calls == 3 and calls[3] == 'down'");
    REQUIRE(lua.dispatchBindingInput("1", false, false, false, false));
    truth("#calls == 4 and calls[4] == 'up'");
}

TEST_CASE_METHOD(Integration, "Tab runs TargetNearestEnemy from the key event", "[input][lua][timing]") {
    // Stock selects and sends CMSG_SET_SELECTION from the binding's script,
    // run from the key event; the client's own poll ran in the UI draw.
    CHECK(addons::LuaEngine::interfaceRunsFirst("TARGETNEARESTENEMY"));
    CHECK_FALSE(addons::LuaEngine::interfaceRunsFirst("JUMP"));
    REQUIRE(manager.setBinding("TAB", "TARGETNEARESTENEMY"));
    run(R"lua(
        calls = {}
        __WoweeBindingScripts.TARGETNEARESTENEMY = function(state) table.insert(calls, state or 'down') end
    )lua");
    REQUIRE(lua.dispatchBindingInput("TAB", false, false, false, true));
    truth("#calls == 1");
}

TEST_CASE_METHOD(Integration, "Keyboard frames use layout keys while binding capture uses physical keys", "[input][lua][O7]") {
    run(R"lua(
        dialog = CreateFrame('Frame', 'LayoutDialog', UIParent)
        dialog:SetSize(300, 200); dialog:SetPoint('CENTER')
        dialog:EnableKeyboard(true)
        dialog:SetScript('OnKeyDown', function(self, key) lastKey = key end)
        dialog:Show()
    )lua");
    layout();
    REQUIRE(lua.keyboardFrameBlocksInput());
    REQUIRE(lua.dispatchFrameKey(SDLK_Z, true, SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_W)));
    truth("lastKey == 'Z'");
    lua.dispatchFrameKey(SDLK_Z, false, SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_W));
    run("dialog:SetPropagateKeyboardInput(true)");
    CHECK_FALSE(lua.keyboardFrameBlocksInput());
    CHECK_FALSE(lua.dispatchFrameKey(SDLK_Y, true));
    truth("lastKey == 'Y'");
    lua.dispatchFrameKey(SDLK_Y, false);
    run("dialog:Hide()"); layout();
    CHECK_FALSE(lua.dispatchFrameKey(SDLK_C, true));
    run(R"lua(
        capture = CreateFrame('Frame', 'KeyBindingFrame', UIParent)
        capture:SetSize(300, 200); capture:SetPoint('CENTER'); capture:EnableKeyboard(true)
        capture:SetScript('OnKeyDown', function(self, key) lastKey = key end)
        capture:Show()
    )lua");
    layout();
    REQUIRE(lua.dispatchFrameKey(SDLK_Z, true, SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_W)));
    truth("lastKey == 'W'");
    lua.dispatchFrameKey(SDLK_Z, false, SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_W));
    run("capture:EnableKeyboard(false)"); layout();
    CHECK_FALSE(lua.keyboardFrameBlocksInput());
    CHECK_FALSE(lua.dispatchFrameKey(SDLK_C, true));
}

TEST_CASE_METHOD(Integration, "Real edit boxes accept layout text and Return or keypad Enter", "[input][lua][O7][O9]") {
    run(R"lua(
        edit = CreateFrame('EditBox', 'IntegrationChat', UIParent)
        edit:SetAutoFocus(false); edit:SetSize(300, 30); edit:SetPoint('CENTER'); edit:Show()
        sent = 0
        edit:SetScript('OnEnterPressed', function(self) sent = sent + 1; message = self:GetText(); self:ClearFocus() end)
    )lua");
    layout();
    for (auto code : {SDLK_RETURN, SDLK_KP_ENTER}) {
        run("edit:SetText(''); edit:SetFocus()");
        REQUIRE(lua.editBoxHasFocus());
        lua.dispatchText("/say azerty qwertz");
        lua.dispatchKey(code, false);
        CHECK_FALSE(lua.editBoxHasFocus());
        truth("message == '/say azerty qwertz'");
    }
    truth("sent == 2");
}

TEST_CASE_METHOD(Integration, "Real wheel handlers own fractional UI travel and camera keeps fractional zoom", "[input][lua][camera][O8]") {
    run(R"lua(
        scroll = CreateFrame('Frame', 'IntegrationWheel', UIParent)
        scroll:SetSize(400, 300); scroll:SetPoint('CENTER'); scroll:EnableMouseWheel(true); scroll:Show()
        wheel = 0; scroll:SetScript('OnMouseWheel', function(self, delta) wheel = wheel + delta end)
    )lua");
    layout();
    for (int i = 0; i < 8; ++i) REQUIRE(lua.dispatchMouseWheel(400, 300, 0.125f));
    truth("wheel == 1");
    REQUIRE(lua.dispatchMouseWheel(400, 300, -0.125f));
    truth("wheel == 1");
    run("scroll:Hide()"); layout();
    CHECK_FALSE(lua.dispatchMouseWheel(400, 300, 0.125f));
    rendering::Camera camera;
    rendering::CameraController controller(&camera);
    glm::vec3 target(0);
    controller.setFollowTarget(&target);
    controller.setIdleOrbitEnabled(false);
    controller.setGravityDisabled(true);
    controller.update(0.05f);
    const auto before = camera.getPosition();
    controller.processMouseWheel(0.125f);
    controller.update(0.05f);
    // This observes the actual camera, not a duplicated zoom formula.
    CHECK(glm::length(camera.getPosition() - before) > 0.0001f);
}

TEST_CASE_METHOD(Integration, "Real camera applies temporary Ctrl walking and a rebindable toggle", "[input][camera][O6]") {
    rendering::Camera camera;
    rendering::CameraController controller(&camera);
    controller.setIdleOrbitEnabled(false);
    controller.setFollowTarget(nullptr);
    controller.setUseWoWSpeed(true);
    camera.setPosition({0, 0, 0}); // Measure speed without large-world float quantization.
    auto& input = core::Input::getInstance();
    const auto speed = [&] {
        const auto before = camera.getPosition();
        input.update(); controller.update(0.01f);
        const auto change = camera.getPosition() - before;
        return glm::length(glm::vec2(change.x, change.y)) / 0.01f;
    };
    event(SDL_SCANCODE_W);
    CHECK(speed() == Catch::Approx(7.0f).margin(0.01));
    for (auto ctrl : {SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL}) {
        input.setVirtualKey(ctrl, true);
        CHECK(speed() == Catch::Approx(2.5f).margin(0.01));
        input.setVirtualKey(ctrl, false);
        CHECK(speed() == Catch::Approx(7.0f).margin(0.01));
    }
    event(SDL_SCANCODE_KP_DIVIDE);
    CHECK(speed() == Catch::Approx(2.5f).margin(0.01));
    manager.beginInputFrame(); event(SDL_SCANCODE_KP_DIVIDE, true, SDL_KMOD_NONE, true);
    CHECK(speed() == Catch::Approx(2.5f).margin(0.01));
    event(SDL_SCANCODE_KP_DIVIDE, false);
    manager.beginInputFrame(); event(SDL_SCANCODE_KP_DIVIDE);
    CHECK(speed() == Catch::Approx(7.0f).margin(0.01));
    manager.beginInputFrame(); event(SDL_SCANCODE_KP_DIVIDE, false);
    controller.setRunSpeedOverride(10); controller.setWalkSpeedOverride(3);
    CHECK(speed() == Catch::Approx(10.0f).margin(0.01));
    input.setVirtualKey(SDL_SCANCODE_LCTRL, true);
    CHECK(speed() == Catch::Approx(3.0f).margin(0.01));
    input.setVirtualKey(SDL_SCANCODE_LCTRL, false);
    REQUIRE(manager.setBinding("CTRL-W", "ADDON_CUSTOM"));
    event(SDL_SCANCODE_W, false); event(SDL_SCANCODE_W, true, SDL_KMOD_CTRL);
    CHECK(speed() == Catch::Approx(0).margin(0.01));
    event(SDL_SCANCODE_W, false);
    REQUIRE(manager.setBinding("CTRL-F8", "TOGGLERUN"));
    manager.beginInputFrame(); event(SDL_SCANCODE_F8, true, SDL_KMOD_CTRL); event(SDL_SCANCODE_F8, false);
    controller.update(0);
    manager.beginInputFrame(); event(SDL_SCANCODE_W);
    CHECK(speed() == Catch::Approx(3.0f).margin(0.01));
    event(SDL_SCANCODE_W, false); event(SDL_SCANCODE_S);
    controller.setRunBackSpeedOverride(4);
    // Walking backward is min(walk, run back) (Wow.exe 0x987570): still walking.
    CHECK(speed() == Catch::Approx(3.0f).margin(0.01));
    event(SDL_SCANCODE_S, false); event(SDL_SCANCODE_W);
    manager.beginInputFrame();
    controller.setUseWoWSpeed(false);
    CHECK(speed() == Catch::Approx(50.0f).margin(0.01));
    input.setVirtualKey(SDL_SCANCODE_LCTRL, true);
    CHECK(speed() == Catch::Approx(15.0f).margin(0.01));
    input.setVirtualKey(SDL_SCANCODE_LCTRL, false);
    input.setVirtualKey(SDL_SCANCODE_LSHIFT, true);
    CHECK(speed() == Catch::Approx(150.0f).margin(0.01));
    input.setVirtualKey(SDL_SCANCODE_LCTRL, true);
    CHECK(speed() == Catch::Approx(45.0f).margin(0.01));
}

TEST_CASE_METHOD(Integration, "Real camera sitting preserves queued taps and wheel command edges", "[input][camera][sit]") {
    rendering::Camera camera;
    rendering::CameraController controller(&camera);
    glm::vec3 target(0);
    controller.setIdleOrbitEnabled(false);
    controller.setFollowTarget(&target);
    controller.setGravityDisabled(true);
    CHECK_FALSE(controller.isSitting());
    // Both transitions precede the consumer: held state is already false.
    event(SDL_SCANCODE_X); event(SDL_SCANCODE_X, false);
    REQUIRE(manager.isCommandPressed("SITORSTAND"));
    REQUIRE_FALSE(manager.isCommandDown("SITORSTAND"));
    controller.update(0);
    CHECK(controller.isSitting());
    manager.beginInputFrame(); controller.update(0);
    CHECK(controller.isSitting());
    event(SDL_SCANCODE_X); controller.update(0);
    CHECK_FALSE(controller.isSitting());
    manager.beginInputFrame(); controller.update(0);
    CHECK_FALSE(controller.isSitting()); // Holding is not another toggle.
    event(SDL_SCANCODE_X, false);
    REQUIRE(manager.setBinding("MOUSEWHEELUP", "SITORSTAND"));
    for (bool expected : {true, false, true}) {
        manager.beginInputFrame();
        // Application's wheel route pulses this real input source in one pump.
        const auto command = manager.resolveCommand("MOUSEWHEELUP");
        manager.setVirtualCommand(command, "mouse-wheel", true);
        manager.setVirtualCommand(command, "mouse-wheel", false);
        REQUIRE(manager.isCommandPressed("SITORSTAND"));
        REQUIRE_FALSE(manager.isCommandDown("SITORSTAND"));
        controller.update(0);
        CHECK(controller.isSitting() == expected);
        manager.beginInputFrame(); controller.update(0);
        CHECK(controller.isSitting() == expected);
    }
}

TEST_CASE_METHOD(Integration, "R resets only real camera angles and respects capture and rebindings", "[input][camera][O6]") {
    rendering::Camera camera;
    rendering::CameraController controller(&camera);
    controller.setIdleOrbitEnabled(false);
    controller.setDefaultSpawn(glm::vec3(99), 37, -12);
    controller.setFollowTarget(nullptr);
    controller.resetAngles();
    camera.setPosition(glm::vec3(10, 20, 30));
    controller.applyLookDelta(50, 30);
    event(SDL_SCANCODE_GRAVE); controller.update(0);
    event(SDL_SCANCODE_GRAVE, false); manager.beginInputFrame();
    const auto position = camera.getPosition();
    const bool grounded = controller.isGrounded(), sitting = controller.isSitting(), autorun = controller.isAutoRunning();
    const bool firstPerson = controller.isFirstPersonView();
    event(SDL_SCANCODE_R); controller.update(0);
    CHECK(controller.getYaw() == Catch::Approx(37));
    CHECK(controller.getFacingYaw() == Catch::Approx(37));
    CHECK(controller.getPitch() == Catch::Approx(-12));
    CHECK(camera.getPosition() == position);
    CHECK(controller.isGrounded() == grounded);
    CHECK(controller.isSitting() == sitting);
    CHECK(controller.isAutoRunning() == autorun);
    CHECK(controller.isFirstPersonView() == firstPerson);
    controller.applyLookDelta(50, 30);
    const auto yaw = controller.getYaw();
    manager.beginInputFrame(); event(SDL_SCANCODE_R, true, SDL_KMOD_NONE, true); controller.update(0);
    CHECK(controller.getYaw() == yaw);
    event(SDL_SCANCODE_R, false);
    ImGui::GetIO().WantTextInput = true;
    manager.beginInputFrame(); event(SDL_SCANCODE_R); controller.update(0);
    CHECK(controller.getYaw() == yaw);
    ImGui::GetIO().WantTextInput = false; controller.update(0);
    CHECK(controller.getYaw() == yaw);
    event(SDL_SCANCODE_R, false);
    REQUIRE(manager.setBinding("CTRL-R", "ADDON_CUSTOM"));
    manager.beginInputFrame(); event(SDL_SCANCODE_R, true, SDL_KMOD_CTRL); controller.update(0);
    CHECK(controller.getYaw() == yaw);
    event(SDL_SCANCODE_R, false);
    REQUIRE(manager.setBinding("CTRL-F8", "WOWEERESETCAMERA"));
    REQUIRE(manager.saveBindings(path("reset-camera.cfg").string()));
    manager.resetToDefaults(); REQUIRE(manager.loadBindings(path("reset-camera.cfg").string()));
    manager.beginInputFrame(); event(SDL_SCANCODE_F8, true, SDL_KMOD_CTRL); controller.update(0);
    CHECK(controller.getYaw() == Catch::Approx(37));
}

TEST_CASE_METHOD(PadIntegration, "Real SDL pad defaults work immediately and LB fallback survives keyboard focus", "[input][pad][R1][O1][O2][O5]") {
    run(R"lua(
        popup = CreateFrame('Frame', 'IntegrationPopup', UIParent)
        popup:SetSize(300, 200); popup:SetPoint('CENTER'); popup:EnableKeyboard(true); popup:Show()
    )lua");
    layout();
    REQUIRE(lua.keyboardFrameBlocksInput());
    press(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    for (auto button : {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                       SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK}) {
        const auto command = manager.commandForKey(ui::padKeyName(button));
        REQUIRE_FALSE(command.empty());
        press(button);
        CHECK(manager.isCommandPressed(command));
        CHECK(manager.isCommandDown(command));
        press(button, false);
        CHECK_FALSE(manager.isCommandDown(command));
    }
    for (const auto& row : ui::padBindings()) {
        const std::string command = row.command ? row.command : "";
        if (!command.starts_with("ACTIONBUTTON")) continue;
        press(row.button);
        CHECK(manager.isCommandPressed("MULTIACTIONBAR1BUTTON" + command.substr(12)));
        press(row.button, false);
    }
    press(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
    // Explicit pad chords take precedence over the modifier fallback.
    REQUIRE(manager.setBinding("SHIFT-PAD1", "ACTIONBUTTON12"));
    press(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER); press(SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(manager.isCommandPressed("ACTIONBUTTON12"));
    press(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
    CHECK(manager.isCommandDown("ACTIONBUTTON12"));
    CHECK_FALSE(manager.isCommandDown("JUMP"));
    press(SDL_GAMEPAD_BUTTON_SOUTH, false);
    CHECK_FALSE(manager.isCommandDown("ACTIONBUTTON12"));
    REQUIRE(manager.setBinding("PAD1"));
    press(SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK_FALSE(manager.isCommandDown("JUMP"));
    press(SDL_GAMEPAD_BUTTON_SOUTH, false);
    // Run the press through the actual GameScreen escape consumer. With
    // nothing open there is nothing for it to close; what it must do is take
    // the press without falling over. The Blizzard map/popup cascade still
    // requires extracted FrameXML.
    replay::Session session;
    ui::GameScreen screen;
    press(SDL_GAMEPAD_BUTTON_EAST);
    REQUIRE(manager.isCommandPressed("TOGGLEGAMEMENU"));
    ImGui::NewFrame();
    screen.render(session.handler());
    ImGui::EndFrame();
}

TEST_CASE_METHOD(PadIntegration, "Pad scripts release original bindings and reset leaves other sources alone", "[input][pad][lua]") {
    REQUIRE(manager.setBinding("PAD1", "TEST_PAD"));
    run(R"lua(
        downs = 0; ups = 0
        __WoweeBindingScripts.TEST_PAD = function(down) if down == 'down' then downs = downs + 1 else ups = ups + 1 end end
        __WoweeBindingRunOnUp.TEST_PAD = true
    )lua");
    press(SDL_GAMEPAD_BUTTON_SOUTH);
    truth("downs == 1 and ups == 0");
    pump(); truth("downs == 1");
    REQUIRE(manager.setBinding("PAD1", "JUMP"));
    press(SDL_GAMEPAD_BUTTON_SOUTH, false);
    truth("ups == 1");
    REQUIRE(manager.setBinding("PAD1", "TEST_PAD"));
    press(SDL_GAMEPAD_BUTTON_SOUTH);
    manager.setVirtualCommand("MOVEFORWARD", "touch", true);
    controls.reset();
    truth("downs == 2 and ups == 2");
    CHECK(manager.isCommandDown("MOVEFORWARD"));
    manager.setVirtualCommand("MOVEFORWARD", "touch", false);
    press(SDL_GAMEPAD_BUTTON_SOUTH, false);
    pad.axis(SDL_GAMEPAD_AXIS_LEFTY, -0.31f); pump();
    CHECK(manager.isCommandDown("MOVEFORWARD"));
    pad.axis(SDL_GAMEPAD_AXIS_LEFTY, -0.25f); pump();
    CHECK(manager.isCommandDown("MOVEFORWARD"));
    pad.axis(SDL_GAMEPAD_AXIS_LEFTY, -0.19f); pump();
    CHECK_FALSE(manager.isCommandDown("MOVEFORWARD"));
    pad.axis(SDL_GAMEPAD_AXIS_LEFTY, 0);
    pad.axis(SDL_GAMEPAD_AXIS_LEFTX, 0.46f); pump();
    CHECK(manager.isCommandDown("STRAFERIGHT"));
    pad.axis(SDL_GAMEPAD_AXIS_LEFTX, 0.34f); pump();
    CHECK_FALSE(manager.isCommandDown("STRAFERIGHT"));
    event(SDL_SCANCODE_W);
    controls.reset();
    CHECK(manager.isCommandDown("MOVEFORWARD"));
}

TEST_CASE_METHOD(Integration, "The real GameScreen idle and action path stays live across rebinds and page changes", "[input][screen][O10]") {
    replay::Session session;
    session.spawnPlayer({{wowee::game::UF::UNIT_FIELD_HEALTH, 100}});
    // No extracted DBCs are needed to send a spell; keep metadata loading out
    // of this input test, exactly as the packet-replay suite does.
    session.handler().spellNameCacheLoadedRef() = true;
    ui::GameScreen screen;
    const auto render = [&] {
        ImGui::NewFrame();
        screen.render(session.handler());
        ImGui::EndFrame();
    };
    const auto casts = [&] { return session.sent().of(wowee::game::Opcode::CMSG_CAST_SPELL); };
    render(); // Warm the real UI/static command names, not a copied poll loop.
    const auto initial = casts().size();
    for (int i = 0; i < 8; ++i) { manager.beginInputFrame(); render(); }
    CHECK(casts().size() == initial);
    for (int page : {1, 2, 6}) {
        screen.actionBarPanel().setMainActionBarPage(page);
        for (int i = 0; i < wowee::game::GameHandler::SLOTS_PER_BAR; ++i) {
            const auto command = (page == 6 ? "MULTIACTIONBAR1BUTTON" : "ACTIONBUTTON") + std::to_string(i + 1);
            const uint32_t spell = 10000 + page * 100 + i;
            const auto slot = ui::ActionBarPanel::actionSlotForPage(page, i);
            session.handler().setActionBarSlot(slot, wowee::game::ActionBarSlot::SPELL, spell);
            REQUIRE(manager.setBinding("F10", command));
            manager.beginInputFrame(); event(SDL_SCANCODE_F10); event(SDL_SCANCODE_F10, false);
            const auto before = casts().size();
            render();
            const auto sent = casts();
            REQUIRE(sent.size() == before + 1);
            // WotLK CMSG_CAST_SPELL starts with castCount, then uint32 spell.
            const auto& bytes = sent.back().getData();
            REQUIRE(bytes.size() >= 5);
            const uint32_t actual = uint32_t(bytes[1]) | uint32_t(bytes[2]) << 8 |
                uint32_t(bytes[3]) << 16 | uint32_t(bytes[4]) << 24;
            CHECK(actual == spell);
            manager.beginInputFrame(); render();
            CHECK(casts().size() == before + 1);
        }
    }
    // Main and bottom commands at the same index are independent presses.
    screen.actionBarPanel().setMainActionBarPage(2);
    session.handler().setActionBarSlot(ui::ActionBarPanel::actionSlotForPage(2, 0), wowee::game::ActionBarSlot::SPELL, 22001);
    session.handler().setActionBarSlot(ui::ActionBarPanel::actionSlotForPage(6, 0), wowee::game::ActionBarSlot::SPELL, 26001);
    REQUIRE(manager.setBinding("F9", "ACTIONBUTTON1"));
    REQUIRE(manager.setBinding("F10", "MULTIACTIONBAR1BUTTON1"));
    manager.beginInputFrame();
    event(SDL_SCANCODE_F9); event(SDL_SCANCODE_F9, false);
    event(SDL_SCANCODE_F10); event(SDL_SCANCODE_F10, false);
    const auto simultaneousBefore = casts().size();
    render();
    const auto simultaneous = casts();
    REQUIRE(simultaneous.size() == simultaneousBefore + 2);
    std::set<uint32_t> simultaneousSpells;
    for (size_t i = simultaneousBefore; i < simultaneous.size(); ++i) {
        const auto& bytes = simultaneous[i].getData();
        REQUIRE(bytes.size() >= 5);
        simultaneousSpells.insert(uint32_t(bytes[1]) | uint32_t(bytes[2]) << 8 |
            uint32_t(bytes[3]) << 16 | uint32_t(bytes[4]) << 24);
    }
    CHECK((simultaneousSpells == std::set<uint32_t>{22001, 26001}));
    manager.beginInputFrame(); render();
    CHECK(casts().size() == simultaneousBefore + 2);
    const auto before = casts().size();
    run("edit = CreateFrame('EditBox', 'ActionCapture', UIParent); edit:SetSize(200,30); edit:SetPoint('CENTER'); edit:Show(); edit:SetFocus()");
    layout();
    REQUIRE(lua.editBoxHasFocus());
    manager.beginInputFrame(); event(SDL_SCANCODE_F10); event(SDL_SCANCODE_F10, false); render();
    CHECK(casts().size() == before);
}

TEST_CASE_METHOD(Integration, "Emitted custom native commands have real labels and do not execute Lua actions", "[input][lua][bindings]") {
    ui::XmlNode root;
    std::string error;
    REQUIRE(ui::parseXml("<Bindings><Binding name='CAMERAZOOMOUT'>CameraZoomOut();</Binding><Binding name='SCREENSHOT' header='MISC'>Screenshot();</Binding></Bindings>", root, error));
    const auto emitted = ui::emitFrameXml(root);
    REQUIRE(emitted.warnings.empty());
    run(emitted.lua);
    truth("BINDING_NAME_WOWEERESETCAMERA == 'Reset Camera Angles'");
    truth("BINDING_NAME_WOWEETOGGLERECORDING == 'Start/Stop Video Recording'");
    CHECK(addons::clientActsOnBinding("WOWEERESETCAMERA"));
    CHECK(addons::clientActsOnBinding("WOWEETOGGLERECORDING"));
    run("__WoweeBindingScripts.WOWEERESETCAMERA('down'); __WoweeBindingScripts.WOWEETOGGLERECORDING('down')");
    manager.beginInputFrame(); event(SDL_SCANCODE_PRINTSCREEN, true, SDL_KMOD_SHIFT); event(SDL_SCANCODE_PRINTSCREEN, false);
    CHECK(manager.isCommandPressed("WOWEETOGGLERECORDING"));
    CHECK_FALSE(manager.isCommandPressed("SCREENSHOT"));
    manager.beginInputFrame(); event(SDL_SCANCODE_PRINTSCREEN); event(SDL_SCANCODE_PRINTSCREEN, false);
    CHECK(manager.isCommandPressed("SCREENSHOT"));
    CHECK_FALSE(manager.isCommandPressed("WOWEETOGGLERECORDING"));
}

TEST_CASE_METHOD(Integration, "Controller editor queries never replace addon keyboard queries", "[input][lua][bindings][editor]") {
    ui::XmlNode root;
    std::string error;
    REQUIRE(ui::parseXml("<Bindings><Binding name='TOGGLEAUTORUN'>ToggleAutoRun();</Binding><Binding name='JUMP'>JumpOrAscendStart();</Binding></Bindings>", root, error));
    run(ui::emitFrameXml(root).lua);
    truth("GetBindingKey('TOGGLEAUTORUN') == '`' and select(2, GetBindingKey('TOGGLEAUTORUN')) == 'NUMLOCK'");
    truth("WoweeGetControllerBindingKey('TOGGLEAUTORUN') == 'PADLSTICK'");
    run(R"lua(
        for i = 1, GetNumBindings() do
            local command, key1, key2 = GetBinding(i)
            if command == 'TOGGLEAUTORUN' then
                assert(key1 == '`' and key2 == 'NUMLOCK')
                local padCommand, pad1 = WoweeGetControllerBinding(i)
                assert(padCommand == command and pad1 == 'PADLSTICK')
                foundAutorun = true
            end
        end
        assert(foundAutorun)
        assert(SetBinding('F9', 'TOGGLEAUTORUN'))
        assert(SetBinding('PADLSTICK', 'JUMP'))
    )lua");
    const auto keyboard = manager.bindings();
    run("WoweeResetControllerBindings()");
    CHECK(manager.bindings() == keyboard);
    truth("WoweeGetControllerBindingKey('TOGGLEAUTORUN') == 'PADLSTICK'");
    REQUIRE(manager.setBinding("PAD1", "ACTIONBUTTON12"));
    const auto controller = manager.controllerBindings();
    run("WoweeResetKeyboardBindings()");
    CHECK(manager.controllerBindings() == controller);
    truth("GetBindingKey('TOGGLEAUTORUN') == '`' and select(2, GetBindingKey('TOGGLEAUTORUN')) == 'NUMLOCK'");
    truth("GetBindingAction('`') == 'TOGGLEAUTORUN'");
    run("assert(SaveBindings(1))");
    const auto savedKeyboard = manager.bindings(), savedController = manager.controllerBindings();
    manager.resetToDefaults();
    REQUIRE(manager.loadBindings(path("bindings.cfg").string()));
    CHECK(manager.bindings() == savedKeyboard);
    CHECK(manager.controllerBindings() == savedController);
}

TEST_CASE_METHOD(Integration, "Existing binding editor isolates device capture, reset, Save and Cancel", "[input][lua][editor]") {
    run(R"lua(
        KeyBindingFrame = CreateFrame('Button', 'KeyBindingFrame', UIParent)
        KeyBindingFrame:SetSize(600, 500); KeyBindingFrame:SetPoint('CENTER')
        KeyBindingFrame:EnableKeyboard(true)
        KeyBindingFrameOutputText = KeyBindingFrame:CreateFontString(nil)
        KeyBindingFrameUnbindButton = CreateFrame('Button', nil, KeyBindingFrame)
        function KeyBindingFrame_SetSelected(command) KeyBindingFrame.selected = command end
        function KeyBindingFrame_Update()
            shownKey1, shownKey2 = GetBindingKey('TOGGLEAUTORUN')
        end
        function KeyBindingFrame_OnKeyDown(self, key)
            if key == 'ESCAPE' then LoadBindings(1); self:Hide() end
        end
        KEY_BOUND = 'Bound'
        assert(SaveBindings(1))
    )lua");
    run(addons::kBindingDeviceEditor);
    // Reinstalling after an addon load is harmless, never makes two selectors.
    run(addons::kBindingDeviceEditor);
    run(R"lua(
        KeyBindingFrame_Update()
        assert(shownKey1 == '`' and shownKey2 == 'NUMLOCK')
        WoweeBindingDeviceSelector:GetScript('OnClick')(WoweeBindingDeviceSelector)
        assert(shownKey1 == 'PADLSTICK' and shownKey2 == nil)
        assert(GetBindingKey('TOGGLEAUTORUN') == '`')
        KeyBindingFrame.selected = 'TOGGLEAUTORUN'; KeyBindingFrame.keyID = 2
        KeyBindingFrame_OnKeyDown(KeyBindingFrame, 'F8') -- wrong device
        assert(KeyBindingFrame.selected == 'TOGGLEAUTORUN')
        KeyBindingFrame_OnKeyDown(KeyBindingFrame, 'PAD4')
        assert(shownKey1 == 'PADLSTICK' and shownKey2 == 'PAD4')
        assert(GetBindingKey('TOGGLEAUTORUN') == '`')
        KeyBindingFrame.selected = 'TOGGLEAUTORUN'; KeyBindingFrame.keyID = 1
        KeyBindingFrameUnbindButton:GetScript('OnClick')()
        assert(shownKey1 == nil and shownKey2 == 'PAD4')
        assert(SaveBindings(1))
        getfenv(KeyBindingFrame_Update).LoadBindings(0) -- reset selected device
        KeyBindingFrame_Update()
        assert(shownKey1 == 'PADLSTICK' and shownKey2 == nil)
        assert(GetBindingKey('TOGGLEAUTORUN') == '`')
        KeyBindingFrame_OnKeyDown(KeyBindingFrame, 'ESCAPE') -- Cancel restores both groups
        KeyBindingFrame_Update()
        assert(shownKey1 == nil and shownKey2 == 'PAD4')
        WoweeBindingDeviceSelector:GetScript('OnClick')(WoweeBindingDeviceSelector)
        KeyBindingFrame.selected = 'TOGGLEAUTORUN'; KeyBindingFrame.keyID = 2
        KeyBindingFrame_OnKeyDown(KeyBindingFrame, 'F8')
        assert(shownKey1 == '`' and shownKey2 == 'F8')
        assert(select(2, WoweeGetControllerBindingKey('TOGGLEAUTORUN')) == 'PAD4')
        KeyBindingFrame_OnKeyDown(KeyBindingFrame, 'ESCAPE')
        KeyBindingFrame_Update()
        assert(shownKey1 == '`' and shownKey2 == 'NUMLOCK')
        assert(not WoweeSetBindingSlot('JUMP', 3, 'SPACE', false))
        assert(not WoweeSetBindingSlot('JUMP', 1, 'PAD1', false))
        assert(not WoweeSetBindingSlot('bad\nrow', 1, nil, false))
    )lua");

    run("KeyBindingFrame:Show(); KeyBindingFrame.selected = 'TOGGLEAUTORUN'; KeyBindingFrame.keyID = 2");
    layout();
    // SDL has already pumped the release when the queued F8 press is dispatched.
    SDL_SetModState(SDL_KMOD_NONE);
    SDL_FlushEvents(SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP);
    for (auto queued : {key(SDL_SCANCODE_LSHIFT, true, SDL_KMOD_SHIFT),
                        key(SDL_SCANCODE_F8, true, SDL_KMOD_SHIFT),
                        key(SDL_SCANCODE_LSHIFT, false), key(SDL_SCANCODE_F8, false)}) {
        REQUIRE(SDL_PushEvent(&queued));
    }
    SDL_Event queued{};
    int dispatched = 0;
    while (SDL_PollEvent(&queued)) {
        if (queued.type != SDL_EVENT_KEY_DOWN && queued.type != SDL_EVENT_KEY_UP) continue;
        REQUIRE(SDL_GetModState() == SDL_KMOD_NONE);
        REQUIRE(lua.dispatchFrameKey(queued.key.key, queued.type == SDL_EVENT_KEY_DOWN,
            SDL_SCANCODE_TO_KEYCODE(queued.key.scancode), queued.key.mod));
        ++dispatched;
    }
    REQUIRE(dispatched == 4);
    truth("select(2, GetBindingKey('TOGGLEAUTORUN')) == 'SHIFT-F8'");
    truth("GetBindingAction('SHIFT-F8') == 'TOGGLEAUTORUN' and KeyBindingFrame.selected == nil");
    // Event context must not leak to a later direct call or change addon queries.
    truth("not IsShiftKeyDown() and not select(1, __WoweeBindingCaptureModifiers())");
    run(R"lua(
        KeyBindingFrame:EnableMouseWheel(true)
        KeyBindingFrame:SetScript('OnMouseWheel', function(self, delta)
            KeyBindingFrame_OnKeyDown(self, delta > 0 and 'MOUSEWHEELUP' or 'MOUSEWHEELDOWN')
        end)
        KeyBindingFrame.selected = 'TOGGLEAUTORUN'; KeyBindingFrame.keyID = 2
    )lua");
    layout();
    REQUIRE(lua.dispatchMouseWheel(400, 300, 1, SDL_KMOD_CTRL | SDL_KMOD_ALT));
    truth("select(2, GetBindingKey('TOGGLEAUTORUN')) == 'ALT-CTRL-MOUSEWHEELUP'");
    truth("not IsControlKeyDown() and not IsAltKeyDown()");
    run(R"lua(
        KeyBindingFrame:RegisterForClicks('MiddleButtonUp')
        KeyBindingFrame.selected = 'TOGGLEAUTORUN'; KeyBindingFrame.keyID = 2
    )lua");
    addons::LuaEngine::MouseButtons buttons;
    buttons.middle = true;
    lua.noteMouseModifiers(SDL_BUTTON_MIDDLE, SDL_KMOD_SHIFT);
    lua.dispatchMouse(400, 300, 600, buttons);
    // Shift was released later in the pump, before the sampled click callback.
    SDL_SetModState(SDL_KMOD_NONE);
    lua.noteMouseModifiers(SDL_BUTTON_MIDDLE, SDL_KMOD_SHIFT);
    lua.dispatchMouse(400, 300, 600, {});
    truth("select(2, GetBindingKey('TOGGLEAUTORUN')) == 'SHIFT-BUTTON3'");
    truth("not select(1, __WoweeBindingCaptureModifiers())");
    run(R"lua(
        WoweeBindingDeviceSelector:GetScript('OnClick')(WoweeBindingDeviceSelector)
        KeyBindingFrame.selected = 'TOGGLEAUTORUN'; KeyBindingFrame.keyID = 2
    )lua");
    // Pad tokens already contain their own modifiers; keyboard state adds none.
    SDL_SetModState(SDL_KMOD_CTRL | SDL_KMOD_ALT);
    REQUIRE(lua.dispatchPadKey("SHIFT-PAD4").taken);
    lua.dispatchPadKey("SHIFT-PAD4", false);
    truth("select(2, WoweeGetControllerBindingKey('TOGGLEAUTORUN')) == 'SHIFT-PAD4'");
}
