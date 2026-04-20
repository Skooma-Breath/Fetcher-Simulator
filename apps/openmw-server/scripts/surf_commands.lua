local mp = require("mp")

local M = {}

local DEFAULTS = {
    enabled = false,
    gravity = 1.3,
    airAccel = 300.0,
    maxAirSpeed = 2000.0,
    friction = 5.0,
    groundAccel = 10.0,
    overbounce = 3.5,
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
    overbounce = "overbounce",
    rampangle = "rampAngle",
    impactoverbounce = "impactOverbounce",
    impactvelocitythreshold = "impactVelocityThreshold",
}

local DISPLAY_FIELDS = {
    { key = "enabled", label = "enabled", kind = "bool" },
    { key = "gravity", label = "gravity" },
    { key = "airAccel", label = "airAccel" },
    { key = "maxAirSpeed", label = "maxAirSpeed" },
    { key = "friction", label = "friction" },
    { key = "groundAccel", label = "groundAccel" },
    { key = "overbounce", label = "overbounce" },
    { key = "rampAngle", label = "rampAngle" },
    { key = "impactOverbounce", label = "impactOverbounce" },
    { key = "impactVelocityThreshold", label = "impactVelocityThreshold" },
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
        overbounce = DEFAULTS.overbounce,
        rampAngle = DEFAULTS.rampAngle,
        impactOverbounce = DEFAULTS.impactOverbounce,
        impactVelocityThreshold = DEFAULTS.impactVelocityThreshold,
    }
end

local function syncDefaultsToServer()
    assert(type(mp.setGlobalPhysics) == "function", "surf_commands.lua requires mp.setGlobalPhysics")
    mp.setGlobalPhysics(copyDefaults())
end

local function formatBool(value)
    return value and "on" or "off"
end

local function getDisplayValue(settings, key)
    local value = settings[key]
    if value == nil then
        return DEFAULTS[key]
    end
    return value
end

local function dumpSettings(player, label, settings)
    settings = settings or copyDefaults()
    player:sendMessage(label)
    for _, field in ipairs(DISPLAY_FIELDS) do
        local value = getDisplayValue(settings, field.key)
        if field.kind == "bool" then
            player:sendMessage(string.format("  %s: %s", field.label, formatBool(value)))
        else
            player:sendMessage(string.format("  %s: %.3f", field.label, value))
        end
    end
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
    player:sendMessage(prefix .. "surf -> show your effective surf values")
    player:sendMessage(prefix .. "surf [on|off|clear|<setting> <value>]")
    player:sendMessage(prefix .. "surf cell [cellId] [on|off|clear|<setting> <value>]")
    player:sendMessage(prefix .. "surf player <name> [on|off|clear|<setting> <value>]")
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
        mp.clearCellPhysics(cellId)
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

local function handlePlayerAction(player, target, args, actionIndex, env)
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

local function handlePlayerCommand(player, args, env)
    local targetName = trim(args[2])
    if not targetName or targetName == "" then
        player:sendMessage("Usage: " .. (env.commandPrefix or "/") .. "surf player <name> [on|off|clear|<setting> <value>]")
        return false
    end

    local target = env.findPlayerByName(targetName)
    if not target then
        player:sendMessage("Player not found: " .. tostring(targetName))
        return false
    end

    return handlePlayerAction(player, target, args, 3, env)
end

function M.handleChat(player, data, env)
    local commandPrefix = env.commandPrefix or "/"
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
        return handlePlayerCommand(player, args, env)
    end
    if scope == "self" then
        return handlePlayerAction(player, player, args, 2, env)
    end

    return handlePlayerAction(player, player, args, 1, env)
end

function M.onPlayerDisconnect(data)
    if data and data.guid then
        mp.clearPlayerPhysics(data.guid)
    end
end

syncDefaultsToServer()

return M
