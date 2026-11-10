// The damage meter's window, run against stand-ins for the frames it makes.
//
// Not the interface: that is what a run of the client is for. This asks what
// a string literal of Lua can only be asked by running it - does it build,
// does it put the right text on its bars, does the mouse wheel move them -
// and the one thing a screenshot cannot show: that a frame of it makes no
// garbage once the numbers stop moving.
#include <catch_amalgamated.hpp>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cstring>
#include <initializer_list>
#include <utility>
#include <string>

#include "addons/damage_meter_lua.hpp"

namespace {

/// Frames, textures and font strings that remember what they were told, and
/// the handful of globals the window reads. Every method nobody asks about is
/// one shared no-op, so calling it makes nothing.
constexpr const char* kFakeInterface = R"LUA(
local noop = function() end
local Frame = {}
Frame.__index = function(t, k)
    local m = Frame[k]
    if m ~= nil then return m end
    return noop
end
local frames = {}
__frames = frames
local function new(name, parent)
    local f = setmetatable({ _name = name, _parent = parent, _shown = true, _scripts = {},
                             _w = 0, _h = 0, _left = 100, _top = 500 }, Frame)
    if name then frames[name] = f end
    return f
end
function Frame:SetScript(k, fn) self._scripts[k] = fn end
function Frame:GetScript(k) return self._scripts[k] end
function Frame:Show() self._shown = true end
function Frame:Hide() self._shown = false end
function Frame:IsShown() return self._shown end
function Frame:SetWidth(w) self._w = w end
function Frame:SetHeight(h) self._h = h end
function Frame:GetWidth() return self._w end
function Frame:GetHeight() return self._h end
function Frame:GetLeft() return self._left end
function Frame:GetTop() return self._top end
function Frame:GetFrameLevel() return 1 end
function Frame:SetText(t) self._text = t end
function Frame:GetText() return self._text end
function Frame:CreateTexture() return new(nil, self) end
function Frame:CreateFontString() return new(nil, self) end
function Frame:GetFont() return 'Fonts\\FRIZQT__.TTF', 12 end

function CreateFrame(kind, name, parent) return new(name, parent) end
UIParent = new('UIParent')
UIParent._w, UIParent._h = 1920, 1080
GameFontHighlightSmall = new('GameFontHighlightSmall')
GameTooltip = new('GameTooltip')
GameTooltip._lines = 0
function GameTooltip:AddLine() self._lines = self._lines + 1 end
function GameTooltip:AddDoubleLine() self._lines = self._lines + 1 end
format = string.format
RAID_CLASS_COLORS = { MAGE = { r = 0.41, g = 0.8, b = 0.94 } }

local settings = { dpsmeter = '1' }
function WoweeGetSetting(k) return settings[k] end
function WoweeSetSetting(k, v) settings[k] = tostring(v) end
local cvars = {}
function GetCVar(k) return cvars[k] end
function SetCVar(k, v) cvars[k] = v end
__cvars = cvars
function UnitName() return 'Tester' end
function GetRealmName() return 'Lab' end

-- Twenty-five combatants with fixed numbers: the meter in a raid between
-- updates, which is when nothing should be drawn anew.
local names = {}
for i = 1, 25 do names[i] = 'Raider' .. i end
__meterCount = 25
function WoweeDamageMeterView() return __meterCount, 32500, 30, true end
function WoweeDamageMeterRow(i)
    if i > __meterCount then return nil end
    local v = 2600 - i * 100
    return names[i], 'MAGE', v, v, v / 30, v * 100 / 32500
end
function WoweeDamageMeterFights() return 0, true, true end
function WoweeDamageMeterFightName() return nil, 30 end
function WoweeDamageMeterAbilities() return 2 end
function WoweeDamageMeterAbility(k) return 'Fireball', 1000, 50, 10, 3 end
function WoweeDamageMeterDeath() return nil end
function WoweeDamageMeterReset() end
)LUA";

struct LuaState {
    lua_State* L = luaL_newstate();
    LuaState() {
        REQUIRE(L != nullptr);
        for (auto [open, name] : {std::pair{luaopen_base, ""}, std::pair{luaopen_table, LUA_TABLIBNAME},
                                  std::pair{luaopen_string, LUA_STRLIBNAME},
                                  std::pair{luaopen_math, LUA_MATHLIBNAME}}) {
            lua_pushcfunction(L, open);
            lua_pushstring(L, name);
            lua_call(L, 1, 0);
        }
    }
    ~LuaState() { lua_close(L); }
    /// Run a chunk; its first result as a string, or the error.
    std::string run(const char* chunk, const char* name) {
        if (luaL_loadbuffer(L, chunk, std::strlen(chunk), name) != 0 || lua_pcall(L, 0, 1, 0) != 0) {
            const char* m = lua_tostring(L, -1);
            std::string err = std::string("error: ") + (m ? m : "?");
            lua_pop(L, 1);
            return err;
        }
        const char* v = lua_tostring(L, -1);
        std::string out = v ? v : "";
        lua_pop(L, 1);
        return out;
    }
};

}  // namespace

