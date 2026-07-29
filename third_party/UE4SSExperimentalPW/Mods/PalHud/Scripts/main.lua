local MOD_NAME = "PalHud"
local VERSION = "1.4.8"
local HUD_TICK_MS = 1000
local SERVER_REFRESH_SECONDS = 5
local DISCOVERY_RETRY_SECONDS = 5
local PROTOCOL_PREFIX = "[PALHUD]|"
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
local HUD_BOOSTER_NAME = "PalHud_Booster"
local HUD_TIME_NAME = "PalHud_Time"
local HUD_PROGRESS_HEIGHT_NAME = "PalHud_ProgressHeight"
local HUD_PROGRESS_NAME = "PalHud_Progress"
local PLAYER_CLASS_NAMES = {
    "PalPlayerCharacter",
    "BP_Player_Female_C",
    "BP_Player_Male_C",
}
PalHudCallbacks = PalHudCallbacks or {}
local Callbacks = PalHudCallbacks

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
local progress_bar_class = nil
local runtime = {
    revision = "unavailable",
    expires_at_unix = 0,
    hud_players = {},
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

local function apply_hud_visibility()
    if not is_valid(hud_overlay) then
        return false
    end
    return call_method(
        hud_overlay,
        "SetVisibility",
        hud_visible and 3 or 1
    )
end

local function collapse_hud_for_local_controller(raw_controller)
    if not is_valid(hud_overlay) then
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
    return call_method(hud_overlay, "SetVisibility", 1)
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
    local ok, text = call_method(value, "ToString")
    if ok and text ~= nil then
        return tostring(text)
    end
    value = unwrap(value)
    if type(value) == "string" then
        return value
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
    local booster_color = make_linear_color(0.72, 0.62, 1.0, 1.0)
    local time_color = make_linear_color(0.48, 0.88, 0.94, 1.0)
    if not configure_text_block(player_block, 15, player_color)
        or not configure_text_block(gacha_block, 14, gacha_color)
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
            progress_bar,
            "SetFillColorAndOpacity",
            make_linear_color(0.12, 0.86, 0.94, 1.0)
        )
        or not call_method(progress_height_box, "SetContent", progress_bar)
        or not call_method(content, "AddChildToVerticalBox", player_block)
        or not call_method(content, "AddChildToVerticalBox", gacha_block)
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
    hud_booster_block = booster_block
    hud_time_block = time_block
    hud_progress_box = progress_height_box
    hud_progress_bar = progress_bar
    log("INFO", "Decorated PalBooster HUD card added to player viewport.")
    return true, nil
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
    local progress = math.max(0, math.min(1, tonumber(view.progress) or 0))
    local booster_visibility = view.boosted and 3 or 1
    local ok = player_ok
        and gacha_ok
        and booster_ok
        and time_ok
        and call_method(hud_player_block, "SetText", player_text)
        and call_method(hud_gacha_block, "SetText", gacha_text)
        and call_method(hud_booster_block, "SetText", booster_text)
        and call_method(hud_time_block, "SetText", time_text)
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

local function gacha_text(value)
    local spins = tonumber(value)
    if spins == nil or spins < 0 then
        return "GACHA • ĐANG ĐỒNG BỘ"
    end
    spins = math.max(0, math.floor(spins))
    return string.format("GACHA • %d", spins)
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
    return {
        player = player_text(player_name, syncing).text,
        gacha = gacha_text(gacha_spins),
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
    return {
        player = player_text(status.player_name, false).text,
        gacha = gacha_text(status.gacha_spins),
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
        render_protocol(as_text(message_param))
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

local function cache_player(raw_player)
    local player = raw_player
    if not is_valid(player) then
        return false, "invalid"
    end
    local controller_ok, controller = call_method(
        player,
        "GetPalPlayerController"
    )
    if not controller_ok or not is_valid(controller) then
        return false, "no_controller"
    end
    local state_ok, state = call_method(player, "GetCachedPlayerState")
    if not state_ok or not is_valid(state) then
        return false, "no_state"
    end
    controllers[controller_key(controller)] = {
        controller = controller,
        player = player,
        state = state,
    }
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
    return true, nil
end

local function update_hud()
    local now = os.time()
    if now >= next_runtime_reload_at then
        next_runtime_reload_at = now + SERVER_REFRESH_SECONDS
        queue_runtime_reload()
    end
    if now < next_server_delivery_at then
        return
    end
    next_server_delivery_at = now + SERVER_REFRESH_SECONDS

    if find_local_player_controller() ~= nil then
        controllers = {}
        discovery_complete = false
        next_discovery_at = now + DISCOVERY_RETRY_SECONDS
        return
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
    Callbacks.on_chat = on_chat
    Callbacks.on_client_message = on_client_message
    Callbacks.on_logout = function(_, exiting_controller)
        forget_controller(exiting_controller)
    end
    Callbacks.on_unpossess = function(controller)
        collapse_hud_for_local_controller(controller)
        forget_controller(controller)
    end
    Callbacks.on_possess = function()
        discovery_complete = false
        next_discovery_at = 0
    end
    local chat_ok, chat_error = pcall(
        RegisterHook,
        CHAT_PATH,
        Callbacks.empty_pre_hook,
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
            "v%s started; mode=server+client hud_tick=%dms server_refresh=%ds.",
            VERSION,
            HUD_TICK_MS,
            SERVER_REFRESH_SECONDS
        )
    )
end

if rawget(_G, "__PALHUD_TESTING") == true then
    _G.__PALHUD_TEST_API = {
        find_local_player_controller = find_local_player_controller,
        is_valid = is_valid,
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
        toggle_hud_visibility
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
