#pragma once

/**
 * damage_meter_lua.hpp - the damage meter's window, in the interface's Lua.
 *
 * Drawn with the game's own frames, fonts, tooltip and dropdown, in the
 * manner of Recount: a dark see-through window with a thin title bar, one
 * class-coloured bar per member of the group, and the mode, the fight, a
 * reset and a close on the title bar. Written for this client; none of
 * Recount's code or art is in it.
 *
 * The numbers are game::DamageMeter's, through the WoweeDamageMeter* calls
 * (lua_system_api.cpp). The window asks for them four times a second and only
 * animates in between, and it keeps every frame, font string and table it
 * makes: a frame's work is a few SetWidth calls and no garbage.
 *
 * Where it is and how big, and which mode it shows, are kept per character in
 * the CVar file, because a druid healer and a rogue want different meters.
 * The "dpsmeter" setting still says whether it is shown at all.
 */

namespace wowee {
namespace addons {

inline constexpr const char* kDamageMeterWindow = R"LUA(
local MODES = {
    { name = 'Damage Done',  rate = false },
    { name = 'DPS',          rate = true },
    { name = 'Healing Done', rate = false },
    { name = 'HPS',          rate = true },
    { name = 'Damage Taken', rate = false },
    { name = 'Overhealing',  rate = false },
    { name = 'Deaths',       count = true },
}
local TITLE_H, ROW_H, ROW_GAP, PAD = 16, 14, 1, 1
local REFRESH = 0.25
-- The stock values, for an interface whose RAID_CLASS_COLORS is missing.
local FALLBACK_COLORS = {
    WARRIOR = { r = 0.78, g = 0.61, b = 0.43 }, PALADIN = { r = 0.96, g = 0.55, b = 0.73 },
    HUNTER = { r = 0.67, g = 0.83, b = 0.45 },  ROGUE = { r = 1.0, g = 0.96, b = 0.41 },
    PRIEST = { r = 1.0, g = 1.0, b = 1.0 },     DEATHKNIGHT = { r = 0.77, g = 0.12, b = 0.23 },
    SHAMAN = { r = 0.0, g = 0.44, b = 0.87 },   MAGE = { r = 0.41, g = 0.8, b = 0.94 },
    WARLOCK = { r = 0.58, g = 0.51, b = 0.79 }, DRUID = { r = 1.0, g = 0.49, b = 0.04 },
}
local GREY = { r = 0.6, g = 0.6, b = 0.6 }

local meter = {
    mode = 1, fight = 0, offset = 0, rows = {}, visible = 0, count = 0,
    since = REFRESH, poll = 0, dirty = true, animating = false,
}
__WoweeDamageMeterState = meter

local function setting(key) return WoweeGetSetting and WoweeGetSetting(key) end

local function short(v)
    if v >= 1e6 then return format('%.2fM', v / 1e6) end
    if v >= 1e4 then return format('%.1fk', v / 1e3) end
    if v >= 1e3 then return format('%.2fk', v / 1e3) end
    return format('%d', v + 0.5)
end
local function rate(v)
    if v >= 1e4 then return short(v) end
    return format('%.1f', v)
end
local function clock(s)
    s = math.floor(s + 0.5)
    return format('%d:%02d', math.floor(s / 60), s % 60)
end

local function classColor(token)
    local t = (RAID_CLASS_COLORS and RAID_CLASS_COLORS[token]) or FALLBACK_COLORS[token]
    return t or GREY
end

-- Per character: "left,top,width,height,mode" in the CVar file.
local function layoutKey()
    local name = UnitName and UnitName('player')
    if not name or name == '' or name == UNKNOWNOBJECT then return nil end
    local realm = GetRealmName and GetRealmName() or ''
    return 'woweeDamageMeter_' .. name .. '_' .. realm
end
local function saveLayout()
    local f, key = meter.frame, meter.key
    if not f or not key or not f:GetLeft() then return end
    SetCVar(key, format('%d,%d,%d,%d,%d', f:GetLeft() + 0.5, f:GetTop() + 0.5,
                        f:GetWidth() + 0.5, f:GetHeight() + 0.5, meter.mode))
end
local function loadLayout(f)
    local saved = meter.key and GetCVar and GetCVar(meter.key)
    local x, y, w, h, mode
    if saved and saved ~= '' then
        x, y, w, h, mode = string.match(saved, '^(%-?%d+),(%-?%d+),(%d+),(%d+),(%d+)$')
    end
    f:ClearAllPoints()
    if x then
        f:SetWidth(math.max(tonumber(w), 140))
        f:SetHeight(math.max(tonumber(h), TITLE_H + ROW_H + 2))
        f:SetPoint('TOPLEFT', UIParent, 'BOTTOMLEFT', tonumber(x), tonumber(y))
        local m = tonumber(mode)
        if m and MODES[m] then meter.mode = m end
        return
    end
    f:SetWidth(220)
    f:SetHeight(TITLE_H + 8 * (ROW_H + ROW_GAP) + PAD * 2)
    -- Where the old meter was dragged to, if it was; else the lower right,
    -- above the bags, where Recount opens.
    local ox, oy = tonumber(setting('dpsmeterleft')), tonumber(setting('dpsmetertop'))
    if ox and oy and ox >= 0 and oy >= 0 then
        f:SetPoint('TOPLEFT', UIParent, 'BOTTOMLEFT', ox, oy)
    else
        f:SetPoint('BOTTOMRIGHT', UIParent, 'BOTTOMRIGHT', -48, 180)
    end
end

local function fontPath()
    if GameFontHighlightSmall and GameFontHighlightSmall.GetFont then
        local path = GameFontHighlightSmall:GetFont()
        if path then return path end
    end
    return 'Fonts\\FRIZQT__.TTF'
end

local function outlined(parent, size, justify)
    local fs = parent:CreateFontString(nil, 'OVERLAY')
    fs:SetFont(fontPath(), size, 'OUTLINE')
    fs:SetJustifyH(justify)
    fs:SetShadowOffset(0, 0)
    return fs
end

local function solid(parent, layer, r, g, b, a)
    local t = parent:CreateTexture(nil, layer)
    t:SetTexture(r, g, b, a)
    return t
end

local function titleButton(parent, glyph, tip, onClick)
    local b = CreateFrame('Button', nil, parent)
    b:SetWidth(TITLE_H - 2) b:SetHeight(TITLE_H - 2)
    local bg = solid(b, 'BACKGROUND', 1, 1, 1, 0)
    bg:SetAllPoints(b)
    b.bg = bg
    local label = outlined(b, 10, 'CENTER')
    label:SetPoint('CENTER', b, 'CENTER', 0, 0)
    label:SetText(glyph)
    label:SetTextColor(0.85, 0.85, 0.85)
    b.label = label
    b:SetScript('OnEnter', function(self)
        self.bg:SetTexture(1, 1, 1, 0.15)
        self.label:SetTextColor(1, 1, 1)
        if GameTooltip then
            GameTooltip:SetOwner(self, 'ANCHOR_TOP')
            GameTooltip:SetText(tip, 1, 1, 1)
            GameTooltip:Show()
        end
    end)
    b:SetScript('OnLeave', function(self)
        self.bg:SetTexture(1, 1, 1, 0)
        self.label:SetTextColor(0.85, 0.85, 0.85)
        if GameTooltip then GameTooltip:Hide() end
    end)
    b:SetScript('OnClick', onClick)
    return b
end

local function fightLabel(sel)
    if sel == 0 then return 'Current Fight' end
    if sel == 1 then return 'Overall Data' end
    local name, duration = WoweeDamageMeterFightName(sel)
    return format('%d. %s (%s)', sel - 1, name or 'Fight', clock(duration or 0))
end

local function updateTitle()
    local f = meter.frame
    local text = MODES[meter.mode].name
    if meter.fight == 1 then text = text .. ' - Overall'
    elseif meter.fight >= 2 then text = text .. ' - Fight ' .. (meter.fight - 1) end
    f.title:SetText(text)
    f.fightButton.label:SetText(meter.fight == 0 and 'C' or (meter.fight == 1 and 'O' or tostring(meter.fight - 1)))
end

local function setMode(m)
    if not MODES[m] then return end
    meter.mode = m
    meter.offset = 0
    meter.dirty = true
    updateTitle()
    saveLayout()
end
local function setFight(sel)
    meter.fight = sel
    meter.offset = 0
    meter.dirty = true
    updateTitle()
end
-- For a test, or anything else, that wants to drive the window as its
-- buttons do.
meter.setMode, meter.setFight = setMode, setFight

local function showMenu(anchor, items)
    if EasyMenu and meter.menu then
        EasyMenu(items, meter.menu, anchor, 0, 0, 'MENU')
        return true
    end
    return false
end

local function modeMenu(anchor)
    local items = { { text = 'Mode', isTitle = true, notCheckable = true } }
    for i, m in ipairs(MODES) do
        items[#items + 1] = { text = m.name, checked = (i == meter.mode),
                              func = function() setMode(i) end }
    end
    if not showMenu(anchor, items) then setMode(meter.mode % #MODES + 1) end
end

local function fightMenu(anchor)
    local items = { { text = 'Fight', isTitle = true, notCheckable = true } }
    local previous = WoweeDamageMeterFights()
    for sel = 0, previous + 1 do
        items[#items + 1] = { text = fightLabel(sel), checked = (sel == meter.fight),
                              func = function() setFight(sel) end }
    end
    if not showMenu(anchor, items) then setFight((meter.fight + 1) % (previous + 2)) end
end
meter.modeMenu, meter.fightMenu = modeMenu, fightMenu

local function showTooltip(row)
    if not GameTooltip or not row.index then return end
    local i = row.index
    local name, class, _, total, perSecond, percent = WoweeDamageMeterRow(i)
    if not name then return end
    local f = meter.frame
    GameTooltip:SetOwner(row, 'ANCHOR_NONE')
    if (f:GetLeft() or 0) > (UIParent:GetWidth() or 0) / 2 then
        GameTooltip:SetPoint('TOPRIGHT', f, 'TOPLEFT', -2, 0)
    else
        GameTooltip:SetPoint('TOPLEFT', f, 'TOPRIGHT', 2, 0)
    end
    local c = classColor(class)
    GameTooltip:AddLine(name, c.r, c.g, c.b)
    local mode = MODES[meter.mode]
    if mode.count then
        local at, killer, spell, amount = WoweeDamageMeterDeath(i)
        GameTooltip:AddDoubleLine('Deaths', short(total), 1, 0.82, 0, 1, 1, 1)
        if at then
            GameTooltip:AddDoubleLine('Last death', clock(at) .. ' into the fight', 1, 0.82, 0, 1, 1, 1)
            if killer then
                local how = spell and (spell .. ' for ' .. short(amount)) or ''
                GameTooltip:AddDoubleLine(killer, how, 1, 0.3, 0.3, 1, 1, 1)
            end
        end
    else
        GameTooltip:AddDoubleLine(mode.name, short(total) .. ' (' .. rate(perSecond) .. '/s)',
                                  1, 0.82, 0, 1, 1, 1)
        local n = WoweeDamageMeterAbilities(i, 6)
        if n > 0 then GameTooltip:AddLine(' ') GameTooltip:AddLine('Top abilities', 1, 0.82, 0) end
        for k = 1, n do
            local spell, amount, share, hits, crits = WoweeDamageMeterAbility(k)
            local detail = format('%s (%.1f%%)', short(amount), share)
            local left = spell
            if hits and hits > 0 then
                left = format('%s  |cff999999%dx', spell, hits)
                if crits and crits > 0 then
                    left = left .. format(', %d%% crit', crits * 100 / hits + 0.5)
                end
                left = left .. '|r'
            end
            GameTooltip:AddDoubleLine(left, detail, 1, 1, 1, 1, 1, 1)
        end
    end
    GameTooltip:Show()
end

local function makeRow(i)
    local f = meter.frame
    local row = CreateFrame('Frame', nil, f)
    row:SetHeight(ROW_H)
    row:EnableMouse(true)
    local back = solid(row, 'BACKGROUND', 1, 1, 1, 0.04)
    back:SetAllPoints(row)
    local fill = row:CreateTexture(nil, 'BORDER')
    fill:SetTexture('Interface\\TargetingFrame\\UI-StatusBar')
    fill:SetPoint('TOPLEFT', row, 'TOPLEFT', 0, 0)
    fill:SetPoint('BOTTOMLEFT', row, 'BOTTOMLEFT', 0, 0)
    fill:SetWidth(1)
    row.fill = fill
    -- A darker lower half over the fill, which gives the flat stock bar a
    -- little depth without a texture of its own.
    local shade = solid(row, 'ARTWORK', 0, 0, 0, 0.18)
    shade:SetPoint('BOTTOMLEFT', fill, 'BOTTOMLEFT', 0, 0)
    shade:SetPoint('BOTTOMRIGHT', fill, 'BOTTOMRIGHT', 0, 0)
    shade:SetHeight(ROW_H / 2)
    row.shade = shade
    local value = outlined(row, 10, 'RIGHT')
    value:SetPoint('RIGHT', row, 'RIGHT', -3, 0)
    row.value = value
    local name = outlined(row, 10, 'LEFT')
    name:SetPoint('LEFT', row, 'LEFT', 3, 0)
    name:SetPoint('RIGHT', value, 'LEFT', -4, 0)
    if name.SetWordWrap then name:SetWordWrap(false) end
    row.name = name
    row.width, row.target = 0, 0
    row:SetScript('OnEnter', function(self)
        meter.hover = self
        self.tipAmount = self.amount
        showTooltip(self)
    end)
    row:SetScript('OnLeave', function(self)
        if meter.hover == self then meter.hover = nil end
        if GameTooltip then GameTooltip:Hide() end
    end)
    row:SetScript('OnMouseWheel', function(_, delta) meter.scroll(delta) end)
    if row.EnableMouseWheel then row:EnableMouseWheel(true) end
    meter.rows[i] = row
    return row
end

-- How many bars fit, and where each sits. Called when the window is made
-- and whenever it is resized; the rows it needs are made here, once.
local function layout()
    local f = meter.frame
    local h = f:GetHeight() - TITLE_H - PAD * 2
    local n = math.max(math.floor((h + ROW_GAP) / (ROW_H + ROW_GAP)), 1)
    meter.visible = n
    for i = 1, n do
        local row = meter.rows[i] or makeRow(i)
        row:ClearAllPoints()
        row:SetPoint('TOPLEFT', f, 'TOPLEFT', PAD, -TITLE_H - PAD - (i - 1) * (ROW_H + ROW_GAP))
        row:SetPoint('TOPRIGHT', f, 'TOPRIGHT', -PAD, -TITLE_H - PAD - (i - 1) * (ROW_H + ROW_GAP))
    end
    for i = n + 1, #meter.rows do meter.rows[i]:Hide() end
    meter.barWidth = f:GetWidth() - PAD * 2
    meter.dirty = true
end

function meter.scroll(delta)
    local most = math.max(meter.count - meter.visible, 0)
    local o = math.min(math.max(meter.offset - delta, 0), most)
    if o ~= meter.offset then
        meter.offset = o
        meter.dirty = true
    end
end

local function refresh()
    local f = meter.frame
    local count, total, duration = WoweeDamageMeterView(meter.mode, meter.fight)
    meter.count = count
    local most = math.max(count - meter.visible, 0)
    if meter.offset > most then meter.offset = most end
    local mode = MODES[meter.mode]
    local top = 0
    if count > 0 then top = select(3, WoweeDamageMeterRow(1)) or 0 end
    local width = meter.barWidth or 1
    for slot = 1, meter.visible do
        local row = meter.rows[slot]
        local i = slot + meter.offset
        if i <= count then
            local name, class, value, amount, perSecond, percent = WoweeDamageMeterRow(i)
            if row.index ~= i or row.who ~= name or row.amount ~= amount or row.mode ~= meter.mode
               or row.perSecond ~= perSecond then
                row.index, row.who, row.amount, row.mode, row.perSecond = i, name, amount, meter.mode, perSecond
                row.name:SetText(i .. '. ' .. name)
                if mode.count then
                    row.value:SetText(short(amount))
                elseif mode.rate then
                    row.value:SetText(format('%s (%s, %.1f%%)', rate(perSecond), short(amount), percent))
                else
                    row.value:SetText(format('%s (%s, %.1f%%)', short(amount), rate(perSecond), percent))
                end
            end
            if row.class ~= class then
                row.class = class
                local c = classColor(class)
                row.fill:SetVertexColor(c.r, c.g, c.b, 0.9)
            end
            row.target = top > 0 and math.max(value / top * width, 1) or 1
            if row.target ~= row.width then meter.animating = true end
            row:Show()
        else
            row.index, row.who = nil, nil
            row:Hide()
        end
    end
    if count == 0 then f.empty:Show() else f.empty:Hide() end
    -- The tooltip of the bar under the mouse follows its numbers, though
    -- only when they moved: rebuilding it is the one thing here that makes
    -- strings.
    local hover = meter.hover
    if hover and hover:IsShown() and hover.index and hover.tipAmount ~= hover.amount then
        hover.tipAmount = hover.amount
        showTooltip(hover)
    end
    meter.duration = duration
end

-- Bars ease to their new width over about a fifth of a second.
local function animate(elapsed)
    if not meter.animating then return end
    local k = math.min(elapsed * 12, 1)
    local moving = false
    for slot = 1, meter.visible do
        local row = meter.rows[slot]
        if row.index then
            local d = row.target - row.width
            if d > 0.5 or d < -0.5 then
                row.width = row.width + d * k
                moving = true
            else
                row.width = row.target
            end
            -- Never 0, which to a texture means "no width of its own".
            local w = math.max(row.width, 1)
            row.fill:SetWidth(w)
            row.fill:SetTexCoord(0, math.min(w / (meter.barWidth or 1), 1), 0, 1)
        end
    end
    meter.animating = moving
end

local function build()
    local f = CreateFrame('Frame', '__WoweeDamageMeter', UIParent)
    meter.frame = f
    -- LOW: under the bags and other windows (MEDIUM, toplevel), which drew
    -- beneath the meter's text where the two overlapped.
    f:SetFrameStrata('LOW')
    f:SetClampedToScreen(true)
    f:SetMovable(true)
    f:SetResizable(true)
    if f.SetMinResize then f:SetMinResize(140, TITLE_H + ROW_H + PAD * 2) end
    if f.SetMaxResize then f:SetMaxResize(600, 800) end
    f:EnableMouse(true)

    local bg = solid(f, 'BACKGROUND', 0.03, 0.03, 0.04, 0.72)
    bg:SetAllPoints(f)
    -- A one-pixel black edge, which is most of what makes it read as a window.
    local edges = {
        { 'TOPLEFT', 'TOPRIGHT', nil, 1 }, { 'BOTTOMLEFT', 'BOTTOMRIGHT', nil, 1 },
        { 'TOPLEFT', 'BOTTOMLEFT', 1, nil }, { 'TOPRIGHT', 'BOTTOMRIGHT', 1, nil },
    }
    for _, e in ipairs(edges) do
        local t = solid(f, 'BORDER', 0, 0, 0, 0.9)
        t:SetPoint(e[1], f, e[1], 0, 0)
        t:SetPoint(e[2], f, e[2], 0, 0)
        if e[3] then t:SetWidth(e[3]) end
        if e[4] then t:SetHeight(e[4]) end
    end

    local bar = CreateFrame('Frame', nil, f)
    bar:SetPoint('TOPLEFT', f, 'TOPLEFT', 0, 0)
    bar:SetPoint('TOPRIGHT', f, 'TOPRIGHT', 0, 0)
    bar:SetHeight(TITLE_H)
    bar:EnableMouse(true)
    bar:RegisterForDrag('LeftButton')
    bar:SetScript('OnDragStart', function() f:StartMoving() end)
    bar:SetScript('OnDragStop', function()
        f:StopMovingOrSizing()
        saveLayout()
    end)
    bar:SetScript('OnMouseUp', function(self, button)
        if button == 'RightButton' then modeMenu(self) end
    end)
    local barBg = bar:CreateTexture(nil, 'BACKGROUND')
    barBg:SetTexture('Interface\\TargetingFrame\\UI-StatusBar')
    barBg:SetVertexColor(0.22, 0.22, 0.26, 0.95)
    barBg:SetAllPoints(bar)
    local line = solid(bar, 'BORDER', 0, 0, 0, 0.9)
    line:SetPoint('BOTTOMLEFT', bar, 'BOTTOMLEFT', 0, 0)
    line:SetPoint('BOTTOMRIGHT', bar, 'BOTTOMRIGHT', 0, 0)
    line:SetHeight(1)

    local close = titleButton(bar, 'x', 'Close', function()
        if WoweeSetSetting then WoweeSetSetting('dpsmeter', '0') end
        f:Hide()
    end)
    close:SetPoint('RIGHT', bar, 'RIGHT', -1, 0)
    local reset = titleButton(bar, 'R', 'Reset all data', function()
        WoweeDamageMeterReset()
        setFight(0)
    end)
    reset:SetPoint('RIGHT', close, 'LEFT', -1, 0)
    local fight = titleButton(bar, 'C', 'Choose the fight', function(self) fightMenu(self) end)
    fight:SetPoint('RIGHT', reset, 'LEFT', -1, 0)
    f.fightButton = fight
    local nextMode = titleButton(bar, '>', 'Next mode', function() setMode(meter.mode % #MODES + 1) end)
    nextMode:SetPoint('RIGHT', fight, 'LEFT', -3, 0)
    local prevMode = titleButton(bar, '<', 'Previous mode', function()
        setMode((meter.mode - 2) % #MODES + 1)
    end)
    prevMode:SetPoint('RIGHT', nextMode, 'LEFT', -1, 0)

    -- The title is the mode menu, as clicking Recount's is.
    local titleButtonFrame = CreateFrame('Button', nil, bar)
    titleButtonFrame:SetPoint('TOPLEFT', bar, 'TOPLEFT', 0, 0)
    titleButtonFrame:SetPoint('BOTTOMRIGHT', prevMode, 'BOTTOMLEFT', -2, 0)
    titleButtonFrame:RegisterForDrag('LeftButton')
    titleButtonFrame:SetScript('OnDragStart', function() f:StartMoving() end)
    titleButtonFrame:SetScript('OnDragStop', function()
        f:StopMovingOrSizing()
        saveLayout()
    end)
    titleButtonFrame:SetScript('OnClick', function(self) modeMenu(self) end)
    local title = outlined(titleButtonFrame, 10, 'LEFT')
    title:SetPoint('LEFT', bar, 'LEFT', 5, 0)
    title:SetPoint('RIGHT', titleButtonFrame, 'RIGHT', 0, 0)
    if title.SetWordWrap then title:SetWordWrap(false) end
    title:SetTextColor(1, 1, 1)
    f.title = title

    local empty = outlined(f, 10, 'CENTER')
    empty:SetPoint('TOP', bar, 'BOTTOM', 0, -8)
    empty:SetText('No data yet')
    empty:SetTextColor(0.6, 0.6, 0.6)
    f.empty = empty

    local grip = CreateFrame('Button', nil, f)
    grip:SetWidth(12) grip:SetHeight(12)
    grip:SetPoint('BOTTOMRIGHT', f, 'BOTTOMRIGHT', -1, 1)
    grip:SetFrameLevel((f:GetFrameLevel() or 0) + 5)
    local gripTex = grip:CreateTexture(nil, 'OVERLAY')
    gripTex:SetTexture('Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Up')
    gripTex:SetAllPoints(grip)
    gripTex:SetAlpha(0.6)
    grip:SetScript('OnMouseDown', function() f:StartSizing('BOTTOMRIGHT') end)
    grip:SetScript('OnMouseUp', function()
        f:StopMovingOrSizing()
        layout()
        refresh()
        saveLayout()
    end)

    f:SetScript('OnSizeChanged', function() if meter.built then layout() end end)
    f:SetScript('OnMouseWheel', function(_, delta) meter.scroll(delta) end)
    if f.EnableMouseWheel then f:EnableMouseWheel(true) end

    meter.menu = CreateFrame('Frame', '__WoweeDamageMeterMenu', UIParent, 'UIDropDownMenuTemplate')
    loadLayout(f)
    meter.built = true
    layout()
    updateTitle()
end

local driver = CreateFrame('Frame', '__WoweeDamageMeterDriver')
driver:SetScript('OnUpdate', function(self, elapsed)
    elapsed = elapsed or arg1 or 0
    meter.poll = meter.poll - elapsed
    if meter.poll <= 0 then
        meter.poll = REFRESH
        meter.enabled = setting('dpsmeter') == '1' and WoweeDamageMeterFights ~= nil
                        and select(3, WoweeDamageMeterFights()) == true
    end
    if not meter.enabled then
        if meter.frame and meter.frame:IsShown() then meter.frame:Hide() end
        return
    end
    if not meter.frame then
        meter.key = layoutKey()
        if not meter.key then return end
        build()
    end
    local f = meter.frame
    if not f:IsShown() then f:Show() meter.dirty = true end
    meter.since = meter.since + elapsed
    if meter.dirty or meter.since >= REFRESH then
        meter.since = 0
        meter.dirty = false
        refresh()
    end
    animate(elapsed)
end)
driver:Show()
)LUA";

}  // namespace addons
}  // namespace wowee
