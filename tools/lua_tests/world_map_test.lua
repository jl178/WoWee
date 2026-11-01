-- World map test for the stock-FrameXML map (lua_world_map_api.cpp).
--
-- Run from the build's bin directory with a character in Elwynn Forest that
-- holds Northshire quests:
--   WOWEE_DEBUG_LUA=../../tools/lua_tests/world_map_test.lua ./wowee
-- Every check logs PASS or FAIL under [luatest]; Lua errors are caught and
-- logged too; the client quits when it is done.
-- Screenshots land beside the binary (WoweeDebug_Screenshot takes a path).
local SHOTS = "world_map_"
local log = WoweeDebug_Log
local fails = 0

if seterrorhandler then
    seterrorhandler(function(msg) log("LUA ERROR (handler): " .. tostring(msg)) end)
end

local queue = {}
local function after(sec, name, fn) queue[#queue + 1] = {sec, name, fn} end
local function check(what, ok, detail)
    if not ok then fails = fails + 1 end
    log((ok and "PASS " or "FAIL ") .. what .. (detail and ("  [" .. detail .. "]") or ""))
end
local function view()
    local file = GetMapInfo()
    return string.format("continent=%s zone=%s areaID=%s map=%s level=%s/%s",
        tostring(GetCurrentMapContinent()), tostring(GetCurrentMapZone()),
        tostring(GetCurrentMapAreaID()), tostring(file),
        tostring(GetCurrentMapDungeonLevel()), tostring(GetNumDungeonMapLevels()))
end
local function shot(name)
    log("SHOT " .. name)
    WoweeDebug_Screenshot(SHOTS .. name .. ".png")
end

-- Elwynn Forest on the Eastern Kingdoms map, worked out from WorldMapArea
-- (Elwynn's centre against Azeroth's bounds).
local ELWYNN_X, ELWYNN_Y = 0.4509, 0.7467
local elwynnIndex
for i, name in ipairs({GetMapZones(2)}) do
    if name == "Elwynn Forest" then elwynnIndex = i end
end

after(0, "open with the TOGGLEWORLDMAP binding", function()
    log("continents: " .. table.concat({GetMapContinents()}, ", "))
    RunBinding("TOGGLEWORLDMAP")
    check("map opens", WorldMapFrame:IsShown())
end)
after(1.5, "zone state", function()
    log("[open] " .. view())
    check("opens on the player's zone (Elwynn)", (GetMapInfo()) == "Elwynn", view())
    local px, py = GetPlayerMapPosition("player")
    check("player is on the zone map", px > 0 and py > 0, string.format("%.4f,%.4f", px, py))
    log("[open] overlays=" .. GetNumMapOverlays() .. " landmarks=" .. GetNumMapLandmarks())
    for i = 1, GetNumMapLandmarks() do
        local name, _, tex, x, y = GetMapLandmarkInfo(i)
        log(string.format("[open]   landmark %s tex=%s at %.3f,%.3f", name, tostring(tex), x, y))
    end
    local n = QuestMapUpdateAllQuests()
    QuestPOIUpdateIcons()
    log("[open] quests with POIs on this map: " .. n)
    for i = 1, n do
        local q, li = QuestPOIGetQuestIDByVisibleIndex(i)
        local done, x, y, obj = QuestPOIGetIconInfo(q)
        log(string.format("[open]   %d quest %s (log %s) %s icon %.4f,%.4f objective %s",
            i, tostring(q), tostring(li), done and "complete" or "open", x or -1, y or -1, tostring(obj)))
    end
    if WorldMapQuestFrame2 and WorldMapQuestFrame2:IsShown() then
        WorldMapFrame_SelectQuestFrame(WorldMapQuestFrame2)
    end
end)
after(2.2, "zone shot", function() shot("zone") end)

after(3.0, "continent", function()
    SetMapZoom(2)
    log("[SetMapZoom(2)] " .. view())
    check("SetMapZoom(2) shows Eastern Kingdoms", GetCurrentMapContinent() == 2 and GetCurrentMapZone() == 0
        and (GetMapInfo()) == "Azeroth", view())
    local name, file = UpdateMapHighlight(ELWYNN_X, ELWYNN_Y)
    check("highlight under the cursor is Elwynn", name == "Elwynn Forest" and file == "Elwynn",
        tostring(name) .. "/" .. tostring(file))
end)
after(3.5, "continent shot", function() shot("continent") end)
after(4.0, "click Elwynn", function()
    ProcessMapClick(ELWYNN_X, ELWYNN_Y)
    log("[ProcessMapClick] " .. view())
    check("ProcessMapClick opens Elwynn", GetCurrentMapZone() == elwynnIndex and (GetMapInfo()) == "Elwynn", view())
    ZoomOut()
    log("[ZoomOut()] " .. view())
    check("ZoomOut() leaves a zone map alone (stock: only dungeon floors)", (GetMapInfo()) == "Elwynn", view())
    WorldMapZoomOutButton_OnClick()
    log("[zoom-out button] " .. view())
    check("zoom-out button: zone -> continent", GetCurrentMapContinent() == 2 and GetCurrentMapZone() == 0, view())
    WorldMapZoomOutButton_OnClick()
    log("[zoom-out button] " .. view())
    check("zoom-out button: continent -> world", GetCurrentMapContinent() == 0 and GetMapInfo() == nil, view())
end)
after(4.5, "world shot", function() shot("world") end)
after(5.0, "world click", function()
    local name = UpdateMapHighlight(0.80, 0.50)
    ProcessMapClick(0.80, 0.50)
    check("click on the world map opens that continent", GetCurrentMapContinent() == 2,
        tostring(name) .. " -> " .. view())
    WorldMapZoomOutButton_OnClick()
    WorldMapZoomOutButton_OnClick()
    check("zoom-out button: world -> cosmic", GetCurrentMapContinent() == -1 and GetMapInfo() == nil
        and not IsZoomOutAvailable(), view())
    SetMapZoom(0)
end)

-- Real mouse events through the client's input path.
after(5.5, "right click", function()
    SetMapZoom(2, elwynnIndex)
    WoweeDebug_Mouse("WorldMapButton", "RightButton", false, 0.2, 0.2)
end)
after(5.8, "right down", function() WoweeDebug_Mouse("WorldMapButton", "RightButton", true, 0.2, 0.2) end)
after(6.1, "right up", function() WoweeDebug_Mouse("WorldMapButton", "RightButton", false, 0.2, 0.2) end)
after(6.6, "right check", function()
    check("right-click on the map zooms out", GetCurrentMapContinent() == 2 and GetCurrentMapZone() == 0, view())
end)
after(6.6, "button move", function() WoweeDebug_Mouse("WorldMapZoomOutButton", "LeftButton", false) end)
after(6.9, "button down", function()
    zoomFocus = GetMouseFocus() and GetMouseFocus():GetName()
    log("mouse focus before the press: " .. tostring(zoomFocus))
    WoweeDebug_Mouse("WorldMapZoomOutButton", "LeftButton", true)
end)
after(7.2, "button up", function() WoweeDebug_Mouse("WorldMapZoomOutButton", "LeftButton", false) end)
after(7.7, "button check", function()
    if zoomFocus == "WorldMapZoomOutButton" then
        check("clicking the Zoom Out button works", GetCurrentMapContinent() == 0, view())
    else
        -- WorldMapTitleButton is anchored to the (hidden) small-map border at
        -- the frame's top-left and is 544 wide, so in a window this narrow it
        -- lies over the Zoom Out button - in the stock layout as well.
        log("SKIP clicking the Zoom Out button: " .. tostring(zoomFocus) .. " covers it at this window width")
    end
    SetMapZoom(2)
    WoweeDebug_Mouse("WorldMapButton", "LeftButton", false, 0.465, 0.77)
end)
after(8.0, "left down", function() WoweeDebug_Mouse("WorldMapButton", "LeftButton", true, 0.465, 0.77) end)
after(8.3, "left up", function() WoweeDebug_Mouse("WorldMapButton", "LeftButton", false, 0.465, 0.77) end)
after(8.8, "left check", function()
    check("left-click on a zone of the continent opens it", (GetMapInfo()) == "Elwynn", view())
end)

-- Dungeon floors and instance maps.
after(9.2, "dalaran", function()
    SetMapByID(504)
    log("[SetMapByID(504)] " .. view())
    check("Dalaran opens on its first floor", (GetMapInfo()) == "Dalaran" and GetCurrentMapDungeonLevel() == 1
        and GetNumDungeonMapLevels() == 2 and GetCurrentMapContinent() == 4, view())
    SetDungeonMapLevel(2)
    check("SetDungeonMapLevel(2)", GetCurrentMapDungeonLevel() == 2, view())
end)
after(9.6, "dalaran shot", function() shot("dalaran") end)
after(10.0, "dalaran zoom", function()
    ZoomOut()
    log("[ZoomOut() from Dalaran] " .. view())
    -- DungeonMap gives both Dalaran floors ParentWorldMapID 495 (The Storm
    -- Peaks), and ZoomOut() (0x00546A90) follows it.
    check("ZoomOut() from a Dalaran floor goes to its DungeonMap parent (495, The Storm Peaks)",
        (GetMapInfo()) == "TheStormPeaks", view())
    SetMapByID(401)
    log("[SetMapByID(401)] " .. view())
    check("Alterac Valley map", (GetMapInfo()) == "AlteracValley" and not IsZoomOutAvailable(), view())
    SetMapToCurrentZone()
    check("SetMapToCurrentZone returns to Elwynn", (GetMapInfo()) == "Elwynn", view())
    SetMapZoom(4)
    WorldMap_OpenToQuest(18)
    check("the quest log's Show Map (WorldMap_OpenToQuest) opens the quest's zone with it selected",
        (GetMapInfo()) == "Elwynn" and WORLDMAP_SETTINGS.selectedQuestId == 18,
        view() .. " selected " .. tostring(WORLDMAP_SETTINGS.selectedQuestId) .. " area " .. tostring(GetQuestWorldMapAreaID(18)))
end)

-- Closing with the keys, through the SDL queue like a real key press.
after(10.5, "M down", function() WoweeDebug_PushKey("M", true) end)
after(10.7, "M up", function() WoweeDebug_PushKey("M", false) end)
after(11.4, "M check", function()
    check("M closes the map", not WorldMapFrame:IsShown())
    if WorldMapFrame:IsShown() then HideUIPanel(WorldMapFrame) end
    RunBinding("TOGGLEWORLDMAP")
    check("the binding opens it again", WorldMapFrame:IsShown())
end)
after(12.0, "Esc down", function() WoweeDebug_PushKey("Escape", true) end)
after(12.2, "Esc up", function() WoweeDebug_PushKey("Escape", false) end)
after(12.9, "Esc check", function()
    check("Escape closes the map", not WorldMapFrame:IsShown())
    check("Escape did not also open the game menu", not (GameMenuFrame and GameMenuFrame:IsShown()))
    if GameMenuFrame and GameMenuFrame:IsShown() then HideUIPanel(GameMenuFrame) end
    if WorldMapFrame:IsShown() then HideUIPanel(WorldMapFrame) end
end)
after(13.5, "done", function()
    log("DONE, " .. fails .. " failure(s)")
    WoweeDebug_Quit()
end)

local runner = CreateFrame("Frame", "WoweeMapTestRunner", WorldFrame)
runner:Show()
local elapsedTotal, idx = 0, 1
runner:SetScript("OnUpdate", function(self, elapsed)
    elapsedTotal = elapsedTotal + (elapsed or 0)
    while queue[idx] and elapsedTotal >= queue[idx][1] do
        local step = queue[idx]
        idx = idx + 1
        local ok, err = pcall(step[3])
        if not ok then
            fails = fails + 1
            log("LUA ERROR in step '" .. step[2] .. "': " .. tostring(err))
        end
    end
end)
log("map test scheduled, " .. #queue .. " steps")
