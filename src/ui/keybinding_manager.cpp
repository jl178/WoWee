#include "ui/keybinding_manager.hpp"
#include "ui/gamepad_controls.hpp"
#include "core/logger.hpp"
#include "addons/binding_file.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wowee::ui {
namespace {
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
std::string baseKey(std::string key) {
    bool stripped = true;
    while (stripped) {
        stripped = false;
        for (const char* mod : {"ALT-", "CTRL-", "SHIFT-"}) {
            if (key.starts_with(mod)) { key.erase(0, std::char_traits<char>::length(mod)); stripped = true; break; }
        }
    }
    return key;
}
bool validCommand(const std::string& command) {
    return !command.empty() && !command.starts_with("HEADER_") &&
        std::none_of(command.begin(), command.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
// Canonicalize aliases and modifier order, rejecting unknown physical identities.
bool normalizeKey(std::string& key) {
    using Manager = KeybindingManager;
    std::string base = key;
    bool shift = false, ctrl = false, alt = false;
    bool stripped = true;
    while (stripped) {
        stripped = false;
        for (auto [prefix, flag] : {std::pair{"ALT-", &alt}, {"CTRL-", &ctrl}, {"SHIFT-", &shift}}) {
            if (base.starts_with(prefix)) {
                if (*flag) return false;
                *flag = true;
                base.erase(0, std::char_traits<char>::length(prefix));
                stripped = true;
                break;
            }
        }
    }
    if (base.empty()) return false;
    if (Manager::isControllerKey(base)) {
        bool found = false;
        for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i)
            if (base == padKeyName(static_cast<SDL_GamepadButton>(i))) { found = true; break; }
        if (!found) return false;
    } else if (base != "MOUSEWHEELUP" && base != "MOUSEWHEELDOWN") {
        const auto physical = Manager::keyFromName(base);
        if (physical == ImGuiKey_None || Manager::keyName(physical).empty()) return false;
        base = Manager::keyName(physical);
    }
    key = Manager::modifiedKey(base, shift, ctrl, alt);
    return true;
}
bool replaceFile(const std::filesystem::path& temp, const std::filesystem::path& file) {
#ifdef _WIN32
    if (MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
#else
    std::error_code ec;
    std::filesystem::rename(temp, file, ec);
    if (!ec) return true;
#endif
    LOG_WARNING("Could not replace bindings at ", file.string(), "; previous file retained");
    return false;
}
struct ActionBinding { const char* command; const char* legacy; const char* label; const char* key; const char* secondKey = ""; };
constexpr ActionBinding actions[] = {
    {"TOGGLECHARACTER0", "toggle_character_screen", "Character Screen", "C"},
    {"OPENALLBAGS", "toggle_inventory", "Inventory", "I"},
    {"TOGGLEBACKPACK", "toggle_bags", "Bags", "B"},
    {"TOGGLESPELLBOOK", "toggle_spellbook", "Spellbook", "P"},
    {"TOGGLETALENTS", "toggle_talents", "Talents", "N"},
    {"TOGGLEQUESTLOG", "toggle_quests", "Quests", "L"},
    {"TOGGLEMINIMAP", "toggle_minimap", "Minimap", ""},
    {"TOGGLEGAMEMENU", "toggle_settings", "Settings", "ESCAPE"},
    {"OPENCHAT", "toggle_chat", "Chat", "ENTER", "NUMPADENTER"},
    {"TOGGLESOCIAL", "toggle_guild_roster", "Social", "O"},
    {"TOGGLELFGPARENT", "toggle_dungeon_finder", "Dungeon Finder", "J"},
    {"TOGGLEWORLDMAP", "toggle_world_map", "World Map", "M"},
    {"NAMEPLATES", "toggle_nameplates", "Nameplates", "V"},
    {"TOGGLEACHIEVEMENT", "toggle_achievements", "Achievements", "Y"},
    {"TOGGLECHARACTER1", "toggle_skills", "Skills / Professions", "K"},
    {"TOGGLECHARACTER4", "toggle_pvp", "Player vs Player", "H"},
    {"TOGGLEAUTORUN", "toggle_autorun", "Autorun", "`", "NUMLOCK"},
};
static_assert(std::size(actions) == KeybindingManager::getActionCount());
std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::function<bool()>& typedInputProbe() { static std::function<bool()> probe; return probe; }
std::vector<ImGuiKey>& consumedKeys() { static std::vector<ImGuiKey> keys; return keys; }
int& slashEchoPumps() { static int pumps = 0; return pumps; }
}

KeybindingManager& KeybindingManager::getInstance() { static KeybindingManager instance; return instance; }
KeybindingManager::KeybindingManager() { resetToDefaults(); }
const char* KeybindingManager::commandForAction(Action action) {
    const auto i = static_cast<size_t>(action);
    return i < std::size(actions) ? actions[i].command : "";
}
void KeybindingManager::initializeDefaults(Device device) {
    if (device == Device::Controller) {
        for (const auto rows : {padBindings(), padExtraBindings()}) {
            for (const auto& row : rows) {
                if (row.command && *row.command) setBinding(padKeyName(row.button), row.command);
            }
        }
        // The scheme-only left bumper selects the existing bottom-left bar.
        for (const auto& row : padBindings()) {
            const std::string command = row.command ? row.command : "";
            if (command.starts_with("ACTIONBUTTON"))
                setBinding("SHIFT-" + std::string(padKeyName(row.button)), "MULTIACTIONBAR1BUTTON" + command.substr(12));
        }
        return;
    }
    for (const auto& action : actions) bindings_[action.command] = {action.key, action.secondKey};
    const Bindings defaults = {
        {"MOVEFORWARD", {"W", "UP"}}, {"MOVEBACKWARD", {"S", "DOWN"}},
        {"TURNLEFT", {"A", "LEFT"}}, {"TURNRIGHT", {"D", "RIGHT"}},
        {"STRAFELEFT", {"Q", ""}}, {"STRAFERIGHT", {"E", ""}},
        {"JUMP", {"SPACE", ""}}, {"SITORSTAND", {"X", ""}},
        {"TOGGLERUN", {"NUMPADDIVIDE", ""}}, {"TOGGLESHEATH", {"Z", ""}},
        {"OPENCHATSLASH", {"/", ""}}, {"TARGETNEARESTENEMY", {"TAB", ""}},
        {"SCREENSHOT", {"PRINTSCREEN", ""}}, {"FRIENDNAMEPLATES", {"SHIFT-V", ""}},
        {"WOWEETOGGLERECORDING", {"SHIFT-PRINTSCREEN", ""}},
        {"CAMERAZOOMIN", {"MOUSEWHEELUP", ""}}, {"CAMERAZOOMOUT", {"MOUSEWHEELDOWN", ""}},
        {"WOWEERESETCAMERA", {"R", ""}},
    };
    bindings_.insert(defaults.begin(), defaults.end());
    for (int i = 1; i <= 12; ++i) {
        const std::string key = i < 10 ? std::to_string(i) : i == 10 ? "0" : i == 11 ? "-" : "=";
        bindings_["ACTIONBUTTON" + std::to_string(i)] = {key, ""};
        bindings_["MULTIACTIONBAR1BUTTON" + std::to_string(i)] = {"SHIFT-" + key, ""};
        if (i <= 8) bindings_["SHAPESHIFTBUTTON" + std::to_string(i)] = {"CTRL-" + key, ""};
    }
}
void KeybindingManager::resetToDefaults() {
    resetToDefaults(Device::KeyboardMouse);
    resetToDefaults(Device::Controller);
}
void KeybindingManager::resetToDefaults(Device device) {
    bindings(device).clear();
    initializeDefaults(device);
}

std::string KeybindingManager::keyName(ImGuiKey key) {
    switch (key) {
        case ImGuiKey_None: return "";
        case ImGuiKey_LeftArrow: return "LEFT";
        case ImGuiKey_RightArrow: return "RIGHT";
        case ImGuiKey_UpArrow: return "UP";
        case ImGuiKey_DownArrow: return "DOWN";
        case ImGuiKey_Apostrophe: return "'";
        case ImGuiKey_Comma: return ",";
        case ImGuiKey_Minus: return "-";
        case ImGuiKey_Period: return ".";
        case ImGuiKey_Slash: return "/";
        case ImGuiKey_Semicolon: return ";";
        case ImGuiKey_Equal: return "=";
        case ImGuiKey_LeftBracket: return "[";
        case ImGuiKey_RightBracket: return "]";
        case ImGuiKey_Backslash: return "\\";
        case ImGuiKey_GraveAccent: return "`";
        case ImGuiKey_KeypadDecimal: return "NUMPADDECIMAL";
        case ImGuiKey_KeypadDivide: return "NUMPADDIVIDE";
        case ImGuiKey_KeypadMultiply: return "NUMPADMULTIPLY";
        case ImGuiKey_KeypadSubtract: return "NUMPADMINUS";
        case ImGuiKey_KeypadAdd: return "NUMPADPLUS";
        case ImGuiKey_KeypadEnter: return "NUMPADENTER";
        case ImGuiKey_KeypadEqual: return "NUMPADEQUALS";
        case ImGuiKey_LeftCtrl: return "LCTRL";
        case ImGuiKey_RightCtrl: return "RCTRL";
        case ImGuiKey_LeftShift: return "LSHIFT";
        case ImGuiKey_RightShift: return "RSHIFT";
        case ImGuiKey_LeftAlt: return "LALT";
        case ImGuiKey_RightAlt: return "RALT";
        case ImGuiKey_LeftSuper: return "LGUI";
        case ImGuiKey_RightSuper: return "RGUI";
        case ImGuiKey_MouseLeft: return "BUTTON1";
        case ImGuiKey_MouseRight: return "BUTTON2";
        case ImGuiKey_MouseMiddle: return "BUTTON3";
        case ImGuiKey_MouseX1: return "BUTTON4";
        case ImGuiKey_MouseX2: return "BUTTON5";
        default: break;
    }
    if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9) return "NUMPAD" + std::to_string(key - ImGuiKey_Keypad0);
    if (key < ImGuiKey_NamedKey_BEGIN || key >= ImGuiKey_GamepadStart) return "";
    return upper(ImGui::GetKeyName(key));
}
ImGuiKey KeybindingManager::keyFromName(const std::string& name) {
    if (name.empty()) return ImGuiKey_None;
    if (name == "MOUSEX1" || name == "MOUSE4") return ImGuiKey_MouseX1;
    if (name == "MOUSEX2" || name == "MOUSE5") return ImGuiKey_MouseX2;
    // Built once, in the order the linear scan visited, first match winning, so
    // answers are unchanged. Every binding poll resolves its key through here.
    static const std::unordered_map<std::string, ImGuiKey> byName = [] {
        std::unordered_map<std::string, ImGuiKey> map;
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
            const auto key = static_cast<ImGuiKey>(k);
            map.emplace(keyName(key), key);
            map.emplace(upper(ImGui::GetKeyName(key)), key);
        }
        map.erase("");
        return map;
    }();
    const auto it = byName.find(name);
    return it == byName.end() ? ImGuiKey_None : it->second;
}
std::string KeybindingManager::mouseButtonNameFromSDL(uint8_t button) {
    switch (button) {
        case SDL_BUTTON_LEFT: return "BUTTON1";
        case SDL_BUTTON_RIGHT: return "BUTTON2";
        case SDL_BUTTON_MIDDLE: return "BUTTON3";
        case SDL_BUTTON_X1: return "BUTTON4";
        case SDL_BUTTON_X2: return "BUTTON5";
        default: return "";
    }
}
std::string KeybindingManager::keyNameFromSDL(uint32_t key) {
    switch (key) {
        case SDLK_RETURN:
        case SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_RETURN): return "ENTER";
        case SDLK_KP_0: return "NUMPAD0";
        case SDLK_KP_1: return "NUMPAD1";
        case SDLK_KP_2: return "NUMPAD2";
        case SDLK_KP_3: return "NUMPAD3";
        case SDLK_KP_4: return "NUMPAD4";
        case SDLK_KP_5: return "NUMPAD5";
        case SDLK_KP_6: return "NUMPAD6";
        case SDLK_KP_7: return "NUMPAD7";
        case SDLK_KP_8: return "NUMPAD8";
        case SDLK_KP_9: return "NUMPAD9";
        case SDLK_KP_PERIOD: return "NUMPADDECIMAL";
        case SDLK_KP_DIVIDE: return "NUMPADDIVIDE";
        case SDLK_KP_MULTIPLY: return "NUMPADMULTIPLY";
        case SDLK_KP_MINUS: return "NUMPADMINUS";
        case SDLK_KP_PLUS: return "NUMPADPLUS";
        case SDLK_KP_ENTER: return "NUMPADENTER";
        case SDLK_KP_EQUALS: return "NUMPADEQUALS";
        case SDLK_LCTRL: return "LCTRL";
        case SDLK_RCTRL: return "RCTRL";
        case SDLK_LSHIFT: return "LSHIFT";
        case SDLK_RSHIFT: return "RSHIFT";
        case SDLK_LALT: return "LALT";
        case SDLK_RALT: return "RALT";
        case SDLK_LGUI: return "LGUI";
        case SDLK_RGUI: return "RGUI";
        case SDLK_NUMLOCKCLEAR: return "NUMLOCK";
        default: return upper(SDL_GetKeyName(key));
    }
}
std::string KeybindingManager::modifiedKey(std::string key, bool shift, bool ctrl, bool alt) {
    if (shift) key = "SHIFT-" + key;
    if (ctrl) key = "CTRL-" + key;
    if (alt) key = "ALT-" + key;
    return key;
}

