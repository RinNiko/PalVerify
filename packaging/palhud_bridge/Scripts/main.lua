local MOD_NAME = "PalHudBridge"
local VERSION = "1.0.0"
local REFRESH_MS = 5000
local PROTOCOL_PREFIX = "[PALHUD]|"
local PLAYER_CLASS_NAMES = {
    "PalPlayerCharacter",
    "BP_Player_Female_C",
    "BP_Player_Male_C",
}

local function log(level, message)
    print(string.format("[%s] [%s] %s\n", MOD_NAME, level, message))
end

local function script_directory()
    local info =
        debug and debug.getinfo and debug.getinfo(1, "S") or nil
    local source = info and tostring(info.source or "") or ""
    if source:sub(1, 1) == "@" then
        source = source:sub(2)
    end
    source = source:gsub("/", "\\")
    return source:match("^(.*\\)") or ".\\"
end

local RUNTIME_PATH =
    script_directory() .. "..\\..\\PalBooster\\Scripts\\runtime.lua"

local function is_valid(value)
    if value == nil then
        return false
    end
    local ok, valid = pcall(function()
        return value:IsValid()
    end)
    return ok and valid == true
end

local function call_method(target, method_name, ...)
    if not is_valid(target) then
        return false, nil
    end
    local method_ok, method = pcall(function()
        return target[method_name]
    end)
    if not method_ok or method == nil then
        return false, nil
    end
    local ok, result = pcall(method, target, ...)
    if not ok then
        return false, nil
    end
    return true, result
end

local function as_text(value)
    if type(value) == "string" then
        return value
    end
    local ok, text = call_method(value, "ToString")
    if ok and text ~= nil then
        return tostring(text)
    end
    if type(value) == "number" or type(value) == "boolean" then
        return tostring(value)
    end
    return ""
end

local function safe_protocol_text(value)
    local text = tostring(value or "")
    text = text:gsub("[%c|]", " "):gsub("%s+", " ")
    text = text:match("^%s*(.-)%s*$") or ""
    if #text > 240 then
        text = text:sub(1, 240)
    end
    return text
end

local function safe_source_label(value)
    local label = safe_protocol_text(value)
    if #label > 80 then
        label = label:sub(1, 80)
    end
    return label
end

local function normalized_player_name(value)
    return safe_protocol_text(value):lower()
end

local function format_multiplier(value)
    local multiplier = tonumber(value) or 1
    local formatted = string.format("%.2f", multiplier)
    return formatted:gsub("0+$", ""):gsub("%.$", "")
end

local function protocol_sources(status)
    local labels = {}
    if type(status) == "table" and type(status.sources) == "table" then
        for _, source in ipairs(status.sources) do
            local label = safe_source_label(source)
            if label ~= "" and #labels < 4 then
                table.insert(labels, label)
            end
        end
    end
    return table.concat(labels, "  +  ")
end

local function build_protocol(status, syncing, fallback_name)
    local boosted = type(status) == "table"
        and tonumber(status.multiplier) ~= nil
        and tonumber(status.multiplier) > 1
    local active_since = boosted
        and math.max(0, math.floor(
            tonumber(status.active_since_unix) or 0
        ))
        or 0
    local active_until = boosted
        and math.max(0, math.floor(
            tonumber(status.active_until_unix) or 0
        ))
        or 0
    local gacha_spins = type(status) == "table"
        and math.floor(tonumber(status.gacha_spins) or -1)
        or -1
    if gacha_spins < -1 or gacha_spins > 100000 then
        gacha_spins = -1
    end
    local player_name = type(status) == "table"
        and status.player_name
        or fallback_name
    return PROTOCOL_PREFIX
        .. (syncing and "2" or (boosted and "1" or "0"))
        .. "|"
        .. tostring(active_since)
        .. "|"
        .. tostring(active_until)
        .. "|"
        .. format_multiplier(boosted and status.multiplier or 1)
        .. "|"
        .. (boosted and status.capped == true and "1" or "0")
        .. "|"
        .. tostring(gacha_spins)
        .. "|"
        .. safe_protocol_text(player_name)
        .. "|"
        .. safe_protocol_text(protocol_sources(status))
end

local function sanitize_runtime(document)
    if type(document) ~= "table"
        or type(document.expires_at_unix) ~= "number"
        or type(document.hud_players) ~= "table"
    then
        return nil
    end
    local clean = {
        revision = tostring(document.revision or "unknown"),
        expires_at_unix = document.expires_at_unix,
        hud_players = {},
    }
    for key, status in pairs(document.hud_players) do
        if type(status) == "table" then
            local clean_status = {
                player_name = tostring(status.player_name or ""),
                active_since_unix =
                    tonumber(status.active_since_unix) or 0,
                active_until_unix =
                    tonumber(status.active_until_unix) or 0,
                multiplier = tonumber(status.multiplier) or 1,
                capped = status.capped == true,
                gacha_spins = tonumber(status.gacha_spins) or -1,
                sources = {},
            }
            if type(status.sources) == "table" then
                for _, source in ipairs(status.sources) do
                    if type(source) == "string" then
                        table.insert(clean_status.sources, source)
                    end
                end
            end
            clean.hud_players[tostring(key)] = clean_status
        end
    end
    return clean
end

local function runtime_status(runtime, player_name)
    local normalized = normalized_player_name(player_name)
    if normalized == ""
        or type(runtime) ~= "table"
        or type(runtime.hud_players) ~= "table"
    then
        return nil
    end
    local match = nil
    for _, status in pairs(runtime.hud_players) do
        if type(status) == "table"
            and normalized_player_name(status.player_name) == normalized
        then
            if match ~= nil then
                return nil
            end
            match = status
        end
    end
    return match
