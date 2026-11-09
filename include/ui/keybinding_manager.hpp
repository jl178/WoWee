#pragma once

#include <imgui.h>
#include <string>
#include <map>
#include <array>
#include <set>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

union SDL_Event;

namespace wowee::ui {

/**
 * Manages keybinding configuration for in-game actions.
 * Supports loading/saving from config files and runtime rebinding.
 */
class KeybindingManager {
public:
    enum class Action {
        TOGGLE_CHARACTER_SCREEN,
        TOGGLE_INVENTORY,
        TOGGLE_BAGS,
        TOGGLE_SPELLBOOK,
        TOGGLE_TALENTS,
        TOGGLE_QUESTS,
        TOGGLE_MINIMAP,
        TOGGLE_SETTINGS,
        TOGGLE_CHAT,
        TOGGLE_GUILD_ROSTER,
        TOGGLE_DUNGEON_FINDER,
        TOGGLE_WORLD_MAP,
        TOGGLE_NAMEPLATES,
        TOGGLE_ACHIEVEMENTS,
        TOGGLE_SKILLS,
        TOGGLE_PVP,
        TOGGLE_AUTORUN,
        ACTION_COUNT
    };

    static KeybindingManager& getInstance();

    // Addon queries retain their keyboard/mouse meaning, regardless of editor view.
    enum class Device { KeyboardMouse, Controller };
    using Bindings = std::map<std::string, std::array<std::string, 2>>;
    Bindings& bindings() { return bindings_; }
    const Bindings& bindings() const { return bindings_; }
    Bindings& controllerBindings() { return controllerBindings_; }
    const Bindings& controllerBindings() const { return controllerBindings_; }
    Bindings& bindings(Device device) { return device == Device::Controller ? controllerBindings_ : bindings_; }
    const Bindings& bindings(Device device) const { return device == Device::Controller ? controllerBindings_ : bindings_; }
    static Device deviceForKey(const std::string& key);
    static bool isControllerKey(const std::string& key) { return deviceForKey(key) == Device::Controller; }
    bool setBinding(const std::string& key, const std::string& command = "");
    bool loadBindings(const std::string& path);
    bool loadBindingsWithLegacy(const std::string& path, const std::string& legacyPath);
    bool saveBindings(const std::string& path) const;
    // Exact lookup for editing; runtime resolution prefers the chord, then its base.
    std::string commandForKey(const std::string& key) const;
    std::string resolveBindingKey(const std::string& key) const;
    std::string resolveCommand(const std::string& key) const;

    /// Override bindings (SetOverrideBinding and its siblings): a binding an
    /// owner frame lays over a key without touching the saved set, removed
    /// again with ClearOverrideBindings(owner). A priority override beats an
    /// ordinary override, which beats the saved binding; among owners of the
    /// same rank the most recent wins, as in the client. ElvUI's and
    /// LibActionButton's bar keys and the vehicle bar all work this way.
    /// An empty command removes this owner's override of the key.
    void setOverrideBinding(uintptr_t owner, bool priority, const std::string& key,
                            const std::string& command);
    void clearOverrideBindings(uintptr_t owner);
    /// The override in force for a key, or "" when there is none.
    std::string overrideForKey(const std::string& key) const;
    bool isCommandPressed(const std::string& command, bool repeat = false) const;
    bool isCommandDown(const std::string& command) const;
    static const char* commandForAction(Action action);
    static std::string keyName(ImGuiKey key);
    static ImGuiKey keyFromName(const std::string& name);
    static std::string keyNameFromSDL(uint32_t key);
    static std::string mouseButtonNameFromSDL(uint8_t button);
    static std::string modifiedKey(std::string key, bool shift, bool ctrl, bool alt);

    // Collect press edges during the pump; sample held state before gameplay.
    void beginInputFrame();
    void processInputEvent(const SDL_Event& event);
    void updateInput();
    // Source identifies the physical producer; release the command chosen at down.
    void setVirtualCommand(const std::string& command, const std::string& source, bool down);

    /**
     * Check if an action's keybinding was just pressed.
     * Uses queue-time edges (or ImGui for standalone interface consumers).
     */
    bool isActionPressed(Action action, bool repeat = false);


    /**
     * Reset all keybindings to defaults.
     */
    void resetToDefaults();
    void resetToDefaults(Device device);
    void resetToDefaults(bool controller) { resetToDefaults(controller ? Device::Controller : Device::KeyboardMouse); }

