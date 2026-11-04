#pragma once

#include <string>

struct SDL_Window;
namespace wowee::addons { class LuaEngine; }

namespace wowee::core {

/// WOWEE_DEBUG_LUA=<file>: runs the file once, a few seconds after the player
/// enters the world. Off (and free) unless the variable is set. See
/// debug_lua_hook.cpp for the helpers the script is given.
void debugLuaHookTick(addons::LuaEngine* engine, SDL_Window* window);

/// The interface was (re)loaded at a world entry. Under WOWEE_AUTO_RELOG the
/// script runs again in each new one, as pass 2, 3, ...
void debugLuaHookNoteInterfaceLoaded();

/// A screenshot the script asked for, or empty. Taken by the caller once the
/// frame is finished.
std::string debugLuaHookTakeScreenshot();

}  // namespace wowee::core