TEST_CASE("the meter window builds and labels its bars", "[meterwindow]") {
    LuaState lua;
    REQUIRE(lua.run(kFakeInterface, "FakeInterface").empty());
    REQUIRE(lua.run(wowee::addons::kDamageMeterWindow, "DamageMeterWindow").empty());
    const std::string out = lua.run(R"LUA(
        local driver = __frames['__WoweeDamageMeterDriver']
        local tick = driver:GetScript('OnUpdate')
        for _ = 1, 30 do tick(driver, 0.016) end
        local m = __WoweeDamageMeterState
        local f = __frames['__WoweeDamageMeter']
        local r1 = m.rows[1]
        return table.concat({
            tostring(f ~= nil and f:IsShown()),
            f.title:GetText(),
            r1.name:GetText(),
            r1.value:GetText(),
            tostring(m.visible),
        }, '|')
    )LUA", "Check");
    // 220 wide, eight rows; the top bar is the 2.5k raider, a twelfth of
    // the raid's 32.5k at 83.3 a second.
    CHECK(out == "true|Damage Done|1. Raider1|2.50k (83.3, 7.7%)|8");
}

TEST_CASE("the meter window scrolls, changes mode and remembers per character", "[meterwindow]") {
    LuaState lua;
    REQUIRE(lua.run(kFakeInterface, "FakeInterface").empty());
    REQUIRE(lua.run(wowee::addons::kDamageMeterWindow, "DamageMeterWindow").empty());
    const std::string out = lua.run(R"LUA(
        local driver = __frames['__WoweeDamageMeterDriver']
        local tick = driver:GetScript('OnUpdate')
        tick(driver, 0.016)
        local m = __WoweeDamageMeterState
        local f = __frames['__WoweeDamageMeter']
        local wheel = f:GetScript('OnMouseWheel')
        wheel(f, -1)
        wheel(f, -1)
        tick(driver, 0.016)
        local scrolled = m.rows[1].name:GetText()
        for _ = 1, 100 do wheel(f, -1) end
        tick(driver, 0.016)
        local bottom = m.rows[m.visible].name:GetText()
        for _ = 1, 100 do wheel(f, 1) end
        tick(driver, 0.016)
        local top = m.rows[1].name:GetText()
        m.rows[1]:GetScript('OnEnter')(m.rows[1])
        return table.concat({ scrolled, bottom, top, tostring(GameTooltip._lines) }, '|')
    )LUA", "Scroll");
    // The tooltip: the name, the mode's total, a gap, a heading, two abilities.
    CHECK(out == "3. Raider3|25. Raider25|1. Raider1|6");

    const std::string mode = lua.run(R"LUA(
        local driver = __frames['__WoweeDamageMeterDriver']
        local m = __WoweeDamageMeterState
        m.setMode(2)
        driver:GetScript('OnUpdate')(driver, 0.016)
        return __frames['__WoweeDamageMeter'].title:GetText() .. '|' .. m.rows[1].value:GetText()
            .. '|' .. tostring(__cvars['woweeDamageMeter_Tester_Lab'])
    )LUA", "Mode");
    // DPS puts the rate first, and choosing it is saved with the window's
    // place and size, under this character's name.
    CHECK(mode == "DPS|83.3 (2.50k, 7.7%)|100,500,220,138,2");
}

TEST_CASE("the meter window comes back where this character left it", "[meterwindow]") {
    LuaState lua;
    REQUIRE(lua.run(kFakeInterface, "FakeInterface").empty());
    REQUIRE(lua.run("SetCVar('woweeDamageMeter_Tester_Lab', '300,700,260,200,5')", "Saved").empty());
    REQUIRE(lua.run(wowee::addons::kDamageMeterWindow, "DamageMeterWindow").empty());
    const std::string out = lua.run(R"LUA(
        local driver = __frames['__WoweeDamageMeterDriver']
        driver:GetScript('OnUpdate')(driver, 0.016)
        local f = __frames['__WoweeDamageMeter']
        return f:GetWidth() .. 'x' .. f:GetHeight() .. '|' .. f.title:GetText()
    )LUA", "Restore");
    CHECK(out == "260x200|Damage Taken");
}

TEST_CASE("a frame of the meter window makes no garbage", "[meterwindow]") {
    LuaState lua;
    REQUIRE(lua.run(kFakeInterface, "FakeInterface").empty());
    REQUIRE(lua.run(wowee::addons::kDamageMeterWindow, "DamageMeterWindow").empty());
    const std::string out = lua.run(R"LUA(
        local driver = __frames['__WoweeDamageMeterDriver']
        local tick = driver:GetScript('OnUpdate')
        -- Built, filled, and the bars finished easing to their widths.
        for _ = 1, 200 do tick(driver, 0.016) end
        collectgarbage('collect')
        collectgarbage('stop')
        -- A first stretch to let the interpreter grow its call stack once,
        -- which it does on a collect and is not the window's doing.
        for _ = 1, 100 do tick(driver, 0.016) end
        -- And one stretch the same as the measured one: the first run of
        -- this loop grows the interpreter once more (a single ~1 KB step a
        -- few ticks in, flat after it), which is the interpreter settling,
        -- not garbage a frame makes.
        for _ = 1, 2000 do tick(driver, 0.016) end
        local before = collectgarbage('count')
        for _ = 1, 2000 do tick(driver, 0.016) end
        local grown = collectgarbage('count') - before
        collectgarbage('restart')
        return format('%.3f', grown)
    )LUA", "Garbage");
    INFO("KB grown over 2000 frames: " << out);
    // Thirty-two seconds of frames and over a hundred refreshes of 25 rows:
    // nothing, because nothing changed. A string a frame would be ~100 KB.
    CHECK(std::stod(out) < 0.1);
}