KeybindingManager::Device KeybindingManager::deviceForKey(const std::string& key) {
    return baseKey(key).starts_with("PAD") ? Device::Controller : Device::KeyboardMouse;
}
bool KeybindingManager::setBinding(const std::string& requestedKey, const std::string& command) {
    std::string key = requestedKey;
    if (!normalizeKey(key) || (!command.empty() && !validCommand(command))) return false;
    auto& deviceBindings = bindings(deviceForKey(key));
    for (auto& [name, slots] : deviceBindings) for (auto& slot : slots) if (slot == key) slot.clear();
    if (!command.empty()) {
        auto& slots = deviceBindings[command];
        (slots[0].empty() ? slots[0] : slots[1]) = key;
    }
    return true;
}
bool KeybindingManager::loadBindings(const std::string& path) {
    std::ifstream in(path);
    if (!in) { LOG_WARNING("Could not read bindings from ", path); return false; }
    const auto saved = wowee::addons::parseBindingFile(in);
    if (!saved.valid) { LOG_WARNING("Could not convert bindings from ", path); return false; }

    // Validate everything before publishing anything, including explicit empties.
    auto candidate = *this;
    candidate.resetToDefaults();
    const auto defaults = candidate.bindings_;
    const auto apply = [&](const Bindings& rows, Device device) {
        for (const auto& [command, slots] : rows) {
            if (!validCommand(command)) return false;
            if (saved.versioned) candidate.bindings(device)[command] = {};
            else {
                // Master mixed device identities in the same two slots. An empty
                // row unbinds the desktop command, not an absent controller row.
                if ((slots[0].empty() && slots[1].empty()) ||
                    std::any_of(slots.begin(), slots.end(), [](const auto& key) { return !key.empty() && !isControllerKey(key); }))
                    candidate.bindings_[command] = {};
                for (const auto& key : slots)
                    if (!key.empty() && isControllerKey(key)) candidate.controllerBindings_[command] = {};
            }
        }
        for (const auto& [command, slots] : rows) {
            for (size_t i = 0; i < slots.size(); ++i) {
                if (slots[i].empty()) continue;
                std::string key = slots[i];
                if (!normalizeKey(key) || (saved.versioned && deviceForKey(key) != device)) return false;
                if (!candidate.setBinding(key, command)) return false;
                auto& target = candidate.bindings(deviceForKey(key))[command];
                // Keep explicit slot identity; old mixed-device rows instead
                // compact independently into their own device's available slots.
                if (saved.versioned || (i == 1 && slots[0].empty())) {
                    for (auto& slot : target) if (slot == key) slot.clear();
                    target[i] = key;
                }
            }
        }
        return true;
    };
    if (!apply(saved.keyboardMouse, Device::KeyboardMouse) || !apply(saved.controller, Device::Controller)) {
        LOG_WARNING("Invalid binding assignment in ", path);
        return false;
    }
    // The old comma writer could not distinguish absent alternatives from an
    // intentional empty second slot. Recover only still-unclaimed defaults.
    for (const auto& command : saved.implicitSecondary) {
        const auto original = defaults.find(command);
        if (original == defaults.end()) continue;
        auto& slots = candidate.bindings_[command];
        const auto& keys = original->second;
        const auto& alternative = slots[0] == keys[1] ? keys[0] : keys[1];
        if (!alternative.empty() && candidate.commandForKey(alternative).empty()) {
            const auto empty = std::find(slots.begin(), slots.end(), "");
            if (empty != slots.end()) *empty = alternative;
        }
    }
    bindings_ = std::move(candidate.bindings_);
    controllerBindings_ = std::move(candidate.controllerBindings_);
    if (saved.nativeSettingsMerged || saved.versioned) migratedPaths_.insert(path);
    return true;
}
bool KeybindingManager::loadBindingsWithLegacy(const std::string& path, const std::string& legacyPath) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) { LOG_WARNING("Could not inspect bindings at ", path, ": ", ec.message()); return false; }
    if (exists && !loadBindings(path)) return false;
    if (migratedPaths_.contains(path)) return true;
    const bool legacyExists = std::filesystem::exists(legacyPath, ec);
    if (ec) { LOG_WARNING("Could not inspect legacy bindings at ", legacyPath, ": ", ec.message()); return false; }
    if (legacyExists && !loadFromConfigFile(legacyPath)) return false;
    // A failed save must not suppress legacy settings on the next reload.
    if (!saveBindings(path)) return false;
    migratedPaths_.insert(path);
    return true;
}
bool KeybindingManager::saveBindings(const std::string& path) const {
    // Mutable legacy query access is retained for API compatibility; validate it
    // too, rather than emitting a file which cannot be read back safely.
    for (const auto device : {Device::KeyboardMouse, Device::Controller}) {
        std::set<std::string> assigned;
        for (const auto& [command, slots] : bindings(device)) {
            if (!validCommand(command)) { LOG_WARNING("Invalid binding command while saving ", path); return false; }
            for (const auto& key : slots) {
                if (key.empty()) continue;
                std::string normalized = key;
                if (!normalizeKey(normalized) || normalized != key || deviceForKey(key) != device || !assigned.insert(key).second) {
                    LOG_WARNING("Invalid binding key while saving ", path, ": ", key);
                    return false;
                }
            }
        }
    }
    const std::filesystem::path file(path), temp(path + ".tmp");
    std::error_code ec;
    if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) { LOG_WARNING("Could not create bindings directory: ", ec.message()); return false; }
    std::ofstream out(temp, std::ios::trunc);
    out << wowee::addons::bindingFileVersion << '\n';
    for (const auto device : {Device::KeyboardMouse, Device::Controller}) {
        out << (device == Device::Controller ? "[Controller]\n" : "[KeyboardMouse]\n");
        for (const auto& [command, slots] : bindings(device))
            out << command << '\t' << slots[0] << '\t' << slots[1] << '\n';
    }
    out.close();
    if (!out) { LOG_WARNING("Could not write bindings to ", temp.string()); return false; }
    return replaceFile(temp, file);
}
std::pair<std::string, std::string> KeybindingManager::resolve(const std::string& requestedKey, bool fallback) const {
    std::string key = requestedKey;
    if (!normalizeKey(key)) return {};
    const auto& rows = bindings(deviceForKey(key));
    const auto exact = [&](const std::string& identity) -> std::pair<std::string, std::string> {
        for (const auto& [command, slots] : rows)
            if (slots[0] == identity || slots[1] == identity) return {identity, command};
        return {};
    };
    auto matched = exact(key);
    const auto base = baseKey(key);
    return matched.second.empty() && fallback && base != key ? exact(base) : matched;
}
std::string KeybindingManager::commandForKey(const std::string& key) const { return resolve(key, false).second; }
std::string KeybindingManager::resolveBindingKey(const std::string& key) const { return resolve(key, true).first; }
std::string KeybindingManager::resolveCommand(const std::string& key) const { return resolve(key, true).second; }
void KeybindingManager::beginInputFrame() {
    pressedBindings_.clear();
    // Presses made outside the input pump (a script's JumpOrAscendStart, run
    // from an OnUpdate after this frame's movement read its input) become this
    // frame's edges, rather than being cleared before anything saw them.
    virtualCommandPressed_ = std::move(virtualPressQueued_);
    virtualPressQueued_.clear();
}
void KeybindingManager::processInputEvent(const SDL_Event& event) {
    nativeInput_ = true;
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        inputFocused_ = false;
        down_.fill(false);
        heldBindings_.clear();
        pressedBindings_.clear();
        virtualCommands_.clear();
        virtualCommandPressed_.clear();
        return;
    }
    if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) inputFocused_ = true;
    if (!inputFocused_) return;

    ImGuiKey key = ImGuiKey_None;
    bool down = false;
    bool repeat = false;
    bool shift = false, ctrl = false, alt = false;
    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
        key = keyFromName(keyNameFromSDL(SDL_SCANCODE_TO_KEYCODE(event.key.scancode)));
        down = event.type == SDL_EVENT_KEY_DOWN;
        repeat = event.key.repeat;
        shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
        ctrl = (event.key.mod & SDL_KMOD_CTRL) != 0;
        alt = (event.key.mod & SDL_KMOD_ALT) != 0;
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        key = keyFromName(mouseButtonNameFromSDL(event.button.button));
        down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        // Unlike SDL_GetModState(), these reflect the queue position, not its final snapshot.
        shift = down_[ImGuiKey_LeftShift - ImGuiKey_NamedKey_BEGIN] || down_[ImGuiKey_RightShift - ImGuiKey_NamedKey_BEGIN];
        ctrl = down_[ImGuiKey_LeftCtrl - ImGuiKey_NamedKey_BEGIN] || down_[ImGuiKey_RightCtrl - ImGuiKey_NamedKey_BEGIN];
        alt = down_[ImGuiKey_LeftAlt - ImGuiKey_NamedKey_BEGIN] || down_[ImGuiKey_RightAlt - ImGuiKey_NamedKey_BEGIN];
    }
    if (key == ImGuiKey_None) return;
    const bool wasDown = down_[key - ImGuiKey_NamedKey_BEGIN];
    down_[key - ImGuiKey_NamedKey_BEGIN] = down;
    const auto base = keyName(key);
    if (!down) { heldBindings_.erase(base); return; }
    // Resolve once at queue position, not again after modifiers/bindings change.
    // Both the command and physical identity survive a down/up within this pump.
    if (!wasDown && !repeat) {
        const auto command = resolveCommand(modifiedKey(base, shift, ctrl, alt));
        const bool blocked = interfaceTakingTypedInput() || ImGui::GetIO().WantTextInput || interfaceConsumedKey(key) ||
            (base.starts_with("BUTTON") && ImGui::GetIO().WantCaptureMouse);
        heldBindings_[base] = blocked ? "" : command;
        if (!blocked && !command.empty()) pressedBindings_.emplace(base, command);
    }
}
void KeybindingManager::updateInput() {
    nativeInput_ = true;
    if (!inputFocused_) return;
    std::array<bool, ImGuiKey_NamedKey_COUNT> next{};
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    for (int s = 1; s < count; ++s) {
        if (!keys[s]) continue;
        // SDL's scancode names preserve physical US positions on every layout.
        const auto name = keyNameFromSDL(SDL_SCANCODE_TO_KEYCODE(s));
        const auto key = keyFromName(name);
        if (key != ImGuiKey_None) next[key - ImGuiKey_NamedKey_BEGIN] = true;
    }
    const auto mouse = SDL_GetMouseState(nullptr, nullptr);
    for (uint8_t button = SDL_BUTTON_LEFT; button <= SDL_BUTTON_X2; ++button) {
        const auto key = keyFromName(mouseButtonNameFromSDL(button));
        next[key - ImGuiKey_NamedKey_BEGIN] = (mouse & SDL_BUTTON_MASK(button)) != 0;
    }
    // Polling only reconciles missed transitions. Existing presses retain the
    // recipient chosen by their event, even if modifiers or assignments changed.
    const bool shift = next[ImGuiKey_LeftShift - ImGuiKey_NamedKey_BEGIN] || next[ImGuiKey_RightShift - ImGuiKey_NamedKey_BEGIN];
    const bool ctrl = next[ImGuiKey_LeftCtrl - ImGuiKey_NamedKey_BEGIN] || next[ImGuiKey_RightCtrl - ImGuiKey_NamedKey_BEGIN];
    const bool alt = next[ImGuiKey_LeftAlt - ImGuiKey_NamedKey_BEGIN] || next[ImGuiKey_RightAlt - ImGuiKey_NamedKey_BEGIN];
    for (int i = 0; i < ImGuiKey_NamedKey_COUNT; ++i) {
        const auto base = keyName(static_cast<ImGuiKey>(i + ImGuiKey_NamedKey_BEGIN));
        if (base.empty()) continue;
        if (!next[i]) heldBindings_.erase(base);
        else if (!down_[i]) {
            const bool blocked = interfaceTakingTypedInput() || ImGui::GetIO().WantTextInput ||
                interfaceConsumedKey(static_cast<ImGuiKey>(i + ImGuiKey_NamedKey_BEGIN)) ||
                (base.starts_with("BUTTON") && ImGui::GetIO().WantCaptureMouse);
            heldBindings_[base] = blocked ? "" : resolveCommand(modifiedKey(base, shift, ctrl, alt));
        }
    }
    down_ = next;
}
void KeybindingManager::setVirtualCommand(const std::string& command, const std::string& source, bool down) {
    // A script's press is not a key and does not wait on keyboard focus; it is
    // queued for the next input frame (see beginInputFrame).
    const bool scripted = (source == "lua");
    if (command.empty() || (down && !inputFocused_ && !scripted)) return;
    auto& sources = virtualCommands_[command];
    if (down && sources.empty()) (scripted ? virtualPressQueued_ : virtualCommandPressed_).insert(command);
    if (down) sources.insert(source); else sources.erase(source);
    // A release must not erase an edge from earlier in the same pump.
    if (sources.empty()) virtualCommands_.erase(command);
}
bool KeybindingManager::keyActive(const std::string& binding, bool pressed, bool repeat) const {
    if (binding.empty() || nativeInput_) return false;
    const auto base = baseKey(binding);
    const auto key = keyFromName(base);
    if (key == ImGuiKey_None || interfaceConsumedKey(key)) return false;
    const auto& io = ImGui::GetIO();
    if (base.starts_with("BUTTON") && io.WantCaptureMouse) return false;
    if (resolveBindingKey(modifiedKey(base, io.KeyShift, io.KeyCtrl, io.KeyAlt)) != binding) return false;
    return pressed ? ImGui::IsKeyPressed(key, repeat) : ImGui::IsKeyDown(key);
}
namespace {
bool inputAvailable(const std::string& key) {
    const auto base = baseKey(key);
    const auto physical = KeybindingManager::keyFromName(base);
    if (physical != ImGuiKey_None && interfaceConsumedKey(physical)) return false;
    return !(base.starts_with("BUTTON") || base.starts_with("MOUSEWHEEL")) || !ImGui::GetIO().WantCaptureMouse;
}
template<class Sources>
bool hasCommand(const Sources& sources, const std::string& command) {
    return std::any_of(sources.begin(), sources.end(), [&](const auto& source) {
        return source.second == command && inputAvailable(source.first);
    });
}
}
bool KeybindingManager::isCommandPressed(const std::string& command, bool repeat) const {
    if (command.empty()) return false;
    // A virtual press (a script's JumpOrAscendStart, a touch or pad control)
    // is not a key, so it does not wait on the window having keyboard focus.
    if (virtualCommandPressed_.count(command)) return true;
    if (!inputFocused_ || interfaceTakingTypedInput() || ImGui::GetIO().WantTextInput) return false;
    if (hasCommand(pressedBindings_, command)) return true;
    if (nativeInput_) return false;
    const auto it = bindings_.find(command);
    return it != bindings_.end() && (keyActive(it->second[0], true, repeat) || keyActive(it->second[1], true, repeat));
}
bool KeybindingManager::isCommandDown(const std::string& command) const {
    if (command.empty() || !inputFocused_ || interfaceTakingTypedInput() || ImGui::GetIO().WantTextInput) return false;
    const auto v = virtualCommands_.find(command);
    if ((v != virtualCommands_.end() && !v->second.empty()) || hasCommand(heldBindings_, command)) return true;
    if (nativeInput_) return false;
    const auto it = bindings_.find(command);
    return it != bindings_.end() && (keyActive(it->second[0], false) || keyActive(it->second[1], false));
}
bool KeybindingManager::isActionPressed(Action action, bool repeat) { return isCommandPressed(commandForAction(action), repeat); }

