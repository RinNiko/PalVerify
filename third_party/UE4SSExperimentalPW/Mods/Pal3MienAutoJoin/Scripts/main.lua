-- Pal3Mien AutoJoin v1.0.0
-- Connects the multiplayer join screen to the stable Palworld 3 Miền address.

local SERVER_HOST = "pal.ae3mien.net"
local SERVER_PORT = 28709
local JOIN_WIDGET_CLASS = "/Script/Pal.PalUIJoinGameBase"

local function log(message)
    print("[Pal3MienAutoJoin] " .. message)
end

local function connect_to_server(join_widget)
    if join_widget == nil then
        return
    end

    local connected, connect_error = pcall(function()
        join_widget:ConnectServerByAddress(SERVER_HOST, SERVER_PORT)
    end)
    if not connected then
        log("Connection request failed: " .. tostring(connect_error))
    end
end

local registered, register_error = pcall(function()
    NotifyOnNewObject(JOIN_WIDGET_CLASS, connect_to_server)
end)
if registered then
    log("Ready for " .. SERVER_HOST .. ":" .. tostring(SERVER_PORT))
else
    log("Join widget hook unavailable: " .. tostring(register_error))
end
