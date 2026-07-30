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

local find_first_calls = 0
local find_all_calls = 0
FindFirstOf = function()
    find_first_calls = find_first_calls + 1
    return nil
end
FindAllOf = function()
    find_all_calls = find_all_calls + 1
    return {}
end

_G.__PALHUD_TESTING = true
local loaded, load_error = pcall(
    dofile,
    "third_party/UE4SSExperimentalPW/Mods/PalHud/Scripts/main.lua"
)
check(loaded, "PalHud lifecycle test harness loads")

local api = _G.__PALHUD_TEST_API
check(type(api) == "table", "PalHud exposes lifecycle helpers in test mode")

if type(api) == "table" then
    local string_wrapper = {
        ToString = function()
            return "[PALHUD]|0|0|0|1|0|0|Alice|"
        end,
    }
    check(
        api.as_text(string_wrapper)
            == "[PALHUD]|0|0|0|1|0|0|Alice|",
        "FString protocol wrappers do not require UObject validity"
    )

    local nested_string_wrapper = {
        get = function()
            return {
                ToString = function()
                    return "[PALHUD]|0|0|0|1|0|0|Nested|"
                end,
            }
        end,
    }
    check(
        api.as_text(nested_string_wrapper)
            == "[PALHUD]|0|0|0|1|0|0|Nested|",
        "RemoteUnrealParam unwraps before FString ToString"
    )
    check(
        type(PalHudCallbacks.toggle_hud_visibility) == "function",
        "F5 toggle callback remains strongly referenced"
    )

    local controller = api.find_local_player_controller()
    check(controller == nil, "missing lifecycle cache resolves no controller")
    check(
        find_first_calls == 0 and find_all_calls == 0,
        "controller lookup performs no global UObject scans"
    )

    local valid = api.is_valid({
        IsValid = function()
            error("UObject instance is nullptr")
        end,
    })
    check(valid == false, "IsValid errors reject stale UObjects")

    local owner = {
        IsValid = function()
            return true
        end,
        IsLocalPlayerController = function()
            return true
        end,
    }
    check(
        PalHudCallbacks.remember_local_player_controller(owner),
        "possess lifecycle caches a valid local controller"
    )
    controller = api.find_local_player_controller()
    check(controller == owner, "lifecycle cache resolves its local controller")
    check(
        find_first_calls == 0 and find_all_calls == 0,
        "cached controller lookup remains scan-free"
    )

    local remote_controller = {
        IsValid = function()
            return true
        end,
        IsLocalPlayerController = function()
            return false
        end,
    }
    check(
        not PalHudCallbacks.remember_local_player_controller(
            remote_controller
        ),
        "remote controllers never replace the local lifecycle cache"
    )
    check(
        api.find_local_player_controller() == owner,
        "rejected remote controller preserves the local cache"
    )
end

_G.__PALHUD_TESTING = nil
_G.__PALHUD_TEST_API = nil

if not loaded then
    original_print("DETAIL " .. tostring(load_error))
end
if failed > 0 then
    os.exit(1)
end
