local MOD_NAME = "PalHud"
local VERSION = "1.6.0"
local HUD_TICK_MS = 1000
local SERVER_REFRESH_SECONDS = 5
local DISCOVERY_RETRY_SECONDS = 5
local PROTOCOL_PREFIX = "[PALHUD]|"
local COMBAT_PROTOCOL_PREFIX = "[PALCOMBAT]|"
local GACHA_PROTOCOL_PREFIX = "[PALHUDGACHA]|"
local HOLOGRAM_PROTOCOL_PREFIX = "[PALHOLO]|"
local HOLOGRAM_DEFAULT_MAX_DISTANCE = 8000
local HOLOGRAM_HEIGHT_OFFSET = 180
local HOLOGRAM_MAX_COUNT = 24
local HOLOGRAM_MAX_LINES = 4
local HOLOGRAM_MAX_LINE_CHARACTERS = 80
local HOLOGRAM_TICK_MS = 100
local CONTROLLER_POSSESS_PATH = "/Script/Engine.Controller:Possess"
local CONTROLLER_UNPOSSESS_PATH = "/Script/Engine.Controller:UnPossess"
local GAME_MODE_LOGOUT_PATH = "/Script/Engine.GameModeBase:K2_OnLogout"
local CHAT_PATH = "/Script/Pal.PalGameStateInGame:BroadcastChatMessage"
local CLIENT_MESSAGE_PATH = "/Script/Engine.PlayerController:ClientMessage"
local USER_WIDGET_CLASS_PATH = "/Script/UMG.UserWidget"
local WIDGET_TREE_CLASS_PATH = "/Script/UMG.WidgetTree"
local TEXT_BLOCK_CLASS_PATH = "/Script/UMG.TextBlock"
local SIZE_BOX_CLASS_PATH = "/Script/UMG.SizeBox"
local BORDER_CLASS_PATH = "/Script/UMG.Border"
local VERTICAL_BOX_CLASS_PATH = "/Script/UMG.VerticalBox"
local CANVAS_PANEL_CLASS_PATH = "/Script/UMG.CanvasPanel"
local PROGRESS_BAR_CLASS_PATH = "/Script/UMG.ProgressBar"
local WIDGET_BLUEPRINT_LIBRARY_PATH =
    "/Script/UMG.Default__WidgetBlueprintLibrary"
local RENDERING_LIBRARY_PATH =
    "/Script/Engine.Default__KismetRenderingLibrary"
local HUD_WIDGET_TREE_NAME = "PalHud_WidgetTree"
local HUD_CARD_ROOT_NAME = "PalHud_CardRoot"
local HUD_FRAME_BORDER_NAME = "PalHud_FrameBorder"
local HUD_PANEL_BORDER_NAME = "PalHud_PanelBorder"
local HUD_CONTENT_NAME = "PalHud_Content"
local HUD_PLAYER_NAME = "PalHud_Player"
local HUD_GACHA_NAME = "PalHud_Gacha"
local HUD_GACHA_TIME_NAME = "PalHud_GachaTime"
local HUD_GACHA_PROGRESS_HEIGHT_NAME = "PalHud_GachaProgressHeight"
local HUD_GACHA_PROGRESS_NAME = "PalHud_GachaProgress"
local HUD_COMBAT_NAME = "PalHud_Combat"
local HUD_COMBAT_PROGRESS_HEIGHT_NAME = "PalHud_CombatProgressHeight"
local HUD_COMBAT_PROGRESS_NAME = "PalHud_CombatProgress"
local HUD_BOOSTER_NAME = "PalHud_Booster"
local HUD_TIME_NAME = "PalHud_Time"
local HUD_PROGRESS_HEIGHT_NAME = "PalHud_ProgressHeight"
local HUD_PROGRESS_NAME = "PalHud_Progress"
local HOLOGRAM_WIDGET_TREE_NAME = "PalHud_HologramWidgetTree"
local HOLOGRAM_CANVAS_NAME = "PalHud_HologramCanvas"
local HOLOGRAM_NOTICE_NAME = "PalHud_HologramNotice"
local PLAYER_CLASS_NAMES = {
    "PalPlayerCharacter",
    "BP_Player_Female_C",
    "BP_Player_Male_C",
}
PalHudCallbacks = PalHudCallbacks or {}
local Callbacks = PalHudCallbacks
local Hologram = {}

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
local LOGO_PATH =
    script_directory() .. "..\\Assets\\logo-wordmark-hud.png"
local HOLOGRAM_STORE_PATH = rawget(_G, "__PALHUD_TEST_STORE_PATH")
    or (script_directory() .. "holograms.tsv")

local started = false
local reload_pending = false
local discovery_complete = false
local next_discovery_at = 0
local next_runtime_reload_at = 0
local next_server_delivery_at = 0
local protocol_message_type = nil
local controllers = {}
local render_failure_logged = false
local protocol_received_logged = false
local render_started_logged = false
local combat_visibility_logged = nil
local delivery_started_logged = false
local last_delivery_failure_reason = nil
local last_discovery_signature = nil
local last_render_failure_reason = nil
local hud_visible = true
local visibility_update_pending = false
local hud_tick_pending = false
local hud_tick_queued_at = 0
local hud_overlay = nil
local hud_player_block = nil
local hud_gacha_block = nil
local hud_gacha_time_block = nil
local hud_gacha_progress_box = nil
local hud_gacha_progress_bar = nil
local hud_combat_block = nil
local hud_combat_progress_box = nil
local hud_combat_progress_bar = nil
local hud_booster_block = nil
local hud_time_block = nil
local hud_progress_box = nil
local hud_progress_bar = nil
local widget_blueprint_library = nil
local rendering_library = nil
local background_logo_texture = nil
local user_widget_class = nil
local widget_tree_class = nil
local text_block_class = nil
local size_box_class = nil
local border_class = nil
local vertical_box_class = nil
local canvas_panel_class = nil
local progress_bar_class = nil
local server_holograms = {}
local server_hologram_revision = 0
local hologram_store_loaded = false
local hologram_load_pending = false
local hologram_persist_pending = false
local hologram_persist_dirty = false
local client_holograms = {}
local client_hologram_revision = -1
local hologram_overlay = nil
local hologram_canvas = nil
local hologram_notice_border = nil
local hologram_notice_text = nil
local hologram_notice_hide_at = 0
local hologram_loop_started = false
local hologram_tick_pending = false
local runtime = {
    revision = "unavailable",
    expires_at_unix = 0,
    hud_players = {},
}
local combat_state = {
    active = false,
    remaining = 0,
    duration = 10,
    received_at = 0,
}
local gacha_reward_state = {
    active = false,
    remaining = 0,
    reward_spins = 0,
    interval_seconds = 0,
    received_at = 0,
    balance = nil,
}
local last_client_view = {
    player = "Người chơi • ĐANG ĐỒNG BỘ",
    gacha = "Gacha • ĐANG ĐỒNG BỘ",
    gacha_time = "",
    gacha_active = false,
    gacha_progress = 0,
    booster = "",
    time = "",
    boosted = false,
    progress = 0,
}

local function log(level, message)
    print(string.format("[%s] [%s] %s\n", MOD_NAME, level, message))
end

local function unwrap(value)
    if value == nil then
        return nil
    end
    local kind = type(value)
    if kind ~= "userdata" and kind ~= "table" then
        return value
    end
    local ok, result = pcall(function()
        return value:get()
    end)
    if ok then
        return result
    end
    return value
end

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

local function call_value_method(target, method_name, ...)
    if target == nil then
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

local function apply_hud_visibility()
    local updated = false
    if is_valid(hud_overlay) then
        updated = call_method(
            hud_overlay,
            "SetVisibility",
            hud_visible and 3 or 1
        )
    end
    if is_valid(hologram_overlay) then
        updated = call_method(
            hologram_overlay,
            "SetVisibility",
            hud_visible and 3 or 1
        ) or updated
    end
    return updated
end

local function collapse_hud_for_local_controller(raw_controller)
    if not is_valid(hud_overlay) and not is_valid(hologram_overlay) then
        return false
    end
    local controller = unwrap(raw_controller)
    local local_ok, is_local = call_method(
        controller,
        "IsLocalPlayerController"
    )
    if not local_ok or is_local ~= true then
        return false
    end
    local collapsed = false
    if is_valid(hud_overlay) then
        collapsed = call_method(hud_overlay, "SetVisibility", 1)
    end
    if is_valid(hologram_overlay) then
        collapsed = call_method(hologram_overlay, "SetVisibility", 1)
            or collapsed
    end
    return collapsed
end

local function toggle_hud_visibility()
    hud_visible = not hud_visible
    if visibility_update_pending then
        return
    end
    visibility_update_pending = true
    local queued, queue_error = pcall(
        ExecuteInGameThread,
        function()
            visibility_update_pending = false
            if apply_hud_visibility() then
                log(
                    "INFO",
                    hud_visible and "HUD shown via F5." or "HUD hidden via F5."
                )
            end
        end
    )
    if not queued then
        visibility_update_pending = false
        log(
            "ERROR",
            "F5 visibility update failed: " .. tostring(queue_error)
        )
    end
end
Callbacks.toggle_hud_visibility = toggle_hud_visibility

local function read_member(target, member_name)
    if target == nil then
        return nil
    end
    local ok, value = pcall(function()
        return target[member_name]
    end)
    if not ok then
        return nil
    end
    return unwrap(value)
end

local function as_text(value)
    if type(value) == "string" then
        return value
    end
    local ok, text = call_value_method(value, "ToString")
    if ok and text ~= nil then
        return tostring(text)
    end
    value = unwrap(value)
    if type(value) == "string" then
        return value
    end
    ok, text = call_value_method(value, "ToString")
    if ok and text ~= nil then
        return tostring(text)
    end
    if value ~= nil then
        return tostring(value)
    end
    return ""
end

local function find_required_object(path)
    local ok, value = pcall(StaticFindObject, path)
    if ok and is_valid(value) then
        return value
    end
    log("ERROR", "Missing required runtime object: " .. path)
    return nil
end

local function new_object_name(value)
    local ok, name = pcall(function()
        return FName(value, EFindName.FNAME_Add)
    end)
    if ok then
        return name
    end
    return FName(value)
end

