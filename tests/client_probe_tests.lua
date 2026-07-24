local commands = {}
local logs = {}

local original_print = print
print = function(message)
    table.insert(logs, tostring(message))
end

ExecuteAsync = function(callback)
    callback()
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
if #commands ~= 1 then
    fail("expected exactly one client agent launch command")
end
if not string.find(
    commands[1],
    "PalVerifyClient.exe",
    1,
    true
) then
    fail("launch command must target PalVerifyClient.exe")
end

original_print("PASS client probe launches observation agent once")
