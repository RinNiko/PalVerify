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

local stale_get_calls = 0
local stale_compass = {
    get = function()
        stale_get_calls = stale_get_calls + 1
        error("UObject instance is nullptr")
    end,
    IsValid = function()
        return false
    end,
}

FindFirstOf = function(class_name)
    if class_name == "WBP_Ingame_Compass_C" then
        return stale_compass
    end
    return nil
end
FindAllOf = function()
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
    local controller = api.find_local_player_controller()
    check(controller == nil, "stale compass does not resolve a controller")
    check(
        stale_get_calls == 0,
        "direct FindFirstOf UObject is never unwrapped through get()"
    )

    local valid = api.is_valid({
        IsValid = function()
            error("UObject instance is nullptr")
        end,
    })
    check(valid == false, "IsValid errors reject stale UObjects")

    local owner_get_calls = 0
    local owner = {
        get = function()
            owner_get_calls = owner_get_calls + 1
            error("direct UObject must not be unwrapped")
        end,
        IsValid = function()
            return true
        end,
        IsLocalPlayerController = function()
            return true
        end,
    }
    FindFirstOf = function()
        return {
            IsValid = function()
                return true
            end,
            GetOwningPlayer = function()
                return owner
            end,
        }
    end
    controller = api.find_local_player_controller()
    check(controller == owner, "valid compass resolves its local controller")
    check(
        owner_get_calls == 0,
        "direct method-returned UObject is never unwrapped through get()"
    )

    local listed_get_calls = 0
    local listed_controller = {
        get = function()
            listed_get_calls = listed_get_calls + 1
            error("direct UObject must not be unwrapped")
        end,
        IsValid = function()
            return true
        end,
        IsLocalPlayerController = function()
            return true
        end,
    }
    FindFirstOf = function()
        return nil
    end
    FindAllOf = function()
        return { listed_controller }
    end
    controller = api.find_local_player_controller()
    check(
        controller == listed_controller,
        "FindAllOf resolves a direct local controller"
    )
    check(
        listed_get_calls == 0,
        "direct FindAllOf UObject is never unwrapped through get()"
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