local function safe_display_text(value)
    local text = tostring(value or "")
    text = text:gsub("[%c]", " "):gsub("%s+", " ")
    text = text:match("^%s*(.-)%s*$") or ""
    if #text > 240 then
        text = text:sub(1, 240)
    end
    return text
end

function Hologram.trim(value)
    return tostring(value or ""):match("^%s*(.-)%s*$") or ""
end

function Hologram.truncate_utf8(value, max_characters)
    local text = tostring(value or "")
    if utf8 == nil or utf8.offset == nil then
        return #text > max_characters and text:sub(1, max_characters) or text
    end
    local ok, next_offset = pcall(
        utf8.offset,
        text,
        max_characters + 1
    )
    if not ok or next_offset == nil then
        return text
    end
    return text:sub(1, next_offset - 1)
end

function Hologram.percent_encode(value)
    return tostring(value or ""):gsub(
        "([^A-Za-z0-9%-%._])",
        function(character)
            return string.format("%%%02X", string.byte(character))
        end
    )
end

function Hologram.percent_decode(value)
    return tostring(value or ""):gsub(
        "%%(%x%x)",
        function(hex)
            return string.char(tonumber(hex, 16))
        end
    )
end

function Hologram.sanitize_id(value)
    local id = Hologram.trim(value):lower()
    if id:match("^[a-z0-9][a-z0-9_-]*$") == nil or #id > 32 then
        return nil
    end
    return id
end

function Hologram.sanitize_text(value)
    local input = tostring(value or "")
    local lines = {}
    for segment in (input .. "|"):gmatch("(.-)|") do
        local line = Hologram.trim(
            segment:gsub(
                "[%z\1-\8\11\12\14-\31\127]",
                " "
            ):gsub("%s+", " ")
        )
        if line ~= "" then
            table.insert(
                lines,
                Hologram.truncate_utf8(
                    line,
                    HOLOGRAM_MAX_LINE_CHARACTERS
                )
            )
            if #lines >= HOLOGRAM_MAX_LINES then
                break
            end
        end
    end
    if #lines == 0 then
        return nil
    end
    return table.concat(lines, "\n")
end

function Hologram.parse_command(raw_message)
    local message = Hologram.trim(raw_message)
    local lower_message = message:lower()
    if lower_message ~= "!holo"
        and lower_message:match("^!holo%s") == nil
    then
        return nil
    end
    local arguments = Hologram.trim(message:sub(6))
    if arguments == "" then
        return { action = "help" }
    end
    local action, rest = arguments:match("^(%S+)%s*(.-)$")
    action = tostring(action or ""):lower()
    rest = Hologram.trim(rest)
    if action == "help" or action == "list" then
        if rest ~= "" then
            return nil, "invalid_syntax"
        end
        return { action = action }
    end
    if action == "move" or action == "remove" then
        local id = Hologram.sanitize_id(rest)
        if id == nil then
            return nil, "invalid_id"
        end
        return { action = action, id = id }
    end
    if action == "set" then
        local raw_id, raw_text = rest:match("^(%S+)%s+(.+)$")
        local id = Hologram.sanitize_id(raw_id)
        if id == nil then
            return nil, "invalid_id"
        end
        local text = Hologram.sanitize_text(raw_text)
        if text == nil then
            return nil, "invalid_text"
        end
        return {
            action = action,
            id = id,
            text = text,
        }
    end
    return nil, "invalid_syntax"
end

function Hologram.sorted_ids(holograms)
    local ids = {}
    for id in pairs(type(holograms) == "table" and holograms or {}) do
        table.insert(ids, id)
    end
    table.sort(ids)
    return ids
end

function Hologram.normalized(record)
    if type(record) ~= "table" then
        return nil
    end
    local id = Hologram.sanitize_id(record.id)
    local x = tonumber(record.x)
    local y = tonumber(record.y)
    local z = tonumber(record.z)
    local max_distance = tonumber(record.max_distance)
    local text = tostring(record.text or "")
    local canonical_text = Hologram.sanitize_text(
        text:gsub("\n", "|")
    )
    if id == nil
        or x == nil
        or y == nil
        or z == nil
        or math.abs(x) > 100000000
        or math.abs(y) > 100000000
        or math.abs(z) > 100000000
        or max_distance == nil
        or max_distance < 100
        or max_distance > 100000
        or text == ""
        or canonical_text ~= text
        or text:find("\t", 1, true) ~= nil
    then
        return nil
    end
    return {
        id = id,
        x = x,
        y = y,
        z = z,
        max_distance = math.floor(max_distance),
        text = text,
    }
end

function Hologram.serialize_store(holograms)
    local lines = {}
    for _, id in ipairs(Hologram.sorted_ids(holograms)) do
        local record = Hologram.normalized(holograms[id])
        if record ~= nil then
            table.insert(
                lines,
                string.format(
                    "%s\t%.6f\t%.6f\t%.6f\t%d\t%s",
                    record.id,
                    record.x,
                    record.y,
                    record.z,
                    record.max_distance,
                    Hologram.percent_encode(record.text)
                )
            )
        end
    end
    return #lines > 0 and (table.concat(lines, "\n") .. "\n") or ""
end

function Hologram.parse_store(document)
    local holograms = {}
    local count = 0
    for line in tostring(document or ""):gmatch("[^\r\n]+") do
        local id, x, y, z, max_distance, encoded_text = line:match(
            "^([^\t]+)\t([^\t]+)\t([^\t]+)\t([^\t]+)\t([^\t]+)\t(.*)$"
        )
        local record = Hologram.normalized({
            id = id,
            x = x,
            y = y,
            z = z,
            max_distance = max_distance,
            text = Hologram.percent_decode(encoded_text),
        })
        if record == nil or holograms[record.id] ~= nil then
            return nil, "invalid_record"
        end
        count = count + 1
        if count > HOLOGRAM_MAX_COUNT then
            return nil, "too_many_records"
        end
        holograms[record.id] = record
    end
    return holograms, nil
end

function Hologram.serialize_protocol(revision, holograms)
    local records = {}
    for _, id in ipairs(Hologram.sorted_ids(holograms)) do
        local record = Hologram.normalized(holograms[id])
        if record ~= nil then
            table.insert(
                records,
                string.format(
                    "%s,%.6f,%.6f,%.6f,%d,%s",
                    record.id,
                    record.x,
                    record.y,
                    record.z,
                    record.max_distance,
                    Hologram.percent_encode(record.text)
                )
            )
        end
    end
    return HOLOGRAM_PROTOCOL_PREFIX
        .. tostring(math.max(0, math.floor(tonumber(revision) or 0)))
        .. "|"
        .. table.concat(records, "~")
end

function Hologram.parse_protocol(raw_message)
    local revision_text, payload = tostring(raw_message or ""):match(
        "^%[PALHOLO%]|(%d+)|(.*)$"
    )
    if revision_text == nil then
        return nil
    end
    local holograms = {}
    local count = 0
    if payload ~= "" then
        for encoded_record in (payload .. "~"):gmatch("(.-)~") do
            local id, x, y, z, max_distance, encoded_text =
                encoded_record:match(
                    "^([^,]+),([^,]+),([^,]+),([^,]+),([^,]+),(.*)$"
                )
            local record = Hologram.normalized({
                id = id,
                x = x,
                y = y,
                z = z,
                max_distance = max_distance,
                text = Hologram.percent_decode(encoded_text),
            })
            if record == nil or holograms[record.id] ~= nil then
                return nil
            end
            count = count + 1
            if count > HOLOGRAM_MAX_COUNT then
                return nil
            end
            holograms[record.id] = record
        end
    end
    return tonumber(revision_text), holograms
end

function Hologram.is_admin(controller)
    return is_valid(controller)
        and unwrap(read_member(controller, "bAdmin")) == true
end

function Hologram.controller_location(controller)
    local pawn_ok, pawn = call_method(controller, "K2_GetPawn")
    if not pawn_ok or not is_valid(pawn) then
        return nil
    end
    local location_ok, location = call_method(
        pawn,
        "K2_GetActorLocation"
    )
    if not location_ok
        or location == nil
        or tonumber(read_member(location, "X")) == nil
        or tonumber(read_member(location, "Y")) == nil
        or tonumber(read_member(location, "Z")) == nil
    then
        return nil
    end
    return {
        X = tonumber(read_member(location, "X")),
        Y = tonumber(read_member(location, "Y")),
        Z = tonumber(read_member(location, "Z")),
    }
end

function Hologram.project_world_location(controller, location)
    if not is_valid(controller) or type(location) ~= "table" then
        return nil
    end
    local screen = { X = 0, Y = 0 }
    if FVector2D ~= nil then
        local vector_ok, vector = pcall(FVector2D, 0, 0)
        if vector_ok and vector ~= nil then
            screen = vector
        end
    end
    local world_location = location
    if FVector ~= nil then
        local vector_ok, vector = pcall(
            FVector,
            tonumber(location.X) or 0,
            tonumber(location.Y) or 0,
            tonumber(location.Z) or 0
        )
        if vector_ok and vector ~= nil then
            world_location = vector
        end
    end
    local projected_ok, projected = call_method(
        controller,
        "ProjectWorldLocationToScreen",
        world_location,
        screen,
        true
    )
    if not projected_ok or projected ~= true then
        return nil
    end
    local x = tonumber(read_member(screen, "X"))
    local y = tonumber(read_member(screen, "Y"))
    if x == nil or y == nil then
        return nil
    end
    return { X = x, Y = y }
end

local function parse_combat_protocol(raw_message, now)
    local active_flag, remaining, duration = tostring(raw_message or ""):match(
        "^%[PALCOMBAT%]|([01])|(%d+)|(%d+)$"
    )
    if active_flag == nil then
        return nil
    end
    remaining = math.floor(tonumber(remaining) or 0)
    duration = math.floor(tonumber(duration) or 0)
    if duration < 1 or duration > 3600
        or remaining < 0 or remaining > duration
    then
        return nil
    end
    return {
        active = active_flag == "1" and remaining > 0,
        remaining = remaining,
        duration = duration,
        received_at = tonumber(now) or os.time(),
    }
end

