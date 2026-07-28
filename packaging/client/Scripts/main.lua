local MOD_NAME = "PalVerify"

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

local SCRIPT_DIRECTORY = script_directory()
local UI_QUEUE = SCRIPT_DIRECTORY .. "ui-queue"
local ui_sequence = 0

local function log(level, message)
    print(string.format("[%s] [%s] %s\n", MOD_NAME, level, message))
end

local function to_string(value)
    if value == nil then
        return ""
    end
    local ok, result = pcall(function()
        return value:ToString()
    end)
    if ok and result then
        return tostring(result)
    end
    return ""
end

local function queue_ui_command(command)
    ui_sequence = (ui_sequence + 1) % 10000
    local name = string.format(
        "ui-%d-%04d",
        os.time(),
        ui_sequence
    )
    local temporary_path = UI_QUEUE .. "\\" .. name .. ".tmp"
    local final_path = UI_QUEUE .. "\\" .. name .. ".cmd"
    local file = io.open(temporary_path, "wb")
    if not file then
        log("ERROR", "Could not create a client UI command.")
        return
    end
    file:write(command)
    file:close()
    local renamed = os.rename(temporary_path, final_path)
    if not renamed then
        os.remove(temporary_path)
        log("ERROR", "Could not publish a client UI command.")
    end
end

local function on_chat(_, chat_message_param)
    local ok, error_message = pcall(function()
        if chat_message_param == nil then
            return
        end
        local message_object = chat_message_param:get()
        if message_object == nil then
            return
        end
        if to_string(message_object.Sender):upper() ~= "SYSTEM" then
            return
        end
        local message = to_string(message_object.Message)
        local verify_code =
            message:match("^%[PAL3MIEN_VERIFY%]%s+(%d%d%d%d%d%d)$")
        if verify_code then
            message_object.Message:Clear()
            queue_ui_command("verify|" .. verify_code)
            return
        end
        if message == "[PAL3MIEN_GIFTCODE]" then
            message_object.Message:Clear()
            queue_ui_command("giftcode|")
        end
    end)
    if not ok then
        log(
            "ERROR",
            "Client UI chat handler failed: "
                .. tostring(error_message)
        )
    end
end

log("INFO", "Loading PalVerify client v1.0.14.")
RegisterHook(
    "/Script/Pal.PalGameStateInGame:BroadcastChatMessage",
    on_chat
)
