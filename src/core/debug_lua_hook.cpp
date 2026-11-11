// debug_lua_hook.cpp - WOWEE_DEBUG_LUA: run a Lua file once the player is in
// the world. A test hook, off by default.
//
// The world map, like most of the interface, can only be judged from inside a
// running client: what the C functions answer depends on the DBCs, the player's
// position and whatever FrameXML did with the answers. This lets a script ask
// those questions and write the answers to the log, without anything outside
// the process touching the desktop.
//
//   WOWEE_DEBUG_LUA=/path/to/test.lua       the file to run
//   WOWEE_DEBUG_LUA_DELAY=12                seconds after world entry (default 10)
//   WOWEE_AUTO_RELOG=N                      after a logout, log in and enter the
//                                           world again (up to N times) and run
//                                           the script again, with the global
//                                           WoweeDebug_Pass = 2, 3, ...
//
// The script gets five extra globals, and only while the variable is set:
//   WoweeDebug_Log(...)            a line in the log, prefixed [luatest]
//   WoweeDebug_Screenshot(path)    a PNG of the next finished frame
//   WoweeDebug_PushKey(name, down [, mods])
//                                  a key event through the SDL queue, so it
//                                  takes the same path as a real press;
//                                  mods is "SHIFT", "CTRL-SHIFT" and so on
//   WoweeDebug_Mouse(frame, button, down [, fx, fy])
//                                  a mouse button event at a point inside a
//                                  frame, through the same queue
//   WoweeDebug_Camera(yaw, pitch, dist)
//                                  the camera at a yaw from the facing
//   WoweeDebug_MouseMotion(dx, dy) a relative mouse move, in pixels
//   WoweeDebug_KnockBack(c, s, h, v) an SMSG_MOVE_KNOCK_BACK handed in
//   WoweeDebug_CameraState()       camera yaw, pitch, facing, movement
//                                  pitch (degrees), distance and the
//                                  player model's camera fade
//   WoweeDebug_TimeOfDay(hours)    hold the world clock at an hour (0..24),
//                                  or a negative hour for the game's clock
//   WoweeDebug_SunYaw()            the camera yaw (as Camera takes it) that
//                                  faces the sun, and the sun's height; then
//                                  the same for where the scene's light
//                                  comes from (the moonlight at night)
//   WoweeDebug_UseObject(name)     right-click the nearest game object of
//                                  that name (a mailbox, a guild vault)
//   WoweeDebug_RightClickCorpse()  right-click the targeted corpse (loot,
//                                  or skin / gather it)
//   WoweeDebug_ProfileTag(label)   under WOWEE_FRAME_PROFILE, close the
//                                  current profile window and open one
//                                  named label (one PROFILE_AB line each,
//                                  with frame and CPU median and p95)
//   WoweeDebug_PadCommand(cmd, down) hold or release a binding command the
//                                  way a pad button does - a virtual command,
//                                  no key behind it - queued for the next
//                                  input frame (the scripted source), for the
//                                  pad paths
//   WoweeDebug_UnitMotion(unit)    "player" or "target": the animation its
//                                  model plays, where the model is drawn
//                                  (render x, y, z), where the unit is
//                                  (canonical x, y, z), whether a server
//                                  spline moves it, the upper-body
//                                  overlay's animation (0 for none) and the
//                                  body animation's time (ms)
//   WoweeDebug_Quit()              close the client
//
// Events pushed this way reach only this client: SDL's queue is in-process.
#include "core/debug_lua_hook.hpp"

#include "addons/lua_engine.hpp"
#include "audio/audio_engine.hpp"
#include "core/application.hpp"
#include "core/entity_spawner.hpp"
#include "core/input.hpp"
#include "core/logger.hpp"
#include "core/profile_tag.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "game/opcode_table.hpp"
#include "network/packet.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/renderer.hpp"
#include "ui/framexml_takeover.hpp"
#include "ui/keybinding_manager.hpp"
#include "ui/widget_tree.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace wowee::core {

namespace {

std::string gPendingScreenshot;
addons::LuaEngine* gEngine = nullptr;
SDL_WindowID gWindowId = 0;

int luaDebugLog(lua_State* L) {
    std::string line;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) line += ' ';
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        const char* s = lua_tostring(L, -1);
        line += s ? s : "?";
        lua_pop(L, 1);
    }
    LOG_WARNING("[luatest] ", line);
    // A scripted run's markers in the sound trace too, so its lines can be
    // read scenario by scenario.
    if (audio::soundTraceEnabled()) audio::soundTraceNote("lua " + line);
    return 0;
}

