package.path = ".\\ue4ss\\Mods\\PalVerify\\Scripts\\?.lua;"
    .. ".\\ue4ss\\Mods\\PalVerify\\Scripts\\?\\init.lua;"
    .. ".\\Mods\\PalVerify\\Scripts\\?.lua;"
    .. ".\\Mods\\PalVerify\\Scripts\\?\\init.lua;"
    .. package.path

local Config = require("config")
local SymbolFilter = require("palverify.symbol_filter")

local started = false

local function log(level, message)
    print(string.format("[PalVerify] [%s] %s\n", level, message))
end

local function call_method(object, method_name, ...)
    if object == nil then
        return false, nil
    end

    local args = { ... }
    return pcall(function()
        local method = object[method_name]
        if method == nil then
            error("method unavailable: " .. method_name)
        end
        return method(object, table.unpack(args))
    end)
end

local function is_valid(object)
    local ok, value = call_method(object, "IsValid")
    return ok and value == true
end

local function as_text(value)
    if type(value) == "string" then
        return value
    end
    if value == nil then
        return nil
    end

    local ok, text = call_method(value, "ToString")
    if ok and type(text) == "string" then
        return text
    end
    return nil
end

local function object_name(object)
    local ok, name = call_method(object, "GetFName")
    if not ok then
        return nil
    end
    return as_text(name)
end

local function collect_class_symbols(class_path)
    local ok, current_class = pcall(StaticFindObject, class_path)
    if not ok or not is_valid(current_class) then
        return nil, nil
    end

    local properties = {}
    local functions = {}
    while is_valid(current_class) do
        call_method(current_class, "ForEachProperty", function(property)
            local name = object_name(property)
            if name ~= nil then
                table.insert(properties, name)
            end
        end)
        call_method(current_class, "ForEachFunction", function(func)
            local name = object_name(func)
            if name ~= nil then
                table.insert(functions, name)
            end
        end)

        local super_ok, super_class =
            call_method(current_class, "GetSuperStruct")
        if not super_ok or not is_valid(super_class) then
            break
        end
        current_class = super_class
    end

    return SymbolFilter.collect(properties), SymbolFilter.collect(functions)
end

local function format_symbols(symbols)
    if symbols == nil or #symbols == 0 then
        return "none"
    end
    return table.concat(symbols, ",")
end

local function run_probe()
    if started then
        return
    end
    started = true

    log("INFO", string.format(
        "v%s started observation_only=%s",
        Config.version,
        tostring(Config.observation_only)
    ))

    for _, class_path in ipairs(Config.classes) do
        local properties, functions = collect_class_symbols(class_path)
        if properties == nil then
            log("WARN", "class_unavailable class=" .. class_path)
        else
            log("INFO", string.format(
                "class_candidates class=%s properties=%s functions=%s",
                class_path,
                format_symbols(properties),
                format_symbols(functions)
            ))
        end
    end

    log(
        "INFO",
        "probe_complete values_read=false enforcement=false pii_collected=false"
    )
end

RegisterInitGameStatePostHook(function()
    run_probe()
end)

ExecuteInGameThreadWithDelay(Config.startup_delay_ms, function()
    run_probe()
end)

log("INFO", string.format(
    "v%s loaded; waiting for game state; observation_only=%s",
    Config.version,
    tostring(Config.observation_only)
))
