local mp = require("mp")
local adminUiService = require("admin_ui_service")
local bardcraftNetworkPolicy = require("bardcraft_network_policy")
local commandRegistry = require("command_registry")
local Config = require("config")
local types = require("openmw.types")
local intentPolicy = require("intent_policy")
local markRecallCommands = require("mark_recall")
local recordDynamicTest = require("recorddynamic_test")
local recordStore = require("recordstore")
local destructibleSpawners = require("destructible_spawners")
local speechCommands = require("speech_commands")
local surfCommands = require("surf_commands")
local surfTimer = require("surf_timer")

------------------------------------------------------------------------
-- Config
------------------------------------------------------------------------
local ADMIN_PASSWORD = Config.ADMIN_PASSWORD or "changeme"
local MAX_PLAYERS = 32
local MOTD = Config.MOTD or "Welcome to the server!  Type /help for commands."
local COMMAND_PREFIX = Config.COMMAND_PREFIX or "/"
local ANNOUNCE_JOIN_LEAVE = Config.ANNOUNCE_JOIN_LEAVE ~= false
local LOG_CHAT = Config.LOG_CHAT == true
local ALLOW_UNVERIFIED_ACTIVATE = Config.ALLOW_UNVERIFIED_ACTIVATE == true
local LOGIN_PREFIX = COMMAND_PREFIX .. "login "
local KICK_PREFIX = COMMAND_PREFIX .. "kick "
local DELETE_PREFIX = COMMAND_PREFIX .. "delete "
local RESETCELL_PREFIX = COMMAND_PREFIX .. "resetcell "
local MPWHERE_PREFIX = COMMAND_PREFIX .. "mpwhere "
local TOMP_PREFIX = COMMAND_PREFIX .. "tomp "
local BRINGMP_PREFIX = COMMAND_PREFIX .. "bringmp "
local TPTO_PREFIX = COMMAND_PREFIX .. "tpto "
local TP_PREFIX = COMMAND_PREFIX .. "tp "
local PLACEAT_PREFIX = COMMAND_PREFIX .. "placeat "
local SPAWNAT_PREFIX = COMMAND_PREFIX .. "spawnat "
local SETTIME_PREFIX = COMMAND_PREFIX .. "settime "
local LUABROADCAST_PREFIX = COMMAND_PREFIX .. "luabroadcast "
local LUACELL_PREFIX = COMMAND_PREFIX .. "luacell "
local LUASEND_PREFIX = COMMAND_PREFIX .. "luasend "
local LUAPING_PREFIX = COMMAND_PREFIX .. "luaping "
local LUABURST_PREFIX = COMMAND_PREFIX .. "luaburst "
local LUASTORAGE_PREFIX = COMMAND_PREFIX .. "luastorage "
local LUATYPES_PREFIX = COMMAND_PREFIX .. "luatypes"
local SUICIDE_PREFIX = COMMAND_PREFIX .. "suicide "

------------------------------------------------------------------------
-- State
------------------------------------------------------------------------
local admins = {}
local tickAccum = 0
local luaTestSeq = 0
local doorStateCache = {}

------------------------------------------------------------------------
-- Helpers
------------------------------------------------------------------------
local function getPlayer(guid)
    if not guid then
        return nil
    end
    return mp.getPlayer(guid)
end

local function findPlayerByName(name)
    for _, p in ipairs(mp.getPlayers()) do
        if p.name == name then
            return p
        end
    end
    return nil
end

local function isAdmin(player)
    return admins[player.guid] == true
end

local function requireAdmin(player)
    if isAdmin(player) then
        return true
    end

    player:sendMessage("Permission denied.")
    return false
end

local function nextLuaTestSeq()
    luaTestSeq = luaTestSeq + 1
    return luaTestSeq
end

