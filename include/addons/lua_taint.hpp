#pragma once

// Taint and secure execution, as World of Warcraft 3.3.5a keeps them.
//
// The Lua VM (extern/lua-5.1.5, see lua.h) carries the mechanism: a taint on
// the running execution and on every table slot, picked up by reading a
// tainted slot or running code compiled from a tainted source. This is the
// client's side of it - who the taints are, what the interface is told
// (issecure, issecurevariable, securecall, hooksecurefunc, forceinsecure,
// InCombatLockdown), and what insecure code is refused:
//
//  - the protected functions (casting, targeting, movement, macros...), which
//    do nothing for tainted code and tell the interface ADDON_ACTION_FORBIDDEN,
//    or - for those only restricted in combat or without a key or click
//    behind them - ADDON_ACTION_BLOCKED;
//  - the protected frames, which tainted code may not move, show, hide or
//    reconfigure while the player is in combat (ADDON_ACTION_BLOCKED).

#include <string>

struct lua_State;

namespace wowee::addons {
class LuaEngine;
}

namespace wowee::addons::taint {

/// The taint standing for a name; 0 for the empty name. Addons are tainted by
/// their own names; Blizzard's load-on-demand addons are the client's own and
/// secure, as they are in the real client.
int idFor(const std::string& name);
int idForAddon(const std::string& addonName);
const char* nameOf(int id);

/// Combat lockdown: begins once PLAYER_REGEN_DISABLED has been delivered and
/// ends before PLAYER_REGEN_ENABLED is, so handlers of either event can still
/// do what they need to.
void setCombatLockdown(bool on);

/// A key press or click is being handled: some protected actions (trade,
/// auction bids, the dungeon finder) are open to insecure code only then.
struct HardwareEventScope {
    HardwareEventScope();
    ~HardwareEventScope();
};

/// While the client builds a frame from its XML or a template, what it applies
/// is the client's doing, not a script's, and is not subject to lockdown.
bool constructing();
struct ConstructionScope {
    ConstructionScope();
    ~ConstructionScope();
};

/// Load source with a taint: interface files secure, an addon's files its own.
/// Restores the load mode and the execution taint on the way out.
struct SourceLoadScope {
    SourceLoadScope(lua_State* L, int taint);
    ~SourceLoadScope();
    lua_State* L;
    int savedTaint;
    int savedMode;
};

/// issecure, issecurevariable, securecall, hooksecurefunc, forceinsecure,
/// InCombatLockdown, and the frame's IsProtected/CanChangeProtectedState.
void registerAPI(lua_State* L);

/// Put the protection in front of the protected functions and the protected
/// frame methods. Run once everything that defines them has run, so that a
/// later definition does not replace the guarded one.
void protectFunctions(lua_State* L);

/// Whether insecure code may change this frame now (CanChangeProtectedState):
/// out of combat, from secure code, or for a frame that is not protected.
bool canChangeProtectedState(lua_State* L, int frameIndex);

}  // namespace wowee::addons::taint