local function combat_view(state, now)
    if type(state) ~= "table" or state.active ~= true then
        return {
            active = false,
            text = "",
            progress = 0,
        }
    end
    local elapsed = math.max(
        0,
        math.floor((tonumber(now) or os.time())
            - (tonumber(state.received_at) or 0))
    )
    local remaining = math.max(
        0,
        math.floor(tonumber(state.remaining) or 0) - elapsed
    )
    local duration = math.max(1, tonumber(state.duration) or 10)
    if remaining <= 0 then
        return {
            active = false,
            text = "",
            progress = 0,
        }
    end
    return {
        active = true,
        text = string.format(
            "CHIẾN ĐẤU • KHÔNG THOÁT GAME • %ds",
            remaining
        ),
        progress = math.max(0, math.min(1, remaining / duration)),
    }
end

local function parse_gacha_protocol(raw_message, now)
    local remaining, reward_spins, interval_seconds =
        tostring(raw_message or ""):match(
            "^%[PALHUDGACHA%]|(%-?%d+)|(%d+)|(%d+)$"
    )
    if remaining == nil then
        return nil
    end
    remaining = math.floor(tonumber(remaining) or 0)
    reward_spins = math.floor(tonumber(reward_spins) or 0)
    interval_seconds = math.floor(tonumber(interval_seconds) or 0)
    if remaining < -1
        or remaining > 604800
        or reward_spins < 0
        or reward_spins > 1000
        or interval_seconds < 0
        or interval_seconds > 604800
        or (remaining < 0) ~= (reward_spins == 0)
        or (remaining < 0) ~= (interval_seconds == 0)
        or remaining > interval_seconds
    then
        return nil
    end
    return {
        active = remaining >= 0
            and reward_spins > 0
            and interval_seconds > 0,
        remaining = remaining,
        reward_spins = reward_spins,
        interval_seconds = interval_seconds,
        received_at = tonumber(now) or os.time(),
        balance = nil,
    }
end

local function safe_protocol_text(value)
    return safe_display_text(value):gsub("|", "/")
end

local format_multiplier
local safe_source_label

local function find_local_player_controller()
    local compass_widget = nil
    pcall(function()
        compass_widget = FindFirstOf("WBP_Ingame_Compass_C")
    end)
    if is_valid(compass_widget) then
        local owner_ok, owner = call_method(
            compass_widget,
            "GetOwningPlayer"
        )
        if owner_ok and is_valid(owner) then
            local local_ok, is_local = call_method(
                owner,
                "IsLocalPlayerController"
            )
            if local_ok and is_local == true then
                return owner
            end
        end
    end

    local class_names = {
        "PalPlayerController",
        "PlayerController",
        "Controller",
    }
    for _, class_name in ipairs(class_names) do
        local player_controllers = nil
        pcall(function()
            player_controllers = FindAllOf(class_name)
        end)
        if type(player_controllers) == "table" then
            for _, raw_controller in ipairs(player_controllers) do
                local controller = raw_controller
                local local_ok, is_local = call_method(
                    controller,
                    "IsLocalPlayerController"
                )
                if local_ok and is_local == true then
                    return controller
                end
            end
        end
    end
    return nil
end

local function make_linear_color(red, green, blue, alpha)
    local ok, color = pcall(FLinearColor, red, green, blue, alpha)
    if ok then
        return color
    end
    return {
        R = red,
        G = green,
        B = blue,
        A = alpha,
    }
end

local function make_slate_color(color)
    local ok, slate_color = pcall(FSlateColor, color)
    if ok then
        return slate_color
    end
    return { SpecifiedColor = color }
end

local function construct_widget(class_object, outer, name)
    local ok, widget = pcall(
        StaticConstructObject,
        class_object,
        outer,
        new_object_name(name)
    )
    if not ok or not is_valid(widget) then
        return nil
    end
    return widget
end

local function configure_text_block(text_block, size, color)
    local font = read_member(text_block, "Font")
    local font_ok = font ~= nil and pcall(function()
        font.Size = size
    end)
    if not font_ok then
        return false
    end
    call_method(
        text_block,
        "SetColorAndOpacity",
        make_slate_color(color)
    )
    call_method(
        text_block,
        "SetShadowColorAndOpacity",
        make_linear_color(0, 0, 0, 0.85)
    )
    call_method(text_block, "SetShadowOffset", { X = 1.0, Y = 1.0 })
    return true
end

local function ensure_hud_widget()
    if is_valid(hud_overlay)
        and is_valid(hud_player_block)
        and is_valid(hud_gacha_block)
        and is_valid(hud_gacha_time_block)
        and is_valid(hud_gacha_progress_box)
        and is_valid(hud_gacha_progress_bar)
        and is_valid(hud_combat_block)
        and is_valid(hud_combat_progress_box)
        and is_valid(hud_combat_progress_bar)
        and is_valid(hud_booster_block)
        and is_valid(hud_time_block)
        and is_valid(hud_progress_box)
        and is_valid(hud_progress_bar)
    then
        return true
    end

    hud_overlay = nil
    hud_player_block = nil
    hud_gacha_block = nil
    hud_gacha_time_block = nil
    hud_gacha_progress_box = nil
    hud_gacha_progress_bar = nil
    hud_combat_block = nil
    hud_combat_progress_box = nil
    hud_combat_progress_bar = nil
    hud_booster_block = nil
    hud_time_block = nil
    hud_progress_box = nil
    hud_progress_bar = nil

    local local_controller = find_local_player_controller()
    if not is_valid(local_controller) then
        return false, "local_player_controller_unavailable"
    end

    if not is_valid(widget_blueprint_library) then
        widget_blueprint_library = find_required_object(
            WIDGET_BLUEPRINT_LIBRARY_PATH
        )
    end
    if not is_valid(rendering_library) then
        rendering_library = find_required_object(RENDERING_LIBRARY_PATH)
    end
    if not is_valid(user_widget_class) then
        user_widget_class = find_required_object(USER_WIDGET_CLASS_PATH)
    end
    if not is_valid(widget_tree_class) then
        widget_tree_class = find_required_object(WIDGET_TREE_CLASS_PATH)
    end
    if not is_valid(text_block_class) then
        text_block_class = find_required_object(TEXT_BLOCK_CLASS_PATH)
    end
    if not is_valid(size_box_class) then
        size_box_class = find_required_object(SIZE_BOX_CLASS_PATH)
    end
    if not is_valid(border_class) then
        border_class = find_required_object(BORDER_CLASS_PATH)
    end
    if not is_valid(vertical_box_class) then
        vertical_box_class = find_required_object(VERTICAL_BOX_CLASS_PATH)
    end
    if not is_valid(progress_bar_class) then
        progress_bar_class = find_required_object(PROGRESS_BAR_CLASS_PATH)
    end
    if not is_valid(widget_blueprint_library)
        or not is_valid(rendering_library)
        or not is_valid(user_widget_class)
        or not is_valid(widget_tree_class)
        or not is_valid(text_block_class)
        or not is_valid(size_box_class)
        or not is_valid(border_class)
        or not is_valid(vertical_box_class)
        or not is_valid(progress_bar_class)
    then
        return false, "umg_class_unavailable"
    end

    local overlay_ok, overlay = call_method(
        widget_blueprint_library,
        "Create",
        local_controller,
        user_widget_class,
        local_controller
    )
    if not overlay_ok or not is_valid(overlay) then
        return false, "overlay_construct_failed"
    end

    local widget_tree = construct_widget(
        widget_tree_class,
        overlay,
        HUD_WIDGET_TREE_NAME
    )
    if widget_tree == nil then
        return false, "widget_tree_construct_failed"
    end
    local tree_assign_ok = pcall(function()
        overlay.WidgetTree = widget_tree
    end)
    if not tree_assign_ok then
        return false, "widget_tree_assign_failed"
    end

    local card_root = construct_widget(
        size_box_class,
        widget_tree,
        HUD_CARD_ROOT_NAME
    )
    local frame_border = card_root ~= nil and construct_widget(
        border_class,
        card_root,
        HUD_FRAME_BORDER_NAME
    ) or nil
    local panel_border = frame_border ~= nil and construct_widget(
        border_class,
        frame_border,
        HUD_PANEL_BORDER_NAME
    ) or nil
    local content = panel_border ~= nil and construct_widget(
        vertical_box_class,
        panel_border,
        HUD_CONTENT_NAME
    ) or nil
    local player_block = content ~= nil and construct_widget(
        text_block_class,
        content,
        HUD_PLAYER_NAME
    ) or nil
    local gacha_block = content ~= nil and construct_widget(
        text_block_class,
        content,
        HUD_GACHA_NAME
    ) or nil
    local gacha_time_block = content ~= nil and construct_widget(
        text_block_class,
        content,
        HUD_GACHA_TIME_NAME
    ) or nil
    local gacha_progress_height_box =
        content ~= nil and construct_widget(
            size_box_class,
            content,
            HUD_GACHA_PROGRESS_HEIGHT_NAME
        ) or nil
    local gacha_progress_bar =
        gacha_progress_height_box ~= nil and construct_widget(
            progress_bar_class,
            gacha_progress_height_box,
            HUD_GACHA_PROGRESS_NAME
        ) or nil
    local combat_block = content ~= nil and construct_widget(
        text_block_class,
        content,
        HUD_COMBAT_NAME
    ) or nil
    local combat_progress_height_box =
        content ~= nil and construct_widget(
            size_box_class,
            content,
            HUD_COMBAT_PROGRESS_HEIGHT_NAME
        ) or nil
    local combat_progress_bar =
        combat_progress_height_box ~= nil and construct_widget(
            progress_bar_class,
            combat_progress_height_box,
            HUD_COMBAT_PROGRESS_NAME
        ) or nil
    local booster_block = content ~= nil and construct_widget(
        text_block_class,
        content,
        HUD_BOOSTER_NAME
    ) or nil
    local time_block = content ~= nil and construct_widget(
        text_block_class,
        content,
        HUD_TIME_NAME
    ) or nil
    local progress_height_box = content ~= nil and construct_widget(
        size_box_class,
        content,
        HUD_PROGRESS_HEIGHT_NAME
    ) or nil
    local progress_bar = progress_height_box ~= nil and construct_widget(
        progress_bar_class,
        progress_height_box,
        HUD_PROGRESS_NAME
    ) or nil
    if card_root == nil
        or frame_border == nil
        or panel_border == nil
        or content == nil
        or player_block == nil
        or gacha_block == nil
        or gacha_time_block == nil
        or gacha_progress_height_box == nil
        or gacha_progress_bar == nil
        or combat_block == nil
        or combat_progress_height_box == nil
        or combat_progress_bar == nil
        or booster_block == nil
        or time_block == nil
        or progress_height_box == nil
        or progress_bar == nil
    then
        return false, "hud_card_construct_failed"
    end

    local root_assign_ok = pcall(function()
        widget_tree.RootWidget = card_root
    end)
    if not root_assign_ok then
        return false, "root_widget_assign_failed"
    end

    if not is_valid(background_logo_texture) then
        local texture_ok, texture = call_method(
            rendering_library,
            "ImportFileAsTexture2D",
            local_controller,
            LOGO_PATH
        )
        if texture_ok and is_valid(texture) then
            background_logo_texture = texture
        else
            log("INFO", "HUD logo watermark unavailable: " .. LOGO_PATH)
        end
    end

    local player_color = make_linear_color(0.89, 0.93, 0.98, 1.0)
    local gacha_color = make_linear_color(1.0, 0.78, 0.28, 1.0)
    local combat_color = make_linear_color(1.0, 0.32, 0.24, 1.0)
    local booster_color = make_linear_color(0.72, 0.62, 1.0, 1.0)
    local time_color = make_linear_color(0.48, 0.88, 0.94, 1.0)
    if not configure_text_block(player_block, 15, player_color)
        or not configure_text_block(gacha_block, 14, gacha_color)
        or not configure_text_block(gacha_time_block, 12, gacha_color)
        or not configure_text_block(combat_block, 14, combat_color)
        or not configure_text_block(booster_block, 13, booster_color)
        or not configure_text_block(time_block, 12, time_color)
    then
        return false, "text_font_configuration_failed"
    end

    if not call_method(card_root, "SetWidthOverride", 460.0)
        or not call_method(
            frame_border,
            "SetBrushColor",
            make_linear_color(0.49, 0.23, 0.93, 0.94)
        )
        or not call_method(
            frame_border,
            "SetPadding",
            { Left = 2.0, Top = 2.0, Right = 2.0, Bottom = 2.0 }
        )
        or not call_method(
            panel_border,
            "SetBrushColor",
            make_linear_color(1.0, 1.0, 1.0, 0.35)
        )
        or not call_method(
            panel_border,
            "SetPadding",
            { Left = 14.0, Top = 10.0, Right = 14.0, Bottom = 12.0 }
        )
        or not call_method(progress_height_box, "SetHeightOverride", 7.0)
        or not call_method(
            gacha_progress_height_box,
            "SetHeightOverride",
            7.0
        )
        or not call_method(
            combat_progress_height_box,
            "SetHeightOverride",
            8.0
        )
        or not call_method(
            combat_progress_bar,
            "SetFillColorAndOpacity",
            make_linear_color(0.95, 0.12, 0.06, 1.0)
        )
        or not call_method(
            progress_bar,
            "SetFillColorAndOpacity",
            make_linear_color(0.12, 0.86, 0.94, 1.0)
        )
        or not call_method(
            gacha_progress_bar,
            "SetFillColorAndOpacity",
            make_linear_color(1.0, 0.62, 0.10, 1.0)
        )
        or not call_method(
            combat_progress_height_box,
            "SetContent",
            combat_progress_bar
        )
        or not call_method(progress_height_box, "SetContent", progress_bar)
        or not call_method(
            gacha_progress_height_box,
            "SetContent",
            gacha_progress_bar
        )
        or not call_method(content, "AddChildToVerticalBox", player_block)
        or not call_method(content, "AddChildToVerticalBox", gacha_block)
        or not call_method(content, "AddChildToVerticalBox", gacha_time_block)
        or not call_method(
            content,
            "AddChildToVerticalBox",
            gacha_progress_height_box
        )
        or not call_method(content, "AddChildToVerticalBox", combat_block)
        or not call_method(
            content,
            "AddChildToVerticalBox",
            combat_progress_height_box
        )
        or not call_method(content, "AddChildToVerticalBox", booster_block)
        or not call_method(content, "AddChildToVerticalBox", time_block)
        or not call_method(
            content,
            "AddChildToVerticalBox",
            progress_height_box
        )
        or not call_method(panel_border, "SetContent", content)
        or not call_method(frame_border, "SetContent", panel_border)
        or not call_method(card_root, "SetContent", frame_border)
    then
        return false, "hud_card_configuration_failed"
    end

    if is_valid(background_logo_texture)
        and not call_method(
            panel_border,
            "SetBrushFromTexture",
            background_logo_texture
        )
    then
        return false, "hud_logo_configuration_failed"
    end

    local add_ok = call_method(overlay, "AddToViewport", 100)
    local position_ok = call_method(
        overlay,
        "SetPositionInViewport",
        { X = 8.0, Y = 8.0 },
        false
    )
    if not add_ok then
        return false, "viewport_add_failed"
    end
    if not position_ok then
        return false, "viewport_position_failed"
    end
    local visibility_ok = call_method(
        overlay,
        "SetVisibility",
        hud_visible and 3 or 1
    )
    if not visibility_ok then
        return false, "viewport_visibility_failed"
    end

    hud_overlay = overlay
    hud_player_block = player_block
    hud_gacha_block = gacha_block
    hud_gacha_time_block = gacha_time_block
    hud_gacha_progress_box = gacha_progress_height_box
    hud_gacha_progress_bar = gacha_progress_bar
    hud_combat_block = combat_block
    hud_combat_progress_box = combat_progress_height_box
    hud_combat_progress_bar = combat_progress_bar
    hud_booster_block = booster_block
    hud_time_block = time_block
    hud_progress_box = progress_height_box
    hud_progress_bar = progress_bar
    log("INFO", "Decorated PalBooster HUD card added to player viewport.")
    return true, nil