int luaDebugScreenshot(lua_State* L) {
    gPendingScreenshot = luaL_checkstring(L, 1);
    return 0;
}

int luaDebugPadCommand(lua_State* L) {
    const char* command = luaL_checkstring(L, 1);
    const bool down = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2) != 0;
    // Held commands count only while the bindings think the window has the
    // keyboard (KeybindingManager::isCommandDown), and a client a script
    // drives in the background loses it whenever another window opens beside
    // it. Said again on each press, as WoweeDebug_PushKey does.
    if (down) {
        SDL_Event focus{};
        focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
        focus.window.windowID = gWindowId;
        ui::KeybindingManager::getInstance().processInputEvent(focus);
    }
    ui::KeybindingManager::getInstance().setVirtualCommand(command, "lua", down);
    return 0;
}

int luaDebugPushKey(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const bool down = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2) != 0;
    const SDL_Scancode sc = SDL_GetScancodeFromName(name);
    if (sc == SDL_SCANCODE_UNKNOWN) {
        LOG_WARNING("[luatest] WoweeDebug_PushKey: no key called ", name);
        return 0;
    }
    // The bindings ignore the keyboard while the window does not have the
    // desktop's focus (KeybindingManager::processInputEvent), and a client a
    // script drives in the background usually does not - so its held keys
    // were dropped and nothing walked. Tell the bindings alone that the
    // window has it - not the event queue, where the same event would also
    // un-suspend the sound of a client playing in the background.
    SDL_Event focus{};
    focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    focus.window.windowID = gWindowId;
    ui::KeybindingManager::getInstance().processInputEvent(focus);

    SDL_Event ev{};
    ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    ev.key.scancode = sc;
    ev.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
    ev.key.down = down;
    ev.key.repeat = false;
    // Held modifiers, as the event carries them: a chord such as SHIFT-DOWN
    // resolves from the event's own mod, not from the keyboard state.
    SDL_Keymod mod = SDL_KMOD_NONE;
    if (const char* mods = lua_tostring(L, 3)) {
        const std::string m = mods;
        if (m.find("SHIFT") != std::string::npos) mod |= SDL_KMOD_LSHIFT;
        if (m.find("CTRL") != std::string::npos) mod |= SDL_KMOD_LCTRL;
        if (m.find("ALT") != std::string::npos) mod |= SDL_KMOD_LALT;
    }
    ev.key.mod = mod;
    ev.key.windowID = gWindowId;
    SDL_PushEvent(&ev);
    // Held as well. Movement reads the keyboard state (Input::update), which
    // SDL fills from the device and not from pushed events, so a pushed W
    // reached the bindings and never moved the character: the "walk" windows
    // of a tour stood still.
    core::Input::getInstance().setVirtualKey(sc, down);
    return 0;
}

/// WoweeDebug_Mouse(frameName, button, down [, fx, fy]) - a mouse button
/// event at a point inside a frame, through the SDL queue. fx, fy are fractions
/// of the frame's width and height from its top-left corner (0.5, 0.5 when
/// left out). A motion event goes first so the press lands where it says.
int luaDebugMouse(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const std::string button = luaL_optstring(L, 2, "LeftButton");
    const bool down = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3) != 0;
    const float fx = static_cast<float>(luaL_optnumber(L, 4, 0.5));
    const float fy = static_cast<float>(luaL_optnumber(L, 5, 0.5));
    if (!gEngine) return 0;
    auto& widgets = gEngine->widgets();
    const ui::Widget* w = widgets.findByName(name);
    if (!w || w->rectW <= 0.0f || w->rectH <= 0.0f) {
        LOG_WARNING("[luatest] WoweeDebug_Mouse: no laid-out frame called ", name);
        lua_pushboolean(L, 0);
        return 1;
    }
    const float sc = widgets.uiScale();
    const float x = (w->left + w->rectW * fx) * sc;
    const float y = ImGui::GetIO().DisplaySize.y - (w->bottom + w->rectH * (1.0f - fy)) * sc;
    SDL_Event mv{};
    mv.type = SDL_EVENT_MOUSE_MOTION;
    mv.motion.x = x;
    mv.motion.y = y;
    mv.motion.windowID = gWindowId;
    SDL_PushEvent(&mv);
    SDL_Event ev{};
    ev.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    ev.button.button = button == "RightButton" ? SDL_BUTTON_RIGHT
                     : button == "MiddleButton" ? SDL_BUTTON_MIDDLE
                                                : SDL_BUTTON_LEFT;
    ev.button.down = down;
    ev.button.clicks = 1;
    ev.button.x = x;
    ev.button.y = y;
    ev.button.windowID = gWindowId;
    SDL_PushEvent(&ev);
    // The world clicks read the button and the pointer through Input, which
    // asks SDL's own state - a pushed event never reaches it. Held there too,
    // as PushKey holds its key.
    core::Input::getInstance().setVirtualMousePosition(glm::vec2(x, y));
    core::Input::getInstance().setVirtualMouseButton(ev.button.button, down);
    lua_pushboolean(L, 1);
    return 1;
}