end

local function object_key(value)
    if not is_valid(value) then
        return value
    end
    local ok, full_name = call_method(value, "GetFullName")
    if ok and full_name ~= nil then
        return tostring(full_name)
    end
    return value
end

local function state_for_player(player, controller)
    local state_ok, state = call_method(player, "GetCachedPlayerState")
    if state_ok and is_valid(state) then
        return state
    end
    state_ok, state = call_method(controller, "GetPalPlayerState")
    if state_ok and is_valid(state) then
        return state
    end
    return nil
end

local function deliver(runtime, message_type)
    local found = {}
    local seen = {}
    for _, class_name in ipairs(PLAYER_CLASS_NAMES) do
        local ok, instances = pcall(FindAllOf, class_name)
        if ok and type(instances) == "table" then
            for _, player in ipairs(instances) do
                local key = object_key(player)
                if not seen[key] then
                    seen[key] = true
                    table.insert(found, player)
                end
            end
        end
    end

    local stats = {
        found = #found,
        invalid = 0,
        no_controller = 0,
        no_state = 0,
        no_name = 0,
        ready = 0,
        sent = 0,
        failed = 0,
    }
    local now = os.time()
    local active = type(runtime) == "table"
        and tonumber(runtime.expires_at_unix) ~= nil
        and runtime.expires_at_unix > now

    for _, player in ipairs(found) do
        if not is_valid(player) then
            stats.invalid = stats.invalid + 1
        else
            local controller_ok, controller = call_method(
                player,
                "GetPalPlayerController"
            )
            if not controller_ok or not is_valid(controller) then
                stats.no_controller = stats.no_controller + 1
            else
                local state = state_for_player(player, controller)
                if state == nil then
                    stats.no_state = stats.no_state + 1
                else
                    local name_ok, raw_name = call_method(
                        state,
                        "GetPlayerName"
                    )
                    local player_name = name_ok and as_text(raw_name) or ""
                    player_name =
                        player_name:match("^%s*(.-)%s*$") or ""
                    if player_name == "" then
                        stats.no_name = stats.no_name + 1
                    else
                        stats.ready = stats.ready + 1
                        local status = active
                            and runtime_status(runtime, player_name)
                            or nil
                        local syncing = not active or status == nil
                        local sent = call_method(
                            controller,
                            "ClientMessage",
                            build_protocol(
                                status,
                                syncing,
                                player_name
                            ),
                            message_type,
                            0
                        )
                        if sent then
                            stats.sent = stats.sent + 1
                        else
                            stats.failed = stats.failed + 1
                        end
                    end
                end
            end
        end
    end
    return stats
end

if _G.__PALHUD_BRIDGE_TESTING == true then
    _G.__PALHUD_BRIDGE_TEST_API = {
        build_protocol = build_protocol,
        deliver = deliver,
        is_valid = is_valid,
        sanitize_runtime = sanitize_runtime,
    }
    return
end

local runtime = {
    revision = "unavailable",
    expires_at_unix = 0,
    hud_players = {},
}
local reload_pending = false
local delivery_pending = false
local delivery_started_logged = false
local last_scan_signature = nil
local message_type = nil

local function create_message_type()
    local ok, value = pcall(function()
        return FName("PalHud", EFindName.FNAME_Add)
    end)
    if ok then
        return value
    end
    return FName("PalHud")
end

local function queue_runtime_reload()
    if reload_pending then
        return
    end
    reload_pending = true
    ExecuteAsync(function()
        local ok, document = pcall(dofile, RUNTIME_PATH)
        local clean = ok and sanitize_runtime(document) or nil
        ExecuteInGameThread(function()
            reload_pending = false
            if clean ~= nil then
                runtime = clean
            end
        end)
    end)
end

local function queue_delivery()
    if delivery_pending then
        return
    end
    delivery_pending = true
    local queued, queue_error = pcall(
        ExecuteInGameThread,
        function()
            delivery_pending = false
            local ok, stats = pcall(deliver, runtime, message_type)
            if not ok then
                log("ERROR", "Delivery failed: " .. tostring(stats))
                return
            end
            local signature = string.format(
                "%d:%d:%d:%d:%d:%d:%d",
                stats.found,
                stats.ready,
                stats.sent,
                stats.invalid,
                stats.no_controller,
                stats.no_state,
                stats.no_name
            )
            if signature ~= last_scan_signature then
                last_scan_signature = signature
                log(
                    "INFO",
                    string.format(
                        "Player scan found=%d ready=%d sent=%d invalid=%d no_controller=%d no_state=%d no_name=%d.",
                        stats.found,
                        stats.ready,
                        stats.sent,
                        stats.invalid,
                        stats.no_controller,
                        stats.no_state,
                        stats.no_name
                    )
                )
            end
            if stats.sent > 0 and not delivery_started_logged then
                delivery_started_logged = true
                log(
                    "INFO",
                    "HUD delivery started via targeted ClientMessage."
                )
            end
        end
    )
    if not queued then
        delivery_pending = false
        log("ERROR", "Could not queue delivery: " .. tostring(queue_error))
    end
end

message_type = create_message_type()
queue_runtime_reload()
queue_delivery()
LoopAsync(REFRESH_MS, function()
    queue_runtime_reload()
    queue_delivery()
end)
log(
    "INFO",
    string.format(
        "v%s started (server-only, no UObject cache).",
        VERSION
    )
)