end

local function remove_hologram_widget(record)
    if type(record) == "table" and is_valid(record.border) then
        call_method(record.border, "RemoveFromParent")
    end
    if type(record) == "table" then
        record.border = nil
        record.text_block = nil
        record.slot = nil
    end
end

local function clear_client_holograms()
    for _, record in pairs(client_holograms) do
        remove_hologram_widget(record)
    end
    client_holograms = {}
    client_hologram_revision = -1
    if is_valid(hologram_overlay) then
        call_method(hologram_overlay, "RemoveFromParent")
    end
    hologram_overlay = nil
    hologram_canvas = nil
    hologram_notice_border = nil
    hologram_notice_text = nil
    hologram_notice_hide_at = 0
end

local function ensure_hologram_layer()
    if is_valid(hologram_overlay) and is_valid(hologram_canvas) then
        return true
    end
    for _, record in pairs(client_holograms) do
        remove_hologram_widget(record)
    end
    if is_valid(hologram_overlay) then
        call_method(hologram_overlay, "RemoveFromParent")
    end
    hologram_overlay = nil
    hologram_canvas = nil
    hologram_notice_border = nil
    hologram_notice_text = nil
    hologram_notice_hide_at = 0

    local local_controller = find_local_player_controller()
    if not is_valid(local_controller) then
        return false, "local_player_controller_unavailable"
    end
    if not is_valid(widget_blueprint_library) then
        widget_blueprint_library = find_required_object(
            WIDGET_BLUEPRINT_LIBRARY_PATH
        )
    end
    if not is_valid(user_widget_class) then
        user_widget_class = find_required_object(USER_WIDGET_CLASS_PATH)
    end
    if not is_valid(widget_tree_class) then
        widget_tree_class = find_required_object(WIDGET_TREE_CLASS_PATH)
    end
    if not is_valid(text_block_class) then
        text_block_class = find_required_object(TEXT_BLOCK_CLASS_PATH)
    end
    if not is_valid(border_class) then
        border_class = find_required_object(BORDER_CLASS_PATH)
    end
    if not is_valid(canvas_panel_class) then
        canvas_panel_class = find_required_object(CANVAS_PANEL_CLASS_PATH)
    end
    if not is_valid(widget_blueprint_library)
        or not is_valid(user_widget_class)
        or not is_valid(widget_tree_class)
        or not is_valid(text_block_class)
        or not is_valid(border_class)
        or not is_valid(canvas_panel_class)
    then
        return false, "hologram_umg_class_unavailable"
    end

    local overlay_ok, overlay = call_method(
        widget_blueprint_library,
        "Create",
        local_controller,
        user_widget_class,
        local_controller
    )
    if not overlay_ok or not is_valid(overlay) then
        return false, "hologram_overlay_construct_failed"
    end
    local widget_tree = construct_widget(
        widget_tree_class,
        overlay,
        HOLOGRAM_WIDGET_TREE_NAME
    )
    local canvas = widget_tree ~= nil and construct_widget(
        canvas_panel_class,
        widget_tree,
        HOLOGRAM_CANVAS_NAME
    ) or nil
    if widget_tree == nil or canvas == nil then
        return false, "hologram_canvas_construct_failed"
    end
    local assigned = pcall(function()
        overlay.WidgetTree = widget_tree
        widget_tree.RootWidget = canvas
    end)
    if not assigned
        or not call_method(overlay, "AddToViewport", 90)
        or not call_method(
            overlay,
            "SetVisibility",
            hud_visible and 3 or 1
        )
    then
        return false, "hologram_canvas_configuration_failed"
    end
    hologram_overlay = overlay
    hologram_canvas = canvas
    return true, nil
end

local function ensure_hologram_widget(record)
    if type(record) ~= "table" then
        return false
    end
    if is_valid(record.border)
        and is_valid(record.text_block)
        and is_valid(record.slot)
    then
        return true
    end
    local layer_ready = ensure_hologram_layer()
    if not layer_ready then
        return false
    end
    local border = construct_widget(
        border_class,
        hologram_canvas,
        "PalHud_HoloBorder_" .. record.id
    )
    local text_block = border ~= nil and construct_widget(
        text_block_class,
        border,
        "PalHud_HoloText_" .. record.id
    ) or nil
    if border == nil or text_block == nil then
        return false
    end
    local text_ok, display_text = pcall(FText, record.text)
    if not text_ok
        or not configure_text_block(
            text_block,
            16,
            make_linear_color(0.52, 0.96, 1.0, 1.0)
        )
        or not call_method(text_block, "SetText", display_text)
        or not call_method(
            border,
            "SetBrushColor",
            make_linear_color(0.02, 0.07, 0.12, 0.78)
        )
        or not call_method(
            border,
            "SetPadding",
            { Left = 10.0, Top = 6.0, Right = 10.0, Bottom = 6.0 }
        )
        or not call_method(border, "SetContent", text_block)
    then
        return false
    end
    local slot_ok, slot = call_method(
        hologram_canvas,
        "AddChildToCanvas",
        border
    )
    if not slot_ok or not is_valid(slot) then
        return false
    end
    call_method(slot, "SetAutoSize", true)
    call_method(slot, "SetAlignment", { X = 0.5, Y = 0.5 })
    call_method(border, "SetVisibility", 1)
    record.border = border
    record.text_block = text_block
    record.slot = slot
    return true
