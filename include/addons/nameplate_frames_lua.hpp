#pragma once

/**
 * nameplate_frames_lua.hpp - the stock nameplate frames, in the interface's Lua.
 *
 * In 3.3.5a a nameplate is a frame: an unnamed child of WorldFrame, made by the
 * client when a unit first needs one and reused after, with two status bars
 * (health, cast) and eleven regions in a fixed order. The client positions it
 * over the unit every frame and writes the unit's state into those regions -
 * the bar's value and colour, the name, the level and its colour, the
 * highlight, the threat glow, the raid icon's texture coordinates, the cast.
 *
 * Nameplate addons are built on exactly that. ElvUI, TidyPlates, Aloft and the
 * rest walk WorldFrame:GetChildren(), recognise a plate by its first region's
 * texture, hide or restyle the stock regions, hang their own frames off the
 * plate, and *read* the unit's state back out of the stock regions - the
 * reaction from the health bar's colour, the level from the level text, the
 * mouseover from the highlight being shown, the raid mark from the icon's
 * texture coordinates.
 *
 * This client draws its plates itself (GameScreen::renderNameplates) and had
 * no such frames, so every one of those addons found nothing. These are the
 * frames, kept in step with the plates the client draws. The client still
 * draws the stock look, element by element, for as long as the element's
 * region is shown with its art: an addon that hides a region or clears its
 * texture - which is how they all take a plate over - stops that part being
 * drawn, as it would on the stock client.
 *
 * The regions' own pictures are therefore not drawn by the widget renderer
 * (they are marked client-drawn); anything an addon creates on or under the
 * plate is, like any other frame.
 *
 *   __WoweeNameplateAcquire(key)  -> the plate frame for a unit, made or reused,
 *                                    plus the widget ids of its parts
 *   __WoweeNameplateUpdate(key, ...) the unit's state, written into the parts
 *   __WoweeNameplateRelease(key)    the unit is gone: hide the plate, keep it
 */

