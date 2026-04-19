local mp = require("mp")

local M = {}

local DEFAULTS = {
    enabled = false,
    gravity = 1.0,
    airAccel = 70.0,
    maxAirSpeed = 2000.0,
    friction = 5.0,
    groundAccel = 10.0,
    jumpSpeed = 268.0,
    overbounce = 1.1,
    rampAngle = 0.8,
    impactOverbounce = 1.1,
    impactVelocityThreshold = 200.0,
}

local SETTING_FIELDS = {
    enabled = "enabled",
    surf = "enabled",
    surfphysics = "enabled",
    surfphysicsenabled = "enabled",
    gravity = "gravity",
    gravitymult = "gravity",
    airaccel = "airAccel",
    maxairspeed = "maxAirSpeed",
    friction = "friction",
    groundaccel = "groundAccel",
    jumpspeed = "jumpSpeed",
    overbounce = "overbounce",
    rampangle = "rampAngle",
    impactoverbounce = "impactOverbounce",
    impactvelocitythreshold = "impactVelocityThreshold",
}

local function trim(text)
    if not text then
        return nil
    end
    return (text:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function lower(text)
    return string.lower(trim(text) or "")
end

local function parseBoolean(value)
    value = lower(value)
    if value == "on" or value == "true" or value == "yes" or value == "1" then
        return true
    end
    if value == "off" or value == "false" or value == "no" or value == "0" then
        return false
    end
    return nil
end

local function normalizeSettingKey(rawKey)
    return SETTING_FIELDS[lower(rawKey or "")]
end

local function copyDefaults()
    return {
        enabled = DEFAULTS.enabled,
        gravity = DEFAULTS.gravity,
        airAccel = DEFAULTS.airAccel,
        maxAirSpeed = DEFAULTS.maxAirSpeed,
        friction = DEFAULTS.friction,
        groundAccel = DEFAULTS.groundAccel,
        jumpSpeed = DEFAULTS.jumpSpeed,
        overbounce = DEFAULTS.overbounce,
        rampAngle = DEFAULTS.rampAngle,
        impactOverbounce = DEFAULTS.impactOverbounce,
        impactVelocityThreshold = DEFAULTS.impactVelocityThreshold,
    }
end

local function formatBool(value)
    return value and "on" or "off"
end

local function dumpSettings(player, label, settings)
    settings = settings or copyDefaults()
    player:sendMessage(string.format(
        "%s enabled=%s gravity=%.3f airAccel=%.3f maxAirSpeed=%.3f",
        label,
        formatBool(settings.enabled),
        settings.gravity or DEFAULTS.gravity,
        settings.airAccel or DEFAULTS.airAccel,
        settings.maxAirSpeed or DEFAULTS.maxAirSpeed
    ))
    player:sendMessage(string.format(
        "friction=%.3f groundAccel=%.3f jumpSpeed=%.3f overbounce=%.3f",
        settings.friction or DEFAULTS.friction,
        settings.groundAccel or DEFAULTS.groundAccel,
        settings.jumpSpeed or DEFAULTS.jumpSpeed,
        settings.overbounce or DEFAULTS.overbounce
    ))
    player:sendMessage(string.format(
        "rampAngle=%.3f impactOverbounce=%.3f impactVelocityThreshold=%.3f",
        settings.rampAngle or DEFAULTS.rampAngle,
        settings.impactOverbounce or DEFAULTS.impactOverbounce,
        settings.impactVelocityThreshold or DEFAULTS.impactVelocityThreshold
    ))
end

local function parseValue(field, rawValue)
    if field == "enabled" then
        local enabled = parseBoolean(rawValue)
        if enabled == nil then
            return nil, "Expected on/off for surf enabled."
        end
        return enabled
    end

    local number = tonumber(rawValue)
    if not number then
        return nil, "Expected a numeric value."
    end

    return number
end

local function currentCellId(player, env)
    return env.normalizeCellId(player.cell)
end

local function canMutatePlayer(requestingPlayer, targetPlayer, env)
    return targetPlayer.guid == requestingPlayer.guid or env.isAdmin(requestingPlayer)
end

local function sendHelp(player, prefix, isAdmin)
    player:sendMessage("!surf -> show your effective surf values")
    player:sendMessage("!surf self [on|off|clear|<setting> <value>]")
    player:sendMessage("!surf cell [cellId] [on|off|clear|<setting> <value>]")
    player:sendMessage("!surf player <name> [on|off|clear|<setting> <value>]")
    if not isAdmin then
        player:sendMessage("Only admins can modify cells or other players.")
    end
end

local function handleCellCommand(player, args, env)
    local cellId = currentCellId(player, env)
    local actionIndex = 2
    local candidate = trim(args[2])
    if candidate and candidate ~= "" then
        local normalizedCandidate = env.normalizeCellId(candidate)
        local maybeField = normalizeSettingKey(candidate)
        local maybeToggle = parseBoolean(candidate)
        if maybeField == nil and maybeToggle == nil and lower(candidate) ~= "clear" then
            cellId = normalizedCandidate
            actionIndex = 3
        end
    end

    if not cellId or cellId == "" then
        player:sendMessage("No current cell available.")
        return false
    end

    local action = trim(args[actionIndex])
    if not action or action == "" then
        dumpSettings(player, "Cell " .. cellId .. ":", mp.getCellPhysics(cellId))
        return false
    end

    if not env.requireAdmin(player) then
        return false
    end

    local toggle = parseBoolean(action)
    if toggle ~= nil then
        mp.setCellPhysics(cellId, { enabled = toggle })
        dumpSettings(player, "Cell " .. cellId .. ":", mp.getCellPhysics(cellId))
        return false
    end

    if lower(action) == "clear" then
        mp.setCellPhysics(cellId, copyDefaults())
        dumpSettings(player, "Cell " .. cellId .. ":", mp.getCellPhysics(cellId))
        return false
    end

    local field = normalizeSettingKey(action)
    if not field then
        player:sendMessage("Unknown surf setting: " .. tostring(action))
        return false
    end

    local value, err = parseValue(field, args[actionIndex + 1])
    if value == nil then
        player:sendMessage(err)
        return false
    end

    mp.setCellPhysics(cellId, { [field] = value })
    dumpSettings(player, "Cell " .. cellId .. ":", mp.getCellPhysics(cellId))
    return false
end

local function handlePlayerCommand(player, args, env, defaultToSelf)
    local target = player
    local actionIndex = 2

    if not defaultToSelf then
        local targetName = trim(args[2])
        if not targetName or targetName == "" then
            player:sendMessage("Usage: !surf player <name> [on|off|clear|<setting> <value>]")
            return false
        end
        target = env.findPlayerByName(targetName)
        if not target then
            player:sendMessage("Player not found: " .. tostring(targetName))
            return false
        end
        actionIndex = 3
    end

    local action = trim(args[actionIndex])
    if not action or action == "" then
        dumpSettings(player, "Player " .. target.name .. ":", mp.getPlayerPhysics(target.guid))
        return false
    end

    if not canMutatePlayer(player, target, env) then
        if target.guid == player.guid then
            player:sendMessage("Permission denied.")
        else
            env.requireAdmin(player)
        end
        return false
    end

    local toggle = parseBoolean(action)
    if toggle ~= nil then
        mp.setPlayerPhysics(target.guid, { enabled = toggle })
        dumpSettings(player, "Player " .. target.name .. ":", mp.getPlayerPhysics(target.guid))
        return false
    end

    if lower(action) == "clear" then
        mp.clearPlayerPhysics(target.guid)
        dumpSettings(player, "Player " .. target.name .. ":", mp.getPlayerPhysics(target.guid))
        return false
    end

    local field = normalizeSettingKey(action)
    if not field then
        player:sendMessage("Unknown surf setting: " .. tostring(action))
        return false
    end

    local value, err = parseValue(field, args[actionIndex + 1])
    if value == nil then
        player:sendMessage(err)
        return false
    end

    mp.setPlayerPhysics(target.guid, { [field] = value })
    dumpSettings(player, "Player " .. target.name .. ":", mp.getPlayerPhysics(target.guid))
    return false
end

function M.handleChat(player, data, env)
    local commandPrefix = env.commandPrefix or "!"
    local base = commandPrefix .. "surf"
    local msg = trim(data.message or "")
    if msg ~= base and msg:sub(1, #base + 1) ~= base .. " " then
        return nil
    end

    local rest = trim(msg:sub(#base + 1))
    if not rest or rest == "" then
        dumpSettings(player, "Player " .. player.name .. ":", mp.getPlayerPhysics(player.guid))
        return false
    end

    local args = {}
    for token in rest:gmatch("%S+") do
        table.insert(args, token)
    end

    local scope = lower(args[1])
    if scope == "help" then
        sendHelp(player, commandPrefix, env.isAdmin(player))
        return false
    end
    if scope == "cell" then
        return handleCellCommand(player, args, env)
    end
    if scope == "player" then
        return handlePlayerCommand(player, args, env, false)
    end
    if scope == "self" then
        return handlePlayerCommand(player, args, env, true)
    end

    player:sendMessage("Usage: !surf | !surf help | !surf self ... | !surf player <name> ... | !surf cell ...")
    return false
end

function M.onPlayerDisconnect(data)
    if data and data.guid then
        mp.clearPlayerPhysics(data.guid)
    end
end

return M
