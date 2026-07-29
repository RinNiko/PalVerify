local original_print = print
local failed = 0

local function check(condition, message)
    if condition then
        original_print("PASS " .. message)
        return
    end
    failed = failed + 1
    original_print("FAIL " .. message)
end

local source_path = "packaging/palhud_bridge/Scripts/main.lua"
local sent_messages = {}
local direct_get_calls = 0

local function direct_object(methods)
    methods.get = function()
        direct_get_calls = direct_get_calls + 1
        error("direct UObject must not be unwrapped")
    end
    methods.IsValid = methods.IsValid or function()
        return true
    end
    return methods
end

local state = direct_object({
    GetPlayerName = function()
        return "Alice"
    end,
})

local controller = direct_object({
    GetFullName = function()
        return "PalPlayerController /Game/Test.Alice"
    end,
    GetPalPlayerState = function()
        return state
    end,
    ClientMessage = function(_, message, message_type, channel)
        table.insert(sent_messages, {
            message = message,
            message_type = message_type,
            channel = channel,
        })
    end,
})

local player = direct_object({
    GetFullName = function()
        return "PalPlayerCharacter /Game/Test.Alice"
    end,
    GetPalPlayerController = function()
        return controller
    end,
    GetCachedPlayerState = function()
        return state
    end,
})

local stale_player = setmetatable(direct_object({
    IsValid = function()
        error("UObject instance is nullptr")
    end,
}), {
    __tostring = function()
        error("stale UObject must not be stringified")
    end,
})

FindAllOf = function()
    return { player, stale_player }
end

_G.__PALHUD_BRIDGE_TESTING = true
local loaded, load_error = pcall(dofile, source_path)
check(loaded, "PalHudBridge test harness loads")

local api = _G.__PALHUD_BRIDGE_TEST_API
check(type(api) == "table", "PalHudBridge exposes test helpers")

local stats = nil
if type(api) == "table" then
    stats = api.deliver({
        revision = "test",
        expires_at_unix = os.time() + 60,
        hud_players = {
            alice = {
                player_name = "Alice",
                active_since_unix = 100,
                active_until_unix = 200,
                multiplier = 2.5,
                capped = true,
                gacha_spins = 7,
                sources = { "Booster A" },
            },
        },
    }, "PalHudMessageType")

    check(type(stats) == "table", "delivery returns pure counters")
    check(stats.found == 2, "delivery counts discovered players")
    check(stats.invalid == 1, "stale UObject is rejected")
    check(stats.ready == 1, "valid player is ready")
    check(stats.sent == 1, "valid player receives one message")
end

check(
    #sent_messages == 1,
    "duplicate class scans do not duplicate delivery"
)
if #sent_messages == 1 then
    check(
        sent_messages[1].message
            == "[PALHUD]|1|100|200|2.5|1|7|Alice|Booster A",
        "bridge preserves the PalHud protocol"
    )
    check(
        sent_messages[1].message_type == "PalHudMessageType"
            and sent_messages[1].channel == 0,
        "bridge targets ClientMessage with the expected arguments"
    )
end
check(direct_get_calls == 0, "direct UObjects are never unwrapped")

local function contains_reference(value, references, seen)
    if references[value] then
        return true
    end
    if type(value) ~= "table" then
        return false
    end
    seen = seen or {}
    if seen[value] then
        return false
    end
    seen[value] = true
    for key, child in pairs(value) do
        if contains_reference(key, references, seen)
            or contains_reference(child, references, seen)
        then
            return true
        end
    end
    return false
end

if type(stats) == "table" then
    check(
        not contains_reference(stats, {
            [player] = true,
            [stale_player] = true,
            [controller] = true,
            [state] = true,
        }),
        "delivery result retains no UObject references"
    )
end

_G.__PALHUD_BRIDGE_TESTING = nil
_G.__PALHUD_BRIDGE_TEST_API = nil

if not loaded then
    original_print("DETAIL " .. tostring(load_error))
end
if failed > 0 then
    os.exit(1)
end
