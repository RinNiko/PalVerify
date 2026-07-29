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

local function near(actual, expected)
    return math.abs((tonumber(actual) or -999) - expected) < 0.0001
end

_G.__PALHUD_TESTING = true
local loaded, load_error = pcall(
    dofile,
    "third_party/UE4SSExperimentalPW/Mods/PalHud/Scripts/main.lua"
)
check(loaded, "PalHud combat test harness loads")

local api = _G.__PALHUD_TEST_API
check(type(api) == "table", "PalHud exposes combat helpers")
check(
    type(api) == "table"
        and type(api.parse_combat_protocol) == "function"
        and type(api.combat_view) == "function",
    "PalHud exposes pure combat protocol and view helpers"
)

if type(api) == "table"
    and type(api.parse_combat_protocol) == "function"
    and type(api.combat_view) == "function"
then
    local state = api.parse_combat_protocol(
        "[PALCOMBAT]|1|10|10",
        100
    )
    check(state.active == true, "active combat protocol is accepted")

    local view = api.combat_view(state, 100)
    check(view.active == true, "combat block is visible immediately")
    check(
        view.text == "CHIẾN ĐẤU • KHÔNG THOÁT GAME • 10s",
        "combat block shows the full countdown"
    )
    check(near(view.progress, 1), "combat bar starts full")

    view = api.combat_view(state, 104)
    check(
        view.text == "CHIẾN ĐẤU • KHÔNG THOÁT GAME • 6s",
        "combat countdown decreases locally every second"
    )
    check(near(view.progress, 0.6), "combat bar drains with countdown")

    state = api.parse_combat_protocol(
        "[PALCOMBAT]|1|10|10",
        104
    )
    view = api.combat_view(state, 104)
    check(
        view.text == "CHIẾN ĐẤU • KHÔNG THOÁT GAME • 10s",
        "a refreshed server tag refills the combat bar"
    )

    view = api.combat_view(state, 115)
    check(view.active == false, "expired combat hides without chat")
    check(near(view.progress, 0), "expired combat bar is empty")

    state = api.parse_combat_protocol(
        "[PALCOMBAT]|0|0|10",
        105
    )
    view = api.combat_view(state, 105)
    check(view.active == false, "inactive protocol clears combat block")

    check(
        api.parse_combat_protocol("[PALCOMBAT]|1|bad|10", 100) == nil,
        "malformed combat protocol is ignored"
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