/// WoweeDebug_Camera(yawDeg, pitchDeg, distance) - the third-person camera at
/// a yaw relative to the character's facing (0 behind, 90 to its side), a
/// pitch and a distance, so a test can look at a spell from the side.
int luaDebugCamera(lua_State* L) {
    const float yaw = static_cast<float>(luaL_optnumber(L, 1, 0.0));
    const float pitch = static_cast<float>(luaL_optnumber(L, 2, -20.0));
    const float dist = static_cast<float>(luaL_optnumber(L, 3, 0.0));
    auto* renderer = Application::getInstance().getRenderer();
    if (auto* cc = renderer ? renderer->getCameraController() : nullptr) cc->debugSetView(yaw, pitch, dist);
    return 0;
}

/// WoweeDebug_MouseMotion(dx, dy) - a relative mouse move of (dx, dy) pixels
/// through the SDL queue, as a held button drags it.
int luaDebugMouseMotion(lua_State* L) {
    SDL_Event mv{};
    mv.type = SDL_EVENT_MOUSE_MOTION;
    mv.motion.xrel = static_cast<float>(luaL_checknumber(L, 1));
    mv.motion.yrel = static_cast<float>(luaL_checknumber(L, 2));
    mv.motion.windowID = gWindowId;
    SDL_PushEvent(&mv);
    return 0;
}

/// WoweeDebug_CameraState() - camera yaw, pitch (degrees), the facing yaw,
/// the movement pitch (degrees) and the camera distance.
int luaDebugCameraState(lua_State* L) {
    auto* renderer = Application::getInstance().getRenderer();
    auto* cc = renderer ? renderer->getCameraController() : nullptr;
    if (!cc) return 0;
    lua_pushnumber(L, cc->getYaw());
    lua_pushnumber(L, cc->getPitch());
    lua_pushnumber(L, cc->getFacingYaw());
    lua_pushnumber(L, glm::degrees(cc->getMovePitchRad()));
    lua_pushnumber(L, cc->getViewDistance());
    lua_pushnumber(L, cc->playerModelAlpha());
    lua_pushnumber(L, cc->debugLiquidDepth());
    lua_pushnumber(L, cc->getPlayerCollider().height);
    return 8;
}

/// WoweeDebug_KnockBack(vcos, vsin, hspeed, vspeed) - an SMSG_MOVE_KNOCK_BACK
/// for the player, handed in as if the server had sent it (a knockback spell
/// the lab will not cast on a player). The ack goes to the server as usual.
int luaDebugKnockBack(lua_State* L) {
    auto* gh = Application::getInstance().getGameHandler();
    if (!gh) return 0;
    static uint32_t counter = 0x7F000000u;
    network::Packet p(game::wireOpcode(game::Opcode::SMSG_MOVE_KNOCK_BACK));
    p.writePackedGuid(gh->getPlayerGuid());
    p.writeUInt32(counter++);
    p.writeFloat(static_cast<float>(luaL_checknumber(L, 1)));
    p.writeFloat(static_cast<float>(luaL_checknumber(L, 2)));
    p.writeFloat(static_cast<float>(luaL_checknumber(L, 3)));
    p.writeFloat(static_cast<float>(luaL_checknumber(L, 4)));
    p.setReadPos(0);
    gh->debugInjectPacket(p);
    return 0;
}