    /**
     * Load keybindings from config file.
     */
    bool loadFromConfigFile(const std::string& filePath);

    /**
     * Get all actions for iteration.
     */
    static constexpr int getActionCount() { return static_cast<int>(Action::ACTION_COUNT); }

private:
    KeybindingManager();

    Bindings bindings_;
    Bindings controllerBindings_;
    std::set<std::string> migratedPaths_;
    std::array<bool, ImGuiKey_NamedKey_COUNT> down_{};
    // Base physical key -> command, latched at down, unaffected by later edits/modifiers.
    std::map<std::string, std::string> heldBindings_;
    std::set<std::pair<std::string, std::string>> pressedBindings_;
    bool nativeInput_ = false;
    bool inputFocused_ = true;
    std::map<std::string, std::set<std::string>> virtualCommands_;
    std::set<std::string> virtualCommandPressed_;
    std::set<std::string> virtualPressQueued_;  ///< scripted presses for the next input frame
    struct OverrideBinding {
        uintptr_t owner = 0;
        bool priority = false;
        uint64_t order = 0;
        std::string command;
    };
    /// Normalised key -> every owner's override of it.
    std::map<std::string, std::vector<OverrideBinding>> overrides_;
    uint64_t overrideOrder_ = 0;
    std::pair<std::string, std::string> resolve(const std::string& key, bool fallback) const;
    bool keyActive(const std::string& key, bool pressed, bool repeat = false) const;
    void initializeDefaults(Device device);
};

/**
 * Is someone typing into the interface that ImGui does not draw?
 *
 * There are two interfaces in this client and only one of them is ImGui, so
 * the io.WantTextInput that everything here used to ask is blind to a chat box
 * FrameXML draws. It answers false for the whole time someone is typing into
 * one, and the keys then do double duty: into the box, and into whatever the
 * key otherwise does. Typing "/logout" opened the quest log on the l and the
 * social panel on the o, and walked the character on the o's neighbours.
 *
 * Asked from several places - bindings, the camera, the sheathe key - so the
 * answer lives here once rather than at each of them.
 */
bool interfaceTakingTypedInput();

/// How to answer the question above. Set once, while the interface is built.
void setTypedInputProbe(std::function<bool()> probe);

/**
 * Whether the interface's focused edit box has already taken this key, this
 * iteration.
 *
 * The guard above is not enough for three of them, because the pump's handling
 * of the key *changes what that guard reports*. The pump hands a focused box
 * every keystroke and stops there - and for these three, taking the key is
 * what lets go of the box. The per-frame poll then runs later in the same
 * iteration, asks whether the interface is taking typed input, and is told no,
 * truthfully, by the box that let go on this very press:
 *
 *  - **Escape** closes the box. The Escape chain then ran on top and put the
 *    game menu up behind the box the player had just dismissed.
 *  - **Enter** sends the line and ends in `ChatEdit_OnEscapePressed`, which
 *    hides the box. Enter is also this client's open-chat key, so sending a
 *    message reopened the box on the same press.
 *  - **Tab** is handed to the box's own handler, which is where the interface
 *    moves between fields - and the target key is Tab.
 *
 * A flag rather than a consumed key, because ImGui's IsKeyPressed does not
 * consume: both sites see the same press whatever either does with it.
 */
bool interfaceConsumedKey(ImGuiKey key);

/// Called by the pump, before it dispatches - after, the box no longer admits
/// to having taken anything.
void noteInterfaceConsumedKey(ImGuiKey key);

/// Cleared at the top of each event pump, so a flag only ever describes the
/// iteration it was set in.
void clearInterfaceConsumedKeys();

/// The '/' that opened the chat box, which must not also be typed into it.
///
/// Pressing slash opens the box with a slash already in it, and SDL sends the
/// same press again as text. Whether that text lands depends on whether it
/// arrives before or after the box takes focus - before, it is dispatched as a
/// frame character and dropped; after, it is inserted, and the player who typed
/// one slash gets two. Which of the two happens is a matter of when SDL chooses
/// to deliver it, so it cannot be reasoned about, only accounted for.
///
/// Set when the key opens the box; the next matching text event within a couple
/// of pumps is swallowed and the mark cleared.
void noteChatOpenedBySlash();

/// Whether this text event is that echo. Consumes the mark when it is.
bool consumeChatSlashEcho(const char* text);

/// Ages the mark by one pump, so a slash typed later is not eaten.
void ageChatSlashEcho();

}  // namespace wowee::ui
