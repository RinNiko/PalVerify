local commands = {}
local logs = {}

package.path = "packaging/server/Scripts/?.lua;"
    .. "packaging/server/Scripts/?/init.lua;"
    .. package.path

local original_print = print
print = function(message)
    table.insert(logs, tostring(message))
end

ExecuteAsync = function(callback)
    callback()
end

ExecuteInGameThreadWithDelay = function(_, _)
end

RegisterInitGameStatePostHook = function(_)
end

local original_execute = os.execute
os.execute = function(command)
    table.insert(commands, command)
    return true, "exit", 0
end

local loaded, error_message =
    pcall(dofile, "packaging/server/Scripts/main.lua")

os.execute = original_execute
print = original_print

local function fail(message)
    original_print("FAIL " .. message)
    os.exit(1)
end

if not loaded then
    fail("server main failed to load: " .. tostring(error_message))
end
if #commands ~= 1 then
    fail("expected exactly one server agent launch command")
end
if not string.find(
    commands[1],
    "PalVerifyServer.exe",
    1,
    true
) then
    fail("launch command must target PalVerifyServer.exe")
end

original_print("PASS server probe launches enforcement agent once")