end

local function render_hologram_notice(message)
    local layer_ready = ensure_hologram_layer()
    if not layer_ready then
        return false
    end
    if not is_valid(hologram_notice_border)
        or not is_valid(hologram_notice_text)
    then
        local border = construct_widget(
            border_class,
            hologram_canvas,
            HOLOGRAM_NOTICE_NAME
        )
        local text_block = border ~= nil and construct_widget(
            text_block_class,
            border,
            HOLOGRAM_NOTICE_NAME .. "Text"
        ) or nil
        if border == nil
            or text_block == nil
            or not configure_text_block(
                text_block,
                16,
                make_linear_color(1.0, 0.82, 0.28, 1.0)
            )
            or not call_method(
                border,
                "SetBrushColor",
                make_linear_color(0.02, 0.04, 0.08, 0.92)
            )
            or not call_method(
                border,
                "SetPadding",
                { Left = 14.0, Top = 8.0, Right = 14.0, Bottom = 8.0 }
            )
            or not call_method(border, "SetContent", text_block)
        then
            return false
        end
        local slot_ok, slot = call_method(
            hologram_canvas,
            "AddChildToCanvas",
            border
        )
        if not slot_ok or not is_valid(slot) then
            return false
        end
        call_method(slot, "SetAutoSize", true)
        call_method(slot, "SetAlignment", { X = 0.5, Y = 0.0 })
        call_method(
            slot,
            "SetAnchors",
            {
                Minimum = { X = 0.5, Y = 0.0 },
                Maximum = { X = 0.5, Y = 0.0 },
            }
        )
        call_method(slot, "SetPosition", { X = 0.0, Y = 72.0 })
        hologram_notice_border = border
        hologram_notice_text = text_block
    end
    local text_ok, display_text = pcall(
        FText,
        safe_display_text(message)
    )
    if not text_ok
        or not call_method(hologram_notice_text, "SetText", display_text)
        or not call_method(hologram_notice_border, "SetVisibility", 3)
    then
        return false
    end
    hologram_notice_hide_at = os.time() + 6
    return true
end

local function update_client_hologram_widgets(controller)
    if not is_valid(controller) then
        return
    end
    local player_location = Hologram.controller_location(controller)
    for _, record in pairs(client_holograms) do
        local visible = false
        if player_location ~= nil and ensure_hologram_widget(record) then
            local dx = record.x - player_location.X
            local dy = record.y - player_location.Y
            local dz = record.z - player_location.Z
            local distance = math.sqrt(dx * dx + dy * dy + dz * dz)
            if distance <= record.max_distance then
                local screen = Hologram.project_world_location(
                    controller,
                    { X = record.x, Y = record.y, Z = record.z }
                )
                if screen ~= nil then
                    local scale = math.max(
                        0.72,
                        math.min(
                            1.08,
                            1.08 - (distance / record.max_distance) * 0.36
                        )
                    )
                    call_method(record.slot, "SetPosition", screen)
                    call_method(
                        record.border,
                        "SetRenderScale",
                        { X = scale, Y = scale }
                    )
                    call_method(record.border, "SetVisibility", 3)
                    visible = true
                end
            end
        end
        if not visible and is_valid(record.border) then
            call_method(record.border, "SetVisibility", 1)
        end
    end
    if hologram_notice_hide_at > 0
        and os.time() >= hologram_notice_hide_at
        and is_valid(hologram_notice_border)
    then
        call_method(hologram_notice_border, "SetVisibility", 1)
        hologram_notice_hide_at = 0
    end
end

Callbacks.game_thread_hologram_tick = function()
    hologram_tick_pending = false
    local controller = find_local_player_controller()
    if controller ~= nil then
        update_client_hologram_widgets(controller)
    end
end

Callbacks.async_hologram_tick = function()
    if next(client_holograms) == nil and hologram_notice_hide_at <= 0 then
        return false
    end
    if hologram_tick_pending then
        return false
    end
    hologram_tick_pending = true
    local queued, queue_error = pcall(
        ExecuteInGameThread,
        Callbacks.game_thread_hologram_tick
    )
    if not queued then
        hologram_tick_pending = false
        log(
            "ERROR",
            "Hologram game-thread queue failed: " .. tostring(queue_error)
        )
    end
    return false
end

local function start_hologram_loop()
    if hologram_loop_started then
        return
    end
    hologram_loop_started = true
    LoopAsync(HOLOGRAM_TICK_MS, Callbacks.async_hologram_tick)
end

local function apply_hologram_sync(revision, holograms)
    if revision == client_hologram_revision then
        return true
    end
    local next_records = {}
    for id, record in pairs(holograms) do
        local previous = client_holograms[id]
        if type(previous) == "table" then
            record.border = previous.border
            record.text_block = previous.text_block
            record.slot = previous.slot
        end
        next_records[id] = record
    end
    for id, previous in pairs(client_holograms) do
        if next_records[id] == nil then
            remove_hologram_widget(previous)
        end
    end
    client_holograms = next_records
    client_hologram_revision = revision
    if next(client_holograms) ~= nil then
        if not ensure_hologram_layer() then
            return false
        end
        for _, record in pairs(client_holograms) do
            if ensure_hologram_widget(record) then
                local text_ok, display_text = pcall(FText, record.text)
                if text_ok then
                    call_method(record.text_block, "SetText", display_text)
                end
            end
        end
        start_hologram_loop()
    end
    return true
end

local function render_hologram_protocol(raw_message)
    local revision, holograms = Hologram.parse_protocol(raw_message)
    if revision == nil then
        return false
    end
    return apply_hologram_sync(revision, holograms)
end

local function render_hologram_notice_protocol(raw_message)
    local encoded = tostring(raw_message or ""):match(
        "^%[PALHOLO%-NOTICE%]|(.*)$"
    )
    if encoded == nil then
        return false
    end
    start_hologram_loop()
    return render_hologram_notice(Hologram.percent_decode(encoded))
end

local function render_client_hud(view)
    local ready, failure_reason = ensure_hud_widget()
    if not ready then
        if failure_reason ~= last_render_failure_reason then
            last_render_failure_reason = failure_reason
            log("INFO", "UMG HUD waiting: " .. tostring(failure_reason))
        end
        return false
    end
    local player_ok, player_text = pcall(
        FText,
        safe_display_text(view.player)
    )
    local booster_ok, booster_text = pcall(
        FText,
        safe_display_text(view.booster)
    )
    local time_ok, time_text = pcall(
        FText,
        safe_display_text(view.time)
    )
    local gacha_ok, gacha_text = pcall(
        FText,
        safe_display_text(view.gacha)
    )
    local gacha_time_ok, gacha_time_text = pcall(
        FText,
        safe_display_text(view.gacha_time)
    )
    local combat = combat_view(combat_state, os.time())
    local combat_ok, combat_text = pcall(
        FText,
        safe_display_text(combat.text)
    )
    local progress = math.max(0, math.min(1, tonumber(view.progress) or 0))
    local gacha_progress = math.max(
        0,
        math.min(1, tonumber(view.gacha_progress) or 0)
    )
    local booster_visibility = view.boosted and 3 or 1
    local gacha_visibility = view.gacha_active and 3 or 1
    local combat_visibility = combat.active and 3 or 1
    local ok = player_ok
        and gacha_ok
        and gacha_time_ok
        and combat_ok
        and booster_ok
        and time_ok
        and call_method(hud_player_block, "SetText", player_text)
        and call_method(hud_gacha_block, "SetText", gacha_text)
        and call_method(
            hud_gacha_time_block,
            "SetText",
            gacha_time_text
        )
        and call_method(
            hud_gacha_time_block,
            "SetVisibility",
            gacha_visibility
        )
        and call_method(
            hud_gacha_progress_box,
            "SetVisibility",
            gacha_visibility
        )
        and call_method(
            hud_gacha_progress_bar,
            "SetPercent",
            gacha_progress
        )
        and call_method(hud_combat_block, "SetText", combat_text)
        and call_method(hud_booster_block, "SetText", booster_text)
        and call_method(hud_time_block, "SetText", time_text)
        and call_method(
            hud_combat_block,
            "SetVisibility",
            combat_visibility
        )
        and call_method(
            hud_combat_progress_box,
            "SetVisibility",
            combat_visibility
        )
        and call_method(
            hud_combat_progress_bar,
            "SetPercent",
            combat.progress
        )
        and call_method(
            hud_booster_block,
            "SetVisibility",
            booster_visibility
        )
        and call_method(
            hud_time_block,
            "SetVisibility",
            booster_visibility
        )
        and call_method(
            hud_progress_box,
            "SetVisibility",
            booster_visibility
        )
        and call_method(hud_progress_bar, "SetPercent", progress)
        and apply_hud_visibility()
    if not ok and not render_failure_logged then
        render_failure_logged = true
        log("ERROR", "UMG booster card update failed.")
    elseif ok then
        render_failure_logged = false
        last_render_failure_reason = nil
    end
    return ok
end

local function format_remaining(seconds)
    local remaining = math.max(0, math.floor(tonumber(seconds) or 0))
    if remaining >= 86400 then
        local days = math.floor(remaining / 86400)
        local hours = math.floor((remaining % 86400) / 3600)
        return string.format("%d ngày %d giờ", days, hours)
    end
    if remaining >= 3600 then
        local hours = math.floor(remaining / 3600)
        local minutes = math.floor((remaining % 3600) / 60)
        return string.format("%d giờ %d phút", hours, minutes)
    end
    return string.format(
        "%d phút %d giây",
        math.floor(remaining / 60),
        remaining % 60
    )
end