local function trim(text)
    if not text then
        return nil
    end

    return (text:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function lower(text)
    if not text then
        return ""
    end

    return string.lower(trim(text))
end

local function parseCommandArgs(text)
    text = trim(text) or ""
    local args = {}
    local length = #text
    local index = 1

    while index <= length do
        while index <= length and text:sub(index, index):match("%s") do
            index = index + 1
        end

        if index > length then
            break
        end

        local current = text:sub(index, index)
        if current == "\"" then
            local token = {}
            local closed = false
            index = index + 1

            while index <= length do
                current = text:sub(index, index)

                if current == "\\" and index < length then
                    local nextChar = text:sub(index + 1, index + 1)
                    if nextChar == "\"" or nextChar == "\\" then
                        table.insert(token, nextChar)
                        index = index + 2
                    else
                        table.insert(token, current)
                        index = index + 1
                    end
                elseif current == "\"" then
                    closed = true
                    index = index + 1
                    break
                else
                    table.insert(token, current)
                    index = index + 1
                end
            end

            if not closed then
                return nil, "Missing closing double quote."
            end

            table.insert(args, table.concat(token))
        else
            local startIndex = index
            while index <= length and not text:sub(index, index):match("%s") do
                index = index + 1
            end

            table.insert(args, text:sub(startIndex, index - 1))
        end
    end

    return args
end

local function normalizeCellId(cellId)
    cellId = trim(cellId)
    if not cellId or cellId == "" then
        return cellId
    end

    if (cellId:sub(1, 1) == "\"" and cellId:sub(-1) == "\"")
        or (cellId:sub(1, 1) == "'" and cellId:sub(-1) == "'") then
        cellId = trim(cellId:sub(2, -2))
    end

    local x, y = cellId:match("^[Ee][Xx][Tt]:%s*(-?%d+)%s*,%s*(-?%d+)$")
    if x and y then
        return string.format("EXT:%d,%d", tonumber(x), tonumber(y))
    end

    x, y = cellId:match("^%s*(-?%d+)%s*,%s*(-?%d+)$")
    if x and y then
        return string.format("EXT:%d,%d", tonumber(x), tonumber(y))
    end

    return cellId
end

local function makeLuaTestPayload(kind, player, extra)
    local payload = {
        seq = nextLuaTestSeq(),
        kind = kind,
        fromGuid = player.guid,
        fromName = player.name,
        fromCell = player.cell,
        serverHour = mp.getWorldTime(),
    }

    if extra then
        for key, value in pairs(extra) do
            payload[key] = value
        end
    end

    return payload
end

local function playerList()
    local names = {}
    for _, p in ipairs(mp.getPlayers()) do
        table.insert(names, p.name)
    end
    return table.concat(names, ", ")
end

local function sendBardcraftCommunityStatus(player)
    local policy = bardcraftNetworkPolicy.get()
    player:sendMessage(string.format(
        "[Bardcraft] Community mode=%s, server MIDI downloads=%s, player uploads=%s, live relay fallback=%s.",
        policy.communitySongSharingMode and "on" or "off",
        policy.allowServerHostedMidiDownloads and "on" or "off",
        policy.allowPlayerSongUpload and "on" or "off",
        policy.allowImportedMidiLiveRelayFallback and "on" or "off"))
end

local function broadcastBardcraftNetworkPolicy(player)
    local sent = 0
    for _, target in ipairs(mp.getPlayers()) do
        local guid = tonumber(target and target.guid)
        if guid then
            mp.send(guid, "BC_BardcraftNetworkPolicy", bardcraftNetworkPolicy.applyFields({
                networkPolicy = bardcraftNetworkPolicy.copy(),
                reason = "runtime-community-mode",
                changedBy = tostring(player.name),
            }))
            sent = sent + 1
        end
    end
    return sent
end

local function handleBardcraftCommunityCommand(player, action)
    if not requireAdmin(player) then
        return
    end

    if action == "on" or action == "off" then
        local policy = bardcraftNetworkPolicy.setRuntimeCommunityMode(action == "on")
        local sent = broadcastBardcraftNetworkPolicy(player)
        mp.log(string.format(
            "[bardcraft] runtime community mode enabled=%s hostedDownloads=%s playerUpload=%s importedRelayFallback=%s changedBy=%s notified=%d",
            tostring(policy.communitySongSharingMode),
            tostring(policy.allowServerHostedMidiDownloads),
            tostring(policy.allowPlayerSongUpload),
            tostring(policy.allowImportedMidiLiveRelayFallback),
            tostring(player.name),
            sent))
        sendBardcraftCommunityStatus(player)
    elseif action == "" or action == "status" then
        sendBardcraftCommunityStatus(player)
    else
        player:sendMessage("Usage: " .. COMMAND_PREFIX .. "bccommunity on|off|status")
    end
end

local function playerListWithPids()
    local entries = {}
    for _, p in ipairs(mp.getPlayers()) do
        table.insert(entries, string.format("[%d] %s", tonumber(p.guid) or 0, p.name))
    end
    return table.concat(entries, ", ")
end

local function findPlayerByPidOrName(value)
    value = trim(value)
    if not value or value == "" then
        return nil
    end

    local pid = tonumber(value)
    if pid then
        pid = math.floor(pid)
        for _, p in ipairs(mp.getPlayers()) do
            if tonumber(p.guid) == pid then
                return p
            end
        end
    end

    local wanted = lower(value)
    for _, p in ipairs(mp.getPlayers()) do
        if lower(p.name) == wanted then
            return p
        end
    end

    return nil
end

local function resolvePlayerCommandTarget(player, args, usage)
    if not args or #args == 0 then
        player:sendMessage("Usage: " .. usage)
        return nil
    end

    local value = table.concat(args, " ")
    local target = findPlayerByPidOrName(value)
    if not target then
        player:sendMessage("Player not found: " .. value)
    end
    return target
end

local function broadcastNameColor(text)
    if mp.broadcastNameColor then
        mp.broadcastNameColor(text)
    else
        mp.broadcast(text)
    end
end

local function distance3(a, b)
    if not a or not b then
        return nil
    end

    local dx = (a.x or 0) - (b.x or 0)
    local dy = (a.y or 0) - (b.y or 0)
    local dz = (a.z or 0) - (b.z or 0)
    return math.sqrt(dx * dx + dy * dy + dz * dz)
end

local function playerPosition(player)
    local pos = player.position or {}
    return {
        x = pos.x or 0,
        y = pos.y or 0,
        z = pos.z or 0,
    }
end

local function toPositiveInteger(value)
    local number = tonumber(value)
    if not number or number <= 0 then
        return nil
    end
    return math.floor(number)
end

local function toNonNegativeNumber(value)
    local number = tonumber(value)
    if not number or number < 0 then
        return nil
    end
    return number
end

local function toPlaceDirection(value)
    local number = tonumber(value)
    if not number then
        return nil
    end

    local direction = math.floor(number)
    if direction < 0 or direction > 3 then
        return nil
    end

    return direction
end

local function plainPosition(position)
    position = position or {}
    return {
        x = tonumber(position.x) or 0,
        y = tonumber(position.y) or 0,
        z = tonumber(position.z) or 0,
        rx = tonumber(position.rx) or 0,
        ry = tonumber(position.ry) or 0,
        rz = tonumber(position.rz) or 0,
    }
end

local function teleportPlayerToPlayer(actor, target)
    local cellId = normalizeCellId(target.cell)
    if not cellId or cellId == "" then
        return false, "Target has no valid cell."
    end

    local position = plainPosition(target.position)
    position.isTeleporting = true
    if not mp.teleportPlayer(actor.guid, cellId, position) then
        return false, "Failed to queue teleport."
    end

    return true, cellId
end

local function placeAtPosition(player, distance, direction)
    local position = plainPosition(player.position)
    local yaw = position.rz or 0
    local sinYaw = math.sin(yaw)
    local cosYaw = math.cos(yaw)

    if direction == 0 then
        position.x = position.x + sinYaw * distance
        position.y = position.y + cosYaw * distance
    elseif direction == 1 then
        position.x = position.x - sinYaw * distance
        position.y = position.y - cosYaw * distance
    elseif direction == 2 then
        position.x = position.x - cosYaw * distance
        position.y = position.y + sinYaw * distance
    elseif direction == 3 then
        position.x = position.x + cosYaw * distance
        position.y = position.y - sinYaw * distance
    end

    return position
end

local function sendPlaceAtUsage(player)
    player:sendMessage("Usage: /placeat <refId|\"ref id\"> [count] [distance] [direction]")
    player:sendMessage("Quote refIds that contain spaces, for example /placeat \"daedric mace\"")
    player:sendMessage("direction: 0=front, 1=back, 2=left, 3=right")
end

local function sendSpawnAtUsage(player)
    player:sendMessage("Usage: /spawnat <refId|\"ref id\"> [distance] [direction] [refNum] [mpNum] [persistent|session]")
    player:sendMessage("Quote refIds that contain spaces, for example /spawnat \"fargoth\"")
    player:sendMessage("direction: 0=front, 1=back, 2=left, 3=right")
    player:sendMessage("refNum defaults to 0; mpNum defaults to server-assigned")
end

local function sendBringMpUsage(player)
    player:sendMessage("Usage: /bringmp <mpNum> [distance] [direction]")
    player:sendMessage("direction: 0=front, 1=back, 2=left, 3=right")
end

local function resolveMpTarget(mpNum)
    local actor = mp.getActorByMpNum and mp.getActorByMpNum(mpNum) or nil
    if actor then
        return "actor", actor
    end

    local object = mp.getObjectByMpNum(mpNum)
    if object then
        return "object", object
    end

    return nil, nil
end

local function positionSummary(position)
    position = plainPosition(position)
    return string.format("%.1f, %.1f, %.1f", position.x, position.y, position.z)
end

local function sendMpTargetSummary(player, kind, target)
    player:sendMessage(string.format(
        "mpNum=%d %s refId=%s cell=%s pos=(%s)%s",
        target.mpNum,
        kind,
        tostring(target.refId),
        tostring(target.cell or target.cellId or ""),
        positionSummary(target.position),
        target.isDead and " dead" or ""
    ))
end

local function resolveVerifiedTarget(object)
    object = object or {}

    local mpNum = toPositiveInteger(object.mpNum)
    if not mpNum then
        return nil
    end

    local placed = mp.getObjectByMpNum(mpNum)
    if not placed then
        return nil
    end

    return {
        action = "take",
        source = "server-placed-object",
        object = {
            id = object.id,
            mpNum = placed.mpNum,
            recordId = placed.refId,
            type = object.type,
            typeName = object.typeName,
            cell = placed.cell,
            position = plainPosition(placed.position),
            count = placed.count,
        },
    }
end

local function doorKey(cellId, refId, objectId)
    return string.format("%s:%s:%s", tostring(cellId or ""), tostring(refId or ""), tostring(objectId or ""))
end

local itemTypeNames = {
    Apparatus = true,
    Armor = true,
    Book = true,
    Clothing = true,
    ESM4Book = true,
    Ingredient = true,
    Light = true,
    Lockpick = true,
    MiscItem = true,
    Potion = true,
    Probe = true,
    Repair = true,
    Weapon = true,
}

local function classifyActivation(object)
    local typeName = tostring(object.typeName or "")
    local recordId = tostring(object.recordId or ""):lower()

    if typeName == "Door" or typeName == "ESM4Door" then
        return "door", "payload-type"
    end

    if recordId:find("door", 1, true) then
        return "door", "record-id"
    end

    if itemTypeNames[typeName] then
        return "take", "payload-type"
    end

    if typeName == "Container" then
        return "container", "payload-type"
    end

    if typeName == "NPC" or typeName == "Creature" or typeName == "Player" then
        return "actor", "payload-type"
    end

    if typeName == "Activator" then
        return "activate", "payload-type"
    end

    return "activate", "fallback"
end

local function enrichActivationResult(result, object, accepted, forcedAction, forcedSource)
    local action, source
    if forcedAction then
        action = forcedAction
        source = forcedSource or "server-verified"
    else
        action, source = classifyActivation(object)
    end
    result.action = action
    result.classificationSource = source
    result.object = {
        id = object.id,
        mpNum = object.mpNum,
        recordId = object.recordId,
        type = object.type,
        typeName = object.typeName,
        cell = object.cell,
        position = plainPosition(object.position),
    }

    if not accepted then
        return
    end

    if action == "door" then
        local exactKey = doorKey(object.cell, object.recordId, object.id)
        local refKey = doorKey(object.cell, object.recordId, "")
        local current = doorStateCache[exactKey]
        if current == nil then
            current = doorStateCache[refKey]
        end
        result.doorKey = exactKey
        result.stateKnown = current ~= nil
        result.stateSource = current ~= nil and "lua-door-cache-last-known" or "unknown"
        result.stateTiming = "last-known"
        if current ~= nil then
            result.state = current and "open" or "closed"
        else
            result.state = "unknown"
        end
        result.mutation = "door-state-packet"
    elseif action == "take" then
        result.mutation = "origin-client-object-delete"
    elseif action == "container" then
        result.mutation = "container-authority-pending"
    elseif action == "actor" then
        result.mutation = "interaction-authority-pending"
    else
        result.mutation = "standard-activation"
    end
end

local function buildActivateIntentContext(data)
    data = data or {}
    local player = getPlayer(data.pid)
    if not player then
        return {
            data = data,
            player = nil,
            actor = data.actor or {},
            object = data.object or {},
            verifiedTarget = nil,
            result = {
                seq = data.seq,
                clientActivationId = data.clientActivationId,
                accepted = false,
                reason = "missing_player",
                serverVerified = false,
                originPid = data.pid,
                actorGuid = 0,
                actorName = "",
                actorId = nil,
                objectId = data.object and data.object.id or nil,
                objectMpNum = data.object and data.object.mpNum or nil,
                objectRecordId = data.object and data.object.recordId or nil,
                objectType = data.object and data.object.type or nil,
                objectCell = data.object and data.object.cell or nil,
                verificationSource = "missing-player",
                distance = -1,
                maxDistance = Config.ACTIVATION_MAX_DISTANCE or 250,
                serverHour = mp.getWorldTime(),
                applyStandardAction = false,
                mutation = "intent-missing-player",
            },
        }
    end

    local object = data.object or {}
    local actor = data.actor or {}
    local verifiedTarget = resolveVerifiedTarget(object)
    if verifiedTarget then
        object = verifiedTarget.object
    end
    local maxDistance = Config.ACTIVATION_MAX_DISTANCE or 250
    local distance = distance3(playerPosition(player), object.position)
    local sameCell = not object.cell or object.cell == "" or object.cell == player.cell
    local accepted = distance ~= nil and distance <= maxDistance and sameCell
    local reason = "accepted"
    if distance == nil then
        reason = "missing_position"
    elseif distance > maxDistance then
        reason = "too_far"
    elseif not sameCell then
        reason = "cell_mismatch"
    end

    local serverVerified = verifiedTarget ~= nil or ALLOW_UNVERIFIED_ACTIVATE
    -- Allow activation of base-game actors (NPCs/corpses) and doors even without
    -- mpNum verification, since these are static ESM objects that can't have mpNums.
    -- The distance check still applies for anti-cheat.
    local preAction = classifyActivation(object)
    local isBaseGameActivation = (preAction == "actor" or preAction == "door")
    if accepted and not serverVerified and not isBaseGameActivation then
        accepted = false
        reason = "unverified_target"
    end

    local result = {
        seq = data.seq,
        clientActivationId = data.clientActivationId,
        accepted = accepted,
        reason = reason,
        serverVerified = serverVerified,
        originPid = data.pid,
        actorGuid = player.guid,
        actorName = player.name,
        actorId = actor.id,
        objectId = object.id,
        objectMpNum = object.mpNum,
        objectRecordId = object.recordId,
        objectType = object.type,
        objectCell = object.cell,
        verificationSource = verifiedTarget and verifiedTarget.source or "client-payload",
        distance = distance or -1,
        maxDistance = maxDistance,
        serverHour = mp.getWorldTime(),
        applyStandardAction = accepted,
    }
    enrichActivationResult(
        result,
        object,
        accepted,
        verifiedTarget and verifiedTarget.action or nil,
        verifiedTarget and verifiedTarget.source or nil
    )

    if not result.accepted then
        result.applyStandardAction = false
    end

    return {
        eventName = "Activate",
        data = data,
        player = player,
        actor = actor,
        object = object,
        verifiedTarget = verifiedTarget,
        result = result,
    }
end

intentPolicy.registerIntent("Activate", buildActivateIntentContext)

local function handleActivate(data)
    local context = intentPolicy.evaluate("Activate", data)
    local player = getPlayer(data and data.pid)
    local object = data and data.object or {}

    if context.ops and #context.ops > 0 then
        local ok, err = mp.applyOps(context.ops)
        context.ops = nil
        if not ok then
            context.accepted = false
            context.reason = "intent_ops_failed"
            context.mutation = "intent-ops-failed"
            context.applyStandardAction = false
            context.failedStage = context.failedStage or "ops"
            context.failedError = err
        end
    end

    if player and player.cell and player.cell ~= "" then
        mp.broadcastToCell(player.cell, "ActivateResult", context)
    else
        mp.broadcast("ActivateResult", context)
    end

    mp.log(string.format(
        "[core] Activate seq=%s by %s action=%s object=%s recordId=%s type=%s distance=%.1f accepted=%s verified=%s reason=%s state=%s mutation=%s",
        tostring(data and data.seq),
        tostring(player and player.name or "<missing>"),
        tostring(context.action),
        tostring(object.id),
        tostring(object.recordId),
        tostring(object.typeName or object.type),
        tonumber(context.distance) or -1,
        tostring(context.accepted),
        tostring(context.serverVerified),
        tostring(context.reason),
        tostring(context.state),
        tostring(context.mutation)
    ))
end

local function handleChat(player, data)
    local msg = data.message or ""

    if LOG_CHAT then
        mp.log("[core] Chat [" .. player.name .. "] " .. msg)
    end

    if msg == COMMAND_PREFIX .. "help" then
        commandRegistry.sendHelp(player, COMMAND_PREFIX, isAdmin(player))
        return false
    end

    if msg == COMMAND_PREFIX .. "who" then
        player:sendMessage("Online (" .. mp.getPlayerCount() .. "): " .. playerList())
        return false
    end

    if msg == COMMAND_PREFIX .. "list" then
        player:sendMessage("Online (" .. mp.getPlayerCount() .. "): " .. playerListWithPids())
        return false
    end

    if msg == COMMAND_PREFIX .. "f" then
        broadcastNameColor(player.name .. " pays respects.")
        return false
    end

    if msg == COMMAND_PREFIX .. "time" then
        local h = mp.getWorldTime()
        local hour = math.floor(h)
        local minute = math.floor((h - hour) * 60)
        player:sendMessage(string.format("Server time: %02d:%02d", hour, minute))
        return false
    end

    if msg == COMMAND_PREFIX .. "uptime" then
        local secs = math.floor(mp.getUptime())
        local mins = math.floor(secs / 60)
        local hours = math.floor(mins / 60)
        player:sendMessage(string.format("Uptime: %dh %02dm %02ds", hours, mins % 60, secs % 60))
        return false
    end

    if msg == COMMAND_PREFIX .. "suicide" or msg:sub(1, #SUICIDE_PREFIX) == SUICIDE_PREFIX then
        local deathMessage = msg == COMMAND_PREFIX .. "suicide" and "" or (trim(msg:sub(#SUICIDE_PREFIX + 1)) or "")
        if not mp.killPlayer(player.guid, deathMessage) then
            player:sendMessage("Failed to kill player.")
        end
        return false
    end

    if msg:sub(1, #LOGIN_PREFIX) == LOGIN_PREFIX then
        local pw = msg:sub(#LOGIN_PREFIX + 1)
        if pw == ADMIN_PASSWORD then
            admins[player.guid] = true
            player:sendMessage("You are now an admin.")
            mp.log("[core] Admin login: " .. player.name)
        else
            player:sendMessage("Wrong password.")
            mp.log("[core] Failed admin login attempt: " .. player.name)
        end
        return false
    end

    local bardcraftCommunityCommand = COMMAND_PREFIX .. "bccommunity"
    if msg == bardcraftCommunityCommand
        or msg:sub(1, #bardcraftCommunityCommand + 1) == bardcraftCommunityCommand .. " "
    then
        local action = msg == bardcraftCommunityCommand and "status"
            or lower(msg:sub(#bardcraftCommunityCommand + 2))
        handleBardcraftCommunityCommand(player, action)
        return false
    end

    if msg:sub(1, #KICK_PREFIX) == KICK_PREFIX then
        if not isAdmin(player) then
            player:sendMessage("Permission denied.")
            return false
        end

        local targetName = msg:sub(#KICK_PREFIX + 1)
        local target = findPlayerByName(targetName)
        if target then
            mp.broadcast(targetName .. " was kicked by " .. player.name .. ".")
            target:kick("Kicked by " .. player.name)
            mp.log("[core] Kick: " .. targetName .. " by " .. player.name)
        else
            player:sendMessage("Player not found: " .. targetName)
        end
        return false
    end

    if msg == COMMAND_PREFIX .. "tpto" or msg:sub(1, #TPTO_PREFIX) == TPTO_PREFIX then
        if not requireAdmin(player) then
            return false
        end

        local rest = msg == COMMAND_PREFIX .. "tpto" and "" or msg:sub(#TPTO_PREFIX + 1)
        local args, parseError = parseCommandArgs(rest)
        if not args then
            player:sendMessage(parseError or "Invalid arguments.")
            player:sendMessage("Usage: /tpto <pid|name>")
            return false
        end

        local target = resolvePlayerCommandTarget(player, args, "/tpto <pid|name>")
        if not target then
            return false
        end

        local ok, result = teleportPlayerToPlayer(player, target)
        if not ok then
            player:sendMessage(result)
            return false
        end

        player:sendMessage(string.format("Teleporting to [%d] %s in %s.", target.guid, target.name, result))
        mp.log(string.format("[core] /tpto by %s target=%s guid=%s cell=%s",
            player.name,
            target.name,
            tostring(target.guid),
            tostring(result)
        ))
        return false
    end

    if msg == COMMAND_PREFIX .. "tp" or msg:sub(1, #TP_PREFIX) == TP_PREFIX then
        if not requireAdmin(player) then
            return false
        end

        local rest = msg == COMMAND_PREFIX .. "tp" and "" or msg:sub(#TP_PREFIX + 1)
        local args, parseError = parseCommandArgs(rest)
        if not args then
            player:sendMessage(parseError or "Invalid arguments.")
            player:sendMessage("Usage: /tp <pid|name>")
            return false
        end

        local target = resolvePlayerCommandTarget(player, args, "/tp <pid|name>")
        if not target then
            return false
        end

        local ok, result = teleportPlayerToPlayer(target, player)
        if not ok then
            player:sendMessage(result)
            return false
        end

        player:sendMessage(string.format("Teleporting [%d] %s to you.", target.guid, target.name))
        if target.guid ~= player.guid then
            target:sendMessage("Teleporting to " .. player.name .. ".")
        end
        mp.log(string.format("[core] /tp by %s target=%s guid=%s cell=%s",
            player.name,
            target.name,
            tostring(target.guid),
            tostring(result)
        ))
        return false
    end

    if msg == COMMAND_PREFIX .. "delete" or msg:sub(1, #DELETE_PREFIX) == DELETE_PREFIX then
        if not requireAdmin(player) then
            return false
        end

        local args, parseError = parseCommandArgs(msg:sub(#DELETE_PREFIX + 1))
        if not args then
            player:sendMessage(parseError or "Invalid arguments.")
            player:sendMessage("Usage: /delete <mpNum> [cell]")
            return false
        end

        local mpNum = toPositiveInteger(args[1])
        if not mpNum then
            player:sendMessage("Usage: /delete <mpNum> [cell]")
            return false
        end

        local cellId = args[2] and normalizeCellId(args[2]) or ""
        if not mp.removeGameObject(mpNum, cellId or "") then
            player:sendMessage("Failed to queue delete for mpNum=" .. tostring(mpNum) .. ".")
            return false
        end

        player:sendMessage(string.format("Queued delete for mpNum=%d.", mpNum))
        mp.log(string.format("[core] /delete by %s mpNum=%d cell=%s", player.name, mpNum, tostring(cellId)))
        return false
    end

    if msg == COMMAND_PREFIX .. "resetcell" or msg:sub(1, #RESETCELL_PREFIX) == RESETCELL_PREFIX then
        if not requireAdmin(player) then
            return false
        end

        local rest = msg == COMMAND_PREFIX .. "resetcell" and "" or msg:sub(#RESETCELL_PREFIX + 1)
        local cellId = normalizeCellId(rest ~= "" and rest or player.cell)
        if not cellId or cellId == "" then
            player:sendMessage("Usage: /resetcell [cell]")
            return false
        end

        local removedSpawners, removedPayloads = destructibleSpawners.removeInCell(cellId)
        if not mp.resetCell(cellId) then
            player:sendMessage("Failed to queue reset for cell " .. tostring(cellId) .. ".")
            return false
        end

        player:sendMessage(string.format(
            "Queued reset for %s. Removed spawners=%d payloads=%d. Relog or reload the cell before testing it.",
            cellId,
            removedSpawners or 0,
            removedPayloads or 0
        ))
        mp.log(string.format("[core] /resetcell by %s cell=%s spawners=%d payloads=%d",
            player.name,
            cellId,
            removedSpawners or 0,
            removedPayloads or 0
        ))
        return false
    end

    if msg == COMMAND_PREFIX .. "mpwhere" or msg:sub(1, #MPWHERE_PREFIX) == MPWHERE_PREFIX then
        if not requireAdmin(player) then
            return false
        end

        local args, parseError = parseCommandArgs(msg:sub(#MPWHERE_PREFIX + 1))
        if not args then
            player:sendMessage(parseError or "Invalid arguments.")
            player:sendMessage("Usage: /mpwhere <mpNum>")
            return false
        end

        local mpNum = toPositiveInteger(args[1])
        if not mpNum then
            player:sendMessage("Usage: /mpwhere <mpNum>")
            return false
        end

        local kind, target = resolveMpTarget(mpNum)
        if not target then
            player:sendMessage("No server-tracked actor or object found for mpNum=" .. tostring(mpNum) .. ".")
            return false
        end

        sendMpTargetSummary(player, kind, target)
        return false
    end

    if msg == COMMAND_PREFIX .. "tomp" or msg:sub(1, #TOMP_PREFIX) == TOMP_PREFIX then
        if not requireAdmin(player) then
            return false
        end

        local args, parseError = parseCommandArgs(msg:sub(#TOMP_PREFIX + 1))
        if not args then
            player:sendMessage(parseError or "Invalid arguments.")
            player:sendMessage("Usage: /tomp <mpNum>")
            return false
        end

        local mpNum = toPositiveInteger(args[1])
        if not mpNum then
            player:sendMessage("Usage: /tomp <mpNum>")
            return false
        end

        local kind, target = resolveMpTarget(mpNum)
        if not target then
            player:sendMessage("No server-tracked actor or object found for mpNum=" .. tostring(mpNum) .. ".")
            return false
        end

        local cellId = normalizeCellId(target.cell or target.cellId)
        if not cellId or cellId == "" then
            player:sendMessage("Target has no valid cell.")
            return false
        end

        local position = plainPosition(target.position)
        position.isTeleporting = true
        if not mp.teleportPlayer(player.guid, cellId, position) then
            player:sendMessage("Failed to queue teleport to mpNum=" .. tostring(mpNum) .. ".")
            return false
        end

        player:sendMessage(string.format("Teleporting to %s mpNum=%d in %s.", kind, mpNum, cellId))
        mp.log(string.format("[core] /tomp by %s mpNum=%d kind=%s cell=%s", player.name, mpNum, kind, cellId))
        return false
    end

    if msg == COMMAND_PREFIX .. "bringmp" or msg:sub(1, #BRINGMP_PREFIX) == BRINGMP_PREFIX then
        if not requireAdmin(player) then
            return false
        end

        local args, parseError = parseCommandArgs(msg:sub(#BRINGMP_PREFIX + 1))
        if not args then
            player:sendMessage(parseError or "Invalid arguments.")
            sendBringMpUsage(player)
            return false
        end

        local mpNum = toPositiveInteger(args[1])
        if not mpNum then
            sendBringMpUsage(player)
            return false
        end

        local actor = mp.getActorByMpNum and mp.getActorByMpNum(mpNum) or nil
        if not actor then
            local object = mp.getObjectByMpNum(mpNum)
            if object then
                player:sendMessage("/bringmp currently supports server-spawned actors; use /tomp for objects.")
            else
                player:sendMessage("No server-tracked actor found for mpNum=" .. tostring(mpNum) .. ".")
            end
            return false
        end

        if actor.isDead then
            player:sendMessage("/bringmp only moves live server-spawned actors.")
            return false
        end

        if #args > 3 then
            sendBringMpUsage(player)
            return false
        end

        local distance = 128
        if args[2] and args[2] ~= "" then
            local parsedDistance = toNonNegativeNumber(args[2])
            if not parsedDistance then
                player:sendMessage("distance must be a non-negative number.")
                return false
            end
            distance = parsedDistance
        end

        local direction = 0
        if args[3] and args[3] ~= "" then
            local parsedDirection = toPlaceDirection(args[3])
            if parsedDirection == nil then
                player:sendMessage("direction must be 0, 1, 2, or 3.")
                return false
            end
            direction = parsedDirection
        end

        local cellId = normalizeCellId(player.cell)
        if not cellId or cellId == "" then
            player:sendMessage("Unable to determine your current cell.")
            return false
        end

        local sourceCellId = normalizeCellId(actor.cell or actor.cellId) or ""
        local position = placeAtPosition(player, distance, direction)
        if not mp.removeGameObject(mpNum, sourceCellId) then
            player:sendMessage("Failed to queue removal for mpNum=" .. tostring(mpNum) .. ".")
            return false
        end
        if not mp.spawnActor(actor.refId, actor.refNum or 0, mpNum, cellId, position,
            { persistent = actor.persistent ~= false }) then
            player:sendMessage("Failed to queue respawn for mpNum=" .. tostring(mpNum) .. ".")
            return false
        end

        player:sendMessage(string.format(
            "Queued bring for actor mpNum=%d refId=%s to %s.",
            mpNum, tostring(actor.refId), cellId
        ))
        mp.log(string.format(
            "[core] /bringmp by %s mpNum=%d refId=%s from=%s to=%s distance=%.1f direction=%d",
            player.name, mpNum, tostring(actor.refId), tostring(sourceCellId), cellId, distance, direction
        ))
        return false
    end

    if msg == COMMAND_PREFIX .. "placeat" or msg:sub(1, #PLACEAT_PREFIX) == PLACEAT_PREFIX then
        if not requireAdmin(player) then
            return false
        end

        local args, parseError = parseCommandArgs(msg:sub(#PLACEAT_PREFIX + 1))
        if not args then
            player:sendMessage(parseError or "Invalid arguments.")
            sendPlaceAtUsage(player)
            return false
        end

        local refId = args[1]
        if not refId or refId == "" then
            sendPlaceAtUsage(player)
            return false
        end

        if #args > 4 then
            sendPlaceAtUsage(player)
            return false
        end

        local countText = args[2]
        local distanceText = args[3]
        local directionText = args[4]

        local count = 1
        if countText and countText ~= "" then
            local parsedCount = toPositiveInteger(countText)
            if not parsedCount then
                player:sendMessage("count must be a positive integer.")
                return false
            end
            count = parsedCount
        end

        local distance = 128
        if distanceText and distanceText ~= "" then
            local parsedDistance = toNonNegativeNumber(distanceText)
            if not parsedDistance then
                player:sendMessage("distance must be a non-negative number.")
                return false
            end
            distance = parsedDistance
        end

        local direction = 0
        if directionText and directionText ~= "" then
            local parsedDirection = toPlaceDirection(directionText)
            if parsedDirection == nil then
                player:sendMessage("direction must be 0, 1, 2, or 3.")
                return false
            end
            direction = parsedDirection
        end

        local cellId = normalizeCellId(player.cell)
        if not cellId or cellId == "" then
            player:sendMessage("Unable to determine your current cell.")
            return false
        end

        local position = placeAtPosition(player, distance, direction)
        if not mp.placeObject(refId, count, cellId, position) then
            player:sendMessage("Failed to queue /placeat for " .. refId .. ".")
            return false
        end

        player:sendMessage(string.format(
            "Placed %s x%d in %s at distance %.1f direction %d.",
            refId, count, cellId, distance, direction
        ))
        mp.log(string.format(
            "[core] /placeat by %s refId=%s count=%d cell=%s distance=%.1f direction=%d",
            player.name, refId, count, cellId, distance, direction
        ))
        return false
    end

    if msg == COMMAND_PREFIX .. "spawnat" or msg:sub(1, #SPAWNAT_PREFIX) == SPAWNAT_PREFIX then
        if not requireAdmin(player) then
            return false
        end

        local args, parseError = parseCommandArgs(msg:sub(#SPAWNAT_PREFIX + 1))
        if not args then
            player:sendMessage(parseError or "Invalid arguments.")
            sendSpawnAtUsage(player)
            return false
        end

        local refId = args[1]
        if not refId or refId == "" then
            sendSpawnAtUsage(player)
            return false
        end

        if #args > 6 then
            sendSpawnAtUsage(player)
            return false
        end

        local distanceText = args[2]
        local directionText = args[3]
        local refNumText = args[4]
        local mpNumText = args[5]
        local persistenceText = lower(args[6])

        local distance = 128
        if distanceText and distanceText ~= "" then
            local parsedDistance = toNonNegativeNumber(distanceText)
            if not parsedDistance then
                player:sendMessage("distance must be a non-negative number.")
                return false
            end
            distance = parsedDistance
        end

        local direction = 0
        if directionText and directionText ~= "" then
            local parsedDirection = toPlaceDirection(directionText)
            if parsedDirection == nil then
                player:sendMessage("direction must be 0, 1, 2, or 3.")
                return false
            end
            direction = parsedDirection
        end

        local refNum = 0
        if refNumText and refNumText ~= "" then
            local parsedRefNum = tonumber(refNumText)
            if not parsedRefNum or parsedRefNum < 0 then
                player:sendMessage("refNum must be a non-negative integer.")
                return false
            end
            refNum = math.floor(parsedRefNum)
        end

        local mpNum = 0
        if mpNumText and mpNumText ~= "" then
            local parsedMpNum = tonumber(mpNumText)
            if not parsedMpNum or parsedMpNum < 0 then
                player:sendMessage("mpNum must be a non-negative integer.")
                return false
            end
            mpNum = math.floor(parsedMpNum)
        end

        local persistent = Config.SPAWNED_ACTOR_DEFAULT_PERSISTENT ~= false
        if persistenceText and persistenceText ~= "" then
            if persistenceText == "persistent" or persistenceText == "persist" then
                persistent = true
            elseif persistenceText == "session" or persistenceText == "temporary" or persistenceText == "temp" then
                persistent = false
            else
                player:sendMessage("persistence must be persistent or session.")
                return false
            end
        end

        local cellId = normalizeCellId(player.cell)
        if not cellId or cellId == "" then
            player:sendMessage("Unable to determine your current cell.")
            return false
        end

        local position = placeAtPosition(player, distance, direction)
        if not mp.spawnActor(refId, refNum, mpNum, cellId, position, { persistent = persistent }) then
            player:sendMessage("Failed to queue /spawnat for " .. refId .. ".")
            return false
        end

        local mpNumLabel = (mpNum == 0) and "auto" or tostring(mpNum)
        player:sendMessage(string.format(
            "Spawned actor %s in %s at distance %.1f direction %d (refNum=%d mpNum=%s %s).",
            refId, cellId, distance, direction, refNum, mpNumLabel, persistent and "persistent" or "session"
        ))
        mp.log(string.format(
            "[core] /spawnat by %s refId=%s cell=%s distance=%.1f direction=%d refNum=%d mpNum=%s persistent=%s",
            player.name, refId, cellId, distance, direction, refNum, mpNumLabel, tostring(persistent)
        ))
        return false
    end

    if msg:sub(1, #SETTIME_PREFIX) == SETTIME_PREFIX then
        if not requireAdmin(player) then
            return false
        end

        local hour = tonumber(msg:sub(#SETTIME_PREFIX + 1))
        if hour and hour >= 0 and hour < 24 then
            mp.setWorldTime(hour)
            mp.broadcast("Server time set to " .. string.format("%02d:00", math.floor(hour))
                .. " by " .. player.name .. ".")
            mp.log("[core] settime " .. hour .. " by " .. player.name)
        else
            player:sendMessage("Usage: /settime <0-23>")
        end
        return false
    end

    if msg == COMMAND_PREFIX .. "luabroadcast" or msg:sub(1, #LUABROADCAST_PREFIX) == LUABROADCAST_PREFIX then
        if not requireAdmin(player) then
            return false
        end

        local note = trim(msg:match("^" .. "%" .. COMMAND_PREFIX .. "luabroadcast%s+(.+)$"))
        local payload = makeLuaTestPayload("server-broadcast", player, {
            note = note ~= "" and note or "server-broadcast",
        })
        mp.broadcast(string.format(
            "[LuaTest] broadcast seq=%d by %s note=%s",
            payload.seq, player.name, payload.note
        ))
        mp.broadcast("MpLuaTest_ServerBroadcast", payload)
        player:sendMessage("Sent Lua broadcast test event.")
        mp.log(string.format("[core] Lua test broadcast seq=%d by %s", payload.seq, player.name))
        return false
    end

    if msg == COMMAND_PREFIX .. "luacell" or msg:sub(1, #LUACELL_PREFIX) == LUACELL_PREFIX then
        if not requireAdmin(player) then
            return false
        end

        local cellId = normalizeCellId(msg:match("^" .. "%" .. COMMAND_PREFIX .. "luacell%s+(.+)$") or player.cell)
        if not cellId or cellId == "" then
            player:sendMessage("No cell available for /luacell.")
            return false
        end

        local payload = makeLuaTestPayload("server-cell", player, {
            note = "server-cell",
            targetCell = cellId,
        })
        mp.broadcastToCell(cellId, string.format(
            "[LuaTest] cell seq=%d by %s target=%s",
            payload.seq, player.name, cellId
        ))
        mp.broadcastToCell(cellId, "MpLuaTest_ServerCell", payload)
        player:sendMessage("Sent Lua cell test event to: " .. cellId)
        mp.log(string.format("[core] Lua test cell seq=%d by %s cell=%s", payload.seq, player.name, cellId))
        return false
    end

    if msg:sub(1, #LUASEND_PREFIX) == LUASEND_PREFIX then
        if not requireAdmin(player) then
            return false
        end

        local targetName, note = msg:match("^" .. "%" .. COMMAND_PREFIX .. "luasend%s+(%S+)%s*(.*)$")
        local target = findPlayerByName(targetName or "")
        if not target then
            player:sendMessage("Player not found: " .. tostring(targetName))
            return false
        end

        note = trim(note)
        local payload = makeLuaTestPayload("server-direct", player, {
            note = note ~= "" and note or "server-direct",
            targetName = target.name,
            targetGuid = target.guid,
        })
        mp.send(target.guid, "MpLuaTest_ServerDirect", payload)
        target:sendMessage(string.format(
            "[LuaTest] direct seq=%d from %s note=%s",
            payload.seq, player.name, payload.note
        ))
        player:sendMessage("Sent Lua direct test event to: " .. target.name)
        mp.log(string.format("[core] Lua test direct seq=%d by %s target=%s", payload.seq, player.name, target.name))
        return false
    end

    if msg == COMMAND_PREFIX .. "luaping" or msg:sub(1, #LUAPING_PREFIX) == LUAPING_PREFIX then
        if not requireAdmin(player) then
            return false
        end

        local targetName = msg:match("^" .. "%" .. COMMAND_PREFIX .. "luaping%s+(.+)$")
        local target = player
        if targetName and targetName ~= "" then
            target = findPlayerByName(targetName)
            if not target then
                player:sendMessage("Player not found: " .. targetName)
                return false
            end
        end

        local payload = {
            kind = "server-requested",
            note = "server-requested-ping",
            requestedBy = player.name,
        }
        mp.send(target.guid, "MpLuaTest_SendPing", payload)
        target:sendMessage("[LuaTest] ping requested by " .. player.name)
        player:sendMessage("Requested Lua ping from: " .. target.name)
        mp.log(string.format("[core] Lua test ping requested by %s from %s", player.name, target.name))
        return false
    end

    if msg:sub(1, #LUABURST_PREFIX) == LUABURST_PREFIX then
        if not requireAdmin(player) then
            return false
        end

        local targetName, countText = msg:match("^" .. "%" .. COMMAND_PREFIX .. "luaburst%s+(%S+)%s*(%d*)$")
        local target = findPlayerByName(targetName or "")
        if not target then
            player:sendMessage("Player not found: " .. tostring(targetName))
            return false
        end

        local count = tonumber(countText) or 5
        if count < 1 then
            count = 1
        elseif count > 25 then
            count = 25
        end

        mp.send(target.guid, "MpLuaTest_SendBurst", {
            count = count,
            requestedBy = player.name,
        })
        target:sendMessage(string.format(
            "[LuaTest] burst requested by %s count=%d",
            player.name, count
        ))
        player:sendMessage(string.format("Requested Lua burst (%d) from: %s", count, target.name))
        mp.log(string.format("[core] Lua test burst requested by %s from %s count=%d", player.name, target.name, count))
        return false
    end

    if msg == COMMAND_PREFIX .. "luastorage" or msg:sub(1, #LUASTORAGE_PREFIX) == LUASTORAGE_PREFIX then
        if not requireAdmin(player) then
            return false
        end

        local section = mp.storage.globalSection("MpLuaTest")
        local requested = tonumber(trim(msg:match("^" .. "%" .. COMMAND_PREFIX .. "luastorage%s+(.+)$") or ""))
        local value = requested or ((section:getCopy("total") or 0) + 1)

        section:set("total", value)
        section:set("lastBy", player.name)
        section:set("lastSeq", nextLuaTestSeq())

        player:sendMessage(string.format("[LuaTest] storage total set to %s.", tostring(value)))
        mp.log(string.format("[core] Lua storage test by %s total=%s", player.name, tostring(value)))
        return false
    end

    if msg == LUATYPES_PREFIX then
        if not requireAdmin(player) then
            return false
        end

        local stats = types.Player.stats(player)
        local health = stats.health
        local inventory = types.Actor.inventory(player)

        player:sendMessage(string.format(
            "[LuaTest] types health %.1f/%.1f modifier %.1f inventory=%d",
            health.current or 0,
            health.base or 0,
            health.modifier or 0,
            #inventory
        ))
        mp.log(string.format(
            "[core] Lua types test by %s health=%.1f/%.1f modifier=%.1f inventory=%d",
            player.name,
            health.current or 0,
            health.base or 0,
            health.modifier or 0,
            #inventory
        ))
        return false
    end

    local markRecallHandled = markRecallCommands.handleChat(player, data, {
        commandPrefix = COMMAND_PREFIX,
        normalizeCellId = normalizeCellId,
    })
    if markRecallHandled ~= nil then
        return markRecallHandled
    end

    if msg:sub(1, #COMMAND_PREFIX + 4) == COMMAND_PREFIX .. "nick" then
        local arg = msg:sub(#COMMAND_PREFIX + 6)
        if msg == COMMAND_PREFIX .. "nick off" then
            player:setNickname("")
            player:sendMessage("Nickname cleared.")
            mp.log("[core] " .. player.name .. " cleared nickname")
        elseif #arg == 0 then
            local current = player:getNickname()
            if current == "" then
                player:sendMessage("No nickname set.  Usage: /nick <name> | /nick off")
            else
                player:sendMessage("Your nickname is: " .. current)
            end
        elseif #arg > 24 then
            player:sendMessage("Nickname too long (max 24 characters).")
        else
            player:setNickname(arg)
            player:sendMessage("Nickname set to: " .. arg)
            mp.log("[core] " .. player.name .. " set nickname to '" .. arg .. "'")
        end
        return false
    end

    if msg:sub(1, #COMMAND_PREFIX) == COMMAND_PREFIX then
        local speechHandled = speechCommands.handleChat(player, data, {
            commandPrefix = COMMAND_PREFIX,
            parseCommandArgs = parseCommandArgs,
        })
        if speechHandled ~= nil then
            return speechHandled
        end

        local surfHandled = surfCommands.handleChat(player, data, {
            commandPrefix = COMMAND_PREFIX,
            findPlayerByName = findPlayerByName,
            isAdmin = isAdmin,
            requireAdmin = requireAdmin,
            normalizeCellId = normalizeCellId,
        })
        if surfHandled ~= nil then
            return surfHandled
        end

        local recordDynamicHandled = recordDynamicTest.handleChat(player, data, {
            commandPrefix = COMMAND_PREFIX,
            parseCommandArgs = parseCommandArgs,
            requireAdmin = requireAdmin,
        })
        if recordDynamicHandled ~= nil then
            return recordDynamicHandled
        end

        local recordStoreHandled = recordStore.handleChat(player, data, {
            commandPrefix = COMMAND_PREFIX,
            parseCommandArgs = parseCommandArgs,
            requireAdmin = requireAdmin,
        })
        if recordStoreHandled ~= nil then
            return recordStoreHandled
        end

        local spawnerHandled = destructibleSpawners.handleChat(player, data, {
            commandPrefix = COMMAND_PREFIX,
            parseCommandArgs = parseCommandArgs,
            requireAdmin = requireAdmin,
            normalizeCellId = normalizeCellId,
            placeAtPosition = placeAtPosition,
        })
        if spawnerHandled ~= nil then
            return spawnerHandled
        end

        local adminUiHandled = adminUiService.handleChat(player, data, {
            commandPrefix = COMMAND_PREFIX,
            requireAdmin = requireAdmin,
            isAdmin = isAdmin,
        })
        if adminUiHandled ~= nil then
            return adminUiHandled
        end

        player:sendMessage("Unknown command.  Type /help for a list.")
        return false
    end

    mp.relayChat(data.guid, msg)
    return false
end

return {
    interfaceName = "IntentPolicy",
    interface = {
        evaluateIntent = function(intentName, data)
            return intentPolicy.evaluate(intentName, data)
        end,
        handleAdminUiHttp = function(data)
            return adminUiService.handleHttpRequest(data, {
                commandPrefix = COMMAND_PREFIX,
                isAdmin = function()
                    return true
                end,
            })
        end,
        handleChatCommand = function(data)
            local guid = data and tonumber(data.guid) or nil
            local player = guid and getPlayer(guid) or nil
            local message = data and tostring(data.message or "") or ""
            if not player then
                return { ok = false, error = "player_not_found" }
            end
            if message == "" then
                return { ok = false, error = "message_required" }
            end
            if message:sub(1, #COMMAND_PREFIX) ~= COMMAND_PREFIX then
                return { ok = false, error = "command_prefix_required" }
            end

            handleChat(player, {
                guid = guid,
                name = player.name,
                message = message,
            })
            return { ok = true }
        end,
    },
    eventHandlers = {
        OnServerInit = function(_)
            admins = {}
            markRecallCommands.onServerInit()
            recordStore.onServerInit()
            recordDynamicTest.onServerInit()
            destructibleSpawners.onServerInit()
            mp.log("[core] Server ready — " .. mp.getPlayerCount() .. " player(s) connected.")
        end,

        OnPlayerConnect = function(data)
            local player = getPlayer(data.guid)
            if not player then
                return
            end

            if ANNOUNCE_JOIN_LEAVE then
                mp.broadcast(">> " .. player.name .. " has joined.  ("
                    .. mp.getPlayerCount() .. "/" .. MAX_PLAYERS .. ")")
            end
            player:sendMessage(MOTD)
            mp.log("[core] Connect: " .. player.name .. " (guid " .. player.guid .. ")")
        end,

        OnPlayerDisconnect = function(data)
            admins[data.guid] = nil
            markRecallCommands.onPlayerDisconnect(data)
            surfCommands.onPlayerDisconnect(data)
            surfTimer.onPlayerDisconnect(data)
            if ANNOUNCE_JOIN_LEAVE then
                mp.broadcast("<< " .. data.name .. " has left.  (" .. (data.reason or "Disconnected") .. ")")
            end
            mp.log("[core] Disconnect: " .. data.name .. " — " .. (data.reason or "Disconnected"))
        end,

        OnPlayerCellChange = function(data)
            mp.log("[core] " .. data.name .. ": " .. tostring(data.oldCell) .. " → " .. tostring(data.newCell))
        end,

        OnPlayerSendMessage = function(data)
            local player = getPlayer(data.guid)
            if not player then
                return false
            end
            return handleChat(player, data)
        end,

        TestPing = function(data)
            local ts = data.ts or 0
            if data.pid then
                mp.log(string.format("[core] TestPing from pid=%d ts=%s", data.pid, tostring(ts)))
                mp.send(data.pid, "TestPong", { ts = ts })
            end
        end,

        SurfTimerTrigger = function(data)
            surfTimer.handleTrigger(data)
        end,

        Activate = function(data)
            handleActivate(data)
        end,

        AdminUi_Request = function(data)
            adminUiService.handleRequest(data, {
                commandPrefix = COMMAND_PREFIX,
                isAdmin = isAdmin,
                normalizeCellId = normalizeCellId,
                placeAtPosition = placeAtPosition,
            })
        end,

        OnDoorState = function(data)
            doorStateCache[doorKey(data.cellId, data.refId, "")] = data.isOpen
            mp.log(string.format("[core] Door %s in '%s' is now %s",
                data.refId, data.cellId, data.isOpen and "open" or "closed"))
        end,

        OnWorldWeather = function(data)
            if data.next and data.next >= 0 then
                mp.log(string.format("[core] Weather '%s': %d → %d (%.2f)",
                    data.region, data.current, data.next, data.transitionFactor))
            else
                mp.log(string.format("[core] Weather '%s': %d", data.region, data.current))
            end
        end,

        OnActorSpawned = function(data)
            destructibleSpawners.onActorSpawned(data)
        end,

        OnActorDeath = function(data)
            destructibleSpawners.onActorDeath(data)
        end,

        OnServerTick = function(data)
            destructibleSpawners.onServerTick(data)
            tickAccum = tickAccum + (data.dt or 0)
            if tickAccum >= 300 then
                tickAccum = 0
                mp.log("[core] Heartbeat — players: " .. mp.getPlayerCount()
                    .. "  time: " .. string.format("%.1f", mp.getWorldTime()) .. "h")
            end
        end,
    }
}
