// Taint in the Lua VM (extern/lua-5.1.5, see lua.h), as World of Warcraft's
// interface Lua keeps it: the execution's taint, picked up from tainted code
// and tainted table slots; every table slot's taint, set by whoever stored
// into it; and the client's own code, which is transparent to both and runs
// what it calls the way the client runs a script handler.
//
// The VM alone, with no client around it: what is tested is exactly which
// taint the execution and the slots end up carrying.
#include <catch_amalgamated.hpp>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <string>

namespace {

constexpr int kAddon = 7;
constexpr int kOther = 9;

int lua_taintNow(lua_State* L) {
    lua_pushinteger(L, lua_gettaint(L));
    return 1;
}

struct Fixture {
    lua_State* L = luaL_newstate();

    Fixture() {
        lua_setmarkstdlib(L, 1);
        lua_pushcfunction(L, luaopen_base);
        lua_call(L, 0, 0);
        lua_pushcfunction(L, luaopen_table);
        lua_call(L, 0, 0);
        lua_setmarkstdlib(L, 0);
        lua_register(L, "taintnow", lua_taintNow);
    }
    ~Fixture() { lua_close(L); }

    /// Run source as interface or addon code carrying `taint` (0: secure).
    void runSource(const std::string& code, int taint) {
        lua_setloadsystem(L, LUA_LOAD_SOURCE);
        lua_settaint(L, taint);
        REQUIRE(luaL_loadstring(L, code.c_str()) == 0);
        lua_setloadsystem(L, LUA_LOAD_AUTO);
        REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
        lua_settaint(L, 0);
    }

    /// Run the client's own Lua (loaded from the host: transparent).
    void runSystem(const std::string& code) {
        REQUIRE(luaL_dostring(L, code.c_str()) == 0);
    }

    int globalTaint(const char* name) {
        lua_pushstring(L, name);
        return lua_rawgettaint(L, LUA_GLOBALSINDEX);
    }

    lua_Number number(const char* name) {
        lua_getglobal(L, name);
        const lua_Number n = lua_tonumber(L, -1);
        lua_pop(L, 1);
        return n;
    }
};

}  // namespace

TEST_CASE("addon code runs tainted and taints what it stores", "[lua][taint]") {
    Fixture f;
    f.runSource("seenByAddon = taintnow() AddonGlobal = 1 t = {} t.x = 2", kAddon);
    CHECK(f.number("seenByAddon") == kAddon);
    CHECK(f.globalTaint("AddonGlobal") == kAddon);
    lua_getglobal(f.L, "t");
    lua_pushstring(f.L, "x");
    CHECK(lua_rawgettaint(f.L, -2) == kAddon);
    lua_pop(f.L, 1);
    // The host is secure again once it returns.
    CHECK(lua_gettaint(f.L) == 0);
}

TEST_CASE("secure code stays secure until it reads something tainted", "[lua][taint]") {
    Fixture f;
    f.runSource("SecureGlobal = 1", 0);
    CHECK(f.globalTaint("SecureGlobal") == 0);
    f.runSource("AddonGlobal = 1", kAddon);
    f.runSource(
        "before = taintnow()\n"
        "local a = SecureGlobal\n"
        "middle = taintnow()\n"
        "local b = AddonGlobal\n"
        "after = taintnow()\n",
        0);
    CHECK(f.number("before") == 0);
    CHECK(f.number("middle") == 0);
    CHECK(f.number("after") == kAddon);
    // What it stored after reading the addon's value carries the taint too.
    CHECK(f.globalTaint("after") == kAddon);
    CHECK(f.globalTaint("before") == 0);
}

TEST_CASE("calling addon code taints the caller", "[lua][taint]") {
    Fixture f;
    f.runSource("function AddonFn() return 1 end", kAddon);
    // A secure function that keeps a local reference, so reading the global
    // is not what taints it - running the function is.
    f.runSource(
        "local fn\n"
        "function Setup(x) fn = x end\n"
        "function SecureCaller() before = taintnow(); fn(); after = taintnow() end\n",
        0);
    lua_getglobal(f.L, "Setup");
    lua_getglobal(f.L, "AddonFn");
    REQUIRE(lua_pcall(f.L, 1, 0, 0) == 0);
    lua_settaint(f.L, 0);
    lua_getglobal(f.L, "SecureCaller");
    REQUIRE(lua_pcall(f.L, 0, 0, 0) == 0);
    CHECK(f.number("before") == 0);
    CHECK(f.number("after") == kAddon);
}

