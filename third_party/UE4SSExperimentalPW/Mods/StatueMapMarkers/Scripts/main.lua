local Config = require("config")
local Common = require("common")

local MOD_NAME = "StatueMapMarkers"
local MOD_VERSION = "1.2.3"
-- 1.2.3: stop FindFirstOf("WBP_Map_Base_C") every second from world load until
-- the first map open. Wait for mapSessionActive (Construct / visible wake);
-- only GUObjectArray-scan when Notify is missing or the session is open.
-- 1.2.2 idle hitch fix + 1.2.1 crash hardening (1.1.8 report): drop global UserWidget Construct/Destruct
-- hooks (ProcessEvent recursion), cancel icon queues with a session token when the
-- map is hidden or replaced (never IsVisible, not IsInViewport alone), check
-- LocationManager GUID before StaticConstructObject and reuse the existing point.
-- Keep 2-arg On Added Location (zeroed LocationUIData hid compass icons).
-- Markers still persist across map close and reattach instead of a full
-- 407-widget rebuild.
-- 1.2.0 fixes a 1.1.9 regression where effigies never appeared on the map.
-- 1.1.9 gated the map-icon maintenance pass behind an on-screen map session,
-- but that also gated the FindFirstOf("WBP_Map_Base_C") lookup that the very
-- first marker build needs. On setups where mapSessionActive never flips true
-- (pooled map widget with no Construct on reopen, SetLocationData bursts while
-- the map is hidden) the 407 icons were therefore never created. (1.2.3 supersedes the 1.2.0 idle ungating of FindFirstOf while
-- nativeIconMapName is nil.)
-- 1.1.9 cuts idle-walk stutter: the world/server-transition check is now driven
-- by compass/map creation notifications (30s poll kept only as fallback) instead
-- of a 5s FindFirstOf poll, and the map-icon maintenance pass (body-switch /
-- self-heal) is gated behind an on-screen map session so it no longer runs a
-- Blueprint GetCurrentMapBody call every 5s while the map is closed. Collection
-- detection and compass removal are unaffected (they run off the pickup hook and
-- refreshCompassMarkers, not this maintenance pass).
-- 1.1.8 resumes the map marker session from visible WBP_Map_Base when a
-- pooled map widget does not emit UserWidget:Construct on open.
-- 1.1.5 keeps Debug logging-only. The removed SetHideFlag hook and icon census
-- called reflected functions while map widgets were being created/destroyed;
-- on some UE4SS builds that could terminate the process outside Lua pcall.

-- UE4SS stores callbacks as Lua registry references. Keep every callback that
-- outlives this chunk in one strongly-referenced table; short-lived anonymous
-- callbacks can otherwise become invalid when UE4SS runs callback GC.
local Callbacks = {}
_G.__STATUE_MAP_MARKERS_CALLBACKS = Callbacks
Callbacks.noop = function() end

local Log = Common.createLogger(MOD_NAME, Config.Debug)
local log = Log.info

local unwrap = Common.unwrapObjectRef

local function asNumber(value)
    if value == nil then return nil end
    local ok, result = pcall(function() return value:get() end)
    if ok then
        local number = tonumber(result)
        if number then return number end
    end
    return tonumber(value)
end

-- Reflected bool properties are plain Lua booleans on stable UE4SS builds,
-- but experimental builds may expose them through a RemoteUnrealParam-like
-- wrapper. In particular, comparing that wrapper directly with true makes
-- every collected relic look uncollected.
local function asBoolean(value)
    if type(value) == "boolean" then return value end
    if type(value) == "number" then return value ~= 0 end
    if value == nil then return nil end

    for _, getterName in ipairs({ "get", "Get" }) do
        local ok, result = pcall(function()
            return value[getterName](value)
        end)
        if ok then
            if type(result) == "boolean" then return result end
            if type(result) == "number" then return result ~= 0 end
        end
    end

    local number = tonumber(value)
    if number ~= nil then return number ~= 0 end
    return nil
end

local isValidObject = Common.isValid
local isMissingPropDummy = Common.isMissingPropertyDummy

-- v1.1.3: SetRenderScale writes an FVector2D ScriptStruct on the inner Icon
-- widget. Every zoom/reset/creation path validated only the parent icon, not
-- icon.Icon, so a stale inner-widget wrapper reached the struct write. That
-- crashes UE4SS inside handle_unreal_property_value while it builds the Lua
-- error traceback (findfield/luaH_next AV) -- and because the crash is in the
-- traceback builder, the surrounding pcall cannot catch it. Resolve and
-- validate the inner Icon here so no struct method is ever invoked on a stale
-- wrapper.
local function safeIconImage(icon)
    if not icon or not isValidObject(icon) then return nil end
    local image
    local ok = pcall(function() image = icon.Icon end)
    if not ok or not image or not isValidObject(image) then return nil end
    return image
end

local nativeIconSize = tonumber(Config.NativeIconSize) or 12
if nativeIconSize < 1 then nativeIconSize = 1 end
if nativeIconSize > 64 then nativeIconSize = 64 end

local collectedIconOpacity = tonumber(Config.CollectedIconOpacity) or 0.45
if collectedIconOpacity < 0 then collectedIconOpacity = 0 end
if collectedIconOpacity > 1 then collectedIconOpacity = 1 end
local hideCollectedIcons = Config.HideCollectedIcons == true
local collectedIconGrayscale = Config.CollectedIconGrayscale ~= false

local uncollectedIconBrightness =
    tonumber(Config.UncollectedIconBrightness) or 1.25
if uncollectedIconBrightness < 0 then uncollectedIconBrightness = 0 end
if uncollectedIconBrightness > 3 then uncollectedIconBrightness = 3 end

-- Follow map zoom more gently than the map itself.
local iconZoomExponent = 0.5

local stateScanIntervalSeconds = math.max(1,
    (tonumber(Config.StateScanIntervalMs) or 10000) / 1000)

-- Diagnostics: periodic counters written to UE4SS.log (disabled by default;
-- set DiagLogIntervalMs in config.lua when investigating an issue). Counter
-- increments are O(1); the expensive FindAllOf census runs only per interval.
local diagIntervalMs = math.floor(tonumber(Config.DiagLogIntervalMs) or 0)
local diagStartTime = os.time()
local Diag = {
    closeFires = 0,       -- OnClosed/OnClose hook fires (per interval)
    setLocationFires = 0, -- PalUIWorldMapIcon:SetLocationData fires (per interval)
    compassAdds = 0,      -- cumulative compass markers created
    compassRemoves = 0,   -- cumulative compass markers removed
    iconsCreated = 0,     -- cumulative native map icons created
    queueBuilds = 0,      -- cumulative 407-icon queue rebuilds
    queueCancels = 0,     -- cumulative icon-queue cancellations
    tickCount = 0,        -- game-thread ticks this interval
    tickTotalMs = 0,      -- total game-thread tick time this interval
    tickMaxMs = 0,        -- worst single tick this interval
}

local cachedRelicActors = nil
local relicInfoCache = {}
local relicSpatialGrid = {}
local compassGridCellMeters = math.max(1,
    tonumber(Config.CompassGridCellMeters) or 300)
local compassGridCellUnits = compassGridCellMeters * 100.0
local activeWorldKey = nil
-- Set by the compass/map creation notifications so the next game-thread tick
-- runs one world-scope check. World/server transition is an event, not a
-- continuous state, so this replaces the former 5s FindFirstOf poll.
local worldScopeCheckRequested = false
local nextRelicStateScanAt = 0
local relicStateScanCursor = 1
local relicStateScanInProgress = false
local relicStateScanBatchSize = math.max(1,
    math.floor(tonumber(Config.StateScanBatchSize) or 8))
local relicCacheNotBefore = 0
local relicCachePendingLogged = false
local pickedPropWarned = false
local lastRelicFlagUpdateAt = 0
local pickupHookInstalled = false
local pendingPickupChecks = {}
local pickupRecheckAt = 0
local pickupRecheckDelaySeconds = math.max(1,
    (tonumber(Config.PickupRecheckDelayMs) or 1000) / 1000)
local pickupRecheckMaxAttempts = math.max(1,
    math.floor(tonumber(Config.PickupRecheckMaxAttempts) or 3))
-- Before the first map build in a world, verify replicated collection flags
-- and only then construct widgets. This state is map-only: compass/gameplay
-- behavior otherwise remains independent.
local mapInitialStateReady = false
local mapInitialStateScanActive = false
local mapInitialStateScanCursor = 1
local mapInitialStateList = {}
local mapInitialStateNotBefore = 0
local mapInitialStateDelaySeconds = math.max(0,
    (tonumber(Config.MapInitialStateDelayMs) or 3000) / 1000)
local mapInitialStateBatchSize = math.max(1,
    math.floor(tonumber(Config.MapInitialStateBatchSize) or 64))
local mapInitialStateQuietSeconds = math.max(0,
    (tonumber(Config.MapInitialStateQuietMs) or 1500) / 1000)

local nativeIcons = {}
local nativeIconsNeedReattach = false
local nativeIconBuildIncomplete = false
local nativeIconMapName = nil
local nativeIconQueue = nil
local nativeIconQueueIndex = 1
local nativeIconQueueToken = 0
local nativeIconQueueActiveToken = 0
local nativeIconFailed = 0
local nativeIconPointClass = nil
local nativePendingPoint = nil
local nativeCapturedIcon = nil
local nativeRelicTextures = {}
local nativeLayoutLogged = false
local nativeMapBase = nil
local nativeMapBody = nil
-- CachedMapScale is an absolute scale (zoom step 0 == 1.0), not a value that
-- should be rebased each time the pooled map widget is opened.
local nativeBaseMapScale = 1.0
local nativeLastMapScale = nil
local mapMarkersVisible = Config.ShowOnMap ~= false
local notifiedMapBase = nil
local mapSessionActive = false
local mapNotifyInstalled = false
local mapConstructHookInstalled = false
local resetNativeMapState
local resetCompassState
local refreshCompassMarkers
local cancelNativeIconQueue
local compassElapsedMs = 0

-- Compass markers use Palworld's native level-object location and widget
-- path. Keep only a small nearest set registered so the 407 relics do not
-- overlap across the whole compass strip.
local compassMarkersVisible = Config.ShowInCompass ~= false
local compassMaxMarkers = math.max(0,
    math.floor(tonumber(Config.CompassMaxMarkers) or 5))
local compassRangeMeters = math.max(0,
    tonumber(Config.CompassRangeMeters) or 300)
local compassUpdateIntervalMs = math.max(1000,
    tonumber(Config.CompassUpdateIntervalMs) or 5000)
local compassHideCollected = Config.CompassHideCollected ~= false
local compassEntries = {}
local compassManager = nil
local compassWidget = nil
local compassManagerName = nil
local compassWidgetName = nil
local compassPointClass = nil
local compassLastCount = -1
local compassFailureCount = 0
local compassPlayer = nil
local compassStatusKey = nil
local notifiedCompassWidget = nil
local compassContextDirty = true
local compassLastPlayerX = nil
local compassLastPlayerY = nil
-- Skip the spatial candidate scan when the player has barely moved and the
-- compass is already full. Range-exit checks on existing markers still run.
local compassMoveSkipUnits = 2000.0 -- 20 m
local compassMoveSkipSquared = compassMoveSkipUnits * compassMoveSkipUnits

local function reportCompassStatus(key, detail)
    if compassStatusKey == key then return end
    compassStatusKey = key
    if detail and detail ~= "" then
        log("Compass status: " .. key .. " (" .. detail .. ")")
    else
        log("Compass status: " .. key)
    end
