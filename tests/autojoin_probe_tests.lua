local registered_class = nil
local registered_callback = nil
local connect_calls = {}

function NotifyOnNewObject(class_name, callback)
    registered_class = class_name
    registered_callback = callback
end

local script_path =
    "third_party/UE4SSExperimentalPW/Mods/Pal3MienAutoJoin/Scripts/main.lua"
local loaded, load_error = pcall(dofile, script_path)
assert(loaded, load_error)
assert(
    registered_class == "/Script/Pal.PalUIJoinGameBase",
    "AutoJoin must follow the multiplayer join widget lifecycle"
)
assert(type(registered_callback) == "function", "AutoJoin hook must register")

local join_widget = {}
function join_widget:ConnectServerByAddress(host, port)
    table.insert(connect_calls, { host = host, port = port })
end

local connected, connect_error = pcall(registered_callback, join_widget)
assert(connected, connect_error)
assert(#connect_calls == 1, "AutoJoin must connect exactly once per join widget")
assert(
    connect_calls[1].host == "pal.ae3mien.net",
    "AutoJoin must use the stable player-facing hostname"
)
assert(connect_calls[1].port == 28709, "AutoJoin must use the public game port")

local ignored, ignored_error = pcall(registered_callback, nil)
assert(ignored, ignored_error)
assert(#connect_calls == 1, "AutoJoin must ignore an unavailable join widget")

print("PASS Pal3Mien AutoJoin probe")
