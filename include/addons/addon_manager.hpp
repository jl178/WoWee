#pragma once

#include "addons/lua_engine.hpp"
#include "addons/toc_parser.hpp"
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::addons {

class AddonManager {
public:
    AddonManager();
    ~AddonManager();

    bool initialize(game::GameHandler* gameHandler, const LuaServices& services = {});
    void scanAddons(const std::string& addonsPath);
    /// One more directory scanAddons searches: where a content pack puts
    /// the addons a realm ships (<override>/interface/addons).
    void setExtraAddonRoot(const std::string& path) { extraAddonRoot_ = path; }
    void loadAllAddons();
    /// Parse an XML file, build what it declares, and follow its includes and
    /// scripts. depth guards against a file that includes itself.
    bool loadXmlFile(const std::string& path, int depth);
    /// Load the original interface from its own manifest, in the order it
    /// states. Opt-in through WOWEE_LOAD_FRAMEXML; see loadAllAddons.
    /// Register this client's own settings as a category in FrameXML's
    /// Interface Options, built from the schema. Runs after FrameXML, because
    /// InterfaceOptions_AddCategory is FrameXML's.
    void registerWoweeOptionsPanel();
    /// Move the coin amounts off the coins they are drawn against.
    void giveCoinAmountsClearance();

    bool loadFrameXml(const std::string& frameXmlDir);
    /// Where FrameXML lives, remembered at scan time so the loader can find it.
    void setFrameXmlDir(const std::string& dir) { frameXmlDir_ = dir; }
    bool runScript(const std::string& code);
    /// Run one line of interface Lua, for a keybinding whose window FrameXML
    /// now owns. Errors are logged rather than thrown: a bad line here should
    /// cost the keypress, not the frame.
    void runInterfaceCommand(const std::string& lua);
    /// The same, for a command whose answer decides what happens next.
    bool interfaceCommandBoolean(const std::string& expression);


    void fireEvent(const std::string& event, const std::vector<std::string>& args = {});
    void update(float deltaTime);
    void shutdown();

    [[nodiscard]] const std::vector<TocFile>& getAddons() const { return addons_; }
    /// The addons that wait to be asked for. Half the interface's own panels
    /// are here - the talent tree, the achievements, the macro editor - and
    /// each is loaded whole or not at all, so one of them raising during load
    /// costs its entire panel rather than degrading it.
    [[nodiscard]] const std::vector<TocFile>& getLoadOnDemandAddons() const { return lodAddons_; }
    /// Every addon found, by name, with the directory it is in - what
    /// "Interface\\AddOns\\<Name>\\..." in its own files refers to. See
    /// AssetManager::setAddonDirectories.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> addonDirectories() const {
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto* list : {&addons_, &lodAddons_})
            for (const auto& a : *list) out.emplace_back(a.addonName, a.basePath);
        return out;
    }

    /// Load a load-on-demand addon by name. Names are matched without regard to
    /// case, because the interface asks for "Blizzard_TalentUI" and the
    /// directory on a case-sensitive filesystem is blizzard_talentui.
    bool loadAddOnByName(const std::string& name, std::string& reason);
    /// Whether an addon's manifest is for this client's interface, as the
    /// client decides it: empty when it is (or when version checking is off
    /// and the addon is merely out of date), otherwise the client's reason.
    [[nodiscard]] std::string versionBlock(const TocFile& addon) const;
    void setAddonVersionCheck(bool on) { versionCheck_ = on; saveEnabledState(); }
    [[nodiscard]] bool isAddonVersionCheckEnabled() const { return versionCheck_; }
    /// Load every load-on-demand addon whose ## LoadWith names this one.
    void loadAddOnsLoadingWith(const std::string& name);
    [[nodiscard]] bool isAddOnLoadedByName(const std::string& name) const;
    LuaEngine* getLuaEngine() { return &luaEngine_; }
    [[nodiscard]] bool isInitialized() const { return luaEngine_.isInitialized(); }

    // Per-addon enable/disable (persisted). Disabled addons are skipped by
    // loadAllAddons; changes take effect on the next load (world enter or /reload).
    [[nodiscard]] bool isAddonEnabled(const std::string& addonName) const;
    void setAddonEnabled(const std::string& addonName, bool enabled);
    // True once any addon has been loaded this session (so the UI can note that a
    // toggle only applies after the next reload).
    [[nodiscard]] bool addonsLoaded() const { return addonsLoaded_; }

    void saveAllSavedVariables();
    void setCharacterName(const std::string& name) { characterName_ = name; }

    /// Take the interface down, leaving a live but empty Lua state.
    ///
    /// The interface belongs to the character that was playing: it is built by
    /// running FrameXML's files, and running them again over a state that
    /// already holds them builds every frame a second time - the widget tree
    /// appends rather than replaces, so both copies draw. That is what a
    /// logout followed by another login without quitting did. Whoever clears
    /// the "addons are loaded" flag has to call this, or the next world entry
    /// loads a second interface on top of the first.
    ///
    /// Saved variables are written on the way out, as they are on a reload.
    bool unloadAll();

    /// Re-initialize the Lua VM and reload all addons (used by /reload).
    bool reload();

private:
    LuaEngine luaEngine_;
    std::vector<TocFile> addons_;
    /// Declared LoadOnDemand: known, listed, and not run until asked for.
    std::vector<TocFile> lodAddons_;
    std::set<std::string> lodLoaded_;
    game::GameHandler* gameHandler_ = nullptr;
    LuaServices luaServices_;
    std::string addonsPath_;
    std::string extraAddonRoot_;

    /// Every global the load-on-demand addons on disk define, read out of their
    /// own files. This is the source of truth for which names must answer as
    /// absent before their addon loads; the literal list in the Lua bootstrap
    /// is a floor for an install with no addons extracted.
    [[nodiscard]] std::vector<std::string> deferredAddonGlobals() const;

    bool loadAddon(const TocFile& addon);
    [[nodiscard]] std::string getSavedVariablesPath(const TocFile& addon) const;
    [[nodiscard]] std::string getSavedVariablesPerCharacterPath(const TocFile& addon) const;
    /// The layout cache for this character: ~/.wowee/layout-cache.<name>.txt,
    /// or without a name when none is known.
    [[nodiscard]] std::string layoutCachePath() const;
    std::string characterName_;

    // addonName -> enabled. Absent means enabled (default on).
    std::unordered_map<std::string, bool> addonEnabled_;
    /// The interface version of the FrameXML this client runs (30300 for
    /// 3.3.5a), read off its manifest; 0 until FrameXML has loaded, which
    /// leaves every addon loadable.
    int clientInterface_ = 0;
    /// The checkAddonVersion setting: on unless the player asked for out of
    /// date addons to be loaded.
    bool versionCheck_ = true;
    std::string frameXmlDir_;
    /// The same directory as it is actually spelled on disk.
    ///
    /// The caller says ".../interface/FrameXML" and this install has
    /// ".../interface/framexml". loadFrameXml resolves that to open it, and
    /// the resolved spelling was thrown away with the local it was kept in -
    /// so every later use of the member was a path that does not exist, on any
    /// filesystem that cares about case.
    std::string frameXmlResolvedDir_;
    /// Why the last loadXmlFile returned false, so a caller loading many files
    /// can report the reasons together instead of leaving them scattered.
    std::string lastXmlError_;
    bool addonsLoaded_ = false;
    static std::string enabledStatePath();
    void loadEnabledState();
    void saveEnabledState() const;
};

} // namespace wowee::addons