local function gacha_view(value)
    local spins = tonumber(value)
    if spins == nil or spins < 0 then
        gacha_reward_state.balance = nil
        return {
            text = "GACHA • ĐANG ĐỒNG BỘ",
            time = "",
            active = false,
            progress = 0,
        }
    end
    spins = math.max(0, math.floor(spins))
    gacha_reward_state.balance = spins
    if gacha_reward_state.active then
        local elapsed = math.max(
            0,
            math.floor(
                os.time() - (gacha_reward_state.received_at or 0)
            )
        )
        local remaining = math.max(
            0,
            math.floor(gacha_reward_state.remaining or 0) - elapsed
        )
        local interval_seconds = math.max(
            1,
            math.floor(gacha_reward_state.interval_seconds or 0)
        )
        local progress = math.max(
            0,
            math.min(1, 1 - (remaining / interval_seconds))
        )
        if remaining > 0 then
            return {
                text = string.format("GACHA • %d", spins),
                time = string.format(
                    "Lượt mới • +%d sau %s",
                    gacha_reward_state.reward_spins,
                    format_remaining(remaining)
                ),
                active = true,
                progress = progress,
            }
        end
        return {
            text = string.format("GACHA • %d", spins),
            time = string.format(
                "Lượt mới • ĐANG NHẬN +%d",
                gacha_reward_state.reward_spins
            ),
            active = true,
            progress = 1,
        }
    end
    return {
        text = string.format("GACHA • %d", spins),
        time = "",
        active = false,
        progress = 0,
    }
end

local function player_text(value, syncing)
    local name = safe_display_text(value)
    if name == "" then
        name = syncing and "ĐANG ĐỒNG BỘ" or "KHÔNG XÁC ĐỊNH"
    end
    return {
        text = "TÊN • "
            .. name
            .. " • [F5 bật/tắt thông báo]",
        name = name,
    }
end

local function inactive_view(syncing, player_name, gacha_spins)
    local gacha = gacha_view(gacha_spins)
    return {
        player = player_text(player_name, syncing).text,
        gacha = gacha.text,
        gacha_time = gacha.time,
        gacha_active = gacha.active,
        gacha_progress = gacha.progress,
        booster = "",
        time = "",
        boosted = false,
        progress = 0,
    }
end

local function booster_view(status, now)
    if type(status) ~= "table"
        or tonumber(status.multiplier) == nil
        or tonumber(status.multiplier) <= 1
    then
        return inactive_view(
            false,
            type(status) == "table" and status.player_name or nil,
            type(status) == "table" and status.gacha_spins or nil
        )
    end

    local labels = {}
    if type(status.sources) == "table" then
        for _, source in ipairs(status.sources) do
            local label = safe_source_label(source)
            if label ~= "" and #labels < 4 then
                table.insert(labels, label)
            end
        end
    elseif type(status.sources) == "string" then
        local label = safe_source_label(status.sources)
        if label ~= "" then
            table.insert(labels, label)
        end
    end

    local progress = 1
    local remaining_text = "Đang hoạt động"
    local active_since = tonumber(status.active_since_unix) or 0
    local active_until = tonumber(status.active_until_unix) or 0
    if active_since > 0 and active_until > active_since then
        progress = (now - active_since) / (active_until - active_since)
        remaining_text = format_remaining(active_until - now)
    end

    local source_text = #labels > 0
        and table.concat(labels, " + ")
        or ("EXP x" .. format_multiplier(status.multiplier))
    local gacha = gacha_view(status.gacha_spins)
    return {
        player = player_text(status.player_name, false).text,
        gacha = gacha.text,
        gacha_time = gacha.time,
        gacha_active = gacha.active,
        gacha_progress = gacha.progress,
        booster = "Booster • " .. source_text,
        time = "Thời gian • " .. remaining_text,
        boosted = true,
        progress = math.max(0, math.min(1, progress)),
    }
end

local function render_protocol(raw_message)
    local boosted_flag,
        active_since,
        active_until,
        multiplier,
        capped_flag,
        gacha_spins,
        player_name,
        sources = raw_message:match(
            "^%[PALHUD%]|([012])|(%d+)|(%d+)|([^|]*)|([01])|"
                .. "(%-?%d+)|([^|]*)|(.*)$"
        )
    if boosted_flag == nil then
        return false
    end
    local status = nil
    if boosted_flag == "1" then
        status = {
            multiplier = tonumber(multiplier),
            sources = sources,
            capped = capped_flag == "1",
            active_since_unix = tonumber(active_since),
            active_until_unix = tonumber(active_until),
            gacha_spins = tonumber(gacha_spins),
            player_name = player_name,
        }
    elseif boosted_flag == "0" then
        status = {
            multiplier = 1,
            sources = {},
            capped = false,
            active_since_unix = 0,
            active_until_unix = 0,
            gacha_spins = tonumber(gacha_spins),
            player_name = player_name,
        }
    end
    local view = boosted_flag == "2"
        and inactive_view(
            true,
            player_name,
            tonumber(gacha_spins)
        )
        or booster_view(status, os.time())
    last_client_view = view
    if not protocol_received_logged then
        protocol_received_logged = true
        log("INFO", "Client protocol received.")
    end
    local rendered = render_client_hud(view)
    if rendered and not render_started_logged then
        render_started_logged = true
        log("INFO", "Client HUD render started.")
    end
    return true
end

local function render_combat_protocol(raw_message)
    local next_state = parse_combat_protocol(raw_message, os.time())
    if next_state == nil then
        return false
    end
    combat_state = next_state
    local rendered = render_client_hud(last_client_view)
    if rendered and combat_visibility_logged ~= combat_state.active then
        combat_visibility_logged = combat_state.active
        log(
            "INFO",
            combat_state.active
                and "Combat cooldown shown on PalHud."
                or "Combat cooldown cleared from PalHud."
        )
    end
    return true
end

local function render_gacha_protocol(raw_message)
    local next_state = parse_gacha_protocol(raw_message, os.time())
    if next_state == nil then
        return false
    end
    gacha_reward_state = next_state
    return true
end

local function on_client_message(
    controller_context,
    message_param,
    message_type_param,
    duration_param
)
    local ok, error_message = pcall(function()
        local controller = unwrap(controller_context)
        local local_ok, is_local = call_method(
            controller,
            "IsLocalPlayerController"
        )
        if not local_ok or is_local ~= true then
            return
        end
        local message = as_text(message_param)
        if not render_protocol(message) then
            if not render_gacha_protocol(message)
                and not render_combat_protocol(message)
                and not render_hologram_protocol(message)
            then
                render_hologram_notice_protocol(message)
            end
        end
    end)
    if not ok then
        log(
            "ERROR",
            "Client protocol handler failed: " .. tostring(error_message)
        )
    end
end

local function on_chat(game_state_context, chat_message_param)
    local ok, error_message = pcall(function()
        local message_object = unwrap(chat_message_param)
        if message_object == nil then
            return
        end
        if as_text(read_member(message_object, "Sender")):upper() ~= "SYSTEM" then
            return
        end
        if find_local_player_controller() == nil then
            return
        end
        if render_protocol(as_text(read_member(message_object, "Message"))) then
            call_method(read_member(message_object, "Message"), "Clear")
        end
    end)
    if not ok then
        log(
            "ERROR",
            "Legacy chat protocol handler failed: " .. tostring(error_message)
        )
    end
end

local function controller_key(controller)
    local ok, full_name = call_method(controller, "GetFullName")
    if ok and full_name ~= nil then
        return tostring(full_name)
    end
    return tostring(controller)
end

local function live_player_entry(raw_player)
    local player = raw_player
    if not is_valid(player) then
        return nil, "invalid"
    end
    local controller_ok, controller = call_method(
        player,
        "GetPalPlayerController"
    )
    if not controller_ok or not is_valid(controller) then
        return nil, "no_controller"
    end
    local state_ok, state = call_method(player, "GetCachedPlayerState")
    if not state_ok or not is_valid(state) then
        return nil, "no_state"
    end
    return {
        controller = controller,
        player = player,
        state = state,
    }, "ready"
end

local function cache_player(raw_player)
    local entry, reason = live_player_entry(raw_player)
    if entry == nil then
        return false, reason
    end
    controllers[controller_key(entry.controller)] = entry
    return true, "ready"
end

local function forget_controller(raw_controller)
    local controller = unwrap(raw_controller)
    if controller == nil then
        return
    end
    local removed = false
    for key, entry in pairs(controllers) do
        if entry.controller == controller then
            controllers[key] = nil
            removed = true
        end
    end
    if removed then
        discovery_complete = false
        next_discovery_at = os.time() + DISCOVERY_RETRY_SECONDS
        log("INFO", "Released player controller cache.")
    end
end

local function discover_controllers()
    local found = {}
    local seen = {}
    local successful_scans = 0
    for _, class_name in ipairs(PLAYER_CLASS_NAMES) do
        local ok, instances = pcall(FindAllOf, class_name)
        if ok and type(instances) == "table" then
            successful_scans = successful_scans + 1
            for _, player in ipairs(instances) do
                local key = controller_key(player)
                if not seen[key] then
                    seen[key] = true
                    table.insert(found, player)
                end
            end
        end
    end
    if successful_scans == 0 then
        next_discovery_at = os.time() + DISCOVERY_RETRY_SECONDS
        return false
    end
    local counts = {
        found = #found,
        ready = 0,
        invalid = 0,
        no_controller = 0,
        no_state = 0,
    }
    for _, player in ipairs(found) do
        local cached, reason = cache_player(player)
        counts[reason] = (counts[reason] or 0) + 1
    end
    local signature = string.format(
        "%d:%d:%d:%d:%d",
        counts.found,
        counts.ready,
        counts.invalid,
        counts.no_controller,
        counts.no_state
    )
    if signature ~= last_discovery_signature then
        last_discovery_signature = signature
        log(
            "INFO",
            string.format(
                "Player scan found=%d ready=%d invalid=%d no_controller=%d no_state=%d.",
                counts.found,
                counts.ready,
                counts.invalid,
                counts.no_controller,
                counts.no_state
            )
        )
    end
    if counts.ready == 0 then
        next_discovery_at = os.time() + DISCOVERY_RETRY_SECONDS
        return false
    end
    discovery_complete = true
    return true
end

local function valid_runtime(document)
    return type(document) == "table"
        and type(document.expires_at_unix) == "number"
        and type(document.hud_players) == "table"
end

local function queue_runtime_reload()
    if reload_pending then
        return
    end
    reload_pending = true
    ExecuteAsync(function()
        local ok, document = pcall(dofile, RUNTIME_PATH)
        ExecuteInGameThread(function()
            reload_pending = false
            if ok and valid_runtime(document) then
                runtime = document
            end
        end)
    end)