end

local function getLocalPlayerPawn()
    local controllers = nil
    pcall(function()
        controllers = FindAllOf("PlayerController")
            or FindAllOf("Controller")
    end)
    if controllers then
        for _, controller in ipairs(controllers) do
            if isValidObject(controller) then
                local isLocal = false
                pcall(function()
                    isLocal = controller:IsLocalPlayerController() == true
                end)
                if isLocal then
                    local pawn = nil
                    pcall(function() pawn = controller.Pawn end)
                    if pawn and isValidObject(pawn) then return pawn end
                end
            end
        end
    end
    return nil
end

local function getWidgetPlayerPawn(widget)
    if not widget or not isValidObject(widget) then return nil end
    local pawn = nil
    pcall(function()
        local controller = widget:GetOwningPlayer()
        if controller and isValidObject(controller) then
            pawn = controller.Pawn
        end
    end)
    if pawn and isValidObject(pawn) then return pawn end
    return nil
end

local function readStringValue(value)
    if value == nil then return nil end
    if type(value) == "string" then return value end
    local ok, result = pcall(function() return value:ToString() end)
    if ok and type(result) == "string" then return result end
    return nil
end

-- The selected save-directory name is stable for one local world or server
-- and changes when the player connects to a different one. Never use
-- tostring(FString): on UE4SS it may return a temporary wrapper address.
local function getWorldKey()
    local ok, gameInstance, value = pcall(function()
        local instance = FindFirstOf("PalGameInstance")
        if not instance or not isValidObject(instance) then return nil, nil end
        return instance, instance:GetSelectedWorldSaveDirectoryName()
    end)
    if not ok or not gameInstance then return nil end

    local key = readStringValue(value)
    if not key then
        local propertyOk, propertyValue = pcall(function()
            return gameInstance.SelectedWorldSaveDirectoryName
        end)
        if propertyOk then key = readStringValue(propertyValue) end
    end
    if not key or key == "" or key == "None"
        or key:match("^FString:%s*[0-9A-Fa-f]+$") then
        return nil
    end
    return key
end

local function resetRelicWorldState(reason)
    if resetCompassState then resetCompassState("world reset") end
    compassElapsedMs = compassUpdateIntervalMs
    compassPlayer = nil
    cachedRelicActors = nil
    relicInfoCache = {}
    relicSpatialGrid = {}
    nextRelicStateScanAt = 0
    relicStateScanCursor = 1
    relicStateScanInProgress = false
    relicCachePendingLogged = false
    pickedPropWarned = false
    lastRelicFlagUpdateAt = 0
    pendingPickupChecks = {}
    pickupRecheckAt = 0
    mapInitialStateReady = false
    mapInitialStateScanActive = false
    mapInitialStateScanCursor = 1
    mapInitialStateList = {}
    mapInitialStateNotBefore = 0
    if resetNativeMapState then resetNativeMapState() end
    if reason then log("Relic cache reset: " .. reason) end
end

local function refreshWorldScope()
    local worldKey = getWorldKey()
    if not worldKey then return end
    if activeWorldKey and activeWorldKey ~= worldKey then
        local previous = activeWorldKey
        activeWorldKey = worldKey
        resetRelicWorldState(string.format(
            "world changed (%s -> %s)", previous, worldKey))
        -- Let objects from the disconnected world reach UE garbage collection
        -- before FindAllOf builds the new server's cache.
        relicCacheNotBefore = os.time() + 3
        return
    end
    activeWorldKey = worldKey
end

local function markerVisibility(isPicked)
    if not mapMarkersVisible or (hideCollectedIcons and isPicked) then
        return 1 -- Collapsed
    end
    return 3 -- HitTestInvisible
end

local function applyNativeIconVisibility(icon, isPicked)
    if not icon or not isValidObject(icon) then return false end
    local visibility = markerVisibility(isPicked)
    pcall(function() icon:SetVisibility(visibility) end)
    -- PalUIWorldMapIcon can restore its root visibility during map updates.
    -- The inner image is not managed by that path, so applying the same state
    -- here keeps the map toggle (and collected filtering) persistent.
    pcall(function()
        if icon.Icon and isValidObject(icon.Icon) then
            icon.Icon:SetVisibility(visibility)
            -- Slate's disabled draw effect is used for grayscale. Some
            -- UE4SS/Palworld combinations render that disabled image once at
            -- its unscaled brush size even after it is collapsed, producing
            -- the large "duplicate" reported with the default hide-collected
            -- setup. Never leave a hidden image disabled.
            local applyGrayscale = collectedIconGrayscale and isPicked
                and visibility ~= 1
            icon.Icon:SetIsEnabled(not applyGrayscale)
        end
    end)
    return true
end

local function applyMarkerVisibility()
    local updated = 0
    for _, native in pairs(nativeIcons) do
        if applyNativeIconVisibility(native.icon, native.isPicked) then
            updated = updated + 1
        end
    end
    return updated
end

-- Top-level accessor so the 407-actor scan does not allocate a closure per
-- pcall (a measurable share of the former ~44 ms scan cost).
local function rawReadPicked(actor)
    return actor.bPickedInClient
end

local function readPicked(actor)
    local ok, value = pcall(rawReadPicked, actor)
    if not ok then return false end
    if isMissingPropDummy(value) then
        if not pickedPropWarned then
            pickedPropWarned = true
            log("bPickedInClient not found; collected state disabled")
        end
        return false
    end
    local picked = asBoolean(value)
    if picked == nil then
        if not pickedPropWarned then
            pickedPropWarned = true
            log("bPickedInClient could not be converted to a boolean; collected state disabled")
        end
        return false
    end
    return picked
end

local function getRelicPosition(actor)
    -- ObtainFXOrigin is attached to the visible statue and is more accurate
    -- than the Blueprint actor root for several relic placements.
    local ok, location = pcall(function()
        return actor:GetObtainFXLocation()
    end)
    if ok and location then
        local x, y, z = tonumber(location.X), tonumber(location.Y),
            tonumber(location.Z)
        if x and y then return x, y, z end
    end

    ok, location = pcall(function()
        return actor.StaticMesh_Base:K2_GetComponentLocation()
    end)
    if ok and location then
        local x, y, z = tonumber(location.X), tonumber(location.Y),
            tonumber(location.Z)
        if x and y then return x, y, z end
    end

    ok, location = pcall(function() return actor:K2_GetActorLocation() end)
    if not ok or not location then return nil end
    return tonumber(location.X), tonumber(location.Y), tonumber(location.Z)
end

