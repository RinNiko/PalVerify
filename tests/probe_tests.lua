package.path = "packaging/server/Scripts/?.lua;"
    .. "packaging/server/Scripts/?/init.lua;"
    .. package.path

local SymbolFilter = require("palverify.symbol_filter")

local passed = 0
local failed = 0

local function equal(actual, expected, message)
    if actual ~= expected then
        error(string.format(
            "%s: expected %s, got %s",
            message or "values differ",
            tostring(expected),
            tostring(actual)
        ), 2)
    end
end

local function test(name, callback)
    local ok, err = pcall(callback)
    if ok then
        passed = passed + 1
        print("PASS " .. name)
    else
        failed = failed + 1
        print("FAIL " .. name .. ": " .. tostring(err))
    end
end

test("platform and identity-related symbols are candidates", function()
    equal(SymbolFilter.is_candidate("SupportedPlatformType"), true)
    equal(SymbolFilter.is_candidate("GetPlatformUserId"), true)
    equal(SymbolFilter.is_candidate("EOSProductUserId"), true)
    equal(SymbolFilter.is_candidate("OnlineAccountId"), true)
    equal(SymbolFilter.is_candidate("DeviceFamily"), true)
end)

test("unrelated gameplay symbols are ignored", function()
    equal(SymbolFilter.is_candidate("GetPlayerName"), false)
    equal(SymbolFilter.is_candidate("StomachDecreace"), false)
    equal(SymbolFilter.is_candidate("GetVelocity"), false)
end)

test("candidate matching is case insensitive", function()
    equal(SymbolFilter.is_candidate("PLATFORM"), true)
    equal(SymbolFilter.is_candidate("uniqueNetId"), true)
end)

test("collection is sorted and de-duplicated", function()
    local symbols = SymbolFilter.collect({
        "GetVelocity",
        "PlatformType",
        "OnlineAccountId",
        "PlatformType",
        "DeviceFamily",
    })

    equal(#symbols, 3)
    equal(symbols[1], "DeviceFamily")
    equal(symbols[2], "OnlineAccountId")
    equal(symbols[3], "PlatformType")
end)

print(string.format("%d passed, %d failed", passed, failed))
if failed > 0 then
    os.exit(1)
end
