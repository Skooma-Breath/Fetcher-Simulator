local mp = require("mp")
local Config = require("config")
local types = require("openmw.types")
local intentPolicy = require("intent_policy")

------------------------------------------------------------------------
-- Config
------------------------------------------------------------------------
local ADMIN_PASSWORD = Config.ADMIN_PASSWORD or "changeme"
local MAX_PLAYERS = 32
local MOTD = Config.MOTD or "Welcome to the server!  Type !help for commands."
local COMMAND_PREFIX = Config.COMMAND_PREFIX or "!"
local ANNOUNCE_JOIN_LEAVE = Config.ANNOUNCE_JOIN_LEAVE ~= false
local LOG_CHAT = Config.LOG_CHAT == true
local ALLOW_UNVERIFIED_ACTIVATE = Config.ALLOW_UNVERIFIED_ACTIVATE == true
local LOGIN_PREFIX = COMMAND_PREFIX .. "login "
local KICK_PREFIX = COMMAND_PREFIX .. "kick "
local SETTIME_PREFIX = COMMAND_PREFIX .. "settime "
local LUABROADCAST_PREFIX = COMMAND_PREFIX .. "luabroadcast "
local LUACELL_PREFIX = COMMAND_PREFIX .. "luacell "
local LUASEND_PREFIX = COMMAND_PREFIX .. "luasend "
local LUAPING_PREFIX = COMMAND_PREFIX .. "luaping "
local LUABURST_PREFIX = COMMAND_PREFIX .. "luaburst "
local LUASTORAGE_PREFIX = COMMAND_PREFIX .. "luastorage "
local LUATYPES_PREFIX = COMMAND_PREFIX .. "luatypes"

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

local function normalizeCellId(cellId)
    cellId = trim(cellId)
    if not cellId or cellId == "" then
        return cellId
    end

    local x, y = cellId:match("^[Ee][Xx][Tt]:%s*(-?%d+)%s*,%s*(-?%d+)$")
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
    if accepted and not serverVerified then
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

intentPolicy.registerHandler("Activate", "demo_common_pants_gold_reward", function(context)
    local object = context.object or {}
    local result = context.result
    if not result.accepted then
        return nil
    end
    if tostring(object.recordId or ""):lower() ~= "common_pants_01" then
        return nil
    end
    if not object.mpNum or not object.cell or object.cell == "" then
        return {
            accepted = false,
            reason = "server_override_unverified_target",
            mutation = "server-override-rejected",
            applyStandardAction = false,
        }
    end

    return {
        applyStandardAction = false,
        mutation = "server-inventory-grant",
        ops = {
            intentPolicy.ops.removePlacedObject(object.mpNum, object.cell),
            intentPolicy.ops.grantInventory(context.player.guid, "gold_001", 50000),
        },
        stop = true,
    }
end)

local function handleChat(player, data)
    local msg = data.message or ""

    if LOG_CHAT then
        mp.log("[core] Chat [" .. player.name .. "] " .. msg)
    end

    if msg == COMMAND_PREFIX .. "help" then
        player:sendMessage(
            "Commands:  !who  !time  !uptime  !nick <n>  !nick off"
                .. (isAdmin(player)
                    and "  !kick <name>  !settime <hour>  !luabroadcast [note]"
                        .. "  !luasend <name> [note]  !luacell [cell]"
                        .. "  !luaping [name]  !luaburst <name> [count]"
                        .. "  !luastorage [value]  !luatypes"
                    or "")
                .. "  !login <password>"
        )
        return false
    end

    if msg == COMMAND_PREFIX .. "who" then
        player:sendMessage("Online (" .. mp.getPlayerCount() .. "): " .. playerList())
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
            player:sendMessage("Usage: !settime <0-23>")
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
            player:sendMessage("No cell available for !luacell.")
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

    if msg:sub(1, #COMMAND_PREFIX + 4) == COMMAND_PREFIX .. "nick" then
        local arg = msg:sub(#COMMAND_PREFIX + 6)
        if msg == COMMAND_PREFIX .. "nick off" then
            player:setNickname("")
            player:sendMessage("Nickname cleared.")
            mp.log("[core] " .. player.name .. " cleared nickname")
        elseif #arg == 0 then
            local current = player:getNickname()
            if current == "" then
                player:sendMessage("No nickname set.  Usage: !nick <name> | !nick off")
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
        player:sendMessage("Unknown command.  Type !help for a list.")
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
    },
    eventHandlers = {
        OnServerInit = function(_)
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

        Activate = function(data)
            handleActivate(data)
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

        OnServerTick = function(data)
            tickAccum = tickAccum + (data.dt or 0)
            if tickAccum >= 300 then
                tickAccum = 0
                mp.log("[core] Heartbeat — players: " .. mp.getPlayerCount()
                    .. "  time: " .. string.format("%.1f", mp.getWorldTime()) .. "h")
            end
        end,
    }
}