local function ensureRelicCache()
    if cachedRelicActors and #cachedRelicActors > 0 then
        -- Fallback for UE4SS builds that cannot convert the world-key FString:
        -- actors from the disconnected world become invalid and must never be
        -- used to populate the next server's map.
        local first = cachedRelicActors[1]
        local last = cachedRelicActors[#cachedRelicActors]
        if isValidObject(first) and isValidObject(last) then return true end
        resetRelicWorldState("cached actors belong to an unloaded world")
    end

    if os.time() < relicCacheNotBefore then return false end

    local actors = nil
    pcall(function() actors = FindAllOf("PalLevelObjectRelic") end)
    if not actors or #actors == 0 then return false end

    -- One current Palworld world has 407 relic actors. A much larger result
    -- means the old and new worlds coexist briefly in GUObjectArray; wait for
    -- the old objects to be collected instead of mixing two servers.
    if #actors > 450 then
        if Config.Debug and not relicCachePendingLogged then
            relicCachePendingLogged = true
            log(string.format(
                "Relic cache pending: %d actors span a world transition",
                #actors))
        end
        return false
    end

    -- The current world contains 407 relic objects. Do not lock in a partial
    -- streaming result during early world startup.
    if #actors < 400 then
        if Config.Debug and not relicCachePendingLogged then
            relicCachePendingLogged = true
            log(string.format("Relic cache pending: only %d actors", #actors))
        end
        return false
    end

    local cache = {}
    local validActors = {}
    local initialStateList = {}
    local spatialGrid = {}
    for _, actor in ipairs(actors) do
        if actor and isValidObject(actor) then
            local fullName = nil
            pcall(function() fullName = tostring(actor:GetFullName()) end)
            if fullName then
                local relicType = nil
                pcall(function()
                    relicType = asNumber(actor:GetRelicType())
                end)
                local wx, wy, wz = getRelicPosition(actor)
                if relicType ~= nil and wx and wy then
                    local info = {
                        actor = actor,
                        fullName = fullName,
                        relicType = relicType,
                        wx = wx, wy = wy, wz = wz,
                        isPicked = readPicked(actor),
                    }
                    cache[fullName] = info
                    validActors[#validActors + 1] = actor
                    initialStateList[#initialStateList + 1] = info
                    local cellX = math.floor(wx / compassGridCellUnits)
                    local cellY = math.floor(wy / compassGridCellUnits)
                    local column = spatialGrid[cellX]
                    if not column then
                        column = {}
                        spatialGrid[cellX] = column
                    end
                    local cell = column[cellY]
                    if not cell then
                        cell = {}
                        column[cellY] = cell
                    end
                    cell[#cell + 1] = info
                end
            end
        end
    end

    if #validActors == 0 then return false end
    cachedRelicActors = validActors
    relicInfoCache = cache
    relicSpatialGrid = spatialGrid
    mapInitialStateList = initialStateList
    relicCachePendingLogged = false
    mapInitialStateReady = false
    mapInitialStateScanActive = false
    mapInitialStateScanCursor = 1
    mapInitialStateNotBefore = os.time() + mapInitialStateDelaySeconds
    log(string.format("Relic cache built: %d actors", #validActors))
    return true
end

local function updateNativePickedState(fullName, isPicked)
    local info = relicInfoCache[fullName]
    if info then info.isPicked = isPicked end

    local native = nativeIcons[fullName]
    if not native or not native.icon or not isValidObject(native.icon)
        or native.isPicked == isPicked then
        return
    end

    pcall(function()
        applyNativeIconVisibility(native.icon, isPicked)
        native.icon:SetRenderOpacity(
            isPicked and collectedIconOpacity or 1.0)
        local brightness = isPicked and 1.0 or uncollectedIconBrightness
        local image = safeIconImage(native.icon)
        if image then
            image:SetColorAndOpacity({
                R = brightness, G = brightness, B = brightness, A = 1.0,
            })
        end
    end)
    native.isPicked = isPicked
end

local function prepareInitialMapState()
    if mapInitialStateReady then return true end
    if not cachedRelicActors then return false end
    if not hideCollectedIcons then
        mapInitialStateReady = true
        return true
    end

    if not mapInitialStateScanActive then
        mapInitialStateScanActive = true
        mapInitialStateScanCursor = 1
    end
    local settleAt = math.max(mapInitialStateNotBefore,
        lastRelicFlagUpdateAt + mapInitialStateQuietSeconds)
    if os.time() < settleAt then return false end

    local checked = 0
    while mapInitialStateScanCursor <= #mapInitialStateList
        and checked < mapInitialStateBatchSize do
        local info = mapInitialStateList[mapInitialStateScanCursor]
        mapInitialStateScanCursor = mapInitialStateScanCursor + 1
        checked = checked + 1
        if not info.isPicked then
            local actor = info.actor
            if actor and isValidObject(actor) and readPicked(actor) then
                updateNativePickedState(info.fullName, true)
            end
        end
    end

    if mapInitialStateScanCursor <= #mapInitialStateList then return false end

    mapInitialStateReady = true
    mapInitialStateScanActive = false
    mapInitialStateScanCursor = 1
    relicStateScanInProgress = false
    relicStateScanCursor = 1
    nextRelicStateScanAt = os.time() + stateScanIntervalSeconds
    if Config.Debug then log("Initial map collection-state scan complete") end
    return true
end

local function refreshRelicStates()
    if not cachedRelicActors then return end
    -- A successful native pickup hook is authoritative after the map's
    -- world-initial state pass. Keep polling exclusively as a compatibility
    -- fallback for UE4SS/game builds where hook registration fails.
    if pickupHookInstalled then return end
    local now = os.time()
    if not relicStateScanInProgress then
        if now < nextRelicStateScanAt then return end
        relicStateScanInProgress = true
        relicStateScanCursor = 1
    end

    -- Keep the fallback off the hot path. Normal pickups update through the
    -- native hook; only a small bounded set is reflected per callback so a
    -- missed event cannot produce the former 407-read game-thread spike.
    local flips = 0
    local checked = 0
    while relicStateScanCursor <= #mapInitialStateList
        and checked < relicStateScanBatchSize do
        local info = mapInitialStateList[relicStateScanCursor]
        relicStateScanCursor = relicStateScanCursor + 1
        checked = checked + 1
        if not info.isPicked then
            local actor = info.actor
            if actor and isValidObject(actor) and readPicked(actor) then
                updateNativePickedState(info.fullName, true)
                flips = flips + 1
            end
        end
    end
    if relicStateScanCursor > #mapInitialStateList then
        relicStateScanInProgress = false
        relicStateScanCursor = 1
        nextRelicStateScanAt = now + stateScanIntervalSeconds
    end
    if Config.Debug and flips > 0 then
        log(string.format("State scan: %d relics newly marked collected", flips))
    end
end

-- Collection changes are delivered through this native function. The event
-- can precede bPickedInClient replication, so unresolved siblings are checked
-- again briefly instead of waking the 407-actor polling fallback.
local PICKUP_RECORD_UPDATE =
    "/Script/Pal.PalLevelObjectObtainable:OnUpdateFlagMapRecord"
-- v1.2.9: OnUpdateFlagMapRecord(Key, bFlag) is broadcast to every obtainable
-- of the affected kind — one pickup fired it on 24 sibling relic instances
-- with bFlag=true, which wrongly marked all of them collected (and v1.2.7's
-- one-way scan then never corrected it). Never trust the parameters for
-- another actor's state: re-read the actor's own bPickedInClient instead.
Callbacks.onPickupRecordUpdated = function(self)
    lastRelicFlagUpdateAt = os.time()
    if mapInitialStateScanActive and not mapInitialStateReady then
        -- Replication changed while the pre-map pass was running. Restart so
        -- no actor checked before the final update can slip into the queue.
        mapInitialStateScanCursor = 1
    end
    local actor = unwrap(self)
    if not actor or not isValidObject(actor) then return end

    local fullName = nil
    pcall(function() fullName = tostring(actor:GetFullName()) end)
    local info = fullName and relicInfoCache[fullName] or nil
    if info then
        local isPicked = readPicked(actor)
        if Config.Debug and isPicked ~= info.isPicked then
            log(string.format("Pickup record: picked=%s relic=%s",
                tostring(isPicked), fullName))
        end
        if isPicked then
            updateNativePickedState(fullName, true)
            pendingPickupChecks[fullName] = nil
            compassElapsedMs = compassUpdateIntervalMs
        elseif not info.isPicked then
            pendingPickupChecks[fullName] = {
                info = info,
                attempts = 0,
            }
            pickupRecheckAt = os.time() + pickupRecheckDelaySeconds
        end
    end
end
local pickupHookOk, pickupHookError = pcall(function()
    RegisterHook(PICKUP_RECORD_UPDATE,
        Callbacks.noop, Callbacks.onPickupRecordUpdated)
end)
if pickupHookOk then
    pickupHookInstalled = true
    log("Hook installed: PalLevelObjectObtainable:OnUpdateFlagMapRecord")
else
    log("Pickup update hook unavailable; polling fallback enabled | " ..
        tostring(pickupHookError))
end

local function processPendingPickupChecks()
    if next(pendingPickupChecks) == nil
        or os.time() < pickupRecheckAt then
        return
    end

    local changed = false
    for fullName, pending in pairs(pendingPickupChecks) do
        local info = pending.info
        local actor = info and info.actor or nil
        if actor and isValidObject(actor) and readPicked(actor) then
            updateNativePickedState(fullName, true)
            pendingPickupChecks[fullName] = nil
            changed = true
        else
            pending.attempts = pending.attempts + 1
            if pending.attempts >= pickupRecheckMaxAttempts
                or not actor or not isValidObject(actor) then
                pendingPickupChecks[fullName] = nil
            end
        end
    end

    if next(pendingPickupChecks) ~= nil then
        pickupRecheckAt = os.time() + pickupRecheckDelaySeconds
    else
        pickupRecheckAt = 0
    end
    if changed then compassElapsedMs = compassUpdateIntervalMs end
end

-- Setup Icon calls this synchronously. Capture the widget it initializes so
-- UE4SS versions that do not expose the Blueprint out value still work.
--
local function isMapBaseOpenOnScreen(mapBase)
    if not mapBase or not isValidObject(mapBase) then return false end
    -- Pooled map widgets can remain IsInViewport()==true while hidden. Only
    -- IsVisible means the map is actually open for marker work.
    local open = false
    pcall(function()
        if mapBase:IsVisible() == true then open = true end
    end)
    return open
end

-- FindFirstOf scans GUObjectArray and hitches. Prefer native/notified refs;
-- throttle the sweep when a fallback search is allowed.
local mapBaseSearchNotBefore = 0
local mapBaseSearchIntervalSeconds = 10

local function findCandidateMapBase(allowFindFirst)
    local mapBase = nativeMapBase
    if mapBase and isValidObject(mapBase) then return mapBase end
    if notifiedMapBase then
        mapBase = unwrap(notifiedMapBase)
        if mapBase and isValidObject(mapBase) then return mapBase end
    end
    -- Never use FindFirstOf from the 1 Hz idle probe or hidden SetLocationData.
    if allowFindFirst == false then return nil end
    local now = os.time()
    if now < mapBaseSearchNotBefore then return nil end
    mapBaseSearchNotBefore = now + mapBaseSearchIntervalSeconds
    mapBase = nil
    pcall(function() mapBase = FindFirstOf("WBP_Map_Base_C") end)
    if mapBase and isValidObject(mapBase) then return mapBase end
    return nil
end

cancelNativeIconQueue = function(reason)
    if nativeIconQueue == nil then
        nativeIconQueueToken = nativeIconQueueToken + 1
        nativeIconQueueActiveToken = 0
        return
    end
    if nativeIconQueueIndex <= #nativeIconQueue then
        nativeIconBuildIncomplete = true
    end
    nativeIconQueue = nil
    nativeIconQueueIndex = 1
    nativeIconQueueToken = nativeIconQueueToken + 1
    nativeIconQueueActiveToken = 0
    Diag.queueCancels = Diag.queueCancels + 1
    if reason then
        log("Native icon queue cancelled: " .. reason)
    end
end

-- Pooled WBP_Map_Base widgets may not emit a dedicated open UFunction. Only
-- treat the map as open when the widget is actually visible so hidden
-- SetLocationData bursts cannot wake the updater.
local function wakeMapSessionFromVisibleMap(source, allowFindFirst)
    if mapSessionActive then return true end
    local mapBase = findCandidateMapBase(allowFindFirst)
    if not isMapBaseOpenOnScreen(mapBase) then return false end
    mapSessionActive = true
    if not nativeMapBase or not isValidObject(nativeMapBase) then
        nativeMapBase = mapBase
    end
    notifiedMapBase = nil
    log("World map session resumed (" .. source .. ")")
    return true
end

-- v1.2.2 / v1.2.3 note: there is NO reliable map-open UFunction. Global
-- UserWidget:Construct is intentionally unused (ProcessEvent recursion while
-- marker widgets are constructed). Prefer a WBP_Map_Base-only Construct hook.
-- SetLocationData may wake only while the map is visible, and only against
-- already-known map wrappers (no FindFirstOf on the idle path).
local ICON_SET_LOCATION = "/Script/Pal.PalUIWorldMapIcon:SetLocationData"
Callbacks.onIconSetLocation = function(self)
    Diag.setLocationFires = Diag.setLocationFires + 1
    if not nativePendingPoint then
        -- Vanilla refreshes pooled map icons while the player is just walking.
        -- Each fire was entering ScriptHook + IsVisible and felt like a micro
        -- hitch. After the marker set exists, reopen/reattach is driven by
        -- WBP_Map_Base Construct and OnClosed — ignore idle SetLocationData.
        if nativeIconMapName == nil or nativeIconsNeedReattach then
            wakeMapSessionFromVisibleMap("SetLocationData", false)
        end
        return
    end
    local icon = unwrap(self)
    if icon and isValidObject(icon) then nativeCapturedIcon = icon end
end
local hookOk, hookErr = pcall(function()
    RegisterHook(ICON_SET_LOCATION,
        Callbacks.noop, Callbacks.onIconSetLocation)
end)
if hookOk then
    log("Hook installed: PalUIWorldMapIcon:SetLocationData")
else
    log("Hook FAILED: SetLocationData | " .. tostring(hookErr))
end

-- Wake the game-thread updater only while a world-map widget exists. This
-- replaces the permanent FindFirstOf("WBP_Map_Base_C") poll while the map is
-- closed. If this notification is unavailable on a UE4SS build, the original
-- polling behavior remains as a compatibility fallback.
local MAP_BASE_CLASS =
    "/Game/Pal/Blueprint/UI/UserInterface/Map/WBP_Map_Base.WBP_Map_Base_C"
Callbacks.onMapBaseCreated = function(mapBase)
    notifiedMapBase = mapBase
    -- A new map widget accompanies a world/server (re)entry; re-check scope.
    worldScopeCheckRequested = true
    -- UObject creation is not equivalent to opening the map: the widget may
    -- be pre-created during gameplay. Visibility / Construct wakes the session.
end
local mapNotifyOk, mapNotifyError = pcall(function()
    NotifyOnNewObject(MAP_BASE_CLASS, Callbacks.onMapBaseCreated)
end)
if mapNotifyOk then
    mapNotifyInstalled = true
    log("Map creation notification installed")
else
    log("Map creation notification unavailable; polling fallback enabled | "
        .. tostring(mapNotifyError))
end

-- Follow the actual HUD lifecycle instead of waiting for a timer or polling
-- every compass instance. Palworld creates a loading compass first and the
-- real in-world compass later; the newest creation notification is the one we
-- should bind to. The async updater performs the actual registration on the
-- next game-thread tick, after widget construction has settled.
local COMPASS_CLASS =
    "/Game/Pal/Blueprint/UI/UserInterface/InGame/Compass/" ..
    "WBP_Ingame_Compass.WBP_Ingame_Compass_C"
Callbacks.onCompassCreated = function(rawWidget)
    local widget = unwrap(rawWidget)
    if not widget or not isValidObject(widget) then return end
    notifiedCompassWidget = widget
    compassContextDirty = true
    compassElapsedMs = compassUpdateIntervalMs
    -- The in-world compass is (re)created on every world/server entry, so this
    -- is the primary event signal for a world-scope re-check.
    worldScopeCheckRequested = true
    if Config.Debug then log("Compass creation notification received") end
end
local compassNotifyOk, compassNotifyError = pcall(function()
    NotifyOnNewObject(COMPASS_CLASS, Callbacks.onCompassCreated)
end)
if compassNotifyOk then
    log("Compass creation notification installed")
else
    log("Compass creation notification unavailable; one-shot search fallback enabled | "
        .. tostring(compassNotifyError))
end

-- Palworld can keep a closed map UObject valid for a while. Object validity
-- therefore cannot reliably tell us that the map is still on screen. Stop the
-- updater from the widget close lifecycle instead.
local function isWorldMapWidget(rawWidget)
    local widget = unwrap(rawWidget)
    if not widget or not isValidObject(widget) then return false end
    local ok, className = pcall(function()
        return tostring(widget:GetClass():GetFullName())
    end)
    return ok and className:find("WBP_Map_Base_C", 1, true) ~= nil
end

local function onWorldMapClosed(rawWidget)
    Diag.closeFires = Diag.closeFires + 1
    if not isWorldMapWidget(rawWidget) then return end
    mapSessionActive = false
    notifiedMapBase = nil
    cancelNativeIconQueue("map closed")
    -- v1.2.3: keep every marker registered across map close. The map widget
    -- is reused for the whole world session and there is no hookable open
    -- signal, so tearing markers down here would leave reopened maps empty.
    -- Persistent markers also make phantom wake-ups free: nothing to rebuild.
    -- World transitions still tear down through resetRelicWorldState, and a
    -- destroyed/recreated widget is caught by the validity and attachment
    -- checks in tryCreateNativeIcons.
    -- v1.2.9: the close drops our widgets from the map's internal icon
    -- registry, so the game stops repositioning them on zoom (markers appear
    -- only near the default zoom). Re-register them on the next update pass.
    nativeIconsNeedReattach = true
    -- Drop ephemeral map-body wrappers; icon widgets stay in nativeIcons for
    -- reattach. Resolve map base/body again on the next visible session.
    nativeMapBody = nil
    if Config.Debug then
        log("World map closed: updater asleep, markers kept")
    end
end
Callbacks.onWorldMapClosed = onWorldMapClosed

-- Map-base Construct only (not global UserWidget). Creating PalUIWorldMapIcon
-- markers does not re-enter this hook, so ProcessEvent recursion is avoided.
Callbacks.onWorldMapConstructed = function(rawWidget)
    local mapBase = unwrap(rawWidget)
    if not mapBase or not isValidObject(mapBase) then return end
    if not isWorldMapWidget(rawWidget) then return end
    notifiedMapBase = mapBase
    nativeMapBase = mapBase
    mapSessionActive = true
    nativeBaseMapScale = 1.0
    nativeLastMapScale = nil
    -- Do not resetNativeMapState here: markers persist and reattach.
    nativeIconsNeedReattach = nativeIconMapName ~= nil
    log("World map Construct: session active")
end

local constructHookOk, constructHookError = pcall(function()
    RegisterHook(MAP_BASE_CLASS .. ":Construct",
        Callbacks.onWorldMapConstructed, Callbacks.noop)
end)
if constructHookOk then
    mapConstructHookInstalled = true
    log("Map Construct hook installed (WBP_Map_Base only)")
else
    log("Map Construct hook unavailable | " .. tostring(constructHookError))
end

-- Prefer PalUserWidget close signals. Do NOT hook UserWidget:Destruct globally:
-- it fires for every UMG widget including marker icons we create, and re-enters
-- ProcessEvent through UE4SS ScriptHook (1.1.8 crash report).
for _, closeFunction in ipairs({
    "/Script/Pal.PalUserWidget:OnClosed",
    "/Script/Pal.PalUserWidgetStackableUI:OnClose",
}) do
    local closeHookOk, closeHookError = pcall(function()
        RegisterHook(closeFunction, Callbacks.noop,
            Callbacks.onWorldMapClosed)
    end)
    if closeHookOk then
        log("Map close hook installed: " .. closeFunction)
    else
        log("Map close hook unavailable: " .. closeFunction .. " | " ..
            tostring(closeHookError))
    end
end

local function applyNativeRelicTexture(iconWidget, relicType)
    local image = nil
    local imageOk = pcall(function() image = iconWidget.Icon end)
    if not imageOk or not image or not isValidObject(image) then
        return false, "native widget has no valid Icon image"
    end

    local suffix = (relicType == 0) and ""
        or string.format("_%02d", relicType)
    local assetName = "T_itemicon_Relic" .. suffix
    local path = "/Game/Others/InventoryItemIcon/Texture/" .. assetName
        .. "." .. assetName

    local texture = nativeRelicTextures[relicType]
    if not (texture and isValidObject(texture)) then
        texture = nil
        pcall(function()
            LoadAsset(path)
            texture = StaticFindObject(path)
        end)
        if texture and isValidObject(texture) then
            nativeRelicTextures[relicType] = texture
        end
    end
    if not texture or not isValidObject(texture) then
        return false, "relic texture not found: " .. path
    end

    local ok, err = pcall(function()
        image:SetBrushFromTexture(texture, false)
    end)
    if not ok then return false, tostring(err) end
    return true
end

local function relicGuid(fullName)
    -- FGuid fields are signed int32. Three independent small polynomial
    -- hashes keep the key deterministic without exceeding Lua's exact integer
    -- range or relying on bitwise operators from a particular Lua build.
    local function hash(seed, multiplier)
        local value = seed
        for index = 1, #fullName do
            value = (value * multiplier + string.byte(fullName, index))
                % 2147483647
        end
        return math.floor(value)
    end
    return {
        A = 1398030676, -- "STAT" namespace for this mod
        B = hash(5381, 33),
        C = hash(7919, 131),
        D = hash(104729, 65599),
    }
end

local function removeCompassEntry(fullName, entry)
    entry = entry or compassEntries[fullName]
    if not entry then return end
    Diag.compassRemoves = Diag.compassRemoves + 1

    local widget = entry.widget or compassWidget
    local manager = entry.manager or compassManager
    local point = entry.point
    local guid = entry.guid

    -- The Blueprint removal handler consults the manager before detaching its
    -- widget, so notify it before removing our local location records.
    if widget and isValidObject(widget) and point and isValidObject(point) then
        pcall(function()
            local removeLocation = widget["OnRemovedLocation"]
            if removeLocation then
                removeLocation(widget, guid, point)
            end
        end)
    end
    if manager and isValidObject(manager) then
        pcall(function() manager.LocationMapInLocal:Remove(guid) end)
        pcall(function() manager.LocationMapCombined:Remove(guid) end)
    end
    compassEntries[fullName] = nil
end

resetCompassState = function(reason)
    local removed = 0
    local oldEntries = compassEntries
    compassEntries = {}
    for fullName, entry in pairs(oldEntries) do
        -- removeCompassEntry normally edits compassEntries. It is already a
        -- new table here, which makes teardown safe while iterating.
        removeCompassEntry(fullName, entry)
        removed = removed + 1
    end
    compassManager = nil
    compassWidget = nil
    compassManagerName = nil
    compassWidgetName = nil
    compassLastCount = -1
    compassFailureCount = 0
    compassPlayer = nil
    compassLastPlayerX = nil
    compassLastPlayerY = nil
    notifiedCompassWidget = nil
    compassContextDirty = true
    if Config.Debug and reason and removed > 0 then
        log(string.format("Compass reset: %s (removed=%d)", reason, removed))
    end
end

local function findCompassContext()
    -- The full UObject search is relatively expensive. Once the visible HUD
    -- and its same-world manager have been selected, reuse them until a world
    -- transition/reset invalidates the cached objects.
    if not compassContextDirty
        and compassWidget and isValidObject(compassWidget)
        and compassManager and isValidObject(compassManager)
        and compassWidgetName and compassManagerName then
        return compassWidget, compassManager,
            compassWidgetName, compassManagerName, nil
    end

    -- Old loading HUDs remain valid after entering a world, so FindFirstOf can
    -- return an invisible compass forever. Pick the newest visible transient
    -- widget, then pair it with the location manager from the same UWorld.
    local widget, widgetScore = nil, -1
    if notifiedCompassWidget and isValidObject(notifiedCompassWidget) then
        widget = notifiedCompassWidget
        widgetScore = 2000
    end
    local widgets = nil
    if not widget then
        pcall(function() widgets = FindAllOf("WBP_Ingame_Compass_C") end)
    end
    if not widget and widgets then
        for index, candidate in ipairs(widgets) do
            if isValidObject(candidate) then
                local score = index / 100000
                local fullName = ""
                pcall(function() fullName = tostring(candidate:GetFullName()) end)
                if fullName:find("/Engine/Transient", 1, true) then
                    score = score + 100
                end
                if not fullName:find("Default__", 1, true) then
                    score = score + 10
                end
                pcall(function()
                    if candidate:IsVisible() == true then score = score + 1000 end
                end)
                pcall(function()
                    if candidate:IsInViewport() == true then score = score + 500 end
                end)
                if getWidgetPlayerPawn(candidate) then score = score + 50 end
                if score >= widgetScore then
                    widget, widgetScore = candidate, score
                end
            end
        end
    end

    local manager = nil
    local targetWorldName = nil
    if widget and isValidObject(widget) then
        pcall(function()
            targetWorldName = tostring(widget:GetWorld():GetFullName())
        end)
    end
    local managers = nil
    pcall(function() managers = FindAllOf("BP_PalLocationManager_C") end)
    if managers then
        for _, candidate in ipairs(managers) do
            if isValidObject(candidate) then
                local candidateWorldName = nil
                pcall(function()
                    candidateWorldName = tostring(candidate:GetWorld():GetFullName())
                end)
                if not manager or (targetWorldName and
                    candidateWorldName == targetWorldName) then
                    manager = candidate
                end
                if targetWorldName and candidateWorldName == targetWorldName then
                    break
                end
            end
        end
    end
    if not widget or not isValidObject(widget)
        or not manager or not isValidObject(manager) then
        return nil, nil, nil, nil, string.format(
            "widget=%s manager=%s",
            tostring(widget ~= nil and isValidObject(widget)),
            tostring(manager ~= nil and isValidObject(manager)))
    end
    local widgetName, managerName = nil, nil
    pcall(function() widgetName = tostring(widget:GetFullName()) end)
    pcall(function() managerName = tostring(manager:GetFullName()) end)
    if not widgetName or not managerName then
        return nil, nil, nil, nil, "object full name unavailable"
    end
    if Config.Debug and (compassWidgetName ~= widgetName
        or compassManagerName ~= managerName) then
        log(string.format("Compass context selected: score=%.3f widget=%s manager=%s",
            widgetScore, widgetName, managerName))
    end
    compassContextDirty = false
    return widget, manager, widgetName, managerName, nil
end

local function getLocationPointByGuid(manager, guid)
    if not manager or not guid then return nil end
    local function tryMap(map)
        if not map then return nil end
        local contains = false
        pcall(function() contains = map:Contains(guid) == true end)
        if not contains then return nil end
        local point = nil
        local out = {}
        pcall(function()
            if map:Find(guid, out) then
                point = unwrap(out[1] or out.Value or out.value)
            end
        end)
        if point and isValidObject(point) then return point end
        point = nil
        pcall(function() point = unwrap(map[guid]) end)
        if point and isValidObject(point) then return point end
        return nil
    end
    return tryMap(manager.LocationMapInLocal)
        or tryMap(manager.LocationMapCombined)
end

local function bindCompassLocation(widget, guid, point)
    if not widget or not guid or not point then return end
    local alreadyBound = false
    pcall(function()
        if widget.CreatedIconMap and widget.CreatedIconMap:Contains(guid) then
            alreadyBound = true
        end
    end)
    if alreadyBound then return end

    local addLocation = widget["On Added Location"]
    if not addLocation then error("compass On Added Location not found") end
    -- Call with LocationId + Location only. The Blueprint fills SearchType /
    -- LocationUIData from GetType() + DT_LocationUIData. Passing a zeroed
    -- FPalLocationUIData (earlier attempt) set displayLength=0 and produced no icons.
    addLocation(widget, guid, point)
end

local function addCompassEntry(fullName, info, widget, manager)
    local createdPoint = nil
    local createdGuid = nil
    local ok, resultOrError = pcall(function()
        compassPointClass = compassPointClass or
            StaticFindObject("/Script/Pal.PalLocationPoint_LevelObject")
        if not compassPointClass then
            error("PalLocationPoint_LevelObject class not found")
        end

        local guid = relicGuid(fullName)
        -- Check LocationManager BEFORE StaticConstructObject. Constructing first
        -- left an incomplete PalLocationPoint when the GUID already existed
        -- (loading compass -> in-world compass re-register).
        local point = getLocationPointByGuid(manager, guid)
        if not point then
            point = StaticConstructObject(compassPointClass, manager)
            if not point then error("location point construction failed") end
            createdPoint = point
            createdGuid = guid
            point.ID = guid
            point.InitialLocationCache = {
                X = info.wx, Y = info.wy, Z = info.wz or 0.0,
            }
            -- Compass distance reads Location; InitialLocationCache alone can
            -- leave a zero target until a later refresh.
            point.Location = {
                X = info.wx, Y = info.wy, Z = info.wz or 0.0,
            }
            point.RelicType = info.relicType
            point.bShouldDisplay = true
            point.bShowInMap = false
            point.bShowInCompass = true

            -- GetLocationPoint/GetLocation used by the native compass widget read
            -- LocationMapCombined. The local map owns our client-only points; no
            -- save data or server state is touched.
            manager.LocationMapInLocal:Add(guid, point)
            manager.LocationMapCombined:Add(guid, point)
        else
            pcall(function()
                point.Location = {
                    X = info.wx, Y = info.wy, Z = info.wz or 0.0,
                }
                point.bShouldDisplay = true
                point.bShowInMap = false
                point.bShowInCompass = true
            end)
        end

        bindCompassLocation(widget, guid, point)
        return {
            guid = guid,
            point = point,
            widget = widget,
            manager = manager,
        }
    end)

    if not ok or not resultOrError then
        if createdGuid then
            if createdPoint and isValidObject(createdPoint)
                and widget and isValidObject(widget) then
                pcall(function()
                    local removeLocation = widget["OnRemovedLocation"]
                    if removeLocation then
                        removeLocation(widget, createdGuid, createdPoint)
                    end
                end)
            end
            pcall(function() manager.LocationMapInLocal:Remove(createdGuid) end)
            pcall(function() manager.LocationMapCombined:Remove(createdGuid) end)
        end
        compassFailureCount = compassFailureCount + 1
        if compassFailureCount <= 5 then
            log("Compass marker failed: " .. tostring(resultOrError))
        elseif compassFailureCount == 6 then
            log("Further compass marker failures will be suppressed")
        end
        return false
    end
    compassFailureCount = 0
    Diag.compassAdds = Diag.compassAdds + 1
    compassEntries[fullName] = resultOrError
    return true
end

refreshCompassMarkers = function()
    if not compassMarkersVisible or compassMaxMarkers <= 0
        or compassRangeMeters <= 0 then
        reportCompassStatus("disabled")
        if next(compassEntries) ~= nil then
            resetCompassState("disabled or hidden")
        end
        return
    end
    if not ensureRelicCache() then
        reportCompassStatus("waiting for relic cache")
        return
    end

    local widget, manager, widgetName, managerName, contextError =
        findCompassContext()
    if not widget or not manager then
        reportCompassStatus("waiting for compass context",
            contextError or "unknown")
        if next(compassEntries) ~= nil then
            resetCompassState("compass unavailable")
        end
        return
    end
    -- FindFirstOf may create a fresh Lua wrapper for the same UObject on every
    -- call. Comparing those wrapper objects directly caused a full remove/add
    -- cycle once per second, which appeared as continuous compass flicker.
    -- The UObject full name is stable for the lifetime of the HUD/world.
    if compassWidgetName ~= widgetName
        or compassManagerName ~= managerName then
        if compassWidgetName or compassManagerName then
            resetCompassState("widget or manager changed")
        end
        compassWidget = widget
        compassManager = manager
        compassWidgetName = widgetName
        compassManagerName = managerName
    else
        -- Retain the newest valid wrappers without treating them as a new
        -- compass session.
        compassWidget = widget
        compassManager = manager
    end

    local playerX, playerY = nil, nil
    pcall(function()
        -- Prefer a cached pawn / the compass owner. FindAllOf(PlayerController)
        -- on every refresh is a visible hitch with compass always enabled.
        if not (compassPlayer and isValidObject(compassPlayer)) then
            compassPlayer = getWidgetPlayerPawn(widget)
                or getLocalPlayerPawn()
        end
        if compassPlayer and isValidObject(compassPlayer) then
            local position = compassPlayer:K2_GetActorLocation()
            playerX, playerY = tonumber(position.X), tonumber(position.Y)
        end
    end)
    if not playerX or not playerY then
        compassPlayer = nil
        reportCompassStatus("waiting for player")
        return
    end

    local rangeUnits = compassRangeMeters * 100.0
    local rangeSquared = rangeUnits * rangeUnits
    -- Keep an existing marker slightly beyond the admission range. More
    -- importantly, do not replace it merely because another relic becomes a
    -- few centimetres nearer. Widget creation/removal is the expensive part
    -- and caused periodic hitches while the player was moving.
    local releaseRangeSquared = rangeSquared * 1.3225 -- 1.15 ^ 2
    local removals = {}
    local activeCount = 0
    for fullName in pairs(compassEntries) do
        local info = relicInfoCache[fullName]
        local remove = not info
            or (compassHideCollected and info.isPicked)
        if info and not remove then
            local dx, dy = info.wx - playerX, info.wy - playerY
            remove = dx * dx + dy * dy > releaseRangeSquared
        end
        if remove then
            removals[#removals + 1] = fullName
        else
            activeCount = activeCount + 1
        end
    end
    for _, fullName in ipairs(removals) do
        removeCompassEntry(fullName)
    end

    local availableSlots = compassMaxMarkers - activeCount
    local nearestCandidates = {}
    local candidateCount = 0
    local movedFar = true
    if compassLastPlayerX and compassLastPlayerY then
        local mdx = playerX - compassLastPlayerX
        local mdy = playerY - compassLastPlayerY
        movedFar = (mdx * mdx + mdy * mdy) >= compassMoveSkipSquared
    end
    -- Full + barely moved: only the range-exit pass above is needed.
    if availableSlots > 0 and (movedFar or activeCount == 0) then
        local playerCellX = math.floor(playerX / compassGridCellUnits)
        local playerCellY = math.floor(playerY / compassGridCellUnits)
        -- Default range/cell size is 300m, so this visits exactly the 3x3
        -- neighborhood. Expand only when a user configures a larger range.
        local cellRadius = math.max(1,
            math.ceil(rangeUnits / compassGridCellUnits))

        local function retainNearest(candidate)
            local insertAt = #nearestCandidates + 1
            for index, existing in ipairs(nearestCandidates) do
                if candidate.distanceSquared < existing.distanceSquared
                    or (candidate.distanceSquared == existing.distanceSquared
                        and candidate.fullName < existing.fullName) then
                    insertAt = index
                    break
                end
            end
            table.insert(nearestCandidates, insertAt, candidate)
            if #nearestCandidates > availableSlots then
                table.remove(nearestCandidates)
            end
        end

        for cellX = playerCellX - cellRadius,
            playerCellX + cellRadius do
            local column = relicSpatialGrid[cellX]
            if column then
                for cellY = playerCellY - cellRadius,
                    playerCellY + cellRadius do
                    local cell = column[cellY]
                    if cell then
                        for _, info in ipairs(cell) do
                            local fullName = info.fullName
                            if not compassEntries[fullName]
                                and not (compassHideCollected
                                    and info.isPicked) then
                                local dx = info.wx - playerX
                                local dy = info.wy - playerY
                                local distanceSquared = dx * dx + dy * dy
                                if distanceSquared <= rangeSquared then
                                    candidateCount = candidateCount + 1
                                    retainNearest({
                                        fullName = fullName,
                                        info = info,
                                        distanceSquared = distanceSquared,
                                    })
                                end
                            end
                        end
                    end
                end
            end
        end

        for _, candidate in ipairs(nearestCandidates) do
            if addCompassEntry(candidate.fullName, candidate.info,
                widget, manager) then
                activeCount = activeCount + 1
            end
        end
    end

    compassLastPlayerX, compassLastPlayerY = playerX, playerY
    if Config.Debug and activeCount ~= compassLastCount then
        log(string.format(
            "Compass markers: active=%d candidates=%d range=%.0fm max=%d",
            activeCount, candidateCount, compassRangeMeters,
            compassMaxMarkers))
    end
    reportCompassStatus("ready", string.format(
        "active=%d candidates=%d player=%.0f,%.0f",
        activeCount, candidateCount, playerX, playerY))
    compassLastCount = activeCount
end

local function getObjectFullName(object)
    if not object or not isValidObject(object) then return nil end
    local name = nil
    pcall(function() name = tostring(object:GetFullName()) end)
    return name
end

local function extractMapBody(output, ...)
    if output then
        if output[1] and isValidObject(output[1]) then return output[1] end
        if output.MapBody and isValidObject(output.MapBody) then
            return output.MapBody
        end
    end
    -- Preserve nil slots in UE4SS's multiple return values. Building a table
    -- and walking it with ipairs would stop at the first nil, potentially
    -- hiding a valid UObject returned in a later slot.
    for index = 1, select("#", ...) do
        local candidate = select(index, ...)
        if candidate and isValidObject(candidate) then return candidate end
    end
    return nil
end

local function getCurrentNativeMapBody(mapBase)
    local result = nil
    pcall(function()
        local getter = mapBase["GetCurrentMapBody"]
        if not getter then return end
        local output = {}
        local r1, r2, r3 = getter(mapBase, output)
        result = extractMapBody(output, r1, r2, r3)
    end)
    return result
end

local function getNativeMapBodyByLocation(mapBase, info)
    local result = nil
    pcall(function()
        local getter = mapBase["GetMapBodyByLocation"]
        if not getter then return end
        local output = {}
        local r1, r2, r3 = getter(mapBase, {
            X = info.wx, Y = info.wy, Z = info.wz or 0.0,
        }, output)
        result = extractMapBody(output, r1, r2, r3)
    end)
    return result
end

local function buildNativeIconQueue(activeMapBody)
    local queue = {}
    local activeBodyName = getObjectFullName(activeMapBody)
    local routed, skipped, unresolved = 0, 0, 0
    for _, info in pairs(relicInfoCache) do
        if not (hideCollectedIcons and info.isPicked) then
            local targetBody = getNativeMapBodyByLocation(nativeMapBase, info)
            local targetBodyName = getObjectFullName(targetBody)
            if targetBodyName and activeBodyName then
                routed = routed + 1
                if targetBodyName == activeBodyName then
                    queue[#queue + 1] = info
                else
                    skipped = skipped + 1
                end
            else
                -- Preserve compatibility if a future game build removes the
                -- location router. The active-body selection still works.
                unresolved = unresolved + 1
                queue[#queue + 1] = info
            end
        end
    end

    local playerX, playerY = nil, nil
    pcall(function()
        local player = getWidgetPlayerPawn(nativeMapBase)
            or getLocalPlayerPawn()
        if player and isValidObject(player) then
            local position = player:K2_GetActorLocation()
            playerX, playerY = tonumber(position.X), tonumber(position.Y)
        end
    end)

    table.sort(queue, function(a, b)
        if playerX and playerY then
            local adx, ady = a.wx - playerX, a.wy - playerY
            local bdx, bdy = b.wx - playerX, b.wy - playerY
            local ad2 = adx * adx + ady * ady
            local bd2 = bdx * bdx + bdy * bdy
            if ad2 ~= bd2 then return ad2 < bd2 end
        end
        return a.fullName < b.fullName
    end)

    return queue, playerX, playerY, routed, skipped, unresolved
end

local function createNativeIcon(mapBase, mapBody, info)
    return pcall(function()
        nativeIconPointClass = nativeIconPointClass or
            StaticFindObject("/Script/Pal.PalLocationPoint_LevelObject")
        if not nativeIconPointClass then
            error("PalLocationPoint_LevelObject class not found")
        end

        local point = StaticConstructObject(nativeIconPointClass, mapBase)
        if not point then error("location point construction failed") end
        point.InitialLocationCache = {
            X = info.wx, Y = info.wy, Z = info.wz or 0.0,
        }
        point.RelicType = info.relicType
        point.bShouldDisplay = true
        point.bShowInMap = true
        point.bShowInCompass = false

        local setupIcon = mapBase["Setup Icon"]
        if not setupIcon then error("Setup Icon function not found") end
        nativePendingPoint = point
        nativeCapturedIcon = nil
        local iconOut = {}
        local r1, r2, r3 = setupIcon(mapBase, 19, point, iconOut)
        nativePendingPoint = nil

        local icon = nil
        local candidates = {
            [1] = nativeCapturedIcon, [2] = r1, [3] = r2,
            [4] = r3, [5] = iconOut[1], [6] = iconOut.Icon,
        }
        for index = 1, 6 do
            local candidate = candidates[index]
            if candidate and isValidObject(candidate) then
                local className = ""
                pcall(function()
                    className = tostring(candidate:GetClass():GetFullName())
                end)
                if className:find("PalUIWorldMapIcon", 1, true)
                    or className:find("WBP_Map_IconSupply", 1, true) then
                    icon = candidate
                    break
                end
            end
        end
        if not icon then error("Setup Icon returned no native icon") end

        local textureOk, textureError = applyNativeRelicTexture(
            icon, info.relicType)
        if not textureOk then error(textureError) end

        pcall(function() icon.bEnableHideOnZoom = false end)
        pcall(function() icon.IgnoreMask = true end)
        pcall(function() icon:SetIgnoreMask(true) end)
        pcall(function() icon:SetGeneralHiding(false) end)

        local addIcon = mapBody["Add Icon By Location"]
        if not addIcon then
            error("Add Icon By Location function not found")
        end
        local addedOut = {}
        addIcon(mapBody, icon, {
            X = info.wx, Y = info.wy, Z = info.wz or 0.0,
        }, true, false, addedOut)
        if addedOut[1] == false or addedOut.added == false then
            error("map body rejected native icon")
        end

        -- Add Icon By Location registers the widget but does not apply the
        -- current map transform when a pooled map is reopened at a retained
        -- zoom step. Palworld normally calls this function only after the
        -- player changes zoom, which is why one wheel input fixed the size.
        -- Refresh this icon through the game's own layout path immediately.
        pcall(function()
            local updateMapIcon = mapBody["Update Map Icon"]
            if updateMapIcon then updateMapIcon(mapBody, icon) end
        end)

        -- PalUIWorldMapIcon updates the root widget's render transform during
        -- map/zoom processing, which overwrites a root SetRenderScale call.
        -- Scale the inner Image instead; the game does not resize that child.
        local desiredX, desiredY = nil, nil
        local renderScale = 1.0
        local sizeApplied, sizeError = pcall(function()
            local image = safeIconImage(icon)
            if not image then error("inner Icon is stale/invalid") end
            image:ForceLayoutPrepass()
            local desired = image:GetDesiredSize()
            desiredX, desiredY = tonumber(desired.X), tonumber(desired.Y)
            local designedSize = math.max(desiredX or 0, desiredY or 0)
            if designedSize <= 0 then designedSize = 64 end
            renderScale = nativeIconSize / designedSize
            image:SetRenderTransformPivot({ X = 0.5, Y = 0.5 })
            image:SetRenderScale({ X = renderScale, Y = renderScale })
        end)
        -- Keep the root enabled so only collected inner images receive the
        -- gray disabled draw effect. HitTestInvisible prevents interaction.
        applyNativeIconVisibility(icon, info.isPicked)
        pcall(function() icon:SetIsEnabled(true) end)
        local brightness = info.isPicked
            and 1.0 or uncollectedIconBrightness
        pcall(function()
            local image = safeIconImage(icon)
            if image then
                image:SetColorAndOpacity({
                    R = brightness, G = brightness,
                    B = brightness, A = 1.0,
                })
            end
        end)
        icon:SetRenderOpacity(info.isPicked and collectedIconOpacity or 1.0)

        if not nativeLayoutLogged then
            nativeLayoutLogged = true
            local slotX, slotY, parentName = nil, nil, "?"
            pcall(function()
                local position = icon.Slot:GetPosition()
                slotX, slotY = tonumber(position.X), tonumber(position.Y)
                parentName = tostring(icon:GetParent():GetFullName())
            end)
            log(string.format(
                "Native layout: slot=%s,%s desired=%s,%s scale=%.4f applied=%s error=%s parent=%s world=%.1f,%.1f",
                tostring(slotX), tostring(slotY),
                tostring(desiredX), tostring(desiredY), renderScale,
                tostring(sizeApplied), tostring(sizeError),
                parentName, info.wx, info.wy))
        end

        return {
            point = point,
            icon = icon,
            isPicked = info.isPicked,
            baseImageScale = renderScale,
        }
    end)
end

resetNativeMapState = function()
    -- Dropping only the Lua references leaves widgets registered in the
    -- world's native icon collection. If a close callback is emitted while
    -- the reused map widget is still alive, the next queue then adds a second
    -- copy of every marker. Explicitly unregister and detach our old widgets.
    cancelNativeIconQueue("native map reset")
    nativeIconBuildIncomplete = false
    local oldMapBase = nativeMapBase
    for _, native in pairs(nativeIcons) do
        local icon = native.icon
        if icon and isValidObject(icon) then
            -- A pooled icon can be registered again by the game even after it
            -- has been detached. Restore its configured base size before
            -- releasing our reference so the previous zoom cannot leak into
            -- the next map session.
            local image = safeIconImage(icon)
            if image then
                pcall(function()
                    image:SetRenderScale({
                        X = native.baseImageScale,
                        Y = native.baseImageScale,
                    })
                end)
            end
            pcall(function() icon:SetVisibility(1) end)
            pcall(function()
                if oldMapBase and isValidObject(oldMapBase) then
                    oldMapBase:RemoveWorldMapIcon(icon)
                end
            end)
            pcall(function() icon:RemoveFromParent() end)
        end
    end
    nativeIcons = {}
    nativeIconsNeedReattach = false
    nativeIconMapName = nil
    nativeIconFailed = 0
    nativePendingPoint = nil
    nativeCapturedIcon = nil
    -- Texture UObject wrappers can remain superficially valid after the map
    -- that loaded them is destroyed. Reload them for each map instance so a
    -- stale brush resource cannot appear as a white square.
    nativeRelicTextures = {}
    nativeLayoutLogged = false
    nativeMapBase = nil
    nativeMapBody = nil
    nativeBaseMapScale = 1.0
    nativeLastMapScale = nil
end

local function readMapScale(mapBody)
    local scale = nil
    pcall(function() scale = asNumber(mapBody.CachedMapScale) end)
    if scale and scale > 0 then return scale end
    return nil
end

local function updateNativeIconZoom()
    local mapBody = nativeMapBody
    if not mapBody or not isValidObject(mapBody) then return end
    local mapScale = readMapScale(mapBody)
    if not mapScale then return end
    local mapScaleChanged = not nativeLastMapScale
        or math.abs(mapScale - nativeLastMapScale) >= 0.0001
    if not mapScaleChanged then return end
    nativeLastMapScale = mapScale

    local ratio = mapScale / nativeBaseMapScale
    if ratio < 0.1 then ratio = 0.1 end
    if ratio > 10 then ratio = 10 end
    local zoomMultiplier = ratio ^ iconZoomExponent
    local updated = 0
    for _, native in pairs(nativeIcons) do
        local image = safeIconImage(native.icon)
        if image then
            pcall(function()
                local scale = native.baseImageScale * zoomMultiplier
                image:SetRenderScale({ X = scale, Y = scale })
            end)
            updated = updated + 1
        end
    end
    if Config.Debug then
        local zoomStep = nil
        pcall(function() zoomStep = asNumber(mapBody.CurrentZoomStep) end)
        log(string.format(
            "Zoom scale: step=%s map=%.4f base=%.4f ratio=%.4f multiplier=%.4f icons=%d",
            tostring(zoomStep), mapScale, nativeBaseMapScale,
            ratio, zoomMultiplier, updated))
    end
end

local function tryCreateNativeIcons()
    local mapBase = nativeMapBase
    if not mapBase or not isValidObject(mapBase) then
        mapBase = nil
        if notifiedMapBase then mapBase = unwrap(notifiedMapBase) end
        -- 1.2.3: do not GUObjectArray-scan while waiting for the first map
        -- open. Construct / Notify / visible SetLocationData set the session;
        -- FindFirstOf is only for Notify-missing fallback or an active session
        -- that lost its cached mapBase ref (1.2.0 ungated this on
        -- nativeIconMapName == nil and caused hitch until first map open).
        if (not mapBase or not isValidObject(mapBase))
            and (not mapNotifyInstalled or mapSessionActive) then
            pcall(function() mapBase = FindFirstOf("WBP_Map_Base_C") end)
        end
        if mapBase and isValidObject(mapBase) then
            nativeMapBase = mapBase
            notifiedMapBase = nil
        end
    end
    if not mapBase or not isValidObject(mapBase) then
        if nativeIconMapName then resetNativeMapState() end
        notifiedMapBase = nil
        mapSessionActive = false
        return
    end

    -- Never keep building icons against a pooled/hidden map body.
    if (nativeIconQueue ~= nil or nativeIconBuildIncomplete)
        and not isMapBaseOpenOnScreen(mapBase) then
        if nativeIconQueue ~= nil then
            cancelNativeIconQueue("map not visible")
        end
        mapSessionActive = false
        nativeIconsNeedReattach = true
        return
    end

    -- Palworld 1.0 keeps the overworld and World Tree bodies in one map
    -- widget. Always ask the Blueprint for the currently displayed body;
    -- caching WBP_Map_Body_MW5 pins every custom icon to the overworld canvas.
    local mapBody = getCurrentNativeMapBody(mapBase)
    if not mapBody or not isValidObject(mapBody) then
        mapBody = nil
        pcall(function() mapBody = mapBase.WBP_Map_Body_MW5 end)
    end
    if not mapBody or not isValidObject(mapBody) then
        if nativeIconMapName then
            resetNativeMapState()
            mapSessionActive = false
        end
        return
    end
    if not ensureRelicCache() then return end
    -- Do not create all 407 widgets and hide collected ones afterward. Finish
    -- the map-only replicated-state pass first, then build a filtered queue.
    if not prepareInitialMapState() then return end

    local mapName = getObjectFullName(mapBase)
    local mapBodyName = getObjectFullName(mapBody)
    if not mapName or not mapBodyName then return end
    local mapSessionName = mapName .. " | " .. mapBodyName

    local function beginNativeIconQueue(queue, reason)
        nativeIconQueue = queue
        nativeIconQueueIndex = 1
        nativeIconQueueToken = nativeIconQueueToken + 1
        nativeIconQueueActiveToken = nativeIconQueueToken
        nativeIconBuildIncomplete = false
        nativeIconFailed = 0
        Diag.queueBuilds = Diag.queueBuilds + 1
        if Config.Debug and reason then
            log("Native icon queue start: " .. reason)
        end
    end

    if nativeIconMapName ~= mapSessionName then
        resetNativeMapState()
        nativeMapBase = mapBase
        nativeIconMapName = mapSessionName
        nativeMapBody = mapBody
        nativeBaseMapScale = 1.0
        local playerX, playerY, routed, skipped, unresolved
        local queue
        queue, playerX, playerY, routed, skipped, unresolved =
            buildNativeIconQueue(mapBody)
        beginNativeIconQueue(queue, "map session changed")
        log(string.format(
            "Native icon queue built: %d relics, body=%s, routed=%d skipped=%d unresolved=%d, order=%s player=%s,%s",
            #nativeIconQueue,
            mapBodyName,
            routed, skipped, unresolved,
            (playerX and playerY) and "nearest-first" or "fallback",
            tostring(playerX), tostring(playerY)))
    elseif nativeIconBuildIncomplete and nativeIconQueue == nil then
        -- Resume only missing markers after a mid-build cancel (map hide).
        nativeMapBase = mapBase
        nativeMapBody = mapBody
        local fullQueue = buildNativeIconQueue(mapBody)
        local resumeQueue = {}
        for _, info in ipairs(fullQueue) do
            local existing = nativeIcons[info.fullName]
            if not (existing and existing.icon and isValidObject(existing.icon)) then
                resumeQueue[#resumeQueue + 1] = info
            end
        end
        if #resumeQueue > 0 then
            local kept = 0
            for _ in pairs(nativeIcons) do kept = kept + 1 end
            beginNativeIconQueue(resumeQueue, "resume after cancel")
            log(string.format(
                "Native icon queue resumed: %d remaining (kept=%d)",
                #resumeQueue, kept))
        else
            nativeIconBuildIncomplete = false
            nativeIconsNeedReattach = true
        end
    else
        nativeMapBase = mapBase
        nativeMapBody = mapBody
    end
    if not nativeIconQueue then
        -- v1.2.9: re-register persistent markers in the map body's icon
        -- collection after each close, so the game's own zoom pass resumes
        -- repositioning them. Add Icon By Location also re-applies the
        -- current map transform; visibility is restored afterwards because
        -- registration can reset it.
        if nativeIconsNeedReattach then
            local addIcon = nil
            pcall(function() addIcon = mapBody["Add Icon By Location"] end)
            if addIcon then
                local updateMapIcon = nil
                pcall(function()
                    updateMapIcon = mapBody["Update Map Icon"]
                end)
                local reattached = 0
                for fullName, native in pairs(nativeIcons) do
                    local icon = native.icon
                    local info = relicInfoCache[fullName]
                    if icon and isValidObject(icon) and info then
                        pcall(function()
                            local addedOut = {}
                            addIcon(mapBody, icon, {
                                X = info.wx, Y = info.wy, Z = info.wz or 0.0,
                            }, true, false, addedOut)
                            if updateMapIcon then
                                updateMapIcon(mapBody, icon)
                            end
                        end)
                        applyNativeIconVisibility(icon, native.isPicked)
                        reattached = reattached + 1
                    end
                end
                nativeIconsNeedReattach = false
                nativeLastMapScale = nil
                if Config.Debug then
                    log(string.format(
                        "Markers re-registered after map close: %d",
                        reattached))
                end
            end
        end

        -- Self-heal: markers persist across map close, but if the game ever
        -- detaches our widgets from the icon canvas (or destroys and rebuilds
        -- the body), force one full rebuild instead of showing an empty map.
        local firstNative = next(nativeIcons) and
            nativeIcons[next(nativeIcons)] or nil
        if firstNative then
            local attached = false
            pcall(function()
                attached = firstNative.icon ~= nil
                    and isValidObject(firstNative.icon)
                    and firstNative.icon:GetParent() ~= nil
            end)
            if not attached then
                log("Markers detached by the game; rebuilding")
                resetNativeMapState()
            end
        end
        return
    end

    local queueToken = nativeIconQueueActiveToken
    local batchSize = math.max(1,
        tonumber(Config.NativeIconBatchSize) or 10)
    local createdThisTick = 0
    while nativeIconQueue
        and queueToken == nativeIconQueueToken
        and nativeIconQueueIndex <= #nativeIconQueue
        and createdThisTick < batchSize do
        if not isMapBaseOpenOnScreen(mapBase) then
            cancelNativeIconQueue("map hidden mid-batch")
            mapSessionActive = false
            nativeIconsNeedReattach = true
            return
        end
        local info = nativeIconQueue[nativeIconQueueIndex]
        nativeIconQueueIndex = nativeIconQueueIndex + 1
        if nativeIcons[info.fullName]
            and nativeIcons[info.fullName].icon
            and isValidObject(nativeIcons[info.fullName].icon) then
            -- Already created (resume path); skip.
        else
            local ok, resultOrError = createNativeIcon(mapBase, mapBody, info)
            nativePendingPoint = nil
            if ok and resultOrError then
                Diag.iconsCreated = Diag.iconsCreated + 1
                nativeIcons[info.fullName] = resultOrError
            else
                nativeIconFailed = nativeIconFailed + 1
                if nativeIconFailed <= 5 then
                    log("Native icon failed | " .. tostring(resultOrError))
                end
            end
        end
        createdThisTick = createdThisTick + 1
    end
    if queueToken ~= nativeIconQueueToken or not nativeIconQueue then
        return
    end
    if createdThisTick > 0 then
        -- Include icons created after a zoom change in the next scale pass.
        nativeLastMapScale = nil
    end

    local completed = nativeIconQueueIndex - 1
    if Config.Debug then
        log(string.format("Native icons progress: %d/%d (failed=%d)",
            completed, #nativeIconQueue, nativeIconFailed))
    end
    if nativeIconQueueIndex > #nativeIconQueue then
        log(string.format("Native icons COMPLETE: created=%d failed=%d",
            #nativeIconQueue - nativeIconFailed, nativeIconFailed))
        nativeIconQueue = nil
        nativeIconQueueActiveToken = 0
        nativeIconBuildIncomplete = false
        -- Re-run the same all-icon layout pass used by a real zoom change.
        -- This catches geometry that was not final during the early batches.
        local layoutOk, layoutError = pcall(function()
            local updateMapIcons = mapBody["Update Map Icons"]
            if not updateMapIcons then
                error("Update Map Icons function not found")
            end
            updateMapIcons(mapBody)
        end)
        log("Native map layout refresh: " ..
            (layoutOk and "complete" or ("failed | " .. tostring(layoutError))))
        nativeLastMapScale = nil
    end
end

-- DIAGNOSTIC BUILD: census helpers. FindAllOf is the expensive part and is
-- confined to one pass per DiagLogIntervalMs on the game thread.
local function countInstances(className)
    local count = -1
    pcall(function()
        local found = FindAllOf(className)
        count = found and #found or 0
    end)
    return count
end

local function countTMap(map)
    if not map then return -1 end
    local result = -1
    pcall(function()
        local n = 0
        map:ForEach(function() n = n + 1 end)
        result = n
    end)
    if result >= 0 then return result end
    pcall(function()
        local n = tonumber(map:Num())
        if n then result = n end
    end)
    return result
end

local function countTableKeys(t)
    local n = 0
    for _ in pairs(t) do n = n + 1 end
    return n
end

-- One DIAG line per interval. Interpretation:
-- * relicPoints grows without bound      -> location-point UObject leak
--   (map open path creates 407 new points per queue build; compass adds more)
-- * mapIcons grows with each map open    -> icon widgets retained by the
--   map's internal registries despite RemoveWorldMapIcon/RemoveFromParent
-- * userWidgets grows while just walking -> compass Blueprint keeps marker
--   widgets after OnRemovedLocation (silent pcall failure)
-- * locMapCombined grows                 -> LocationMap Remove silently fails
-- * setLoc high while mapSession=true and the map is closed on screen
--                                        -> stuck map session, updater churns
local function runDiagnostics()
    local censusStartedAt = os.clock()
    local mapIcons = countInstances("PalUIWorldMapIcon")
    local relicPoints = countInstances("PalLocationPoint_LevelObject")
    local userWidgets = countInstances("UserWidget")
    local locCombined = -1
    if compassManager and isValidObject(compassManager) then
        local locationMap = nil
        pcall(function() locationMap = compassManager.LocationMapCombined end)
        locCombined = countTMap(locationMap)
    end
    local luaMb = collectgarbage("count") / 1024
    local censusMs = (os.clock() - censusStartedAt) * 1000
    local tickAvgMs = Diag.tickCount > 0
        and (Diag.tickTotalMs / Diag.tickCount) or 0
    log(string.format(
        "DIAG +%ds | alive: mapIcons=%d relicPoints=%d userWidgets=%d"
            .. " locMapCombined=%d luaMB=%.1f"
            .. " | mine: nativeIcons=%d compassEntries=%d mapSession=%s queue=%s"
            .. " | tick: n=%d avg=%.2fms max=%.1fms census=%.0fms"
            .. " | fires/interval: close=%d setLoc=%d"
            .. " | totals: iconsCreated=%d queueBuilds=%d queueCancels=%d"
            .. " compassAdd=%d compassRemove=%d",
        os.time() - diagStartTime,
        mapIcons, relicPoints, userWidgets, locCombined, luaMb,
        countTableKeys(nativeIcons), countTableKeys(compassEntries),
        tostring(mapSessionActive), tostring(nativeIconQueue ~= nil),
        Diag.tickCount, tickAvgMs, Diag.tickMaxMs, censusMs,
        Diag.closeFires, Diag.setLocationFires,
        Diag.iconsCreated, Diag.queueBuilds, Diag.queueCancels,
        Diag.compassAdds, Diag.compassRemoves))
    Diag.closeFires = 0
    Diag.setLocationFires = 0
    Diag.tickCount = 0
    Diag.tickTotalMs = 0
    Diag.tickMaxMs = 0
end

local intervalMs = tonumber(Config.ScanIntervalMs) or 1000
if intervalMs < 250 then intervalMs = 250 end
local nativeIconIntervalMs = tonumber(Config.NativeIconIntervalMs) or 250
if nativeIconIntervalMs < 250 then nativeIconIntervalMs = 250 end
local tickIntervalMs = math.min(intervalMs, nativeIconIntervalMs)
local regularElapsedMs = intervalMs
local diagElapsedMs = 0
compassElapsedMs = compassUpdateIntervalMs
local mapToggleKeyName = Config.MapToggleKey
-- Backward compatibility for configs from v1.1.5 and earlier.
if mapToggleKeyName == nil then mapToggleKeyName = Config.ToggleKey end
local compassToggleKeyName = Config.CompassToggleKey

-- v1.2.4: a phantom SetLocationData wake can keep mapSessionActive true until
-- the next real map close, which previously ran the full per-second pass
-- (FindFirstOf world key, Blueprint GetCurrentMapBody, GetFullName strings)
-- and showed up as a periodic micro-stutter. Split the slow work onto its own
-- 5-second cadence; only zoom tracking stays on the fast path.
local slowScanIntervalMs = 5000
-- World/server transition is detected from creation notifications (see
-- worldScopeCheckRequested). Keep only a slow poll as a fallback for UE4SS
-- builds where NotifyOnNewObject never fires; ensureRelicCache's stale-actor
-- check is a second safety net. Start "due" so the first world is scoped at
-- load.
local worldScopeIntervalMs = 30000
local worldScopeElapsedMs = worldScopeIntervalMs
local nativeMaintenanceElapsedMs = slowScanIntervalMs
local mapVisibilityProbeElapsedMs = 0

-- A native icon batch can take longer than the 250 ms producer interval.
-- Coalesce EngineTick work so UE4SS never accumulates a queue of registry refs
-- to the same Lua function. This is the failure mode that produces
-- "Ref was not function" followed by FCallbackGarbageCollector cleanup.
local gameThreadTickQueued = false
local gameThreadTickQueuedAt = 0
Callbacks.combinedGameThreadTick = function()
    local tickStartedAt = os.clock()
    local ok, err = pcall(function()
        -- Self-heal a stuck session: pooled widgets can look briefly visible
        -- then stay "active" after the map is gone. Cheap IsVisible only.
        if mapSessionActive then
            local mapBase = nativeMapBase
            if not (mapBase and isValidObject(mapBase)) then
                mapBase = unwrap(notifiedMapBase)
            end
            if not isMapBaseOpenOnScreen(mapBase) then
                mapSessionActive = false
                cancelNativeIconQueue("session not visible")
                nativeIconsNeedReattach = nativeIconMapName ~= nil
                nativeMapBody = nil
            end
        end
        -- Only poll visibility when markers are not built yet AND the
        -- map-base Construct hook is missing.
        if not mapConstructHookInstalled
            and nativeIconMapName == nil
            and not mapSessionActive and mapMarkersVisible then
            wakeMapSessionFromVisibleMap("visibility probe", false)
            mapVisibilityProbeElapsedMs = 0
        end
        -- World-key poll is expensive (FindFirstOf). Event-driven notify is
        -- preferred; only fall back while markers are still missing.
        if worldScopeCheckRequested
            or (nativeIconMapName == nil
                and worldScopeElapsedMs >= worldScopeIntervalMs) then
            worldScopeCheckRequested = false
            worldScopeElapsedMs = 0
            refreshWorldScope()
        end
        if regularElapsedMs >= intervalMs then
            regularElapsedMs = 0
            -- Cache validation is cheap once built; skip the full relic-state
            -- pass while exploring unless the pickup hook is absent.
            if nativeIconMapName == nil or mapSessionActive
                or not pickupHookInstalled then
                ensureRelicCache()
                refreshRelicStates()
            end
        end
        processPendingPickupChecks()
        if compassElapsedMs >= compassUpdateIntervalMs then
            compassElapsedMs = 0
            refreshCompassMarkers()
        end
        -- First marker build waits for mapSessionActive (Construct / visible
        -- wake). Polling tryCreate while nativeIconMapName is nil was the
        -- pre-first-open FindFirstOf hitch.
        if nativeIconQueue ~= nil
            or (mapSessionActive and (nativeIconMapName == nil
                or nativeIconBuildIncomplete
                or nativeIconsNeedReattach
                or nativeMaintenanceElapsedMs >= slowScanIntervalMs))
            or (not mapNotifyInstalled and nativeIconMapName == nil) then
            nativeMaintenanceElapsedMs = 0
            tryCreateNativeIcons()
        end
        -- Zoom tracking is meaningless while the map is closed.
        if mapSessionActive then
            updateNativeIconZoom()
        end
        if diagIntervalMs > 0 and diagElapsedMs >= diagIntervalMs then
            diagElapsedMs = 0
            runDiagnostics()
        end
    end)
    local tickMs = (os.clock() - tickStartedAt) * 1000
    Diag.tickCount = Diag.tickCount + 1
    Diag.tickTotalMs = Diag.tickTotalMs + tickMs
    if tickMs > Diag.tickMaxMs then Diag.tickMaxMs = tickMs end
    gameThreadTickQueued = false
    gameThreadTickQueuedAt = 0
    if not ok then log("Game-thread update failed: " .. tostring(err)) end
end

local mapVisibilityUpdateQueued = false
Callbacks.applyMapVisibilityOnGameThread = function()
    local ok, updated = pcall(function()
        return applyMarkerVisibility()
    end)
    mapVisibilityUpdateQueued = false
    if not ok then
        log("Map marker visibility update failed: " .. tostring(updated))
        return
    end
    log(string.format(
        "Map markers %s (key=%s, icons=%d)",
        mapMarkersVisible and "ON" or "OFF", mapToggleKeyName, updated))
end

Callbacks.onMapToggleKey = function()
    mapMarkersVisible = not mapMarkersVisible
    if mapVisibilityUpdateQueued then return end
    mapVisibilityUpdateQueued = true
    local ok, err = pcall(ExecuteInGameThread,
        Callbacks.applyMapVisibilityOnGameThread)
    if not ok then
        mapVisibilityUpdateQueued = false
        log("Map marker visibility queue failed: " .. tostring(err))
    end
end

local compassVisibilityUpdateQueued = false
Callbacks.applyCompassVisibilityOnGameThread = function()
    local ok, err = pcall(function()
        if compassMarkersVisible then
            compassElapsedMs = compassUpdateIntervalMs
            refreshCompassMarkers()
        else
            resetCompassState("compass toggle off")
        end
    end)
    compassVisibilityUpdateQueued = false
    if not ok then
        log("Compass marker visibility update failed: " .. tostring(err))
        return
    end
    log(string.format("Compass markers %s (key=%s)",
        compassMarkersVisible and "ON" or "OFF", compassToggleKeyName))
end

Callbacks.onCompassToggleKey = function()
    compassMarkersVisible = not compassMarkersVisible
    if compassVisibilityUpdateQueued then return end
    compassVisibilityUpdateQueued = true
    local ok, err = pcall(ExecuteInGameThread,
        Callbacks.applyCompassVisibilityOnGameThread)
    if not ok then
        compassVisibilityUpdateQueued = false
        log("Compass marker visibility queue failed: " .. tostring(err))
    end
end

local function registerToggleKey(keyName, defaultKey, callback, label)
    if keyName == nil then keyName = defaultKey end
    if keyName == false or tostring(keyName) == "" then
        log(label .. " toggle key disabled")
        return "disabled"
    end
    keyName = string.upper(tostring(keyName))
    local toggleKey = Key and Key[keyName] or nil
    if toggleKey then
        local keyBindOk, keyBindError = pcall(function()
            RegisterKeyBind(toggleKey, callback)
        end)
        if keyBindOk then
            log(label .. " toggle key registered: " .. keyName)
        else
            log(label .. " toggle key FAILED: " .. tostring(keyBindError))
        end
    else
        log("Invalid " .. string.lower(label) .. " toggle key: " .. keyName)
    end
    return keyName
end

mapToggleKeyName = registerToggleKey(mapToggleKeyName, "F9",
    Callbacks.onMapToggleKey, "Map marker")
compassToggleKeyName = registerToggleKey(compassToggleKeyName, "F10",
    Callbacks.onCompassToggleKey, "Compass marker")

Callbacks.asyncTick = function()
    regularElapsedMs = regularElapsedMs + tickIntervalMs
    compassElapsedMs = compassElapsedMs + tickIntervalMs
    diagElapsedMs = diagElapsedMs + tickIntervalMs
    worldScopeElapsedMs = worldScopeElapsedMs + tickIntervalMs
    nativeMaintenanceElapsedMs = nativeMaintenanceElapsedMs + tickIntervalMs
    mapVisibilityProbeElapsedMs = mapVisibilityProbeElapsedMs + tickIntervalMs
    -- The 250 ms cadence is needed only while the creation queue is active.
    -- Once all icons exist, zoom/state maintenance enters the game thread at
    -- the configured general interval (1 second by default).
    local creationActive = nativeIconQueue ~= nil
        or mapInitialStateScanActive
        or (mapSessionActive and nativeIconBuildIncomplete)
    local regularTickDue = regularElapsedMs >= intervalMs
    -- Visibility probe is a last-resort path for first marker build when the
    -- map-base Construct hook could not be installed. Never poll after the
    -- marker set exists — that was the 1.2.1 walking hitch.
    local mapProbeDue = not mapConstructHookInstalled
        and nativeIconMapName == nil
        and not mapSessionActive and mapMarkersVisible
        and mapVisibilityProbeElapsedMs >= intervalMs
    -- Do not wake solely because markers are not built yet: that reintroduced
    -- the pre-first-map FindFirstOf hitch (fixed in 1.2.3). Construct /
    -- Notify / probe wake the session; Notify-missing still polls.
    local mapWorkDue = (mapSessionActive or creationActive
        or not mapNotifyInstalled)
        and (creationActive or regularTickDue
            or (mapSessionActive and nativeIconsNeedReattach))
    local compassWorkDue = compassMarkersVisible
        and compassElapsedMs >= compassUpdateIntervalMs
    local diagWorkDue = diagIntervalMs > 0
        and diagElapsedMs >= diagIntervalMs
    local pickupRecheckWorkDue = next(pendingPickupChecks) ~= nil
        and os.time() >= pickupRecheckAt
    local workDue = mapWorkDue or compassWorkDue
        or pickupRecheckWorkDue or diagWorkDue or mapProbeDue
    if workDue and gameThreadTickQueued
        and os.time() - gameThreadTickQueuedAt >= 10 then
        -- Recover if UE4SS discarded a queued callback before invoking it.
        gameThreadTickQueued = false
        gameThreadTickQueuedAt = 0
        log("Recovering a stale game-thread update")
    end
    if workDue and not gameThreadTickQueued then
        gameThreadTickQueued = true
        gameThreadTickQueuedAt = os.time()
        local ok, err = pcall(ExecuteInGameThread,
            Callbacks.combinedGameThreadTick)
        if not ok then
            gameThreadTickQueued = false
            gameThreadTickQueuedAt = 0
            log("Game-thread update queue failed: " .. tostring(err))
        end
    end
    return false
end
LoopAsync(tickIntervalMs, Callbacks.asyncTick)

log(string.format(
    "Mod loaded (v%s: no pre-first-map FindFirstOf, WBP_Map_Base Construct, session-token queues, diag=%dms, map=%s/%s, hide collected=%s, grayscale=%s, zoom=0.5, brightness=%.2f, size=%d, batch=%d, nativeInterval=%dms, stateScan=%dms, compass=%s/%s/%d markers/%.0fm/%dms)",
    MOD_VERSION,
    diagIntervalMs,
    tostring(mapMarkersVisible),
    tostring(mapToggleKeyName),
    tostring(hideCollectedIcons),
    tostring(collectedIconGrayscale),
    uncollectedIconBrightness,
    nativeIconSize,
    tonumber(Config.NativeIconBatchSize) or 10,
    nativeIconIntervalMs,
    math.floor(stateScanIntervalSeconds * 1000),
    tostring(compassMarkersVisible),
    tostring(compassToggleKeyName),
    compassMaxMarkers,
    compassRangeMeters,
    compassUpdateIntervalMs))