// Import master's native settings; only bindings.cfg is written now.
bool KeybindingManager::loadFromConfigFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) { LOG_WARNING("Could not read legacy bindings from ", path); return false; }
    auto candidate = *this;
    KeybindingManager defaults;
    Bindings rows;
    std::set<std::string> singleSlots;
    std::string line;
    bool section = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;
        if (trimmed == "[Keybindings]") { section = true; continue; }
        if (trimmed[0] == '[') { section = false; continue; }
        if (!section) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string command = trim(line.substr(0, eq));
        if (command == "toggle_quest_log") command = "toggle_quests";
        for (const auto& action : actions) if (command == action.legacy) { command = action.command; break; }
        // This is a native settings section, not the addon binding file. Unknown
        // future settings and comments must never claim another command's key.
        if (!defaults.bindings_.contains(command)) continue;
        const auto keys = line.substr(eq + 1);
        const auto sep = keys.find('\t');
        auto slots = std::array<std::string, 2>{trim(keys.substr(0, sep)), sep == std::string::npos ? "" : trim(keys.substr(sep + 1))};
        for (auto& key : slots) {
            key = upper(key);
            if (key == "NONE") key.clear();
            if (!key.empty() && (!normalizeKey(key) || isControllerKey(key))) {
                LOG_WARNING("Invalid legacy binding for ", command, " in ", path);
                return false;
            }
        }
        singleSlots.erase(command);
        if (sep == std::string::npos && !slots[0].empty()) singleSlots.insert(command);
        rows[command] = slots;
    }
    if (in.bad()) { LOG_WARNING("Could not read legacy bindings from ", path); return false; }
    for (const auto& [command, slots] : rows) candidate.bindings_[command] = {};
    for (const auto& [command, slots] : rows) {
        for (size_t i = 0; i < slots.size(); ++i) {
            if (slots[i].empty()) continue;
            candidate.setBinding(slots[i], command);
            auto& target = candidate.bindings_[command];
            for (auto& slot : target) if (slot == slots[i]) slot.clear();
            target[i] = slots[i];
        }
    }
    for (const auto& command : singleSlots) {
        auto& slots = candidate.bindings_[command];
        const auto& keys = defaults.bindings_[command];
        const auto& alternative = slots[0] == keys[1] ? keys[0] : keys[1];
        if (!alternative.empty() && candidate.commandForKey(alternative).empty() && slots[1].empty()) slots[1] = alternative;
    }
    bindings_ = std::move(candidate.bindings_);
    return true;
}
bool interfaceTakingTypedInput() { const auto& probe = typedInputProbe(); return probe && probe(); }
void setTypedInputProbe(std::function<bool()> probe) { typedInputProbe() = std::move(probe); }
bool interfaceConsumedKey(ImGuiKey key) { const auto& keys = consumedKeys(); return std::find(keys.begin(), keys.end(), key) != keys.end(); }
void noteInterfaceConsumedKey(ImGuiKey key) { if (!interfaceConsumedKey(key)) consumedKeys().push_back(key); }
void clearInterfaceConsumedKeys() { consumedKeys().clear(); }
void noteChatOpenedBySlash() { slashEchoPumps() = 2; }
bool consumeChatSlashEcho(const char* text) {
    if (slashEchoPumps() <= 0 || !text || text[0] != '/' || text[1] != '\0') return false;
    slashEchoPumps() = 0;
    return true;
}
void ageChatSlashEcho() { if (slashEchoPumps() > 0) --slashEchoPumps(); }
} // namespace wowee::ui

