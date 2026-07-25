local commands = {}
local logs = {}
local hooks = {}

local original_print = print
print = function(message)
    table.insert(logs, tostring(message))
end

ExecuteAsync = function(callback)
    callback()
end

RegisterHook = function(path, callback)
    hooks[path] = callback
end

local original_execute = os.execute
os.execute = function(command)
    table.insert(commands, command)
    return true, "exit", 0
end

local loaded, error_message =
    pcall(dofile, "packaging/client/Scripts/main.lua")

os.execute = original_execute
print = original_print

local function fail(message)
    original_print("FAIL " .. message)
    os.exit(1)
end

if not loaded then
    fail("client main failed to load: " .. tostring(error_message))
end
if #commands ~= 2 then
    fail("expected queue preparation and one client agent launch")
end
if not string.find(
    commands[2],
    "PalVerifyClient.exe",
    1,
    true
) then
    fail("launch command must target PalVerifyClient.exe")
end
local chat_hook =
    hooks["/Script/Pal.PalGameStateInGame:BroadcastChatMessage"]
if type(chat_hook) ~= "function" then
    fail("client must register its private UI command hook")
end

local queued_command = ""
local cleared = false
local original_open = io.open
local original_rename = os.rename
io.open = function()
    return {
        write = function(_, value)
            queued_command = value
        end,
        close = function()
        end,
    }
end
os.rename = function()
    return true
end

local function unreal_string(value, clearable)
    return {
        ToString = function()
            return value
        end,
        Clear = function()
            if clearable then
                cleared = true
            end
        end,
    }
end

chat_hook(nil, {
    get = function()
        return {
            Sender = unreal_string("SYSTEM", false),
            Message = unreal_string(
                "[PAL3MIEN_VERIFY] 123456",
                true
            ),
        }
    end,
})

io.open = original_open
os.rename = original_rename

if queued_command ~= "verify|123456" then
    fail("private verification message must become a strict UI command")
end
if not cleared then
    fail("private UI transport message must be hidden from chat")
end

original_print("PASS client probe launches agent and intercepts private UI commands")