namespace wowee {
namespace addons {

inline constexpr const char* kNameplateFramesLua = R"LUA(
local pool, byKey, free = {}, {}, {}

local function art(parent, layer, path)
    local t = parent:CreateTexture(nil, layer)
    if path then t:SetTexture(path) end
    __WoweeSetClientDrawn(t, true)
    return t
end

local function newPlate()
    local f = CreateFrame("Frame", nil, WorldFrame)
    f:SetSize(128, 32)
    f:SetFrameStrata("BACKGROUND")
    -- Children: health, then cast, as GetChildren answers them.
    local health = CreateFrame("StatusBar", nil, f)
    health:SetStatusBarTexture("Interface\\TargetingFrame\\UI-TargetingFrame-BarFill")
    health:SetPoint("BOTTOMLEFT", f, "BOTTOMLEFT", 4, 4)
    health:SetSize(103, 9)
    health:SetMinMaxValues(0, 1)
    health:SetValue(1)
    __WoweeSetClientDrawn(health, true)
    local cast = CreateFrame("StatusBar", nil, f)
    cast:SetStatusBarTexture("Interface\\TargetingFrame\\UI-TargetingFrame-BarFill")
    cast:SetPoint("TOPRIGHT", f, "BOTTOMRIGHT", -4, -4)
    cast:SetSize(103, 9)
    cast:SetStatusBarColor(1, 0.7, 0)
    cast:Hide()
    __WoweeSetClientDrawn(cast, true)
    -- Regions, in the client's order.
    local threat = art(f, "BORDER", "Interface\\TargetingFrame\\UI-TargetingFrame-Flash")
    threat:SetTexCoord(0, 0.555, 0.53, 0.6)
    threat:SetAllPoints(f)
    threat:Hide()
    local border = art(f, "BORDER", "Interface\\Tooltips\\Nameplate-Border")
    border:SetAllPoints(f)
    local castBorder = art(f, "BORDER", "Interface\\Tooltips\\Nameplate-Border")
    castBorder:SetAllPoints(cast)
    castBorder:Hide()
    local castShield = art(f, "ARTWORK", "Interface\\Tooltips\\Nameplate-CastBar-Shield")
    castShield:SetAllPoints(cast)
    castShield:Hide()
    local castIcon = art(f, "OVERLAY")
    castIcon:SetSize(13, 13)
    castIcon:SetPoint("LEFT", cast, "LEFT", 2, 0)
    castIcon:Hide()
    local highlight = art(f, "ARTWORK", "Interface\\Tooltips\\Nameplate-Glow")
    highlight:SetBlendMode("ADD")
    highlight:SetAllPoints(f)
    highlight:Hide()
    local name = f:CreateFontString(nil, "ARTWORK", "GameFontWhite")
    name:SetPoint("BOTTOM", f, "CENTER", 0, 0)
    __WoweeSetClientDrawn(name, true)
    local level = f:CreateFontString(nil, "ARTWORK", "GameFontNormalSmall")
    level:SetPoint("CENTER", f, "BOTTOMRIGHT", -12, 9)
    __WoweeSetClientDrawn(level, true)
    local boss = art(f, "ARTWORK", "Interface\\TargetingFrame\\UI-TargetingFrame-Skull")
    boss:SetSize(13, 13)
    boss:SetPoint("CENTER", level, "CENTER")
    boss:Hide()
    local raid = art(f, "ARTWORK", "Interface\\TargetingFrame\\UI-RaidTargetingIcons")
    raid:SetSize(26, 26)
    raid:SetPoint("RIGHT", f, "LEFT", 0, 0)
    raid:Hide()
    local elite = art(f, "ARTWORK", "Interface\\Tooltips\\EliteNameplateIcon")
    elite:SetTexCoord(0, 0.578125, 0, 0.84375)
    elite:SetSize(37, 27)
    elite:SetPoint("CENTER", level, "CENTER", 4, -1)
    elite:Hide()
    f.__plate = {
        health = health, cast = cast, threat = threat, border = border,
        castBorder = castBorder, castShield = castShield, castIcon = castIcon,
        highlight = highlight, name = name, level = level, boss = boss,
        raid = raid, elite = elite, state = {},
    }
    f:Hide()
    pool[#pool + 1] = f
    return f
end

local function ids(f)
    local p = f.__plate
    return f.__wid, p.health.__wid, p.cast.__wid, p.threat.__wid, p.border.__wid,
           p.castBorder.__wid, p.castShield.__wid, p.castIcon.__wid,
           p.highlight.__wid, p.name.__wid, p.level.__wid, p.boss.__wid,
           p.raid.__wid, p.elite.__wid
end

function __WoweeNameplateAcquire(key)
    local f = byKey[key]
    if not f then
        f = table.remove(free) or newPlate()
        byKey[key] = f
        wipe(f.__plate.state)
        f:Show()
    end
    return ids(f)
end

function __WoweeNameplateRelease(key)
    local f = byKey[key]
    if not f then return end
    byKey[key] = nil
    f:Hide()
    free[#free + 1] = f
end

-- Only what changed is written, so a plate whose unit stands still costs a
-- comparison per field rather than a dozen setter calls - and the scripts an
-- addon hooked on them (OnValueChanged, OnShow) fire when the state moves,
-- not every frame.
local function shown(region, on)
    if on then
        if not region:IsShown() then region:Show() end
    elseif region:IsShown() then
        region:Hide()
    end
end

function __WoweeNameplateUpdate(key, x, y, alpha,
                                hp, hpMax, hr, hg, hb,
                                nameText, nr, ng, nb,
                                levelText, lr, lg, lb, skull, elite,
                                highlight, threat, tr, tg, tb,
                                raidIndex, casting, castValue, castMax,
                                shielded, castTexture)
    local f = byKey[key]
    if not f then return end
    local p, s = f.__plate, f.__plate.state
    local scale = f:GetEffectiveScale()
    if scale <= 0 then scale = 1 end
    x, y = x / scale, y / scale
    if s.x ~= x or s.y ~= y then
        s.x, s.y = x, y
        f:ClearAllPoints()
        f:SetPoint("TOP", WorldFrame, "BOTTOMLEFT", x, y)
    end
    if f:GetAlpha() ~= alpha then f:SetAlpha(alpha) end

    if s.hpMax ~= hpMax then s.hpMax = hpMax; p.health:SetMinMaxValues(0, hpMax) end
    if s.hp ~= hp then s.hp = hp; p.health:SetValue(hp) end
    if s.hr ~= hr or s.hg ~= hg or s.hb ~= hb then
        s.hr, s.hg, s.hb = hr, hg, hb
        p.health:SetStatusBarColor(hr, hg, hb)
    end
    if s.name ~= nameText then s.name = nameText; p.name:SetText(nameText) end
    if s.nr ~= nr or s.ng ~= ng or s.nb ~= nb then
        s.nr, s.ng, s.nb = nr, ng, nb
        p.name:SetTextColor(nr, ng, nb)
    end
    if s.level ~= levelText then s.level = levelText; p.level:SetText(levelText) end
    if s.lr ~= lr or s.lg ~= lg or s.lb ~= lb then
        s.lr, s.lg, s.lb = lr, lg, lb
        p.level:SetTextColor(lr, lg, lb)
    end
    if s.skull ~= skull then
        s.skull = skull
        shown(p.boss, skull)
        shown(p.level, not skull)
    end
    if s.elite ~= elite then s.elite = elite; shown(p.elite, elite) end
    if s.highlight ~= highlight then s.highlight = highlight; shown(p.highlight, highlight) end
    if s.threat ~= threat or s.tr ~= tr or s.tg ~= tg or s.tb ~= tb then
        s.threat, s.tr, s.tg, s.tb = threat, tr, tg, tb
        if threat then p.threat:SetVertexColor(tr, tg, tb) end
        shown(p.threat, threat)
    end
    if s.raid ~= raidIndex then
        s.raid = raidIndex
        if raidIndex and raidIndex > 0 then
            SetRaidTargetIconTexture(p.raid, raidIndex)
            shown(p.raid, true)
        else
            shown(p.raid, false)
        end
    end
    if s.casting ~= casting then
        s.casting = casting
        shown(p.cast, casting)
    end
    if casting then
        if s.castMax ~= castMax then s.castMax = castMax; p.cast:SetMinMaxValues(0, castMax) end
        if s.castValue ~= castValue then s.castValue = castValue; p.cast:SetValue(castValue) end
        if s.shielded ~= shielded then
            s.shielded = shielded
            shown(p.castShield, shielded)
            shown(p.castBorder, not shielded)
        end
        if s.castTexture ~= castTexture then
            s.castTexture = castTexture
            p.castIcon:SetTexture(castTexture)
            shown(p.castIcon, castTexture ~= nil)
        end
    end
end
)LUA";

}  // namespace addons
}  // namespace wowee