namespace wowee::addons {

bool clientActsOnBinding(const std::string& command) {
    using Manager = wowee::ui::KeybindingManager;
    for (int i = 0; i < Manager::getActionCount(); ++i)
        if (command == Manager::commandForAction(static_cast<Manager::Action>(i))) return true;
    static constexpr const char* native[] = {
        "MOVEFORWARD", "MOVEBACKWARD", "TURNLEFT", "TURNRIGHT", "STRAFELEFT", "STRAFERIGHT",
        "JUMP", "SITORSTAND", "TOGGLERUN", "OPENCHATSLASH", "TARGETNEARESTENEMY",
        "SCREENSHOT", "WOWEETOGGLERECORDING", "TOGGLESHEATH", "FRIENDNAMEPLATES", "CAMERAZOOMIN", "CAMERAZOOMOUT",
        "WOWEERESETCAMERA",
    };
    for (const char* name : native) if (command == name) return true;
    for (const auto& [prefix, count] : {std::pair{"ACTIONBUTTON", 12},
             {"SHAPESHIFTBUTTON", 8}, {"MULTIACTIONBAR1BUTTON", 12}})
        for (int i = 1; i <= count; ++i)
            if (command == prefix + std::to_string(i)) return true;
    return false;
}

} // namespace wowee::addons

