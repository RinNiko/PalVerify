local Common = {}

local commonSource = debug.getinfo(1, "S").source or ""
local commonDirectory = commonSource:sub(1, 1) == "@"
    and (commonSource:sub(2):match("^(.*)[/\\]") or ".") or "."
local memberLayoutChecked = false

-- Palworld uses a customized Unreal Engine layout. UE4SS reads this file
-- before Lua mods start, so this shared check only reports an incompatible
-- installation; the user must reinstall UE4SS and restart the game.
local function warnIfMemberVariableLayoutMissing()
    if memberLayoutChecked
        or _G.__PALWORLD_MEMBER_VARIABLE_LAYOUT_CHECKED then
        return
    end
    memberLayoutChecked = true
    _G.__PALWORLD_MEMBER_VARIABLE_LAYOUT_CHECKED = true

    local candidates = {
        "MemberVariableLayout.ini",
        commonDirectory .. "/../../../MemberVariableLayout.ini",
    }
    for _, path in ipairs(candidates) do
        local openOk, file = pcall(io.open, path, "r")
        if openOk and file then
            pcall(function() file:close() end)
            return
        end
    end

    print("[Palworld UE4SS] WARNING: MemberVariableLayout.ini was not found. This UE4SS installation may be incompatible with the current Palworld version and may cause crashes in Lua mods that access Unreal objects.\n")
    print("[Palworld UE4SS] Reinstall the Palworld experimental UE4SS release, then fully restart the game: https://github.com/Okaetsu/RE-UE4SS/releases/tag/experimental-palworld\n")
end

-- Create a small per-mod logger without storing any mutable global state.
function Common.createLogger(modName, debugEnabled)
    warnIfMemberVariableLayoutMissing()
    local logger = {}

    function logger.info(message)
        print(string.format("[%s] %s\n", modName, tostring(message)))
    end

    function logger.debug(message)
        if debugEnabled then logger.info(message) end
    end

    return logger
end

-- Return the directory of the Lua file which called this function. This must
-- inspect the caller because debug.getinfo(1) would point at common.lua.
function Common.getScriptDirectory()
    local info = debug.getinfo(2, "S")
    local source = info and info.source or ""
    if type(source) ~= "string" or source:sub(1, 1) ~= "@" then
        return "."
    end
    return source:sub(2):match("^(.*)[/\\]") or "."
end

-- RemoteUnrealParam values use lowercase get(). If get() returns nil, retain
-- the original value so callers can still handle ordinary Lua/UObject values.
function Common.unwrapRemote(value, keepSuccessfulNil)
    if value == nil then return nil end
    local ok, result = pcall(function() return value:get() end)
    if ok and (result ~= nil or keepSuccessfulNil) then return result end
    return value
end

-- UObject-like reflected references may use Get(), while hook parameters use
-- get(). This deliberately does not load a soft reference.
function Common.unwrapObjectRef(value)
    if value == nil then return nil end
    local ok, result = pcall(function() return value:Get() end)
    if ok and result then return result end
    ok, result = pcall(function() return value:get() end)
    if ok and result then return result end
    return value
end

-- Resolve an object or soft-object reference. Keep this separate from scalar
-- conversion: calling Get() first on a scalar RemoteUnrealParam can produce a
-- dummy UObject on some UE4SS builds.
function Common.resolveObjectRef(value)
    if value == nil then return nil end
    local ok, result = pcall(function() return value:Get() end)
    if ok and result then return result end
    ok, result = pcall(function() return value:LoadSynchronous() end)
    if ok and result then return result end
    ok, result = pcall(function() return value:get() end)
    if ok and result then return result end
    return value
end

-- Scalar hook parameters must prefer lowercase get(); see resolveObjectRef.
function Common.readNumberParam(value)
    if value == nil then return nil end
    local ok, result = pcall(function() return value:get() end)
    if ok then
        local number = tonumber(result)
        if number ~= nil then return number end
    end
    local directOk, direct = pcall(tonumber, value)
    if directOk and direct ~= nil then return direct end
    local textOk, text = pcall(tostring, value)
    if not textOk then return nil end
    return tonumber(text)
end

function Common.toString(value, unwrap)
    if unwrap then value = unwrap(value) end
    if value == nil then return nil end
    if type(value) == "string" then return value end
    local ok, result = pcall(function() return value:ToString() end)
    if ok and result ~= nil then return tostring(result) end
    return tostring(value)
end

function Common.isValid(object)
    if not object then return false end
    local ok, valid = pcall(function() return object:IsValid() end)
    return ok and valid == true
end

-- A missing reflected property can be returned as a dummy UObject. Calling a
-- method on that dummy can crash outside pcall, so detect it before unwrapping.
function Common.isMissingPropertyDummy(value)
    if value == nil then return false end
    local ok, text = pcall(tostring, value)
    return ok and type(text) == "string"
        and text:match("^UObject:%s*%x+$") ~= nil
end

function Common.getProperty(object, names, unwrap)
    unwrap = unwrap or Common.unwrapRemote
    object = unwrap(object)
    if not object then return nil end

    for _, name in ipairs(names) do
        local ok, value = pcall(function() return object[name] end)
        if ok and value ~= nil
            and not Common.isMissingPropertyDummy(value) then
            local resolved = unwrap(value)
            if resolved ~= nil then return resolved end
        end
    end
    return nil
end

function Common.round(value)
    if value >= 0 then return math.floor(value + 0.5) end
    return math.ceil(value - 0.5)
end

function Common.clamp(value, minimum, maximum)
    return math.max(minimum, math.min(maximum, value))
end

function Common.requireFiniteNumber(value, fieldName)
    local number = tonumber(value)
    if number == nil or number ~= number
        or number == math.huge or number == -math.huge then
        error((fieldName or "value") .. " must be a finite number")
    end
    return number
end

return Common