/// WoweeDebug_SunYaw() - the camera yaw offset (as WoweeDebug_Camera takes
/// it) that looks toward the sun, and the sun's height in degrees, so a test
/// can shoot into and across the sun at any hour.
int luaDebugSunYaw(lua_State* L) {
    auto* renderer = Application::getInstance().getRenderer();
    auto* lm = renderer ? renderer->getLightingManager() : nullptr;
    auto* cc = renderer ? renderer->getCameraController() : nullptr;
    if (!lm || !cc) return 0;
    const glm::vec3 s = lm->getLightingParams().sunDir;
    float yaw = glm::degrees(std::atan2(s.y, s.x)) - cc->getFacingYaw();
    yaw = std::fmod(yaw + 720.0f, 360.0f);
    lua_pushnumber(L, yaw);
    lua_pushnumber(L, glm::degrees(std::asin(glm::clamp(s.z, -1.0f, 1.0f))));
    // And where the scene's light (and its shadows) come from, which at
    // night is the stock light, not the sun: the yaw and the height, taken
    // above the horizon as the shadow map takes it.
    glm::vec3 from = -lm->getLightingParams().directionalDir;
    if (glm::dot(from, from) < 1e-6f) return 2;
    from = glm::normalize(from.z < 0.0f ? -from : from);
    float lyaw = glm::degrees(std::atan2(from.y, from.x)) - cc->getFacingYaw();
    lua_pushnumber(L, std::fmod(lyaw + 720.0f, 360.0f));
    lua_pushnumber(L, glm::degrees(std::asin(glm::clamp(from.z, -1.0f, 1.0f))));
    return 4;
}

/// WoweeDebug_TimeOfDay(hours) - the light and sky held at an hour, so a test
/// can shoot the same place at noon and at dusk. A negative hour gives the
/// clock back to the game.
int luaDebugTimeOfDay(lua_State* L) {
    const double hours = luaL_optnumber(L, 1, -1.0);
    auto* renderer = Application::getInstance().getRenderer();
    if (auto* lm = renderer ? renderer->getLightingManager() : nullptr) {
        if (hours < 0.0) lm->useRealTime(true);
        else lm->setTimeOfDay(static_cast<float>(std::fmod(hours, 24.0) / 24.0));
    }
    return 0;
}

/// WoweeDebug_UseObject(name) - the nearest game object called that, used as
/// a right-click would use it. Answers whether one was found. Mailboxes and
/// guild vaults have no unit to InteractUnit, and a test needs to open them.
int luaDebugUseObject(lua_State* L) {
    const std::string want = luaL_checkstring(L, 1);
    auto* gh = Application::getInstance().getGameHandler();
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    const auto& mi = gh->getMovementInfo();
    uint64_t best = 0;
    float bestD = 1e30f;
    for (const auto& [g, e] : gh->getEntityManager().getEntities()) {
        if (!e || e->getType() != game::ObjectType::GAMEOBJECT) continue;
        if (std::static_pointer_cast<game::GameObject>(e)->getName() != want) continue;
        const float dx = e->getX() - mi.x, dy = e->getY() - mi.y, dz = e->getZ() - mi.z;
        const float d = dx * dx + dy * dy + dz * dz;
        if (d < bestD) { bestD = d; best = g; }
    }
    if (best) gh->interactWithGameObject(best);
    lua_pushboolean(L, best != 0);
    return 1;
}

/// WoweeDebug_RightClickCorpse() - the targeted dead unit, right-clicked
/// (0x731260's loot / skin branches). Answers whether there was a target.
int luaDebugRightClickCorpse(lua_State* L) {
    auto* gh = Application::getInstance().getGameHandler();
    const uint64_t guid = gh ? gh->getTargetGuid() : 0;
    if (guid) gh->rightClickCorpse(guid, false);
    lua_pushboolean(L, guid != 0);
    return 1;
}

/// WoweeDebug_UnitMotion(unit) - anim id, drawn render x, y, z, entity
/// canonical x, y, z, on-spline; nil when the unit or its model is unknown.
int luaDebugUnitMotion(lua_State* L) {
    const std::string token = luaL_checkstring(L, 1);
    auto& app = Application::getInstance();
    auto* gh = app.getGameHandler();
    auto* renderer = app.getRenderer();
    auto* cr = renderer ? renderer->getCharacterRenderer() : nullptr;
    if (!gh || !cr) return 0;
    uint64_t guid = 0;
    uint32_t inst = 0;
    if (token == "player") {
        guid = gh->getPlayerGuid();
        inst = renderer->getCharacterInstanceId();
    } else if (token == "target") {
        guid = gh->getTargetGuid();
        if (auto* sp = app.getEntitySpawner()) {
            inst = sp->getCreatureInstanceId(guid);
            if (inst == 0) inst = sp->getPlayerInstanceId(guid);
        }
    }
    auto e = guid ? gh->getEntityManager().getEntity(guid) : nullptr;
    if (!e || inst == 0) return 0;
    uint32_t anim = 0;
    float t = 0.0f, d = 0.0f;
    cr->getAnimationState(inst, anim, t, d);
    glm::vec3 drawn(0.0f);
    cr->getInstancePosition(inst, drawn);
    lua_pushinteger(L, anim);
    lua_pushnumber(L, drawn.x);
    lua_pushnumber(L, drawn.y);
    lua_pushnumber(L, drawn.z);
    lua_pushnumber(L, e->getX());
    lua_pushnumber(L, e->getY());
    lua_pushnumber(L, e->getZ());
    lua_pushboolean(L, e->hasStockMotion() && e->stockMotionIsSpline());
    lua_pushinteger(L, cr->upperBodyAnimId(inst));
    lua_pushnumber(L, t);
    return 10;
}

