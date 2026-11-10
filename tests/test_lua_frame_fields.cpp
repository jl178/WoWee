// A frame table's own widget and fields (addons/lua_api_helpers.hpp), read raw.
//
// LibActionButton makes every button with
// setmetatable(CreateFrame(...), {__index = Generic}), Generic being a
// CheckButton of its own, and restrictedframes.lua stands a surrogate
// { [0] = frame[0], [1] = frame } with that metatable in for each button.
// Read through __index, the surrogate's widget was Generic's: not explicitly
// protected, so the button had no protected handle, a header's ChildUpdate
// skipped it, and every ElvUI bar showed page 1.
//
// Pure functions of the Lua stack, so no client is needed.

#include <catch_amalgamated.hpp>

#include "addons/lua_api_helpers.hpp"

#include <string>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

using wowee::addons::frameWidgetId;
using wowee::addons::pushFrameField;

namespace {

struct State {
    lua_State* L = luaL_newstate();
    State() {
        lua_setmarkstdlib(L, 1);
        lua_pushcfunction(L, luaopen_base);  // setmetatable, getmetatable
        lua_call(L, 0, 0);
        lua_setmarkstdlib(L, 0);
    }
    ~State() { lua_close(L); }

    /// A frame table: its own __wid, [0] and __name, registered by widget id
    /// in __WoweeFramesByWid the way CreateFrame registers it.
    void makeFrame(const char* global, uint32_t id, const char* name) {
        lua_newtable(L);
        lua_pushinteger(L, id);
        lua_setfield(L, -2, "__wid");
        lua_pushlightuserdata(L, reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
        lua_rawseti(L, -2, 0);
        if (name) {
            lua_pushstring(L, name);
            lua_setfield(L, -2, "__name");
        }
        lua_getglobal(L, "__WoweeFramesByWid");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setglobal(L, "__WoweeFramesByWid");
        }
        lua_pushinteger(L, id);
        lua_pushvalue(L, -3);
        lua_rawset(L, -3);
        lua_pop(L, 1);
        lua_setglobal(L, global);
    }

    void run(const char* code) { REQUIRE(luaL_dostring(L, code) == 0); }
};

}  // namespace

TEST_CASE("a frame over a prototype frame is itself", "[lua][frame]") {
    State s;
    s.makeFrame("Generic", 10, nullptr);
    s.makeFrame("Button", 20, "Bar2Button1");
    s.run("setmetatable(Button, { __index = Generic })");
    lua_getglobal(s.L, "Button");
    REQUIRE(frameWidgetId(s.L, -1) == 20);
    lua_pop(s.L, 1);
}

TEST_CASE("a secure handle's surrogate is the frame it stands for, not the prototype",
          "[lua][frame]") {
    State s;
    s.makeFrame("Generic", 10, nullptr);
    s.makeFrame("Button", 20, "Bar2Button1");
    s.run("setmetatable(Button, { __index = Generic })\n"
          "Surrogate = setmetatable({ [0] = Button[0], [1] = Button }, getmetatable(Button))");
    lua_getglobal(s.L, "Surrogate");
    REQUIRE(frameWidgetId(s.L, -1) == 20);
    pushFrameField(s.L, -1, "__name");
    REQUIRE(lua_isstring(s.L, -1));
    REQUIRE(std::string(lua_tostring(s.L, -1)) == "Bar2Button1");
    lua_pop(s.L, 2);
}

TEST_CASE("a field the frame lacks is not borrowed from its prototype", "[lua][frame]") {
    State s;
    s.makeFrame("Generic", 10, "Proto");
    s.makeFrame("Button", 20, nullptr);
    s.run("Generic.__scripts = { OnClick = function() end }\n"
          "setmetatable(Button, { __index = Generic })");
    lua_getglobal(s.L, "Button");
    pushFrameField(s.L, -1, "__scripts");
    REQUIRE(lua_isnil(s.L, -1));
    pushFrameField(s.L, -2, "__name");
    REQUIRE(lua_isnil(s.L, -1));
    lua_pop(s.L, 3);
}

TEST_CASE("anything that is not a frame names no widget", "[lua][frame]") {
    State s;
    lua_pushnumber(s.L, 5);
    REQUIRE(frameWidgetId(s.L, -1) == 0);
    pushFrameField(s.L, -1, "__name");
    REQUIRE(lua_isnil(s.L, -1));
    lua_newtable(s.L);
    REQUIRE(frameWidgetId(s.L, -1) == 0);
    lua_pop(s.L, 3);
}
