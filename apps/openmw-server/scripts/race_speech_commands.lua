local speechCommands = require("speech_commands")

local M = {}

local TYPE_ORDER = {
    "attack",
    "alarm",
    "flee",
    "follower",
    "hello",
    "hit",
    "idle",
    "intruder",
    "oppose",
    "service",
    "thief",
    "uniform",
}

local TYPE_ALIASES = {
    alarm = "alarm",
    alr = "alarm",
    atk = "attack",
    attack = "attack",
    flee = "flee",
    fle = "flee",
    follower = "follower",
    flw = "follower",
    hello = "hello",
    hlo = "hello",
    hit = "hit",
    hurt = "hit",
    pain = "hit",
    idle = "idle",
    idl = "idle",
    intruder = "intruder",
    int = "intruder",
    oppose = "oppose",
    op = "oppose",
    service = "service",
    srv = "service",
    thief = "thief",
    thf = "thief",
    uniform = "uniform",
    uni = "uniform",
}

local function trim(value)
    if value == nil then
        return ""
    end
    return (tostring(value):gsub("^%s+", ""):gsub("%s+$", ""))
end

local function lower(value)
    return string.lower(trim(value))
end

local function playerGender(player)
    if player and player.gender == 0 then
        return "female"
    end
    if player and player.isMale == false then
        return "female"
    end
    return "male"
end

local function parsePositiveInteger(value)
    local raw = trim(value)
    if raw == "" or not raw:match("^%d+$") then
        return nil
    end
    local number = tonumber(raw)
    if not number or number < 1 then
        return nil
    end
    return math.floor(number)
end

local function entryPath(entries, requestedIndex)
    local index = parsePositiveInteger(requestedIndex)
    local entry = index and entries and entries[index]
    return entry and entry[2] or nil
end

local function raceEntries(data, player)
    local race = lower(player and player.race)
    local raceData = data.races and data.races[race]
    return raceData and raceData[playerGender(player)], race
end

local function availableTypes(entries)
    local parts = {}
    local seen = {}
    for _, speechType in ipairs(TYPE_ORDER) do
        local values = entries and entries[speechType]
        if values and #values > 0 then
            parts[#parts + 1] = speechType .. " 1-" .. tostring(#values)
            seen[speechType] = true
        end
    end
    if entries then
        local extras = {}
        for speechType, values in pairs(entries) do
            if not seen[speechType] and values and #values > 0 then
                extras[#extras + 1] = speechType
            end
        end
        table.sort(extras)
        for _, speechType in ipairs(extras) do
            parts[#parts + 1] = speechType .. " 1-" .. tostring(#entries[speechType])
        end
    end
    return table.concat(parts, ", ")
end

function M.new(options)
    assert(type(options) == "table", "race speech command options are required")
    assert(type(options.data) == "table", "race speech data is required")

    local commands = {}
    for _, command in ipairs(options.commands or {}) do
        commands[lower(command)] = true
    end
    local helpCommands = {}
    for _, command in ipairs(options.helpCommands or {}) do
        helpCommands[lower(command)] = true
    end

    local label = options.label or "Race speech"
    local usageCommand = options.usageCommand or "/speech"
    local logTag = options.logTag or "race-speech"
    local handler = {}

    local function sendHelp(player)
        local entries, race = raceEntries(options.data, player)
        player:sendMessage(string.format("%s: %s <type> <index>", label, usageCommand))
        if not entries then
            player:sendMessage(string.format("No dedicated %s lines are available for race '%s'.", label:lower(), race))
            return
        end
        player:sendMessage("Valid speech: " .. availableTypes(entries))
    end

    function handler.handleChat(player, data, env)
        local commandPrefix = env.commandPrefix or "/"
        local message = data.message or ""
        if message:sub(1, #commandPrefix) ~= commandPrefix then
            return nil
        end

        local args, parseError = env.parseCommandArgs(message:sub(#commandPrefix + 1))
        if not args then
            player:sendMessage(parseError or "Invalid arguments.")
            return false
        end

        local command = lower(args[1])
        if helpCommands[command] then
            sendHelp(player)
            return false
        end
        if not commands[command] then
            return nil
        end

        local entries = raceEntries(options.data, player)
        local speechType = TYPE_ALIASES[lower(args[2])] or lower(args[2])
        local path = entryPath(entries and entries[speechType], args[3])
        if not path then
            player:sendMessage("That is not a valid speech for your current race. Try one of the following:")
            local valid = availableTypes(entries)
            player:sendMessage(valid ~= "" and valid or "No dedicated lines are available.")
            return false
        end

        return speechCommands.playPath(player, path, logTag)
    end

    return handler
end

return M
