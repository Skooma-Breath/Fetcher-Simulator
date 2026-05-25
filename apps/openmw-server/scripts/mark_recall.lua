local mp = require("mp")

local M = {}

local DEFAULT_SURF_CELL_SETTINGS = {
    surf_mesa_mw = { enabled = true },
    surf_utopia_mw = { enabled = true },
    surf_kitsune_mw = {
        enabled = true,
        gravity = 1.6,
        airAccel = 600.0,
        maxAirSpeed = 2000.0,
        friction = 5.0,
        groundAccel = 10.0,
        overbounce = 4.0,
        rampAngle = 0.8,
        impactOverbounce = 1.1,
        impactVelocityThreshold = 200.0,
    },
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

local function normalizeMarkName(name)
    local normalized = trim(name)
    if not normalized or normalized == "" then
        return nil
    end
    return string.lower(normalized)
end

local function copyPosition(position)
    return {
        x = position.x or 0,
        y = position.y or 0,
        z = position.z or 0,
        rx = position.rx or 0,
        ry = position.ry or 0,
        rz = position.rz or 0,
    }
end

local function describeMark(mark)
    return string.format(
        "%s @ %s (%.1f, %.1f, %.1f)",
        mark.name,
        mark.cell,
        mark.position.x,
        mark.position.y,
        mark.position.z
    )
end

local function loadMarks(player)
    return mp.getPlayerMarks(player.guid)
end

local function findMark(player, normalizedName)
    for _, mark in ipairs(loadMarks(player)) do
        if lower(mark.name) == normalizedName then
            return mark
        end
    end
    return nil
end

local function resolveCommand(msg, commandPrefix)
    local commands = { "mark", "recall", "marks", "unmark" }
    for _, name in ipairs(commands) do
        local command = (commandPrefix or "/") .. name
        if msg == command or msg:sub(1, #command + 1) == command .. " " then
            return name, trim(msg:sub(#command + 1))
        end
    end
    return nil
end

local function sendUsage(player)
    player:sendMessage("/mark <name> saves your current location.")
    player:sendMessage("/recall <name> teleports you to a saved mark.")
    player:sendMessage("/marks lists your saved marks.")
    player:sendMessage("/unmark <name> deletes a saved mark.")
end

local function handleMark(player, markName, env)
    local key = normalizeMarkName(markName)
    if not key then
        sendUsage(player)
        return false
    end

    local cellId = env.normalizeCellId(player.cell)
    if not cellId or cellId == "" then
        player:sendMessage("Unable to determine your current cell.")
        return false
    end

    local mark = {
        name = key,
        cell = cellId,
        position = copyPosition(player.position),
    }

    if not mp.savePlayerMark(player.guid, mark.name, mark.cell, mark.position) then
        player:sendMessage("Failed to save mark '" .. mark.name .. "'.")
        return false
    end

    player:sendMessage("Saved mark: " .. describeMark(mark))
    return false
end

local function handleRecall(player, markName)
    local key = normalizeMarkName(markName)
    if not key then
        sendUsage(player)
        return false
    end

    local mark = findMark(player, key)
    if not mark then
        player:sendMessage("No saved mark named '" .. tostring(markName) .. "'.")
        return false
    end

    if not mp.teleportPlayer(player.guid, mark.cell, mark.position) then
        player:sendMessage("Recall failed for '" .. mark.name .. "'.")
        return false
    end

    player:sendMessage("Recalled to " .. describeMark(mark))
    return false
end

local function handleMarks(player)
    local entries = loadMarks(player)

    if #entries == 0 then
        player:sendMessage("No saved marks.")
        return false
    end

    table.sort(entries, function(left, right)
        return lower(left.name) < lower(right.name)
    end)

    player:sendMessage("Saved marks (" .. tostring(#entries) .. "):")
    for _, mark in ipairs(entries) do
        player:sendMessage(" - " .. describeMark(mark))
    end
    return false
end

local function handleUnmark(player, markName)
    local key = normalizeMarkName(markName)
    if not key then
        sendUsage(player)
        return false
    end

    local mark = findMark(player, key)
    if not mark then
        player:sendMessage("No saved mark named '" .. tostring(markName) .. "'.")
        return false
    end

    if not mp.deletePlayerMark(player.guid, mark.name) then
        player:sendMessage("Failed to delete mark '" .. mark.name .. "'.")
        return false
    end

    player:sendMessage("Deleted mark: " .. mark.name)
    return false
end

function M.handleChat(player, data, env)
    local msg = trim(data.message or "")
    if not msg or msg == "" then
        return nil
    end

    local command, rest = resolveCommand(msg, env.commandPrefix)
    if not command then
        return nil
    end

    if command == "mark" then
        return handleMark(player, rest, env)
    end
    if command == "recall" then
        return handleRecall(player, rest)
    end
    if command == "marks" then
        return handleMarks(player)
    end
    if command == "unmark" then
        return handleUnmark(player, rest)
    end

    return nil
end

function M.onServerInit()
    for cellId, settings in pairs(DEFAULT_SURF_CELL_SETTINGS) do
        mp.setCellPhysics(cellId, settings)
    end
end

function M.onPlayerDisconnect(data)
end

return M
