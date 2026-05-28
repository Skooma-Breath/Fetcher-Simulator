local mp = require("mp")
local config = require("config")
local speechData = require("speech_data")

local M = {}

local TYPE_ORDER = {
    "attack",
    "crattack",
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

local COLLECTION_ORDER = {
    "default",
    "tb",
    "bm",
    "ord",
    "vampire",
}

local lastSpeechAtByGuid = {}
local lastCooldownNoticeAtByGuid = {}

local TYPE_ALIASES = {
    atk = "attack",
    attack = "attack",
    cratk = "crattack",
    crattack = "crattack",
    creatureattack = "crattack",
    flee = "flee",
    fle = "flee",
    follower = "follower",
    flw = "follower",
    hello = "hello",
    hlo = "hello",
    hit = "hit",
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

local COLLECTION_ALIASES = {
    default = "default",
    tb = "tb",
    tribunal = "tb",
    bm = "bm",
    bloodmoon = "bm",
    ord = "ord",
    ordinator = "ord",
    v = "vampire",
    vamp = "vampire",
    vampire = "vampire",
}

local GLOBAL_ALIASES = {
    misc = "misc",
    special = "special",
    extras = "special",
    extra = "special",
    ww = "werewolf",
    wolf = "werewolf",
    werewolf = "werewolf",
}

local function trim(text)
    if not text then
        return ""
    end

    return (tostring(text):gsub("^%s+", ""):gsub("%s+$", ""))
end

local function lower(text)
    return string.lower(trim(text))
end

local function canonicalType(value)
    return TYPE_ALIASES[lower(value)]
end

local function playerRace(player)
    return lower(player and player.race or "")
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

local function splitSpeechInput(input)
    local value = lower(input)
    if value == "" then
        return nil
    end

    local globalType = GLOBAL_ALIASES[value]
    if globalType then
        return "global", globalType
    end

    local underscore = value:find("_", 1, true)
    if underscore and underscore > 1 then
        local collection = COLLECTION_ALIASES[value:sub(1, underscore - 1)]
        local speechType = canonicalType(value:sub(underscore + 1))
        if collection and speechType then
            return collection, speechType
        end
        return nil
    end

    local speechType = canonicalType(value)
    if speechType then
        return "default", speechType
    end

    return nil
end

local function sanitizeVoicePath(path)
    path = trim(path):gsub("/", "\\")
    if path == "" then
        return nil
    end

    local lowered = string.lower(path)
    if lowered:sub(1, 6) == "sound\\" then
        path = path:sub(7)
        lowered = string.lower(path)
    end

    if lowered:find("%.%.", 1, true) then
        return nil
    end

    if lowered:sub(1, 3) ~= "vo\\" or lowered:sub(-4) ~= ".mp3" then
        return nil
    end

    return path
end

local function clientVoicePath(path)
    if not path or path == "" then
        return nil
    end

    local normalized = path:gsub("/", "\\")
    if string.lower(normalized):sub(1, 6) == "sound\\" then
        return normalized
    end

    if string.lower(normalized):sub(1, 3) == "vo\\" then
        return "Sound\\" .. normalized
    end

    return normalized
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

local function entryPathById(entries, requestedId)
    if not entries then
        return nil
    end

    local raw = lower(requestedId)
    if raw == "" then
        return nil
    end

    local requestedNumber = parsePositiveInteger(raw)
    for _, entry in ipairs(entries) do
        local id = entry[1]
        if id == raw then
            return entry[2]
        end

        if requestedNumber and tonumber(id) == requestedNumber then
            return entry[2]
        end
    end

    return nil
end

local function globalEntryPath(entries, requestedIndex)
    local index = parsePositiveInteger(requestedIndex)
    if not index then
        return nil
    end

    local entry = entries and entries[index]
    return entry and entry[2] or nil
end

local function speechCooldownKey(player)
    return tostring(player and player.guid or player and player.name or "")
end

local function speechCooldownSeconds()
    local value = tonumber(config.SPEECH_COOLDOWN_SECONDS or 0) or 0
    if value <= 0 then
        return 0
    end
    return value
end

local function speechCooldownNoticeSeconds()
    local value = tonumber(config.SPEECH_COOLDOWN_NOTICE_SECONDS or 1.5) or 1.5
    if value < 0 then
        return 0
    end
    return value
end

local function canPlaySpeechNow(player)
    local cooldown = speechCooldownSeconds()
    if cooldown <= 0 then
        return true
    end

    if type(mp.getUptime) ~= "function" then
        return true
    end

    local now = mp.getUptime()
    if type(now) ~= "number" then
        return true
    end

    local key = speechCooldownKey(player)
    local last = lastSpeechAtByGuid[key]
    if last and now - last < cooldown then
        local lastNotice = lastCooldownNoticeAtByGuid[key] or 0
        if now - lastNotice >= speechCooldownNoticeSeconds() then
            local remaining = math.max(cooldown - (now - last), 0)
            player:sendMessage(string.format("Speech is on cooldown for %.1fs.", remaining))
            lastCooldownNoticeAtByGuid[key] = now
        end
        return false
    end

    return true
end

local function markSpeechPlayed(player)
    if speechCooldownSeconds() <= 0 then
        return
    end

    if type(mp.getUptime) ~= "function" then
        return
    end

    local now = mp.getUptime()
    if type(now) == "number" then
        lastSpeechAtByGuid[speechCooldownKey(player)] = now
    end
end

local function resolveSpeechPath(player, speechInput, speechIndex)
    local input = lower(speechInput)
    if input == "file" or input == "path" then
        return sanitizeVoicePath(speechIndex)
    end

    local collection, speechType = splitSpeechInput(input)
    if not collection then
        return nil
    end

    if collection == "global" then
        return globalEntryPath(speechData.global[speechType], speechIndex)
    end

    local race = playerRace(player)
    local gender = playerGender(player)
    local raceData = speechData.races[race]
    local entries = raceData
        and raceData[collection]
        and raceData[collection][gender]
        and raceData[collection][gender][speechType]

    return entryPathById(entries, speechIndex)
end

local function compactRanges(numbers)
    if #numbers == 0 then
        return ""
    end

    table.sort(numbers)
    local ranges = {}
    local start = numbers[1]
    local previous = numbers[1]

    for index = 2, #numbers do
        local current = numbers[index]
        if current == previous + 1 then
            previous = current
        else
            ranges[#ranges + 1] = start == previous and tostring(start) or (tostring(start) .. "-" .. tostring(previous))
            start = current
            previous = current
        end
    end

    ranges[#ranges + 1] = start == previous and tostring(start) or (tostring(start) .. "-" .. tostring(previous))
    return table.concat(ranges, ",")
end

local function describeEntries(entries, positional)
    if not entries or #entries == 0 then
        return nil
    end

    if positional then
        return "1-" .. tostring(#entries)
    end

    local numbers = {}
    local extras = {}
    for _, entry in ipairs(entries) do
        local id = entry[1]
        if id:match("^%d+$") then
            numbers[#numbers + 1] = tonumber(id)
        else
            extras[#extras + 1] = id
        end
    end

    local description = compactRanges(numbers)
    if #extras > 0 then
        table.sort(extras)
        description = description .. " +" .. table.concat(extras, ",")
    end

    return description
end

local function appendCollectionHelp(parts, raceData, gender, collection)
    local genderData = raceData
        and raceData[collection]
        and raceData[collection][gender]

    if not genderData then
        return
    end

    local seen = {}
    local prefix = collection == "default" and "" or (collection .. "_")

    for _, speechType in ipairs(TYPE_ORDER) do
        local description = describeEntries(genderData[speechType], false)
        if description then
            parts[#parts + 1] = prefix .. speechType .. " " .. description
            seen[speechType] = true
        end
    end

    for speechType, entries in pairs(genderData) do
        if not seen[speechType] then
            local description = describeEntries(entries, false)
            if description then
                parts[#parts + 1] = prefix .. speechType .. " " .. description
            end
        end
    end
end

local function printableValidList(player)
    local raceData = speechData.races[playerRace(player)]
    local gender = playerGender(player)
    local parts = {}

    for _, collection in ipairs(COLLECTION_ORDER) do
        appendCollectionHelp(parts, raceData, gender, collection)
    end

    local misc = describeEntries(speechData.global.misc, true)
    local special = describeEntries(speechData.global.special, true)
    local werewolf = describeEntries(speechData.global.werewolf, true)
    if misc then
        parts[#parts + 1] = "misc " .. misc
    end
    if special then
        parts[#parts + 1] = "special " .. special
    end
    if werewolf then
        parts[#parts + 1] = "werewolf " .. werewolf
    end

    parts[#parts + 1] = "file <Vo\\...mp3>"
    return table.concat(parts, ", ")
end

local function sendSpeechHelp(player)
    player:sendMessage("Speech: /speech <type> <index> or /s <type> <index>")
    player:sendMessage("Valid speech: " .. printableValidList(player))
end

function M.handleChat(player, data, env)
    local commandPrefix = env.commandPrefix or "/"
    local msg = data.message or ""
    if msg:sub(1, #commandPrefix) ~= commandPrefix then
        return nil
    end

    local args, parseError = env.parseCommandArgs(msg:sub(#commandPrefix + 1))
    if not args then
        player:sendMessage(parseError or "Invalid arguments.")
        return false
    end

    local command = lower(args[1])
    if command == "speechhelp" then
        sendSpeechHelp(player)
        return false
    end

    if command ~= "speech" and command ~= "s" then
        return nil
    end

    local path
    if args[2] == "file" or args[2] == "path" then
        path = resolveSpeechPath(player, args[2], args[3])
    elseif args[2] and args[3] then
        path = resolveSpeechPath(player, args[2], args[3])
    end

    if not path then
        player:sendMessage("That is not a valid speech. Try one of the following:")
        player:sendMessage(printableValidList(player))
        return false
    end

    if not canPlaySpeechNow(player) then
        return false
    end

    local soundPath = clientVoicePath(path)
    if not mp.playSpeech(player.guid, soundPath) then
        player:sendMessage("Could not play speech for your character right now.")
        return false
    end

    markSpeechPlayed(player)
    mp.log(string.format("[speech] %s played %s", player.name, soundPath))
    return false
end

return M