end

local function read_hologram_store()
    local file = io.open(HOLOGRAM_STORE_PATH, "rb")
    if file == nil then
        return {}, nil
    end
    local document = file:read("*a")
    file:close()
    return Hologram.parse_store(document)
end

local function write_hologram_store(document)
    local temporary_path = HOLOGRAM_STORE_PATH .. ".tmp"
    local backup_path = HOLOGRAM_STORE_PATH .. ".bak"
    local file, open_error = io.open(temporary_path, "wb")
    if file == nil then
        return false, open_error
    end
    local write_ok, write_error = file:write(document)
    file:close()
    if write_ok == nil then
        os.remove(temporary_path)
        return false, write_error
    end

    os.remove(backup_path)
    local had_previous = os.rename(HOLOGRAM_STORE_PATH, backup_path) == true
    local published, publish_error = os.rename(
        temporary_path,
        HOLOGRAM_STORE_PATH
    )
    if not published then
        if had_previous then
            os.rename(backup_path, HOLOGRAM_STORE_PATH)
        end
        os.remove(temporary_path)
        return false, publish_error
    end
    os.remove(backup_path)
    return true, nil
end

local function queue_hologram_load()
    if hologram_store_loaded or hologram_load_pending then
        return
    end
    hologram_load_pending = true
    ExecuteAsync(function()
        local ok, document, load_error = pcall(read_hologram_store)
        ExecuteInGameThread(function()
            hologram_load_pending = false
            if ok and type(document) == "table" then
                server_holograms = document
                hologram_store_loaded = true
                server_hologram_revision = server_hologram_revision + 1
                log(
                    "INFO",
                    string.format(
                        "Hologram store loaded (%d entries).",
                        #Hologram.sorted_ids(server_holograms)
                    )
                )
            else
                log(
                    "ERROR",
                    "Hologram store load failed: "
                        .. tostring(load_error or document)
                )
            end
        end)
    end)
end

local queue_hologram_persist
queue_hologram_persist = function()
    if hologram_persist_pending then
        hologram_persist_dirty = true
        return
    end
    hologram_persist_pending = true
    hologram_persist_dirty = false
    local revision = server_hologram_revision
    local document = Hologram.serialize_store(server_holograms)
    ExecuteAsync(function()
        local ok, written, write_error = pcall(
            write_hologram_store,
            document
        )
        ExecuteInGameThread(function()
            hologram_persist_pending = false
            if not ok or written ~= true then
                log(
                    "ERROR",
                    "Hologram store write failed: "
                        .. tostring(write_error or written)
                )
            end
            if hologram_persist_dirty
                or server_hologram_revision ~= revision
            then
                queue_hologram_persist()
            end
        end)
    end)
end

local function player_state(entry)
    if is_valid(entry.state) then
        return entry.state
    end
    local state_ok, state = call_method(
        entry.player,
        "GetCachedPlayerState"
    )
    if state_ok and is_valid(state) then
        entry.state = state
        return state
    end
    state_ok, state = call_method(entry.controller, "GetPalPlayerState")
    if state_ok and is_valid(state) then
        entry.state = state
        return state
    end
    return nil
end

local function normalized_player_name(value)
    local name = as_text(value)
    name = name:gsub("[%c|]", " "):gsub("%s+", " ")
    name = name:match("^%s*(.-)%s*$") or ""
    return name:lower()
end

local function player_name(entry)
    local state = player_state(entry)
    if state == nil then
        return nil
    end
    local name_ok, name = call_method(state, "GetPlayerName")
    if not name_ok then
        return nil
    end
    name = as_text(name):match("^%s*(.-)%s*$") or ""
    if name == "" then
        return nil
    end
    return name
end

local function send_hologram_notice(controller, message)
    if not is_valid(controller) or protocol_message_type == nil then
        return false
    end
    return call_method(
        controller,
        "ClientMessage",
        "[PALHOLO-NOTICE]|" .. Hologram.percent_encode(message),
        protocol_message_type,
        0
    )
end

local function controllers_for_sender(sender_name)
    local matches = {}
    local seen = {}
    for _, class_name in ipairs(PLAYER_CLASS_NAMES) do
        local ok, instances = pcall(FindAllOf, class_name)
        if ok and type(instances) == "table" then
            for _, player in ipairs(instances) do
                local entry = live_player_entry(player)
                if entry ~= nil and player_name(entry) == sender_name then
                    local key = controller_key(entry.controller)
                    if not seen[key] then
                        seen[key] = true
                        table.insert(matches, entry.controller)
                    end
                end
            end
        end
    end
    return matches
end

local function hologram_help()
    return "Lệnh: !holo set <id> <dòng 1 | dòng 2>; "
        .. "!holo move <id>; !holo remove <id>; !holo list"
end

local function on_admin_chat(_, chat_message_param)
    local ok, error_message = pcall(function()
        local message_object = unwrap(chat_message_param)
        if message_object == nil then
            return
        end
        local message_value = read_member(message_object, "Message")
        local command, command_error = Hologram.parse_command(
            as_text(message_value)
        )
        if command == nil and command_error == nil then
            return
        end

        call_value_method(message_value, "Clear")
        if find_local_player_controller() ~= nil then
            return
        end

        local sender_name = Hologram.trim(as_text(
            read_member(message_object, "Sender")
        ))
        if sender_name == "" or sender_name:upper() == "SYSTEM" then
            return
        end
        local matching_controllers = controllers_for_sender(sender_name)
        if #matching_controllers ~= 1 then
            for _, controller in ipairs(matching_controllers) do
                send_hologram_notice(
                    controller,
                    "Không thể xác định duy nhất người gửi lệnh."
                )
            end
            return
        end
        local controller = matching_controllers[1]
        if not Hologram.is_admin(controller) then
            send_hologram_notice(
                controller,
                "Bạn không có quyền quản trị Palworld."
            )
            return
        end
        if command == nil then
            send_hologram_notice(controller, hologram_help())
            return
        end
        if command.action == "help" then
            send_hologram_notice(controller, hologram_help())
            return
        end
        if not hologram_store_loaded then
            send_hologram_notice(
                controller,
                "Dữ liệu hologram đang tải, hãy thử lại sau vài giây."
            )
            return
        end
        if command.action == "list" then
            local ids = Hologram.sorted_ids(server_holograms)
            send_hologram_notice(
                controller,
                #ids == 0
                    and "Chưa có hologram."
                    or ("Hologram: " .. table.concat(ids, ", "))
            )
            return
        end

        if command.action == "remove" then
            if server_holograms[command.id] == nil then
                send_hologram_notice(
                    controller,
                    "Không tìm thấy hologram '" .. command.id .. "'."
                )
                return
            end
            server_holograms[command.id] = nil
            server_hologram_revision = server_hologram_revision + 1
            queue_hologram_persist()
            send_hologram_notice(
                controller,
                "Đã xóa hologram '" .. command.id .. "'."
            )
            return
        end

        local location = Hologram.controller_location(controller)
        if location == nil then
            send_hologram_notice(
                controller,
                "Không đọc được vị trí nhân vật hiện tại."
            )
            return
        end
        if command.action == "move" then
            local record = server_holograms[command.id]
            if record == nil then
                send_hologram_notice(
                    controller,
                    "Không tìm thấy hologram '" .. command.id .. "'."
                )
                return
            end
            record.x = location.X
            record.y = location.Y
            record.z = location.Z + HOLOGRAM_HEIGHT_OFFSET
            server_hologram_revision = server_hologram_revision + 1
            queue_hologram_persist()
            send_hologram_notice(
                controller,
                "Đã chuyển hologram '" .. command.id .. "'."
            )
            return
        end

        if server_holograms[command.id] == nil
            and #Hologram.sorted_ids(server_holograms)
                >= HOLOGRAM_MAX_COUNT
        then
            send_hologram_notice(
                controller,
                "Đã đạt giới hạn "
                    .. tostring(HOLOGRAM_MAX_COUNT)
                    .. " hologram."
            )
            return
        end
        server_holograms[command.id] = {
            id = command.id,
            x = location.X,
            y = location.Y,
            z = location.Z + HOLOGRAM_HEIGHT_OFFSET,
            max_distance = HOLOGRAM_DEFAULT_MAX_DISTANCE,
            text = command.text,
        }
        server_hologram_revision = server_hologram_revision + 1
        queue_hologram_persist()
        send_hologram_notice(
            controller,
            "Đã đặt hologram '" .. command.id .. "' tại vị trí hiện tại."
        )
    end)
    if not ok then
        log(
            "ERROR",
            "Hologram admin command failed: " .. tostring(error_message)
        )
    end
end

local function runtime_status_for_player_name(name)
    local normalized = normalized_player_name(name)
    if normalized == "" or type(runtime.hud_players) ~= "table" then
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

format_multiplier = function(value)
    local multiplier = tonumber(value) or 1
    local formatted = string.format("%.2f", multiplier)
    formatted = formatted:gsub("0+$", ""):gsub("%.$", "")
    return formatted
end

safe_source_label = function(value)
    local label = tostring(value or "")
    label = label:gsub("[%c|]", " "):gsub("%s+", " ")
    label = label:match("^%s*(.-)%s*$") or ""
    if #label > 80 then
        label = label:sub(1, 80)
    end
    return label
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

