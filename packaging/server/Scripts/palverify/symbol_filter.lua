local SymbolFilter = {}

local keywords = {
    "platform",
    "online",
    "account",
    "unique",
    "netid",
    "eos",
    "device",
}

function SymbolFilter.is_candidate(name)
    if type(name) ~= "string" then
        return false
    end

    local normalized = string.lower(name)
    for _, keyword in ipairs(keywords) do
        if string.find(normalized, keyword, 1, true) ~= nil then
            return true
        end
    end

    return false
end

function SymbolFilter.collect(names)
    local unique = {}

    for _, name in ipairs(names or {}) do
        if SymbolFilter.is_candidate(name) then
            unique[name] = true
        end
    end

    local result = {}
    for name in pairs(unique) do
        table.insert(result, name)
    end
    table.sort(result)
    return result
end

return SymbolFilter