TEST_CASE("the host calls a handler with the handler's own taint", "[lua][taint]") {
    Fixture f;
    f.runSource("function SecureHandler() seen = taintnow() end", 0);
    // Left tainted by something else, the host still runs a secure handler
    // secure, and puts its own taint back afterwards.
    lua_settaint(f.L, kOther);
    lua_getglobal(f.L, "SecureHandler");
    REQUIRE(lua_pcall(f.L, 0, 0, 0) == 0);
    CHECK(f.number("seen") == 0);
    CHECK(lua_gettaint(f.L) == kOther);
    lua_settaint(f.L, 0);
}

TEST_CASE("the client's own Lua is transparent", "[lua][taint]") {
    Fixture f;
    f.runSystem(
        "local store = {}\n"
        "function SystemStore(k, v) store[k] = v end\n"
        "function SystemRead(k) return store[k] end\n"
        "function SystemCall(fn) local r = fn() return r, taintnow() end\n"
        "function SystemCallStored() local r = Stored() return r, taintnow() end\n");
    f.runSource("function AddonFn() return taintnow() end", kAddon);
    f.runSource("function SecureFn() return taintnow() end", 0);

    // Tainted code storing through it leaves no taint behind.
    f.runSource("SystemStore('k', 1)", kAddon);
    f.runSource("local v = SystemRead('k') readBack = taintnow()", 0);
    CHECK(f.number("readBack") == 0);

    // Running ordinary Lua for tainted code, it runs that Lua as the client
    // runs a handler - with the callee's taint - and its caller's comes back.
    f.runSource("insideSecure, afterSecure = SystemCall(SecureFn)", kAddon);
    CHECK(f.number("insideSecure") == 0);
    CHECK(f.number("afterSecure") == kAddon);
    // And secure code having the client run addon code is not tainted by it:
    // the client reads the addon's function itself (Stored), which taints
    // nothing, and the addon's taint ends with the call.
    f.runSource("Stored = AddonFn", kAddon);
    f.runSource("insideAddon, afterAddon = SystemCallStored()", 0);
    CHECK(f.number("insideAddon") == kAddon);
    CHECK(f.number("afterAddon") == 0);
}

TEST_CASE("the standard library does not launder taint", "[lua][taint]") {
    Fixture f;
    f.runSource("function SecureFn() return taintnow() end", 0);
    // pcall is a C function, but the standard library's: a secure function
    // called through it from tainted code runs tainted, as if called directly.
    f.runSource("local ok, v = pcall(SecureFn) viaPcall = v", kAddon);
    CHECK(f.number("viaPcall") == kAddon);
    // rawset from tainted code taints the slot as a plain store would.
    f.runSource("rawset(_G, 'RawStored', 1)", kAddon);
    CHECK(f.globalTaint("RawStored") == kAddon);
}

TEST_CASE("a slot's taint can be read and kept by the client", "[lua][taint]") {
    Fixture f;
    f.runSource("Slot = 1", kAddon);
    lua_pushstring(f.L, "Slot");
    lua_rawsettaint(f.L, LUA_GLOBALSINDEX, 0);
    CHECK(f.globalTaint("Slot") == 0);
    lua_pushstring(f.L, "Missing");
    CHECK(lua_rawgettaint(f.L, LUA_GLOBALSINDEX) == 0);
}

TEST_CASE("functions returning fewer values than asked still fill with nil", "[lua][taint]") {
    // setnilvalue is evaluated once per slot: luaD_poscall pads missing
    // results with setnilvalue(res++), and a macro that read its argument
    // twice skipped every other slot and wrote past the frame.
    Fixture f;
    f.runSource(
        "local function one() return 1 end\n"
        "local a, b, c, d = one()\n"
        "padded = (a == 1 and b == nil and c == nil and d == nil) and 1 or 0\n",
        0);
    CHECK(f.number("padded") == 1);
}