local function send_protocol(entry, status, syncing)
    if not is_valid(entry.controller) then
        return false, "controller_unavailable"
    end
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
    local gacha_next_reward_seconds = type(status) == "table"
        and math.floor(
            tonumber(status.gacha_next_reward_seconds) or -1
        )
        or -1
    local gacha_reward_spins = type(status) == "table"
        and math.floor(tonumber(status.gacha_reward_spins) or 0)
        or 0
    local gacha_reward_interval_seconds = type(status) == "table"
        and math.floor(
            tonumber(status.gacha_reward_interval_seconds) or 0
        )
        or 0
    if syncing
        or gacha_next_reward_seconds < -1
        or gacha_next_reward_seconds > 604800
        or gacha_reward_spins < 0
        or gacha_reward_spins > 1000
        or gacha_reward_interval_seconds < 0
        or gacha_reward_interval_seconds > 604800
        or (gacha_next_reward_seconds < 0) ~= (gacha_reward_spins == 0)
        or (gacha_next_reward_seconds < 0)
            ~= (gacha_reward_interval_seconds == 0)
        or gacha_next_reward_seconds > gacha_reward_interval_seconds
    then
        gacha_next_reward_seconds = -1
        gacha_reward_spins = 0
        gacha_reward_interval_seconds = 0
    elseif gacha_next_reward_seconds >= 0 then
        local runtime_age = math.max(
            0,
            os.time() - math.floor(
                tonumber(runtime.generated_at_unix) or os.time()
            )
        )
        gacha_next_reward_seconds = math.max(
            0,
            gacha_next_reward_seconds - runtime_age
        )
    end
    call_method(
        entry.controller,
        "ClientMessage",
        GACHA_PROTOCOL_PREFIX
            .. tostring(gacha_next_reward_seconds)
            .. "|"
            .. tostring(gacha_reward_spins)
            .. "|"
            .. tostring(gacha_reward_interval_seconds),
        protocol_message_type,
        0
    )
    local protocol_message = PROTOCOL_PREFIX
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
        .. safe_protocol_text(
            type(status) == "table" and status.player_name or ""
        )
        .. "|"
        .. safe_protocol_text(protocol_sources(status))
    local sent = call_method(
        entry.controller,
        "ClientMessage",
        protocol_message,
        protocol_message_type,
        0
    )
    if not sent then
        return false, "rpc_call_failed"
    end
    if hologram_store_loaded then
        local hologram_sent = call_method(
            entry.controller,
            "ClientMessage",
            Hologram.serialize_protocol(
                server_hologram_revision,
                server_holograms
            ),
            protocol_message_type,
            0
        )
        if not hologram_sent then
            return false, "hologram_rpc_call_failed"
        end
    end
    return true, nil
end

local function update_hud()
    local now = os.time()
    if now >= next_runtime_reload_at then
        next_runtime_reload_at = now + SERVER_REFRESH_SECONDS
        queue_runtime_reload()
    end
    if find_local_player_controller() ~= nil then
        local should_render = combat_state.active
            or (
                gacha_reward_state.active
                and gacha_reward_state.balance ~= nil
            )
        if gacha_reward_state.balance ~= nil then
            local gacha = gacha_view(
                gacha_reward_state.balance
            )
            last_client_view.gacha = gacha.text
            last_client_view.gacha_time = gacha.time
            last_client_view.gacha_active = gacha.active
            last_client_view.gacha_progress = gacha.progress
        end
        if should_render then
            render_client_hud(last_client_view)
        end
        controllers = {}
        discovery_complete = false
        next_discovery_at = now + DISCOVERY_RETRY_SECONDS
        return
    end
    if now < next_server_delivery_at then
        return
    end
    next_server_delivery_at = now + SERVER_REFRESH_SECONDS

    if not hologram_store_loaded then
        queue_hologram_load()
    end
    if not discovery_complete and now >= next_discovery_at then
        discover_controllers()
    end

    local active = runtime.expires_at_unix > now
    local invalid = {}
    for key, entry in pairs(controllers) do
        if not is_valid(entry.controller) or not is_valid(entry.player) then
            table.insert(invalid, key)
        else
            local name = player_name(entry)
            local status = nil
            if active then
                status = runtime_status_for_player_name(name)
            end
            local syncing = not active or status == nil
            if status == nil then
                status = {
                    player_name = name,
                    multiplier = 1,
                    sources = {},
                    gacha_spins = -1,
                }
            end
            local sent, failure_reason = send_protocol(
                entry,
                status,
                syncing
            )
            if sent and not delivery_started_logged then
                delivery_started_logged = true
                last_delivery_failure_reason = nil
                log("INFO", "HUD delivery started via targeted ClientMessage.")
            elseif not sent
                and failure_reason ~= last_delivery_failure_reason
            then
                last_delivery_failure_reason = failure_reason
                log(
                    "ERROR",
                    "HUD delivery failed: " .. tostring(failure_reason)
                )
            end
        end
    end
    for _, key in ipairs(invalid) do
        controllers[key] = nil
    end
    controllers = {}
    discovery_complete = false
    next_discovery_at = now + DISCOVERY_RETRY_SECONDS
end

Callbacks.game_thread_hud_tick = function()
    hud_tick_pending = false
    hud_tick_queued_at = 0
    local ok, error_message = pcall(update_hud)
    if not ok then
        log(
            "ERROR",
            "HUD game-thread update failed: " .. tostring(error_message)
        )
    end
end

Callbacks.async_hud_tick = function()
    local now = os.time()
    if hud_tick_pending
        and now - hud_tick_queued_at >= 10
    then
        hud_tick_pending = false
        hud_tick_queued_at = 0
        log("WARN", "Recovering stale queued HUD update.")
    end
    if hud_tick_pending then
        return false
    end

    hud_tick_pending = true
    hud_tick_queued_at = now
    local queued, queue_error = pcall(
        ExecuteInGameThread,
        Callbacks.game_thread_hud_tick
    )
    if not queued then
        hud_tick_pending = false
        hud_tick_queued_at = 0
        log(
            "ERROR",
            "HUD game-thread queue failed: " .. tostring(queue_error)
        )
    end
    return false
end

local function start()
    if started then
        return
    end
    started = true

    local required_functions = {
        CLIENT_MESSAGE_PATH,
        "/Script/Pal.PalPlayerCharacter:GetPalPlayerController",
        "/Script/Pal.PalPlayerCharacter:GetCachedPlayerState",
        "/Script/Pal.PalPlayerController:GetPalPlayerState",
        "/Script/Engine.Controller:K2_GetPawn",
        "/Script/Engine.Actor:K2_GetActorLocation",
        "/Script/Engine.PlayerController:ProjectWorldLocationToScreen",
        "/Script/Engine.PlayerState:GetPlayerName",
        CONTROLLER_POSSESS_PATH,
        CONTROLLER_UNPOSSESS_PATH,
        GAME_MODE_LOGOUT_PATH,
        CHAT_PATH,
    }
    for _, path in ipairs(required_functions) do
        if find_required_object(path) == nil then
            log("ERROR", "PalHud disabled because the game API changed.")
            return
        end
    end

    protocol_message_type = new_object_name("PalHud")

    Callbacks.empty_pre_hook = Callbacks.empty_pre_hook or function() end
    Callbacks.on_admin_chat = on_admin_chat
    Callbacks.on_chat = on_chat
    Callbacks.on_client_message = on_client_message
    Callbacks.on_logout = function(_, exiting_controller)
        forget_controller(exiting_controller)
    end
    Callbacks.on_unpossess = function(controller)
        if collapse_hud_for_local_controller(controller) then
            clear_client_holograms()
        end
        forget_controller(controller)
    end
    Callbacks.on_possess = function()
        discovery_complete = false
        next_discovery_at = 0
    end
    local chat_ok, chat_error = pcall(
        RegisterHook,
        CHAT_PATH,
        Callbacks.on_admin_chat,
        Callbacks.on_chat
    )
    local client_message_ok, client_message_error = pcall(
        RegisterHook,
        CLIENT_MESSAGE_PATH,
        Callbacks.empty_pre_hook,
        Callbacks.on_client_message
    )
    local possess_ok, possess_error = pcall(
        RegisterHook,
        CONTROLLER_POSSESS_PATH,
        Callbacks.empty_pre_hook,
        Callbacks.on_possess
    )
    local logout_ok, logout_error = pcall(
        RegisterHook,
        GAME_MODE_LOGOUT_PATH,
        Callbacks.on_logout,
        Callbacks.empty_pre_hook
    )
    local unpossess_ok, unpossess_error = pcall(
        RegisterHook,
        CONTROLLER_UNPOSSESS_PATH,
        Callbacks.on_unpossess,
        Callbacks.empty_pre_hook
    )
    if not chat_ok
        or not client_message_ok
        or not possess_ok
        or not logout_ok
        or not unpossess_ok
    then
        log(
            "ERROR",
            "PalHud hook registration failed: "
                .. tostring(
                    chat_error
                        or client_message_error
                        or possess_error
                        or logout_error
                        or unpossess_error
                )
        )
        return
    end

    local startup_discovered = discover_controllers()
    controllers = {}
    discovery_complete = false
    next_discovery_at = 0
    if not startup_discovered then
        log(
            "INFO",
            "No player controller at startup; waiting for world entry."
        )
    end
    queue_runtime_reload()
    next_runtime_reload_at = os.time() + SERVER_REFRESH_SECONDS
    LoopAsync(HUD_TICK_MS, Callbacks.async_hud_tick)
    log(
        "INFO",
        string.format(
            "v%s started; mode=server+client+hologram hud_tick=%dms server_refresh=%ds.",
            VERSION,
            HUD_TICK_MS,
            SERVER_REFRESH_SECONDS
        )
    )
end

if rawget(_G, "__PALHUD_TESTING") == true then
    _G.__PALHUD_TEST_API = {
        as_text = as_text,
        combat_view = combat_view,
        controller_location = Hologram.controller_location,
        find_local_player_controller = find_local_player_controller,
        is_admin_controller = Hologram.is_admin,
        is_valid = is_valid,
        parse_combat_protocol = parse_combat_protocol,
        parse_gacha_protocol = parse_gacha_protocol,
        parse_hologram_command = Hologram.parse_command,
        parse_hologram_protocol = Hologram.parse_protocol,
        parse_hologram_store = Hologram.parse_store,
        project_world_location = Hologram.project_world_location,
        serialize_hologram_protocol = Hologram.serialize_protocol,
        serialize_hologram_store = Hologram.serialize_store,
    }
    return
end

log("INFO", "Loading PalHud v" .. VERSION .. ".")
local f5_key_ok, f5_key = pcall(function()
    return Key.F5
end)
if f5_key_ok and f5_key ~= nil and RegisterKeyBind ~= nil then
    local bind_ok, bind_error = pcall(
        RegisterKeyBind,
        f5_key,
        Callbacks.toggle_hud_visibility
    )
    if bind_ok then
        log("INFO", "F5 HUD toggle registered.")
    else
        log(
            "ERROR",
            "F5 HUD toggle registration failed: " .. tostring(bind_error)
        )
    end
else
    log("INFO", "F5 HUD toggle unavailable on this host.")
end
Callbacks.start = start
RegisterInitGameStatePostHook(Callbacks.start)
ExecuteInGameThreadWithDelay(10000, Callbacks.start)
