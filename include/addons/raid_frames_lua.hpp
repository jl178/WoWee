#pragma once

/**
 * raid_frames_lua.hpp - raid-style group frames, in the interface's Lua.
 *
 * Not stock: 3.3.5 has the party frames and the Raid tab's pull-outs, and no
 * grid of the whole group. This is that grid, off unless the "raidframes"
 * setting (CVar woweeRaidFrames) turns it on:
 *
 *  - one secure button per member, click to target; the party (you and up to
 *    four) in a column, a raid in one column per subgroup;
 *  - a class-coloured health bar, the name, the health missing, a thin power
 *    bar, Dead / Ghost / Offline, a red border when the unit has aggro, and
 *    half alpha out of range;
 *  - the stock party frames are hidden while it is on, and come back off;
 *  - dragged by the handle above it, right-click the handle to lock; the
 *    place is kept in the CVars woweeRaidFramesX / woweeRaidFramesY.
 *
 * Laid out and shown only out of combat (secure buttons cannot move in it);
 * a roster change in combat is laid out when combat ends. The bars refresh
 * ten times a second from an OnUpdate, which costs less than tracking a
 * dozen UNIT_* events for forty units.
 */

namespace wowee {
namespace addons {

inline constexpr const char* kRaidFramesWindow = R"LUA(
local W, H, GAP, HANDLE_H = 72, 34, 2, 10
local MAX = 40
local REFRESH = 0.1
local COLORS = {
    WARRIOR = { 0.78, 0.61, 0.43 }, PALADIN = { 0.96, 0.55, 0.73 },
    HUNTER = { 0.67, 0.83, 0.45 },  ROGUE = { 1.0, 0.96, 0.41 },
    PRIEST = { 1.0, 1.0, 1.0 },     DEATHKNIGHT = { 0.77, 0.12, 0.23 },
    SHAMAN = { 0.0, 0.44, 0.87 },   MAGE = { 0.41, 0.8, 0.94 },
    WARLOCK = { 0.58, 0.51, 0.79 }, DRUID = { 1.0, 0.49, 0.04 },
}
local POWER = {
    [0] = { 0.0, 0.44, 0.87 }, [1] = { 0.8, 0.0, 0.0 }, [2] = { 1.0, 0.5, 0.25 },
    [3] = { 1.0, 1.0, 0.0 },   [6] = { 0.0, 0.82, 1.0 },
}

local rf = { buttons = {}, pending = false, since = 0, enabled = false }
__WoweeRaidFramesState = rf

local function cvarOn(name) return GetCVar and GetCVar(name) == '1' end

local root = CreateFrame('Frame', 'WoweeRaidFrame', UIParent)
root:SetSize(W, H)
root:SetMovable(true)
root:SetClampedToScreen(true)
root:Hide()

local handle = CreateFrame('Frame', 'WoweeRaidFrameHandle', root)
handle:SetHeight(HANDLE_H)
handle:SetPoint('BOTTOMLEFT', root, 'TOPLEFT', 0, 1)
handle:SetPoint('BOTTOMRIGHT', root, 'TOPRIGHT', 0, 1)
handle:EnableMouse(true)
handle:RegisterForDrag('LeftButton')
local handleBg = handle:CreateTexture(nil, 'BACKGROUND')
handleBg:SetAllPoints()
handleBg:SetTexture(0, 0, 0, 0.45)
local handleText = handle:CreateFontString(nil, 'OVERLAY', 'GameFontNormalSmall')
handleText:SetPoint('LEFT', 3, 0)
handleText:SetText('Group')

local function locked() return cvarOn('woweeRaidFramesLocked') end
local function showHandle()
    if locked() then handle:SetAlpha(0) else handle:SetAlpha(1) end
end

local function placeRoot()
    root:ClearAllPoints()
    local x, y = tonumber(GetCVar('woweeRaidFramesX') or ''), tonumber(GetCVar('woweeRaidFramesY') or '')
    if x and y and (x ~= 0 or y ~= 0) then
        root:SetPoint('TOPLEFT', UIParent, 'BOTTOMLEFT', x, y)
    else
        root:SetPoint('TOPLEFT', UIParent, 'TOPLEFT', 20, -200)
    end
end

handle:SetScript('OnDragStart', function()
    if locked() or InCombatLockdown() then return end
    root:StartMoving()
end)
handle:SetScript('OnDragStop', function()
    root:StopMovingOrSizing()
    local x, y = root:GetLeft(), root:GetTop()
    if x and y then
        SetCVar('woweeRaidFramesX', format('%d', x + 0.5))
        SetCVar('woweeRaidFramesY', format('%d', y + 0.5))
    end
end)
handle:SetScript('OnMouseUp', function(_, button)
    if button ~= 'RightButton' then return end
    SetCVar('woweeRaidFramesLocked', locked() and '0' or '1')
    showHandle()
end)
handle:SetScript('OnEnter', function(self)
    GameTooltip:SetOwner(self, 'ANCHOR_TOPLEFT')
    GameTooltip:SetText('Group frames')
    GameTooltip:AddLine(locked() and 'Right-click to unlock.' or 'Drag to move. Right-click to lock.', 1, 1, 1)
    GameTooltip:Show()
    if locked() then handle:SetAlpha(1) end
end)
handle:SetScript('OnLeave', function()
    GameTooltip:Hide()
    showHandle()
end)

local function makeButton(i)
    local b = CreateFrame('Button', 'WoweeRaidFrameUnit' .. i, root, 'SecureUnitButtonTemplate')
    b:SetSize(W, H)
    b:RegisterForClicks('AnyUp')
    b:SetAttribute('type1', 'target')
    b:SetAttribute('*type2', 'menu')
    b.menu = function(self)
        local unit = self:GetAttribute('unit')
        if unit and ToggleDropDownMenu and FriendsDropDown then
            FriendsDropDown.unit = unit
            FriendsDropDown.name = UnitName(unit)
            FriendsDropDown.initialize = function() UnitPopup_ShowMenu(FriendsDropDown, unit == 'player' and 'SELF' or (UnitInRaid(unit) and 'RAID_PLAYER' or 'PARTY'), unit) end
            FriendsDropDown.displayMode = 'MENU'
            ToggleDropDownMenu(1, nil, FriendsDropDown, 'cursor')
        end
    end
    local bg = b:CreateTexture(nil, 'BACKGROUND')
    bg:SetAllPoints()
    bg:SetTexture(0.08, 0.08, 0.08, 0.85)
    local hp = CreateFrame('StatusBar', nil, b)
    hp:SetPoint('TOPLEFT', 1, -1)
    hp:SetPoint('BOTTOMRIGHT', -1, 5)
    hp:SetStatusBarTexture('Interface\\TargetingFrame\\UI-StatusBar')
    hp:SetMinMaxValues(0, 1)
    local mp = CreateFrame('StatusBar', nil, b)
    mp:SetPoint('TOPLEFT', hp, 'BOTTOMLEFT', 0, -1)
    mp:SetPoint('BOTTOMRIGHT', -1, 1)
    mp:SetStatusBarTexture('Interface\\TargetingFrame\\UI-StatusBar')
    mp:SetMinMaxValues(0, 1)
    local name = hp:CreateFontString(nil, 'OVERLAY', 'GameFontHighlightSmall')
    name:SetPoint('TOP', 0, -3)
    name:SetWidth(W - 4)
    name:SetHeight(12)
    local status = hp:CreateFontString(nil, 'OVERLAY', 'GameFontNormalSmall')
    status:SetPoint('BOTTOM', 0, 2)
    local aggro = b:CreateTexture(nil, 'OVERLAY')
    aggro:SetPoint('TOPLEFT', -1, 1)
    aggro:SetPoint('BOTTOMRIGHT', 1, -1)
    aggro:SetTexture(1, 0, 0, 0.0)
    b.hp, b.mp, b.name, b.status, b.aggro = hp, mp, name, status, aggro
    b:Hide()
    return b
end

local function unitsWanted()
    local list = {}
    local nRaid = GetNumRaidMembers and GetNumRaidMembers() or 0
    if nRaid > 0 then
        for i = 1, nRaid do
            local _, _, subgroup = GetRaidRosterInfo(i)
            list[#list + 1] = { unit = 'raid' .. i, group = subgroup or 1 }
        end
    elseif (GetNumPartyMembers and GetNumPartyMembers() or 0) > 0 then
        list[1] = { unit = 'player', group = 1 }
        for i = 1, GetNumPartyMembers() do list[#list + 1] = { unit = 'party' .. i, group = 1 } end
    end
    return list
end

local function setPartyFramesHidden(hide)
    if hide then
        if HidePartyFrame then HidePartyFrame() end
    elseif ShowPartyFrame then
        ShowPartyFrame()
    end
end

local function layout()
    if InCombatLockdown() then rf.pending = true; return end
    rf.pending = false
    local list = rf.enabled and unitsWanted() or {}
    local perGroup, cols = {}, 0
    for i = 1, MAX do
        local b = rf.buttons[i] or makeButton(i)
        rf.buttons[i] = b
        local want = list[i]
        if want then
            b:SetAttribute('unit', want.unit)
            local g = want.group
            perGroup[g] = (perGroup[g] or 0) + 1
            if g > cols then cols = g end
            b:ClearAllPoints()
            b:SetPoint('TOPLEFT', root, 'TOPLEFT', (g - 1) * (W + GAP), -(perGroup[g] - 1) * (H + GAP))
            b:Show()
        else
            b:SetAttribute('unit', nil)
            b:Hide()
        end
    end
    local rows = 0
    for _, n in pairs(perGroup) do if n > rows then rows = n end end
    if #list > 0 then
        root:SetSize(math.max(cols, 1) * (W + GAP) - GAP, math.max(rows, 1) * (H + GAP) - GAP)
        root:Show()
    else
        root:Hide()
    end
    setPartyFramesHidden(rf.enabled)
end
rf.layout = layout

local function refresh(b)
    local unit = b:GetAttribute('unit')
    if not unit or not UnitExists(unit) then return end
    local _, class = UnitClass(unit)
    local c = COLORS[class or ''] or { 0.5, 0.5, 0.5 }
    local hp, hpMax = UnitHealth(unit) or 0, UnitHealthMax(unit) or 0
    local frac = hpMax > 0 and hp / hpMax or 0
    b.hp:SetValue(frac)
    b.name:SetText(UnitName(unit) or '')
    local mp, mpMax = UnitPower(unit) or 0, UnitPowerMax(unit) or 0
    b.mp:SetValue(mpMax > 0 and mp / mpMax or 0)
    local pc = POWER[UnitPowerType(unit) or 0] or POWER[0]
    b.mp:SetStatusBarColor(pc[1], pc[2], pc[3])
    if not UnitIsConnected(unit) then
        b.hp:SetStatusBarColor(0.4, 0.4, 0.4); b.status:SetText('Offline')
    elseif UnitIsGhost and UnitIsGhost(unit) then
        b.hp:SetStatusBarColor(0.4, 0.4, 0.4); b.status:SetText('Ghost')
    elseif UnitIsDeadOrGhost(unit) then
        b.hp:SetStatusBarColor(0.4, 0.4, 0.4); b.status:SetText('Dead')
    else
        b.hp:SetStatusBarColor(c[1], c[2], c[3])
        local missing = hpMax - hp
        b.status:SetText(missing > 0 and format('-%.1fk', missing / 1000) or '')
    end
    local threat = UnitThreatSituation and UnitThreatSituation(unit) or 0
    b.aggro:SetTexture(1, 0, 0, (threat and threat >= 2) and 0.55 or 0.0)
    local inRange = unit == 'player' or not UnitInRange or UnitInRange(unit)
    b:SetAlpha(inRange and 1.0 or 0.45)
end

root:SetScript('OnUpdate', function(_, elapsed)
    rf.since = rf.since + elapsed
    if rf.since < REFRESH then return end
    rf.since = 0
    for i = 1, MAX do
        local b = rf.buttons[i]
        if b and b:IsShown() then refresh(b) end
    end
end)

function WoweeRaidFrames_Apply()
    rf.enabled = cvarOn('woweeRaidFrames')
    placeRoot()
    showHandle()
    layout()
end

local events = CreateFrame('Frame', 'WoweeRaidFramesEvents')
events:RegisterEvent('PLAYER_ENTERING_WORLD')
events:RegisterEvent('PARTY_MEMBERS_CHANGED')
events:RegisterEvent('RAID_ROSTER_UPDATE')
events:RegisterEvent('PLAYER_REGEN_ENABLED')
events:SetScript('OnEvent', function(_, event)
    if event == 'PLAYER_ENTERING_WORLD' then
        if not rf.hooked and hooksecurefunc and PartyMemberFrame_UpdateMember then
            rf.hooked = true
            -- The party frames show themselves again on every roster change.
            hooksecurefunc('PartyMemberFrame_UpdateMember', function(self)
                if rf.enabled and not InCombatLockdown() then self:Hide() end
            end)
        end
        WoweeRaidFrames_Apply()
    elseif event == 'PLAYER_REGEN_ENABLED' then
        if rf.pending then layout() end
    else
        layout()
    end
end)
)LUA";

} // namespace addons
} // namespace wowee
