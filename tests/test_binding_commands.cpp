#include "input_test_support.hpp"
#include "ui/gamepad_controls.hpp"
#include "addons/binding_file.hpp"
#include <tuple>

using namespace input_test;

TEST_CASE_METHOD(Fixture, "Complete defaults need no Lua and device resets are independent", "[bindings][R1][O1]") {
    const auto keyboard = manager.bindings();
    const auto controller = manager.controllerBindings();
    REQUIRE(keyboard.at("TOGGLEAUTORUN") == std::array<std::string, 2>{"`", "NUMLOCK"});
    // The fixture saved the singleton before its first reset. Neither the
    // constructor nor reset may depend on Lua having seeded controller keys.
    CHECK(previous.bindings() == keyboard);
    CHECK(previous.controllerBindings() == controller);
    for (const auto rows : {wowee::ui::padBindings(), wowee::ui::padExtraBindings()}) {
        for (const auto& row : rows) {
            if (row.command && *row.command)
                CHECK(manager.commandForKey(wowee::ui::padKeyName(row.button)) == row.command);
        }
    }
    CHECK(manager.commandForKey("PADLSTICK") == "TOGGLEAUTORUN");
    for (auto scan : {SDL_SCANCODE_GRAVE, SDL_SCANCODE_NUMLOCKCLEAR}) {
        manager.beginInputFrame(); event(scan);
        CHECK(manager.isCommandPressed("TOGGLEAUTORUN"));
        event(scan, false);
    }
    REQUIRE(manager.setBinding("F9", "TOGGLEAUTORUN"));
    CHECK(manager.controllerBindings() == controller);
    const auto editedKeyboard = manager.bindings();
    REQUIRE(manager.setBinding("PADLSTICK", "JUMP"));
    CHECK(manager.bindings() == editedKeyboard);
    manager.resetToDefaults(true);
    CHECK(manager.controllerBindings() == controller);
    CHECK(manager.bindings() == editedKeyboard);
    REQUIRE(manager.setBinding("PAD1"));
    const auto editedController = manager.controllerBindings();
    manager.resetToDefaults(false);
    CHECK(manager.bindings() == keyboard);
    CHECK(manager.controllerBindings() == editedController);
    manager.resetToDefaults();
    REQUIRE(manager.saveBindings(path("defaults.cfg").string()));
    REQUIRE(manager.setBinding("NUMLOCK"));
    REQUIRE(manager.setBinding("PADLSTICK"));
    REQUIRE(manager.loadBindings(path("defaults.cfg").string()));
    CHECK(manager.bindings() == keyboard);
    CHECK(manager.controllerBindings() == controller);
}

TEST_CASE_METHOD(Fixture, "Shared resolution prefers explicit chords and otherwise falls back", "[bindings][O4][O5]") {
    for (const auto& base : {"SPACE", "TAB", "ESCAPE", "Z", "`", "NUMLOCK", "PAD1", "PAD2", "PADRSHOULDER", "PADLSTICK", "PADRSTICK"}) {
        const auto command = manager.commandForKey(base);
        REQUIRE_FALSE(command.empty());
        for (const auto& prefix : {"SHIFT-", "CTRL-", "ALT-", "ALT-CTRL-SHIFT-"}) {
            const std::string chord = std::string(prefix) + base;
            // A chord stock binds itself (SHIFT-TAB, CTRL-TAB, SHIFT-SPACE)
            // is explicit, so it is not a fallback to test.
            if (!manager.commandForKey(chord).empty()) continue;
            CHECK(manager.resolveCommand(chord) == command);
            REQUIRE(manager.setBinding(chord, "CUSTOM_CHORD"));
            CHECK(manager.resolveCommand(chord) == "CUSTOM_CHORD");
            REQUIRE(manager.setBinding(chord));
        }
    }
    for (const auto& row : wowee::ui::padBindings()) {
        const std::string command = row.command ? row.command : "";
        if (command.starts_with("ACTIONBUTTON"))
            CHECK(manager.resolveCommand("SHIFT-" + std::string(wowee::ui::padKeyName(row.button))) ==
                  "MULTIACTIONBAR1BUTTON" + command.substr(12));
    }
}