int luaDebugProfileTag(lua_State* L) {
    setProfileTag(luaL_checkstring(L, 1));
    return 0;
}

int luaDebugQuit(lua_State* L) {
    (void)L;
    SDL_Event ev{};
    ev.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&ev);
    return 0;
}

}  // namespace

namespace {
int gInterfaceLoads = 0;
bool relogRequested() {
    const char* v = std::getenv("WOWEE_AUTO_RELOG");
    return v && *v && std::atoi(v) > 0;
}
}  // namespace

void debugLuaHookNoteInterfaceLoaded() { ++gInterfaceLoads; }

void debugLuaHookTick(addons::LuaEngine* engine, SDL_Window* window) {
    if (window) gWindowId = SDL_GetWindowID(window);
    static const char* path = std::getenv("WOWEE_DEBUG_LUA");
    if (!path || !*path || !engine || !engine->getState()) return;
    static bool done = false;
    static int pass = 0;
    using Clock = std::chrono::steady_clock;
    static Clock::time_point entered{};
    static bool sawEntry = false;
    // Under WOWEE_AUTO_RELOG the client logs back in after a logout and
    // loads a new interface; the script runs again there, as pass 2, 3, ...
    if (done && pass > 0 && relogRequested() && gInterfaceLoads > pass) {
        done = false;
        sawEntry = false;
    }
    if (done || !ui::frameXmlWorldEntered()) return;

    if (!sawEntry) {
        sawEntry = true;
        entered = Clock::now();
        return;
    }
    static const double delay = [] {
        const char* v = std::getenv("WOWEE_DEBUG_LUA_DELAY");
        return (v && *v) ? std::atof(v) : 10.0;
    }();
    if (std::chrono::duration<double>(Clock::now() - entered).count() < delay) return;
    done = true;

    std::ifstream in(path);
    if (!in) {
        LOG_WARNING("[luatest] WOWEE_DEBUG_LUA: cannot read ", path);
        return;
    }
    std::stringstream ss;
    ss << in.rdbuf();

    lua_State* L = engine->getState();
    lua_register(L, "WoweeDebug_Log", luaDebugLog);
    lua_register(L, "WoweeDebug_Screenshot", luaDebugScreenshot);
    lua_register(L, "WoweeDebug_PushKey", luaDebugPushKey);
    lua_register(L, "WoweeDebug_PadCommand", luaDebugPadCommand);
    lua_register(L, "WoweeDebug_Quit", luaDebugQuit);
    lua_register(L, "WoweeDebug_ProfileTag", luaDebugProfileTag);
    lua_register(L, "WoweeDebug_Mouse", luaDebugMouse);
    lua_register(L, "WoweeDebug_Camera", luaDebugCamera);
    lua_register(L, "WoweeDebug_MouseMotion", luaDebugMouseMotion);
    lua_register(L, "WoweeDebug_KnockBack", luaDebugKnockBack);
    lua_register(L, "WoweeDebug_CameraState", luaDebugCameraState);
    lua_register(L, "WoweeDebug_TimeOfDay", luaDebugTimeOfDay);
    lua_register(L, "WoweeDebug_SunYaw", luaDebugSunYaw);
    lua_register(L, "WoweeDebug_UseObject", luaDebugUseObject);
    lua_register(L, "WoweeDebug_RightClickCorpse", luaDebugRightClickCorpse);
    lua_register(L, "WoweeDebug_UnitMotion", luaDebugUnitMotion);
    gEngine = engine;
    ++pass;
    if (relogRequested()) {
        lua_pushinteger(L, pass);
        lua_setglobal(L, "WoweeDebug_Pass");
    }

    LOG_WARNING("[luatest] running ", path, " (pass ", pass, ")");
    if (!engine->executeString(ss.str())) {
        LOG_WARNING("[luatest] the script failed to run");
    }
}

std::string debugLuaHookTakeScreenshot() {
    std::string out;
    out.swap(gPendingScreenshot);
    return out;
}

}  // namespace wowee::core