TEST_CASE_METHOD(Fixture, "Queued physical edges keep queue-time modifiers and survive short presses", "[bindings][O4][O7][O9]") {
    REQUIRE(manager.setBinding("CTRL-SHIFT-K", "CUSTOM_CHORD"));
    SDL_SetModState(SDL_KMOD_NONE); // final global snapshot is deliberately wrong
    event(SDL_SCANCODE_K, true, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
    event(SDL_SCANCODE_K, false);
    CHECK(manager.isCommandPressed("CUSTOM_CHORD"));
    CHECK_FALSE(manager.isCommandDown("CUSTOM_CHORD"));
    manager.beginInputFrame();
    event(SDL_SCANCODE_K, true, SDL_KMOD_CTRL | SDL_KMOD_SHIFT, true);
    CHECK_FALSE(manager.isCommandPressed("CUSTOM_CHORD"));
    event(SDL_SCANCODE_K, false);
    for (auto scan : {SDL_SCANCODE_SPACE, SDL_SCANCODE_TAB, SDL_SCANCODE_ESCAPE,
                      SDL_SCANCODE_Z, SDL_SCANCODE_GRAVE, SDL_SCANCODE_NUMLOCKCLEAR}) {
        const auto command = manager.commandForKey(Manager::keyNameFromSDL(SDL_SCANCODE_TO_KEYCODE(scan)));
        for (auto mod : {SDL_KMOD_SHIFT, SDL_KMOD_CTRL, SDL_KMOD_ALT}) {
            // Stock binds SHIFT-TAB, CTRL-TAB and SHIFT-SPACE themselves.
            const std::string chord = Manager::modifiedKey(
                Manager::keyNameFromSDL(SDL_SCANCODE_TO_KEYCODE(scan)),
                mod == SDL_KMOD_SHIFT, mod == SDL_KMOD_CTRL, mod == SDL_KMOD_ALT);
            if (!manager.commandForKey(chord).empty()) continue;
            manager.beginInputFrame();
            event(scan, true, mod); event(scan, false, mod);
            CHECK(manager.isCommandPressed(command));
            manager.beginInputFrame();
            CHECK_FALSE(manager.isCommandPressed(command));
        }
    }
    // Event key is the layout identity; gameplay uses the physical US position.
    for (auto [scan, layout, command] : {
        std::tuple{SDL_SCANCODE_W, SDLK_Z, "MOVEFORWARD"},
        std::tuple{SDL_SCANCODE_A, SDLK_Q, "TURNLEFT"},
        std::tuple{SDL_SCANCODE_Y, SDLK_Z, "TOGGLEACHIEVEMENT"},
        std::tuple{SDL_SCANCODE_Z, SDLK_Y, "TOGGLESHEATH"},
        std::tuple{SDL_SCANCODE_SLASH, SDLK_MINUS, "OPENCHATSLASH"}}) {
        manager.beginInputFrame(); event(scan, true, SDL_KMOD_NONE, false, layout);
        CHECK(manager.isCommandPressed(command));
        event(scan, false);
    }
    for (auto [scan, imgui] : {std::pair{SDL_SCANCODE_RETURN, ImGuiKey_Enter},
                              std::pair{SDL_SCANCODE_KP_ENTER, ImGuiKey_KeypadEnter}}) {
        manager.beginInputFrame(); event(scan); event(scan, false);
        CHECK(manager.isCommandPressed("OPENCHAT"));
        wowee::ui::noteInterfaceConsumedKey(imgui);
        CHECK_FALSE(manager.isCommandPressed("OPENCHAT"));
        wowee::ui::clearInterfaceConsumedKeys();
    }
}

TEST_CASE_METHOD(Fixture, "Typing capture, focus loss and independent sources cannot leave commands stuck", "[bindings]") {
    event(SDL_SCANCODE_W);
    manager.setVirtualCommand("MOVEFORWARD", "pad-stick", true);
    manager.setVirtualCommand("MOVEFORWARD", "touch", true);
    event(SDL_SCANCODE_W, false);
    CHECK(manager.isCommandDown("MOVEFORWARD"));
    manager.setVirtualCommand("MOVEFORWARD", "pad-stick", false);
    CHECK(manager.isCommandDown("MOVEFORWARD"));
    manager.setVirtualCommand("MOVEFORWARD", "touch", false);
    CHECK_FALSE(manager.isCommandDown("MOVEFORWARD"));
    for (bool imgui : {false, true}) {
        manager.beginInputFrame();
        wowee::ui::setTypedInputProbe([imgui] { return !imgui; });
        ImGui::GetIO().WantTextInput = imgui;
        event(SDL_SCANCODE_GRAVE);
        CHECK_FALSE(manager.isCommandPressed("TOGGLEAUTORUN"));
        wowee::ui::setTypedInputProbe({}); ImGui::GetIO().WantTextInput = false;
        CHECK_FALSE(manager.isCommandPressed("TOGGLEAUTORUN"));
        event(SDL_SCANCODE_GRAVE, false);
    }
    event(SDL_SCANCODE_W);
    manager.setVirtualCommand("MOVEFORWARD", "touch", true);
    SDL_Event focus{}; focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    manager.processInputEvent(focus);
    CHECK_FALSE(manager.isCommandDown("MOVEFORWARD"));
    event(SDL_SCANCODE_W);
    CHECK_FALSE(manager.isCommandDown("MOVEFORWARD"));
    focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED; manager.processInputEvent(focus);
    event(SDL_SCANCODE_W);
    CHECK(manager.isCommandPressed("MOVEFORWARD"));
}

TEST_CASE_METHOD(Fixture, "Real mouse identities, wheel pulses and held-key recipients remain distinct", "[bindings][input]") {
    REQUIRE(manager.setBinding("BUTTON2", "RIGHT_MOUSE"));
    REQUIRE(manager.setBinding("BUTTON3", "MIDDLE_MOUSE"));
    for (auto [button, command] : {std::pair{SDL_BUTTON_RIGHT, "RIGHT_MOUSE"},
                                  std::pair{SDL_BUTTON_MIDDLE, "MIDDLE_MOUSE"}}) {
        manager.beginInputFrame();
        SDL_Event mouse{}; mouse.type = SDL_EVENT_MOUSE_BUTTON_DOWN; mouse.button.button = button;
        manager.processInputEvent(mouse);
        CHECK(manager.isCommandPressed(command));
        CHECK(manager.isCommandDown(command));
        mouse.type = SDL_EVENT_MOUSE_BUTTON_UP; manager.processInputEvent(mouse);
        CHECK_FALSE(manager.isCommandDown(command));
    }
    REQUIRE(manager.setBinding("MOUSEWHEELUP", "WHEEL_PULSE"));
    manager.beginInputFrame();
    const auto wheelCommand = manager.resolveCommand("MOUSEWHEELUP");
    manager.setVirtualCommand(wheelCommand, "mouse-wheel", true);
    manager.setVirtualCommand(wheelCommand, "mouse-wheel", false);
    CHECK(manager.isCommandPressed("WHEEL_PULSE"));
    CHECK_FALSE(manager.isCommandDown("WHEEL_PULSE"));
    manager.beginInputFrame(); CHECK_FALSE(manager.isCommandPressed("WHEEL_PULSE"));
    event(SDL_SCANCODE_W);
    REQUIRE(manager.setBinding("CTRL-W", "EXPLICIT_CHORD"));
    event(SDL_SCANCODE_LCTRL, true, SDL_KMOD_CTRL);
    CHECK(manager.isCommandDown("MOVEFORWARD"));
    CHECK_FALSE(manager.isCommandPressed("EXPLICIT_CHORD"));
    event(SDL_SCANCODE_W, false, SDL_KMOD_CTRL);
    manager.beginInputFrame(); event(SDL_SCANCODE_W, true, SDL_KMOD_CTRL);
    CHECK(manager.isCommandDown("EXPLICIT_CHORD"));
    CHECK_FALSE(manager.isCommandDown("MOVEFORWARD"));
}

TEST_CASE_METHOD(Fixture, "Binding persistence preserves both device groups and exact unbinds", "[bindings][persistence]") {
    int index = 0;
    for (const auto& keyName : {",", "=", "NUMPADENTER", "PRINTSCREEN", "BUTTON2", "BUTTON3", "CTRL-F8", "ALT-F9"}) {
        REQUIRE(manager.setBinding(keyName, "ADDON_" + std::to_string(++index)));
    }
    REQUIRE(manager.setBinding("MOUSEX1", "EXTRA_MOUSE"));
    CHECK(manager.commandForKey("BUTTON4") == "EXTRA_MOUSE");
    REQUIRE(manager.setBinding("PAD1"));
    REQUIRE(manager.setBinding("PADLSTICK"));
    manager.bindings()["SECONDARY_ONLY"] = {"", "ALT-F11"};  // F11 is stock's fourth bag
    manager.controllerBindings()["PAD_SECONDARY_ONLY"] = {"", "PADFORWARD"};
    const auto keyboard = manager.bindings(), controller = manager.controllerBindings();
    for (auto invalid : {"NOT_A_KEY", "CTRL-", "PADNOTREAL", "F1\n"})
        CHECK_FALSE(manager.setBinding(invalid, "ADDON_COMMAND"));
    CHECK_FALSE(manager.setBinding("F12", "HEADER_TEST"));
    CHECK_FALSE(manager.setBinding("F12", "BAD\nCOMMAND"));
    CHECK(manager.bindings() == keyboard);
    CHECK(manager.controllerBindings() == controller);
    const auto file = path("roundtrip.cfg");
    REQUIRE(manager.saveBindings(file.string()));
    manager.resetToDefaults();
    REQUIRE(manager.loadBindings(file.string()));
    CHECK(manager.bindings() == keyboard);
    CHECK(manager.controllerBindings() == controller);
    CHECK_FALSE(manager.loadBindings(path("does-not-exist.cfg").string()));
    CHECK(manager.bindings() == keyboard);
    CHECK(manager.controllerBindings() == controller);
    const auto bytes = contents(file);
    fs::create_directory(file.string() + ".tmp");
    REQUIRE(manager.setBinding("F12", "UNSAVED"));
    CHECK_FALSE(manager.saveBindings(file.string()));
    CHECK(contents(file) == bytes);
    fs::remove(file.string() + ".tmp");
}

TEST_CASE_METHOD(Fixture, "Master comma saves retain alternatives through save and restart", "[bindings][O3][R1]") {
    const auto file = path("master.cfg");
    writeFile(file, "MOVEFORWARD=W,\r\nMOVEBACKWARD=S,\r\nTURNLEFT=F8,\r\n"
                    "TOGGLEGAMEMENU=ESCAPE,\r\nTOGGLEAUTORUN=NUMLOCK,\r\nJUMP=SPACE,PAD1\r\n");
    REQUIRE(manager.loadBindings(file.string()));
    CHECK(manager.commandForKey("UP") == "MOVEFORWARD");
    CHECK(manager.commandForKey("DOWN") == "MOVEBACKWARD");
    CHECK(manager.commandForKey("LEFT") == "TURNLEFT");
    CHECK(manager.commandForKey("F8") == "TURNLEFT");
    CHECK(manager.commandForKey("ESCAPE") == "TOGGLEGAMEMENU");
    CHECK(manager.commandForKey("PAD2") == "TOGGLEGAMEMENU");
    for (auto autorun : {"`", "NUMLOCK", "PADLSTICK"})
        CHECK(manager.commandForKey(autorun) == "TOGGLEAUTORUN");
    REQUIRE(manager.saveBindings(file.string()));
    const auto keyboard = manager.bindings(), controller = manager.controllerBindings();
    auto restarted = manager; restarted.resetToDefaults();
    REQUIRE(restarted.loadBindings(file.string()));
    CHECK(restarted.bindings() == keyboard);
    CHECK(restarted.controllerBindings() == controller);
    writeFile(file, "MOVEFORWARD=W,\nJUMP=UP,\nMOVEBACKWARD=,\n");
    restarted.resetToDefaults(); REQUIRE(restarted.loadBindings(file.string()));
    CHECK(restarted.commandForKey("UP") == "JUMP");
    CHECK(restarted.commandForKey("DOWN").empty());
    writeFile(file, "MOVEFORWARD=W\t\nTOGGLEAUTORUN=\t\n");
    REQUIRE(restarted.loadBindings(file.string()));
    CHECK(restarted.commandForKey("UP").empty());
    CHECK(restarted.commandForKey("NUMLOCK").empty());
    CHECK(restarted.commandForKey("PADLSTICK") == "TOGGLEAUTORUN");
}

TEST_CASE_METHOD(Fixture, "Legacy native settings trim recognized rows and never steal keys for unknown rows", "[bindings][R2]") {
    const auto file = path("legacy-import.cfg"), legacy = path("settings.cfg");
    writeFile(file, "TOGGLECHARACTER0=C,\nTOGGLEQUESTLOG=L,\n");
    writeFile(legacy, "[Keybindings]\n toggle_character_screen = F9 \n"
                      "# old binding=C\nunknown_future_setting=C\ntoggle_quest_log=None\ntoggle_inventory=\n");
    REQUIRE(manager.loadBindingsWithLegacy(file.string(), legacy.string()));
    CHECK(manager.commandForKey("F9") == "TOGGLECHARACTER0");
    CHECK(manager.commandForKey("C").empty());
    CHECK(manager.bindings().at("TOGGLEQUESTLOG")[0].empty());
    CHECK(manager.bindings().at("OPENALLBAGS")[0].empty());
    // An unknown row must not claim C when it remains a valid default.
    const auto unknownFile = path("unknown-import.cfg"), unknownSettings = path("unknown-settings.cfg");
    writeFile(unknownSettings, "[Keybindings]\n# old binding=C\nunknown_future_setting=C\n");
    auto unknown = manager; unknown.resetToDefaults();
    REQUIRE(unknown.loadFromConfigFile(unknownSettings.string()));
    CHECK(unknown.commandForKey("C") == "TOGGLECHARACTER0");
    REQUIRE(unknown.saveBindings(unknownFile.string()));
    unknown.resetToDefaults(); REQUIRE(unknown.loadBindings(unknownFile.string()));
    CHECK(unknown.commandForKey("C") == "TOGGLECHARACTER0");
    const auto failedFile = path("failed-import.cfg");
    writeFile(failedFile, "TOGGLECHARACTER0=C,\n");
    fs::create_directory(failedFile.string() + ".tmp");
    const auto beforeImport = contents(failedFile), legacyBytes = contents(legacy);
    auto failed = manager; failed.resetToDefaults();
    CHECK_FALSE(failed.loadBindingsWithLegacy(failedFile.string(), legacy.string()));
    CHECK(contents(failedFile) == beforeImport);
    CHECK(contents(legacy) == legacyBytes);
    // Cancel/reload after a failed migration must reapply native customizations.
    CHECK_FALSE(failed.loadBindingsWithLegacy(failedFile.string(), legacy.string()));
    CHECK(failed.commandForKey("F9") == "TOGGLECHARACTER0");
    fs::remove(failedFile.string() + ".tmp");
    REQUIRE(failed.saveBindings(failedFile.string()));
    failed.resetToDefaults(); REQUIRE(failed.loadBindings(failedFile.string()));
    CHECK(failed.commandForKey("F9") == "TOGGLECHARACTER0");
    REQUIRE(manager.setBinding("F9"));
    REQUIRE(manager.saveBindings(file.string()));
    auto restarted = manager; restarted.resetToDefaults();
    REQUIRE(restarted.loadBindingsWithLegacy(file.string(), legacy.string()));
    CHECK(restarted.commandForKey("F9").empty());
    restarted.resetToDefaults();
    REQUIRE(restarted.saveBindings(file.string()));
    REQUIRE(restarted.loadBindingsWithLegacy(file.string(), legacy.string()));
    CHECK(restarted.commandForKey("C") == "TOGGLECHARACTER0");
    // Back to the defaults, where F9 is stock's third bag, not the import.
    CHECK(restarted.commandForKey("F9") == "TOGGLEBAG2");
}

TEST_CASE_METHOD(Fixture, "Stock's default keys are bound where this client leaves them free", "[bindings]") {
    // Data/wtf/defaultbindings.wtf. Up is the previous page, down the next.
    CHECK(manager.commandForKey("SHIFT-UP") == "PREVIOUSACTIONPAGE");
    CHECK(manager.commandForKey("SHIFT-MOUSEWHEELUP") == "PREVIOUSACTIONPAGE");
    CHECK(manager.commandForKey("SHIFT-DOWN") == "NEXTACTIONPAGE");
    CHECK(manager.commandForKey("SHIFT-MOUSEWHEELDOWN") == "NEXTACTIONPAGE");
    CHECK(manager.commandForKey("F1") == "TARGETSELF");
    CHECK(manager.commandForKey("F2") == "TARGETPARTYMEMBER1");
    CHECK(manager.commandForKey("SHIFT-TAB") == "TARGETPREVIOUSENEMY");
    CHECK(manager.commandForKey("CTRL-SHIFT-TAB") == "TARGETPREVIOUSFRIEND");
    CHECK(manager.commandForKey("F8") == "TOGGLEBAG1");
    CHECK(manager.commandForKey("U") == "TOGGLECHARACTER2");
    CHECK(manager.commandForKey("CTRL-F1") == "SHAPESHIFTBUTTON1");
    CHECK(manager.bindings().at("JUMP") == std::array<std::string, 2>{"SPACE", "NUMPAD0"});
    // This client's own choices for a key win over stock's.
    CHECK(manager.commandForKey("R") == "WOWEERESETCAMERA");
    CHECK(manager.commandForKey("SHIFT-1") == "MULTIACTIONBAR1BUTTON1");
    CHECK(manager.commandForKey("CTRL-1") == "SHAPESHIFTBUTTON1");
    // The camera keeps the mouse buttons.
    CHECK(manager.commandForKey("BUTTON1").empty());
    CHECK(manager.commandForKey("BUTTON2").empty());
    CHECK(manager.commandForKey("BUTTON3").empty());
}
